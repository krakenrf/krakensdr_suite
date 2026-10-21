# MUSIC DoA Algorithm Reference — kraken_doa_v2 (for GNU Radio port)

> **Licence note:** the code excerpts in this document are quoted from
> kraken_doa_v2, which is under the KrakenSDR Source Available Licence
> (`kraken_doa_v2/LICENSE`), as a reference for the port. They are not part of
> this GNU Radio module and are not licensed under the GPL; the module's own
> implementation (`lib/doa_music_impl.cc`) is.

All paths relative to `/home/krakenrf/krakensdr_v2/kraken_doa_v2/`. Core file: `src/signal_processing/music_processor.cpp` (impl), `include/signal_processing/music_processor.hpp` (state/config), `include/config.hpp` (constants), `include/globals.hpp` (`uca_angle_sign`), `src/networking/data_receiver.cpp` (feed + gating), `src/signal_processing/shared_decimator.cpp` (pre-MUSIC decimation).

---

## 0. End-to-end processing chain (what feeds MUSIC)

```
Heimdall TCP :8091  — packets: 32B header + N_ch*8B metadata + N_ch*16384 uint8 IQ pairs
  header fields (BE u32): magic 'MCHQ', channels, samples(=16384), phase_comp_state,
  noise_source, freq_change_counter, group_index, retuning_in_progress
  (data_receiver.cpp:147-161; server NUM_SAMPLES=16384 @ 2.4 MSPS → ~6.83 ms/packet)
    ↓ uint8 → complex<float>: (b/127.5 - 1.0), with DC correction   (data_receiver.cpp:383-396)
    ↓ RawDataBuffer (bounded queue)
decimation_processor_thread (data_receiver.cpp:434)
    ↓ per-DecimatorInstance async task ("VFO"; each owns its own MUSICProcessor,
      decimator_manager.hpp:16-61)
STAGE 1: SharedDecimator::decimateMultiChannel — per channel:
    ↓ phase-continuous NCO mixes VFO offset to baseband at input rate
      (phase_inc = -2π·offset/2.4e6, shared_decimator.cpp:270-296)
    ↓ cascaded Kaiser low-pass firdecim_cccf stages (liquid-dsp), factors ≤8,
      60 dB stopband, passband 0.4·final_rate  (shared_decimator.cpp:89-140)
      default decimation = 10 → 240 kHz complex float stream (config.hpp:159, DEFAULT_BANDWIDTH_INDEX=7)
STAGE 2 (same task, no barrier): MUSICProcessor::processDecimatedIQ(decimated)  (data_receiver.cpp:642)
    ↓ circular accumulator → overlapping snapshots → covariance → EVD → pseudospectrum
Output: pseudospectrum (360 doubles) + interpolated peak + confidence
    → WS binary "multi-DoA" broadcast every 200 ms (websocket_server.cpp:427-434,
      message_builders.cpp:328-436), DoA CSV logger, DOA_value.html
```

**MUSIC input is DECIMATED, offset-mixed IQ** — never the raw 2.4 MSPS. All `DOA_NUM_ELEMENTS`=5 channels go through identical decimator state so cross-channel phase is preserved (per-channel persistent mixer phase + filter history, shared_decimator.hpp:78-94).

---

## 1. Block sizes, accumulation, covariance

### Constants (`include/config.hpp:105-110`)
```cpp
constexpr int DOA_NUM_ELEMENTS = 5;        // M
constexpr int DOA_BLOCK_SIZE = 256;        // documented; actual value hardcoded in MUSICConfig
constexpr int DOA_NUM_SNAPSHOTS = 64;      // ** NOT USED by the processor ** (see below)
constexpr int DOA_ANGULAR_RESOLUTION = 1;  // deg
constexpr int DOA_NUM_ANGLES = 360;
```

