#!/usr/bin/env python3
"""FM broadcast test transmissions for the rds plugin: stations with a real
RDS / RBDS stream (EN 50067 groups, check words + offset words, differential
+ biphase coding, cos(pi f td / 4) shaping on 57 kHz, 3 x the pilot), stereo
pilot + programme, SCA subcarriers (FM audio with 150 us pre-emphasis, FSK
data) and RDS2 data streams, each station at its own bearing on kraken_doa's
UCA (ANT0 on +x, the others CLOCKWISE), written in 0.1 s pieces.

  rds_gen.py SCENE.json OUT.cf32|OUT.cu8

OUT.json gets rate, channels, rf_hz, radius_m and the truth. Test with

  plugins/rds/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 [--offset HZ]

SCENE (JSON):
{
  "rate": 480000,            output rate: 480 kHz x an integer (480 kHz, 2.4 MHz)
  "offset_hz": 0,            where the stations sit in the output band
  "rf_hz": 98.5e6, "seconds": 12, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 1.0,          cu8: noise per I / Q in LSB (cf32: the same scale / 127.5)
  "stations": [{
    "name": "A", "bearing": 60, "at": 0.3, "until": 6.0,  seconds of the scene
    "cnr_db": 30,            carrier over the noise in 200 kHz
    "pi": "9201" | "callsign": "KRKN" (RBDS: the PI from the call sign),
    "ps": "KRAKEN", "pty": 10, "tp": 1, "ta": [[2.0, 3.5]], "ms": 1, "stereo": true,
    "rt": ["text", ...], "rt_period_s": 4, "rtplus": [["item.title", "Song"], ["item.artist", "Band"]],
    "af": [98.5, 101.2], "ecc": "F1", "lang": 9, "pin": [22, 14, 0],
    "ct": "2026-10-22T03:04", "lto": 26 (local time offset, half hours), "ct_period_s": 60,
    "basic_per_extra": 4 (0A / 2A groups between the others: 1A, 3A, RT+, PTYN, EON, Long PS, eRT),
    "ptyn": "DEBUG", "lps": "Long PS (UTF-8)", "ert": "Enhanced RadioText (UTF-8)",
    "eon": [{"pi": "9202", "ps": "OTHER", "af": [99.1], "pty": 1}],
    "rds_dev_hz": 2000, "pilot_dev_hz": 6750, "audio_dev_hz": 40000,
    "sca": [{"hz": 67000, "type": "audio", "dev_hz": 6000, "inj_hz": 7000, "tones": [440, 1000]},
            {"hz": 92000, "type": "fsk", "baud": 1200, "dev_hz": 4000, "inj_hz": 7000}],
    "rds2": [{"hz": 66500, "dev_hz": 2000}]
  }]
}
"""
import json
import os
import sys
from datetime import datetime

import numpy as np
from scipy.signal import fftconvolve, firwin, lfilter, resample_poly, upfirdn

C = 299792458.0
FI = 480000           # internal rate
FR = 228000           # RDS baseband: 192 samples per bit
POLY = 0x5B9
OFF = {'A': 0x0FC, 'B': 0x198, 'C': 0x168, 'Cp': 0x350, 'D': 0x1B4}


def syndrome(w):
    r = w & 0x3FFFFFF
    for b in range(25, 9, -1):
        if r >> b & 1:
            r ^= POLY << (b - 10)
    return r & 0x3FF


def block(info, off):
    m = (info & 0xFFFF) << 10
    return m | (syndrome(m) ^ OFF[off])


def rds_char(s):
    """text -> RDS character codes (ASCII as is; a few Latin letters)"""
    tab = {'á': 0x80, 'à': 0x81, 'é': 0x82, 'è': 0x83, 'ö': 0x97, 'ü': 0x99, 'ä': 0x91, 'ñ': 0x9A, '€': 0xA9}
    return [tab.get(ch, ord(ch) if 32 <= ord(ch) < 127 else 0x20) for ch in s]


