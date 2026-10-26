# Digital TV plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital TV (`plugins/dtv/`, README.md there; ARRAY plugin API):**
- Plugin id `dtv`, name "Digital TV (DVB-T/T2, ISDB-T, ATSC 1.0/3.0,
  DTMB)", v2.0 - was `sfn` "SFN Separation (..., DAB)" (renamed 2026-10-14;
  DAB removed, to become a plugin of its own). A broadcast TV signal's own
  information from the 2.4 MHz slice the VFO sees: the standard + mode, the
  channel (measured centre + the raster's number - `channel_label()` in
  decoder.cpp: 8 MHz UHF 21-69, 7 MHz Band III / Australia, 6 MHz North
  America / Korea, ISDB-T Japan / Americas with the 1/7 MHz offset, China
  DS for DTMB), DVB-T TPS (BCH(67,53)-checked; constellation, hierarchy,
  code rates, guard, mode, cell ID, DVB-H, net bit rate), ISDB-T TMCC
  (structure-checked; layers A / B / C, partial reception, next parameters,
  the EWS start flag -> a "⚠" event), ATSC 1.0's fixed parameters, DTMB's
  header; the echo profile (CIR paths). `Estimator::info()` / `take_news()`
  / `centre_hz()` / `bw_mhz()`; OFDM signalling via
  `OfdmStandard::signal_carriers` / `signalling` / `signalling_symbols`
  (`signalling.hpp`: DBPSK soft bits; common phase step from DVB-T's
  continual pilots, else the squared products)
