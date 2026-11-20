#!/usr/bin/env python3
"""Signal classifier training data: the clean signal bank.

  bank.py [--work DIR] [--classes a,b,...] [--quick] [--jobs 12] [--list] [--stats]

For every class of classes.json: many clean, noise-free, single-transmitter
clips of complex baseband (one antenna), made by the plugins' own test
signal generators (plugins/<id>/tools/*_gen.*, run as separate programs)
and by synth.py (analog AM / SSB, CW, carriers, the unknown families), with
seeded random IDs, payloads, modes, speeds, message lengths and timing.

  DIR/bank/<class>/<clip>.cf32   complex64, the signal centred on 0 Hz,
                                 scaled to power 1 while on air
  DIR/bank/<class>/<clip>.json   rate, samples, seconds, centre / 99 %
                                 bandwidth measured, on-air intervals (s),
                                 duty, peak power, variant tags, the commands
  DIR/bank/index.json            every clip's metadata (rewritten each run)

Resumable: a clip whose .json exists is skipped. --quick: 2 short clips a
class (a smoke test). DIR: --work, else $KRAKEN_SIGCLASS_WORK, else
~/.cache/kraken_sigclass - never inside the repository. The plugins must be
built first (make -C kraken_doa_v2/plugins: the .cpp generators link
plugins/lib/build/libkrakendig.a). numpy + scipy only.
"""
import argparse
import contextlib
import io
import json
import os
import shutil
import sys
import time
import traceback
import zlib
from fractions import Fraction
from multiprocessing import Pool

# one BLAS thread per worker process (set before numpy loads): with the default
# thread pools forked workers hung in a futex wait on long wide clips
os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
os.environ.setdefault('OMP_NUM_THREADS', '1')
import numpy as np  # noqa: E402
from scipy.signal import resample_poly, welch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS = os.path.normpath(os.path.join(HERE, '..', '..', 'plugins'))
sys.path.insert(0, os.path.join(PLUGINS, 'sdk', 'tools'))
import make_testsignal as mts  # noqa: E402  (its run(): .py with python3 -I, .cpp compiled into WORK/.bin)


def default_work():
    return os.environ.get('KRAKEN_SIGCLASS_WORK') or os.path.join(os.path.expanduser('~'), '.cache', 'kraken_sigclass')


def load_classes():
    with open(os.path.join(HERE, 'classes.json'), encoding='utf-8') as fh:
        return json.load(fh)['classes']


def G(plugin, name):
    """a plugin generator's path"""
    return os.path.join(PLUGINS, plugin, 'tools', name)


SYNTH = os.path.join(HERE, 'synth.py')
AL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'
HEX = '0123456789ABCDEF'


# ------------------------------------------------------------------ random content

def r_call(rng, suffix=True):
    """a synthetic amateur-style callsign (random letters / digit)"""
    c = ''.join(rng.choice(list(AL), int(rng.integers(1, 3)))) + str(int(rng.integers(0, 10))) + \
        ''.join(rng.choice(list(AL), int(rng.integers(2, 4))))
    return c


def r_text(rng, lo=4, hi=60, chars=AL + '0123456789 ' + '     ./-'):
    n = int(rng.integers(lo, hi + 1))
    return ''.join(rng.choice(list(chars), n)).strip() or 'TEST'


def r_hex(rng, n):
    return ''.join(rng.choice(list(HEX), n))


def r_digits(rng, lo, hi):
    return ''.join(rng.choice(list('0123456789'), int(rng.integers(lo, hi + 1))))


def r_reg(rng):
    return rng.choice(['N', 'D-', 'G-', 'VH-', 'ZK-', 'C-', 'F-', 'JA', 'PH-']) + ''.join(rng.choice(list(AL), 3 + int(rng.integers(0, 2))))


def times(rng, seconds, dur, gap, start=(0.2, 1.0)):
    """transmission start times + durations filling the clip: [(at, dur)]"""
    t = rng.uniform(*start)
    out = []
    while True:
        d = float(rng.uniform(*dur)) if isinstance(dur, tuple) else dur
        if t + d > seconds - 0.2:
            break
        out.append((round(t, 3), round(d, 3)))
        t += d + rng.uniform(*gap)
    return out


# ------------------------------------------------------------------ recipes
# recipe(rng, k, quick) -> job: steps (argv lists, run in the job's folder),
# files (JSON written there first), out (the generator's output), and the
# post-processing: rate (decimate to), recentre (shift the measured centre to
# 0 Hz), merge_ms (gaps shorter than this inside one on-air interval)

def j(steps, out, variant, rate=None, files=None, recentre=True, merge_ms=30, seconds=None):
    return {'steps': steps, 'out': out, 'variant': variant, 'rate': rate, 'files': files or {},
            'recentre': recentre, 'merge_ms': merge_ms}


def rc_dmr(rng, k, quick):
    S = 6 if quick else 16
    mode = 'bs' if rng.random() < 0.45 else 'dmo'
    cc = int(rng.integers(0, 16))
    voice = 'ambe'      # what real radios send: vocoder frames with their FEC (not random bits)
    a = [G('dmr', 'dmr_gen.py'), 'x.cf32', '--mode', mode, '--cc', str(cc), '--voice', voice, '--seed', str(int(rng.integers(1, 1 << 30)))]
    calls = times(rng, S, (1.0, 5.0) if mode == 'bs' else (0.8, 4.0), (0.3, 3.0) if mode == 'dmo' else (-0.5, 2.0))
    for at, d in calls or [(0.5, 2.0)]:
        a += ['--at', str(at), '--slot', str(int(rng.integers(1, 3))), '--dur', str(d),
              '--src', str(int(rng.integers(1, 16776415))), '--dst', str(int(rng.integers(1, 99999)))]
    return j([a], 'x.cf32', {'mode': mode, 'cc': cc, 'voice': voice})


def rc_p25p1(rng, k, quick):
    S = 6 if quick else 16
    nac = '0x%03X' % int(rng.integers(1, 0xFFE))
    a = [G('p25', 'p25_gen.py'), 'x.cf32', '--seed', str(int(rng.integers(1, 1 << 30)))]
    v = {'nac': nac}
    for at, d in times(rng, S, (1.0, 5.0), (0.3, 3.0)) or [(0.5, 2.0)]:
        a += ['--at', str(at), '--nac', nac, '--dur', str(d), '--src', str(int(rng.integers(1, 16777215))),
              '--dst', str(int(rng.integers(1, 65535))), '--end', str(rng.choice(['tdu', 'tdulc'])),
              '--voice', 'imbe']
        if rng.random() < 0.2:
            a += ['--algid', '0x84', '--kid', '0x%X' % int(rng.integers(1, 0xFFFF))]
        if rng.random() < 0.2:
            a += ['--adj', str(int(rng.integers(2, 5)))]
    return j([a], 'x.cf32', v)


def rc_p25p2(rng, k, quick):
    S = 6 if quick else 14
    d = 'out' if rng.random() < 0.5 else 'in'
    nac = '%03X' % int(rng.integers(1, 0xFFE))
    a = [G('p25', 'p25p2_gen.py'), 'x.cf32', '--dir', d, '--seconds', str(S), '--seed', str(int(rng.integers(1, 1 << 30))),
         '--nac', nac, '--wacn', r_hex(rng, 5), '--sysid', '%03X' % int(rng.integers(1, 0xFFF))]
    v = {'dir': d}
    lchs = [0, 1]
    if d == 'out' and rng.random() < 0.3:
        c = int(rng.integers(0, 2))
        a += ['--cch', str(c)]
        lchs.remove(c)
        v['cch'] = True
    free = {0: 0.3, 1: 0.3}      # per logical channel: when it is free again (a call + its hang time + end)
    t = rng.uniform(0.3, 1.0)
    while t < S - 3.0:
        lch = int(rng.choice(lchs))
        at = max(t, free[lch])
        du = float(rng.uniform(1.0, 4.0))
        if at + du > S - 2.5:
            break
        a += ['--lch', str(lch), '--at', '%.3f' % at, '--dur', '%.3f' % du,
              '--src', str(int(rng.integers(1, 16777215))), '--dst', str(int(rng.integers(1, 65535)))]
        if rng.random() < 0.15:
            a += ['--algid', '84', '--kid', '%X' % int(rng.integers(1, 0xFFFF))]
        free[lch] = at + du + 2.5
        t = at + rng.uniform(0.2, 3.0) if d == 'out' else at + du + rng.uniform(0.2, 2.5)
    return j([a], 'x.cf32', v)


def rc_nxdn(rng, k, quick):
    S = 6 if quick else 16
    ran = int(rng.integers(0, 64))
    outb = int(rng.random() < 0.5)
    a = [G('nxdn', 'nxdn_gen.py'), 'x.cf32', '--seed', str(int(rng.integers(1, 1 << 30)))]
    for at, d in times(rng, S, (1.0, 5.0), (0.3, 3.0)) or [(0.5, 2.0)]:
        a += ['--at', str(at), '--ran', str(ran), '--outbound', str(outb), '--dur', str(d), '--src', str(int(rng.integers(1, 65519))), '--dst', str(int(rng.integers(1, 65519)))]
    return j([a], 'x.cf32', {'ran': ran, 'outbound': outb})


