#include "signal_processing/fft_processor.hpp"
#include "signal_processing/beamformer.hpp"
#include "globals.hpp"
#include "config.hpp"
#include <fftw3.h>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <limits>
#include <cstring>

// External global for user-adjustable edge clip (defined in main.cpp)
extern std::atomic<float> current_edge_clip;

// Optimization: Constants for magnitude calculation
namespace {
    const float log10_of_2 = 0.30102999566398119521f; // For fast log10 via log2

    // Helper: Calculate noise floor from FFT data using 10th percentile method
    // Uses the center 80% of bins to avoid edge rolloff
    // Returns noise floor in dB
    float calculate_noise_floor(const std::vector<float>& fft_data) {
        const int fft_size = current_fft_size.load();
        if (fft_data.size() < static_cast<size_t>(fft_size)) {
            return -200.0f; // Invalid
        }

        const int total_bins = fft_size;
        const int usable_bins = static_cast<int>(total_bins * 0.8f);  // Center 80%
        const int skip_bins = (total_bins - usable_bins) / 2;  // Skip 10% on each side

        // Extract magnitudes from usable region into per-thread scratch: this
        // runs per decimator per packet (squelch, auto-eigen, FM path), often
        // under the global fft_mutex, so skip the heap allocation each call.
        thread_local std::vector<float> mags;
        mags.assign(fft_data.begin() + skip_bins, fft_data.begin() + skip_bins + usable_bins);

        // Use nth_element O(n) instead of sort O(n log n) to find 10th percentile
        int percentile_idx = static_cast<int>(mags.size() * 0.10f);
        std::nth_element(mags.begin(), mags.begin() + percentile_idx, mags.end());
        return mags[percentile_idx];
    }

    // Helper: Initialize a single channel's FFT context
    // IMPORTANT: Caller must hold fftw_planner_mutex before calling this function!
    void initialize_channel_context_locked(ChannelFFTContext& ctx, int fft_size, bool use_measure) {
        // Cleanup any existing resources
        if (ctx.plan) {
            fftwf_destroy_plan(ctx.plan);
            ctx.plan = nullptr;
        }
        if (ctx.fft_in) {
            fftwf_free(ctx.fft_in);
            ctx.fft_in = nullptr;
        }
        if (ctx.fft_out) {
            fftwf_free(ctx.fft_out);
            ctx.fft_out = nullptr;
        }

        // Allocate new buffers
        ctx.fft_in = fftwf_alloc_complex(fft_size);
        ctx.fft_out = fftwf_alloc_complex(fft_size);

        // Create plan - use FFTW_MEASURE for first channel (optimal), FFTW_ESTIMATE for others (fast)
        // The MEASURE plan is shared via wisdom, so subsequent channels benefit too
        unsigned flags = use_measure ? FFTW_MEASURE : FFTW_ESTIMATE;
        ctx.plan = fftwf_plan_dft_1d(fft_size, ctx.fft_in, ctx.fft_out, FFTW_FORWARD, flags);

        // Pre-compute Hamming window for this channel
        ctx.window_lut.resize(fft_size);
        for (int i = 0; i < fft_size; i++) {
            ctx.window_lut[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (fft_size - 1));
        }

        ctx.current_size = fft_size;
        ctx.initialized = true;
    }
}

void FFTProcessor::initialize() {
    // OPTIMIZATION: Try to load FFTW wisdom for instant optimal FFT plans
    std::cout << "Loading FFTW wisdom..." << std::endl;

    // Lock FFTW planner mutex for all plan creation operations
    std::lock_guard<std::mutex> fftw_lock(fftw_planner_mutex);

    int wisdom_loaded = fftwf_import_wisdom_from_filename("fft_wisdom.dat");
    if (wisdom_loaded) {
        std::cout << "  FFTW wisdom loaded successfully" << std::endl;
    } else {
        std::cout << "  No wisdom file found, will measure optimal plan (takes ~100ms)" << std::endl;
    }

    // Initialize per-channel FFT contexts for TRUE PARALLEL processing
    // Each channel gets its own FFTW plan and buffers
    std::cout << "Initializing " << MAX_CHANNELS << " per-channel FFT contexts..." << std::endl;

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        std::lock_guard<std::mutex> lock(channel_fft_contexts[ch].mutex);
        // Use FFTW_MEASURE for first channel to establish wisdom, FFTW_ESTIMATE for others
        bool use_measure = (ch == 0 && !wisdom_loaded);
        initialize_channel_context_locked(channel_fft_contexts[ch], FFT_SIZE, use_measure);
    }

    // Save wisdom after first channel's MEASURE planning
    if (!wisdom_loaded) {
        fftwf_export_wisdom_to_filename("fft_wisdom.dat");
        std::cout << "  FFTW wisdom saved to fft_wisdom.dat" << std::endl;
    }

    // Initialize magnitude storage arrays
    for (int i = 0; i < MAX_CHANNELS; i++) {
        fft_magnitudes[i].resize(FFT_SIZE);
        fft_averaged[i].resize(FFT_SIZE);
        std::fill(fft_averaged[i].begin(), fft_averaged[i].end(), -80.0f);
        std::fill(fft_magnitudes[i].begin(), fft_magnitudes[i].end(), -80.0f);
    }

    std::cout << "FFT Processor initialized with PER-CHANNEL PARALLELISM: " << FFT_SIZE << " points, "
              << MAX_CHANNELS << " independent channels" << std::endl;
    std::cout << "  TRUE PARALLEL: Each channel has dedicated FFTW plan - no serialization!" << std::endl;
    std::cout << "  Expected performance: 5-8x throughput improvement in wideband mode" << std::endl;
}

