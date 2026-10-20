#include "utils/platform.hpp"
#include "message_builders.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/audio_mixer.hpp"
#include "signal_processing/beamformer.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "networking/binary_message.hpp"
#include "networking/gpsd_client.hpp"
#include "networking/web_mapper.hpp"
#include "station_info.hpp"
#include "channel_manager.hpp"
#include "doa_logger.hpp"
#include "utils/system_stats.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include "incidents.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <mutex>
#ifdef _WIN32
#include <filesystem>
#include <system_error>
#endif

using namespace std;
using namespace std::chrono;

extern DecimatorManager decimator_manager;

// Cached frequency arrays for scanner (avoid heap alloc every frame)
static std::vector<float> cached_wideband_freqs;
static float cached_wb_min_freq = 0, cached_wb_max_freq = 0;
static size_t cached_wb_size = 0;

static std::vector<float> cached_coherent_freqs;
static float cached_coh_center_freq = 0;
static size_t cached_coh_size = 0;
static int cached_coh_fft_size = 0;

// Normalize FFT data so noise floor = 0dB. Returns the noise floor value.
static float normalize_to_noise_floor(std::vector<float>& fft_data) {
    float noise_floor = FFTProcessor::calculate_noise_floor(fft_data);
    for (float& mag : fft_data) {
        mag -= noise_floor;
    }
    return noise_floor;
}

// Downsampling of the old-format frames (types 0 / 6 / 4, pages without
// spectrum views): SPECTRUM_LEGACY_POINTS per tuner band, as the removed
// "Downsampling" setting's default gave (8 at 16384)
static int legacy_fft_decimation() {
    return std::max(1, current_fft_size.load() / SPECTRUM_LEGACY_POINTS);
}

// The same for one tuner channel's spectrum: its floor as the squelch sees it
// - the receiver's remembered floor while a signal fills the band
static void normalize_to_channel_floor(std::vector<float>& fft_data, int channel) {
    const float noise_floor = FFTProcessor::channel_noise_floor(channel, fft_data);
    for (float& mag : fft_data) {
        mag -= noise_floor;
    }
}

