#include "scanner_manager.hpp"
#include "utils/json_escape.hpp"
#include "globals.hpp"
#include "control_handler.hpp"
#include "networking/websocket_server.hpp"
#include "networking/data_receiver.hpp"
#include "channel_manager.hpp"
#include "message_builders.hpp"
#include "decimator_manager.hpp"
#include "config.hpp"
#include "utils/parse_num.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace std;

ScannerManager::ScannerManager()
    : running_(false),
      last_fft_update_(std::chrono::steady_clock::now()),
      last_dwell_start_(std::chrono::steady_clock::now()),
      signal_lost_time_(std::chrono::steady_clock::now()),
      scan_resume_time_(std::chrono::steady_clock::now())
{
}

ScannerManager::~ScannerManager() {
    stop();
}

// ============================================================================
// JSON PARSING HELPERS
// ============================================================================

// Minimal JSON helpers for scanner config files. String contents are always
// skipped with their escapes honored, so a label containing quotes, ']' or
// '}' (e.g. "Tower [N]") no longer truncates the frequency list or the label.

// Position just past the closing quote of the string starting at `q` (a '"').
// Scanner frequencies are float MHz; Hz goes out as uint64 (the Wideband
// variant reaches 6668 MHz - a uint32 cast wrapped anything past 4294 MHz).
static uint64_t mhz_to_hz(float mhz) {
    return static_cast<uint64_t>(llround(static_cast<double>(mhz) * 1e6));
}

// RF the array can tune to: the R820T range, or the Wideband variant's span
static bool scanner_rf_valid(uint64_t hz) {
    uint64_t min_hz = RTL_TUNER_MIN_HZ, max_hz = RTL_TUNER_MAX_HZ;
    if (wb_variant_enabled.load(std::memory_order_relaxed)) wb_variant_rf_union_range(min_hz, max_hz);
    return hz >= min_hz && hz <= max_hz;
}

static size_t skipJsonString(const string& s, size_t q) {
    for (size_t i = q + 1; i < s.size(); i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '"') return i + 1;
    }
    return string::npos;
}

// Index of the ']' / '}' closing the bracket at `open`, or npos.
static size_t findJsonClose(const string& s, size_t open) {
    int depth = 0;
    for (size_t i = open; i < s.size(); i++) {
        const char c = s[i];
        if (c == '"') {
            i = skipJsonString(s, i);
            if (i == string::npos) return string::npos;
            i--;  // loop increment
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            if (--depth == 0) return i;
        }
    }
    return string::npos;
}

// Start of the value for "key" (whitespace around ':' allowed), or npos.
static size_t findJsonValue(const string& json, const string& key) {
    const string needle = "\"" + key + "\"";
    for (size_t p = json.find(needle); p != string::npos; p = json.find(needle, p + 1)) {
        size_t v = json.find_first_not_of(" \t\r\n", p + needle.size());
        if (v == string::npos || json[v] != ':') continue;
        v = json.find_first_not_of(" \t\r\n", v + 1);
        if (v != string::npos) return v;
    }
    return string::npos;
}

static string extractStringValue(const string& json, const string& key) {
    size_t v = findJsonValue(json, key);
    if (v == string::npos || json[v] != '"') return "";
    string out;
    for (size_t i = v + 1; i < json.size(); i++) {
        char c = json[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < json.size()) {
            c = json[++i];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':  // \uXXXX: keep ASCII, drop the rest
                    if (i + 4 < json.size()) {
                        const unsigned long cp = strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
                        if (cp >= 0x20 && cp < 0x7f) out += static_cast<char>(cp);
                        i += 4;
                    }
                    break;
                default: out += c;  // \" \\ \/
            }
            continue;
        }
        out += c;
    }
    return out;  // unterminated: best effort
}

static float extractFloatValue(const string& json, const string& key, float default_val) {
    size_t v = findJsonValue(json, key);
    if (v == string::npos) return default_val;
    size_t end = json.find_first_of(",}] \t\r\n", v);
    try {
        return stof_finite(json.substr(v, end == string::npos ? string::npos : end - v));
    } catch (...) {
        return default_val;
    }
}

static int extractIntValue(const string& json, const string& key, int default_val) {
    size_t v = findJsonValue(json, key);
    if (v == string::npos) return default_val;
    size_t end = json.find_first_of(",}] \t\r\n", v);
    try {
        return stoi(json.substr(v, end == string::npos ? string::npos : end - v));
    } catch (...) {
        return default_val;
    }
}

static bool extractBoolValue(const string& json, const string& key, bool default_val) {
    size_t v = findJsonValue(json, key);
    if (v == string::npos) return default_val;
    return json.compare(v, 4, "true") == 0 || json.compare(v, 1, "1") == 0;
}

