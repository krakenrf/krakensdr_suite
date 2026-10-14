#!/usr/bin/env python3
"""D-STAR test transmissions: GMSK 4800 bit/s (BT 0.5, +-1200 Hz) with the
real framing - preamble, frame sync, RF header (MY / UR / RPT1 / RPT2,
CRC-16/X.25, rate-1/2 K=3 code, 24-column interleaver, x^7+x^4+1
scrambler), voice frames (72 voice bits - random, no AMBE - + 24 scrambled
slow-data bits, a data sync every 21st frame) and the end pattern.

  dstar_gen.py OUT.cf32 [--rate 48000] --at T --dur S --my CALL [--sfx ID51] [--ur CQCQCQ]
               [--rpt1 "F1ZIL  B"] [--rpt2 "F1ZIL  G"] [--at ... (next transmission)]

One radio's transmissions as complex baseband (cf32) + OUT.json. Put several
radios at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys
from math import erf, log, pi, sqrt

import numpy as np

BAUD = 4800.0


def crc_x25(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def field(s, n):
    return (s.upper() + ' ' * n)[:n].encode('ascii')


def header_bits(my, sfx, ur, rpt1, rpt2, flags=0x00):
    h = bytes([flags, 0, 0]) + field(rpt2, 8) + field(rpt1, 8) + field(ur, 8) + field(my, 8) + field(sfx, 4)
    c = crc_x25(h)
    h += bytes([c & 0xFF, c >> 8])
    bits = [(h[i // 8] >> (i % 8)) & 1 for i in range(328)] + [0, 0]
    # rate 1/2, K = 3, G = 7, 5 (register: bit 0 = the input)
    coded, st = [], 0
    for b in bits:
        reg = (st << 1) | b
        coded += [bin(reg & 7).count('1') & 1, bin(reg & 5).count('1') & 1]
        st = reg & 3
    # whitening x^7 + x^4 + 1, all ones
    s = [1] * 7
    scr = []
    for _ in range(660):
        b = s[6] ^ s[3]
        scr.append(b)
        s = [b] + s[:6]
    air = []
    for i in range(660):
        if i < 336:
            col, row = i // 28, i % 28
        else:
            col, row = 12 + (i - 336) // 27, (i - 336) % 27
        air.append(coded[row * 24 + col] ^ scr[i])
    return air


def word(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def transmission(my, sfx, ur, rpt1, rpt2, dur, rng):
    bits = [1, 0] * 32 + [1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0]
    bits += header_bits(my, sfx, ur, rpt1, rpt2, 0x40 if rpt1.strip() else 0x00)
    scr = [0x70, 0x4F, 0x93]
    nframes = max(21, int(dur / 0.02))
    for f in range(nframes):
        bits += list(rng.integers(0, 2, 72))
        if f % 21 == 0:
            bits += word(0xAAB468, 24)          # data sync
        else:
            for k in range(3):                  # slow data: filler 0x66, scrambled, LSB first
                bits += [((0x66 ^ scr[k]) >> j) & 1 for j in range(8)]
    bits += word(0xAAAA135E, 32) + [0, 1] * 8   # end pattern
    return bits


def gmsk(bits, rate, bt=0.5):
    a = np.where(np.asarray(bits) > 0, 1.0, -1.0)
    T = 1 / BAUD
    sig = sqrt(log(2)) / (2 * pi * bt)
    n = int((len(a) + 2) * T * rate)
    t = np.arange(n) / rate
    freq = np.zeros(n)
    verf = np.vectorize(erf)
    span = int(3 * T * rate) + 2
    for k in range(len(a)):
        c = (k + 0.5) * T
        i0 = max(0, int((c - 3 * T) * rate))
        tt = t[i0:i0 + 2 * span] - c
        g = 0.5 * (verf((tt + T / 2) / (sqrt(2) * sig * T)) - verf((tt - T / 2) / (sqrt(2) * sig * T)))
        freq[i0:i0 + 2 * span] += a[k] * g * (BAUD / 4)
    return np.exp(2j * pi * np.cumsum(freq) / rate)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate = 48000.0
    txs, cur = [], None
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'at':
            cur = {'at': float(v), 'dur': 3.0, 'my': 'N0CALL', 'sfx': '', 'ur': 'CQCQCQ', 'rpt1': '', 'rpt2': ''}
            txs.append(cur)
        else:
            cur[k] = float(v) if k == 'dur' else v
        i += 2
    rng = np.random.default_rng(7)
    parts = []
    for tx in txs:
        y = gmsk(transmission(tx['my'], tx['sfx'], tx['ur'], tx['rpt1'], tx['rpt2'], tx['dur'], rng), rate)
        parts.append((tx, y))
        print('%8.3f .. %8.3f s  MY %s / %s  UR %s  RPT1 %s' % (tx['at'], tx['at'] + len(y) / rate, tx['my'], tx['sfx'],
                                                             tx['ur'], tx['rpt1']))
    end = max(tx['at'] + len(y) / rate for tx, y in parts) + 0.3
    x = np.zeros(int(end * rate), np.complex128)
    for tx, y in parts:
        n0 = int(round(tx['at'] * rate))
        x[n0:n0 + len(y)] += y
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
