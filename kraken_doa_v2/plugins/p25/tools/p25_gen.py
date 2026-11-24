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

  p25_gen.py OUT.cf32 [--rate 48000] [--seed N] --at T --dur S --src UNIT --dst TG [--nac 0x293]
             [--end tdu|tdulc] [--voice imbe|random] [--algid 0x80] [--kid 0]
             [--adj K] [--u2u] [--release S] [--at ... (next call)]
  p25_gen.py OUT.cf32 --control [--cc ID-NUM (1-1)] [--seconds 10] [--nac HEX] [--wacn HEX]
             [--sysid HEX] [--rfss N] [--site N] [--blocks 3|1]
             [--iden ID:BASE_HZ:SPACING_HZ:TXOFFSET_HZ[:SLOTS]] ... [--adj RFSS:SITE:ID-NUM] ...
             [--grant T:ID-NUM:TG:SRC[:DUR[:SERVICE_OPTIONS]]] ...
             [--ugrant T:ID-NUM:DST:SRC[:DUR]] ...

--u2u: a unit-to-unit call (HDU talkgroup 0, link control LCO 0x03 naming
--dst as the called unit). --release S: S seconds after the call's
terminator the system's clear-down, three TDULCs with the call termination
link control (LCO 0x0F).

--control: a trunked system's control channel instead, continuous for
--seconds: TSDUs of --blocks trunking blocks (3 = 75 ms frames, 1 = 37.5 ms),
the broadcasts cycling (NET_STS_BCST, RFSS_STS_BCST with --cc as the site's
control channel, a channel identifier per --iden - IDEN_UP_TDMA with SLOTS
2 / 4, IDEN_UP above 762 MHz, else IDEN_UP_VU -, ADJ_STS_BCST per --adj) and
the grants on their timeline: at T the group voice grant (GRP_V_CH_GRANT,
service options e.g. 0x40 = encrypted) / unit-to-unit grant
(UU_V_CH_GRANT), twice, then its update (GRP_V_CH_GRANT_UPDT, two calls
per block / UU_V_CH_GRANT_UPDT) every second for DUR s (default 4). Prints
every grant message sent ("t grant ..." / "t update ...").

--seed N: the random generator's seed (the voice bits); without it 25, so the
same options give the same file.

--algid / --kid: the call's algorithm and key in the HDU and every LDU2; an
ALGID other than 0x80 also sets the link control's "protected" service
option (an encrypted call - its voice frames are whatever --voice says, the
decoder must mute them). --adj K: LDU1 number 0, K, 2K, ... carries an
adjacent site status link control (LCO 0x22, implicit MFID: RFSS 1 site 2 of
system 0x1AE on channel 0x20B4) instead of the voice user's, as real systems
interleave them (K = 2: the first LDU1 after the HDU is one).

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


# --- NID: BCH(63,16) + parity bit -----------------------------------------------------
# systematic: the 47 parity bits are the remainder of m(x) x^47 divided by the
# generator polynomial (octal, TIA-102.BAAA) - as p25.cpp's nid_codeword()
NID_G = 0o6331141367235453


def nid_bits(nac, duid):
    m = (nac << 4) | duid
    r = m << 47
    for i in range(62, 46, -1):
        if r & (1 << i):
            r ^= NID_G << (i - 47)
    cw = bits((m << 47) | r, 63)
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


def lc_group(tg, src, so=0x00):
    return hexbits(bytes_bits([0x00, 0x00, so, 0x00, tg >> 8, tg & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]))


def lc_adjacent():
    # LCO 0x22 with the SF bit: the ADJ_STS_BCST body (LRA, sys, RFSS, site, channel, service class)
    return hexbits(bytes_bits([0x62, 0x00, 0x31, 0xAE, 0x01, 0x02, 0x20, 0xB4, 0x70]))


def es(algid=0x80, kid=0):
    return hexbits(bytes_bits([0] * 9 + [algid, kid >> 8, kid & 0xFF]))


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


def lc_unit(dst, src, so=0x00):
    # LCO 0x03 unit-to-unit voice channel user: service options, target, source
    return hexbits(bytes_bits([0x03, 0x00, so, dst >> 16, (dst >> 8) & 0xFF, dst & 0xFF, src >> 16, (src >> 8) & 0xFF,
                               src & 0xFF]))


