// Validation of the 📶 path loss engine (kraken_doa.html, the viewshed
// worker vsWorkerMain: itmP2P, p1812P2P, p2108*, plSweep, runLink) in node:
//
//   node tools/pathloss_validate.mjs [--refs DIR] [--no-refs] [--net]
//
// 1 ITM / Longley-Rice vs NTIA's published point-to-point cases
//   (github.com/NTIA/itm: p2p.csv + pfls.csv)
// 2 ITU-R P.1812 vs the intermediate values of the ITU-R SG3 validation logs
//   (github.com/eeveetza/Py1812, tests/: Bullington, spherical Earth,
//   smooth-Earth heights, horizons, troposcatter, Lb)
// 3 ITU-R P.2108 3.3 vs its validation table (github.com/eeveetza/Py2108)
// 4 the coverage sweep vs the exact P.1812 on the same ray profiles, on
//   synthetic terrain (generated terrarium tiles - no network): beyond and
//   in line of sight, sea + land, above ground / above sea level targets
// 5 a link vs the coverage at the same cells; sanity checks (flat sea =
//   plane-Earth loss, free space <= P.1812, clutter)
// 6 (--net) 4 on real terrain (AWS Terrain Tiles) at Auckland
// 1-3 clone the reference repositories into --refs (default: a temp dir;
// git needed) unless already there; --no-refs skips them.
import fs from 'fs';
import os from 'os';
import path from 'path';
import zlib from 'zlib';
import {execFileSync} from 'child_process';

const HERE = path.dirname(new URL(import.meta.url).pathname);
const html = fs.readFileSync(path.join(HERE, '..', 'kraken_doa.html'), 'utf8');
const args = process.argv.slice(2), net = args.includes('--net'), noRefs = args.includes('--no-refs');
const refs = args.includes('--refs') ? args[args.indexOf('--refs') + 1] : path.join(os.tmpdir(), 'kraken_pathloss_refs');
let fails = 0;
const check = (c, what) => { console.log((c ? 'PASS ' : 'FAIL ') + what); if (!c) fails++; };
const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(p / 100 * s.length))] : 0; };

