#!/usr/bin/env python3
"""Synthetic 1030 MHz interrogators at known bearings on kraken_doa's UCA, at
2.4 MHz - test transmissions for the ssr1030 plugin (decoding, ID separation).

  ssr_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces.
OUT.json gets rate, channels, rf_hz, radius_m and the truth (each
interrogator's bearing, kind, codes, the Mode S frames it sent).

Each interrogation is built as the envelope a transmitter sends - pulses with
their exact edges, the Mode S P6 as 0.25 us DPSK chips (a phase reversal
before a chip = 1) - and received through a receiver step response S(t)
(sum of edge jumps x S(t - edge)):
  "response": "measured"   the KrakenSDR's (plugins/ssr1030/s_default.h - the
                           plugin starts from the same table)
              "lowpass"    a different one: a windowed-sinc low-pass (cut-off
                           "cutoff_hz", default 1.0 MHz) centred "shift_hz"
                           (default 40 kHz) off - the plugin has to learn it
plus a carrier offset per interrogator, a random phase per interrogation and
an optional echo per interrogator ("echo": {"delay_us": 2.5, "rel_db": -15}).

Mode S frames have the real formats: UF11 all-call (PR 0, IC = II / SI with
CL), UF4 / UF5 surveillance (DI 7 + IIS), UF0 short air-air, UF16 ACAS
broadcast (UDS 0x32, the aircraft's own address in bits 65-88); parity =
CRC-24 (0xFFF409) of the first 32 / 88 bits XOR the address ENCODED for the
uplink (the coefficients x^47..x^24 of A(x) G(x)); all-calls and broadcasts
use the address FFFFFF.

SCENE (JSON):
{
  "rf_hz": 1030e6, "seconds": 12, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 0.6, "response": "measured",
  "interrogators": [
    {"name": "SSR-A", "type": "ssr", "bearing": 60, "peak_db": 36,
     "pri_us": 5000, "jitter_pct": 1.0, "modes": ["A", "C"], "p4": "short",
     "uf11_every": 3, "ii": 10,           (or "si": 23)
     "scan_s": 4.0, "beam_deg": 2.5, "sidelobe_db": -28, "az0_deg": 100,
     "sls_db": -12,                        P2 (omni control antenna) against the main beam's peak
     "targets": ["C82827", "7C1474"],      roll-called (UF4 / UF5) once per pass, in the main beam
     "df_hz": -3000},
    {"name": "MLAT", "type": "fixed", "bearing": 200, "level_db": 22,
     "period_s": 1.0, "allcalls": ["A", "A", "C"], "allcall_ms": 5, "p4": "short",
     "targets": ["C82929", "C8195C"], "rollcall_ms": 1, "uf": [4, 5], "df_hz": 2000},
    {"name": "TCAS", "type": "acas", "bearing": 300, "level_db": 24, "address": "C820CF",
     "steps": 8, "step_db": 1.0, "step_ms": 17.5, "s1_db": -2, "period_s": 1.0,
     "uf0_targets": ["C82081"], "broadcast_s": 4.0, "df_hz": 500}
  ]
}
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/ssr1030/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf 1030e6
"""
import json
import os
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0
POLY = 0xFFF409
T_CHIP = 0.25e-6


# ---------------------------------------------------------------- Mode S
def crc24(bits):
    r = 0
    for b in bits:
        top = ((r >> 23) & 1) ^ b
        r = (r << 1) & 0xFFFFFF
        if top:
            r ^= POLY
    return r


def encode_address(a):
    g = (1 << 24) | POLY
    prod = 0
    for i in range(25):
        if (g >> i) & 1:
            prod ^= a << i
    return (prod >> 24) & 0xFFFFFF


