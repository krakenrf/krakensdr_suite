#!/usr/bin/env python3
"""Synthetic pulse radars at known bearings on kraken_doa's UCA, at 2.4 MHz -
test transmissions for the radar plugin (parameters, ID separation).

  radar_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces
(a 5-antenna 2.4 MHz scene is 24 MB/s as cu8, 96 MB/s as cf32 - keep scenes
short and use cu8 when you can). OUT.json gets rate, channels, rf_hz,
radius_m and the truth (each radar's bearing, frequencies, PRIs, scan).

  cu8   8-bit IQ like the receiver's (127.5 + x, clipped to 0..255): the
        main beam clips the ADC when it is strong - like the real thing.
        Plays through a fake heimdall (8091 format) into kraken_doa too
  cf32  the same scaled to full scale 1.0 (+-1 clip), no quantisation

Test:  plugins/radar/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf RF_HZ

SCENE (JSON):
{
  "rf_hz": 1355e6,            the band's centre (sets the wavelength)
  "seconds": 12, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 1.0,           receiver noise rms per I / Q (8-bit LSB)
  "radars": [{
    "name": "R1",             for the truth list
    "bearing": 60,            degrees, as the MUSIC DoA plot shows it (CCW from ANT0)
    "peak_db": 40,            main beam: pulse power over the noise (per sample)
    "pulses": [               the pulse group sent every PRI (one or more)
      {"offset_hz": 0,        centre frequency in the band
       "width_us": 60,
       "chirp_hz": -1.2e6,    linear FM sweep over the pulse (0 = plain)
       "delay_us": 0,         after the PRI's start
       "rel_db": 0}],
    "pri": {"type": "constant", "pri_us": 1000, "jitter_pct": 0}
         | {"type": "stagger", "seq_us": [1100, 1250, 1180]}
         | {"type": "blocks", "sets_us": [[1712, 1395], [1748, 1424]], "block": 8}
                              blocks: each PRI for "block" pulses, the next set
                              of PRIs in each turn of the antenna
    "scan_s": 4.8,            antenna turn (0 = not rotating: the main beam on us)
    "beam_deg": 1.4,          -3 dB width of the main beam (Gaussian)
    "sidelobe_db": -30,       sidelobes (with a +-5 dB ripple over azimuth)
    "az0_deg": 90,            where the antenna points at t = 0 (0 = at us)
    "rise_us": 0.1,
    "echo": {"bearing": 200, "delay_us": 15, "rel_db": -12, "az_deg": 120}
                              optional: a reflector lit when the antenna points
                              az_deg away from us; its echo comes from "bearing"
  }]
}
Antennas: kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as
plugins/sdk/tools/uca_scene.py).
"""
import json
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0


def pattern_db(az, beam, side):
    """antenna gain (dB, 0 = main beam peak) at az degrees off the main beam"""
    az = (np.asarray(az, float) + 180) % 360 - 180
    main = -3 * (2 * az / beam) ** 2
    rip = 3 * np.sin(np.deg2rad(az) * 7.3) + 2 * np.sin(np.deg2rad(az) * 19.1 + 1)
    sl = side + rip - np.where(np.abs(az) > 90, 6, 0)
    return np.maximum(main, sl)


