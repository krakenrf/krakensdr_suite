#!/usr/bin/env python3
"""Signal classifier: the model on REAL recordings, as the receiver would
see them.

  eval_real.py [--library DIR] [--model M] [--work DIR] [--seconds 4]
               [--jobs 5] [--classes a,b] [--ids SUBSTR] [--out TABLE.tsv]
               [--rf-mask on|off] [--max-windows 12]
  eval_real.py --selfcheck [--work DIR] [--bank DIR]   (the feed path on synthetic captures)

Library: --library, else $KRAKEN_IQ_LIBRARY, else ~/krakensdr_iq_library
(read-only: its _tools/iqlib.py reads the entries - one antenna of a
multi-channel entry, by its channel_layout; nothing is copied).

What is scored (per expect row; an entry without rows: its first signal
anywhere in its band):
  - a row is scored only while it is on air (the row's `on_air`):
    "continuous" (or no field) - always; "intermittent" with measured
    `on_air_s` intervals - only in a window that holds >= 2 s of its on-air
    time (4 of the 8 snapshots of a 4 s window: the tracker's evidence then
    rests mostly on looks with the signal present) or one of its
    transmissions whole (a burst or packet is seen complete in one
    snapshot); "intermittent" without intervals - not scored, reported as
    "not known on air"
  - windows: an entry with intermittent rows gets several --seconds windows
    chosen greedily from the intervals (each adds the most rows not yet on
    air in an earlier one, at most --max-windows); every row is scored once,
    in the chosen window where it is on air longest (continuous rows in the
    first); other entries one window: the busiest stretch, after skipping an
    opening run of exact zeros (a recorder's silence)
  - a row's track: among the tracks overlapping the row's band (centre +-
    max(bw, 6 kHz) / 2) only plausible widths count - a band-wide row (>= 2
    MHz) takes only tracks >= 25 % of its width (a spur inside is not the
    row's track), a narrower one tracks <= max(4 x bw, 30 kHz) first; of those
    only the tracks within STRONG_DB (10 dB) of the strongest compete (a row
    describes the dominant signal in its band: a faint wide receiver hump must
    not beat the signal's own track; strength = the highest in-band SNR of the
    detections in the track's band); then the best band overlap (intersection
    / union - not the most hits: a band-edge spur with more hits beat the
    right track), then hits. A row without an offset: the most hits, then
    overlap, inside the recording's band. (`iou_pick`: the overlap-first pick,
    for comparison)
  - right = the row's class (a merged class as its target) or any of its
    `classes` (one channel alternating two systems); family = the coarse
    modulation family (classes.json); sure = p >= the class's min_p, else
    the model's min_confidence; drawn = p >= 0.5 and not none

How each entry is fed (the feed may remove what the RECORDING device added -
its band edge, its DC line, its squelch gaps - never clean the signal):
  - native (own captures in the receiver's format, cu8 / heimdall at 2.4
    MS/s): the bytes as they are, antenna 0
  - IQ at >= 2 MS/s: resampled to 2.4 MHz (wider ones lose what lies beyond
    +-1.2 MHz), the gap to 2.4 MHz filled with noise at its own density
  - IQ below 2 MS/s: squelch gaps (exact-zero runs >= 1 ms) get noise at the
    recording's quiet level; the recording receiver's DC line (classes
    without a carrier) is attenuated to the floor beside it (no noise is
    injected: silent stretches stay silent); resampled to 2.4 MHz and
    placed +300 kHz off the centre; then BAND-LIMITED: per row the signal
    band (half = min(row bw, the class's widest occupied bw) / 2, at least
    the class's narrowest / 2) plus a margin (crop = max(1.8 x half, half + 2
    kHz)), clipped to the recording's passband (the bins whose quiet floor is
    within 10 dB of the floor around the signal: an IF-filtered recording
    keeps only its passband), is kept; everything else in the 2.4 MHz band is
    white noise at the level just beside the signal (the passband floor
    between the signal and the crop edge). So the recording's own floor shape
    and band edges are not presented as signals (a plateau ~1 dB over a
    white fill was detected as a 70-100 kHz signal). Rows without an offset:
    the whole passband is kept, filled at its quiet level
  - discriminator audio (disc_audio): FM re-modulated (peak deviation by
    class), noise for 25 dB in 12.5 kHz - a weaker test, reported apart
  then 8 bits at a realistic level (noise ~6 LSB rms, peaks <= ~120 LSB,
  >= 0.8 LSB of receiver noise), `sigclass_tool classify --track` over the
  0.5 s snapshots; the tracks after the last snapshot are scored.

Groups, per ROW from what the library states (no text match on a whole
entry): native (own captures in the receiver's format); open (a class the
model has no class for, or "unknown": right when labelled unk_* / none /
below min_confidence); disc_audio; confirmed (rtl_433's test corpus, or the
entry's `verified` reports a positive decoder count and the row's own note
does not say it is not decoded / unverified / undetermined); probe (no
decoder run, only the format / spectrum checked); unconfirmed (the rest);
undetermined (the row's `truth: "undetermined"` - the library could not
establish its class from the recording: listed, never scored; an entry
with only such rows is not fed at all).

False labels: every drawn track that overlaps no row of its entry (any row
with an offset, on air or not, known class or not), per capture, sure /
unsure; single-service captures, busy sites (>= 5 rows with offsets: their
unidentified channels are mostly real transmitters) and captures the library
marks as dominated by receiver intermodulation (real RF, not transmitters)
on separate lines.

Output: the table (TSV, one row per expected signal, scored or not, with the
reason), and TABLE.txt: per group and class (exact, family, shown), the
trust table per group, the false labels, run time. numpy + scipy only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import zlib
from fractions import Fraction
from multiprocessing import Pool

import numpy as np
from scipy.signal import resample_poly, welch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bank import default_work, load_classes  # noqa: E402
import model_io  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
# (KRAKEN_SIGCLASS_TOOL: another build - an older layout's tool to compare models on the same path)
TOOL = os.environ.get('KRAKEN_SIGCLASS_TOOL') or os.path.join(REPO, 'tools', 'build', 'sigclass_tool')
FS = 2400000.0
SNAP_S = 0.5
WIN_ON_S = 2.0            # an intermittent row needs this much on-air time in a window (or a whole transmission)
DEV = {'dstar': 1200, 'dmr': 1944, 'p25p1': 1800, 'nxdn': 2400, 'ysf': 2700, 'pocsag': 4500, 'flex': 4800, 'aprs': 3000,
       'mpt1327': 2500, 'nfm_voice': 3000, 'v23': 3500}
# the entry's verified text reports a positive decoder count ("1392 valid frames", "7 packets", "8 messages")
POSITIVE = re.compile(r'\b([1-9][\d,]*)\s+(?:valid\s+)?(?:frames?|packets?|messages?|decodes?|grants?|interrogations)\b'
                      # ... or a decoded physical-layer fact: a DTV mode, a cell's PCI, a decoded tone / burst
                      r'|\b(?:DVB-T2?|ISDB-T|DTMB|ATSC(?: 3\.0)?) (?:\d+k|mode \d)|\bPCI \d+|\bCTCSS [\d.]+ Hz|\b(?:one|\d+) [\w-]+ bursts? decoded', re.I)
# ... a row's own note says it is not decoded
ROW_NEG = re.compile(r'not decoded|not verified|unverified|undetermined|not identified|no sync|not confirmed|unconfirmed|not dmr|not p25', re.I)
NO_RUN = re.compile(r'no decoder run|spectrum probe|spectrum check only|format / spectrum|only the format|file sizes and format only|\bnot run\b', re.I)
iqlib = None
CLS = {c['id']: c for c in load_classes()}
MERGED = {c['id']: c['merged_into'] for c in CLS.values() if c.get('merged_into')}
MIN_P = {}          # the model's per-class sure thresholds (meta classes min_p)


def coarse_family(fam):
    """classes.json's family (the modulation) -> a coarse group for the
    family-level score: fsk, psk, ofdm, vsb, pulse, am, fm, css, noise, none"""
    f = re.sub(r'\s*\(.*\)', '', fam or '')
    if f in ('none', 'noise', 'CSS'):
        return f.lower()
    if 'OFDM' in f:
        return 'ofdm'
    if 'VSB' in f:
        return 'vsb'
    if f in ('pulsed', 'OOK', 'pulsed CW'):
        return 'pulse'
    if 'AM' in f or f in ('unmodulated', 'SSB'):
        return 'am'
    if 'FM' in f and 'FSK' not in f:
        return 'fm'
    if 'FSK' in f or 'GMSK' in f:
        return 'fsk'
    if 'PSK' in f or 'QAM' in f:
        return 'psk'
    return f.lower()


FAMILY = {c['id']: coarse_family(c.get('family', '')) for c in CLS.values()}
# classes whose spectrum has no carrier line at the centre: a line there in a
# narrow recording is the recording receiver's DC spike (notch_dc)
NO_CARRIER = {'dstar', 'dmr', 'p25p1', 'p25p2', 'nxdn', 'ysf', 'tetra', 'pocsag', 'flex', 'ais', 'vdl2', 'radiosonde', 'lora',
              'ism_fsk', 'flarm', 'uat', 'dab', 'dvbt', 'dtmb', 'atsc3', 'lte', 'nr'}


# ------------------------------------------------------------------ groups, on air
def is_native(e):
    return e.get('provenance') == 'own-capture' and e['format'] in ('cu8', 'heimdall8091') and abs((e.get('sample_rate_hz') or 0) - FS) < 1


def entry_decoded(e):
    return 'rtl_433_tests' in e['id'] or 'rtl_433' in e['path'] or bool(POSITIVE.search(str(e.get('verified') or '')))


def group_of(e, x, known):
    if x.get('truth') == 'undetermined':
        return 'undetermined'       # the library could not establish the row's class: in no score
    if is_native(e):
        return 'native'
    if x['class'] not in known:
        return 'open'
    if e['format'] == 'disc_audio':
        return 'disc_audio'
    dec = entry_decoded(e)
    if dec and not ROW_NEG.search(str(x.get('note') or '')):
        return 'confirmed'
    if not dec and NO_RUN.search(str(e.get('verified') or '')):
        return 'probe'
    return 'unconfirmed'


def site_of(e, exp):
    """single / busy (>= 5 rows with offsets: its unidentified channels are mostly real transmitters) /
    im (the library notes receiver intermodulation products in it)"""
    if re.search(r'intermodulation', str(e.get('notes') or ''), re.I) and re.search(r'products', str(e.get('notes') or ''), re.I):
        return 'im'
    return 'busy' if sum(1 for x in exp if x.get('offset_hz') is not None) >= 5 else 'single'


def on_time(x, t0, t1):
    """seconds of row x's on-air intervals inside [t0, t1], and whether one interval lies wholly inside"""
    tot, whole = 0.0, False
    for a, b in x.get('on_air_s') or []:
        tot += max(0.0, min(b, t1) - max(a, t0))
        whole = whole or (a >= t0 and b <= t1)
    return tot, whole


