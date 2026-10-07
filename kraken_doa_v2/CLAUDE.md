# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

This directory contains the **Heimdall DoA Client** - an FFT viewer and MUSIC Direction of Arrival processor for RTL-SDR systems. This client connects to the Heimdall Server (in `../heimdall_v2/`) to receive multi-channel IQ data and performs real-time signal processing.

**IMPORTANT:** For complete system architecture, see `../CLAUDE.md` (root-level documentation covering both server and client).

## Build Commands

```bash
# Quick start
make              # Build (auto-installs uWebSockets)
make run          # Build and run client
make deps         # Install system dependencies (Ubuntu/Debian)

# Build management
make clean        # Remove build artifacts
make rebuild      # Clean and rebuild
make distclean    # Remove everything including uWebSockets
make status       # Check dependencies and build status

# Debugging and profiling
make debug        # Build with debug symbols
make debug-arm    # ARM build with NEON debug output
```

# Important Build Note

DO NOT use all 4 cores when building with -j4 or nproc. Use 3 cores MAXIMUM. Otherwise the system will crash due to all cores being in use.

**ARM Detection:** The Makefile automatically detects ARM architecture (aarch64, armv7l) and enables NEON optimizations w
ith `-march=native -mtune=native`.

## Architecture Overview

### Data Flow Pipeline

```
TCP Client (port 8091)
  ↓
IQ Converter (uint8 → complex<float>)
  ↓
Raw Data Buffer (bounded queue, auto-cleanup)
  ↓
Decimation Processor Thread
  ├→ Decimator Manager (2 decimators: FM + MUSIC)
  │   ├→ SharedDecimator (thread-local instances)
  │   │   └→ Coefficient cache (95%+ hit rate)
  │   ↓
  │  FFT Work Queue (moodycamel::ConcurrentQueue)
  │    ↓
  ├→ Per-Channel FFT Workers (8 parallel threads)
  │   └→ FFTW3 (per-channel plans, true parallel)
  ↓
FM Demodulator Thread
  └→ Audio output (48kHz WebSocket stream)
  ↓
MUSIC DoA Processor Thread
  └→ Direction estimates (WebSocket stream)
  ↓
WebSocket Server (port 8080, HTTPS)
  └→ Browser UI (kraken_doa.html)
```

### Threading Model

**Worker Threads:**
- `data_receiver_thread`: TCP client → IQ conversion → raw buffer
- `decimation_processor_thread`: Decimation processing for all channels
- `fft_processor_thread_per_channel` × 8: One FFT worker per channel (true parallelism)
- `fm_processor_thread`: FM demodulation and audio streaming
- `doa_processor_thread`: MUSIC algorithm execution
- `web_server_main`: uWebSockets event loop (HTTP/WebSocket)
- Status monitoring thread: Periodic statistics printing

**Control commands run on ONE thread - the uWS loop:** every
`ControlHandler::handle_websocket_message()` call (browser messages, the
persisted-settings replay, web-mapper cloud settings) is dispatched on the
uWebSockets loop thread. The replay is deferred there by the data receiver on
its first heimdall connection (`loop->defer`); replaying from the receiver
thread raced browser commands (a change mid-replay could be overwritten or go
unsaved). From any other thread, use `loop->defer` - never call the dispatcher
directly.

**Thread Count:**
- Base workers: 8 threads
- Per-channel FFT: 8 threads (one per channel)
- VFO pipelines: with 2+ VFOs, the first runs on the receiver thread and the
  rest on a persistent 3-thread pool (`std::async` used to create an OS thread
  per VFO per packet)
- ThreadPools: 10 threads (2 decimators × 5 channels each)
- **Total: ~23-28 threads on 4 cores** (intentional oversubscription for throughput)

### Key Components

**Signal Processing** (`src/signal_processing/`):
- `fft_processor.cpp`: Spectrum calculation with FFTW3 wisdom files
- `fm_demodulator.cpp`: Phase-based FM demod with hardware atan2f
- `music_processor.cpp`: Eigenvalue-based direction finding
- `shared_decimator.cpp`: Thread-local decimators with coefficient caching

**Networking** (`src/networking/`):
- `tcp_client.cpp`: Connects to Heimdall server (ports 8091, 8092)
- `websocket_server.cpp`: Browser UI communication (port 8080, HTTPS).
  `idleTimeout = 30`: a client silent for 30 s is pinged and closed if it
  doesn't pong (browsers pong automatically), so a phone that drops off Wi-Fi
  is gone in ~30 s instead of lingering until TCP gives up (~15 min)
- `data_receiver.cpp`: Packet reception, IQ conversion, decimation coordination
- `binary_message.cpp`: Wire protocol encoding/decoding

**Utilities** (`src/utils/`):
- `iq_converter.cpp`: Optimized uint8→float conversion (compiler auto-vectorization)
- `raw_data_buffer.cpp`: Bounded queue with automatic old data cleanup
- `ring_buffer.cpp`: Lock-free circular buffers for FFT/audio
- `system_stats.cpp`: CPU/memory usage tracking
- `thread_pool.hpp`: Generic thread pool (used by SharedDecimator)
- `include/utils/json_escape.hpp`: `json_escape()` for every hand-built JSON
  emitter, and `json_find()` - the one flat-object reader (settings file,
  web-mapper cloud messages) - which decodes every JSON escape including
  `\uXXXX` (to UTF-8, surrogate pairs too), so values round-trip. It matches
  only KEYS of the top-level object (a string value equal to the key name,
  key text inside another string, or a nested object's key is skipped)

**Managers** (`src/`):
- `channel_manager.cpp`: Channel state (frequency, gain, tuner mappings)
- `decimator_manager.cpp`: Dynamic decimator allocation (FM + MUSIC)
- `control_handler.cpp`: WebSocket command processing
- `message_builders.cpp`: JSON/binary message construction

## Configuration

Edit `include/config.hpp`:

**Network:**
- `WEB_PORT`: Client web UI (HTTPS/WSS, default 8080)
- `DOA_HTTP_PORT`: Plain HTTP DoA value page for Android app (default 8081)
- `TCP_DATA_PORT`: Heimdall data port (default 8091)
- `TCP_CONTROL_PORT`: Heimdall control port (default 8092)

**Signal Processing:**
- `FFT_SIZE`: FFT size (default 16384)
- `MAX_CHANNELS`: Compile-time channel CEILING (8, matches heimdall's
  NUM_DEVICES). The live count is the runtime atomic `active_num_elements`,
  synced from the 8091 packet header
- `SAMPLE_RATE`: Expected sample rate (default 2.4e6)
- `AUDIO_SAMPLE_RATE`: Browser audio rate (default 48000) - **MUST match browser's native rate**
- `DECIMATION_FACTOR`: Default decimation (default 10)

**MUSIC DoA:**
- `DOA_NUM_ELEMENTS`: Compile-time CEILING on antenna elements (8). MUSIC and
  the beamformer follow the runtime count: each processing pass calls
  `syncElementCount()`, which re-reads `active_num_elements` and, on a change,
  resizes all per-element state (accumulator, covariance, steering vectors,
  MVDR/FD-DAS tables) and drops stale statistics. `DOA_DEFAULT_ELEMENTS` (5)
  is only the pre-connect default
- The UI adapts automatically: `onActiveElementsChanged()` regenerates the
  channel selector, UCA/ULA/custom diagrams, position table and SNR displays
  from the `active_elements` status field
- `DOA_BLOCK_SIZE`: Samples per block (default 256)
- `DOA_NUM_SNAPSHOTS`: Covariance snapshots (default 64)
- `DOA_ANGULAR_RESOLUTION`: Degrees per step (default 1)

**UCA element ordering (array chirality):**
- The UCA is expected to be wired **CLOCKWISE**: ANT0 on the +x axis, each
  subsequent channel stepping clockwise (0, -72, -144, -216, -288 deg on a
  5-element ring). This applies to both standard KrakenSDR arrays and the
  Wideband boards
- `uca_angle_sign()` (`include/globals.hpp`) returns `-1.0` and is the single
  choke point - it mirrors the element angle about +x (ANT0 stays put, the
  rest reverse order) so DoA OUTPUT stays unit-circle CCW. Flip it to `+1.0`
  for a counter-clockwise array, and flip `UCA_ANGLE_SIGN` in
  `kraken_doa.html` to match
- Fed by: MUSIC UCA steering vectors and the custom-position UCA default
  (`music_processor.cpp`), the beamformer geometry (`beamformer.cpp`), and in
  the UI the `drawUCADiagram()` element layout plus the default custom
  positions. ULA and user-entered CUSTOM positions are taken literally and
  are NOT mirrored

**Bandwidth Options:**
- `BANDWIDTH_OPTIONS[]`: Integer decimation factors (1 to 2400)
- Range: 2.4 MHz down to 1 kHz (no resampling)
- `DEFAULT_BANDWIDTH_INDEX`: Initial selection (default 7 = 240 kHz)

**KrakenSDR Wideband (downconverter) variant:**
- Run with `--wideband` (heimdall too) - see root `../CLAUDE.md` for the design
- `WB_VARIANT_IF_HZ` (1268 MHz), `WB_LO_MIN_HZ`/`WB_LO_MAX_HZ`: must match
  `heimdall_v2/config.h`; give the UI its RF limits and the LO readout
- Mixer side FOLLOWS THE FREQUENCY: the FREQ handler auto-selects when the
  current side can't reach the RF (`wb_auto_side`: high <= 4132 MHz, below
  above) and records/broadcasts the change; heimdall applies the same rule
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

**Operating modes (top-bar Mode selector):**
- `OPERATING_MODE:coherent|wideband|independent` (control_handler.cpp,
  persisted after FREQ/GAIN/DECIMATORS; `WIDEBAND_MODE:` is the old form):
  refused except coherent on the downconverter variant; stops the discrete
  scanner outside wideband and the continuous one outside coherent; sends
  heimdall `set_operating_mode`; entering independent re-sends the saved
  per-tuner tuning. `apply_operating_mode_state(mode)` is the one place the
  flags (`wideband_mode_enabled`, `independent_mode_enabled`,
  `multi_tuner_mode()`) change: leaving coherent parks DoA
  (`doa_enabled_before_wideband`), returning restores it; DOA:/BEAMFORMING:
  outside coherent only update what is restored. It broadcasts
  `{"operating_mode":{"mode","tuners":[{f,g}]}}` (also sent on connect).
  The coherent `active_channel` (CH selector) is saved on leaving coherent
  and put back on return (the FM source VFO moves it to its tuner meanwhile).
  The handler saves the mode itself (`SettingsStore::record`): OPERATING_MODE
  is in `is_query_command` (no sync_cmd echo), which also skips the generic
  save - it was never remembered, so each cold start the replayed default
  switched heimdall (which had restored independent) back to coherent
- Page: only the `operating_mode` message switches the mode at once; the FFT
  frame format and system_status flags are hints (`opHint`) that must disagree
  for 1.5 s - stale frames around a change used to tear the panes down and
  rebuild them per message. Pane resizes are debounced (120 ms) and skipped
  when the grid size is unchanged; canvas sizes are only re-assigned when they
  change; `WaterfallDisplay.resize` keeps the picture; the grid reserves its
  scrollbar gutter (`scrollbar-gutter: stable both-edges`); the top bar's
  hw-stats has a fixed width so it can't re-wrap the bar
- `TUNER_FREQ:ch:mhz` / `TUNER_GAIN:ch:db` (independent only) -> heimdall
  `set_independent_tuner`; stored in `indep_tuner_freq_hz` / `indep_tuner_gain_db`
  and persisted as the composite `TUNERS:f/g,...`. `exact_tuner_hz(ch)` =
  the requested frequency when the header's float agrees (~64 Hz resolution)
- `note_server_mode()`: the data receiver reads heimdall's mode from the
  phase-state flags (0x400 / 0x800, once seen) and adopts a mismatch that
  outlasts the client's own change by 3 s (heimdall restored its saved mode)
- Multi-tuner gates (`multi_tuner_mode()`): every channel converted and
  FFT'd, MUSIC/beamforming off, each VFO decimates `tuner_channel`, FFT squelch
  reads that tuner, digital RF label = `exact_tuner_hz(tuner) + offset`
