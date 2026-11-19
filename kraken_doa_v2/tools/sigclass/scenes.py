#!/usr/bin/env python3
"""Signal classifier training data: the scene composer. Turns the clean
signal bank (bank.py) into what the receiver sees - 2.4 MHz, 8-bit IQ
captures with 0..K signals at random offsets, levels and impairments - and
writes the ground truth of every capture.

  scenes.py --count N [--out DIR] [--bank DIR] [--seconds 0.5] [--batch 400]
            [--jobs 12] [--seed S] [--first I] [--max-gb 4] [--classes a,b]
            [--consume "CMD {batch}"] [--keep]
  scenes.py --preview [--out DIR]          a dozen captures + a text summary
  scenes.py --single CLASS --offset HZ --snr DB --seconds S [--clip ID]
            [--no-fading] --out DIR       one signal (decoder checks)

Output: DIR/batch_NNNNN/cap_IIIIIIIII.cu8 (I/Q bytes, 127.5 = 0, like the
RTL-SDR) + DIR/batch_NNNNN/truth.jsonl (one line per capture, see TRUTH
below). A batch folder appears under its final name only when complete
(written as batch_NNNNN.tmp first); the consumer reads it and deletes it.
The composer waits while the batches on disk exceed --max-gb. --consume
runs CMD on each finished batch ({batch} = its path) and deletes the batch
when CMD succeeds (unless --keep). Capture I depends only on (--seed, I), so
any range can be regenerated. DIR defaults to /dev/shm/kraken_sigclass/scenes.

How a capture is made (all signals in the frequency domain, one inverse FFT):
  the signals: 4 % none; else 1-8 (30 / 20 / 15 / 10 / 8 / 7 / 5 / 5 %), the
  first of any class (uniform), the others narrow; a wide first one (99 %
  bandwidth > 600 kHz or band-filling) gets at most 3 narrow ones on top
  placement: 25 % one channel spacing (12.5 / 25 kHz, 8.33 for AM, +-300
  Hz) from a narrow one already there, else ~11 % at / near DC, ~11 % at the
  band edge (partly beyond it: those bins are lost, not folded), the rest
  anywhere; narrow signals don't overlap (5 %, adjacent 25 % of the
  narrower); band-filling clips move only +-20 kHz (their generators
  already placed the 2.4 MHz window in the channel)
  levels: SNR in the signal's bandwidth 70 % U(5, 25), 30 % U(-3, 45) dB;
  at most 40 dB between the signals' powers (the weak one raised)
  each signal: a clip of its class, a time cut (bursts: 55 % one fully
  inside, 25 % cut by the capture's edge, 15 % anywhere, 5 % absent - the
  truth's on_air), the native-rate segment with flat fading (50 % static,
  30 % Rician K 0-10 dB, 20 % Rayleigh; Doppler 0.3-100 Hz log-uniform),
  Wiener phase noise (linewidth 0.01-5 Hz), soft burst edges (0.2-1 ms,
  narrow keyed classes), its FFT bins placed at the offset - taking
  (1 + ppm) x the samples maps a sample-rate error of up to ~+-50 ppm
  exactly onto the 2.4 MHz bins (whole samples; none for clips at >= 1.2
  MHz) - and in 15 % a 2-3 path multipath response (paths -3..-15 dB,
  delays 0.1-1.5 / bandwidth, wide signals 0.2-6 us)
  the receiver: white noise 3-40 LSB rms per I / Q (log-uniform: the gain
  setting; in 30 % 0.3-3 LSB, a quiet antenna at full gain), passband droop 0.5-6 dB at +-1.2 MHz (|f|^3..10), in 12 % 1-2
  spurs (clean carriers at 8-40 dB in 1 kHz, labelled carrier), IQ
  imbalance +-0.5 dB / +-3 deg, DC offset +-3 LSB, 8 bits (floor(x + 128),
  0..255); everything scaled down (a lower gain) when the signals' peaks +
  3 noise rms would pass 105 LSB, except in 6 % of captures, driven 1.2-2.5x
  past it (clipping)

TRUTH (per capture): {"file", "index", "seconds", "rate", "noise_lsb" (rms
per I / Q after gain + quantisation), "rx": {...}, "signals": [{"class",
"clip", "offset_hz" (the signal's 99 % band centre), "bw_hz" (the SNR
bandwidth: its 99 % bandwidth clamped to the class's range), "lo_hz",
"hi_hz" (its 99 % band edges), "snr_db" (signal while on air / noise in
bw_hz), "peak_snr_db", "sinr_avg_db" (its energy over the whole capture
against the noise + the other signals inside its band), "on_air" (fraction
of the capture it transmits), "in_band" (fraction of its power inside the
2.4 MHz; null when nothing of it is in the cut), "variant", "ppm",
"fading", "multipath", "spur"}]}. numpy + scipy only.
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from collections import deque
from multiprocessing import Pool

import numpy as np
import scipy.fft as sfft

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bank import default_work, load_classes  # noqa: E402

FS = 2400000.0
MARGIN_S = 0.025      # each side: the segments' circular-FFT edges fall in it (cut off)

# ------------------------------------------------------------------ the bank

_IDX = None
_BY = None
_CLS = None
_MM = {}
_FB = {}


def init_worker(bank, classes_sel):
    global _IDX, _BY, _CLS
    _IDX = json.load(open(os.path.join(bank, 'index.json')))
    _CLS = {c['id']: c for c in load_classes()}
    _BY = {}
    for m in _IDX:
        if classes_sel and m['class'] not in classes_sel:
            continue
        m['_path'] = os.path.join(bank, m['path'])
        _BY.setdefault(m['class'], []).append(m)
    for m in _IDX:
        c = _CLS.get(m['class'], {})
        m['_wide'] = bool(c.get('fills_band')) or m['bw99_hz'] > 600e3
        lo, hi = c.get('occ_bw_hz', [1000, 2.4e6])
        m['_bw'] = float(min(max(m['bw99_hz'], lo, 1000.0), hi, 2.4e6))
        m['_iv'] = np.array(m['intervals'], float).reshape(-1, 2)


def clip_data(m):
    x = _MM.get(m['_path'])
    if x is None:
        if len(_MM) > 200:
            _MM.clear()
        x = np.memmap(m['_path'], dtype=np.complex64, mode='r')
        _MM[m['_path']] = x
    return x


def segment(m, t0, n):
    """n native samples from clip time t0 (zeros outside the clip)"""
    x = clip_data(m)
    i0 = int(round(t0 * m['rate']))
    out = np.zeros(n, np.complex128)
    a, b = max(0, i0), min(len(x), i0 + n)
    if b > a:
        out[a - i0:b - i0] = x[a:b]
    return out


def overlap(iv, a, b):
    if not len(iv):
        return 0.0
    return float(np.sum(np.clip(np.minimum(iv[:, 1], b) - np.maximum(iv[:, 0], a), 0, None)))


def pick_time(rng, m, T_seg, T_cap, margin, mode=None):
    """the segment's start in clip time and the capture window's on-air fraction"""
    dur = m['seconds']
    iv = m['_iv']
    span = max(dur - T_seg, 0.0)
    if m['continuous'] or not len(iv):
        t0 = rng.uniform(0, span) if span > 0 else 0.0
    else:
        mode = mode or rng.choice(['full', 'partial', 'random', 'absent'], p=[0.55, 0.25, 0.15, 0.05])
        k = int(rng.integers(0, len(iv)))
        a, b = iv[k]
        if mode == 'full':
            if b - a <= T_cap:
                c0 = rng.uniform(b - T_cap, a)          # capture window start
            else:
                c0 = rng.uniform(a, b - T_cap)
        elif mode == 'partial':
            edge = a if rng.random() < 0.5 else b
            c0 = edge - rng.uniform(0.1, 0.9) * T_cap
        elif mode == 'absent':
            gaps = [(iv[i, 1], iv[i + 1, 0]) for i in range(len(iv) - 1) if iv[i + 1, 0] - iv[i, 1] > T_cap]
            if iv[0, 0] > T_cap + margin:
                gaps.append((margin, iv[0, 0]))
            if gaps:
                g = gaps[int(rng.integers(0, len(gaps)))]
                c0 = rng.uniform(g[0], g[1] - T_cap)
            else:
                c0 = rng.uniform(0, max(dur - T_cap, 0))
        else:
            c0 = rng.uniform(0, max(dur - T_cap, 0))
        t0 = float(np.clip(c0 - margin, -margin, max(span, 0) + margin))
    c0 = t0 + margin
    return t0, overlap(iv, c0, c0 + T_cap) / T_cap


# ------------------------------------------------------------------ impairments

def fading(rng, n, rate, T):
    """complex gain over n native samples (mean power 1) + its description"""
    r = rng.random()
    if r < 0.5:
        return None, {'type': 'static'}
    fd = float(np.exp(rng.uniform(np.log(0.3), np.log(100))))
    K = 0.0 if r > 0.8 else float(10 ** (rng.uniform(0, 10) / 10))
    L = max(16, int(T * 2000))
    t = np.linspace(0, T, L)
    M = 12
    a = rng.uniform(0, 2 * np.pi, M)
    ph = rng.uniform(0, 2 * np.pi, M)
    h = np.sum(np.exp(1j * (2 * np.pi * fd * np.cos(a)[:, None] * t[None, :] + ph[:, None])), axis=0) / np.sqrt(M)
    los = np.exp(1j * (2 * np.pi * fd * np.cos(rng.uniform(0, 2 * np.pi)) * t + rng.uniform(0, 2 * np.pi)))
    g = np.sqrt(K / (K + 1)) * los + np.sqrt(1 / (K + 1)) * h
    g = g / np.sqrt(np.mean(np.abs(g) ** 2))
    tt = np.arange(n) / rate
    gi = np.interp(tt, t, g.real) + 1j * np.interp(tt, t, g.imag)
    return gi, {'type': 'rayleigh' if K == 0 else 'rician', 'doppler_hz': round(fd, 2), 'k_db': round(10 * np.log10(K), 1) if K else None}


def phase_noise(rng, n, rate):
    beta = float(np.exp(rng.uniform(np.log(0.01), np.log(5.0))))
    return np.exp(1j * np.cumsum(rng.standard_normal(n)) * np.sqrt(2 * np.pi * beta / rate))


def edge_ramps(m, t0, n, rate, ramp_s):
    """soft on / off edges at the clip's interval ends inside the segment"""
    e = None
    k = max(2, int(ramp_s * rate))
    up = 0.5 - 0.5 * np.cos(np.pi * np.arange(k) / k)
    for a, b in m['_iv']:
        for edge, rise in ((a, True), (b, False)):
            i = int(round((edge - t0) * rate))
            if -k < i < n + k:
                if e is None:
                    e = np.ones(n)
                lo, hi = (i, i + k) if rise else (i - k, i)
                seg = up if rise else up[::-1]
                s0, s1 = max(lo, 0), min(hi, n)
                if s1 > s0:
                    e[s0:s1] = np.minimum(e[s0:s1], seg[s0 - lo:s1 - lo])
    return e


