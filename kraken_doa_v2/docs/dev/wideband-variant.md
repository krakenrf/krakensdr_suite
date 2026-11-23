# KrakenSDR Wideband (downconverter) variant

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**KrakenSDR Wideband (downconverter) variant:**
- Run with `--wideband` (heimdall too) - see `../docs/dev/suite-reference.md` *KrakenSDR Wideband (Downconverter) Variant* for the design
- `WB_VARIANT_IF_HZ` (1268 MHz), `WB_LO_MIN_HZ`/`WB_LO_MAX_HZ`: must match
  `heimdall_v2/config.h`; give the UI its RF limits and the LO readout
- Mixer side FOLLOWS THE FREQUENCY: the FREQ handler auto-selects when the
  current side can't reach the RF or the retune crosses
  `WB_SIDE_CROSSOVER_HZ` (`wb_side_for_retune`; `wb_auto_side`: high below
  2200 MHz, below from it up - the high side loses conversion gain as its LO
  climbs) and records/broadcasts the change; heimdall applies the same rule
  on its own retunes. `MIXER_SIDE:high|low|below` is a manual override for
  RFs several sides can reach (image dodging) - rejected (and the persisted
  value restored) when the side can't reach the current RF; the UI greys
  those buttons out per frequency. high: LO = IF + RF (24-4132 MHz); low:
  LO = IF - RF (24-1183 MHz); below: LO = RF - IF (1353-6668 MHz). Persisted,
  replayed BEFORE `FREQ:` on startup; tuning limits are the union span
  (`wb_variant_rf_union_range`, 24-6668 MHz)
- Antenna ring FOLLOWS THE FREQUENCY (outer < 1 GHz, center 1-2.5 GHz, inner
  above; `wb_ring_for_rf`, must match heimdall's `WB_RING_*_MIN_HZ`). The
  ring buttons and the `ARRAY:` command/persistence are gone; heimdall throws
  the switches itself on retunes, the client mirrors the rule for the
  toolbar ring indicator (`wbv-ring-ind`) and the Wideband topology radius
- The top bar's mixer field (`#wbv-group`; since 2026-11-18 the `mixer`
  icon - a circle with a cross - in place of its "Mixer" caption, the ring /
  LO readout right above the High / Low / Below buttons and the LO drive,
  right edges together; *sidebar.md* "Top bar and rail"). With it the bar
  does not fit one row at 1700 px: the controls keep the first row and the
  status block wraps under them
- DoA topology `TOPOLOGY:WIDEBAND` (client-only, wb variant only, falls back
  to UCA otherwise): UCA steering math with the array radius auto-set from
  the active ring - `WB_RING_RADIUS_MM` = 127.5 / 51 / 20.4 mm (outer /
  center / inner). `wb_topology_active` gates it; `RADIUS:` commands are
  ignored while active, and the UI re-sends the user radius when switching
  back to UCA. Topology panel shows the ring + auto radius status
- WebSocket command `LO_CURRENT:0..7` sets the LO synthesizer output drive
  current (relayed to heimdall as `set_lo_current`; no recalibration - the LO
  is shared by all mixers so the change is common-mode; persisted). Shown as
  the "Cur" selector next to the Mix buttons
- Backend pushes `{"wb_variant":{...}}` to browsers (connect + side/ring
  changes), including the hardware constants (IF, LO span, ring boundaries
  and radii) so the UI computes side availability / ring locally per
  frequency; `kraken_doa.html` shows the ring indicator, the Mix
  High/Low/Below buttons (unavailable sides greyed) and the LO/IF readout
- Frequency stays the true RF everywhere (MUSIC wavelength, spectrum axis);
  heimdall corrects high-side spectral inversion at the source, so no DSP
  path here is orientation-aware. The tuner-spread wideband SCAN mode and its
  UI are hidden in this variant (tuners must stay parked at the IF)
- Array chirality: see the UCA ordering note under *Configuration* - the
  Wideband boards and the standard arrays are both treated as CLOCKWISE, so
  `uca_angle_sign()` is variant-independent
- Board firmware line (the WB section, `updateWbTelemetry` in the page): heimdall's
  `downconverter.fw` - version, an alarm when tuning falls back to `'w'`
  (`set_cmd`), an alarm when the LO synthesizer is not confirmed in
  generator mode (`lo_mode` other than `generator`: `no generator mode`,
  `generator mode unconfirmed`, `LO mode not set`; heimdall's
  `docs/dev/wideband-variant.md`), unanswered commands (`no_reply`). Changes
  of `set_cmd` and `lo_mode` are logged to the board's event log
  (`wb_logs/wb_events_YYYYMMDD.csv`, below; `wb_board.cpp`). An older
  heimdall sends no `lo_mode`: no alarm.

