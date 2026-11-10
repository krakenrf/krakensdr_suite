#include "wb_board.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t MAX_PENDING = 256 * 1024;               // a line longer than this is garbage
constexpr auto LOG_PERIOD = std::chrono::seconds(10);
constexpr auto STALE_AFTER = std::chrono::seconds(30);
constexpr uintmax_t ROTATE_BYTES = 5 * 1024 * 1024;
constexpr int KEEP_OLD = 3;

std::mutex mtx;
std::string pending;                 // bytes after the last newline
std::string latest;                  // the last "downconverter" object
Clock::time_point latest_at{};
Clock::time_point last_log{};
bool lost_logged = false;
std::map<std::string, std::string> last_state;   // event keys -> last value

// The balanced {...} after `"key":` in s, or "" (heimdall's JSON has no
// braces inside strings in this object)
std::string object_after(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\":{";
    const size_t at = s.find(k);
    if (at == std::string::npos) return {};
    const size_t start = at + k.size() - 1;
    int depth = 0;
    for (size_t i = start; i < s.size(); i++) {
        if (s[i] == '{') depth++;
        else if (s[i] == '}' && --depth == 0) return s.substr(start, i - start + 1);
    }
    return {};
}

bool num_of(const std::string& o, const char* key, double& v) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = o.find(k);
    if (at == std::string::npos) return false;
    const char* p = o.c_str() + at + k.size();
    char* end = nullptr;
    v = std::strtod(p, &end);
    return end != p;
}

