#!/usr/bin/env python3
"""P25 Phase 2 (two-slot TDMA, TIA-102.BBAC-A) test transmissions, 6000
symbols/s, 30 ms timeslots, 12-slot superframes (360 ms), 4-superframe
ultraframes (1.44 s), two logical channels LCH 0 / LCH 1 per RF channel.

Two kinds of transmitter:
  --dir out  a base station's outbound channel: continuous H-DQPSK (pi/4
             differential QPSK, +-45 / +-135 deg per symbol, raised-cosine
             alpha 1 / 3.6 kHz shaping), every slot boundary an ISCH (I-ISCH
             (40,9,16) coset code before slots 0 1 4 5 8 9 with LCH type,
             channel, location, flag and ultraframe count; S-ISCH before
             2 3 6 7 10 11). Per LCH either a voice channel (VCH) - idle
             MAC_IDLE FACCH (S-OEMI) / SACCH (I-OEMI) or voice calls, in the
             order real Phase 2 sites send them: two MAC_ACTIVE (voice
             channel user), two MAC_PTT (offset 1, 0), the 4V 4V 4V 4V 2V
             cycle with ESS-B / ESS-A and a voice channel user message
             (MAC_ACTIVE) on the SACCH, one MAC_ACTIVE, MAC_HANGTIME for
             --hang s, three MAC_END_PTT (source 0xFFFFFF, the system), then
             idle again - or
             a TDMA control channel (--cch, OECI MAC_SIGNAL bursts cycling
             NET_STS, RFSS_STS, ADJ_STS, IDEN_UP, IDEN_UP_TDMA, SYNC_BCST,
             plus group voice grant updates while the other LCH has a call)
  --dir in   ONE subscriber radio's inbound bursts: H-CPM (h 1/3, L 4,
             4-level), 168 modulated symbols centred in the 30 ms slot with
             raised-cosine ramps over the 6 guard symbols each side, silence
             between bursts: IEMI (22-symbol sync) MAC_PTT x2, voice 4V / 2V
             with P1 / P2 pilots, the inbound SACCH (MAC_ACTIVE + VCU) in
             superframes 0-2 of each ultraframe, MAC_END_PTT x2

  p25p2_gen.py OUT.cf32 [--dir out|in] [--wacn HEX --sysid HEX --nac HEX]
       [--seconds S] [--seed N] [--slots FILE] [--grid-offset S]
       [--scramble on|off] [--voice-scramble on|off] [--in-advance 243]
       [--in-pn-offset slot|zero] [--voice ambe|silence|random]
       [--shaping nrz|impulse] [--hang S] [--cch LCH] [--chan HEX] [--rfss N] [--site N] [--lra HEX]
       [--lch N] --at T [--dur S] [--src UNIT] [--dst TG|UNIT] [--u2u]
       [--emergency] [--algid HEX --kid HEX] [--sf3-src UNIT]
       [--lch N --at T ... (next call)]
  p25p2_gen.py --selftest

Per-call options (--lch --at --dur --src --dst --u2u --emergency --algid
--kid --sf3-src) belong to one call; giving one of them again starts the
next call. --lch, --src and --dst carry over to the next call unless given.
Defaults: --lch 0 --dur 2.0 --src 1001 --dst 101 --algid 0x80 (clear) --kid 0.
  --at T        the call starts at the first suitable slot at or after T s
                (the LCH's next FACCH / non-SACCH slot)
  --dur S       voice length, rounded to whole 360 ms voice cycles (>= 1)
  --u2u         unit-to-unit call: --dst is the target unit (VCU opcode 0x02,
                MAC_PTT group address 0)
  --emergency   service options bit 0x80 in the VCU
  --algid/--kid ALGID != 0x80 = an encrypted call: MAC_PTT and ESS carry
                ALGID / KID / a random MI, service options 0x40, the voice
                frames are random (valid FEC) - a decoder must mute it
  --sf3-src U   (out) the unit named by the outbound SACCH VCU in superframe
                3 of each ultraframe (the inbound talker; default = --src) -
                a monitor takes the talker from superframes 0-2
Global options:
  --wacn/--sysid/--nac  network identity (default 0xBEE00 / 0x14C / 0x293):
                the scrambling seed WACN.SYSID.NAC (44 bits) and the NAC /
                colour code in MAC_END_PTT, NET_STS_BCST, the OECI trailer
  --seconds S   file length (default: the last call's end + hang + 0.5 s;
                3 s for an idle / control channel without calls)
  --seed N      random generator (MI, voice frames; default 25)
  --slots FILE  also write the bit-level slots, one line per timeslot:
                "<index> <O|I> <superframe_slot> <180 dibits '0'..'3'>",
                index = the 30 ms slot number counted from the grid's slot 0
                (= the file start without --grid-offset; superframe = index
                // 12, ultraframe count = superframe % 4), dibit value
                2 x bit1 + bit0 (+1 '0', +3 '1', -1 '2', -3 '3'), as
                transmitted (scrambled); outbound every whole slot in the file
                (symbols 0-9 / 170-179 = the ISCH halves), inbound only slots
                with a burst (guard / ramp symbols '0')
  --grid-offset S  slot 0 of the TDMA grid starts S s after the file start
                (default 0; rounded to a sample). Subscriber files built with
                the same grid can be mixed without colliding: LCH 0 inbound
                uses slots 1 3 5 7 9 (+ SACCH 10), LCH 1 0 2 4 6 8 (+ 11)
  --scramble on|off  FACCH / SACCH with the "with scrambling" DUIDs (%1001 /
                %0011, default on) or the unscrambled ones (%1111 / %1100)
  --voice-scramble on|off  scramble the 4V / 2V voice and ESS bits too
                (default on; INFERRED in the fact sheet, 8.4 / Q3)
  --in-advance N  inbound seed = outbound seed advanced N LFSR steps
                (default 243; SINGLE source, Q4)
  --in-pn-offset slot|zero  inbound burst in superframe slot s uses PN bits
                360 s ... (slot, default; INFERRED) or 0 ... (zero)
  --voice ambe|silence|random  clear-call voice frames: AMBE+2 frames with
                valid FEC, fixed pitch, random other parameters (ambe, a
                buzz), the silence code b0 = 124 (silence) or 72 raw random
                bits without valid FEC (random)
  --shaping impulse|nrz  (out) the RC alpha 1 / 3.6 kHz filter shapes one
                pulse per symbol (impulse, default: the spectrum of real sites,
                -6 dB at 3.6 kHz, -13 dB at 5 kHz, -23 dB at 6 kHz; levels
                ~+-783 / +-2280 Hz) or the phase-held NRZ signal (nrz, [KTH]'s
                equation: 6-10 dB steeper than every real site measured)
  --hang S      (out) MAC_HANGTIME between a call's last voice burst and its
                MAC_END_PTTs (default 1.0 s; real sites: 0.2 - 1.5 s). A call
                on the same LCH within it starts without the END_PTTs
  --cch LCH     (out) LCH 0 / 1 is a TDMA control channel (LCCH); give it
                twice for a dual LCCH (I-ISCH OS flag 1)
  --chan HEX    (out) this RF channel's channel field, identifier 4 bits +
                number 12 bits, even (default 0x10C8: identifier 1 = TDMA
                2-slot, 851.000 MHz + 12.5 kHz steps); LCH n = chan | n
  --rfss/--site/--lra  (out) site parameters for the broadcasts
                (default 1 / 1 / 0x01)
  --selftest    run the fact sheet's test vectors and exit (non-zero on any
                mismatch)

Output: complex float32 (cf32, I/Q interleaved) at 48 kHz, 8 samples per
symbol, centred on 0 Hz; outbound scaled to RMS 1, inbound constant envelope
1 in a burst + OUT.json ({"rate": 48000}). Put several subscriber radios at
their own bearings with plugins/sdk/tools/uca_scene.py.

Written from the clean-room P25 Phase 2 fact sheet (TIA-102.BBAC-A tables,
test vectors measured on a real recording) and this repo's conventions; no
third-party decoder's code. The AMBE+2 frame FEC (Golay(24,12) c0,
Golay(23,12) c1 whitened by the PN seeded from c0's data, c2 / c3 plain) and
the placement table are the 3600x2450 rate's, the same as DMR's.
"""
import json
import os
import re
import sys

import numpy as np

SYM_RATE = 6000
RATE = 48000
SPS = RATE // SYM_RATE                    # 8 samples per symbol
SLOT_SYMS = 180
SLOT_BITS = 360
SF_SLOTS = 12
UF_SF = 4
SLOT_SAMPLES = SLOT_SYMS * SPS            # 1440

