// ContinuousScanner implementation
//
// Reconstructed from the fft_viewer binary committed at 30f205f (the original
// source was never committed to git). Log messages, JSON wire format, constants
// and control flow match the original binary.

#include "continuous_scanner.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

#include "globals.hpp"
#include "channel_manager.hpp"
#include "control_handler.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "signal_processing/fft_processor.hpp"
#include "networking/websocket_server.hpp"

// Defined in main.cpp
extern std::atomic<float> current_edge_clip;

// Global instance (declared extern in globals.hpp)
ContinuousScanner continuous_scanner;

ContinuousScanner::~ContinuousScanner() {
    stop();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool ContinuousScanner::start() {
    // start/stop are serialized: each was a check-then-act on running_, so two
    // concurrent stops (a browser command vs. shutdown) could both pass and
    // both join() the scanner thread (UB), and two starts could both spawn
    // one (std::terminate on the second thread assignment). The scanner
    // thread never calls start/stop, so stop() may join it under the lock.
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (running_.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(signals_mutex_);
        detected_signals_.clear();
        current_signal_index_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& t : tracked_signals_) t = TrackedSignal();
        num_tracked_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        peak_memory_.clear();
    }

    if (wideband_scan_enabled_.load()) {
        // Faster server settling while the scanner drives retunes
        std::string cmd = "{\"set_stability_delay\":{\"delay_ms\":" +
                          std::to_string(500) + "}}";
        ControlHandler::send_control_command(cmd);
        std::cout << "ContinuousScanner: Set server stability delay to "
                  << 500 << "ms" << std::endl;
    }

    bool start_with_retune = false;
    if (wideband_scan_enabled_.load()) {
        buildBandPlan();
        std::lock_guard<std::mutex> lock(band_plan_mutex_);
        if (num_bands_ > 0) {
            current_band_index_ = 0;
            start_with_retune = true;
        }
    }

    running_.store(true);
    if (start_with_retune) {
        initiateRetune(0);
    } else {
        state_.store(SCANNING);
    }

    scanner_thread_ = std::thread(&ContinuousScanner::scannerThread, this);

    std::cout << "ContinuousScanner: Started (dwell=" << dwell_time_ms_.load()
              << "ms, squelch=" << squelch_level_db_.load() << "dB"
              << ", wideband=" << (wideband_scan_enabled_.load() ? "ON" : "OFF")
              << ")" << std::endl;

    std::ostringstream oss;
    oss << "{\"continuous_scanner\":{\"state\":\"started\",";
    oss << "\"squelch_db\":" << squelch_level_db_.load() << ",";
    oss << "\"show_squelch\":" << (show_squelch_.load() ? "true" : "false");
    oss << "}}";
    WebSocketServer::broadcast_json_message(oss.str());

    return true;
}

bool ContinuousScanner::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!running_.exchange(false)) {
        return false;
    }
    state_.store(IDLE);

    if (scanner_thread_.joinable()) {
        scanner_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        peak_memory_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (auto& t : tracked_signals_) {
            if (t.active && t.decimator_id >= 0) {
                decimator_manager.setFrequencyOffset(t.decimator_id, 0.0f);
                std::ostringstream oss;
                oss << "{\"decimator_freq_offset\":{\"id\":" << t.decimator_id
                    << ",\"offset_hz\":0}}";
                WebSocketServer::broadcast_json_message(oss.str());
            }
            t = TrackedSignal();
        }
        num_tracked_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(band_plan_mutex_);
        current_band_index_ = 0;
    }

    std::string cmd = "{\"set_stability_delay\":{\"delay_ms\":" +
                      std::to_string(3000) + "}}";
    ControlHandler::send_control_command(cmd);
    std::cout << "ContinuousScanner: Restored server stability delay to "
              << 3000 << "ms" << std::endl;

    std::cout << "ContinuousScanner: Stopped" << std::endl;

    WebSocketServer::broadcast_json_message(
        "{\"continuous_scanner\":{\"state\":\"stopped\"}}");

    return true;
}

// ============================================================================
// Settings
// ============================================================================

void ContinuousScanner::setDwellTime(int dwell_ms) {
    dwell_ms = std::max(100, std::min(30000, dwell_ms));
    dwell_time_ms_.store(dwell_ms);
    std::cout << "ContinuousScanner: Dwell time set to " << dwell_ms << " ms"
              << std::endl;
}

void ContinuousScanner::setSquelchLevel(float squelch_db) {
    squelch_level_db_.store(squelch_db);

    std::cout << "ContinuousScanner: Squelch set to " << squelch_db << " dB"
              << std::endl;

    std::stringstream ss;
    ss << "{\"continuous_scanner\":{\"squelch_db\":" << squelch_db << "}}";
    WebSocketServer::broadcast_json_message(ss.str());
}

void ContinuousScanner::setDecayTime(int decay_ms) {
    decay_ms = std::max(0, std::min(30000, decay_ms));
    decay_time_ms_.store(decay_ms);
    std::cout << "ContinuousScanner: Decay time set to " << decay_ms << " ms"
              << std::endl;
}

void ContinuousScanner::setShowSquelch(bool show) {
    show_squelch_.store(show);

    std::stringstream ss;
    ss << "{\"continuous_scanner\":{\"show_squelch\":"
       << (show ? "true" : "false") << "}}";
    WebSocketServer::broadcast_json_message(ss.str());
}

