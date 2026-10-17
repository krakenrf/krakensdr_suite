#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include "kraken_fft.hpp"   // pffft (plugins/sdk)
#include "config.hpp"
#include "types.hpp"
#include "signal_processing/beamformed_fft.hpp"

// Compressed FFT data structure (using uint8 for 4x bandwidth reduction)
struct CompressedFFT {
    std::vector<float> frequencies;     // Still float32 for accuracy
    std::vector<uint8_t> min_envelope;  // Compressed magnitude data
    std::vector<uint8_t> max_envelope;  // Compressed magnitude data
};

// Per-channel FFT context for true parallel processing
// Each channel has its own FFT plan and buffers, eliminating serialization
struct ChannelFFTContext {
    kfft_plan plan = nullptr;
    kfft_complex* fft_in = nullptr;
    kfft_complex* fft_out = nullptr;
    std::mutex mutex;                    // Per-channel mutex for parallel execution
    std::vector<float> window_lut;       // Per-channel window lookup table
    int current_size = 0;                // Current FFT size for this channel
    bool initialized = false;

    ChannelFFTContext() = default;
    ~ChannelFFTContext() = default;

    // Non-copyable, non-movable due to mutex
    ChannelFFTContext(const ChannelFFTContext&) = delete;
    ChannelFFTContext& operator=(const ChannelFFTContext&) = delete;
    ChannelFFTContext(ChannelFFTContext&&) = delete;
    ChannelFFTContext& operator=(ChannelFFTContext&&) = delete;
};

// Global per-channel FFT contexts - declared here, defined in main.cpp
extern std::array<ChannelFFTContext, MAX_CHANNELS> channel_fft_contexts;

class FFTProcessor {
public:
    static void initialize();
    static void cleanup();

    // buffer_generation: persistent_data_buffer generation the data was
    // written under (0 = data not from the shared buffer). If the generation
    // changes while reading, the frame is discarded as possibly overwritten.
    static void process_channel_fft(const std::complex<float>* iq_data, size_t num_samples, int channel,
                                    uint32_t buffer_generation = 0);

    // Dynamic FFT resizing
    static void resize(int new_fft_size);
    static int get_current_size();
    static int get_current_decimation();
    static void set_decimation(int new_decimation);

    // Compressed decimation (uses uint8 for magnitudes)
    static CompressedDecimatedFFT decimate_fft_compressed(const std::vector<float>& fft_data, float center_freq_mhz, int decimation);

    // Wideband FFT stitching
    static void stitch_wideband_fft(
        const std::vector<std::vector<float>>& channel_ffts,
        const std::array<uint32_t, MAX_CHANNELS>& tuner_freqs,
        int num_tuners,
        std::vector<float>& output_fft,
        float& min_freq_mhz,
        float& max_freq_mhz
    );

    // Compress float magnitudes to uint8 (dB range: -120 to 0 dB)
    static std::vector<uint8_t> compress_magnitudes(const std::vector<float>& magnitudes);

    // Decompress uint8 back to float magnitudes
    static std::vector<float> decompress_magnitudes(const std::vector<uint8_t>& compressed);

    // Decimate and compress wideband FFT for efficient transmission
    static CompressedFFT decimate_and_compress_wideband(
        const std::vector<float>& fft_data,
        float min_freq_mhz,
        float max_freq_mhz,
        int decimation_factor
    );

    // Calculate noise floor from FFT data using 10th percentile method
    // Used for coherent mode normalization to match wideband reference level
    static float calculate_noise_floor(const std::vector<float>& fft_data);

    // The 0 dB reference of tuner channel `channel`'s averaged spectrum: the
    // floor of its latest frame, or the receiver's remembered floor when a
    // signal fills the band (noise_floor.hpp). Measured from fft_data when the
    // channel has no recent frame. Display, squelch and scanner all use it.
    static float channel_noise_floor(int channel, const std::vector<float>& fft_data);

    // Reset all FFT buffers immediately (called on frequency change)
    // Clears both raw and averaged buffers to prevent stale data display
    static void reset_all_buffers();

    // Get peak magnitude from current FFT data for specified channel (in dB)
    static float get_peak_magnitude(int channel);

    // Check if signal exceeds squelch threshold
    // Returns true if squelch is open (signal present), false if closed
    static bool check_squelch(int channel, float squelch_threshold_db);

    // Get peak magnitude within a specific frequency range (for per-decimator squelch)
    // center_freq_mhz: Center frequency of the range to check
    // bandwidth_mhz: Bandwidth of the range to check
    static float get_peak_magnitude_in_range(int channel, float center_freq_mhz, float bandwidth_mhz);

    // Check squelch within a specific frequency range
    // In-band peak relative to the channel's FFT noise floor (0 dB = floor).
    // Returns -999.0f when no valid FFT data is available - callers gating on
    // "no signal" MUST treat that as unknown, not as quiet.
    // exclude_dc_bins > 0 skips +-N bins around the spectrum center in the
    // peak search (the residual DC spike must not read as a signal when this
    // check gates the auto-squelch floor learning).
    static float get_range_peak_db(int channel, float center_freq_mhz, float bandwidth_mhz,
                                   int exclude_dc_bins = 0);
    static bool check_squelch_in_range(int channel, float squelch_threshold_db,
                                       float center_freq_mhz, float bandwidth_mhz,
                                       int exclude_dc_bins = 0);

    // Check squelch on one decimator's beamformed FFT data.
    // Returns true if squelch is open (signal present above threshold).
    // Note: beamformed FFT is normalized to noise floor = 0dB inside.
    static bool check_squelch_beamformed(const BeamformedFFTData& bf, float squelch_threshold_db);

    // Process beamformed IQ data into a decimator's beamformed-FFT slice (out).
    // Uses a shared FFT context (mutex-guarded) since beamformed data has a
    // different sample rate than the main FFT.
    static void process_beamformed_fft(BeamformedFFTData& out,
                                        const std::complex<float>* iq_data, size_t num_samples,
                                        float center_freq_hz, float sample_rate_hz);
};
