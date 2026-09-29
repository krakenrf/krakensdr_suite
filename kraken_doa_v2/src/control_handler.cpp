#include "control_handler.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "channel_manager.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/beamformer.hpp"
#include "signal_processing/music_processor.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "continuous_scanner.hpp"
#include "station_info.hpp"
#include "settings_store.hpp"
#include "doa_logger.hpp"
#include "message_builders.hpp"
#include "networking/data_receiver.hpp"
#include "networking/web_mapper.hpp"
#include "networking/websocket_server.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include <string>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <map>
#include <mutex>
#include <vector>
#include <atomic>

using namespace std;

extern std::atomic<int> active_channel;
extern DecimatorManager decimator_manager;
extern std::atomic<float> current_edge_clip;

// --- Parsing helpers ---
static bool parse_bool(string_view sv) {
    return sv == "1" || sv == "true";
}
static float parse_float(string_view msg, size_t offset) {
    return stof_finite(string(msg.substr(offset)));
}
static double parse_double(string_view msg, size_t offset) {
    return stod_finite(string(msg.substr(offset)));
}
static int parse_int(string_view msg, size_t offset) {
    return stoi(string(msg.substr(offset)));
}

// --- Decimator iteration helpers ---
template<typename Fn>
static void forEachMusicProcessor(Fn&& fn) {
    for (const auto& d : decimator_manager.getAllDecimators()) {
        if (d && d->music_processor && !d->being_deleted.load(std::memory_order_relaxed))
            fn(d->music_processor.get());
    }
}

// --- Broadcast helpers ---
static void broadcast(const string& json) {
    WebSocketServer::broadcast_json_message(json);
}

// --- Multi-browser settings sync ---
// Every state-changing command a browser sends is echoed to ALL connected
// browsers as {"sync_cmd":"<command>"} so their UIs stay in sync. The latest
// value of selected commands is also stored and replayed to newly connecting
// clients (the backend, not the browser, is the source of truth).
static std::map<string, string> sync_replay_store;
static std::mutex sync_store_mutex;

static string make_sync_cmd_json(string_view cmd) {
    return "{\"sync_cmd\":\"" + json_escape(cmd) + "\"}";
}

// --- KrakenSDR Wideband (downconverter) variant ---
// Full variant state for the browser: whether the mode is active (--wideband),
// the selected mixing side / ring, and the hardware constants (IF, LO span,
// ring boundaries and radii) the UI needs to compute per-frequency side
// availability and the ring indicator locally. Sent on connect and
// re-broadcast whenever the side or ring changes. rf_min/max is the union of
// all sides (the side auto-switches with the frequency).
static string build_wb_variant_json() {
    bool en = wb_variant_enabled.load(std::memory_order_relaxed);
    int side = wb_variant_mixer_side.load(std::memory_order_relaxed);
    uint64_t min_hz = 0, max_hz = 0;
    if (en) wb_variant_rf_union_range(min_hz, max_hz);
    stringstream ss;
    ss << "{\"wb_variant\":{\"enabled\":" << (en ? "true" : "false")
       << ",\"if_hz\":" << WB_VARIANT_IF_HZ
       << ",\"lo_min_hz\":" << WB_LO_MIN_HZ
       << ",\"lo_max_hz\":" << WB_LO_MAX_HZ
       << ",\"ring1_hz\":" << WB_RING_CENTER_MIN_HZ
       << ",\"ring2_hz\":" << WB_RING_INNER_MIN_HZ
       << ",\"ring_radii_mm\":[" << WB_RING_RADIUS_MM[0] << ","
       << WB_RING_RADIUS_MM[1] << "," << WB_RING_RADIUS_MM[2] << "]"
       << ",\"side\":\"" << wb_mixer_side_name(side) << "\""
       << ",\"array\":" << wb_variant_array.load(std::memory_order_relaxed)
       << ",\"lo_current\":" << wb_variant_lo_current.load(std::memory_order_relaxed)
       << ",\"rf_min_hz\":" << min_hz
       << ",\"rf_max_hz\":" << max_hz << "}}";
    return ss.str();
}

// Read-only queries change no state - echoing them would just be noise.
static bool is_query_command(string_view msg) {
    return msg.starts_with("GET_") ||
           msg.starts_with("SCANNER_GET_") ||
           msg == "CONTINUOUS_SCANNER_STATUS" ||
           msg.starts_with("LOG_LIST") ||      // recordings listing query
           msg.starts_with("LOG_DELETE:");     // delete acks via its own list broadcast
}

// Commands whose latest value is replayed to newly connecting clients.
// FM/DOA are rebuilt from live atomics instead (the scanner mutates them
// without going through a command); gain/frequency/channel/averaging reach
// new clients continuously via the binary FFT header.
static bool is_replayed_command(string_view msg) {
    static const char* prefixes[] = {
        "TOPOLOGY:", "CUSTOM_POSITIONS:", "RADIUS:", "SPACING:", "ELEVATION_RESOLUTION:",
        "MUSIC_NUM_SNAPSHOTS:", "MUSIC_SNAPSHOT_LENGTH:",
        "MUSIC_FB_AVERAGING:", "MUSIC_COVARIANCE_ALPHA:", "EDGE_CLIP:",
        "MUSIC_SIGNAL_SOURCES:", "ULA_MODE:", "ARRAY_OFFSET:",
        "STATION_ID:", "LOCATION_SOURCE:", "STATIC_LOCATION:",
        "LOG_INTERVAL:", "LOG_FORMAT:",
        "WEB_MAPPER:", "WEB_MAPPER_MODE:", "WEB_MAPPER_KEY:",
        "WEB_MAPPER_URL:", "WEB_MAPPER_WS_PORT:",
    };
    for (const char* p : prefixes) {
        if (msg.starts_with(p)) return true;
    }
    return false;
}

// The KrakenPro API key is a secret: every browser receives the sync_cmd
// echo of each applied command and the replay on connect, and the page is
// served to anyone who can reach it. So the key is never sent to browsers -
// they only learn whether one is saved (WEB_MAPPER_KEY_SET:1/0). The real
// key is still persisted and used by the web mapper.
static string redact_for_sync(string_view msg) {
    constexpr string_view KEY_CMD = "WEB_MAPPER_KEY:";
    if (msg.starts_with(KEY_CMD)) {
        return string("WEB_MAPPER_KEY_SET:") + (msg.size() > KEY_CMD.size() ? "1" : "0");
    }
    return string(msg);
}

// A command a handler refused (out of range, can't apply now). Thrown from
// handle_message_impl so the dispatcher neither persists nor echoes it: a
// rejected command used to be recorded to disk and broadcast anyway,
// replacing the valid saved value (and showing it in every browser).
struct CommandRejected : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// When a handler normalizes its value (clamp, wrap), it records the command
// as actually applied; that - not the raw request - is persisted and echoed,
// so e.g. AVG:5 is stored as AVG:1 and a browser's field snaps to the real
// value. Every dispatch runs on the uWS loop thread (browser commands, the
// deferred settings replay and the web mapper's deferred cloud settings).
static std::optional<string> g_applied_cmd;
static void set_applied(string_view prefix, double value, int decimals) {
    ostringstream os;
    os << prefix << fixed << setprecision(decimals) << value;
    g_applied_cmd = os.str();
}

static void store_for_replay(string_view msg, size_t key_len) {
    std::lock_guard<std::mutex> lock(sync_store_mutex);
    sync_replay_store[string(msg.substr(0, key_len))] = string(msg);
}

// --- Backend settings persistence ---
// Which commands are remembered (and how) is defined by the schema in
// settings_store.cpp; SettingsStore::record() simply ignores commands that
// aren't in it. True while we are replaying the saved file ourselves, so the
// record hooks don't re-save commands mid-restore. The replay runs on the uWS
// loop thread (see data_receiver.cpp), so no browser command can interleave
// with it - one arriving meanwhile is dispatched, and saved, after it.
static std::atomic<bool> g_replaying_settings{false};

// --- Decimator (VFO) setup persistence ---
// The VFO set is dynamic (count, per-VFO tuning/bandwidth/demod/squelch, FM
// source), so it is remembered as ONE composite DECIMATORS: setting rather
// than per-command schema entries. The FM source is stored as a list POSITION,
// not an id: ids can become non-contiguous after removals but are renumbered
// 0..N-1 on the next start.
static string build_decimator_snapshot() {
    auto info = decimator_manager.getDecimatorInfoList();
    int fm_id = decimator_manager.getFMDecimatorId();
    size_t fm_index = 0;
    for (size_t i = 0; i < info.size(); i++)
        if (info[i].id == fm_id) { fm_index = i; break; }
    ostringstream ss;
    ss << fm_index << "|" << fixed << setprecision(2);
    for (size_t i = 0; i < info.size(); i++) {
        const auto& d = info[i];
        if (i) ss << ";";
        ss << d.frequency_offset_hz << "," << d.bandwidth_index << ","
           << DecimatorManager::demodModeToString(d.demod_mode) << ","
           << (d.squelch_enabled ? 1 : 0) << "," << d.squelch_level << ","
           << d.squelch_method << "," << d.squelch_eigen_threshold;
    }
    return ss.str();
}

