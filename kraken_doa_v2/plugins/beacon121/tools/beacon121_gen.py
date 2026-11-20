#!/usr/bin/env python3
"""121.5 / 243 MHz swept-tone beacon test transmissions (ELT / EPIRB / PLB
homing signal) and the other signals of the guard channel, for the beacon121
plugin. One transmitter per file, as on the air:

  AM, full carrier (A3X), modulation factor m = (A - B) / (A + B) of the
  envelope, the audio tone sweeping over >= 700 Hz inside 300 - 1600 Hz
  2 - 4 times a second (downward - ETSI EN 302 152-1 8.2.1.2, UK CAA Spec 16
  2.2.3 - or upward, ITU-R M.690-3 c), a modulation duty cycle of 33 - 55 %
  (EN 302 152-1 8.2.3: the positive half cycle at the envelope's half
  amplitude points over the audio period; 33 - 66 % in CAA Spec 16), carrier
  within +-50 ppm (+-6.075 kHz at 121.5 MHz). Options for what the standards
  allow besides: a keyed carrier (CAA Spec 16 2.2.4: e.g. 0.75 s on / 1.5 s
  off), an unmodulated CW period (2 s every 8 s, 2.2.4 c), the pause of a
  406 MHz beacon's homer during its 406 MHz burst (<= 2 s, EN 302 152-1
  8.2.1.3; C/S T.001 4.5.3: homing never at the same time as the 406 MHz
  signal) and the optional Morse identification letter right after it (FCC
  95.1402: Morse "P" on a 406 MHz PLB's 121.5 MHz signal).

  beacon121_gen.py OUT.cf32 [--rate 32000] [--seconds S] [--offset HZ]
       [--at S] [--dur S] [--level DB] [--cn0 DBHZ] [--seed N] [--rf HZ]
       [--kind beacon|carrier|tone|voice|siren]
       beacon: [--lo 300] [--hi 1600] [--sweep-rate 3] [--dir down|up|updown]
               [--shape linear|exp] [--wave square|sine] [--duty 0.5]
               [--depth 0.9] [--drift HZ_PER_MIN] [--rate-wobble PCT]
               [--keyed ON,OFF] [--cw ON,PERIOD]
               [--pause-every S] [--pause-len S] [--pause-first S]
               [--morse LETTERS] [--morse-mode tone|carrier] [--morse-unit S]
       tone:   [--tone HZ] [--depth M]          a steady AM tone
       siren:  [--tones F1,F2] [--sweep-rate R] a two-tone hi-lo
       voice:  [--depth M]                       voice-like AM (pitch,
               formants, syllables, pauses - synthetic)
       carrier: an unmodulated carrier

--offset: the carrier in Hz from the input's 0 Hz (the plugin's VFO sits on
121.5 MHz). --at / --dur: when it is on the air (default: the whole file).
--level: carrier amplitude in dB (0 = 1.0). --cn0: adds white noise for this
carrier-to-noise density (dBHz, of a level-0 carrier) - leave it out for
uca_scene.py, which adds its own (and scales every source to unit power).

Output: OUT.cf32 (complex float32) + OUT.json (rate, rf_hz, the parameters).
Several transmitters at their own bearings: plugins/sdk/tools/uca_scene.py.
"""
import argparse
import json
import sys

import numpy as np

MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.', 'H': '....',
         'I': '..', 'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---', 'P': '.--.',
         'Q': '--.-', 'R': '.-.', 'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-',
         'Y': '-.--', 'Z': '--..'}


def morse_key(text, unit):
    """[(on, seconds)] of a Morse text: dot 1, dash 3, gap 1, letter gap 3, word gap 7 units"""
    out = []
    for wi, word in enumerate(text.upper().split()):
        if wi:
            out.append((False, 7 * unit))
        for li, ch in enumerate(word):
            if li:
                out.append((False, 3 * unit))
            for si, sym in enumerate(MORSE.get(ch, '')):
                if si:
                    out.append((False, unit))
                out.append((True, (1 if sym == '.' else 3) * unit))
    return out


