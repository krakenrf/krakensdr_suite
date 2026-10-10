#include "signal_processing/music_processor.hpp"
#include "array_cal.hpp"
#include "utils/parse_num.hpp"
#include "config.hpp"
#include "globals.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <complex>
#include <chrono>
#include <limits>

using namespace std;
using namespace Eigen;

MUSICProcessor::MUSICProcessor() : current_topology(ArrayTopology::UCA), pseudospectrum(DOA_NUM_ANGLES) {
    
    try {
        // Runtime element count: follow whatever the server is streaming right
        // now (processors are created after the first packets, and
        // syncElementCount() tracks later changes frame by frame).
        num_elements_ = std::clamp(active_num_elements.load(std::memory_order_relaxed),
                                   2, DOA_NUM_ELEMENTS);

        // Initialize default configuration
        config_.snapshot_length = 256;
        config_.num_snapshots = 32;
        config_.overlap_samples = 64;
        config_.min_snapshots = 16;
        
        // Initialize pseudospectrum
        pseudospectrum.setZero();
        
        // Initialize accumulator with power-of-2 buffer
        size_t requested_size = accumulatorSamplesFor(config_);
        accumulator_.resize(requested_size, num_elements_);
        accumulator_.total_samples = 0;
        accumulator_.global_sample_index = 0;
        
        // Pre-allocate working matrices to avoid repeated allocations
        snapshot_working_matrix_.resize(num_elements_, config_.snapshot_length);
        correlation_matrix_.resize(num_elements_, num_elements_);
        combined_snapshots_.resize(num_elements_, config_.num_snapshots * config_.snapshot_length);
        
        // Initialize snapshot manager
        snapshot_mgr_.snapshots.reserve(config_.num_snapshots);
        snapshot_mgr_.snapshots_ready = false;
        
        // Pre-calculate steering vectors using UCA as default
        updateSteeringVectors();
        
        cout << "MUSIC Processor initialized with optimized power-of-2 buffer" << endl;
        cout << "Configuration: " << config_.num_snapshots << " snapshots of " << config_.snapshot_length << " samples" << endl;
        cout << "Buffer size: " << accumulator_.buffer_size << " samples (mask: 0x" << hex << accumulator_.buffer_size_mask << dec << ")" << endl;
    }
    catch (const std::exception& e) {
        cerr << "ERROR: Exception during MUSIC processor initialization: " << e.what() << endl;
        throw;
    }
    catch (...) {
        cerr << "ERROR: Unknown exception during MUSIC processor initialization" << endl;
        throw;
    }
}


MUSICProcessor::~MUSICProcessor() {
    try {
        processing_enabled = false;

        // Wait for any in-flight processing pass to finish before members
        // are destroyed
        {
            lock_guard<mutex> config_lock(config_mutex_);
            lock_guard<mutex> lock(accumulator_.buffer_mutex);
        }

        cout << "MUSIC Processor destroyed safely" << endl;
    }
    catch (...) {
        // Never throw from destructor
    }
}

void MUSICProcessor::setConfig(const MUSICConfig& config) {
    // config_mutex_ excludes the processing thread for real (the previous
    // disable + 50ms sleep was only a heuristic)
    lock_guard<mutex> config_lock(config_mutex_);

    {
        lock_guard<mutex> lock(accumulator_.buffer_mutex);
        
        config_ = config;
        if (config_.snapshot_length > 0 &&
            config_.snapshot_length * config_.num_snapshots > MAX_FRAME_SAMPLES) {
            config_.num_snapshots = max<size_t>(1, MAX_FRAME_SAMPLES / config_.snapshot_length);
            config_.min_snapshots = min(config_.min_snapshots, config_.num_snapshots);
        }
        
        // Use power-of-2 buffer size for fast circular indexing
        size_t requested_size = accumulatorSamplesFor(config_);
        accumulator_.resize(requested_size, num_elements_);
        
        accumulator_.total_samples = 0;
        accumulator_.global_sample_index = 0;
        
        // Resize working matrices
        snapshot_working_matrix_.resize(num_elements_, config_.snapshot_length);
        
        size_t max_combined_samples = config_.num_snapshots * config_.snapshot_length;
        combined_snapshots_.resize(num_elements_, max_combined_samples);
        
        snapshot_mgr_.snapshots.clear();
        snapshot_mgr_.snapshots.reserve(config_.num_snapshots);
        snapshot_mgr_.snapshots_ready = false;

        covariance_avg_valid_ = false;  // snapshot statistics changed
        resetAutoSourceTracking();

        cout << "MUSIC configuration updated: " << config_.num_snapshots
             << " snapshots of " << config_.snapshot_length << " samples" << endl;
        cout << "Buffer resized to: " << num_elements_ << "x" << accumulator_.buffer_size 
             << " (requested: " << requested_size << ", mask: 0x" << hex << accumulator_.buffer_size_mask << dec << ")" << endl;
    }
}

// Accumulator capacity: one frame plus one full-rate input block. MUSIC runs
// right after each block is added (same thread), so a frame never waits on more;
// 3 frames of complex<double> per element (rounded up to a power of two) was
// most of the per-VFO memory at large snapshot settings.
size_t MUSICProcessor::accumulatorSamplesFor(const MUSICConfig& cfg) {
    return cfg.snapshot_length * cfg.num_snapshots + static_cast<size_t>(FFT_SIZE);
}

MUSICConfig MUSICProcessor::getConfig() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return config_;
}

void MUSICProcessor::syncElementCount() {
    // Caller holds config_mutex_. Cheap no-op on the hot path until the
    // global element count (synced from the server's packet header) changes.
    const int target = std::clamp(active_num_elements.load(std::memory_order_relaxed),
                                  2, DOA_NUM_ELEMENTS);
    if (target == num_elements_) return;

    cout << "MUSIC processor: element count " << num_elements_ << " -> "
         << target << ", reinitializing" << endl;

    {
        lock_guard<mutex> lock(accumulator_.buffer_mutex);
        num_elements_ = target;

        // Accumulated samples/snapshots have the old channel count - drop them
        size_t requested_size = accumulatorSamplesFor(config_);
        accumulator_.resize(requested_size, num_elements_);
        accumulator_.total_samples = 0;
        accumulator_.global_sample_index = 0;

        snapshot_working_matrix_.resize(num_elements_, config_.snapshot_length);
        correlation_matrix_.resize(num_elements_, num_elements_);
        combined_snapshots_.resize(num_elements_, config_.num_snapshots * config_.snapshot_length);

        snapshot_mgr_.snapshots.clear();
        snapshot_mgr_.snapshots_ready = false;
    }

    // Frame statistics from the old array are meaningless now
    covariance_avg_valid_ = false;
    resetAutoSourceTracking();

    // Steering vectors are per-element; regenerate for the new count. Custom
    // positions keep their ceiling-sized storage - the first num_elements_
    // entries are used. Elements the user never placed (the count grew past
    // the CUSTOM_POSITIONS sent) get the UI's default instead of the origin,
    // where they made every bearing wrong while the table looked plausible.
    steering_vectors_valid = false;
    steering_vectors_2d_valid_ = false;
    if (custom_positions_valid_) {
        fillDefaultCustomPositions();
        detect3DArray();
    }
    if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) {
        updateSteeringVectors2D();
    } else {
        updateSteeringVectors();
    }
}

void MUSICProcessor::startTiming() {
    last_timing_start_ = std::chrono::steady_clock::now();
}

void MUSICProcessor::endTiming() {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - last_timing_start_);
    double time_ms = duration.count() / 1000.0;
    
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.last_processing_time_ms = time_ms;
    
    // Update running average
    if (stats_.timing_samples == 0) {
        stats_.avg_processing_time_ms = time_ms;
        stats_.timing_samples = 1;
    } else {
        // Exponential moving average with higher weight on recent samples
        const double alpha = 0.1; // 10% weight on new sample
        stats_.avg_processing_time_ms = alpha * time_ms + (1.0 - alpha) * stats_.avg_processing_time_ms;
        stats_.timing_samples = std::min(stats_.timing_samples + 1, static_cast<size_t>(100)); // Cap at 100 samples
    }
}

bool MUSICProcessor::processDecimatedIQ(const SharedDecimator::MultiChannelDecimated& decimated_data) {
    // Quick check without locks first. Any non-empty block is accumulated:
    // frames are built from the accumulator, not from single blocks, and at
    // the narrowest bandwidths a block is tiny (16384 / 2400 = ~7 samples at
    // 1 kHz). A former 16-sample minimum meant MUSIC never ran at 2 kHz / 1 kHz.
    if (!processing_enabled || decimated_data.min_samples == 0) {
        return false;
    }

    // Hold config_mutex_ for the whole processing pass so config setters
    // can't resize steering vectors / pseudospectrum out from under us.
    lock_guard<mutex> config_lock(config_mutex_);

    // Follow the server's live element count (heimdall can be reconfigured at
    // runtime); resizes all per-element state when it changed.
    syncElementCount();
    // ✈ the array calibration in use changed (applied / switched / removed)
    if (cal_gen_ != array_cal::generation()) {
        if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) updateSteeringVectors2D();
        else updateSteeringVectors();
    }

    if (decimated_data.num_channels < static_cast<size_t>(num_elements_)) {
        return false;
    }

    // Only ever build a frame from contiguous samples (see MAX_INPUT_GAP):
    // leftover pre-gap samples used to fill up to ~97% of the first frame
    // after a retune or a squelch opening (at narrow bandwidths).
    const auto now = std::chrono::steady_clock::now();
    // A VFO offset change is a discontinuity too: samples mixed down at two
    // offsets must never share a frame.
    if (last_input_time_ != std::chrono::steady_clock::time_point{} &&
        (now - last_input_time_ > MAX_INPUT_GAP ||
         decimated_data.output_rate_hz != last_input_rate_hz_ ||
         std::abs(decimated_data.freq_offset_hz - last_input_offset_hz_) > 0.5f)) {
        clearAccumulatorLocked();
    }
    last_input_time_ = now;
    last_input_rate_hz_ = decimated_data.output_rate_hz;
    last_input_offset_hz_ = decimated_data.freq_offset_hz;

    // Copy into the accumulator (input is only read)
    addToAccumulatorOptimized(decimated_data);
    
    // Try to extract snapshots and process
    bool processed = false;
    if (extractSnapshotsOptimized()) {
        processed = processSnapshotCollectionOptimized();
    }

    // Stamp each newly published frame so the DoA logger can dedup and pace
    // itself to the real data rate (see getResultStampMs()). A held frame
    // (setPublishHold) is computed but NOT published, so it is not stamped -
    // downstream consumers keep holding the last published frame.
    if (processed && frame_published_) {
        result_stale_.store(false, std::memory_order_relaxed);
        result_stamp_ms_.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count(),
            std::memory_order_relaxed);
    }

    // Update statistics (reduced frequency)
    if (processed && ++stats_update_counter_ % 16 == 0) { // Update stats every 16th successful call
        lock_guard<mutex> lock(stats_mutex_);
        stats_.blocks_processed++;
        
        // Safe access to accumulator stats
        {
            lock_guard<mutex> acc_lock(accumulator_.buffer_mutex);
            stats_.buffer_fill_ratio = static_cast<double>(accumulator_.samples_available) / 
                                  accumulator_.buffer_size;
        }
    }
    
    return processed;
}

