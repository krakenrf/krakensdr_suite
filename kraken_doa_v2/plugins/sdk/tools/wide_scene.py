#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KrakenRF Inc. - the plugin SDK (plugins/sdk/LICENSE)
"""A WIDE test scene: narrowband transmitters (a plugin generator's cf32 at
48 / 72 / 96 kHz ..., or real recordings) placed ANYWHERE in the receiver's
2.4 MHz - each at its own frequency offset, start time, bearing and level on
kraken_doa's uniform circular array - with noise over the whole band, written
as the multi-antenna 8-bit file a wide debug-playback entry's "output" is.
For signals that need more than one narrow band: a trunked system's control
channel and its traffic channels several hundred kHz apart, neighbouring
channels, several systems at once. (uca_scene.py keeps a scene inside
+-0.35 x one low rate.)

  wide_scene.py SCENE.json OUT.cu8

OUT.cu8: 2.4 MHz, every antenna sample by sample (uint8 I/Q, antenna 0
first), + OUT.json (rate, channels, rf_hz = the file's centre, radius_m,
truth) - what make_testsignal.py's "output" and the offline decoders read:

  plugins/<id>/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --offset HZ

(the sidecar gives the rate and the RF; --offset = the source's offset_hz:
the decoder mixes it to 0 Hz and resamples to its own rate).
It is written in blocks of 0.1 s: memory stays ~100 MB whatever the length
(sources are read through memory maps; a disc_s16 source is held whole).

SCENE (JSON):
{
  "rf_hz": 851.5e6,           the 2.4 MHz's centre = the tuner (each source's
                              wavelength is taken at its own rf_hz + offset_hz)
  "channels": 5, "radius_m": 0.12,
  "seconds": 20,
  "snr_db": 20,               a source at gain_db 0 against the noise in snr_bw_hz
  "snr_bw_hz": 48000,         (default 48 kHz, the narrow scenes' usual rate: a
                              12.5 kHz channel then sees ~6 dB more)
  "seed": 1,
  "rms": 0.15,                the 8-bit level: rms of noise + every source at once
                              (as heimdall's), as a fraction of full scale
  "sources": [{
     "name": "CC",            for the truth list
     "file": "cc.cf32",       cf32 | cu8 | cs16 IQ, or "format": "disc_s16" = an FM
                              discriminator recording (S16LE mono), FM-modulated with
                              "dev_hz" Hz per full scale or its rms scaled to "rms_hz"
                              (default 1500); "invert": true mirrors it
     "rate": 48000,           the file's sample rate (any up to 2.4 MHz)
     "from": 0, "dur": 0,     the part of the file to use (s; dur 0 = to the end)
     "loop": false,           true: the part repeats until the scene ends
     "at": 0.5,               where it starts in the scene (s)
     "offset_hz": -400000,    its place in the 2.4 MHz (from the centre)
     "bearing": 60,           degrees, as the MUSIC DoA plot shows it (CCW from ANT0)
     "gain_db": 0, "shift_hz": 0   (shift_hz: an extra carrier error)
  }]
}
Every source is scaled to unit power over the part that is on the air (its
silences don't count) times gain_db. Antennas: kraken_doa's UCA - ANT0 on +x,
the others CLOCKWISE; a plane wave from bearing b reaches antenna k with
phase 2 pi r / lambda cos(b - phi_k), phi_k = -2 pi k / M.
"""
import json
import sys
import time
from fractions import Fraction

import numpy as np
from scipy.signal import firwin, resample_poly

RATE = 2.4e6
BLOCK = 240000          # output samples per block (0.1 s)
NOISE_TABLE = 1 << 21   # complex Gaussian samples drawn once (16 MB), read at random offsets
C = 299792458.0