- Squelch outside coherent: always the FFT method on the VFO's own tuner
  (pipeline, fm_only indicator and the audio gate) - the eigenvalue methods
  need MUSIC, which doesn't run, so their state froze (and the audio with it);
  no beamformed-FFT squelch either (a saved "beamforming on" read a stale
  overlay). The saved method is kept and applies again in coherent. The page
  offers only "FFT Peak" there (`sqEffMethod`; the VFO cards rebuild on a mode
  change - `decListMode` is part of updateDecimatorList's structure check)
- VFO tuner: `SET_DECIMATOR_FREQ:id:khz[:tuner]` (independent: the offset is
  relative to that tuner; the echo carries `tuner`), `decimator_info[].tuner`,
  DECIMATORS snapshot field 10
- Selected VFO (`activeDecimatorId`, all modes): `selectDecimator(id)` -
  clicking its card in the Decimators box (pointerdown anywhere on it) or
  grabbing its tuning bar (main display or pane); the card gets an outline in
  the VFO's colour (`.dec-item.sel`, `decHighlightSelected`, re-applied after
  every list update; a removed VFO hands the selection to the first). A click
  on empty spectrum places this VFO, and only its edges resize
- FFT message type 6 (independent): `u32 6, u32 nch, f32 avg_alpha, f32
  sample_rate, f32 bin_step`, per tuner `u32 center_hz, f32 gain_db, u32 ds,
  u8 min[ds], u8 max[ds]` (bin i at center - sr/2 + i*step)
- UI (kraken_doa.html, "OPERATING MODE" block): `setOpMode` / `applyOpModeUI`
  (body classes `mode-*`; `.coh-only`, `.wb-only`, `.indep-only` elements),
  `#indep-grid` panes (`panesBuild`; flex rows sized by `panesLayout`: up to
  3 per row - 5 tuners = 3 + 2, 4 = 2 x 2 -, the last row centered, >= 360 px
  wide / 230 px tall; each head has a digit frequency selector like the top
  bar's - click top/bottom half or mouse wheel, `paneFreqSelectorInit`, own
  `.pfd` class since the top bar queries `.freq-digit` page-wide - plus a gain
  slider; canvas spectrum `paneDrawSpec`, a
  `WaterfallDisplay` each, overlay `paneDrawOverlay` with VFO bars + AI
  badges - tuning bars in the main display's style: a grab handle centred in the spectrum AND the waterfall section, edge handles; `paneHit` = handle > active VFO's edges > body, the edges drag the bandwidth (snapped like `drawBox`, tooltip), bars are clipped to the plot area and a clipped side has no edge; an FFT squelch is drawn like `drawBox`'s - dashed yellow line at the level inside the bar, on the spectrum, labelled - and is draggable (all modes: `squelchHitMain` / `paneHit` kind 'squelch' within 5 px of the line, highest priority, ns-resize cursor; `squelchSetFromDrag` moves the card slider, sends DEC_SQUELCH_LEVEL at most every 80 ms + the final level, 0-80 dB); pointer handling `paneDown/Move/Up`: a VFO bar drags the VFO, empty spectrum pans the tuner like the main display - throttled TUNER_FREQ every 50 ms, final value on release, the drawn spectrum follows the data, not the drag - and a click without moving places the selected VFO); the top bar's frequency
  digits and gain (`.not-indep`) are hidden in independent mode - each pane
  has its own (`selTuner` = the highlighted pane; `sendFreqChange` / `sgn`
  still route to TUNER_FREQ / TUNER_GAIN for it), `vfoCenterMhz(dec)` for VFO absolute
  frequencies, per-pane waterfall auto-range (`computeAutoRange`)
- Zoom (kraken_doa.html "SPECTRUM ZOOM" block): `{level 1..16, off}` per
  display - `mainZoom` (coherent / wideband) or each pane's `p.zoom`;
  `zoomSlice()` cuts the visible part out of the data BEFORE it reaches the
  plot / waterfall (`applyClip`, `indepOnFrame`), so spectrum, tuning bars and
  waterfall share it. Mouse wheel = x1.25 around the cursor (`zoomWheel`; on
  #fp / #wf-container, or a pane's overlay - which selects it); the Zoom
  slider (wf-controls bar, log2 x 25) shows / sets the main or the selected
  pane's zoom (`zoomSyncSlider`)
- Split: `#fp-split` drags the main spectrum height (`kraken_fp_h`,
  localStorage; window resize refits uPlot); each pane's `.tpane-split` sets
  the grid's `--spec-frac` (shared by every pane, `kraken_pane_split`);
  double-click = default. Pane digits are 22 px (1.5x)
- `ai/sigtool.py capture` picks the tuner whose band holds `--freq` (status
  `tuner_frequencies`) outside coherent; `--channel N` forces one

**Patch / 3D topology (`TOPOLOGY:PATCH3D`):**
- UI topology button for the calculator's "Patch / 3D" layouts (upright patch
  panels: square/diamond + centre, grid, offset rows, L-shape, ring; 3D: ring +
  centre mast, staggered ring, stacked rings). The backend runs it as CUSTOM
  (`PATCH3D` maps to `ArrayTopology::CUSTOM`); the page generates the
  positions (`layoutPositions()` in kraken_doa.html - keep it identical to the
  calculator's geometry) and sends `CUSTOM_POSITIONS:`
- The layout is UI state: `ARRAY_LAYOUT:shape,elements,size_mm,height_mm`
  (whitelisted shape, persisted, replayed to browsers; no effect on the DSP).
  Picking a layout also sets the custom Direction (`CUSTOM_MODE:`): Forward
  only for patch panels (front/back ambiguous), Both for 3D
- An element-count change does NOT regenerate the positions (the physical
  array didn't change): the panel warns and offers "Re-apply"
- Custom front/back truncation (`CUSTOM_MODE:FORWARD|BACKWARD|BOTH`, separate
  from `ULA_MODE:`) is applied to the 2D spectrum BEFORE the az/el peak search

**Antenna array calculator (`array_calculator.html`):**
- Standalone page (no external resources), served by its own route in
  `websocket_server.cpp` (read per request like kraken_doa.html, so edits
  need no rebuild; the ROUTE itself needs a rebuild). Opened by the
  "📐 Array Calculator" button in the MUSIC DoA box (`openArrayCalculator()`),
  which passes the receiver's state as URL params: `n` elements, `f` MHz,
  `topo` UCA|ULA|CUSTOM, `r` radius mm, `s` spacing mm
- Port of krakensdr_docs `Antenna_Array_Size_Calculator.xlsx` (same formulas,
  but λ = c/f instead of 300/f, so ~0.07% off): UCA radius = m·λ/(2 sin(π/N)),
  ULA length = (N-1)·m·λ, est. resolution = deg(1.22/aperture_in_λ)/10 with
  the sheet's aperture definitions; spacing multipliers 0.5..0.1, ≤ 25° = ok.
  "Patch / 3D" tab layouts are sized so the largest nearest-neighbour
  spacing = m·λ: 3D (ring + centre mast, staggered, two stacked rings - add
  an elevation estimate) and flat patch arrays (`p-*`: polygon + centre with
  side or corner forward - square / diamond at N=5 -, 2×k grid, two offset
  rows (triangular lattice), L-shape, ring). Patch arrays are always upright
  panels facing the horizon (x = 0, numbered clockwise seen from behind):
  azimuth + elevation, but front/back mirror-ambiguous. (A flat board facing
  the sky was dropped - z = 0 makes the receiver's CUSTOM MUSIC azimuth-only.)
  Patch layouts keep their
  element spacing across layout / count changes
- The frequency marker on the "Your array" chart is draggable (x axis
  frozen during the drag); the MHz field steps 0.1 MHz from any typed value
  (no `min` attribute, so the step base is the value attribute, which the
  page keeps in sync)
- Coordinates use the receiver's frame (x forward = ANT0, y left, z up, UCA
  clockwise). "Use this array in the DoA receiver" calls
  `window.opener.applyArrayFromCalculator(cfg)` in kraken_doa.html, which
  sets UCA radius / ULA spacing / the Patch / 3D layout through the normal UI
  paths (so they are sent and persisted); refused if N differs from the live
  element count. The receiver passes its layout as URL params `shape`, `ls`
  (size mm), `lh` (height mm)

**Digital voice/data decoders (one per VFO): engine `src/digital/`, decoders `plugins/`:**
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below);
  kraken_doa contains no protocol code. Shipped: p25, dmr, tetra, dstar,
  nxdn, mpt1327, pocsag, aprs, adsb (see *ADS-B* below), ais (see *AIS*)
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
  and beamforming off) only the FM source and the VFOs with a decoder are
  decimated, on one channel (`fm_only` in data_receiver.cpp)
- Modes (`dig::Mode`): OFF, AUTO, PLUGIN (+ plugin id). Wire / snapshot form
  `OFF`, `AUTO`, `PLUGIN:<id>`; `parse_mode_string()` maps the old protocol
  names (P25, DMR, TETRA, DSTAR, D-STAR, NXDN, MPT1327) to their plugins, so
  saved settings and old clients keep working
- `Engine` (digital_decoder.cpp): AFC mixer at the VFO rate -> one msresamp
  per distinct plugin `sample_rate` (shared by the plugins at that rate) ->
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
  plain copy (a sin/cos per sample cost ~5% of a core at 2.4 MHz). A VFO
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
  (utils/parse_num.hpp), never `std::isfinite` - kraken_doa builds with
  -Ofast, which folds it to true (a `"h":nan` broke the page's JSON.parse);
  digi_test (-O2) doesn't show that
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
    plugin enables only P25, the dmr plugin only DMR
  - `P25Proto` (plugins/p25): NID BCH(63,16) by table search, TSBK (rate 1/2
    trellis + CRC), LDU1/LDU2 (Hamming(10,6) + RS(24,12)/(24,16)), HDU
    (Golay(18,6) + RS(36,20)), TDULC (Golay(24,12) + RS(24,12)), PDU header.
    IDEN_UP tables turn channel numbers into MHz. Option `nac`
  - `DmrProto` (plugins/dmr): CACH TACT (timeslot), slot type Golay(20,8)
    (max 2 corrections - a third of random words are within 3), BPTC(196,96)
    voice LC header / terminator (RS(12,9)), CSBK, PI and data headers, EMB
    QR(16,7) + embedded LC, GPS and talker alias LCs. Control-channel chatter
    (Aloha, broadcasts, acks) only with the verbose option. Option `slot`
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
- Tables (`kp::Host::table_columns / table_row / table_remove`, wire 25-27):
  one per plugin in `Report` (headings + rows by key, max 1000, a row not
  updated for 10 min left out; `table_clear()` empties the rows on a retune /
  mode change, headings stay). `status_json(.., tables)`:
  `"tables":{"<id>":{"cols":[..],"rows":[[key, age_s, cells..]]}}` -
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
  the map, selects + centres the marker)
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
  in AUTO once AUTO detected it
- UI (kraken_doa.html, "Digital decoders" block): any number of VFOs decode
  at once. Each VFO card (Decimators box, `updateDecimatorList`) has a
  "Digital decoder" tick (`digToggle`: DIGITAL_MODE AUTO or the mode picked
  while off, remembered per VFO in localStorage) and a mode list
  (`digSetModeFor`: picking a decoder also switches it on) plus a state line
  (`digRenderCards`, also run at the end of updateDecimatorList, which
  rebuilds the cards). The bottom panel has one tab per decoder MODE
  (plugin), not per VFO: `infoTab` = `dig:<plugin>` | `ai`
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
  kraken_doa_v2): `g++ -std=c++20 -O2 -Iinclude tools/digi_test.cpp
  src/digital/*.cpp -lliquid`
- Validated on live P25 (VFO on a trunked control channel + its voice
  channel) and DMR Tier III here, and on recordings for TETRA, D-STAR, DMR,
  NXDN, MPT1327 and P25 (regression of 33 checks incl. voice passes through
  the plugins)

**🗺 Map (right-hand pane) + decoder map points:**
- Plugins report positions with `host.map_point(kp::MapPoint)` / `map_remove`
  (kraken_plugin.hpp; `kp::Info::map = true` makes the decoder tab offer
  "🗺 Plot on map" - also shown once a decoder has points; off until the
  user ticks it). Stored per decoder in its `Report` (above), sent only
  while ticked. Reporting today: adsb (aircraft), aprs (stations / objects /
  items, marker from the APRS symbol, course / speed from `ccc/sss`, killed
  objects removed, ttl 1 h), dmr (GPS info LC -> the talking radio: id =
  radio ID from the slot's voice LC, label = talker alias), dstar (GPS
  slow data: NMEA RMC / GGA with a valid checksum, DPRS with a valid
  CRC-16/X.25 -> the MY callsign / DPRS callsign). The lib decoders use
  `dig::Report::map()` / `station()` (dig_common.hpp)