// Rvalue overload: delegates - the input is only read, never consumed.
// (The old version of this pair deep-copied the entire multichannel block
// on every call from the const& side.)
bool MUSICProcessor::processDecimatedIQ(SharedDecimator::MultiChannelDecimated&& decimated_data) {
    return processDecimatedIQ(static_cast<const SharedDecimator::MultiChannelDecimated&>(decimated_data));
}


void MUSICProcessor::addToAccumulatorOptimized(const SharedDecimator::MultiChannelDecimated& decimated_data) {
    lock_guard<mutex> lock(accumulator_.buffer_mutex);
    
    if (accumulator_.buffer.rows() != num_elements_ || accumulator_.buffer.cols() == 0) {
        cout << "ERROR: Accumulator buffer not properly initialized" << endl;
        return;
    }
    
    size_t new_sample_count = decimated_data.min_samples;
    
    // Limit to buffer size
    new_sample_count = min(new_sample_count, accumulator_.buffer_size);
    
    // OPTIMIZATION: Use bit masking instead of modulo for power-of-2 buffer
    size_t write_idx = accumulator_.write_index;
    const size_t mask = accumulator_.buffer_size_mask;
    
    // Process samples in two chunks to handle wrap-around efficiently
    size_t chunk1_size = min(new_sample_count, accumulator_.buffer_size - write_idx);
    size_t chunk2_size = new_sample_count - chunk1_size;
    
    // First chunk (no wrap-around) - use move semantics where possible
    for (int ch = 0; ch < num_elements_; ch++) {
        auto& channel = decimated_data.channels[ch];  // Non-const reference since we own the data
        const std::complex<float>* src_ptr = channel.samples.data();
        
        for (size_t i = 0; i < chunk1_size && i < channel.samples.size(); i++) {
            accumulator_.buffer(ch, write_idx + i) = src_ptr[i];
        }
    }
    
    // Second chunk (wrapped around to beginning)
    if (chunk2_size > 0) {
        for (int ch = 0; ch < num_elements_; ch++) {
            auto& channel = decimated_data.channels[ch];
            if (chunk1_size < channel.samples.size()) {
                const std::complex<float>* src_ptr = channel.samples.data() + chunk1_size;
                
                size_t copy_size = min(chunk2_size, channel.samples.size() - chunk1_size);
                for (size_t i = 0; i < copy_size; i++) {
                    accumulator_.buffer(ch, i) = src_ptr[i];
                }
            }
        }
    }
    
    // stream positions of these samples (talker DoA frame tap)
    acc_pos_.push_back({accumulator_.global_sample_index, decimated_data.stream_pos, new_sample_count});

    // Update indices using bit masking
    accumulator_.write_index = (write_idx + new_sample_count) & mask;
    accumulator_.samples_available = min(accumulator_.samples_available + new_sample_count, accumulator_.buffer_size);
    accumulator_.total_samples += new_sample_count;
    accumulator_.global_sample_index += new_sample_count;

    const size_t oldest = accumulator_.global_sample_index - accumulator_.samples_available;
    while (acc_pos_.size() > 1 && acc_pos_.front().g0 + acc_pos_.front().n <= oldest) acc_pos_.pop_front();
}

uint64_t MUSICProcessor::streamPosOf(size_t g) const {
    for (auto it = acc_pos_.rbegin(); it != acc_pos_.rend(); ++it)
        if (it->g0 <= g) return it->pos + min(g - it->g0, it->n > 0 ? it->n - 1 : 0);
    return acc_pos_.empty() ? 0 : acc_pos_.front().pos;
}

bool MUSICProcessor::extractSnapshotsOptimized() {
    lock_guard<mutex> lock(accumulator_.buffer_mutex);

    // Effective snapshot length. At narrow bandwidths even MIN_NARROW_SNAPSHOTS
    // configured-length snapshots can exceed the frame cap (4 x 2048 at 1 kHz
    // = 6.6 s per bearing): shorten them so MIN_NARROW_SNAPSHOTS fit in
    // MAX_FRAME_SECONDS (>= 64 samples). The covariance averages over samples,
    // so the snapshot length is only the framing unit. Overlap scales along.
    size_t snap_len = config_.snapshot_length;
    size_t overlap = config_.overlap_samples;
    if (last_input_rate_hz_ > 0.0f && snap_len > 0) {
        const double window = static_cast<double>(last_input_rate_hz_) * MAX_FRAME_SECONDS;
        const size_t fit_len = max<size_t>(64, static_cast<size_t>(window / MIN_NARROW_SNAPSHOTS));
        if (fit_len < snap_len) {
            overlap = overlap * fit_len / snap_len;
            snap_len = fit_len;
        }
    }
    eff_snapshot_len_ = snap_len;

    size_t step_size = (snap_len > overlap) ? (snap_len - overlap) : snap_len;

    // Snapshots per frame. At narrow bandwidths the configured minimum
    // (default 16 x 256 with 64 overlap = 3136 samples) takes seconds to
    // collect - 1.6 s at 2 kHz, 3.1 s at 1 kHz - so bearings would crawl. Cap
    // the frame at MAX_FRAME_SECONDS of samples, but never below
    // MIN_NARROW_SNAPSHOTS (with the length above, >= 4 x 64 samples, still
    // far more than the element count for the covariance). Wider bandwidths
    // are unaffected.
    size_t min_snaps = config_.min_snapshots;
    if (last_input_rate_hz_ > 0.0f && snap_len > 0) {
        const double window = static_cast<double>(last_input_rate_hz_) * MAX_FRAME_SECONDS;
        size_t fit = (window > snap_len)
            ? static_cast<size_t>((window - snap_len) / step_size) + 1 : 1;
        fit = max(fit, MIN_NARROW_SNAPSHOTS);
        min_snaps = min(min_snaps, fit);
    }

    size_t samples_needed = min_snaps * snap_len;
    if (overlap > 0 && min_snaps > 1) {
        samples_needed -= (min_snaps - 1) * overlap;
    }
    
    if (accumulator_.samples_available < samples_needed) {
        return false;
    }
    
    snapshot_mgr_.snapshots.clear();
    snapshot_mgr_.snapshots.reserve(config_.num_snapshots);

    size_t start_offset = 0;
    
    // Pre-check if working matrix needs resizing (do once, not in loop)
    if (snapshot_working_matrix_.rows() != num_elements_ || 
        snapshot_working_matrix_.cols() != static_cast<Eigen::Index>(snap_len)) {
        snapshot_working_matrix_.resize(num_elements_, snap_len);
    }
    
    for (size_t snap = 0; snap < config_.num_snapshots; snap++) {
        if (start_offset + snap_len > accumulator_.samples_available) {
            break;
        }
        
        // Use optimized snapshot extraction
        getSnapshotWindowOptimized(start_offset, snapshot_working_matrix_);
        snapshot_mgr_.snapshots.emplace_back(snapshot_working_matrix_);
        start_offset += step_size;
    }
    
    snapshot_mgr_.snapshots_ready = (snapshot_mgr_.snapshots.size() >= min_snaps);
    
    if (snapshot_mgr_.snapshots_ready && snapshot_mgr_.snapshots.size() > 0) {
        size_t consumed_samples = step_size * (snapshot_mgr_.snapshots.size() - 1) + snap_len;
        consumed_samples = min(consumed_samples, accumulator_.samples_available);
        // the frame's samples in the VFO stream (the oldest ones are consumed)
        const size_t oldest = accumulator_.global_sample_index - accumulator_.samples_available;
        frame_a_ = streamPosOf(oldest);
        frame_b_ = streamPosOf(oldest + consumed_samples - 1) + 1;
        accumulator_.samples_available -= consumed_samples;
    }
    
    return snapshot_mgr_.snapshots_ready;
}