def band_square(phase, duty, f_inst, fmax):
    """a rectangular wave of this duty cycle (levels +-1 incl. its mean), band-limited
    to fmax (harmonics k f < fmax, Lanczos-smoothed): no aliasing at the file's rate"""
    w = np.full(phase.shape, 2 * duty - 1, np.float64)
    kmax = int(fmax // 200)
    for k in range(1, kmax + 1):
        a = 4 / (k * np.pi) * np.sin(k * np.pi * duty)
        on = k * f_inst < fmax
        if not on.any():
            break
        sigma = np.sinc(k * f_inst / fmax)    # Lanczos factor per sample
        w += np.where(on, a * sigma * np.cos(k * phase - k * np.pi * duty), 0.0)
    return w


def sweep_freq(t, a):
    """the instantaneous audio frequency of the sweep"""
    r = a.sweep_rate
    if a.rate_wobble:
        # the sweep oscillator's rate wandering slowly (+-PCT over ~20 s)
        r = r * (1 + a.rate_wobble / 100 * np.sin(2 * np.pi * t / 20.0))
        u = np.cumsum(r) / a.rate
    else:
        u = t * r
    u = np.mod(u + 0.37, 1.0)
    lo, hi = a.lo, a.hi
    if a.dir == 'updown':
        u = 1 - np.abs(2 * u - 1)          # 0 -> 1 -> 0: up, then down
        frac = u
    elif a.dir == 'up':
        frac = u
    else:
        frac = 1 - u
    if a.shape == 'exp':
        return lo * (hi / lo) ** frac
    return lo + (hi - lo) * frac


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out')
    p.add_argument('--rate', type=float, default=32000)
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--offset', type=float, default=0)
    p.add_argument('--at', type=float, default=0)
    p.add_argument('--dur', type=float, default=0)
    p.add_argument('--level', type=float, default=0)
    p.add_argument('--cn0', type=float, default=None)
    p.add_argument('--seed', type=int, default=1)
    p.add_argument('--rf', type=float, default=121.5e6)
    p.add_argument('--kind', default='beacon', choices=['beacon', 'carrier', 'tone', 'voice', 'siren'])
    p.add_argument('--lo', type=float, default=300)
    p.add_argument('--hi', type=float, default=1600)
    p.add_argument('--sweep-rate', type=float, default=3.0)
    p.add_argument('--dir', default='down', choices=['down', 'up', 'updown'])
    p.add_argument('--shape', default='linear', choices=['linear', 'exp'])
    p.add_argument('--wave', default='square', choices=['square', 'sine'])
    p.add_argument('--duty', type=float, default=0.5)
    p.add_argument('--depth', type=float, default=0.9)
    p.add_argument('--drift', type=float, default=0, help='carrier drift, Hz per minute')
    p.add_argument('--rate-wobble', type=float, default=0)
    p.add_argument('--keyed', default='')
    p.add_argument('--cw', default='')
    p.add_argument('--pause-every', type=float, default=0)
    p.add_argument('--pause-len', type=float, default=0.5)
    p.add_argument('--pause-first', type=float, default=5)
    p.add_argument('--morse', default='')
    p.add_argument('--morse-mode', default='tone', choices=['tone', 'carrier'])
    p.add_argument('--morse-unit', type=float, default=0.1)
    p.add_argument('--morse-tone', type=float, default=1000)
    p.add_argument('--tone', type=float, default=1000)
    p.add_argument('--tones', default='700,1100')
    a = p.parse_args()

    rate = a.rate
    n = int(round(a.seconds * rate))
    t = np.arange(n) / rate
    rng = np.random.default_rng(a.seed)
    fmax = min(4000.0, 0.45 * rate)
    t_end = a.at + a.dur if a.dur > 0 else a.seconds
    on_air = (t >= a.at) & (t < t_end)
    carrier_on = on_air.astype(np.float64)        # carrier keying (1 / 0)
    mod = np.zeros(n)                             # modulation waveform, -1..1 (envelope 1 + m mod)
    depth = a.depth
    info = {'kind': a.kind, 'offset_hz': a.offset, 'at': a.at, 'end': t_end}

    if a.kind == 'beacon':
        f = sweep_freq(t, a)
        ph = 2 * np.pi * np.cumsum(f) / rate
        mod = band_square(ph, a.duty, f, fmax) if a.wave == 'square' else np.sin(ph)
        mod = 2 * (mod - mod.min()) / (mod.max() - mod.min()) - 1      # envelope extremes exactly 1 +- m
        info.update({'lo': a.lo, 'hi': a.hi, 'sweep_rate': a.sweep_rate, 'dir': a.dir, 'shape': a.shape,
                     'wave': a.wave, 'duty': a.duty if a.wave == 'square' else 0.5, 'depth': depth})
        mod_on = np.ones(n)
        if a.cw:
            c_on, c_per = (float(v) for v in a.cw.split(','))
            mod_on[np.mod(t - a.at, c_per) < c_on] = 0
            info['cw'] = [c_on, c_per]
        if a.keyed:
            k_on, k_off = (float(v) for v in a.keyed.split(','))
            carrier_on *= (np.mod(t - a.at, k_on + k_off) < k_on)
            info['keyed'] = [k_on, k_off]
        pauses = []
        if a.pause_every > 0:
            tp = a.at + a.pause_first
            while tp < t_end:
                carrier_on[(t >= tp) & (t < tp + a.pause_len)] = 0
                pauses.append(round(tp, 3))
                if a.morse:
                    # the Morse letter right after the pause, then the sweep again
                    t0 = tp + a.pause_len + 0.3
                    for on, d in morse_key(a.morse, a.morse_unit):
                        seg = (t >= t0) & (t < t0 + d)
                        if a.morse_mode == 'tone':
                            mod_on[seg] = 2 if on else 0        # 2 = the steady Morse tone
                        else:
                            carrier_on[seg] = 1 if on else 0
                            mod_on[seg] = 0
                        t0 += d
                    # a quiet unit before and after the letter (key up)
                    pre = (t >= tp + a.pause_len) & (t < tp + a.pause_len + 0.3)
                    post = (t >= t0) & (t < t0 + 0.3)
                    mod_on[pre | post] = 0
                    if a.morse_mode == 'carrier':
                        carrier_on[pre | post] = 0      # key up = no carrier
                tp += a.pause_every
            info.update({'pauses': pauses, 'pause_len': a.pause_len})
            if a.morse:
                info.update({'morse': a.morse, 'morse_mode': a.morse_mode, 'morse_unit': a.morse_unit})
        tone = np.sin(2 * np.pi * a.morse_tone * t)
        mod = np.where(mod_on == 2, tone, mod * (mod_on == 1))
    elif a.kind == 'tone':
        mod = np.sin(2 * np.pi * a.tone * t)
        info.update({'tone': a.tone, 'depth': depth})
    elif a.kind == 'siren':
        f1, f2 = (float(v) for v in a.tones.split(','))
        f = np.where(np.mod(t * a.sweep_rate, 1.0) < 0.5, f1, f2)
        mod = np.sin(2 * np.pi * np.cumsum(f) / rate)
        info.update({'tones': [f1, f2], 'rate': a.sweep_rate, 'depth': depth})
    elif a.kind == 'voice':
        # voice-like: a glottal pulse train (pitch wandering 90 - 220 Hz) through
        # three formants that change per syllable, syllables of 0.08 - 0.35 s,
        # gaps, phrase pauses; AM up to the given depth
        n_f = int(np.ceil(a.seconds * 50))
        f0 = np.interp(t, np.arange(n_f) / 50, 150 + 60 * np.cumsum(rng.normal(0, 0.15, n_f)).clip(-1.2, 1.2))
        gph = 2 * np.pi * np.cumsum(f0) / rate
        env = np.zeros(n)
        F = np.zeros((3, n))
        vowels = [(700, 1200, 2600), (300, 2300, 3000), (500, 1000, 2400), (350, 900, 2300), (600, 1700, 2500)]
        tt = a.at
        while tt < t_end:
            if rng.random() < 0.12:
                tt += rng.uniform(0.5, 1.5)               # a phrase pause
            d = rng.uniform(0.08, 0.35)
            seg = (t >= tt) & (t < tt + d)
            env[seg] = np.sin(np.pi * (t[seg] - tt) / d) ** 0.5 * rng.uniform(0.5, 1.0)
            v = vowels[rng.integers(len(vowels))]
            for i in range(3):
                F[i, seg] = v[i] * rng.uniform(0.9, 1.1)
            tt += d + rng.uniform(0.03, 0.25)
        x = np.zeros(n)
        for k in range(1, 30):
            fk = k * f0
            amp = np.zeros(n)
            for i, bw in enumerate((90, 120, 160)):
                amp += 1 / (1 + ((fk - F[i]) / bw) ** 2) * (0.6 ** i)
            amp *= fk < min(3400, fmax)
            x += amp * np.cos(k * gph) / k ** 0.5
        x *= env
        mod = x / max(np.abs(x).max(), 1e-9)
        info.update({'depth': depth})
    else:
        depth = 0

    env = carrier_on * (1 + depth * mod)
    fc = a.offset + a.drift / 60 * t
    ph = 2 * np.pi * np.cumsum(fc) / rate + rng.uniform(0, 2 * np.pi)
    x = 10 ** (a.level / 20) * env * np.exp(1j * ph)
    if a.cn0 is not None:
        sigma = np.sqrt(rate / 10 ** (a.cn0 / 10))
        x = x + (rng.standard_normal(n) + 1j * rng.standard_normal(n)) * sigma / np.sqrt(2)
        info['cn0_dbhz'] = a.cn0 + a.level
    x.astype(np.complex64).tofile(a.out)
    side = a.out.rsplit('.', 1)[0] + '.json'
    with open(side, 'w') as fp:
        json.dump({'rate': rate, 'rf_hz': a.rf, 'seconds': a.seconds, 'signal': info}, fp, indent=1)
    print(f'{a.out}: {a.seconds:.1f} s at {rate:.0f} Hz, {a.kind} at {a.offset:+.0f} Hz', file=sys.stderr)


if __name__ == '__main__':
    main()
