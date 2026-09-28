#include "correlation.hpp"
#include "../sdr/sdr_pipeline.hpp"
#include "../sdr/sdr_device.hpp"
#include "../dsp/fft_plan.hpp"
#include "../core/config.hpp"
#include "../core/utils.hpp"
#include "../core/buffer_pool.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <array>
#include <chrono>
#include <Eigen/Dense>
#include <Eigen/Eigenvalues>

// External buffer pool for L2 buffers (defined in sdr_pipeline.cpp)
extern BufferPool<std::vector<ComplexBuffer>> l2_buffer_pool;

#if ENABLE_COHERENCE_MONITOR
// ---- #5 streaming coherence monitor (default OFF; see config.h) --------------
//
// Differential cross-correlation backstop run at ~2 Hz on the already
// phase/lag-compensated set. A coherent channel peaks sharply at lag ~ 0 against
// the reference; a dropped USB packet shifts a channel by a full NUM_SAMPLES, so
// its block no longer overlaps the reference block in time and the zero-lag peak
// VANISHES (the correlation goes flat). "Flat" alone is ambiguous - it also
// happens on a quiet band - so the decision is DIFFERENTIAL: a common signal is
// only deemed present when at least one channel IS coherent with the reference
// (which also confirms the reference is good); only then is a channel that has
// lost its zero-lag coherence treated as a suspect. Persistence avoids acting on
// a single noisy snapshot. Known v1 blind spot: a drop on the reference channel
// de-correlates every channel at once (coherent_count == 0 -> no action);
// documented, acceptable for a default-off backstop. Runs only on the single
// correlation_processor thread.
static void maybe_run_coherence_monitor(const std::vector<ComplexBuffer>& complex_set) {
    // Only meaningful once channels are locked & coherent, coherent mode, and
    // never while a recovery is already recalibrating.
    if (operating_mode.load() != OperatingMode::COHERENT) return;
    if (recovery_in_progress.load(std::memory_order_acquire)) return;
    if (!phase_compensation) return;
    {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        if (phase_compensation->state != PhaseCompensatorState::CONVERGED) return;
    }

    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() < 500) return;
    last = now;

    const int num_channels = static_cast<int>(complex_set.size());
    // The differential test needs at least one OTHER non-reference channel to be
    // coherent before it will flag a suspect, so it cannot work with only a
    // single non-ref channel (a 2-element array): if that lone channel drops, it
    // is the only non-coherent one, signal_present stays false, and the drop is
    // never flagged. Require >= 3 elements; for 2-element arrays this backstop is
    // inert (an absolute single-channel check would be needed - not implemented).
    if (num_channels < 3) return;
    if (static_cast<int>(complex_set[REF_CHANNEL].size()) < NUM_SAMPLES) return;

    constexpr float kJumpSamples = 30.0f;   // |lag| above this == a gross slip
    constexpr float kPeakRatio   = 8.0f;    // peak/mean gate for "real common signal"
    constexpr int   kPersist     = 3;       // consecutive bad checks (~1.5 s) before acting

    auto* ref_plan  = fft_pool.acquire();
    auto* chan_plan = fft_pool.acquire();
    ref_plan->forward(complex_set[REF_CHANNEL], NUM_SAMPLES);  // CS/2 offset, matches the lag path
    auto* ref_fft = ref_plan->get_output();

    std::array<bool, NUM_DEVICES> evaluable{};
    std::array<bool, NUM_DEVICES> coherent{};
    int coherent_count = 0;

    for (int ch = 0; ch < num_channels && ch < NUM_DEVICES; ch++) {
        if (ch == REF_CHANNEL || !devices[ch]) continue;
        if (!devices[ch]->compensation.lag_compensation_locked.load(std::memory_order_relaxed)) continue;
        if (static_cast<int>(complex_set[ch].size()) < NUM_SAMPLES) continue;
        evaluable[ch] = true;

        chan_plan->forward(complex_set[ch]);
        chan_plan->cross_correlate(ref_fft, chan_plan->get_output());
        const auto* ctime = chan_plan->get_correlation_time();

        float peak_mag = 0.0f; int peak_idx = 0; double sum_mag = 0.0;
        for (int i = 0; i < CORRELATION_SIZE; i++) {
            const float m = std::abs(ctime[i]);
            sum_mag += m;
            if (m > peak_mag) { peak_mag = m; peak_idx = i; }
        }
        const float mean_mag = (sum_mag > 0.0) ? static_cast<float>(sum_mag / CORRELATION_SIZE) : 0.0f;
        const bool  strong   = (mean_mag > 0.0f) && (peak_mag >= kPeakRatio * mean_mag);
        const float lag      = static_cast<float>(peak_idx - CORRELATION_SIZE / 2);
        coherent[ch] = strong && (std::abs(lag) <= kJumpSamples);
        if (coherent[ch]) coherent_count++;
    }

    fft_pool.release(ref_plan);
    fft_pool.release(chan_plan);

    static std::array<int, NUM_DEVICES> strikes{};
    const bool signal_present = (coherent_count >= 1);
    for (int ch = 0; ch < num_channels && ch < NUM_DEVICES; ch++) {
        if (ch == REF_CHANNEL) continue;
        if (!evaluable[ch] || !signal_present) { strikes[ch] = 0; continue; }
        if (!coherent[ch]) {
            if (++strikes[ch] >= kPersist) {
                strikes[ch] = 0;
                signal_coherence_lost("streaming coherence monitor: channel lost zero-lag coherence");
            }
        } else {
            strikes[ch] = 0;
        }
    }
}
#endif  // ENABLE_COHERENCE_MONITOR

