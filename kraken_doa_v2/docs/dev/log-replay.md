# Log replay

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**⏯ Log replay (🗺 Map, UI only, every mode; kraken_doa.html "⏯ LOG REPLAY"):**
- The user's request (2026-10-20): load and simulate KrakenSDR DoA log files
  of the Android app (krakensdr_rdf_android - its newer SQLite and its older
  CSV), plot them on the map, scrub the timeline, replay at various speeds.
  The receiver's own 📝 DoA Recording (doa_logger.cpp) writes the SAME
  formats, so it replays too (its box: 🗺 Replay = `replaySelected()`,
  fetched from `recordings/<name>` like the download). Opened IN THE
  BROWSER (map button "Replay" -> the strip's 📂, a file / file pair dropped
  on `#map-pane` - `lrDropInit` -, or that button): nothing goes to the
  receiver, no command, no route, no library (sql.js etc. would need a CDN
  or a vendored file + route: the page parses everything itself)
- Formats (the Android code: `data/log/FastLogParser.kt`, `LogEntry.kt`,
  `data/log/SqliteLogFormat.kt`, `HeatMap.kt`): CSV without a header, ", "
  separated (the app's reserved fields "R,R,R,R" without spaces; the
  server's with), the layout by FIELD COUNT - 6 (ts, lat, lon, gps heading,
  compass heading, bearing), 8 (+ confidence, power), exactly 373 (2021: ts,
  lat, lon, gps, compass, bearing, conf, power, freq Hz, antenna, latency,
  heading mode, 360 spectrum), >= 377 (the DOA_value.html line: ts, bearing,
  conf, power, freq Hz, antenna, latency, station id, lat, lon, gps heading,
  compass heading, heading mode, 4 reserved, 360 spectrum - the app's
  distance-gated rows carry N<count>,X<m>,E<range> there); a line of >= 753
  fields = several records (a DOA_value.html body whose newlines were lost:
  split before each "<12-14 digit timestamp>," not after a comma). SQLite:
  `entries` (ts_ms, decimator_id - the server's VFO, the app's NULL -,
  bearing_deg, confidence, power_db, freq_hz, antenna, station_id, lat, lon,
  gps_heading, compass_heading, heading_sensor, spectrum BLOB u8 x
  spectrum_len scaled spec_min_db .. spec_max_db; the app adds latency +
  reserved1-4 - ALTER TABLE on a server file, so old rows are short) + `meta`
  (format_version 1, app krakensdr_rdf_android | kraken_doa_v2). Conventions:
  epoch ms; the spectrum 360 x 1 deg COUNTER-CLOCKWISE from the array's
  front, dB above its minimum (clamped -100); the bearing CLOCKWISE from the
  heading (360 - argmax); compass bearing = heading + bearing, i.e. compass
  B = spectrum index heading - B; the heading = the compass field in
  COMPASS mode, else the GPS one (KRAKEN / MANUAL write it into both).
  Confidence / power scales differ per server generation (only displayed)