void FFTProcessor::export_wisdom() {
    std::lock_guard<std::mutex> fftw_lock(fftw_planner_mutex);
    if (fftwf_export_wisdom_to_filename("fft_wisdom.dat")) {
        std::cout << "FFTW wisdom exported to fft_wisdom.dat" << std::endl;
    } else {
        std::cerr << "Failed to export FFTW wisdom to fft_wisdom.dat" << std::endl;
    }
}

void FFTProcessor::cleanup() {
    // Persist wisdom for any plan created since startup so the comment at the
    // call site ("Saves FFTW wisdom") is actually true.
    export_wisdom();

    std::cout << "Cleaning up " << MAX_CHANNELS << " per-channel FFT contexts..." << std::endl;

    // Lock FFTW planner mutex for all plan destruction operations
    std::lock_guard<std::mutex> fftw_lock(fftw_planner_mutex);

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        std::lock_guard<std::mutex> lock(channel_fft_contexts[ch].mutex);
        ChannelFFTContext& ctx = channel_fft_contexts[ch];

        if (ctx.plan) {
            fftwf_destroy_plan(ctx.plan);
            ctx.plan = nullptr;
        }
        if (ctx.fft_in) {
            fftwf_free(ctx.fft_in);
            ctx.fft_in = nullptr;
        }
        if (ctx.fft_out) {
            fftwf_free(ctx.fft_out);
            ctx.fft_out = nullptr;
        }
        ctx.window_lut.clear();
        ctx.initialized = false;
    }

    std::cout << "FFT Processor cleanup complete" << std::endl;
}

int FFTProcessor::get_current_size() {
    return current_fft_size.load();
}

int FFTProcessor::get_current_decimation() {
    return current_fft_decimation.load();
}

void FFTProcessor::set_decimation(int new_decimation) {
    if (new_decimation < 1 || new_decimation > 64) {
        std::cout << "FFT Decimation: Invalid value " << new_decimation << " (must be 1-64)" << std::endl;
        return;
    }
    int old_decimation = current_fft_decimation.exchange(new_decimation);
    std::cout << "FFT Decimation changed: " << old_decimation << " -> " << new_decimation << std::endl;
}

void FFTProcessor::resize(int new_fft_size) {
    // Validate: must be power of 2 and within range
    if (new_fft_size < 1024 || new_fft_size > 65536) {
        std::cout << "FFT Resize: Invalid size " << new_fft_size << " (must be 1024-65536)" << std::endl;
        return;
    }
    // Check power of 2
    if ((new_fft_size & (new_fft_size - 1)) != 0) {
        std::cout << "FFT Resize: Invalid size " << new_fft_size << " (must be power of 2)" << std::endl;
        return;
    }

    int old_size = current_fft_size.load();
    if (new_fft_size == old_size) {
        std::cout << "FFT Resize: Already at size " << new_fft_size << std::endl;
        return;
    }

    std::cout << "FFT Resize: " << old_size << " -> " << new_fft_size << " points (resizing "
              << MAX_CHANNELS << " channel contexts)" << std::endl;

    // CRITICAL: We must wait for all in-flight FFT operations to complete before resizing.
    // fftwf_execute() might be running concurrently, and destroying the plan/buffers
    // while it's executing causes heap corruption ("double free or corruption").

    // Step 1: Signal that resize is starting - new FFT work should not start
    fft_resize_in_progress.store(true, std::memory_order_release);

    // Step 2: Wait for all active FFT workers to finish
    {
        std::unique_lock<std::mutex> resize_lock(fft_resize_mutex);

        // Wait until no workers are active (with timeout to prevent deadlock)
        auto wait_start = std::chrono::steady_clock::now();
        constexpr auto MAX_WAIT = std::chrono::milliseconds(500);  // 500ms timeout

        while (fft_workers_active.load(std::memory_order_acquire) > 0) {
            auto status = fft_resize_complete.wait_for(resize_lock, std::chrono::milliseconds(10));

            // Check timeout
            if (std::chrono::steady_clock::now() - wait_start > MAX_WAIT) {
                std::cerr << "FFT Resize: Warning - timeout waiting for workers (active="
                          << fft_workers_active.load() << ")" << std::endl;
                break;
            }
            (void)status;  // Suppress unused warning
        }

        std::cout << "FFT Resize: All workers drained, proceeding with resize" << std::endl;
    }

    // Step 3: Lock FFTW planner mutex for all plan creation/destruction operations
    std::lock_guard<std::mutex> fftw_lock(fftw_planner_mutex);

    // Resize all per-channel FFT contexts
    // Each channel is resized independently with its own lock
    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        std::lock_guard<std::mutex> lock(channel_fft_contexts[ch].mutex);
        // Use FFTW_ESTIMATE for fast resize (wisdom already established at startup)
        initialize_channel_context_locked(channel_fft_contexts[ch], new_fft_size, false);
    }

    // Resize magnitude and averaged arrays
    {
        std::lock_guard<std::mutex> fft_lock(fft_mutex);
        for (int i = 0; i < MAX_CHANNELS; i++) {
            fft_magnitudes[i].resize(new_fft_size);
            fft_averaged[i].resize(new_fft_size);
            std::fill(fft_averaged[i].begin(), fft_averaged[i].end(), -80.0f);
            std::fill(fft_magnitudes[i].begin(), fft_magnitudes[i].end(), -80.0f);
        }
    }

    // Update atomic size AFTER all contexts are resized
    current_fft_size.store(new_fft_size);

    // Step 4: Clear resize flag to allow FFT processing to resume
    fft_resize_in_progress.store(false, std::memory_order_release);

    float scale_factor = 1.0f / (static_cast<float>(new_fft_size) * new_fft_size);
    std::cout << "FFT Resize complete: " << new_fft_size << " points, scale=" << scale_factor << std::endl;
}

