#!/usr/bin/env python3
"""MPT1327 test signals as on the air (MPT 1327 / MPT 1343): 1200 bit/s FFSK
(1 = one cycle of 1200 Hz, 0 = one and a half of 1800 Hz) on NBFM. Every
codeword: 48 bits + BCH(63,48) check (last bit inverted) + even parity
(MPT 1327 3.2.3); field layouts from MPT 1327 section 5.

CONTROL CHANNEL (a site's forward control channel): back-to-back 128-bit
slots, each the control channel system codeword (CCSC: 0, SYS, CCS, preamble
1010..; its CCS chosen so that the check bits are the SYNC 0xC4D7) and an
address codeword - an ALOHA when nothing else is due.

  mpt1327_gen.py OUT.cf32 --sys 0x42E1 [--prefix 42] [--rate 48000] [--dev 2500]
                 --at T --dur S [--at T --dur S ...]
                 [--grant T:CHAN:CALLED:CALLING[:FLAGS]] ...
                 [--bcast SYSDEF:CHAN[:SYS]] ... [--maint PER:IVAL:PON:ID]
                 [--move T:CONT] [--clear T:CHAN:CONT]

  --grant  a GTC at T s (sent in two slots in a row, as MPT 1327 9.1.1.12's
           examples) for channel CHAN (the 10-bit field), CALLED / CALLING
           "PPP-IIII" (prefix-ident) or ALL (the system-wide ident). FLAGS:
           u = an individual call (the TSC checks the called unit first:
           AHY, POINT 0, 0.4 s before), e = emergency (ACKE 0.2 s before),
           d = a data call (bit D), rN = sent again N and 2N s later (a
           system that re-sends GTCs during the call for late entry)
  --bcast  a BCAST every 3 s (in turn with the others): SYSDEF 0 announce
           control channel, 4 adjacent site (SYS = the neighbour), 5 vote now
  --maint  BCAST SYSDEF 2 (call maintenance parameters) every 3 s too
  --move   MOVE to control channel CONT, every radio ((M) = 0)
  --clear  a CLEAR codeword on the control channel (not standard; a test
           that it gives no grant end there)

TRAFFIC CHANNEL (the site's downlink of one call, talk-through: the radios'
call maintenance messages repeated, then the TSC's clear-down):

  mpt1327_gen.py OUT.cf32 --traffic --chan N [--cont M] [--rate 48000] [--dev 2500]
                 --item T:D:UNIT[:ADDR][:x] [--item ...] [--periodic S]
                 [--clear T] [--key T] [--seed N] [--sdev 2000]

  --item   a speech item by UNIT ("PPP-IIII") from T for D s: Pressel On
           (MAINT OPER 000) at T, made-up speech-like audio (tone glides +
           filtered noise, no real speech), Periodic (010) every --periodic s,
           Pressel Off (001) at the end; MAINT's PFIX/IDENT1 = ADDR if given
           (a group's address, the BCAST ID = 0 rule), else UNIT; x = then
           two Disconnects (011); o = no Pressel Off (a lost message)
  --clear  two CLEAR messages (multiple message format) at T, carrier off
           after them; without it the carrier ends with the last item
  --key    the downlink's carrier from T (default: 0.2 s before the first
           item)
  --uplink the radios' side instead: a carrier per item, no clear-down

Both write OUT (cf32) + OUT.json (rate) + OUT.truth.json (the messages sent,
with their times). Several transmitters at their bearings: plugins/sdk/tools/
uca_scene.py (narrow), wide_scene.py (a control channel and its traffic
channels in one 2.4 MHz).
"""
import json
import sys

import numpy as np

FS = 48000
SPB = 40
SYNC = 0xC4D7
SYNT = 0x3B28
SLOT_S = 128 / 1200.0


def check16(info48):
    """BCH(63,48) check (g 0xE815, last check bit inverted) + even parity -> the codeword's low 16 bits"""
    r = info48 << 15
    for i in range(62, 14, -1):
        if (r >> i) & 1:
            r ^= 0xE815 << (i - 15)
    chk = (r ^ 1) & 0x7FFF
    cw = (info48 << 16) | (chk << 1)
    return cw | (bin(cw).count('1') & 1)