void MUSICProcessor::getSnapshotWindowOptimized(size_t start_offset, MatrixXcd& output) {
    // OPTIMIZATION: Use bit masking and block copying to minimize per-sample operations
    
    const size_t mask = accumulator_.buffer_size_mask;
    
    // Calculate starting position using bit mask
    size_t oldest_sample_idx = (accumulator_.write_index + accumulator_.buffer_size - 
                               accumulator_.samples_available) & mask;
    size_t start_idx = (oldest_sample_idx + start_offset) & mask;
    
    // Check if we can do a contiguous copy
    size_t contiguous_samples = min(eff_snapshot_len_, 
                                   accumulator_.buffer_size - start_idx);
    
    if (contiguous_samples == eff_snapshot_len_) {
        // Fast path: All data is contiguous, copy entire block at once
        for (int ch = 0; ch < num_elements_; ch++) {
            output.row(ch) = accumulator_.buffer.block(ch, start_idx, 1, eff_snapshot_len_);
        }
    } else {
        // Slow path: Data wraps around, need two copies
        size_t wrap_samples = eff_snapshot_len_ - contiguous_samples;
        
        for (int ch = 0; ch < num_elements_; ch++) {
            // Copy first part (from start_idx to end of buffer)
            output.block(ch, 0, 1, contiguous_samples) = 
                accumulator_.buffer.block(ch, start_idx, 1, contiguous_samples);
            
            // Copy second part (from beginning of buffer)
            output.block(ch, contiguous_samples, 1, wrap_samples) = 
                accumulator_.buffer.block(ch, 0, 1, wrap_samples);
        }
    }
}



bool MUSICProcessor::processSnapshotCollectionOptimized() {
    if (!snapshot_mgr_.snapshots_ready || snapshot_mgr_.snapshots.empty()) {
        return false;
    }
    
    // Double-check we're still enabled (config might have changed)
    if (!processing_enabled) {
        return false;
    }
    
    // Start timing measurement
    //startTiming();
    
    // Calculate total samples needed and validate all snapshots
    size_t total_samples = 0;
    size_t expected_snapshot_length = eff_snapshot_len_;  // see extractSnapshotsOptimized
    
    for (const auto& snapshot : snapshot_mgr_.snapshots) {
        if (snapshot.rows() != num_elements_) {
            cout << "ERROR: Snapshot has wrong number of rows: " << snapshot.rows() 
                 << " expected: " << num_elements_ << endl;
            //endTiming();
            return false;
        }
        
        if (snapshot.cols() != static_cast<Eigen::Index>(expected_snapshot_length)) {
            cout << "ERROR: Snapshot has wrong length: " << snapshot.cols() 
                 << " expected: " << expected_snapshot_length << endl;
            //endTiming();
            return false;
        }
        
        total_samples += snapshot.cols();
    }
    
    // Ensure we have valid snapshots
    if (total_samples == 0) {
        cout << "ERROR: No valid snapshot data" << endl;
        //endTiming();
        return false;
    }
    
    // Verify combined matrix can handle the data
    if (combined_snapshots_.rows() != num_elements_ || 
        combined_snapshots_.cols() < static_cast<Eigen::Index>(total_samples)) {
        // Resize if needed
        combined_snapshots_.resize(num_elements_, total_samples);
    }
    
    // Copy snapshots to combined matrix with extra validation
    size_t col_offset = 0;
    for (const auto& snapshot : snapshot_mgr_.snapshots) {
        size_t snap_cols = snapshot.cols();
        
        // Final bounds check before block operation
        if (col_offset + snap_cols > total_samples || 
            col_offset + snap_cols > static_cast<size_t>(combined_snapshots_.cols())) {
            cout << "ERROR: Block operation would exceed bounds: " 
                 << "offset=" << col_offset << ", cols=" << snap_cols 
                 << ", total=" << total_samples 
                 << ", matrix_cols=" << combined_snapshots_.cols() << endl;
            //endTiming();
            return false;
        }
        
        // Safe block copy
        combined_snapshots_.block(0, col_offset, num_elements_, snap_cols) = snapshot;
        col_offset += snap_cols;
    }
    
    // Use only the actual data portion
    auto data_view = combined_snapshots_.leftCols(total_samples);

    // Verify data view is valid
    if (data_view.rows() != num_elements_ || data_view.cols() != static_cast<Eigen::Index>(total_samples)) {
        cout << "ERROR: Invalid data view dimensions: " << data_view.rows() << "x" << data_view.cols() << endl;
        //endTiming();
        return false;
    }

    frame_published_ = true;

    // Compute correlation matrix using optimized BLAS operations
    computeCorrelationMatrixOptimized(data_view);

    // Run MUSIC algorithm (use 2D version for 3D arrays)
    if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) {
        computeMUSICSpectrum2D();
    } else {
        computeMUSICSpectrumOptimized();
    }

    if (frame_tap_ && frame_R_.rows() == num_elements_ && frame_b_ > frame_a_) {
        frame_tap_(frame_R_, frame_a_, frame_b_, last_input_rate_hz_, static_cast<double>(current_frequency),
                   eigenvalue_ratio_.load(std::memory_order_relaxed));
    }
    
    // End timing measurement
    //endTiming();
    
    // Update statistics
    {
        lock_guard<mutex> lock(stats_mutex_);
        stats_.snapshots_processed += snapshot_mgr_.snapshots.size();
    }
    
    return true;
}

void MUSICProcessor::computeCorrelationMatrixOptimized(const MatrixXcd& X) {
    int M = X.rows();  // Number of elements (full array)
    int N = X.cols();  // Number of samples

    if (N == 0) {
        correlation_matrix_.setZero(M, M);
        return;
    }

    // Ensure correlation matrix is right size
    if (correlation_matrix_.rows() != M || correlation_matrix_.cols() != M) {
        correlation_matrix_.resize(M, M);
    }

    // Standard covariance: Use Eigen's selfadjointView with rankUpdate
    // This is specifically optimized for R = X * X^H and uses BLAS syrk/herk
    double scale = 1.0 / static_cast<double>(N);
    correlation_matrix_.setZero();
    correlation_matrix_.selfadjointView<Eigen::Lower>().rankUpdate(X, scale);

    // Copy lower triangle to upper for consistency (selfadjointView only fills lower)
    correlation_matrix_.triangularView<Eigen::StrictlyUpper>() =
        correlation_matrix_.triangularView<Eigen::StrictlyLower>().adjoint();

    // Forward-backward averaging: R_fb = (R + J*conj(R)*J)/2, where J is the
    // exchange (flip) matrix - J*conj(R)*J is conj(R) reversed in both
    // dimensions. Doubles the effective snapshot support and partially
    // decorrelates coherent multipath. Only valid for a centro-symmetric
    // array, hence restricted to ULA: on a UCA, conjugation maps arrivals to
    // their antipodes and would create 180-degree ghost peaks.
    if (fb_averaging_enabled_ && current_topology == ArrayTopology::ULA) {
        Eigen::MatrixXcd flipped = correlation_matrix_.conjugate().reverse();
        correlation_matrix_ = 0.5 * (correlation_matrix_ + flipped);
    }

    // Normalize by trace so the matrix is scale-invariant. Done before the
    // temporal average so gain/AGC level changes between frames don't skew it.
    double trace_R = correlation_matrix_.trace().real();
    if (trace_R > 1e-15) {
        correlation_matrix_ *= (1.0 / trace_R);
    }
    if (frame_tap_) frame_R_ = correlation_matrix_;   // this frame alone (talker DoA)

    // Temporal smoothing across frames: R_avg = (1-a)*R_avg + a*R_frame.
    // Reduces bearing jitter on weak/continuous signals at the cost of
    // response time. Reset whenever frequency/config/topology changes.
    if (covariance_alpha_ < 0.999f) {
        double a = static_cast<double>(covariance_alpha_);
        if (covariance_avg_valid_ &&
            covariance_avg_.rows() == M && covariance_avg_.cols() == M) {
            covariance_avg_ = (1.0 - a) * covariance_avg_ + a * correlation_matrix_;
        } else {
            covariance_avg_ = correlation_matrix_;
            covariance_avg_valid_ = true;
        }
        correlation_matrix_ = covariance_avg_;
    } else {
        covariance_avg_valid_ = false;
    }

    // Add small regularization to diagonal for numerical stability
    const double regularization = 1e-12;
    correlation_matrix_.diagonal().array() += regularization;
}

void MUSICProcessor::computeMUSICSpectrumOptimized() {
    int expected_rows = correlation_matrix_.rows();

    // Check steering vectors are valid and have correct dimensions
    if (!steering_vectors_valid || steering_vectors.rows() != expected_rows) {
        pseudospectrum.setConstant(0.0);
        return;
    }

    int M = correlation_matrix_.rows();
    if (M < 2) {
        pseudospectrum.setConstant(0.0);
        return;
    }

    // Use pre-allocated eigensolver member to avoid repeated construction
    if (!eigensolver_initialized_) {
        eigensolver_.reset(new SelfAdjointEigenSolver<MatrixXcd>(M));
        eigensolver_initialized_ = true;
    }

    // OPTIMIZATION: Compute only eigenvalues we need using the Lower triangle
    eigensolver_->compute(correlation_matrix_.selfadjointView<Eigen::Lower>());
    if (eigensolver_->info() != Success) {
        cout << "ERROR: Hermitian eigendecomposition failed" << endl;
        pseudospectrum.setConstant(0.0);
        return;
    }

    // Get eigenvectors - use const reference, no copy
    const MatrixXcd& eigenvectors = eigensolver_->eigenvectors();

    // Compute eigenvalue ratio for signal detection (squelch)
    // λ1/mean(λ2..λN) - high ratio indicates coherent signal present
    // Eigenvalues are sorted ascending, so eigenvalues(M-1) is the largest
    {
        const auto& eigenvalues = eigensolver_->eigenvalues();
        if (M > 1) {
            double lambda_max = eigenvalues(M - 1);  // Largest eigenvalue (signal)
            double noise_mean = 0.0;
            for (int i = 0; i < M - 1; i++) {
                noise_mean += eigenvalues(i);
            }
            noise_mean /= (M - 1);

            // Compute ratio, avoid division by zero
            float ratio = 1.0f;
            if (noise_mean > 1e-15) {
                ratio = static_cast<float>(lambda_max / noise_mean);
            }
            storeEigenvalueRatio(ratio);
        }
    }

    // Signal-subspace dimension (auto-estimate or the manual expectation),
    // clamped so the noise subspace keeps at least one dimension.
    int signal_dimension = resolveSignalDimension(M);
    int noise_dimension = M - signal_dimension;

    // Publish hold (FFT squelch closed with beamforming, Digital squelch):
    // freeze the published output - the ratio (stored above) keeps updating.
    // Also skips the (relatively costly) noise-subspace projection.
    if (publish_hold_.load(std::memory_order_relaxed)) {
        frame_published_ = false;
        return;
    }

    // Extract noise subspace (smallest eigenvalues are first)
    // Use leftCols() for efficient column extraction
    auto E = eigenvectors.leftCols(noise_dimension);

    // Use element-space steering vectors (M × 360)
    const MatrixXcd& S = steering_vectors;

    // OPTIMIZATION: Pre-allocated matrix with .noalias() for faster multiplication
    // Ensure EH_S_working_ is sized correctly
    if (EH_S_working_.rows() != noise_dimension || EH_S_working_.cols() != num_angles_) {
        EH_S_working_.resize(noise_dimension, num_angles_);
    }

    // Pre-compute E^H * S for all angles at once
    // Use .noalias() since we know there's no aliasing
    EH_S_working_.noalias() = E.adjoint() * S;

    // OPTIMIZATION: Vectorized pseudospectrum computation (ARM NEON)
    // Two-step computation to avoid expression template overhead
    pseudospectrum.noalias() = EH_S_working_.colwise().squaredNorm();

    // Apply threshold and inversion using array operations
    const double threshold = 1e-15;
    pseudospectrum = pseudospectrum.cwiseMax(threshold).cwiseInverse();

    // ULA front/back truncation and array orientation offset
    applyOutputTransforms();
}