def rc_ysf(rng, k, quick):
    S = 6 if quick else 16
    dev = 'narrow' if rng.random() < 0.3 else 'wide'
    a = [G('ysf', 'ysf_gen.py'), 'x.cf32', '--seed', str(int(rng.integers(1, 1 << 30))), '--dev', dev]
    modes = []
    for at, d in times(rng, S, (1.0, 5.0), (0.3, 3.0)) or [(0.5, 2.0)]:
        m = str(rng.choice(['dn', 'vw', 'vd1'], p=[0.5, 0.3, 0.2]))
        modes.append(m)
        a += ['--at', str(at), '--dur', str(d), '--src', r_call(rng), '--mode', m]
        if m != 'vw' and rng.random() < 0.3:
            a += ['--text', r_text(rng, 4, 20)]
        if rng.random() < 0.3:
            a += ['--dgid', str(int(rng.integers(1, 100)))]
    return j([a], 'x.cf32', {'dev': dev, 'modes': sorted(set(modes))})


def rc_dstar(rng, k, quick):
    S = 6 if quick else 16
    a = [G('dstar', 'dstar_gen.py'), 'x.cf32', '--seed', str(int(rng.integers(1, 1 << 30)))]
    for at, d in times(rng, S, (1.0, 5.0), (0.3, 3.0)) or [(0.5, 2.0)]:
        a += ['--at', str(at), '--dur', str(d), '--my', r_call(rng), '--sfx', r_text(rng, 1, 4, AL + '0123456789'),
              '--ur', 'CQCQCQ' if rng.random() < 0.5 else r_call(rng)]
        if rng.random() < 0.4:
            rp = r_call(rng)
            a += ['--rpt1', (rp + '        ')[:7] + 'B', '--rpt2', (rp + '        ')[:7] + 'G']
    return j([a], 'x.cf32', {})


TETRA_CC_OK = [0, 1, 4, 6, 7, 9, 11, 12, 13, 14, 15, 16, 21, 22, 23, 24, 25, 26, 29, 30, 31, 32, 33, 35, 36, 37, 41, 42, 43, 44, 45,
               47, 48, 49, 50, 51, 53, 55, 56, 57, 58, 59, 60, 61, 62, 63]


def rc_tetra(rng, k, quick):
    S = 4 if quick else 12
    # colour codes the tetra plugin decodes from tetra_gen.py's output (18 of 0-63 do not: a
    # generator / decoder mismatch, reported - see README)
    cc = int(rng.choice(TETRA_CC_OK))
    mcc, mnc, la = int(rng.integers(200, 800)), int(rng.integers(1, 100)), int(rng.integers(1, 16000))
    a = [G('tetra', 'tetra_gen.py'), 'x.cf32', '--mcc', str(mcc), '--mnc', str(mnc), '--cc', str(cc), '--la', str(la),
         '--carrier', str(int(rng.integers(0, 4096))), '--seed', str(int(rng.integers(1, 1 << 30)))]
    if rng.random() < 0.75:
        a += ['--at', '0', '--dur', str(S)]
        v = {'keyed': False}
    else:
        for at, d in times(rng, S, (1.5, 5.0), (0.3, 2.0)) or [(0.3, 2.0)]:
            a += ['--at', str(at), '--dur', str(d)]
        v = {'keyed': True}
    v.update({'cc': cc})
    return j([a], 'x.cf32', v)


def rc_mpt1327(rng, k, quick):
    S = 4 if quick else 12
    a = [G('mpt1327', 'mpt1327_gen.py'), 'x.cf32', '--sys', '0x%04X' % int(rng.integers(0, 0x7FFF)),
         '--prefix', str(int(rng.integers(0, 128))), '--dev', str(int(rng.integers(1800, 3200)))]
    if rng.random() < 0.7:
        a += ['--at', '0', '--dur', str(S)]
    else:
        for at, d in times(rng, S, (1.5, 5.0), (0.3, 2.0)) or [(0.3, 2.0)]:
            a += ['--at', str(at), '--dur', str(d)]
    return j([a], 'x.cf32', {})


def page_text(rng, func):
    if func == 0:
        return r_text(rng, 3, 20, '0123456789 -')
    return r_text(rng, 4, 80, AL + '0123456789 .,:-/')


def rc_pocsag(rng, k, quick):
    S = 6 if quick else 16
    baud = [512, 1200, 2400][k % 3]
    dev = int(rng.choice([4500, 4500, 4000, 3500, 4800]))
    a = [G('pagers', 'pocsag_gen.py'), 'x.cf32', '--dev', str(dev), '--bt', '%.2f' % rng.uniform(0.3, 1.0)]
    for at, _ in times(rng, S, (1.0, 4.0), (0.3, 2.5)) or [(0.3, 1.0)]:
        for _ in range(int(rng.integers(1, 4))):
            func = int(rng.choice([0, 3, 3]))
            a += ['--page', str(at), str(baud), str(int(rng.integers(8, 2097151))), str(func), page_text(rng, func)]
    return j([a], 'x.cf32', {'baud': baud, 'dev': dev})


def rc_flex(rng, k, quick):
    mode = ['1600/2', '3200/2', '3200/4', '6400/4'][k % 4]
    nfr = 3 if quick else int(rng.integers(5, 9))
    a = [G('pagers', 'flex_gen.py'), 'x.cf32', '--dev', str(int(rng.choice([4800, 4800, 4500]))),
         '--start', str(int(rng.integers(0, 15))), str(int(rng.integers(0, 120))), '--frames', str(nfr)]
    for fr in range(nfr):
        if rng.random() < 0.25 and fr > 0:
            continue
        for _ in range(int(rng.integers(1, 4))):
            t = str(rng.choice(['alpha', 'alpha', 'numeric', 'tone', 'binary']))
            cap = str(int(rng.integers(1, 2031614)))
            txt = {'alpha': r_text(rng, 4, 70, AL + '0123456789 .,:-'), 'numeric': r_text(rng, 3, 15, '0123456789 -'),
                   'tone': str(int(rng.integers(0, 8))), 'binary': r_hex(rng, 2 * int(rng.integers(2, 10)))}[t]
            a += ['--page', str(fr), mode, cap, t, txt]
    return j([a], 'x.cf32', {'mode': mode, 'frames': nfr})


def aprs_info(rng):
    r = rng.random()
    if r < 0.4:
        return '!%02d%05.2f%s/%03d%05.2f%s>%s' % (rng.integers(0, 90), rng.uniform(0, 60), rng.choice(['N', 'S']),
                                                 rng.integers(0, 180), rng.uniform(0, 60), rng.choice(['E', 'W']), r_text(rng, 0, 30))
    if r < 0.7:
        return '>' + r_text(rng, 4, 50)
    return ':%-9s:%s' % (r_call(rng), r_text(rng, 4, 40))


def rc_aprs(rng, k, quick):
    S = 6 if quick else 16
    a = [G('aprs', 'aprs_gen.py'), 'x.cf32', '--dev', str(int(rng.integers(2500, 4000))), '--txdelay', '%.2f' % rng.uniform(0.1, 0.5)]
    for at, _ in times(rng, S, (0.8, 1.2), (0.3, 3.0)) or [(0.3, 1.0)]:
        src = r_call(rng) + ('-%d' % rng.integers(1, 16) if rng.random() < 0.5 else '')
        path = str(rng.choice(['WIDE1-1', 'WIDE1-1,WIDE2-1', 'WIDE2-2', r_call(rng) + '*,WIDE2-1']))
        a += ['--at', str(at), '%s>APRS,%s:%s' % (src, path, aprs_info(rng))]
    return j([a], 'x.cf32', {})


def rc_v23(rng, k, quick):
    S = 6 if quick else 16
    a = [G('v23_telemetry', 'v23_gen.py'), 'x.cf32', '--dev', str(int(rng.integers(2500, 4500))), '--lead', '%.2f' % rng.uniform(0.1, 0.8),
         '--sync', str(int(rng.integers(4, 16))), '--format', str(rng.choice(['8O1', '8O2', '8E1', '8N1']))]
    for at, _ in times(rng, S, (0.6, 1.0), (0.3, 3.0)) or [(0.3, 1.0)]:
        a += ['--packet', str(at), r_hex(rng, 2 * int(rng.integers(2, 40)))]
    return j([a], 'x.cf32', {})


