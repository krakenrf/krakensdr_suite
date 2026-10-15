#!/usr/bin/env python3
"""Synthetic VHF wildlife transmitters (pulsed CW) for the wildlife plugin.

  collar_gen.py tx OUT.cf32 --rate HZ --seconds S [--json '{transmitter}'] [--offset HZ] [--ppm N] ...
      one transmitter, unit-amplitude pulses (no noise) - for
      plugins/sdk/tools/uca_scene.py scenes (the debug playback)
  collar_gen.py scene SCENE.json OUT.cf32|OUT.cu8
      every antenna of a 5-element UCA (sample by sample), written in
      0.05 s pieces (a 2.4 MHz scene of many seconds never sits in memory),
      with noise and interference; + OUT.json (rate, channels, rf_hz,
      radius_m, truth)

A transmitter (JSON object; the tx command takes it with --json, or the
common keys as options):
  type        "train" (a collar: one pulse per period) | "coded" (Lotek /
              Motus style: bursts of 4 pulses, gaps = the tag's code)
  offset_hz   carrier offset from the scene's centre;  drift_hz_s  (linear)
  chirp_hz    the carrier moving this far (linearly) over each pulse - a real
              tag's oscillator pulled by the keying (UAV-RT recording: ~-90 Hz
              over its 16 ms pulses); 0 = none (default)
  ppm         pulses per minute;  width_ms (20; coded 2.5);  jitter_ms (0.5)
  start       first pulse (s);  rise_ms  edges (raised cosine, 1 ms; coded 0.3),
              centred on the half-amplitude points (width_ms = their distance)
  switch_at, switch_ppm   the rate changes there (mortality: x2)
  rates       [[from_s, ppm], ...]: a rate schedule (activity sensors that
              switch back and forth, a mortality switch and back, ...)
  tsd_at [s...], tsd_hours N   Wildtech / DOC time-since-death stream (DOC
              Kiwi Best Practice Manual 6.4.3): double beep, (2 + digit)
              normal pulses per digit, double beeps between, 3 digits
  chick_at [s...], chick [8 numbers]   Wildtech Chick Timer stream (fields
              of the Manual 6.4.4) sent the way the Wildtech kakapo Egg Timer
              manual V6.3 describes: per number a burst of (2 + tens) fast
              pulses, 3 s gap, a burst of (2 + units), then 5 normal pulses
  egg_at, egg [11 numbers]   the Wildtech kakapo Egg Timer (11 outputs)
  double_ms   double-beep spacing (150);  fast_ms  fast pulse spacing (250)
  gaps_ms [g1, g2, g3], bi_s   coded tags (lead to lead, ms; burst interval)
  -- scene only --
  bearing     degrees as the MUSIC plot shows them (CCW from ANT0)
  cn0_dbhz    carrier to noise density of its pulses (dBHz)
  name        for the truth

SCENE: {"rate", "rf_hz", "seconds", "channels": 5, "radius_m": 0.12, "seed",
  "noise": true, "sources": [transmitters],
  "interference": [{"type": "impulse", "per_s": 5, "db": 30} |
                   {"type": "carrier", "offset_hz": .., "cn0_dbhz": ..} |
                   {"type": "fsk", "offset_hz": .., "baud": 2400, "dev_hz": 2400,
                    "burst_s": 0.3, "every_s": 2.0, "cn0_dbhz": ..} |
                   {"type": "voice_fm", "offset_hz": .., "cn0_dbhz": ..}]}
Noise: unit variance per complex sample (N0 = 1 / rate).
"""
import json
import sys

import numpy as np

C = 299792458.0


