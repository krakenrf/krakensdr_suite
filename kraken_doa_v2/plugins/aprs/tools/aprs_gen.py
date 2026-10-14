#!/usr/bin/env python3
"""APRS / AX.25 1200 bit/s test transmissions: Bell 202 AFSK on NBFM, real
framing (AX.25 UI frames, bit stuffing, NRZI, CRC-16/X.25 FCS).

  aprs_gen.py OUT.cf32 [--rate 48000] [--dev 3000] [--txdelay 0.25]
                       [--offset HZ] --at T "SRC>DST,PATH:info" [--at T "..."]...

One transmitter's frames at the given times (s), complex baseband at --rate
(cf32) + OUT.json {rate}. A path entry ending in "*" has the "has been
repeated" bit set: "N0CALL-9>APRS,DIGI*,WIDE2-1:..." is the frame as DIGI
retransmits it. Put several transmitters at their own bearings with
plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np


def crc_x25(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def addr(call, last, hbit=False, cbit=False):
    base, _, ssid = call.partition('-')
    base = (base.upper() + '      ')[:6]
    out = bytes(ord(c) << 1 for c in base)
    s = 0x60 | ((int(ssid) if ssid else 0) & 15) << 1 | (1 if last else 0)
    if hbit or cbit:
        s |= 0x80
    return out + bytes([s])


def frame_bytes(tnc2):
    head, _, info = tnc2.partition(':')
    src, _, rest = head.partition('>')
    parts = rest.split(',')
    dst, path = parts[0], [p for p in parts[1:] if p]
    b = addr(dst, False, cbit=True) + addr(src, not path)
    for i, p in enumerate(path):
        b += addr(p.rstrip('*'), i == len(path) - 1, hbit=p.endswith('*'))
    b += bytes([0x03, 0xF0]) + info.encode('latin-1')
    f = crc_x25(b)
    return b + bytes([f & 0xFF, f >> 8])


def hdlc_bits(payload, nflags_before, nflags_after=2):
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    bits = flag * nflags_before
    ones = 0
    for byte in payload:
        for k in range(8):
            b = (byte >> k) & 1
            bits.append(b)
            if b:
                ones += 1
                if ones == 5:
                    bits.append(0)
                    ones = 0
            else:
                ones = 0
    return bits + flag * nflags_after


def afsk(bits, fs, baud=1200.0):
    # NRZI: 0 = change tone, 1 = keep; start on mark
    tone = 1200.0
    level = []
    for b in bits:
        if b == 0:
            tone = 2200.0 if tone == 1200.0 else 1200.0
        level.append(tone)
    spb = fs / baud
    n = int(len(bits) * spb)
    idx = np.minimum((np.arange(n) / spb).astype(int), len(bits) - 1)
    f = np.asarray(level)[idx]
    return np.sin(2 * np.pi * np.cumsum(f) / fs)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, dev, txdelay, offset = 48000.0, 3000.0, 0.25, 0.0
    frames = []
    i = 1
    while i < len(a):
        if a[i] == '--rate':
            rate = float(a[i + 1]); i += 2
        elif a[i] == '--dev':
            dev = float(a[i + 1]); i += 2
        elif a[i] == '--txdelay':
            txdelay = float(a[i + 1]); i += 2
        elif a[i] == '--offset':
            offset = float(a[i + 1]); i += 2
        elif a[i] == '--at':
            frames.append((float(a[i + 1]), a[i + 2])); i += 3
        else:
            print('unknown option', a[i]); sys.exit(2)
    fa = 48000.0   # audio rate
    end = 0.0
    bursts = []
    for t, tnc2 in frames:
        bits = hdlc_bits(frame_bytes(tnc2), max(1, int(txdelay * 1200 / 8)))
        audio = afsk(bits, fa)
        tail = np.zeros(int(0.03 * fa))
        audio = np.concatenate([np.zeros(int(0.02 * fa)), audio, tail])   # carrier up before / after the modulation
        bursts.append((t, audio, tnc2))
        end = max(end, t + len(audio) / fa)
    n = int((end + 0.5) * rate)
    x = np.zeros(n, np.complex128)
    for t, audio, tnc2 in bursts:
        # FM at the audio rate, then to the output rate
        ph = 2 * np.pi * np.cumsum(audio * dev) / fa
        y = np.exp(1j * ph)
        if abs(rate - fa) > 1e-6:
            from fractions import Fraction
            from scipy.signal import resample_poly
            fr = Fraction(rate / fa).limit_denominator(1000)
            y = resample_poly(y, fr.numerator, fr.denominator)
        n0 = int(round(t * rate))
        m = min(len(y), n - n0)
        k = np.arange(m) + n0
        x[n0:n0 + m] += y[:m] * np.exp(2j * np.pi * offset * k / rate)
        print('%8.3f .. %8.3f s  %s' % (t, t + m / rate, tnc2))
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
