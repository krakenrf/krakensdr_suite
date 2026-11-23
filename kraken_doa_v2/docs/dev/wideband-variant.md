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
  compressed** (`WB_LOG_ZIP:`, M, new, 0-3650, default **30**, as the decoder log; it was 365 until 2026-11-23): the days a
  finished day is kept as `<name>.csv.gz` (one gzip per day, in place,
  `zcat` reads it) after the plain ones; **0 = the old rule** (deleted after
  Keep). Both logs share the two settings (the sheet's "Logging (both
  logs)"), persisted like the others (`wb_log_keep_days`,
  `wb_log_zip_days`; an updated receiver's settings file keeps its stored
  Keep - 30 on most - and gets Then compressed 30 backfilled). By the age
  of the day in the name (UTC; today 0): plain while age <= N, gzipped while
  N < age <= N + M, deleted after; today's files are never touched; a
  `.csv.gz.part` (a gzip a crash interrupted - the plain file is still
  there) is removed and the day compressed again.
- **The gzip is durable and never loses rows** (`logret::gzip_file`, review
  of 2026-11-23; the decoder log's too). The part is fsync'ed, moved over
  the gz (`fs_replace`), the folder fsync'ed (`fs_fsync_dir`), and only then
  the plain file removed: on ext4 with delayed allocation (the Pi's SD card)
  a power cut after an unsynced rename left an EMPTY `.gz` and no plain
  file. A day that already has a `.gz`: its rows ending with exactly the
  plain file's (a crash between the move and the unlink) -> the plain one is
  only removed; other rows (the clock was set back after a pass and the day
  written again) -> the plain day is added as one more gzip member after
  them (concatenated members are one valid gzip file: `zcat`, `zgrep` and
  gzgets read them all) - before, the older gz was replaced and its rows
  lost; a damaged gz -> nothing is changed, the plain file stays beside it.
  **Damaged** (`logret::gz_intact`, review round 3): anything but complete
  gzip members to the file's last byte - a member cut short, bytes after
  the last member (zeros after a power cut on FAT / exFAT, garbage), a file
  that is not gzip at all. zlib's own reader stops quietly at trailing
  bytes and reads a non-gzip file as it is (gzdirect), so the first version
  took such files for healthy: the appended member landed after the
  garbage, invisible to zcat, gzgets and the history, and the plain file
  was unlinked. The walk inflates member after member with zlib's
  `inflate` and requires each to end exactly where the next begins. A file
  found damaged is remembered (its size and modification time) and not
  inflated again by later passes until it changes; it is said once per run
  on stderr, and "cannot gzip <file>" once per file per run (both repeated
  every pass before). The tail check streams both files (constant memory). While both a plain day and a
  different gz exist (until the next pass), the history and the export
  read the PLAIN file only (one file per day, as for the crash state), so
  the gz's older rows are missing from the charts until the pass has added
  the day to it.
- **The pass** (`passes()`, a `logret::Scheduler`: its own thread, one at a
  time, both logs in one pass; a log line "Telemetry logs: retention pass,
  Keep N + Then compressed M days" each time):
  - **at the first use, with the STORED settings, without waiting for
    heimdall** (`ensure_started()`, from the first `host_log_tick` /
    `wb_board_ingest` / setter): the WB_LOG_* values are read from the
    settings store (`SettingsStore::apply_commands()`; the store is loaded
    in `main` before any thread that calls in here starts), then the
    sampler starts and the passes are allowed. The first version waited for
    the settings replay, which only comes once heimdall connects - without
    heimdall no pass ever ran while the host log kept growing (review,
    2026-11-23); the replay applies the same values later (no change, no
    pass). The stored on/off and period apply from the start too (before,
    rows were written with the defaults until the replay). The setters call
    it first, so a stored value never overwrites a newer one;
  - **a settings change: 5 s after the LAST one** (`request_after_change`,
    `Scheduler::DEBOUNCE`). A replay (RESET_SETTINGS, the end of a playback)
    sets Keep and then Then compressed, and the page's two selects are two
    commands (with the arrow keys one per option passed): a pass between
    them ran on half the change - stored 30 / 0 -> 7 / 365 through 7 / 0
    deleted days 8-30 that the final rule gzips (the reviewer's race
    harness: 16 of 24 runs). A timed request (startup, the day change, the
    hour) while a change waits joins it;
  - **the deletions of a change wait 10 min after the LAST change** (the
    grace, `Scheduler` `delete_grace`, review round 3): until then every
    pass only gzips (the log line says "gzip only"); 10 min after the last
    change a pass with the deletions runs by itself. The debounce only joins
    changes made within seconds - a person setting Keep 7 and, 6 s later
    (reading the tip), Then compressed 1 year got the deletions of 7 / none
    in between: days 8-30 gone (the reviewer's twosteps). Gzipping on an
    intermediate rule loses nothing. A change that should delete is seen on
    disk 10 min later (the `?` tip says so). `KRAKEN_LOGRET_GRACE_S`
    (1-3600, read at startup) shortens it for tests - not a user setting;
  - **the page's two selects are one command**: `WB_LOG_RETENTION:<keep>,<zip>`
    (control_handler; clamped like the two, saved as `wb_log_keep_days` /
    `wb_log_zip_days` - the replay uses those -, the clamped pair echoed);
    the page sends it whenever either select changes, when the receiver
    says it takes it (`retention_cmd: true` in `logging`), else the old
    command of the select that changed. `WB_LOG_KEEP` / `WB_LOG_ZIP` stay
    (older pages, the replay, scripts). **Remaining limit**: two separate
    commands more than 10 min apart (an older page, a script) do apply the
    rule in between - it was in force for 10 min. The assistant has no tool
    for these settings. The decoder log's identical pattern (its two number
    fields, `DECODER_LOG_DAYS` / `DECODER_LOG_ZIP_DAYS`) has the same
    10-min grace since the final review (2026-11-23; its startup replay is
    no change - *decoder-log.md*);
  - at the UTC day change and every hour: `retention_timer()`, its own
    thread (the sampler asked before - a hung statfs stopped retention);
    `KRAKEN_TELEMETRY_PASS_S` shortens the hour for tests;
  - **never while a debug playback runs** (the hold, `playback::active()`):
    its settings are a sandbox, the files are the user's. What was asked
    waits; when the playback ends the stored values are replayed and the
    pass runs 5 s later, once, on them.
  Before, `purge_old_logs` ran from the row append - on the uWS loop for the
  host log, at the first row after the start, with the default 30 days.
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
  alike, every member), the plain one when both exist; a day the pass
  gzipped after the listing is read from its `.gz`. Same rows, same buckets
  as with every file plain (tested below). **Only complete rows**: a line
  without its line end (the live file in the middle of a write, a gz cut
  short) and a row without every column of the header are skipped - a torn
  "...,2" charted 0.002 GB free before. Lines are read by length (gzread,
  not gzgets + strlen): a power cut leaves zeros (NUL bytes) in a plain
  file where a write was - the row written after them (same day, after the
  restart) starts after the last NUL and is kept; the torn row and the zeros
  are dropped (198 of 200 rows read before - the zeros swallowed the next
  row). The events of the range keep the
  newest 200 as a ring while reading (they were all held, then cut).
  Listing and reading never create a folder: a receiver without the board
  has no `wb_logs/` (the first write creates it). The history's `log` object adds `plain_bytes`,
  `plain_files`, `gz_bytes`, `gz_files` (`bytes` / `files` are now every
  file of the log: rows and events, plain and gz, an older build's).
  `export.csv?...&gz=1` gives the same rows gzipped (`*_last_7d.csv.gz`).
  **The export is at most 31 days** (the page offers 30; it is written as
  one temporary file before it is sent - 366 host days were 166 MB, board
  days ~320 MB); the history still takes up to 366 days.
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
  start); it still appends the row and an event line on the loop (small
  writes). vcgencmd failing (missing, a timeout, odd output) keeps the last
  value; after 3 failures in a row the flags are -1 and it is tried once
  every 10 min (a machine without it starts no child every 10 s). Measured with a fake vcgencmd that takes 500 ms first on PATH,
  65 s of the status on the TEST page's WebSocket: this build 130 messages
  at 2.000 Hz, max interval 502 ms; the old build a 1003 ms gap at every
  vcgencmd call (6 in 65 s - the loop, and with it the spectrum, audio and
  every command, stood still for 500 ms each time).
  The sampler no longer asks for the hourly / day-change passes: a statfs
  that hangs (a dead network mount) would have stopped retention with it -
  `retention_timer()`, a thread of its own, asks (review round 3).
- **The host alarms have hysteresis** (`HostAlarm`, review 2026-11-23): up at
  once; down only after the value has stayed inside the lower level's limits
  moved inward by a margin for 30 s without a break - temperature 3 C
  (warn 75 / alarm 82 C), memory 3 points (85 / 95 %), storage 10 % more MB
  and one point more (2 GB or 10 % / 500 MB or 3 %), the throttling flags
  no margin. A Pi at 75 C wrote two events a second before (the 5 MB day
  cap within hours). The board's alarms are heimdall's (its own hysteresis);
  this one is written from that description, not from heimdall's code.
- **The page** (the telemetry sheet, `wbSheetOpen`; "Logging (both logs)"):
  Keep [7 days / 30 / 90 / 1 year] and **Then compressed** [none / 30 days
  / 90 / 1 year / 3 years / 10 years] (another stored value is added to its
  list), a `?` beside them that says the rule with the numbers in force
  (`wbLogRule`: "Keep 7, Then compressed 30: the last 7 days plain, the 30
  days before those (days 8 - 37) compressed, older days deleted", and that
  a change's deletions come 10 min after the last change). Both stay usable
  while logging is off - they apply to the days on disk (an older receiver,
  no `zip_days`: disabled, as it only applied them when it wrote a row).
  Either select sends `WB_LOG_RETENTION:<keep>,<zip>` (`wbLogSetRetention`).
  The history must answer for the source asked for: an older receiver
  answers `src=host` with the board's history (no `src` in it); the sheet
  says "not supported by this receiver" instead of charting the board's
  temperature as the CPU's (`wbSheetLoad`, review round 3). The size line (`wbLogInfo`): "4.1 MB plain
  + 12.3 MB compressed in 742 files, from 2025-10-20", the plain / gz file
  counts and the rule in its tooltip. A `.csv.gz` download beside Download
  CSV for the 7 / 30 day ranges. The values ride `system_status`
  (`hw.logging` / `wb_telemetry.logging`: `keep_days`, `zip_days`).
- **Compatibility.** An older page with this receiver: Keep only (`zip_days`
  ignored); the receiver keeps gzipping with the stored Then compressed.
  This page with an older receiver (no `zip_days`): no Then compressed, no
  tip, no `.csv.gz` button, the old size line; without `retention_cmd` (the
  first follow-up version) the select that changed sends its own command. The WB_LOG_* commands now
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
- **Tested after the review (2026-11-23, scratch `w_logs/r2/`, `e2e/t_r2_e2e.py`,
  `e2e/t_page2.py`)**: `gzip_file` with an existing gz (different rows ->
  a member added, zcat reads both; equal rows / a tail -> only the unlink;
  damaged -> untouched; zero-length), its system calls under strace (fsync
  of the part -> rename -> fsync of the folder -> unlink; the previous
  version: rename -> unlink); the Scheduler (debounce, join, hold, one more
  after a pass); the real `wb_board.cpp` with a stubbed store / playback
  (the reviewer's race: 24 runs, KEEP..ZIP 0 us - 2 s apart, none deleted,
  two passes each; history / export over plain, gz, truncated gz, gz +
  garbage, zero-length gz, a torn line, a short row; the clock-back day;
  no `wb_logs/`; vcgencmd failing / flaky; the alarms flapping at 75 C /
  85 %: one event each, cleared 30 s after 71 C); the previous commit fails
  each of these. End to end: no heimdall (stored 7 / 0 applied, a page's
  change 5 s later; the reviewer's `e2e_noheimdall.sh` and `e2e_reset.sh`,
  5 / 5); RESET_SETTINGS from stored 30 / 0 for the telemetry and the
  decoder log (5 trials, days 8-30 gzipped, none deleted); a real debug
  playback of a small SigMF recording (Keep 2 / 0 for both logs during it:
  nothing touched; after the stop the stored values and one pass that
  changes nothing); SIGTERM -> exit in < 1 s; the page's two selects
  changed within a second: one pass on the final values.
- **Tested after the second review (round 3, scratch `w_logs/r3/`, `e2e/t_page3.py`,
  `t_r2_e2e.py` S5)** - each check fails on the round-2 commit (5d7b530):
  the reviewer's `t_gz` (a gz with trailing zeros / garbage + other rows:
  refused, the plain day kept - before: appended after the garbage and
  unlinked) and `t_wb2` (NUL run: 200 of 200 rows in history and export -
  before 198; twosteps through the setters 6 s apart: days 8-30 gzipped, none
  deleted - before all 23 deleted; perf: a 366-day export request writes 31
  days, 14 MB - before 166 MB); gz_intact on every damaged kind and
  gzip_file refusing each; a damaged gz opened once in three passes with one
  warning (strace) - before every pass, and the plain day lost; the grace
  (gzip-only passes, the deletions after the last change + grace, a timed
  pass joining); statvfs hanging (LD_PRELOAD): the periodic passes still
  ran and deleted - before none; WB_LOG_RETENTION end to end (one pass,
  saved as the two settings, echoed, clamped 900,-5 -> 365,0, malformed
  refused); the page against both builds: Keep 7 then 1 year 6 s later ->
  days 8-30 gzipped, none deleted (before: 46 of 46 deleted), Keep / Then
  compressed usable while logging is off (before: disabled), an older
  receiver's src=host reply not charted (before: the board's temperature
  shown as the CPU's).

