#!/usr/bin/env python3
"""Test transmissions for ID separation: single-channel recordings (real or
from a plugin's generator) placed in time, frequency and DIRECTION onto
kraken_doa's uniform circular array - every transmitter at its own known
bearing - so a decoder's per-talker DoA can be checked against the truth.

  uca_scene.py SCENE.json OUT.cf32 [--wide OUT.cu8 [--wide-offset HZ]] [--clean]

OUT.cf32: every antenna, sample by sample (cf32, antenna 0 first) at the
scene's rate, + OUT.json (rate, channels, rf_hz, radius_m, truth). Test it
with the decoder itself:

  plugins/<id>/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 [--offset HZ]

(antenna 0 is decoded; the summary lists every talker's bearing from its
own samples of all antennas). --wide also writes the scene at 2.4 MHz as
8-bit IQ of every antenna (sample by sample), the band placed at
--wide-offset Hz, for a fake heimdall feeding the real kraken_doa.
--clean leaves the noise out of OUT.cf32 and writes its rms (per sample at
the scene's rate, in the file's scale) as "noise_rms" into OUT.json:
kraken_doa's debug playback adds the noise itself over the whole 2.4 MHz
(make_testsignal.py).

SCENE (JSON):
{
  "rate": 48000,              output sample rate (Hz)
  "rf_hz": 144.8e6,           carrier frequency (sets the wavelength)
  "channels": 5, "radius_m": 0.12,
  "seconds": 30,
  "snr_db": 25,               per source at gain_db 0, noise in the whole output band
  "seed": 1,
  "sources": [{
     "name": "N0CALL",        for the truth list
     "file": "x.cf32",        cf32 | cu8 | cs16 IQ, or "format": "disc_s16" = an FM
                              discriminator recording (S16LE mono, e.g. dsd's .dis /
                              .raw), FM-modulated with "dev_hz" Hz per full scale or
                              its rms scaled to "rms_hz" (default 1500); "invert": true
                              mirrors it
     "rate": 48000,           the file's sample rate
     "from": 0, "dur": 5,     the part of the file to use (s; dur 0 = to the end)
     "at": 1.0,               where it starts in the scene (s)
     "bearing": 60,           degrees, as the MUSIC DoA plot shows it (CCW from ANT0)
     "gain_db": 0, "shift_hz": 0
  }]
}
Antennas: kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE; a plane wave
from bearing b reaches antenna k with phase 2 pi r / lambda cos(b - phi_k),
phi_k = -2 pi k / M.
"""
import json
import sys
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly

WIDE_RATE = 2.4e6


def load(src):
    path = src['file']
    fmt = src.get('format') or path.rsplit('.', 1)[-1]
    if fmt == 'disc_s16':
        d = np.fromfile(path, dtype='<i2').astype(np.float64)
        d -= d.mean()
        rate = float(src['rate'])
        # Hz: dev_hz per full scale, else the recording's rms -> rms_hz (default
        # 1500 Hz, a 4FSK / GMSK voice signal); invert = mirrored spectrum
        hz = d / 32768.0 * float(src['dev_hz']) if 'dev_hz' in src else d / max(d.std(), 1e-9) * float(src.get('rms_hz', 1500))
        if src.get('invert'):
            hz = -hz
        x = np.exp(2j * np.pi * np.cumsum(hz) / rate)
    elif fmt in ('cu8', 'u8'):
        d = np.fromfile(path, dtype=np.uint8).astype(np.float64)
        x = (d[0::2] - 127.5) / 127.5 + 1j * (d[1::2] - 127.5) / 127.5
    elif fmt in ('cs16', 's16'):
        d = np.fromfile(path, dtype='<i2').astype(np.float64) / 32768.0
        x = d[0::2] + 1j * d[1::2]
    else:
        x = np.fromfile(path, dtype=np.complex64).astype(np.complex128)
    rate = float(src['rate'])
    a = int(src.get('from', 0) * rate)
    n = int(src.get('dur', 0) * rate)
    x = x[a:a + n] if n > 0 else x[a:]
    return x - (x.mean() if src.get('dc', False) else 0), rate


