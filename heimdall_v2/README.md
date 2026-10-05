# Heimdall

Heimdall is a C++20, real-time **phase-coherent receiver** for an array of RTL-SDR
dongles (a KrakenSDR-style 5-channel setup). It keeps every channel sample- and
phase-aligned against a shared noise source, then streams the synchronized
multi-channel IQ — plus live correlation/calibration telemetry — to a browser UI,
a downstream DoA client, and standard SDR tools.

It is the back half of a two-part system: this server handles the hardware,
coherence, and calibration; the **DoA client** (`../kraken_doa_v2/`) consumes the IQ
stream and runs FFT/FM/MUSIC direction finding.

---

## What it does

- **Coherent multi-RTL-SDR capture** — N dongles sharing one clock, read on
  per-device realtime USB threads with an allocation-free callback.
- **Automatic calibration** — closed-loop lag (sample-timing) alignment and
  eigenvalue-based phase alignment against a switched noise source, with an
  optional per-frequency-bin equalizer for band-wide coherence.
- **Coherence-loss detection & self-recovery** — a watchdog detects USB drops /
  overload and runs a full recalibration without manual intervention.
- **Multiple output interfaces** — browser UI (8070), raw multi-channel IQ
  (8091), JSON control (8092), and an `rtl_tcp`-compatible single channel (1234).
- **Operating modes** — coherent (all tuners locked together) and wideband /
  discrete-scanner (tuners spread across the spectrum for fast scanning).
- **ARM-first** — NEON vectorization on Raspberry Pi 4/5, scalar fallback on x86_64.

---

## Quick start

```bash
make deps      # install librtlsdr-dev, libfftw3-dev, libeigen3-dev, libssl-dev, build tools
make           # build (auto-clones & builds uWebSockets)
make run       # build and run
```

Then open **http://localhost:8070**.

> **librtlsdr fork required.** Fine lag steering uses
> `rtlsdr_set_sample_freq_correction_f()`, which only exists in the **krakenrf
> librtlsdr fork**. With stock librtlsdr the coarse path still works but sub-PPM
> servoing will not.

Useful targets: `make rebuild`, `make clean`, `make status`, `make debug`,
`make distclean`. A CMake build also exists (`mkdir build && cd build && cmake .. && make`).

All compile-time settings live in **`config.h`** (device count, frequency, sample
rate, gain, ports, buffer/realtime tuning). See [Configuration](#configuration).

---

## How the code works

### Module layout (`src/`)

```
main.cpp            orchestration: starts threads & servers, owns shutdown
core/               types, config, logging, lock-free buffer pool, settings persistence, utils
sdr/                device enumeration/init, per-device L1 buffers + RTL callback, the ingest pipeline
dsp/                FFT plan pool, cross-correlation + peak detection, lag/phase compensation, watchdog
net/                tcp_data (8091), tcp_control (8092), rtl_tcp (1234) servers
web/                uWebSockets HTTP + WebSocket server (8070), HTML loader
```

Dependencies flow one way: `web → {net, dsp, sdr, core}`, `net/dsp → {sdr, core}`,
`sdr → core`, `core` depends on nothing.

### Ingest pipeline (the L1 → L2-raw → L2 split)

The hot path is deliberately split so USB keep-up never depends on DSP load —
the usual cause of silent sample loss under CPU pressure.

1. **RTL callback** (per-device USB thread, realtime, **alloc-free**) → pushes raw
   byte buffers into that device's **L1** lock-free queue. On L1 overflow or pool
   exhaustion it does **not** drop one device's buffer (that would silently break
   coherence) — it signals *coherence lost* so the watchdog can recover cleanly.
2. **Sample drain** (`sample_processor`, realtime) → pulls one block from *every*
   device in lockstep into an aligned set and hands it to the **L2-raw** staging
   queue. Cheap: no conversion here. Overflow drops whole aligned sets (coherence-safe).
