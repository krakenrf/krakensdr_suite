#include "signal_processing/fm_demodulator.hpp"
#include "config.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>
#include <atomic>

FMDemodulatorRobust::FMDemodulatorRobust() : audio_ring_buffer(AUDIO_RING_BUFFER_SIZE) {
    // Initialize with default values
    input_sample_rate = 240000.0f;
    audio_resampler = nullptr;

    setupAudioProcessing(input_sample_rate.load());
    recreateFilters();

    // Initialize Opus encoder (48 kHz, mono, VOIP application for low latency)
    int error;
    opus_encoder = opus_encoder_create(AUDIO_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !opus_encoder) {
        std::cerr << "Failed to create Opus encoder: " << opus_strerror(error) << std::endl;
        opus_encoder = nullptr;
    } else {
        // Configure encoder for optimal FM audio quality and low latency
        opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
        opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));  // Better for FM audio
        opus_encoder_ctl(opus_encoder, OPUS_SET_VBR(1));  // Variable bitrate for better quality
        opus_encoder_ctl(opus_encoder, OPUS_SET_VBR_CONSTRAINT(0));  // Unconstrained VBR

        // Pre-allocate buffers
        opus_input_buffer.resize(AUDIO_SAMPLES_PER_PACKET);
        opus_output_buffer.resize(4000);  // Max Opus packet size

        std::cout << "Opus encoder initialized: 48kHz mono, " << (OPUS_BITRATE/1000) << " kbps, complexity " << OPUS_COMPLEXITY << std::endl;
    }

    std::cout << "FM Demodulator initialized (WBFM mode, 75kHz deviation)" << std::endl;
    std::cout << "Default: " << (input_sample_rate.load()/1000.0f) << "kHz input" << std::endl;
}

void FMDemodulatorRobust::setDemodulatorMode(DemodulatorMode mode) {
    if (mode != current_mode.load()) {
        current_mode = mode;
        need_filter_recreation = true;

        // Reset demod state under the processing lock - the FM thread
        // reads/writes these same plain floats inside process_decimated_samples
        {
            std::lock_guard<std::mutex> lock(process_mutex);
            nbfm_dc_state = 0.0f;
            am_dc_state = 0.0f;
            am_agc_gain = 1.0f;
            prev_sample = {0.0f, 0.0f};
        }

        const char* mode_name;
        switch (mode) {
            case DemodulatorMode::WBFM: mode_name = "WBFM"; break;
            case DemodulatorMode::NBFM: mode_name = "NBFM"; break;
            case DemodulatorMode::AM:   mode_name = "AM"; break;
            default: mode_name = "Unknown"; break;
        }

        if (mode == DemodulatorMode::AM) {
            std::cout << "Demodulator mode changed to " << mode_name
                      << " (envelope detection)" << std::endl;
        } else {
            float deviation = getCurrentDeviation();
            std::cout << "Demodulator mode changed to " << mode_name
                      << " (deviation: " << (deviation/1000.0f) << " kHz)" << std::endl;
        }
    }
}

DemodulatorMode FMDemodulatorRobust::getDemodulatorMode() const {
    return current_mode.load();
}

float FMDemodulatorRobust::getCurrentDeviation() const {
    switch (current_mode.load()) {
        case DemodulatorMode::WBFM: return WBFM_DEVIATION_HZ;
        case DemodulatorMode::NBFM: return NBFM_DEVIATION_HZ;
        case DemodulatorMode::AM:   return 0.0f;  // AM doesn't use deviation
        default: return NBFM_DEVIATION_HZ;
    }
}

FMDemodulatorRobust::~FMDemodulatorRobust() {
    processing_enabled = false;

    // Wait for any in-flight processing call to release the lock before
    // destroying the liquid-dsp objects (a sleep is not a guarantee)
    std::lock_guard<std::mutex> lock(process_mutex);

    // Cleanup Opus encoder
    if (opus_encoder) {
        opus_encoder_destroy(opus_encoder);
        opus_encoder = nullptr;
    }

    // Cleanup liquid-dsp objects
    if (rf_agc) agc_crcf_destroy(rf_agc);
    if (deemphasis) iirfilt_rrrf_destroy(deemphasis);
    if (dc_blocker) iirfilt_rrrf_destroy(dc_blocker);
    if (audio_resampler) resamp_rrrf_destroy(audio_resampler);
}