void initialize_correlation_result(CorrelationResult& result) {
    int num_elements = active_num_elements.load();
    result.correlation_data.resize(num_elements - 1);
    result.scale_factors.resize(num_elements - 1);
    std::for_each(result.correlation_data.begin(), result.correlation_data.end(),
                  [](auto& data) { data.resize(CORRELATION_SIZE); });

    for (int i = 0; i < num_elements; i++) {
        result.lags[i] = result.phases[i] = 0.0f;
        result.amplitudes[i] = 1.0f;  // Initialize amplitude corrections to 1.0 (no correction)
        if (i != REF_CHANNEL) {
            result.channel_states[i] = LagCompensatorState::MEASURING;
            result.channel_zero_counts[i] = 0;
        }
    }
}

// Gaussian interpolation for more accurate peak estimation
ParabolicPeak gaussian_interpolation(const std::complex<float>* data, int peak_idx, int data_size) {
    ParabolicPeak result;
    
    if (peak_idx <= 0 || peak_idx >= data_size - 1) {
        result.refined_index = static_cast<float>(peak_idx);
        result.refined_magnitude = std::abs(data[peak_idx]);
        result.refined_phase_complex = data[peak_idx];
        result.refined_phase_deg = std::arg(data[peak_idx]) * 180.0f / M_PI;
        return result;
    }
    
    // Get magnitudes of the three points
    float y1 = std::abs(data[peak_idx - 1]);
    float y2 = std::abs(data[peak_idx]);
    float y3 = std::abs(data[peak_idx + 1]);
    
    // Avoid log of zero or negative values
    const float epsilon = 1e-10f;
    y1 = std::max(y1, epsilon);
    y2 = std::max(y2, epsilon);
    y3 = std::max(y3, epsilon);
    
    // Take natural log for Gaussian fitting
    float ln_y1 = std::log(y1);
    float ln_y2 = std::log(y2);
    float ln_y3 = std::log(y3);
    
    // Check for degenerate case
    float denominator = ln_y1 - 2.0f * ln_y2 + ln_y3;
    if (std::abs(denominator) < epsilon) {
        // Fall back to simple peak position
        result.refined_index = static_cast<float>(peak_idx);
        result.refined_magnitude = y2;
        result.refined_phase_complex = data[peak_idx];
        result.refined_phase_deg = std::arg(data[peak_idx]) * 180.0f / M_PI;
        return result;
    }
    
    // Gaussian interpolation formula for position
    float delta = 0.5f * (ln_y1 - ln_y3) / denominator;
    delta = clamp(delta, -0.5f, 0.5f);
    
    result.refined_index = static_cast<float>(peak_idx) + delta;
    
    // Estimate peak magnitude using Gaussian model
    float a = ln_y2;
    float b = 0.5f * (ln_y3 - ln_y1);
    float c = 0.5f * denominator;
    
    // Peak of the fitted Gaussian (in log domain)
    float peak_ln_magnitude = a - (b * b) / (4.0f * c + epsilon);
    result.refined_magnitude = std::exp(peak_ln_magnitude);
    
    // Phase interpolation using Gaussian weighting
    if (std::abs(delta) < 1e-6f) {
        result.refined_phase_complex = data[peak_idx];
    } else {
        // Gaussian weights based on distance from refined peak
        float sigma_sq = 0.5f;  // Controls the width of the Gaussian weight
        float w1 = std::exp(-0.5f * (delta + 1.0f) * (delta + 1.0f) / sigma_sq);
        float w2 = std::exp(-0.5f * delta * delta / sigma_sq);
        float w3 = std::exp(-0.5f * (delta - 1.0f) * (delta - 1.0f) / sigma_sq);
        float w_sum = w1 + w2 + w3;
        
        // Weighted average of complex values
        result.refined_phase_complex = (w1 * data[peak_idx - 1] + 
                                       w2 * data[peak_idx] + 
                                       w3 * data[peak_idx + 1]) / w_sum;
    }
    
    result.refined_phase_deg = std::arg(result.refined_phase_complex) * 180.0f / M_PI;
    return result;
}