def rc_nfm(rng, k, quick):
    S = 6 if quick else 18
    iv = times(rng, S, (1.0, 6.0), (0.3, 3.0)) or [(0.3, 3.0)]
    tx = ','.join('%.2f:%.2f' % (a, a + d) for a, d in iv)
    a = [G('analog_id', 'analog_gen.py'), 'x.cf32', '--seconds', str(S), '--tx', tx, '--seed', str(int(rng.integers(1, 1 << 30)))]
    v = {}
    r = rng.random()
    if r < 0.45:
        tone = float(rng.choice([67.0, 71.9, 77.0, 82.5, 88.5, 94.8, 100.0, 107.2, 114.8, 123.0, 131.8, 141.3, 151.4, 162.2,
                                 173.8, 186.2, 203.5, 218.1, 233.6, 250.3]))
        a += ['--ctcss', str(tone)]
        v['tone'] = 'ctcss'
    elif r < 0.65:
        a += ['--dcs', str(rng.choice(['023', '025', '031', '051', '065', '114', '131', '205', '244', '311', '365', '432', '503', '627', '754'])) + str(rng.choice(['N', 'I']))]
        v['tone'] = 'dcs'
    else:
        v['tone'] = 'none'
    if rng.random() < 0.35:
        a += ['--narrow']
        v['narrow'] = True
    else:
        a += ['--voice-dev', str(int(rng.integers(2000, 4500)))]
    if rng.random() < 0.2:
        a += ['--deemph']
    s = rng.random()
    if s < 0.1:
        a += ['--mdc', '01:80:%04X' % int(rng.integers(1, 0xFFFF))]
        v['sig'] = 'mdc'
    elif s < 0.18:
        a += ['--fleetsync', '%d-%d' % (rng.integers(100, 350), rng.integers(1000, 4999))]
        v['sig'] = 'fleetsync'
    elif s < 0.24:
        a += ['--selcall', 'CCIR1:' + r_digits(rng, 5, 5)]
        v['sig'] = 'selcall'
    elif s < 0.3:
        a += ['--dtmf', r_digits(rng, 3, 8)]
        v['sig'] = 'dtmf'
    return j([a], 'x.cf32', v)


def rc_ais(rng, k, quick):
    S = 4 if quick else 12
    ch = str(rng.choice(['A', 'B']))
    a = [G('ais', 'ais_gen.py'), 'x.cf32', '--mmsi', str(int(rng.integers(201000000, 775999999))),
         '--lat', '%.4f' % rng.uniform(-60, 60), '--lon', '%.4f' % rng.uniform(-179, 179),
         '--sog', '%.1f' % rng.uniform(0, 25), '--cog', '%.1f' % rng.uniform(0, 359)]
    t = rng.uniform(0.05, 0.3)
    while t < S - 0.1:
        a += ['--at', '%.3f' % t, ch]
        t += rng.uniform(0.15, 2.0)
    return j([a], 'x.cf32', {'channel': ch})


def acars_msg(rng, up):
    if up:
        return 'up:%s:%s:%s:%s' % (r_reg(rng), rng.choice(['SA', 'SQ', '_d', 'C1', 'RA']), rng.choice(list(AL)), r_text(rng, 0, 60))
    return 'down:%s:%s:%d:M%02dA%s%04d%s' % (r_reg(rng), rng.choice(['H1', '5Z', 'Q0', '_d', '80', '10']), rng.integers(0, 10),
                                            rng.integers(1, 99), ''.join(rng.choice(list(AL), 2)), rng.integers(0, 9999), r_text(rng, 0, 80))


def rc_acars(rng, k, quick):
    S = 6 if quick else 16
    a = [G('acars', 'acars_gen.py'), 'x.cf32', '--depth', '%.2f' % rng.uniform(0.4, 0.95), '--prekey', str(int(rng.integers(8, 40)))]
    up = rng.random() < 0.3
    for at, _ in times(rng, S, (0.4, 1.2), (0.3, 3.0)) or [(0.3, 1.0)]:
        a += ['--at', str(at), acars_msg(rng, up)]
    return j([a], 'x.cf32', {'dir': 'up' if up else 'down'})


def rc_vdl2(rng, k, quick):
    S = 6 if quick else 16
    a = [G('vdl2', 'vdl2_gen.py'), 'x.cf32', '--clock-ppm', '%.1f' % rng.uniform(-20, 20)]
    ac = 'A' + r_hex(rng, 6)
    gs = 'G' + r_hex(rng, 6)
    for at, _ in times(rng, S, (0.2, 0.8), (0.2, 2.5)) or [(0.3, 1.0)]:
        r = rng.random()
        if r < 0.5:
            spec = 'aoa:%s>%s:%s' % (ac, gs, acars_msg(rng, False))
        elif r < 0.75:
            spec = 'data:%s>%s:%d' % (ac, gs, rng.integers(20, 600))
        elif r < 0.9:
            spec = 'rr:%s>%s' % (ac, gs)
        else:
            spec = 'xid:%s:%s:%.3f:%.3f' % (gs, ''.join(rng.choice(list(AL), 4)), rng.uniform(-60, 60), rng.uniform(-179, 179))
        a += ['--at', str(at), spec]
    return j([a], 'x.cf32', {})


def rc_radiosonde(rng, k, quick):
    S = 4 if quick else 14
    t = 'rs41' if k % 2 == 0 else 'm20'
    ser = ('%s%07d' % (rng.choice(list(AL)), rng.integers(0, 9999999))) if t == 'rs41' else \
        '%d%02d-%d-%d%04d' % (rng.integers(0, 10), rng.integers(1, 13), rng.integers(1, 9), rng.integers(0, 2), rng.integers(0, 8192))
    a = [G('radiosonde', 'sonde_gen.py'), 'x.cf32', '--type', t, '--serial', ser, '--lat', '%.3f' % rng.uniform(-60, 60),
         '--lon', '%.3f' % rng.uniform(-179, 179), '--range_km', '%.1f' % rng.uniform(2, 150), '--az_deg', str(int(rng.integers(0, 360))),
         '--alt0', str(int(rng.integers(100, 30000))), '--climb', '%.1f' % rng.uniform(-15, 7), '--seconds', str(S),
         '--phase', '%.2f' % rng.uniform(0, 0.9)]
    return j([a], 'x.cf32', {'type': t})


BEACON_PROTOS = ['std-mmsi', 'std-elt24', 'std-elt-serial', 'std-epirb-serial', 'std-plb-serial', 'std-test', 'nat-elt',
                 'nat-epirb', 'nat-plb', 'user-maritime', 'user-serial', 'user-aviation']


def rc_beacon406(rng, k, quick):
    S = 6 if quick else 16
    p = str(rng.choice(BEACON_PROTOS))
    a = [G('beacon406', 'beacon_gen.py'), 'x.cf32', '--seconds', str(S), '--seed', str(int(rng.integers(1, 1 << 30))),
         '--protocol', p, '--country', str(int(rng.integers(201, 775))), '--phase-noise', '%.2f' % rng.uniform(0, 0.3)]
    fields = {'std-mmsi': ['--mmsi', str(int(rng.integers(201000000, 775999999)))], 'std-elt24': ['--address', r_hex(rng, 6)],
              'user-maritime': ['--mmsi', str(int(rng.integers(201000000, 775999999)))], 'user-aviation': ['--reg', r_reg(rng).replace('-', '')]}
    a += fields.get(p, ['--serial', str(int(rng.integers(1, 9999))), '--cert', str(int(rng.integers(1, 999))), '--national', str(int(rng.integers(1, 99999)))])
    if p.startswith(('std', 'nat')) and rng.random() < 0.7:
        a += ['--lat', '%.3f' % rng.uniform(-60, 60), '--lon', '%.3f' % rng.uniform(-179, 179)]
    if rng.random() < 0.3:
        a += ['--short']
    t = rng.uniform(0.1, 0.6)
    v = {'protocol': p}
    while t < S - 0.7:
        r = rng.random()
        if r < 0.1:
            a += ['--sgb-at', '%.2f' % t]
            t += 1.0
            v['sgb'] = True
        else:
            a += ['--test-at' if r < 0.2 else '--at', '%.2f' % t]
        t += rng.uniform(0.6, 3.0)
    return j([a], 'x.cf32', v)


def rc_wildlife(rng, k, quick):
    S = 8 if quick else 30
    if rng.random() < 0.7:
        tx = {'type': 'train', 'offset_hz': 0, 'ppm': float(rng.uniform(20, 90)), 'width_ms': float(rng.uniform(12, 35)),
              'start': float(rng.uniform(0.05, 1.0)), 'seed': int(rng.integers(1, 1 << 30)), 'jitter_ms': float(rng.uniform(0, 2))}
        if rng.random() < 0.3:
            tx['chirp_hz'] = float(rng.uniform(-120, 0))
    else:
        tx = {'type': 'coded', 'offset_hz': 0, 'gaps_ms': [round(float(x), 1) for x in rng.uniform(20, 65, 3)],
              'bi_s': float(rng.uniform(2.0, 10.0)), 'start': float(rng.uniform(0.05, 1.5))}
    a = [G('wildlife', 'collar_gen.py'), 'tx', 'x.cf32', '--rate', '48000', '--seconds', str(S), '--json', json.dumps(tx)]
    return j([a], 'x.cf32', {'type': tx['type']}, merge_ms=5)


