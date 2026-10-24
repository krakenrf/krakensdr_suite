# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Heimdall is a C++20 real-time direction-finding system using multiple RTL-SDR devices for phase-coherent signal processing. It performs cross-correlation, phase/lag compensation, and provides web-based visualization through multiple streaming interfaces.

## Build Commands

### Quick Start
```bash
make              # Build (auto-installs uWebSockets)
make run          # Build and run
```

### Common Build Commands
```bash
make deps         # Install system dependencies (Ubuntu/Debian)
make deps-fedora  # Install dependencies (Fedora/RHEL)
make deps-arch    # Install dependencies (Arch)
make status       # Check installed dependencies and module status
make debug        # Show build configuration
make clean        # Remove build artifacts
make rebuild      # Clean and full rebuild
make distclean    # Remove everything including uWebSockets
```

### CMake Alternative
```bash
# First install uWebSockets (`make check-uws` does exactly this): the PINNED
# commit (UWS_COMMIT in the Makefile) - newer master defines uWS::time_ms() in
# HttpCache.h without `inline`, which breaks the link
git clone https://github.com/uNetworking/uWebSockets
git -C uWebSockets checkout fe7da4cb05622b8d004718ec3ca05101782eb1c2
git -C uWebSockets submodule update --init uSockets
cd uWebSockets/uSockets && WITH_SSL=0 make && cd ../..

# The librtlsdr fork's static library must exist first (../install.sh builds
# it, or: make ../librtlsdr/build/src/librtlsdr.a)

# Then build
mkdir build && cd build
cmake ..
make -j3          # never more than 3 jobs on the Pi
./heimdall
```

## Architecture

### Modular Structure

The codebase is organized into 5 independent modules with clear dependency hierarchy:

```
main.cpp (orchestration)
├── web/        → core/, dsp/, sdr/, net/
├── net/        → core/, sdr/
├── dsp/        → core/, sdr/
├── sdr/        → core/
└── core/       (no dependencies)
```

**Module Files:**
- `src/core/`: types.hpp, config.hpp, logging.hpp, utils.{hpp,cpp}
- `src/sdr/`: sdr_device.{hpp,cpp}, sdr_init.{hpp,cpp}, sdr_pipeline.{hpp,cpp}
- `src/dsp/`: fft_plan.{hpp,cpp}, correlation.{hpp,cpp}, compensation.{hpp,cpp}
- `src/net/`: tcp_common.{hpp,cpp}, tcp_data_server.{hpp,cpp}, tcp_control_server.{hpp,cpp}, rtl_tcp_server.{hpp,cpp}
- `src/web/`: html_loader.{hpp,cpp}, web_server.{hpp,cpp}

### Core Module (`src/core/`)

Provides fundamental types and utilities shared across all modules.

**Key Files:**
- `config.hpp`: Wraps config.h constants, includes NUM_DEVICES, ports
- `types.hpp`: Type aliases (Complex, ComplexBuffer), enums (LagCompensatorState, PhaseCompensatorState), structs (CorrelationResult, PhaseCompensationData, ChannelCompensation)
- `utils.{hpp,cpp}`: Endian conversion, clamping, IQ LUT, `set_thread_realtime()` (best-effort SCHED_RR)
- `buffer_pool.hpp`: Lock-free buffer pool (`acquire`/`try_acquire`/`release`) used for the alloc-free callback and L2/L2-raw recycling
- `logging.hpp`: LOG_I(), LOG_W(), LOG_E() macros

**Important Types:**
- `CorrelationResult`: Correlation data, lags, phases, compensation state
- `PhaseCompensationData`: Phase compensation vectors and convergence tracking
- `ChannelCompensation`: Per-channel lag compensation state machine
- `FFTProcessingControl`: FFT enable/disable control with atomic flags

### SDR Module (`src/sdr/`)

Manages RTL-SDR device lifecycle and sample streaming.

