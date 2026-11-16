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

const char* const TELEMETRY_HEADER =
    "time_utc,revision,fw,vin_v,iin_a,iin_saturated,temp_c,rh_pct,lo_hz,lo_hz_read,side,array,errors,stale";

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

// The telemetry log files that can hold rows from `from` (epoch s) on, oldest
// first: the size-rotated wb_telemetry.csv.N .. wb_telemetry.csv of an older
// build, then the daily wb_telemetry_YYYYMMDD.csv files.
std::vector<std::filesystem::path> telemetry_files(long long from) {
    std::vector<std::filesystem::path> legacy, daily;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(), ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("wb_telemetry_", 0) == 0 && n.size() == 25 && n.substr(21) == ".csv") {
            int y, m, d;
            if (std::sscanf(n.c_str() + 13, "%4d%2d%2d", &y, &m, &d) != 3) continue;
            if ((days_from_civil(y, m, d) + 1) * 86400 <= from) continue;   // the whole day is older
            daily.push_back(e.path());
        } else if (n == "wb_telemetry.csv" || (n.rfind("wb_telemetry.csv.", 0) == 0 && n.size() == 18)) {
            legacy.push_back(e.path());
        }
    }
    std::sort(daily.begin(), daily.end());
    std::sort(legacy.begin(), legacy.end(), [](const auto& a, const auto& b) {
        return a.filename().string().size() != b.filename().string().size()
                   ? a.filename().string().size() > b.filename().string().size()   // ".N" before the live file
                   : a.filename().string() > b.filename().string();                // .3 before .1
    });
    legacy.insert(legacy.end(), daily.begin(), daily.end());
    return legacy;
}

std::filesystem::path telemetry_file_today() {
    const std::string now = utc_now();   // 2026-11-11T...
    return log_dir() / ("wb_telemetry_" + now.substr(0, 4) + now.substr(5, 2) + now.substr(8, 2) + ".csv");
}

// Delete what is older than the keep setting: daily files by their date, the
// older build's rotated files by their modification time. At most once an hour.
void purge_old_logs() {
    static Clock::time_point last{};
    static int last_keep = -1;
    const int keep = log_keep_days.load();
    if (last != Clock::time_point{} && keep == last_keep && Clock::now() - last < std::chrono::hours(1)) return;
    last = Clock::now();
    last_keep = keep;
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long cutoff = now - static_cast<long long>(keep) * 86400;
    std::error_code ec;
    std::vector<std::filesystem::path> gone;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(), ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("wb_telemetry_", 0) == 0 && n.size() == 25 && n.substr(21) == ".csv") {
            int y, m, d;
            if (std::sscanf(n.c_str() + 13, "%4d%2d%2d", &y, &m, &d) == 3 && (days_from_civil(y, m, d) + 1) * 86400 <= cutoff)
                gone.push_back(e.path());
        } else if (n.rfind("wb_telemetry.csv", 0) == 0 || n.rfind("wb_export_", 0) == 0) {
            const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(e.path(), ec);
            const auto limit = n.rfind("wb_export_", 0) == 0 ? std::chrono::hours(1) : std::chrono::hours(24LL * keep);
            if (!ec && age > limit) gone.push_back(e.path());
        }
    }
    for (const auto& p : gone) std::filesystem::remove(p, ec);
}

void append_line(const char* name, const char* header, const std::string& line) {
    const bool telemetry = std::strcmp(name, "wb_telemetry.csv") == 0;
    const std::filesystem::path f = telemetry ? telemetry_file_today() : log_dir() / name;
    if (telemetry) purge_old_logs();
    else rotate_if_big(f);
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
            append_line("wb_telemetry.csv", TELEMETRY_HEADER, row);
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

// One logged row: time and the four measurements (NaN = not in the row)
struct Row {
    long long t = 0;
    double v[4] = {NAN, NAN, NAN, NAN};   // vin_v, iin_a, temp_c, rh_pct
};

bool parse_row(const std::string& line, Row& r) {
    if (line.size() < 20 || line[0] < '0' || line[0] > '9') return false;   // header, garbage, a torn last line
    r.t = parse_utc(line.c_str());
    if (r.t < 0) return false;
    static const int column[4] = {3, 4, 6, 7};
    size_t pos = 0;
    int col = 0, want = 0;
    while (want < 4) {
        const size_t comma = line.find(',', pos);
        const size_t end = comma == std::string::npos ? line.size() : comma;
        if (col == column[want]) {
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

}  // namespace

std::string wb_board_history_json(long range_s) {
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
    for (const auto& f : telemetry_files(from)) {
        std::ifstream in(f, std::ios::binary);
        std::string line;
        Row r;
        while (std::getline(in, line)) {
            r = Row{};
            if (!parse_row(line, r) || r.t < from || r.t > now) continue;
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
    char b[96];
    std::snprintf(b, sizeof(b), "{\"range_s\":%ld,\"bucket_s\":%ld,\"now\":%lld,\"rows\":%ld,\"t\":[", range_s, bucket, now, rows);
    j += b;
    for (size_t i = 0; i < nb; i++) {
        std::snprintf(b, sizeof(b), "%s%lld", i ? "," : "", from + static_cast<long long>(i) * bucket + bucket / 2);
        j += b;
    }
    j += "]";
    static const char* const names[4] = {"vin", "iin", "temp", "rh"};
    for (int k = 0; k < 4; k++) {
        j += std::string(",\"") + names[k] + "\":[";
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
    for (const char* n : {"wb_events.csv.3", "wb_events.csv.2", "wb_events.csv.1", "wb_events.csv"}) {
        std::ifstream in(log_dir() / n, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
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
    for (const auto& f : telemetry_files(0)) {
        bytes += std::filesystem::file_size(f, ec);
        files++;
        const std::string n = f.filename().string();
        if (oldest.empty() && n.size() == 25) oldest = n.substr(13, 4) + "-" + n.substr(17, 2) + "-" + n.substr(19, 2);
    }
    std::snprintf(b, sizeof(b), ",\"log\":{\"bytes\":%llu,\"files\":%d,\"oldest\":\"%s\"}}",
                  static_cast<unsigned long long>(bytes), files, oldest.c_str());
    j += b;
    return j;
}

std::string wb_board_export_csv(long range_s, std::string* download_name) {
    range_s = clamp_range(range_s);
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long from = now - range_s;
    static std::atomic<unsigned> serial{0};
    const std::string stamp = utc_now().substr(0, 10);
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(log_dir(), ec)) {   // exports of earlier downloads
        if (e.path().filename().string().rfind("wb_export_", 0) != 0) continue;
        const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(e.path(), ec);
        if (!ec && age > std::chrono::minutes(10)) std::filesystem::remove(e.path(), ec);
    }
    const std::filesystem::path out = log_dir() / ("wb_export_" + std::to_string(now) + "_" + std::to_string(serial.fetch_add(1)) + ".csv");
    std::ofstream o(out, std::ios::binary);
    if (!o) return {};
    o << TELEMETRY_HEADER << "\n";
    for (const auto& f : telemetry_files(from)) {
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
        *download_name = "wb_telemetry_" + stamp + "_last_" + span + ".csv";
    }
    return out.string();
}
