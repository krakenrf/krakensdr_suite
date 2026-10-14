#!/usr/bin/env python3
"""ILS localizer / VOR test signal (ICAO Annex 10 modulation) with its Morse
ident on 1020 Hz: localizer = carrier AM by 90 and 150 Hz (20 % each, DDM
--ddm); VOR = 30 Hz AM (variable, 30 %) + the 9960 Hz subcarrier (30 %)
frequency-modulated +-480 Hz by the 30 Hz reference, the phase difference
being the radial. Ident keyed at --wpm, repeated every --every s.

  ils_gen.py OUT.cf32 --type vor|loc --ident WN [--radial 123] [--ddm 0]
             [--offset HZ] [--rate 48000] [--wpm 7] [--every 8] --at T --dur S [...]

Complex baseband (cf32) + OUT.json. Put several navaids at their own
bearings with plugins/sdk/tools/uca_scene.py.
"""
import json
import sys

import numpy as np

MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.', 'H': '....', 'I': '..',
         'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.',
         'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..',
         '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....', '6': '-....',
         '7': '--...', '8': '---..', '9': '----.'}


def keying(text, unit, n, rate, every):
    """1 while the key is down: the ident from t = 0.5 s, again every `every` s"""
    seq = []
    for ci, c in enumerate(text.upper()):
        for si, s in enumerate(MORSE[c]):
            seq += [1] * (1 if s == '.' else 3) + [0]
        seq += [0, 0]
    k = np.zeros(n)
    t = 0.5
    while t * rate < n:
        for i, v in enumerate(seq):
            a = int((t + i * unit) * rate)
            if v:
                k[a:min(n, a + int(unit * rate))] = 1
        t += every
    # soft edges (5 ms)
    w = np.hanning(int(0.005 * rate) | 1)
    return np.convolve(k, w / w.sum(), 'same')


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'type': 'vor', 'ident': 'WN', 'radial': 123.0, 'ddm': 0.0, 'offset': 0.0, 'rate': 48000.0, 'wpm': 7.0, 'every': 8.0}
    on = []
    i = 1
    while i < len(a):
        k, v = a[i].lstrip('-'), a[i + 1]
        if k == 'at':
            on.append([float(v), 10.0])
        elif k == 'dur':
            on[-1][1] = float(v)
        else:
            o[k] = v if k in ('type', 'ident') else float(v)
        i += 2
    rate = o['rate']
    end = max(t + d for t, d in on) + 0.2
    n = int(end * rate)
    t = np.arange(n) / rate
    key = keying(o['ident'], 1.2 / o['wpm'], n, rate, o['every'])
    ident = 0.10 * key * np.sin(2 * np.pi * 1020 * t)
    if o['type'] == 'loc':
        m90, m150 = 0.2 + o['ddm'] / 2, 0.2 - o['ddm'] / 2
        env = 1 + m90 * np.sin(2 * np.pi * 90 * t) + m150 * np.sin(2 * np.pi * 150 * t) + ident
    else:
        r = np.deg2rad(o['radial'])
        ref = 0.3 * np.cos(2 * np.pi * 9960 * t + (480 / 30) * np.sin(2 * np.pi * 30 * t))
        env = 1 + 0.3 * np.cos(2 * np.pi * 30 * t - r) + ref + ident
    x = env * np.exp(2j * np.pi * o['offset'] * t)
    y = np.zeros(n, np.complex128)
    for t0, d in on:
        a0, a1 = int(t0 * rate), min(n, int((t0 + d) * rate))
        ramp = np.minimum(1, np.minimum(np.arange(a1 - a0), np.arange(a1 - a0)[::-1]) / (0.01 * rate))
        y[a0:a1] = x[a0:a1] * ramp
        print('%8.3f .. %8.3f s  %s %s' % (t0, t0 + (a1 - a0) / rate, o['type'].upper(), o['ident']))
    y.astype(np.complex64).tofile(out)
    json.dump({'rate': rate}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