string MessageBuilders::build_fft_message(SpectrumSnapshot* snap, bool legacy) {
    lock_guard<mutex> lock(fft_mutex);
    if (snap) {
        for (auto& d : snap->disp) d.valid = false;
        snap->independent = false;
    }

    BinaryMessage msg;
    msg.add_value(uint32_t(0)); // FFT message type

    // Check for wideband mode
    bool wideband_active = wideband_mode_enabled.load();

    if (!data_ready || num_channels == 0) {
        msg.add_value(uint32_t(0));
        // Get decimation from first decimator if available
        int decimation = 1;
        if (decimator_manager.getDecimatorCount() > 0) {
            auto dec = decimator_manager.getDecimator(0);
            if (dec && dec->decimator) {
                decimation = dec->decimator->getDecimationFactor();
            }
        }
        msg.add_value(static_cast<uint32_t>(decimation));
        msg.add_value(100.0f);
        msg.add_value(49.6f);
        msg.add_value(0.1f);
        msg.add_value(uint32_t(0));
        msg.add_value(uint32_t(0));
        return msg.to_string();
    }

    int channels = num_channels.load();
    int current_active = active_channel.load();

    if (current_active >= channels || current_active >= MAX_CHANNELS) {
        current_active = 0;
        active_channel = 0;
    }

    // Independent mode: one spectrum per tuner, all in one message (type 6):
    //   u32 6, u32 nch, f32 avg_alpha, f32 sample_rate_hz, f32 bin_step_hz,
    //   per tuner: u32 center_hz, f32 gain_db, u32 ds, u8 min[ds], u8 max[ds]
    // Bin i sits at center - sample_rate/2 + i*bin_step (decimate_fft_compressed)
    if (independent_mode_enabled.load()) {
        BinaryMessage im;
        im.add_value(uint32_t(6));
        const int nch = min(channels, MAX_CHANNELS);
        const int dec = legacy_fft_decimation();
        im.add_value(static_cast<uint32_t>(nch));
        im.add_value(averaging_alpha.load());
        im.add_value(static_cast<float>(SAMPLE_RATE));
        im.add_value(static_cast<float>(SAMPLE_RATE * dec / max(1, current_fft_size.load())));
        static std::vector<float> norm;
        for (int ch = 0; ch < nch; ch++) {
            im.add_value(static_cast<uint32_t>(exact_tuner_hz(ch)));
            im.add_value(ChannelManager::get_gain(ch));
            if (ch >= static_cast<int>(fft_averaged.size()) || fft_averaged[ch].empty()) {
                im.add_value(uint32_t(0));
                continue;
            }
            norm = fft_averaged[ch];
            normalize_to_channel_floor(norm, ch);
            if (snap && norm.size() >= 2) {
                SpectrumDisplay& d = snap->disp[1 + ch];
                d.valid = true;
                d.center_hz = static_cast<double>(exact_tuner_hz(ch));
                d.step_hz = static_cast<double>(SAMPLE_RATE) / static_cast<double>(norm.size());
                d.f0_hz = d.center_hz - static_cast<double>(SAMPLE_RATE) / 2.0;  // as decimate_fft_compressed
                d.gain_db = ChannelManager::get_gain(ch);
                d.db = norm;
            }
            if (!legacy) continue;
            CompressedDecimatedFFT c = FFTProcessor::decimate_fft_compressed(
                norm, exact_tuner_hz(ch) / 1e6f, dec);
            im.add_value(static_cast<uint32_t>(c.min_envelope.size()));
            im.add_vector(c.min_envelope);
            im.add_vector(c.max_envelope);
        }
        if (snap) {
            snap->independent = true;
            snap->channels = static_cast<uint8_t>(nch);
            snap->avg_alpha = averaging_alpha.load();
        }
        return legacy ? im.to_string() : string();
    }

    // Handle wideband mode
    if (wideband_active) {
        lock_guard<mutex> wb_lock(wideband_fft_mutex);

        // Stitch all channel FFTs together (wideband SCAN mode only - tuner
        // frequencies here are true tuner values, always within uint32)
        std::array<uint32_t, MAX_CHANNELS> tuner_freq_copy;
        for (int i = 0; i < MAX_CHANNELS; i++) {
            tuner_freq_copy[i] = static_cast<uint32_t>(tuner_frequencies[i].load());
        }

        float min_freq_mhz, max_freq_mhz;
        // CRITICAL FIX: Stitch from fft_magnitudes (raw FFT), NOT fft_averaged!
        // fft_averaged has already been averaged at the per-channel level.
        // If we stitch from that and average again at wideband level, we get DOUBLE averaging.
        // This causes signals to take 2x longer to converge with high averaging percentages.
        FFTProcessor::stitch_wideband_fft(
            fft_magnitudes,  // Use RAW FFT data, not averaged!
            tuner_freq_copy,
            channels,
            wideband_fft_magnitudes,
            min_freq_mhz,
            max_freq_mhz
        );

        // If stitching failed (empty result), fall back to normal mode display
        if (!wideband_fft_magnitudes.empty()) {
            // CRITICAL: Check if FFT data is valid (not reset values)
            // After reset, fft_data_valid is set to false. It's set to true when
            // the FFT processor writes the first valid frame. This prevents us from
            // consuming the reset with stale/reset data.
            bool data_is_valid = fft_data_valid.load(std::memory_order_acquire);

            if (!data_is_valid) {
                // Skip this frame - wait for real FFT data from FFT processor
                // Don't update wideband_last_reset_gen - preserve reset for next frame
                // Fall through to normal mode display below
            } else {
                // Apply averaging to wideband FFT with generation counter support
                if (wideband_fft_averaged.size() != wideband_fft_magnitudes.size()) {
                    wideband_fft_averaged.resize(wideband_fft_magnitudes.size());
                    std::copy(wideband_fft_magnitudes.begin(), wideband_fft_magnitudes.end(),
                              wideband_fft_averaged.begin());
                } else {
                    // EXPONENTIAL MOVING AVERAGE (EMA) with generation-based reset
                    // On reset: alpha=1.0 → direct copy of raw FFT to averaged buffer
                    // This initializes the EMA with the first valid frame's data
                    uint32_t current_gen = fft_reset_generation.load(std::memory_order_acquire);
                    uint32_t last_gen = wideband_last_reset_gen.load(std::memory_order_acquire);
                    bool should_reset = (last_gen < current_gen);
                    float alpha = should_reset ? 1.0f : averaging_alpha.load();

                    if (should_reset) {
                        wideband_last_reset_gen.store(current_gen, std::memory_order_release);
                    }

                    // Apply EMA: alpha=1.0 on reset means fft_averaged = raw (direct copy)
                    for (size_t i = 0; i < wideband_fft_magnitudes.size(); i++) {
                        wideband_fft_averaged[i] = alpha * wideband_fft_magnitudes[i] +
                                                   (1.0f - alpha) * wideband_fft_averaged[i];
                    }
                }

                static std::vector<float> normalized_wideband;
                normalized_wideband = wideband_fft_averaged;
                normalize_to_noise_floor(normalized_wideband);

                // Feed normalized FFT data to discrete scanner for signal detection
                if (scanner_manager.isRunning()) {
                    // Rebuild frequency array only when parameters change
                    size_t wb_size = normalized_wideband.size();
                    if (wb_size != cached_wb_size || min_freq_mhz != cached_wb_min_freq || max_freq_mhz != cached_wb_max_freq) {
                        cached_wb_size = wb_size;
                        cached_wb_min_freq = min_freq_mhz;
                        cached_wb_max_freq = max_freq_mhz;
                        cached_wideband_freqs.resize(wb_size);
                        float freq_step_mhz = (max_freq_mhz - min_freq_mhz) / static_cast<float>(wb_size);
                        for (size_t i = 0; i < wb_size; i++) {
                            cached_wideband_freqs[i] = min_freq_mhz + (i * freq_step_mhz);
                        }
                    }
                    scanner_manager.updateFFTData(normalized_wideband, cached_wideband_freqs);
                }

                if (snap && normalized_wideband.size() >= 2) {
                    // bin i at min + i / size * (max - min), as decimate_and_compress_wideband
                    SpectrumDisplay& d = snap->disp[0];
                    d.valid = true;
                    d.f0_hz = static_cast<double>(min_freq_mhz) * 1e6;
                    d.step_hz = (static_cast<double>(max_freq_mhz) - min_freq_mhz) * 1e6 /
                                static_cast<double>(normalized_wideband.size());
                    d.center_hz = (static_cast<double>(min_freq_mhz) + max_freq_mhz) / 2.0 * 1e6;
                    d.gain_db = ChannelManager::get_gain(0);
                    d.db = normalized_wideband;
                    snap->marker = 0xFE;
                    snap->channels = static_cast<uint8_t>(channels);
                    snap->avg_alpha = averaging_alpha.load();
                }
                if (!legacy) return string();

                // Decimate and compress the wideband FFT (old-format frames)
                const int wideband_decimation = legacy_fft_decimation();
                CompressedFFT compressed = FFTProcessor::decimate_and_compress_wideband(
                    normalized_wideband,
                    min_freq_mhz,
                    max_freq_mhz,
                    wideband_decimation
                );

                // Build message with compressed wideband data
                msg.add_value(static_cast<uint32_t>(channels));
                msg.add_value(uint32_t(1)); // Decimation (not applicable in wideband mode)
                msg.add_value((min_freq_mhz + max_freq_mhz) / 2.0f); // Center frequency
                msg.add_value(ChannelManager::get_gain(0)); // Use first channel gain
                msg.add_value(averaging_alpha.load());
                msg.add_value(uint32_t(0xFE)); // Special marker for compressed wideband mode (0xFE instead of 0xFF)

                msg.add_value(static_cast<uint32_t>(compressed.frequencies.size()));

                // Add compressed data (frequencies as float32, magnitudes as uint8)
                msg.add_vector(compressed.frequencies);
                msg.add_vector(compressed.min_envelope);  // uint8 compressed
                msg.add_vector(compressed.max_envelope);  // uint8 compressed

                return msg.to_string();
            }
            // If data_is_reset_values was true, fall through to normal mode display
        } else {
            // Stitching failed - fall back to normal mode
            static int empty_warning_counter = 0;
            if ((empty_warning_counter++ % 50) == 0) {
                cerr << "WARNING: Wideband FFT stitching produced empty result! Falling back to single-channel display." << endl;
                cerr << "  Check that FFT data is available for all channels." << endl;
            }
            // Fall through to normal mode processing below
        }
    }
    
    // Get decimation from first decimator
    int decimation = 1;
    if (decimator_manager.getDecimatorCount() > 0) {
        auto dec = decimator_manager.getDecimator(0);
        if (dec && dec->decimator) {
            decimation = dec->decimator->getDecimationFactor();
        }
    }

    if (current_active >= fft_averaged.size() || fft_averaged[current_active].empty()) {
        msg.add_value(static_cast<uint32_t>(channels));
        msg.add_value(static_cast<uint32_t>(decimation));
        msg.add_value(ChannelManager::get_frequency(current_active) / 1e6f);
        msg.add_value(ChannelManager::get_gain(current_active));
        msg.add_value(averaging_alpha.load());
        msg.add_value(static_cast<uint32_t>(current_active));
        msg.add_value(uint32_t(0));
        return msg.to_string();
    }

    msg.add_value(static_cast<uint32_t>(channels));
    msg.add_value(static_cast<uint32_t>(decimation));

    float active_freq_mhz = ChannelManager::get_frequency(current_active) / 1e6f;
    float active_gain_db = ChannelManager::get_gain(current_active);

    msg.add_value(active_freq_mhz);
    msg.add_value(active_gain_db);
    msg.add_value(averaging_alpha.load());

    // Use bit 7 (0x80) to indicate compressed format
    // Compressed channel: 0x80 | channel_id (128-135 for channels 0-7)
    // Uncompressed channel: channel_id (0-7)
    // Wideband compressed: 0xFE (254)
    // Wideband uncompressed: 0xFF (255)
    uint32_t channel_marker = 0x80 | current_active;  // Compressed single-channel format
    msg.add_value(channel_marker);

    float center_freq_mhz = active_freq_mhz;

    // Normalize noise floor to 0dB for consistent display and squelch detection
    static std::vector<float> normalized_fft;
    normalized_fft = fft_averaged[current_active];
    normalize_to_channel_floor(normalized_fft, current_active);

    // SIGNAL MONITORING IN LOCKED STATE:
    // Feed normalized coherent FFT to scanner to monitor signal presence
    // Scanner will automatically resume scanning if signal drops below squelch
    if (scanner_manager.isRunning() && scanner_manager.getState() == ScannerState::LOCKED) {
        // Rebuild coherent frequency array only when parameters change
        size_t coh_size = normalized_fft.size();
        int coh_fft_size = current_fft_size.load();
        if (coh_size != cached_coh_size || center_freq_mhz != cached_coh_center_freq || coh_fft_size != cached_coh_fft_size) {
            cached_coh_size = coh_size;
            cached_coh_center_freq = center_freq_mhz;
            cached_coh_fft_size = coh_fft_size;
            cached_coherent_freqs.resize(coh_size);
            float freq_step_mhz = (SAMPLE_RATE / static_cast<float>(coh_fft_size)) / 1e6f;
            float start_freq_mhz = center_freq_mhz - (SAMPLE_RATE / 2.0f) / 1e6f;
            for (size_t i = 0; i < coh_size; i++) {
                cached_coherent_freqs[i] = start_freq_mhz + (i * freq_step_mhz);
            }
        }
        scanner_manager.updateFFTData(normalized_fft, cached_coherent_freqs);
    }

    if (snap && normalized_fft.size() >= 2) {
        // the exact centre (the header's float MHz is ~30 Hz coarse at 400 MHz,
        // more than a bin at full zoom)
        SpectrumDisplay& d = snap->disp[0];
        const uint64_t hz = coherent_tuner_hz();
        d.valid = true;
        d.center_hz = hz ? static_cast<double>(hz)
                         : static_cast<double>(ChannelManager::get_frequency(current_active));
        d.step_hz = static_cast<double>(SAMPLE_RATE) / static_cast<double>(normalized_fft.size());
        d.f0_hz = d.center_hz - static_cast<double>(SAMPLE_RATE) / 2.0;  // as decimate_fft_compressed
        d.gain_db = active_gain_db;
        d.db = normalized_fft;
        snap->marker = static_cast<uint8_t>(0x80 | current_active);
        snap->channels = static_cast<uint8_t>(channels);
        snap->avg_alpha = averaging_alpha.load();
    }
    if (!legacy) return string();

    CompressedDecimatedFFT compressed = FFTProcessor::decimate_fft_compressed(normalized_fft, center_freq_mhz, legacy_fft_decimation());

    msg.add_value(static_cast<uint32_t>(compressed.frequencies.size()));

    // Add compressed data (frequencies as float32, magnitudes as uint8)
    msg.add_vector(compressed.frequencies);
    msg.add_vector(compressed.min_envelope);  // uint8 compressed
    msg.add_vector(compressed.max_envelope);  // uint8 compressed

    return msg.to_string();
}

