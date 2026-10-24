# Debug playback

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**🧪 Debug playback of the decoders' test transmissions (`src/playback.cpp`, `include/playback.hpp`, sidebar "🧪 Debug Playback"):**
- What: a decoder's synthetic multi-antenna scene (known IDs / bearings,
  5-element UCA r 0.12 m) played through the REAL pipeline as heimdall's
  8091 packets - spectrum, waterfall, VFO, decoder, DoA, the 📡 DF panel.
  Catalogue IN THE PLUGINS: `plugins/<id>/tools/testsignals.json` (a list;
  format + how to add one: plugins/SDK.md *Test transmissions for the debug
  playback*). 36 entries in 26 catalogues (2026-10-23) - EVERY decoder:
  ais, aprs, dstar, nxdn, tetra, mpt1327, ils (VOR), analog_id (two: the
  CTCSS tones + `analog-ids-six`, radio IDs), radar,
  ssr1030, uat, flarm, beacon406, lora (six), wildlife (five), dtv, cells,
  adsb, dme, ism, pocsag, v23_telemetry, p25, dmr, radiosonde (the last
  three since 2026-10-16 - their earlier tests used public recordings),
  rds (2026-10-22)
- Generation: `PLAYBACK_GEN:<id>` forks `python3 -I
  plugins/sdk/tools/make_testsignal.py <id> --plugins DIR --cache
  testsignal_cache --lat --lon` (nice 10, own process group; PROGRESS lines
  read by a thread; `PLAYBACK_GEN_CANCEL` = SIGTERM to the group). The
  driver runs the entry's `steps` (generators; .cpp compiled once into
  `.bin/`) in `testsignal_cache/.work-<id>/`, then `uca_scene.py --clean`
  (narrow) or takes the step's 2.4 MHz `output` (cf32 -> cu8 at rms 0.15),
  and writes `<id>.dat` + the FLAT sidecar `<id>.json` (format, rate,
  channels, samples, tuner_hz, band_offset_hz, radius_m, noise_rms,
  vfo_offset_khz, vfo_bw_khz, vfo_demod, vfo_mode, commands = "\n"-joined
  WebSocket commands, truth). Station lat / lon = `station_info.resolve()`
  at that moment (the cache is cwd-relative + gitignored; nothing with a
  position goes into the repo). Cache capped at 3 GB (`prune`, least
  recently played - start() touches the sidecar - go first)
