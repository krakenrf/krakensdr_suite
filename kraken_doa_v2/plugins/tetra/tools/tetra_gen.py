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
               [--rate 72000] [--seed N] [--traffic K] --at T --dur S [--at T --dur S ...]

--seed N: the random generator's seed (the cell's TDMA time at the start and
its slot timing); without it the colour code, so the same options give the
same file.
--traffic K (K >= 1): a loaded cell - random SCH/F and AACH bits in every
burst (K seeds them, with the seed); without it every normal burst is the
same empty one and the output is unchanged.

The cell's carrier on during each [at, at + dur) (cf32 + OUT.json). Put
several cells at their own bearings with plugins/sdk/tools/uca_scene.py.

A TRUNKED SITE (one carrier of it per file, the same calls in every file):

  tetra_gen.py OUT.cf32 --seconds S --carrier N [--main M] [--mcc .. --mnc ..
               --cc .. --la ..] [--rate HZ] --call SPEC [--call SPEC ...]

  SPEC = comma-separated key=value: cid (call identifier), gssi (the group),
         carrier + tn (the assigned carrier / timeslot 1-4), um (its traffic
         usage marker, 4..63), at (s: the call set-up on the MCCH),
         tx=SSI@T0-T1[+SSI@T0-T1...] (the transmissions), enc=1 (end-to-end
         encrypted), release=T (D-RELEASE on the assigned slot's SCH/F after
         the hang time) or release=stolen (D-TX CEASED + D-RELEASE in the two
         stolen halves of the last traffic slot); for the decoder's
         call table tests: prio=P (the D-SETUP's call priority, 15 =
         emergency), nocalling=1 (D-SETUP without a calling party),
         noparty=K (the STCH D-TX GRANTEDs of transmission K on, 0-based,
         without a transmitting party)
  --truncate B   (a robustness test) every CMCE PDU loses its last B bits
                 inside a correctly coded block (CRC valid, MAC length right)