def rc_vor(rng, k, quick):
    S = 6 if quick else 16
    ident = ''.join(rng.choice(list(AL), int(rng.integers(2, 4))))
    a = [G('ils', 'ils_gen.py'), 'x.cf32', '--type', 'vor', '--ident', ident, '--radial', str(int(rng.integers(0, 360))),
         '--wpm', str(int(rng.integers(6, 12))), '--every', str(int(rng.integers(4, 12))), '--at', '0', '--dur', str(S)]
    return j([a], 'x.cf32', {}, recentre=False)


def rc_ils_loc(rng, k, quick):
    S = 6 if quick else 16
    ident = 'I' + ''.join(rng.choice(list(AL), int(rng.integers(2, 4))))
    a = [G('ils', 'ils_gen.py'), 'x.cf32', '--type', 'loc', '--ident', ident, '--ddm', '%.3f' % rng.uniform(-0.15, 0.15),
         '--wpm', str(int(rng.integers(6, 12))), '--every', str(int(rng.integers(4, 12))), '--at', '0', '--dur', str(S)]
    return j([a], 'x.cf32', {}, recentre=False)


def rc_synth(kind, S, Sq):
    def f(rng, k, quick):
        s = Sq if quick else S
        return j([[SYNTH, kind, 'x.cf32', '--seed', str(int(rng.integers(1, 1 << 30))), '--seconds', str(s)]], 'x.cf32', None,
                 recentre=kind not in ('am', 'carrier', 'cw', 'vor'), merge_ms=30)
    return f


# ---- wide generators (2.4 MHz scenes, one antenna, almost no noise)

WIDE = {'channels': 1, 'radius_m': 0.12, 'noise_lsb': 0.01}


def scene_job(gen, scene, variant, rate=None, recentre=True, merge_ms=30, out='x.cf32'):
    return j([[gen, 'sc.json', out]], out, variant, rate=rate, files={'sc.json': scene}, recentre=recentre, merge_ms=merge_ms)


def rc_lora(rng, k, quick):
    S = 4 if quick else 10
    narrow = rng.random() < 0.25
    if narrow:
        bw = float(rng.choice([7812.5, 10417, 15625, 20833, 31250, 41667, 62500]))
        sf = int(rng.integers(6, 11))
    else:
        bw = float([125000, 250000, 500000][k % 3])
        sf = int(rng.integers(7, 13))
    rate = 48000 if bw <= 31250 else 240000 if bw <= 125000 else 480000 if bw <= 250000 else 800000
    rf = 900e6
    proto = str(rng.choice(['lora', 'meshtastic', 'lorawan'], p=[0.4, 0.3, 0.3]))
    # one packet's airtime: (preamble + 4.25 + payload symbols) x 2^SF / BW
    nbytes = int(rng.integers(5, 60))
    tsym = 2 ** sf / bw
    air = (8 + 4.25 + 8 + np.ceil(max(8 * nbytes - 4 * sf + 28, 0) / (4 * sf)) * 5) * tsym
    period = max(air + rng.uniform(0.05, 1.0), air * rng.uniform(1.05, 2.5))
    if k % 5 >= 3:
        # as a receiver mostly sees LoRa: a short packet now and then (most 0.5 s
        # snapshots hold none, a few one or two)
        S = 8 if quick else 24
        period = max(air + 0.2, float(rng.uniform(1.5, 8.0)))
    src = {'proto': proto, 'name': 'x', 'bearing': 0, 'sf': sf, 'bw': bw, 'cr': int(rng.integers(1, 5)), 'freq_mhz': rf / 1e6,
           'level_db': 60, 'period_s': float(period), 't0': float(rng.uniform(0.05, 0.5))}
    if proto == 'lora':
        src['bytes'] = [int(b) for b in rng.integers(0, 256, nbytes)]
        src['sync_word'] = int(rng.choice([0x12, 0x34, 0x2B, int(rng.integers(0, 256))]))
    elif proto == 'meshtastic':
        src.update({'from': int(rng.integers(1, 1 << 32)), 'to': 4294967295, 'id': int(rng.integers(1, 1 << 31)),
                    'port': 'text', 'text': r_text(rng, 3, 40), 'channel': 'LongFast'})
    else:
        src.update({'devaddr': '0x' + r_hex(rng, 8), 'fcnt': int(rng.integers(0, 65535)), 'fport': int(rng.integers(1, 224)),
                    'mtype': str(rng.choice(['unconf_up', 'conf_up'])), 'bytes': [int(b) for b in rng.integers(0, 256, nbytes)]})
    sc = dict(WIDE, rf_hz=rf, seconds=S, seed=int(rng.integers(1, 1 << 30)), sources=[src])
    return scene_job(G('lora', 'lora_gen.py'), sc, {'sf': sf, 'bw': bw, 'proto': proto}, rate=rate)


def rc_ism(fsk):
    def f(rng, k, quick):
        S = 4 if quick else 6
        kinds = ['wh24', 'toyota'] if fsk else ['ev1527', 'pt2262', 'acurite_tower', 'tx141th']
        kind = kinds[k % len(kinds)]
        at = []
        t = rng.uniform(0.1, 0.6)
        while t < S - 0.6:      # far denser than real devices: a 0.5 s cut holds about one
            at.append(round(float(t), 3))
            t += rng.uniform(0.25, 1.2)
        s = {'kind': kind, 'offset_hz': 0, 'bearing': 0, 'level_db': 60, 'at': at, 'clock': float(rng.uniform(0.97, 1.03))}
        if kind == 'ev1527':
            while True:      # a "10" digit pair somewhere (else it reads as a PT2262)
                c = r_hex(rng, 6).lower()
                b = bin(int(c, 16))[2:].zfill(24)
                if '10' in [b[i:i + 2] for i in range(0, 24, 2)]:
                    break
            s.update({'code': c, 'te_us': int(rng.integers(200, 500)), 'frames': int(rng.integers(3, 12))})
        elif kind == 'pt2262':
            s.update({'tristate': ''.join(rng.choice(list('01F'), 12)), 'te_us': int(rng.integers(200, 500)), 'frames': int(rng.integers(3, 12))})
        elif kind == 'acurite_tower':
            s.update({'id': int(rng.integers(1, 16383)), 'channel': str(rng.choice(['A', 'B', 'C'])), 'temperature_C': round(float(rng.uniform(-20, 40)), 1),
                      'humidity': int(rng.integers(5, 99)), 'battery_ok': 1, 'copies': 3})
        elif kind == 'tx141th':
            s.update({'id': int(rng.integers(1, 255)), 'channel': int(rng.integers(0, 3)), 'temperature_C': round(float(rng.uniform(-20, 40)), 1),
                      'humidity': int(rng.integers(5, 99)), 'packets': int(rng.integers(4, 12))})
        elif kind == 'wh24':
            s.update({'id': int(rng.integers(1, 255)), 'temperature_C': round(float(rng.uniform(-20, 40)), 1), 'humidity': int(rng.integers(5, 99)),
                      'wind_dir_deg': int(rng.integers(0, 359)), 'wind_avg_m_s': round(float(rng.uniform(0, 15)), 1),
                      'wind_max_m_s': round(float(rng.uniform(0, 25)), 1), 'rain_mm': round(float(rng.uniform(0, 100)), 1),
                      'dev_hz': float(rng.uniform(25e3, 50e3))})
        else:
            s.update({'id': r_hex(rng, 8).lower(), 'pressure_PSI': round(float(rng.uniform(20, 40)), 2), 'temperature_C': int(rng.integers(-10, 60)),
                      'status': 0, 'copies': int(rng.integers(1, 4)), 'dev_hz': float(rng.uniform(15e3, 30e3))})
        sc = dict(WIDE, rf_hz=433.92e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), sources=[s])
        return scene_job(G('ism', 'ism_gen.py'), sc, {'kind': kind}, rate=240000, merge_ms=30)
    return f


def rc_flarm(rng, k, quick):
    S = 4 if quick else 8
    ch = float(rng.choice([868.2, 868.4]))       # the EU FLARM / OGN channels (the plugin listens there)
    src = []
    kinds = set()
    for i in range(int(rng.integers(1, 9))):     # several aircraft on the one channel, ~1 packet a second each
        kind = 'flarm' if rng.random() < 0.55 else 'ogn'
        kinds.add(kind)
        s = {'kind': kind, 'name': 'x%d' % i, 'address': r_hex(rng, 6), 'addr_type': int(rng.integers(1, 4)), 'channels_mhz': [ch],
             'bearing': 0, 'level_db': float(rng.uniform(45, 60)), 'period_s': float(rng.uniform(0.8, 1.2)), 't0': float(rng.uniform(0.05, 1.0)),
             'jitter': bool(rng.random() < 0.5), 'df_hz': float(rng.uniform(-8e3, 8e3))}
        if kind == 'ogn':
            s.update({'acft_type': int(rng.integers(1, 15)), 'alt_m': int(rng.integers(100, 5000)), 'climb_ms': round(float(rng.uniform(-5, 5)), 1),
                      'speed_kmh': int(rng.integers(0, 250)), 'track': int(rng.integers(0, 359)), 'range_km': float(rng.uniform(1, 40))})
        src.append(s)
    sc = dict(WIDE, rf_hz=ch * 1e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), station=[0.0, 0.0], sources=src)
    return scene_job(G('flarm', 'flarm_gen.py'), sc, {'kinds': '+'.join(sorted(kinds)), 'aircraft': len(src), 'ch_mhz': ch}, rate=480000, merge_ms=10)


