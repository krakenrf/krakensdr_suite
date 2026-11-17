#!/usr/bin/env python3
"""Synthetic off-air analog TV (PAL / NTSC) for the `analogtv` decoder.

Renders a test-card luma pattern into a composite video signal with proper
H-sync / blanking / active timing and two interlaced fields, negative-AM
modulates it onto the vision carrier (carrier strongest at sync, weakest at
peak white) and writes it as an SDR recording.

  analogtv_gen.py OUT.cf32 [--standard pal|ntsc] [--seconds S] [--rate HZ]
                  [--channels M] [--bearing DEG] [--rf HZ] [--noise F]
                  [--gt OUT.pgm]

OUT by extension: .cf32 (complex float32) or .cu8 (unsigned 8-bit I/Q, as
heimdall delivers). With --channels > 1 the antennas are written sample by
sample (antenna 0 first) on kraken_doa's clockwise UCA (ANT0 on +x) - the
wide multichannel scene the Debug Playback plays. OUT.json (the sidecar) gets
rate / channels / rf_hz / radius_m. --gt writes the ground-truth luma as a
PGM (active_lines x width) for the offline correlation harness.

The clean-room test card and all timing come from ITU-R BT.470 / BT.1700;
no copyrighted assets and no GPL analog-TV code are used.
"""
import json
import math
import sys

import numpy as np

C = 299792458.0

# Per-standard geometry and horizontal timing (microseconds). line = front
# porch + sync + back porch + active; active_us chosen so they fill the line.
STD = {
    'pal': dict(name='PAL', system='625/50', line_rate=15625.0, field_rate=50.0,
                total_lines=625, active_lines=576, line_us=64.0, fp=1.65, sync=4.7, bp=5.7),
    'ntsc': dict(name='NTSC', system='525/60', line_rate=15734.266, field_rate=60000.0 / 1001.0,
                 total_lines=525, active_lines=480, line_us=1e6 / 15734.266, fp=1.5, sync=4.7, bp=4.5),
}

# envelope amplitude (fraction of full scale): sync tip = strongest carrier,
# peak white = weakest (negative modulation). A wide sync-to-black gap keeps
# the vertical-sync runs well clear of even the darkest picture content.
SYNC_AMP = 0.95
BLANK_AMP = 0.55
BLACK_AMP = 0.53
WHITE_AMP = 0.08


def test_card(rows, cols):
    """A clean-room grayscale test card, rows x cols float in [0,1]
    (0 = black, 1 = white): a diagonal background, a bright centre disc, a
    grey staircase, a block of vertical bars and a couple of big squares."""
    y = (np.arange(rows) / rows)[:, None]
    x = (np.arange(cols) / cols)[None, :]
    img = 0.20 + 0.45 * x + 0.20 * (1.0 - y)            # diagonal background wedge

    # top 16%: an 8-step grey staircase
    top = y[:, 0] < 0.16
    stair = np.floor(x * 8) / 7.0
    img[top, :] = stair

    # a band of vertical bars around the middle
    band = (y[:, 0] >= 0.62) & (y[:, 0] < 0.82)
    bars = (np.floor(x * 12) % 2).astype(float)
    img[band, :] = bars

    # two big solid squares (one white, one black) lower-left / lower-right
    sq = (y >= 0.84) & (y < 0.98)
    img[np.where(sq & (x < 0.22))] = 1.0
    img[np.where(sq & (x > 0.78))] = 0.0

    # a bright centre disc with a thin dark ring (circle in normalized coords)
    cx, cy, r = 0.5, 0.42, 0.20
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    img = np.where(d < r, 0.95, img)
    img = np.where((d >= r) & (d < r + 0.015), 0.05, img)

    return np.clip(img, 0.0, 1.0)


def smooth(x, n):
    if n <= 1:
        return x
    w = np.hanning(n + 2)[1:-1]
    return np.convolve(x, w / w.sum(), mode='same')


