#!/usr/bin/env python3
"""Synthetic ISM-band devices (433.92 MHz etc.) at known bearings on
kraken_doa's UCA, at 2.4 MHz - test signals for the ism plugin (decoding,
the per-device DoA with its channel filtered out of the band).

  ism_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces
(a few seconds of 5 antennas at 2.4 MHz don't fit in memory as one array).
OUT.json gets rate, channels, rf_hz, radius_m and the truth (every device's
talker id as the plugin forms it, bearing, frequency, transmission times).

Devices (the framing / checks the plugin's decoders verify - protocol notes
from rtl_433's documentation, see the plugin's README):
  ev1527         OOK, clock Te; bit = 4 Te, carrier 3 Te (1) / 1 Te (0); frame
                 = sync (1 Te on, 31 Te off) + 24 bits (20-bit address + 4
                 key bits), repeated; no checksum (two equal frames count).
                 The code must hold a "10" digit pair (else it reads as a
                 PT2262). Talker "EV1527 <address, 5 hex>"
  pt2262         the same bit timing, 12 tri-state digits (0 = 00, 1 = 11,
                 F = 01). Talker "PT2262 <first 8 digits>"
  acurite_tower  Acurite 592TXR: OOK PWM 612 us, a 1 = 408 us pulse, a 0 =
                 220 us; 4 sync pulses (620 / 620 us) before each of 3
                 copies; 7 bytes CCiiiiii iiiiiiii pB000100 ph.. pT.. pT..
                 sum (channel A/B/C, 14-bit id, even parity in every byte's
                 MSB from byte 2 on, battery, type 0x04, humidity, temperature
                 C*10 + 1000, byte sum). Talker "Acurite-Tower <id>"
  tx141th        LaCrosse TX141TH-Bv2: OOK PWM 625 us, a 1 = 417 us pulse, a 0
                 = 208 us; 4 sync pulses (833 / 833 us) before each packet,
                 sent 4..12 times; 40 bits id | batt_low test channel:2
                 temp:12 (C*10 + 500) | humidity | LFSR digest (reflected,
                 gen 0x31, key 0xF4). Talker "LaCrosse-TX141THBv2 <id>"
  wh24           Fine Offset WH24 outdoor array: FSK NRZ 58 us (higher tone =
                 1), preamble aa.., sync 2d d4, 17 bytes: 0x24, id, wind
                 direction, flags (dir / speed bit 8, low battery, temperature
                 bits 10-8), temperature (C*10 + 400), humidity, wind, gust,
                 rain (0.3 mm), UV, light (0.1 lux), CRC-8 (0x31, init 0) of
                 bytes 0..14, byte sum of 0..15. Talker "Fineoffset-WH24 <id>"
  toyota         Toyota TPMS (PMV-C210): FSK 52 us half-bits, preamble 01..,
                 sync 001111, 72 bits differential Manchester (a transition at
                 every cell start, 1 = no mid-cell transition): id 32 | status
                 bit, pressure 8 ((PSI + 7) * 4) | temperature 8 (C + 40) |
                 status 7 | inverted pressure 8 | CRC-8 (0x07, init 0x80).
                 Talker "Toyota <id, 8 hex>"

SCENE (JSON):
{
  "rf_hz": 433.92e6,             the tuner's centre (sets the wavelength)
  "seconds": 6, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.5,              receiver noise rms per I / Q (8-bit LSB)
  "sources": [
    {"kind": "ev1527", "code": "9b37d2", "te_us": 350, "frames": 5,
     "offset_hz": -870e3,        the device's carrier in the 2.4 MHz band
     "bearing": 330,             degrees, as the MUSIC DoA plot shows them (CCW from ANT0)
     "level_db": 10,             per-sample SNR at 2.4 MHz (its 240 kHz demodulation band: +10 dB)
     "at": [0.3, 3.6],           transmission start times (s)
     "clock": 1.01},             its timing off by this factor (cheap RC clocks)
    {"kind": "pt2262", "tristate": "0F1F0F1F0001", ...},
    {"kind": "acurite_tower", "id": 12053, "channel": "A", "temperature_C": 18.4,
     "humidity": 62, "battery_ok": 1, ...},
    {"kind": "tx141th", "id": 90, "channel": 0, "temperature_C": 21.3, "humidity": 48,
     "packets": 8, ...},
    {"kind": "wh24", "id": 101, "temperature_C": 15.2, "humidity": 71, "wind_dir_deg": 225,
     "wind_avg_m_s": 3.4, "wind_max_m_s": 5.6, "rain_mm": 12.3, "dev_hz": 40e3, ...},
    {"kind": "toyota", "id": "4e3a91c7", "pressure_PSI": 33.25, "temperature_C": 24,
     "status": 0, "copies": 2, "dev_hz": 20e3, ...}
  ]
}
Antennas: kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as
uca_scene.py): a plane wave from bearing b reaches antenna k with phase
2 pi r / lambda cos(b - phi_k), phi_k = -2 pi k / M (lambda of the device's
own frequency).

Test:  plugins/ism/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 --rf 433.92e6
"""
import json
import math
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0


