#!/usr/bin/env python3
"""Signal classifier training data: the signal classes no plugin generator
makes - analog AM and SSB voice, CW Morse, bare carriers - and the "unknown"
families the classifier rejects into (random FSK, PSK / QAM, OFDM, pulse
trains, band-limited noise). One clean, noise-free transmitter per file.

  synth.py KIND OUT.cf32 --seed N [--seconds S]

KIND: am | ssb | cw | carrier | unk_fsk | unk_psk | unk_ofdm | unk_pulse | unk_noise
Every other parameter is drawn from the seed. Writes OUT.cf32 (complex64,
the signal centred on 0 Hz) + OUT.json {"rate", "variant": {...}}.

The unknown families keep away from the known protocols' exact parameters
(symbol rates, subcarrier spacings, pulse timings: AVOID_* below), so the
model has somewhere to put a signal it does not know without being taught
that a real DMR or DVB-T signal is unknown. numpy + scipy only.
"""
import importlib.util
import json
import os
import sys

import numpy as np
from scipy.signal import butter, hilbert, resample_poly, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS = os.path.normpath(os.path.join(HERE, '..', '..', 'plugins'))

# output rates a clip may have (all divide 2.4 MHz)
RATES = [48000, 96000, 240000, 480000, 800000, 1200000, 2400000]

# symbol rates (Bd) of the known protocols, kept +-6 % away from
AVOID_RS = [400, 512, 1200, 1600, 2400, 3200, 4800, 6000, 6400, 9600, 10500, 17241, 18000, 19231,
            38400, 100000, 1041667]
# OFDM subcarrier spacings (Hz) of the known systems (DAB, DVB-T 2k / 4k / 8k, DVB-T2 16k / 32k,
# ISDB-T modes 1-3, DTMB, ATSC 3.0, LTE / NR), kept +-5 % away from
AVOID_SCS = [1000, 1116, 2232, 4464, 279, 558, 992, 1984, 3968, 2000, 843, 1688, 3375, 15000, 30000]


