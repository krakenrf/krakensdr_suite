#include "compensation.hpp"
#include "fft_plan.hpp"
#include "../sdr/sdr_device.hpp"
#include "../sdr/sdr_pipeline.hpp"
#include "../sdr/sdr_init.hpp"
#include "../sdr/pipeline_control.hpp"
#include "../core/config.hpp"
#include <algorithm>
#include <iostream>
#include <thread>
#include <cmath>
#include <vector>
#include <array>
#include <complex>

// Forward declarations for functions defined elsewhere
extern void clear_l2_buffer();
extern void clear_l1_buffer();

// ---- Optional per-bin phase calibration (frequency-dependent equalizer) ----

// Finish phase calibration: mark CONVERGED, flag devices calibration-complete,
// turn off the noise-source bias-tee, and auto-disable FFT. Assumes the caller
// holds phase_compensation->state_mutex.
static void complete_phase_calibration_locked() {
    phase_compensation->state = PhaseCompensatorState::CONVERGED;
    phase_compensation->per_bin_measured = true;
    // Clears any in-flight coherence recovery: reconvergence ends the recal that
    // a coherence event armed (harmless no-op for normal startup/settings recal,
    // where the flag is already false).
    recovery_in_progress.store(false, std::memory_order_release);
    std::cout << "Phase: CONVERGED!" << std::endl;

    for (const auto& device : devices) {
        if (device) device->compensation.initial_calibration_complete = true;
    }

    // C5: calibration done - restore the wide L2-raw cap for the steady-state cushion.
    l2_raw_cap.store(L2_RAW_MAX, std::memory_order_relaxed);

    // A fresh convergence supersedes any stale-calibration flag (--kerberos).
    kerberos_cal_stale.store(false, std::memory_order_release);

    set_bias_tee_all_devices(false, devices);

    std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
    fft_control.fft_enabled = false;
    fft_control.auto_disabled = true;
    fft_control.user_override = false;
    std::cout << "Phase: FFT auto-disabled after calibration complete" << std::endl;
}

// Decide what happens once the scalar phase compensation has converged: if
// per-bin mode is on and we have not yet built the equalizer this calibration
// cycle, enter the per-bin measurement window (noise source + FFT stay on);
// otherwise finish. Assumes phase_compensation->state_mutex is held.
static void proceed_after_scalar_convergence_locked() {
    if (per_bin_cal.enabled.load(std::memory_order_relaxed) && !phase_compensation->per_bin_measured) {
        per_bin_cal.ready.store(false, std::memory_order_release);  // don't apply a stale FIR while measuring
        per_bin_cal.begin_measurement(active_num_elements.load(), CORRELATION_SIZE);
        phase_compensation->per_bin_start = std::chrono::steady_clock::now();
        phase_compensation->state = PhaseCompensatorState::MEASURING_PER_BIN;
        std::cout << "Phase: scalar converged; measuring per-bin equalizer ("
                  << PerBinCalibration::REQUIRED_SNAPSHOTS << " snapshots)..." << std::endl;
    } else {
        complete_phase_calibration_locked();
    }
}

// Build a complex equalizing FIR per channel from the accumulated per-bin
// cross-spectra. Phase-only correction W_i(f) = exp(-j*arg(H_i(f))) where
// H_i(f) = <X_i*conj(X_ref)> / <|X_ref|^2>; a centering linear-phase ramp puts
// the impulse response at group delay FIR_LEN/2 (applied to every channel,
// reference = pure delta, so relative timing is preserved). Bins below the SNR
// gate get no correction. Runs once, on the lag-compensation thread, after the
// measurement window completes (single reader; writer has stopped).
// Returns true if equalizers were built for all active channels. Writes only
// eq_coeff/eq_tail; the single per_bin_cal.ready publish flag is set by the
// caller (under state_mutex, after verifying no recalibration intervened).
static bool design_per_bin_equalizers() {
    const int nch  = per_bin_cal.num_channels;
    const int bins = per_bin_cal.num_bins;
    constexpr int L = PerBinCalibration::FIR_LEN;
    constexpr int D = L / 2;  // group delay (taps)
    if (nch <= 0 || bins != CORRELATION_SIZE) return false;

    double peak = 0.0;
    for (int k = 0; k < bins; k++) peak = std::max(peak, per_bin_cal.ref_power[k]);
    const double gate = static_cast<double>(PerBinCalibration::SNR_GATE) * peak;
    if (peak <= 0.0) return false;  // no reference power -> nothing to design

    FftPlan* plan = fft_pool.acquire();
    std::vector<std::complex<float>> W(bins);

    for (int ch = 0; ch < nch && ch < static_cast<int>(devices.size()); ch++) {
        auto& dev = devices[ch];
        if (!dev) continue;

        if (ch == REF_CHANNEL) {
            // Reference: pure delta at the common group-delay tap.
            dev->eq_coeff.fill(Complex(0.0f, 0.0f));
            dev->eq_coeff[D] = Complex(1.0f, 0.0f);
            dev->eq_tail.fill(Complex(0.0f, 0.0f));
            std::cout << "Per-bin EQ ch" << ch << ": reference (delta)" << std::endl;
            continue;
        }

        const auto& acc = per_bin_cal.cross_acc[ch];
        for (int k = 0; k < bins; k++) {
            std::complex<float> w;
            if (per_bin_cal.ref_power[k] > gate && std::abs(acc[k]) > 0.0f) {
                // ref_power is real-positive, so arg(H) == arg(cross-spectrum)
                w = std::polar(1.0f, -std::arg(acc[k]));     // exp(-j*arg H)
            } else {
                w = std::complex<float>(1.0f, 0.0f);          // low SNR -> identity
            }
            const float ramp = -2.0f * static_cast<float>(M_PI) * static_cast<float>(k) * D / bins;
            W[k] = w * std::polar(1.0f, ramp);                // center impulse at tap D
        }

        // Inverse transform -> impulse response (FFTW backward is unnormalized,
        // scaled by `bins`). Window L taps and normalize DC to unit magnitude.
        std::complex<float>* h = plan->inverse_transform(W.data());

        // Sanity check (once): confirm the impulse peak landed near tap D, i.e.
        // the half-buffer offset was correctly cancelled. If it is far from D,
        // the windowed taps would be noise.
        {
            int peak_n = 0; float peak_mag = 0.0f;
            for (int n = 0; n < bins; n++) {
                const float m = std::abs(h[n]);
                if (m > peak_mag) { peak_mag = m; peak_n = n; }
            }
            std::cout << "Per-bin EQ ch" << ch << ": impulse peak at tap " << peak_n
                      << " (expected ~" << D << ")" << std::endl;
        }

        std::array<Complex, L> taps{};
        std::complex<double> dc(0.0, 0.0);
        for (int n = 0; n < L; n++) {
            const float win = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * n / (L - 1));
            const Complex t = (h[n] / static_cast<float>(bins)) * win;
            taps[n] = t;
            dc += std::complex<double>(t);
        }
        const double dcmag = std::abs(dc);
        if (dcmag > 1e-9) {
            const float inv = static_cast<float>(1.0 / dcmag);
            for (auto& t : taps) t *= inv;
        }

        dev->eq_coeff = taps;
        dev->eq_tail.fill(Complex(0.0f, 0.0f));
        std::cout << "Per-bin EQ ch" << ch << ": designed (" << L << " taps, DC mag " << dcmag << ")" << std::endl;
    }

    fft_pool.release(plan);
    return true;
}