# ------------------------------------------------------------ checks ---
def crc8(data, poly, init):
    c = init
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def lfsr_digest8_reflect(data, gen, key):
    """bytes from the last to the first, bits LSB first (the plugin's bits.cpp)"""
    s = 0
    for d in reversed(data):
        for k in range(8):
            if (d >> k) & 1:
                s ^= key
            key = ((key << 1) ^ gen) & 0xFF if key & 0x80 else (key << 1) & 0xFF
    return s


def even_parity_msb(v):
    """v (7 bits) with its MSB set so the byte has even parity"""
    v &= 0x7F
    return v | (0x80 if bin(v).count('1') & 1 else 0)


def bits_of(data, nbits=None):
    out = []
    for b in data:
        out += [(b >> (7 - i)) & 1 for i in range(8)]
    return out if nbits is None else out[:nbits]


# ---------------------------------------------------------- framings ---
# Every framing returns (modulation, segments): OOK [(on 0/1, us)], FSK
# [(tone +1 / -1, us)], plus a talker id and a summary for the truth.
def fixed_code_segments(code24, te):
    """EV1527 / PT2262 frames: sync + 24 bits, frames times, then a closing sync"""
    frame = [(1, te), (0, 31 * te)]
    for i in range(23, -1, -1):
        b = (code24 >> i) & 1
        frame += [(1, 3 * te), (0, te)] if b else [(1, te), (0, 3 * te)]
    return frame


def ev1527(s):
    code = int(s['code'], 16) & 0xFFFFFF
    pairs = [(code >> (2 * d)) & 3 for d in range(12)]
    if 2 not in pairs:
        raise SystemExit('ev1527 %s: no "10" digit pair - the plugin would read it as a PT2262' % s['code'])
    te = float(s.get('te_us', 350))
    segs = []
    for _ in range(int(s.get('frames', 5))):
        segs += fixed_code_segments(code, te)
    segs += [(1, te), (0, 31 * te)]
    return 'OOK', segs, 'EV1527 %05x' % (code >> 4), {'code': '%06x' % code, 'key': '%x' % (code & 15)}


def pt2262(s):
    ts = s['tristate'].upper()
    if len(ts) != 12 or any(c not in '01F' for c in ts):
        raise SystemExit('pt2262: 12 digits of 0 / 1 / F')
    code = 0
    for c in ts:
        code = (code << 2) | {'0': 0, '1': 3, 'F': 1}[c]
    te = float(s.get('te_us', 300))
    segs = []
    for _ in range(int(s.get('frames', 5))):
        segs += fixed_code_segments(code, te)
    segs += [(1, te), (0, 31 * te)]
    return 'OOK', segs, 'PT2262 ' + ts[:8], {'tristate': ts, 'data': ts[8:]}


def acurite_tower(s):
    ident = int(s['id']) & 0x3FFF
    ch = {'A': 3, 'B': 2, 'C': 0}[s.get('channel', 'A')]
    hum = int(s.get('humidity', 50))
    t = int(round(float(s.get('temperature_C', 20.0)) * 10)) + 1000
    batt = 1 if s.get('battery_ok', 1) else 0
    b = [(ch << 6) | (ident >> 8), ident & 0xFF, even_parity_msb((batt << 6) | 0x04), even_parity_msb(hum),
         even_parity_msb(t >> 7), even_parity_msb(t & 0x7F)]
    b.append(sum(b) & 0xFF)
    segs = []
    for _ in range(int(s.get('copies', 3))):
        segs += [(1, 620), (0, 620)] * 4
        for bit in bits_of(b):
            segs += [(1, 408), (0, 204)] if bit else [(1, 220), (0, 392)]
    return 'OOK', segs, 'Acurite-Tower %d' % ident, {'bytes': bytes(b).hex()}


