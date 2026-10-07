# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This repository contains **Heimdall v2**, a real-time direction-finding (DoA) system for RTL-SDR devices. The project consists of two main C++ applications that work together:

1. **Heimdall Server** (`heimdall_v2/`): Multi-RTL-SDR coherent receiver with phase compensation
2. **DoA Client** (`kraken_doa_v2/`): FFT viewer and MUSIC DoA processor

Both applications are optimized for ARM platforms (Raspberry Pi 4/5) with NEON SIMD support, but also run on x86_64.

## Repository Structure

```
krakensdr_v2/                        # repository root
├── heimdall_v2/                     # Main server application
│   ├── src/
│   │   ├── core/       # Core types, config, settings, logging, utilities
│   │   ├── sdr/        # RTL-SDR device management and sample pipeline
│   │   ├── dsp/        # FFT, correlation, compensation algorithms
│   │   ├── net/        # TCP servers for data/control/RTL-TCP
│   │   ├── web/        # uWebSockets web interface
│   │   └── main.cpp    # Server entry point
│   ├── external/concurrentqueue/    # Vendored moodycamel queue
│   ├── config.h        # Main configuration file
│   ├── Makefile        # Primary build system
│   └── index.html      # Web UI
│
└── kraken_doa_v2/                   # DoA client application
    ├── src/
    │   ├── signal_processing/  # FFT, FM demod, MUSIC DoA, beamformer, decimator
    │   ├── networking/         # TCP client, data receiver, WebSocket server
    │   ├── utils/              # Ring buffers, IQ conversion, stats
    │   ├── channel_manager.cpp
    │   ├── scanner_manager.cpp
    │   └── main.cpp            # Client entry point
    ├── include/
    │   ├── config.hpp          # Client configuration
    │   └── globals.hpp         # Global state declarations
    ├── Makefile                # Client build system
    └── kraken_doa.html         # Client web UI
```

## Build Commands

### Heimdall Server

```bash
cd heimdall_v2

# Quick start
make              # Build (auto-installs uWebSockets)
make run          # Build and run server
make deps         # Install system dependencies (Ubuntu/Debian)

# Alternative distributions
make deps-fedora  # Fedora/RHEL
make deps-arch    # Arch Linux

# Build management
make clean        # Remove build artifacts
make rebuild      # Clean and rebuild
make distclean    # Remove everything including uWebSockets
make status       # Check dependencies and module status
make debug        # Show build configuration

# CMake alternative (needs ../librtlsdr/build/src/librtlsdr.a - built by
# install.sh or by `make` above; CMake stops with a message if it's missing)
mkdir build && cd build
cmake ..
make -j3          # never more than 3 jobs on the Pi (see kraken_doa_v2/CLAUDE.md)
./heimdall
```

### DoA Client

```bash
cd kraken_doa_v2

# Quick start
make              # Build (auto-installs uWebSockets)
make run          # Build and run client
make deps         # Install system dependencies

# Build management
make clean        # Remove build artifacts
make rebuild      # Clean and rebuild
make distclean    # Remove everything including uWebSockets
make status       # Check dependencies and build status

# Debugging
make debug        # Build with debug symbols
make debug-arm    # ARM build with NEON debug output
```

## Running the Stack (install.sh / run.sh)

- `./install.sh` installs apt dependencies, builds the librtlsdr fork (see
  *Dependencies*) and both apps. It does NOT remove distro RTL-SDR packages -
  heimdall links the fork statically, so gqrx, gr-osmosdr, rtl_433 etc. can
  stay installed on the stock library.
- `./run.sh [--wideband|-w] [--kerberos] [--kerberos_sw|--kerberos-sw] [--ext_noise|--ext-noise]` starts
  both apps in a tmux split (`NO_TMUX=1` = headless, logs in `logs/`);
  `./run.sh stop` stops everything. Unknown arguments are an error (a
  mistyped flag used to be ignored silently); `-h` prints usage. heimdall
  and kraken_doa themselves also exit 1 on an unknown or incomplete option
  (heimdall accepts `--kerberos-sw` as well as `--kerberos_sw`).
- A heimdall startup failure after the dongles open (busy 8091/8092, device
  init, downconverter) releases the hardware and `_Exit(1)`s like the normal
  shutdown. A busy 1234 is NOT fatal: heimdall runs without the optional
  RTL-TCP tap (a stock rtl_tcp on a spare dongle uses that port) - a plain `return` ran global destructors that joined the
  RTL-TCP thread blocked in `accept()` and hung. A web port that can't be
  bound also ends with status 1. The shutdown waits (<= 5 s) for an
  element-count change holding `settings_mutex`, so it can't close handles a
  just-started reader is using.
- Each app runs under a supervisor (`run.sh __supervise`) that restarts it
  after a crash. Ctrl+C, `run.sh stop`, `systemctl stop` and closing the tmux
  pane/window all stop it cleanly: the supervisor runs the app as a
  background child it `wait`s on and forwards the signal, because a pane close
  SIGHUPs only the supervisor (the pane's session leader), never the app.
- Starting AND `stop` sweep stale processes first: any heimdall / kraken_doa /
  run.sh supervisor outside the current session (a dead tmux server, an old
  run, a headless run) gets SIGTERM per process group (clean shutdown,
  dongles released), SIGKILL after 10 s. Processes of other users are only
  reported.
- The convergence wait before the client starts ends at once when heimdall
  runs in wideband scan / independent mode (8091 phase-state bits 0x400 /
  0x800, probe exit 3): no calibration runs there - it used to hold the client
  back for the whole timeout, and headless mode then gave up on the stack
- The convergence wait before the client starts watches heimdall's process:
  headless mode aborts with heimdall's log tail if it dies; in tmux mode the
  client pane gives up if the heimdall pane's supervisor exits (a crash is
  restarted by the supervisor, so the wait continues through it).
- Headless mode has no supervisor: when heimdall ends on its own, run.sh
  exits with heimdall's status (1 = startup failure, 128+N = killed by signal
  N, e.g. 139 for a crash); a stop it was asked for (Ctrl+C / TERM) exits 0.
- Testing a COPY of run.sh still stops the live stack: before starting it
  replaces any tmux session named `$TMUX_SESSION` (default `krakensdr`) and
  sweeps stale heimdall/kraken_doa processes - give the copy its own
  `TMUX_SESSION` and stub `stop_stale`.
- `run.sh` from INSIDE its own tmux session (e.g. a window opened there):
  starting refuses (replacing the session would close that shell mid-start);
  `stop` stops the apps and sweeps stale processes, then closes the session
  (and that window) last. Only panes run.sh started are signalled
- heimdall saves the last USER tuning (frequency, gain) in
  `heimdall_settings.conf` once it has been stable for 10 s and restores it
  before the dongles open, so a crash restart comes back where the array was
  (scanner hops are not saved)
- Boot service (`install-pi-service.sh`): `Type=forking`, `ExecStop=run.sh
  stop`, `LimitRTPRIO=30` (realtime USB threads), `Restart=on-failure`.

## Docker (DOCKER.md)

- `Dockerfile` (one image: both apps, librtlsdr fork + uWebSockets cloned at
  pinned commits, runtime packages derived from `ldd`) + `docker-compose.yml`
  (two containers, `heimdall` and `kraken_doa`, `network_mode: host`,
  `restart: unless-stopped` = autostart at boot) + `docker/entrypoint.sh`