3. **Conversion worker** (`conversion_worker`, normal priority) → converts uint8 IQ
   → complex with lag/phase/EQ compensation applied, broadcasts every set to the
   TCP/RTL-TCP servers, then pushes to the **L2** queue. Runs at normal priority on
   purpose: it may fall behind safely, and at realtime it would priority-invert
   against the web/FFTW threads.
4. **Correlation processor** → consumes the *newest* L2 set (drains backlog so
   readings are ~10 ms fresh), FFTs all channels, cross-correlates against the
   reference, and broadcasts correlation + state over WebSocket.
5. **Per-channel compensation threads** + **coherence watchdog** run the
   calibration state machines and recovery.

> **Coherence invariant:** the N-th sample set from every device must be the same
> instant (the dongles are sample-locked). A single dropped USB packet slips one
> channel by a full `NUM_SAMPLES` — far beyond what compensation can represent — so
> **all buffer dropping happens at the aligned (whole-set) stage, never per-device.**

### Threads

Main (uWebSockets loop) · per-device USB readers (RT) · sample drain (RT) ·
conversion worker · correlation processor · per-channel compensation ·
coherence watchdog · discrete scanner · TCP status broadcaster (2 Hz) ·
data/control/RTL-TCP server threads.

---

## Calibration system

Coherent DoA requires every channel **time-aligned (lag)** and **phase-aligned**.
Calibration runs against the array's built-in **noise source** (a bias-tee-switched
broadband diode) in two stages — **lag first, then phase** — and is triggered at
startup, after a frequency/gain change (phase-only, faster), and after a detected
coherence loss (full).

A **150 ms noise-settle gate** (`NOISE_SETTLE_MS`) discards readings right after the
noise source switches on, to flush pre-noise samples still in the USB ring and skip
the diode turn-on transient.

### 1. Lag measurement (`dsp/correlation.cpp`)

Each non-reference channel is cross-correlated against the reference. Lag is read in
two parts:

- **Integer part** — the Gaussian-interpolated cross-correlation peak (~±0.15 samples).
- **Sub-sample part** — a weighted least-squares fit of the cross-spectrum *phase
  ramp*: for a pure delay every bin's phase advances linearly with frequency, so the
  slope gives the fractional delay with far less bias than peak interpolation. Two
  subtleties: the constant per-boot inter-channel phase is removed first (else
  `atan2` wrapping corrupts the slope), and the regressor is **physical frequency**
  (a V across FFT bins: `k` lower half, `k − N` upper half), not bin index.

### 2. Lag compensation (`dsp/compensation.cpp`)

A per-channel state machine drives lag to zero. The clock is steered with
`rtlsdr_set_sample_freq_correction_f()` (krakenrf fork) — writes only the RTL2832
resampler-offset registers, no tuner-PLL retune. Resolution ≈ one register count ≈
0.06 PPM (2⁻²⁴).

| Stage | Region | Action |
|-------|--------|--------|
| `BURSTING` | \|lag\| ≥ 6 | Coarse open-loop PPM bursts bring lag into the fine regime (the peak smears at high PPM). |
| `SERVOING` | \|lag\| < 6 | Continuous proportional servo on the **median of 10 readings** (~4 Hz). Rate tiers: 16 counts > 2 samples, 4 counts > 1, then **1 count** (hardware minimum) < 1 sample. The servo also *learns* the random ±0.2–0.3-sample kick each register write produces. |
| Digital trim | \|residual\| < 0.75 | Final sub-sample residual folds into a per-channel **8-tap windowed-sinc fractional-delay FIR** in software — because every hardware write kicks the lag, the finest alignment must never touch hardware. |
| `CONVERGED` | \|lag\| < 0.02 | Locked after two quiet windows. |

All channels run the fractional-delay FIR at all times (a unit delta when idle) so
they share an identical base group delay.

### 3. Phase compensation (`dsp/compensation.cpp`, `dsp/correlation.cpp`)