def on_in(x, t0, t1):
    tot, whole = on_time(x, t0, t1)
    return tot >= WIN_ON_S or whole


def air_kind(x):
    if x.get('on_air') != 'intermittent':
        return 'continuous'
    return 'intervals' if x.get('on_air_s') else 'unknown'


def plan_windows(e, exp, seconds, max_windows):
    """-> [(t0, [row indices scored there])], {row index: reason not scored}"""
    try:
        dur = float(iqlib.duration_s(e) or e.get('duration_s') or seconds)
    except Exception:  # noqa: BLE001
        dur = float(e.get('duration_s') or seconds)
    kinds = [air_kind(x) for x in exp]
    why = {i: 'not known on air (intermittent, no intervals)' for i, k in enumerate(kinds) if k == 'unknown'}
    inter = [i for i, k in enumerate(kinds) if k == 'intervals']
    cont = [i for i, k in enumerate(kinds) if k == 'continuous']
    t_first = start_of(e, seconds)          # the entry's usual window: the continuous rows are scored there
    if not inter:
        return [(t_first, cont)], why
    last = max(0.0, dur - seconds)
    cands = np.unique(np.clip(np.round(np.concatenate([[0.0]] + [[a - 0.25, b - seconds + 0.25, a + 0.5 * (b - a) - 0.5 * seconds]
                                                                   for i in inter for a, b in exp[i]['on_air_s']]) / SNAP_S) * SNAP_S,
                                      0.0, last))
    chosen = [t_first]
    covered = {i for i in inter if on_in(exp[i], t_first, t_first + seconds)}
    while len(chosen) < max_windows:
        best = None
        for t in cands:
            new = [i for i in inter if i not in covered and on_in(exp[i], t, t + seconds)]
            if not new:
                continue
            key = (len(new), sum(on_time(exp[i], t, t + seconds)[0] for i in new))
            if best is None or key > best[0]:
                best = (key, t, new)
        if best is None:
            break
        chosen.append(float(best[1]))
        covered.update(best[2])
    for i in inter:
        if i not in covered:
            why[i] = 'not on air >= %.0f s (or one whole transmission) in any window' % WIN_ON_S
    plan = {t: [] for t in chosen}
    for i in covered:
        t = max(chosen, key=lambda t: (on_in(exp[i], t, t + seconds), on_time(exp[i], t, t + seconds)[0]))
        plan[t].append(i)
    plan[t_first] += cont
    # (a window without rows still runs when nothing else does: its false labels count)
    return [(t, sorted(plan[t])) for t in sorted(chosen) if plan[t]] or [(t_first, [])], why


