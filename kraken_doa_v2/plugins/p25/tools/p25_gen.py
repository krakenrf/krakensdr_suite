#!/usr/bin/env python3
"""P25 Phase 1 test transmissions (TIA-102.BAAA, C4FM 4800 symbols/s,
+-600 / +-1800 Hz): voice calls as a subscriber radio sends them - a header
data unit (HDU: MI, ALGID, KID, talkgroup; Golay(18,6) + RS(36,20,17)),
logical link data units LDU1 (link control: group voice channel user, LCO 0,
talkgroup + source unit ID; Hamming(10,6) + RS(24,12,13)) and LDU2
(encryption sync, ALGID 0x80 = clear; RS(24,16,9)) alternating, each with 9
IMBE voice frames, then a terminator: TDU (simple) or TDULC (link control
LCO 0x0F call termination, Golay(24,12) + RS(24,12,13)). Every frame with
its frame sync, NID (NAC + DUID, BCH(63,16) + parity) and a status symbol
after every 35 dibits.

  p25_gen.py OUT.cf32 [--rate 48000] --at T --dur S --src UNIT --dst TG [--nac 0x293]
             [--end tdu|tdulc] [--voice imbe|random] [--at ... (next call)]

The IMBE frames are placeholders with valid FEC (Golay(23,12) / Hamming(15,11)
+ the PN whitening seeded by u0, a fixed pitch, random spectral bits) - they
decode as a buzz, not speech; --voice random sends random bits instead.

C4FM: dibits -> +-1 / +-3 impulses -> raised cosine (alpha 0.2) x inverse
sinc shaping filter (it pre-compensates the receiver's integrate-and-dump
filter) -> FM, 600 Hz per level unit.

One radio's calls as complex baseband (cf32) + OUT.json. Put several radios
at their own bearings with plugins/sdk/tools/uca_scene.py. The FEC encoders
follow the decoder's (plugins/p25/p25.cpp, plugins/lib/dig_fec.cpp), which
was validated on real P25 recordings; the IMBE interleave / FEC tables are
read from plugins/lib/mbe_tables.hpp (mbelib / DSD, ISC).
"""
import json
import os
import re
import sys

import numpy as np

RATE = 48000.0
SPS = 10
SYNC = 0x5575F5FF77FF
LEVEL = {0b01: 3, 0b00: 1, 0b10: -1, 0b11: -3}
STATUS = (0, 1)          # status symbol "01" (busy, inbound)

# --- tables ------------------------------------------------------------------
TABLES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'lib', 'mbe_tables.hpp')


def table(name):
    src = open(TABLES).read()
    m = re.search(r'\b' + name + r'\[\d+\]\s*=\s*\{([^}]*)\}', src)
    return [int(v, 0) for v in m.group(1).replace('\n', ' ').split(',') if v.strip()]


iW, iX, iY, iZ = (table(n) for n in ('iW', 'iX', 'iY', 'iZ'))
GOLAY_GEN = table('golayGenerator')
HAMMING_GEN = table('hammingGenerator')


