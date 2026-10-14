#!/usr/bin/env python3
"""MPT1327 control channel test signal: a site's forward control channel as
on the air (MPT 1327) - 1200 bit/s FFSK (1 = one cycle of 1200 Hz, 0 = one
and a half of 1800 Hz) on NBFM, back-to-back 128-bit slots: the control
channel system codeword (CCSC: 0, SYS, CCS, preamble 1010..; its CCS chosen
so that the BCH(63,48) check + parity are the SYNC 0xC4D7) and an address
codeword (an ALOHA, BCH-checked).

  mpt1327_gen.py OUT.cf32 --sys 0x42E1 [--prefix 42] [--rate 48000] [--dev 2500]
                 --at T --dur S [--at T --dur S ...]

The site's control channel on during each [at, at + dur) (cf32 + OUT.json).
Put several sites at their own bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

FS = 48000
SPB = 40
SYNC = 0xC4D7


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


def aloha(prefix):
    # 1 | PFIX (7) | IDENT1 (13: 8191 all radios) | type 256 (ALOHA) | rest 0
    info = 1 << 47 | (prefix & 0x7F) << 40 | 8191 << 27 | 256 << 18
    return check16(info)


def bits64(cw):
    return [(cw >> (63 - i)) & 1 for i in range(64)]


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'sys': 0x42E1, 'prefix': 42, 'rate': 48000.0, 'dev': 2500.0}
    on = []
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'at':
            on.append([float(v), 5.0])
        elif k == 'dur':
            on[-1][1] = float(v)
        else:
            o[k] = int(v, 0) if k in ('sys', 'prefix') else float(v)
        i += 2
    c, ad = bits64(ccsc(o['sys'])), bits64(aloha(o['prefix']))
    end = max(t + d for t, d in on) + 0.2
    nslots = int(end * 1200 / 128) + 2
    bits = []
    for _ in range(nslots):
        bits += c + ad
    f = np.repeat(np.where(np.asarray(bits) > 0, 1200.0, 1800.0), SPB)
    audio = np.sin(2 * np.pi * np.cumsum(f) / FS)
    x = np.exp(2j * np.pi * np.cumsum(audio * o['dev']) / FS)
    rate = o['rate']
    if abs(rate - FS) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(rate / FS).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    y = np.zeros(int(end * rate), np.complex128)
    for t, d in on:
        a0 = int(t * rate)
        seg = x[a0:a0 + int(d * rate)]
        ramp = np.minimum(1, np.minimum(np.arange(len(seg)), np.arange(len(seg))[::-1]) / (0.003 * rate))
        y[a0:a0 + len(seg)] = seg * ramp
        print('%8.3f .. %8.3f s  control channel SYS 0x%04X' % (t, t + len(seg) / rate, o['sys']))
    y.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