// Calculate phase and amplitude calibration using eigenvalue decomposition
// Returns phases (in degrees) and amplitudes (linear scale) for each channel
void calculate_phase_amplitude_calibration_eigen(
    const std::vector<ComplexBuffer>& complex_set,
    std::map<int, float>& phases,
    std::map<int, float>& amplitudes) {

    phases.clear();
    amplitudes.clear();

    const int num_channels = static_cast<int>(complex_set.size());

    // Initialize all phases and amplitudes
    for (int ch = 0; ch < num_channels; ch++) {
        phases[ch] = 0.0f;
        amplitudes[ch] = 1.0f;  // Default: no amplitude correction
    }

    // Need at least 2 channels for meaningful eigen decomposition
    if (num_channels < 2 || complex_set.empty()) {
        return;
    }

    // Determine the number of samples to use (use minimum across all channels)
    size_t num_samples = NUM_SAMPLES;
    for (const auto& channel : complex_set) {
        num_samples = std::min(num_samples, channel.size());
    }

    if (num_samples == 0) {
        return;
    }

    // Create spatial correlation matrix Rxx = X * X^H
    // where X is the [num_channels x num_samples] matrix of IQ samples
    Eigen::MatrixXcf Rxx = Eigen::MatrixXcf::Zero(num_channels, num_channels);

    // Build the correlation matrix
    for (int i = 0; i < num_channels; i++) {
        for (int j = 0; j < num_channels; j++) {
            std::complex<float> sum(0.0f, 0.0f);
            
            // Calculate dot product: sum(x_i * conj(x_j))
            for (size_t k = 0; k < num_samples; k++) {
                sum += complex_set[i][k] * std::conj(complex_set[j][k]);
            }
            
            Rxx(i, j) = sum / static_cast<float>(num_samples);
        }
    }
    
    // Perform eigenvalue decomposition
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcf> eigensolver(Rxx);
    
    if (eigensolver.info() != Eigen::Success) {
        std::cerr << "Eigenvalue decomposition failed!" << std::endl;
        return;
    }
    
    // Get eigenvalues and eigenvectors
    // Note: SelfAdjointEigenSolver returns eigenvalues in ascending order
    auto eigenvalues = eigensolver.eigenvalues();
    auto eigenvectors = eigensolver.eigenvectors();

    // The largest eigenvalue is the last one (they're sorted in ascending order)
    int max_idx = num_channels - 1;

    // Get the dominant eigenvector (corresponds to largest eigenvalue)
    Eigen::VectorXcf dominant_eigenvector = eigenvectors.col(max_idx);

    std::cout << "Eigenvalues (sorted ascending): ";
    for (int i = 0; i < num_channels; i++) {
        std::cout << eigenvalues(i) << " ";
    }
    std::cout << "\nUsing eigenvector " << max_idx << " with eigenvalue " << eigenvalues(max_idx) << std::endl;

    // Calculate IQ differences (phase calibration factors)
    // iq_diffs = 1 / dominant_eigenvector
    Eigen::VectorXcf iq_diffs(num_channels);
    for (int i = 0; i < num_channels; i++) {
        if (std::abs(dominant_eigenvector(i)) > 1e-10f) {
            iq_diffs(i) = std::complex<float>(1.0f, 0.0f) / dominant_eigenvector(i);
        } else {
            iq_diffs(i) = std::complex<float>(1.0f, 0.0f);
        }
    }

    // Normalize to reference channel
    std::complex<float> ref_value = iq_diffs(REF_CHANNEL);
    if (std::abs(ref_value) > 1e-10f) {
        for (int i = 0; i < num_channels; i++) {
            iq_diffs(i) /= ref_value;
        }
    }

    // Extract phases and amplitudes from the complex IQ differences
    // The iq_diffs represent the channel mismatches that need to be corrected
    for (int ch = 0; ch < num_channels; ch++) {
        float phase_rad = std::arg(iq_diffs(ch));
        phases[ch] = -phase_rad * 180.0f / M_PI;  // Convert to degrees

        // Extract amplitude correction factor
        float amplitude = std::abs(iq_diffs(ch));

        // Limit amplitude correction to reasonable range (0.5x to 2x = ±6dB)
        // to avoid numerical instability from extreme values
        amplitude = std::max(0.5f, std::min(2.0f, amplitude));
        amplitudes[ch] = amplitude;

        // Debug output for verification
        if (ch != REF_CHANNEL) {
            std::cout << "Ch" << ch << ": eigenvec=" << dominant_eigenvector(ch)
                     << " iq_diff=" << iq_diffs(ch)
                     << " Amp=" << 20*std::log10(amplitude) << "dB"
                     << " Phase=" << phases[ch] << "°" << std::endl;
        }
    }
}