std::string str_of(const std::string& o, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t at = o.find(k);
    if (at == std::string::npos) return {};
    size_t p = at + k.size();
    if (p < o.size() && o[p] == '"') {
        const size_t e = o.find('"', p + 1);
        return e == std::string::npos ? std::string() : o.substr(p + 1, e - p - 1);
    }
    size_t e = p;
    while (e < o.size() && o[e] != ',' && o[e] != '}') e++;
    return o.substr(p, e - p);   // true / false / a number
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::filesystem::path log_dir() {
    std::error_code ec;
    std::filesystem::path d = std::filesystem::current_path(ec) / "wb_logs";
    std::filesystem::create_directories(d, ec);
    return d;
}

void rotate_if_big(const std::filesystem::path& f) {
    std::error_code ec;
    if (!std::filesystem::exists(f, ec) || std::filesystem::file_size(f, ec) < ROTATE_BYTES) return;
    for (int i = KEEP_OLD; i >= 1; i--) {
        std::filesystem::path from = i == 1 ? f : std::filesystem::path(f.string() + "." + std::to_string(i - 1));
        std::filesystem::path to(f.string() + "." + std::to_string(i));
        std::filesystem::remove(to, ec);          // Windows' rename does not overwrite
        std::filesystem::rename(from, to, ec);
    }
}

void append_line(const char* name, const char* header, const std::string& line) {
    const std::filesystem::path f = log_dir() / name;
    rotate_if_big(f);
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(f, ec);
    std::ofstream out(f, std::ios::app | std::ios::binary);
    if (!out) return;
    if (fresh) out << header << "\n";
    out << line << "\n";
}

void event(const std::string& what, const std::string& detail) {
    std::cout << "WB board: " << what << " " << detail << std::endl;
    append_line("wb_events.csv", "time_utc,event,detail", utc_now() + "," + what + ",\"" + detail + "\"");
}

// Log alarm / state changes: key -> value; the first sighting is logged too
void track(const std::string& key, const std::string& value, const std::string& detail) {
    auto it = last_state.find(key);
    if (it != last_state.end() && it->second == value) return;
    last_state[key] = value;
    event(key, value + (detail.empty() ? "" : " (" + detail + ")"));
}

void log_object(const std::string& dc) {
    const std::string fw = object_after(dc, "fw");
    const std::string tel = object_after(dc, "telemetry");
    const std::string alarms = object_after(tel, "alarms");

    // events
    if (!fw.empty()) {
        const std::string ver = str_of(fw, "date") + "." + str_of(fw, "build");
        track("firmware", ver, str_of(fw, "telemetry_ok") == "true" ? "" : "telemetry disabled: firmware too old");
        track("tune_command", str_of(fw, "set_cmd"), str_of(fw, "set_cmd") == "w" ? "Flash save per retune!" : "");
    }
    if (str_of(tel, "enabled") == "true") {
        track("board", str_of(tel, "revision"), "sensor " + str_of(tel, "sensor") + ", R6 " + str_of(tel, "r_shunt_mohm") +
                                                    " mOhm" + (str_of(tel, "shunt_consistent") == "false" ? ", shunt voltage disagrees" : ""));
        double v = 0, i = 0, t = 0, rh = 0;
        const bool has_p = num_of(tel, "vin_v", v) && num_of(tel, "iin_a", i);
        const bool has_e = num_of(tel, "temp_c", t) && num_of(tel, "rh_pct", rh);
        char d[96];
        std::snprintf(d, sizeof(d), "%.2f V %.2f A %.1f C %.0f %%RH", v, i, t, rh);
        for (const char* a : {"iin", "vin", "temp", "rh", "sensor"})
            track(std::string("alarm_") + a, str_of(alarms, a), d);

        if (Clock::now() - last_log >= LOG_PERIOD) {
            last_log = Clock::now();
            char row[320];
            std::snprintf(row, sizeof(row), "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s",
                          utc_now().c_str(), str_of(tel, "revision").c_str(),
                          (str_of(fw, "date") + "." + str_of(fw, "build")).c_str(),
                          has_p ? str_of(tel, "vin_v").c_str() : "", has_p ? str_of(tel, "iin_a").c_str() : "",
                          str_of(tel, "iin_saturated").c_str(), has_e ? str_of(tel, "temp_c").c_str() : "",
                          has_e ? str_of(tel, "rh_pct").c_str() : "", str_of(dc, "lo_hz").c_str(),
                          str_of(tel, "lo_hz_read").c_str(), str_of(dc, "side").c_str(), str_of(dc, "array").c_str(),
                          str_of(tel, "errors").c_str(), str_of(tel, "stale").c_str());
            append_line("wb_telemetry.csv",
                        "time_utc,revision,fw,vin_v,iin_a,iin_saturated,temp_c,rh_pct,lo_hz,lo_hz_read,side,array,errors,stale",
                        row);
        }
    } else if (!tel.empty()) {
        track("telemetry", "disabled", str_of(tel, "reason"));
    }
}

void handle_line(const std::string& line) {
    const std::string dc = object_after(line, "downconverter");
    if (dc.empty()) return;
    latest = dc;
    latest_at = Clock::now();
    if (lost_logged) {
        lost_logged = false;
        event("telemetry", "restored");
    }
    log_object(dc);
}

}  // namespace

void wb_board_ingest(const char* data, size_t n) {
    std::lock_guard<std::mutex> lock(mtx);
    // only status lines carry "downconverter"; command replies pass through harmlessly
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            handle_line(pending);
            pending.clear();
        } else if (pending.size() < MAX_PENDING) {
            pending.push_back(data[i]);
        }
    }
    if (pending.size() >= MAX_PENDING) pending.clear();
}

std::string wb_board_json() {
    std::lock_guard<std::mutex> lock(mtx);
    if (latest.empty()) return {};
    const auto age = Clock::now() - latest_at;
    if (age > STALE_AFTER) {
        if (!lost_logged) {
            lost_logged = true;
            event("telemetry", "lost (no status from heimdall for 30 s)");
        }
        return {};
    }
    char a[32];
    std::snprintf(a, sizeof(a), ",\"age_s\":%.1f}", std::chrono::duration<double>(age).count());
    return latest.substr(0, latest.size() - 1) + a;
}