void FMDemodulatorRobust::setInputSampleRate(float rate_hz) {
    if (rate_hz != input_sample_rate.load()) {
        input_sample_rate = rate_hz;
        setupAudioProcessing(rate_hz);
        need_filter_recreation = true;
        
        std::cout << "FM input rate changed to " << (rate_hz/1000.0f) << "kHz, using audio resampling to exactly 48.0kHz (ratio=" << resampling_ratio.load() << ")" << std::endl;
    }
}

float FMDemodulatorRobust::getInputSampleRate() const {
    return input_sample_rate.load();
}

void FMDemodulatorRobust::setupAudioProcessing(float input_rate) {
    // ALWAYS resample (no integer audio decimation) to guarantee exactly 48kHz
    // output regardless of input bandwidth.
    // Exact 48 kHz ratio. The decimator carries each block's leftover input
    // into the next block, so the demod input runs at exactly the nominal
    // rate (no per-block truncation loss to compensate for).
    resampling_ratio = AUDIO_SAMPLE_RATE / input_rate;

    std::cout << "Audio processing: " << (input_rate/1000.0f) << "kHz -> 48.0kHz using resampling"
              << " (ratio=" << resampling_ratio.load() << ")" << std::endl;
}

void FMDemodulatorRobust::updateFiltersIfNeeded() {
    if (need_filter_recreation.exchange(false)) {
        std::lock_guard<std::mutex> lock(process_mutex);
        recreateFilters();
    }
}

