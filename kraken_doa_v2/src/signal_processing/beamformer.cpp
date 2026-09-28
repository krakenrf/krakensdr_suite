// Beamformer implementation - coherent multi-channel combining
//
// Reconstructed from the shipped fft_viewer binary after the original
// source was lost. Constants, defaults, thresholds and algorithm structure
// were recovered from the disassembly:
//   - Speed of light 299792458 m/s, geometry in mm (x0.001 to meters)
//   - UCA elements at 0/72/144/216/288 deg, ULA centered [-2..2]*spacing
//     (the UCA angles are now mirrored clockwise - see uca_angle_sign())
//   - Steering phase: 2*pi*(x*cos(theta) + y*sin(theta)) with positions
//     in wavelengths; weights applied conjugated (phase alignment)
//   - Setter change thresholds: angle 0.1 deg, radius/spacing 0.1 mm,
//     frequency 1000 Hz, sample rate 100 Hz
//   - MVDR alpha clamped [0.01, 1.0], snapshots clamped [64, 4096]
//   - SNR estimate: 256-pt FFT, peak minus 10th percentile (sorted[25])
//   - SNR improvement refreshed every 10th process() call
//   - Selection diversity: 2 dB switching hysteresis
//   - FD-DAS: 256-pt FFT, Hann window, 50% overlap-add, phase table
//     refreshed when angle changes >0.1 deg or frequency >1000 Hz

#include "signal_processing/beamformer.hpp"
#include "utils/parse_num.hpp"
#include "signal_processing/fft_processor.hpp"   // fftw_planner_mutex
#include "globals.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
constexpr double SPEED_OF_LIGHT = 299792458.0;
constexpr double TWO_PI = 6.283185307179586;
constexpr double DEG_TO_RAD = 0.017453292519943295;
}

// ============================================================================
// Construction / destruction
// ============================================================================

Beamformer::Beamformer() {
    // Runtime element count: follow whatever the server is streaming right now
    // (beamformers are created at runtime; syncElementCount() tracks later
    // changes block by block).
    num_elements_ = std::clamp(active_num_elements.load(std::memory_order_relaxed),
                               2, NUM_ELEMENTS);

    steering_vector_ = Eigen::VectorXcd::Zero(num_elements_);
    element_x_.assign(num_elements_, 0.0);
    element_y_.assign(num_elements_, 0.0);

    covariance_ = Eigen::MatrixXcd::Identity(num_elements_, num_elements_);
    covariance_accum_ = Eigen::MatrixXcd::Zero(num_elements_, num_elements_);
    mvdr_weights_ = Eigen::VectorXcd::Zero(num_elements_);

    snr_db_buffer_.resize(SNR_FFT_SIZE);
    snr_sorted_buffer_.resize(SNR_FFT_SIZE);

    // SNR estimation FFT - uses FFTW_MEASURE, so this constructor must run
    // after FFTW wisdom is loaded (see main.cpp). The planner is not
    // thread-safe; beamformers are now created at runtime (per decimator),
    // so serialize plan creation against all other FFTW planning.
    snr_in_ = fftwf_alloc_complex(SNR_FFT_SIZE);
    snr_out_ = fftwf_alloc_complex(SNR_FFT_SIZE);
    if (snr_in_ && snr_out_) {
        std::lock_guard<std::mutex> planner_lock(fftw_planner_mutex);
        snr_plan_ = fftwf_plan_dft_1d(SNR_FFT_SIZE, snr_in_, snr_out_,
                                      FFTW_FORWARD, FFTW_MEASURE);
    }

    computeElementPositions();
    initFreqDomainDAS();
}

Beamformer::~Beamformer() {
    cleanupFreqDomainDAS();
    {
        // Plan destruction is a planner operation - serialize it too.
        std::lock_guard<std::mutex> planner_lock(fftw_planner_mutex);
        if (snr_plan_) fftwf_destroy_plan(snr_plan_);
    }
    if (snr_in_) fftwf_free(snr_in_);
    if (snr_out_) fftwf_free(snr_out_);
}

// ============================================================================
// Enable / disable
// ============================================================================