def rc_wfm(rng, k, quick):
    S = 3 if quick else 8
    st = {'name': 'x', 'bearing': 0, 'at': 0.0, 'until': S, 'cnr_db': 70, 'pi': r_hex(rng, 4), 'ps': r_text(rng, 3, 8, AL + '0123456789'),
          'pty': int(rng.integers(0, 31)), 'tp': int(rng.integers(0, 2)), 'ms': 1, 'stereo': bool(rng.random() < 0.8),
          'rt': [r_text(rng, 10, 60)], 'audio_dev_hz': float(rng.uniform(25000, 75000)), 'rds_dev_hz': float(rng.choice([0, 1200, 2000, 3000]))}
    if rng.random() < 0.15:
        st['sca'] = [{'hz': 67000, 'type': 'audio', 'dev_hz': 6000, 'inj_hz': 7000, 'tones': [440, 1000]}]
    sc = dict(WIDE, rate=480000, offset_hz=0, rf_hz=98e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), stations=[st])
    return scene_job(G('rds', 'rds_gen.py'), sc, {'stereo': st['stereo'], 'rds': st['rds_dev_hz'] > 0}, recentre=False)


def rc_adsb(rng, k, quick):
    S = 3 if quick else 6
    src = []
    for i in range(int(rng.integers(1, 25))):
        src.append({'name': 'a%d' % i, 'icao': r_hex(rng, 6), 'callsign': ''.join(rng.choice(list(AL), 3)) + r_digits(rng, 1, 4),
                    'cat': int(rng.integers(1, 8)), 'alt_ft': int(rng.integers(500, 42000)), 'gs_kt': int(rng.integers(80, 560)),
                    'track': int(rng.integers(0, 359)), 'vrate_fpm': int(rng.integers(-3000, 3000)), 'range_km': float(rng.uniform(5, 300)),
                    'bearing': 0, 'level_db': float(rng.uniform(35, 60)), 'df_hz': float(rng.uniform(-200e3, 200e3)),
                    'rate': float(rng.uniform(0.5, 1.5)), 'replies': float(rng.choice([0, 0, 1, 3, 8])), 'squawk': int(rng.integers(0, 7777))})
    sc = dict(WIDE, rf_hz=1090e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), station=[0.0, 0.0], sources=src)
    return scene_job(G('adsb', 'adsb_gen.py'), sc, {'aircraft': len(src)}, recentre=False, merge_ms=5)


def rc_uat(rng, k, quick):
    S = 3 if quick else 6
    src = []
    for i in range(int(rng.integers(1, 21))):
        src.append({'kind': 'adsb', 'name': 'u%d' % i, 'address': r_hex(rng, 6), 'aq': 0, 'callsign': r_call(rng), 'cat': int(rng.integers(1, 10)),
                    'alt_ft': int(rng.integers(500, 18000)), 'gs_kt': int(rng.integers(50, 300)), 'track': int(rng.integers(0, 359)),
                    'vrate_fpm': int(rng.integers(-1500, 1500)), 'range_km': float(rng.uniform(3, 150)), 'bearing': 0,
                    'level_db': float(rng.uniform(40, 60)), 'period_s': 1.0, 't0': float(rng.uniform(0.0, 1.0)), 'df_hz': float(rng.uniform(-20e3, 20e3))})
    if rng.random() < 0.4:
        src.append({'kind': 'uplink', 'name': 'gs', 'site': int(rng.integers(1, 30)), 'slot': int(rng.integers(0, 31)), 'range_km': 30,
                    'bearing': 0, 'level_db': float(rng.uniform(45, 60)), 'period_s': 1.0,
                    'products': [{'id': 413, 'text': ['METAR XXXX %02d%02d00Z %03d%02dKT 9999 %s' % (rng.integers(1, 28), rng.integers(0, 23),
                                                                                                     rng.integers(0, 36) * 10, rng.integers(0, 30), r_text(rng, 5, 20))]},
                                 {'id': 63, 'bytes': int(rng.integers(20, 120))}]})
    sc = dict(WIDE, rf_hz=978e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), station=[0.0, 0.0], sources=src)
    return scene_job(G('uat', 'uat_gen.py'), sc, {'sources': len(src)}, recentre=False, merge_ms=5)


def rc_dme(rng, k, quick):
    S = 3 if quick else 8
    mode = str(rng.choice(['X', 'Y']))
    b = {'name': 'b', 'kind': 'beacon', 'mhz': 1100, 'mode': mode, 'pairs_s': int(rng.integers(700, 2800)),
         'ident': ''.join(rng.choice(list(AL), 3)), 'ident_at': float(rng.uniform(0, S)), 'dot_s': float(rng.uniform(0.08, 0.14)),
         'bearing': 0, 'level_db': 60, 'df_hz': float(rng.uniform(-30e3, 30e3))}
    if rng.random() < 0.4:
        b['tacan'] = {'bearing': int(rng.integers(0, 360)), 'm15': float(rng.uniform(0.1, 0.3)), 'm135': float(rng.uniform(0.05, 0.15))}
    src = [b]
    for i in range(int(rng.integers(0, 3))):
        src.append({'name': 'i%d' % i, 'kind': 'interrogator', 'mhz': 1100, 'mode': mode, 'pairs_s': int(rng.integers(10, 150)),
                    'bearing': 0, 'level_db': float(rng.uniform(40, 58)), 'df_hz': float(rng.uniform(-60e3, 60e3))})
    sc = dict(WIDE, rf_hz=1100e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), sources=src)
    return scene_job(G('dme', 'dme_gen.py'), sc, {'mode': mode, 'tacan': 'tacan' in b}, recentre=False, merge_ms=5)


def rc_ssr(rng, k, quick):
    S = 4 if quick else 10
    t = str(rng.choice(['ssr', 'fixed', 'acas'], p=[0.6, 0.2, 0.2]))
    if t == 'ssr':
        s = {'name': 's', 'type': 'ssr', 'bearing': 0, 'peak_db': 60, 'pri_us': int(rng.integers(2000, 8000)), 'jitter_pct': float(rng.uniform(0, 2)),
             'modes': [str(m) for m in rng.choice(['A', 'C'], int(rng.integers(1, 4)))], 'p4': str(rng.choice(['short', 'long', ''])),
             'uf11_every': int(rng.integers(0, 5)), 'ii': int(rng.integers(0, 16)), 'scan_s': float(rng.uniform(2, 12)),
             'beam_deg': float(rng.uniform(2, 4)), 'sidelobe_db': float(rng.uniform(-32, -22)), 'az0_deg': float(rng.uniform(0, 360)),
             'sls_db': float(rng.uniform(-14, -8)), 'targets': [r_hex(rng, 6) for _ in range(int(rng.integers(0, 6)))],
             'df_hz': float(rng.uniform(-20e3, 20e3))}
    elif t == 'fixed':
        s = {'name': 's', 'type': 'fixed', 'bearing': 0, 'level_db': 60, 'period_s': float(rng.uniform(0.3, 1.5)),
             'allcalls': [str(m) for m in rng.choice(['A', 'C'], int(rng.integers(1, 4)))], 'allcall_ms': int(rng.integers(2, 10)),
             'p4': 'short', 'targets': [r_hex(rng, 6) for _ in range(int(rng.integers(1, 6)))], 'rollcall_ms': 1, 'uf': [4, 5],
             'df_hz': float(rng.uniform(-20e3, 20e3))}
    else:
        s = {'name': 's', 'type': 'acas', 'bearing': 0, 'level_db': 60, 'address': r_hex(rng, 6), 'steps': int(rng.integers(4, 10)),
             'step_db': 1.0, 'step_ms': float(rng.uniform(10, 25)), 's1_db': -2, 'period_s': 1.0,
             'uf0_targets': [r_hex(rng, 6) for _ in range(int(rng.integers(0, 3)))], 'broadcast_s': 4.0, 'df_hz': float(rng.uniform(-20e3, 20e3))}
    sc = dict(WIDE, rf_hz=1030e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), response=str(rng.choice(['measured', 'lowpass'])), interrogators=[s])
    return scene_job(G('ssr1030', 'ssr_gen.py'), sc, {'type': t}, recentre=False, merge_ms=5)