def callsign_pi(cs):
    cs = cs.upper()
    v = (ord(cs[1]) - 65) * 676 + (ord(cs[2]) - 65) * 26 + (ord(cs[3]) - 65)
    return (0x1000 if cs[0] == 'K' else 0x54A8) + v


def mjd_of(dt):
    return (dt - datetime(1858, 11, 17)).days


class Groups:
    """the group sequence of one RDS stream (a station)"""

    def __init__(self, st, rng):
        self.st = st
        self.pi = int(st['pi'], 16) if 'pi' in st else callsign_pi(st['callsign'])
        st['pi_int'] = self.pi
        self.ps = (rds_char(st.get('ps', 'KRAKEN')) + [0x20] * 8)[:8]
        self.rts = st.get('rt', ['Test transmission'])
        self.rt_period = float(st.get('rt_period_s', 6))
        self.af = [int(round((f - 87.5) * 10)) for f in st.get('af', [])]
        self.k = 0
        self.ps_seg = self.rt_seg = self.af_i = self.eon_i = self.lps_seg = self.ert_seg = 0
        self.ptyn_seg = 0
        self.rt_ab = 0
        self.rt_cur = -1
        self.t = 0
        lps = st.get('lps', '').encode('utf-8')
        self.lps = list(lps[:32]) + ([0x0D] if len(lps) < 32 else [])
        ert = st.get('ert', '').encode('utf-8')
        self.ert = list(ert[:128]) + ([0x0D] if len(ert) < 128 else [])
        self.ct_dt = datetime.fromisoformat(st['ct']) if 'ct' in st else None
        self.ct_sent = -1e9
        self.ct_period = float(st.get('ct_period_s', 60))   # stations: once a minute
        nb = int(st.get('basic_per_extra', 4))                # 0A / 2A groups before each other one
        seq = ['0A', '2A'] * 4
        extra = ['1A']
        if 'rtplus' in st:
            extra += ['3A:4BD7', '11A']
        if 'ptyn' in st:
            extra += ['10A']
        if st.get('eon'):
            extra += ['14A']
        if 'lps' in st:
            extra += ['15A']
        if 'ert' in st:
            extra += ['3A:6552', '12A']
        self.seq = []
        for e in extra:   # an extra group after every nb basic ones
            self.seq += seq[:nb] + [e]
        if not self.seq:
            self.seq = seq

    def rt_text(self):
        i = int(self.t / self.rt_period) % len(self.rts)
        if i != self.rt_cur:
            self.rt_cur = i
            self.rt_ab ^= 1
            self.rt_seg = 0
        txt = self.rts[i]
        c = rds_char(txt)[:64]
        if len(c) < 64:
            c += [0x0D]
        while len(c) % 4:
            c += [0x20]
        return txt, c

    def b2(self, gtype, ver, low):
        st = self.st
        return (gtype << 12) | (ver << 11) | (int(st.get('tp', 0)) << 10) | (int(st.get('pty', 0)) << 5) | (low & 0x1F)

    def ta(self):
        for a, b in self.st.get('ta', []):
            if a <= self.t < b:
                return 1
        return 0

    def next(self, t):
        """the next group's 4 blocks (26-bit words)"""
        self.t = t
        st = self.st
        pi = self.pi
        if self.ct_dt is not None and t - self.ct_sent >= self.ct_period:
            self.ct_sent = t
            return self.g4a(t)
        g = self.seq[self.k % len(self.seq)]
        self.k += 1
        if g == '0A':
            c = self.ps_seg
            self.ps_seg = (c + 1) % 4
            di = {3: int(st.get('stereo', True)), 2: 0, 1: 0, 0: int(st.get('dynpty', 0))}
            low = (self.ta() << 4) | (int(st.get('ms', 1)) << 3) | (di[c] << 2) | c
            if self.af:
                codes = [224 + len(self.af)] + self.af
                if len(codes) % 2:
                    codes += [205]
                a = self.af_i % (len(codes) // 2)
                self.af_i += 1
                bc = codes[2 * a] << 8 | codes[2 * a + 1]
            else:
                bc = 0xE0CD   # 0 AFs + filler
            return [block(pi, 'A'), block(self.b2(0, 0, low), 'B'), block(bc, 'C'),
                    block(self.ps[2 * c] << 8 | self.ps[2 * c + 1], 'D')]
        if g == '2A':
            _, c = self.rt_text()
            nseg = len(c) // 4
            s = self.rt_seg % nseg
            self.rt_seg += 1
            q = c[4 * s:4 * s + 4]
            return [block(pi, 'A'), block(self.b2(2, 0, self.rt_ab << 4 | s), 'B'), block(q[0] << 8 | q[1], 'C'),
                    block(q[2] << 8 | q[3], 'D')]
        if g == '1A':
            v = (self.k // len(self.seq)) % 2
            if v == 0:
                bc = (0 << 12) | int(st.get('ecc', '00'), 16)
            else:
                bc = (3 << 12) | int(st.get('lang', 0))
            day, hour, minute = st.get('pin', [0, 0, 0])
            pin = (day << 11) | (hour << 6) | minute
            return [block(pi, 'A'), block(self.b2(1, 0, 0), 'B'), block(bc, 'C'), block(pin, 'D')]
        if g.startswith('3A:'):
            aid = int(g[3:], 16)
            agt = {0x4BD7: 11 * 2, 0x6552: 12 * 2}[aid]
            msg = 1 if aid == 0x6552 else 0   # eRT: UTF-8
            return [block(pi, 'A'), block(self.b2(3, 0, agt), 'B'), block(msg, 'C'), block(aid, 'D')]
        if g == '11A':   # RT+ tags into the current RadioText
            txt, _ = self.rt_text()
            names = ['dummy', 'item.title', 'item.album', 'item.tracknumber', 'item.artist']
            tags = []
            for name, sub in st.get('rtplus', []):
                p = txt.find(sub)
                if p >= 0 and name in names:
                    tags.append((names.index(name), p, len(sub) - 1))
            tags = (tags + [(0, 0, 0), (0, 0, 0)])[:2]
            (t1, s1, l1), (t2, s2, l2) = tags
            low = (self.rt_cur & 1) << 4 | 1 << 3 | (t1 >> 3)
            bc = (t1 & 7) << 13 | s1 << 7 | l1 << 1 | (t2 >> 5)
            bd = (t2 & 0x1F) << 11 | s2 << 5 | (l2 & 0x1F)
            return [block(pi, 'A'), block(self.b2(11, 0, low), 'B'), block(bc, 'C'), block(bd, 'D')]
        if g == '12A':   # eRT
            n = (len(self.ert) + 3) // 4
            s = self.ert_seg % n
            self.ert_seg += 1
            q = (self.ert[4 * s:4 * s + 4] + [0x20] * 4)[:4]
            return [block(pi, 'A'), block(self.b2(12, 0, s), 'B'), block(q[0] << 8 | q[1], 'C'),
                    block(q[2] << 8 | q[3], 'D')]
        if g == '10A':
            p = (rds_char(st['ptyn']) + [0x20] * 8)[:8]
            s = self.ptyn_seg % 2
            self.ptyn_seg += 1
            q = p[4 * s:4 * s + 4]
            return [block(pi, 'A'), block(self.b2(10, 0, s), 'B'), block(q[0] << 8 | q[1], 'C'),
                    block(q[2] << 8 | q[3], 'D')]
        if g == '15A':
            n = (len(self.lps) + 3) // 4
            s = self.lps_seg % n
            self.lps_seg += 1
            q = (self.lps[4 * s:4 * s + 4] + [0x20] * 4)[:4]
            return [block(pi, 'A'), block(self.b2(15, 0, s), 'B'), block(q[0] << 8 | q[1], 'C'),
                    block(q[2] << 8 | q[3], 'D')]
        if g == '14A':
            eons = st['eon']
            e = eons[(self.eon_i // 6) % len(eons)]
            v = [0, 1, 2, 3, 4, 13][self.eon_i % 6]
            self.eon_i += 1
            onpi = int(e['pi'], 16)
            if v <= 3:
                p = (rds_char(e.get('ps', '')) + [0x20] * 8)[:8]
                bc = p[2 * v] << 8 | p[2 * v + 1]
            elif v == 4:
                af = [int(round((f - 87.5) * 10)) for f in e.get('af', [])] + [205, 205]
                bc = af[0] << 8 | af[1]
            else:
                bc = int(e.get('pty', 0)) << 11
            return [block(pi, 'A'), block(self.b2(14, 0, v), 'B'), block(bc, 'C'), block(onpi, 'D')]
        raise ValueError(g)

    def g4a(self, t):
        from datetime import timedelta
        dt = self.ct_dt + timedelta(minutes=int(t // 60))
        mjd = mjd_of(dt)
        lto = int(self.st.get('lto', 0))
        b = self.b2(4, 0, mjd >> 15)
        c = (mjd & 0x7FFF) << 1 | (dt.hour >> 4)
        d = (dt.hour & 0xF) << 12 | dt.minute << 6 | (32 if lto < 0 else 0) | abs(lto)
        return [block(self.pi, 'A'), block(b, 'B'), block(c, 'C'), block(d, 'D')]


def rds_baseband(gen, seconds, rng, bit0_t):
    """the shaped biphase baseband at FR for `seconds` (peak ~1); the bit times"""
    nbits = int(seconds * 1187.5) + 208
    bits = []
    while len(bits) < nbits:
        for w in gen.next(len(bits) / 1187.5 + bit0_t):
            bits += [(w >> (25 - i)) & 1 for i in range(26)]
    bits = np.array(bits[:nbits], dtype=np.int8)
    d = np.bitwise_xor.accumulate(bits)   # differential encoding
    sym = 2.0 * d - 1
    x = np.zeros(nbits * 192 + 2048)
    x[np.arange(nbits) * 192] = sym
    x[np.arange(nbits) * 192 + 96] -= sym
    # H(f) = cos(pi f td / 4), |f| < 2 / td: RRC, roll-off 1, T = td / 2 (96 samples)
    T = 96
    tt = np.arange(-8 * T, 8 * T + 1) / T
    with np.errstate(divide='ignore', invalid='ignore'):
        h = 4 * np.cos(2 * np.pi * tt) / (np.pi * (1 - 16 * tt ** 2))
    h[np.abs(np.abs(tt) - 0.25) < 1e-9] = 1.0   # the limit at |t| = T / 4: 4 cos(pi/2)/... -> 1 (scaled below)
    h /= h.sum()
    y = fftconvolve(x, h)[8 * T:8 * T + nbits * 192]
    return y / max(np.max(np.abs(y)), 1e-9)


def audio48(kind, seconds, rng, stereo=True):
    """programme audio, 48 kHz, band-limited to 15 kHz (L, R in about +-1)"""
    n = int(seconds * 48000) + 4800
    lp = firwin(255, 14500, fs=48000)
    out = []
    for ch in range(2):
        w = rng.standard_normal(n)
        w = lfilter([1, -0.95], [1], w)          # brighter
        w = np.convolve(w, lp, mode='same')
        env = 0.3 + 0.7 * np.abs(np.sin(2 * np.pi * (0.7 + 0.3 * ch) * np.arange(n) / 48000))
        tones = sum(0.25 * np.sin(2 * np.pi * f * np.arange(n) / 48000 + rng.uniform(0, 6.28)) for f in (330, 880 + 110 * ch, 2200))
        s = 0.5 * w / max(np.std(w), 1e-9) * env + tones
        out.append(s / max(np.max(np.abs(s)), 1e-9))
    if not stereo:
        out[1] = out[0]
    return out


def sca_audio16(sc, seconds, rng):
    """an SCA's audio at 16 kHz: tones (or noise bursts), 150 us pre-emphasis, peak 1"""
    n = int(seconds * 16000) + 1600
    t = np.arange(n) / 16000
    tones = sc.get('tones', [1000])
    a = np.zeros(n)
    for i, f in enumerate(tones):   # the tones take turns, 1 s each
        on = ((t // 1.0) % len(tones)) == i
        a += on * np.sin(2 * np.pi * f * t)
    tau = 150e-6
    pre = a + tau * np.gradient(a) * 16000
    return pre / max(np.max(np.abs(pre)), 1e-9)


class Upsampler:
    """x L, piece by piece: every output sample from complete input (a fixed
    latency of D input samples, the same for every station)"""

    def __init__(self, L):
        self.L = L
        # pass to ~85 % of the input band (FM sidebands to +-150 kHz), images out
        self.h = firwin(24 * L + 1, 0.85 / L, window=('kaiser', 9.0)) * L if L > 1 else None
        self.tail = None
        if L > 1:
            self.d = (len(self.h) - 1) // 2
            self.D = -(-(self.d + 1) // L)
            self.nt = self.D + -(-self.d // L) + 1

    def push(self, x):
        if self.L == 1:
            return x
        if self.tail is None:
            self.tail = np.zeros(self.nt, dtype=x.dtype)
        xx = np.concatenate([self.tail, x])
        y = upfirdn(self.h, xx, up=self.L)
        i0 = (self.nt - self.D) * self.L + self.d
        out = y[i0:i0 + len(x) * self.L]
        self.tail = xx[-self.nt:]
        return out.astype(np.complex64)


def station_signal(st, seconds, rng, cnr_amp):
    """one station's complex baseband at FI for the whole scene (0 outside [at, until])"""
    N = int(round(seconds * FI))
    t = np.arange(N) / FI
    gen = Groups(st, rng)
    php = 2 * np.pi * 19000 * t
    stereo = st.get('stereo', True)
    mpx = np.zeros(N)
    L, R = audio48(st.get('audio', 'mix'), seconds, rng, stereo)
    L = resample_poly(L, 10, 1)[:N]
    R = resample_poly(R, 10, 1)[:N]
    adev = float(st.get('audio_dev_hz', 40000))
    mpx += adev * 0.5 * (L + R)
    if stereo:
        mpx += adev * 0.5 * (L - R) * np.cos(2 * php)
        mpx += float(st.get('pilot_dev_hz', 6750)) * np.cos(php)
    del L, R
    # RDS on 57 kHz = 3 x the pilot
    bb = rds_baseband(gen, seconds, rng, 0.0)
    bbi = np.interp(t, np.arange(len(bb)) / FR, bb)
    mpx += float(st.get('rds_dev_hz', 2000)) * bbi * np.cos(3 * php)
    del bb, bbi
    for r2 in st.get('rds2', []):   # RDS2: an RDS-like stream of its own groups
        g2 = Groups({'pi': 'F000', 'ps': 'RDS2', 'rt': ['RDS2 stream'], 'pty': 0}, rng)
        b2 = rds_baseband(g2, seconds, rng, 0.0)
        mpx += float(r2.get('dev_hz', 2000)) * np.interp(t, np.arange(len(b2)) / FR, b2) * np.cos(2 * np.pi * r2['hz'] * t)
    for sc in st.get('sca', []):
        fc = float(sc['hz'])
        dev = float(sc.get('dev_hz', 6000))
        if sc.get('type', 'audio') == 'audio':
            a = resample_poly(sca_audio16(sc, seconds, rng), 30, 1)[:N]
        else:   # FSK: random NRZ bits, Gaussian BT 0.5
            baud = float(sc.get('baud', 1200))
            spb = FI / baud
            nb = int(seconds * baud) + 8
            bits = 2.0 * rng.integers(0, 2, nb) - 1
            a = bits[np.minimum((np.arange(N) / spb).astype(int), nb - 1)]
            bt = 0.5
            sig = spb * np.sqrt(np.log(2)) / (2 * np.pi * bt)
            g = np.exp(-0.5 * (np.arange(-int(3 * sig), int(3 * sig) + 1) / sig) ** 2)
            a = np.convolve(a, g / g.sum(), mode='same')
        ph = 2 * np.pi * fc * t + 2 * np.pi * dev * np.cumsum(a) / FI
        mpx += float(sc.get('inj_hz', 7000)) * np.cos(ph)
        del a, ph
    phase = 2 * np.pi * np.cumsum(mpx) / FI + rng.uniform(0, 2 * np.pi)
    del mpx
    x = np.exp(1j * phase).astype(np.complex64)
    a0, a1 = float(st.get('at', 0)), float(st.get('until', seconds))
    env = np.clip(np.minimum(t - a0, a1 - t) / 0.005, 0, 1).astype(np.float32)
    return x * (env * cnr_amp)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1], encoding='utf-8'))
    out = sys.argv[2]
    fmt = out.rsplit('.', 1)[-1]
    if fmt not in ('cu8', 'cf32'):
        sys.exit('output: .cu8 or .cf32')
    if os.path.abspath(out.rsplit('.', 1)[0] + '.json') == os.path.abspath(sys.argv[1]):
        sys.exit('OUT.json would overwrite the scene: give the output another name')
    rate = float(scene.get('rate', FI))
    L = int(round(rate / FI))
    if abs(L * FI - rate) > 1:
        sys.exit('rate: 480 kHz x an integer')
    M = int(scene.get('channels', 5))
    Rr = float(scene.get('radius_m', 0.12))
    rf = float(scene['rf_hz'])
    lam = C / rf
    T = float(scene['seconds'])
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 1.0))
    off = float(scene.get('offset_hz', 0))
    phi = -2 * np.pi * np.arange(M) / M
    # noise power per complex sample at the output rate; in 200 kHz: x 200k / rate
    pn200 = 2 * sig_n ** 2 * 200e3 / rate
    sts, steers, ups, truth = [], [], [], []
    for st in scene.get('stations', []):
        amp = np.sqrt(pn200 * 10 ** (float(st.get('cnr_db', 30)) / 10))
        sts.append(station_signal(st, T, rng, amp))
        b = np.deg2rad(float(st['bearing']))
        steers.append(np.exp(2j * np.pi * Rr / lam * np.cos(b - phi)).astype(np.complex64))
        ups.append(Upsampler(L))
        tr = {k: st[k] for k in ('name', 'bearing', 'at', 'until', 'ps', 'callsign', 'pi') if k in st}
        tr['pi'] = '%04X' % st['pi_int']
        tr['sca'] = st.get('sca', [])
        truth.append(tr)
        print('%-4s PI %04X bearing %5.1f  %.1f-%.1f s  CNR %.0f dB' % (st.get('name', '?'), st['pi_int'], st['bearing'],
              float(st.get('at', 0)), float(st.get('until', T)), float(st.get('cnr_db', 30))))
    N = int(round(T * rate))
    PIECE_I = int(0.1 * FI)
    sh_w = 2 * np.pi * off / rate
    with open(out, 'wb') as fo:
        for c0 in range(0, int(round(T * FI)), PIECE_I):
            c1 = min(int(round(T * FI)), c0 + PIECE_I)
            n = (c1 - c0) * L
            o0 = c0 * L
            x = ((rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n).astype(np.complex64)
            if sts:
                s = np.zeros(n, np.complex64)
                for k in range(len(sts)):
                    y = ups[k].push(sts[k][c0:c1])
                    if off:
                        y = y * np.exp(1j * sh_w * np.arange(o0, o0 + n)).astype(np.complex64)
                    x += y[:, None] * steers[k][None, :]
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                (x / np.float32(127.5)).astype(np.complex64).tofile(fo)
    side = {'rate': rate, 'channels': M, 'rf_hz': rf, 'radius_m': Rr, 'offset_hz': off, 'noise_lsb': sig_n,
            'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at %.0f kHz (%s)' % (out, M, N / rate, rate / 1e3, fmt))


if __name__ == '__main__':
    main()
