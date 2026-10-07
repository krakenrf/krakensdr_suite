// ============================================
// src/networking/data_receiver.cpp - OPTIMIZED
// ============================================

#include "networking/data_receiver.hpp"
#include "utils/thread_pool.hpp"
#include "utils/parse_num.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/shared_decimator.hpp"
#include "signal_processing/beamformer.hpp"
#include "decimator_manager.hpp"
#include "doa_logger.hpp"
#include "networking/tcp_client.hpp"
#include "utils/system_stats.hpp"
#include "utils/ring_buffer.hpp"
#include "utils/raw_data_buffer.hpp"
#include "utils/iq_converter.hpp"
#include "channel_manager.hpp"
#include "scanner_manager.hpp"
#include "control_handler.hpp"
#include "networking/websocket_server.hpp"
#include "utils/endian_utils.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <algorithm>
#include <sstream>
#include <array>
#include <future>
#include <cerrno>

using namespace std;
using namespace chrono;

extern DecimatorManager decimator_manager;

// Global raw data buffer for incoming TCP packets
RawDataBuffer raw_data_buffer(150, 2048, milliseconds(10000));

// Queue for decimated FM data
struct DecimatedFMWorkItem {
    vector<complex<float>> decimated_samples;
    int channel_id;
    int decimation_used;
    float output_rate_hz;
    chrono::steady_clock::time_point timestamp;
    
    // OPTIMIZATION: Add move constructor and move assignment
    DecimatedFMWorkItem() = default;
    DecimatedFMWorkItem(DecimatedFMWorkItem&&) = default;
    DecimatedFMWorkItem& operator=(DecimatedFMWorkItem&&) = default;
    
    // Delete copy operations to prevent accidental copies
    DecimatedFMWorkItem(const DecimatedFMWorkItem&) = delete;
    DecimatedFMWorkItem& operator=(const DecimatedFMWorkItem&) = delete;
};
moodycamel::ConcurrentQueue<DecimatedFMWorkItem> fm_decimated_queue;
mutex fm_decimated_mutex;
condition_variable fm_decimated_available;

// steady_clock ms of the last completed server retune. 0 = never retuned.
// ALL DoA processing is skipped for RETUNE_DOA_MIN_HOLD_MS after it, and then
// for as long as heimdall reports its post-retune cooldown
// (WAITING_FOR_STABILITY: the compensation is reset to identity and the noise
// source is still OFF, so doa_is_calibrating() can't see it). The calibration
// that follows has the noise source on (doa_is_calibrating). The minimum covers
// the ~20 ms between the new frequency appearing in the packet header and the
// cooldown state. Was a fixed 4 s, sized for heimdall's former 3 s cooldown.
static std::atomic<int64_t> g_last_retune_complete_ms{0};
static constexpr int64_t RETUNE_DOA_MIN_HOLD_MS = 500;
static constexpr uint32_t PHASE_STATE_WAITING_FOR_STABILITY = 5;  // heimdall PhaseCompensatorState

static inline int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Called per packet right after updatePhaseCalibrationStatus(), so the state
// is THIS packet's
static inline bool doa_retune_hold_active() {
    if (scanner_manager.getPhaseState() == PHASE_STATE_WAITING_FOR_STABILITY) return true;
    const int64_t t = g_last_retune_complete_ms.load(std::memory_order_relaxed);
    return t != 0 && (steady_now_ms() - t) < RETUNE_DOA_MIN_HOLD_MS;
}