def multipath(rng, bw, wide):
    """2-3 paths: (relative complex gain, delay s) with delays ~ 0.1-1.5 / bw"""
    paths = []
    for _ in range(int(rng.integers(1, 3))):
        d = rng.uniform(0.1, 1.5) / max(bw, 2e3) if not wide else rng.uniform(0.2e-6, 6e-6)
        g = 10 ** (rng.uniform(-15, -3) / 20) * np.exp(1j * rng.uniform(0, 2 * np.pi))
        paths.append((g, d))
    return paths


# ------------------------------------------------------------------ one capture

def choose_signals(rng, opt):
    classes = sorted(_BY)
    if opt.get('single'):
        return [opt['single']]
    r = rng.random()
    if r < 0.04:
        return []
    K = int(rng.choice([1, 2, 3, 4, 5, 6, 7, 8], p=[0.3, 0.2, 0.15, 0.1, 0.08, 0.07, 0.05, 0.05]))
    first = classes[int(rng.integers(0, len(classes)))]
    out = [first]
    narrow = [c for c in classes if not all(m['_wide'] for m in _BY[c])]
    wide_first = all(m['_wide'] for m in _BY[first])
    if wide_first:
        K = min(K, 1 + int(rng.choice([0, 0, 1, 2, 3])))
    if not narrow:
        K = 1
    for _ in range(K - 1):
        out.append(narrow[int(rng.integers(0, len(narrow)))])
    return out