**Telemetry logs: the board's and the computer's (`wb_board.cpp`; PR #5 added the host log,
the compression came with its follow-up, 2026-11-23):**
- **The files.** Two logs, one folder each under kraken_doa's working
  directory: `wb_logs/` (the Wideband board, rows only while heimdall sends
  its `downconverter` status) and `host_logs/` (the computer: CPU
  temperature and load, memory, free / total space of the logs' drive, the
  Pi's throttling flags - every build). Per UTC day one CSV of rows
  (`wb_telemetry_YYYYMMDD.csv` / `host_telemetry_YYYYMMDD.csv`, one row every
  WB_LOG_PERIOD s, default 10) and one of events (`wb_events_YYYYMMDD.csv` /
  `host_events_YYYYMMDD.csv`: alarm changes, firmware warnings, lost /
  restored data). Measured on synthetic days with realistic noise (400
  days, rows every 10 s): board rows 873 kB / day plain -> 75 kB gzipped
  (11.6x: most columns are constant), host rows 482 kB -> 71 kB (6.8x); a
  real receiver's ~0.8 / ~0.55 MB a day plain (PR #5) gives about the same
  (the ratio follows the noise in the readings). An event file is a few
  hundred bytes; gzip gains little there, it is kept for one rule.
- **The rule: plain for N days, then gzipped for M more, then deleted** -
  the decoder log's (*decoder-log.md* "Plain for N days, then compressed
  for M more") and its code: `utils/log_retention.hpp` (`retention_action`,
  `gzip_file`, `run_pass`, `Scheduler`), extracted from `decoder_log.cpp`
  so both logs run the same lines. **Keep** (`WB_LOG_KEEP:`, N, 1-365 - no
  "forever" here, as before) is the plain days: a stored value stays, a new
  install gets **7** (it was 30, with deletion after it). **Then
  compressed** (`WB_LOG_ZIP:`, M, new, 0-3650, default **365**): the days a
  finished day is kept as `<name>.csv.gz` (one gzip per day, in place,
  `zcat` reads it) after the plain ones; **0 = the old rule** (deleted after
  Keep). Both logs share the two settings (the sheet's "Logging (both
  logs)"), persisted like the others (`wb_log_keep_days`,
  `wb_log_zip_days`; an updated receiver's settings file keeps its stored
  Keep - 30 on most - and gets Then compressed 365 backfilled). By the age
  of the day in the name (UTC; today 0): plain while age <= N, gzipped while
  N < age <= N + M, deleted after; today's files are never touched; a
  `.csv.gz.part` (a gzip a crash interrupted - the plain file is still
  there) is removed and the day compressed again; a day with both the plain
  file and its gz (a crash between the last two steps) is read once (the
  plain one) and gzipped again.
- **The pass** (`passes()`, a `logret::Scheduler`: its own thread, one at a
  time, both logs in one pass) runs **not before the settings replay**
  (`wb_board_settings_restored()`, called next to the decoder log's
  `settings_restored()` at the end of
  `ControlHandler::apply_persisted_settings`): until then the stored Keep
  is not known, and a pass with the defaults could compress or delete days
  the stored settings keep (the decoder log's lesson). Then on a change of
  Keep / Then compressed, at the UTC day change and every hour (the
  sampler thread asks). Before, `purge_old_logs` ran from the row append -
  on the uWS loop for the host log, at the first row after the start, with
  the default 30 days.
- **An older build's files** (no date in the name: the size-rotated
  `wb_telemetry.csv[.N]` of the first board log, PR #5's
  `wb_events.csv[.1-.3]` / `host_events.csv`): still read by the history,
  never compressed, deleted by their modification time after Keep + Then
  compressed days. Finished exports (`<tag>_export_*`) after an hour, as
  before. Nothing else in the folders is touched (only these names are
  listed).
- **Events: daily files, not the rotated log gzipped (decision).** PR #5's
  event log rotated at 5 MB (3 old files). Gzipping the rotated `.1` - `.3`
  would have needed the rotation and the gzip to lock each other out (a
  rotation renaming `.1` while the pass gzips it, then unlinks the NEW `.1`,
  loses data), a gzip outside the day rule, and events kept for as long as
  20 MB lasts - years. Daily files take the same rule, lister and reader as
  the rows (the pass only ever touches days >= 2 old, never a file being
  written), and an event lives as long as the rows around it. The 5 MB
  bound stays as a cap per day: a day's event file stops at 5 MB (an alarm
  flapping all day), with one `log_events` line saying so.