The main carrier (--main, default --carrier) has the MCCH on timeslot 1:
D-SETUP to the group with a channel allocation (+ a usage marker
assignment) and D-CONNECT to the caller at `at`, back-up D-SETUPs every 2 s
(the first one fragmented: MAC-RESOURCE + MAC-END with a long proprietary
element, in a BL-UDATA with FCS), and a D-TX GRANTED with a channel
allocation to each later transmitting party 0.1 s before its transmission.
On the assigned carrier / timeslot: per transmission an STCH D-TX GRANTED
(the transmitting party) in its first slot and every 2 s, TCH/S (random
speech frames) with the AACH's traffic usage marker, an STCH D-TX CEASED in
its last slot; between transmissions the hang time (AACH assigned control);
then the release, and the slot unallocated. Every carrier has the cell's
BSCH / BNCH (SYSINFO naming the main carrier) in frame 18; all carriers of
the site share the TDMA timing (slot 0 at sample 0). Band 3 (300 MHz base),
offset 0, duplex spacing code 0 (10 MHz): carrier 3600 = 390.0000 MHz.
"""
import json
import sys

import numpy as np

SPS = 4
FS = 72000.0
Y = [1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1]
N = [1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0]
P = [0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0]
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
    b = u(2, 2) + u(0, 2) + u(cell.get('main', cell['carrier']), 12) + u(3, 4) + u(0, 2) + u(0, 3) + [0] + u(0, 2)
    b += u(3, 3)
    b += u(0, 4) + u(0, 4) + u(0, 4) + [0] + u(0, 16)
    b += [0] * (124 - 42 - len(b))
    b += u(cell['la'], 14) + u(0xFFFF, 16) + u(0b100000101000, 12)
    return b


def slot_bits(cell, tn, fn, mn, rnd=None):
    scr = scramb_init(cell['mcc'], cell['mnc'], cell['cc'])
    # (rnd: --traffic - every normal burst's SCH/F block and every AACH carry random
    # bits, as a loaded cell's signalling / traffic does; without it the empty
    # MAC-RESOURCE and an all-zero AACH, the same burst every frame)
    if fn == 18:
        # synchronization continuous downlink burst
        b = [0] * 14 + [1] * 8 + [0] * 64 + [1] * 8
        b += block(bsch(cell, tn, fn, mn), 120, 11, 3)
        b += Y
        b += aach(0 if rnd is None else int(rnd.integers(0, 1 << 14)), scr)
        b += block(sysinfo(cell), 216, 101, scr)
        b += [0] * 12
    else:
        # normal continuous downlink burst: SCH/F (empty MAC-RESOURCE)
        info = [0] * 268 if rnd is None else [int(v) for v in rnd.integers(0, 2, 268)]
        blk = block(info, 432, 103, scr)
        bb = aach(0 if rnd is None else int(rnd.integers(0, 1 << 14)), scr)
        b = [0] * 14 + blk[:216] + bb[:14] + N + bb[14:] + blk[216:] + [0] * 12
    assert len(b) == 510, len(b)
    return b


# --- a trunked site (EN 300 392-2 clauses 14, 21; --call) -------------------

TRUNCATE = 0   # --truncate


def fcs32(bits):
    """basic link FCS (EN 300 392-2 annex C): CRC-32 G(x) 0x04C11DB7, first 32
    bits complemented (the register starts at all ones), remainder complemented"""
    crc = 0xFFFFFFFF
    for b in bits:
        fb = ((crc >> 31) ^ b) & 1
        crc = (crc << 1) & 0xFFFFFFFF
        if fb:
            crc ^= 0x04C11DB7
    return u(crc ^ 0xFFFFFFFF, 32)


def llc_mle_cmce(cmce, fcs=False):
    """TM-SDU: BL-UDATA (BL-UDATA with FCS if asked) -> MLE protocol discriminator CMCE (2)"""
    tl = u(2, 3) + (cmce[:len(cmce) - TRUNCATE] if TRUNCATE else cmce)
    return (u(6, 4) + tl + fcs32(tl)) if fcs else (u(2, 4) + tl)


def bsi(enc):
    # basic service information: speech TCH/S (000), end-to-end encryption
    # flag, point-to-multipoint (01), TETRA encoded speech (00)
    return (enc & 1) << 4 | 1 << 2


def party(ssi):
    # P-bit, party type identifier SSI (01), the SSI
    return [1] + u(1, 2) + u(ssi, 24)


def d_setup(cid, enc, calling, proprietary=0, prio=0):
    # call time-out infinite, no hook signalling, simplex, the BSI, transmission
    # granted to another user, the call priority (15 = emergency)
    b = u(7, 5) + u(cid, 14) + u(0, 4) + [0] + [0] + u(bsi(enc), 8) + u(3, 2) + [0] + u(prio, 4)
    # O-bit, notification, temporary address, calling party (None: not given)
    b += [1] + [0] + [0] + (party(calling) if calling is not None else [0])
    if proprietary:
        b += [1] + u(15, 4) + u(proprietary, 11) + [i & 1 for i in range(proprietary)]
    return b + [0]                                  # the last M-bit


def d_connect(cid):
    # call time-out 5 min, no hook signalling, simplex, transmission granted,
    # request permission, a call owner; no optional elements
    return u(2, 5) + u(cid, 14) + u(7, 4) + [0] + [0] + u(0, 2) + [0] + [1] + [0]


def d_tx_granted(cid, grant, enc, src):
    b = u(11, 5) + u(cid, 14) + u(grant, 2) + [0] + [enc & 1] + [0]
    return b + [1] + [0] + (party(src) if src is not None else [0]) + [0]   # src None: no transmitting party


def d_tx_ceased(cid):
    return u(9, 5) + u(cid, 14) + [0] + [0]


def d_release(cid):
    return u(6, 5) + u(cid, 14) + u(1, 5) + [0]     # cause: user requested disconnection


def chan_alloc(carrier, tn, updown):
    # replace current channel, one timeslot, no CLCH, no cell change, no
    # extended numbering, no monitoring pattern + frame 18 pattern none
    return u(0, 2) + u(8 >> (tn - 1), 4) + u(updown, 2) + [0, 0] + u(carrier, 12) + [0] + u(0, 2) + u(0, 2)


def mac_resource(ssi, sdu, alloc=None, um=None, li=None):
    """MAC-RESOURCE (21.4.3.1): address SSI (001) or SSI + usage marker
    assignment (110), an optional channel allocation, the TM-SDU; fill bits
    to the octet the length indication counts. li=0x3E / 0x3F: the PDU runs
    to the end of its block (returned open; mac_block completes it)"""
    at, addr = (6, u(ssi, 24) + u(um, 6)) if um is not None else (1, u(ssi, 24))

    def hdr(fill, li_):
        return [0, 0, fill, 0, 0, 0, 0] + u(li_, 6) + u(at, 3) + addr + [0, 0] + ([1] + alloc if alloc else [0])
    n = len(hdr(0, 0)) + len(sdu)
    if li is not None:
        # 0x3E: fill bits to the end of the block; 0x3F: the fragment fills it
        return hdr(1 if li == 0x3E else 0, li) + sdu, True
    octets = (n + 7) // 8
    fill = octets * 8 != n
    b = hdr(1 if fill else 0, octets) + sdu
    if fill:
        b += [1] + [0] * (octets * 8 - n - 1)
    return b, False


def mac_end(sdu):
    """MAC-END (21.4.3.3, pi/4-DQPSK): the last fragment, no allocation"""
    n = 13 + len(sdu)
    octets = (n + 7) // 8
    fill = octets * 8 != n
    b = [0, 1, 1, 1 if fill else 0, 0] + u(octets, 6) + [0, 0] + sdu
    if fill:
        b += [1] + [0] * (octets * 8 - n - 1)
    return b


NULL_PDU = [0, 0, 0, 0, 0, 0, 0] + u(2, 6) + u(0, 3)


def mac_block(pdus, k1):
    """PDUs one after the other in a MAC block of k1 type-1 bits, then a Null
    PDU (if it fits) and fill; a PDU marked open (LI 0x3E / 0x3F) is completed
    with fill bits to the end of the block"""
    b = []
    for bits, open_end in pdus:
        b += bits
        if open_end:
            assert len(b) < k1, (len(b), k1)
            return b + [1] + [0] * (k1 - len(b) - 1)
    assert len(b) <= k1, (len(b), k1)
    if k1 - len(b) >= 16:
        b += NULL_PDU
    if len(b) < k1:
        b += [1] + [0] * (k1 - len(b) - 1)
    return b


def parse_call(spec):
    c = {'enc': 0, 'release': 'stolen'}
    for kv in spec.split(','):
        k, v = kv.split('=', 1)
        if k == 'tx':
            c['tx'] = []
            for t in v.split('+'):
                ssi, span = t.split('@')
                t0, t1 = span.split('-')
                c['tx'].append((int(ssi), float(t0), float(t1)))
        elif k == 'at':
            c[k] = float(v)
        elif k == 'release':
            c[k] = v if v == 'stolen' else float(v)
        else:
            c[k] = int(v)
    return c


class Site:
    """what every slot of one carrier of the site carries, by its time"""

    def __init__(self, cell, calls):
        self.cell, self.calls = cell, calls
        self.scr = scramb_init(cell['mcc'], cell['mnc'], cell['cc'])
        self.rng = np.random.default_rng(cell['carrier'] * 7 + 1)
        # the MCCH's queue: (time, [PDUs]) - each entry one MCCH slot
        q = []
        for c in calls:
            src0, cid, al1 = c['tx'][0][0], c['cid'], chan_alloc(c['carrier'], c['tn'], 1)

            def ds(prop=0, c=c, cid=cid, src0=src0):
                return d_setup(cid, c['enc'], None if c.get('nocalling') else src0, proprietary=prop,
                               prio=c.get('prio', 0))
            q.append((c['at'], [mac_resource(c['gssi'], llc_mle_cmce(ds()), al1, um=c['um'])]))
            q.append((c['at'] + 0.001, [mac_resource(src0, llc_mle_cmce(d_connect(cid)),
                                                     chan_alloc(c['carrier'], c['tn'], 3), um=c['um'])]))
            end, k, t = self.call_end(c), 0, c['at'] + 2.0
            while t < end:
                if k == 0:
                    # a back-up D-SETUP in two fragments, its TM-SDU with an FCS
                    sdu = llc_mle_cmce(ds(230), fcs=True)
                    first, _ = mac_resource(c['gssi'], [], al1, um=c['um'], li=0x3F)
                    cut = 268 - len(first)
                    q.append((t, [(first + sdu[:cut], False)]))
                    q.append((t + 0.001, [(mac_end(sdu[cut:]), False)]))
                else:
                    q.append((t, [mac_resource(c['gssi'], llc_mle_cmce(ds()), al1, um=c['um'])]))
                k += 1
                t += 2.0
            for ssi, t0, _ in c['tx'][1:]:
                q.append((t0 - 0.1, [mac_resource(ssi, llc_mle_cmce(d_tx_granted(cid, 0, c['enc'], ssi)),
                                                  chan_alloc(c['carrier'], c['tn'], 3), um=c['um'])]))
        q.sort(key=lambda e: e[0])
        self.mcch = q

    @staticmethod
    def call_end(c):
        return c['tx'][-1][2] if c['release'] == 'stolen' else c['release']

    def slot_kind(self, carrier, tn, fn, t, dt):
        """what the slot [t, t + dt) of a carrier's timeslot carries"""
        if carrier == self.cell['main'] and tn == 1:
            return ('mcch',)
        for c in self.calls:
            if c['carrier'] != carrier or c['tn'] != tn or t < c['tx'][0][1] or t > self.call_end(c) + 0.2:
                continue
            gssi, cid = c['gssi'], c['cid']
            for i, (ssi, t0, t1) in enumerate(c['tx']):
                if t0 <= t < t1:
                    # this timeslot comes every 4 slots, frame 18 (sync) skipped
                    first = t < t0 + (8 if fn == 1 else 4) * dt
                    last = t + (8 if fn == 17 else 4) * dt >= t1
                    if last and i == len(c['tx']) - 1 and c['release'] == 'stolen':
                        # D-TX CEASED + D-RELEASE: both halves stolen
                        p1 = mac_resource(gssi, llc_mle_cmce(d_tx_ceased(cid)), li=0x3E)
                        p2 = mac_resource(gssi, llc_mle_cmce(d_release(cid)))
                        return ('stch', [p1], [p2], c['um'])
                    if last:
                        return ('stch', [mac_resource(gssi, llc_mle_cmce(d_tx_ceased(cid)))], None, c['um'])
                    # the 4 slots of a frame: one D-TX GRANTED in the first and every 2 s
                    if first or int((t - t0) / 2.0) != int((t - t0 - 4 * dt) / 2.0):
                        g = mac_resource(gssi, llc_mle_cmce(d_tx_granted(cid, 3, c['enc'],
                                                                         None if i >= c.get('noparty', 99) else ssi)))
                        return ('stch', [g], None, c['um'])
                    return ('tch', c['um'])
            if c['release'] != 'stolen' and c['release'] <= t < c['release'] + 8 * dt:
                return ('hang', [mac_resource(gssi, llc_mle_cmce(d_release(cid)))])
            if t < self.call_end(c):
                return ('hang', [])
        return ('free',)

    def slot(self, carrier, tn, fn, mn, t, dt):
        cell, scr = self.cell, self.scr
        if fn == 18:
            # synchronization continuous downlink burst on every timeslot
            b = [0] * 14 + [1] * 8 + [0] * 64 + [1] * 8
            b += block(bsch(cell, tn, fn, mn), 120, 11, 3)
            b += Y
            b += aach(0, scr)
            b += block(sysinfo(cell), 216, 101, scr)
            return b + [0] * 12
        k = self.slot_kind(carrier, tn, fn, t, dt)
        train = N
        if k[0] == 'mcch':
            pdus = self.mcch.pop(0)[1] if self.mcch and self.mcch[0][0] <= t else []
            blk = block(mac_block(pdus, 268), 432, 103, scr)
            msg = 0                         # common control
        elif k[0] == 'tch':
            blk = [int(x) for x in self.rng.integers(0, 2, 432)]   # TCH/S: random speech frames
            msg = (2 << 12) | (k[1] << 6)   # assigned only, the traffic usage marker
        elif k[0] == 'stch':
            h1 = block(mac_block(k[1], 124), 216, 101, scr)
            h2 = (block(mac_block(k[2], 124), 216, 101, scr) if k[2]
                  else [int(x) for x in self.rng.integers(0, 2, 216)])
            blk = h1 + h2
            train = P                       # normal training sequence 2: the first half stolen
            msg = (2 << 12) | (k[3] << 6)
        elif k[0] == 'hang':
            blk = block(mac_block(k[1], 268), 432, 103, scr)
            msg = (2 << 12) | (1 << 6)     # assigned control
        else:
            blk = block(mac_block([], 268), 432, 103, scr)
            msg = 1 << 12                   # unallocated
        bb = aach(msg, scr)
        return [0] * 14 + blk[:216] + bb[:14] + train + bb[14:] + blk[216:] + [0] * 12