// ============================================================================
// CONFIGURATION MANAGEMENT
// ============================================================================

bool ScannerManager::loadConfig(const std::string& json_str) {
    try {
        // Parsed into a copy and committed only when every entry is valid: a
        // refused load keeps the previous (possibly running) configuration
        ScannerConfig cfg;
        cfg.name = extractStringValue(json_str, "name");
        if (cfg.name.empty()) cfg.name = "Scanner";

        cfg.version = extractIntValue(json_str, "version", 1);
        // Default squelch is 20dB above normalized noise floor (0dB)
        cfg.squelch_db = extractFloatValue(json_str, "squelch_db", 20.0f);
        cfg.dwell_time_ms = extractIntValue(json_str, "dwell_time_ms", 200);
        cfg.hysteresis_db = extractFloatValue(json_str, "hysteresis_db", 5.0f);
        cfg.lock_timeout_ms = extractIntValue(json_str, "lock_timeout_ms", 200);

        // Parse frequencies array
        cfg.frequencies.clear();
        size_t freq_array_start = findJsonValue(json_str, "frequencies");
        if (freq_array_start != string::npos && json_str[freq_array_start] == '[') {
            size_t freq_array_end = findJsonClose(json_str, freq_array_start);
            if (freq_array_end != string::npos) {
                string freq_array = json_str.substr(freq_array_start + 1, freq_array_end - freq_array_start - 1);

                // Parse each frequency object (brackets inside labels skipped)
                size_t pos = 0;
                while ((pos = freq_array.find_first_of("{\"", pos)) != string::npos) {
                    if (freq_array[pos] == '"') {  // a stray string between objects
                        pos = skipJsonString(freq_array, pos);
                        if (pos == string::npos) break;
                        continue;
                    }
                    size_t obj_end = findJsonClose(freq_array, pos);
                    if (obj_end == string::npos) break;

                    string freq_obj = freq_array.substr(pos, obj_end - pos + 1);

                    ScannerFrequency freq;
                    freq.freq_mhz = extractFloatValue(freq_obj, "freq_mhz", 100.0f);
                    freq.bandwidth_khz = extractFloatValue(freq_obj, "bandwidth_khz", 12.5f);
                    freq.label = extractStringValue(freq_obj, "label");
                    freq.enabled = extractBoolValue(freq_obj, "enabled", true);

                    // The UI's add prompt and file loads only checked > 0:
                    // refuse an entry the array can't tune to (or a bandwidth
                    // wider than the stream) rather than send it to heimdall
                    if (!std::isfinite(freq.freq_mhz) || !scanner_rf_valid(mhz_to_hz(freq.freq_mhz)) ||
                        !std::isfinite(freq.bandwidth_khz) || freq.bandwidth_khz <= 0.0f ||
                        freq.bandwidth_khz > SAMPLE_RATE / 1e3f) {
                        cerr << "Scanner config: entry '" << freq.label << "' (" << freq.freq_mhz
                             << " MHz, " << freq.bandwidth_khz << " kHz) out of range - config refused" << endl;
                        return false;
                    }

                    cfg.frequencies.push_back(freq);
                    pos = obj_end + 1;
                }
            }
        }

        lock_guard<mutex> lock(config_mutex_);
        config_ = std::move(cfg);
        status_.config_loaded = true;
        cout << "Scanner config loaded: " << config_.name << " with "
             << config_.frequencies.size() << " frequencies" << endl;

        return true;
    } catch (const exception& e) {
        cerr << "Failed to load scanner config: " << e.what() << endl;
        return false;
    }
}

string ScannerManager::getConfigJson() const {
    lock_guard<mutex> lock(config_mutex_);

    stringstream json;
    json << "{\"name\":\"" << json_escape(config_.name) << "\","
         << "\"version\":" << config_.version << ","
         << "\"squelch_db\":" << config_.squelch_db << ","
         << "\"dwell_time_ms\":" << config_.dwell_time_ms << ","
         << "\"hysteresis_db\":" << config_.hysteresis_db << ","
         << "\"lock_timeout_ms\":" << config_.lock_timeout_ms << ","
         << "\"frequencies\":[";

    for (size_t i = 0; i < config_.frequencies.size(); i++) {
        const auto& freq = config_.frequencies[i];
        if (i > 0) json << ",";
        json << "{\"freq_mhz\":" << freq.freq_mhz << ","
             << "\"bandwidth_khz\":" << freq.bandwidth_khz << ","
             << "\"label\":\"" << json_escape(freq.label) << "\","
             << "\"enabled\":" << (freq.enabled ? "true" : "false") << "}";
    }

    json << "]}";
    return json.str();
}

