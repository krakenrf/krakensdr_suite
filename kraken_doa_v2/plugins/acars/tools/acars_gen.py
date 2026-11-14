#!/usr/bin/env python3
"""ACARS (VHF, ARINC 618) test transmissions: 2400 bit/s MSK (1200 / 2400 Hz,
NRZI) on an AM carrier, real block framing (pre-key, '+' '*' SYN SYN SOH,
odd parity, CRC-16/KERMIT BCS, DEL).

  acars_gen.py OUT.cf32 [--rate 48000] [--offset HZ] [--depth 0.7]
                        [--prekey 16] --at T "KIND:ADDR:LABEL:BLOCK:TEXT" ...

One transmitter's blocks at the given times (s), complex baseband at --rate
(cf32) + OUT.json {rate}. KIND: down (aircraft -> ground: BLOCK a digit,
TEXT starts with the message number + flight ID, e.g. "M01AXY0123..."),
up (ground -> aircraft: BLOCK a letter), down+ / up+ (ETB: more blocks
follow). ADDR: the registration (padded to 7 with '.'), "" = broadcast
(NULs, a squitter). LABEL: 2 characters, "_d" = the general response _DEL.
TEXT "" = no text (the block ends at ETX after the block ID). Put several
transmitters at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

BAUD = 2400.0


def crc_kermit(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c


def par(ch):
    v = ord(ch) & 0x7F if isinstance(ch, str) else ch & 0x7F
    return v | (0x80 if bin(v).count('1') % 2 == 0 else 0)


def block_bytes(spec):
    kind, addr, label, bid, text = spec.split(':', 4)
    etb = kind.endswith('+')
    kind = kind.rstrip('+')
    if kind not in ('down', 'up'):
        raise SystemExit('KIND must be down / up / down+ / up+: ' + spec)
    if label == '_d':
        label = '_\x7f'
    if len(label) != 2 or len(bid) != 1:
        raise SystemExit('LABEL must be 2 characters, BLOCK 1: ' + spec)
    a = ('.' * 7 + addr)[-7:] if addr else '\0' * 7
    body = [par('2')] + [par(c) for c in a] + [par('\x15')] + [par(c) for c in label] + [par(bid)]
    if text:
        body += [par('\x02')] + [par(c) for c in text[:220]]
    body += [par('\x17' if etb else '\x03')]
    bcs = crc_kermit(body)
    return bytes(body) + bytes([bcs & 0xFF, bcs >> 8, par('\x7f')])


def bits_of(block, prekey):
    bits = [1] * (8 * prekey)
    for c in [par('+'), par('*'), par('\x16'), par('\x16'), par('\x01')] + list(block):
        bits += [(c >> k) & 1 for k in range(8)]
    return bits + [1] * 8   # a character of trailer while the carrier drops


def msk_audio(bits, fa):
    # NRZI: a bit like the previous one = 2400 Hz (a whole cycle), a change =
    # 1200 Hz (half a cycle); phase continuous, starting from 1 (the pre-key)
    prev = 1
    f = []
    for b in bits:
        f.append(2400.0 if b == prev else 1200.0)
        prev = b
    spb = fa / BAUD
    n = int(len(bits) * spb)
    idx = np.minimum((np.arange(n) / spb).astype(int), len(bits) - 1)
    return np.sin(2 * np.pi * np.cumsum(np.asarray(f)[idx]) / fa)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, offset, depth, prekey = 48000.0, 0.0, 0.7, 16
    blocks = []
    i = 1
    while i < len(a):
        if a[i] == '--rate':
            rate = float(a[i + 1]); i += 2
        elif a[i] == '--offset':
            offset = float(a[i + 1]); i += 2
        elif a[i] == '--depth':
            depth = float(a[i + 1]); i += 2
        elif a[i] == '--prekey':
            prekey = int(a[i + 1]); i += 2
        elif a[i] == '--at':
            blocks.append((float(a[i + 1]), a[i + 2])); i += 3
        else:
            print('unknown option', a[i]); sys.exit(2)
    fa = 48000.0   # audio rate (a multiple of 2400: 20 samples per bit)
    end = 0.0
    bursts = []
    for t, spec in blocks:
        audio = msk_audio(bits_of(block_bytes(spec), prekey), fa)
        # carrier up 10 ms before the modulation and 5 ms after it
        env = np.concatenate([np.zeros(int(0.01 * fa)), audio, np.zeros(int(0.005 * fa))])
        bursts.append((t, 1.0 + depth * env, spec))
        end = max(end, t + len(env) / fa)
    n = int((end + 0.3) * rate)
    x = np.zeros(n, np.complex128)
    for t, am, spec in bursts:
        y = am.astype(np.complex128)
        if abs(rate - fa) > 1e-6:
            from fractions import Fraction
            from scipy.signal import resample_poly
            fr = Fraction(rate / fa).limit_denominator(1000)
            y = resample_poly(y, fr.numerator, fr.denominator)
        n0 = int(round(t * rate))
        m = min(len(y), n - n0)
        k = np.arange(m) + n0
        x[n0:n0 + m] += y[:m] * np.exp(2j * np.pi * offset * k / rate)
        print('%8.3f .. %8.3f s  %s' % (t, t + m / rate, spec))
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
