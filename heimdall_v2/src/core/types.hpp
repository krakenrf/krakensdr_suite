#pragma once

#include "config.hpp"  // Need this for NUM_DEVICES, BUFFER_SIZE, etc.
#include "atomic_complex.hpp"
#include <vector>
#include <complex>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <optional>
#include <memory>
#include <utility>

// Type aliases
using SampleBuffer = std::vector<uint8_t>;
using Complex = std::complex<float>;
using ComplexBuffer = std::vector<Complex>;

// Enums
enum class LagCompensatorState {
    MEASURING = 0,   // entry/reset state; hands straight to the servo
    SERVOING  = 1,   // closed-loop proportional sample-clock servo
    CONVERGED = 2
};

enum class PhaseCompensatorState {
    WAITING_FOR_LAG_COMPLETION = 0, MEASURING_INITIAL_PHASE = 1,
    APPLYING_COMPENSATION = 2, VERIFYING_CONVERGENCE = 3, CONVERGED = 4,
    WAITING_FOR_STABILITY = 5,    // Cooldown period after frequency change
    MEASURING_PER_BIN = 6         // Optional: accumulate cross-spectra to build per-bin equalizer FIRs
};

// POD Structs
struct ParabolicPeak {
    float refined_index, refined_magnitude, refined_phase_deg;
    Complex refined_phase_complex;
};

struct DeviceMapping {
    std::string serial;
    int physical_device_id;
    int channel_index;
    bool found;
    
    DeviceMapping(const std::string& s, int ch) : serial(s), physical_device_id(-1), channel_index(ch), found(false) {}
};

struct FFTProcessingControl {
    std::atomic<bool> fft_enabled{true}, auto_disabled{false}, user_override{false};
    std::mutex control_mutex;
};

struct ChannelCompensation {
    LagCompensatorState state = LagCompensatorState::MEASURING;
    float convergence_threshold = 0.15f;  // locked-channel drift warning fires at 2x this |lag|
    // Closed-loop proportional servo (SERVOING state) - the whole lag
    // acquisition mechanism. The correction register stays live
    // (rtlsdr_set_sample_freq_correction_f, RTL2832 regs 0x3e/0x3f): it
    // moves ONLY the sample clock - the tuner LO is untouched, so the
    // correlation peak stays usable at full authority and the servo can
    // measure WHILE correcting. Counts are proportional to the median of
    // the last 5 lag readings (exponential glide), saturating at
    // servo_max_counts far out and tapering into the delicate endgame
    // (freeze near zero, write-kick learning, software frac_delay trim).
    //
    // Gain is delay-aware: the loop actuates immediately but measures
    // hundreds of ms late (pipeline transit + median window + update
    // period), and a delayed proportional loop rings unless
    // gain x total_delay stays well under 1. The control update period is
    // measured each update and the effective gain capped at ~0.15/period
    // (total delay scales with the update period on a loaded machine).
    float servo_gain = 0.5f;        // 1/s ceiling - lag decays as e^{-gain*t}
    float servo_readings[5] = {0};  // circular buffer for the control median
    int servo_reading_count = 0;    // readings seen since servo engage
    int servo_counts = 0;           // register counts currently applied
    // Mid-flight authority ceiling: 6000 counts ~ 357 ppm ~ 858 samples/s.
    // The register field allows +/-8191 counts (+/-488 ppm, fork-clamped);
    // the peak attenuates as ~2*Si(pi*D/2)/(pi*D) with D = ppm*NUM_SAMPLES
    // drift across the window: 69% at 100 ppm, ~17% at 357 ppm, ~12% at the
    // register ceiling - detection at noise-source SNR is NOT the binding
    // constraint (validated on hardware near the ceiling); 6000 keeps
    // amplitude margin for weaker noise coupling (e.g. wideband variant).
    int servo_max_counts = 6000;
    int servo_update_count = 0;     // control updates (for log throttling)
    // Coarse flight (|med| >= 2) runs a ROLLING median: a control update on
    // every reading, ~5x faster than the endgame's every-5th-reading cadence,
    // which raises the delay-safe adaptive gain cap by the same factor (the
    // dominant convergence cost is the exponential taper, tau = 1/gain).
    // Register writes are hysteresis-gated (>= 12.5% count change) so the
    // rolling updates don't multiply USB traffic. The endgame below 2
    // samples keeps the original non-overlapping 5-reading cadence and
    // freeze/kick logic untouched.
    int servo_last_act_reading = 0; // reading index of the last control update
    std::chrono::steady_clock::time_point servo_last_update;  // for the adaptive gain
    float servo_convergence_threshold = 0.02f;  // |med lag| target to lock
    // Each register write kicks the lag by a systematically negative amount
    // (channel-dependent, ~-0.05..-0.15). The servo predicts the post-write
    // landing and learns the kick from every freeze transition.
    float kick_estimate = -0.07f;
    float med_at_freeze = 0.0f;
    bool kick_pending = false;
    int updates_since_write = 0;
    // Software fractional-delay command (samples), applied by the sample
    // pipeline as an 8-tap windowed-sinc FIR. Deterministic sub-sample
    // alignment: hardware register writes kick the lag by a random
    // +/-0.2-0.3 samples, so the final correction must not touch hardware.
    std::atomic<float> frac_delay{0.0f};
    int zero_lag_count = 0;
    std::atomic<bool> initial_calibration_complete{false};
    std::atomic<bool> lag_compensation_locked{false};  // Lock after initial convergence
    // Throttle for the locked-channel drift warning (per channel: touched only
    // by this channel's lag thread, under compensation_mutex)
    std::chrono::steady_clock::time_point last_drift_warning;
};