Once all channels are lag-converged, phase is calibrated by **eigenvalue
decomposition** of the spatial correlation matrix `Rxx = X·Xᴴ`. The dominant
eigenvector captures each channel's complex gain (phase + amplitude mismatch); its
inverse, normalized to the reference, is the correction vector. Each channel is then
multiplied by `amplitude · e^(−jφ)` in the hot path (lock-free atomic vector).

State machine: `WAITING_FOR_LAG_COMPLETION` → `MEASURING_INITIAL_PHASE` (average
`required_stable_readings`) → `APPLYING_COMPENSATION` → `VERIFYING_CONVERGENCE`
(all channels < 1° for several cycles; re-apply up to a limit) → `CONVERGED` → noise
source off, FFT auto-disabled. A frequency change restarts this after a short
stability cooldown.

### 4. Optional: per-bin (frequency-dependent) EQ

Scalar phase compensation aligns each channel at the band *center* only; residual
IF-filter group-delay differences and sub-sample lag leave a few degrees of error at
the band edges. The optional **Per-Bin Phase Calibration** mode (web UI checkbox,
**OFF by default**) fixes this band-wide: after scalar convergence it accumulates the
per-bin cross-spectrum `⟨Xᵢ(f)·conj(X_ref(f))⟩` over an extra window (noise still on),
designs a short complex **phase-only equalizing FIR** per channel
(`FIR_LEN = 32` taps, SNR-gated, reference = pure delta so all keep a common group
delay), and applies it as the final pipeline stage. Off by default because it adds a
per-channel complex FIR to the hot path — enable only on faster hardware (e.g. Pi 5).

The toggle is **persisted** in `heimdall_settings.conf` (a `key=value` file in the
working dir, read at startup by `core/settings.cpp`); a saved "on" rebuilds the
equalizer automatically during the first calibration.

> **Half-buffer offset:** the correlation FFT transforms the reference at a CS/2
> offset, multiplying its spectrum by `(-1)ᵏ`. The per-bin accumulation cancels this
> so the designed impulse lands at the intended group-delay tap (logged as
> "impulse peak at tap N").

### 5. Coherence-loss detection & recovery

**Detected by:** L1 overflow / sample-pool exhaustion (callback), a device stalled
past `STUCK_DEVICE_MAX_TIMEOUTS` (~3 s) in the drain, and an optional low-rate
streaming cross-correlation monitor (`ENABLE_COHERENCE_MONITOR`, **default OFF** —
heuristic, needs on-hardware threshold tuning). Any detector calls
`signal_coherence_lost()`, latching a flag and bumping `coherence_event_count`.