def tx141th(s):
    ident = int(s['id']) & 0xFF
    t = int(round(float(s.get('temperature_C', 20.0)) * 10)) + 500
    hum = int(s.get('humidity', 50))
    b1 = ((0 if s.get('battery_ok', 1) else 1) << 7) | ((1 if s.get('test') else 0) << 6) | \
         ((int(s.get('channel', 0)) & 3) << 4) | ((t >> 8) & 15)
    b = [ident, b1, t & 0xFF, hum]
    b.append(lfsr_digest8_reflect(b, 0x31, 0xF4))
    segs = []
    for _ in range(int(s.get('packets', 8))):
        segs += [(1, 833), (0, 833)] * 4
        for bit in bits_of(b):
            segs += [(1, 417), (0, 208)] if bit else [(1, 208), (0, 417)]
    segs += [(1, 833), (0, 833)]
    return 'OOK', segs, 'LaCrosse-TX141THBv2 %d' % ident, {'bytes': bytes(b).hex()}


def nrz_segments(bits, bit_us):
    segs = []
    for b in bits:
        tone = 1 if b else -1
        if segs and segs[-1][0] == tone:
            segs[-1] = (tone, segs[-1][1] + bit_us)
        else:
            segs.append((tone, bit_us))
    return segs


def wh24(s):
    ident = int(s['id']) & 0xFF
    traw = int(round(float(s.get('temperature_C', 20.0)) * 10)) + 400
    hum = int(s.get('humidity', 50))
    d = int(s.get('wind_dir_deg', 0)) % 360
    F, R = 1.12, 0.3     # WH24 scale factors (wind m/s per count, rain mm per count)
    spd = int(round(float(s.get('wind_avg_m_s', 0)) * 8 / F)) & 0x1FF
    gust = int(round(float(s.get('wind_max_m_s', 0)) / F)) & 0xFF
    rain = int(round(float(s.get('rain_mm', 0)) / R)) & 0xFFFF
    uv = int(s.get('uv_raw', 1200)) & 0xFFFF
    light = int(round(float(s.get('light_lux', 25430)) * 10)) & 0xFFFFFF
    low = 0 if s.get('battery_ok', 1) else 1
    b = [0x24, ident, d & 0xFF, ((d >> 8) << 7) | ((spd >> 8) << 4) | (low << 3) | ((traw >> 8) & 7), traw & 0xFF,
         hum, spd & 0xFF, gust, rain >> 8, rain & 0xFF, uv >> 8, uv & 0xFF, light >> 16, (light >> 8) & 0xFF,
         light & 0xFF]
    b.append(crc8(b, 0x31, 0x00))
    b.append(sum(b) & 0xFF)
    frame = [0xAA] * int(s.get('preamble_bytes', 4)) + [0x2D, 0xD4] + b + [0x00]
    return 'FSK', nrz_segments(bits_of(frame), 58.0), 'Fineoffset-WH24 %d' % ident, {'bytes': bytes(b).hex()}


def toyota(s):
    ident = int(s['id'], 16) & 0xFFFFFFFF
    pr = int(round((float(s.get('pressure_PSI', 32.0)) + 7.0) / 0.25)) & 0xFF
    t = int(round(float(s.get('temperature_C', 20.0)))) + 40
    st = int(s.get('status', 0)) & 0xFF
    b = [ident >> 24, (ident >> 16) & 0xFF, (ident >> 8) & 0xFF, ident & 0xFF,
         (st & 0x80) | (pr >> 1), ((pr & 1) << 7) | (t >> 1), ((t & 1) << 7) | (st & 0x7F), pr ^ 0xFF]
    b.append(crc8(b, 0x07, 0x80))
    halves = [0, 1] * 8 + [0, 0, 1, 1, 1, 1]          # preamble, sync
    prev = halves[-1]
    for bit in bits_of(b) + [0, 0]:                    # 72 data bits + 2 trailer cells
        first = 1 - prev                               # the clock transition
        second = first if bit else 1 - first
        halves += [first, second]
        prev = second
    one = nrz_segments(halves, 52.0)
    segs = []
    for k in range(int(s.get('copies', 2))):
        if k:
            segs.append((0, 3000))                     # carrier off between copies
        segs += one
    return 'FSK', segs, 'Toyota %08x' % ident, {'bytes': bytes(b).hex()}


