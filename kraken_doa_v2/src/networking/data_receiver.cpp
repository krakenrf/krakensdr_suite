// ============================================
// src/networking/data_receiver.cpp - OPTIMIZED
// ============================================

#include "wb_board.hpp"
#include "app_version.hpp"
#include "antenna_bias.hpp"
#include "networking/data_receiver.hpp"
#include "playback.hpp"
#include "recorder.hpp"
#include "ai_capture.hpp"
#include "sweep.hpp"
#include "utils/thread_pool.hpp"
#include "utils/parse_num.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/audio_mixer.hpp"
#include "signal_processing/shared_decimator.hpp"
#include "signal_processing/beamformer.hpp"
#include "signal_processing/noise_floor.hpp"
#include "signal_processing/bin_doa.hpp"
#include "sigclass_service.hpp"
#include "decimator_manager.hpp"
#include "doa_logger.hpp"
#include "voice2text.hpp"
#include "networking/tcp_client.hpp"
#include "utils/system_stats.hpp"
#include "utils/ring_buffer.hpp"
#include "utils/raw_data_buffer.hpp"
#include "utils/iq_converter.hpp"
#include "channel_manager.hpp"
#include "scanner_manager.hpp"
#include "trunk_follow.hpp"
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
// debug playback: packets waiting for the pipeline before the next is read
// (~110 ms at 2.4 MHz; data_receiver_thread)
constexpr size_t PLAYBACK_MAX_QUEUED = 16;