void process_correlations(CorrelationResult& correlation_result, FFTProcessingControl& fft_control) {
    std::vector<ComplexBuffer> complex_set;

    // Try to dequeue from L2 buffer (non-blocking)
    if (!l2_buffer.try_dequeue(complex_set)) {
        return;
    }
    l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);

    // Drain to the newest set: the producer outpaces this loop, so the queue
    // runs ~100 sets (~0.7s) deep and the head is always stale - which made
    // every lag measurement ~0.7s behind reality (the feedback delay that
    // forced long rests after slides). The backlog gets dropped by the
    // producer anyway; processing only the newest set removes the latency
    // without losing anything.
    {
        std::vector<ComplexBuffer> newer;
        while (l2_buffer.try_dequeue(newer)) {
            l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
            l2_buffer_pool.release(std::move(complex_set));
            complex_set = std::move(newer);
        }
    }

    // Get number of active channels from actual data size
    const int num_channels = static_cast<int>(complex_set.size());
    if (num_channels < 2) {
        return;  // Need at least 2 channels
    }

    // RAII helper to return buffer to pool on function exit
    struct BufferGuard {
        std::vector<ComplexBuffer>& buf;
        ~BufferGuard() { l2_buffer_pool.release(std::move(buf)); }
    } buffer_guard{complex_set};

    // Check if FFT processing is enabled
    if (!fft_control.fft_enabled) {
        return;
    }

    // Acquire FFT plans from pool
    auto ref_plan = fft_pool.acquire();
    std::vector<FftPlan*> channel_plans;
    for (int i = 0; i < num_channels - 1; i++) {
        channel_plans.push_back(fft_pool.acquire());
    }

    // Process reference channel
    ref_plan->forward(complex_set[REF_CHANNEL], NUM_SAMPLES);
    auto* ref_fft = ref_plan->get_output();

    // Process other channels - maintain proper channel-to-plan mapping
    std::vector<std::complex<float>*> channel_ffts(num_channels, nullptr);
    int plan_idx = 0;
    for (int ch = 0; ch < num_channels; ch++) {
        if (ch != REF_CHANNEL) {
            channel_plans[plan_idx]->forward(complex_set[ch]);
            channel_ffts[ch] = channel_plans[plan_idx]->get_output();
            plan_idx++;
        }
    }

    // Optional per-bin phase calibration: while in the MEASURING_PER_BIN window
    // (noise source + FFT still on), accumulate the per-bin cross-spectrum
    // X_i(f)*conj(X_ref(f)) and the reference power |X_ref(f)|^2. This is the
    // only added work, and the cross-spectrum is essentially the same product
    // the lag correlation already forms. Single writer (this thread); the
    // designer reads only after snapshots reaches REQUIRED (release/acquire).
    if (per_bin_cal.enabled.load(std::memory_order_relaxed)) {
        bool measuring;
        {
            std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
            measuring = (phase_compensation->state == PhaseCompensatorState::MEASURING_PER_BIN);
        }
        if (measuring
            && per_bin_cal.num_channels == num_channels
            && per_bin_cal.num_bins == CORRELATION_SIZE
            && per_bin_cal.snapshots.load(std::memory_order_acquire) < PerBinCalibration::REQUIRED_SNAPSHOTS) {
            // The reference is forward-transformed at offset NUM_SAMPLES (= CS/2)
            // for the lag path, so ref_fft[k] = (-1)^k * X_ref0[k] while the
            // channels are transformed at offset 0. Cancel that (-1)^k here so
            // the cross-spectrum is the true X_i0*conj(X_ref0) (otherwise the
            // designed equalizer impulse lands CS/2 samples away and is garbage).
            for (int ch = 0; ch < num_channels; ch++) {
                const std::complex<float>* fft = (ch == REF_CHANNEL) ? ref_fft : channel_ffts[ch];
                if (!fft) continue;
                auto& acc = per_bin_cal.cross_acc[ch];
                for (int k = 0; k < CORRELATION_SIZE; k++) {
                    const float sign = (k & 1) ? -1.0f : 1.0f;
                    acc[k] += fft[k] * std::conj(ref_fft[k]) * sign;
                }
            }
            for (int k = 0; k < CORRELATION_SIZE; k++) {
                per_bin_cal.ref_power[k] += static_cast<double>(std::norm(ref_fft[k]));
            }
            per_bin_cal.snapshots.fetch_add(1, std::memory_order_release);
        }
    }

    // Correlation processing for lag detection
    std::map<int, float> current_lags;
    current_lags[REF_CHANNEL] = 0.0f;
    
    // Process each channel against reference for lag detection. The per-pair
    // correlation traces and peak scales are staged here and swapped into
    // correlation_result under data_mutex below: the web thread reads those
    // vectors under the lock, and an element-count reconfiguration resizes them.
    static thread_local std::vector<float> staged_scale;
    static thread_local std::vector<std::vector<float>> staged_corr;
    staged_scale.resize(num_channels - 1);
    staged_corr.resize(num_channels - 1);
    for (auto& trace : staged_corr) trace.resize(CORRELATION_SIZE);
    int corr_idx = 0;
    for (int ch = 0; ch < num_channels; ch++) {
        if (ch == REF_CHANNEL) continue;

        // Cross-correlate using the first available plan
        auto* corr_plan = channel_plans[0];
        corr_plan->cross_correlate(ref_fft, channel_ffts[ch]);
        auto* correlation_time = corr_plan->get_correlation_time();
        
        auto max_it = std::max_element(correlation_time, correlation_time + CORRELATION_SIZE,
                                 [](const auto& a, const auto& b) { return std::abs(a) < std::abs(b); });
        
        int peak_index = std::distance(correlation_time, max_it);
        ParabolicPeak refined_peak = gaussian_interpolation(correlation_time, peak_index, CORRELATION_SIZE);

        float lag = refined_peak.refined_index - static_cast<float>(CORRELATION_SIZE / 2);

        // Refine the lag from the cross-spectrum phase ramp. A lag L puts a
        // phase of -2*pi*k*(L - CS/2)/CS on bin k (the CS/2 comes from the
        // reference's half-buffer offset). Derotate by the coarse peak
        // estimate so the residual ramp is small and wrap-free, then fit it
        // with weighted least squares across the whole band: the slope
        // leverage of the full 2.4 MHz span gives ~1e-3 sample precision,
        // and |S|^2 weights make faded bins (garbage phase) harmless.
        // Adjacent-bin product schemes (tried first) fail here: the per-bin
        // delay phase is ~1e-5 rad, so either weight fluctuations or faded
        // bins swamp it with noise.
        {
            const auto* cross_spec = corr_plan->get_correlation_freq();

            // Weight cap so a few very strong bins can't dominate the fit
            double pow_sum = 0.0;
            for (int k = 0; k < CORRELATION_SIZE; k++) {
                pow_sum += std::norm(cross_spec[k]);
            }
            const double w_cap = 4.0 * pow_sum / CORRELATION_SIZE;

            // Derotate by the INTEGER part of the coarse lag (the DFT shift
            // theorem in bin index k is exact only for integer shifts). The
            // remaining FRACTIONAL delay rotates the phase along PHYSICAL
            // frequency, which in FFT bin order is a V: +slope for k<CS/2,
            // -slope for the negative-frequency upper half. Fitting against
            // bin index instead of physical frequency (the original bug)
            // makes the fit collapse onto bogus fixed points.
            const int m = static_cast<int>(std::lround(lag));
            const double step = 2.0 * M_PI *
                (static_cast<double>(m) - CORRELATION_SIZE / 2) / CORRELATION_SIZE;
            const std::complex<double> rot_step(std::cos(step), std::sin(step));
            std::complex<double> rot(1.0, 0.0);

            // Pass 1: integer derotation + find the CONSTANT inter-channel
            // phase (random tuner PLL phase at startup). It must be removed
            // before fitting or channels whose constant phase sits near
            // +/-pi wrap in atan2 and corrupt the slope.
            static thread_local std::vector<std::complex<float>> zbuf;
            zbuf.resize(CORRELATION_SIZE);
            std::complex<double> mean_acc(0.0, 0.0);
            for (int k = 0; k < CORRELATION_SIZE; k++) {
                const std::complex<double> z = std::complex<double>(cross_spec[k]) * rot;
                rot *= rot_step;
                if ((k & 0x3FF) == 0x3FF) rot /= std::abs(rot);  // fight drift
                zbuf[k] = std::complex<float>(z);
                mean_acc += z;
            }
            const double phi0 = (std::abs(mean_acc) > 0.0) ? std::arg(mean_acc) : 0.0;
            const std::complex<float> deph(static_cast<float>(std::cos(-phi0)),
                                           static_cast<float>(std::sin(-phi0)));

            // Pass 2: weighted LS fit of residual phase vs PHYSICAL frequency
            double sw = 0.0, swk = 0.0, swp = 0.0, swkk = 0.0, swkp = 0.0;
            for (int k = 0; k < CORRELATION_SIZE; k++) {
                const std::complex<float> z = zbuf[k] * deph;
                const double w = std::min(static_cast<double>(std::norm(z)), w_cap);
                const double psi = std::atan2(z.imag(), z.real());
                const double fk = (k < CORRELATION_SIZE / 2)
                    ? static_cast<double>(k)
                    : static_cast<double>(k - CORRELATION_SIZE);
                sw += w; swk += w * fk; swp += w * psi;
                swkk += w * fk * fk; swkp += w * fk * psi;
            }

            const double denom = swkk - swk * swk / std::max(sw, 1e-30);
            if (sw > 0.0 && denom > 0.0) {
                const double slope = (swkp - swk * swp / sw) / denom;
                const float delta = static_cast<float>(-slope * CORRELATION_SIZE / (2.0 * M_PI));


                // The residual is relative to the integer peak m; it must be
                // sub-sample if the peak was right - otherwise keep the
                // Gaussian estimate (weak/odd signal)
                if (std::abs(delta) < 0.75f) {
                    lag = static_cast<float>(m) + delta;
                }
            }
        }
        
        current_lags[ch] = lag;
        
        // Store correlation data at the correct index
        staged_scale[corr_idx] = refined_peak.refined_magnitude;
        float scale = refined_peak.refined_magnitude > 0 ? 1.0f / refined_peak.refined_magnitude : 1.0f;
        
        std::transform(correlation_time, correlation_time + CORRELATION_SIZE,
                 staged_corr[corr_idx].begin(),
                 [scale](const auto& val) { return std::abs(val) * scale; });
        
        corr_idx++;
    }
    
    // Phase and amplitude calculation logic
    std::map<int, float> current_phases;
    std::map<int, float> current_amplitudes;

    // Check if we're in post-calibration monitoring mode
    bool post_calibration_monitoring = false;
    if (phase_compensation) {
        std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
        post_calibration_monitoring = (phase_compensation->state == PhaseCompensatorState::CONVERGED);
    }

    if (post_calibration_monitoring) {
        // Post-calibration: Only calculate phases if user has enabled FFT for monitoring
        bool user_wants_monitoring = false;
        {
            std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
            user_wants_monitoring = fft_control.fft_enabled && fft_control.user_override;
        }

        if (user_wants_monitoring) {
            // User explicitly enabled FFT for monitoring - calculate phases and amplitudes
            calculate_phase_amplitude_calibration_eigen(complex_set, current_phases, current_amplitudes);
            static int monitor_calc_count = 0;
            if (++monitor_calc_count % 20 == 1) {
                std::cout << "Post-calibration monitoring #" << monitor_calc_count << " - ";
                for (const auto& [ch, phase] : current_phases) {
                    if (ch != REF_CHANNEL) {
                        std::cout << "Ch" << ch << ":" << phase << "° ";
                    }
                }
                std::cout << std::endl;
            }
        } else {
            // No user monitoring requested - set phases and amplitudes to defaults
            for (int ch = 0; ch < num_channels; ch++) {
                current_phases[ch] = 0.0f;
                current_amplitudes[ch] = 1.0f;
            }
        }
    } else {
        // During calibration: Check if all channels have converged lag compensation
        bool all_lag_converged = true;
        for (int ch = 0; ch < num_channels; ch++) {
            if (ch == REF_CHANNEL) continue;
            
            if (devices[ch]) {
                std::lock_guard<std::mutex> comp_lock(devices[ch]->compensation_mutex);
                if (devices[ch]->compensation.state != LagCompensatorState::CONVERGED) {
                    all_lag_converged = false;
                    break;
                }
            }
        }
        
        // Only calculate phases using eigenvalue decomposition when lag compensation is done
        if (all_lag_converged && phase_compensation) {
            PhaseCompensatorState phase_state;
            {
                std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
                phase_state = phase_compensation->state;
            }
            
            // Calculate phases and amplitudes using eigenvalue decomposition when in appropriate states
            if (phase_state == PhaseCompensatorState::MEASURING_INITIAL_PHASE ||
                phase_state == PhaseCompensatorState::VERIFYING_CONVERGENCE ||
                phase_state == PhaseCompensatorState::APPLYING_COMPENSATION) {

                calculate_phase_amplitude_calibration_eigen(complex_set, current_phases, current_amplitudes);

                // Debug output when we calculate new phases during calibration
                static int phase_calc_count = 0;
                if (++phase_calc_count % 10 == 1) {
                    std::cout << "Calibration phase+amp calculation #" << phase_calc_count << " - ";
                    for (const auto& [ch, phase] : current_phases) {
                        if (ch != REF_CHANNEL) {
                            float amp_db = 20*std::log10(current_amplitudes[ch]);
                            std::cout << "Ch" << ch << ":" << phase << "°";
                            if (std::abs(amp_db) > 0.1f) {
                                std::cout << "(" << amp_db << "dB)";
                            }
                            std::cout << " ";
                        }
                    }
                    std::cout << std::endl;
                }
            } else {
                // Use previous phases/amplitudes or defaults
                for (int ch = 0; ch < num_channels; ch++) {
                    current_phases[ch] = correlation_result.phases.count(ch) ? correlation_result.phases[ch] : 0.0f;
                    current_amplitudes[ch] = correlation_result.amplitudes.count(ch) ? correlation_result.amplitudes[ch] : 1.0f;
                }
            }
        } else {
            // Lag compensation not complete - set all phases and amplitudes to defaults
            for (int ch = 0; ch < num_channels; ch++) {
                current_phases[ch] = 0.0f;
                current_amplitudes[ch] = 1.0f;
            }
        }
    }
    
    // Release FFT plans back to pool
    fft_pool.release(ref_plan);
    for (auto* plan : channel_plans) {
        fft_pool.release(plan);
    }
    
    // Update correlation results
    {
        std::lock_guard<std::mutex> lock(correlation_result.data_mutex);
        correlation_result.lags = current_lags;
        correlation_result.phases = current_phases;
        correlation_result.amplitudes = current_amplitudes;
        // Swap (O(1)) the staged traces in - unless this set's channel count no
        // longer matches the result arrays: a set dequeued just before an
        // element-count reconfiguration resized them is stale, so its traces
        // are dropped rather than written out of bounds.
        if (staged_corr.size() == correlation_result.correlation_data.size()) {
            correlation_result.scale_factors.swap(staged_scale);
            correlation_result.correlation_data.swap(staged_corr);
        }
        correlation_result.data_ready = true;
        correlation_result.data_sequence++;
        
        // Update channel states
        bool any_lag_active = false;
        for (int ch = 0; ch < num_channels; ch++) {
            if (ch == REF_CHANNEL) continue;
            
            if (devices[ch]) {
                std::lock_guard<std::mutex> comp_lock(devices[ch]->compensation_mutex);
                auto state = devices[ch]->compensation.state;
                correlation_result.channel_states[ch] = state;
                correlation_result.channel_zero_counts[ch] = devices[ch]->compensation.zero_lag_count;
                if (state != LagCompensatorState::CONVERGED) {
                    any_lag_active = true;
                }
            }
        }
        
        correlation_result.compensation_active = any_lag_active;
        
        if (phase_compensation) {
            std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
            auto phase_state = phase_compensation->state;
            correlation_result.phase_state = phase_state;
            correlation_result.phase_compensation_active = (phase_state != PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION && 
                                                           phase_state != PhaseCompensatorState::CONVERGED);
            correlation_result.phase_compensation_complete = (phase_state == PhaseCompensatorState::CONVERGED);
        }
    }

