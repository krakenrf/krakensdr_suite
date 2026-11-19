#!/usr/bin/env python3
"""Signal classifier: the model on REAL recordings, as the receiver would
see them.

  eval_real.py [--library DIR] [--model M] [--work DIR] [--seconds 4]
               [--jobs 8] [--classes a,b] [--ids SUBSTR] [--out TABLE.tsv]

Library: --library, else $KRAKEN_IQ_LIBRARY, else ~/krakensdr_iq_library
(read-only: its _tools/iqlib.py reads the entries; nothing is copied).

Per entry (up to --seconds from its start; heimdall captures: antenna 0):
  - the receiver's own bytes (own captures, cu8 / heimdall at 2.4 MS/s):
    as they are - exactly what the runtime sees (group native)
  - IQ below 2 MS/s: resampled to 2.4 MHz and placed off the centre (the
    detector ignores +-400 Hz around DC) - +300 kHz, less when the recording
    is wide; the rest of the band filled with white noise at the
    recording's own noise density (its quiet spectrum: a low percentile of
    the PSD), so no pedestal - the residual step between the recording's
    band and the rest is measured and reported (pedestal_db)
  - IQ at >= 2 MS/s: only resampled to 2.4 MHz (wider ones lose what lies
    beyond +-1.2 MHz)
  - disc_audio (receiver audio): FM re-modulated (peak deviation by class),
    noise added for 25 dB in 12.5 kHz - a weaker test, reported apart
  then 8 bits at a realistic level (noise ~6 LSB rms, peaks <= ~120 LSB),
  `sigclass_tool classify --track` over its 0.5 s snapshots, and the tracks
  compared with the entry's `expect` (else its first class anywhere in the
  recording's band).
Groups: native (own off-air captures in the receiver's format; empty-band /
mixed entries count as open set: no shown label is the right answer),
confirmed (a suite decoder or rtl_433's test corpus confirmed it),
probe (spectrum check only), unconfirmed (marked so, or the decoder found 0),
disc_audio, open (a signal type the model has no class for: correct when
labelled unk_* / none / below min_confidence).

Output: the table (TSV, one row per expected signal) + a summary per group
and class on stdout and in TABLE.txt. numpy + scipy only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from fractions import Fraction
from multiprocessing import Pool

import numpy as np
from scipy.signal import resample_poly, welch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bank import default_work, load_classes  # noqa: E402
import model_io  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOL = os.path.join(REPO, 'tools', 'build', 'sigclass_tool')
FS = 2400000.0
DEV = {'dstar': 1200, 'dmr': 1944, 'p25p1': 1800, 'nxdn': 2400, 'ysf': 2700, 'pocsag': 4500, 'flex': 4800, 'aprs': 3000,
       'mpt1327': 2500, 'nfm_voice': 3000, 'v23': 3500}
WEAK = re.compile(r'unconfirmed|\b0 (frames|valid|packets|messages|bursts|events)\b|nothing on|no valid|not decod', re.I)
PROBE = re.compile(r'no decoder run|spectrum probe|format / spectrum|only the format', re.I)
iqlib = None
MERGED = {c['id']: c['merged_into'] for c in load_classes() if c.get('merged_into')}


def group_of(e, known):
    if e.get('provenance') == 'own-capture' and e['format'] in ('cu8', 'heimdall8091') and abs((e.get('sample_rate_hz') or 0) - FS) < 1:
        return 'native'         # the receiver's own bytes at 2.4 MS/s: exactly what the runtime sees
    if e['signals'][0] not in known:
        return 'open'
    if e['format'] == 'disc_audio':
        return 'disc_audio'
    v = str(e.get('verified') or '')
    if 'rtl_433_tests' in e['id'] or 'rtl_433' in e['path']:
        return 'confirmed'      # rtl_433's own test corpus (with its expected decodes)
    if WEAK.search(v):
        return 'unconfirmed'
    if PROBE.search(v) and 'valid' not in v:
        return 'probe'
    return 'confirmed'


def resample(x, rate, out_rate):
    fr = Fraction(out_rate / rate).limit_denominator(2000)
    return resample_poly(x, fr.numerator, fr.denominator).astype(np.complex64), rate * fr.numerator / fr.denominator


def noise_floor_shape(x, rate, nfft=1024):
    """per-bin noise PSD (power / Hz) of the recording: a low running percentile"""
    f, p = welch(x, fs=rate, nperseg=min(nfft, len(x)), return_onesided=False, detrend=False, scaling='density')
    o = np.argsort(f)
    f, p = f[o], p[o]
    k = 16
    pad = np.pad(p, k, mode='edge')
    fl = np.array([np.percentile(pad[i:i + 2 * k + 1], 20) for i in range(len(p))])
    # the quiet level: a low percentile over the recording's band without its filter's
    # roll-off (a signal may fill the middle; the median there overestimated it 40 dB)
    mid = np.abs(f) < 0.42 * rate
    n0 = float(np.percentile(fl[mid], 10))
    return f, fl, n0


def build(e, seconds, rng):
    """-> 2.4 MHz complex samples (LSB units, before quantisation), placement shift, info"""
    x, rate, centre = iqlib.open_entry(e, seconds=seconds, channel=0)
    x = np.asarray(x, np.complex64)
    cls = e['signals'][0]
    info = {}
    if rate is None or len(x) < 1000:
        raise ValueError('no rate / too short')
    if e['format'] == 'disc_audio':
        a = x.real.astype(np.float64)
        a -= a.mean()
        a /= np.percentile(np.abs(a), 99.5) + 1e-12
        a = np.clip(a, -1.2, 1.2)
        z = np.exp(1j * 2 * np.pi * DEV.get(cls, 2500) / rate * np.cumsum(a)).astype(np.complex64)
        y, r2 = resample(z, rate, FS)
        shift = 300e3
        y = y * np.exp(2j * np.pi * shift * np.arange(len(y)) / FS).astype(np.complex64)
        n0 = 1.0 / (10 ** 2.5 * 12500)        # 25 dB in 12.5 kHz for a unit-power carrier
        noise = (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y))) * np.sqrt(n0 * FS / 2)
        y = y + noise.astype(np.complex64)
        info['pedestal_db'] = 0.0
        return y, shift, info, rate, n0
    if e['format'] in ('cu8', 'heimdall8091') and abs(rate - FS) < 1:
        # the receiver's own bytes (rtl_sdr / heimdall at 2.4 MS/s): as they are
        v = np.empty(2 * len(x), np.float32)
        v[0::2] = x.real
        v[1::2] = x.imag
        info['native'] = np.clip(np.rint(v * 127.5 + 127.5), 0, 255).astype(np.uint8)
        info['pedestal_db'] = 0.0
        return x, 0.0, info, rate, 1.0
    x = x - x.mean()
    if rate >= 2e6:
        y = resample(x, rate, FS)[0] if abs(rate - FS) > 1 else x
        f, fl, n0 = noise_floor_shape(y[:min(len(y), 4 << 20)], FS)
        if rate < FS:
            y = fill_noise(y, rate, n0, 0.0, rng)
        info['pedestal_db'] = 0.0
        return y, 0.0, info, rate, n0
    # narrow: the recording's own floor shape, then placed and filled
    f, fl, n0 = noise_floor_shape(x[:min(len(x), 1 << 22)], rate)
    y, r2 = resample(x, rate, FS)
    shift = 300e3 if rate <= 1.2e6 else max(0.0, (0.94 * FS - rate) / 2)
    if shift < 20e3:
        shift = 20e3        # off DC at least
    y = y * np.exp(2j * np.pi * shift * np.arange(len(y)) / FS).astype(np.complex64)
    z = fill_noise(y, rate, n0, shift, rng, f, fl)
    ped = pedestal(z, rate, shift)
    if abs(ped) > 2.0:
        # a recording filtered narrower than its rate: its quiet passband is louder than
        # the stop band the first estimate saw (or quieter: the estimate held signal) -
        # fill to the passband's level
        n0 *= 10 ** (ped / 10)
        z = fill_noise(y, rate, n0, shift, rng, f, fl)
        ped2 = pedestal(z, rate, shift)
        info['pedestal_first_db'] = ped
        ped = ped2
    info['pedestal_db'] = ped
    return z, shift, info, rate, n0


def fill_noise(y, rate, n0, shift, rng, f=None, fl=None):
    """white noise at density n0 where the recording's own floor is below it
    (outside its band, and its anti-alias roll-off)"""
    N = len(y)
    fb = np.fft.fftfreq(N, 1 / FS)
    if f is None:
        own = np.where(np.abs(fb - shift) < 0.45 * rate, n0, 0.0)
    else:
        own = np.interp(fb - shift, f, fl, left=0.0, right=0.0)
        own = np.where(np.abs(fb - shift) <= 0.5 * rate, own, 0.0)
    add = np.maximum(n0 - own, 0.0)
    X = (rng.standard_normal(N) + 1j * rng.standard_normal(N)) * np.sqrt(add * FS * N / 2)
    return (y + np.fft.ifft(X).astype(np.complex64)).astype(np.complex64)


def pedestal(y, rate, shift):
    """quiet level inside the recording's band minus outside it (dB)"""
    f, p = welch(y[:1 << 21], fs=FS, nperseg=4096, return_onesided=False, detrend=False)
    inside = np.abs(f - shift) < 0.35 * rate
    outside = (np.abs(f - shift) > 0.6 * rate) & (np.abs(f) < 0.45 * FS)
    if not inside.any() or not outside.any():
        return 0.0
    return float(10 * np.log10(np.percentile(p[inside], 20) / np.percentile(p[outside], 20)))


