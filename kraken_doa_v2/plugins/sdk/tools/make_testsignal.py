#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 KrakenRF Inc. - the plugin SDK (plugins/sdk/LICENSE)
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
                                       (Windows: the tools/build/NAME.exe
                                       plugins/Makefile prebuilt, if there)
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
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time

ID_RE = re.compile(r'^[a-z0-9][a-z0-9-]{0,47}$')
WINDOWS = os.name == 'nt'
MACOS = sys.platform == 'darwin'


def catalogues(plugins):
    out = []
    for d in sorted(os.listdir(plugins)):
        f = os.path.join(plugins, d, 'tools', 'testsignals.json')
        if os.path.isfile(f):
            with open(f, encoding='utf-8') as fh:
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
        # Windows: UTF-8 mode, as kraken_doa runs this script (open() and the
        # pipes default to the ANSI code page there; -E ignores PYTHONUTF8),
        # and -E instead of -I: -I also hides the user site-packages, where
        # the Microsoft Store Python's pip installs numpy + scipy (macOS:
        # likewise - Apple's python3 has no writable site-packages, pip
        # installs with --user)
        argv = ([sys.executable, '-X', 'utf8', '-E'] if WINDOWS else
                [sys.executable, '-E'] if MACOS else [sys.executable, '-I']) + argv
    elif exe.endswith('.cpp'):
        name = os.path.splitext(os.path.basename(exe))[0]
        bindir = os.path.join(cache, '.bin')
        os.makedirs(bindir, exist_ok=True)
        # Windows: name.exe - a program without it doesn't start from a
        # command line (CreateProcess appends .exe), and g++ adds none to
        # "-o name.exe.tmp"
        binp = os.path.join(bindir, name + ('.exe' if WINDOWS else ''))
        # Windows: plugins/Makefile prebuilds the generators (tools/build/),
        # as a PC running kraken_doa usually has no compiler - an older one
        # is taken too when there is no g++ to rebuild it with
        pre = os.path.join(os.path.dirname(exe), 'build', name + '.exe')
        if WINDOWS and os.path.exists(pre) and (os.path.getmtime(pre) >= os.path.getmtime(exe) or not shutil.which('g++')):
            binp = pre
        elif not os.path.exists(binp) or os.path.getmtime(binp) < os.path.getmtime(exe):
            src_dir = os.path.dirname(os.path.dirname(exe))
            # a plugin with its own LICENSE (plugins/Makefile): the SDK and
            # pffft only, never the source-available lib/
            libdir = os.path.join(plugins, 'lib', 'build')
            libs = ([os.path.join(libdir, 'pffft.o'), os.path.join(libdir, 'pffft_common.o')]
                    if os.path.exists(os.path.join(src_dir, 'LICENSE')) else [os.path.join(libdir, 'libkrakendig.a')])
            cmd = ['g++', '-std=c++20', '-O2', '-I' + src_dir, '-I' + os.path.join(plugins, 'sdk'),
                   '-I' + os.path.join(plugins, '..', 'external', 'pffft'), exe] + libs + ['-lm', '-o', binp + '.tmp']
            if WINDOWS:
                if not shutil.which('g++'):
                    fail('%s is not built: run make in kraken_doa_v2/plugins (MSYS2 UCRT64) - it makes %s'
                         % (os.path.basename(exe), pre))
                # the plugins' Windows flags (plugins/Makefile): M_PI only
                # with _USE_MATH_DEFINES under -std=c++20; pffft linked
                # statically (pffft.h declares its functions dllimport
                # otherwise); the POSIX shims + the process setup (binary file
                # mode, Winsock ...) of every exe of the suite; a static exe,
                # so the generator doesn't need MinGW's DLLs on the PATH
                win = os.path.join(plugins, '..', 'win')
                cmd[1:1] = ['-D_WIN32_WINNT=0x0A00', '-DWINVER=0x0A00', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
                            '-DNOGDI', '-D_USE_MATH_DEFINES', '-D_FILE_OFFSET_BITS=64', '-DPFFFT_STATIC_DEFINE',
                            '-include', os.path.join(win, 'posix_compat.h')]
                cmd.insert(cmd.index(exe) + 1, os.path.join(win, 'win_init.cpp'))
                k = cmd.index('-lm') + 1
                cmd[k:k] = ['-static', '-lws2_32', '-lwinmm']
            elif MACOS:
                # the plugins' macOS shims (plugins/Makefile: -include ../mac/posix_compat.h)
                cmd[1:1] = ['-include', os.path.join(plugins, '..', 'mac', 'posix_compat.h')]
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


def replace(src, dst):
    """os.replace - on Windows also onto a file kraken_doa has open: a moment
    of reading (no FILE_SHARE_DELETE) is waited out, a file held open for
    longer is renamed aside first (allowed while it is open, unlike replacing
    it) and deleted once it is let go (here or by prune)"""
    if not WINDOWS:
        os.replace(src, dst)
        return
    for _ in range(20):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            time.sleep(0.05)
    aside = '%s.old-%d-%d' % (dst, os.getpid(), int(time.time() * 1000))
    os.replace(dst, aside)
    os.replace(src, dst)
    try:
        os.remove(aside)
    except OSError:
        pass


def prune(cache, keep):
    """the cache over CACHE_MAX_BYTES: the least recently played / made go
    (kraken_doa touches a sidecar when it plays it)"""
    if WINDOWS:
        for f in os.listdir(cache):   # replace()'s leftovers, once let go
            if '.old-' in f:
                try:
                    os.remove(os.path.join(cache, f))
                except OSError:
                    pass
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
    if WINDOWS:
        # kraken_doa splits the PROGRESS / ERROR lines at \n: no \r\n
        sys.stdout.reconfigure(newline='\n')
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
    if WINDOWS:
        # a Windows Python seldom has them: say so on the page (kraken_doa
        # shows the ERROR line), not in a traceback in the log
        # (scipy: uca_scene.py and most generators written in Python)
        need = ['numpy'] + (['scipy'] if 'scene' in e or any(str(st[0]).endswith('.py') for st in e.get('steps', [])) else [])
        missing = [m for m in need if importlib.util.find_spec(m) is None]
        if missing:
            fail('Python needs %s for test transmissions: "%s" -m pip install %s'
                 % (' + '.join(missing), sys.executable, ' '.join(missing)))
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
            base = json.load(open(content.pop('$from'), encoding='utf-8'))
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
    # the old ones may be playing (kraken_doa has them mapped / open)
    replace(dat, os.path.join(cache, ident + '.dat'))
    replace(os.path.join(cache, ident + '.json.tmp'), os.path.join(cache, ident + '.json'))
    shutil.rmtree(work, ignore_errors=True)
    log.close()
    prune(cache, ident)
    progress(100, 'done in %.0f s, %.0f MB' % (time.time() - t0, os.path.getsize(os.path.join(cache, ident + '.dat')) / 1e6))
    print('DONE', flush=True)


if __name__ == '__main__':
    main()