class Source:
    """one transmitter: its samples (the chosen part of the file, at the
    file's rate) through get(a, b), unit power x gain"""

    def __init__(self, s):
        self.s = s
        path = s['file']
        fmt = s.get('format') or path.rsplit('.', 1)[-1]
        self.rate = float(s['rate'])
        if fmt == 'disc_s16':
            d = np.fromfile(path, dtype='<i2').astype(np.float64)
            d -= d.mean()
            hz = d / 32768.0 * float(s['dev_hz']) if 'dev_hz' in s else d / max(d.std(), 1e-9) * float(s.get('rms_hz', 1500))
            if s.get('invert'):
                hz = -hz
            self.raw = np.exp(2j * np.pi * np.cumsum(hz) / self.rate).astype(np.complex64)
            self.kind = 'c'
        elif fmt in ('cu8', 'u8'):
            self.raw = np.memmap(path, dtype=np.uint8, mode='r')
            self.kind = 'u8'
        elif fmt in ('cs16', 's16'):
            self.raw = np.memmap(path, dtype='<i2', mode='r')
            self.kind = 's16'
        else:
            self.raw = np.memmap(path, dtype=np.complex64, mode='r')
            self.kind = 'c'
        total = self.raw.shape[0] // (1 if self.kind == 'c' else 2)
        self.a = min(total, int(s.get('from', 0) * self.rate))
        n = int(s.get('dur', 0) * self.rate)
        self.n = min(total - self.a, n) if n > 0 else total - self.a
        self.loop = bool(s.get('loop', False))
        self.scale = 1.0
        # unit power over the part on the air (a strided look at long files)
        k = max(1, self.n // 2000000)
        x = self._read(0, self.n, k)
        p = np.abs(x) ** 2
        on = p > 0.05 * np.percentile(p, 99) if p.size else p
        self.scale = 10 ** (float(s.get('gain_db', 0)) / 20) / np.sqrt(max(float(p[on].mean()) if p.size else 0.0, 1e-20))

    def _read(self, a, b, step=1):
        a += self.a
        b += self.a
        if self.kind == 'c':
            return np.asarray(self.raw[a:b:step], dtype=np.complex128)
        d = np.asarray(self.raw[2 * a:2 * b], dtype=np.float64)
        if self.kind == 'u8':
            d = (d - 127.5) / 127.5
        else:
            d = d / 32768.0
        x = d[0::2] + 1j * d[1::2]
        return x[::step]

    def get(self, a, b):
        """samples a .. b of the part (zero outside it; looped if loop)"""
        out = np.zeros(b - a, np.complex128)
        if self.n <= 0:
            return out
        if not self.loop:
            lo, hi = max(a, 0), min(b, self.n)
            if hi > lo:
                out[lo - a:hi - a] = self._read(lo, hi)
            return out * self.scale
        i = a
        while i < b:
            if i < 0:
                i = 0
                continue
            k = i % self.n
            m = min(b - i, self.n - k)
            out[i - a:i - a + m] = self._read(k, k + m)
            i += m
        return out * self.scale


def main():
    a = sys.argv[1:]
    if len(a) != 2:
        print(__doc__)
        sys.exit(2)
    t_start = time.time()
    sc = json.load(open(a[0], encoding='utf-8'))
    out = a[1]
    M = int(sc.get('channels', 5))
    r = float(sc.get('radius_m', 0.12))
    rf = float(sc['rf_hz'])
    N = int(round(float(sc['seconds']) * RATE))
    rng = np.random.default_rng(int(sc.get('seed', 1)))
    phi = -2 * np.pi * np.arange(M) / M
    noise_p = 10 ** (-float(sc.get('snr_db', 20)) / 10) * RATE / float(sc.get('snr_bw_hz', 48000))
    srcs, truth = [], []
    sig_p = 0.0
    for s in sc['sources']:
        src = Source(s)
        f = Fraction(RATE / src.rate).limit_denominator(1000)
        src.up, src.dn = f.numerator, f.denominator
        if abs(src.rate * src.up / src.dn - RATE) > 1e-3:
            sys.exit('%s: %.0f Hz is no ratio of 2.4 MHz this tool can resample exactly' % (s['file'], src.rate))
        # scipy's default filter (Kaiser 5.0, 10 x max(up, dn) taps a side), designed once
        mr = max(src.up, src.dn)
        src.h = firwin(2 * 10 * mr + 1, 1.0 / mr, window=('kaiser', 5.0)).astype(np.float32)
        src.pad = src.dn * 64
        src.n0 = int(round(float(s.get('at', 0)) * RATE))
        src.len = N - src.n0 if src.loop else src.n * src.up // src.dn
        src.len = max(0, min(src.len, N - src.n0))
        src.f = float(s.get('offset_hz', 0)) + float(s.get('shift_hz', 0))
        if abs(src.f) + 0.5 * min(src.rate, RATE) * 0.8 > RATE / 2:
            print('warning: %s at %+.0f Hz reaches past the 2.4 MHz' % (s.get('name', s['file']), src.f))
        lam = C / (rf + src.f)
        b = np.deg2rad(float(s['bearing']))
        src.steer = np.exp(2j * np.pi * r / lam * np.cos(b - phi)).astype(np.complex64)
        # its carrier over one block (k = 0 .. BLOCK-1); a block at n0 = this x e^(j 2 pi f n0 / R)
        src.rot = np.exp(2j * np.pi * src.f * np.arange(BLOCK) / RATE).astype(np.complex64)
        sig_p += 10 ** (float(s.get('gain_db', 0)) / 10)
        srcs.append(src)
        truth.append({'name': s.get('name', s['file']), 'bearing': s['bearing'], 'at': src.n0 / RATE, 'dur': src.len / RATE,
                      'offset_hz': float(s.get('offset_hz', 0)), 'shift_hz': float(s.get('shift_hz', 0)),
                      'rf_hz': rf + float(s.get('offset_hz', 0))})
        print('%-14s %7.2f .. %7.2f s  %+9.0f Hz (%.6f MHz)  bearing %6.1f deg  %+5.1f dB' % (
            truth[-1]['name'], src.n0 / RATE, (src.n0 + src.len) / RATE, src.f, (rf + src.f) / 1e6, float(s['bearing']),
            float(s.get('gain_db', 0))))
    A = float(sc.get('rms', 0.15)) / np.sqrt(noise_p + sig_p)
    sigma = np.float32(np.sqrt(noise_p / 2) * A)
    blk = np.zeros((M, BLOCK), np.complex64)
    # the noise: a table drawn once, each antenna and block reading it from a
    # random offset of its own (independent between antennas at every lag the
    # array sees; a 2.4 MHz x 5 antenna draw per block cost more than the rest)
    table = (rng.standard_normal((NOISE_TABLE, 2), dtype=np.float32).view(np.complex64)[:, 0] * sigma)
    iq = np.empty((BLOCK, M, 2), np.float32)
    clipped = 0
    with open(out, 'wb') as fo:
        for o0 in range(0, N, BLOCK):
            o1 = min(N, o0 + BLOCK)
            B = o1 - o0
            y = blk[:, :B]
            # noise over the whole band, every antenna its own
            for k, off in enumerate(rng.integers(0, NOISE_TABLE - B, M)):
                y[k] = table[off:off + B]
            for src in srcs:
                lo, hi = max(o0, src.n0), min(o1, src.n0 + src.len)
                if hi <= lo:
                    continue
                j0, j1 = lo - src.n0, hi - src.n0          # the source's own output samples
                a0 = max(0, (j0 // src.up) * src.dn - src.pad)
                a1 = -(-j1 * src.dn // src.up) + src.pad
                x = src.get(a0, a1).astype(np.complex64)
                if src.up != src.dn:
                    x = resample_poly(x, src.up, src.dn, window=src.h)
                base = a0 * src.up // src.dn
                x = x[j0 - base:j1 - base]
                x = x.astype(np.complex64, copy=False) * (src.rot[:hi - lo] *
                                                          np.complex64(A * np.exp(2j * np.pi * (np.mod(src.f * lo, RATE) / RATE))))
                y[:, lo - o0:hi - o0] += src.steer[:, None] * x[None, :]
            # 8 bits, sample by sample
            v = iq[:B]
            v[:, :, 0] = y.real.T
            v[:, :, 1] = y.imag.T
            v *= 127.5
            v += 127.5
            np.rint(v, out=v)
            clipped += int(np.count_nonzero((v < 0) | (v > 255)))
            np.clip(v, 0, 255, out=v)
            v.astype(np.uint8).tofile(fo)
    side = {'rate': RATE, 'channels': M, 'rf_hz': rf, 'radius_m': r, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    dt = time.time() - t_start
    print('%s: %d antennas, %.1f s at 2.4 MHz, %d sources, %s clipped values, %.1f s (%.2f x real time)' % (
        out, M, N / RATE, len(srcs), clipped, dt, dt / max(N / RATE, 1e-9)))


if __name__ == '__main__':
    main()