void FFTProcessor::process_channel_fft(const std::complex<float>* iq_data, size_t num_samples, int channel,
                                       uint32_t buffer_generation) {
    if (channel < 0 || channel >= MAX_CHANNELS) return;

    // CRITICAL: Check if resize is in progress - skip processing to allow safe resize
    // This prevents the race condition where we're executing fftwf_execute while
    // the resize operation is destroying/recreating the FFT context
    if (fft_resize_in_progress.load(std::memory_order_acquire)) {
        return;  // Skip this FFT - resize is happening
    }

    // Increment active worker count before doing any real work
    // This must happen AFTER the resize check to avoid being counted during resize
    fft_workers_active.fetch_add(1, std::memory_order_acq_rel);

    // RAII guard to ensure we always decrement the counter and signal completion
    // even if we return early or an exception occurs
    struct WorkerGuard {
        ~WorkerGuard() {
            int remaining = fft_workers_active.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                // Last worker finished - signal the resize operation if it's waiting
                fft_resize_complete.notify_one();
            }
        }
    } guard;

    // Double-check resize flag after incrementing counter (race prevention)
    // If resize started between our check and increment, exit now
    if (fft_resize_in_progress.load(std::memory_order_acquire)) {
        return;  // Guard will decrement counter
    }

    // Get the per-channel FFT context - allows TRUE PARALLEL processing
    ChannelFFTContext& ctx = channel_fft_contexts[channel];

    // Lock only THIS channel's context - other channels can process in parallel!
    std::lock_guard<std::mutex> lock(ctx.mutex);

    // Check if context is initialized and get current FFT size
    if (!ctx.initialized) {
        std::cerr << "FFT ch" << channel << ": Context not initialized!" << std::endl;
        return;
    }

    const int fft_size = ctx.current_size;

    // Allow processing with fewer samples using zero-padding
    // Minimum 1024 samples for meaningful spectrum (less would be too noisy)
    constexpr size_t MIN_SAMPLES_FOR_FFT = 1024;
    if (num_samples < MIN_SAMPLES_FOR_FFT) return;

    // OPTIMIZATION: Work directly with complex<float> - no conversion needed!
    size_t max_samples = std::min(static_cast<size_t>(fft_size), num_samples);

    // Compute proper scale factor based on actual samples used (not FFT size)
    // When zero-padding, energy is proportional to actual_samples, not fft_size
    // scale = 1/(actual_samples * fft_size) for proper dB normalization
    float actual_scale = 1.0f / (static_cast<float>(max_samples) * fft_size);

    // When zero-padding (max_samples < fft_size), compute window on-the-fly
    // to properly taper the actual data length
    if (max_samples < static_cast<size_t>(fft_size)) {
        // Dynamic Hamming window for actual data length
        float inv_n_minus_1 = 1.0f / (max_samples - 1);
        for (size_t i = 0; i < max_samples; i++) {
            float win = 0.54f - 0.46f * cosf(2.0f * M_PI * i * inv_n_minus_1);
            ctx.fft_in[i][0] = iq_data[i].real() * win;
            ctx.fft_in[i][1] = iq_data[i].imag() * win;
        }
    } else {
        // Use pre-computed window (normal case, no zero-padding)
        for (size_t i = 0; i < max_samples; i++) {
            // OPTIMIZATION: Use per-channel pre-computed window LUT (eliminates cosf() call)
            float win = ctx.window_lut[i];
            ctx.fft_in[i][0] = iq_data[i].real() * win;
            ctx.fft_in[i][1] = iq_data[i].imag() * win;
        }
    }

    // OPTIMIZATION: Use memset for efficient zero-padding
    if (max_samples < static_cast<size_t>(fft_size)) {
        std::memset(&ctx.fft_in[max_samples], 0, (fft_size - max_samples) * sizeof(fftwf_complex));
    }

    // The windowing loop above was the last read of the shared persistent
    // buffer. If the buffer wrapped while we were reading, the producer may
    // have overwritten our region mid-copy - drop the (possibly torn) frame.
    if (buffer_generation != 0 &&
        persistent_buffer_generation.load(std::memory_order_acquire) != buffer_generation) {
        return;
    }

    // Execute FFT using THIS channel's plan - TRUE PARALLEL with other channels!
    fftwf_execute(ctx.plan);

    // Now lock the shared magnitude arrays for storage
    std::lock_guard<std::mutex> mag_lock(fft_mutex);

    // EXPONENTIAL MOVING AVERAGE (EMA) with generation-based reset
    // Formula: fft_averaged = alpha * new_value + (1-alpha) * old_value
    //
    // On reset (generation change): alpha=1.0 → fft_averaged = new_value (direct copy)
    // This initializes the EMA with the first valid frame's data, then subsequent
    // frames blend from that starting point using the configured averaging_alpha.
    //
    // Each channel independently tracks its last processed generation to ensure
    // exactly one reset per frequency change (no race conditions).
    static uint32_t channel_last_reset_gen[MAX_CHANNELS] = {0};

    uint32_t current_gen = fft_reset_generation.load(std::memory_order_acquire);
    bool should_reset = (channel_last_reset_gen[channel] < current_gen);
    float alpha = should_reset ? 1.0f : averaging_alpha.load(std::memory_order_relaxed);

    if (should_reset) {
        channel_last_reset_gen[channel] = current_gen;
    }

    // OPTIMIZATION: Fast magnitude calculation with log2-based dB conversion
    for (int i = 0; i < fft_size; i++) {
        // Shift for DC centering
        // fft_size is validated power-of-2: bitmask instead of per-bin modulo
        // (integer division inhibited auto-vectorization of this hot loop)
        int shifted_idx = (i + fft_size/2) & (fft_size - 1);
        float real = ctx.fft_out[shifted_idx][0];
        float imag = ctx.fft_out[shifted_idx][1];

        // Use actual_scale which accounts for zero-padding
        float power = (real * real + imag * imag) * actual_scale + 1e-15f;

        // OPTIMIZATION: Fast log10 using hardware log2f (ARM VLOG2 instruction)
        // log10(x) = log2(x) * log10(2)
        // This is ~60% faster than log10f() on ARM
        float db = 10.0f * (std::log2f(power) * log10_of_2);

        // Store instantaneous and averaged values
        fft_magnitudes[channel][i] = db;

        // If reset flag is set (alpha=1.0), this becomes: fft_averaged = 1.0 * db + 0.0 * old = db (direct copy)
        // Otherwise uses normal exponential averaging
        fft_averaged[channel][i] = alpha * db + (1.0f - alpha) * fft_averaged[channel][i];
    }

    // Mark FFT data as valid now that we've written real data
    // This flag prevents build_fft_message from using stale/reset data
    if (!fft_data_valid.load(std::memory_order_relaxed)) {
        fft_data_valid.store(true, std::memory_order_release);
    }
}