def zero_opening_s(e, scan_s=60.0):
    """length of the run of exact zeros the recording opens with (a recorder's silence), s"""
    try:
        x = iqlib.open_entry(e, offset_s=0.0, seconds=scan_s, channel=0)[0]
    except Exception:  # noqa: BLE001
        return 0.0
    nz = np.flatnonzero(np.abs(x) > 0)
    rate = float(e.get('sample_rate_hz') or 1.0)
    if not len(nz):
        return 0.0
    s = nz[0] / rate
    return float(np.floor(s / SNAP_S) * SNAP_S) if s >= 0.1 else 0.0


def start_of(e, seconds):
    if is_native(e) or e['format'] == 'heimdall8091':
        return busiest_start(e, seconds)
    z0 = zero_opening_s(e)
    return max(z0, busiest_start(e, seconds, first=z0))


def busiest_start(e, seconds, scan_s=120.0, step_s=0.5, probe_s=0.02, first=0.0):
    """start (s) of the --seconds window with the most power in the first
    scan_s of the recording from `first` (a recording that opens with
    silence, a gated one: the signal is there later). Probes probe_s every
    step_s."""
    try:
        dur = iqlib.duration_s(e)
    except Exception:   # noqa: BLE001 - unknown length: from the start
        return first
    if not dur or dur <= first + seconds + step_s or e['format'] == 'heimdall8091':
        return first
    t = np.arange(first, min(dur - probe_s, first + scan_s), step_s)
    pw = []
    for s in t:
        x = iqlib.open_entry(e, offset_s=float(s), seconds=probe_s, channel=0)[0]
        x = np.asarray(x, np.complex64)
        pw.append(float(np.mean(np.abs(x - x.mean()) ** 2)) if len(x) else 0.0)
    pw = np.array(pw)
    w = max(1, int(round(seconds / step_s)))
    n = len(pw) - w + 1
    if n <= 1:
        return first
    c = np.convolve(pw, np.ones(w), mode='valid')[:n]
    k = int(np.argmax(c))
    return float(t[k]) if c[k] > 1.5 * c[0] else first     # from the start unless clearly busier later


# ------------------------------------------------------------------ the feed
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
    # the quiet level: a low percentile over the recording's band without its filter's roll-off
    mid = np.abs(f) < 0.42 * rate
    n0 = float(np.percentile(fl[mid], 10))
    return f, fl, n0


def zero_runs(x, rate, min_s=1e-3):
    """samples in runs of exact zeros >= min_s long (a gated recording's silence)"""
    z = (x.real == 0) & (x.imag == 0)
    out = np.zeros(len(x), bool)
    if not z.any():
        return out
    d = np.diff(np.concatenate([[0], z.astype(np.int8), [0]]))
    a, b = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
    m = int(min_s * rate)
    for s, e in zip(a, b):
        if e - s >= m:
            out[s:e] = True
    return out


def notch_dc(x, rate, floor_density=None, half=100.0):
    """the recording receiver's own DC line (within +-100 Hz of the recording's
    centre, >= 10 dB over the spectrum beside it) attenuated to that level -
    the bins are scaled, no noise is added (a silent stretch stays silent).
    The level is the lower of the ring beside the line and the recording's
    floor (for a narrow signal the ring is in-band). Only for classes without
    a carrier (NO_CARRIER). -> (x, line dB or 0)"""
    N = len(x)
    X = np.fft.fft(x)
    f = np.fft.fftfreq(N, 1 / rate)
    a = np.abs(f)
    inner = a <= half
    ring = (a > 2 * half) & (a <= 6 * half)
    if not inner.any() or not ring.any():
        return x, 0.0
    p = np.abs(X) ** 2
    ref = float(np.median(p[ring]))
    if floor_density:
        ref = min(ref, float(floor_density) * rate * N)
    pk = float(p[inner].max())
    if ref <= 0 or pk < 10.0 * ref:
        return x, 0.0
    X[inner] *= np.sqrt(ref / np.maximum(p[inner], ref))
    return np.fft.ifft(X).astype(np.complex64), 10 * np.log10(pk / ref)


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


def signal_half(x, rate):
    """half the band a row's signal occupies (Hz): min(row bw, class's widest occupied bw) / 2, at least the
    class's narrowest / 2 and 1 kHz"""
    cls = MERGED.get(x['class'], x['class'])
    occ = CLS.get(cls, {}).get('occ_bw_hz')
    bw = float(x.get('bw_hz') or 0) or float('inf')
    hi = float(occ[1]) if occ else float('inf')
    h = 0.5 * min(bw, hi)
    if not np.isfinite(h):
        h = 0.45 * rate
    return max(h, 0.5 * float(occ[0]) if occ else 0.0, 1000.0)