KINDS = {'ev1527': ev1527, 'pt2262': pt2262, 'acurite_tower': acurite_tower, 'tx141th': tx141th, 'wh24': wh24,
         'toyota': toyota}


# --------------------------------------------------------- waveforms ---
def smooth(x, n):
    if n <= 1:
        return x
    w = np.hanning(n + 2)[1:-1]
    return np.convolve(x, w / w.sum(), mode='same')


def waveform(mod, segs, clock, dev_hz, rng):
    """unit-amplitude complex baseband at 2.4 MHz (carrier at 0 Hz) of one
    transmission, from its segments (us)"""
    edges = np.concatenate([[0.0], np.cumsum([d * clock for _, d in segs])]) * 1e-6 * FS
    pad = 48
    n = int(math.ceil(edges[-1])) + 2 * pad
    lv = np.zeros(n)
    for (v, _), a, e in zip(segs, edges[:-1], edges[1:]):
        lv[pad + int(round(a)):pad + int(round(e))] = v
    if mod == 'OOK':
        env = smooth(lv, 9)                            # ~4 us edges
        ph = np.zeros(n)
    else:
        on = smooth((lv != 0).astype(float), 25)       # ~10 us ramp at the ends
        f = smooth(lv, 11) * dev_hz                    # +-dev, edges smoothed over ~5 us
        ph = 2 * np.pi * np.cumsum(f) / FS
        env = on
    ph = ph + rng.uniform(0, 2 * np.pi)
    return (env * np.exp(1j * ph)).astype(np.complex64), pad


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
    rf = float(scene.get('rf_hz', 433.92e6))
    T = float(scene.get('seconds', 6))
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.5))
    noise_p = 2 * sig_n ** 2
    phi = -2 * np.pi * np.arange(M) / M

    waves = []      # (k0, wave, steer)
    truth = []
    for si, s in enumerate(scene.get('sources', [])):
        kind = s['kind']
        if kind not in KINDS:
            sys.exit('unknown kind %s (one of %s)' % (kind, ', '.join(KINDS)))
        mod, segs, tid, info = KINDS[kind](s)
        b = float(s.get('bearing', 0))
        off = float(s.get('offset_hz', 0))
        lam = C / (rf + off)
        steer = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(b) - phi))
        amp = math.sqrt(noise_p * 10 ** (float(s.get('level_db', 10)) / 10))
        clock = float(s.get('clock', 1.0))
        dev = float(s.get('dev_hz', 30e3))
        sent = []
        for t0 in s.get('at', [0.5]):
            w, pad = waveform(mod, segs, clock, dev, rng)
            k0 = int(round(float(t0) * FS)) - pad
            if k0 < 0 or k0 + len(w) > N:
                sys.exit('%s: the transmission at %.3f s (%.0f ms) does not fit in the %.1f s scene' %
                         (tid, t0, 1e3 * len(w) / FS, T))
            kk = np.arange(k0, k0 + len(w))
            w = w * np.complex64(amp) * np.exp(2j * np.pi * off * kk / FS).astype(np.complex64)
            waves.append((k0, w, steer.astype(np.complex64)))
            sent.append([round(float(t0), 4), round(float(t0) + (len(w) - 2 * pad) / FS, 4)])
        tr = {'name': tid, 'tid': tid, 'kind': kind, 'modulation': mod, 'bearing': b, 'offset_hz': off,
              'mhz': round((rf + off) / 1e6, 4), 'level_db': float(s.get('level_db', 10)), 'transmissions': sent}
        tr.update(info)
        truth.append(tr)
        print('%-28s %s  %+7.1f kHz  bearing %5.1f  level %4.1f dB  %d x %.0f ms' %
              (tid, mod, off / 1e3, b, tr['level_db'], len(sent), 1e3 * (sent[0][1] - sent[0][0]) if sent else 0))
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = ((rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n).astype(np.complex64)
            for k0, w, steer in waves:
                a, e = max(c0, k0), min(c1, k0 + len(w))
                if e <= a:
                    continue
                x[a - c0:e - c0, :] += w[a - k0:e - k0, None] * steer[None, :]
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
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s), %d transmissions' % (out, M, N / FS, fmt, len(waves)))


if __name__ == '__main__':
    main()