### Actual runtime config (`music_processor.hpp:16-21`, ctor `music_processor.cpp:19-22`)
```cpp
struct MUSICConfig {
    size_t snapshot_length = 256;   // samples per snapshot (= DOA_BLOCK_SIZE)
    size_t num_snapshots  = 32;     // max snapshots per frame (NOT config.hpp's 64!)
    size_t overlap_samples = 64;    // overlap between consecutive snapshots
    size_t min_snapshots  = 16;     // minimum to trigger a frame
};
```
Runtime-adjustable via WS commands `MUSIC_SNAPSHOT_LENGTH:` (64–2048; overlap forced to len/4) and `MUSIC_NUM_SNAPSHOTS:` (8–128; min forced to max(8, n/2)) — control_handler.cpp:813-836.

### Accumulator (`music_processor.cpp:202-253`, struct hpp:291-325)
- Per-processor circular buffer `M × buffer_size`, `buffer_size = nextPow2(snapshot_length·num_snapshots·3)` (= 32768 default), bitmask indexing. Each `processDecimatedIQ` call appends `decimated_data.min_samples` (min over channels) samples per channel. Calls with `min_samples < 16` or `num_channels < 5` are rejected (music_processor.cpp:146-152).

### Snapshot extraction (`extractSnapshotsOptimized`, music_processor.cpp:255-300)
- `step = snapshot_length - overlap` = 256−64 = **192**.
- Trigger: `samples_available ≥ min_snapshots·len − (min_snapshots−1)·overlap` = 16·256−15·64 = **3136 samples** (~13.1 ms @ 240 kHz → up to ~75 frames/s; measured MUSIC compute ~8-12 ms on Pi4).
- Extracts up to `num_snapshots` (32) overlapping M×256 windows starting at the oldest sample; then consumes `step·(count−1)+len` samples from the buffer (leftover ≤ overlap retained → snapshot continuity across frames).

### Covariance (`computeCorrelationMatrixOptimized`, music_processor.cpp:443-505)
Snapshots are **concatenated horizontally** into `X` (M × N, N = count·256, up to 8192; overlapped samples appear in two snapshots so are double-weighted — effectively simple sample covariance with 25% overlap reuse):
```cpp
// R = X * X^H / N   (Eigen selfadjoint rank-update → BLAS herk); MatrixXcd = complex<double>
double scale = 1.0 / static_cast<double>(N);
correlation_matrix_.setZero();
correlation_matrix_.selfadjointView<Eigen::Lower>().rankUpdate(X, scale);
correlation_matrix_.triangularView<Eigen::StrictlyUpper>() =
    correlation_matrix_.triangularView<Eigen::StrictlyLower>().adjoint();

// Forward-backward averaging — ULA ONLY (off by default, WS MUSIC_FB_AVERAGING:)
// (on a UCA conjugation maps arrivals to antipodes → 180° ghosts, hpp:130-137)
if (fb_averaging_enabled_ && current_topology == ArrayTopology::ULA) {
    Eigen::MatrixXcd flipped = correlation_matrix_.conjugate().reverse(); // J·conj(R)·J
    correlation_matrix_ = 0.5 * (correlation_matrix_ + flipped);
}

// Trace normalization (scale invariance vs gain/AGC)
double trace_R = correlation_matrix_.trace().real();
if (trace_R > 1e-15) correlation_matrix_ *= (1.0 / trace_R);

// Optional temporal EMA across frames (alpha = new-frame weight, default 1.0 = OFF;
// WS MUSIC_COVARIANCE_ALPHA:, clamped 0.05-1.0; reset on retune/config/topology change)
if (covariance_alpha_ < 0.999f) { covariance_avg_ = (1-a)*covariance_avg_ + a*R; R = covariance_avg_; }

correlation_matrix_.diagonal().array() += 1e-12;   // regularization
```
**No spatial smoothing** (no subarray averaging) anywhere. No windowing of snapshots.

---

## 2. Eigendecomposition & noise subspace