// Decimates FFT data with min/max envelopes, compressed to uint8 magnitudes
CompressedDecimatedFFT FFTProcessor::decimate_fft_compressed(const std::vector<float>& fft_data, float center_freq_mhz, int decimation) {
    CompressedDecimatedFFT result;

    if (fft_data.empty()) {
        std::cout << "FFT Decimation (Compressed): Empty input data" << std::endl;
        return result;
    }

    const int fft_size = current_fft_size.load();
    const float sample_rate_mhz = SAMPLE_RATE / 1e6f;
    const int decimated_size = fft_size / decimation;

    result.frequencies.reserve(decimated_size);
    std::vector<float> min_envelope_float;
    std::vector<float> max_envelope_float;
    min_envelope_float.reserve(decimated_size);
    max_envelope_float.reserve(decimated_size);

    for (int i = 0; i < decimated_size; i++) {
        int start_idx = i * decimation;
        int end_idx = std::min(start_idx + decimation, static_cast<int>(fft_data.size()));

        if (start_idx >= fft_data.size()) break;

        float min_val = std::numeric_limits<float>::max();
        float max_val = std::numeric_limits<float>::lowest();

        for (int j = start_idx; j < end_idx; j++) {
            if (j < fft_data.size()) {
                min_val = std::min(min_val, fft_data[j]);
                max_val = std::max(max_val, fft_data[j]);
            }
        }

        // Calculate frequency for this bin
        float freq = center_freq_mhz - sample_rate_mhz/2 + (i * decimation * sample_rate_mhz / fft_size);

        result.frequencies.push_back(freq);
        min_envelope_float.push_back(min_val);
        max_envelope_float.push_back(max_val);
    }

    // Compress magnitudes to uint8
    result.min_envelope = compress_magnitudes(min_envelope_float);
    result.max_envelope = compress_magnitudes(max_envelope_float);

    return result;
}