// Spectrum view (type 7), little-endian, 64-byte header:
//   u32 7 | u8 display (0 main, 1 + ch tuner) | u8 marker (0x80|ch coherent,
//   0xFE wideband scan, ch independent) | u8 channels | u8 flags (1 = the
//   requested window, else the whole band) | u32 view seq |
//   f64 centre Hz | f64 band lo Hz | f64 band hi Hz (first / last bin of the
//   whole spectrum) | f64 start Hz (point 0) | f64 step Hz | f32 gain dB |
//   f32 averaging alpha | u32 n | u8 level[n] (0..255 = 0..120 dB, max-pooled)
// No per-point frequencies (the old type 0 sent a float32 for every point:
// two thirds of it) and no min envelope (no page ever drew it).
static SpectrumSnapshot g_spectrum_snapshot;
static std::atomic<int64_t> g_spectrum_ms{0};
static std::atomic<double> g_spectrum_center_hz{0};

SpectrumSnapshot& MessageBuilders::spectrum_snapshot() { return g_spectrum_snapshot; }

void MessageBuilders::note_spectrum() {
    const SpectrumDisplay& d = g_spectrum_snapshot.disp[0];
    if (!g_spectrum_snapshot.independent && d.valid) g_spectrum_center_hz.store(d.center_hz);
    g_spectrum_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
}

