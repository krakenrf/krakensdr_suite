# Heimdall DoA Client (`kraken_doa`)

A real-time **FFT viewer, FM receiver, and MUSIC direction-finding (DoA) processor**
for coherent multi-channel RTL-SDR arrays (KrakenSDR-class hardware). It connects to
a running **Heimdall server**, receives phase-synchronized IQ from all antenna
channels, and serves a browser UI (`kraken_doa.html`) for spectrum visualization,
audio, beamforming, scanning, and bearing estimation.

```
  RTL-SDR array ──► Heimdall server ──TCP──► kraken_doa (this app) ──WSS──► Browser UI
                    (coherent IQ)    8091/8092                       8080
```

This client does the signal processing (decimation, FFT, FM demod, MUSIC, beamforming)
and pushes results to the browser. The Heimdall server owns the radios and the
phase/lag calibration; this client never touches the hardware directly — it asks the
server to retune/regain over a control socket.

---

## How it connects to Heimdall

The client opens **two TCP connections** to the Heimdall server (default `localhost`):

### Data socket — port `8091` (IQ stream, server → client)
`data_receiver_thread` (`src/networking/data_receiver.cpp`) connects, then reads a
continuous stream of length-framed packets. Each packet is:

```
[ 32-byte base header ] [ 8 bytes × N channels metadata ] [ 2 × N × samples uint8 IQ ]
```

Base header (big-endian), magic `0x4D434851` = `'MCHQ'`:

| field | meaning |
|-------|---------|
| `magic` | frame sync `'MCHQ'` |
| `channels` | number of antenna channels (5) |
| `samples_per_channel` | IQ samples in this packet |
| `phase_compensation_state` | server calibration state |
| `noise_source_active` | cal noise injection flag |
| `frequency_change_counter` | bumps on every retune (used to drop stale data) |
| `current_group_index` / `retuning_in_progress` | retune coordination |

Per-channel metadata carries each channel's `frequency_hz` and `gain_db`. IQ samples
are interleaved `uint8` I/Q pairs, converted to `complex<float>` by
`IQConverter::convert_uint8_to_complex_float()` (compiler-auto-vectorized, DC
correction on). The socket uses a 500 ms `SO_RCVTIMEO` so shutdown stays responsive;
if the connection drops it retries every second and flushes any partial packet.

### Control socket — port `8092` (commands, client → server)
`send_control_command()` lazily opens a second socket and sends **newline-free JSON**,
one command per write (a mutex serializes concurrent senders). Browser actions that
need the radio to change are translated here, e.g.:

```json
{"set_frequency":{"frequency":100000000,"channel":0}}
{"set_gain":{"gain":49.6,"channel":0}}
{"set_wideband_mode":{"enable":true}}
```

If a send fails the socket is dropped and reopened on the next command.

### Browser socket — port `8080` (HTTPS / WSS)
`web_server_main()` (uWebSockets) serves `kraken_doa.html` over **HTTPS with a
self-signed cert** (`server.crt`/`server.key`) — HTTPS is required because the Web
Audio API needs a secure context. Open `https://<host>:8080` and accept the cert.
A plain-HTTP companion endpoint runs on `8081` (`DOA_value.html`, for the Android app).

The browser receives **binary** frames tagged by a leading `uint32` type:

| type | payload | rate |
|------|---------|------|
| `0` | FFT spectrum (compressed min/max envelope + metadata) | ~50 ms |
| `1` | Opus-encoded FM audio (48 kHz, 40 ms packets) | ~40 ms |
| `3` | per-decimator MUSIC DoA spectrum (+ elevation for 3D arrays) | ~200 ms |

plus periodic JSON system status, and `{"sync_cmd":...}` echoes so multiple open
browsers stay in sync. The browser sends back short text commands (`FREQ:100.5`,
`GAIN:40`, `DOA:1`, `UI:KEY:value`, …) handled in `src/control_handler.cpp`.

