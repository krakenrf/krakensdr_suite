#!/usr/bin/env python3
"""Signal classifier training: synthetic captures -> the C++ core's features
-> labelled examples -> one MLP per view -> sigclass.model + a report.

  train.py gen --name A --count N --seed S [--jobs 10] [--work DIR]
        scenes.py captures (2.4 MHz cu8 + truth), each batch run through
        `sigclass_tool features` and matched to the truth (ingest, beside the
        composer) -> WORK/ds/A/batch_NNNNN.npz; the captures are deleted
  train.py build --name A [--name B ...] --out WORK/ds/X.npz
        the batches merged into one cache (retraining needs no regeneration)
  train.py fit --data WORK/ds/A.npz [--test WORK/ds/B.npz] [--hidden 192,96]
        [--epochs 40] [--model OUT] [--report OUT.txt] [--seed 1] [--limit N]
        per view: train / val / test split by bank clip (disjoint payloads,
        IDs, modes), class-weighted cross-entropy (label smoothing), Adam with
        decoupled weight decay, dropout; temperature per view fitted on val;
        min_confidence where the shown predictions are >= 95 % precise on val
  train.py gradcheck                      the MLP's gradient against finite differences
  train.py ingest BATCH --ds DIR          one batch (what gen runs per batch)

Matching (a detection's band [offset -+ bw/2] against each truth's 99 %
band, at least its SNR bandwidth wide; truths not on air in the capture are
ignored): cover_d = overlap / detection width, cover_t = overlap / truth width.
A truth is a candidate when cover_d >= 0.5 or cover_t >= 0.5; with several
candidates of one class (co-channel transmitters of one system) that class,
else the one with >= 6 dB more in-detection power (SNR x bandwidth x cover_t
x on_air) than all the others together labels it, else the detection is
dropped (ambiguous). No truth with cover_d or cover_t over 0.1: the
detection is labelled `none` (noise, leakage, images); in between: dropped.
A truth on air for >= 10 % of the capture and >= 50 % inside the band is
"detected" when a detection was labelled with it or covers >= 30 % of it
(the detection ceiling in the report).

numpy + scipy only (the trainer is written here; no torch / sklearn).
"""
import os

for _v in ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ.setdefault(_v, '8')

import argparse  # noqa: E402
import datetime  # noqa: E402
import glob  # noqa: E402
import json  # noqa: E402
import shlex  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import time  # noqa: E402
import zlib  # noqa: E402

import numpy as np  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bank import default_work, load_classes  # noqa: E402
import model_io  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
TOOL = os.path.join(REPO, 'tools', 'build', 'sigclass_tool')
NUM_VIEWS = 4
VIEW_NAME = ['24 kHz', '96 kHz', '480 kHz', '2.4 MHz']
DET_KEYS = ['offset_hz', 'bw_hz', 'snr_db', 'peak_db', 'duty', 'bursty', 'fills_band', 'view']


def label_ids():
    return [c['id'] for c in load_classes()]


# ------------------------------------------------------------------ ingest

def truth_band(s):
    lo, hi = float(s['lo_hz']), float(s['hi_hz'])
    mid = 0.5 * (lo + hi)
    half = 0.5 * max(hi - lo, float(s.get('bw_hz') or 0.0), 200.0)
    return mid - half, mid + half


def match_capture(dets, truth, lab):
    """dets: list of dicts (tool rows of one capture); truth: the capture's
    truth line -> per detection (label, truth index, cover_d, cover_t) and
    per truth (detected?, the view of the detection that matched)"""
    sig = [s for s in truth['signals']]
    live = [i for i, s in enumerate(sig) if s['on_air'] > 0 and (s['in_band'] is None or s['in_band'] > 0)]
    bands = {i: truth_band(sig[i]) for i in live}
    out = []
    t_det = {i: -1 for i in range(len(sig))}
    t_cov = {i: 0.0 for i in range(len(sig))}
    t_amb = {i: False for i in range(len(sig))}
    for d in dets:
        a = d['offset_hz'] - 0.5 * max(d['bw_hz'], 100.0)
        b = d['offset_hz'] + 0.5 * max(d['bw_hz'], 100.0)
        cands, mx = [], 0.0
        covs = {}
        for i in live:
            lo, hi = bands[i]
            ov = max(0.0, min(b, hi) - max(a, lo))
            cd, ct = ov / (b - a), ov / (hi - lo)
            covs[i] = (cd, ct)
            mx = max(mx, cd, ct)
            # near a wide or pulsed signal (its spectrum spreads past its 99 % band:
            # skirts, lobes, pilots at the edge) a detection is not clearly 'none'
            wt = hi - lo
            if wt > 100e3 and ov == 0.0 and min(abs(a - hi), abs(b - lo)) < 0.5 * wt:
                mx = max(mx, 0.2)
            if ct > t_cov[i]:
                t_cov[i] = ct
                if ct >= 0.3 and t_det[i] < 0:
                    t_det[i] = d['view']
            if cd >= 0.5 or ct >= 0.5:
                s = sig[i]
                p = 10 ** (s['snr_db'] / 10) * s['bw_hz'] * min(ct, 1.0) * s['on_air']
                cands.append((p, i))
        if cands:
            cands.sort(reverse=True)
            same = len({sig[i]['class'] for _, i in cands}) == 1     # co-channel radios of one system
            if len(cands) == 1 or same or cands[0][0] >= 4.0 * sum(p for p, _ in cands[1:]):
                i = cands[0][1]
                out.append((lab.index(sig[i]['class']), i, covs[i][0], covs[i][1]))
                t_det[i] = d['view']
                continue
            for _, i in cands:
                t_amb[i] = True
            out.append((-1, -1, 0.0, 0.0))
        elif mx < 0.1:
            out.append((lab.index('none'), -1, 0.0, 0.0))
        else:
            out.append((-1, -1, 0.0, 0.0))
    match_capture.extra = (t_cov, t_amb)
    return out, t_det


