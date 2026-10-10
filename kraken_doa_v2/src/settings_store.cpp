#include "settings_store.hpp"
#include "utils/parse_num.hpp"
#include "utils/json_escape.hpp"

#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>

using namespace std;

namespace {

// Replace `path` with `data` atomically AND durably: write a temp file
// (checking every write), fsync it, rename it over the old file, then fsync
// the directory so the rename itself survives a power cut. On any failure the
// temp file is removed and the existing file is left untouched - a full disk
// or a power cut can never leave the settings truncated or empty.
bool write_file_atomic(const char* path, const std::string& data) {
    const std::string tmp = std::string(path) + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
    if (::close(fd) != 0) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path) != 0) { ::unlink(tmp.c_str()); return false; }
    const std::string p(path);
    const size_t slash = p.rfind('/');
    const std::string dir = (slash == std::string::npos) ? "." : p.substr(0, slash ? slash : 1);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }  // best effort
    return true;
}
    const char* SETTINGS_FILE = "doa_settings.json";

    enum class VType { BOOL, NUMBER, STRING };

    struct Setting {
        const char* key;     // JSON key
        const char* prefix;  // command prefix, including the trailing ':'
        VType type;          // how the value is written/read in JSON
        const char* def;     // hardcoded default, in command-suffix form
    };

    // The canonical list of remembered settings. Values are stored internally in
    // command-suffix form ("1", "49.6", "UCA", ...); the type only governs how
    // each is rendered in / parsed from JSON (bool 1<->true, number, string).
    // Order is the file's order and the startup replay order - CHANNEL precedes
    // FREQ/GAIN, which depend on the active channel.
    const Setting SCHEMA[] = {
        // Tuner / audio
        {"fm",                       "FM:",                       VType::BOOL,   "1"},
        {"doa",                      "DOA:",                      VType::BOOL,   "1"},
        {"channel",                  "CHANNEL:",                  VType::NUMBER, "0"},
        // Wideband (downconverter) variant mixing side (high|low|below);
        // applied only when running with --wideband, remembered either way.
        // MUST precede FREQ: the replayed frequency is range-checked against
        // the selected side.
        {"mixer_side",               "MIXER_SIDE:",               VType::STRING, "high"},
        // (antenna ring is no longer persisted - it is auto-selected from the
        // tuned frequency; a stale antenna_array field in an old settings file
        // is simply ignored)
        // Wideband variant LO output drive current (0-7)
        {"lo_current",               "LO_CURRENT:",               VType::NUMBER, "3"},
        {"frequency_mhz",            "FREQ:",                     VType::NUMBER, "100.000"},
        {"gain_db",                  "GAIN:",                     VType::NUMBER, "49.6"},
        {"averaging_alpha",          "AVG:",                      VType::NUMBER, "0.150"},
        // MUSIC / DoA array
        {"topology",                 "TOPOLOGY:",                 VType::STRING, "UCA"},
        // x,y,z;... in mm. Empty = none saved (the UCA default is used).
        {"custom_positions",         "CUSTOM_POSITIONS:",         VType::STRING, ""},
        // Patch / 3D topology layout "shape,elements,size_mm,height_mm" (UI state)
        {"array_layout",             "ARRAY_LAYOUT:",             VType::STRING, ""},
        {"array_radius_mm",          "RADIUS:",                   VType::NUMBER, "50"},
        {"element_spacing_mm",       "SPACING:",                  VType::NUMBER, "30"},
        {"ula_mode",                 "ULA_MODE:",                 VType::STRING, "BOTH"},
        {"custom_mode",              "CUSTOM_MODE:",              VType::STRING, "BOTH"},
        {"array_offset_deg",         "ARRAY_OFFSET:",             VType::NUMBER, "0"},
        // Forward-backward averaging (applied only while topology is ULA)
        {"fb_averaging",             "MUSIC_FB_AVERAGING:",       VType::BOOL,   "0"},
        // Temporal covariance smoothing: new-frame weight, 1.0 = off
        {"covariance_alpha",         "MUSIC_COVARIANCE_ALPHA:",   VType::NUMBER, "1.00"},
        {"num_snapshots",            "MUSIC_NUM_SNAPSHOTS:",      VType::NUMBER, "32"},
        {"snapshot_length",          "MUSIC_SNAPSHOT_LENGTH:",    VType::NUMBER, "256"},
        // Signal-subspace dimension; 0 = automatic (eigenvalue threshold)
        {"signal_sources",           "MUSIC_SIGNAL_SOURCES:",     VType::NUMBER, "1"},
        // 3D (custom, non-planar) arrays: degrees per elevation step
        {"elevation_resolution",     "ELEVATION_RESOLUTION:",     VType::NUMBER, "1.00"},
        // Beamforming / diversity
        {"beamforming",              "BEAMFORMING:",              VType::BOOL,   "0"},
        {"beamforming_mode",         "BEAMFORMING_MODE:",         VType::STRING, "DAS"},
        {"manual_steering",          "MANUAL_STEERING:",          VType::BOOL,   "0"},
        {"steering_angle_deg",       "STEERING_ANGLE:",           VType::NUMBER, "0"},
        {"mvdr_loading",             "MVDR_DIAGONAL_LOADING:",    VType::NUMBER, "0.500"},
        // Display
        {"edge_clip",                "EDGE_CLIP:",                VType::NUMBER, "0.80"},
        // FFT
        {"fft_size",                 "FFT_SIZE:",                 VType::NUMBER, "16384"},
        {"fft_downsampling",         "FFT_DECIMATION:",           VType::NUMBER, "8"},
        // Decimator (VFO) setup - one composite value because the set is
        // dynamic: "<fm_index>|<vfo>;<vfo>;..." with each <vfo> =
        // "offset_hz,bw_index,demod,squelch_en,squelch_db,squelch_method (0 FFT /
        // 3 Digital),0 (was the eigenvalue threshold),digital_mode,digital_opts,tuner"
        // and fm_index the LIST POSITION of the FM-source VFO (ids are
        // renumbered 0..N-1 across restarts). Built and parsed in
        // control_handler.cpp; the default is the single startup VFO and must
        // match DecimatorInstance's constructor defaults + DEFAULT_BANDWIDTH_INDEX
        // Incident map: addresses in decoder messages are looked up within
        // this many km of the station
        {"geo_radius_km",            "GEO_RADIUS_KM:",            VType::NUMBER, "300"},
        // Decoder data log (sidebar 🗂 Decoder Logging): the folder first, so
        // switching it on doesn't create the default folder
        {"decoder_log_dir",          "DECODER_LOG_DIR:",          VType::STRING, "decoder_logs"},
        {"decoder_log_types",        "DECODER_LOG_TYPES:",        VType::STRING, "event,message,position,incident"},
        {"decoder_log_days",         "DECODER_LOG_DAYS:",         VType::NUMBER, "7"},
        {"decoder_log_pos_s",        "DECODER_LOG_POS_S:",        VType::NUMBER, "10"},
        {"decoder_log",              "DECODER_LOG:",              VType::BOOL,   "0"},
        // Mobile DF heat map on the 🗺 Map: collect bearings while driving,
        // half-width of the grid (km)
        {"rdf_enabled",              "RDF:",                      VType::BOOL,   "1"},
        {"rdf_range_km",             "RDF_RANGE_KM:",             VType::NUMBER, "10"},
        {"decimators",               "DECIMATORS:",               VType::STRING, "0|0.00,7,WBFM,0,15.00,0,2.00"},
        // Decoder plugins the user excluded from the Digital Decoder's "Auto
        // detect" ("id,id"; empty = every plugin takes part)
        {"auto_detect_off",          "AUTO_DETECT_OFF:",          VType::STRING, ""},
        // Independent mode's per-tuner tuning "f/g,f/g,..." (Hz / dB, 0 / -999 =
        // none) and the operating mode (top-bar Mode selector). After FREQ /
        // GAIN and the VFOs (their tuners); TUNERS before the mode, which
        // re-sends it to heimdall when independent mode is entered
        {"independent_tuners",       "TUNERS:",                   VType::STRING, ""},
        {"operating_mode",           "OPERATING_MODE:",           VType::STRING, "coherent"},
        // Station
        {"station_id",               "STATION_ID:",               VType::STRING, "KrakenSDR"},
        {"location_source",          "LOCATION_SOURCE:",          VType::STRING, "gps"},
        {"static_location",          "STATIC_LOCATION:",          VType::STRING, "0,0,0"},
        // Web mapper output (built-in replacement for web_mapper_middleware).
        // Config precedes the enable flag in replay order so a restored
        // "enabled" starts with its key/mode/url already applied.
        {"web_mapper_mode",          "WEB_MAPPER_MODE:",          VType::STRING, "remote"},
        {"web_mapper_key",           "WEB_MAPPER_KEY:",           VType::STRING, ""},
        {"web_mapper_url",           "WEB_MAPPER_URL:",           VType::STRING, "wss://map.krakenrf.com:2096"},
        {"web_mapper_ws_port",       "WEB_MAPPER_WS_PORT:",       VType::NUMBER, "8021"},
        {"web_mapper_enabled",       "WEB_MAPPER:",               VType::BOOL,   "0"},
        // Local DoA recording (config only — recording is never auto-started;
        // the target filename is transient and not persisted)
        {"log_interval_sec",         "LOG_INTERVAL:",             VType::NUMBER, "1"},
        {"log_format",               "LOG_FORMAT:",               VType::STRING, "sqlite"},
        // Spectrum / waterfall display preferences (UI:* keys)
        {"doa_convention",           "UI:DOA_CONV:",              VType::STRING, "compass"},
        {"waterfall_fft_ymax",       "UI:FFT_Y:",                 VType::NUMBER, "60"},
        {"waterfall_speed",          "UI:WF_SPEED:",              VType::NUMBER, "5"},
        {"waterfall_colormap",       "UI:WF_COLORMAP:",           VType::STRING, "sdryoussef"},
        {"waterfall_min_db",         "UI:WF_MIN:",                VType::NUMBER, "-10"},
        {"waterfall_max_db",         "UI:WF_MAX:",                VType::NUMBER, "50"},
        {"waterfall_autorange",      "UI:WF_AUTO:",               VType::BOOL,   "1"},
    };

    map<string, string> g_store;   // JSON key -> value in command-suffix form
    mutex g_mtx;                    // guards g_store
    mutex g_write_mtx;             // serializes file writes
    atomic<bool> g_dirty{false};
    atomic<bool> g_running{false};
    thread g_saver;

    const Setting* find_by_prefix(string_view cmd) {
        for (const Setting& s : SCHEMA)
            if (cmd.starts_with(s.prefix)) return &s;
        return nullptr;
    }

    string bool_suffix(string_view token) {
        return (token == "true" || token == "1") ? "1" : "0";
    }

    bool is_number(const string& s) {
        if (s.empty()) return false;
        char* end = nullptr;
        const double v = strtod(s.c_str(), &end);
        // strtod accepts "inf"/"nan", which would be written out as invalid JSON
        return end && *end == '\0' && is_finite_value(v);
    }

    // Serialize the whole store as pretty JSON (schema order) and write it
    // atomically and durably (write_file_atomic).
    void write_now() {
        lock_guard<mutex> wlk(g_write_mtx);
        string out = "{\n";
        {
            lock_guard<mutex> lk(g_mtx);
            const size_t n = sizeof(SCHEMA) / sizeof(SCHEMA[0]);
            for (size_t i = 0; i < n; i++) {
                const Setting& s = SCHEMA[i];
                auto it = g_store.find(s.key);
                const string& v = (it != g_store.end()) ? it->second : string(s.def);
                out += "  \"";
                out += s.key;
                out += "\": ";
                switch (s.type) {
                    case VType::BOOL:
                        out += (v == "1" || v == "true") ? "true" : "false";
                        break;
                    case VType::NUMBER:
                        out += is_number(v) ? v : "0";
                        break;
                    case VType::STRING:
                        out += "\"" + json_escape(v) + "\"";
                        break;
                }
                out += (i + 1 < n) ? ",\n" : "\n";
            }
        }
        out += "}\n";

        if (!write_file_atomic(SETTINGS_FILE, out))
            cerr << "SettingsStore: could not save " << SETTINGS_FILE << " (" << strerror(errno)
                 << ") - previous settings kept" << endl;
    }
}

