#!/usr/bin/env python3
"""Debug playback: builds one test transmission of a decoder's catalogue
(plugins/<id>/tools/testsignals.json) into kraken_doa's playback cache.

  make_testsignal.py ID --plugins DIR --cache DIR [--lat L --lon L]
  make_testsignal.py --check --plugins DIR      (every catalogue parses; ids unique)

kraken_doa runs it (nice 10) when the user presses Generate in the
"Debug Playback" box and shows its PROGRESS lines; it also works by hand.
Output: CACHE/ID.dat + CACHE/ID.json (the sidecar kraken_doa reads: flat
keys, see below). Work files go to CACHE/.work-ID/ (deleted afterwards).

A catalogue entry (a JSON list per plugin):
{
  "id": "ais-four-ships",              [a-z0-9-], unique over all plugins
  "title": "...", "description": "...", what is in it + what to expect
  "rf_hz": 162.0e6,                    the band's centre (where the VFO goes)
  "tuner_offset_hz": 300000,           the band's place in the 2.4 MHz the tuner
                                       receives (tuner centre = rf_hz - this);
                                       0 for fixed-frequency / lock_vfo decoders
  "vfo": {"bw_khz": 100, "demod": "NBFM", "opts": {"key": "value"}},
                                       the VFO playback sets up (decoder = the
                                       plugin; opts = its DIGITAL_OPT options)
  "commands": ["..."],                 optional extra WebSocket commands
  "files": {"name.json": {...}},       written into the work dir first; {"$from":
                                       "{tools}/scene.json", "seconds": 6} = that
                                       file with those keys replaced
  "steps": [["{tools}/x_gen.py", "a.cf32", "--opt", "{lat+0.05}"], ...],
                                       *.py runs with python3, *.cpp is
                                       compiled once (g++ -O2, with sdk/ +
                                       lib/build/libkrakendig.a - pffft for
                                       kraken_fft.hpp) into CACHE/.bin/ and run
  "scene": {uca_scene.py SCENE},       narrow scenes: placed on the UCA by
                                       uca_scene.py --clean (no noise in the
                                       file; kraken_doa interpolates to 2.4 MHz
                                       and adds the noise live, "snr_db")
  "output": "out.cu8",                 OR wide scenes: a step writes the 2.4 MHz
                                       multichannel file itself (cu8 or cf32 -
                                       cf32 is converted to cu8) + its .json
  "expect": [{"tid": "512000001", "bearing": 20}, {"bearing": 60}],
  "tol_deg": 5                         the UI's check: a talker with that id (or
                                       any, without "tid") within tol_deg
}
Placeholders in steps / files: {tools} (the plugin's tools/), {sdk}
(plugins/sdk/tools), {plugins}, {work}, {lat} {lon} (the station's location
at generation time, 0 0 without one), {lat+0.05} {lon-0.1} ..., and a value
"{station}" = [lat, lon]. Map decoders' positions are therefore near the
station; the bearings are relative to the array (as the MUSIC plot shows
them), not compass bearings.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

ID_RE = re.compile(r'^[a-z0-9][a-z0-9-]{0,47}$')


def catalogues(plugins):
    out = []
    for d in sorted(os.listdir(plugins)):
        f = os.path.join(plugins, d, 'tools', 'testsignals.json')
        if os.path.isfile(f):
            with open(f) as fh:
                for e in json.load(fh):
                    e['decoder'] = e.get('decoder', d)
                    e['_plugin'] = d
                    out.append(e)
    return out


def progress(pct, text):
    print('PROGRESS %d %s' % (pct, text), flush=True)


def fail(msg):
    print('ERROR ' + msg, flush=True)
    sys.exit(1)


def subst(v, ctx):
    if isinstance(v, str):
        if v == '{station}':
            return [ctx['lat'], ctx['lon']]

        def one(m):
            k, op, num = m.group(1), m.group(2), m.group(3)
            if k in ('lat', 'lon'):
                x = ctx[k] + (float(op + num) if op else 0.0)
                return '%.6f' % x
            return ctx[k]
        s = re.sub(r'\{(tools|sdk|plugins|work|lat|lon)(?:([+-])([0-9.]+))?\}', one, v)
        if s != v and re.fullmatch(r'-?[0-9.]+', s) and re.fullmatch(r'\{(lat|lon)[^}]*\}', v):
            return float(s)
        return s
    if isinstance(v, list):
        return [subst(x, ctx) for x in v]
    if isinstance(v, dict):
        return {k: subst(x, ctx) for k, x in v.items()}
    return v


def run(argv, work, cache, plugins, log):
    exe = argv[0]
    if exe.endswith('.py'):
        argv = [sys.executable, '-I'] + argv
    elif exe.endswith('.cpp'):
        name = os.path.splitext(os.path.basename(exe))[0]
        bindir = os.path.join(cache, '.bin')
        os.makedirs(bindir, exist_ok=True)
        binp = os.path.join(bindir, name)
        if not os.path.exists(binp) or os.path.getmtime(binp) < os.path.getmtime(exe):
            src_dir = os.path.dirname(os.path.dirname(exe))
            cmd = ['g++', '-std=c++20', '-O2', '-I' + src_dir, '-I' + os.path.join(plugins, 'sdk'),
                   '-I' + os.path.join(plugins, '..', 'external', 'pffft'), exe,
                   os.path.join(plugins, 'lib', 'build', 'libkrakendig.a'), '-lm', '-o', binp + '.tmp']
            r = subprocess.run(cmd, cwd=work, stdout=log, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                fail('compiling %s failed (see %s)' % (os.path.basename(exe), log.name))
            os.replace(binp + '.tmp', binp)
        argv = [binp] + argv[1:]
    r = subprocess.run(argv, cwd=work, stdout=log, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        fail('%s failed (exit %d, see %s)' % (os.path.basename(exe), r.returncode, log.name))


def to_cu8(src, dst, channels, progress_base):
    """2.4 MHz multichannel cf32 -> cu8 (rms 0.15 of full scale, like heimdall's)"""
    import numpy as np
    x = np.memmap(src, dtype=np.complex64, mode='r')
    n = x.shape[0] // channels * channels
    head = np.asarray(x[:min(n, 1 << 21)])
    sc = 0.15 / max(float(np.sqrt(np.mean(np.abs(head) ** 2))), 1e-12)
    step = channels * (1 << 18)
    last = -1
    with open(dst, 'wb') as fo:
        for a in range(0, n, step):
            b = np.asarray(x[a:min(n, a + step)]) * sc
            iq = np.empty((b.shape[0], 2), np.float32)
            iq[:, 0] = b.real
            iq[:, 1] = b.imag
            np.clip(np.rint(iq * 127.5 + 127.5), 0, 255).astype(np.uint8).tofile(fo)
            pct = progress_base + int(10 * a / max(n, 1))
            if pct != last:
                progress(pct, 'converting to 8 bits')
                last = pct


CACHE_MAX_BYTES = 3 << 30


def prune(cache, keep):
    """the cache over CACHE_MAX_BYTES: the least recently played / made go
    (kraken_doa touches a sidecar when it plays it)"""
    items = []
    for f in os.listdir(cache):
        if f.endswith('.dat') and not f.startswith('.'):
            i = f[:-4]
            side = os.path.join(cache, i + '.json')
            t = os.path.getmtime(side) if os.path.exists(side) else 0
            items.append((t, i, os.path.getsize(os.path.join(cache, f))))
    total = sum(x[2] for x in items)
    for t, i, sz in sorted(items):
        if total <= CACHE_MAX_BYTES:
            break
        if i == keep:
            continue
        for ext in ('.dat', '.json', '.log'):
            try:
                os.remove(os.path.join(cache, i + ext))
            except OSError:
                pass
        total -= sz


def main():
    a = sys.argv[1:]
    o = {'plugins': None, 'cache': None, 'lat': '0', 'lon': '0'}
    ident, check = None, False
    i = 0
    while i < len(a):
        if a[i] == '--check':
            check = True
            i += 1
        elif a[i].startswith('--') and i + 1 < len(a):
            o[a[i][2:]] = a[i + 1]
            i += 2
        else:
            ident = a[i]
            i += 1
    if not o['plugins'] or not (check or (ident and o['cache'])):
        print(__doc__)
        sys.exit(2)
    plugins = os.path.abspath(o['plugins'])
    entries = catalogues(plugins)
    if check:
        seen = set()
        for e in entries:
            assert ID_RE.match(e['id']), e['id']
            assert e['id'] not in seen, 'duplicate ' + e['id']
            seen.add(e['id'])
            assert ('scene' in e) != ('output' in e), e['id'] + ': scene or output'
            for k in ('title', 'description', 'rf_hz', 'vfo'):
                assert k in e, e['id'] + ': no ' + k
        print('%d test transmissions in %d catalogues: OK' % (len(entries), len({e['_plugin'] for e in entries})))
        return
    if not ID_RE.match(ident):
        fail('bad id')
    match = [e for e in entries if e['id'] == ident]
    if not match:
        fail('no test transmission "%s"' % ident)
    e = match[0]
    cache = os.path.abspath(o['cache'])
    os.makedirs(cache, exist_ok=True)
    work = os.path.join(cache, '.work-' + ident)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    ctx = {'tools': os.path.join(plugins, e['_plugin'], 'tools'), 'sdk': os.path.join(plugins, 'sdk', 'tools'),
           'plugins': plugins, 'work': work, 'lat': float(o['lat']), 'lon': float(o['lon'])}
    log = open(os.path.join(cache, ident + '.log'), 'w')
    t0 = time.time()
    for name, content in e.get('files', {}).items():
        content = subst(content, ctx)
        if isinstance(content, dict) and '$from' in content:
            # an in-repo scene file with some keys replaced
            base = json.load(open(content.pop('$from')))
            base.update(content)
            content = base
        with open(os.path.join(work, name), 'w') as fh:
            json.dump(content, fh, indent=1)
    steps = e.get('steps', [])
    for k, st in enumerate(steps):
        progress(5 + 70 * k // max(len(steps) + ('scene' in e), 1), 'running %s' % os.path.basename(st[0]))
        run([str(x) for x in subst(st, ctx)], work, cache, plugins, log)
    side = {}
    dat = os.path.join(cache, ident + '.dat.tmp')
    if 'scene' in e:
        progress(75, 'placing the transmitters on the array')
        sc = subst(e['scene'], ctx)
        sc.setdefault('rf_hz', e['rf_hz'])
        sc.setdefault('channels', 5)
        sc.setdefault('radius_m', 0.12)
        sc.setdefault('seconds', e.get('seconds', 10))
        with open(os.path.join(work, 'scene_def.json'), 'w') as fh:
            json.dump(sc, fh, indent=1)
        run([os.path.join(ctx['sdk'], 'uca_scene.py'), 'scene_def.json', 'scene.cf32', '--clean'], work, cache, plugins, log)
        side = json.load(open(os.path.join(work, 'scene.json')))   # uca_scene's sidecar
        os.replace(os.path.join(work, 'scene.cf32'), dat)
        fmt = 'cf32'
    else:
        out = os.path.join(work, e['output'])
        side = json.load(open(out.rsplit('.', 1)[0] + '.json'))
        if out.endswith('.cf32'):
            to_cu8(out, dat, int(side['channels']), 80)
        else:
            os.replace(out, dat)
        fmt = 'cu8'
        if abs(float(side['rate']) - 2.4e6) > 1:
            fail('a wide test transmission must be at 2.4 MHz')
    rate = float(side['rate'])
    ch = int(side['channels'])
    bps = 8 * ch if fmt == 'cf32' else 2 * ch
    nsamp = os.path.getsize(dat) // bps
    off = float(e.get('tuner_offset_hz', 0))
    vfo = e.get('vfo', {})
    cmds = ['DIGITAL_OPT:0:%s.%s:%s' % (e['decoder'], k, v) for k, v in vfo.get('opts', {}).items()]
    cmds += e.get('commands', [])
    meta = {
        'id': ident, 'decoder': e['decoder'], 'title': e.get('title', ident),
        'format': fmt, 'rate': rate, 'channels': ch, 'samples': nsamp, 'seconds': nsamp / rate,
        'rf_hz': float(e['rf_hz']), 'tuner_hz': float(e['rf_hz']) - off, 'band_offset_hz': off,
        'radius_m': float(side.get('radius_m', 0.12)),
        'noise_rms': float(side.get('noise_rms', 0.0)),
        'vfo_offset_khz': float(vfo.get('offset_khz', off / 1e3)), 'vfo_bw_khz': float(vfo.get('bw_khz', 2400)),
        'vfo_demod': vfo.get('demod', 'NBFM'), 'vfo_mode': 'PLUGIN:' + e['decoder'],
        'commands': '\n'.join(cmds), 'generated': int(time.time()),
        'truth': side.get('truth', []),
    }
    with open(os.path.join(cache, ident + '.json.tmp'), 'w') as fh:
        json.dump(meta, fh, indent=1)
    os.replace(dat, os.path.join(cache, ident + '.dat'))
    os.replace(os.path.join(cache, ident + '.json.tmp'), os.path.join(cache, ident + '.json'))
    shutil.rmtree(work, ignore_errors=True)
    log.close()
    prune(cache, ident)
    progress(100, 'done in %.0f s, %.0f MB' % (time.time() - t0, os.path.getsize(os.path.join(cache, ident + '.dat')) / 1e6))
    print('DONE', flush=True)


if __name__ == '__main__':
    main()