void Beamformer::enable() { enabled_.store(true, std::memory_order_release); }
void Beamformer::disable() { enabled_.store(false, std::memory_order_release); }
bool Beamformer::isEnabled() const { return enabled_.load(std::memory_order_acquire); }

// ============================================================================
// Configuration setters/getters
// ============================================================================

void Beamformer::setSteeringAngle(float angle_deg) {
    // Normalize to [0, 360)
    angle_deg = wrap_degrees(angle_deg);

    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(steering_angle_deg_ - angle_deg) > 0.1f) {
        steering_angle_deg_ = angle_deg;
        steering_vector_valid_ = false;
        // MVDR weights point at the old angle until recomputed
        mvdr_weights_valid_ = false;
    }
}

float Beamformer::getSteeringAngle() const { return steering_angle_deg_; }

void Beamformer::setArrayTopology(ArrayTopology topology) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (topology_ != topology) {
        topology_ = topology;
        steering_vector_valid_ = false;
        computeElementPositions();
    }
}

ArrayTopology Beamformer::getArrayTopology() const { return topology_; }

void Beamformer::setArrayRadius(float radius_mm) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(array_radius_mm_ - radius_mm) > 0.1f) {
        array_radius_mm_ = radius_mm;
        steering_vector_valid_ = false;
        computeElementPositions();
    }
}

float Beamformer::getArrayRadius() const { return array_radius_mm_; }

void Beamformer::setElementSpacing(float spacing_mm) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(element_spacing_mm_ - spacing_mm) > 0.1f) {
        element_spacing_mm_ = spacing_mm;
        steering_vector_valid_ = false;
        computeElementPositions();
    }
}

float Beamformer::getElementSpacing() const { return element_spacing_mm_; }

void Beamformer::setFrequency(float freq_hz) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(frequency_hz_ - freq_hz) > 1000.0f) {
        frequency_hz_ = freq_hz;
        steering_vector_valid_ = false;
        computeElementPositions();
    }
}

float Beamformer::getFrequency() const { return frequency_hz_; }

void Beamformer::setSampleRate(float rate_hz) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(sample_rate_hz_ - rate_hz) > 100.0f) {
        sample_rate_hz_ = rate_hz;
    }
}

float Beamformer::getSampleRate() const { return sample_rate_hz_; }

void Beamformer::setBeamformingMode(BeamformingMode mode) {
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (mode_ != mode) {
        mode_ = mode;
        mvdr_weights_valid_ = false;
    }
}

BeamformingMode Beamformer::getBeamformingMode() const { return mode_; }

void Beamformer::setMVDRDiagonalLoading(float alpha) {
    alpha = std::min(alpha, 1.0f);
    alpha = std::max(alpha, 0.01f);
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (std::fabs(mvdr_diagonal_loading_ - alpha) > 0.001f) {
        mvdr_diagonal_loading_ = alpha;
        mvdr_weights_valid_ = false;
    }
}

float Beamformer::getMVDRDiagonalLoading() const { return mvdr_diagonal_loading_; }

void Beamformer::setMVDRSnapshotLength(size_t snapshots) {
    snapshots = std::max<size_t>(snapshots, 64);
    snapshots = std::min<size_t>(snapshots, 4096);
    std::lock_guard<std::mutex> config_lock(config_mutex_);
    if (mvdr_snapshot_length_ != snapshots) {
        mvdr_snapshot_length_ = snapshots;
        snapshot_count_ = 0;
        covariance_accum_.setZero();
        mvdr_weights_valid_ = false;
    }
}

size_t Beamformer::getMVDRSnapshotLength() const { return mvdr_snapshot_length_; }

// ============================================================================
// Runtime element count
// ============================================================================

