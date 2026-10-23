#!/usr/bin/env python3
"""Analog FM test transmissions for the analog_id plugin: one transmitter's
complex baseband (cf32), keyed on and off, with speech-like (or real speech)
voice through a radio's transmit audio chain, a CTCSS tone (+ reverse burst)
or a DCS code (+ turn-off code) as ETSI TS 103 236 describes them, and the
radio's in-band ID signalling: MDC1200 packets, FleetSync blocks (1200 /
2400 bit/s, FleetSync II), 5-tone selcall, DTMF, a tone burst. Several
transmitters at known bearings: one file each, put on the array by
plugins/sdk/tools/uca_scene.py ("at": 0 - the file keeps its own timing).

  analog_gen.py OUT.cf32 [--seconds 20] [--rate 48000] [--tx 1:6,8.5:12]
               [--ctcss 88.5 | --dcs 023N | (neither: carrier squelch)]
               [--tone-dev 600] [--voice-dev 3000] [--narrow]
               [--speech a.wav[,b.wav]] [--no-hpf] [--deemph]
               [--tail reverse|reverse120|off|turnoff|none]
               [--tone-err PCT] [--baud-err PCT] [--offset-hz HZ] [--cnr DB] [--seed N]
               [--mdc OP:ARG:UNIT[:E0E1E2E3]] [--fleetsync FLEET-UNIT[:TOFLEET-TOUNIT][:TYPE[:FLAGS]]]
               [--fs-baud 1200|2400] [--fs2] [--selcall STD:DIGITS] [--dtmf DIGITS[:ON_MS:OFF_MS]]
               [--burst HZ[:MS]] [--sig-at pre|post|both] [--sig-dev HZ] [--sig-delay S]
               [--sig-preemph] [--sig-baud-err PCT] [--audio-u8 OUT.u8]

--tx      carrier on from A to B seconds (several: comma-separated)
--narrow  12.5 kHz channel: voice 1500 Hz, tone 350 Hz, signalling 1250 Hz peak deviation
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
--mdc     an MDC1200 packet (hex): op:arg:unit, a double packet with the
          second block's 4 bytes (e.g. 35:89:5678:830D1234 - 1234 calls 5678)
--fleetsync  a FleetSync message from FLEET-UNIT (e.g. 100-1001) to another
          (default itself), TYPE (hex, default 80 = ANI), FLAGS (hex byte 0,
          default FE; the end-of-transmission ID gets FC)
--selcall  5-tone selcall, e.g. CCIR1:12345, ZVEI1:11223 (repeats become E)
--dtmf    DTMF digits (default 70 ms on / 70 ms off)
--burst   a tone burst at the key-up (default 1750 Hz, 600 ms)
--sig-at  where the ID signalling goes in every transmission: pre (after the
          key-up, voice after it), post (just before the carrier / tone ends)
          or both (default pre); the voice is muted while it is sent
--sig-dev  its peak deviation (Hz; default 2500, narrow 1250)
--sig-preemph  the signalling through the transmitter's pre-emphasis too
          (radios that inject it into the microphone path)
--sig-baud-err  the data encoders' bit-rate error (%)
--audio-u8  also write the modulating audio (voice + tones + signalling) as
          8 kHz unsigned 8-bit (the reference decoders' input)
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


# --- MDC1200 (the format as the public decoders document it) ---------------
MDC_SYNC = 0x07092A446F


def mdc_crc(data):
    c = 0
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def mdc_block(d4):
    """4 data bytes -> the 112 transmitted bits of a block (time order)"""
    c = mdc_crc(d4)
    d7 = list(d4) + [c & 0xFF, c >> 8, 0]
    dbits = [(d7[i] >> j) & 1 for i in range(7) for j in range(8)]

    def d(t):
        return dbits[t] if t >= 0 else 0
    pbits = [d(t) ^ d(t - 2) ^ d(t - 5) ^ d(t - 6) for t in range(56)]
    coded = dbits + pbits
    out = [0] * 112
    for t in range(112):
        out[(t % 7) * 16 + t // 7] = coded[t]
    return out


def mdc_bits(op, arg, unit, extra=None, preamble=7):
    bits = []
    for _ in range(preamble):
        bits += [(0x55 >> (7 - i)) & 1 for i in range(8)]
    bits += [(MDC_SYNC >> (39 - i)) & 1 for i in range(40)]
    bits += mdc_block([op, arg, (unit >> 8) & 0xFF, unit & 0xFF])
    if extra is not None:
        bits += mdc_block(list(extra))
    return bits


def mdc_audio(bits, rate, baud, ph0=0.0):
    """XOR-precoded MSK: a bit that differs from the one before = 1800 Hz,
    an equal one = 1200 Hz, phase continuous"""
    n = int(round(len(bits) * rate / baud))
    f = np.zeros(n)
    prev = 0
    for i, b in enumerate(bits):
        a, e = int(round(i * rate / baud)), int(round((i + 1) * rate / baud))
        f[a:e] = 1800.0 if b != prev else 1200.0
        prev = b
    return np.sin(ph0 + 2 * np.pi * np.cumsum(f) / rate)


# --- FleetSync --------------------------------------------------------------
def fs_codeword(d6):
    """6 data bytes -> the 64-bit block, an MPT 1327 codeword (as
    plugins/mpt1327/tools/mpt1327_gen.py builds them): the 48 data bits, the
    BCH(63,48) check bits (g = 0xE815) with the last one inverted, an even
    parity bit"""
    info = int.from_bytes(bytes(d6), 'big')
    r = info << 15
    for i in range(62, 14, -1):
        if (r >> i) & 1:
            r ^= 0xE815 << (i - 15)
    cw = (info << 16) | (((r ^ 1) & 0x7FFF) << 1)
    return cw | (bin(cw).count('1') & 1)


def bch15_syndrome(w):
    r = 0
    for p in range(15):
        if (w >> p) & 1:
            t = 1
            for _ in range(14 - p):
                t <<= 1
                if t & 0x100:
                    t ^= 0x117
            r ^= t
    return r


_BCH_NIB = {}


def bch15_nibble(nib):
    if nib not in _BCH_NIB:
        for w in range(0x8000):
            if (w >> 8) & 0xF == nib and bch15_syndrome(w) == 0:
                _BCH_NIB[nib] = w
                break
    return _BCH_NIB[nib]


def fs_bits(data, fs2=False, preamble=48):
    """data: bytes, a multiple of 6 -> preamble 1010..., sync 0x23EB, blocks"""
    bits = [1 - (i & 1) for i in range(preamble)]
    bits += [(0x23EB >> (15 - i)) & 1 for i in range(16)]

    for k in range(0, len(data), 6):
        cw = fs_codeword(data[k:k + 6])
        if not fs2:
            bits += [(cw >> (63 - i)) & 1 for i in range(64)]
            continue
        # FleetSync II: each 16-bit quarter of the block (bits 63..48 first) ->
        # its 4 nibbles, each a BCH(15,7) codeword in a 16-bit field; the
        # fields go on air in pairs, the second of each pair first
        for q in range(4):
            quarter = (cw >> (48 - 16 * q)) & 0xFFFF
            code = [bch15_nibble((quarter >> (12 - 4 * n)) & 0xF) for n in range(4)]
            for n in (1, 0, 3, 2):
                bits += [(code[n] >> (15 - b)) & 1 for b in range(16)]
    return bits


def fs_audio(bits, rate, baud, ph0=0.0):
    """a 1 = 1200 Hz, a 0 = 1800 Hz (1200 bit/s) / 2400 Hz (2400 bit/s)"""
    n = int(round(len(bits) * rate / baud))
    f = np.zeros(n)
    f0 = 1800.0 if baud < 2000 else 2400.0
    for i, b in enumerate(bits):
        a, e = int(round(i * rate / baud)), int(round((i + 1) * rate / baud))
        f[a:e] = 1200.0 if b else f0
    return np.sin(ph0 + 2 * np.pi * np.cumsum(f) / rate)


# --- 5-tone selcall / DTMF / bursts (Midian "Tone signaling charts") --------
SELCALL = {
    'ZVEI1': ([2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200], 2600, 0.070),
    'ZVEI2': ([2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200], 970, 0.070),
    'ZVEI3': ([2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000], 2400, 0.070),
    'DZVEI': ([2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000], 2400, 0.070),
    'CCIR1': ([1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860], 2110, 0.100),
    'CCIR2': ([1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860], 2110, 0.070),
    'EEA': ([1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860], 2110, 0.040),
    'EIA': ([600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869], 459, 0.033),
    'NATEL': ([1633, 631, 697, 770, 852, 941, 1040, 1209, 1336, 1477], 1805, 0.070),
}
DTMF_ROWS = [697, 770, 852, 941]
DTMF_COLS = [1209, 1336, 1477, 1633]
DTMF_KEYS = '123A456B789C*0#D'


def tone_seq(freqs_durs, rate):
    """a run of tones (phase continuous), [(hz or (hz1, hz2) or 0 = pause, seconds)]"""
    out = []
    ph1 = ph2 = 0.0
    for f, d in freqs_durs:
        n = int(round(d * rate))
        t = np.arange(n) / rate
        if isinstance(f, tuple):
            s = 0.5 * (np.sin(ph1 + 2 * np.pi * f[0] * t) + np.sin(ph2 + 2 * np.pi * f[1] * t))
            ph1 += 2 * np.pi * f[0] * n / rate
            ph2 += 2 * np.pi * f[1] * n / rate
        elif f > 0:
            s = np.sin(ph1 + 2 * np.pi * f * t)
            ph1 += 2 * np.pi * f * n / rate
        else:
            s = np.zeros(n)
        out.append(s)
    return np.concatenate(out) if out else np.zeros(0)


def selcall_audio(spec, rate):
    std, digits = spec.split(':')
    table, rep, dur = SELCALL[std.upper()]
    seq, prev = [], None
    for ch in digits:
        f = rep if ch == prev else table[int(ch)]
        seq.append((f, dur))
        prev = ch if ch != prev else None   # "111" = 1 E 1
    return tone_seq(seq, rate), '%s %s' % ({'ZVEI1': 'ZVEI', 'ZVEI2': 'ZVEI', 'ZVEI3': 'DZVEI', 'DZVEI': 'DZVEI',
                                             'CCIR1': 'CCIR', 'CCIR2': 'CCIR'}.get(std.upper(), std.upper()), digits)


def dtmf_audio(spec, rate):
    p = spec.split(':')
    on, off = (float(p[1]) / 1000, float(p[2]) / 1000) if len(p) >= 3 else (0.07, 0.07)
    seq = []
    for ch in p[0].upper():
        k = DTMF_KEYS.index(ch)
        seq += [((DTMF_ROWS[k // 4], DTMF_COLS[k % 4]), on), (0, off)]
    return tone_seq(seq[:-1], rate), 'DTMF ' + p[0].upper()


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
         'cnr': None, 'seed': 1, 'mdc': None, 'fleetsync': None, 'fs-baud': 1200, 'selcall': None, 'dtmf': None,
         'burst': None, 'sig-at': 'pre', 'sig-dev': None, 'sig-delay': 0.15, 'sig-baud-err': 0.0, 'audio-u8': None}
    flags = set()
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        if k in ('narrow', 'no-hpf', 'deemph', 'fs2', 'sig-preemph'):
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
    sig_dev = float(o['sig-dev'] or (1250 if narrow else 2500))
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
    tail_len = 0.18 if tail in ('reverse', 'reverse120', 'turnoff') else 0.15 if tail == 'off' else 0.0

    # the ID signalling of every transmission (one burst after the other)
    baud_k = 1 + float(o['sig-baud-err']) / 100
    sig_parts, sig_ids = [], []

    def add_sig(at, x, ident):
        sig_parts.append((at, x))
        sig_ids.append(ident)
    for at in (('pre', 'post') if o['sig-at'] == 'both' else (o['sig-at'],)):
        if o['burst'] and at == 'pre':
            p = o['burst'].split(':')
            hz, ms = float(p[0]), float(p[1]) if len(p) > 1 else 600.0
            add_sig(at, tone_seq([(hz, ms / 1000)], rate), 'Burst %.0f' % hz)
        if o['mdc']:
            p = o['mdc'].split(':')
            op, arg, unit = int(p[0], 16), int(p[1], 16), int(p[2], 16)
            ex = bytes.fromhex(p[3]) if len(p) > 3 else None
            caller = unit if ex is None else (ex[2] << 8) | ex[3]
            add_sig(at, mdc_audio(mdc_bits(op, arg, unit, ex), rate, 1200 * baud_k), 'MDC %04X' % caller)
        if o['fleetsync']:
            p = o['fleetsync'].split(':')
            fl, un = (int(x) for x in p[0].split('-'))
            tf, tu = (int(x) for x in p[1].split('-')) if len(p) > 1 and p[1] else (fl, un)
            typ = int(p[2], 16) if len(p) > 2 else 0x80
            fla = int(p[3], 16) if len(p) > 3 else (0xFC if at == 'post' else 0xFE)
            fu, tun = un - 999, tu - 999
            data = [fla, typ | (1 if tf != fl else 0), fl - 99, (fu >> 4) & 0xFF, ((fu & 0xF) << 4) | ((tun >> 8) & 0xF),
                    tun & 0xFF]
            if tf != fl:
                data += [tf - 99, 0, 0, 0, 0, 0]
            baud = float(o['fs-baud'])
            add_sig(at, fs_audio(fs_bits(data, 'fs2' in flags), rate, baud * baud_k), 'FleetSync %d-%d' % (fl, un))
        if o['selcall']:
            x, ident = selcall_audio(o['selcall'], rate)
            add_sig(at, x, ident)
        if o['dtmf']:
            x, ident = dtmf_audio(o['dtmf'], rate)
            add_sig(at, x, ident)

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
    sig = np.zeros(N)
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
            tr = {'name': tone_id, 'start': t0, 'end': tone_end, 'carrier_end': t1, 'tail': tail}
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
            tr = {'name': tone_id, 'start': t0, 'end': t0 + code_len, 'carrier_end': t1, 'tail': tail}
        else:
            tr = {'name': 'carrier', 'start': t0, 'end': t1, 'carrier_end': t1, 'tail': 'none'}
        # the signalling: pre right after the key-up, post right before the end
        bursts = []
        pre = [x for at, x in sig_parts if at == 'pre']
        post = [x for at, x in sig_parts if at == 'post']
        p = n0 + int(float(o['sig-delay']) * rate)
        for x in pre:
            e = min(n1, p + len(x))
            sig[p:e] = x[:e - p]
            voice[max(n0, p - int(0.05 * rate)):e + int(0.05 * rate)] = 0
            bursts.append([p / rate, e / rate])
            p = e + int(0.06 * rate)
        if pre:
            voice[n0:p] = 0
        end_n = n1 - int((tail_len + 0.03) * rate)
        p = end_n - sum(len(x) + int(0.06 * rate) for x in post)
        if post:
            voice[max(n0, p - int(0.05 * rate)):n1] = 0
        for x in post:
            e = min(n1, p + len(x))
            sig[p:e] = x[:e - p]
            bursts.append([p / rate, e / rate])
            p = e + int(0.06 * rate)
        if sig_ids:
            tr['ids'] = sig_ids
            tr['bursts'] = bursts
            tr['talker'] = next((s for s in sig_ids if s.startswith(('MDC', 'FleetSync'))),
                                next((s for s in sig_ids if not s.startswith('Burst')), tone_id or sig_ids[0]))
        truth.append(tr)

    sig_audio = sig * sig_dev
    if 'sig-preemph' in flags:
        b, a_ = bilinear([750e-6, 1], [750e-6 / 20, 1], rate)
        sig_audio = lfilter(b, a_, sig_audio)
        sig_audio *= sig_dev / max(np.max(np.abs(sig_audio)), 1e-9)
    audio = voice + tone + sig_audio
    if 'deemph' in flags:
        b, a_ = bilinear([0, 1], [750e-6, 1], rate)
        audio = lfilter(b, a_, audio) * 1.0
    if o['audio-u8']:
        f = Fraction(8000, int(rate)).limit_denominator(1000)
        y = resample_poly(audio, f.numerator, f.denominator)
        y = y / max(np.max(np.abs(y)), 1e-9) * 0.8
        np.clip(np.round(y * 127 + 128), 0, 255).astype(np.uint8).tofile(o['audio-u8'])
    inst = audio + float(o['offset-hz'])
    x = np.exp(2j * np.pi * np.cumsum(inst) / rate) * on
    if o['cnr'] is not None:
        # carrier power 1; noise density so that a 16 kHz channel holds 1 / CNR
        n0 = 10 ** (-float(o['cnr']) / 10) / 16000.0
        sigma = np.sqrt(n0 * rate / 2)
        x = x + sigma * (rng.standard_normal(N) + 1j * rng.standard_normal(N))
        x /= max(1.0, np.max(np.abs(x)))
    x.astype(np.complex64).tofile(out)
    side = {'rate': rate, 'truth': truth, 'id': tone_id, 'tone_dev': tone_dev, 'voice_dev': voice_dev,
            'sig': sig_ids, 'sig_dev': sig_dev}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %.1f s at %.0f Hz, %s, %d transmission(s), tail %s%s' % (
        out, N / rate, rate, tone_id or 'no tone', len(txs), tail,
        (', IDs ' + ' + '.join(sig_ids) + ' (' + o['sig-at'] + ')') if sig_ids else ''))


if __name__ == '__main__':
    main()
