# Suite reference (long form)

Moved verbatim from the root `CLAUDE.md` on 2026-10-24 (that file now keeps only
what every session needs). Not loaded automatically - read it when working in
this area. Where this and the component `CLAUDE.md` files disagree, the code and
the component files win.

Architecture, configuration, every client feature in summary, key
algorithms, dependencies, testing, performance. The topic files next to
this one hold the sections that were moved out of it (run-and-deploy,
windows, librtlsdr-fork, macos, compiler-flags, licensing).

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

Windows port (see `windows.md`): `WINDOWS.md`,
`build_windows.bat` and `run.bat` at the root, `windows/` (MSYS2 setup,
build and package scripts, the `krakensdr.exe` launcher's source).

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
# install.sh or by `make` above; CMake stops with a message if it's missing
# or older than krakenrf/librtlsdr 578b43d)
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
- **DSP Module**: FFT (pffft, vendored `external/pffft`), cross-correlation, eigenvalue-based phase calibration (Eigen3)
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
- Per-VFO decimator: block FIR stages, split as a cost model finds cheapest
  (halfband stages, the mixer folded into the first stage)
- FFT: pffft (vendored `kraken_doa_v2/external/pffft`, BSD-style licence;
  replaced FFTW 2026-10-17) behind `plugins/sdk/kraken_fft.hpp` - also the
  decoder plugins' FFT
- liquid-dsp built from source (`kraken_doa_v2/liquid.mk`: pinned v1.7.0,
  static, NEON dot products, WITHOUT FFTW / libfec - Debian's libliquid1
  links both): nothing GPL linked into kraken_doa or a plugin; its filters /
  decimators run 1.5-3.4x faster than Debian's build
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

**VFO bandwidth (any width; details: `kraken_doa_v2/CLAUDE.md` *Arbitrary VFO
bandwidth*)**:
- ANY whole Hz from 1 kHz to 2.4 MHz (the user's request 2026-10-21; it was
  the 30 `BANDWIDTH_OPTIONS` sizes): `DecimatorInstance::bandwidth_hz`, WS
  `SET_DECIMATOR_BW_HZ:id:hz` (`SET_DECIMATOR_BW:id:index` still works), the
  DECIMATORS snapshot's last field. Still integer decimation only (no
  resampling): by the largest divisor D of 2.4 MHz whose rate holds the
  width, the cascade's last stage cut at the width itself (gain 2.4 MHz /
  width, = D as before for the old sizes). Measured on the Pi 5: up to 1.2
  MHz a custom width costs what the efficient size at its rate does (the
  rate <= 1.25x the width at 300 kHz and below); 1.2 - 2.4 MHz runs at 2.4
  MS/s with a full-rate filter (+~9 % of a core with 5 antennas over the
  2.4 MHz pass-through; the BW box's tooltip says so). The stages are block
  FIRs (`SharedDecimator::FirStage`, replacing liquid's firdecim - whose
  per-sample window pushes were most of its cost; 4 outputs per NEON
  pass), split as a cost model of them finds cheapest
  (`SharedDecimator::planStages`, 2026-10-22: any order of factors 2..8,
  halfband stages for 2, the mixer folded into the first stage's taps where
  that is cheaper - measured against every candidate plan of 28 widths, the
  pick is the fastest or within 2 %), a block through the whole cascade in
  1024-sample chunks (the stages' buffers stay in L1 - in the live
  pipeline the first stage was memory-bound: an extra 12.5 kHz VFO with
  DoA and 5 antennas cost ~19 % of a core, ~6-7 % now), a first stage
  whose input isn't mixed in front filtering straight from the input block
  (no copy into it), every VFO in one pass over each antenna
  (`decimateVfos`: three narrow VFOs ~24 -> ~20 % of a core in user-space
  cycles, one VFO unchanged); kraken_doa with 1 / 2 / 3 such VFOs 28 / 49 / 67 ->
  18 / 26 / 31 % of a core (perf-profiled 2026-10-22, before the one pass): the first block FIR was 3.3x the
  decimator before arbitrary widths, this 2.0x the first block FIR;
  kraken_doa against the build before arbitrary widths (measured the day
  before, same harness) 22-31 % less CPU at every old size but 2.4 MHz (6 %;
  240 kHz 50.1 -> 35.2 % of a core, 12 kHz 36.4 -> 26.9, 1.2 MHz 87.4 ->
  60.4), the same response (checked against a double-precision reference of
  each plan). No snapping option (an "Efficient bandwidths only" tick was
  removed the same day, the user's call). The VFO card's BW is a number box
  in kHz
- Supports 2.4 MHz down to 1 kHz bandwidth (the narrow end - 10 kHz to 1 kHz -
  is for weak CW beacons, wildlife tags, fox hunts). MUSIC accumulates across
  blocks (a 1 kHz block is ~7 samples) and caps a frame at ~1 s of samples
  (min 4 snapshots), so narrow bandwidths still give ~1 bearing/s; the
  beamformed FFT collects short blocks until it has 64 samples. The web UI
  draws a VFO at least 8 px wide so a 1 kHz bar stays visible/grabbable
- VFO tuning step (top bar `Step:`, per browser, 1 Hz - 100 kHz incl. 6.25 /
  8.33 / 12.5 / 25 kHz; default 1 kHz): a tuning-bar drag or a click on the
  spectrum / a pane puts the VFO on a multiple of it (its absolute
  frequency); Shift while dragging = fine, 1/100 of the movement to the Hz.
  Typed / saved frequencies and the card's digits stay exact. A drag used to
  move only past 1 kHz, in whole pixels (~1093 Hz). Details:
  `kraken_doa_v2/CLAUDE.md` *VFO tuning STEP*

**Scanners**:
- The discrete (server-side hop), continuous and saved-frequency scanners
  are mutually exclusive: starting one while another runs is refused and
  the browser's button reset
- 📻 Saved-frequency scanner (`kraken_doa_v2/src/sf_scanner.cpp` +
  `sf_scan_plan.cpp`; details: `kraken_doa_v2/CLAUDE.md` *Saved-frequency
  scan*): scans the saved frequencies ticked in the 📻 list - one by one or
  a whole category (its `scan` flag: also entries added later; a frequency's
  `scan` -1 leaves it out) - with the page's SELECTED VFO: groups them into
  tuner bands (all entries whose VFO fits the edge-clipped 2.4 MHz of one
  centre - a centre kept off every entry's channel: DC is removed / the
  RTL's spike; entries with a gain only share a band with that gain), watches
  every entry of a band at once in the averaged FFT (squelch = dB over the
  noise floor) and retunes only between bands. Coherent: one band at a
  time (Dwell each), nothing measured while heimdall recalibrates after a
  retune; independent: a band on every tuner no other VFO is on; wideband
  scan (the user's question 2026-10-22 - it had been refused for no good
  reason): one band = heimdall's tuner spread (n tuners side by side, edge
  clip x 2.4 MHz apart, ~10.8 MHz with 5), each entry on the tuner nearest it
  and its VFO kept off the seams, retuned with WIDEBAND_FREQ (FREQ would end
  the mode), no calibration (`sf_scan_plan`'s Layout). Lock = the
  entry's decoder / demod / bandwidth + the VFO's offset (the page's
  sfTune), heard; Delay after the transmission, Skip / Hold / Longest stay
  (clean listening time only). The list goes in PASSES: a frequency left by
  Skip / Longest stay has had its turn until the next pass (always-on
  broadcast stations take turns round and round). With each retune the VFO
  is pre-placed, muted, on the band's first frequency (ready when the array
  has recalibrated); while the array calibrates a locked VFO is muted.
  Coherent: while it SEARCHES it holds heimdall's calibration (8092
  `hold_calibration`, a 3 s lease renewed every second - heimdall_v2/CLAUDE.md
  *Calibration hold*): retunes skip the 3 s cooldown + noise-source
  calibration, so bands take turns in well under a second; a lock releases
  it and the calibration starts at once (the VFO muted until it has run).
  ⚙ "VFOs: one per active frequency" (`SFSCAN_MULTI:`, persisted, off by
  default; the user's request 2026-10-22, for Voice to Text): every
  frequency active at once in the band(s) watched is listened to at once -
  its VFO the first, a VFO the SCAN MAKES each further one (a copy of its VFO
  as it was at the start: demod, bandwidth, decoder, squelch, Transcribe;
  then the entry's own settings), each ending on its own (Delay, Longest
  stay); a made VFO is removed then and at the stop (never saved; made /
  removed through the manager + a `decimator_info` broadcast - the
  decimator_added / _removed messages reset every page's audio player); one
  the user removes = that frequency done this pass; <= 16 VFOs.
  Its own commands go through `apply_command` but are NOT saved
  (`sfscan::dispatching()`); the tuning it leaves is saved at stop. A manual
  retune (FREQ, TUNER_FREQ of a scanned tuner, WIDEBAND_FREQ), a mode change,
  removing its VFO or a playback stop it. WS `SF_SCAN:`, `SFSCAN_START:<vfo>` / `STOP` / `SKIP` /
  `HOLD:` / `SQUELCH:` / `DWELL:` / `HANG:` / `MAX:` / `MULTI:` (persisted),
  `GET_SFSCAN` -> `{"sfscan":{...}}`
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

**Spectrum to the browser (spectrum views, FFT message type 7)** (details:
`kraken_doa_v2/CLAUDE.md` *Spectrum views*): each page tells kraken_doa which
part of each display it shows (`FFT_VIEW:` - window from the centre + its
plot's width in device pixels) and gets that window max-pooled from the
full-resolution FFT to about its width (at most 4096 points): zoomed in, the
same points cover less spectrum - down to one FFT bin - so the detail grows,
the data rate doesn't; the beamformed overlays (type 4) follow each page's
spacing. Frames carry start + step instead of a frequency per point and only
the max envelope, and go permessage-deflate compressed: a page took ~250
KB/s before, ~20 KB/s now. The "Downsampling" setting is gone (FFT_DECIMATION
ignored); FFT Settings' "Slow link" tick (per browser) asks for half the
points. Pages that never send `FFT_VIEW` (an open tab from before) keep
getting the old type 0 / 6 frames at 2048 points per tuner band

**Digital voice/data decoders (per VFO)**: engine `kraken_doa_v2/src/digital/`,
decoders `kraken_doa_v2/plugins/`
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below):
  p25, dmr, tetra (downlink), dstar, nxdn (48 and 96), mpt1327 (analogue
  trunking signalling), pagers (POCSAG + FLEX; was pocsag), aprs, adsb (1090 MHz; manual only -
  picking it tunes the VFO + tuner to 1090 MHz at 2.4 MHz and draws the VFO
  as a locked line, `kp::Info::fixed_freq_hz`; v2.0: in coherent mode with
  DoA on it combines every antenna - `kp::Info::multichannel`, a beam per
  weak message, overlapping messages separated, each message's own pulse
  covariance as its DoA - ~6 dB more sensitive with 5 antennas, plus
  extended bit repair and long CPR pairing; each an option in its tab;
  `kp::Info::lock_vfo` -
  ism, cells, dtv - locks a 2.4 MHz VFO at its tuner centre the same way
  without tuning), ais (ships, both channels
  161.975 / 162.025 MHz in one 100 kHz VFO on 162.000 MHz; manual only,
  fixed frequency like adsb) and radiosonde (weather balloons, 400-406 MHz:
  RS41, DFM, M10 / M20, iMet-4 / -54, LMS6, MRZ side by side in one plugin;
  weather facts, a sounding in the event log, balloon map points), dme
  (DME / TACAN beacons, 962-1213 MHz: every 1 MHz channel inside the VFO -
  a 2.4 MHz VFO covers up to three - with its Morse ident, VOR / ILS
  pairing, pulse data, exact frequency, TACAN bearing, aircraft
  interrogations; a DoA per beacon; manual only), ils (ILS localizer /
  glide slope DDM, VOR radial, marker beacons, 1020 Hz idents; 48 kHz) and
  ism (licence-free 315 / 345 / 433 / 868 / 915 MHz devices anywhere in
  the 2.4 MHz the tuner receives: weather stations, TPMS, key fobs /
  remotes, alarm sensors, energy meters - 69 OOK / FSK protocol decoders,
  unknown repeating codes too; a device table and a DoA per device; manual
  only; `kp::Info::lock_vfo`: picking it puts the VFO on its tuner's centre
  at 2.4 MHz, drawn as a locked line, the tuner left where the user put it;
  `kp::Info::multichannel`: in coherent mode with DoA on it gets EVERY
  antenna - `process_multi`, wire SAMPLES_MC - and combines each burst from
  all of them with its own covariance's main eigenvector: >= 6 dB more
  sensitive with 5 antennas; one channel as before in independent mode),
  radar (pulsed radars in the 2.4 MHz - L-band ATC / en-route surveillance
  1215-1400 MHz, wind profilers: pulses, chirp, PRI pattern, scan, beam
  width; a DoA per radar from its main beam; manual only, `lock_vfo`),
  ssr1030 (the 1030 MHz uplink: SSR / Mode S / TCAS interrogations, Mode S
  bits read at 2.4 MHz, a DoA per interrogator; manual only, fixed 1030 MHz),
  uat (the US 978 MHz ADS-B link: aircraft, TIS-B / ADS-R traffic, ground
  station uplinks with their FIS-B products / DLAC text; RS GF(256); a DoA
  per aircraft and per ground station - TIS-B is the station's signal;
  manual only, fixed 978 MHz; no UAT in NZ: tested on real payloads
  re-modulated) and flarm (FLARM + OGN trackers on every regional channel
  inside the 2.4 MHz - EU / NZ: tune 868.8 MHz; FLARM by radio ID + bearing
  only, its payload is encrypted and NOT decoded; OGN fully, stealth =
  no-track honoured; a DoA per aircraft on its own channel; manual only,
  `lock_vfo`, `tunes` once to the band) and analog_id ("Analog FM IDs",
  was `ctcss` - renamed 2026-10-22 when the radio IDs were added, the
  user's request; saved `PLUGIN:ctcss` VFOs / `ctcss.*` options load as
  analog_id: everything on analog FM that names a transmitter, on a 12-24
  kHz VFO: CTCSS tones / DCS codes (every standard + non-standard tone,
  measured to ~0.01 Hz, reverse burst; DCS 23-bit Golay words, normal /
  inverted, turn-off code - ETSI TS 103 236), MDC1200 (Motorola unit IDs,
  PTT ID pre / post, emergency, status, call alert ...; read coherently,
  convolutional code + CRC), FleetSync / FleetSync II (Kenwood fleet-unit,
  1200 / 2400 bit/s; read coherently by a Viterbi over the phase), 5-tone
  selcall (ZVEI1-3 / DZVEI / PZVEI / CCIR / EEA / EIA / NATEL / MODAT), DTMF
  (talk-off protected) and tone bursts (1750 Hz repeater access); each
  transmission ONE talker named by the strongest of what it carried -
  sender ID > selcall / DTMF > tone / code > burst ("MDC 1029", "FleetSync
  100-1001", "CCIR 12345", "CTCSS 88.5") - from its onset to its end, a
  DoA per radio; IDs sent at release rename it; valid spans every 50 ms
  drive the Digital squelch, `kp::Info::squelch_hold_s` 0.35 s; option
  "ID / tone squelch" = only those IDs / kinds / tones open it and get
  talkers; no voice: Voice to Text keeps the analog demod and names the
  transcript after the radio) and beacon406 (Cospas-Sarsat 406
  MHz distress beacons - EPIRB / ELT / PLB / SSAS - on every 406.0-406.1 MHz
  channel of a 100 kHz VFO, manual only, fixed 406.050 MHz: C/S T.001
  first-generation bursts (160 ms carrier + 112 / 144 bits biphase-L at 400
  bit/s), BCH(82,61) / BCH(38,26) corrected, every protocol (user, user-
  location, standard / national location, RLS, ELT(DT), test,
  orbitography): 15-hex ID, country, type, MMSI / aircraft address /
  registration / serial / call sign, the encoded position on the map;
  SELF-TEST frame sync = a test, any other = a "⚠ DISTRESS" alert (to be
  reported to the RCC by the user - the decoder reports nothing); a DoA per
  15-hex ID (a packet per burst on its own 3 kHz channel, kept 24 h, DF kind
  `sar` 🆘); second-generation (T.018 DSSS) bursts detected, not decoded) and
  lora (LoRa CSS in the whole 2.4 MHz, manual only, `lock_vfo`, `tunes`;
  REGION-NEUTRAL: `region` / `lorawan` Auto = every Meshtastic region (the
  firmware's 25: US, EU_433 / 868, CN, JP, ANZ / ANZ_433, RU, KR, TW, IN,
  NZ_865, TH, UA, MY, SG_923, PH, KZ, NP_865, BR_902) and every LoRaWAN
  RP002 plan (EU868, US915, AU915, AS923-1..4, KR920, IN865, RU864, CN470,
  EU433, CN779 - uplinks, downlinks / RX2 inverted IQ, class-B beacons with
  their CRCs) whose band holds the tuned frequency, within a CPU budget by
  priority; specific ones selectable, Auto never retunes. A PLAN-FREE SEARCH
  finds LoRa anywhere in the VFO (any centre, SF5-12, 62.5-500 kHz - option
  `all`: down to 7.8 kHz -, both IQ): LoRa-shaped energy in the channelizer's
  FFTs -> dechirped at every SF from 0.8 s of FFT history -> a decoded
  transmitter becomes a LEARNED channel. SF5 / SF6 (SX126x, header
  convention not verified on a real radio), BW 7.8-500 kHz, CR 4/5-4/8,
  explicit / implicit header (beacons); the radios' coding chain (verified on
  real AU915 gateway downlinks at 927.5 MHz). Meshtastic: the clear 16-byte
  header, the payload decrypted ONLY with the PUBLISHED default channel key -
  a private channel shows its header only, keys are never guessed - into the
  Data protobuf (text, position on the map, nodeinfo, telemetry); LoRaWAN:
  clear MAC header only - DevAddr / FCnt / FPort, join DevEUI - the payload
  is encrypted. ID separation: a DoA per node / DevAddr on its own channel,
  the RELAYING node (`!relay-XX`) when a Meshtastic frame was repeated, a
  LoRaWAN DOWNLINK / beacon = its gateway ("GW 927.5"), other LoRa its channel
  + SF + sync word ("LoRa 926.901 SF7 0x12"), DF kind `node` 📶; SEPARATION
  BY CODE (`multichannel`: every decoded symbol de-spread with its own chirp
  on every antenna -> the packet's own covariance, `kp::Talker::cov` -
  never dropped / mixed when transmitters overlap; two gateways behind one
  "GW" ID split into "#1 / #2" by their signatures). Live at
  927.5 MHz the search found the private SF7 BW250 sync-0x12 system on 926.9
  / 927.4 MHz whose "SF9 BW500" bursts had never decoded (same chirp slope)
  and learned both channels) and wildlife (wildlife tracking collars /
  tags - pulsed CW, any country, VHF / UHF - anywhere in the VFO at the
  VFO'S OWN RATE:
  `kp::Info::sample_rate` 0, new - `kp::Host::sample_rate()`, the plugin
  started `--serve --rate HZ` and restarted on a bandwidth change -, so a
  2.4 MHz VFO surveys a band and a narrow one follows one collar; manual
  only, talkers. ~250 Hz FFT bins,
  each pulse measured on the raw samples (edges, carrier to a few Hz,
  C/N0); a collar = its pulse train (period with misses / extras, ppm,
  pulse) named by its frequency in MHz; REGION-NEUTRAL by default
  (profile `global`, the user's call): modes from the collar's own rate -
  baseline, a sustained rise held 2 min = ⚠ possible mortality, toggling
  between known rates = activity; region / vendor profiles only ADD
  (`profiles.hpp`: na / eu / au doubling = MORTALITY, motus channel names,
  nz = DOC channels + rate table + Wildtech data streams - time since
  death, Chick / Egg Timer; without a scheme a stream shows as its pulse
  groups), band hints from a worldwide list; Lotek / Motus-style coded
  tags (4-pulse bursts) by their gaps +
  burst interval, several on one frequency; keyed-carrier checks drop FSK /
  FM lines, impulses, carriers. ID separation: a packet per pulse on its
  own 1 kHz channel, DF kind `collar` 🐾, `names` option. kraken_doa:
  packet channels <= 10 kHz in a wide VFO are decimated before the
  filter - `narrow_cov`) and rds (FM broadcast: RDS / RBDS on 57 kHz,
  RDS2 streams, the pilot and every SCA subcarrier - FM audio, FSK data,
  DARC; Listen plays an SCA or the main programme; every antenna combined
  before the FM discriminator; a DoA per station by its PI - see *📻 RDS /
  RBDS + SCA plugin* below).
  Plugins learn the VFO's RF from `kp::Host::rf_hz()` (wire RF, 7). Each decimator (VFO) runs one
  plugin, or AUTO (every plugin the user left ticked for "Auto detect" in the
  sidebar's plugin list - all by default, `PLUGIN_AUTO:id:0|1`, persisted as
  `AUTO_DETECT_OFF:` - at once, minus `manual_only` plugins and those
  needing a VFO > 2x wider; the
  one whose frames pass FEC/CRC wins). Any
  number of VFOs decode at once: a "Digital decoder" tick + mode list on each
  VFO card (Decimators box; the list grouped by `kp::Info::category` - Two-way
  radio, Aviation, Marine & distress, ... - plugins/SDK.md), one tab per decoder mode (plugin - all of them, always, also unused ones) in the
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
- Voice: Demod "Digital" (or the panel's Listen button) on a VFO heard (🔊)
  plays the decoded voice. P25 IMBE is built in (port of mbelib, ISC); DMR /
  D-STAR / NXDN AMBE use a user-installed mbelib (dlopen, `KRAKEN_MBELIB`), TETRA
  ACELP the user-built ETSI codec programs (`tetra-cdecoder` /
  `tetra-sdecoder` on PATH or `KRAKEN_TETRA_CODEC_DIR`; Windows also
  `tetra-*.exe` in the kraken_doa_v2 folder, fed through named pipes - the
  programs need file names, Windows has no /dev/stdin) - neither is shipped
  (licensing); README "Digital voice codecs" has the install steps
  (Windows: WINDOWS.md). Docker:
  files in `docker-data/codecs/` are picked up by the entrypoint. Encrypted
  calls are muted
- Fed from the VFO's decimated stream (beamformer output when beamforming
  runs), so DoA, beamforming and decoding can run on the same signal

**🔊 VFO audio (speaker toggle on each VFO card, all modes)** (details:
`kraken_doa_v2/CLAUDE.md` *VFO audio*): any number of VFOs heard at once -
the speaker next to a card's D0 / D1 title is green (heard) or red with a
cross (muted); it replaced the single "Audio Src" radio button. Each VFO
heard has its own demodulator + jitter buffer in
`kraken_doa_v2/src/signal_processing/audio_mixer.cpp`; the audio timer sums
them (soft-limited) into the one Opus stream the page plays. WS
`AUDIO_VFO:<id>:0|1` (`SET_FM_DECIMATOR:<id>` = only that VFO), saved in the
DECIMATORS snapshot head as list positions ("0,2", "-1" = none). LATENCY
CONTROL (2026-10-23 - after a CPU spike the audio stayed late against the
spectrum for good): the audio timer sends the packets due by the steady
clock on every platform (Linux's uSockets ran one callback for any number
of missed timerfd ticks), and both jitter buffers - each VFO's ring and the
page's AudioWorklet - drop what they held beyond their floor for a whole
window (2 s / 3 s; 0.1 s / 0.15-1 s floors, the page's adapting to feed
stalls), crossfaded; details `kraken_doa_v2/CLAUDE.md` *VFO audio*

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

**🤖 AI assistant (docked right-hand column; replaced the AI Signal Lab
2026-10-17)** (details: `kraken_doa_v2/CLAUDE.md` *AI assistant*):
- A chat with Claude Code (`claude -p`, one process per question,
  `--resume` within a conversation) that answers questions about what the
  page shows and what was received, looks transmitters up online (WebSearch /
  WebFetch: licence registers, sigidwiki, SondeHub ...), writes decoder
  plugins, and CONTROLS the receiver. `src/ai_manager.cpp` (job runner,
  conversations `ai/sessions/<id>/`, identifications `ai/identifications.json`
  = the spectrum's "AI" badges), `ai/kraken_ai.py` (the bridge: the agent's
  command line, permissions, sandbox settings, stream-json -> events),
  `src/ai_mcp.cpp` = kraken_doa's MCP server (streamable HTTP with JSON
  answers, `127.0.0.1:AI_MCP_PORT` 8079, a bearer token per run handed to the
  bridge in its environment): reading tools (screen, spectrum + peak hold,
  decoders, bearings, map points, transcripts, decoder log, saved
  frequencies, incidents, plugins, IQ capture + analysis, wait for events,
  identifications) and control tools (tune, gain, VFOs, decoders, options,
  DoA, saved frequencies) through `ControlHandler::apply_command` - refused
  while the panel's "Read only" (`AI_READONLY:`, persisted) is on. Each
  question carries the page's view (zoom window, selected VFO, tab, map).
  The dock's ⚙ row picks the model and effort (`AI_MODEL:` / `AI_EFFORT:`,
  persisted; passed to each question's `claude -p`); "Default" names what
  it runs - "Default (Opus 5.5)", "Default (Medium)" for the model picked -
  from Claude Code's cached model catalog (`kraken_ai.py status`)
- 🗺 The assistant plots on the map (`src/ai_map.cpp`: named layers of
  points / lines / areas / circles / bearing lines, from its own features,
  GeoJSON or a CSV - transmitter sites from licence registers, bearings,
  anything with a position; `ai/map_layers.json`, every page, the "🤖 AI
  layers" panel) and drives the asking page's map + Viewshed / path loss
  (they run in the browser: `{"ai_page":..}` commands the asking page
  carries out and answers - `AI_PAGE_REPLY`)
- IQ captures (`capture_iq`) are made INSIDE kraken_doa (`src/ai_capture.cpp`:
  the packets' bytes -> the VFO decimator's cascade, ~6 % of a core per
  antenna; `all_antennas` = every antenna for a plugin's offline bearings)
  - `ai/sigtool.py` (Python) analyses them
- Lockdown (the user's requirement: no core files): no user settings / MCP /
  CLAUDE.md / auto memory (`--setting-sources ""`, `--strict-mcp-config`,
  `CLAUDE_CODE_DISABLE_CLAUDE_MDS`, `..._AUTO_MEMORY`; without them each turn
  loaded both CLAUDE.md files + this repo's memory, ~224k tokens), path-scoped
  Read / Grep / Glob / Edit rules (a bare Grep allow reads anything not
  denied), Bash only in Claude Code's sandbox (bubblewrap + socat; none =
  no Bash), and kraken_doa runs every plugin the assistant changed under
  bwrap (`src/ai_sandbox.cpp`: its own list by ctime, `ai/state/`; also
  `--info` and the debug playback's generators); `CXXFLAGS` filtered; the
  main Makefile builds plugins with `-f Makefile` (a GNUmakefile there
  can't take over)
- OFF unless enabled on the Pi (`python3 ai/kraken_ai.py setup`); native
  install only. Tests in the job scratch (ai/t_*.py)
- Windows (`py -3 ai/kraken_ai.py setup` on the PC): everything but the
  sandboxes - no shell for the agent (Claude Code has no sandbox there) and a
  plugin it changed doesn't run until trusted (capture_iq and the viewshed /
  path-loss tools not yet tested there); see `windows.md`

**Decoder plugins** (details: `kraken_doa_v2/CLAUDE.md`):
- A plugin = a source folder `kraken_doa_v2/plugins/<id>/` (decoder.cpp +
  extras, API `plugins/sdk/kraken_plugin.hpp`, guide `plugins/SDK.md`), linked
  with `plugins/sdk/plugin_host.cpp` into its OWN executable
  `plugins/<id>/build/decoder` (kraken_doa_v2's `make` builds them; a plugin
  that fails to compile only warns). The Digital Decoder runs it as a child
  process per VFO (`Mode::PLUGIN`, wire form `PLUGIN:<id>`): complex baseband
  over stdin, facts/events/valid/audio/map points back over stdout (binary
  protocol in kraken_plugin.hpp; kraken_doa sends the station location). No
  child of kraken_doa inherits its sockets / files (closefrom(3) at every
  spawn, sockets SOCK_CLOEXEC - a plugin used to hold the old 8091
  connection open after a reconnect). A
  crash is reported and restarted with a back-off; a
  rebuild (atomic rename) restarts running decoders. Shipped: p25, dmr,
  tetra, dstar, nxdn, mpt1327, pagers (POCSAG + FLEX), aprs (written by the AI
  Signal Lab, the assistant's predecessor), adsb, ais, radiosonde, dme, ils, ism, radar, ssr1030, uat, flarm, analog_id, beacon406, lora, wildlife, rds, dtv and cells (ARRAY
  plugins: every antenna's samples, see *Digital TV* / *LTE / 5G NR Cell
  Decoder* below; both read the signals' own broadcast information). A
  plugin folder may also hold `CXXFLAGS` (extra compiler flags - only
  optimisation / target / -D ones are taken, `CXXFLAGS_ALLOWED`: -B and the
  like make the compiler run programs)
- Same executable tests offline: `decoder --file x.cf32 [--offset HZ] [--rf HZ]`;
  `--channels M --uca R` on a recording of every antenna prints each
  talker's bearing (ID separation)
- The shipped protocol plugins are thin wrappers around `plugins/lib/`
  (libkrakendig.a: FEC, 4FSK sync, vocoders, front ends) - kraken_doa itself
  contains no protocol code
- Plugins move between receivers as folders only: copy `plugins/<id>/`, run
  `make`, press ↻ (no web-UI export / import / rebuild - removed on purpose:
  compiling code sent from a web page was a recipe for issues)

**📻 Saved frequencies (sidebar) + 📍 map markers** (details:
`kraken_doa_v2/CLAUDE.md` *Saved frequencies*): `kraken_doa_v2/src/saved_freqs.cpp`
- One backend list (every browser): categories (user-made; Airband,
  Broadcast, Business band, Amateur, Marine, Emergency services to start),
  locations (= the 🗺 map markers) and frequencies - name + frequency
  required; category, location, demod, bandwidth, gain ("Keep the VFO's" by
  default, "Current gain" = the gain when saved, or any gain step), digital
  decoder, tone / code, notes optional. Several frequencies at one place share one location,
  so its marker lists them all (the editor snaps a new location within 15 m
  of a marker onto it)
- Sidebar "📻 Saved Frequencies": search, grouped by category (fold, 👁 =
  marks on the spectrum), ⚙ Categories (add / rename / recolour / delete),
  + Add / ☆ Save on a VFO card (filled in from that VFO), editor dialog
  (location: a marker, typed, picked on the map or the station's). A click
  on an entry tunes the SELECTED VFO: its decoder, demod and bandwidth (when
  set), then the frequency (retuning the receiver if outside the band), then
  its gain (when set; independent mode: the VFO's tuner)
- Spectrum: tags with the names at the top of the main spectrum and the
  independent-mode panes; click = tune
- ▶ Scan: a tick per entry and per category heading picks what the
  saved-frequency scanner scans (see *Scanners*), kept in the list itself
  (`scan` per frequency / category, `scan_uncat`; `SF_SCAN:`)
- WS `SF_SET:{json}` (+ newcat / newloc made in the same step), `SF_DEL:id`,
  `SF_LOC_SET` / `SF_LOC_DEL` (a location's frequencies stay, unplaced),
  `SF_CAT_SET` / `SF_CAT_DEL` (uncategorises), `GET_SF` ->
  `{"savedfreq":{cats,locs,freqs}}` to all pages. File
  `saved_frequencies.json` (cwd-relative, gitignored); the old
  `map_markers.json` (MARKER_* commands, removed) is migrated once on the
  first start and renamed `.migrated`

**🗺 Map (right-hand pane)** (details: `kraken_doa_v2/CLAUDE.md` *Map*):
- Street (OpenStreetMap) / satellite (Esri World Imagery) map drawn by the
  page itself (no map library, no CDN): without internet it keeps working on
  a lat/lon grid with a notice. Opened with the 🗺 Map button under the
  waterfall; a tab next to MUSIC DoA in coherent mode, the whole right pane
  otherwise; resizable; view settings per browser (localStorage)
- 🔍 Search (2026-10-21; kraken_doa_v2/CLAUDE.md *🔍 Map search*):
  coordinates in the usual forms, UTM, MGRS, grid locators, map links and
  the page's own places (markers, decoder points ...) read OFFLINE as typed;
  addresses / cities from OpenStreetMap Nominatim BY THE BROWSER, only on
  Enter, <= 1 request a second; a pin with the position in every form + 📍
  Add as marker. 🏠 = the station (was ⌖), 🎯 = the estimated transmitter
  (📡 DF heat map / ⏯ replay; again = the next)
- Shows the positions decoder plugins report (`kp::Host::map_point`: ADS-B
  aircraft, APRS stations, DMR GPS, D-STAR GPS / DPRS) per VFO decoder whose
  "🗺 Plot on map" is ticked in its decoder tab (off by default,
  `DIGITAL_OPT:id:map:0|1`, saved with the VFO), plus the station and range
  rings. kraken_doa pushes `{"map":...}` once a second (changed points + the
  keys of all live ones); `GET_MAP` asks for everything
- 📡 The station on the map: click the mast to select it (cyan, in a
  ring), then drag it, and drag its heading line's handle to turn it
  (static location only; right-click "📡 Set station here" switches to
  static, asking first) - sent as the Station Information panel's
  `STATIC_LOCATION:`; the station's viewsheds follow. Viewshed points are
  selected + dragged the same way
- 📍 User markers = the LOCATIONS of the saved frequencies (see *📻 Saved
  frequencies* below - one feature): right-click (touch: long press) the
  map -> "📍 Add Marker" (name, position, notes) or "📻 Save a frequency
  here"; a marker's panel at the map's bottom right lists EVERY saved
  frequency at that location - clicking one tunes the SELECTED VFO there
  (moves it inside the band shown, else retunes the receiver / the VFO's own
  tuner in independent mode), "+ Frequency" adds one. Pins take their
  frequencies' category colour when they share one
- 📡 Masts layer ("Masts" button, per browser): radio masts / towers /
  antennas from OpenStreetMap, fetched BY THE BROWSER from the Overpass API
  (overpass-api.de - not through kraken_doa): the 300 km region around the
  station in ONE query (Overpass rations queries per connection), other
  areas at zoom >= 11; kept a week in localStorage, drawn at any zoom;
  coloured by use (communication:* tags), click = all their tags, "📍 Add as
  marker". UI-only (kraken_doa.html "📡 OSM MASTS")
- 👁 Viewshed ("Viewshed" button; LINE OF SIGHT IS THE STATION'S ONLY -
  the user's call 2026-10-13, no right-click item for it, other sites
  hidden in that mode; in signal mode right-click "📶 Add TX antenna here"
  (the only way to add one; Delete removes the selected one), markers,
  masts; the station's mast carries the callsign): the ground in line of sight of an antenna there (height above
  ground, radius up to 1000 km, target height above ground or sea level,
  refraction k), anywhere in the world - AWS Terrain Tiles fetched BY THE
  BROWSER, computed in a Web Worker: rays along GREAT CIRCLES in RINGS (~8
  m cells around the antenna, coarser with distance; each ray carries its
  horizon outwards - one coarse grid lost the hills next to the antenna), Earth
  curvature, the tiles' artefacts removed (incl. a band of up-to-32 km
  "terrain" in the sea north of the Coromandel - cleaned per tile with
  its neighbours' edges, so it doesn't depend on the observer), decoded +
  cleaned tiles kept in the worker (256: a moved point / new height loads
  and cleans only the tiles it hasn't got - ~2 s per update at 1000 km on
  a Pi 5), up to 6 at once in their own colours, per browser
  (localStorage). UI-only (kraken_doa.html "👁
  VIEWSHED")
- 📶 Path loss = the viewshed's "📶 Signal" mode (its panel switches 👁 Line
  of sight | 📶 Signal - one feature, no separate button): how often (%)
  a TRANSMITTER's signal is good enough for receivers around it, also
  WITHOUT line of sight - coloured by that reliability, every shown
  transmitter at once (the best per place), the edge of the line of sight
  a black line (2 px; OFF by default - the panel's "Line of sight edge"
  switch, per browser); line of sight mode is the plain viewshed (both show
  the identical line of sight). Each site is a TX or an RX, added by right-clicking
  ("📶 Add TX antenna here"; no TX / RX tag to switch, no RX adding) - the STATION ISN'T PART OF THE SIGNAL
  MODE (the user's call: hidden there, never coloured or linked; it keeps
  its line of sight viewshed); ONLY TRANSMITTERS ARE COLOURED (the user's call): TX sites
  (receivers at the target height), and the TX place of a link from an RX
  site (`vsLinkItem`: around the place, receivers at that site's antenna
  height, out past it; worked out when the place is dropped) - an RX site
  itself isn't. The target can be in the air (above sea level). Per transmitter: the
  CHOSEN MODEL to every cell on the viewshed's rays (`plSweep`) - P.1812
  (the whole profile per ray via convex hulls, O(log n) per cell, EXACTLY
  P.1812 on the sampled profile), Longley-Rice (`itmCell`: NTIA's ITM on
  the ray's own points, incremental horizons / fits, delta h per cell -
  ~6x P.1812's time: labelled "much slower"), or free space (no terrain);
  the rays split over helper workers (`plSweepPar`: one per core but one,
  the same cells to the bit);
  Okumura-Hata was dropped (the user's call: no terrain, valid only for
  1-20 km / a 30-200 m base). LINK (right-click "📶 Path loss analysis from TX 1 to here" - the
  selected TX antenna, else the first; only once one is placed - markers,
  masts): a TX antenna -> a
  place, the great-circle terrain profile through ITU-R
  P.1812-6 (delta-Bullington diffraction + troposcatter, median, no
  ducting), NTIA's ITM / Longley-Rice (a JS port of the public-domain C++)
  and free space, drawn as a profile + link budget in a strip
  at the bottom of the map. Channel: ITU-R P.2108 clutter at the ends (3.1
  both on the ground, 3.3 air-to-ground at the ground end by elevation),
  the TX / RX antennas' polarization (vertical / horizontal / circular /
  a custom angle: mismatch cos^2 or 3 dB, capped by depolarization from
  the CLUTTER around each antenna - XPD 20 dB antennas' own, 15 open, 10
  suburban, 7 urban, 5 dense, none above the clutter; the same everywhere,
  not switched at the horizon; the models' own h / v = the TX antenna's),
  P.1546 location variability + Rician fading whose K follows the path
  per cell (lower behind terrain, towards Rayleigh where troposcatter
  takes over; continuous through the horizon) -> the margin a
  reliability needs. In both modes the panel labels each site's antenna
  and the target "TX antenna" / "RX antenna" by the roles. Validated against NTIA's ITM cases + C++, the ITU-R SG3
  P.1812 logs and P.2108's table, the coverage per model against the same
  models on its own ray profiles: `kraken_doa_v2/tools/pathloss_validate.mjs`.
  UI-only (kraken_doa.html "📶 PATH LOSS"; the engine lives in the viewshed
  worker); settings per browser (localStorage `kraken_pl`). DroneRanger
  (the user's reference) only has free space + a line-of-sight check
- Incident map: text messages from plugins (`kp::Host::message`, POCSAG /
  FLEX pages) -> street address found in the text -> looked up ONLINE in
  OpenStreetMap Nominatim (no downloaded map data) within `GEO_RADIUS_KM`
  of the station (Pagers tab, default 300, persisted) -> incidents (map
  markers + the Pagers tab's table; the same address on several VFOs = one
  incident listing them; kept 24 h, incidents.tsv). Code:
  `kraken_doa_v2/src/incidents.cpp`, `geo_address.cpp`, `geo_http.cpp`

**🗂 Decoder Logging (sidebar, all modes)** (details: `kraken_doa_v2/CLAUDE.md`
*Decoder data log*): what every VFO's decoder reports - events, text
messages, positions (throttled per object), raw frames (`kp::Host::raw`,
only while ticked: OPTION `log_raw`), incidents, voice transcripts - to
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

**🗣 Voice to Text (per VFO, all modes)** (details: `kraken_doa_v2/CLAUDE.md`
*Voice to Text*): what is said on the VFOs with "🗣 Transcribe" ticked,
written down LOCALLY by whisper.cpp (MIT, pinned v1.9.4; OpenAI Whisper
models + Silero VAD) - opt-in setup `cd kraken_doa_v2 && make whisper`
(= `transcribe/setup.sh`: builds `transcribe/build/kraken_whisper`, gets
base.en) or `WHISPER=1 ./install.sh`; Docker builds it in (`WHISPER=0`
leaves it out) with the models in `docker-data/whisper_models/`
- Source per VFO, not what the 🔊 speakers play: its digital decoder's voice when the
  decoder is on and decodes voice (P25 / DMR / D-STAR / NXDN / TETRA vocoder
  output via `dig::set_voice_tap_handler`, with the talker's unit ID /
  talkgroup; cut at talker changes), else - no decoder, or one of no voice
  (`DigitalDecoder::takes_voice()`: analog_id, pagers ...) - its analog demod
  (NBFM / AM / WBFM) of the same stream, cut by its squelch (squelch off: 10
  dB over the noise in the FFT), named after the decoder's talker if it has
  one (analog_id: "MDC 1029", "CTCSS 88.5"; `DigitalDecoder::lead_talker`). Bearing during
  each transmission in coherent mode with DoA on
- `src/voice2text.cpp`: one worker thread (taps, queue <= 40 / 10 min,
  helper process); the helper (`PluginProcess`, nice 10, one job at a time,
  wire `include/voice2text_wire.hpp`) runs VAD (noise / tones / carriers ->
  "no speech", no transcript), whisper, hallucination filters; a vocabulary
  prompt is sent as "Glossary: ... ." (a bare word list made whisper stop
  after one word)
- Cutting: a two-way transmission is one transcript up to 25 s (longer:
  parts, cut at its latest pause after 11 s); WBFM, or anything open longer
  than 30 s, in pieces from 8 s on at a pause (at the latest 15 s, then at
  the quietest 100 ms with 0.3 s overlap) - the text follows a broadcast live.
  A pause = 240 ms under a tenth of the median energy, or twice the noise
  floor on hissy audio
- Out: the 🗣 Voice tab under the waterfall (search, VFO filter, ▶ audio
  clip, CSV), `transcripts.jsonl` (cwd), decoder log type "voice", the
  audio as Ogg Opus 16 kbit/s in the DECODER LOG FOLDER
  (`<dir>/voice_clips/YYYY-MM-DD/<id>.opus`, deleted with Decoder Logging's
  Keep days, none written below 100 MB free, 2 GB backstop),
  `GET /voice_clip/<id>.opus` (audio/ogg, token like /recordings). Commands
  `VOICE2TEXT_*` (settings persisted before DECIMATORS; the per-VFO flag
  is DECIMATORS snapshot field 11), `VOICE2TEXT_DOWNLOAD:` = a catalogue
  model via curl
- Each transcript keeps its DoA LOBE: the MUSIC pseudospectrum averaged over
  the transmission (turned to north per frame by the heading of that moment;
  without a heading relative to the array) + where the station was, in
  transcripts.jsonl. Clicking the 🧭 bearing in its row
  (`VOICE2TEXT_LOBE:<id>`, the list itself only says `lb`) draws it on the
  🗺 map - pale colours of its own, dashed, never a VFO's live-lobe colour -
  with a tag whose ✕ removes it
- Pi 5: base.en ~280 MB RAM, ~2-3x faster than real time on 2-3 cores

**📡 Mobile DF on the map (coherent mode)** (details: `kraken_doa_v2/CLAUDE.md`
*Mobile direction finding*): each VFO's live DoA lobe (north frame, from
the station heading) + bearing on the 🗺 Map, and while driving a heat map
of the transmitter's position: `kraken_doa_v2/src/rdf_engine.cpp` (the
algorithm - an improved version of the KrakenSDR Android app's grid; offline
comparison `tools/rdf_sim.cpp`) + `src/rdf_mapper.cpp` (GPS track, MUSIC
frames time-aligned to it, distance gate, per-VFO solver, rdf_session.bin).
The heat map's grids are windows that SLIDE cell by cell (the coarse one
with the vehicle, the fine one with the estimate) - no ~7 km re-centring
jump near the edge any more (2026-10-21); `rdf_grid` carries each window's
centre / half-width / rotation, drawn by the page's `rdfGridDraw`.
`RDF:0|1`, `RDF_RANGE_KM:1-50` (persisted), `RDF_RESET:<vfo|-1>[:<talker>]`, `GET_RDF`;
pushes `{"rdf":..}` 2 Hz + `{"rdf_grid":..}` per changed VFO / talker. Right pane
tab "⊞ Both" shows the MUSIC DoA plots and the map at once

**⏯ Log replay (🗺 Map's "Replay", every mode, UI only)** (details:
`kraken_doa_v2/CLAUDE.md` *⏯ Log replay*): a DoA log of a drive - the
KrakenSDR Android app's (krakensdr_rdf_android: CSV in all its layouts, by
field count, or its SQLite `entries` table) or a 📝 DoA Recording of this
receiver (same formats; its box's 🗺 Replay) - opened in the browser (📂 /
dropped on the map, an SQLite `-wal` file with it), nothing sent to the
receiver. A worker (`lrWorkerMain` in kraken_doa.html) reads it - its own
SQLite reader (+ the WAL's committed pages), no library - and runs the
mobile DF engine PORTED from `src/rdf_engine.cpp` (keep the two alike:
`kraken_doa_v2/tools/replay_validate.mjs --cpp` + `tools/rdf_cmp.cpp`)
fed like rdf_mapper.cpp. The strip at the bottom of the map plays it back
(0.5-500x, scrub, step, click the track): track, vehicle, the moment's lobe
+ bearing, bearing lines, power dots, the heat map + 95 % ellipse as known
at that moment (snapshots along the drive), Follow centring the vehicle
every frame; long pauses shortened. A CSV a
spreadsheet re-saved (timestamps as 1.68843E+12 = rounded to 2.8 h, padded
rows, ';' + decimal commas, UTF-16) is read too: times rebuilt from the
track (`lrRetime`), "The drive took N min" corrects the pace

**📡 DoA per talker (P25 / DMR / NXDN unit IDs, D-STAR callsigns, ADS-B aircraft, AIS ships, DME beacons, ISM devices, APRS stations, radiosondes, TETRA cells, MPT1327 sites, VOR / ILS idents, pulse radars, 1030 MHz interrogators, UAT aircraft / ground stations, FLARM / OGN aircraft, analog FM radios - MDC1200 / FleetSync IDs, 5-tone selcall / DTMF, CTCSS tones / DCS codes, 406 MHz distress beacons, LoRa / Meshtastic nodes / LoRaWAN DevAddrs / gateways / off-plan LoRa transmitters, wildlife collars / tags, FM broadcast stations by their RDS PI)** (details:
`kraken_doa_v2/CLAUDE.md` *DoA per talker*). **ID separation is a core part
of every decoder** (the user's rule, 2026-10-15): if the signal carries a
coded ID per transmitter, the plugin reports it as a talker and is tested
with several transmitters at known bearings - `plugins/SDK.md` *ID
separation*, generators in `plugins/<id>/tools/`, `plugins/sdk/tools/
uca_scene.py` (scenes on the UCA), `decoder --file x.cf32 --channels 5
--uca 0.12` (each talker's bearing offline). APRS: per frame its RF
transmitter (source, or the digipeater marked "*"); radiosonde: type +
serial per frame; TETRA: the cell (MCC-MNC + colour code) per decoded
slot; MPT1327: the control channel's site (SYS); ILS / VOR: the Morse
ident (packets, `kp::Talker::packet`, each in its channel). Not applicable
(README says why): pagers (POCSAG / FLEX: the RIC / capcode = recipient), v23_telemetry, dtv (SFN
copies identical: by delay), iMet-4 (no serial). A decoder plugin that knows who
transmits reports it (`kp::Host::talker` / `talker_end`, `Info::talkers`,
with the frames' exact sample times; `kp::Talker::channel` for concurrent
transmissions - DMR timeslots; id "?" = a busy channel whose radio isn't
named yet). p25: LDU1 / TDULC link control source; dmr: full / embedded LC
source per timeslot; nxdn: VCALL source; dstar: MY callsign of the RF
header (or its slow-data copy); adsb: every accepted message as a
`kp::Talker::packet` (exact samples) - the VFO keeps per-16-sample
covariances for 1.5 s while packets come, each aircraft's lobe = its
packets of the last ~2 s, compared on the map with the bearing of its
ADS-B position (median = array / heading check); ais: every frame as a
packet on its channel (`kp::Talker::freq_hz` / `bw_hz`, averaged over
`avg_s` 20 s) - VFOs up to 500 kHz keep the raw samples instead and each
packet's channel is filtered out of every antenna, so a ship on the other
AIS channel at the same moment doesn't leak in; dme: clean reply pulse
pairs of each beacon (up to 100/s, 10 s average; a 📡 beacon in the DF
panel); ism: every decoded transmission of a device (model + id; the
latest <= 0.24 s of its rows) as a packet at the burst's own carrier /
FSK centre and occupied bandwidth, averaged per the plugin's "DoA per
device" option (each transmission alone by default - one bearing per
transmission in its history; a 📟 device in the DF panel, listed by name,
kept for the plugin's "Keep devices" time - 24 h by default -
`kp::Talker::keep_s`) - its VFO is 2.4 MHz wide, so kraken_doa keeps the
stream as 16-bit samples (2.5 s, 120 MB with 5 antennas - the plugin reports a
device up to ~2 s after it; 1 s left 8 of 19 without a bearing) to filter each
device's channel out (the whole band mixed overlapping devices' bearings).
The DF list sorts by any column heading (per kind, remembered per
browser); the 2 Hz status carries every talker only every 2.5 s
(`{"rdf"}`), the recently heard ones in between (`{"rdf_part"}`, merged by
the page - a separate key so an old page in a tab that reconnected after a
restart ignores it instead of showing it as the whole list).
TETRA: the downlink's base
station per cell (co-channel cells apart). The
VFO's decimated-stream position (`MultiChannelDecimated::stream_pos`) is the
common clock: MUSIC frames (`MUSICProcessor::setFrameTap`: each frame's own
covariance + its stream samples) that lie inside one talker's transmission
are summed per transmission and run through MUSIC again
(`spectrumFromCovariance`) - `kraken_doa_v2/src/talker_doa.cpp`. Every
radio gets its bearing, history and its own mobile DF heat map (rdf_mapper
keys VFO + talker, rdf_session.bin "KRDF2"); the 🗺 Map's 📡 DF panel picks
Whole signal / Every radio / one radio and lists the talkers

**📻 RDS / RBDS + SCA plugin** (details: `kraken_doa_v2/plugins/rds/README.md`,
`kraken_doa_v2/CLAUDE.md` *RDS / RBDS + SCA*): the plugin `rds` ("RDS / RBDS +
SCA (FM broadcast)", 2026-10-22) on a WBFM VFO (240 kHz for RDS, 300-400 kHz
for a 92 kHz SCA; the VFO's own rate, wider ones filtered to the channel):
every antenna combined before the FM discriminator in coherent mode
(`multichannel`, ~7 dB: RDS at 10 dB CNR with 5 antennas vs 18 dB with one),
RDS / RBDS groups (biphase BPSK demodulator, block sync + soft burst
correction, a corrected block's data only when confirmed twice): PI, RBDS
call sign, country (ECC), PS (dynamic flagged), PTY (RDS / RBDS names) +
PTYN, RadioText, RT+, eRT, Long PS, clock time, AF, TP / TA, DI, PIN, EON,
the ODA directory, TMC / DAB / EWS groups; RDS2 streams (66.5 / 71.25 / 76
kHz, block-synced); the pilot and every MPX subcarrier 20-99 kHz probed - SCA
FM audio (tones told apart from FSK), FSK data with its bit rate, DARC-like,
AM; Listen = an SCA's audio (150 us) or the main programme in mono,
"🔊 Listening" again = back to the VFO's own WBFM (the bandwidth kept); a talker
packet "PI xxxx" per RDS group (UI kind `broadcast` 🎙). Generator
`plugins/rds/tools/rds_gen.py`, playback `rds-two-stations`. Checked on a
real US station's recording (pysdr.org: PI, RT, dynamic PS, a real 67 kHz
SCA) and end to end through kraken_doa's debug playback (two stations at
60.0 / 200.0 deg)

**📟 Pagers plugin (POCSAG + FLEX)** (details:
`kraken_doa_v2/plugins/pagers/README.md`, `kraken_doa_v2/CLAUDE.md`
*Pagers*): the plugin `pagers` ("Pagers (POCSAG + FLEX)", was `pocsag` -
renamed 2026-10-23 when FLEX was added, the user's request; saved
`PLUGIN:pocsag` VFOs / `pocsag.*` options / Auto detect entries load as
pagers, `dig::renamed_plugin`) decodes POCSAG (512 / 1200 / 2400 bit/s, as
before - byte for byte the old plugin's pages, events now start "POCSAG ")
and Motorola FLEX (1600/2, 3200/2, 3200/4, 6400/4, any frame) on one FM
front end, no setting. FLEX: short + long capcodes (the MC68175 data
sheet's conversions), alphanumeric / numeric / tone only / short message /
binary / secure pages, fragments put together, group calls (temporary
addresses: the message names the pagers told to listen), date / time;
every check of the format (4-bit word checks, fragment / numeric checks,
signature). Written from Motorola's MC68175 data sheet + the FLEX patent's
sync patterns (Jelmer Bruijn's write-up) - no other decoder's code;
conventions (bit order, polarity: 1 = lower frequency, 4-level mapping)
confirmed on a real 6400/4 recording (sigidwiki, 39 pages, every check
right). Receiver: sync 1 hunted every quarter bit on the smoothed
discriminator (64 bits; no clock loop - it lost frames after a key-up / an
idle frame), the next frame looked for where the last one's grid ends
(frames are back to back), data on the sync's symbol grid with the timing
followed per block from level changes only, damaged-block rules so no page
lands on a wrong capcode. Generator `plugins/pagers/tools/flex_gen.py`,
playback `flex-four-speeds`; cross-checked with multimon-ng (GPL, scratch
only) both ways

**📶 Digital TV plugin** (details: `kraken_doa_v2/plugins/dtv/README.md`,
`kraken_doa_v2/CLAUDE.md` *Digital TV*): the plugin `dtv` ("Digital TV
(DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB)", was `sfn` "SFN Separation" - DAB
removed, to become a plugin of its own; saved `PLUGIN:sfn` VFOs load as
dtv; picked on a 2.4 MHz VFO like any decoder) reads what a broadcast TV
signal says about itself in the slice of the channel the VFO sees: standard
+ mode, channel (measured centre + raster number), DVB-T TPS (BCH-checked:
constellation, code rate, guard, mode, cell ID, net bit rate), ISDB-T TMCC
(layers, partial reception, the emergency warning flag -> a ⚠ event) and
the echo profile of the channel impulse response, measured against the
signal's known reference - DVB-T / DVB-T2 / ISDB-T / ATSC 3.0 scattered
pilots (generic engine `plugins/dtv/ofdm.cpp`), the ATSC 1.0 field sync,
DTMB PN headers. **Path separation** (coherent mode; OFF by default - an on
/ off button in the decoder tab, option `separate`; any plugin option with
choices exactly 0 / 1 is such a button; was "SFN separation" - renamed
because a reflection and another transmitter of a single frequency network
are indistinguishable copies of the signal, and on the real Auckland muxes
every extra path was a reflection): on, every antenna's CIR is analysed,
paths = peaks of the per-tap spatial covariance, tracked; each one's
covariance goes back as a `kp::Spatial` and kraken_doa runs MUSIC with one
source on it: a talker "Path-n" with its own DoA lobe / heat map on the 🗺
Map's 📡 DF panel, picked there, in the decoder tab's delay-profile plot
(`kp::Plot`) or table; delays from the main (strongest) path, the table's
"From first" column the delay after the first arrival (off: antenna 0
only, the echo profile without bearings, ~1/3 of the CPU). ARRAY plugin
API (`kp::Info::array`, `process_array`, bursts of 0.35 s every second,
`Host::spatial`, `Host::plot`): `plugins/SDK.md`. Synthetic generator
`plugins/dtv/tools/sfn_gen.cpp` (real TPS / TMCC); offline `decoder --file
x.cf32 [--opt separate=1 --uca R]`. Real Auckland DVB-T: TPS + cell IDs
on all six muxes. The SFN part replaced an earlier in-process blind
separation (commit 7f3535a)

**📱 LTE / 5G NR Cell Decoder plugin** (details:
`kraken_doa_v2/plugins/cells/README.md`, `kraken_doa_v2/CLAUDE.md` *LTE / 5G
NR Cell Decoder*): the plugin `cells` ("LTE / 5G NR Cell Decoder", was "Cell
Separation"; a 2.4 MHz VFO within ~0.5 MHz of the LTE carrier centre / ~0.2
MHz of the NR SS/PBCH block) reads the public broadcast information of every
cell on the carrier - LTE MIB (PBCH), PCFICH, the SI-RNTI DCI (PDCCH common
search space, formats 1A / 1C), SIB1 and SI messages (PDSCH, turbo, soft
combining of repeats in the circular buffer), RRC UPER (SIB1, SIB2 - SIB5):
operator MCC-MNC, TAC, cell identity / eNB, band, EARFCN, bandwidth, uplink,
neighbours ... - shown as table columns + per-row details
(`kp::Host::table_detail`, wire TABLE_DETAIL 38: a ▸ in the decoder tab's
table opens a row's label / value lines). Only broadcast system information,
nothing addressed to a phone. A carrier wider than the VFO is read in part
(SIB1 / SIB2 of a 10 MHz carrier in seconds; an SI message sent once per
window with its PRBs outside the VFO stays unread). And each cell gets its
own bearing - an LTE / NR carrier carries many cells at once (frequency
reuse 1); each scrambles its signals with its physical cell ID, so it
separates them by CODE (dtv's path separation: by delay): carrier on the LTE 100 kHz raster /
NR GSCN (+ cyclic-prefix fine offset, 0 / +-1 subcarrier sweep, then
tracked from the cells - the "Frequency offset" fact calibrates the
receiver), PSS fold -> SSS (joint 3-root PSS channel, magnitude score,
robust z, successive cancellation incl. co-timed cells of other N_ID2) ->
CRS (LTE ports 0 / 1) / SSS (NR, every beam) impulse response per antenna
-> per-tap covariance minus the floor (noise + other cells) -> a
`kp::Spatial` talker "LTE-<pci>" / "NR-<pci>" (📱 kind 'cell' in the DF
panel, strongest first). Not near a carrier: it retunes the receiver
itself (`scan` option, +-10 MHz in 1.2 MHz steps, `kp::Host::tune` -> wire
TUNE -> `ControlHandler::request_plugin_tune` -> FREQ / TUNER_FREQ on the
uWS loop; only plugins declaring `kp::Info::tunes`) and centres the carrier.
Its VFO (and the dtv plugin's) is `kp::Info::lock_vfo`: locked at the
tuner centre like a fixed-frequency decoder, moved by tuning the receiver.
Generator `plugins/cells/tools/cell_gen.cpp` (OFDM built at 2.4 MHz
directly). Live: band-8 cells on 954.8 MHz (10 MHz carrier) - MIB, SIB1
(530-01, TAC, eNB / cell) and SIB2 decoded from the ~23 % the VFO sees

**📡 Pulse radar plugin** (details: `kraken_doa_v2/plugins/radar/README.md`,
`kraken_doa_v2/CLAUDE.md` *Pulse radar*): the plugin `radar` ("Pulse radar
(L-band ATC / surveillance)", 2.4 MHz, `lock_vfo`, manual only) measures
every pulsed radar in the band like an ELINT receiver: pulses (width,
frequency, linear FM chirp, level, ADC clipping; a radar's pulse group -
frequency diversity, fill pulse + chirp - linked by its fixed delay), PRF
and PRI pattern (constant / stagger / blocks / jitter, the PRI sets it
switches between), antenna scan period and -3 dB beam width from the main
beam sweeping past. Weak chirps fading into pieces, pulses merged with
their echo, echoes and the RTL's ghost images are folded into their radar
(on the real recordings one radar first came out as 9-15). ID separation by
SIGNATURE (no coded ID): talker id = frequency to 0.1 MHz, packets = only
its unclipped main-beam pulses (within 15 dB of the scan's peak, in a burst
that dwells near the peak like the main beam - a reflector lit elsewhere in
the turn stays out), each with its own channel -> 📡 kind `radar` in the DF
panel. Generator `plugins/radar/tools/radar_gen.py` (radars at known
bearings, every antenna, cu8 / cf32). Live: the 1355.0 MHz L-band radar
here - 59 us LFM -1.17 MHz, PRI 1712 / 1395 us in blocks of 8 alternating
per turn with 1748 / 1424, 12.3 rpm, 1.5 deg beam, its short pulse on
1350.07 MHz, a steady bearing; plugin 2-3 % of a core

**🛰 SSR / Mode S / TCAS interrogations (1030 MHz)** (details:
`kraken_doa_v2/plugins/ssr1030/README.md`, `kraken_doa_v2/CLAUDE.md`
*1030 MHz interrogations*): the plugin `ssr1030` (2.4 MHz,
`fixed_freq_hz` 1030 MHz like adsb, manual only) reads the UPLINK the 1090
MHz replies answer: Mode A / C (P2 SLS level, P4 short / long all-calls,
S1 whisper-shout) and Mode S - the 4 Mchip/s DPSK bits at 0.6 samples per
chip by a sequence estimator against the receiver's measured complex step
response (`s_default.h`, re-fitted from parity-checked frames): UF0 / 4 / 5
/ 11 / 16 / 20 / 21 / 24, target addresses (uplink AP = CRC xor the
ENCODED address - coefficients x^47..x^24 of A(x)G(x), confirmed live),
II / SI codes, ACAS addresses (UF16 MU); all-calls / broadcasts checked +
corrected against the known address. ID separation: SSR = a periodic
all-call train (sequence search; sidelobes by the omni P2's level; named
"SSR II 10" from its checked UF11), Fixed = a non-rotating interrogator at
one steady level (multilateration), ACAS = whisper-shout / checked UF16
broadcasts ("ACAS 7C4A51"); packets = an SSR's main-beam interrogations,
the others' all -> DF kind `interrogator` (🛰 / ✈). PRF / PRI, interlace,
scan, beam, SLS per interrogator via `plugins/lib/radar_pri` /
`radar_scan` (moved there from the radar plugin, shared). Generator
`plugins/ssr1030/tools/ssr_gen.py`. Live: SSR II 10 (199 Hz, A / C + UF11
every 3rd, 5.00 s scan, 2.6 deg beam), the airport multilateration
interrogator (1 s bursts, UF4 / 5 to the aircraft adsb hears), TCAS
aircraft by their broadcasts; plugin ~4 % of a core. Mode S bits need a
response close to the KrakenSDR's (a different synthetic receiver: none
decoded); coexists with dme (6X interrogations on 1030 MHz)

**🧪 Debug playback (sidebar "🧪 Debug Playback")** (details:
`kraken_doa_v2/CLAUDE.md` *Debug playback*): a decoder's synthetic
multi-antenna test transmission (known IDs + bearings) played through the
real pipeline in place of heimdall's 8091 stream - spectrum, VFO, decoder,
DoA, 📡 DF panel, a Check of the bearings (talkers; for decoders without
transmitter IDs - pagers, v23_telemetry - the VFO's own DoA with the
Digital squelch, and decoded events). Catalogue per plugin in
`plugins/<id>/tools/testsignals.json` (how to add one: plugins/SDK.md),
generated on demand by `plugins/sdk/tools/make_testsignal.py` into
`testsignal_cache/` (cwd-relative, gitignored, <= 3 GB; narrow scenes
stored at their own rate and interpolated + noised live, 2.4 MHz ones as
cu8 loops). While it plays: nothing reaches heimdall
(`DataReceiver::send_control_command` drops it), tuning / scanners
refused, the settings store is a sandbox (Stop - or a restart - brings the
user's settings back; doa_settings.json / heimdall_settings.conf
untouched), web mapper / decoder log / Voice to Text / incidents / DoA
recording paused, a red "DEBUG PLAYBACK - not live" banner + spectrum
overlay. A playback never outruns the pipeline (a busy machine plays it
slower than real time instead of dropping packets). 37 entries in 26
catalogues - every decoder has its own (p25, dmr, radiosonde, then rds,
then analog_id's `analog-ids-six`, then pagers' `flex-four-speeds` last)

**📼 IQ & audio recorder + playback of recordings (sidebar "📼 IQ & Audio
Recorder")** (details: `kraken_doa_v2/CLAUDE.md` *📼 IQ & audio recorder*,
format: `kraken_doa_v2/SIGMF.md`): `src/recorder.cpp`, `recorder_audio.cpp`,
`sigmf.cpp`, playback in `playback.cpp`
- IQ (coherent): every antenna as a SigMF COLLECTION - one Recording per
  antenna + `<name>.sigmf-collection` in the folder `<name>/` (`spatial`
  extension for the geometry / calibration bursts, `krakensdr` namespace for
  the array settings, VFOs, band, station + GPS track); full band = the
  8091 bytes (cu8 2.4 MHz, 24 MB/s with 5 antennas) or one VFO's band (ci16
  at its rate). Transport = one SigMF Archive `<name>.sigmf` (tar) made on
  the fly by the download / taken by the upload, played in place
- Audio (any mode, also during a playback): a WAV (48 / 16 / 8 kHz) or Ogg
  Opus file per VFO - its own demod (the live audio's demodulator) or, with
  demod Digital, its decoder's voice (new record tap); squelch closed =
  silence or left out
- Limits: 10 s .. 24 h or Unlimited; refused when the projected size doesn't
  fit (a reserve of max(512 MiB, 2 %) stays free; FAT32 4 GiB per file),
  stopped at the reserve. A drive slower than the data (this Pi's SD card:
  ~10 MB/s) buffers in memory (<= 40 % of MemAvailable) and writes after the
  end; with its speed known ("Test write speed" / an earlier recording) a
  length the buffer can't cover is refused
- Open = playback through the real pipeline (the 🧪 debug playback's
  sandbox: heimdall disconnected, nothing saved / sent out) with pause, seek,
  loop, the recording's array settings, its VFOs if asked, the station where
  it was made (`StationInfo::setPlaybackOverride`, along its GPS track);
  foreign SigMF too (1-8 channels, complex u/i/f types, 2.4 MHz / N)
- "Local Recording" (DoA logger) is called "📝 DoA Recording" in the UI now
- Files only of recordings are listed / served / deleted (the folder setting
  can point anywhere); `RECORDER_*` commands, `GET /recording/<name>[/<file>]`,
  `POST /recording_upload/<name>.sigmf`

**🔇 Digital squelch (per VFO, with its digital decoder on)** (details:
`kraken_doa_v2/CLAUDE.md` *Digital squelch*): squelch methods are FFT Peak
and Digital (the eigenvalue methods were removed - saved values map to
FFT). Digital opens while the decoder reports valid frames (+1.5 s, or the
plugin's `kp::Info::squelch_hold_s` - analog_id 0.35 s) and the
VFO's DoA is published from exactly the confirmed samples
(`TalkerDoa::set_digital` -> `MUSICProcessor::publishFromCovariance`; MUSIC's
own frames held) - interference / noise between the frames never reaches
the bearing. Plugins report frames with their samples via
`kp::Host::valid(start_s, end_s)` (all shipped decoders do; the lib ones via
`RxContext::valid_at` / `dig::report_valid`); talker spans and packets count
too

**📈 Spectrum noise floor (the 0 dB of display, FFT squelch, scanner)**
(details: `kraken_doa_v2/CLAUDE.md` *Noise floor*):
`kraken_doa_v2/src/signal_processing/noise_floor.cpp`. Per tuner channel the
floor is the frame's 10th percentile as before, unless a signal fills the
band (DVB-T, LTE - wider than the 2.4 MHz: the percentile landed on it, it was
drawn at ~0 dB and an FFT squelch inside it never opened): then the
receiver's own floor, remembered per tuner + gain in `noise_floors.json`
(cwd, gitignored). Coherent mode tells a band-filling signal from a real
noise rise by two antennas' coherence (receiver noise is independent per
antenna); independent / wideband scan mode: > 6 dB above = a signal, the
remembered floor creeping up 1 dB / min while held. Wideband scan mode
(the stitched spectrum) uses no remembered floors: the tuners' common
response shape (measured on the KrakenSDR, learned on) is taken off, the gain
steps between neighbours come from the 0.48 MHz their bands share (kept per
pair of dongles through retunes), and a tuner whose floor stands over the
quietest one's (gains taken off) is filled by a signal (DVB-T / LTE over
several tuners) and blends to that floor + its gain (1 -> 3 dB over, no
switching) - each tuner used to be drawn against its own percentile, so such
a signal dropped to ~0 dB past the boundary (fixed 2026-10-21; a first version
misjudged real hardware and dropped the signal during drags, see the client
CLAUDE.md). A retuned tuner's average is shifted with the move, so a drag
doesn't draw the old band moved along

**✈ Array calibration from ADS-B aircraft (coherent mode)** (details:
`kraken_doa_v2/CLAUDE.md` *Array calibration*): `kraken_doa_v2/src/array_cal.cpp`.
Aircraft packets with the position the adsb plugin gives them
(`kp::Talker::lat / lon / alt_m`, moved on to the packet's time) are
transmitters at known directions; a fit finds each element's cable delay and
position error and the array's rotation - physical units, so MUSIC and the
beamformer use them on every frequency (`array_cal::corrections()` in their
steering). Cross-checked on aircraft left out of the fit; applied by the user
(sidebar "🧪 Experimental" -> "✈ Array Calibration", `ARRAY_CAL_*`), saved in
`array_cal.json` (cwd), used only while the array settings match. Needs a
static location (or GPS + compass) and the array standing still.
EXPERIMENTAL / on the back burner: on the live array the aircraft's
responses were too distorted (multipath / low elevation) for a good fit -
see the client CLAUDE.md

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
  Web Mapper, DoA Recording, continuous scanner, CH selector) are hidden
  outside coherent; the discrete scanner section only shows in wideband
- The assistant's IQ captures (`capture_iq`, done inside kraken_doa -
  `ai_capture.cpp`) record the tuner whose band holds the signal outside
  coherent mode
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

**Drag retunes**: coherent and wideband scan retunes close together (a drag,
within 400 ms - up to 1 s while the previous step isn't finished; wideband
scan since 2026-10-21: 114 -> 74 ms per step on Windows) use the librtlsdr fork's
`rtlsdr_set_center_freq_fast` (PLL autotune clock left at 128 kHz between
steps, lock still read every step); the coherence watchdog (400 ms quiet)
and every noise-source switch-on (`set_bias_tee_all_devices(true)`, which
every calibration goes through) finish them with `rtlsdr_finish_tune`.
8092 drag step, 5 tuners (2026-10-19): Windows (one by one) 114 -> 74 ms
(fast path off / on); Raspberry Pi 5 (parallel threads) 19.0 -> 14.4 ms
(00606c0: 48.1), since the noise-source off write runs in parallel too
(5 dongles 18.3 -> 3.8 ms; before: 33.4 -> 28.7, 00606c0 62.5), see
heimdall_v2/CLAUDE.md *Drag retunes*

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

**Architecture** (`kraken_doa_v2/src/signal_processing/shared_decimator.cpp`;
kraken_doa_v2/CLAUDE.md *Arbitrary VFO bandwidth*):
- Per VFO a `SharedDecimator`, per antenna its own mixer + stage state;
  every VFO's block in ONE pass per antenna (`SharedDecimator::decimateVfos`,
  called once per packet by data_receiver.cpp): a task per antenna (per
  tuner in independent mode) runs each VFO's cascade on a 1024-sample chunk
  while it is in L1, the first task on the calling thread, the others on
  ONE decimation pool shared by every VFO (cores + 1 threads; it was 8
  threads per VFO: 51 -> 32 threads with three VFOs). Bit for bit what each
  VFO alone gives; three narrow VFOs with DoA ~4 % of a core less, one VFO
  the same; MUSIC etc. per VFO after the pass
- Integer decimation factors (no resampling), split into stages of 2..8 by
  `planStages` (a cost model of the kernels; halfband stages, the mixer
  folded into the first stage where cheaper)
- Block FIR stages (`FirStage`): every output straight from the stage's
  buffer, 4 outputs per NEON / SSE pass; buffers grow without zeroing; a
  block goes through the cascade in 1024-sample chunks (`DECIM_CHUNK`), so
  the buffers stay in L1 (the whole 128 KB block per antenna made the
  first stage memory-bound); a first stage whose input isn't mixed in front
  filters straight from the input block (`FirStage::begin_input`)

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
- heimdall's `make` clones/builds just the static library when it's missing,
  and fast-forwards a clone older than 578b43d (the fast drag calls + the
  stalled control OUT retry; else it stops - `librtlsdr-fork.md`);
  `install.sh` builds the fork and installs it to `/usr/local` (library,
  headers, udev rules) with its `rtl_*` tools linked statically too
  (`-DLINK_RTLTOOLS_AGAINST_STATIC_LIB=ON`)
- Do NOT install `librtlsdr-dev` for heimdall (distro `librtlsdr0` /
  `rtl-sdr` packages may stay installed for other software)

**Required System Libraries**:
- `libusb-1.0-0-dev`, `cmake`: build and link the librtlsdr fork
- (no FFT library: pffft is vendored in `heimdall_v2/external/pffft` - the
  same files as kraken_doa_v2's; FFTW was dropped 2026-10-17, see
  heimdall_v2/CLAUDE.md *FFT (pffft)*. No program of the suite links FFTW;
  only gr-krakensdr_v2 still uses it - GNU Radio needs it anyway)
- `libeigen3-dev`: Eigenvalue decomposition for phase calibration
- `libssl-dev`: SSL/TLS for uWebSockets
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies**:
- `uWebSockets/`: HTTP/WebSocket server (auto-installed by Makefile, pinned -
  see below)

### DoA Client

**Required System Libraries**:
- (no FFT library: pffft is vendored in `kraken_doa_v2/external/pffft`; no
  `libliquid-dev`: liquid-dsp is built from source, below)
- `libeigen3-dev`: MUSIC DoA algorithm
- `libssl-dev`: SSL for WebSocket server
- `libopus-dev`, `zlib1g-dev`, `libsqlite3-dev`: audio, compression, settings
- `build-essential`, `cmake`, `git`, `pkg-config`: Build tools (cmake + git
  for liquid-dsp)

**Vendored Dependencies**:
- `uWebSockets/`: WebSocket server (auto-installed by Makefile)
- `external/pffft/`: the FFT (pffft, FFTPACK / BSD-style licence, pinned
  commit in its README.md) - kraken_doa's and the plugins' through
  `plugins/sdk/kraken_fft.hpp`; replaced FFTW (GPL) 2026-10-17. Built with
  `-DPFFFT_ENABLE_NEON` on ARM (the Makefiles add it). Details:
  kraken_doa_v2/CLAUDE.md *FFT (pffft)*
- `liquid-dsp/` (gitignored, like `uWebSockets/`): liquid-dsp (MIT) fetched
  at a pinned commit (`LIQUID_COMMIT` in `kraken_doa_v2/liquid.mk` = v1.7.0,
  Debian 13's version) and built by its own CMake as a STATIC library without
  FFTW / libfec - Debian's libliquid1 links FFTW (GPL) + libfec (LGPL), so
  every program on it loaded FFTW. One recipe for the Makefile, plugins/
  Makefile, CMake and the Dockerfile; `ldd kraken_doa` / any plugin shows no
  liquid, FFTW or libfec. Details: kraken_doa_v2/CLAUDE.md *liquid-dsp*

**uWebSockets is PINNED** (`UWS_COMMIT` in both Makefiles = `UWS_COMMIT` in
the Dockerfile, fe7da4cb, 2026-07-31; keep all three equal): uWebSockets
master since ~Oct 2026 adds `src/HttpCache.h`, which `App.h` includes and
which defines `uWS::time_ms()` in the header without `inline` - every
object including App.h gets a copy and the link fails with "multiple
definition of `uWS::time_ms()'" (seen on a fresh install). `check-uws`
clones that commit (only the uSockets submodule - `--recursive` pulled
hundreds of MB of fuzz corpora), and a Makefile-parse-time check switches
an existing checkout on any other commit back to it, rebuilds uSockets and
drops the objects built against the other headers (kraken: obj/ dep/;
heimdall: build/<module>/), so re-running install.sh repairs such a
machine. Override for testing: `make UWS_COMMIT=<sha>`

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

## Performance Notes

### ARM NEON Optimizations

Both applications automatically detect ARM architecture and enable NEON optimizations:

**Heimdall Server**:
- ARM NEON vectorization for sample processing
- Automatic fallback to scalar on x86_64
- Built for the CPU it is built on (`-mcpu=native`; was the Pi 4's
  Cortex-A72) - see `compiler-flags.md`

**DoA Client**:
- IQ conversion: Compiler auto-vectorization preferred over manual NEON
- Decimation: the VFO decimator's own block FIR kernels (GCC vector
  extensions -> NEON); liquid-dsp (built from source - `liquid.mk`, NEON dot
  products) designs its taps and runs the demodulators' / plugins' filters
- FFT: pffft (NEON), no planning / wisdom file - see kraken_doa_v2/CLAUDE.md
  *FFT (pffft)*; power -> dB per bin with `fast_db10` (`include/utils/
  fast_log.hpp`) in straight loops that vectorize (the old wrapped-index
  loop called libm's log2f per bin: ~2.6 % of a core per spectrum)
- MUSIC / beamforming / talker DoA (2026-10-26): plain loops with the
  element count as a template parameter, vectorized by the compiler - the
  frame covariance straight from a complex<float> accumulator, the
  noise-subspace projection over cached planar steering vectors, the
  beamformer's combine loop, talker_doa's rings; MUSIC ~7x less CPU, same
  bearings (kraken_doa_v2/docs/dev/reference.md *MUSIC Algorithm*)

### Compiler flags

See `compiler-flags.md`.

### Memory Management

**Server**:
- L1 buffers: Per-device circular buffers
- L2 buffer: Global synchronized queue (moodycamel::ConcurrentQueue)
- FFT pool: thread-safe pool of pffft plans (RAII); forward transforms in place, pffft's scratch one per thread

**Client**:
- Raw data buffer: Bounded queue with automatic cleanup
- Ring buffers: Lock-free for FFT data
- Malloc arenas: Increased to 32 for multi-threaded performance (`mallopt(M_ARENA_MAX, 32)`)

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
- **8079**: the AI assistant's MCP server, 127.0.0.1 only, bearer token per
  run (`AI_MCP_PORT`, `KRAKEN_AI_MCP_PORT` overrides it)
- Connects to server ports 8091 (data) and 8092 (control)