void ContinuousScanner::setWidebandScanRange(float start_hz, float end_hz) {
    if (start_hz >= end_hz) {
        std::cerr << "ContinuousScanner: Invalid wideband range: start >= end"
                  << std::endl;
        return;
    }
    wideband_start_hz_.store(start_hz);
    wideband_end_hz_.store(end_hz);
    std::cout << "ContinuousScanner: Wideband range set to "
              << start_hz * 1e-6f << " - " << end_hz * 1e-6f << " MHz"
              << std::endl;
}

void ContinuousScanner::setWidebandScanEnabled(bool enable) {
    wideband_scan_enabled_.store(enable);
    std::cout << "ContinuousScanner: Wideband scan "
              << (enable ? "ENABLED" : "DISABLED") << std::endl;
}

// ============================================================================
// FFT snapshot and signal detection
// ============================================================================

void ContinuousScanner::refreshFFTData() {
    int ch = active_channel.load();
    std::unique_lock<std::mutex> lock(fft_mutex);
    if (!data_ready.load()) return;
    if (ch < 0 || num_channels.load() <= 0) return;
    if (ch >= (int)fft_averaged.size()) return;
    std::vector<float> spec = fft_averaged[ch];
    lock.unlock();

    float freq = ChannelManager::get_frequency(ch);

    if (spec.size() > 10) {
        // Use the SAME noise-floor estimate as the FFT display normalization
        // (10th percentile of the center 80% of bins, skipping the filter
        // edge rolloff). Estimating over ALL bins let the rolloff skirt drag
        // the floor several dB low, which made the squelch trigger on signals
        // that sat below the squelch line drawn on the FFT display.
        float noise = FFTProcessor::calculate_noise_floor(spec);
        if (noise <= -190.0f) {
            // Fallback (size mismatch): 10th percentile over all bins
            std::vector<float> tmp = spec;
            size_t idx = tmp.size() / 10;
            std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
            noise = tmp[idx];
        }
        for (auto& v : spec) v -= noise;
    }

    std::lock_guard<std::mutex> lk(fft_data_mutex_);
    current_fft_ = std::move(spec);
    fft_center_freq_hz_ = freq;
    fft_sample_rate_hz_ = 2400000.0f;
    last_fft_time_ = std::chrono::steady_clock::now();
}

float ContinuousScanner::getScanBandwidthHz() {
    float bw_hz = 10000.0f;
    auto decimators = decimator_manager.getAllDecimators();
    for (const auto& d : decimators) {
        if (!d || !d->enabled || d->being_deleted.load()) continue;
        bw_hz = get_bandwidth_option(d->bandwidth_index).bandwidth_mhz * 1e6f;
        break;  // first usable decimator (D0, the one FM audio follows)
    }
    return std::max(bw_hz, 10000.0f);
}

bool ContinuousScanner::isCalibrationActive() {
    // Same criterion the RETUNING handler uses for "calibration in progress":
    // noise source on, or phase compensation not yet in VERIFYING/CONVERGED.
    // Additionally require a continuous noise-quiet window
    // (ScannerManager::NOISE_QUIET_MS) so short calibration bursts between
    // polls (every 100ms here) don't slip through unnoticed.
    return scanner_manager.isNoiseRecentlyActive() ||
           scanner_manager.getPhaseState() <= 3;
}

