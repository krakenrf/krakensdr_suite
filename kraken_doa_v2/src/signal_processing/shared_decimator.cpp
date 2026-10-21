// ============================================
// src/signal_processing/shared_decimator.cpp
// HYBRID PARALLELIZATION: Per-antenna threading within SharedDecimator
// Per channel: input-rate NCO mixer (phase-continuous across blocks)
// followed by a cascade of Kaiser low-pass decimation stages.
// ============================================

#include "signal_processing/shared_decimator.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>

using namespace std;

SharedDecimator::SharedDecimator() {}

SharedDecimator::~SharedDecimator() {
    cleanup();
}

bool SharedDecimator::initialize(int num_channels) {
    lock_guard<mutex> lock(init_mutex);

    if (initialized.load()) {
        cout << "SharedDecimator: Already initialized, cleaning up first" << endl;
        cleanup();
    }

    num_channels_initialized = num_channels;
    current_bandwidth_hz = bandwidth_option_hz(DEFAULT_BANDWIDTH_INDEX);

    // One persistent filter/mixer state per channel (see header comment)
    channel_states_.clear();
    channel_states_.reserve(num_channels);
    for (int ch = 0; ch < num_channels; ch++) {
        channel_states_.push_back(std::make_unique<ChannelDecimState>());
    }

    // Create thread pool for parallel channel decimation
    channel_thread_pool = std::make_unique<ThreadPool>(num_channels);

    initialized = true;

    cout << "SharedDecimator initialized:" << endl;
    cout << "  Channels: " << num_channels << endl;
    cout << "  ThreadPool size: " << num_channels << " threads" << endl;
    cout << "  Default bandwidth: " << getBandwidthHz() << " Hz (decimation " << getDecimationFactor() << ")" << endl;
    cout << "  Multi-stage decimation with input-rate mixer enabled" << endl;

    return true;
}

void SharedDecimator::cleanup() {
    // Destroy thread pool first (waits for all tasks to complete)
    channel_thread_pool.reset();

    // Safe to drop per-channel state only after the pool is gone
    channel_states_.clear();

    initialized = false;
}

// Decompose a total decimation factor into cascaded stage factors,
// largest-first, each <= 8. Every factor a bandwidth runs at divides 2.4 MHz
// (2^a * 3^b * 5^c - decimation_for_bandwidth), so this always yields a
// complete factorization; an unexpected large prime falls back to a single
// stage.
std::vector<int> SharedDecimator::decomposeFactor(int decimation_factor) {
    std::vector<int> factors;
    int remaining = decimation_factor;
    while (remaining > 1) {
        int picked = 0;
        for (int f = 8; f >= 2; f--) {
            if (remaining % f == 0) {
                picked = f;
                break;
            }
        }
        if (picked == 0) {
            picked = remaining;  // Prime > 8 - run it as one stage
        }
        factors.push_back(picked);
        remaining /= picked;
    }
    return factors;
}

std::vector<SharedDecimator::StageDesign> SharedDecimator::designStages(int bandwidth_hz) {
    bandwidth_hz = clamp_bandwidth_hz(bandwidth_hz);
    const int decimation_factor = decimation_for_bandwidth(bandwidth_hz);
    std::vector<StageDesign> out;

    // The bandwidth fills its output rate (bandwidth_is_efficient): the
    // filters exactly as before arbitrary bandwidths (bandwidth = rate)
    const bool exact = bandwidth_hz * decimation_factor == SAMPLE_RATE_HZ;
    if (exact && decimation_factor == 1) return out;   // the whole band: mixer only

    // Stages largest-first; a narrower bandwidth than the full rate without
    // decimation (1.2 - 2.4 MHz) is one stage of factor 1 - a plain filter
    std::vector<int> factors = decomposeFactor(decimation_factor);
    if (factors.empty()) factors.push_back(1);

    // Per-stage Kaiser low-pass design. At each stage only frequencies that
    // alias onto the final wanted band (+-0.4 * the bandwidth, matching the
    // 80% usable-bandwidth convention) need to be attenuated, so early stages
    // get wide transition bands and few taps while the last stage makes the
    // sharp cut at a low rate. This keeps anti-aliasing adequate at ANY
    // total factor - the old single stage was capped at 127 taps, which was
    // grossly under-dimensioned above factor ~63. The last stage stops from
    // 0.6 * the bandwidth (-6 dB at its edge): with the bandwidth = the rate
    // that is where the aliases begin, as it always was; a narrower
    // bandwidth in a wider rate (the rate holds it - the next divisor of 2.4
    // MHz) is cut there too, so the VFO passes its bandwidth and no more, and
    // its gain (each stage 1 / (2 cutoff)) is SAMPLE_RATE / bandwidth for
    // every width, = D for the efficient sizes.
    const float final_rate = SAMPLE_RATE / static_cast<float>(decimation_factor);
    const float bandwidth = exact ? final_rate : static_cast<float>(bandwidth_hz);
    const float passband_hz = 0.4f * bandwidth;
    float stage_input_rate = SAMPLE_RATE;

    for (size_t s = 0; s < factors.size(); s++) {
        const int factor = factors[s];
        float stage_output_rate = stage_input_rate / factor;
        float stopband_hz = stage_output_rate - passband_hz;
        if (s + 1 == factors.size() && !exact) stopband_hz = std::min(stopband_hz, 0.6f * bandwidth);
        float cutoff_norm = 0.5f * (passband_hz + stopband_hz) / stage_input_rate;
        float transition_norm = (stopband_hz - passband_hz) / stage_input_rate;

        // Kaiser tap estimate for 60 dB stopband attenuation
        unsigned int taps = static_cast<unsigned int>(
            ceilf(52.0f / (14.36f * transition_norm)));
        taps = std::clamp(taps, 9u, 2047u);
        if ((taps % 2) == 0) taps++;

        StageDesign d{factor, vector<float>(taps)};
        liquid_firdes_kaiser(taps, cutoff_norm, 60.0f, 0.0f, d.taps.data());
        out.push_back(std::move(d));
        stage_input_rate = stage_output_rate;
    }
    return out;
}