bool ScannerManager::setSquelch(float squelch_db) {
    lock_guard<mutex> lock(config_mutex_);
    config_.squelch_db = squelch_db;
    return true;
}

bool ScannerManager::setDwellTime(int dwell_ms) {
    {
        lock_guard<mutex> lock(config_mutex_);
        config_.dwell_time_ms = dwell_ms;
    }

    // If scanner is running, update the server-side dwell time immediately
    if (running_.load()) {
        // Build frequency list in Hz (need to reconfigure with new dwell time)
        vector<uint64_t> freq_list_hz;
        for (const auto& group : groups_) {
            freq_list_hz.push_back(mhz_to_hz(group.center_freq_mhz));
        }

        // Send updated configuration to Heimdall
        stringstream config_json;
        config_json << "{\"configure_scanner\":true,\"frequency_groups\":[";
        for (size_t i = 0; i < freq_list_hz.size(); i++) {
            if (i > 0) config_json << ",";
            config_json << freq_list_hz[i];
        }
        config_json << "],\"dwell_time_ms\":" << dwell_ms << "}";

        cout << "Updating server-side scanner dwell time: " << dwell_ms << " ms" << endl;
        ControlHandler::send_control_command(config_json.str());
    }

    return true;
}

ScannerConfig ScannerManager::getConfig() const {
    lock_guard<mutex> lock(config_mutex_);
    return config_;
}

// ============================================================================
// FREQUENCY GROUPING - CALCULATE OPTIMAL CENTER FREQUENCY
// ============================================================================

void ScannerManager::buildFrequencyGroups() {
    groups_.clear();

    // Get all enabled frequencies with their indices
    vector<pair<float, size_t>> enabled_freqs; // (freq_mhz, original_index)
    for (size_t i = 0; i < config_.frequencies.size(); i++) {
        if (config_.frequencies[i].enabled) {
            enabled_freqs.push_back({config_.frequencies[i].freq_mhz, i});
        }
    }

    if (enabled_freqs.empty()) {
        cout << "No enabled frequencies" << endl;
        return;
    }

    // Sort by frequency
    sort(enabled_freqs.begin(), enabled_freqs.end());

    // Greedy grouping algorithm: minimize number of groups
    // Each group can cover up to 9.6 MHz (wideband array span)
    const float MAX_SPAN = 9.6f;

    size_t i = 0;
    while (i < enabled_freqs.size()) {
        FrequencyGroup group;
        float min_freq = enabled_freqs[i].first;
        float max_freq = min_freq;

        // Add first frequency to group
        group.freq_indices.push_back(enabled_freqs[i].second);
        i++;

        // Try to add more frequencies while staying within MAX_SPAN
        while (i < enabled_freqs.size()) {
            float next_freq = enabled_freqs[i].first;
            float potential_span = next_freq - min_freq;

            if (potential_span <= MAX_SPAN) {
                // Fits in current group
                group.freq_indices.push_back(enabled_freqs[i].second);
                max_freq = next_freq;
                i++;
            } else {
                // Doesn't fit, start new group
                break;
            }
        }

        // Calculate center frequency for this group (midpoint)
        group.center_freq_mhz = (min_freq + max_freq) / 2.0f;
        group.span_mhz = max_freq - min_freq;

        groups_.push_back(group);

        cout << "Group " << groups_.size() << ": Center=" << group.center_freq_mhz
             << " MHz, Span=" << group.span_mhz << " MHz, "
             << group.freq_indices.size() << " frequencies" << endl;
    }

    cout << "Scanner: Built " << groups_.size() << " frequency groups" << endl;
}

// ============================================================================
// SCANNER CONTROL
// ============================================================================