std::vector<std::pair<float, float>> ContinuousScanner::findAllSignals() {
    std::vector<std::pair<float, float>> signals;

    // One detected signal occupies the user-selected decimator bandwidth.
    // (Read before taking fft_data_mutex_ - it locks the decimator manager.)
    float half_bw_hz = getScanBandwidthHz() * 0.5f;

    std::lock_guard<std::mutex> lock(fft_data_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - last_fft_time_).count();
    if (age_ms > 2000 || current_fft_.empty() || fft_sample_rate_hz_ <= 0.0f) {
        return signals;
    }

    float squelch   = squelch_level_db_.load();
    float edge_clip = current_edge_clip.load();

    int   num_bins   = (int)current_fft_.size();
    float bin_width  = fft_sample_rate_hz_ / (float)num_bins;
    float start_freq = fft_center_freq_hz_ - fft_sample_rate_hz_ * 0.5f;

    // Only scan the centered edge_clip fraction of the spectrum
    int usable_bins = (int)((float)num_bins * edge_clip);
    int start_bin   = (num_bins - usable_bins) / 2;
    int end_bin     = start_bin + usable_bins;

    // Local-maximum window: half the decimator bandwidth, minimum 5 bins.
    // A wide signal (e.g. 200 kHz broadcast FM) then yields a single peak
    // instead of one detection per spectral ripple.
    int window = (int)(half_bw_hz / bin_width);
    if (window < 5) window = 5;

    // Scan the entire usable region - including the outer half-bandwidth at
    // each edge, so a signal near the spectrum edge is still detected and
    // tuned even if the decimator bandwidth won't fully fit around it. The
    // local-max window and edge walk are simply clamped to the usable region.
    for (int i = start_bin; i < end_bin; i++) {
        float power = current_fft_[i];
        if (power <= squelch) continue;

        // Must be the maximum within +/- window bins (clamped to the
        // usable region so edge bins can still qualify as peaks)
        int lo = std::max(start_bin, i - window);
        int hi = std::min(end_bin - 1, i + window);
        bool is_peak = true;
        for (int j = lo; j <= hi; j++) {
            if (power < current_fft_[j]) { is_peak = false; break; }
        }
        if (!is_peak) continue;

        // Reject flat plateaus: must be strictly above at least one neighbor
        float left  = current_fft_[std::max(i - 1, 0)];
        float right = current_fft_[std::min(i + 1, num_bins - 1)];
        if (!(power > left) && !(power > right)) continue;

        // Determine the signal's extent: walk out from the peak until power
        // drops below squelch (bounded by the decimator bandwidth and the
        // usable region), then center the detection on that span. For a
        // signal cut off by the spectrum edge the center is biased inward,
        // which is the best the decimator can do anyway.
        int left_edge  = i;
        int right_edge = i;
        while (left_edge > lo && current_fft_[left_edge - 1] > squelch) {
            left_edge--;
        }
        while (right_edge < hi && current_fft_[right_edge + 1] > squelch) {
            right_edge++;
        }

        float center_bin;
        if (right_edge > left_edge) {
            center_bin = 0.5f * (float)(left_edge + right_edge);
        } else {
            // Single-bin peak: parabolic interpolation for sub-bin accuracy
            float denom = left + right - 2.0f * power;
            float delta = 0.0f;
            if (std::fabs(denom) > 1e-4f) {
                delta = 0.5f * (left - right) / denom;
                delta = std::min(0.5f, std::max(-0.5f, delta));
            }
            center_bin = (float)i + delta;
        }
        float freq = start_freq + center_bin * bin_width;

        // Merge detections closer than half the decimator bandwidth
        // (keep the stronger one)
        bool merged = false;
        for (auto& sig : signals) {
            if (std::fabs(sig.first - freq) < half_bw_hz) {
                if (power > sig.second) {
                    sig.first  = freq;
                    sig.second = power;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            signals.push_back({freq, power});
        }
    }

    std::sort(signals.begin(), signals.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return signals;
}

float ContinuousScanner::getSignalStrength(float freq_hz) {
    // Read before taking fft_data_mutex_ (locks the decimator manager)
    float half_bw_hz = getScanBandwidthHz() * 0.5f;

    std::lock_guard<std::mutex> lock(fft_data_mutex_);

    if (current_fft_.empty() || fft_sample_rate_hz_ <= 0.0f) {
        return -100.0f;
    }

    int   num_bins   = (int)current_fft_.size();
    float bin_width  = fft_sample_rate_hz_ / (float)num_bins;
    float start_freq = fft_center_freq_hz_ - fft_sample_rate_hz_ * 0.5f;
    int   bin = (int)((freq_hz - start_freq) / fft_sample_rate_hz_
                      * (float)current_fft_.size());
    if (bin < 0 || bin >= num_bins) {
        return -100.0f;
    }

    // Strongest bin anywhere inside the signal's bandwidth - a wide signal's
    // energy is spread out, so a few bins around the center are not enough.
    int win = (int)(half_bw_hz / bin_width);
    if (win < 3) win = 3;

    float strength = current_fft_[bin];
    for (int j = std::max(0, bin - win); j <= std::min(num_bins - 1, bin + win); j++) {
        strength = std::max(strength, current_fft_[j]);
    }
    return strength;
}

// ============================================================================
// Peak memory
// ============================================================================

void ContinuousScanner::updatePeakMemory(const std::vector<std::pair<float, float>>& signals) {
    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto now = std::chrono::steady_clock::now();
    int decay_ms = decay_time_ms_.load();

    for (const auto& sig : signals) {
        int key_khz = (int)(sig.first * 0.001f);
        peak_memory_[key_khz] = now;
    }

    if (decay_ms <= 0) {
        peak_memory_.clear();
        return;
    }

    for (auto it = peak_memory_.begin(); it != peak_memory_.end();) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - it->second).count();
        if (age_ms > decay_ms) {
            it = peak_memory_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::pair<float, float>> ContinuousScanner::getSignalsFromMemory() {
    int decay_ms = decay_time_ms_.load();
    if (decay_ms <= 0) {
        return {};
    }

    // Read before taking memory_mutex_ (locks the decimator manager)
    float half_bw_hz = getScanBandwidthHz() * 0.5f;

    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<float, float>> remembered;
    for (const auto& entry : peak_memory_) {
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - entry.second).count();
        if (age_ms > decay_ms) continue;
        remembered.push_back({(float)entry.first * 1000.0f, 0.0f});
    }

    std::sort(remembered.begin(), remembered.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Deduplicate within half the decimator bandwidth
    std::vector<std::pair<float, float>> result;
    for (const auto& sig : remembered) {
        bool duplicate = false;
        for (const auto& r : result) {
            if (std::fabs(r.first - sig.first) < half_bw_hz) { duplicate = true; break; }
        }
        if (!duplicate) {
            result.push_back(sig);
        }
    }
    return result;
}

bool ContinuousScanner::isSignalInMemory(float freq_hz) {
    int decay_ms = decay_time_ms_.load();
    if (decay_ms <= 0) {
        return false;
    }

    int key_khz = (int)(freq_hz * 0.001f);

    // Match within half the decimator bandwidth (10 kHz minimum) - a wide
    // signal's detected center can shift between visits.
    int tol_khz = std::max(10, (int)(getScanBandwidthHz() * 0.5f * 0.001f));

    std::lock_guard<std::mutex> lock(memory_mutex_);
    auto now = std::chrono::steady_clock::now();
    for (const auto& entry : peak_memory_) {
        if (std::abs(entry.first - key_khz) <= tol_khz) {
            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - entry.second).count();
            if (age_ms <= decay_ms) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Decimator tracking
// ============================================================================

std::vector<int> ContinuousScanner::getAvailableDecimators() {
    std::vector<int> available;
    auto decimators = decimator_manager.getAllDecimators();
    for (const auto& d : decimators) {
        if (!d) continue;
        if (!d->enabled) continue;
        if (d->being_deleted.load()) continue;
        available.push_back(d->id);
    }
    return available;
}

void ContinuousScanner::tuneTo(int decimator_id, float freq_hz, float signal_db,
                               int signal_index) {
    // Baseband offset from the current FFT center frequency
    float offset_hz;
    {
        std::lock_guard<std::mutex> lock(fft_data_mutex_);
        offset_hz = freq_hz - fft_center_freq_hz_;
    }

    decimator_manager.setFrequencyOffset(decimator_id, offset_hz);

    // Record the lock: reuse the slot with the same decimator id,
    // otherwise the first inactive slot
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        int slot = -1;
        for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
            if (tracked_signals_[i].decimator_id == decimator_id) { slot = i; break; }
        }
        if (slot < 0) {
            for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
                if (!tracked_signals_[i].active) { slot = i; break; }
            }
        }
        if (slot >= 0) {
            TrackedSignal& s = tracked_signals_[slot];
            s.decimator_id = decimator_id;
            s.freq_hz      = freq_hz;
            s.offset_hz    = offset_hz;
            s.signal_db    = signal_db;
            s.lock_time    = std::chrono::steady_clock::now();
            s.signal_index = signal_index;
            s.active       = true;
        }
        int count = 0;
        for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
            if (tracked_signals_[i].active) count++;
        }
        num_tracked_ = count;
    }

    std::cout << "ContinuousScanner: D" << decimator_id << " -> "
              << freq_hz * 1e-6f << " MHz (offset " << offset_hz * 1e-3f
              << " kHz, " << signal_db << " dB)" << std::endl;

    // Point the FM demodulator at the first active tracked signal's decimator
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
            if (tracked_signals_[i].active) {
                if (decimator_manager.getFMDecimatorId() != tracked_signals_[i].decimator_id) {
                    decimator_manager.setFMDecimatorId(tracked_signals_[i].decimator_id);
                }
                break;
            }
        }
    }

    {
        std::stringstream ss;
        ss << "{\"decimator_freq_offset\":{\"id\":" << decimator_id
           << ",\"offset_hz\":" << offset_hz << "}}";
        WebSocketServer::broadcast_json_message(ss.str());
    }
    {
        std::stringstream ss;
        ss << "{\"continuous_scanner\":{\"signal_locked\":{"
           << "\"decimator_id\":" << decimator_id << ","
           << "\"freq_mhz\":" << freq_hz * 1e-6f << ","
           << "\"signal_db\":" << signal_db << "}}}";
        WebSocketServer::broadcast_json_message(ss.str());
    }
}