def band_limited(y, rate, shift, rows, f, fl, rng):
    """keep each row's signal band + margin (inside the recording's passband), white noise at the level just
    beside the signal everywhere else. -> (y, n0, crops [(lo, hi) Hz from the recording's centre])"""
    keep_f = np.zeros(len(f), bool)
    ring_vals, pls, crops = [], [], []
    for x in rows:
        off = x.get('offset_hz')
        if off is None:
            mid = np.abs(f) < 0.42 * rate
            pl = float(np.median(fl[mid]))
            pas = mid & (fl >= pl / 10)
            keep_f |= pas
            ring_vals += list(np.percentile(fl[pas], 10, keepdims=True)) if pas.any() else []
            pls.append(pl)
            crops.append((float(f[pas].min()) if pas.any() else 0.0, float(f[pas].max()) if pas.any() else 0.0))
            continue
        off = float(off)
        h = signal_half(x, rate)
        crop = max(1.8 * h, h + 2000.0)
        near = np.abs(f - off) <= crop
        if not near.any():
            continue
        pl = float(np.median(fl[near]))
        pas = (fl >= pl / 10) & (np.abs(f) < 0.47 * rate)
        pls.append(pl)
        # the contiguous passband around the signal, within the crop
        i0 = int(np.argmin(np.abs(f - off)))
        lo = hi = i0
        while lo - 1 >= 0 and pas[lo - 1] and abs(f[lo - 1] - off) <= crop:
            lo -= 1
        while hi + 1 < len(f) and pas[hi + 1] and abs(f[hi + 1] - off) <= crop:
            hi += 1
        keep_f[lo:hi + 1] = True
        crops.append((float(f[lo]), float(f[hi])))
        ring = pas & (np.abs(f - off) > h) & (np.abs(f - off) <= crop)
        ring[:lo] = False
        ring[hi + 1:] = False
        ring_vals += list(fl[ring])
    if not keep_f.any():
        return y, None, []
    L = float(np.median(ring_vals)) if ring_vals else float(np.median(pls))
    N = len(y)
    fb = np.fft.fftfreq(N, 1 / FS)
    df = f[1] - f[0]
    idx = np.clip(np.round((fb - shift - f[0]) / df).astype(int), 0, len(f) - 1)
    keep = keep_f[idx] & (np.abs(fb - shift) <= 0.5 * rate)
    Y = np.fft.fft(y)
    k = int((~keep).sum())
    Y[~keep] = (rng.standard_normal(k) + 1j * rng.standard_normal(k)) * np.sqrt(L * FS * N / 2)
    return np.fft.ifft(Y).astype(np.complex64), L, crops


def build(e, t0, seconds, rows, rng):
    """-> 2.4 MHz complex samples (LSB units, before quantisation) or native bytes, placement shift, info,
    rate, n0"""
    x, rate, centre = iqlib.open_entry(e, offset_s=t0, seconds=seconds, channel=0)
    x = np.asarray(x, np.complex64)
    cls = rows[0]['class'] if rows else e['signals'][0]
    info = {'start_s': t0}
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
    gaps = zero_runs(x, rate)
    live = ~gaps
    if not live.any():
        raise ValueError('silent: no non-zero samples in the window')
    x = x - x[live].mean()
    f, fl, n0 = noise_floor_shape(x[live][:min(int(live.sum()), 1 << 22)], rate)
    if gaps.any():
        # a squelch-gated recording (its quiet stretches written as exact zeros): the receiver's noise goes
        # on there - white noise at the recording's own quiet level
        k = int(gaps.sum())
        x[gaps] = ((rng.standard_normal(k) + 1j * rng.standard_normal(k)) * np.sqrt(n0 * rate / 2)).astype(np.complex64)
        info['gated'] = round(float(k) / len(x), 3)
    if any(MERGED.get(r['class'], r['class']) in NO_CARRIER for r in rows):
        x, dc = notch_dc(x, rate, floor_density=n0)
        if dc:
            info['dc_notch_db'] = round(dc, 1)
    if rate >= 2e6:
        y = resample(x, rate, FS)[0] if abs(rate - FS) > 1 else x
        f2, fl2, n02 = noise_floor_shape(y[:min(len(y), 4 << 20)], FS)
        if rate < FS:
            y = fill_noise(y, rate, n02, 0.0, rng)
        info['pedestal_db'] = 0.0
        return y, 0.0, info, rate, n02
    # narrow: resampled, placed off the centre, band-limited to the rows' signals, the rest filled
    y, r2 = resample(x, rate, FS)
    shift = 300e3 if rate <= 1.2e6 else max(0.0, (0.94 * FS - rate) / 2)
    if shift < 20e3:
        shift = 20e3        # off DC at least
    y = y * np.exp(2j * np.pi * shift * np.arange(len(y)) / FS).astype(np.complex64)
    if any(r.get('offset_hz') is not None for r in rows):
        z, L, crops = band_limited(y, rate, shift, [r for r in rows if r.get('offset_hz') is not None], f, fl, rng)
    else:
        z, L, crops = y, None, []
    if L is None:
        # no row says where the signal is (rtl_433 corpus, entries without rows): it cannot be band-limited -
        # the whole recording, the rest of the band filled at its quiet level, refilled once at the
        # passband's level when the step between them exceeds 2 dB
        L = n0
        z = fill_noise(y, rate, L, shift, rng, f, fl)
        ped = pedestal(z, rate, shift)
        if abs(ped) > 2.0:
            L *= 10 ** (ped / 10)
            z = fill_noise(y, rate, L, shift, rng, f, fl)
    info['crop'] = ' '.join('%+.0f..%+.0f' % c for c in crops)
    info['fill_vs_floor_db'] = round(10 * np.log10(L / n0), 1) if n0 > 0 else ''
    info['pedestal_db'] = pedestal(z, rate, shift)
    return z, shift, info, rate, L


def pedestal(y, rate, shift):
    """quiet level inside the recording's band minus outside it (dB)"""
    f, p = welch(y[:1 << 21], fs=FS, nperseg=4096, return_onesided=False, detrend=False)
    inside = np.abs(f - shift) < 0.35 * rate
    outside = (np.abs(f - shift) > 0.6 * rate) & (np.abs(f) < 0.45 * FS)
    if not inside.any() or not outside.any():
        return 0.0
    return round(float(10 * np.log10(np.percentile(p[inside], 20) / np.percentile(p[outside], 20))), 1)


def quantise(y, n0, rng=None):
    """noise ~6 LSB rms per I / Q over the band, peaks <= ~120 LSB; a recording
    whose SNR is more than 8 bits hold gets the receiver noise of a real one
    (>= 0.8 LSB rms: below that the 8 bits would gate the noise with the
    signal - a real receiver's own noise never vanishes)"""
    g = np.sqrt(2 * 36.0 / max(n0 * FS, 1e-30))
    pk = float(np.percentile(np.abs(np.concatenate([y.real, y.imag])), 99.99)) * g
    if pk > 120:
        g *= 120 / pk
    v = np.empty(2 * len(y), np.float32)
    v[0::2] = y.real * g
    v[1::2] = y.imag * g
    sn = np.sqrt(max(n0 * FS, 0.0) / 2) * g      # the noise rms per I / Q now
    if sn < 0.8:
        r = rng if rng is not None else np.random.default_rng(0)
        v += (r.standard_normal(len(v)) * np.sqrt(0.64 - sn * sn)).astype(np.float32)
    return np.clip(np.floor(v + 128.0), 0, 255).astype(np.uint8), g


