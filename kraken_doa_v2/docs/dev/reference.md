# Hot paths, development tasks, key files, common issues, testing, dependencies

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

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
beacon or AM carrier parked there). The seed is a flag, not NaN (written for
the `-Ofast` build of then, which assumed no NaNs).

**The packet's arrays are reused** (2026-10-26, performance pass;
`data_receiver.cpp` `take_iq_vector` / `recycle_iq_vectors`): every packet
was a new `RawDataPacket` whose per-antenna `resize(16384)` meant a 128 KB
malloc + zero fill (5 antennas: ~94 MB/s of memset, the pages faulted in
again whenever the allocator returns them) before the conversion overwrote
it. The decimation thread now hands the arrays back when it is done with a
packet (a guard at the top of its loop - nothing keeps a pointer into them:
the VFO pipelines are joined, the FFT copies its channel); the receiver
takes them for the next one (at most 4 packets' worth wait). A 2015
MacBook Air (i5-5350U, Apple clang 14; job scratch doa-dsp/convbench.cpp,
pktbench.cpp, median of 9): conversion + allocation 1.27 -> 0.84 ns per
antenna sample on one thread; the receiver + decimation threads (the
arrays freed on the other thread, as live) 1.88 -> 1.37 - ~0.5 ns x 12
MS/s = ~0.6 % of a core with 5 antennas. Same samples (the conversion is
unchanged). Pi (glibc: 128 KB is the mmap threshold, freed on another
thread) / Windows not measured. Not taken: an integer sum for the DC
mean (bit-identical, 0.84 -> 0.82 ns: noise); per-packet breakdown here
(doa-dsp/fftbench.cpp, µs per packet: conversion of 5 antennas 89, the copy
into the FFT buffer 15, window 22, pffft 16384 151, dB + average 29, the
noise floor's percentiles 43 averaged).

### 2. Decimation Architecture

(The thread-local decimators / coefficient cache this section used to
describe are long gone.) Per VFO a `SharedDecimator`, per antenna a
`ChannelDecimState` (it follows the channel, not the pool thread): the
mixer (in front of, or folded into, the first stage) and a cascade of
`FirStage` block FIRs (every output computed straight from the stage's
buffer - no per-sample window pushes; regular / folded / halfband), split
as `planStages` finds cheapest, a block through the whole cascade in
1024-sample chunks (`DECIM_CHUNK`: the buffers stay in L1), a first stage
whose input isn't mixed in front filtering straight from the block (no
copy), every VFO in ONE pass per antenna (`decimateVfos`: a task per
antenna - per input array - running each VFO's cascade on a chunk while it
is in L1; the first task on the calling thread, the others on ONE
decimation pool shared by every VFO, `decimation_pool()`, cores + 1
threads; each VFO had its own pool of 8 until 2026-10-22: 51 threads with
three VFOs, 32 now). Details + measurements:
*Arbitrary VFO bandwidth* above (`tools/decim_bench.cpp`). Pi 5, one
antenna, hot caches, ns per input sample with the VFO off the tuner centre
(mixer included): ~2.1-3.5 up to 240 kHz, ~3.9-7.3 at 250 kHz - 1.2 MHz,
7.9-9.9 for the full-rate widths 1.2 - 2.4 MHz, 3.2 for 2.4 MHz (the mixer
alone). In the live pipeline a 12.5 kHz VFO with DoA and 5 antennas adds
~6-7 % of a core to kraken_doa (it was ~19 % before the chunking: memory-
bound, see *Arbitrary VFO bandwidth*); in user-space cycles at 2.4 GHz,
from one to three such VFOs ~2.7 % each with the shared pass (4.8 before),
from three to five ~3.1 each (the same before).

### 3. FFT Processing

**Per-Channel FFT Plans (pffft, see *FFT (pffft)*):**
- Each channel has dedicated `ChannelFFTContext` (plan, buffers)
- True parallel execution with zero mutex contention
- 5x parallelism on 5-channel system
- No planning / wisdom file: a pffft plan is made at once (FFTW needed
  `fft_wisdom.dat` + FFTW_MEASURE; a leftover file is unused, gitignored)

```cpp
ctx.plan = kfft_plan_dft_1d(fft_size, ctx.fft_in, ctx.fft_out, KFFT_FORWARD);
kfft_execute(ctx.plan);
```

**Power -> dB per bin (2026-10-22):** `bins_to_db` (fft_processor.cpp: the
spectrum, the beamformed spectrum; beamformer.cpp's SNR FFT the same) with
`fast_db10` (`include/utils/fast_log.hpp`: exponent bits + atanh series,
<= 6e-6 dB from the exact value) on straight runs - the DC-centring wrap is
done as two runs. The old loop wrapped its index with a mask, so it never
vectorized: a libm `log2f` per bin, 8.5 ns on the Pi 5 (its comment's
"hardware log2f / VLOG2" doesn't exist on ARMv8), ~29 % of kraken_doa's CPU
with no VFO decimated (~2.6 % of a core; per tuner FFT'd, so 5x that in
independent mode). Now 2.7 ns a bin on the Pi (glibc would vectorize
std::log2f in the straight loop too: 2.5), 0.39 ns on the dev PC's Ryzen
(MinGW has no vector log2f: 2.6 there) - one implementation, the same values
on every platform. `tools/fast_log_test.cpp` (93.6 M values 1e-15 .. 1e6 +
the three loop shapes timed)

### 4. FM Demodulation

**Hardware atan2f:** Uses ARM NEON internally, faster than any approximation
```cpp
float phase = std::atan2(q, i);  // float overload; hardware accelerated on ARM
```
Use the `std::` overloads, not the C names: libstdc++ before GCC 14 (Debian
12's GCC 12) has no `std::log10f` / `std::atan2f` / `std::sqrtf` ... (C89
functions' `f` forms) - only the C99 ones (`std::log2f`, `std::lrintf`,
`std::fmaf`).

**Liquid-DSP Resampling:** Multi-stage decimation for 48kHz audio output

**AM loops are set in seconds** (`recreateFilters`, re-run on every rate or
mode change): RF AGC tau 50 ms, envelope AGC tau 0.5 s, DC blocker 30 Hz.
Fixed per-sample constants put their corners at kHz at wide VFOs and stripped
the AM audio's lows.

**FM only (DoA and beamforming off):** only the VFOs heard (🔊) run (+ those with a decoder / Transcribe / audio recording), decimating
only the listened-to channel (squelch reads the main FFT, not decimated data),
and the receiver converts only that channel. None of them (every VFO muted,
no decoder): nothing is decimated - until 2026-10-22 that empty list took
the several-VFO branch of the pass (`reserve(size - 1)`: length_error) and
kraken_doa terminated (found while profiling; the build from before
arbitrary widths did the same).

### 5. MUSIC Algorithm

**Eigenvalue Decomposition:** Eigen library with SelfAdjointEigenSolver
- Optimized for Hermitian correlation matrices
- NEON vectorization enabled automatically
- Pre-allocated working matrices

**Snapshot Management:** Circular buffer with power-of-2 size
- Bitmask indexing (no modulo)
- Overlap processing for temporal smoothing

**Performance:** 256 snapshots, 5 channels → 8-12ms total (old Pi 4
figure; see the 2026-10-26 pass below)

**Frame path (2026-10-26, performance pass):** the accumulator is one
contiguous `complex<float>` row per element (it was a `complex<double>`
MatrixXcd filled with a strided per-sample loop); a frame's snapshots are
summed into the covariance straight from it (`covAccumulate`: double sums,
the element count a template parameter so the pair loops vectorize; each
stretch between snapshot boundaries once, times the number of snapshots
holding it - the 1/4 overlaps were summed twice) instead of being copied
into a matrix each (a heap allocation per snapshot), then all into one
combined matrix, for Eigen's rankUpdate. The noise-subspace projection is a
blocked real-arithmetic loop over planar steering vectors cached with them
(`noiseProjection`, also the talker path and the 3D grid, whose 4 MB
E^H S temporary per frame is gone). Measured on a 2015 MacBook Air
(i5-5350U, Apple clang 14, the app's flags), 5-element UCA, one VFO:
240 kHz 3.5 -> 0.5 % of a core (480 -> 70 us a frame, ~7x), 12.5 kHz 0.17
-> 0.03 %, 2.4 MHz 16.3 -> 2.2 % (7.4x), a 3D custom array at 240 kHz 24.2
-> 8.3 %. Same results: covariance within 1.4e-15, eigenvalues 2e-12,
pseudospectrum 2e-13 (relative), bearings and eigenvalue ratio identical,
1 and 2 sources, 0 / 10 / 30 dB SNR (synthetic scenes, HEAD vs new on the
same samples). `-ffp-contract=off` vs the app's flags: no measurable
difference. Pi / Windows not measured. Tested, not taken: a fixed-max-size
(8 x 8) eigensolver (2-5 % of a ~4.7 us solve), explicit `std::fma` in the
covariance loop (2.8x slower: one dependency chain), float accumulation
(the noise eigenvalues need the double sums).

**Beamformer (2026-10-26):** the delay-and-sum / MVDR output loop
(`combineChannels`: weights to plain arrays, element loop unrolled per
element count) and the MVDR covariance (a loop per element pair instead of a
per-sample Eigen outer product): delay-and-sum 2.4x at 240 kHz (22 -> 9 us a
block) and 3.4x at 2.4 MHz (214 -> 63 us), MVDR 6.2x / 4.0x, outputs
bit-identical on the test blocks (same 2015 MacBook Air; Pi / Windows not
measured). Float accumulation would be 1.6x faster again but not
bit-identical (up to 7e-6 relative) - not taken.

**Frame size cap:** `snapshot_length x num_snapshots` <= `MAX_FRAME_SAMPLES`
(65536). The accumulator scales with it (complex<float> since 2026-10-26;
complex<double> before, plus two copies of every frame) - the old
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

A VFO takes any width (*Arbitrary VFO bandwidth* above), so there is no list
to edit for that. `BANDWIDTH_OPTIONS[]` (`include/config.hpp`) is only the
index list older pages / files / scripts use (`SET_DECIMATOR_BW:id:index`,
DECIMATORS field 2) - don't reorder it. The limits are `VFO_BW_MIN_HZ` /
`VFO_BW_MAX_HZ`; the decimation stays integer (no resampling) by a divisor
of `SAMPLE_RATE_HZ` (`decimation_for_bandwidth`); the page's efficient sizes
(`BW_EFFICIENT`) are computed the same way - keep the two alike.

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
- `src/signal_processing/shared_decimator.cpp:decimateVfos()` - every VFO's decimation, a task per antenna (`ChannelPass::chunk` per VFO and chunk; `FirStage::produce`: the FIR kernels)
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
2. Narrow the VFOs (a smaller bandwidth = a higher decimation); between 1.2
   and 2.4 MHz a VFO runs a full-rate filter - 1.2 MHz or 2.4 MHz exactly is
   cheaper (the BW box's tooltip says what rate a width runs at)
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
- (no FFT library: pffft is vendored in `external/pffft`; no
  `libliquid-dev`: liquid-dsp is built from source - below)
- `libeigen3-dev`: MUSIC DoA algorithm (eigenvalue decomposition)
- `libssl-dev`: SSL for WebSocket server (HTTPS required for Web Audio API)
- `libopus-dev`, `zlib1g-dev`, `libsqlite3-dev`: audio, compression, settings
- `build-essential`, `cmake`, `git`, `pkg-config`: Build tools (cmake + git
  also fetch and build liquid-dsp)

**Vendored Dependencies:**
- `uWebSockets/`: HTTP/WebSocket server (auto-installed by Makefile)
- `liquid-dsp/`: filters / decimators / resamplers (MIT), fetched at a
  pinned commit and built as a static library without FFTW / libfec by
  `liquid.mk` (see *liquid-dsp*)
- `external/pffft/`: the FFT (see *FFT (pffft)*)
- `concurrentqueue.h`: Moodycamel lock-free queue (header-only, in `include/`)

**Install All:**
```bash
make deps  # Ubuntu/Debian
```

## Additional Documentation

- `OPTIMIZATION_SUMMARY.md`: Detailed performance analysis and benchmark results
- `AGENTS.md`: Development workflow and contribution guidelines
- `../CLAUDE.md`: suite-wide rules; `../docs/dev/`: the long-form suite notes (suite-reference.md, run-and-deploy.md, windows.md, librtlsdr-fork.md, macos.md, compiler-flags.md, licensing.md)
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

**End to end, before vs after the 2026-10-26 performance pass** (job
scratch 66074be2 e2e/: two TEST builds on moved ports - main d785e1b and
the branch at dc095b1 - fed by a fake heimdall streaming 5 channels x
16384 samples at 146.5 packets/s: a clockwise UCA, R = 250 mm, a CW tone
at +200 kHz from a 60 deg bearing, a 10 dB weaker one at -500 kHz from
200 deg, noise 0.2 FS; bearings from the type-3 pseudospectrum, CPU =
user + sys over 60 s after 15 s settling, 5 alternating rounds each, a
2015 MacBook Air i5-5350U, Apple clang 14). Process CPU, % of a core,
old -> new: one 12.5 kHz NBFM VFO with DoA 31.8 -> 30.2 (-5 %), a 240 kHz
WBFM VFO 45.3 -> 37.0 (-18 %), a 2.4 MHz VFO at +100 kHz 111.4 -> 56.3
(-49 %), 12.5 kHz + delay-and-sum beamforming 32.5 -> 30.6 (-6 %). The
same MUSIC frame and message counts per window, the same bearing to
0.01 deg (59.997 / 59.993 / 59.76 - the last biased by the second tone
inside the wide VFO with 1 source set - identical in both builds), stderr
empty, no drops. Pi / Windows not measured.
