#!/usr/bin/env python3
"""NXDN test transmissions, from the NXDN Forum's TS 1-A (Common Air
Interface) and TS 1-C (Trunking Procedures, Type-C), version 1.3 - section
numbers below are theirs. 4FSK with RRC 0.2: NXDN96 (9600 bps, 4800 Bd,
+-800 / +-2400 Hz) or NXDN48 (--bps 4800: 2400 Bd, +-350 / +-1050 Hz). Every
frame: frame sync word, LICH (7 bits + even parity of the top 4, each bit a
dibit), the coded channels (CRC with the register preset to ones, 4 tail
bits, the K=5 rate-1/2 code G1 = 1+D^3+D^4 / G2 = 1+D+D^2+D^4, puncturing,
row-in column-out interleaving) and the PN scrambler x^9 + x^4 + 1.

One radio's calls (conventional RDCH, or a trunked traffic channel with
--trunk 1):
  nxdn_gen.py OUT.cf32 [--rate 48000] [--bps 9600|4800] [--seed N]
              --at T --dur S --src UNIT --dst TG [--unit 1] [--ran 1] [--outbound 1]
              [--trunk 1] [--cipher 0-3] [--key K] [--emergency 1]
              [--hold S] [--disc 1|2] [--dup CH] [--at ... (next call)]
  a call = a header frame (VCALL in both FACCH1), voice frames (random
  AMBE+2 bits, the VCALL in the SACCH superframe), a TX_REL frame; with
  --hold the TC's TX_REL_EX (talk-back permitted) for S seconds after it,
  with --disc 1 / 2 its DISC (FACCH1 / FACCH2) at the end (TS 1-C 16.4),
  with --dup CH VCALL_ASSGN_DUP for the call's group on CH in every other
  hang frame.
  --trunk 1 sends it as the TC's outbound RTCH (call type conference /
  individual, the transmission mode of --bps), --trunk 2 as a composite
  control channel (RTCH_C, TS 1-C 16.3).

A Type-C control channel (outbound RCCH, 4.4.1: CAC + collision control
field + post field), running the whole file:
  nxdn_gen.py OUT.cf32 --control [--rate 48000] [--bps 9600|4800] --seconds S
              [--ran R] [--cat global|regional|local] [--system N] [--site N]
              [--cc1 N] [--cc2 N] [--adj SITE:CH]...
              [--grant T:DUR:CH:TARGET:SRC[:FLAGS]]... [--cch-new T:CH]
  superframe of 9 frames (Bn 1, Gn 1, Pn 2, Mn 2, In 2 - TS 1-C Figure 5.1-2):
  SITE_INFO in its first frame (single message format), SRV_INFO +
  ADJ_SITE_INFO (dual format) where nothing else is due. A grant at T:
  VCALL_ASSGN in the next paging frame and twice more 0.25 s apart (Nr_ob 3,
  Tr_ob), then VCALL_ASSGN_DUP every 0.5 s (Tr_dup) until T + DUR - in the
  dual format, paired with another grant or SRV_INFO. FLAGS: u = unit-to-unit
  call (TARGET is a unit), e = emergency, d = data call (DCALL_ASSGN / _DUP),
  n = the call is 4800 bps (NXDN48). --cch-new T:CH: CCH_INFO "New" for this
  site (the control channel moves) from T on, 3 times.

Output: complex baseband (cf32) at --rate + OUT.json. Put several radios at
their own bearings with plugins/sdk/tools/uca_scene.py, a trunked system's
control and traffic channels across the 2.4 MHz with sdk/tools/wide_scene.py.
Only made-up IDs.

--seed N: the random generator's seed (the AMBE+2 bits); without it 11, so
the same options give the same file.
"""
import json
import sys

import numpy as np

FSW = [-3, 1, -3, 3, -3, -3, 3, 3, -1, 3]                 # 4.4.4, CDF59
POST = [3, 3, 3, -3, 3, -3, 3, 3, -3, -3, -3, 3]           # 4.4.5
DIBIT = {(0, 1): 3, (0, 0): 1, (1, 0): -1, (1, 1): -3}     # 3.3
SYM_BITS = {v: k for k, v in DIBIT.items()}
PUNCT14 = {3, 11}   # puncturing matrix 1111111 / 1011101 (CAC, FACCH2): X4, X12 of every 14 erased