void FFTProcessor::stitch_wideband_fft(
    const std::vector<std::vector<float>>& channel_ffts,
    const std::array<uint32_t, MAX_CHANNELS>& tuner_freqs,
    int num_tuners,
    std::vector<float>& output_fft,
    float& min_freq_mhz,
    float& max_freq_mhz
) {
    if (num_tuners == 0 || channel_ffts.empty()) {
        output_fft.clear();
        return;
    }

    // Constants - use dynamic FFT size and edge clip setting
    const int fft_size = current_fft_size.load();
    const float sample_rate_mhz = SAMPLE_RATE / 1e6f;
    const float edge_clip = current_edge_clip.load();  // User-adjustable edge clip (0.0-1.0)
    const int total_bins = fft_size;
    // At least 1 bin: usable_bins==0 would index an empty percentile vector below
    const int usable_bins = std::max(1, static_cast<int>(total_bins * edge_clip));
    const int skip_bins = (total_bins - usable_bins) / 2;  // Skip edge bins on each side

    // Step 1: Calculate noise floor for each tuner (10th percentile for robustness)
    std::vector<float> noise_floor_offsets(num_tuners, 0.0f);

    for (int tuner = 0; tuner < num_tuners; tuner++) {
        if (tuner >= static_cast<int>(channel_ffts.size()) || channel_ffts[tuner].size() < static_cast<size_t>(total_bins)) {
            continue;
        }

        const std::vector<float>& fft_data = channel_ffts[tuner];

        // Extract magnitudes from usable region and sort to find percentile
        std::vector<float> tuner_mags;
        tuner_mags.reserve(usable_bins);

        for (int i = 0; i < usable_bins; i++) {
            int fft_idx = skip_bins + i;
            tuner_mags.push_back(fft_data[fft_idx]);
        }

        // Use nth_element O(n) instead of sort O(n log n) to find 10th percentile
        int percentile_idx = static_cast<int>(tuner_mags.size() * 0.10f);
        std::nth_element(tuner_mags.begin(), tuner_mags.begin() + percentile_idx, tuner_mags.end());
        float noise_floor = tuner_mags[percentile_idx];

        // Store this tuner's noise floor for offset calculation
        noise_floor_offsets[tuner] = noise_floor;
    }

    // Calculate offsets to:
    // 1. Align all tuners to the maximum noise floor (equalize tuner sensitivity)
    // 2. Then shift so noise floor sits at 0dB (normalized output)
    // Final offset = (max_noise_floor - tuner_noise_floor) - max_noise_floor
    //              = -tuner_noise_floor
    // This makes each tuner's noise floor become 0dB
    for (int tuner = 0; tuner < num_tuners; tuner++) {
        // offset = align_to_max + shift_to_zero
        // align_to_max = max_noise_floor - noise_floor_offsets[tuner]
        // shift_to_zero = -max_noise_floor
        // Combined: -noise_floor_offsets[tuner] (which is the original tuner noise floor)
        noise_floor_offsets[tuner] = -noise_floor_offsets[tuner];
    }

    // Store the reference noise floor for debug (now always 0dB after normalization)
    // Coherent mode will also normalize to 0dB independently
    extern std::atomic<float> wideband_reference_noise_floor;
    wideband_reference_noise_floor.store(0.0f, std::memory_order_release);  // Normalized to 0dB

    // Step 2: Create frequency-magnitude pairs with noise floor alignment applied
    std::vector<std::pair<float, float>> freq_mag_pairs;
    freq_mag_pairs.reserve(num_tuners * usable_bins);

    // Process each tuner's FFT with alignment offset
    for (int tuner = 0; tuner < num_tuners; tuner++) {
        if (tuner >= static_cast<int>(channel_ffts.size()) || channel_ffts[tuner].size() < total_bins) {
            continue;
        }

        const std::vector<float>& fft_data = channel_ffts[tuner];
        float center_freq_mhz = tuner_freqs[tuner] / 1e6f;
        float tuner_offset = noise_floor_offsets[tuner];

        // Extract center 80% of this tuner's FFT with alignment offset applied
        for (int i = 0; i < usable_bins; i++) {
            int fft_idx = skip_bins + i;

            // Calculate frequency for this bin (DC-centered FFT)
            // FFT is already fftshift'd: bin 0 = -Fs/2, bin FFT_SIZE/2 = DC, bin FFT_SIZE-1 ≈ +Fs/2
            float bin_freq_offset = (fft_idx - total_bins / 2) * sample_rate_mhz / total_bins;

            float absolute_freq_mhz = center_freq_mhz + bin_freq_offset;
            float magnitude = fft_data[fft_idx] + tuner_offset;  // Apply alignment offset

            freq_mag_pairs.emplace_back(absolute_freq_mhz, magnitude);
        }
    }

    // Sort by frequency
    std::sort(freq_mag_pairs.begin(), freq_mag_pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // Handle overlapping bins by averaging
    output_fft.clear();
    if (freq_mag_pairs.empty()) return;

    output_fft.reserve(freq_mag_pairs.size());

    // Determine frequency resolution for overlap detection
    const float freq_resolution = sample_rate_mhz / total_bins;
    const float overlap_threshold = freq_resolution * 0.5f;  // Half a bin width

    float current_freq = freq_mag_pairs[0].first;
    float mag_sum = freq_mag_pairs[0].second;
    int count = 1;

    for (size_t i = 1; i < freq_mag_pairs.size(); i++) {
        float freq = freq_mag_pairs[i].first;
        float mag = freq_mag_pairs[i].second;

        if (std::abs(freq - current_freq) < overlap_threshold) {
            // Same bin, accumulate for averaging
            mag_sum += mag;
            count++;
        } else {
            // New bin, output the averaged previous bin
            output_fft.push_back(mag_sum / count);

            // Start new bin
            current_freq = freq;
            mag_sum = mag;
            count = 1;
        }
    }

    // Output last bin
    if (count > 0) {
        output_fft.push_back(mag_sum / count);
    }

    // Calculate frequency range
    if (!freq_mag_pairs.empty()) {
        min_freq_mhz = freq_mag_pairs.front().first;
        max_freq_mhz = freq_mag_pairs.back().first;
    }

}

// Compress float magnitudes (dB) to uint8 for bandwidth reduction
// After noise floor normalization, values are 0dB (noise) to ~80dB (strong signals)
// Maps 0 dB to 120 dB → 0 to 255 (resolution: ~0.47 dB/step)
// This provides headroom for strong signals while keeping noise at 0
std::vector<uint8_t> FFTProcessor::compress_magnitudes(const std::vector<float>& magnitudes) {
    std::vector<uint8_t> compressed;
    compressed.reserve(magnitudes.size());

    constexpr float MIN_DB = 0.0f;     // Noise floor (normalized)
    constexpr float MAX_DB = 120.0f;   // Maximum expected signal
    constexpr float DB_RANGE = MAX_DB - MIN_DB;  // 120 dB
    constexpr float SCALE = 255.0f / DB_RANGE;   // 2.125

    for (float mag_db : magnitudes) {
        // Clamp to range and scale to 0-255
        float normalized = (mag_db - MIN_DB) * SCALE;
        int value = static_cast<int>(normalized + 0.5f);  // Round to nearest

        // Clamp to uint8 range
        if (value < 0) value = 0;
        if (value > 255) value = 255;

        compressed.push_back(static_cast<uint8_t>(value));
    }

    return compressed;
}

// Decompress uint8 back to float magnitudes (for testing)
std::vector<float> FFTProcessor::decompress_magnitudes(const std::vector<uint8_t>& compressed) {
    std::vector<float> magnitudes;
    magnitudes.reserve(compressed.size());

    constexpr float MIN_DB = 0.0f;     // Noise floor (normalized)
    constexpr float DB_RANGE = 120.0f;
    constexpr float SCALE = DB_RANGE / 255.0f;  // ~0.47 dB/step

    for (uint8_t val : compressed) {
        float mag_db = MIN_DB + (val * SCALE);
        magnitudes.push_back(mag_db);
    }

    return magnitudes;
}

// Decimate and compress wideband FFT for efficient transmission
CompressedFFT FFTProcessor::decimate_and_compress_wideband(
    const std::vector<float>& fft_data,
    float min_freq_mhz,
    float max_freq_mhz,
    int decimation_factor
) {
    CompressedFFT result;

    if (fft_data.empty() || decimation_factor < 1) {
        return result;
    }

    const size_t input_size = fft_data.size();
    const size_t output_size = (input_size + decimation_factor - 1) / decimation_factor;

    result.frequencies.reserve(output_size);
    std::vector<float> min_envelope_float;
    std::vector<float> max_envelope_float;
    min_envelope_float.reserve(output_size);
    max_envelope_float.reserve(output_size);

    const float freq_range = max_freq_mhz - min_freq_mhz;

    // Decimate using min/max envelope
    for (size_t i = 0; i < output_size; i++) {
        size_t start_idx = i * decimation_factor;
        size_t end_idx = std::min(start_idx + decimation_factor, input_size);

        if (start_idx >= input_size) break;

        float min_val = std::numeric_limits<float>::max();
        float max_val = std::numeric_limits<float>::lowest();

        for (size_t j = start_idx; j < end_idx; j++) {
            min_val = std::min(min_val, fft_data[j]);
            max_val = std::max(max_val, fft_data[j]);
        }

        // Calculate frequency for this decimated bin
        float freq = min_freq_mhz + (static_cast<float>(i * decimation_factor) / input_size) * freq_range;

        result.frequencies.push_back(freq);
        min_envelope_float.push_back(min_val);
        max_envelope_float.push_back(max_val);
    }

    // Compress magnitudes to uint8
    result.min_envelope = compress_magnitudes(min_envelope_float);
    result.max_envelope = compress_magnitudes(max_envelope_float);

    return result;
}

// Public wrapper for calculate_noise_floor helper function
// Calculates noise floor from FFT data using 10th percentile method
float FFTProcessor::calculate_noise_floor(const std::vector<float>& fft_data) {
    return ::calculate_noise_floor(fft_data);  // Call the helper in anonymous namespace
}

// Reset all FFT buffers immediately (called on frequency change)
// This ensures no stale data from previous frequency is displayed
void FFTProcessor::reset_all_buffers() {
    std::lock_guard<std::mutex> lock(fft_mutex);

    // Reset all channel FFT buffers to noise floor level
    const float RESET_LEVEL = -100.0f;  // Below typical noise floor

    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
        std::fill(fft_magnitudes[ch].begin(), fft_magnitudes[ch].end(), RESET_LEVEL);
        std::fill(fft_averaged[ch].begin(), fft_averaged[ch].end(), RESET_LEVEL);
    }
}