def ccsc(sys_id):
    # 0 | SYS (15) | CCS (16) | 0xAAAA: the CCS that makes the check bits the SYNC
    for ccs in range(1 << 16):
        info = (sys_id & 0x7FFF) << 32 | ccs << 16 | 0xAAAA
        cw = check16(info)
        if cw & 0xFFFF == SYNC:
            return cw
    raise SystemExit('no CCS for SYS 0x%04X' % sys_id)


def fields(spec):
    """address codeword from [(value, bits), ...] filling the 48 information bits"""
    v, n = 0, 0
    for val, bits in spec:
        assert 0 <= val < (1 << bits), (val, bits)
        v = (v << bits) | val
        n += bits
    assert n == 48, n
    return check16(v)


def gen_type(cat, typ, func):
    # bit 22 = 1, CAT 3, TYPE 2, FUNC 3 (MPT 1327 5.2)
    return [(1, 1), (cat, 3), (typ, 2), (func, 3)]


def aloha(prefix, ident=8191, func=0, chan4=0):
    # 5.5.1: 1 | PFIX | IDENT1 | 1 000 00 FUNC | CHAN4 4 | WT 3 | RSVD 2 | (M) 5 | (N) 4
    return fields([(1, 1), (prefix, 7), (ident, 13)] + gen_type(0, 0, func) +
                  [(chan4, 4), (0, 3), (0, 2), (0, 5), (0, 4)])


def gtc(prefix, id1, chan, id2, d=0):
    # 5.4: 1 | PFIX 7 | IDENT1 13 | 0 | D | CHAN 10 | IDENT2 13 | (N) 2
    return fields([(1, 1), (prefix, 7), (id1, 13), (0, 1), (d, 1), (chan, 10), (id2, 13), (0, 2)])


def ack(prefix, id1, id2, func=0, qual=0):
    # 5.5.2: 1 | PFIX | IDENT1 | 1 000 01 FUNC | IDENT2 13 | QUAL 1 | (N) 4
    return fields([(1, 1), (prefix, 7), (id1, 13)] + gen_type(0, 1, func) + [(id2, 13), (qual, 1), (0, 4)])


def ahy(prefix, id1, id2, point=0, e=0):
    # 5.5.3.2.1: 1 | PFIX | IDENT1 | 1 000 10 000 | IDENT2 13 | D | POINT | CHECK | E | AD
    return fields([(1, 1), (prefix, 7), (id1, 13)] + gen_type(0, 2, 0) +
                  [(id2, 13), (0, 1), (point, 1), (0, 1), (e, 1), (0, 1)])


def maint(prefix, ident, chan, oper):
    # 5.5.4.2: 1 | PFIX | IDENT1 | 1 000 11 001 | CHAN 10 | OPER 3 | RSVD 5
    return fields([(1, 1), (prefix, 7), (ident, 13)] + gen_type(0, 3, 1) + [(chan, 10), (oper, 3), (0, 5)])


def clear(chan, cont):
    # 5.5.4.3: 1 | CHAN 10 | CONT 10 | 1 000 11 010 | RSVD 4 | SPARE 2 | REVS 101010101010
    return fields([(1, 1), (chan, 10), (cont, 10)] + gen_type(0, 3, 2) + [(0, 4), (0, 2), (0xAAA, 12)])


def move(prefix, ident, cont, m=0):
    # 5.5.4.4: 1 | PFIX | IDENT1 | 1 000 11 011 | CONT 10 | (M) 5 | RSVD 2 | SPARE 1
    return fields([(1, 1), (prefix, 7), (ident, 13)] + gen_type(0, 3, 3) + [(cont, 10), (m, 5), (0, 2), (0, 1)])


def bcast(sysdef, sys_id, params18):
    # 5.5.4.5: 1 | SYSDEF 5 | SYS 15 | 1 000 11 100 | PARAMETERS 18
    return fields([(1, 1), (sysdef, 5), (sys_id & 0x7FFF, 15)] + gen_type(0, 3, 4) + [(params18, 18)])


def bcast_chan(sysdef, sys_id, chan, adjsite=0):
    # a) / b) / e) / f): CHAN 10 | SPARE 2 | RSVD 2 | ADJSITE 4 (a, b: RSVD 6)
    return bcast(sysdef, sys_id, chan << 8 | adjsite)


def bcast_maint(sys_id, per, ival, pon, mid):
    # c): PER 1 | IVAL 5 | PON 1 | ID 1 | RSVD 2 | SPARE 8
    return bcast(2, sys_id, per << 17 | ival << 12 | pon << 11 | mid << 10)


