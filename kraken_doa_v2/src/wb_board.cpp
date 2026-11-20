#include "wb_board.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t MAX_PENDING = 256 * 1024;               // a line longer than this is garbage
std::atomic<bool> log_enabled{true};
std::atomic<int> log_period_s{10};
std::atomic<int> log_keep_days{30};
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

// A telemetry log: one CSV file per UTC day in its own folder, an event log
// beside it, and the columns the history charts show.
struct LogSpec {
    const char* tag;        // "wb" / "host": export and download names
    const char* dir;        // folder under the working directory
    const char* prefix;     // daily files: <prefix>YYYYMMDD.csv
    const char* legacy;     // the size-rotated file of an older build ("" = none)
    const char* events;     // event log (rotates at 5 MB, 3 old files kept)
    const char* header;
    const char* name[4];    // the charted series ...
    int column[4];          // ... and their CSV columns
};
const LogSpec WB_LOG{"wb", "wb_logs", "wb_telemetry_", "wb_telemetry.csv", "wb_events.csv",
    "time_utc,revision,fw,vin_v,iin_a,iin_saturated,temp_c,rh_pct,lo_hz,lo_hz_read,side,array,errors,stale",
    {"vin", "iin", "temp", "rh"}, {3, 4, 6, 7}};
const LogSpec HOST_LOG{"host", "host_logs", "host_telemetry_", "", "host_events.csv",
    "time_utc,cpu_temp_c,cpu_pct,ram_used_mb,ram_total_mb,disk_free_mb,disk_total_mb,throttled",
    {"temp", "cpu", "ram", "disk"}, {1, 2, 3, 5}};

std::filesystem::path log_dir(const LogSpec& s) {
    std::error_code ec;
    std::filesystem::path d = std::filesystem::current_path(ec) / s.dir;
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

// days since 1970-01-01 of a civil date (proleptic Gregorian) - a portable timegm
long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// "2026-11-11T09:13:02Z" -> epoch seconds, -1 if it is not one
long long parse_utc(const char* s) {
    int y, mo, d, h, mi, se;
    if (std::sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
}

// <prefix>YYYYMMDD.csv -> the first second after that day, -1 for any other name
long long daily_file_end(const LogSpec& s, const std::string& n) {
    const size_t pl = std::strlen(s.prefix);
    int y, m, d;
    if (n.size() != pl + 12 || n.compare(0, pl, s.prefix) != 0 || n.compare(pl + 8, 4, ".csv") != 0 ||
        std::sscanf(n.c_str() + pl, "%4d%2d%2d", &y, &m, &d) != 3) return -1;
    return (days_from_civil(y, m, d) + 1) * 86400;
}

bool legacy_file(const LogSpec& s, const std::string& n) {
    const size_t l = std::strlen(s.legacy);
    return l && (n == s.legacy || (n.size() == l + 2 && n.compare(0, l, s.legacy) == 0 && n[l] == '.'));
}

// The log files that can hold rows from `from` (epoch s) on, oldest first:
// the older build's rotated files (.3 .. .1, then the live one), then the daily files.
std::vector<std::filesystem::path> telemetry_files(const LogSpec& s, long long from) {
    std::vector<std::filesystem::path> legacy, daily;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(s), ec)) {
        const std::string n = e.path().filename().string();
        if (const long long end = daily_file_end(s, n); end >= 0) {
            if (end > from) daily.push_back(e.path());
        } else if (legacy_file(s, n)) {
            legacy.push_back(e.path());
        }
    }
    std::sort(daily.begin(), daily.end());
    std::sort(legacy.begin(), legacy.end(), [](const auto& a, const auto& b) {
        const std::string x = a.filename().string(), y = b.filename().string();
        return x.size() != y.size() ? x.size() > y.size() : x > y;
    });
    legacy.insert(legacy.end(), daily.begin(), daily.end());
    return legacy;
}

