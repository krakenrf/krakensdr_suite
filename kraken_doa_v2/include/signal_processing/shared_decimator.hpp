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
        // The bandwidth the block was filtered to (<= output_rate_hz: the
        // rate is the nearest integer decimation that holds it)
        int bandwidth_hz = SAMPLE_RATE_HZ;
        // Position of sample 0 in the VFO's decimated stream (DecimatorInstance
        // ::stream_pos, set by the pipeline): MUSIC frames and the digital
        // decoder's talker spans are both counted in it, which is how a
        // frame is matched to the radio that transmitted it (talker_doa.hpp)
        uint64_t stream_pos = 0;
        
        MultiChannelDecimated() : num_channels(0), min_samples(0), decimation_used(1), 
                                  output_rate_hz(SAMPLE_RATE), freq_offset_hz(0.0f) {}
        
        // OPTIMIZATION: Enable move semantics
        MultiChannelDecimated(MultiChannelDecimated&&) = default;
        MultiChannelDecimated& operator=(MultiChannelDecimated&&) = default;
        
        // Still allow copying when needed
        MultiChannelDecimated(const MultiChannelDecimated&) = default;
        MultiChannelDecimated& operator=(const MultiChannelDecimated&) = default;
    };

    // One decimating low-pass FIR stage: complex samples, real taps. Output
    // j = the taps' dot product with the L inputs ending at input j * M
    // (liquid firdecim's alignment). The samples no output has used up yet
    // stay in its buffer, so it takes any number of samples per call. It
    // replaced liquid's firdecim_crcf (2026-10-21): that copies every input
    // sample into its window with a call of its own - ~8.9 ns of the 9.5-13
    // ns a sample cost on the Pi 5, the multiplies ~0.14 ns each; this
    // computes each output straight from the buffer: 1.5-3x faster, the same
    // outputs to float rounding (tools/decim_bench.cpp)
    struct FirStage {
        int M = 1;
        std::vector<float> hr;                    // the taps, reversed
        std::vector<std::complex<float>> buf;     // history + the samples not used up
        size_t pos = 0;                           // buf index of the next output's newest sample

        FirStage(int m, const std::vector<float>& taps);
        void reset();                             // a clean start: zero history (as liquid's window)
        std::complex<float>* append(size_t n);    // room for n new samples at the end of buf
        size_t pending() const;                   // outputs the samples in buf give now
        size_t produce(std::complex<float>* out); // computes them (pending() of them), drops what no later output needs
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
    // change only retunes the mixer. Real taps on complex samples (FirStage;
    // half the multiplies of the complex-tap cccf it used before). The mixer
    // writes straight into the first stage's buffer, each stage's outputs
    // into the next one's: a block that isn't a multiple of a stage's factor
    // leaves its tail in that stage for the next block (it was a separate
    // "carry"), so no input sample is dropped and the stream stays
    // continuous across blocks.
    struct ChannelDecimState {
        std::vector<FirStage> stages;        // Cascaded low-pass decimators (none = the whole band)
        int decimation_factor = 0;           // Total factor (0 = not built)
        int bandwidth_hz = 0;                // The last stage's cut (with the factor)
        float freq_offset_hz = 0.0f;         // Mixer frequency
        std::complex<float> mixer_phase{1.0f, 0.0f};  // Persists across blocks

        // Back to a clean start: mixer phase 1, empty filter history, nothing pending
        void resetRuntime() {
            mixer_phase = std::complex<float>(1.0f, 0.0f);
            for (auto& s : stages) s.reset();
        }
        void destroyStages() { stages.clear(); }
    };
    std::vector<std::unique_ptr<ChannelDecimState>> channel_states_;
    // Channel count of the previous decimateMultiChannel block. Coherent
    // processing needs every channel's mixer phase and filter state (history + pending samples)
    // to be identical; a channel that sat out some blocks (element count
    // 5 -> 4 -> 5, or newly added) would otherwise resume with a stale or
    // fresh state and a fixed phase error vs the others. Only touched by the
    // (sequential) decimateMultiChannel caller.
    size_t last_num_channels_ = 0;

    // Current settings
    std::atomic<float> current_freq_offset_hz{0.0f};
    std::atomic<int> current_bandwidth_hz{bandwidth_option_hz(DEFAULT_BANDWIDTH_INDEX)};

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
    
    // Bandwidth (any whole Hz, VFO_BW_MIN_HZ .. the whole band - config.hpp):
    // the decimation factor follows from it (decimation_for_bandwidth)
    void setBandwidthHz(int hz);
    void setBandwidthIndex(int index) { setBandwidthHz(bandwidth_option_hz(index)); }   // a BANDWIDTH_OPTIONS size
    int getBandwidthHz() const { return current_bandwidth_hz.load(); }
    int getDecimationFactor() const { return decimation_for_bandwidth(getBandwidthHz()); }
    float getBandwidthMhz() const { return getBandwidthHz() / 1e6f; }
    float getOutputRate() const { return SAMPLE_RATE / getDecimationFactor(); }

    // Frequency translation
    void setFrequencyOffset(float offset_hz);
    float getFrequencyOffset() const { return current_freq_offset_hz.load(); }

    // Direct float IQ input methods (efficient - no conversion needed).
    // bandwidth_hz <= 0 = the current one (getBandwidthHz)
    DecimatedData decimateChannel(
        const std::complex<float>* iq_data,
        size_t num_samples,
        int channel_id,
        int bandwidth_hz = -1,
        float freq_offset_hz = 0.0f
    );

    // Zero-copy core: the caller guarantees the pointed-to sample arrays
    // stay valid for the duration of the call (processing only reads them).
    // A null pointer / zero length entry yields an empty result. Pass the
    // bandwidth read ONCE per block, so every channel of it is filtered alike
    MultiChannelDecimated decimateMultiChannel(
        const std::complex<float>* const* channel_ptrs,
        const size_t* channel_lens,
        size_t num_channels,
        int bandwidth_hz = -1,
        float freq_offset_hz = 0.0f
    );

    // Convenience overloads - both only READ the input and delegate to the
    // pointer core; neither copies the sample data
    MultiChannelDecimated decimateMultiChannel(
        const std::vector<std::vector<std::complex<float>>>& channel_data,
        int bandwidth_hz = -1,
        float freq_offset_hz = 0.0f
    );

    MultiChannelDecimated decimateMultiChannel(
        std::vector<std::vector<std::complex<float>>>&& channel_data,
        int bandwidth_hz = -1,
        float freq_offset_hz = 0.0f
    );

    // Statistics
    size_t getBlocksProcessed() const { return total_blocks_processed.load(); }
    size_t getSamplesProcessed() const { return total_samples_processed.load(); }

    // Decompose a total decimation factor into cascaded stage factors
    static std::vector<int> decomposeFactor(int decimation_factor);

    // The cascade's filters for a bandwidth: per stage its factor and real
    // taps (the decimator, the benchmark and the offline tests share it)
    struct StageDesign {
        int factor;
        std::vector<float> taps;
    };
    static std::vector<StageDesign> designStages(int bandwidth_hz);

private:
    // Ensure the per-channel mixer/stage cascade matches the requested
    // settings, rebuilding the stages when the bandwidth changed.
    ChannelDecimState* ensureChannelState(int channel, int bandwidth_hz, float freq_offset_hz);

    // (Re)build the low-pass stage cascade for a bandwidth
    bool buildStages(ChannelDecimState& st, int bandwidth_hz);
};