def rc_radar(rng, k, quick):
    S = 4 if quick else 10
    pulses = []
    for i in range(int(rng.choice([1, 1, 1, 2]))):
        kind = rng.random()
        width = float(np.exp(rng.uniform(np.log(0.4), np.log(100))))
        pulses.append({'offset_hz': float(rng.uniform(-500e3, 500e3)), 'width_us': round(width, 2),
                       'chirp_hz': float(rng.uniform(-1.6e6, 1.6e6)) if (kind < 0.35 and width > 5) else 0.0,
                       'delay_us': float(0 if i == 0 else rng.uniform(20, 150)), 'rel_db': float(0 if i == 0 else rng.uniform(-6, 0))})
    pt = rng.random()
    if pt < 0.5:
        pri = {'type': 'constant', 'pri_us': float(rng.uniform(250, 5000)), 'jitter_pct': float(rng.choice([0, 0, 1, 5]))}
    elif pt < 0.8:
        base = rng.uniform(500, 3000)
        pri = {'type': 'stagger', 'seq_us': [round(float(base * rng.uniform(0.85, 1.15)), 1) for _ in range(int(rng.integers(2, 6)))]}
    else:
        pri = {'type': 'blocks', 'sets_us': [[round(float(rng.uniform(800, 2500)), 1) for _ in range(2)] for _ in range(2)], 'block': int(rng.integers(4, 12))}
    scan = float(rng.choice([0, rng.uniform(1.5, 12)], p=[0.2, 0.8]))
    r = {'name': 'r', 'bearing': 0, 'peak_db': 60, 'pulses': pulses, 'pri': pri, 'scan_s': scan, 'beam_deg': float(rng.uniform(1, 4)),
         'sidelobe_db': float(rng.uniform(-35, -22)), 'az0_deg': float(rng.uniform(0, 360)), 'rise_us': float(rng.uniform(0.05, 0.5))}
    sc = dict(WIDE, rf_hz=1300e6, seconds=S, seed=int(rng.integers(1, 1 << 30)), radars=[r])
    return scene_job(G('radar', 'radar_gen.py'), sc, {'pri': pri['type'], 'scan': scan > 0, 'chirp': any(p['chirp_hz'] for p in pulses)},
                     recentre=False, merge_ms=10)


def rc_dab(rng, k, quick):
    sec = 0.96 if quick else 2.88
    base = json.load(open(G('dab', 'scene_single.json')))
    base.update({'channels': 1, 'seconds': sec, 'snr_db': 60, 'seed': int(rng.integers(1, 1 << 30)), 'offset_khz': 0,
                 'cfo_hz': 0, 'sco_ppm': 0})
    base['transmitters'] = base['transmitters'][:1]
    base['transmitters'][0].update({'main': int(rng.integers(1, 70)), 'sub': int(rng.integers(1, 24)), 'bearing': 0, 'delay_us': 0, 'level_db': 0})
    if rng.random() < 0.4:      # an SFN: a second transmitter, delayed
        base['transmitters'].append({'name': 't2', 'main': int(rng.integers(1, 70)), 'sub': int(rng.integers(1, 24)), 'bearing': 0,
                                     'delay_us': float(rng.uniform(5, 200)), 'level_db': float(rng.uniform(-15, -3)), 'phase_deg': float(rng.uniform(0, 360))})
    return j([[G('dab', 'dab_gen.cpp'), '--scene', 'sc.json', '--format', 'cf32', '--out', 'x.cf32']], 'x.cf32',
             {'sfn': len(base['transmitters']) > 1}, files={'sc.json': base}, recentre=False)


# (mode, guard, pilots) - DVB-T2: FFT k, GI index (0 = 1/128 .. 6 = 1/4), PP; ATSC 3.0: FFT k, GI 1-12,
# SP index (sfn_gen's tables: only the combinations the standards allow)
DTV = {'dvbt': [('2k', g, 0) for g in (4, 8, 16, 32)] + [('8k', g, 0) for g in (4, 8, 16, 32)],
       'isdbt': [(m, g, 0) for m in ('1', '2', '3') for g in (4, 8, 16, 32)],
       'dvbt2': [('32', 0, 7), ('32', 1, 4), ('32', 2, 2), ('32', 2, 8), ('32', 3, 4), ('32', 4, 2), ('32', 5, 8), ('16', 1, 6),
                 ('16', 2, 4), ('16', 4, 3), ('16', 6, 1), ('16', 3, 5), ('8', 2, 4), ('8', 4, 2), ('8', 6, 1), ('8', 0, 7),
                 ('4', 1, 7), ('4', 4, 2), ('2', 2, 5), ('1', 6, 1)],
       'atsc3': [('8', 1, 10), ('8', 3, 9), ('8', 5, 5), ('8', 4, 6), ('16', 3, 13), ('16', 5, 8), ('16', 4, 11), ('16', 8, 4),
                 ('16', 1, 15), ('32', 3, 12), ('32', 5, 12), ('32', 4, 14), ('32', 7, 8), ('32', 9, 6)],
       'dtmb': [('', p, 0) for p in (420, 595, 945)],
       'atsc': [('', 0, 0)]}


def rc_dtv(std):
    def f(rng, k, quick):
        S = 0.6 if quick else 2.0
        opts = DTV[std]
        mode, g, pil = opts[k % len(opts)]
        bw = {'dvbt': [8, 7, 6], 'isdbt': [6, 6, 8], 'dvbt2': [8, 7, 6, 5], 'atsc3': [6, 7, 8], 'dtmb': [8], 'atsc': [6]}[std]
        bw = int(rng.choice(bw))
        a = [G('dtv', 'sfn_gen.cpp'), '--std', std, '--secs', str(S), '--snr', '70', '--out', 'x.cf32', '--bw', str(bw)]
        v = {'bw_mhz': bw}
        if std in ('dvbt', 'isdbt'):
            a += ['--mode', mode, '--gi', str(g)]
            v.update({'mode': mode, 'gi': g})
        elif std == 'dvbt2':
            a += ['--mode', mode, '--t2gi', str(g), '--pp', str(pil), '--ext', '1' if int(mode) >= 8 and rng.random() < 0.6 else '0']
            v.update({'mode': mode + 'k', 't2gi': g, 'pp': pil})
        elif std == 'atsc3':
            a += ['--mode', mode, '--a3gi', str(g), '--sp', str(pil)]
            v.update({'mode': mode + 'k', 'a3gi': g, 'sp': pil})
        elif std == 'dtmb':
            a += ['--pn', str(g)]
            v.update({'pn': g})
        # the 2.4 MHz window somewhere in the channel
        half = bw * 0.95e6 / 2 * (0.93 if std == 'atsc' else 1)
        if std == 'atsc' and rng.random() < 0.5:     # the ATSC pilot (2.69 MHz below the centre) in view
            off = float(rng.uniform(-2.6e6, -1.6e6))
        elif rng.random() < 0.85:
            off = float(rng.uniform(-(half - 1.25e6), half - 1.25e6))
        else:       # near the channel edge: part of the window empty
            off = float(rng.choice([-1, 1]) * rng.uniform(half - 1.25e6, half - 0.3e6))
        a += ['--offset', '%.0f' % off, '--tx', '0,0,0']
        if rng.random() < 0.3:      # an SFN / echo path
            a += ['--tx', '0,%.1f,%.1f' % (rng.uniform(1, 80), rng.uniform(-15, -3))]
            v['echo'] = True
        if std == 'dvbt':
            a += ['--cell', str(int(rng.integers(0, 65535)))]
        v['offset_hz'] = round(off)
        return j([a], 'x.cf32', v, recentre=False)
    return f


def rc_cells(tech):
    def f(rng, k, quick):
        S = 0.5 if quick else 2.0
        a = [G('cells', 'cell_gen.cpp'), '--secs', str(S), '--snr', '70', '--seed', str(int(rng.integers(1, 1 << 30))), '--out', 'x.cf32',
             # half the carriers lightly loaded (a quiet cell: the reference signals and the
             # synchronisation / broadcast channels, little data)
             '--load', '%.2f' % (rng.uniform(0.02, 0.2) if rng.random() < 0.5 else rng.uniform(0.2, 1.0))]
        v = {}
        # half the carriers / SSBs near the window's centre (where the cells plugin searches)
        near = rng.random() < 0.5
        if tech == 'nr':
            scs = int(rng.choice([15, 15, 30]))
            a += ['--nrscs', str(scs), '--nr_off', '%.0f' % (rng.uniform(-250e3, 250e3) if near else rng.uniform(-900e3, 900e3)),
                  '--ssbper', str(int(rng.choice([10, 20, 20, 40])))]
            cell = 'nr,%d,0,0,%.1f,%.1f,%d' % (rng.integers(0, 1008), rng.uniform(0, 5000), rng.uniform(-300, 300), rng.integers(1, 5))
            v['scs'] = scs
        else:
            t = 'ltetdd' if rng.random() < 0.3 else 'lte'
            nrb = int(rng.choice([6, 15, 25, 50, 100], p=[0.15, 0.2, 0.25, 0.2, 0.2]))
            a += ['--nrb', str(nrb), '--lte_off', '%.0f' % (rng.uniform(-400e3, 400e3) if near or nrb <= 15 else rng.uniform(-900e3, 900e3))]
            cell = '%s,%d,0,0,%.1f,%.1f,%d' % (t, rng.integers(0, 504), rng.uniform(0, 5000), rng.uniform(-300, 300), rng.integers(1, 3))
            v.update({'duplex': 'tdd' if t == 'ltetdd' else 'fdd', 'nrb': nrb})
        a += ['--cell', cell]
        return j([a], 'x.cf32', v, recentre=False)
    return f