# ------------------------------------------------------------------ scoring
def row_band(x, shift, in_band):
    if x.get('offset_hz') is None:
        return shift - 0.5 * min(in_band, FS), shift + 0.5 * min(in_band, FS), 0.0
    c = shift + float(x['offset_hz'])
    half = 0.5 * max(float(x.get('bw_hz') or 0), 6e3)
    return c - half, c + half, float(x.get('bw_hz') or 0)


STRONG_DB = 10.0      # a track this much weaker than the strongest one at a row is not the row's dominant signal


def track_strength(lines):
    """per final track: the highest in-band SNR (dB) of the detections, in any snapshot, whose centre lies in the
    track's band - how strong the signal the track stands for is"""
    out = {}
    for t in lines[-1].get('tracks', []):
        lo, hi = t['centre_hz'] - 0.5 * t['bw_hz'], t['centre_hz'] + 0.5 * t['bw_hz']
        s = [d.get('snr_db', 0.0) for L in lines for d in L.get('detections', []) if lo <= d['offset_hz'] <= hi]
        out[t['id']] = max(s) if s else 0.0
    return out


def match(x, tracks, shift, in_band, strength=None, rule='strength'):
    """the row's track (see the module docstring). rule 'strength': among the tracks of plausible width, those
    within STRONG_DB of the strongest compete on band overlap (a row describes the dominant signal in its band - a
    faint wide receiver hump must not beat the signal's own track); rule 'iou': overlap first (the rule before)"""
    lo, hi, ebw = row_band(x, shift, in_band)
    cands = []
    for t in tracks:
        tl, th = t['centre_hz'] - 0.5 * t['bw_hz'], t['centre_hz'] + 0.5 * t['bw_hz']
        ov = min(th, hi) - max(tl, lo)
        if ov <= 0:
            continue
        if x.get('offset_hz') is None:
            cands.append(((1, t['hits'], ov), t))
            continue
        if ebw >= 2e6 and t['bw_hz'] < 0.25 * min(ebw, 0.94 * FS):
            continue
        fit = 1 if (ebw <= 0 or t['bw_hz'] <= max(4.0 * ebw, 30e3)) else 0
        iou = ov / (max(th, hi) - min(tl, lo))
        cands.append(((fit, round(iou, 3), t['hits']), t))
    if not cands:
        return None
    if rule == 'strength' and strength is not None and x.get('offset_hz') is not None:
        top_fit = max(k[0] for k, _ in cands)
        cands = [c for c in cands if c[0][0] == top_fit]
        smax = max(strength.get(t['id'], 0.0) for _, t in cands)
        cands = [c for c in cands if strength.get(c[1]['id'], 0.0) >= smax - STRONG_DB]
    return max(cands, key=lambda c: c[0])[1]


def sure_of(top):
    return bool(top) and top[0]['id'] != 'none' and top[0]['p'] >= MIN_P.get(top[0]['id'], THR[0])


THR = [0.0]


def run_tool(u8, eid, rf, o):
    tmp = '/dev/shm/evalreal_%d_%s.cu8' % (os.getpid(), re.sub(r'[^A-Za-z0-9]', '_', eid)[-60:])
    u8.tofile(tmp)
    cmd = [TOOL, 'classify', '--model', o.model, '--track', '--rate', '2400000', '--format', 'cu8', tmp]
    if rf > 0:
        cmd += ['--rf', '%.0f' % rf]
    r = subprocess.run(cmd, capture_output=True, text=True)
    os.remove(tmp)
    lines = [json.loads(l) for l in r.stdout.splitlines() if l.startswith('{')]
    if not lines:
        raise RuntimeError('classify: ' + r.stderr[-200:])
    return lines


