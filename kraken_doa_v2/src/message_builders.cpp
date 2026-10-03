#include "message_builders.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
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
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstdio>
#include <string>
#include <map>
#include <mutex>

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

string MessageBuilders::build_fft_message() {
    lock_guard<mutex> lock(fft_mutex);

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

                // Decimate and compress wideband FFT using user-adjustable setting
                int wideband_decimation = current_fft_decimation.load();
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
    normalize_to_noise_floor(normalized_fft);

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

    CompressedDecimatedFFT compressed = FFTProcessor::decimate_fft_compressed(normalized_fft, center_freq_mhz, current_fft_decimation.load());

    msg.add_value(static_cast<uint32_t>(compressed.frequencies.size()));

    // Add compressed data (frequencies as float32, magnitudes as uint8)
    msg.add_vector(compressed.frequencies);
    msg.add_vector(compressed.min_envelope);  // uint8 compressed
    msg.add_vector(compressed.max_envelope);  // uint8 compressed

    return msg.to_string();
}

string MessageBuilders::build_audio_message() {
    if (!fm_enabled.load() || !fm_demod.is_processing_enabled()) {
        return string();
    }

    auto opus_packet = fm_demod.get_opus_packet(AUDIO_SAMPLES_PER_PACKET);
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
    json << ",\"phase_state\":" << phase_state;
    json << ",\"noise_source\":" << (scanner_manager.isNoiseSourceActive() ? "true" : "false");
    json << ",\"active_elements\":" << active_num_elements.load();

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

    // Include per-decimator squelch states with eigenvalue ratio
    json << ",\"decimator_squelch\":[";
    auto info_list = decimator_manager.getDecimatorInfoList();
    bool first = true;
    for (const auto& info : info_list) {
        if (!first) json << ",";
        first = false;
        // Get eigenvalue ratio (instant + 5s peak hold), the effective eigen
        // threshold, and auto source estimate from the MUSIC processor
        float eigen_ratio = 1.0f;
        float eigen_peak = 1.0f;
        float eigen_thr = 0.0f;  // effective threshold in eigen modes (0 = n/a or unlearned)
        int est_sources = -1;    // -1 = auto mode off / no estimate
        auto decimator = decimator_manager.getDecimator(info.id);
        if (decimator && decimator->music_processor) {
            eigen_ratio = decimator->music_processor->getEigenvalueRatio();
            eigen_peak = decimator->music_processor->getEigenvalueRatioPeak();
            est_sources = decimator->music_processor->getEstimatedNumSources();
            int method = decimator->squelch_method.load(std::memory_order_relaxed);
            if (method == 2) {
                eigen_thr = decimator->auto_eigen_threshold.load(std::memory_order_relaxed);
            } else if (method == 1) {
                eigen_thr = decimator->squelch_eigen_threshold.load(std::memory_order_relaxed);
            }
        }
        json << "{\"id\":" << info.id
             << ",\"open\":" << (info.squelch_open ? "true" : "false")
             << ",\"eigen_ratio\":" << eigen_ratio
             << ",\"eigen_peak\":" << eigen_peak
             << ",\"eigen_thr\":" << eigen_thr
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

    // Include beamforming status (reported for the FM-source decimator, which is
    // the audible/primary one; mode/MVDR are shared across all decimators).
    int bf_fm_dec_id = decimator_manager.getFMDecimatorId();
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

string MessageBuilders::build_beamformed_fft_message(int decimator_id) {
    // Only send if beamforming is enabled
    if (!beamforming_enabled.load()) {
        return string();
    }

    auto inst = decimator_manager.getDecimator(decimator_id);
    if (!inst || !inst->beamformer) {
        return string();
    }
    BeamformedFFTData& bf = inst->beamformed_fft;

    if (!bf.valid.load(std::memory_order_acquire)) {
        return string();
    }

    // Copy this decimator's averaged FFT under its own lock, then release.
    vector<float> bf_normalized_fft;
    {
        lock_guard<mutex> lock(bf.mutex);
        if (bf.averaged.empty()) {
            return string();
        }
        bf_normalized_fft = bf.averaged;
    }

    BinaryMessage msg;
    msg.add_value(uint32_t(4)); // Beamformed FFT message type
    msg.add_value(static_cast<uint32_t>(decimator_id));  // which decimator this overlay is for

    float center_freq_mhz = bf.center_freq_hz.load(std::memory_order_relaxed) / 1e6f;
    float bandwidth_mhz = bf.bandwidth_hz.load(std::memory_order_relaxed) / 1e6f;

    // Get this decimator's beamformer stats for SNR boost display
    auto stats = inst->beamformer->getStats();

    // Normalize beamformed FFT to noise floor = 0dB
    // Note: can't use normalize_to_noise_floor() here because FFTProcessor::calculate_noise_floor()
    // checks against current_fft_size which is the main FFT size, not the beamformed slice size.
    {
        vector<float> bf_sorted = bf_normalized_fft;
        size_t p10 = bf_sorted.size() / 10;
        std::nth_element(bf_sorted.begin(), bf_sorted.begin() + p10, bf_sorted.end());
        float noise_floor = bf_sorted[p10];
        for (float& mag : bf_normalized_fft) {
            mag -= noise_floor;
        }
    }

    // DECIMATION: Match the DISPLAY bin density of the main FFT (bins per MHz after decimation)
    // Main FFT: current_fft_size bins for SAMPLE_RATE Hz, then decimated by current_fft_decimation
    // Display density = main_fft_size / (sample_rate_mhz * fft_decimation) bins/MHz
    // Beamformed FFT should have: bandwidth_mhz * display_density bins
    size_t input_size = bf_normalized_fft.size();
    float sample_rate_mhz = SAMPLE_RATE / 1e6f;
    int main_fft_size = current_fft_size.load();
    int fft_decimation = current_fft_decimation.load();

    // Calculate the display bin density (after main FFT's decimation is applied)
    float display_bins_per_mhz = static_cast<float>(main_fft_size) / (sample_rate_mhz * fft_decimation);

    // Target bins for beamformed FFT to match the same visual density
    size_t target_bins = static_cast<size_t>(bandwidth_mhz * display_bins_per_mhz);

    // Ensure minimum of 16 bins and don't upsample
    target_bins = std::max(target_bins, size_t(16));
    target_bins = std::min(target_bins, input_size);

    size_t decimation_factor = (input_size > target_bins) ? (input_size / target_bins) : 1;
    size_t output_size = (input_size + decimation_factor - 1) / decimation_factor;

    // Decimate by taking max value in each bin group (preserves peaks)
    vector<float> decimated_fft;
    decimated_fft.reserve(output_size);

    for (size_t i = 0; i < output_size; i++) {
        size_t start_idx = i * decimation_factor;
        size_t end_idx = min(start_idx + decimation_factor, input_size);

        float max_val = bf_normalized_fft[start_idx];
        for (size_t j = start_idx + 1; j < end_idx; j++) {
            max_val = max(max_val, bf_normalized_fft[j]);
        }
        decimated_fft.push_back(max_val);
    }

    // Add metadata (use decimated size)
    msg.add_value(center_freq_mhz);
    msg.add_value(bandwidth_mhz);
    msg.add_value(static_cast<uint32_t>(decimated_fft.size()));

    msg.add_value(stats.current_steering_angle);
    msg.add_value(stats.estimated_snr_improvement_db);

    // Compress magnitudes to uint8 for bandwidth savings
    // Map 0dB to 120dB range to 0-255
    vector<uint8_t> compressed;
    compressed.reserve(decimated_fft.size());

    constexpr float MIN_DB = 0.0f;
    constexpr float MAX_DB = 120.0f;
    constexpr float DB_RANGE = MAX_DB - MIN_DB;
    constexpr float SCALE = 255.0f / DB_RANGE;

    for (float mag_db : decimated_fft) {
        float normalized = (mag_db - MIN_DB) * SCALE;
        int value = static_cast<int>(normalized + 0.5f);
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        compressed.push_back(static_cast<uint8_t>(value));
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
                int tuner_ch = decimator_inst->wideband_tuner_channel;
                float tuner_freq_hz = ChannelManager::get_frequency(tuner_ch);

                // Convert: tuner_relative → absolute → wideband_center_relative
                float absolute_freq = tuner_freq_hz + info.frequency_offset_hz;
                ui_offset_hz = absolute_freq - wideband_center_hz;
            }
        }

        if (!first) json << ",";
        first = false;

        json << "{\"id\":" << info.id
             << ",\"freq_offset_hz\":" << ui_offset_hz  // Send UI-friendly offset
             << ",\"bandwidth_index\":" << info.bandwidth_index
             << ",\"bandwidth_mhz\":" << info.bandwidth_mhz
             << ",\"enabled\":" << (info.enabled ? "true" : "false")
             << ",\"is_fm_source\":" << (info.is_fm_source ? "true" : "false")
             << ",\"demod_mode\":\"" << DecimatorManager::demodModeToString(info.demod_mode) << "\""
             << ",\"squelch_enabled\":" << (info.squelch_enabled ? "true" : "false")
             << ",\"squelch_level\":" << info.squelch_level
             << ",\"squelch_open\":" << (info.squelch_open ? "true" : "false")
             << ",\"squelch_method\":\"" << (info.squelch_method == 2 ? "EIGEN_AUTO" : (info.squelch_method == 1 ? "EIGEN" : "FFT")) << "\""
             << ",\"squelch_eigen_threshold\":" << info.squelch_eigen_threshold
             << ",\"digital_mode\":\"" << dig::mode_name(static_cast<dig::Mode>(info.digital_mode)) << "\""
             << ",\"digital_opts\":{\"verbose\":" << (info.digital_opts.verbose ? "true" : "false")
             << ",\"dmr_slot\":" << info.digital_opts.dmr_slot
             << ",\"p25_nac\":" << info.digital_opts.p25_nac
             << ",\"invert\":" << (info.digital_opts.invert ? "true" : "false") << "}"
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
    std::lock_guard<std::mutex> lk(mu);
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
             << dd->status_json(after, history ? 400 : 100).substr(1);
        if (!history) st.second = dd->last_event_seq();
    }
    json << "]}";
    if (first) return "";
    return json.str();
}