def bits_of(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def frame(fields, addr):
    """fields: list of (value, nbits) for the first 32 / 88 bits; addr 24-bit"""
    b = []
    for v, n in fields:
        b += bits_of(v, n)
    ap = crc24(b) ^ encode_address(addr)
    return b + bits_of(ap, 24)


def uf11(ii=None, si=None):
    if si:
        ic, cl = si % 16, si // 16 + 1
    else:
        ic, cl = ii or 0, 0
    return frame([(11, 5), (0, 4), (ic, 4), (cl, 3), (0, 16)], 0xFFFFFF)


def uf45(uf, addr, ii=0, rr=0):
    # PC 0, RR, DI 7, SD = IIS (4) + 12 zero bits
    return frame([(uf, 5), (0, 3), (rr, 5), (7, 3), (ii << 12, 16)], addr)


def uf0(addr):
    return frame([(0, 5), (0, 3), (0, 1), (0, 4), (0, 1), (0, 8), (0, 10)], addr)


def uf16_broadcast(own):
    # UF16, RL 0, AQ 0, MU: UDS 0x32 + 24 zero bits + the aircraft's own address (bits 65-88)
    mu = (0x32 << 48) | own
    return frame([(16, 5), (0, 3), (0, 1), (0, 4), (0, 1), (0, 18), (mu, 56)], 0xFFFFFF)


# ---------------------------------------------------------------- waveforms: edges (t_s, complex jump)
def pulse_edges(t0, w, a):
    return [(t0, a), (t0 + w, -a)]


def modes_edges(t1, mode, p4, p2_amp, s1_amp, a):
    sp = {'A': 8e-6, 'B': 17e-6, 'C': 21e-6, 'D': 25e-6}[mode]
    e = pulse_edges(t1, 0.8e-6, a) + pulse_edges(t1 + sp, 0.8e-6, a)
    if p2_amp:
        e += pulse_edges(t1 + 2e-6, 0.8e-6, p2_amp)
    if p4:
        e += pulse_edges(t1 + sp + 2e-6, 1.6e-6 if p4 == 'long' else 0.8e-6, a)
    if s1_amp:
        e += pulse_edges(t1 - 2e-6, 0.8e-6, s1_amp)
    return e, sp + (2e-6 + 1.6e-6 if p4 else 0.8e-6)


def modes_s_edges(t1, bits, a, p2a=None):
    e = pulse_edges(t1, 0.8e-6, a) + pulse_edges(t1 + 2e-6, 0.8e-6, a if p2a is None else p2a)
    t6 = t1 + 3.5e-6
    chips = [1] * 5 + [-1] * 2
    for b in bits:
        chips.append(-chips[-1] if b else chips[-1])
    chips += [chips[-1]] * 2
    e.append((t6, a * chips[0]))
    for k in range(1, len(chips)):
        if chips[k] != chips[k - 1]:
            e.append((t6 + k * T_CHIP, a * (chips[k] - chips[k - 1])))
    e.append((t6 + len(chips) * T_CHIP, -a * chips[-1]))
    return e, 3.5e-6 + len(chips) * T_CHIP


# ---------------------------------------------------------------- the receiver's step response
def response(scene):
    kind = scene.get('response', 'measured')
    if kind == 'measured':
        here = os.path.dirname(os.path.abspath(__file__))
        vals = []
        for line in open(os.path.join(here, '..', 's_default.h')):
            if line.strip().startswith('//'):
                continue
            for part in line.split('},'):
                part = part.strip().strip('{},')
                if part:
                    re_, im_ = part.replace('f', '').split(',')
                    vals.append(complex(float(re_), float(im_)))
        S = np.array(vals)
        grid = -1e-6 + np.arange(len(S)) / 32e6
        return grid, S
    # a windowed-sinc low-pass, its centre shifted by shift_hz (default 40 kHz: nearly symmetric)
    fc = float(scene.get('cutoff_hz', 1.0e6))
    sh = float(scene.get('shift_hz', 40e3))
    fs = 96e6
    n = np.arange(-int(2e-6 * fs), int(2e-6 * fs) + 1)
    t = n / fs
    h = 2 * fc / fs * np.sinc(2 * fc * t) * np.kaiser(len(n), 6.0) * np.exp(2j * np.pi * sh * t)
    h /= h.sum()
    S = np.cumsum(h)
    grid = t
    return grid, S / S[-1]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    fmt_ = out.rsplit('.', 1)[-1]
    if fmt_ not in ('cu8', 'cf32'):
        sys.exit('output: .cu8 or .cf32')
    if os.path.abspath(out.rsplit('.', 1)[0] + '.json') == os.path.abspath(sys.argv[1]):
        sys.exit('OUT.json would overwrite the scene: give the output another name')
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene.get('rf_hz', 1030e6))
    lam = C / rf
    T = float(scene['seconds'])
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 0.6))
    noise_p = 2 * sig_n ** 2
    phi = -2 * np.pi * np.arange(M) / M
    gS, S = response(scene)

    def amp(db):
        return np.sqrt(noise_p * 10 ** (db / 10))

    items = []      # (t_start, t_end, edges, steering, df, echo)
    truth = []
    for it in scene['interrogators']:
        b = np.deg2rad(float(it['bearing']))
        steer = np.exp(2j * np.pi * R / lam * np.cos(b - phi))
        df = float(it.get('df_hz', 0))
        echo = it.get('echo')
        tr = {'name': it['name'], 'type': it['type'], 'bearing': it['bearing'], 'frames': 0, 'allcalls': 0}
        typ = it['type']
        if typ == 'ssr':
            pri = float(it.get('pri_us', 5000)) * 1e-6
            jit = float(it.get('jitter_pct', 0)) / 100
            modes = it.get('modes', ['A', 'C'])
            p4 = it.get('p4', 'short')
            every = int(it.get('uf11_every', 0))
            scan = float(it.get('scan_s', 4.0))
            beam = float(it.get('beam_deg', 2.5))
            side = float(it.get('sidelobe_db', -28))
            az0 = float(it.get('az0_deg', 0))
            peak = float(it['peak_db'])
            sls = float(it.get('sls_db', -12))
            targets = [int(a, 16) for a in it.get('targets', [])]
            ii = it.get('ii')
            si = it.get('si')
            t = float(it.get('t0_s', rng.uniform(0, pri)))
            k = 0
            done_pass = {}
            while t < T:
                az = ((t / scan * 360 + az0 + 180) % 360) - 180
                g = max(-3 * (2 * az / beam) ** 2, side + 3 * np.sin(np.deg2rad(az) * 7.3) - (6 if abs(az) > 90 else 0))
                a = amp(peak + g) * np.exp(2j * np.pi * rng.uniform())
                p2a = amp(peak + sls) * np.exp(2j * np.pi * rng.uniform())
                if every and k % every == 0:
                    bits = uf11(ii=ii, si=si)
                    e, L = modes_s_edges(t - 140e-6, bits, a, p2a=a)
                    items.append((t - 140e-6, t - 140e-6 + L, e, steer, df, echo))
                    tr['frames'] += 1
                e, L = modes_edges(t, modes[k % len(modes)], p4, p2a, 0, a)
                items.append((t, t + L, e, steer, df, echo))
                tr['allcalls'] += 1
                # roll-calls: each target once per pass, in the main beam (|az| < beam / 2), mid-period
                turn = int(np.floor((t / scan * 360 + az0 + 180) / 360))
                if abs(az) < beam / 2 and targets:
                    for ti, addr in enumerate(targets):
                        if done_pass.get((turn, ti)):
                            continue
                        if rng.uniform() < 0.3:
                            done_pass[(turn, ti)] = True
                            uf = 4 if ti % 2 == 0 else 5
                            bits = uf45(uf, addr, ii=ii if ii is not None else 0)
                            ts = t + 0.4 * pri + 0.3e-3 * (ti % 4)
                            e, L = modes_s_edges(ts, bits, a, p2a=a)
                            items.append((ts, ts + L, e, steer, df, echo))
                            tr['frames'] += 1
                            break
                t += pri * (1 + jit * rng.uniform(-1, 1))
                k += 1
            tr.update({'ii': ii, 'si': si, 'pri_us': pri * 1e6, 'scan_s': scan, 'beam_deg': beam,
                       'main_beam_s': [float((360 * kk - az0) / 360 * scan) for kk in range(-1, int(T / scan) + 3)
                                       if 0 <= (360 * kk - az0) / 360 * scan < T],
                       'targets': it.get('targets', [])})
        elif typ == 'fixed':
            P = float(it.get('period_s', 1.0))
            lvl = float(it['level_db'])
            acs = it.get('allcalls', ['A', 'A', 'C'])
            acms = float(it.get('allcall_ms', 5)) * 1e-3
            p4 = it.get('p4', 'short')
            targets = [int(a, 16) for a in it.get('targets', [])]
            rcms = float(it.get('rollcall_ms', 1)) * 1e-3
            ufs = it.get('uf', [4, 5])
            t = float(it.get('t0_s', rng.uniform(0, P)))
            n = 0
            while t < T:
                for j, m in enumerate(acs):
                    a = amp(lvl) * np.exp(2j * np.pi * rng.uniform())
                    e, L = modes_edges(t + j * acms, m, p4, 0, 0, a)
                    items.append((t + j * acms, t + j * acms + L, e, steer, df, echo))
                    tr['allcalls'] += 1
                t2 = t + len(acs) * acms + 1.25e-3
                for j, addr in enumerate(targets):
                    a = amp(lvl) * np.exp(2j * np.pi * rng.uniform())
                    bits = uf45(ufs[(n + j) % len(ufs)], addr, ii=0)
                    e, L = modes_s_edges(t2 + j * rcms, bits, a)
                    items.append((t2 + j * rcms, t2 + j * rcms + L, e, steer, df, echo))
                    tr['frames'] += 1
                t += P
                n += 1
            tr.update({'period_s': P, 'targets': it.get('targets', [])})
        elif typ == 'acas':
            P = float(it.get('period_s', 1.0))
            lvl = float(it['level_db'])
            steps = int(it.get('steps', 8))
            sdb = float(it.get('step_db', 1.0))
            sms = float(it.get('step_ms', 17.5)) * 1e-3
            s1 = float(it.get('s1_db', -2))
            own = int(it.get('address', '000000'), 16)
            tg0 = [int(a, 16) for a in it.get('uf0_targets', [])]
            bc = float(it.get('broadcast_s', 0))
            t = float(it.get('t0_s', rng.uniform(0, P)))
            next_bc = t + 0.3
            while t < T:
                for s in range(steps):
                    l_ = lvl - (steps - 1 - s) * sdb
                    a = amp(l_) * np.exp(2j * np.pi * rng.uniform())
                    s1a = amp(l_ + s1) * np.exp(2j * np.pi * rng.uniform()) if s else 0
                    e, L = modes_edges(t + s * sms, 'C', 'short', 0, s1a, a)
                    items.append((t + s * sms - 2e-6, t + s * sms + L, e, steer, df, echo))
                    tr['allcalls'] += 1
                t3 = t + steps * sms + 5e-3
                for j, addr in enumerate(tg0):
                    a = amp(lvl) * np.exp(2j * np.pi * rng.uniform())
                    e, L = modes_s_edges(t3 + j * 2e-3, uf0(addr), a)
                    items.append((t3 + j * 2e-3, t3 + j * 2e-3 + L, e, steer, df, echo))
                    tr['frames'] += 1
                if bc and t >= next_bc:
                    a = amp(lvl) * np.exp(2j * np.pi * rng.uniform())
                    tb = t3 + 20e-3
                    e, L = modes_s_edges(tb, uf16_broadcast(own), a)
                    items.append((tb, tb + L, e, steer, df, echo))
                    tr['frames'] += 1
                    next_bc += bc
                t += P * (1 + 0.02 * rng.uniform(-1, 1))
            tr.update({'address': it.get('address'), 'steps': steps, 'period_s': P})
        truth.append(tr)
        print('%-8s %-5s bearing %6.1f  all-calls %d  Mode S %d' % (tr['name'], typ, it['bearing'], tr['allcalls'], tr['frames']))
    items.sort(key=lambda x: x[0])
    starts = np.array([x[0] for x in items])
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            lo = np.searchsorted(starts, (c0 - 100) / FS - 50e-6)
            hi = np.searchsorted(starts, c1 / FS + 5e-6)
            for ts, te, edges, steer, df, echo in items[lo:hi]:
                k0 = max(c0, int(np.floor((ts - 2.5e-6) * FS)))
                k1 = min(c1, int(np.ceil((te + 6e-6) * FS)) + 1)
                if k1 <= k0:
                    continue
                tt = np.arange(k0, k1) / FS
                y = np.zeros(len(tt), complex)
                for u, e in edges:
                    x_ = tt - u
                    y += e * (np.interp(x_, gS, S.real, left=0, right=S.real[-1]) + 1j * np.interp(x_, gS, S.imag, left=0, right=S.imag[-1]))
                    if echo:
                        de = float(echo.get('delay_us', 2.0)) * 1e-6
                        ea = 10 ** (float(echo.get('rel_db', -15)) / 20)
                        x2 = x_ - de
                        y += ea * e * (np.interp(x2, gS, S.real, left=0, right=S.real[-1]) + 1j * np.interp(x2, gS, S.imag, left=0, right=S.imag[-1]))
                y *= np.exp(2j * np.pi * df * (tt - ts))
                x[k0 - c0:k1 - c0, :] += y[:, None] * steer[None, :]
            if fmt_ == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                yv = x / 127.5
                yv = np.clip(yv.real, -1, 1) + 1j * np.clip(yv.imag, -1, 1)
                yv.astype(np.complex64).tofile(fo)
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s)' % (out, M, N / FS, fmt_))


if __name__ == '__main__':
    main()