#if ENABLE_COHERENCE_MONITOR
    // Backstop for the FFT-ON post-calibration monitoring path (the FFT-OFF
    // streaming path calls this from correlation_processor). complex_set is still
    // valid here - the BufferGuard releases it only at function exit. Internally
    // rate-limited and gated to CONVERGED, so this is a no-op during calibration.
    maybe_run_coherence_monitor(complex_set);
#endif
}

std::string build_correlation_message(const CorrelationResult& correlation_result,
                                    const FFTProcessingControl& fft_control) {
    std::lock_guard<std::mutex> lock(correlation_result.data_mutex);

    // Channel count for THIS message: the live element count, capped at what
    // the per-pair arrays hold. A reconfiguration stores the new count before
    // it resizes them, and this timer can fire in between - reading past the
    // arrays (growing) or emitting a header that disagrees with them.
    const int num_elements = std::min(active_num_elements.load(),
                                      static_cast<int>(correlation_result.correlation_data.size()) + 1);

    std::vector<uint8_t> message;
    message.reserve(16384);

    // Header (7th field = per-bin phase calibration enabled; parsed by index.html)
    bool overall_compensation_active = correlation_result.compensation_active || correlation_result.phase_compensation_active;
    const std::array<uint32_t, 7> header_data = {
        static_cast<uint32_t>(num_elements - 1), CORRELATION_SIZE, REF_CHANNEL, static_cast<uint32_t>(num_elements),
        overall_compensation_active ? 1u : 0u, bias_tee_enabled ? 1u : 0u,
        per_bin_cal.enabled.load(std::memory_order_relaxed) ? 1u : 0u
    };
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(header_data.data()), 
                   reinterpret_cast<const uint8_t*>(header_data.data() + header_data.size()));
    
    // Frequency, gain, and RTL-TCP channel
    float freq_mhz = current_frequency.load() / 1e6f;
    float gain_db = (current_gain.load() == -1) ? -1.0f : (current_gain.load() / 10.0f);
    uint32_t rtl_channel = static_cast<uint32_t>(rtl_tcp_channel.load());
    
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(&freq_mhz), 
                   reinterpret_cast<const uint8_t*>(&freq_mhz) + sizeof(float));
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(&gain_db), 
                   reinterpret_cast<const uint8_t*>(&gain_db) + sizeof(float));
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(&rtl_channel), 
                   reinterpret_cast<const uint8_t*>(&rtl_channel) + sizeof(uint32_t));
    
    // Scale factors (exactly num_elements - 1 entries, whatever the vector holds)
    std::vector<float> sf(num_elements - 1, 0.0f);
    if (fft_control.fft_enabled && correlation_result.data_ready) {
        std::copy_n(correlation_result.scale_factors.begin(),
                    std::min(sf.size(), correlation_result.scale_factors.size()), sf.begin());
    }
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(sf.data()), 
                  reinterpret_cast<const uint8_t*>(sf.data() + sf.size()));
    
    // Lags and phases. Indexed by channel (exactly num_elements entries each) -
    // the client parses nDev values, so map-order iteration would misalign the
    // packet whenever the maps hold more or fewer channels than num_elements
    // (e.g. stale entries right after an element-count reconfiguration).
    std::vector<float> lags_array(num_elements, 0.0f), phases_array(num_elements, 0.0f);
    for (int ch = 0; ch < num_elements; ch++) {
        if (correlation_result.lags.count(ch))   lags_array[ch]   = correlation_result.lags.at(ch);
        if (correlation_result.phases.count(ch)) phases_array[ch] = correlation_result.phases.at(ch);
    }

    message.insert(message.end(), reinterpret_cast<const uint8_t*>(lags_array.data()),
                   reinterpret_cast<const uint8_t*>(lags_array.data() + lags_array.size()));
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(phases_array.data()),
                   reinterpret_cast<const uint8_t*>(phases_array.data() + phases_array.size()));

    // Live measured amplitude (eigen correction factor |e_ref|/|e_ch|, 1.0 =
    // matched; the Amp column shows the channel's gain vs the reference, the
    // amplitude counterpart of the Phase column) and the APPLIED compensation
    // vector as (gain dB, phase deg) pairs - the same values as the control
    // port's channel_comp. Parsed by index.html.
    std::vector<float> amps_array(num_elements, 1.0f), comp_array(num_elements * 2, 0.0f);
    for (int ch = 0; ch < num_elements; ch++) {
        if (correlation_result.amplitudes.count(ch)) amps_array[ch] = correlation_result.amplitudes.at(ch);
        if (phase_compensation) {
            const Complex c = phase_compensation->compensation_vector.load(ch);
            comp_array[ch * 2]     = 20.0f * std::log10(std::max(std::abs(c), 1e-6f));
            comp_array[ch * 2 + 1] = std::arg(c) * 180.0f / static_cast<float>(M_PI);
        }
    }
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(amps_array.data()),
                   reinterpret_cast<const uint8_t*>(amps_array.data() + amps_array.size()));
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(comp_array.data()),
                   reinterpret_cast<const uint8_t*>(comp_array.data() + comp_array.size()));

    // Compensation status
    std::vector<uint32_t> comp_status(num_elements * 4, 0);
    for (const auto& [channel, state] : correlation_result.channel_states) {
        if (channel != REF_CHANNEL && channel >= 0 && channel < num_elements) {
            comp_status[channel * 4] = static_cast<uint32_t>(state);
            comp_status[channel * 4 + 1] = correlation_result.channel_zero_counts.count(channel) ? 
                                          correlation_result.channel_zero_counts.at(channel) : 0;
            comp_status[channel * 4 + 2] = (state == LagCompensatorState::CONVERGED) ? 1 : 0;
            
            // Send actual phase_state enum value (0-5) for proper cooldown detection
            // 0=WAITING_FOR_LAG, 1=MEASURING, 2=APPLYING, 3=VERIFYING, 4=CONVERGED, 5=WAITING_FOR_STABILITY
            uint32_t status_info = static_cast<uint32_t>(correlation_result.phase_state);
            if (correlation_result.compensation_active && status_info == 0) {
                status_info = 1;  // Legacy: indicate lag compensation active
            }
            
            if (fft_control.fft_enabled) status_info |= 0x100;
            if (fft_control.auto_disabled) status_info |= 0x200;
            if (fft_control.user_override) status_info |= 0x400;
            
            comp_status[channel * 4 + 3] = status_info;
        }
    }
    message.insert(message.end(), reinterpret_cast<const uint8_t*>(comp_status.data()), 
                   reinterpret_cast<const uint8_t*>(comp_status.data() + comp_status.size()));
    
    // Add correlation data
    if (correlation_result.data_ready && fft_control.fft_enabled) {
        for (int corr_idx = 0; corr_idx < num_elements - 1; corr_idx++) {
            const auto& corr = correlation_result.correlation_data[corr_idx];
            int channel = (corr_idx >= REF_CHANNEL) ? corr_idx + 1 : corr_idx;
            float lag = correlation_result.lags.count(channel) ? correlation_result.lags.at(channel) : 0.0f;
            int peak_idx = clamp(static_cast<int>(lag + CORRELATION_SIZE / 2), 0, static_cast<int>(CORRELATION_SIZE) - 1);
            
            std::vector<uint8_t> decimated_data;
            std::vector<int16_t> decimated_indices;
            
            for (int i = 0; i < CORRELATION_SIZE; ) {
                int distance_from_peak = std::abs(i - peak_idx);
                int decimation_factor = (distance_from_peak <= 25) ? 1 : (distance_from_peak <= 200) ? 4 : 32;
                
                float max_val = *std::max_element(corr.begin() + i, 
                                           corr.begin() + std::min(i + decimation_factor, static_cast<int>(CORRELATION_SIZE)));
                
                decimated_data.push_back(static_cast<uint8_t>(max_val * 255.0f));
                decimated_indices.push_back(static_cast<int16_t>(i - CORRELATION_SIZE / 2));
                i += decimation_factor;
            }
            
            uint32_t decimated_size = decimated_data.size();
            message.insert(message.end(), reinterpret_cast<const uint8_t*>(&decimated_size), 
                          reinterpret_cast<const uint8_t*>(&decimated_size) + sizeof(uint32_t));
            message.insert(message.end(), reinterpret_cast<const uint8_t*>(decimated_indices.data()), 
                          reinterpret_cast<const uint8_t*>(decimated_indices.data()) + decimated_indices.size() * sizeof(int16_t));
            message.insert(message.end(), decimated_data.begin(), decimated_data.end());
        }
    } else {
        // Dummy data when FFT disabled
        for (int corr_idx = 0; corr_idx < num_elements - 1; corr_idx++) {
            uint32_t decimated_size = 100;
            message.insert(message.end(), reinterpret_cast<const uint8_t*>(&decimated_size), 
                          reinterpret_cast<const uint8_t*>(&decimated_size) + sizeof(uint32_t));
            
            std::vector<int16_t> dummy_indices(decimated_size);
            std::iota(dummy_indices.begin(), dummy_indices.end(), -CORRELATION_SIZE/2);
            for (auto& idx : dummy_indices) idx *= (CORRELATION_SIZE/decimated_size);
            
            message.insert(message.end(), reinterpret_cast<const uint8_t*>(dummy_indices.data()), 
                          reinterpret_cast<const uint8_t*>(dummy_indices.data()) + dummy_indices.size() * sizeof(int16_t));
            
            std::vector<uint8_t> zero_corr(decimated_size, 0);
            message.insert(message.end(), zero_corr.begin(), zero_corr.end());
        }
    }
    
    return std::string(reinterpret_cast<char*>(message.data()), message.size());
}