def _speech_module():
    spec = importlib.util.spec_from_file_location('analog_gen', os.path.join(PLUGINS, 'analog_id', 'tools', 'analog_gen.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def speech(n, rate, rng):
    """speech-like audio (analog_gen.py's synth_speech), peak 1"""
    return _speech_module().synth_speech(n, rate, rng)


def away(x, avoid, frac):
    """True if x is not within frac of any value in avoid"""
    return all(abs(x - a) > frac * a for a in avoid)


def draw_away(rng, lo, hi, avoid, frac):
    for _ in range(1000):
        x = float(np.exp(rng.uniform(np.log(lo), np.log(hi))))
        if away(x, avoid, frac):
            return x
    raise RuntimeError('no value away from the known ones')


def pick_rate(bw):
    """the lowest rate that holds a signal of bandwidth bw within +-0.35 rate"""
    for r in RATES:
        if bw <= 0.7 * r:
            return r
    return RATES[-1]


def ramp_env(n, rate, ivals, ramp_s):
    """an on / off envelope: 1 inside the intervals (s), raised-cosine edges"""
    e = np.zeros(n)
    k = max(1, int(ramp_s * rate))
    up = 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    for a, b in ivals:
        i, j = int(a * rate), min(n, int(b * rate))
        if j - i < 2 * k + 1:
            continue
        e[i:j] = 1
        e[i:i + k] = up
        e[j - k:j] = up[::-1]
    return e


def keyed_intervals(rng, seconds, on=(1.0, 6.0), off=(0.3, 3.0), start=(0.05, 1.0)):
    t = rng.uniform(*start)
    out = []
    while t < seconds - 0.2:
        d = rng.uniform(*on)
        out.append([t, min(seconds - 0.05, t + d)])
        t += d + rng.uniform(*off)
    return out


def bandpass_audio(x, rate, lo, hi):
    sos = butter(4, [lo, hi], btype='bandpass', fs=rate, output='sos')
    return sosfilt(sos, x)


def compress(x, rng):
    """a transmitter's speech processor: soft limiting by a random amount"""
    g = rng.uniform(1.0, 6.0)
    y = np.tanh(g * x / max(np.max(np.abs(x)), 1e-9))
    return y / max(np.max(np.abs(y)), 1e-9)


# --------------------------------------------------------------- analog voice

def gen_am(rng, seconds):
    rate = 48000
    n = int(seconds * rate)
    chan = '8.33k' if rng.random() < 0.4 else '25k'
    hi = 2500 if chan == '8.33k' else rng.uniform(2700, 3400)
    a = compress(bandpass_audio(speech(n, rate, rng), rate, 300, hi), rng)
    depth = rng.uniform(0.4, 0.95)
    cont = rng.random() < 0.25      # a broadcast / ATIS-style carrier that stays on
    ivals = [[0.0, seconds]] if cont else keyed_intervals(rng, seconds)
    env = ramp_env(n, rate, ivals, rng.uniform(0.002, 0.01))
    x = env * (1 + depth * a)
    v = {'channel': chan, 'depth': round(depth, 2), 'keyed': not cont, 'audio_hi_hz': round(hi)}
    return rate, x.astype(np.complex128), v


def gen_ssb(rng, seconds):
    rate = 48000
    n = int(seconds * rate)
    side = 'usb' if rng.random() < 0.5 else 'lsb'
    lo, hi = rng.uniform(200, 400), rng.uniform(2400, 3000)
    a = compress(bandpass_audio(speech(n, rate, rng), rate, lo, hi), rng)
    z = hilbert(a)
    if side == 'lsb':
        z = np.conj(z)
    ivals = keyed_intervals(rng, seconds, on=(2.0, 12.0), off=(0.3, 3.0))
    env = ramp_env(n, rate, ivals, 0.005)
    x = env * z
    car = 0.0
    if rng.random() < 0.3:          # carrier leakage of a cheap transmitter
        car = 10 ** (rng.uniform(-40, -20) / 20)
        x = x + car * env
    return rate, x, {'side': side, 'audio_hz': [round(lo), round(hi)], 'carrier_db': round(20 * np.log10(car), 1) if car else None}


MORSE = {'A': '.-', 'B': '-...', 'C': '-.-.', 'D': '-..', 'E': '.', 'F': '..-.', 'G': '--.', 'H': '....', 'I': '..',
         'J': '.---', 'K': '-.-', 'L': '.-..', 'M': '--', 'N': '-.', 'O': '---', 'P': '.--.', 'Q': '--.-', 'R': '.-.',
         'S': '...', 'T': '-', 'U': '..-', 'V': '...-', 'W': '.--', 'X': '-..-', 'Y': '-.--', 'Z': '--..',
         '0': '-----', '1': '.----', '2': '..---', '3': '...--', '4': '....-', '5': '.....', '6': '-....',
         '7': '--...', '8': '---..', '9': '----.', '/': '-..-.', '?': '..--..', '=': '-...-'}


def morse_text(rng):
    """random groups that look like CW traffic (synthetic callsigns, reports, random groups)"""
    al = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'

    def call():
        return ''.join(rng.choice(list(al), int(rng.integers(1, 3)))) + str(int(rng.integers(0, 10))) + \
            ''.join(rng.choice(list(al), int(rng.integers(1, 4))))
    words = []
    for _ in range(int(rng.integers(8, 30))):
        r = rng.random()
        if r < 0.3:
            words.append(call())
        elif r < 0.45:
            words.append(rng.choice(['CQ', 'DE', 'K', 'TU', 'RST', '5NN', 'QTH', 'QRZ?', 'BK', '73', 'R', '=']))
        elif r < 0.7:
            words.append(''.join(rng.choice(list(al + '0123456789'), 5)))
        else:
            words.append(''.join(rng.choice(list(al), int(rng.integers(2, 7)))))
    return ' '.join(words)


def gen_cw(rng, seconds):
    rate = 48000
    n = int(seconds * rate)
    wpm = rng.uniform(8, 35)
    dot = 1.2 / wpm
    dash = rng.uniform(2.7, 3.3)
    weight = rng.uniform(0.85, 1.15)
    ivals = []
    t = rng.uniform(0.05, 0.8)
    while t < seconds - 0.5:
        for w in morse_text(rng).split(' '):
            for ch in w:
                for s in MORSE.get(ch, ''):
                    d = dot * (dash if s == '-' else 1) * weight
                    ivals.append([t, t + d])
                    t += d + dot * (2 - weight)
                t += 2 * dot * rng.uniform(0.9, 1.4)
            t += 4 * dot * rng.uniform(0.9, 1.6)
            if t > seconds - 0.2:
                break
        t += rng.uniform(0.5, 3.0)       # a pause between overs
    ivals = [iv for iv in ivals if iv[1] < seconds - 0.01]
    rise = rng.uniform(0.002, 0.008)
    env = np.zeros(n)
    k = max(1, int(rise * rate))
    up = 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    for a, b in ivals:
        i, j = int(a * rate), int(b * rate)
        e = np.ones(j - i + k)
        e[:k] = up
        e[-k:] = up[::-1]
        m = min(n, i + len(e))
        env[i:m] = np.maximum(env[i:m], e[:m - i])
    ph = np.zeros(n)
    chirp = 0.0
    if rng.random() < 0.2:             # a chirpy oscillator pulled by the keying
        chirp = rng.uniform(-25, 25)
        ph = 2 * np.pi * np.cumsum(chirp * env) / rate
    x = env * np.exp(1j * ph)
    return rate, x, {'wpm': round(wpm, 1), 'rise_ms': round(rise * 1e3, 1), 'chirp_hz': round(chirp, 1)}


def gen_carrier(rng, seconds):
    rate = 48000
    n = int(seconds * rate)
    t = np.arange(n) / rate
    kind = rng.choice(['steady', 'keyed', 'drift', 'hum'], p=[0.4, 0.2, 0.25, 0.15])
    ivals = [[0.0, seconds]]
    env = np.ones(n)
    if kind == 'keyed':
        ivals = keyed_intervals(rng, seconds, on=(1.5, 8.0), off=(0.5, 3.0))
        env = ramp_env(n, rate, ivals, rng.uniform(0.001, 0.02))
    f = np.zeros(n)
    if kind == 'drift':
        f = rng.uniform(-8, 8) * (t - seconds / 2) + rng.uniform(0, 3) * np.sin(2 * np.pi * t / rng.uniform(2, 20))
    am = np.ones(n)
    if kind == 'hum':
        am = 1 + rng.uniform(0.01, 0.08) * np.sin(2 * np.pi * rng.choice([50.0, 60.0, 100.0, 120.0]) * t)
    ph = 2 * np.pi * np.cumsum(f) / rate + np.cumsum(rng.standard_normal(n)) * np.sqrt(2 * np.pi * rng.uniform(0.05, 3) / rate)
    x = env * am * np.exp(1j * ph)
    return rate, x, {'kind': str(kind)}


# ------------------------------------------------------------ unknown families

def bursts_or_continuous(rng, seconds, p_cont, on=(0.02, 1.5), off=(0.05, 2.0)):
    if rng.random() < p_cont:
        return [[0.0, seconds]], True
    return keyed_intervals(rng, seconds, on=on, off=off, start=(0.02, 0.5)), False


def gen_unk_fsk(rng, seconds):
    M = int(rng.choice([2, 4, 8], p=[0.5, 0.35, 0.15]))
    kind = rng.choice(['cpfsk', 'gfsk', 'afsk'], p=[0.4, 0.4, 0.2])
    if kind == 'afsk':
        M = 2
        rs = draw_away(rng, 150, 4000, AVOID_RS, 0.06)
        while True:
            f1, f2 = sorted(rng.uniform(300, 4000, 2))
            # not the Bell 202 / V.23 / MPT1327 / FFSK tone pairs
            if f2 - f1 > 0.5 * rs and all(abs(f1 - a) > 60 or abs(f2 - b) > 60
                                          for a, b in ((1200, 2200), (1300, 2100), (1200, 1800), (1300, 1700))):
                break
        dev = rng.uniform(1500, 5000)
        bw = 2 * (dev + f2)
    else:
        rs = draw_away(rng, 300, 150000, AVOID_RS, 0.06)
        h = rng.uniform(0.3, 2.5) if M == 2 else rng.uniform(0.2, 1.0)
        dev = h * rs / 2               # outer tone - centre, per unit level spacing
        bw = (M - 1) * h * rs + 2 * rs
    rate = pick_rate(bw)
    sps = rate / rs
    ivals, cont = bursts_or_continuous(rng, seconds, 0.3, on=(max(0.01, 200 / rs), max(0.05, min(2.0, 6000 / rs))))
    n = int(seconds * rate)
    nsym = int(seconds * rs) + 2
    sym = rng.integers(0, M, nsym)
    pre = int(rng.integers(16, 200))
    # every burst starts with an alternating preamble
    for a, b in ivals:
        i = int(a * rs)
        sym[i:i + pre] = np.where(np.arange(min(pre, nsym - i)) % 2 == 0, 0, M - 1)
    lev = (2 * sym - (M - 1)).astype(float)
    idx = np.minimum((np.arange(n) / sps).astype(int), nsym - 1)
    fr = lev[idx]
    bt = None
    if kind == 'gfsk':
        bt = rng.uniform(0.3, 1.0)
        L = int(4 * sps) | 1
        tt = (np.arange(L) - L // 2) / sps
        g = np.exp(-(2 * np.pi ** 2 * bt ** 2 * tt ** 2) / np.log(2))
        fr = np.convolve(fr, g / g.sum(), mode='same')
    if kind == 'afsk':
        bits = (fr > 0)
        tone = np.where(bits, f2, f1)
        audio = np.sin(2 * np.pi * np.cumsum(tone) / rate)
        phase = 2 * np.pi * dev * np.cumsum(audio) / rate
    else:
        phase = 2 * np.pi * dev * np.cumsum(fr) / rate
    env = ramp_env(n, rate, ivals, min(0.002, 4 / rs))
    x = env * np.exp(1j * phase)
    v = {'kind': str(kind), 'M': M, 'rs': round(rs, 1), 'dev_hz': round(dev, 1), 'continuous': cont}
    if bt:
        v['bt'] = round(bt, 2)
    if kind == 'afsk':
        v['tones'] = [round(f1), round(f2)]
    return rate, x, v


def rrc(beta, sps, span):
    n = np.arange(-span * sps, span * sps + 1) / sps
    h = np.zeros_like(n)
    for i, t in enumerate(n):
        if abs(t) < 1e-9:
            h[i] = 1 - beta + 4 * beta / np.pi
        elif beta > 0 and abs(abs(t) - 1 / (4 * beta)) < 1e-9:
            h[i] = beta / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * beta)) + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
        else:
            h[i] = (np.sin(np.pi * t * (1 - beta)) + 4 * beta * t * np.cos(np.pi * t * (1 + beta))) / (np.pi * t * (1 - (4 * beta * t) ** 2))
    return h / np.sqrt(np.sum(h ** 2))


def constellation(name):
    if name == 'bpsk':
        return np.array([1, -1], complex)
    if name in ('qpsk', 'oqpsk'):
        return np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4)))
    if name == '8psk':
        return np.exp(1j * np.pi / 4 * np.arange(8))
    if name == '16qam':
        a = np.array([-3, -1, 1, 3])
        return (a[:, None] + 1j * a[None, :]).ravel() / np.sqrt(10)
    if name == '32apsk':
        return np.concatenate([np.exp(1j * (np.pi / 4 + np.pi / 2 * np.arange(4))) * 0.4,
                               np.exp(1j * (np.pi / 12 + np.pi / 6 * np.arange(12))) * 0.75,
                               np.exp(1j * np.pi / 8 * np.arange(16))])
    raise ValueError(name)