def pri_times(r, T, rng):
    """start times of every PRI over [0, T) (+ which set) for one radar"""
    p = r['pri']
    kind = p.get('type', 'constant')
    scan = float(r.get('scan_s', 0))
    az0 = float(r.get('az0_deg', 0))
    t = float(r.get('t0_s', rng.uniform(0, 0.002)))
    out = []
    i = 0
    while t < T:
        out.append(t)
        if kind == 'constant':
            d = float(p['pri_us']) * (1 + float(p.get('jitter_pct', 0)) / 100 * rng.uniform(-1, 1))
        elif kind == 'stagger':
            seq = p['seq_us']
            d = float(seq[i % len(seq)])
        else:
            sets = p['sets_us']
            # the set of this turn of the antenna (turns counted from az = 180, behind)
            turn = int(np.floor(((t / scan * 360 if scan > 0 else 0) + az0 + 180) / 360)) if scan > 0 else 0
            s = sets[turn % len(sets)]
            b = int(p.get('block', 8))
            d = float(s[(i // b) % len(s)])
        t += d * 1e-6
        i += 1
    return np.array(out)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    fmt = out.rsplit('.', 1)[-1]
    if fmt not in ('cu8', 'cf32'):
        sys.exit('output: .cu8 or .cf32')
    import os
    if os.path.abspath(out.rsplit('.', 1)[0] + '.json') == os.path.abspath(sys.argv[1]):
        sys.exit('OUT.json would overwrite the scene: give the output another name')
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene['rf_hz'])
    lam = C / rf
    T = float(scene['seconds'])
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 1.0))
    phi = -2 * np.pi * np.arange(M) / M
    noise_p = 2 * sig_n ** 2   # complex noise power (LSB^2)

    # every pulse of every radar: (start sample (float), radar index, pulse type, amplitude, steering, phase)
    items = []
    truth = []
    for ri, r in enumerate(scene['radars']):
        starts = pri_times(r, T, rng)
        b = np.deg2rad(r['bearing'])
        steer = np.exp(2j * np.pi * R / lam * np.cos(b - phi))
        echo = r.get('echo')
        esteer = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(echo['bearing']) - phi)) if echo else None
        scan = float(r.get('scan_s', 0))
        az0 = float(r.get('az0_deg', 0))
        for k, ts in enumerate(starts):
            az = (ts / scan * 360 if scan > 0 else 0) + az0
            g = pattern_db(az, float(r.get('beam_deg', 1.5)), float(r.get('sidelobe_db', -30)))
            for pi, pu in enumerate(r['pulses']):
                t = ts + float(pu.get('delay_us', 0)) * 1e-6
                lvl = float(r['peak_db']) + float(pu.get('rel_db', 0)) + g
                a = np.sqrt(noise_p * 10 ** (lvl / 10))
                ph = rng.uniform(0, 2 * np.pi)
                items.append((t, ri, pi, a, steer, ph))
                if echo:
                    ge = pattern_db(az - float(echo['az_deg']), float(r.get('beam_deg', 1.5)), float(r.get('sidelobe_db', -30)))
                    le = float(r['peak_db']) + float(pu.get('rel_db', 0)) + float(echo['rel_db']) + ge
                    items.append((t + float(echo['delay_us']) * 1e-6, ri, pi, np.sqrt(noise_p * 10 ** (le / 10)), esteer, ph + 1.0))
        tr = {'name': r.get('name', 'R%d' % ri), 'bearing': r['bearing'],
              'freqs_hz': [rf + float(p.get('offset_hz', 0)) for p in r['pulses']],
              'pulses': r['pulses'], 'pri': r['pri'], 'scan_s': scan, 'beam_deg': r.get('beam_deg', 1.5),
              'peak_db': r['peak_db'], 'mean_pri_us': float(np.mean(np.diff(starts)) * 1e6) if len(starts) > 1 else 0}
        if scan > 0:
            # main beam on us when az = 0 (mod 360)
            k0 = np.ceil(az0 / 360)
            tr['main_beam_s'] = [float((360 * k - az0) / 360 * scan) for k in np.arange(k0, k0 + T / scan + 1) if 0 <= (360 * k - az0) / 360 * scan < T]
        truth.append(tr)
        print('%-8s bearing %6.1f  %d pulses/PRI  %d PRIs  mean PRI %.1f us  scan %.2f s  main beams %s' % (
            tr['name'], r['bearing'], len(r['pulses']), len(starts), tr['mean_pri_us'], scan,
            ', '.join('%.2f' % x for x in tr.get('main_beam_s', []))))
    items.sort(key=lambda x: x[0])
    starts = np.array([x[0] for x in items])

    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            # pulses touching this piece
            lo = np.searchsorted(starts, (c0 - 2 * FS * 1e-3) / FS)
            hi = np.searchsorted(starts, c1 / FS)
            for t, ri, pi, a, steer, ph in items[lo:hi]:
                r = scene['radars'][ri]
                pu = r['pulses'][pi]
                W = float(pu['width_us']) * 1e-6
                k0 = max(c0, int(np.floor(t * FS)))
                k1 = min(c1, int(np.ceil((t + W) * FS)) + 1)
                if k1 <= k0:
                    continue
                k = np.arange(k0, k1)
                tt = k / FS - t
                rise = max(float(r.get('rise_us', 0.1)) * 1e-6, 1e-9)
                env = np.clip(np.minimum(tt, W - tt) / rise, 0, 1)
                env = 0.5 - 0.5 * np.cos(np.pi * env)
                B = float(pu.get('chirp_hz', 0))
                f0 = float(pu.get('offset_hz', 0))
                phase = 2 * np.pi * ((f0 - B / 2) * tt + B / (2 * W) * tt ** 2) + ph
                s = a * env * np.exp(1j * phase)
                x[k - c0, :] += s[:, None] * steer[None, :]
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                y = np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)
                y.astype(np.complex64).tofile(fo)
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s)' % (out, M, N / FS, fmt))


if __name__ == '__main__':
    main()