// Call after any command that mutates the VFO set. Scanner-driven offset
// moves bypass the control handler entirely, so transient scan state is
// never captured.
static void record_decimator_snapshot() {
    if (g_replaying_settings.load()) return;
    SettingsStore::record("DECIMATORS:" + build_decimator_snapshot());
}

// Restore a saved snapshot: grow/shrink to the saved VFO count, apply each
// VFO's settings positionally, then reselect the FM source. Parses the whole
// snapshot before mutating anything so a malformed value leaves the startup
// defaults intact. Returns true if a snapshot was applied.
static bool apply_decimator_snapshot(const string& snap) {
    size_t bar = snap.find('|');
    if (bar == string::npos) return false;

    struct Vfo {
        float offset_hz; int bw_index; DemodulatorMode demod;
        bool sq_en; float sq_db; int sq_method; float sq_eigen;
    };
    int fm_index;
    vector<Vfo> vfos;
    try {
        fm_index = stoi(snap.substr(0, bar));
        stringstream list(snap.substr(bar + 1));
        string entry;
        while (getline(list, entry, ';')) {
            stringstream es(entry);
            array<string, 7> f;
            for (auto& tok : f)
                if (!getline(es, tok, ',')) return false;
            vfos.push_back({stof_finite(f[0]),
                            std::clamp(stoi(f[1]), 0, NUM_BANDWIDTH_OPTIONS - 1),
                            DecimatorManager::stringToDemodMode(f[2]),
                            f[3] == "1", stof_finite(f[4]),
                            std::clamp(stoi(f[5]), 0, 2), stof_finite(f[6])});
        }
    } catch (const exception&) {
        return false;
    }
    if (vfos.empty()) return false;

    auto info = decimator_manager.getDecimatorInfoList();
    while (info.size() < vfos.size()) {
        if (decimator_manager.addDecimator() < 0) break;
        info = decimator_manager.getDecimatorInfoList();
    }
    while (info.size() > vfos.size()) {
        if (!decimator_manager.removeDecimator(info.back().id)) break;
        info = decimator_manager.getDecimatorInfoList();
    }

    size_t n = min(info.size(), vfos.size());
    for (size_t i = 0; i < n; i++) {
        const Vfo& v = vfos[i];
        int id = info[i].id;
        decimator_manager.setFrequencyOffset(id, v.offset_hz);
        decimator_manager.setBandwidthIndex(id, v.bw_index);
        decimator_manager.setDemodMode(id, v.demod);
        decimator_manager.setSquelchEnabled(id, v.sq_en);
        decimator_manager.setSquelchLevel(id, v.sq_db);
        decimator_manager.setSquelchMethod(id, v.sq_method);
        decimator_manager.setSquelchEigenThreshold(id, v.sq_eigen);
    }

    if (fm_index < 0 || static_cast<size_t>(fm_index) >= n) fm_index = 0;
    decimator_manager.setFMDecimatorId(info[fm_index].id);
    fm_demod.setDemodulatorMode(vfos[fm_index].demod);
    fm_demod.reset_audio_buffer();

    cout << "Restored " << n << " decimator(s), FM source: decimator "
         << info[fm_index].id << endl;
    return true;
}

vector<string> ControlHandler::get_connect_sync_messages() {
    vector<string> out;
    // Live toggle state first, then stored settings
    out.push_back(make_sync_cmd_json(fm_enabled.load() ? "FM:1" : "FM:0"));
    out.push_back(make_sync_cmd_json(doa_enabled.load() ? "DOA:1" : "DOA:0"));
    // Wideband (downconverter) variant state - tells the browser whether to
    // show the mixing-side controls and which RF range to allow
    out.push_back(build_wb_variant_json());
    std::lock_guard<std::mutex> lock(sync_store_mutex);
    for (const auto& [key, cmd] : sync_replay_store) {
        out.push_back(make_sync_cmd_json(cmd));
    }
    return out;
}

static void broadcast_doa_state(bool enabled) {
    stringstream ss;
    ss << "{\"doa_state\":{\"enabled\":" << (enabled ? "true" : "false") << "}}";
    broadcast(ss.str());
}

// Enable/disable all MUSIC processors and broadcast state
static void setAllDoaEnabled(bool enable) {
    forEachMusicProcessor([&](auto* mp) {
        if (enable) mp->enable(); else mp->disable();
    });
    broadcast_doa_state(enable);
}

// Handle legacy FREQ_OFFSET[_n] for a decimator by POSITION. Decimator IDs
// are smallest-available, not positional, so after a remove/re-add the n-th
// decimator's ID need not equal n - resolve the position to an actual ID.
static void handle_freq_offset(int decimator_idx, float offset_khz) {
    float offset_hz = offset_khz * 1000.0f;

    auto all_decimators = decimator_manager.getAllDecimators();
    if (decimator_idx < 0 || decimator_idx >= static_cast<int>(all_decimators.size())) {
        cout << "FREQ_OFFSET: no decimator at position " << decimator_idx << endl;
        return;
    }
    int id = all_decimators[decimator_idx]->id;
    decimator_manager.setFrequencyOffset(id, offset_hz);
    record_decimator_snapshot();

    float rf_frequency = ChannelManager::get_frequency(active_channel.load(std::memory_order_relaxed));
    float effective_frequency = rf_frequency + offset_hz;

    cout << "Decimator " << id << " frequency offset changed to " << offset_khz << " kHz" << endl;
    cout << "Effective frequency: " << (effective_frequency/1e6) << " MHz" << endl;
}

// --- DoA logger helpers ---

// Broadcast the current recording state to all browsers immediately (the same
// "recording":{...} object also rides the 500ms system_status broadcast).
static void broadcast_recording_status() {
    std::string json = "{";
    doa_logger.appendStatusJson(json);
    json += "}";
    broadcast(json);
}

// List of recordings in the fixed doa_recordings/ folder, for the UI list +
// download/delete. No arbitrary device paths are exposed.
static std::string build_log_list_json() {
    // For the file currently being recorded, report the live logical size (same
    // as the status panel) instead of the lagging on-disk main-file size.
    bool running = doa_logger.isRunning();
    std::string active = running ? doa_logger.activeFilename() : std::string();
    uint64_t active_bytes = running ? doa_logger.fileBytes() : 0;

    std::ostringstream js;
    js << "{\"log_list\":{\"dir\":\"" << json_escape(doa_recordings_dir()) << "\",\"files\":[";
    auto files = doa_list_recordings();
    for (size_t i = 0; i < files.size(); i++) {
        if (i) js << ",";
        uint64_t size = (running && files[i].name == active) ? active_bytes : files[i].size;
        js << "{\"name\":\"" << json_escape(files[i].name)
           << "\",\"size\":" << size
           << ",\"mtime\":" << files[i].mtime_ms << "}";
    }
    js << "]}}";
    return js.str();
}

