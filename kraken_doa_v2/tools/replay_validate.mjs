// node tools/replay_validate.mjs [--cpp ./rdf_cmp] [--page kraken_doa.html]   (node 18+, from kraken_doa_v2)
//
// The ⏯ log replay's worker (lrWorkerMain in kraken_doa.html), offline:
//  1. a synthetic drive around a transmitter at a KNOWN place, written as the
//     DoA logs the KrakenSDR Android app / doa_logger.cpp write - the newest
//     CSV layout (DOA_value.html lines, with and without the spaces), the
//     2021 373-field one, bearings only, and the COMPASS heading mode (the GPS
//     field garbage) - parsed and run through the heat map: the estimate must
//     land on the transmitter (this checks the conventions: spectrum index
//     counter-clockwise from the array's front, bearing clockwise from the
//     heading, the heading by mode)
//  2. damaged input is refused with a message
//  3. a 120 km drive: the heat map's window follows the vehicle by whole
//     cells (never a jump), re-anchors its frame without moving, and the
//     estimate lands on the transmitter
//  4. --cpp: the engine port vs the C++ engine (src/rdf_engine.cpp) on the
//     records the replay made and the long drive's - every estimate, the
//     coarse window's place, the final heat map rasters, the measured
//     offset. Build it first:
//       g++ -std=c++20 -O2 -Iinclude tools/rdf_cmp.cpp src/rdf_engine.cpp -o rdf_cmp
//     Run this after changing either engine: the two must stay alike.
import fs from 'fs';
import os from 'os';
import path from 'path';
import {execFileSync} from 'child_process';

const arg = k => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
const page = arg('--page') || 'kraken_doa.html', cpp = arg('--cpp');
let html = fs.readFileSync(page, 'utf8');
const a = html.indexOf('  function lrWorkerMain() {'), b = html.indexOf('  // (end of lrWorkerMain)');
if (a < 0 || b < 0) { console.error('lrWorkerMain not found in ' + page); process.exit(2); }
// the worker in this scope (a vm context would be ~10x slower), its replies collected
const replies = [], self = {};
new Function('self', 'postMessage', html.slice(a, b) + '\nlrWorkerMain();')(self, m => replies.push(m));
const T = self.lrTest;
let fail = 0, n = 0;
const check = (ok, msg) => { n++; console.log(ok ? 'ok  ' : 'FAIL', msg); if (!ok) fail++; };
const load = (name, text) => { self.onmessage({data: {op: 'load', id: 1, name, buf: new TextEncoder().encode(text).buffer}}); return replies.pop(); };
const heat = msg => new Promise(res => {
  const snaps = [], ests = [], t0 = Date.now();
  const k0 = replies.length;
  self.onmessage({data: Object.assign({op: 'heat', rangeKm: 10}, msg)});
  const poll = () => {
    while (replies.length > k0) {
      const m = replies.splice(k0, 1)[0];
      if (m.op !== 'heat' || m.job !== msg.job) continue;
      snaps.push(...m.snaps); ests.push(...m.ests);
      if (m.done) return res({snaps, ests, why: m.why, recs: m.recs, ms: Date.now() - t0});
    }
    setTimeout(poll, 2);
  };
  poll();
});