def bits64(cw):
    return [(cw >> (63 - i)) & 1 for i in range(64)]


def bitsn(v, n):
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def party(s, default_prefix):
    """'PPP-IIII' -> (prefix, ident); 'ALL' -> (prefix, 8191)"""
    if s.upper() == 'ALL':
        return default_prefix, 8191
    p, i = s.split('-')
    return int(p), int(i)


def ffsk_audio(bits):
    """phase-continuous FFSK, unit amplitude, SPB samples per bit"""
    f = np.repeat(np.where(np.asarray(bits) > 0, 1200.0, 1800.0), SPB)
    return np.sin(2 * np.pi * np.cumsum(f) / FS)


def fm(audio_hz, rate, on=None):
    x = np.exp(2j * np.pi * np.cumsum(audio_hz) / FS)
    if on is not None:
        x = x * on
    if abs(rate - FS) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / FS).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    return x


def ramp_mask(n, spans, rate=FS):
    """1 inside each [a, b) sample span, 3 ms ramps"""
    m = np.zeros(n)
    r = int(0.003 * rate)
    for a, b in spans:
        a, b = max(0, a), min(n, b)
        if b <= a:
            continue
        k = np.arange(b - a)
        m[a:b] = np.maximum(m[a:b], np.minimum(1, np.minimum(k, k[::-1]) / max(1, r)))
    return m


def write(out, x, rate, truth):
    x.astype(np.complex64).tofile(out)
    base = out.rsplit('.', 1)[0]
    json.dump({'rate': rate}, open(base + '.json', 'w'))
    json.dump(truth, open(base + '.truth.json', 'w'), indent=1)


def parse(a):
    out = a[0]
    o = {'sys': 0x42E1, 'prefix': 42, 'rate': 48000.0, 'dev': 2500.0, 'sdev': 2000.0, 'chan': 0, 'cont': 0,
         'periodic': 0.0, 'seed': 1, 'clear': None, 'key': None}
    on, grants, bcasts, items, moves, clears = [], [], [], [], [], []
    traffic = False
    maint_p = None
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        if k in ('traffic', 'uplink'):
            o[k] = traffic = True
            i += 1
            continue
        v = a[i + 1]
        if k == 'at':
            on.append([float(v), 5.0])
        elif k == 'dur':
            on[-1][1] = float(v)
        elif k == 'grant':
            grants.append(v)
        elif k == 'bcast':
            bcasts.append(v)
        elif k == 'maint':
            maint_p = [int(x) for x in v.split(':')]
        elif k == 'move':
            moves.append(v)
        elif k == 'item':
            items.append(v)
        elif k == 'clear':
            if traffic:
                o['clear'] = float(v)
            else:
                clears.append(v)
        elif k in ('sys', 'prefix', 'chan', 'cont', 'seed'):
            o[k] = int(v, 0)
        else:
            o[k] = float(v)
        i += 2
    return out, o, on, grants, bcasts, maint_p, moves, clears, items, traffic