void ControlHandler::handle_message_impl(string_view message) {
    if (message.starts_with("FREQ:")) {
        double freq_mhz = parse_double(message, 5);
        uint64_t freq_hz = static_cast<uint64_t>(llround(freq_mhz * 1e6));

        // Wideband variant: keep the request inside the union RF span (the
        // mixer side auto-switches, so any side's reach is tunable).
        if (wb_variant_enabled.load(std::memory_order_relaxed)) {
            uint64_t min_hz, max_hz;
            wb_variant_rf_union_range(min_hz, max_hz);
            uint64_t clamped = std::clamp(freq_hz, min_hz, max_hz);
            if (clamped != freq_hz) {
                cout << "FREQ " << freq_hz / 1e6 << " MHz outside wideband range, clamped to "
                     << clamped / 1e6 << " MHz" << endl;
                freq_hz = clamped;
                set_applied("FREQ:", freq_hz / 1e6, 6);
            }
        }

        // If wideband mode is active, disable it first to turn off bias-tee/noise source
        if (wideband_mode_enabled.load(std::memory_order_relaxed)) {
            cout << "Disabling wideband mode before manual frequency retune (prevents bias-tee staying active)" << endl;

            wideband_mode_enabled = false;

            send_control_command("{\"set_wideband_mode\":{\"enable\":false}}");
            broadcast("{\"wideband_mode\":{\"enabled\":false}}");

            // Restore DoA state if it was enabled before wideband
            if (doa_enabled_before_wideband.load(std::memory_order_relaxed)) {
                doa_enabled = true;
                setAllDoaEnabled(true);
                cout << "DoA processing re-enabled (restored previous state)" << endl;
            }
        }

        ChannelManager::set_frequency(static_cast<float>(freq_hz), active_channel.load(std::memory_order_relaxed));

        forEachMusicProcessor([&](auto* mp) { mp->setFrequency(static_cast<float>(freq_hz)); });

        stringstream json;
        json << "{\"set_frequency\":{\"frequency\":" << freq_hz << ",\"channel\":" << active_channel.load(std::memory_order_relaxed) << "}}";
        send_control_command(json.str());

        // Wideband variant: mixer side and antenna ring follow the frequency.
        // heimdall switches both itself on the retune (wideband_retune_rf);
        // mirror the same rules here for the UI state and persistence, and
        // drive the Wideband topology's auto radius on ring changes.
        if (wb_variant_enabled.load(std::memory_order_relaxed)) {
            bool changed = false;

            int side = wb_variant_mixer_side.load(std::memory_order_relaxed);
            if (!wb_side_valid(side, freq_hz)) {
                side = wb_auto_side(freq_hz);
                wb_variant_mixer_side = side;
                string side_cmd = string("MIXER_SIDE:") + wb_mixer_side_name(side);
                SettingsStore::record(side_cmd);
                broadcast(make_sync_cmd_json(side_cmd));
                cout << "Mixer side auto-selected: " << wb_mixer_side_name(side)
                     << " for " << freq_hz / 1e6 << " MHz" << endl;
                changed = true;
            }

            int ring = wb_ring_for_rf(freq_hz);
            if (ring != wb_variant_array.load(std::memory_order_relaxed)) {
                wb_variant_array = ring;
                cout << "Antenna ring auto-selected: " << WB_RING_NAMES[ring]
                     << " for " << freq_hz / 1e6 << " MHz" << endl;
                changed = true;

                if (wb_topology_active.load(std::memory_order_relaxed)) {
                    const float r = WB_RING_RADIUS_MM[ring];
                    forEachMusicProcessor([&](auto* mp) { mp->setArrayRadius(r); });
                    cout << "Wideband topology: array radius set to " << r
                         << " mm (" << WB_RING_NAMES[ring] << " ring)" << endl;
                }
            }

            if (changed) broadcast(build_wb_variant_json());

            // Re-assert the side on every retune: heimdall no-ops when it
            // already matches, and converges back if the two ends drifted
            // (a dropped command, a heimdall restart, a retune made through
            // heimdall's own web UI).
            stringstream side_json;
            side_json << "{\"set_mixer_side\":{\"side\":\"" << wb_mixer_side_name(side) << "\"}}";
            send_control_command(side_json.str());
        }
    }
    else if (message.starts_with("MIXER_SIDE:")) {
        // Wideband variant: choose where the LO sits.
        // high => LO = IF + RF (spectrum inversion corrected by heimdall),
        // low => LO = IF - RF (upright; RF capped at IF - LO_MIN),
        // below => LO = RF - IF (upright; the only side past 4.1 GHz).
        string_view side_sv = message.substr(11);
        int side;
        if (side_sv == "high") side = WB_SIDE_HIGH;
        else if (side_sv == "low") side = WB_SIDE_LOW;
        else if (side_sv == "below") side = WB_SIDE_BELOW;
        else {
            throw CommandRejected("use high|low|below");
        }

        // Outside wideband mode, or replaying the saved settings: keep the
        // preference but touch no hardware. The replay runs MIXER_SIDE before
        // FREQ, while the RF is still the 100 MHz default - validating here
        // rejected e.g. a saved "below" (1353+ MHz) on every start. The FREQ
        // replay that follows keeps the side if it reaches the saved RF and
        // otherwise auto-selects, and sends it to heimdall either way.
        if (!wb_variant_enabled.load(std::memory_order_relaxed) || g_replaying_settings.load()) {
            wb_variant_mixer_side = side;
            return;
        }

        // Manual override: only sides that can reach the CURRENT RF are
        // selectable (the UI greys the rest out); reject others instead of
        // moving the frequency. The side otherwise follows the frequency
        // automatically (see the FREQ handler).
        int ch = active_channel.load(std::memory_order_relaxed);
        uint64_t rf = static_cast<uint64_t>(llround(ChannelManager::get_frequency(ch)));
        if (!wb_side_valid(side, rf)) {
            // Snap any stale browser UI back to the real state
            broadcast(build_wb_variant_json());
            throw CommandRejected(string(wb_mixer_side_name(side)) + " side cannot reach current RF " +
                                  to_string(rf / 1000000) + " MHz");
        }

        wb_variant_mixer_side = side;

        stringstream json;
        json << "{\"set_mixer_side\":{\"side\":\"" << wb_mixer_side_name(side) << "\"}}";
        send_control_command(json.str());

        // Push the full variant state (side + new RF limits) to all browsers
        broadcast(build_wb_variant_json());

        cout << "Mixer side set to " << wb_mixer_side_name(side)
             << (side == WB_SIDE_HIGH ? " (LO = IF + RF)"
               : side == WB_SIDE_LOW  ? " (LO = IF - RF)" : " (LO = RF - IF)") << endl;
    }
    else if (message.starts_with("LO_CURRENT:")) {
        // Wideband variant: LO synthesizer output drive current (0-7). Shared
        // by all mixers (one LO), so no recalibration is needed on change.
        int current = static_cast<int>(parse_float(message, 11));
        if (current < 0 || current > 7) {
            throw CommandRejected("use 0-7");
        }

        if (!wb_variant_enabled.load(std::memory_order_relaxed)) {
            // Remembered/replayed setting outside wideband mode - keep the
            // preference but touch no hardware.
            wb_variant_lo_current = current;
            return;
        }

        wb_variant_lo_current = current;

        stringstream json;
        json << "{\"set_lo_current\":{\"current\":" << current << "}}";
        send_control_command(json.str());

        broadcast(build_wb_variant_json());
        cout << "LO drive current set to " << current << " (0-7)" << endl;
    }
    else if (message.starts_with("WIDEBAND_FREQ:")) {
        // Change frequency while staying in wideband mode (no cooldown needed)
        float freq_mhz = parse_float(message, 14);
        uint32_t freq_hz = static_cast<uint32_t>(freq_mhz * 1e6);

        ChannelManager::set_frequency(freq_hz, active_channel.load(std::memory_order_relaxed));

        // Send frequency change to server - server will handle wideband tuner spread
        stringstream json;
        json << "{\"set_frequency\":{\"frequency\":" << freq_hz << "}}";
        send_control_command(json.str());

        cout << "Wideband frequency changed to " << freq_mhz << " MHz" << endl;
    }
    else if (message.starts_with("FREQ_OFFSET:")) {
        float offset_khz = parse_float(message, 12);
        handle_freq_offset(0, offset_khz);

        stringstream json;
        json << "{\"set_freq_offset\":{\"offset_hz\":" << (offset_khz * 1000.0f) << "}}";
        send_control_command(json.str());
    }
    else if (message.starts_with("FREQ_OFFSET_1:")) {
        handle_freq_offset(0, parse_float(message, 14));
    }
    else if (message.starts_with("FREQ_OFFSET_2:")) {
        handle_freq_offset(1, parse_float(message, 14));
    }
    else if (message.starts_with("GAIN:")) {
        float gain_db = parse_float(message, 5);
        int ch = active_channel.load(std::memory_order_relaxed);
        ChannelManager::set_gain(gain_db, ch);

        stringstream json;
        json << "{\"set_gain\":{\"gain\":" << gain_db << ",\"channel\":" << ch << "}}";
        send_control_command(json.str());
    }
    else if (message.starts_with("CHANNEL:")) {
        int new_channel = parse_int(message, 8);
        if (new_channel >= 0 && new_channel < MAX_CHANNELS) {
            active_channel = new_channel;
            cout << "Active channel changed to: " << new_channel << " (Display & FM)" << endl;

            // Clear audio buffer immediately for instant channel switching
            fm_demod.reset_audio_buffer();

            lock_guard<mutex> lock(fft_mutex);
            if (new_channel < static_cast<int>(fft_magnitudes.size()) && new_channel < static_cast<int>(fft_averaged.size())) {
                fft_averaged[new_channel] = fft_magnitudes[new_channel];
            }
        }
    }
    else if (message.starts_with("AVG:")) {
        // Weight of the newest FFT frame; the UI sends 1 - slider/100. Outside
        // [0, 1] the running average diverges.
        float alpha = std::clamp(parse_float(message, 4), 0.0f, 1.0f);
        averaging_alpha = alpha;
        set_applied("AVG:", alpha, 3);

        // In wideband mode, reset ALL channel FFTs and the wideband stitched buffer
        // Otherwise, only reset the active channel
        bool is_wideband = wideband_mode_enabled.load(std::memory_order_relaxed);

        lock_guard<mutex> lock(fft_mutex);
        int channels = num_channels.load(std::memory_order_relaxed);
        int active_ch = active_channel.load(std::memory_order_relaxed);

        if (is_wideband) {
            // Reset all channel FFTs
            for (int ch = 0; ch < min(channels, MAX_CHANNELS); ch++) {
                if (ch < static_cast<int>(fft_magnitudes.size()) && ch < static_cast<int>(fft_averaged.size())) {
                    fft_averaged[ch] = fft_magnitudes[ch];
                }
            }

            // Clear wideband stitched averaging buffer (same as scanner retune does)
            lock_guard<mutex> wb_lock(wideband_fft_mutex);
            wideband_fft_averaged.clear();

            cout << "FFT averaging reset: All " << channels << " channels + wideband buffer cleared (wideband mode)" << endl;
        } else {
            // Normal mode: only reset active channel
            if (active_ch < min(channels, MAX_CHANNELS)) {
                fft_averaged[active_ch] = fft_magnitudes[active_ch];
            }
            cout << "FFT averaging reset: Channel " << active_ch << " (normal mode)" << endl;
        }
    }
    else if (message.starts_with("FM:")) {
        bool enable = parse_bool(message.substr(3));

        if (enable != fm_enabled.load(std::memory_order_relaxed)) {
            fm_enabled = enable;

            if (enable) {
                cout << "Enabling FM demodulator on channel " << active_channel.load(std::memory_order_relaxed) << endl;
                fm_demod.enable_processing();
            } else {
                cout << "Disabling FM demodulator" << endl;
                fm_demod.disable_processing();
            }
        }
    }
    else if (message.starts_with("DEMOD_MODE:")) {
        string mode_str = string(message.substr(11));
        DemodulatorMode new_mode;
        int suggested_bandwidth_index = -1;  // -1 means don't change

        if (mode_str == "NBFM" || mode_str == "nbfm") {
            new_mode = DemodulatorMode::NBFM;
            suggested_bandwidth_index = 20;  // 12 kHz - appropriate for NBFM
        } else if (mode_str == "AM" || mode_str == "am") {
            new_mode = DemodulatorMode::AM;
            suggested_bandwidth_index = 14;  // 60 kHz - appropriate for AM
        } else {
            new_mode = DemodulatorMode::WBFM;  // Default to WBFM
            suggested_bandwidth_index = 7;    // 240 kHz - appropriate for WBFM
        }

        fm_demod.setDemodulatorMode(new_mode);

        // Auto-adjust bandwidth for FM decimator to match demod mode
        if (suggested_bandwidth_index >= 0) {
            int fm_dec_id = decimator_manager.getFMDecimatorId();
            if (fm_dec_id >= 0) {
                decimator_manager.setBandwidthIndex(fm_dec_id, suggested_bandwidth_index);
                cout << "Auto-adjusted FM decimator bandwidth to "
                     << get_bandwidth_option(suggested_bandwidth_index).display_name
                     << " for " << mode_str << " mode" << endl;

                // Update FM demodulator with new input rate
                auto fm_dec_inst = decimator_manager.getDecimator(fm_dec_id);
                if (fm_dec_inst && fm_dec_inst->decimator) {
                    fm_demod.setInputSampleRate(fm_dec_inst->decimator->getOutputRate());
                }

                // Invalidate the FM decimator's beamformed FFT to force refresh
                // with the new bandwidth.
                if (fm_dec_inst) {
                    fm_dec_inst->beamformed_fft.valid.store(false, std::memory_order_release);
                }

                record_decimator_snapshot();  // bandwidth auto-adjust changed VFO state
            }
        }

        // Broadcast the mode change to all WebSocket clients
        const char* mode_name;
        switch (new_mode) {
            case DemodulatorMode::WBFM: mode_name = "WBFM"; break;
            case DemodulatorMode::NBFM: mode_name = "NBFM"; break;
            case DemodulatorMode::AM:   mode_name = "AM"; break;
            default: mode_name = "WBFM"; break;
        }

        // Include the new bandwidth in the response so UI can update
        stringstream json;
        json << "{\"demod_mode\":{\"mode\":\"" << mode_name << "\"";
        if (suggested_bandwidth_index >= 0) {
            json << ",\"bandwidth_index\":" << suggested_bandwidth_index;
        }
        json << "}}";
        broadcast(json.str());

        cout << "Demodulator mode set to " << mode_name << endl;
    }
    else if (message.starts_with("BANDWIDTH:")) {
        int bandwidth_index = parse_int(message, 10);

        // Set bandwidth on all decimators
        auto all_decimators = decimator_manager.getAllDecimators();
        for (const auto& decimator : all_decimators) {
            if (decimator && !decimator->being_deleted.load(std::memory_order_relaxed)) {
                decimator_manager.setBandwidthIndex(decimator->id, bandwidth_index);
            }
        }

        // Update FM demodulator with new input rate from FM decimator
        int fm_decimator = decimator_manager.getFMDecimatorId();
        if (fm_decimator >= 0) {
            auto fm_dec_inst = decimator_manager.getDecimator(fm_decimator);
            if (fm_dec_inst && fm_dec_inst->decimator) {
                float new_rate_hz = fm_dec_inst->decimator->getBandwidthMhz() * 1e6f;
                fm_demod.setInputSampleRate(new_rate_hz);
                cout << "Processing bandwidth changed to: " << fm_dec_inst->decimator->getBandwidthName() << endl;
            }
        }

        cout << "Bandwidth updated for all decimators" << endl;
        record_decimator_snapshot();
    }
    else if (message.starts_with("DOA:")) {
        bool enable = parse_bool(message.substr(4));

        if (enable != doa_enabled.load(std::memory_order_relaxed)) {
            doa_enabled = enable;
            setAllDoaEnabled(enable);
            cout << (enable ? "Enabling" : "Disabling") << " MUSIC DoA processors for all decimators" << endl;
        }
    }
    else if (message.starts_with("BEAMFORMING:")) {
        bool enable = parse_bool(message.substr(12));

        if (enable != beamforming_enabled.load(std::memory_order_relaxed)) {
            beamforming_enabled = enable;

            if (enable) {
                decimator_manager.setBeamformingEnabledAll(true);
                cout << "Beamforming enabled - coherent combining of all "
                     << active_num_elements.load(std::memory_order_relaxed)
                     << " channels on every decimator" << endl;

                if (!doa_enabled.load(std::memory_order_relaxed)) {
                    doa_enabled = true;
                    setAllDoaEnabled(true);
                    cout << "DoA auto-enabled for beamforming steering" << endl;
                }
            } else {
                decimator_manager.setBeamformingEnabledAll(false);
                cout << "Beamforming disabled - using single channel" << endl;
            }

            stringstream json;
            json << "{\"beamforming\":{\"enabled\":" << (enable ? "true" : "false") << "}}";
            broadcast(json.str());
        }
    }
    else if (message.starts_with("BEAMFORMING_MODE:")) {
        string mode_str = string(message.substr(17));

        BeamformingMode new_mode;
        string mode_name;
        if (mode_str == "MVDR" || mode_str == "mvdr") {
            new_mode = BeamformingMode::MVDR;
            mode_name = "MVDR";
        } else if (mode_str == "DIVERSITY" || mode_str == "diversity") {
            new_mode = BeamformingMode::SELECTION_DIVERSITY;
            mode_name = "DIVERSITY";
        } else if (mode_str == "FREQ_DAS" || mode_str == "freq_das" || mode_str == "FD-DAS") {
            new_mode = BeamformingMode::FREQ_DOMAIN_DAS;
            mode_name = "FREQ_DAS";
        } else {
            new_mode = BeamformingMode::DELAY_AND_SUM;
            mode_name = "DAS";
        }

        decimator_manager.setBeamformingModeAll(new_mode);

        stringstream json;
        json << "{\"beamforming_mode\":{\"mode\":\"" << mode_name << "\"}}";
        broadcast(json.str());

        cout << "Beamforming mode set to " << mode_name << " (all decimators)" << endl;
    }
    else if (message.starts_with("MVDR_DIAGONAL_LOADING:")) {
        float alpha = parse_float(message, 22);
        decimator_manager.setMVDRDiagonalLoadingAll(alpha);

        stringstream json;
        json << "{\"mvdr_config\":{\"diagonal_loading\":" << alpha << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("MVDR_SNAPSHOTS:")) {
        size_t snapshots = static_cast<size_t>(parse_int(message, 15));
        decimator_manager.setMVDRSnapshotLengthAll(snapshots);

        stringstream json;
        json << "{\"mvdr_config\":{\"snapshot_length\":" << snapshots << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("MANUAL_STEERING:")) {
        int enable = parse_int(message, 16);
        manual_steering_enabled.store(enable != 0, std::memory_order_relaxed);

        stringstream json;
        json << "{\"manual_steering\":{\"enabled\":" << (enable ? "true" : "false")
             << ",\"angle\":" << manual_steering_angle.load(std::memory_order_relaxed) << "}}";
        broadcast(json.str());

        cout << "Manual steering " << (enable ? "enabled" : "disabled")
             << " (angle=" << manual_steering_angle.load() << "°)" << endl;
    }
    else if (message.starts_with("STEERING_ANGLE:")) {
        float angle = parse_float(message, 15);
        angle = wrap_degrees(angle);
        manual_steering_angle.store(angle, std::memory_order_relaxed);
        set_applied("STEERING_ANGLE:", angle, 2);

        stringstream json;
        json << "{\"manual_steering\":{\"enabled\":" << (manual_steering_enabled.load() ? "true" : "false")
             << ",\"angle\":" << angle << "}}";
        broadcast(json.str());

        cout << "Manual steering angle set to " << angle << "°" << endl;
    }
    else if (message.starts_with("RADIUS:")) {
        if (wb_topology_active.load(std::memory_order_relaxed)) {
            // The Wideband topology derives the radius from the active ring
            cout << "RADIUS: ignored - Wideband topology sets the radius from the antenna ring" << endl;
            return;
        }
        float radius_mm = parse_float(message, 7);
        forEachMusicProcessor([&](auto* mp) { mp->setArrayRadius(radius_mm); });
        cout << "DoA array radius set to " << radius_mm << " mm for all decimators" << endl;
    }
    else if (message.starts_with("TOPOLOGY:")) {
        string topology_str = string(message.substr(9));
        ArrayTopology new_topology;

        // WIDEBAND: the KrakenSDR Wideband array - UCA math with the radius
        // auto-set from the antenna ring the tuned frequency selects
        // (WB_RING_RADIUS_MM). Only meaningful with --wideband; otherwise
        // fall back to plain UCA.
        bool wb_topo = (topology_str == "WIDEBAND");
        if (wb_topo && !wb_variant_enabled.load(std::memory_order_relaxed)) {
            cout << "TOPOLOGY: WIDEBAND requires --wideband mode - using UCA" << endl;
            wb_topo = false;
        }
        wb_topology_active = wb_topo;

        if (topology_str == "ULA") new_topology = ArrayTopology::ULA;
        else if (topology_str == "CUSTOM") new_topology = ArrayTopology::CUSTOM;
        else new_topology = ArrayTopology::UCA;  // UCA and WIDEBAND

        forEachMusicProcessor([&](auto* mp) { mp->setArrayTopology(new_topology); });

        if (wb_topo) {
            const int ring = wb_variant_array.load(std::memory_order_relaxed);
            const float r = WB_RING_RADIUS_MM[ring];
            forEachMusicProcessor([&](auto* mp) { mp->setArrayRadius(r); });
            cout << "DoA array topology set to WIDEBAND: radius " << r << " mm ("
                 << WB_RING_NAMES[ring] << " ring, follows frequency)" << endl;
        } else {
            cout << "DoA array topology set to " << topology_str << " for all decimators" << endl;
        }
    }
    else if (message.starts_with("SPACING:")) {
        float spacing_mm = parse_float(message, 8);
        forEachMusicProcessor([&](auto* mp) { mp->setElementSpacing(spacing_mm); });
        cout << "DoA element spacing set to " << spacing_mm << " mm for all decimators" << endl;
    }
    else if (message.starts_with("CUSTOM_POSITIONS:")) {
        // Format: CUSTOM_POSITIONS:x0,y0,z0;x1,y1,z1;x2,y2,z2;x3,y3,z3;x4,y4,z4
        // Persisted: TOPOLOGY:CUSTOM alone came back after a restart with the
        // default positions (wrong bearings until the table was re-edited).
        string positions_str = string(message.substr(17));
        if (positions_str.empty()) return;  // none saved: keep the defaults
        std::array<ElementPosition, DOA_NUM_ELEMENTS> positions;

        // Parse positions
        stringstream ss(positions_str);
        string element_str;
        int elem_idx = 0;

        while (getline(ss, element_str, ';') && elem_idx < DOA_NUM_ELEMENTS) {
            stringstream elem_ss(element_str);
            string coord;
            int coord_idx = 0;

            while (getline(elem_ss, coord, ',') && coord_idx < 3) {
                try {
                    float val = stof_finite(coord);
                    switch (coord_idx) {
                        case 0: positions[elem_idx].x_mm = val; break;
                        case 1: positions[elem_idx].y_mm = val; break;
                        case 2: positions[elem_idx].z_mm = val; break;
                    }
                } catch (...) {
                    cout << "Warning: Could not parse coordinate " << coord << endl;
                }
                coord_idx++;
            }
            elem_idx++;
        }

        // Applying positions from the UI switches to CUSTOM; the startup
        // replay only restores them - the saved TOPOLOGY decides the mode.
        const bool switch_topology = !g_replaying_settings.load();
        forEachMusicProcessor([&](auto* mp) {
            mp->setCustomPositions(positions);
            if (switch_topology) mp->setArrayTopology(ArrayTopology::CUSTOM);
        });

        // Check if 3D array
        bool is_3d = false;
        for (int i = 0; i < elem_idx; i++) {
            if (std::abs(positions[i].z_mm) > 0.01f) {
                is_3d = true;
                break;
            }
        }

        cout << "Custom element positions set for " << elem_idx << " elements ("
             << (is_3d ? "3D array" : "2D array") << ")" << endl;

        stringstream json;
        json << "{\"custom_positions\":{\"set\":true,\"num_elements\":" << elem_idx
             << ",\"is_3d\":" << (is_3d ? "true" : "false") << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("ELEVATION_RESOLUTION:")) {
        float resolution = std::clamp(parse_float(message, 21), 0.5f, 5.0f);
        forEachMusicProcessor([&](auto* mp) { mp->setElevationResolution(resolution); });
        set_applied("ELEVATION_RESOLUTION:", resolution, 2);
        cout << "Elevation resolution set to " << resolution << " degrees for all processors" << endl;
    }
    else if (message.starts_with("MUSIC_SNAPSHOT_LENGTH:")) {
        int snapshot_length = std::clamp(parse_int(message, 22), 64, 2048);
        set_applied("MUSIC_SNAPSHOT_LENGTH:", snapshot_length, 0);

        forEachMusicProcessor([&](auto* mp) {
            MUSICConfig config = mp->getConfig();
            config.snapshot_length = static_cast<size_t>(snapshot_length);
            config.overlap_samples = snapshot_length / 4;
            mp->setConfig(config);
        });

        cout << "MUSIC snapshot length set to " << snapshot_length << " samples for all processors" << endl;
    }
    else if (message.starts_with("MUSIC_NUM_SNAPSHOTS:")) {
        int num_snapshots = std::clamp(parse_int(message, 20), 8, 128);
        set_applied("MUSIC_NUM_SNAPSHOTS:", num_snapshots, 0);

        forEachMusicProcessor([&](auto* mp) {
            MUSICConfig config = mp->getConfig();
            config.num_snapshots = static_cast<size_t>(num_snapshots);
            config.min_snapshots = max(static_cast<size_t>(8), static_cast<size_t>(num_snapshots / 2));
            mp->setConfig(config);
        });

        cout << "MUSIC number of snapshots set to " << num_snapshots << " for all processors" << endl;
    }
    else if (message.starts_with("MUSIC_FB_AVERAGING:")) {
        // Forward-backward averaging; only takes effect for ULA topology
        bool enable = parse_bool(message.substr(19));
        forEachMusicProcessor([&](auto* mp) { mp->setFBAveragingEnabled(enable); });
        cout << "MUSIC forward-backward averaging " << (enable ? "enabled" : "disabled")
             << " for all processors (ULA only)" << endl;

        stringstream json;
        json << "{\"music_fb_averaging\":{\"enabled\":" << (enable ? "true" : "false") << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("MUSIC_COVARIANCE_ALPHA:")) {
        // Temporal covariance smoothing: alpha = new-frame weight, 1.0 = off
        float alpha = std::clamp(parse_float(message, 23), 0.05f, 1.0f);
        forEachMusicProcessor([&](auto* mp) { mp->setCovarianceAveragingAlpha(alpha); });
        set_applied("MUSIC_COVARIANCE_ALPHA:", alpha, 2);
        cout << "MUSIC covariance averaging alpha set to " << alpha << " for all processors" << endl;

        stringstream json;
        json << "{\"music_covariance_alpha\":{\"alpha\":" << alpha << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("MUSIC_SIGNAL_SOURCES:")) {
        // Expected number of signal sources (signal-subspace dimension).
        // 0 = automatic per-frame estimation (eigenvalue-dominance threshold).
        // Clamp to the live element count (MUSIC re-clamps to M-1 at compute
        // time if the count changes afterwards).
        int sources = std::clamp(parse_int(message, 21), 0,
                                 active_num_elements.load(std::memory_order_relaxed) - 1);
        bool auto_mode = (sources == 0);
        forEachMusicProcessor([&](auto* mp) {
            mp->setAutoNumSources(auto_mode);
            if (!auto_mode) mp->setNumSignalSources(sources);
        });
        if (auto_mode) {
            cout << "MUSIC expected signal sources set to auto for all processors" << endl;
        } else {
            cout << "MUSIC expected signal sources set to " << sources << " for all processors" << endl;
        }

        stringstream json;
        json << "{\"music_signal_sources\":{\"sources\":" << sources << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("ULA_MODE:")) {
        // ULA forward/backward output truncation: FORWARD | BACKWARD | BOTH
        string mode_str = string(message.substr(9));
        ULAOutputMode mode;
        if (mode_str == "FORWARD") mode = ULAOutputMode::FORWARD;
        else if (mode_str == "BACKWARD") mode = ULAOutputMode::BACKWARD;
        else { mode = ULAOutputMode::BOTH; mode_str = "BOTH"; }

        forEachMusicProcessor([&](auto* mp) { mp->setULAOutputMode(mode); });
        cout << "MUSIC ULA output mode set to " << mode_str << " for all processors" << endl;

        stringstream json;
        json << "{\"ula_mode\":{\"mode\":\"" << mode_str << "\"}}";
        broadcast(json.str());
    }
    else if (message.starts_with("ARRAY_OFFSET:")) {
        // Array orientation offset added to the reported DoA (degrees).
        float offset = wrap_degrees(parse_float(message, 13));  // MUSIC wraps it the same way
        forEachMusicProcessor([&](auto* mp) { mp->setArrayOffset(offset); });
        set_applied("ARRAY_OFFSET:", offset, 2);
        cout << "MUSIC array offset angle set to " << offset << " degrees for all processors" << endl;

        stringstream json;
        json << "{\"array_offset\":{\"offset\":" << offset << "}}";
        broadcast(json.str());
    }
    // --- Station Information (callsign + location source) ---
    // The applied command is echoed to all browsers as {"sync_cmd":...} and
    // replayed to new clients (see is_replayed_command), so no explicit state
    // broadcast is needed here.
    else if (message.starts_with("STATION_ID:")) {
        string id = string(message.substr(11));
        station_info.setId(id);
        cout << "Station ID set to '" << id << "'" << endl;
    }
    else if (message.starts_with("LOCATION_SOURCE:")) {
        string src = string(message.substr(16));
        LocationSource ls;
        if (src == "static") ls = LocationSource::STATIC;
        else if (src == "gps") ls = LocationSource::GPS;
        else ls = LocationSource::MOBILE;  // "mobile" or anything unrecognized
        station_info.setSource(ls);
        cout << "Location source set to '" << src << "'" << endl;
    }
    else if (message.starts_with("STATIC_LOCATION:")) {
        // Format: <lat>,<lon>,<heading>. A malformed value throws in stod and is
        // caught by handle_websocket_message (state left unchanged).
        string payload = string(message.substr(16));
        size_t c1 = payload.find(',');
        size_t c2 = (c1 == string::npos) ? string::npos : payload.find(',', c1 + 1);
        if (c1 != string::npos && c2 != string::npos) {
            double lat = stod_finite(payload.substr(0, c1));
            double lon = stod_finite(payload.substr(c1 + 1, c2 - c1 - 1));
            double heading = stod_finite(payload.substr(c2 + 1));
            station_info.setStatic(lat, lon, heading);
            cout << "Static location set to " << lat << ", " << lon
                 << ", heading " << heading << endl;
        } else {
            cout << "STATIC_LOCATION: malformed payload '" << payload << "'" << endl;
        }
    }
    // --- Web mapper output (built-in replacement for web_mapper_middleware) ---
    // Applied commands are echoed as {"sync_cmd":...}, replayed to new clients
    // and persisted via the settings schema, like the station settings above.
    else if (message.starts_with("WEB_MAPPER:")) {
        bool enable = parse_bool(message.substr(11));
        web_mapper.setEnabled(enable);
        cout << "Web mapper output " << (enable ? "enabled" : "disabled") << endl;
    }
    else if (message.starts_with("WEB_MAPPER_MODE:")) {
        string mode = string(message.substr(16));
        web_mapper.setMode(mode);
        cout << "Web mapper mode set to '" << web_mapper.getMode() << "'" << endl;
    }
    else if (message.starts_with("WEB_MAPPER_KEY:")) {
        web_mapper.setKey(string(message.substr(15)));
        cout << "Web mapper KrakenPro key updated" << endl;
    }
    else if (message.starts_with("WEB_MAPPER_URL:")) {
        string url = string(message.substr(15));
        if (url.rfind("wss://", 0) != 0) {
            throw CommandRejected("must start with wss://");
        }
        web_mapper.setServerUrl(url);
        cout << "Web mapper server URL set to '" << url << "'" << endl;
    }
    else if (message.starts_with("WEB_MAPPER_WS_PORT:")) {
        int port = parse_int(message, 19);
        if (port < 1 || port > 65535) {
            throw CommandRejected("port must be 1-65535");
        }
        web_mapper.setLocalWsPort(port);
        cout << "Web mapper local WS port set to " << port << endl;
    }
    // Dynamic decimator controls
    else if (message.starts_with("ADD_DECIMATOR")) {
        int new_id = decimator_manager.addDecimator();
        if (new_id >= 0) {
            cout << "Added new decimator with ID: " << new_id << endl;

            stringstream json;
            json << "{\"decimator_added\":{\"id\":" << new_id << "}}";
            broadcast(json.str());
            record_decimator_snapshot();
        }
    }
    else if (message.starts_with("REMOVE_DECIMATOR:")) {
        int id = parse_int(message, 17);
        if (decimator_manager.removeDecimator(id)) {
            cout << "Removed decimator with ID: " << id << endl;

            stringstream json;
            json << "{\"decimator_removed\":{\"id\":" << id << "}}";
            broadcast(json.str());
            record_decimator_snapshot();
        }
    }
    else if (message.starts_with("SET_DECIMATOR_FREQ:")) {
        // Format: SET_DECIMATOR_FREQ:id:offset_khz
        string params = string(message.substr(19));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            float offset_khz = stof_finite(params.substr(colon_pos + 1));
            float offset_hz = offset_khz * 1000.0f;

            // The UI works in wideband-center-relative coordinates. In wideband
            // mode offset_hz is rewritten below to be relative to the selected
            // tuner (for internal DSP), so keep the original value to echo back
            // unchanged - otherwise the UI snaps the tuning bar to the per-tuner
            // offset, which is always near the middle of the wideband span.
            float ui_offset_hz = offset_hz;

            // In wideband mode, calculate which tuner contains the selected frequency
            // Store this in the decimator instance so each decimator can use different tuners
            int best_channel = active_channel.load();  // Default to current active
            if (wideband_mode_enabled.load()) {
                int nc = num_channels.load();
                if (nc > 0) {
                    // Calculate wideband spectrum center (average of all tuner frequencies)
                    float min_tuner_freq = ChannelManager::get_frequency(0);
                    float max_tuner_freq = ChannelManager::get_frequency(nc - 1);
                    float wideband_center_hz = (min_tuner_freq + max_tuner_freq) / 2.0f;

                    // Calculate absolute frequency user selected
                    // offset_hz is relative to wideband center
                    float selected_freq_hz = wideband_center_hz + offset_hz;

                    // Find which tuner contains this frequency (closest tuner)
                    best_channel = 0;
                    float min_distance = std::abs(selected_freq_hz - ChannelManager::get_frequency(0));

                    for (int ch = 1; ch < nc; ch++) {
                        float ch_freq_hz = ChannelManager::get_frequency(ch);
                        float distance = std::abs(selected_freq_hz - ch_freq_hz);

                        if (distance < min_distance) {
                            min_distance = distance;
                            best_channel = ch;
                        }
                    }

                    // Recalculate offset relative to selected channel
                    float best_ch_freq_hz = ChannelManager::get_frequency(best_channel);
                    offset_hz = selected_freq_hz - best_ch_freq_hz;

                    // Store which tuner this decimator should use
                    auto decimator_inst = decimator_manager.getDecimator(id);
                    if (decimator_inst) {
                        decimator_inst->wideband_tuner_channel = best_channel;
                        cout << "Decimator " << id << " → Tuner " << best_channel
                             << " (freq=" << (ChannelManager::get_frequency(best_channel) / 1e6f) << " MHz)"
                             << ", offset=" << (offset_hz / 1000.0f) << " kHz" << endl;
                    }

                    // Update global active_channel for FM demod if this is the FM source
                    if (decimator_manager.getFMDecimatorId() == id) {
                        active_channel = best_channel;
                        cout << "Updated global active_channel to " << best_channel << " (FM source)" << endl;
                    }
                }
            }

            if (decimator_manager.setFrequencyOffset(id, offset_hz)) {
                cout << "Decimator " << id << " frequency offset set to " << (offset_hz / 1000.0f) << " kHz" << endl;

                stringstream json;
                json << "{\"decimator_freq_offset\":{\"id\":" << id << ",\"offset_hz\":" << ui_offset_hz << "}}";
                broadcast(json.str());
                record_decimator_snapshot();
            }
        }
    }
    else if (message.starts_with("SET_DECIMATOR_BW:")) {
        // Format: SET_DECIMATOR_BW:id:bandwidth_index
        string params = string(message.substr(17));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            int bw_index = stoi(params.substr(colon_pos + 1));

            if (decimator_manager.setBandwidthIndex(id, bw_index)) {
                cout << "Decimator " << id << " bandwidth index set to " << bw_index << endl;

                stringstream json;
                json << "{\"decimator_bandwidth\":{\"id\":" << id << ",\"bandwidth_index\":" << bw_index << "}}";
                broadcast(json.str());
                record_decimator_snapshot();
            }
        }
    }
    else if (message.starts_with("SET_FM_DECIMATOR:")) {
        // Set which decimator feeds the FM demodulator
        int id = parse_int(message, 17);
        decimator_manager.setFMDecimatorId(id);

        // Apply the decimator's demod mode to the FM demodulator
        auto decimator_inst = decimator_manager.getDecimator(id);
        if (decimator_inst) {
            fm_demod.setDemodulatorMode(decimator_inst->demod_mode);

            cout << "FM demodulator now using decimator " << id
                 << " (offset=" << (decimator_inst->frequency_offset_hz / 1000.0f) << " kHz"
                 << ", mode=" << DecimatorManager::demodModeToString(decimator_inst->demod_mode) << ")" << endl;
        } else {
            cout << "FM demodulator now using decimator " << id << endl;
        }

        // Clear audio buffer immediately for instant FM source switching
        fm_demod.reset_audio_buffer();

        stringstream json;
        json << "{\"fm_decimator\":{\"id\":" << id << "}}";
        broadcast(json.str());
        record_decimator_snapshot();
    }
    else if (message.starts_with("SET_DECIMATOR_DEMOD:")) {
        // Format: SET_DECIMATOR_DEMOD:id:mode
        string params = string(message.substr(20));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            string mode_str = params.substr(colon_pos + 1);

            DemodulatorMode new_mode = DecimatorManager::stringToDemodMode(mode_str);

            if (decimator_manager.setDemodMode(id, new_mode)) {
                // If this is the FM source decimator, apply the mode to the FM demodulator immediately
                if (decimator_manager.getFMDecimatorId() == id) {
                    fm_demod.setDemodulatorMode(new_mode);
                    cout << "FM demodulator mode updated to " << DecimatorManager::demodModeToString(new_mode)
                         << " (from FM source decimator " << id << ")" << endl;
                }

                // Broadcast update to UI
                stringstream json;
                json << "{\"decimator_demod\":{\"id\":" << id
                     << ",\"mode\":\"" << DecimatorManager::demodModeToString(new_mode) << "\"}}";
                broadcast(json.str());
                record_decimator_snapshot();
            }
        }
    }
    else if (message.starts_with("GET_DECIMATOR_INFO")) {
        // Send current decimator configuration to UI
        broadcast(MessageBuilders::build_decimator_info_message());
    }
    else if (message.starts_with("DECIMATORS:")) {
        // Persisted-settings replay: rebuild the whole VFO setup in one shot.
        if (apply_decimator_snapshot(string(message.substr(11)))) {
            handle_message_impl("GET_DECIMATOR_INFO");  // push restored state to browsers
        }
    }
    else if (message.starts_with("WIDEBAND_MODE:")) {
        bool enable = parse_bool(message.substr(14));
        wideband_mode_enabled = enable;

        // Handle DoA state when toggling wideband mode
        if (enable) {
            doa_enabled_before_wideband = doa_enabled.load(std::memory_order_relaxed);
            if (doa_enabled.load(std::memory_order_relaxed)) {
                doa_enabled = false;
                setAllDoaEnabled(false);
                cout << "DoA processing disabled for wideband mode (will restore when disabled)" << endl;
            }
        } else {
            if (doa_enabled_before_wideband.load(std::memory_order_relaxed)) {
                doa_enabled = true;
                setAllDoaEnabled(true);
                cout << "DoA processing re-enabled (restored previous state)" << endl;
            }
        }

        stringstream json;
        json << "{\"set_wideband_mode\":{\"enable\":" << (enable ? "true" : "false") << "}}";
        send_control_command(json.str());

        cout << "Wideband scan mode " << (enable ? "enabled" : "disabled") << endl;

        stringstream response;
        response << "{\"wideband_mode\":{\"enabled\":" << (enable ? "true" : "false") << "}}";
        broadcast(response.str());
    }
    else if (message.starts_with("WIDEBAND_BASE_FREQ:")) {
        float freq_mhz = parse_float(message, 19);
        uint32_t base_freq_hz = static_cast<uint32_t>(freq_mhz * 1e6);

        // Send command to Heimdall server
        stringstream json;
        json << "{\"set_wideband_frequencies\":{\"base_frequency\":" << base_freq_hz << "}}";
        send_control_command(json.str());

        cout << "Wideband base frequency set to " << freq_mhz << " MHz" << endl;
    }
    // Discrete Scanner Commands
    else if (message.starts_with("SCANNER_LOAD_CONFIG:")) {
        // Load scanner configuration from JSON
        string config_json = string(message.substr(20));
        if (scanner_manager.loadConfig(config_json)) {
            cout << "Scanner configuration loaded successfully" << endl;
            broadcast("{\"scanner_config_loaded\":true}");
        } else {
            cerr << "Failed to load scanner configuration" << endl;
            broadcast("{\"scanner_config_loaded\":false}");
        }
    }
    else if (message.starts_with("SCANNER_GET_CONFIG")) {
        string config_json = scanner_manager.getConfigJson();
        stringstream response;
        response << "{\"scanner_config\":" << config_json << "}";
        broadcast(response.str());
    }
    else if (message.starts_with("SCANNER_START")) {
        // Start discrete scanner
        if (!scanner_manager.start()) {
            cerr << "Failed to start discrete scanner" << endl;
        }
    }
    else if (message.starts_with("SCANNER_STOP")) {
        // Stop discrete scanner
        scanner_manager.stop();
    }
    else if (message.starts_with("SCANNER_NEXT")) {
        scanner_manager.next();
    }
    else if (message.starts_with("SCANNER_SET_SQUELCH:")) {
        scanner_manager.setSquelch(parse_float(message, 20));
    }
    else if (message.starts_with("SCANNER_SET_DWELL:")) {
        int dwell_ms = parse_int(message, 18);
        scanner_manager.setDwellTime(dwell_ms);
    }
    // Continuous Scanner Commands
    else if (message.starts_with("CONTINUOUS_SCANNER_START")) {
        continuous_scanner.start();
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_STOP")) {
        continuous_scanner.stop();
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_DWELL:")) {
        int dwell_ms = parse_int(message, 25);
        continuous_scanner.setDwellTime(dwell_ms);

        stringstream json;
        json << "{\"continuous_scanner\":{\"dwell_ms\":" << dwell_ms << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_SQUELCH:")) {
        float squelch_db = parse_float(message, 27);
        continuous_scanner.setSquelchLevel(squelch_db);

        stringstream json;
        json << "{\"continuous_scanner\":{\"squelch_db\":" << squelch_db << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_DECAY:")) {
        int decay_ms = parse_int(message, 25);
        continuous_scanner.setDecayTime(decay_ms);

        stringstream json;
        json << "{\"continuous_scanner\":{\"decay_ms\":" << decay_ms << "}}";
        broadcast(json.str());
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_STATUS")) {
        broadcast(continuous_scanner.getStatusJson());
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_SHOW_SQUELCH:")) {
        continuous_scanner.setShowSquelch(parse_bool(message.substr(32)));
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_WIDEBAND_RANGE:")) {
        // Format: CONTINUOUS_SCANNER_WIDEBAND_RANGE:start_mhz:end_mhz
        string params = string(message.substr(34));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            float start_mhz = stof_finite(params.substr(0, colon_pos));
            float end_mhz = stof_finite(params.substr(colon_pos + 1));
            // Reject (not stored, not echoed) anything that can't be a scan
            // range; buildBandPlan also caps the band count.
            if (!(start_mhz > 0.0f && end_mhz > start_mhz && end_mhz <= MAX_WIDEBAND_SCAN_MHZ)) {
                throw std::invalid_argument("wideband scan range out of bounds");
            }
            float start_hz = start_mhz * 1e6f;
            float end_hz = end_mhz * 1e6f;

            continuous_scanner.setWidebandScanRange(start_hz, end_hz);

            stringstream json;
            json << "{\"continuous_scanner\":{\"wideband_scan\":{"
                 << "\"start_mhz\":" << start_mhz
                 << ",\"end_mhz\":" << end_mhz << "}}}";
            broadcast(json.str());
        }
    }
    else if (message.starts_with("CONTINUOUS_SCANNER_WIDEBAND:")) {
        bool enable = parse_bool(message.substr(28));
        continuous_scanner.setWidebandScanEnabled(enable);

        stringstream json;
        json << "{\"continuous_scanner\":{\"wideband_scan\":{"
             << "\"enabled\":" << (enable ? "true" : "false") << "}}}";
        broadcast(json.str());
    }
    // FFT Settings
    else if (message.starts_with("FFT_SIZE:")) {
        int new_size = parse_int(message, 9);
        int old_size = FFTProcessor::get_current_size();

        // Resize FFT
        FFTProcessor::resize(new_size);

        // Auto-adjust decimation only for smaller FFT sizes (< 32768)
        // For large FFT sizes (32768+), user controls downsampling manually for fine-tuned resolution
        if (new_size < 32768) {
            int current_decimation = FFTProcessor::get_current_decimation();
            int output_points = FFTProcessor::get_current_size() / current_decimation;

            if (output_points > 4096) {
                // Calculate new decimation to target ~2048 output points
                int new_decimation = FFTProcessor::get_current_size() / 2048;
                // Round up to next power of 2 for efficiency
                int power = 1;
                while (power < new_decimation) power *= 2;
                new_decimation = power;

                FFTProcessor::set_decimation(new_decimation);
                cout << "Auto-adjusted downsampling to " << new_decimation
                     << " (output points: " << (FFTProcessor::get_current_size() / new_decimation) << ")" << endl;
            }
        }

        // Reset audio buffer to prevent corruption
        fm_demod.reset_audio_buffer();

        // Send acknowledgment to UI
        stringstream json;
        json << "{\"fft_settings\":{\"fft_size\":" << FFTProcessor::get_current_size()
             << ",\"decimation\":" << FFTProcessor::get_current_decimation() << "}}";
        broadcast(json.str());

        cout << "FFT size changed: " << old_size << " -> " << FFTProcessor::get_current_size() << endl;
    }
    else if (message.starts_with("FFT_DECIMATION:")) {
        int new_decimation = parse_int(message, 15);
        int old_decimation = FFTProcessor::get_current_decimation();

        // Set decimation
        FFTProcessor::set_decimation(new_decimation);

        // Reset audio buffer to prevent corruption
        fm_demod.reset_audio_buffer();

        // Send acknowledgment to UI
        stringstream json;
        json << "{\"fft_settings\":{\"fft_size\":" << FFTProcessor::get_current_size()
             << ",\"decimation\":" << FFTProcessor::get_current_decimation() << "}}";
        broadcast(json.str());

        cout << "FFT decimation changed: " << old_decimation << " -> " << FFTProcessor::get_current_decimation() << endl;
    }
    else if (message.starts_with("GET_FFT_SETTINGS")) {
        // Send current FFT settings to UI
        stringstream json;
        json << "{\"fft_settings\":{\"fft_size\":" << FFTProcessor::get_current_size()
             << ",\"decimation\":" << FFTProcessor::get_current_decimation() << "}}";
        broadcast(json.str());
    }
    // Squelch commands
    // Per-decimator squelch commands
    else if (message.starts_with("DEC_SQUELCH_ENABLE:")) {
        // Format: DEC_SQUELCH_ENABLE:id:0|1
        string params = string(message.substr(19));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            bool enable = (params.substr(colon_pos + 1) == "1");
            if (decimator_manager.setSquelchEnabled(id, enable))
                record_decimator_snapshot();
        }
    }
    else if (message.starts_with("DEC_SQUELCH_LEVEL:")) {
        // Format: DEC_SQUELCH_LEVEL:id:level_db
        string params = string(message.substr(18));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            float level = stof_finite(params.substr(colon_pos + 1));
            if (decimator_manager.setSquelchLevel(id, level))
                record_decimator_snapshot();
        }
    }
    else if (message.starts_with("DEC_SQUELCH_METHOD:")) {
        // Format: DEC_SQUELCH_METHOD:id:FFT|EIGEN|EIGEN_AUTO
        string params = string(message.substr(19));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            string method_str = params.substr(colon_pos + 1);
            int method = 0;
            if (method_str == "EIGEN_AUTO" || method_str == "AUTO" || method_str == "2") method = 2;
            else if (method_str == "EIGEN" || method_str == "EIGENVALUE" || method_str == "1") method = 1;
            if (decimator_manager.setSquelchMethod(id, method))
                record_decimator_snapshot();
        }
    }
    else if (message.starts_with("DEC_SQUELCH_EIGEN:")) {
        // Format: DEC_SQUELCH_EIGEN:id:threshold (linear eigenvalue ratio)
        string params = string(message.substr(18));
        size_t colon_pos = params.find(':');
        if (colon_pos != string::npos) {
            int id = stoi(params.substr(0, colon_pos));
            float threshold = stof_finite(params.substr(colon_pos + 1));
            if (decimator_manager.setSquelchEigenThreshold(id, threshold))
                record_decimator_snapshot();
        }
    }
    else if (message.starts_with("EDGE_CLIP:")) {
        // Set edge clip percentage for FFT display.
        // Lower bound must stay above 0: edge_clip==0 would give the wideband
        // scanner a zero step (infinite band plan) and stitch_wideband_fft
        // zero usable bins.
        float edge_clip = std::clamp(parse_float(message, 10), 0.1f, 1.0f);
        set_applied("EDGE_CLIP:", edge_clip, 2);

        float old_clip = current_edge_clip.exchange(edge_clip);

        cout << "Edge clip changed: " << (old_clip * 100.0f) << "% -> " << (edge_clip * 100.0f) << "%" << endl;

        // Forward to server so wideband tuner spacing matches FFT stitching
        stringstream server_json;
        server_json << "{\"set_wideband_edge_clip\":{\"edge_clip\":" << edge_clip << "}}";
        send_control_command(server_json.str());
        // Browsers pick up the change from the EDGE_CLIP sync_cmd echo.
    }
    // --- Local DoA recording ---
    else if (message.starts_with("LOG_START")) {
        bool ok = doa_logger.start();
        broadcast_recording_status();
        broadcast(build_log_list_json());  // new file appears in the list
        cout << "DoA logging start " << (ok ? "OK" : "FAILED") << endl;
    }
    else if (message.starts_with("LOG_STOP")) {
        doa_logger.stop();
        broadcast_recording_status();
        broadcast(build_log_list_json());  // final size reflected
    }
    else if (message.starts_with("LOG_INTERVAL:")) {
        double sec = parse_float(message, 13);  // "LOG_INTERVAL:" == 13 chars
        doa_logger.setIntervalSeconds(sec);
        cout << "DoA log interval set to " << sec << " s" << endl;
    }
    else if (message.starts_with("LOG_FORMAT:")) {
        string fmt = string(message.substr(11));  // "csv" | "sqlite"
        doa_logger.setFormatString(fmt);
        cout << "DoA log format set to " << fmt << endl;
    }
    else if (message.starts_with("LOG_FILE:")) {
        doa_logger.setFilename(string(message.substr(9)));  // bare name only
    }
    else if (message.starts_with("LOG_LIST")) {
        broadcast(build_log_list_json());
    }
    else if (message.starts_with("LOG_DELETE:")) {
        string name = string(message.substr(11));
        if (doa_logger.isRunning() && doa_sanitize_filename(name) == doa_logger.activeFilename()) {
            cout << "DoA log delete refused (recording in progress): " << name << endl;
        } else {
            bool ok = doa_delete_recording(name);
            cout << "DoA log delete '" << name << "': " << (ok ? "OK" : "not found") << endl;
        }
        broadcast(build_log_list_json());
    }
}