void FMDemodulatorRobust::recreateFilters() {
    // Reset state variables
    prev_sample = {0.0f, 0.0f};
    nbfm_dc_state = 0.0f;
    am_dc_state = 0.0f;
    am_agc_gain = 1.0f;

    // Cleanup existing filters
    if (rf_agc) agc_crcf_destroy(rf_agc);
    if (deemphasis) iirfilt_rrrf_destroy(deemphasis);
    if (dc_blocker) iirfilt_rrrf_destroy(dc_blocker);
    if (audio_resampler) resamp_rrrf_destroy(audio_resampler);

    float current_input_rate = input_sample_rate.load();
    DemodulatorMode mode = current_mode.load();

    // Initialize RF AGC with mode-dependent settings
    rf_agc = agc_crcf_create();
    if (mode == DemodulatorMode::AM) {
        // AM: Fast AGC for bursty signals
        agc_crcf_set_bandwidth(rf_agc, 0.1f);   // Fast AGC response for bursty AM
        agc_crcf_set_gain(rf_agc, 1.0f);
        agc_crcf_set_scale(rf_agc, 0.7f);       // Slightly higher scale for AM
    } else {
        // FM: Slower AGC to avoid modulation artifacts
        agc_crcf_set_bandwidth(rf_agc, 0.005f); // Slower AGC response
        agc_crcf_set_gain(rf_agc, 1.0f);
        agc_crcf_set_scale(rf_agc, 0.5f);       // Conservative scale to prevent overdriving
    }

    const char* mode_name;
    switch (mode) {
        case DemodulatorMode::WBFM: mode_name = "WBFM"; break;
        case DemodulatorMode::NBFM: mode_name = "NBFM"; break;
        case DemodulatorMode::AM:   mode_name = "AM"; break;
        default: mode_name = "Unknown"; break;
    }

    // Demodulation itself is done inline in process_decimated_samples
    // (envelope detection / atan2 phase discriminator) - no liquid-dsp object.
    if (mode == DemodulatorMode::AM) {
        std::cout << "Created AM demodulator: mode=" << mode_name
                  << ", input_rate=" << (current_input_rate/1000.0f)
                  << "kHz (envelope detection)" << std::endl;
    } else {
        float deviation_hz = (mode == DemodulatorMode::WBFM) ? WBFM_DEVIATION_HZ : NBFM_DEVIATION_HZ;
        std::cout << "Created FM demodulator: mode=" << mode_name
                  << ", deviation=" << (deviation_hz/1000.0f)
                  << "kHz, input_rate=" << (current_input_rate/1000.0f) << "kHz" << std::endl;
    }
    
    // ALWAYS use audio resampler to guarantee exactly 48kHz output
    float ratio = resampling_ratio.load();
    
    // Design resampler parameters
    unsigned int filter_len = 13;      // Good filter length
    float as = 60.0f;                  // Stop-band attenuation
    float cutoff_freq = 0.4f;          // Conservative cutoff frequency

    // liquid's cutoff is relative to the INPUT rate (its own default is
    // min(0.49, rate/2)). Upsampling (ratio > 1) only has to reject images
    // above the input Nyquist, so cut just under it. (0.4/ratio cut the audio
    // itself - ~1.2 kHz at 12 kHz bandwidth, ~250 Hz at 5 kHz - and made
    // NBFM voice muffled.)
    if (ratio > 1.0f) {
        cutoff_freq = 0.45f;
    }
    // For downsampling (ratio < 1), use formula that prevents aliasing
    else if (ratio < 1.0f) {
        cutoff_freq = ratio * 0.4f;  // Prevents aliasing by keeping cutoff below output Nyquist
        if (cutoff_freq > 0.45f) cutoff_freq = 0.45f;  // Maximum cutoff
    }

    audio_resampler = resamp_rrrf_create(ratio, filter_len, cutoff_freq, as, 64);

    if (!audio_resampler) {
        std::cerr << "CRITICAL ERROR: Failed to create audio resampler with ratio=" << ratio
                  << ", cutoff=" << cutoff_freq << ", filter_len=" << filter_len
                  << ", as=" << as << std::endl;
        // Try again with more conservative parameters
        cutoff_freq = 0.15f;
        filter_len = 10;
        audio_resampler = resamp_rrrf_create(ratio, filter_len, cutoff_freq, as, 64);
        if (!audio_resampler) {
            std::cerr << "CRITICAL ERROR: Resampler creation failed even with conservative params!" << std::endl;
            // Try one more time with minimal parameters
            cutoff_freq = 0.1f;
            filter_len = 8;
            as = 40.0f;
            audio_resampler = resamp_rrrf_create(ratio, filter_len, cutoff_freq, as, 32);
            if (!audio_resampler) {
                std::cerr << "FATAL ERROR: Cannot create resampler even with minimal params! Disabling FM processing." << std::endl;
                processing_enabled = false;
                return;
            }
            std::cerr << "Created resampler with minimal fallback parameters" << std::endl;
        } else {
            std::cerr << "Created resampler with fallback parameters" << std::endl;
        }
    }

    std::cout << "Created audio resampler: " << (current_input_rate/1000.0f) << "kHz -> 48.0kHz exactly"
             << " (ratio=" << ratio << ", cutoff=" << cutoff_freq << ", filter_len=" << filter_len << ")" << std::endl;


    // De-emphasis filter (75µs time constant for FM broadcast)
    // Only used for WBFM - NBFM/AM don't use pre-emphasis
    if (mode == DemodulatorMode::WBFM) {
        // Bilinear one-pole low-pass at fc = 1/(2π·75µs) ≈ 2122 Hz:
        //   t = tan(π·fc/fs), H(z) = (t/(1+t))·(1+z⁻¹) / (1 - ((1-t)/(1+t))·z⁻¹)
        // (The previous design used alpha = tan²(wc/2), which put the corner
        // at ~300 Hz instead of 2122 Hz.)
        float tau = 75e-6f;
        float fc = 1.0f / (2.0f * M_PI * tau);
        float t = tanf(M_PI * fc / AUDIO_SAMPLE_RATE);
        float b0 = t / (1.0f + t);
        float b_deemph[2] = {b0, b0};
        float a_deemph[2] = {1.0f, -(1.0f - t) / (1.0f + t)};
        deemphasis = iirfilt_rrrf_create(b_deemph, 2, a_deemph, 2);
        std::cout << "De-emphasis filter enabled (75µs) for WBFM" << std::endl;
    } else {
        // NBFM/AM: No de-emphasis needed, create unity passthrough filter
        float b_pass[1] = {1.0f};
        float a_pass[1] = {1.0f};
        deemphasis = iirfilt_rrrf_create(b_pass, 1, a_pass, 1);
        std::cout << "De-emphasis filter bypassed for " << mode_name << std::endl;
    }

    // DC blocker: one-pole high-pass at 20 Hz for exactly 48 kHz:
    //   H(z) = g·(1 - z⁻¹) / (1 - r·z⁻¹), r = (1-t)/(1+t), g = (1+r)/2
    // (The previous design's pole landed at z ≈ -1, i.e. at Nyquist - that
    // made it a full-band differentiator that killed all bass, not a 20 Hz
    // high-pass.)
    float t_dc = tanf(M_PI * 20.0f / AUDIO_SAMPLE_RATE);
    float r_dc = (1.0f - t_dc) / (1.0f + t_dc);
    float g_dc = (1.0f + r_dc) / 2.0f;
    float b_dc[2] = {g_dc, -g_dc};
    float a_dc[2] = {1.0f, -r_dc};
    dc_blocker = iirfilt_rrrf_create(b_dc, 2, a_dc, 2);
}

