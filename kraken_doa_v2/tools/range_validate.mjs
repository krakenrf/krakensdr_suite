// Validation of the 📏 range from signal strength (kraken_doa.html "📏 RANGE")
// in node - the page's own code: the level's pdf (rgLik, from the 📶 path
// loss's reliability tables), the lobe's bearing factor (rgAng), the
// station's sweep (the viewshed worker vsWorkerMain, synthetic terrain as in
// pathloss_validate.mjs - no network) and the posterior worker (rgPostMain):
//
//   node tools/range_validate.mjs
//
// 1 the level's pdf: integrates to 1; no fading = the normal pdf; Rayleigh:
//   the measured level 2.51 dB below the local mean on average (in dB)
// 2 the bearing factor: peak at the lobe's, the floor eps (one lobe = one
//   observation: not tempered), the ratio peak / floor
// 3 free space, an exact level, a lobe at 60 deg: the most likely place = the
//   true transmitter; the distance's quantiles = a posterior worked out
//   independently (plain loops) from the same cells
// 4 calibration (Monte Carlo, ITU-R P.1812 over the synthetic hills, a flat
//   lobe): transmitters uniform over the area, levels from the model ->
//   the true place inside the 50 / 90 % regions ~50 / 90 % of the time
// 5 auto fading (K per cell) runs; levels no place fits (too strong / weak)
// 6 the station's sweep (the station as the worker's "Tx" end, the clutter
//   roles swapped as the page's rgSweepMsg) = links from the transmitter's
//   cells to the station, the clutter at its true ends
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';
import {fileURLToPath} from 'url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const H = fs.readFileSync(path.join(HERE, '..', 'kraken_doa.html'), 'utf8').replace(/\r/g, '');
let fails = 0;
const check = (c, what) => { console.log((c ? 'PASS ' : 'FAIL ') + what); if (!c) fails++; };
// a top-level page function's source (2-space indent: a one-liner, else it ends at the next "\n  }\n")
const fnSrc = name => {
  const a = H.indexOf(`  function ${name}(`);
  if (a < 0) throw new Error('no function ' + name);
  const line = H.slice(a, H.indexOf('\n', a) + 1), open = line.split('{').length, close = line.split('}').length;
  return open > 1 && open === close ? line : H.slice(a, H.indexOf('\n  }\n', a) + 4);
};
const [, EPS, ALPHA, NEAR] = H.match(/const RG_EPS = ([\d.]+), RG_ALPHA = ([\d.]+), RG_NEAR_M = (\d+)/).map(Number);

// ---- the page's functions (the settings they read: rgState) ----
const rgState = {fade: 'none', K: 6, model: 'p1812'};
const P = new Function('rgState', `
  const plFadeTabs = new Map(), plRelCache = new Map();
  let rgKidx = null, rgKidxKey = '';
  const RG_EPS = ${EPS}, RG_ALPHA = ${ALPHA};
  ${['plI0e', 'plFadeTab', 'plFadeCdfDb', 'plPhi', 'plOutage', 'plRelLut', 'rgKeffQ', 'rgAutoFade', 'rgLik', 'rgAng', 'rdfB64'].map(fnSrc).join('\n')}
  return {rgLik, rgAng};`)(rgState);