**Key Files:**
- `sdr_device.{hpp,cpp}`: SDRDevice class with L1 buffers (per-device) + the alloc-free RTL callback
- `sdr_init.{hpp,cpp}`: Serial number-based device enumeration and initialization
- `sdr_pipeline.{hpp,cpp}`: Sample drain (`sample_processor`), conversion worker (`conversion_worker`), phase/lag/EQ compensation application, buffer flush helpers, and `signal_coherence_lost()`
- `pipeline_control.{hpp,cpp}`: Lifecycle of the device-touching threads (USB readers, drain, conversion, lag compensation) behind `pipeline_running`, plus `reconfigure_num_elements()` — the runtime element-count change (stop pipeline → close all handles → reopen first N → restart → full recal via `force_recalibration`; rolls back on open failure). The `devices` vector is STRUCTURALLY IMMUTABLE after startup: one SDRDevice object per expected serial for the life of the process, inactive channels keep `dev == nullptr`. When changing the element count, per-channel `correlation_result` maps MUST be cleared+reinitialized under `data_mutex` (stale higher-channel entries overflow the correlation message's `num_elements*4` status array — heap corruption)

**Important Concepts:**
- **L1 Buffers**: Per-device lock-free queues (`devices[i]->l1_buffer`) holding raw byte buffers from the callback
- **L2-raw Buffer**: Staging queue (`l2_raw_buffer`) of aligned *raw* sample sets handed from the drain to the conversion worker (inner buffers owned by per-device `sample_pool`s; recycled via `recycle_raw_set`)
- **L2 Buffer**: Global queue (`l2_buffer`) of aligned *converted* (compensated) sample sets consumed by correlation
- **Coherence signalling**: `signal_coherence_lost()` latches the `coherence_lost` flag; the coherence watchdog (dsp/compensation.cpp) performs recovery
- **Serial Number Mapping**: Uses DeviceMapping structs to match serial numbers to physical device IDs (config.h DEVICE_MAP)

### DSP Module (`src/dsp/`)

Signal processing: FFT, cross-correlation, and compensation algorithms.

**Key Files:**
- `fft_plan.{hpp,cpp}`: RAII FFT plan (`FftPlan`: forward, cross_correlate, inverse_transform at `CORRELATION_SIZE`; pffft - see *FFT (pffft)*) and the thread-safe plan pool (`FftPool`)
- `correlation.{hpp,cpp}`: Cross-correlation processing, Gaussian interpolation for peak detection, correlation message building
- `compensation.{hpp,cpp}`: Phase and lag compensation state machines

**Important Concepts:**
- **Eigenvalue Decomposition**: Uses Eigen3 for phase calibration (more robust than correlation peaks)
- **Lag Compensation**: closed-loop proportional sample-clock servo, MEASURING → SERVOING → CONVERGED
- **Phase Compensation State Machine**: WAITING_FOR_LAG_COMPLETION → MEASURING_INITIAL_PHASE → APPLYING_COMPENSATION → VERIFYING_CONVERGENCE → CONVERGED
- `process_correlations()` is the main DSP entry point, called by correlation processor thread
- Gaussian interpolation provides sub-sample peak accuracy

### Net Module (`src/net/`)

TCP servers for remote control and IQ data streaming.

**Key Files:**
- `tcp_common.{hpp,cpp}`: Shared TCP utilities and client management
- `tcp_data_server.{hpp,cpp}`: Multi-channel IQ streaming on port 8091 (for FFT viewers)
- `tcp_control_server.{hpp,cpp}`: JSON command interface on port 8092
- `rtl_tcp_server.{hpp,cpp}`: RTL-TCP compatible server on port 1234 with selectable source channel

**Important Concepts:**
- **TCP Data Server**: Streams all channels simultaneously for external FFT analysis
- **TCP Control Server**: Accepts JSON commands, broadcasts status at 2 Hz
- **RTL-TCP Server**: Compatible with rtl_tcp clients (SDR#, GQRX), source channel selectable via web UI
- All servers run in separate threads and use atomic variables for thread-safe config access

### Web Module (`src/web/`)

uWebSockets-based web interface for visualization and control.

**Key Files:**
- `html_loader.{hpp,cpp}`: HTML file loading with template variable substitution
- `web_server.{hpp,cpp}`: uWebSockets HTTP server and WebSocket interface on port 8070
- `index.html` (project root, NOT under `src/web/`): Complete web UI, read from the working directory at startup by `html_loader.cpp`

**Important Concepts:**
- Uses uWebSockets (not standard POSIX sockets) for high performance
- WebSocket broadcasts correlation results, FFT data, and status updates
- Web UI provides frequency/gain control, bias-tee toggle, RTL-TCP channel selection

## Configuration

All runtime configuration is in `config.h` at the project root:

**Key Settings:**
- `NUM_DEVICES`: compile-time CEILING on device count (default 8). The runtime
  count is `active_num_elements`: `-n` flag > persisted `num_elements` setting
  > count of expected serials attached on USB (`--serials s0,s1,...`, default
  "1000".."1007" — Kraken convention extended to the ceiling, so a stock
  KrakenSDR auto-detects 5 and a KerberosSDR 4). Change
  at runtime via the web UI "Array Elements" selector / `NUM_ELEMENTS:<n>` WS
  command / `set_num_elements` control command (devices reopen + full recal;
  see `src/sdr/pipeline_control.cpp`). Unopened dongles stay free on USB for
  other programs
- `REF_CHANNEL`: Reference channel index (default 0)
- `CENTER_FREQ`: RF center frequency in Hz (default 100 MHz)
- `SAMPLE_RATE`: Sample rate in Hz (default 2.4 MSPS)
- `NUM_SAMPLES`: Samples per FFT (must be power of 2, default 16384)
- `GAIN`: Gain in tenths of dB (default 496 = 49.6 dB)
- `WEB_PORT`: Web interface port (default 8070)
- `RTL_TCP_PORT`: RTL-TCP server port (defined in types.hpp, default 1234)
- `TCP_DATA_PORT`: Multi-channel data streaming port (defined in types.hpp, default 8091)
- `TCP_CONTROL_PORT`: JSON control interface port (defined in types.hpp, default 8092)

**Optimization Flags:**
- `ENABLE_BIAS_TEE`: Enable bias-tee on all devices (default 1)
- `USB_RESET_ON_INIT`: Reset USB on initialization (default 1)
- Uses ARM NEON vectorization on aarch64, automatically falls back to scalar on x86_64
- Compiled `-O3 -fno-math-errno -fcx-fortran-rules` + `-mcpu=native` on
  aarch64 (`-march=native` x86_64; docs/dev/compiler-flags.md). The
  correlation's whole-array |z| loops use `std::sqrt(std::norm(z))` / a
  norm compare - `std::abs` of a complex is a `hypotf` call per element

**Coherence / Ingest Robustness:**
- `RTL_USB_BUF_COUNT`: librtlsdr async USB transfer buffers (default 32 ≈ 218 ms cushion @ 2.4 MSPS; 0 = library default 15). Wider ring = more slack before the RTL2832 FIFO overflows if the reader thread stalls.
- `L2_RAW_MAX`: L2-raw staging depth cap (default 16). Bounds added latency under conversion overload; the per-device sample pool is sized from this.
- `STUCK_DEVICE_MAX_TIMEOUTS`: consecutive 100 ms drain timeouts on one device before declaring coherence lost (default 30 ≈ 3 s).
- `RT_PRIO_USB_READER`, `RT_PRIO_SAMPLE_DRAIN`: SCHED_RR realtime priorities (best-effort). The conversion worker deliberately runs at normal priority.
- `ENABLE_COHERENCE_MONITOR`: low-rate streaming coherence backstop (default **0/OFF**). A differential cross-correlation heuristic; needs ≥3 elements and on-hardware threshold tuning before enabling (a false positive triggers a disruptive ~30–60 s recalibration). The application-level detectors (L1 overflow / pool exhaustion / stuck device) are always on and cover the high-CPU-load case.

**Realtime scheduling (optional):** the USB readers and sample drain use SCHED_RR if the process is granted it. Without privilege they run at normal priority (safe default). To enable: add `<user> - rtprio 30` to `/etc/security/limits.conf` (or grant `CAP_SYS_NICE`). limits.conf only covers PAM logins: the boot service (`install-pi-service.sh`) sets `LimitRTPRIO=30` in its unit instead.

**KrakenSDR Wideband (downconverter) variant (`--wideband` / `-w`):**
- Hardware variant with per-tuner mixers driven by ONE shared LO: an on-board
  synthesizer speaking the Othernet moRFeus USB HID protocol (`10c4:eac9`,
  custom "WB fw" firmware; driver in `src/sdr/downconverter.cpp` - hidraw, no
  extra library). SET/GET frequency are implemented and the LO output is
  always on. heimdall programs the LO drive current (`WB_LO_CURRENT` default
  3; runtime `set_lo_current` control command, 0-7, surfaced as the "Cur"
  selector in the client UI next to Mix) and the frequency; the stock-moRFeus
  generator-mode command is not sent (unimplemented on this firmware, which
  answers unknown functions with an ident string). A current change is
  common-mode across channels (one shared LO) so it triggers no phase recal.
  Malformed HID frames wedge the parser until a power cycle - reports must be
  exactly 17 bytes with a leading 0 report-id byte.
- Tuners are parked at `WB_VARIANT_IF_HZ` (1268 MHz, the hardware IF filter);
  `update_sdr_settings()` maps a frequency change to an LO reprogram via
  `wideband_retune_rf()` (sdr_init.cpp), which every retune path uses
  (control port, web UI, scanner). The mixer side (`MixerSide`) places the
  LO: `LO = IF + RF` (high, RF 24-4132 MHz), `LO = IF - RF` (low, RF
  24-1183 MHz) or `LO = RF - IF` (below - LO under the RF, RF 1353-6668 MHz,
  the only side past the 5400 MHz synthesizer ceiling). The side follows the
  frequency: kept while it can reach the RF, else auto-selected
  (`downconverter_auto_side`: high up to its ceiling, below above it).
  `set_mixer_side` is a manual override that REJECTS sides unable to reach
  the current RF. The antenna ring also follows the frequency
  (`wb_ring_for_rf`: outer < 1 GHz, center < 2.5 GHz, inner above;
  `WB_RING_*_MIN_HZ` in config.h) - `wideband_retune_rf()` throws the RF
  switches on boundary crossings. Retune validation uses the union span
  (`downconverter_rf_union_range`, 24-6668 MHz). `current_frequency` stays
  the true RF (`uint64_t`) and is what packets/status report.
- High-side injection conjugates the baseband; the conversion hot loop negates
  Q (`sdr_pipeline.cpp`) so calibration, TCP clients and DoA all see an
  upright spectrum with true RF phases. Low and below arrive upright. Side
  changes run the same cooldown -> phase-recal flow as retunes (LO
  distribution phases move with LO frequency).
- **Noise-source RF path switching (critical for calibration):** unlike the
  standard KrakenSDR, where the coupler network keeps the noise source
  permanently coupled into every channel (GPIO0 merely powers it), the
  Wideband board has RF switches between the antenna rings / noise source and
  the mixers, driven by **GPIO1–6 of the channel-0 chip** (serial 1000).
  `wideband_set_noise_path()` (sdr_init.cpp) throws them; it is called from
  `set_bias_tee_all_devices()` (the single noise on/off choke point) and once
  after device init. Without it the tuners keep looking at the antennas while
  the noise source runs, the channels share no common signal, and lag/phase
  calibration never converges. Switch patterns are ported verbatim from the
  v1 heimdall_daq_fw wideband branch (rtl_daq.c).
- Three antenna rings (0 = outer, 1 = center, 2 = inner) selected by the same
  switch bank: `downconverter.array_select`, control command `set_array`
  (reported as `downconverter.array` in status JSON). A ring change runs the
  same cooldown -> phase-recal flow as a retune (different switch/cable path).
- Mutually exclusive with `OperatingMode::WIDEBAND_SCAN` (tuner spread);
  `set_wideband_mode` is refused while the variant is active.
- Missing moRFeus is fatal at startup in this mode; non-root access needs the
  hidraw udev rule (install.sh installs `99-morfeus.rules`). GPIO switching
  needs the librtlsdr fork with `rtlsdr_set_bias_tee_gpio` (the `librtlsdr/`
  source tree at the repo root, linked statically - see *Dependencies*).

## Data Flow

The ingest path is deliberately split into a cheap, time-critical **drain** and a
heavier **conversion** stage (the L1 → L2-raw → L2 decoupling) so that USB keep-up
never depends on DSP/serialization load — the dominant cause of silent sample
loss under high CPU load.

1. **RTL-SDR Callback** (`rtlsdr_callback` in sdr_device.cpp, per-device USB reader thread):
   - Receives raw IQ samples; pushes them to the device's L1 circular buffer (`devices[i]->l1_buffer`).
   - Strictly **allocation-free** (`BufferPool::try_acquire`). On L1 overflow or pool exhaustion it does **not** silently drop the oldest buffer for one device (that shifts a channel by a full packet and silently breaks coherence); it calls `signal_coherence_lost()` so the watchdog can recover coherently.

2. **Sample Drain** (`sample_processor` in sdr_pipeline.cpp):
   - Pulls one block from every device's L1 in lockstep into an aligned raw set, then hands it to the `l2_raw_buffer` staging queue. **No conversion/compensation/TCP here** — keeps the drain fast. Runs at realtime priority.
   - On staging overflow it drops the oldest **whole aligned set** (coherence-safe). The drain also OWNS the L1 flush (recovery / retune): `clear_l1_buffer()` posts a request that the drain serves between sets, dropping any partial set, and `flush_l1_now()` waits for a quiet point (every active device's `last_arrival_ns` - stamped at the END of the callback, after the enqueue - within half a packet period of the others and of now) so each device loses the same round of packets. A stamp taken before the enqueue let one device keep a packet the others lost: a whole-packet slip the lag servo then had to slew off. The quiet point also requires each device's last inter-arrival gap (`prev_arrival_ns` -> `last_arrival_ns`) to be 0.5-1.5 periods: a reader thread catching up after a stall delivers an OLD round late and then in a burst, which the last-arrival times alone can't tell from the current round. No quiet point within 4 periods: flush anyway, with a (10 s throttled) warning. A persistently stalled device trips `STUCK_DEVICE_MAX_TIMEOUTS` → `signal_coherence_lost()`.

3. **Conversion Worker** (`conversion_worker` in sdr_pipeline.cpp):
   - Consumes `l2_raw_buffer` **in order**; converts uint8 IQ → complex with phase/lag/EQ compensation (`samples_to_complex_with_compensation`, per channel, written in place into a recycled `l2_buffer_pool` set). Everything applied set-wide (phase vector × forward comp, noise-source state, per-bin EQ, spectral inversion) is read ONCE per set (`SetConversionState`), so a set straddling a change is converted the same way on every channel.
   - Broadcasts **every** set to the TCP servers (tcp_data_server, rtl_tcp_server), then pushes the converted set to the L2 global queue (`l2_buffer`).
   - **Flush epoch**: every flush (`clear_l1_buffer` / `clear_l2_buffer` → `flush_downstream()`) empties L2-raw and L2 and THEN bumps `l2_flush_epoch`, under `l2_flush_mutex`. The worker reads the epoch BEFORE dequeuing a set; the TCP broadcast and the L2 push happen under the mutex after re-checking it, so no pre-flush set reaches L2 (calibration) or the 8091/1234 taps after a recovery/retune flush. Broadcasting under it is safe because the data server reads the phase state with a try_lock (`get_phase_compensation_state_nonblocking`) - `check_phase_convergence` flushes while holding `state_mutex`. `clear_l1_buffer()` empties the downstream queues AFTER the drain has served the L1 flush (a set completed just before it went into L2-raw).
   - Runs at **normal** priority on purpose: it may fall behind safely (whole sets drop upstream at L2-raw) and at realtime priority its malloc/mutex use would priority-invert against the web/correlation threads.

4. **Correlation Processor** (`correlation_processor` in correlation.cpp):
   - Consumes L2 (drain-to-newest); FFTs all channels; cross-correlates vs the reference.
   - Extracts lag/phase via Gaussian interpolation; updates `CorrelationResult`; broadcasts via WebSocket.

5. **Compensation Processors** (per-channel threads in compensation.cpp):
   - Run the lag compensation state machine; apply frequency corrections; phase compensation runs once after all channels converge.

6. **Coherence Watchdog** (`coherence_watchdog` in compensation.cpp):
   - Watches the `coherence_lost` flag; on a detected desync runs `recover_coherence()` (full flush + recalibration). See **Coherence-Loss Detection and Recovery** under Key Algorithms.

## Threading Model

**Main Threads:**
- **Main Thread**: Runs uWebSockets event loop, handles WebSocket connections
- **Per-Device RTL-SDR Threads**: One per device, receives samples from hardware (realtime/SCHED_RR, best-effort)
- **Sample Drain Thread**: Cheap aligned L1→L2-raw collection (realtime/SCHED_RR, best-effort)
- **Conversion Worker Thread**: IQ conversion/compensation + TCP broadcast, L2-raw→L2 (normal priority)
- **Correlation Processor Thread**: DSP processing and peak detection
- **Per-Channel Compensation Threads**: One per non-reference channel, runs state machines
- **Coherence Watchdog Thread**: Runs `recover_coherence()` (full recalibration) after a detected desync
- **TCP Status Broadcaster Thread**: Broadcasts status at 2 Hz
- **TCP Server Threads**: One each for data server, control server, and RTL-TCP server

**Synchronization:**
- L1 / L2 / L2-raw queues: lock-free `moodycamel` queues, each paired with an atomic size counter
- Settings: Global mutex (`settings_mutex`) for frequency/gain changes
- Correlation result: Internal mutex (`CorrelationResult::data_mutex`)
- Phase compensation: Internal mutex (`PhaseCompensationData::state_mutex`)
- Atomic flags: `global_running`, `current_frequency`, `current_gain`, `bias_tee_enabled`, `rtl_tcp_channel`, plus coherence flags `coherence_lost`, `recovery_in_progress`, `coherence_event_count`
- **Queue size counter rule (important):** `l1_buffer_size`, `l2_buffer_size`, `l2_raw_buffer_size` are maintained ONLY by matched `fetch_add` (on enqueue) / `fetch_sub` (on every dequeue). Never `store(0)` them — a store races concurrent producers/consumers and can leave a phantom item whose later `fetch_sub` underflows the `size_t` to `SIZE_MAX` (which permanently wedges the L1-full / staging-depth checks).
- **Thread scheduling:** `set_thread_realtime()` (core/utils) raises the USB readers and sample drain to SCHED_RR (round-robin, so equal-priority readers can't lock each other out). Best-effort: with no `CAP_SYS_NICE`/`rtprio` privilege it logs once and runs at normal priority.

## Key Algorithms

### Lag Compensation: Closed-Loop Proportional Servo

Compensates for sample-clock offsets between devices. The entire acquisition
is ONE mechanism: `MEASURING` (brief reading-consistency check) hands any lag
magnitude straight to `SERVOING`, a closed-loop proportional-rate servo on
the **register-only** actuator `rtlsdr_set_sample_freq_correction_f()`
(RTL2832 sample-frequency-offset regs 0x3e/0x3f, ±0x1FFF counts of 2^-24 ≈
±488 ppm; the fork clamps).

- **Counts ∝ measured lag** (median of last 5 readings), saturating at
  `servo_max_counts` (6000 ≈ 357 ppm ≈ 858 samples/s) far out and tapering
  exponentially — the endgame (freeze near zero, write-kick learning,
  software `frac_delay` trim, ±0.02 lock) is unchanged.
- **Why this actuator**: it moves ONLY the sample clock. The correlation
  peak attenuates as ~2·Si(πΔ/2)/(πΔ), Δ = ppm×NUM_SAMPLES drift per
  window: 69% at 100 ppm, 17% at 357 ppm, 12% at the ±488 ppm register
  ceiling — hardware-verified usable right AT the ceiling (477 ppm probe:
  clean tracking at ~1100 samples/s), so the register range, not peak
  visibility, is the binding limit. The servo measures WHILE correcting.
- **Two-speed cadence**: coarse flight (|med| ≥ 2) updates on EVERY reading
  (rolling median, register writes hysteresis-gated at ≥12.5% change);
  the endgame keeps the original every-5th-reading cadence and untouched
  freeze/kick logic. Faster updates raise the delay-safe gain ~5×, and the
  exponential taper (τ = 1/gain) dominates total convergence time.
- **Delay-aware gain**: readings lag reality by pipeline transit + median
  window + update period. A delayed proportional loop rings unless
  gain×delay ≪ 1, so effective gain is capped at ~0.15/update_period
  (measured per control update); `servo_gain` (0.5/s) is the idle-machine
  ceiling. Above |lag| 2 the drive direction FOLLOWS the measurement (an
  overshoot self-corrects); below 2 it keeps the entry direction and the
  freeze logic owns stopping (noise flips the median's sign there).
- **Engage** zeroes the register first (a stale correction from an
  interrupted run would read as unfixable clock drift).
- Verified on Pi 4: lags up to −4818 → all-channel lag + phase convergence
  in ~25 s; −10,635 under full CPU load converged monotonically, zero
  coherence events.

**Why not timed "bursts" on the PLL path** (`rtlsdr_set_freq_correction`,
the historical approach — code removed July 2026): that call retunes the
tuner LO by ppm×RF (10 kHz at 100 ppm/100 MHz), which destroys the
correlation peak while active (mid-burst readings are garbage), each
set/reset pair slips a random ~2–4 samples, and open-loop timing makes the
landing point dice on a loaded machine (landing = lag −
ppm×2.4×(duration + timing jitter) − slip). The register-only servo has
none of these failure modes.

### Phase Compensation (phase-only)

Uses eigenvalue decomposition of the spatial correlation matrix. The dominant
eigenvector of the noise-source data is proportional to the per-channel complex
gains; its phase gives each channel's correction, a UNIT phasor per channel.
Gain is measured (for display) but deliberately not corrected - see below.

1. **WAITING_FOR_LAG_COMPLETION**: Wait for all channels to reach lag convergence
2. **MEASURING_INITIAL_PHASE**: Collect phase measurements (readings are ignored
   until `NOISE_SETTLE_MS` after noise-on; only sets whose phases came from the
   eigen solve count - `CorrelationResult::phases_measured` - so placeholder
   zeros published outside the measuring states never read as a calibration)
3. **APPLYING_COMPENSATION**: Average `required_stable_readings` (3) snapshots as
   unit phasors (circular mean), apply to all future samples
4. **VERIFYING_CONVERGENCE**: Measured on the compensated stream; requires phases
   within ±1°. A failing check triggers the retry pass, which multiplies the
   remaining phase into the vector.
5. **CONVERGED**: Phase drift within threshold (±1°)

**Retune recalibration timing** (re-measured 2026-10-02, 8091 stream): a
retune/gain/side/ring change runs the `STABILITY_DELAY_MS` (3000 ms) cooldown
with the noise OFF -> noise on -> `NOISE_SETTLE_MS` (250 ms, was 1000) gate ->
3 snapshots -> verify: ~3.5 s total (was 4.3 s). The cooldown is a DEBOUNCE
kept at 3 s by choice (each retune restarts it, so a drag calibrates once at
the end and slow tuning doesn't flash the noise source between steps) -
nothing physical needs it: with the noise source on during the cooldown, the
tuner phases sit within 0.2-0.4° of final from the first 50 ms after 1 / 30 /
400 MHz steps, and a 500 ms cooldown recalibrates in ~1.0 s with the same
Check Calibration residual (0.25° vs 0.20° - the check's rounding). The noise
reaches the stream in the first packet after the flag flips, at full power,
and the phases settle within ~20 ms. Runtime override: control port
`set_stability_delay` (100-10000 ms; the continuous scanner uses 500 while it
runs). The DoA client holds DoA on heimdall's WAITING_FOR_STABILITY state (min
500 ms) and the noise flag (+250 ms) instead of a fixed 4 s.

**Calibration hold** (control port `{"command":"hold_calibration","hold_ms":N}`,
`set_calibration_hold()` / `calibration_held()` in compensation.cpp; added
2026-10-22 for the DoA client's saved-frequency scan, which searches its bands
with the array uncalibrated and calibrates only where it finds a signal): a
retune / gain change still enters the cooldown (noise off,
WAITING_FOR_STABILITY - clients hold DoA, identity compensation) but the lag
driver doesn't let it expire into `handle_settings_change()` while held, and
the periodic check stands down (`checkable`, `preempted`). A LEASE: N > 0
holds for N ms from now (<= 30000; the client renews it every second for
3 s), so a client that stops renewing - stopped, crashed - lets it lapse and
the pending calibration runs as usual; N = 0 releases at once and, if a hold
was in effect, backdates a pending cooldown's `last_frequency_change` by the
stability delay, so the calibration starts on the lag driver's next tick (~1 s
to CONVERGED instead of 3.5 s) - a 0 with no hold in effect (lapsed, never
sent) leaves an ordinary cooldown alone. A coherence recovery is NOT held (it needs the full recal). Status
JSON `calibration_hold`. Unlike the discrete scanner's deferral (which keeps
the old compensation and CONVERGED across its hops - fine in wideband mode,
no DoA) the held state is WAITING_FOR_STABILITY: a coherent client would
otherwise compute DoA on a calibration from another band. Tested only
against a simulated heimdall in kraken_doa's e2e test (the real dongles were
in use) - the paths are the existing cooldown's, plus one gate.

**Drag retunes (fast path, `update_sdr_settings` in sdr_init.cpp)**: a
coherent retune within `FAST_TUNE_SETTLE_MS` (400 ms) of the previous one, or
before the previous step's tuners were finished (at most
`FAST_TUNE_DRAG_WINDOW_MS`, 1 s), tunes with the fork's
`rtlsdr_set_center_freq_fast(RTLSDR_TUNE_KEEP_AUTOTUNE)`: the R820T's PLL
autotune clock stays at 128 kHz between steps (the full path writes 128 kHz
before N / SDM and 8 kHz after the lock: 2 control transfers per tuner and
step); the lock is still read on every step. Only when `fast_retune_allowed()`
(coherent, not `--kerberos` manual cal, no discrete scan, no recovery, no
downconverter); decided and tuned in one `device_io_mutex` hold; the rollback
of a failed step is always the full path. The WIDEBAND SCAN's retunes
(`setup_wideband_frequencies`: every tuner to its own spread frequency) take
it the same way (2026-10-21, the user's "dragging in wideband mode is
laggy"): `drag_step()` = the one decision for both modes, sharing
`last_drag_retune_ns` (renamed from last_coherent_retune_ns - every retune
of either mode is the drag's latest, so the watchdog's finish never runs
inside a wideband drag) and `fast_tune_pending`; `fast_wideband_allowed()`
(wideband scan, no discrete scan - its hops are seconds apart, flushed and
settled -, no recovery, no downconverter); a failed wideband drag step goes
once more on the full path (logged); `finish_fast_retunes` retunes a tuner
it finds unlocked at its wideband scan frequency when the library lost it.
Measured on the KrakenSDR (Windows, 5 tuners, 8092 `set_frequency` reply,
steps 50 ms apart; job scratch 0d0a7cca hw_*): 114.0 -> 74.0 ms median per
step (max 123 -> 83) for 25 / 100 / 250 kHz steps in the FM band, a drag
across the 109.5 MHz divider boundary and 100 kHz steps at 595 MHz; the
first step after a pause and typed jumps stay on the full path (153 / 194-216
ms both); after every drag all five tuners on their frequencies (status +
8091 headers), neighbours' spectra lined up bin for bin in their overlaps
(lag 0 wherever the overlap held a signal, identical to the old build), 0
coherence events, no failed step, no unlocked tuner; 199 of 204 steps on the
fast path. Mode switches straight after a drag (tuners pending): wideband ->
coherent recalibrated and CONVERGED in 25 s, coherent drag -> wideband (the
spread itself a drag step, 83 ms) and wideband -> independent -> wideband
with the tuners right and aligned. The Pi (parallel threads) not measured `finish_fast_retunes()`
(`rtlsdr_finish_tune`: lock read, then 8 kHz) puts the tuners back in the
full path's resting state - from the coherence watchdog once 400 ms have
passed without a retune, and from `set_bias_tee_all_devices(true)` in any
case. INVARIANT: every calibration switches the noise source on through
`set_bias_tee_all_devices(true)`, so it never runs on tuners still in the
drag state - a new calibration entry point that skips that call (noise
already on) must call `finish_fast_retunes()` itself. A tuner the finish
finds unlocked is retuned on the full path (twice), else
`signal_coherence_lost("PLL not locked after a drag")`. Closing the handles (element-count change, reopen) drops the
pending state. Windows tunes the dongles one by one, Linux in parallel threads
(`for_each_tuner`; each thread only its own handle and result slot, the
atomics written only under `device_io_mutex`), both with the fast path. A
step after the finish (0.4-1 s stepping) goes back to the full path: the
finish + the next 128 kHz write would cost more than it saves.
`RTLSDR_TUNE_SPAN_AUTOTUNE` (128 kHz inside the N / SDM write) is NOT
passed: it saved only a drag's first step (72.7 -> 61.7 ms, measured with an
earlier library), later steps cost the same. Measured 2026-10-19 (5
streaming tuners, 40 steps of 25 kHz 50 ms apart, 8092 `set_frequency`
reply; library details docs/dev/librtlsdr-fork.md):
- Windows (one by one): first step 112.8 ms (full path, 5 transfers per
  tuner), step 2 writes 128 kHz once (4), later steps 74.0 ms median (3: `R5`
  register 4, `W15+2` N / SDM, `R3` lock; fast path off 114.0 ms, 5); the
  watchdog's finish (`R3` lock read, `W1a` 8 kHz, 2 per tuner) 0.41-0.45 s
  after the last step; a recalibration started mid-drag finished the tuners
  0.11-0.14 s after the last step, before the noise-source GPO write; steps
  during a recovery took the full path and left nothing pending; typed jumps
  7-8 transfers per tuner (full path); a gain change 7, no PLL write; 0
  coherence events. With the 5000 us PLL settle wait (Pi numbers below
  too): later steps 73.9 ms, typed jumps +12-13 ms (5 tuners x ~3 ms at
  divider crossings)
- Raspberry Pi 5 (parallel threads; transfers per dongle incl. the dummy
  read and the noise-source GPIO read + write; 3 runs): first step 18.5-19.5
  ms (8), step 2 7, later steps 14.3-14.5 ms median (6; fast path off 19.0
  ms, 8; 00606c0 48.1 ms, 20); the tuners are done 10.1 ms into a step (fast
  off 14.8, 00606c0 43.9). begin_retune_cooldown re-asserts noise-off on
  every retune (its skip when already off is `#ifdef _WIN32` only), and
  set_bias_tee_all_devices switches the dongles through for_each_tuner
  (results logged after the loop; ext_noise / wideband / kerberos switches
  stay sequential after it): the 5 dongles' `Sr3001` / `S3001` take 3.8 ms,
  9 of 10 transfers overlapping (one after another: 18.3 ms, the step 28.7
  ms - fast off 33.4, 00606c0 62.5). Jumps 19-31 ms (were 40-42), gain
  changes 28 ms (43). The finish (lock read, 8 kHz) 0.41-0.46 s after the
  last step; a recalibration started mid-drag (3 of 3 runs) finished every
  tuner 2.0-2.2 ms before the first noise-on GPO write (+10.8 to +36 ms after
  the last step: the noise-on hook, not the watchdog), the 5 noise-on
  writes within 0.3 ms; steps during the recovery took the full path (15-16
  ms); phases reconverged in 2.6-3.6 s; 0 coherence events
- Pi, FIXED (Nagle): every ~10th step used to reply ~12 ms later with the
  same USB span - a 2 Hz status broadcast fell inside each slow step and
  the reply came 40.1-47.0 ms after it: Nagle + Linux's 40 ms delayed ACK,
  as TCP_NODELAY on 8092 was set only on Windows; TCP_QUICKACK on the
  client removed it. heimdall now sets TCP_NODELAY on every platform
  (tcp_control_server.cpp; 8091 / 1234 stream bulk samples and keep
  Nagle). Measured 2026-10-20, no TCP_QUICKACK on the client: 0 late
  replies in 3 fast-path runs (4 of each 40-step drag's steps had a
  broadcast inside; steps 2+ 14.2 ms median, p90 14.6-14.8, max 16.7-16.8
  - once 39.0 ms with no broadcast inside: heimdall idle 25 ms between
  the tuner writes and the noise-GPIO read, cause unknown) and the fast
  path off run (18.9 ms median, max 21.6), 0 also in the mid-drag
  recalibration drags (were 2 per drag); a QUICKACK drag is now the same
  as a plain one. 00606c0's 48 ms step outlasts the 40 ms, so the test
  can't tell there
- Pi, once in 5 runs (2026-10-19): a 599 ms gap inside a step (all 5
  threads; cause unproven, likely the test's USB logging shim) made the
  next I2C write fail with PIPE on every dongle; the full-path rollback
  restored the previous settings without a coherence loss, but the reply
  said `{"status":"success","frequency":<new>}`. Both FIXED: the library's
  stall retry now runs on Linux too and recovered every such write (see
  docs/dev/librtlsdr-fork.md), and a change that still fails replies an error with
  the PREVIOUS value (`SdrUpdateResult`, the optional out-parameter of
  update_sdr_settings; its bool still means "hardware written,
  recalibrate"): set_frequency / set_gain answer
  `{"status":"error","message":"Retune failed on channel(s) X - previous
  settings restored","frequency":<previous Hz>,"failed_channels":[X]}`
  (gain: `"Gain change failed ..."`, `"gain":<previous dB>`), also for a
  failed restore (`"... and restoring the previous settings failed on
  channel(s) X - tuners inconsistent, coherence recovery forced"`), a
  failed Wideband LO retune and an independent-mode tuner that failed (the
  others took it). Measured 2026-10-20 with injected tuner-write failures
  (LIBUSB_ERROR_IO, never retried; Windows also drop / land / 4 PIPE
  stalls in a row) on Windows and the Pi 5: set_frequency, set_gain and a
  drag step replied the error with the previous frequency / gain (a drag
  step: the step before's; the next steps went through), every tuner's
  registers afterwards equal to those before the command (a drag step:
  only 0x1a differs, the full-path rollback's 8 kHz), all locked; cooldown
  + recalibration, reconverged in 3.9-4.1 s (Windows drag: 2.1 s after
  its end), 0 coherence events; a failed
  restore: coherence_events 0 -> 1, recovered in 11.5 s (Windows) / 13.3
  s (Pi); heimdall kept running. Windows replies 193-330 ms (normal drag
  steps 74). A single PIPE stall was retried by the library and replied
  success. Not tested: independent mode, the Wideband LO failure, the web
  UI path, a failed restore that leaves the tuners really on different
  frequencies (the forced recovery flushes and recalibrates, it doesn't
  retune - code reading). Every reply consumer only logs an error
  (kraken_doa discards replies and follows the 8091 header; the GR source
  logs a warning). The web UI has no error display; its "Current:" values
  come from current_frequency / current_gain

Needs a fork with the call (`#ifdef RTLSDR_TUNE_KEEP_AUTOTUNE`: the code
still compiles the full path only against an older one, but the Makefile and
CMake now require krakenrf/librtlsdr 578b43d - `RTLSDR_FIX_MARK`, every
platform, the Dockerfile pins it); they also rebuild / check
`../librtlsdr/build/src/librtlsdr.a` against the fork's sources on every
platform, since a new header with an old library would fail the link.

**Why phase-only**: introduced 2026-09-29 while the IF VGA was under the RTL2832
AGC, when each dongle's gain hunted by up to ~2.5 dB between 16k-sample
snapshots and the former gain correction (averaged from 3 snapshots) locked in
up to ~1.6 dB of error, making the match worse. With the VGA now fixed (below)
the gains are stable to ~0.1 dB, but the noise source clips at that VGA step,
which biases a gain estimate. Re-tested 2026-09-30 with the VGA fixed: the noise-source gain estimate is not
trustworthy - it shifts by 0.7-1.0 dB between VGA step 8 (ADC clipping) and
step 0, and disagrees with the receiver noise-floor ratio by up to 2.4 dB (the
noise source compresses the tuner front end, so it measures saturation levels,
not small-signal gain). Gain correction stays off.
The `NOISE_SETTLE_MS` gate still keeps the USB-ring backlog and noise-source
turn-on out of the estimate.

**IF VGA fixed, as stock osmocom librtlsdr (decided 2026-09-30)**: the fork
leaves the R820T IF VGA under the RTL2832 AGC even in manual gain mode
(`r82xx_set_if_mode()` with if_mode 0 sets VGA register 0x0c bit 4 and
`rtlsdr_vga_control()` enables the demod AGC loop). `set_fixed_if_vga()`
(sdr_init.cpp) re-fixes it after every gain setup - device open and each
`update_sdr_settings()` gain change - via `rtlsdr_set_tuner_if_mode(10000 +
index)`: `R820T_IF_VGA_MANUAL_IDX` 8 (16.3 dB) / `R820T_IF_VGA_AUTO_IDX` 11
(26.5 dB) in config.h. Measured on hardware:
- gain stable to ~0.1 dB between snapshots; no noise-on ramp
- the noise source clips ~11-14% of samples at step 8 regardless of RF gain
  (it compresses the tuner front end, so lowering the RF gain doesn't help);
  simulated phase bias from that clipping is <= ~0.26° - accepted
- strong antenna signals clip at 49.6 dB RF gain, clean at ~42 dB
- switching the VGA to a low step only while the noise source is on was
  tested and REJECTED: the VGA step itself shifts each dongle's phase by a
  different amount (~1° at step 1, up to ~8° at step 0, vs step 8), so
  calibration and operation must run at the same step
- `set_if_vga` control command `{"command":"set_if_vga","index":0-15}` fixes
  the step on all open tuners at runtime (`set_if_vga_all()`), for tuning /
  diagnostics; not persisted, the next gain change re-applies the configured step

**Application**: One complex multiply per sample by
`phase_compensation->compensation_vector[channel]` (unit magnitude; the S2P
forward compensation, when enabled, is folded into the same scalar and may
carry gain) in `samples_to_complex_with_compensation()`. Each element
(`AtomicComplex`, core/atomic_complex.hpp) packs real+imag into ONE 64-bit
atomic, so the hot loop can never read a torn vector mid-calibration.

**Visibility**: the control-port status JSON (port 8092, 2 Hz) reports the live
applied vector per channel as `"channel_comp":[{"amp_db":..,"phase_deg":..},..]`
(`amp_db` is always 0). The web UI channel table shows the applied phase in
its **Correction** column next to the live measured **Phase** / **Amp** (gain vs
the reference, EWMA-smoothed against single-snapshot jitter); both ride the
8070 binary correlation message after the phases array
(`build_correlation_message` in correlation.cpp ↔ `processData` in index.html).

### Operating Modes (coherent / wideband scan / independent)

`OperatingMode` (types.hpp): COHERENT, WIDEBAND_SCAN (tuners spread side by
side, `setup_wideband_frequencies`) and INDEPENDENT (every tuner its own
receiver). `set_operating_mode()` (sdr_init.cpp) is the one switch; 8092
`set_operating_mode {"mode":...}` / the old `set_wideband_mode`:
- Leaving COHERENT parks the calibration (`park_calibration_locked`: phase
  machine parked, per-bin EQ dropped, noise source off, FFT/correlation off).
  Every "calibration stands down" check is `operating_mode != COHERENT`
  (phase/lag stages, retune cooldown, `handle_settings_change`, flush-only
  recovery, periodic monitor, element-count changes refused)
- INDEPENDENT: no compensation applied in the conversion
  (`snapshot_conversion_state` -> identity); per tuner `set_independent_tuner`
  (frequency and/or gain; `wideband_config.tuner_frequencies` /
  `tuner_gains`); `set_frequency` / `set_gain` / `SDR_SETTINGS` set EVERY tuner
  (no-op shortcut and rollback skipped in `update_sdr_settings`); the discrete
  scanner and the web UI's noise-source switch are refused
- Returning to COHERENT (`return_to_coherent_locked`): every tuner back on
  `current_frequency` and the common gain, then the phase recalibration (or
  the KerberosSDR manual-calibration rules)
- 8091: per-channel frequency outside coherent, per-channel gain in
  independent; phase-state flags 0x400 (wideband) / 0x800 (independent). Status
  JSON: `operating_mode`, `tuner_frequencies`, `tuner_gains`; web STATE:
  `operating_mode` (index.html shows a banner)
- Persisted (`operating_mode`, `tuner_freq<N>`, `tuner_gain<N>` in
  heimdall_settings.conf via `settings::note_mode`) and re-entered at startup
  before the pipeline threads start (`restore_operating_mode`)
- Unavailable in the downconverter variant (tuners parked at the IF)

### Coherence-Loss Detection and Recovery

**Invariant**: coherence requires that the N-th sample set from every device is the same instant. The dongles are sample-locked (shared clock); calibration only removes a small sub-sample/few-sample offset. A single dropped USB packet shifts one channel by a full `NUM_SAMPLES` — far outside what lag/phase compensation can represent — and silently destroys coherence. All buffer dropping must therefore happen at the **aligned** stage (whole sets, all channels) and **never per-device**.

**Detection** (`signal_coherence_lost()` in sdr_pipeline.cpp sets the `coherence_lost` flag, bumps `coherence_event_count`):
- RTL callback: L1 overflow / sample-pool exhaustion (sdr_device.cpp)
- Sample drain: a device stalled past `STUCK_DEVICE_MAX_TIMEOUTS`
- Optional streaming monitor (`maybe_run_coherence_monitor`, default OFF — see config)

**Recovery** (`coherence_watchdog` thread → `recover_coherence()` in compensation.cpp):
- Re-engages the noise source, resets lag to MEASURING + flushes L1/L2/L2-raw, resets phase to WAITING_FOR_LAG_COMPLETION (identity vector, per-bin dropped), re-enables FFT — i.e. a full startup-equivalent recalibration (the only recovery robust to an arbitrary full-packet slip).
- `recovery_in_progress` spans the whole async recal (cleared in `complete_phase_calibration_locked`), and is surfaced in the control-port status (`coherence_events`, `recovering`).
- A frequency / gain / mixer-side / ring change during a recovery (or a KerberosSDR manual recal) restarts its phase stage (`recovery_settings_changed`: MEASURING_INITIAL_PHASE, settle gate re-armed, pre-retune sets flushed) once lag has converged - a measurement past that point was taken on the old settings and could verify on pre-retune data and latch it as CONVERGED. A discrete-scanner hop marks the calibration for a recal when the scan stops (`scanner_cal_deferred`; `--kerberos`: STALE).
- Debounce: an event that fires **during** a recovery is peeked (not consumed) and acted on once the current recovery completes; a recovery that never converges is aborted after 120 s with the noise source switched off and FFT disabled so it can't latch a no-noise-source calibration.
- Flush-only (no recal) in wideband scan or while the coherent discrete scanner is active (a recal can't converge while hopping). The same rule covers every other recal trigger during a discrete scan: a retune/gain cooldown, `handle_settings_change()` and a calibration already in flight when `start_scanner` arrives (its noise source is switched off) are deferred via `scanner_cal_deferred`, and the lag driver runs one `begin_retune_cooldown()` when the scan stops; the periodic check stands down for a scan started mid-check.
- **Calibration only progresses on the noise source**: lag acquisition (MEASURING/SERVOING) and the phase measuring stages ignore readings while `bias_tee_enabled` is false. The parked states (kerberos uncalibrated idle, the 120 s abort, a calibration stood down for a scan) idle the machines by switching FFT off, so turning the FFT on there used to acquire on the antenna signal and latch a "calibration"; a servo caught with the noise off is parked (register zeroed), and a lag reset zeroes a live correction register too. Likewise `begin_retune_cooldown()` and `handle_settings_change()` are no-ops in wideband scan (the phase stages skip scan mode, so a recal would leave the noise source on across the scan; leaving the scan recalibrates anyway), and a scan-mode retune from the web UI (`SDR_SETTINGS`) or control port (`set_frequency`) re-spreads the tuners via `setup_wideband_frequencies()` instead of tuning them all to one frequency. The retune/gain cooldown handlers (web_server.cpp, tcp_control_server.cpp) and `handle_settings_change` skip their phase-only override while `recovery_in_progress` so they don't clobber the recovery.

### KerberosSDR Mode (--kerberos / --kerberos_sw)

KerberosSDR has no noise-source RF switch (noise couples in via a directional
coupler), so calibration is only valid with the antennas manually
disconnected. With `--kerberos`, every automatic noise-on path is suppressed
(`kerberos_manual_cal_only()` in core/config.hpp guards them all):

- Startup, coherence loss and element-count reconfiguration land in
  `kerberos_enter_uncalibrated()` (compensation.cpp): noise off, FFT off
  (idles the lag/phase machines, same trick as the watchdog's 120s abort),
  machines reset, identity compensation, `kerberos_cal_stale` cleared
- Frequency/gain changes keep the old compensation applied and set
  `kerberos_cal_stale` (STALE) instead of entering the cooldown recal
  (web_server.cpp SDR_SETTINGS, tcp_control_server.cpp set_frequency,
  handle_settings_change); the periodic monitor never checks
- The ONLY calibration trigger is FORCE_RECAL (web UI "Full Recalibration" button, confirm
  dialog): the watchdog calls `recover_coherence(manual=true)`, which runs
  the normal full noise-source recalibration; convergence clears the stale
  flag in `complete_phase_calibration_locked()`
- Status: `kerberos_calibration_state()` returns
  uncalibrated/calibrating/calibrated/stale - reported in the 8092 status
  JSON, the web STATE message (banner + greyed periodic-recal controls), and
  as HIGH BITS of the 8091 header's phase-state field (bit 8 = kerberos,
  bit 9 = stale; the DoA client and GR source mask the low byte)
- Element count defaults to 4 in kerberos mode (4-channel hardware; the
  persisted setting and USB auto-detect are skipped) - only `-n` overrides
- `--kerberos_sw` implies kerberos_mode but keeps automatic calibration:
  `src/sdr/kerberos_gpio.cpp` drives the CKOVAL antenna switches from Pi
  header GPIOs 23/24 (`KERBEROS_SW_GPIO_ANT1/ANT2` in config.h) via the
  linux/gpio.h v2 character-device API on the `pinctrl-*` gpiochip (found by
  label - rp1 on Pi 5, bcm2711 on Pi 4; pigpio is NOT used, it's broken on
  RP1). Idle 23=1/24=0 (antenna input 1); `kerberos_gpio_set_noise_path()`
  is called from `set_bias_tee_all_devices()` and the device-open path: noise
  on = save selection + both LOW, noise off = restore. Init in main() before
  device init; falls back to plain --kerberos (manual cal) if the GPIOs are
  unavailable; cleanup at shutdown restores the antenna selection and
  releases the lines. The 8091 kerberos header bit is NOT set in _sw mode
  (clients treat it as a normal KrakenSDR)

### External Noise Source Array (--ext_noise)

Standard KrakenSDR + an add-on antenna array whose noise source is powered from
the CH0 antenna bias tee. `external_noise_mode` (core/config.hpp, set by
`--ext_noise` / `--ext-noise`): `set_bias_tee_all_devices()` holds the internal
noise source (GPIO0, every chip) OFF and switches GPIO1 of the channel-0 chip
(`set_external_noise_power`) instead; `init_rtlsdr_device()` forces GPIO0 off
and `open_active_devices()` powers the external source for the startup
calibration. `apply_antenna_bias_tees()` treats CH0 as reserved (bit 0 refused
and cleared, GPIO1 never written from the mask). Everything else - calibration
state machines, `bias_tee_enabled` semantics, the 8091 noise flag - is
unchanged. Exclusive with `--wideband` and `--kerberos(_sw)` (refused at
startup).

### Calibration Buttons (web UI, Compensation Status card)

- **Check Calibration** (`CHECK_CAL`; control port `check_calibration`): the
  periodic monitor runs its non-destructive check at the next tick - briefly
  pulses the noise source, measures the residual lag/phase on the compensated
  stream, and sets `force_recalibration` only if out of tolerance. Works with
  the periodic check disabled; not in plain `--kerberos` (it pulses the noise).
  Result in the web STATE `cal_check` (`ok` / `drifted` / `no_verdict`, worst
  lag + phase), shown under the buttons
- **Full Recalibration** (`FORCE_RECAL`): the watchdog's full lag + phase
  recalibration from scratch (confirm dialog; KerberosSDR: disconnect antennas).
  `RESET_LAG_COMPENSATION` / `reset_lag_compensation` remain as aliases

### Pipeline Decoupling (L1 → L2-raw → L2)

The time-critical L1 drain (`sample_processor`) is separated from the heavy IQ conversion / compensation / TCP serialization (`conversion_worker`) by the `l2_raw_buffer` staging queue, so USB keep-up is independent of DSP load. Overflow drops whole aligned sets at L2-raw (coherence-safe), latency-bounded by `L2_RAW_MAX`. Per-device `sample_pool` is sized `MAX_BUFFER_SIZE + L2_RAW_MAX + 4` to cover both in-flight stages. The per-channel FIR state (`frac_tail`/`eq_tail`) stays correct because the worker processes sets in order on one thread.

## Common Development Tasks

### Adding a New Module

1. Create directory under `src/new_module/`
2. Add module source files to Makefile variables (e.g., `NEW_MODULE_SOURCES`)
3. Update `SOURCES` variable to include new module
4. Update `BUILD_DIR` creation in Makefile to include new subdirectory
5. Update CMakeLists.txt with new source set
6. Follow dependency hierarchy: new module should only depend on lower-level modules

### Modifying Signal Processing

- **Correlation algorithm**: Edit `src/dsp/correlation.cpp::process_correlations()`
- **FFT parameters**: Edit config.h `NUM_SAMPLES` and `CORRELATION_SIZE`
- **Peak detection**: Modify `gaussian_interpolation()` in correlation.cpp
- **Compensation thresholds**: Edit `ChannelCompensation` and `PhaseCompensationData` structs in types.hpp

### Adding Web API Endpoints

1. Edit `src/web/web_server.cpp::start_web_server()`
2. Add new HTTP route handler or WebSocket message handler
3. Update `index.html` to use new endpoint
4. Rebuild (HTML is loaded at runtime, no need to re-embed)

### Adding TCP Server Commands

1. Edit `src/net/tcp_control_server.cpp::handle_command()`
2. Add new JSON command handler in switch statement
3. Update README documentation for new command format
4. Test with netcat: `echo '{"command":"new_command"}' | nc localhost 8092`

## Debugging

**Runtime Inspection:**
- Web interface shows live correlation plots and compensation state at http://localhost:8070
- TCP control server broadcasts status JSON every 500ms
- Check logs for "Lag compensator" and "Phase compensator" state messages
- With stdout redirected to a file, output is errors-only (stdout is discarded
  at startup so long-running logs can't grow unbounded); set
  `HEIMDALL_VERBOSE_LOG=1` to restore full output for debugging

**Common Issues:**
- **No devices found**: Check serial numbers in config.h match physical devices (run `rtl_test` to list)
- **Poor correlation peaks**: Verify all devices on same frequency, check antenna connections
- **Phase drift**: Increase `PhaseCompensationData::required_stable_readings` in types.hpp
- **Lag servo ringing / slow convergence**: gain auto-derates with the measured update period (CPU load lengthens feedback delay); tune `ChannelCompensation::servo_gain` / `servo_max_counts` in types.hpp
- **Frequent coherence recoveries / `!! COHERENCE LOST` logs**: the ingest is dropping under load. Watch `coherence_events` / `recovering` in the control-port status. Causes: CPU overload (the conversion worker or correlation can't keep up), USB/power issues, or a flaky dongle. Grant realtime scheduling (see Configuration), reduce active channels, or check USB. A recovery that wedges is aborted after 120 s (noise source switched off).
- **Realtime priority not taking effect**: `set_thread_realtime` logs once if it lacks privilege; add `rtprio` to limits.conf or grant `CAP_SYS_NICE`. The app runs correctly at normal priority either way.

### FFT (pffft)

- The user's request (2026-10-17, after kraken_doa_v2's swap): FFTW (GPL)
  replaced by pffft (FFTPACK / BSD-style) so the suite needs no FFTW at all -
  `external/pffft` = the same files as kraken_doa_v2's (each app builds on its
  own; keep the copies identical). Built as C with `-DPFFFT_ENABLE_NEON` on
  ARM (Makefile `PFFFT_CC` / `PFFFT_CFLAGS`, CMake target `pffft`); without it
  pffft silently builds its scalar code. `config.h`'s `FFTW_PLANNER_EFFORT`
  was dead (the plans were always FFTW_ESTIMATE) and is gone
- `src/dsp/fft_plan.cpp`: `FftPlan` keeps its interface; pffft directly
  (`pffft_transform_ordered` = FFTW's natural order, unscaled both ways, e^-
  forward) - `CORRELATION_SIZE` (2 x NUM_SAMPLES) is a power of two, which
  pffft takes as it is (static_assert). The forward transform zero-pads
  straight into `output` and runs IN PLACE; pffft's scratch is ONE buffer per
  thread (`thread_work()`), not per plan. Both matter: a correlation pass
  walks 5-9 plans of 256 KB buffers, more than the Pi 5's L2 - the first
  port (separate input + per-plan scratch, as FFTW had) was only 4 % faster
  per pass although each transform alone was 25-28 % faster.
  `correlation_freq` stays a separate buffer: correlation.cpp reads the
  cross-spectrum after `cross_correlate`
- Benchmark (2026-10-17, job scratch 46eef211 hp/: bench_fft.cpp drives the
  old and the new `FftPlan` with correlation.cpp's per-set sequence on a
  5-channel noise-source scene; heimdall's own -O3 -march=armv8-a flags, Pi
  5): forward (copy + zero-pad + FFT, 32768 points) 305-320 -> 200-210 us,
  cross_correlate 365-375 -> 264-275 us, inverse 310-320 -> 220-235 us; a
  whole 5-channel set (5 forwards, 4 cross-correlations, 4 peak searches)
  3.65-3.67 -> 2.22-2.29 ms (-39 %); correlation outputs identical to 1.9e-7
  of the peak (same lags, phases to 0.0001 deg). On the KrakenSDR (hb.py:
  416.839 MHz, 5 channels, the user's settings copied, old HEAD build vs this
  one, alternately): every run converged (5 + 5), 0 coherence events; during
  the calibration heimdall 60.8-61.1 -> 57.8-57.9 % of a core, the
  correlation thread 47-49 -> 44-45 % - with its 10 ms loop sleep that is
  ~9.2 -> ~8.0 ms per correlation pass (the FFTs' 1.2 ms; the rest is the
  eigen solve, peak interpolation and the WebSocket message), so ~7 % more
  lag / phase readings per second for less CPU; startup to CONVERGED 22-36 s
  either way (the lag servo's slew, not the FFT). Once CONVERGED the
  correlation FFT is off: 12.5-14.2 % of a core for both (conversion worker +
  USB readers) - pffft changes nothing there

## Dependencies

**KrakenSDR librtlsdr fork** (github.com/krakenrf/librtlsdr), linked
STATICALLY from `../librtlsdr/build/src/librtlsdr.a` with the fork's own
headers (`-I../librtlsdr/include`). A distro `librtlsdr0` sits earlier in the
loader's search path than `/usr/local/lib`, so a dynamic link could silently
load the stock library; linked in, it can't (`ldd heimdall` shows no
librtlsdr). `make` clones/builds the static library if it's missing, and
fast-forwards a clone older than krakenrf/librtlsdr 578b43d (or stops with
instructions; `RTLSDR_FIX_MARK` = `rtlsdr_finish_tune`, every platform -
CMakeLists.txt checks the same); do NOT install `librtlsdr-dev` (distro
rtl-sdr packages may stay installed).
The fork's retune changes in 578b43d, on top of 00606c0 (skip-unchanged
register writes, one N / SDM write, the I2C repeater left on,
`rtlsdr_set_center_freq_fast` / `rtlsdr_finish_tune`, the stalled control
OUT retry) apply on every platform and to the installed `rtl_*`
tools too (only fewer USB transfers for them): docs/dev/librtlsdr-fork.md,
*Drag retunes* above.
`init_rtlsdr_device()` fails a channel whose sample rate, center frequency or
USB buffer reset can't be set (a silently mistuned dongle breaks coherence);
other setup calls only warn.

**System Libraries:**
- `libusb-1.0-0-dev`, `cmake`: build and link the librtlsdr fork
- (no FFT library: pffft is vendored in `external/pffft` - see *FFT (pffft)*)
- `libeigen3-dev`: Eigenvalue decomposition for phase calibration
- `libssl-dev`, `libcrypto++-dev`: SSL/TLS for uWebSockets (optional)
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies:**
- `uWebSockets/`: Cloned and built automatically by Makefile (HTTP/WebSocket server)
- `external/pffft/`: the FFT (pffft, FFTPACK / BSD-style licence, pinned commit
  in its README.md; the SAME files as `kraken_doa_v2/external/pffft` - keep the
  two copies identical)

## Testing

No formal test suite currently. Manual testing workflow:

1. Start Heimdall: `make run`
2. Open web interface: http://localhost:8070
3. Verify all channels show correlation peaks
4. Monitor lag compensation: All channels should reach CONVERGED state within 30 seconds
5. Monitor phase compensation: Should apply once and remain stable
6. Test RTL-TCP: Connect SDR# or GQRX to `localhost:1234`
7. Test TCP control: `echo '{"command":"get_status"}' | nc localhost 8092`

## Port Reference

- **8070**: Web interface (HTTP + WebSocket)
- **8091**: TCP data server (multi-channel IQ streaming)
- **8092**: TCP control server (JSON commands)
- **1234**: RTL-TCP server (rtl_tcp compatible, selectable channel)
