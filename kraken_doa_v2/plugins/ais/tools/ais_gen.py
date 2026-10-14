#!/usr/bin/env python3
"""AIS test transmissions: one ship's position reports (message 1, class A)
as on the air - GMSK BT 0.4, 9600 bit/s, NRZI, HDLC (training sequence,
flags, bit stuffing, CRC-16/X.25 FCS), bytes sent LSB first - on channel A
(161.975 MHz) or B (162.025 MHz) of a VFO on 162.000 MHz.

  ais_gen.py OUT.cf32 --mmsi 512000001 [--lat -36.8 --lon 174.8 --sog 12.3 --cog 45]
             [--rate 100000] --at T A|B [--at T A|B ...]

Complex baseband at --rate (the ais plugin's 100 kHz) + OUT.json. Put
several ships at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys
from math import erf, log, pi, sqrt

import numpy as np

BAUD = 9600.0


def crc_x25(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def msg1(mmsi, lat, lon, sog, cog):
    f = [(1, 6), (0, 2), (mmsi, 30), (0, 4), (128, 8), (int(round(sog * 10)), 10), (1, 1),
         (int(round(lon * 600000)) & ((1 << 28) - 1), 28), (int(round(lat * 600000)) & ((1 << 27) - 1), 27),
         (int(round(cog * 10)), 12), (int(round(cog)) % 360, 9), (30, 6), (0, 2), (0, 3), (0, 1), (0, 19)]
    bits = []
    for v, n in f:
        bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]
    return bits   # 168


def frame_bits(payload_bits):
    by = bytes(int(''.join(map(str, payload_bits[i:i + 8])), 2) for i in range(0, len(payload_bits), 8))
    fcs = crc_x25(by)
    data = by + bytes([fcs & 0xFF, fcs >> 8])
    out, ones = [], 0
    for b in data:
        for k in range(8):   # LSB first
            bit = (b >> k) & 1
            out.append(bit)
            ones = ones + 1 if bit else 0
            if ones == 5:
                out.append(0)
                ones = 0
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    train = [0, 1] * 12
    return train + flag + out + flag + [0] * 4


def gmsk(bits, rate, bt=0.4):
    # NRZI: 0 = change level
    lvl, a = 1, []
    for b in bits:
        if b == 0:
            lvl = -lvl
        a.append(lvl)
    a = np.asarray(a, float)
    T = 1 / BAUD
    sig = sqrt(log(2)) / (2 * pi * bt)
    n = int((len(a) + 2) * T * rate)
    t = np.arange(n) / rate
    freq = np.zeros(n)
    verf = np.vectorize(erf)
    for k in range(len(a)):
        c = (k + 0.5) * T
        sel = np.abs(t - c) < 3 * T
        tt = t[sel] - c
        g = 0.5 * (verf((tt + T / 2) / (sqrt(2) * sig * T)) - verf((tt - T / 2) / (sqrt(2) * sig * T)))
        freq[sel] += a[k] * g * (BAUD / 4)   # h = 0.5: +-2400 Hz
    ph = 2 * pi * np.cumsum(freq) / rate
    return np.exp(1j * ph)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'rate': 100000.0, 'mmsi': 512000001, 'lat': -36.8, 'lon': 174.8, 'sog': 12.3, 'cog': 45.0}
    tx = []
    i = 1
    while i < len(a):
        if a[i] == '--at':
            tx.append((float(a[i + 1]), a[i + 2].upper()))
            i += 3
        else:
            o[a[i].lstrip('-')] = float(a[i + 1])
            i += 2
    rate = o['rate']
    bits = frame_bits(msg1(int(o['mmsi']), o['lat'], o['lon'], o['sog'], o['cog']))
    burst = gmsk(bits, rate)
    ramp = np.minimum(1, np.minimum(np.arange(len(burst)), np.arange(len(burst))[::-1]) / (0.0004 * rate))
    burst *= ramp
    end = max(t for t, _ in tx) + len(burst) / rate + 0.2
    x = np.zeros(int(end * rate), np.complex128)
    for t, ch in tx:
        n0 = int(round(t * rate))
        k = np.arange(len(burst)) + n0
        x[n0:n0 + len(burst)] += burst * np.exp(2j * np.pi * (-25000 if ch == 'A' else 25000) * k / rate)
        print('%9.4f .. %9.4f s  channel %s  MMSI %d' % (t, t + len(burst) / rate, ch, int(o['mmsi'])))
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate, 'rf_hz': 162.0e6}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