- **Path separation** (option `separate`, default "0" = off; an on / off
  button in the decoder tab; plugin v2.1 - it was "SFN separation" until
  2026-10-15): the signal reaches the antennas on several paths -
  reflections, and in a single frequency network the other transmitters -
  one VFO DoA mixes them. On: every ANTENNA's channel impulse response
  against the signal's known reference, the paths as peaks of the per-tap
  spatial covariance (by delay), tracked, each one's covariance reported;
  kraken_doa runs MUSIC with one source on it -> a talker "Path-n" per path
  (`SfnCore::tid`), its own DoA lobe. Renamed because a transmitter and a
  reflection are indistinguishable (identical copies of the signal; DVB-T's
  cell ID is the same on every SFN transmitter) - on the real Auckland
  muxes every "SFN-2.." was a reflection. Delays from the MAIN path
  (`SfnCore::strongest()`: the strongest confirmed track, kept unless
  another is 1 dB stronger), signed (a weaker earlier path is negative);
  the circular order still comes from `earliest()` (after the largest gap),
  whose delay is the table's "From first" column; `sp.ref` = the main path
  (UI tag "main"). Events "New path Path-n: ..." / "Path Path-n gone"; fact
  "Paths" (count, spread, first-before-main). Off (`SfnCore::
  set_profile_only`, the core's default): only antenna 0 is analysed (the
  plugin drops the rest of each block; kraken_doa still sends them), the
  CIR's paths become the "Echo profile" plot (markers without talkers) +
  "Paths" fact, a path marked when seen again within 1.5 taps in one of
  the two bursts before; `Estimator::profile_floor_db` (ATSC 3.0 22 dB)
  hides artefacts one antenna can't tell by direction. Toggling resets the
  core: its table rows go, `SfnCore::reset()` calls `spatial_remove("")`
  (the paths leave the 📡 DF panel within ~1 s instead of SPATIAL_TTL_MS 2
  min) and turning separation off also sends empty table columns (no empty
  path table left in the tab); a lost track is removed the same way
  (`spatial_remove("Path-n")`). Option keys `separate` / `max_tx` ("Max
  paths") kept, so saved VFOs keep their setting. Per standard: `ofdm.cpp` (generic pilot engine:
  guard fold / P1 grid / symbol tracking, pilot comb phase - each bin's
  power against its burst average first, else a 2k signal with paths 37 us
  apart never locked -, carrier offset by the sparsest CIR, per-symbol phase
  step measured + removed, runs aligned) + `dvbt.cpp`, `dvbt2.cpp`,
  `isdbt.cpp`, `atsc3.cpp`; `atsc.cpp` (field sync), `dtmb.cpp` (PN
  headers, phase schedule fit); `sfn_core.cpp` (per-tap covariance, floor,
  peaks, echo / sidelobe rules, tracks, spatial reports, plot, table);
  `std/*_tables.hpp` (the standards' tables). Bursts shorter than half a
  burst (the host's blocks overshooting one) are ignored
- Compatibility: `dig::parse_mode_string` maps `PLUGIN:sfn` to `dtv` and
  `options_from_string` the saved `sfn.*` options to `dtv.*`
  (`renamed_plugin()` in dig_report.cpp); the plugin treats an unknown
  `standard` (a saved "dab") as auto
- ARRAY plugin API (kraken_plugin.hpp): `kp::Info::array` (+
  `array_burst_s`, `array_period_s`; dtv 0.35 s every 1 s),
  `Decoder::process_array(const kp::ArrayBlock&)` (m antennas, n samples
  each, the VFO's own rate - no resampling / AFC -, t0 = Host::time(),
  gap), `Host::spatial(kp::Spatial)` (id, label, span, m x m cov, delay,
  power, snr, ref, avg_s), `Host::spatial_remove(id)` ("" = all of the
  plugin's: `SpatialReport::remove` -> `TalkerDoa::add_spatial` erases them
  and their pending mobile DF records), `Host::plot(kp::Plot)` (title,
  units, x0 / dx, y <= 1000, threshold, markers {x, label, talker}). Wire
  SAMPLES_ARRAY 9 (int16 IQ per antenna, gap flag), SPATIAL 35, PLOT 36,
  SPATIAL_REMOVE 39 (unknown types are ignored, so older hosts are safe).
  An empty TABLE_COLUMNS payload = no columns (it used to become one blank
  column): with no rows either, the page shows no table. `dig_plugin`
  reads array / burst / period from `--info`; array => manual_only. The
  offline host runs array plugins on multichannel cf32 (`--channels`,
  `--all`, `--uca R` prints each spatial talker's MUSIC bearing)
- Host: `DigitalDecoder::wants_array()` / `push_array()` - the data
  receiver passes every VFO block of all antennas while not calibrating /
  in the retune hold; only blocks inside the plugin's bursts (stream time)
  are copied and sent (int16). Engine `on_spatial` -> `SpatialReport`
  (span in VFO positions) -> `set_spatial_handler` (DecimatorManager) ->
  `TalkerDoa::add_spatial`: a spatial talker (`TalkerInfo::spatial`,
  sp_delay_us / sp_db / sp_snr / sp_ref), its covariances averaged over
  avg_s (`SPATIAL_AVG_S` 3 s of stream time), MUSIC with ONE source
  (`spectrumFromCovariance(R, 1)`), each report a mobile DF record;
  forgotten after `SPATIAL_TTL_MS` 2 min. `on_plot` -> `Report::plot_set`
  -> the status JSON's `plots` {plugin: {t, xu, yu, x0, dx, ymin, ymax,
  thr, y, mk: [[x, label, talker]], age}} (with the tables: at most once a
  second per VFO). rdf_mapper: talker field `sp: [delay_us, power_db,
  snr_db, first]`; spatial talker IDs are volatile (per plugin run): their
  heat maps are not saved to rdf_session.bin
- UI (kraken_doa.html "Plugin plots + TV signal paths" block): the
  decoder tab draws each VFO's plugin plot (`digRenderPlots` ->
  `#info-dec-plots`, `plotDraw`; a plot whose markers name no talker - dtv's
  echo profile - gets a tooltip saying so instead of "click a peak", and
  the DF panel section only shows plots with talker markers); a marker naming a talker and a table row
  whose key is a talker of that VFO (`digRowTalker`, class `talker`) call
  `talkerPick(vfo, tid)`: the 🗺 Map opens with the 📡 DF panel on that
  VFO and the talker selected (again = the whole signal). The DF panel's
  `#map-df-sfn` section draws the selected VFO's plot with talker markers
  (`sfnRenderSection`). Talkers with `sp` are kind 'path' (📶, by
  delay from the main path, "main" = the strongest; "Every path (n)")
- It replaced an in-process blind SFN separation per VFO (sfn_doa.cpp,
  `DEC_SFN:` / `SFN_MAX_US:`; commit 7f3535a): the per-tap CIR against a
  known reference needs the standard's structure, which belongs in a
  plugin