void apply_phase_compensation_once(const std::map<int, float>& measured_phases) {
    if (!phase_compensation) return;

    // Skip phase compensation in wideband scan mode
    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) return;

    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
    if (phase_compensation->compensation_applied) return;

    // Noise-source settle gate. Ignore readings (and restart the accumulation streak)
    // until NOISE_SETTLE_MS after the noise source was engaged, so the pre-noise samples
    // in the USB ring and the noise-source turn-on transient cannot bias the calibration.
    // Anchored to NOISE-ON (noise_on_ns): a retune just switched the noise on so the gate
    // waits; startup / post-recovery has the noise already settled so the gate is already
    // open and adds no wait. Stamped once per noise-on, so it opens after at most
    // NOISE_SETTLE_MS and cannot stall the machine.
    const long long since_noise_on_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() -
        phase_compensation->noise_on_ns.load(std::memory_order_relaxed);
    if (since_noise_on_ns < static_cast<long long>(PhaseCompensationData::NOISE_SETTLE_MS) * 1000000LL) {
        phase_compensation->stable_nonzero_count = 0;
        return;
    }

    // PHASE-ONLY calibration: the correction is a unit phasor per channel, no
    // gain. Introduced while the IF VGA was under the RTL2832 AGC, when a
    // dongle's gain hunted by up to ~2.5 dB between snapshots and a gain
    // correction averaged from a few snapshots locked in up to ~1.6 dB of error
    // (measured 2026-09-29). With the VGA now fixed (config.h) the gains are
    // stable to ~0.1 dB, but the noise source clips ~13% of samples at that
    // step and compresses the tuner front end, so it measures saturation levels,
    // not small-signal gain: re-tested 2026-09-30, its gain estimate moved
    // 0.7-1.0 dB between VGA steps 8 and 0 and disagreed with the receiver
    // noise-floor ratio by up to 2.4 dB. Gain correction therefore stays off.
    // Gain is still measured and shown (web "Amp" column).
    //
    // Every reading counts, including one where all phases are already within
    // +-1 deg. A former "some phase > 1 deg" gate (it kept placeholder zeros
    // out, now done by phases_measured in the caller) wedged the machine with
    // the noise source on whenever the array was already nearly in phase.

    // Increment 4: snapshot averaging. The eigen solve is single-shot per set; instead
    // of waiting N readings then applying only the LATEST (noisy) one, average the last
    // N independent post-settle snapshots as unit phasors (circular-mean phase) and
    // apply the average. ~sqrt(N) cleaner first estimate -> VERIFYING passes first try
    // (no apply->verify retry tail), which is what makes the reduced
    // required_stable_readings safe. Tied to stable_nonzero_count: a fresh streak
    // (count == 0, e.g. just reset by the settle gate, a zero reading, or
    // handle_settings_change) restarts the average.
    if (phase_compensation->stable_nonzero_count == 0) {
        phase_compensation->phase_accum.fill(Complex(0.0f, 0.0f));
        phase_compensation->phase_accum_count = 0;
    }
    for (const auto& [channel, phase] : measured_phases) {
        if (channel == REF_CHANNEL || channel < 0 || channel >= NUM_DEVICES) continue;
        const float phase_radians = phase * static_cast<float>(M_PI) / 180.0f;
        phase_compensation->phase_accum[channel] += Complex(std::cos(-phase_radians), std::sin(-phase_radians));
    }
    phase_compensation->phase_accum_count++;
    phase_compensation->stable_nonzero_count++;

    if (phase_compensation->stable_nonzero_count < phase_compensation->required_stable_readings) return;

    const int n = std::max(1, phase_compensation->phase_accum_count);
    std::cout << "Phase: Applying compensation (avg of " << n << " snapshots) [";
    bool first = true;
    for (const auto& kv : measured_phases) {
        const int channel = kv.first;
        if (channel == REF_CHANNEL || channel < 0 || channel >= NUM_DEVICES) continue;

        // Circular-mean direction over the window, renormalized to a unit phasor.
        Complex dir = phase_compensation->phase_accum[channel];
        const float mag = std::abs(dir);
        dir = (mag > 1e-6f) ? dir / mag : Complex(1.0f, 0.0f);

        // Lock-free atomic store (cold path). comp = exp(-j*phase): phase only.
        phase_compensation->compensation_vector.store(channel, dir);

        if (!first) std::cout << ", ";
        const float meas_deg = -std::arg(dir) * 180.0f / static_cast<float>(M_PI);
        std::cout << "Ch" << channel << ":" << meas_deg << "°";
        first = false;
    }
    std::cout << "]" << std::endl;

    clear_l2_buffer();
    phase_compensation->compensation_applied = true;
    // Increment 1: drop the APPLYING_COMPENSATION pass-through. It performed no
    // measurement - it only burned one driver tick flipping itself to VERIFYING.
    // clear_l2_buffer() above (which also flushes L2-raw) plus the data_sequence gate
    // already ensure VERIFYING's first reading is post-compensation data.
    phase_compensation->state = PhaseCompensatorState::VERIFYING_CONVERGENCE;
    phase_compensation->checks_since_compensation = 0;
}

bool check_phase_convergence(const std::map<int, float>& current_phases) {
    if (!phase_compensation) return false;

    // Skip phase compensation in wideband scan mode
    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
        return false;
    }

    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
    phase_compensation->checks_since_compensation++;

    float max_phase_error = 0.0f;
    bool all_converged = std::all_of(current_phases.begin(), current_phases.end(),
        [&max_phase_error](const auto& pair) {
            if (pair.first == REF_CHANNEL) return true;
            float abs_phase = std::abs(pair.second);
            max_phase_error = std::max(max_phase_error, abs_phase);
            return abs_phase <= phase_compensation->convergence_threshold_degrees;
        });

    // Phase only: gain is not corrected (see apply_phase_compensation_once), so
    // there is no gain residual to converge.

    if (all_converged) {
        phase_compensation->convergence_count++;
        if (phase_compensation->convergence_count >= phase_compensation->required_convergence_readings) {
            // Either finish, or (per-bin mode) enter the equalizer measurement window.
            proceed_after_scalar_convergence_locked();
            return true;
        }
    } else {
        phase_compensation->convergence_count = 0;
        
        if (phase_compensation->checks_since_compensation >= phase_compensation->max_checks_before_recompensate) {
            phase_compensation->failed_convergence_attempts++;
            
            if (phase_compensation->failed_convergence_attempts < phase_compensation->max_convergence_attempts) {
                std::cout << "Phase: Applying additional compensation" << std::endl;

                for (const auto& [channel, phase] : current_phases) {
                    if (channel != REF_CHANNEL) {
                        float phase_radians = phase * M_PI / 180.0f;
                        // Unit phasor: phase-only, like the first apply
                        Complex additional_comp(std::cos(-phase_radians), std::sin(-phase_radians));
                        // Read-modify-write for atomic compensation vector
                        auto current = phase_compensation->compensation_vector.load(channel);
                        phase_compensation->compensation_vector.store(channel, current * additional_comp);
                    }
                }
                
                phase_compensation->checks_since_compensation = 0;
                phase_compensation->convergence_count = 0;
                // Reduce buffer clearing to allow continuous refinement
                clear_l2_buffer();
            } else {
                // Max attempts reached: finish (or measure per-bin equalizer first).
                std::cout << "Phase: max convergence attempts reached" << std::endl;
                proceed_after_scalar_convergence_locked();
            }
        }
    }
    
    return false;
}

std::optional<PhaseCompensatorState> get_phase_compensation_state() {
    if (!phase_compensation) return std::nullopt;
    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
    return phase_compensation->state;
}

std::optional<PhaseCompensatorState> get_phase_compensation_state_nonblocking() {
    static std::atomic<int> last{static_cast<int>(PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION)};
    if (!phase_compensation) return std::nullopt;
    std::unique_lock<std::mutex> lock(phase_compensation->state_mutex, std::try_to_lock);
    if (lock.owns_lock()) last.store(static_cast<int>(phase_compensation->state), std::memory_order_relaxed);
    return static_cast<PhaseCompensatorState>(last.load(std::memory_order_relaxed));
}

void reset_phase_state_locked(PhaseCompensatorState state) {
    phase_compensation->state = state;
    // Any reset ends a retune cooldown (begin_retune_cooldown re-arms it after
    // resetting). A coherence recovery that interrupted one used to leave the
    // flag set, so the status reported "cooldown" indefinitely.
    phase_compensation->cooldown_active = false;
    phase_compensation->compensation_applied = false;
    phase_compensation->convergence_count = 0;
    phase_compensation->stable_nonzero_count = 0;
    phase_compensation->failed_convergence_attempts = 0;
    phase_compensation->checks_since_compensation = 0;
    // The per-bin equalizer must be re-measured; stop applying any stale FIR
    // immediately. Clearing ready under state_mutex (the same lock the design
    // uses to publish) makes the recal-vs-design race safe.
    phase_compensation->per_bin_measured = false;
    per_bin_cal.ready.store(false, std::memory_order_release);
    // Identity for every channel. The reference entry is never written (apply
    // and verify skip REF_CHANNEL), so this is equivalent to resetting only the
    // non-reference entries.
    for (int i = 0; i < NUM_DEVICES; ++i) {
        phase_compensation->compensation_vector.store(i, Complex(1.0f, 0.0f));
    }
}