struct PhaseCompensationData {
    PhaseCompensatorState state = PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION;

    // Lock-free atomic compensation vector for hot-path reads
    // No mutex needed for reads during sample processing!
    AtomicCompensationVector<NUM_DEVICES> compensation_vector;

    bool compensation_applied = false;
    int convergence_count = 0, stable_nonzero_count = 0, failed_convergence_attempts = 0, checks_since_compensation = 0;
    // required_stable_readings: post-settle snapshots averaged before the first apply
    // (10->5->3; the settle gate guarantees they are post-noise and the complex-domain
    // averaging keeps a 3-snapshot estimate clean). required_convergence_readings 5->4.
    int required_convergence_readings = 4, required_stable_readings = 3, max_convergence_attempts = 3, max_checks_before_recompensate = 10;
    float convergence_threshold_degrees = 1.0f, nonzero_threshold_degrees = 1.0f;
    // Amplitude counterpart of the two phase thresholds above: a channel counts
    // as mismatched (apply gate) or unconverged (verify) when its gain error
    // exceeds this many dB. Verification measures the already-compensated
    // stream, so a residual left by the per-pass ±6 dB measurement clamp fails
    // the check and the retry pass multiplies the remainder into the vector.
    // 2.0 dB, NOT tighter: a strong external signal present during the cal
    // (e.g. a broadcast carrier at the tuned frequency) contaminates the
    // noise-source measurement and legitimately wobbles the amplitude estimate
    // by ~1-2 dB - a tighter tolerance chases that wobble through the whole
    // retry budget with the noise source engaged, stretching every retune
    // calibration by tens of seconds. Clamp-truncation residuals (the case
    // this check exists for) are >= 2 dB and still trigger the retry.
    float amplitude_tolerance_db = 2.0f;

    // Noise-source settle gate. noise_on_ns is the steady_clock nanosecond stamp of when
    // the noise source (bias tee) was last ENGAGED for calibration - set in
    // set_bias_tee_all_devices(true). apply_phase_compensation_once ignores readings
    // until NOISE_SETTLE_MS after it, so the pre-noise samples still in the librtlsdr USB
    // ring (~RTL_USB_BUF_COUNT buffers) and the bias-tee/noise-diode turn-on transient
    // cannot bias the estimate. Anchored to NOISE-ON (not MEASURING entry): a retune just
    // switched the noise on, so the gate waits; but at startup / after a coherence
    // recovery the noise has already been on through the seconds-long lag calibration, so
    // the gate is already open and adds NO wait. Default 0 == "long ago", so the first
    // startup calibration is never gated. Atomic so the bias-tee path can stamp it
    // without taking state_mutex.
    std::atomic<long long> noise_on_ns{0};
    // The binding transient is the tuner IF AGC, not USB transit (~30-50 ms): the
    // krakenrf librtlsdr fork leaves the R820T VGA under the RTL2832 AGC even in
    // manual gain mode, so noise-on ramps every channel ~10 dB down at a per-dongle
    // rate over ~1 s. For the first few hundred ms the channel mismatch exceeds the
    // +/-6 dB amplitude-measurement clamp (reads as a steady, wrong value), so this
    // floor must cover that part; the AMP_SETTLE_* gate below catches the tail.
    static constexpr int NOISE_SETTLE_MS = 1000;
    // AGC settle gate (apply_phase_compensation_once): until AMP_SETTLE_MAX_MS after
    // noise-on, a snapshot whose amplitude on any channel is more than AMP_SETTLE_DB
    // off the current streak's running mean restarts the averaging streak, so the
    // applied average is only ever built from a settled AGC. Past the bound the gate
    // is off, so a noisy measurement can never stall the machine.
    static constexpr float AMP_SETTLE_DB = 0.5f;
    static constexpr int AMP_SETTLE_MAX_MS = 5000;