# --- choices the fact sheet marks INFERRED / SINGLE / UNKNOWN (named here) -------
DEF_IN_ADVANCE = 243          # 8.2: inbound seed = outbound seed advanced 243 steps (SINGLE)
DEF_IN_PN_OFFSET = 'slot'     # 8.3: inbound slot s -> PN bits 360 s (INFERRED)
DEF_VOICE_SCRAMBLE = True     # 8.4: 4V / 2V voice + ESS bits scrambled (INFERRED)
# 1.2 / Q1: the RC filter on impulses, measured 2026-11-18 on three real sites'
# IQ (PA-STARNet, two Victoria MMR channels): averaged spectrum -1.5 / -3.7 /
# -5.4 / -7.7 / -10.0 / -13.0 / -22 dB at 2 / 3 / 3.6 / 4.05 / 4.5 / 5 / 6 kHz,
# the impulse model -1.5 / -3.7 / -5.9 / -7.6 / -10.1 / -13.1 / -23.4 dB; the
# NRZ reading of [KTH] (-3.1 / -7.5 / -11.7 / ... / -27 dB at 5 kHz) and an
# RRC 0.35 (-34 dB at 4.05 kHz) do not fit
DEF_SHAPING = 'impulse'
FR_IDLE = 1                   # I-ISCH FR flag on an idle VCH (as the real idle VCH, 8.5)
FR_CALL = 0                   # FR flag while a call holds the LCH (INFERRED: SACCH not free)
OFFSET_NA = 7                 # MAC header offset "not applicable" (CONFIRMED-RF)
N_PTT = 2                     # MAC_PTT bursts before the first 4V ([BBAC-A] Fig. 29)
N_END_PTT = 2                 # inbound: MAC_END_PTT bursts after the last voice burst (count INFERRED)
# Outbound call framing as measured on real sites (2026-11-18: PA-STARNet,
# Victoria MMR, three sigidwiki recordings - every call the same way):
# MAC_ACTIVE FACCHs (voice channel user) before the two MAC_PTTs (2-8 seen),
# one MAC_ACTIVE after the last voice burst, MAC_HANGTIME (0.2-1.5 s), then
# 3-6 MAC_END_PTT FACCH / SACCH bursts with source 0xFFFFFF (the system
# controller), then MAC_IDLE. The hang time's voice channel user names no
# source (0).
N_PRE_ACTIVE = 2              # outbound MAC_ACTIVE bursts before the MAC_PTTs
N_END_PTT_OUT = 3             # outbound MAC_END_PTT FACCH bursts after the hang time
END_PTT_SRC_OUT = 0xFFFFFF    # outbound MAC_END_PTT source address (measured)
U2U_PTT_GROUP = 0             # MAC_PTT / END_PTT group address on a unit-to-unit call (INFERRED)
RFSS_FLAGS = 0x3              # flags nibble before SYSID in RFSS_STS / ADJ_STS (as observed on [RF])
SYNC_FLAGS = 0x090000         # top 24 bits of SYNC_BCST's body (as observed on [RF])
SSC = 0x70                    # system service class (as observed)
CPM_PULSE_LEAD = 1.5          # H-CPM: symbol k's 4T pulse starts 1.5 T before its interval -> centred on it
AMBE_PITCH = 48               # b0 of the placeholder AMBE+2 frames (0..119 = voiced pitch)
AMBE_SILENCE_B0 = 124         # b0 = 124 / 125: AMBE+2 silence frame (mbelib's 3600x2450 parameter decoder)

# --- conventions ------------------------------------------------------------------
# dibit (bit1 bit0) -> symbol: 01 +3, 00 +1, 10 -1, 11 -3; dibit value = 2 bit1 + bit0
DIBIT_SYM = {0: 1, 1: 3, 2: -1, 3: -3}
SYM_DIBIT = {v: k for k, v in DIBIT_SYM.items()}
DQPSK_PHASE = {0: np.pi / 4, 1: 3 * np.pi / 4, 2: -np.pi / 4, 3: -3 * np.pi / 4}

# slot -> LCH (fact sheet 2.1); slots 10 / 11 are the inverted (SACCH) slots
OUT_LCH = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0]
IN_LCH = [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1]
OUT_SACCH = {0: 11, 1: 10}
IN_SACCH = {0: 10, 1: 11}
S_ISCH_BEFORE = {2, 3, 6, 7, 10, 11}

# sync words (symbols, first transmitted first; fact sheet 3)
S_ISCH = 0x575D57F7FF
S_OEMI_SYMS = [-3, -3, -3, -3, 3, 3, 3, 3, 3, -3, 3, -3, 3, -3, -3, 3, 3, -3, -3, 3, 3]
S_IECI_SYMS = [3, 3, -3, -3, 3, 3, 3, -3, 3, -3, -3, 3, 3, 3, 3, -3, 3, -3, -3, -3, -3, -3]
S_ISCH_SYMS = [3, 3, 3, -3, 3, 3, -3, 3, 3, 3, 3, -3, -3, -3, 3, -3, -3, -3, -3, -3]
P1 = [1, -1, 1, -1]
P2 = [-1, 1, -1, 1]

# DUID info values (fact sheet 5.3)
DUID_4V, DUID_2V = 0x0, 0x6
DUID_SACCH_S, DUID_SACCH = 0x3, 0xC
DUID_FACCH_S, DUID_FACCH = 0x9, 0xF
DUID_LCCH_S, DUID_LCCH = 0x4, 0xD

# MAC PDU opcodes (header bits 0-2)
MAC_SIGNAL, MAC_PTT, MAC_END_PTT, MAC_IDLE, MAC_ACTIVE, MAC_HANGTIME = 0, 1, 2, 3, 4, 6