void Beamformer::syncElementCount() {
    // Caller holds config_mutex_. Cheap no-op on the hot path until the
    // global element count (synced from the server's packet header) changes.
    const int target = std::clamp(active_num_elements.load(std::memory_order_relaxed),
                                  2, NUM_ELEMENTS);
    if (target == num_elements_) return;

    std::cout << "Beamformer: element count " << num_elements_ << " -> "
              << target << ", reinitializing" << std::endl;
    num_elements_ = target;

    // Per-element geometry and weights
    steering_vector_ = Eigen::VectorXcd::Zero(num_elements_);
    element_x_.assign(num_elements_, 0.0);
    element_y_.assign(num_elements_, 0.0);
    steering_vector_valid_ = false;

    // MVDR state accumulated with the old count is meaningless now
    covariance_ = Eigen::MatrixXcd::Identity(num_elements_, num_elements_);
    covariance_accum_ = Eigen::MatrixXcd::Zero(num_elements_, num_elements_);
    mvdr_weights_ = Eigen::VectorXcd::Zero(num_elements_);
    mvdr_weights_valid_ = false;
    snapshot_count_ = 0;

    // Frequency-domain DAS per-channel tables and history (same lock order as
    // freqDomainDAS: config_mutex_ then fdds_mutex_)
    {
        std::lock_guard<std::mutex> lock(fdds_mutex_);
        fdds_steering_.assign(num_elements_,
                              std::vector<std::complex<float>>(FDDAS_FFT_SIZE,
                                                               std::complex<float>(1.0f, 0.0f)));
        fdds_history_.assign(num_elements_, {});
        fdds_overlap_.assign(FDDAS_HOP, std::complex<float>(0.0f, 0.0f));
        fdds_last_angle_ = -1000.0f; // Force phase table rebuild
        fdds_last_freq_ = 0.0f;
    }

    computeElementPositions();
}

// ============================================================================
// Array geometry
// ============================================================================

void Beamformer::computeElementPositions() {
    // Positions are stored in wavelengths so steering phases are simply
    // 2*pi*(x*cos + y*sin)
    const double wavelength_m = SPEED_OF_LIGHT / static_cast<double>(frequency_hz_);

    if (topology_ == ArrayTopology::UCA) {
        const double radius_wl =
            (static_cast<double>(array_radius_mm_) * 0.001) / wavelength_m;
        // Array elements are wired clockwise (ANT0 on +x) - mirror the angle
        const double dir = uca_angle_sign();
        for (int k = 0; k < num_elements_; k++) {
            const double angle = dir * TWO_PI * k / num_elements_; // 0, -72, -144, ...
            element_x_[k] = radius_wl * std::cos(angle);
            element_y_[k] = radius_wl * std::sin(angle);
        }
    } else {
        // ULA (and CUSTOM fallback): elements centered on the origin along x
        const double spacing_wl =
            (static_cast<double>(element_spacing_mm_) * 0.001) / wavelength_m;
        const double center = (num_elements_ - 1) / 2.0;
        for (int k = 0; k < num_elements_; k++) {
            element_x_[k] = (k - center) * spacing_wl;
            element_y_[k] = 0.0;
        }
    }
}

void Beamformer::updateSteeringVector() {
    if (steering_vector_valid_) return;

    computeElementPositions();

    const double theta = static_cast<double>(steering_angle_deg_) * DEG_TO_RAD;

    if (topology_ == ArrayTopology::UCA) {
        const double cos_t = std::cos(theta);
        const double sin_t = std::sin(theta);
        for (int k = 0; k < num_elements_; k++) {
            const double phase = TWO_PI * (element_x_[k] * cos_t + element_y_[k] * sin_t);
            steering_vector_(k) = std::complex<double>(std::cos(phase), std::sin(phase));
        }
    } else {
        const double sin_t = std::sin(theta);
        for (int k = 0; k < num_elements_; k++) {
            const double phase = TWO_PI * element_x_[k] * sin_t;
            steering_vector_(k) = std::complex<double>(std::cos(phase), std::sin(phase));
        }
    }

    steering_vector_valid_ = true;
}

// ============================================================================
// Channel gathering helper
// ============================================================================

size_t Beamformer::gatherChannels(const SharedDecimator::MultiChannelDecimated& input,
                                  const std::complex<float>* (&ptrs)[NUM_ELEMENTS]) const {
    if (input.channels.size() < static_cast<size_t>(num_elements_)) return 0;

    size_t min_samples = SIZE_MAX;
    for (int ch = 0; ch < num_elements_; ch++) {
        const auto& samples = input.channels[ch].samples;
        if (samples.empty()) return 0;
        ptrs[ch] = samples.data();
        min_samples = std::min(min_samples, samples.size());
    }
    return min_samples;
}