- **Reading** (`history_json`, `export_csv`, on their worker threads): each
  day's file through zlib (`gzopen` / `gzgets` read plain and gzip files
  alike), the plain one when both exist; a day the pass gzipped after the
  listing is read from its `.gz`. Same rows, same buckets as with every file
  plain (tested below). The history's `log` object adds `plain_bytes`,
  `plain_files`, `gz_bytes`, `gz_files` (`bytes` / `files` are now every
  file of the log: rows and events, plain and gz, an older build's).
  `export.csv?...&gz=1` gives the same rows gzipped (`*_last_7d.csv.gz`).
- **The sampler thread (`sampler()`, started at the first `host_log_tick`
  or at the settings replay).** `host_log_tick` runs on the uWS loop with
  every status (2 Hz); its statfs (`std::filesystem::space` of the working
  directory) and - on a Pi 5, which has no sysfs file for it - `vcgencmd
  get_throttled` (a child process; now through `kproc::run_capture` with a
  5 s timeout instead of `popen`, which leaked our sockets into the shell)
  ran on that loop every 10 s: a slow vcgencmd delayed the spectrum, audio
  and every command by as long. They moved to the sampler, which measures
  every 10 s; `host_log_tick` only reads its last values (the first row and
  the storage alarm wait for its first measurement, a few ms after the
  start). Measured with a fake vcgencmd that takes 500 ms first on PATH,
  65 s of the status on the TEST page's WebSocket: this build 130 messages
  at 2.000 Hz, max interval 502 ms; the old build a 1003 ms gap at every
  vcgencmd call (6 in 65 s - the loop, and with it the spectrum, audio and
  every command, stood still for 500 ms each time).
- **The page** (the telemetry sheet, `wbSheetOpen`; "Logging (both logs)"):
  Keep [7 days / 30 / 90 / 1 year] and **Then compressed** [none / 30 days
  / 90 / 1 year / 3 years / 10 years] (another stored value is added to its
  list), a `?` beside them that says the rule with the numbers in force
  (`wbLogRule`: "Keep 7, Then compressed 365: the last 7 days plain, the 365
  days before those (days 8 - 372) compressed, older days deleted"), both
  disabled while logging is off. The size line (`wbLogInfo`): "4.1 MB plain
  + 12.3 MB compressed in 742 files, from 2025-10-20", the plain / gz file
  counts and the rule in its tooltip. A `.csv.gz` download beside Download
  CSV for the 7 / 30 day ranges. The values ride `system_status`
  (`hw.logging` / `wb_telemetry.logging`: `keep_days`, `zip_days`).
- **Compatibility.** An older page with this receiver: Keep only (`zip_days`
  ignored); the receiver keeps gzipping with the stored Then compressed.
  This page with an older receiver (no `zip_days`): no Then compressed, no
  tip, no `.csv.gz` button, the old size line. The WB_LOG_* commands now
  save and echo the clamped value (`set_applied`; a WB_LOG_KEEP:900 was
  saved as 900 by the generic path after the handler saved 365).
- **Tested (2026-11-23, scratch `w_logs/`)**: the rule (a C++ harness:
  invariants on 5 M random cases, a model of the pass with a crash before
  every file operation, the real `run_pass` / `gzip_file` on every state a
  crash of `gzip_file` leaves; nine mutants of the rule / the gzip order
  each caught); the decoder log unchanged (the shared rule against a
  verbatim copy of the old function on 5.6 M cases incl. N = 0 / M = 0; the
  old and the new build's passes on the same fabricated decoder log leave
  the same files); end to end, a TEST receiver on 400 synthetic days per
  log (before the replay no pass; after it the stored Keep 30 and Then
  compressed 365; every day plain / gz / gone as the rule says, the gz
  content = the rows, the crash states repaired, the older build's files by
  mtime, foreign files untouched; the history for 1 h - 366 d across the
  boundary = a reference over all-plain files, bucket by bucket, and the
  events; the 30 d export, plain and `.csv.gz` = the concatenated rows;
  Keep 7 / 2 and Then compressed 0 by command; a second pass changing
  nothing); the cadence with a 500 ms vcgencmd; the page in headless
  Chromium.

