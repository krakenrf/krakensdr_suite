#!/usr/bin/env python3
"""NXDN96 test transmissions (conventional RDCH, 4800 Bd 4FSK, RRC 0.2,
+-800 / +-2400 Hz): a voice call as on the air - a header frame with VCALL
in both FACCH1 halves, voice frames (random AMBE+2 bits) with the VCALL in
the SACCH superframe (4 fragments, SR 3..0), a TX_REL frame - each frame
with frame sync, LICH, SACCH (CRC-6) / FACCH1 (CRC-12), the punctured
rate-1/2 K=5 code, interleaving and the frame scrambler.

  nxdn_gen.py OUT.cf32 [--rate 48000] --at T --dur S --src UNIT --dst TG [--unit 1]
              [--ran 1] [--outbound 1] [--at ... (next call)]

One radio's calls as complex baseband (cf32) + OUT.json. Put several radios
at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

SPS = 10                 # 48 kHz / 4800 Bd
FSW = [-3, 1, -3, 3, -3, -3, 3, 3, -1, 3]
SCRAMBLER = [0x00, 0x00, 0x00, 0x82, 0xA0, 0x88, 0x8A, 0x00, 0xA2, 0xA8, 0x82, 0x8A, 0x82, 0x02, 0x20, 0x08,
             0x8A, 0x20, 0xAA, 0xA2, 0x82, 0x08, 0x22, 0x8A, 0xAA, 0x08, 0x28, 0x88, 0x28, 0x28, 0x00, 0x0A,
             0x02, 0x82, 0x20, 0x28, 0x82, 0x2A, 0xAA, 0x20, 0x22, 0x80, 0xA8, 0x8A, 0x08, 0xA0, 0xAA, 0x02]
DIBIT = {(0, 0): 1, (0, 1): 3, (1, 0): -1, (1, 1): -3}
SYM_BITS = {1: (0, 0), 3: (0, 1), -1: (1, 0), -3: (1, 1)}


def crc_ones(bits, width, poly):
    mask, top = (1 << width) - 1, 1 << (width - 1)
    crc = mask
    for b in bits:
        fb = bool(crc & top) ^ bool(b)
        crc = (crc << 1) & mask
        if fb:
            crc ^= poly
    return [(crc >> (width - 1 - i)) & 1 for i in range(width)]


def conv(bits):
    # K = 5, G1 = 0x19, G2 = 0x17 (register: bit 0 = the input)
    out, st = [], 0
    for b in bits:
        reg = (st << 1) | b
        out += [bin(reg & 0x19).count('1') & 1, bin(reg & 0x17).count('1') & 1]
        st = reg & 15
    return out


def code(info, crc_w, poly, period, punct, rows):
    bits = info + crc_ones(info, crc_w, poly) + [0, 0, 0, 0]
    m = conv(bits)
    d = [b for i, b in enumerate(m) if i % period not in punct]
    n = len(d)
    cols = n // rows
    rx = [0] * n
    for i in range(n):
        rx[(i % cols) * rows + i // cols] = d[i]
    return rx


def u(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def l3(kind, src, dst, nbits):
    if kind == 'vcall':
        b = [0x01, 0x00, 0x00, src >> 8, src & 0xFF, dst >> 8, dst & 0xFF, 0x00, 0x00, 0x00]
    else:   # TX_REL
        b = [0x08, 0x00, 0x00, src >> 8, src & 0xFF, dst >> 8, dst & 0xFF, 0x00, 0x00, 0x00]
    bits = []
    for x in b:
        bits += u(x, 8)
    return bits[:nbits]


def frame(usc, option, sacch_info, halves, outbound):
    rfct = 2   # conventional RDCH
    lich = (rfct << 6) | (usc << 4) | (option << 2) | (outbound << 1)
    lich |= ((lich >> 7) ^ (lich >> 6) ^ (lich >> 5) ^ (lich >> 4)) & 1
    bits = []
    for s in FSW:
        bits += list(SYM_BITS[s])
    for i in range(8):
        bits += [(lich >> (7 - i)) & 1, 1]
    bits += code(sacch_info, 6, 0x27, 6, {5}, 5)                       # SACCH 60
    for h in halves:
        bits += h                                                     # 2 x 144
    assert len(bits) == 384
    for i in range(384):
        if (SCRAMBLER[i // 8] >> (7 - i % 8)) & 1:
            bits[i] ^= 1
    return [DIBIT[(bits[2 * j], bits[2 * j + 1])] for j in range(192)]


def call(src, dst, ran, dur, outbound, rng):
    facch_v = code(l3('vcall', src, dst, 80), 12, 0x80F, 4, {1}, 9)
    facch_r = code(l3('rel', src, dst, 80), 12, 0x80F, 4, {1}, 9)
    vc72 = l3('vcall', src, dst, 72)
    syms = []
    # header: VCALL in both FACCH1 halves, non-superframe SACCH
    syms += frame(0, 0, [0, 0] + u(ran, 6) + [0] * 18, [facch_v, facch_v], outbound)
    n = max(4, int(dur / 0.04) // 4 * 4)
    for f in range(n):
        sr = 3 - f % 4
        sa = u(sr, 2) + u(ran, 6) + vc72[18 * (f % 4):18 * (f % 4) + 18]
        syms += frame(2, 3, sa, [list(rng.integers(0, 2, 144)), list(rng.integers(0, 2, 144))], outbound)
    syms += frame(0, 0, [0, 0] + u(ran, 6) + [0] * 18, [facch_r, facch_r], outbound)
    return syms


def rrc(sps, span, alpha):
    t = np.arange(-span * sps, span * sps + 1) / sps
    h = np.zeros(len(t))
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(4 * alpha * x) - 1) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h


def modulate(syms, rate):
    up = np.zeros(len(syms) * SPS + 12 * SPS)
    up[6 * SPS:6 * SPS + len(syms) * SPS:SPS] = syms
    h = rrc(SPS, 6, 0.2)
    f = np.convolve(up, h / h[len(h) // 2], 'same') * 800.0   # +-1 -> +-800 Hz, +-3 -> +-2400 Hz
    x = np.exp(2j * np.pi * np.cumsum(f) / 48000.0)
    if abs(rate - 48000.0) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / 48000.0).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    return x


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
            cur = {'at': float(v), 'dur': 3.0, 'src': 1001, 'dst': 100, 'ran': 1, 'outbound': 0}
            txs.append(cur)
        else:
            cur[k] = float(v) if k == 'dur' else int(v)
        i += 2
    rng = np.random.default_rng(11)
    parts = []
    for tx in txs:
        y = modulate(call(tx['src'], tx['dst'], tx['ran'], tx['dur'], tx['outbound'], rng), rate)
        parts.append((tx, y))
        print('%8.3f .. %8.3f s  unit %d -> TG %d  RAN %d' % (tx['at'], tx['at'] + len(y) / rate, tx['src'], tx['dst'], tx['ran']))
    end = max(tx['at'] + len(y) / rate for tx, y in parts) + 0.3
    x = np.zeros(int(end * rate), np.complex128)
    for tx, y in parts:
        n0 = int(round(tx['at'] * rate))
        x[n0:n0 + len(y)] += y
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