- `PluginProcess::flush()` keeps a write offset (`wpos_`) instead of
  erasing every write from the front of `wbuf_` (quadratic in what is
  queued: an array burst queues ~17 MB and the pipe takes 64 KB - 1 MB at
  a time - it cost up to 60 % of a core) and asks for a 1 MB pipe. While
  only array plugins run (`array_only_`), `push()` passes a one-sample tick
  every 100 ms instead of the VFO's stream (it starts the plugin, reads
  its replies, follows the RF)
- Memory (2026-10-15, kraken_doa reached 2.3 GB RSS and swapped): the
  backlog limit in `PluginProcess::send` only applied to SAMPLES, so
  SAMPLES_ARRAY (and SAMPLES_MC) queued in `wbuf_` without end - and the
  queue was only written when a new block came in, so between bursts the
  plugin got 1 MB (one pipe) per 100 ms tick: ~10 MB/s, less than the
  17 MB/s of a 5-antenna dtv burst stream, while the plugin itself idled
  at 7 % of a core; and a plugin really slower than real time (dtv
  searching after a retune: ~95 % of a core for tens of seconds) made it
  grow too. Now any message sent with `max_backlog > 0` is dropped past
  it (~1 s; control messages pass 0 = always queued),
  `DigitalDecoder::run` keeps writing every 2 ms while bytes are pending
  (`Engine::flush_pending`, which reads the replies too), and `stop()`
  frees the buffer (`clear()` kept its capacity). Live with a dtv VFO:
  RSS 140-220 MB (was "~500 MB normal" = a kept backlog buffer), the
  plugin fed its full 16.8 MB/s, a 12 s SIGSTOPped plugin = no growth
- CPU (Pi 5, DVB-T live, separation on): plugin ~25 % of a core,
  kraken_doa +6-16 %; the estimation per burst ~3x what it is with
  separation off (real 522 MHz capture: ~100 vs ~35 ms, busy Pi)
- Real captures (/home/krakenrf/dvbt_compress_study/captures, 6 Auckland
  DVB-T muxes, 5 coherent channels): DVB-T 8k GI 1/16 locked on all, TPS
  BCH-checked in every frame (64-QAM 3/4, 26.35 Mbit/s, cell IDs 32100,
  10100, 20100, 31100, 30100, 33100 - round decimals, so the high byte in
  frames 1 / 3 is right), one transmitter each (48-55 dB above the floor) +
  local reflections 13-35 dB down - hence the `max_rel_db` option (30 dB).
  No real SFN, no real ISDB-T tried yet
- Tests: offline regression of 12 synthetic recordings (`tools/sfn_gen.cpp`
  - every standard, awkward VFO offsets, DTMB phase-varying, ATSC 3.0
  excess samples; README's table; scratchpad regress2.sh) - separation on:
  delays / powers exact, bearings within 1.3 deg; off: exactly the 3 paths
  each. The generator sends real TPS (BCH parity, `--cell`) and TMCC
  (`--ews`): DVB-T 2k / ISDB-T modes 1 and 3 decoded, the EWS event. Live
  (scratchpad e2e/: fake_heimdall_rec.py streams a recording in heimdall's
  8091 format; t_sfn_plugin.py drives a TEST build on ports 4708x / 4709x;
  t_sfn_plugin_ui.py = headless Chromium): DVB-T exact, the UI's plot /
  table / DF panel picks; t_dtv_ui.py: `PLUGIN:sfn` -> dtv, the Path
  separation button (Off by default, On / Off by the echo), the echo
  profile's tooltip, cells' "Cell information" button
- Performance pass (2026-10-26; a 2015 MacBook Air i5-5350U, Apple
  clang 14; Pi / Windows not measured):
  `sfn::Resampler::run` (dsp_util.hpp, ~20 % of the plugin's CPU) computes
  interior outputs four at a time (eight independent 24-tap sums, each in
  the old order) and rounds the phase without the `lround` libm call (the
  same value: x - its integer part is exact): 9.7 -> 6.3 ms per 0.1 s
  burst (1.4-1.5x), bit-identical (also with `-ffp-contract=fast`);
  `dtv-dvbt-paths` identical, offline 2.20 -> 2.05 s (-7 %). Not done: `is_pilot` (a virtual call per
  carrier in `find_offset`, ~11 % of the plugin) could be a table per
  pattern phase