void DataReceiver::data_receiver_thread() {
    // OPTIMIZATION: Register thread and get index for lock-free stats access
    size_t thread_idx = global_stats.register_thread("DataReceiver");

    // Initialize the decimator manager with 1 default decimator
    decimator_manager.initialize(MAX_CHANNELS, 1);

    vector<uint8_t> buffer(4 * 1024 * 1024);
    size_t bytes_in_buffer = 0;

    // Upper bound for the per-channel sample count in a packet header
    constexpr size_t MAX_SAMPLES_PER_CHANNEL = 65536;

    // Track server-side scanner frequency changes
    uint32_t last_frequency_change_counter = 0;
    bool first_packet = true;

    // Restore persisted settings once, on the first successful connection (the
    // decimators now exist and the server is reachable for tuner commands).
    // Not repeated on reconnect, so a transient drop won't stomp live state.
    bool settings_restored = false;

    cout << "Data receiver thread started with optimized scalar IQ conversion" << endl;
    cout << "Using compiler auto-vectorized simple scalar loop (8.5% faster than NEON)" << endl;
    cout << "Server-side discrete scanner detection enabled" << endl;

    while (running) {
        if (!data_client.connect_to_port(TCP_DATA_PORT)) {
            global_stats.increment_errors(thread_idx);
            this_thread::sleep_for(seconds(1));
            continue;
        }

        cout << "Connected to Heimdall data port" << endl;

        if (!settings_restored) {
            // Replay on the uWS loop thread, where browser commands (and the
            // web mapper's cloud settings) are dispatched. Replaying from this
            // thread raced them: a browser change landing mid-replay could be
            // overwritten by the saved value, and was never saved itself (the
            // replay's no-save flag is global). defer() is uWS's only
            // thread-safe call; the web server is normally listening long
            // before heimdall's data port accepts us.
            while (running && !loop) this_thread::sleep_for(milliseconds(100));
            // A previous client instance (crash + supervisor restart) may have
            // left heimdall's discrete scan hopping: only this client drives
            // it, and its scanner starts stopped - make heimdall agree
            ControlHandler::send_control_command("{\"stop_scanner\":true}");
            if (loop) loop->defer([] { ControlHandler::apply_persisted_settings(); });
            settings_restored = true;
        }

        // Fresh connection = fresh stream: discard any partial packet left
        // over from the previous connection so old and new bytes never splice
        // into one structurally-valid-but-garbage packet.
        bytes_in_buffer = 0;

        while (running) {
            ssize_t bytes_read = data_client.receive_data(
                buffer.data() + bytes_in_buffer, 
                buffer.size() - bytes_in_buffer
            );
            
            if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                // Receive timeout (SO_RCVTIMEO) - no data yet; loop so the
                // `running` flag is observed for prompt shutdown
                continue;
            }

            if (bytes_read <= 0) {
                cerr << "TCP connection lost, reconnecting..." << endl;
                data_client.disconnect();
                break;
            }
            
            bytes_in_buffer += bytes_read;
            global_stats.add_bytes_processed(thread_idx, bytes_read);
            
            // Parse complete packets from TCP stream
            size_t offset = 0;
            while (offset + 32 <= bytes_in_buffer) {  // Minimum header now 32 bytes (added retuning_in_progress)
                uint32_t magic = EndianUtils::read_be32(buffer.data(), offset);

                if (magic != TCP_MAGIC) {
                    offset++;
                    continue;
                }

                uint32_t channels = EndianUtils::read_be32(buffer.data(), offset + 4);
                uint32_t samples = EndianUtils::read_be32(buffer.data(), offset + 8);
                uint32_t phase_comp_state = EndianUtils::read_be32(buffer.data(), offset + 12);
                uint32_t noise_source = EndianUtils::read_be32(buffer.data(), offset + 16);

                // KerberosSDR support flags piggyback on the phase-state
                // field's high bits (heimdall tcp_data_server): bit 8 =
                // kerberos mode, bit 9 = calibration stale. Mask them off so
                // every downstream phase-state comparison keeps working.
                server_kerberos_mode.store((phase_comp_state & 0x100u) != 0, std::memory_order_relaxed);
                server_cal_stale.store((phase_comp_state & 0x200u) != 0, std::memory_order_relaxed);
                // Bits 10 / 11: heimdall's operating mode (wideband scan /
                // independent). Only acted on once this heimdall has shown it
                // sends them (an older one never sets either bit)
                {
                    static bool mode_bits_seen = false;
                    const int hmode = (phase_comp_state & 0x800u) ? 2 : (phase_comp_state & 0x400u) ? 1 : 0;
                    if (hmode) mode_bits_seen = true;
                    if (mode_bits_seen) ControlHandler::note_server_mode(hmode);
                }
                phase_comp_state &= 0xFFu;
                uint32_t frequency_change_counter = EndianUtils::read_be32(buffer.data(), offset + 20);
                uint32_t current_group_index = EndianUtils::read_be32(buffer.data(), offset + 24);
                uint32_t retuning_in_progress = EndianUtils::read_be32(buffer.data(), offset + 28);

                // Sanity-check header fields BEFORE acting on them: a corrupt
                // or desynced stream must not drive huge allocations
                // (channels*samples*2 wraps in 32-bit arithmetic), out-of-
                // bounds metadata reads, or a parser stall waiting for a
                // bogus multi-GB "packet". Treat as a false magic match and
                // resync byte by byte.
                if (channels == 0 || channels > MAX_CHANNELS ||
                    samples == 0 || samples > MAX_SAMPLES_PER_CHANNEL) {
                    offset++;
                    continue;
                }

                // Track phase calibration state changes for logging
                static uint32_t last_phase_state = 0;
                if (phase_comp_state != last_phase_state) {
                    // heimdall PhaseCompensatorState order
                    const char* state_names[] = {"WAITING_LAG", "MEASURING", "APPLYING", "VERIFYING",
                                                 "CONVERGED", "WAITING_STABILITY", "MEASURING_PER_BIN"};
                    const char* state_name = (phase_comp_state < 7) ? state_names[phase_comp_state] : "UNKNOWN";
                    cout << "Phase calibration: " << state_name << endl;
                    last_phase_state = phase_comp_state;
                }

                // Track retuning state transitions for FFT reset
                // We want to reset FFT when retuning COMPLETES (transition from 1->0)
                // This ensures we don't capture noisy settling data
                static bool was_retuning = false;
                bool is_retuning = (retuning_in_progress != 0);

                if (was_retuning && !is_retuning) {
                    // Retuning just completed
                    cout << "[RETUNING] *** RETUNING COMPLETE *** Data flow should resume now!" << endl;

                    // Hold DoA for the settling window that follows (see
                    // g_last_retune_complete_ms)
                    g_last_retune_complete_ms.store(steady_now_ms(), std::memory_order_relaxed);

                    // In WIDEBAND mode: Skip FFT reset - signals stay visible during drag-to-tune
                    // In COHERENT mode: Reset FFT averaging for fresh phase calibration data
                    bool is_wideband_mode = multi_tuner_mode();   // wideband or independent

                    if (!is_wideband_mode) {
                        // Coherent mode: reset FFT averaging to start fresh
                        uint32_t old_gen = fft_reset_generation.fetch_add(1, std::memory_order_release);
                        cout << "Retuning complete! Resetting FFT averaging (gen " << old_gen << " → " << (old_gen + 1) << ")" << endl;

                        // Mark FFT data as invalid until new data arrives
                        fft_data_valid.store(false, std::memory_order_release);

                        // CRITICAL: Drain FFT work queue to prevent stale data from consuming the reset
                        // Stale work items would see the new generation, use alpha=1.0, and set their
                        // channel's last_gen to current. Then when NEW data arrives, last_gen matches
                        // and normal averaging is used instead of initializing from fresh data.
                        FFTWorkItem stale_item;
                        while (fft_work_queue.try_dequeue(stale_item)) {
                        }

                        // Clear per-channel FFT buffers to prevent stale data display
                        // First valid FFT frame will reinitialize with alpha=1.0 (direct copy)
                        FFTProcessor::reset_all_buffers();

                        // Clear wideband FFT buffers (will be re-initialized by first frame with alpha=1.0)
                        {
                            lock_guard<mutex> wb_lock(wideband_fft_mutex);
                            fill(wideband_fft_magnitudes.begin(), wideband_fft_magnitudes.end(), -100.0f);
                            fill(wideband_fft_averaged.begin(), wideband_fft_averaged.end(), -100.0f);
                        }
                    }
                }

                // Update frequency change counter tracking. Also arms the DoA
                // retune hold: the retuning_in_progress pulse can fall entirely
                // between packets (a tuner reprogram takes ~10-50 ms), but the
                // counter bump is always visible (the post-retune cooldown
                // after it is held on heimdall's phase state).
                if (!first_packet && frequency_change_counter != last_frequency_change_counter) {
                    cout << "Server frequency change detected! Counter: " << last_frequency_change_counter
                         << " → " << frequency_change_counter
                         << ", Group: " << current_group_index
                         << ", Retuning: " << (is_retuning ? "YES" : "NO") << endl;
                    g_last_retune_complete_ms.store(steady_now_ms(), std::memory_order_relaxed);
                    last_frequency_change_counter = frequency_change_counter;
                } else if (first_packet) {
                    // Initialize on first packet
                    last_frequency_change_counter = frequency_change_counter;
                    first_packet = false;
                }

                was_retuning = is_retuning;

                // Header: magic(4) + channels(4) + samples(4) + phase_state(4) + noise_source(4) +
                //         freq_change_counter(4) + current_group_index(4) + retuning_in_progress(4) + per-channel(8*N)
                size_t header_size = 32 + static_cast<size_t>(channels) * 8;
                size_t packet_size = header_size + static_cast<size_t>(channels) * samples * 2;
                
                if (offset + packet_size > bytes_in_buffer) {
                    break;
                }
                
                RawDataPacket raw_packet;
                raw_packet.num_channels = channels;
                raw_packet.samples_per_channel = samples;
                raw_packet.phase_compensation_state = phase_comp_state;
                raw_packet.noise_source_active = noise_source;
                raw_packet.channel_iq_data.resize(channels);

                // Parse channel metadata (now starts at offset 32 after retuning_in_progress field)
                std::array<float, MAX_CHANNELS> ch_freq_hz{};
                for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++) {
                    size_t ch_info_offset = offset + 32 + ch * 8;

                    float frequency_hz = EndianUtils::read_le_float(buffer.data(), ch_info_offset);
                    float gain_db = EndianUtils::read_le_float(buffer.data(), ch_info_offset + 4);

                    ch_freq_hz[ch] = frequency_hz;

                    ChannelManager::update_channel_info(ch, frequency_hz, gain_db);
                }
                
                // Arm the DoA retune hold whenever the REFERENCE channel's
                // metadata frequency changes. This is ground truth in every
                // packet - unlike retuning_in_progress (a pulse that can fall
                // entirely between packets) or frequency_change_counter (only
                // bumped by scanner group changes), both of which proved
                // unreliable for plain retunes. The hold then follows
                // heimdall's post-retune cooldown state (doa_retune_hold_active).
                {
                    static float last_ref_freq_hz = 0.0f;
                    const float ref_freq_hz = ch_freq_hz[0];
                    if (last_ref_freq_hz != 0.0f && ref_freq_hz != 0.0f &&
                        std::abs(ref_freq_hz - last_ref_freq_hz) > 1.0f) {
                        g_last_retune_complete_ms.store(steady_now_ms(), std::memory_order_relaxed);
                        cout << "[DoA] Reference frequency changed ("
                             << last_ref_freq_hz / 1e6f << " -> " << ref_freq_hz / 1e6f
                             << " MHz) - holding DoA until heimdall recalibrates" << endl;
                    }
                    if (ref_freq_hz != 0.0f) last_ref_freq_hz = ref_freq_hz;
                }

                // SKIP PROCESSING if server is currently retuning (data contains noise)
                // Only advance past the packet without processing it
                if (is_retuning) {
                    // Still update tuner frequencies so UI shows correct target frequency
                    for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++) {
                        tuner_frequencies[ch].store(static_cast<uint64_t>(ch_freq_hz[ch]),
                                                    std::memory_order_relaxed);
                    }
                    offset += packet_size;
                    continue;  // Skip to next packet - don't process noisy settling data
                }

                // Sync active_num_elements from the wire header BEFORE the
                // gates below, so a runtime element-count change on the server
                // applies from this very packet (MUSIC, beamformer and the UI
                // all follow this atomic).
                int current_elements = active_num_elements.load(std::memory_order_relaxed);
                if (static_cast<int>(channels) != current_elements && channels >= 2 && channels <= MAX_CHANNELS) {
                    cout << "Active elements synced from server: " << current_elements << " → " << channels << endl;
                    active_num_elements.store(channels, std::memory_order_release);
                    current_elements = static_cast<int>(channels);
                }

                // OPTIMIZATION: Determine which channels to convert upfront
                // Use relaxed memory ordering for 5-10% faster atomic loads (flags don't need strict ordering)
                int current_active = active_channel.load(std::memory_order_relaxed);
                bool need_doa = (static_cast<int>(channels) >= current_elements && doa_enabled.load(std::memory_order_relaxed));
                bool need_fm = (fm_enabled.load(std::memory_order_relaxed) && current_active < static_cast<int>(channels));
                // Wideband / independent: every tuner is converted (one FFT each)
                bool is_wideband = multi_tuner_mode();
                bool need_beamforming = (static_cast<int>(channels) >= current_elements && beamforming_enabled.load(std::memory_order_relaxed));

                // OPTIMIZATION #4: Use stack-allocated array instead of heap vector (eliminates allocation)
                // OPTIMIZATION #5: Use boolean flags instead of std::find (O(1) lookup instead of O(n) search)
                bool channels_to_convert[MAX_CHANNELS] = {false};
                int num_channels_to_convert = 0;

                // Wideband mode: Convert ALL channels for combined FFT display
                if (is_wideband) {
                    for (int ch = 0; ch < min(static_cast<int>(channels), MAX_CHANNELS); ch++) {
                        channels_to_convert[ch] = true;
                        num_channels_to_convert++;
                    }
                }
                // Convert all elements if DoA or beamforming needs them
                // (beamforming needs all active elements for coherent
                // combining). FM alone decimates only the listened-to channel,
                // which is marked below.
                else if (need_doa || need_beamforming) {
                    for (int ch = 0; ch < min(current_elements, static_cast<int>(channels)); ch++) {
                        if (!channels_to_convert[ch]) {
                            channels_to_convert[ch] = true;
                            num_channels_to_convert++;
                        }
                    }
                }

                // Active channel must exist in THIS packet: channel_iq_data only
                // has `channels` entries, so marking a higher index would write
                // past the vector and read IQ bytes beyond the packet.
                bool active_in_packet = current_active >= 0 &&
                                        current_active < MAX_CHANNELS &&
                                        current_active < static_cast<int>(channels);

                // FM channel (if not already marked)
                if (need_fm && active_in_packet && !channels_to_convert[current_active]) {
                    channels_to_convert[current_active] = true;
                    num_channels_to_convert++;
                }

                // FFT channel (if not already marked)
                if (active_in_packet && !channels_to_convert[current_active]) {
                    channels_to_convert[current_active] = true;
                    num_channels_to_convert++;
                }

                // OPTIMIZATION: Batch convert all needed channels with optimized NEON
                if (num_channels_to_convert > 0) {
                    // Pre-allocate exact sizes for channels marked for conversion
                    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                        if (channels_to_convert[ch]) {
                            raw_packet.channel_iq_data[ch].resize(samples);
                        }
                    }

                    // Convert all channels in one optimized pass. DC correction
                    // (removes the RTL centre spike) tracks each channel's offset
                    // across packets - see convert_uint8_to_complex_float_tracked_dc.
                    // Only this receiver thread converts, so plain state is fine.
                    static float dc_i[MAX_CHANNELS], dc_q[MAX_CHANNELS];
                    static bool dc_seeded[MAX_CHANNELS] = {};
                    for (int ch = 0; ch < MAX_CHANNELS; ch++) {
                        if (channels_to_convert[ch]) {
                            const uint8_t* channel_iq_bytes = buffer.data() + offset + header_size + ch * samples * 2;

                            IQConverter::convert_uint8_to_complex_float_tracked_dc(
                                channel_iq_bytes,
                                raw_packet.channel_iq_data[ch].data(),
                                samples,
                                dc_i[ch], dc_q[ch], dc_seeded[ch]
                            );
                        }
                    }
                }
                
                // OPTIMIZATION: Use move semantics
                if (!raw_data_buffer.push_packet(std::move(raw_packet))) {
                    global_stats.increment_errors(thread_idx);
                } else {
                    global_stats.increment_operations(thread_idx);
                }

                num_channels = channels;
                data_ready = true;

                offset += packet_size;
            }

            if (offset > 0) {
                if (offset < bytes_in_buffer) {
                    memmove(buffer.data(), buffer.data() + offset, bytes_in_buffer - offset);
                }
                bytes_in_buffer -= offset;
            }

            global_stats.print_stats_if_time(thread_idx);
        }
    }

    cout << "Data receiver thread exiting" << endl;
}


