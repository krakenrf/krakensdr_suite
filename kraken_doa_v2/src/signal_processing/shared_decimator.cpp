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
    current_bandwidth_index = DEFAULT_BANDWIDTH_INDEX;

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
    cout << "  Default bandwidth: " << getBandwidthName() << " (" << getBandwidthMhz() << " MHz)" << endl;
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
// largest-first, each <= 8. All BANDWIDTH_OPTIONS factors are of the form
// 2^a * 3^b * 5^c, so this always yields a complete factorization; an
// unexpected large prime falls back to a single stage.
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

bool SharedDecimator::buildStages(ChannelDecimState& st, int decimation_factor) {
    st.destroyStages();

    if (decimation_factor <= 1) {
        return true;  // Pass-through, mixer only
    }

    st.stage_factors = decomposeFactor(decimation_factor);

    // Per-stage Kaiser low-pass design. At each stage only frequencies that
    // alias onto the final wanted band (+-0.4 * final_rate, matching the 80%
    // usable-bandwidth convention) need to be attenuated, so early stages
    // get wide transition bands and few taps while the last stage makes the
    // sharp cut at a low rate. This keeps anti-aliasing adequate at ANY
    // total factor - the old single stage was capped at 127 taps, which was
    // grossly under-dimensioned above factor ~63.
    const float final_rate = SAMPLE_RATE / static_cast<float>(decimation_factor);
    const float passband_hz = 0.4f * final_rate;
    float stage_input_rate = SAMPLE_RATE;

    for (int factor : st.stage_factors) {
        float stage_output_rate = stage_input_rate / factor;
        float stopband_hz = stage_output_rate - passband_hz;
        float cutoff_norm = 0.5f * (passband_hz + stopband_hz) / stage_input_rate;
        float transition_norm = (stopband_hz - passband_hz) / stage_input_rate;

        // Kaiser tap estimate for 60 dB stopband attenuation
        unsigned int taps = static_cast<unsigned int>(
            ceilf(52.0f / (14.36f * transition_norm)));
        taps = std::clamp(taps, 9u, 2047u);
        if ((taps % 2) == 0) taps++;

        vector<float> h(taps);
        liquid_firdes_kaiser(taps, cutoff_norm, 60.0f, 0.0f, h.data());

        vector<liquid_float_complex> hc(taps);
        for (unsigned int i = 0; i < taps; i++) {
            hc[i] = liquid_float_complex{h[i], 0.0f};
        }

        firdecim_cccf stage = firdecim_cccf_create(factor, hc.data(), taps);
        if (!stage) {
            cerr << "SharedDecimator: failed to create stage decimator (factor "
                 << factor << ", " << taps << " taps)" << endl;
            st.destroyStages();
            return false;
        }
        st.stages.push_back(stage);
        stage_input_rate = stage_output_rate;
    }
    return true;
}

// Per-channel state: only one pool task touches a given channel at a time
// (one task per channel per block, blocks sequential), so no locking needed.
SharedDecimator::ChannelDecimState* SharedDecimator::ensureChannelState(
    int channel, int decimation_factor, float freq_offset_hz) {

    ChannelDecimState& st = *channel_states_[channel];

    if (st.decimation_factor != decimation_factor) {
        // Factor changed - rebuild the cascade. Filter history and mixer
        // phase are only meaningful for unchanged settings.
        if (!buildStages(st, decimation_factor)) {
            st.decimation_factor = 0;
            return nullptr;
        }
        st.decimation_factor = decimation_factor;
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
        st.carry.clear();
    } else if (fabs(st.freq_offset_hz - freq_offset_hz) > 0.1f) {
        // Offset changed: the low-pass stages are offset-independent, so
        // only the mixer retunes (phase continuity is meaningless across a
        // retune). This also removes the old cache-quantization bug where a
        // filter built for one offset served requests up to 1 kHz away.
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
        st.carry.clear();  // mixed with the old offset
    }
    return &st;
}


// Bandwidth management methods
void SharedDecimator::setBandwidthIndex(int index) {
    if (isValidBandwidthIndex(index)) {
        current_bandwidth_index = index;
        cout << "Bandwidth set to: " << getBandwidthName() 
             << " (" << getBandwidthMhz() << " MHz, decimation=" << getDecimationFactor() << ")" << endl;
    }
}

int SharedDecimator::getDecimationFactor() const {
    return getBandwidthOptionSafe(current_bandwidth_index.load()).decimation_factor;
}

