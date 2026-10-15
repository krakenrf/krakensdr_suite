#!/usr/bin/env python3
"""DMR Tier II test transmissions (ETSI TS 102 361-1, 4FSK 4800 symbols/s,
RRC 0.2, +-648 / +-1944 Hz) with the real framing and FEC:

- voice LC header / terminator with LC (full LC: FLCO 0 group voice, TG,
  source ID; RS(12,9) with the header / terminator CRC masks; BPTC(196,96);
  slot type CC + data type, Golay(20,8))
- voice superframes: burst A with the voice sync, B..F with the EMB (CC, PI,
  LCSS; QR(16,7)) and the embedded LC (5-bit checksum, Hamming(16,11) rows +
  column parity, interleaved over B..E; F null)
- idle bursts (BPTC) on a base station's free timeslot
- placeholder AMBE+2 frames (3 per burst) with valid FEC: Golay(24,12) c0,
  Golay(23,12) c1 whitened by the PN seeded from c0, c2 / c3 random - they
  are not speech; --voice random sends random bits instead

Two kinds of transmitter:
  --mode bs   a repeater (BS-sourced): continuous 30 ms slots, each a CACH
              (TACT: AT, TC = the timeslot, LCSS; Hamming(7,4)) + a burst;
              calls on timeslot 1 and 2 (may overlap), idle bursts between,
              BS syncs; keyed from 0.2 s before its first call to 0.3 s after
              its last
  --mode dmo  a radio in TDMA direct mode: only its own timeslot (a 27.5 ms
              burst every 60 ms, ramped), the TS1 / TS2 direct-mode syncs

  dmr_gen.py OUT.cf32 [--rate 48000] [--mode bs|dmo] [--cc 1] [--voice ambe|random]
             --at T --slot 1|2 --dur S --src ID --dst TG [--at ... (next call)]

One transmitter as complex baseband (cf32) + OUT.json. Put several at their
own bearings with plugins/sdk/tools/uca_scene.py. The FEC encoders follow the
decoder's (plugins/dmr/dmr.cpp, plugins/lib/dig_fec.cpp), which was
validated on real DMR recordings; the AMBE interleave / Golay tables are read
from plugins/lib/mbe_tables.hpp (mbelib / DSD, ISC).
"""
import json
import os
import re
import sys

import numpy as np

RATE = 48000.0
SPS = 10
LEVEL = {0b01: 3, 0b00: 1, 0b10: -1, 0b11: -3}
SYNC = {('bs', 'voice'): 0x755FD7DF75F7, ('bs', 'data'): 0xDFF57D75DF5D,
        ('ts1', 'voice'): 0x5D577F7757FF, ('ts1', 'data'): 0xF7FDD5DDFD55,
        ('ts2', 'voice'): 0x7DFFD5F55D5F, ('ts2', 'data'): 0xD7557F5FF7F5}
DT_HEADER, DT_TERM, DT_IDLE = 1, 2, 9

TABLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'lib', 'mbe_tables.hpp')


def table(name):
    src = open(TABLES).read()
    m = re.search(r'\b' + name + r'\[\d+\]\s*=\s*\{([^}]*)\}', src)
    return [int(v, 0) for v in m.group(1).replace('\n', ' ').split(',') if v.strip()]


rW, rX, rY, rZ = (table(n) for n in ('rW', 'rX', 'rY', 'rZ'))
GOLAY_GEN = table('golayGenerator')