void MUSICProcessor::resetAccumulator() {
    // config_mutex_ too: snapshot_mgr_ is used by the processing pass under it
    lock_guard<mutex> config_lock(config_mutex_);
    clearAccumulatorLocked();
}

void MUSICProcessor::clearAccumulatorLocked() {
    lock_guard<mutex> lock(accumulator_.buffer_mutex);
    accumulator_.write_index = 0;
    accumulator_.samples_available = 0;
    acc_pos_.clear();
    snapshot_mgr_.snapshots.clear();
    snapshot_mgr_.snapshots_ready = false;
}

size_t MUSICProcessor::getTotalSamplesProcessed() const {
    lock_guard<mutex> lock(accumulator_.buffer_mutex);
    return accumulator_.global_sample_index;
}

MUSICProcessor::ProcessingStats MUSICProcessor::getStats() const {
    lock_guard<mutex> lock(stats_mutex_);
    return stats_;
}

VectorXd MUSICProcessor::getPseudospectrum() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return pseudospectrum;
}

float MUSICProcessor::interpolatePeakAngle(Eigen::Index max_idx, double max_val) const {
    return interpolatePeakAngleOf(pseudospectrum, max_idx, max_val, offset_residual_deg_);
}

float MUSICProcessor::interpolatePeakAngleOf(const Eigen::VectorXd& pseudospectrum, Eigen::Index max_idx,
                                             double max_val, float residual) const {
    // 3-point parabolic (quadratic) vertex fit on the LOG pseudospectrum: the
    // MUSIC peak is far closer to quadratic in ln(P) than in P, so the log fit
    // recovers the vertex well below grid resolution. delta is the fractional
    // bin offset of the vertex, guaranteed in [-0.5, 0.5] when max_idx is a
    // true local maximum.
    const int n = num_angles_;
    double delta = 0.0;
    if (n >= 3 && static_cast<int>(pseudospectrum.size()) == n && max_val > 0.0) {
        int i = static_cast<int>(max_idx);
        double pl = pseudospectrum((i - 1 + n) % n);
        double pr = pseudospectrum((i + 1) % n);
        // Skip when a neighbor is non-positive (peak sits at a ULA half-plane
        // mask edge) - fall back to the grid-centered angle.
        if (pl > 0.0 && pr > 0.0) {
            double y1 = log(pl);
            double y2 = log(max_val);
            double y3 = log(pr);
            double denom = y1 - 2.0 * y2 + y3;
            if (denom < -1e-12) {
                delta = 0.5 * (y1 - y3) / denom;
                delta = max(-0.5, min(0.5, delta));
            }
        }
    }

    float angle = (static_cast<float>(max_idx) + static_cast<float>(delta)) * angular_resolution_ + residual;
    if (angle >= 360.0f) angle -= 360.0f;
    if (angle < 0.0f) angle += 360.0f;
    return angle;
}

float MUSICProcessor::getSpectrumArrayOffset() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return spectrum_offset_deg_;
}

float MUSICProcessor::getPeakAngle() const {
    lock_guard<mutex> config_lock(config_mutex_);
    if (pseudospectrum.size() == 0) return -1.0f;

    // Find the index of maximum value
    Eigen::Index max_idx;
    double max_val = pseudospectrum.maxCoeff(&max_idx);

    // Check if the spectrum has valid data (not all zeros)
    if (max_val <= 0) return -1.0f;

    return interpolatePeakAngle(max_idx, max_val);
}

std::pair<float, float> MUSICProcessor::getPeakAngleWithConfidence() const {
    lock_guard<mutex> config_lock(config_mutex_);
    if (pseudospectrum.size() == 0) {
        return {-1.0f, 0.0f};
    }

    // Find the index and value of maximum
    Eigen::Index max_idx;
    double max_val = pseudospectrum.maxCoeff(&max_idx);

    // Check if the spectrum has valid data
    if (max_val <= 0) {
        return {-1.0f, 0.0f};
    }

    float angle_degrees = interpolatePeakAngle(max_idx, max_val);
    return {angle_degrees, peakConfidenceOf(pseudospectrum, max_val)};
}

float MUSICProcessor::peakConfidenceOf(const Eigen::VectorXd& pseudospectrum, double max_val) {
    // Calculate confidence as peak-to-average ratio (normalized). The mean
    // covers only bins still in play: ULA forward/backward truncation zeroes
    // half the circle (applyOutputTransforms), which halved a plain mean and
    // doubled the ratio - an inflated confidence. MUSIC values are always
    // > 0, so the zeroed bins are exactly the masked ones.
    double sum_val = 0.0;
    int n_live = 0;
    for (Eigen::Index i = 0; i < pseudospectrum.size(); i++) {
        if (pseudospectrum(i) > 0.0) { sum_val += pseudospectrum(i); n_live++; }
    }
    double mean_val = n_live > 0 ? sum_val / n_live : 0.0;
    float confidence = 0.0f;

    if (mean_val > 0) {
        // Confidence is how much the peak stands out from the average
        // Normalize to 0-1 range using a realistic maximum ratio
        double ratio = max_val / mean_val;

        // Real-world peak/mean ratios with multipath, finite snapshots, and practical
        // calibration typically range from 1.0 (noise) to 3-5 (strong directional signal).
        // The divisor of 3.0 gives:
        //   ratio 1.0 → 0%     (pure noise, no directional signal)
        //   ratio 1.5 → 17%    (weak signal)
        //   ratio 2.0 → 33%    (moderate signal)
        //   ratio 3.0 → 67%    (strong signal)
        //   ratio 4.0 → 100%   (very strong, clean signal)
        confidence = static_cast<float>(std::min(1.0, (ratio - 1.0) / 3.0));
    }

    return confidence;
}

void MUSICProcessor::setArrayTopology(ArrayTopology topology) {
    lock_guard<mutex> config_lock(config_mutex_);
    if (current_topology != topology) {
        current_topology = topology;
        eigensolver_initialized_ = false;
        steering_vectors_valid = false;
        steering_vectors_2d_valid_ = false;
        covariance_avg_valid_ = false;  // FB averaging behavior may change
        resetAutoSourceTracking();

        if (topology == ArrayTopology::CUSTOM) {
            // Initialize custom positions to UCA if not already set
            if (!custom_positions_valid_) {
                initializeCustomPositionsToUCA();
            }

            // Use 2D or 1D steering vectors based on array geometry
            if (is_3d_array_) {
                updateSteeringVectors2D();
            } else {
                updateSteeringVectors();
            }
            cout << "Array topology changed to: CUSTOM (" << (is_3d_array_ ? "3D" : "2D") << " array)" << endl;
        } else {
            updateSteeringVectors();
            const char* name = (topology == ArrayTopology::UCA) ? "UCA" : "ULA";
            cout << "Array topology changed to: " << name << endl;
        }
    }
}


ArrayTopology MUSICProcessor::getArrayTopology() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return current_topology;
}

void MUSICProcessor::setArrayRadius(float radius_mm) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Only update if radius actually changed
    if (abs(array_radius_mm - radius_mm) > 0.01f) {  // 0.01mm tolerance
        array_radius_mm = radius_mm;
        updateSteeringVectors();
    }
}

float MUSICProcessor::getArrayRadius() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return array_radius_mm;
}

void MUSICProcessor::setElementSpacing(float spacing_mm) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Only update if spacing actually changed
    if (abs(element_spacing_mm - spacing_mm) > 0.01f) {  // 0.01mm tolerance
        element_spacing_mm = spacing_mm;
        updateSteeringVectors();
    }
}

float MUSICProcessor::getElementSpacing() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return element_spacing_mm;
}