void correlation_processor(CorrelationResult& correlation_result, FFTProcessingControl& fft_control) {
    std::cout << "Starting correlation processor with eigenvalue decomposition" << std::endl;

    while (global_running) {
        // Check FFT enabled state
        bool fft_enabled = false;
        {
            std::lock_guard<std::mutex> fft_lock(fft_control.control_mutex);
            fft_enabled = fft_control.fft_enabled;
        }
        
        if (fft_enabled) {
            // FFT enabled - process correlations normally
            process_correlations(correlation_result, fft_control);
        } else {
            // FFT disabled - drain buffer but don't process.
            std::vector<ComplexBuffer> set;
            if (l2_buffer.try_dequeue(set)) {
                l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
#if ENABLE_COHERENCE_MONITOR
                // This is the normal-streaming path: post-calibration FFT is
                // auto-disabled, so the low-rate monitor is the only thing
                // measuring coherence. It self-rate-limits and gates internally.
                maybe_run_coherence_monitor(set);
#endif
                l2_buffer_pool.release(std::move(set));  // recycle (was previously dropped)
            }

            // Update correlation result to indicate no data ready
            {
                std::lock_guard<std::mutex> lock(correlation_result.data_mutex);
                correlation_result.data_ready = false;
                correlation_result.data_sequence++;
                
                // Clear phase data when FFT is disabled
                for (auto& [channel, phase] : correlation_result.phases) {
                    phase = 0.0f;
                }
            }
        }
        
        if (PROCESS_LOOP_DELAY > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(PROCESS_LOOP_DELAY));
        }
    }
}