// ---- 1. a synthetic drive around a transmitter at a known place
const TX = [-36.93, 174.9];
let seed = 11;
const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648; };
const gauss = () => Math.sqrt(-2 * Math.log(rnd() + 1e-12)) * Math.cos(2 * Math.PI * rnd());
const pts = [];
{
  let [lat, lon] = [-36.95, 174.88], course = 60;
  for (let i = 0; i < 1200; i++) {
    if (i % 40 === 0) {
      course = (course + (rnd() < 0.5 ? -1 : 1) * (40 + 60 * rnd()) + 360) % 360;
      if (Math.hypot(lat - TX[0], lon - TX[1]) > 0.035) course = (T.rdfBrg(lat, lon, TX[0], TX[1]) + 60 * rnd() - 30 + 360) % 360;
    }
    [lat, lon] = T.rdfFromEnu(lat, lon, 13 * Math.sin(course * Math.PI / 180), 13 * Math.cos(course * Math.PI / 180));
    pts.push({t: 1729000000000 + i * 1000, lat, lon, course});
  }
}
// a MUSIC-like spectrum: 360 x 1 deg counter-clockwise from the array's front, dB above its minimum
const spectrum = peak => {
  const v = [];
  for (let i = 0; i < 360; i++) {
    const d = ((i - peak + 540) % 360) - 180, d2 = ((i - peak - 140 + 540) % 360) - 180;
    v.push(10 * Math.log10(Math.exp(-0.5 * (d / 7) ** 2) + 0.08 * Math.exp(-0.5 * (d2 / 15) ** 2) + 0.002 * rnd()));
  }
  const mn = Math.max(Math.min(...v), -100);
  return v.map(x => (Math.max(x, -100) - mn).toFixed(2));
};
const rowsFor = mode => pts.map(p => {
  const B = (T.rdfBrg(p.lat, p.lon, TX[0], TX[1]) + 2.5 * gauss() + 360) % 360;   // the measured compass bearing
  const gh = mode === 'GPS' ? p.course : 360 * rnd(), ch = mode === 'GPS' ? 360 * rnd() : p.course;   // the unused heading: garbage
  return {t: p.t, lat: p.lat.toFixed(7), lon: p.lon.toFixed(7), gh: gh.toFixed(3), ch: ch.toFixed(3), mode,
          rel: Math.round((B - p.course + 360) % 360), conf: (2 + 7 * rnd()).toFixed(2), pow: (-30 + 10 * rnd()).toFixed(1),
          spec: spectrum((p.course - B + 360) % 360)};
});
const gpsRows = rowsFor('GPS'), cmpRows = rowsFor('COMPASS');
const layouts = {
  'newest (DOA_value.html line)': rs => rs.map(r => [r.t, r.rel.toFixed(1), r.conf, r.pow, 433920000, 'UCA', 100, 'NOCALL', r.lat, r.lon, r.gh, r.ch, r.mode, 'R,R,R,R', ...r.spec].join(', ') + ', \n').join(''),
  'newest without spaces': rs => rs.map(r => [r.t, r.rel, r.conf, r.pow, 433920000, 'UCA', 100, 'NOCALL', r.lat, r.lon, r.gh, r.ch, r.mode, 'R,R,R,R', ...r.spec].join(',') + ',,, \n').join(''),
  '2021 (373 fields)': rs => rs.map(r => [r.t, r.lat, r.lon, r.gh, r.ch, r.rel, r.conf, r.pow, 400000000, 'UCA', 100, r.mode, ...r.spec].join(',') + ', \n').join(''),
  'bearings only (6 fields)': rs => rs.map(r => [r.t, r.lat, r.lon, r.gh, r.ch, r.rel].join(',') + '\r\n').join(''),
  // a spreadsheet's re-save: 13-digit timestamps as 1.72900E+12 (6 significant digits - every row at one
  // "moment", rebuilt from the track), rows padded to a common width; then the ';' + decimal-comma form
  'Excel re-save (1.729E+12 timestamps)': rs => rs.map(r => [excelTs(r.t), r.rel, r.conf, r.pow, 433920000, ' UCA', 100, ' NOCALL', r.lat, r.lon, r.gh, r.ch, ' ' + r.mode, ' R', 'R', 'R', 'R', ...r.spec].join(',') + ',,,,\r\n').join(''),
  'Excel re-save, ";" + decimal commas': rs => rs.map(r => [excelTs(r.t), r.rel, r.conf, r.pow, 433920000, ' UCA', 100, ' NOCALL', r.lat, r.lon, r.gh, r.ch, ' ' + r.mode, ' R', 'R', 'R', 'R', ...r.spec]
                                          .map(x => typeof x === 'string' && /^-?\d+\.\d+(E\+\d+)?$/.test(x) ? x.replace('.', ',') : x).join(';') + ';\r\n').join(''),
};
function excelTs(t) { const s = t.toExponential(5).toUpperCase(); return s.replace(/0+E/, 'E').replace('.E', 'E'); }
let job = 0, gpsRecs = null;
for (const [lab, write] of [...Object.entries(layouts).map(([l, w]) => [l, () => w(gpsRows)]), ['COMPASS heading mode', () => layouts['newest (DOA_value.html line)'](cmpRows)]]) {
  const m = load(lab, write());
  if (m.op !== 'loaded') { check(false, `${lab}: ${m.error}`); continue; }
  const h = await heat({job: ++job, sig: 0});
  const e = h.ests[h.ests.length - 1], d = e ? T.rdfDistM(e.lat, e.lon, TX[0], TX[1]) : Infinity;
  const rt = m.info.retimed;
  check(m.info.rows === 1200 && d < 60 && !!rt === lab.startsWith('Excel'),
        `${lab}: ${m.info.rows} rows, ${h.recs} bearings in ${h.ms} ms, estimate ${d.toFixed(1)} m from the transmitter (r95 ${e ? e.r95 : '-'} m)` +
        (rt ? `; times rebuilt (${rt.how} ${rt.fixMs} ms per fix: ${(rt.durMs / 60000).toFixed(0)} min for the real 20)` : ''));
  if (lab.startsWith('newest (')) gpsRecs = T.L.solver.recs.map(r => Object.assign({}, r, {p: Float32Array.from(r.p)}));
}