bool ScannerManager::start() {
    if (running_.load()) {
        cerr << "Scanner already running" << endl;
        return false;
    }

    if (!status_.config_loaded.load()) {
        cerr << "No scanner config loaded" << endl;
        return false;
    }

    // Build frequency groups (calculates optimal centers)
    buildFrequencyGroups();

    if (groups_.empty()) {
        cerr << "No enabled frequencies to scan" << endl;
        return false;
    }

    // Must be in wideband mode
    if (!wideband_mode_enabled.load()) {
        cerr << "Discrete scanner requires wideband mode to be enabled first" << endl;
        return false;
    }

    // ========================================================================
    // SERVER-SIDE SCANNER: Send configuration to Heimdall
    // The server will handle all frequency switching internally
    // Client just detects frequency changes from packet headers and resets FFT
    // ========================================================================

    // Build frequency list in Hz
    vector<uint64_t> freq_list_hz;
    for (const auto& group : groups_) {
        freq_list_hz.push_back(mhz_to_hz(group.center_freq_mhz));
    }

    // Send configuration to Heimdall
    stringstream config_json;
    config_json << "{\"configure_scanner\":true,\"frequency_groups\":[";
    for (size_t i = 0; i < freq_list_hz.size(); i++) {
        if (i > 0) config_json << ",";
        config_json << freq_list_hz[i];
    }
    config_json << "],\"dwell_time_ms\":" << config_.dwell_time_ms << "}";

    cout << "Configuring server-side scanner: " << groups_.size() << " groups, "
         << config_.dwell_time_ms << " ms dwell" << endl;
    ControlHandler::send_control_command(config_json.str());

    // Small delay to let configuration arrive
    this_thread::sleep_for(chrono::milliseconds(50));

    // Start server-side scanner
    stringstream start_json;
    start_json << "{\"start_scanner\":true}";
    ControlHandler::send_control_command(start_json.str());

    running_ = true;
    status_.state = ScannerState::SCANNING;
    status_.current_group = 0;

    // Record scan start time for initial settling period
    scan_resume_time_ = chrono::steady_clock::now();

    // Pause FM demodulator during scanning (save state to restore later)
    fm_enabled_before_scanner = fm_enabled.load();
    if (fm_enabled.load()) {
        fm_enabled = false;
        cout << "FM demodulator paused during scanning" << endl;
    }

    cout << "Server-side discrete scanner started (NO local thread, NO delays!)" << endl;
    cout << "FFT reset triggered automatically by packet header frequency_change_counter" << endl;

    // Notify UI
    stringstream ui_json;
    ui_json << "{\"scanner_started\":true}";
    WebSocketServer::broadcast_json_message(ui_json.str());

    return true;
}

bool ScannerManager::stop() {
    if (!running_.load()) {
        return false;
    }

    // Stop the lock/resume worker FIRST: it checks running_ before every
    // server command (see still_running) and exits within one of its short
    // sleeps. Stopping the server scanner while a resume was mid-sequence
    // let it send configure/start_scanner afterwards - heimdall kept hopping
    // while the client reported the scanner stopped.
    running_ = false;
    for (int i = 0; i < 100 && transition_in_progress_.load(std::memory_order_acquire); i++) {
        this_thread::sleep_for(chrono::milliseconds(10));
    }

    // Send stop command to server
    stringstream stop_json;
    stop_json << "{\"stop_scanner\":true}";
    ControlHandler::send_control_command(stop_json.str());

    status_.state = ScannerState::IDLE;

    // Restore FM demodulator state if it was enabled before scanning
    if (fm_enabled_before_scanner.load()) {
        fm_enabled = true;
        cout << "FM demodulator restored after scanning" << endl;
    }

    cout << "Server-side discrete scanner stopped" << endl;

    // Notify UI
    stringstream json;
    json << "{\"scanner_stopped\":true}";
    WebSocketServer::broadcast_json_message(json.str());

    return true;
}

bool ScannerManager::next() {
    // Force continue scanning (skip current locked signal)
    if (running_.load() && status_.state == ScannerState::LOCKED) {
        cout << "User requested continue - skipping current signal" << endl;
        startResumeTransition();
        return true;
    }
    return false;
}

bool ScannerManager::isRunning() const {
    return running_.load();
}

// ============================================================================
// FFT MONITORING AND SIGNAL DETECTION
// ============================================================================