void DataReceiver::decimation_processor_thread() {
    // OPTIMIZATION: Register thread and get index for lock-free stats access
    size_t thread_idx = global_stats.register_thread("DecimationProcessor");
    
    cout << "Decimation processor thread started" << endl;
    cout << "Working with pre-converted float IQ data for efficiency" << endl;
    
    while (running) {
        RawDataPacket raw_packet;

        if (!raw_data_buffer.try_pop_packet(raw_packet, milliseconds(100))) {
            if (raw_data_buffer.is_shutdown()) {
                break;
            }
            continue;
        }

        if (raw_packet.is_stale(milliseconds(1500))) {
            global_stats.increment_errors(thread_idx);
            continue;
        }

        // Update scanner with phase calibration status from server
        // This allows scanner to avoid checking for signal loss during calibration
        scanner_manager.updatePhaseCalibrationStatus(raw_packet.phase_compensation_state,
                                                    raw_packet.noise_source_active);

        // Push calibration-state / noise-flag changes to the browsers at once
        // (the system_status broadcast is every 500 ms - the "(Noise On)"
        // badge lagged a ~0.4 s retune calibration by up to half a second).
        {
            static uint32_t last_cal = 0xFFFFFFFFu;
            const uint32_t cal = (raw_packet.phase_compensation_state << 1) | (raw_packet.noise_source_active ? 1u : 0u);
            if (cal != last_cal) {
                last_cal = cal;
                WebSocketServer::broadcast_json_message(
                    "{\"cal_live\":{\"phase_state\":" + to_string(raw_packet.phase_compensation_state) +
                    ",\"noise_source\":" + (raw_packet.noise_source_active ? "true" : "false") + "}}");
            }
        }

        // Noise source switched on/off: restart the spectrum averaging so the
        // display jumps to the new state. The EMA works in dB, so a strong
        // antenna signal took ~3 frames (~150 ms) to sink into the noise-source
        // spectrum and the noise ~2 to clear - a third of a ~0.4 s retune
        // calibration burst showed the signal "with the noise on". The flag
        // trails the samples by 0-2 packets, so this packet is already the
        // new state. Queued items from before the switch are dropped: they
        // would consume the reset (see the retune reset above).
        {
            static bool last_noise = false;
            const bool noise = raw_packet.noise_source_active != 0;
            if (noise != last_noise) {
                last_noise = noise;
                fft_reset_generation.fetch_add(1, std::memory_order_release);
                FFTWorkItem stale_item;
                while (fft_work_queue.try_dequeue(stale_item)) {
                }
            }
        }


        int current_active = static_cast<int>(active_channel.load(std::memory_order_relaxed));

        // Element count for THIS packet: the receiver thread syncs the global
        // from the wire header, but a queued packet may still carry the old
        // channel count - never gather more channels than the packet has.
        int packet_elements = min(active_num_elements.load(std::memory_order_relaxed),
                                  static_cast<int>(raw_packet.num_channels));

        bool fm_needs_processing = fm_enabled.load(std::memory_order_relaxed) &&
                                  static_cast<int>(raw_packet.num_channels) > current_active;
        // MUSIC requires coherent channels: off in wideband and independent
        // mode, where each VFO decimates only its own tuner (tuner_channel)
        bool wideband_enabled = multi_tuner_mode();
        bool music_needs_processing = !wideband_enabled &&
                                      packet_elements >= 2 &&
                                      static_cast<int>(raw_packet.num_channels) >= active_num_elements.load(std::memory_order_relaxed) &&
                                      doa_enabled.load(std::memory_order_relaxed);
        // Beamforming also requires coherent channels
        bool beamforming_needs_processing = !wideband_enabled &&
                                            packet_elements >= 2 &&
                                            static_cast<int>(raw_packet.num_channels) >= active_num_elements.load(std::memory_order_relaxed) &&
                                            beamforming_enabled.load(std::memory_order_relaxed);
        
        // IMPORTANT: Check if FFT needs the same channel as MUSIC
        bool fft_needs_active_channel = (current_active < static_cast<int>(raw_packet.num_channels) &&
                                         current_active < static_cast<int>(raw_packet.channel_iq_data.size()) &&
                                         current_active < MAX_CHANNELS);

        // Process through all decimators in DecimatorManager (with parallelization)
        // Run decimators if DoA (MUSIC), FM, or beamforming needs processing
        // Digital voice/data decoders consume their VFO's decimated stream
        const bool digital_needs_processing = decimator_manager.anyDigitalActive();
        if ((music_needs_processing || fm_needs_processing || beamforming_needs_processing || digital_needs_processing) &&
            decimator_manager.getDecimatorCount() > 0) {
            // OPTIMIZED PIPELINE: Process decimation + MUSIC in single async tasks
            // This eliminates the synchronization barrier between decimation and MUSIC stages
            // FM / digital only (no MUSIC, no beamforming): the FM audio and the
            // digital decoders are the only consumers of decimated data -
            // squelch reads the main FFT - so run just the FM source VFO and
            // the VFOs with a digital decoder, on just the listened-to channel.
            // Every VFO used to decimate every element for nothing.
            const bool fm_only = !music_needs_processing && !beamforming_needs_processing;
            const int fm_source_id = decimator_manager.getFMDecimatorId();
            vector<shared_ptr<DecimatorManager::DecimatorInstance>> active_decimators;
            for (auto& inst : decimator_manager.getAllDecimators()) {
                // Skip disabled or being-deleted decimators
                if (!inst || !inst->enabled || inst->being_deleted.load(std::memory_order_relaxed)) continue;
                const bool single_wanted = (fm_needs_processing && inst->id == fm_source_id) ||
                                           inst->digital_mode.load(std::memory_order_relaxed) != 0;
                if (fm_only && !single_wanted) {
                    // Not decimated in this mode, but keep its FFT-method
                    // squelch indicator live (it reads the main FFT only)
                    // (outside coherent mode every method runs as FFT - the
                    // eigenvalue ones need MUSIC, which doesn't run there)
                    if (inst->squelch_enabled.load(std::memory_order_relaxed) &&
                        (wideband_enabled ||
                         static_cast<SquelchMethod>(inst->squelch_method.load(std::memory_order_relaxed)) == SquelchMethod::FFT)) {
                        inst->squelch_open.store(FFTProcessor::check_squelch_in_range(
                            wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed) : current_active,
                            inst->squelch_level.load(std::memory_order_relaxed),
                            inst->frequency_offset_hz, inst->decimator->getBandwidthMhz() * 1e6f),
                            std::memory_order_relaxed);
                    }
                    continue;
                }
                active_decimators.push_back(std::move(inst));
            }

            // Pipelined task per decimator: decimation → MUSIC (no intermediate barrier)
            // In wideband mode, each decimator gets data from its specific tuner
            auto run_pipeline =
                [&raw_packet, fm_id = fm_source_id, wideband_enabled, packet_elements, fm_only, current_active](
                    const shared_ptr<DecimatorManager::DecimatorInstance>& inst) -> DecimatorManager::ProcessResult {

                    DecimatorManager::ProcessResult result;
                    result.decimator_id = inst->id;
                    result.is_fm_source = (inst->id == fm_id);
                    result.instance = inst;  // kept for the post-barrier beamforming pass

                    try {
                        // Point at this decimator's input channels directly in
                        // raw_packet - no copies. Safe because all pipeline
                        // futures are collected before raw_packet goes out of
                        // scope, and decimateMultiChannel only reads the data.
                        // Wideband: 1 channel from this decimator's tuner;
                        // normal mode: all active elements for MUSIC
                        // (arrays sized to the DOA_NUM_ELEMENTS ceiling).
                        const complex<float>* channel_ptrs[DOA_NUM_ELEMENTS] = {nullptr};
                        size_t channel_lens[DOA_NUM_ELEMENTS] = {0};
                        size_t input_channels;

                        if (wideband_enabled) {
                            int tuner_ch = inst->tuner_channel;
                            if (tuner_ch >= 0 && tuner_ch < static_cast<int>(raw_packet.channel_iq_data.size())) {
                                channel_ptrs[0] = raw_packet.channel_iq_data[tuner_ch].data();
                                channel_lens[0] = raw_packet.channel_iq_data[tuner_ch].size();
                            }
                            input_channels = 1;
                        } else if (fm_only) {
                            if (current_active < static_cast<int>(raw_packet.channel_iq_data.size())) {
                                channel_ptrs[0] = raw_packet.channel_iq_data[current_active].data();
                                channel_lens[0] = raw_packet.channel_iq_data[current_active].size();
                            }
                            input_channels = 1;
                        } else {
                            input_channels = static_cast<size_t>(packet_elements);
                            for (int ch = 0; ch < packet_elements; ch++) {
                                if (ch < static_cast<int>(raw_packet.channel_iq_data.size())) {
                                    channel_ptrs[ch] = raw_packet.channel_iq_data[ch].data();
                                    channel_lens[ch] = raw_packet.channel_iq_data[ch].size();
                                }
                            }
                        }

                        // STAGE 1: Decimation. The offset is read ONCE per block
                        // (the control thread may move the VFO meanwhile), so
                        // every channel mixes - and is labelled - with the same one.
                        const float block_offset_hz = inst->frequency_offset_hz;
                        result.decimated_data = inst->decimator->decimateMultiChannel(
                            channel_ptrs, channel_lens, input_channels,
                            inst->decimator->getDecimationFactor(),
                            block_offset_hz
                        );

                        // Restore user-facing offset sign
                        result.decimated_data.freq_offset_hz = block_offset_hz;
                        result.decimated_data.stream_pos =
                            inst->stream_pos.fetch_add(result.decimated_data.min_samples, std::memory_order_relaxed);
                        for (auto& channel_data_out : result.decimated_data.channels) {
                            channel_data_out.freq_offset_hz = block_offset_hz;
                        }

                        // STAGE 2: MUSIC (immediately after decimation, no barrier)
                        // SKIP MUSIC in wideband mode (channels are not phase-coherent)
                        // SKIP MUSIC if per-decimator squelch is enabled but closed (FFT method only)
                        // IMPORTANT: When beamforming is enabled, NEVER squelch MUSIC - it needs
                        // to run to provide DoA steering angle. Squelch only applies to audio output.
                        // IMPORTANT: When eigenvalue squelch is enabled, ALWAYS run MUSIC to compute ratio.
                        bool squelch_allows_music = true;
                        // Wideband / independent: no beamformer and no MUSIC run,
                        // so the FFT squelch on this VFO's own tuner is the only one
                        // (an eigenvalue method would freeze its last state - and
                        // the audio with it; the saved method returns in coherent)
                        bool bf_active = beamforming_enabled.load(std::memory_order_relaxed) && !wideband_enabled;
                        // Squelch method is per decimator (each has its own MUSIC
                        // processor, so an eigenvalue squelch is naturally local too)
                        SquelchMethod current_squelch_method = wideband_enabled ? SquelchMethod::FFT
                            : static_cast<SquelchMethod>(inst->squelch_method.load(std::memory_order_relaxed));
                        bool use_eigenvalue_squelch = (current_squelch_method == SquelchMethod::EIGENVALUE ||
                                                       current_squelch_method == SquelchMethod::EIGENVALUE_AUTO);

                        if (inst->squelch_enabled.load(std::memory_order_relaxed) && !bf_active && !use_eigenvalue_squelch) {
                            // FFT-based squelch: Only apply squelch to MUSIC when beamforming is OFF
                            // When beamforming is ON, MUSIC must run to provide steering
                            float squelch_level = inst->squelch_level.load(std::memory_order_relaxed);
                            int check_channel = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed)
                                                                 : active_channel.load(std::memory_order_relaxed);
                            float offset_hz = inst->frequency_offset_hz;
                            float bw_hz = inst->decimator->getBandwidthMhz() * 1e6f;
                            squelch_allows_music = FFTProcessor::check_squelch_in_range(
                                check_channel, squelch_level, offset_hz, bw_hz);
                            // Update squelch_open state for UI feedback
                            inst->squelch_open.store(squelch_allows_music, std::memory_order_relaxed);
                        } else if (bf_active && !use_eigenvalue_squelch) {
                            // Beamforming active with FFT squelch: always allow MUSIC, update squelch
                            // state from THIS decimator's beamformed FFT (from the previous block).
                            if (inst->squelch_enabled.load(std::memory_order_relaxed) &&
                                inst->beamformed_fft.valid.load(std::memory_order_acquire)) {
                                float squelch_level = inst->squelch_level.load(std::memory_order_relaxed);
                                bool bf_squelch_open = FFTProcessor::check_squelch_beamformed(
                                    inst->beamformed_fft, squelch_level);
                                inst->squelch_open.store(bf_squelch_open, std::memory_order_relaxed);
                            }
                        }
                        // Note: When use_eigenvalue_squelch is true, squelch_allows_music stays true
                        // so MUSIC always runs to compute eigenvalue ratio. Squelch state is updated after MUSIC.
                        //
                        // HARD GATE during heimdall calibration (noise source on
                        // + settle hold): the injected noise is a strong coherent
                        // signal that would OPEN every squelch method and spin
                        // the DoA. Skip MUSIC entirely - published DoA freezes,
                        // and the accumulator/covariance/auto-squelch never see
                        // calibration data. Applies to every method, squelch on
                        // or off.
                        if (!wideband_enabled &&
                            squelch_allows_music &&
                            !doa_is_calibrating() &&
                            !doa_retune_hold_active() &&
                            !result.decimated_data.channels.empty() &&
                            result.decimated_data.min_samples > 0 &&
                            inst->music_processor &&
                            !inst->being_deleted.load(std::memory_order_relaxed)) {

                            // Ensure MUSIC processor has correct frequency (fixes startup sync issue)
                            // Check reference channel (0) tuner frequency + the offset THIS
                            // block was decimated with - not the VFO's live offset: a block
                            // decimated just before a VFO move carries the old one, and
                            // retuning MUSIC to the new offset let it into the new frame
                            float expected_freq = static_cast<float>(tuner_frequencies[0].load(std::memory_order_relaxed))
                                                + result.decimated_data.freq_offset_hz;
                            float current_music_freq = inst->music_processor->getEffectiveFrequency();
                            if (std::abs(current_music_freq - expected_freq) > 1000.0f) {
                                inst->music_processor->setFrequency(expected_freq);
                            }

                            // Arm the eigenvalue-squelch publish gate: a closed
                            // squelch freezes the published DoA at the last open
                            // frame (mirrors the FFT method, which skips MUSIC
                            // when closed). Armed in beamforming mode too - MUSIC
                            // keeps computing (fresh ratio), publish freezes, and
                            // steering simply holds the last open bearing.
                            // Auto mode gates on the PREVIOUS frame's learned
                            // threshold (one frame stale is negligible; 0 while
                            // still learning = gate open).
                            {
                                bool gate_armed = use_eigenvalue_squelch &&
                                                  inst->squelch_enabled.load(std::memory_order_relaxed);
                                float gate_thr = 0.0f;
                                if (gate_armed) {
                                    gate_thr = (current_squelch_method == SquelchMethod::EIGENVALUE_AUTO)
                                        ? inst->auto_eigen_threshold.load(std::memory_order_relaxed)
                                        : inst->squelch_eigen_threshold.load(std::memory_order_relaxed);
                                }
                                inst->music_processor->setSquelchGate(gate_armed, gate_thr);

                                // FFT method with beamforming: MUSIC must keep
                                // running (it provides steering), but a closed
                                // squelch still freezes the PUBLISHED DoA - via
                                // the external hold, using the beamformed squelch
                                // state from the previous block.
                                bool fft_bf_hold = !use_eigenvalue_squelch && bf_active &&
                                                   inst->squelch_enabled.load(std::memory_order_relaxed) &&
                                                   !inst->squelch_open.load(std::memory_order_relaxed);
                                inst->music_processor->setPublishHold(fft_bf_hold);
                            }

                            inst->music_processor->processDecimatedIQ(result.decimated_data);
                            // per-packet DoA (ADS-B): the stream the decoder's packets are cut from
                            if (inst->talker_doa && inst->talker_doa->wants_blocks())
                                inst->talker_doa->add_block(result.decimated_data);

                            // Update eigenvalue squelch state if enabled
                            if (use_eigenvalue_squelch && inst->squelch_enabled.load(std::memory_order_relaxed)) {
                                float eigen_ratio = inst->music_processor->getEigenvalueRatio();
                                if (current_squelch_method == SquelchMethod::EIGENVALUE_AUTO) {
                                    // Self-learned threshold; expected_freq is this
                                    // VFO's tuner freq + offset, so a retune OR an
                                    // offset drag resets the learning.
                                    // Learning is allowed only while (a) the FFT
                                    // shows NO clearly visible in-band signal
                                    // (>= 12 dB over the normalized floor, DC bins
                                    // excluded) - that's what disambiguates a
                                    // continuous transmission (freeze learning,
                                    // stay open) from a coherent elevated noise
                                    // floor (learn it, stay closed) - and (b) the
                                    // server isn't calibrating (noise-source data
                                    // is not the environment and must never seed
                                    // the floor).
                                    int fft_ch = active_channel.load(std::memory_order_relaxed);
                                    float auto_bw_hz = inst->decimator->getBandwidthMhz() * 1e6f;
                                    // "No FFT data" (right after a retune reset)
                                    // must read as UNKNOWN, never as "quiet" -
                                    // learning blind is how a settling transient
                                    // seeds a garbage floor.
                                    float in_band_peak_db = FFTProcessor::get_range_peak_db(
                                        fft_ch, inst->frequency_offset_hz, auto_bw_hz,
                                        DecimatorManager::AUTO_EIGEN_FFT_DC_EXCLUDE_BINS);
                                    bool fft_valid = in_band_peak_db > -900.0f;
                                    bool fft_quiet = fft_valid &&
                                        in_band_peak_db <= DecimatorManager::AUTO_EIGEN_FFT_SIGNAL_DB;
                                    bool allow_learn = fft_quiet && !doa_is_calibrating();
                                    DecimatorManager::updateAutoEigenThreshold(
                                        *inst, eigen_ratio, expected_freq, allow_learn);
                                }
                                // Squelch state = the processor's hysteretic gate,
                                // i.e. exactly the state that froze/published this
                                // frame, so UI/audio always agree with the freeze.
                                bool eigen_squelch_open = !inst->music_processor->isSquelchGateClosed();
                                inst->squelch_open.store(eigen_squelch_open, std::memory_order_relaxed);
                            }
                        }

                    } catch (const std::exception& e) {
                        cerr << "Pipeline " << inst->id << " error: " << e.what() << endl;
                    }

                    return result;
                };

            vector<DecimatorManager::ProcessResult> results;
            results.reserve(active_decimators.size());

            // Timed for the dashboard's "decimate" figure: the whole pass up
            // to the barrier (every VFO's decimation + MUSIC task).
            const auto pass_start = steady_clock::now();
            if (active_decimators.size() == 1) {
                // Common case: run inline instead of spawning an OS thread per packet
                results.push_back(run_pipeline(active_decimators[0]));
            } else {
                // Every VFO but the first on a persistent pool, the first
                // inline here, then one barrier. std::async(launch::async)
                // created and joined an OS thread per VFO per packet (~146
                // packets/s). Pool tasks only wait on their decimator's own
                // channel pool, never on this one, so a queued VFO can't
                // deadlock it.
                static ThreadPool vfo_pool(3);
                vector<future<DecimatorManager::ProcessResult>> pipeline_futures;
                pipeline_futures.reserve(active_decimators.size() - 1);
                for (size_t i = 1; i < active_decimators.size(); i++) {
                    const auto inst = active_decimators[i];
                    pipeline_futures.push_back(vfo_pool.submit(
                        [&run_pipeline, inst]() { return run_pipeline(inst); }));
                }
                results.push_back(run_pipeline(active_decimators[0]));
                for (auto& future : pipeline_futures) {
                    results.push_back(future.get());
                }
            }
            decimator_manager.setLastProcessMs(
                duration<double, std::milli>(steady_clock::now() - pass_start).count());

            // Beamforming: steer each active decimator's coherent combine by its
            // OWN MUSIC DoA, and produce a per-decimator beamformed FFT slice.
            // Runs sequentially after the barrier (one beamformer per decimator).
            if (beamforming_needs_processing) {
                bool manual_steer = manual_steering_enabled.load(std::memory_order_relaxed);
                float manual_angle = manual_steering_angle.load(std::memory_order_relaxed);

                for (auto& result : results) {
                    auto& inst = result.instance;
                    if (!inst || !inst->beamformer || !inst->music_processor) continue;
                    if (result.decimated_data.channels.empty()) continue;

                    auto [doa_angle, confidence] = inst->music_processor->getPeakAngleWithConfidence();

                    // Use DoA angle if valid; keep the current steering angle as a
                    // fallback (even weak steering beats none). Confidence threshold
                    // is very low (0.1%) - if MUSIC finds ANY direction, use it.
                    float steering_angle = inst->beamformer->getSteeringAngle();
                    if (manual_steer) {
                        steering_angle = manual_angle;
                    } else if (doa_angle >= 0 && confidence > 0.001f) {
                        // doa_angle is reported in world frame (array offset already
                        // applied); the beamformer steers in array frame, so undo it.
                        // the offset the bearing was computed with: a frozen
                        // DoA keeps the old one while the setting may change
                        float arr = static_cast<float>(doa_angle) - inst->music_processor->getSpectrumArrayOffset();
                        arr = wrap_degrees(arr);
                        steering_angle = arr;
                    }

                    inst->beamformer->setSteeringAngle(steering_angle);

                    // Sync beamformer array config with this decimator's MUSIC
                    inst->beamformer->setArrayTopology(inst->music_processor->getArrayTopology());
                    inst->beamformer->setArrayRadius(inst->music_processor->getArrayRadius());
                    inst->beamformer->setElementSpacing(inst->music_processor->getElementSpacing());
                    if (inst->music_processor->getArrayTopology() == ArrayTopology::CUSTOM &&
                        inst->music_processor->hasValidCustomPositions()) {
                        inst->beamformer->setCustomPositions(inst->music_processor->getCustomPositions());
                    }
                    inst->beamformer->setFrequency(inst->music_processor->getEffectiveFrequency());
                    inst->beamformer->setSampleRate(result.decimated_data.output_rate_hz);

                    result.beamformer_ran = true;
                    if (inst->beamformer->process(result.decimated_data, result.beamformed_samples)) {
                        result.have_beamformed = true;

                        // Per-decimator beamformed FFT for display (center freq from
                        // tuner + this decimator's offset).
                        float center_freq_hz = static_cast<float>(tuner_frequencies[0].load(std::memory_order_relaxed))
                                             + inst->frequency_offset_hz;
                        float sample_rate_hz = result.decimated_data.output_rate_hz;

                        FFTProcessor::process_beamformed_fft(
                            inst->beamformed_fft,
                            result.beamformed_samples.data(),
                            result.beamformed_samples.size(),
                            center_freq_hz,
                            sample_rate_hz
                        );
                    }
                }
            }

            // Digital decoders: fed the same stream as the FM audio would be -
            // the beamformer output when beamforming ran for this VFO (empty
            // while FD-DAS fills: skip, never mix in raw blocks), else the
            // listened-to channel. Before the FM queueing, which moves the
            // beamformed samples out.
            if (digital_needs_processing) {
                for (auto& result : results) {
                    const auto& inst = result.instance;
                    if (!inst || inst->digital_mode.load(std::memory_order_relaxed) == 0) continue;
                    if (result.decimated_data.channels.empty() || result.decimated_data.min_samples == 0) continue;
                    auto dd = inst->getDigital();
                    if (!dd) continue;
                    const vector<complex<float>>* src = nullptr;
                    if (result.beamformer_ran) {
                        if (result.have_beamformed) src = &result.beamformed_samples;
                    } else {
                        int idx = (wideband_enabled || fm_only) ? 0 : current_active;
                        if (idx < static_cast<int>(result.decimated_data.channels.size()))
                            src = &result.decimated_data.channels[idx].samples;
                    }
                    if (!src || src->empty()) continue;
                    int tuner = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed) : 0;
                    if (tuner < 0 || tuner >= MAX_CHANNELS) tuner = 0;
                    double rf = static_cast<double>(exact_tuner_hz(tuner)) + result.decimated_data.freq_offset_hz;
                    dd->push(src->data(), src->size(), result.decimated_data.output_rate_hz, rf,
                             result.decimated_data.stream_pos);
                }
            }

            // Now queue FM data (after MUSIC is done to avoid data race)
            // If beamforming produced output, use that instead of single-channel data
            for (auto& result : results) {
                if (!result.decimated_data.channels.empty() && result.decimated_data.min_samples > 0) {

                    // If this is the FM source decimator, queue FM data
                    // In wideband mode: only 1 channel at index 0 (contains active_channel data)
                    // In normal mode: multiple channels, access by current_active index
                    if (result.is_fm_source && fm_needs_processing) {
                        DecimatedFMWorkItem fm_item;
                        fm_item.channel_id = current_active;  // Logical channel ID (user-facing)
                        fm_item.decimation_used = result.decimated_data.decimation_used;
                        fm_item.output_rate_hz = result.decimated_data.output_rate_hz;
                        fm_item.timestamp = steady_clock::now();

                        // Beamformer running for this decimator: its output only.
                        // FD-DAS emits in 128-sample hops after a 256-sample fill,
                        // so a short block (narrow bandwidths) often yields
                        // nothing; falling back to the raw channel for those
                        // blocks interleaved current raw audio with the delayed
                        // beamformed stream (~1.36x real-time input, scrambled,
                        // clicks at every switch). Otherwise: the single channel.
                        if (result.beamformer_ran) {
                            if (result.have_beamformed && !result.beamformed_samples.empty()) {
                                fm_item.decimated_samples = std::move(result.beamformed_samples);
                            }
                        } else {
                            // Single-channel blocks (wideband, FM only) carry it at 0
                            int channel_idx = (wideband_enabled || fm_only) ? 0 : current_active;
                            if (channel_idx < static_cast<int>(result.decimated_data.channels.size()) &&
                                !result.decimated_data.channels[channel_idx].samples.empty()) {
                                fm_item.decimated_samples = result.decimated_data.channels[channel_idx].samples;
                            }
                        }

                        // OPTIMIZATION: ConcurrentQueue is already lock-free, no mutex needed
                        // Only the condition variable notification needs to be done
                        if (!fm_item.decimated_samples.empty() && fm_decimated_queue.size_approx() < 20) {
                            fm_decimated_queue.enqueue(std::move(fm_item));
                            fm_decimated_available.notify_one();
                        }
                    }
                }
            }

        }

        // FFT Processing
        // Wideband / independent mode: FFT for all channels (stitched, or one
        // spectrum per tuner). Coherent: only the active channel
        bool is_wideband = multi_tuner_mode();

        if (is_wideband) {
            // Wideband mode: Process FFT for all channels
            int channels_to_process = min(static_cast<int>(raw_packet.num_channels), MAX_CHANNELS);
            int channels_enqueued = 0;

            for (int ch = 0; ch < channels_to_process; ch++) {
                if (ch >= static_cast<int>(raw_packet.channel_iq_data.size())) continue;

                const auto& float_data = raw_packet.channel_iq_data[ch];
                if (float_data.empty()) continue;

                size_t num_samples = float_data.size();

                {
                    lock_guard<mutex> pb_lock(persistent_buffer_mutex);

                    if (persistent_buffer_size + num_samples > persistent_data_buffer.size() * 0.8) {
                        persistent_buffer_size = 0;
                        // New generation: queued items referencing the region
                        // about to be overwritten will be discarded by workers
                        persistent_buffer_generation.fetch_add(1, std::memory_order_release);
                    }

                    if (persistent_buffer_size + num_samples <= persistent_data_buffer.size()) {
                        // OPTIMIZATION: Direct copy - no conversion! (was float→uint8→float, huge waste)
                        std::complex<float>* dest = persistent_data_buffer.data() + persistent_buffer_size;
                        std::memcpy(dest, float_data.data(), num_samples * sizeof(std::complex<float>));

                        FFTWorkItem work_item(ch, dest, num_samples,
                                              persistent_buffer_generation.load(std::memory_order_relaxed));

                        if (fft_work_queue.size_approx() < 100) {
                            fft_work_queue.enqueue(work_item);
                            channels_enqueued++;
                        }

                        persistent_buffer_size += num_samples;
                    }
                }
            }

            // After enqueueing work for ALL channels, notify ALL FFT threads at once
            // This ensures all channels get processed in parallel
            if (channels_enqueued > 0) {
                fft_work_available.notify_all();
            }
        } else if (fft_needs_active_channel) {
            // Normal mode: Process FFT only for active channel
            const auto& float_data = raw_packet.channel_iq_data[current_active];

            // Check if data is still available (might have been moved)
            if (!float_data.empty()) {
                size_t num_samples = float_data.size();

                {
                    lock_guard<mutex> pb_lock(persistent_buffer_mutex);

                    if (persistent_buffer_size + num_samples > persistent_data_buffer.size() * 0.8) {
                        persistent_buffer_size = 0;
                        // New generation: queued items referencing the region
                        // about to be overwritten will be discarded by workers
                        persistent_buffer_generation.fetch_add(1, std::memory_order_release);
                    }

                    if (persistent_buffer_size + num_samples <= persistent_data_buffer.size()) {
                        // OPTIMIZATION: Direct copy - no conversion! (was float→uint8→float, huge waste)
                        std::complex<float>* dest = persistent_data_buffer.data() + persistent_buffer_size;
                        std::memcpy(dest, float_data.data(), num_samples * sizeof(std::complex<float>));

                        FFTWorkItem work_item(current_active, dest, num_samples,
                                              persistent_buffer_generation.load(std::memory_order_relaxed));

                        // OPTIMIZATION: ConcurrentQueue is already lock-free, no mutex needed
                        // Removing mutex reduces contention and improves performance by 10-20%
                        if (fft_work_queue.size_approx() < 50) {
                            fft_work_queue.enqueue(work_item);
                            fft_work_available.notify_one();  // notify_one() is already thread-safe
                        }

                        persistent_buffer_size += num_samples;
                    }
                }
            }
        }

        global_stats.increment_operations(thread_idx);
        global_stats.print_stats_if_time(thread_idx);
    }

    cout << "Decimation processor thread exiting" << endl;
}