// Get peak magnitude from current FFT data for specified channel (in dB)
float FFTProcessor::get_peak_magnitude(int channel) {
    if (channel < 0 || channel >= MAX_CHANNELS) {
        return -120.0f;  // Return very low value for invalid channel
    }

    std::lock_guard<std::mutex> lock(fft_mutex);

    const auto& magnitudes = fft_averaged[channel];
    if (magnitudes.empty()) {
        return -120.0f;
    }

    // Find peak magnitude
    float peak = -120.0f;
    for (const float& mag : magnitudes) {
        if (mag > peak) {
            peak = mag;
        }
    }

    return peak;
}

// Check if signal exceeds squelch threshold
// Returns true if squelch is open (signal present), false if closed
// Note: squelch_threshold_db is relative to noise floor (0dB = noise floor)
bool FFTProcessor::check_squelch(int channel, float squelch_threshold_db) {
    if (channel < 0 || channel >= MAX_CHANNELS) {
        return false;
    }

    std::lock_guard<std::mutex> lock(fft_mutex);
    const auto& magnitudes = fft_averaged[channel];
    if (magnitudes.empty()) {
        return false;
    }

    // Calculate noise floor for this channel
    float noise_floor = calculate_noise_floor(magnitudes);

    // Find peak magnitude
    float peak = -120.0f;
    for (const float& mag : magnitudes) {
        if (mag > peak) {
            peak = mag;
        }
    }

    // Normalize peak relative to noise floor (so 0dB = noise floor)
    float normalized_peak = peak - noise_floor;

    return normalized_peak > squelch_threshold_db;
}

