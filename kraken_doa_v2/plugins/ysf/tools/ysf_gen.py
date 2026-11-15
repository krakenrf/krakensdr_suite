#!/usr/bin/env python3
"""Yaesu System Fusion (C4FM FDMA) test transmissions, written from Yaesu's
"Amateur Radio Digital Standards" v1.02 (2015-07-29; section numbers below
are that document's): C4FM 4800 symbols/s (9600 bit/s), root raised cosine
alpha 0.2, +-900 / +-2700 Hz (wide) or +-450 / +-1350 Hz (narrow), 100 ms
frames of 960 bits. A call as on the air: one Header Channel frame (HC),
Communication Channel frames (CC) with the FICH frame counters, one
Terminator Channel frame (TC). Every frame: frame sync, FICH (CRC-16,
Golay(24,12), K=5 rate-1/2 code, dibit interleave), DCH units (PN(9,5)
whitening, CRC-16, K=5 code, interleave) and the voice channels.

  ysf_gen.py OUT.cf32 [--rate 48000] [--seed 7]
             --at T --dur S --src CALL [--dst CALL] [--mode dn|vw|vd1]
             [--dgid N] [--down CALL --up CALL] [--radioid XXXXX]
             [--mr 0] [--voip 0] [--rptid XXXXX] [--cm 0|1|3] [--dstid XXXXX]
             [--dev wide|narrow] [--text "..."] [--at ... (next call)]

  --mode dn   V/D mode type 2 (DT 10, sect. 4.2.2) - what Yaesu radios call DN
              (default): 5 x [DCH 40 | VCH 72 | VeCH 32]; DCH rolls Dest, Src,
              Down, Up, Rem1+2, Rem3+4 (FT 5), + DT1 / DT2 with --text (FT 6 / 7)
  --mode vw   Voice FR mode (DT 11, sect. 4.4): CC0 sub-header (FT 1 / FN 0:
              CSD3 + 2 full-rate voice frames), FT 1 / FN 1, then FT 0 / FN 0
              frames of 5 x 144-bit full-rate voice frames
  --mode vd1  V/D mode type 1 (DT 00, sect. 4.2.1): 5 x [DCH 72 | VCH 72]; DCH
              rolls CSD1, CSD2, CSD3 (FT 2), + DT1..DT5 with --text (FT 3..7)
  --dgid N    FICH squelch code SC = N (1..126; SQL type 0 = code enabled);
              0 = no code (SC 0, SQL type 1 = disabled) - the default
  --dst       destination callsign; default "ALL" + 7 spaces (the standard
              defines no "all stations" form; this is an assumption)
  --down/--up CSD2 (repeater downlink / uplink callsigns); spaces by default
  --radioid   the source's 5-byte Radio ID (CSD3 Rem4; Dest area with --cm 1)
  --rptid     the repeater's Radio ID in Rem1 and Rem2 (sect. 4.2.1 (5))
  --mr --voip FICH message routing (0 direct, 1 / 2 downlink) and VoIP bit
  --cm        FICH call mode: 0 group / CQ, 1 radio ID (Dest area = --dstid +
              --radioid), 3 individual
  --text      DT data (V/D modes only; Voice FR mode carries no text)
Per-call options given before the first --at are defaults for every call.

Voice: the standard puts DVSI AMBE+2 frames into the voice channels but does
not define their bits, so they are seeded random bits: V/D 2 - 49 bits per
20 ms, coded as sect. 4.5 (3) specifies (27 bits x3 + 22 + 1 zero, PN(9,5)
whitening, 4 x 26 interleave -> VCH 72 + VeCH 32); V/D 1 - the 72-bit VCH
(49 + 23 vocoder FEC) as random bits; Voice FR - the 144-bit VCH (88 + 56)
as random bits. OUT.json has every frame's FICH word, DCH unit and voice
bits, so a decoder's output can be compared bit for bit.

One radio's calls as complex baseband (cf32) + OUT.json. Put several radios
at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

RATE = 48000.0
SPS = 10                                 # 48 kHz / 4800 symbols/s
FS = 0xD471C9634D                        # 4.2.1 (3): frame sync, 40 bits
LEVEL = {(0, 0): 1, (0, 1): 3, (1, 0): -1, (1, 1): -3}   # 3.2 (2) Table 3-1
DEV_HZ = {'wide': 900.0, 'narrow': 450.0}                # Hz per symbol unit
FI = {'hc': 0b00, 'cc': 0b01, 'tc': 0b10}                # Table 4-1 frame type
DT = {'vd1': 0b00, 'dn': 0b10, 'vw': 0b11}               # Table 4-1 data type
# Appendix A: Golay(24,12,8) generator matrix [I | P]; row k multiplies d(11-k)
GOLAY_P = ['110001110101', '011000111011', '111101101000', '011110110100',
           '001111011010', '110110011001', '011011001101', '001101100111',
           '110111000110', '101010010111', '100100111110', '100011101011']


def u(v, n):
    """Integer -> n bits, most significant first."""
    return [(v >> (n - 1 - i)) & 1 for i in range(n)]


def text_bits(s, n):
    """ASCII text, space-padded to n bytes, each byte MSB first."""
    b = s.encode('ascii')
    if len(b) > n:
        raise SystemExit('"%s" is longer than %d characters' % (s, n))
    out = []
    for x in b.ljust(n, b' '):
        out += u(x, 8)
    return out


def bits_hex(bits):
    return '%0*X' % ((len(bits) + 3) // 4, int(''.join(map(str, bits)), 2) << (-len(bits) % 4))


def crc16(bits):
    # 4.5 (1)(b) / (2)(c), Fig. 4-25: x^16+x^12+x^5+1, registers S15..S00 = 0,
    # feedback S15 ^ input into S00, S05, S12; all bits inverted at the end;
    # sent S15 first
    crc = 0
    for b in bits:
        fb = ((crc >> 15) & 1) ^ b
        crc = (crc << 1) & 0xFFFF
        if fb:
            crc ^= 0x1021
    return u(crc ^ 0xFFFF, 16)


def golay24(d):
    # Appendix A: txd = d11..d0 p11..p0, d11 = the segment's first bit
    p = [0] * 12
    for k in range(12):
        if d[k]:
            p = [a ^ int(c) for a, c in zip(p, GOLAY_P[k])]
    return list(d) + p


def conv(bits):
    # Appendix B: K=5, G1 = 1+x^3+x^4, G2 = 1+x+x^2+x^4 (x^k = k-th delay),
    # registers 0 at the start; output G1 then G2 for every input bit.
    # The caller has appended the 4 zero tail bits.
    out, reg = [], 0
    for b in bits:
        reg = ((reg << 1) | b) & 0x1F      # bit k = the input k bits ago
        out += [bin(reg & 0b11001).count('1') & 1, bin(reg & 0b10111).count('1') & 1]
    return out


def interleave_dibits(bits, cols):
    # Figs. 4-26 / 4-30 / 4-31: dibits S00.. written row by row into rows of
    # `cols` dibits (20 rows), read column by column: S00 S{cols} S{2 cols} ..
    d = [bits[2 * i:2 * i + 2] for i in range(len(bits) // 2)]
    rows = len(d) // cols
    out = []
    for c in range(cols):
        for r in range(rows):
            out += d[r * cols + c]
    return out


def pn95(n):
    # 4.5 (2)(b), Fig. 4-28: PN(9,5), S08..S00 = 1 1 1 0 0 1 0 0 1, output S00,
    # shift towards S00, S08 <- S00 ^ S04; restarted for every unit
    s = [1, 0, 0, 1, 0, 0, 1, 1, 1]        # s[k] = S0k
    out = []
    for _ in range(n):
        out.append(s[0])
        fb = s[0] ^ s[4]
        s = s[1:] + [fb]
    return out


def fich(f):
    # 4.2.1 (4) Fig. 4-9: the 32-bit FICH word, bit 31 first
    w = (f['fi'] << 30) | (0b10 << 28) | (f['cm'] << 26) | (0 << 24) | (0 << 22) \
        | (f['fn'] << 19) | (f['ft'] << 16) | (0 << 15) | (f['dev'] << 14) | (f['mr'] << 11) \
        | (f['voip'] << 10) | (f['dt'] << 8) | (f['sql'] << 7) | f['sc']
    return w


def fich_encode(w):
    # 4.5 (1), Fig. 4-26: 32 + CRC16 = 48 -> 4 x Golay(24,12) = 96 -> +4 zeros
    # -> K=5 code 200 -> 100 dibits, 20 rows x 5 dibits, read by column
    b = u(w, 32)
    b = b + crc16(b)
    g = []
    for k in range(4):
        g += golay24(b[12 * k:12 * k + 12])
    return interleave_dibits(conv(g + [0, 0, 0, 0]), 5)


def dch_encode(unit):
    # 4.5 (2), Fig. 4-27: whitening -> CRC16 -> +4 zeros -> K=5 code ->
    # interleave. 160-bit unit (Fig. 4-30): 360 bits, 20 rows x 9 dibits;
    # 80-bit unit (Fig. 4-31, V/D type 2 CC): 200 bits, 20 rows x 5 dibits
    w = [a ^ b for a, b in zip(unit, pn95(len(unit)))]
    c = conv(w + crc16(w) + [0, 0, 0, 0])
    return interleave_dibits(c, 9 if len(unit) == 160 else 5)


def vd2_voice(v49):
    # 4.5 (3), Fig. 4-32: 27 + 22 bits; each of the 27 sent 3 times (81), + 22,
    # + one 0 = 104; PN(9,5) whitening (restarted per 20 ms frame); 4 rows x 26
    # bits read by column: S0 S26 S52 S78 S1 ...; first 72 -> VCH, last 32 -> VeCH
    s = [b for b in v49[:27] for _ in range(3)] + list(v49[27:]) + [0]
    s = [a ^ b for a, b in zip(s, pn95(104))]
    out = [s[(k % 4) * 26 + k // 4] for k in range(104)]
    return out[:72], out[72:]


def frame_syms(fich_w, payload):
    bits = u(FS, 40) + fich_encode(fich_w) + payload
    assert len(bits) == 960, len(bits)
    return [LEVEL[(bits[2 * i], bits[2 * i + 1])] for i in range(480)]


def call(c, rng):
    """One call -> (symbols, frame log for the sidecar)."""
    mode = c['mode']
    radioid = text_bits(c['radioid'], 5)
    dest = text_bits(c['dstid'], 5) + radioid if c['cm'] == 1 else text_bits(c['dst'], 10)
    src = text_bits(c['src'], 10)
    down, up = text_bits(c['down'], 10), text_bits(c['up'], 10)
    rpt = text_bits(c['rptid'], 5)
    rem = rpt + rpt + text_bits('', 5) + radioid          # Rem1..Rem4 (4.2.1 (5))
    csd1, csd2, csd3 = dest + src, down + up, rem
    dgid = c['dgid']
    base = {'cm': c['cm'], 'dev': 1 if c['dev'] == 'narrow' else 0, 'mr': c['mr'],
            'voip': c['voip'], 'dt': DT[mode], 'sql': 0 if dgid else 1, 'sc': dgid}
    text = c['text'].encode('ascii')

    # the DCH rolling (Tables 4-3 / 4-4); FT = the last FN
    if mode == 'dn':
        if len(text) > 20:
            raise SystemExit('V/D mode type 2 carries at most 20 bytes of text')
        units = [dest, src, down, up, rem[:80], rem[80:]]
        units += [text_bits(text[10 * k:10 * k + 10].decode(), 10) for k in range((len(text) + 9) // 10)]
    elif mode == 'vd1':
        if len(text) > 100:
            raise SystemExit('V/D mode type 1 carries at most 100 bytes of text')
        units = [csd1, csd2, csd3]
        units += [text_bits(text[20 * k:20 * k + 20].decode(), 20) for k in range((len(text) + 19) // 20)]
    elif text:
        raise SystemExit('Voice FR mode carries no text (4.4 (6))')

    log, syms = [], []

    def emit(kind, fn, ft, payload, unit=None, voice=None):
        w = fich(dict(base, fi=FI[kind], fn=fn, ft=ft))
        log.append({'sym': len(syms), 'fi': kind, 'fn': fn, 'ft': ft, 'fich': '%08X' % w,
                    'dch': bits_hex(unit) if unit is not None else None,
                    'voice': [''.join(map(str, v)) for v in voice] if voice else []})
        syms.extend(frame_syms(w, payload))

    def hc_tc(kind):
        # 4.2.1 (1) Fig. 4-7: DCH-1 = CSD1, DCH-2 = CSD2, 72-bit pieces alternating
        d1, d2 = dch_encode(csd1), dch_encode(csd2)
        p = []
        for i in range(5):
            p += d1[72 * i:72 * i + 72] + d2[72 * i:72 * i + 72]
        emit(kind, 0, 0, p, unit=csd1 + csd2)

    hc_tc('hc')
    n = int(round(c['dur'] / 0.1))
    if mode == 'vw':
        n = max(2, n)
        for j in range(n):
            if j == 0:
                # 4.4 (2) Fig. 4-21: CC0 sub header, FT 1 / FN 0: CSD3 in DCH(0..4),
                # 72 reserved bits (sent as zeros), VCH(3), VCH(4)
                v = [list(rng.integers(0, 2, 144)) for _ in range(2)]
                emit('cc', 0, 1, dch_encode(csd3) + [0] * 72 + v[0] + v[1], unit=csd3, voice=v)
            else:
                # Fig. 4-22: 5 x 144-bit full-rate VCH; FT 1 / FN 1 once, then FT 0 / FN 0
                v = [list(rng.integers(0, 2, 144)) for _ in range(5)]
                emit('cc', 1 if j == 1 else 0, 1 if j == 1 else 0, sum(v, []), voice=v)
    else:
        ft = len(units) - 1
        n = max(ft + 1, n)
        for j in range(n):
            fn = j % (ft + 1)
            d = dch_encode(units[fn])
            p, v = [], []
            for i in range(5):
                if mode == 'dn':
                    # 4.2.2 (2) Fig. 4-12: DCH(i) 40 | VCH(i) 72 | VeCH(i) 32
                    v49 = list(rng.integers(0, 2, 49))
                    vch, vech = vd2_voice(v49)
                    p += d[40 * i:40 * i + 40] + vch + vech
                    v.append(v49)
                else:
                    # 4.2.1 (2) Fig. 4-8: DCH(i) 72 | VCH(i) 72
                    v72 = list(rng.integers(0, 2, 72))
                    p += d[72 * i:72 * i + 72] + v72
                    v.append(v72)
            emit('cc', fn, ft, p, unit=units[fn], voice=v)
    hc_tc('tc')
    return syms, log


def rrc(sps, span, alpha):
    # 3.2 (3): |H(f)| = 1 / cos[(T/4a)(2 pi |f| - pi (1-a)/T)] / 0, T = 1/4800,
    # a = 0.2 - a root raised cosine; its impulse response:
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


PAD = 16  # symbols of unmodulated carrier before / after a call (filter tails)


def modulate(syms, dev_hz, rate):
    # 3.2 (1) Fig. 3-1: symbols -> transmit filter H(f) -> FM. The filter is
    # scaled to unit gain per symbol (a run of +1 symbols = +dev_hz), so after
    # the receiver's matching RRC (3.2 (4)) the symbol centres sit at
    # +-1 / +-3 x dev_hz
    up = np.zeros((len(syms) + 2 * PAD) * SPS)
    up[PAD * SPS:(PAD + len(syms)) * SPS:SPS] = syms
    h = rrc(SPS, 16, 0.2)
    f = np.convolve(up, h * SPS / h.sum(), 'same') * dev_hz
    x = np.exp(2j * np.pi * np.cumsum(f) / RATE)
    if abs(rate - RATE) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / RATE).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    return x


CALL_KEYS = {'dur': float, 'src': str, 'dst': str, 'mode': str, 'dgid': int, 'down': str, 'up': str,
             'radioid': str, 'rptid': str, 'mr': int, 'voip': int, 'cm': int, 'dstid': str, 'dev': str,
             'text': str}


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-') or len(a) % 2 == 0:
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, seed = RATE, 7
    defaults = {'dur': 3.0, 'src': 'N0CALL', 'dst': 'ALL', 'mode': 'dn', 'dgid': 0, 'down': '', 'up': '',
                'radioid': '', 'rptid': '', 'mr': 0, 'voip': 0, 'cm': 0, 'dstid': '', 'dev': 'wide', 'text': ''}
    txs, cur = [], defaults
    for i in range(1, len(a), 2):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'rate':
            rate = float(v)
        elif k == 'seed':
            seed = int(v)
        elif k == 'at':
            cur = dict(at=float(v), **defaults)
            txs.append(cur)
        elif k in CALL_KEYS:
            cur[k] = CALL_KEYS[k](v)
        else:
            raise SystemExit('unknown option --' + k)
    for c in txs:
        if c['mode'] not in DT or c['dev'] not in DEV_HZ or not 0 <= c['dgid'] <= 127 \
                or c['cm'] not in (0, 1, 3) or not 0 <= c['mr'] <= 7 or c['voip'] not in (0, 1):
            raise SystemExit('bad call options: %r' % c)
    if not txs:
        raise SystemExit('no call (--at T ...)')
    rng = np.random.default_rng(seed)
    parts, calls = [], []
    for c in txs:
        syms, log = call(c, rng)
        y = modulate(syms, DEV_HZ[c['dev']], rate)
        parts.append((c, y))
        for f in log:   # the frame sync's first symbol centre, s from the file start
            f['t'] = round(c['at'] + (PAD + f.pop('sym')) / 4800.0, 6)
        calls.append(dict(c, end=round(c['at'] + len(y) / rate, 6), frames=log))
        print('%8.3f .. %8.3f s  %-10s -> %-10s  %s  DG-ID %d  %d frames' % (
            c['at'], c['at'] + len(y) / rate, c['src'], c['dst'], c['mode'], c['dgid'], len(log)))
    end = max(c['at'] + len(y) / rate for c, y in parts) + 0.3
    x = np.zeros(int(end * rate), np.complex128)
    for c, y in parts:
        n0 = int(round(c['at'] * rate))
        x[n0:n0 + len(y)] += y
    x.astype(np.complex64).tofile(out)
    with open(out.rsplit('.', 1)[0] + '.json', 'w') as fh:
        json.dump({'rate': rate, 'seed': seed, 'calls': calls}, fh, indent=1)


if __name__ == '__main__':
    main()