std::atomic<bool> scanner_cal_deferred{false};

void recovery_settings_changed_locked(const char* what) {
    if (!recovery_in_progress.load(std::memory_order_acquire)) return;
    const auto st = phase_compensation->state;
    if (st == PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION ||
        st == PhaseCompensatorState::CONVERGED) {
        std::cout << what << " during coherence recovery - covered by the full recal" << std::endl;
        return;
    }
    reset_phase_state_locked(PhaseCompensatorState::MEASURING_INITIAL_PHASE);
    phase_compensation->noise_on_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);
    clear_l2_buffer();  // state -> l2 flush: same order as check_phase_convergence
    std::cerr << what << " during recalibration - phase measurement restarted at the new settings" << std::endl;
}

void recovery_settings_changed(const char* what) {
    if (!phase_compensation || !recovery_in_progress.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
    recovery_settings_changed_locked(what);
}

void begin_retune_cooldown(const char* what) {
    // --kerberos: no automatic recalibration after a settings change (the
    // noise couples into the connected antennas). Keep the old compensation
    // applied - approximately valid nearby - and mark it STALE for the UIs.
    // Owned here so EVERY caller (frequency, gain, mixer side, antenna ring)
    // gets it; gain/side/ring used to run the cooldown, wiping the manual
    // calibration and parking the machine in WAITING_FOR_STABILITY for good.
    if (kerberos_manual_cal_only()) {
        if (get_phase_compensation_state() == PhaseCompensatorState::CONVERGED) {
            kerberos_cal_stale.store(true, std::memory_order_release);
            std::cerr << "KerberosSDR: " << what << " - calibration is STALE. "
                         "Disconnect antennas and press Recalibrate." << std::endl;
        }
        recovery_settings_changed(what);  // a manual recal running: restart its phase stage
        return;
    }

    // Wideband scan has no lag/phase calibration (the tuners sit on different
    // frequencies), and leaving the scan recalibrates from scratch. A cooldown
    // started here would expire into handle_settings_change() and switch the
    // noise source on for a calibration that can't run until the scan ends.
    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) return;

    // Discrete scanner hopping: a calibration can't converge across hops, and
    // the cooldown's expiry used to switch the noise source on into the
    // scanned data. Keep the current calibration applied and recalibrate once
    // the scan stops (the lag driver watches scanner_cal_deferred).
    if (discrete_scanner.enabled.load()) {
        scanner_cal_deferred.store(true, std::memory_order_release);
        std::cout << what << " during a discrete scan - recalibration deferred until the scan stops" << std::endl;
        return;
    }

    if (!phase_compensation) return;
    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
    // Re-checked under the lock: callers test recovery_in_progress first, but
    // a recovery starting in between would otherwise have its noise source
    // switched off and its phase machine parked in WAITING_FOR_STABILITY,
    // where handle_settings_change() then refused every tick until the 120 s
    // abort. The recovery recalibrates at the new settings anyway.
    if (recovery_in_progress.load(std::memory_order_acquire)) {
        recovery_settings_changed_locked(what);
        return;
    }
    set_bias_tee_all_devices(false, devices);  // state -> device_io: documented lock order
    reset_phase_state_locked(PhaseCompensatorState::WAITING_FOR_STABILITY);
    phase_compensation->last_frequency_change = std::chrono::steady_clock::now();
    phase_compensation->cooldown_active = true;
    std::cout << what << ": entering " << phase_compensation->stability_delay_override_ms.load()
              << "ms cooldown (bias tee OFF)" << std::endl;
}

void reset_lag_compensation_all_channels() {
    std::cout << "Resetting lag compensation for all channels..." << std::endl;

    for (const auto& device : devices) {
        if (device && device->index != REF_CHANNEL) {
            std::lock_guard<std::mutex> lock(device->compensation_mutex);

            // Reset the state machine (MEASURING re-engages the servo, which
            // zeroes the correction register and its own counters on entry -
            // but only when a FRESH reading reaches it). Zero a live servo
            // correction here too: a parked machine (FFT off: kerberos idle,
            // 120 s abort) never gets that reading, and the channel kept up to
            // ~357 ppm of sample-clock skew until its L1 overflowed.
            if (device->compensation.servo_counts != 0) {
                std::lock_guard<std::recursive_mutex> io(device_io_mutex);
                if (device->dev) rtlsdr_set_sample_freq_correction_f(device->dev, 0.0f);
                device->compensation.servo_counts = 0;
            }
            device->compensation.state = LagCompensatorState::MEASURING;
            device->compensation.zero_lag_count = 0;
            device->compensation.lag_compensation_locked = false;
            device->compensation.frac_delay = 0.0f;

            std::cout << "Channel " << device->index << " lag compensation unlocked and reset" << std::endl;
        }
    }

    // Clear buffers to start fresh
    clear_l2_buffer();
    clear_l1_buffer();
}

const char* kerberos_calibration_state() {
    if (recovery_in_progress.load(std::memory_order_acquire)) return "calibrating";
    if (get_phase_compensation_state() == PhaseCompensatorState::CONVERGED) {
        return kerberos_cal_stale.load(std::memory_order_acquire) ? "stale" : "calibrated";
    }
    return "uncalibrated";
}

// --kerberos: park in the uncalibrated idle state (see compensation.hpp).
// Mirrors the watchdog's 120s-abort end state: with FFT disabled the
// correlation feeds zeroed phases, so neither the lag servo nor the phase
// machine can converge (or latch a bogus calibration) until a manual
// recalibration re-enables everything through recover_coherence(true).
void kerberos_enter_uncalibrated(const char* reason) {
    set_bias_tee_all_devices(false, devices);
    // Parked, not calibrating: the steady-state L2-raw cushion (C5). The boot
    // value is the tight calibration cap, which otherwise stayed for the whole
    // uncalibrated run.
    l2_raw_cap.store(L2_RAW_MAX, std::memory_order_relaxed);

    reset_lag_compensation_all_channels();  // lag -> MEASURING + L1/L2 flush
    for (const auto& device : devices) {
        if (device) {
            std::lock_guard<std::mutex> lock(device->compensation_mutex);
            device->compensation.initial_calibration_complete = false;
        }
    }

    if (phase_compensation) {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        reset_phase_state_locked(PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION);
    }

    {
        std::lock_guard<std::mutex> fl(fft_control.control_mutex);
        fft_control.fft_enabled = false;
        fft_control.auto_disabled = true;
        fft_control.user_override = false;
    }

    kerberos_cal_stale.store(false, std::memory_order_release);

    std::cerr << "KerberosSDR: UNCALIBRATED (" << reason
              << "). Disconnect all antennas, then press Recalibrate in the web UI."
              << std::endl;
}

