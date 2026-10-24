# Saved-frequency scan

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**📻 Saved-frequency scan (`src/sf_scanner.cpp`, `src/sf_scan_plan.cpp`, UI "📻 SAVED FREQUENCY SCAN" in kraken_doa.html):**
- What it scans is part of the saved-frequency list (saved_freqs.cpp, so
  every page shows the same ticks and it survives restarts): a CATEGORY's
  `scan` (bool) takes every frequency in it - also those added later -,
  `scan_uncat` (top level of the file / message) the uncategorised ones; a
  FREQUENCY's `scan` 1 = scanned on its own, -1 = left out even though its
  category is scanned, 0 = its category decides. `SF_SCAN:{"freqs":{id:-1|0|1},
  "cats":{id:bool},"uncat":bool}` (any subset; ids gone meanwhile skipped; a
  bad value refuses the whole command - `{"savedfreq":...,"error","id":"sfscan"}`)
  -> the whole list to every page like every SF_ command. SF_SET / SF_CAT_SET
  WITHOUT "scan" keep the entry's (the editor / ⚙ Categories don't send it);
  a frequency moved to another category, or left without one by SF_CAT_DEL,
  drops a -1. `savedfreq::scan_entries()` (the effective list, lowest first)
  and `savedfreq::generation()` (bumped by every save) feed the scanner
