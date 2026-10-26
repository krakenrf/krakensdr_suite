# heimdall architecture, data flow, threads, debugging, dependencies

Moved verbatim from `heimdall_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every heimdall session needs). Not loaded automatically. *Italic*
names refer to sections of the other files in this folder (index:
`README.md`); "above" / "below" meant elsewhere in the old file.

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
   - Conversion and the 8-tap lag FIR run in ONE pass, in blocks of 256 samples through a small buffer that carries the FIR's 7-sample history (2026-10-26; before: the whole 16384-sample chunk into a 128 KB thread-local intermediate, then a second pass), and uint8 -> float is the arithmetic `(u - 127.5) * SCALE` - the same two float operations the old 256-entry `iq_lut()` table was built with, so the same values, but it vectorizes and the table lookups didn't (`iq_lut()` is gone). Micro-benchmark (job scratch 66074be2 heimdall/conv, 5 channels x 16384, the tails carried; a 2015 MacBook Air i5-5350U, Apple clang 14, medians of interleaved runs): 2.9-3.9 -> 1.6-2.2 ns/sample (1.7-1.9x), i.e. ~3.5 -> ~1.9 % of a core at 2.4 MS/s x 5 channels; output and tails bit-identical (also with a retrim, a 5-sample chunk, spectral inversion, and under -ffp-contract=on / fast / off). Tested, not taken: fusion with the table kept (1.3x), arithmetic conversion without fusion (1.45x), 1024-sample blocks (no better than 256). Pi / Windows not measured.
   - Runs at **normal** priority on purpose: it may fall behind safely (whole sets drop upstream at L2-raw) and at realtime priority its malloc/mutex use would priority-invert against the web/correlation threads.

4. **Correlation Processor** (`correlation_processor` in correlation.cpp):
   - Consumes L2 (drain-to-newest); FFTs all channels; cross-correlates vs the reference.
   - Extracts lag/phase via Gaussian interpolation; updates `CorrelationResult`; broadcasts via WebSocket. Per-set cost and the 2026-10-26 rework of the lag fit: `calibration.md` (*Correlation pass arithmetic*).

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