int64_t MessageBuilders::spectrum_ms() { return g_spectrum_ms.load(); }
double MessageBuilders::spectrum_center_hz() { return g_spectrum_center_hz.load(); }

string MessageBuilders::build_spectrum_view(const SpectrumSnapshot& snap, int display, double lo_off_hz,
                                            double hi_off_hz, uint32_t bins, uint32_t seq,
                                            double* point_step_hz) {
    if (display < 0 || display > MAX_CHANNELS) return string();
    const SpectrumDisplay& d = snap.disp[display];
    const size_t L = d.db.size();
    if (!d.valid || L < 2 || !(d.step_hz > 0)) return string();

    // bins whose centres lie in the window (at least 2), else the whole band
    size_t i0 = 0, i1 = L - 1;
    bool windowed = false;
    if (hi_off_hz > lo_off_hz) {
        const double a = std::ceil((d.center_hz + lo_off_hz - d.f0_hz) / d.step_hz - 1e-6);
        const double b = std::floor((d.center_hz + hi_off_hz - d.f0_hz) / d.step_hz + 1e-6);
        const double last = static_cast<double>(L - 1);
        const double ca = std::clamp(a, 0.0, last), cb = std::clamp(b, 0.0, last);
        if (cb - ca >= 1.0) {
            i0 = static_cast<size_t>(ca);
            i1 = static_cast<size_t>(cb);
            windowed = true;
        }
    }
    const size_t count = i1 - i0 + 1;
    // points: what the page asks for (its plot's width in device pixels),
    // never more than SPECTRUM_VIEW_MAX_POINTS - zoomed in, the same number
    // of points cover less spectrum (down to one FFT bin each)
    const size_t budget = SPECTRUM_VIEW_MAX_POINTS;
    const size_t want = std::clamp<size_t>(bins ? bins : SPECTRUM_LEGACY_POINTS, 16, budget);
    // the pool nearest to `want` points (0.67 - 1.5x of it: rounding up made
    // 819 bins in an 816-pixel plot 410 points - half the resolution), never
    // past the budget
    const size_t pool = std::max({size_t(1), (count + budget - 1) / budget, (count + want / 2) / want});
    const size_t n = (count + pool - 1) / pool;
    if (point_step_hz) *point_step_hz = d.step_hz * static_cast<double>(pool);

    string out;
    out.resize(64 + n);
    char* p = out.data();
    auto put = [&p](const auto& v) { std::memcpy(p, &v, sizeof v); p += sizeof v; };
    put(uint32_t(7));
    put(static_cast<uint8_t>(display));
    put(display == 0 ? snap.marker : static_cast<uint8_t>(display - 1));
    put(snap.channels);
    put(static_cast<uint8_t>(windowed ? 1 : 0));
    put(seq);
    put(d.center_hz);
    put(d.f0_hz);
    put(d.f0_hz + static_cast<double>(L - 1) * d.step_hz);
    // a point stands for its pooled bins: at their middle
    put(d.f0_hz + (static_cast<double>(i0) + (static_cast<double>(pool) - 1.0) / 2.0) * d.step_hz);
    put(d.step_hz * static_cast<double>(pool));
    put(d.gain_db);
    put(snap.avg_alpha);
    put(static_cast<uint32_t>(n));
    // max of each pool (keeps narrow peaks), quantised as compress_magnitudes
    constexpr float SCALE = 255.0f / 120.0f;
    const float* src = d.db.data();
    for (size_t j = 0; j < n; j++) {
        const size_t a = i0 + j * pool, b = std::min(a + pool, i1 + 1);
        float m = src[a];
        for (size_t k = a + 1; k < b; k++) m = std::max(m, src[k]);
        const int q = static_cast<int>(m * SCALE + 0.5f);
        *p++ = static_cast<char>(static_cast<uint8_t>(std::clamp(q, 0, 255)));
    }
    return out;
}

string MessageBuilders::build_audio_message() {
    if (!fm_enabled.load() || !audio_mixer.is_processing_enabled()) {
        return string();
    }

    // every VFO heard, mixed (audio_mixer.hpp)
    auto opus_packet = audio_mixer.get_opus_packet(AUDIO_SAMPLES_PER_PACKET);
    if (opus_packet.empty()) {
        return string();
    }

    BinaryMessage msg;
    msg.add_value(uint32_t(1)); // Audio message type
    msg.add_value(static_cast<uint32_t>(AUDIO_SAMPLES_PER_PACKET));  // Original sample count
    msg.add_value(AUDIO_SAMPLE_RATE);  // Sample rate
    msg.add_value(uint32_t(opus_packet.size()));  // Opus packet size

    // Add Opus packet data
    msg.add_vector(opus_packet);

    return msg.to_string();
}

