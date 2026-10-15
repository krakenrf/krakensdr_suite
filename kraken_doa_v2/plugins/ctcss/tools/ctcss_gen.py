#!/usr/bin/env python3
"""Analog FM test transmissions with CTCSS / DCS for the ctcss plugin: one
transmitter's complex baseband (cf32), keyed on and off, with speech-like
(or real speech) voice through a radio's transmit audio chain, a CTCSS tone
(+ reverse burst) or a DCS code (+ turn-off code), as ETSI TS 103 236
describes them. Several transmitters at known bearings: one file each, put
on the array by plugins/sdk/tools/uca_scene.py ("at": 0 - the file keeps its
own timing).

  ctcss_gen.py OUT.cf32 [--seconds 20] [--rate 48000] [--tx 1:6,8.5:12]
               [--ctcss 88.5 | --dcs 023N | (neither: carrier squelch)]
               [--tone-dev 600] [--voice-dev 3000] [--narrow]
               [--speech a.wav[,b.wav]] [--no-hpf] [--deemph]
               [--tail reverse|reverse120|off|turnoff|none]
               [--tone-err PCT] [--baud-err PCT] [--offset-hz HZ] [--cnr DB] [--seed N]

--tx      carrier on from A to B seconds (several: comma-separated)
--narrow  12.5 kHz channel: voice 1500 Hz, tone 350 Hz peak deviation
--speech  mono WAV(s), looped; default: synthetic speech (pitch 90-220 Hz,
          moving formants, syllables and pauses)
--no-hpf  no 300 Hz voice high-pass in the transmitter (a radio that lets
          the voice into the tone band - the false-tone test)
--deemph  the whole audio (voice + tone) through a 750 us de-emphasis, as a
          repeater's audio chain may pass it
--cnr     add noise: carrier to noise in a 16 kHz channel (dB), over the whole
          file (noise only while the carrier is off)
--tail    end of every transmission: reverse = the tone's phase turned 180 deg
          for the last 0.18 s (CTCSS default), reverse120 = 120 deg, off = the
          tone stops 0.15 s before the carrier, turnoff = DCS turn-off code
          (134.4 Hz for 0.18 s, DCS default), none
Writes OUT.json: rate + the truth (each transmission's start / end / id).
"""
import json
import sys
import wave
from fractions import Fraction

import numpy as np
from scipy.signal import bilinear, butter, lfilter, resample_poly

GOLAY_G = 0xC75


def dcs_word(code):
    """time-order 23-bit word of an octal DCS code (bit 0 sent first)"""
    d = (code & 0x1FF) | (4 << 9)
    for p in range(2048):
        w = d | (p << 12)
        v = w
        for b in range(22, 10, -1):
            if v & (1 << b):
                v ^= GOLAY_G << (b - 11)
        if v & 0x7FF == 0:
            return w
    raise ValueError('no codeword')


def load_wav(path, rate):
    w = wave.open(path)
    n, ch, sw, r = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
    d = np.frombuffer(w.readframes(n), dtype={1: np.uint8, 2: '<i2', 4: '<i4'}[sw]).astype(np.float64)
    if sw == 1:
        d -= 128
    d = d.reshape(-1, ch).mean(axis=1)
    f = Fraction(rate / r).limit_denominator(1000)
    d = resample_poly(d, f.numerator, f.denominator)
    return d / max(np.max(np.abs(d)), 1e-9)