def bits(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def val(b):
    v = 0
    for x in b:
        v = (v << 1) | int(x)
    return v


def bytes_bits(bs):
    out = []
    for b in bs:
        out += bits(b, 8)
    return out


def syms_bits(syms):
    out = []
    for s in syms:
        out += bits(SYM_DIBIT[s], 2)
    return out


def hexbits(b):
    return [val(b[6 * i:6 * i + 6]) for i in range(len(b) // 6)]


def unhex(hs):
    out = []
    for h in hs:
        out += bits(h, 6)
    return out


# --- I-ISCH (40,9,16) coset code, fact sheet 4.3 ------------------------------------
ISCH_G = [0x8816CE36D7, 0x201DFD4F64, 0x100F4B1758, 0x0C00DED18E, 0x020807F7FF,
          0x09048D9B72, 0x009DA3A171, 0x0058CBAA4E, 0x00343D8597]
ISCH_C0 = 0x184229D461


def i_isch(lch_type, ch, loc, flag, uf):
    info = (lch_type << 7) | (ch << 5) | (loc << 3) | (flag << 2) | uf
    c = ISCH_C0
    for r in range(9):
        if (info >> (8 - r)) & 1:
            c ^= ISCH_G[r]
    return c


# --- DUID (8,4,4), fact sheet 5.1 ------------------------------------------------------
DUID_G = [0x8D, 0x4B, 0x2E, 0x17]


def duid_cw(info):
    c = 0
    for r in range(4):
        if (info >> (3 - r)) & 1:
            c ^= DUID_G[r]
    return c


# --- GF(64) Reed-Solomon, x^6 + x + 1, mother code (63,35), roots a^1..a^28 -----------
GF_EXP = [0] * 126
GF_LOG = [0] * 64
_x = 1
for _i in range(63):
    GF_EXP[_i] = _x
    GF_LOG[_x] = _i
    _x <<= 1
    if _x & 0x40:
        _x ^= 0x43
for _i in range(63, 126):
    GF_EXP[_i] = GF_EXP[_i - 63]


def gmul(a, b):
    return GF_EXP[GF_LOG[a] + GF_LOG[b]] if a and b else 0


def rs_generator(nroots=28):
    """coefficients, highest degree first: prod (x - a^i), i = 1..nroots"""
    g = [1]
    for i in range(1, nroots + 1):
        r = GF_EXP[i % 63]
        ng = g + [0]
        for j in range(len(g)):
            ng[j + 1] ^= gmul(g[j], r)
        g = ng
    return g


RS_GEN = rs_generator()


def rs_parity(info, nsend):
    """the first nsend of the 28 parity hexbits (highest degree first) of a
    shortened / punctured (63,35) codeword with these info hexbits"""
    rem = [0] * 28
    for s in info:
        fb = s ^ rem[0]
        rem = rem[1:] + [0]
        if fb:
            for j in range(28):
                rem[j] ^= gmul(fb, RS_GEN[j + 1])
    return rem[:nsend]


# code: (info hexbits K', parity hexbits sent R')
RS_IECI, RS_OECI, RS_IEMI, RS_SOEMI, RS_ESS = (23, 23), (30, 22), (26, 20), (26, 19), (16, 28)


def rs_encode_bits(info_bits, code):
    k, r = code
    h = hexbits(info_bits)
    assert len(h) == k, (len(h), k)
    return unhex(rs_parity(h, r))


# --- CRCs (fact sheet 7.2) -------------------------------------------------------------
def crc_bits(b, width, poly, xorout):
    reg = 0
    top = 1 << (width - 1)
    mask = (1 << width) - 1
    for x in b:
        fb = (1 if reg & top else 0) ^ x
        reg = (reg << 1) & mask
        if fb:
            reg ^= poly
    return reg ^ xorout


def crc12(b):
    return crc_bits(b, 12, 0x897, 0xFFF)


def crc16(b):
    return crc_bits(b, 16, 0x1021, 0xFFFF)


# --- PN44 scrambler (fact sheet 8) -----------------------------------------------------
_PN_CACHE = {}


def pn_bits(seed, start, n):
    """b_start .. b_start+n-1 of the sequence whose first 44 bits are the seed (MSB first)"""
    seed &= (1 << 44) - 1
    if seed == 0:
        seed = (1 << 44) - 1
    key = (seed, start + n)
    if key not in _PN_CACHE:
        L = start + n
        b = np.zeros(max(L, 44) + 44, np.uint8)
        b[:44] = bits(seed, 44)
        for i in range(max(L - 44, 0)):
            b[i + 44] = b[i] ^ b[i + 10] ^ b[i + 24] ^ b[i + 29] ^ b[i + 35] ^ b[i + 40]
        _PN_CACHE[key] = b[:L]
    return _PN_CACHE[key][start:start + n]


def make_seed(wacn, sysid, nac):
    return ((wacn & 0xFFFFF) << 24) | ((sysid & 0xFFF) << 12) | (nac & 0xFFF)


# --- AMBE+2 3600x2450 frame (72 bits) ------------------------------------------------
# placement of the c0..c3 vector bits in a 36-dibit VF field (fact sheet 6.3):
# dibit k -> (vector, bit) of bit 1, (vector, bit) of bit 0
VF_MAP = [((0, 23), (0, 5)), ((1, 10), (2, 3)), ((0, 22), (0, 4)), ((1, 9), (2, 2)),
          ((0, 21), (0, 3)), ((1, 8), (2, 1)), ((0, 20), (0, 2)), ((1, 7), (2, 0)),
          ((0, 19), (0, 1)), ((1, 6), (3, 13)), ((0, 18), (0, 0)), ((1, 5), (3, 12)),
          ((0, 17), (1, 22)), ((1, 4), (3, 11)), ((0, 16), (1, 21)), ((1, 3), (3, 10)),
          ((0, 15), (1, 20)), ((1, 2), (3, 9)), ((0, 14), (1, 19)), ((1, 1), (3, 8)),
          ((0, 13), (1, 18)), ((1, 0), (3, 7)), ((0, 12), (1, 17)), ((2, 10), (3, 6)),
          ((0, 11), (1, 16)), ((2, 9), (3, 5)), ((0, 10), (1, 15)), ((2, 8), (3, 4)),
          ((0, 9), (1, 14)), ((2, 7), (3, 3)), ((0, 8), (1, 13)), ((2, 6), (3, 2)),
          ((0, 7), (1, 12)), ((2, 5), (3, 1)), ((0, 6), (1, 11)), ((2, 4), (3, 0))]


def golay23_parity(d):
    """Golay(23,12) parity of 12 data bits, generator x^11+x^10+x^6+x^5+x^4+x^2+1 (0xC75)"""
    r = d << 11
    for i in range(22, 10, -1):
        if r & (1 << i):
            r ^= 0xC75 << (i - 11)
    return r & 0x7FF


def ambe_pn(u0):
    """the 23 whitening bits of c1 (bit 22 first), seeded by c0's 12 data bits"""
    p = 16 * u0
    out = []
    for _ in range(23):
        p = (173 * p + 13849) % 65536
        out.append(p >> 15)
    return out


def ambe_vectors(u0, u1, u2, u3):
    """[c0 (24), c1 (23), c2 (11), c3 (14)] as lists indexed by bit number"""
    g0 = (u0 << 11) | golay23_parity(u0)
    c0 = [0] * 24
    for j in range(23):
        c0[j + 1] = (g0 >> j) & 1
    c0[0] = bin(g0).count('1') & 1
    g1 = (u1 << 11) | golay23_parity(u1)
    pn = ambe_pn(u0)
    c1 = [((g1 >> j) & 1) ^ pn[22 - j] for j in range(23)]
    c2 = [(u2 >> j) & 1 for j in range(11)]
    c3 = [(u3 >> j) & 1 for j in range(14)]
    return [c0, c1, c2, c3]


def place_vf(vec):
    out = []
    for (v1, b1), (v0, b0) in VF_MAP:
        out += [vec[v1][b1], vec[v0][b0]]
    return out


def ambe_frame(rng, mode, encrypted):
    """72 bits of a VF field in transmission order"""
    if mode == 'random' and not encrypted:
        return [int(x) for x in rng.integers(0, 2, 72)]
    u0, u1 = int(rng.integers(0, 4096)), int(rng.integers(0, 4096))
    u2, u3 = int(rng.integers(0, 2048)), int(rng.integers(0, 16384))
    if not encrypted:
        # b0 (pitch / frame class) = u0 bits 11..6 + u3 bit 0
        b0 = AMBE_SILENCE_B0 if mode == 'silence' else AMBE_PITCH
        u0 = ((b0 >> 1) << 6) | (u0 & 0x3F)
        u3 = (u3 & ~1) | (b0 & 1)
        if mode == 'silence':
            u0, u1, u2, u3 = u0 & ~0x3F, 0, 0, u3 & 1
    return place_vf(ambe_vectors(u0, u1, u2, u3))


# --- bursts: 360 bits + a mask of the scrambled positions ---------------------------------
class Burst:
    def __init__(self, kind, duid):
        self.kind = kind
        self.duid = duid
        self.b = [0] * SLOT_BITS
        self.m = [False] * SLOT_BITS
        self.cov = [0] * SLOT_SYMS

    def put(self, s0, s1, src, scr):
        n = 2 * (s1 - s0 + 1)
        assert len(src) == n, (self.kind, s0, s1, len(src), n)
        self.b[2 * s0:2 * s1 + 2] = [int(x) for x in src]
        for k in range(s0, s1 + 1):
            self.cov[k] += 1
        if scr:
            self.m[2 * s0:2 * s1 + 2] = [True] * n

    def put_duid(self, positions):
        cw = bits(duid_cw(self.duid), 8)
        for k, s in enumerate(positions):
            self.put(s, s, cw[2 * k:2 * k + 2], False)

    def scramble(self, pn, enable_voice):
        """XOR the PN window onto the info / parity (and voice) positions"""
        if not enable_voice and self.kind in ('4V', '2V'):
            return
        for j in range(SLOT_BITS):
            if self.m[j]:
                self.b[j] ^= int(pn[j])


OUT_DUID_SYMS = (10, 47, 132, 169)
IN_DUID_SYMS = (46, 83, 132, 169)


def out_oemi_i(duid, info180):
    """OECI (LCCH) and I-OEMI (SACCH): info X(179..0), parity P(131..0)"""
    par = rs_encode_bits(info180, RS_OECI)
    bu = Burst('OECI' if duid in (DUID_LCCH, DUID_LCCH_S) else 'I-OEMI', duid)
    bu.put_duid(OUT_DUID_SYMS)
    bu.put(11, 46, info180[0:72], True)
    bu.put(48, 101, info180[72:180], True)
    bu.put(102, 131, par[0:60], True)
    bu.put(133, 168, par[60:132], True)
    return bu


def out_oemi_s(duid, info156):
    """S-OEMI (FACCH): info X(155..0), sync, parity P(113..0)"""
    par = rs_encode_bits(info156, RS_SOEMI)
    sync = syms_bits(S_OEMI_SYMS)
    bu = Burst('S-OEMI', duid)
    bu.put_duid(OUT_DUID_SYMS)
    bu.put(11, 46, info156[0:72], True)
    bu.put(48, 78, info156[72:134], True)
    bu.put(79, 99, sync, False)
    bu.put(100, 110, info156[134:156], True)
    bu.put(111, 131, par[0:42], True)
    bu.put(133, 168, par[42:114], True)
    return bu


def out_4v(vf, ess_b24):
    bu = Burst('4V', DUID_4V)
    bu.put_duid(OUT_DUID_SYMS)
    bu.put(11, 46, vf[0], True)
    bu.put(48, 83, vf[1], True)
    bu.put(84, 95, ess_b24, True)
    bu.put(96, 131, vf[2], True)
    bu.put(133, 168, vf[3], True)
    return bu


def out_2v(vf, ess_a168):
    bu = Burst('2V', DUID_2V)
    bu.put_duid(OUT_DUID_SYMS)
    bu.put(11, 46, vf[0], True)
    bu.put(48, 83, vf[1], True)
    bu.put(84, 131, ess_a168[0:96], True)
    bu.put(133, 168, ess_a168[96:168], True)
    return bu


def in_frame(bu, sync_or_pilot):
    """inbound: guard symbols 0-5 / 174-179 stay '0', P2 at 170-173"""
    bu.put_duid(IN_DUID_SYMS)
    if sync_or_pilot == 'sync':
        bu.put(6, 27, syms_bits(S_IECI_SYMS), False)
    else:
        bu.put(6, 9, syms_bits(P1), False)
    bu.put(170, 173, syms_bits(P2), False)


def in_iemi(duid, info156):
    """IEMI (inbound FACCH / SACCH): info X(155..0), parity P(119..0)"""
    par = rs_encode_bits(info156, RS_IEMI)
    bu = Burst('IEMI', duid)
    in_frame(bu, 'sync')
    bu.put(28, 45, info156[0:36], True)
    bu.put(47, 82, info156[36:108], True)
    bu.put(84, 107, info156[108:156], True)
    bu.put(108, 131, par[0:48], True)
    bu.put(133, 168, par[48:120], True)
    return bu


def in_ieci(duid, info136):
    """IECI (inbound LCCH): info X(135..0) + 2 reserved bits, parity P(137..0)"""
    info = list(info136) + [0, 0]
    par = rs_encode_bits(info, RS_IECI)
    bu = Burst('IECI', duid)
    in_frame(bu, 'sync')
    bu.put(28, 45, info[0:36], True)
    bu.put(47, 82, info[36:108], True)
    bu.put(84, 97, info[108:136], True)
    bu.put(98, 98, info[136:138], True)
    bu.put(99, 131, par[0:66], True)
    bu.put(133, 168, par[66:138], True)
    return bu


def in_4v(vf, ess_b24):
    bu = Burst('4V', DUID_4V)
    in_frame(bu, 'pilot')
    bu.put(10, 45, vf[0], True)
    bu.put(47, 82, vf[1], True)
    bu.put(84, 95, ess_b24, True)
    bu.put(96, 131, vf[2], True)
    bu.put(133, 168, vf[3], True)
    return bu


def in_2v(vf, ess_a168):
    bu = Burst('2V', DUID_2V)
    in_frame(bu, 'pilot')
    bu.put(10, 45, vf[0], True)
    bu.put(47, 82, vf[1], True)
    bu.put(84, 131, ess_a168[0:96], True)
    bu.put(133, 168, ess_a168[96:168], True)
    return bu


# --- MAC PDUs -------------------------------------------------------------------------
def header(opcode, offset):
    return ((opcode & 7) << 5) | ((offset & 7) << 2)


def pad_null(octets, n):
    """messages, then Null Information (0x00 + zeros) up to n octets"""
    assert len(octets) <= n
    return list(octets) + [0] * (n - len(octets))


def pad_filler(octets, n):
    """messages, then Null Avoid Zero Bias (0x08, length, 0x88 ...) up to n octets"""
    r = n - len(octets)
    assert r == 0 or r >= 2, r
    return list(octets) + ([0x08, r] + [0x88] * (r - 2) if r else [])


def facch_info(octets18):
    """FACCH (S-OEMI / IEMI) and inbound SACCH: 144-bit PDU + CRC-12 = 156 bits"""
    b = bytes_bits(octets18)
    assert len(b) == 144
    return b + bits(crc12(b), 12)


def sacch_out_info(octets21):
    """outbound SACCH (I-OEMI): 168-bit PDU + CRC-12 = 180 bits"""
    b = bytes_bits(octets21)
    assert len(b) == 168
    return b + bits(crc12(b), 12)


def oeci_info(octets19, nac):
    """outbound LCCH: header + 18 octets of messages + 12-bit NAC + CRC-16 = 180 bits"""
    b = bytes_bits(octets19) + bits(nac, 12)
    assert len(b) == 164
    return b + bits(crc16(b), 16)


def so_byte(call):
    return (0x80 if call['emergency'] else 0) | (0x40 if call['algid'] != 0x80 else 0)


def vcu_msg(call, src=None):
    src = call['src'] if src is None else src
    if call['u2u']:
        t = call['dst']
        return [0x02, so_byte(call), t >> 16, (t >> 8) & 0xFF, t & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]
    g = call['dst']
    return [0x01, so_byte(call), g >> 8, g & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]


def group_of(call):
    return U2U_PTT_GROUP if call['u2u'] else call['dst'] & 0xFFFF


def mac_ptt(call, offset):
    mi = call['mi']
    o = [header(MAC_PTT, offset)] + [(mi >> (8 * (8 - i))) & 0xFF for i in range(9)]
    o += [call['algid'], call['kid'] >> 8, call['kid'] & 0xFF]
    s = call['src']
    g = group_of(call)
    o += [s >> 16, (s >> 8) & 0xFF, s & 0xFF, g >> 8, g & 0xFF]
    assert len(o) == 18
    return o


def mac_end_ptt(call, nac, src=None):
    s = call['src'] if src is None else src
    g = group_of(call)
    o = [header(MAC_END_PTT, OFFSET_NA), (nac >> 8) & 0x0F, nac & 0xFF] + [0] * 10
    o += [s >> 16, (s >> 8) & 0xFF, s & 0xFF, g >> 8, g & 0xFF]
    assert len(o) == 18
    return o


def ess_bits(call):
    """ESS info (ALGID KID MI, 16 hexbits) and RS(44,16,29) parity (28 hexbits)"""
    info = bits(call['algid'], 8) + bits(call['kid'], 16) + bits(call['mi'], 72)
    par = rs_encode_bits(info, RS_ESS)
    return info, par


# --- control channel broadcasts (abbreviated, partition 01) ------------------------------
def iden_tdma_body(ident, ctype, offset_hz, spacing_hz, base_hz):
    sp = spacing_hz // 125
    mag = abs(offset_hz) // spacing_hz
    a = (ident << 60) | (ctype << 56) | ((1 if offset_hz > 0 else 0) << 55) | ((mag & 0x1FFF) << 42)
    a |= (sp & 0x3FF) << 32
    a |= (base_hz // 5) & 0xFFFFFFFF
    return [(a >> (56 - 8 * i)) & 0xFF for i in range(8)]


def iden_body(ident, bw_hz, offset_hz, spacing_hz, base_hz):
    off = (abs(offset_hz) // 250000) & 0xFF
    a = (ident << 60) | (((bw_hz // 125) & 0x1FF) << 51) | ((0x100 if offset_hz > 0 else 0) | off) << 42
    a |= ((spacing_hz // 125) & 0x3FF) << 32
    a |= (base_hz // 5) & 0xFFFFFFFF
    return [(a >> (56 - 8 * i)) & 0xFF for i in range(8)]


def sync_fields_body(year, month, day, hours, minutes, micro):
    """SYNC_BCST body: SYNC_FLAGS (24) | year-2000 (7) | month (4) | day (5) | hours (5) |
    minutes (6) | micro-slots (13) - the field split checked against the real broadcast"""
    a = (SYNC_FLAGS << 40) | ((year % 100) << 33) | (month << 29) | (day << 24) | (hours << 19) | (minutes << 13) | micro
    return [(a >> (56 - 8 * i)) & 0xFF for i in range(8)]


def sync_bcst_body(t_slot):
    """2026-01-01 00:00 at the grid's slot 0 + the micro-slots (7.5 ms) to slot t_slot, so
    (minutes 8000 + micro-slots) mod 48 / 192 = 0 at a superframe / ultraframe start"""
    minutes, micro = divmod(4 * t_slot, 8000)       # 4 micro-slots per 30 ms timeslot
    hours, minutes = divmod(minutes, 60)
    return sync_fields_body(2026, 1, 1, hours % 24, minutes, micro)


class Site:
    def __init__(self, o):
        self.wacn, self.sysid, self.nac = o['wacn'], o['sysid'], o['nac']
        self.chan, self.rfss, self.site, self.lra = o['chan'], o['rfss'], o['site'], o['lra']
        self.base_hz, self.spacing_hz, self.offset_hz = 851000000, 12500, -45000000

    def broadcasts(self, n, k, grant):
        """the k-th LCCH burst's message (at slot n); grant = (lch, call) or None"""
        ch = self.chan
        if grant is not None and k % 2 == 1:
            lch, call = grant
            c = (self.chan & ~1) | lch
            g = call['dst'] & 0xFFFF
            return [0x42, c >> 8, c & 0xFF, g >> 8, g & 0xFF, c >> 8, c & 0xFF, g >> 8, g & 0xFF]
        k = k // 2 if grant is not None else k
        sel = k % 6
        if sel == 0:
            w, s = self.wacn, self.sysid
            return [0x7B, self.lra, w >> 12, (w >> 4) & 0xFF, ((w & 0xF) << 4) | (s >> 8), s & 0xFF,
                    ch >> 8, ch & 0xFF, SSC, (self.nac >> 8) & 0x0F, self.nac & 0xFF]
        if sel in (1, 2):
            rfss, site, c = (self.rfss, self.site, ch) if sel == 1 else (self.rfss, self.site + 1, ch + 0x20)
            return [0x7A if sel == 1 else 0x7C, self.lra, (RFSS_FLAGS << 4) | (self.sysid >> 8), self.sysid & 0xFF,
                    rfss & 0xFF, site & 0xFF, c >> 8, c & 0xFF, SSC]
        if sel == 3:
            return [0x7D] + iden_body(0, 12500, self.offset_hz, self.spacing_hz, self.base_hz)
        if sel == 4:
            return [0x73] + iden_tdma_body(ch >> 12, 3, self.offset_hz, self.spacing_hz, self.base_hz)
        return [0x70] + sync_bcst_body(n)


# --- call scheduling ----------------------------------------------------------------------
def slot_start_s(n, off):
    return (off + n * SLOT_SAMPLES) / RATE


def next_4v1_offset(n, ns_list, v1_slots):
    """MAC header offset of a burst at slot n: the LCH's non-SACCH bursts between it
    and the next voice cycle's first 4V (INFERRED meaning), 7 = none"""
    nxt = [v for v in v1_slots if v > n]
    if not nxt:
        return OFFSET_NA
    v = min(nxt)
    cnt = sum(1 for s in ns_list if n < s < v)
    return cnt if cnt < OFFSET_NA else OFFSET_NA


def schedule(calls, direction, off, n_first, n_last, hang_s):
    """per LCH: {slot: (what, call, info)} for the calls' bursts"""
    lch_tab = OUT_LCH if direction == 'out' else IN_LCH
    sacch_tab = OUT_SACCH if direction == 'out' else IN_SACCH
    plan = {0: {}, 1: {}}
    spans = {0: [], 1: []}
    for c in sorted(calls, key=lambda c: c['at']):
        L = c['lch']
        ns = [n for n in range(n_first, n_last + 1) if lch_tab[n % 12] == L and n % 12 != sacch_tab[L]]
        start = [n for n in ns if slot_start_s(n, off) >= c['at'] - 1e-9]
        ncyc = max(1, int(round(c['dur'] / 0.36)))
        out = direction == 'out'
        pre = N_PRE_ACTIVE if out else 0
        # outbound: MAC_ACTIVE x pre, PTT x2, voice, MAC_ACTIVE; the END_PTTs
        # follow the hang time. Inbound: PTT x2, voice, END_PTT x2
        need = pre + N_PTT + 5 * ncyc + (1 if out else N_END_PTT)
        if len(start) < need + (N_END_PTT_OUT if out else 0):
            sys.exit('call at %.3f s on LCH %d does not fit into the file (--seconds)' % (c['at'], L))
        i0 = ns.index(start[0])
        seq = ns[i0:i0 + need]
        if spans[L] and seq[0] <= spans[L][-1][1]:
            sys.exit('call at %.3f s on LCH %d overlaps the previous call on that LCH' % (c['at'], L))
        v1 = [seq[pre + N_PTT + 5 * k] for k in range(ncyc)]
        c['v1'] = v1
        c['ns'] = ns
        for k in range(pre):
            plan[L][seq[k]] = ('pre', c, None)
        for k in range(N_PTT):
            plan[L][seq[pre + k]] = ('ptt', c, N_PTT - 1 - k)
        for cy in range(ncyc):
            for p in range(5):
                plan[L][seq[pre + N_PTT + 5 * cy + p]] = ('voice', c, p)
        if out:
            plan[L][seq[-1]] = ('post', c, None)
        else:
            for k in range(N_END_PTT):
                plan[L][seq[N_PTT + 5 * ncyc + k]] = ('end', c, k)
        first, last = seq[0], seq[-1]
        c['first'], c['last'] = first, last
        # SACCH slots of the call
        for n in range(first, last + 1):
            if n % 12 == sacch_tab[L]:
                plan[L][n] = ('sacch', c, None)
        # outbound: hang time, then the END_PTTs (also on a SACCH among them)
        end_last = last
        if out:
            nh = int(round(max(0.0, hang_s) * 1000 / 30))
            hang_end = last + nh
            for n in range(last + 1, hang_end + 1):
                plan[L].setdefault(n, ('hang', c, None))
            ends = [n for n in ns if n > hang_end][:N_END_PTT_OUT]
            if ends:
                end_last = min(n_last, ends[-1])
                for n in range(hang_end + 1, end_last + 1):
                    if lch_tab[n % 12] == L or n % 12 == sacch_tab[L]:
                        plan[L].setdefault(n, ('end', c, None))
        spans[L].append((first, last, end_last, c))
    # a later call on the LCH cuts the previous call's hang time and END_PTTs
    for L in (0, 1):
        for i in range(len(spans[L]) - 1):
            nxt_first = spans[L][i + 1][0]
            for n in range(nxt_first, spans[L][i][2] + 1):
                if n in plan[L] and plan[L][n][0] in ('hang', 'end') and plan[L][n][1] is spans[L][i][3]:
                    del plan[L][n]
    return plan, spans


def voice_burst(direction, c, p, rng, voice_mode):
    enc = c['algid'] != 0x80
    info, par = c['ess']
    if p < 4:
        vf = [ambe_frame(rng, voice_mode, enc) for _ in range(4)]
        seg = info[24 * p:24 * p + 24]
        return (out_4v if direction == 'out' else in_4v)(vf, seg)
    vf = [ambe_frame(rng, voice_mode, enc) for _ in range(2)]
    return (out_2v if direction == 'out' else in_2v)(vf, par)


def outbound_slots(o, calls, cch, n_first, n_last, rng):
    """{slot n: (burst, isch_info_or_None)}; isch for the ISCH BEFORE slot n"""
    site = Site(o)
    seed = make_seed(o['wacn'], o['sysid'], o['nac'])
    scr = o['scramble']
    plan, spans = schedule(calls, 'out', o['off'], n_first, n_last, o['hang'])
    lcch_count = {0: 0, 1: 0}
    out = {}
    for n in range(n_first, n_last + 1):
        s = n % 12
        L = OUT_LCH[s]
        sacch = (s == OUT_SACCH[L])
        if L in cch:
            other = 1 - L
            grant = None
            for (f, la, he, c) in spans[other]:
                if other not in cch and f <= n <= la:
                    grant = (other, c)
            msg = site.broadcasts(n, lcch_count[L], grant)
            lcch_count[L] += 1
            pdu = [header(MAC_SIGNAL, OFFSET_NA)] + pad_filler(msg, 18)
            bu = out_oemi_i(DUID_LCCH, oeci_info(pdu, o['nac']))
            busy = True
        else:
            what, c, arg = plan[L].get(n, ('idle', None, None))
            busy = what in ('pre', 'ptt', 'voice', 'post', 'end', 'sacch')
            if what == 'voice':
                bu = voice_burst('out', c, arg, rng, o['voice'])
            elif sacch:
                if what == 'sacch':
                    uf_sf = (n // 12) % 4
                    src = c['sf3_src'] if (uf_sf == 3 and c['sf3_src'] is not None) else c['src']
                    pdu = [header(MAC_ACTIVE, next_4v1_offset(n, c['ns'], c['v1']))] + pad_null(vcu_msg(c, src), 20)
                elif what == 'hang':
                    pdu = [header(MAC_HANGTIME, OFFSET_NA)] + pad_null(vcu_msg(c, 0), 20)
                elif what == 'end':
                    pdu = mac_end_ptt(c, o['nac'], END_PTT_SRC_OUT) + [0] * 3
                else:
                    pdu = [header(MAC_IDLE, OFFSET_NA)] + [0] * 20
                bu = out_oemi_i(DUID_SACCH_S if scr else DUID_SACCH, sacch_out_info(pdu))
            else:
                if what in ('pre', 'post'):
                    pdu = [header(MAC_ACTIVE, OFFSET_NA)] + pad_null(vcu_msg(c), 17)
                elif what == 'ptt':
                    pdu = mac_ptt(c, arg)
                elif what == 'end':
                    pdu = mac_end_ptt(c, o['nac'], END_PTT_SRC_OUT)
                elif what == 'hang':
                    pdu = [header(MAC_HANGTIME, OFFSET_NA)] + pad_null(vcu_msg(c, 0), 17)
                else:
                    pdu = [header(MAC_IDLE, OFFSET_NA)] + [0] * 17
                bu = out_oemi_s(DUID_FACCH_S if scr else DUID_FACCH, facch_info(pdu))
        if scr and (bu.duid in (DUID_FACCH_S, DUID_SACCH_S) or (bu.kind in ('4V', '2V'))):
            bu.scramble(pn_bits(seed, 360 * s, SLOT_BITS), o['voice_scramble'])
        # ISCH before slot n (describes slot n)
        if s in S_ISCH_BEFORE:
            isch = S_ISCH
        else:
            if L in cch:
                lt, flag = 3, (1 if len(cch) == 2 else 0)
            else:
                lt, flag = 0, (FR_CALL if busy else FR_IDLE)
            isch = i_isch(lt, s % 4, s // 4, flag, (n // 12) % 4)
        out[n] = (bu, isch)
    return out


def inbound_slots(o, calls, n_first, n_last, rng):
    seed = make_seed(o['wacn'], o['sysid'], o['nac'])
    scr = o['scramble']
    plan, spans = schedule(calls, 'in', o['off'], n_first, n_last, 0)
    out = {}
    for L in (0, 1):
        for n, (what, c, arg) in plan[L].items():
            s = n % 12
            if what == 'sacch':
                if (n // 12) % 4 == 3:
                    continue                 # superframe 3: the talker listens to the outbound SACCH
                pdu = [header(MAC_ACTIVE, next_4v1_offset(n, c['ns'], c['v1']))] + pad_null(vcu_msg(c), 17)
                bu = in_iemi(DUID_SACCH_S if scr else DUID_SACCH, facch_info(pdu))
            elif what == 'ptt':
                bu = in_iemi(DUID_FACCH_S if scr else DUID_FACCH, facch_info(mac_ptt(c, arg)))
            elif what == 'end':
                bu = in_iemi(DUID_FACCH_S if scr else DUID_FACCH, facch_info(mac_end_ptt(c, o['nac'])))
            elif what == 'voice':
                bu = voice_burst('in', c, arg, rng, o['voice'])
            else:
                continue
            if scr and (bu.duid in (DUID_FACCH_S, DUID_SACCH_S) or bu.kind in ('4V', '2V')):
                start = o['in_advance'] + (360 * s if o['in_pn_offset'] == 'slot' else 0)
                bu.scramble(pn_bits(seed, start, SLOT_BITS), o['voice_scramble'])
            out[n] = bu
    return out


# --- modulators -------------------------------------------------------------------------
def rc_response(t):
    """raised cosine spectrum, alpha 1, -6 dB at 3.6 kHz, zero from 7.2 kHz (impulse response)"""
    fh = 7200.0
    x = 2 * fh * t
    with np.errstate(divide='ignore', invalid='ignore'):
        h = fh * np.sinc(x) / (1 - x * x)
    h[np.abs(np.abs(x) - 1) < 1e-9] = fh / 2
    return h


def hdqpsk(dibits, shaping):
    """pi/4-DQPSK; symbol k centred on sample 8k + 3.5.
    impulse: one RC pulse per symbol, centred on the symbol (default: real sites' spectrum)
    nrz:     the phase-held NRZ phasor through the RC filter ([KTH]'s equation)"""
    d = np.array([DQPSK_PHASE[v] for v in dibits])
    a = np.exp(1j * np.cumsum(d))
    if shaping == 'nrz':
        h = rc_response(np.arange(-96, 97) / RATE)
        return np.convolve(np.repeat(a, SPS), h / h.sum(), 'same')
    M = 97
    g = rc_response((np.arange(-M, M + 1) + 0.5) / RATE)
    g *= SPS / g.sum()
    imp = np.zeros(len(a) * SPS, complex)
    imp[np.arange(len(a)) * SPS + SPS // 2] = a      # impulse at 8k + 4, taps at half-sample offsets
    return np.convolve(imp, g)[M:M + len(imp)]


def cpm_pulse_increments():
    """q((m+1) T/8) - q(m T/8), m = 0..31: the H-CPM phase response per sample (sum 1/2)"""
    T = 1.0
    L = 4
    fine = 4000
    t = (np.arange(L * SPS * fine) + 0.5) / (SPS * fine)
    u = t - L * T / 2
    g = np.sinc(0.75 * u / T) * np.cos(np.pi * u / (L * T)) ** 2
    g = g / (g.sum() / (SPS * fine)) * 0.5          # integral = 1/2
    return g.reshape(L * SPS, fine).sum(axis=1) / (SPS * fine)


def cpm_q(kT):
    inc = cpm_pulse_increments()
    return float(np.sum(inc[:int(round(kT * SPS))]))


def ramp(n):
    k = np.arange(n) + 0.5
    return 0.5 - 0.5 * np.cos(np.pi * k / n)


# --- output -------------------------------------------------------------------------------
def slot_line(n, d, burst_bits):
    dib = ''.join(str(2 * burst_bits[2 * i] + burst_bits[2 * i + 1]) for i in range(SLOT_SYMS))
    return '%d %s %d %s' % (n, d, n % 12, dib)


def parse(argv):
    o = {'dir': 'out', 'wacn': 0xBEE00, 'sysid': 0x14C, 'nac': 0x293, 'seconds': None, 'seed': 25,
         'slots': None, 'grid_offset': 0.0, 'scramble': True, 'voice_scramble': DEF_VOICE_SCRAMBLE,
         'in_advance': DEF_IN_ADVANCE, 'in_pn_offset': DEF_IN_PN_OFFSET, 'shaping': DEF_SHAPING, 'voice': 'ambe', 'hang': 1.0,
         'cch': set(), 'chan': 0x10C8, 'rfss': 1, 'site': 1, 'lra': 0x01, 'out': None, 'selftest': False}
    per_call_val = {'lch', 'at', 'dur', 'src', 'dst', 'algid', 'kid', 'sf3-src'}
    per_call_flag = {'emergency', 'u2u'}
    sticky = ('lch', 'src', 'dst')
    calls = []
    cur = None

    def new_call(prev):
        c = {'lch': 0, 'at': None, 'dur': 2.0, 'src': 1001, 'dst': 101, 'algid': 0x80, 'kid': 0,
             'sf3_src': None, 'emergency': False, 'u2u': False, '_given': set()}
        if prev is not None:
            for k in sticky:
                c[k] = prev[k]
        calls.append(c)
        return c

    i = 0
    a = list(argv)
    while i < len(a):
        tok = a[i]
        if not tok.startswith('--'):
            if o['out'] is not None:
                sys.exit('unexpected argument ' + tok)
            o['out'] = tok
            i += 1
            continue
        k = tok[2:]
        if k in ('help', 'h'):
            print(__doc__)
            sys.exit(0)
        if k == 'selftest':
            o['selftest'] = True
            i += 1
            continue
        if k in per_call_flag or k in per_call_val:
            if cur is None or k in cur['_given']:
                cur = new_call(cur)
            cur['_given'].add(k)
            if k in per_call_flag:
                cur[k] = True
                i += 1
                continue
            v = a[i + 1]
            if k in ('at', 'dur'):
                cur[k] = float(v)
            elif k == 'sf3-src':
                cur['sf3_src'] = int(v, 0)
            elif k in ('algid', 'kid'):
                cur[k] = int(v, 16)
            else:
                cur[k] = int(v, 0)
            i += 2
            continue
        if i + 1 >= len(a):
            sys.exit('missing value for ' + tok)
        v = a[i + 1]
        i += 2
        if k == 'dir':
            if v not in ('out', 'in'):
                sys.exit('--dir out|in')
            o['dir'] = v
        elif k in ('wacn', 'sysid', 'nac', 'chan', 'lra'):
            o[k] = int(v, 16)
        elif k in ('seed', 'rfss', 'site', 'in-advance'):
            o[k.replace('-', '_')] = int(v, 0)
        elif k in ('seconds', 'grid-offset', 'hang'):
            o[k.replace('-', '_')] = float(v)
        elif k in ('scramble', 'voice-scramble'):
            if v not in ('on', 'off'):
                sys.exit('--%s on|off' % k)
            o[k.replace('-', '_')] = (v == 'on')
        elif k == 'in-pn-offset':
            if v not in ('slot', 'zero'):
                sys.exit('--in-pn-offset slot|zero')
            o['in_pn_offset'] = v
        elif k == 'shaping':
            if v not in ('nrz', 'impulse'):
                sys.exit('--shaping nrz|impulse')
            o['shaping'] = v
        elif k == 'voice':
            if v not in ('ambe', 'silence', 'random'):
                sys.exit('--voice ambe|silence|random')
            o['voice'] = v
        elif k == 'slots':
            o['slots'] = v
        elif k == 'cch':
            o['cch'].add(int(v))
        else:
            sys.exit('unknown option ' + tok)
    for c in calls:
        if c['at'] is None:
            sys.exit('a call needs --at')
        if c['lch'] not in (0, 1):
            sys.exit('--lch 0|1')
        if c['lch'] in o['cch']:
            sys.exit('LCH %d is a control channel (--cch): no calls on it' % c['lch'])
        del c['_given']
    if o['dir'] == 'in' and o['cch']:
        sys.exit('--cch is for --dir out')
    if o['chan'] & 1:
        sys.exit('--chan must be even (LCH 0)')
    return o, calls


def generate(o, calls):
    rng = np.random.default_rng(o['seed'])
    off = int(round(o['grid_offset'] * RATE))
    o['off'] = off
    for c in calls:
        c['mi'] = 0 if c['algid'] == 0x80 else int(rng.integers(0, 1 << 62)) << 10 | int(rng.integers(0, 1 << 10))
        c['ess'] = ess_bits(c)
    if o['seconds'] is None:
        if calls:
            end = 0.0
            for c in calls:
                ncyc = max(1, int(round(c['dur'] / 0.36)))
                if o['dir'] == 'out':
                    nb = N_PRE_ACTIVE + N_PTT + 5 * ncyc + 1 + N_END_PTT_OUT
                    end = max(end, c['at'] + 0.072 * nb + o['hang'] + 0.1)
                else:
                    end = max(end, c['at'] + 0.072 * (N_PTT + 5 * ncyc + N_END_PTT) + 0.1)
            o['seconds'] = end + 0.5
        else:
            o['seconds'] = 3.0
    nsamp = int(round(o['seconds'] * RATE))
    n_first = (-off) // SLOT_SAMPLES
    n_last = (nsamp - off) // SLOT_SAMPLES
    lines = []
    if o['dir'] == 'out':
        slots = outbound_slots(o, calls, o['cch'], n_first, n_last + 1, rng)
        stream = []
        for n in range(n_first, n_last + 1):
            bu, isch = slots[n]
            b = list(bu.b)
            b[0:20] = bits(isch & 0xFFFFF, 20)
            b[340:360] = bits(slots[n + 1][1] >> 20, 20)
            if 0 <= off + n * SLOT_SAMPLES and off + (n + 1) * SLOT_SAMPLES <= nsamp:
                lines.append(slot_line(n, 'O', b))
            stream += [2 * b[2 * i] + b[2 * i + 1] for i in range(SLOT_SYMS)]
        y = hdqpsk(stream, o['shaping'])
        y /= np.sqrt(np.mean(np.abs(y) ** 2))
        s0 = off + n_first * SLOT_SAMPLES
        x = np.zeros(nsamp, np.complex128)
        lo, hi = max(0, s0), min(nsamp, s0 + len(y))
        x[lo:hi] = y[lo - s0:hi - s0]
    else:
        slots = inbound_slots(o, calls, n_first, n_last, rng)
        ntot = nsamp + 4 * SLOT_SAMPLES
        imp = np.zeros(ntot + 64)
        env = np.zeros(ntot)
        base = 2 * SLOT_SAMPLES                       # sample index of file sample 0 in the work arrays
        up, dn = ramp(6 * SPS), ramp(6 * SPS)[::-1]
        lead = int(round(CPM_PULSE_LEAD * SPS))
        for n in sorted(slots):
            bu = slots[n]
            p0 = base + off + n * SLOT_SAMPLES
            if p0 < 0 or p0 + SLOT_SAMPLES > ntot:
                continue
            if 0 <= off + n * SLOT_SAMPLES and off + (n + 1) * SLOT_SAMPLES <= nsamp:
                lines.append(slot_line(n, 'I', bu.b))
            for k in range(6, 174):
                imp[p0 + k * SPS - lead] += DIBIT_SYM[2 * bu.b[2 * k] + bu.b[2 * k + 1]]
            env[p0:p0 + 6 * SPS] = up
            env[p0 + 6 * SPS:p0 + 174 * SPS] = 1.0
            env[p0 + 174 * SPS:p0 + 180 * SPS] = dn
        inc = cpm_pulse_increments()
        dphi = 2 * np.pi * (1.0 / 3.0) * np.convolve(imp, inc)[:ntot]
        phi = np.cumsum(dphi)
        y = env * np.exp(1j * phi)
        x = y[base:base + nsamp]
    lines.sort(key=lambda s: int(s.split(' ', 1)[0]))
    return x, lines


def describe(o, calls):
    for c in sorted(calls, key=lambda c: c['at']):
        t0 = slot_start_s(c['first'], o['off'])
        t1 = slot_start_s(c['last'] + 1, o['off'])
        kind = 'unit %d' % c['dst'] if c['u2u'] else 'TG %d' % c['dst']
        fl = []
        if c['algid'] != 0x80:
            fl.append('encrypted ALGID 0x%02X KID 0x%04X' % (c['algid'], c['kid']))
        if c['emergency']:
            fl.append('EMERGENCY')
        if c['sf3_src'] is not None and o['dir'] == 'out':
            fl.append('sf3 talker %d' % c['sf3_src'])
        print('%8.3f .. %8.3f s  %s LCH %d  %d -> %s  %d voice cycles%s' % (
            t0, t1, 'outbound' if o['dir'] == 'out' else 'inbound', c['lch'], c['src'], kind, len(c['v1']),
            ('  ' + ', '.join(fl)) if fl else ''))


# --- self test --------------------------------------------------------------------------
def hexstr_bits(h):
    out = []
    for ch in h:
        out += bits(int(ch, 16), 4)
    return out


def selftest():
    fails = []

    def check(name, ok, detail=''):
        print('%-4s %s%s' % ('ok' if ok else 'FAIL', name, ('  ' + detail) if detail and not ok else ''))
        if not ok:
            fails.append(name)

    # sync words
    check('S-ISCH sync 0x575D57F7FF', val(syms_bits(S_ISCH_SYMS)) == S_ISCH)
    check('S-OEMI sync 0x3FD55DDF5F5', val(syms_bits(S_OEMI_SYMS)) == 0x3FD55DDF5F5)
    check('IECI / IEMI sync 0x5F577D577FF', val(syms_bits(S_IECI_SYMS)) == 0x5F577D577FF)
    # I-ISCH table (4.4)
    tab = {
        (0, 0, 0): '184229D461 18761451F6 181AE27E2F 182EDFFBB8', (0, 0, 1): '1146A44F13 117299CA84 111E6FE55D 112A5260CA',
        (0, 0, 2): '1A4A2E239E 1A7E13A609 1A12E589D0 1A26D80C47', (0, 1, 0): '1442F705EF 1476CA8078 141A3CAFA1 142E012A36',
        (0, 1, 1): '1D467A9E9D 1D72471B0A 1D1EB134D3 1D2A8CB144', (0, 1, 2): '164AF0F210 167ECD7787 16123B585E 162606DDC9',
        (3, 0, 0): 'B0491AADD2 B07D272845 B011D1079C B025EC820B', (3, 0, 1): 'B94D9736A0 B979AAB337 B9155C9CEE B921611979',
        (3, 0, 2): 'B2411D5A2D B27520DFBA B219D6F063 B22DEB75F4', (3, 1, 0): 'BC49C47C5C BC7DF9F9CB BC110FD612 BC25325385',
        (3, 1, 1): 'B54D49E72E B5797462B9 B515824D60 B521BFC8F7', (3, 1, 2): 'BE41C38BA3 BE75FE0E34 BE190821ED BE2D35A47A'}
    bad = []
    for (lt, ch, loc), row in tab.items():
        for uf, h in enumerate(row.split()):
            if i_isch(lt, ch, loc, 0, uf) != int(h, 16):
                bad.append((lt, ch, loc, uf))
            if i_isch(lt, ch, loc, 1, uf) != int(h, 16) ^ 0x009DA3A171:
                bad.append((lt, ch, loc, uf, 'flag'))
    check('I-ISCH table 4.4 (72 words + flag)', not bad, str(bad[:4]))
    words = [i_isch(i >> 7, (i >> 5) & 3, (i >> 3) & 3, (i >> 2) & 1, i & 3) for i in range(512)]
    dmin = min(bin(words[a] ^ words[b]).count('1') for a in range(512) for b in range(a + 1, 512))
    check('I-ISCH minimum distance 16', dmin == 16, 'got %d' % dmin)
    # DUID
    dt = [0x00, 0x17, 0x2E, 0x39, 0x4B, 0x5C, 0x65, 0x72, 0x8D, 0x9A, 0xA3, 0xB4, 0xC6, 0xD1, 0xE8, 0xFF]
    check('DUID (8,4,4) codewords', [duid_cw(i) for i in range(16)] == dt)
    # RS generator and parity vectors (7.1, 14)
    gen_oct = '1 26 55 65 12 51 67 43 12 26 35 27 15 75 55 42 67 50 45 56 61 42 51 11 53 07 24 13 34'
    check('RS generator polynomial (eq. 5)', RS_GEN == [int(v, 8) for v in gen_oct.split()])
    vec = {RS_IECI: '56 22 47 27 13 51 33 17 37 42 20 18 41 16 62 25 12 56 8 1 58 26 0',
           RS_OECI: '35 32 51 51 53 10 24 44 17 48 18 21 61 48 14 15 30 51 54 14 38 9',
           RS_IEMI: '49 35 32 7 49 33 8 17 60 58 1 41 44 49 47 53 47 48 52 40',
           RS_SOEMI: '49 35 32 7 49 33 8 17 60 58 1 41 44 49 47 53 47 48 52',
           RS_ESS: '62 1 38 42 19 41 20 22 25 18 2 17 18 18 25 5 2 32 35 39 25 52 18 21 12 3 18 61'}
    names = {RS_IECI: 'IECI (46,23)', RS_OECI: 'OECI/I-OEMI (52,30)', RS_IEMI: 'IEMI (46,26)',
             RS_SOEMI: 'S-OEMI (45,26)', RS_ESS: 'ESS (44,16)'}
    for code, v in vec.items():
        k, r = code
        got = rs_parity(list(range(1, k + 1)), r)
        check('RS parity %s, info 1..K' % names[code], got == [int(x) for x in v.split()], str(got))
    one = [int(x) for x in '22 45 53 10 41 55 35 10 22 29 23 13 61 45 34 55 40 37 46 49 34 41 9 43 7 20 11 28'.split()]
    check('RS parity, info 0..0 1', rs_parity([0] * 15 + [1], 28) == one)
    check('ESS parity of a clear call (10.2)', rs_parity([32] + [0] * 15, 28) == [int(x) for x in
          '13 13 8 60 4 29 7 53 42 16 13 12 56 5 29 15 11 15 31 60 21 32 44 43 59 9 12 3'.split()])
    # CRCs (7.2, 14)
    fa = hexstr_bits('7C0000000000000000000000000000000000CA7')
    check('CRC-12 FACCH MAC_IDLE = 0xCA7', crc12(fa[:144]) == val(fa[144:]))
    sa = hexstr_bits('7C' + '0' * 40 + '131')
    check('CRC-12 SACCH MAC_IDLE = 0x131', len(sa) == 180 and crc12(sa[:168]) == 0x131)
    oeci = ['1C7B04927151F8825A70002C0807888888888802C1DDD', '1C7A0431F82C2C825A7008098888888888888802CED5A',
            '1C7C0431F83939828A7008098888888888888802C8813', '1C73833840640A250BC008098888888888888802C4964',
            '1C7D0322D0640A250BC008098888888888888802C4194', '1C700900002AF4201A3008098888888888888802CD83A',
            '1C8FA4070A26000A080B88888888888888888802CAF0A', '1C81A40C09110D830A83F2853608068888888802C01F5',
            '1CD6004000000000FFFF08098888888888888802C0BC9']
    bad = [h for h in oeci if crc16(hexstr_bits(h)[:164]) != val(hexstr_bits(h)[164:])]
    check('CRC-16 of the 9 real OECI fields', not bad, str(bad))
    # the generator's own OECI messages reproduce the real ones
    s = Site({'wacn': 0x92715, 'sysid': 0x1F8, 'nac': 0x02C, 'chan': 0x825A, 'rfss': 0x2C, 'site': 0x2C, 'lra': 0x04})
    s.base_hz, s.spacing_hz, s.offset_hz = 851000000, 12500, -45000000
    net = [0x1C] + pad_filler(s.broadcasts(0, 0, None), 18)
    rfss = [0x1C] + pad_filler(s.broadcasts(0, 1, None), 18)
    idu = [0x1C] + pad_filler(s.broadcasts(0, 3, None), 18)
    idt = [0x1C] + pad_filler([0x73] + iden_tdma_body(8, 3, -45000000, 12500, 851000000), 18)
    for name, pdu, ref in (('NET_STS_BCST', net, oeci[0]), ('RFSS_STS_BCST', rfss, oeci[1]),
                           ('IDEN_UP', idu, oeci[4]), ('IDEN_UP_TDMA', idt, oeci[3])):
        check('OECI %s field = the real one' % name, oeci_info(pdu, 0x02C) == hexstr_bits(ref))
    check('SYNC_BCST field split = the real 70 09 00 00 2A F4 20 1A 30',
          [0x70] + sync_fields_body(2021, 7, 20, 4, 0, 6704) == [0x70, 0x09, 0x00, 0x00, 0x2A, 0xF4, 0x20, 0x1A, 0x30])
    # PN44 (8.5)
    pv = [(0x92715, 0x1F8, 0x02C, ['927151F802C7EB85', '92E41FC92C7F2EF2', 'DEF49AFA6BA42898', 'E745B920C3E35510']),
          (0xBEE00, 0x14C, 0x293, ['BEE0014C293864CE', 'B9183C8F0DD094DB', '53772EC122BC36E4', '58A2CCAB6FE8265E']),
          (0x00001, 0x001, 0x001, ['0000100100132F45', 'B4F716681C799673', '6E3E9843CF3E912D', '518C8D69A4BA14E9'])]
    for w, sy, nc, hs in pv:
        sd = make_seed(w, sy, nc)
        got = ['%016X' % val(pn_bits(sd, st, 64)) for st in (0, 360, 3600, 4256)]
        check('PN44 seed 0x%011X' % sd, got == hs, str(got))
    check('PN44 zero seed -> all ones', '%016X' % val(pn_bits(0, 0, 64)) == 'FFFFFFFFFFF0F714')
    # H-CPM phase response (1.3)
    qs = [cpm_q(k) for k in (1, 2, 3, 4)]
    check('H-CPM q(T..4T) = 0.00542 0.25 0.49458 0.5', all(abs(a - b) < 2e-5 for a, b in
          zip(qs, (0.00542, 0.25, 0.49458, 0.5))), str(qs))
    # the two real bursts (8.5): idle VCH, WACN 0x92715 SYSID 0x1F8 NAC 0x02C
    seed = make_seed(0x92715, 0x1F8, 0x02C)
    rx1 = hexstr_bits('28B47A392C7F2EF245FBE0157C0B94E8B1A8908BFD55DDF5F55112FE104BBD0A889B5D87FE43563EAC142575D5')
    rx10 = hexstr_bits('7F7FF30A6BA42898E4C9030B123089C883457AD4373494FD7B1ABA0AEA0AC5F416856E96E2DE2FFA8C475575D5')
    f = out_oemi_s(DUID_FACCH_S, facch_info([header(MAC_IDLE, OFFSET_NA)] + [0] * 17))
    f.scramble(pn_bits(seed, 360 * 1, SLOT_BITS), True)
    sa = out_oemi_i(DUID_SACCH_S, sacch_out_info([header(MAC_IDLE, OFFSET_NA)] + [0] * 20))
    sa.scramble(pn_bits(seed, 360 * 10, SLOT_BITS), True)
    check('real slot-1 FACCH burst, symbols 10-169', f.b[20:340] == rx1[20:340],
          'diff %d bits' % sum(a != b for a, b in zip(f.b[20:340], rx1[20:340])))
    check('real slot-10 SACCH burst, symbols 10-169', sa.b[20:340] == rx10[20:340],
          'diff %d bits' % sum(a != b for a, b in zip(sa.b[20:340], rx10[20:340])))
    # ISCH halves: before slot 1 = I-ISCH VCH ch 1 loc 0 FR = FR_IDLE, ultraframe count 3;
    # before slot 2, 10, 11 = S-ISCH
    i1 = i_isch(0, 1, 0, FR_IDLE, 3)
    check('real slot-1 ISCH halves (I-ISCH uf 3 FR %d / S-ISCH)' % FR_IDLE,
          rx1[0:20] == bits(i1 & 0xFFFFF, 20) and rx1[340:] == bits(S_ISCH >> 20, 20))
    check('real slot-10 ISCH halves (S-ISCH / S-ISCH)',
          rx10[0:20] == bits(S_ISCH & 0xFFFFF, 20) and rx10[340:] == bits(S_ISCH >> 20, 20))
    # burst maps cover every symbol exactly once (outbound: + ISCH 0-9 / 170-179; inbound: + guard)
    z36, z24, z168 = [0] * 72, [0] * 24, [0] * 168
    maps = [('OECI', out_oemi_i(DUID_LCCH, [0] * 180), 'out'), ('S-OEMI', out_oemi_s(DUID_FACCH, [0] * 156), 'out'),
            ('out 4V', out_4v([z36] * 4, z24), 'out'), ('out 2V', out_2v([z36] * 2, z168), 'out'),
            ('IEMI', in_iemi(DUID_FACCH, [0] * 156), 'in'), ('IECI', in_ieci(DUID_LCCH, [0] * 136), 'in'),
            ('in 4V', in_4v([z36] * 4, z24), 'in'), ('in 2V', in_2v([z36] * 2, z168), 'in')]
    for name, bu, d in maps:
        cov = list(bu.cov)
        edge = list(range(10)) + list(range(170, 180)) if d == 'out' else list(range(6)) + list(range(174, 180))
        for k in edge:
            cov[k] += 1
        check('burst map %s covers symbols 0-179 once' % name, all(c == 1 for c in cov))
    # AMBE: the fact sheet's 6.3 table = the repo's DMR tables; Golay(23,12) = mbelib's generator
    tables = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'lib', 'mbe_tables.hpp')
    if os.path.exists(tables):
        src = open(tables).read()

        def table(name):
            m = re.search(r'\b' + name + r'\[\d+\]\s*=\s*\{([^}]*)\}', src)
            return [int(v, 0) for v in m.group(1).replace('\n', ' ').split(',') if v.strip()]
        rw, rx, ry, rz = (table(n) for n in ('rW', 'rX', 'rY', 'rZ'))
        check('VF placement (6.3) = repo rW/rX/rY/rZ', all(VF_MAP[k] == ((rw[k], rx[k]), (ry[k], rz[k])) for k in range(36)))
        gg = table('golayGenerator')
        ok = True
        for d in range(4096):
            e = 0
            for k in range(12):
                if (d >> (11 - k)) & 1:
                    e ^= gg[k]
            if e != golay23_parity(d):
                ok = False
                break
        check('Golay(23,12) parity = mbelib golayGenerator', ok)
    else:
        print('skip VF placement / Golay table comparison (lib/mbe_tables.hpp not found)')
    # full call round trip on the bit level: burst maps, FEC and scrambling
    o, calls = parse(['x', '--lch', '0', '--at', '0.1', '--dur', '0.7', '--lch', '1', '--at', '0.2', '--dur', '0.4',
                      '--algid', 'AA', '--kid', '1234', '--seconds', '3'])
    x, lines = generate(o, calls)
    check('outbound: one slot line per 30 ms slot', len(lines) == 100, str(len(lines)))
    print('%d test(s) failed' % len(fails) if fails else 'all tests passed')
    return 1 if fails else 0


def main():
    o, calls = parse(sys.argv[1:])
    if o['selftest']:
        sys.exit(selftest())
    if o['out'] is None:
        print(__doc__)
        sys.exit(2)
    x, lines = generate(o, calls)
    describe(o, calls)
    x.astype(np.complex64).tofile(o['out'])
    json.dump({'rate': RATE}, open(o['out'].rsplit('.', 1)[0] + '.json', 'w'))
    if o['slots']:
        with open(o['slots'], 'w') as fh:
            fh.write('\n'.join(lines) + '\n')
    print('%s: %.2f s, %s, WACN 0x%05X SYSID 0x%03X NAC 0x%03X%s' % (
        o['out'], len(x) / RATE, 'outbound' if o['dir'] == 'out' else 'inbound', o['wacn'], o['sysid'], o['nac'],
        (', LCCH on LCH ' + '+'.join(str(v) for v in sorted(o['cch']))) if o['cch'] else ''))


if __name__ == '__main__':
    main()