// OPTIMIZATION: Move version - takes ownership of input data
void FMDemodulatorRobust::process_decimated_samples(
    std::vector<std::complex<float>>&& decimated_samples, std::vector<float>& audio_out) {

    audio_out.clear();
    if (!processing_enabled.load() || decimated_samples.empty()) {
        return;
    }

    updateFiltersIfNeeded();

    std::unique_lock<std::mutex> lock(process_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !processing_enabled.load()) {
        return;
    }

    // Calculate signal strength
    float sum_power = 0.0f;
    for (const auto& sample : decimated_samples) {
        float real = sample.real();
        float imag = sample.imag();
        sum_power += real * real + imag * imag;
    }
    signal_strength = sum_power / decimated_samples.size();
    
    // Step 1: Demodulation of all samples
    work_demod_audio_.clear();
    work_demod_audio_.reserve(decimated_samples.size());

    float deviation_hz = getCurrentDeviation();
    float current_rate = input_sample_rate.load();
    DemodulatorMode mode = current_mode.load();

    if (mode == DemodulatorMode::AM) {
        // AM demodulation: envelope detection
        // Simply take the magnitude of the complex signal
        for (const auto& sample : decimated_samples) {
            // Apply RF AGC
            liquid_float_complex liq_sample{sample.real(), sample.imag()};
            liquid_float_complex agc_output;
            agc_crcf_execute(rf_agc, liq_sample, &agc_output);

            // Envelope detection: magnitude of the signal
            float envelope = std::sqrt(agc_output.real() * agc_output.real() +
                                       agc_output.imag() * agc_output.imag());

            // Apply simple AGC to normalize the envelope
            float target_level = 0.3f;
            float error = target_level - envelope * am_agc_gain;
            am_agc_gain += am_agc_alpha * error;
            if (am_agc_gain < 0.1f) am_agc_gain = 0.1f;
            if (am_agc_gain > 10.0f) am_agc_gain = 10.0f;

            float am_output = envelope * am_agc_gain;

            // DC removal to center the audio around zero
            am_dc_state = am_dc_alpha * am_dc_state + (1.0f - am_dc_alpha) * am_output;
            am_output = am_output - am_dc_state;

            // Scale output
            am_output *= 2.0f;

            work_demod_audio_.push_back(am_output);
        }
    } else {
        // FM demodulation (WBFM or NBFM)
        // Manual FM demodulation (quadrature discriminator), ~10x faster than
        // liquid-dsp freqdem_demodulate

        // Calculate FM demodulation gain
        // For proper normalization: output should be ±1 for ±max_deviation
        // phase_diff_max = 2*pi * deviation / sample_rate
        // We want: fm_output = phase_diff * gain = ±1 when deviation = max
        // So: gain = sample_rate / (2*pi * deviation)
        float gain;
        if (mode == DemodulatorMode::NBFM) {
            // For NBFM: Use much lower gain to prevent clipping
            // NBFM signals are typically strong relative to their deviation
            gain = current_rate / (2.0f * M_PI * deviation_hz);
            // Strict limit to prevent clipping - NBFM needs much less gain
            if (gain > 0.75f) gain = 0.75f;
        } else {
            // WBFM: same normalization - ±1 output at ±max deviation.
            // (The old 1/kf form was π× larger than this spec and relied on
            // the broken de-emphasis/DC-blocker designs to cancel it.)
            gain = current_rate / (2.0f * M_PI * deviation_hz);
        }

        // Process samples with optimized algorithm (complex multiply method)
        for (const auto& sample : decimated_samples) {
            // Apply RF AGC (still using liquid-dsp for RF stage)
            liquid_float_complex liq_sample{sample.real(), sample.imag()};
            liquid_float_complex agc_output;
            agc_crcf_execute(rf_agc, liq_sample, &agc_output);

            // Clamp AGC gain to prevent oscillation after calibration
            // The liquid-dsp AGC can oscillate between 0 and 1000000 with high-power transients
            float current_agc_gain = agc_crcf_get_gain(rf_agc);
            constexpr float MIN_AGC_GAIN = 0.1f;
            constexpr float MAX_AGC_GAIN = 100.0f;
            if (current_agc_gain < MIN_AGC_GAIN || current_agc_gain > MAX_AGC_GAIN) {
                agc_crcf_set_gain(rf_agc, std::clamp(current_agc_gain, MIN_AGC_GAIN, MAX_AGC_GAIN));
            }

            // Quadrature discriminator: the phase step is the angle of
            // s[n] * conj(s[n-1]), already in (-pi, pi] (no unwrap), taken
            // with an exact atan2f. The former linear arctangent fit was off
            // by up to 4.07 deg per sample angle - up to 7.9 deg on each phase
            // step (7% of full deviation), i.e. 27 dB SINAD of audible
            // distortion. atan2f costs ~23 ns/sample on a Pi 4 (0.55% of one
            // core at the 240 kS/s WBFM rate), so the approximation bought
            // nothing worth that.
            const std::complex<float> cur(agc_output.real(), agc_output.imag());
            const std::complex<float> step = cur * std::conj(prev_sample);
            const float phase_diff = std::atan2(step.imag(), step.real());
            prev_sample = cur;

            float fm_output = phase_diff * gain;

            // For NBFM: Apply fast DC removal inline to prevent slow fade
            // This uses a simple single-pole high-pass filter with fast response
            if (mode == DemodulatorMode::NBFM) {
                nbfm_dc_state = nbfm_dc_alpha * nbfm_dc_state + (1.0f - nbfm_dc_alpha) * fm_output;
                fm_output = fm_output - nbfm_dc_state;
            }

            work_demod_audio_.push_back(fm_output);
        }
    }
    
    // Step 2: Always use resampling to get exactly 48kHz output
    work_processed_audio_.clear();

    // BUGFIX: Check if audio_resampler is valid before using it
    if (!audio_resampler) {
        std::cerr << "ERROR: audio_resampler is null, cannot process FM audio!" << std::endl;
        return;
    }

    // Use resampler to get exactly 48kHz - no decimation path
    size_t total_resampled = 0;
    for (const float& audio_sample : work_demod_audio_) {
        unsigned int num_written;
        // resamp_rrrf_execute writes up to ceil(ratio)+1 samples per input.
        // Worst case is the 1 kHz bandwidth option: 48000/1000 ≈ 48 → 49 writes.
        float output_samples[64];

        resamp_rrrf_execute(audio_resampler, audio_sample, output_samples, &num_written);

        for (unsigned int i = 0; i < num_written; i++) {
            work_processed_audio_.push_back(output_samples[i]);
        }
        total_resampled += num_written;
    }

    // Step 3: Apply audio processing (de-emphasis, DC block, gain)
    for (float audio_sample : work_processed_audio_) {
        // Apply de-emphasis (WBFM only - NBFM/AM have passthrough filter)
        iirfilt_rrrf_execute(deemphasis, audio_sample, &audio_sample);

        // Apply DC blocking - but skip for NBFM/AM since we already did fast DC removal
        // The slow DC blocker (6 second time constant) causes NBFM audio to fade
        if (mode == DemodulatorMode::WBFM) {
            iirfilt_rrrf_execute(dc_blocker, audio_sample, &audio_sample);
        }

        // Apply gain compensation and boost
        float gain_compensation;
        switch (mode) {
            case DemodulatorMode::NBFM:
                // NBFM: Low gain - signal is already strong
                gain_compensation = 0.5f;
                break;
            case DemodulatorMode::AM:
                // AM: Moderate gain - AGC already applied in demodulation
                gain_compensation = 1.5f;
                break;
            case DemodulatorMode::WBFM:
            default:
                // WBFM: modest makeup gain. The discriminator now outputs ±1
                // at full deviation and the corrected filters no longer eat
                // ~30 dB of mid-band, so the old 6.0x would clip constantly.
                gain_compensation = 1.5f;
                break;
        }
        audio_sample *= gain_compensation;

        // Soft limiting to prevent clipping
        if (fabsf(audio_sample) > 0.95f) {
            audio_sample = 0.95f * tanhf(audio_sample / 0.95f);
        }

        audio_out.push_back(audio_sample);
        // Peak meter with decay - a pure max() would ratchet up and stick at
        // the historical peak forever
        float lvl = audio_level.load();
        audio_level = std::max(lvl * 0.999f, fabsf(audio_sample));
    }
}