void ControlHandler::handle_websocket_message(string_view message) {
    // A malformed number in any command (stof/stoi throwing) must not
    // propagate into the uWS event loop, which would exit(1) the process.
    try {
        // UI:<KEY>:<value> - browser display settings (waterfall colormap
        // etc.) the backend doesn't interpret. Store the latest value per
        // key and relay to all browsers so their displays stay in sync.
        // Reset every persisted setting to its hardcoded default: write the
        // defaults to disk and apply them live (the browser reloads to pick up
        // the full default UI).
        if (message == "RESET_SETTINGS") {
            auto defaults = SettingsStore::reset_to_defaults();
            g_replaying_settings.store(true);
            for (const auto& cmd : defaults) handle_websocket_message(cmd);
            g_replaying_settings.store(false);
            return;
        }

        if (message.starts_with("UI:")) {
            size_t value_sep = message.find(':', 3);
            if (value_sep == string_view::npos) return;
            store_for_replay(message, value_sep);
            if (!g_replaying_settings.load()) SettingsStore::record(message);
            broadcast(make_sync_cmd_json(message));
            return;
        }

        g_applied_cmd.reset();
        handle_message_impl(message);
        // The command as applied (see set_applied); the raw request otherwise
        const string applied = g_applied_cmd ? *g_applied_cmd : string(message);

        // Echo the applied command to all browsers for settings sync
        // (secrets redacted - see redact_for_sync)
        if (!is_query_command(message)) {
            const string sync_msg = redact_for_sync(applied);
            if (is_replayed_command(message)) {
                store_for_replay(sync_msg, sync_msg.find(':'));
            }
            // Persist remembered settings to disk (the schema decides which;
            // skipped while we are replaying the saved file ourselves).
            if (!g_replaying_settings.load()) {
                SettingsStore::record(applied);
            }
            broadcast(make_sync_cmd_json(sync_msg));
        }
    } catch (const CommandRejected& e) {
        cout << "Rejected control message: '" << string(message.substr(0, 100))
             << "' (" << e.what() << ")" << endl;
    } catch (const exception& e) {
        cerr << "Ignoring malformed control message: '"
             << string(message.substr(0, 100)) << "' (" << e.what() << ")" << endl;
    }
}

void ControlHandler::send_control_command(const string& cmd) {
    DataReceiver::send_control_command(cmd);
}

void ControlHandler::apply_persisted_settings() {
    auto commands = SettingsStore::apply_commands();
    if (commands.empty()) return;
    cout << "Restoring " << commands.size() << " persisted setting(s)" << endl;
    g_replaying_settings.store(true);
    for (const auto& cmd : commands) {
        // Reuse the full command path: applies the setting, forwards tuner
        // changes to the server, and primes the browser-sync replay store.
        handle_websocket_message(cmd);
    }
    g_replaying_settings.store(false);
}