def gen_unk_psk(rng, seconds):
    mod = str(rng.choice(['bpsk', 'qpsk', 'oqpsk', '8psk', '16qam', '32apsk'], p=[0.2, 0.3, 0.1, 0.15, 0.15, 0.1]))
    rs = draw_away(rng, 500, 700000, AVOID_RS, 0.06)
    beta = rng.uniform(0.1, 0.6)
    bw = rs * (1 + beta)
    rate = pick_rate(bw)
    # 4 samples per symbol, then to the clip's rate by a small ratio (sets the true symbol rate)
    from fractions import Fraction
    sps = 4
    fr = Fraction(rate / (rs * sps)).limit_denominator(64)
    rs = rate / (sps * float(fr))
    ivals, cont = bursts_or_continuous(rng, seconds, 0.5, on=(max(0.005, 300 / rs), max(0.05, min(2.0, 20000 / rs))))
    nsym = int(seconds * rs) + 16
    C = constellation(mod)
    s = C[rng.integers(0, len(C), nsym)]
    for a, b in ivals:      # a known preamble at each burst start (BPSK-like)
        i = int(a * rs)
        k = min(int(rng.integers(32, 128)), nsym - i)
        s[i:i + k] = C[0] * np.where(rng.random(k) < 0.5, 1, -1)
    up = np.zeros(nsym * sps, complex)
    up[::sps] = s
    h = rrc(beta, sps, 8)
    y = np.convolve(up, h, mode='same')
    if mod == 'oqpsk':
        y = y.real + 1j * np.roll(y.imag, sps // 2)
    if fr != 1:
        y = resample_poly(y, fr.numerator, fr.denominator)
    n = int(seconds * rate)
    y = np.resize(y, n) if len(y) < n else y[:n]
    env = ramp_env(n, rate, ivals, min(0.001, 4 / rs))
    x = env * y / np.sqrt(np.mean(np.abs(y) ** 2))
    return rate, x, {'mod': mod, 'rs': round(float(rs), 1), 'rolloff': round(beta, 2), 'continuous': cont}


def gen_unk_ofdm(rng, seconds):
    for _ in range(1000):
        scs = draw_away(rng, 400, 80000, AVOID_SCS, 0.05)
        bw = float(np.exp(rng.uniform(np.log(15e3), np.log(2.2e6))))
        nused = int(bw / scs)
        if 12 <= nused <= 4000:
            break
    rate = pick_rate(nused * scs * 1.05)
    nfft = int(round(rate / scs))
    scs = rate / nfft
    nused = min(nused, int(0.8 * nfft))
    cpf = float(rng.choice([1 / 4, 1 / 8, 1 / 16, 1 / 32, rng.uniform(0.05, 0.3)]))
    ncp = max(1, int(round(cpf * nfft)))
    mod = str(rng.choice(['qpsk', '16qam', 'bpsk']))
    C = constellation(mod)
    pilot_every = int(rng.choice([0, 3, 6, 12]))
    ivals, cont = bursts_or_continuous(rng, seconds, 0.5, on=(0.005, 1.0))
    n = int(seconds * rate)
    symlen = nfft + ncp
    nsym = n // symlen + 2
    k = np.arange(nused) - nused // 2
    k = k[k != 0] if rng.random() < 0.7 else k      # often a null DC carrier
    X = np.zeros((nsym, nfft), complex)
    X[:, k % nfft] = C[rng.integers(0, len(C), (nsym, len(k)))]
    if pilot_every:
        X[:, k[::pilot_every] % nfft] = 4 / 3
    y = np.fft.ifft(X, axis=1) * np.sqrt(nfft)
    y = np.concatenate([y[:, -ncp:], y], axis=1).ravel()[:n]
    env = ramp_env(n, rate, ivals, 0.0005)
    x = env * y
    return rate, x, {'scs_hz': round(scs, 1), 'nfft': nfft, 'used': int(len(k)), 'cp': round(ncp / nfft, 4),
                     'mod': mod, 'pilots_every': pilot_every, 'continuous': cont}


def gen_unk_pulse(rng, seconds):
    kind = str(rng.choice(['periodic', 'random', 'chirp', 'coded'], p=[0.35, 0.25, 0.2, 0.2]))
    width = float(np.exp(rng.uniform(np.log(100e-6), np.log(5e-3))))
    bwc = rng.uniform(20e3, 400e3) if kind == 'chirp' else 0.0
    rate = 480000 if bwc > 150e3 else 240000 if (bwc > 0 or width < 400e-6) else 96000
    n = int(seconds * rate)
    t0 = rng.uniform(0.0, 0.05)
    starts = []
    if kind in ('periodic', 'chirp'):
        pri = float(np.exp(rng.uniform(np.log(max(2e-3, 2.5 * width)), np.log(100e-3))))
        jit = rng.uniform(0, 0.05)
        t = t0
        while t < seconds - width:
            starts.append(t)
            t += pri * (1 + jit * rng.standard_normal())
    elif kind == 'random':
        lam = rng.uniform(3, 200)
        pri = 1 / lam
        t = t0
        while t < seconds - width:
            starts.append(t)
            t += width + rng.exponential(1 / lam)
    else:       # coded groups: a random pattern of pulses, repeated
        g = rng.integers(0, 2, int(rng.integers(4, 16)))
        g[0] = 1
        slot = width * rng.uniform(1.5, 3.0)
        pri = rng.uniform(len(g) * slot * 1.5, len(g) * slot * 6 + 0.02)
        t = t0
        while t < seconds - len(g) * slot:
            starts += [t + i * slot for i in np.nonzero(g)[0]]
            t += pri
    rise = min(width / 4, rng.uniform(5e-6, 100e-6))
    x = np.zeros(n, complex)
    L = int(width * rate)
    k = max(1, int(rise * rate))
    e = np.ones(L)
    up = 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    e[:k] = up
    e[-k:] = up[::-1]
    tt = np.arange(L) / rate
    p = e * (np.exp(1j * np.pi * bwc / width * (tt - width / 2) ** 2) if bwc else 1)
    for s in starts:
        i = int(s * rate)
        if i + L <= n:
            x[i:i + L] += p
    v = {'kind': kind, 'width_us': round(width * 1e6, 1), 'pri_ms': round(pri * 1e3, 2)}
    if bwc:
        v['chirp_hz'] = round(bwc)
    return rate, x, v


def gen_unk_noise(rng, seconds):
    bw = float(np.exp(rng.uniform(np.log(2e3), np.log(2.0e6))))
    rate = pick_rate(bw * 1.1)
    n = int(seconds * rate)
    shape = str(rng.choice(['flat', 'tilt', 'notch', 'bumps']))
    X = np.fft.fft(rng.standard_normal(n) + 1j * rng.standard_normal(n))
    f = np.fft.fftfreq(n, 1 / rate)
    edge = rng.uniform(0.02, 0.15) * bw
    H = np.clip((bw / 2 - np.abs(f)) / edge + 0.5, 0, 1)
    if shape == 'tilt':
        H = H * 10 ** (rng.uniform(-10, 10) * f / bw / 20)
    elif shape == 'notch':
        H = H * (1 - 0.95 * np.exp(-((f - rng.uniform(-0.4, 0.4) * bw) / (rng.uniform(0.02, 0.1) * bw)) ** 2))
    elif shape == 'bumps':
        for _ in range(int(rng.integers(2, 5))):
            H = H * (1 + rng.uniform(0.5, 3) * np.exp(-((f - rng.uniform(-0.45, 0.45) * bw) / (rng.uniform(0.03, 0.15) * bw)) ** 2))
    y = np.fft.ifft(X * H)
    ivals, cont = bursts_or_continuous(rng, seconds, 0.5, on=(0.01, 2.0))
    x = ramp_env(n, rate, ivals, 0.0005) * y
    return rate, x, {'bw_hz': round(bw), 'shape': shape, 'continuous': cont}


# ----------------------------------------------- broadcast FM, generic ISM devices

def music(n, rate, rng):
    """music-like audio: notes with harmonics on a scale, a bass line, drum hits (peak 1)"""
    out = np.zeros(n)
    t = 0
    scale = 110.0 * 2 ** (np.array([0, 2, 3, 5, 7, 8, 10]) / 12.0)
    while t < n:
        L = int(rng.uniform(0.08, 0.6) * rate)
        f0 = float(rng.choice(scale)) * 2 ** int(rng.integers(0, 4))
        tt = np.arange(min(L, n - t)) / rate
        env = np.exp(-tt * rng.uniform(1, 8)) * (1 - np.exp(-tt * 200))
        v = sum(rng.uniform(0.2, 1) / (h + 1) * np.sin(2 * np.pi * f0 * (h + 1) * tt + rng.uniform(0, 6.28)) for h in range(6))
        out[t:t + len(tt)] += env * v
        t += int(L * rng.uniform(0.3, 1.0))
    bpm = rng.uniform(70, 160)
    step = int(60 / bpm * rate)
    for s in range(int(rng.integers(0, step)), n, step):
        L = min(int(0.15 * rate), n - s)
        tt = np.arange(L) / rate
        out[s:s + L] += rng.uniform(0.5, 1.5) * rng.standard_normal(L) * np.exp(-tt * 30)
        out[s:s + L] += 1.2 * np.sin(2 * np.pi * 60 * tt) * np.exp(-tt * 12)
    return out / max(np.max(np.abs(out)), 1e-9)


def gen_wfm(rng, seconds):
    """broadcast FM: music / speech, pre-emphasis, a limiter, stereo MPX with the 19 kHz
    pilot and (often) an RDS-like BPSK subcarrier at 57 kHz, up to 75 kHz deviation"""
    arate, rate = 48000, 480000
    na = int(seconds * arate) + 4800
    kind = str(rng.choice(['music', 'speech', 'mixed']))
    def prog():
        if kind == 'music':
            return music(na, arate, rng)
        if kind == 'speech':
            return speech(na, arate, rng)
        return 0.6 * music(na, arate, rng) + 0.7 * speech(na, arate, rng)
    Lc = prog()
    Rc = 0.7 * Lc + 0.3 * prog() if rng.random() < 0.8 else Lc.copy()
    tau = float(rng.choice([50e-6, 75e-6]))
    out = []
    for a in (Lc, Rc):
        a = bandpass_audio(a, arate, 30, 15000)
        a = a + tau * arate * np.diff(a, prepend=a[0])        # pre-emphasis (first order)
        a = np.tanh(rng.uniform(1.5, 5) * a / max(np.max(np.abs(a)), 1e-9))   # the station's limiter
        out.append(resample_poly(a, 10, 1)[:int(seconds * rate)])
    L, R = out
    n = len(L)
    t = np.arange(n) / rate
    stereo = rng.random() < 0.8
    mpx = 0.5 * (L + R)
    if stereo:
        mpx = mpx + 0.5 * (L - R) * np.cos(2 * np.pi * 38000 * t)
    mpx = 0.9 * mpx / max(np.max(np.abs(mpx)), 1e-9)
    if stereo:
        mpx = mpx + 0.09 * np.cos(2 * np.pi * 19000 * t)
    rds = rng.random() < 0.7
    if rds:
        bits = rng.integers(0, 2, int(seconds * 1187.5) + 2) * 2 - 1
        sym = np.repeat(bits, 2)                              # biphase halves
        sym[1::2] *= -1
        idx = np.minimum((t * 2375).astype(int), len(sym) - 1)
        b = np.convolve(sym[idx].astype(float), np.hanning(101) / np.hanning(101).sum(), mode='same')
        mpx = mpx + 0.04 * b * np.cos(2 * np.pi * 57000 * t)
    dev = float(rng.uniform(45e3, 80e3))
    x = np.exp(1j * 2 * np.pi * dev * np.cumsum(mpx) / rate)
    return rate, x, {'kind': kind, 'stereo': stereo, 'rds': rds, 'dev_khz': round(dev / 1e3), 'synth': True}


def gen_ism_ook(rng, seconds):
    """a generic OOK remote / sensor: PWM / PPM / Manchester frames of random timing,
    repeated, keyed hard or soft, the oscillator pulled at key-up (cheap SAW / LC)"""
    rate = 600000
    n = int(seconds * rate)
    coding = str(rng.choice(['pwm', 'ppm', 'manchester']))
    te = float(np.exp(rng.uniform(np.log(80e-6), np.log(1000e-6))))
    nbits = int(rng.integers(12, 72))
    reps = int(rng.integers(1, 10))
    rise = float(np.exp(rng.uniform(np.log(0.3e-6), np.log(30e-6))))
    pull = rng.uniform(-30e3, 30e3) if rng.random() < 0.6 else 0.0
    pull_tau = rng.uniform(20e-6, 2e-3)
    env = np.zeros(n)
    fr = np.zeros(n)
    t = rng.uniform(0.05, 0.4)
    on_iv = []

    def key(a, b):
        i, j = int(a * rate), min(n, int(b * rate))
        if j - i < 2:
            return
        k = max(1, int(rise * rate))
        e = np.ones(j - i)
        r = 0.5 - 0.5 * np.cos(np.pi * np.arange(min(k, len(e))) / max(k, 1))
        e[:len(r)] = r
        e[len(e) - len(r):] = np.minimum(e[len(e) - len(r):], r[::-1])
        env[i:j] = np.maximum(env[i:j], e)
        tt = np.arange(j - i) / rate
        fr[i:j] += pull * np.exp(-tt / pull_tau)
    while t < seconds - 0.3:
        bits = rng.integers(0, 2, nbits)
        t0 = t
        for _ in range(reps):
            key(t, t + te)      # a sync pulse, then the long sync gap
            t += te * rng.uniform(8, 32)
            for b in bits:
                if coding == 'pwm':
                    w = te * (3 if b else 1)
                    key(t, t + w)
                    t += 4 * te
                elif coding == 'ppm':
                    key(t, t + te)
                    t += te * (4 if b else 2)
                else:
                    half = te / 2
                    key(t + (0 if b else half), t + (half if b else te))
                    t += te
            t += te * rng.uniform(4, 30)
            if t > seconds - 0.05:
                break
        on_iv.append([t0, t])
        t += rng.uniform(0.25, 1.2)
    ph = 2 * np.pi * np.cumsum(fr) / rate
    x = env * np.exp(1j * ph)
    return rate, x, {'coding': coding, 'te_us': round(te * 1e6), 'bits': nbits, 'rise_us': round(rise * 1e6, 1),
                     'pull_khz': round(pull / 1e3, 1), 'synth': True}


def gen_ism_fsk(rng, seconds):
    """a generic 2-FSK sensor / meter: preamble 1010.., a sync word, a payload, NRZ or
    Manchester, 2-100 kbit/s, +-5..80 kHz, packets of a few ms, repeated"""
    rate = 480000
    n = int(seconds * rate)
    br = float(np.exp(rng.uniform(np.log(2e3), np.log(100e3))))
    dev = float(np.clip(rng.uniform(0.5, 3.0) * br / 2, 5e3, 80e3))
    manch = rng.random() < 0.4
    gauss = rng.random() < 0.5
    sps = rate / br
    x = np.zeros(n, complex)
    t = rng.uniform(0.05, 0.4)
    while t < seconds - 0.2:
        nb = int(rng.integers(16, 64)) + 16 + 8 * int(rng.integers(6, 40))
        bits = np.concatenate([np.tile([1, 0], int(rng.integers(8, 32))), rng.integers(0, 2, nb)])
        if manch:
            bits = np.column_stack([bits, 1 - bits]).ravel()
            spb = sps / 2
        else:
            spb = sps
        L = int(len(bits) * spb)
        i0 = int(t * rate)
        if i0 + L >= n:
            break
        lev = 2.0 * bits[np.minimum((np.arange(L) / spb).astype(int), len(bits) - 1)] - 1
        if gauss:
            k = int(max(3, 2 * spb)) | 1
            g = np.exp(-0.5 * ((np.arange(k) - k // 2) / max(spb * 0.4, 1.0)) ** 2)
            lev = np.convolve(lev, g / g.sum(), mode='same')
        drift = rng.uniform(-5e3, 5e3)
        ph = 2 * np.pi * np.cumsum(dev * lev + drift) / rate
        r = max(2, int(20e-6 * rate))
        e = np.ones(L)
        e[:r] = np.linspace(0, 1, r)
        e[-r:] = np.linspace(1, 0, r)
        x[i0:i0 + L] = e * np.exp(1j * ph)
        t += L / rate + (rng.uniform(0.005, 0.05) if rng.random() < 0.5 else rng.uniform(0.25, 1.2))
    return rate, x, {'br': round(br), 'dev_khz': round(dev / 1e3, 1), 'manchester': manch, 'gauss': gauss, 'synth': True}


KINDS = {'wfm': gen_wfm, 'ism_ook': gen_ism_ook, 'ism_fsk': gen_ism_fsk, 'am': gen_am, 'ssb': gen_ssb, 'cw': gen_cw, 'carrier': gen_carrier, 'unk_fsk': gen_unk_fsk,
         'unk_psk': gen_unk_psk, 'unk_ofdm': gen_unk_ofdm, 'unk_pulse': gen_unk_pulse, 'unk_noise': gen_unk_noise}


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in KINDS:
        print(__doc__)
        sys.exit(2)
    kind, out = a[0], a[1]
    seed, seconds = 1, 10.0
    i = 2
    while i < len(a):
        if a[i] == '--seed':
            seed = int(a[i + 1])
        elif a[i] == '--seconds':
            seconds = float(a[i + 1])
        else:
            sys.exit('unknown option ' + a[i])
        i += 2
    rng = np.random.default_rng(seed)
    rate, x, variant = KINDS[kind](rng, seconds)
    x = np.asarray(x, np.complex128)
    x = x / max(np.sqrt(np.mean(np.abs(x[np.abs(x) > 0]) ** 2)) if np.any(np.abs(x) > 0) else 1.0, 1e-12) * 0.3
    x.astype(np.complex64).tofile(out)
    with open(out.rsplit('.', 1)[0] + '.json', 'w') as fh:
        json.dump({'rate': rate, 'variant': variant}, fh)
    print('%s: %s %.1f s at %d Hz %s' % (out, kind, seconds, rate, json.dumps(variant)))


if __name__ == '__main__':
    main()