void FMDemodulatorRobust::add_audio_samples(const std::vector<float>& samples) {
    if (samples.empty() || !processing_enabled.load()) {
        return;
    }

    audio_ring_buffer.write(samples.data(), samples.size());

    if (!initial_fill_complete.load()) {
        if (audio_ring_buffer.available() >= INITIAL_FILL_TARGET) {
            initial_fill_complete = true;
            std::cout << "Audio buffer initial fill complete" << std::endl;
        }
    }
}

std::vector<float> FMDemodulatorRobust::get_audio_buffer(size_t requested_samples) {
    std::vector<float> result(requested_samples);

    if (!processing_enabled.load() || !initial_fill_complete.load()) {
        std::fill(result.begin(), result.end(), 0.0f);
        return result;
    }

    audio_ring_buffer.read(result.data(), requested_samples);
    return result;
}

float FMDemodulatorRobust::get_signal_strength() const { 
    return signal_strength.load(); 
}

float FMDemodulatorRobust::get_audio_level() const { 
    return audio_level.load(); 
}

void FMDemodulatorRobust::get_buffer_stats(size_t& writes, size_t& reads, size_t& over, size_t& under) const {
    audio_ring_buffer.get_stats(writes, reads, over, under);
}

float FMDemodulatorRobust::get_buffer_fill() const { 
    return audio_ring_buffer.fill_percentage(); 
}

