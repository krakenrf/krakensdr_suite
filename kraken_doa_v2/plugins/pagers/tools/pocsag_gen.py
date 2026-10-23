#!/usr/bin/env python3
"""POCSAG pager test transmissions (ITU-R M.584): 2-FSK on the carrier, real
framing - 576-bit 1010 preamble, batches of the sync codeword 0x7CD215D8 + 8
frames x 2 codewords, BCH(31,21) + even parity, address codewords in the
frame of the RIC's low 3 bits, numeric (4-bit BCD, LSB first) and
alphanumeric (7-bit ASCII, LSB first) messages, idle fill 0x7A89C197.

  pocsag_gen.py OUT.cf32 [--rate 48000] [--dev 4500] [--offset HZ] [--bt 0.5]
                --page T BAUD RIC FUNC TEXT [--page ...] [--batch-gap]
                [--fm T DUR TONE_HZ DEV_HZ]

--page: one transmission at T s (several pages at the same T and baud go out
in ONE transmission, one after the other). FUNC 0 = numeric (TEXT: digits,
space, -, *, U, (, )), 3 = alphanumeric. --fm: an analog FM transmission
(a voice-like interferer: TONE_HZ with a slow warble, DEV_HZ peak) - a
different transmitter, so give it its own output file + bearing.
Binary 1 = the LOWER frequency (POCSAG). One transmitter per file: complex
baseband at --rate (cf32) + OUT.json {rate}; place transmitters at bearings
with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

SYNC = 0x7CD215D8
IDLE = 0x7A89C197
BCH_G = 0x769          # x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1


def bch_cw(data21):
    """21 bits (flag + 20) -> 32-bit codeword: BCH(31,21) check + even parity"""
    c = data21 << 10
    for i in range(30, 9, -1):
        if c & (1 << i):
            c ^= BCH_G << (i - 10)
    cw31 = (data21 << 10) | (c & 0x3FF)
    return (cw31 << 1) | (bin(cw31).count('1') & 1)


def address_cw(ric, func):
    return bch_cw(((ric >> 3) & 0x3FFFF) << 2 | (func & 3))


def message_cws(text, func):
    """the message codewords (flag 1 + 20 data bits each)"""
    bits = []
    if func == 0:
        table = {c: i for i, c in enumerate("0123456789*U -)(")}
        for ch in text:
            v = table[ch]
            bits += [(v >> k) & 1 for k in range(4)]          # LSB first
        while len(bits) % 20:
            bits += [0, 0, 1, 1]                                # space (0xC, LSB first)
    else:
        for ch in text:
            v = ord(ch) & 0x7F
            bits += [(v >> k) & 1 for k in range(7)]          # LSB first
        bits += [0, 0, 1, 0, 0, 0, 0]                           # EOT
        while len(bits) % 20:
            bits.append(0)
    out = []
    for i in range(0, len(bits), 20):
        w = 0
        for b in bits[i:i + 20]:
            w = (w << 1) | b
        out.append(bch_cw((1 << 20) | w))
    return out


def transmission_bits(pages):
    """[(ric, func, text)] -> the bit list of one transmission"""
    cws = []   # codewords after the preamble, batch positions 0..15 repeating
    for ric, func, text in pages:
        frame = ric & 7
        while len(cws) % 16 // 2 != frame:     # wait for the RIC's frame
            cws.append(IDLE)
        cws.append(address_cw(ric, func))
        cws += message_cws(text, func)
    cws.append(IDLE)
    while len(cws) % 16:
        cws.append(IDLE)
    bits = [(k + 1) & 1 for k in range(576)]    # 1010...
    for b in range(0, len(cws), 16):
        for w in [SYNC] + cws[b:b + 16]:
            bits += [(w >> (31 - k)) & 1 for k in range(32)]
    return bits


def fsk(bits, baud, rate, dev, bt):
    """NRZ 2-FSK, 1 = -dev, Gaussian-smoothed frequency (BT), continuous phase"""
    n = int(np.ceil(len(bits) * rate / baud)) + 1
    t = np.arange(n) / rate
    k = np.minimum((t * baud).astype(int), len(bits) - 1)
    f = np.where(np.asarray(bits)[k] == 1, -dev, dev).astype(np.float64)
    if bt > 0:
        sps = rate / baud
        sigma = np.sqrt(np.log(2)) / (2 * np.pi * bt) * sps
        h = np.exp(-0.5 * (np.arange(-int(3 * sigma) - 1, int(3 * sigma) + 2) / sigma) ** 2)
        f = np.convolve(f, h / h.sum(), mode='same')
    return np.exp(2j * np.pi * np.cumsum(f) / rate)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, dev, offset, bt = 48000.0, 4500.0, 0.0, 0.5
    pages, fms = [], []
    i = 1
    while i < len(a):
        if a[i] == '--rate':
            rate = float(a[i + 1]); i += 2
        elif a[i] == '--dev':
            dev = float(a[i + 1]); i += 2
        elif a[i] == '--offset':
            offset = float(a[i + 1]); i += 2
        elif a[i] == '--bt':
            bt = float(a[i + 1]); i += 2
        elif a[i] == '--page':
            pages.append((float(a[i + 1]), int(a[i + 2]), int(a[i + 3]), int(a[i + 4]), a[i + 5])); i += 6
        elif a[i] == '--fm':
            fms.append(tuple(float(v) for v in a[i + 1:i + 5])); i += 5
        else:
            print('unknown option', a[i]); sys.exit(2)
    # pages at the same time + rate = one transmission
    tx = {}
    for t, baud, ric, func, text in pages:
        if not 0 <= ric < 1 << 21 or func not in (0, 1, 2, 3):
            sys.exit('bad RIC / function')
        tx.setdefault((t, baud), []).append((ric, func, text))
    bursts = []
    for (t, baud), pg in sorted(tx.items()):
        bits = transmission_bits(pg)
        y = fsk(bits, baud, rate, dev, bt)
        bursts.append((t, y))
        for ric, func, text in pg:
            print('%7.3f s  %4d bit/s  RIC %07d F%d %s: %s' % (t, baud, ric, func, 'numeric' if func == 0 else 'alpha', text))
        print('          %d bits, %.3f s on air' % (len(bits), len(bits) / baud))
    for t, dur, tone, fdev in fms:
        n = int(dur * rate)
        tt = np.arange(n) / rate
        audio = np.sin(2 * np.pi * (tone * tt + 0.15 * tone / 3.0 * np.sin(2 * np.pi * 3.0 * tt)))
        audio *= 0.6 + 0.4 * np.sin(2 * np.pi * 1.7 * tt) ** 2
        bursts.append((t, np.exp(2j * np.pi * fdev * np.cumsum(audio) / rate)))
        print('%7.3f s  analog FM %.1f s, %.0f Hz tone, %.0f Hz deviation' % (t, dur, tone, fdev))
    end = max((t + len(y) / rate for t, y in bursts), default=0.0)
    n = int((end + 0.1) * rate)
    x = np.zeros(n, np.complex128)
    for t, y in bursts:
        n0 = int(round(t * rate))
        m = min(len(y), n - n0)
        # a short power ramp: the carrier keys up / down
        ramp = np.ones(m)
        r = min(int(0.002 * rate), m // 2)
        ramp[:r] = np.linspace(0, 1, r)
        ramp[m - r:] = np.linspace(1, 0, r)
        k = np.arange(m) + n0
        x[n0:n0 + m] += y[:m] * ramp * np.exp(2j * np.pi * offset * k / rate)
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