// Delete what is older than the keep setting: daily files by their date, the
// older build's rotated files and finished exports by their modification time.
// At most once an hour per log (and at once when the setting changes).
void purge_old_logs(const LogSpec& s) {
    static Clock::time_point last[2]{};
    static int last_keep[2] = {-1, -1};
    const int k = &s == &HOST_LOG ? 1 : 0;
    const int keep = log_keep_days.load();
    if (last[k] != Clock::time_point{} && keep == last_keep[k] && Clock::now() - last[k] < std::chrono::hours(1)) return;
    last[k] = Clock::now();
    last_keep[k] = keep;
    const long long cutoff = static_cast<long long>(std::time(nullptr)) - static_cast<long long>(keep) * 86400;
    const std::string exports = std::string(s.tag) + "_export_";
    std::error_code ec;
    std::vector<std::filesystem::path> gone;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(s), ec)) {
        const std::string n = e.path().filename().string();
        if (const long long end = daily_file_end(s, n); end >= 0) {
            if (end <= cutoff) gone.push_back(e.path());
        } else if (legacy_file(s, n) || n.rfind(exports, 0) == 0) {
            const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(e.path(), ec);
            const auto limit = n.rfind(exports, 0) == 0 ? std::chrono::hours(1) : std::chrono::hours(24LL * keep);
            if (!ec && age > limit) gone.push_back(e.path());
        }
    }
    for (const auto& p : gone) std::filesystem::remove(p, ec);
}

void append_file(const std::filesystem::path& f, const char* header, const std::string& line) {
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(f, ec);
    std::ofstream out(f, std::ios::app | std::ios::binary);
    if (!out) return;
    if (fresh) out << header << "\n";
    out << line << "\n";
}

// one measurement row into today's file
void append_row(const LogSpec& s, const std::string& row) {
    purge_old_logs(s);
    const std::string now = utc_now();   // 2026-11-11T...
    append_file(log_dir(s) / (s.prefix + now.substr(0, 4) + now.substr(5, 2) + now.substr(8, 2) + ".csv"), s.header, row);
}

void event(const LogSpec& s, const std::string& what, const std::string& detail) {
    std::cout << (&s == &HOST_LOG ? "Host: " : "WB board: ") << what << " " << detail << std::endl;
    const std::filesystem::path f = log_dir(s) / s.events;
    rotate_if_big(f);
    append_file(f, "time_utc,event,detail", utc_now() + "," + what + ",\"" + detail + "\"");
}
void event(const std::string& what, const std::string& detail) { event(WB_LOG, what, detail); }