def ingest(batch, ds):
    lab = label_ids()
    os.makedirs(ds, exist_ok=True)
    name = os.path.basename(os.path.normpath(batch))
    dst = os.path.join(ds, name + '.npz')
    if os.path.exists(dst):
        return
    truths = [json.loads(l) for l in open(os.path.join(batch, 'truth.jsonl'))]
    lst = os.path.join(batch, 'files.txt')
    with open(lst, 'w') as fh:
        for t in truths:
            fh.write(os.path.join(batch, t['file']) + '\n')
    pre = os.path.join(batch, 'feat')
    r = subprocess.run([TOOL, 'features', '--rate', '2400000', '--format', 'cu8', '--out', pre, '--list', lst],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('sigclass_tool features failed: ' + r.stderr[-2000:])
    D = int(json.loads(subprocess.run([TOOL, 'layout'], capture_output=True, text=True).stdout)['dim'])
    X = np.fromfile(pre + '.f32', np.float32).reshape(-1, D)
    rows = [json.loads(l) for l in open(pre + '.jsonl')]
    by = {}
    for k, rw in enumerate(rows):
        by.setdefault(os.path.basename(rw['file']), []).append(k)
    n = len(rows)
    y = np.full(n, -1, np.int16)
    tidx = np.full(n, -1, np.int16)
    cov = np.zeros((n, 2), np.float32)
    cap = np.zeros(n, np.int64)
    clip = np.array([''] * n, dtype=object)
    tsnr = np.full(n, np.nan, np.float32)
    ton = np.full(n, np.nan, np.float32)
    T = {k: [] for k in ('cap', 'cls', 'clip', 'snr', 'on_air', 'in_band', 'bw', 'det_view', 'spur', 'lo', 'hi', 'maxcov', 'amb')}
    for t in truths:
        ks = by.get(t['file'], [])
        res, t_det = match_capture([rows[k] for k in ks], t, lab)
        t_cov, t_amb = match_capture.extra
        for k, (lb, ti, cd, ct) in zip(ks, res):
            y[k], tidx[k], cov[k] = lb, ti, (cd, ct)
            cap[k] = t['index']
            if ti >= 0:
                s = t['signals'][ti]
                clip[k] = s['clip'] or ('spur-%d' % t['index'])
                tsnr[k], ton[k] = s['snr_db'], s['on_air']
            else:
                clip[k] = 'cap-%d' % t['index']
        for i, s in enumerate(t['signals']):
            T['cap'].append(t['index'])
            T['cls'].append(lab.index(s['class']))
            T['clip'].append(s['clip'] or ('spur-%d' % t['index']))
            T['snr'].append(s['snr_db'])
            T['on_air'].append(s['on_air'])
            T['in_band'].append(-1.0 if s['in_band'] is None else s['in_band'])
            T['bw'].append(s['bw_hz'])
            T['det_view'].append(t_det[i])
            T['spur'].append(bool(s['spur']))
            lo, hi = truth_band(s)
            T['lo'].append(lo)
            T['hi'].append(hi)
            T['maxcov'].append(t_cov[i])
            T['amb'].append(t_amb[i])
    for t in truths:
        if t['file'] not in by:
            pass
    det = np.array([[rw[k] for k in DET_KEYS] for rw in rows], np.float64).reshape(-1, len(DET_KEYS))
    np.savez(dst + '.tmp.npz', X=X, y=y, tidx=tidx, cov=cov, cap=cap, clip=clip.astype(str), tsnr=tsnr, ton=ton, det=det,
             **{'T_' + k: np.array(v) for k, v in T.items()})
    os.replace(dst + '.tmp.npz', dst)
    for f in (pre + '.f32', pre + '.jsonl', lst):
        os.remove(f)


def cmd_gen(o):
    ds = os.path.join(o.work, 'ds', o.name)
    os.makedirs(ds, exist_ok=True)
    out = os.path.join(o.shm, o.name)
    consume = ' '.join(shlex.quote(x) for x in [sys.executable, '-I', os.path.abspath(__file__), 'ingest', '{batch}', '--ds', ds])
    cmd = [sys.executable, '-I', os.path.join(HERE, 'scenes.py'), '--work', o.work, '--count', str(o.count), '--first', str(o.first),
           '--batch', str(o.batch), '--jobs', str(o.jobs), '--seed', str(o.seed), '--out', out, '--consume', consume,
           '--consumers', str(o.consumers), '--max-gb', '6']
    if o.classes:
        cmd += ['--classes', o.classes]
    # batches already ingested are skipped by scenes.py only while their folder exists: skip them here
    print(' '.join(cmd), flush=True)
    sys.exit(subprocess.run(cmd).returncode)


def cmd_build(o):
    parts = []
    for nm in o.name:
        fs = sorted(glob.glob(os.path.join(o.work, 'ds', nm, 'batch_*.npz')))
        parts += fs
        print('%s: %d batches' % (nm, len(fs)))
    acc = {}
    for f in parts:
        z = np.load(f, allow_pickle=False)
        for k in z.files:
            acc.setdefault(k, []).append(z[k])
    # keys every batch has (batches ingested by an older train.py may lack newer ones)
    out = {k: np.concatenate(v) for k, v in acc.items() if len(v) == len(parts)}
    np.savez(o.out, **out)
    print('%s: %d detections, %d truths' % (o.out, len(out['y']), len(out['T_cls'])))


# ------------------------------------------------------------------ the MLP

class MLP:
    def __init__(self, sizes, rng):
        self.W, self.b = [], []
        for i in range(len(sizes) - 1):
            fan = sizes[i]
            self.W.append((rng.standard_normal((sizes[i + 1], fan)) * np.sqrt(2.0 / fan)).astype(np.float32))
            self.b.append(np.zeros(sizes[i + 1], np.float32))

    def params(self):
        return self.W + self.b

    def forward(self, X, drop=0.0, rng=None):
        acts, masks = [X], []
        a = X
        for i in range(len(self.W)):
            z = a @ self.W[i].T + self.b[i]
            if i + 1 < len(self.W):
                a = np.maximum(z, 0)
                if drop > 0:
                    m = (rng.random(a.shape) >= drop).astype(np.float32) / (1 - drop)
                    a = a * m
                else:
                    m = None
                masks.append(m)
                acts.append(a)
            else:
                a = z
        return a, acts, masks

    def loss_grad(self, X, Y, w, drop=0.0, rng=None):
        """Y: target distributions (n, C); w: sample weights (n,) -> loss, grads"""
        logits, acts, masks = self.forward(X, drop, rng)
        z = logits - logits.max(axis=1, keepdims=True)
        lse = np.log(np.exp(z).sum(axis=1, keepdims=True))
        logp = z - lse
        ws = w / w.sum()
        loss = -float(np.sum(ws[:, None] * Y * logp))
        g = (np.exp(logp) - Y) * ws[:, None]
        gW, gb = [None] * len(self.W), [None] * len(self.W)
        for i in range(len(self.W) - 1, -1, -1):
            gW[i] = g.T @ acts[i]
            gb[i] = g.sum(axis=0)
            if i > 0:
                g = g @ self.W[i]
                g = g * (acts[i] > 0)
                if masks[i - 1] is not None:
                    g = g * masks[i - 1]
        return loss, gW + gb

    def logits(self, X, bs=8192):
        return np.concatenate([self.forward(X[i:i + bs])[0] for i in range(0, len(X), bs)]) if len(X) else np.zeros((0, len(self.b[-1])))


class Adam:
    def __init__(self, params, lr, wd, b1=0.9, b2=0.999, eps=1e-8):
        self.p, self.lr, self.wd, self.b1, self.b2, self.eps = params, lr, wd, b1, b2, eps
        self.m = [np.zeros_like(p) for p in params]
        self.v = [np.zeros_like(p) for p in params]
        self.t = 0

    def step(self, grads, lr, decay_mask):
        self.t += 1
        c1, c2 = 1 - self.b1 ** self.t, 1 - self.b2 ** self.t
        for p, g, m, v, dm in zip(self.p, grads, self.m, self.v, decay_mask):
            m *= self.b1
            m += (1 - self.b1) * g
            v *= self.b2
            v += (1 - self.b2) * g * g
            p -= lr * ((m / c1) / (np.sqrt(v / c2) + self.eps) + (self.wd * p if dm else 0))


def cmd_gradcheck(o):
    rng = np.random.default_rng(3)
    net = MLP([7, 5, 4, 3], rng)
    net.W = [w.astype(np.float64) for w in net.W]
    net.b = [b.astype(np.float64) + rng.standard_normal(b.shape) * 0.1 for b in net.b]
    X = rng.standard_normal((11, 7))
    Y = np.eye(3)[rng.integers(0, 3, 11)] * 0.9 + 0.1 / 3
    w = rng.uniform(0.5, 2, 11)
    loss, grads = net.loss_grad(X, Y, w)
    worst = 0.0
    for p, g in zip(net.params(), grads):
        for idx in np.ndindex(p.shape):
            old = p[idx]
            p[idx] = old + 1e-6
            lp, _ = net.loss_grad(X, Y, w)
            p[idx] = old - 1e-6
            lm, _ = net.loss_grad(X, Y, w)
            p[idx] = old
            num = (lp - lm) / 2e-6
            worst = max(worst, abs(num - g[idx]) / max(1e-8, abs(num) + abs(g[idx])))
    print('gradient check: worst relative error %.2e (%s)' % (worst, 'OK' if worst < 1e-5 else 'FAIL'))
    sys.exit(0 if worst < 1e-5 else 1)


def softmax(z, T=1.0):
    z = z / T
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


def train_view(Xtr, ytr, wtr, Xva, yva, C, o, rng, log):
    D = Xtr.shape[1]
    sizes = [D] + [int(h) for h in o.hidden.split(',') if h] + [C]
    net = MLP(sizes, rng)
    opt = Adam(net.params(), o.lr, o.wd)
    decay = [True] * len(net.W) + [False] * len(net.b)
    n = len(Xtr)
    eps = o.smooth
    Yall = np.full((C, C), eps / C, np.float32) + np.eye(C, dtype=np.float32) * (1 - eps)
    steps = max(1, n // o.bs)
    best, best_state, best_ep, bad = -1.0, None, 0, 0
    for ep in range(o.epochs):
        lr = o.lr * (0.5 * (1 + np.cos(np.pi * ep / o.epochs))) + 1e-5
        perm = rng.permutation(n)
        tl = 0.0
        for s in range(steps):
            idx = perm[s * o.bs:(s + 1) * o.bs]
            loss, grads = net.loss_grad(Xtr[idx], Yall[ytr[idx]], wtr[idx], o.dropout, rng)
            opt.step(grads, lr, decay)
            tl += loss
        if len(Xva):
            pv = net.logits(Xva).argmax(axis=1)
            rec = [np.mean(pv[yva == c] == c) for c in range(C) if np.any(yva == c)]
            score = float(np.mean(rec))
        else:
            score = -tl
        log('    epoch %2d  lr %.5f  loss %.4f  val macro recall %.4f' % (ep + 1, lr, tl / steps, score))
        if score > best:
            best, best_ep, bad = score, ep, 0
            best_state = ([w.copy() for w in net.W], [b.copy() for b in net.b])
        else:
            bad += 1
            if bad >= o.patience:
                break
    net.W, net.b = best_state
    log('    best epoch %d (val macro recall %.4f)' % (best_ep + 1, best))
    return net


def fit_temperature(logits, y):
    if not len(y):
        return 1.0
    best = (1e9, 1.0)
    for lt in np.linspace(np.log(0.3), np.log(5.0), 61):
        T = float(np.exp(lt))
        p = softmax(logits, T)
        nll = -np.mean(np.log(np.maximum(p[np.arange(len(y)), y], 1e-12)))
        best = min(best, (nll, T))
    return best[1]


SPLIT_WORK = [default_work()]


def split_of(clips):
    """0 train, 1 val, 2 test - by bank clip, stratified per class: of each class's
    clips (sorted by a hash of their name) every 10th from the 1st is test, every
    10th from the 2nd val (so a class with >= 3 clips has test and val clips);
    spurs and none (no clip) by capture"""
    clips = np.asarray(clips)
    out = np.zeros(len(clips), np.int8)
    uniq = np.unique(clips)
    # the bank's whole clip list decides (the same split in every dataset)
    allc = set(uniq)
    try:
        allc |= {m['clip'] for m in json.load(open(os.path.join(SPLIT_WORK[0], 'bank', 'index.json')))}
    except (OSError, ValueError):
        pass
    by = {}
    for c in sorted(allc):
        key = c.rsplit('_', 1)[0] if '_' in c and not c.startswith(('spur-', 'cap-')) else '#'
        by.setdefault(key, []).append(c)
    sp = {}
    for key, cs in by.items():
        cs = sorted(cs, key=lambda c: zlib.crc32(c.encode()))
        for k, c in enumerate(cs):
            if key == '#':
                h = zlib.crc32(c.encode()) % 10
                sp[c] = 2 if h == 0 else 1 if h == 1 else 0
            else:
                sp[c] = 2 if k % 10 == 0 else 1 if k % 10 == 1 else 0
    for c in uniq:
        out[clips == c] = sp[c]
    return out


def merges(lab):
    """label index -> label index of the class it is merged into (classes.json
    "merged_into": classes the features cannot tell apart in the receiver's 2.4 MHz)"""
    m = np.arange(len(lab))
    for c in load_classes():
        if c.get('merged_into'):
            m[lab.index(c['id'])] = lab.index(c['merged_into'])
    return m


def load_data(path, lab):
    """a dataset cache (one or several .npz joined by commas) with the merges applied"""
    parts = [dict(np.load(p, allow_pickle=False)) for p in path.split(',')]
    keys = set.intersection(*[set(p) for p in parts])
    z = {k: np.concatenate([p[k] for p in parts]) for k in keys}
    m = merges(lab)
    y = z['y'].astype(np.int64)
    z['y'] = np.where(y >= 0, m[np.maximum(y, 0)], y).astype(np.int16)
    z['T_cls'] = m[z['T_cls']]
    # a piece of a wide signal in a narrow view (a DVB-T pilot, a DME lobe, an ADS-B
    # pulse spectrum's side): a class whose narrowest occupied bandwidth is more than
    # twice the widest this view takes cannot be a signal of its own there - it is
    # trained as none (the page then shows no stray wide-class label on a narrow track)
    occ_lo = np.array([float((c.get('occ_bw_hz') or [0, 0])[0]) for c in load_classes()])
    vmax = np.array([0.45 * 2.4e6 / d for d in (100, 25, 5, 1)])
    view = z['det'][:, 7].astype(int)
    yy = z['y'].astype(np.int64)
    bad = (yy >= 0) & (view < 3) & (occ_lo[np.maximum(yy, 0)] > 2.0 * vmax[view])   # (the widest view takes anything)
    z['y'] = np.where(bad, lab.index('none'), yy).astype(np.int16)
    z['n_view_impossible'] = int(bad.sum())
    return z


def cmd_fit(o):
    SPLIT_WORK[0] = o.work
    t_start = time.time()
    lab = label_ids()
    classes = load_classes()
    by_id = {c['id']: c for c in classes}
    rep = []

    def log(s=''):
        print(s, flush=True)
        rep.append(s)
    z = load_data(o.data, lab)
    zt = load_data(o.test, lab) if o.test else None
    D = z['X'].shape[1]
    sp = split_of(z['clip'])
    keep = z['y'] >= 0
    if o.min_cover_t > 0:
        frag = (z['tidx'] >= 0) & (z['cov'][:, 1] < o.min_cover_t)
        keep &= ~frag
    view = z['det'][:, 7].astype(int)
    rng = np.random.default_rng(o.seed)
    layout = model_io.layout_from_tool(TOOL)
    log('sigclass training %s - data %s (%d detections, %d labelled), test %s' % (
        datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), o.data, len(z['y']), int(keep.sum()), o.test or '(the data file\'s test clips)'))
    log('layout %s, dim %d; MLP %d-%s-C, epochs %d, lr %g, wd %g, dropout %g, smoothing %g, class weight power %g' % (
        layout['hash'], D, D, o.hidden.replace(',', '-'), o.epochs, o.lr, o.wd, o.dropout, o.smooth, o.cw_power))
    views = {}
    vinfo = {}
    test_pack = []
    val_pack = []
    for v in range(NUM_VIEWS):
        mv = keep & (view == v)
        ytr_all = z['y'][mv & (sp == 0)]
        cnt = np.bincount(ytr_all, minlength=len(lab))
        cls_v = [c for c in range(len(lab)) if cnt[c] >= o.min_examples]
        if len(cls_v) < 2:
            log('view %d: too few classes (%d) - no model' % (v, len(cls_v)))
            continue
        remap = -np.ones(len(lab), int)
        remap[cls_v] = np.arange(len(cls_v))
        sel = mv & (remap[np.maximum(z['y'], 0)] >= 0)

        def take(mask):
            return z['X'][mask].astype(np.float32), remap[z['y'][mask]]
        Xtr, ytr = take(sel & (sp == 0))
        Xva, yva = take(sel & (sp == 1))
        if o.limit and len(Xtr) > o.limit:
            ii = rng.choice(len(Xtr), o.limit, replace=False)
            Xtr, ytr = Xtr[ii], ytr[ii]
        Xtr = np.where(np.isfinite(Xtr), Xtr, 0)
        Xva = np.where(np.isfinite(Xva), Xva, 0)
        mean = Xtr.mean(axis=0)
        std = Xtr.std(axis=0)
        std = np.where(std < 1e-6, 0.0, std)
        inv = np.where(std > 0, 1.0 / np.where(std > 0, std, 1), 0.0).astype(np.float32)
        Ztr = ((Xtr - mean) * inv).astype(np.float32)
        Zva = ((Xva - mean) * inv).astype(np.float32)
        cc = np.bincount(ytr, minlength=len(cls_v)).astype(np.float64)
        cw = (cc.sum() / np.maximum(cc, 1) / len(cls_v)) ** o.cw_power
        if lab.index('none') in cls_v:     # the hidden class: a false label is very visible on the page
            cw[cls_v.index(lab.index('none'))] *= o.none_weight
        wtr = cw[ytr].astype(np.float32)
        log('view %d (%s): %d classes, train %d, val %d' % (v, VIEW_NAME[v], len(cls_v), len(Ztr), len(Zva)))
        net = train_view(Ztr, ytr, wtr, Zva, yva, len(cls_v), o, rng, log)
        Lva = net.logits(Zva)
        T = fit_temperature(Lva, yva)
        log('    temperature %.3f' % T)
        layers = [{'W': W, 'b': b, 'act': 'relu' if i + 1 < len(net.W) else 'none'} for i, (W, b) in enumerate(zip(net.W, net.b))]
        views[v] = {'classes': [{'id': lab[c], 'name': by_id[lab[c]]['name']} for c in cls_v], 'mean': mean, 'std': std,
                    'temperature': T, 'layers': layers}
        vinfo[v] = {'classes': [lab[c] for c in cls_v], 'train': int(len(Ztr)), 'val': int(len(Zva)), 'temperature': T}
        val_pack.append((v, cls_v, softmax(Lva, T), np.array(cls_v)[yva], z, np.nonzero(sel & (sp == 1))[0]))
        # test: the test file's test-clip detections (else the data file's)
        zz = zt if zt is not None else z
        spz = split_of(zz['clip'])
        keepz = (zz['y'] >= 0) & (zz['det'][:, 7].astype(int) == v) & (spz == 2)
        idx = np.nonzero(keepz)[0]
        Xte = zz['X'][idx].astype(np.float32)
        Xte = np.where(np.isfinite(Xte), Xte, 0)
        P = softmax(net.logits(((Xte - mean) * inv).astype(np.float32)), T) if len(idx) else np.zeros((0, len(cls_v)))
        test_pack.append((v, cls_v, P, zz['y'][idx], zz, idx))
    # min_confidence on val: shown = predicted not none and p >= thr; precision >= 95 %
    none_i = lab.index('none')
    allp, allok = [], []
    for v, cls_v, P, yv, zz, idx in val_pack:
        if not len(P):
            continue
        pred = np.array(cls_v)[P.argmax(axis=1)]
        truth = yv
        shown = pred != none_i
        allp.append(P.max(axis=1)[shown])
        allok.append((pred == truth)[shown])
    allp = np.concatenate(allp)
    allok = np.concatenate(allok)
    thr = 0.0
    for t in np.linspace(0.0, 0.99, 100):
        s = allp >= t
        if s.sum() and allok[s].mean() >= o.precision:
            thr = float(t)
            break
    log()
    log('min_confidence %.2f: val precision of the shown labels %.3f at %.1f %% of them' % (
        thr, allok[allp >= thr].mean() if np.any(allp >= thr) else float('nan'), 100 * np.mean(allp >= thr)))
    meta = {'classes': classes, 'trained': datetime.datetime.now().strftime('%Y-%m-%d'), 'min_confidence': round(thr, 3),
            'layout_hash': layout['hash'], 'views': {str(k): vv for k, vv in vinfo.items()},
            'dataset': {'file': os.path.basename(o.data), 'detections': int(len(z['y'])), 'captures': int(len(np.unique(z['cap'])))},
            'trainer': {'hidden': o.hidden, 'epochs': o.epochs, 'lr': o.lr, 'wd': o.wd, 'dropout': o.dropout, 'smoothing': o.smooth,
                        'cw_power': o.cw_power, 'none_weight': o.none_weight, 'seed': o.seed},
            'notes': 'none = a detection that is no signal (hidden); unk_* = unknown families. Display a label only at '
                     'p >= min_confidence.'}
    model_io.write_model(o.model, meta, views, layout)
    log('model -> %s (%.2f MB)' % (o.model, os.path.getsize(o.model) / 1e6))
    report(test_pack, lab, thr, log, z if zt is None else zt)
    log()
    log('fit took %.0f s' % (time.time() - t_start))
    if o.report:
        with open(o.report, 'w') as fh:
            fh.write('\n'.join(rep) + '\n')


def report(packs, lab, thr, log, zz):
    none_i = lab.index('none')
    log()
    log('=== TEST (clips disjoint from training) ===')
    tot_n = tot1 = tot3 = 0
    allc = {}
    conf = {}
    snr_bins = [-10, 0, 5, 10, 15, 20, 25, 50]
    acc_snr = np.zeros((len(snr_bins) - 1, 2))
    for v, cls_v, P, y, z, idx in packs:
        if not len(P):
            continue
        ytrue = np.asarray(y)
        order = np.argsort(-P, axis=1)
        top1 = np.array(cls_v)[order[:, 0]]
        top3 = np.array(cls_v)[order[:, :3]]
        ok1 = top1 == ytrue
        ok3 = np.any(top3 == ytrue[:, None], axis=1)
        rec = {c: np.mean(ok1[ytrue == c]) for c in set(ytrue)}
        prec = {c: np.mean(ytrue[top1 == c] == c) for c in set(top1)}
        log('view %d (%s): %d test detections, top-1 %.3f, top-3 %.3f, macro recall %.3f, macro precision %.3f' % (
            v, VIEW_NAME[v], len(y), ok1.mean(), ok3.mean(), np.mean(list(rec.values())), np.mean(list(prec.values()))))
        tot_n += len(y)
        tot1 += ok1.sum()
        tot3 += ok3.sum()
        for c in set(ytrue) | set(top1):
            a = allc.setdefault(c, [0, 0, 0])
            a[0] += int(np.sum(ytrue == c))
            a[1] += int(np.sum((ytrue == c) & ok1))
            a[2] += int(np.sum(top1 == c))
        for t, p in zip(ytrue, top1):
            if t != p:
                conf[(t, p)] = conf.get((t, p), 0) + 1
        ts = z['tsnr'][idx]
        for k in range(len(snr_bins) - 1):
            m = (ts >= snr_bins[k]) & (ts < snr_bins[k + 1])
            acc_snr[k] += (ok1[m].sum(), m.sum())
    if not tot_n:
        log('no test data')
        return
    log('overall: %d test detections, top-1 %.3f, top-3 %.3f' % (tot_n, tot1 / tot_n, tot3 / tot_n))
    recs = [a[1] / a[0] for c, a in allc.items() if a[0]]
    precs = [a[1] / a[2] for c, a in allc.items() if a[2]]
    log('macro recall %.3f, macro precision %.3f (over %d classes)' % (np.mean(recs), np.mean(precs), len(recs)))
    log()
    log('%-11s %7s %7s %7s' % ('class', 'n', 'recall', 'prec'))
    for c in sorted(allc, key=lambda c: lab[c]):
        a = allc[c]
        log('%-11s %7d %7s %7s' % (lab[c], a[0], '%.3f' % (a[1] / a[0]) if a[0] else '-', '%.3f' % (a[1] / a[2]) if a[2] else '-'))
    log()
    log('30 largest confusions (true -> predicted: count, share of the true class):')
    for (t, p), n in sorted(conf.items(), key=lambda kv: -kv[1])[:30]:
        log('  %-10s -> %-10s %6d  %5.1f %%' % (lab[t], lab[p], n, 100.0 * n / allc[t][0]))
    log()
    log('accuracy vs truth SNR (labelled signals, none excluded):')
    for k in range(len(snr_bins) - 1):
        if acc_snr[k, 1]:
            log('  %4d..%3d dB: %6d  top-1 %.3f' % (snr_bins[k], snr_bins[k + 1], acc_snr[k, 1], acc_snr[k, 0] / acc_snr[k, 1]))
    # open set
    log()
    log('open set: how unk_* and none test detections are labelled (top-1, share):')
    for c in [i for i, l in enumerate(lab) if l.startswith('unk_') or l == 'none']:
        rows = []
        for v, cls_v, P, y, z, idx in packs:
            if not len(P):
                continue
            ytrue = np.asarray(y)
            top1 = np.array(cls_v)[P.argmax(axis=1)]
            m = ytrue == c
            rows += list(zip(top1[m], P.max(axis=1)[m]))
        if not rows:
            continue
        cnt = {}
        for p, _ in rows:
            cnt[p] = cnt.get(p, 0) + 1
        known = sum(n for p, n in cnt.items() if not (lab[p].startswith('unk_') or lab[p] == 'none'))
        known_shown = sum(1 for p, q in rows if not (lab[p].startswith('unk_') or lab[p] == 'none') and q >= thr)
        log('  %-10s n %6d: %s | as a known class %.1f %% (shown at min_confidence %.1f %%)' % (
            lab[c], len(rows), ', '.join('%s %.1f%%' % (lab[p], 100.0 * n / len(rows)) for p, n in sorted(cnt.items(), key=lambda kv: -kv[1])[:5]),
            100.0 * known / len(rows), 100.0 * known_shown / len(rows)))
    # detection ceiling
    log()
    log('detection ceiling (test-file truths on air >= 10 %% and >= 50 %% in band; detected = a detection matched it):')
    tc, ts, ton, tib, td = zz['T_cls'], zz['T_snr'], zz['T_on_air'], zz['T_in_band'], zz['T_det_view']
    m0 = (ton >= 0.1) & ((tib >= 0.5) | (tib < 0))
    bins = [-10, 0, 5, 10, 20, 50]
    log('  %-11s %6s %6s ' % ('class', 'n', 'det') + ' '.join('%7s' % ('%d..%d' % (bins[k], bins[k + 1])) for k in range(len(bins) - 1)))
    for c in range(len(lab)):
        m = m0 & (tc == c)
        if not m.any():
            continue
        cells = []
        for k in range(len(bins) - 1):
            mm = m & (ts >= bins[k]) & (ts < bins[k + 1])
            cells.append('%7s' % ('%.2f' % np.mean(td[mm] >= 0) if mm.sum() >= 5 else '-'))
        log('  %-11s %6d %6.3f ' % (lab[c], m.sum(), np.mean(td[m] >= 0)) + ' '.join(cells))
    m = m0
    log('  %-11s %6d %6.3f' % ('all', m.sum(), np.mean(td[m] >= 0)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    g = sub.add_parser('gen')
    g.add_argument('--name', required=True)
    g.add_argument('--count', type=int, required=True)
    g.add_argument('--first', type=int, default=0)
    g.add_argument('--seed', type=int, default=1)
    g.add_argument('--batch', type=int, default=400)
    g.add_argument('--jobs', type=int, default=10)
    g.add_argument('--consumers', type=int, default=2)
    g.add_argument('--classes', default='')
    g.add_argument('--shm', default='/dev/shm/kraken_sigclass')
    i = sub.add_parser('ingest')
    i.add_argument('batch')
    i.add_argument('--ds', required=True)
    b = sub.add_parser('build')
    b.add_argument('--name', action='append', required=True)
    b.add_argument('--out', required=True)
    f = sub.add_parser('fit')
    f.add_argument('--data', required=True)
    f.add_argument('--test', default='')
    f.add_argument('--model', default=os.path.join(REPO, 'sigclass.model'))
    f.add_argument('--report', default='')
    f.add_argument('--hidden', default='192,96')
    f.add_argument('--epochs', type=int, default=40)
    f.add_argument('--patience', type=int, default=8)
    f.add_argument('--bs', type=int, default=512)
    f.add_argument('--lr', type=float, default=2e-3)
    f.add_argument('--wd', type=float, default=1e-4)
    f.add_argument('--dropout', type=float, default=0.2)
    f.add_argument('--smooth', type=float, default=0.05)
    f.add_argument('--cw-power', type=float, default=0.75)
    f.add_argument('--min-examples', type=int, default=100)
    f.add_argument('--min-cover-t', type=float, default=0.0, help='drop fragment detections covering less of their truth')
    f.add_argument('--precision', type=float, default=0.95)
    f.add_argument('--none-weight', type=float, default=1.0, help='extra loss weight of the none class')
    f.add_argument('--limit', type=int, default=0, help='at most this many training examples per view (learning curve)')
    f.add_argument('--seed', type=int, default=1)
    sub.add_parser('gradcheck')
    for p in (g, i, b, f):
        p.add_argument('--work', default=default_work())
    o = ap.parse_args()
    if o.cmd == 'gen':
        cmd_gen(o)
    elif o.cmd == 'ingest':
        ingest(o.batch, o.ds)
    elif o.cmd == 'build':
        cmd_build(o)
    elif o.cmd == 'fit':
        cmd_fit(o)
    elif o.cmd == 'gradcheck':
        cmd_gradcheck(o)


if __name__ == '__main__':
    main()