void ScannerManager::updateFFTData(const std::vector<float>& magnitudes, const std::vector<float>& frequencies) {
    lock_guard<mutex> lock(fft_mutex_);
    cached_magnitudes_ = magnitudes;
    cached_frequencies_ = frequencies;
    last_fft_update_ = chrono::steady_clock::now();

    // A lock/resume transition worker is mid-sequence: the state it is about
    // to change isn't settled yet, so don't act on this frame
    if (transition_in_progress_.load(std::memory_order_acquire)) {
        return;
    }

    // If scanner is running and in SCANNING state, check for signals
    if (running_.load() && status_.state == ScannerState::SCANNING) {
        // SETTLING PERIOD after resuming scan: Wait for fresh FFT data before detecting signals
        // This prevents false detection of "ghost" signals from decaying averaged FFT data
        auto now = chrono::steady_clock::now();
        auto time_since_resume = chrono::duration_cast<chrono::milliseconds>(now - scan_resume_time_);
        const int SCAN_RESUME_SETTLING_MS = 1500;  // 1.5 seconds settling after resume

        if (time_since_resume.count() < SCAN_RESUME_SETTLING_MS) {
            // Still in settling period - don't detect signals yet
            return;
        }

        // Check each enabled frequency in the configuration
        for (size_t i = 0; i < config_.frequencies.size(); i++) {
            if (config_.frequencies[i].enabled) {
                float signal_db = 0.0f;
                if (checkSignalPresent(i, magnitudes, frequencies, signal_db)) {
                    // Signal detected! Lock onto it (async - the sequence
                    // sleeps between server commands)
                    startLockTransition(i, signal_db);
                    break; // Only lock onto first detected signal
                }
            }
        }
    }
    // If locked, monitor signal strength and check for timeout
    else if (running_.load() && status_.state == ScannerState::LOCKED) {
        size_t locked_idx = status_.locked_freq_index.load();
        float signal_db = 0.0f;

        // SETTLING PERIOD: Don't monitor for signal loss until mode has fully switched
        // The system needs time to:
        // 1. Switch from wideband to coherent mode
        // 2. Retune all tuners to the locked frequency
        // 3. Get fresh FFT data at the new frequency
        // 4. Wait for FFT averaging to stabilize with signal data
        auto now = chrono::steady_clock::now();
        auto time_since_lock = chrono::duration_cast<chrono::milliseconds>(now - signal_lock_time_);
        const int LOCK_SETTLING_MS = 3000;  // 3 seconds settling after lock (needs time for coherent FFT to stabilize)

        if (time_since_lock.count() < LOCK_SETTLING_MS) {
            // Still settling - don't monitor for signal loss yet
            signal_lost_time_ = now;  // Keep resetting the lost timer during settling
            return;
        }

        // In LOCKED/coherent mode, the signal is at the CENTER of the FFT
        // (decimator offset is set to 0 when locking, so signal is centered)
        // We check the peak signal in the center portion of the spectrum
        if (locked_idx < config_.frequencies.size() && !magnitudes.empty()) {
            // Find peak signal in center 20% of spectrum (where the signal should be)
            size_t center = magnitudes.size() / 2;
            size_t search_radius = magnitudes.size() / 10; // 10% on each side = 20% total
            size_t start = (center > search_radius) ? (center - search_radius) : 0;
            size_t end = min(center + search_radius, magnitudes.size());

            float peak_db = -200.0f;
            for (size_t i = start; i < end; i++) {
                if (magnitudes[i] > peak_db) {
                    peak_db = magnitudes[i];
                }
            }
            signal_db = peak_db;

            // Check signal presence with LOSS hysteresis (lower threshold than detection)
            // Detection uses: squelch + hysteresis (e.g., -75 dB)
            // Loss uses: squelch - hysteresis (e.g., -85 dB)
            // This creates a hysteresis window preventing oscillation
            float loss_threshold_db = config_.squelch_db - config_.hysteresis_db;
            bool signal_present = (signal_db > loss_threshold_db);

            // Log first frame after settling ends
            static bool first_after_settling = true;
            if (first_after_settling) {
                cout << "Settling complete - starting signal monitoring (squelch=" << config_.squelch_db
                     << " dB, loss_threshold=" << loss_threshold_db << " dB)" << endl;
                first_after_settling = false;
            }

            if (signal_present) {
                // Signal still present, update strength
                status_.locked_signal_db = signal_db;
                signal_lost_time_ = chrono::steady_clock::now(); // Reset lost timer
            } else {
                // Signal lost - check if we've been without signal for long enough
                auto now = chrono::steady_clock::now();
                auto lost_duration = chrono::duration_cast<chrono::milliseconds>(now - signal_lost_time_);

                if (lost_duration.count() >= config_.lock_timeout_ms) {
                    cout << "Signal lost for " << lost_duration.count() << " ms (below "
                         << loss_threshold_db << " dB) - resuming scan" << endl;
                    first_after_settling = true;  // Reset for next lock
                    startResumeTransition();
                }
            }
        }

        // Also check for absolute lock timeout (max time to stay locked)
        // Note: 'now' is already declared above in settling period check
        auto lock_duration = chrono::duration_cast<chrono::seconds>(now - signal_lock_time_);
        const int MAX_LOCK_TIME_SECONDS = 60; // Maximum 60 seconds locked on one frequency

        if (lock_duration.count() >= MAX_LOCK_TIME_SECONDS) {
            cout << "Maximum lock time (" << MAX_LOCK_TIME_SECONDS << "s) exceeded - resuming scan" << endl;
            startResumeTransition();
        }
    }
}

void ScannerManager::updatePhaseCalibrationStatus(uint32_t phase_state, uint32_t noise_source_active) {
    phase_compensation_state_ = phase_state;
    noise_source_active_ = noise_source_active;

    if (noise_source_active) {
        // Per-packet timestamp so pollers (continuous scanner, 100ms loop)
        // can detect short noise bursts that fall between their polls
        last_noise_active_ns_.store(
            chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_release);
    }

    // If locked and waiting for phase calibration to complete
    if (status_.state == ScannerState::LOCKED) {
        // Check if phase compensation has converged (state == 4 = CONVERGED)
        if (phase_state == 4) {
            cout << "Phase calibration converged while locked on signal" << endl;
        }
    }
}

