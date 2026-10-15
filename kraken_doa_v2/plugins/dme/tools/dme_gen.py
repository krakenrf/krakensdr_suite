#!/usr/bin/env python3
"""Synthetic DME / TACAN transmissions at known bearings on kraken_doa's UCA,
at 2.4 MHz - test signals for the dme plugin (beacons, idents, TACAN,
interrogations, DoA per beacon).

  dme_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces;
OUT.json gets rate, channels, rf_hz, radius_m and the truth.

Signals (ICAO Annex 10 Vol I 3.5 / TACAN as publicly described): Gaussian
pulses of 3.5 us half-amplitude width in pairs - beacon replies 12 us apart
in X mode, 30 us in Y mode; aircraft interrogations 12 / 36 us (X / Y) on
1025-1150 MHz. A beacon sends random pairs (squitter + replies, a 60 us
dead time after each) at "pairs_s"; during its Morse ident (dots of
"dot_s", every loop at "ident_at") exactly 1350 pairs per second instead.
A TACAN beacon also sends reference bursts - north 15 per second, 8
auxiliary between them (X: 12 / 6 pairs 24 us apart; Y: 13 single pulses
30 / 15 us apart) - and amplitude-modulates every other pulse by its
rotating pattern: 1 + m15 cos(psi - psi0) + m135 cos(9 (psi - psi0)), psi
the rotation since the north burst, psi0 = bearing - 90 deg (the north burst
goes out as the pattern's maximum points east, it turns clockwise). Each
transmitter has its own carrier offset and a continuous carrier phase.

SCENE (JSON):
{
  "rf_hz": 1150.5e6, "seconds": 8, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.5,
  "sources": [
    {"name": "63Y", "kind": "beacon", "mhz": 1150, "mode": "Y", "pairs_s": 2700,
     "ident": "TKN", "ident_at": 4.4, "dot_s": 0.1,
     "tacan": {"bearing": 237, "m15": 0.2, "m135": 0.1},
     "bearing": 60, "level_db": 25, "df_hz": -3000},
    {"name": "126X aircraft", "kind": "interrogator", "mhz": 1150, "mode": "X",
     "pairs_s": 60, "bearing": 120, "level_db": 15}
  ]
}
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py). Make
"seconds" a whole number of 1/15 s (TACAN turns) so the loop stays periodic.

Test:  plugins/dme/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 --rf 1150.5e6
"""
import json
import math
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0
MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.', 'H': '....', 'I': '..',
         'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.',
         'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..',
         '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....', '6': '-....',
         '7': '--...', '8': '---..', '9': '----.'}
SIGMA_US = 3.5 / (2 * math.sqrt(2 * math.log(2)))   # amplitude half width 3.5 us


def key_spans(text, t0, dot):
    """the key-down spans [(a, b)] of a Morse ident"""
    spans, t = [], t0
    for li, ch in enumerate(text.upper()):
        if li:
            t += 2 * dot                      # letter gap 3 (1 already after the mark)
        for sym in MORSE[ch]:
            d = dot if sym == '.' else 3 * dot
            spans.append((t, t + d))
            t += d + dot
    return spans


