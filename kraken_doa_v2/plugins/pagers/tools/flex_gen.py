#!/usr/bin/env python3
"""FLEX pager test transmissions: frames of 1.875 s back to back, the real
format - sync 1 (32 bits 1010, the speed's A word, B, A inverted) and the
frame information word at 1600 bit/s 2-level, sync 2 (bit sync, C, both
inverted) and 11 blocks at the frame's speed: 1600/2, 3200/2, 3200/4 or
6400/4 (phases A; A + C; A + C; A + B + C + D), 8 BCH(31,21) + parity
codewords a phase a block, bit-interleaved, information LSB first. A phase:
block information word (+ date / time words), short or long addresses, a
vector word per address word, the message words, idle words (0).
Sources: the frame and message formats of Motorola's MC68175 data sheet
(appendix A / B), the sync patterns of the FLEX patent (as published by
Jelmer Bruijn); bit order and symbol mapping as seen in a real 6400/4
recording - binary 1 = the LOWER frequency, 4-level: the first bit the
sign, the second 1 = an inner level (+-1.6 kHz).

  flex_gen.py OUT.cf32 [--rate 48000] [--dev 4800] [--offset HZ] [--bt 0.5]
              [--invert] [--start CYCLE FRAME] [--frames N] [--idle-mode MODE]
              [--time "YYYY-MM-DD HH:MM:SS GMT+HHMM [DST]"]
              --page FRAME MODE CAPCODE TYPE TEXT [--page ...]

--page: FRAME = the frame in this file (0 = the first), MODE = 1600/2,
3200/2, 3200/4 or 6400/4 (every page of a frame in its mode), CAPCODE =
1 - 2031614 (one address word) or a long address (2101249 - 4297068542, two
words), TYPE = alpha | numeric | tone | short | binary | secure |
group:A:FRAME2 (a short instruction: listen to temporary address A, 0-15, =
capcode 2029568 + A, in this file's frame FRAME2). alpha / secure TEXT with
'|' goes out in fragments, one a frame from FRAME on; numeric: 0-9, space,
-, U, [, ], * (spare); tone: TEXT = the source 0-7; short: 3 digits (8 for
a long address); binary: hex digits. A page goes in phase (capcode / 4) mod
the frame's phases. --time adds the date / time block information words to
the first frame. One transmitter per file: complex baseband at --rate
(cf32) + OUT.json {rate}; place transmitters at bearings with
plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

BCH_G = 0x769          # x^10 + x^9 + x^8 + x^6 + x^5 + x^3 + 1
FRAME_S = 1.875
# sync 1 A words (first sent = MSB), data symbol rate, levels, phases
MODES = {'1600/2': (0x78F35939, 1600, 2, 1), '3200/2': (0x84E75939, 3200, 2, 2),
         '3200/4': (0x4F975939, 1600, 4, 2), '6400/4': (0x215F5939, 3200, 4, 4)}
SYNC1_B = 0x5555
SYNC2_C = 0xED84
TEMP0 = 0x1F7800
NUMERIC = {c: i for i, c in enumerate('0123456789*U -][')}


def rev(v, n):
    return int(format(v, '0%db' % n)[::-1], 2)


def codeword(info):
    """21 information bits -> the 32 bits as sent: information LSB first, the
    10 check bits, even parity (POCSAG's codeword with every field reversed)"""
    d = rev(info & 0x1FFFFF, 21)
    c = d << 10
    for i in range(30, 9, -1):
        if c >> i & 1:
            c ^= BCH_G << (i - 10)
    cw31 = (d << 10) | (c & 0x3FF)
    cw = (cw31 << 1) | (bin(cw31).count('1') & 1)
    return [(cw >> (31 - k)) & 1 for k in range(32)]


def with_sum(w):
    """the 4-bit check of block information / frame information / vector words"""
    s = sum((w >> k) & 0xF for k in (4, 8, 12, 16)) + ((w >> 20) & 1)
    return w | ((0xF - s) & 0xF)


def group_sum(w):
    return (w & 0xFF) + ((w >> 8) & 0xFF) + ((w >> 16) & 0x1F)


def address_words(cap):
    """CAPCODE -> its address word(s) (MC68175 'CAPCODE to binary conversion')"""
    if 1 <= cap <= 2031614:
        return [cap + 32768]
    if 2101249 <= cap <= 1075843072:                 # long address set 1-2
        x = cap - 2068481
        return [x % 32768 + 1, 2097151 - x // 32768]
    if 1075843073 <= cap <= 3223326720:              # sets 1-3, 1-4
        x = cap - 2068481
        return [x % 32768 + 1, x // 32768 + 1933312]
    if 3223326721 <= cap <= 4297068542:              # set 2-3
        x = cap - 2068479
        return [x % 32768 + 2064383, x // 32768 + 1867776]
    sys.exit('capcode %d is not a FLEX address' % cap)


def pack(bits):
    """bits -> 21-bit words, LSB first"""
    words = []
    for i in range(0, len(bits), 21):
        w = 0
        for k, b in enumerate(bits[i:i + 21]):
            w |= b << k
        words.append(w)
    return words


def alpha_fragments(text, kind):
    """alpha (V=5) / secure (V=0) message -> fragments: (vector type, words)"""
    parts = text.split('|')
    total = ''.join(parts)
    sig = (~sum(ord(c) & 0x7F for c in total)) & 0x7F
    frags = []
    for i, part in enumerate(parts):
        last = i == len(parts) - 1
        F = 3 if i == 0 else (i - 1) % 3
        C = 0 if last else 1
        fill = 0x03 if last else 0x00               # ETX ends the message, NUL a fragment
        chars = [ord(c) & 0x7F for c in part]
        if kind == 'alpha' and i == 0:
            chars = [sig] + chars                   # the signature: first slot of word 2
        while len(chars) % 3:
            chars.append(fill)
        body = [chars[k] | chars[k + 1] << 7 | chars[k + 2] << 14 for k in range(0, len(chars), 3)]
        hdr = C << 10 | F << 11                    # N = 0, R = 0, M = 0 (secure: spares 0)
        s = sum(group_sum(w) for w in [hdr] + body)
        hdr |= (~s) & 0x3FF
        frags.append((5 if kind == 'alpha' else 0, [hdr] + body))
    return frags


def numeric_words(text):
    """standard numeric (V=3): K4 K5, then 4-bit digits LSB first, space fill"""
    digits = [NUMERIC[c] for c in text]
    nbits = 2 + 4 * len(digits)
    nw = (nbits + 20) // 21
    if nw > 8:
        sys.exit('numeric page too long: %r' % text)
    while 2 + 4 * (len(digits) + 1) <= 21 * nw:
        digits.append(0xC)
    bits = [0, 0]
    for d in digits:
        bits += [(d >> k) & 1 for k in range(4)]
    bits += [0] * (21 * nw - len(bits))
    words = pack(bits)
    s = sum(group_sum(w) for w in words) & 0xFF
    s = (s & 0x3F) + (s >> 6)
    k = (~s) & 0x3F
    words[0] |= k >> 4                              # K4 K5
    return words, k & 0xF                           # K0-K3 go in the vector


def binary_words(hexs):
    """hex / binary (V=6): K C F N, then R M D H B s S (B = 1: binary), data, fill"""
    bits = []
    for c in hexs:
        v = int(c, 16)
        bits += [(v >> k) & 1 for k in range(4)]
    data_bytes = [sum(bits[i + k] << k for k in range(8) if i + k < len(bits)) for i in range(0, len(bits), 8)]
    sig = (~sum(data_bytes)) & 0xFF
    fill = 1 - bits[-1]                             # the opposite of the last data bit
    nfull = (len(bits) + 20) // 21
    if len(bits) == 21 * nfull:
        nfull += 1                                  # exactly full: one more word of fill
    bits += [fill] * (21 * nfull - len(bits))
    w2 = 1 << 4 | sig << 13                         # B = 0001: 1 bit a character
    hdr = 0 << 12 | 3 << 13                         # C = 0, F = 3, N = 0
    words = [hdr, w2] + pack(bits)
    s = sum(group_sum(w) for w in words)
    words[0] |= (~s) & 0xFFF
    return words


def page_parts(frame, mode, cap, kind, text, start_cycle, start_frame):
    """-> [(frame, mode, address words, vector maker, words)] - one per fragment"""
    aw = address_words(cap)
    long_addr = len(aw) == 2
    out = []
    if kind in ('alpha', 'secure'):
        for i, (v, words) in enumerate(alpha_fragments(text, kind)):
            out.append((frame + i, mode, aw, ('msg', v, None), words))
    elif kind == 'numeric':
        words, k03 = numeric_words(text)
        out.append((frame, mode, aw, ('num', 3, k03), words))
    elif kind == 'binary':
        out.append((frame, mode, aw, ('msg', 6, None), binary_words(text)))
    elif kind == 'tone':
        s = int(text) & 7
        out.append((frame, mode, aw, ('fixed', with_sum(2 << 4 | 1 << 7 | s << 9), 0), []))
    elif kind == 'short':
        d = [NUMERIC[c] for c in text.ljust(8 if long_addr else 3)]
        dv = d[0] | d[1] << 4 | d[2] << 8
        extra = sum(x << (4 * k) for k, x in enumerate(d[3:8])) if long_addr else 0
        out.append((frame, mode, aw, ('fixed', with_sum(2 << 4 | 0 << 7 | dv << 9), extra), []))
    elif kind.startswith('group:'):
        _, a, f2 = kind.split(':')
        fnum = (start_frame + int(f2)) % 128
        out.append((frame, mode, aw, ('fixed', with_sum(1 << 4 | 0 << 7 | (fnum | int(a) << 7) << 10), 0), []))
    else:
        sys.exit('unknown page type %r' % kind)
    return out


def build_phase(pages, ext):
    """[(address words, vector maker, words)] -> the phase's 88 information words"""
    ai = 1 + len(ext)
    naddr = sum(len(aw) for aw, _, _ in pages)
    vs = ai + naddr
    words = [0] * 88
    words[0] = with_sum(len(ext) << 8 | vs << 10)
    for i, e in enumerate(ext):
        words[1 + i] = e
    msg = vs + naddr                                # the message field after the vectors
    a = ai
    for aw, (how, v, extra), mw in pages:
        vi = vs + (a - ai)
        for k, w in enumerate(aw):
            words[a + k] = w
        long_addr = len(aw) == 2
        if how == 'fixed':
            words[vi] = v
            if long_addr:
                words[vi + 1] = extra
        else:
            field = mw[1:] if long_addr else mw
            if long_addr:
                words[vi + 1] = mw[0]               # a long address: its first message word
            b = msg if field else 0
            n = len(mw)
            if how == 'num':
                vec = 3 << 4 | b << 7 | (n - 1) << 14 | extra << 17
            else:
                vec = v << 4 | b << 7 | n << 14
            words[vi] = with_sum(vec)
            for w in field:
                if msg >= 88:
                    sys.exit('frame phase full')
                words[msg] = w
                msg += 1
        a += len(aw)
    return words


def time_biws(spec):
    """'YYYY-MM-DD HH:MM:SS GMT+HHMM [DST]' (the zone's standard time offset) ->
    date, time, system message block information words"""
    date, tim, zone = spec.split()[:3]
    dst = spec.split()[3:] == ['DST']
    Y, M, D = (int(x) for x in date.split('-'))
    h, m, s = (int(x) for x in tim.split(':'))
    sign = -1 if zone[3] == '-' else 1
    mins = sign * (int(zone[4:6]) * 60 + int(zone[6:8]))
    zones = [0, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660, 720, 210, 270, 330, None, 345, 390, 570,
             -210, -660, -600, -540, -480, -420, -360, -300, -240, -180, -120, -60]
    if mins not in zones:
        sys.exit('%s is not a FLEX time zone (give the standard time offset + DST)' % zone)
    z = zones.index(mins)
    w1 = with_sum(1 << 4 | ((Y - 1994) % 32 | D << 5 | M << 10) << 7)
    w2 = with_sum(2 << 4 | (h | m << 5 | (s * 8 // 60) << 11) << 7)
    acc = int(round((s - (s * 8 // 60) * 7.5) * 64 / 60)) & 7
    w3 = with_sum(5 << 4 | (0b0100 | z << 4 | (0 if dst else 1) << 9 | acc << 11) << 7)   # L = 1: standard time
    return [w1, w2, w3]


def frame_symbols(mode, cycle, fnum, phases_words):
    """-> [(baud, frequency in units of the deviation)] for one frame"""
    a_word, baud, levels, nph = MODES[mode]
    syms = []
    bits1 = [1, 0] * 16 + [(a_word >> (31 - k)) & 1 for k in range(32)] + \
            [(SYNC1_B >> (15 - k)) & 1 for k in range(16)] + [1 - ((a_word >> (31 - k)) & 1) for k in range(32)]
    fiw = with_sum(cycle << 4 | fnum << 8)
    bits1 += codeword(fiw)
    syms += [(1600, -1.0 if b else 1.0) for b in bits1]

    def lvl(a, b):
        return (-1.0 if a else 1.0) * (1.0 / 3 if b else 1.0)
    total = baud // 40
    if levels == 2:
        nbs = total // 2 - 16
        s2 = [(k + 1) & 1 for k in range(nbs)] + [(SYNC2_C >> (15 - k)) & 1 for k in range(16)] + \
             [k & 1 for k in range(nbs)] + [1 - ((SYNC2_C >> (15 - k)) & 1) for k in range(16)]
        syms += [(baud, -1.0 if b else 1.0) for b in s2]
    else:
        nbs = total // 2 - 8
        cpairs = [((SYNC2_C >> (15 - 2 * j)) & 1, (SYNC2_C >> (14 - 2 * j)) & 1) for j in range(8)]
        s2 = [((k + 1) & 1, 0) for k in range(nbs)] + cpairs + [(k & 1, 0) for k in range(nbs)] + \
             [(1 - a, 1 - b) for a, b in cpairs]
        syms += [(baud, lvl(a, b)) for a, b in s2]
    # the blocks: per phase 8 codewords bit-interleaved, the phases multiplexed
    for blk in range(11):
        streams = []
        for p in range(nph):
            cws = [codeword(phases_words[p][blk * 8 + w]) for w in range(8)]
            streams.append([cws[j % 8][j // 8] for j in range(256)])
        if mode == '1600/2':
            syms += [(baud, -1.0 if b else 1.0) for b in streams[0]]
        elif mode == '3200/2':
            for j in range(256):
                syms += [(baud, -1.0 if streams[0][j] else 1.0), (baud, -1.0 if streams[1][j] else 1.0)]
        elif mode == '3200/4':
            syms += [(baud, lvl(streams[0][j], streams[1][j])) for j in range(256)]
        else:
            for j in range(256):
                syms += [(baud, lvl(streams[0][j], streams[1][j])), (baud, lvl(streams[2][j], streams[3][j]))]
    return syms


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, dev, offset, bt, invert = 48000.0, 4800.0, 0.0, 0.5, False
    start_cycle, start_frame, nframes, idle_mode, tspec = 0, 0, None, '1600/2', None
    pages = []
    i = 1
    while i < len(a):
        o = a[i]
        if o == '--rate':
            rate = float(a[i + 1]); i += 2
        elif o == '--dev':
            dev = float(a[i + 1]); i += 2
        elif o == '--offset':
            offset = float(a[i + 1]); i += 2
        elif o == '--bt':
            bt = float(a[i + 1]); i += 2
        elif o == '--invert':
            invert = True; i += 1
        elif o == '--start':
            start_cycle, start_frame = int(a[i + 1]), int(a[i + 2]); i += 3
        elif o == '--frames':
            nframes = int(a[i + 1]); i += 2
        elif o == '--idle-mode':
            idle_mode = a[i + 1]; i += 2
        elif o == '--time':
            tspec = a[i + 1]; i += 2
        elif o == '--page':
            pages.append((int(a[i + 1]), a[i + 2], int(a[i + 3]), a[i + 4], a[i + 5])); i += 6
        else:
            print('unknown option', o); sys.exit(2)
    parts = []
    for frame, mode, cap, kind, text in pages:
        if mode not in MODES:
            sys.exit('unknown mode %r' % mode)
        parts += page_parts(frame, mode, cap, kind, text, start_cycle, start_frame)
        print('frame %3d  %s  capcode %d %s: %s' % (frame, mode, cap, kind, text))
    if nframes is None:
        nframes = max((p[0] for p in parts), default=0) + 1
    # frame by frame: its mode (the pages', else the previous frame's), phases
    syms_t = []        # (start time, baud, value)
    t = 0.05
    mode = idle_mode
    for f in range(nframes):
        mine = [p for p in parts if p[0] == f]
        modes = {p[1] for p in mine}
        if len(modes) > 1:
            sys.exit('frame %d: pages in different modes' % f)
        if modes:
            mode = modes.pop()
        nph = MODES[mode][3]
        per_phase = [[] for _ in range(nph)]
        for _, _, aw, vec, words in mine:
            cap_word = aw[0]
            per_phase[((cap_word - 32768) >> 2) % nph if len(aw) == 1 else 0].append((aw, vec, words))
        ext = time_biws(tspec) if (tspec and f == 0) else []
        phases_words = [build_phase(pp, ext) for pp in per_phase]
        fn = start_frame + f
        cycle = (start_cycle + fn // 128) % 15
        tt = t
        for baud, v in frame_symbols(mode, cycle, fn % 128, phases_words):
            syms_t.append((tt, v))
            tt += 1.0 / baud
        t += FRAME_S
        print('frame %3d  %s  cycle %d frame %d  (%.3f s)' % (f, mode, cycle, fn % 128, t - FRAME_S))
    end = t + 0.05
    n = int(end * rate)
    times = np.array([s[0] for s in syms_t])
    vals = np.array([s[1] for s in syms_t])
    ts = np.arange(n) / rate
    k = np.searchsorted(times, ts, side='right') - 1
    on = (k >= 0) & (ts < t)
    f = np.where(on, vals[np.clip(k, 0, len(vals) - 1)], 0.0) * dev * (-1 if invert else 1)
    if bt > 0:
        sps = rate / 3200
        sigma = np.sqrt(np.log(2)) / (2 * np.pi * bt) * sps
        h = np.exp(-0.5 * (np.arange(-int(3 * sigma) - 1, int(3 * sigma) + 2) / sigma) ** 2)
        f = np.convolve(f, h / h.sum(), mode='same')
    x = np.exp(2j * np.pi * np.cumsum(f + offset) / rate)
    # keyed up just before the first frame, down after the last
    ramp = np.zeros(n)
    k0, k1 = int(0.03 * rate), int((t + 0.02) * rate)
    ramp[k0:k1] = 1.0
    r = int(0.002 * rate)
    ramp[k0:k0 + r] = np.linspace(0, 1, r)
    ramp[k1 - r:k1] = np.linspace(1, 0, r)
    (x * ramp).astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
