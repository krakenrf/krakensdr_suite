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

Three kinds of transmitter:
  --mode bs   a repeater (BS-sourced): continuous 30 ms slots, each a CACH
              (TACT: AT, TC = the timeslot, LCSS; Hamming(7,4)) + a burst;
              calls on timeslot 1 and 2 (may overlap), idle bursts between,
              BS syncs; keyed from 0.2 s before its first call to 0.3 s after
              its last. A Tier III PAYLOAD channel with --sys (the CACH then
              carries the P_SYS_Parms short LC) and per call --clear S (the
              TS clears the slot S seconds after the terminator: three
              P_CLEAR CSBKs to the call's talkgroup / unit, back to the TSCC). --key-slot N:
              only timeslot N's 30 ms are on the air - one carrier's two
              timeslots from two transmitters at their own bearings in a
              scene (the per-slot DoA path; a real repeater sends both)
  --mode tscc a Tier III trunked station control channel (TS 102 361-4) on
              timeslot 1 for --seconds: C_ALOHA, C_BCAST announcements (call
              timers, adjacent sites, Chan_Freq as an MBC with its BC_AP
              block), the grants given with --grant (each sent twice, then
              repeated once a second as a late-entry grant while its call
              lasts), the CACH carrying C_SYS_Parms; timeslot 2 idle or a
              composite payload channel (calls given with --at ... --slot 2)
  --mode dmo  a radio in TDMA direct mode: only its own timeslot (a 27.5 ms
              burst every 60 ms, ramped), the TS1 / TS2 direct-mode syncs

  dmr_gen.py OUT.cf32 [--rate 48000] [--mode bs|dmo|tscc] [--cc 1] [--voice ambe|random]
             [--sys MODEL.NET.SITE] [--seconds S] [--key-slot 1|2] [--seed N]
             --at T --slot 1|2 --dur S --src ID --dst TG [--unit 1] [--pi 1] [--emerg 1] [--clear S]
             [--at ... (next call)]
  tscc only: --grant T,KIND,CH,SLOT,DST,SRC[,DUR[,FLAGS]]   KIND tv|btv|pv|td|pd; CH = a logical
                 channel number, or LCN@RX_MHZ/TX_MHZ/CC for a grant with appended absolute
                 parameters (CG_AP); DUR = how long late-entry repeats follow (default 0);
                 FLAGS e = emergency, o = offset timing
             --chanfreq LCN,RX_MHZ,TX_MHZ   a Chan_Freq announcement (repeated every ~2 s)
             --adjacent SITE,LCN            an adjacent site announcement
             --move LCN,T                   C_MOVE (mask 0: every radio) to LCN from T s on

--seed N: the random generator's seed (voice bits, CACH, idle bursts);
without it 31 + the colour code, so the same options give the same file.

MODEL = tiny|small|large|huge. One transmitter as complex baseband (cf32) +
OUT.json. Put several at their own bearings with plugins/sdk/tools/uca_scene.py,
or anywhere in a 2.4 MHz scene with sdk/tools/wide_scene.py. The FEC encoders
follow the decoder's (plugins/dmr/dmr.cpp, dmr_trunk.cpp,
plugins/lib/dig_fec.cpp), which was validated on real DMR recordings; the
AMBE interleave / Golay tables are read from plugins/lib/mbe_tables.hpp
(mbelib / DSD, ISC). The Tier III PDUs follow ETSI TS 102 361-4 (layouts in
the decoder's comments), the CACH short LC / MBC framing TS 102 361-1.
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


def full_lc(flco, dst, src, so=0):
    return [flco & 0x3F, 0x00, so & 0xFF, dst >> 16, (dst >> 8) & 0xFF, dst & 0xFF, src >> 16, (src >> 8) & 0xFF, src & 0xFF]


# --- CSBK / MBC (TS 102 361-1 7.2 / 7.4, B.3.12) ----------------------------------------
DT_CSBK, DT_MBC_HDR, DT_MBC_CONT = 3, 4, 5


def crc_ccitt(data):
    """CRC-CCITT as the decoder checks it (crc16_ccitt_bytes: init 0, inverted)"""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc ^ 0xFFFF


def pack(fields):
    """[(value, nbits), ...] (MSB first, 80 bits) -> 10 bytes"""
    v, n = 0, 0
    for val, nb in fields:
        v = (v << nb) | (val & ((1 << nb) - 1))
        n += nb
    assert n == 80, n
    return [(v >> (72 - 8 * i)) & 0xFF for i in range(10)]


def block_info(b10, mask):
    """10 PDU bytes + their CRC (xor mask) -> the 196 BPTC bits of a data burst"""
    c = crc_ccitt(b10) ^ mask
    return bptc_196(bytes_bits(list(b10) + [c >> 8, c & 0xFF]))


def mbc_info(hdr10, conts):
    """an MBC: the header (CRC mask 0xAAAA) + continuation blocks; the last block's
    CRC covers every block after the header (intermediate: 12 bytes, last: 10), no mask"""
    out = [(DT_MBC_HDR, block_info(hdr10, 0xAAAA))]
    allb = []
    for c in conts[:-1]:
        allb += c                                    # 12 bytes each
        out.append((DT_MBC_CONT, bptc_196(bytes_bits(c))))
    last = conts[-1]
    c = crc_ccitt(allb + last)
    out.append((DT_MBC_CONT, bptc_196(bytes_bits(last + [c >> 8, c & 0xFF]))))
    return out


MODELS = {'tiny': 0, 'small': 1, 'large': 2, 'huge': 3}
SITE_BITS = [3, 5, 8, 10]


def syscode(spec, par=3):
    """MODEL.NET.SITE -> the 16-bit C_SYScode (TS 102 361-4 figure 6.15)"""
    m, net, site = spec.split('.')
    m = MODELS[m.lower()]
    sb = SITE_BITS[m]
    return (m << 14) | (int(net) << (2 + sb)) | (int(site) << 2) | par


def c_aloha(code, ms=0):
    # LB PF CSBKO FID | res TSCCAS sync version(3) offset active | mask(5) SF(2) NRand(4) Reg Backoff(4) | C_SYScode | MS
    return pack([(1, 1), (0, 1), (0x19, 6), (0, 8), (0, 1), (0, 1), (0, 1), (2, 3), (0, 1), (1, 1),
                 (0, 5), (0, 2), (7, 4), (0, 1), (6, 4), (code, 16), (ms, 24)])


def c_bcast(atype, p1, code, p2, lb=1):
    return pack([(lb, 1), (0, 1), (0x28, 6), (0, 8), (atype, 5), (p1, 14), (0, 1), (6, 4), (code, 16), (p2, 24)])


def ap_block(csbko, cc, chan, rx_hz, tx_hz, with_cc=True):
    """CG_AP / MV_AP / VN_AP (colour code) or BC_AP (none): CdefParms type 0"""
    def split(hz):
        mhz_i = int(hz // 1000000)
        return mhz_i, int(round((hz - mhz_i * 1000000) / 125.0))
    txm, txk = split(tx_hz)
    rxm, rxk = split(rx_hz)
    mid = [(0, 4), (cc, 4)] if with_cc else [(0, 8)]
    return pack([(1, 1), (0, 1), (csbko, 6)] + mid + [(0, 4), (0, 2), (chan, 12), (txm, 10), (txk, 13), (rxm, 10), (rxk, 13)])


GRANT_OP = {'pv': 0x30, 'tv': 0x31, 'btv': 0x32, 'pd': 0x33, 'td': 0x34}


def grant_pdu(kind, lpcn, ts, dst, src, late=0, emerg=0, offset=0, lb=1):
    op = GRANT_OP[kind]
    return pack([(lb, 1), (0, 1), (op, 6), (0, 8), (lpcn, 12), (ts - 1, 1), (late, 1), (emerg, 1), (offset, 1),
                 (dst, 24), (src, 24)])


def p_clear(lpcn, gi, dst, src):
    return pack([(1, 1), (0, 1), (0x2E, 6), (0, 8), (lpcn, 12), (0, 1), (0, 2), (gi, 1), (dst, 24), (src, 24)])


def c_move(lpcn, mask=0, ms=0):
    return pack([(1, 1), (0, 1), (0x39, 6), (0, 8), (0, 9), (mask, 5), (0, 5), (0, 1), (6, 4), (0, 4), (lpcn, 12), (ms, 24)])


ALLMSI, TSI = 0xFFFED4, 0xFFFECA


# --- CACH short LC (TS 102 361-1 7.1.4, B.2.3, B.3.3, B.3.7) -------------------------------
def crc8(v28):
    r = 0
    for i in range(27, -1, -1):
        fb = ((r >> 7) & 1) ^ ((v28 >> i) & 1)
        r = (r << 1) & 0xFF
        if fb:
            r ^= 0x07
    return r


def hamming17(m):
    r = m << 5
    for i in range(16, 4, -1):
        if r & (1 << i):
            r ^= 0x25 << (i - 5)
    return (m << 5) | (r & 0x1F)


def short_lc_frags(slco, data24):
    """a 28-bit short LC -> the 17 payload bits of its 4 CACH bursts"""
    v = (slco << 24) | data24
    rows = [v >> 16, (v >> 4) & 0xFFF, ((v & 0xF) << 8) | crc8(v)]
    m = [bits(hamming17(r), 17) for r in rows]
    m.append([m[0][c] ^ m[1][c] ^ m[2][c] for c in range(17)])
    flat = [0] * 68
    for k in range(68):
        flat[k] = m[k % 4][k // 4]
    return [flat[17 * i:17 * i + 17] for i in range(4)]


def sys_parms(code, payload):
    """C_SYS_Parms (SLCO 2) / P_SYS_Parms (3): MODEL(2) NET+SITE(12) Reg|composite(1) slot counter(9)"""
    d = ((code >> 14) << 22) | (((code >> 2) & 0xFFF) << 10) | (0 << 9) | 0
    return short_lc_frags(3 if payload else 2, d)


CACH_PAYLOAD = [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23]


def cach_lc(slot, lcss, frag17):
    tact = bits(parity_code((1 << 3) | ((slot - 1) << 2) | lcss, 4, H7), 7)
    c = [0] * 24
    for p, b in zip([0, 4, 8, 12, 14, 18, 22], tact):
        c[p] = b
    for p, b in zip(CACH_PAYLOAD, frag17):
        c[p] = b
    return c


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


def call_bursts(kind, cc, slot, src, dst, dur, voice, rng, unit=0, pi=0, emerg=0):
    """the bursts of one call on its timeslot: 2 headers, superframes, a terminator"""
    lc = full_lc(0x03 if unit else 0x00, dst, src, (0x40 if pi else 0) | (0x80 if emerg else 0))
    frags = embedded_lc(lc)
    out = [data_burst(kind, cc, DT_HEADER, lc_burst_info(lc, DT_HEADER)) for _ in range(2)]
    nsf = max(1, int(round(dur / 0.36)))
    for _ in range(nsf):
        out.append(voice_burst_a(kind, voice_bits(rng, voice)))
        for k, lcss in enumerate([1, 3, 3, 2]):
            out.append(voice_burst_emb(cc, lcss, frags[k], voice_bits(rng, voice), pi))
        out.append(voice_burst_emb(cc, 0, [0] * 32, voice_bits(rng, voice), pi))
    out.append(data_burst(kind, cc, DT_TERM, lc_burst_info(lc, DT_TERM)))
    return out


def idle_burst(cc, rng):
    return data_burst('bs', cc, DT_IDLE, bptc_196(list(rng.integers(0, 2, 96))))


def parse_grant(v):
    """T,KIND,CH,SLOT,DST,SRC[,DUR[,FLAGS]] (CH = LCN or LCN@RX_MHZ/TX_MHZ/CC)"""
    f = v.split(',')
    g = {'t': float(f[0]), 'kind': f[1].lower(), 'slot': int(f[3]), 'dst': int(f[4]), 'src': int(f[5]),
         'dur': float(f[6]) if len(f) > 6 else 0.0, 'flags': f[7] if len(f) > 7 else '', 'abs': None}
    ch = f[2]
    if '@' in ch:
        lcn, rest = ch.split('@')
        rx, tx, gcc = rest.split('/')
        g['lcn'] = int(lcn)
        g['abs'] = (float(rx) * 1e6, float(tx) * 1e6, int(gcc))
    else:
        g['lcn'] = int(ch)
    return g


def tscc_timeline(nslots, sysc, cc, grants, chanfreqs, adjacents, moves, calls):
    """the TSCC's timeslot-1 bursts: grants as they fall due, else the broadcast cycle"""
    out = {}

    def burst(dt, info):
        return data_burst('bs', cc, dt, info)

    def csbk(b10):
        return [burst(DT_CSBK, block_info(b10, 0xA5A5))]

    def mbc(hdr, conts):
        return [burst(dt, info) for dt, info in mbc_info(hdr, conts)]

    def grant_bursts(g, late):
        op = GRANT_OP[g['kind']]
        em, off = int('e' in g['flags']), int('o' in g['flags'])
        lt = late if g['kind'] in ('tv', 'btv') else 0
        if g['abs']:
            rx, tx, gcc = g['abs']
            hdr = grant_pdu(g['kind'], 0xFFF, g['slot'], g['dst'], g['src'], lt, em, off, lb=0)
            return mbc(hdr, [ap_block(op, gcc, g['lcn'], rx, tx)])
        return csbk(grant_pdu(g['kind'], g['lcn'], g['slot'], g['dst'], g['src'], lt, em, off))

    # (time, order, bursts): every grant twice back to back, then late entry once a second
    due = []
    for n, g in enumerate(grants):
        due.append((g['t'], n, grant_bursts(g, 0) + grant_bursts(g, 0)))
        t = g['t'] + 1.0
        while t < g['t'] + g['dur']:
            due.append((t, n, grant_bursts(g, 1)))
            t += 1.0
        print('%8.3f s  TSCC %s_GRANT %d -> %s %d on LCN %d slot %d%s' % (
            g['t'], g['kind'].upper(), g['src'], 'TG' if g['kind'] in ('tv', 'btv', 'td') else 'unit', g['dst'],
            g['lcn'], g['slot'], ' (absolute %.6f MHz)' % (g['abs'][0] / 1e6) if g['abs'] else ''))
    due.sort(key=lambda x: (x[0], x[1]))
    model = sysc >> 14
    sb = SITE_BITS[model]
    net = (sysc >> (2 + sb)) & ((1 << (12 - sb)) - 1)
    cycle = 0
    pending = []
    for s in range(0, nslots, 2):
        t = s * 0.03
        if not pending and due and due[0][0] <= t:
            pending = list(due.pop(0)[2])
        if not pending:
            k = cycle % 8
            cycle += 1
            active_moves = [m for m in moves if m[1] <= t]
            if k == 1:
                pending = csbk(c_bcast(1, (0x1FF << 5) | 31, sysc, (4095 << 12) | 4095))
            elif k == 3 and adjacents:
                site, lcn = adjacents[(cycle // 8) % len(adjacents)]
                nb = (model << 14) | (net << (2 + sb)) | (site << 2) | 3
                pending = csbk(c_bcast(6, nb >> 2, sysc, (1 << 23) | (1 << 22) | lcn))
            elif k == 5 and chanfreqs:
                lcn, rx, tx = chanfreqs[(cycle // 8) % len(chanfreqs)]
                pending = mbc(c_bcast(5, 0, sysc, lcn, lb=0), [ap_block(0x28, 0, lcn, rx, tx, with_cc=False)])
            elif k == 7 and active_moves:
                pending = csbk(c_move(active_moves[-1][0]))
            else:
                pending = csbk(c_aloha(sysc))
        out[s] = pending.pop(0)
    return out


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
    rate, mode, cc, voice, seed = RATE, 'bs', 1, 'ambe', None
    calls, cur = [], None
    sysc, seconds, key_slot = None, None, 0
    grants, chanfreqs, adjacents, moves = [], [], [], []
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
        elif k == 'sys':
            sysc = syscode(v)
        elif k == 'seconds':
            seconds = float(v)
        elif k == 'key-slot':
            key_slot = int(v)
        elif k == 'grant':
            grants.append(parse_grant(v))
        elif k == 'chanfreq':
            f = v.split(',')
            chanfreqs.append((int(f[0]), float(f[1]) * 1e6, float(f[2]) * 1e6))
        elif k == 'adjacent':
            f = v.split(',')
            adjacents.append((int(f[0]), int(f[1])))
        elif k == 'move':
            f = v.split(',')
            moves.append((int(f[0]), float(f[1])))
        elif k == 'seed':
            seed = int(v)
        elif k == 'at':
            cur = {'at': float(v), 'dur': 2.0, 'slot': 1, 'src': 1001, 'dst': 1, 'unit': 0, 'pi': 0, 'emerg': 0}
            calls.append(cur)
        elif k in ('dur', 'clear'):
            cur[k] = float(v)
        else:
            cur[k] = int(v)
        i += 2
    if mode == 'tscc' and sysc is None:
        sysc = syscode('small.1.1')
    rng = np.random.default_rng(31 + cc if seed is None else seed)
    slot_syms = 144                                  # a timeslot = 30 ms
    if calls:
        t_end = max(c['at'] + c['dur'] + 0.8 + c.get('clear', 0) for c in calls) + 0.5
    else:
        t_end = 0
    if seconds:
        t_end = max(t_end, seconds)
    nslots = int(t_end * 4800 / slot_syms) + 2
    if mode == 'tscc' and seconds:
        nslots = int(round(seconds * 4800 / slot_syms))
    track = np.zeros(nslots * slot_syms)
    env = np.zeros(nslots * slot_syms)
    busy = {}                                        # timeslot index -> burst bits
    for c in calls:
        kind = 'ts1' if c['slot'] == 1 else 'ts2'
        if mode in ('bs', 'tscc'):
            kind = 'bs'
        bl = call_bursts(kind, cc, c['slot'], c['src'], c['dst'], c['dur'], voice, rng, c['unit'], c['pi'], c['emerg'])
        # first slot index of this call's timeslot (slot 1 = even, 2 = odd)
        s0 = int(c['at'] * 4800 / slot_syms)
        if s0 % 2 != c['slot'] - 1:
            s0 += 1
        for k, b in enumerate(bl):
            busy[s0 + 2 * k] = (c['slot'], b)
        s1 = s0 + 2 * len(bl)
        if c.get('clear') is not None:
            # Tier III: the TS clears the payload channel after the hang time
            sc = s1 + 2 * int(round(c['clear'] / 0.06))
            # as a real Tier III network clears a voice call: addressed to its talkgroup / unit
            pc = data_burst('bs', cc, DT_CSBK, block_info(p_clear(0, 0 if c['unit'] else 1, c['dst'], TSI), 0xA5A5))
            for k in range(3):
                busy[sc + 2 * k] = (c['slot'], pc)
            print('%8.3f s  slot %d  P_CLEAR' % (sc * 0.03, c['slot']))
        print('%8.3f .. %8.3f s  %s slot %d  %d -> %s %d%s  (%d bursts)' % (
            s0 * 0.03, s1 * 0.03, mode.upper(), c['slot'], c['src'], 'unit' if c['unit'] else 'TG', c['dst'],
            ' [privacy]' if c['pi'] else '', len(bl)))
    if mode == 'tscc':
        tscc_slot1 = tscc_timeline(nslots, sysc, cc, grants, chanfreqs, adjacents, moves, calls)
        frags = sys_parms(sysc, False)
        for s in range(nslots):
            slot = s % 2 + 1
            if slot == 1:
                b = tscc_slot1.get(s) or data_burst('bs', cc, DT_CSBK, block_info(c_aloha(sysc), 0xA5A5))
            else:
                b = busy[s][1] if s in busy else idle_burst(cc, rng)
            k = s % 4
            seq = cach_lc(slot, [1, 3, 3, 2][k], frags[k]) + b
            track[s * slot_syms:(s + 1) * slot_syms] = to_syms(seq)
            env[s * slot_syms:(s + 1) * slot_syms] = 1
    elif mode == 'bs':
        first = min(busy) - int(0.2 / 0.03)
        last = max(busy) + int(0.3 / 0.03)
        if sysc is not None:
            first -= first % 4                       # whole short LCs from the first burst on
        frags = sys_parms(sysc, True) if sysc is not None else None
        for s in range(max(first, 0), min(last, nslots)):
            slot = s % 2 + 1
            b = busy[s][1] if s in busy else idle_burst(cc, rng)
            if frags:
                k = s % 4
                seq = cach_lc(slot, [1, 3, 3, 2][k], frags[k]) + b
            else:
                seq = cach(slot, rng) + b
            track[s * slot_syms:(s + 1) * slot_syms] = to_syms(seq)
            if key_slot and slot != key_slot:
                continue                             # that timeslot is another transmitter's
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