void ContinuousScanner::updateFMToSignal() {
    std::lock_guard<std::mutex> lock(tracked_mutex_);
    for (auto& sig : tracked_signals_) {
        if (sig.active) {
            if (decimator_manager.getFMDecimatorId() != sig.decimator_id) {
                decimator_manager.setFMDecimatorId(sig.decimator_id);
            }
            return;
        }
    }
}

void ContinuousScanner::clearBandState() {
    {
        std::lock_guard<std::mutex> lock(signals_mutex_);
        detected_signals_.clear();
        current_signal_index_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(memory_mutex_);
        peak_memory_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        num_tracked_ = 0;
        for (auto& sig : tracked_signals_) sig = TrackedSignal{};
    }
}

// ============================================================================
// Wideband band plan
// ============================================================================

void ContinuousScanner::buildBandPlan() {
    constexpr float BAND_WIDTH_HZ = 2400000.0f;  // full tuner bandwidth
    constexpr float HALF_BAND_HZ  = 1200000.0f;

    float start_hz = wideband_start_hz_.load();
    float end_hz   = wideband_end_hz_.load();

    if (start_hz <= 0.0f || end_hz <= start_hz) {
        std::cerr << "ContinuousScanner: Invalid wideband range for band plan"
                  << std::endl;
        return;
    }

    // Guard against a degenerate edge clip (a ~0 step means endless bands).
    // Written out rather than std::max, which passes NaN straight through.
    float edge_clip = current_edge_clip.load();
    if (!(edge_clip >= 0.1f)) edge_clip = 0.1f;
    if (edge_clip > 1.0f) edge_clip = 1.0f;
    const double span = static_cast<double>(end_hz) - start_hz;

    // Step by the usable (edge-clipped) bandwidth so band edges overlap. The
    // count is computed up front and each center derived from its index:
    // accumulating `center += step` in float stops advancing at large
    // frequencies, and a huge range must not allocate without bound.
    constexpr int MAX_BANDS = 4096;
    const double step = static_cast<double>(edge_clip) * BAND_WIDTH_HZ;
    const double n_bands = (span <= BAND_WIDTH_HZ) ? 1.0
                         : std::ceil((span - BAND_WIDTH_HZ) / step) + 1.0;
    if (!(n_bands <= MAX_BANDS)) {
        std::cerr << "ContinuousScanner: wideband range needs " << n_bands
                  << " bands (max " << MAX_BANDS << "), not scanning" << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(band_plan_mutex_);
    band_centers_.clear();

    if (span <= BAND_WIDTH_HZ) {
        // Whole range fits in one tuner band
        band_centers_.push_back((start_hz + end_hz) * 0.5f);
    } else {
        for (int i = 0; i < static_cast<int>(n_bands); i++) {
            band_centers_.push_back(static_cast<float>(start_hz + HALF_BAND_HZ + i * step));
        }
    }

    current_band_index_ = 0;
    num_bands_ = (int)band_centers_.size();

    std::cout << "ContinuousScanner: Band plan built with " << num_bands_
              << " bands:" << std::endl;
    for (int i = 0; i < num_bands_; i++) {
        float c = band_centers_[i];
        std::cout << "  Band " << i << ": center=" << c * 1e-6f
                  << " MHz (covers " << (c - HALF_BAND_HZ) * 1e-6f
                  << " - " << (c + HALF_BAND_HZ) * 1e-6f
                  << " MHz)" << std::endl;
    }
}

void ContinuousScanner::initiateRetune(int band_index) {
    float target_freq;
    {
        std::lock_guard<std::mutex> lock(band_plan_mutex_);
        if (band_index < 0 || band_index >= (int)band_centers_.size()) return;
        target_freq = band_centers_[band_index];
        current_band_index_ = band_index;
    }

    // Skip if the SDR center is already within 10 kHz of the target
    float current_freq = ChannelManager::get_frequency(active_channel.load());
    if (std::fabs(current_freq - target_freq) < 10000.0f) {
        std::cout << "ContinuousScanner: Already at " << target_freq * 1e-6f
                  << " MHz (current=" << current_freq * 1e-6f
                  << "), skipping retune" << std::endl;
        clearBandState();
        state_.store(SCANNING);
        return;
    }

    // Zero decimator offsets and clear tracking before moving the center
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
            if (tracked_signals_[i].active && tracked_signals_[i].decimator_id >= 0) {
                decimator_manager.setFrequencyOffset(tracked_signals_[i].decimator_id, 0.0f);
            }
            tracked_signals_[i] = TrackedSignal{};
        }
        num_tracked_ = 0;
    }

    // Command the Heimdall server to retune the hardware
    {
        std::stringstream ss;
        ss << "{\"set_frequency\":{\"frequency\":" << (unsigned int)target_freq << "}}";
        ControlHandler::send_control_command(ss.str());
    }

    ChannelManager::set_frequency(target_freq, -1);

    // Update every decimator's MUSIC processor to the new center frequency
    for (const auto& dec : decimator_manager.getAllDecimators()) {
        if (dec && dec->music_processor && !dec->being_deleted.load()) {
            dec->music_processor->setFrequency(target_freq);
        }
    }

    state_.store(RETUNING);
    retune_start_time_ = std::chrono::steady_clock::now();
    calibration_started_ = false;
    calibration_done_    = false;

    uint32_t phase_state = scanner_manager.getPhaseState();
    bool noise_src = scanner_manager.isNoiseSourceActive();

    std::cout << "ContinuousScanner: Retuning to " << target_freq * 1e-6f
              << " MHz (band " << band_index << "/" << num_bands_
              << "), current phase_state=" << phase_state
              << ", noise_src=" << (noise_src ? "ON" : "OFF") << std::endl;

    {
        std::stringstream ss;
        ss << "{\"continuous_scanner\":{\"state\":\"RETUNING\","
           << "\"retune_freq_mhz\":" << target_freq * 1e-6f << ","
           << "\"wideband_scan\":{\"current_band\":" << band_index
           << ",\"total_bands\":" << num_bands_
           << ",\"current_center_mhz\":" << target_freq * 1e-6f << "}}}";
        WebSocketServer::broadcast_json_message(ss.str());
    }
}