// ---- synthetic terrain, served as terrarium PNGs through fetch (as pathloss_validate.mjs) ----
const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = b => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (type, data) => {
  const out = Buffer.alloc(12 + data.length), t = Buffer.from(type, 'ascii');
  out.writeUInt32BE(data.length, 0); t.copy(out, 4); data.copy(out, 8);
  out.writeUInt32BE(crc32(Buffer.concat([t, data])), 8 + data.length);
  return out;
};
const synthH = (la, lo) => {
  const x = (lo - 174.5) * 88, y = (la + 36.9) * 111;
  let h = 40 + 25 * x - 2 * y;
  h += 350 * Math.exp(-((x - 12) ** 2 + (y - 4) ** 2) / 18);
  h += 220 * Math.exp(-((x - 0.45 * y - 25) ** 2) / 6) * (0.6 + 0.4 * Math.sin(y / 3));
  h -= 90 * Math.exp(-((x - 18) ** 2) / 4);
  h += 15 * Math.sin(x * 1.7) * Math.sin(y * 1.3) + 4 * Math.sin(x * 9.1 + y * 7.3);
  return x < -6 ? Math.min(h, -20 - 5 * (-6 - x)) : h;
};
globalThis.fetch = async url => {
  const m = String(url).match(/terrarium\/(\d+)\/(\d+)\/(\d+)\.png$/);
  if (!m) throw new Error('offline: ' + url);
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
  return new Response(Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
};

// ---- the workers' code: the sweep (vsTest) and the posterior (rgTest) ----
const msgs = [];
globalThis.self = {postMessage: m => msgs.push(m)};
new Function(H.slice(H.indexOf('  function vsWorkerMain() {'), H.indexOf('  // (end of vsWorkerMain)')) + '\nvsWorkerMain();')();
const T = self.vsTest;
new Function(fnSrc('rgPostMain') + '\nrgPostMain();')();
const R = self.rgTest;
const pz = H.indexOf('  function vsPlan(');
const vsPlan = new Function('VS_RE', 'VS_ZFINE', 'VS_ZSTEP', 'VS_NMAX', H.slice(pz, H.indexOf('\n  }\n', pz) + 4) + '; return vsPlan;')(
  6371000, +H.match(/VS_ZFINE = (\d+)/)[1], +H.match(/VS_ZSTEP = (\d+)/)[1], +H.match(/VS_NMAX = (\d+)/)[1]);
let job = 0;
// the page's station sweep (rgSweepMsg): the station is the worker's "Tx" end, the cells a transmitter at tgt
async function sweep(lat, lon, h, rKm, sig) {
  msgs.length = 0;
  const m = {job: ++job, lat, lon, h, amsl: false, k: 4 / 3, levels: vsPlan(lat, rKm * 1000), sig, par: 0};
  await T.run(m);
  const r = msgs.find(x => x.job === m.job && (x.levels || x.error));
  if (r.error) throw new Error(r.error);
  const levels = r.levels.map(l => ({z: l.z, N: l.N, gx0: l.gx0, gy0: l.gy0, loss: new Uint16Array(l.loss), fad: new Uint16Array(l.fad)}));
  R.init({key: 'k' + job, lat, lon, rM: rKm * 1000, levels: levels.map(l => ({...l, loss: l.loss.slice(), fad: l.fad.slice()}))});
  return {key: 'k' + job, lat, lon, rM: rKm * 1000, levels};
}
const D2R = Math.PI / 180, RE = 6371000;
const dest = (lat, lon, brg, d) => {
  const p1 = lat * D2R, b = brg * D2R, a = d / RE;
  const p2 = Math.asin(Math.sin(p1) * Math.cos(a) + Math.cos(p1) * Math.sin(a) * Math.cos(b));
  return [p2 / D2R, lon + Math.atan2(Math.sin(b) * Math.sin(a) * Math.cos(p1), Math.cos(a) - Math.sin(p1) * Math.sin(p2)) / D2R];
};
const hav = (la1, lo1, la2, lo2) => {
  const a = Math.sin((la2 - la1) * D2R / 2) ** 2 + Math.cos(la1 * D2R) * Math.cos(la2 * D2R) * Math.sin((lo2 - lo1) * D2R / 2) ** 2;
  return 2 * RE * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
};
const bearing = (la1, lo1, la2, lo2) => {
  const y = Math.sin((lo2 - lo1) * D2R) * Math.cos(la2 * D2R);
  const x = Math.cos(la1 * D2R) * Math.sin(la2 * D2R) - Math.sin(la1 * D2R) * Math.cos(la2 * D2R) * Math.cos((lo2 - lo1) * D2R);
  return (Math.atan2(y, x) / D2R + 360) % 360;
};
// the cell (finest ring first) holding a place, as the page's hover
const cellAt = (S, la, lo) => {
  const nx = (lo + 180) / 360, ny = (1 - Math.log(Math.tan(Math.PI / 4 + la * D2R / 2)) / Math.PI) / 2;
  for (let li = 0; li < S.levels.length; li++) {
    const l = S.levels[li], Wv = 256 * 2 ** l.z, i = Math.floor(nx * Wv - l.gx0), j = Math.floor(ny * Wv - l.gy0);
    if (i < 0 || j < 0 || i >= l.N || j >= l.N) continue;
    const k = j * l.N + i;
    if (l.loss[k] === 65535) continue;
    return l.loss[k] === 65534 ? null : {li, k, loss: l.loss[k] / 10};
  }
  return null;
};
const lobeAt = (deg, sd) => {
  const u = new Uint8Array(720);
  for (let k = 0; k < 720; k++) { const d = ((k * 0.5 - deg + 540) % 360) - 180; u[k] = Math.round(255 * Math.exp(-0.5 * (d / sd) ** 2)); }
  return Buffer.from(u).toString('base64');
};
const post = (S, ang, base, lik, seq = 1) => {
  if (lik.idx) R.setIdx(lik.idx);
  return R.post({seq, key: S.key, ang, base, D: lik.D, nk: lik.nk, near: NEAR});
};

// ---- 1: the level's pdf ----
{
  const sum = (D, o = 0) => { let s = 0, m = 0; for (let i = 0; i < 481; i++) { s += D[o + i] * 0.25; m += D[o + i] * 0.25 * (-50 + i * 0.25); } return [s, m / s]; };
  rgState.fade = 'none';
  const n = P.rgLik(6);
  const [s0, m0] = sum(n.D);
  let worst = 0;
  for (let i = 1; i < 480; i++) { const m = -50 + i * 0.25; worst = Math.max(worst, Math.abs(n.D[i] - Math.exp(-0.5 * (m / 6) ** 2) / (6 * Math.sqrt(2 * Math.PI)))); }
  check(Math.abs(s0 - 1) < 1e-3 && Math.abs(m0) < 0.01 && worst < 2e-4, `1 no fading, sigma 6 dB: the pdf = the normal's (worst ${worst.toExponential(1)}), integral ${s0.toFixed(4)}, mean ${m0.toFixed(3)} dB`);
  rgState.fade = 'rayleigh';
  const r = P.rgLik(4);
  const [s1, m1] = sum(r.D);
  check(Math.abs(s1 - 1) < 2e-3 && Math.abs(m1 - 2.51) < 0.05, `1 Rayleigh + sigma 4 dB: integral ${s1.toFixed(4)}, the level ${m1.toFixed(2)} dB below the local mean on average (2.51)`);
  rgState.fade = 'auto';
  const a = P.rgLik(5);
  let ok = a.nk > 2 && a.idx && a.idx.length === 65536;
  for (let t = 0; t < a.nk; t++) ok = ok && Math.abs(sum(a.D, t * 481)[0] - 1) < 3e-3;
  check(ok, `1 auto fading: ${a.nk} tables (Rayleigh .. Rician ${rgState.K} dB), each integrates to 1; idx per fadQ`);
}
// ---- 2: the bearing factor ----
{
  const ang = P.rgAng(lobeAt(123, 3), 4);
  let im = 0;
  for (let k = 1; k < 720; k++) if (ang[k] > ang[im]) im = k;
  const floor = Math.min(...ang), want = EPS ** ALPHA;
  // (sd 4 deg on a 3 deg lobe: p peak ~ 1 / (sqrt(2 pi) 5 deg / 0.5 deg) -> (0.95 x 720 p + 0.05) / 0.05 ~ 550)
  check(Math.abs(im * 0.5 - 123) <= 0.5 && Math.abs(floor - want) < 0.01 * want && ang[im] / floor > (ALPHA === 1 ? 300 : 5),
        `2 the bearing factor: peak at ${im * 0.5} deg (lobe 123), floor ${floor.toFixed(4)} (eps^alpha ${want.toFixed(4)}), peak / floor ${(ang[im] / floor).toFixed(1)}`);
}

// ---- 3: free space, an exact level ----
const ST = [-36.88, 174.62];
{
  const t0 = Date.now();
  const S = await sweep(ST[0], ST[1], 10, 8, {f: 146, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0, hmRx: 0, model: 'fs'});
  const [tla, tlo] = dest(ST[0], ST[1], 60, 4000), c = cellAt(S, tla, tlo);
  rgState.fade = 'none';
  const sigma = 6, lik = P.rgLik(sigma), sd = 4, ang = P.rgAng(lobeAt(60, 2), sd);
  const r = post(S, ang, c.loss, lik);   // (EIRP + gains - the measured = the true cell's loss: m = 0 there)
  const st = r.st;
  // (the most likely place = the centre of the most probable block, 2 x 1.05 x 8 km / 128 = 131 m:
  // the mode of a broad distribution - sigma 6 dB is a factor of 2 in distance in free space)
  check(!r.empty && Math.abs(st.d - 4000) < 400 && Math.abs(st.b - 60) < 3,
        `3 free space, exact level: the most likely place ${st.d.toFixed(0)} m at ${st.b.toFixed(1)} deg (true 4000 m, 60 deg); ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  // the same posterior in plain loops: each cell's centre -> distance, bearing; the normal pdf; the area
  const w = [];
  let Z = 0;
  for (const l of S.levels) {
    const W = 256 * 2 ** l.z, mpp = 2 * Math.PI * RE / W;
    for (let j = 0; j < l.N; j++) {
      const la = Math.atan(Math.sinh(Math.PI * (1 - 2 * (l.gy0 + j + 0.5) / W))) / D2R, A = (mpp * Math.cos(la * D2R)) ** 2;
      for (let i = 0; i < l.N; i++) {
        const ls = l.loss[j * l.N + i];
        if (ls >= 65534) continue;
        const lo = (l.gx0 + i + 0.5) / W * 360 - 180, d = hav(ST[0], ST[1], la, lo), m = c.loss - ls / 10;
        const b = bearing(ST[0], ST[1], la, lo) * 2, b0 = Math.floor(b), a = d < NEAR ? 1 : ang[b0 % 720] + (ang[(b0 + 1) % 720] - ang[b0 % 720]) * (b - b0);
        const p = a * Math.exp(-0.5 * (m / sigma) ** 2) / (sigma * Math.sqrt(2 * Math.PI)) * A;
        if (p > 0) { w.push([d, p]); Z += p; }
      }
    }
  }
  w.sort((x, y) => x[0] - y[0]);
  const q = [0.05, 0.25, 0.5, 0.75, 0.95].map(pp => { let s = 0; for (const [d, p] of w) { s += p; if (s >= pp * Z) return d; } return S.rM; });
  const dq = q.map((v, i) => Math.abs(v - st.q[i]));
  check(Math.max(...dq) < 0.004 * S.rM + 20, `3 the distance's quantiles = an independent posterior: 5/25/50/75/95 % ${st.q.map(v => (v / 1000).toFixed(2)).join(' / ')} km ` +
        `vs ${q.map(v => (v / 1000).toFixed(2)).join(' / ')} (worst ${Math.max(...dq).toFixed(0)} m)`);
  check(st.q[0] < 4000 && 4000 < st.q[4], `3 the true distance within the 90 % interval`);
  // levels no place fits: 100 dB too strong (the station's own cell is still within the table: the
  // page warns - mMax < -3 sigma), 200 dB too weak (nothing within it: empty)
  const e = post(S, ang, c.loss - 100, lik, 2), e2 = post(S, ang, c.loss + 200, lik, 3);
  check(!e.empty && e.st.mMax < -3 * sigma, `5 100 dB too strong: the strongest place's median ${(-e.st.mMax).toFixed(0)} dB short (> 3 sigma: the page's "no place fits")`);
  check(e2.empty && e2.mMin > 3 * sigma, `5 200 dB too weak: empty, the weakest place ${e2.mMin.toFixed(0)} dB too strong`);
}

// ---- 4: calibration over the terrain (P.1812, a flat lobe) ----
{
  const t0 = Date.now();
  const S = await sweep(ST[0], ST[1], 10, 8, {f: 446, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 0, envRx: 0, hmTx: 0, hmRx: 0, model: 'p1812'});
  rgState.fade = 'none';
  const sigma = 6, lik = P.rgLik(sigma), ang = new Float32Array(720).fill(1);
  let seed = 11;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const gauss = () => Math.sqrt(-2 * Math.log(rnd() + 1e-12)) * Math.cos(2 * Math.PI * rnd());
  let n = 0, in50 = 0, in90 = 0;
  for (let t = 0; t < 150; t++) {
    const [la, lo] = dest(ST[0], ST[1], 360 * rnd(), S.rM * Math.sqrt(rnd()));   // uniform over the area
    const c = cellAt(S, la, lo);
    if (!c) continue;
    const r = post(S, ang, c.loss + sigma * gauss(), lik, 10 + t);
    if (r.empty) continue;
    const u = r.levels[c.li][c.k], cc = u ? (u - 1) / 254 * 0.99 : 1;
    n++;
    if (cc <= 0.5) in50++;
    if (cc <= 0.9) in90++;
  }
  const f50 = in50 / n, f90 = in90 / n;
  check(n > 120 && Math.abs(f50 - 0.5) < 0.1 && Math.abs(f90 - 0.9) < 0.06,
        `4 calibration, P.1812 over the hills, sigma 6 dB: the true place in the 50 % region ${(f50 * 100).toFixed(0)} %, in the 90 % region ${(f90 * 100).toFixed(0)} % of ${n} (${((Date.now() - t0) / 1000).toFixed(0)} s)`);
  // auto fading: K per cell from the path
  rgState.fade = 'auto';
  const la = P.rgLik(sigma), c = cellAt(S, ...dest(ST[0], ST[1], 100, 5000));
  const r = post(S, P.rgAng(lobeAt(100, 3), 4), c.loss, la, 999);
  check(!r.empty && Number.isFinite(r.st.d) && r.st.q.every(Number.isFinite) && r.st.q[0] < 5000 && 5000 < r.st.q[4],
        `5 auto fading (K per cell), exact level, P.1812: most likely ${(r.st.d / 1000).toFixed(2)} km at ${r.st.b.toFixed(0)} deg, ` +
        `90 % ${(r.st.q[0] / 1000).toFixed(2)} - ${(r.st.q[4] / 1000).toFixed(2)} km (true 5 km, 100 deg)`);
}
// ---- 6: the station's sweep = the transmitter's real direction ----
// the page's rgSweepMsg: the station is the worker's "Tx" end - envTx = OUR
// clutter (open, 10 m), envRx = the transmitter's (urban, 1.5 m: P.2108's
// loss down in it). A link from each cell (the transmitter) to the station
// (the other way round, the clutter at its true ends) must give the same loss
{
  const S = await sweep(ST[0], ST[1], 10, 8, {f: 146, tgt: 1.5, amsl: false, pol: 1, N0: 301, envTx: 2, envRx: 4, hmTx: 0, hmRx: 15, model: 'p1812'});
  let seed = 5;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
  const diffs = [];
  let clutTx = 0;
  for (let t = 0; t < 25; t++) {
    const [la, lo] = dest(ST[0], ST[1], 360 * rnd(), 500 + 7000 * rnd()), c = cellAt(S, la, lo);
    if (!c) continue;
    const l = S.levels[c.li], W = 256 * 2 ** l.z, i = c.k % l.N, j = Math.floor(c.k / l.N);
    const cla = Math.atan(Math.sinh(Math.PI * (1 - 2 * (l.gy0 + j + 0.5) / W))) / D2R, clo = (l.gx0 + i + 0.5) / W * 360 - 180;
    msgs.length = 0;
    const m = {job: ++job, link: {a: {lat: cla, lon: clo, h: 1.5, amsl: false}, b: {lat: ST[0], lon: ST[1], h: 10, amsl: false}, f: 146, k: 4 / 3, pol: 1, N0: 301,
                                  climate: 5, eps: 15, sigma: 0.005, envA: 4, envB: 2, hmA: 15, hmB: 0, rel: 50}};
    await T.run(m);
    const r = msgs.find(x => x.job === m.job && (x.link || x.error));
    if (!r || r.error) { check(false, '6 link: ' + (r && r.error)); continue; }
    diffs.push(Math.abs(r.link.p1812.Lb + r.link.clut.a + r.link.clut.b - c.loss));
    clutTx = Math.max(clutTx, r.link.clut.a);
  }
  diffs.sort((x, y) => x - y);
  const med = diffs[Math.floor(diffs.length / 2)], p90 = diffs[Math.floor(diffs.length * 0.9)];
  check(diffs.length >= 20 && med < 1.5 && clutTx > 5, `6 the station's sweep vs links from the transmitter's cells to the station (P.1812 + P.2108, the TX in urban ` +
        `clutter - up to ${clutTx.toFixed(1)} dB of it): |diff| median ${med.toFixed(2)}, p90 ${p90.toFixed(2)} dB (${diffs.length})`);
}
console.log(fails ? `${fails} FAILED` : 'ALL PASS');
process.exit(fails ? 1 : 0);