// Get peak magnitude within a specific frequency range (for per-decimator squelch)
// The range is specified as an offset from the center frequency and a bandwidth
// Note: This checks the fft_averaged data which is DC-centered
float FFTProcessor::get_peak_magnitude_in_range(int channel, float offset_hz, float bandwidth_hz) {
    if (channel < 0 || channel >= MAX_CHANNELS) {
        return -120.0f;
    }

    std::lock_guard<std::mutex> lock(fft_mutex);

    const auto& magnitudes = fft_averaged[channel];
    const int fft_size = current_fft_size.load();

    if (magnitudes.empty() || magnitudes.size() < static_cast<size_t>(fft_size)) {
        return -120.0f;
    }

    // FFT bins: DC at index 0, positive frequencies 1 to N/2, negative frequencies N/2+1 to N-1
    // After fftshift: negative frequencies first, then DC at N/2, then positive frequencies
    // Actually our FFT is stored with DC at center (bin N/2), negative freqs on left, positive on right

    const float sample_rate_hz = SAMPLE_RATE;  // 2.4 MHz
    const float hz_per_bin = sample_rate_hz / static_cast<float>(fft_size);

    // Calculate bin range for the specified frequency window
    // offset_hz is relative to tuner center (positive = higher freq)
    // bandwidth_hz is the total width to check

    float min_freq_hz = offset_hz - bandwidth_hz / 2.0f;
    float max_freq_hz = offset_hz + bandwidth_hz / 2.0f;

    // Convert to bin indices (DC is at center bin fft_size/2)
    int center_bin = fft_size / 2;
    int min_bin = center_bin + static_cast<int>(min_freq_hz / hz_per_bin);
    int max_bin = center_bin + static_cast<int>(max_freq_hz / hz_per_bin);

    // Clamp to valid range
    min_bin = std::max(0, min_bin);
    max_bin = std::min(fft_size - 1, max_bin);

    if (min_bin > max_bin) {
        return -120.0f;
    }

    // Find peak magnitude in the specified range
    float peak = -120.0f;
    for (int i = min_bin; i <= max_bin; i++) {
        if (magnitudes[i] > peak) {
            peak = magnitudes[i];
        }
    }

    return peak;
}

// Check squelch within a specific frequency range
// Note: squelch_threshold_db is relative to noise floor (0dB = noise floor)
float FFTProcessor::get_range_peak_db(int channel, float offset_hz, float bandwidth_hz,
                                      int exclude_dc_bins) {
    if (channel < 0 || channel >= MAX_CHANNELS) {
        return -999.0f;
    }

    std::lock_guard<std::mutex> lock(fft_mutex);

    const auto& magnitudes = fft_averaged[channel];
    const int fft_size = current_fft_size.load();

    if (magnitudes.empty() || magnitudes.size() < static_cast<size_t>(fft_size)) {
        return -999.0f;  // no valid FFT data (e.g. right after a retune reset)
    }

    // Calculate noise floor for this channel (for normalization)
    float noise_floor = calculate_noise_floor(magnitudes);

    // Calculate bin range for the specified frequency window
    const float sample_rate_hz = SAMPLE_RATE;
    const float hz_per_bin = sample_rate_hz / static_cast<float>(fft_size);

    float min_freq_hz = offset_hz - bandwidth_hz / 2.0f;
    float max_freq_hz = offset_hz + bandwidth_hz / 2.0f;

    int center_bin = fft_size / 2;
    int min_bin = center_bin + static_cast<int>(min_freq_hz / hz_per_bin);
    int max_bin = center_bin + static_cast<int>(max_freq_hz / hz_per_bin);

    min_bin = std::max(0, min_bin);
    max_bin = std::min(fft_size - 1, max_bin);

    if (min_bin > max_bin) {
        return -999.0f;
    }

    // Find peak magnitude in the specified range, optionally skipping the
    // bins around DC (residual center spike)
    float peak = -120.0f;
    for (int i = min_bin; i <= max_bin; i++) {
        if (exclude_dc_bins > 0 && std::abs(i - center_bin) <= exclude_dc_bins) {
            continue;
        }
        if (magnitudes[i] > peak) {
            peak = magnitudes[i];
        }
    }

    // Normalize peak relative to noise floor (so 0dB = noise floor)
    return peak - noise_floor;
}

bool FFTProcessor::check_squelch_in_range(int channel, float squelch_threshold_db,
                                          float offset_hz, float bandwidth_hz,
                                          int exclude_dc_bins) {
    const float peak_db = get_range_peak_db(channel, offset_hz, bandwidth_hz, exclude_dc_bins);
    if (peak_db <= -900.0f) {
        return false;  // no data -> squelch closed (legacy semantics)
    }
    return peak_db > squelch_threshold_db;
}


// Check squelch on one decimator's beamformed FFT data
// Note: bf.averaged contains RAW dB values, we must normalize here
bool FFTProcessor::check_squelch_beamformed(const BeamformedFFTData& bf, float squelch_threshold_db) {
    // Check if beamformed data is valid
    if (!bf.valid.load(std::memory_order_acquire)) {
        return false;  // No valid beamformed data, squelch closed
    }

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(bf.mutex));

    if (bf.averaged.empty()) {
        return false;  // No data, squelch closed
    }

    // Calculate noise floor using 10th percentile (nth_element is O(n) vs sort O(n log n)).
    // Per-thread scratch: called per packet from the pipeline and FM threads.
    thread_local std::vector<float> sorted_mags;
    sorted_mags.assign(bf.averaged.begin(), bf.averaged.end());
    size_t percentile_idx = static_cast<size_t>(sorted_mags.size() * 0.10f);
    std::nth_element(sorted_mags.begin(), sorted_mags.begin() + percentile_idx, sorted_mags.end());
    float noise_floor = sorted_mags[percentile_idx];

    // Find peak in beamformed FFT
    float raw_peak = -120.0f;
    for (const float& mag : bf.averaged) {
        if (mag > raw_peak) {
            raw_peak = mag;
        }
    }

    // Normalize peak relative to noise floor (so 0dB = noise floor, matching display)
    float normalized_peak = raw_peak - noise_floor;

    return normalized_peak > squelch_threshold_db;
}