string MessageBuilders::build_multi_doa_message() {
    if (!doa_enabled.load()) {
        return string();
    }

    auto all_decimators = decimator_manager.getAllDecimators();
    if (all_decimators.empty()) {
        return string();
    }

    // Build decimator data first, then prepend count
    // This avoids count mismatch if some decimators are skipped during transitions
    struct DecimatorData {
        uint32_t id;
        float freq_offset_khz;
        uint32_t bandwidth_index;
        uint32_t num_angles;
        float array_radius;
        uint32_t blocks_processed;
        float resolution;
        vector<float> spectrum;
        // 2D MUSIC elevation data (for 3D arrays)
        bool is_3d;
        uint32_t num_elevation_angles;
        float elevation_resolution;
        int peak_elevation;
        vector<float> elevation_spectrum;
    };
    vector<DecimatorData> valid_decimators;

    for (const auto& decimator_inst : all_decimators) {
        if (!decimator_inst || !decimator_inst->music_processor || decimator_inst->being_deleted.load()) continue;

        try {
            auto spectrum = decimator_inst->music_processor->getPseudospectrum();
            int num_angles = decimator_inst->music_processor->getNumAngles();
            float resolution = decimator_inst->music_processor->getAngularResolution();

            // Skip if spectrum size doesn't match expected (resolution transition in progress)
            if (spectrum.size() != num_angles || num_angles <= 0) {
                continue;
            }

            DecimatorData data;
            data.id = decimator_inst->id;
            data.freq_offset_khz = decimator_inst->frequency_offset_hz / 1000.0f;
            data.bandwidth_index = decimator_inst->bandwidth_index;
            data.num_angles = num_angles;
            data.array_radius = decimator_inst->music_processor->getArrayRadius();
            data.blocks_processed = decimator_inst->music_processor->getBlocksProcessed();
            data.resolution = resolution;
            data.spectrum.resize(spectrum.size());
            for (int i = 0; i < spectrum.size(); i++) {
                data.spectrum[i] = static_cast<float>(spectrum(i));
            }

            // Get 2D MUSIC elevation data if 3D array
            data.is_3d = decimator_inst->music_processor->is3DArray();
            data.num_elevation_angles = 0;
            data.elevation_resolution = 1.0f;
            data.peak_elevation = 0;
            if (data.is_3d) {
                auto el_spectrum = decimator_inst->music_processor->getElevationPseudospectrum();
                data.num_elevation_angles = decimator_inst->music_processor->getNumElevationAngles();
                data.elevation_resolution = decimator_inst->music_processor->getElevationResolution();
                auto [az, el] = decimator_inst->music_processor->getPeakAzimuthElevation();
                data.peak_elevation = el;
                data.elevation_spectrum.resize(el_spectrum.size());
                for (size_t i = 0; i < static_cast<size_t>(el_spectrum.size()); i++) {
                    data.elevation_spectrum[i] = static_cast<float>(el_spectrum(i));
                }
            }

            valid_decimators.push_back(std::move(data));
        } catch (const std::exception& e) {
            continue;
        }
    }

    if (valid_decimators.empty()) {
        return string();
    }

    // Now build the message with correct count
    BinaryMessage msg;
    msg.add_value(uint32_t(3)); // Multi-DoA message type
    msg.add_value(static_cast<uint32_t>(valid_decimators.size()));

    for (const auto& data : valid_decimators) {
        msg.add_value(data.id);
        msg.add_value(data.freq_offset_khz);
        msg.add_value(data.bandwidth_index);
        msg.add_value(data.num_angles);
        msg.add_value(data.array_radius);
        msg.add_value(data.blocks_processed);
        msg.add_value(data.resolution);
        msg.add_vector(data.spectrum);
        // Add 2D MUSIC elevation data
        msg.add_value(data.is_3d ? uint32_t(1) : uint32_t(0));
        if (data.is_3d) {
            msg.add_value(data.num_elevation_angles);
            msg.add_value(data.elevation_resolution);
            msg.add_value(static_cast<int32_t>(data.peak_elevation));
            msg.add_vector(data.elevation_spectrum);
        }
    }

    return msg.to_string();
}