// Log alarm / state changes: key -> value; the first sighting is logged too
void track(const LogSpec& s, std::map<std::string, std::string>& seen, const std::string& key, const std::string& value,
           const std::string& detail) {
    auto it = seen.find(key);
    if (it != seen.end() && it->second == value) return;
    // an alarm that is fine at the first look is not news (every start would
    // write one line per alarm); anything else seen first is
    const bool quiet = it == seen.end() && value == "ok" && key.rfind("alarm_", 0) == 0;
    seen[key] = value;
    if (quiet) return;
    event(s, key, value + (detail.empty() ? "" : " (" + detail + ")"));
}
void track(const std::string& key, const std::string& value, const std::string& detail) {
    track(WB_LOG, last_state, key, value, detail);
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

        if (log_enabled.load() && Clock::now() - last_log >= std::chrono::seconds(log_period_s.load())) {
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
            append_row(WB_LOG, row);
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
    char a[128];
    std::snprintf(a, sizeof(a), ",\"age_s\":%.1f,\"logging\":{\"enabled\":%s,\"period_s\":%d,\"keep_days\":%d}}",
                  std::chrono::duration<double>(age).count(), log_enabled.load() ? "true" : "false",
                  log_period_s.load(), log_keep_days.load());
    return latest.substr(0, latest.size() - 1) + a;
}

void wb_board_set_log_enabled(bool on) { log_enabled.store(on); }

int wb_board_set_log_period(int seconds) {
    seconds = std::clamp(seconds, 1, 3600);
    log_period_s.store(seconds);
    return seconds;
}

int wb_board_set_log_keep_days(int days) {
    days = std::clamp(days, 1, 365);
    log_keep_days.store(days);
    return days;
}

namespace {

long clamp_range(long range_s) { return std::clamp(range_s, 60L, 366L * 86400L); }

// One logged row: time and the charted measurements (NaN = not in the row)
struct Row {
    long long t = 0;
    double v[4] = {NAN, NAN, NAN, NAN};
};

bool parse_row(const LogSpec& s, const std::string& line, Row& r) {
    if (line.size() < 20 || line[0] < '0' || line[0] > '9') return false;   // header, garbage, a torn last line
    r.t = parse_utc(line.c_str());
    if (r.t < 0) return false;
    size_t pos = 0;
    int col = 0, want = 0;
    while (want < 4) {
        const size_t comma = line.find(',', pos);
        const size_t end = comma == std::string::npos ? line.size() : comma;
        if (col == s.column[want]) {
            if (end > pos) {
                char* e = nullptr;
                const double x = std::strtod(line.c_str() + pos, &e);
                if (e != line.c_str() + pos) r.v[want] = x;
            }
            want++;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
        col++;
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) o += ' ';
        else o += static_cast<char>(c);
    }
    return o;
}

std::string history_json(const LogSpec& s, long range_s) {
    range_s = clamp_range(range_s);
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long from = now - range_s;
    // at least two log periods per bucket: one-period buckets leave holes
    // wherever a row lands a moment late
    const long bucket = std::max({1L, (range_s + 599) / 600, 2L * log_period_s.load()});
    const size_t nb = static_cast<size_t>((range_s + bucket - 1) / bucket);

    struct Acc { double mn = 0, mx = 0, sum = 0; long n = 0; };
    std::vector<Acc> acc[4];
    for (auto& a : acc) a.resize(nb);
    long rows = 0;
    for (const auto& f : telemetry_files(s, from)) {
        std::ifstream in(f, std::ios::binary);
        std::string line;
        Row r;
        while (std::getline(in, line)) {
            r = Row{};
            if (!parse_row(s, line, r) || r.t < from || r.t > now) continue;
            const size_t b = std::min(nb - 1, static_cast<size_t>((r.t - from) / bucket));
            rows++;
            for (int k = 0; k < 4; k++) {
                if (std::isnan(r.v[k])) continue;
                Acc& a = acc[k][b];
                if (!a.n || r.v[k] < a.mn) a.mn = r.v[k];
                if (!a.n || r.v[k] > a.mx) a.mx = r.v[k];
                a.sum += r.v[k];
                a.n++;
            }
        }
    }

    std::string j;
    j.reserve(nb * 110 + 4096);
    char b[128];
    std::snprintf(b, sizeof(b), "{\"src\":\"%s\",\"range_s\":%ld,\"bucket_s\":%ld,\"now\":%lld,\"rows\":%ld,\"t\":[", s.tag, range_s, bucket, now, rows);
    j += b;
    for (size_t i = 0; i < nb; i++) {
        std::snprintf(b, sizeof(b), "%s%lld", i ? "," : "", from + static_cast<long long>(i) * bucket + bucket / 2);
        j += b;
    }
    j += "]";
    for (int k = 0; k < 4; k++) {
        j += std::string(",\"") + s.name[k] + "\":[";
        for (int part = 0; part < 3; part++) {          // min, mean, max
            j += part ? ",[" : "[";
            for (size_t i = 0; i < nb; i++) {
                const Acc& a = acc[k][i];
                if (i) j += ',';
                if (!a.n) { j += "null"; continue; }
                std::snprintf(b, sizeof(b), "%.3f", part == 0 ? a.mn : part == 1 ? a.sum / a.n : a.mx);
                j += b;
            }
            j += "]";
        }
        j += "]";
    }

    // events of the range (the newest 200), oldest first
    std::vector<std::string> ev;
    for (const char* suffix : {".3", ".2", ".1", ""}) {
        std::ifstream in(log_dir(s) / (std::string(s.events) + suffix), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const long long t = parse_utc(line.c_str());
            if (t < from) continue;                      // also the header (-1)
            const size_t c1 = line.find(','), c2 = c1 == std::string::npos ? c1 : line.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;
            std::string detail = line.substr(c2 + 1);
            if (detail.size() >= 2 && detail.front() == '"' && detail.back() == '"') detail = detail.substr(1, detail.size() - 2);
            std::snprintf(b, sizeof(b), "{\"t\":%lld,\"e\":\"", t);
            ev.push_back(b + json_escape(line.substr(c1 + 1, c2 - c1 - 1)) + "\",\"d\":\"" + json_escape(detail) + "\"}");
        }
    }
    j += ",\"events\":[";
    for (size_t i = ev.size() > 200 ? ev.size() - 200 : 0, first = 1; i < ev.size(); i++, first = 0) j += (first ? "" : ",") + ev[i];
    j += "]";

    // size of the whole log on disk
    uintmax_t bytes = 0;
    int files = 0;
    std::string oldest;
    std::error_code ec;
    const size_t pl = std::strlen(s.prefix);
    for (const auto& f : telemetry_files(s, 0)) {
        bytes += std::filesystem::file_size(f, ec);
        files++;
        const std::string n = f.filename().string();
        if (oldest.empty() && daily_file_end(s, n) >= 0) oldest = n.substr(pl, 4) + "-" + n.substr(pl + 4, 2) + "-" + n.substr(pl + 6, 2);
    }
    std::snprintf(b, sizeof(b), ",\"log\":{\"dir\":\"%s\",\"bytes\":%llu,\"files\":%d,\"oldest\":\"%s\"}}",
                  s.dir, static_cast<unsigned long long>(bytes), files, oldest.c_str());
    j += b;
    return j;
}

std::string export_csv(const LogSpec& s, long range_s, std::string* download_name) {
    range_s = clamp_range(range_s);
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long from = now - range_s;
    static std::atomic<unsigned> serial{0};
    const std::string exports = std::string(s.tag) + "_export_";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(s), ec)) {   // exports of earlier downloads
        if (e.path().filename().string().rfind(exports, 0) != 0) continue;
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(e.path(), ec);
        if (!ec && age > std::chrono::minutes(10)) std::filesystem::remove(e.path(), ec);
    }
    const std::filesystem::path out = log_dir(s) / (exports + std::to_string(now) + "_" + std::to_string(serial.fetch_add(1)) + ".csv");
    std::ofstream o(out, std::ios::binary);
    if (!o) return {};
    o << s.header << "\n";
    for (const auto& f : telemetry_files(s, from)) {
        std::ifstream in(f, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const long long t = parse_utc(line.c_str());
            if (t >= from && t <= now) o << line << "\n";
        }
    }
    if (!o.good()) return {};
    if (download_name) {
        const char* span = range_s <= 3600 ? "1h" : range_s <= 86400 ? "24h" : range_s <= 7 * 86400 ? "7d" : "30d";
        *download_name = std::string(s.tag) + "_telemetry_" + utc_now().substr(0, 10) + "_last_" + span + ".csv";
    }
    return out.string();
}

