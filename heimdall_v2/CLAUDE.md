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
# First install uWebSockets
git clone --recursive https://github.com/uNetworking/uWebSockets
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
- `fft_plan.{hpp,cpp}`: RAII FFT wrappers (FFTForward, FFTInverse) and thread-safe FFT plan pool
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
   - On staging overflow it drops the oldest **whole aligned set** (coherence-safe). The drain also OWNS the L1 flush (recovery / retune): `clear_l1_buffer()` posts a request that the drain serves between sets, dropping any partial set, and `flush_l1_now()` waits for a quiet point (every active device's `last_arrival_ns` - stamped at the END of the callback, after the enqueue - within half a packet period of the others and of now) so each device loses the same round of packets. A stamp taken before the enqueue let one device keep a packet the others lost: a whole-packet slip the lag servo then had to slew off. A persistently stalled device trips `STUCK_DEVICE_MAX_TIMEOUTS` → `signal_coherence_lost()`.

3. **Conversion Worker** (`conversion_worker` in sdr_pipeline.cpp):
   - Consumes `l2_raw_buffer` **in order**; converts uint8 IQ → complex with phase/lag/EQ compensation (`samples_to_complex_with_compensation`, per channel, written in place into a recycled `l2_buffer_pool` set).
   - Broadcasts **every** set to the TCP servers (tcp_data_server, rtl_tcp_server), then pushes the converted set to the L2 global queue (`l2_buffer`).
   - Runs at **normal** priority on purpose: it may fall behind safely (whole sets drop upstream at L2-raw) and at realtime priority its malloc/mutex use would priority-invert against the web/FFTW threads.

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

### Coherence-Loss Detection and Recovery

**Invariant**: coherence requires that the N-th sample set from every device is the same instant. The dongles are sample-locked (shared clock); calibration only removes a small sub-sample/few-sample offset. A single dropped USB packet shifts one channel by a full `NUM_SAMPLES` — far outside what lag/phase compensation can represent — and silently destroys coherence. All buffer dropping must therefore happen at the **aligned** stage (whole sets, all channels) and **never per-device**.

**Detection** (`signal_coherence_lost()` in sdr_pipeline.cpp sets the `coherence_lost` flag, bumps `coherence_event_count`):
- RTL callback: L1 overflow / sample-pool exhaustion (sdr_device.cpp)
- Sample drain: a device stalled past `STUCK_DEVICE_MAX_TIMEOUTS`
- Optional streaming monitor (`maybe_run_coherence_monitor`, default OFF — see config)

**Recovery** (`coherence_watchdog` thread → `recover_coherence()` in compensation.cpp):
- Re-engages the noise source, resets lag to MEASURING + flushes L1/L2/L2-raw, resets phase to WAITING_FOR_LAG_COMPLETION (identity vector, per-bin dropped), re-enables FFT — i.e. a full startup-equivalent recalibration (the only recovery robust to an arbitrary full-packet slip).
- `recovery_in_progress` spans the whole async recal (cleared in `complete_phase_calibration_locked`), and is surfaced in the control-port status (`coherence_events`, `recovering`).
- Debounce: an event that fires **during** a recovery is peeked (not consumed) and acted on once the current recovery completes; a recovery that never converges is aborted after 120 s with the noise source switched off and FFT disabled so it can't latch a no-noise-source calibration.
- Flush-only (no recal) in wideband scan or while the coherent discrete scanner is active (a recal can't converge while hopping). Likewise `begin_retune_cooldown()` and `handle_settings_change()` are no-ops in wideband scan (the phase stages skip scan mode, so a recal would leave the noise source on across the scan; leaving the scan recalibrates anyway), and a scan-mode retune from the web UI (`SDR_SETTINGS`) or control port (`set_frequency`) re-spreads the tuners via `setup_wideband_frequencies()` instead of tuning them all to one frequency. The retune/gain cooldown handlers (web_server.cpp, tcp_control_server.cpp) and `handle_settings_change` skip their phase-only override while `recovery_in_progress` so they don't clobber the recovery.

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
- The ONLY calibration trigger is FORCE_RECAL (web UI button, confirm
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

## Dependencies

**KrakenSDR librtlsdr fork** (github.com/krakenrf/librtlsdr), linked
STATICALLY from `../librtlsdr/build/src/librtlsdr.a` with the fork's own
headers (`-I../librtlsdr/include`). A distro `librtlsdr0` sits earlier in the
loader's search path than `/usr/local/lib`, so a dynamic link could silently
load the stock library; linked in, it can't (`ldd heimdall` shows no
librtlsdr). `make` clones/builds the static library if it's missing; do NOT
install `librtlsdr-dev` (distro rtl-sdr packages may stay installed).
`init_rtlsdr_device()` fails a channel whose sample rate, center frequency or
USB buffer reset can't be set (a silently mistuned dongle breaks coherence);
other setup calls only warn.

**System Libraries:**
- `libusb-1.0-0-dev`, `cmake`: build and link the librtlsdr fork
- `libfftw3-dev`: Fast Fourier Transform (single-precision `fftw3f`)
- `libeigen3-dev`: Eigenvalue decomposition for phase calibration
- `libssl-dev`, `libcrypto++-dev`: SSL/TLS for uWebSockets (optional)
- `build-essential`, `git`, `pkg-config`: Build tools

**Vendored Dependencies:**
- `uWebSockets/`: Cloned and built automatically by Makefile (HTTP/WebSocket server)

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