bool SharedDecimator::buildStages(ChannelDecimState& st, int bandwidth_hz) {
    st.destroyStages();

    for (auto& d : designStages(bandwidth_hz)) {
        if (d.taps.empty() || d.factor < 1) {
            cerr << "SharedDecimator: bad stage design (factor " << d.factor << ", "
                 << d.taps.size() << " taps)" << endl;
            st.destroyStages();
            return false;
        }
        st.stages.emplace_back(d.factor, d.taps);
    }
    return true;
}

// ---- FirStage: the block FIR (see the header) ------------------------------
SharedDecimator::FirStage::FirStage(int m, const std::vector<float>& taps)
    : M(std::max(1, m)), hr(taps.rbegin(), taps.rend()) {
    reset();
}

void SharedDecimator::FirStage::reset() {
    // L - 1 zeros of history: the first output's window ends at the first
    // sample, as liquid's window starts out
    buf.assign(hr.size() - 1, complex<float>(0.0f, 0.0f));
    pos = hr.size() - 1;
}

complex<float>* SharedDecimator::FirStage::append(size_t n) {
    const size_t at = buf.size();
    buf.resize(at + n);
    return buf.data() + at;
}

size_t SharedDecimator::FirStage::pending() const {
    return pos < buf.size() ? (buf.size() - 1 - pos) / static_cast<size_t>(M) + 1 : 0;
}

size_t SharedDecimator::FirStage::produce(complex<float>* out) {
    const size_t L = hr.size(), n = pending();
    const float* h = hr.data();
    // Interleaved I / Q as floats: two dot products with the same real taps,
    // which the compiler vectorizes (-Ofast lets it reorder the sums)
    const float* b = reinterpret_cast<const float*>(buf.data());
    for (size_t j = 0; j < n; j++) {
        const float* w = b + 2 * (pos - (L - 1));
        float ar = 0.0f, ai = 0.0f;
        for (size_t k = 0; k < L; k++) {
            ar += h[k] * w[2 * k];
            ai += h[k] * w[2 * k + 1];
        }
        out[j] = complex<float>(ar, ai);
        pos += static_cast<size_t>(M);
    }
    // Keep only what a later output needs: its window's first sample onwards
    const size_t drop = std::min(pos - (L - 1), buf.size());
    buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(drop));
    pos -= drop;
    return n;
}

// Per-channel state: only one pool task touches a given channel at a time
// (one task per channel per block, blocks sequential), so no locking needed.
SharedDecimator::ChannelDecimState* SharedDecimator::ensureChannelState(
    int channel, int bandwidth_hz, float freq_offset_hz) {

    ChannelDecimState& st = *channel_states_[channel];

    if (st.bandwidth_hz != bandwidth_hz) {
        // Bandwidth (and with it the factor / the filters) changed - rebuild
        // the cascade. Filter history and mixer phase are only meaningful for
        // unchanged settings.
        if (!buildStages(st, bandwidth_hz)) {
            st.decimation_factor = 0;
            st.bandwidth_hz = 0;
            return nullptr;
        }
        st.decimation_factor = decimation_for_bandwidth(bandwidth_hz);
        st.bandwidth_hz = bandwidth_hz;
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
    } else if (fabs(st.freq_offset_hz - freq_offset_hz) > 0.1f) {
        // Offset changed: the low-pass stages are offset-independent, so
        // only the mixer retunes (phase continuity is meaningless across a
        // retune). This also removes the old cache-quantization bug where a
        // filter built for one offset served requests up to 1 kHz away. The
        // few samples the stages hold mixed with the old offset go through
        // like their history (a transient; MUSIC drops the frames around an
        // offset change anyway) - the stream stays sample-continuous.
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
    }
    return &st;
}


