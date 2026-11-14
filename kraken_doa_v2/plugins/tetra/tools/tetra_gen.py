#!/usr/bin/env python3
"""TETRA downlink test signal: one cell's continuous base-station carrier as
on the air (EN 300 392-2) - pi/4-DQPSK 18 ksym/s, RRC 0.35, 255-symbol
timeslots with the TDMA time running (TN / FN / MN). Frame 18: synchronization
bursts (BSCH: MCC, MNC, colour code, TDMA time - CRC-16, RCPC 2/3 of the
rate-1/4 K=5 mother code, block interleaver, scrambled with the BSCH code;
block 2 = BNCH SYSINFO with the location area, scrambled with the cell's
code). Frames 1-17: normal bursts, SCH/F (an empty MAC-RESOURCE) on
timeslot 1, the AACH (Reed-Muller 30,14) on every slot.

  tetra_gen.py OUT.cf32 --mcc 530 --mnc 1 --cc 12 --la 1001 [--carrier 3600]
               [--rate 72000] --at T --dur S [--at T --dur S ...]

The cell's carrier on during each [at, at + dur) (cf32 + OUT.json). Put
several cells at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

SPS = 4
FS = 72000.0
Y = [1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1]
N = [1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0]
RM_GEN = [[1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0], [0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0],
          [1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0], [1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0],
          [1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0], [0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0],
          [0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0], [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1],
          [1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1], [0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1],
          [0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1], [0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1],
          [0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1], [0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1]]


def u(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


# scrambling sequence (EN 300 392-2 8.2.5) - as dig_fec.cpp: the term x^k of
# c(x) = 1 + x + x^2 + x^4 + x^5 + x^7 + x^8 + x^10 + x^11 + x^12 + x^16 +
# x^22 + x^23 + x^26 + x^32 is cell 32 - k (cell 31 = the latest output bit)
SCRAMB_TAPS = sum(1 << (32 - k) for k in (1, 2, 4, 5, 7, 8, 10, 11, 12, 16, 22, 23, 26, 32))


def scramble_seq(n, state):
    out = []
    for _ in range(n):
        s = bin(state & SCRAMB_TAPS).count('1') & 1
        state = (state >> 1) | (s << 31)
        out.append(s)
    return out


def scramb_init(mcc, mnc, cc):
    """the 30-bit extended colour code (MCC 10, MNC 14, colour code 6 bits) + two 1s"""
    ecc = ((mcc & 0x3FF) << 20) | ((mnc & 0x3FFF) << 6) | (cc & 0x3F)
    return ((ecc << 2) | 3) & 0xFFFFFFFF


def crc16(bits):
    crc = 0xFFFF
    for b in bits:
        crc ^= (b & 1) << 15
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return u(crc ^ 0xFFFF, 16)


def block(info, n, a, scr):
    """type-1 bits -> CRC, tail, RCPC 2/3 (K=5 rate-1/4 mother), interleaver, scrambler"""
    t2 = info + crc16(info) + [0, 0, 0, 0]
    polys = [0x13, 0x1D, 0x17, 0x1B]
    mother, st = [], 0
    for b in t2:
        reg = (st << 1) | b
        mother += [bin(reg & p).count('1') & 1 for p in polys]
        st = reg & 15
    # rate 2/3: of every 8 mother-code bits the 1st, 2nd and 5th are sent
    sent = (0, 1, 4)
    t3 = [mother[8 * (j // 3) + sent[j % 3]] for j in range(n)]
    # (n, a) block interleaver: bit k (1..n) goes to position 1 + (a k mod n)
    t4 = [0] * n
    for k in range(n):
        t4[(a * (k + 1)) % n] = t3[k]
    s = scramble_seq(n, scr)
    return [t4[i] ^ s[i] for i in range(n)]


def aach(msg14, scr):
    par = 0
    for i in range(14):
        if (msg14 >> (13 - i)) & 1:
            row = 0
            for j in range(16):
                row = (row << 1) | RM_GEN[i][j]
            par ^= row
    cw = (msg14 << 16) | par
    bits = u(cw, 30)
    s = scramble_seq(30, scr)
    return [bits[i] ^ s[i] for i in range(30)]


def bsch(cell, tn, fn, mn):
    return (u(0, 4) + u(cell['cc'], 6) + u(tn - 1, 2) + u(fn, 5) + u(mn, 6) + u(0, 2) + u(0, 3) + [0, 0, 0] +
            u(cell['mcc'], 10) + u(cell['mnc'], 14) + u(1, 2) + u(2, 2) + [0])


def sysinfo(cell):
    b = u(2, 2) + u(0, 2) + u(cell['carrier'], 12) + u(3, 4) + u(0, 2) + u(0, 3) + [0] + u(0, 2) + u(3, 3)
    b += u(0, 4) + u(0, 4) + u(0, 4) + [0] + u(0, 16)
    b += [0] * (124 - 42 - len(b))
    b += u(cell['la'], 14) + u(0xFFFF, 16) + u(0b100000101000, 12)
    return b


def slot_bits(cell, tn, fn, mn):
    scr = scramb_init(cell['mcc'], cell['mnc'], cell['cc'])
    if fn == 18:
        # synchronization continuous downlink burst
        b = [0] * 14 + [1] * 8 + [0] * 64 + [1] * 8
        b += block(bsch(cell, tn, fn, mn), 120, 11, 3)
        b += Y
        b += aach(0, scr)
        b += block(sysinfo(cell), 216, 101, scr)
        b += [0] * 12
    else:
        # normal continuous downlink burst: SCH/F (empty MAC-RESOURCE)
        blk = block([0] * 268, 432, 103, scr)
        bb = aach(0, scr)
        b = [0] * 14 + blk[:216] + bb[:14] + N + bb[14:] + blk[216:] + [0] * 12
    assert len(b) == 510, len(b)
    return b


def rrc(sps, span, beta):
    t = np.arange(-span * sps, span * sps + 1) / sps
    h = np.zeros(len(t))
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - beta + 4 * beta / np.pi
        elif abs(abs(4 * beta * x) - 1) < 1e-9:
            h[i] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - beta)) + 4 * beta * x * np.cos(np.pi * x * (1 + beta))) / (np.pi * x * (1 - (4 * beta * x) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    cell = {'mcc': 530, 'mnc': 1, 'cc': 12, 'la': 1001, 'carrier': 3600}
    rate = FS
    on = []
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'at':
            on.append([float(v), 5.0])
        elif k == 'dur':
            on[-1][1] = float(v)
        else:
            cell[k] = int(v)
        i += 2
    end = max(t + d for t, d in on) + 0.2
    nslots = int(end * 18000 / 255) + 2
    rng = np.random.default_rng(cell['cc'])
    # the cell's own TDMA time and symbol timing
    tn, fn, mn = 1, int(rng.integers(1, 18)), int(rng.integers(1, 61))
    dphi = {(0, 0): np.pi / 4, (0, 1): 3 * np.pi / 4, (1, 0): -np.pi / 4, (1, 1): -3 * np.pi / 4}
    ph, syms = 0.0, []
    for _ in range(nslots):
        b = slot_bits(cell, tn, fn, mn)
        for k in range(255):
            ph += dphi[(b[2 * k], b[2 * k + 1])]
            syms.append(np.exp(1j * ph))
        tn += 1
        if tn > 4:
            tn = 1
            fn += 1
            if fn > 18:
                fn = 1
                mn = mn % 60 + 1
    up = np.zeros(len(syms) * SPS, np.complex128)
    up[::SPS] = syms
    x = np.convolve(up, rrc(SPS, 8, 0.35), 'same')
    if abs(rate - FS) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / FS).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    lead = int(rng.integers(0, 255 * SPS)) * rate / FS / rate   # random slot timing
    y = np.zeros(int(end * rate), np.complex128)
    for t, d in on:
        a0, a1 = int(t * rate), int((t + d) * rate)
        s0 = int((t + lead) * rate) % (255 * 4 * 18 * SPS)
        seg = x[s0:s0 + (a1 - a0)]
        ramp = np.minimum(1, np.minimum(np.arange(len(seg)), np.arange(len(seg))[::-1]) / (0.002 * rate))
        y[a0:a0 + len(seg)] = seg * ramp
        print('%8.3f .. %8.3f s  cell MCC %d MNC %d CC %d LA %d' % (t, t + len(seg) / rate, cell['mcc'], cell['mnc'],
                                                                   cell['cc'], cell['la']))
    y.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