// ---- synthetic terrain, served as terrarium PNGs through fetch ----
const realFetch = globalThis.fetch;
const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = b => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (type, data) => {
  const out = Buffer.alloc(12 + data.length), t = Buffer.from(type, 'ascii');
  out.writeUInt32BE(data.length, 0); t.copy(out, 4); data.copy(out, 8);
  out.writeUInt32BE(crc32(Buffer.concat([t, data])), 8 + data.length);
  return out;
};
// heights (m; < 0 = sea floor) at lat / lon: a coast with hills, a ridge and a valley
const synthH = (la, lo) => {
  const x = (lo - 174.5) * 88, y = (la + 36.9) * 111;   // km from 36.9 S 174.5 E
  let h = 40 + 25 * x - 2 * y;   // rising inland to the east, the sea to the west
  h += 350 * Math.exp(-((x - 12) ** 2 + (y - 4) ** 2) / 18);   // a hill
  h += 220 * Math.exp(-((x - 0.45 * y - 25) ** 2) / 6) * (0.6 + 0.4 * Math.sin(y / 3));   // a ridge
  h -= 90 * Math.exp(-((x - 18) ** 2) / 4);   // a valley
  h += 15 * Math.sin(x * 1.7) * Math.sin(y * 1.3) + 4 * Math.sin(x * 9.1 + y * 7.3);   // roughness
  return x < -6 ? Math.min(h, -20 - 5 * (-6 - x)) : h;
};
let synthServed = 0;
const synthFetch = async url => {
  const m = String(url).match(/terrarium\/(\d+)\/(\d+)\/(\d+)\.png$/);
  if (!m) return realFetch(url);
  const [z, tx, ty] = m.slice(1).map(Number), W = 256 * 2 ** z, raw = Buffer.alloc(256 * (1 + 256 * 3));
  for (let r = 0; r < 256; r++) {
    const la = Math.atan(Math.sinh(Math.PI - 2 * Math.PI * (ty * 256 + r + 0.5) / W)) * 180 / Math.PI;
    for (let c = 0; c < 256; c++) {
      const lo = (tx * 256 + c + 0.5) / W * 360 - 180, v = synthH(la, lo) + 32768, o = r * 769 + 1 + c * 3;
      raw[o] = Math.floor(v / 256); raw[o + 1] = Math.floor(v) & 255; raw[o + 2] = Math.floor((v % 1) * 256);
    }
  }
  const ih = Buffer.alloc(13);
  ih.writeUInt32BE(256, 0); ih.writeUInt32BE(256, 4); ih[8] = 8; ih[9] = 2;
  synthServed++;
  return new Response(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
};

// ---- the worker ----
const a = html.indexOf('  function vsWorkerMain() {'), b = html.indexOf('  // (end of vsWorkerMain)');
const msgs = [];
globalThis.self = {postMessage: m => msgs.push(m)};
new Function(html.slice(a, b) + '\nvsWorkerMain();')();
const T = self.vsTest;
const pz = html.indexOf('  function vsPlan(');
const vsPlan = new Function('VS_RE', 'VS_ZFINE', 'VS_ZSTEP', 'VS_NMAX', html.slice(pz, html.indexOf('\n  }\n', pz) + 4) + '; return vsPlan;')(
  6371000, +html.match(/VS_ZFINE = (\d+)/)[1], +html.match(/VS_ZSTEP = (\d+)/)[1], +html.match(/VS_NMAX = (\d+)/)[1]);
let job = 0;
const runMsg = async m => {
  msgs.length = 0;
  m.job = ++job;
  await T.run(m);
  return msgs.find(x => x.job === m.job && (x.levels || x.link || x.error));
};

// ---- 1-3: the references ----
const refRepo = (name, url) => {
  const dir = path.join(refs, name);
  if (!fs.existsSync(dir)) {
    fs.mkdirSync(refs, {recursive: true});
    try { execFileSync('git', ['clone', '-q', '--depth', '1', url, dir], {stdio: 'inherit'}); }
    catch (e) { console.log(`(can't clone ${url}: ${e.message})`); return null; }
  }
  return dir;
};
if (!noRefs) {
  const itm = refRepo('itm', 'https://github.com/NTIA/itm.git');
  if (itm) {
    const rows = fs.readFileSync(path.join(itm, 'p2p.csv'), 'utf8').trim().split(/\r?\n/).slice(1).map(l => l.split(',').map(Number));
    const pfls = fs.readFileSync(path.join(itm, 'pfls.csv'), 'utf8').trim().split(/\r?\n/).map(l => l.split(',').filter(s => s !== '').map(Number));
    let worst = 0;
    rows.forEach((r, i) => {
      const [htx, hrx, eps, sigma, N0, f, pol, climate, time, loc, sit, mdvar, A] = r;
      const res = T.itmP2P(pfls[i], htx, hrx, {climate, N0, f, pol, eps, sigma, mdvar, time, loc, sit});
      worst = Math.max(worst, res.err ? Infinity : Math.abs(res.A - A));
    });
    check(worst < 0.006, `1 ITM: NTIA's ${rows.length} point-to-point cases (LOS, diffraction, troposcatter) within ${worst.toFixed(4)} dB (given to 0.01)`);
  }
  const py = refRepo('Py1812', 'https://github.com/eeveetza/Py1812.git');
  if (py) {
    const D = path.join(py, 'tests');
    const logs = fs.readdirSync(path.join(D, 'validation_results')).filter(f => f.endsWith('_log.csv')).sort();
    let worst = 0, bad = 0;
    for (const lf of logs) {
      const all = fs.readFileSync(path.join(D, 'validation_results', lf), 'utf8').split(/\r?\n/).map(l => l.split(','));
      const kv = {};
      for (const c of all) if (c.length >= 4 && c[3] !== '' && kv[c[0].trim()] === undefined) kv[c[0].trim()] = +c[3];
      const lines = fs.readFileSync(path.join(D, 'validation_profiles', lf.replace(/_\d+_log\.csv$/, '.csv')), 'utf8').split(/\r?\n/);
      const s = lines.findIndex(l => l.startsWith('{Begin of Profile}')), np = +lines[s + 1].split(',')[1], d = [], h = [], R = [];
      for (let i = 0; i < np; i++) { const c = lines[s + 2 + i].split(','); d.push(+c[0]); h.push(+c[1]); R.push(c[3] ? +c[3] : 0); }
      const args1812 = ae => T.p1812P2P(Float64Array.from(d), Float64Array.from(h), Float64Array.from(R), kv['htg (m)'], kv['hrg (m)'], kv['f (GHz)'], ae, kv['w'], kv['N0'], kv['pol'] - 1);
      const r = args1812(kv['ae (km)']), rb = args1812(3 * 6371);   // (Py1812 logs Lbulla / Lbulls / Ldsph of its beta0-radius run)
      const cmp = [['Lbfs', r.Lbfs], ['Ld50 (dB)', r.Ld], ['Ldb (dB)', rb.Ld], ['Lbulla (dB)', rb.Lbulla], ['Lbulls (dB)', rb.Lbulls], ['Ldsph (dB)', rb.Ldsph],
                   ['th_t (mrad)', r.sh.tt], ['th_r (mrad)', r.sh.tr], ['th (mrad)', r.sh.theta], ['dlt (km)', r.sh.dlt], ['dlr (km)', r.sh.dlr],
                   ['hstd (m)', r.sh.hstd], ['hsrd (m)', r.sh.hsrd]];
      if (kv['p (%)'] === 50) cmp.push(['Lbs (dB)', r.Lbs], ['Lb (dB)', r.Lb]);
      for (const [k, v] of cmp) {
        if (kv[k] === undefined) continue;
        const e = Math.abs(v - kv[k]);
        worst = Math.max(worst, e);
        if (e > 1e-6 * Math.max(1, Math.abs(kv[k]))) { bad++; console.log('   ', lf, k, v, kv[k]); }
      }
    }
    check(!bad && logs.length > 50, `2 P.1812: ${logs.length} ITU-R SG3 validation logs, every intermediate value within ${worst.toExponential(1)}`);
  }
  const p21 = refRepo('Py2108', 'https://github.com/eeveetza/Py2108.git');
  if (p21) {
    const rows = fs.readFileSync(path.join(p21, 'tests/validation_examples/validation_example_p2108_3.csv'), 'utf8').trim().split(/\r?\n/).slice(1).map(l => l.split(',').map(Number));
    let worst = 0;
    for (const [f, th, p, h, hm, L] of rows) worst = Math.max(worst, Math.abs(T.p2108Aero(f, th, p, h, hm) - L));
    check(worst < 1e-4, `3 P.2108 3.3: ${rows.length} rows within ${worst.toExponential(1)} dB`);
  }
}

// ---- 4 + 6: the coverage sweep vs the exact P.1812 on its own profiles ----
async function sweepVsExact(name, lat, lon, h, R, sig) {
  const m = {lat, lon, h, k: 4 / 3, levels: vsPlan(lat, R * 1000), sig, debug: {rays: [5, 811, 2003, 3333, 4099]}};
  const t0 = Date.now(), res = await runMsg(m);
  if (res.error) { check(false, `${name}: ${res.error}`); return null; }
  const ae = 4 / 3 * 6371, errIn = [], errOut = [];
  let nan = 0, n0 = 0;
  for (const r of res.sig.rays) {
    const n = r.s.length;
    for (let j = 1; j < n; j += Math.max(1, Math.floor(n / 120))) {
      if (r.L[j] < 0) { n0++; continue; }   // (the target under the ground)
      const d = [0], hh = [res.gnd];
      for (let i = 0; i < j; i++) if (r.add[i]) { d.push(r.s[i] / 1000); hh.push(r.g[i]); }
      d.push(r.s[j] / 1000); hh.push(r.g[j]);
      if (d.length < 3) continue;
      const hr = sig.amsl ? sig.tgt - r.g[j] : sig.tgt;
      if (hr < 0.5) continue;   // (the sweep lifts a target below 0.5 m to 0.5 m)
      const ex = T.p1812P2P(Float64Array.from(d), Float64Array.from(hh), null, h, hr, sig.f / 1000, ae, r.sea[j], sig.N0, sig.pol, true);
      let cl = 0;
      if (sig.amsl) { if (sig.hmTx > 0) cl = T.p2108Aero(sig.f / 1000, Math.max(0, Math.atan((hh[hh.length - 1] + hr - res.eye) / (d[d.length - 1] * 1000) - d[d.length - 1] * 1000 / (2 * ae * 1000)) * 180 / Math.PI), 50, Math.max(1, h), sig.hmTx); }
      else cl = T.p2108Terminal(sig.f / 1000, h, sig.envTx) + T.p2108Terminal(sig.f / 1000, Math.max(0.5, sig.tgt), sig.envRx);
      const e = Math.abs(r.L[j] - (ex.Lb + cl));
      if (!Number.isFinite(r.L[j])) nan++;
      (ex.bull.los ? errIn : errOut).push(e);
    }
  }
  let cells = 0, badCells = 0;
  for (const l of res.levels) {
    const loss = new Uint16Array(l.loss), need = new Uint16Array(l.need);
    for (let i = 0; i < loss.length; i++) if (need[i] !== 65535) { cells++; if (loss[i] === 0) badCells++; }
  }
  const worst = Math.max(pct(errIn, 100), pct(errOut, 100));
  check(worst < 1e-6 && !nan && cells > 1e5 && !badCells,
        `${name}: sweep = exact P.1812 on ${errIn.length} line-of-sight + ${errOut.length} beyond-horizon samples (worst ${worst.toExponential(1)} dB)` +
        `${n0 ? `, ${n0} under the ground skipped` : ''}; ${cells} cells, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  return res;
}
globalThis.fetch = synthFetch;
const sig0 = {f: 446, tgt: 2, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0};
const cov = await sweepVsExact('4 synthetic coast 25 km, 446 MHz, 2 m target', -36.88, 174.62, 10, 25, sig0);
await sweepVsExact('4 synthetic, 146 MHz, horizontal, clutter suburban / urban', -36.88, 174.62, 8, 12, {f: 146, tgt: 1.5, amsl: false, pol: 0, N0: 320, envTx: 3, envRx: 4, hmTx: 0});
await sweepVsExact('4 synthetic, aircraft at 1500 m above sea level, P.2108 3.3', -36.88, 174.62, 10, 25, {f: 1090, tgt: 1500, amsl: true, pol: 1, N0: 301, envTx: 3, envRx: 0, hmTx: 8});

// ---- 5: links ----
if (cov) {
  const D2R = Math.PI / 180, diffs = [];
  let seed = 3;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  for (let t = 0; t < 30; t++) {
    const k = Math.floor(rnd() * cov.levels.length), l = cov.levels[k], N = l.N, loss = new Uint16Array(l.loss);
    let i, j;
    for (let tries = 0; tries < 2000; tries++) { i = Math.floor(rnd() * N); j = Math.floor(rnd() * N); if (loss[j * N + i] !== 65535) break; }
    const W = 256 * 2 ** l.z, la = Math.atan(Math.sinh(Math.PI - 2 * Math.PI * (l.gy0 + j + 0.5) / W)) / D2R, lo = (l.gx0 + i + 0.5) / W * 360 - 180;
    const r = await runMsg({link: {a: {lat: -36.88, lon: 174.62, h: 10}, b: {lat: la, lon: lo, h: 2, amsl: false}, f: 446, k: 4 / 3, pol: 1, N0: 301,
                                   climate: 5, eps: 15, sigma: 0.005, envA: 0, envB: 0, hmA: 0, rel: 90}});
    if (r.error) { check(false, 'link: ' + r.error); continue; }
    diffs.push(Math.abs(loss[j * N + i] / 10 - r.link.p1812.Lb));
  }
  check(pct(diffs, 50) < 1.5, `5 a link to a coverage cell's centre vs the cell (same model, other sampling): |diff| median ${pct(diffs, 50).toFixed(2)}, p90 ${pct(diffs, 90).toFixed(2)} dB`);
}
const sea = await runMsg({link: {a: {lat: -36.95, lon: 174.40, h: 10}, b: {lat: -36.95, lon: 174.288, h: 10, amsl: false}, f: 146, k: 4 / 3, pol: 1, N0: 301,
                                 climate: 7, eps: 80, sigma: 5, envA: 0, envB: 0, hmA: 0, rel: 90}});
if (sea.error) check(false, 'sea link: ' + sea.error);
else {
  const L = sea.link, plane = 40 * Math.log10(L.D) - 20 * Math.log10(10 * 10);   // plane-Earth loss (far beyond the two-ray breakpoint)
  check(L.sea > 0.99 && Math.abs(L.p1812.Lb - plane) < 2 && Math.abs(L.itm.A - plane) < 3,
        `5 flat sea 10 km, 10 m masts, 146 MHz: P.1812 ${L.p1812.Lb.toFixed(1)}, ITM ${L.itm.A.toFixed(1)} dB vs plane Earth ${plane.toFixed(1)} dB`);
  check(L.p1812.Lb >= L.fs && L.clut.a === 0 && L.clut.b === 0, '5 P.1812 >= free space; no clutter for environment "none"');
}
// (A stands on the hill's flank, 555 m: the hill hides an aircraft at 3000 m 35 km NE - 8000 m is in sight)
const air = await runMsg({link: {a: {lat: -36.88, lon: 174.62, h: 3}, b: {lat: -36.70, lon: 174.95, h: 8000, amsl: true}, f: 1090, k: 4 / 3, pol: 1, N0: 301,
                                 climate: 5, eps: 15, sigma: 0.005, envA: 3, envB: 3, hmA: 8, rel: 90}});
check(!air.error && air.link.clut.aero && air.link.clut.b === 0 && air.link.clut.a > 0 && air.link.p1812.los,
      `5 aircraft at 8000 m: in sight, air-to-ground clutter at the ground end only (${air.error || air.link.clut.a.toFixed(1) + ' dB at ' + air.link.elevA.toFixed(1) + ' deg'})`);
console.log(`(${synthServed} synthetic tiles served)`);

if (net) {
  globalThis.fetch = realFetch;
  await sweepVsExact('6 Auckland 30 km, real terrain, 146 MHz', -36.85, 174.76, 10, 30, {f: 146, tgt: 2, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0});
}
console.log(fails ? `${fails} FAILED` : 'ALL PASS');
process.exit(fails ? 1 : 0);