def trunk_main(out, cell, calls, seconds, rate):
    site = Site(cell, calls)
    nslots = int(seconds * 18000 / 255) + 2
    rng = np.random.default_rng(cell['cc'])
    tn, fn, mn = 1, int(rng.integers(1, 18)), int(rng.integers(1, 61))
    dphi = {(0, 0): np.pi / 4, (0, 1): 3 * np.pi / 4, (1, 0): -np.pi / 4, (1, 1): -3 * np.pi / 4}
    ph, syms = 0.0, []
    dt = 255 / 18000.0
    for k in range(nslots):
        b = site.slot(cell['carrier'], tn, fn, mn, k * dt, dt)
        assert len(b) == 510, len(b)
        for i in range(255):
            ph += dphi[(b[2 * i], b[2 * i + 1])]
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
    x = x[:int(seconds * rate)]
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))
    f = 300e6 + cell['carrier'] * 25e3
    print('%.1f s  cell MCC %d MNC %d CC %d LA %d, carrier %d (%.4f MHz)%s' % (
        seconds, cell['mcc'], cell['mnc'], cell['cc'], cell['la'], cell['carrier'], f / 1e6,
        ' = the main carrier (MCCH)' if cell['carrier'] == cell['main'] else ', main carrier %d' % cell['main']))
    for c in calls:
        if c['carrier'] == cell['carrier']:
            print('  call %d TG %d on timeslot %d: %s%s' % (c['cid'], c['gssi'], c['tn'], ', '.join(
                '%d %.2f-%.2f s' % x for x in c['tx']), ', end-to-end encrypted' if c['enc'] else ''))


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
    rate, seed, traffic = FS, None, 0
    on = []
    calls = []
    seconds = None
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'seed':
            seed = int(v)
        elif k == 'at':
            on.append([float(v), 5.0])
        elif k == 'dur':
            on[-1][1] = float(v)
        elif k == 'call':
            calls.append(parse_call(v))
        elif k == 'seconds':
            seconds = float(v)
        elif k == 'truncate':
            global TRUNCATE
            TRUNCATE = int(v)
        elif k == 'traffic':
            traffic = int(v)
        else:
            cell[k] = int(v)
        i += 2
    if seconds is not None or calls:
        cell.setdefault('main', cell['carrier'])
        return trunk_main(out, cell, calls, seconds or 15.0, rate)
    end = max(t + d for t, d in on) + 0.2
    nslots = int(end * 18000 / 255) + 2
    rng = np.random.default_rng(cell['cc'] if seed is None else seed)
    # the cell's own TDMA time and symbol timing
    tn, fn, mn = 1, int(rng.integers(1, 18)), int(rng.integers(1, 61))
    dphi = {(0, 0): np.pi / 4, (0, 1): 3 * np.pi / 4, (1, 0): -np.pi / 4, (1, 1): -3 * np.pi / 4}
    ph, syms = 0.0, []
    # (its own generator: the cell's timing above is drawn the same with or without it)
    rnd = np.random.default_rng([traffic, cell['cc'] if seed is None else seed]) if traffic else None
    for _ in range(nslots):
        b = slot_bits(cell, tn, fn, mn, rnd)
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