// ---- 2. damaged input
for (const [lab, buf] of [['empty file', new ArrayBuffer(0)], ['binary', new Uint8Array(2000).buffer], ['text, no DoA rows', new TextEncoder().encode('a,b\n1,2,3\n').buffer],
                          ['SQLite header only', new TextEncoder().encode('SQLite format 3\0' + ' '.repeat(600)).buffer]]) {
  self.onmessage({data: {op: 'load', id: 1, name: lab, buf}});
  const m = replies.pop();
  check(m.op === 'error', `${lab}: refused (${m.error || 'NOT refused'})`);
}

// ---- 3. the heat map's windows on a long drive (120 km east, the transmitter near its end): the
// coarse window follows the vehicle by whole cells - never a jump (it used to stay put until the
// vehicle was 0.7 of the range out, then jumped ~7 km) - and its frame re-anchors past 100 km
// without the window moving; the estimate still lands on the transmitter
const LTX = [-36.95, 175.95];
const longRecs = [];
{
  let [lat, lon] = [-36.96, 174.70];
  for (let i = 0; i < 3200; i++) {
    const d = T.rdfDistM(lat, lon, LTX[0], LTX[1]);
    const course = d < 3000 ? (T.rdfBrg(lat, lon, LTX[0], LTX[1]) + 90) % 360 : 92;
    [lat, lon] = T.rdfFromEnu(lat, lon, 40 * Math.sin(course * Math.PI / 180), 40 * Math.cos(course * Math.PI / 180));
    const b = (T.rdfBrg(lat, lon, LTX[0], LTX[1]) + 2.5 * gauss() + 360) % 360;
    longRecs.push({t: i * 3000, lat, lon, conf: 1, frames: 5, p: T.rdfBearingLobe(b, 3)});
  }
}
{
  const S = new T.RdfSolver(10, 256), t0 = Date.now();
  let jump = 0, prev = null, frames = new Set(), reanchorJump = 0;
  for (const r of longRecs) {
    const frame0 = S.coarse && S.coarse.lat0 + ',' + S.coarse.lon0;
    S.add(r);
    const c = S.coarse.centre(), fr = S.coarse.lat0 + ',' + S.coarse.lon0;
    frames.add(fr);
    if (prev) {
      const d = T.rdfDistM(prev[0], prev[1], c[0], c[1]);
      if (frame0 && frame0 !== fr) reanchorJump = Math.max(reanchorJump, d); else jump = Math.max(jump, d);
    }
    prev = c;
  }
  const e = S.est, d = T.rdfDistM(e.lat, e.lon, LTX[0], LTX[1]), cell = S.coarse.cell;
  check(jump <= Math.SQRT2 * cell * 1.01 && frames.size >= 2 && reanchorJump <= Math.SQRT2 * cell * 1.01 && d < 100,
        `120 km drive: the coarse window moved at most ${jump.toFixed(0)} m per bearing (a cell: ${cell.toFixed(0)} m), ${frames.size - 1} new frame(s) ` +
        `on the way (the window moved ${reanchorJump.toFixed(0)} m then), estimate ${d.toFixed(0)} m from the transmitter, ${((Date.now() - t0) / longRecs.length).toFixed(1)} ms per bearing`);
}

