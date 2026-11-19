# Decoder data log

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Decoder data log (`src/decoder_log.cpp`, ⚙ Setup Settings → Decoder Logging; a sidebar box until 2026-11-19):**
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
    a field of its own in the section's Files block.
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
- File: `<dir>/decoders-YYYY-MM-DD.jsonl` (LOCAL date), one JSON object per
  line: `t` (local ISO 8601 with ms + UTC offset), `vfo`, `mhz` (VFO RF; not
  on incidents), `dec` (plugin id), `type`, then per type: text / from+text
  / id, label, kind, lat, lon, alt_m, kmh, hdg (finite only) + `info` (the
  map popup's "Key: value" lines as an object - ADS-B: squawk, vertical
  rate, IAS / TAS, category, emergency, signal) / data / address, lat, lon,
  precision, confidence, text / voice: text, src (= dec: plugin id or NBFM /
  AM / WBFM), dur, conf, from + to (talker id + label), doa / brg, lang (not
  "en"), clip (`voice_clips/YYYY-MM-DD/<id>.opus`, relative to the log folder)
- SD card: records go to a string buffer (cap 32 MB, then counted as
  dropped); the worker (1 s tick) appends it every `FLUSH_S` (5 s) with one
  fwrite + fflush, keeping the file open. Below `MIN_FREE_MB` (100) free the
  buffer is kept and an error shown instead of writing
- Midnight (local date change seen by the worker): the rest of the buffer
  goes to the finished day's file (records stamped up to the tick), it is
  closed, then `compress_old()` gzips every finished uncompressed day on a
  detached thread (zlib level 6 -> `.gz.part` -> rename, then unlink; also at
  startup and on a folder change - a crash across midnight leaves one) and
  `retention()` deletes days older than `days` (by the file name's date;
  also every 10 min, on DECODER_LOG_DAYS and on a folder change). Only names
  matching `decoders-YYYY-MM-DD.jsonl[.gz]` are ever touched
- Commands (control_handler.cpp; persisted, schema after GEO_RADIUS_KM -
  DIR first, DECODER_LOG last): `DECODER_LOG:0|1`,
  `DECODER_LOG_TYPES:event,message,position,raw,incident,voice` (any
  subset, kept as sent; the default has all but raw; only `voice` - the
  transcripts' Logging - counts since 2026-11-19, `raw` only for the
  migration),
  `DECODER_LOG_DAYS:0-3650`, `DECODER_LOG_POS_S:1-3600`,
  `DECODER_LOG_DIR:path` (created if missing, write-tested; refused ->
  `{"declog_error":"..."}` and the old folder stays; the same folder again
  is a no-op, so the replay creates nothing while logging is off),
  `GET_DECODER_LOG`. Status `{"declog":{enabled, types, voice (the
  transcripts' Logging; its presence tells a page that the VFO decoders
  have their own Logging), days, pos_s,
  flush_s, dir, dir_abs, free, total, today, today_bytes, per_day, files,
  total_bytes, buffered, dropped, compressing, error, mounts:[{path, dev,
  type, free, total}], mount}}` every 2 s (websocket_server, only with
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
  - Files: Positions every N s (the old Positions tick's seconds), Keep
    (days), Save to - the drive list (picking one fills the
    folder field with `<mount>/kraken_decoder_logs`, `/` = the default
    `decoder_logs`; Apply sends it, `declog_error` shows under the field).
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