def quantise(y, n0):
    """noise ~6 LSB rms per I / Q over the band, peaks <= ~120 LSB"""
    g = np.sqrt(2 * 36.0 / max(n0 * FS, 1e-30))
    pk = float(np.percentile(np.abs(np.concatenate([y.real, y.imag])), 99.99)) * g
    if pk > 120:
        g *= 120 / pk
    v = np.empty(2 * len(y), np.float32)
    v[0::2] = y.real * g
    v[1::2] = y.imag * g
    return np.clip(np.floor(v + 128.0), 0, 255).astype(np.uint8), g


def one(args):
    e, o, known, thr = args
    rng = np.random.default_rng(zlib_hash(e['id']))
    rows = []
    try:
        y, shift, info, rate, n0 = build(e, o.seconds, rng)
    except Exception as ex:         # noqa: BLE001 - an unreadable entry is reported, not fatal
        return [{'id': e['id'], 'group': group_of(e, known), 'class': e['signals'][0], 'error': str(ex)[:120]}]
    if info.get('native') is not None:
        u8, g = info['native'], 1.0
    else:
        u8, g = quantise(y, n0)
    tmp = '/dev/shm/evalreal_%d_%s.cu8' % (os.getpid(), re.sub(r'[^A-Za-z0-9]', '_', e['id'])[-60:])
    u8.tofile(tmp)
    r = subprocess.run([TOOL, 'classify', '--model', o.model, '--track', '--rate', '2400000', '--format', 'cu8', tmp],
                       capture_output=True, text=True)
    os.remove(tmp)
    lines = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
    if not lines:
        return [{'id': e['id'], 'group': group_of(e, known), 'class': e['signals'][0], 'error': 'classify: ' + r.stderr[-200:]}]
    tracks = lines[-1].get('tracks', [])
    grp = group_of(e, known)
    exp = [x for x in (e.get('expect') or []) if x.get('class')] or [{'class': e['signals'][0], 'offset_hz': None, 'bw_hz': None}]
    in_band = rate if rate < 2e6 else FS
    for x in exp:
        if x.get('offset_hz') is None:
            lo, hi = shift - 0.5 * min(in_band, FS), shift + 0.5 * min(in_band, FS)
        else:
            c = shift + float(x['offset_hz'])
            half = 0.5 * max(float(x.get('bw_hz') or 0), 6e3)
            lo, hi = c - half, c + half
        if hi < -0.47 * FS or lo > 0.47 * FS:
            rows.append({'id': e['id'], 'group': grp, 'class': x['class'], 'error': 'outside the 2.4 MHz band'})
            continue
        best = None
        ebw = float(x.get('bw_hz') or 0) if x.get('offset_hz') is not None else 0.0
        for t in tracks:
            tl, th = t['centre_hz'] - 0.5 * t['bw_hz'], t['centre_hz'] + 0.5 * t['bw_hz']
            ov = min(th, hi) - max(tl, lo)
            if ov <= 0:
                continue
            # a track about as wide as the expected signal first (not a wide one that
            # merely covers it), then the most hits, then the most overlap
            fit = 1 if (ebw <= 0 or t['bw_hz'] <= max(4.0 * ebw, 30e3)) else 0
            key = (fit, t['hits'], ov)
            if best is None or key > best[0]:
                best = (key, t)
        row = {'id': e['id'], 'group': grp, 'class': x['class'], 'rate': rate, 'snaps': len(lines), 'pedestal_db': round(info['pedestal_db'], 1),
               'tracks': len(tracks), 'gain': round(float(g), 4)}
        if best is None:
            row.update({'detected': False})
        else:
            t = best[1]
            top = t['top']
            row.update({'detected': True, 'top1': top[0]['id'] if top else '', 'p1': top[0]['p'] if top else 0.0,
                        'top3': [q['id'] for q in top], 'hits': t['hits'], 'det_bw': round(t['bw_hz']), 'det_off': round(t['centre_hz'] - shift),
                        'view': t['view']})
            if grp == 'open' or x['class'] not in known:
                row['ok1'] = (not top) or top[0]['id'].startswith('unk_') or top[0]['id'] == 'none' or top[0]['p'] < thr
            else:
                want = MERGED.get(x['class'], x['class'])     # a class merged into another counts as that one
                row['ok1'] = bool(top) and top[0]['id'] == want
                row['ok3'] = bool(top) and want in row['top3']
                row['shown_ok'] = bool(top) and top[0]['id'] == want and top[0]['p'] >= thr
        # labels the page would show (not none, p >= min_confidence) - on an empty band all false
        row['shown_tracks'] = sum(1 for t in tracks if t['top'] and t['top'][0]['id'] != 'none' and t['top'][0]['p'] >= thr)
        # a wide track over the recording's quiet band = a noise-fill pedestal
        row['wide_extra'] = sum(1 for t in tracks if t['bw_hz'] > 0.6 * min(in_band, FS) and t is not (best[1] if best else None)
                                and rate < 2e6)
        rows.append(row)
    return rows


