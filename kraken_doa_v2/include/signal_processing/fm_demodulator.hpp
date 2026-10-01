#pragma once

#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <complex>
#include <liquid/liquid.h>
#include <opus/opus.h>
#include "config.hpp"
#include "utils/ring_buffer.hpp"

// Demodulator mode enum
enum class DemodulatorMode {
    WBFM,   // Wideband FM (broadcast FM, 75kHz deviation)
    NBFM,   // Narrowband FM (voice comms, ~5kHz deviation)
    AM      // Amplitude Modulation (envelope detection)
};

class FMDemodulatorRobust {
private:
    // liquid-dsp objects. Must be null-initialized: recreateFilters() and the
    // destructor call *_destroy() on any non-null pointer, which would free
    // garbage for a stack/heap-constructed instance.
    agc_crcf rf_agc = nullptr;
    iirfilt_rrrf deemphasis = nullptr;
    iirfilt_rrrf dc_blocker = nullptr;

    // Audio buffer
    AudioRingBuffer audio_ring_buffer;
    std::atomic<bool> initial_fill_complete{false};

    // Opus encoder for audio compression
    OpusEncoder* opus_encoder = nullptr;
    std::vector<opus_int16> opus_input_buffer;  // int16 input for Opus
    std::vector<unsigned char> opus_output_buffer;  // Encoded Opus packet
    static constexpr int OPUS_BITRATE = 32000;  // 32 kbps (good quality for FM)
    static constexpr int OPUS_COMPLEXITY = 5;   // 0-10, 5 is balanced

    // Monitoring
    std::atomic<float> signal_strength{0.0f};
    std::atomic<float> audio_level{0.0f};

    // Thread safety
    mutable std::mutex process_mutex;
    std::atomic<bool> processing_enabled{true};

    // Configuration - proper FM handling
    std::atomic<float> input_sample_rate{240000.0f};
    static constexpr float WBFM_DEVIATION_HZ = 75000.0f;  // 75kHz for wideband FM (broadcast)
    static constexpr float NBFM_DEVIATION_HZ = 5000.0f;   // 5kHz for narrowband FM (voice comms)
    std::atomic<DemodulatorMode> current_mode{DemodulatorMode::WBFM};

    // DC removal for NBFM/AM (simple IIR high-pass, faster than WBFM DC blocker)
    float nbfm_dc_alpha{0.995f};  // Faster DC removal for NBFM
    float nbfm_dc_state{0.0f};    // DC filter state

    // AM demodulation state
    float am_dc_alpha{0.99f};     // DC removal for AM (fast settling for bursty signals)
    float am_dc_state{0.0f};      // AM DC filter state
    float am_agc_gain{1.0f};      // AM AGC gain
    float am_agc_alpha{0.05f};    // AM AGC attack/decay rate (fast for bursty signals)
    
    // Resampling support
    resamp_rrrf audio_resampler = nullptr;  // Real-to-real resampler for audio
    // Downsampling (input > 48 kHz): multi-stage half-band + arbitrary
    // resampler, used instead of audio_resampler (see recreateFilters)
    msresamp_rrrf audio_msresampler = nullptr;
    std::atomic<float> resampling_ratio{1.0f};
    
    // Filter recreation tracking
    std::atomic<bool> need_filter_recreation{false};

    // FM discriminator state: the previous (AGC'd) sample. Zero after a
    // reset, so the first output is 0 (atan2(0, 0)).
    std::complex<float> prev_sample{0.0f, 0.0f};

    // Pre-allocated work buffers (avoid heap alloc per call in process_decimated_samples)
    std::vector<float> work_demod_audio_;
    std::vector<float> work_processed_audio_;

public:
    FMDemodulatorRobust();
    ~FMDemodulatorRobust();

    // Configuration methods
    void setInputSampleRate(float rate_hz);
    float getInputSampleRate() const;
    void updateFiltersIfNeeded();

    // Demodulator mode control
    void setDemodulatorMode(DemodulatorMode mode);
    DemodulatorMode getDemodulatorMode() const;
    float getCurrentDeviation() const;
    
    // Process pre-decimated complex samples into 48 kHz audio. audio_out is
    // cleared and refilled; the caller keeps it across calls so its capacity is
    // reused (left empty when nothing was produced).
    void process_decimated_samples(std::vector<std::complex<float>>&& decimated_samples,
                                   std::vector<float>& audio_out);
    
    void add_audio_samples(const std::vector<float>& samples);
    std::vector<float> get_audio_buffer(size_t requested_samples = 480);
    std::vector<unsigned char> get_opus_packet(size_t requested_samples = 480);

    float get_signal_strength() const;
    float get_audio_level() const;
    void get_buffer_stats(size_t& writes, size_t& reads, size_t& over, size_t& under) const;
    float get_buffer_fill() const;
    
    void disable_processing();
    void enable_processing();
    bool is_processing_enabled() const;
    void reset_audio_buffer();  // Clear audio buffer (call after FFT settings change)

    // Wideband mode support
    static int get_tuner_for_frequency(uint32_t target_freq_hz,
                                        const std::array<std::atomic<uint32_t>, MAX_CHANNELS>& tuner_freqs,
                                        int num_tuners);

private:
    void recreateFilters();
    void setupAudioProcessing(float input_rate);

};