def control(out, o, on, grants, bcasts, maint_p, moves, clears):
    prefix, sys_id = o['prefix'], o['sys']
    c = bits64(ccsc(sys_id))
    end = max(t + d for t, d in on) + 0.2
    nslots = int(end * 1200 / 128) + 2
    idle = aloha(prefix)
    slots = [idle] * nslots
    what = [None] * nslots
    truth = []

    def put(t, cw, label):
        s = max(0, int(np.ceil(t / SLOT_S - 1e-9)))
        while s < nslots and what[s] is not None:
            s += 1
        if s < nslots:
            slots[s] = cw
            what[s] = label
            truth.append({'t': round(s * SLOT_S, 4), 'msg': label})

    for g in grants:
        p = g.split(':')
        t, chan = float(p[0]), int(p[1])
        pc, ic = party(p[2], prefix)
        pa, ia = party(p[3], prefix)
        fl = p[4] if len(p) > 4 else ''
        pre = pc if ic != 8191 else pa
        if 'u' in fl:
            put(t - 0.4, ahy(pre, ic, ia, point=0, e=1 if 'e' in fl else 0), 'AHY %s' % p[2])
        if 'e' in fl:
            put(t - 0.2, ack(pre, ic, ia, func=5), 'ACKE %s %s' % (p[2], p[3]))
        reps = [t, t + SLOT_S]
        if 'r' in fl:
            n = int(fl.split('r')[1][:1] or 2)
            reps += [t + n * k for k in (1, 2)]
        for k, tt in enumerate(reps):
            put(tt, gtc(pre, ic, chan, ia, 1 if 'd' in fl else 0),
                'GTC ch %d %s <- %s%s%s' % (chan, p[2], p[3], ' data' if 'd' in fl else '', ' repeat' if k else ''))
    for m in moves:
        t, cont = m.split(':')
        put(float(t), move(prefix, 8191, int(cont)), 'MOVE %s' % cont)
    for cl in clears:
        t, ch, cont = cl.split(':')
        put(float(t), clear(int(ch), int(cont)), 'CLEAR %s' % ch)
    periodic = []
    for b in bcasts:
        p = [int(x, 0) for x in b.split(':')]
        periodic.append((bcast_chan(p[0], p[2] if len(p) > 2 else sys_id, p[1]), 'BCAST %s' % b))
    if maint_p:
        periodic.append((bcast_maint(sys_id, *maint_p), 'BCAST maint %s' % ':'.join(map(str, maint_p))))
    for k, (cw, label) in enumerate(periodic):
        t = 0.5 + 3.0 * k / max(1, len(periodic))
        while t < end:
            put(t, cw, label)
            t += 3.0
    bits = []
    for s in range(nslots):
        bits += c + bits64(slots[s])
    audio = ffsk_audio(bits)
    x = fm(audio * o['dev'], o['rate'])
    rate = o['rate']
    y = np.zeros(int(end * rate), np.complex128)
    for t, d in on:
        a0 = int(t * rate)
        seg = x[a0:a0 + int(d * rate)]
        ramp = np.minimum(1, np.minimum(np.arange(len(seg)), np.arange(len(seg))[::-1]) / (0.003 * rate))
        y[a0:a0 + len(seg)] = seg * ramp
        print('%8.3f .. %8.3f s  control channel SYS 0x%04X' % (t, t + len(seg) / rate, sys_id))
    truth = [m for m in sorted(truth, key=lambda m: m['t']) if any(t <= m['t'] < t + d for t, d in on)]
    for m in truth:
        print('%8.3f s  %s' % (m['t'], m['msg']))
    write(out, y, rate, {'sys': sys_id, 'messages': truth})


def speech(n, rng):
    """made-up speech-like audio (no real speech): syllables of gliding
    harmonic tones (100-250 Hz pitch, three formant-like bands) and bursts
    of band-limited noise, 300-3000 Hz, peak 1"""
    from scipy.signal import butter, lfilter
    out = np.zeros(n)
    i = 0
    while i < n:
        L = int(rng.uniform(0.08, 0.25) * FS)
        L = min(L, n - i)
        t = np.arange(L) / FS
        env = np.sin(np.pi * np.arange(L) / max(1, L)) ** 1.5
        if rng.random() < 0.75:
            f0 = rng.uniform(100, 250) * (1 + 0.15 * t / max(t[-1], 1e-3) * rng.choice([-1, 1]))
            ph = 2 * np.pi * np.cumsum(f0) / FS
            s = np.zeros(L)
            for fmt in (rng.uniform(300, 900), rng.uniform(900, 2000), rng.uniform(2000, 3000)):
                h = max(1, int(round(fmt / f0.mean())))
                s += np.sin(h * ph) / h ** 0.3
        else:
            s = rng.standard_normal(L)
        out[i:i + L] = s * env
        gap = int(rng.uniform(0.01, 0.12) * FS)
        i += L + gap
    b, a = butter(4, [300 / (FS / 2), 3000 / (FS / 2)], 'band')
    out = lfilter(b, a, out)
    return out / (np.max(np.abs(out)) + 1e-9)


