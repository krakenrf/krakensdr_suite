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
// 4 the coverage sweep vs the exact model on the same ray profiles, on
//   synthetic terrain (generated terrarium tiles - no network): P.1812
//   beyond and in line of sight, sea + land, above ground / above sea level
//   targets; Longley-Rice (its distance-based steps = NTIA's ITM on evenly
//   spaced profiles, the sweep = them, vs NTIA's ITM resampled); free
//   space; Hata
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

// ---- the coverage's Longley-Rice, straight from its definition: NTIA's ITM
// 1.2.2 point to point on the points P ([[s m, z m], ...], TX at s = 0 ..
// RX) whatever their spacing - its steps in profile indices taken in
// distance (the mean height of the middle 80 % of the points, the horizons,
// delta h resampled by distance, trapezoid-weighted fits); LongleyRice /
// Variability are the worker's (checked in 1). Plain loops, no prefix sums,
// envelopes or hulls (the sweep's way of getting the same numbers)
function itmRay(P, h0, h1, o) {
  const np = P.length - 1, s = P.map(p => p[0]), z = P.map(p => p[1]), d = s[np];
  const p10 = Math.trunc(0.1 * np);
  let hs = 0;
  for (let i = p10; i <= np - p10; i++) hs += z[i];
  hs /= np - 2 * p10 + 1;
  const Ns = hs === 0 ? o.N0 : o.N0 * Math.exp(-hs / 9460), ge = 157e-9 * (1 - 0.04665 * Math.exp(Ns / 179.3)), ae = 1 / ge;
  const zt = z[0] + h0, zr = z[np] + h1, th = [(zr - zt) / d - d / (2 * ae), -(zr - zt) / d - d / (2 * ae)], dhz = [d, d];
  for (let i = 1; i < np; i++) {
    const a = (z[i] - zt) / s[i] - s[i] / (2 * ae), b = -(zr - z[i]) / (d - s[i]) - (d - s[i]) / (2 * ae);
    if (a > th[0]) { th[0] = a; dhz[0] = s[i]; }
    if (b > th[1]) { th[1] = b; dhz[1] = d - s[i]; }
  }
  const last = x => { let k = 0; while (k < np && s[k + 1] <= x) k++; return k; };   // the last point at or before x
  const seg = x => Math.min(last(x), np - 1);
  const fidx = x => { const k = seg(x); return k + (x - s[k]) / (s[k + 1] - s[k]); };
  const zAt = x => { const k = seg(x); return z[k] + (z[k + 1] - z[k]) * (x - s[k]) / (s[k + 1] - s[k]); };
  const deltaH = (d0, d1) => {
    const xs = fidx(d0), xe = fidx(d1);
    if (xe - xs < 2) return 0;
    const q10 = Math.min(Math.max(4, Math.trunc(0.1 * (xe - xs + 8))), 25), n = 10 * q10 - 5, nps = n - 1, v = [];
    for (let j = 0; j < n; j++) v.push(zAt(d0 + j * (d1 - d0) / nps));
    let W = 0, mx = 0, mz = 0, xx = 0, xz = 0;   // least squares over j, trapezoid weights
    v.forEach((y, j) => { const w = j === 0 || j === nps ? 0.5 : 1; W += w; mx += w * j; mz += w * y; xx += w * j * j; xz += w * j * y; });
    mx /= W; mz /= W;
    const b = (xz / W - mx * mz) / (xx / W - mx * mx), r = v.map((y, j) => y - (mz + b * (j - mx))).sort((p, q) => p - q);
    return (r[n - q10] - r[q10 - 1]) / (1 - 0.8 * Math.exp(-(d1 - d0) / 50e3));
  };
  const lsq = (d0, d1) => {
    let is = d0 <= 0 ? 0 : last(d0), ie = d1 >= d ? np : last(d1);
    if (ie < np && s[ie] < d1) ie++;
    if (ie <= is) { is = Math.max(is - 1, 0); ie = Math.min(ie + 1, np); }
    let W = 0, S1 = 0, S2 = 0, Z = 0, ZS = 0;
    for (let i = is; i < ie; i++) {
      const h2 = (s[i + 1] - s[i]) / 2;
      W += 2 * h2; S1 += h2 * (s[i] + s[i + 1]); S2 += h2 * (s[i] ** 2 + s[i + 1] ** 2); Z += h2 * (z[i] + z[i + 1]); ZS += h2 * (z[i] * s[i] + z[i + 1] * s[i + 1]);
    }
    const ms = S1 / W, mz = Z / W, b = (ZS / W - ms * mz) / (S2 / W - ms * ms);
    return [mz - b * ms, mz + b * (d - ms)];
  };
  const h = [h0, h1], q = T.itmQuickRest(th, dhz, h, d, ae, z[0], z[np], deltaH, lsq);
  const lr = T.itmLongleyRice(q.th, o.f, T.itmZg(o.eps, o.sigma, o.f, o.pol), q.dhz, q.he, ge, Ns, q.dh, h, d);
  if (lr.err) return null;
  return T.itmVariability(o.time, o.loc, o.sit, q.he, q.dh, o.f, d, lr.A, o.climate, o.mdvar).A + T.itmFreeSpace(d, o.f);
}
// it is NTIA's ITM on an evenly spaced profile
{
  let seed = 11, worst = 0, n = 0;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const cases = [];
  for (let t = 0; t < 300; t++) {
    const np = 2 + Math.floor(rnd() ** 2 * 1500), xi = 5 + rnd() * 200, pfl = new Float64Array(np + 3);
    pfl[0] = np; pfl[1] = xi;
    let hh = rnd() * 400 - 50;
    for (let i = 0; i <= np; i++) { hh += (rnd() - 0.5) * 30 + 8 * Math.sin(i / 37); pfl[i + 2] = Math.max(0, hh) * (rnd() < 0.02 ? 1.5 : 1); }
    cases.push([pfl, 0.5 + rnd() ** 2 * 300, 0.5 + rnd() ** 2 * 300,
                {climate: 1 + Math.floor(rnd() * 7), N0: 260 + rnd() * 100, f: 30 + rnd() ** 2 * 5000, pol: rnd() < 0.5 ? 0 : 1, eps: 4 + rnd() * 70,
                 sigma: 0.001 + rnd() * 5, mdvar: [0, 1, 2, 3, 12][Math.floor(rnd() * 5)], time: 1 + rnd() * 98, loc: 1 + rnd() * 98, sit: 1 + rnd() * 98}]);
  }
  for (const [pfl, h0, h1, o] of cases) {
    const ref = T.itmP2P(pfl, h0, h1, o);
    if (ref.err) continue;
    const P = [];
    for (let i = 0; i <= pfl[0]; i++) P.push([i * pfl[1], pfl[i + 2]]);
    const A = itmRay(P, h0, h1, o);
    worst = Math.max(worst, A === null ? Infinity : Math.abs(A - ref.A));
    n++;
  }
  check(worst < 1e-6 && n > 250, `4 Longley-Rice on uneven points = NTIA's ITM on ${n} random evenly spaced profiles (worst ${worst.toExponential(1)} dB)`);
}

