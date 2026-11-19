# Decoder data log

(2026-11-20: records of type `call` - one per finished call of a trunked
system, in the control channel's decoder folder at that VFO's Log / Log All:
*trunk-following*, "Decoder log".)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Decoder data log (`src/decoder_log.cpp`, ⚙ Setup Settings → Decoder Logging; a sidebar box until 2026-11-19):**
- **The files (2026-11-19).** The user: "I want to change it so that each
  decoder logs to its own file in its own folder, in the decoder_logs
  folder. The X days of logging will be kept in individual files in each
  folder. Secondly, add a feature with setting that asks to keep log files
  longer than X days, but in compressed format. So for example we can keep
  7 days of uncompressed logs, and up to 30 days of compressed logs. Where
  each time the expiring uncompressed log file is moved into a zip file or
  similar. Thirdly, move the "POsitions every" setting into the decoders
  itself that need it (like ads-b, AIS etc)." So:
  - **A folder per decoder**: `<dir>/<name>/<name>-YYYY-MM-DD.jsonl`, `<name>`
    = the record's `dec` (the plugin id: `adsb/adsb-2026-11-19.jsonl`,
    `ais/`, `pagers/` ...). Per decoder, not per VFO: two VFOs running ADS-B
    write one file, told apart by each line's `vfo` / `mhz` (the record
    format did not change; `dec` stays in every line, files get copied
    around). **Incidents** go to the file of the decoder whose page it was
    (their `dec`). **Transcripts** (type voice; `dec` = a plugin id or NBFM /
    AM / WBFM) all go to ONE folder, `transcripts/transcripts-YYYY-MM-DD.jsonl`:
    a transcript belongs to Voice to Text, and a user looks for them in one
    place. The clips stay where they were (`voice_clips/YYYY-MM-DD/`; the
    lines point at them).
  - **The folder name is made safe by the writer** whatever the id
    (`declog::safe_folder_name`, `include/decoder_log_names.hpp` - pure,
    header-only, so a harness tests it): only `[a-z0-9_-]` (A-Z lowered,
    anything else `_`), at most 32 characters, `other` when no letter or
    digit is left (`""`, `..`, `/`), and a `dec-` prefix on the names Windows
    reserves (con, prn, aux, nul, com1-9, lpt1-9) and on our own folders
    (`transcripts`, `voice_clips`) so a plugin of that name can't mix with
    them. No path can leave `<dir>`. Folders are created with
    `diskutil::mkdirs` (the compat layer's call: `create_directories` on
    Windows).
  - **The old combined files** (`<dir>/decoders-YYYY-MM-DD.jsonl[.gz]`,
    written before this) stay where they are, are not split, stay
    searchable, and age by the same rule (below). Today's, if the program is
    updated mid-day, is no longer appended to and is treated like a finished
    day (not counted in Today / Per day).
  - **Several files are open at once**: a buffer per file (a map folder ->
    text, `State::bufs`); every `FLUSH_S` one append + fflush per file with
    records, nothing written per record; the 32 MB cap and the dropped
    counter are for all buffers together (a dropped flush now counts its
    lines); the free-space floor as before (checked once per flush). A file
    without a record for 10 min is closed at the next flush tick (an idle
    decoder holds no handle; `KRAKEN_DECLOG_IDLE_S` = 1-3600 s, read at
    startup, shortens it for tests - not a user setting). The day change
    writes every buffer into its file of the finished day and closes them
    all. The status' `open_files` counts the open ones.
  - **One function knows the layout**: `declog::day_files(dir, date)` - the
    plain and gz files of one day in every decoder folder, `transcripts/`
    and the old combined file (folder `""`). The assistant's search uses it;
    no other file builds log paths. Only folders whose name
    `safe_folder_name` would give back (plus `transcripts`; never
    `voice_clips`) and only names `<folder>-YYYY-MM-DD.jsonl`, `.jsonl.gz`,
    `.jsonl.gz.part` (and `decoders-...` in `<dir>`) are ever listed - so
    nothing else in the folder is ever touched.