- A log RE-SAVED BY A SPREADSHEET (the user's OneDrive "KrakenSDR Android
  Logs": 040723-106700-hunt1.csv + its copy, 030823-gsm.csv) is read too
  (the user's request 2026-10-20, `lrDecodeText` / `lrParseCsv` /
  `lrRetime`): Excel's General format writes the 13-digit timestamps as
  1.68843E+12 - 6 significant digits = rounded to 10^7 ms (2.8 h), every row
  of a drive at one or two "moments" (speed / turn rate came out NaN: no
  heat map); numbers lose the space after the separator, text keeps it
  (" UCA"); rows padded to the longest with empty fields (a 2021-layout row
  then had the newest layout's count - trailing empty fields are dropped
  before counting: 372 / 373 = 2021, >= 377 newest); European locales ';'
  + decimal commas; "Unicode text" = UTF-16 (BOM) + tabs; quoted fields.
  The rounding step comes from the digits kept (`tsStep`; also found when
  every timestamp is a multiple of >= 10^5 ms and repeats within a signal -
  re-saved as plain numbers). Times are REBUILT in file order from the track:
  the clock advances one GPS fix per change of position (the row rate varies
  20/s .. 1/2.4 s between logs, the fix rate much less: 1.2-2.4 s per fix in
  the 20 intact logs, median 1.6 = `LR_FIX_MS`); a group of rows at one
  position longer than the run's median (a stop; several VFOs per fix) takes
  its share; the pace is fitted where the rounded value steps twice (> 2.8 h
  logs: a 10 h synthetic drive to 0 s), else assumed; placed next to an
  intact row (the app appended after the save - only when the run still
  fits its rounded values: rows appended days later aren't glued on), else
  on a step of the rounded value (it passes the step's middle there), else
  centred in its rounding step (time of day +-1.4 h). The strip says so
  (`.lr-warn`, "≈" on the clock) and "The drive took [N] min" re-times it
  (worker op 'retime' on the kept text - `L.text`, only for re-timed CSVs;
  the page keeps the signal, the place on the timeline and the map view).
  The heat map hardly depends on the pace (its bearings close by distance;
  speed only gates at 2 m/s and widens the bearing sigma). Checked
  (scratch t_excel.mjs on 20 intact logs x 4 damaged forms - Excel CSV,
  ';' + decimal comma, UTF-16 tabs, half damaged): the same rows, positions,
  bearings; estimates 0-391 m from the originals' (median ~5 m), bearings
  as many, rebuilt duration 0.66-1.41x the real (median 1.04); 77 / 80 - the
  misses are kiwi-433-110823 (51 rows, one per 4.6 s: time 3x too short,
  more rows over the 8 deg/s turn limit, 7 vs 16 bearings, its estimate
  still 103 m from the original's). t_retime.mjs 14 / 14,
  tools/replay_validate.mjs has two spreadsheet layouts
- The worker (`lrWorkerMain`, started from a Blob of its source like the
  viewshed's; `self.lrTest` exposes it to node):
  - its own READ-ONLY SQLITE READER (`sqlOpen`: header, table b-trees +
    overflow chains, index b-trees for WITHOUT ROWID, records incl. int48 /
    int64, UTF-16 databases, the INTEGER PRIMARY KEY = rowid,
    `sqlParseCreate` for the column names). A `-wal` file's COMMITTED frames
    (salts + chained checksums, up to the last commit frame) are laid over
    the pages: the server's recordings are WAL mode (checkpointed every 30 s
    + at stop), the app copies its file in DELETE mode. A -wal that breaks
    the read is dropped (read again without it, `walBad` shown); the page
    only pairs `x.db-wal` with `x.db` (a WAL names no database - SQLite
    itself can't tell either)
  - `lrParse`: rows sorted by time (stable); per row the heading by mode,
    compass bearing (logged bearing, else 360 - the spectrum's argmax),
    speed from the TRACK of every row (+-3 s; sparse distance-gated rows:
    neighbours within 30 s), turn rate from the signal's neighbouring rows
    (<= 3 s), "dup" = the same spectrum as the signal's previous row (the
    same DoA frame logged again - the app's poll / the server's held body);
    signals = frequency (+ VFO); the spectrum kept ONLY as the display lobe
    (linear 10^(dB/10) min..max -> u8, the live map's scale) + its linear
    floor `lmin` (the heat map rebuilds the linear spectrum from the two;
    a Float32 spectrum per row cost 1.4 KB). Arrays go to the page by
    structured clone (the worker keeps its copy for the heat jobs)
  - THE MOBILE DF ENGINE PORTED (`RdfGate` / `RdfGrid` / `RdfSolver`,
    `rdfNorthLobe`; src/rdf_engine.cpp line by line - KEEP THE TWO ALIKE,
    `tools/replay_validate.mjs --cpp` compares them) and fed like
    rdf_mapper.cpp feeds it: GPS heading rows only at >= 2 m/s and <= 8
    deg/s turning (compass / KrakenSDR / manual heading: <= 30 deg/s), sigma
    hypot(2, GPS 1.5 + 8 / speed | 4), the gate's range from the current
    estimate (mode mass > 0.5); bearing-only rows (6 / 8 fields) get a
    Gaussian lobe of hypot(5, heading sigma). `estimate()` leaves out the
    offset posterior (`offset(e)`: only for snapshots - it was ~40 % of a
    record's time); estimates / marginals cached per grid version.
    Measured: ~10-16 ms per record in Chrome / node (0.7-2.6 s for the
    sample drives, 20-26 s for a 2 h 2-VFO log of 1404 records)
  - a heat job (`lrHeat`, op 'heat' {job, sig, rangeKm}) runs in 80 ms
    slices (a newer job / cancel / load gets in between): per record its
    estimate (`ests`, keyed by the time the window CLOSED - when it was
    known), and SNAPSHOTS of the coarse + fine rasters (12 nats, row 0
    north, `RDF_LUT` colours) every max(1 s, duration / 400) - <= ~400 x 80
    KB; rasters transferred. Progress `pct` + `why` (used / slow / turning /
    dup / nopos / nohdg / nolobe)
- Page (globals block: `lrUi` - localStorage `kraken_lr` {speed, follow,
  track, lines (s: 0 off, 30, 120, 600, -1 all), lobe, heat, power, range
  km}, rebuilt field by field -, `lr` the log, `lrOpen`, `lrReady`;
  mapDrawNow -> `lrDraw` (after v2tLobeDraw: heat snapshot <= t, track -
  driven solid amber, to come dashed on a dark underlay, broken at pauses -,
  power dots, bearing lines of the window, the estimate's 95 % ellipse +
  "Replay TX ±r95", the moment's lobe around the vehicle + its bearing to
  the map's edge) and `lrDrawTop` (the vehicle arrow, over the markers)).
  Timeline: pauses > `LR_GAP` 60 s take `LR_GAPC` 4 s (`lrAxisOf` /
  `lrTimeOf`; the Android sample redoubt16sept.csv has a stray 2026 row
  after a 2022 drive - 3.9 years -> 12 min); playback advances the axis by
  real time x speed (0.5-500x), redraws <= ~30 fps, pauses when the map is
  hidden; Follow keeps the vehicle in the middle of the map above the strip
  every frame (`lrFollow`; it used to re-centre in one jump when the vehicle
  neared an edge - the user's 2026-10-21 request); a drag of the map (the
  view moved > 3 px from where Follow put it at the same zoom,
  `lr.followAt`) turns Follow off, a wheel zoom keeps it. The heat
  snapshots carry their window (c / half / rot) and are drawn by
  `rdfGridDraw` like the live map's. A row is shown up to `LR_HOLD` 30 s; the vehicle is
  interpolated between rows <= 10 s apart. Strip `#map-lr` (bottom, like
  the path-loss strip; `.map-pane.lr-open` + `--lr-h` = its height lift the
  DF / marker panels, the status line, the path-loss strip and cap the
  top-left stack): name / format / signal list, transport (⏮ ⏴ ▶ ⏵, speed,
  date-time, position / length), layer buttons, ±range, the timeline
  canvas (`lrTlDraw`: per pixel column the strongest row - height + colour
  = power over the log's 5-95 % range -, white dots = the bearings that went
  into the heat map, dashed = a shortened pause, green bar = the heat job's
  progress, playhead; base layer cached), the moment's row + "Heat map: N of
  M bearings · TX ±r · distance / bearing from the vehicle", offset when
  measured. Keys on the strip: Space / k, ← →, Home / End. A click on the
  track (`lrHit`, 8 px; after markers / decoder points / masts / AI layers)
  jumps there; ⤢ (fit all) includes the drive; the status line's "No
  positions yet" hint is left out while the strip is open
- Tests (job scratch ae566749 lr/; node = Pinokio's v24, MSYS2 g++ for the
  C++): the SQLite reader vs Python's sqlite3 on 512 B / 4 / 64 KB pages,
  overflow BLOBs, WITHOUT ROWID, ALTER TABLE, UTF-16, an uncheckpointed WAL
  (12 / 12); every Android sample CSV (krakensdr_rdf_android root +
  travelsimulator/) parsed - three drives of the same "redoubt" transmitter
  (2021 400 MHz, 2021 "home", 2022 416.588 MHz) agree within ~150 m (r95
  98-187 m); SQLite made from them in both writers' schemas (+ the server's
  WAL tail) = the CSV's rows, bearings and estimate to 0.0 m; the engine
  port vs the C++ (cmp_engine / tools/rdf_cmp.cpp) on three real drives:
  rasters identical (<= 1 step), offsets to 4 decimals, estimates <= 1.3 m
  (float32 vs double); a synthetic drive around a known transmitter in
  every layout + COMPASS mode with a garbage GPS field: 4-37 m off
  (`tools/replay_validate.mjs`, committed); headless Chrome (t_ui.mjs, 35
  checks, real mouse / keys): the Replay button, file load, fit, the end /
  mid-drive estimate, play / pause at 50x, timeline drag, ←, Space, a click
  on the track, the layer buttons + kraken_lr, range, a path-loss strip
  standing on it, ⤢, a multi-VFO receiver log (signal list, "no heat map:
  standing still"), a dropped SQLite + -wal pair, a -wal alone, a non-log
  file, the DoA Recording box's 🗺 Replay, phone width, ✕, reload, damaged
  localStorage, no page exceptions; t_perf.mjs: a 2 h 2-VFO 149 MB CSV
  (57,600 rows) loads in 3.3 s, heat in ~26 s, a frame <= ~10 ms with all
  lines + power dots, 60 MB JS heap

