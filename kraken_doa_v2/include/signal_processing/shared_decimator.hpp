#pragma once

#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <map>
#include <thread>
#include <liquid/liquid.h>
#include <complex>
#include "types.hpp"
#include "config.hpp"
#include "utils/thread_pool.hpp"

// Shared decimator with frequency translation and integrated bandwidth management
class SharedDecimator {
public:
    struct DecimatedData {
        std::vector<std::complex<float>> samples;
        size_t num_samples;
        int channel_id;
        float frequency_hz;
        int decimation_used;
        float output_rate_hz;
        float freq_offset_hz;
        
        DecimatedData() : num_samples(0), channel_id(0), frequency_hz(100e6f), 
                          decimation_used(1), output_rate_hz(SAMPLE_RATE), freq_offset_hz(0.0f) {}
        
        // OPTIMIZATION: Enable move semantics
        DecimatedData(DecimatedData&&) = default;
        DecimatedData& operator=(DecimatedData&&) = default;
        
        // Still allow copying when needed
        DecimatedData(const DecimatedData&) = default;
        DecimatedData& operator=(const DecimatedData&) = default;
    };
    
    struct MultiChannelDecimated {
        std::vector<DecimatedData> channels;
        size_t num_channels;
        size_t min_samples;
        int decimation_used;
        float output_rate_hz;
        float freq_offset_hz;
        
        MultiChannelDecimated() : num_channels(0), min_samples(0), decimation_used(1), 
                                  output_rate_hz(SAMPLE_RATE), freq_offset_hz(0.0f) {}
        
        // OPTIMIZATION: Enable move semantics
        MultiChannelDecimated(MultiChannelDecimated&&) = default;
        MultiChannelDecimated& operator=(MultiChannelDecimated&&) = default;
        
        // Still allow copying when needed
        MultiChannelDecimated(const MultiChannelDecimated&) = default;
        MultiChannelDecimated& operator=(const MultiChannelDecimated&) = default;
    };

private:
    std::mutex init_mutex;  // Guards initialize()/cleanup()
    std::atomic<bool> initialized{false};
    int num_channels_initialized = 0;

    // Per-channel filter/mixer state. The decimation filters and the input
    // mixer carry history between blocks, so this state MUST follow the
    // channel, not the worker thread (the ThreadPool gives no stable
    // channel→thread mapping). Within a block exactly one pool task touches
    // a given channel, and blocks are processed sequentially per
    // SharedDecimator, so no per-state locking is needed.
    //
    // Architecture per channel: an input-rate NCO mixes the wanted band to
    // baseband (phase-continuous across blocks), then a cascade of plain
    // low-pass Kaiser decimation stages reduces the rate. Cascading keeps
    // the anti-alias filtering adequate at high total factors (the old
    // single stage was capped at 127 taps, which barely filtered anything
    // above factor ~63), and offset-independent filters mean an offset
    // change only retunes the mixer.
    struct ChannelDecimState {
        std::vector<firdecim_cccf> stages;   // Cascaded low-pass decimators
        std::vector<int> stage_factors;      // Per-stage decimation factors
        int decimation_factor = 0;           // Total factor (0 = not built)
        float freq_offset_hz = 0.0f;         // Mixer frequency
        std::complex<float> mixer_phase{1.0f, 0.0f};  // Persists across blocks
        // Already-mixed input samples not yet consumed by the cascade (always
        // fewer than decimation_factor): a block that isn't a multiple of the
        // factor leaves its tail here for the next block, so no input sample
        // is dropped and the stream stays continuous across blocks.
        std::vector<std::complex<float>> carry;

        void destroyStages() {
            for (auto s : stages) {
                if (s) firdecim_cccf_destroy(s);
            }
            stages.clear();
            stage_factors.clear();
        }
        ~ChannelDecimState() { destroyStages(); }
    };
    std::vector<std::unique_ptr<ChannelDecimState>> channel_states_;

    // Current settings
    std::atomic<float> current_freq_offset_hz{0.0f};
    std::atomic<int> current_bandwidth_index{DEFAULT_BANDWIDTH_INDEX};

    // Statistics
    std::atomic<size_t> total_blocks_processed{0};
    std::atomic<size_t> total_samples_processed{0};

    // Thread pool for parallel channel decimation
    std::unique_ptr<ThreadPool> channel_thread_pool;

public:
    SharedDecimator();
    ~SharedDecimator();
    
    bool initialize(int num_channels);
    void cleanup();
    
    // Bandwidth management (per-decimator bandwidth option / decimation factor)
    void setBandwidthIndex(int index);
    int getBandwidthIndex() const { return current_bandwidth_index.load(); }
    int getDecimationFactor() const;
    float getBandwidthMhz() const;
    const char* getBandwidthName() const;
    bool isValidBandwidthIndex(int index) const;
    
    // Frequency translation
    void setFrequencyOffset(float offset_hz);
    float getFrequencyOffset() const { return current_freq_offset_hz.load(); }

    // Direct float IQ input methods (efficient - no conversion needed)
    DecimatedData decimateChannel(
        const std::complex<float>* iq_data, 
        size_t num_samples,
        int channel_id, 
        int decimation_factor = -1,
        float freq_offset_hz = 0.0f
    );
    
    // Zero-copy core: the caller guarantees the pointed-to sample arrays
    // stay valid for the duration of the call (processing only reads them).
    // A null pointer / zero length entry yields an empty result.
    MultiChannelDecimated decimateMultiChannel(
        const std::complex<float>* const* channel_ptrs,
        const size_t* channel_lens,
        size_t num_channels,
        int decimation_factor = -1,
        float freq_offset_hz = 0.0f
    );

    // Convenience overloads - both only READ the input and delegate to the
    // pointer core; neither copies the sample data
    MultiChannelDecimated decimateMultiChannel(
        const std::vector<std::vector<std::complex<float>>>& channel_data,
        int decimation_factor = -1,
        float freq_offset_hz = 0.0f
    );

    MultiChannelDecimated decimateMultiChannel(
        std::vector<std::vector<std::complex<float>>>&& channel_data,
        int decimation_factor = -1,
        float freq_offset_hz = 0.0f
    );
    
    // Get effective bandwidth after decimation
    float getOutputRate(int decimation_factor = -1) const;
    float getOutputBandwidth(int decimation_factor = -1) const;
    
    // Statistics
    size_t getBlocksProcessed() const { return total_blocks_processed.load(); }
    size_t getSamplesProcessed() const { return total_samples_processed.load(); }

private:
    // Ensure the per-channel mixer/stage cascade matches the requested
    // settings, rebuilding the stages when the decimation factor changed.
    ChannelDecimState* ensureChannelState(int channel, int decimation_factor, float freq_offset_hz);

    // Decompose a total decimation factor into cascaded stage factors
    static std::vector<int> decomposeFactor(int decimation_factor);

    // (Re)build the low-pass stage cascade for the given total factor
    bool buildStages(ChannelDecimState& st, int decimation_factor);

    const BandwidthOption& getBandwidthOptionSafe(int index) const;
};