// ============================================================================
// Delay-and-sum (time domain, narrowband)
// ============================================================================

void Beamformer::delayAndSum(const SharedDecimator::MultiChannelDecimated& input,
                             std::vector<std::complex<float>>& output) {
    updateSteeringVector();

    const std::complex<float>* ch[NUM_ELEMENTS];
    const size_t n = gatherChannels(input, ch);
    if (n == 0) {
        output.clear();
        return;
    }

    // Diagnostic mode: manual steering at ~0 deg performs a plain
    // unweighted coherent sum (no phase alignment)
    const bool plain_sum =
        manual_steering_enabled.load(std::memory_order_relaxed) &&
        std::fabs(manual_steering_angle.load(std::memory_order_relaxed)) < 0.1f;

    output.resize(n);

    if (plain_sum) {
        for (size_t i = 0; i < n; i++) {
            double acc_re = 0.0, acc_im = 0.0;
            for (int k = 0; k < num_elements_; k++) {
                acc_re += static_cast<double>(ch[k][i].real());
                acc_im += static_cast<double>(ch[k][i].imag());
            }
            output[i] = std::complex<float>(static_cast<float>(acc_re),
                                            static_cast<float>(acc_im));
        }
        return;
    }

    // out[i] = sum_k conj(w_k) * s_k[i]  - aligns each channel's phase to
    // the steering direction. Accumulate in double for precision.
    for (size_t i = 0; i < n; i++) {
        double acc_re = 0.0, acc_im = 0.0;
        for (int k = 0; k < num_elements_; k++) {
            const double w_re = steering_vector_(k).real();
            const double w_im = steering_vector_(k).imag();
            const double s_re = static_cast<double>(ch[k][i].real());
            const double s_im = static_cast<double>(ch[k][i].imag());
            // conj(w) * s
            acc_re += w_re * s_re + w_im * s_im;
            acc_im += w_re * s_im - w_im * s_re;
        }
        output[i] = std::complex<float>(static_cast<float>(acc_re),
                                        static_cast<float>(acc_im));
    }
}

// ============================================================================
// SNR estimation (256-point FFT, peak minus 10th-percentile noise floor)
// ============================================================================

float Beamformer::computeChannelSNR_FFT(const std::vector<std::complex<float>>& samples) {
    if (!snr_plan_ || samples.empty()) return 0.0f;

    std::lock_guard<std::mutex> lock(snr_mutex_);

    const size_t n = std::min(samples.size(), static_cast<size_t>(SNR_FFT_SIZE));
    for (size_t i = 0; i < n; i++) {
        snr_in_[i][0] = samples[i].real();
        snr_in_[i][1] = samples[i].imag();
    }
    if (n < SNR_FFT_SIZE) {
        std::memset(&snr_in_[n], 0, (SNR_FFT_SIZE - n) * sizeof(fftwf_complex));
    }

    fftwf_execute(snr_plan_);

    for (int i = 0; i < SNR_FFT_SIZE; i++) {
        const float re = snr_out_[i][0];
        const float im = snr_out_[i][1];
        snr_db_buffer_[i] = 10.0f * std::log10f(re * re + im * im + 1e-15f);
    }

    snr_sorted_buffer_ = snr_db_buffer_;
    std::sort(snr_sorted_buffer_.begin(), snr_sorted_buffer_.end());

    // Noise floor at the 10th percentile (index 25 of 256), peak at the top
    const float noise_floor = snr_sorted_buffer_[SNR_FFT_SIZE / 10];
    const float peak = snr_sorted_buffer_[SNR_FFT_SIZE - 1];
    return peak - noise_floor;
}

// ============================================================================
// Selection diversity
// ============================================================================