def traffic_ch(out, o, items):
    rate, chan, cont = o['rate'], o['chan'], o['cont']
    rng = np.random.default_rng(o['seed'])
    msgs = []        # (time, [codewords], label)
    speech_spans = []
    its = []
    for it in items:
        p = it.split(':')
        t, d = float(p[0]), float(p[1])
        pu, iu = party(p[2], o['prefix'])
        pa, ia = (party(p[3], o['prefix']) if len(p) > 3 and '-' in p[3] else (pu, iu))
        disc = 'x' in p[3:]
        lost = 'o' in p[3:]
        its.append((t, d))
        msgs.append((t, [maint(pa, ia, chan, 0)], 'MAINT pressel on %03d-%04d' % (pa, ia)))
        if o['periodic'] > 0:
            tp = t + o['periodic']
            while tp < t + d - 0.3:
                msgs.append((tp, [maint(pa, ia, chan, 2)], 'MAINT periodic %03d-%04d' % (pa, ia)))
                tp += o['periodic']
        if not lost:
            msgs.append((t + d, [maint(pa, ia, chan, 1)], 'MAINT pressel off %03d-%04d' % (pa, ia)))
        if disc:
            msgs.append((t + d + 0.15, [maint(pa, ia, chan, 3)] * 2, 'MAINT disconnect x2 %03d-%04d' % (pa, ia)))
    if o['clear'] is not None:
        msgs.append((o['clear'], [clear(chan, cont)] * 2, 'CLEAR x2 ch %d -> %d' % (chan, cont)))
    key = o['key'] if o['key'] is not None else min(t for t, _ in its) - 0.2
    last = max([t + 0.2 + len(c) * 80 / 1200.0 for t, c, _ in msgs] + [t + d + 0.2 for t, d in its])
    end = last + 0.3
    n = int(end * FS)
    audio = np.zeros(n)
    burst = np.zeros(n, bool)
    truth = []
    for t, cws, label in sorted(msgs, key=lambda m: m[0]):
        # LET 12 bits (carrier, no modulation) + preamble 16 + (SYNT + codeword,
        # 16 bit reversals between messages) + hang-over bit
        bits = []
        pre = [1, 0] * 8
        for k, cw in enumerate(cws):
            bits += pre + bitsn(SYNT, 16) + bits64(cw)
        bits += [0]
        a0 = int(t * FS) + 12 * SPB
        seg = ffsk_audio(bits)
        a1 = min(n, a0 + len(seg))
        audio[a0:a1] = seg[:a1 - a0] * o['dev']
        burst[a0 - 12 * SPB - int(0.02 * FS):a1 + int(0.01 * FS)] = True   # speech muted around it (MPT 1343 4.2.3.1)
        truth.append({'t': round(t, 4), 'msg': label, 'start': round(a0 / FS, 4), 'end': round(a1 / FS, 4)})
    for t, d in its:
        a, b = int(t * FS), int((t + d) * FS)
        s = speech(b - a, rng) * o['sdev']
        audio[a:b] = np.where(burst[a:b], audio[a:b], s)
    carrier_end = (o['clear'] + 2 * 96 / 1200.0 + 0.05) if o['clear'] is not None else max(t + d for t, d in its) + 0.25
    x = fm(audio, rate, None)
    if o.get('uplink'):
        # the radios' own transmissions: a carrier per item (to its last message)
        spans = []
        for t, d in its:
            e = max([m['end'] for m in truth if t - 0.01 <= m['t'] <= t + d + 0.5] + [t + d]) + 0.02
            spans.append((t, e))
            print('%8.3f .. %8.3f s  uplink carrier' % (t, e))
        m = ramp_mask(len(x), [(int(a * rate), int(b * rate)) for a, b in spans], rate)
        carrier = spans
    else:
        m = ramp_mask(len(x), [(int(key * rate), int(carrier_end * rate))], rate)
        carrier = [round(key, 4), round(carrier_end, 4)]
        print('%8.3f .. %8.3f s  traffic channel %d downlink carrier' % (key, carrier_end, chan))
    y = x * m
    for e in truth:
        print('%8.3f s  %s' % (e['t'], e['msg']))
    for t, d in its:
        print('%8.3f .. %8.3f s  speech' % (t, t + d))
    write(out, y, rate, {'chan': chan, 'carrier': carrier, 'messages': truth,
                         'items': [[t, d] for t, d in its]})


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out, o, on, grants, bcasts, maint_p, moves, clears, items, traffic = parse(a)
    if traffic:
        if not items:
            raise SystemExit('--traffic needs at least one --item')
        traffic_ch(out, o, items)
    else:
        if not on:
            raise SystemExit('give --at T --dur S')
        control(out, o, on, grants, bcasts, maint_p, moves, clears)


if __name__ == '__main__':
    main()