- Plan (`sf_scan_plan.cpp`, no receiver state - tested offline): entries
  outside the tunable range (R820T, or the Wideband variant's union) or with
  a decoder that holds its VFO (`fixed_freq_hz` / `lock_vfo` plugins: ADS-B,
  AIS, ism, cells, dtv, lora, radar ...) are `skipped` with a reason. The rest
  go into BANDS = one tuner centre (whole kHz, clamped to the range) whose
  usable span (edge clip x 2.4 MHz) holds every member's VFO (its bandwidth
  around its frequency: the entry's, else its demod's usual - WBFM 240 kHz,
  NBFM / AM 16 kHz, what the page's demod buttons set -, else the VFO's,
  capped at 240 kHz for fitting): entries with a gain - per gain, lowest
  first, a new band when the next doesn't fit; then the rest into the first
  band they fit (any gain), else bands of their own. BALANCED: the packing
  width is the narrowest (binary search) that still needs no more bands
  than the full span - packed to the full span, the user's six broadcast
  stations 0.8 MHz apart came out {95.8, 96.6, 97.4} with 95.8 at -840 kHz
  (the tuner band's edge) and 96.6 40 kHz from DC; balanced: three pairs at
  +-400 kHz (2026-10-22). Bands lowest centre
  first. The CENTRE keeps every channel off DC (`pick_centre`: half its
  channel + `DC_GUARD_HZ` 3 kHz away - the IQ path takes the DC off and an
  RTL-SDR has its spike there: a band of ONE entry centred on it never saw
  its carrier, found by the e2e test), the nearest such centre to the middle
  that still holds every VFO, else (channels every few kHz) the one farthest
  from them. `band_fits()` = a centre that already holds a band, channels
  off DC (or the plan's own centre) - no retune.
  Level of an entry = strongest bin in the inner 80 % of its channel (its
  bandwidth, else 180 kHz WBFM / 12.5 kHz) of the averaged FFT, minus the
  channel's noise floor (`FFTProcessor::channel_noise_floor`)
- WIDEBAND SCAN MODE (the user's question 2026-10-22: "why does this
  scanner not work in wideband mode? I see no reason why it should not" -
  there was none: it was refused only because the plan knew one tuner per
  band): `Layout` = the tuners a band is received with - `single_layout(span)`
  ({0}, half = span / 2: coherent / independent, exactly the arithmetic from
  before) or `spread_layout(n, spacing)` = heimdall's setup_wideband_frequencies
  (tuner k on centre + (k - n / 2) x spacing, integer n / 2; spacing =
  uint32(SAMPLE_RATE x edge clip) in float maths as there: 2160000 at 0.9;
  half = spacing / 2). An entry is received on the tuner NEAREST it (lowest
  on a tie - the receiver's SET_DECIMATOR_FREQ picks a wideband VFO's tuner
  that way), so its VFO must lie inside THAT tuner's usable part (not across
  a seam) and its channel off THAT tuner's DC. `pick_centre` searches the
  centres whose tuners cover the band (every tuner inside the tunable range)
  for one with every VFO inside its tuner (feasible) and DC-clear, nearest
  the middle; else the feasible one farthest from DC; else the least
  overlap. An entry still across a seam at its band's centre is planned
  again (a band of its own, `SEAM_ROUNDS` 4, then kept). Unit test: the six
  broadcast stations = 1 band (3 on one tuner), 300 dense entries over 11.2
  MHz = 3 bands (6 on one tuner), none across a seam. Scanner: one slot (ch
  -1), the band's centre = tuner n / 2's (`wide_centre_hz` from the stream,
  whole kHz), retune WIDEBAND_FREQ (FREQ ends wideband mode), SETTLE until
  EVERY tuner is at centre + its offset (`in_stream`), the VFO pre-placed
  only then (the receiver picks a wideband VFO's tuner from the tuners'
  stream frequencies) at its offset from the spread's middle ((first + last
  tuner) / 2 - tuner n / 2 with an odd count), each entry measured on its
  nearest tuner's FFT (`entry_tuner`, `Specs` = each tuner's spectrum once
  per tick) with the tuners' common response taken off first
  (`nfloor::shape_db`, as the stitched spectrum does: its floor -
  `channel_noise_floor` = nfloor's in wideband mode - is that flattened
  spectrum's, so the level = what the page shows; measured on the raw
  spectrum against that floor, plain noise read 15 dB on one tuner after the
  merge of the wideband alignment, 13396fb, in the test), no calibration /
  hold; the plan is re-made when the edge clip or the tuner count changes
  (heimdall respaces the tuners). A manual WIDEBAND_FREQ stops it; at stop
  the gain is saved (no FREQ in wideband, as before). Test:
  `t_sfscan_wide.py` (the fake spreads its tuners like heimdall and, in
  wideband mode, gives them the real response shape - WB_SHAPE_DEFAULT read
  from noise_floor.cpp: with flat noise the alignment measured gain steps of
  several dB between tuners and chained them into one tuner's floor)
- Scanner (`sf_scanner.cpp`, one thread, 100 ms ticks; class `Scan`, state
  TUNE -> SETTLE -> WATCH -> LISTEN): SLOTS = the array (coherent: 1, the
  FFT of `active_channel`; wideband: 1, the whole spread) or every tuner NO
  OTHER VFO is on (independent;
  another VFO moving onto a scanned tuner = a new turn without it, none left
  = stop). TURNS = the bands in slot-sized blocks (slot k: band cycle + k,
  none past the last band); a PASS = every turn once, then from the first
  again. TUNE: a slot whose centre doesn't hold its band gets FREQ: /
  TUNER_FREQ:, a band's gain GAIN: / TUNER_GAIN: when it differs, and the
  VFO is PRE-PLACED (`place(.., heard=false)`, muted) on the first retuned
  band's first entry that hasn't had its turn - in the same go as the
  retune (the user's idea, 2026-10-22): during the coherent calibration
  that follows its filters settle / its decoder starts, so it is in place
  the moment the data is clean (also on the first turn of a run). SETTLE:
  the stream's header frequency (`tuner_frequencies`) within 2 kHz (6 s max,
  else that band is left out this turn: `ok:false`) + 250 ms, then an FFT
  averaging reset (`fft_reset_generation`; independent retunes don't get one
  from heimdall). READY = not `server_retuning` (new global: the header's
  retuning flag - packets are skipped meanwhile, the spectrum is the old
  band), `fft_data_valid`, `rebuild_ms()` since the last FFT reset (until
  the reset's first frame weighs < 5 % in the average, from AVG and the FFT
  size, + 0.1 s, 0.25-2.5 s: 0.25 s at the defaults - was a fixed 1 s, the
  continuous scanner's) and, coherent, not calibrating: noise source on / within
  `NOISE_QUIET_MS`, or phase state 0-3 / 6 (WAITING_STABILITY 5 is fine:
  noise off, antennas in) - the phase state alone is ignored after 20 s
  (a heimdall that never settles); KerberosSDR manual mode: noise only.
  WATCH: an entry over the squelch on 2 polls in a row is active (`act`);
  entries in `done_` (left by Skip / Longest stay: had their turn this
  pass) wait; the first other active one after the last heard (list order,
  wrapping) is LOCKED. Nothing to lock: with more bands than slots the
  next turn comes AT ONCE when something active here is done (its turn is
  over - the others' bands next), on Skip, or after DWELL of ready watching
  (summed over the turn, not restarted per transmission - a busy channel
  can't hold its band for good), not while held; past the last band = a new
  pass (`done_` cleared). Every band watched at once: a new pass as soon as
  all that is active is done. (Was: Skip / Longest stay excluded a
  frequency until it measured quiet - a broadcast station never does, so
  after one pass every station was out and the scan cycled its bands with
  the VFO parked, muted, on the last one: the user's report 2026-10-22.)
  LOCK / pre-place (`place`), on the uWS loop as `sfTune` in the page: DIGITAL_MODE (the entry's decoder),
  SET_DECIMATOR_DEMOD (+ that demod's usual bandwidth), SET_DECIMATOR_BW_HZ
  (the entry's, exactly - any width since the arbitrary VFO bandwidth, merged
  2026-10-22: the scanner used to snap it to the nearest of the 30 old sizes;
  Digital: widened to the narrowest efficient size holding the decoder's
  `min_vfo_rate`, `efficient_bandwidth_at_least` - 24 kHz for Auto; `Cfg::bw`
  is Hz), SET_DECIMATOR_FREQ (offset from the slot's centre; independent
  `:tuner`), the VFO heard (pre-placing: muted). LISTEN: the entry measured
  on its slot; over squelch - 3 dB keeps it, not ready (calibration) counts
  as active AND MUTES the VFO (the noise source would be heard; heard again
  the tick the data is clean - status `cur.heard`); resumes after HANG of
  quiet, or after MAX of CLEAN listening (`listened_ms_`, status `cur.ms` -
  calibration time used to count: a 2 s stay could pass in noise-source
  time) -> done; SKIP -> done - in WATCH it advances the turn; HOLD stays
  (WATCH: no turn advance). Resume mutes the VFO; a plan change while
  listening re-tunes when it resumes (`need_tune_`)
- CALIBRATION HOLD (coherent, not KerberosSDR manual mode; the user's call
  2026-10-22: "while it's scanning and not found a frequency, turn
  calibration off; begin it once a signal is found"): `Scan::hold()` after
  every tick - not listening: heimdall's `hold_calibration` lease (3 s,
  renewed every 1 s; heimdall_v2/CLAUDE.md *Calibration hold*), so a retune
  only puts heimdall in WAITING_FOR_STABILITY (spectrum clean, DoA held -
  READY for the scan) instead of the 3 s cooldown + ~0.5-1 s noise-source
  calibration per band; a LOCK releases it (`hold_ms` 0: the calibration
  starts at once); stop() releases it too (the array calibrates where the
  scan stopped). A lock with the phase state WAITING_FOR_STABILITY (i.e. a
  calibration now pending) is placed MUTED (`cal_wait_`) until the
  calibration has run (`cal_seen_` + ready) - heard once, calibrated, DoA
  valid, instead of a blip cut off by the noise source; no calibration
  starting within 1.5 s (an older heimdall: unknown command, its own 3 s
  cooldown) unmutes it. Longest stay counts heard time only. The e2e test's
  fake heimdall models it (cooldown expiry only when not held, release =
  calibration now, `cal_count`): 0 calibrations across the searched bands
  with a 3 s heimdall cooldown, the release at the lock, the calibration
  starting at once, heard ~1.5 s after the lock. The per-lock state above
  (`cal_wait`, `cal_seen`, heard, listened time) lives in each `Lock`
- ONE VFO PER ACTIVE FREQUENCY (`SFSCAN_MULTI:0|1`, `g_multi`, schema
  `sfscan_multi`, off by default; the user's request 2026-10-22 "creates as
  many decimators as there are active signals in one band - for whisper
  recording etc"): `Scan::locks_` (a `Lock` per frequency listened to; off
  = at most one, the scan's VFO - the old single-lock behaviour). WATCH
  locks every active entry (round the list from the last heard) while
  `can_lock()`: the scan's VFO when free, else a VFO the SCAN MAKES (token
  `Lock::tok`, `g_extras`: token -> {vfo, weak_ptr to its instance} -
  ids are reused, so a VFO counts as the scan's only while the manager
  still holds THAT instance: one the user removed and replaced under the
  same id is never touched), while the manager's 16 VFOs (`VFO_LIMIT` =
  its private MAX_DECIMATORS) aren't all taken. LISTEN measures each lock
  on its slot and, with multi on, the bands watched (`measure()`) - a
  frequency that becomes active gets its VFO then (not one done this
  pass). Each lock ends on its own (Delay -> not done, Longest stay ->
  done; Skip ends all, done); the band moves on only when none is left
  (`after_locks`). `make_extra` (uWS loop): `addDecimator()` (muted, 0 Hz),
  then the scan VFO's settings recorded at start (`Cfg::demod / bw / dec /
  sq_* / transcribe`) through apply_command, then `apply_entry` (the
  entry's decoder / demod / bandwidth / offset / tuner - shared with the
  scan's VFO). `drop_extra` / stop(): muted first (removing the last VFO
  heard makes the first one left heard), `removeDecimator` +
  `audio_mixer.retain`, out of `g_extras` only after the removal
  (`free_tuners` skips the scan's own VFOs - independent mode would take
  one for another VFO on a scanned tuner and start a new turn). Made /
  removed through the manager with a `decimator_info` broadcast, NOT
  ADD_ / REMOVE_DECIMATOR: their decimator_added / _removed messages make
  every page reset its audio player (a gap in what the other VFOs play).
  A made VFO the user removes (`extra_vfo` -2) = that frequency done this
  pass (else it got a new VFO at once); one that couldn't be made (-3) =
  not done. Switching multi off while listening ends the made VFOs' locks.
  stop() removes them all BEFORE `set_on_stop` saves the VFO snapshot: they
  are never part of the user's setup. Not followed by the 📼 audio
  recorder (its VFO list is fixed when it starts); Voice to Text, decoders,
  DoA, 🔊 work per VFO, so the made ones get them
- Commands run through `ControlHandler::apply_command` on the uWS loop
  (`loop->defer`, dropped when the run ended - `alive(run id)`), so pages see
  them like a browser's; `sfscan::dispatching()` (set around them) keeps them
  out of the settings file (the generic record, TUNER_FREQ's TUNERS:, FREQ's
  independent TUNERS:) and from stopping the scan; `record_decimator_snapshot`
  skips while it runs (as for the other scanners). At stop `set_on_stop` ->
  `persist_after_sf_scan` (control_handler.cpp) saves the live FREQ / GAIN
  (or TUNERS) + the VFO snapshot: what the scan left is the user's now.
  Shutdown only joins the thread (nothing saved)
- Stops it: SFSCAN_STOP, a manual FREQ (any mode), TUNER_FREQ of a scanned
  tuner, a manual WIDEBAND_FREQ, OPERATING_MODE to another mode, heimdall switching mode
  (`note_server_mode`), its VFO removed, a playback, the plan left empty
  (each with `msg`). Refused: a playback, another scanner
  running (and the other two refuse while it runs), a VFO its decoder holds,
  nothing scannable. The VFO: the page's selected one (`SFSCAN_START:<id>`,
  -1 / unknown = the first). Muted while scanning, heard while locked; stop
  keeps it heard if it was listening, else as before the scan. (Muting it
  in independent mode exposed a crash in data_receiver.cpp's decimation
  pass: FM / digital only with NO VFO heard left the pass without VFOs and
  `reserve(size() - 1)` threw - the process ended; muting every VFO by hand
  did the same. Fixed there: an empty pass does nothing)
- Status `{"sfscan":{run, state tune|watch|listen, hold, vfo, mode, n, nb,
  cycle, ready, watch:[{b,ch,hz,lo,hi,n,ok}], cur:{id,name,hz,db,ms,quiet,
  heard,vfo,extra} | null, locks:[the same, every one listened to - vfo -1 =
  being made], act:[ids <= 50], skipped:[{id,name,why} <= 50], nskipped, sq,
  dwell, hang, max, multi[, msg][, err]}}` to every page: on changes and
  every 500 ms while running; GET_SFSCAN (page connect). `cur` = the first
  lock (pages from before `locks` read it). `err` = a refused start (the
  page that pressed ▶ shows it); `msg` = why it stopped. SFSCAN_* are
  `is_query_command`s; SFSCAN_SQUELCH (3-40 dB, 0.5 steps) / DWELL (500-60000
  ms) / HANG (0-60000) / MAX (0-3600000, 0 = no limit) / MULTI (0 / 1) are
  saved by the handler (schema `sfscan_*`, defaults 12 dB / 2 s / 2 s / 0 /
  off)
- Page: a tick per row and per category heading (`sfsCatState`: 2 checked,
  1 indeterminate - `data-mixed`, set after innerHTML -, 0); a row's tick
  toggles its effective state (`sfsToggleFreq`: -1 / 0 / 1 by its group), a
  heading's = all / none of it (its rows back to 0); clicks are
  preventDefault'ed (no optimistic update - the list's broadcast redraws).
  `#sfs` panel above the categories: ▶ Scan (`sfsStart`, the selected VFO) /
  ■ Stop / ⏸ Hold / ⏭ Skip, "N ticked", ⚙ settings (squelch, delay, dwell,
  longest stay, "VFOs: one per active frequency" - `#sfs-multi`, its row
  hidden for a receiver without `multi` -, tick all / none), the status
  (`sfsRender`: a line per frequency listened to, 🔊 name · MHz · dB · time
  → its VFO ("a new VFO…" while being made) - `sfsLocks(st)` = `locks`, or
  `cur` from an older receiver -, or "Scanning N frequencies", the watched
  band(s) + tuners, "N left out" with the reasons as its tooltip). Rows:
  `.sfs-cur` (🔊, green outline - every one listened to), `.sfs-act` (orange
  dot) - `sfsMark` after every list render and status. While the scan RUNS its squelch is drawn as a DOTTED
  yellow line across the main spectrum (`sfsDrawSquelch` from drawBox,
  beside the other scanners' cyan / magenta lines; label "📻 Scan N dB") and
  on the independent panes of the tuners it watches (`sfsPaneSquelch` from
  paneDrawOverlay); it follows the squelch slider while dragged (`sfsSqDrag`,
  globals block - drawBox can run before the section). No show / hide
  button (the user's call, 2026-10-22: always while scanning, never
  otherwise). A VFO's FFT squelch is the DASHED yellow line inside its bar
- Tests (job scratch, not in the repo): `t_sfscan.cpp` (saved_freqs.cpp
  scan flags - category / single / left out, editor saves keep them,
  refusals change nothing, reload, category delete - and sf_scan_plan.cpp:
  bands, gains, range ends, 300 entries, levels), `t_sfscan_e2e.py` (a TEST
  build - ports 28080/28081/28091/28092, `KRAKEN_AI_MCP_PORT` - fed by
  `fake_heimdall.py`: synthetic carriers in 8091 packets, the 8092 commands
  obeyed, coherent retunes with the retuning flag, WAITING_STABILITY and a
  noise-source calibration; checks the plan, both bands tuned, the gain
  with its band, lock / VFO / audio / delay / skip / hold / longest stay,
  list changes, refusals, manual-retune stop, what is saved, the user's
  broadcast-FM case - 5 always-on stations in 3 bands, Longest stay 2 s, a
  1.5 s stability window so a lock straddles the calibration: every station
  in list order pass after pass, 2 s of clean time each, muted / heard
  again across the calibration, the VFO pre-placed > 0.8 s before its lock -,
  independent mode with two bands on two tuners and a VFO taking a tuner;
  the fake's timing is set over its side port, `{"timing":{"stab":1.5}}`),
  `t_sfscan_wide.py` (wideband scan mode, 30 checks: the six stations in
  one band, every one inside its tuner + off DC, no calibration hold, never
  out of wideband mode, the VFO on its nearest tuner at the right offset,
  two VFOs on two tuners with multi, the bands taking turns by
  WIDEBAND_FREQ, an edge clip change re-planned + respaced, a manual
  WIDEBAND_FREQ stops it),
  `t_sfscan_multi.py` (one VFO per active frequency, 35 checks: the setting
  saved; two active at once - the scan's VFO + a made one at their
  offsets, the entry's demod / bandwidth, Transcribe + squelch copied, both
  heard after the calibration, no decimator_added -, a third while
  listening, one ending after the Delay -> its VFO removed, a made VFO
  removed by hand -> no new one for its still-active frequency, multi off
  while listening, stop -> only the scan's VFO saved; independent mode: two
  on one tuner, the made VFO never taken for a foreign one),
  `t_sfscan_ui.py` (the same + the page in a headless Edge with its own
  profile over the DevTools protocol: ticks per row / heading incl. the
  mixed state and -1 / 0 / 1, counts, ▶ / ■ / Hold / Skip, the 🔊 row, the
  status + "left out" tooltip, the settings inputs, the multi tick + a
  status line and 🔊 row per frequency, tick all / none, the editor keeping
  a tick, no page exceptions). On a Windows PC with the live
  stack running the test build gets its own ports, cwd and exe name (the
  launcher's stale-process sweep), builds through `C:\msys64\usr\bin\bash
  -lc` (Git Bash's runtime loses TMP for gcc)