bool ScannerManager::checkSignalPresent(size_t freq_index, const std::vector<float>& magnitudes,
                                        const std::vector<float>& frequencies, float& signal_db) {
    if (freq_index >= config_.frequencies.size()) return false;
    if (frequencies.empty() || magnitudes.empty()) return false;

    float target_freq_mhz = config_.frequencies[freq_index].freq_mhz;
    float bandwidth_khz = config_.frequencies[freq_index].bandwidth_khz;
    float squelch_db = config_.squelch_db;

    // Convert bandwidth to MHz (half on each side of center)
    float half_bandwidth_mhz = (bandwidth_khz / 1000.0f) / 2.0f;
    float freq_min_mhz = target_freq_mhz - half_bandwidth_mhz;
    float freq_max_mhz = target_freq_mhz + half_bandwidth_mhz;

    // Find peak signal within the specified bandwidth
    float peak_db = -200.0f;
    bool found_bin_in_range = false;

    for (size_t i = 0; i < frequencies.size(); i++) {
        if (frequencies[i] >= freq_min_mhz && frequencies[i] <= freq_max_mhz) {
            found_bin_in_range = true;
            if (magnitudes[i] > peak_db) {
                peak_db = magnitudes[i];
            }
        }
    }

    // If no bins found in range, the target frequency is outside current FFT coverage
    if (!found_bin_in_range) {
        return false;
    }

    signal_db = peak_db;

    // Check if above squelch with hysteresis
    float threshold_db = squelch_db + config_.hysteresis_db;

    return (signal_db > threshold_db);
}

void ScannerManager::startLockTransition(size_t freq_index, float signal_db) {
    bool expected = false;
    if (!transition_in_progress_.compare_exchange_strong(expected, true)) return;
    thread([this, freq_index, signal_db]() {
        lockOnSignal(freq_index, signal_db);
        transition_in_progress_.store(false, std::memory_order_release);
    }).detach();
}

void ScannerManager::startResumeTransition() {
    bool expected = false;
    if (!transition_in_progress_.compare_exchange_strong(expected, true)) return;
    thread([this]() {
        resumeScanning();
        transition_in_progress_.store(false, std::memory_order_release);
    }).detach();
}