// ---- the computer itself (CPU temperature / load, memory, storage) ----
std::mutex host_mtx;
std::map<std::string, std::string> host_state;      // event keys -> last value
std::string host_fields;                            // the JSON fields for system_status.hw

// Raspberry Pi firmware flags (under-voltage, frequency capping, throttling:
// bits 0-3 now, 16-19 since boot), -1 where there is none
int read_throttled() {
#ifndef _WIN32
    std::ifstream f("/sys/devices/platform/soc/soc:firmware/get_throttled");
    std::string v;
    if (f && (f >> v) && !v.empty()) return static_cast<int>(std::strtol(v.c_str(), nullptr, 16));
    // Pi 5: no such file, the firmware tool is the only source ("throttled=0x0")
    static bool no_tool = false;
    if (no_tool) return -1;
    if (FILE* p = popen("vcgencmd get_throttled 2>/dev/null", "r")) {
        char line[64] = {0};
        const bool got = std::fgets(line, sizeof(line), p) != nullptr;
        pclose(p);
        const char* eq = got ? std::strchr(line, '=') : nullptr;
        if (eq) return static_cast<int>(std::strtol(eq + 1, nullptr, 16));
    }
    no_tool = true;  // not a Pi (or no vcgencmd): do not spawn a shell every time
#endif
    return -1;
}

}  // namespace

std::string wb_board_history_json(long range_s, bool host) { return history_json(host ? HOST_LOG : WB_LOG, range_s); }

std::string wb_board_export_csv(long range_s, std::string* download_name, bool host) {
    return export_csv(host ? HOST_LOG : WB_LOG, range_s, download_name);
}