def rc_analogtv(rng, k, quick):
    S = 0.5 if quick else 2.0
    std = 'pal' if k % 2 == 0 else 'ntsc'
    a = [G('analogtv', 'analogtv_gen.py'), 'x.cf32', '--standard', std, '--seconds', str(S), '--channels', '1', '--noise', '0',
         '--seed', str(int(rng.integers(1, 1 << 30)))]
    return j([a], 'x.cf32', {'standard': std}, recentre=False)


def rc_then_synth(first, n_first, kind, S, Sq):
    """clips 0 .. n_first-1 from the plugin generator recipe, the later ones from
    synth.py's generic variant (added after the first bank: the first clips stay
    exactly as they were)"""
    syn = rc_synth(kind, S, Sq)

    def f(rng, k, quick):
        return first(rng, k, quick) if k < n_first else syn(rng, k, quick)
    return f


def rc_split(*parts):
    """clips by index range: parts = (n, recipe), ... - the first n clips from the
    first recipe, the next n from the second, ..."""
    def f(rng, k, quick):
        a = 0
        for n, r in parts:
            if k < a + n:
                return r(rng, k - a, quick)
            a += n
        return parts[-1][1](rng, k - a, quick)
    return f


# class -> (recipe, clips, quick clips)
RECIPES = {
    'dmr': (rc_then_synth(rc_dmr, 32, 'dmr_ms', 16, 4), 40),     # + mobile-to-repeater bursts (synth.py)
    # P25 Phase 1: voice calls in C4FM (p25_gen.py), a C4FM control channel, CQPSK / LSM (synth.py)
    'p25p1': (rc_split((24, rc_p25p1), (12, rc_synth('p25cc', 16, 4)), (12, rc_synth('p25lsm', 16, 4))), 48),
    'p25p2': (rc_p25p2, 24),
    # NXDN96 (nxdn_gen.py) and NXDN48 (synth.py)
    'nxdn': (rc_then_synth(rc_nxdn, 20, 'nxdn48', 16, 4), 32), 'ysf': (rc_ysf, 20),
    'dstar': (rc_dstar, 24), 'tetra': (rc_tetra, 24), 'mpt1327': (rc_mpt1327, 16), 'pocsag': (rc_pocsag, 24), 'flex': (rc_flex, 24),
    'aprs': (rc_aprs, 40), 'v23': (rc_v23, 30), 'nfm_voice': (rc_nfm, 30), 'ais': (rc_ais, 40), 'acars': (rc_acars, 40),
    'vdl2': (rc_vdl2, 40), 'radiosonde': (rc_radiosonde, 16), 'beacon406': (rc_beacon406, 30), 'wildlife': (rc_wildlife, 30),
    'am_voice': (rc_synth('am', 20, 6), 30), 'ssb': (rc_synth('ssb', 20, 6), 24), 'cw': (rc_synth('cw', 20, 6), 20),
    'carrier': (rc_synth('carrier', 12, 4), 20), 'vor': (rc_then_synth(rc_vor, 12, 'vor', 16, 4), 24), 'ils_loc': (rc_ils_loc, 12),
    'lora': (rc_lora, 48), 'ism_ook': (rc_then_synth(rc_ism(False), 64, 'ism_ook', 6, 3), 104), 'ism_fsk': (rc_then_synth(rc_ism(True), 50, 'ism_fsk', 6, 3), 80), 'flarm': (rc_flarm, 30),
    'wfm': (rc_then_synth(rc_wfm, 24, 'wfm', 8, 3), 48), 'adsb': (rc_adsb, 16), 'uat': (rc_uat, 16), 'dme': (rc_dme, 12), 'ssr1030': (rc_ssr, 12), 'radar': (rc_radar, 24),
    'dab': (rc_dab, 16), 'dvbt': (rc_dtv('dvbt'), 32), 'dvbt2': (rc_dtv('dvbt2'), 30), 'isdbt': (rc_dtv('isdbt'), 24),
    'dtmb': (rc_dtv('dtmb'), 18), 'atsc': (rc_dtv('atsc'), 16), 'atsc3': (rc_dtv('atsc3'), 28),
    'lte': (rc_cells('lte'), 40), 'nr': (rc_cells('nr'), 32), 'analogtv': (rc_analogtv, 12),
    'unk_fsk': (rc_synth('unk_fsk', 10, 4), 60), 'unk_psk': (rc_synth('unk_psk', 8, 3), 60), 'unk_ofdm': (rc_synth('unk_ofdm', 6, 3), 50),
    'unk_pulse': (rc_synth('unk_pulse', 10, 4), 40), 'unk_noise': (rc_synth('unk_noise', 6, 3), 40),
}


# ------------------------------------------------------------------ analysis

def read_output(path, side):
    """antenna 0 of a generator's output as complex64"""
    ch = int(side.get('channels', 1))
    if path.endswith('.cu8'):
        u = np.fromfile(path, np.uint8).astype(np.float32)
        u = (u - 127.5) / 127.5
        x = (u[0::2] + 1j * u[1::2]).astype(np.complex64)
    else:
        x = np.fromfile(path, np.complex64)
    return np.ascontiguousarray(x[0::ch]) if ch > 1 else x


def psd(x, rate):
    nper = int(min(len(x), 2 ** int(np.round(np.log2(rate / 25.0)))))
    nper = max(256, min(nper, 1 << 17))
    f, p = welch(x, fs=rate, nperseg=nper, return_onesided=False, detrend=False, scaling='spectrum')
    o = np.argsort(f)
    return f[o], p[o]


def occupied(f, p, frac=0.99):
    c = np.cumsum(p)
    c = c / c[-1]
    lo = f[np.searchsorted(c, (1 - frac) / 2)]
    hi = f[min(len(f) - 1, np.searchsorted(c, 1 - (1 - frac) / 2))]
    mid = f[np.searchsorted(c, 0.5)]
    return float(lo), float(hi), float(mid)


def activity(x, rate, merge_ms):
    """per-sample on-air mask (smoothed |x|^2 above -40 dB of its 99.5th
    percentile) -> intervals (1 ms blocks, gaps < merge_ms joined)"""
    pw = np.abs(x) ** 2
    w = max(4, int(rate * 2e-4))
    k = np.ones(w) / w
    sm = np.convolve(pw, k, mode='same')
    lit = sm[sm > sm.max() * 1e-6]
    if not len(lit):
        return np.zeros(len(x), bool), []
    ref = np.percentile(lit, 99.5)
    mask = sm > ref * 1e-4
    B = max(1, int(rate * 1e-3))
    nb = len(mask) // B
    blk = mask[:nb * B].reshape(nb, B).any(axis=1)
    iv = []
    i = 0
    while i < nb:
        if blk[i]:
            s = i
            while i < nb and blk[i]:
                i += 1
            iv.append([s, i])
        else:
            i += 1
    gap = max(1, int(merge_ms))
    out = []
    for a, b in iv:
        if out and a - out[-1][1] < gap:
            out[-1][1] = b
        else:
            out.append([a, b])
    return mask, [[round(a * B / rate, 4), round(b * B / rate, 4)] for a, b in out]