- USB passthrough: `/dev/bus/usb` bind-mounted + `device_cgroup_rules: c
  189:* rmw` (follows re-enumeration after heimdall's USB reset / a replug,
  unlike `devices:`); `ulimits: rtprio: 30` for the SCHED_RR threads
- Each app runs with cwd `/data/<app>` (= `docker-data/<app>` on the host),
  web files symlinked in - every runtime file path in both apps is
  cwd-relative, so a new persisted file lands in the volume automatically; a
  new STATIC file the apps serve must be added to the Dockerfile COPY, the
  entrypoint's `link_files` and the `.dockerignore` allow-list
- `.dockerignore` is an allow-list: secrets (`doa_settings.json`, `api_token`,
  `server.key`) can't reach the image. `docker-data/` and `.env` are gitignored
- heimdall's entrypoint waits for `KRAKEN_TUNERS` dongles (boot enumeration);
  kraken_doa's waits for CONVERGED from the 8091 header (not in KerberosSDR
  manual mode, nor in wideband / independent mode - header bits 0x400 /
  0x800 - where no calibration runs); empty compose variables are unset (apps treat "" as set)
- `--wideband` / `--kerberos_sw` need `privileged: true` (hidraw, gpiochip,
  `/proc/device-tree` is masked otherwise) via `compose.override.yaml`

## Architecture

### Heimdall Server (heimdall_v2/)

**Purpose**: Coherent multi-channel RTL-SDR receiver with real-time phase/lag compensation

**Module Structure**:
```
main.cpp (orchestration)
├── web/        → core/, dsp/, sdr/, net/
├── net/        → core/, sdr/
├── dsp/        → core/, sdr/
├── sdr/        → core/
└── core/       (no dependencies)
```

**Key Components**:
- **Core Module**: Types, configuration, logging, utilities
- **SDR Module**: RTL-SDR device management, L1/L2 buffers, sample pipeline
- **DSP Module**: FFT (FFTW3), cross-correlation, eigenvalue-based phase calibration (Eigen3)
- **Net Module**: TCP data server (port 8091), control server (8092), RTL-TCP (1234)
- **Web Module**: uWebSockets HTTP/WebSocket server (port 8070)

**Data Flow**:
1. RTL-SDR callbacks → L1 buffers (per-device, alloc-free; signal coherence loss instead of silent per-device drops)
2. Sample drain → L2-raw staging (cheap aligned collection, realtime priority)
3. Conversion worker → convert/compensate + TCP broadcast → L2 buffer (synchronized, phase-compensated)
4. Correlation processor → FFT, cross-correlation, compensation
5. TCP servers → Stream to DoA client and external tools; WebSocket → web UI
6. Coherence watchdog → full flush + recalibration on a detected desync

**Threading Model**:
- Main thread: uWebSockets event loop
- Per-device RTL-SDR threads (realtime)
- Sample drain thread (realtime) and conversion worker thread (normal priority)
- Correlation processor thread
- Per-channel compensation threads
- Coherence watchdog thread (runs recovery after a detected coherence loss)
- TCP server threads (data, control, RTL-TCP)
- Status broadcaster thread

See `heimdall_v2/CLAUDE.md` → *Coherence-Loss Detection and Recovery* and *Pipeline Decoupling* for details.

### DoA Client (kraken_doa_v2/)

**Purpose**: FFT visualization, FM demodulation, MUSIC DoA processing

**Key Components**:
- **Signal Processing**: FFT processor, FM demodulator, MUSIC DoA algorithm
- **Networking**: TCP client (connects to Heimdall server), WebSocket server (browser UI)
- **Utils**: Ring buffers, optimized IQ conversion (ARM NEON), system stats
- **Managers**: Channel manager, decimator manager

**Data Flow**:
1. TCP client → Receive IQ data from Heimdall server (port 8091)
2. IQ converter → Convert uint8 to complex<float> (NEON-optimized)
3. Raw data buffer → Store with automatic cleanup
4. Decimator → Bandwidth reduction with integer decimation
5. FFT processor → Spectrum visualization
6. FM demodulator → Audio output
7. MUSIC processor → Direction of arrival estimation
8. WebSocket → Broadcast to browser (port 8080)

**Optimizations** (see OPTIMIZATION_SUMMARY.md):
- Compiler auto-vectorization for IQ conversion (faster than manual NEON)
- Thread-local decimators with shared coefficient cache
- FFTW3 wisdom files for optimal FFT plans
- Moodycamel concurrent queues for lock-free communication
- ARM NEON support with automatic fallback to scalar code

## Configuration

### Heimdall Server Configuration

Edit `heimdall_v2/config.h`:

**RTL-SDR Settings**:
- `NUM_DEVICES`: compile-time CEILING on device count (default 8; sizes static
  per-channel state). The count actually used is runtime — see *Runtime
  element count* below
- `REF_CHANNEL`: Reference channel index (default 0)
- `CENTER_FREQ`: RF center frequency in Hz (default 100 MHz)
- `SAMPLE_RATE`: Sample rate in Hz (default 2.4 MSPS)
- `GAIN`: Gain in tenths of dB (default 496 = 49.6 dB)
- `NUM_SAMPLES`: Samples per FFT, must be power of 2 (default 16384)

**Ports**:
- `WEB_PORT`: Web interface (default 8070)
- `RTL_TCP_PORT`: RTL-TCP server (default 1234)
- `TCP_DATA_PORT`: Multi-channel data streaming (default 8091)
- `TCP_CONTROL_PORT`: JSON control interface (default 8092)

**Hardware**:
- `ENABLE_BIAS_TEE`: Enable bias-tee on all devices (default 1)
- `USB_RESET_ON_INIT`: Reset USB on initialization (default 1)

**Device Mapping**:
- Serial number-based device enumeration ensures consistent channel ordering
- Default serial list "1000".."1007" (KrakenSDR convention extended to the
  8-channel ceiling; defined in `main.cpp`, declared in `src/core/config.hpp`
  as `expected_serials`); override at runtime with `--serials s0,s1,...`

**Runtime element count (N-channel support)**:
- Startup N resolves as: `-n` flag > `num_elements` in `heimdall_settings.conf`
  (written by the web UI selector) > count of expected serials actually
  attached (`count_expected_devices_present()` — a stock KrakenSDR comes up
  with 5, a KerberosSDR with 4)
- Exception: `--kerberos` pins the default to 4 (KerberosSDR is 4-channel
  hardware; persisted value and USB auto-detection are ignored so simulating
  on a 5-dongle KrakenSDR still runs 4). Only an explicit `-n` overrides
- Only a runtime choice (web UI / `NUM_ELEMENTS:` / `set_num_elements`) is
  saved; a count from `-n` or `--kerberos` lasts for that run only
  (`settings::persisted_num_elements` is what `save()` writes)
- Per-port antenna bias tees survive count changes: the saved mask keeps the
  bits of channels that aren't open and re-applies them when they reopen