void ScannerManager::lockOnSignal(size_t freq_index, float signal_db) {
    // Runs on a transition worker thread (see startLockTransition), so copy
    // the frequency entry under config_mutex_ - a concurrent SCANNER_CONFIG
    // on the uWS thread may reallocate the vector.
    ScannerFrequency freq;
    {
        lock_guard<mutex> lock(config_mutex_);
        if (freq_index >= config_.frequencies.size()) return;
        freq = config_.frequencies[freq_index];
    }

    cout << "SIGNAL DETECTED: " << freq.label << " at " << freq.freq_mhz
         << " MHz (" << signal_db << " dB)" << endl;

    if (!still_running()) return;
    // Stop server-side scanner
    stringstream stop_json;
    stop_json << "{\"stop_scanner\":true}";
    ControlHandler::send_control_command(stop_json.str());

    // Small delay to let server process stop command
    this_thread::sleep_for(chrono::milliseconds(50));
    if (!still_running()) return;

    // Switch to coherent mode (disable wideband)
    // CRITICAL: Update CLIENT-side flag immediately so message_builders switches to per-channel FFT.
    // Through the shared helper: it also brings back the DoA that wideband
    // mode parked - the lock exists to take bearings, and DoA used to stay off.
    ControlHandler::apply_wideband_mode_state(false);

    // Send command to server
    stringstream coherent_json;
    coherent_json << "{\"set_wideband_mode\":{\"enable\":false}}";
    ControlHandler::send_control_command(coherent_json.str());

    // Notify UI of wideband mode state change
    stringstream wb_update;
    wb_update << "{\"wideband_mode\":{\"enabled\":false}}";
    WebSocketServer::broadcast_json_message(wb_update.str());

    this_thread::sleep_for(chrono::milliseconds(100));
    if (!still_running()) return;

    // Tune all tuners to the exact detected frequency
    const uint64_t freq_hz = mhz_to_hz(freq.freq_mhz);
    stringstream freq_json;
    freq_json << "{\"set_frequency\":{\"frequency\":" << freq_hz << "}}";
    ControlHandler::send_control_command(freq_json.str());

    // IMMEDIATELY update client-side state so message_builders has correct frequency
    // when it switches to per-channel FFT (now that wideband_mode_enabled = false)
    ChannelManager::set_frequency(freq_hz, -1);  // -1 = all channels
    cout << "Client tuning frequency set to " << freq.freq_mhz << " MHz" << endl;

    // CRITICAL: Set offset tuning bar to 0 since signal is now centered in coherent mode
    // In wideband mode, decimators had offsets to select different parts of the spectrum
    // In coherent mode, all tuners are on the same frequency, so offset should be 0
    // Through DecimatorManager, not the raw decimator: the manager's
    // frequency_offset_hz is what the pipeline passes to the decimator and
    // what MUSIC's wavelength, the DoA logger and the web mapper use - setting
    // only the decimator left the old wideband offset in effect.
    auto all_decimators = decimator_manager.getAllDecimators();
    for (const auto& dec : all_decimators) {
        if (dec && !dec->being_deleted.load(std::memory_order_relaxed)) {
            decimator_manager.setFrequencyOffset(dec->id, 0.0f);
        }
    }
    cout << "Offset tuning bar centered (all decimator offsets set to 0)" << endl;

    // Find and apply the bandwidth setting from the frequency list
    // MUST DO THIS BEFORE sending decimator_info to browser!
    // Convert bandwidth from kHz to MHz for comparison
    float target_bandwidth_mhz = freq.bandwidth_khz / 1000.0f;

    // Find closest bandwidth option
    int best_bandwidth_index = DEFAULT_BANDWIDTH_INDEX;
    float min_bw_diff = 1e9f;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        float bw_diff = abs(BANDWIDTH_OPTIONS[i].bandwidth_mhz - target_bandwidth_mhz);
        if (bw_diff < min_bw_diff) {
            min_bw_diff = bw_diff;
            best_bandwidth_index = i;
        }
    }

    // Apply bandwidth setting to all decimators
    for (const auto& dec : all_decimators) {
        if (dec && !dec->being_deleted.load(std::memory_order_relaxed)) {
            decimator_manager.setBandwidthIndex(dec->id, best_bandwidth_index);
        }
    }
    const auto& selected_bw = BANDWIDTH_OPTIONS[best_bandwidth_index];
    cout << "Bandwidth set to " << selected_bw.display_name
         << " (target: " << freq.bandwidth_khz << " kHz)" << endl;

    // NOW send updated decimator info to browser with correct offset AND bandwidth
    WebSocketServer::broadcast_json_message(MessageBuilders::build_decimator_info_message());
    cout << "Decimator info broadcast to browser (offset=0, bandwidth=" << best_bandwidth_index << ")" << endl;

    // Give server time to switch mode, tune, and send first coherent FFT packet
    // This ensures the display will show the centered spectrum when we declare lock complete
    this_thread::sleep_for(chrono::milliseconds(300));
    if (!still_running()) return;

    // CRITICAL: Reset FFT averaging to clear stale wideband data
    // The per-channel FFT buffers have data from wideband mode (different frequencies)
    // Without this reset, the signal monitoring would see incorrect/stale data
    uint32_t old_gen = fft_reset_generation.fetch_add(1, std::memory_order_release);
    cout << "FFT reset triggered (gen " << old_gen << " → " << (old_gen + 1) << ") for coherent mode transition" << endl;

    // Also reset the signal_lost_time_ so the 200ms timeout starts fresh after settling
    signal_lost_time_ = chrono::steady_clock::now();

    // Enable bias tee (noise source) for phase calibration
    // Note: Heimdall doesn't have a command for this yet, it's enabled by default
    // Phase calibration will start automatically in coherent mode

    // Update scanner state (re-checked: a stop() since the last check has
    // already set IDLE, which a late LOCKED would overwrite - stale state)
    if (!still_running()) return;
    status_.state = ScannerState::LOCKED;
    status_.locked_freq_index = freq_index;
    status_.locked_signal_db = signal_db;
    signal_lock_time_ = chrono::steady_clock::now();

    cout << "Locked onto " << freq.label << " - Coherent mode with phase calibration" << endl;
    cout << "   (Lag calibration will NOT be performed)" << endl;

    // Notify UI with scanner_signal_locked (format expected by kraken_doa.html)
    stringstream ui_json;
    ui_json << "{\"scanner_signal_locked\":{\"freq_mhz\":" << freq.freq_mhz
            << ",\"signal_db\":" << signal_db
            << ",\"label\":\"" << json_escape(freq.label) << "\"}}";
    WebSocketServer::broadcast_json_message(ui_json.str());
}