void MUSICProcessor::setFrequencyWithOffset(float base_freq_hz, float offset_hz) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Calculate effective frequency after frequency translation
    float effective_freq = base_freq_hz + offset_hz;
    
    // Only update if frequency actually changed significantly
    if (abs(current_frequency - effective_freq) > 1.0f) {  // 1 Hz tolerance
        float old_freq = current_frequency;
        current_frequency = effective_freq;
        covariance_avg_valid_ = false;  // retune invalidates the temporal average
        clearAccumulatorLocked();       // ...and the not-yet-processed samples
        result_stale_.store(true, std::memory_order_relaxed);  // the last bearing is for the old frequency
        resetAutoSourceTracking();
        updateSteeringVectors();
        // A 3D custom array also keeps an azimuth x elevation set computed at
        // the wavelength: without this a retune left 3D bearings and
        // elevations computed for the old frequency.
        if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) updateSteeringVectors2D();
        
        // Log the change with details
        cout << "MUSIC steering vectors updated:" << endl;
        cout << "  Base frequency: " << (base_freq_hz/1e6) << " MHz" << endl;
        cout << "  Frequency offset: " << (offset_hz/1000.0f) << " kHz" << endl;
        cout << "  Effective frequency: " << (effective_freq/1e6) << " MHz" << endl;
        cout << "  Previous frequency: " << (old_freq/1e6) << " MHz" << endl;
        cout << "  Topology: " << (current_topology == ArrayTopology::UCA ? "UCA" : "ULA") << endl;
        
        if (current_topology == ArrayTopology::ULA) {
            // For ULA, wavelength matters
            float old_wavelength = 3e8 / old_freq;
            float new_wavelength = 3e8 / effective_freq;
            cout << "  Wavelength change: " << (old_wavelength*1000) << "mm -> " 
                 << (new_wavelength*1000) << "mm" << endl;
        }
    }
}

void MUSICProcessor::setFrequency(float freq_hz) {
    lock_guard<mutex> config_lock(config_mutex_);
    // This sets the frequency without any offset consideration
    // Only update if frequency actually changed
    if (abs(current_frequency - freq_hz) > 1.0f) {  // 1 Hz tolerance
        current_frequency = freq_hz;
        covariance_avg_valid_ = false;  // retune invalidates the temporal average
        clearAccumulatorLocked();       // ...and the not-yet-processed samples
        result_stale_.store(true, std::memory_order_relaxed);  // the last bearing is for the old frequency
        resetAutoSourceTracking();
        updateSteeringVectors();
        // A 3D custom array also keeps an azimuth x elevation set computed at
        // the wavelength: without this a retune left 3D bearings and
        // elevations computed for the old frequency.
        if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) updateSteeringVectors2D();
        cout << "MUSIC frequency set to " << (freq_hz/1e6) << " MHz (no offset)" << endl;
    }
}

void MUSICProcessor::setFBAveragingEnabled(bool enabled) {
    lock_guard<mutex> config_lock(config_mutex_);
    if (fb_averaging_enabled_ != enabled) {
        fb_averaging_enabled_ = enabled;
        covariance_avg_valid_ = false;  // statistics change shape
        cout << "MUSIC forward-backward averaging " << (enabled ? "enabled" : "disabled")
             << " (applied only for ULA topology)" << endl;
    }
}

bool MUSICProcessor::isFBAveragingEnabled() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return fb_averaging_enabled_;
}

void MUSICProcessor::setCovarianceAveragingAlpha(float alpha) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Clamp: alpha is the new-frame weight; below 0.05 the response gets
    // uselessly sluggish, 1.0 disables averaging entirely.
    alpha = max(0.05f, min(1.0f, alpha));
    if (abs(covariance_alpha_ - alpha) > 0.001f) {
        covariance_alpha_ = alpha;
        covariance_avg_valid_ = false;
        cout << "MUSIC covariance averaging alpha set to " << alpha
             << (alpha >= 0.999f ? " (averaging off)" : "") << endl;
    }
}

float MUSICProcessor::getCovarianceAveragingAlpha() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return covariance_alpha_;
}

void MUSICProcessor::setAutoNumSources(bool enabled) {
    lock_guard<mutex> config_lock(config_mutex_);
    if (auto_num_sources_ != enabled) {
        auto_num_sources_ = enabled;
        resetAutoSourceTracking();
        if (!enabled) {
            estimated_num_sources_.store(-1, std::memory_order_relaxed);
        }
        cout << "MUSIC automatic source-count estimation "
             << (enabled ? "enabled" : "disabled")
             << " (eigenvalue threshold " << AUTO_SOURCES_THRESHOLD_DB << " dB)" << endl;
    }
}

bool MUSICProcessor::isAutoNumSources() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return auto_num_sources_;
}

int MUSICProcessor::getEstimatedNumSources() const {
    return estimated_num_sources_.load(std::memory_order_relaxed);
}

int MUSICProcessor::estimateNumSources(const Eigen::VectorXd& eigenvalues) const {
    // Eigenvalue-dominance threshold. Information criteria (MDL/AIC) assume
    // ideal equal-variance white noise; at N~8k samples they are statistically
    // sharp enough that the sub-dB noise-floor and residual-calibration
    // asymmetries of real tuners register as "sources", saturating the count
    // at M-1. Instead, grow the noise set from the smallest eigenvalue upward
    // (ascending order); an eigenvalue is a source only when it BOTH
    //   1. stands AUTO_SOURCES_THRESHOLD_DB above the running noise mean, AND
    //   2. sits within AUTO_SOURCES_REL_THRESHOLD_DB of the dominant
    //      eigenvalue - leakage from residual calibration error, channel
    //      dispersion across the band, and partially-decorrelated multipath
    //      scales with SIGNAL power, so at high SNR it clears any
    //      noise-relative test but stays well below the dominant eigenvalue.
    // Non-source eigenvalues join the noise set (raising the baseline).
    // Scale-invariant, so it works on the trace-normalized covariance.
    const int M = static_cast<int>(eigenvalues.size());
    if (M < 2) return 0;

    const double noise_factor = std::pow(10.0, AUTO_SOURCES_THRESHOLD_DB / 10.0);
    const double rel_factor = std::pow(10.0, -AUTO_SOURCES_REL_THRESHOLD_DB / 10.0);
    const double lambda_max = max(eigenvalues(M - 1), 1e-15);

    double noise_sum = max(eigenvalues(0), 1e-15);
    int noise_count = 1;

    for (int i = 1; i < M; i++) {
        const double noise_mean = noise_sum / noise_count;
        const double lambda = max(eigenvalues(i), 1e-15);
        if (lambda > noise_factor * noise_mean && lambda > rel_factor * lambda_max) {
            // This and every larger eigenvalue are signal
            return M - i;
        }
        noise_sum += lambda;
        noise_count++;
    }
    return 0;
}

int MUSICProcessor::resolveSignalDimension(int M) {
    if (auto_num_sources_) {
        const int k_raw = estimateNumSources(eigensolver_->eigenvalues());

        // Frame hysteresis: adopt a new count only after it has repeated for
        // AUTO_SOURCES_STABLE_FRAMES consecutive frames, so an eigenvalue
        // hovering at a threshold cannot flip the subspace every update.
        if (auto_k_current_ < 0) {
            // First estimate after a reset: adopt immediately
            auto_k_current_ = k_raw;
            auto_k_candidate_ = k_raw;
            auto_k_streak_ = 0;
        } else if (k_raw == auto_k_current_) {
            auto_k_candidate_ = k_raw;
            auto_k_streak_ = 0;
        } else if (k_raw == auto_k_candidate_) {
            if (++auto_k_streak_ >= AUTO_SOURCES_STABLE_FRAMES) {
                auto_k_current_ = k_raw;
                auto_k_streak_ = 0;
            }
        } else {
            auto_k_candidate_ = k_raw;
            auto_k_streak_ = 1;
        }

        estimated_num_sources_.store(auto_k_current_, std::memory_order_relaxed);
        // MUSIC needs at least one signal dimension to scan against, even when
        // the estimate says "no source" (the eigenvalue-ratio squelch covers
        // that case).
        return max(1, min(auto_k_current_, M - 1));
    }
    estimated_num_sources_.store(-1, std::memory_order_relaxed);
    return max(1, min(num_signal_sources_, M - 1));
}

void MUSICProcessor::enable() {
    processing_enabled = true;
}

void MUSICProcessor::disable() { 
    processing_enabled = false; 
}

bool MUSICProcessor::isEnabled() const { 
    return processing_enabled; 
}

size_t MUSICProcessor::getBlocksProcessed() const {
    lock_guard<mutex> lock(stats_mutex_);
    return stats_.blocks_processed;
}

bool MUSICProcessor::nominalPositionsLocked(double* x, double* y, double* z) const {
    const int M = num_elements_;
    if (current_topology == ArrayTopology::UCA) {
        if (M < 3) return false;
        // Array elements are wired clockwise (ANT0 on +x) - mirror the angle
        const double r = array_radius_mm / 1000.0;
        const double dir = uca_angle_sign();
        for (int elem = 0; elem < M; elem++) {
            const double elem_angle = dir * 2.0 * M_PI / M * elem;
            x[elem] = r * cos(elem_angle);
            y[elem] = r * sin(elem_angle);
            z[elem] = 0.0;
        }
        return true;
    }
    if (current_topology == ArrayTopology::CUSTOM && custom_positions_valid_) {
        for (int elem = 0; elem < M; elem++) {
            x[elem] = custom_positions_[elem].x_mm / 1000.0;
            y[elem] = custom_positions_[elem].y_mm / 1000.0;
            z[elem] = custom_positions_[elem].z_mm / 1000.0;
        }
        return true;
    }
    return false;   // ULA (or custom positions not set)
}

bool MUSICProcessor::nominalPositions(std::vector<double>* x, std::vector<double>* y, std::vector<double>* z) const {
    lock_guard<mutex> config_lock(config_mutex_);
    const int M = num_elements_;
    x->assign(M, 0.0);
    y->assign(M, 0.0);
    z->assign(M, 0.0);
    return nominalPositionsLocked(x->data(), y->data(), z->data());
}

bool MUSICProcessor::isCalibrationApplied() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return calibration_applied_;
}