// Full coherence recovery after a detected desync. This is the heavy, robust
// path: it re-engages the noise source and re-runs lag+phase calibration from
// the startup state - the only recovery guaranteed to fix an arbitrary
// full-packet slip (phase compensation cannot represent it, and a locked lag
// state will not re-measure on its own). Runs only on the watchdog thread.
void recover_coherence(bool manual) {
    // Under state_mutex: begin_retune_cooldown / the periodic check's restore
    // test the flag under it and then switch the noise source off - set
    // outside it, one could pass the test just before the recovery started and
    // turn its noise off, stalling it until the 120 s abort
    if (phase_compensation) {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        recovery_in_progress.store(true, std::memory_order_release);
    } else {
        recovery_in_progress.store(true, std::memory_order_release);
    }
    const uint32_t n = coherence_event_count.load(std::memory_order_relaxed);
    std::cerr << "Coherence recovery #" << n << ": full flush + recalibration starting..." << std::endl;

    // --kerberos: an AUTOMATIC trigger (coherence loss, element-count change)
    // must never energize the noise source - with the antennas connected the
    // "calibration" would be against live signals. Flush and drop to the
    // uncalibrated idle state; the user recalibrates manually. A manual
    // trigger (FORCE_RECAL - the user confirmed the antennas are off) runs
    // the normal full recalibration below.
    if (kerberos_manual_cal_only() && !manual) {
        kerberos_enter_uncalibrated("coherence lost or devices reconfigured");
        recovery_in_progress.store(false, std::memory_order_release);
        return;
    }

    // Flush-only path. Two cases where a full noise-source recalibration must
    // NOT run: (a) wideband scan has no lag/phase calibration; (b) active
    // coherent scanning retunes+flushes every dwell, so a recal that needs a
    // stable frequency could never converge and would leave the noise source
    // stuck on across the scan. In both, flushing restarts positional alignment,
    // which is all that is meaningful.
    auto needs_flush_only = []() {
        return operating_mode.load() == OperatingMode::WIDEBAND_SCAN ||
               discrete_scanner.enabled.load();
    };
    if (needs_flush_only()) {
        // No calibration will run: the noise source must not stay on through
        // the scan (a device reopen just before switches it on)
        if (bias_tee_enabled.load()) set_bias_tee_all_devices(false, devices);
        clear_l1_buffer();
        clear_l2_buffer();
        std::cerr << "Coherence recovery: wideband/scanner active - buffers flushed, no recalibration" << std::endl;
        recovery_in_progress.store(false, std::memory_order_release);
        return;
    }

    // C5: a full recal is starting - tighten the L2-raw cap so the re-running
    // lag/phase loops see fresher data; complete_phase_calibration_locked restores it.
    l2_raw_cap.store(L2_RAW_CAL_MAX, std::memory_order_relaxed);

    // 1) Noise source on - calibration needs the broadband common signal.
    set_bias_tee_all_devices(true, devices);

    // 2) Reset every channel's lag state to MEASURING and flush L1+L2. The flush
    //    is what re-aligns the device streams to within the correlation window
    //    after a full-packet slip; the lag servo then nulls the residual.
    reset_lag_compensation_all_channels();

    // 3) Clear the per-channel calibration-complete latch so the lag state
    //    machine re-runs (CONVERGED short-circuits on it) and the bias tee is
    //    allowed to switch off again once phase reconverges.
    for (const auto& device : devices) {
        if (device) {
            std::lock_guard<std::mutex> lock(device->compensation_mutex);
            device->compensation.initial_calibration_complete = false;
        }
    }

    // 4) Reset phase compensation to the startup state (identity vector, waiting
    //    for lag to complete) and drop any stale per-bin equalizer.
    if (phase_compensation) {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        reset_phase_state_locked(PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION);
    }

    // Re-check the mode just before committing FFT-on. If wideband or scanning
    // started while we were resetting state (TOCTOU), abandon the recal and undo
    // the noise source so we don't fight their FFT-off / no-cal invariants. Lag
    // is left in MEASURING and phase in WAITING_FOR_LAG_COMPLETION, which matches
    // the state those transitions establish anyway.
    if (needs_flush_only()) {
        set_bias_tee_all_devices(false, devices);
        l2_raw_cap.store(L2_RAW_MAX, std::memory_order_relaxed);  // no calibration running
        std::cerr << "Coherence recovery: wideband/scanner started mid-recovery, abandoning recal" << std::endl;
        recovery_in_progress.store(false, std::memory_order_release);
        return;
    }

    // 5) Re-enable FFT - both the lag servo and phase calibration drive off it.
    {
        std::lock_guard<std::mutex> lock(fft_control.control_mutex);
        fft_control.fft_enabled = true;
        fft_control.auto_disabled = false;
        fft_control.user_override = false;
    }

    // recovery_in_progress stays TRUE here: calibration now runs asynchronously
    // over ~30-60s via the lag/phase state machines. It is cleared in
    // complete_phase_calibration_locked() once phase reconverges, so the flag
    // spans the whole recalibration for the UI alert.
    std::cerr << "Coherence recovery #" << n
              << ": recalibration armed (lag -> phase). DF output is invalid until reconverged." << std::endl;
}