void ContinuousScanner::advanceToNextBand() {
    int old_band, next_band, num;
    {
        std::lock_guard<std::mutex> lock(band_plan_mutex_);
        num = num_bands_;
        if (num <= 0) return;
        old_band  = current_band_index_;
        next_band = (old_band + 1) % num;
        current_band_index_ = next_band;
    }

    if (next_band == 0) {
        std::cout << "ContinuousScanner: Wrapping from band " << old_band
                  << " back to band 0 (full cycle of " << num << " bands)"
                  << std::endl;
    } else {
        std::cout << "ContinuousScanner: Advancing from band " << old_band
                  << " to band " << next_band << "/" << num << std::endl;
    }
    initiateRetune(next_band);
}

// ============================================================================
// Scan loop
// ============================================================================

void ContinuousScanner::scannerThread() {
    std::cout << "ContinuousScanner: Scanner thread started" << std::endl;

    int empty_scan_count = 0;
    bool calib_pause_logged = false;

    // After an FFT averaging reset, hold off scanning until the exponential
    // average has rebuilt from several clean frames. A single frame is too
    // noisy: weak signals sit below squelch and the first tune of a band
    // would land mid-band instead of on the lowest real signal.
    constexpr auto FFT_REBUILD_HOLD = std::chrono::milliseconds(1000);
    auto scan_hold_until = std::chrono::steady_clock::time_point{};

    try {
        while (running_.load()) {
            refreshFFTData();

            int state = state_.load();

            // In wideband scan mode the server recalibrates after every band
            // retune (and may recalibrate spontaneously). While that runs the
            // FFT contains noise-source garbage: don't detect signals, don't
            // feed peak memory, don't scan/tune, and hold the dwell countdown.
            // The RETUNING state does its own calibration tracking.
            bool calibrating = wideband_scan_enabled_.load() &&
                               state != RETUNING && isCalibrationActive();
            if (calibrating && !calib_pause_logged) {
                std::cout << "ContinuousScanner: Calibration active - pausing "
                          << "scan/dwell until it completes" << std::endl;
                calib_pause_logged = true;
            } else if (!calibrating && calib_pause_logged) {
                // Calibration ran while we were scanning/dwelling: the FFT
                // average is full of noise-source garbage. Re-initialize it
                // before trusting any detections again.
                uint32_t old_gen = fft_reset_generation.fetch_add(1, std::memory_order_release);
                fft_data_valid.store(false, std::memory_order_release);
                scan_hold_until = std::chrono::steady_clock::now() + FFT_REBUILD_HOLD;
                std::cout << "ContinuousScanner: Calibration finished - resuming"
                          << " (FFT averaging reset, gen " << old_gen << " -> "
                          << (old_gen + 1) << ")" << std::endl;
                calib_pause_logged = false;
            }

            // After an FFT averaging reset, fft_averaged holds pre-reset
            // history until the next frame re-initializes it, and then needs
            // FFT_REBUILD_HOLD to average out single-frame noise. Don't
            // detect or tune until both have passed.
            bool fft_ready = fft_data_valid.load(std::memory_order_acquire) &&
                             std::chrono::steady_clock::now() >= scan_hold_until;

            std::vector<std::pair<float, float>> signals;
            if (!calibrating && fft_ready && state != RETUNING) {
                signals = findAllSignals();
                updatePeakMemory(signals);
            }

            if (calibrating && state == DWELLING) {
                // Hold the dwell countdown: restart the timers so the full
                // dwell time runs against clean post-calibration data.
                auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(tracked_mutex_);
                for (auto& sig : tracked_signals_) {
                    if (sig.active) sig.lock_time = now;
                }
            } else if (calibrating) {
                // SCANNING/IDLE: just wait

            } else if (state == SCANNING) {
                if (!fft_ready) {
                    // Waiting for the first clean FFT frame after a band
                    // retune - the first tune of a band must come from fresh
                    // data so the sweep starts at the lowest real signal.
                } else if (!signals.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(signals_mutex_);
                        detected_signals_ = signals;
                        current_signal_index_ = 0;
                    }
                    auto available = getAvailableDecimators();
                    if (available.empty()) {
                        std::cerr << "ContinuousScanner: No decimator available" << std::endl;
                    } else {
                        // At most one VFO per tracking slot (as when dwelling): a
                        // 4th+ VFO was retuned with no slot to track it, then never
                        // zeroed on retune/stop and missing from the status.
                        int num_to_tune = std::min({(int)signals.size(), (int)available.size(),
                                                    MAX_TRACKED_SIGNALS});
                        for (int i = 0; i < num_to_tune; i++) {
                            tuneTo(available[i], signals[i].first, signals[i].second, i);
                        }
                        state_.store(DWELLING);
                        empty_scan_count = 0;
                    }
                } else if (wideband_scan_enabled_.load()) {
                    // Nothing found in this band: give it 5 scan passes, then move on
                    empty_scan_count++;
                    if (empty_scan_count > 4) {
                        advanceToNextBand();
                        empty_scan_count = 0;
                    }
                }

            } else if (state == DWELLING) {
                int   dwell_ms = dwell_time_ms_.load();
                float squelch  = squelch_level_db_.load();

                bool any_active = false;
                {
                    std::lock_guard<std::mutex> lock(tracked_mutex_);
                    for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
                        if (tracked_signals_[i].active) { any_active = true; break; }
                    }
                }
                if (!any_active) {
                    state_.store(SCANNING);
                } else {
                    auto now = std::chrono::steady_clock::now();
                    bool all_expired = true;
                    {
                        std::lock_guard<std::mutex> lock(tracked_mutex_);
                        for (auto& sig : tracked_signals_) {
                            if (!sig.active) continue;
                            float strength = getSignalStrength(sig.freq_hz);
                            long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  now - sig.lock_time).count();
                            if (elapsed_ms < dwell_ms) all_expired = false;
                            if (elapsed_ms > 500) {                       // 500 ms grace period
                                if (strength < squelch - 3.0f) {          // 3 dB hysteresis
                                    if (isSignalInMemory(sig.freq_hz)) {
                                        sig.signal_db = strength;
                                    }
                                } else {
                                    sig.signal_db = strength;
                                    std::lock_guard<std::mutex> mem_lock(memory_mutex_);
                                    peak_memory_[(int)(sig.freq_hz * 0.001f)] = now;
                                }
                            } else {
                                sig.signal_db = strength;
                            }
                        }
                    }

                    if (all_expired) {
                        auto remembered = getSignalsFromMemory();

                        if (remembered.empty() && signals.empty()) {
                            bool any_in_memory = false;
                            {
                                std::lock_guard<std::mutex> lock(tracked_mutex_);
                                for (auto& sig : tracked_signals_) {
                                    if (sig.active && isSignalInMemory(sig.freq_hz)) {
                                        any_in_memory = true;
                                        break;
                                    }
                                }
                            }
                            if (any_in_memory) {
                                // Keep dwelling on remembered signals: restart the dwell timers
                                std::lock_guard<std::mutex> lock(tracked_mutex_);
                                for (auto& sig : tracked_signals_) {
                                    if (sig.active) sig.lock_time = now;
                                }
                                empty_scan_count = 0;
                            } else {
                                empty_scan_count++;
                                if (empty_scan_count > 4) {
                                    {
                                        std::lock_guard<std::mutex> lock(tracked_mutex_);
                                        for (auto& sig : tracked_signals_) sig = TrackedSignal();
                                        num_tracked_ = 0;
                                    }
                                    empty_scan_count = 0;
                                    if (wideband_scan_enabled_.load()) {
                                        advanceToNextBand();
                                    } else {
                                        state_.store(SCANNING);
                                    }
                                }
                            }
                        } else {
                            // Move on to the next signal(s) in the band
                            float last_freq = -1.0f;
                            {
                                std::lock_guard<std::mutex> lock(tracked_mutex_);
                                for (auto& sig : tracked_signals_) {
                                    if (sig.active) last_freq = std::max(last_freq, sig.freq_hz);
                                }
                            }

                            const auto& scan_list = remembered.empty() ? signals : remembered;
                            auto available = getAvailableDecimators();
                            int max_tune = std::min((int)available.size(), 3);
                            float half_bw_hz = getScanBandwidthHz() * 0.5f;

                            int  start_index = 0;
                            bool advanced    = false;
                            if (last_freq > 0.0f) {
                                bool found = false;
                                for (int i = 0; i < (int)scan_list.size(); i++) {
                                    if (scan_list[i].first > last_freq + half_bw_hz) {
                                        start_index = i;
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found) {
                                    if (wideband_scan_enabled_.load()) {
                                        std::cout << "ContinuousScanner: Band scan complete (wrapped past "
                                                  << last_freq * 1e-6f
                                                  << " MHz), advancing to next band" << std::endl;
                                        advanceToNextBand();
                                        empty_scan_count = 0;
                                        advanced = true;
                                    }
                                    // non-wideband: wrap around to start_index = 0
                                }
                            }

                            if (!advanced) {
                                int num_to_tune = std::min((int)scan_list.size(), max_tune);
                                for (int k = 0; k < num_to_tune; k++) {
                                    int   idx  = (start_index + k) % (int)scan_list.size();
                                    float freq = scan_list[idx].first;
                                    float db   = 0.0f;
                                    for (const auto& sig : signals) {
                                        if (std::fabs(sig.first - freq) < half_bw_hz) {
                                            db = sig.second;
                                            break;
                                        }
                                    }
                                    tuneTo(available[k], freq, db, idx);
                                }
                                {
                                    std::lock_guard<std::mutex> lock(signals_mutex_);
                                    detected_signals_ = scan_list;
                                    current_signal_index_ = start_index;
                                }
                                empty_scan_count = 0;
                                // stays in DWELLING with fresh dwell timers
                            }
                        }
                    }
                }

            } else if (state == RETUNING) {
                auto now = std::chrono::steady_clock::now();
                long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      now - retune_start_time_).count();

                uint32_t phase = scanner_manager.getPhaseState();
                bool     noise = scanner_manager.isNoiseSourceActive();
                const char* phase_names[] = {"WAITING_LAG", "MEASURING", "WAITING_STAB",
                                             "APPLYING", "VERIFYING", "CONVERGED"};
                const char* phase_name = (phase <= 5) ? phase_names[phase] : "UNKNOWN";

                if (!calibration_started_) {
                    if (noise || phase <= 3) {
                        calibration_started_ = true;
                        std::cout << "ContinuousScanner: Calibration started (phase=" << phase_name
                                  << ", noise=" << (noise ? "ON" : "OFF")
                                  << ", elapsed=" << elapsed_ms << "ms)" << std::endl;
                    } else if (elapsed_ms > 60000) {
                        std::cout << "ContinuousScanner: Timeout waiting for calibration to start after "
                                  << elapsed_ms << "ms, skipping band" << std::endl;
                        clearBandState();
                        advanceToNextBand();
                        empty_scan_count = 0;
                    }
                } else if (!calibration_done_) {
                    // Complete only after the noise source has been quiet for
                    // a continuous window: the server pulses it during
                    // verification, and a momentary OFF reading between
                    // bursts must not count as "calibration finished".
                    if (!noise && phase > 3 && scanner_manager.msSinceNoiseActive() >= ScannerManager::NOISE_QUIET_MS) {
                        std::cout << "ContinuousScanner: Calibration complete (phase=" << phase_name
                                  << ", noise=OFF, elapsed=" << elapsed_ms << "ms) — settling"
                                  << std::endl;
                        calibration_done_time_ = now;
                        calibration_done_ = true;
                    } else {
                        static int retune_log_counter = 0;
                        if (++retune_log_counter % 20 == 0) {
                            std::cout << "ContinuousScanner: Waiting for calibration... phase="
                                      << phase_name << ", noise=" << (noise ? "ON" : "OFF")
                                      << ", elapsed=" << elapsed_ms << "ms" << std::endl;
                        }
                        if (elapsed_ms > 60000) {
                            std::cout << "ContinuousScanner: Calibration timeout after "
                                      << elapsed_ms << "ms, skipping band" << std::endl;
                            clearBandState();
                            advanceToNextBand();
                            empty_scan_count = 0;
                        }
                    }
                } else {
                    long settle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - calibration_done_time_).count();
                    if (settle_ms >= 500) {
                        std::cout << "ContinuousScanner: Settle complete, scanning band "
                                  << current_band_index_ << "/" << num_bands_ << std::endl;

                        // Discard the FFT averaging history (previous band +
                        // calibration noise). SCANNING then waits for the
                        // first clean frame plus FFT_REBUILD_HOLD, so the
                        // band's first tune comes from a settled average and
                        // starts at the lowest real signal.
                        uint32_t old_gen = fft_reset_generation.fetch_add(1, std::memory_order_release);
                        fft_data_valid.store(false, std::memory_order_release);
                        scan_hold_until = std::chrono::steady_clock::now() + FFT_REBUILD_HOLD;
                        std::cout << "ContinuousScanner: FFT averaging reset (gen "
                                  << old_gen << " -> " << (old_gen + 1)
                                  << ") for fresh band scan" << std::endl;

                        clearBandState();
                        state_.store(SCANNING);
                        empty_scan_count = 0;
                    }
                }
            }
            // IDLE: no handling, just poll

            static int status_counter = 0;
            if (++status_counter % 10 == 0) {
                WebSocketServer::broadcast_json_message(getStatusJson());
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (const std::exception& e) {
        std::cerr << "ContinuousScanner: Thread exception: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "ContinuousScanner: Thread unknown exception" << std::endl;
    }

    std::cout << "ContinuousScanner: Thread exiting" << std::endl;
}