- **Plain for N days, then compressed for M more (2026-11-19).** Two
  settings: **Keep** (`DECODER_LOG_DAYS`, N, default 7, 0-3650) and **Then
  compressed** (`DECODER_LOG_ZIP_DAYS`, M, new, default 30, 0-3650). The
  rule, by the age of a file's date (today 0, yesterday 1;
  `declog::retention_action`, pure):
  - a finished day stays **plain** while its age <= N (this replaced the
    gzip at midnight: yesterday is plain now), is **gzipped** in its folder
    (`<name>-YYYY-MM-DD.jsonl.gz`: one gzip per day and decoder - the
    existing zlib code, `zcat` / `zgrep` read it, a damaged file costs one
    day; no archive library, no growing archive rewritten) when N < age <=
    N + M, and **deleted** when age > N + M (a plain one is deleted, not
    compressed first);
  - the user's example "7 days of uncompressed logs, and up to 30 days of
    compressed logs" = **N 7, M 30**: plain for days 1-7, compressed for
    days 8-37, deleted after day 37 - the 30 is counted after the 7, not
    including them (the page's `?` tip says it with these numbers);
  - **M = 0**: deleted when it leaves the plain window (the old rule);
  - **N = 0** keeps its old meaning, keep forever: nothing is ever
    compressed or deleted (and Then compressed is shown disabled). M has no
    "forever" (not offered: 3650 days is the most; an empty field would have
    needed a third state in the command and the status);
  - a day the old version already gzipped (younger than N) **stays gz** -
    never decompressed; gz files are kept until N + M, then deleted;
  - a `.gz.part` (a gzip a crash interrupted: the plain file is still
    there) is removed and the plain file compressed again; today's files
    (age <= 0, also a clock set back) are never touched;
  - the old combined files follow the same ages.
  The **pass** (`run_pass`, a detached thread, one at a time - a request
  during a pass runs another after it, `request_pass`): deletions first,
  then the compressions one file at a time (`.gz.part` -> `fs_replace` ->
  unlink; a crash leaves the plain file, retried at the next pass). It runs
  at startup, at the day change, on Keep / Then compressed / folder
  changes and every 10 min - **but not before the settings replay**
  (`declog::settings_restored()`, called at the end of
  `ControlHandler::apply_persisted_settings`, which runs once heimdall
  connects): the stored Keep days must be known first. (Before, retention
  ran a second after startup with the default 7 days, so a stored Keep 0 or
  30 could lose days while heimdall was not yet connected.)
- **Voice clips** keep following `keep_days()` = N, the plain window only,
  as before: they are Opus already (gzip gains nothing) and they are the
  bulky part; Then compressed does not apply to them (the "Keep the audio"
  tooltip and the Keep `?` tip say so). N = 0 keeps them forever, as before.
- **The status' size rows** sum over everything: every folder, plain and
  gz, the old combined files (`files`, `total_bytes`); `today_bytes` = today's
  files in the decoder folders (not today's old combined file - no longer
  written). New: `zip_days`, `gz_ratio` (uncompressed / compressed size of
  the .gz files there, from each file's ISIZE trailer - cached by path and
  size; files whose ISIZE is smaller than the file, tiny or wrapped at 4
  GiB, are not counted; 0 = none), `gz_measured` (how many were),
  `steady_bytes` (the receiver's own steady-state estimate), `pos_per_vfo`
  (true: Positions every is per decoder), `layout` ("folders"),
  `open_files`. **Disk lasts** (`dlDiskLasts`): what the log occupies at
  steady state at today's rate = (today + N) plain days + M days / ratio
  (the measured ratio, else ~6x, said in the tooltip), compared with the
  free space plus what the log holds now (its own files are reused):
  "fits (7 + 30 days ≈ 1.1 GB)", else the days until full (the plain days
  first, then the compressed ones); N = 0: free / per day (plain forever).
  The label's tooltip says it is an estimate from today's rate. An older
  receiver (no `zip_days`): the old formula and wording.
- **Positions every is the decoder's (2026-11-19).** A per-VFO decoder
  option **`pos_s`** (1-3600 s, default 10) beside `log`: `dig::Options::pos_s`,
  `DIGITAL_OPT:<vfo>:pos_s:N` (anything else refused: "pos_s must be 1-3600
  (seconds)"), the option string's new key `ps=` (an older kraken_doa
  ignores it), `digital_opts.pos_s` and the decoder status' `opts.pos_s`,
  `DigitalDecoder::pos_interval_s()` (an atomic copy, like `log_mode()`).
  The record handler's "one position per object every N s" uses the
  option of the VFO the record came from (keyed vfo|plugin|id as before).
  It affects only the log: the map gets every point.
  - The page shows it in the decoder's head row right after the Logging
    select, **only for decoders that report positions**: the plugin's
    static capability `kp::Info::map` ("map" in the plugin list; the same
    capability that offers "Plot on map" - ADS-B, AIS, APRS, UAT, FLARM,
    radiosonde, VDL2, DMR / D-STAR GPS, LoRa, 406 MHz), not "a point was
    seen" (`digReportsPositions(tab, st)`; the status' `map_capable` only
    when the plugin isn't in the list). "Positions every [10] s", the
    meaning in its tooltip; greyed and disabled (the value still shown)
    while the VFO's Logging is Off or logging is not allowed - the tooltip
    says which.
  - It left Setup Settings (the field under Files).
  - **Migration**: a VFO stored without `ps` gets the stored global
    `DECODER_LOG_POS_S` (`options_from_string(.., &no_pos_key)`, the
    DECIMATORS replay - DECODER_LOG_POS_S is replayed before it), so nothing
    changes for an updated receiver; new VFOs / decoders get 10.
  - **Compatibility**: `DECODER_LOG_POS_S:N` (an older page) is still
    accepted and persisted: it sets the global (what a VFO without a key
    uses) and, from a page, every VFO decoder's `pos_s`. This page with an
    older receiver (no `pos_per_vfo` in the declog status, no `pos_s` in the
    options): no field in the rows, the global field in Setup Settings
    (`#dl-pos-wrap`), no Then compressed (`#dl-zip-wrap`, shown only with
    `zip_days`). The assistant's `set_decoder_option` takes `pos_s`.
- **What is written (2026-11-19).** The user: "Change it so that Log all is
  replaced with a drop down that has "Logging 'Off / Log / Log All'".
  Logging can then be enabled per decoder, and then we can remove the
  "what to log" settings in the cog menu. Log and Log All will be greyed
  out if "allow logging to disk" is turned off. Add a logging drop down to
  transcriptions too." - and, told that the repeated messages would then
  only show while Log All logs them: "Yes add a seperate show all messages
  checkbox too". So:
  - **"Allow logging to disk"** (`DECODER_LOG:0|1`) is a permission:
    nothing is written while it is off.
  - **Each VFO's decoder has a Logging mode** (`dig::Options::log`,
    `DIGITAL_OPT:<vfo>:log:0|1|2` - also the words off / log / all -, in the
    snapshot's option string as `l=`, in `digital_opts.log` and the decoder
    status' `opts.log`). It belongs to the VFO, whichever decoder it runs
    (Auto detect too). A new VFO / decoder starts at **Off**.
  - **The rule per record** (the record handler in `start()`, which asks
    `DigitalDecoder::log_mode()` - an atomic copy of `Options::log` - per
    record; `record_incident`'s `log_mode`): written iff logging is allowed
    and the source VFO's mode allows the type -
    - event, message, position: Log or Log All. Under Log that is what the
      decoder reports, so the repeated control-channel messages too while
      its **Show all messages** tick (`verbose`) is on;
    - raw: Log All only (the plugin only builds them then: OPTION
      `log_raw=1` is sent only while Log All is in effect, and W_RAW is
      dropped otherwise - `Engine::log_all()` in digital_decoder.cpp);
    - incident (incidents.cpp, written when the address lookup finishes):
      the Logging of the VFO whose decoder sent the message, at that time;
    - voice (voice2text.cpp): the transcripts' own Logging (below).
  - **Log All also turns the plugin's `verbose` on**: `kp::Host::verbose()`
    = the VFO's Show all messages OR (Log All AND logging allowed). The host
    cannot tell a "verbose-only" report from another (the wire EVENT is a
    de-duplication time + text), so Log All writing the repeated messages
    means they are on screen too while it is in effect - the page shows
    the Show all messages tick ticked and disabled then. Making the two
    independent would need a plugin-API change (not done).
  - **Transcripts**: one switch for all, the Transcription panel's
    **Logging: Off / Log** (no Log All: transcription has no repeated
    class). It is the `voice` member of `DECODER_LOG_TYPES` - the persisted
    setting it always was, so a stored choice is kept. The page sends the
    list it has with `voice` added / removed; the receiver reports it as
    `voice` in the status (`voice_logged()`: allowed and voice; the
    voice2text status' `log.enabled`). The audio clips keep their own
    "Keep the audio".
  - **`DECODER_LOG_TYPES`** is still accepted with any subset of the six
    names (an older page in an open tab sends it from its What to log
    ticks) and stored as sent; only `voice` decides anything. A list stored
    by an older version can therefore not switch categories off unseen.
  - **Positions every N s** (`DECODER_LOG_POS_S`) is a rate, not a choice:
    a field of its own in the section's Files block - per decoder since
    2026-11-19 (above).
- **Migration** (`options_from_string(s, &no_log_key)`, control_handler's
  DECIMATORS replay): a VFO whose stored options have no `l` (written by an
  older version) gets **Log**, its `v` (the old "Log all", now Show all
  messages) kept - exactly what the old version wrote, while the master
  switch stays as stored. Exception: if the stored `DECODER_LOG_TYPES` had
  `raw` (`declog::legacy_raw()`; that setting is replayed before
  DECIMATORS), those VFOs get **Log All**, so raw frames keep being
  written - which also turns their repeated messages on (Log All's
  verbose). The options are applied only to VFOs whose decoder is on (as
  before), so a VFO without a decoder starts at Off when one is picked. The
  transcripts: the stored `voice` member as it was. The snapshot keeps
  `v=` for an older kraken_doa (a downgrade reads its "Log all" tick).
- **Compatibility**: an older page with this receiver - its What to log
  ticks send `DECODER_LOG_TYPES` (only voice counts), its "Log all" tick
  `DIGITAL_OPT:..:verbose` (Show all messages, unchanged); it never sets a
  Logging, so VFOs it creates stay Off. This page with an older receiver
  (no `log` in `digital_opts`, no `voice` in the declog status): each
  decoder row shows only the Show all messages tick (still `verbose`, the
  old receiver's "Log all"), no Logging select; the section keeps the old
  What to log block (`#dl-types-sec`: the six ticks, Positions without its
  seconds - the field is the separate Positions every) and says
  "Per-decoder logging needs the updated program" (`#dl-old`, the reason
  in its tooltip); the Transcription panel's select still works (the voice
  member of the old types list), Voice to Text's "turn on" switches both
  on as before.
- Sources: the engine's record handler (`dig::set_record_handler`,
  `LogRecord{type, plugin, text, from, point, rf_hz}`, called on each
  decoder's worker thread): "event" (a Report event that passed the
  de-duplication), "message" (`kp::Host::message`), "position" (a map
  point - logged at most once per vfo|plugin|id every `pos_s`, default
  10 s; regardless of "Plot on map"), "raw" (`kp::Host::raw`, wire RAW 29);
  plus "incident" from incidents.cpp (`declog::record_incident`, new
  incidents only) and "voice" from voice2text.cpp (`declog::record_voice`,
  a transcript, `t` = when the transmission BEGAN - `head(.., t_ms)`; see
  *Voice to Text*). The VFO id comes from `DigitalDecoder::vfo()` (set by
  DecimatorManager when it creates the decoder - ids never change). NOT via
  `decimator_manager` from the handler: it runs on the decoder's worker
  thread, and holding the last reference to the VFO instance there would
  destroy the decoder on its own thread (self-join)
- Raw frames cost CPU, so plugins only build them while asked:
  `dig::set_log_allowed(enabled)` + the VFO's Log All -> its Runners send
  OPTION `log_raw=0|1` (handled in plugin_host.cpp, not passed to the
  decoder; `set_raw_wanted` and the "Raw frames" tick until 2026-11-19) ->
  `kp::Host::raw_wanted()`. ADS-B: Mode S hex + level dB; APRS: TNC2;
  pagers: "POCSAG RIC n Ff baud" + message codewords' 20 data bits, "FLEX
  <speed> <cycle.frame> <phase> capcode n V<type>" + vector + message words
- File: `<dir>/<decoder>/<decoder>-YYYY-MM-DD.jsonl` (LOCAL date;
  `<dir>/decoders-YYYY-MM-DD.jsonl` before 2026-11-19), one JSON object per
  line: `t` (local ISO 8601 with ms + UTC offset), `vfo`, `mhz` (VFO RF; not
  on incidents), `dec` (plugin id), `type`, then per type: text / from+text
  / id, label, kind, lat, lon, alt_m, kmh, hdg (finite only) + `info` (the
  map popup's "Key: value" lines as an object - ADS-B: squawk, vertical
  rate, IAS / TAS, category, emergency, signal) / data / address, lat, lon,
  precision, confidence, text / voice: text, src (= dec: plugin id or NBFM /
  AM / WBFM), dur, conf, from + to (talker id + label), doa / brg, lang (not
  "en"), clip (`voice_clips/YYYY-MM-DD/<id>.opus`, relative to the log folder)
- SD card: records go to a buffer per file (cap 32 MB for all, then counted
  as dropped); the worker (1 s tick) appends each every `FLUSH_S` (5 s) with
  one fwrite + fflush, keeping the files open (closed after 10 min without
  a record). Below `MIN_FREE_MB` (100) free the buffers are kept and an
  error shown instead of writing
- Midnight (local date change seen by the worker): the rest of the buffers
  goes to the finished day's files (records stamped up to the tick), they
  are closed, then the retention pass (above: plain for Keep, gzip for Then
  compressed, then deleted; zlib level 6 -> `.gz.part` -> rename, then
  unlink). Until 2026-11-19 every finished day was gzipped at once and days
  older than Keep deleted. Only our names are ever touched
- Commands (control_handler.cpp; persisted, schema after GEO_RADIUS_KM -
  DIR first, DECODER_LOG last): `DECODER_LOG:0|1`,
  `DECODER_LOG_TYPES:event,message,position,raw,incident,voice` (any
  subset, kept as sent; the default has all but raw; only `voice` - the
  transcripts' Logging - counts since 2026-11-19, `raw` only for the
  migration),
  `DECODER_LOG_DAYS:0-3650`, `DECODER_LOG_ZIP_DAYS:0-3650` (default 30;
  after DAYS in the schema), `DECODER_LOG_POS_S:1-3600` (the old global,
  above),
  `DECODER_LOG_DIR:path` (created if missing, write-tested; refused ->
  `{"declog_error":"..."}` and the old folder stays; the same folder again
  is a no-op, so the replay creates nothing while logging is off),
  `GET_DECODER_LOG`. Status `{"declog":{enabled, types, voice (the
  transcripts' Logging; its presence tells a page that the VFO decoders
  have their own Logging), days, zip_days, pos_s, pos_per_vfo,
  flush_s, layout, dir, dir_abs, free, total, today, today_bytes, per_day,
  steady_bytes, gz_ratio, gz_measured, files,
  total_bytes, buffered, dropped, compressing, open_files, error,
  mounts:[{path, dev, type, free, total}], mount}}` every 2 s (websocket_server, only with
  subscribers) and after each command. `mounts` = /, /media/*, /mnt/*,
  /run/media/*, /srv* (no pseudo / tmpfs) + always the folder's own mount
  (`mount`); statvfs of the folder or its nearest existing parent
- UI (kraken_doa.html, the Decoder Logging section of ⚙ Setup Settings,
  `.set-pane[data-t="declog"]`, `setDlgOpen('declog')`; `dlOnStatus` etc.).
  It was a sidebar box (`#declog-section`, `data-sb` "decoder-logging")
  until 2026-11-19, when the user asked for it to move into Setup Settings
  ("everything relating to setting up the software", *sidebar.md*); the
  element ids stayed, so the code that drives them did not change:
  - a card on top: the master switch, renamed from "Log to disk" to
    **"Allow logging to disk"** (the user's wording, 2026-11-19), with its
    state beside it (`#dl-state`: Off / Logging / Not writing - the box's
    Status row) and the box's `?`; under it the problems as warning lines
    (`#dl-warn`; `#dl-old` for an older receiver), then the status
    (Logging - what is logging now, `dlRenderWhat`: "D0, D2 · transcripts"
    with each VFO's decoder and mode in its tooltip, "nothing", "(when
    allowed)" and dimmed while not allowed; redrawn on every declog status
    and decimator info - Folder, Disk free + used bar, Today,
    Per day = today's bytes over the time logged today, All logs, Disk
    lasts assuming ~6x gzip; Per day and Disk lasts dimmed while off) and
    the buffered / gzipping note (`#dl-note`). The card's look follows the
    state: grey dot while off, ✓ logging, ⚠ amber with a problem, red while
    not writing;
  - What to log: the type ticks in two columns (one below 420 px of
    window), Positions with its seconds field inline - removed 2026-11-19
    (what is written is chosen per decoder and for the transcripts, above);
    the block stays in the page (`#dl-types-sec`, hidden) for an older
    receiver only;
  - Files: Keep [7] days and Then compressed [30] days side by side
    (`.dl-keep`; the rule with the user's example in Keep's `?` tip, each
    field's meaning in its title; Then compressed disabled while Keep is 0;
    hidden for an older receiver), Save to - the drive list (picking one fills the
    folder field with `<mount>/kraken_decoder_logs`, `/` = the default
    `decoder_logs`; Apply sends it, `declog_error` shows under the field).
    Positions every N s was here (`#dl-pos-wrap`) until 2026-11-19 - now
    shown only for an older receiver.
  Every control sends its command at once, as in the box.
- Problems (`dlProblems(d)`, only while logging is on): not writing (the
  receiver's `error`: the folder can't be created / written, less than
  100 MB free), records dropped (`dropped`), the disk nearly full (under
  1 GB free - the bar's red). The box showed them at a glance in the
  sidebar; since the move they are noticed from outside the section: a ⚠
  on its row in the dialog's list (`#set-tab-declog-n`, titled with the
  problems), a warning line in every decoder tab under its VFO rows
  (`digLogWarnHtml(ids)`: "Decoder log: not writing · 3 records dropped",
  the receiver's reason in its tooltip, + a Decoder Logging button that
  opens the section; one line per tab - the log is one for all decoders -
  and only while one of the tab's VFOs is set to Log / Log All),
  the Transcription panel's head while the transcripts' Logging is Log, and
  Voice to Text's "To disk" row (a link to the section; "off · turn on" -
  sets the transcripts to Log - or "off · not allowed" - to the section).
  The error itself also shows in the card while logging is off, as the
  box's Status row did. `dlOnStatus` redraws the decoder tab, the panel head
  and the Voice to Text section only when the problems, the permission, the
  transcripts' switch or the receiver's kind change (not on every
  free-space figure)
- The Logging controls on the page: a decoder's in its tab's VFO row
  (`digLogCtlHtml`, *digital-decoders.md*); the transcripts' in the
  Transcription panel's row of controls after the VFO filter (`#v2t-lsel`,
  `v2tRenderLogSel`, `v2tSetLog`). Both greyed while logging to disk is not
  allowed: the Log options disabled, the select showing Off (the stored
  choice untouched - it is back once logging is allowed), its tooltip naming
  the stored choice and where to allow logging, and a small icon button
  beside it to the section (`setDlgOpen('declog')` - a disabled option
  can't be clicked)
- Test (scratchpad h/t_declog.py + e2e3): a TEST build (ports 1808x) fed
  synthetic POCSAG, started with `TZ` set so local midnight came 2 min later:
  5 s write steps, rotation + gzip at midnight with the boundary records on
  the right side, retention (7-day-old kept, 8+ deleted, foreign files kept),
  startup gzip of an unfinished old day, folder change / refusal, type and
  on/off changes, no writes while off
- Test of the per-decoder Logging (2026-11-19, scratch `test_logmode.py`,
  105 checks, + a C++ harness of the option string, 30): the TEST build
  behind a fake heimdall that streams the debug playback's cu8 file
  `adsb-six-aircraft` as the 8091 stream in real time, looped (the debug
  playback itself pauses the decoder log, so it can't show writes), two
  VFOs running ADS-B, a log folder of its own; real mouse / key events on
  the selects. Per phase the records written in a 15 s window are read
  back from the file: Log -> events + positions, no raw, no repeated
  messages (ADS-B's "DF17 ..." lines, verbose only); Log + Show all
  messages -> those repeated messages too, no raw; Log All -> positions,
  repeated messages, raw frames; Off -> nothing although the decoder
  decodes (and shows everything with the tick on); not allowed -> nothing
  at all, and Log All not in effect (no repeated messages on screen with
  the tick off). The verbose the plugin got is read from the screen (its
  repeated messages in the VFO's event log) for tick off / on x Log / Log
  All, Off with the tick, and Log All not allowed. Also the page's states
  (greyed options, Off shown, the stored mode in the tooltip and back when
  allowed, the tick ticked + disabled under Log All), the Transcription
  select and the receiver's `voice_logged()` (the voice2text status'
  `log.enabled`; no transcript was written - Voice to Text isn't installed
  on the test receiver), an old page's commands, this page against an old
  receiver (simulated statuses), the assistant's `set_decoder_option` over
  the MCP endpoint (log words / numbers, a bad value refused, `verbose`
  unchanged, Read only refused), and the migration: the receiver restarted
  on settings files in the old version's form - D0 with "Log all" ticked
  and D1 without -> Log (verbose kept / off), a VFO without a decoder ->
  Off, a stored list without `position` still writing positions, a list
  with `raw` -> Log All with raw frames, the stored master switch off ->
  nothing written. Incidents were checked by reading only (no pager signal
  with a geocodable address on the test receiver)
- Test of the files, retention and Positions every (2026-11-19, scratch
  `test_logfiles.py` + two C++ harnesses). Harness 1 (`names_test.cpp`,
  64): `safe_folder_name` on plugin ids, odd ids (`..`, slashes, `/abs`,
  Windows reserved names, `transcripts`, `voice_clips`, empty, non-ASCII,
  every 1-2 byte id), the file-name parser, `retention_action` for every
  age 0-60 at (7, 30), (7, 0), (0, any), (1, 1), young gz, parts, today,
  and the option string's `ps` (missing / bad / round trip). Harness 2
  (`declog_harness.cpp`, 29): the real decoder_log.cpp with the decoder
  and playback calls stubbed - transcripts from NBFM and dmr in
  `transcripts/` only, an incident in its plugin's folder, the odd ids'
  folders and nothing outside the log folder, idle files closed (fds read
  from /proc), a playback / not allowed / Logging Off writing nothing,
  `day_files`, and no pass before `settings_restored()`. End to end, the
  TEST build behind the fake heimdall streaming the ADS-B test transmission
  (then the AIS one, converted to 2.4 MS/s cu8): ADS-B on two VFOs -> one
  file `adsb/adsb-<today>.jsonl` with both VFOs, nothing combined; AIS on a
  third -> `ais/ais-<today>.jsonl`; D0 at 1 s and D1 at 10 s -> per aircraft
  positions >= 0.99 s / >= 9.9 s apart and D0 >= 3x D1; the file open while
  logging and closed 20 s after the last record (`KRAKEN_DECLOG_IDLE_S=20`);
  nothing written during a debug playback; retention on fabricated day
  files (decoder folders, old combined files plain and gz, a young gz, a
  `.gz.part` beside its plain file, an orphan part, foreign files) for
  (7, 30), (7, 0), (0, 30), (0, 0), (1, 1) and M / N changed at once - the
  exact plain / gz / gone set, compressed content equal to the original
  (`zcat`), foreign files untouched; the status' files / total / today /
  gz ratio for that set; Disk lasts for known numbers; the assistant's
  search (decoder folders plain and gz, transcripts, old combined plain and
  gz, the decoder filter); the page's field (only for decoders with
  positions - Pagers has none -, greyed states, real typing, clamping),
  the Files block, the old receiver, the old command, the migration of a
  stored global (25) and a stored `ps` winning over it.