float SharedDecimator::getBandwidthMhz() const {
    return getBandwidthOptionSafe(current_bandwidth_index.load()).bandwidth_mhz;
}

const char* SharedDecimator::getBandwidthName() const {
    return getBandwidthOptionSafe(current_bandwidth_index.load()).display_name;
}

bool SharedDecimator::isValidBandwidthIndex(int index) const {
    return index >= 0 && index < NUM_BANDWIDTH_OPTIONS;
}

const BandwidthOption& SharedDecimator::getBandwidthOptionSafe(int index) const {
    if (isValidBandwidthIndex(index)) {
        return BANDWIDTH_OPTIONS[index];
    }
    return BANDWIDTH_OPTIONS[DEFAULT_BANDWIDTH_INDEX];
}

void SharedDecimator::setFrequencyOffset(float offset_hz) {
    current_freq_offset_hz = offset_hz;
}

float SharedDecimator::getOutputRate(int decimation_factor) const {
    if (decimation_factor <= 0) {
        decimation_factor = getDecimationFactor();
    }
    return SAMPLE_RATE / decimation_factor;
}

float SharedDecimator::getOutputBandwidth(int decimation_factor) const {
    return getOutputRate(decimation_factor) * 0.8f;
}

// Per-channel block decimation: input-rate mixer, then the stage cascade
SharedDecimator::DecimatedData SharedDecimator::decimateChannel(
    const complex<float>* iq_data, size_t num_samples, int channel_id,
    int decimation_factor, float freq_offset_hz) {

    DecimatedData result;
    result.channel_id = channel_id;

    if (decimation_factor <= 0) {
        decimation_factor = getDecimationFactor();
    }

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;

    if (!initialized.load() || channel_id < 0 || channel_id >= num_channels_initialized || !iq_data) {
        cerr << "SharedDecimator: Not initialized, invalid channel, or null data" << endl;
        return result;
    }

    if (fabs(freq_offset_hz) < 1.0f && fabs(current_freq_offset_hz.load()) > 1.0f) {
        freq_offset_hz = current_freq_offset_hz.load();
        result.freq_offset_hz = freq_offset_hz;
    }

    // Per-channel persistent mixer/filter state (see ensureChannelState)
    ChannelDecimState* st = ensureChannelState(channel_id, decimation_factor, freq_offset_hz);
    if (!st) {
        cerr << "SharedDecimator: Failed to build decimator for channel " << channel_id << endl;
        return result;
    }

    const size_t D = static_cast<size_t>(decimation_factor);

    // Transient scratch buffers - no cross-block state, so thread_local is
    // safe here even though worker<->channel mapping varies between blocks
    thread_local vector<complex<float>> mix_buffer;
    thread_local vector<complex<float>> head_buffer;
    thread_local vector<complex<float>> stage_buffer_a;
    thread_local vector<complex<float>> stage_buffer_b;

    const complex<float>* src = iq_data;

    // Mix the wanted band to baseband at the INPUT rate, before any
    // filtering. The oscillator runs over EVERY input sample and its phase
    // persists across blocks (st->mixer_phase) - a phase step at a block
    // boundary is audible as clicks in FM and contaminates MUSIC snapshots
    // that span blocks.
    if (fabs(freq_offset_hz) > 1.0f) {
        mix_buffer.resize(num_samples);

        float phase_increment = -2.0f * M_PI * freq_offset_hz / SAMPLE_RATE;
        float sin_val, cos_val;
        sincosf(phase_increment, &sin_val, &cos_val);
        complex<float> phase_step(cos_val, sin_val);
        complex<float> phase = st->mixer_phase;

        for (size_t i = 0; i < num_samples; i++) {
            mix_buffer[i] = iq_data[i] * phase;
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

        src = mix_buffer.data();
    }

    // Run n (a multiple of D) mixed samples through the cascade, APPENDING
    // the n / D outputs to result.samples. The FIR stages keep their history
    // between calls, so running a block in pieces equals running it whole.
    auto run_cascade = [&](const complex<float>* in, size_t n) {
        const size_t base = result.samples.size();
        if (st->stages.empty()) {
            // Decimation factor 1: mixer-only pass-through
            result.samples.insert(result.samples.end(), in, in + n);
            return;
        }
        // Ping-pong between scratch buffers; the last stage writes directly
        // into the result (std::complex<float> and liquid_float_complex
        // share memory layout)
        size_t cur_n = n;
        const complex<float>* cur_src = in;
        for (size_t s = 0; s < st->stages.size(); s++) {
            size_t out_n = cur_n / static_cast<size_t>(st->stage_factors[s]);
            complex<float>* dst;
            if (s + 1 == st->stages.size()) {
                result.samples.resize(base + out_n);
                dst = result.samples.data() + base;
            } else {
                vector<complex<float>>& buf = (s % 2 == 0) ? stage_buffer_a : stage_buffer_b;
                buf.resize(out_n);
                dst = buf.data();
            }

            firdecim_cccf_execute_block(
                st->stages[s],
                const_cast<liquid_float_complex*>(
                    reinterpret_cast<const liquid_float_complex*>(cur_src)),
                out_n,
                reinterpret_cast<liquid_float_complex*>(dst));

            cur_src = dst;
            cur_n = out_n;
        }
    };

    // Consume the previous block's leftover tail plus this block, D samples
    // at a time; whatever doesn't fill a whole D-chunk carries over. (Dropping
    // it, as before, spliced the signal and jumped the mixer phase at every
    // block boundary - an audible buzz on any off-center VFO whose factor
    // doesn't divide the block size.)
    result.samples.clear();
    result.samples.reserve((st->carry.size() + num_samples) / D);
    size_t pos = 0;
    if (!st->carry.empty()) {
        const size_t need = D - st->carry.size();
        if (num_samples < need) {
            st->carry.insert(st->carry.end(), src, src + num_samples);
            pos = num_samples;
        } else {
            // One D-chunk from the carry + the head of this block (a short
            // copy - at most D samples - instead of copying the whole block)
            head_buffer.assign(st->carry.begin(), st->carry.end());
            head_buffer.insert(head_buffer.end(), src, src + need);
            run_cascade(head_buffer.data(), D);
            st->carry.clear();
            pos = need;
        }
    }
    const size_t body = ((num_samples - pos) / D) * D;
    if (body > 0) {
        run_cascade(src + pos, body);
        pos += body;
    }
    if (pos < num_samples) {
        st->carry.insert(st->carry.end(), src + pos, src + num_samples);
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
    size_t num_channels, int decimation_factor, float freq_offset_hz) {

    MultiChannelDecimated result;

    if (decimation_factor <= 0) {
        decimation_factor = getDecimationFactor();
    }

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;

    if (!initialized.load() || num_channels == 0 || !channel_ptrs || !channel_lens) {
        return result;
    }

    if (fabs(freq_offset_hz) < 1.0f && fabs(current_freq_offset_hz.load()) > 1.0f) {
        freq_offset_hz = current_freq_offset_hz.load();
        result.freq_offset_hz = freq_offset_hz;
    }

    result.num_channels = min(num_channels, static_cast<size_t>(num_channels_initialized));
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
    // carried-over tail a block yields floor(N/D) or floor(N/D)+1 samples.
    // Every channel gets the same input length, so their carries - and
    // output counts - stay equal; min_samples is the common count.
    result.min_samples = SIZE_MAX;

    // HYBRID PARALLELIZATION: Launch parallel tasks for each antenna using ThreadPool
    vector<std::future<DecimatedData>> antenna_futures(result.num_channels);

    for (size_t ch = 0; ch < result.num_channels; ch++) {
        const complex<float>* ch_ptr = channel_ptrs[ch];
        antenna_futures[ch] = channel_thread_pool->submit(
            [this, ch, ch_ptr, min_samples, decimation_factor, freq_offset_hz]() -> DecimatedData {
                return decimateChannel(
                    ch_ptr,
                    min_samples,
                    static_cast<int>(ch),
                    decimation_factor,
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
    static size_t stats_counter = 0;
    if (++stats_counter % 1000 == 0) {
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
    int decimation_factor, float freq_offset_hz) {

    size_t n = min(channel_data.size(), static_cast<size_t>(MAX_CHANNELS));
    const complex<float>* ptrs[MAX_CHANNELS] = {nullptr};
    size_t lens[MAX_CHANNELS] = {0};
    for (size_t ch = 0; ch < n; ch++) {
        ptrs[ch] = channel_data[ch].data();
        lens[ch] = channel_data[ch].size();
    }
    return decimateMultiChannel(ptrs, lens, n, decimation_factor, freq_offset_hz);
}

SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    vector<vector<complex<float>>>&& channel_data,
    int decimation_factor, float freq_offset_hz) {

    return decimateMultiChannel(
        static_cast<const vector<vector<complex<float>>>&>(channel_data),
        decimation_factor, freq_offset_hz);
}