void DataReceiver::fft_processor_thread() {
    // OPTIMIZATION: Register thread and get index for lock-free stats access
    size_t thread_idx = global_stats.register_thread("FFTProcessor");
    
    while (running) {
        // OPTIMIZATION: Only lock for condition variable wait, not for dequeue
        // ConcurrentQueue operations are lock-free and don't need mutex protection
        {
            unique_lock<mutex> lock(fft_queue_mutex);
            if (!fft_work_available.wait_for(lock, milliseconds(100), []{
                return !running.load(std::memory_order_relaxed) || fft_work_queue.size_approx() > 0;
            })) {
                continue;
            }
        } // Lock released here

        if (!running.load(std::memory_order_relaxed)) break;

        // Dequeue without holding mutex (ConcurrentQueue is lock-free)
        FFTWorkItem work_item;
        if (!fft_work_queue.try_dequeue(work_item)) continue;

        auto age = steady_clock::now() - work_item.timestamp;
        if (age > milliseconds(500)) continue;

        FFTProcessor::process_channel_fft(work_item.data, work_item.num_samples, work_item.channel,
                                          work_item.buffer_generation);
        global_stats.increment_operations(thread_idx);
        global_stats.print_stats_if_time(thread_idx);
    }
}

void DataReceiver::fm_processor_thread() {
    // OPTIMIZATION: Register thread and get index for lock-free stats access
    size_t thread_idx = global_stats.register_thread("FMProcessor");

    // Track consecutive timeouts to detect stalls
    int consecutive_timeouts = 0;

    // Demodulated audio, reused across packets (capacity survives each call)
    std::vector<float> audio_samples;

    while (running) {
        if (!fm_enabled.load(std::memory_order_relaxed)) {
            this_thread::sleep_for(milliseconds(50));
            consecutive_timeouts = 0;  // Reset on FM disable
            continue;
        }

        // OPTIMIZATION: Only lock for condition variable wait, not for dequeue
        // ConcurrentQueue operations are lock-free and don't need mutex protection
        {
            unique_lock<mutex> lock(fm_decimated_mutex);
            if (!fm_decimated_available.wait_for(lock, milliseconds(100), []{
                return !running.load(std::memory_order_relaxed) || fm_decimated_queue.size_approx() > 0;
            })) {
                consecutive_timeouts++;
                // Log if FM data stops flowing
                if (consecutive_timeouts == 20) {  // 2 seconds of no data
                    cout << "FM processor: No data for 2s - check decimation pipeline" << endl;
                } else if (consecutive_timeouts % 100 == 0) {  // Every 10 seconds
                    cout << "FM processor: No data for " << (consecutive_timeouts / 10) << "s" << endl;
                }
                continue;
            }
        } // Lock released here

        // Detect if we're recovering from a long gap in data
        bool recovering_from_gap = consecutive_timeouts >= 15;  // 1.5 seconds of no data
        consecutive_timeouts = 0;  // Reset on successful wait

        if (!running.load(std::memory_order_relaxed)) break;

        // Dequeue without holding mutex (ConcurrentQueue is lock-free)
        DecimatedFMWorkItem work_item;
        if (!fm_decimated_queue.try_dequeue(work_item)) {
            continue;
        }

        auto age = steady_clock::now() - work_item.timestamp;

        // If recovering from a gap, drain any stale items first
        if (recovering_from_gap) {
            while (age > milliseconds(100)) {
                if (!fm_decimated_queue.try_dequeue(work_item)) break;
                age = steady_clock::now() - work_item.timestamp;
            }
            fm_demod.reset_audio_buffer();
        }

        // Drop stale items
        if (age > milliseconds(200)) {
            global_stats.increment_errors(thread_idx);
            continue;
        }

        // Only update sample rate if it actually changed
        static float last_fm_rate = 0.0f;
        if (work_item.output_rate_hz != last_fm_rate) {
            fm_demod.setInputSampleRate(work_item.output_rate_hz);
            last_fm_rate = work_item.output_rate_hz;
        }

        fm_demod.process_decimated_samples(std::move(work_item.decimated_samples), audio_samples);

        // Digital demod: the FM output only sets how many samples this block
        // is worth; the audio itself is the VFO's decoded digital voice
        // (silence when there is none). Squelch doesn't apply - the decoder
        // only produces audio for valid voice frames.
        if (!audio_samples.empty()) {
            auto src_dec = decimator_manager.getDecimator(decimator_manager.getFMDecimatorId());
            if (src_dec && src_dec->demod_mode.load(std::memory_order_relaxed) == DemodulatorMode::DIGITAL) {
                auto dd = src_dec->getDigital();
                if (dd) dd->pull_voice(audio_samples.data(), audio_samples.size());
                else std::fill(audio_samples.begin(), audio_samples.end(), 0.0f);
                fm_demod.add_audio_samples(audio_samples);
                global_stats.increment_operations(thread_idx);
                global_stats.print_stats_if_time(thread_idx);
                continue;
            }
        }

        if (!audio_samples.empty()) {
            // Check squelch for the FM source decimator - if enabled and closed, output silence
            bool squelch_allows_audio = true;
            int fm_dec_id = decimator_manager.getFMDecimatorId();
            auto fm_decimator = decimator_manager.getDecimator(fm_dec_id);
            if (fm_decimator && fm_decimator->squelch_enabled.load(std::memory_order_relaxed)) {
                float squelch_level = fm_decimator->squelch_level.load(std::memory_order_relaxed);

                // Check squelch method (per decimator). Wideband / independent:
                // always FFT on the VFO's own tuner (no MUSIC, no beamformer)
                const bool multi = multi_tuner_mode();
                SquelchMethod audio_squelch_method = multi ? SquelchMethod::FFT : static_cast<SquelchMethod>(
                    fm_decimator->squelch_method.load(std::memory_order_relaxed));

                if (audio_squelch_method == SquelchMethod::EIGENVALUE ||
                    audio_squelch_method == SquelchMethod::EIGENVALUE_AUTO) {
                    // Eigenvalue-based squelch: the pipeline task maintains the
                    // hysteretic gate state in squelch_open (same state that
                    // freezes the DoA publish), so audio simply follows it -
                    // audio and DoA can never disagree about open/closed.
                    squelch_allows_audio = fm_decimator->squelch_open.load(std::memory_order_relaxed);
                } else if (!multi && beamforming_enabled.load(std::memory_order_relaxed) &&
                           fm_decimator->beamformed_fft.valid.load(std::memory_order_acquire)) {
                    // FFT-based squelch with beamforming: use the FM decimator's
                    // beamformed signal for better SNR
                    squelch_allows_audio = FFTProcessor::check_squelch_beamformed(
                        fm_decimator->beamformed_fft, squelch_level);
                } else {
                    // FFT-based squelch: fall back to single-channel squelch
                    int check_channel = multi ? fm_decimator->tuner_channel.load(std::memory_order_relaxed)
                                              : active_channel.load(std::memory_order_relaxed);
                    float offset_hz = fm_decimator->frequency_offset_hz;
                    float bw_hz = fm_decimator->decimator->getBandwidthMhz() * 1e6f;
                    squelch_allows_audio = FFTProcessor::check_squelch_in_range(
                        check_channel, squelch_level, offset_hz, bw_hz);
                }
                fm_decimator->squelch_open.store(squelch_allows_audio, std::memory_order_relaxed);
            }

            if (squelch_allows_audio) {
                fm_demod.add_audio_samples(audio_samples);
            } else {
                // Output silence when squelched to maintain audio timing
                std::fill(audio_samples.begin(), audio_samples.end(), 0.0f);
                fm_demod.add_audio_samples(audio_samples);
            }
            global_stats.increment_operations(thread_idx);
        }

        global_stats.print_stats_if_time(thread_idx);
    }

    cout << "FM processor thread exiting" << endl;
}