void Beamformer::selectionDiversity(const SharedDecimator::MultiChannelDecimated& input,
                                    std::vector<std::complex<float>>& output) {
    const std::complex<float>* ch[NUM_ELEMENTS];
    const size_t n = gatherChannels(input, ch);
    if (n == 0) {
        output.clear();
        return;
    }

    // Estimate SNR on every channel
    float best_snr = -100.0f;
    int best_channel = 0;
    for (int k = 0; k < num_elements_; k++) {
        const float snr = computeChannelSNR_FFT(input.channels[k].samples);
        channel_snrs_[k] = snr;
        if (snr > best_snr) {
            best_snr = snr;
            best_channel = k;
        }
    }

    // Hysteresis: keep the current channel unless the best candidate is at
    // least 2 dB better (or the current channel's estimate is unusable)
    const int current = selected_channel_.load(std::memory_order_relaxed);
    int selected = best_channel;
    if (current >= 0 && current < num_elements_ && current != best_channel) {
        const float current_snr = channel_snrs_[current];
        if (current_snr > -50.0f && (best_snr - current_snr) < 2.0f) {
            selected = current;
        }
    }
    selected_channel_.store(selected, std::memory_order_relaxed);

    // Output is a straight copy of the selected channel
    output.assign(ch[selected], ch[selected] + n);
}

// ============================================================================
// MVDR
// ============================================================================

void Beamformer::updateCovarianceMatrix(const SharedDecimator::MultiChannelDecimated& input) {
    const std::complex<float>* ch[NUM_ELEMENTS];
    const size_t n = gatherChannels(input, ch);
    if (n == 0) return;

    // Accumulate spatial covariance snapshots: accum += x * x^H
    Eigen::VectorXcd x(num_elements_);
    const size_t remaining = (mvdr_snapshot_length_ > snapshot_count_)
                                 ? (mvdr_snapshot_length_ - snapshot_count_)
                                 : 0;
    const size_t take = std::min(n, remaining);

    for (size_t i = 0; i < take; i++) {
        for (int k = 0; k < num_elements_; k++) {
            x(k) = std::complex<double>(ch[k][i].real(), ch[k][i].imag());
        }
        covariance_accum_ += x * x.adjoint();
    }
    snapshot_count_ += take;

    if (snapshot_count_ >= mvdr_snapshot_length_) {
        // Average the snapshots, then apply diagonal loading proportional
        // to the average channel power: R += alpha * (trace(R)/N) * I
        Eigen::MatrixXcd r = covariance_accum_ / static_cast<double>(snapshot_count_);

        const double loading =
            static_cast<double>(mvdr_diagonal_loading_) *
            (r.trace().real() / num_elements_);
        for (int k = 0; k < num_elements_; k++) {
            r(k, k) += std::complex<double>(loading, 0.0);
        }

        covariance_ = r;
        covariance_accum_.setZero();
        snapshot_count_ = 0;
        mvdr_weights_valid_ = false;
    }
}

void Beamformer::computeMVDRWeights() {
    updateSteeringVector();

    // w = R^-1 a / (a^H R^-1 a)
    Eigen::LDLT<Eigen::MatrixXcd> ldlt(covariance_);
    if (ldlt.info() != Eigen::Success) {
        // Singular covariance: fall back to conventional weights
        mvdr_weights_ = steering_vector_ / static_cast<double>(num_elements_);
        mvdr_condition_number_ = 1e6f;
        mvdr_weights_valid_ = true;
        return;
    }

    const Eigen::VectorXcd r_inv_a = ldlt.solve(steering_vector_);
    const std::complex<double> denom = steering_vector_.adjoint() * r_inv_a;

    if (std::abs(denom) < 1e-12) {
        mvdr_weights_ = steering_vector_ / static_cast<double>(num_elements_);
        mvdr_condition_number_ = 1e6f;
    } else {
        mvdr_weights_ = r_inv_a / denom;

        // Condition estimate from the LDLT diagonal (capped at 1e6)
        const Eigen::VectorXd d = ldlt.vectorD().real().cwiseAbs();
        const double d_max = d.maxCoeff();
        const double d_min = d.minCoeff();
        const double cond = (d_min > 0.0) ? (d_max / d_min) : 1e6;
        mvdr_condition_number_ = static_cast<float>(std::min(cond, 1e6));
    }

    mvdr_weights_valid_ = true;
}