def beacon_pulses(s, T, rng):
    """[(t_us, amp factor)] of every pulse a beacon sends in the loop"""
    mode = s.get('mode', 'X')
    sp = 12.0 if mode == 'X' else 30.0
    if s.get('kind') == 'interrogator':
        sp = 12.0 if mode == 'X' else 36.0
    pulses = []
    busy = []                                   # [(a_us, b_us)] reserved
    tac = s.get('tacan')
    turn = 1e6 / 15
    if tac:
        nb = int(round(T * 15))
        for k in range(nb):
            for j in range(9):                  # j 0 = north, 1..8 auxiliary
                t0 = k * turn + j * turn / 9
                if mode == 'X':
                    n, step = (12, 24.0) if j == 0 else (6, 24.0)
                    for i in range(n):
                        pulses += [(t0 + i * step, None), (t0 + i * step + 12, None)]
                    span = n * step + 12
                else:
                    n, step = 13, 30.0 if j == 0 else 15.0
                    pulses += [(t0 + i * step, None) for i in range(n)]
                    span = (n - 1) * step
                busy.append((t0 - 60, t0 + span + 60))
    keys = []
    if s.get('ident'):
        dot = float(s.get('dot_s', 0.1))
        keys = [(a * 1e6, b * 1e6) for a, b in key_spans(s['ident'], float(s.get('ident_at', 0.5)), dot)]
        for a, b in keys:
            t = a
            while t < b:
                if not any(x <= t <= y for x, y in busy):
                    pulses += [(t, 1.0), (t + sp, 1.0)]
                t += 1e6 / 1350
    # random pairs (squitter + replies) outside the key-down spans / bursts
    rate = float(s.get('pairs_s', 1000))
    t = rng.exponential(1e6 / rate)
    busy_all = sorted(busy + [(a - 60, b + 60) for a, b in keys])
    bi = 0
    while t < T * 1e6 - 100:
        while bi < len(busy_all) and busy_all[bi][1] < t:
            bi += 1
        if bi < len(busy_all) and busy_all[bi][0] <= t + sp + 10:
            t = busy_all[bi][1] + rng.exponential(1e6 / rate)
            continue
        pulses += [(t, 1.0), (t + sp, 1.0)]
        t += sp + 60 + rng.exponential(1e6 / rate)
    out = []
    for t, a in pulses:
        if a is None:
            g = 1.0                              # reference bursts: not modulated
        elif tac:
            psi = 2 * math.pi * (t % turn) / turn
            p0 = math.radians(float(tac['bearing']) - 90)
            g = 1 + float(tac.get('m15', 0.2)) * math.cos(psi - p0) + float(tac.get('m135', 0.1)) * math.cos(9 * (psi - p0))
        else:
            g = 1.0
        out.append((t, g))
    out.sort()
    return out, keys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    if out.rsplit('.', 1)[0] == sys.argv[1].rsplit('.', 1)[0]:
        sys.exit('the output sidecar would overwrite the scene')
    fmt = out.rsplit('.', 1)[-1]
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene.get('rf_hz', 1150.5e6))
    T = float(scene.get('seconds', 8))
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.5))
    noise_p = 2 * sig_n ** 2
    phi = -2 * np.pi * np.arange(M) / M
    hw = int(math.ceil(4.5 * SIGMA_US * 1e-6 * FS))
    srcs = []
    truth = []
    for s in scene['sources']:
        f_rf = float(s['mhz']) * 1e6
        lam = C / f_rf
        b = float(s['bearing'])
        steer = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(b) - phi))
        amp = math.sqrt(noise_p * 10 ** (float(s.get('level_db', 20)) / 10))
        off = f_rf - rf + float(s.get('df_hz', 0))
        pulses, keys = beacon_pulses(s, T, rng)
        ph0 = rng.uniform(0, 2 * np.pi)
        srcs.append((np.array([p[0] for p in pulses]) * 1e-6 * FS, np.array([p[1] for p in pulses]), amp, steer, off, ph0))
        tr = {'name': s['name'], 'kind': s.get('kind', 'beacon'), 'mhz': s['mhz'], 'mode': s.get('mode', 'X'),
              'bearing': b, 'level_db': s.get('level_db', 20), 'pulses': len(pulses)}
        for k in ('ident', 'ident_at', 'tacan'):
            if k in s:
                tr[k] = s[k]
        truth.append(tr)
        print('%-16s %-12s %4d MHz %s  bearing %6.1f  %6d pulses (%5.0f / s)  level %4.1f dB  offset %+.1f kHz%s%s' % (
            s['name'], s.get('kind', 'beacon'), s['mhz'], s.get('mode', 'X'), b, len(pulses), len(pulses) / T,
            float(s.get('level_db', 20)), off / 1e3, '  ident ' + s['ident'] if s.get('ident') else '',
            '  TACAN %.1f deg' % s['tacan']['bearing'] if s.get('tacan') else ''))
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            kk = np.arange(c0, c1)
            for pos, gain, amp, steer, off, ph0 in srcs:
                lo = np.searchsorted(pos, c0 - hw - 1)
                hi = np.searchsorted(pos, c1 + hw + 1)
                if hi <= lo:
                    continue
                env = np.zeros(n)
                for p, g in zip(pos[lo:hi], gain[lo:hi]):
                    a = max(c0, int(p) - hw)
                    e = min(c1, int(p) + hw + 2)
                    if e <= a:
                        continue
                    tt = (np.arange(a, e) - p) / FS * 1e6
                    env[a - c0:e - c0] += g * np.exp(-0.5 * (tt / SIGMA_US) ** 2)
                carrier = np.exp(1j * (ph0 + 2 * np.pi * off * kk / FS))
                x += (amp * env * carrier)[:, None] * steer[None, :]
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                y = np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)
                y.astype(np.complex64).tofile(fo)
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s)' % (out, M, N / FS, fmt))


if __name__ == '__main__':
    main()