def frame_envelope(p, rate, gt, out_w):
    """One full interlaced frame (625 / 525 lines) of the composite envelope
    (float, fraction of full scale). The two fields are split at total/2 =
    312.5 / 262.5 lines, so the second field's lines are offset by half a
    line - the real interlace geometry; vsync-to-vsync is exactly total/2
    line periods, which the decoder divides by to recover the line period."""
    line_us = p['line_us']
    spl = rate / p['line_rate']                       # samples per line (fractional)
    total = p['total_lines']                           # 625 / 525
    half = total / 2.0                                 # 312.5 / 262.5 (one field, fractional)
    active_fl = p['active_lines'] // 2                  # 288 / 240
    skip = total // 2 - active_fl                       # vertical-blanking lines per field (24 / 22)
    active_start = p['fp'] + p['sync'] + p['bp']
    active_us = line_us - active_start
    nf = int(round(total * spl))

    i = np.arange(nf)
    glf = i / spl                                      # continuous line coordinate, 0..total
    field = (glf >= half).astype(np.int64)             # 0 (first field) / 1 (second)
    lf = glf - field * half                            # line within the field (fractional)
    il = np.floor(lf).astype(np.int64)                 # integer line index in the field
    phase = (lf - il) * line_us                        # us into the line

    amp = np.full(nf, BLANK_AMP, dtype=np.float64)

    # normal horizontal sync pulse on every line
    amp[(phase >= p['fp']) & (phase < p['fp'] + p['sync'])] = SYNC_AMP

    # vertical sync: the first 3 lines of each field are broad pulses (sync
    # level for most of each half-line, with a short serration) - a long run
    # the decoder locks the field on
    vs = il < 3
    halfp = np.mod(phase, line_us / 2.0)
    amp[vs] = BLANK_AMP
    amp[vs & (halfp < (line_us / 2.0 - 2.0))] = SYNC_AMP

    # active video on the picture lines (field 0 -> even rows, field 1 -> odd)
    act = (il >= skip) & (il < skip + active_fl) & (phase >= active_start) & (phase < line_us)
    k = np.clip(il - skip, 0, active_fl - 1)
    prow = np.clip(2 * k + field, 0, p['active_lines'] - 1)
    xf = (phase - active_start) / active_us
    col = np.clip((xf * out_w).astype(np.int64), 0, out_w - 1)
    amp[act] = BLACK_AMP + (WHITE_AMP - BLACK_AMP) * gt[prow[act], col[act]]

    # round the edges (sync notch gets a defined centre tip; realistic band-limit)
    amp = smooth(amp, max(2, int(round(2.2e-6 * rate))))
    return amp.astype(np.float32)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    out = None
    std = 'pal'
    seconds = 2.0
    rate = 2.4e6
    channels = 1
    bearing = 40.0
    rf = 200e6
    noise = 0.012
    gt_path = None
    radius = 0.12
    seed = 1
    i = 0
    while i < len(a):
        t = a[i]
        if t == '--standard':
            std = a[i + 1].lower(); i += 2
        elif t == '--seconds':
            seconds = float(a[i + 1]); i += 2
        elif t == '--rate':
            rate = float(a[i + 1]); i += 2
        elif t == '--channels':
            channels = int(a[i + 1]); i += 2
        elif t == '--bearing':
            bearing = float(a[i + 1]); i += 2
        elif t == '--rf':
            rf = float(a[i + 1]); i += 2
        elif t == '--noise':
            noise = float(a[i + 1]); i += 2
        elif t == '--gt':
            gt_path = a[i + 1]; i += 2
        elif t == '--radius':
            radius = float(a[i + 1]); i += 2
        elif t == '--seed':
            seed = int(a[i + 1]); i += 2
        elif t.startswith('--'):
            sys.exit('unknown option ' + t)
        else:
            out = t; i += 1
    if out is None:
        sys.exit('need an output file')
    if std not in STD:
        sys.exit('standard must be pal or ntsc')
    p = STD[std]
    fmt = out.rsplit('.', 1)[-1].lower()
    if fmt not in ('cf32', 'cu8'):
        sys.exit('output must be .cf32 or .cu8')

    out_w = 720
    gt = test_card(p['active_lines'], out_w)
    env = frame_envelope(p, rate, gt, out_w)           # one frame
    nf = env.shape[0]

    M = max(1, channels)
    phi = -2 * np.pi * np.arange(M) / M
    lam = C / rf
    steer = np.exp(2j * np.pi * radius / lam * np.cos(np.deg2rad(bearing) - phi)).astype(np.complex64)
    steer[0] = steer[0] * np.exp(1j * 0.6)             # a non-trivial phase so I and Q both carry it
    rng = np.random.default_rng(seed)

    total = int(round(seconds * rate))
    written = 0
    with open(out, 'wb') as fo:
        while written < total:
            k0 = written % nf
            take = min(nf - k0, total - written)
            base = env[k0:k0 + take]                    # (take,) real envelope
            n = take
            # per-antenna complex: envelope * steering + complex noise
            x = base[:, None] * steer[None, :] + (rng.standard_normal((n, M))
                                                  + 1j * rng.standard_normal((n, M))).astype(np.complex64) * np.float32(noise)
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + 127.5 * x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + 127.5 * x.imag), 0, 255)
                u.tofile(fo)
            else:
                x.astype(np.complex64).tofile(fo)
            written += take

    side = {'rate': rate, 'channels': M, 'rf_hz': rf, 'radius_m': radius,
            'noise_rms': float(noise), 'standard': p['name'],
            'truth': [{'name': p['name'], 'standard': p['name'], 'bearing': bearing}]}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)

    if gt_path:
        with open(gt_path, 'wb') as fg:
            fg.write(('P5\n%d %d\n255\n' % (out_w, p['active_lines'])).encode())
            np.clip(np.rint(gt * 255), 0, 255).astype(np.uint8).tofile(fg)

    print('%s: %s %s, %.2f s, %d antenna(s) at %.3f MHz, %s%s'
          % (out, p['name'], p['system'], written / rate, M, rate / 1e6, fmt,
             (', gt ' + gt_path) if gt_path else ''))


if __name__ == '__main__':
    main()