// Bandwidth management methods
void SharedDecimator::setBandwidthHz(int hz) {
    // (no log line: a bar edge being dragged sets it ~12 times a second; the
    // control handler logs the applied width)
    current_bandwidth_hz = clamp_bandwidth_hz(hz);
}

void SharedDecimator::setFrequencyOffset(float offset_hz) {
    current_freq_offset_hz = offset_hz;
}

// Per-channel block decimation: input-rate mixer, then the stage cascade
SharedDecimator::DecimatedData SharedDecimator::decimateChannel(
    const complex<float>* iq_data, size_t num_samples, int channel_id,
    int bandwidth_hz, float freq_offset_hz) {

    DecimatedData result;
    result.channel_id = channel_id;

    bandwidth_hz = bandwidth_hz > 0 ? clamp_bandwidth_hz(bandwidth_hz) : getBandwidthHz();
    const int decimation_factor = decimation_for_bandwidth(bandwidth_hz);

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;

    if (!initialized.load() || channel_id < 0 || channel_id >= num_channels_initialized || !iq_data) {
        cerr << "SharedDecimator: Not initialized, invalid channel, or null data" << endl;
        return result;
    }

    // Per-channel persistent mixer/filter state (see ensureChannelState)
    ChannelDecimState* st = ensureChannelState(channel_id, bandwidth_hz, freq_offset_hz);
    if (!st) {
        cerr << "SharedDecimator: Failed to build decimator for channel " << channel_id << endl;
        return result;
    }

    // The (mixed) input goes straight into the first stage's buffer; the
    // whole band (no stage) straight into the result. Every stage keeps the
    // samples its next output still needs - the tail of a block that isn't
    // a multiple of its factor included - so no input sample is dropped.
    // (Dropping that tail, long ago, spliced the signal and jumped the mixer
    // phase at every block boundary - an audible buzz on any off-center VFO
    // whose factor doesn't divide the block size.)
    result.samples.clear();
    complex<float>* dst;
    if (st->stages.empty()) {
        result.samples.resize(num_samples);
        dst = result.samples.data();
    } else {
        dst = st->stages.front().append(num_samples);
    }

    // Mix the wanted band to baseband at the INPUT rate, before any
    // filtering. The oscillator runs over EVERY input sample and its phase
    // persists across blocks (st->mixer_phase) - a phase step at a block
    // boundary is audible as clicks in FM and contaminates MUSIC snapshots
    // that span blocks.
    if (fabs(freq_offset_hz) > 1.0f) {
        float phase_increment = -2.0f * M_PI * freq_offset_hz / SAMPLE_RATE;
        float sin_val, cos_val;
        sincosf(phase_increment, &sin_val, &cos_val);
        complex<float> phase_step(cos_val, sin_val);
        complex<float> phase = st->mixer_phase;

        for (size_t i = 0; i < num_samples; i++) {
            dst[i] = iq_data[i] * phase;
            phase *= phase_step;

            // Periodic normalization to prevent magnitude drift
            if ((i & 0x3FF) == 0x3FF) {
                float mag = abs(phase);
                if (mag > 0.0f) phase /= mag;
            }
        }

        // Normalize before saving so drift can't compound over blocks
        float mag = abs(phase);
        if (mag > 0.0f) phase /= mag;
        st->mixer_phase = phase;
    } else {
        std::copy(iq_data, iq_data + num_samples, dst);
    }

    // The cascade: each stage's outputs straight into the next one's
    // buffer, the last one's into the result
    for (size_t s = 0; s < st->stages.size(); s++) {
        FirStage& f = st->stages[s];
        const size_t n = f.pending();
        complex<float>* out;
        if (s + 1 == st->stages.size()) {
            result.samples.resize(n);
            out = result.samples.data();
        } else {
            out = st->stages[s + 1].append(n);
        }
        f.produce(out);
    }

    result.num_samples = result.samples.size();
    total_samples_processed.fetch_add(num_samples);
    total_blocks_processed.fetch_add(1);

    return result;  // NRVO will optimize this
}

