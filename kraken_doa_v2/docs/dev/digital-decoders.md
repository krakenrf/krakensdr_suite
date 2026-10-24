# Digital voice / data decoders (engine, protocol receivers, decoder tabs)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital voice/data decoders (one per VFO): engine `src/digital/`, decoders `plugins/`:**
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below);
  kraken_doa contains no protocol code. Shipped: p25, dmr, tetra, dstar,
  nxdn, mpt1327, pagers (see *Pagers*; was pocsag), aprs, adsb (see *ADS-B* below), ais (see *AIS*),
  radiosonde (see *Radiosonde*), dme and ils (see *DME / TACAN* and *ILS /
  VOR / marker*), ism (see *ISM devices*), radar (see *Pulse radar*),
  ssr1030 (see *1030 MHz interrogations*), uat (see *UAT*), flarm (see
  *FLARM / OGN*), analog_id (see *Analog FM IDs*; was ctcss), beacon406 (see *406 MHz
  distress beacons*), lora (see *LoRa / Meshtastic / LoRaWAN*), wildlife (see
  *Wildlife collars*), rds (see *RDS / RBDS + SCA*), dtv and cells
  (ARRAY plugins - see *Digital TV* and *LTE / 5G NR Cell Decoder*)
- Which plugins run in Auto detect is the USER's choice: the "Auto detect"
  tick per plugin in the sidebar's plugin list (Digital Decoders box), all on by
  default. WS `PLUGIN_AUTO:id:0|1` -> `PluginRegistry::set_auto()` (bumps the
  registry generation, so decoders in AUTO rebuild their plugin set) and
  records the setting `AUTO_DETECT_OFF:id,id` (settings_store schema, before
  DECIMATORS; replayed through `set_auto_off()`); the plugin list's `auto`
  field is that state. `kp::Info::auto_detect` is deprecated and ignored
- `DigitalDecoder` (digital_decoder.hpp) hangs off a `DecimatorInstance`
  (`digital`, created on first use, `digital_mode` mirrors the mode). The
  decimation pass pushes the VFO's decimated samples (the beamformer output
  when beamforming ran for it, else the listened-to channel - the same stream
  FM audio gets) with `push()`; a worker thread ("digital-dec") per decoder
  drives the plugin processes, so the pipeline never waits. Queue capped at
  1 s (oldest dropped, `dropped` in the status). In FM/digital-only mode (DoA
  and beamforming off) only the VFOs heard (🔊) and the VFOs with a decoder are
  decimated, on one channel (`fm_only` in data_receiver.cpp)
- Modes (`dig::Mode`): OFF, AUTO, PLUGIN (+ plugin id). Wire / snapshot form
  `OFF`, `AUTO`, `PLUGIN:<id>`; `parse_mode_string()` maps the old protocol
  names (P25, DMR, TETRA, DSTAR, D-STAR, NXDN, MPT1327) to their plugins and
  renamed plugins to their new id (`renamed_plugin()`: `sfn` -> `dtv`,
  `ctcss` -> `analog_id`, `pocsag` -> `pagers`; the snapshot's `sfn.*`,
  `ctcss.*`, `pocsag.*` options too), so saved settings and old clients keep
  working