void Beamformer::mvdrBeamform(const SharedDecimator::MultiChannelDecimated& input,
                              std::vector<std::complex<float>>& output) {
    updateCovarianceMatrix(input);

    if (!mvdr_weights_valid_) {
        computeMVDRWeights();
    }

    const std::complex<float>* ch[NUM_ELEMENTS];
    const size_t n = gatherChannels(input, ch);
    if (n == 0) {
        output.clear();
        return;
    }

    output.resize(n);
    for (size_t i = 0; i < n; i++) {
        double acc_re = 0.0, acc_im = 0.0;
        for (int k = 0; k < num_elements_; k++) {
            const double w_re = mvdr_weights_(k).real();
            const double w_im = mvdr_weights_(k).imag();
            const double s_re = static_cast<double>(ch[k][i].real());
            const double s_im = static_cast<double>(ch[k][i].imag());
            // conj(w) * s
            acc_re += w_re * s_re + w_im * s_im;
            acc_im += w_re * s_im - w_im * s_re;
        }
        output[i] = std::complex<float>(static_cast<float>(acc_re),
                                        static_cast<float>(acc_im));
    }
}

// ============================================================================
// Frequency-domain delay-and-sum (wideband, 50% overlap-add)
// ============================================================================

void Beamformer::initFreqDomainDAS() {
    std::lock_guard<std::mutex> lock(fdds_mutex_);

    fdds_fft_in_ = fftwf_alloc_complex(FDDAS_FFT_SIZE);
    fdds_fft_out_ = fftwf_alloc_complex(FDDAS_FFT_SIZE);
    fdds_ifft_in_ = fftwf_alloc_complex(FDDAS_FFT_SIZE);
    fdds_ifft_out_ = fftwf_alloc_complex(FDDAS_FFT_SIZE);

    {
        // Serialize plan creation against all other FFTW planning (the planner
        // is not thread-safe and beamformers are created at runtime).
        std::lock_guard<std::mutex> planner_lock(fftw_planner_mutex);
        if (fdds_fft_in_ && fdds_fft_out_) {
            fdds_fwd_plan_ = fftwf_plan_dft_1d(FDDAS_FFT_SIZE, fdds_fft_in_, fdds_fft_out_,
                                               FFTW_FORWARD, FFTW_MEASURE);
        }
        if (fdds_ifft_in_ && fdds_ifft_out_) {
            fdds_inv_plan_ = fftwf_plan_dft_1d(FDDAS_FFT_SIZE, fdds_ifft_in_, fdds_ifft_out_,
                                               FFTW_BACKWARD, FFTW_MEASURE);
        }
    }

    // Hann window (sums to unity at 50% overlap)
    fdds_window_.resize(FDDAS_FFT_SIZE);
    for (int i = 0; i < FDDAS_FFT_SIZE; i++) {
        fdds_window_[i] = 0.5f * (1.0f - std::cos(TWO_PI * i / (FDDAS_FFT_SIZE - 1)));
    }

    fdds_steering_.assign(num_elements_,
                          std::vector<std::complex<float>>(FDDAS_FFT_SIZE,
                                                           std::complex<float>(1.0f, 0.0f)));
    fdds_history_.assign(num_elements_, {});
    fdds_overlap_.assign(FDDAS_HOP, std::complex<float>(0.0f, 0.0f));
    fdds_last_angle_ = -1000.0f; // Force phase table rebuild on first use
    fdds_last_freq_ = 0.0f;
}

void Beamformer::cleanupFreqDomainDAS() {
    std::lock_guard<std::mutex> lock(fdds_mutex_);

    {
        std::lock_guard<std::mutex> planner_lock(fftw_planner_mutex);
        if (fdds_fwd_plan_) { fftwf_destroy_plan(fdds_fwd_plan_); fdds_fwd_plan_ = nullptr; }
        if (fdds_inv_plan_) { fftwf_destroy_plan(fdds_inv_plan_); fdds_inv_plan_ = nullptr; }
    }
    if (fdds_fft_in_) { fftwf_free(fdds_fft_in_); fdds_fft_in_ = nullptr; }
    if (fdds_fft_out_) { fftwf_free(fdds_fft_out_); fdds_fft_out_ = nullptr; }
    if (fdds_ifft_in_) { fftwf_free(fdds_ifft_in_); fdds_ifft_in_ = nullptr; }
    if (fdds_ifft_out_) { fftwf_free(fdds_ifft_out_); fdds_ifft_out_ = nullptr; }
}