void FMDemodulatorRobust::disable_processing() {
    processing_enabled = false;

    {
        std::lock_guard<std::mutex> lock(process_mutex);
        audio_ring_buffer.clear();
        audio_ring_buffer.reset_stats();
        initial_fill_complete = false;
    }

    signal_strength = 0.0f;
    audio_level = 0.0f;
}

void FMDemodulatorRobust::enable_processing() {
    processing_enabled = true;
    DemodulatorMode mode = current_mode.load();
    const char* mode_name;
    switch (mode) {
        case DemodulatorMode::WBFM: mode_name = "WBFM"; break;
        case DemodulatorMode::NBFM: mode_name = "NBFM"; break;
        case DemodulatorMode::AM:   mode_name = "AM"; break;
        default: mode_name = "Unknown"; break;
    }

    if (mode == DemodulatorMode::AM) {
        std::cout << "Audio processing enabled (" << mode_name << " mode, envelope detection)" << std::endl;
    } else {
        float deviation = getCurrentDeviation();
        std::cout << "Audio processing enabled (" << mode_name << " mode, "
                  << (deviation/1000.0f) << "kHz deviation)" << std::endl;
    }
}

bool FMDemodulatorRobust::is_processing_enabled() const {
    return processing_enabled.load();
}

void FMDemodulatorRobust::reset_audio_buffer() {
    std::lock_guard<std::mutex> lock(process_mutex);

    // Clear the audio buffer and reset state
    audio_ring_buffer.clear();
    prev_sample = {0.0f, 0.0f};
    nbfm_dc_state = 0.0f;
    am_dc_state = 0.0f;
    am_agc_gain = 1.0f;

    // Reset AGC to known good state
    if (rf_agc) {
        agc_crcf_reset(rf_agc);
    }

    // Reset resampler state
    if (audio_resampler) {
        resamp_rrrf_reset(audio_resampler);
    }

    // Force buffer refill before audio resumes
    initial_fill_complete = false;
}