// ============================================================================
// Status
// ============================================================================

std::string ContinuousScanner::getStatusJson() const {
    std::stringstream ss;

    int st = state_.load();
    const char* state_str = (st == DWELLING)  ? "DWELLING"
                          : (st == RETUNING)  ? "RETUNING"
                          : (st == SCANNING)  ? "SCANNING"
                          : "IDLE";

    ss << "{\"continuous_scanner\":{";
    ss << "\"running\":" << (running_.load() ? "true" : "false") << ",";
    ss << "\"state\":\"" << state_str << "\",";
    ss << "\"dwell_ms\":" << dwell_time_ms_.load() << ",";
    ss << "\"squelch_db\":" << squelch_level_db_.load() << ",";
    ss << "\"show_squelch\":" << (show_squelch_.load() ? "true" : "false") << ",";
    ss << "\"decay_ms\":" << decay_time_ms_.load();

    {
        std::lock_guard<std::mutex> lock(signals_mutex_);
        ss << ",\"detected_count\":" << detected_signals_.size();
        ss << ",\"signal_index\":" << current_signal_index_;
    }

    ss << ",\"tracked_signals\":[";
    {
        std::lock_guard<std::mutex> lock(tracked_mutex_);
        bool first = true;
        for (int i = 0; i < MAX_TRACKED_SIGNALS; i++) {
            const TrackedSignal& sig = tracked_signals_[i];
            if (!sig.active) continue;
            if (!first) ss << ",";
            first = false;
            long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sig.lock_time).count();
            ss << "{\"decimator_id\":" << sig.decimator_id
               << ",\"freq_mhz\":" << (sig.freq_hz * 1e-6f)
               << ",\"signal_db\":" << sig.signal_db
               << ",\"dwell_elapsed_ms\":" << elapsed_ms
               << "}";
        }
    }
    ss << "]";

    ss << ",\"wideband_scan\":{";
    ss << "\"enabled\":" << (wideband_scan_enabled_.load() ? "true" : "false");
    ss << ",\"start_mhz\":" << (wideband_start_hz_.load() * 1e-6f);
    ss << ",\"end_mhz\":" << (wideband_end_hz_.load() * 1e-6f);
    {
        std::lock_guard<std::mutex> lock(band_plan_mutex_);
        ss << ",\"current_band\":" << current_band_index_;
        ss << ",\"total_bands\":" << num_bands_;
        if (num_bands_ > 0 && current_band_index_ < (int)band_centers_.size()) {
            ss << ",\"current_center_mhz\":"
               << (band_centers_[current_band_index_] * 1e-6f);
        }
    }
    ss << "}";
    ss << "}}";

    return ss.str();
}