void ScannerManager::resumeScanning() {
    if (!still_running() || status_.state != ScannerState::LOCKED) return;

    // Runs on a transition worker thread (see startResumeTransition); copy
    // shared config under config_mutex_ before the multi-step sequence.
    vector<FrequencyGroup> groups_snapshot;
    int dwell_time_ms_snapshot;
    {
        lock_guard<mutex> lock(config_mutex_);
        groups_snapshot = groups_;
        dwell_time_ms_snapshot = config_.dwell_time_ms;
    }
    if (groups_snapshot.empty()) return;

    cout << "Resuming scanning mode" << endl;

    // CRITICAL: Reset FFT averaging immediately to prevent ghost signal detection
    // The old averaged FFT data still shows the signal that just disappeared
    // Without this reset, the scanner would immediately re-lock on the decaying ghost
    uint32_t old_gen = fft_reset_generation.fetch_add(1, std::memory_order_release);
    cout << "FFT reset triggered (gen " << old_gen << " → " << (old_gen + 1) << ") to clear ghost signals" << endl;

    // Also clear wideband FFT buffers directly
    {
        lock_guard<mutex> wb_lock(wideband_fft_mutex);
        fill(wideband_fft_magnitudes.begin(), wideband_fft_magnitudes.end(), -100.0f);
        fill(wideband_fft_averaged.begin(), wideband_fft_averaged.end(), -100.0f);
    }

    // CRITICAL: We're in coherent mode (wideband=false on server).
    // Need to re-enable wideband mode which will reset phase calibration.
    // The server's set_wideband_mode(true) resets phase_compensation->state to WAITING_FOR_LAG_COMPLETION
    // and disables phase calibration eigenvalue calculations.

    // Update client-side flag BEFORE sending command to server
    // This ensures message_builders switches to wideband FFT mode
    ControlHandler::apply_wideband_mode_state(true);  // parks DoA again for the scan

    // Enable wideband mode on server - this will:
    // 1. Set operating_mode to WIDEBAND_SCAN
    // 2. Reset phase calibration state to WAITING_FOR_LAG_COMPLETION
    // 3. Disable FFT processing (server-side)
    // 4. Turn off bias tee if calibration was interrupted
    stringstream wideband_json;
    wideband_json << "{\"set_wideband_mode\":{\"enable\":true,\"base_frequency\":"
                  << mhz_to_hz(groups_snapshot[0].center_freq_mhz) << "}}";
    cout << "Sending wideband enable command to server" << endl;
    ControlHandler::send_control_command(wideband_json.str());

    // Notify UI of wideband mode state change
    stringstream wb_update;
    wb_update << "{\"wideband_mode\":{\"enabled\":true}}";
    WebSocketServer::broadcast_json_message(wb_update.str());

    // Wait for server to process wideband mode change and reset phase calibration
    this_thread::sleep_for(chrono::milliseconds(200));
    if (!still_running()) return;

    // Re-send scanner configuration to ensure server has correct frequency list
    // This is important because server state may have been corrupted during lock/unlock
    vector<uint64_t> freq_list_hz;
    for (const auto& group : groups_snapshot) {
        freq_list_hz.push_back(mhz_to_hz(group.center_freq_mhz));
    }

    stringstream config_json;
    config_json << "{\"configure_scanner\":true,\"frequency_groups\":[";
    for (size_t i = 0; i < freq_list_hz.size(); i++) {
        if (i > 0) config_json << ",";
        config_json << freq_list_hz[i];
    }
    config_json << "],\"dwell_time_ms\":" << dwell_time_ms_snapshot << "}";

    cout << "Re-sending scanner configuration: " << groups_snapshot.size() << " groups" << endl;
    ControlHandler::send_control_command(config_json.str());

    this_thread::sleep_for(chrono::milliseconds(100));
    if (!still_running()) return;

    // Restart server-side scanner
    stringstream start_json;
    start_json << "{\"start_scanner\":true}";
    cout << "Sending start_scanner command to server" << endl;
    ControlHandler::send_control_command(start_json.str());

    // Update state - use SETTLING state to prevent immediate signal detection
    // This gives time for the server to switch frequencies and new FFT data to arrive
    if (!still_running()) return;  // stopped meanwhile: keep stop()'s IDLE
    status_.state = ScannerState::SCANNING;
    status_.locked_freq_index = 0;
    status_.locked_signal_db = -999.0f;

    // Record when scanning resumed - used for settling period to prevent ghost signal detection
    scan_resume_time_ = chrono::steady_clock::now();

    // Notify UI with scanner_signal_lost (format expected by kraken_doa.html)
    stringstream ui_json;
    ui_json << "{\"scanner_signal_lost\":true}";
    WebSocketServer::broadcast_json_message(ui_json.str());
}