def resample(x, r_in, r_out):
    if abs(r_in - r_out) < 1e-6:
        return x
    f = Fraction(r_out / r_in).limit_denominator(1000)
    return resample_poly(x, f.numerator, f.denominator)


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(a[0]))
    out = a[1]
    wide = None
    wide_off = 0.0
    clean = '--clean' in a
    for i, v in enumerate(a):
        if v == '--wide':
            wide = a[i + 1]
        if v == '--wide-offset':
            wide_off = float(a[i + 1])
    rate = float(scene['rate'])
    M = int(scene.get('channels', 5))
    r = float(scene.get('radius_m', 0.12))
    lam = 299792458.0 / float(scene['rf_hz'])
    N = int(float(scene['seconds']) * rate)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    phi = -2 * np.pi * np.arange(M) / M
    sig = np.zeros((M, N), np.complex128)
    truth = []
    for s in scene['sources']:
        x, r_in = load(s)
        x = resample(x, r_in, rate)
        # unit power over the part that is on the air (a burst's silence doesn't count)
        p = np.abs(x) ** 2
        on = p > 0.05 * np.percentile(p, 99)
        x = x / np.sqrt(max(p[on].mean(), 1e-20)) * 10 ** (s.get('gain_db', 0) / 20)
        n0 = int(round(s.get('at', 0) * rate))
        if n0 >= N:
            continue
        x = x[:N - n0]
        k = np.arange(len(x))
        if s.get('shift_hz', 0):
            x = x * np.exp(2j * np.pi * s['shift_hz'] * (k + n0) / rate)
        b = np.deg2rad(s['bearing'])
        steer = np.exp(2j * np.pi * r / lam * np.cos(b - phi))
        sig[:, n0:n0 + len(x)] += steer[:, None] * x[None, :]
        truth.append({'name': s.get('name', s['file']), 'bearing': s['bearing'], 'at': n0 / rate,
                      'dur': len(x) / rate, 'shift_hz': s.get('shift_hz', 0)})
        print('%-14s %7.2f .. %7.2f s  bearing %6.1f deg  %+8.0f Hz' % (truth[-1]['name'], n0 / rate,
              (n0 + len(x)) / rate, s['bearing'], s.get('shift_hz', 0)))
    sigma = 10 ** (-float(scene.get('snr_db', 25)) / 20)
    if clean:
        noisy = sig.copy()
    else:
        noisy = sig + (rng.standard_normal((M, N)) + 1j * rng.standard_normal((M, N))) * sigma / np.sqrt(2)
    peak = max(np.max(np.abs(noisy)), 1e-9)
    noisy /= peak * 1.05
    noisy.T.astype(np.complex64).tofile(out)
    side = {'rate': rate, 'channels': M, 'rf_hz': float(scene['rf_hz']), 'radius_m': r, 'truth': truth}
    if clean:
        side['noise_rms'] = sigma / (peak * 1.05)
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at %.0f Hz' % (out, M, N / rate, rate))
    if not wide:
        return
    # 2.4 MHz, the band at wide_off, noise of the same density over the whole
    # 2.4 MHz, 8-bit; in pieces (memory)
    f = Fraction(WIDE_RATE / rate).limit_denominator(1000)
    up, dn = f.numerator, f.denominator
    sig /= peak * 1.05
    sigma_w = sigma / (peak * 1.05) * np.sqrt(WIDE_RATE / rate)
    scale = 0.25 / max(sigma_w * 3, 0.3)   # leave headroom in the 8 bits
    piece = dn * int(0.25 * rate / dn)     # input samples per piece
    pad = dn * 64
    with open(wide, 'wb') as fo:
        n_done = 0
        for c0 in range(0, N, piece):
            c1 = min(N, c0 + piece)
            a0, a1 = max(0, c0 - pad), min(N, c1 + pad)
            y = resample_poly(sig[:, a0:a1], up, dn, axis=1)
            o0 = (c0 - a0) * up // dn
            y = y[:, o0:o0 + (c1 - c0) * up // dn]
            k = np.arange(y.shape[1]) + n_done
            y = y * np.exp(2j * np.pi * wide_off * k / WIDE_RATE)[None, :]
            y = y + (rng.standard_normal(y.shape) + 1j * rng.standard_normal(y.shape)) * sigma_w / np.sqrt(2)
            y *= scale
            iq = np.empty((y.shape[1], M, 2), np.float32)
            iq[:, :, 0] = y.real.T
            iq[:, :, 1] = y.imag.T
            np.clip(np.rint(iq * 127.5 + 127.5), 0, 255).astype(np.uint8).tofile(fo)
            n_done += y.shape[1]
    json.dump({'rate': WIDE_RATE, 'channels': M, 'rf_hz': float(scene['rf_hz']) - wide_off, 'radius_m': r,
               'band_offset_hz': wide_off, 'truth': truth},
              open(wide.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz, band at %+.0f Hz' % (wide, M, n_done / WIDE_RATE, wide_off))


if __name__ == '__main__':
    main()
