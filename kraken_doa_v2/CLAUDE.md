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
- Squelch outside coherent: the FFT method on the VFO's own tuner (pipeline,
  fm_only indicator and the audio gate), no beamformed-FFT squelch (a saved
  "beamforming on" read a stale overlay); the Digital squelch works in every
  mode (it only needs the decoder - see *Digital squelch*). The VFO cards
  rebuild on a mode change (`decListMode` is part of updateDecimatorList's
  structure check)
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
- `applyClip(newFrame)`: ONLY a frame from the receiver (`applyClip(true)`)
  adds a waterfall line; zoom / zoom slider / edge-clip slider redraw the
  last frame without one (the waterfall just takes the new range for its
  overlay + clicks), and a wheel notch that changes nothing (`zoomWheel`
  returns false at 1x / 16x) redraws nothing. Every redraw used to push the
  frame into the waterfall again: it ran 2.5-3.5x fast while zooming
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
  nxdn, mpt1327, pocsag, aprs, adsb (see *ADS-B* below), ais (see *AIS*),
  radiosonde (see *Radiosonde*), dme and ils (see *DME / TACAN* and *ILS /
  VOR / marker*), ism (see *ISM devices*), radar (see *Pulse radar*),
  ssr1030 (see *1030 MHz interrogations*), uat (see *UAT*), flarm (see
  *FLARM / OGN*), dtv and cells (ARRAY plugins - see *Digital TV* and *LTE
  / 5G NR Cell Decoder*)
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
  names (P25, DMR, TETRA, DSTAR, D-STAR, NXDN, MPT1327) to their plugins and
  renamed plugins to their new id (`renamed_plugin()`: `sfn` -> `dtv`; the
  snapshot's `sfn.*` options too), so saved settings and old clients keep
  working
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
  separation, adsb's 1-bit repair, dme's DoA per beacon, cells' Cell
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
  the button and the list. Code that sets `sel.value` calls `ddSync(sel)`
  (`digFillSelect` and the tab rebuild do); `digFillSelect` leaves the value
  alone while that list is open (`ddOpenSel`). Keyboard: arrows / Home /
  End / Enter / Escape / Tab; a press elsewhere, a scroll or a resize closes
  it. Tab labels and state lines use `digShort(name)` (without the
  "(details)"; the tab's tooltip has the whole name). Test (scratchpad
  e2e/t_dd_ui.py, headless Chromium, the real plugin list). The bottom panel has one tab per decoder MODE
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
  fractional zoom (tiles of the nearest integer zoom scaled) MAP_MIN_Z 2 -
  MAP_MAX_Z 22: past a layer's deepest tiles (`max` 19) those are scaled up
  (z22 ~3 cm/px, 8x); range rings down to 5 m, graticule to 0.00001 deg
  (decimals from the step), drag / wheel
  / pinch / double-click, +/− ⌖ (station) ⤢ (fit all), Street (OSM
  tile.openstreetmap.org) / Satellite (Esri World_Imagery + the
  World_Boundaries_and_Places label layer), attribution shown. Tiles: cache
  of 400 (LRU), a failed tile retried after 15 s doubling to 5 min, a missing
  tile drawn from a loaded lower-zoom tile; underneath always a lat/lon
  graticule, so with no internet the markers sit on a plain grid + an
  "offline" notice (tile errors in the last minute and no tile loaded for
  30 s). Station = yellow mast + range rings (step to suit the zoom; the
  mast can be selected and dragged - *📡 Station on the map*).
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

**📡 Station on the map (UI only, kraken_doa.html "📡 STATION ON THE MAP"):**
- A click on the station's mast selects it (`mapStSel`: cyan mast in a
  ring; a click elsewhere or Esc deselects - `mapSelectObj`); a press on
  the SELECTED mast drags it (`mapObjDrag`, started in mapInit's
  pointerdown before the pan; touch: no long-press menu then), and a
  heading line (70 px, label in degrees) ends in a round handle that turns
  it (0.5 deg steps). Only with a static location (`mapStaticSource()` =
  the Station Information select): a GPS / phone position would be
  replaced by the next fix - the station is still selectable there, the
  status line says how to place it by hand
- Right-click menu "📡 Set station here": static location there (heading
  kept); from GPS / mobile it asks (confirm) and switches the source
  (`setLocationSource('static')`). Selects the station