def zlib_hash(s):
    import zlib
    return zlib.crc32(s.encode())


def summarise(rows, thr, out):
    lines = []

    def p(s=''):
        lines.append(s)
        print(s)
    p('min_confidence %.2f' % thr)
    for grp in ('native', 'confirmed', 'probe', 'unconfirmed', 'disc_audio', 'open'):
        R = [r for r in rows if r['group'] == grp and 'error' not in r]
        E = [r for r in rows if r['group'] == grp and 'error' in r]
        if not R and not E:
            continue
        p()
        if grp == 'open':
            det = [r for r in R if r.get('detected')]
            ok = sum(1 for r in det if r['ok1'])
            p('== open set (no class for these): %d signals, %d detected, %d (%.0f %%) labelled unk_* / none / below min_confidence, %d errors'
              % (len(R), len(det), ok, 100.0 * ok / max(len(det), 1), len(E)))
            by = {}
            for r in det:
                by.setdefault(r['class'], []).append(r)
            for c in sorted(by):
                rs = by[c]
                p('  %-22s n %3d  ok %3d  top-1: %s' % (c, len(rs), sum(r['ok1'] for r in rs),
                                                     ', '.join(sorted({'%s %.2f' % (r['top1'], r['p1']) for r in rs if not r['ok1']})[:4])))
            continue
        n = len(R)
        d = sum(1 for r in R if r.get('detected'))
        o1 = sum(1 for r in R if r.get('ok1'))
        o3 = sum(1 for r in R if r.get('ok3'))
        so = sum(1 for r in R if r.get('shown_ok'))
        p('== %s: %d expected signals, detected %d (%.0f %%), top-1 %d (%.0f %% of all, %.0f %% of detected), top-3 %d (%.0f %%), shown correct %d, %d errors'
          % (grp, n, d, 100.0 * d / max(n, 1), o1, 100.0 * o1 / max(n, 1), 100.0 * o1 / max(d, 1), o3, 100.0 * o3 / max(n, 1), so, len(E)))
        by = {}
        for r in R:
            by.setdefault(r['class'], []).append(r)
        p('  %-11s %4s %4s %4s %4s  %s' % ('class', 'n', 'det', 'top1', 'top3', 'wrong top-1 (count)'))
        for c in sorted(by):
            rs = by[c]
            wrong = {}
            for r in rs:
                if r.get('detected') and not r.get('ok1'):
                    wrong[r['top1']] = wrong.get(r['top1'], 0) + 1
            p('  %-11s %4d %4d %4d %4d  %s' % (c, len(rs), sum(1 for r in rs if r.get('detected')), sum(1 for r in rs if r.get('ok1')),
                                               sum(1 for r in rs if r.get('ok3')), ', '.join('%s %d' % kv for kv in sorted(wrong.items(), key=lambda kv: -kv[1]))))
    ped = [r['pedestal_db'] for r in rows if 'pedestal_db' in r and r.get('rate', 3e6) < 2e6 and r['group'] != 'disc_audio']
    if ped:
        p()
        p('noise-fill pedestal (quiet level inside the recording band minus outside): median %.1f dB, |x| > 3 dB in %d of %d; '
          'wide extra tracks in %d entries' % (np.median(ped), sum(abs(x) > 3 for x in ped), len(ped), sum(1 for r in rows if r.get('wide_extra'))))
    with open(out[:-4] + '.txt', 'w') as fh:
        fh.write('\n'.join(lines) + '\n')