void MUSICProcessor::updateSteeringVectors() {
    try {
        int M = num_elements_;

        steering_vectors.resize(M, num_angles_);

        if (M <= 0 || num_angles_ <= 0) {
            cout << "ERROR: Invalid array parameters - M=" << M << " angles=" << num_angles_ << endl;
            steering_vectors_valid = false;
            return;
        }

        // Element positions (metres) based on topology
        vector<double> x_pos(M), y_pos(M), z_unused(M), tau(M, 0.0);
        double wavelength = 3e8 / current_frequency;  // meters
        cal_gen_ = array_cal::generation();

        if (current_topology == ArrayTopology::CUSTOM && !custom_positions_valid_) {
            cout << "ERROR: Custom positions not set" << endl;
            steering_vectors_valid = false;
            return;
        }
        if (current_topology == ArrayTopology::UCA && M < 3) {
            cout << "ERROR: UCA requires at least 3 elements" << endl;
            steering_vectors_valid = false;
            return;
        }
        if (current_topology == ArrayTopology::ULA) {
            // Element 0 at origin (leftmost), subsequent elements to the right
            double d = element_spacing_mm / 1000.0;
            for (int elem = 0; elem < M; elem++) {
                x_pos[elem] = elem * d;  // 0, d, 2d, 3d, 4d, ...
                y_pos[elem] = 0.0;
            }
        } else {
            // UCA (wired clockwise, ANT0 on +x) / CUSTOM (XY only for 1D steering vectors)
            nominalPositionsLocked(x_pos.data(), y_pos.data(), z_unused.data());
        }

        // ✈ Array calibration from aircraft (array_cal.hpp): the corrected
        // geometry and each element's extra delay - physical units, so they
        // hold at this frequency too
        calibration_applied_ = false;
        if (current_topology != ArrayTopology::ULA) {
            vector<double> dx(M), dy(M);
            if (array_cal::corrections(M, x_pos.data(), y_pos.data(), dx.data(), dy.data(), tau.data())) {
                for (int elem = 0; elem < M; elem++) {
                    x_pos[elem] += dx[elem];
                    y_pos[elem] += dy[elem];
                }
                calibration_applied_ = true;
            }
        }
        for (int elem = 0; elem < M; elem++) {   // to wavelengths
            x_pos[elem] /= wavelength;
            y_pos[elem] /= wavelength;
        }

        // Vectorized steering vector computation
        for (int angle_idx = 0; angle_idx < num_angles_; angle_idx++) {
            double theta_rad = angle_idx * angular_resolution_ * M_PI / 180.0;
            VectorXcd steering_vector(M);

            for (int elem = 0; elem < M; elem++) {
                double phase;
                if (current_topology == ArrayTopology::ULA) {
                    // ULA: phase depends only on x position (linear array)
                    phase = 2.0 * M_PI * x_pos[elem] * sin(theta_rad);
                } else {
                    // UCA and CUSTOM: phase depends on both x and y positions (2D array);
                    // a calibrated element's cable delay lags it by 2 pi f tau
                    phase = 2.0 * M_PI * (x_pos[elem] * cos(theta_rad) + y_pos[elem] * sin(theta_rad)) -
                            2.0 * M_PI * current_frequency * tau[elem];
                }

                steering_vector(elem) = complex<double>(cos(phase), sin(phase));
            }

            double norm = steering_vector.norm();
            if (norm > 1e-15) {
                steering_vector = steering_vector / norm;
            }

            steering_vectors.col(angle_idx) = steering_vector;
        }

        steering_vectors_valid = true;

        const char* topo_name = (current_topology == ArrayTopology::UCA) ? "UCA" :
                                (current_topology == ArrayTopology::ULA) ? "ULA" : "CUSTOM";
        cout << "Steering vectors updated for " << topo_name
             << " at " << (current_frequency/1e6) << " MHz" << endl;

    } catch (const std::exception& e) {
        cout << "ERROR: Exception in updateSteeringVectors: " << e.what() << endl;
        steering_vectors_valid = false;
    } catch (...) {
        cout << "ERROR: Unknown exception in updateSteeringVectors" << endl;
        steering_vectors_valid = false;
    }
}

float MUSICProcessor::getEigenvalueRatio() const {
    return eigenvalue_ratio_.load(std::memory_order_relaxed);
}

float MUSICProcessor::getEigenvalueRatioPeak() const {
    return eigenvalue_ratio_peak_.load(std::memory_order_relaxed);
}

void MUSICProcessor::storeEigenvalueRatio(float ratio) {
    eigenvalue_ratio_.store(ratio, std::memory_order_relaxed);

    // Peak hold: keep the maximum ratio seen in the last EIGEN_PEAK_HOLD_MS so
    // sub-second bursts survive long enough for the 2 Hz status/UI to show
    // them. A new maximum re-arms the hold; an expired hold resets to current.
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float peak = eigenvalue_ratio_peak_.load(std::memory_order_relaxed);
    const int64_t stamp = eigenvalue_peak_stamp_ms_.load(std::memory_order_relaxed);
    if (ratio >= peak || now_ms - stamp > EIGEN_PEAK_HOLD_MS) {
        eigenvalue_ratio_peak_.store(ratio, std::memory_order_relaxed);
        eigenvalue_peak_stamp_ms_.store(now_ms, std::memory_order_relaxed);
    }
}

float MUSICProcessor::getAngularResolution() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return angular_resolution_;
}

int MUSICProcessor::getNumAngles() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return num_angles_;
}

void MUSICProcessor::setNumSignalSources(int n) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Store the REQUESTED count, bounded only by the compile-time ceiling;
    // estimateNumSources() limits it to the live count (M - 1) per frame.
    // Clamping to num_elements_ here lost the setting whenever it was applied
    // before the real count was known: the startup replay runs while
    // num_elements_ is still the pre-connect default (5), so a saved 6 on an
    // 8-element array became 4 for good.
    int clamped = max(1, min(n, DOA_NUM_ELEMENTS - 1));
    if (num_signal_sources_ != clamped) {
        num_signal_sources_ = clamped;
        cout << "MUSIC expected signal sources set to " << clamped << endl;
    }
}

int MUSICProcessor::getNumSignalSources() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return num_signal_sources_;
}

void MUSICProcessor::setULAOutputMode(ULAOutputMode mode) {
    lock_guard<mutex> config_lock(config_mutex_);
    if (ula_output_mode_ != mode) {
        ula_output_mode_ = mode;
        const char* name = (mode == ULAOutputMode::FORWARD)  ? "FORWARD" :
                           (mode == ULAOutputMode::BACKWARD) ? "BACKWARD" : "BOTH";
        cout << "MUSIC ULA output mode set to " << name << endl;
    }
}

ULAOutputMode MUSICProcessor::getULAOutputMode() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return ula_output_mode_;
}

void MUSICProcessor::setCustomOutputMode(ULAOutputMode mode) {
    lock_guard<mutex> config_lock(config_mutex_);
    if (custom_output_mode_ != mode) {
        custom_output_mode_ = mode;
        const char* name = (mode == ULAOutputMode::FORWARD)  ? "FORWARD" :
                           (mode == ULAOutputMode::BACKWARD) ? "BACKWARD" : "BOTH";
        cout << "MUSIC custom-array output mode set to " << name << endl;
    }
}

ULAOutputMode MUSICProcessor::getCustomOutputMode() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return custom_output_mode_;
}

ULAOutputMode MUSICProcessor::activeOutputMode() const {
    // Caller holds config_mutex_
    if (current_topology == ArrayTopology::ULA) return ula_output_mode_;
    if (current_topology == ArrayTopology::CUSTOM) return custom_output_mode_;
    return ULAOutputMode::BOTH;
}

// Forward = within +/-90deg of 0deg, i.e. angles in [0,90] U [270,360);
// backward = (90,270). Angles are in the array frame (before the offset).
static bool keepHalfPlane(ULAOutputMode mode, float angle) {
    if (mode == ULAOutputMode::BOTH) return true;
    bool forward = (angle <= 90.0f) || (angle >= 270.0f);
    return (mode == ULAOutputMode::FORWARD) ? forward : !forward;
}

void MUSICProcessor::setArrayOffset(float degrees) {
    lock_guard<mutex> config_lock(config_mutex_);
    // Normalize to [0, 360)
    degrees = wrap_degrees(degrees);
    if (abs(array_offset_deg_ - degrees) > 0.001f) {
        array_offset_deg_ = degrees;
        cout << "MUSIC array offset angle set to " << degrees << " degrees" << endl;
    }
}

float MUSICProcessor::getArrayOffset() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return array_offset_deg_;
}

void MUSICProcessor::applyOutputTransforms() {
    // Caller holds config_mutex_. Operates on the final azimuth `pseudospectrum`
    // (length num_angles_).
    if (static_cast<int>(pseudospectrum.size()) != num_angles_ || num_angles_ <= 0) {
        return;
    }
    applyOutputTransformsTo(pseudospectrum, &offset_residual_deg_);
    spectrum_offset_deg_ = array_offset_deg_;  // what this spectrum's angles include
}

void MUSICProcessor::applyOutputTransformsTo(Eigen::VectorXd& pseudospectrum, float* residual) const {
    *residual = 0.0f;
    if (static_cast<int>(pseudospectrum.size()) != num_angles_ || num_angles_ <= 0) return;

    // --- Forward/backward truncation (ULA, CUSTOM) ---
    // A ULA's response is mirror-symmetric about its axis (the 90deg-270deg
    // line): s(theta) == s(180 - theta); so is a custom array whose elements
    // all share one x (an upright panel facing forward). Zeroing one
    // half-plane resolves the front/back ambiguity.
    const ULAOutputMode half = activeOutputMode();
    if (half != ULAOutputMode::BOTH) {
        for (int i = 0; i < num_angles_; i++) {
            if (!keepHalfPlane(half, i * angular_resolution_)) pseudospectrum(i) = 0.0;
        }
    }

    // --- Array orientation offset ---
    // Rotate the spectrum so reported angle = array angle + offset (mod 360):
    // rotated(i) = raw(i - shift). Done after masking so the kept ULA lobe and
    // its mask rotate together with the physical array. The rotation is by
    // whole grid bins; the remaining fraction is added to the interpolated
    // peak (interpolatePeakAngle) - it used to be dropped, biasing every
    // bearing by up to half a bin (0.5 deg) against the offset the beamformer
    // applies in full.
    if (array_offset_deg_ != 0.0f) {
        int shift = static_cast<int>(lround(array_offset_deg_ / angular_resolution_));
        *residual = array_offset_deg_ - static_cast<float>(shift) * angular_resolution_;
        shift = ((shift % num_angles_) + num_angles_) % num_angles_;
        if (shift != 0) {
            Eigen::VectorXd rotated(num_angles_);
            for (int i = 0; i < num_angles_; i++) {
                int j = ((i - shift) % num_angles_ + num_angles_) % num_angles_;
                rotated(i) = pseudospectrum(j);
            }
            pseudospectrum.swap(rotated);
        }
    }
}