// Serialize all control-socket access: send_control_command() is called from
// both the uWS control thread and the scanner thread, and drain_control_socket()
// runs on the status thread. TCPClient has no internal locking, so without this
// two JSON commands could interleave byte-wise on the socket and
// connect/disconnect could race.
static mutex control_socket_mutex;

// Discard whatever heimdall has pushed at us. It answers every command and
// broadcasts a status JSON at 2 Hz to every control client; nothing here
// consumes either (channel metadata arrives on the 8091 data stream instead).
// Left unread it fills our 128 KB receive buffer in about two minutes, the
// window closes, and heimdall's control server eventually wedges on a blocking
// send() - taking its accept loop and command handling down with it.
// Caller must hold control_socket_mutex.
static void drain_control_socket_locked() {
    if (!control_client.is_connected()) return;

    char scratch[4096];
    while (true) {
        ssize_t n = control_client.receive_data(scratch, sizeof(scratch));
        if (n > 0) {
            if (static_cast<size_t>(n) < sizeof(scratch)) break;  // drained
            continue;                                             // more waiting
        }
        if (n == 0) {          // peer closed
            control_client.disconnect();
            break;
        }
        if (errno == EINTR) continue;
        break;                 // EAGAIN/EWOULDBLOCK: nothing buffered
    }
}