string MessageBuilders::build_system_status_message() {
    extern ScannerManager scanner_manager;

    uint32_t phase_state = scanner_manager.getPhaseState();

    stringstream json;
    json << "{\"system_status\":{";
    json << "\"wideband\":" << (wideband_mode_enabled.load() ? "true" : "false");
    json << ",\"independent\":" << (independent_mode_enabled.load() ? "true" : "false");
    json << ",\"phase_state\":" << phase_state;
    json << ",\"noise_source\":" << (scanner_manager.isNoiseSourceActive() ? "true" : "false");
    json << ",\"active_elements\":" << active_num_elements.load();
    json << ",\"platform\":\"" << platform::os_name() << "\"";   // the UI's per-OS instructions
#ifdef _WIN32
    // running from the source folder (kraken_doa_v2/Makefile in the cwd) or
    // from a package (windows/package.sh: no sources, Makefile or MSYS2) -
    // the UI's build steps differ. Once: the cwd doesn't change
    static const bool source_tree = [] {
        std::error_code ec;
        return std::filesystem::is_regular_file("Makefile", ec);
    }();
    json << ",\"source_tree\":" << (source_tree ? "true" : "false");
#endif

    // KerberosSDR support mode (heimdall --kerberos): calibration is manual,
    // so the UI must warn when the server is uncalibrated or the calibration
    // went stale after a settings change. State derived from the masked phase
    // state + the header flag bits (see data_receiver.cpp).
    json << ",\"kerberos\":" << (server_kerberos_mode.load(std::memory_order_relaxed) ? "true" : "false");
    if (server_kerberos_mode.load(std::memory_order_relaxed)) {
        const char* cal_state;
        if (phase_state == 4) {  // CONVERGED
            cal_state = server_cal_stale.load(std::memory_order_relaxed) ? "stale" : "calibrated";
        } else if (scanner_manager.isNoiseSourceActive()) {
            cal_state = "calibrating";
        } else {
            cal_state = "uncalibrated";
        }
        json << ",\"cal_state\":\"" << cal_state << "\"";
    }

    // Per-decimator squelch states (+ MUSIC's auto source estimate)
    json << ",\"decimator_squelch\":[";
    auto info_list = decimator_manager.getDecimatorInfoList();
    bool first = true;
    for (const auto& info : info_list) {
        if (!first) json << ",";
        first = false;
        int est_sources = -1;    // -1 = auto mode off / no estimate
        auto decimator = decimator_manager.getDecimator(info.id);
        if (decimator && decimator->music_processor) est_sources = decimator->music_processor->getEstimatedNumSources();
        json << "{\"id\":" << info.id
             << ",\"open\":" << (info.squelch_open ? "true" : "false")
             << ",\"est_sources\":" << est_sources << "}";
    }
    json << "]";

    // Include hardware stats (CPU temp, CPU usage, RAM)
    auto hw = get_hardware_stats();
    json << ",\"hw\":{";
    json << "\"cpu_temp\":" << fixed << setprecision(1) << hw.cpu_temp_c;
    json << ",\"cpu_pct\":" << fixed << setprecision(1) << hw.cpu_usage_pct;
    json << ",\"ram_used\":" << fixed << setprecision(0) << hw.ram_used_mb;
    json << ",\"ram_total\":" << fixed << setprecision(0) << hw.ram_total_mb;
    json << "}";

    // Include GPS status (from gpsd). Field names lat/lon/heading/speed/timestamp
    // are part of the API contract for downstream consumers.
    GpsFix gps = gps_client.get();
    json << ",\"gps\":{";
    json << "\"connected\":" << (gps.connected ? "true" : "false");
    json << ",\"device\":" << (gps.device_present ? "true" : "false");
    json << ",\"fix\":\"" << gps.fix_str() << "\"";
    json << ",\"mode\":" << gps.mode;
    json << ",\"lat\":" << fixed << setprecision(7) << gps.lat;
    json << ",\"lon\":" << setprecision(7) << gps.lon;
    json << ",\"alt\":" << setprecision(1) << gps.alt;
    json << ",\"speed\":" << setprecision(2) << gps.speed;
    json << ",\"heading\":" << setprecision(1) << gps.heading();
    json << ",\"heading_source\":\"" << gps.heading_source() << "\"";
    json << ",\"has_compass\":" << (gps.has_compass ? "true" : "false");
    json << ",\"compass_heading\":" << setprecision(1) << gps.compass_heading;
    json << ",\"track\":" << setprecision(1) << gps.track;
    json << ",\"track_valid\":" << (gps.track_valid ? "true" : "false");
    json << ",\"sats_used\":" << gps.sats_used;
    json << ",\"sats_visible\":" << gps.sats_visible;
    json << ",\"timestamp\":" << gps.timestamp_ms;
    json << "}";

    // Station identity + effective location (callsign and the lat/lon/heading
    // actually reported downstream, resolved from the selected location source:
    // mobile (0's), static (manual), or gps (live fix)).
    StationLocation sl = station_info.resolve();
    json << ",\"station\":{";
    json << "\"id\":\"" << json_escape(sl.id) << "\"";
    json << ",\"source\":\"" << sl.source_str() << "\"";
    json << ",\"lat\":" << fixed << setprecision(7) << sl.lat;
    json << ",\"lon\":" << setprecision(7) << sl.lon;
    json << ",\"heading\":" << setprecision(1) << sl.heading;
    json << ",\"heading_source\":\"" << sl.heading_source_str() << "\"";
    json << ",\"heading_valid\":" << (sl.heading_valid ? "true" : "false");
    json << ",\"speed\":" << setprecision(2) << sl.speed;
    json << ",\"timestamp\":" << sl.timestamp_ms;
    json << "}";

    // Local DoA recording status (running / path / file size / entries).
    std::string rec_json;
    doa_logger.appendStatusJson(rec_json);   // -> "recording":{...}
    json << "," << rec_json;

    // Web mapper output status (mode / connection state / record count).
    std::string wm_json;
    web_mapper.appendStatusJson(wm_json);    // -> "web_mapper":{...}
    json << "," << wm_json;

    // Include beamforming status (reported for the first VFO heard - the
    // audible/primary one, else the first VFO; mode/MVDR are shared across all
    // decimators).
    int bf_fm_dec_id = decimator_manager.primaryAudioVfo();
    if (bf_fm_dec_id < 0) {
        const auto all = decimator_manager.getAllDecimators();
        if (!all.empty() && all.front()) bf_fm_dec_id = all.front()->id;
    }
    auto bf_fm_inst = decimator_manager.getDecimator(bf_fm_dec_id);
    json << "},\"beamforming_status\":{";
    json << "\"enabled\":" << (beamforming_enabled.load() ? "true" : "false");
    if (beamforming_enabled.load() && bf_fm_inst && bf_fm_inst->beamformer) {
        auto stats = bf_fm_inst->beamformer->getStats();
        json << ",\"steering_angle\":" << stats.current_steering_angle;
        json << ",\"snr_boost_db\":" << stats.estimated_snr_improvement_db;

        // Mode string
        const char* mode_str = "DAS";
        if (stats.current_mode == BeamformingMode::MVDR) mode_str = "MVDR";
        else if (stats.current_mode == BeamformingMode::SELECTION_DIVERSITY) mode_str = "DIVERSITY";
        else if (stats.current_mode == BeamformingMode::FREQ_DOMAIN_DAS) mode_str = "FREQ_DAS";
        json << ",\"mode\":\"" << mode_str << "\"";

        json << ",\"mvdr_condition\":" << stats.mvdr_condition_number;

        // Selection diversity stats
        json << ",\"selected_channel\":" << stats.selected_channel;
        json << ",\"selected_channel_snr\":" << stats.selected_channel_snr;
        json << ",\"channel_snrs\":[";
        for (int i = 0; i < DOA_NUM_ELEMENTS; i++) {
            if (i > 0) json << ",";
            json << stats.channel_snrs[i];
        }
        json << "]";

        // Get confidence from the FM decimator's MUSIC processor
        float confidence = 0.0f;
        if (bf_fm_inst && bf_fm_inst->music_processor) {
            confidence = bf_fm_inst->music_processor->getPeakAngleWithConfidence().second;
        }
        json << ",\"confidence\":" << confidence;
    }
    json << "}}";

    return json.str();
}