def place(rng, m, placed, bw, wide, opt):
    """the offset of the signal's band centre (Hz)"""
    if opt.get('offset') is not None:
        return float(opt['offset'])
    lo, hi = m['occ_lo_hz'] - m['centre_hz'], m['occ_hi_hz'] - m['centre_hz']
    if wide:
        if m['rate'] >= FS - 1 and (_CLS[m['class']].get('fills_band') or m['bw99_hz'] > 1.8e6):
            return float(rng.uniform(-20e3, 20e3))
        room = max(0.0, (FS * 0.98 - (hi - lo)) / 2)
        return float(rng.uniform(-room, room))
    for _ in range(30):
        nb = [p for p in placed if not p['wide']]
        if nb and opt.get('cochannel') is not None:
            # on a channel already used, a few hundred Hz off (transmitters taking turns /
            # overlapping on one channel, as on a repeater input or a busy simplex channel)
            p = opt['cochannel']
            return float(p['offset'] + rng.uniform(-1500, 1500))
        r = 0.0 if (nb and rng.random() < 0.25) else rng.uniform(0.25, 1.0)
        if r < 0.25:        # a neighbour one channel spacing away
            p = nb[int(rng.integers(0, len(nb)))]
            sp = float(rng.choice([12500.0, 25000.0] + ([8333.33] if m['class'] == 'am_voice' else [])))
            f = p['offset'] + rng.choice([-1, 1]) * sp + rng.normal(0, 300)
            adj = True
        elif r < 0.33:      # at / near DC
            f = rng.uniform(-1, 1) * max(bw, 5e3)
            adj = False
        elif r < 0.41:      # at the band edge, sometimes partly beyond it
            s = rng.choice([-1, 1])
            f = s * (FS / 2 - rng.uniform(-0.3, 0.8) * (hi - lo))
            adj = False
        else:
            f = rng.uniform(-FS / 2 * 0.98 - lo, FS / 2 * 0.98 - hi)
            adj = False
        a, b = f + lo, f + hi
        ok = True
        for p in placed:
            ov = min(b, p['hi']) - max(a, p['lo'])
            if p['wide']:       # narrow signals may sit on top of a wide one
                continue
            if ov > (0.25 if adj else 0.05) * min(b - a, p['hi'] - p['lo']):
                ok = False
        if ok:
            return float(f)
    return None