// ============================================================================
// Custom Array Positions and 2D MUSIC Implementation
// ============================================================================

void MUSICProcessor::initializeCustomPositionsToUCA() {
    custom_positions_given_ = 0;  // all defaults
    fillDefaultCustomPositions();
    custom_positions_valid_ = true;
    is_3d_array_ = false;
}

// Default for every element the user didn't place: the same 50 mm UCA over the
// live element count that the web UI's positions table shows for those rows
void MUSICProcessor::fillDefaultCustomPositions() {
    const float radius_mm = 50.0f;
    const int M = num_elements_;

    // Array elements are wired clockwise (ANT0 on +x) - mirror the angle
    const double dir = uca_angle_sign();
    for (int elem = custom_positions_given_; elem < M; elem++) {
        double angle = dir * 2.0 * M_PI / M * elem;
        custom_positions_[elem].x_mm = static_cast<float>(radius_mm * cos(angle));
        custom_positions_[elem].y_mm = static_cast<float>(radius_mm * sin(angle));
        custom_positions_[elem].z_mm = 0.0f;
    }
}

void MUSICProcessor::setCustomPositions(const std::array<ElementPosition, DOA_NUM_ELEMENTS>& positions,
                                        int count) {
    lock_guard<mutex> config_lock(config_mutex_);

    custom_positions_ = positions;
    custom_positions_given_ = std::clamp(count, 0, static_cast<int>(DOA_NUM_ELEMENTS));
    fillDefaultCustomPositions();
    custom_positions_valid_ = true;
    detect3DArray();

    // Invalidate steering vectors to force regeneration
    steering_vectors_valid = false;
    steering_vectors_2d_valid_ = false;

    // Regenerate steering vectors
    if (current_topology == ArrayTopology::CUSTOM) {
        if (is_3d_array_) {
            updateSteeringVectors2D();
        } else {
            updateSteeringVectors();
        }
    }

    cout << "Custom element positions set (" << (is_3d_array_ ? "3D array" : "2D array") << ")" << endl;
}

// Caller holds config_mutex_. 3D = any ACTIVE element has a non-zero Z.
// Re-run whenever the element count changes (syncElementCount): positions are
// usually restored at startup while num_elements_ is still the pre-connect
// default, so an array whose height differences are in elements 5-7 was
// otherwise classified 2D for good.
void MUSICProcessor::detect3DArray() {
    is_3d_array_ = false;
    if (!custom_positions_valid_) return;
    for (int i = 0; i < num_elements_; i++) {
        if (std::abs(custom_positions_[i].z_mm) > 0.01f) {
            is_3d_array_ = true;
            break;
        }
    }
}

int MUSICProcessor::getCustomPositionsCount() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return custom_positions_given_;
}

std::array<ElementPosition, DOA_NUM_ELEMENTS> MUSICProcessor::getCustomPositions() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return custom_positions_;
}

Eigen::VectorXd MUSICProcessor::getElevationPseudospectrum() const {
    lock_guard<mutex> config_lock(config_mutex_);
    return elevation_pseudospectrum_;
}

std::pair<int, int> MUSICProcessor::getPeakAzimuthElevation() const {
    lock_guard<mutex> config_lock(config_mutex_);
    // Convert peak indices to degrees (wrap azimuth after rounding)
    int az_deg = static_cast<int>(peak_azimuth_ * angular_resolution_ + 0.5f) % 360;
    // lround, not (int)(x + 0.5): that truncates toward zero, so every
    // negative elevation came out 1 degree too high (-10.0 -> -9)
    int el_deg = static_cast<int>(std::lround(peak_elevation_ * elevation_resolution_ - 90.0f));
    return {az_deg, el_deg};
}

void MUSICProcessor::setElevationResolution(float resolution_degrees) {
    lock_guard<mutex> config_lock(config_mutex_);

    // Clamp to valid range [0.5, 5.0]
    resolution_degrees = max(0.5f, min(5.0f, resolution_degrees));

    if (abs(elevation_resolution_ - resolution_degrees) > 0.01f) {
        elevation_resolution_ = resolution_degrees;
        // -90 .. +90: floor, not round - rounding up put the top step past
        // +90 deg for resolutions that don't divide 180 (1.9 deg -> +90.5)
        num_elevation_angles_ = static_cast<int>(std::floor(180.0f / resolution_degrees + 1e-4f)) + 1;

        // Resize elevation spectrum
        elevation_pseudospectrum_.resize(num_elevation_angles_);
        elevation_pseudospectrum_.setZero();

        // Mark 2D steering vectors as invalid
        steering_vectors_2d_valid_ = false;

        // Regenerate if in CUSTOM 3D mode
        if (current_topology == ArrayTopology::CUSTOM && is_3d_array_) {
            updateSteeringVectors2D();
        }

        cout << "MUSIC elevation resolution set to " << resolution_degrees << " degrees ("
             << num_elevation_angles_ << " elevation angles)" << endl;
    }
}

void MUSICProcessor::updateSteeringVectors2D() {
    try {
        int M = num_elements_;

        if (!custom_positions_valid_) {
            cout << "ERROR: Cannot compute 2D steering vectors - custom positions not set" << endl;
            steering_vectors_2d_valid_ = false;
            return;
        }

        double wavelength = 3e8 / current_frequency;  // meters
        double k = 2.0 * M_PI / wavelength;

        // Verify 3D array status
        is_3d_array_ = false;
        for (int i = 0; i < M; i++) {
            if (std::abs(custom_positions_[i].z_mm) > 0.01f) {
                is_3d_array_ = true;
                break;
            }
        }

        if (!is_3d_array_) {
            // Fall back to 1D azimuth-only steering vectors
            cout << "No Z variation detected - using 1D MUSIC" << endl;
            updateSteeringVectors();
            return;
        }

        // Compute 2D steering vectors
        int total_angles = num_angles_ * num_elevation_angles_;
        steering_vectors_2d_.resize(M, total_angles);

        cout << "Computing 2D steering vectors: " << num_angles_ << " azimuth x "
             << num_elevation_angles_ << " elevation = " << total_angles << " total" << endl;

        // positions (metres) + the array calibration's corrections and delays
        vector<double> px(M), py(M), pz(M), dx(M, 0.0), dy(M, 0.0), tau(M, 0.0);
        nominalPositionsLocked(px.data(), py.data(), pz.data());
        cal_gen_ = array_cal::generation();
        if (array_cal::corrections(M, px.data(), py.data(), dx.data(), dy.data(), tau.data())) {
            for (int i = 0; i < M; i++) {
                px[i] += dx[i];
                py[i] += dy[i];
            }
        }

        for (int az_idx = 0; az_idx < num_angles_; az_idx++) {
            double theta_rad = az_idx * angular_resolution_ * M_PI / 180.0;
            double cos_theta = cos(theta_rad);
            double sin_theta = sin(theta_rad);

            for (int el_idx = 0; el_idx < num_elevation_angles_; el_idx++) {
                // Elevation: -90 to +90 degrees
                double phi_rad = (el_idx * elevation_resolution_ - 90.0) * M_PI / 180.0;
                double cos_phi = cos(phi_rad);
                double sin_phi = sin(phi_rad);

                int col_idx = az_idx * num_elevation_angles_ + el_idx;
                VectorXcd sv(M);

                for (int elem = 0; elem < M; elem++) {
                    const double x = px[elem], y = py[elem], z = pz[elem];

                    // 3D steering vector phase
                    // s(θ,φ) = exp(j*k*(x*cos(θ)*cos(φ) + y*sin(θ)*cos(φ) + z*sin(φ)))
                    // (- 2 pi f tau: a calibrated element's cable delay)
                    double phase = k * (x * cos_theta * cos_phi +
                                       y * sin_theta * cos_phi +
                                       z * sin_phi) - 2.0 * M_PI * current_frequency * tau[elem];

                    sv(elem) = complex<double>(cos(phase), sin(phase));
                }

                // Normalize steering vector
                double norm = sv.norm();
                if (norm > 1e-15) {
                    sv /= norm;
                }

                steering_vectors_2d_.col(col_idx) = sv;
            }
        }

        steering_vectors_2d_valid_ = true;

        // Also update 1D steering vectors for compatibility (using XY positions only)
        updateSteeringVectors();

        // Resize spectrum vectors
        azimuth_pseudospectrum_.resize(num_angles_);
        azimuth_pseudospectrum_.setZero();
        elevation_pseudospectrum_.resize(num_elevation_angles_);
        elevation_pseudospectrum_.setZero();

        cout << "2D steering vectors computed successfully for 3D array at "
             << (current_frequency/1e6) << " MHz" << endl;

    } catch (const std::exception& e) {
        cout << "ERROR: Exception in updateSteeringVectors2D: " << e.what() << endl;
        steering_vectors_2d_valid_ = false;
    }
}