`music_processor.cpp:522-580` (1D) / 1398-1445 (2D):
- **`Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd>`** (double-precision complex), pre-allocated, `compute(correlation_matrix_.selfadjointView<Eigen::Lower>())`. Eigen returns eigenvalues **ascending** → noise subspace = `eigenvectors.leftCols(noise_dimension)`.
- Signal dimension (`resolveSignalDimension`, music_processor.cpp:919-952): manual `num_signal_sources_` (default **1**, WS `MUSIC_SIGNAL_SOURCES:`, clamped `[1, M-1]`) **or** auto-estimate (`setAutoNumSources`):
  - `estimateNumSources` (music_processor.cpp:881-917): eigenvalue-dominance test on ascending eigenvalues — grow noise set from smallest up; λ counts as a source iff `λ > 10^(6dB/10)·running_noise_mean` **AND** `λ > λ_max·10^(-12dB/10)` (constants `AUTO_SOURCES_THRESHOLD_DB=6`, `AUTO_SOURCES_REL_THRESHOLD_DB=12`, hpp:428-436). Deliberately **not** MDL/AIC (comments hpp:160-162).
  - Adoption hysteresis: new count must repeat `AUTO_SOURCES_STABLE_FRAMES=3` consecutive frames.
  - Even estimate 0 keeps signal_dimension=1.
- **Eigenvalue ratio** (squelch metric, computed every frame BEFORE gating, music_processor.cpp:539-559): `ratio = λ_max / mean(λ_1..λ_{M-1})`, plus 5 s peak-hold (`EIGEN_PEAK_HOLD_MS`, hpp:65).

---

## 3. Steering vectors (`updateSteeringVectors`, music_processor.cpp:970-1069)

Precomputed matrix `S` (M × 360), regenerated on frequency/topology/radius/spacing change. Wavelength: `λ = 3e8 / current_frequency` (frequency = **RF center + VFO offset**; enforced each block against packet metadata with 1 kHz tolerance, data_receiver.cpp:602-609; in the wideband-downconverter variant this is the true RF).

Element positions (in **wavelengths**):
```cpp
// UCA (default; radius default 50 mm, WS RADIUS: in mm):
double r = (array_radius_mm / 1000.0) / wavelength;
const double dir = uca_angle_sign();      // globals.hpp: constant -1.0 - arrays are
                                          // expected wired CLOCKWISE on both standard KrakenSDR
                                          // and Wideband. Mirror about +x; ANT0 stays at +x.
for (int elem = 0; elem < M; elem++) {
    double elem_angle = dir * 2.0 * M_PI / M * elem;
    x_pos[elem] = r * cos(elem_angle);  y_pos[elem] = r * sin(elem_angle);
}
// ULA (spacing default 30 mm, WS SPACING:): x = elem*d, y = 0
// CUSTOM: user XY(Z) positions in mm (z≠0 → 2D MUSIC, section 7)
```
Steering vector per azimuth bin θ (grid: **360 bins × 1°, fixed**, θ = bin·1° in radians):
```cpp
double phase;
if (current_topology == ArrayTopology::ULA)
    phase = 2.0 * M_PI * x_pos[elem] * sin(theta_rad);                       // ULA
else
    phase = 2.0 * M_PI * (x_pos[elem]*cos(theta_rad) + y_pos[elem]*sin(theta_rad)); // UCA/CUSTOM
steering_vector(elem) = complex<double>(cos(phase), sin(phase));             // exp(+j·phase)
// then normalized: s /= ||s||  (each column unit-norm)
```
Sign conventions to preserve in a port: **positive exponent** `exp(+j·2π/λ·(x·cosθ + y·sinθ))`; azimuth is **unit-circle: 0° = +x = ANT0 direction, CCW positive** (project memory: polar UI adds a compass toggle on top; the DSP itself is unit-circle). The clockwise element mirror (`dir=-1`, applied on all hardware) is what keeps the OUTPUT convention CCW while the array itself runs clockwise. This OOT module's `doa_music` block matches it via `UCA_ANGLE_SIGN` (`lib/doa_music_impl.cc`). `wb_topology_active` (TOPOLOGY:WIDEBAND) is just UCA with radius auto-set to the ring: 127.5/51/20.4 mm (`WB_RING_RADIUS_MM`, config.hpp:98; control_handler.cpp:719-742).

