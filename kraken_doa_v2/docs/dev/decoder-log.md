# Decoder data log

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Decoder data log (`src/decoder_log.cpp`, sidebar "🗂 Decoder Logging"):**
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
  `dig::set_raw_wanted(enabled && raw ticked)` -> each Runner sends OPTION
  `log_raw=0|1` (handled in plugin_host.cpp, not passed to the decoder) ->
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
  subset; the default has all but raw - voice since Voice to Text, so only
  new installs log it without ticking it),
  `DECODER_LOG_DAYS:0-3650`, `DECODER_LOG_POS_S:1-3600`,
  `DECODER_LOG_DIR:path` (created if missing, write-tested; refused ->
  `{"declog_error":"..."}` and the old folder stays; the same folder again
  is a no-op, so the replay creates nothing while logging is off),
  `GET_DECODER_LOG`. Status `{"declog":{enabled, types, days, pos_s,
  flush_s, dir, dir_abs, free, total, today, today_bytes, per_day, files,
  total_bytes, buffered, dropped, compressing, error, mounts:[{path, dev,
  type, free, total}], mount}}` every 2 s (websocket_server, only with
  subscribers) and after each command. `mounts` = /, /media/*, /mnt/*,
  /run/media/*, /srv* (no pseudo / tmpfs) + always the folder's own mount
  (`mount`); statvfs of the folder or its nearest existing parent
- UI (kraken_doa.html, "Decoder Logging" block, `dlOnStatus` etc.): enable,
  type ticks + positions interval, Keep days, drive list (picking one fills
  the folder field with `<mount>/kraken_decoder_logs`, `/` = the default
  `decoder_logs`; Apply sends it), status (free / total + used bar, today,
  est. per day = today's bytes over the time logged today, all logs,
  "disk lasts" assuming ~6x gzip), buffered / dropped / gzipping note
- Test (scratchpad h/t_declog.py + e2e3): a TEST build (ports 1808x) fed
  synthetic POCSAG, started with `TZ` set so local midnight came 2 min later:
  5 s write steps, rotation + gzip at midnight with the boundary records on
  the right side, retention (7-day-old kept, 8+ deleted, foreign files kept),
  startup gzip of an unfinished old day, folder change / refusal, type and
  on/off changes, no writes while off