def bits(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def val(b):
    v = 0
    for x in b:
        v = (v << 1) | x
    return v


def bytes_bits(bs):
    out = []
    for b in bs:
        out += bits(b, 8)
    return out


# --- block codes (dig_fec.cpp layouts, first transmitted bit = MSB) -----------------
def golay23_parity(d):
    r = d << 11
    for i in range(22, 10, -1):
        if r & (1 << i):
            r ^= 0xC75 << (i - 11)
    return r & 0x7FF


def golay20(d):
    cw = (d << 11) | golay23_parity(d)
    return ((cw << 1) | (bin(cw).count('1') & 1)) & 0xFFFFF


def qr16(d):
    r = d << 8
    for i in range(14, 7, -1):
        if r & (1 << i):
            r ^= 0x139 << (i - 8)
    cw = (d << 8) | (r & 0xFF)
    return (cw << 1) | (bin(cw).count('1') & 1)


def parity_code(m, k, eqs):
    cw = m
    for eq in eqs:
        p = 0
        for i in eq:
            p ^= (m >> (k - 1 - i)) & 1
        cw = (cw << 1) | p
    return cw


H15 = [[0, 1, 2, 3, 5, 7, 8], [1, 2, 3, 4, 6, 8, 9], [2, 3, 4, 5, 7, 9, 10], [0, 1, 2, 4, 6, 7, 10]]
H13 = [[0, 1, 3, 5, 6], [0, 1, 2, 4, 6, 7], [0, 1, 2, 3, 5, 7, 8], [0, 2, 4, 5, 8]]
H16 = [[0, 1, 2, 3, 5, 7, 8], [1, 2, 3, 4, 6, 8, 9], [2, 3, 4, 5, 7, 9, 10], [0, 1, 2, 4, 6, 7, 10], [0, 2, 5, 6, 8, 9, 10]]
H7 = [[0, 1, 2], [1, 2, 3], [0, 1, 3]]

# --- RS(12,9) over GF(256), 0x11D (the decoder's check, as an encoder) --------------
EXP = [0] * 512
LOG = [0] * 256
_x = 1
for _i in range(255):
    EXP[_i] = _x
    LOG[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= 0x11D
for _i in range(255, 512):
    EXP[_i] = EXP[_i - 255]


def gmul(a, b):
    return EXP[LOG[a] + LOG[b]] if a and b else 0


def rs129(d9):
    poly = [64, 56, 14]
    par = [0, 0, 0]
    for x in d9:
        db = x ^ par[2]
        par[2] = par[1] ^ gmul(poly[2], db)
        par[1] = par[0] ^ gmul(poly[1], db)
        par[0] = gmul(poly[0], db)
    return list(d9) + [par[2], par[1], par[0]]


def bptc_196(d96):
    d = [0] * 196
    for i in range(8):
        d[4 + i] = d96[i]
    n = 8
    for r in range(1, 9):
        for a in range(r * 15 + 1, r * 15 + 12):
            d[a] = d96[n]
            n += 1
    for r in range(9):
        p = r * 15 + 1
        cw = parity_code(val(d[p:p + 11]), 11, H15)
        d[p:p + 15] = bits(cw, 15)
    for c in range(15):
        col = [d[c + 1 + 15 * a] for a in range(9)]
        cw = bits(parity_code(val(col), 9, H13), 13)
        for a in range(13):
            d[c + 1 + 15 * a] = cw[a]
    raw = [0] * 196
    for a in range(196):
        raw[(a * 181) % 196] = d[a]
    return raw


def full_lc(flco, dst, src):
    return [flco & 0x3F, 0x00, 0x00, dst >> 16, (dst >> 8) & 0xFF, dst & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]


def lc_burst_info(lc9, dt):
    by = rs129(lc9)
    mask = 0x96 if dt == DT_HEADER else 0x99
    for i in range(9, 12):
        by[i] ^= mask
    return bptc_196(bytes_bits(by))


def embedded_lc(lc9):
    lc = bytes_bits(lc9)
    crc = sum(lc9) % 31
    d = [0] * 128
    d[0:11] = lc[0:11]
    d[16:27] = lc[11:22]
    n = 22
    for r in range(2, 7):
        d[16 * r:16 * r + 10] = lc[n:n + 10]
        n += 10
        d[16 * r + 10] = (crc >> (6 - r)) & 1
    for r in range(7):
        d[16 * r:16 * r + 16] = bits(parity_code(val(d[16 * r:16 * r + 11]), 11, H16), 16)
    for c in range(16):
        p = 0
        for r in range(7):
            p ^= d[c + 16 * r]
        d[112 + c] = p
    raw = [0] * 128
    b = 0
    for a in range(128):
        raw[a] = d[b]
        b += 16
        if b > 127:
            b -= 127
    return [raw[32 * i:32 * i + 32] for i in range(4)]


# --- AMBE+2 placeholder frame (72 bits, transmission order) ------------------------------
def golay23_mbelib(d12):
    ecc = 0
    for k in range(12):
        if (d12 >> (11 - k)) & 1:
            ecc ^= GOLAY_GEN[k]
    blk = (d12 << 11) | ecc
    return [(blk >> j) & 1 for j in range(23)]     # index j = in[j]


def ambe_frame(rng, mode):
    if mode == 'random':
        return list(rng.integers(0, 2, 72))
    fr = [[0] * 24 for _ in range(4)]
    u0 = int(rng.integers(0, 4096))
    u1 = int(rng.integers(0, 4096))
    g0 = golay23_mbelib(u0)
    for j in range(23):
        fr[0][j + 1] = g0[j]
    fr[0][0] = sum(g0) & 1
    g1 = golay23_mbelib(u1)
    pr = [16 * u0]
    for i in range(1, 24):
        pr.append((173 * pr[-1] + 13849) % 65536)
    pr = [p // 32768 for p in pr]
    k = 1
    for j in range(22, -1, -1):
        fr[1][j] = g1[j] ^ pr[k]
        k += 1
    for j in range(11):
        fr[2][j] = int(rng.integers(0, 2))
    for j in range(14):
        fr[3][j] = int(rng.integers(0, 2))
    out = [0] * 72
    for i in range(36):
        out[2 * i] = fr[rW[i]][rX[i]]
        out[2 * i + 1] = fr[rY[i]][rZ[i]]
    return out


def voice_bits(rng, mode):
    f = [ambe_frame(rng, mode) for _ in range(3)]
    return f[0] + f[1] + f[2]          # 216 bits; frame 2 straddles the sync / EMB field


# --- bursts (264 bits) ---------------------------------------------------------------
def data_burst(sync_kind, cc, dt, info196):
    st = bits(golay20((cc << 4) | dt), 20)
    return info196[:98] + st[:10] + bits(SYNC[(sync_kind, 'data')], 48) + st[10:] + info196[98:]


def voice_burst_a(sync_kind, v):
    return v[:108] + bits(SYNC[(sync_kind, 'voice')], 48) + v[108:]


def voice_burst_emb(cc, lcss, frag32, v, pi=0):
    e = bits(qr16((cc << 3) | (pi << 2) | lcss), 16)
    return v[:108] + e[:8] + frag32 + e[8:] + v[108:]


def cach(slot, rng):
    tact = bits(parity_code((1 << 3) | ((slot - 1) << 2) | 0, 4, H7), 7)   # AT busy, TC, LCSS 0
    c = list(rng.integers(0, 2, 24))
    for p, b in zip([0, 4, 8, 12, 14, 18, 22], tact):
        c[p] = b
    return c


def call_bursts(kind, cc, slot, src, dst, dur, voice, rng):
    """the bursts of one call on its timeslot: 2 headers, superframes, a terminator"""
    lc = full_lc(0x00, dst, src)
    frags = embedded_lc(lc)
    out = [data_burst(kind, cc, DT_HEADER, lc_burst_info(lc, DT_HEADER)) for _ in range(2)]
    nsf = max(1, int(round(dur / 0.36)))
    for _ in range(nsf):
        out.append(voice_burst_a(kind, voice_bits(rng, voice)))
        for k, lcss in enumerate([1, 3, 3, 2]):
            out.append(voice_burst_emb(cc, lcss, frags[k], voice_bits(rng, voice)))
        out.append(voice_burst_emb(cc, 0, [0] * 32, voice_bits(rng, voice)))
    out.append(data_burst(kind, cc, DT_TERM, lc_burst_info(lc, DT_TERM)))
    return out


def idle_burst(cc, rng):
    return data_burst('bs', cc, DT_IDLE, bptc_196(list(rng.integers(0, 2, 96))))


def to_syms(b):
    return [LEVEL[(b[2 * i] << 1) | b[2 * i + 1]] for i in range(len(b) // 2)]


# --- modulation ---------------------------------------------------------------------------
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


def fm(sym_track, env):
    """sym_track: symbol value per symbol slot (0 = no symbol); env: amplitude per symbol slot"""
    n = len(sym_track)
    up = np.zeros(n * SPS)
    up[::SPS] = sym_track
    h = rrc(SPS, 8, 0.2)
    rx = h / h.sum()                                # the receiver's RRC (unity DC gain)
    h = h / np.convolve(h, rx).max()                # levels exact after the receiver's matched filter
    f = np.convolve(up, h, 'same') * 648.0          # +-1 -> +-648 Hz, +-3 -> +-1944 Hz
    a = np.repeat(env, SPS)
    # ramps of a burst's edges (raised cosine, ~1.25 ms)
    k = np.ones(int(0.00125 * RATE)) / int(0.00125 * RATE)
    a = np.convolve(a, k, 'same')
    return a * np.exp(2j * np.pi * np.cumsum(f) / RATE)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, mode, cc, voice = RATE, 'bs', 1, 'ambe'
    calls, cur = [], None
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'mode':
            mode = v
        elif k == 'cc':
            cc = int(v)
        elif k == 'voice':
            voice = v
        elif k == 'at':
            cur = {'at': float(v), 'dur': 2.0, 'slot': 1, 'src': 1001, 'dst': 1}
            calls.append(cur)
        elif k == 'dur':
            cur[k] = float(v)
        else:
            cur[k] = int(v)
        i += 2
    rng = np.random.default_rng(31 + cc)
    slot_syms = 144                                  # a timeslot = 30 ms
    t_end = max(c['at'] + c['dur'] + 0.8 for c in calls) + 0.5
    nslots = int(t_end * 4800 / slot_syms) + 2
    track = np.zeros(nslots * slot_syms)
    env = np.zeros(nslots * slot_syms)
    busy = {}                                        # timeslot index -> burst bits
    for c in calls:
        kind = 'bs' if mode == 'bs' else ('ts1' if c['slot'] == 1 else 'ts2')
        bl = call_bursts(kind, cc, c['slot'], c['src'], c['dst'], c['dur'], voice, rng)
        # first slot index of this call's timeslot (slot 1 = even, 2 = odd)
        s0 = int(c['at'] * 4800 / slot_syms)
        if s0 % 2 != c['slot'] - 1:
            s0 += 1
        for k, b in enumerate(bl):
            busy[s0 + 2 * k] = (c['slot'], b)
        print('%8.3f .. %8.3f s  %s slot %d  %d -> TG %d  (%d bursts)' % (
            s0 * 0.03, (s0 + 2 * len(bl)) * 0.03, mode.upper(), c['slot'], c['src'], c['dst'], len(bl)))
    if mode == 'bs':
        first = min(busy) - int(0.2 / 0.03)
        last = max(busy) + int(0.3 / 0.03)
        for s in range(max(first, 0), min(last, nslots)):
            slot = s % 2 + 1
            b = busy[s][1] if s in busy else idle_burst(cc, rng)
            seq = cach(slot, rng) + b
            track[s * slot_syms:(s + 1) * slot_syms] = to_syms(seq)
            env[s * slot_syms:(s + 1) * slot_syms] = 1
    else:
        for s, (slot, b) in busy.items():
            # a 132-symbol burst centred in its 30 ms slot (6 symbols of guard each side)
            p = s * slot_syms + 6
            track[p:p + 132] = to_syms(b)
            env[p:p + 132] = 1
    x = fm(track, env)
    if abs(rate - RATE) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / RATE).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