// Wideband mode support: Determine which tuner covers the target frequency
int FMDemodulatorRobust::get_tuner_for_frequency(
    uint32_t target_freq_hz,
    const std::array<std::atomic<uint32_t>, MAX_CHANNELS>& tuner_freqs,
    int num_tuners
) {
    if (num_tuners <= 0 || num_tuners > MAX_CHANNELS) {
        return 0;  // Default to first tuner
    }

    // Each tuner covers its center frequency +/- half the sample rate
    const uint32_t half_bandwidth = SAMPLE_RATE / 2;

    // Find the tuner whose coverage includes the target frequency
    int best_tuner = 0;
    uint32_t min_distance = UINT32_MAX;

    for (int i = 0; i < num_tuners; i++) {
        uint32_t tuner_center = tuner_freqs[i].load();
        uint32_t lower_edge = tuner_center - half_bandwidth;
        uint32_t upper_edge = tuner_center + half_bandwidth;

        // Check if target frequency is within this tuner's coverage
        if (target_freq_hz >= lower_edge && target_freq_hz <= upper_edge) {
            // Calculate distance from center
            uint32_t distance = (target_freq_hz > tuner_center) ?
                                (target_freq_hz - tuner_center) :
                                (tuner_center - target_freq_hz);

            // Prefer tuner with target frequency closest to center
            if (distance < min_distance) {
                min_distance = distance;
                best_tuner = i;
            }
        }
    }

    // If no tuner covers the frequency, find the closest one
    if (min_distance == UINT32_MAX) {
        for (int i = 0; i < num_tuners; i++) {
            uint32_t tuner_center = tuner_freqs[i].load();
            uint32_t distance = (target_freq_hz > tuner_center) ?
                                (target_freq_hz - tuner_center) :
                                (tuner_center - target_freq_hz);

            if (distance < min_distance) {
                min_distance = distance;
                best_tuner = i;
            }
        }
    }

    return best_tuner;
}

std::vector<unsigned char> FMDemodulatorRobust::get_opus_packet(size_t requested_samples) {
    if (!opus_encoder) {
        return std::vector<unsigned char>();
    }

    // The int16 conversion below writes requested_samples entries into
    // opus_input_buffer - reject anything beyond its fixed capacity
    if (requested_samples > opus_input_buffer.size()) {
        return std::vector<unsigned char>();
    }

    auto pcm_samples = get_audio_buffer(requested_samples);
    if (pcm_samples.size() != requested_samples) {
        return std::vector<unsigned char>();
    }

    // Convert float PCM [-1.0, 1.0] to int16 PCM [-32768, 32767]
    for (size_t i = 0; i < pcm_samples.size(); i++) {
        float clamped = std::max(-1.0f, std::min(1.0f, pcm_samples[i]));
        opus_input_buffer[i] = static_cast<opus_int16>(clamped * 32767.0f);
    }

    // Encode to Opus
    opus_int32 encoded_bytes = opus_encode(
        opus_encoder,
        opus_input_buffer.data(),
        requested_samples,
        opus_output_buffer.data(),
        opus_output_buffer.size()
    );

    if (encoded_bytes < 0) {
        std::cerr << "Opus encoding failed: " << opus_strerror(encoded_bytes) << std::endl;
        return std::vector<unsigned char>();
    }

    // Return the encoded packet (copy only the used bytes)
    return std::vector<unsigned char>(opus_output_buffer.begin(), opus_output_buffer.begin() + encoded_bytes);
}

