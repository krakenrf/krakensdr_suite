"""Per-class "sure" thresholds (meta classes[i]["min_p"]) for a model, from the tuning data only: the
synthetic val split (V) and HALF A of the real rows of one eval_real group (--real-group: native = the own
captures, the recipe until 2026-11-21; confirmed = the decoder-confirmed public recordings, a global recipe
that uses no own capture) - half A = crc32(entry id) even, half B stays test data. Rule per shown class c: the lowest t >= min_confidence at which the val labels c
with p >= t are >= PREC right; then raised above every native-A label c that was wrong at p (+0.01,
capped at 1.01 = never sure) - wrong at an identified signal or drawn where none is (false_tracks).
Writes the model (format 3) with the thresholds and classes.json's RF table in its metadata; run
eval_real.py on it WITH the RF mask (the receiver's default).
  thresholds.py MODEL_IN MODEL_OUT DATA.npz[,..] EVAL_TABLE.tsv [--prec 0.95] [--work DIR] [--real-group native|confirmed]
(numpy only; the shipped model: --prec 0.95)"""
import argparse
import csv
import json
import os
import sys
import zlib
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import model_io
import train
ap = argparse.ArgumentParser()
ap.add_argument('model_in')
ap.add_argument('model_out')
ap.add_argument('data')
ap.add_argument('native')
ap.add_argument('--prec', type=float, default=0.95)
ap.add_argument('--work', default=None)
ap.add_argument('--real-group', default='native', choices=['native', 'confirmed'],
                help='the eval_real group whose half A raises the thresholds (native: own captures; confirmed: public)')
o = ap.parse_args()
train.SPLIT_WORK[0] = o.work or train.default_work()
lab = train.label_ids()
meta, views, hdr = model_io.read_model(o.model_in)
thr0 = float(meta.get('min_confidence', 0.8))
z = train.load_data(o.data, lab)
sp = train.split_of(z['clip'])
y = z['y'].astype(int)
view = z['det'][:, 7].astype(int)
recs = {}          # class -> [(p, right)]
for v, vm in views.items():
    m = np.flatnonzero((sp == 1) & (view == v) & (y >= 0))
    ids = [c['id'] for c in vm['classes']]
    for a in range(0, len(m), 1000):
        idx = m[a:a + 1000]
        Xf = z['X'][idx].astype(np.float64)
        if 'Q' in z:
            Xf = np.concatenate([Xf, z['Q'][idx].astype(np.float64) / 127.0], axis=1)
        P = model_io.forward(vm, Xf)
        for pr, t in zip(P, y[idx]):
            c = ids[pr.argmax()]
            if c != 'none':
                recs.setdefault(c, []).append((float(pr.max()), c == lab[t]))
merged = {c['id']: c['merged_into'] for c in meta['classes'] if c.get('merged_into')}
native_wrong = {}
for r in csv.DictReader(open(o.native), delimiter='\t'):
    # (only rows eval_real scored - on air in their window; tables before 2026-11-21 have no 'scored' column)
    if r['group'] != o.real_group or r.get('error') or r.get('scored', 'True') != 'True' or r['detected'] != 'True' or zlib.crc32(r['id'].encode()) % 2:
        continue
    top, p = r['top1'], float(r['p1'] or 0)
    if not top or top == 'none' or r['class'] not in lab:
        continue
    # (the table's own verdict when it has one: a row with several classes - one channel, two systems)
    if (r['ok1'] == 'False') if r.get('ok1') in ('True', 'False') else (top != merged.get(r['class'], r['class'])):
        native_wrong[top] = max(native_wrong.get(top, 0.0), p)
# ... and the false labels there: drawn tracks at no identified signal (eval_real's false_tracks)
for r in csv.DictReader(open(o.native), delimiter='\t'):
    if r['group'] != o.real_group or zlib.crc32(r['id'].encode()) % 2 or not r.get('false_tracks'):
        continue
    if r.get('site', 'busy' if r['id'].startswith('mixed/') else 'single') != 'single':
        continue    # a busy site's capture (its unidentified channels are mostly real transmitters) or an IM-dominated one
    for f in r['false_tracks'].split(';'):
        c, pr = f.split(':')[:2]
        native_wrong[c] = max(native_wrong.get(c, 0.0), float(pr))
out = {}
for c, lst in sorted(recs.items()):
    lst.sort()
    ps = np.array([p for p, _ in lst])
    ok = np.array([k for _, k in lst], float)
    t = None
    for cand in np.linspace(thr0, 0.99, int(round((0.99 - thr0) / 0.01)) + 1):
        s = ps >= cand
        if s.sum() >= 5 and ok[s].mean() >= o.prec:
            t = float(cand)
            break
    if t is None:
        t = 1.01 if len(lst) >= 5 else thr0
    if c in native_wrong:
        t = max(t, min(1.01, native_wrong[c] + 0.01))
    if t > thr0 + 1e-9:
        out[c] = round(t, 3)
    print('%-11s val shown %5d, precision at min_confidence %.2f%s -> min_p %s' % (
        c, len(lst), ok[ps >= thr0].mean() if np.any(ps >= thr0) else float('nan'),
        ' (native A wrong at %.2f)' % native_wrong[c] if c in native_wrong else '', out.get(c, '-')))
# the class table as classes.json has it now (its RF ranges, generic flags, bands - the
# model file carries them: format 3), plus the thresholds
from bank import load_classes  # noqa: E402
meta['classes'] = [dict(c) for c in load_classes()]
meta['bands'] = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'classes.json'))).get('bands', [])
for c in meta['classes']:
    if c['id'] in out:
        c['min_p'] = out[c['id']]
    else:
        c.pop('min_p', None)
meta['thresholds'] = {'rule': 'per class: the lowest p >= min_confidence with >= %.0f %% right among the synthetic val labels of '
                              'the class, raised above every wrong label of it on half A of the %s rows of the real evaluation; '
                              '1.01 = never sure' % (100 * o.prec, 'own native' if o.real_group == 'native' else 'decoder-confirmed public'),
                      'real_group': o.real_group, 'real_half': 'crc32(entry id) even'}
model_io.write_model(o.model_out, meta, views, model_io.layout_from_tool(train.TOOL))
print('-> %s: %d classes with their own min_p' % (o.model_out, len(out)))