    // Increment 4: snapshot accumulators for the averaged first apply (circular-mean
    // phasor + mean amplitude over the post-settle window). Reset whenever a fresh
    // streak begins (stable_nonzero_count == 0), so any external count reset restarts.
    std::array<Complex, NUM_DEVICES> phase_accum{};
    std::array<float, NUM_DEVICES> amp_accum{};
    int phase_accum_count = 0;

    // Mutex only for state changes, NOT for compensation vector reads
    std::mutex state_mutex;

    // Optional per-bin equalizer: true once the equalizer has been measured/
    // built for the current calibration cycle (reset on every recalibration).
    bool per_bin_measured = false;
    // When the MEASURING_PER_BIN window started (for a stall/timeout fallback).
    std::chrono::steady_clock::time_point per_bin_start;

    // Frequency stability cooldown for drag-to-scroll feature
    std::chrono::steady_clock::time_point last_frequency_change;
    std::atomic<bool> cooldown_active{false};
    static constexpr int STABILITY_DELAY_MS = 3000;         // Default 3s cooldown
    std::atomic<int> stability_delay_override_ms{STABILITY_DELAY_MS};  // Runtime-configurable

    PhaseCompensationData() : last_frequency_change(std::chrono::steady_clock::now()) {}
};

// Optional per-bin (frequency-dependent) phase calibration.
//
// The scalar phase compensation aligns every channel at a single (band-center)
// phase; it cannot represent the residual frequency-dependent mismatch (analog
// IF-filter group-delay differences between dongles + any residual sub-sample
// lag), which can reach several degrees at the band edges. When this mode is
// enabled, after the scalar phase calibration converges the system spends an
// extra measurement window (noise source still on) accumulating the per-bin
// cross-spectrum X_i(f)*conj(X_ref(f)), then designs a short complex
// equalizing FIR per channel that flattens the residual phase across the whole
// band. The FIR is applied to the broadband stream so every downstream
// frequency is coherent with no feedback needed.
//
// OFF by default: it adds a per-channel complex FIR to the hot path, only
// worthwhile on faster hardware. When off, nothing here runs and the hot path
// is unchanged (scalar multiply only).
struct PerBinCalibration {
    static constexpr int FIR_LEN = 32;            // equalizer taps (group delay FIR_LEN/2)
    static constexpr int REQUIRED_SNAPSHOTS = 32; // cross-spectra averaged before design
    static constexpr float SNR_GATE = 0.05f;      // bins below this fraction of peak ref power -> no correction

    std::atomic<bool> enabled{false};             // UI toggle (default OFF)
    // Single publish flag for the designed equalizer set. Set true (release)
    // only after EVERY channel's eq_coeff is written, so the hot path never
    // sees a partially-equalized channel set; cleared on any (re)calibration.
    // Gating the hot path on this one flag (not per-device flags) makes the
    // whole channel set switch in/out atomically.
    std::atomic<bool> ready{false};

    // Measurement accumulators (written by the correlation thread during
    // MEASURING_PER_BIN, read by the designer thread once snapshots complete).
    // Handoff is via the atomic snapshot counter: the writer stops at
    // REQUIRED_SNAPSHOTS, the reader proceeds only after seeing that count.
    std::vector<std::vector<std::complex<float>>> cross_acc;  // [channel][bin]
    std::vector<double> ref_power;                            // [bin]
    std::atomic<int> snapshots{0};
    int num_bins = 0;
    int num_channels = 0;

    // Reset accumulators to begin a fresh measurement window.
    void begin_measurement(int channels, int bins) {
        num_channels = channels;
        num_bins = bins;
        cross_acc.assign(channels, std::vector<std::complex<float>>(bins, std::complex<float>(0.0f, 0.0f)));
        ref_power.assign(bins, 0.0);
        snapshots.store(0, std::memory_order_release);
    }
};