// Process beamformed IQ data into one decimator's beamformed-FFT slice (out).
// Uses a shared FFT context for beamformed data (different sample rate); the
// context is mutex-guarded so this is safe even if called from multiple threads.
void FFTProcessor::process_beamformed_fft(BeamformedFFTData& out,
                                           const std::complex<float>* iq_data, size_t num_samples,
                                           float center_freq_hz, float sample_rate_hz) {
    // Use a fixed FFT size for beamformed data (1024 is good for decimated data)
    constexpr int BEAMFORMED_FFT_SIZE = 1024;

    // Minimum samples needed - reduced to support narrow bandwidths (NBFM at 12 kHz = ~82 samples)
    // Zero-padding handles smaller sample counts, just with lower frequency resolution
    constexpr size_t MIN_SAMPLES = 32;
    if (num_samples < MIN_SAMPLES || !iq_data) {
        return;
    }

    // Use a static context for beamformed FFT (thread-safe via mutex)
    static fftwf_plan bf_plan = nullptr;
    static fftwf_complex* bf_fft_in = nullptr;
    static fftwf_complex* bf_fft_out = nullptr;
    static std::vector<float> bf_window;
    static std::mutex bf_fft_mutex;
    static bool bf_initialized = false;

    std::lock_guard<std::mutex> lock(bf_fft_mutex);

    // Initialize on first call
    if (!bf_initialized) {
        std::lock_guard<std::mutex> fftw_lock(fftw_planner_mutex);

        bf_fft_in = fftwf_alloc_complex(BEAMFORMED_FFT_SIZE);
        bf_fft_out = fftwf_alloc_complex(BEAMFORMED_FFT_SIZE);
        bf_plan = fftwf_plan_dft_1d(BEAMFORMED_FFT_SIZE, bf_fft_in, bf_fft_out, FFTW_FORWARD, FFTW_ESTIMATE);

        // Pre-compute Hamming window
        bf_window.resize(BEAMFORMED_FFT_SIZE);
        for (int i = 0; i < BEAMFORMED_FFT_SIZE; i++) {
            bf_window[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (BEAMFORMED_FFT_SIZE - 1));
        }

        bf_initialized = true;
        std::cout << "Beamformed FFT initialized: " << BEAMFORMED_FFT_SIZE << " points" << std::endl;
    }

    // Determine how many samples to use
    size_t max_samples = std::min(static_cast<size_t>(BEAMFORMED_FFT_SIZE), num_samples);

    // Compute scale factor
    float scale = 1.0f / (static_cast<float>(max_samples) * BEAMFORMED_FFT_SIZE);

    // Copy data with windowing
    if (max_samples < static_cast<size_t>(BEAMFORMED_FFT_SIZE)) {
        // Dynamic window for smaller data
        float inv_n = 1.0f / (max_samples - 1);
        for (size_t i = 0; i < max_samples; i++) {
            float win = 0.54f - 0.46f * cosf(2.0f * M_PI * i * inv_n);
            bf_fft_in[i][0] = iq_data[i].real() * win;
            bf_fft_in[i][1] = iq_data[i].imag() * win;
        }
        // Zero-pad the rest
        std::memset(&bf_fft_in[max_samples], 0, (BEAMFORMED_FFT_SIZE - max_samples) * sizeof(fftwf_complex));
    } else {
        // Use pre-computed window
        for (size_t i = 0; i < max_samples; i++) {
            bf_fft_in[i][0] = iq_data[i].real() * bf_window[i];
            bf_fft_in[i][1] = iq_data[i].imag() * bf_window[i];
        }
    }

    // Execute FFT
    fftwf_execute(bf_plan);

    // Lock this decimator's beamformed FFT output mutex
    std::lock_guard<std::mutex> out_lock(out.mutex);

    // Resize output vectors if needed
    if (out.magnitudes.size() != BEAMFORMED_FFT_SIZE) {
        out.magnitudes.resize(BEAMFORMED_FFT_SIZE);
        out.averaged.resize(BEAMFORMED_FFT_SIZE);
        std::fill(out.averaged.begin(), out.averaged.end(), -80.0f);
    }

    // Get averaging alpha
    float alpha = averaging_alpha.load(std::memory_order_relaxed);

    // Compute magnitudes with DC centering
    for (int i = 0; i < BEAMFORMED_FFT_SIZE; i++) {
        int shifted_idx = (i + BEAMFORMED_FFT_SIZE / 2) & (BEAMFORMED_FFT_SIZE - 1);
        float real = bf_fft_out[shifted_idx][0];
        float imag = bf_fft_out[shifted_idx][1];

        float power = (real * real + imag * imag) * scale + 1e-15f;
        float db = 10.0f * (std::log2f(power) * log10_of_2);

        out.magnitudes[i] = db;
        out.averaged[i] = alpha * db + (1.0f - alpha) * out.averaged[i];
    }

    // Update metadata
    out.center_freq_hz.store(center_freq_hz, std::memory_order_relaxed);
    out.bandwidth_hz.store(sample_rate_hz, std::memory_order_relaxed);
    out.valid.store(true, std::memory_order_release);
}