- `MessageBuilders::build_map_message()`, 1 Hz on TOPIC_CTL (websocket_server
  map_timer, sent even when empty - it carries the station):
  `{"map":{"full":bool,"station":[lat,lon]|null,"pts":[{v,p,i,la,lo,l,k,h,
  a,s,x,age,ttl}],"keys":["vfo|plugin|id",...]}}` - pts = the points
  updated since the previous push (per-VFO map seq, like the digital
  events), keys = every live point of every decoder that is on with
  "Plot on map" on: pages drop markers whose key is missing (removal,
  expiry, Plot on map off, decoder off). `request_map_full()` (GET_MAP from
  a page opening the map, DIGITAL_OPT map) makes the next push carry all
- UI (kraken_doa.html, "🗺 MAP" block; state `mapState` etc. in the globals
  block because `applyOpModeUI` -> `rpApply` can run before that block is
  evaluated): own canvas slippy map, NO map library - the page must work
  without internet (uPlot is inlined for the same reason). Web Mercator,
  fractional zoom (tiles of the nearest integer zoom scaled), drag / wheel
  / pinch / double-click, +/− ⌖ (station) ⤢ (fit all), Street (OSM
  tile.openstreetmap.org) / Satellite (Esri World_Imagery + the
  World_Boundaries_and_Places label layer), attribution shown. Tiles: cache
  of 400 (LRU), a failed tile retried after 15 s doubling to 5 min, a missing
  tile drawn from a loaded lower-zoom tile; underneath always a lat/lon
  graticule, so with no internet the markers sit on a plain grid + an
  "offline" notice (tile errors in the last minute and no tile loaded for
  30 s). Station = yellow mast + range rings (step to suit the zoom).
  Markers by `kind` (aircraft rotated by heading, vehicle, ship, station,
  person, point) in the VFO's colour, label + altitude, greyed after 30 s
  without an update; trails (client-side, last 15 min / 400 points);
  click = popup (the plugin's "key: value" info lines + position, distance
  / bearing from the station, age); status line = counts per VFO decoder
- Right-hand pane: `.doa-panel#right-pane` holds the DoA displays and
  `#map-pane`. `mapToggle()` (🗺 Map button under the waterfall, ✕, the
  decoder tab's "Show map") / `rpShow('doa'|'map')` (header tabs, coherent
  only) -> `rpApply()`: body classes `map-on` (pane shown in every mode)
  and `rp-map` (map tab visible - always outside coherent); width
  `--map-w` (42% default, `#rp-resizer` drag on its left edge, double-click
  = default). The calibration greying applies to `.doa-content` only.
  localStorage `kraken_map` {on, tab, w, layer, z, mx, my, trails}; first
  open centres on the station (else the world) and fits the first markers
- Tests: scratchpad mock-server test t_map (offline tiles forced by
  pointing the layers at a closed port) and an end-to-end run of the real
  kraken_doa (from a scratch dir - its own doa_settings.json) fed by a fake
  heimdall streaming a synthetic 1090 MHz recording