bool MessageBuilders::beamformed_spectrum(int decimator_id, BeamformedSpectrum& out) {
    // Only send if beamforming is enabled
    if (!beamforming_enabled.load()) {
        return false;
    }

    auto inst = decimator_manager.getDecimator(decimator_id);
    if (!inst || !inst->beamformer) {
        return false;
    }
    BeamformedFFTData& bf = inst->beamformed_fft;

    if (!bf.valid.load(std::memory_order_acquire)) {
        return false;
    }

    // Copy this decimator's averaged FFT under its own lock, then release.
    {
        lock_guard<mutex> lock(bf.mutex);
        if (bf.averaged.empty()) {
            return false;
        }
        out.db = bf.averaged;
    }

    out.id = static_cast<uint32_t>(decimator_id);
    out.center_mhz = bf.center_freq_hz.load(std::memory_order_relaxed) / 1e6f;
    out.bandwidth_mhz = bf.bandwidth_hz.load(std::memory_order_relaxed) / 1e6f;

    // Get this decimator's beamformer stats for SNR boost display
    auto stats = inst->beamformer->getStats();
    out.steering_deg = stats.current_steering_angle;
    out.snr_boost_db = stats.estimated_snr_improvement_db;

    // Normalize beamformed FFT to noise floor = 0dB
    // Note: can't use normalize_to_noise_floor() here because FFTProcessor::calculate_noise_floor()
    // checks against current_fft_size which is the main FFT size, not the beamformed slice size.
    {
        static thread_local vector<float> bf_sorted;
        bf_sorted = out.db;
        size_t p10 = bf_sorted.size() / 10;
        std::nth_element(bf_sorted.begin(), bf_sorted.begin() + p10, bf_sorted.end());
        float noise_floor = bf_sorted[p10];
        for (float& mag : out.db) {
            mag -= noise_floor;
        }
    }
    return true;
}

string MessageBuilders::build_beamformed_fft_message(const BeamformedSpectrum& bf, double point_step_hz) {
    const size_t input_size = bf.db.size();
    if (input_size == 0 || !(bf.bandwidth_mhz > 0)) return string();

    // The main spectrum's point spacing: the page's (its zoom), else the
    // old-format frames' (SPECTRUM_LEGACY_POINTS over a tuner's band)
    if (!(point_step_hz > 0))
        point_step_hz = static_cast<double>(SAMPLE_RATE) * legacy_fft_decimation() /
                        std::max(1, current_fft_size.load());
    // Max-pool bins of the overlay (preserves peaks) to that spacing - a power
    // of two, so the points stay evenly spaced over the band (the page places
    // point i at centre - bw / 2 + i * bw / n), at least 16 points
    const double bin_hz = static_cast<double>(bf.bandwidth_mhz) * 1e6 / static_cast<double>(input_size);
    size_t pool = 1;
    while (pool * 2 * bin_hz <= point_step_hz && input_size / (pool * 2) >= 16) pool *= 2;
    const size_t output_size = (input_size + pool - 1) / pool;

    BinaryMessage msg;
    msg.add_value(uint32_t(4)); // Beamformed FFT message type
    msg.add_value(bf.id);       // which decimator this overlay is for
    msg.add_value(bf.center_mhz);
    msg.add_value(bf.bandwidth_mhz);
    msg.add_value(static_cast<uint32_t>(output_size));
    msg.add_value(bf.steering_deg);
    msg.add_value(bf.snr_boost_db);

    // Compress magnitudes to uint8 for bandwidth savings
    // Map 0dB to 120dB range to 0-255
    vector<uint8_t> compressed;
    compressed.reserve(output_size);
    constexpr float SCALE = 255.0f / 120.0f;
    for (size_t i = 0; i < output_size; i++) {
        const size_t a = i * pool, b = min(a + pool, input_size);
        float m = bf.db[a];
        for (size_t j = a + 1; j < b; j++) m = max(m, bf.db[j]);
        const int q = static_cast<int>(m * SCALE + 0.5f);
        compressed.push_back(static_cast<uint8_t>(std::clamp(q, 0, 255)));
    }

    // Add compressed FFT data
    msg.add_vector(compressed);

    return msg.to_string();
}

string MessageBuilders::build_decimator_info_message() {
    auto info_list = decimator_manager.getDecimatorInfoList();

    // In wideband mode, convert offsets to be relative to wideband center for UI
    bool wideband = wideband_mode_enabled.load();
    float wideband_center_hz = 100e6f;  // Default

    if (wideband) {
        int nc = num_channels.load();
        if (nc > 0) {
            float min_tuner_freq = ChannelManager::get_frequency(0);
            float max_tuner_freq = ChannelManager::get_frequency(nc - 1);
            wideband_center_hz = (min_tuner_freq + max_tuner_freq) / 2.0f;
        }
    }

    stringstream json;
    json << "{\"decimator_info\":[";
    bool first = true;

    for (auto& info : info_list) {
        // Convert offset to wideband-center-relative for UI
        float ui_offset_hz = info.frequency_offset_hz;

        if (wideband) {
            // Get this decimator's tuner channel
            auto decimator_inst = decimator_manager.getDecimator(info.id);
            if (decimator_inst) {
                int tuner_ch = decimator_inst->tuner_channel;
                float tuner_freq_hz = ChannelManager::get_frequency(tuner_ch);

                // Convert: tuner_relative → absolute → wideband_center_relative
                float absolute_freq = tuner_freq_hz + info.frequency_offset_hz;
                ui_offset_hz = absolute_freq - wideband_center_hz;
            }
        }

        if (!first) json << ",";
        first = false;

        // The VFO's tuner (wideband / independent mode); in independent mode
        // freq_offset_hz is relative to that tuner's frequency
        int vfo_tuner = 0;
        if (auto di = decimator_manager.getDecimator(info.id)) vfo_tuner = di->tuner_channel.load();
        json << "{\"id\":" << info.id
             << ",\"freq_offset_hz\":" << ui_offset_hz  // Send UI-friendly offset
             << ",\"tuner\":" << vfo_tuner
             << ",\"bandwidth_index\":" << info.bandwidth_index
             << ",\"bandwidth_mhz\":" << info.bandwidth_mhz
             << ",\"enabled\":" << (info.enabled ? "true" : "false")
             << ",\"audio\":" << (info.audio_out ? "true" : "false")
             // the old single "Audio Src" (pages from before the speaker toggles)
             << ",\"is_fm_source\":" << (info.audio_out ? "true" : "false")
             << ",\"demod_mode\":\"" << DecimatorManager::demodModeToString(info.demod_mode) << "\""
             << ",\"squelch_enabled\":" << (info.squelch_enabled ? "true" : "false")
             << ",\"squelch_level\":" << info.squelch_level
             << ",\"squelch_open\":" << (info.squelch_open ? "true" : "false")
             << ",\"squelch_method\":\"" << (info.squelch_method == static_cast<int>(SquelchMethod::DIGITAL) ? "DIGITAL" : "FFT") << "\""
             << ",\"transcribe\":" << (info.transcribe ? "true" : "false")
             << ",\"digital_mode\":\"" << json_escape(dig::mode_string(static_cast<dig::Mode>(info.digital_mode), info.digital_plugin)) << "\""
             << ",\"digital_opts\":{\"verbose\":" << (info.digital_opts.verbose ? "true" : "false")
             << ",\"invert\":" << (info.digital_opts.invert ? "true" : "false")
             << ",\"map\":" << (info.digital_opts.map ? "true" : "false") << ",\"plugin\":{";
        bool fo = true;
        for (const auto& kv : info.digital_opts.plugin) {
            json << (fo ? "" : ",") << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
            fo = false;
        }
        json << "}}"
             << "}";
    }
    json << "]}";

    return json.str();
}