def one(args):
    e, o, known, thr = args
    THR[0] = thr
    rng = np.random.default_rng(zlib.crc32(e['id'].encode()))
    exp = [x for x in (e.get('expect') or []) if x.get('class')] or [{'class': e['signals'][0], 'offset_hz': None, 'bw_hz': None}]
    site = site_of(e, exp)
    base = {'id': e['id'], 'site': site}
    if all(x.get('truth') == 'undetermined' for x in exp):
        return [dict(base, group='undetermined', **{'class': x['class'], 'row': i, 'scored': False, 'on_air': air_kind(x),
                                                   'why_not': 'truth undetermined'}) for i, x in enumerate(exp)]
    try:
        plan, why = plan_windows(e, exp, o.seconds, o.max_windows)
    except Exception as ex:  # noqa: BLE001
        return [dict(base, group=group_of(e, x, known), **{'class': x['class'], 'row': i, 'error': 'plan: %s' % str(ex)[:100]}) for i, x in enumerate(exp)]
    rows, false = [], {}
    done = set()
    for t0, idx in plan:
        try:
            y, shift, info, rate, n0 = build(e, t0, o.seconds, [x for x in exp if x.get('offset_hz') is not None or len(exp) == 1] or exp, rng)
            u8, g = (info['native'], 1.0) if info.get('native') is not None else quantise(y, n0, rng)
            rf = float(e['center_hz']) - shift if o.rf_mask == 'on' and e.get('center_hz') else 0.0
            lines = run_tool(u8, e['id'], rf, o)
        except Exception as ex:  # noqa: BLE001 - an unreadable entry is reported, not fatal
            for i in idx:
                rows.append(dict(base, group=group_of(e, exp[i], known), **{'class': exp[i]['class'], 'row': i, 'window_s': t0, 'error': str(ex)[:120]}))
                done.add(i)
            continue
        tracks = lines[-1].get('tracks', [])
        strength = track_strength(lines)
        in_band = rate if rate < 2e6 else FS
        # false labels: drawn tracks overlapping no row of the entry
        bands = [row_band(x, shift, in_band)[:2] for x in exp if x.get('offset_hz') is not None]
        for t in tracks:
            top = t['top']
            if not top or top[0]['id'] == 'none' or top[0]['p'] < 0.5:
                continue
            tl, th = t['centre_hz'] - 0.5 * t['bw_hz'], t['centre_hz'] + 0.5 * t['bw_hz']
            if any(min(th, b) - max(tl, a) > 0 for a, b in bands):
                continue
            if len(bands) == 0 and len(exp) == 1 and exp[0].get('offset_hz') is None:
                continue        # the whole band is the row's
            key = (top[0]['id'], int(round((t['centre_hz'] - shift) / 5e3)))
            if key not in false or t['hits'] > false[key][3]:
                false[key] = (top[0]['id'], top[0]['p'], sure_of(top), t['hits'], t['centre_hz'] - shift)
        for i in idx:
            x = exp[i]
            done.add(i)
            grp = group_of(e, x, known)
            if grp == 'undetermined':
                rows.append(dict(base, group=grp, **{'class': x['class'], 'row': i, 'window_s': t0, 'scored': False,
                                                     'on_air': air_kind(x), 'why_not': 'truth undetermined'}))
                continue
            row = dict(base, group=grp, **{'class': x['class'], 'row': i, 'window_s': t0, 'scored': True, 'on_air': air_kind(x),
                                           'on_s': round(on_time(x, t0, t0 + o.seconds)[0], 1) if air_kind(x) == 'intervals' else '',
                                           'rate': rate, 'snaps': len(lines), 'pedestal_db': info.get('pedestal_db', ''),
                                           'rf': round(rf) if rf > 0 else '', 'gated': info.get('gated', ''),
                                           'dc_notch_db': info.get('dc_notch_db', ''), 'crop': info.get('crop', ''),
                                           'fill_db': info.get('fill_vs_floor_db', ''), 'tracks': len(tracks), 'gain': round(float(g), 4)})
            if x.get('offset_hz') is not None and (row_band(x, shift, in_band)[1] < -0.47 * FS or row_band(x, shift, in_band)[0] > 0.47 * FS):
                row.update(scored=False, why_not='outside the 2.4 MHz band')
                rows.append(row)
                continue
            t = match(x, tracks, shift, in_band, strength)
            t_iou = match(x, tracks, shift, in_band, strength, rule='iou')
            if t_iou is not None:
                row['iou_pick'] = '%s:%.0f:%.0f' % (t_iou['top'][0]['id'] if t_iou['top'] else '', t_iou['centre_hz'] - shift, t_iou['bw_hz'])
            if t is None:
                row['detected'] = False
            else:
                top = t['top']
                allowed = {MERGED.get(c, c) for c in ([x['class']] + list(x.get('classes') or []))}
                row.update({'detected': True, 'top1': top[0]['id'] if top else '', 'p1': top[0]['p'] if top else 0.0,
                            'top3': [q['id'] for q in top], 'hits': t['hits'], 'det_bw': round(t['bw_hz']),
                            'det_off': round(t['centre_hz'] - shift), 'view': t['view'], 'sure': sure_of(top)})
                if grp == 'open' or x['class'] not in known:
                    row['ok1'] = (not top) or top[0]['id'].startswith('unk_') or top[0]['id'] == 'none' or top[0]['p'] < thr
                else:
                    row['ok1'] = bool(top) and top[0]['id'] in allowed
                    row['ok3'] = bool(top) and bool(allowed & set(row['top3']))
                    row['okf'] = bool(top) and FAMILY.get(top[0]['id']) in {FAMILY.get(a) for a in allowed}
                    row['shown_ok'] = bool(top) and row['ok1'] and top[0]['p'] >= thr
            row['shown_tracks'] = sum(1 for q in tracks if q['top'] and q['top'][0]['id'] != 'none' and q['top'][0]['p'] >= thr)
            rows.append(row)
    for i, x in enumerate(exp):
        if i not in done:
            rows.append(dict(base, group=group_of(e, x, known), **{'class': x['class'], 'row': i, 'scored': False, 'on_air': air_kind(x),
                                                                  'why_not': why.get(i, 'not scored')}))
    rows.sort(key=lambda r: r['row'])
    if rows:
        rows[0]['false_tracks'] = ';'.join('%s:%.2f:%s:%d:%.0f' % (c, p, 'sure' if s else 'unsure', h, off) for c, p, s, h, off in false.values())
    return rows