// Forward (feed-forward) phase/amplitude compensation from VNA S2P files.
//
// The noise-source calibration aligns the channels at the receiver inputs, so
// it cannot see the RF parts in FRONT of the KrakenSDR (antenna cables, filters,
// LNAs) - those sit OUTSIDE the calibration loop. Each such chain is measured
// on a VNA as a 2-port Touchstone (.s2p) file; we take the forward-transmission
// term S21(f) - the magnitude/phase the signal picks up passing through the part
// - interpolate it at the tuned center frequency, and form a per-channel
// correction RELATIVE to the reference channel:
//     vector[k] = S21_ref(fc) / S21_k(fc)
// whose phase is the differential insertion phase (-dPhi_k) and whose magnitude
// matches channel gains. (With correct_amplitude == false the magnitude is
// forced to 1 -> phase-only.) This is multiplied into the SAME per-chunk scalar
// as the closed-loop phase vector in samples_to_complex_with_compensation(), so
// it costs one complex multiply per chunk (zero per-sample cost) and is kept in
// a SEPARATE vector so it survives the identity resets the closed-loop vector
// undergoes on every recalibration / retune.
//
// OFF by default. The hot path checks `enabled` (relaxed) then `ready` (acquire)
// exactly like PerBinCalibration; when off nothing here runs. The tables/files
// are cold-path only (web/control/startup threads, guarded by `mutex`); the hot
// path only ever reads the lock-free `vector`.
struct ForwardCompensation {
    static constexpr int N = NUM_DEVICES;

    std::atomic<bool> enabled{false};            // UI master toggle (default OFF)
    std::atomic<bool> ready{false};              // vector computed for current freq
    std::atomic<bool> correct_amplitude{true};   // false => phase-only (unit magnitude)

    // Per-channel correction read in the hot path (identity until computed).
    AtomicCompensationVector<N> vector;

    // Cold-path state: parsed S21(f) tables, selected file names, parse-OK flags.
    // Guarded by `mutex`; never touched from the sample hot path.
    std::mutex mutex;
    std::array<std::vector<std::pair<double, std::complex<double>>>, N> tables;  // [ch] -> sorted (freq_hz, S21)
    std::array<std::string, N> files{};   // selected filename per channel ("" = none)
    std::array<bool, N> loaded{};         // tables[ch] parsed & non-empty
};

struct CorrelationResult {
    std::vector<std::vector<float>> correlation_data;
    std::vector<float> scale_factors;
    std::map<int, float> lags, phases;
    std::map<int, float> amplitudes;  // Amplitude correction factors from eigenvalue decomposition
    std::map<int, LagCompensatorState> channel_states;
    std::map<int, int> channel_zero_counts;
    mutable std::mutex data_mutex;  // mutable allows locking in const functions
    bool data_ready = false, compensation_active = false, phase_compensation_active = false, phase_compensation_complete = false;
    uint64_t data_sequence = 0;
    PhaseCompensatorState phase_state = PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION;

    CorrelationResult() = default;  // Use default constructor
    void initialize();  // Domain-specific initialization moved to correlation module
};

// Wideband scan mode
enum class OperatingMode {
    COHERENT = 0,       // Normal phase-coherent operation
    WIDEBAND_SCAN = 1   // Wideband scan mode (no phase calibration)
};

// Discrete scanner configuration and state
struct DiscreteScannerConfig {
    std::vector<uint64_t> frequency_groups;  // List of frequencies to scan in Hz
    std::atomic<uint32_t> dwell_time_ms{1000};  // Time to spend on each frequency
    std::atomic<bool> enabled{false};  // Scanner active flag
    std::atomic<uint32_t> current_group_index{0};  // Current frequency index (0-N)
    std::atomic<uint32_t> frequency_change_counter{0};  // Increments each frequency change
    std::atomic<bool> retuning_in_progress{false};  // True during tuner settling time
    std::atomic<uint32_t> settling_time_ms{150};  // Tuner settling time in ms (default 150ms)
    mutable std::mutex config_mutex;  // Protects frequency_groups vector (mutable for const functions)

    DiscreteScannerConfig() = default;

    // Thread-safe getters
    size_t get_num_groups() const {
        std::lock_guard<std::mutex> lock(config_mutex);
        return frequency_groups.size();
    }

    uint64_t get_frequency(size_t index) const {
        std::lock_guard<std::mutex> lock(config_mutex);
        if (index < frequency_groups.size()) {
            return frequency_groups[index];
        }
        return CENTER_FREQ;
    }