// Queue for decimated FM data: one item per VFO heard (🔊) per block
struct DecimatedFMWorkItem {
    vector<complex<float>> decimated_samples;
    int vfo_id = -1;   // the VFO (audio_mixer channel)
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

// Auto detect's internal streams (dig::StreamWant; docs/dev/digital-decoders.md
// "Internal streams"): a VFO whose decoder in Auto detect runs plugins that
// need more bandwidth than the VFO has gets, per width its decoder wants, a
// SharedDecimator of that width at the VFO's offset - decimated in the VFO's
// pass, handed to the decoder with the VFO's block. Not VFOs: no
// DecimatorInstance, no MUSIC / squelch / list / settings. Owned by the
// decimation thread alone (created, reconfigured, freed there); a set goes
// as soon as its VFO no longer wants one (decoder off / fixed / VFO wide
// enough / deleted)
namespace {
struct AutoStream {
    int width = 0;
    bool mc = false;                                  // every antenna (a multichannel plugin on it)
    std::unique_ptr<SharedDecimator> dec;
    double delay_s = 0;                               // its cascade's delay (groupDelaySeconds)
    SharedDecimator::MultiChannelDecimated out;       // this block
    size_t src = 0;                                   // the listened-to channel's index in out
    vector<complex<int16_t>> mcbuf;                   // every antenna packed for the decoder
};
struct AutoStreamSet {
    weak_ptr<DecimatorManager::DecimatorInstance> vfo;
    const DecimatorManager::DecimatorInstance* key = nullptr;
    uint32_t gen = 0;
    bool have_gen = false;
    vector<AutoStream> streams;
    int vfo_bw = 0;
    double vfo_delay_s = 0;                           // the VFO's own cascade's delay
    bool seen = false;                                // in this block's pass
};

// The set's decimators follow the decoder's wanted widths (a width kept keeps
// its filter state; only a change allocates)
void sync_auto_streams(AutoStreamSet& set, const dig::DigitalDecoder& dd, int vfo_id) {
    const uint32_t gen = dd.streams_gen();
    if (set.have_gen && gen == set.gen) return;
    set.gen = gen;
    set.have_gen = true;
    dig::StreamWant w[dig::DigitalDecoder::MAX_STREAMS];
    const int n = dd.wanted_streams(w);
    vector<AutoStream> next;
    next.reserve(static_cast<size_t>(n));
    std::string list;
    for (int i = 0; i < n; i++) list += (i ? ", " : "") + std::to_string(w[i].width_hz) + (w[i].mc ? " Hz (every antenna)" : " Hz");
    cout << "Auto detect: VFO " << vfo_id << " internal streams: " << (n ? list : "none") << endl;
    for (int i = 0; i < n; i++) {
        AutoStream s;
        for (auto& o : set.streams)
            if (o.dec && o.width == w[i].width_hz) s = std::move(o);
        if (!s.dec) {
            s.width = w[i].width_hz;
            s.dec = make_unique<SharedDecimator>();
            s.dec->initialize(MAX_CHANNELS);
            s.dec->setBandwidthHz(s.width);
            s.delay_s = SharedDecimator::groupDelaySeconds(s.width);
        }
        s.mc = w[i].mc;
        next.push_back(std::move(s));
    }
    set.streams = std::move(next);   // the ones not wanted any more go here
}
}  // namespace


// heimdall's calibration state for the pages: the scanner manager's phase
// state / noise flag (system_status, the assistant) and {"cal_live":...} at
// once on a change. The decimation thread calls it per packet; in sweep mode
// (no packet reaches it) the data receiver does - the chip showed the last
// state from before the sweep for the whole session
static void note_calibration_state(uint32_t phase_state, uint32_t noise_source) {
    scanner_manager.updatePhaseCalibrationStatus(phase_state, noise_source);
    static std::atomic<uint32_t> last_cal{0xFFFFFFFFu};
    const uint32_t cal = (phase_state << 1) | (noise_source ? 1u : 0u);
    if (last_cal.exchange(cal) != cal)
        WebSocketServer::broadcast_json_message(
            "{\"cal_live\":{\"phase_state\":" + to_string(phase_state) +
            ",\"noise_source\":" + (noise_source ? "true" : "false") + "}}");
}

void DataReceiver::data_receiver_thread() {
    // OPTIMIZATION: Register thread and get index for lock-free stats access
    size_t thread_idx = global_stats.register_thread("DataReceiver");

    // Initialize the decimator manager with 1 default decimator
    decimator_manager.initialize(MAX_CHANNELS, 1);

    vector<uint8_t> buffer(4 * 1024 * 1024);
    size_t bytes_in_buffer = 0;
    // bytes were lost before the next packet (a resync, a reconnect, a
    // source switch): the Signal ID tap restarts its snapshot (sigclass_service.hpp)
    bool stream_gap = true;

    // Upper bound for the per-channel sample count in a packet header
    constexpr size_t MAX_SAMPLES_PER_CHANNEL = 65536;

    // Track server-side scanner frequency changes
    uint32_t last_frequency_change_counter = 0;
    bool first_packet = true;

    // Restore persisted settings once, on the first successful connection (the
    // decimators now exist and the server is reachable for tuner commands).
    // Not repeated on reconnect, so a transient drop won't stomp live state.
    bool settings_restored = false;
    // The replay waits for the first packet header (at most REPLAY_WAIT_MS
    // after the connect): the saved settings must meet heimdall's channel
    // count and generic-RTL-SDR flag, not the pre-connect defaults - a
    // KrakenSDR session's coherent mode / 5 tuners against one plain dongle
    // sent commands heimdall could only refuse
    bool replay_pending = false;
    int64_t replay_deadline_ms = 0;
    constexpr int64_t REPLAY_WAIT_MS = 3000;
    auto start_replay = [&]() {
        replay_pending = false;
        if (loop) loop->defer([] { ControlHandler::apply_persisted_settings(); });
    };

    cout << "Data receiver thread started with optimized scalar IQ conversion" << endl;
    cout << "Using compiler auto-vectorized simple scalar loop (8.5% faster than NEON)" << endl;
    cout << "Server-side discrete scanner detection enabled" << endl;

    while (running) {
        // Debug playback (playback.hpp): the packets come from the test
        // transmission instead, heimdall's stream is disconnected meanwhile
        const bool pb = playback::active();
        if (pb) {
            if (data_client.is_connected()) data_client.disconnect();
            raw_data_buffer.clear();
            cout << "Data receiver: playing the debug playback (heimdall disconnected)" << endl;
        } else if (!data_client.connect_to_port(TCP_DATA_PORT)) {
            global_stats.increment_errors(thread_idx);
            this_thread::sleep_for(seconds(1));
            continue;
        } else {
            raw_data_buffer.clear();
            cout << "Connected to Heimdall data port" << endl;
            // sweep mode: the plan again (a restarted heimdall may hold another)
            if (settings_restored && sweep_mode_enabled.load() && loop)
                loop->defer([] { ControlHandler::send_sweep_plan(); });
        }

        if (!pb && !settings_restored) {
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
            replay_pending = true;   // on the first packet (start_replay)
            replay_deadline_ms = steady_now_ms() + REPLAY_WAIT_MS;
            settings_restored = true;
        }

        // Fresh connection = fresh stream: discard any partial packet left
        // over from the previous connection so old and new bytes never splice
        // into one structurally-valid-but-garbage packet.
        bytes_in_buffer = 0;
        stream_gap = true;

        while (running) {
            if (playback::active() != pb) break;   // source switched
            if (replay_pending && !pb && steady_now_ms() >= replay_deadline_ms) {
                cerr << "No packet from heimdall " << REPLAY_WAIT_MS / 1000
                     << " s after connecting - restoring the saved settings anyway" << endl;
                start_replay();
            }
            // a playback is a file, not a receiver: it never outruns the
            // pipeline. A machine too busy for 2.4 MHz x 5 antennas in real
            // time plays it slower instead - the full raw buffer dropped
            // whole packets (25-40 % on a Pi 5 running a second stack), and
            // the decoders whose bursts span packets (ISM devices) failed
            // their checks on the cut pieces
            if (pb) {
                while (running && playback::active() && raw_data_buffer.size() >= PLAYBACK_MAX_QUEUED)
                    this_thread::sleep_for(milliseconds(2));
            }
            ssize_t bytes_read = pb ? playback::read(buffer.data() + bytes_in_buffer, buffer.size() - bytes_in_buffer)
                                    : data_client.receive_data(
                buffer.data() + bytes_in_buffer, 
                buffer.size() - bytes_in_buffer
            );
            
            if (pb && bytes_read <= 0) continue;
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
                    stream_gap = true;
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
                // Bits 10 / 11 / 13: heimdall's operating mode (wideband scan /
                // independent / sweep - sweep comes with 0x800, so a client
                // that doesn't know it sees independent tuners), acted on after
                // the sanity gate below
                const int hmode = (phase_comp_state & 0x2000u) ? 3 : (phase_comp_state & 0x800u) ? 2
                                : (phase_comp_state & 0x400u) ? 1 : 0;
                // Bit 12: generic RTL-SDRs, no KrakenSDR (no coherent mode).
                // Stored after the sanity gate below (a false magic match
                // must not flip it); bits above it are ignored
                const bool generic_bit = (phase_comp_state & 0x1000u) != 0;
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
                    stream_gap = true;
                    continue;
                }

                // Generic RTL-SDR mode changed (first packet of a generic
                // heimdall, or a KrakenSDR heimdall after it; a debug playback
                // is a coherent array - its packets never set the bit): the
                // pages get the operating_mode message with the new value
                if (server_generic_rtlsdr.exchange(generic_bit, std::memory_order_relaxed) != generic_bit) {
                    if (generic_bit)
                        cout << "heimdall runs generic RTL-SDRs (no KrakenSDR): coherent mode unavailable" << endl;
                    else if (!pb)
                        cout << "heimdall runs a KrakenSDR array: coherent mode available" << endl;
                    if (loop) loop->defer([] { ControlHandler::on_generic_rtlsdr_changed(); });
                }
                // heimdall's mode: only acted on once this heimdall has shown
                // it sends the bits (an older one never sets either). After
                // the generic flag: in generic mode a client in coherent mode
                // follows at once (note_server_mode)
                {
                    static bool mode_bits_seen = false;
                    if (hmode) mode_bits_seen = true;
                    // sweep mode asked for: heimdall's header shows it soon, or
                    // heimdall doesn't know it (an older one: fall back)
                    if (!pb) ControlHandler::note_sweep_header(hmode);
                    if (mode_bits_seen) ControlHandler::note_server_mode(hmode);
                }
                // Sweep mode: the tuners hop ~20 times a second - none of the
                // per-retune lines below
                const bool sweeping = sweep_mode_enabled.load(std::memory_order_relaxed);

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
                server_retuning.store(is_retuning, std::memory_order_relaxed);   // (the saved-frequency scan)

                if (was_retuning && !is_retuning) {
                    // Retuning just completed
                    if (!sweeping) cout << "[RETUNING] *** RETUNING COMPLETE *** Data flow should resume now!" << endl;

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
                    }
                }

                // Update frequency change counter tracking. Also arms the DoA
                // retune hold: the retuning_in_progress pulse can fall entirely
                // between packets (a tuner reprogram takes ~10-50 ms), but the
                // counter bump is always visible (the post-retune cooldown
                // after it is held on heimdall's phase state).
                if (!first_packet && frequency_change_counter != last_frequency_change_counter) {
                    if (!sweeping)
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
                
                // every whole packet is numbered (a jump after the decimation = lost packets)
                static uint64_t packet_seq = 0;
                packet_seq++;
                RawDataPacket raw_packet;
                raw_packet.seq = packet_seq;
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
                    raw_packet.freq_hz[ch] = frequency_hz;   // where its spectrum is (FFTWorkItem::freq_hz)

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
                        if (!sweeping)
                            cout << "[DoA] Reference frequency changed ("
                                 << last_ref_freq_hz / 1e6f << " -> " << ref_freq_hz / 1e6f
                                 << " MHz) - holding DoA until heimdall recalibrates" << endl;
                    }
                    if (ref_freq_hz != 0.0f) last_ref_freq_hz = ref_freq_hz;
                }

