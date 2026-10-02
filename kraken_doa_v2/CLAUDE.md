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