// Watchdog thread: drives coherence recovery off the coherence_lost flag.
// Debounced so it runs only one recovery at a time, but an event that fires
// DURING a recovery is NOT lost - it is peeked (not consumed) and acted on once
// the current recovery finishes. A recovery that never converges is force-aborted
// after a cap, with the noise source switched back off so a wedged recal can't
// leave it energized on the good channels.
void coherence_watchdog() {
    std::cout << "Coherence watchdog started" << std::endl;
    std::chrono::steady_clock::time_point recovery_started{};

    while (global_running) {
        // Hold off entirely while an element-count reconfiguration owns the
        // devices: the pipeline is stopped, so a "stalled device" event latched
        // during the teardown is expected noise, and recover_coherence() must
        // not touch handles being closed/reopened. Events stay latched and are
        // consumed once the reconfiguration finishes (it clears the stale ones
        // it caused itself and arms a fresh recalibration).
        if (reconfig_in_progress.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        bool recovering = recovery_in_progress.load(std::memory_order_acquire);

        // Safety: a recalibration that never reaches CONVERGED (e.g. a dongle
        // stays unplugged, or there is no signal) must not wedge the watchdog.
        // Abort after a cap AND turn the noise source off so a stuck recovery
        // does not keep the calibration noise injected into the good channels.
        if (recovering &&
            std::chrono::duration_cast<std::chrono::seconds>(now - recovery_started).count() > 120) {
            std::cerr << "Coherence watchdog: recovery exceeded 120s, aborting (noise source off)" << std::endl;
            if (operating_mode.load() != OperatingMode::WIDEBAND_SCAN) {
                set_bias_tee_all_devices(false, devices);
                // Also stop the calibration pipeline. recover_coherence turned the
                // noise source on AND FFT on; turning only the noise source off
                // while the lag/phase machines keep running would let them latch a
                // calibration captured WITHOUT the noise source - nulling the real
                // DOA and reporting CONVERGED. With FFT disabled, correlation feeds
                // zeroed phases so the phase machine cannot converge; it stays in a
                // non-CONVERGED (uncalibrated, identity-compensation) state until a
                // new coherence event re-runs recovery.
                std::lock_guard<std::mutex> fl(fft_control.control_mutex);
                fft_control.fft_enabled = false;
                fft_control.auto_disabled = true;
                fft_control.user_override = false;
            }
            recovery_in_progress.store(false, std::memory_order_release);
            // No calibration is running now: back to the steady-state L2-raw
            // cushion (C5) - the tight calibration cap stayed until the next
            // successful convergence
            l2_raw_cap.store(L2_RAW_MAX, std::memory_order_relaxed);
            recovering = false;  // allow a fresh recovery below
        }

        // Consume the triggers only when we can act (not already recovering);
        // while a recovery is in progress, PEEK so a new desync OR a forced recal
        // fired mid-recovery stays latched and is acted on as soon as this
        // recovery completes. The 120s abort above runs first, so a force-abort in
        // the same tick lets us consume+act immediately rather than waiting another
        // tick. force_recalibration carries the periodic-check / web-UI "force
        // recalibration" requests through the same single-threaded recovery path.
        bool coh, forced;
        if (recovering) {
            coh    = coherence_lost.load(std::memory_order_acquire);
            forced = force_recalibration.load(std::memory_order_acquire);
        } else {
            coh    = coherence_lost.exchange(false, std::memory_order_acq_rel);
            forced = force_recalibration.exchange(false, std::memory_order_acq_rel);
        }

        if ((coh || forced) && !recovering) {
            if (forced && !coh)
                std::cout << "Coherence watchdog: forced recalibration requested" << std::endl;
            recovery_started = now;
            // forced == user-requested (FORCE_RECAL): in --kerberos mode that
            // is the only trigger allowed to run the noise-source calibration.
            recover_coherence(forced);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cout << "Coherence watchdog exiting" << std::endl;
}

// ---- Periodic calibration check ----

// Measure the current residual lag/phase (with the noise source on) and decide
// whether the calibration has drifted out of tolerance. Briefly engages the
// noise source + eigen monitoring if the user did not already have them on, then
// restores EXACTLY what it changed (anything the user already had on is left
// untouched). Sets force_recalibration if drifted. Runs on the periodic monitor
// thread only. Non-destructive: in CONVERGED the lag servo stays locked and no
// phase compensation is re-applied, so this only reads back the residual the
// existing post-calibration monitor already computes.
static void run_calibration_check(CorrelationResult& correlation_result) {
    using namespace std::chrono;

    // Snapshot what the user already had on.
    const bool noise_was_on = bias_tee_enabled.load();
    bool fft_was_enabled, fft_was_override;
    {
        std::lock_guard<std::mutex> fl(fft_control.control_mutex);
        fft_was_enabled  = fft_control.fft_enabled;
        fft_was_override = fft_control.user_override;
    }

    // Engage the measurement conditions. The CONVERGED-state monitor only
    // computes residual phases when FFT is enabled AND user_override is set (see
    // process_correlations()), so force both for the measurement window.
    if (!noise_was_on) set_bias_tee_all_devices(true, devices);
    {
        std::lock_guard<std::mutex> fl(fft_control.control_mutex);
        fft_control.fft_enabled   = true;
        fft_control.auto_disabled = false;
        fft_control.user_override = true;
    }

    // Bail if shutdown begins or a real coherence recovery preempts us (once armed
    // the recovery owns the noise/FFT state, so we must not fight it).
    auto preempted = []() {
        return !global_running.load() || recovery_in_progress.load(std::memory_order_acquire) ||
               reconfig_in_progress.load(std::memory_order_acquire) ||
               discrete_scanner.enabled.load();  // a scan started mid-check
    };
    auto sleep_slice = [&](milliseconds total) {
        const auto deadline = steady_clock::now() + total;
        while (steady_clock::now() < deadline && !preempted())
            std::this_thread::sleep_for(milliseconds(50));
    };

    // Settle: wait out the pre-noise USB samples + bias-tee/noise-diode turn-on so
    // the residual is measured on genuinely noise-illuminated data.
    sleep_slice(milliseconds(800));

    // Accumulate the residual over several fresh correlation snapshots so a single
    // noisy reading cannot flip the decision.
    constexpr int kWantSnapshots = 8;
    constexpr int kMinSnapshots  = 4;
    std::array<float, NUM_DEVICES> lag_abs_sum{};
    std::array<float, NUM_DEVICES> phase_abs_sum{};
    int snapshots = 0;
    uint64_t last_seq = 0;
    const auto deadline = steady_clock::now() + milliseconds(2500);
    while (snapshots < kWantSnapshots && steady_clock::now() < deadline && !preempted()) {
        auto st = get_phase_compensation_state();
        if (!st || *st != PhaseCompensatorState::CONVERGED) break;  // state changed under us
        {
            std::lock_guard<std::mutex> lock(correlation_result.data_mutex);
            // Only sets whose phases came from the eigen solve: placeholder
            // sets (zeros / previous values, published before the FFT
            // override took effect) counted as perfect readings and diluted
            // a real drift below the threshold
            if (correlation_result.data_sequence > last_seq && correlation_result.phases_measured) {
                last_seq = correlation_result.data_sequence;
                for (int ch = 0; ch < NUM_DEVICES; ++ch) {
                    if (ch == REF_CHANNEL) continue;
                    if (correlation_result.lags.count(ch))
                        lag_abs_sum[ch] += std::abs(correlation_result.lags[ch]);
                    if (correlation_result.phases.count(ch))
                        phase_abs_sum[ch] += std::abs(correlation_result.phases[ch]);
                }
                ++snapshots;
            }
        }
        std::this_thread::sleep_for(milliseconds(30));
    }

    // Restore exactly what we changed - but only while the calibration we
    // checked is still the live one. A retune cooldown, gain change, per-bin
    // toggle or coherence recovery started during the window moves the phase
    // machine off CONVERGED and owns the noise source and FFT from then on;
    // restoring over it switched them off under it, stalling it or leaving it
    // calibrating on the live antennas. state_mutex makes the check + restore
    // atomic against a calibration starting in between (same state -> FFT ->
    // device lock order as complete_phase_calibration_locked).
    bool still_converged = false;
    if (phase_compensation) {
        std::lock_guard<std::mutex> sl(phase_compensation->state_mutex);
        still_converged = phase_compensation->state == PhaseCompensatorState::CONVERGED &&
                          !recovery_in_progress.load(std::memory_order_acquire);
        if (still_converged) {
            {
                std::lock_guard<std::mutex> fl(fft_control.control_mutex);
                if (!fft_was_override) fft_control.user_override = false;
                if (!fft_was_enabled) {
                    fft_control.fft_enabled   = false;
                    fft_control.auto_disabled = true;
                }
            }
            if (!noise_was_on) set_bias_tee_all_devices(false, devices);
        } else if (!noise_was_on && operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
            // Superseded by wideband-scan entry, which owns nothing that needs
            // the noise source: undo our own noise-on (scan entry may have run
            // between the monitor's scan-mode check and our switch-on).
            set_bias_tee_all_devices(false, devices);
        }
    }
    if (!still_converged) {
        // The snapshots may straddle the new calibration's start, and it is
        // recalibrating anyway - no verdict.
        std::cout << "Periodic calibration check: superseded by a calibration in progress" << std::endl;
        last_cal_check_result = 3;
        return;
    }

    // Decide only if we gathered enough data.
    bool bad_lag = false, bad_phase = false;
    float worst_lag = 0.0f, worst_phase = 0.0f;
    if (snapshots >= kMinSnapshots) {
        for (int ch = 0; ch < NUM_DEVICES; ++ch) {
            if (ch == REF_CHANNEL) continue;
            const float avg_lag   = lag_abs_sum[ch]   / snapshots;
            const float avg_phase = phase_abs_sum[ch] / snapshots;
            worst_lag   = std::max(worst_lag, avg_lag);
            worst_phase = std::max(worst_phase, avg_phase);
            if (avg_lag   > PERIODIC_RECAL_LAG_THRESHOLD)   bad_lag   = true;
            if (avg_phase > PERIODIC_RECAL_PHASE_THRESHOLD) bad_phase = true;
        }
    }
    const bool bad = bad_lag || bad_phase;

    if (snapshots < kMinSnapshots) {
        std::cout << "Periodic calibration check: skipped (only " << snapshots
                  << " snapshots; system busy)" << std::endl;
        last_cal_check_result = 3;
        return;
    }
    last_cal_check_lag = worst_lag;
    last_cal_check_phase = worst_phase;
    last_cal_check_result = bad ? 2 : 1;

    if (bad) {
        if (bad_lag)   periodic_recal_lag_fail_count.fetch_add(1, std::memory_order_relaxed);
        if (bad_phase) periodic_recal_phase_fail_count.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "Periodic calibration check: DRIFTED ("
                  << (bad_lag ? "lag " : "") << (bad_phase ? "phase " : "")
                  << "out of tolerance; worst |lag|=" << worst_lag
                  << " samp, |phase|=" << worst_phase << " deg) - requesting recalibration"
                  << " [lag fails=" << periodic_recal_lag_fail_count.load(std::memory_order_relaxed)
                  << ", phase fails=" << periodic_recal_phase_fail_count.load(std::memory_order_relaxed)
                  << "]" << std::endl;
        force_recalibration.store(true, std::memory_order_release);
    } else {
        std::cout << "Periodic calibration check: OK (worst |lag|=" << worst_lag
                  << " samp, |phase|=" << worst_phase << " deg)" << std::endl;
    }
}

void periodic_calibration_monitor(CorrelationResult& correlation_result) {
    using namespace std::chrono;
    std::cout << "Periodic calibration monitor started" << std::endl;
    auto last_check = steady_clock::now();

    while (global_running) {
        std::this_thread::sleep_for(milliseconds(500));
        if (!global_running) break;

        // An on-demand check (Check Calibration button): runs even with the
        // periodic check disabled, under the same conditions
        const bool manual = calibration_check_requested.exchange(false, std::memory_order_acq_rel);

        // --kerberos: the check would energize the noise source with antennas
        // connected - never run it, regardless of the enable setting.
        if (kerberos_manual_cal_only()) {
            if (manual) {
                std::cerr << "Calibration check: not available in KerberosSDR manual mode "
                             "(use Full Recalibration with the antennas disconnected)" << std::endl;
                last_cal_check_result = 3;
            }
            last_check = steady_clock::now();
            continue;
        }
        if (!manual && !periodic_recal_enabled.load(std::memory_order_relaxed)) {
            last_check = steady_clock::now();  // hold the timer while disabled
            continue;
        }

        // Only check in steady coherent operation: coherent mode, not scanning,
        // not mid-recovery, no forced recal already pending, and phase CONVERGED.
        // Anything else resets the timer so the period counts only steady CONVERGED
        // time and we never check right after (or during) a recalibration.
        auto st = get_phase_compensation_state();
        const bool checkable =
            operating_mode.load() == OperatingMode::COHERENT &&
            !discrete_scanner.enabled.load() &&
            !recovery_in_progress.load(std::memory_order_acquire) &&
            !reconfig_in_progress.load(std::memory_order_acquire) &&
            !force_recalibration.load(std::memory_order_acquire) &&
            st && *st == PhaseCompensatorState::CONVERGED;

        if (!checkable) {
            if (manual) {
                std::cout << "Calibration check: not now - calibration in progress, scanning or not yet calibrated" << std::endl;
                last_cal_check_result = 3;
            }
            last_check = steady_clock::now();
            continue;
        }

        const int period_min = std::max(1, periodic_recal_minutes.load(std::memory_order_relaxed));
        if (manual || steady_clock::now() - last_check >= minutes(period_min)) {
            if (manual) std::cout << "Calibration check: starting (requested)" << std::endl;
            else std::cout << "Periodic calibration check: starting (every " << period_min << " min)" << std::endl;
            cal_check_running = true;
            run_calibration_check(correlation_result);
            cal_check_running = false;
            last_check = steady_clock::now();
        }
    }
    std::cout << "Periodic calibration monitor exiting" << std::endl;
}

bool process_channel_lag_compensation(int channel, float lag) {
    // Skip lag compensation in wideband scan mode
    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) return false;

    auto device_it = std::find_if(devices.begin(), devices.end(),
        [channel](const auto& dev) { return dev && dev->index == channel; });

    if (device_it == devices.end()) return false;

    auto& device = *device_it;
    std::lock_guard<std::mutex> lock(device->compensation_mutex);
    auto& comp = device->compensation;
    auto now = std::chrono::steady_clock::now();

    // Lag ACQUISITION (MEASURING / SERVOING) only runs on the noise source.
    // Parked states - kerberos uncalibrated idle, the watchdog's 120 s abort, a
    // calibration stood down for a discrete scan - rely on FFT off to idle the
    // machines; turning the FFT on (web UI checkbox) used to let the servo
    // acquire on the live antenna signal and the phase stages then latch a
    // "calibration" against it. A servo caught with the noise off is parked
    // (register zeroed) so it can't hold a clock skew.
    if (comp.state != LagCompensatorState::CONVERGED && !bias_tee_enabled.load(std::memory_order_acquire)) {
        if (comp.state == LagCompensatorState::SERVOING && comp.servo_counts != 0) {
            {
                std::lock_guard<std::recursive_mutex> io(device_io_mutex);
                if (device->dev) rtlsdr_set_sample_freq_correction_f(device->dev, 0.0f);
            }
            comp.servo_counts = 0;
            comp.state = LagCompensatorState::MEASURING;
            std::cerr << "Channel " << channel << ": noise source off mid-acquisition - "
                         "lag servo parked (register zeroed)" << std::endl;
        }
        return false;
    }

    switch (comp.state) {
        case LagCompensatorState::SERVOING: {
            // Closed-loop proportional servo (see types.hpp): the correction
            // register stays live while the lag keeps being measured, on the
            // MEDIAN of the last 5 readings so single-set outliers cannot
            // steer the loop.
            //
            // Every register write kicks the lag by a systematically negative
            // amount (~-0.05..-0.15, per channel). All stop/nudge decisions
            // therefore use the PREDICTED post-write lag (med + kick), and
            // the kick is learned from each freeze: without this, negative
            // lags get kicked away from zero by the very write that should
            // finish them, and the servo orbits instead of locking.
            constexpr float kRegStep = 1.0f / 16777216.0f;        // 2^-24
            constexpr float count_rate = kRegStep * SAMPLE_RATE;  // samples/s per count

            if (!device->dev) break;

            comp.servo_readings[comp.servo_reading_count % 5] = lag;
            comp.servo_reading_count++;
            if (comp.servo_reading_count < 5) break;

            float r[5];
            std::copy(comp.servo_readings, comp.servo_readings + 5, r);
            std::nth_element(r, r + 2, r + 5);
            const float med = r[2];

            // Update cadence (see types.hpp): coarse flight acts on every
            // reading (rolling median); the endgame acts every 5th reading
            // so its median windows stay non-overlapping (the freeze/kick
            // logic was tuned on that cadence).
            const bool coarse = std::abs(med) >= 2.0f;
            if (!coarse &&
                comp.servo_reading_count - comp.servo_last_act_reading < 5) break;
            comp.servo_last_act_reading = comp.servo_reading_count;

            comp.servo_update_count++;
            comp.updates_since_write++;
            const bool median_settled = comp.updates_since_write >= 2;  // median fully post-write

            // Delay-aware effective gain (see types.hpp): total feedback
            // delay (pipeline transit + median window + update period)
            // scales with the control update period on a loaded machine, so
            // cap gain x period to keep the delayed loop overdamped.
            const float update_dt = std::clamp(
                std::chrono::duration<float>(now - comp.servo_last_update).count(), 0.05f, 5.0f);
            comp.servo_last_update = now;
            const float g_eff = std::min(comp.servo_gain, 0.15f / update_dt);

            // Learn the write kick: compare where the freeze actually landed
            // with where the lag was when the stop was commanded.
            if (comp.kick_pending && comp.servo_counts == 0 && median_settled) {
                float kick_obs = std::clamp(med - comp.med_at_freeze, -0.5f, 0.5f);
                comp.kick_estimate = std::clamp(0.7f * comp.kick_estimate + 0.3f * kick_obs, -0.4f, 0.1f);
                comp.kick_pending = false;
                std::cout << "Channel " << channel << " servo freeze: landed " << med
                          << " (kick=" << kick_obs << ", est=" << comp.kick_estimate << ")" << std::endl;
            }

            const float thr = comp.servo_convergence_threshold;
            int apply = comp.servo_counts;

            if (comp.servo_counts != 0) {
                // Actuating: stop when the PREDICTED post-write lag crosses
                // zero from this side of approach - aim for the center, not
                // the threshold edge, so freezes land with margin. One-sided
                // so a skipped-over crossing still stops.
                const float predicted = med + comp.kick_estimate;
                const bool stop = (comp.servo_counts > 0) ? (predicted <= 0.0f) : (predicted >= 0.0f);
                if (median_settled && stop) {
                    apply = 0;
                } else {
                    // refresh magnitude tier
                    const float amag = std::abs(med);
                    int counts = (amag < 1.0f) ? 1 : (amag < 2.0f) ? 4 :
                        std::max(1, std::min(comp.servo_max_counts,
                                 static_cast<int>(std::lround(g_eff * amag / count_rate))));
                    // Above the noise floor the drive direction must FOLLOW
                    // the measurement: at coarse authority the loop can
                    // overshoot zero mid-flight (rate x feedback delay), and
                    // keeping the entry direction would then run AWAY from
                    // zero with growing counts. Below 2 samples keep the
                    // original direction (endgame: noise flips med's sign
                    // and the freeze logic owns stopping there).
                    if (amag >= 2.0f) {
                        apply = (med > 0.0f) ? counts : -counts;
                    } else {
                        apply = (comp.servo_counts > 0) ? counts : -counts;
                    }
                }
            } else if (median_settled && std::abs(med) >= thr && !comp.kick_pending) {
                if (std::abs(med) < 0.75f) {
                    // Deterministic digital trim: fold the measured residual
                    // into the software fractional delay. No hardware write,
                    // no random kick - the correlation measures the corrected
                    // stream, so the reading nulls on the next windows.
                    const float cur = comp.frac_delay.load();
                    const float next = std::clamp(cur - med, -0.9f, 0.9f);
                    comp.frac_delay.store(next);
                    comp.updates_since_write = 0;  // let the median refresh
                    comp.zero_lag_count = 0;
                    std::cout << "Channel " << channel << " software frac delay: " << cur
                              << " -> " << next << " (residual " << med << ")" << std::endl;
                } else if (std::abs(med) >= 2.0f) {
                    // Coarse (re-)engage: go straight to the proportional
                    // rate - the careful +/-1 nudge below is endgame-only
                    // and would crawl at 0.14 samples/s from here.
                    const int counts = std::max(1, std::min(comp.servo_max_counts,
                        static_cast<int>(std::lround(g_eff * std::abs(med) / count_rate))));
                    apply = (med > 0.0f) ? counts : -counts;
                } else {
                    // Too large for the digital trim: hardware nudge,
                    // direction planned around both write kicks.
                    const float after_kicks = med + 2.0f * comp.kick_estimate;
                    if (after_kicks > 0.0f)      apply = 1;
                    else if (after_kicks < 0.0f) apply = -1;
                    else                         apply = (med > 0) ? 1 : -1;
                }
            }

            if (apply != comp.servo_counts) {
                // Coarse rolling updates would otherwise rewrite the register
                // on nearly every reading; gate on a meaningful change
                // (>= 12.5% or 2 counts). Sign changes, freezes and engages
                // always write; the endgame writes on any change (as before).
                const bool must_write = !coarse || apply == 0 || comp.servo_counts == 0 ||
                    ((apply > 0) != (comp.servo_counts > 0)) ||
                    std::abs(apply - comp.servo_counts) >=
                        std::max(2, std::abs(comp.servo_counts) / 8);
                if (must_write) {
                    if (apply == 0) {
                        comp.med_at_freeze = med;
                        comp.kick_pending = true;
                    }
                    {
                        // device_io_mutex (innermost): shutdown/reconfiguration
                        // close handles under it
                        std::lock_guard<std::recursive_mutex> io(device_io_mutex);
                        if (device->dev) rtlsdr_set_sample_freq_correction_f(device->dev, apply * kRegStep);
                    }
                    comp.servo_counts = apply;
                    comp.updates_since_write = 0;
                    comp.zero_lag_count = 0;  // the write glitches the lag; restart lock counting
                }
            }

            if ((comp.servo_update_count % 4) == 0) {
                std::cout << "Channel " << channel << " servo: med lag=" << med
                          << ", counts=" << comp.servo_counts
                          << ", kick_est=" << comp.kick_estimate << std::endl;
            }

            // Lock after 8 consecutive quiet control updates (~2s) with the
            // register at zero and the median inside the target.
            if (comp.servo_counts == 0 && median_settled && std::abs(med) < thr) {
                if (++comp.zero_lag_count >= 8) {
                    comp.state = LagCompensatorState::CONVERGED;
                    comp.lag_compensation_locked = true;
                    std::cout << "Channel " << channel << " LAG CONVERGED via servo! (med lag="
                              << med << ") [LOCKED]" << std::endl;
                    return true;
                }
            } else if (comp.servo_counts == 0 && median_settled) {
                comp.zero_lag_count = 0;
            }
            break;
        }

        case LagCompensatorState::MEASURING:
            // Entry/reset state: hand straight to the servo - it measures
            // while correcting, so no settling or pre-qualification of the
            // reading is needed. Zero the register first: after an
            // interrupted run or external reset it may hold a stale
            // correction, which the servo would otherwise see as unfixable
            // clock drift (it assumes counts=0).
            {
                std::lock_guard<std::recursive_mutex> io(device_io_mutex);
                if (device->dev) rtlsdr_set_sample_freq_correction_f(device->dev, 0.0f);
            }
            comp.state = LagCompensatorState::SERVOING;
            comp.servo_update_count = 0;
            comp.servo_counts = 0;
            comp.servo_last_update = now;
            comp.servo_last_act_reading = 0;
            comp.servo_reading_count = 0;
            comp.zero_lag_count = 0;
            // A kick observation pending from the previous run would compare
            // this run's first frozen median with that run's freeze point and
            // skew kick_estimate (the learned value itself - a property of
            // the dongle - is kept)
            comp.kick_pending = false;
            comp.med_at_freeze = 0.0f;
            comp.updates_since_write = 0;
            std::cout << "Channel " << channel << " servo engaged (lag=" << lag << ")" << std::endl;
            break;
            
        case LagCompensatorState::CONVERGED:
            // If initial calibration is complete, always stay converged
            if (device->compensation.initial_calibration_complete) return true;

            // CONVERGED always carries lag_compensation_locked (both are set
            // together at lock and cleared together by
            // reset_lag_compensation_all_channels(), all under
            // compensation_mutex), so never automatically recalibrate here.
            // Only log significant drift (throttled per channel).
            if (std::abs(lag) > comp.convergence_threshold * 2.0f) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - comp.last_drift_warning).count() > 10) {
                    std::cerr << "Channel " << channel << " lag drift detected (lag=" << lag
                              << "), but locked. Use 'reset_lag_compensation' to recalibrate." << std::endl;
                    comp.last_drift_warning = now;
                }
            }
            return true;
    }
    
    return false;
}