void host_log_tick(double temp_c, double cpu_pct, double ram_used_mb, double ram_total_mb) {
    std::lock_guard<std::mutex> lock(host_mtx);
    static Clock::time_point last_row{}, last_slow{};
    static double disk_free_mb = -1, disk_total_mb = -1;
    static int throttled = -1;
    const auto now = Clock::now();
    if (last_slow == Clock::time_point{} || now - last_slow >= std::chrono::seconds(10)) {   // statfs + sysfs: not at 2 Hz
        last_slow = now;
        std::error_code ec;
        const auto sp = std::filesystem::space(std::filesystem::current_path(ec), ec);
        if (!ec) { disk_free_mb = sp.available / 1048576.0; disk_total_mb = sp.capacity / 1048576.0; }
        throttled = read_throttled();
    }

    // alarm levels (shown on the dashboard, logged when they change)
    const double ram_pct = ram_total_mb > 0 ? 100.0 * ram_used_mb / ram_total_mb : 0;
    const double disk_pct = disk_total_mb > 0 ? 100.0 * disk_free_mb / disk_total_mb : 100;
    const char* a_temp = temp_c < 0 ? "ok" : temp_c >= 82 ? "alarm" : temp_c >= 75 ? "warn" : "ok";
    const char* a_ram = ram_pct >= 95 ? "alarm" : ram_pct >= 85 ? "warn" : "ok";
    const char* a_disk = disk_free_mb < 0 ? "ok" : (disk_free_mb < 500 || disk_pct < 3) ? "alarm" : (disk_free_mb < 2048 || disk_pct < 10) ? "warn" : "ok";
    const char* a_power = throttled < 0 ? "ok" : (throttled & 0x1) ? "alarm" : (throttled & 0xF000F) ? "warn" : "ok";
    char d[176], tc[24] = "no temperature";
    if (temp_c >= 0) std::snprintf(tc, sizeof(tc), "%.0f C", temp_c);
    std::snprintf(d, sizeof(d), "%s, CPU %.0f %%, RAM %.0f / %.0f MB, storage %.1f GB free of %.1f GB", tc, cpu_pct,
                  ram_used_mb, ram_total_mb, disk_free_mb / 1024, disk_total_mb / 1024);
    track(HOST_LOG, host_state, "alarm_temp", a_temp, d);
    track(HOST_LOG, host_state, "alarm_ram", a_ram, d);
    track(HOST_LOG, host_state, "alarm_storage", a_disk, d);
    if (throttled >= 0) {
        char t[64];
        std::snprintf(t, sizeof(t), "get_throttled 0x%x%s", throttled, (throttled & 0x1) ? ": under-voltage NOW" : (throttled & 0x10000) ? ": under-voltage has occurred" : "");
        track(HOST_LOG, host_state, "alarm_power", a_power, t);
    }

    char f[360];
    std::snprintf(f, sizeof(f), ",\"disk_free_mb\":%.0f,\"disk_total_mb\":%.0f,\"throttled\":%d,"
                  "\"alarms\":{\"temp\":\"%s\",\"ram\":\"%s\",\"disk\":\"%s\",\"power\":\"%s\"},"
                  "\"logging\":{\"enabled\":%s,\"period_s\":%d,\"keep_days\":%d}",
                  disk_free_mb, disk_total_mb, throttled, a_temp, a_ram, a_disk, a_power,
                  log_enabled.load() ? "true" : "false", log_period_s.load(), log_keep_days.load());
    host_fields = f;

    if (log_enabled.load() && (last_row == Clock::time_point{} || now - last_row >= std::chrono::seconds(log_period_s.load()))) {
        last_row = now;
        char row[200], temp[16] = "";                    // no temperature reading (Windows): an empty cell
        if (temp_c >= 0) std::snprintf(temp, sizeof(temp), "%.1f", temp_c);
        std::snprintf(row, sizeof(row), "%s,%s,%.1f,%.0f,%.0f,%.0f,%.0f,%d", utc_now().c_str(), temp, cpu_pct, ram_used_mb,
                      ram_total_mb, disk_free_mb, disk_total_mb, throttled);
        append_row(HOST_LOG, row);
    }
}

std::string host_log_json_fields() {
    std::lock_guard<std::mutex> lock(host_mtx);
    return host_fields;
}