def pn_sequence(n):
    """4.6: x^9 + x^4 + 1, preset S8..S0 = 0 1 1 1 0 0 1 0 0, output S0,
    S0 ^ S4 shifted in at S8 - one bit per symbol from the LICH on"""
    s = [0, 0, 1, 0, 0, 1, 1, 1, 0]        # S0..S8
    out = []
    for _ in range(n):
        out.append(s[0])
        fb = s[0] ^ s[4]
        s = s[1:] + [fb]
    return out


PN = pn_sequence(182)


def crc_ones(bits, width, poly):
    # 4.5.5: register preset to all ones; the CRC is the register after the
    # last bit, S(width-1) first
    mask, top = (1 << width) - 1, 1 << (width - 1)
    crc = mask
    for b in bits:
        fb = bool(crc & top) ^ bool(b)
        crc = (crc << 1) & mask
        if fb:
            crc ^= poly
    return [(crc >> (width - 1 - i)) & 1 for i in range(width)]


def conv(bits):
    # K = 5, G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4, read G1, G2 alternately
    out, d = [], [0, 0, 0, 0]               # d[k] = the input k+1 bits ago
    for b in bits:
        out.append(b ^ d[2] ^ d[3])
        out.append(b ^ d[0] ^ d[1] ^ d[3])
        d = [b] + d[:3]
    return out


def interleave(y, rows):
    # written row by row (12 / 4 / ... per row), read column by column
    cols = len(y) // rows
    return [y[r * cols + c] for c in range(cols) for r in range(rows)]


def code(info, crc_w, poly, period, punct, rows):
    bits = info + crc_ones(info, crc_w, poly) + [0, 0, 0, 0]
    m = conv(bits)
    return interleave([b for i, b in enumerate(m) if i % period not in punct], rows)


