#pragma once

#include <vector>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <map>
#include <thread>
#include <string>
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

    // Samples held by a stage: grows without zeroing what it adds (the
    // producer writes it at once), drops from the front
    struct SampleBuf {
        std::vector<std::complex<float>> v;       // storage (only ever grows)
        size_t n = 0;                             // samples held
        std::complex<float>* grow(size_t k);      // room for k more at the end
        void drop(size_t k);                      // the first k go
        void zeros(size_t k);                     // k zeros and nothing else
        std::complex<float>* data() { return v.data(); }
        const std::complex<float>* data() const { return v.data(); }
    };

    // Where a producer's n samples go: one run (b null), or a halfband
    // stage's two phases - a gets the 1st, 3rd ... sample, b the 2nd, 4th ...
    struct Dst {
        std::complex<float>* a = nullptr;
        std::complex<float>* b = nullptr;
    };

    // One decimating low-pass FIR stage on complex samples. Output j = the
    // taps' dot product with the inputs ending at input j * M (liquid
    // firdecim's alignment), computed straight from the stage's buffer of
    // history + the samples no output has used up yet, so it takes any
    // number of samples per call; 4 outputs per pass sharing the tap loads,
    // the taps duplicated per I / Q float and padded to a multiple of 4 with
    // leading zeros (GCC vector extensions -> NEON / SSE). It replaced
    // liquid's firdecim_crcf (2026-10-21): that copies every input sample
    // into its window with a call of its own - ~8.9 ns of the 9.5-13 ns a
    // sample cost on the Pi 5, a multiply-add ~0.14 ns. Kinds:
    //  - regular: real taps
    //  - folded (fold(w), the first stage of a plan with `fold`): the VFO's
    //    mixer in the taps - h[k] e^{+jwk} on the RAW input, each output
    //    turned by the mixer's phase at its newest sample: the oscillator runs
    //    at the output rate instead of the input rate
    //  - halfband (factor 2, cut-off at a quarter of its input rate): every
    //    other tap is 0 but the centre one. The input kept as its two phases
    //    (x = the outputs' newest samples, o = the others, written so by the
    //    producer - take()), the x taps a plain FIR at the output rate + the
    //    centre tap on o: ~40 % fewer multiply-adds than a regular stage
    struct FirStage {
        int M = 1;
        bool half = false;
        size_t L = 0;                             // window (a multiple of 4; halfband: of the x phase)
        std::vector<float> h;                     // its taps, oldest sample first (the padding first)
        std::vector<float> t2;                    // h, each twice (a sample's I and Q)
        float hc = 0.0f;                          // halfband: the centre tap (on o)
        size_t cd = 0;                            // halfband: o's history (the centre's delay)
        bool folded = false;                      // complex taps in use
        std::vector<float> g2r, g2i;              // folded taps' re / im, each twice
        std::complex<float> ph{1.0f, 0.0f};       // folded: the mixer's phase at the next output's newest sample
        std::complex<float> ps{1.0f, 0.0f};       // folded: its step per output
        SampleBuf x;                              // the samples (halfband: the even phase)
        SampleBuf o;                              // halfband: the odd phase
        size_t pos = 0;                           // index in x of the next output's newest sample
        unsigned par = 0;                         // halfband: the next sample goes to x (0) or o (1)

        // taps = the design's (StageDesign::taps); halfband needs 4m+3 of them
        FirStage(int m, const std::vector<float>& taps, bool halfband = false);
        void reset();                             // a clean start: zero history (as liquid's window)
        Dst take(size_t n);                       // room for n new samples
        size_t pending() const;                   // outputs the samples held give now
        void produce(Dst out);                    // computes them, drops what no later output needs
        void fold(double w);                      // the mixer (w rad per input sample, 0 = none) in the taps
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
    // Architecture per channel: an NCO mixes the wanted band to baseband
    // (phase-continuous across blocks), a cascade of low-pass Kaiser
    // decimation stages reduces the rate (planStages: the cheapest split of
    // the factor). Cascading keeps the anti-alias filtering adequate at high
    // total factors (the old single stage was capped at 127 taps, which
    // barely filtered anything above factor ~63), and offset-independent
    // filters mean an offset change only retunes the mixer. The mixer runs
    // at the input rate in front of the first stage, writing into its
    // buffer (a halfband's two phases), or - `fold` - inside the first
    // stage's taps (FirStage::fold). Each stage's outputs go straight into
    // the next one's buffer: a block that isn't a multiple of a stage's
    // factor leaves its tail in that stage for the next block, so no input
    // sample is dropped and the stream stays continuous across blocks.
    struct ChannelDecimState {
        std::vector<FirStage> stages;        // Cascaded low-pass decimators (none = the whole band)
        bool fold = false;                   // The mixer is in the first stage's taps
        int decimation_factor = 0;           // Total factor (0 = not built)
        int bandwidth_hz = 0;                // The last stage's cut (with the factor)
        float freq_offset_hz = 0.0f;         // Mixer frequency
        std::complex<float> mixer_phase{1.0f, 0.0f};  // Persists across blocks (the mixer in front)

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

    // The cascade for a bandwidth (the decimator, the benchmark and the
    // offline tests share it): per stage its factor, kind and real taps,
    // whether the mixer is folded into the first stage, and the cost model's
    // estimate (ns per input sample of one antenna, mixer included, Pi 5)
    struct StageDesign {
        int factor;
        bool half = false;                   // run as a halfband (taps: 4m+3, the even offsets from the centre 0)
        std::vector<float> taps;
    };
    struct Plan {
        std::vector<StageDesign> stages;
        bool fold = false;
        double cost_ns = 0.0;
        std::string describe() const;        // "8F:31 5:23 2h:39" (factor, F folded, h halfband : taps)
    };
    static Plan planStages(int bandwidth_hz);

private:
    // Ensure the per-channel mixer/stage cascade matches the requested
    // settings, rebuilding the stages when the bandwidth changed.
    ChannelDecimState* ensureChannelState(int channel, int bandwidth_hz, float freq_offset_hz);

    // (Re)build the low-pass stage cascade for a bandwidth
    bool buildStages(ChannelDecimState& st, int bandwidth_hz);
};