void Beamformer::updateFDDASSteeringPhases() {
    // Only rebuild when steering angle moved >0.1 deg or RF frequency
    // changed >1 kHz
    if (std::fabs(steering_angle_deg_ - fdds_last_angle_) <= 0.1f &&
        std::fabs(frequency_hz_ - fdds_last_freq_) <= 1000.0f) {
        return;
    }
    fdds_last_angle_ = steering_angle_deg_;
    fdds_last_freq_ = frequency_hz_;

    computeElementPositions();

    const double theta = static_cast<double>(steering_angle_deg_) * DEG_TO_RAD;
    const double cos_t = std::cos(theta);
    const double sin_t = std::sin(theta);
    const double fc = static_cast<double>(frequency_hz_);
    const double bin_hz = static_cast<double>(sample_rate_hz_) / FDDAS_FFT_SIZE;

    for (int k = 0; k < num_elements_; k++) {
        // Geometric delay toward the steering direction. Positions are in
        // wavelengths at fc: tau_k = (x*cos + y*sin) / fc seconds.
        const double proj_wl = (topology_ == ArrayTopology::UCA)
                                   ? (element_x_[k] * cos_t + element_y_[k] * sin_t)
                                   : (element_x_[k] * sin_t);
        const double tau = proj_wl / fc;

        for (int b = 0; b < FDDAS_FFT_SIZE; b++) {
            // Complex baseband bin frequencies: [0..127] positive,
            // [128..255] negative
            const int bin = (b < FDDAS_FFT_SIZE / 2) ? b : b - FDDAS_FFT_SIZE;
            const double f_abs = fc + bin * bin_hz;
            const double phase = TWO_PI * f_abs * tau;
            // Stored phasor is the per-bin steering weight; applied
            // conjugated during combining (phase alignment)
            fdds_steering_[k][b] = std::complex<float>(
                static_cast<float>(std::cos(phase)),
                static_cast<float>(std::sin(phase)));
        }
    }
}

void Beamformer::freqDomainDAS(const SharedDecimator::MultiChannelDecimated& input,
                               std::vector<std::complex<float>>& output) {
    std::lock_guard<std::mutex> lock(fdds_mutex_);

    output.clear();

    if (!fdds_fwd_plan_ || !fdds_inv_plan_) return;

    const std::complex<float>* ch[NUM_ELEMENTS];
    const size_t n = gatherChannels(input, ch);
    if (n == 0) return;

    updateFDDASSteeringPhases();

    // Append new samples to per-channel history
    for (int k = 0; k < num_elements_; k++) {
        fdds_history_[k].insert(fdds_history_[k].end(), ch[k], ch[k] + n);
    }

    const size_t available = fdds_history_[0].size();
    if (available < FDDAS_FFT_SIZE) return; // Need a full frame

    const float scale = 1.0f / FDDAS_FFT_SIZE; // FFTW round-trip normalization
    static thread_local std::vector<std::complex<float>> frame_sum;
    frame_sum.resize(FDDAS_FFT_SIZE);

    size_t pos = 0;
    while (pos + FDDAS_FFT_SIZE <= available) {
        std::fill(frame_sum.begin(), frame_sum.end(), std::complex<float>(0.0f, 0.0f));

        // Transform each channel, phase-align per bin, accumulate
        for (int k = 0; k < num_elements_; k++) {
            const std::complex<float>* src = fdds_history_[k].data() + pos;
            for (int i = 0; i < FDDAS_FFT_SIZE; i++) {
                fdds_fft_in_[i][0] = src[i].real() * fdds_window_[i];
                fdds_fft_in_[i][1] = src[i].imag() * fdds_window_[i];
            }
            fftwf_execute(fdds_fwd_plan_);

            for (int b = 0; b < FDDAS_FFT_SIZE; b++) {
                const std::complex<float> bin(fdds_fft_out_[b][0], fdds_fft_out_[b][1]);
                frame_sum[b] += bin * std::conj(fdds_steering_[k][b]);
            }
        }

        // Back to time domain
        for (int b = 0; b < FDDAS_FFT_SIZE; b++) {
            fdds_ifft_in_[b][0] = frame_sum[b].real();
            fdds_ifft_in_[b][1] = frame_sum[b].imag();
        }
        fftwf_execute(fdds_inv_plan_);

        // Overlap-add: first half overlaps the previous frame's tail
        for (int i = 0; i < FDDAS_HOP; i++) {
            output.emplace_back(
                fdds_overlap_[i].real() + fdds_ifft_out_[i][0] * scale,
                fdds_overlap_[i].imag() + fdds_ifft_out_[i][1] * scale);
        }
        for (int i = 0; i < FDDAS_HOP; i++) {
            fdds_overlap_[i] = std::complex<float>(
                fdds_ifft_out_[FDDAS_HOP + i][0] * scale,
                fdds_ifft_out_[FDDAS_HOP + i][1] * scale);
        }

        pos += FDDAS_HOP;
    }

    // Keep unprocessed tail for the next call
    for (int k = 0; k < num_elements_; k++) {
        fdds_history_[k].erase(fdds_history_[k].begin(), fdds_history_[k].begin() + pos);
    }
}

