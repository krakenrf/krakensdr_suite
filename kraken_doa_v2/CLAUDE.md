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

**Digital voice/data decoders (one per VFO): engine `src/digital/`, decoders `plugins/`:**
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below);
  kraken_doa contains no protocol code. Shipped: p25, dmr, tetra, dstar,
  nxdn, mpt1327, pocsag, aprs
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
  user left in Auto detect (rebuilt when the registry generation changes);
  detection = majority of `valid()` frames in a 4 s window. Only the "lead"
  plugin (the fixed one, or the detected one) gets VOICE_WANTED and has its
  audio / voice state / carrier offset used. AFC: plugin offset reports are
  averaged and applied at most every 300 ms, ignoring reports from the 300 ms
  after a correction (the pipe latency made an every-report integrator run
  away - TETRA drifted -5 kHz). A VFO retune (> 100 Hz) or mode change resets
  everything (RESET to the plugins; `reset_pending_` is consumed by the
  worker)
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
  `info` {id: facts}, `voice`, `opts`
- Options (`dig::Options`): verbose, invert (host side: conj before the
  mixer) + `plugin` {"<id>.<key>": value} - the plugins' declared options,
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
  rebuilds the cards). Each decoding VFO gets a tab in the bottom panel
  (`infoRenderDecoder`: mode, Listen, the plugin's options - in AUTO those of
  the detected plugin -, verbose, invert, bandwidth / plugin problems, status,
  facts, filterable event log, plugin stderr). The sidebar "🔐 Digital
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
  time / log; `kp::Option` key/label/default/choices "v=Label|..."/help),
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