**Incident map (`src/incidents.cpp`, `geo_address.cpp`, `geo_http.cpp`):**
- Plugins send free-text messages with `host.message(from, text)`
  (`kp::Info::messages`; POCSAG: every alphanumeric page). Wire MESSAGE (28)
  -> Engine -> `dig::set_message_handler()` (set by `incidents::start()`;
  unset in digi_test: dropped) with the DigitalDecoder* (resolved to its VFO
  through `decimator_manager`, so a removed VFO's messages are dropped)
- Address finding (`geo::geocode`, no local street data - the user wants an
  online lookup, NOT a downloaded OSM extract). Latin-script countries; text
  folded to upper-case ASCII words (accents; ß -> SS, Æ -> AE, Œ -> OE;
  apostrophes kept in the word for the search - Nominatim needs O'CONNELL -
  but dropped from keys; , ; : ( ) recorded as breaks a name doesn't cross).
  Candidate groups, those with a house number next to them first, <= 10
  lookups per message:
  1. name + type after (English ST/RD/AVE..., German / Dutch / Nordic /
     Hungarian / Turkish words written separately: STRASSE, ALLEE, WEG,
     GATE, GATAN, VEJ, UTCA, CADDESI...) + optional direction ("QUEEN ST W")
  2. type before + name (RUE, AVENUE/AVENIDA, BOULEVARD, CALLE, VIA, PIAZZA,
     RUA, PRACA, UL/ULICA, JL/JALAN... - `prefix_types()`; UL / OS are not
     in OSM's names and are dropped from the key; a name without its prefix
     type in OSM also matches)
  3. one word with the type joined on (`COMPOUNDS`: -STRASSE/-STR/-STRAAT
     -> ST, -WEG, -PLATZ, -ALLEE, -GRACHT, -LAAN, -GATAN, -VAGEN, -GADE,
     -VEJ, -VEIEN, -KATU, -TIE, -UTCA...)
  House number before or after the street ("NO" / "NR" before it skipped).
  Name words: up to 4, never a stop word (English pager words + FEU,
  INCENDIO, BRAND, POZAR...) or a word with digits except ordinals (5TH,
  42ND). `name_word()`: SAINT -> ST, MOUNT -> MT, NORTH -> N (US "N MAIN
  ST" = North Main Street), FIFTH -> 5TH. Each candidate is searched in Nominatim (`geo::Nominatim`,
  nominatim.openstreetmap.org jsonv2 + addressdetails, viewbox = the radius
  around the station, bounded) - a result counts only if its road's key
  (`street_key`: ST/STREET, SAINT/ST, MOUNT/MT...) is the candidate's
  (motorways / highways also without leading words). Choice: (1) the house
  (house_number) in a place the text names (result suburb / town / city
  key - also without CITY / CENTRAL / N / MITTE... - found in the rest of
  the text, compared loosely: AE/OE/UE = A/O/U so ZUERICH = Zürich, no
  spaces / apostrophes), (2) the junction with a cross street (closest pair
  <= 1.5 km; "5TH AVE / E 42ND ST"), (3) a search with the 1-2 words after
  the street ("17 ATKINSON AVE OTAHUHU"; Nominatim only answers when every
  word matched), (4) the house nearest the station, (5) the street in a
  named place, (6) the nearest. Key comparison (`same_street`): equal, or
  equal loosely (umlauts as AE/OE/UE, spaces: "MH" = "M.H."), or OSM adds
  a direction (O'Connell Street Lower, Queen Street West).
  Confidence high / medium / low (several same-named streets, or the words
  after the street found nothing). <= 8 lookups per message
- Nominatim usage policy: User-Agent naming kraken_doa, >= 1.1 s between
  requests (callers queue on the lock), answers cached 24 h (2000). Plain
  OpenSSL HTTPS GET (`geo::https_get`, certificate verified). HTTP 429 /
  502-504: no requests for 1 min doubling to 30 min; reported as offline,
  so the messages wait (seen during testing: ~100 test lookups in a few
  minutes got 429s)
- `incidents.cpp`: worker thread + queue (300). No internet (no HTTP answer)
  -> the message is retried every minute for 30 min; an error answer or
  no address -> dropped. An incident = VFO + address key (street key +
  number) per PLUGIN - the same address paged within 2 h on ANY VFO adds a
  page to it and its (VFO, frequency) to `heard` (fire + ambulance pagers
  paging one address = one incident; table "D0, D2" / "153.2750,
  157.4500", popup "Received on"). `vfo` / `rf_hz` = the first / latest.
  The message handler gets the VFO's RF (`MessageHandler(.., rf_hz)`). Kept 24 h, max 5000,
  saved to incidents.tsv (cwd; reloaded at start; written <= every 10 s and
  at shutdown via `incidents::save_now()`). A new incident logs "📍
  Incident: ..." in the decoder's event log
- Output: `incidents::map_json(plugins)` in `build_map_message` (plugin
  "incidents", kind "incident" = red triangle in the UI, `v` = the VFO that
  received it; shown while any decoder running that plugin -
  `DigitalDecoder::active_plugin()`, the fixed or detected one - has Plot on
  map on, so a removed VFO's incidents stay visible), and NOT per VFO:
  `incidents::message_json()` -> `{"incidents":{seq, geo, cols:[VFO, MHz,
  Paged, ...], rows:[[id, age, plugin, vfo, cells..]]}}`
  (`MessageBuilders::build_incidents_message`: from the 1 Hz map timer when
  `incidents::seq()` (bumped by new / updated / expired / clear) or the geo
  status changed, else every 15 s; forced on `GET_INCIDENTS` (page connect)
  and INCIDENTS_CLEAR). UI `incOnMessage` -> `incData`, rows shown in the
  tab of their plugin (`digIncRows`); `incidents::status_json()` also rides
  as `"geo"` next to `"digital"` (UI `geoStatus`)
- Commands: `GEO_RADIUS_KM:<10-1000>` (persisted, schema before
  DECIMATORS), `INCIDENTS_CLEAR`, `GET_INCIDENTS`. UI: `digIncidentsRow(plugin)`
  in the tab of a messages plugin (`kp::Info::messages`; Plot on map shown
  for its VFOs too). Test: scratchpad h/t_tabs.py (POCSAG on D0, a second
  VFO joins, switches to DMR, is removed; incidents merged across VFOs) and
  t_tabs_adsb.py (merged aircraft table)
- Tests (scratchpad): `tools/geo_test.cpp` (`geo_test LAT LON KM "text"...`,
  online) on NZ fire / ambulance style pages; end to end on a TEST build
  with other ports (scratchpad/kbuild: config.hpp ports 18080/18081/18091/
  18092 - the live stack keeps 8080/8091) fed synthetic POCSAG pages
- Tested (tools/geo_test, scratchpad world.py: sample pages with a station
  in each country): US (S Wacker Dr, N Michigan Ave, 350 5th Ave, the 5th
  Ave / E 42nd St junction), Canada, UK, Ireland, Australia, South Africa,
  NZ, France, Germany, Austria, Switzerland, Netherlands, Spain, Italy,
  Portugal, Brazil, Sweden, Denmark, Norway, Finland, Poland, Hungary - the
  right street everywhere, mostly the house itself
- Not found: non-Latin scripts (Cyrillic, Greek, Arabic, CJK - not read),
  route numbers (SH1, I-95, A1), landmarks, names OSM spells out where the
  page abbreviates them (Jakarta's "Jl. MH Thamrin" is "Jalan Mohammad
  Husni Thamrin"), streets Nominatim can't match (e.g. "SOUTHERN MOTORWAY"
  - OSM: "Auckland Southern Motorway", found only with "Auckland")

**Mobile direction finding (🗺 Map, coherent mode; `src/rdf_engine.cpp`, `src/rdf_mapper.cpp`):**
- Live lobes: the sampler thread (rdf_mapper, 10 Hz) takes every NEW MUSIC
  frame of every VFO with DoA (`getResultStampMs` changes; not
  `isResultStale`), turns it to the north frame with the station heading
  (compass bearing tau of array angle b: tau = heading - b; b unit circle
  CCW from ANT0 = vehicle front) and publishes it as 720 x 0.5 deg u8 - the
  pseudospectrum LINEAR, min..max, the MUSIC DoA plot's (PolarPlot) scale; a
  dB scale drew a far fatter lobe than the plot - + the sub-bin peak in `{"rdf":{on, range_km, state, station
  {lat, lon, hdg, spd, src}, vfos:[{id, mhz, lobe, peak, conf, held, frames,
  gate_m, moved_m, records}]}}` (websocket_server rdf_timer, 2 Hz).
  `held` = GPS course while slower than MIN_SPEED (drawn dashed)
- Time alignment (the Android app pairs a frame with the latest fix): the
  GPS track is kept 20 s (fix valid time = local receive time -
  FIX_LATENCY_S 0.15 s); a frame is placed at its stamp - FRAME_LAG_S 0.3 s
  and gets the interpolated position + heading (circular) + the turn rate
  over +-0.5 s (`fix_at`)
- Gating per frame (`state`): coherent mode + DoA on + a fix (< 3 s old) +
  no noise-source calibration (`doa_is_calibrating`) + collecting on (RDF:)
  + not a static location + GPS course: speed >= 2 m/s and turn rate <= 8
  deg/s (compass: <= 30 deg/s). Frame bearing sigma = hypot(2 deg MUSIC /
  site, heading: compass 4, GPS course 1.5 + 8 / speed)
- `rdf::north_lobe` (rotate + interpolate + Gaussian blur + normalise) ->
  `rdf::Gate` per VFO: frames are combined as the normalised GEOMETRIC mean
  of their (5 % floored) probabilities - repeated views of one bearing
  multiply (an arithmetic mean only widened the lobe and lost to the
  Android app in the simulator) - and the window closes after R *
  tan(3 deg) of travel, 15-60 m (R = distance to the estimate, 1 km before
  one); a pause > 10 s drops the window, > 90 s without closing too.
  Stopped = no records (no double counting)
- `rdf::Grid`: cells in METRES (azimuthal equidistant around an origin,
  n x n), cell bearing = planar angle in that frame + the meridian
  convergence at the record (exact to << 0.1 deg over 50 km; the Android
  app's grid is a lat/lon lattice). Each record adds alpha (0.5) * ln((1 -
  eps) * p(bearing - delta) * BINS + eps) (log-likelihood ratio vs "no
  information", eps 0.05 = multipath / outlier floor; p linear-interpolated
  on 0.5 deg bins) per offset plane delta in {-8..8 step 2} deg; cells within
  max(30 m, 1.5 cells) of the record get 0 (the bearing of a cell around the
  receiver is undefined). Marginal M = log-sum-exp over delta with a N(0, 4
  deg) prior: a systematic rotation of all bearings (array mounting,
  compass) is integrated out instead of fitted + applied (the Android app's
  CAL sweep) - the offset's posterior mean / sd is reported. Posterior read
  scale s = min(1, 40 / (alpha * records)) (systematic errors don't average
  out). `estimate()`: MAP cell + parabola per axis (the posterior mean of a
  banana-shaped early posterior is far off), the main mode (8-connected
  cells >= 1e-4 of the peak) -> covariance ellipse (+ a cell's variance),
  50 / 95 % HPD radii, mode mass, `at_edge`
- `rdf::Solver`: coarse grid 256 x 256 over +-range_km (default 10 -> 78 m
  cells) with its origin at the first record; re-centred (rebuilt from all
  records) when the vehicle gets > 0.7 range from it (on a confident
  estimate, else on the vehicle). Fine grid 128 x 128 around a confident
  coarse estimate (mode mass > 0.5, not at the edge, r95 < range / 3): half
  = clamp(4 * r95, 250 m, range / 3), rebuilt from all records when the
  estimate moves > 0.3 half or the wanted size changes > 1.6x, else updated
  per record; the estimate comes from the fine grid while its mode isn't at
  its edge. Max 4000 records (then the older half is thinned)
- Engine thread (rdf_mapper): owns the solvers; a VFO retuned by more than
  10 kHz starts over (its old session is parked by frequency); records are
  saved to `rdf_session.bin` (cwd, gitignored; "KRDF1" + blocks of freq,
  saved time, records with the lobe as u8 log probability over 20 nats) every
  30 s and at shutdown, and a VFO on a frequency within 5 kHz of a saved
  (< 24 h) session resumes it. Since the per-talker maps the file is
  "KRDF2" (each block also carries the talker ID, "" = the VFO; "KRDF1" is
  still read) and every map is keyed VFO + talker (`Key` in rdf_mapper.cpp) Grid messages at most every 2 s per VFO:
  `{"rdf_grid":{vfo, mhz, records, range_km, nats, coarse/fine:{n, cell_m,
  nw, se, data (b64 u8, row 0 north, 255 * (1 + s (M - Mmax) / 12 nats))},
  est:{lat, lon, r50, r95, a, b, ang, mass, edge, off, off_sd}, lines:[[lat,
  lon, bearing, t]] (last 150 records)}}`; `GET_RDF` (page opening the map)
  makes the next push carry every VFO
- Right-hand pane tabs (coherent mode, map on): MUSIC DoA | 🗺 Map | ⊞ Both
  (`mapState.tab` 'both' -> body class `rp-both`: the DoA displays on top -
  side by side - and the map below, `#rp-vsplit` divider dragging
  `mapState.dh` -> `--doa-h`, double-click = 42 %; `rpApplySplit`)
- UI (kraken_doa.html "Mobile DF" block): "📡 DF" panel bottom-left of the
  map (coherent only, hidden until the first `rdf` message): VFO select (All
  / Dn), Lobe / Lines / Heat toggles (`mapState.rdf*`, localStorage),
  Collect (RDF:), Range (RDF_RANGE_KM:), ◎ (centre on the estimate), Reset;
  status per VFO (bearings, next in N m, TX +-r95, edge / mode-mass
  warnings, measured offset when |off| >= 2 deg and > 2 sd after 20
  records) and the reason frames aren't used (`RDF_STATE`). `rdfDraw` (in
  `mapDrawNow` before the rings): heat canvases (`RDF_LUT` blue..red,
  transparent below 8 %) stretched between nw / se, bearing lines (1.5x the
  range, older fainter), estimate X + 2.45-sigma (95 %) ellipse (dashed at the
  edge), lobes (radius 0.3 of the map, dashed when held) + the peak line,
  the heading tick. Heat map: the selected VFO's, or in All the only VFO
  that has one
- Simulator (`tools/rdf_sim.cpp`, `g++ -std=c++20 -O2 -Iinclude
  tools/rdf_sim.cpp src/rdf_engine.cpp`): Monte-Carlo drives (2.5 deg noise,
  +-5 deg mounting offset, 25 % multipath stretches with a stronger
  reflection, lagging GPS course, 0.4 s frame latency) vs a re-implementation
  of the Android app's choices (dB-averaged 20 m windows, 3 deg blur, alpha
  0.25, cap 25, nearest bin, best-offset sweep -6..6, latest fix). Env knobs
  SIM_CLEAN, SIM_MP, SIM_ALPHA, SIM_ESS, SIM_GATE_MAX, SIM_RANGE0, SIM_NOMARG,
  SIM_MAP. Result: equal or better accuracy, a 95 % region that holds the
  transmitter (40/40 vs the Android-style 28/40 on 5-minute drives), the
  mounting offset measured to ~0.5 deg (see the report in the commit / chat)
- End-to-end test (scratchpad h/t_rdf.py + e2e5/fake_drive.py: fake gpsd on
  12947 via `KRAKEN_GPSD_PORT` + a fake heimdall streaming a 5-element UCA
  tone from a hidden transmitter along a driven loop): lobe within 0.2 deg
  of the true bearing, estimate within ~10 m after 26 bearings, VFO select,
  Reset

**DoA per talker (P25 / DMR / NXDN unit IDs, D-STAR callsigns, ADS-B aircraft; `src/talker_doa.cpp`, `plugins/{p25,dmr,nxdn,dstar,adsb}`, 🗺 Map 📡 DF panel):**
- Goal: a VFO on a P25 / DMR / NXDN / D-STAR channel carries many radios; the VFO's DoA
  mixes them. The decoder names who transmits when, the VFO's signal is cut
  at those boundaries and each radio's own samples go through MUSIC - one
  bearing (+ history + mobile DF heat map) per unit ID, picked on the map
- Plugin API (kraken_plugin.hpp): `kp::Talker {id, label, start_s, end_s,
  channel}`, `Host::talker(t)` (again per frame: end_s grows; a new id ends
  the previous one ON ITS CHANNEL where it starts) / `talker_end(at_s,
  channel)`, `Info::talkers`
  (`--info` "talkers", `PluginInfo::talkers`). Times are `Host::time()` =
  plugin input samples / rate since the process started (WireHost: during
  process() it is the block's FIRST sample). Wire TALKER 30 (id, label,
  start, end as "%.4f" text [, channel - omitted for 0]) / TALKER_END 31
  (at [, channel]). The offline test host cuts spans like the live host and
  prints them ("talkers: N transmissions", "ch N")
- Channels (DMR timeslots): `Runner::talkers` holds one open talker per
  channel; `TalkerSpan::channel`. TalkerDoa commits a frame inside a
  channel != 0 span only `CHANNEL_HOLD_MS` (1.5 s) after MUSIC computed it -
  the other slot's radio may be reported later with a start backdated over
  it. Id "?" (`tdoa::unnamed_talker`) = a busy channel whose radio isn't
  known yet: its span blocks frames like any other (two ids -> dropped) but
  creates no talker; frames only inside it wait. When the plugin learns the
  id it reports it from the same start, so the host closes "?" at zero
  length. Lib plugins: `dig::Bridge::talkers(rate)` sets RxContext::talker /
  talker_end (receiver indexes + channel -> Host::time()), `restart_clock()`
  in reset()
- DMR (`DmrProto`, channel = slot 0 MS / direct without slot, 1, 2): burst
  `burst_a_ = sync_end - 77.5 sps`, 132 symbols. A transmission = confirmed
  bursts (voice LC header, voice B..F with a valid EMB, terminator) < 0.5 s
  apart, talker = full / embedded LC source (FLCO 0 "TG n", 3 "unit call to
  n"; label + talker alias, " · slot N", " · via repeater" for BS-sourced);
  ended by the terminator or `tick()`. A busy slot 1/2 without an ID yet is
  reported as "?". BS bursts whose slot the CACH didn't give are skipped.
  Fixed with it: bursts extrapolated from a sync (sync NONE) took the slot
  from the CACH even on MS-sourced / direct signals, which have none - a
  random slot, so B..F (and the embedded LC) never decoded there; they now
  take the kind + slot of the burst they follow (`ext_bs_`, `ext_slot_`)
- NXDN (`NxdnReceiver`): frame `frame_a_ = sync_end - 9.5 sps`, 192
  symbols. A transmission = valid frames with voice (LICH option != 0, not
  UDCH) or a VCALL, < 0.5 s apart; talker = VCALL source (label "TG n" /
  "unit call to n", " · via repeater" when the LICH says outbound); TX_REL
  ends it (its frames included); `tick()` from process()
- D-STAR (`DstarReceiver`, no FEC on voice frames): sync events in sample
  order from correlate(): a data sync in the 420 ms cadence (<= 2 missed)
  confirms everything up to it; the first sync of a transmission exactly
  756 bits after a header frame sync starts it at that header (else late
  entry from the sync); the end pattern confirms up to its start and ends
  it; 2 missed syncs end it. Talker = MY callsign (padding squeezed: "F1ZIL
  B"), label "/sfx to UR via RPT1", from the RF header with a good CRC
  (matched by its sync position - the CRC job runs after the first data
  sync may have been seen) or, on a late entry, the slow-data header copy
- P25 (`P25Proto`): frame boundaries from the sync - frame dibit 0 is centred
  23 symbols before `sync_end`, so `frame_a_ = sync_end - 23.5 sps`, length
  HDU 396 / LDU 864 / TDU 72 / TDULC 216 dibits. A transmission = valid
  HDU / LDU1 / LDU2 frames < 0.75 s apart (`gap()`); its talker = the LDU1 /
  TDULC link control source (LCO 0x00 "TG n", 0x03 "unit call to n";
  source 0 ignored). Reported on every valid voice frame (start = the
  transmission's first frame, end = this frame's end), ended at the TDU /
  TDULC start, at a new HDU, or by `tick()` (called after each block) when
  frames stopped for gap(); a source change without a terminator ends the
  old one at the end of its last LC frame. Positions are receiver samples
  (48 kHz since its reset); decoder.cpp maps them with `t0_` = host.time()
  at the reset (RxContext::talker / talker_end, dig_common.hpp). On
  iq_143.0.u8 (-787.5 kHz): 2010621 0.134-0.314 s, 3250 0.912-2.255 s,
  2010621 6.813- s - matching the decoded frames
- Sample clock: `DecimatorInstance::stream_pos` counts the VFO's decimated
  samples; run_pipeline stamps each block (`MultiChannelDecimated::
  stream_pos`, fetch_add of min_samples) and passes it to MUSIC and to
  `DigitalDecoder::push(.., pos)` (NO_POS = count locally, offline tests).
  Each `Runner` keeps `pos_map` (plugin sample p0 + i = VFO position v0 + i
  * in_rate / plugin_rate, per SENT chunk - dropped chunks never reach the
  plugin's clock; 60 s kept) and turns TALKER times into VFO positions ->
  `dig::TalkerSpan {plugin, id, label, start, end, closed, rate}` ->
  `DigitalDecoder::set_talker_handler` (set by `DecimatorManager::
  ensureDigital`, holds the TalkerDoa, never the instance). Open talkers are
  closed by the host on RESET (retune / mode change), process restart /
  crash / rebuild and stop
- MUSIC: `acc_pos_` maps accumulator samples to stream positions, so
  extractSnapshotsOptimized knows each frame's [a, b); the frame's OWN
  trace-normalised covariance (before the temporal averaging, which would
  mix in earlier frames) goes to `setFrameTap` for every computed frame -
  published or not (eigen gate / publish hold), but none while MUSIC is
  skipped (FFT squelch closed, calibration, retune hold).
  `spectrumFromCovariance(R)` = MUSIC on a given R with the live steering
  vectors (2D max-projection for 3D custom arrays), source count (auto: the
  estimate without hysteresis), half-plane and array offset, own
  eigensolver - the processor's results untouched
- `tdoa::TalkerDoa` (one per VFO, `DecimatorInstance::talker_doa`, active
  while its decoder is on - `setDigitalMode`; off = forget all): frames wait
  (<= FRAME_WAIT_S 6 s - the unit ID comes ~0.5 s + queue after the call
  start) until a span claims them: a frame lies GUARD_S (40 ms) inside one
  talker's span and touches no other talker's span -> its covariance is
  added (weighted by samples) to the talker's CURRENT transmission (one
  span = one transmission; a new one pushes the previous bearing into
  `hist`, max 20) and MUSIC is re-run on the sum (dirty ones each update,
  all every 2 s so array setting changes follow); the frame alone is also
  run through MUSIC and returned as a `TalkerFrame` (a mobile DF record).
  Frames straddling a boundary / two radios / gaps are dropped. A MUSIC
  frequency move > 10 kHz resets it. Talkers kept 30 min after last heard,
  max 100. Lock order: the tap runs under the MUSIC lock and takes ours;
  update() never calls MUSIC while holding ours
- rdf_mapper: the sampler (10 Hz) calls `update()` + `snapshot()` per VFO
  with DoA (coherent + DoA on) and feeds each TalkerFrame into the key
  {vfo, tid}'s own gate -> its own solver (created with its first record or
  a saved session; at most MAX_TALKER_MAPS 32 in memory - the stalest is
  parked, ~3 MB of grids each). The status adds `talkers:[{vfo, tid, label,
  active, tx, tframes, tx_frames, age, tx_age, doa (array frame, as the
  DoA plot), lobe / peak (the latest transmission turned to north with the
  heading at its last frame, cached once the 20 s track no longer reaches
  it), conf, held, frames, gate_m, moved_m, records, hist:[[age, compass |
  null, doa, conf, frames]]}]`; `rdf_grid` has `tid`. `RDF_RESET:<vfo>`
  resets the VFO and its talkers, `RDF_RESET:<vfo>:<tid>` one talker
- UI ("Mobile DF" block): `#map-df-tid` (shown when the selected VFO(s)
  have talkers): Whole signal ('' - the VFO as before) / Every radio ('*':
  every talker's latest lobe + bearing labelled with its ID, own colours
  from the ID hash, dashed + faint when not on air, estimates) / one radio
  ('vfo|tid': its lobe, earlier transmissions as faint bearing lines, its
  heat map / lines / estimate, status line). `#map-df-tl` lists the
  talkers (on air ●, ID, label, bearing - compass, or "arr" = the DoA
  plot's angle when there is no heading -, last heard, transmissions);
  clicking a row selects it (again = Whole signal). `mapState.rdfTid`
  (localStorage); `rdfGrids` keyed "vfo|tid"
- Packets (ADS-B per aircraft; `kp::Talker::packet`, wire flags field "p",
  times sent with 7 decimals): the plugin reports every accepted message
  of a confirmed aircraft (`talker_packet` in plugins/adsb: start = the
  first preamble pulse - mag_ index + timing phase / 5, mapped through
  `abs0_` / `blk_abs_` to host.time() -, end = + (8 + bits) us; offline
  check on a synthetic feed: start within 0.5 sample of the truth, sd
  0.16). Engine: packet -> one closed TalkerSpan {packet} (vfo_pos rounds;
  <= 50 ms). TalkerDoa: while packets come (`want_blocks_`, off after
  PACKET_IDLE_MS 30 s without, ring freed) the pipeline calls
  `add_block()` after processDecimatedIQ (same gate: no calibration /
  retune hold / closed FFT squelch) -> per CHUNK (16) stream samples the
  upper-triangle covariance into a ring of RING_S 1.5 s (5 ch at 2.4 MHz:
  ~27 MB, ~8 % of a Pi 5 core); a packet = sum of the chunks entirely
  inside it (>= 2), trace-normalised; waits PACKET_HOLD_MS 200 for an
  overlapping packet of another talker (both dropped). Per aircraft
  R = sum of packets weighted exp(-dt / PACKET_TAU_S 1.5 s) in stream
  time, MUSIC at most every PACKET_RECOMPUTE_MS 1 s, `hist` every 10 s
  (its bearing track), tx = packets, tx_frames = packets in the average;
  forgotten after PACKET_TTL_MS 2 min; MAX_TALKERS 200. No mobile DF
  records. rdf_mapper: `pk:true`, lobe turned with the heading at
  `spec_ms`, no lobe 60 s after its last packet. UI: ✈ rows, "Every
  aircraft", Δ pos column + selected-aircraft line = DF bearing minus the
  bearing to its ADS-B map point (needs the decoder's Plot on map;
  `rdfPosCheck`), "ADS-B check" = median / quartiles of that over the
  aircraft heard < 30 s (`rdfPosSummary`) - a steady offset = array
  mounting / heading error
- Channel filtering (AIS): `kp::Talker::freq_hz` / `bw_hz` (packet's
  centre in the plugin's band - the engine maps it to the VFO's: + afc_hz_,
  mirrored with invert), `avg_s` (averaging; also TTL = max(2 min, 30
  avg_s) and the lobe kept max(60 s, 15 avg_s)). Wire TALKER fields 7-9.
  TalkerDoa keeps the raw samples while the VFO rate <= RAW_MAX_RATE (500
  kHz; 5 antennas at 100 kHz ~ 6 MB for RING_S) and `packet_cov_raw`
  mixes each antenna by -freq, Hamming-windowed-sinc low-pass (cut-off
  bw / 2, ~3 fs / bw taps, run-in from the samples before the packet),
  covariance over exactly the packet; overlapping packets are only dropped
  when on the same channel. Above 500 kHz (ADS-B) the chunk ring as before
  (freq ignored). PACKET_IDLE_MS 5 min; MAX_PACKET_S 0.25 (5 AIS slots).
  Status `pl` = the talker's plugin (UI: ✈ / ⛴, "Every aircraft / ship",
  "Position check")
- Not split: TETRA (the plugin decodes the downlink - every bearing is the
  base station's), MPT1327 (analogue voice). On a repeater OUTPUT every ID's
  bearing is the repeater's. A DMR repeater INPUT with both slots busy
  can't be split (MS bursts carry no slot) - the two radios' LCs land on
  slot 0, so their frames mostly touch both and are dropped
- Offline (plugin `--file`, recordings from dsdcc / szechyjs dsd-samples
  FM-modulated to cf32): dstar_f1zil_1 full (158 s): F1NSR 1.584-149.692 s,
  F1ZIL B (the repeater's reply) 149.982-150.543, F5LKW 154.553-; DMR MS
  dmr_1_48000: 3 calls of ID 1 matching the LC header / terminator bursts;
  dmr_it_8 (BS slot 2): "?" 0.257 s collapsed, 2222223 0.257-19.964 ch 2
- Test 3 (scratchpad e2e3/: gen_air.py + t_air.py + cdp3.py): six
  synthetic aircraft (DF17 ident / position / velocity + DF11, random
  sample timing, some colliding) at compass 20..320 deg, 25-80 km, steered
  from the bearing of their encoded position onto a 5-element UCA (120 mm,
  1090 MHz, static station heading 0), 2.4 MHz VFO: every aircraft 0.0 deg
  off its true bearing (~9 packets averaged), the VFO bearing jumping;
  kraken_doa ~85-90 % of a core total. UI from captured messages: list,
  Every aircraft, one aircraft, ADS-B check 0.0 / +3.0 deg (bearings
  turned 3 deg)
- Test 2 (scratchpad e2e2/: gen3.py + t3.py, three VFOs on one fake feed):
  D-STAR F1NSR 60 / F1ZIL B 200 / F5LKW 120 deg, DMR BS slot 2 2222223 60,
  DMR MS ID 1 200 - every transmission exact, VFO bearings jumping
- Test (scratchpad e2e/: gen_feed.py + fake_heimdall5.py + t_talk.py):
  iq_143.0.u8's P25 channel (two radios) steered per talker onto a 5-element
  UCA (2010621 -> 60 deg, 3250 -> 200 deg, gaps 300, rest of the band 0,
  independent noise), streamed by a fake heimdall into the real kraken_doa
  (scratch cwd, VFO -787.5 kHz 24 kHz P25, RADIUS 500): per-talker DoA 59.9
  / 200.0 deg every transmission, while the VFO's own bearing jumps between
  52 / 160 / 300. UI checked from file:// with a stub WebSocket replaying the
  captured messages (headless Chromium can't reach servers in the agent
  sandbox; map tiles failed fast = offline mode)

**Decoder data log (`src/decoder_log.cpp`, sidebar "🗂 Decoder Logging"):**
- Sources: the engine's record handler (`dig::set_record_handler`,
  `LogRecord{type, plugin, text, from, point, rf_hz}`, called on each
  decoder's worker thread): "event" (a Report event that passed the
  de-duplication), "message" (`kp::Host::message`), "position" (a map
  point - logged at most once per vfo|plugin|id every `pos_s`, default
  10 s; regardless of "Plot on map"), "raw" (`kp::Host::raw`, wire RAW 29);
  plus "incident" from incidents.cpp (`declog::record_incident`, new
  incidents only). The VFO id comes from `DigitalDecoder::vfo()` (set by
  DecimatorManager when it creates the decoder - ids never change). NOT via
  `decimator_manager` from the handler: it runs on the decoder's worker
  thread, and holding the last reference to the VFO instance there would
  destroy the decoder on its own thread (self-join)
- Raw frames cost CPU, so plugins only build them while asked:
  `dig::set_raw_wanted(enabled && raw ticked)` -> each Runner sends OPTION
  `log_raw=0|1` (handled in plugin_host.cpp, not passed to the decoder) ->
  `kp::Host::raw_wanted()`. ADS-B: Mode S hex + level dB; APRS: TNC2;
  POCSAG: "RIC n Ff baud" + message codewords' 20 data bits
- File: `<dir>/decoders-YYYY-MM-DD.jsonl` (LOCAL date), one JSON object per
  line: `t` (local ISO 8601 with ms + UTC offset), `vfo`, `mhz` (VFO RF; not
  on incidents), `dec` (plugin id), `type`, then per type: text / from+text
  / id, label, kind, lat, lon, alt_m, kmh, hdg (finite only) + `info` (the
  map popup's "Key: value" lines as an object - ADS-B: squawk, vertical
  rate, IAS / TAS, category, emergency, signal) / data / address, lat, lon,
  precision, confidence, text
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
  `DECODER_LOG_TYPES:event,message,position,raw,incident` (any subset),
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

**Fixed-frequency decoders (`kp::Info::fixed_freq_hz`, ADS-B 1090 MHz):**
- Done in the web UI (kraken_doa.html, `digApplyFixedTune`, from
  `digSetModeFor` / `digToggle` - i.e. when a user picks the decoder, not on
  a settings replay): narrowest bandwidth >= `min_vfo_rate`
  (`setDecimatorBandwidthFromUI`), `SET_DECIMATOR_FREQ:id:0` (a queued drag
  dropped first), then the tuner through the normal paths - `paneSendFreq`
  (TUNER_FREQ, the VFO's tuner) in independent mode, `sendFreqChange`
  (FREQ / WIDEBAND_FREQ) otherwise
- `vfoLocked(d)` (the VFO's decoder is such a plugin): `drawLockedVfoLine`
  draws one solid line + "D<n> <plugin name>" in drawBox, the waterfall
  overlay (`drawLockedLines`) and the panes; the VFO gets NO entry in
  `fo.boxes` / `waterfall.overlay.boxes` / `pane.bars`, so it can't be
  grabbed, resized or squelch-dragged; `centerActiveBarAt` and the pane
  click don't move it when selected; `digRenderCards` disables its Freq / BW
  / Tuner fields. Picking another decoder unlocks it (bandwidth stays)
- Test: scratchpad t_adsb_lock (coherent + independent)

**AIS (`plugins/ais/`, ships):**
- Input 100 kHz (`sample_rate` = `min_vfo_rate` = 100 kHz = the VFO's "100
  kHz" bandwidth, 2.4 MHz / 24: no resampling), `fixed_freq_hz` 162.000
  MHz, manual only: one VFO holds channel A (161.975, -25 kHz) and B
  (162.025, +25 kHz). Per channel: mix, 49-tap 8 kHz low-pass decimated to
  50 kHz, FM discriminator (clamped +-12 kHz), 21-tap 5.5 kHz low-pass ->
  2 x 8 slicers (8 bit-timing phases at 5.21 samples/bit, linear
  interpolation; threshold = the slicer's running mean over ~64 bits, or
  the discriminator's mean over 48 bits centred on the bit - running sum
  `cs` - which follows scanner-audio baseline wander) -> NRZI -> HDLC
  (flag, destuffing, 7 ones = abort) -> CRC-16/X.25 -> payload bits (bytes
  LSB first on air). A frame decoded by several slicers (same channel +
  CRC, closing flag within 4 bits) is reported once
- Messages (`ais_msg.hpp`): 1-3 (class A position, status, ROT, SOG, COG,
  heading), 4 / 11 (base station), 5 (IMO, callsign, name, type, size,
  draught, ETA, destination), 9 (SAR aircraft), 18 / 19 (class B), 21 (aid
  to navigation), 24 A/B (class B static), 27 (long range). MMSI 970 / 972
  / 974 (SART / MOB / EPIRB) and status 14 -> "⚠" events (every 10 min).
  Outputs: map points (kind ship / station / point / aircraft / person,
  heading = true heading else COG, ttl 30 min), table (MMSI, name,
  callsign, type, class, status, SOG, COG, HDG, position, distance /
  bearing, destination, length, channel, signal, messages), facts, raw =
  NMEA !AIVDM (multi-sentence over 60 characters) + level. Vessels
  forgotten after 30 min. Option `range` (km, default 500)
- DoA: every frame -> `kp::Talker{packet, freq_hz = -25000 / +25000, bw_hz
  16000, avg_s 20}`, span = training sequence (24 bits before the opening
  flag) .. closing flag, through the channel's filter delays (`to_time`:
  input index = 2 (t - 10.5) - 24); the spans land within 0.3 bit of the
  truth on synthetic signals
- Tested (scratchpad e2e4/): synthetic GMSK (ais_gen.py: real layouts,
  NRZI, stuffing, BT 0.4, ships on both channels incl. simultaneous A / B
  transmissions) - every frame without a same-channel collision decoded,
  none from noise; real recordings (cu8 96 kHz two-channel ship162
  ais_96k.bin, sigidwiki 48 kHz IQ, AIS-catcher example, freerange
  rtl_fm discriminator files with aisdecoder NMEA, Long Beach / Helsinki
  scanner audio): all aisdecoder messages + more, at least the numpy
  probe's count on IQ (62 vs 70 on the 8-bit Long Beach audio)

**ADS-B (`plugins/adsb/`):**
- 2.4 MHz input (`min_vfo_rate` 2 MHz; picking the decoder sets the 2.4 MHz
  "No decimation" bandwidth and tunes to 1090 MHz - above). Magnitude -> chips integrated
  at 5 sub-sample timings (box-filter weights, 0.5 us = 1.2 samples - the
  same slicer as dump1090's 2.4 MHz demodulator) -> amplitude gate on
  preamble pulses 1-3 vs the gap (noise floor = lowest 512-sample mean,
  smoothed) -> preamble score at the 5 timings (pulses >= 2x gaps, no gap
  as strong as a pulse) -> the best 2 sliced -> CRC-24. DF17/18: 1-bit
  repair (syndrome table) or 2 bits among the 8 least certain (CRC linear:
  XOR of per-bit syndromes) - repaired / DF11 with an interrogator id /
  address-parity replies (DF0/4/5/16/20/21, syndrome = address) only for
  aircraft already heard cleanly; an aircraft counts (events, map,
  valid()) from its 2nd message
- `modes.cpp`: CRC, AC12 / AC13 altitude (25 ft and Gillham), squawk,
  callsign, surface speed, CPR global (even/odd within 10 s airborne,
  25 s surface - surface needs a reference) and local decoding (against
  the aircraft's position < 60 s old), NL(). Position checks: within the
  `range` option of the station (default 500 km) and <= ~Mach 2.5 from the
  last position (4 consecutive failing global fixes replace it)
- Unusual activity -> events starting with "⚠" (once per occurrence, with
  position + altitude, so the decoder data log keeps them): emergency
  (7500 / 7600 / 7700 or TC28 emergency state) and its end, a CHANGED
  squawk, special squawks 7400 (lost link UAV) / 7777 (US interceptor) /
  0000, categories A6 high performance / B2 lighter than air / B3
  parachutist / B4 ultralight / B6 UAV / B7 space vehicle, |vertical rate|
  >= 6000 ft/min (`VRATE_ALERT_FPM`, re-armed below 2/3 of it), >= 400 kt
  below 10000 ft (`FAST_LOW_KT/FT`, ES altitude + velocity < 30 s old).
  Plus "IDENT (SPI)" (no ⚠; ES surveillance status 3, or 2 replies with
  flight status 4/5 within 10 s; at most every 2 min). A squawk from a
  DF5/21 reply (parity-checked only - one bit error is another code) is
  only taken when the next reply within 30 s repeats it; TC28's at once.
  Test: scratchpad adsb/gen_unusual.py (one aircraft per case + an
  ordinary one that must stay silent + a corrupted reply)
- Output: the aircraft table (`TABLE_COLS`: every decoded field, a row per
  aircraft updated at most 1/s, key = the map point id), summary facts,
  events (new aircraft, callsign, first position, emergencies 7500/7600/7700
  or the TC28 emergency state), map points (kind aircraft / vehicle for
  C1-C2, heading = track, ttl 60 s), every message in verbose. Aircraft
  forgotten 60 s after the last message; `reset()` drops them all
- Validated on synthetic recordings (`--station` for the offline test):
  the published "1090 MHz Riddle" messages decode to their documented
  values (KLM1023; 40621D at 52.2572, 3.9194, 38000 ft; 485020 159 kt /
  183 deg), 100% of messages at >= 15 dB SNR, ~67% at 12 dB, none from 20 s
  of noise. CPU (Pi 5): plugin ~5% of a core, kraken_doa +~10% for
  the 19 MB/s of samples it forwards. Not yet tried on live 1090 MHz

**Digital voice (`plugins/lib/dig_vocoder.cpp`, Demod "Digital"):**
- `DemodulatorMode::DIGITAL`: the FM thread still runs the FM demodulator
  (as NBFM - `setDemodulatorMode` maps it) only because its output length
  clocks the audio stream; for a DIGITAL Audio Src VFO the samples are then
  replaced by `DigitalDecoder::pull_voice()` (silence when there is none) and
  squelch is skipped. `pull_voice` also marks the voice "wanted" for 0.5 s -
  the lead plugin gets VOICE_WANTED and runs its vocoder only then.
  `SET_DECIMATOR_DEMOD:id:DIGITAL` switches the VFO's decoder to AUTO if off
- Voice FIFO (digital_decoder.cpp): 8 kHz plugin audio (AUDIO messages) ->
  slow AGC (tanh limited) -> 6x interpolation -> 48 kHz FIFO; playback starts
  at 250 ms buffered (frames arrive in bursts, a P25 LDU = 180 ms) or once
  the input stops for 300 ms (a call's tail), re-prefills after an underrun
- P25: `ImbeDecoder`, a port of mbelib's IMBE 7200x4400 (ISC; tables in
  `mbe_tables.hpp` with the notice) - Golay/Hamming + PN, parameter decoding,
  mbelib's synthesis with a per-instance RNG (mbelib uses rand()). Verified
  against libmbe on the same frames: correlation 0.9987 with the same random
  sequence (`-DDIG_IMBE_TEST_RAND` switches the RNG for that test). The 9
  frames of an LDU sit at status-free dibit offsets 56,128,220,...,768 and
  are deinterleaved with DSD's iW..iZ (ISC). Muted when the HDU/LDU2 ALGID
  isn't 0x80 or the LC "protected" bit is set
- DMR / D-STAR / NXDN: `MbeLib` dlopens `$KRAKEN_MBELIB` / libmbe.so.1 at first use
  (`KRAKEN_MBELIB=none` disables it) - inside the plugin process
  (no AMBE code shipped or linked); `AmbeStream` keeps a per-stream state in
  3 x 4 KB blocks (mbe_parms is ~1.2 KB). DMR: 3 AMBE+2 frames per voice
  burst (frame 2 straddles the sync/EMB), rW..rZ tables, one slot played at a
  time (option slot, else the first to talk until quiet for 1 s), muted on
  the EMB/LC privacy bit; a burst with no clean frame is not played (false
  syncs). D-STAR: 21 AMBE frames per superframe (frame 0 is before the data
  sync), dW/dX tables, stops at the end pattern
- TETRA: `TetraCodec` runs the ETSI programs as `cdecoder /dev/stdin
  /dev/stdout | sdecoder ...` (posix_spawn, non-blocking pipes, 690-word
  test-frame blocks of descrambled type-4 soft bits). Full-slot traffic
  (AACH usage marker >= 4, training sequence 1) of one timeslot; after 400 ms
  without traffic, erasure blocks flush the pipes' stdio buffers. Muted for
  60 s after any encrypted MAC-RESOURCE. ~0.5 s extra latency (pipe buffers)
- Codec availability: the sidebar box (above), and each voice plugin sets a "Voice codec" fact (e.g.
  "AMBE+2: libmbe.so.1 (mbelib 1.3.0)" / "not installed"), and its voice
  state says "... codec not installed" while someone listens
- Docker: the entrypoint exports `KRAKEN_MBELIB` / `KRAKEN_TETRA_CODEC_DIR`
  when `/data/codecs` holds libmbe.so.1 / cdecoder + sdecoder (inherited by
  the plugin processes)
- Tested: offline (regression incl. voice), through the real Opus audio
  stream with a fake heimdall replaying recordings of all four, and live:
  DMR Tier III traffic-channel speech here; P25 calls here are AES-256 and
  are correctly muted (all before the move into plugins; the regression
  re-verified it after)

**Decoder plugins (`plugins/`, `src/digital/dig_plugin.cpp`):**
- A plugin is a folder `plugins/<id>/` (`[a-z0-9_-]{1,32}`, not `sdk` /
  `lib`): decoder.cpp with `KRAKEN_PLUGIN(Class, {.id, .name, .description,
  .version, .sample_rate, .min_vfo_rate, .author, .auto_detect, .options})`
  (fields in that order; `.auto_detect` is ignored), optional extra .cpp/.hpp, README.md. API
  `plugins/sdk/kraken_plugin.hpp` (`kp::Decoder::process(const kp::cf*, n)`
  gets the VFO's complex baseband at `sample_rate`; `kp::Host`: fact / event /
  valid / audio (8 kHz) / voice_state / freq_error / verbose / voice_wanted /
  time / log, map_point / table_* / message / raw (data log) / talker +
  talker_end (DoA per radio, see *DoA per talker*); `kp::Option` key/label/default/choices "v=Label|..."/help),
  helpers `plugins/sdk/kraken_dsp.hpp`, the shared library `plugins/lib/`,
  guide `plugins/SDK.md` (written for LLMs too)
- `plugins/Makefile` builds `lib/build/libkrakendig.a`, then links each
  plugin's sources + `sdk/plugin_host.cpp` + the library into its OWN
  executable `plugins/<id>/build/decoder` (linked as decoder.tmp, then
  renamed: a running copy keeps its inode). The top Makefile's `plugins`
  target runs it with -k and only warns on failure; CMake has an ALL target
- `decoder --info` (JSON incl. `options` and `options_tsv` - the
  tab-separated copy kraken_doa's flat JSON reader parses; read by
  `PluginRegistry::scan()` at startup, on `PLUGINS_RESCAN` (worker thread,
  `scan_async`) and after builds; `generation()` bumps when the list
  changes), `--serve` (live), `--file REC [--offset HZ] [--opt k=v]
  [--verbose] [--audio out.wav]` (offline test: same resampling / 10 ms
  blocks as live, cf32 / cu8 / cs16 / 16+24-bit IQ WAV, prints events +
  "VALID FRAMES: N")
- Live (`Runner` in digital_decoder.cpp): the process is started on the
  decoder thread (`PluginProcess`, posix_spawn, non-blocking pipes), SAMPLES
  in 16k chunks (dropped, counted, when > 1 s behind; offline: blocking
  `flush_blocking`), replies polled after every block. Protocol: 8-byte
  header (type, len) + payload, types in kraken_plugin.hpp `kp::wire`
  (SYNC / SYNC_DONE drain the offline tests). The plugin's fd 1 is pointed
  at stderr (protocol on a dup), so a stray printf can't corrupt it; its
  stderr tail (30 lines) is in the status JSON (`plugins[].log`) and the
  panel's "Plugin output". Stopping a decoder closes every plugin's stdin
  first, then reaps them (parallel exit - AUTO has 6)
- Crash / malformed message -> event with the signal and last stderr line,
  restart after 2, 4 .. 30 s; missing / unbuilt plugin -> retried every 5 s
  (a snapshot may name a plugin this receiver lacks: accepted on replay,
  refused interactively). The exe's mtime is checked every 2 s: a rebuild
  restarts the decoder ("Rebuilt plugin loaded")

**AI Signal Lab (`src/ai_manager.cpp`, `ai/`):**
- Sidebar box "🤖 AI Signal Lab" + the "🤖 AI" tab of the panel under the
  waterfall. `AiManager` runs ONE job at a time: `python3 ai/kraken_ai.py
  investigate|create|ask|test`, posix_spawn in its own process group (Stop = killpg TERM, KILL
  after 3 s); a reader THREAD reads its output - the uWS loop only starts /
  stops jobs. The bridge prints one JSON event per line
  (`{"ev":"status|text|tool|tool_result|report|usage|error|done|exit",...}`,
  checked by a strict JSON validator before being relayed); AiManager relays
  each as `{"ai_event":{seq,kind,t,e}}`, keeps the last 300 (replayed in
  `ai_state.log`) and tracks the current session from the `done` events.
  After create: registry rescan (reader thread) + `{"plugins"}`
- Gate: `ai/ai_config.json` `"enabled": true`, written only by `python3
  ai/kraken_ai.py setup` on the Pi (checks the CLI login, asks for
  confirmation). Read on every command, so no restart is needed;
  `KRAKEN_AI_CONFIG` overrides the path (both sides)
- Sessions = saved investigations (`ai/sessions/<YYYYmmdd-HHMMSS>-<kHz>/`):
  capture.cf32/.json, plots, analysis.md, session.json (incl. the Claude
  session id - create / ask `--resume` it), `chat.json` (the conversation:
  [{role user|assistant, kind investigate|ask|create, text, t}], appended by
  the bridge; seeded from session.json for sessions older than it) and
  `activity.jsonl` (every event of the agent, <= 4 MB). `AI_SESSIONS` ->
  `{"ai_sessions":[{id,freq_hz,signal_name,created,state,plugin_id,...}]}`
  (newest first; also sent on AI_STATE and after every job),
  `AI_SESSION_GET:id` -> `{"ai_session":{...,chat,activity}}` (chat.json and
  each activity line embedded only if they pass the JSON validator - the
  agent can edit files in its session folder; fallback: a chat built from
  session.json), `AI_SESSION_SELECT:id` (the current session: Ask / Create
  use it), `AI_SESSION_DELETE:id` (`kraken_ai.py delete`: the folder + the
  Claude transcript ~/.claude/projects/*/<uuid>.jsonl). All file work on
  detached worker threads, results broadcast
- UI: "AI" badges at the top of the spectrum above every session's
  frequency (`aiDrawMarkers` at the end of `drawBox()`, groups within 2 kHz,
  clicking one opens it - `handleDown` checks `aiMarkerHit` first), the
  History list in the sidebar box, and the bottom panel's AI tab (session
  list, chat bubbles rendered with the escaping Markdown renderer, agent
  activity, follow-up input). The AI's output is shown ONLY in that tab: the
  sidebar box has the controls and a one-line status (`#ai-status`: bridge
  status messages, errors, done + "⤢ Show"). A new investigation has no
  session until its `done` event, so the tab shows it as the job view
  (`aiViewId === AI_JOB`, list entry "⏳ Investigating…" / "Last job": live
  text + activity from `aiLog`, or the error) and switches to the session the
  `done` names; the user's own Investigate / Create opens the tab; a job
  started elsewhere (another browser) moves the AI tab's view to it but
  never switches the panel to the AI tab. Bottom panel: `#info-panel` in the spectrum
  panel under the waterfall controls, resizable (top edge), open state +
  height kept in localStorage; opening/closing fires a window resize so the
  waterfall refits. Tabs (`infoTab` = `dec:<vfo>` | `ai`): one per VFO with a
  decoder on, then AI
- WS commands (all excluded from sync_cmd echo - `is_query_command`): `AI_STATE`
  (-> ai_state with log + plugins + sessions), `AI_TEST`, `AI_CANCEL`,
  `AI_INVESTIGATE:vfo:freq_hz:seconds[:instructions]` (freq 0 = the VFO's,
  must lie in the current span; capture rate = VFO rate clamped 12.5-600 kHz,
  2-60 s), `AI_CREATE:id[:instructions]` (existing plugin = improve it),
  `AI_ASK:question`, the AI_SESSION* commands above, `PLUGINS_LIST`,
  `PLUGINS_RESCAN` (`PLUGIN_AUTO` / `PLUGIN_CODECS`: see the digital
  decoders section). Errors -> `{"ai_error":{cmd,error}}`. There is
  deliberately no web-UI build / export / import: plugins are copied as
  folders and built with `make` by the user
- `ai/kraken_ai.py`: Claude runs as `claude -p --output-format stream-json
  --verbose --permission-mode dontAsk --safe-mode` (no user hooks / MCP /
  CLAUDE.md), cwd = session dir, tools allowed: Read of the session /
  plugins / ai/*.py, Glob, Grep, WebSearch, WebFetch, Edit in the session (+
  `plugins/<id>/` for create), `Bash(python3 ai/sigtool.py:*)`,
  `Bash(python3 <session>/*)` (a `:*` prefix rule only matches whole words -
  the wildcard is needed for paths), and for create `Bash(make -C plugins
  PLUGIN=<id>:*)` + `Bash(plugins/<id>/build/decoder:*)` (absolute paths;
  `//abs` in Read/Edit rules). Compound commands outside these are refused.
  Backend "command" (`setup --backend command --command "..."`) pipes the
  prompt to any CLI, unrestricted. `--capture-file REC.cf32` investigates a
  recording
- `ai/sigtool.py` (numpy only): `capture` (8091 channel 0, exact centre from
  8092 get_status, packets with the noise source on / retuning are cut out,
  then mix + fast-convolution channelizer to 2.4 MHz / integer), `analyze`
  (bursts, in-passband spectrum stats, envelope, x^2/x^4 lines, FM
  discriminator histogram / transition lines / audio tones, PNG plots via a
  built-in rasterizer + 5x7 font), `spectrogram`, `extract`, `demod`,
  `tones`, `symbols` (per-segment best-phase sampling, k-means levels, eye
  ratio, repeated 24/32/48-bit words = sync candidates, frame period)
- Verified: live investigations (162.4 / 454.6 MHz) by the user; investigate
  + create on a synthetic APRS recording produced the `aprs` plugin (5/5
  frames, 0.5% CPU, quiet on noise) in ~5 min; the UI (panel, history,
  markers, options) against a mock server in headless Firefox

**Web Mapper output (built-in, replaces web_mapper_middleware):**
- `src/networking/web_mapper.cpp` streams one legacy "doapost" record per VFO
  to the KrakenSDR web mapper — the record is built from the SAME capture
  helpers as DOA_value.html / the DoA logger (`capture_doa_records()`), so it
  stays byte-compatible with the external contract with map.krakenrf.com and
  the Android app. The old Node.js `web_mapper_middleware/` is deprecated and
  no longer needed (no Node/npm install, nothing extra to run)
- Two modes: `remote` (WSS client to the KrakenPro cloud map; hand-rolled
  TLS WebSocket client over OpenSSL — uWS is server-only) and `local` (plain
  WebSocket broadcast server, default port 8021, for LAN map clients). The
  local-mode HTTP settings endpoint the middleware had (port 8042) is gone:
  config lives in the sidebar now
- Remote mode also REGISTERS the station: the legacy flat settings.json
  schema is pushed on connect/change (1 s hash-guarded check) with a ping
  every 10 s, and settings pushed BACK by the cloud are applied to the
  receiver by dispatching normal control commands on the uWS loop
  (`loop->defer` → `ControlHandler::handle_websocket_message`), so sync,
  persistence and heimdall forwarding behave as if a browser sent them.
  Cloud VFO bandwidths snap to the nearest `BANDWIDTH_OPTIONS` entry;
  `vfo_squelch_N` sets the level AND enables that VFO's squelch (the mapper
  assumes squelch is always on)
- Everything runs on one worker thread started from main(); records are
  captured on a 200 ms tick, gated on `doa_enabled` and
  `doa_is_calibrating()` (noise-source pulses would map garbage bearings).
  Each VFO's own squelch setting is respected (no separate mapper setting):
  squelch off = that VFO's bearings always transmit; squelch on = only
  while its squelch is open
- WS commands (persisted via settings_store, replayed to new browsers):
  `WEB_MAPPER:0|1`, `WEB_MAPPER_MODE:remote|local`, `WEB_MAPPER_KEY:<key>`,
  `WEB_MAPPER_URL:wss://...`, `WEB_MAPPER_WS_PORT:<port>`. The key is
  SECRET: it is persisted but never echoed or replayed to browsers -
  `redact_for_sync()` (control_handler.cpp) turns it into
  `WEB_MAPPER_KEY_SET:1|0`, and the UI field stays empty with a "saved"
  placeholder. Live state rides system_status as
  `web_mapper:{enabled,mode,state,clients,records,error}`; the sidebar
  "🌐 Web Mapper" panel drives it all
- The cloud connection VERIFIES the server certificate (system CA store +
  hostname; map.krakenrf.com serves a valid `*.krakenrf.com` cert). The
  legacy middleware didn't, which let anyone on the network path pose as
  the map server and collect the API key. `KRAKEN_WEB_MAPPER_INSECURE=1`
  (environment) turns verification off for a self-hosted server with a
  self-signed certificate. A cloud settings push can NOT change the server
  URL (`mapping_server_url` is ignored) - that stays a local, web-UI-only
  decision. Reassembled cloud messages are capped at 1 MB
- Station identity/location are NOT web-mapper settings: the callsign and
  the resolved lat/lon/heading come from StationInfo (the "Station
  Information" panel), exactly like DOA_value.html

## Critical Performance Optimizations

### 1. IQ Conversion (MOST IMPORTANT)

**Current Implementation:** Simple scalar loop with compiler auto-vectorization

```cpp
for (size_t i = 0; i < num_samples; i++) {
    output[i] = std::complex<float>(
        input[i*2] / 127.5f - 1.0f,      // I
        input[i*2+1] / 127.5f - 1.0f     // Q
    );
}
```

**Why This Beats Manual NEON:**
- GCC -O3 -march=native auto-vectorizes this pattern
- 8.5% faster than manual NEON intrinsics
- Zero function call overhead
- Predictable memory access for cache optimization

**NEVER replace this with arm_neon.h intrinsics!** See `OPTIMIZATION_SUMMARY.md` for benchmarks.

**DC correction** (`convert_uint8_to_complex_float_tracked_dc`, used by the
data receiver): each channel's DC offset is an EMA of packet means (k = 0.05
per ~7 ms packet, corner ~1 Hz), subtracted from every sample. Subtracting
each packet's OWN mean was a ~150 Hz-wide notch at the centre (it removed a CW
beacon or AM carrier parked there). The seed is a flag, not NaN: `-Ofast`
assumes no NaNs.

### 2. Decimation Architecture

**Thread-Local Decimators:**
- Zero allocation per call (thread_local storage)
- Shared coefficient cache with 95%+ hit rate
- Pre-reserved buffers (no dynamic allocation in hot path)

**Coefficient Caching:**
```cpp
thread_local std::map<...> decimator_cache;  // Per-thread instance
static std::shared_mutex cache_mutex;        // Multi-reader, single-writer
```

**Performance:** ~8000 samples/µs per channel

### 3. FFT Processing

**Per-Channel FFTW Plans:**
- Each channel has dedicated `ChannelFFTContext` (plan, buffers)
- True parallel execution with zero mutex contention
- 5x parallelism on 5-channel system

**FFTW3 Wisdom Files:**
- Pre-planned transforms saved to disk
- 50x faster startup (0.5s → 0.01s)
- Optimized for Raspberry Pi 4 Cortex-A72

**Plan Creation Flags:**
```cpp
fftwf_plan_dft_r2c_1d(size, in, out,
    FFTW_MEASURE | FFTW_DESTROY_INPUT | FFTW_UNALIGNED);
```

### 4. FM Demodulation

**Hardware atan2f:** Uses ARM NEON internally, faster than any approximation
```cpp
float phase = std::atan2f(q, i);  // Hardware accelerated on ARM
```

**Liquid-DSP Resampling:** Multi-stage decimation for 48kHz audio output

**AM loops are set in seconds** (`recreateFilters`, re-run on every rate or
mode change): RF AGC tau 50 ms, envelope AGC tau 0.5 s, DC blocker 30 Hz.
Fixed per-sample constants put their corners at kHz at wide VFOs and stripped
the AM audio's lows.

**FM only (DoA and beamforming off):** only the FM source VFO runs, decimating
only the listened-to channel (squelch reads the main FFT, not decimated data),
and the receiver converts only that channel.

### 5. MUSIC Algorithm

**Eigenvalue Decomposition:** Eigen library with SelfAdjointEigenSolver
- Optimized for Hermitian correlation matrices
- NEON vectorization enabled automatically
- Pre-allocated working matrices

**Snapshot Management:** Circular buffer with power-of-2 size
- Bitmask indexing (no modulo)
- Overlap processing for temporal smoothing

**Performance:** 256 snapshots, 5 channels → 8-12ms total

**Frame size cap:** `snapshot_length x num_snapshots` <= `MAX_FRAME_SAMPLES`
(65536). Every MUSIC buffer is complex<double> and scales with it - the old
2048 x 128 maximum was ~200 MB per VFO. `setConfig` lowers num_snapshots to
fit; the MUSIC_SNAPSHOT_LENGTH handler echoes the lowered count
(MUSIC_NUM_SNAPSHOTS) so browsers and the settings file follow.

**Retune / calibration gate:** DoA processing is skipped while heimdall
calibrates: the per-packet noise flag plus `NOISE_QUIET_MS` (250 ms - the
decimators' filter memory, <= ~25 ms) and, after a retune, a hold of at least
`RETUNE_DOA_MIN_HOLD_MS` (500 ms) that lasts while heimdall reports
WAITING_FOR_STABILITY (its post-retune cooldown: compensation reset, noise
off). Was a fixed 4 s hold + 750 ms. First new bearing ~4 s after a retune
with heimdall's 3 s cooldown (~1.6 s with 500 ms).

**Calibration bursts in the UI:** the receiver pushes `{"cal_live":{phase_state,
noise_source}}` the moment a packet's phase state or noise flag changes (the
500 ms system_status was too slow for a ~0.4 s retune burst) and restarts the
spectrum EMA on every noise switch. The page's calibration badge follows
cal_live; the waterfall marks noise-on rows with an orange strip in the left
margin, and auto-range holds through a burst (+1 s) - ranging a noise frame
(no antenna signals) collapsed the colour scale, which turned the waterfall red
for up to 500 ms past the burst, over rows that already showed the antennas.

**Input continuity:** the accumulator is cleared on a gap > 200 ms, a sample
rate change or a VFO offset change, and the data receiver retunes MUSIC from
the offset each block was decimated with (not the VFO's live one), so a frame
never mixes samples mixed down at two offsets.

### 6. Memory Management

**Thread-Local Storage:**
```cpp
thread_local std::vector<std::complex<float>> buffer;
buffer.reserve(expected_size);  // Pre-allocate once
```

**Move Semantics:**
```cpp
queue.push(std::move(data));  // No copy, transfer ownership
```

**Malloc Arenas:**
```cpp
mallopt(M_ARENA_MAX, 32);  // In main.cpp for 23-28 threads
```

## Common Development Tasks

### Adding New Signal Processing Features

1. **Location:** Add to `src/signal_processing/`
2. **Header:** Create corresponding header in `include/signal_processing/`
3. **Integration:** Update `data_receiver.cpp` to call your processor
4. **UI Control:** Add WebSocket command in `control_handler.cpp`
5. **Message:** Add JSON builder in `message_builders.cpp`
6. **Rebuild:** `make rebuild`

### Modifying Bandwidth Options

1. **Config:** Edit `BANDWIDTH_OPTIONS[]` in `include/config.hpp`
2. **No rebuild needed** for option changes (loaded at runtime)
3. **Constraints:** Only integer decimation factors (no resampling)
4. **Range:** Factor must divide `SAMPLE_RATE` evenly

### Updating Web UI

1. **File:** Edit `kraken_doa.html`
2. **No rebuild needed** (loaded at runtime by `websocket_server.cpp`)
3. **Protocol:** Match message format in `message_builders.cpp`
4. **Testing:** Refresh browser (hard reload: Ctrl+Shift+R)

**Sidebar width (desktop):** the handle on the sidebar's right edge
(`#sidebar-resizer`, `initSidebarResize()`) drags it between 230 px and
min(720 px, 60% of the window); double-click = default. The sidebar keeps
its 290 px layout and is scaled with CSS `zoom` (`--sb-zoom` = width / 290),
so text, buttons and inputs grow with it. Width kept in localStorage
(`kraken_sidebar_w`), re-clamped on window resizes, then a window `resize`
refits the spectrum / waterfall. Mobile drawer: zoom 1, no handle. Anything
inside the sidebar positioned with `position:fixed` from
`getBoundingClientRect()` must divide by the zoom (see the help tips'
`place()`)

### Adding WebSocket Commands

1. **Handler:** Add to `control_handler.cpp` in the `if/else if` chain.
   Commands are plain text `PREFIX:value`. Throw `CommandRejected` for an
   invalid value (it is then neither persisted nor echoed to browsers); call
   `set_applied()` when the handler clamps/normalizes, so the applied value -
   not the raw request - is what gets saved and synced
2. **Persistence / sync:** add the prefix to the schema in `settings_store.cpp`
   to remember it, and to `is_replayed_command()` to replay it to newly
   connecting browsers
3. **Message Builder:** Add function in `message_builders.cpp` if it needs one
4. **Test:** Send from browser console: `ws.send("YOUR_CMD:value")`

### Performance Profiling

```bash
# Install perf
sudo apt-get install linux-perf

# Record with call graph
sudo perf record -g -F 99 ./kraken_doa

# Analyze results
sudo perf report

# Check context switches (should be <10,000/sec)
sudo perf stat -e context-switches ./kraken_doa

# View live stats
sudo perf top
```

## Key Files Reference

**Entry Point:**
- `src/main.cpp:75` - main() function, thread initialization
- `src/main.cpp:70` - initialize_persistent_buffer()
- `src/main.cpp:78` - mallopt() for multi-threaded malloc performance

**Configuration:**
- `include/config.hpp` - All compile-time constants
- `include/globals.hpp` - Global state declarations
- `include/types.hpp` - Shared data structures

**Critical Hot Paths:**
- `src/utils/iq_converter.cpp:convert()` - IQ conversion (auto-vectorized)
- `src/signal_processing/shared_decimator.cpp:decimate()` - Thread-local decimation
- `src/networking/data_receiver.cpp:data_receiver_thread()` - Main data ingestion
- `src/networking/data_receiver.cpp:decimation_processor_thread()` - Decimation coordination
- `src/networking/data_receiver.cpp:fft_processor_thread_per_channel()` - Per-channel FFT workers

**UI Communication:**
- `src/networking/websocket_server.cpp:web_server_main()` - WebSocket event loop
- `src/control_handler.cpp` - Command processing
- `src/message_builders.cpp` - JSON/binary message construction
- `kraken_doa.html` - Browser UI (no rebuild needed to modify)

## Common Issues and Solutions

### Audio Dropouts or Crackling

**Symptom:** Browser audio stutters or has gaps

**Root Cause:** Audio sample rate mismatch between config and browser

**Solution:**
1. Open browser console (F12)
2. Look for: "AudioContext created with native sample rate: XXXXX Hz"
3. Edit `include/config.hpp`: Set `AUDIO_SAMPLE_RATE` to match (usually 48000 or 44100)
4. Rebuild: `make rebuild`

### FFT Display Frozen

**Symptom:** Spectrum display not updating

**Check:**
1. Is Heimdall server running? `echo '{"command":"get_status"}' | nc localhost 8092`
2. TCP connection: Check console for "Connected to data server" message
3. WebSocket: Browser console should show WebSocket connection established
4. Data flow: `make run` output should show periodic raw buffer stats

### High CPU Usage on Raspberry Pi

**Symptom:** CPU usage >90% constantly

**Solutions:**
1. Reduce active channels (process fewer channels)
2. Increase decimation factor: Higher index in `BANDWIDTH_OPTIONS[]`
3. Disable DoA processing if not needed: `doa_enabled = false` via WebSocket
4. Check thread count: Should be ~23-28 threads (expected)

**Not a solution:** Reducing thread count below optimal will decrease throughput

### Compile Errors After Modifying Headers

**Symptom:** Old definitions still referenced after header changes

**Solution:**
```bash
make distclean  # Remove all build artifacts AND dependencies
make            # Full rebuild with fresh dependency tracking
```

**Why:** Dependency files (`.d`) may have stale information

### uWebSockets Build Failures

**Symptom:** Missing headers from `uWebSockets/src/`

**Solution:**
```bash
make distclean       # Remove local uWebSockets
make check-uws       # Force fresh clone with --recursive
```

**Why:** Submodules may not have been initialized properly

## Performance Benchmarks (Raspberry Pi 4)

| Component | Operation | Time | Throughput |
|-----------|-----------|------|------------|
| IQ Conversion | 65536 samples | ~5µs | 12 GB/s |
| Decimation | 5ch × 16384 samples | ~2ms | 40 MS/s |
| FFT | 1024-point | ~150µs | 6.8M FFT/s |
| FM Demod | 1024 samples | ~15µs | 68 MS/s |
| MUSIC | 256 snaps, 5ch | ~10ms | 100 DoA/s |

**System-Level:**
- Packet processing: 50-80 packets/sec sustained
- CPU utilization: 75-85% (expected with thread oversubscription)
- Cache hit rate: ~60% (acceptable for real-time streaming)
- Memory bandwidth: ~70% utilized

## Testing Workflow

**Manual Testing:**
1. Ensure Heimdall server is running first (`../heimdall_v2/`)
2. Wait for server phase compensation to converge (~60s)
3. Start client: `make run`
4. Open browser: `https://localhost:8080` (accept self-signed cert)
5. Verify FFT display updates in real-time
6. Test FM: Enable and check audio playback
7. Test DoA: Enable and verify direction estimates
8. Test bandwidth: Change via UI dropdown, verify smooth updates

**Performance Testing:**
```bash
# Profile with perf
sudo perf record -g ./kraken_doa
# Run for 30-60 seconds, then Ctrl+C
sudo perf report

# Monitor context switches
sudo perf stat -e context-switches -I 1000 ./kraken_doa

# Check cache behavior
sudo perf stat -e cache-references,cache-misses ./kraken_doa
```

## Dependencies

**Required System Libraries:**
- `libfftw3-dev`: FFT processing (single-precision `fftw3f`)
- `libliquid-dev`: Digital signal processing (decimation, filtering, resampling)
- `libeigen3-dev`: MUSIC DoA algorithm (eigenvalue decomposition)
- `libssl-dev`: SSL for WebSocket server (HTTPS required for Web Audio API)
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies:**
- `uWebSockets/`: HTTP/WebSocket server (auto-installed by Makefile)
- `concurrentqueue.h`: Moodycamel lock-free queue (header-only, in `include/`)

**Install All:**
```bash
make deps  # Ubuntu/Debian
```

## Additional Documentation

- `OPTIMIZATION_SUMMARY.md`: Detailed performance analysis and benchmark results
- `AGENTS.md`: Development workflow and contribution guidelines
- `../CLAUDE.md`: Root-level documentation covering full Heimdall v2 system
- `../heimdall_v2/CLAUDE.md`: Heimdall server architecture
- `Makefile`: Build system reference (see `make help`)

## Design Principles

**From Performance Optimization Experience:**

1. **Compiler auto-vectorization > manual SIMD**
   - GCC is very smart with simple loops
   - ARM NEON support is excellent in modern GCC
   - Manual intrinsics add complexity without benefit

2. **Thread-local storage eliminates allocations**
   - Zero-cost per-call pattern
   - Perfect for streaming workloads
   - Avoids lock contention

3. **Cache locality > excessive parallelism**
   - L1 cache is 100x faster than RAM
   - Thread-local data stays hot
   - 6-7x thread oversubscription is intentional (I/O bound)

4. **Hardware math functions on ARM**
   - `atan2f` uses NEON internally
   - Faster than approximations
   - `sincos()` faster than separate `sin()`/`cos()`

5. **Power-of-2 buffer sizes**
   - Bitmask indexing (no modulo)
   - Cache-aligned naturally
   - Compiler optimization friendly

6. **Measure first, optimize second**
   - Profile to find real bottlenecks
   - Don't assume what's slow
   - Benchmarks are essential

## Port Reference

**DoA Client:**
- **8080**: Web interface (HTTPS + WebSocket)
- **8081**: Plain HTTP DoA value page (Android app)
- Connects to Heimdall server ports 8091 (data) and 8092 (control)

**Heimdall Server** (in `../heimdall_v2/`):
- **8070**: Web interface (HTTP + WebSocket)
- **8091**: TCP data server (multi-channel IQ streaming)
- **8092**: TCP control server (JSON commands)
- **1234**: RTL-TCP server (rtl_tcp compatible)