namespace SettingsStore {

void load() {
    string json;
    bool existed = false;
    {
        ifstream f(SETTINGS_FILE);
        if (f) {
            existed = true;
            stringstream ss; ss << f.rdbuf();
            json = ss.str();
        }
    }

    bool added_missing = false;
    {
        lock_guard<mutex> lk(g_mtx);
        for (const Setting& s : SCHEMA) {
            string token;
            if (existed && json_find(json, s.key, token)) {
                g_store[s.key] = (s.type == VType::BOOL) ? bool_suffix(token) : token;
            } else {
                g_store[s.key] = s.def;
                added_missing = true;
            }
        }
    }

    // Keep the file complete: create it on first run, or backfill new keys.
    if (!existed || added_missing) write_now();

    cout << "SettingsStore: " << (existed ? "loaded" : "created")
         << " settings from " << SETTINGS_FILE << endl;
}

vector<string> apply_commands() {
    vector<string> out;
    lock_guard<mutex> lk(g_mtx);
    for (const Setting& s : SCHEMA) {
        auto it = g_store.find(s.key);
        out.push_back(string(s.prefix) + (it != g_store.end() ? it->second : string(s.def)));
    }
    return out;
}

void record(string_view command) {
    const Setting* s = find_by_prefix(command);
    if (!s) return;  // not a remembered setting
    string suffix(command.substr(strlen(s->prefix)));
    // A NUMBER setting with a non-numeric value would be written out as 0
    // (write_now) - keep the previous value instead.
    if (s->type == VType::NUMBER && !is_number(suffix)) {
        cerr << "SettingsStore: not saving non-numeric " << s->key << " '" << suffix << "'" << endl;
        return;
    }
    {
        lock_guard<mutex> lk(g_mtx);
        auto it = g_store.find(s->key);
        if (it != g_store.end() && it->second == suffix) return;  // unchanged
        g_store[s->key] = suffix;
    }
    g_dirty.store(true, memory_order_relaxed);
}

bool is_known(string_view command) {
    return find_by_prefix(command) != nullptr;
}

vector<string> reset_to_defaults() {
    {
        lock_guard<mutex> lk(g_mtx);
        for (const Setting& s : SCHEMA) g_store[s.key] = s.def;
    }
    g_dirty.store(false, memory_order_relaxed);
    write_now();  // persist the hardcoded defaults immediately
    return apply_commands();
}

void flush() {
    if (g_dirty.exchange(false, memory_order_acq_rel)) write_now();
}

void start() {
    if (g_running.exchange(true)) return;
    g_saver = thread([]() {
        while (g_running.load(memory_order_relaxed)) {
            // ~1s debounce, polled in 100 ms steps so shutdown is prompt.
            for (int i = 0; i < 10 && g_running.load(memory_order_relaxed); i++)
                this_thread::sleep_for(chrono::milliseconds(100));
            if (g_dirty.exchange(false, memory_order_acq_rel)) write_now();
        }
    });
}

void stop() {
    if (!g_running.exchange(false)) return;
    if (g_saver.joinable()) g_saver.join();
    if (g_dirty.exchange(false, memory_order_acq_rel)) write_now();  // final flush
}

} // namespace SettingsStore