---

## 4. Pseudospectrum, peak selection, temporal behavior

### Core loop (`computeMUSICSpectrumOptimized`, music_processor.cpp:578-604)
```cpp
auto E = eigenvectors.leftCols(noise_dimension);   // M × (M-K) noise subspace
const MatrixXcd& S = steering_vectors;             // M × 360
EH_S_working_.noalias() = E.adjoint() * S;         // (M-K) × 360
pseudospectrum.noalias() = EH_S_working_.colwise().squaredNorm();   // ||E^H s||²
pseudospectrum = pseudospectrum.cwiseMax(1e-15).cwiseInverse();     // P(θ)=1/max(||E^H s||²,1e-15)
applyOutputTransforms();   // ULA half-plane mask, then array-offset rotation
```
(Note: since each column of S is unit-norm, this is the classic `P = 1 / (s^H E E^H s)`.)

### Output transforms (`applyOutputTransforms`, music_processor.cpp:1151-1188)
1. **ULA front/back mask** (`ULA_MODE:` FORWARD/BACKWARD/BOTH, types.hpp:21-25): FORWARD keeps bins in `[0,90] ∪ [270,360)`, BACKWARD keeps `(90,270)`; masked bins set to 0.
2. **Array offset rotation** (`ARRAY_OFFSET:` deg, default 0): circular shift `rotated(i) = raw(i - round(offset/res))` so reported = array-frame angle + offset. (Beamformer un-does this to steer in array frame, data_receiver.cpp:722-727.)

### Peak selection (`getPeakAngle` / `getPeakAngleWithConfidence`, music_processor.cpp:636-720)
- Single global `maxCoeff` over the 360-bin spectrum (no multi-peak search in the 1D path).
- **Sub-grid refinement**: 3-point parabolic vertex fit on **ln(P)** with circular indexing:
```cpp
double y1=log(P[i-1]), y2=log(P[i]), y3=log(P[i+1]);
double denom = y1 - 2*y2 + y3;
if (denom < -1e-12) delta = clamp(0.5*(y1-y3)/denom, -0.5, 0.5);
angle = (max_idx + delta) * 1.0°;   // wrapped to [0,360); falls back to grid angle
                                    // if a neighbor ≤ 0 (ULA mask edge)
```
- Returns −1 if spectrum empty/all-zero.

### Temporal smoothing of the OUTPUT
- No smoothing/averaging of the pseudospectrum or the angle itself. The only temporal elements are: (a) optional covariance EMA (§1), (b) snapshot overlap, (c) **publish freeze** — when the eigenvalue-squelch gate is closed or `publish_hold_` set, the frame is computed (ratio stays fresh) but `pseudospectrum`/peak/`result_stamp_ms_` are NOT updated; consumers hold the last open frame (music_processor.cpp:565-576; hpp:66-97).

---

## 5. Output conventions & quality metric

- **Angle**: degrees `[0,360)`, unit-circle frame (ANT0 = +x = 0°, CCW positive), plus user `ARRAY_OFFSET`. Grid 1°, readout ~0.1°-class via parabolic interpolation.
- **Confidence** (music_processor.cpp:699-717): peak-to-mean ratio of the (linear) pseudospectrum, normalized:
```cpp
confidence = min(1.0, (max_val/mean_val - 1.0) / 3.0);   // 1.0→0%, 2.0→33%, 4.0→100%
```
- **Eigenvalue ratio** `λ1/mean(λ2..λN)` (+ 5 s peak hold) exported for squelch/status (`~1` noise, `>2-3` weak, `>10` strong).
- Frame stamp `result_stamp_ms_` (system_clock ms) on every *published* frame — used for dedup by the logger.
- Broadcast: 200 ms WS timer, binary message type 3 with per-VFO id, offset kHz, 360 float spectrum, resolution, radius (+ elevation block for 3D) — message_builders.cpp:328-436.

---