// ============================================================================
// Main processing entry point
// ============================================================================

bool Beamformer::process(const SharedDecimator::MultiChannelDecimated& input,
                         std::vector<std::complex<float>>& output) {
    if (!enabled_.load(std::memory_order_acquire)) {
        output.clear();
        return false;
    }

    // Exclude control-thread setters (mode/MVDR/geometry) for the whole block
    std::lock_guard<std::mutex> config_lock(config_mutex_);

    // Follow the server's live element count (heimdall can be reconfigured
    // at runtime); resizes all per-element state when it changed.
    syncElementCount();

    switch (mode_) {
        case BeamformingMode::MVDR:
            mvdrBeamform(input, output);
            break;
        case BeamformingMode::SELECTION_DIVERSITY:
            selectionDiversity(input, output);
            break;
        case BeamformingMode::FREQ_DOMAIN_DAS:
            freqDomainDAS(input, output);
            break;
        case BeamformingMode::DELAY_AND_SUM:
        default:
            delayAndSum(input, output);
            break;
    }

    if (output.empty()) {
        return false;
    }

    // Refresh the SNR improvement estimate every 10th block:
    // SNR(beamformed output) minus SNR(reference channel 0)
    if (++snr_update_counter_ > 9 &&
        !input.channels.empty() && !input.channels[0].samples.empty() &&
        snr_plan_) {
        snr_update_counter_ = 0;
        const float ref_snr = computeChannelSNR_FFT(input.channels[0].samples);
        const float out_snr = computeChannelSNR_FFT(output);
        last_snr_improvement_db_ = out_snr - ref_snr;
    }

    // Update statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.total_blocks_processed++;
        stats_.total_samples_processed += output.size();
        stats_.current_steering_angle = steering_angle_deg_;
        stats_.estimated_snr_improvement_db = last_snr_improvement_db_;
        stats_.current_mode = mode_;
        stats_.mvdr_condition_number = mvdr_condition_number_;
        const int sel = selected_channel_.load(std::memory_order_relaxed);
        stats_.selected_channel = sel;
        stats_.selected_channel_snr =
            (sel >= 0 && sel < 8) ? channel_snrs_[sel] : 0.0f;
        std::memcpy(stats_.channel_snrs, channel_snrs_, sizeof(channel_snrs_));
    }

    return true;
}

// ============================================================================
// Data access
// ============================================================================

int Beamformer::getSelectedChannel() const {
    return selected_channel_.load(std::memory_order_relaxed);
}

// ============================================================================
// Statistics
// ============================================================================

Beamformer::Stats Beamformer::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void Beamformer::resetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = Stats{};
}