def main():
    global iqlib
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--library', default=os.environ.get('KRAKEN_IQ_LIBRARY') or os.path.expanduser('~/krakensdr_iq_library'))
    ap.add_argument('--model', default=os.path.join(REPO, 'sigclass.model'))
    ap.add_argument('--work', default=default_work())
    ap.add_argument('--out', default='')
    ap.add_argument('--seconds', type=float, default=4.0)
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--classes', default='')
    ap.add_argument('--ids', default='')
    o = ap.parse_args()
    sys.path.insert(0, os.path.join(o.library, '_tools'))
    import iqlib as _iq
    iqlib = _iq
    o.out = o.out or os.path.join(o.work, 'eval_real.tsv')
    meta, _, _ = model_io.read_model(o.model)
    thr = float(meta.get('min_confidence', 0.0)) if isinstance(meta, dict) else 0.0
    known = {c['id'] for c in load_classes()}
    ents = iqlib.load_index(os.path.join(o.library, 'index.json'))['entries']
    sel = [e for e in ents if (not o.classes or e['signals'][0] in o.classes.split(',')) and (not o.ids or o.ids in e['id'])]
    with Pool(o.jobs) as pool:
        rows = [r for rs in pool.imap_unordered(one, [(e, o, known, thr) for e in sel]) for r in rs]
    rows.sort(key=lambda r: (r['group'], r['class'], r['id']))
    keys = ['group', 'class', 'id', 'detected', 'top1', 'p1', 'ok1', 'ok3', 'shown_ok', 'top3', 'hits', 'view', 'det_off', 'det_bw',
            'tracks', 'shown_tracks', 'wide_extra', 'pedestal_db', 'rate', 'snaps', 'error']
    with open(o.out, 'w') as fh:
        fh.write('\t'.join(keys) + '\n')
        for r in rows:
            fh.write('\t'.join(str(r.get(k, '')) for k in keys) + '\n')
    summarise(rows, thr, o.out)
    print('table -> %s' % o.out)


if __name__ == '__main__':
    main()