void DataReceiver::drain_control_socket() {
    lock_guard<mutex> lock(control_socket_mutex);
    drain_control_socket_locked();
}

void DataReceiver::send_control_command(const string& cmd) {
    lock_guard<mutex> lock(control_socket_mutex);

    // Keep the receive side empty before writing: a full receive buffer here
    // is what back-pressures heimdall into its stall.
    drain_control_socket_locked();

    // Newline-terminate: heimdall's control server splits commands on
    // newlines. Without it, back-to-back commands (the startup settings
    // replay, scanner hops) coalesce into one TCP segment and heimdall
    // parses only one command out of the pile.
    const string framed = cmd + "\n";

    // Two attempts: the connection is opened lazily on the first command
    // (and re-opened after a drop), and a single silently-dropped command
    // desyncs state the caller assumes was applied (e.g. the startup
    // settings replay's MIXER_SIDE landing while heimdall holds another
    // side).
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!control_client.is_connected()) {
            if (!control_client.connect_to_port(TCP_CONTROL_PORT)) continue;
            control_client.set_nonblocking(true);
        }
        if (control_client.send_data(framed.c_str(), framed.length())) return;
        control_client.disconnect();
    }
    cerr << "Control command dropped (server unreachable): " << cmd << endl;
}

RawDataBuffer::Stats DataReceiver::get_raw_buffer_stats() {
    return raw_data_buffer.get_stats();
}

void DataReceiver::print_raw_buffer_status() {
    raw_data_buffer.print_stats("RawDataBuffer");
}

void DataReceiver::flush_data_pipeline() {
    // Clear raw data buffer (discard all buffered IQ packets)
    raw_data_buffer.clear();

    // Clear FFT work queue (drain all pending FFT work items)
    FFTWorkItem dummy_work;
    while (fft_work_queue.try_dequeue(dummy_work)) {
    }
}