- Sent as the panel's `STATIC_LOCATION:lat,lon,heading` (6 decimals, the
  sidebar fields set too): the position on release, the heading every 300
  ms while it turns (the DoA lobes turn live; settings are saved ~1 s
  debounced) + on release. `mapStLocal` {lat, lon, hdg} is shown meanwhile
  (`mapStationPos()` / `mapStationHdg()` - the drawing, hit tests and the
  viewshed follow use those; popups etc. keep `mapStation`, the receiver's)
  until a map push reports exactly that position (`%.6f`) or 6 s pass
- Hover cursors: `move` over a selected draggable, `grab` over the heading
  handle. State in the globals block (`mapStSel`, `vsSel`, `mapObjDrag`,
  `mapStLocal`)
- Tests (scratchpad): t_vs_station.py (select, heading drag live + release,
  station drag with a pause -> viewshed follows, receiver confirms, click
  away / pan when unselected, Set station here, GPS source: selectable but
  fixed + the confirm switch, a viewshed point and a marker's viewshed
  dragged, Esc, a panel edit followed), t_vs_touch.py (tap select, touch
  drag, touch heading)

**📍 Map markers (user-placed; `src/map_markers.cpp`, UI "📍 MAP MARKERS" in kraken_doa.html):**
- Right-click on `#map-canvas` (`contextmenu`; touch: a 600 ms long press
  without moving, `lpT` in `mapInit` - a touch pan needs 8 px, a mouse 3 px)
  -> `mapMenuOpen` (`#map-menu`, positioned in the pane): "📍 Add Marker"
  first, on a marker's pin also Edit / Delete, the clicked lat/lon as a
  footer. Closes on a press elsewhere (document capture listener), Escape,
  wheel, window blur. The right mouse button no longer starts a pan
- Backend = the single source of truth (shared by every browser): a list of
  {id, lat, lon, name, notes, freqs:[{hz, label}], t}. `MARKER_SET:{json}`
  creates / replaces (id = [A-Za-z0-9_-]{1,24}, chosen by the page),
  `MARKER_DEL:id`, `GET_MARKERS` (page connect) - every one answered to ALL
  pages with `markers::message_json()` = `{"markers":{"list":[...]}}`; a
  refused MARKER_SET adds `error` + `id` (only the page that sent it - the id
  is random - reports it). `MARKER_` is in `is_query_command` (no sync_cmd
  echo). Parsed with `geo::json_parse` (geo_address.hpp). Limits: 500
  markers, 50 frequencies (1 Hz..100 GHz, stored as integer Hz), name /
  label 80 bytes, notes 4000 (cut on a UTF-8 boundary, control characters
  dropped - notes keep \n / \t). `map_markers.json` (cwd-relative, so
  Docker's volume gets it) is rewritten (tmp + rename) on every change and
  read by `markers::load()` in main; not touched by RESET_SETTINGS
- Page: state `mkList / mkSel / mkEdit / mkWantEdit / mapMenuAt` in the
  globals block (mapDrawNow -> `mkDraw` / `mkRender` can run before the
  section is evaluated). No optimistic updates: Add Marker sends MARKER_SET
  and sets `mkWantEdit`; the list arriving with that id opens it in edit
  mode (name focused). Pins (`MK_COL` pink, tip on the spot, head 15 px
  above; `mkHit` tests the head, before decoder points) with the name + the
  first frequency (+N). `#map-mk` panel (bottom right; `.map-pane.mk-open`
  narrows the 📡 DF panel at the bottom left so they don't overlap): view =
  position, distance / bearing from the station, frequency buttons, notes,
  Edit / Centre / Delete; edit form built once (`dataset.edit`) so typing
  survives list pushes, `mkLeaveEdit` asks before dropping unsaved changes
  (`dataset.orig` vs `mkFormRaw`); "lat, lon" pasted in the latitude box
  works. A click on the empty map closes the view (not an edit). ⤢ fits the
  markers too
- `mkTune(hz)` (a frequency button): the SELECTED VFO (`activeDecimatorId`);
  refused for a locked VFO (fixed-frequency decoder) or outside
  FREQ_MIN..FREQ_MAX. Coherent / wideband scan: inside `maxOffsetHz` the VFO
  just moves; else `FREQ:` on whole kHz (`sendFreqChange`) with the VFO
  offset taking the rest (145.5125 -> FREQ:145.513 + VFO -0.5 kHz).
  Independent: a tuner whose band holds it (`paneMoveVfo`, own first), else
  the VFO's own tuner retunes (`paneSendFreq`) with the VFO at 0
- Tests (scratchpad): t_markers.py (backend commands, refusals, a second
  browser, the file, reload after restart) and t_ui.py (headless Chromium
  via CDP: right-click / long press / Escape, add + edit + save, escaping,
  tune paths incl. independent mode with stubbed panes, delete) on a TEST
  build with ports 18080/18081/18091/18092. On this Pi, run
  `/usr/lib/chromium/chromium` directly (the /usr/bin wrapper adds a V8 flag
  the renderer rejects) with `--no-proxy-server` (the system proxy lookup
  hangs every network load)

**📡 OpenStreetMap masts layer (UI only, kraken_doa.html "📡 OSM MASTS"):**
- "Masts" button (`mapState.masts`, localStorage like the other view
  settings) + a colour legend (`#map-legend`). The BROWSER fetches from
  OpenStreetMap's Overpass API (`MAST_URL` overpass-api.de, CORS `*`), like
  the tiles - kraken_doa is not involved. overpass-api.de answers 406
  without a Referer (browsers send one: the page has no referrer policy -
  keep it that way) and OFTEN 504 under load
- Query: `nwr` man_made=mast (tower:type != lighting), man_made=tower +
  tower:type=communication, man_made=antenna in the bbox; `out tags center
  qt 5000`; `[timeout:25][maxsize:67108864]` (a small maxsize is admitted
  more often than the 512 MiB default under load)
- overpass-api.de RATIONS queries per connection (`/api/status`: "Rate
  limit: 2", a slot blocked for a while after each query; 429 beyond) - the
  first version loaded view by view, got 429 after a few pans and only ever
  showed the area loaded first ("masts only within a radius of me"). Now,
  in zoom-12 cells (`mastCells`: "x,y" -> load time), one query at a time:
  1. the REGION: MAST_REGION_KM 150 each way around the station
     (`mastRegionCells`; no station 5 s after the first try: around the
     view, z >= 8) in ONE query ([timeout:60], abort 80 s; NZ test: 1301
     masts in 8 s). 5000 answers = too dense: `mastRegionKm` halves (< 20
     = no region). Re-centred when the station moves a third of it.
     After 2 failed region queries the view goes first
  2. elsewhere the view's missing cells (+1 around) at zoom >= MAST_MIN_Z
     11, 0.7 s after the view stops changing ([timeout:25], abort 45 s; 5000
     answers = shown, cells not marked, no new query of that bbox below zoom
     + 1: `mastDense`)
  `mastSchedule()` runs after every draw but restarts the countdown only
  when the VIEW changed (`mastViewKey`; reset while the layer is off) -
  redraws of the same view (DF status 2 Hz, map push 1 Hz) restarted it
  forever. Errors: 429 / 5xx / no answer / 200 with an error `remark`
  (query cut short - its cells are NOT marked) = busy; fetch rejected = no
  internet; retry after 15 s doubling to 5 min (`mastRetryAt`, shown in the
  status line). Memory cleared above MAST_MAX 30000 masts
- Loaded masts are drawn at ANY zoom (dots below 13); status adds "zoom in
  to load other areas" below zoom 11 when the view has cells not loaded.
  localStorage `kraken_masts` {v:1, cells:[[key, t]], masts:[[type, id, lat,
  lon, tags]]} (saved 2 s after a load, dropped if > 3.5 M chars; read on
  the first draw): cells older than MAST_KEEP_DAYS 7 and their masts are
  dropped, so a reload asks Overpass nothing (NZ region: 266 k chars)
- Third-party data: elements kept only with type node / way / relation and
  an integer id (key `n123` goes into onclick), every tag escaped
  (`digEsc`). Category (`mastCat`, colour): broadcast (communication:
  television / radio / broadcast / dab) > mobile (mobile_phone, gsm, umts,
  lte, 2G-5G, nr) > microwave > amateur (amateur_radio[:repeater] - not
  the POTA / SOTA award tags) > not stated
- Drawn after the DF heat maps, before the station / markers: lattice
  icon (z >= 13) or dot, names at z >= 15. Click (after user markers and
  decoder points): popup in `#map-pop` (`mapSel` = "osm:<key>"; a map push
  doesn't close it) - type + construction, "Used for" (`mastServices`:
  "Mobile phone (LTE, UMTS)"), operator, height, position, distance /
  bearing, every other tag; link to openstreetmap.org; "📍 Add as marker"
  (`mastToMarker` -> `mkCreate`: name + OSM details as the notes, opens the
  editor). Attribution adds "masts © OpenStreetMap contributors" on the
  satellite layer
- Tests (scratchpad): t_masts.py (popup, legend, 504 / timeout remark /
  offline, malformed / injected elements) and t_masts2.py (region first,
  any zoom, the localStorage cache incl. expiry, too-dense region, region
  failures -> view first, off / on) - headless Chromium, real Overpass only
  for the first region load, the rest stubbed via
  `Page.addScriptToEvaluateOnNewDocument` (a fetch wrapper)

**👁 Viewshed (UI only, kraken_doa.html "👁 VIEWSHED"):**
- Line of sight over the terrain from the station or any picked place:
  "Viewshed" button (`mapState.vs` layer + `vsOpen` fold). The STATION'S
  viewshed is always there (`vsEnsureStation`, from `vsToggle` / `vsRender`:
  re-added when missing; its row has no ✕, `vsRemove` refuses it - hide it
  with its colour box) - no "+ Station" button (the user's call
  2026-10-12, and no "+ TX Node" either: TX antennas are added only by
  right-clicking). DELETE removes the selected node (`keydown` beside Esc:
  a viewshed site - not the station's - or the link's place = the link;
  not while typing in a field). The station's mast on
  the map carries the callsign (Station Information `station-id`, redrawn
  on `STATION_ID:` / an edit). LINE OF SIGHT MODE IS THE STATION'S
  VIEWSHED ONLY (the user's call 2026-10-13): `vsJobItems` = the station,
  `vsDraw` skips the rest, their rows are hidden (CSS), `vsHit` picks none,
  and the right-click menu / marker / mast have no add item there - nor
  any viewshed / path loss item at all outside signal mode or with the
  viewshed off (`plMenuOn`, 2026-10-13) (it was
  "👁 Line of sight from a TX antenna here", before that "👁 Viewshed from
  here"). Signal mode adds TX antennas: right-click "📶 Add TX antenna
  here" / "at “marker”" (`vsAddLabel` / `vsAddShort` / `vsAddTip`), the
  marker panel and the OSM mast popup (`vsAddMast`: antenna
  at the mast's `height` tag, feet with ' / ft). Up to `VS_MAX` 6, each in
  its own colour, drawn over the tiles (`vsDraw`, before the DF heat maps)
  with a dashed radius ring (the circle on the ground, drawn point by point
  - at 1000 km it is visibly egg-shaped on the map). Radius 1 to
  `VS_RMAX_KM` 1000 km (globals block; aircraft at 10 km drop below the
  horizon at ~450 km, balloons at 30-35 km at ~750-800 km)
- Terrain: AWS Terrain Tiles (Mapzen terrarium PNG, `elevation-tiles-prod`
  on S3, CORS `*`, max zoom 15; SRTM / GMTED2010 / ETOPO1 + national models,
  e.g. LINZ 8 m in NZ), fetched BY THE BROWSER with `cache: 'force-cache'`
  (S3 sends no Cache-Control) - kraken_doa is not involved. Decoded by the
  worker's own PNG decoder (DecompressionStream + unfilter), NOT a canvas:
  anti-fingerprinting canvas noise (Brave, Firefox resistFingerprinting)
  would flip the R channel's low bit = 256 m
- Worker (`vsWorkerMain`, started from a Blob of its source; only its own
  scope runs there; `self.vsTest` exposes its functions to node tests):
  one job at a time. A job first loads the tiles it doesn't hold in memory
  (6 at a time; the progress counts only those - "loading terrain k/n",
  then "computing"), then cleans the tiles that need it (below), then
  computes ring by ring. Decoded tiles stay in memory (`CACHE_MAX` 256 =
  64 MB, Float32, each with its clean-up's fixes; a job marks its tiles
  used, and after it the least recently used go first, never the tiles
  that job used): a moved point or a
  changed height / k loads only the tiles it hasn't got (a 3 km move at
  1000 km: 16-21 of ~200). It was an LRU of 80 - smaller than one 300 / 1000 km
  job (150-200 tiles) read in the same order every time, so it never hit
  and every update decoded everything again (~2.3 s at 1000 km; the
  download itself was always the browser's HTTP cache). `{cancel: job}`
  stops one while loading (between tiles) or between rings: a
  `setTimeout(0)` before each ring (and every 0.1 s of cleaning) lets the
  message in - with every tile in memory nothing else waits, so a dragged
  point's stale job would run to its end
- RINGS (`vsPlan`, the job's `levels: [{z, r0, r1}]`): zoom `VS_ZFINE` 14
  around the antenna (~8 m cells at 37 deg, to ~3.8 km), each next ring one
  zoom coarser (`VS_ZSTEP` 1) out to about twice the distance, each grid
  <= `VS_NMAX` 1025 cells a side (radius measured at the poleward edge):
  cell / distance stays 1/512..1/256, so terrain is seen at about the same
  angle everywhere. 30 km = 4 rings (z14-11, 8-61 m), 300 km = 8 (z14-7,
  8-978 m), 1000 km = 10 (z14-5, 8 m - 3.9 km). A ray of ring k starts at
  r0 with the slope carried in from ring k-1 (`hzIn`: its rays' slopes at
  r1 by start bearing; every ring has the same `nRays` = 8 x the largest
  half-size, so they line up), skips everything inside r0, assigns cells
  to rExt = r1 + 0.75 of the next ring's cell (overlap, no gaps) and
  returns its slopes at r1. The antenna's ground = the z14 ring's.
  `ringGrid()`: the grid (half-size from the poleward edge's cell size)
  and the tiles touching the ring by great-circle distance to their edges
  (sampled every 8 px; not those inside r0). WHY: the first
  version used ONE grid for the radius (~400 m cells at 300 km): the hill
  next to the user's antenna (1.2-2 deg high to the NE) was averaged away
  and the antenna stood on a 400 m average, so aircraft at 10 km looked in
  sight at 300 km where they need 11.5 km. Against a high-resolution ray
  reference (scratchpad vs/ref_rays.py: z14 to 10 km, z13 to 40, z12 to
  160, z11 beyond) at the user's station, 14 bearings x 9 distances, the
  lowest altitude in sight: one grid mean error 74 / 415 / 1243 m (0-30 /
  30-120 / 120-300 km, 50 points "visible" > 200 m too low); rings 1 / 9 /
  33 m, none
- Cost: 30 km = 90 tiles, 7.6 MB (Denver 8.7), 300 km = 171 tiles, 15 MB,
  1000 km = 194 tiles, 16.8 MB; the first download ~6-15 s. Compute (Pi 5,
  node, Auckland) for 30 / 300 / 1000 km: the first time (tiles decoded +
  cleaned) 2.2 / 4.9 / 5.9 s, again (same place, new height / k) 1.1 /
  1.8 / 2.1 s - the sweep 0.9 / 1.5 / 1.8 s of it. Mountains clean longer
  the first time (Dolomites 1000 km: 9.6 s, 5.9 of it the clean-up of 189
  tiles; again 2.2 s). A point moved at 1000 km (5 places, downloads
  cached): 3 km 2.3-2.5 s, 0.5 km 1.9-2.2 s. History: 9-28 s before the
  tile memory and the faster passes (every tile decoded again, every pass
  a full scan), then 4.8-8.6 s, then ~5-11 s -> 2-2.5 s with the
  interpolated sweep + per-tile clean-up (t_cmp.mjs; the first run in
  mountains ~5-10 % slower than cleaning the grids: whole tiles + edges)
- Terrain clean-up (`despike(raw, N, cellM)`, then <= 0 m -> 0 - the tiles
  hold the sea FLOOR; land below sea level also becomes 0, Dead Sea /
  Caspian basins are wrong). The tiles have resampling artefacts where
  sources meet: Auckland coast z11-12 (+948 next to -1819 m, 100-500 m
  pillars), a seam on the Colorado plains (912-1610 m in 1377 m flat), the
  Rockies (-218 m next to 2520 m), and a BAND 2-4 rows thick at -36.12 deg
  in the sea north of the Coromandel (NZ), zooms 8-13, up to 32767 m (100-
  150 km NE of the user's station). Rules: > 9000 m = invalid (5x5 median
  of the valid ones); then up to 8 passes (until nothing changes): a cell
  with |e - mean of 8| > 25 m that stands out from the 24 around it (5x5,
  clamped values) > 40 m, > 6 MAD + 40 m and > 1.5 cells' worth of height
  (at coarse zooms an island / headland / cone is 1-3 cells on flat ground:
  zoom 8 around Auckland flattened 271 real ones without it), or - cells
  <= 100 m - with the resampling overshoot the other way in its raw 3x3
  (below the median by half its height); or standing > max(500 m, 4
  cells) and > 6 MAD + 40 ABOVE the 12 cells of its column or row (+-6:
  the band hides from the 5x5; lower thresholds cut a real 240 m ridge in
  Colorado and, for pits, filled a real gorge). Checked on 742 tiles: no
  cell of the clean LINZ z14 tiles, Everest, Matterhorn, Mt Cook, El
  Capitan, Dolomites, Dead Sea is touched; what goes is the band, coast
  artefacts (982-1314 m "peaks" around Auckland, max real ~700 m) and
  seams. Tiny islets / sea stacks of 1-3 cells at fine zooms can go too.
  Run PER TILE (`clean(z, x, y, t)`): the tile + `PAD` 16 pixels of its 8
  neighbours (those in memory; a missing one = the tile mirrored at that
  edge), the tile's own latitude for the cell size; kept as the cells it
  changed (`t.fix` {i, v}) and the neighbours it had (`t.nb`, bit (v+1)*3
  + u+1). A job cleans a tile only when it has no fixes yet or a neighbour
  that is in this job wasn't there then (`nbMask`): a 0.6 km move at 1000
  km cleans ~5 tiles, 3 km ~45 (16 new + those next to them), the same
  place again none. The rings are put together from raw tiles + fixes.
  Checked (t_worker 7): 133 cached tiles with all neighbours, incl. the
  Coromandel band, give exactly the cells of despiking their 3x3 mosaic at
  once (PAD 8 left 220-340 m of a band's edge in the sea: a band clears
  from the side over several passes); with a neighbour missing, the cells
  > 2 PAD from that edge are unchanged; at 5 places x 1000 km the terrain
  the rays use is identical to cleaning each ring's grid (what it did
  before: every job, every grid - and the grids held zeros inside each
  ring's hole, so cells next to it were "fixed" too: the most of the
  Dolomites' 120 / Colorado's 324 spikes, all in cells no ray reads).
  Faster passes (same result): the passes after the first test only the
  cells a fix of the pass before can reach (the 5x5 around it, +-6 along
  its row and column) - each pass was a full scan, and in mountains a
  few cells keep changing for 5-7 passes (Dolomites 1000 km: 12.4 s ->
  4.3 s); the MAD is only worked out when the test gets that far
- Sweep of one ring along GREAT CIRCLES (the path a line of sight takes).
  The first versions stepped straight lines on the map (R2) = rhumb lines:
  at 300 km their start bearing is ~0.7 deg off the great circle's to the
  same place, at 1000 km ~2 deg, and mid-way they run up to ~15 km beside
  the true path (at the user's station, 300 km to the NE: 11483 m needed
  along the rhumb line, 11762 m along the great circle). `nRays` rays at
  even start bearings (<= 0.8 cell apart at the ring's edge), each walked
  from r0 in steps of 0.4 of the ring's smallest cell: the position on the
  sphere by rotation (p, q unit vectors; no drift over 3000 steps), -> cell
  coordinates by atan2 longitude (wrapped around the observer's - the
  antimeridian) and atanh Mercator latitude - exactly every K = 8 steps,
  linear between (a great circle bends on the Mercator grid by <= 1 /
  radian, so over 8 steps, <= 3.2 cells, it leaves the line by < 0.001
  cell at zoom 5; a jump - the longitude wrap, a pole - falls back to
  every step exactly). The two transcendentals were most of the sweep:
  3.5 -> 1.8 s at 1000 km; vs every step exact, 0.014 % of the cells'
  `need` moved > 1 m and none changed in sight at 2 m - 10 km; terrain
  bilinear; each cell takes the sample nearest its centre (`best`, u8 of
  the squared offset); a cell no sample landed in (>= 3 of 4 neighbours
  have one) takes their mean. Distance = the exact great-circle distance.
  Curvature d^2 / (2 k R), R 6371 km, k 4/3 radio / 7/6 light / 1
  (`vsState.k`; vs the exact form 100 m at 1000 km). Output
  per cell: `need` = the height a target needs above the ground to be in
  sight (u16: 0.1 m steps to 3 km, then 10 m; 65535 = not this ring) and
  `ground` (i16 m, the ray's interpolated ground) - so the target height,
  above ground (`need <= t`) or above sea level (`ground + need <= t`,
  aircraft), repaints at once (`vsPaint`) without recomputing
- Page: `vsState` (globals block; localStorage `kraken_vs`: tgt = ONE
  target height (default 2 m) that `amsl` reads as above ground or above
  sea level - switching keeps the number (it was a value per reference, so
  the box jumped to another number; an old {agl, amsl} pair loads as the
  one that was shown), amsl, k, h / r = what new ones get (the last used), items
  [{id, name, lat, lon, h, r, col, on, st = the station, mk = from a
  marker}] - rebuilt field by field on load, a damaged entry is dropped),
  `vsRes` (results by item, key = lat, lon, h, r, k; `levels` = the rings,
  each with its canvas), `vsFail`. Jobs start from `vsDraw` (so after a
  reload only with the map shown). Drawing: each viewshed's rings coarse ->
  fine, opaque, onto one layer (`vsOff`, map-sized), then that layer at
  alpha 0.44 onto the map - the rings' overlapping edges don't tint twice.
  `% in sight` by area (cells weighted by their size). Hover: the finest
  ring with data there. A hidden item frees its canvases; ImageData only
  while painting (phones). `vsReady` (globals block, set at the section's
  end) guards vsDraw / vsRender: rpApply can draw the map before the
  section's consts exist. Panel `#map-vs`
  in `.map-tl` (under the top-left buttons, with the masts legend): target
  + above ground / sea level + refraction, a row per viewshed (show / hide
  swatch, ⌖ fits its circle, ✕, Antenna / Radius - a change recomputes,
  "% in sight", tooltip: ground + antenna, cell size, zoom, time, spikes),
  the readout under the pointer (a tap
  on touch): ground height, per viewshed in sight / "hidden - needs N m
  above ground (or sea level)", distance + bearing from it
- The station's viewsheds (`st`) FOLLOW the station (`vsFollowStation`,
  from every map push and from the drags below): moved > 20 m -> new
  position + recompute; with a GPS / phone source at most every 30 s
  (driving would recompute without end). A point's viewshed (not the
  station's) is selected by a click on its dot (white ring) and dragged
  (`vsMoveItem`; from a marker: the `mk` link is dropped). While dragging,
  a 0.4 s pause updates (the running job is cancelled between rings,
  ~0.6 s), the release does. See *📡 Station on the map* below
- Tests (scratchpad vs/ + t_vs_ui.py): `node t_worker.mjs [--net]`: PNG
  decoder = PIL on 27 tiles; despike = the numpy reference
  (ref_despike2.py; scan_despike2.py runs it over every cached tile) incl.
  the band; flat sea: hidden height = (D - sqrt(2kRh))^2 / 2kR within
  0.12 m at 1080 points around 48.5 N (synthetic worlds built on the
  sphere with `ringGrid`), a 10 m target from 100 m in sight to 4.12
  (sqrt 100 + sqrt 10) km; a flat-topped wall (no curvature): 198.8 m
  needed 6 km behind (10 + 90 x 6 / 2.83-2.87 km); two rings (19 m / 76 m)
  = one 38 m grid at 60 N (median 0, worst 7.7 m; 16 km behind a 60 m
  hill 2 km out 430 vs 430 m - 1 m without the carried horizon); real
  terrain (Auckland 30 / 300 / 1000 km, Mt Cook, Zermatt, Denver 80 km,
  across the antimeridian in Fiji, Tromso 600 km at 69.6 N) vs a brute-
  force great-circle line of sight per cell through the ring holding each
  step: same in-sight answer for 99.5-100 % of 2000 cells each; offline /
  HTTP 404 / garbage tile / cancel; tiles in memory (same place again: 0
  fetches, same result; a 2 km move loads 13 of 154; a cancel while
  computing stops the job after the ring it is in; trimmed to CACHE_MAX
  keeping the last job's tiles; least recently used dropped first - 30 km
  jobs A, B, C, A, D: A still all in memory); per-tile clean-up = the 3x3
  mosaic's (above). t_pole.mjs: 84 N / 83 S at 1000 km - ~0.1 % of the
  sweep's 8-step chunks fall back to exact steps (past a pole), no odd
  values. t_cmp.mjs <old copy of the page>: old vs new worker on real
  terrain at 5 places (terrain / need differences, in-sight flips at 2 m
  - 10 km) and the time first / again / after moves (t_equiv.mjs: the
  same, for changes that must be exact). ref_rays.py (great circles; z10 past
  400 km) + dump_run.mjs + compare.py: at the user's station, 8 bearings x
  11 distances to 995 km, mean error 1 / 6 / 22 / 107 m (0-30 / 30-120 /
  120-400 / 400-1000 km; out there 14-93 km are needed and a 3.9 km cell
  spans ~470 m of that). t_vs_ui.py (headless Chromium against the
  test build, real tiles): button, station auto-add, progress, hover,
  target / sea-level repaint, right-click, radius change (rings 14-12 for
  10 km), hide / show, ⌖, k change, marker + mast, cancel by removing, the
  limit, reload, station moved, damaged localStorage, 300 km (8 rings, no
  seams in the screenshot), 1001 km refused, 1000 km (10 rings, 194 tiles;
  a 30 km balloon in sight to ~590-700 km)

**📶 Path loss = the viewshed's 📶 Signal mode (UI only, kraken_doa.html "📶 PATH LOSS"; the engine lives in the viewshed worker):**
- ONE feature with the viewshed (the user's call, after a separate "Path
  loss" mode that was the viewshed OR path loss): the viewshed panel's mode
  switch `vsState.mode` = 'los' (👁 Line of sight: the plain viewshed,
  unchanged) | 'sig' (📶 Signal: how often (%) the signal between each site
  and a radio at the target height is good enough - also WITHOUT line of
  sight, coloured by that reliability, NOT restricted to the cells in
  sight; the edge of the line of sight as a black line, switchable). Each site has
  a ROLE: TX or RX (the station's RX, other sites as added). The STATION
  ISN'T PART OF THE SIGNAL MODE (the user's call 2026-10-12): its row is
  hidden there (`.vs-it.vs-st`), `vsJobItems` / `vsLinkItem` / `plEnds` /
  `plPickSite` / `plDraw`'s tags skip it, a link needs a TX antenna
  (none: no path loss menu item / buttons, `plLinkTo` flashes). TX
  antennas come only from the right-click menu ("📶 Add TX antenna here",
  `vsAddAt('tx')`, named "TX n"; markers too) - no RX adding, no TX / RX
  tag to switch a role (the user's call 2026-10-13; `vsSetRole` is left
  for old RX sites / tests). ONLY
  TRANSMITTERS ARE COLOURED (the user's call, 2026-10-12: an RX site used
  to get its own "where would a transmitter be heard" map - reciprocal, the
  same numbers, but centred on the receiver; with its radius smaller than
  the link it showed green next to a red link, and "best per place" mixed
  TX coverage with RX hearing areas): TX sites (receivers at the target
  height), and the link's TX place when the link's site receives -
  `vsLinkItem()`, id `VS_LINK_ID` '_link', not in vsState.items: around the
  place at the target height (above sea level too: the worker's `m.amsl`),
  receivers at the RX site's antenna height (`vsTgtOf`), radius max(the RX
  site's, 1.25 x the distance + 1 km), worked out when the place is
  DROPPED (vsPump skips it while mapObjDrag is 'pl'), dropped when the link
  goes / its site transmits (`vsLinkSync`). `vsJobItems()` = what is worked
  out / coloured / hovered (LOS mode: every site). An RX site in signal
  mode (old saved ones): no job, no radius circle, its row says "an RX
  antenna - not coloured ..." or
  its link's TX status. Every shown transmitter at once, the BEST per
  place. The LINK is a tool on top: a site <-> a place
  (at the target height), its profile + every model + the budget. The
  other end can be in the air (target above sea level: aircraft, balloons,
  drones - the aircraft is often the TX for a KrakenSDR). The user's
  reference (github.com/mariohm1311/DroneRanger, no licence: ideas only)
  has free space + a line-of-sight check only; the NLOS physics comes from
  the ITU-R Recommendations and NTIA's ITM
- Engine (vsWorkerMain; `self.vsTest` exports it for node):
  - ITM / Longley-Rice: `itmP2P(pfl, htx, hrx, {climate, N0, f, pol, eps,
    sigma, mdvar, time, loc, sit})` = NTIA ITM 1.3 (C++, = ITM 1.2.2),
    point-to-point mode, ported function by function (FindHorizons ->
    itmHorizons, ...; the area mode left out). NTIA's notice is in the code
    (not copyrighted in the US; derivative works allowed with a note of the
    changes + an acknowledgement). Checked: NTIA's 5 published cases
    (p2p.csv / pfls.csv: LOS, diffraction, troposcatter) to their 0.01 dB,
    and 3000 random profiles / parameters against the compiled NTIA C++:
    worst relative difference 4.5e-10, the same mode and warning bits
  - ITU-R P.1812-6: `p1812P2P(d km, h m, R clutter m|null, htg, hrg, f GHz,
    ae km, omega sea fraction, N0, pol 0 h / 1 v, flag4)` - Bullington
    (`p1812Bull`, 4.3.1), the smooth-path Bullington of Attachment 4
    (`p1812BullSmooth`, flag4), spherical Earth (`p1812Se` 4.3.2, first term
    `p1812SeFtIn` 4.3.3 land / sea mixed by omega), smooth-Earth heights +
    horizons (`p1812Heights`, eq 85-95 with the obstruction correction 89-91),
    troposcatter (`p1812Tropo`), at p = 50 %: Lb = max(Lbfs, -5 log(10^-0.2
    Lbs + 10^-0.2 (Lbfs + Ld))). No ducting / layer reflection (they matter
    only for small time percentages), no location variability (the page's).
    Checked against the 63 ITU-R SG3 validation logs in Py1812's tests:
    every intermediate (Lbfs, Bullington, spherical, Ld50, Ldb, horizon
    angles / distances, hstd / hsrd, Lbs, Lb) within 5e-8 dB. NB Py1812's
    dl_p logs Lbulla / Lbulls / Ldsph of its LAST run, the beta0 radius 3 x
    6371 km - not the median one
  - ITU-R P.2108: `p2108Terminal(fGHz, h, type)` (3.1, 30 MHz - 3 GHz: 1
    water / 2 open: height gain; 3 suburban / 4 urban, trees / 5 dense urban:
    diffraction over the clutter, street 27 m; R 10 / 10 / 10 / 15 / 20 m)
    and `p2108Aero(fGHz, theta deg, p %, h, hm)` (3.3 of P.2108-2:
    Earth-space / aeronautical clutter by elevation; erfinv = Giles' single
    precision). Checked: the 282,800 rows of Py2108's validation table
    within 2e-6 dB. Used below its 0.5 GHz floor too
  - Okumura-Hata / COST-231 was a model too (map + link row) and was
    DROPPED (the user's call, 2026-10-12): no terrain - concentric circles
    on the map -, valid only for 150-2000 MHz, 1-20 km, a 30-200 m base, so
    most KrakenSDR paths (handheld to handheld, < 1 km) were outside it. A
    saved 'hata' falls back to P.1812
- LINK (`runLink`, message `m.link`: a = the TX, b = the RX, each {lat,
  lon, h, amsl}): the great circle A -> B (slerp)
  sampled every cell of the finest zoom <= 14 that keeps it to `PL_NMAX`
  4000 points (30 km: z14 8 m; 300 km: ~z11; 1000 km: ~z9), bilinear on the
  job's tiles cleaned as a viewshed's (missing neighbours mirrored), the ends
  on zoom-14 ground; sea = raw height <= 0. P.1812 with flag4 (as the
  coverage, so the two agree), ITM median (mdvar 12, 50 / 50 / 50) and at the
  reliability (mobile mode 2: time = location = rel, situation 50), free
  space (slant); clutter: one end above sea level (in the air) -> 3.3
  at the OTHER end by the direct ray's elevation there (hm from its type: 8
  suburban / 15 urban / 25 dense, none for water / open), both on the
  ground -> 3.1 at both, both in the air -> none (checked: the same path
  both ways round = the same P.1812 loss, the clutter moved). Also the
  elevation at each end, the least first-Fresnel clearance and where, the
  Bullington edge (dbp km, hbp m incl. the Earth bulge) and the profile
  (Float32 d km, h m) for the chart. Its OWN worker instance (`plWorker`,
  the same code: a link must not wait behind a coverage job; one job at a
  time, a newer link cancels the running one)
- COVERAGE (`runSignal` + `plSweep`: a viewshed job with `m.sig` = {f, tgt
  (the other end's height), amsl, pol, N0, envTx, envRx, hmTx, hmRx} - the
  worker's "Tx" is always the SITE, its clutter by the site's role
  (`plSigMsg`); the site's `m.h` above its ground (`m.amsl`, a site in the
  air, is supported by the worker but not used by the page); the
  viewshed's own job, so its terrain tiles are shared):
  every ring's terrain at once (~4 MB each, 10 rings
  at 1000 km), the rays of sweep() but RAY-MAJOR through all the rings, each
  ring from where the previous ended to its rExt (the coarser ring's cells
  there stay empty, under the finer ones). Each ray carries the upper
  convex hull of its profile in the curved-Earth frame (e = h - s^2 / 2 ae,
  where rays are straight): the receiver's horizon is the tangent from it
  (binary search) -> Bullington's S_rim (P.1812's slopes differ from these
  by +-s / 2 ae: the same argmax); the hull of the plain heights for the
  obstruction correction (hobs = a support query, alpha_obr a tangent,
  alpha_obt a running max); running sums v1 / v2 (eq 85-86) and the sea
  count (omega). Line of sight: nu_max over the profile EXACTLY, by branch
  and bound over blocks of 32 / 1024 samples (their highest point bounds
  nu; J = 0 below -0.78, so clear paths rule out whole blocks) - the point
  of least clearance is often not a hull vertex (a 2 m receiver's own
  foreground): hull candidates alone were up to 10 dB off. The profile =
  every 2nd sample (0.8 cell; the terrain is no finer). Two passes: 1 finds
  each cell's sample (nearest its centre, as sweep), 2 evaluates P.1812
  once per cell + the clutter (both on the ground: 3.1, fixed per job; the
  RX in the air: 3.3 at the TX by the elevation there; the TX in the air:
  3.3 at the RX). Per ring `loss` (u16, 0.1 dB; 65535 = the receiver under
  the ground) + `need` / `ground` FROM sweep() itself, run after plSweep on
  the same terrain (plSweep's own line of sight - one ray through all the
  rings - differed from sweep()'s ring-by-ring one in ~0.1 % of the cells;
  now both modes show the identical line of sight, checked cell by cell;
  a cell only sweep() sampled takes its neighbours' loss). Per ring also
  `fad` (u16, the fading's path factors from P.1812's terrain analysis -
  with Longley-Rice too, ~+15 % there: high byte the terrain's knife-edge
  loss beyond the smooth Earth's, Lbull - Lbulls, 0.25 dB; low byte Lbs -
  Lbd + 32 dB, 0.25 dB; 255 = free space / no troposcatter), carried
  through the fills, the helpers' merge and the link of a cell's
  neighbours; checked against exact P.1812 per recorded sample (±1 per
  byte). Spherical-Earth constants hoisted (`seConst`), one
  polarization, one ground when omega is 0 / 1. Progress `['sig', %]`
  (status: "signal N %…"), a break for a cancel every 0.25 s
  Checked (tools/pathloss_validate.mjs; scratch t_cov.mjs): the loss at
  every recorded ray sample = `p1812P2P(flag4)` on that ray's profile prefix
  within 1e-11 dB (Auckland, Zermatt, the Coromandel coast, a synthetic
  coast; receivers above ground / sea level, a TX in the air, clutter, both
  polarizations); a
  link to a cell's centre vs the cell: median 0.04-0.1 dB, p90 0.3-1.3 dB -
  single cells up to ~16 dB apart at shores / ridges (the coverage's outer
  rings use coarser terrain: 30 m at 15 km). Time (node on the Pi 5, tiles
  in memory): 30 km ~6 s, 300 km ~11 s (2.5 / 4.4 M cells; the viewshed
  1-2 s); in the browser on the Pi it took ~17-21 s for 30 km
- SEVERAL CORES (`plSweepPar`, the user's request after WebGPU was weighed:
  no f64 in WGSL - the exact-to-the-reference checks would go -, per-ray
  sequential hulls, no WebGPU on the Pi's Chromium): the signal sweep's
  rays split over HELPERS - nested workers running vsWorkerMain itself
  (`helperPool`, kept between jobs) - contiguous ray ranges, both passes:
  pass 0's best[] merged by the minimum, pass 1's cells merged in ray
  order (a later ray overwrites, as in one plSweep) -> THE SAME CELLS TO
  THE BIT (checked: hash of every need / ground / loss, P.1812 + ITM). The
  coordinating worker does the line of sight (sweep() into needL / groundL)
  meanwhile and the finish (`plSweep(.., {finish: true})`). Helpers:
  hardwareConcurrency - 1 (max 7), fewer if their terrain copies would
  pass 600 MB (no SharedArrayBuffer without cross-origin isolation), none
  below 2; `m.par` / page `vsPar` (null auto, 0 one worker - tests); not
  for m.debug or without nested workers (node: the validator). Replies
  carry job + pass (a cancelled job's late reply can't answer the next
  job); a failure / cancel stops every helper. The row's tooltip says "on
  N workers". Only the signal sweep. The LINE OF SIGHT viewshed was
  parallelized too (the user asked; ring by ring - the horizons carried
  between rings, the next ring posted as soon as the last one's horizons
  were back, compact cell lists merged in ray order: bit-identical) and
  REVERTED: its sweep is memory-bound, ~175 ns a cell, so copying each
  ring's terrain to every helper (~270 ms at 300 km, 3 helpers) + merging
  their cells + the fill-in (~450 ms) cost what the helpers saved - on the
  Pi 5 with the receiver running: 30 km 0.85 -> 0.77 s, 300 km 1.43 ->
  1.5-1.7 s (slower); more helpers make the copies worse. Without
  SharedArrayBuffer (cross-origin isolation) that won't change.
  The line of sight runs DURING PASS 1 (pass 0 is quick - only each cell's
  sample -, so during pass 0 it held pass 1 back: 1.9x -> 2.3x). Measured
  (scratch t_par.py, browser on the Pi 5, 4 cores, the KrakenSDR stack
  using ~0.85 of one): P.1812 30 km 8.6 -> 3.8 s, Longley-Rice 10 km 27.8
  -> 11.6 s, the same hash of every cell value; a parallel job cancelled
  at 94 % and the next = one worker's
- THE MODEL of the coverage = the page's choice (`m.sig.model`; the user
  expected the map to follow it - it was P.1812 only at first): p1812 (all
  of the above); fs = free space (32.45 + 20 log f + 20 log of the slant
  distance, as the link's) + the clutter; itm = Longley-Rice per cell
  (`itmCell`) + the clutter - the picker says "much slower"
  (`PL_MODEL_OPTS`). fs skips the terrain work (~3.5 s for 12 km in node).
  Loss 65534 = outside the model's range (ITM: an antenna > 3000 m above
  its ground) - transparent, the hover says so
- ITM PER CELL (`itmCell`, model 'itm'): ITM 1.2.2 point to point on the
  ray's own points (the site, the points added so far, the cell). Their
  spacing grows ring by ring, so each of ITM's steps in profile INDICES is
  taken in DISTANCE - and reduces to NTIA's exactly on an evenly spaced
  profile: hs = the mean of the middle 80 % of the points (index-based as
  ITM; -> Ns -> ITM's own Earth radius, per cell), the horizons over the
  interior points, delta h resampled by distance (p10 from the fractional
  point-index span), LinearLeastSquaresFit with trapezoid weights in
  distance (= ITM's half-weighted ends). The shared code: `itmQuickRest`
  (QuickPfl after the horizons), `itmDhFit` + `itmSelect` (quickselect),
  `itmZg`, `itmLongleyRice`, `itmVariability` (median, mdvar 12 - as the
  link's). Incremental per ray (`itmAdd`): prefix sums (hs, the fits:
  O(1)); the TX horizon = the upper envelope of the lines (z - zt) / s - c
  s in the cell's curvature c = 1 / 2 ae (exact for each cell's own c, a
  binary search); the RX horizon = the hull of (s, z - cRef s^2) (cRef from
  the site's Ns) + a climb along it in the cell's c (they differ only on
  near ties). Delta h per cell (`itmDhRay`: <= 245 points; each one's
  neighbours from the runs of even spacing - a ring's - not a search) -
  about half of ITM's time. Reusing a ray's last delta h while its window
  moved < 2 % halved that but put single cells up to ~5 dB off: rejected.
  Checked (tools/pathloss_validate.mjs): the distance-based definitions
  (`itmRay`, plain loops) = NTIA's itmP2P on 300 random evenly spaced
  profiles within 1e-8 dB; the sweep = itmRay on every recorded ray sample
  within 1e-8 dB; itmRay on the ray's own uneven points vs itmP2P on them
  resampled every 10 m: median 0.03, max 0.22 dB; a link vs its coverage
  cell: median 0.05, p90 0.1 dB. Time: ~6x P.1812 (node on the Pi 5, the
  same 1.94 M cells: 47 vs 7.8 s; delta h ~40 % - its resampling, the fit,
  two quickselects -, LongleyRice + variability ~25 %) - the model's
  tooltip says so. Tried for delta h: a binary search per point, then the
  runs (2x), no closures in the loop, a top / bottom-k buffer instead of
  quickselect (slower: terrain residuals come in rising / falling runs)
- Page: `plState` (globals block; localStorage `kraken_pl`, rebuilt field by
  field): f MHz, ptx W, gt / gr dBi, lt / lr dB, bw kHz, nf, snr (->
  sensitivity = -174 + 10 log BW + NF + SNR), model (p1812 | itm | fs -
  the map's and the link's budget; the strip lists all three), polT /
  polR + angT / angR (the TX / RX antennas' polarization, see below; an
  old single `pol` 0 / 1 becomes both antennas' h / v), N0 (301:
  troposcatter + ITM), climate + ground (ITM, `PL_GROUNDS`), envTx / envRx
  (P.2108 types, `PL_ENVS`: the transmitter's / receiver's surroundings,
  whichever end that is), fade (auto = Rician, K per cell from the path,
  see FADING | rician = K everywhere | rayleigh | none), K dB (a clear
  path's), rel % (the wanted reliability), sigL
  (null = P.1546-6 (12): K + 1.3 log f, K 0.5 open / a receiver in the air,
  1.0 an antenna up near the clutter, 1.2 one in it - of the RECEIVER: the
  site when it is RX, else the target, `plSiteSigma`), set / prof (folds),
  link {site (a viewshed item id), lat, lon, name, mk}. What each setting
  redoes (`PL_REDO`): S = the sites' signal jobs (f, the TX antenna's
  polarization - the models' h / v -, N0, clutter, model; climate + ground
  only with ITM - they are in `plSigMsg`, so in `vsKey`: a job runs only
  where the key changed), L = the link; everything else (the RX antenna's
  polarization, fade, K too) only repaints from the losses + `fad` kept (`vsRepaint` ->
  `plPaintSig`). The viewshed's target height / reference are shared
  (`vsSetTgt` / `vsSetRef` call `vsSigChanged`). The REFRACTION is not:
  `vsK()` = 4/3 in the signal mode (the user's call: its models are for
  radio waves - light / no bending made P.1812's effective Earth radius,
  the line of sight edge / fading choice, the link's profile and the
  air-to-ground clutter angle inconsistent; ITM always took its own from
  N0), the stored choice (`vsState.k`, kept) in the line of sight mode -
  in the job key, the job, the link (`plLinkMsg`); the selector shows
  "Radio (k = 4/3)" disabled in the signal mode (t_kref.py)
- POLARIZATION (`plPolLoss(ends)`, `plPolEnds(it)` / `plLinkPolEnds(e)`,
  `plWavePol`): each antenna 'v' / 'h' / 'c' (circular) / 'a' (linear at
  angT / angR degrees from horizontal). The mismatch relative to matched
  antennas (the models are for those): the share co of the power the RX
  antenna takes - two linear cos^2 of the angle between, linear /
  circular 1/2, circular / circular 1 (the same hand assumed) - with
  depolarization x: co (1 - x) + (1 - co) x, over (1 - x). x = the
  antennas' own (XPD 20 dB, `PL_XPD_ANT`) and each END's clutter
  (`PL_XPD` by the Clutter type: none / water none, open 15, suburban 10,
  urban / forest 7, dense urban 5 dB - mobile radio measurements, 3GPP
  38.901's scattered rays 7-12 dB), fully for an antenna down in it, x (1
  - (h / R)^2) up to P.2108's clutter height R (`PL_ENV_R`), none above it
  or in the air (`plDepol`), combined as mixings in turn. The wave keeps
  its polarization along the path (direct, reflected, diffracted,
  troposcatter: forward, in the path's plane) - scattering off the clutter
  mixes it, so the figure is the SAME over the whole map. Until
  2026-10-12 XPD was 20 dB in sight / 8 dB beyond by the cell's line of
  sight: the user's 320 m TX over the sea, H TX / V RX, fell to 79 % at
  the 87 km horizon and jumped back to 96 % just past it (~13 dB less
  mismatch); scratch t_seapol.mjs / t_seapol2.mjs. The propagation models'
  own polarization (P.1812 / ITM take h or v) = the TX antenna's (a
  slanted one: the nearer, 45° horizontal; circular: vertical). Shown
  under the model ("matched" / "mismatch x dB": the link's ends, else the
  first transmitter's - `plPolNow`), in the hover ("+ polarization x dB")
  and as a budget row in the strip
- FADING (`plFading(terr, dts)`, `plKeff`): auto = Rician whose K (the
  setting: a clear path's) follows the path, continuously - TERRAIN in
  the way lowers it dB for dB by its knife-edge loss beyond the smooth
  Earth's (Lbull - Lbulls: the steady path dims behind a hill, scattered
  power still reaches round it; the Earth's bulge over open sea / flat
  land adds none), TROPOSCATTER (Rayleigh) taking over far beyond the
  horizon: K = Kt (1 - w) / (1 + Kt w), w = 1 / (1 + 10^((Lbs - Lbd) /
  10)). From P.1812 with either terrain model (the cell's `fad`, the
  link's `p1812`); free space: K. Until 2026-10-12 auto was Rician in
  sight / Rayleigh beyond - a ~19-point step down at the same horizon
  once the polarization step was gone. Shown in the hover ("Rician K x
  dB") and the strip ("Rician K x dB (6 dB on a clear path; terrain in
  the way y dB, troposcatter z %)", "≈ Rayleigh" below -10 dB)
- LABELS (`vsRender`, BOTH modes - the user's call 2026-10-12: line of
  sight said "Antenna" / "Target"): a site row's "TX antenna" / "RX
  antenna" by its role; the target -> the other end's ("RX antenna" for
  TX sites, "TX antenna" for RX sites, "TX / RX antenna" when the shown
  sites have both, or none). No TX / RX tag on the rows any more (removed
  2026-10-13: TX antennas are placed by right-clicking)
- Reliability: `plOutage(m, sigma, kind, K)` = integral of phi(z)
  F_fade(sigma z - m dB) over +-6 sigma (121-point trapezoid; the Rician
  power CDF tabulated ONCE per K, `plFadeTab`: the power pdf with I0e - A&S
  9.8.1 / 9.8.2 - integrated by Simpson every 0.02 dB from -60 to +20 dB,
  within 1e-4 of a fine reference up to K 25 dB, ~7x faster than the old
  per-call Simpson over the envelope; 'none' = Phi(-m / sigma) exactly; K
  >= 30 dB = none) -> `plNeed(rel, sigma, kind, K)` (bisection, cached).
  Per cell: `plRelLut(sigma, kind, K)` (the reliability every 0.25 dB of
  margin from -50 to +70 dB, linear between), auto: `plRelSet(sigma)` = a
  table per K level (Rayleigh at -20 dB, Rician up to the setting <= 30 dB
  in <= 1 dB steps, ~0.3 s the first time per sigma on the Pi) + idx[fad]
  (65536 entries, per K setting) -> `plRelK` interpolates between the two
  levels around the cell's K: within 2e-4 of the integral at its exact K.
  Checked: closed forms (Rayleigh 90 % = 9.773 dB, lognormal 1.2816 /
  2.3263 sigma) and Monte Carlo (Rayleigh / Rician 6 dB with sigma ~4 dB:
  10.03 +- 0.02 % outage at the 90 % margin)
- The viewshed side (👁 VIEWSHED, hooks only): `vsKey` = the geometry +
  `|` + JSON `plSigMsg(it)` in signal mode; `vsResOk` also accepts a signal
  result in line of sight mode (its `need` / `ground` are sweep()'s - mode
  switches compute nothing); `vsPump` posts `sig` in signal mode;
  `vsOnWorker` keeps `loss`; `vsPaint` hands a signal result in signal mode
  to `plPaintSig`; `vsDraw` draws `plDrawSig` instead of the tinted
  per-site layers (the radius circles + dots stay); `vsHoverRender` prefixes
  `plHoverSig` (TX / RX, reliability %, dBm, loss). Items: `role` 'tx' |
  'rx' (old saved items: the station rx, others tx), `vsSetRole` (a new job
  only when the two ends' clutter differs; always a repaint - the
  receiver's sigma). `vsSetMode` (persisted), `vsSigChanged` (a stale
  running job is cancelled, the next pumped)
- Paint: `plPaintSig` -> each ring's canvas pixel = ABGR with R = 1 + the
  reliability x 254, G = 255 in sight, A 255 (no data / the target under the
  ground: transparent); res.vis = the area >= the wanted reliability ("%
  covered"). `plDrawSig` -> `plComposite`: each shown site's rings coarse ->
  fine onto `plOffS`, then onto `plComp` with 'lighten' (= the max per
  channel: R the BEST site's reliability, G in sight of ANY), getImageData,
  per screen pixel `plColLut`: a GRADIENT (`PL_STOPS` 0 / 25 /
  50 / 75 / 90 / 95 / 99 %, red - orange - yellow - greens, linear in RGB
  between - `plRelRgb`; the user's calls: first hard bands, then every
  place coloured (below 25 % red, not blank), then blended - no hard
  edges); EVERY place with data within the radius is coloured; below the
  wanted reliability the alpha ramps 255 -> 105 over the 10 % below it
  (a step was a hard edge too); `plComp` is drawn at 0.6. The LINE OF
  SIGHT EDGE on its own layer `plLine`, drawn over it at full strength:
  pixels in sight next to one out of sight AND the other way round (2
  device px; dilated once more at devicePixelRatio >= 1.5 = 4, i.e. 2 CSS
  px), black alpha 0xd9 (the user's call, 2026-10-12: it was 1 px faint
  white inside the colours, hard to see). `plState.losEdge` (default OFF -
  the user's call; `kraken_pl` - its first version `los` defaulted on and
  was saved with everything, so that key isn't read): the "Line of sight
  edge" tick on the Wanted reliability row
  (always shown), a redraw only - in the composite's key, not the jobs';
  the legend names it only while on (`.pl-ll` swatch). The legend: a gradient bar with the stops
  evenly spaced + their labels (`.pl-leg-g`, `.pl-leg-t`),
  putImageData, drawn at alpha 0.6. Cached by `plCompKey` (view, size,
  `plSigVer` - bumped by every paint -, rel, the sites): pointer moves
  don't redo it. Checked in the browser: every sampled pixel's colour = the
  band of the best site's reliability from the cells (2000+ pixels, one and
  two sites; faint pixels read back +-1 per channel - premultiplied canvas)
- The link: `plState.link` = a site + a place; `plEnds()` -> {tx, rx} by the
  site's role (the place at the target height / reference, so the link =
  the coverage cell there: 146.9 vs 146.6 dB in the test). Made by the
  right-click menu ("📶 Path loss analysis from TX 1 to here": the
  selected TX antenna, else the first placed - none: no item; `plMenuItem`
  / `plLinkLabel` / `plLinkTip` via `plLinkSite` - until 2026-10-12 "📶
  Link: TX here ↔ “Station”", then "📶 Signal from here to …" / "… to
  here" + "Add RX antenna here", reconciled by the user 2026-10-13), a
  marker (menu + "📶 Path loss from TX 1" in its panel, `plLinkShort`,
  `plLinkMarker`), an OSM mast (the same, `plLinkMast` - at the target
  height; "👁 Line of sight" / "📶 Add TX antenna" adds the mast as a TX antenna
  at its own height). The marker panel and the mast popup re-render on
  every map draw, so their words follow the mode and the picked site. The
  menu wraps long items (max 280 px) instead of cutting them; the site = `plPickSite()`:
  the selected TX antenna, else the first one placed (never the station or
  an RX site; none: null). Turns
  the viewshed on in signal mode. Only in signal mode (`plLinkOn`). The
  place: selected + dragged (`mapSelectObj('pl:far')`, drag kind 'pl' ->
  `plMoveFar` after a 0.15 s pause); the site's point moves the link with
  it. ✕ (`plLinkRemove`; removing the site - Delete on the selected place too -
  removes it too). `runLink` on its own worker (`plWorker`, see LINK above)
- UI: the signal block `#map-vs-sig` in the viewshed panel (class
  `vs-sig`: shown only by `.map-vs.sig`; `vs-los` the reverse): Freq + TX
  power, Model (and ITM's climate / ground when it is Longley-Rice), the
  TX / RX antennas' Polarization (+ an angle field for 'custom', + the
  mismatch) - not folded: they change the map -, the rest folded
  under "Antennas, receiver, channel" (gains, cables, receiver, N0,
  clutter TX / RX, fading + K, sigma_L), "Wanted reliability", the legend
  (`plLegendHtml`, naming the model); `#map-vs-link` (the link in one line + Show / Hide). `plRenderSig`
  runs from `vsRender`; `plRender` = `vsRender`. Strip `#map-plk` at the
  bottom of the map (`.map-pane.pl-open` lifts the DF panel, the marker
  panel and the status line): TX → RX, the path, the three models (a row
  click = the budget's and the map's model), clutter (which end,
  air-to-ground), polarization (the mismatch, TX → RX), received (dBm,
  µV), margin + reliability, ITM at the reliability, the heights above sea
  level, terrain zoom / tiles / ms; the profile canvas (terrain + the
  Earth bulge over the chord, direct ray, F1 + 0.6 F1, Bullington's
  construction, sea blue, TX / RX in their colours; under the pointer:
  distance / ground / clearance, and a dot on the map). The chart's F1
  radius is clamped at >= 0: Dm * i / n rounds a hair past Dm at some
  lengths, the sqrt went NaN and took the whole y scale with it (the
  user's "the profile sometimes doesn't draw after a move" - reproduced
  by t_drag.py, real mouse drags, deterministic per position). On the map
  (`plDraw`, signal mode only, after the rings, before the markers): a
  TX / RX tag beside each shown site, the link's great circle coloured by
  its reliability band (dashed while computing), the place's TX / RX badge,
  the edge ▲
- Tests: committed `tools/pathloss_validate.mjs` (node 20, from
  kraken_doa_v2: 1-3 the references, cloned from GitHub into --refs; 4-5
  synthetic terrain served as generated terrarium PNGs through fetch -
  offline, the whole decode / clean path; 6 `--net` real terrain).
  Scratch (job 26b3d7af): t_itm_rand.mjs (vs the compiled NTIA C++),
  t_cov.mjs, t_link.mjs, t_need.py (signal vs line of sight `need`: 0
  cells differ), t_drag.py (real mouse drags of the link's place, random
  speeds / pauses: the profile drawn for the current link whenever the
  worker is idle - 2 of 40 blank before the F1 clamp, 0 of 80 after), and
  t_uni_ui.py - 91 checks (incl. TX antennas only + the one path loss
  item, line of sight = the station's only, no menu items outside signal;
  the labels by role;
  polarization: RX-only a repaint, TX h / v
  a job, 60° = cos^2 30°, circular, urban receivers less mismatch - each
  vs an independent formula, one figure; the link's budget row; the
  fading: tables vs the integral, K per cell 6 dB clear / lower behind
  terrain, hover + strip K, the K setting a repaint only; the edge line:
  off by default, black, >= 2 px, switched on / off - no job, the same colours, the
  legend, kept over a reload; the
  TX / RX antenna labels; an old `pol` / a saved 'hata' migrating; t_dpr2.py:
  the line 2 / 4 device px at devicePixelRatio 1 / 2) in headless Chromium on the
  page served statically WITH a UTF-8 charset (no receiver; the station set
  from the test): no Path loss button, line of sight as before, signal
  (job, status, legend, roles), the composite (line pixels, coverage out of
  sight, per-pixel colour = band of the best reliability at zoom 16 - at
  zoom 10 a pixel averages ~16x16 fine cells), the reliability table,
  hover, repaint-only power / wanted reliability, role swap (same clutter:
  no job, sigma changes; other clutter: a job), a second site (best server,
  more coverage), the menu's words ("📶 Signal from “Point 1” to here"
  with a TX site selected, "📶 Signal from here to “Station”" + its tooltip,
  "📶 Add TX antenna here" / line of sight "👁 Line of sight
  from a TX antenna here", the buttons), link + strip +
  budget + link = coverage cell, chart + hover, model row, drag the place
  (no coverage job), Esc, ⇄, line of sight again (no job, strip hidden) and
  back, viewshed off / on, ✕, a link from line of sight mode, the selected
  site, a removed site's link, reload, damaged localStorage, no exceptions

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
  / Dn), Lobe / Lines / Heat toggles (`mapState.rdf*`, localStorage;
  Lobe = the lobe shapes, Lines = EVERY bearing line: the lobes' peak lines,
  a talker's earlier bearings, the driving records' lines - the peak lines
  used to come with Lobe, so Lines did nothing at a fixed station),
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

**DoA per talker (P25 / DMR / NXDN unit IDs, D-STAR callsigns, ADS-B aircraft, AIS ships, DME beacons, ISM devices, APRS stations, radiosondes, TETRA cells, MPT1327 sites, VOR / ILS idents, pulse radars, 1030 MHz interrogators, UAT aircraft / ground stations, FLARM / OGN aircraft; `src/talker_doa.cpp`, `plugins/{p25,dmr,nxdn,dstar,adsb,ais,dme,ism,aprs,radiosonde,tetra,mpt1327,ils,radar,ssr1030,uat,flarm}`, 🗺 Map 📡 DF panel):**
- RULE (ID separation, the user's 2026-10-15): every decoder whose signal
  carries a coded ID per TRANSMITTER reports it as a talker - a core part
  of every such decoder, new ones included. How + how to test:
  plugins/SDK.md *ID separation*. Not applicable (README says why): pocsag
  (the RIC is the recipient; simulcast transmitters send identical
  signals), v23_telemetry (proprietary payload, no known address), dtv
  (SFN copies are identical - separated by delay instead), radiosonde's
  iMet-4 (sends no serial), glide slopes / marker beacons (no ident)
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
  published or not (publish hold), but none while MUSIC is
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
  frequency move > 10 kHz resets it. Talkers kept 30 min after last heard
  (packet talkers: below), MAX_TALKERS 500 (the least recently heard go
  first). Lock order: the tap runs under the MUSIC lock and takes ours;
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
  null, doa, conf, frames]]}]`, and `tfull`: false = only the talkers
  heard in the last TALKERS_RECENT_MS 10 s (every push but each
  TALKERS_FULL_EVERY 5th - 2.5 s - and the one after GET_RDF; an ISM
  receiver keeps hundreds of devices for 24 h, all of them twice a second
  was a few hundred kB/s per page). Those partial pushes are sent as
  `{"rdf_part":..}` (same fields), the full ones as `{"rdf":..}`: a page
  loaded before partial pushes existed - an open tab reconnects by itself
  after the stack restarts, keeping its old code - took each partial push
  for the whole list and the DF list flashed between one device and all of
  them; it ignores `rdf_part` (full list every 2.5 s, lobes 0.4 Hz until
  reloaded). `rdfOnStatus(m, partial)` keeps the others from the
  previous pushes, their ages (age, tx_age, hist) moved on by the time
  since, `active` false. `rdf_grid` has `tid`. `RDF_RESET:<vfo>`
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
- Order (`rdfTalkersListed`, list + dropdown), per kind (`rdfKinds`): the
  column heading the user clicked (`th[data-sort]` -> `rdfSetSort`, picked
  by press + release like the rows; again = reversed, counts start with
  the most; `mapState.rdfSort[kind] = [col, 1|-1]`, localStorage; ▲ / ▼ on
  the heading), else `RDF_SORT_DEF`: radios newest first; packet talkers
  (aircraft / ships) NEAREST first by their reported position (Dist
  column; needs the decoder's Plot on map), those without one by name -
  newest first reshuffled a busy ADS-B list with every packet; DME beacons
  and ISM devices by name (newest first moved the device the user was about
  to click on every transmission, and devices heard in the same second
  swapped places on every push: there was no tie-break). Ties always go by
  name, ID, VFO; empty values last. Radios stay above packet talkers. Name
  and reading cells are clipped (130 / 80 px, `.clip`, full text as the
  tooltip) so Bearing / Heard / Tx fit the 380 px panel. The list is only
  built while the panel is open. Aircraft are named by callsign (`rdfShortName` / `rdfAirParts`:
  the ADS-B talker label is "CALLSIGN · 35000 ft"): the lobe label, the
  list's first column (bold; ICAO second), the dropdown "CALLSIGN (ICAO)";
  the ICAO until a callsign is known. A row is picked by press + release
  (`rdfTlDown` / `rdfTlUp`, the row remembered at the press, <= 8 px
  movement), not `click`: the list is rebuilt several times a second and a
  click whose press and release hit different row elements was lost
- On the map: the talker whose lobe is shown alone (`rdfTrackedPt()`: one
  picked in the list / ID selector, with a map point - its decoder's Plot on
  map) gets a 19 px ring in its lobe's colour; the point whose popup is open
  (also a decoder-table row click, `digTableRowClick`) a white 14 px one -
  both with a dark edge. A decoder point's popup (`mapPointPopupHtml`) has a
  "📡 DoA" button in its header when the point is a talker of the DF status
  (`rdfPopupTalker`: same VFO + id; coherent only): `rdfTrackBtn` picks it
  (switches the panel's VFO select to its VFO unless All, opens the panel,
  Lobe on), again = Whole signal; "DoA ✓" while picked. It acts on
  pointerdown: the popup is rebuilt every second ("Updated N s ago")
- Packets (ADS-B per aircraft; `kp::Talker::packet`, wire flags field "p",
  times sent with 7 decimals): the plugin reports every accepted message
  of a confirmed aircraft (`talker_packet` in plugins/adsb: start = the
  first preamble pulse - mag_ index + timing phase / 5, mapped through
  `abs0_` / `blk_abs_` to host.time() -, end = + (8 + bits) us; offline
  check on a synthetic feed: start within 0.5 sample of the truth, sd
  0.16). Engine: packet -> one closed TalkerSpan {packet} (vfo_pos rounds;
  <= 50 ms). TalkerDoa: while packets come (`want_blocks_`, off after
  PACKET_IDLE_MS 30 s without, ring freed) the pipeline calls
  `add_block()` for every block except while heimdall calibrates / the
  retune hold runs (a gap: packets across it are dropped) - NOT gated by the
  FFT squelch (the decoder confirms each packet itself; until 2026-10-13 it
  sat inside MUSIC's squelch gate, and on a 2.4 MHz ISM VFO with the FFT
  squelch on the averaged spectrum opened late or never for a short burst:
  devices decoded at once took minutes to reach the DF list, or never did)
  -> per CHUNK (16) stream samples the
  upper-triangle covariance into a ring of RING_S 1.5 s (5 ch at 2.4 MHz:
  ~27 MB, ~8 % of a Pi 5 core); a packet = sum of the chunks entirely
  inside it (>= 2), trace-normalised; waits PACKET_HOLD_MS 200 for an
  overlapping packet of another talker (both dropped). Per aircraft
  R = sum of packets weighted exp(-dt / PACKET_TAU_S 1.5 s) in stream
  time, MUSIC at most every PACKET_RECOMPUTE_MS 1 s, `hist` every 10 s
  (its bearing track), tx = packets, tx_frames = packets in the average;
  forgotten after PACKET_TTL_MS 2 min (or 30 x avg_s, or the plugin's
  `kp::Talker::keep_s` - wire TALKER field 13, `TalkerSpan::keep_s`,
  `TalkerInfo::keep_s`; ism: its "Keep devices" option, 24 h). No mobile DF
  records. rdf_mapper: `pk:true`, lobe turned with the heading at
  `spec_ms`, no lobe 60 s after its last packet (a talker with `keep_s` - ISM devices - keeps its lobe as long as it is kept: picked in the DF list hours later it still shows; "Every ..." draws only those heard in the last 60 s). UI: ✈ rows, "Every
  aircraft", Δ pos column + selected-aircraft line = DF bearing minus the
  bearing to its ADS-B map point (needs the decoder's Plot on map;
  `rdfPosCheck`), "ADS-B check" = median / quartiles of that over the
  aircraft heard < 30 s (`rdfPosSummary`) - a steady offset = array
  mounting / heading error.
  What became of a VFO's packets is counted (`TalkerDoa::packet_stats()`,
  `PacketStats`: in / ok / late - its samples had left the ring / gap - the
  ring started after it / early / overlap / other, the report lag - stream
  past the packet's end when it came - last / median / max of the last
  minute, the ring's length, the latest 20 drops with id, reason, lag) ->
  the DF status per VFO `pk` -> the 📡 DF panel's "Dn: N decoded, M with a
  bearing · k too late (reported up to x s after, y s kept), ..." line
  (`rdfPkLine`, the drops in its tooltip). Added 2026-10-13: ISM devices
  decoded at once that never reached the DF list could not be told apart
  (every drop was silent).
- Channel filtering (AIS): `kp::Talker::freq_hz` / `bw_hz` (packet's
  centre in the plugin's band - the engine maps it to the VFO's: + afc_hz_,
  mirrored with invert), `avg_s` (averaging; also TTL = max(2 min, 30
  avg_s) and the lobe kept max(60 s, 15 avg_s)). Wire TALKER fields 7-9.
  TalkerDoa keeps the raw samples while the VFO rate <= RAW_MAX_RATE (500
  kHz; 5 antennas at 100 kHz ~ 6 MB for RING_S) and `packet_cov_raw`
  mixes each antenna by -freq, Hamming-windowed-sinc low-pass (cut-off
  bw / 2, ~3 fs / bw taps, run-in from the samples before the packet),
  covariance over exactly the packet; overlapping packets are only dropped
  when on the same channel. Above 500 kHz the chunk ring as before (ADS-B,
  DME; freq ignored) - unless a packet named a channel <= WIDE_CHAN_MAX_BW
  250 kHz (the ism plugin's devices in 2.4 MHz): `want_wide_` then keeps the
  samples themselves as 16-bit (`wide_raw_`, x WIDE_SCALE 8192, WIDE_RING_S
  2.5 s: 120 MB at 2.4 MHz with 5 antennas; not released after
  PACKET_IDLE_MS - a fob / door sensor after a quiet spell lost its
  transmission - only with the decoder. It was 1 s: the ism plugin reports a
  device when its burst group ends, up to MAX_LEN_S 1.2 s after the device's
  rows when other activity keeps the group open, + the queue - live 1.3-1.8
  s for a weather station on the tuner centre, 8 of 19 transmissions without
  a bearing, seen with the drop counters below) and `packet_cov_wide` applies the
  same channel filter as complex taps h[t] e^(-iwt) (mixing first differs
  only by a phase common to all antennas) at <= WIDE_MAX_OUT 1024 points
  spread over the packet, at most one per fs / (3 bw) - the filter at all
  576k samples of a 0.24 s packet was ~1 G multiply-adds; ~2 M now; without
  a channel (digital squelch) <= 16384 samples spread over the piece. The
  16-bit ring costs less per sample than the chunk ring (3.6 vs 7.8 % of a
  Pi 5 core, scratchpad bench/ring_bench). Why (scratchpad e2e/ab.sh, 7
  devices across the 2.4 MHz, transmissions overlapping): filtered 7/7
  bearings exact in every run, whole band 5-6/7 (Schrader read the
  LaCrosse's 90 deg, a fob 96 deg) at the same kraken_doa CPU (~130 %).
  PACKET_IDLE_MS 5 min; MAX_PACKET_S 0.25 (5 AIS slots).
  Status `pl` = the talker's plugin (UI: ✈ / ⛴ / 📡 beacon for `dme` / 📟
  device for `ism` - `rdfKind`; "Every aircraft / ship / beacon / device",
  "Position check"; beacons and devices have no reported position: no Dist
  / Δ pos columns, no Plot-on-map hint; beacons counted in pulse pairs,
  devices in transmissions; both listed by name. A bearing whose lobe has
  gone - 60 s after the last packet - is the compass bearing from the
  station heading when the station is static (`rdfStaticCompass`), else
  the DoA plot's angle "arr")
- ID separation added 2026-10-15 (packets, the channel always named):
  - APRS (`plugins/aprs`): a packet per frame, id = the frame's RF
    TRANSMITTER: the source, or the last path entry with the H bit ("*") when
    it is a callsign (a digipeater that inserted its call); repeated under a
    generic alias only (WIDEn / RELAY / TRACE ..., `generic_alias`) = no
    talker. Label = packet type / "digipeats SRC". Samples: the frame's last
    <= 0.24 s, 3 ms short of the decoded end (a 1200 bit/s frame is 0.15-2
    s); 0 Hz / 16 kHz, avg_s 30, keep_s 1 h
  - Radiosonde: a packet per frame, id = type + serial (= its map point /
    table key), label subtype + altitude; the frame's FIRST <= 0.24 s from
    its sync (the RS41's decoded length is the extended frame's 0.86 s - the
    end may already be another signal: the first version took the end and
    one RS41 frame read a DFM that followed at 140 deg -> 133.9 instead of
    30); bw 24 kHz for M10 / M20 / iMet-4 types, else 15 kHz; avg_s 10,
    keep_s 3 h. iMet-4: no talker (no serial)
  - TETRA (`TetraReceiver::cell_packet`): the downlink is the base
    station's - id "MCC-MNC CCn" (e.g. "530-1 CC12"; the colour code seeds
    the scrambling, so another co-channel cell's blocks fail the CRC here),
    label "LA n · carrier MHz" (from SYSINFO); every slot with a block that
    passed its CRC (BSCH, BNCH, SCH/F, SCH/HD) = one packet of the whole
    255-symbol slot, the RRC front end's 24-sample delay taken off
    (`input_delay`); 0 Hz / 25 kHz, avg_s 10, keep_s 30 min. Co-channel
    cells (frequency reuse) heard in turn keep their own bearings
  - MPT1327: each control channel slot with a good CCSC = a packet of the
    SITE, id "SYS hhhh", CCSC + address codeword (107 ms) 2 ms inside both
    ends; 12.5 kHz. Mobiles' return channel isn't decoded (their idents:
    not implemented); traffic channels name no transmitter
  - ILS / VOR (`plugins/ils`): id "<ident> LOC|VOR" while the ident was
    heard within 30 s on this carrier (within 50 Hz of where it was heard -
    another navaid on the channel doesn't inherit it); a packet = the middle
    0.24 s of every analysed block of the recognised navaid, at the
    carrier's offset, LOC 4 kHz / VOR 22 kHz wide, avg_s 10, keep_s 1 h
  - `dig::RxContext::packet` (dig_common.hpp): packets for the lib plugins
    in receiver sample indexes, mapped by `Bridge::clock(rate)` (tetra,
    mpt1327)
  - Pulse radars (`plugins/radar`, task 2 of the batch): no coded ID - a
    radar is told apart by its SIGNATURE (pulse kinds + PRI pattern), id =
    its frequency to 0.1 MHz; packets = its main-beam pulses only (unclipped,
    within 15 dB of the scan's peak, in a burst that dwells near the peak
    like its main beam: a lit reflector stays out), each with its own
    channel, avg_s 30, keep_s 1 h - see *Pulse radar*
  - 1030 MHz interrogators (`plugins/ssr1030`, task 3 of the batch): an
    SSR by its periodic all-call train + its checked UF11 code ("SSR II
    10"), a fixed interrogator by its steady level ("Fixed #n"), an ACAS by
    whisper-shout / its checked UF16 broadcast address ("ACAS 7C4A51");
    packets = each interrogation +-10 us (an SSR's only in its main beam),
    the whole band, avg_s 30 / 10 / 5, keep_s 1 h (ACAS 10 min) - see *1030
    MHz interrogations*. UI kind `interrogator` (rdfIcon: ✈ when the id
    starts with "ACAS", else 🛰; "Interrogator / Type · scan · PRF /
    Interr.", by name, unit "interrogation")
  - UAT (`plugins/uat`, task 4 of the batch): every ADS-B message an
    aircraft / vehicle / beacon sends itself (address qualifier 0 / 1 / 4 /
    5) = a packet of its address (sync .. last parity bit, the whole band,
    avg_s 5, keep_s 10 min, its position at the packet's time when NACp >=
    8); TIS-B / ADS-R messages are sent BY a ground station - packets of
    "GS site N" (the TIS-B site ID), like its uplink frames (avg_s 20, keep_s
    30 min, the station's uplinked position) - never of the aircraft they
    describe. UI: aircraft kind ✈ like adsb's, ground stations kind `gs` 🗼
    (`rdfKind`: pl uat + tid "GS ..."; "Station / UAT ground station · slot
    / Frames", position columns, by name) - see *UAT*
  - FLARM / OGN (`plugins/flarm`, task 4): every packet an aircraft sends
    itself = a packet of its address ("FLR DD1234", "ICA C81A2B", "OGN
    3C5A21", "RND / ANO ..."), its first sync chip .. last data chip, on its
    channel (freq_hz = the burst's measured carrier, bw_hz 160 kHz - aircraft
    transmitting at once on different channels of the VFO keep their own
    bearings), avg_s 10, keep_s 1 h; an OGN packet with a relay count names
    nobody who sent it: no talker. FLARM's payload is encrypted (unpublished
    keys): ID + bearing only. UI kind `flarm` (✈, 🪂 paraglider / hang glider
    / parachute, 🚁, 🎈 by the label's aircraft type; "Aircraft / Reg ·
    altitude · type / Pkts", position columns, nearest first;
    `RDF_KIND_WORD` names it "aircraft" in "Every ..." / counts) - see
    *FLARM / OGN*
  - UI (kraken_doa.html): `RDF_PK_KIND` plugin -> kind (station 📻 aprs,
    sonde 🎈, site 🗼 tetra + mpt1327, navaid 🧭 ils, radar 📡 radar - list
    "Radar / PRF · pulse · scan / Pulses", by name; ship / aircraft /
    beacon / device as before), `RDF_PK_UNIT` (slot, frame, block, pulse ...),
    `RDF_POS_KINDS` (aircraft, ship, station, sonde: Dist / Δ pos columns,
    position check, "Plot on map" hint), list headings, `RDF_SORT_DEF`
    (stations / sondes nearest first, sites / navaids by name). The AIS
    base-station / aid-to-navigation icons only for ais talkers (an APRS
    point is no ⚓)
  - D-STAR fix found by the scene test: a new header within two
    superframes of a transmission that ended WITHOUT its end pattern (cut
    off / lost) was taken as a false sync inside the old one - the new
    transmission came up as a late entry without a talker. A header 756
    bits before a data sync now always starts a new transmission
    (`tx_sync`, `new_hdr`). Also: an end pattern in noise long after
    (> 2 superframes) no longer logs "End of transmission"
- Tests 2026-10-15 (scratch t1/; generators in the repo, scenes from
  `plugins/sdk/tools/uca_scene.py`, 5-element UCA r 0.12 m):
  offline `decoder --file scene.cf32 --channels 5 --uca 0.12` (per-talker
  bearings in the offline host, `talker_bearings` in plugin_host.cpp) and
  end to end through the real kraken_doa (scratch cwd, fake heimdall
  streaming the scene's 2.4 MHz cu8, DF status `doa`): every talker within
  0.1 deg offline and 0.0 deg end to end -
  APRS 3 transmitters (one a digipeater repeating another; an alias-only
  repeat gives no talker), AIS 4 ships on A / B with simultaneous A + B
  transmissions, D-STAR the real F1NSR (dsdcc dstar_f1zil_1) + 2 generated
  callsigns back to back (also with FFT squelch 10 dB / Digital squelch),
  NXDN the real unit 1203 (dsd-samples nxdn96_3) + 2 generated units,
  radiosonde 3 real sondes (radiosonde_auto_rx RS41 / DFM-09 / M20) one
  after the other, TETRA 2 co-channel cells, MPT1327 2 sites, VOR 2
  co-channel idents (offline only). kraken_doa ~50 % of a Pi 5 core in
  these runs (2.4 MHz, 5 antennas); plugins 0.4-3 %. Regression (old vs new binaries): p25 /
  dmr / dstar / nxdn on the dsd / dsdcc samples identical; the others only
  add their talker lines (+ the D-STAR fix). No live check: no AIS
  (162 MHz, 7 min) or APRS (144.575 MHz, 3 min) traffic at the station
- Not split: MPT1327 voice (analogue). On a repeater OUTPUT every ID's
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

**✈ Array calibration from ADS-B aircraft (`src/array_cal.cpp`, sidebar "🧪 Experimental" -> "✈ Array Calibration", coherent) - EXPERIMENTAL, on the back burner:**
- Status (2026-10-09): works on synthetic data; on the live array here (205 mm,
  1090 MHz, aircraft at 0-9 deg elevation) each aircraft's response matched
  the nominal array only ~0.56 (healthy ~0.95) - steady per aircraft but
  different for aircraft in nearly the same direction (multipath / the
  antennas at low elevation), so the 88 deg lookalike lobes of the 0.876
  lambda ring often won. Wiring order and radius checked fine. Constant
  per-element offsets helped only partly (main lobe right 43 -> 54 %,
  leave-one-aircraft-out). Known open issues: `line_of_sight()` judges an
  aircraft by its STRONGEST lobe, so aircraft whose lookalike lobe won are
  left out although they have a lobe at the truth (should test "a strong
  lobe within LOS_DEG of the truth"); Apply doesn't ask before using a
  result with no cross-check / a worse one. Recording + scripts:
  /home/krakenrf/acal_capture (cap8091 capture, analyze / models /
  stability / loo .py)
- What it fixes: what the noise source can't see - per-element cable delay
  (element 0 = 0), per-element x / y position error and a rotation of the
  whole array (mounting / station heading) - in PHYSICAL units, so it holds
  at every frequency (the phase of a delay scales with f; copying 1090 MHz
  phases elsewhere was worse than nothing in the simulation). At one
  frequency an antenna's own phase looks exactly like a delay and is absorbed
  into it (carried to other frequencies scaled, i.e. wrongly - small at lower
  ones); what the model can't explain is reported per element ("Unexpl.")
- Input: `kp::Talker::lat / lon / alt_m` on packets (wire TALKER fields
  10-12, `TalkerSpan::lat...`): the adsb plugin's `packet_position` = the
  aircraft's last position moved on with GS / track / vertical rate to the
  packet's start time (airborne, fix <= `POS_AT_MAX_S` 3 s old, a velocity
  beyond 0.1 s; GNSS altitude, else barometric). TalkerDoa commits a packet
  -> `CalPacket {id, R, lat, lon, alt}` (only while collecting, `take_cal`)
  -> the rdf sampler adds the station (static location, or GPS with a
  compass, standing still - `cal_state`; height from a 3D GPS fix, else 0)
  -> `array_cal::add()`: ECEF -> east / north / up = compass azimuth +
  elevation; steering angle = heading - array offset - azimuth (rdf's
  compass = heading - displayed angle); dominant eigenvector + eigenvalue
  ratio of R. Gates: >= 5 km, <= 450 km, 2..60 deg elevation (below 2 deg:
  ground / horizon traffic - live at Auckland Airport five taxiing aircraft
  that sent airborne messages at -200 ft, bearing 246 deg, all read 161 deg
  and bent the fit: 16 deg unexplained, check worse), ratio >= 10, one packet
  per aircraft per 500 ms; 36 compass sectors of max 400 packets
  (then a random one replaced). The array: `note_array()` with
  `MUSICProcessor::nominalPositions()` (UCA / CUSTOM; ULA = not
  calibratable) - other positions = a refit of the same packets, another
  element count / array offset / static heading = start over (they are baked
  into the packets)
- Line of sight (`line_of_sight()`, before the fit): each aircraft's median
  bearing error with the NOMINAL array at its own elevation; the consensus =
  the offset aircraft in the most 10-deg DIRECTIONS agree with (+-25 deg; then
  the most aircraft) - a reflection is consistent per place, so a busy
  airport's aircraft would out-vote a few good ones by count; aircraft more
  than `LOS_DEG` 25 off it are left out whole (`left_out` [{id ICAO, err,
  n}], `aircraft_used`). Per-packet weights can't catch them: their packets
  are consistently wrong
- Fit (worker thread, every 20 s with new packets, ~1 s at the 14400-packet
  cap on a Pi 5): MUSIC's model (`phases()` = k (x cos el cos th + y cos el
  sin th + z sin el) - 2 pi f tau, c = 3e8 as MUSIC) against each packet's
  eigenvector, per-packet common phase projected out; start = rotation
  search in 2 deg steps + per-element circular-mean phase as a delay; Gauss-
  Newton MAP (priors: rotation 30 deg, delay 2 ns, position 15 mm), Cauchy
  reweighting (multipath / wrong packets). Cross-check: fitted on half the
  aircraft, the other half's bearings (operational el = 0 steering, 1 deg
  grid + parabola on the eigenvector, aircraft below 20 deg) before / after.
  Warnings: < 6 of 12 30-deg sectors covered, > 15 deg unexplained, check
  worse
- In use: `apply()` saves `array_cal.json` (cwd - Docker volume; gitignored)
  and bumps `generation()`; `corrections(n, x, y)` gives dx / dy (rotation
  included when `rotation` is on) + tau, only while `enabled` and the
  nominal positions match the calibrated ones within 0.5 mm. MUSIC
  (`updateSteeringVectors` / `2D`: corrected positions + -2 pi f tau; the
  processing pass rebuilds them when `generation()` changed;
  `isCalibrationApplied()`) and the beamformer (`computeElementPositions`,
  `element_tau_`, FD-DAS per bin) use it. Not touched by RESET_SETTINGS
- Commands (`ARRAY_CAL` in `is_query_command`, broadcasts): `ARRAY_CAL_COLLECT:0|1`
  (not remembered across restarts; 0 = a last fit), `ARRAY_CAL_RESET`,
  `ARRAY_CAL_APPLY`, `ARRAY_CAL_USE:0|1`, `ARRAY_CAL_ROTATION:0|1`,
  `ARRAY_CAL_REMOVE`, `GET_ARRAY_CAL`. Status `{"array_cal":{collecting,
  state, samples, aircraft, last_s, rejected{near, far, low, high, weak},
  sectors[36], cap, note, array{n, ok}, fitting, min_samples, fit{ok, error,
  t, age_s, samples, aircraft, aircraft_used, left_out[], left_out_n, f_mhz, n, used, sectors, el, rot, tau_ps[],
  dx_mm[], dy_mm[], resid[], resid_all, gain_db[], cv, before / after{n, med,
  p90, wrong}, warn}, applied{...fit, enabled, rotation, match}}}` every 2 s
- UI (the "🧪 Experimental" sidebar section `#exp-section` at the bottom,
  just above Open Heimdall Server / Reset Settings - collapsed, coherent
  only; its header carries the calibration badge `#acal-badge`, "in use" /
  "collecting", so an active calibration shows while collapsed; block
  `#acal-section`, `acal*` in kraken_doa.html): the
  calibration in use (Use / Include the rotation / Remove, its check), Collect
  / Reset, why packets aren't used (`ACAL_STATE`), a 36-sector coverage ring,
  the latest result (check, rotation, per element delay - also as coax mm at
  VF 0.66 -, x / y, unexplained, gain dB - shown only, gains aren't
  corrected) + Apply. Boxes re-render only when their HTML changes (the
  result's age has its own span): a re-render under the pointer swallowed
  clicks
- Tests (scratchpad acal/): `t_acal.cpp` (array_cal + MUSICProcessor on
  synthetic packets from a 205 mm UCA with rotation 2.5 deg, 12-60 ps
  delays, 2-4 mm position errors, 2-5 deg antenna phases: positions within
  0.25 mm, delays = truth incl. the antenna phases, MUSIC 1090 MHz median
  error 3.0 -> 0.09 deg, 433 MHz 2.9 -> 0.74 (0.35 without antenna
  differences), save / load, rotation off, other radius = not used, refit /
  start-over rules; `t_acal airport`: 5 good aircraft + 6 clustered at 246
  deg reflected from 161 deg + 2 on the ground -> the 6 left out, the 2
  skipped, positions within 0.25 mm); `adsb_sim.py` (Mode S generator: DF17 ident / CPR
  position / velocity, PPM at 24 MHz integrated to 2.4, the same array
  errors; `file` for `decoder --file`, `serve` = a fake heimdall on 8091):
  plugin packet positions median 2.4 m off the truth; `t_e2e.py` against the
  test build (ports 1808x, `KRAKEN_PLUGIN_DIR` = the real plugins): 30
  aircraft, fit within 0.25 mm / 0.1 ps, live per-aircraft DF error (below
  20 deg) 3.05 -> 0.22 deg, off again 3.44; `t_acal_ui.py` (headless
  Chromium against it)

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
  POCSAG: "RIC n Ff baud" + message codewords' 20 data bits
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

**🗣 Voice to Text (`src/voice2text.cpp`, `include/voice2text*.hpp`, `transcribe/`, sidebar "🗣 Voice to Text", the 🗣 Voice tab):**
- What is said on a VFO, written down by a LOCAL, free model: whisper.cpp
  (MIT, pinned v1.9.4 = `WHISPER_COMMIT` in `transcribe/setup.sh`) with
  OpenAI's Whisper models (ggml-*.bin from huggingface.co/ggerganov/whisper.cpp)
  + Silero VAD (ggml-silero-v6.2.0.bin from ggml-org/whisper-vad). Opt-in:
  `make whisper` = `transcribe/setup.sh` (clone + CMake build -j3 max ~5-10
  min + base.en + VAD; `--models-only M..`, `--build-only`, `--list`,
  `--update` = what `make all`'s `whisper-update` runs once it is set up:
  incremental, quiet, re-pins whisper.cpp); `WHISPER=1 ./install.sh`
- Per VFO: `DecimatorInstance::transcribe` (VFO card "🗣 Transcribe",
  `VOICE2TEXT_VFO:id:0|1` -> `DecimatorManager::setTranscribe`, which also
  sets the decoder's voice tap; DECIMATORS snapshot field 11, optional -
  older files load with it off). `anyTranscribeActive()` makes the pipeline
  decimate (in fm_only mode the VFO counts as `single_wanted`)
- Source = the VFO's own stream, not the Audio Src's: with its DIGITAL
  DECODER ON, the decoder's voice - `dig::set_voice_tap_handler` (one
  global handler) gets the lead plugin's W_AUDIO 8 kHz + plugin id + its
  talker + RF, on the decoder's worker thread. The talker: the open one on
  the channel the plugin says its audio is from (`kp::Host::audio_channel`
  -> wire AUDIO_CHANNEL 34 -> `Runner::audio_channel`, reset on a restart;
  the dmr plugin sends the timeslot it plays via `RxContext::voice_channel`),
  else the open talker that STARTED first (decoders play the first to
  talk). It was the most recent one across channels: with both DMR slots
  busy, slot 1's speech was named after slot 2's radio and split when that
  call ended (review 2026-10-13). `voice_tap()` also counts as VOICE_WANTED, so the
  vocoder runs while nobody listens (a POCSAG / non-voice decoder gives
  nothing). Otherwise ANALOG: data_receiver feeds `v2t::feed_analog` after
  the digital push (same stream: beamformer output when it ran, else the
  listened-to channel) with the VFO's demod and a gate = its squelch_open
  when the squelch is on (FFT or Digital), else
  `check_squelch_in_range(.., AUTO_SQUELCH_DB 10 dB)` - the same FFT test as
  the squelch, so a squelch-off VFO still only records while a signal is
  there
- ONE worker thread ("voice2text") owns the taps, the job queue and the
  helper process; inputs are copied into a queue (q_mu; <= 3 s per VFO
  waiting, else dropped), status / history are read under st_mu. Per tap:
  analog demod at the VFO rate -> msresamp to 16 kHz -> Butterworth HP (260
  Hz NBFM / AM - CTCSS out -, 100 Hz WBFM) + LP (3.8 / 7 kHz). FM = atan2
  discriminator / deviation (5 kHz NBFM, 75 kHz WBFM, 75 us de-emphasis for
  WBFM only - as the live audio); AM = envelope - 30 Hz DC, power AGC 0.5 s.
  Digital = 8 kHz x2 (firinterp)
- Cutting (`Worker::analog / digital / finish`): analog starts on the gate
  with 0.3 s pre-roll, ends after the gate is closed `HANG_S` 0.8 s (0.25 s
  tail kept), or `ANALOG_STALL_MS` 2 s after its input stopped (heimdall
  gone: it used to stay open and glue the audio from after the gap on);
  digital ends `DIGITAL_GAP_MS` 1.5 s after the last voice, or at a talker
  change. Unnamed audio ("" / "?") after a named talker only marks where it
  began (`Tap::anon_from`): the next name decides - the same radio = one
  transmission (P25 closes a talker after a 0.75 s pause), another radio =
  `split()` there (the next radio's first frames come before the plugin
  names it); a transmission that starts unnamed adopts the first name.
  RF move > 500 Hz / demod / rate change ends the transmission. < 0.4 s
  dropped. Long audio -> pieces (`part` 1, 2..; `maybe_cut` / `cut` after
  every input block): two-way = ONE transcript up to `TWOWAY_MAX_S` 25 s
  (its text comes at its end anyway; whisper does better with the whole -
  cutting at 18 s split 20 s transmissions for nothing), then cut at its
  latest pause after `TWOWAY_MIN_S` 11 s; CONTINUOUS (a WBFM VFO, or
  `tx_samples` - the transmission so far - > `CONTINUOUS_S` 30 s: AM
  broadcast, a weather channel) from `CONT_SOFT_S` 8 s on at the latest
  pause after `CONT_MIN_S` 5 s, so the text follows ~10-20 s behind. Pause =
  `PAUSE_FRAMES` 12 x 20 ms frames (`Tap::fe`, energies kept as `cur` grows)
  under max(0.1 median, 2 x 10th percentile) <= 0.5 median - a tenth of the
  median alone found NO pause in hissy radio audio (an Apollo recording's
  gaps sit at a third of the median), cut in its middle. No pause by the
  maximum (15 / 25 s): at the quietest 100 ms of the last 3 s, the next
  piece repeating `OVERLAP_S` 0.3 s. DoA: while recording, each NEW published MUSIC frame
  (`getResultStampMs`, not stale, coherent, DoA on) adds its compass-sense
  bearing (360 - peak, as the DoA logger) weighted by confidence -> circular
  mean = `doa`; `brg` = doa + station heading when valid. With the Digital
  squelch the VFO's DoA is published from confirmed frames only, so a
  digital transmission's bearing is the talker's
- LOBE per transcript (`sample_doa`, the same frames + weights as `doa`):
  each frame's pseudospectrum (`getPseudospectrum`, |v|, min..max -> 0..1,
  array angle = index x res, linear between bins) summed into
  `LOBE_BINS` 720 x 0.5 deg = the map's live-lobe format (rdf_mapper.cpp
  `display_lobe`: compass tau = heading - array angle) - TWICE: `lobe_n`
  turned by the heading of the frame's moment (frames with a heading - a
  vehicle turning during a transmission stays right) and `lobe_a` with
  heading 0 (relative to the array, all frames); `finish` keeps lobe_n when
  >= half the weight had a heading (`lf` "n"), else lobe_a (`lf` "a"), min..max
  -> u8 -> base64 (960 chars), + the station's mean position (`slat` /
  `slon`, longitudes around the first - antimeridian). Parts of a long
  transmission each get their own (`Tap::reset_doa`). In transcripts.jsonl
  (`transcript_json(t, true)`: lobe, lf, slat, slon; a bad slat keeps the
  lobe, a malformed lobe - not 960 base64 chars - is dropped), NOT in the
  pushed lists (`transcript_json(t, false)`: only `"lb":"n|a"`) - 300
  history items x 1 KB to every page on every connect.
  `VOICE2TEXT_LOBE:<id>` (digits only, `is_query_command`) -> broadcast
  `v2t::lobe_message(id)` = `{"voice2text":{"lobe":{id, t, vfo, mhz, dur,
  lobe, lf, doa, brg, slat, slon}}}` or `{id, error}`; the page that asked
  (`v2tLobeWant`) shows it, the others ignore it
- Queue: <= `MAX_QUEUED_JOBS` 40 / `MAX_QUEUED_S` 600 s (then the oldest
  go, `dropped`); `usable_` (helper + model files there) else input is
  ignored and the queue cleared (no "dropped" spam while not set up)
- Helper `transcribe/build/kraken_whisper` (transcribe/kraken_whisper.cpp,
  CMake: whisper.cpp via add_subdirectory, static) started with
  `PluginProcess::start(argv)` (new overload; same pipes / stderr tail as
  plugins): `--serve --model M --threads N [--vad V]`, nice 10 (it renices
  itself). Wire `include/voice2text_wire.hpp` (plugin framing): JOB = id +
  int16 16 kHz, OPTION key=value (language, prompt, threads, vad,
  vad_threshold, translate, beam), READY (model, type, multilingual, vad,
  version, system), RESULT (id, conf, no_speech, proc_ms, speech_ms, lang,
  text, dropped reason), ERROR. ONE job at a time (the next is sent after
  the RESULT); options re-sent when they change; a model change restarts
  it. Crash -> the in-flight job is retried once, restart after 2..30 s; a
  job running > max(90 s, 15x its length) kills it. No VFO transcribing for
  `HELPER_IDLE_STOP_MS` 2 min -> stopped (base.en ~280 MB RAM)
- In the helper per job: Silero VAD (min speech 200 ms, min silence 300 ms,
  pad 200 ms) -> only the speech parts, joined by 150 ms of silence (< 200
  ms of speech = "no speech", whisper never runs: 30-40 ms for 5 s of noise)
  -> whisper_full (greedy, no_context, no_timestamps, suppress_nst, language
  "en" for .en models) -> segments with no_speech > 0.6 AND avg logprob < -1
  skipped -> [..] (..) *..* and music notes stripped, a sentence / word
  repeated 4+ times kept once -> dropped: empty, subtitle-credit phrases
  ("thanks for watching"...), lone fillers ("thank you", "you", "bye"...)
  when no_speech > 0.35 or conf < 0.5. conf = mean token p. Greedy decoding
  sometimes ENDS after the first sentence ("Roger the register." of 11 s of
  speech; the clip offline gave the whole text): a result with < 0.6 words
  per second of speech (>= 4 s) is decoded again as two halves of the
  speech, split at the VAD gap nearest the middle (`joins`), the longer
  text wins. Timestamps (whisper.cpp seeks on from the last one) did NOT
  help - same text, and always on they broke a clip that decoded fine.
  Our own VAD
  (not whisper_full's vad flag): its segment list goes stale when a clip has
  NO speech, so speech_ms couldn't be read
- PROMPT ("Words to expect", `VOICE2TEXT_PROMPT:`, <= 300): whisper reads it
  as the text said before, and a bare list ("Houston, Roger, over") made
  greedy decoding END after one word ("the", conf 0.99) on 2 of 4 test
  transmissions - with or without carry_initial_prompt / timestamps /
  no_context. `prompt_text()` turns a list into a finished sentence
  ("Glossary: Houston, Roger, over.") - all 4 decoded fully; and a prompted
  result with < 0.4 words/s over >= 2.5 s of speech is decoded again
  without the prompt (the longer text wins). Beam search was no cure (one
  gave "Later.")
- Outputs (`on_result`): history (last `MAX_HISTORY` 1000 in memory,
  appended to `transcripts.jsonl` - cwd, gitignored; rewritten from the
  history when it passes 3000 lines, `rewrite_history_locked`; a line that
  doesn't parse is skipped at startup - `stod` threw and the supervisor
  restarted into the same crash), `declog::record_voice`, the clip (below),
  `fresh` for the push - the clip list, file and history in ONE
  st_mu section that drops the result when `clear_gen` moved since its id
  was taken (a Clear meanwhile brought it back, clip gone). Transcript id =
  max(last + 1, t0 ms) (unique, sortable, survives restarts). JSON {id, t,
  dur, vfo, mhz, src, tid, tl, doa, brg, lb (pushes) / lobe, lf, slat, slon
  (the file), part, conf, lang, ms, clip, text}.
  The status `error` = `last_error`, kept until the helper loads (READY) or
  a job goes through (a crash's message used to be overwritten by
  "starting" within ms)
- Pushes: `{"voice2text":{"new":[..]}}` (1 s timer, `take_new_message`) and
  `{"voice2text":{"status":{..}}}` when it changed (state off | starting |
  ready | busy | error | not installed | no model, error, installed,
  models [{name, mb, multi, note}], catalogue (MODEL_CATALOGUE = setup.sh's
  CATALOGUE - keep them equal) with have, vad_model, settings, model_multi,
  log{enabled}, loaded, vad_loaded, queue, queued_s, dropped, done, nospeech,
  failed, rtf, clips, clip_mb, clip_dir (absolute), clip_days, clip_free_mb,
  clip_low, clips_skipped, clip_error, loop_max_ms + in_dropped_s (the
  worker's longest loop / input dropped at the 3 s cap - diagnostics),
  vfos [{id, src, rec, secs, digital}],
  download{name, running, pct, error}); `GET_VOICE2TEXT` (page connect) ->
  status + `history` (300). `VOICE2TEXT_CLEAR` empties history, file and
  clips. Settings (schema BEFORE DECIMATORS, so restored VFOs start with
  the model set): `VOICE2TEXT_MODEL:` (name = ggml-<name>.bin, [A-Za-z0-9._-],
  no leading dot), `_LANGUAGE:` (auto | code), `_THREADS:1-4` (default 2),
  `_PROMPT:`, `_VAD:`, `_TRANSLATE:`, `_KEEP_AUDIO:`; `VOICE2TEXT_VFO:`,
  `_DOWNLOAD:`, `_CLEAR` are in `is_query_command` (own broadcasts)
- `VOICE2TEXT_DOWNLOAD:name`: catalogue names only (fixed Hugging Face
  URLs), `curl` via posix_spawnp on a detached thread -> `.part` -> rename
  (+ the VAD model when missing); progress = file size / catalogue MB
- Paths: `KRAKEN_TRANSCRIBE_DIR` (default `transcribe`, cwd-relative) holds
  `build/kraken_whisper`; models in `KRAKEN_WHISPER_MODELS` (default
  `<dir>/models`). Docker: helper built in the image (`--build-arg
  WHISPER=0` = without), models in `/data/whisper_models` (entrypoint), curl
  in the runtime image
- Clips ("Keep the audio", default on; `save_clip` / `write_opus`, on the
  worker): Ogg Opus (RFC 7845: OpusHead + OpusTags, libopus - already linked
  for the live audio - 16 kHz mono VOIP 16 kbit/s, complexity 8, pre-skip =
  lookahead x 3, the last page's granule trims the padding; Ogg CRC by hand;
  ~2 kB/s = ~7 MB per hour of speech vs 115 MB as WAV) with Vorbis-comment
  tags TITLE (the text), DATE, KRAKEN_VFO / MHZ / SOURCE / TALKER / BEARING.
  In the DECODER LOG's folder, also with logging off: `declog::folder()` +
  `/voice_clips/YYYY-MM-DD/<id>.opus` (local date; .tmp + rename).
  `clips_tick` (2 s): a changed folder = rescan (`scan_clips`: only
  `<digits>.opus` in YYYY-MM-DD dirs; clips in the old folder stay there,
  no longer in the index - the tab says "the audio is gone"); retention
  (`clip_retention`: day dirs older than `declog::keep_days()`, 0 = keep;
  clip-named files only, then rmdir - a foreign file keeps its dir) every
  10 min and when the days / date change; free space (statvfs of the
  nearest existing parent) < `MIN_FREE_BYTES` 100 MB -> `clip_low`, clips
  skipped + counted (transcripts go on); `MAX_CLIP_BYTES` 2 GB backstop,
  oldest first. The log record's `clip` is relative to the log folder
- HTTP `GET /voice_clip/<id>.opus` (websocket_server, `audio/ogg`): host
  check + token like /recordings; only ids in the clip list
  (`v2t::clip_path`)
- UI (kraken_doa.html "🗣 VOICE TO TEXT" block; state `v2tStatus / v2tList /
  v2tUnseen` in the globals block - `infoRenderTabs` reads them): sidebar
  box `#v2t-section` (`v2tRenderBox`: install hint, model + ⬇ Get list,
  language row only for multilingual models, cores + speed, prompt, VAD /
  keep ticks, status, "To disk" + `v2tLogOn` = DECODER_LOG_TYPES + voice,
  DECODER_LOG:1), VFO cards (`dec-v2t-<id>` + state, `v2tRenderCards` from
  updateDecimatorList and every status), info panel tab `infoTab = 'voice'`
  (`#info-body-voice`, `v2tRenderTab`: newest first, 400 rows, search with
  `<mark>` on the raw text, VFO filter, ▶ `v2tPlay` - fetch with the token
  -> blob -> Audio; a browser without Ogg Opus (`canPlayType`, older
  Safari): `v2tDecodeOgg` (Ogg pages -> packets, pre-skip from OpusHead ->
  the live audio's opus-decoder.min.js) -> WebAudio; the box's clip line
  `#v2t-clips` (count, MB, Keep days, folder, low disk / errors), ⬇ CSV `v2tExport` - cells starting = + - @ get a ' so a
  spreadsheet doesn't run them, Clear), tab label with a red dot while
  recording and a count of transcripts that came while hidden
- Lobes on the 🗺 map (kraken_doa.html, in the "🗣 VOICE TO TEXT" block;
  state `v2tLobes` (id -> {L, col, peak, el}) / `v2tLobeWant` in the globals
  block - mapDrawNow -> `v2tLobeDraw` runs before the section is
  evaluated): a row's 🧭 bearing (`.v2t-brg.lb`, `lb` set) -> `v2tLobeClick`
  (again = hide) -> VOICE2TEXT_LOBE -> `v2tLobeAdd`: colour = the first of
  `V2T_LOBE_COLS` (white, pale violet / blue / green / orange / red - none of
  the VFO colours, `getDecimatorColor`) no other shown lobe has, the map
  opened (`mapToggle` / `rpShow('map')` from the DoA tab), centred on the
  place when it is off screen. `v2tLobeDraw` (after `rdfDraw`): from
  slat / slon, else the station now (`mapStationPos`); `rdfLobePath` at the
  live lobes' radius (0.3 of the map), fill 0.16, dark edge + the colour
  DASHED (live lobes: solid in the VFO's colour); a tag (`.v2t-ltag` in
  `#map-v2t` over the canvas, made once per lobe, moved every draw: past
  the tip along the peak, kept inside the map) "● D0 17:18:19 · 🧭 60°"
  ("arr 90° (array)" for an `lf` "a" lobe - drawn with the array's front
  north) + ✕ (`v2tLobeRemove`); its pointerdown doesn't reach the map (the
  map listens on the canvas). The row's bearing is outlined in the lobe's
  colour while shown; a history without the id (Clear) removes it
- Measured (Pi 5, other sessions loading it, load avg 5-8): base.en ~280 MB
  RSS, an 8-11 s transmission in 3-7 s on 2-3 threads (rtf 0.3-0.6),
  tiny.en 175 MB / ~2x faster but poor on radio audio, small.en-q5_1 460 MB
  and ~2x slower than real time (the most accurate). `audio_ctx` (shorter
  encoder window) was SLOWER here and is documented to cost accuracy - not
  used. Parakeet (also in whisper.cpp 1.9) not used
- Tests (scratchpad of job 27e7efb6): fake_heimdall_voice.py (FM with a 100
  Hz CTCSS / AM / bare carrier speech clips onto a 5-element UCA from set
  bearings, generated block by block) + a TEST build on 3908x + t_v2t.py:
  D0 NBFM squelch on (JFK 60 deg, Apollo-13 200 deg, a 1.5 s carrier ->
  "no speech", fast speech 300 deg), D1 AM squelch off (auto gate,
  overlapping D0), D2 not ticked: every text as expected, doa exact (300 /
  160 / 60 / 240), D2 nothing, decoder log + clips + snapshot field,
  refused commands (`../../etc/passwd` model, threads 9, `EN;rm`, unknown
  download, VFO 9), clip route 200 / 404s. Digital: the scratch plugin
  ptest/voicetest (scripted decoded voice + talkers through the real plugin
  protocol): unit 1234567 -> TG 100 11.0 s, 7654321 right after (split at
  the change), "?" then 5550001 (adopted) - texts right, from / to in the
  log; call 2 named only 0.6 s in -> call 1 exactly 11.0 s; `VT_MODE=dmr`:
  slot 2's radio keys up during slot 1's call (talker with a later end,
  audio played after) -> 1001 one 11.0 s transcript, 2002 its own.
  run_dl.sh: empty models folder -> "no model", `VOICE2TEXT_DOWNLOAD:tiny.en`
  (progress, the VAD model too, a 2nd refused while running) -> model ->
  ready (the page that pressed ⬇ Get switches to it when the chosen model
  isn't installed, `v2tAutoPick`). Corrupt history lines skipped; a scene
  cut mid-transmission -> its 6.3 s transcribed 2 s later. t_ui.py
  (headless Chromium, 34 checks): box, cards, tab, search, filter, escaping
  of injected markup, CSV formula guard, clip play / stop (native Ogg Opus
  and the forced decoder fallback - the whole clip decoded), "audio is
  gone", the clip line, toggles, settings, tab kept over a reload. No P25
  voice recording here and no mbelib (DMR / D-STAR / NXDN): real vocoders
  not tried. Cutting + clips (make_bc.sh / run_bc.sh, 3 antennas): a 36 s
  WBFM station (JFK + Apollo + Micro Machines, no pre-emphasis in the
  generator) -> 5.5 / 11.9 / 7.9 / 11.4 s pieces, the first cut at JFK's
  pause ("... ask not" | "what your country ..."); a 25 s two-way -> 22.2 +
  3.4 s at a pause, a 20 s one -> ONE transcript; Opus files = transcript
  dur (ffprobe), retention of a planted 12-day-old folder (its foreign file
  + folder kept), relative clip paths in the log. NB the generator's weak
  carrier (0.05 in 0.02 noise, ~300 Hz rms deviation: ~17 dB audio SNR)
  made the two-way unintelligible - not the code: 10 dB more matched the
  source offline. run_clips.sh: DECODER_LOG_DIR mid-run -> new clips + log
  in the new folder; a build with MIN_FREE_BYTES 2^60 -> no clips,
  clips_skipped 1..4, transcripts on. Lobes (run_lobe.sh + t_lobe.py):
  the voice scene's 4 transcripts each with a 720-bin north lobe peaking at
  its bearing (300 / 240 / 160 / 60, <= 0.1 deg), the station position;
  pushes without lobe data; a bad id refused, an unknown one -> error; after
  a restart (no heimdall) all 4 back from transcripts.jsonl.
  run_lobe_ui.sh + t_lobe_ui.py (headless Chromium, 20 checks, live D0 / D1
  lobes on the map at the same time): bearings clickable, click -> map opens,
  peak = bearing, tag + ✕, row marked, colours unlike the VFOs', drawn
  along the peak and nothing opposite (on a canvas of their own - the map's
  holds cross-origin tiles, unreadable), a second lobe in another colour,
  ✕ / a second click remove, an array-frame lobe tagged "(array)", error
  answer, another page's answer ignored, Clear removes them

**Noise floor = the spectrum's 0 dB (`src/signal_processing/noise_floor.cpp`, `nfloor::`):**
- Every tuner channel's spectrum is drawn, FFT-squelched (`check_squelch`,
  `get_range_peak_db` -> `check_squelch_in_range`) and scanned (continuous
  scanner) relative to ONE floor per channel: `FFTProcessor::
  channel_noise_floor(ch, data)`. It was each caller's own 10th percentile
  of the centre 80 % of the bins (`calculate_noise_floor`, still used by the
  wideband scan's stitched spectrum) - a signal wider than the 2.4 MHz
  (DVB-T, LTE) fills those bins, so the percentile landed on it: drawn at
  ~0-10 dB (only its edge showed its height) and an FFT squelch inside it
  never opened (the user's report, 2026-10-14)
- `nfloor::on_frame` (FFT worker, under fft_mutex - after the EMA; worked
  out at most every `EVAL_MS` 50 per channel and on every retune: a
  percentile of 13k bins is ~0.1-0.2 ms on the Pi 5 and frames come ~146 / s
  - the squelch checks, which each took their own percentile per packet,
  read the cached floor now): f = the averaged spectrum's percentile (today's floor), r = the RAW
  frame's percentile + 10 log10(fft size) (independent of the averaging and
  the FFT size). The receiver's own floor is remembered per tuner channel and
  gain (`"ch|gain"`, from the 8091 header; AGC (-1) and calibration
  (`doa_is_calibrating`) = the frame's floor, nothing learned):
  follows a lower r within ~0.5 s, a higher one 1 dB/s, nothing learned
  `SETTLE_MS` 600 after a retune; a tuner with none takes the others' at
  that gain. HELD (the band's floor is a signal) -> floor = f - (r - ref):
  the averaged spectrum's percentile stands as far above its noise as the
  frame's (a band-filling signal fluctuates like noise). Not held = f,
  exactly as before
- When is it held? Coherent mode: the COHERENCE TEST - receiver noise is
  independent per antenna, a signal isn't. `measure_coherence` (decimation
  thread, the FFT channel + any other antenna of the same packet, every
  `MSC_PERIOD_MS` 250; the receiver converts a second channel for it when
  DoA is off - `coherence_due()`): Welch 256-point Hann segments (64 per
  16384-sample packet), magnitude-squared coherence per bin, the mean over
  the quietest quarter of the centre 80 % (no DC) -> >= `MSC_SIGNAL` 0.2 =
  signal (noise ~1/64; a signal at SNR s (s/(1+s))^2), EMA 0.5, valid 2 s
  and only after the last retune. Signal: held once r > ref + 1 dB, ref
  never rises (a day on DVB-T doesn't wear it away); noise: not held, ref
  follows (a real rise - active antenna, interference - is learned at once).
  Independent / wideband scan mode or no fresh verdict: held when r > ref +
  6 dB (released < 4), ref creeps up 1 dB a minute while held (a real +10
  dB rise shows as ~10 dB noise for minutes there)
- `noise_floors.json` (cwd, gitignored; `{"version":1,"floors":{"0|49.6":
  -27.49,...}}`): loaded at start (`nfloor::load`), written by the FFT
  worker at most every 5 min when a floor moved >= 0.5 dB
  (`maybe_save`) and at shutdown (`save_now`), so a start on DVB-T is right
  at once. Not touched by RESET_SETTINGS
- Not covered: the beamformed-FFT squelch (beamforming on: its own narrow
  spectrum's percentile, `check_squelch_beamformed`), the wideband scan's
  stitched spectrum + discrete scanner
- Tests: unit (scratch t_nfloor.cpp, `-DNFLOOR_TEST_CLOCK` = a test clock,
  minutes in no time) - noise = the old floor exactly; coherent +20 dB
  band-filling shown at +20 for 10 min; a +10 dB uncorrelated rise learned
  at once; independent: +20 held, creeping to +10 in 10 min; retune onto the
  signal right within 0.5 s; AGC / calibration = old; FFT 8192; another
  tuner taking tuner 0's floor; a strong carrier in noise (verdict noise);
  the file round trip. End to end (scratch e2e/fake_heimdall_floor.py +
  t_floor.py, a test build on ports 4708x): 5 coherent channels, phases
  noise / a band-filling signal 20 dB over it, the same in every antenna /
  noise 10 dB up, independent per antenna / noise / the signal again, each a
  retune; VFO 0 FFT squelch 10 dB. The band's median as drawn (noise's own:
  2.1-2.4 dB): before 2.2 dB on the signal, squelch never open; now 21.6-
  21.9 dB, squelch open 100 % (DoA off for 12 s of it - the second channel
  converted for the test), the +10 dB noise at 0 dB after ~1 s (no verdict
  for the first frames after the retune), squelch closed. kraken_doa CPU the
  same (50-55 vs 52 % of a core)
**Digital TV (`plugins/dtv/`, README.md there; ARRAY plugin API):**
- Plugin id `dtv`, name "Digital TV (DVB-T/T2, ISDB-T, ATSC 1.0/3.0,
  DTMB)", v2.0 - was `sfn` "SFN Separation (..., DAB)" (renamed 2026-10-14;
  DAB removed, to become a plugin of its own). A broadcast TV signal's own
  information from the 2.4 MHz slice the VFO sees: the standard + mode, the
  channel (measured centre + the raster's number - `channel_label()` in
  decoder.cpp: 8 MHz UHF 21-69, 7 MHz Band III / Australia, 6 MHz North
  America / Korea, ISDB-T Japan / Americas with the 1/7 MHz offset, China
  DS for DTMB), DVB-T TPS (BCH(67,53)-checked; constellation, hierarchy,
  code rates, guard, mode, cell ID, DVB-H, net bit rate), ISDB-T TMCC
  (structure-checked; layers A / B / C, partial reception, next parameters,
  the EWS start flag -> a "⚠" event), ATSC 1.0's fixed parameters, DTMB's
  header; the echo profile (CIR paths). `Estimator::info()` / `take_news()`
  / `centre_hz()` / `bw_mhz()`; OFDM signalling via
  `OfdmStandard::signal_carriers` / `signalling` / `signalling_symbols`
  (`signalling.hpp`: DBPSK soft bits; common phase step from DVB-T's
  continual pilots, else the squared products)
- **Path separation** (option `separate`, default "0" = off; an on / off
  button in the decoder tab; plugin v2.1 - it was "SFN separation" until
  2026-10-15): the signal reaches the antennas on several paths -
  reflections, and in a single frequency network the other transmitters -
  one VFO DoA mixes them. On: every ANTENNA's channel impulse response
  against the signal's known reference, the paths as peaks of the per-tap
  spatial covariance (by delay), tracked, each one's covariance reported;
  kraken_doa runs MUSIC with one source on it -> a talker "Path-n" per path
  (`SfnCore::tid`), its own DoA lobe. Renamed because a transmitter and a
  reflection are indistinguishable (identical copies of the signal; DVB-T's
  cell ID is the same on every SFN transmitter) - on the real Auckland
  muxes every "SFN-2.." was a reflection. Delays from the MAIN path
  (`SfnCore::strongest()`: the strongest confirmed track, kept unless
  another is 1 dB stronger), signed (a weaker earlier path is negative);
  the circular order still comes from `earliest()` (after the largest gap),
  whose delay is the table's "From first" column; `sp.ref` = the main path
  (UI tag "main"). Events "New path Path-n: ..." / "Path Path-n gone"; fact
  "Paths" (count, spread, first-before-main). Off (`SfnCore::
  set_profile_only`, the core's default): only antenna 0 is analysed (the
  plugin drops the rest of each block; kraken_doa still sends them), the
  CIR's paths become the "Echo profile" plot (markers without talkers) +
  "Paths" fact, a path marked when seen again within 1.5 taps in one of
  the two bursts before; `Estimator::profile_floor_db` (ATSC 3.0 22 dB)
  hides artefacts one antenna can't tell by direction. Toggling resets the
  core: its table rows go, `SfnCore::reset()` calls `spatial_remove("")`
  (the paths leave the 📡 DF panel within ~1 s instead of SPATIAL_TTL_MS 2
  min) and turning separation off also sends empty table columns (no empty
  path table left in the tab); a lost track is removed the same way
  (`spatial_remove("Path-n")`). Option keys `separate` / `max_tx` ("Max
  paths") kept, so saved VFOs keep their setting. Per standard: `ofdm.cpp` (generic pilot engine:
  guard fold / P1 grid / symbol tracking, pilot comb phase - each bin's
  power against its burst average first, else a 2k signal with paths 37 us
  apart never locked -, carrier offset by the sparsest CIR, per-symbol phase
  step measured + removed, runs aligned) + `dvbt.cpp`, `dvbt2.cpp`,
  `isdbt.cpp`, `atsc3.cpp`; `atsc.cpp` (field sync), `dtmb.cpp` (PN
  headers, phase schedule fit); `sfn_core.cpp` (per-tap covariance, floor,
  peaks, echo / sidelobe rules, tracks, spatial reports, plot, table);
  `std/*_tables.hpp` (the standards' tables). Bursts shorter than half a
  burst (the host's blocks overshooting one) are ignored
- Compatibility: `dig::parse_mode_string` maps `PLUGIN:sfn` to `dtv` and
  `options_from_string` the saved `sfn.*` options to `dtv.*`
  (`renamed_plugin()` in dig_report.cpp); the plugin treats an unknown
  `standard` (a saved "dab") as auto
- ARRAY plugin API (kraken_plugin.hpp): `kp::Info::array` (+
  `array_burst_s`, `array_period_s`; dtv 0.35 s every 1 s),
  `Decoder::process_array(const kp::ArrayBlock&)` (m antennas, n samples
  each, the VFO's own rate - no resampling / AFC -, t0 = Host::time(),
  gap), `Host::spatial(kp::Spatial)` (id, label, span, m x m cov, delay,
  power, snr, ref, avg_s), `Host::spatial_remove(id)` ("" = all of the
  plugin's: `SpatialReport::remove` -> `TalkerDoa::add_spatial` erases them
  and their pending mobile DF records), `Host::plot(kp::Plot)` (title,
  units, x0 / dx, y <= 1000, threshold, markers {x, label, talker}). Wire
  SAMPLES_ARRAY 9 (int16 IQ per antenna, gap flag), SPATIAL 35, PLOT 36,
  SPATIAL_REMOVE 39 (unknown types are ignored, so older hosts are safe).
  An empty TABLE_COLUMNS payload = no columns (it used to become one blank
  column): with no rows either, the page shows no table. `dig_plugin`
  reads array / burst / period from `--info`; array => manual_only. The
  offline host runs array plugins on multichannel cf32 (`--channels`,
  `--all`, `--uca R` prints each spatial talker's MUSIC bearing)
- Host: `DigitalDecoder::wants_array()` / `push_array()` - the data
  receiver passes every VFO block of all antennas while not calibrating /
  in the retune hold; only blocks inside the plugin's bursts (stream time)
  are copied and sent (int16). Engine `on_spatial` -> `SpatialReport`
  (span in VFO positions) -> `set_spatial_handler` (DecimatorManager) ->
  `TalkerDoa::add_spatial`: a spatial talker (`TalkerInfo::spatial`,
  sp_delay_us / sp_db / sp_snr / sp_ref), its covariances averaged over
  avg_s (`SPATIAL_AVG_S` 3 s of stream time), MUSIC with ONE source
  (`spectrumFromCovariance(R, 1)`), each report a mobile DF record;
  forgotten after `SPATIAL_TTL_MS` 2 min. `on_plot` -> `Report::plot_set`
  -> the status JSON's `plots` {plugin: {t, xu, yu, x0, dx, ymin, ymax,
  thr, y, mk: [[x, label, talker]], age}} (with the tables: at most once a
  second per VFO). rdf_mapper: talker field `sp: [delay_us, power_db,
  snr_db, first]`; spatial talker IDs are volatile (per plugin run): their
  heat maps are not saved to rdf_session.bin
- UI (kraken_doa.html "Plugin plots + TV signal paths" block): the
  decoder tab draws each VFO's plugin plot (`digRenderPlots` ->
  `#info-dec-plots`, `plotDraw`; a plot whose markers name no talker - dtv's
  echo profile - gets a tooltip saying so instead of "click a peak", and
  the DF panel section only shows plots with talker markers); a marker naming a talker and a table row
  whose key is a talker of that VFO (`digRowTalker`, class `talker`) call
  `talkerPick(vfo, tid)`: the 🗺 Map opens with the 📡 DF panel on that
  VFO and the talker selected (again = the whole signal). The DF panel's
  `#map-df-sfn` section draws the selected VFO's plot with talker markers
  (`sfnRenderSection`). Talkers with `sp` are kind 'path' (📶, by
  delay from the main path, "main" = the strongest; "Every path (n)")
- It replaced an in-process blind SFN separation per VFO (sfn_doa.cpp,
  `DEC_SFN:` / `SFN_MAX_US:`; commit 7f3535a): the per-tap CIR against a
  known reference needs the standard's structure, which belongs in a
  plugin
- `PluginProcess::flush()` keeps a write offset (`wpos_`) instead of
  erasing every write from the front of `wbuf_` (quadratic in what is
  queued: an array burst queues ~17 MB and the pipe takes 64 KB - 1 MB at
  a time - it cost up to 60 % of a core) and asks for a 1 MB pipe. While
  only array plugins run (`array_only_`), `push()` passes a one-sample tick
  every 100 ms instead of the VFO's stream (it starts the plugin, reads
  its replies, follows the RF)
- CPU (Pi 5, DVB-T live, separation on): plugin ~25 % of a core,
  kraken_doa +6-16 %; the estimation per burst ~3x what it is with
  separation off (real 522 MHz capture: ~100 vs ~35 ms, busy Pi)
- Real captures (/home/krakenrf/dvbt_compress_study/captures, 6 Auckland
  DVB-T muxes, 5 coherent channels): DVB-T 8k GI 1/16 locked on all, TPS
  BCH-checked in every frame (64-QAM 3/4, 26.35 Mbit/s, cell IDs 32100,
  10100, 20100, 31100, 30100, 33100 - round decimals, so the high byte in
  frames 1 / 3 is right), one transmitter each (48-55 dB above the floor) +
  local reflections 13-35 dB down - hence the `max_rel_db` option (30 dB).
  No real SFN, no real ISDB-T tried yet
- Tests: offline regression of 12 synthetic recordings (`tools/sfn_gen.cpp`
  - every standard, awkward VFO offsets, DTMB phase-varying, ATSC 3.0
  excess samples; README's table; scratchpad regress2.sh) - separation on:
  delays / powers exact, bearings within 1.3 deg; off: exactly the 3 paths
  each. The generator sends real TPS (BCH parity, `--cell`) and TMCC
  (`--ews`): DVB-T 2k / ISDB-T modes 1 and 3 decoded, the EWS event. Live
  (scratchpad e2e/: fake_heimdall_rec.py streams a recording in heimdall's
  8091 format; t_sfn_plugin.py drives a TEST build on ports 4708x / 4709x;
  t_sfn_plugin_ui.py = headless Chromium): DVB-T exact, the UI's plot /
  table / DF panel picks; t_dtv_ui.py: `PLUGIN:sfn` -> dtv, the Path
  separation button (Off by default, On / Off by the echo), the echo
  profile's tooltip, cells' "Cell information" button

**LTE / 5G NR Cell Decoder (`plugins/cells/`, README.md there; ARRAY plugin API):**
- Plugin id `cells` (kept from "Cell Separation"; name "LTE / 5G NR Cell
  Decoder", v2.0): the public broadcast information of every cell on a
  carrier (LTE MIB, SIB1, SIB2 - SIB5; NR: PCI / beams / SSB frequency) +
  each cell's own bearing. Only broadcast system information - nothing
  addressed to a phone (no C-RNTI / P-RNTI / paging) is looked for
- LTE / 5G NR cells share a carrier (frequency reuse 1): one VFO DoA mixes
  them. Every cell scrambles its sync / reference signals with its PCI, so
  correlating with one cell's sequences gives its channel alone - separation
  by CODE, also for cells at the same delay (co-sited sectors, synchronised
  networks), where dtv's path separation separates by delay. Talkers "LTE-<pci>" / "NR-<pci>"
  (spatial, no delay; label + "eNB n / c" once SIB1 is read)
- Broadcast information (LTE; option `decode`, default on): `lte_dl.cpp`
  (`CellRx`: a subframe's 14 symbols on every antenna at the cell's frame
  timing, channel per port 0-3 from the CRS - LS, subframe-averaged with the
  cell's CFO turned back, interpolated across subcarriers, `set_ports`
  limits it to the MIB's count; `txd_llr`: SFBC / SFBC + FSTD over all
  antennas, unseen REs = 0; PBCH -> MIB (4 SFN-mod-4 hypotheses x 1 / 2 / 4
  ports, CRC16 mask = ports); PCFICH -> CFI; `control_layout` (PCFICH +
  PHICH REGs, PDCCH REGs by subcarrier then symbol, quadruplet interleaver +
  PCI shift); `pdcch_llr` (common search space only, CCE 0-15);
  `dci_search` (AL 4 / 8, formats 1A / 1C, SI-RNTI, re-encoded agreement
  >= 0.8)), `lte_pdsch.cpp` (`parse_dci` -> `Grant`: RIV, distributed VRB ->
  PRB per slot (36.211 6.2.3.2), TBS from `lte_tables.hpp`; `pdsch_res` (no
  CRS / PBCH / sync / control); `pdsch_llr`; `TbAcc` = soft combining in the
  turbo circular buffer + coverage), `turbo.cpp` (QPP, encoder, rate
  matching, max-log-MAP with 0.7 extrinsic scaling and CRC early stop),
  `fec.cpp` (CRC16 / 24A / 24B, tail-biting K=7 encoder + 3-pass Viterbi,
  convolutional rate matching), `lte_rrc.cpp` (UPER reader; SIB1 incl. v890
  / v8h0 / v9e0 / v920; SystemInformation with SIB2 - SIB5 field by field,
  extension additions skipped as open types, any other root SIB ends the
  message - `complete = false`), `lte_meta.cpp` (`meta_burst`: the MIB every
  burst = the frames' SFN - two agreeing frames the first time; SIB1 =
  subframe 5 of even frames, RV ((SFN/2) mod 4) for 1C; each SI message in
  its window from SIB1's schedule, RV from the window index, round robin
  over the messages; per cell `META_CELL_S` 0.1 s, per burst `META_BURST_S`
  0.2 s, the cell served longest ago first; read again after 600 s; a
  changed SIB1 value tag clears the SI messages), `lte_info.hpp` (36.101
  band table EARFCN <-> MHz incl. uplink, NR bands, NR-ARFCN, MCC ->
  country). `lte_tables.hpp` (QPP, TBS, TBS 1C) was generated from srsRAN's
  copy of the 3GPP tables (QPP pairs checked to be permutations; its
  TBS[6][0] = 328 corrected to 36.213's 88)
- Output: table Cell, PCI, Mode, Operator, TAC, eNB / cell, Band, (E)ARFCN,
  Bandwidth, Ports / beams, Power, Above noise, Timing, Spread + every row's
  details (`table_detail`: physical, MIB, downlink, SIB1, its DCI, SI fields
  "SIB2 · ...", "Decoding: waiting for ..."); facts LTE carrier, Network
  (PLMN), System information; an event per decoded block
- Partial visibility: a 10 MHz carrier is ~23 % inside the VFO - the DCI
  decodes from the visible REGs, SIBs are combined until the CRC passes. An
  SI message sent once per window (RV 0 every time) shows the same bits in
  every repeat and stays unread when its PRBs are mostly outside (SIB3 /
  SIB5 on the live band-8 carrier: 36-40 % coverage). `MetaParams::verbose`
  (the plugin's verbose option) logs every transmission combined (DCI,
  window index, RV, coverage)
- Any VFO rate with a whole number of samples per 15 kHz works (a wider
  SDR): the usable band is `VIS_FRAC` 0.4375 x the rate (+-1.05 MHz at 2.4
  MHz; `vis_hz_`), the raster reach `search_span()` = it - `SYNC_HALF_HZ`
  0.45 MHz (>= 0.6 MHz), the scan step `scan_step_` 2 x that. The cell
  search runs on a decimated copy (`search_at` -> `search_decim`: the
  largest D with fs / D >= 1.92 (LTE) / 2.4 (NR 15 kHz) / 4.8 MS/s (NR 30
  kHz) and a usable numerology; `decim_taps` / `decimate` in dsp_util.hpp,
  centred FIR, 14 D + 1 taps; `SearchParams::band_hz` = 0.4 x the decimated
  rate limits the sync subcarriers used; hits' sample positions scaled back
  by D; `SearchOut::rate` = the fold's rate for the plot), everything else at
  the whole rate. 2.4 MHz: D = 1 - output byte-identical to before. The PSS
  fold's overlap-save block grows past 1024 when a symbol is > 256 samples.
  Tested (scratchpad wide/): synthetic LTE at 15.36 MS/s and NR at 9.6 MS/s
  off the VFO centre (bearings within 0.5 deg), the live capture upsampled
  to 9.6 MS/s (MIB / SIB1 / SIB2). CPU at wide rates not addressed (mixing
  per search candidate + channel estimation at the whole rate: 0.6-2.6 s per
  burst at 9.6 MS/s on a Pi 5)
- Files: `seq.hpp` (36.211 / 38.211 sequences: LTE PSS ZC, SSS, CRS Gold +
  RE mapping; NR PSS / SSS), `ofdm.hpp` (numerology at the input rate: 2.4
  MHz = 160 samples per 15 kHz symbol, no resampling; `SymFft` = FFT window
  started early inside the CP, the fractional start put back as a phase
  ramp), `search.cpp` (`cp_offset`, PSS fold, image-aware peak choice, SSS
  test), `chest.cpp` (per-cell CRS / SSS impulse responses -> per-tap
  covariance -> the path's covariance), `decoder.cpp` (raster search / lock /
  frequency tracking per technology, tracks, table, plot, facts), bursts 0.3
  s every 1 s
- Carrier: candidates on the LTE 100 kHz raster (|offset| <= 0.6 MHz) / NR
  GSCN (<= 0.3 MHz) around the VFO centre, `SEARCH_PER_BURST` 4 per burst;
  fine offset = the cyclic-prefix correlation folded over 0.5 ms, mean
  removed (CW / DC), modulo the subcarrier spacing; whole-subcarrier
  ambiguity swept (0, +1, -1 per raster sweep: receivers up to ~22 kHz off);
  after 3 sweeps without a cell, search every 4th burst. A candidate counts
  only CONFIRMED cells (a ZC PSS shifted in frequency correlates at another
  TIME: wrong raster positions show peaks + SSS "hits"). Locked: the
  frequency is tracked from the cells (`f_abs`; per cell `df_hz` = the SSS's
  phase against the PSS / dt; a > 300 Hz residual from a z >= 8 cell re-mixes
  the burst once) - the CP estimate is biased when two systems share the
  slice (DSS); LOCK_MISSES 5 bursts. Short burst fragments (< half a burst)
  are ignored
- PSS fold (2 antennas, FFT 1024 overlap-save, 5 ms LTE / 20 ms NR); peak
  candidates strongest first, judged against expected images: `X3` (each
  PSS as received - CP + symbol - against each template, per lag) from the
  peaks taken and the SSS waveforms of the cells found (prefix sums); peaks
  of another N_ID2 within 4 samples are exempt (co-timed sectors = images
  there); max 8 per N_ID2
- SSS: PSS channel by least squares of all three PSS over 9 subcarriers
  (filters precomputed per N_ID2), w = SSS x conj(H), summed over antennas
  and <= 32 occurrences (LTE per frame half; locked: only the carrier's
  duplex), scored by |sum W d| over every N_ID1 (LTE x sf0/sf5 x FDD/TDD),
  robust z (median / MAD of all hypotheses) >= 6; cancellation = the
  detected SSS x its smoothed (+-5 subcarriers) W d, at the same peak and
  for every co-timed (<= 24 samples) found cell before a peak is tested
- Confirmation: `estimate(quick)` (40 slots, port 0) must find a path >=
  `min_snr`; full estimate: LTE CRS ports 0 / 1, symbols 0 + 4 (3-comb, 22.2
  us window from -5.4 us; the cell's CFO slot to slot removed between the
  symbols), 160 slots, energy gate drops TDD uplink slots; port 1 counts
  within 10 dB of port 0 at the same delay; NR SSS of every block (67 us).
  `analyse()`: per-tap covariance, floor = low-trace taps, D = tr(F^-1 C)/m,
  path = strongest or first within 15 dB (`path` option), main lobe minus F
  -> unit-trace covariance, power, snr, spread
- Carrier scan (`scan` option: 0 / 5 / 10 / 20 MHz, default 10; `Scan` in
  decoder.cpp, survives reset()): a whole k=0 raster sweep where the
  receiver is tuned finds no cell -> `kp::Host::tune` to f0 +1.2, -1.2,
  +2.4 ... MHz (SCAN_STEP_HZ: the raster reaches +-0.6 MHz, LTE and NR), a
  scan position's whole raster in ONE burst; found -> centred once (a carrier
  > 5 kHz off the tuning, also without scanning; LTE before NR); nothing
  within the range -> back to f0, stays (`done`). Bursts from before an asked
  retune are skipped; an RF change it didn't ask for (> 2 kHz) = the user's:
  starts over from there; a retune not seen after 15 s is given up, two =
  scan stopped. Host side: `Info::tunes` + wire TUNE 37 (rf_hz text) ->
  Engine (only plugins declaring it, >= 2 s apart; a refusal = an event in
  the decoder's log) -> `DigitalDecoder::set_tune_handler` (DecimatorManager:
  holds only the VFO id) -> `ControlHandler::request_plugin_tune` (checks
  wideband scan mode / the tuner range on the decoder thread, then
  `loop->defer`: VFO looked up there, FREQ:<rf - offset> or
  TUNER_FREQ:<tuner>:.. in independent mode through
  `handle_websocket_message` - saved, sent to heimdall, synced to pages)
- UI (kraken_doa.html): talkers with `sp` and `pl === 'cells'` are kind
  'cell' (📱; list head Cell / Technology · PCI · power, strongest first, no
  "1st" tag, selected line "x dB below the strongest cell"); the DF panel
  section says "N cells" with a sync-correlation tooltip
- Tests: 10 synthetic cases (`tools/cell_gen.cpp`; README table) - every
  cell above the limits found, bearings within 1 deg, no false cells on
  noise; live (scratchpad e2e/: fake_heimdall_rec.py + t_cells_plugin.py +
  t_cells_ui.py on the TEST build): 3 LTE + 1 NR cell within 0.1 deg, UI
  picks; scan (fake_heimdall_multi.py: retunable, set_frequency on the
  control port switches between recordings of one 5 MHz LTE carrier at
  806.0 MHz seen from 808.0 / 809.2 / 806.8 / 810.4 / 805.6 / 806.0; +
  t_cells_scan.py): from 808.0 it stepped 809.2, 806.8, 810.4, 805.6 (~7 s a
  step), found the carrier +400 kHz off, centred 806.000, cells at 40.0 /
  199.9 / 299.9 deg. CPU (Pi 5): ~15-20 % of a core per locked technology.
  Live (2026-10-14, the user's stack on 954.8 MHz, LTE band 8): three FDD
  cells (PCI 191 / 190 / 253, 2 ports each; 190 / 191 co-sited - timing
  +0.04 us) found and tracked, receiver +1.9-2.1 ppm; bearing confidences low
  (0.16-0.34) and the co-sited pair 48 deg apart - multipath, not yet checked
  against the real towers. Broadcast information on 6 s / 20 s captures of
  that carrier (10 MHz, 2 ports, the SI-RNTI DCI in format 1C, AL 4 at CCE
  12, from ~23 % of the control region): the MIB in every frame, SIB1 of PCI
  191 / 253 after 2-8 transmissions (530-01 New Zealand, TAC 41776, eNB
  5330 / 12 and 42599 / 11, band 8), SIB2 after 3-14 (uplink EARFCN 21748 =
  band 8's duplex spacing); SIB3 / SIB5 not (above). Streamed through the
  test build the events arrive the same; UI (scratchpad e2e/t_meta_ui.py:
  the page with a recorded status injected): ▸ toggles, details open, stay
  open across pushes, close; facts. The SDK's offline host now streams
  multichannel files block by block (a 20 s 5-antenna cf32 is 1.9 GB: read
  whole it was OOM-killed; 74 MB now)

**Digital squelch (squelch method "Digital", with the VFO's decoder on):**
- The squelch methods are FFT (0) and DIGITAL (3) - the eigenvalue methods
  (1 / 2: λ ratio with a manual / self-learned threshold) were removed (too
  hard to set up): saved snapshots and old `DEC_SQUELCH_METHOD:EIGEN*` map to
  FFT, `DEC_SQUELCH_EIGEN:` is ignored, MUSIC's eigen gate is gone (only the
  publish hold is left). `SquelchMethod` (types.hpp), snapshot field 6 =
  the method, field 7 = "0" (was the threshold - the column stays)
- In effect only while the VFO's digital decoder is on
  (`DecimatorManager::effectiveSquelchMethod`; else FFT - the choice is kept
  and comes back with the decoder), in EVERY operating mode (it needs no
  MUSIC). Open = the active plugin (fixed, or the one Auto detect locked onto)
  reported a valid frame within `DIGITAL_SQUELCH_HOLD_MS` 1.5 s
  (`digitalSquelchOpen` -> `DigitalDecoder::frames_within`, the engine's
  `last_valid_by_` per plugin) - the pipeline's squelch_open, the audio gate,
  the web mapper
- The DoA (coherent, `dig_doa` in the pipeline): MUSIC runs with its publish
  held (`setPublishHold`) and `TalkerDoa::set_digital(true)` keeps the
  stream (raw <= 500 kHz / chunk ring above, as for packets). Every
  CONFIRMED piece goes in as the covariance of exactly its samples
  (`dig_confirm`: only what wasn't taken yet - `dig_done_` - and is still in
  the ring; >= 16 samples): valid frames with their samples (wire VALID with
  a payload -> `TalkerSpan::signal`), talker transmissions (as they grow;
  "?" too), packets (their own covariance, channel-filtered for AIS).
  `TalkerDoa::update` (rdf sampler, 10 Hz) publishes at most every
  `DIG_PUBLISH_MS` 200 ms: R of the samples confirmed since, averaged with
  the previous like the processor's frames (`getCovarianceAveragingAlpha`)
  -> `MUSICProcessor::publishFromCovariance` (pseudospectrum, peak, 3D
  elevation, stamp - so the DoA plot, map lobe, logger, web mapper, mobile
  DF and the beamformer's steering all see only confirmed samples). Nothing
  confirmed = the published DoA stays at the last confirmed one
- Plugin API: `kp::Host::valid(start_s, end_s)` (Host::time() of the frame's
  first sample / its end; counts as a valid frame; default = valid()) ->
  wire VALID payload "start\0end" (old hosts ignore it) -> `on_valid_span`
  (vfo_pos mapping, <= `MAX_SIGNAL_SPAN_S` 3 s). Lib protocols:
  `RxContext::valid_at` + `dig::report_valid(ctx, mode, a, b)` with receiver
  sample indexes, `Bridge::clock(rate)` / `restart_clock()` (in
  `talkers(rate)` too): p25 (frame_a_..frame_b_, a TSDU ends after its last
  good TSBK), dmr (burst), nxdn (frame; the CAC path computes its own),
  dstar (RF header; a superframe from the previous data sync), tetra (the
  slot), mpt1327 (CCSC / SYNT + address codeword). Standalone: pocsag (a
  synced batch with <= 2 bad codewords - replaces the old valid() per
  address codeword), aprs + v23_telemetry (frame end - length at 1200 bit/s),
  radiosonde (`FskRx::clock` per block -> `FskFrame::t_start / t_end`;
  `Frame::t_start / t_end`; DFM / LMS6 from the FSK frame that completed the
  position; iMet-4 from its UART bytes). The offline host prints "valid
  frames with their samples: N, covering X s" (`--verbose`: each one)
- UI: the VFO card's method list = "FFT Peak (dB)" + "Digital (decoded
  frames)" only while the decoder is on (`sqEffMethod`, `digDecoderOn`;
  the card rebuilds when the decoder goes on / off); Digital hides the dB
  slider (and the spectrum's squelch line) and shows a one-line note
- Tests (scratchpad acal/): `dig_sim.py` (POCSAG bursts from 60 deg every
  4 s + an FM interferer in the gaps from 200 deg, 5-element UCA r 0.5 m at
  145 MHz; `file` / `serve` = a fake heimdall) + `t_dig.py` against the
  test build: no squelch 47 / 53 % pager / interferer bearings, Digital 100 %
  pager, open 50 % of the time (expected 49 %), FFT at 5 dB 44 / 56 %,
  Digital with the decoder off = FFT, EIGEN_AUTO -> FFT; `t_sq_ui.py`
  (headless Chromium: methods offered, rows, OPEN / CLOSED following the
  bursts, decoder off / on). Offline: pocsag batches within ~1 ms of the
  true spans; radiosonde recordings unchanged frame counts (RS41 119, M10 /
  M20 120, iMet-4 119, iMet-54 241, LMS6 120, noise 0). P25 / DMR / NXDN /
  D-STAR / TETRA / MPT1327 spans: built, not tried on recordings (none here)

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
- `kp::Info::lock_vfo` (ism, radar, cells, dtv; `--info` "lock_vfo", `PluginInfo::lock_vfo`,
  the plugin list's `lock_vfo`): the same lock with no frequency - the
  bandwidth + `SET_DECIMATOR_FREQ:id:0`, the tuner left alone
  (`digFixedPlugin` returns such plugins too, `digApplyFixedTune` skips the
  retune). Fields' tooltip: "... keeps this VFO on its tuner's centre with
  the full bandwidth - tune the receiver instead". `mkTune` (📍 marker
  frequency) on such a VFO retunes the tuner (FREQ: whole kHz, VFO stays at
  0) where a fixed-frequency one refuses
- Test: scratchpad t_adsb_lock (coherent + independent); e2e/t_ui_wide.py
  (lock_vfo: fields + reason, the commands a pick sends - no FREQ, ADS-B as
  the control sends FREQ:1090.000 -, the marker paths, DF devices);
  t_cells_lock.py (cells: offset 0 / 2.4 MHz without a FREQ, no grab box,
  marker tune, another decoder unlocks). The cells / dtv branch called the
  same lock `kp::Info::whole_band` before the merge (2026-10-15): one name
  now; `--info` "whole_band" still reads as lock_vfo

**Radiosonde (`plugins/radiosonde/`, weather balloons; README.md there):**
- One plugin, eight families side by side (`sonde::Type` each, sonde.hpp):
  rs41.cpp, dfm.cpp, m10.cpp (M10 + M20), imet4.cpp (AFSK), imet54.cpp,
  lms6.cpp, mrz.cpp. 48 kHz in; two channel filters (+-7 kHz: the 2400-4800
  Bd types; +-12 kHz: M10 / M20 9600 Bd, iMet-4 AFSK) -> FM discriminators
  shared by all types. min_vfo_rate 24 kHz
- fsk.cpp `FskRx`: symbol-long moving average (integrate & dump) on the
  discriminator, NPH timing phases each on a FIXED search grid with its own
  sync-word shift register (both polarities); a capture runs on its OWN
  clock with a Gardner loop. (The first version moved the search grid with
  the timing loop: running on the dead air after a frame it pulled every
  phase to one instant and the next sync was missed - 94 of 118 RS41 frames.)
  DC + deviation from the sync's 1s / 0s, soft symbols normalised to +-1.
  Several phases catch one frame: types drop captures within ~0.3 frame of
  a decoded one (`last_t0_`)
- rs.cpp: generic GF(256) Reed-Solomon (BM / Chien / Forney; cw[i] = x^i):
  RS41 (0x11D, fcr 0, prim 1, 24 roots), LMS6 CCSDS (0x187, 112, 11, 32).
  LMS6: the known sync bytes 00 58 F3 3F B8 are prepended as perfect symbols
  so `kp::viterbi_decode` starts in the right encoder state; LMS frames span
  consecutive blocks (stream of RS data bytes, broken by a failed block / gap)
- decoder.cpp keeps per sonde (type + serial) the last value of every field
  (`Frame`, NAN = not in this frame) and publishes at most 1/s: facts (one
  sonde: the latest), table row, map point kind "balloon" (ttl 3 h; the page
  keeps balloon trails 4 h / 4000 points), events: new sonde, standard
  pressure levels passed on the way up (pressure measured or ISA at the GPS
  altitude, "(est.)"), burst (300 m below the maximum > 3 km, descending),
  landed (30 s still after burst). Velocity for types without it from the
  fix 3-10 s back (5+ satellites). freq_error = the frame's DC (AFC)
- Validated against the radiosonde_auto_rx recordings + the rs1729 decoders
  as reference (results in the README); `SONDE_DUMP=1 decoder --file ...`
  prints every frame for such comparisons. The flight logic was tested with
  a simulated 30 km flight (scratchpad harness including decoder.cpp)
- Not supported: RS92 (needs ephemeris), 1680 MHz types, Meisei, MTS01.
  rs1729/RS is GPL-3.0: used as documentation + test reference only
- DoA per sonde (`Radiosonde::talker`, ID separation): every frame a
  packet of type + serial, its first <= 0.24 s - see *DoA per talker*

**DME / TACAN (`plugins/dme/`, README.md there):**
- 2.4 MHz input (`min_vfo_rate` 600 kHz, manual only, talkers). From
  `Host::rf_hz()` every integer MHz of 962..1213 within +-0.8 MHz of the
  VFO centre is a channel (no RF: the centre, unnamed). FFT channelizer:
  4096 points, overlap-save (OVL 1024, hop 3072), per channel the bins
  around its centre (raised cosine flat +-350 kHz, zero +-550 kHz) -> 2048
  IFFT = 1.2 MHz, the block's shift phase undone (continuous carrier
  phase). Channel sample K = input sample in0_ + 2K; `abs_in_` never resets
  (Host::time()). FFTW_ESTIMATE: MEASURE planned for 32 s on the Pi
- Pulses: noise = median magnitude (every 7th sample, ~50 ms, smoothed),
  threshold `min_snr` (10 dB, peak power over median^2 / ln 2), local max
  +-3 samples, half-amplitude width 2.4-5.6 us (SSR 0.8 us pulses fail),
  leading edge at 50 % interpolated, rise 10-90 %, phase steps for the
  carrier. Pairs: 12 / 30 / 36 us +-1, <= 4 dB apart, nearest spacing wins;
  `role_of(mhz, kind)`: 12 us on 962-1024 / 1151-1213 = X replies, 30 us on
  1025-1150 = Y replies, 12 / 36 us on 1025-1150 = X / Y interrogations of
  channel mhz - 1024 (`nav::dme_by_reply` / `dme_by_interrogation`)
- Ident: a reply pair with one 1/1350 s (+-5 us, refined while keyed)
  earlier is "periodic"; per 10 ms tick key = >= 6 pairs and most periodic,
  3-tick majority -> `nav::MorseDecoder` (unit 0.07-0.2 s, end gap 1.2 s);
  reported when clean (no '?') with timing fit >= 0.6
- TACAN: reference bursts = runs of >= 5 equally spaced 12 us pairs every
  20-36 us (>= 9 = north) or >= 9 single pulses at 12 / 15 / 30 us (Y: 30 =
  north, 15 = auxiliary; X pulses every 12 us: >= 18 = north) - the
  sources disagree on the X spacing, both are taken; TACAN = 10+ north
  bursts 1/15 s apart in 2 s. Reply pulses wait 0.1 s (`tpulses`), then
  those outside bursts with a turn (north burst before and after, 1/15 s
  +-5 %) go into a 5-term least-squares fit (c0, 15 Hz, 135 Hz; decays over
  ~3 s): bearing = 90° + the 15 Hz maximum's phase (north burst = maximum
  east, clockwise), refined by the 135 Hz candidate nearest it
- Output: table row per channel AND pair kind (`row_key` mhz/kind:
  replies and interrogations on one frequency are separate rows;
  interrogations "about N aircraft" = clusters of pair levels > 2 dB
  apart), facts for the busiest reply channel (+ "Channels heard",
  "Interrogations"), events (heard, ident, TACAN, quiet), raw = idents.
  `host.valid()` every 0.25 s with >= 3 pairs (the Digital squelch holds
  1.5 s). DoA: clean pairs (no pulse between, none 15 us before / after),
  >= 15 dB, token bucket `doa_rate` (100/s) -> Talker packet id = channel
  ("40X"), label "IAA · 1001 MHz", start - 4 us .. second pulse + 8 us (>=
  2 whole 16-sample chunks), freq_hz = the channel's offset, avg_s 10
- CPU (Pi 5): 3.3 % of a core per channel (scratchpad bench); the offline
  host's double sin/cos NCO doubles the figure it prints
- Tests (scratchpad dme/): the live 40X recordings (90 s / 480 kHz in the
  AI session, 20 s / 2.4 MHz), gen_dme.py synthetic A (18Y + 81X
  interrogations + Y TACAN 237°), B (weak 18X + X TACAN 123.4°), C (SSR
  on 1030 MHz), D (no RF), N (noise); end to end feed.py + t_dme_e2e.py
  (beacon 69.4° vs whole VFO 198°), t_dme_sq.py (Digital squelch: VFO DoA
  69-70°, open 20/20), t_dme_ui.py (decoder tab, DF panel)

**Pulse radar (`plugins/radar/`, README.md there):**
- Every pulsed radar in the 2.4 MHz the tuner receives (L-band ATC /
  en-route surveillance 1215-1400 MHz, wind profilers ...): `sample_rate`
  = `min_vfo_rate` = 2.4 MHz, `lock_vfo`, manual only, talkers. No protocol:
  ELINT-style measurements of each radar's pulses, PRIs and antenna
- `pulse.cpp` `Detector`: per sample |x|^2 + a 3- and a 24-sample sum (10
  us: weak long chirps that fade across their sweep) against the noise
  (median of 64-sample means), the long threshold 6 dB under the short
  (`sens` 10 dB); processed in chunks that don't wrap the ring (2^15
  samples, 13.7 ms - fits the L2 cache). A segment's pulses:
  runs of (3-sample mean > short OR 9-sample mean > long threshold) apart by
  > 4 us; edges = the OUTERMOST -6 dB crossings (raw / 3 / 9-sample power),
  corrected by W / 4 per side (a rectangular pulse comes out exact - the
  outward search from the peak split fading chirps into 3-5 pieces live);
  frequency from phase steps, LFM rate from 6-sample pieces fitted against
  time; clipped = an I / Q sample >= 0.96 full scale
- `decoder.cpp`: pulses -> modes (width / frequency / chirp tolerances,
  medians of 256) -> a radar when a mode has >= 20 pulses in 4 s and a PRI
  structure (`lib/radar_pri.cpp`). Folded into an existing radar (on the real
  recordings a single 60 us chirp radar first came out as 9-15 "radars"): pieces of its weak
  chirps (`piece_of`: same rate, shorter, inside its sweep - its start
  placed by the piece's frequency, one pulse per chirp, no PRI / shape),
  `belongs()` = modes on its PRI grid (a pulse merged with its echo is
  longer, its sweep bent, but comes one PRI after the radar's pulse), its
  PRIs on its frequency, ghosts with its PRIs >= 20 dB down (an image of a
  strong radar from outside the band), `fold_pieces` also radars with its
  scan period on its frequency; `try_link`: a fixed delay (+-1.5 us, 70 %)
  at comparable power = another pulse of its group (frequency diversity /
  fill pulse), >= 10 dB down = its echo (alias). A weaker copy of a pulse
  within 0.3 PRI = an echo, dropped. `merge_modes` joins duplicate modes
- `lib/radar_pri.cpp` (shared with ssr1030): differences 100 us - 30 ms clustered +-max(1.5 us, 0.12 %),
  sums of 2-3 levels with fewer members = missed pulses; kinds constant /
  blocks (runs, the level cycle without breaks) / stagger (period) / mixed
  / jitter; `straddles` = a set switched inside the window (not recorded).
  PRI sets the radar switches between are listed (live: per turn)
- `lib/radar_scan.cpp` (shared with ssr1030): levels in 5 ms bins x 8192 (41 s); main-beam passes = bursts
  within 6 dB of the 41 s max that DWELL there >= half the longest burst's
  time (a reflector lit by the beam elsewhere - and, with the main beam
  clipped, one only ~10 dB down reaches the clip level too - is shorter up
  there); stored passes re-checked every second (the first turn's sidelobe
  bursts drop out); `scan_period` = the spacing most passes repeat at, a
  least-squares fit over the chain; `fit_lobe` = robust parabola in dB over
  the lobe's unclipped top 10 dB (contiguous from the top, not the
  sidelobes) -> -3 dB width; none from a clipped pass
- DoA (ID separation by signature): talker id = frequency to 0.1 MHz
  ("1355.0 MHz", "#2" ...), packets = each candidate pulse +-24 samples
  (>= 2 whole covariance chunks), `freq_hz` = its centre, `bw_hz` = sweep +
  2 / width (> 250 kHz: kraken_doa's chunk ring, the whole band), avg_s
  `doa_avg` 30, keep_s `keep` 1 h; judged DOA_HOLD_S 0.3 s after the pulse:
  unclipped, >= 15 dB SNR, not a piece / alias, within `doa_db` 15 of the
  scan's peak, in a burst whose 150 ms max is within 6 dB of it and whose
  dwell near it (+-0.3 s) >= half the main-beam passes' (`mb_dwell`); only
  after 2 passes (or 30 s: not rotating); <= 60 / s. `valid(start, end)` per
  packet, `valid()` while a radar's pulses come
- Outputs: table (Radar, MHz, Pulse, PRF, PRI, Scan, RPM, Beam, Peak dBFS,
  SNR, Pulses/s, Main beam) + details, facts (strongest radar), events,
  plot (the strongest radar's level over one turn, 0 = main beam on us,
  marker = its talker, threshold = the DoA level; the DF panel's plot
  section names it "radar ... pulse level over one turn"), raw = a line per
  main-beam pass. `RADAR_DUMP=1` logs pulses / modes / passes (offline)
- Generator `tools/radar_gen.py` (scene JSON -> every antenna cu8 / cf32 in
  0.1 s pieces; pulse groups, LFM, PRI kinds + alternating sets, rotating
  antenna, a lit reflector's echo; refuses a scene named like its output's
  sidecar); `tools/scene_three.json`
- Tests (scratch t2/): three radars (scene_three) offline + end to end
  through kraken_doa (fake heimdall): 60.0 / 200.0 / 300.0 deg exact, the
  parameters (README table); a clipped main beam + a reflector -14 dB:
  140.0 (truth; 253.6 = the reflector before the dwell rule); noise 0
  pulses; UI t_ui_radar.py (headless Chromium, 16 checks). Real Auckland
  1355.0 MHz (live + recordings): 59 us LFM -1.17 MHz, PRI 1712 / 1395 us
  blocks of 8 alternating per turn with 1748 / 1424, scan 4.88 s, beam 1.5
  deg (12 dB gain), its short pulse on 1350.07 MHz; array 116-119 deg (~255
  compass). CPU (Pi 5): plugin 2.2-2.9 % live

**1030 MHz interrogations (`plugins/ssr1030/`, README.md there):**
- SSR / Mode S / ACAS interrogations, the uplink of the 1090 MHz replies:
  `sample_rate` = `min_vfo_rate` = 2.4 MHz, `fixed_freq_hz` 1030 MHz (like
  adsb: picking it tunes the VFO + tuner), manual only, talkers
- `front.cpp`: raw power per sample (0.8 us pulses are 2 samples, P1 -> P2
  / S1 -> P1 / P3 -> P4 only 2 us apart): segments of samples over the
  threshold (`sens` 10 dB over the noise = median of 64-sample means, >=
  a quarter LSB^2), short pulses = local peaks with their edges at a
  quarter of the peak (interpolated, a valley ends a pulse); a LONG pulse
  (Mode S P6) = a run of the 5-sample mean over threshold - 3 dB for >= 10
  us, split at its first deep valley in the first 3 us (P1 / P2 merge into
  the mean); a 2^15-sample ring keeps the samples for the DPSK
- `interr.cpp`: a pulse waits 40 us, then as P1: Mode S = a long pulse from
  +1.7..3.9 us (P2 merged or not) of 17-23 / 30-37 us total; else P3 at
  8 / 17 / 21 / 25 us (+-0.3, same level +-3 dB, 0.35-1.45 us wide), P2 at
  +2 (any level: SLS), P4 at P3 + 2 (width >= 1.2 us = long), S1 at -2
  (not stronger). A B / D whose +2 us pulse has an A / C of its own = an S1
  (ACAS steps came out as Mode D)
- `dpsk.cpp`: P6 timing (12.5 ns grid over 3.25-3.75 us after P1) + P1 /
  P2 / P6 amplitudes by least squares on the known part (to t6 + 1.45 us);
  per-survivor Viterbi over the chips (32 states, chips 0-4 +1, 5-6 -1, then
  the data, 2 guard chips; g(x) = S(x) - S(x - 0.25 us) over -1 .. 4.25
  us); carrier offset from the decoded frame's phase slope, a 2nd pass; the
  lower residual wins. S = complex step response on -1 .. 4 us / 32 per us,
  default `s_default.h` (measured here: least squares over checked live
  frames), re-fitted (Cholesky, smoothness 5 % + 0.4 % pull to the table)
  from the normal equations of checked frames (forgetting 1/300; known
  parts of strong frames added at weight 0.3, refits only on checked ones -
  a preamble-only bootstrap was tried on synthetic other receivers and
  didn't converge: removed). ~0.5 ms per frame
- `uplink.cpp`: CRC-24, the uplink address encoding (top 24 coefficients
  of A(x)G(x)) and its GF(2) inverse, fields per UF; UF11 / UF16 corrected
  against the all-ones address (two bit errors 1-3 apart or one in the last
  8 - a wrong run of chips; ~1 in 50000 random frames would pass)
- `track.cpp` (the ID separation): SSR = sequence search on unattributed
  all-calls (difference histogram 20 us bins, +-2 % smoothed local maxima,
  shortest PRI first; chains at n = 1..3 x P within JIT 3.5 %, >= 10
  members, >= 50 % of the slots; a chain at a multiple of a known SSR's PRI
  or one that went quiet = that SSR), live tracking within 4x its own mean
  PRI deviation x sqrt(n) (n <= 5) and 9-12 dB; sidelobe all-calls (P2 >=
  P1 + 2 dB) by the omni P2's absolute level (+-3 dB); UF11 within 0.6 ms of
  a slot; roll-calls by the checked II of an SSR (IIS / SIS), else within 4
  PRIs at its level. Fixed = three bursts (gaps <= 25 ms) at one level
  (+-3 dB, >= 13 dB) within 15 s, then everything at that level that may be
  a fixed one's (`fixed_ok`: no S1, no UF16 / UF11, no P2 >= -6 dB A / C,
  no B / D); bursts > 3 s apart whose all-calls keep an SSR's PRI = that
  SSR's main beam, not fixed. ACAS = whisper-shout sequences (Mode C with
  S1, steps <= 60 ms apart, not falling > 4 dB, >= 3 steps, half with S1)
  matched ~k s later (+-12 %) at +-8 dB; a checked UF16 broadcast = an ACAS
  by its address (also without whisper-shout); UF0 / UF16 prefer an ACAS
  heard within 1.5 s at +-6 dB. Scan / beam with `radar_scan`, PRI with
  `radar_pri` (moved from the radar plugin to `plugins/lib`, identical
  radar output checked), interlace = the A / C letter cycle over the slots
  + the UF11 spacing. DoA candidates judged 0.3 s later, dropped beyond 1 s
  (kraken_doa's 1.5 s ring; the discovery replays up to 4 s back)
- Outputs: table (Interrogator, Type, Interrogations, PRF, PRI, Scan, RPM,
  Beam, SLS P2, Mode S, Targets (>= 3 sightings), Level dBFS, SNR, Per s,
  Main beam) + details, facts, events, plot (strongest SSR over one turn;
  the DF panel's plot section shows it as "🛰 ... interrogation level over
  one turn"), raw = each Mode S frame's hex. `SSR_DUMP=1` / `SSR_PULSES=1`
  offline logs
- Generator `tools/ssr_gen.py` (SSRs, fixed, ACAS at known bearings,
  every antenna, real formats / parity, a receiver response "measured" or
  "lowpass", echo, df); `tools/scene_four.json`
- Tests (scratch t3/, details in the README): scene_four offline + end to
  end 60.0 / 150.0 / 200.0 / 300.0 deg exact, noise nothing, DME pairs on
  1030 ignored (dme sees them, not the SSR pulses), UI headless Chromium;
  live: SSR II 10 (199 Hz, scan 5.00 s, 2.6 deg, A / C + UF11 every 3rd,
  215 deg array frame), multilateration 1 s bursts (276 deg), TCAS by
  broadcasts; interrogation -> reply pairs with adsb on a 1090 MHz tuner
  (independent mode) confirmed the address encoding. CPU ~4 % of a core

**UAT (`plugins/uat/`, 978 MHz, README.md there):**
- The US's second ADS-B link + its ground stations' uplink: `sample_rate`
  2.4 MHz, `min_vfo_rate` 2 MHz, `fixed_freq_hz` 978 MHz (like adsb),
  manual only, talkers, map. Not used in NZ: tested on synthetic signals
  and on dump978's sample data (real payloads) re-modulated
- CPFSK 1.041667 Mbit/s, h 0.6: 2.304 samples per bit. Gate = 32-sample
  block power > 2x the noise floor (lowest 1024-sample mean, smoothed); in
  a hot stretch the discriminator (fast atan2) smoothed over 2 samples,
  linearly interpolated at bit centres, correlated with the 36-bit sync
  (+ = ADS-B 0xEACDDA4E2, - = uplink, its complement); peak refined to 0.1
  sample, levels from the sync (<= 4 bit errors), Gardner timing loop (mu
  0.03 sample per transition; +-80 ppm clocks fine)
- RS (`rs.cpp` = radiosonde's generic GF(256) decoder; on-air byte i =
  x^(n-1-i), a "correction" into the shortened zeros = failure): long
  RS(48,34) with type != 0 first, then basic RS(30,18) type 0; uplink 6 x
  RS(92,72) de-interleaved (on-air 6 i + b). poly 0x187, fcr 120, prim 1
- `msg.cpp`: header / state vector / mode status / aux SV (dump978's field
  positions = DO-282B's), uplink header + information frames + FIS-B APDU
  header (product id, t_opt time), DLAC text -> records (`text_records`),
  product names. `UAT_DUMP=1` (offline) logs every message's fields
- Outputs: table (Address, Source, Callsign, Category, Squawk, altitudes,
  V/S, GS, Track, Lat / Lon, distance / bearing, Air / ground, Emergency,
  NIC / NACp, Signal, Messages; details: mode status / ground station
  products + latest text records), map (aircraft, vehicle, point, ground
  station kind station), events (new aircraft / TIS-B target / station,
  callsign, first position, products, ⚠ emergencies + unusual categories),
  raw = dump978's text format `-hex;rs=;ss=` / `+hex;...`. Options `tisb`
  (show TIS-B / ADS-R traffic), `fisb` (text records in the event log)
- ID separation: see *DoA per talker* (aircraft by address; TIS-B / ADS-R
  and uplinks = the ground station "GS site N")
- Generator `tools/uat_gen.py` (scene JSON: adsb / tisb / uplink sources or
  `replay` of a dump978 text file; real RS, CPFSK + Gaussian BT 0.5, clock
  ppm, carrier offset, 5-element UCA, 0.1 s pieces, cu8 / cf32);
  `tools/scene_five.json`
- Tests (scratch t4/): dump978 sample data (1143 real messages) re-modulated
  -> 1143 decoded byte for byte; the generator's RS = Karn's decoder with
  dump978's parameters on 9326 codewords; fields vs dump978's uat2text
  (built as a reference): 439 / 439 ADS-B identical (speed +-1 kt
  rounding), 704 / 704 uplinks, 87 / 88 text records. Sensitivity: ADS-B
  99 % from 6 dB per-sample SNR (2.4 MHz), uplinks 91-100 % from 6-8 dB;
  noise nothing. scene_five offline + end to end: 5 talkers (4 aircraft,
  the ground station incl. its TIS-B) 0.0 deg off; UI headless Chromium
  (✈ / 🗼, the station's details + METAR, DF error 0.0). CPU 2.1 % of a
  core live (end to end)

**FLARM / OGN (`plugins/flarm/`, README.md there):**
- Glider / GA collision-avoidance radios: `sample_rate` = `min_vfo_rate`
  2.4 MHz, `lock_vfo`, `tunes` (once, when no plan channel is in the VFO:
  option `band` - auto from the station location -> 868.8 MHz for EU / NZ,
  921.6 AU / S. America, 915.2 North America, 916.6 Israel; option `tune`
  off = never), manual only, talkers, map. Channels: every plan channel
  within +-1.05 MHz of the VFO centre (868.2, 868.4, 868.8, 869.25, 866.0,
  920.9, the 902.2 + 0.4k grid of the US / AU hopping bands, option
  `extra`); no RF known (offline without --rf) = a 100 kHz grid
- Front end: 256-point FFT every 256 samples, Hann; power in +-75 kHz per
  channel vs its gated-mean floor > `sens` (6 dB); a 2-14 ms burst, decided
  4 frames after it ended (the strongest of overlapping bursts within 250
  kHz), mixed down from a 2^17-sample (55 ms) ring, 49-tap 110 kHz low-pass
  decimated to 400 kHz, fast-atan2 discriminator, summed per chip (4
  samples); sync search for both (last 56 chips of F531FAB6 / 0AF3656C,
  either polarity, <= 6 chip errors), timing +-1 sample tried. Bursts
  starting together (+-2 frames) on 3+ channels spanning >= 600 kHz = one
  broadband event, skipped (live: heimdall's calibration noise after a
  retune hit all four EU / NZ channels at once). `FLARM_DUMP=dir`: every
  demodulated burst's 400 kHz baseband + a log line (live diagnosis)
- FLARM: 26 bytes, CRC-16 CCITT (init FFFF) over 31 FA B6 + 24 bytes (also
  the inverted bits); ID bytes 0-2 LE + address type (byte 3 bits 4-5) only.
  The encrypted 20 bytes (unpublished keys) are NOT decoded - by the
  project's rules (no brute force / reverse engineering of undisclosed
  encryption)
- OGN (`ogn.cpp`, `ogn_ldpc.h` = the protocol's LDPC(208,160) parity-check
  matrix): hard check, else normalised min-sum (0.75, 16 iterations, <= 16
  bits changed); header parity checked; TEA (all-zero key, 8 rounds)
  de-whitening; position / status / info decoded (info: personal strings -
  pilot, crew, ICE, pilot ID, task - dropped); `Encrypted` = address only;
  `Stealth` = no map point, no position in table / talker / events (the
  bearing stays, "no-track"); relay > 0 = no talker
- Outputs: table (Aircraft, Protocol, type, Registration / CN, altitude,
  climb, speed, track, Lat / Lon, distance / bearing, Notes, MHz, Signal,
  Packets; details), map (trackable OGN only), events, raw `FLARM|OGN hex
  MHz dB`; table_keep 30 min
- Generator `tools/flarm_gen.py` (FLARM: real header + CRC, random bytes
  for the encrypted part; OGN: real fields, whitening, LDPC from the
  plugin's matrix by GF(2) elimination - = the tracker's own generator
  table; relays `same_as`; channels per packet; GFSK BT 0.5);
  `tools/scene_six.json`
- Tests (scratch t4/): scene_six (2 FLARM, OGN glider, no-track OGN
  paraglider, owner-encrypted OGN, a relay; three packets at the same
  moment on 868.2 / 868.4 / 869.25): offline 44 / 44 packets, 5 talkers
  0.0 deg, the relay none; end to end 0.0 deg (285 / 289 packets with a
  bearing); UI headless Chromium (kind flarm, 🪂 / ✈, Every aircraft,
  distance + DF error, nothing of the no-track one's position).
  Sensitivity (160 kHz channel SNR): FLARM 98 % from 9.8 dB, OGN 100 % from
  7.8 dB (LDPC). CPU 2.9 % of a core live (end to end). Live (real array,
  2026-10-15 09:15-09:30, a weekday morning): no FLARM / OGN; the plugin
  retuned the receiver from 922.98 to 868.8 MHz itself (band auto: 868.8 MHz);
  a periodic narrow-FSK device near 868.166 MHz every 16 s and the
  calibration noise demodulated, nothing reported; 2.6 % of a core

**ILS / VOR / marker (`plugins/ils/`, README.md there):**
- 48 kHz (`min_vfo_rate` 48 kHz: the VOR subcarrier reaches +-10.4 kHz).
  Every 0.5 s the last 1 s: one complex FFT (1 Hz bins); carrier = the
  strongest bin within +-20 kHz >= 15 dB over the median (Hann power from
  the unwindowed bins: 0.5 X[k] - 0.25 X[k-1] - 0.25 X[k+1]); a second
  carrier 4-32 kHz away, not at +-9960 Hz, a line (30x what is 30 Hz
  aside), >= 20 dB, carrying 90 / 150 Hz too = two-frequency ILS
- `demod`: the carrier's bins +-bw (12 kHz; sep - 1.5 kHz next to a second
  carrier) -> 24000-point IFFT -> envelope -> m = e / mean - 1 -> r2c FFT:
  tone depth = 2 sqrt(sum |M|^2 over +-w bins) / N (30 / 90 / 150 / 400 /
  1020 / 1300 / 3000 Hz, 9960 +-800). VOR: bins 9960 +-900 -> 2048 IFFT ->
  instantaneous frequency (times between samples) -> DFT at 30 Hz = the
  reference phase; radial = arg(R) - arg(M[30]) (the variable lags the
  reference by the radial); circular 1 s blocks hold whole 30 Hz cycles,
  so no filter delays; averaged as vectors (0.8 / 0.2)
- Type (`judge`, three agreeing blocks): VOR (9960 Hz > 10 %, 30 Hz > 8 %,
  FM 250-800 Hz), ILS (SDM 0.2-1.2, a tone > 8 %, no 30 Hz; LOC / GS by
  the RF band, else SDM < / >= 0.6), marker (a keyed 400 / 1300 / 3000 Hz
  tone > 30 %; within 0.5 MHz of 75 MHz when the RF is known - airband
  voice has keyed-looking 400 Hz bursts), else "AM carrier". CPU (Pi 5):
  0.7 % of a core
- Keying: 10 ms Hann Goertzel ticks of the middle 0.5 s of each block
  (contiguous with the 0.5 s hop); `Keyer`: 98th / 15th percentile of 30 s,
  keyed when >= 6x apart (noise alone ~5x) and >= 2 %, hysteresis; the
  1020 Hz Morse starts after a 1.2 s gap once keyed (else the first ident
  is cut); markers' / glide slopes' keying never makes an ident. Facts
  (1 s): navaid, frequency + pairing (`nav::` tables), carrier, ident,
  modulation (per type), course / glide path (DDM, side, needle uA, fly
  left / right / up / down, SDM), second carrier, radial (+ the station's
  bearing; true with the `magvar` option), marker, audio (LOC / VOR only).
  `valid(t0, t0 + 1)` per recognised block
- DoA per navaid (ID separation, `talker`): LOC / VOR blocks become
  packets of "<ident> LOC|VOR" while the ident is fresh (30 s, same carrier
  within 50 Hz) - see *DoA per talker*. Generator `tools/ils_gen.py`
  (LOC DDM / VOR radial + keyed 1020 Hz ident)
- Tests (scratchpad dme/gen_nav.py, t_vor.py): see the README

**ISM devices (`plugins/ism/`, 315 / 345 / 433 / 868 / 915 MHz; README.md there):**
- The whole 2.4 MHz the tuner receives (`sample_rate` = `min_vfo_rate` =
  2.4 MHz), `lock_vfo` (picking it puts the VFO on its tuner's centre at
  2.4 MHz, a locked line - see *Fixed-frequency decoders*), manual only,
  talkers. Weather stations, TPMS, key fobs / remotes, alarm sensors,
  energy meters: 69 protocol decoders (~140 model names, rtl_433's naming
  and JSON keys). Protocol notes from rtl_433's documentation (GPL-2.0,
  documentation + rtl_433_tests recordings as test data only); own
  implementation. (A 240 kHz variant - the same decoders on a 240 kHz VFO -
  existed first; removed 2026-10-12: the full band did everything it did at
  about the same CPU, for ~60 MB more memory)
- `frontend.cpp`, `FrontEnd(10)` (input = 10 x `ism::FS`, 240 kHz):
  640-point FFT every 320 samples (3.75 kHz bins, 133 us hop; per-bin arrays
  in frequency order), noise floor per bin (gated mean, a constant carrier
  absorbed in ~10 s); a bin is active when it + its neighbours in this frame
  and the one 640 samples back hold the threshold (option `sens`, 10 dB; 0
  false bursts in 20 s of noise at 8 dB). Active runs of bins go to
  PACKAGES by frequency (`max_pkgs_` 8 at once):
  within `MARGIN_BINS` 6 of a package's region = that package; a package's
  region grows during its first `LEARN_S` 50 ms (strong segments only - the
  second tone of a wide FSK burst; CurrentCost sends 33 ms of one tone
  first); later a strong segment elsewhere (>= -20 dB of the strongest
  active one: not splatter) starts its own package = another device
  transmitting at the same time on another frequency (demodulated and its
  DoA filtered on its own - two overlapping fobs got each other's bearing
  before). Packages ending within 2 ms are merged (one transmitter's
  tones). A package ends after `GAP_END_S` 30 ms of quiet or `MAX_LEN_S`
  1.2 s (a Prologue sends 7 copies in 0.95 s; a held fob is cut)
- A finished package is mixed down by its centre and decimated to 240 kHz
  (`make_segment`: Blackman 321 taps, cut-off 108 kHz - flat to ~88 kHz,
  -74 dB from 128 kHz; computed at every 10th input sample only; input ring
  2^22 = 1.75 s, 32 MB); its noise = the median floor of the 64 bins around
  it x sum(h^2) (the RTL's 2.4 MHz isn't flat). Positions are in 240 kHz
  samples from here on (every protocol works at 240 kHz), `Burst::fc` is the
  offset in the whole band (talker `freq_hz`, device MHz). `FrontEnd(1)`
  still takes a 240 kHz input and demodulates from the ring directly
- Demodulation of that segment, two passes: a wide filter finds the carrier
  or the two FSK tones (2-means of the discriminator; the middle must lie in
  the occupied band - near the band edge a noisy discriminator suggested
  -140 kHz), then a filter centred on them: OOK = envelope > half the
  signal level (hysteresis, >= 3x the noise), runs < 18 us merged, dropouts
  < 40 us next to a 2x longer pulse and spikes < 22 us merged; FSK (higher
  tone = pulse) = the discriminator sliced between tracked tones, or for
  tones >= 50 kHz apart (HopeRF-style +-60 kHz: Bresser, IT+, WH24...) the
  two tones' own filtered levels compared - a tone near the band edge,
  attenuated by the VFO filter, broke the discriminator; a carrier dip < 20
  us keeps the tone (Renault)
- `bits.cpp`: slicers with rtl_433's r_device parameters and semantics
  (PWM, PPM - also its empty rows -, PCM NRZ / RZ with its preamble-based bit
  width and the gap_limit / long cap on trailing zeros, Manchester zero-bit
  - its prepended 0 -, DMC, PIWM); rows carry their sample span (the DoA) and
  their message (`Rows::split()`: rows between gaps > reset_limit = one
  rtl_433 decoder call - one burst can hold several). CRC-8 / -8le / -16 /
  -16lsb, LFSR digests, sums, parity
- Protocols `p_*.cpp`, `ISM_PROTOCOL(name, strength, fn)`: STRONG (CRC /
  digest >= 8 bits) and MEDIUM (checksum / parity + structure) run on every
  burst, WEAK (repeat-only: EV1527 / PT2262, Nexus, Prologue, TX141-Bv2...)
  only when nothing stronger decoded it. One message per device (model + id)
  and burst. `generic.cpp`: a burst nothing decoded, sliced by its own timing
  (PWM / PPM / NRZ) - a row >= 12 bits sent twice = an "Unknown" device with
  the code as its id (valid frame); else a clean long row = a signal class
  (modulation, coding, bit rate, frequency), shown only when heard again
  within 10 min (one-off fragments / collisions / wake-ups are not devices)
- `decoder.cpp`: device table (Model, ID, Type, Ch, Reading, Battery, MHz,
  SNR, Msgs; key = the talker id), events (new device, button / state /
  alarm, low battery, verbose: every message), raw = the message as one JSON
  line (rtl_433 keys + `mic`) + MHz + SNR, `valid(start, end)`. DoA: one
  `kp::Talker::packet` per message, id "model id" (<= 32), span = the latest
  0.24 s of its rows (the whole burst when one device decoded from it -
  kraken_doa takes <= 0.25 s and keeps 2.5 s at 2.4 MHz), `freq_hz` = the burst's
  carrier / FSK centre, `bw_hz` = occupied + 10 kHz (25-160 kHz), `avg_s` =
  option `doa_avg` (1 = each transmission alone, 60, 600). Options `units`
  (si / us), `unknown` (0 = none from now on, and the ones heard so far are dropped: `table_remove` + `Host::talker_forget` - wire TALKER_FORGET 33 -> `TalkerSpan::forget` -> `TalkerDoa::add_span` erases the talker and its waiting packets), `keep_h` "Keep devices" (1 / 6 / 24 / 168 h,
  default 24): a device not heard for that long is forgotten here (max
  1000, then the least recently heard), its table row hidden
  (`Host::table_keep`, wire TABLE_KEEP 32 -> `Report::table_keep`, per
  plugin table, 60 s .. 7 days, default 10 min) and kraken_doa drops its
  talker (`Talker::keep_s`). Was 1 h here, 10 min in the table and 2 min
  for the talker: devices vanished from the 📡 DF list after 2 min
- UI: `rdfKind` 'device' for `pl` 'ism' (📟, "Device / Reading / Tx", no
  position columns, by name in the DF list - see *DoA per talker*, Order)
- EVERY ANTENNA (2026-10-13, `kp::Info::multichannel`, the user's request:
  "improve sensitivity"): in coherent mode with DoA on, kraken_doa hands
  the plugin all antennas (`process_multi`; wire SAMPLES_MC 8 = uint32 m +
  n x m complex int16 / 8192, sample by sample). `FrontEnd::push_multi`:
  antennas 1.. in 16-bit rings beside antenna 0's (+67 MB), each FFT hop
  every antenna through the one FFTW plan, each brought to the antennas'
  mean noise (`xn_` = its median bin power, ~1 s; `xg_` the factor - the
  five here differ by up to 5.7 dB) -> detection on their mean power; each
  package sums the M x M covariance of its active bins' (normalised)
  spectra (`Pkg::R`); `finish` takes its main eigenvector (power iteration),
  weights w = v sqrt(g) (|v| = 1: the combination's noise = the floor's),
  antenna 0's weight real, and `make_segment` mixes down sum conj(w_k) x_k
  - blind MRC, no calibration. Measured: real 5-antenna recording (20 s):
  antenna 0 8 messages, best antenna 44, combined 88 (+ a Toyota TPMS and a
  distant WH65B only combined); a real WH65B frame planted at set SNRs into
  5 noise channels: antenna 0 decodes down to 6 dB (100 kHz), combined down
  to 0 dB; 5-channel noise: nothing. CPU 38 % vs 14 % of a core (offline).
  Not in independent / wideband mode or with DoA off (one channel, as
  before; checked through the engine with digi_test u8 vs u8mc). Antenna 0
  of the array here hears far less than the others (its noise 5 dB higher):
  the single-channel decoder used the CH selector's antenna
- Detector fixes found on a 30 s live recording at sens 8 (same day): a
  package continues on max(threshold - 2, 7.5) dB (6 dB: noise kept 93 %
  of package regions active - packages ran to 1.2 s, merged transmissions,
  DoA reports came 1.3-1.8 s late); splatter is also compared with what
  starts in the same frame; >= 3 packages born in one frame with >= 3 of
  them >= 5 ms long = a broadband event (not demodulated; clicks - a 251
  kHz spur comb here - don't count). Unknown signals: PWM only when most
  pulse + gap pairs keep the period (NRZ 1T / 2T runs fooled it), classes
  matched within 15 % / 30 kHz and keeping their first name (the name
  jittered every burst), 12 dB; a decode with the same maker + id as a >= 15
  dB stronger overlapping one > 30 kHz away is dropped as a copy. Notes:
  the "LoRa-like" wide bursts at 433.997 MHz are a real wide-FSK (+-65 kHz)
  device whose two tones, decoded apart, looked like two OOK devices with
  inverted codes; 9 "Jansite" TPMS ids at 433.997 MHz live were most
  likely its preamble passing the checksum-less Jansite decoder (not
  reproduced on the recording - left as is)
- Keep tests (scratchpad e2e/: scene_keep.json - the devices stop at 40 s,
  3 antennas; run_keep.sh + t_keep.py, niced next to a live stack):
  default - all 8 still listed at 190 s, 159 s after the last packet (the
  old 2 min dropped them at 120 s); `keep_h` 0.025 (90 s) - talkers and
  table rows leave together, none older than 88 s. t_ui_sort.py (replayed
  capture with partial pushes): the list complete between full pushes, one
  order throughout, by name, compass bearings, heading sorts + reverse +
  localStorage, a row still picks its device
- Built with `-O3` (`plugins/ism/CXXFLAGS`): the front end's per-bin loops
  (`__restrict` pointers) vectorise - GCC's -O2 cost model takes no loop
  that needs an epilogue; the output is identical (checked on all 1125
  recordings against the -O2 build, scratchpad scripts/cmp_bin.py). CPU
  (Pi 5): detector ~4 % of a core (bench/fe_bench; 9.1 % before the
  frequency-order arrays and -O3), the plugin 14-17 % live in a dense scene
  (7 devices, bursts ~40 % of the time; the 19 MB/s pipe included), ~7 %
  with 5 devices; kraken_doa no busier than with a 240 kHz VFO (no
  decimation at 2.4 MHz)
- Multi-device tests (scratchpad e2e/): gen_wide.py scene_w1.json -> a 20 s
  2.4 MHz cu8 (7 devices at -950..+900 kHz, six starting within 150 ms):
  every transmission of the 6 known devices decoded at its frequency, the
  LaCrosse recording as the same unknown code as at 240 kHz; end to end
  (`run_e2e.sh scene_w1.json 70 --plugin ism --bw 0`): every device's
  bearing exact, every transmission in its history
- Tests: rtl_433_tests (1125 recordings at 250 kHz - 1 MHz, rate / RF from
  the file name, padded with 0.4 s of noise - a burst ends after 30 ms of
  quiet), each converted to a 2.4 MHz cf32 as a 2.4 MHz receiver would
  record it (scratchpad scripts/to_wide.py, `ISM_WIDE=1 run_tests.py`):
  resampled, the band outside the recording's filled with noise at its own
  density (per-bin 20th percentile over time / 0.223, median over frequency)
  and its DC left as recorded. 954 of the 1030 with expected output decode
  fully, no wrong fields. Plain upsampling (empty band) gave 853: the
  detector's noise floors start from the median bin - near zero there - and
  rise only 0.01 %/frame; a fill that was too strong or a DC step from
  subtracting the file mean also broke files - all test artefacts. Against
  the 240 kHz version (960 + 3 wrong fields on the same files at 240 kHz):
  5 carriers at the edge (+-120..125 kHz) of 250 kHz recordings (HCS200 x4,
  Rubicson gfile039 - the conversion's edge; the 240 kHz one folded them
  back), 2 with a signal from the first sample (TX35DTH-IT, Steelmate leak:
  the floors start from the whole band's median, a package opened at t=0
  took what came later), 1 marginal (14 dB); it missed WH2303 gfile018 and
  THGR122N-0001. The misses common to both are models not done (other
  LaCrosse, Rubicson pool / meat, ThermoPro TX2 = Prologue bit for bit,
  TX31UIT) and rtl_433's own false positives (GS558 on EV1527 fobs).
  Scratchpad: scripts/run_tests.py, one.py (`ISM_DUMP=1`: every burst's
  pulse trains; `ISM_TRACE=1`: the detector; `ISM_ROWS=<name>`: a
  protocol's rows), dump2.sh (old 240 kHz vs full band, one recording)
- End to end (scratchpad e2e/: fake_heimdall_ism.py streams a 5-element UCA
  scene, 433.92 MHz, r 290 mm, into a test kraken_doa on ports 39080 /
  39091; t_ism.py): synthetic EV1527 / PT2262 fobs + real recordings (EV1527
  PIR, Toyota TPMS, Acurite tower, Schrader TPMS, a 71-bit PPM unknown) at
  5 bearings / frequencies, some transmissions overlapping in time: every
  device 0.0 deg off, every transmission in its history right; the unknown
  device too (`run_e2e.sh scene2.json 70 --plugin ism --bw 0`; first run
  with the 240 kHz version, the same with the full band)

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
  16000, avg_s 20, keep_s 1800}` (kept as long as the vessel list), span = training sequence (24 bits before the opening
  flag) .. closing flag, through the channel's filter delays (`to_time`:
  input index = 2 (t - 10.5) - 24); the spans land within 0.3 bit of the
  truth on synthetic signals
- Generator in the repo since 2026-10-15: `plugins/ais/tools/ais_gen.py`
  (message 1 per ship, channel A / B, times; GMSK BT 0.4, NRZI, HDLC, CRC)
  -> `plugins/sdk/tools/uca_scene.py` for ships at known bearings
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
  valid / audio (8 kHz) + audio_channel (whose talker channel - Voice to
  Text) / voice_state / freq_error / verbose / voice_wanted /
  time / log, map_point / table_* / message / raw (data log) / talker +
  talker_end (DoA per radio, see *DoA per talker*) / station / tune
  (retune request, `Info::tunes` plugins only - see *LTE / 5G NR Cell
  Decoder*) / table_detail (a table row's label / value lines) / rf_hz +
  rf_inverted (the RF at the input's 0 Hz: wire RF 7 "rf_hz,inv" = the
  engine's `rf_hz_` + `afc_hz_` (mirrored with invert), sent by
  `send_settings` when it moves > 10 Hz and after a (re)start; offline
  `--rf HZ` or the sidecar's `rf_hz`); `kp::Option` key/label/default/choices "v=Label|..."/help),
  helpers `plugins/sdk/kraken_dsp.hpp`, the shared library `plugins/lib/`,
  guide `plugins/SDK.md` (written for LLMs too)
- `plugins/Makefile` builds `lib/build/libkrakendig.a`, then links each
  plugin's sources + `sdk/plugin_host.cpp` + the library into its OWN
  executable `plugins/<id>/build/decoder` (linked as decoder.tmp, then
  renamed: a running copy keeps its inode). The top Makefile's `plugins`
  target runs it with -k and only warns on failure; CMake has an ALL target.
  An optional `CXXFLAGS` file in a plugin folder adds compiler flags (last
  on the command line; ism: -O3)
- `decoder --info` (JSON incl. `options` and `options_tsv` - the
  tab-separated copy kraken_doa's flat JSON reader parses; read by
  `PluginRegistry::scan()` at startup, on `PLUGINS_RESCAN` (worker thread,
  `scan_async`) and after builds; `generation()` bumps when the list
  changes), `--serve` (live), `--file REC [--offset HZ] [--opt k=v]
  [--verbose] [--audio out.wav]` (offline test: same resampling / 10 ms
  blocks as live, cf32 / cu8 / cs16 / 16+24-bit IQ WAV, prints events +
  "VALID FRAMES: N"); `--channels M --uca R` on a recording of every
  antenna (sample-major): antenna 0 decoded (multichannel plugins: all),
  then `talker_bearings` - each talker's covariance from its own samples
  of every antenna (spans 40 ms inside, packets exact, each through its
  `freq_hz` / `bw_hz` channel or the plugin's band; > 0.25 s packets and
  same-channel overlaps dropped like TalkerDoa; the resampler's delay
  taken off) -> MUSIC, one source, kraken_doa's UCA (needs --rf)
- Every antenna for `kp::Info::multichannel` plugins: `DigitalDecoder::push(..,
  mc, mc_m)` (the pipeline packs the block's channels as int16 x MC_SCALE
  only while `wants_multichannel()` and the block holds the coherent array:
  not multi-tuner, not fm_only), the Block carries them, `Engine::run`
  sends SAMPLES_MC chunks instead of SAMPLES to such a runner at the VFO's
  rate with no AFC correction (invert = every antenna's Q negated), same
  pos_map. digi_test `u8mc --channels M` feeds an interleaved recording
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
  never switches the panel to the AI tab. The chat column (`#info-ai-chat`)
  holds two lasting children: `.ai-msgs` (the bubbles; the running job's
  "working N s" clock is a `.ai-el` span filled by `aiFillElapsed`, so the
  1 s tick doesn't change the HTML) and `details.ai-acts` (agent activity)
  whose `.ai-log` only APPENDS new lines (rebuilt only when the list's first
  entry changes - the 300-line cap - or the view changes) and keeps its
  scroll position, following the newest line while scrolled to the end;
  its open state is set once per view (live = open, saved = collapsed) and
  then left to the user. (It used to rebuild everything every second, which
  threw a reader of the activity back to the top.) Bottom panel: `#info-panel` in the spectrum
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
- Memory (sigtool + `ai/memguard/`): the Pi has ~1 GB free with the stack
  running, and a Python process that outgrows it swaps the receiver to a
  standstill (two 45 / 50 s captures froze the Pi: capture held the whole
  recording plus float64 / complex128 copies, ~140 MB per second). So:
  - `capture` streams: a reader thread takes the 8091 packets off the
    socket, the main thread converts / DC-removes (smoothed piece means,
    ramped) / mixes (`mix(.., n0)`: blockwise, phase continuous) /
    channelizes (`Channelizer`: the overlap-save filter fed a piece at a
    time, same output as on the whole array) ~0.2 s at a time and appends
    to `<out>.part`, renamed when done (removed on error / SIGTERM). ~70 MB
    for any length (old: 481 MB for 3 s), keeps up with real time on a Pi
    5; `--seconds` <= 600, refused when the file wouldn't fit on the disk
  - analysis: cf32 inputs are `np.memmap`ped (page cache, not process
    memory); `extract` streams like capture; `demod` works in pieces (the
    demodulator, then a Channelizer to <= 2x the audio rate, a small
    resample at the end - one FFT over a 45 s recording alone took ~580 MB);
    `fm_disc`, levels and `bursts` run in blocks; `welch(.., fn=)` applies
    the envelope / x^k / mean removal per segment; `analyze`'s modulation
    statistics use at most `STATS_MAX` (2^22) samples of the signal (the
    report says so). 45 s at 480 kHz: analyze 1290 -> 378 MB, demod ~1 GB ->
    250 MB (both incl. 173 MB of the mapped file), symbols 9+ min -> 11 s (its
    `np.interp` built a recording-long axis per call). The SSB demod was
    also fixed (it took the real part 1.5 kHz off zero: a 1 kHz USB tone
    came out at 500 Hz)
  - ceiling: `memguard/kraken_memguard.py` `apply()` sets RLIMIT_DATA (heap
    + anonymous mappings = every numpy array) to MemAvailable minus
    max(400 MB, 10 % of RAM) (>= 256 MB; a tighter inherited limit is
    kept; `KRAKEN_AI_MEM_LIMIT=0` = none), so too much = MemoryError
    instead of a frozen Pi. sigtool applies it itself (MemoryError -> "work
    on a part / extract to a lower rate"); `kraken_ai.py` starts the agent
    with `memguard/` on PYTHONPATH (`agent_env()`), so its
    `sitecustomize.py` applies it to every Python process the agent runs
    (checked: the claude CLI passes PYTHONPATH to its Bash tool). The
    agent's system prompt says how to work on long recordings
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