- Changeable at runtime via the web UI "Array Elements" card, WS command
  `NUM_ELEMENTS:<n>`, or control-port `{"command":"set_num_elements","num_elements":N}`:
  the pipeline threads stop, ALL device handles close and the first N reopen
  (`src/sdr/pipeline_control.cpp`), then a full recalibration runs. Rolls back
  to the previous count if the open fails (e.g. selecting more elements than
  dongles attached); refused during recovery/scanning
- Devices beyond N are never opened, so other programs can claim them over USB
  (e.g. run with `-n 4` and use the 5th dongle in SDR#)
- The 8091 packet header carries the live channel count; the DoA client and
  the GNU Radio source adapt from the wire

### DoA Client Configuration

Edit `kraken_doa_v2/include/config.hpp`:

**Network**:
- `WEB_PORT`: Client web UI (HTTPS/WSS, default 8080)
- `DOA_HTTP_PORT`: Plain HTTP DoA value page for Android app (default 8081)
- `TCP_DATA_PORT`: Heimdall server data port (default 8091)
- `TCP_CONTROL_PORT`: Heimdall server control port (default 8092)

**Signal Processing**:
- `FFT_SIZE`: FFT size (default 16384)
- `MAX_CHANNELS`: Compile-time channel ceiling (8, matches server); live count
  follows the 8091 packet header (`active_num_elements`)
- `SAMPLE_RATE`: Expected sample rate (default 2.4e6)
- `AUDIO_SAMPLE_RATE`: Browser audio rate (default 48000)

**MUSIC DoA**:
- `DOA_NUM_ELEMENTS`: Compile-time ceiling on antenna elements (8). MUSIC,
  the beamformer and the UI adapt at runtime to the streamed channel count,
  reinitializing per-element state on a mid-stream change. Settings that
  depend on the count are stored as requested and limited per frame (signal
  sources: min(requested, elements-1)) or re-derived on a count change (3D
  custom array), since the startup replay runs before the real count is known
- `DOA_BLOCK_SIZE`: Samples per processing block (default 256)
- `DOA_ANGULAR_RESOLUTION`: Degrees per step (default 1)
- **CUSTOM topology**: positions beyond the count given in CUSTOM_POSITIONS
  (e.g. after the element count grows) get the UI table's default - a 50 mm
  UCA over the live count - instead of the origin
- **Patch / 3D topology** (`TOPOLOGY:PATCH3D`, runs as CUSTOM): common
  upright patch-panel and 3D layouts picked from a list (same geometry as the
  array calculator); layout persisted as `ARRAY_LAYOUT:`. Custom arrays have a
  front/back **Direction** (`CUSTOM_MODE:`) - Forward only for patch panels
- **UCA element ordering**: the array is expected to be wired **CLOCKWISE**
  (ANT0 on +x, ANT1 clockwise from it). `uca_angle_sign()` in
  `kraken_doa_v2/include/globals.hpp` returns -1 and is the single choke
  point - see the client CLAUDE.md for the full list of sites it feeds

**Bandwidth Options**:
- `BANDWIDTH_OPTIONS`: Integer decimation factors with no resampling
- Supports 2.4 MHz down to 1 kHz bandwidth (the narrow end - 10 kHz to 1 kHz -
  is for weak CW beacons, wildlife tags, fox hunts). MUSIC accumulates across
  blocks (a 1 kHz block is ~7 samples) and caps a frame at ~1 s of samples
  (min 4 snapshots), so narrow bandwidths still give ~1 bearing/s; the
  beamformed FFT collects short blocks until it has 64 samples. The web UI
  draws a VFO at least 8 px wide so a 1 kHz bar stays visible/grabbable

**Scanners**:
- The discrete (server-side hop) and continuous scanners are mutually
  exclusive: starting one while the other runs is refused and the browser's
  button reset
- Discrete scanner: config entries outside the tunable range (R820T, or the
  Wideband variant's span) are refused - the whole load, the previous config
  stays; frequencies go to heimdall as uint64 Hz. heimdall's settle and dwell
  waits end on stop / reconfigure / start (`run_generation`), so a 1 h dwell
  no longer blocks them, and a cut-short dwell doesn't advance the group
- Continuous wideband scan: a range that isn't tunable (or needs > 4096 bands)
  empties the band plan and start() refuses with an error shown in the panel;
  on a manual-calibration KerberosSDR it doesn't wait for automatic
  calibrations (none run) - only a manual one (noise on) pauses it
- Locking onto a signal turns DoA back on (`apply_wideband_mode_state`, the
  single place wideband-scan mode parks/restores DoA; idempotent)

**Antenna array calculator**: `kraken_doa_v2/array_calculator.html`
(served at `/array_calculator.html`, opened from the MUSIC DoA box) sizes
UCA / ULA / flat patch / 3D arrays for a frequency (port of the krakensdr_docs Excel
sheet), shows the usable frequency range of an existing array, draws
top/side/3D views with coordinates, and can push the result into the
receiver. See `kraken_doa_v2/CLAUDE.md`

**Digital voice/data decoders (per VFO)**: engine `kraken_doa_v2/src/digital/`,
decoders `kraken_doa_v2/plugins/`
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below):
  p25, dmr, tetra (downlink), dstar, nxdn (48 and 96), mpt1327 (analogue
  trunking signalling), pocsag, aprs and adsb (1090 MHz; manual only -
  picking it tunes the VFO + tuner to 1090 MHz at 2.4 MHz and draws the VFO
  as a locked line, `kp::Info::fixed_freq_hz`). Each decimator (VFO) runs one
  plugin, or AUTO (every plugin the user left ticked for "Auto detect" in the
  sidebar's plugin list - all by default, `PLUGIN_AUTO:id:0|1`, persisted as
  `AUTO_DETECT_OFF:` - at once, minus `manual_only` plugins and those
  needing a VFO > 2x wider; the
  one whose frames pass FEC/CRC wins). Any
  number of VFOs decode at once: a "Digital decoder" tick + mode list on each
  VFO card (Decimators box), one tab per decoder mode (plugin - all of them, always, also unused ones) in the
  panel under the waterfall - every VFO running that mode in it: a settings
  row each, merged tables / incidents with VFO + MHz columns, one merged
  event log (a VFO switched to another mode or removed leaves its lines in
  the old mode's tab); a VFO in Auto detect shows up in the tab of the
  decoder it detected -
  waterfall (its settings + data); the sidebar "🔐 Digital Decoders" box shows
  which voice codecs are installed (+ install steps for missing ones, from
  `plugins/lib/build/codecs` - the plugins' own detection) and the plugin list
  (Auto detect ticks, ↻ re-scan); WS
  `DIGITAL_MODE:id:OFF|AUTO|PLUGIN:<id>` (old names P25/DMR/... still map),
  `DIGITAL_OPT:id:verbose|invert|<plugin>.<key>:value` (plugin-declared
  options, e.g. `dmr.slot`, `p25.nac`); persisted in the VFO snapshot. See
  `kraken_doa_v2/CLAUDE.md` for the design
- Voice: Demod "Digital" (or the panel's Listen button) on the Audio Src VFO
  plays the decoded voice. P25 IMBE is built in (port of mbelib, ISC); DMR /
  D-STAR / NXDN AMBE use a user-installed mbelib (dlopen, `KRAKEN_MBELIB`), TETRA
  ACELP the user-built ETSI codec programs (`tetra-cdecoder` /
  `tetra-sdecoder` on PATH or `KRAKEN_TETRA_CODEC_DIR`) - neither is shipped
  (licensing); README "Digital voice codecs" has the install steps. Docker:
  files in `docker-data/codecs/` are picked up by the entrypoint. Encrypted
  calls are muted
- Fed from the VFO's decimated stream (beamformer output when beamforming
  runs), so DoA, beamforming and decoding can run on the same signal

**Web Mapper output (built-in)**:
- The DoA client streams legacy "doapost" records to the KrakenSDR web mapper
  directly (`kraken_doa_v2/src/networking/web_mapper.cpp`) — the old Node.js
  `web_mapper_middleware/` is DEPRECATED and no longer needs installing or
  running (run.sh / install.sh never referenced it; Node is not required)
- Configure from the client web UI sidebar ("🌐 Web Mapper" panel): enable
  toggle, mode (KrakenPro Cloud via WSS to map.krakenrf.com:2096, or Local
  Network WS broadcast on port 8021), KrakenPro API key, server URL, local
  port. All persisted in doa_settings.json. Each VFO's own squelch setting
  is respected: squelch off = always transmit, squelch on = only while open
- Callsign + location come from the existing Station Information panel; cloud-
  pushed settings (remote retune from the map) are applied through the normal
  control-command path, except the server URL, which only the local web UI
  can change. Only values that differ from what the receiver itself reports
  are applied - the cloud echoes whole settings objects, and re-applying our
  own legacy-schema values was lossy (auto sources off, WIDEBAND topology to
  UCA, GPS fix over the static location, squelch forced on). The cloud server's TLS certificate is verified
  (`KRAKEN_WEB_MAPPER_INSECURE=1` disables that for a self-signed self-hosted
  server). See `kraken_doa_v2/CLAUDE.md` for details

**Decoder plugins + AI Signal Lab** (details: `kraken_doa_v2/CLAUDE.md`):
- A plugin = a source folder `kraken_doa_v2/plugins/<id>/` (decoder.cpp +
  extras, API `plugins/sdk/kraken_plugin.hpp`, guide `plugins/SDK.md`), linked
  with `plugins/sdk/plugin_host.cpp` into its OWN executable
  `plugins/<id>/build/decoder` (kraken_doa_v2's `make` builds them; a plugin
  that fails to compile only warns). The Digital Decoder runs it as a child
  process per VFO (`Mode::PLUGIN`, wire form `PLUGIN:<id>`): complex baseband
  over stdin, facts/events/valid/audio/map points back over stdout (binary
  protocol in kraken_plugin.hpp; kraken_doa sends the station location). A
  crash is reported and restarted with a back-off; a
  rebuild (atomic rename) restarts running decoders. Shipped: p25, dmr,
  tetra, dstar, nxdn, mpt1327, pocsag, aprs (written by the AI
  Signal Lab), adsb
- Same executable tests offline: `decoder --file x.cf32 [--offset HZ]`
- The shipped protocol plugins are thin wrappers around `plugins/lib/`
  (libkrakendig.a: FEC, 4FSK sync, vocoders, front ends) - kraken_doa itself
  contains no protocol code
- AI Signal Lab (sidebar box, `src/ai_manager.cpp`): runs `ai/kraken_ai.py`
  (Claude Code `claude -p`, restricted tools, or any LLM CLI) to investigate a
  VFO's signal (`ai/sigtool.py` captures from heimdall 8091 + measures it) and
  to write/build/test plugins. Every investigation is kept (ai/sessions/<id>/
  incl. chat.json) - "AI" badges in the spectrum + a History list open it in
  the "🤖 AI" tab under the waterfall (chat, agent steps, follow-ups, delete).
  The AI's output (answers, live progress, agent steps) is shown ONLY there;
  the sidebar box has the controls + a one-line job status
  OFF unless enabled on the Pi with
  `python3 ai/kraken_ai.py setup` (`ai/ai_config.json`, gitignored; the web UI
  can't enable it - it runs an agent and native code). Native install only
- Plugins move between receivers as folders only: copy `plugins/<id>/`, run
  `make`, press ↻ (no web-UI export / import / rebuild - removed on purpose:
  compiling code sent from a web page was a recipe for issues)

**🗺 Map (right-hand pane)** (details: `kraken_doa_v2/CLAUDE.md` *Map*):
- Street (OpenStreetMap) / satellite (Esri World Imagery) map drawn by the
  page itself (no map library, no CDN): without internet it keeps working on
  a lat/lon grid with a notice. Opened with the 🗺 Map button under the
  waterfall; a tab next to MUSIC DoA in coherent mode, the whole right pane
  otherwise; resizable; view settings per browser (localStorage)
- Shows the positions decoder plugins report (`kp::Host::map_point`: ADS-B
  aircraft, APRS stations, DMR GPS, D-STAR GPS / DPRS) per VFO decoder whose
  "🗺 Plot on map" is ticked in its decoder tab (off by default,
  `DIGITAL_OPT:id:map:0|1`, saved with the VFO), plus the station and range
  rings. kraken_doa pushes `{"map":...}` once a second (changed points + the
  keys of all live ones); `GET_MAP` asks for everything
- Incident map: text messages from plugins (`kp::Host::message`, POCSAG
  pages) -> street address found in the text -> looked up ONLINE in
  OpenStreetMap Nominatim (no downloaded map data) within `GEO_RADIUS_KM`
  of the station (POCSAG tab, default 300, persisted) -> incidents (map
  markers + the POCSAG tab's table; the same address on several VFOs = one
  incident listing them; kept 24 h, incidents.tsv). Code:
  `kraken_doa_v2/src/incidents.cpp`, `geo_address.cpp`, `geo_http.cpp`

**🗂 Decoder Logging (sidebar, all modes)** (details: `kraken_doa_v2/CLAUDE.md`
*Decoder data log*): what every VFO's decoder reports - events, text
messages, positions (throttled per object), raw frames (`kp::Host::raw`,
only while ticked: OPTION `log_raw`), incidents - to
`<dir>/decoders-YYYY-MM-DD.jsonl` (local date). Buffered in memory, written
every 5 s (SD card); at local midnight the day is closed + gzipped in the
background; days older than the Keep setting (default 7, 0 = forever) are
deleted - ONLY files named `decoders-YYYY-MM-DD.jsonl[.gz]`. Writing pauses
below 100 MB free. Folder default `decoder_logs` (cwd-relative, so the
Docker volume gets it); the box lists drives (/, /media, /mnt, /run/media,
/srv + the folder's own) with free space. `DECODER_LOG*:` commands,
persisted. Code: `kraken_doa_v2/src/decoder_log.cpp`
- Position records carry the map popup's details as `info` (ADS-B: squawk,
  vertical rate...); unusual things are events starting with "⚠" (ADS-B:
  emergencies + their end, squawk changes, special squawks, UAV / balloon
  / high-performance types, >= 6000 ft/min, >= 400 kt below 10000 ft)

**📡 Mobile DF on the map (coherent mode)** (details: `kraken_doa_v2/CLAUDE.md`
*Mobile direction finding*): each VFO's live DoA lobe (north frame, from
the station heading) + bearing on the 🗺 Map, and while driving a heat map
of the transmitter's position: `kraken_doa_v2/src/rdf_engine.cpp` (the
algorithm - an improved version of the KrakenSDR Android app's grid; offline
comparison `tools/rdf_sim.cpp`) + `src/rdf_mapper.cpp` (GPS track, MUSIC
frames time-aligned to it, distance gate, per-VFO solver, rdf_session.bin).
`RDF:0|1`, `RDF_RANGE_KM:1-50` (persisted), `RDF_RESET:<vfo|-1>`, `GET_RDF`;
pushes `{"rdf":..}` 2 Hz + `{"rdf_grid":..}` per changed VFO. Right pane
tab "⊞ Both" shows the MUSIC DoA plots and the map at once

### Operating Modes (top-bar Mode selector: Coherent / Wideband / Independent)

- **Coherent**: the array (DoA, beamforming, calibration). **Wideband**: the
  tuner-spread scan (one stitched spectrum, discrete scanner). **Independent**:
  every tuner its own receiver - own frequency + gain, one spectrum +
  waterfall pane per tuner, VFOs on any tuner, no calibration
- Client: `OPERATING_MODE:coherent|wideband|independent` (persisted; the old
  `WIDEBAND_MODE:` maps to it), `TUNER_FREQ:ch:mhz` / `TUNER_GAIN:ch:db` (+
  composite `TUNERS:` persisted), `SET_DECIMATOR_FREQ:id:khz:tuner` (VFO onto a
  tuner, `DecimatorInstance::tuner_channel`, saved in the DECIMATORS snapshot).
  Outside coherent DoA/beamforming are parked (`apply_operating_mode_state`),
  each VFO decimates (and squelches on) its own tuner, the FFT runs on every
  tuner. Independent mode streams FFT message type 6 (every tuner's spectrum)
- heimdall: `set_operating_mode`, `set_independent_tuner`; identity
  compensation in independent mode; mode + per-tuner tuning persisted in
  heimdall_settings.conf and restored at startup; 8091 phase-state flags
  0x400 / 0x800 - the client follows heimdall's mode from them
  (`ControlHandler::note_server_mode`, after a 3 s grace for its own changes)
- The DoA panel and DoA/coherent sidebar sections (MUSIC DoA, Beamforming,
  Web Mapper, Local Recording, continuous scanner, CH selector) are hidden
  outside coherent; the discrete scanner section only shows in wideband
- AI Signal Lab captures (`sigtool.py capture`) record the tuner whose band
  holds the signal outside coherent mode
- Only coherent exists on the downconverter variant. See heimdall_v2/CLAUDE.md
  *Operating Modes* and kraken_doa_v2/CLAUDE.md *Operating modes*

### KrakenSDR Wideband (Downconverter) Variant

Runtime mode for the KrakenSDR Wideband hardware variant, which mixes the RF
down to a fixed, filtered IF in front of every tuner using ONE shared LO (an
Othernet moRFeus box in signal-generator mode, USB HID `10c4:eac9`). Enable
with `--wideband` (`-w`) on BOTH apps, or `./run.sh --wideband` for the stack.

- Tuners are parked at the IF (`WB_VARIANT_IF_HZ`, 1268 MHz) for good; a
  "retune" reprograms the LO only (`heimdall_v2/src/sdr/downconverter.cpp`)
- The user-facing frequency stays the true RF everywhere: `set_frequency`
  takes RF, packet metadata/status report RF, MUSIC wavelength uses RF
- Mixing side FOLLOWS THE FREQUENCY automatically; the mixer side places the
  LO: high side `LO = IF + RF` (RF 24-4132 MHz, spectrum inversion corrected
  at the source by conjugating in heimdall's conversion loop), low side
  `LO = IF - RF` (RF 24-1183 MHz, upright) or below `LO = RF - IF` (LO under
  the RF, RF 1353-6668 MHz, upright - the only side past 4.1 GHz). Every
  retune runs `wideband_retune_rf()` (heimdall sdr_init.cpp): keeps the
  current side if it can reach the RF, else auto-selects (high up to its
  ceiling, below above it). Manual override where several sides reach the
  RF (image dodging): client `MIXER_SIDE:high|low|below` -> heimdall
  `set_mixer_side`, which REJECTS a side that can't reach the current RF
  (the UI greys those buttons out). Side changes trigger the normal retune
  cooldown + phase recalibration
- Antenna ring also FOLLOWS THE FREQUENCY (no user control): outer < 1 GHz,
  center 1-2.5 GHz, inner > 2.5 GHz (`WB_RING_CENTER_MIN_HZ` /
  `WB_RING_INNER_MIN_HZ`, both config files). `wideband_retune_rf()` throws
  the RF switches on a boundary crossing; the client mirrors the rule for
  its ring indicator (ring buttons removed, `ARRAY:` no longer persisted;
  heimdall's `set_array` remains for manual testing but the next retune
  overrides it)
- Client DoA topology "WIDEBAND" (client-only, `TOPOLOGY:WIDEBAND`): UCA
  math with the array radius auto-set from the active ring - outer 127.5 mm,
  center 51 mm, inner 20.4 mm (`WB_RING_RADIUS_MM`, client config.hpp).
  RADIUS: commands are ignored while it is active. The ring follows the RF of
  the data stream (`ControlHandler::follow_wideband_ring`, from the FREQ
  handler AND the receiver), so retunes made by heimdall's UI or the
  continuous scanner move the radius too
- The noise source needs RF path switching on this hardware: GPIO0 only
  powers it; GPIO1-6 of the channel-0 chip drive the on-board RF switches
  that route noise vs. the selected antenna ring into the mixers
  (`wideband_set_noise_path()` in `heimdall_v2/src/sdr/sdr_init.cpp`, called
  from the noise on/off choke point and from `wideband_retune_rf()` on ring
  boundary crossings). The three rings (0=outer, 1=center, 2=inner) follow
  the frequency automatically; a ring change rides the normal retune
  cooldown + phase recalibration. heimdall's `set_array` JSON command
  remains for manual testing only
- LO output drive current (0-7, default `WB_LO_CURRENT` 3): client "Cur"
  selector next to Mix -> WS `LO_CURRENT:N` -> heimdall `set_lo_current`;
  persisted; no recalibration (shared LO = common-mode change)
- Constants live in `heimdall_v2/config.h` and `kraken_doa_v2/include/config.hpp`
  (`WB_VARIANT_IF_HZ`, `WB_LO_MIN_HZ`/`WB_LO_MAX_HZ`) and must match
- Antenna rings are wired CLOCKWISE here as on the standard arrays; the
  client's single chirality choke point (`uca_angle_sign()` in
  `kraken_doa_v2/include/globals.hpp`) is therefore variant-independent -
  see *UCA element ordering* under the DoA client configuration
- NOT the same thing as `OperatingMode::WIDEBAND_SCAN` / `wideband_config`
  (the tuner-spread spectrum scan) - that mode is refused while the
  downconverter variant is active, since all tuners must sit at the IF
- Frequencies are `uint64_t` end-to-end (headroom for LO/RF math; nothing
  currently exceeds uint32 but the plumbing doesn't assume it)

### KerberosSDR Support (--kerberos / --kerberos_sw)

The older 4-channel KerberosSDR has **no noise-source RF switch** - the noise
is coupled in through a directional coupler, so the antennas must be MANUALLY
disconnected for a calibration to be valid. Start heimdall with `--kerberos`
(the DoA client auto-detects the mode from the data stream; no client flag).

- **No automatic noise-cal ever runs**: startup parks in an UNCALIBRATED idle
  state (noise off, FFT off - which idles the lag/phase machines - identity
  compensation; data still streams), the periodic calibration monitor never
  checks, a frequency/gain change marks the calibration **STALE** (the old
  compensation stays applied - approximately valid nearby - with a warning)
  instead of recalibrating, and a coherence loss or element-count change
  flushes and drops back to UNCALIBRATED
  (`kerberos_enter_uncalibrated()` / `kerberos_manual_cal_only()` in
  `heimdall_v2/src/dsp/compensation.cpp` + `core/config.hpp`)
- Leaving the tuner-spread wideband scan never runs the automatic recal here:
  a manual calibration from before the scan is put back (STALE if the
  frequency moved meanwhile), otherwise the state stays UNCALIBRATED
- **Manual calibration**: the heimdall web UI's "Full Recalibration"
  button (confirm dialog: disconnect all antennas first) is the ONLY trigger
  that runs the noise-source calibration (the noise-source checkbox is locked
  and `BIAS_TEE_ENABLE` refused in this mode - noise + FFT on would calibrate
  against the connected antennas) - it flows through
  `recover_coherence(manual=true)` on the watchdog thread
- **State surfaces**: 8092 status JSON gains `kerberos_mode`, `kerberos_sw`,
  `calibration_state` (`uncalibrated`/`calibrating`/`calibrated`/`stale`);
  the web STATE message carries the same; the 8091 packet header piggybacks
  flags on the phase-state field's HIGH BITS (bit 8 = kerberos, bit 9 =
  stale; consumers mask the low byte - done in the DoA client and the GR
  source). Both web UIs show a color-coded warning banner with the
  disconnect-antennas workflow
- **Element count defaults to 4** in kerberos mode (the hardware is
  4-channel); only an explicit `-n` overrides it
- **`--kerberos_sw`** (KerberosSDR modified with third-party CKOVAL antenna
  switches): implies `--kerberos` but restores FULLY AUTOMATIC calibration.
  The switches are driven from Raspberry Pi header GPIOs 23/24
  (`KERBEROS_SW_GPIO_ANT1/ANT2` in `heimdall_v2/config.h`; BCM numbering,
  same pins as the V1 DAQ firmware): idle = 23 high / 24 low (antenna input
  1), both LOW while the noise source is on (antennas disconnected), previous
  selection restored after. Implemented with the linux/gpio.h **v2
  character-device ioctls** on the `pinctrl-*` gpiochip (pigpio does not work
  on the Pi 5 / RP1) in `heimdall_v2/src/sdr/kerberos_gpio.cpp`, hooked into
  the `set_bias_tee_all_devices()` choke point. If the GPIOs are unavailable
  (not a Raspberry Pi per /proc/device-tree/model, chip inaccessible, lines
  claimed) heimdall falls back to plain `--kerberos` (manual calibration)
  with a warning, since auto-cal without switching would calibrate against
  live antennas. The client is NOT flagged in `_sw` mode (header bit 8 stays
  clear) - it behaves exactly as with a KrakenSDR
- `./run.sh --kerberos` / `./run.sh --kerberos_sw` (or `KERBEROS=1` /
  `KERBEROS_SW=1` env) pass the flag to heimdall; flags are combinable with
  `--wideband`. In manual `--kerberos` mode run.sh skips the convergence wait
  and starts the client immediately (heimdall never converges on its own
  there); `--kerberos_sw` keeps the normal wait. run.sh's convergence probe
  masks the phase-state low byte (high bits = kerberos flags)

### External Noise Source Array (--ext_noise)

A standard KrakenSDR plus an add-on antenna array with its OWN noise source,
powered from the KrakenSDR's CH0 antenna bias tee. Start heimdall with
`--ext_noise` (alias `--ext-noise`), or `./run.sh --ext_noise` / `EXT_NOISE=1`
(boot service: `VARIANT_FLAGS=--ext_noise`). The DoA client needs no flag.

- The only behavioural change: every noise-source switch drives the CH0 bias
  tee (GPIO1 of the channel-0 chip, the same GPIO as the CH0 per-port antenna
  bias tee) instead of the KrakenSDR's internal noise source (GPIO0), which is
  held OFF - forced off at device open too, since the librtlsdr fork keeps GPIO
  state across close/open. One choke point: `set_bias_tee_all_devices()` +
  the startup open in `open_active_devices()` (heimdall `sdr_init.cpp`), so
  calibration, recovery, retune recal, the periodic check and shutdown all
  follow automatically; `bias_tee_enabled` keeps meaning "noise source on"
- CH0's bias tee is reserved: `ANT_BIAS_MASK` bit 0 is refused (and cleared),
  the heimdall UI greys out the "Ch 0 (noise)" box; channels 1+ work as usual
- Reported as `external_noise` in the 8092 status JSON and the web STATE
- Refused together with `--wideband` (GPIO1-6 drive that board's RF switches)
  and `--kerberos`/`--kerberos_sw` (different noise path) - heimdall and
  run.sh both reject the combination

## Key Algorithms

### Phase and Lag Compensation (Heimdall Server)

**Lag Compensation (closed-loop proportional servo)**:
1. MEASURING: entry/reset state — zeroes the correction register and engages the servo
2. SERVOING: register-only correction (`rtlsdr_set_sample_freq_correction_f`,
   sample clock only — tuner LO untouched, correlation peak stays usable),
   counts proportional to the measured lag, saturating at ~100 ppm and
   tapering exponentially into the freeze/lock endgame
3. CONVERGED: lag locked within ±0.02 samples (median)

**Phase Compensation** (eigenvalue-based, PHASE-ONLY):
1. WAITING_FOR_LAG_COMPLETION: Wait for all channels to converge lag
2. MEASURING_INITIAL_PHASE: Collect stable phase measurements
3. APPLYING_COMPENSATION: Average 3 snapshots' phases into a unit-phasor correction
4. VERIFYING_CONVERGENCE: Check phase stability (±1°)
5. CONVERGED: Phase drift within ±1°

The dominant eigenvector of the noise-source data gives each channel's phase
(and gain) relative to the reference; the correction is a unit phasor per
channel, applied per sample in the conversion hot loop and reported in the
control-port status JSON as `channel_comp` (`amp_db` is always 0).

**Gain is measured but NOT corrected**: introduced (2026-09-29) while the IF
VGA was under the RTL2832 AGC, when each dongle's gain hunted by up to ~2.5 dB
between snapshots and a gain correction averaged from 3 snapshots locked in up
to ~1.6 dB of error. With the VGA now fixed the gains are stable to ~0.1 dB,
but the noise source clips at that VGA step (which biases a gain estimate).
Re-tested 2026-09-30 with the VGA fixed: the noise-source gain estimate is not
trustworthy - it shifts by 0.7-1.0 dB between VGA step 8 (ADC clipping) and
step 0, and disagrees with the receiver noise-floor ratio by up to 2.4 dB (the
noise source compresses the tuner front end, so it measures saturation levels,
not small-signal gain). Gain correction stays off.

**IF VGA fixed, like stock osmocom librtlsdr (decided 2026-09-30)**: the
krakenrf fork leaves the R820T IF VGA under the RTL2832 AGC even in manual
gain mode (`r82xx_set_if_mode`, if_mode 0). heimdall overrides that after
every gain setup with `rtlsdr_set_tuner_if_mode(10000 + index)` (fixed step,
RTL2832 AGC loop off): step 8 (16.3 dB) with manual gain, 11 (26.5 dB) with
tuner AGC - `R820T_IF_VGA_*` in heimdall's config.h. Measured effects:
- gain stable to ~0.1 dB between snapshots, no ~10 dB noise-on ramp
- the noise source clips ~11-14% of samples at step 8 at ANY RF gain (it also
  compresses the tuner front end, so the RF gain doesn't lower it); simulated
  phase bias from that is <= ~0.26° - accepted
- strong antenna signals clip at 49.6 dB RF gain (clean at ~42 dB): lower the
  RF gain at strong-signal sites
- switching the VGA down only while the noise source is on was REJECTED:
  the VGA step itself shifts each dongle's phase differently (~1° at step 1,
  up to ~8° at step 0, vs step 8), so calibration and operation must use the
  same step
- runtime tuning aid: control port `{"command":"set_if_vga","index":0-15}`
  (not persisted; the next gain change re-applies the configured step)

**Why Eigenvalue Decomposition**:
- More robust than correlation peak phase
- Handles multi-path and interference better
- Uses spatial correlation matrix of all channels

### IQ Conversion (DoA Client)

**Approach**: Simple scalar loop with compiler auto-vectorization

```cpp
for (size_t i = 0; i < num_samples; i++) {
    output[i] = std::complex<float>(
        input[i*2] / 127.5f - 1.0f,      // I
        input[i*2+1] / 127.5f - 1.0f     // Q
    );
}
```

**Why This Works**:
- GCC -O3 -march=native auto-vectorizes this pattern
- Outperforms manual NEON intrinsics by 8.5%
- Zero function call overhead
- Predictable memory access for cache optimization

### Decimation (DoA Client)

**Architecture**:
- Thread-local decimator instances (zero allocation per call)
- Shared coefficient cache with 95%+ hit rate
- Pre-reserved buffers to avoid dynamic allocation
- Integer decimation factors (no resampling)

## Development Workflow

### Adding New DSP Features to Server

1. Determine which module: typically `src/dsp/`
2. Update relevant header file in `src/dsp/`
3. Implement in corresponding `.cpp` file
4. Add to correlation processing loop if needed (`correlation.cpp`)
5. Update WebSocket message format in `web_server.cpp`
6. Rebuild: `make rebuild`

### Adding New Client Features

1. Signal processing: Add to `src/signal_processing/`
2. UI control: Update `kraken_doa.html` and WebSocket handlers
3. Configuration: Add to `include/config.hpp`
4. Rebuild: `make rebuild`

### Modifying Web UI

**Server UI** (`heimdall_v2/index.html`):
- Loaded at runtime by `html_loader.cpp`
- Supports template variable substitution
- No rebuild needed, just refresh browser

**Client UI** (`kraken_doa_v2/kraken_doa.html`):
- Loaded at runtime
- No rebuild needed, just refresh browser

### Adding TCP Commands

**Server** (`src/net/tcp_control_server.cpp`):
1. Add command handler in `handle_command()`
2. Update JSON command parsing
3. Test with: `echo '{"command":"your_command"}' | nc localhost 8092`

**Client** (`src/control_handler.cpp`):
1. Add command handler in control message processing
2. Update WebSocket message handling

## Common Issues and Solutions

### Server Issues

**No RTL-SDR devices found**:
- Check `config.h` serial numbers match physical devices
- Run `rtl_test` to enumerate devices
- Verify USB permissions: `sudo usermod -a -G plugdev $USER`

**Poor correlation peaks**:
- Verify all devices on same frequency
- Check antenna connections
- Ensure bias-tee is enabled if using active antennas

**Phase compensation not converging**:
- Wait for lag compensation to complete first (30-60 seconds)
- Increase `PhaseCompensationData::required_stable_readings` in types.hpp
- Check signal strength on all channels

**Lag servo ringing or slow convergence**:
- The servo gain auto-derates with the measured control update period; check
  CPU load first (slow correlation passes lengthen the feedback delay)
- Tune `ChannelCompensation::servo_gain` / `servo_max_counts` in types.hpp

### Client Issues

**FFT display frozen**:
- Check TCP connection to Heimdall server (port 8091)
- Verify server is running and streaming data
- Check browser console for WebSocket errors

**Audio dropouts**:
- Browser audio sample rate mismatch: Check console for actual rate
- Update `AUDIO_SAMPLE_RATE` in config.hpp to match
- Reduce decimation factor if CPU is overloaded

**High CPU usage on Raspberry Pi**:
- Reduce number of active channels
- Increase decimation factor (reduce bandwidth)
- Disable DoA processing if not needed

## Dependencies

### Heimdall Server

**KrakenSDR librtlsdr fork** ([krakenrf/librtlsdr](https://github.com/krakenrf/librtlsdr)),
linked STATICALLY from `librtlsdr/build/src/librtlsdr.a` (gitignored source
tree at the repo root):
- Why static: a distro `librtlsdr0` lives earlier in the loader's search path
  (`/usr/lib/aarch64-linux-gnu`) than `/usr/local/lib`, so a dynamically
  linked heimdall would silently load the stock library and lose the fork's
  features (register-only sample-clock servo, GPIO bias-tee control). `ldd
  heimdall` shows no librtlsdr.
- heimdall's `make` clones/builds just the static library when it's missing;
  `install.sh` builds the fork and installs it to `/usr/local` (library,
  headers, udev rules) with its `rtl_*` tools linked statically too
  (`-DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON`)
- Do NOT install `librtlsdr-dev` for heimdall (distro `librtlsdr0` /
  `rtl-sdr` packages may stay installed for other software)

**Required System Libraries**:
- `libusb-1.0-0-dev`, `cmake`: build and link the librtlsdr fork
- `libfftw3-dev`: Fast Fourier Transform (single-precision `fftw3f`)
- `libeigen3-dev`: Eigenvalue decomposition for phase calibration
- `libssl-dev`: SSL/TLS for uWebSockets
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies**:
- `uWebSockets/`: HTTP/WebSocket server (auto-installed by Makefile)

### DoA Client

**Required System Libraries**:
- `libfftw3-dev`: FFT processing
- `libliquid-dev`: Digital signal processing (decimation, filtering)
- `libeigen3-dev`: MUSIC DoA algorithm
- `libssl-dev`: SSL for WebSocket server
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies**:
- `uWebSockets/`: WebSocket server (auto-installed by Makefile)

## Testing

### Manual Testing Workflow

**Server**:
1. Start server: `cd heimdall_v2 && make run`
2. Open web interface: http://localhost:8070
3. Verify all channels show correlation peaks
4. Monitor lag compensation: All channels should reach CONVERGED within 60s
5. Monitor phase compensation: Should apply once and remain stable
6. Test RTL-TCP: Connect SDR# or GQRX to `localhost:1234`
7. Test control: `echo '{"command":"get_status"}' | nc localhost 8092`

**Client**:
1. Ensure server is running first
2. Start client: `cd kraken_doa_v2 && make run`
3. Open web interface: https://localhost:8080
4. Verify FFT display updates
5. Test FM demodulation: Enable and check audio
6. Test DoA: Enable and verify direction estimates
7. Test bandwidth changes: Should update smoothly without gaps

### Integration Testing

1. Start server first
2. Wait for phase compensation to converge
3. Start client
4. Verify end-to-end data flow
5. Test frequency changes via server web UI
6. Verify client updates bandwidth appropriately

## Logging

Both apps write informational messages to stdout and errors/abnormal events to
stderr. On an interactive terminal, stdout drives the live StatusDashboard TUI
(or the plain scrolling logs with `HEIMDALL_NO_TUI` / `KRAKEN_DOA_NO_TUI`).
When stdout is NOT a terminal (redirected to a logfile or journal, e.g.
`run.sh` headless mode), stdout is **discarded at startup** so a deployment
that runs for years accumulates an errors-only log that cannot grow unbounded
from routine output. Set `HEIMDALL_VERBOSE_LOG=1` / `KRAKEN_DOA_VERBOSE_LOG=1`
to restore full output to files for debugging.

All cout/cerr output is made terminal-safe (`SanitizingBuf` installed in
each `main()`, and the TUI's log capture): log lines carry client-chosen
strings (WebSocket commands and close reasons, filenames, settings values),
so C0/C1 control characters (ESC, `\r`, BEL, ...), DEL and invalid UTF-8 are
replaced with `?` - an escape sequence can't recolor/rewrite the terminal,
TUI or a `tail`ed log file. Valid UTF-8 passes through. The apps' own log
lines must therefore not contain escape codes (the dashboards write theirs
straight to the terminal fd). Header: `heimdall_v2/src/core/log_sanitize.hpp`
= `kraken_doa_v2/include/utils/log_sanitize.hpp` (keep them identical).

Everything that indicates a fault goes to stderr and therefore still reaches
the log: coherence loss + recovery progress, periodic-calibration DRIFT,
locked-channel lag drift, device/GPIO/downconverter failures, client
connection loss, fatal startup errors. Recurring per-broadcast warnings in the
TCP data server are throttled (5 s) so a persistent fault cannot flood the
log. `run.sh` headless mode truncates `logs/*.log` at each start; a few
static-constructor lines printed before `main()` still land there (bounded,
once per start).

## Performance Notes

### ARM NEON Optimizations

Both applications automatically detect ARM architecture and enable NEON optimizations:

**Heimdall Server**:
- ARM NEON vectorization for sample processing
- Automatic fallback to scalar on x86_64
- Optimized for Raspberry Pi 4 Cortex-A72

**DoA Client**:
- IQ conversion: Compiler auto-vectorization preferred over manual NEON
- Decimation: Thread-local instances with shared coefficient cache
- FFT: FFTW3 wisdom files for optimal plans

### Memory Management

**Server**:
- L1 buffers: Per-device circular buffers
- L2 buffer: Global synchronized queue (moodycamel::ConcurrentQueue)
- FFT pool: Thread-safe FFTW plan pool (RAII management)

**Client**:
- Raw data buffer: Bounded queue with automatic cleanup
- Ring buffers: Lock-free for FFT data
- Malloc arenas: Increased to 32 for multi-threaded performance (`mallopt(M_ARENA_MAX, 32)`)

## Network Access Checks

- **Host allow-list (DNS rebinding):** heimdall's WebSocket/POST routes and the
  client's WebSocket upgrade, `/recordings` and 8081 page only answer requests
  addressed by an IP literal, `localhost`, this machine's hostname (bare or
  `.local`/`.lan`/`.home`/`.localdomain`/`.home.arpa`), or a name listed in
  `KRAKEN_ALLOWED_HOSTS` (comma-separated, both apps - set it when reaching
  the Pi through a reverse proxy or a custom DNS name). Header:
  `heimdall_v2/src/core/host_check.hpp` = `kraken_doa_v2/include/utils/host_check.hpp`
- **Same-origin:** both apps refuse a browser request whose `Origin` doesn't
  match its `Host` (cross-site WebSocket hijacking); requests without `Origin`
  (native apps, curl) only need the host check
- **API token (client, optional):** unauthenticated sockets get 10 s to send
  `AUTH:`, at most 8 may wait at once, and an IP with 5 wrong tokens in 60 s
  is refused for the rest of that window
- **Control port 8092** has no auth (LAN tools / the DoA client) but hangs up
  on any HTTP request: a web page's fetch/form POST to it would otherwise have
  its headers skipped and a JSON body run as a command
- **Local web-mapper port 8021** (opt-in) checks the Host allow-list but no
  Origin: it exists for mapper pages served from elsewhere, so anything on the
  LAN can read it by design
- **8081 `DOA_value.html`** stays token-free (the Android app can't send one)
  but has no CORS header, so other web pages can't read it

## Port Reference

**Heimdall Server**:
- **8070**: Web interface (HTTP + WebSocket)
- **8091**: TCP data server (multi-channel IQ streaming)
- **8092**: TCP control server (JSON commands)
- **1234**: RTL-TCP server (rtl_tcp compatible, selectable channel). A
  read-only tap: client tuning commands (frequency, gain, sample rate) are
  read and logged as ignored, since applying them would retune the whole DF
  array. Reports an R820T; TCP keepalive plus a 10 s send-stall limit free the
  single slot from a vanished client

**DoA Client**:
- **8080**: Web interface (HTTPS + WebSocket)
- **8081**: Plain HTTP DoA value page (Android app)
- Connects to server ports 8091 (data) and 8092 (control)

## Git Secret Guards

Station secrets (KrakenPro API key, latitude/longitude) are kept out of git
automatically:

- **Clean filter** (`.gitattributes` → `scripts/git-secrets-clean`): when a
  settings/config JSON is committed, the blob git stores has
  `krakenpro_key`/`web_mapper_key` emptied and `latitude`/`longitude`/
  `static_location` zeroed. The WORKING file keeps its real values.
- **Pre-push hook** (`.githooks/pre-push`): backstop that scans outgoing
  commits and blocks the push if a key / real coordinates / a tracked
  `doa_settings.json` or `api_token` slipped through (bypass:
  `git push --no-verify`). Root commits and merge commits (including a
  secret added while resolving a merge) are scanned too.
- Filters and hooks are per-clone git config, so `install.sh` registers them
  (`filter.kraken-secrets.*`, `core.hooksPath .githooks`). A clone that never
  ran install.sh silently skips both — run it once after cloning.
- `doa_settings.json` (runtime home of the key + station location) is
  gitignored and must stay untracked.

## Additional Documentation

- **Server**: See `heimdall_v2/CLAUDE.md` for detailed server architecture
- **Server**: See `heimdall_v2/README.md` for module details
- **Client**: See `kraken_doa_v2/OPTIMIZATION_SUMMARY.md` for performance analysis
- **Client**: See `kraken_doa_v2/AGENTS.md` for development workflow