// Digital decoder status of every VFO with a decoder on (or one VFO's full
// history). Each push carries only the events logged since the previous push
// for that VFO; a page that opens the panel asks for the history
// (DIGITAL_HISTORY:id), and pages de-duplicate events by sequence number.
string MessageBuilders::build_digital_message(int only_id, bool history) {
    static std::mutex mu;
    // VFO id -> (decoder, last event seq pushed); the decoder pointer resets
    // the count when a VFO id is reused by a new decoder
    static std::map<int, std::pair<const void*, uint64_t>> sent;
    static std::map<int, int64_t> tables_sent_ms;   // the plugins' tables: at most 1 Hz
    std::lock_guard<std::mutex> lk(mu);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    stringstream json;
    json << "{\"digital\":[";
    bool first = true;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst) continue;
        if (only_id >= 0 && inst->id != only_id) continue;
        auto dd = inst->getDigital();
        if (!dd) continue;
        if (only_id < 0 && dd->mode() == dig::Mode::OFF) continue;
        auto& st = sent[inst->id];
        if (st.first != dd.get()) st = {dd.get(), 0};
        uint64_t after = history ? 0 : st.second;
        if (!first) json << ",";
        first = false;
        json << "{\"id\":" << inst->id << ",\"history\":" << (history ? "true" : "false") << ","
             << [&] {
                    // the plugins' tables, at most 1 Hz (incidents: build_incidents_message)
                    int64_t& ts = tables_sent_ms[inst->id];
                    const bool tables = history || now - ts >= 1000;
                    if (tables) ts = now;
                    return dd->status_json(after, history ? 400 : 100, tables);
                }().substr(1);
        if (!history) st.second = dd->last_event_seq();
    }
    json << "],\"geo\":" << incidents::status_json() << "}";
    if (first) return "";
    return json.str();
}

// The incident map's table + lookup status for the page's decoder tabs
// ({"incidents":...}, incidents.cpp): on GET / clear, and from the 1 Hz map
// timer when something changed (or every 15 s, for the "Seen" ages)
string MessageBuilders::build_incidents_message(bool force) {
    static std::mutex mu;
    static uint64_t sent_seq = ~0ull;
    static std::string sent_geo;
    static int64_t sent_ms = 0;
    std::lock_guard<std::mutex> lk(mu);
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t seq = incidents::seq();
    const std::string geo = incidents::status_json();
    if (!force && seq == sent_seq && geo == sent_geo && now - sent_ms < 15000) return "";
    sent_seq = seq;
    sent_geo = geo;
    sent_ms = now;
    return incidents::message_json();
}

namespace {
std::atomic<bool> map_full_requested{true};
}

void MessageBuilders::request_map_full() { map_full_requested.store(true); }

string MessageBuilders::build_map_message() {
    // VFO id -> (decoder, last map seq pushed); a new decoder starts over
    static std::map<int, std::pair<const void*, uint64_t>> sent;
    const bool full = map_full_requested.exchange(false);
    StationLocation sl = station_info.resolve();
    const bool have_station = !(sl.lat == 0.0 && sl.lon == 0.0) && is_finite_value(sl.lat) && is_finite_value(sl.lon);
    dig::set_station_location(have_station, sl.lat, sl.lon);
    std::string pts, keys;
    std::map<int, std::pair<const void*, uint64_t>> now;
    // incidents (incidents.cpp) of every plugin a decoder with Plot on map
    // runs - also those received by a VFO that has gone since
    static std::set<std::string> inc_plugins_sent;
    static uint64_t inc_sent = 0;
    std::set<std::string> inc_plugins;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst) continue;
        auto dd = inst->getDigital();
        if (!dd || dd->mode() == dig::Mode::OFF || !dd->options().map) continue;
        auto it = sent.find(inst->id);
        uint64_t after = (!full && it != sent.end() && it->second.first == dd.get()) ? it->second.second : 0;
        now[inst->id] = {dd.get(), dd->report().map_json(inst->id, after, pts, keys)};
        const std::string ap = dd->active_plugin();
        if (!ap.empty()) inc_plugins.insert(ap);
    }
    sent = std::move(now);
    // a plugin newly shown: all its incidents (the seq alone would skip them)
    const bool inc_full = full || inc_plugins != inc_plugins_sent;
    inc_sent = incidents::map_json(inc_plugins, inc_full ? 0 : inc_sent, pts, keys);
    inc_plugins_sent = std::move(inc_plugins);
    stringstream json;
    json << "{\"map\":{\"full\":" << (full ? "true" : "false") << ",\"station\":";
    if (have_station) {
        char b[64];
        snprintf(b, sizeof b, "[%.6f,%.6f]", sl.lat, sl.lon);
        json << b;
    } else {
        json << "null";
    }
    json << ",\"pts\":[" << pts << "],\"keys\":[" << keys << "]}}";
    return json.str();
}