# ------------------------------------------------------------------ summary
def summarise(rows, thr, out, t_run):
    lines = []

    def p(s=''):
        lines.append(s)
        print(s)
    p('min_confidence %.2f; per-class min_p for %d classes; run time %.0f s' % (thr, len(MIN_P), t_run))
    nu = sum(1 for r in rows if r['group'] == 'undetermined')
    if nu:
        p('undetermined truth (the library could not establish the class; not scored): %d rows' % nu)
    for grp in ('native', 'confirmed', 'probe', 'unconfirmed', 'disc_audio', 'open'):
        G = [r for r in rows if r['group'] == grp]
        if not G:
            continue
        R = [r for r in G if r.get('scored') and 'error' not in r]
        E = [r for r in G if 'error' in r]
        ns = {}
        for r in G:
            if not r.get('scored') and 'error' not in r:
                ns[r.get('why_not', '?')] = ns.get(r.get('why_not', '?'), 0) + 1
        p()
        if grp == 'open':
            det = [r for r in R if r.get('detected')]
            ok = sum(1 for r in det if r['ok1'])
            p('== open set (no class for these): %d rows, %d scored, %d detected, %d (%.0f %%) labelled unk_* / none / below min_confidence, %d errors'
              % (len(G), len(R), len(det), ok, 100.0 * ok / max(len(det), 1), len(E)))
            continue
        n = len(R)
        d = sum(1 for r in R if r.get('detected'))
        o1 = sum(1 for r in R if r.get('ok1'))
        o3 = sum(1 for r in R if r.get('ok3'))
        so = sum(1 for r in R if r.get('shown_ok'))
        of = sum(1 for r in R if r.get('okf'))
        p('== %s: %d rows, %d scored (%s), %d errors | detected %d, top-1 %d (%.0f %%), top-3 %d, family %d (%.0f %%), shown correct %d'
          % (grp, len(G), n, ', '.join('%d %s' % (v, k) for k, v in sorted(ns.items())) or 'all', len(E), d, o1, 100.0 * o1 / max(n, 1), o3,
             of, 100.0 * of / max(n, 1), so))
        by = {}
        for r in R:
            by.setdefault(r['class'], []).append(r)
        p('  %-11s %4s %4s %4s %4s %4s %4s  %s' % ('class', 'n', 'det', 'top1', 'top3', 'fam', 'shwn', 'wrong top-1 (count)'))
        for c in sorted(by):
            rs = by[c]
            wrong = {}
            for r in rs:
                if r.get('detected') and not r.get('ok1'):
                    wrong[r['top1']] = wrong.get(r['top1'], 0) + 1
                elif not r.get('detected'):
                    wrong['(missed)'] = wrong.get('(missed)', 0) + 1
            p('  %-11s %4d %4d %4d %4d %4d %4d  %s' % (c, len(rs), sum(1 for r in rs if r.get('detected')), sum(1 for r in rs if r.get('ok1')),
                                                       sum(1 for r in rs if r.get('ok3')), sum(1 for r in rs if r.get('okf')),
                                                       sum(1 for r in rs if r.get('shown_ok')),
                                                       ', '.join('%s %d' % kv for kv in sorted(wrong.items(), key=lambda kv: -kv[1]))))
    # trust: track level, scored rows of known classes
    p()
    p('trust (scored rows; sure = p >= the class min_p, else min_confidence; unsure = drawn below that; hidden = p < 0.5, none, no track)')
    p('  %-11s %5s | %10s %10s %9s | %12s %12s | %6s | %10s' % ('group', 'n', 'sure right', 'sure wrong', 'precision', 'unsure right',
                                                               'unsure wrong', 'hidden', 'sure share'))
    for grp in ('native', 'confirmed', 'probe', 'unconfirmed', 'disc_audio'):
        R = [r for r in rows if r['group'] == grp and r.get('scored') and 'error' not in r and r['class'] in CLS]
        if not R:
            continue
        c = {'sr': 0, 'sw': 0, 'ur': 0, 'uw': 0, 'h': 0}
        for r in R:
            if not r.get('detected') or not r.get('top1') or r['top1'] == 'none' or float(r.get('p1') or 0) < 0.5:
                c['h'] += 1
            elif r.get('sure'):
                c['sr' if r['ok1'] else 'sw'] += 1
            else:
                c['ur' if r['ok1'] else 'uw'] += 1
        s = c['sr'] + c['sw']
        p('  %-11s %5d | %10d %10d %9.2f | %12d %12d | %6d | %10.2f' % (grp, len(R), c['sr'], c['sw'], c['sr'] / max(s, 1), c['ur'], c['uw'],
                                                                      c['h'], s / max(len(R), 1)))
    # false labels
    p()
    fl = []
    for r in rows:
        if r.get('false_tracks'):
            for f in r['false_tracks'].split(';'):
                c, pr, su, h, off = f.split(':')
                fl.append((r['site'], r['group'], r['id'], c, float(pr), su, int(h), float(off)))
    for site, title in (('single', 'single-service captures / recordings'), ('busy', 'busy sites (unidentified channels: mostly real transmitters)'),
                        ('im', 'captures dominated by receiver intermodulation (real RF, not transmitters)')):
        F = [f for f in fl if f[0] == site]
        for grp in ('native', 'confirmed', 'probe', 'unconfirmed', 'disc_audio', 'open'):
            G = [f for f in F if f[1] == grp]
            if G:
                p('false labels, %s, %s: %d sure, %d unsure (%d entries)' % (title, grp, sum(1 for f in G if f[5] == 'sure'),
                                                                            sum(1 for f in G if f[5] == 'unsure'), len({f[2] for f in G})))
    p()
    p('false labels on own (native) captures, per capture:')
    for f in sorted(fl, key=lambda f: (f[0], f[2], f[7])):
        if f[1] == 'native':
            p('    %-6s %-62s %-10s p %.2f %-6s hits %d at %+.0f kHz' % (f[0], f[2][-62:], f[3], f[4], f[5], f[6], f[7] / 1e3))
    with open(out[:-4] + '.txt', 'w') as fh:
        fh.write('\n'.join(lines) + '\n')


KEYS = ['group', 'class', 'id', 'row', 'site', 'window_s', 'scored', 'why_not', 'on_air', 'on_s', 'detected', 'top1', 'p1', 'sure', 'ok1',
        'ok3', 'okf', 'shown_ok', 'top3', 'hits', 'view', 'det_off', 'det_bw', 'tracks', 'shown_tracks', 'pedestal_db', 'fill_db', 'crop',
        'gated', 'dc_notch_db', 'rate', 'snaps', 'rf', 'iou_pick', 'false_tracks', 'error']


def write_table(rows, path):
    with open(path, 'w') as fh:
        fh.write('\t'.join(KEYS) + '\n')
        for r in rows:
            fh.write('\t'.join(str(r.get(k, '')) for k in KEYS) + '\n')