def synth_speech(n, rate, rng):
    """speech-like audio: syllables of a glottal pulse train through moving formants"""
    out = np.zeros(n)
    t = 0
    while t < n:
        if rng.random() < 0.25:                      # a pause between phrases
            t += int(rng.uniform(0.15, 0.6) * rate)
            continue
        L = int(rng.uniform(0.12, 0.32) * rate)       # one syllable
        L = min(L, n - t)
        if L <= 0:
            break
        f0 = rng.uniform(90, 220) * np.exp(np.linspace(0, rng.uniform(-0.25, 0.25), L))
        f0 *= 1 + 0.01 * rng.standard_normal(L).cumsum() / np.sqrt(np.arange(1, L + 1))
        ph = np.cumsum(f0 / rate)
        pulses = np.diff(np.floor(ph), prepend=0.0)   # one impulse per period
        src = lfilter([1], [1, -0.95], lfilter([1], [1, -0.9], pulses))   # glottal roll-off
        seg = np.zeros(L)
        for fc, bw, g in ((rng.uniform(300, 800), 80, 1.0), (rng.uniform(900, 2300), 120, 0.6),
                          (rng.uniform(2400, 3000), 200, 0.3)):
            r = np.exp(-np.pi * bw / rate)
            th = 2 * np.pi * fc / rate
            seg += g * lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], src)
        env = np.sin(np.pi * np.arange(L) / L) ** 0.7
        seg *= env * rng.uniform(0.5, 1.0)
        seg += 0.02 * rng.standard_normal(L) * env   # a bit of breath
        out[t:t + L] += seg
        t += L + int(rng.uniform(0.02, 0.12) * rate)
    return out / max(np.max(np.abs(out)), 1e-9)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'seconds': 20.0, 'rate': 48000.0, 'tx': None, 'ctcss': None, 'dcs': None, 'tone-dev': None,
         'voice-dev': None, 'speech': None, 'tail': None, 'tone-err': 0.0, 'baud-err': 0.0, 'offset-hz': 0.0,
         'cnr': None, 'seed': 1}
    flags = set()
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        if k in ('narrow', 'no-hpf', 'deemph'):
            flags.add(k)
            i += 1
            continue
        if k not in o:
            sys.exit('unknown option ' + a[i])
        o[k] = a[i + 1]
        i += 2
    rate = float(o['rate'])
    N = int(float(o['seconds']) * rate)
    rng = np.random.default_rng(int(o['seed']))
    narrow = 'narrow' in flags
    tone_dev = float(o['tone-dev'] or (350 if narrow else 600))
    voice_dev = float(o['voice-dev'] if o['voice-dev'] is not None else (1500 if narrow else 3000))
    txs = [(float(x.split(':')[0]), float(x.split(':')[1])) for x in (o['tx'] or '1:%g' % (float(o['seconds']) - 1)).split(',')]
    kind, tone_id, code, inv = None, '', None, False
    if o['ctcss']:
        kind = 'ctcss'
        f_tone = float(o['ctcss']) * (1 + float(o['tone-err']) / 100)
        tone_id = 'CTCSS %.1f' % float(o['ctcss'])
    elif o['dcs']:
        kind = 'dcs'
        c = o['dcs'].upper().lstrip('D')
        inv = c.endswith('I')
        code = int(c.rstrip('NI'), 8)
        tone_id = 'DCS %03o%s' % (code, 'I' if inv else 'N')
    tail = o['tail'] or ('reverse' if kind == 'ctcss' else 'turnoff' if kind == 'dcs' else 'none')

    # voice: speech through the transmitter's audio chain (pre-emphasis,
    # 300 Hz high-pass (6th order: ~16 dB down at 220 Hz), 3 kHz low-pass,
    # limiter)
    if voice_dev > 0:
        if o['speech']:
            clips = [load_wav(p, rate) for p in o['speech'].split(',')]
            sp = np.concatenate(clips)
            sp = np.tile(sp, int(np.ceil(N / len(sp))))[:N]
        else:
            sp = synth_speech(N, rate, rng)
        b, a_ = bilinear([750e-6, 1], [750e-6 / 20, 1], rate)    # +6 dB/oct from 212 Hz (to ~4 kHz)
        sp = lfilter(b, a_, sp)
        if 'no-hpf' not in flags:
            b, a_ = butter(6, 300, 'highpass', fs=rate)
            sp = lfilter(b, a_, sp)
        b, a_ = butter(6, 3000, 'lowpass', fs=rate)
        sp = lfilter(b, a_, sp)
        sp = np.tanh(2.5 * sp / max(np.percentile(np.abs(sp), 99.5), 1e-9)) / np.tanh(2.5)
        voice = sp * voice_dev
    else:
        voice = np.zeros(N)

    t = np.arange(N) / rate
    on = np.zeros(N, bool)
    tone = np.zeros(N)
    truth = []
    for (t0, t1) in txs:
        n0, n1 = int(t0 * rate), min(N, int(t1 * rate))
        on[n0:n1] = True
        tt = t[n0:n1] - t0
        if kind == 'ctcss':
            phase = 2 * np.pi * f_tone * tt + rng.uniform(0, 2 * np.pi)
            seg = np.cos(phase)
            tone_end = t1
            if tail in ('reverse', 'reverse120'):
                k = tt >= (t1 - t0) - 0.18
                seg[k] = np.cos(phase[k] + (np.pi if tail == 'reverse' else 2 * np.pi / 3))
                tone_end = t1 - 0.18
            elif tail == 'off':
                seg[tt >= (t1 - t0) - 0.15] = 0
                tone_end = t1 - 0.15
            tone[n0:n1] = tone_dev * seg
            truth.append({'name': tone_id, 'start': t0, 'end': tone_end, 'carrier_end': t1, 'tail': tail})
        elif kind == 'dcs':
            w = dcs_word(code)
            bits = np.array([(w >> b) & 1 for b in range(23)], float)
            if inv:
                bits = 1 - bits
            baud = 134.4 * (1 + float(o['baud-err']) / 100)
            code_len = (t1 - t0) - (0.18 if tail == 'turnoff' else 0)
            idx = (tt * baud).astype(int)
            nrz = (2 * bits[idx % 23] - 1)
            nrz[tt >= code_len] = 0
            # the encoder's filter: < 1 % of the power above 300 Hz; a little droop
            b, a_ = butter(4, 250, 'lowpass', fs=rate)
            nrz = lfilter(b, a_, nrz)
            b, a_ = butter(1, 2.0, 'highpass', fs=rate)
            nrz = lfilter(b, a_, nrz)
            seg = tone_dev * nrz
            if tail == 'turnoff':
                k = tt >= code_len
                seg[k] = tone_dev * np.sin(2 * np.pi * 134.4 * (tt[k] - code_len))
            tone[n0:n1] = seg
            truth.append({'name': tone_id, 'start': t0, 'end': t0 + code_len, 'carrier_end': t1, 'tail': tail})
        else:
            truth.append({'name': 'carrier', 'start': t0, 'end': t1, 'carrier_end': t1, 'tail': 'none'})

    audio = voice + tone
    if 'deemph' in flags:
        b, a_ = bilinear([0, 1], [750e-6, 1], rate)
        audio = lfilter(b, a_, audio) * 1.0
    inst = audio + float(o['offset-hz'])
    x = np.exp(2j * np.pi * np.cumsum(inst) / rate) * on
    if o['cnr'] is not None:
        # carrier power 1; noise density so that a 16 kHz channel holds 1 / CNR
        n0 = 10 ** (-float(o['cnr']) / 10) / 16000.0
        sigma = np.sqrt(n0 * rate / 2)
        x = x + sigma * (rng.standard_normal(N) + 1j * rng.standard_normal(N))
        x /= max(1.0, np.max(np.abs(x)))
    x.astype(np.complex64).tofile(out)
    side = {'rate': rate, 'truth': truth, 'id': tone_id, 'tone_dev': tone_dev, 'voice_dev': voice_dev}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %.1f s at %.0f Hz, %s, %d transmission(s), tail %s' % (out, N / rate, rate, tone_id or 'no tone',
                                                                     len(txs), tail))


if __name__ == '__main__':
    main()