// ---- 4 + 6: the coverage sweep vs the exact model on its own profiles ----
// (sig.model: p1812 (default), itm, fs, hata; txAmsl: the transmitter's h is above sea level - in the air)
const HATA_ENV = [, 'open', 'open', 'suburban', 'urban', 'large'];
async function sweepVsExact(name, lat, lon, h, R, sig, txAmsl) {
  const m = {lat, lon, h, amsl: !!txAmsl, k: 4 / 3, levels: vsPlan(lat, R * 1000), sig, debug: {rays: [5, 811, 2003, 3333, 4099]}};
  const t0 = Date.now(), res = await runMsg(m);
  if (res.error) { check(false, `${name}: ${res.error}`); return null; }
  if (txAmsl) h = res.eye - res.gnd;   // (the TX above its ground, as the exact model takes it)
  const ae = 4 / 3 * 6371, errIn = [], errOut = [], model = sig.model || 'p1812', vsEven = [];
  let nan = 0, n0 = 0, nOut = 0;
  for (const r of res.sig.rays) {
    const n = r.s.length;
    for (let j = 1; j < n; j += Math.max(1, Math.floor(n / 120))) {
      if (r.L[j] === -2) { nOut++; continue; }   // (outside the model's range)
      if (r.L[j] < 0) { n0++; continue; }   // (the target under the ground)
      const d = [0], hh = [res.gnd];
      for (let i = 0; i < j; i++) if (r.add[i]) { d.push(r.s[i] / 1000); hh.push(r.g[i]); }
      d.push(r.s[j] / 1000); hh.push(r.g[j]);
      if (d.length < 3) continue;
      const hr = sig.amsl ? sig.tgt - r.g[j] : sig.tgt;
      if (hr < 0.5) continue;   // (the sweep lifts a target below 0.5 m to 0.5 m)
      const sm = d[d.length - 1] * 1000, hrs = hh[hh.length - 1] + hr, htg = Math.max(0.5, h);
      let ex;
      if (model === 'p1812') ex = T.p1812P2P(Float64Array.from(d), Float64Array.from(hh), null, h, hr, sig.f / 1000, ae, r.sea[j], sig.N0, sig.pol, true);
      else if (model === 'fs') ex = {Lb: 32.45 + 20 * Math.log10(sig.f) + 10 * Math.log10((sm / 1000) ** 2 + ((res.eye - hrs) / 1000) ** 2), bull: {los: true}};
      else if (model === 'hata') ex = {Lb: T.hataLoss(sig.f, sm / 1000, Math.max(htg, hr), Math.min(htg, hr), HATA_ENV[htg >= hr ? sig.envRx : sig.envTx] || 'open'), bull: {los: true}};
      else {
        const o = {N0: sig.N0, f: sig.f, pol: sig.pol, eps: sig.eps, sigma: sig.sigma, climate: sig.climate, mdvar: 12, time: 50, loc: 50, sit: 50};
        const P = d.map((x, i) => [x * 1000, hh[i]]), A = itmRay(P, htg, hr, o);
        if (A === null) { nOut++; continue; }
        ex = {Lb: A, bull: {los: true}};
        // the same path evenly resampled (10 m) through NTIA's ITM: how much the uneven points change it
        if (vsEven.length < 150 && sm > 2000) {
          const ne = Math.ceil(sm / 10), pfl = new Float64Array(ne + 3);
          pfl[0] = ne; pfl[1] = sm / ne;
          let k = 0;
          for (let i = 0; i <= ne; i++) {
            const x = i * sm / ne;
            while (k < P.length - 2 && P[k + 1][0] < x) k++;
            pfl[i + 2] = P[k][1] + (P[k + 1][1] - P[k][1]) * (x - P[k][0]) / (P[k + 1][0] - P[k][0]);
          }
          const ev = T.itmP2P(pfl, htg, hr, o);
          if (!ev.err) vsEven.push(Math.abs(ev.A - A));
        }
      }
      // clutter: both on the ground 3.1; the RX up: 3.3 at the TX; the TX up: 3.3 at the RX (by the elevation at that end); none with Hata
      let cl = 0;
      const el = (from, to) => Math.max(0, Math.atan((to - from) / sm - sm / (2 * ae * 1000)) * 180 / Math.PI);
      if (model === 'hata') cl = 0;
      else if (sig.amsl && !txAmsl) { if (sig.hmTx > 0) cl = T.p2108Aero(sig.f / 1000, el(res.eye, hrs), 50, Math.max(1, h), sig.hmTx); }
      else if (txAmsl && !sig.amsl) { if (sig.hmRx > 0) cl = T.p2108Aero(sig.f / 1000, el(hrs, res.eye), 50, Math.max(1, hr), sig.hmRx); }
      else if (!sig.amsl) cl = T.p2108Terminal(sig.f / 1000, h, sig.envTx) + T.p2108Terminal(sig.f / 1000, Math.max(0.5, sig.tgt), sig.envRx);
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
  const worst = Math.max(pct(errIn, 100), pct(errOut, 100)), MN = {p1812: 'P.1812', itm: 'Longley-Rice', fs: 'free space', hata: 'Hata'}[model];
  check(worst < 1e-6 && !nan && cells > 1e5 && !badCells && errIn.length + errOut.length > 300,
        `${name}: sweep = exact ${MN} on ` + (model === 'p1812' ? `${errIn.length} line-of-sight + ${errOut.length} beyond-horizon samples` : `${errIn.length} samples`) +
        ` (worst ${worst.toExponential(1)} dB)${n0 ? `, ${n0} under the ground skipped` : ''}${nOut ? `, ${nOut} outside the model's range` : ''}; ${cells} cells, ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  if (vsEven.length) check(pct(vsEven, 50) < 0.5, `${name}: Longley-Rice on the ray's own points vs NTIA's ITM on them resampled every 10 m: |diff| median ` +
                           `${pct(vsEven, 50).toFixed(2)}, p90 ${pct(vsEven, 90).toFixed(2)}, max ${pct(vsEven, 100).toFixed(2)} dB (${vsEven.length} paths)`);
  return res;
}
globalThis.fetch = synthFetch;
const sig0 = {f: 446, tgt: 2, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0};
const cov = await sweepVsExact('4 synthetic coast 25 km, 446 MHz, 2 m target', -36.88, 174.62, 10, 25, sig0);
await sweepVsExact('4 synthetic, 146 MHz, horizontal, clutter suburban / urban', -36.88, 174.62, 8, 12, {f: 146, tgt: 1.5, amsl: false, pol: 0, N0: 320, envTx: 3, envRx: 4, hmTx: 0});
await sweepVsExact('4 synthetic, aircraft at 1500 m above sea level, P.2108 3.3', -36.88, 174.62, 10, 25, {f: 1090, tgt: 1500, amsl: true, pol: 1, N0: 301, envTx: 3, envRx: 0, hmTx: 8});
await sweepVsExact('4 synthetic, a drone TX at 900 m above sea level, receivers 1.5 m in suburbs (3.3 at them)', -36.88, 174.62, 900, 15,
                   {f: 2400, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 3, hmTx: 0, hmRx: 8}, true);
const sigItm = {f: 446, tgt: 2, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0, model: 'itm', climate: 5, eps: 15, sigma: 0.005};
const covItm = await sweepVsExact('4 synthetic coast 15 km, Longley-Rice, 446 MHz', -36.88, 174.62, 10, 15, sigItm);
await sweepVsExact('4 synthetic, Longley-Rice, horizontal, sea water, aircraft at 1500 m above sea level (3.3)', -36.88, 174.62, 10, 10,
                   {f: 1090, tgt: 1500, amsl: true, pol: 0, N0: 320, envTx: 3, envRx: 0, hmTx: 8, model: 'itm', climate: 7, eps: 81, sigma: 5});
await sweepVsExact('4 synthetic, free space + clutter suburban / urban', -36.88, 174.62, 8, 12, {f: 146, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 3, envRx: 4, hmTx: 0, model: 'fs'});
await sweepVsExact('4 synthetic, Hata (the lower end\'s environment)', -36.88, 174.62, 40, 12, {f: 146, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 3, envRx: 4, hmTx: 0, model: 'hata'});

// ---- 5: links ----
// a link from the site to cells' centres vs the cells (same model, other sampling); pick(link) -> its loss
async function linkVsCells(cov, sig, pick, label, lim) {
  const D2R = Math.PI / 180, diffs = [];
  let seed = 3;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  for (let t = 0; t < 30; t++) {
    const k = Math.floor(rnd() * cov.levels.length), l = cov.levels[k], N = l.N, loss = new Uint16Array(l.loss);
    let i, j;
    for (let tries = 0; tries < 2000; tries++) { i = Math.floor(rnd() * N); j = Math.floor(rnd() * N); if (loss[j * N + i] < 65534) break; }
    const W = 256 * 2 ** l.z, la = Math.atan(Math.sinh(Math.PI - 2 * Math.PI * (l.gy0 + j + 0.5) / W)) / D2R, lo = (l.gx0 + i + 0.5) / W * 360 - 180;
    const r = await runMsg({link: {a: {lat: -36.88, lon: 174.62, h: 10}, b: {lat: la, lon: lo, h: sig.tgt, amsl: false}, f: sig.f, k: 4 / 3, pol: sig.pol, N0: sig.N0,
                                   climate: sig.climate || 5, eps: sig.eps || 15, sigma: sig.sigma || 0.005, envA: 0, envB: 0, hmA: 0, rel: 90}});
    if (r.error) { check(false, 'link: ' + r.error); continue; }
    const v = pick(r.link);
    if (v !== null) diffs.push(Math.abs(loss[j * N + i] / 10 - v));
  }
  check(pct(diffs, 50) < lim, `5 ${label}: a link to a coverage cell's centre vs the cell (same model, other sampling): |diff| median ` +
        `${pct(diffs, 50).toFixed(2)}, p90 ${pct(diffs, 90).toFixed(2)} dB (${diffs.length})`);
}
if (cov) await linkVsCells(cov, sig0, L => L.p1812.Lb, 'P.1812', 1.5);
if (covItm) await linkVsCells(covItm, sigItm, L => L.itm.err ? null : L.itm.A, 'Longley-Rice', 2);
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
      `5 RX aircraft at 8000 m: in sight, air-to-ground clutter at the ground end only (${air.error || air.link.clut.a.toFixed(1) + ' dB at ' + air.link.elevA.toFixed(1) + ' deg'})`);
// the same path the other way round: the aircraft transmits - the same loss (reciprocal), the clutter at the RX
const air2 = await runMsg({link: {a: {lat: -36.70, lon: 174.95, h: 8000, amsl: true}, b: {lat: -36.88, lon: 174.62, h: 3, amsl: false}, f: 1090, k: 4 / 3, pol: 1, N0: 301,
                                  climate: 5, eps: 15, sigma: 0.005, envA: 3, envB: 3, hmA: 8, hmB: 8, rel: 90}});
check(!air2.error && air2.link.clut.a === 0 && Math.abs(air2.link.clut.b - air.link.clut.a) < 1e-9 && Math.abs(air2.link.p1812.Lb - air.link.p1812.Lb) < 0.5,
      `5 TX aircraft at 8000 m, the other way: clutter at the RX ${air2.error || air2.link.clut.b.toFixed(1)} dB, P.1812 ${air2.error ? '' : air2.link.p1812.Lb.toFixed(2) + ' vs ' + air.link.p1812.Lb.toFixed(2)} dB`);
console.log(`(${synthServed} synthetic tiles served)`);

if (net) {
  globalThis.fetch = realFetch;
  await sweepVsExact('6 Auckland 30 km, real terrain, 146 MHz', -36.85, 174.76, 10, 30, {f: 146, tgt: 2, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0});
}
console.log(fails ? `${fails} FAILED` : 'ALL PASS');
process.exit(fails ? 1 : 0);