# ------------------------------------------------------------------ selfcheck
def selfcheck(o, known, thr):
    """the feed path on synthetic captures with known answers: a scenes.py signal as a native 2.4 MS/s file, the
    same signal as a 48 kHz recording (plain, IF-filtered with a raised passband floor, opening with 3 s of exact
    zeros), a silent file and a noise-only file. Exit 1 on a failure."""
    wd = os.path.join(o.work, 'selfcheck')
    os.makedirs(wd, exist_ok=True)
    rng = np.random.default_rng(5)
    results = []

    def make(cls, off, snr):
        d = os.path.join(wd, '%s_%d' % (cls, snr))
        f = os.path.join(d, 'single_%s.cu8' % cls)
        if not os.path.exists(f):
            cmd = [sys.executable, '-I', os.path.join(HERE, 'scenes.py'), '--single', cls, '--offset', str(off), '--snr', str(snr),
                   '--seconds', '8', '--no-fading', '--out', d, '--seed', '11']
            if o.bank:
                cmd += ['--bank', o.bank]
            subprocess.run(cmd, capture_output=True, text=True)
        return f

    def entry(eid, path, fmt, rate, rows, prov='download'):
        return {'id': 'selfcheck/' + eid, 'path': path, 'format': fmt, 'sample_rate_hz': rate, 'center_hz': None, 'channels': 1,
                'signals': [rows[0]['class']] if rows else ['noise'], 'expect': rows, 'provenance': prov, 'verified': 'selfcheck: 1 valid frame (synthetic)',
                'notes': '', 'description': 'selfcheck'}

    def narrow(src, off, out, rate=48000.0, zeros_s=0.0, ifband=None):
        a = np.fromfile(src, np.uint8).astype(np.float32)
        x = ((a[0::2] - 127.5) + 1j * (a[1::2] - 127.5)) / 127.5
        x = x * np.exp(-2j * np.pi * (off - 5000.0) * np.arange(len(x)) / FS)
        fr = Fraction(rate / FS).limit_denominator(2000)
        y = resample_poly(x, fr.numerator, fr.denominator).astype(np.complex64)
        if ifband:
            # an IF-filtered recording: +-ifband passband, 30 dB lower stopband, the passband floor 12 dB up
            Y = np.fft.fft(y)
            fb = np.fft.fftfreq(len(y), 1 / rate)
            Y *= np.where(np.abs(fb) <= ifband, 1.0, 10 ** (-30 / 20))
            y = np.fft.ifft(Y).astype(np.complex64)
            nz = (rng.standard_normal(len(y)) + 1j * rng.standard_normal(len(y))) * float(np.std(y)) * 0.5
            NZ = np.fft.fft(nz) * (np.abs(fb) <= ifband)
            y = (y + np.fft.ifft(NZ)).astype(np.complex64)
        if zeros_s:
            y = np.concatenate([np.zeros(int(zeros_s * rate), np.complex64), y])
        y.astype(np.complex64).tofile(out)
        return out

    cases = []
    for cls, off in (('dmr', 210000.0), ('pocsag', -430000.0)):
        src = make(cls, off, 20)
        if not os.path.exists(src):
            results.append(('make %s' % cls, False, 'scenes.py failed (bank?)'))
            continue
        bw = 12500 if cls == 'dmr' else 16000
        cases.append((entry('%s_native' % cls, src, 'cu8', FS, [{'class': cls, 'offset_hz': off, 'bw_hz': bw}], prov='own-capture'), cls, 'right'))
        cases.append((entry('%s_48k' % cls, narrow(src, off, os.path.join(wd, '%s_48k.cf32' % cls)), 'cf32', 48000.0,
                            [{'class': cls, 'offset_hz': 5000, 'bw_hz': bw}]), cls, 'right'))
        cases.append((entry('%s_48k_if' % cls, narrow(src, off, os.path.join(wd, '%s_48k_if.cf32' % cls), ifband=12000.0), 'cf32', 48000.0,
                            [{'class': cls, 'offset_hz': 5000, 'bw_hz': bw}]), cls, 'right'))
        cases.append((entry('%s_48k_zeros' % cls, narrow(src, off, os.path.join(wd, '%s_48k_zeros.cf32' % cls), zeros_s=3.0), 'cf32', 48000.0,
                            [{'class': cls, 'offset_hz': 5000, 'bw_hz': bw}]), cls, 'right, start >= 3 s'))
    np.zeros(2 * 48000 * 6, np.float32).tofile(os.path.join(wd, 'silent.cf32'))
    cases.append((entry('silent', os.path.join(wd, 'silent.cf32'), 'cf32', 48000.0, []), None, 'reported silent, no label'))
    (((rng.standard_normal(48000 * 6) + 1j * rng.standard_normal(48000 * 6)) * 0.01).astype(np.complex64)).tofile(os.path.join(wd, 'noise.cf32'))
    cases.append((entry('noise', os.path.join(wd, 'noise.cf32'), 'cf32', 48000.0, [{'class': 'unknown', 'offset_hz': None, 'bw_hz': None}]), None,
                  'no drawn label'))
    for e, cls, want in cases:
        rs = one((e, o, known, thr))
        r = rs[0]
        if cls is None:
            ft = [f for f in (r.get('false_tracks') or '').split(';') if f]
            if e['id'].endswith('silent'):
                ok = 'silent' in str(r.get('error', ''))
                results.append((e['id'], ok, r.get('error', '') or 'no error'))
            else:
                drawn = r.get('detected') and r.get('top1') not in ('', 'none') and float(r.get('p1') or 0) >= 0.5 and not r['top1'].startswith('unk_')
                ok = not drawn and not ft
                results.append((e['id'], ok, 'track %s %s, false %d' % (r.get('top1', '-'), r.get('p1', ''), len(ft))))
            continue
        ok = bool(r.get('ok1'))
        if 'start' in want:
            ok = ok and float(r.get('window_s') or 0) >= 3.0
        results.append((e['id'], ok, '%s %.2f (%s) start %.1f s pedestal %s crop %s' % (r.get('top1', '-'), float(r.get('p1') or 0),
                                                                                  'sure' if r.get('sure') else 'unsure', float(r.get('window_s') or 0),
                                                                                  r.get('pedestal_db', ''), r.get('crop', ''))))
    bad = 0
    for name, ok, msg in results:
        print('%-4s %-34s %s' % ('PASS' if ok else 'FAIL', name, msg))
        bad += 0 if ok else 1
    print('selfcheck: %d of %d passed' % (len(results) - bad, len(results)))
    return 1 if bad else 0


def main():
    global iqlib
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--library', default=os.environ.get('KRAKEN_IQ_LIBRARY') or os.path.expanduser('~/krakensdr_iq_library'))
    ap.add_argument('--index', default='', help="another index.json for the library's entries (a staged one; default LIBRARY/index.json)")
    ap.add_argument('--model', default=os.path.join(REPO, 'sigclass.model'))
    ap.add_argument('--work', default=default_work())
    ap.add_argument('--out', default='')
    ap.add_argument('--seconds', type=float, default=4.0)
    ap.add_argument('--jobs', type=int, default=5)
    ap.add_argument('--classes', default='')
    ap.add_argument('--ids', default='')
    ap.add_argument('--max-windows', type=int, default=12)
    ap.add_argument('--rf-mask', default='on', choices=['on', 'off'], help="pass the entry's centre_hz (the model's RF mask)")
    ap.add_argument('--selfcheck', action='store_true', help='the feed path on synthetic captures with known answers')
    ap.add_argument('--bank', default='', help='selfcheck: the bank for scenes.py (default WORK/bank)')
    o = ap.parse_args()
    sys.path.insert(0, os.path.join(o.library, '_tools'))
    import iqlib as _iq
    iqlib = _iq
    o.out = o.out or os.path.join(o.work, 'eval_real.tsv')
    meta, _, _ = model_io.read_model(o.model)
    thr = float(meta.get('min_confidence', 0.0)) if isinstance(meta, dict) else 0.0
    MIN_P.clear()
    if isinstance(meta, dict):
        MIN_P.update({c['id']: float(c['min_p']) for c in meta.get('classes', []) if isinstance(c, dict) and c.get('min_p') is not None})
    known = {c['id'] for c in load_classes()}
    if o.selfcheck:
        sys.exit(selfcheck(o, known, thr))
    ents = iqlib.load_index(o.index or os.path.join(o.library, 'index.json'))['entries']
    sel = [e for e in ents if (not o.classes or e['signals'][0] in o.classes.split(',')) and (not o.ids or o.ids in e['id'])]
    t0 = time.time()
    with Pool(o.jobs) as pool:
        rows = [r for rs in pool.imap_unordered(one, [(e, o, known, thr) for e in sel]) for r in rs]
    rows.sort(key=lambda r: (r['group'], r['class'], r['id'], r['row']))
    write_table(rows, o.out)
    summarise(rows, thr, o.out, time.time() - t0)
    print('table -> %s' % o.out)


if __name__ == '__main__':
    main()