## 6. Gating — when MUSIC is allowed to run (data_receiver.cpp:546-683)

MUSIC runs per packet per decimator **only if ALL** of:
1. `doa_enabled` (WS `DOA:1`) and packet `channels ≥ DOA_NUM_ELEMENTS` (data_receiver.cpp:468-470); decimator enabled & not being deleted.
2. **Not** tuner-spread wideband scan mode (channels not coherent).
3. **Not** `is_retuning` (packet flag — retuning packets are skipped entirely upstream, data_receiver.cpp:313).
4. **`!doa_is_calibrating()`** (doa_logger.cpp:37-51): noise source currently active **or** active within the last 750 ms. **Gates ONLY on the noise source, NOT on phase_state** — steady-state DoA runs at phase_state 4/CONVERGED and a phase_state test would freeze everything (project memory + websocket_server.cpp:490-497). This is a hard gate: calibration noise never enters the accumulator/covariance.
5. **`!doa_retune_hold_active()`** (data_receiver.cpp:61-76): 4000 ms (`RETUNE_DOA_HOLD_MS`) hold after any retune, armed by retuning-complete transition, freq-change counter bump, or reference-channel metadata frequency change — covers heimdall's ~3 s post-retune cooldown where the noise source is already OFF but data is settling.
6. FFT squelch (method 0), if enabled and beamforming OFF: skip MUSIC entirely when closed (DoA freezes). With beamforming ON, MUSIC always runs (provides steering) and a closed squelch instead sets `setPublishHold(true)`. Eigenvalue squelch (methods 1/2) never skips MUSIC — it arms `setSquelchGate(enabled, thr)` and the processor freezes publishing internally with hysteresis (close below `thr·0.8`, reopen at `thr`; `EIGEN_GATE_HYSTERESIS`, hpp:369).

Phase_state values from server packets (0-5): WAITING_LAG, MEASURING, WAITING_STABILITY, APPLYING, VERIFYING, CONVERGED (data_receiver.cpp:178).

---

## 7. 2D MUSIC (CUSTOM topology with any z ≠ 0) — music_processor.cpp:1288-1504

- Steering: `s(θ,φ) = exp(+j·k·(x·cosθ·cosφ + y·sinθ·cosφ + z·sinφ))`, `k = 2π/λ`, positions in meters, unit-normalized; grid 360 az × 181 el (−90°..+90°, res settable 0.5–5°). Same covariance/EVD; pseudospectrum over 360·181 columns; peak = global argmax; 1D spectra by **max-projection** marginalization (not mean — music_processor.cpp:1476-1504).

## 8. Porting notes / gotchas

- `config.hpp`'s `DOA_NUM_SNAPSHOTS=64` and `DOA_BLOCK_SIZE=256` are **documentation only** — the processor hardcodes 32/256/64/16 in its constructor. Only `DOA_NUM_ELEMENTS` and `DOA_NUM_ANGLES` are actually consumed.
- Everything inside MUSIC is **double precision** (`MatrixXcd`); input is `complex<float>` upcast on accumulator write.
- Element count is compile-time fixed at 5 (`DOA_NUM_ELEMENTS`); a `active_num_elements` global exists but the MUSIC path uses all 5 rows.
- Decimator NCO phase continuity across blocks matters: "restarting at (1,0) every block would … contaminate MUSIC snapshots that span blocks" (shared_decimator.cpp:266-269). In GNU Radio a stream-based xlating FIR gives this for free.
- Steering-vector/covariance sign consistency: data model is baseband IQ after identical per-channel LO/decimation; steering uses `exp(+j·…)` and noise-projection `||E^H s||²` — keep both together or the bearing mirrors.
- Beamformer (`src/signal_processing/beamformer.cpp:207-242`) uses the identical geometry (`dir = uca_angle_sign()`, `phase = 2π(x·cosθ + y·sinθ)`), steered by MUSIC's peak minus array offset.
- Per-VFO architecture: each DecimatorInstance owns an independent MUSICProcessor; settings commands fan out via `forEachMusicProcessor`.