                // 📼 full-band IQ recording: the packet as heimdall sent it
                // (retuning packets only mark a gap)
                if (rec::full_active()) {
                    float rf[MAX_CHANNELS], rg[MAX_CHANNELS];
                    for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++) {
                        rf[ch] = ch_freq_hz[ch];
                        rg[ch] = EndianUtils::read_le_float(buffer.data(), offset + 32 + ch * 8 + 4);
                    }
                    rec::PacketInfo pi;
                    pi.phase_state = phase_comp_state;
                    pi.noise = noise_source != 0;
                    pi.retuning = is_retuning;
                    pi.freq_hz = rf;
                    pi.gain_db = rg;
                    rec::on_packet(buffer.data() + offset + header_size, channels, samples, pi);
                }

                // 🤖 the AI assistant's IQ capture (capture_iq): the wanted
                // channels' bytes, copied for its worker (ai_capture.hpp)
                if (aicap::active()) {
                    float rf[MAX_CHANNELS], rg[MAX_CHANNELS];
                    for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++) {
                        rf[ch] = ch_freq_hz[ch];
                        rg[ch] = EndianUtils::read_le_float(buffer.data(), offset + 32 + ch * 8 + 4);
                    }
                    aicap::on_packet(buffer.data() + offset + header_size, std::min<uint32_t>(channels, MAX_CHANNELS), samples,
                                     noise_source != 0, is_retuning, rf, rg);
                }

                // Signal ID (sigclass_service.hpp): one channel's bytes into
                // the classifier's snapshot while a capture is armed (one
                // atomic load per packet otherwise). Here every packet passes,
                // heimdall's and a playback's, retuning and noise ones included (in
                // sweep mode the worker is parked: never armed - sigclass_service.cpp)
                if (sigsvc::armed())
                    sigsvc::on_packet(buffer.data() + offset + header_size, channels, samples, ch_freq_hz.data(),
                                      noise_source != 0, is_retuning, stream_gap);
                stream_gap = false;

                // Sync active_num_elements from the wire header BEFORE the
                // gates below, so a runtime element-count change on the server
                // applies from this very packet (MUSIC, beamformer and the UI
                // all follow this atomic).
                // One channel is a single generic RTL-SDR dongle (independent
                // mode): the tuner count everything outside coherent follows
                // (panes, TUNER_FREQ, the saved-frequency scan). MUSIC and the
                // beamformer need >= 2 and stay off (packet_elements >= 2).
                // Called after the retuning skip below, as before; in sweep
                // mode for every packet (the sweep's plan follows the count)
                auto sync_elements = [&]() {
                    int elems = active_num_elements.load(std::memory_order_relaxed);
                    if (static_cast<int>(channels) != elems && channels >= 1 && channels <= MAX_CHANNELS) {
                        cout << "Active elements synced from server: " << elems << " → " << channels << endl;
                        active_num_elements.store(channels, std::memory_order_release);
                        elems = static_cast<int>(channels);
                        // VFOs on a tuner that is gone move to tuner 0 (and the sweep's plan follows)
                        if (loop) loop->defer([] { ControlHandler::clamp_vfo_tuners(); });
                    }
                    if (replay_pending && !pb) start_replay();   // the count + flags are heimdall's now
                    return elems;
                };

                // Sweep mode: the sweep engine is the only consumer (sweep.hpp) -
                // nothing is converted, decimated, FFT'd or demodulated here
                if (sweeping && !pb) {
                    for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++)
                        tuner_frequencies[ch].store(static_cast<uint64_t>(ch_freq_hz[ch]), std::memory_order_relaxed);
                    sync_elements();
                    note_calibration_state(phase_comp_state, noise_source);
                    // the common gain as heimdall runs it: the pages learn a change
                    // (another page's, the assistant's) from the sweep message
                    // (with a gain table a header's gain is that hop's region's, not the common one)
                    if (!sweep::regions_active() && sweep::note_gain(EndianUtils::read_le_float(buffer.data(), offset + 32 + 4), true) && loop)
                        loop->defer([] { WebSocketServer::broadcast_json_message(sweep::state_json()); });
                    std::array<float, MAX_CHANNELS> ch_gain{};
                    for (uint32_t ch = 0; ch < channels && ch < MAX_CHANNELS; ch++)
                        ch_gain[ch] = EndianUtils::read_le_float(buffer.data(), offset + 32 + ch * 8 + 4);
                    sweep::on_packet(buffer.data() + offset + header_size, channels, samples, ch_freq_hz.data(), ch_gain.data(),
                                     frequency_change_counter, current_group_index, is_retuning);
                    num_channels = channels;
                    data_ready = true;
                    offset += packet_size;
                    continue;
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

                const int current_elements = sync_elements();

                // OPTIMIZATION: Determine which channels to convert upfront
                // Use relaxed memory ordering for 5-10% faster atomic loads (flags don't need strict ordering)
                int current_active = active_channel.load(std::memory_order_relaxed);
                // a 📼 VFO-band IQ recording needs every antenna too
                // (and 🎨 the bearing spectrum: bin_doa.hpp)
                bool need_doa = (static_cast<int>(channels) >= current_elements &&
                                 (doa_enabled.load(std::memory_order_relaxed) || rec::band_vfo() >= 0 ||
                                  bin_doa::wanted()));
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

                // Noise floor, coherent: a few times a second a second antenna
                // for the coherence test (noise_floor.hpp) - with DoA off only
                // the FFT channel is converted otherwise
                if (!is_wideband && active_in_packet && channels >= 2 && nfloor::coherence_due()) {
                    const int other = (current_active + 1) % static_cast<int>(channels);
                    if (other < MAX_CHANNELS && !channels_to_convert[other]) {
                        channels_to_convert[other] = true;
                        num_channels_to_convert++;
                    }
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

    // Auto detect's internal streams, per VFO that wants them (this thread
    // only); vfo_auto = each VFO's set in the block being processed
    vector<AutoStreamSet> auto_sets;
    vector<AutoStreamSet*> vfo_auto;

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
        // This allows scanner to avoid checking for signal loss during calibration.
        // Calibration-state / noise-flag changes go to the browsers at once
        trunk::note_packet();
        // (the system_status broadcast is every 500 ms - the "(Noise On)"
        // badge lagged a ~0.4 s retune calibration by up to half a second).
        note_calibration_state(raw_packet.phase_compensation_state, raw_packet.noise_source_active);

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
        // Digital voice/data decoders consume their VFO's decimated stream,
        // and so does Voice to Text (voice2text.cpp) on VFOs without a decoder
        const bool digital_needs_processing = decimator_manager.anyDigitalActive();
        const bool transcribe_needs_processing = decimator_manager.anyTranscribeActive();
        // 📼 recorder: a VFO's band of every antenna (IQ), VFOs' audio
        const int rec_band_vfo = wideband_enabled ? -1 : rec::band_vfo();
        const bool rec_audio = rec::audio_active();
        if ((music_needs_processing || fm_needs_processing || beamforming_needs_processing || digital_needs_processing ||
             transcribe_needs_processing || rec_band_vfo >= 0 || rec_audio) &&
            decimator_manager.getDecimatorCount() > 0) {
            // OPTIMIZED PIPELINE: Process decimation + MUSIC in single async tasks
            // This eliminates the synchronization barrier between decimation and MUSIC stages
            // FM / digital only (no MUSIC, no beamforming): the FM audio, the
            // digital decoders and Voice to Text are the only consumers of
            // decimated data - squelch reads the main FFT - so run just the FM
            // source VFO and the VFOs with a digital decoder / transcription,
            // on just the listened-to channel. Every VFO used to decimate
            // every element for nothing.
            // (a VFO-band recording needs every antenna: decimated as with DoA on)
            const bool fm_only = !music_needs_processing && !beamforming_needs_processing && rec_band_vfo < 0;
            vector<shared_ptr<DecimatorManager::DecimatorInstance>> active_decimators;
            for (auto& inst : decimator_manager.getAllDecimators()) {
                // Skip disabled or being-deleted decimators
                if (!inst || !inst->enabled || inst->being_deleted.load(std::memory_order_relaxed)) continue;
                const bool single_wanted = (fm_needs_processing && inst->audio_out.load(std::memory_order_relaxed)) ||
                                           inst->digital_mode.load(std::memory_order_relaxed) != 0 ||
                                           inst->transcribe.load(std::memory_order_relaxed) ||
                                           (rec_audio && rec::audio_wants(inst->id));
                if (fm_only && !single_wanted) {
                    // Not decimated in this mode, but keep its FFT-method
                    // squelch indicator live (it reads the main FFT only; a
                    // VFO without a decoder can't be on the Digital squelch)
                    if (inst->squelch_enabled.load(std::memory_order_relaxed) &&
                        DecimatorManager::effectiveSquelchMethod(*inst) == SquelchMethod::FFT) {
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

            // Each VFO's input channels - pointers straight into raw_packet,
            // no copies: everything below is done before raw_packet goes out
            // of scope, and the decimators only read them. Wideband /
            // independent: 1 channel from the VFO's tuner; FM only: the
            // listened-to channel; else every active element for MUSIC. The
            // offset and the bandwidth are read ONCE per block (the control
            // thread may move the VFO meanwhile), so every channel mixes and
            // filters - and is labelled - with the same ones.
            const size_t num_vfos = active_decimators.size();
            vector<SharedDecimator::VfoBlock> vfo_blocks(num_vfos);
            vector<DecimatorManager::ProcessResult> results(num_vfos);
            for (size_t v = 0; v < num_vfos; v++) {
                const auto& inst = active_decimators[v];
                DecimatorManager::ProcessResult& r = results[v];
                r.decimator_id = inst->id;
                r.audio_out = inst->audio_out.load(std::memory_order_relaxed);
                r.instance = inst;  // kept for the post-barrier beamforming pass

                SharedDecimator::VfoBlock& b = vfo_blocks[v];
                b.decimator = inst->decimator.get();
                b.bandwidth_hz = inst->decimator->getBandwidthHz();
                b.freq_offset_hz = inst->frequency_offset_hz;
                b.out = &r.decimated_data;
                // a channel the packet lacks stays a null entry (an empty result)
                auto take = [&raw_packet, &b](int ch) {
                    if (b.num_channels >= static_cast<size_t>(MAX_CHANNELS)) return;
                    if (ch >= 0 && ch < static_cast<int>(raw_packet.channel_iq_data.size())) {
                        b.ptrs[b.num_channels] = raw_packet.channel_iq_data[ch].data();
                        b.lens[b.num_channels] = raw_packet.channel_iq_data[ch].size();
                    }
                    b.num_channels++;
                };
                if (wideband_enabled) {
                    take(inst->tuner_channel.load(std::memory_order_relaxed));
                } else if (fm_only) {
                    take(current_active);
                } else {
                    for (int ch = 0; ch < packet_elements; ch++) take(ch);
                }
            }

            // Auto detect's internal streams (AutoStreamSet above): extra
            // blocks in the same pass, at the VFO's offset (read once, with
            // its block), on the VFO's channel choice - the listened-to
            // channel (wideband / independent: the VFO's tuner); every
            // antenna only for a stream with a multichannel plugin, when the
            // VFO's own stream would get them (coherent, not FM only). Nothing
            // at all for a VFO that wants none
            for (auto& s : auto_sets) s.seen = false;
            vfo_auto.assign(num_vfos, nullptr);
            for (size_t v = 0; v < num_vfos; v++) {
                const auto& inst = active_decimators[v];
                if (inst->digital_mode.load(std::memory_order_relaxed) != static_cast<int>(dig::Mode::AUTO)) continue;
                const auto dd = inst->getDigital();
                if (!dd || !dd->internal_streams() || !dd->wants_streams()) continue;
                AutoStreamSet* set = nullptr;
                for (auto& s : auto_sets)
                    if (s.key == inst.get() && s.vfo.lock() == inst) set = &s;
                if (!set) {
                    auto_sets.emplace_back();
                    set = &auto_sets.back();
                    set->vfo = inst;
                    set->key = inst.get();
                }
                set->seen = true;
                sync_auto_streams(*set, *dd, inst->id);
                if (set->vfo_bw != vfo_blocks[v].bandwidth_hz) {
                    set->vfo_bw = vfo_blocks[v].bandwidth_hz;
                    set->vfo_delay_s = SharedDecimator::groupDelaySeconds(set->vfo_bw);
                }
            }
            // a set no VFO wants any more goes (its decimators with it)
            auto_sets.erase(std::remove_if(auto_sets.begin(), auto_sets.end(),
                                           [](const AutoStreamSet& s) {
                                               if (!s.seen) cout << "Auto detect: internal streams freed" << endl;
                                               return !s.seen;
                                           }),
                            auto_sets.end());
            for (auto& s : auto_sets)
                for (size_t v = 0; v < num_vfos; v++)
                    if (active_decimators[v].get() == s.key) vfo_auto[v] = &s;
            for (size_t v = 0; v < num_vfos; v++) {
                if (!vfo_auto[v]) continue;
                const auto& inst = active_decimators[v];
                for (auto& st : vfo_auto[v]->streams) {
                    SharedDecimator::VfoBlock b;
                    b.decimator = st.dec.get();
                    b.bandwidth_hz = st.width;
                    b.freq_offset_hz = vfo_blocks[v].freq_offset_hz;
                    b.out = &st.out;
                    auto take = [&raw_packet, &b](int ch) {
                        if (b.num_channels >= static_cast<size_t>(MAX_CHANNELS)) return;
                        if (ch >= 0 && ch < static_cast<int>(raw_packet.channel_iq_data.size())) {
                            b.ptrs[b.num_channels] = raw_packet.channel_iq_data[ch].data();
                            b.lens[b.num_channels] = raw_packet.channel_iq_data[ch].size();
                        }
                        b.num_channels++;
                    };
                    st.src = 0;
                    if (wideband_enabled) {
                        take(inst->tuner_channel.load(std::memory_order_relaxed));
                    } else if (fm_only || !st.mc) {
                        take(current_active);
                    } else {
                        for (int ch = 0; ch < packet_elements; ch++) take(ch);
                        st.src = static_cast<size_t>(std::max(0, current_active));
                    }
                    vfo_blocks.push_back(b);
                }
            }

            // Timed for the dashboard's "decimate" figure: the whole pass up
            // to the barrier (the decimation + every VFO's MUSIC task).
            const auto pass_start = steady_clock::now();

            // STAGE 1: every VFO's decimation in ONE pass over each input
            // channel (SharedDecimator::decimateVfos: the VFOs on an antenna
            // filter each chunk of it while it is in the core's L1 - each
            // VFO's own decimateMultiChannel read every antenna's block
            // again). One VFO: exactly its decimateMultiChannel
            try {
                SharedDecimator::decimateVfos(vfo_blocks.data(), vfo_blocks.size());   // the VFOs + internal streams
            } catch (const std::exception& e) {
                cerr << "Decimation error: " << e.what() << endl;
                for (auto& r : results) r.decimated_data = SharedDecimator::MultiChannelDecimated();
                for (auto& s : auto_sets)
                    for (auto& st : s.streams) st.out = SharedDecimator::MultiChannelDecimated();
            }

            // Per VFO after the decimation: its stream position, squelch,
            // MUSIC, the per-talker DoA. In wideband / independent mode
            // (no MUSIC) each decimator got data from its specific tuner
            auto run_pipeline =
                [&results, &vfo_blocks, wideband_enabled](size_t v) {

                    DecimatorManager::ProcessResult& result = results[v];
                    const auto& inst = result.instance;
                    const float block_offset_hz = vfo_blocks[v].freq_offset_hz;

                    try {
                        // Restore user-facing offset sign
                        result.decimated_data.freq_offset_hz = block_offset_hz;
                        result.decimated_data.stream_pos =
                            inst->stream_pos.fetch_add(result.decimated_data.min_samples, std::memory_order_relaxed);
                        for (auto& channel_data_out : result.decimated_data.channels) {
                            channel_data_out.freq_offset_hz = block_offset_hz;
                        }

                        // STAGE 2: MUSIC (after the shared decimation pass)
                        // SKIP MUSIC in wideband mode (channels are not phase-coherent)
                        // SKIP MUSIC if per-decimator squelch is enabled but closed (FFT method only)
                        // IMPORTANT: When beamforming is enabled, NEVER squelch MUSIC - it needs
                        // to run to provide DoA steering angle. Squelch only applies to audio output.
                        // Digital squelch: MUSIC always runs (its frames are held back); the
                        // VFO's DoA is published from the samples the decoder confirmed
                        // (talker_doa.cpp) and the squelch follows the decoder's valid frames.
                        bool squelch_allows_music = true;
                        bool bf_active = beamforming_enabled.load(std::memory_order_relaxed) && !wideband_enabled;
                        const bool sq_on = inst->squelch_enabled.load(std::memory_order_relaxed);
                        // Digital needs the VFO's decoder on (else FFT); outside coherent mode
                        // (no MUSIC) it still gates the audio
                        const bool sq_digital = DecimatorManager::effectiveSquelchMethod(*inst) == SquelchMethod::DIGITAL;

                        if (sq_on && sq_digital) {
                            inst->squelch_open.store(DecimatorManager::digitalSquelchOpen(*inst), std::memory_order_relaxed);
                        } else if (sq_on && !bf_active) {
                            // FFT-based squelch: Only apply squelch to MUSIC when beamforming is OFF
                            // When beamforming is ON, MUSIC must run to provide steering
                            // (wideband / independent: the VFO's own tuner)
                            float squelch_level = inst->squelch_level.load(std::memory_order_relaxed);
                            int check_channel = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed)
                                                                 : active_channel.load(std::memory_order_relaxed);
                            float offset_hz = inst->frequency_offset_hz;
                            float bw_hz = inst->decimator->getBandwidthMhz() * 1e6f;
                            squelch_allows_music = FFTProcessor::check_squelch_in_range(
                                check_channel, squelch_level, offset_hz, bw_hz);
                            // Update squelch_open state for UI feedback
                            inst->squelch_open.store(squelch_allows_music, std::memory_order_relaxed);
                        } else if (bf_active) {
                            // Beamforming active with FFT squelch: always allow MUSIC, update squelch
                            // state from THIS decimator's beamformed FFT (from the previous block).
                            if (sq_on && inst->beamformed_fft.valid.load(std::memory_order_acquire)) {
                                float squelch_level = inst->squelch_level.load(std::memory_order_relaxed);
                                bool bf_squelch_open = FFTProcessor::check_squelch_beamformed(
                                    inst->beamformed_fft, squelch_level);
                                inst->squelch_open.store(bf_squelch_open, std::memory_order_relaxed);
                            }
                        }
                        // the confirmed-samples DoA runs while the Digital squelch is in effect
                        const bool dig_doa = sq_on && sq_digital && !wideband_enabled;
                        if (inst->talker_doa) inst->talker_doa->set_digital(dig_doa && inst->talker_doa->active());
                        //
                        // HARD GATE during heimdall calibration (noise source on
                        // + settle hold): the injected noise is a strong coherent
                        // signal that would OPEN every squelch method and spin
                        // the DoA. Skip MUSIC entirely - published DoA freezes,
                        // and the accumulator/covariance/auto-squelch never see
                        // calibration data. Applies to every method, squelch on
                        // or off. Debug (doa_display_held) lets MUSIC run through
                        // the noise source - for the page only: its frames are
                        // marked (setNoiseInput), talker DoA below keeps the gate.
                        const bool noise_held = doa_is_calibrating();
                        const bool retune_held = doa_retune_hold_active();
                        const bool doa_held = noise_held || retune_held;
                        const bool music_held = retune_held || doa_display_held(noise_held);
                        if (!wideband_enabled &&
                            squelch_allows_music &&
                            !music_held &&
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

                            // Publish hold: the FFT method with beamforming (MUSIC must
                            // keep running - it provides steering - but a closed squelch
                            // still freezes the PUBLISHED DoA, using the beamformed squelch
                            // state from the previous block), and the Digital squelch (the
                            // published DoA comes from the confirmed samples instead)
                            {
                                bool fft_bf_hold = !sq_digital && bf_active && sq_on &&
                                                   !inst->squelch_open.load(std::memory_order_relaxed);
                                inst->music_processor->setPublishHold(fft_bf_hold || dig_doa);
                            }

                            inst->music_processor->setNoiseInput(noise_held);
                            inst->music_processor->processDecimatedIQ(result.decimated_data);
                        }
                        // per-packet DoA (ADS-B, AIS, DME, ISM devices) / the Digital
                        // squelch: the stream the decoder's packets and confirmed frames
                        // are cut from. Kept whatever the FFT squelch says - the decoder
                        // confirms each packet itself - but not while heimdall calibrates
                        // or retunes (the gap that leaves drops the packets across it).
                        // Inside the squelch gate, a closed FFT squelch left the ring
                        // empty: on a 2.4 MHz ISM VFO the averaged spectrum opens late or
                        // never for a short burst, so a device decoded at once took
                        // minutes to reach the DF list, or never did
                        if (!wideband_enabled && !doa_held && inst->talker_doa &&
                            inst->talker_doa->wants_blocks() &&
                            !inst->being_deleted.load(std::memory_order_relaxed))
                            inst->talker_doa->add_block(result.decimated_data);

                    } catch (const std::exception& e) {
                        cerr << "Pipeline " << inst->id << " error: " << e.what() << endl;
                    }
                };

            if (num_vfos == 1 || wideband_enabled || fm_only) {
                // One VFO, or one channel each (MUSIC returns at once on a
                // block of one channel): inline, no pool
                for (size_t v = 0; v < num_vfos; v++) run_pipeline(v);
            } else if (num_vfos > 1) {
                // Every VFO but the first on a persistent pool, the first
                // inline here, then one barrier. std::async(launch::async)
                // created and joined an OS thread per VFO per packet (~146
                // packets/s). The tasks wait on no pool, so a queued VFO
                // can't deadlock it.
                static ThreadPool vfo_pool(3);
                vector<future<void>> pipeline_futures;
                pipeline_futures.reserve(num_vfos - 1);
                for (size_t v = 1; v < num_vfos; v++) {
                    pipeline_futures.push_back(vfo_pool.submit([&run_pipeline, v]() { run_pipeline(v); }));
                }
                run_pipeline(0);
                for (auto& future : pipeline_futures) {
                    future.get();
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
                            sample_rate_hz,
                            static_cast<float>(result.decimated_data.bandwidth_hz)
                        );
                    }
                }
            }

            // Digital decoders: fed the same stream as the FM audio would be -
            // the beamformer output when beamforming ran for this VFO (empty
            // while FD-DAS fills, and on about one block in three after: its
            // 256-point frames are longer than a narrow VFO's ~85 samples a
            // block - no VFO samples then, never mixed-in raw blocks), else
            // the listened-to channel. Before the FM queueing, which moves
            // the beamformed samples out. Auto detect's internal streams go
            // with every block they were decimated in, VFO samples or not
            if (digital_needs_processing) {
                for (auto& result : results) {
                    const auto& inst = result.instance;
                    if (!inst || inst->digital_mode.load(std::memory_order_relaxed) == 0) continue;
                    const bool vfo_ok = !result.decimated_data.channels.empty() && result.decimated_data.min_samples > 0;
                    auto dd = inst->getDigital();
                    if (!dd) continue;
                    // array plugins (kp::Info::array): every antenna's block, not
                    // during a noise-source calibration / retune hold (like MUSIC)
                    if (vfo_ok && dd->wants_array() && !doa_is_calibrating() && !doa_retune_hold_active()) {
                        const auto& chs = result.decimated_data.channels;
                        const size_t ns = result.decimated_data.min_samples;
                        const complex<float>* ptr[8];
                        int m = 0;
                        while (m < static_cast<int>(chs.size()) && m < 8 && chs[static_cast<size_t>(m)].samples.size() >= ns) {
                            ptr[m] = chs[static_cast<size_t>(m)].samples.data();
                            m++;
                        }
                        int at = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed) : 0;
                        if (at < 0 || at >= MAX_CHANNELS) at = 0;
                        if (m > 0)
                            dd->push_array(ptr, m, ns, result.decimated_data.output_rate_hz,
                                           static_cast<double>(exact_tuner_hz(at)) + result.decimated_data.freq_offset_hz,
                                           result.decimated_data.stream_pos);
                    }
                    const vector<complex<float>>* src = nullptr;
                    if (!vfo_ok) {
                        // (no VFO samples this block)
                    } else if (result.beamformer_ran) {
                        if (result.have_beamformed) src = &result.beamformed_samples;
                    } else {
                        int idx = (wideband_enabled || fm_only) ? 0 : current_active;
                        if (idx < static_cast<int>(result.decimated_data.channels.size()))
                            src = &result.decimated_data.channels[idx].samples;
                    }
                    if (src && src->empty()) src = nullptr;
                    int tuner = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed) : 0;
                    if (tuner < 0 || tuner >= MAX_CHANNELS) tuner = 0;
                    double rf = static_cast<double>(exact_tuner_hz(tuner)) + result.decimated_data.freq_offset_hz;
                    // every antenna for a multichannel plugin (ism: each burst
                    // combined from all of them) - only when the block holds
                    // the whole coherent array: not in independent / wideband
                    // mode (each VFO has its own tuner), nor with DoA and
                    // beamforming off (one channel decimated); those decode
                    // the one channel as before
                    const std::complex<int16_t>* mc = nullptr;
                    int mc_m = 0;
                    const auto& chs = result.decimated_data.channels;
                    const size_t n = src ? src->size() : 0;
                    if (n && dd->wants_multichannel() && !wideband_enabled && !fm_only && chs.size() >= 2 &&
                        n == result.decimated_data.min_samples) {
                        const int m = static_cast<int>(std::min<size_t>(chs.size(), 8));
                        bool ok = true;
                        for (int k = 0; k < m; k++) ok = ok && chs[static_cast<size_t>(k)].samples.size() >= n;
                        if (ok) {
                            thread_local vector<complex<int16_t>> mcbuf;
                            mcbuf.resize(n * static_cast<size_t>(m));
                            const float sc = dig::DigitalDecoder::MC_SCALE;
                            auto s16 = [](float v) { return static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f)); };
                            for (int k = 0; k < m; k++) {
                                const complex<float>* q = chs[static_cast<size_t>(k)].samples.data();
                                for (size_t i = 0; i < n; i++)
                                    mcbuf[i * static_cast<size_t>(m) + static_cast<size_t>(k)] = {s16(q[i].real() * sc), s16(q[i].imag() * sc)};
                            }
                            mc = mcbuf.data();
                            mc_m = m;
                        }
                    }
                    // Auto detect's internal streams of this block, in the
                    // VFO's stream positions: the VFO's block starts at
                    // stream_pos, and a wider stream's cascade is shorter - what
                    // it shows reaches the VFO's stream (vfo_delay - its
                    // delay) later. Pushed with the VFO's block - and without
                    // one when the VFO has none this time (the beamformer held
                    // its samples back: they come later, in a bigger block;
                    // an internal block not pushed now would be lost). Its
                    // positions come from the decimated block's stream_pos,
                    // never from the beamformer's output. Every antenna as the
                    // VFO's own stream would get them
                    dig::AuxStream aux[dig::DigitalDecoder::MAX_STREAMS];
                    size_t naux = 0;
                    const size_t vi = static_cast<size_t>(&result - results.data());
                    if (AutoStreamSet* set = vi < vfo_auto.size() ? vfo_auto[vi] : nullptr) {
                        const double vrate = result.decimated_data.output_rate_hz;
                        for (auto& st : set->streams) {
                            if (naux >= static_cast<size_t>(dig::DigitalDecoder::MAX_STREAMS)) break;
                            const auto& o = st.out;
                            if (o.min_samples == 0 || st.src >= o.channels.size()) continue;
                            const auto& ss = o.channels[st.src].samples;
                            if (ss.empty()) continue;
                            dig::AuxStream& a = aux[naux++];
                            a.width_hz = st.width;
                            a.rate_hz = o.output_rate_hz;
                            a.x = ss.data();
                            a.n = ss.size();
                            a.pos = static_cast<double>(result.decimated_data.stream_pos) +
                                    (set->vfo_delay_s - st.delay_s) * vrate;
                            if (st.mc && !wideband_enabled && !fm_only && o.channels.size() >= 2 && a.n == o.min_samples) {
                                const int m = static_cast<int>(std::min<size_t>(o.channels.size(), 8));
                                bool ok = true;
                                for (int k = 0; k < m; k++) ok = ok && o.channels[static_cast<size_t>(k)].samples.size() >= a.n;
                                if (ok) {
                                    st.mcbuf.resize(a.n * static_cast<size_t>(m));
                                    const float sc = dig::DigitalDecoder::MC_SCALE;
                                    auto s16 = [](float v) { return static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f)); };
                                    for (int k = 0; k < m; k++) {
                                        const complex<float>* q = o.channels[static_cast<size_t>(k)].samples.data();
                                        for (size_t i = 0; i < a.n; i++)
                                            st.mcbuf[i * static_cast<size_t>(m) + static_cast<size_t>(k)] = {s16(q[i].real() * sc), s16(q[i].imag() * sc)};
                                    }
                                    a.mc = st.mcbuf.data();
                                    a.mc_m = m;
                                }
                            }
                        }
                    }
                    if (n == 0 && naux == 0) continue;
                    dd->push(src ? src->data() : nullptr, n, result.decimated_data.output_rate_hz, rf,
                             result.decimated_data.stream_pos, mc, mc_m, result.decimated_data.bandwidth_hz, aux, naux);
                }
            }

            // 🗣 Voice to Text: VFOs with Transcribe on and no digital decoder
            // of voice (one that decodes voice sends it through the decoder's
            // voice tap; analog_id / pagers / adsb ... decode none: the analog
            // demod then, named after the decoder's talker - the CTCSS tone /
            // DCS code) - the same stream, demodulated in voice2text.cpp with
            // the VFO's demod. Cut into transmissions by the VFO's squelch;
            // with it off, by a signal AUTO_SQUELCH_DB over the noise floor in
            // the FFT
            if (transcribe_needs_processing) {
                for (auto& result : results) {
                    const auto& inst = result.instance;
                    if (!inst || !inst->transcribe.load(std::memory_order_relaxed)) continue;
                    std::string v2t_tid, v2t_tlabel;
                    if (inst->digital_mode.load(std::memory_order_relaxed) != 0) {
                        auto dd = inst->getDigital();
                        if (!dd || dd->takes_voice()) continue;
                        dd->lead_talker(&v2t_tid, &v2t_tlabel);
                    }
                    if (result.decimated_data.channels.empty() || result.decimated_data.min_samples == 0) continue;
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
                    const double rf = static_cast<double>(exact_tuner_hz(tuner)) + result.decimated_data.freq_offset_hz;
                    bool open;
                    if (inst->squelch_enabled.load(std::memory_order_relaxed)) {
                        open = inst->squelch_open.load(std::memory_order_relaxed);
                    } else {
                        const int ch = wideband_enabled ? inst->tuner_channel.load(std::memory_order_relaxed)
                                                        : active_channel.load(std::memory_order_relaxed);
                        open = FFTProcessor::check_squelch_in_range(ch, v2t::AUTO_SQUELCH_DB, inst->frequency_offset_hz,
                                                                    inst->decimator->getBandwidthMhz() * 1e6f);
                    }
                    v2t::feed_analog(inst->id, src->data(), src->size(), result.decimated_data.output_rate_hz, rf,
                                     inst->demod_mode.load(std::memory_order_relaxed), open, v2t_tid, v2t_tlabel);
                }
            }

            // 📼 recorder: the band VFO's block of every antenna (IQ), and each
            // recorded VFO's stream - the one its FM audio / decoders get -
            // with its demod and squelch (audio)
            if (rec_band_vfo >= 0) {
                for (auto& result : results) {
                    const auto& inst = result.instance;
                    if (!inst || inst->id != rec_band_vfo) continue;
                    const auto& chs = result.decimated_data.channels;
                    const size_t ns = result.decimated_data.min_samples;
                    const complex<float>* ptr[MAX_CHANNELS];
                    int m = 0;
                    while (m < static_cast<int>(chs.size()) && m < MAX_CHANNELS && chs[static_cast<size_t>(m)].samples.size() >= ns) {
                        ptr[m] = chs[static_cast<size_t>(m)].samples.data();
                        m++;
                    }
                    if (m > 0 && ns > 0)
                        rec::on_band_block(inst->id, raw_packet.seq, ptr, m, ns, result.decimated_data.output_rate_hz,
                                           static_cast<double>(coherent_tuner_hz()) + result.decimated_data.freq_offset_hz,
                                           result.decimated_data.freq_offset_hz, raw_packet.noise_source_active != 0,
                                           raw_packet.phase_compensation_state);
                }
            }
            if (rec_audio) {
                for (auto& result : results) {
                    const auto& inst = result.instance;
                    if (!inst || !rec::audio_wants(inst->id)) continue;
                    if (result.decimated_data.channels.empty() || result.decimated_data.min_samples == 0) continue;
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
                    const double rf = static_cast<double>(exact_tuner_hz(tuner)) + result.decimated_data.freq_offset_hz;
                    const auto dd = inst->digital_mode.load(std::memory_order_relaxed) != 0 ? inst->getDigital() : nullptr;
                    rec::feed_audio(inst->id, src->data(), src->size(), result.decimated_data.output_rate_hz, rf,
                                    inst->demod_mode.load(std::memory_order_relaxed),
                                    inst->squelch_enabled.load(std::memory_order_relaxed),
                                    inst->squelch_open.load(std::memory_order_relaxed), dd && dd->takes_voice());
                }
            }

            // Now queue FM data (after MUSIC is done to avoid data race)
            // If beamforming produced output, use that instead of single-channel data
            // One item per VFO heard (🔊): the FM thread feeds each into its
            // own audio_mixer channel
            size_t n_audio = 0;
            for (const auto& result : results) n_audio += result.audio_out ? 1 : 0;
            const size_t fm_queue_max = 20 * std::max<size_t>(1, n_audio);
            for (auto& result : results) {
                if (!result.decimated_data.channels.empty() && result.decimated_data.min_samples > 0) {

                    // A VFO heard: queue its FM data
                    // In wideband mode: only 1 channel at index 0 (contains active_channel data)
                    // In normal mode: multiple channels, access by current_active index
                    if (result.audio_out && fm_needs_processing) {
                        DecimatedFMWorkItem fm_item;
                        fm_item.vfo_id = result.decimator_id;
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
                        if (!fm_item.decimated_samples.empty() && fm_decimated_queue.size_approx() < fm_queue_max) {
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
                        work_item.freq_hz = raw_packet.freq_hz[ch];   // the spectrum's place (see process_channel_fft)
                        work_item.seq = raw_packet.seq;

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

                // Noise floor: is the band's quiet part receiver noise (independent
                // per antenna) or a signal filling it? Any other antenna of this
                // packet (the receiver converts one when the test is due)
                if (nfloor::coherence_due()) {
                    for (int b = 0; b < static_cast<int>(raw_packet.channel_iq_data.size()) && b < MAX_CHANNELS; b++) {
                        const auto& xb = raw_packet.channel_iq_data[b];
                        if (b == current_active || xb.size() != num_samples) continue;
                        nfloor::measure_coherence(current_active, float_data.data(), xb.data(), num_samples);
                        break;
                    }
                }

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
                        work_item.freq_hz = raw_packet.freq_hz[current_active];   // (where its spectrum is: the search counts frames by it)
                        work_item.seq = raw_packet.seq;

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

        // 🎨 Bearing spectrum (bin_doa.hpp): the packet's antennas, which
        // nothing here reads any more, moved to its worker - with the reason
        // when this packet must not be used (parked like the DoA)
        if (bin_doa::enabled()) {
            const char* park = nullptr;
            const int ne = packet_elements;
            if (wideband_enabled) park = "only in coherent mode";
            else if (server_generic_rtlsdr.load(std::memory_order_relaxed)) park = "generic RTL-SDRs: no coherent array";
            else if (ne < 2 || static_cast<int>(raw_packet.channel_iq_data.size()) < ne ||
                     raw_packet.channel_iq_data[ne - 1].empty()) park = "waiting for every antenna";
            else if (doa_display_held(raw_packet.noise_source_active != 0 || doa_is_calibrating()))
                park = "calibrating (noise source on)";   // (not with Debug on)
            else if (doa_retune_hold_active()) park = "settling after a retune";
            else if (server_cal_stale.load(std::memory_order_relaxed)) park = "calibration stale";
            const uint64_t chz = coherent_tuner_hz();
            bin_doa::on_packet(std::move(raw_packet.channel_iq_data), ne, raw_packet.seq,
                               chz ? static_cast<double>(chz) : static_cast<double>(raw_packet.freq_hz[0]), park);
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
                                          work_item.buffer_generation, work_item.freq_hz, work_item.seq);
        nfloor::maybe_save();   // noise_floors.json now and then
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
                // (sweep mode: nothing is demodulated - no data is right)
                if (consecutive_timeouts == 20 && !sweep_mode_enabled.load(std::memory_order_relaxed)) {  // 2 s of no data
                    cout << "FM processor: No data for 2s - check decimation pipeline" << endl;
                } else if (consecutive_timeouts % 100 == 0 && !sweep_mode_enabled.load(std::memory_order_relaxed)) {  // Every 10 seconds
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
            audio_mixer.reset();
        }

        // Drop stale items
        if (age > milliseconds(200)) {
            global_stats.increment_errors(thread_idx);
            continue;
        }

        // The VFO's own channel of the mix (🔊 any number of VFOs at once):
        // its demodulator follows the VFO's rate and demod block by block.
        // A VFO muted / removed since the block was queued is skipped
        auto vfo = decimator_manager.getDecimator(work_item.vfo_id);
        if (!vfo || !vfo->audio_out.load(std::memory_order_relaxed) ||
            vfo->being_deleted.load(std::memory_order_relaxed)) continue;
        auto ch = audio_mixer.channel(work_item.vfo_id);
        if (!ch) continue;
        const DemodulatorMode vfo_mode = vfo->demod_mode.load(std::memory_order_relaxed);
        ch->mode.store(vfo_mode, std::memory_order_relaxed);
        FMDemodulatorRobust& demod = ch->demod;
        demod.setInputSampleRate(work_item.output_rate_hz);
        demod.setDemodulatorMode(vfo_mode);

        demod.process_decimated_samples(std::move(work_item.decimated_samples), audio_samples);
        if (audio_samples.empty()) {
            global_stats.print_stats_if_time(thread_idx);
            continue;
        }
        ch->fed_ms.store(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(),
                         std::memory_order_relaxed);

        // Digital demod: the FM output only sets how many samples this block
        // is worth; the audio itself is the VFO's decoded digital voice
        // (silence when there is none). Squelch doesn't apply - the decoder
        // only produces audio for valid voice frames.
        if (vfo_mode == DemodulatorMode::DIGITAL) {
            auto dd = vfo->getDigital();
            if (dd) dd->pull_voice(audio_samples.data(), audio_samples.size());
            else std::fill(audio_samples.begin(), audio_samples.end(), 0.0f);
            demod.add_audio_samples(audio_samples);
            global_stats.increment_operations(thread_idx);
            global_stats.print_stats_if_time(thread_idx);
            continue;
        }

        // Squelch of this VFO - if enabled and closed, output silence (keeps the
        // audio timing)
        bool squelch_allows_audio = true;
        if (vfo->squelch_enabled.load(std::memory_order_relaxed)) {
            float squelch_level = vfo->squelch_level.load(std::memory_order_relaxed);

            // Check squelch method (per decimator). Wideband / independent:
            // FFT on the VFO's own tuner (no beamformer there); Digital in
            // every mode (it only needs the decoder)
            const bool multi = multi_tuner_mode();
            if (DecimatorManager::effectiveSquelchMethod(*vfo) == SquelchMethod::DIGITAL) {
                // Digital: open while the VFO's decoder reports valid frames
                squelch_allows_audio = DecimatorManager::digitalSquelchOpen(*vfo);
            } else if (!multi && beamforming_enabled.load(std::memory_order_relaxed) &&
                       vfo->beamformed_fft.valid.load(std::memory_order_acquire)) {
                // FFT-based squelch with beamforming: use the VFO's
                // beamformed signal for better SNR
                squelch_allows_audio = FFTProcessor::check_squelch_beamformed(
                    vfo->beamformed_fft, squelch_level);
            } else {
                // FFT-based squelch: fall back to single-channel squelch
                int check_channel = multi ? vfo->tuner_channel.load(std::memory_order_relaxed)
                                          : active_channel.load(std::memory_order_relaxed);
                float offset_hz = vfo->frequency_offset_hz;
                float bw_hz = vfo->decimator->getBandwidthMhz() * 1e6f;
                squelch_allows_audio = FFTProcessor::check_squelch_in_range(
                    check_channel, squelch_level, offset_hz, bw_hz);
            }
            vfo->squelch_open.store(squelch_allows_audio, std::memory_order_relaxed);
        }
        if (!squelch_allows_audio) std::fill(audio_samples.begin(), audio_samples.end(), 0.0f);
        demod.add_audio_samples(audio_samples);
        global_stats.increment_operations(thread_idx);

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

// Read whatever heimdall has pushed at us. It answers every command and
// broadcasts a status JSON at 2 Hz to every control client. Channel metadata
// arrives on the 8091 data stream instead; the only things taken from here are
// the Wideband board's "downconverter" object (wb_board.cpp: board telemetry
// for the UI and its log), heimdall's "version" (app_version.cpp) and its
// antenna bias tees (antenna_bias.cpp: the status fields and the replies to
// set_antenna_bias_tees) - everything else is discarded.
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
            wb_board_ingest(scratch, static_cast<size_t>(n));
            heimdall_version_ingest(scratch, static_cast<size_t>(n));
            antbias::ingest(scratch, static_cast<size_t>(n));
            ControlHandler::ingest_sweep_reply(scratch, static_cast<size_t>(n));   // an older heimdall's refusal
            sweep::ingest_control_reply(scratch, static_cast<size_t>(n));          // the gain curve in its status
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
    // heimdall's status (its antenna bias tees for the pages, its version) comes
    // over this connection, which commands open lazily: open it here too - every
    // 5 s at most while it is down - so the pages see that state before anything
    // was sent. Not during a playback (nothing may reach heimdall).
    if (!control_client.is_connected() && !playback::active()) {
        static steady_clock::time_point last_try{};
        const auto now = steady_clock::now();
        if (now - last_try >= seconds(5)) {
            last_try = now;
            if (control_client.connect_to_port(TCP_CONTROL_PORT)) control_client.set_nonblocking(true);
        }
    }
    drain_control_socket_locked();
}

void DataReceiver::send_control_command(const string& cmd) {
    // Debug playback: heimdall keeps the user's tuning - the session's
    // commands only change this client (a stop replays the saved settings)
    if (playback::active()) {
        cout << "[playback] not sent to heimdall: " << cmd.substr(0, 120) << endl;
        return;
    }
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