// ---- 4. the port vs the C++ engine: the CSV drive's records (part 1) and the long drive's
function cmpCpp(recs, lab) {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'rdfcmp-'));
  const buf = Buffer.alloc(4 + recs.length * (8 * 3 + 8 + 720 * 4));
  let o = buf.writeUInt32LE(recs.length, 0);
  for (const r of recs) {
    o = buf.writeDoubleLE(r.t, o); o = buf.writeDoubleLE(r.lat, o); o = buf.writeDoubleLE(r.lon, o);
    o = buf.writeFloatLE(r.conf, o); o = buf.writeInt32LE(r.frames, o);
    for (let k = 0; k < 720; k++) o = buf.writeFloatLE(r.p[k], o);
  }
  fs.writeFileSync(path.join(tmp, 'recs.bin'), buf);
  const off = execFileSync(path.resolve(cpp), [path.join(tmp, 'recs.bin'), tmp]).toString().trim().split(' ').map(Number);
  const est = fs.readFileSync(path.join(tmp, 'est.txt'), 'utf8').trim().split('\n').map(l => l.split(' ').map(Number));
  // (the records as the C++ reads them: float32 lobes)
  const S = new T.RdfSolver(10, 256);
  // (an estimate AT THE GRID'S EDGE - the transmitter outside it - is the top of a flat ridge along the
  // border: float32 vs double picks another cell of it; those are counted, the confident ones compared)
  // (a confident estimate whose top cell ties with a neighbour to float precision may sit one cell over:
  // allowed for < 1 % of them, never more than that cell)
  let worst = 0, worstR = 0, worstC = 0, flags = 0, edgeOff = 0, confN = 0, cellOff = 0, overCell = 0;
  recs.forEach((r0, i) => {
    S.add(Object.assign({}, r0, {p: Float32Array.from(r0.p)}));
    const e = S.est, c = est[i], cc = S.coarse.centre();
    worstC = Math.max(worstC, T.rdfDistM(cc[0], cc[1], c[11], c[12]));
    if (!!e.valid !== !!c[0]) { flags++; return; }
    if (!e.valid) return;
    if (!!e.edge !== !!c[9] || !!S.fine !== !!c[10]) flags++;
    const d = T.rdfDistM(e.lat, e.lon, c[1], c[2]);
    if (e.edge || e.mass <= 0.5) { if (d > 1) edgeOff++; return; }
    confN++;
    const cell = S.estGrid.cell;
    if (d > 0.05 * cell) { cellOff++; if (d > 1.01 * Math.SQRT2 * cell) overCell++; }
    else worst = Math.max(worst, d);
    worstR = Math.max(worstR, Math.abs(e.r95 - c[4]) / c[4]);
  });
  check(cellOff <= 0.01 * confN && !overCell && worstR < 0.01 && worstC < 0.01 && !flags,
        `C++ vs JS, ${lab}: ${recs.length} records, confident estimates within ${worst.toFixed(2)} m (${cellOff} of ${confN} a cell over, a tie), r95 within ` +
        `${(worstR * 100).toFixed(3)} %, the coarse window's centre within ${worstC.toFixed(4)} m, edge / fine-grid flags ${flags ? 'DIFFER ' + flags + ' times' : 'the same'}` +
        (edgeOff ? ` (${edgeOff} estimates at the grid's edge elsewhere on its ridge)` : ''));
  for (const [g, name] of [[S.coarse, 'coarse.bin'], [S.fine, 'fine.bin']]) {
    const want = fs.readFileSync(path.join(tmp, name));
    if (!g) { check(!want.length, `${lab} ${name}: none in JS${want.length ? ' but one in C++' : ' or C++'}`); continue; }
    const got = g.raster(12);
    let worse = 0;
    for (let i = 0; i < got.length; i++) if (Math.abs(got[i] - want[i]) > 1) worse++;
    check(got.length === want.length && !worse, `${lab} ${name}: ${worse} of ${got.length} cells differ by more than 1`);
  }
  const jo = S.estGrid.offset(S.est);
  check(Math.abs(jo.off - off[0]) < 0.01 && Math.abs(jo.sd - off[1]) < 0.01, `${lab} offset JS ${jo.off.toFixed(3)} ± ${jo.sd.toFixed(3)}, C++ ${off[0].toFixed(3)} ± ${off[1].toFixed(3)}`);
  fs.rmSync(tmp, {recursive: true, force: true});
}
if (cpp) {
  if (gpsRecs) cmpCpp(gpsRecs, 'CSV drive');
  cmpCpp(longRecs, '120 km drive');
} else console.log('(the C++ comparison: build tools/rdf_cmp.cpp and pass --cpp ./rdf_cmp)');
console.log(fail ? `${fail} of ${n} FAILED` : `all ${n} passed`);
process.exit(fail ? 1 : 0);