def bits(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def val(b):
    v = 0
    for x in b:
        v = (v << 1) | x
    return v


# --- NID: BCH(63,16) (the decoder's shift-register encoder) + parity bit ------------
NID_G = [1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1,
         1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1]


def nid_bits(nac, duid):
    m = (nac << 4) | duid
    data = [(m >> (15 - i)) & 1 for i in range(16)]
    bb = [0] * 47
    for i in range(15, -1, -1):
        fb = data[i] ^ bb[46]
        for j in range(46, 0, -1):
            bb[j] = (bb[j - 1] ^ fb) if (fb and NID_G[j]) else bb[j - 1]
        bb[0] = 1 if (NID_G[0] and fb) else 0
    cw = bits(m, 16) + bb
    return cw + [sum(cw) & 1]


# --- block codes (dig_fec.cpp layouts: first transmitted bit = MSB) ---------------
def golay23_parity(d):
    r = d << 11
    for i in range(22, 10, -1):
        if r & (1 << i):
            r ^= 0xC75 << (i - 11)
    return r & 0x7FF


def golay24(d):
    cw = (d << 11) | golay23_parity(d)
    return (cw << 1) | (bin(cw).count('1') & 1)


def golay18(d):
    return golay24(d) & 0x3FFFF


HAM10_EQ = [[0, 1, 2, 5], [0, 1, 3, 5], [0, 2, 3, 4], [1, 2, 3, 4]]


def hamming10(m):
    cw = m
    for eq in HAM10_EQ:
        p = 0
        for i in eq:
            p ^= (m >> (5 - i)) & 1
        cw = (cw << 1) | p
    return cw


# --- Reed-Solomon GF(64), x^6 + x + 1, fcr 1 (data first, parity last) -----------
GF_EXP = [0] * 128
GF_LOG = [0] * 64
_x = 1
for _i in range(63):
    GF_EXP[_i] = _x
    GF_LOG[_x] = _i
    _x <<= 1
    if _x & 0x40:
        _x ^= 0x43
for _i in range(63, 128):
    GF_EXP[_i] = GF_EXP[_i - 63]


def gmul(a, b):
    return GF_EXP[GF_LOG[a] + GF_LOG[b]] if a and b else 0


def rs64(data, nroots):
    gen = [0] * (nroots + 1)
    gen[0] = 1
    for i in range(1, nroots + 1):
        r = GF_EXP[i % 63]
        for j in range(i, 0, -1):
            gen[j] = gen[j - 1] ^ gmul(gen[j], r)
        gen[0] = gmul(gen[0], r)
    rem = [0] * nroots
    for s in data:
        fb = s ^ rem[0]
        for j in range(nroots - 1):
            rem[j] = rem[j + 1] ^ gmul(fb, gen[nroots - 1 - j])
        rem[nroots - 1] = gmul(fb, gen[0])
    return list(data) + rem


def hexbits(bitlist):
    return [val(bitlist[6 * i:6 * i + 6]) for i in range(len(bitlist) // 6)]


def bytes_bits(bs):
    out = []
    for b in bs:
        out += bits(b, 8)
    return out


# --- IMBE placeholder frame (144 bits in transmission order) -------------------------
def imbe_frame(rng, mode):
    if mode == 'random':
        return list(rng.integers(0, 2, 144))
    d = list(rng.integers(0, 2, 88))
    b0 = 100                          # pitch index (valid: <= 207 -> L = 32 harmonics)
    for i, k in enumerate([0, 1, 2, 3, 4, 5, 85, 86]):
        d[k] = (b0 >> (7 - i)) & 1
    u = [val(d[12 * i:12 * i + 12]) for i in range(4)] + [val(d[48 + 11 * i:59 + 11 * i]) for i in range(3)] + [val(d[81:88])]
    fr = [[0] * 23 for _ in range(8)]
    for i in range(4):               # Golay(23,12), mbelib: in[22..11] data, in[10..0] parity
        ecc = 0
        for k in range(12):
            if (u[i] >> (11 - k)) & 1:
                ecc ^= GOLAY_GEN[k]
        blk = (u[i] << 11) | ecc
        for j in range(23):
            fr[i][j] = (blk >> j) & 1
    for i in range(4, 7):            # Hamming(15,11): in[14..4] data, in[3..0] parity
        blk = u[i] << 4
        for k in range(4):
            blk |= (bin(blk & HAMMING_GEN[k] & 0x7FF0).count('1') & 1) << (3 - k)
        for j in range(15):
            fr[i][j] = (blk >> j) & 1
    for j in range(7):
        fr[7][j] = (u[7] >> j) & 1
    # PN whitening of c1..c6, seeded by u0
    pr = [16 * u[0]]
    for i in range(1, 115):
        pr.append((173 * pr[-1] + 13849) % 65536)
    pr = [p // 32768 for p in pr]
    k = 1
    for i in range(1, 4):
        for j in range(22, -1, -1):
            fr[i][j] ^= pr[k]
            k += 1
    for i in range(4, 7):
        for j in range(14, -1, -1):
            fr[i][j] ^= pr[k]
            k += 1
    out = [0] * 144
    for j in range(72):
        out[2 * j] = fr[iW[j]][iX[j]]
        out[2 * j + 1] = fr[iY[j]][iZ[j]]
    return out


# --- frames (status-free bits) -> dibits with status symbols ---------------------------
def with_status(sf, raw_dibits):
    dib = [(sf[2 * i], sf[2 * i + 1]) for i in range(len(sf) // 2)]
    out, k = [], 0
    for i in range(raw_dibits):
        if i % 36 == 35:
            out.append(STATUS)
        else:
            out.append(dib[k] if k < len(dib) else (0, 0))
            k += 1
    assert k == len(dib), (k, len(dib))
    return [LEVEL[(a << 1) | b] for a, b in out]


def head(nac, duid):
    return bits(SYNC, 48) + nid_bits(nac, duid)


def hdu(nac, tg, algid=0x80, kid=0, mfid=0):
    by = [0] * 9 + [mfid, algid, kid >> 8, kid & 0xFF, tg >> 8, tg & 0xFF]   # MI(72) MFID ALGID KID TGID
    cw = rs64(hexbits(bytes_bits(by)), 16)                                      # RS(36,20,17)
    b = []
    for h in cw:
        b += bits(golay18(h), 18)
    sf = head(nac, 0x0) + b + [0] * 10
    assert len(sf) == 770
    return with_status(sf, 396)


def ldu(nac, duid, payload72, imbe):
    """payload72: LC (LDU1, 12 hexbits -> RS(24,12)) or ES (LDU2, 16 hexbits -> RS(24,16))"""
    nroots = 12 if duid == 0x5 else 8
    cw = rs64(payload72, nroots)
    words = []
    for h in cw:
        words += bits(hamming10(h), 10)
    segs = [words[40 * i:40 * i + 40] for i in range(6)]
    sf = head(nac, duid)
    sf += imbe[0] + imbe[1]
    for i in range(6):
        sf += segs[i] if i < 5 else []
        if i < 5:
            sf += imbe[2 + i]
    # LC6, IMBE8, LSD, IMBE9
    sf += segs[5] + imbe[7] + [0] * 32 + imbe[8]
    assert len(sf) == 1680, len(sf)
    return with_status(sf, 864)


def lc_group(tg, src):
    return hexbits(bytes_bits([0x00, 0x00, 0x00, 0x00, tg >> 8, tg & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]))


def es_clear():
    return hexbits(bytes_bits([0] * 9 + [0x80, 0, 0]))


def tdu(nac):
    sf = head(nac, 0x3) + [0] * 28
    return with_status(sf, 72)


def tdulc(nac, src):
    lc = hexbits(bytes_bits([0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, src >> 16, (src >> 8) & 0xFF, src & 0xFF]))
    cw = rs64(lc, 12)                                                           # RS(24,12,13)
    b = []
    for i in range(12):
        b += bits(golay24((cw[2 * i] << 6) | cw[2 * i + 1]), 24)
    sf = head(nac, 0xF) + b + [0] * 20
    return with_status(sf, 216)


def call(src, tg, nac, dur, end, voice, rng):
    syms = hdu(nac, tg)
    n = max(2, int(round(dur / 0.18)))
    for k in range(n):
        im = [imbe_frame(rng, voice) for _ in range(9)]
        if k % 2 == 0:
            syms += ldu(nac, 0x5, lc_group(tg, src), im)
        else:
            syms += ldu(nac, 0xA, es_clear(), im)
    syms += tdulc(nac, src) if end == 'tdulc' else tdu(nac)
    return syms


# --- C4FM ---------------------------------------------------------------------------
def c4fm_filter():
    """raised cosine (alpha 0.2) x (pi f T) / sin(pi f T), 16 symbols long, scaled
    so that after the receiver's 1-symbol integrate-and-dump a level is exact"""
    nfft = 8192
    f = np.fft.fftfreq(nfft, 1.0 / RATE)
    T = 1 / 4800.0
    a = 0.2
    af = np.abs(f)
    H = np.where(af <= (1 - a) / (2 * T), 1.0,
                 np.where(af <= (1 + a) / (2 * T), 0.5 * (1 + np.cos(np.pi * T / a * (af - (1 - a) / (2 * T)))), 0.0))
    x = np.pi * f * T
    H = H * np.where(np.abs(x) < 1e-9, 1.0, x / np.where(np.abs(x) < 1e-9, 1.0, np.sin(x)))
    h = np.fft.fftshift(np.real(np.fft.ifft(H)))
    c = nfft // 2
    L = 8 * SPS
    h = h[c - L:c + L + 1] * np.hamming(2 * L + 1)
    g = np.convolve(h, np.ones(SPS) / SPS)
    return h / g.max()


def modulate(syms, rate):
    up = np.zeros(len(syms) * SPS + 20 * SPS)
    up[10 * SPS:10 * SPS + len(syms) * SPS:SPS] = syms
    f = np.convolve(up, c4fm_filter(), 'same') * 600.0      # +-1 -> +-600 Hz, +-3 -> +-1800 Hz
    x = np.exp(2j * np.pi * np.cumsum(f) / RATE)
    if abs(rate - RATE) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / RATE).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    return x


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate = RATE
    txs, cur = [], None
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'at':
            cur = {'at': float(v), 'dur': 3.0, 'src': 1001, 'dst': 101, 'nac': 0x293, 'end': 'tdu', 'voice': 'imbe'}
            txs.append(cur)
        elif k in ('end', 'voice'):
            cur[k] = v
        elif k == 'dur':
            cur[k] = float(v)
        else:
            cur[k] = int(v, 0)
        i += 2
    rng = np.random.default_rng(25)
    parts = []
    for tx in txs:
        y = modulate(call(tx['src'], tx['dst'], tx['nac'], tx['dur'], tx['end'], tx['voice'], rng), rate)
        parts.append((tx, y))
        print('%8.3f .. %8.3f s  unit %d -> TG %d  NAC 0x%03X  end %s' % (
            tx['at'], tx['at'] + len(y) / rate, tx['src'], tx['dst'], tx['nac'], tx['end']))
    end = max(tx['at'] + len(y) / rate for tx, y in parts) + 0.3
    x = np.zeros(int(end * rate), np.complex128)
    for tx, y in parts:
        n0 = int(round(tx['at'] * rate))
        x[n0:n0 + len(y)] += y
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