---

## Sidebar features

The collapsible left sidebar groups all controls. Top bar (always visible) has the
**center-frequency dial, gain slider, FFT channel selector, audio on/off + volume**,
live hardware stats, and status badges (connection / coherent mode / calibration /
cooldown / scanner).

| Section | What it does |
|---------|--------------|
| **⚙️ Decimators** | Add/remove **virtual decimators**. Each is an independent tuned "receiver" with its own frequency offset, bandwidth, and demod mode — run a narrowband DoA search on one while FM audio plays from another. |
| **🎯 MUSIC DoA** | Toggle DoA; pick array **topology** (UCA / ULA / Custom with per-element X/Y/Z positions → 3D arrays unlock an elevation spectrum); set **radius/spacing**; **FB averaging** (ULA only) to decorrelate coherent multipath; an **Averaging** slider (exponential covariance smoothing across updates) for steadier bearings on weak signals; **Signal Sources** 1–4 or **Auto** (per-frame eigenvalue estimate); and tune **snapshots** and **snapshot length**. Bearings are read out with sub-degree precision via parabolic peak interpolation on the fixed 1° grid. |
| **📡 Beamforming / Diversity** | Coherently combine all channels for ~7 dB SNR gain. Modes: **Delay-and-Sum**, **Freq-Domain DAS**, **MVDR** (adaptive null-steering, with regularization + condition number), and **Selection Diversity** (pick best-SNR channel, anti-multipath). Auto-steers to the DoA bearing or use **manual steering**. Shows live steering angle, confidence, and SNR boost. |
| **📻 Wideband Mode** | Spread all tuners across the full 2.4 MHz for wide spectrum viewing (DoA disabled). Drag the spectrum to retune. Required for the discrete Scanner. |
| **🖥️ Display** | Spectrum **averaging** (smoothing) and **edge clip** (trim filter roll-off at band edges). |
| **🔇 Squelch** | Per-decimator signal-present detection (in each decimator's panel): choose **FFT Peak** (magnitude vs dB threshold), **Eigenvalue** (λ₁/λ̄ ratio from that decimator's MUSIC covariance, log-scale threshold up to 100k), or **Auto Eigenvalue** (threshold self-learned from the ratio's noise floor; learning pauses while the FFT shows a visible in-band signal, so a continuous transmission opens the squelch by default instead of teaching itself closed, while elevated coherent noise floors are learned and squelched; relearns when the VFO moves). The λ readout shows the instantaneous ratio plus a 5 s peak hold so bursty signals are catchable — gates audio, scanner dwell, and the DoA output (a closed squelch freezes the bearing display/logs at the last open frame in every method, beamforming included; DoA is also frozen automatically whenever heimdall runs a calibration). |
| **📊 FFT Settings** | FFT **size** (1024–65536) and display **downsampling** factor; shows resulting points-sent. Bigger = finer resolution, more CPU. |
| **📻 Scanner** | Discrete frequency-list scanner (needs wideband mode). Load/save/new scan files, per-frequency enable, squelch, dwell time; locks onto and reports active frequencies. Start / Next controls. |
| **🔍 Continuous Scanner** | Sweeps for active signals automatically — either within the current bandwidth or across a **wideband MHz range** (e.g. 88–108). Adjustable squelch, dwell, and signal-decay; lists tracked signals as they appear. |

The main area shows the **spectrum + waterfall** (click/drag the tuning bar to set the
listen frequency; waterfall has speed, colormap, and min/max/auto-range controls) and
the **MUSIC DoA panel** (compass-style bearing display, one per active decimator).

---

## How the code works

Pipeline (see `src/networking/data_receiver.cpp` for the hot path):

```
TCP 8091 ─► IQ convert (uint8→complex<float>) ─► raw data buffer (bounded, auto-cleanup)
   │
   ├─► decimation_processor_thread ─► per-decimator decimation (thread-local, cached coeffs)
   │        │
   │        ├─► 8× per-channel FFT workers (FFTW3, per-channel plans, true parallel) ─► spectrum
   │        ├─► fm_processor_thread ─► FM demod + Opus ─► 48 kHz audio
   │        ├─► doa_processor_thread ─► MUSIC (Eigen) ─► bearings
   │        └─► beamformer (DAS / MVDR / diversity)
   │
   └─► WebSocket server (8080) ─► browser
```

**Threading** (`src/main.cpp` startup order): FFTW wisdom + beamformer init → data
receiver → decimation processor → 8 FFT workers → FM processor → web server (8080) →
DoA HTTP server (8081) → status monitor. ~23–28 threads total — intentional
oversubscription on 4 cores, since most are I/O-bound.

**Key design choices** (details in `OPTIMIZATION_SUMMARY.md`, `CLAUDE.md`):
- Compiler auto-vectorization for IQ conversion (beats hand-written NEON here).
- Thread-local decimators with a shared coefficient cache (zero alloc in hot path).
- Per-channel FFTW plans + on-disk wisdom (`fft_wisdom.dat`) for instant startup.
- Lock-free `moodycamel::ConcurrentQueue` between stages; `mallopt(M_ARENA_MAX,32)`.

### Source layout
```
src/networking/   tcp_client, data_receiver, websocket_server, binary_message
src/signal_processing/  fft_processor, fm_demodulator, music_processor,
                        beamformer, shared_decimator
src/utils/        iq_converter, raw_data_buffer, ring_buffer, system_stats
src/              channel_manager, decimator_manager, scanner_manager,
                  continuous_scanner, control_handler, message_builders,
                  main.cpp
include/config.hpp   all compile-time constants
kraken_doa.html      browser UI (loaded at runtime — edit & refresh, no rebuild)
```

---

## Build & run

```bash
make            # build (auto-installs uWebSockets); use -j3 MAX on a Pi (never -j4)
make run        # build and run
make rebuild    # clean + build
make deps       # install system deps (Ubuntu/Debian)
make status     # check deps / build state
```

**Dependencies:** `libfftw3-dev`, `libliquid-dev`, `libeigen3-dev`, `libssl-dev`,
`build-essential`, `git`, `pkg-config`. uWebSockets is vendored/auto-installed.

**Usage:**
1. Start the Heimdall server first and let phase calibration converge (~60 s).
2. `make run` here.
3. Open `https://<host>:8080`, accept the self-signed cert.
4. Tune via the frequency dial, enable DoA/FM/beamforming as needed.

> The UI loads `kraken_doa.html` at runtime, so UI tweaks just need a browser refresh
> (Ctrl+Shift+R) — no rebuild. Editing C++ or `include/config.hpp` requires `make`.

---

## Configuration (`include/config.hpp`)

| Constant | Default | Notes |
|----------|---------|-------|
| `WEB_PORT` | `8080` | Browser UI (HTTPS/WSS) |
| `DOA_HTTP_PORT` | `8081` | Plain HTTP `DOA_value.html` (Android app) |
| `TCP_DATA_PORT` | `8091` | Heimdall IQ stream |
| `TCP_CONTROL_PORT` | `8092` | Heimdall control (JSON) |
| `SAMPLE_RATE` | `2.4e6` | Must match server |
| `FFT_SIZE` | `16384` | Default; runtime-selectable in UI |
| `MAX_CHANNELS` / `DOA_NUM_ELEMENTS` | `5` | Antenna channels |
| `AUDIO_SAMPLE_RATE` | `48000` | **Must match the browser's native rate** (see console) |
| `BANDWIDTH_OPTIONS[]` | 2.4 MHz → 1 kHz | Integer decimation only, no resampling |

For deeper architecture and tuning notes, see `CLAUDE.md`, `OPTIMIZATION_SUMMARY.md`,
and the root `../CLAUDE.md` (full server + client system).
