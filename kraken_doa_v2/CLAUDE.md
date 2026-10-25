# kraken_doa_v2/CLAUDE.md

kraken_doa: the client that takes heimdall's coherent IQ (8091, control on
8092) and does everything users see - spectrum and waterfall, VFOs with
audio, MUSIC DoA and beamforming, decoder plugins, the map, scanners,
recording, Voice to Text, the AI assistant - behind one HTTPS / WebSocket
page on 8080. The suite-wide rules in `../CLAUDE.md` apply here too; this
file adds what kraken_doa work needs.

Every feature's design, protocol, edge cases and test history is in
`docs/dev/<topic>.md` (not loaded automatically; index at the end). Read the
topic before changing a feature - most hold decisions the user made and
bugs that were already fixed once. In those files an *italic* name is
another topic.

## Build

```bash
make                           # kraken_doa + liquid-dsp (pinned, static) + every plugin; fetches pinned uWebSockets
make run | status | debug | debug-arm | whisper | clean | rebuild | distclean | deps
make -C plugins PLUGIN=<id>    # one plugin (a plugin that fails to compile only warns in the full build)
```

- At most `-j3` on a Pi. Stale header dependencies after big header
  changes: `make distclean && make`.
- Flags `-O3 -ffast-math -fno-finite-math-only -march=native`; `obj/.flags`
  rebuilds everything when they change. On macOS `mac/fast_complex.h` is
  force-included into kraken_doa and the plugins (`std::complex` `*` / `*=`
  / `norm` without libc++'s inf / NaN recovery; `../docs/dev/macos.md`).
- `kraken_doa.html` (and `array_calculator.html`) are read per request:
  edit, then hard-reload the browser. New HTTP routes need a rebuild.

## Where things are

- `src/main.cpp` (startup, threads), `include/config.hpp` (compile-time
  constants, including the ports), `include/globals.hpp` (global state,
  `uca_angle_sign()`), `include/types.hpp`.
- `src/networking/`: `tcp_client.cpp` (8091 / 8092), `data_receiver.cpp`
  (ingest; per packet one `SharedDecimator::decimateVfos` pass, then each
  VFO's squelch / MUSIC / talker DoA), `websocket_server.cpp` (the uWS loop,
  routes, timers), `web_mapper.cpp`, `gpsd_client.cpp`.
- `src/signal_processing/`: `fft_processor`, `shared_decimator`,
  `music_processor`, `beamformer`, `audio_mixer`, `fm_demodulator`,
  `noise_floor`.
- `src/control_handler.cpp` (every WebSocket command),
  `src/message_builders.cpp` (outgoing JSON / binary),
  `src/settings_store.cpp` (what is persisted to `doa_settings.json`).
- Features: `src/digital/` (decoder engine, plugin processes),
  `saved_freqs`, `sf_scanner` / `sf_scan_plan`, `continuous_scanner`,
  `scanner_manager`, `recorder*` / `sigmf` / `playback`, `voice2text`,
  `decoder_log`, `incidents` / `geo_*`, `rdf_engine` / `rdf_mapper`,
  `talker_doa`, `array_cal`, `station_info`, `ai_*`.
- `plugins/<id>/` decoders, `plugins/lib/` (libkrakendig), `plugins/sdk/`
  (plugin API `kraken_plugin.hpp` + `plugin_host.cpp`); the plugin guide is
  `plugins/SDK.md`.
- `kraken_doa.html`: the whole UI (~24,500 lines). The `// Globals` block
  sits near the top; sections are marked by comment titles ("📡 STATION ON
  THE MAP", "👁 VIEWSHED", ...), which the topic files cite.
- `tools/`: offline benches and validators (`decim_bench.cpp`,
  `digi_test.cpp`, `replay_validate.mjs`, `pathloss_validate.mjs`,
  `rdf_sim.cpp`, `fast_log_test.cpp`).

## Threads and the command path

- Every control command runs on ONE thread, the uWS loop:
  `ControlHandler::handle_websocket_message()` / `apply_command()` (browser
  messages, the settings replay, web-mapper cloud settings, the AI
  assistant's MCP tools, the scanners). From any other thread post it with
  `loop->defer` - never call the dispatcher directly.
- Per packet the data receiver runs one decimation pass for every VFO (a
  task per antenna on the shared decimation pool, cores + 1 threads), then
  each VFO's pipeline (VFO 0 inline, the rest on a 3-thread pool). ~29-32
  threads is normal.
- Decoder plugins are child processes, one per VFO decoder, started through
  `kproc` (`include/utils/process.hpp`), which keeps sockets and files from
  leaking into children. Spawn every child that way.

## Adding a WebSocket command

1. Handle it in `control_handler.cpp`: plain text `PREFIX:value`. Throw
   `CommandRejected` for an invalid value (then it is neither saved nor
   echoed); call `set_applied()` when you clamp or normalise, so the applied
   value is what gets saved and synced.
2. To persist it, add the prefix to the schema in `settings_store.cpp`. The
   schema's order is the startup replay order (e.g. `MIXER_SIDE:` before
   `FREQ:`, VFO-dependent settings after `DECIMATORS:`).
3. To replay it to newly connected browsers, add it to
   `is_replayed_command()`. Commands that answer with their own broadcast go
   in `is_query_command()` - that also skips the generic save, so such a
   handler saves with `SettingsStore::record` itself if it should persist.
4. Build any reply in `message_builders.cpp` and update the page to match.
   Try it from the browser console: `ws.send("PREFIX:value")`.

## Rules

The page (`kraken_doa.html`)
- It must work without internet: no CDN, no map library (uPlot is inlined).
  Tiles, terrain, Overpass and Nominatim are fetched by the browser, never
  by kraken_doa; honour their usage policies (Nominatim: on Enter only,
  <= 1 request a second).
- Escape all third-party text (OSM tags, search results, decoder text,
  plugin output) before it goes into HTML.
- State that drawing code uses (`mapDrawNow`, `drawBox`, `rpApply`, ...)
  goes in the globals block: that code can run before a later section's
  `const`s exist.
- The sidebar is scaled with CSS `zoom`: anything inside it positioned with
  `position:fixed` from `getBoundingClientRect()` must divide by the zoom.
- Shared state (saved frequencies, scan ticks, settings) lives in the
  receiver: the page sends a command and redraws from the broadcast every
  page gets - no optimistic updates.
- Keep the protocol compatible both ways: an older page in an open tab and
  an older receiver must keep working. Add fields and message keys rather
  than changing old ones (e.g. `bandwidth_index` kept next to
  `bandwidth_hz`, FFT type 7 only for pages that send `FFT_VIEW`).

Signal processing
- The uint8 -> complex conversion is a plain loop the compiler vectorizes;
  don't replace it with NEON intrinsics (slower, measured).
- Decimation is integer only, by divisors of 2.4 MHz.
  `BANDWIDTH_OPTIONS[]` is a legacy index list - don't reorder it - and the
  page's `BW_EFFICIENT` mirrors `decimation_for_bandwidth`: keep the two
  alike.
- NaN means "unknown" (map points, JSON input): test with
  `is_finite_value()` (`include/utils/parse_num.hpp`), and use
  `json_escape()` / `json_find()` (`include/utils/json_escape.hpp`) for
  hand-built JSON.
- Use the `std::` float overloads (`std::atan2(float, float)`), not the C
  `f` names: libstdc++ before GCC 14 has no `std::log10f`, `std::atan2f`,
  `std::sqrtf`.
- `AUDIO_SAMPLE_RATE` must match the browser's native rate.

Decoders
- Region-neutral by default: a decoder must work anywhere; region or vendor
  profiles may only add (LoRa plans, wildlife profiles).
- Never decode encrypted payloads or guess keys - only published default
  keys (Meshtastic's default channel). Real people's messages (pager pages
  and the like) never go into the repo.
- Writing or rewriting source-available plugin code: work from the
  standards and measurements without consulting GPL decoders (the licensing
  rules are in `../CLAUDE.md`).
- A plugin-API change touches `plugins/sdk/kraken_plugin.hpp` (wire
  protocol), `plugin_host.cpp` and `plugins/SDK.md` together.

Sandboxes and secrets
- While a debug playback or a recording plays (`playback::active()`,
  `include/playback.hpp`), nothing may reach heimdall, the settings store
  is a sandbox, and the web mapper, decoder log, Voice to Text, incidents
  and DoA recording pause. A new feature that sends to heimdall or the
  outside world, or persists anything, must respect that.
- The web-mapper key is persisted but never echoed or replayed to
  browsers. New routes stay behind the Host / Origin checks; `/recordings`
  and voice clips need the API token when one is set.
- The AI assistant controls the receiver only through `apply_command`
  (refused while "Read only" is on) and may not edit `plugins/*/LICENSE*`;
  keep new MCP control tools on that path (`docs/dev/ai-assistant.md`).

## Testing a change

There is no automated suite in the repo; each change gets tests written in
the job's scratch directory (not committed). The established setup:
- A TEST build: a copy of the tree with the ports in `include/config.hpp`
  moved (e.g. 2808x / 2809x) plus `KRAKEN_AI_MCP_PORT`, run from a scratch
  directory (its own `doa_settings.json`). Never test against the live
  stack's ports or settings. On Windows also give the exe another name - the
  launcher's stale-process sweep stops any `kraken_doa.exe`.
- A fake heimdall in Python that streams synthetic 8091 packets (a UCA
  scene with tones / signals at known bearings, the noise flag, retunes) and
  obeys 8092 commands.
- UI checks in headless Chromium over the DevTools protocol, with real mouse
  and key events (`.click()` doesn't focus). Chromium needs
  `--no-proxy-server --password-store=basic`, or page loads stall on the
  keyring.
- Decoders: `plugins/<id>/build/decoder --file x.cf32 [--channels 5 --uca R]`
  offline, and the debug playback end to end.
- CPU on the Pi: compare user-space cycles (`perf stat -e cycles:u`), not
  CPU %, which the ondemand governor makes swing by several points.

## Topic index (`docs/dev/`)

| Topic | File |
|---|---|
| Build, data flow, threads, components | `overview.md` |
| Config constants, MUSIC DoA, UCA element ordering | `configuration.md` |
| Arbitrary VFO bandwidth, the decimator (plans, block FIRs, shared pass, measurements) | `vfo-bandwidth.md` |
| Operating modes, VFO cards, VFO tuning step, spectrum zoom, independent panes | `operating-modes.md` |
| KrakenSDR Wideband (downconverter) variant | `wideband-variant.md` |
| Patch / 3D topology; antenna array calculator | `patch-3d-topology.md`, `array-calculator.md` |
| Spectrum views (FFT message type 7) | `spectrum-views.md` |
| Noise floor (the spectrum's 0 dB) | `noise-floor.md` |
| VFO audio (mixer, latency control) | `vfo-audio.md` |
| FFT (pffft); liquid-dsp | `fft-pffft.md`, `liquid-dsp.md` |
| Digital decoders: engine, AFC, protocol receivers, decoder tabs and tables | `digital-decoders.md` |
| Decoder plugins (process model, registry); digital voice (codecs) | `decoder-plugins.md`, `digital-voice.md` |
| Digital squelch; fixed-frequency decoders | `digital-squelch.md`, `fixed-frequency-decoders.md` |
| DoA per talker (ID separation, packets, covariances) | `doa-per-talker.md` |
| Plugins: ADS-B, AIS, ISM, LoRa, Pagers, RDS, Radiosonde, FLARM / OGN, Digital TV, LTE / 5G NR, Pulse radar, 1030 MHz, UAT, DME, ILS / VOR, Analog FM IDs, 406 MHz, Wildlife | `plugin-adsb.md`, `plugin-ais.md`, `plugin-ism.md`, `plugin-lora.md`, `plugin-pagers.md`, `plugin-rds.md`, `plugin-radiosonde.md`, `plugin-flarm.md`, `plugin-dtv.md`, `plugin-cells.md`, `plugin-radar.md`, `plugin-ssr1030.md`, `plugin-uat.md`, `plugin-dme.md`, `plugin-ils.md`, `plugin-analog-id.md`, `plugin-beacon406.md`, `plugin-wildlife.md` |
| Map pane and decoder map points | `map.md` |
| Station on the map; map search | `station-on-map.md`, `map-search.md` |
| Saved frequencies and map markers | `saved-frequencies.md` |
| Saved-frequency scan (plan, wideband mode, calibration hold, one VFO per frequency) | `saved-frequency-scan.md` |
| OpenStreetMap masts layer | `osm-masts.md` |
| Viewshed; path loss (P.1812, ITM, P.2108, coverage) | `viewshed.md`, `path-loss.md` |
| Incident map (pager addresses) | `incident-map.md` |
| Mobile direction finding; log replay | `mobile-df.md`, `log-replay.md` |
| Array calibration from ADS-B (experimental) | `array-calibration.md` |
| Debug playback | `debug-playback.md` |
| IQ & audio recorder, SigMF, playback of recordings | `recorder.md` |
| Decoder data log | `decoder-log.md` |
| Voice to Text | `voice-to-text.md` |
| AI assistant (MCP server, bridge, lockdown, sandbox, map layers, IQ capture) | `ai-assistant.md` |
| Web mapper output | `web-mapper.md` |
| Hot paths, DC correction, FFT dB, AM loops, MUSIC gates, dev tasks, key files, common issues | `reference.md` |
