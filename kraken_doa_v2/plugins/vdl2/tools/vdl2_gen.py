#!/usr/bin/env python3
"""VDL Mode 2 test transmissions (ICAO Annex 10 Vol III 6.4): D8PSK at 10 500
symbols/s, raised cosine alpha 0.6, ramp-up + unique word, scrambled header
(length + (25,20) FEC) and data: AVLC frames (HDLC flags / stuffing / FCS)
in rows of 249 octets with RS(255,249) check octets, interleaved.

  vdl2_gen.py OUT.cf32 [--rate 48000] [--offset HZ] [--clock-ppm PPM]
                       [--invert] --at T SPEC [--at T SPEC]...

One transmitter's bursts at the given times (s), complex baseband at --rate
(cf32) + OUT.json {rate}. SPEC, several frames in one burst joined by '+':
  aoa:SRC>DST:KIND:REG:LABEL:BID:TEXT   ACARS over AVLC (an I frame); KIND
                                       down / up (as acars_gen.py)
  xid:GS:ICAO[,ICAO]:LAT:LON           a ground station's XID (GSIF) to all
                                       aircraft: airport coverage, location
  rr:SRC>DST                           a receive-ready (link control)
  data:SRC>DST:NBYTES                  an I frame of NBYTES (an X.25 packet)
Addresses: A<hex6> aircraft (its ICAO address), G<hex6> ground station,
* = all aircraft. Put several transmitters at their own bearings with
plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

RS_SYM = 10500.0
SPS = 10                        # modulation at 105 kHz
GRAY = {0: 0, 1: 1, 3: 2, 2: 3, 6: 4, 7: 5, 5: 6, 4: 7}   # XYZ -> phase change (pi/4)
UW = '000 010 011 110 000 001 101 110 001 100 011 111 101 111 100 010'.split()
H = ['00000000111111111111', '00111111000011111111', '11000111001100001111',
     '11011011010100110011', '01101001111001010101']

# GF(256), x^8 + x^7 + x^2 + x + 1; RS generator roots alpha^120..125
EXP = [0] * 512
LOG = [0] * 256
_v = 1
for _i in range(255):
    EXP[_i] = _v
    LOG[_v] = _i
    _v <<= 1
    if _v & 0x100:
        _v ^= 0x187
for _i in range(255, 512):
    EXP[_i] = EXP[_i - 255]


def gmul(a, b):
    return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


GEN = [1]
for _j in range(120, 126):
    _n = [0] * (len(GEN) + 1)
    for _k, _c in enumerate(GEN):
        _n[_k + 1] ^= _c                     # x * g
        _n[_k] ^= gmul(_c, EXP[_j])          # alpha^j * g
    GEN = _n                                 # GEN[k] = coefficient of x^k


def rev8(b):
    return int('{:08b}'.format(b)[::-1], 2)


def rs_parity(data):
    """6 check octets of a row (data = its non-fill octets, first = highest power)."""
    msg = [rev8(b) for b in data] + [0] * (249 - len(data))   # trailing virtual fill
    rem = [0] * 6                                              # rem[k] = coeff of x^k
    for c in msg:
        fb = c ^ rem[5]
        for t in range(5, 0, -1):
            rem[t] = rem[t - 1] ^ gmul(fb, GEN[t])
        rem[0] = gmul(fb, GEN[0])
    return [rev8(rem[5 - p]) for p in range(6)]   # sent first: the highest power


def crc_x25(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def addr_octets(spec, flag):
    if spec == '*':
        typ, a = 1, 0xFFFFFF
    else:
        typ = {'A': 1, 'G': 5}[spec[0]]
        a = int(spec[1:], 16)
    v = (flag << 27) | (typ << 24) | a
    bits = [(v >> (27 - i)) & 1 for i in range(28)]          # sent most significant first
    out = []
    for o in range(4):
        b = 0
        for k in range(7):
            b |= bits[o * 7 + k] << (k + 1)
        out.append(b)
    return out


def avlc(src, dst, ctl, info):
    # flag bits as heard on real links: a ground destination 0, an aircraft /
    # all-aircraft destination 1; the source 0 for commands (I / XID), 1 for RR
    dflag = 0 if dst.startswith('G') else 1
    sflag = 1 if ctl & 3 == 1 else 0
    o = addr_octets(dst, dflag) + addr_octets(src, sflag)
    o[7] |= 1                                                # end of the address field
    body = bytes(o) + bytes([ctl]) + bytes(info)
    f = crc_x25(body)
    return body + bytes([f & 0xFF, f >> 8])


def par(ch):
    v = ord(ch) & 0x7F if isinstance(ch, str) else ch & 0x7F
    return v | (0x80 if bin(v).count('1') % 2 == 0 else 0)


def acars_block(kind, reg, label, bid, text):
    if label == '_d':
        label = '_\x7f'
    a = ('.' * 7 + reg)[-7:] if reg else '\0' * 7
    b = [par('2')] + [par(c) for c in a] + [par('\x15')] + [par(c) for c in label] + [par(bid)]
    if text:
        b += [par('\x02')] + [par(c) for c in text]
    b += [par('\x03')]
    c = 0
    for x in b:                                              # BCS: CRC-16/KERMIT
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return b + [c & 0xFF, c >> 8, par('\x7f')]


def frame(spec, ns):
    kind, _, rest = spec.partition(':')
    if kind == 'aoa':
        sd, k, reg, label, bid, text = rest.split(':', 5)
        src, dst = sd.split('>')
        ctl = (ns & 7) << 1                                  # I frame, N(S)
        return avlc(src, dst, ctl, [0xFF, 0xFF, 0x01] + acars_block(k, reg, label, bid, text))
    if kind == 'xid':
        gs, icaos, lat, lon = rest.split(':')
        cov = b''.join(c.encode()[:4].ljust(4) for c in icaos.split(','))
        la = int(round(float(lat) * 10)) & 0xFFF
        lo = int(round(float(lon) * 10)) & 0xFFF
        priv = [0x00, 0x01, ord('V'), 0x04, 0x01, 0x20, 0xC1, len(cov)] + list(cov) + \
               [0xC8, 0x03, la >> 4, ((la & 0xF) << 4) | (lo >> 8), lo & 0xFF]
        pub = [0x01, 0x09] + list(b'8885:1993') + [0x02, 0x02, 0x00, 0x01, 0x03, 0x03, 0x20, 0xA4, 0x88]
        info = [0x82, 0x80, 0x00, len(pub)] + pub + [0xF0, 0x00, len(priv)] + priv
        return avlc(gs, '*', 0xAF, info)
    if kind == 'rr':
        src, dst = rest.split('>')
        return avlc(src, dst, 0x01 | (1 << 5), [])
    if kind == 'data':
        sd, n = rest.split(':')
        src, dst = sd.split('>')
        rng = np.random.default_rng(len(sd) + int(n))
        return avlc(src, dst, (ns & 7) << 1, list(rng.integers(0, 256, int(n))))
    raise SystemExit('unknown frame: ' + spec)


def hdlc_bits(frames):
    flag = [0, 1, 1, 1, 1, 1, 1, 0]
    bits = list(flag)
    for f in frames:
        ones = 0
        for byte in f:
            for k in range(8):                               # lsb first
                b = (byte >> k) & 1
                bits.append(b)
                if b:
                    ones += 1
                    if ones == 5:
                        bits.append(0)
                        ones = 0
                else:
                    ones = 0
        bits += flag
    return bits


def nparity(k):
    return 0 if k <= 2 else 2 if k <= 30 else 4 if k <= 67 else 6


def burst_bits(data_bits):
    L = len(data_bits)
    padded = data_bits + [0] * (-L % 8)
    octets = [sum(padded[i * 8 + j] << (7 - j) for j in range(8)) for i in range(len(padded) // 8)]
    rows = (L + 1991) // 1992
    table = []
    for r in range(rows):
        d = octets[r * 249:(r + 1) * 249]
        np_ = nparity(len(d))
        table.append((d, rs_parity(d)[:np_]))
    out = []
    for col in range(255):
        for d, p in table:
            if col < len(d):
                out.append(d[col])
            elif 249 <= col < 249 + len(p):
                out.append(p[col - 249])
    bits = []
    for o in out:
        bits += [(o >> (7 - j)) & 1 for j in range(8)]
    hdr = [0, 0, 0] + [(L >> i) & 1 for i in range(17)]
    fec = [sum(int(H[r][i]) * hdr[i] for i in range(20)) % 2 for r in range(5)]
    allb = hdr + fec + bits
    s = 0x6959                                               # PN: stages 1..15 = 1101 0010 1011 001
    for i in range(len(allb)):
        f = ((s >> 14) ^ s) & 1
        s = (s >> 1) | (f << 14)
        allb[i] ^= f
    return allb


def symbols(bits):
    bits = bits + [0] * (-len(bits) % 3)
    steps = [GRAY[int(t[0]) << 2 | int(t[1]) << 1 | int(t[2])] for t in UW]
    for i in range(0, len(bits), 3):
        steps.append(GRAY[bits[i] << 2 | bits[i + 1] << 1 | bits[i + 2]])
    return steps


def modulate(steps, ppm):
    ph = np.cumsum([0] * 5 + steps) * np.pi / 4              # 5 ramp-up symbols as '000'
    syms = np.exp(1j * ph)
    syms[:5] *= np.array([0.0, 0.3, 0.8, 1.0, 1.0])          # power ramp
    sps = SPS * (1 + ppm * 1e-6)                             # the transmitter's symbol clock
    n = int(len(syms) * sps) + 20 * SPS
    x = np.zeros(n, complex)
    for k, s in enumerate(syms):                              # raised cosine pulses (alpha 0.6) at fractional positions
        c = k * sps + 8 * SPS
        i0 = int(np.floor(c))
        fr = c - i0
        t = (np.arange(-8 * SPS, 8 * SPS + 2) - fr) / SPS
        den = 1 - (1.2 * t) ** 2
        p = np.sinc(t) * np.cos(np.pi * 0.6 * t) / np.where(np.abs(den) < 1e-8, 1, den)
        p = np.where(np.abs(den) < 1e-8, np.pi / 4 * np.sinc(1 / 1.2), p)
        x[i0 - 8 * SPS:i0 + 8 * SPS + 2] += s * p
    return x


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    rate, offset, ppm, invert = 48000.0, 0.0, 0.0, False
    bursts = []
    i = 1
    while i < len(a):
        if a[i] == '--rate':
            rate = float(a[i + 1]); i += 2
        elif a[i] == '--offset':
            offset = float(a[i + 1]); i += 2
        elif a[i] == '--clock-ppm':
            ppm = float(a[i + 1]); i += 2
        elif a[i] == '--invert':
            invert = True; i += 1
        elif a[i] == '--at':
            bursts.append((float(a[i + 1]), a[i + 2])); i += 3
        else:
            print('unknown option', a[i]); sys.exit(2)
    fm = RS_SYM * SPS
    made = []
    end = 0.0
    for n, (t, spec) in enumerate(bursts):
        frames = [frame(s, n) for s in spec.split('+')]
        x = modulate(symbols(burst_bits(hdlc_bits(frames))), ppm)
        made.append((t, x, spec))
        end = max(end, t + len(x) / fm)
    from fractions import Fraction
    from scipy.signal import resample_poly
    fr = Fraction(rate / fm).limit_denominator(1000)
    total = np.zeros(int((end + 0.2) * rate), complex)
    for t, x, spec in made:
        y = resample_poly(x, fr.numerator, fr.denominator)
        n0 = int(round(t * rate))
        m = min(len(y), len(total) - n0)
        k = np.arange(m) + n0
        total[n0:n0 + m] += y[:m] * np.exp(2j * np.pi * offset * k / rate)
        print('%8.3f .. %8.3f s  %s' % (t, t + m / rate, spec))
    if invert:
        total = np.conj(total)
    total = total / max(1e-9, np.sqrt(np.mean(np.abs(total[np.abs(total) > 0.1]) ** 2)))   # bursts at rms 1
    total.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