**Recovered by** the `coherence_watchdog` thread → `recover_coherence()`: re-engage
the noise source, flush L1/L2/L2-raw, reset lag to `MEASURING` and phase to
`WAITING_FOR_LAG_COMPLETION`, re-enable FFT — a full startup-equivalent recal (the
only recovery robust to an arbitrary full-packet slip). `recovery_in_progress` is
surfaced in the control-port status; a recovery that never converges is aborted after
120 s (noise source off). While a wideband/discrete scan is active it flushes only
(a recal can't converge while hopping).

### Key tunables (`core/types.hpp`, `config.h`)

| Field | Default | Meaning |
|-------|---------|---------|
| `slide_threshold` | 6.0 | Lag below which servo (not bursts) is used |
| `servo_max_counts` | 16 | Register-count rate ceiling (~0.95 PPM) |
| `lag_avg_window` | 10 | Readings per median control update |
| `slide_convergence_threshold` | 0.02 | Lag lock target (samples) |
| `convergence_threshold_degrees` | 1.0 | Phase lock target |
| `NOISE_SETTLE_MS` | 150 | Discard window after noise-source on |
| `PerBin FIR_LEN / REQUIRED_SNAPSHOTS / SNR_GATE` | 32 / 32 / 0.05 | Per-bin equalizer length, averaging, low-SNR gate |

---

## Interfaces & API

| Service | Port | Protocol | Purpose |
|---------|------|----------|---------|
| **Web UI** | **8070** | HTTP + WebSocket | Browser control & live visualization (`WEB_PORT` in `config.h`) |
| **TCP data** | **8091** | binary | Synchronized multi-channel IQ stream (FFT/DoA clients) |
| **TCP control** | **8092** | line JSON | Programmatic control + 2 Hz status broadcast |
| **RTL-TCP** | **1234** | `rtl_tcp` | One selectable channel for SDR#/GQRX/SDR++/CubicSDR |

### Web interface (port 8070)

uWebSockets serves `index.html` (loaded at runtime — edit and refresh, no rebuild)
on `GET /`, and a WebSocket that **broadcasts a binary correlation/state frame every
50 ms (20 Hz)**: a header (num elements, correlation size, ref channel, compensation
active, bias-tee state, per-bin enabled), current frequency/gain/RTL-TCP channel,
then per-channel scale factors, lags, phases, compensation state, and decimated
correlation traces. The UI gives frequency/gain control, bias-tee toggle, per-bin
calibration toggle, RTL-TCP channel selection, and live correlation/compensation
plots.

**Incoming WebSocket commands (text):**

| Message | Effect |
|---------|--------|
| `FFT_ENABLE` / `FFT_DISABLE` | Enable/disable FFT processing (clears the post-cal auto-disable) |
| `BIAS_TEE_ENABLE` / `BIAS_TEE_DISABLE` | Switch the noise source on/off (all devices) |
| `PER_BIN_ENABLE` / `PER_BIN_DISABLE` | Toggle per-bin phase calibration (persisted) |
| `SDR_SETTINGS:{"frequency":F,"gain":G}` | Set center frequency / gain with cooldown handling |
| `RTL_TCP_CHANNEL:<n>` | Select which channel the RTL-TCP server streams |

### TCP control (port 8092, JSON)

Newline-delimited JSON commands; the server also **broadcasts a status JSON at 2 Hz**
with `settings` (center_freq, gain, sample_rate, rtl_tcp_channel), `num_channels`,
`operating_mode` (`coherent`/`wideband`), `wideband_enabled`, `cooldown_active` /
`cooldown_remaining_ms`, and the coherence fields `coherence_events` + `recovering`.

```bash
echo '{"command":"set_frequency","frequency":433000000}' | nc localhost 8092
```

| Command | Fields | Purpose |
|---------|--------|---------|
| `set_frequency` | `frequency` (Hz) | Retune all tuners (coherent mode) |
| `set_gain` | `gain` (dB, `-1`=auto) | Set gain on all channels |
| `set_rtl_tcp_channel` | `channel` | Choose RTL-TCP source channel |
| `set_stability_delay` | `delay_ms` | Override retune cooldown |
| `set_operating_mode` | `mode` (`coherent`/`wideband`/`independent`) | Switch the operating mode (wideband spreads the tuners around the current frequency; coherent retunes all and recalibrates) |
| `set_independent_tuner` | `channel`, `frequency?` (Hz), `gain?` (dB, `-1`=auto) | Independent mode: tune one tuner / set its gain |
| `set_wideband_mode` | `enable`, `base_frequency?` | Enter/leave wideband scan mode (older form of `set_operating_mode`) |
| `set_wideband_frequencies` | `base_frequency` | Per-tuner spacing in wideband |
| `set_wideband_edge_clip` | `edge_clip` (0.1–1.0) | Usable bandwidth fraction/tuner |
| `get_wideband_status` | — | Query wideband coverage/frequencies |
| `reset_lag_compensation` | — | Force lag back to MEASURING |
| `configure_scanner` | `frequency_groups[]`, `dwell_time_ms` | Set up discrete scanner |
| `start_scanner` / `stop_scanner` | — | Run/stop frequency hopping |
| `get_scanner_status` | — | Query scanner state |

### TCP data (port 8091, binary)

Each broadcast packet is a header followed by interleaved IQ (channel 0's samples,
then channel 1's, …), one `uint8` per I and Q (128 = zero):

```
magic 'MCHQ' (0x4D434851) | num_channels | num_samples | phase_state
| noise_source_active | freq_change_counter | current_group_index | retuning_in_progress   (big-endian uint32 ×8)
per channel: ch_frequency_hz, ch_gain_db   (little-endian float ×2)
then IQ bytes...
```

Total = `32 + num_channels·8 + num_channels·num_samples·2` bytes. `phase_state`
mirrors the phase compensator enum so clients know when the stream is fully
calibrated; its high bits are flags (consumers mask the low byte): 0x100
KerberosSDR manual calibration, 0x200 calibration stale, 0x400 wideband scan
mode, 0x800 independent mode. In wideband / independent mode each channel
carries its own frequency, in independent mode also its own gain.

### RTL-TCP (port 1234)

Standard `rtl_tcp` server (sends the `RTL0` dongle-info header, accepts the usual
client commands) exposing **one selectable channel** of the array as a uint8 IQ
stream — point GQRX/SDR#/SDR++ at `localhost:1234`. Pick the channel via the web UI
or the `set_rtl_tcp_channel` control command.

---

## Configuration

Edit **`config.h`** and rebuild (`make rebuild`). Highlights:

| Setting | Default | Meaning |
|---------|---------|---------|
| `NUM_DEVICES` | 5 | RTL-SDR dongles |
| `REF_CHANNEL` | 0 | Reference channel index |
| `CENTER_FREQ` | 100 MHz | Startup RF center frequency |
| `SAMPLE_RATE` | 2.4 MSPS | Sample rate |
| `GAIN` | 496 | Gain in tenths of dB (49.6 dB) |
| `NUM_SAMPLES` | 16384 | Samples per FFT (power of 2) |
| `WEB_PORT` | 8070 | Web interface port |
| `ENABLE_BIAS_TEE` | 1 | Bias-tee / noise source on all devices |
| `RTL_USB_BUF_COUNT` | 32 | librtlsdr async USB buffers (~218 ms cushion) |
| `L2_RAW_MAX` / `L2_RAW_CAL_MAX` | 16 / 4 | Staging depth (steady-state / during cal) |
| `STUCK_DEVICE_MAX_TIMEOUTS` | 30 | Drain timeouts before declaring coherence lost |
| `ENABLE_COHERENCE_MONITOR` | 0 | Streaming coherence backstop (heuristic, off) |
| `RT_PRIO_USB_READER` / `RT_PRIO_SAMPLE_DRAIN` | 20 / 15 | SCHED_RR priorities (best-effort) |

**Device mapping:** devices are enumerated by serial number (`EXPECTED_SERIALS` in
`core/config.hpp`, `DEVICE_MAP` in `config.h`) so channel order is stable across
reboots regardless of USB enumeration order.

**Realtime scheduling (optional):** USB readers and the drain use `SCHED_RR` if the
process is granted it; otherwise they log once and run at normal priority. To enable,
add `<user> - rtprio 30` to `/etc/security/limits.conf` or grant `CAP_SYS_NICE`.

---

## Dependencies

`librtlsdr-dev` (**krakenrf fork** — see note above), `libfftw3-dev` (single-precision
`fftw3f`), `libeigen3-dev` (phase eigendecomposition), `libssl-dev`,
`build-essential` / `git` / `pkg-config`. `uWebSockets/` is vendored and built
automatically by the Makefile.

## Further reading

- `CLAUDE.md` — detailed architecture, threading, and coherence-recovery notes.
- `../kraken_doa_v2/` — the FFT / FM / MUSIC DoA client that consumes port 8091.
</content>
</invoke>