def pulse_times(tx, seconds):
    """[(lead s, width s)] of one transmitter"""
    rng = np.random.default_rng(int(tx.get('seed', 7)))
    jit = float(tx.get('jitter_ms', 0.5)) / 1e3
    if tx.get('type', 'train') == 'coded':
        w = float(tx.get('width_ms', 2.5)) / 1e3
        g = [x / 1e3 for x in tx['gaps_ms']]
        bi = float(tx['bi_s'])
        out = []
        t = float(tx.get('start', 0.3))
        while t < seconds:
            out.append((t, w))
            a = t
            for gi in g:
                a += gi
                out.append((a, w))
            t += bi
        return [p for p in out if p[0] + p[1] < seconds]
    w = float(tx.get('width_ms', 20)) / 1e3
    period = 60.0 / float(tx.get('ppm', 30))
    # rate schedule: [[from s, ppm], ...] (switch_at / switch_ppm = one entry)
    sched = [(float(a), 60.0 / float(b)) for a, b in tx.get('rates', [])]
    if 'switch_at' in tx:
        sched.append((float(tx['switch_at']), 60.0 / float(tx.get('switch_ppm', 60))))
    sched.sort()

    def period_at(x):
        p = period
        for a, q in sched:
            if x >= a:
                p = q
        return p
    dbl = float(tx.get('double_ms', 150)) / 1e3
    fast = float(tx.get('fast_ms', 250)) / 1e3
    streams = []
    hours = int(tx.get('tsd_hours', 139))
    for ts in tx.get('tsd_at', []):
        streams.append((float(ts), 'tsd', [int(c) for c in '%03d' % hours]))
    for ts in tx.get('chick_at', []):
        streams.append((float(ts), 'burst', list(tx['chick'])))
    for ts in tx.get('egg_at', []):
        streams.append((float(ts), 'burst', list(tx['egg'])))
    streams.sort()
    out = []
    t = float(tx.get('start', 0.5))
    si = 0
    while t < seconds:
        P = period_at(t)
        if si < len(streams) and t >= streams[si][0]:
            _, kind, data = streams[si]
            si += 1
            if kind == 'tsd':
                def dbeep(x):
                    out.append((x, w))
                    out.append((x + dbl, w))
                dbeep(t)
                t += P
                for d in data:
                    for _ in range(d + 2):
                        out.append((t, w))
                        t += P
                    dbeep(t)
                    t += P
            else:
                for num in data:
                    for i, d in enumerate((num // 10, num % 10)):
                        for k in range(d + 2):
                            out.append((t + k * fast, w))
                        t += (d + 1) * fast + (3.0 if i == 0 else P)
                    for _ in range(5):
                        out.append((t, w))
                        t += P
            continue
        out.append((t + (rng.standard_normal() * jit if jit > 0 else 0), w))
        t += P
    return [p for p in out if 0 <= p[0] and p[0] + p[1] < seconds]


class Renderer:
    """one transmitter's complex baseband, piece by piece"""

    def __init__(self, tx, rate, seconds):
        self.p = pulse_times(tx, seconds)
        self.lead = np.array([a for a, _ in self.p])
        self.rate = rate
        self.f0 = float(tx.get('offset_hz', 0))
        self.drift = float(tx.get('drift_hz_s', 0))
        self.rise = float(tx.get('rise_ms', 0.3 if tx.get('type') == 'coded' else 1.0)) / 1e3
        self.ph0 = float(tx.get('phase', 0.3))
        self.chirp = float(tx.get('chirp_hz', 0))

    def piece(self, n0, n):
        t = (n0 + np.arange(n)) / self.rate
        env = np.zeros(n)
        cph = np.zeros(n)   # the in-pulse chirp's phase (analytic per pulse: piece independent)
        t_a, t_b = t[0], t[-1]
        i0 = np.searchsorted(self.lead, t_a - 0.2)
        r2 = self.rise / 2
        for a, w in self.p[i0:]:
            if a - r2 > t_b:
                break
            if a + w + r2 < t_a:
                continue
            k0 = max(0, int(np.floor((a - r2 - t_a) * self.rate)))
            k1 = min(n, int(np.ceil((a + w + r2 - t_a) * self.rate)) + 1)
            tt = t[k0:k1]
            # raised-cosine edges centred on the half-amplitude points a, a + w
            up = np.clip((tt - (a - r2)) / self.rise, 0, 1)
            dn = np.clip(((a + w + r2) - tt) / self.rise, 0, 1)
            e = (0.5 - 0.5 * np.cos(np.pi * up)) * (0.5 - 0.5 * np.cos(np.pi * dn))
            env[k0:k1] = np.maximum(env[k0:k1], e)
            if self.chirp:
                u = np.clip((tt - a) / w, 0, 1)
                cph[k0:k1] = 2 * np.pi * self.chirp * (0.5 * u * u * w + np.maximum(tt - a - w, 0))
        ph = 2 * np.pi * (self.f0 * t + 0.5 * self.drift * t * t) + self.ph0 + cph
        return env * np.exp(1j * ph)


def cmd_tx(a):
    out = a[0]
    opts = {}
    tx = {}
    i = 1
    while i < len(a):
        k = a[i].lstrip('-')
        v = a[i + 1]
        i += 2
        if k == 'json':
            tx.update(json.loads(v))
        elif k in ('rate', 'seconds'):
            opts[k] = float(v)
        else:
            try:
                tx[k] = json.loads(v)
            except ValueError:
                tx[k] = v
    rate, seconds = opts['rate'], opts['seconds']
    r = Renderer(tx, rate, seconds)
    n = int(seconds * rate)
    with open(out, 'wb') as fo:
        piece = int(rate * 0.1)
        for n0 in range(0, n, piece):
            r.piece(n0, min(piece, n - n0)).astype(np.complex64).tofile(fo)
    json.dump({'rate': rate, 'pulses': len(r.p)}, open(out.rsplit('.', 1)[0] + '.json', 'w'))
    print('%s: %d pulses, %.1f s at %.0f Hz' % (out, len(r.p), seconds, rate))


def cmd_scene(a):
    scene = json.load(open(a[0]))
    out = a[1]
    if out.rsplit('.', 1)[0] + '.json' == a[0]:
        sys.exit('the scene would be overwritten by the output sidecar')
    rate = float(scene['rate'])
    secs = float(scene['seconds'])
    M = int(scene.get('channels', 5))
    rad = float(scene.get('radius_m', 0.12))
    lam = C / float(scene['rf_hz'])
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    phi = -2 * np.pi * np.arange(M) / M
    srcs = []
    truth = []
    for s in scene.get('sources', []):
        r = Renderer(s, rate, secs)
        amp = np.sqrt(10 ** (float(s.get('cn0_dbhz', 40)) / 10) / rate)
        b = np.deg2rad(float(s.get('bearing', 0)))
        steer = np.exp(2j * np.pi * rad / lam * np.cos(b - phi))
        srcs.append((r, amp, steer))
        truth.append({'name': s.get('name', ''), 'bearing': s.get('bearing', 0), 'offset_hz': s.get('offset_hz', 0),
                      'pulses': len(r.p), 'cn0_dbhz': s.get('cn0_dbhz', 40)})
    inter = scene.get('interference', [])
    noise = bool(scene.get('noise', True))
    cu8 = out.endswith('.cu8')
    scale = 0.25 / 3.0
    n = int(secs * rate)
    piece = max(1, int(rate * 0.05))
    with open(out, 'wb') as fo:
        for n0 in range(0, n, piece):
            k = min(piece, n - n0)
            y = np.zeros((M, k), np.complex128)
            if noise:
                y += (rng.standard_normal((M, k)) + 1j * rng.standard_normal((M, k))) / np.sqrt(2)
            for r, amp, steer in srcs:
                x = r.piece(n0, k)
                if np.any(x):
                    y += steer[:, None] * (amp * x)[None, :]
            t = (n0 + np.arange(k)) / rate
            for it in inter:
                typ = it['type']
                b = np.deg2rad(float(it.get('bearing', 90)))
                st = np.exp(2j * np.pi * rad / lam * np.cos(b - phi))
                amp = np.sqrt(10 ** (float(it.get('cn0_dbhz', 50)) / 10) / rate)
                if typ == 'impulse':
                    # broadband clicks (ignition, power lines): a few samples each
                    cnt = rng.poisson(float(it.get('per_s', 5)) * k / rate)
                    for _ in range(cnt):
                        p = rng.integers(0, k)
                        ln = max(1, int(rate * 20e-6))
                        a = 10 ** (float(it.get('db', 30)) / 20)
                        y[:, p:p + ln] += a * np.exp(2j * np.pi * rng.random()) * st[:, None]
                elif typ == 'carrier':
                    y += st[:, None] * (amp * np.exp(2j * np.pi * float(it.get('offset_hz', 0)) * t))[None, :]
                elif typ == 'fsk':
                    every = float(it.get('every_s', 2.0))
                    bl = float(it.get('burst_s', 0.3))
                    on = (t % every) < bl
                    if np.any(on):
                        baud = float(it.get('baud', 2400))
                        sym = np.floor(t * baud).astype(np.int64)
                        bits = ((sym * 2654435761) >> 7) & 1
                        f = float(it.get('offset_hz', 0)) + float(it.get('dev_hz', 2400)) * (2 * bits - 1)
                        ph = 2 * np.pi * np.cumsum(f) / rate + it.setdefault('_ph', 0.0)
                        it['_ph'] = float(ph[-1] % (2 * np.pi))
                        y += st[:, None] * (amp * on * np.exp(1j * ph))[None, :]
                elif typ == 'voice_fm':
                    m = np.sin(2 * np.pi * 400 * t) * np.sin(2 * np.pi * 3.1 * t) + 0.5 * np.sin(2 * np.pi * 1250 * t)
                    f = float(it.get('offset_hz', 0)) + 2500 * m
                    ph = 2 * np.pi * np.cumsum(f) / rate + it.setdefault('_ph', 0.0)
                    it['_ph'] = float(ph[-1] % (2 * np.pi))
                    y += st[:, None] * (amp * np.exp(1j * ph))[None, :]
            if cu8:
                iq = np.empty((k, M, 2), np.float64)
                iq[:, :, 0] = (y.real * scale).T
                iq[:, :, 1] = (y.imag * scale).T
                np.clip(np.rint(iq * 127.5 + 127.5), 0, 255).astype(np.uint8).tofile(fo)
            else:
                y.T.astype(np.complex64).tofile(fo)
    side = {'rate': rate, 'channels': M, 'rf_hz': float(scene['rf_hz']), 'radius_m': rad, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    for tr in truth:
        print('%-12s %+9.0f Hz  bearing %6.1f  %5.1f dBHz  %d pulses' % (tr['name'], tr['offset_hz'], tr['bearing'],
                                                                         tr['cn0_dbhz'], tr['pulses']))
    print('%s: %d antennas, %.1f s at %.0f Hz' % (out, M, secs, rate))


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('tx', 'scene'):
        print(__doc__)
        sys.exit(2)
    if sys.argv[1] == 'tx':
        cmd_tx(sys.argv[2:])
    else:
        cmd_scene(sys.argv[2:])


if __name__ == '__main__':
    main()