def finish(job, work_dir, dest, meta_base):
    side = json.load(open(os.path.join(work_dir, job['out'].rsplit('.', 1)[0] + '.json')))
    rate = float(side['rate'])
    x = read_output(os.path.join(work_dir, job['out']), side).astype(np.complex128)
    if not np.all(np.isfinite(x)):
        raise RuntimeError('non-finite samples')
    shift = 0.0
    if job['recentre']:
        f, p = psd(x, rate)
        lo, hi, mid = occupied(f, p)
        shift = 0.5 * (lo + hi)
        if abs(shift) > 1:
            x = x * np.exp(-2j * np.pi * shift * np.arange(len(x)) / rate)
    if job['rate'] and abs(job['rate'] - rate) > 1:
        fr = Fraction(job['rate'] / rate).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
        rate = rate * fr.numerator / fr.denominator
    mask, iv = activity(x, rate, job['merge_ms'])
    if not iv:
        raise RuntimeError('no signal in the output')
    p_on = float(np.mean(np.abs(x[mask]) ** 2))
    x = x / np.sqrt(p_on)
    f, p = psd(x, rate)
    lo, hi, mid = occupied(f, p)
    pk = np.convolve(np.abs(x) ** 2, np.ones(4) / 4, mode='same')
    on_s = sum(b - a for a, b in iv)
    meta = dict(meta_base)
    meta['gen_s'] = round(time.time() - meta.pop('t0'), 1)
    meta.update({'rate': rate, 'samples': int(len(x)), 'seconds': round(len(x) / rate, 4),
                 'centre_hz': round(0.5 * (lo + hi), 1), 'bw99_hz': round(hi - lo, 1), 'occ_lo_hz': round(lo, 1), 'occ_hi_hz': round(hi, 1),
                 'shifted_hz': round(shift, 1), 'duty': round(float(np.mean(mask)), 4), 'peak_db': round(float(10 * np.log10(np.percentile(pk, 99.9))), 2),
                 'intervals': iv, 'on_air_s': round(on_s, 3), 'continuous': bool(on_s > 0.98 * len(x) / rate)})
    if side.get('variant'):
        meta['variant'] = dict(side['variant'], **(meta.get('variant') or {}))
    tmp = dest + '.cf32.tmp'
    x.astype(np.complex64).tofile(tmp)
    os.replace(tmp, dest + '.cf32')
    with open(dest + '.json.tmp', 'w') as fh:
        json.dump(meta, fh, indent=0)
    os.replace(dest + '.json.tmp', dest + '.json')
    return meta


def clip_rng(cls, k, seed):
    return np.random.default_rng([seed, zlib.crc32(cls.encode()), k])


def make_clip(args):
    cls, k, quick, work, seed = args
    name = '%s_%03d' % (cls, k)
    dest = os.path.join(work, 'bank', cls, name)
    if os.path.exists(dest + '.json'):
        return name, 'skip', None
    rng = clip_rng(cls, k, seed)
    job = RECIPES[cls][0](rng, k, quick)
    wd = os.path.join(work, 'tmp', name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    t0 = time.time()
    log = open(os.path.join(wd, 'log.txt'), 'w')
    try:
        for fn, content in job['files'].items():
            with open(os.path.join(wd, fn), 'w') as fh:
                json.dump(content, fh, indent=1)
        for st in job['steps']:
            mts.run([str(s) for s in st], wd, work, PLUGINS, log)
        meta = finish(job, wd, dest, {'class': cls, 'clip': name, 'variant': job['variant'],
                                      'cmd': [[os.path.relpath(s, PLUGINS) if isinstance(s, str) and s.startswith(PLUGINS) else
                                               ('tools/sigclass/synth.py' if s == SYNTH else s) for s in st] for st in job['steps']],
                                      'files': job['files'], 'seed': [seed, k], 't0': t0})
        log.close()
        shutil.rmtree(wd, ignore_errors=True)
        return name, 'ok', meta
    except BaseException as e:       # make_testsignal's fail() exits
        log.close()
        msg = '%s: %s' % (type(e).__name__, e)
        try:
            msg += ' | ' + open(os.path.join(wd, 'log.txt')).read()[-600:].replace('\n', ' | ')
        except OSError:
            pass
        if not isinstance(e, SystemExit):
            msg += ' | ' + traceback.format_exc()[-800:].replace('\n', ' | ')
        return name, 'fail', msg


def precompile(work):
    """the .cpp generators, one at a time (make_testsignal.run compiles into WORK/.bin)"""
    wd = os.path.join(work, 'tmp', '_compile')
    os.makedirs(wd, exist_ok=True)
    for cpp in (G('dab', 'dab_gen.cpp'), G('dtv', 'sfn_gen.cpp'), G('cells', 'cell_gen.cpp')):
        with open(os.path.join(wd, 'log.txt'), 'w') as log, contextlib.redirect_stdout(io.StringIO()):
            try:     # compiled, then run with a bad option: exits non-zero, the binary stays
                mts.run([cpp, '--selftest-none'], wd, work, PLUGINS, log)
            except SystemExit:
                pass
        if not os.path.exists(os.path.join(work, '.bin', os.path.splitext(os.path.basename(cpp))[0])):
            sys.exit('compiling %s failed: %s' % (cpp, open(os.path.join(wd, 'log.txt')).read()[-2000:]))


def write_index(work):
    out = []
    root = os.path.join(work, 'bank')
    for cls in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        d = os.path.join(root, cls)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith('.json'):
                m = json.load(open(os.path.join(d, f)))
                m.pop('files', None)
                m['path'] = os.path.join(cls, f[:-5] + '.cf32')
                out.append(m)
    with open(os.path.join(root, 'index.json.tmp'), 'w') as fh:
        json.dump(out, fh)
    os.replace(os.path.join(root, 'index.json.tmp'), os.path.join(root, 'index.json'))
    return out


def stats(index):
    by = {}
    for m in index:
        by.setdefault(m['class'], []).append(m)
    print('%-11s %5s %8s %8s %9s %9s  %s' % ('class', 'clips', 'seconds', 'on_air', 'bw99 kHz', 'rate kHz', 'variants'))
    for c in sorted(by):
        ms = by[c]
        vs = {}
        for m in ms:
            for kk, vv in (m.get('variant') or {}).items():
                if isinstance(vv, (str, int, float, bool)) and kk not in ('rs', 'dev_hz', 'scs_hz', 'width_us', 'pri_ms', 'bw_hz', 'offset_hz',
                                                                             'depth', 'audio_hi_hz', 'wpm', 'rise_ms', 'chirp_hz', 'nfft', 'used',
                                                                             'cp', 'rolloff', 'bt', 'cc', 'ran', 'aircraft', 'sources', 'bw_mhz'):
                    vs.setdefault(kk, set()).add(str(vv))
        bw = np.median([m['bw99_hz'] for m in ms]) / 1e3
        rates = sorted({int(m['rate'] / 1e3) for m in ms})
        print('%-11s %5d %8.1f %8.1f %9.1f %9s  %s' % (c, len(ms), sum(m['seconds'] for m in ms), sum(m['on_air_s'] for m in ms), bw,
                                                       ','.join(map(str, rates)), '; '.join('%s=%s' % (kk, '/'.join(sorted(v))[:60]) for kk, v in vs.items())))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--work', default=default_work())
    ap.add_argument('--classes', default='')
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--seed', type=int, default=20261120)
    ap.add_argument('--clips', type=int, default=0, help='clips a class (default: the recipe\'s)')
    ap.add_argument('--stats', action='store_true', help='only print the bank statistics')
    ap.add_argument('--list', action='store_true', help='list the classes and clip counts')
    o = ap.parse_args()
    work = os.path.abspath(o.work)
    if os.path.commonpath([work, os.path.dirname(os.path.dirname(HERE))]) == os.path.dirname(os.path.dirname(HERE)):
        sys.exit('the work folder must be outside the repository')
    ids = [c['id'] for c in load_classes() if not c.get('hidden')]
    missing = [c for c in ids if c not in RECIPES]
    if missing:
        sys.exit('no recipe for ' + ', '.join(missing))
    sel = [c for c in ids if not o.classes or c in o.classes.split(',')]
    if o.list:
        for c in sel:
            print(c, RECIPES[c][1])
        return
    if o.stats:
        stats(write_index(work))
        return
    if not os.path.exists(os.path.join(PLUGINS, 'lib', 'build', 'libkrakendig.a')):
        sys.exit('build the plugins first: make -C %s' % PLUGINS)
    os.makedirs(work, exist_ok=True)
    precompile(work)
    tasks = []
    for c in sel:
        n = 2 if o.quick else (o.clips or RECIPES[c][1])
        tasks += [(c, k, o.quick, work, o.seed) for k in range(n)]
    # wide generators first (the longest), then the rest
    slow = ('dab', 'dvbt', 'dvbt2', 'isdbt', 'dtmb', 'atsc', 'atsc3', 'lte', 'nr', 'adsb', 'uat', 'dme', 'ssr1030', 'radar', 'analogtv')
    tasks.sort(key=lambda t: 0 if t[0] in slow else 1)
    t0 = time.time()
    nok = nfail = nskip = 0
    with Pool(max(1, o.jobs), maxtasksperchild=8) as pool:
        for name, st, info in pool.imap_unordered(make_clip, tasks):
            if st == 'ok':
                nok += 1
                print('ok   %-18s %6.1f s %7.0f Hz  on %.1f s  bw %.1f kHz  (%.0f s)' % (name, info['seconds'], info['rate'], info['on_air_s'],
                                                                                    info['bw99_hz'] / 1e3, info['gen_s']), flush=True)
            elif st == 'skip':
                nskip += 1
            else:
                nfail += 1
                print('FAIL %s %s' % (name, info), flush=True)
    print('%d made, %d skipped, %d failed in %.0f s' % (nok, nskip, nfail, time.time() - t0))
    shutil.rmtree(os.path.join(work, 'tmp', '_compile'), ignore_errors=True)
    stats(write_index(work))
    if nfail:
        sys.exit(1)


if __name__ == '__main__':
    main()