// OPTIMIZED: Multi-channel float IQ input method with precomputed coefficients
// HYBRID PARALLELIZATION: Per-antenna threading. Zero-copy core - the
// caller's sample arrays are only read and must outlive the call (all pool
// futures are collected before returning, so that just means the call site's
// data must stay alive for the duration of this function).
SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    const complex<float>* const* channel_ptrs, const size_t* channel_lens,
    size_t num_channels, int bandwidth_hz, float freq_offset_hz) {

    MultiChannelDecimated result;

    bandwidth_hz = bandwidth_hz > 0 ? clamp_bandwidth_hz(bandwidth_hz) : getBandwidthHz();
    const int decimation_factor = decimation_for_bandwidth(bandwidth_hz);

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;
    result.bandwidth_hz = bandwidth_hz;

    if (!initialized.load() || num_channels == 0 || !channel_ptrs || !channel_lens) {
        return result;
    }

    // The offset is exactly the caller's, for every channel of the block. (A
    // fallback to the live current_freq_offset_hz when this was ~0 - re-read
    // again inside each channel task - let channels of one block mix at
    // different offsets when a VFO moved off 0 Hz mid-block.)
    result.num_channels = min(num_channels, static_cast<size_t>(num_channels_initialized));
    if (result.num_channels != last_num_channels_) {
        for (size_t ch = 0; ch < channel_states_.size(); ch++) channel_states_[ch]->resetRuntime();
        last_num_channels_ = result.num_channels;
    }
    result.channels.resize(result.num_channels);  // Pre-allocate exact size for parallel writes

    // Find minimum number of samples across channels
    size_t min_samples = SIZE_MAX;
    for (size_t ch = 0; ch < result.num_channels; ch++) {
        min_samples = min(min_samples, channel_ptrs[ch] ? channel_lens[ch] : 0);
    }

    if (min_samples == 0 || min_samples == SIZE_MAX) {
        return result;
    }
    // Per-channel output counts come from the channels themselves: with the
    // samples the stages keep from the block before, a block yields
    // floor(N/D) or floor(N/D)+1 samples. Every channel gets the same input
    // length, so their pending samples - and output counts - stay equal;
    // min_samples is the common count.
    result.min_samples = SIZE_MAX;

    // HYBRID PARALLELIZATION: Launch parallel tasks for each antenna using ThreadPool
    vector<std::future<DecimatedData>> antenna_futures(result.num_channels);

    for (size_t ch = 0; ch < result.num_channels; ch++) {
        const complex<float>* ch_ptr = channel_ptrs[ch];
        antenna_futures[ch] = channel_thread_pool->submit(
            [this, ch, ch_ptr, min_samples, bandwidth_hz, freq_offset_hz]() -> DecimatedData {
                return decimateChannel(
                    ch_ptr,
                    min_samples,
                    static_cast<int>(ch),
                    bandwidth_hz,
                    freq_offset_hz
                );
            }
        );
    }

    // Collect results from parallel antenna processing
    for (size_t ch = 0; ch < result.num_channels; ch++) {
        DecimatedData ch_data = antenna_futures[ch].get();

        if (ch_data.samples.empty() && min_samples >= static_cast<size_t>(decimation_factor)) {
            cerr << "SharedDecimator: No samples generated for channel " << ch << endl;
            // Consumers iterate channels[ch].samples[0..min_samples); an
            // empty channel makes the whole block unusable for coherent
            // processing, so mark it as such instead of leaving a stale
            // min_samples that would read past the empty vector
            result.min_samples = 0;
            continue;
        }

        if (ch_data.num_samples < result.min_samples) {
            result.min_samples = ch_data.num_samples;
        }

        result.channels[ch] = std::move(ch_data);  // Direct assignment to pre-allocated slot
    }

    // Log performance statistics periodically
    // Shared by every VFO's decimator, which run concurrently: atomic
    static std::atomic<size_t> stats_counter{0};
    if ((stats_counter.fetch_add(1, std::memory_order_relaxed) + 1) % 1000 == 0) {
        cout << "SharedDecimator performance (hybrid parallel): "
             << "Blocks: " << getBlocksProcessed() << ", "
             << "Samples: " << getSamplesProcessed() << endl;
    }

    return result;  // NRVO will optimize this
}

// Vector overloads: only READ the input and delegate to the pointer core.
// (The old const& version deep-copied the entire multichannel block.)
SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    const vector<vector<complex<float>>>& channel_data,
    int bandwidth_hz, float freq_offset_hz) {

    size_t n = min(channel_data.size(), static_cast<size_t>(MAX_CHANNELS));
    const complex<float>* ptrs[MAX_CHANNELS] = {nullptr};
    size_t lens[MAX_CHANNELS] = {0};
    for (size_t ch = 0; ch < n; ch++) {
        ptrs[ch] = channel_data[ch].data();
        lens[ch] = channel_data[ch].size();
    }
    return decimateMultiChannel(ptrs, lens, n, bandwidth_hz, freq_offset_hz);
}

SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    vector<vector<complex<float>>>&& channel_data,
    int bandwidth_hz, float freq_offset_hz) {

    return decimateMultiChannel(
        static_cast<const vector<vector<complex<float>>>&>(channel_data),
        bandwidth_hz, freq_offset_hz);
}