void MUSICProcessor::computeMUSICSpectrum2D() {
    if (!is_3d_array_ || !steering_vectors_2d_valid_) {
        // Fall back to 1D
        computeMUSICSpectrumOptimized();
        return;
    }

    int M = correlation_matrix_.rows();
    if (M < 2) {
        pseudospectrum.setConstant(0.0);
        azimuth_pseudospectrum_.setZero();
        elevation_pseudospectrum_.setZero();
        return;
    }

    // Use pre-allocated eigensolver
    if (!eigensolver_initialized_) {
        eigensolver_.reset(new SelfAdjointEigenSolver<MatrixXcd>(M));
        eigensolver_initialized_ = true;
    }

    eigensolver_->compute(correlation_matrix_.selfadjointView<Eigen::Lower>());
    if (eigensolver_->info() != Success) {
        cout << "ERROR: Eigendecomposition failed in 2D MUSIC" << endl;
        pseudospectrum.setConstant(0.0);
        return;
    }

    const MatrixXcd& eigenvectors = eigensolver_->eigenvectors();

    // Compute eigenvalue ratio (same as 1D)
    {
        const auto& eigenvalues = eigensolver_->eigenvalues();
        if (M > 1) {
            double lambda_max = eigenvalues(M - 1);
            double noise_mean = 0.0;
            for (int i = 0; i < M - 1; i++) {
                noise_mean += eigenvalues(i);
            }
            noise_mean /= (M - 1);

            float ratio = 1.0f;
            if (noise_mean > 1e-15) {
                ratio = static_cast<float>(lambda_max / noise_mean);
            }
            storeEigenvalueRatio(ratio);
        }
    }

    // Signal-subspace dimension (auto-estimate or the manual expectation)
    int signal_dimension = resolveSignalDimension(M);
    int noise_dimension = M - signal_dimension;

    // Publish hold (see the 1D path): freeze azimuth+elevation spectra and
    // the 2D peak while held.
    if (publish_hold_.load(std::memory_order_relaxed)) {
        frame_published_ = false;
        return;
    }

    // Extract noise subspace
    auto E = eigenvectors.leftCols(noise_dimension);

    // Compute 2D pseudospectrum
    int total_angles = num_angles_ * num_elevation_angles_;
    VectorXd spectrum_2d(total_angles);

    // E^H * S_2d for all 2D angles
    MatrixXcd EH_S_2d = E.adjoint() * steering_vectors_2d_;
    spectrum_2d = EH_S_2d.colwise().squaredNorm();

    // Apply threshold and inversion
    const double threshold = 1e-15;
    spectrum_2d = spectrum_2d.cwiseMax(threshold).cwiseInverse();

    // Half-plane truncation BEFORE the 2D peak search: the 1D mask in
    // applyOutputTransforms() comes after the peak/elevation are picked, so
    // an upright panel's mirror (rear) peak could still win here
    const ULAOutputMode half = custom_output_mode_;
    if (half != ULAOutputMode::BOTH) {
        for (int az_idx = 0; az_idx < num_angles_; az_idx++) {
            if (keepHalfPlane(half, az_idx * angular_resolution_)) continue;
            spectrum_2d.segment(az_idx * num_elevation_angles_, num_elevation_angles_).setZero();
        }
    }

    // Find 2D peak
    Eigen::Index max_idx;
    spectrum_2d.maxCoeff(&max_idx);

    peak_azimuth_ = static_cast<int>(max_idx / num_elevation_angles_);
    peak_elevation_ = static_cast<int>(max_idx % num_elevation_angles_);

    // Marginalize to get 1D spectrums
    marginalizeSpectrums(spectrum_2d);

    // Update compatibility pseudospectrum with azimuth spectrum
    pseudospectrum = azimuth_pseudospectrum_;

    // Array orientation offset (ULA truncation is a no-op for 3D custom arrays)
    applyOutputTransforms();
}

void MUSICProcessor::marginalizeSpectrums(const Eigen::VectorXd& spectrum_2d) {
    // Collapse the 2D spectrum with a MAX projection, not a mean: MUSIC peaks
    // are needle-sharp, so averaging over the other axis buries the peak under
    // the floor of all its off-peak cells (and can shift the reported bearing,
    // since getPeakAngle() reads the collapsed azimuth spectrum). With the max
    // projection each axis's argmax coincides exactly with the true 2D peak.

    // Max over elevation to get azimuth spectrum
    azimuth_pseudospectrum_.resize(num_angles_);
    for (int az_idx = 0; az_idx < num_angles_; az_idx++) {
        double best = 0.0;
        for (int el_idx = 0; el_idx < num_elevation_angles_; el_idx++) {
            int col_idx = az_idx * num_elevation_angles_ + el_idx;
            best = max(best, spectrum_2d(col_idx));
        }
        azimuth_pseudospectrum_(az_idx) = best;
    }

    // Max over azimuth to get elevation spectrum
    elevation_pseudospectrum_.resize(num_elevation_angles_);
    for (int el_idx = 0; el_idx < num_elevation_angles_; el_idx++) {
        double best = 0.0;
        for (int az_idx = 0; az_idx < num_angles_; az_idx++) {
            int col_idx = az_idx * num_elevation_angles_ + el_idx;
            best = max(best, spectrum_2d(col_idx));
        }
        elevation_pseudospectrum_(el_idx) = best;
    }
}

// ============================================================================
// Talker DoA support (talker_doa.hpp)
// ============================================================================

void MUSICProcessor::setFrameTap(FrameTap tap) {
    lock_guard<mutex> config_lock(config_mutex_);
    frame_tap_ = std::move(tap);
    if (!frame_tap_) frame_R_.resize(0, 0);
}

bool MUSICProcessor::publishFromCovariance(const Eigen::MatrixXcd& R) {
    lock_guard<mutex> config_lock(config_mutex_);
    const int M = num_elements_;
    if (M < 2 || R.rows() != M || R.cols() != M || num_angles_ <= 0) return false;
    const bool use_2d = current_topology == ArrayTopology::CUSTOM && is_3d_array_ && steering_vectors_2d_valid_ &&
                        steering_vectors_2d_.rows() == M;
    if (!use_2d && (!steering_vectors_valid || steering_vectors.rows() != M)) return false;
    SelfAdjointEigenSolver<MatrixXcd> es(R);
    if (es.info() != Success) return false;
    // signal subspace as the live frames choose it (without their hysteresis)
    const int k = auto_num_sources_ ? estimateNumSources(es.eigenvalues()) : num_signal_sources_;
    const int signal_dimension = max(1, min(k, M - 1));
    const auto E = es.eigenvectors().leftCols(M - signal_dimension);
    if (use_2d) {
        VectorXd s2 = (E.adjoint() * steering_vectors_2d_).colwise().squaredNorm();
        s2 = s2.cwiseMax(1e-15).cwiseInverse();
        if (custom_output_mode_ != ULAOutputMode::BOTH)
            for (int az = 0; az < num_angles_; az++)
                if (!keepHalfPlane(custom_output_mode_, az * angular_resolution_))
                    s2.segment(az * num_elevation_angles_, num_elevation_angles_).setZero();
        Eigen::Index max_idx;
        s2.maxCoeff(&max_idx);
        peak_azimuth_ = static_cast<int>(max_idx / num_elevation_angles_);
        peak_elevation_ = static_cast<int>(max_idx % num_elevation_angles_);
        marginalizeSpectrums(s2);
        pseudospectrum = azimuth_pseudospectrum_;
    } else {
        pseudospectrum = (E.adjoint() * steering_vectors).colwise().squaredNorm();
        pseudospectrum = pseudospectrum.cwiseMax(1e-15).cwiseInverse();
    }
    applyOutputTransforms();
    result_stale_.store(false, std::memory_order_relaxed);
    result_stamp_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count(),
                           std::memory_order_relaxed);
    return true;
}

bool MUSICProcessor::spectrumFromCovariance(const Eigen::MatrixXcd& R, Eigen::VectorXd* spec, float* peak_deg,
                                            float* conf) const {
    lock_guard<mutex> config_lock(config_mutex_);
    const int M = num_elements_;
    if (M < 2 || R.rows() != M || R.cols() != M || num_angles_ <= 0) return false;
    const bool use_2d = current_topology == ArrayTopology::CUSTOM && is_3d_array_ && steering_vectors_2d_valid_ &&
                        steering_vectors_2d_.rows() == M;
    if (!use_2d && (!steering_vectors_valid || steering_vectors.rows() != M)) return false;

    SelfAdjointEigenSolver<MatrixXcd> es(R);
    if (es.info() != Success) return false;
    // signal subspace as the live frames choose it (without their hysteresis)
    const int k = auto_num_sources_ ? estimateNumSources(es.eigenvalues()) : num_signal_sources_;
    const int signal_dimension = max(1, min(k, M - 1));
    const auto E = es.eigenvectors().leftCols(M - signal_dimension);

    VectorXd out;
    if (use_2d) {
        VectorXd s2 = (E.adjoint() * steering_vectors_2d_).colwise().squaredNorm();
        s2 = s2.cwiseMax(1e-15).cwiseInverse();
        if (custom_output_mode_ != ULAOutputMode::BOTH)
            for (int az = 0; az < num_angles_; az++)
                if (!keepHalfPlane(custom_output_mode_, az * angular_resolution_))
                    s2.segment(az * num_elevation_angles_, num_elevation_angles_).setZero();
        out.resize(num_angles_);
        for (int az = 0; az < num_angles_; az++) out(az) = s2.segment(az * num_elevation_angles_, num_elevation_angles_).maxCoeff();
    } else {
        out = (E.adjoint() * steering_vectors).colwise().squaredNorm();
        out = out.cwiseMax(1e-15).cwiseInverse();
    }
    float residual = 0.0f;
    applyOutputTransformsTo(out, &residual);
    Eigen::Index imax;
    const double vmax = out.maxCoeff(&imax);
    if (!(vmax > 0.0)) return false;
    if (peak_deg) *peak_deg = interpolatePeakAngleOf(out, imax, vmax, residual);
    if (conf) *conf = peakConfidenceOf(out, vmax);
    if (spec) spec->swap(out);
    return true;
}