static constexpr int SERVO_STALE_MS = 1000;

// A servo left mid-flight with no fresh readings - the FFT switched off by the
// user, the watchdog's 120 s abort, KerberosSDR's uncalibrated state or
// wideband-scan entry - kept its sample-clock correction register set for
// good: a skew of up to ~357 ppm that walks the channel away from the others
// until L1 overflows (coherence loss). Zero the register and hand the channel
// back to MEASURING, which re-engages the servo cleanly when readings return.
// (A converged channel always has counts 0, so only SERVOING can be stale.)
static void park_stale_servo(int channel) {
    auto device_it = std::find_if(devices.begin(), devices.end(),
        [channel](const auto& dev) { return dev && dev->index == channel; });
    if (device_it == devices.end()) return;
    auto& device = *device_it;
    std::lock_guard<std::mutex> lock(device->compensation_mutex);
    auto& comp = device->compensation;
    if (comp.state != LagCompensatorState::SERVOING || comp.servo_counts == 0) return;
    {
        std::lock_guard<std::recursive_mutex> io(device_io_mutex);
        if (device->dev) rtlsdr_set_sample_freq_correction_f(device->dev, 0.0f);
    }
    std::cerr << "Channel " << channel << ": lag servo had no fresh readings for "
              << SERVO_STALE_MS << " ms (FFT off) - correction register zeroed ("
              << comp.servo_counts << " counts), back to MEASURING" << std::endl;
    comp.servo_counts = 0;
    comp.state = LagCompensatorState::MEASURING;
}