- Source (`Source`, data receiver thread): `cf32` = every antenna at a
  narrow rate R (2.4 MHz / R whole), noise-free: polyphase interpolation x L
  (12 taps per branch, Kaiser 60 dB: passes 0.35 R, stops from 0.65 R),
  shifted to the band offset (rounded to 100 Hz -> a phasor table of one
  period), scaled as uca_scene --wide (A = 0.25 / max(3 sigma_w, 0.3)),
  noise sigma_w = noise_rms sqrt(L) per complex sample from a 65536-entry
  Gaussian table indexed by xorshift, 8 bits. `cu8` = the bytes at 2.4 MHz,
  sample-major -> channel-major. Looped (input index mod N), paced to real
  time (sleep_until in <= 20 ms steps; > 1 s behind = resync, no burst).
  Header: CONVERGED (4), no noise source, counter / group / retuning 0, every
  channel tuner_hz + 30.0 dB. Cost logged every ~28 s ("Playback <id>: x
  ms per packet = y % of a core"): narrow ~1.8 ms per 16384-sample packet =
  27 % of a Pi 5 core, wide 0.2-0.4 ms = 3-6 %
- `data_receiver_thread`: `playback::active()` decides the source per
  connection loop - active: data_client disconnected, `playback::read()`
  instead of `receive_data` into the same parser (so every header path is
  the real one); a switch either way breaks out and clears the raw buffer.
  Not connected to heimdall at startup = playback still works. A playback
  never outruns the pipeline: before each packet the thread waits while
  the raw buffer holds >= `PLAYBACK_MAX_QUEUED` 16 packets (a file has no
  deadline) - a machine too busy for 2.4 MHz x 5 antennas in real time
  plays it slower. It used to fill the 150-packet raw buffer, which then
  dropped whole packets: a test kraken_doa next to the live stack on the
  Pi 5 lost 25-40 % of a wide scene's packets with DoA on, and the ism
  plugin's bursts (cut in pieces) failed their checks - 2 of 6 devices -
  while ADS-B / DME's microsecond packets passed
- `DataReceiver::send_control_command`: dropped (logged "[playback] not
  sent to heimdall") while active - the single choke point, heimdall's
  tuning / heimdall_settings.conf are never touched. `handle_websocket_message`
  first runs `playback::handle_command` (PLAYBACK_*), then, while active and
  not replaying / not playback's own (`internal()`), `playback::refuse()`:
  FREQ (unless within 1 kHz of the playback's tuner: fixed-frequency
  decoders' own retune), GAIN / TUNER_* / TUNERS, OPERATING_MODE other than
  coherent, WIDEBAND_MODE / FREQ, MIXER_SIDE, LO_CURRENT, FREQ_OFFSET,
  scanner starts, RESET_SETTINGS - refused with `{"playback":{"notice":..}}` (a toast on every
  page). `request_plugin_tune` refuses too (cells / flarm scans)
- Settings: `SettingsStore::begin_sandbox()` (flush what is pending, copy
  the store, no file write until `end_sandbox()` - write_now is a no-op);
  the session's commands go into the store as usual. Stop: every VFO's
  decoder OFF (forgets the session's talkers), `end_sandbox()` (the copy
  back), `rdfmap::set_sandbox(false)`, `ControlHandler::
  apply_persisted_settings()` (the normal startup replay: VFOs, decoders,
  array, mode, FREQ / GAIN to heimdall - equal values, heimdall no-ops).
  Playback is never persisted: a crash / restart mid-playback loads the
  untouched file and comes back live. rdf_mapper: `set_sandbox(true)` saves
  rdf_session.bin and stops writing it, `false` drops everything the
  playback added (maps, talker entries) and reloads the file
- Session (`apply_session`, 1.2 s after start - deferred so the decoders
  just switched off can't report talkers into the new session; TalkerDoa
  also takes spans / spatial reports only from its VFO's current plugin
  now, `TalkerDoa::set_plugin`): `OPERATING_MODE:coherent`, `DECIMATORS:0|
  <offset>,<bw idx>,<demod>,0,-60,0,0,PLUGIN:<id>,,0,0` (one VFO, squelch
  off, heard), `DOA:1`, `TOPOLOGY:UCA`, `RADIUS:<scene mm>`, the entry's
  commands (its plugin options as DIGITAL_OPT) - all via
  `handle_websocket_message` with `internal()` set. Switching entries while
  playing keeps the sandbox (the user's state from before the first one)
- Paused while active (`playback::active()` checks): decoder data log
  (record handler, incidents, voice records), Voice to Text (taps), incidents
  (no Nominatim lookups), web mapper (no records), DoA logger, array
  calibration packets, noise_floors.json saves (the in-memory floors may
  learn the playback's level - re-learnt live within seconds). The 📼
  recorder's AUDIO recordings keep running (the way to get audio out of an
  IQ recording); an IQ recording is refused during any playback, and a
  playback while one runs
- Since 2026-10-16 the same machinery also plays 📼 IQ recordings (`RecSource`,
  `PLAYBACK_OPEN` ... - see *📼 IQ & audio recorder*); the header / pacing /
  interpolation helpers (`write_header`, `Pacer`, `design_interp`,
  `gauss_table`, `q8`) are shared by both sources, refusal texts say
  "Playback: ..."; the status carries `kind` "test" | "rec"
- Status `{"playback":{active, id, title, rf_hz, loop_s, pos_s, elapsed_s,
  decoder, gen{id, running, pct, text, error}, catalog:[{plugin, text}] -
  the catalogue files as TEXT (the page parses each: a broken file loses
  only its entries), cache:{id:{mb, generated, seconds}}}}` from a 1 Hz
  timer when it changed, every 2 s while playing / generating; `PLAYBACK_LIST`
  forces one (page connect). `PLAYBACK_START:<id>`, `PLAYBACK_STOP`,
  `PLAYBACK_DELETE:<id>`
- UI (kraken_doa.html "DEBUG PLAYBACK" block, `pb*`): sidebar box above
  Settings (the list: title + decoder, description on a click, ▶ Play / ■
  Stop, ⚙ Generate / ↻ Regenerate with a progress bar + Cancel, 🗑; the
  state; the Check), body class `pb-on` -> `#pb-banner` in the top bar
  ("🧪 DEBUG PLAYBACK - not live" + title + Stop) and `#pb-overlay` over
  the spectrum (big text + red outline); `#pb-toast` for notices. Check
  (`pbCheckRows`): each `expect` row vs `rdfStatus.talkers` (`tid` exact,
  else any talker; array-frame `doa`) within `tol` / `tol_deg` (5); a
  `vfo` row = that VFO's own DoA (`doaPlots[v].peakAngle`, the MUSIC plot
  - decoders without transmitter IDs, with the Digital squelch in the
  entry's commands), an `event` row = an event of the VFO's decoder
  containing the text, only those since its last "Decoder set to / on"
  (`pbSessionEvents`: the user's decoder before the playback doesn't count)
- Tests (scratch t6/: fake_heimdall_log.py - logs every control command and
  data connection -, run_pb.sh + t_pb.py, t_pb_ui.py): all 14 entries end to
  end on the scratch kraken_doa, every expected talker within 0.1 deg (CTCSS
  151.4 +3.5 - it captures another carrier, as offline); refusals (FREQ,
  GAIN, independent, RESET_SETTINGS, SCANNER_START) noticed; NO control
  command reached heimdall during playback, the user's FREQ / GAIN / mode
  sent at Stop, the 8091 connection back at once; doa_settings.json
  byte-identical after Stop and after a kill + restart mid-playback (live
  again, the user's VFO + decoder running); switching entries leaves no
  talkers of the previous one. UI (headless Chromium, real mouse): box,
  delete + Generate (progress, cached), Play -> banner + overlay, Check 3 / 3,
  DF talkers, toast, banner Stop. Live stack: see the commit
- Tests 2026-10-16 (scratch pb1/: a TEST build on ports 4708x / 4709x with
  nothing on 47091 - the playback needs no heimdall -, run_pb.sh + t_pb2.py
  - talkers, the VFO DoA from the multi-DoA binary message, the session's
  decoder events; jscheck.py - the page's scripts + pbCheckRows on mocked
  state): the five new entries 29 / 29 rows (adsb 6 aircraft, dme 2
  beacons + 2 idents + the TACAN, ism 6 devices - all 0.0-0.2 deg; pocsag
  6 pages + VFO 60.0, v23 4 packets + VFO 135.0), 0 packets dropped,
  doa_settings.json identical after Stop. VFO DoA over the last 27 s of
  the pocsag run: 60.0 .. 60.0 with the Digital squelch, 17.7 .. 352.7 with
  the squelch off (the FM interferer and the noise between the pages)
- Tests 2026-10-16, the WHOLE catalogue (scratch pb2/: the same test
  build, niced below the live stack, every entry generated fresh or taken
  from a cache generated after its generator last changed; 45 s per entry,
  80 s for ils and the wildlife entries): 31 of 32 entries pass every row -
  adsb 6/6, ais 4/4, aprs 3/3, beacon406 5/5, cells 2/2, ctcss 4/4 (151.4
  +3.6 deg, as offline), dme 5/5, dmr 4/4, dstar 2/2, dtv 3/3, flarm 5/5,
  ils 2/2, ism 6/6, lora-mesh-five 6/6, lora-au915-gateway 6/6, lora-eu868
  4/4, mpt1327 2/2, nxdn 2/2, p25 4/4, pocsag 7/7, radar 3/3, radiosonde
  2/2, ssr1030 4/4, tetra 2/2, uat 5/5, v23 5/5, wildlife-five 5/5,
  -survey 5/5, -one-collar 1/1, -na-collars 4/4, -motus-tags 4/4 (talkers
  0.0 deg except the wildlife ones, within 5); `lora-private-search` 1 / 5
  (also alone for 120 s): through the playback the plan-free search
  learns only the 446.1 MHz SF10 link, while the same file offline (`lora
  decoder --file <cache>.dat --format cu8 --channels 5 --rf 446e6`) learns
  all five within 4 s. Its facts: "Search ... 14.0 % CPU · 151 candidates,
  4 probed · paused (CPU budget)" - the search's share is wall-clock time
  per stream time, and with the live stack + the test build on the Pi
  (load 5-9) it passed its 12 % limit and stopped probing (once the one
  link learned at 446.069 instead of 446.100: 0 / 5) - open, for the lora
  plugin. Settings file identical after every run, 0 notices