- `Engine` (digital_decoder.cpp): AFC mixer at the VFO rate -> one msresamp
  per distinct plugin `sample_rate` (shared by the plugins at that rate; a
  plugin declaring 0 gets the VFO's rate unresampled - `Runner::vfo_rate`,
  started `--serve --rate HZ`, restarted when the VFO's rate changes) ->
  each plugin process (`Runner`). AUTO runs one Runner per built plugin the
  user left in Auto detect (rebuilt when the registry generation changes)
  minus `manual_only` plugins (`kp::Info::manual_only`, ADS-B - the plugin
  list shows no Auto detect tick for them, `digAutoPlugins()` leaves them
  out) and those whose `min_vfo_rate` is more than 2x the VFO's rate
  (`auto_ids()`, `AUTO_MAX_RATE_SHORTFALL`: ADS-B's 2.4 MHz upsampled from a
  12.5 kHz VFO only burned CPU) - so AUTO starts its plugins with the first
  block (rate known) and rebuilds when the VFO's rate changes the set;
  detection = majority of `valid()` frames in a 4 s window. Only the "lead"
  plugin (the fixed one, or the detected one) gets VOICE_WANTED and has its
  audio / voice state / carrier offset used. AFC: plugin offset reports are
  averaged and applied at most every 300 ms, ignoring reports from the 300 ms
  after a correction (the pipe latency made an every-report integrator run
  away - TETRA drifted -5 kHz). With no correction (yet) the mixer is a
  plain copy (a sin/cos per sample cost ~5% of a core at 2.4 MHz) and its
  phase goes back to 0; idle, the correction decays to the VFO centre and
  lands on 0 below 0.5 Hz (until 2026-10-16 the phase of the last
  correction was kept for good: once a decoder that reports an offset -
  D-STAR, P25, ... - had run on a VFO, a later multichannel plugin there -
  lora, ism - got one antenna until kraken_doa restarted: a debug playback
  of lora-mesh-five after dstar-two-radios had no separation by code). A VFO
  retune (> 100 Hz) or mode change resets everything (RESET to the plugins,
  facts + map points cleared; `reset_pending_` is consumed by the worker)
- Station location for the plugins: `dig::set_station_location()` (called
  by `build_map_message` every second from `station_info.resolve()`; 0,0 =
  unknown) bumps a generation; each Runner sends wire STATION "lat,lon" (""
  = unknown) when it changes and after a (re)start -> `kp::Host::station()`
- Map points: wire MAP_POINT (id, lat, lon, label, kind, heading, altitude_m,
  speed_kmh, ttl_s, info - '\0'-separated text, "" = unknown; validated in
  `on_map_point`, ttl clamped 10 s..24 h) / MAP_REMOVE -> `Report::map_set /
  map_remove` (key plugin|id, max 2000 per decoder, expired by ttl).
  NaN marks unknown fields: test with `is_finite_value()`
  (utils/parse_num.hpp) - kraken_doa built with -Ofast until 2026-10-23,
  which folded `std::isfinite` to true (a `"h":nan` broke the page's
  JSON.parse; digi_test (-O2) didn't show that); its Makefile adds
  -fno-finite-math-only now, and is_finite_value is right under any flags
- The protocol receivers live in the plugins (`plugins/<id>/<id>.cpp`), on
  the shared library `plugins/lib/` (libkrakendig.a): `dig_common` (the
  RxContext / Report / Options interface they were written against, mapped
  onto `kp::Host` by `dig::Bridge`; `FmFrontEnd` = 48 kHz Kaiser 6.5 kHz +
  discriminator in Hz; `RrcFrontEnd`), `dig_fec` (block codes by nearest
  codeword, RS GF(64) Berlekamp-Massey, soft Viterbi, CRCs), `dig_fsk4`
  (P25/DMR 4FSK sync receiver -> `Fsk4Sink`), `dig_vocoder` (IMBE, mbelib
  AMBE, TETRA ACELP), `mbe_tables.hpp`. Each receiver is sync-driven -
  nothing is reported unless its own FEC/CRC passed:
  - `Fsk4Receiver` (lib/dig_fsk4.cpp): P25 on an integrate-and-dump filtered
    discriminator, DMR on RRC 0.2; normalized sync correlation per sample,
    the sync's own +-3 symbols give timing, level and centre (= carrier
    offset), no symbol-timing loop. DMR voice bursts B..F (no sync) are
    decoded at positions extrapolated from the last sync. DMR data sync =
    voice sync with every symbol negated (`^ 0xAAAA...`, NOT `~`). The p25
    plugin enables only P25, the dmr plugin only DMR. A DMR VOICE sync off
    the 30 ms grid of a CONFIRMED burst sequence (`dmr_confirmed_`: its
    last burst checked out - EMB / slot type + FEC, or a voice sync on the
    grid) is ignored: random voice payload correlates >= 0.8 with a sync
    word now and then (~1 in 50 bursts of random AMBE data), and burst A is
    checked by nothing, so a false one moved the extrapolation of B..F off
    their timing; a sequence a bare voice sync started is not confirmed
    (any sync re-anchors it, as before - a real MS recording has a false BS
    voice sync 10 ms after a radio's real one) (2026-10-16, found with the
    dmr generator)
  - `P25Proto` (plugins/p25): NID BCH(63,16) by table search, TSBK (rate 1/2
    trellis + CRC), LDU1/LDU2 (Hamming(10,6) + RS(24,12)/(24,16)), HDU
    (Golay(18,6) + RS(36,20)), TDULC (Golay(24,12) + RS(24,12)), PDU header.
    IDEN_UP tables turn channel numbers into MHz. Option `nac`
  - `DmrProto` (plugins/dmr): CACH TACT (timeslot), slot type Golay(20,8)
    (max 2 corrections - a third of random words are within 3), BPTC(196,96)
    voice LC header / terminator (RS(12,9)), CSBK, PI and data headers, EMB
    QR(16,7) + embedded LC, GPS and talker alias LCs. Control-channel chatter
    (Aloha, broadcasts, acks) only with the verbose option. Option `slot`.
    The kind + slot that extrapolated bursts are read with (`ext_bs_`,
    `ext_slot_`) come only from a burst that checked out (`set_ext`): a
    false direct-mode data sync inside a repeater's burst made the next
    burst B..F skip its CACH, on the wrong slot (2026-10-16). Real
    recordings: dmr_1 263 -> 269 valid bursts, dmr_it_8 604 -> 605, dmr_2
    identical, talkers unchanged
  - `DstarReceiver` (plugins/dstar): RF header (scrambler, 24-column
    interleaver, K=3 Viterbi, CRC-16/X.25), slow data (text message, GPS /
    DPRS, header copy), end pattern
  - `NxdnReceiver` (plugins/nxdn): NXDN96 (4800 Bd, 10 sps) and NXDN48
    (2400 Bd, 20 sps) searched at once, each on its own RRC 0.2 filter;
    10-symbol FSW (0.88 threshold), whole-frame descrambler (negates
    symbols), LICH (8 bits on the first bit of 8 symbols, second bits 1,
    even parity of bits 7..4), SACCH (5x12 interleave, puncture every 6th
    from 5, K=5 G 0x19/0x17, CRC-6 0x27) and FACCH1 (9x16, every 4th from 1,
    CRC-12 0x80F). A SACCH counts only when it follows the previous good one
    exactly one frame later (CRC-6 alone passes 1 random word in 64);
    FACCH1 counts alone. Layer 3: VCALL (unit/TG, cipher), TX_REL, DCALL,
    trunking CAC (site / service info, grants). Voice: 4 (or 2) AMBE+2
    frames per frame -> mbelib (nW..nZ)
  - `Mpt1327Receiver` (plugins/mpt1327): MPT1327 / MPT1343 analogue
    trunking signalling, 1200 bit/s FFSK (1 = 1200 Hz, 0 = 1800 Hz) on the
    FM discriminator. Two sliding one-bit (40-sample) tone correlators; each
    tone is normalized by its own running level (radio pre-emphasis makes
    1800 Hz louder - without it the sync never correlated). Sync = 4
    preamble bits + SYNC 0xC4D7 (control) / SYNT 0x3B28 (traffic), hard
    match <= 2 bit errors. The SYNC is the CHECK FIELD of the control
    channel system codeword (CCSC = 0, SYS 15 bits, CCS, 0xAAAA), so the
    CCSC starts 44 bits before the pattern; the address codeword (message)
    follows it. Codewords: 48 bits + BCH(63,48) g 0xE815 with the last check
    bit inverted + even parity; single-bit errors corrected. Messages: GTC
    grants (type < 256), Aloha variants, ACK*, AHOY*, status, CLEAR, MOVE,
    BCAST (SYSDEF), data headers; idents shown prefix-ident. Validated on
    a live NZ control channel (SYS 0x42E1, ~96 % of slots); GTC field
    positions follow sdrtrunk and have not been seen live yet
  - `TetraReceiver` (plugins/tetra, 72 kHz RRC 0.35 input): differential
    detection, 4th-power frequency estimate, slot sync from the training
    sequences (both spectral orientations tried), BSCH (MCC/MNC/colour code
    -> scrambling code), BNCH SYSINFO, AACH (Reed-Muller), SCH/F and SCH/HD
    MAC-RESOURCE with the MM/CMCE PDU type of clear signalling
- Report (`dig_report.hpp`): facts table per plugin id (key, value, age),
  event log (400 entries, identical texts de-duplicated per window; events
  carry `src` = plugin id and `p` = its name, "" = the decoder itself).
  `MessageBuilders::build_digital_message()` pushes `{"digital":[...]}` on
  TOPIC_CTL at 4 Hz (only while a decoder is on), with only the events since
  the previous push; `DIGITAL_HISTORY:id` replies with the whole log,
  `DIGITAL_CLEAR:id` empties it. Status: `mode`, `detected` (plugin id),
  `detected_name`, `frames` {id: [4 s, total]}, `plugins` [{id, name, state
  running|starting|missing|not built|crashed|error, error, dropped, log}],
  `info` {id: facts}, `voice`, `opts`, `map_capable` (a running plugin
  declares `kp::Info::map`), `map_points`, `tables` (below; only in some
  pushes)
- Tables (`kp::Host::table_columns / table_row / table_remove / table_keep`,
  wire 25-27, 32; `table_detail`, wire TABLE_DETAIL 38): one per plugin in
  `Report` (headings + rows by key, max 1000, a row not updated for the
  table's keep time - 10 min, or what the plugin set with `table_keep`, 60 s
  .. 7 days - left out; `table_clear()` empties the rows on a retune / mode
  change, headings stay; a row's details - label / value pairs, <= 200 -
  live in its `TableRow::detail`, kept across row updates, dropped with the
  row, a detail for a missing row ignored). `status_json(.., tables)`:
  `"tables":{"<id>":{"cols":[..],"rows":[[key, age_s, cells..]],
  "det":{"<key>":[[label, value]..]}}}` -
  `build_digital_message` adds it at most once a second per VFO (+ every
  history reply); the page keeps the last tables in between
  (`digOnMessage`, same mode). UI `digTablesHtml(tab)` merges the plugin's
  table of every VFO in the tab with leading "VFO" / "MHz" columns (row key
  `vfo|plugin|key` = its map point) + the tab's incidents -> `#info-dec-table`
  above the facts / log (`#info-body-dec.has-table`; the panel grows to
  420 px the first time): header click sorts (numeric when both cells are
  numbers, empty cells last; per plugin in localStorage
  `kraken_dig_table_sort`), "Seen" column from the row age, a row whose
  `vfo|plugin|key` is a map point is clickable (`digTableRowClick`: opens
  the map, selects + centres the marker). A row with details gets a ▸
  toggle (`.dig-det-tog`, `digDetToggle` - stopPropagation, so the row's
  own click isn't fired; open rows in `digDetOpen`, this page only): the
  details render as a row of their own under it (`tr.dig-det`, a
  label / value table in `.dig-det-box` - sticky left, `100cqw` of the
  `.dig-table-wrap` container, so a table wider than the panel doesn't
  push them out of view)
- Options (`dig::Options`): verbose, invert (host side: conj before the
  mixer), map ("Plot on map", default OFF - the user ticks it in the
  decoder tab; persisted as `&m=1`; `DIGITAL_OPT:id:map:0|1` also asks for
  a full map push) + `plugin`
  {"<id>.<key>": value} - the plugins' declared options,
  sent to the plugin as OPTION key=value ("" = back to its default).
  `DIGITAL_OPT:id:verbose|invert|<id>.<key>:value` (old keys dmr_slot /
  p25_nac map to dmr.slot / p25.nac). Persisted as the snapshot field
  `v=1&i=0&dmr.slot=1` (values percent-encoded; the old `v/slot/nac/inv` form
  still loads). The panel shows a plugin's options while it is selected, or
  in AUTO once AUTO detected it. An option whose two choices are exactly
  "0" / "1" is an on / off button (`digVfoRowHtml`, `.dig-opt-tog`: "● Label:
  On" highlighted / "○ Label: Off", the choice's text without a trailing
  "(...)", the help + "click to turn on / off" as its tooltip; it sends
  DIGITAL_OPT with the other value and shows the echoed state) - dtv's Path
  separation, adsb's Array combining / Overlap separation (its Bit repair
  is a three-way list since v2.0), dme's DoA per beacon, cells' Cell
  information; other choice lists stay `<select>`s. The settings rows
  (`#info-dec-top`) are rebuilt only while no input / list in them has the
  focus or a mode list is open (`ddOpenSel`); a focused BUTTON doesn't hold
  the rebuild back (a clicked button keeps the focus - the on / off button
  showed its new state only after a click elsewhere) and gets the focus
  back afterwards (the same button by position). Test: scratchpad
  e2e/t_dtv_toggle.py clicks with real mouse events (`.click()` doesn't
  focus, so the first test missed it)
- UI (kraken_doa.html, "Digital decoders" block): any number of VFOs decode
  at once. Each VFO card (Decimators box, `updateDecimatorList`) has a
  "Digital decoder" tick (`digToggle`: DIGITAL_MODE AUTO or the mode picked
  while off, remembered per VFO in localStorage) and a mode list
  (`digSetModeFor`: picking a decoder also switches it on) plus a state line
  (`digRenderCards`, also run at the end of updateDecimatorList, which
  rebuilds the cards). The mode lists (VFO card + the tab's Mode) are
  two-line dropdowns: a native `<select>` draws options on one line, and
  the then "SFN Separation (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB, DAB)" ran past the
  card. The `<select>` stays as the hidden model (value, options from
  `digModeOptions`, its onchange) and `ddEnhance(sel)` puts a button
  (`.dd-btn`) after it + a pop-up list (`.dd-pop`, in body, zoomed like the
  sidebar when the button is in it); an option "Name (details)"
  (`ddSplit`) shows the name with the details on a smaller second line, in
  the button and the list. The plugins are listed in GROUPS (the user's
  request 2026-10-23; `digGroups`, also the saved-frequency editor's
  decoder list): one `<optgroup>` - a heading in the pop-up - per
  `kp::Info::category` (`--info` / plugin list `category`, case ignored),
  the shipped groups in a fixed order (Two-way radio, Aviation, Marine &
  distress, Paging & packet, ISM & IoT, Telemetry & tracking, Broadcast &
  cellular), other categories after them by name, none = Other last, each
  group by name; only plugins without one (built before categories) = the
  old single "Decoders" group. Code that sets `sel.value` calls `ddSync(sel)`
  (`digFillSelect` and the tab rebuild do); `digFillSelect` leaves the value
  alone while that list is open (`ddOpenSel`). Keyboard: arrows / Home /
  End / Enter / Escape / Tab; a press elsewhere, a scroll or a resize closes
  it. Tab labels and state lines use `digShort(name)` (without the
  "(details)"; the tab's tooltip has the whole name). Test (scratchpad
  e2e/t_dd_ui.py, headless Chromium, the real plugin list). The bottom panel has one tab per decoder MODE
  (plugin), not per VFO: `infoTab` = `dig:<plugin>` (the AI tab is gone -
  the assistant has its own dock)
  (`infoPanelOpen('dec:<vfo>')` still works: the tab that VFO is in,
  `digTabOfVfo`). A VFO is in tab P while it runs P fixed or Auto detect
  locked onto P (`digVfoPlugin`, `digTabVfos`). There is NO Auto detect tab:
  a VFO in AUTO appears in the tab of the decoder it detected (its mode list
  there says Auto detect), and in none while it is still searching (its VFO
  card shows "Searching…"). The tabs are ALWAYS all there (`digTabList`):
  every built plugin (+ one this receiver lacks that still has events /
  incidents here) - those a VFO runs first, then the rest, each group by name - selectable with no VFO running it and no
  data yet; such an idle tab says so and offers "Start it on: D0 D1 ..."
  (`digSetModeFor`). The panel follows a VFO whose mode is picked
  (`digSetModeFor` / `digToggle` -> `infoPanelOpen('dec:<id>')`). A tab
  (`infoRenderDecoder`) shows: a settings row per VFO in it
  (`digVfoRowHtml`: coloured VFO + MHz + state, mode, Listen - only for
  voice decoders, `kp::Info::voice` (p25 dmr dstar nxdn tetra; `digHasVoice`:
  in AUTO the detected plugin, else any ticked one) -, the plugin's options -
  in AUTO those of the detected plugin -, verbose, invert, Plot on map,
  bandwidth / plugin problems), the incident row (messages plugins), the
  tables (below), per VFO a status + facts block (`digFactsHtml(id, tab)`:
  only that plugin's facts / frames), the first
  plugin stderr, and ONE event log merged from every VFO (`digTabEvents`:
  events whose `src` is the plugin - from VFOs in it, VFOs since switched
  to another mode, and removed VFOs (`digArchive`, this page only, marked
  "D3†") - plus the decoder's own lines (no src) of the VFOs in the tab since
  their last "Decoder set to / on"; each line tagged with its VFO in the
  VFO's colour). Clear (`digClearTab`) hides the tab's lines up to now
  (`kraken_dig_cleared`) - the decoders keep their logs (another mode's tab
  still needs them) The sidebar "🔐 Digital
  Decoders" box shows the plugin list (`aiRenderPlugins` -> `#ai-plugins`:
  Auto detect tick, ↻ = `PLUGINS_RESCAN` after a manual copy + make; no
  export / import / rebuild in the web UI - plugins move as folders + `make`;
  a plugin is put on a VFO with the VFO card's decoder list; collapsed
  by default - `digTogglePlugins`, open state in localStorage
  `kraken_dig_plugins_open`; the header shows the count and a ⚠ count of
  plugins with an error / stale / unbuilt) and the
  voice codecs (`digOnCodecs`): IMBE / AMBE
  (mbelib) / ACELP (ETSI TETRA) installed or not, with the README's install
  steps for a missing one. Source: `plugins/lib/build/codecs` (lib/tools/
  codecs.cpp, built by plugins/Makefile) - the SAME MbeLib / TetraCodec
  detection the plugins use, run by kraken_doa with its environment
  (KRAKEN_MBELIB, KRAKEN_TETRA_CODEC_DIR, PATH) via
  `dig::codec_status_message()` on a worker thread, on AI_STATE (page
  connect) and `PLUGIN_CODECS` ("↻ Check again") -> `{"codecs":{checked,
  imbe_ok, imbe, ambe_ok, ambe, acelp_ok, acelp}}`. Running plugins dlopen
  mbelib once, so a newly installed codec needs the decoder switched off and
  on
  `DIGITAL_HISTORY:id` is asked once per VFO / mode. Demod "Digital" keeps the
  VFO's bandwidth (widened only below the decoder's need - it used to force
  NBFM's 16 kHz, too narrow for TETRA / AUTO)
- Bandwidth: each plugin's `min_vfo_rate` (12 kHz for the 4FSK ones, 24 kHz
  for TETRA; AUTO = the largest of the ticked set) - the panel warns and
  offers the narrowest wide-enough option
- CPU (Pi 5, 24 kHz VFO): one plugin ~3% of a core, AUTO ~11% with the six
  protocol plugins and ~15% with all nine shipped/user plugins ticked (each
  process has its own front end; was ~4.5% in-process) - unticking unused
  decoders saves it
- Offline test harness: `tools/digi_test.cpp` (runs the plugins like a VFO,
  synchronously: waits for slow plugins instead of dropping, SYNC-drains at
  the end; .dis files are FM-modulated again; `--opt dmr.slot=1`; run from
  kraken_doa_v2): `g++ -std=c++20 -O2 -Iinclude -isystem
  liquid-dsp/build-kraken/include tools/digi_test.cpp src/digital/*.cpp
  src/utils/process_posix.cpp src/utils/fs_compat.cpp src/ai_sandbox.cpp
  src/geo_address.cpp liquid-dsp/build-kraken/libliquid.a` (liquid built by `make` first - see
  *liquid-dsp*; the plugin processes are kproc's, `utils/process.hpp`; the
  Windows command line is in the file's header)
- Validated on live P25 (VFO on a trunked control channel + its voice
  channel) and DMR Tier III here, and on recordings for TETRA, D-STAR, DMR,
  NXDN, MPT1327 and P25 (regression of 33 checks incl. voice passes through
  the plugins)