def call(src, tg, nac, dur, end, voice, rng, algid=0x80, kid=0, adj=0, u2u=False):
    syms = hdu(nac, 0 if u2u else tg, algid, kid)
    n = max(2, int(round(dur / 0.18)))
    so = 0x40 if algid != 0x80 else 0x00
    for k in range(n):
        im = [imbe_frame(rng, voice) for _ in range(9)]
        if k % 2 == 0:
            lc = lc_adjacent() if adj and (k // 2) % adj == 0 else lc_unit(tg, src, so) if u2u else lc_group(tg, src, so)
            syms += ldu(nac, 0x5, lc, im)
        else:
            syms += ldu(nac, 0xA, es(algid, kid), im)
    syms += tdulc(nac, src) if end == 'tdulc' else tdu(nac)
    return syms


# --- control channel: TSDU frames of trunking signalling blocks (TSBK) ---------------
# A TSBK = 12 octets: LB (last block) | P | opcode (6), MFID, 8 argument octets,
# CRC-16 (CCITT x^16 + x^12 + x^5 + 1, initial 0, the remainder inverted) - as
# the decoder checks it (dig_fec crc16_ccitt_bytes). The 96 bits + a flush
# dibit go through the rate-1/2 trellis coder (TIA-102.BAAA: 4 states = the
# previous input dibit, each input dibit -> one of 16 constellation points =
# two output dibits) and the 98-dibit block interleaver. The TSDU = frame sync,
# NID (DUID 0x7), 1-3 blocks, padded with null dibits to a multiple of 36
# frame dibits (status symbols included): 1 block = 180 dibits (37.5 ms) and 3
# blocks = 360 (75 ms), the frame spacings of real single- and triple-block
# control channels (measured: 37.5 ms and 75 ms between syncs).
# The coder's tables are the decoder's (dig_fec p25_trellis_half), which
# decodes real control channels.
TRELLIS_ENC = [0, 15, 12, 3, 4, 11, 8, 7, 13, 2, 1, 14, 9, 6, 5, 10]
TRELLIS_PTS = [(+1, -1), (-1, -1), (+3, -3), (-3, -3), (-3, -1), (+3, -1), (-1, -3), (+1, -3),
               (-3, +3), (+3, +3), (-1, +1), (+1, +1), (+1, +3), (-1, +3), (+3, +1), (-3, +1)]
TRELLIS_ILV = [0, 1, 8, 9, 16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97,
               2, 3, 10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
               4, 5, 12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93,
               6, 7, 14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95]
DIBIT = {3: (0, 1), 1: (0, 0), -1: (1, 0), -3: (1, 1)}


def crc16_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc ^ 0xFFFF


def trellis_half(octets12):
    """12 octets -> 196 bits (98 dibits) in transmission order"""
    dib = []
    for b in octets12:
        dib += [(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3]
    dib.append(0)                       # flush
    t, s = [], 0
    for d in dib:
        t += TRELLIS_PTS[TRELLIS_ENC[s * 4 + d]]
        s = d
    out = []
    for i in range(98):
        out += DIBIT[t[TRELLIS_ILV[i]]]
    return out


def tsbk(op, args8, mfid=0x00, last=False):
    by = [(0x80 if last else 0) | (op & 0x3F), mfid] + list(args8)
    assert len(by) == 10
    c = crc16_ccitt(by)
    return by + [c >> 8, c & 0xFF]


def tsdu(nac, blocks):
    """blocks: 1-3 TSBKs of 12 octets (the last one's LB bit set by the caller)"""
    sf = head(nac, 0x7)
    for b in blocks:
        sf += trellis_half(b)
    n = len(sf) // 2
    raw = n + (n - 1) // 35
    raw = (raw + 35) // 36 * 36
    sf += [0] * (2 * (raw - raw // 36) - len(sf))
    return with_status(sf, raw)


def u64(v):
    return [(v >> (56 - 8 * i)) & 0xFF for i in range(8)]


def chan16(s):
    """'1-412' -> identifier << 12 | number"""
    i, n = s.split('-')
    return (int(i) << 12) | int(n)


class Iden:
    """a channel identifier: ID:BASE_HZ:SPACING_HZ:TXOFFSET_HZ[:SLOTS]"""

    def __init__(self, spec):
        p = spec.split(':')
        self.id, self.base, self.spacing, self.txoff = int(p[0]), float(p[1]), float(p[2]), float(p[3])
        self.slots = int(p[4]) if len(p) > 4 else 1

    def tsbk(self):
        base = int(round(self.base / 5))
        sp = int(round(self.spacing / 125))
        bw = 0x64 if self.spacing <= 12500 else 0xC8      # bandwidth, 125 Hz units (not read by receivers here)
        if self.slots > 1:     # IDEN_UP_TDMA: channel type 3 = TDMA 12.5 kHz 2 slots (4: 4 slots)
            ctype = {2: 3, 4: 4}[self.slots]
            off = int(round(abs(self.txoff) / self.spacing)) | (0x2000 if self.txoff > 0 else 0)
            v = (self.id << 60) | (ctype << 56) | (off << 42) | (sp << 32) | base
            return tsbk(0x33, u64(v))
        if self.base >= 762e6:  # IDEN_UP (800 / 900 MHz): offset sign + 8 bits x 250 kHz
            off = int(round(abs(self.txoff) / 250000)) | (0x100 if self.txoff > 0 else 0)
            v = (self.id << 60) | (bw << 51) | (off << 42) | (sp << 32) | base
            return tsbk(0x3D, u64(v))
        # IDEN_UP_VU: bandwidth (4), offset sign + 13 bits x the spacing
        off = int(round(abs(self.txoff) / self.spacing)) | (0x2000 if self.txoff > 0 else 0)
        v = (self.id << 60) | ((5 if self.spacing <= 6250 else 4) << 56) | (off << 42) | (sp << 32) | base
        return tsbk(0x34, u64(v))


def control_channel(o, rng):
    """o: the parsed --control options -> +-1 / +-3 symbols, o['seconds'] long"""
    nac, wacn, sysid, rfss, site, cc = o['nac'], o['wacn'], o['sysid'], o['rfss'], o['site'], o['cc']
    bcast = [tsbk(0x3B, u64((0x01 << 56) | (wacn << 36) | (sysid << 24) | (cc << 8) | 0x70)),          # NET_STS_BCST
             tsbk(0x3A, u64((0x01 << 56) | (sysid << 40) | (rfss << 32) | (site << 24) | (cc << 8) | 0x70))]  # RFSS_STS_BCST
    bcast += [i.tsbk() for i in o['iden']]
    for a in o['adj']:     # ADJ_STS_BCST: RFSS:SITE:CHANNEL of a neighbour
        r, s_, ch = a.split(':')
        bcast.append(tsbk(0x3C, u64((0x01 << 56) | (sysid << 40) | (int(r) << 32) | (int(s_) << 24) | (chan16(ch) << 8) | 0x70)))
    # grants: (at, until, first blocks, update body) - the grant twice, then
    # its update every second until the call and its hang time are over
    pend = []
    for g in o['grants']:
        f = g.split(':')
        kind, at, ch, tgt, src = f[0], float(f[1]), chan16(f[2]), int(f[3]), int(f[4])
        dur = float(f[5]) if len(f) > 5 and f[5] else 4.0
        so = int(f[6], 0) if len(f) > 6 and f[6] else 0
        if kind == 'g':     # GRP_V_CH_GRANT, then GRP_V_CH_GRANT_UPDT
            first = tsbk(0x00, u64((so << 56) | (ch << 40) | (tgt << 24) | src))
            upd = ('g', ch, tgt)
        else:               # UU_V_CH_GRANT, then UU_V_CH_GRANT_UPDT
            first = tsbk(0x04, u64((ch << 48) | (tgt << 24) | src))
            upd = ('u', ch, tgt, src)
        pend.append({'at': at, 'until': at + dur, 'first': first, 'n_first': 2, 'upd': upd, 'next_upd': at + 0.5})
    nblk = o['blocks']
    frame_s = (180 if nblk == 1 else 360) * 1.0 / 4800
    syms, t, bi = [], 0.0, 0
    log = []
    while t < o['seconds']:
        blocks = []
        for g in pend:   # due first grants go first
            if g['at'] <= t and g['n_first'] > 0 and len(blocks) < nblk:
                blocks.append(g['first'])
                g['n_first'] -= 1
                log.append('%7.3f grant %s' % (t, g['upd']))
        ups = [g for g in pend if g['n_first'] == 0 and g['next_upd'] <= t < g['until']]
        gu = [g for g in ups if g['upd'][0] == 'g']
        done = []
        while gu and len(blocks) < nblk:     # GRP_V_CH_GRANT_UPDT: two calls per block (one twice)
            a, b = gu[0]['upd'], (gu[1] if len(gu) > 1 else gu[0])['upd']
            blocks.append(tsbk(0x02, u64((a[1] << 48) | (a[2] << 32) | (b[1] << 16) | b[2])))
            done += gu[:2]
            gu = gu[2:]
        for g in [g for g in ups if g['upd'][0] == 'u']:
            if len(blocks) < nblk:
                u = g['upd']
                blocks.append(tsbk(0x06, u64((u[1] << 48) | (u[2] << 24) | u[3])))
                done.append(g)
        for g in done:   # the others come with the next frame
            g['next_upd'] = t + 1.0
            log.append('%7.3f update %s' % (t, g['upd']))
        while len(blocks) < nblk:
            blocks.append(bcast[bi % len(bcast)])
            bi += 1
        blocks[-1] = list(blocks[-1])
        blocks[-1][0] |= 0x80                       # last block
        c = crc16_ccitt(blocks[-1][:10])
        blocks[-1][10:12] = [c >> 8, c & 0xFF]
        syms += tsdu(nac, blocks)
        t += frame_s
    return syms, log


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


def control_main(out, a):
    o = {'rate': RATE, 'seconds': 10.0, 'nac': 0x293, 'wacn': 0xBEE00, 'sysid': 0x14C, 'rfss': 1, 'site': 1,
         'cc': None, 'iden': [], 'adj': [], 'grants': [], 'blocks': 3}
    i = 0
    while i < len(a):
        k = a[i].lstrip('-')
        if k == 'control':
            i += 1
            continue
        v = a[i + 1]
        if k in ('rate', 'seconds'):
            o[k] = float(v)
        elif k == 'iden':
            o['iden'].append(Iden(v))
        elif k == 'adj':
            o['adj'].append(v)
        elif k == 'grant':
            o['grants'].append('g:' + v)
        elif k == 'ugrant':
            o['grants'].append('u:' + v)
        elif k == 'cc':
            o['cc'] = chan16(v)
        elif k in ('wacn', 'sysid', 'nac'):
            o[k] = int(v, 16)
        else:
            o[k] = int(v, 0)
        i += 2
    if o['cc'] is None:
        o['cc'] = chan16('1-1')   # the site's own control channel, as RFSS_STS_BCST names it
    if o['blocks'] not in (1, 3):
        sys.exit('--blocks 1 or 3')
    syms, log = control_channel(o, np.random.default_rng(25))
    for line in log:   # every grant message sent (first grants, updates): the test's truth
        print(line)
    x = modulate(syms, o['rate'])
    n = int(round(o['seconds'] * o['rate']))
    # continuous carrier: the file starts at the first frame (modulate's
    # 10-symbol lead-in cut off) and is exactly --seconds long
    lead = int(round(10 * SPS * o['rate'] / RATE))
    x = x[lead:lead + n]
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': o['rate']}, open(out.rsplit('.', 1)[0] + '.json', 'w'))
    print('control channel: %.3f s, NAC 0x%03X WACN %05X system %03X RFSS %d site %d, CC %d-%d, %d grants' % (
        len(x) / o['rate'], o['nac'], o['wacn'], o['sysid'], o['rfss'], o['site'], o['cc'] >> 12, o['cc'] & 0xFFF,
        len(o['grants'])))


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    if '--control' in a[1:]:
        return control_main(out, a[1:])
    rate, seed = RATE, None
    txs, cur = [], None
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        if k == 'u2u':
            cur[k] = True
            i += 1
            continue
        v = a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'seed':
            seed = int(v, 0)
        elif k == 'at':
            cur = {'at': float(v), 'dur': 3.0, 'src': 1001, 'dst': 101, 'nac': 0x293, 'end': 'tdu', 'voice': 'imbe',
                   'algid': 0x80, 'kid': 0, 'adj': 0, 'u2u': False, 'release': 0.0}
            txs.append(cur)
        elif k in ('end', 'voice'):
            cur[k] = v
        elif k in ('dur', 'release'):
            cur[k] = float(v)
        else:
            cur[k] = int(v, 0)
        i += 2
    rng = np.random.default_rng(25 if seed is None else seed)
    parts = []
    for tx in txs:
        y = modulate(call(tx['src'], tx['dst'], tx['nac'], tx['dur'], tx['end'], tx['voice'], rng, tx['algid'], tx['kid'],
                          tx['adj'], tx['u2u']), rate)
        parts.append((tx['at'], y))
        print('%8.3f .. %8.3f s  unit %d -> %s %d  NAC 0x%03X  end %s' % (
            tx['at'], tx['at'] + len(y) / rate, tx['src'], 'unit' if tx['u2u'] else 'TG', tx['dst'], tx['nac'], tx['end']))
        if tx['release'] > 0:
            # the system's clear-down after the hang time: call termination
            # TDULCs (LCO 0x0F), three in a row
            t_rel = tx['at'] + len(y) / rate + tx['release']
            z = modulate(tdulc(tx['nac'], tx['src']) * 3, rate)
            parts.append((t_rel, z))
            print('%8.3f .. %8.3f s  release (TDULC call termination x3)' % (t_rel, t_rel + len(z) / rate))
    end = max(at + len(y) / rate for at, y in parts) + 0.3
    x = np.zeros(int(end * rate), np.complex128)
    for at, y in parts:
        n0 = int(round(at * rate))
        x[n0:n0 + len(y)] += y
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