    // Thread-safe setters
    void set_frequency_groups(const std::vector<uint64_t>& groups) {
        std::lock_guard<std::mutex> lock(config_mutex);
        frequency_groups = groups;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(config_mutex);
        frequency_groups.clear();
        current_group_index = 0;
        frequency_change_counter = 0;
        enabled = false;
    }
};

// Per-tuner frequency configuration for wideband mode
struct WidebandConfig {
    std::array<std::atomic<uint32_t>, NUM_DEVICES> tuner_frequencies;
    std::atomic<bool> enabled{false};
    std::atomic<float> edge_clip{0.8f};  // Edge clip for tuner spacing (0.0-1.0)

    WidebandConfig() {
        // Initialize all tuner frequencies to CENTER_FREQ
        for (size_t i = 0; i < NUM_DEVICES; i++) {
            tuner_frequencies[i] = CENTER_FREQ;
        }
    }

    void set_tuner_frequency(int tuner, uint32_t freq) {
        if (tuner >= 0 && tuner < NUM_DEVICES) {
            tuner_frequencies[tuner] = freq;
        }
    }

    uint32_t get_tuner_frequency(int tuner) const {
        if (tuner >= 0 && tuner < NUM_DEVICES) {
            return tuner_frequencies[tuner].load();
        }
        return CENTER_FREQ;
    }
};

// Global state variables that need to be shared across modules
// current_frequency is the user-facing RF frequency in Hz. In normal mode the
// tuners are set to it directly; in the KrakenSDR Wideband (downconverter)
// variant the tuners sit at the IF and this is the RF the LO mixes down
// (uint64_t: low-side injection reaches ~6.7 GHz, past uint32's ~4.3 GHz cap).
extern std::atomic<uint64_t> current_frequency;
extern std::atomic<int> current_gain;
extern std::atomic<bool> bias_tee_enabled;
// Per-port antenna bias tee state, bit N = channel N (GPIO N+1 on the
// channel-0 chip; GPIO0 is the noise source). Persisted across restarts.
extern std::atomic<uint32_t> antenna_bias_tee_mask;
extern std::atomic<int> rtl_tcp_channel;
extern std::atomic<bool> global_running;
extern std::atomic<OperatingMode> operating_mode;
extern std::atomic<int> active_num_elements;  // Set via -n flag at startup (2 to NUM_DEVICES)

// ---- Coherence-loss detection / recovery ----
// Coherence rests on one invariant: the N-th sample set from every device must
// be the same instant. A single dropped USB packet shifts one channel by a full
// NUM_SAMPLES - outside what lag compensation can represent - and silently
// destroys coherence. Any code that detects a desync condition (per-device L1
// overflow, sample-pool exhaustion, or a post-lock lag discontinuity) sets
// coherence_lost via signal_coherence_lost(); the coherence watchdog consumes
// the flag and runs a full flush + recalibration. coherence_event_count is a
// monotonically increasing tally surfaced to the UI; recovery_in_progress is
// true while the watchdog is recalibrating.
extern std::atomic<bool> coherence_lost;
extern std::atomic<bool> recovery_in_progress;
extern std::atomic<uint32_t> coherence_event_count;
extern WidebandConfig wideband_config;
extern DiscreteScannerConfig discrete_scanner;

// ---- Periodic calibration check ----
// The periodic calibration monitor re-verifies lag/phase coherence every
// periodic_recal_minutes (when enabled) and, if the calibration has drifted,
// sets force_recalibration so the coherence watchdog runs a full recalibration.
// The web UI "force recalibration" button also sets force_recalibration. The
// flag is consumed by coherence_watchdog() exactly like coherence_lost, so all
// recalibration stays serialized on the single watchdog thread.
extern std::atomic<bool> force_recalibration;
extern std::atomic<bool> periodic_recal_enabled;
extern std::atomic<int> periodic_recal_minutes;
// Tally of how many periodic checks found the lag / phase coherence out of
// tolerance and triggered a recalibration (counted independently - a check that
// finds both bad bumps both). Session counters, surfaced to the web UI.
extern std::atomic<uint32_t> periodic_recal_lag_fail_count;
extern std::atomic<uint32_t> periodic_recal_phase_fail_count;

// Forward declarations for commonly shared globals (defined in main.cpp)
struct SDRDevice;
extern std::vector<std::unique_ptr<SDRDevice>> devices;
extern std::optional<PhaseCompensationData> phase_compensation;
extern std::mutex settings_mutex;
extern FFTProcessingControl fft_control;
extern PerBinCalibration per_bin_cal;
extern ForwardCompensation forward_comp;