def u(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def octets(b):
    bits = []
    for x in b:
        bits += u(x, 8)
    return bits


# --- channel coding (4.5) ------------------------------------------------------
def sacch(info26):          # SR + 18 bits, CRC-6 (x^6+x^5+x^2+x+1), 60 bits, 12 x 5
    return code(info26, 6, 0x27, 6, {5}, 5)


def facch1(l3_80):          # 80 bits, CRC-12 (x^12+x^11+x^3+x^2+x+1), 144 bits, 16 x 9
    return code(l3_80, 12, 0x80F, 4, {1}, 9)


def cac(sr8, l3_144):       # SR + 144 + 3 null, CRC-16 (x^16+x^12+x^5+1), 300 bits, 12 x 25
    return code(sr8 + l3_144 + [0, 0, 0], 16, 0x1021, 14, PUNCT14, 25)


def facch2(sr8, l3_176):    # SR + 176, CRC-15, 348 bits, 12 x 29
    return code(sr8 + l3_176, 15, 0x4CC5, 14, PUNCT14, 29)


def lich(rfct, fct, opt, outbound):
    v = (rfct << 5) | (fct << 3) | (opt << 1) | outbound   # 7 bits
    p = ((v >> 6) ^ (v >> 5) ^ (v >> 4) ^ (v >> 3)) & 1       # even parity of the top 4
    bits = []
    for b in u(v, 7) + [p]:
        bits += [b, 1]                                         # 0 -> 01, 1 -> 11
    return bits


def frame(lich_bits, body, post=False):
    """FSW + LICH + body (364 bits, or 340 + post) -> 192 symbols"""
    bits = []
    for s in FSW:
        bits += list(SYM_BITS[s])
    bits += lich_bits + body
    nsc = 182 - (12 if post else 0)       # the post field is not scrambled
    assert len(bits) == (384 - 24 if post else 384)
    syms = [DIBIT[(bits[2 * j], bits[2 * j + 1])] for j in range(len(bits) // 2)]
    for k in range(nsc):
        if PN[k]:
            syms[10 + k] = -syms[10 + k]
    if post:
        syms += POST
    return syms


# --- layer 3 messages (6.4) ----------------------------------------------------
def cc_option(emergency):
    return 0x80 if emergency else 0x00


def call_byte(unit, mode, conventional=False):
    # Call Type (6.5.12) 001 conference / 100 individual + Voice Call Option
    # (6.5.13): duplex 0, transmission mode 000 = 4800 bps, 010 = 9600 bps EHR
    if conventional:
        return 0x80 if unit else 0x00
    return ((4 if unit else 1) << 5) | mode


def l3_call(kind, src, dst, unit=False, mode=2, cipher=0, key=0, emergency=False, conventional=False, tx=1):
    t = {'vcall': 0x01, 'rel': 0x08, 'rel_ex': 0x07, 'disc': 0x11}[kind]
    cb = call_byte(unit, mode, conventional) if kind == 'vcall' else (call_byte(unit, mode, conventional) & 0xE0)
    o7 = (cipher << 6) | key if kind == 'vcall' else (tx << 7 if kind == 'rel_ex' else (0x20 if kind == 'disc' else 0))
    if conventional and kind in ('vcall', 'rel'):
        cc = 0
    else:
        cc = cc_option(emergency)
    return [t, cc, cb, src >> 8, src & 0xFF, dst >> 8, dst & 0xFF, o7, 0x00, 0x00]


CATS = {'global': (0, 10, 12), 'local': (1, 17, 5), 'regional': (2, 14, 8)}


def location_id(cat, system, site):
    c, sb, tb = CATS[cat]
    return (c << 22) | (system << tb) | site


def m_site_info(lid, cc1, cc2, nadj):
    # 6.4.3.1: Location ID, Channel Structure (Bn 1, Gn 1, Pn 2, Mn 2, In 2),
    # Service Information, Restriction Information, Channel Access
    # Information (spare in 1.3), Version, Adjacent Site Allocation, 1st / 2nd
    # control channel
    chs = (1 << 14) | (1 << 11) | (2 << 7) | (2 << 4) | 2
    svc = 0b1011_0011_0000_0000    # multi-site, registration, group reg., voice, data
    bits = u(0x18, 8) + u(lid, 24) + u(chs, 16) + u(svc, 16) + u(0, 24) + u(0, 24) + u(0x01, 8)
    bits += u(nadj, 4) + u(cc1, 10) + u(cc2, 10)
    assert len(bits) == 144
    return bits


def m_srv_info(lid):
    svc = 0b1011_0011_0000_0000
    return u(0x19, 8) + u(lid, 24) + u(svc, 16) + u(0, 24)                 # 9 octets


def m_adj_site(entries, nbits):
    bits = u(0x1B, 8)
    for lid, num, ch in entries:
        bits += u(lid, 24) + u(num & 0x0F, 6) + u(ch, 10)
    return (bits + [0] * nbits)[:nbits]


def m_cch_info_new(lid, ch):
    # 6.4.3.3: flags Curr New Add Del, Control Channel 1 / 2
    return u(0x1A, 8) + u(lid, 24) + [0, 1, 0, 0, 0, 0] + u(ch, 10) + [0] * 6 + u(0, 10) + [0] * 8


def m_assign(dup, data, src, dst, ch, unit, emergency, mode):
    # 6.4.1.25 / 26 / 31 / 32: VCALL_ASSGN 0x04, _DUP 0x05, DCALL_ASSGN 0x0E, _DUP 0x0D
    t = (0x0D if dup else 0x0E) if data else (0x05 if dup else 0x04)
    bits = u(t, 8) + u(cc_option(emergency), 8) + u((4 if unit else 1), 3) + u(mode, 5)
    bits += u(src, 16) + u(dst, 16) + u(0, 6) + u(ch, 10)                  # Call Timer unspecified
    assert len(bits) == 72
    return bits


def m_idle():
    return u(0x10, 8) + [0] * 64


# --- traffic channel ---------------------------------------------------------
def traffic_call(tx, rng):
    src, dst, ran = tx['src'], tx['dst'], tx['ran']
    trunk = bool(tx['trunk'])
    rfct = {0: 2, 1: 1, 2: 3}[tx['trunk']]        # RDCH / RTCH / RTCH_C (composite control channel)
    outb = tx['outbound']
    unit = bool(tx['unit'])
    mode = 0 if tx['bps'] == 4800 else 2
    kw = dict(unit=unit, mode=mode, emergency=bool(tx['emergency']), conventional=not trunk)
    vcall = l3_call('vcall', src, dst, cipher=tx['cipher'], key=tx['key'], **kw)
    facch_v = facch1(octets(vcall))
    facch_r = facch1(octets(l3_call('rel', src, dst, **kw)))
    vc72 = octets(vcall)[:72]
    sa_single = [0, 0] + u(ran, 6) + [0] * 18     # non-superframe SACCH (structure 00)
    syms = []
    # header: VCALL in both FACCH1 halves, non-superframe SACCH (usc 0, option 0)
    syms += frame(lich(rfct, 0, 0, outb), sacch(sa_single) + facch_v + facch_v)
    fdur = 0.04 if tx['bps'] == 9600 else 0.08    # 4.4: 40 ms at 9600 bps, 80 ms at 4800 bps
    n = max(4, int(tx['dur'] / fdur) // 4 * 4)
    for f in range(n):
        sr = 3 - f % 4
        sa = u(sr, 2) + u(ran, 6) + vc72[18 * (f % 4):18 * (f % 4) + 18]
        voice = list(rng.integers(0, 2, 144)) + list(rng.integers(0, 2, 144))
        syms += frame(lich(rfct, 2, 3, outb), sacch(sa) + voice)
    syms += frame(lich(rfct, 0, 0, outb), sacch(sa_single) + facch_r + facch_r)
    # the TC after the transmission: TX_REL_EX (talk-back permitted) for
    # the hold time, then DISC (TS 1-C 16.4)
    if tx['hold'] > 0:
        rex = facch1(octets(l3_call('rel_ex', src, dst, tx=1, **kw)))
        # --dup CH: every other hang frame carries VCALL_ASSGN_DUP (RTCH
        # format, 6.4.1.26 - the group's channel, late entry) in its second FACCH1
        dup = None
        if tx['dup']:
            dup = facch1(m_assign(True, False, src, dst, tx['dup'], False, bool(tx['emergency']), mode) + [0] * 8)
        for k in range(int(round(tx['hold'] / fdur))):
            syms += frame(lich(rfct, 0, 0, 1), sacch(sa_single) + rex + (dup if dup and k % 2 else rex))
    if tx['disc'] == 1:
        dsc = facch1(octets(l3_call('disc', src, dst, **kw)))
        for _ in range(2):
            syms += frame(lich(rfct, 0, 0, 1), sacch(sa_single) + dsc + dsc)
    elif tx['disc'] == 2:
        sr8 = [0, 0] + u(ran, 6)
        l3 = (octets(l3_call('disc', src, dst, **kw)) + [0] * 176)[:176]
        for _ in range(2):
            syms += frame(lich(rfct, 1, 0, 1), facch2(sr8, l3))
    return syms


# --- control channel -----------------------------------------------------------
def control_channel(c):
    fdur = 0.04 if c['bps'] == 9600 else 0.08
    nframes = int(round(c['seconds'] / fdur))
    lid = location_id(c['cat'], c['system'], c['site'])
    ran = c['ran']
    adj = [(location_id(c['cat'], c['system'], s), i + 1, ch) for i, (s, ch) in enumerate(c['adj'])]
    # outbound messages due: (time, 72-bit message)
    due = []
    for g in c['grants']:
        mode = 0 if 'n' in g['flags'] else 2
        args = (g['src'], g['target'], g['ch'], 'u' in g['flags'], 'e' in g['flags'], mode)
        data = 'd' in g['flags']
        for k in range(3):                                    # Nr_ob = 3, Tr_ob = 250 ms
            due.append((g['t'] + 0.25 * k, m_assign(False, data, *args)))
        t = g['t'] + 1.0
        while t < g['t'] + g['dur']:                          # Tr_dup = 0.5 s
            due.append((t, m_assign(True, data, *args)))
            t += 0.5
    for t, ch in c['cch_new']:
        for k in range(3):
            due.append((t + 0.25 * k, m_cch_info_new(lid, ch)))
    due.sort(key=lambda x: x[0])
    syms = []
    pending = []
    adj_i = 0
    e_field = [1, 1] + [0, 1] + [1, 1] + [0, 1] + [0] * 16    # I/B idle, P/D don't, CK, R/N none, PE
    for k in range(nframes):
        t0 = k * fdur
        while due and due[0][0] <= t0:
            pending.append(due.pop(0)[1])
        pos = k % 9                       # BC P P M M P P M M
        head = pos == 0
        paging = pos in (1, 2, 5, 6)
        if head:
            msgs = [m_site_info(lid, c['cc1'], c['cc2'], len(adj))]
        elif paging and pending:
            msgs = [pending.pop(0)]
            # the dual format never carries the same message twice (6.2.2.1)
            j = next((n for n, p in enumerate(pending) if p != msgs[0]), None)
            msgs.append(pending.pop(j) if j is not None else m_srv_info(lid))
        else:
            other = m_adj_site([adj[adj_i % len(adj)]], 72) if adj else m_idle()
            adj_i += 1
            msgs = [m_srv_info(lid), other]
        dual = len(msgs) == 2
        sr = [1 if head else 0, 1 if dual else 0] + u(ran, 6)
        body = cac(sr, msgs[0] + msgs[1] if dual else msgs[0])
        syms += frame(lich(0, 0, 0, 1), body + e_field, post=True)
    return syms


# --- modulation ---------------------------------------------------------------
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


def modulate(syms, rate, bps):
    sps = 10 if bps == 9600 else 20                      # at 48 kHz
    dev = 800.0 if bps == 9600 else 350.0                # Hz per level unit (3.3)
    up = np.zeros(len(syms) * sps + 12 * sps)
    up[6 * sps:6 * sps + len(syms) * sps:sps] = syms
    h = rrc(sps, 6, 0.2)
    f = np.convolve(up, h / h[len(h) // 2], 'same') * dev
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
    rate, bps, seed = 48000.0, 9600, None
    txs, cur = [], None
    ctl = None
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        if k == 'control':
            ctl = {'seconds': 10.0, 'ran': 1, 'cat': 'global', 'system': 123, 'site': 9, 'cc1': 1, 'cc2': 0,
                   'adj': [], 'grants': [], 'cch_new': []}
            i += 1
            continue
        v = a[i + 1]
        i += 2
        if k == 'rate':
            rate = float(v)
        elif k == 'bps':
            bps = int(v)
        elif k == 'seed':
            seed = int(v)
        elif ctl is not None:
            if k == 'seconds':
                ctl[k] = float(v)
            elif k == 'cat':
                ctl[k] = v
            elif k == 'adj':
                s, ch = v.split(':')
                ctl['adj'].append((int(s), int(ch)))
            elif k == 'grant':
                p = v.split(':')
                ctl['grants'].append({'t': float(p[0]), 'dur': float(p[1]), 'ch': int(p[2]), 'target': int(p[3]),
                                      'src': int(p[4]), 'flags': p[5] if len(p) > 5 else ''})
            elif k == 'cch-new':
                t, ch = v.split(':')
                ctl['cch_new'].append((float(t), int(ch)))
            else:
                ctl[k] = int(v)
        elif k == 'at':
            cur = {'at': float(v), 'dur': 3.0, 'src': 1001, 'dst': 100, 'ran': 1, 'outbound': 0, 'unit': 0,
                   'trunk': 0, 'cipher': 0, 'key': 0, 'emergency': 0, 'hold': 0.0, 'disc': 0, 'dup': 0}
            txs.append(cur)
        else:
            cur[k] = float(v) if k in ('dur', 'hold') else int(v)
    if bps not in (4800, 9600):
        sys.exit('--bps: 4800 or 9600')
    rng = np.random.default_rng(11 if seed is None else seed)
    parts = []
    if ctl is not None:
        y = modulate(control_channel(dict(ctl, bps=bps)), rate, bps)
        parts.append(({'at': 0.0}, y))
        lid = location_id(ctl['cat'], ctl['system'], ctl['site'])
        print('control channel: %s system %d site %d (location ID %06X), RAN %d, CC %d/%d, %.2f s' %
              (ctl['cat'], ctl['system'], ctl['site'], lid, ctl['ran'], ctl['cc1'], ctl['cc2'], len(y) / rate))
        for g in ctl['grants']:
            print('%8.3f .. %8.3f s  grant %s %d <- %d on channel %d%s' %
                  (g['t'], g['t'] + g['dur'], 'unit' if 'u' in g['flags'] else 'TG', g['target'], g['src'], g['ch'],
                   (' [' + g['flags'] + ']') if g['flags'] else ''))
    for tx in txs:
        tx['bps'] = bps
        y = modulate(traffic_call(tx, rng), rate, bps)
        parts.append((tx, y))
        print('%8.3f .. %8.3f s  unit %d -> %s %d  RAN %d%s%s' % (
            tx['at'], tx['at'] + len(y) / rate, tx['src'], 'unit' if tx['unit'] else 'TG', tx['dst'], tx['ran'],
            '  trunked' if tx['trunk'] else '', ('  cipher %d' % tx['cipher']) if tx['cipher'] else ''))
    end = max(tx['at'] + len(y) / rate for tx, y in parts) + (0.0 if ctl is not None else 0.3)
    if ctl is not None:
        end = max(end, ctl['seconds'])
    x = np.zeros(int(end * rate), np.complex128)
    for tx, y in parts:
        n0 = int(round(tx['at'] * rate))
        m = min(len(y), len(x) - n0)
        x[n0:n0 + m] += y[:m]
    x.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