def make_capture(index, opt):
    rng = np.random.default_rng([opt['seed'], index])
    T_cap = opt['seconds']
    N = int(round(T_cap * FS))
    Mg = int(round(MARGIN_S * FS))
    Nb = N + 2 * Mg
    Nb += Nb % 2
    df = FS / Nb
    sig_classes = choose_signals(rng, opt)
    # the receiver noise (the gain setting): 3-40 LSB rms, and in 30 % 0.3-3 LSB - an
    # RTL-SDR with a quiet antenna is at 0.6-1 LSB even at full gain (own off-air
    # captures, 2026-11-20): the 8-bit quantiser is then barely dithered
    sigma = float(np.exp(rng.uniform(np.log(0.3), np.log(3)))) if rng.random() < 0.3 else float(np.exp(rng.uniform(np.log(3), np.log(40))))
    X = np.zeros(Nb, np.complex64)
    placed = []
    truth = []
    for si, cls in enumerate(sig_classes):
        # 12 % of the later narrow signals share a channel with an earlier one (half of
        # them the same class: two radios of one system taking turns)
        opt['cochannel'] = None
        nbp = [p for p in placed if not p['wide']]
        if si > 0 and nbp and not opt.get('single') and rng.random() < 0.12:
            opt['cochannel'] = nbp[int(rng.integers(0, len(nbp)))]
            if rng.random() < 0.5 and not all(m['_wide'] for m in _BY[opt['cochannel']['cls']]):
                cls = opt['cochannel']['cls']
        clips = _BY[cls]
        if opt.get('clip'):
            clips = [c for c in clips if c['clip'] == opt['clip']] or clips
        m = clips[int(rng.integers(0, len(clips)))]
        wide = m['_wide']
        bw = m['_bw']
        f = place(rng, m, placed, bw, wide, opt)
        if f is None:
            continue
        lo, hi = m['occ_lo_hz'] - m['centre_hz'], m['occ_hi_hz'] - m['centre_hz']
        placed.append({'offset': f, 'lo': f + lo, 'hi': f + hi, 'wide': wide, 'fills': bool(_CLS[cls].get('fills_band')), 'cls': cls})
        # sample-rate error: (1 + ppm) x the native samples into the same output time - exact
        # in whole samples (at 48 kHz one sample of a 0.55 s segment = 38 ppm); none for clips
        # at >= 1.2 MHz (an FFT of an arbitrary length near 1.3 M is slow)
        nk = int(round(Nb * m['rate'] / FS))
        msamp = nk if m['rate'] >= 1.2e6 else int(round(nk * (1 + rng.uniform(-50, 50) * 1e-6)))
        ppm = (msamp / nk - 1) * 1e6
        T_seg = msamp / m['rate']
        t0, on_air = pick_time(rng, m, T_seg, T_cap * (1 + ppm * 1e-6), MARGIN_S * (1 + ppm * 1e-6), opt.get('time_mode'))
        x = segment(m, t0, msamp)
        soft = not m['continuous'] and m['rate'] <= 100e3 and cls not in ('wildlife', 'cw', 'unk_pulse', 'carrier')
        e = edge_ramps(m, t0, msamp, m['rate'], rng.uniform(2e-4, 1e-3)) if soft else None
        if e is not None:
            x *= e
        fad, fdesc = (None, {'type': 'static'}) if opt.get('no_fading') else fading(rng, msamp, m['rate'], T_seg)
        if fad is not None:
            x *= fad
        x *= phase_noise(rng, msamp, m['rate'])
        S = sfft.fft(x.astype(np.complex64), workers=1) if msamp > 100000 else np.fft.fft(x)
        jb = (np.arange(msamp, dtype=np.int64) + msamp // 2) % msamp - msamp // 2      # signed native bins, FFT order
        fr = jb * df                                                    # their output frequency (rel. to the clip's 0 Hz)
        mp = []
        if not opt.get('no_fading') and rng.random() < 0.15:
            mp = multipath(rng, bw, wide)
            H = np.ones(msamp, np.complex128)
            for g, d in mp:
                H += g * np.exp(-2j * np.pi * fr * d)
            S *= H / np.sqrt(1 + sum(abs(g) ** 2 for g, d in mp))
        # the clip's centre (its 99 % band middle) goes to f
        sb = int(round((f - m['centre_hz']) / df))
        ob = jb + sb
        keep = np.abs(ob) < Nb // 2
        p_all = float(np.sum(np.abs(S) ** 2))
        p_in = float(np.sum(np.abs(S[keep]) ** 2))
        snr_db = opt['snr'] if opt.get('snr') is not None else \
            (float(rng.uniform(5, 25)) if rng.random() < 0.7 else float(rng.uniform(-3, 45)))
        truth.append({'class': cls, 'clip': m['clip'], 'offset_hz': round(f + 0.0, 1), 'bw_hz': round(bw, 1),
                      'lo_hz': round(f + lo, 1), 'hi_hz': round(f + hi, 1), 'snr_db': snr_db, 'on_air': round(on_air, 4),
                      'in_band': round(p_in / p_all, 4) if p_all > 0 else None, 'variant': m.get('variant'), 'ppm': round(ppm, 1),
                      'fading': fdesc, 'multipath': [[round(abs(g), 3), round(d * 1e6, 3)] for g, d in mp], 'spur': False,
                      '_S': S[keep], '_ob': ob[keep], '_scale': Nb / msamp, '_peak': m['peak_db'], '_bwr': bw})
    # spurs: clean carriers (labelled carrier)
    if not opt.get('single') and rng.random() < 0.12:
        for _ in range(int(rng.integers(1, 3))):
            f = float(rng.uniform(-FS / 2 * 0.97, FS / 2 * 0.97))
            truth.append({'class': 'carrier', 'clip': None, 'offset_hz': round(f, 1), 'bw_hz': 1000.0, 'lo_hz': round(f, 1),
                          'hi_hz': round(f, 1), 'snr_db': float(rng.uniform(8, 40)), 'on_air': 1.0, 'in_band': 1.0, 'variant': {'spur': True},
                          'ppm': 0.0, 'fading': {'type': 'static'}, 'multipath': [], 'spur': True, '_bwr': 1000.0, '_peak': 0.0})
    # levels: power while on = SNR x noise in bw; at most 40 dB between signals
    pw = [10 ** (t['snr_db'] / 10) * t['_bwr'] for t in truth]
    if pw:
        top = max(pw)
        for t, p in zip(truth, pw):
            if p < top * 1e-4:
                t['snr_db'] = 10 * np.log10(top * 1e-4 / t['_bwr'])
    # the gain: the peak of an I or Q sample ~ the signals' peak amplitudes + 3 noise rms;
    # pulled down to stay under full scale (127.5), except in ~6 % let clip
    noise_p = 2 * sigma ** 2
    pk = np.sqrt(sum(10 ** (t['snr_db'] / 10) * noise_p * t['_bwr'] / FS * 10 ** (max(t['_peak'], 0) / 10)
                     for t in truth if t['on_air'] > 0)) + 3 * sigma
    clip_mode = (not opt.get('single')) and rng.random() < 0.06
    g = (rng.uniform(1.2, 2.5) * 105.0 / pk) if clip_mode else min(1.0, 105.0 / pk)
    sigma *= g
    noise_p = 2 * sigma ** 2
    X.real += rng.standard_normal(Nb, dtype=np.float32) * np.float32(sigma * np.sqrt(Nb))      # white noise, drawn as its spectrum
    X.imag += rng.standard_normal(Nb, dtype=np.float32) * np.float32(sigma * np.sqrt(Nb))
    spurs = []
    for t in truth:
        a = np.sqrt(10 ** (t['snr_db'] / 10) * noise_p * t['_bwr'] / FS)      # clips are at power 1 while on
        if t['spur']:
            spurs.append((t['offset_hz'], a))
            continue
        X[t['_ob'] % Nb] += t['_S'] * (a * t['_scale'])       # the bins are distinct
    # receiver: droop toward the band edges
    droop_db = float(rng.uniform(0.5, 6.0))
    p_ex = float(rng.uniform(3, 10))
    fb = _FB.get(Nb)
    if fb is None:
        fb = _FB[Nb] = np.abs(np.fft.fftfreq(Nb, 1 / FS) / (FS / 2)).astype(np.float32)
    Hd = np.power(np.float32(10), np.float32(-droop_db / 20) * fb ** np.float32(p_ex))
    # SINR (averaged over the whole capture, off-air time included): the signal's energy in its
    # band against the noise + the other signals there, from the spectrum before the droop
    P = np.abs(X) ** 2
    for t in truth:
        if t['spur']:
            continue
        a = np.sqrt(10 ** (t['snr_db'] / 10) * noise_p * t['_bwr'] / FS) * t['_scale']
        ob = t['_ob']
        half = max(t['hi_hz'] - t['lo_hz'], t['bw_hz']) / 2      # its 99 % band, at least the SNR bandwidth
        mid = (t['hi_hz'] + t['lo_hz']) / 2
        lo_b, hi_b = int(round((mid - half) / df)), int(round((mid + half) / df))
        sel = (ob >= lo_b) & (ob <= hi_b)
        own = float(np.sum(np.abs(t['_S'][sel] * a) ** 2))
        bins = np.arange(max(lo_b, -Nb // 2 + 1), min(hi_b, Nb // 2 - 1) + 1) % Nb
        tot_b = float(np.sum(P[bins]))
        t['_sinr'] = own / max(tot_b - own, 1e-30)
    X *= Hd
    y = sfft.ifft(X, workers=1)[Mg:Mg + N].astype(np.complex128)
    tt = np.arange(N) / FS
    for f, a in spurs:
        y += a * np.exp(1j * (2 * np.pi * f * tt + rng.uniform(0, 2 * np.pi)))
    # IQ imbalance, DC offset, 8 bits
    ge = 10 ** (rng.uniform(-0.5, 0.5) / 20)
    phe = np.deg2rad(rng.uniform(-3, 3))
    I = y.real * np.sqrt(ge)
    Q = (y.imag * np.cos(phe) + y.real * np.sin(phe)) / np.sqrt(ge)
    dc = rng.uniform(-3, 3, 2)
    I += dc[0]
    Q += dc[1]
    iq = np.empty(2 * N, np.float64)
    iq[0::2] = I
    iq[1::2] = Q
    q = np.floor(iq + 128.0)
    clipped = float(np.mean((q < 0) | (q > 255)))
    u8 = np.clip(q, 0, 255).astype(np.uint8)
    n_eff = np.sqrt(sigma ** 2 + 1 / 12.0)
    sigs = []
    for t in truth:
        t['snr_db'] = round(float(t['snr_db'] + 10 * np.log10(sigma ** 2 / n_eff ** 2)), 2)
        t['peak_snr_db'] = round(t['snr_db'] + float(t['_peak']), 2)
        t['sinr_avg_db'] = round(float(10 * np.log10(max(t['_sinr'], 1e-12) * sigma ** 2 / n_eff ** 2)), 2) if '_sinr' in t else t['snr_db']
        sigs.append({k: v for k, v in t.items() if not k.startswith('_')})
    rx = {'gain_scale_db': round(20 * np.log10(g), 2), 'dc_lsb': [round(float(d), 2) for d in dc], 'iq_gain_db': round(20 * np.log10(ge), 3),
          'iq_phase_deg': round(float(np.rad2deg(phe)), 2), 'droop_db': round(droop_db, 2), 'clip_mode': clip_mode, 'clipped': round(clipped, 6)}
    return u8, {'index': index, 'seconds': T_cap, 'rate': FS, 'noise_lsb': round(float(n_eff), 3), 'rx': rx, 'signals': sigs}


# ------------------------------------------------------------------ batches

_OPT = None


def worker_init(bank, opt):
    global _OPT
    _OPT = opt
    init_worker(bank, set(opt['classes']) if opt.get('classes') else None)


def work_one(args):
    index, path = args
    u8, tr = make_capture(index, _OPT)
    fn = 'cap_%09d.cu8' % index
    u8.tofile(os.path.join(path, fn))
    tr['file'] = fn
    return index, tr


def dir_bytes(root):
    tot = 0
    for d in os.listdir(root):
        p = os.path.join(root, d)
        if d.startswith('batch_') and os.path.isdir(p):
            tot += sum(os.path.getsize(os.path.join(p, f)) for f in os.listdir(p))
    return tot


def run_batches(o, opt, bank):
    out = os.path.abspath(o.out)
    os.makedirs(out, exist_ok=True)
    first, count, bs = o.first, o.count, o.batch
    batches = []
    for b in range((count + bs - 1) // bs):
        i0 = first + b * bs
        name = os.path.join(out, 'batch_%05d' % (i0 // bs if first % bs == 0 else b))
        if os.path.exists(name):
            print('%s exists - skipped' % name, flush=True)
            continue
        batches.append((name, i0, min(first + count, i0 + bs)))
    tasks = [(k, i) for k, (name, i0, i1) in enumerate(batches) for i in range(i0, i1)]
    t0 = time.time()
    done = 0
    res = [dict() for _ in batches]
    opened = set()
    consumers = []
    window = 4 * o.jobs           # captures in flight: the workers never wait for a batch to close
    with Pool(o.jobs, initializer=worker_init, initargs=(bank, opt)) as pool:
        q = deque()
        nxt = 0
        while nxt < len(tasks) or q:
            while nxt < len(tasks) and len(q) < window:
                k, i = tasks[nxt]
                if k not in opened:
                    while o.max_gb and dir_bytes(out) > o.max_gb * 1e9:
                        time.sleep(1.0)
                    shutil.rmtree(batches[k][0] + '.tmp', ignore_errors=True)
                    os.makedirs(batches[k][0] + '.tmp')
                    opened.add(k)
                q.append((k, pool.apply_async(work_one, ((i, batches[k][0] + '.tmp'),))))
                nxt += 1
            k, ar = q.popleft()
            i, tr = ar.get()
            res[k][i] = tr
            name, i0, i1 = batches[k]
            if len(res[k]) < i1 - i0:
                continue
            with open(os.path.join(name + '.tmp', 'truth.jsonl'), 'w') as fh:
                for j in range(i0, i1):
                    fh.write(json.dumps(res[k][j]) + '\n')
            os.replace(name + '.tmp', name)
            res[k] = None
            done += i1 - i0
            print('%s: %d captures (%d of %d, %.1f captures/s)' % (name, i1 - i0, done, len(tasks), done / (time.time() - t0)), flush=True)
            if o.consume:
                # the consumer runs beside the composer (at most --consumers at a time)
                while len(consumers) >= max(1, o.consumers):
                    reap(consumers, o, block=True)
                cmd = [c.replace('{batch}', name) for c in shlex.split(o.consume)]
                consumers.append((name, subprocess.Popen(cmd)))
            reap(consumers, o, block=False)
    while consumers:
        reap(consumers, o, block=True)
    print('%d captures in %.0f s (%.1f captures/s)' % (done, time.time() - t0, done / max(time.time() - t0, 1e-9)))


def reap(consumers, o, block):
    """finished consumers: check them, delete their batches (block: wait for the oldest)"""
    for name, p in list(consumers):
        if block and (name, p) == consumers[0]:
            p.wait()
        if p.poll() is None:
            continue
        consumers.remove((name, p))
        if p.returncode != 0:
            sys.exit('consumer failed on %s (exit %d)' % (name, p.returncode))
        if not o.keep:
            shutil.rmtree(name, ignore_errors=True)
        if block:
            return


def preview(o, opt, bank):
    out = os.path.abspath(o.out)
    os.makedirs(out, exist_ok=True)
    worker_init(bank, opt)
    lines = []
    t = time.time()
    for i in range(o.first, o.first + 12):
        u8, tr = make_capture(i, opt)
        fn = 'cap_%09d.cu8' % i
        u8.tofile(os.path.join(out, fn))
        tr['file'] = fn
        with open(os.path.join(out, 'truth.jsonl'), 'a') as fh:
            fh.write(json.dumps(tr) + '\n')
        v = u8.astype(np.float32) - 127.5
        z = v[0::2] + 1j * v[1::2]
        spec = np.abs(np.fft.fftshift(np.fft.fft(z[:1 << 20].reshape(-1, 4096), axis=1), axes=1)) ** 2
        spec = 10 * np.log10(spec.mean(axis=0) + 1e-9)
        bars = ''.join(' .:-=+*#%@'[int(np.clip((s - np.median(spec)) / 4, 0, 9))] for s in spec.reshape(64, 64).max(axis=1))
        lines.append('%s  noise %.1f LSB  rms %.1f  clipped %.4f%%  gain %+.1f dB' % (fn, tr['noise_lsb'], np.std(v), 100 * tr['rx']['clipped'],
                                                                                 tr['rx']['gain_scale_db']))
        lines.append('  |%s|  (-1.2 .. +1.2 MHz, 37.5 kHz a column, 4 dB a step over the median)' % bars)
        for s in tr['signals']:
            lines.append('  %-10s %+9.1f kHz  bw %7.1f kHz  SNR %5.1f dB  SINR %5.1f  on %.2f  in %4s  %s %s%s' % (
                s['class'], s['offset_hz'] / 1e3, s['bw_hz'] / 1e3, s['snr_db'], s['sinr_avg_db'], s['on_air'],
                '-' if s['in_band'] is None else '%.2f' % s['in_band'],
                s['fading']['type'], 'mp%d ' % len(s['multipath']) if s['multipath'] else '', json.dumps(s['variant'])[:60] if s['variant'] else ''))
    el = time.time() - t
    lines.append('12 captures in %.1f s (one process)' % el)
    txt = '\n'.join(lines)
    with open(os.path.join(out, 'summary.txt'), 'w') as fh:
        fh.write(txt + '\n')
    print(txt)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--bank', default=None)
    ap.add_argument('--work', default=default_work())
    ap.add_argument('--out', default='/dev/shm/kraken_sigclass/scenes')
    ap.add_argument('--count', type=int, default=0)
    ap.add_argument('--first', type=int, default=0)
    ap.add_argument('--seconds', type=float, default=0.5)
    ap.add_argument('--batch', type=int, default=400)
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--max-gb', type=float, default=4.0)
    ap.add_argument('--classes', default='')
    ap.add_argument('--consume', default='')
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--consumers', type=int, default=2, help='consumers running at a time')
    ap.add_argument('--preview', action='store_true')
    ap.add_argument('--single', default=None, help='one signal of this class')
    ap.add_argument('--clip', default=None)
    ap.add_argument('--offset', type=float, default=None)
    ap.add_argument('--snr', type=float, default=None)
    ap.add_argument('--no-fading', action='store_true')
    o = ap.parse_args()
    bank = os.path.abspath(o.bank or os.path.join(o.work, 'bank'))
    if not os.path.exists(os.path.join(bank, 'index.json')):
        sys.exit('no bank index in %s: run bank.py first' % bank)
    opt = {'seed': o.seed, 'seconds': o.seconds, 'classes': [c for c in o.classes.split(',') if c], 'single': o.single,
           'clip': o.clip, 'offset': o.offset, 'snr': o.snr, 'no_fading': o.no_fading}
    if o.single:
        opt['time_mode'] = 'random'
        worker_init(bank, opt)
        os.makedirs(o.out, exist_ok=True)
        u8, tr = make_capture(o.first, opt)
        fn = os.path.join(o.out, 'single_%s.cu8' % o.single)
        u8.tofile(fn)
        with open(fn[:-4] + '.json', 'w') as fh:
            json.dump(dict(tr, file=os.path.basename(fn), channels=1), fh, indent=1)
        print('%s: %s' % (fn, json.dumps(tr['signals'])))
        return
    if o.preview:
        preview(o, opt, bank)
        return
    if o.count <= 0:
        sys.exit('--count N (or --preview / --single)')
    run_batches(o, opt, bank)


if __name__ == '__main__':
    main()