void channel_lag_compensation_processor(int channel, CorrelationResult& correlation_result) {
    std::cout << "Starting lag compensation for channel " << channel << std::endl;

    uint64_t last_processed_sequence = 0;
    auto last_fresh = std::chrono::steady_clock::now();

    while (global_running && pipeline_running.load(std::memory_order_acquire)) {
        std::optional<float> current_lag;
        std::map<int, float> current_phases;
        uint64_t current_sequence = 0;
        // With FFT off the correlation thread still advances data_sequence
        // (it drives the time-based states below, e.g. the retune cooldown)
        // but produces no measurement: data_ready is false, lags keep their
        // last values and phases are zeroed. Only a fresh reading may drive
        // the lag servo or a measuring phase state - a stale lag held the
        // servo's correction register on for good (e.g. FFT_DISABLE
        // mid-calibration, the watchdog's 120 s abort), and zeroed phases
        // read as a perfectly converged calibration.
        bool fresh = false;
        // Phases are only a calibration measurement once every channel's lag
        // is converged: before that the correlation thread publishes zeros
        // (with data_ready set), which read as a perfectly converged
        // calibration if a lag reset lands mid-VERIFYING.
        bool lag_all_converged = true;
        // ...and only if the correlation thread actually measured them: a set
        // computed while the phase machine was still in WAITING_FOR_LAG (or
        // any other non-measuring state) carries placeholder phases.
        bool phases_measured = false;

        {
            std::lock_guard<std::mutex> lock(correlation_result.data_mutex);
            current_sequence = correlation_result.data_sequence;
            if (current_sequence > last_processed_sequence) {
                fresh = correlation_result.data_ready;
                current_lag = correlation_result.lags[channel];
                current_phases = correlation_result.phases;
                phases_measured = correlation_result.phases_measured;
                for (const auto& [ch, st] : correlation_result.channel_states) {
                    if (ch != REF_CHANNEL && st != LagCompensatorState::CONVERGED) lag_all_converged = false;
                }
                last_processed_sequence = current_sequence;
            }
        }
        // ...and only while the noise source is on: the phase stages must never
        // calibrate on antenna signals (see the lag gate in
        // process_channel_lag_compensation for the parked states this covers).
        const bool phase_reading = fresh && lag_all_converged && phases_measured &&
                                   bias_tee_enabled.load(std::memory_order_acquire);

        if (current_lag) {
            const auto now = std::chrono::steady_clock::now();
            if (fresh) {
                last_fresh = now;
            } else if (now - last_fresh > std::chrono::milliseconds(SERVO_STALE_MS)) {
                park_stale_servo(channel);
                last_fresh = now;  // don't re-check every tick
            }
        }
        
        // A recalibration deferred because the discrete scanner was hopping
        // (begin_retune_cooldown / handle_settings_change / start_scanner):
        // run it once the scan stops, at the frequency it stopped on. Checked
        // here, before the CONVERGED + FFT-off skip below - that is the normal
        // idle state, and the deferred recal never ran from inside it. A
        // coherence recovery stood down by start_scanner is still in progress:
        // give it its noise source back so it resumes (begin_retune_cooldown
        // would defer to it, and it would sit until the 120 s abort).
        if (channel == (REF_CHANNEL == 0 ? 1 : 0) && !discrete_scanner.enabled.load() &&
            scanner_cal_deferred.exchange(false, std::memory_order_acq_rel)) {
            if (recovery_in_progress.load(std::memory_order_acquire)) {
                if (!bias_tee_enabled.load()) set_bias_tee_all_devices(true, devices);
                std::cout << "Discrete scan stopped: resuming the stood-down coherence recovery" << std::endl;
            } else {
                begin_retune_cooldown("Discrete scan stopped");
            }
        }

        if (current_lag) {
            // Skip ALL compensation processing if calibration is complete AND FFT is disabled
            if (auto current_state = get_phase_compensation_state();
                current_state && *current_state == PhaseCompensatorState::CONVERGED) {
                
                // Check if FFT is enabled (user wants continued processing)
                bool fft_enabled = false;
                {
                    std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
                    fft_enabled = fft_control.fft_enabled;
                }
                
                if (!fft_enabled) {
                    // Calibration complete and FFT disabled - stop all compensation processing
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    continue;
                }
                // If FFT is enabled, continue with compensation processing for monitoring
            }
            
            if (fresh) process_channel_lag_compensation(channel, *current_lag);
            
            if (channel == (REF_CHANNEL == 0 ? 1 : 0)) {
                if (auto current_state = get_phase_compensation_state()) {
                    switch (*current_state) {
                        case PhaseCompensatorState::WAITING_FOR_STABILITY: {
                            // Frequency stability cooldown - wait 3 seconds before starting calibration
                            if (phase_compensation) {
                                bool should_start_calibration = false;
                                {
                                    std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
                                    // Re-check state after acquiring mutex (TOCTOU protection)
                                    // Another thread may have updated frequency, resetting the timer
                                    if (phase_compensation->state != PhaseCompensatorState::WAITING_FOR_STABILITY) {
                                        break;  // State changed, don't proceed
                                    }

                                    auto now = std::chrono::steady_clock::now();
                                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - phase_compensation->last_frequency_change).count();

                                    int delay_ms = phase_compensation->stability_delay_override_ms.load();
                                    // Never start a calibration while the discrete
                                    // scanner hops - wait for it to stop.
                                    if (elapsed >= delay_ms && !discrete_scanner.enabled.load()) {
                                        std::cout << "Frequency stable for " << delay_ms << "ms, starting calibration..." << std::endl;
                                        phase_compensation->cooldown_active = false;
                                        should_start_calibration = true;
                                    }
                                }  // Release mutex before calling handle_settings_change()

                                if (should_start_calibration) {
                                    handle_settings_change();  // This resets to MEASURING_INITIAL_PHASE
                                }
                            }
                            break;
                        }
                        case PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION: {
                            // Under data_mutex: the correlation thread writes these
                            // entries and a reconfiguration clears the map
                            bool all_lag_converged;
                            {
                                std::lock_guard<std::mutex> rl(correlation_result.data_mutex);
                                all_lag_converged = std::all_of(correlation_result.channel_states.begin(),
                                    correlation_result.channel_states.end(),
                                    [](const auto& pair) { return pair.second == LagCompensatorState::CONVERGED; });
                            }

                            if (all_lag_converged && phase_compensation) {
                                std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
                                // Re-check state after acquiring mutex (TOCTOU protection)
                                // Another thread may have set WAITING_FOR_STABILITY
                                if (phase_compensation->state != PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION) {
                                    break;  // State changed, don't overwrite
                                }
                                phase_compensation->state = PhaseCompensatorState::MEASURING_INITIAL_PHASE;
                                phase_compensation->stable_nonzero_count = 0;
                                std::cout << "Phase: All channels lag converged" << std::endl;
                                clear_l2_buffer();
                            }
                            break;
                        }
                        case PhaseCompensatorState::MEASURING_INITIAL_PHASE:
                            if (phase_reading) apply_phase_compensation_once(current_phases);
                            break;
                        case PhaseCompensatorState::APPLYING_COMPENSATION:
                            // Increment 1: legacy pass-through. apply_phase_compensation_once
                            // now goes straight to VERIFYING_CONVERGENCE, so this state is no
                            // longer set. Funnel into the verify handler (not an empty flip)
                            // so a stray APPLYING value can never strand the state machine.
                            if (phase_reading) check_phase_convergence(current_phases);
                            break;
                        case PhaseCompensatorState::VERIFYING_CONVERGENCE:
                            if (phase_reading) check_phase_convergence(current_phases);
                            break;
                        case PhaseCompensatorState::MEASURING_PER_BIN: {
                            // Accumulate snapshots (correlation thread), then design the
                            // equalizer FIRs and finish. Bail out and finish WITHOUT the
                            // equalizer (scalar-only, ready stays false) if the user
                            // disabled per-bin, if FFT got turned off (no snapshots will
                            // arrive), or if we time out - so the machine can never hang.
                            const bool enabled = per_bin_cal.enabled.load(std::memory_order_relaxed);
                            const int snaps = per_bin_cal.snapshots.load(std::memory_order_acquire);
                            bool fft_on;
                            {
                                std::lock_guard<std::mutex> fl(fft_control.control_mutex);
                                fft_on = fft_control.fft_enabled;
                            }

                            const bool can_design = enabled && fft_on &&
                                snaps >= PerBinCalibration::REQUIRED_SNAPSHOTS;
                            const bool design_ok = can_design && design_per_bin_equalizers();

                            std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
                            if (phase_compensation->state != PhaseCompensatorState::MEASURING_PER_BIN)
                                break;  // a recalibration intervened; do not publish

                            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now() - phase_compensation->per_bin_start).count();
                            const bool timed_out = elapsed > 15;

                            if (design_ok) {
                                per_bin_cal.ready.store(true, std::memory_order_release);
                                complete_phase_calibration_locked();
                            } else if (!enabled || !fft_on || timed_out) {
                                std::cout << "Per-bin: finishing without equalizer (enabled=" << enabled
                                          << ", fft_on=" << fft_on << ", timed_out=" << timed_out << ")" << std::endl;
                                complete_phase_calibration_locked();
                            }
                            // else: keep waiting for more snapshots
                            break;
                        }
                        case PhaseCompensatorState::CONVERGED:
                            // No further phase compensation needed unless user re-enables FFT
                            break;
                    }
                }
            }
        }
        
        // Increment 2: faster cal-tick. 50 ms IS the lag-servo cadence (its 5-reading
        // median, 2-update post-write settle, and 8-update lock counters are tuned for
        // it), so keep it for every channel by default. EXCEPTION: the single channel
        // that drives the phase state machine polls faster (20 ms) WHILE a phase
        // calibration is actively stepping, so the MEASURING/VERIFYING reading gates
        // (which advance once per new correlation set, ~20-40 ms) are not throttled by
        // the tick. Strictly scoped to the driver channel AND the active-cal states, so
        // every other channel / state keeps its load-bearing 50 ms lag-servo cadence.
        int tick_ms = 50;
        if (channel == (REF_CHANNEL == 0 ? 1 : 0)) {
            if (auto st = get_phase_compensation_state(); st &&
                (*st == PhaseCompensatorState::MEASURING_INITIAL_PHASE ||
                 *st == PhaseCompensatorState::APPLYING_COMPENSATION ||
                 *st == PhaseCompensatorState::VERIFYING_CONVERGENCE)) {
                tick_ms = 20;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
    }
}