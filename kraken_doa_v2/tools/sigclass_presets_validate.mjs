// Run with node tools/sigclass_presets_validate.mjs from kraken_doa_v2.
// Execute the actual Signal ID entry and saved-frequency tuning functions
// with mocked UI/transport boundaries (no receiver or radio hardware).
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const html = fs.readFileSync(new URL('../kraken_doa.html', import.meta.url), 'utf8');
function extract(start, end) {
  const a = html.indexOf(start), b = html.indexOf(end, a + start.length);
  assert(a >= 0 && b > a, start);
  return html.slice(a,b);
}
const code = [
  extract('function sigPresetLocked(pl)', 'function sigPresetSelect()'),
  extract('function sigEntry(t)', 'function sigTuneId(id)'),
  extract('function tkVoiceDec(d)', '// a voice VFO\'s live state:'),
  extract('function sfTune(x, opts = {})', '// the VFO\'s decoder holds it on a fixed frequency')
].join('\n');
const calls = [], flashes = [], plugins = {
  p25: {built:true,voice:true}, dmr:{built:true,voice:true},
  ais:{built:true,fixed_freq_hz:162e6}, ism:{built:true,lock_vfo:true},
  pagers:{built:true,voice:false}, unavailable:{built:false,voice:true}
};
const d = {id:0, digital_mode:'PLUGIN:p25',demod_mode:'NBFM',bw:24000,tuner:0};
const cx = vm.createContext({
  sigPresetReady:false,sigPresetOverrides:new Map(),activeDecimatorId:0,
  decimatorInfo:[d],digPlugin:id=>plugins[id],sigBandCentre:()=>null,
  vfoLocked:d=>!!(plugins[d.digital_mode?.slice(7)]?.fixed_freq_hz || plugins[d.digital_mode?.slice(7)]?.lock_vfo),
  sfFixedLock:d=>!!plugins[d.digital_mode?.slice(7)]?.fixed_freq_hz,
  digFixedPlugin:m=>plugins[m?.slice(7)],ws:{readyState:1},FREQ_MIN:24,FREQ_MAX:1766,
  digSetModeFor:(id,mode)=>{calls.push(['decoder',id,mode]);d.digital_mode=mode},
  digToggle:(id,on)=>{calls.push(['toggle',id,on]);d.digital_mode='OFF'},
  setDecimatorDemodMode:(id,mode)=>{calls.push(['demod',id,mode]);d.demod_mode=mode},
  setDecimatorBandwidthHzFromUI:(id,hz)=>{calls.push(['bandwidth',id,hz]);d.bw=hz},
  bwClampHz:hz=>Math.max(1000,Math.min(2400000,hz)),decBwHz:d=>d.bw,
  sfTuneHz:(hz,opts)=>{calls.push(['frequency',hz,opts.tuner]);return ''},
  sfFlash:(msg,err)=>flashes.push([msg,err]),sfHasGain:()=>false, mkFmtMhz:hz=>String(hz/1e6),
  sfDecName:x=>x
});
vm.runInContext(code,cx);
const track={cls:'p25p2',name:'P25 Phase II',hz:769532305,plugin:'p25',demod:'NBFM',vbw:24000,bw:9000};
const entry=t=>JSON.parse(JSON.stringify(cx.sigEntry(t)));
assert.deepEqual(entry(track),{id:'',name:'P25 Phase II',hz:769532305,dec:'PLUGIN:p25',mode:'NBFM',bw:24000});
cx.sigPresetReady=true;
cx.sigPresetOverrides.set('p25p2',{demod:'DIGITAL',hz:12000});
assert.deepEqual(entry(track),{id:'',name:'P25 Phase II',hz:769532305,dec:'PLUGIN:p25',mode:'DIGITAL',bw:12000});
cx.sigTune(track,{tuner:3});
assert.deepEqual(calls.splice(0),[['demod',0,'DIGITAL'],['bandwidth',0,12000],['frequency',769532305,3]]);
d.digital_mode='OFF';d.demod_mode='NBFM';d.bw=24000;
cx.sigTune(track);
assert.deepEqual(calls.splice(0),[['decoder',0,'PLUGIN:p25'],['demod',0,'DIGITAL'],['bandwidth',0,12000],['frequency',769532305,undefined]]);
d.trunk={role:'voice'};
cx.sigTune(track);
assert.deepEqual(calls.splice(0),[]);
assert.match(flashes.at(-1)[0], /voice VFO of trunk following/);
assert.equal(flashes.at(-1)[1],true);
delete d.trunk;
console.log('PASS upstream trunk-managed voice VFO guard: no decoder, demod, bandwidth or frequency changes');
cx.sigPresetOverrides.set('p25p2',{demod:'DIGITAL',hz:9500});
assert.equal(entry(track).bw,9500);
cx.sigPresetOverrides.set('nxdn',{demod:'DIGITAL',hz:4000});
plugins.nxdn={built:true,voice:true};
assert.equal(entry({...track,cls:'nxdn',plugin:'nxdn'}).bw,4000);
assert.equal(entry({...track,plugin:'unavailable'}).mode,'NBFM');
assert.equal(entry({...track,plugin:'unavailable'}).bw,24000);
for (const [cls, vbw] of [['pocsag',24000],['flex',25000]]) {
  const t={...track,cls,name:cls,plugin:'pagers',vbw};
  assert.equal(entry(t).mode,'NBFM');
  cx.sigPresetOverrides.set(cls,{demod:'DIGITAL',hz:12500});
  assert.equal(entry(t).mode,'DIGITAL');
  assert.equal(entry(t).dec,'PLUGIN:pagers');
  assert.equal(entry(t).bw,12500);
  d.digital_mode='OFF';d.demod_mode='NBFM';d.bw=vbw;
  cx.sigTune(t);
  assert.deepEqual(calls.splice(0),[['decoder',0,'PLUGIN:pagers'],['demod',0,'DIGITAL'],['bandwidth',0,12500],['frequency',t.hz,undefined]]);
  plugins.pagers.built=false;
  assert.equal(entry(t).mode,'NBFM');
  assert.equal(entry(t).bw,vbw);
  plugins.pagers.built=true;
}
console.log('PASS POCSAG/FLEX non-voice Digital presets, tuning order, unavailable fallback');
for (const plugin of ['ais','ism']) {
  const t={...track,plugin,cls:plugin,hz:plugin==='ais'?162000000:433920000,vbw:plugin==='ais'?100000:2400000};
  cx.sigPresetOverrides.set(plugin,{demod:'AM',hz:12000});
  assert.equal(entry(t).bw,t.vbw);
  assert.equal(entry(t).mode,'NBFM');
}
assert.equal(entry({...track,cls:'other',vbw:0,bw:10000}).bw,12500);
cx.sigPresetOverrides.set('ssb',{demod:'FACTORY',hz:2700});
const ssb=entry({...track,cls:'ssb',plugin:null,demod:null,vbw:3000});
assert.equal(ssb.mode,''); assert.equal(ssb.bw,2700);
assert.equal(entry({...track,unknown:true}).bw,24000);
cx.sigPresetReady=false;
assert.equal(entry(track).bw,24000);
cx.sigPresetReady=true;cx.sigPresetOverrides.clear();
assert.equal(entry(track).bw,24000);
console.log('PASS factory defaults, custom widths, already/new decoder, tuner option, unavailable decoder, locked/fixed policy, dynamic fallback, old receiver and reset');
