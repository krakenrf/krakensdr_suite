#include "wb_board.hpp"

#include "utils/log_retention.hpp"
#include "utils/process.hpp"

#include <zlib.h>

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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
using logret::Kind;

constexpr size_t MAX_PENDING = 256 * 1024;               // a line longer than this is garbage
std::atomic<bool> log_enabled{true};
std::atomic<int> log_period_s{10};
std::atomic<int> log_keep_days{7};     // Keep: days a finished day stays plain (WB_LOG_KEEP)
std::atomic<int> log_zip_days{365};    // Then compressed: days gzipped after those (WB_LOG_ZIP; 0 = deleted after Keep)
constexpr auto STALE_AFTER = std::chrono::seconds(30);
// a day's event file stops at this size (an alarm flapping all day): the rest of
// that UTC day's events are not written (the size-rotated event log before the
// daily files capped it at 4 x 5 MB)
constexpr uintmax_t EVENTS_DAY_CAP = 5 * 1024 * 1024;

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

// today's UTC date as a day number (logret::day_number): the files are per UTC day
long utc_day() {
    return static_cast<long>(static_cast<long long>(std::time(nullptr)) / 86400);
}

// A telemetry log: one CSV file of rows and one of events per UTC day in its
// own folder - plain for the Keep days, then gzipped for the Then compressed
// days, then deleted (utils/log_retention.hpp, the decoder log's rule) - and
// the columns the history charts show.
struct LogSpec {
    const char* tag;            // "wb" / "host": export and download names
    const char* label;          // the pass's log lines
    const char* dir;            // folder under the working directory
    const char* prefix;         // rows: <prefix>YYYYMMDD.csv, then <prefix>YYYYMMDD.csv.gz
    const char* events;         // events: <events>YYYYMMDD.csv, then .csv.gz
    const char* legacy;         // the size-rotated rows of an older build ("" = none)
    const char* legacy_events;  // the size-rotated event log of an older build (5 MB, .1 - .3)
    const char* header;
    const char* name[4];        // the charted series ...
    int column[4];              // ... and their CSV columns
};
const LogSpec WB_LOG{"wb", "WB board log", "wb_logs", "wb_telemetry_", "wb_events_", "wb_telemetry.csv", "wb_events.csv",
    "time_utc,revision,fw,vin_v,iin_a,iin_saturated,temp_c,rh_pct,lo_hz,lo_hz_read,side,array,errors,stale",
    {"vin", "iin", "temp", "rh"}, {3, 4, 6, 7}};
const LogSpec HOST_LOG{"host", "Host log", "host_logs", "host_telemetry_", "host_events_", "", "host_events.csv",
    "time_utc,cpu_temp_c,cpu_pct,ram_used_mb,ram_total_mb,disk_free_mb,disk_total_mb,throttled",
    {"temp", "cpu", "ram", "disk"}, {1, 2, 3, 5}};
constexpr const char* EVENTS_HEADER = "time_utc,event,detail";

// the folder, relative to the working directory (created if missing)
fs::path log_dir(const LogSpec& s) {
    std::error_code ec;
    fs::create_directories(s.dir, ec);
    return fs::path(s.dir);
}

// "2026-11-11T09:13:02Z" -> epoch seconds, -1 if it is not one
long long parse_utc(const char* s) {
    int y, mo, d, h, mi, se;
    if (std::sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    return static_cast<long long>(logret::day_number(y, mo, d)) * 86400 + h * 3600 + mi * 60 + se;
}

// <prefix>YYYYMMDD.csv / .csv.gz / .csv.gz.part -> its day number (and kind); -1 for any other name
long daily_day(const char* prefix, const std::string& n, Kind* kind) {
    const size_t pl = std::strlen(prefix);
    if (n.size() < pl + 12 || n.compare(0, pl, prefix) != 0) return -1;
    for (size_t i = pl; i < pl + 8; i++)
        if (n[i] < '0' || n[i] > '9') return -1;
    const std::string rest = n.substr(pl + 8);
    const Kind k = rest == ".csv" ? Kind::PLAIN : rest == ".csv.gz" ? Kind::GZ : rest == ".csv.gz.part" ? Kind::PART : Kind::NONE;
    if (k == Kind::NONE) return -1;
    const int y = std::atoi(n.substr(pl, 4).c_str()), m = std::atoi(n.substr(pl + 4, 2).c_str()),
              d = std::atoi(n.substr(pl + 6, 2).c_str());
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    if (kind) *kind = k;
    return logret::day_number(y, m, d);
}

// "2026-11-11T..." -> <prefix>20261111.csv
std::string day_file_name(const char* prefix, const std::string& utc) {
    return prefix + utc.substr(0, 4) + utc.substr(5, 2) + utc.substr(8, 2) + ".csv";
}

// the older build's file `base` or one of its rotations (base.1 .. base.9)
bool legacy_name(const char* base, const std::string& n) {
    const size_t l = std::strlen(base);
    return l && (n == base || (n.size() == l + 2 && n.compare(0, l, base) == 0 && n[l] == '.' && n[l + 1] >= '0' && n[l + 1] <= '9'));
}

// The names in the log's folder (only plain ASCII names are ours; anything
// the file system can't hand over as such is skipped)
std::vector<std::pair<fs::path, std::string>> folder_entries(const LogSpec& s) {
    std::vector<std::pair<fs::path, std::string>> v;
    std::error_code ec;
    try {
        for (fs::directory_iterator it(log_dir(s), ec), end; !ec && it != end; it.increment(ec))
            v.emplace_back(it->path(), it->path().filename().string());
    } catch (const std::exception&) {
    }
    return v;
}

struct DayFile {
    fs::path path;
    long day = -1;
    Kind kind = Kind::NONE;
    bool events = false;   // the event log's file of that day (else the rows')
};

// every daily file of the log - rows and events, plain / gz / part - in no order
std::vector<DayFile> day_files(const LogSpec& s) {
    std::vector<DayFile> v;
    for (const auto& [p, n] : folder_entries(s)) {
        Kind k = Kind::NONE;
        if (const long d = daily_day(s.prefix, n, &k); d >= 0) v.push_back({p, d, k, false});
        else if (const long e = daily_day(s.events, n, &k); e >= 0) v.push_back({p, e, k, true});
    }
    return v;
}

// The files that can hold rows (events = true: events) from `from` (epoch s)
// on, oldest first: the older build's rotated files (.3 .. .1, then the live
// one), then one file per day - the plain one, else its .gz (both exist only
// while a gzip is being made, or after a crash between its last two steps:
// the same rows; read once)
std::vector<fs::path> log_files(const LogSpec& s, long long from, bool events) {
    std::vector<fs::path> legacy;
    std::map<long, std::pair<fs::path, Kind>> daily;   // day -> its file
    const char* base = events ? s.legacy_events : s.legacy;
    for (const auto& [p, n] : folder_entries(s)) {
        if (legacy_name(base, n)) legacy.push_back(p);
    }
    for (const auto& f : day_files(s)) {
        if (f.events != events || (f.kind != Kind::PLAIN && f.kind != Kind::GZ)) continue;
        if ((static_cast<long long>(f.day) + 1) * 86400 <= from) continue;   // the whole day is before the range
        auto it = daily.find(f.day);
        if (it == daily.end() || f.kind == Kind::PLAIN) daily[f.day] = {f.path, f.kind};
    }
    std::sort(legacy.begin(), legacy.end(), [](const auto& a, const auto& b) {
        const std::string x = a.filename().string(), y = b.filename().string();
        return x.size() != y.size() ? x.size() > y.size() : x > y;
    });
    for (const auto& kv : daily) legacy.push_back(kv.second.first);
    return legacy;
}

// Lines of a log file, plain or gzip (zlib reads a plain file as it is), one
// at a time without the line end; a damaged gzip ends early
class LineReader {
public:
    explicit LineReader(const fs::path& p) : f_(gzopen(p.string().c_str(), "rb")) {
        if (f_) gzbuffer(f_, 128 * 1024);
    }
    ~LineReader() {
        if (f_) gzclose(f_);
    }
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;
    bool ok() const { return f_ != nullptr; }
    bool next(std::string& line) {
        line.clear();
        if (!f_) return false;
        char buf[4096];
        for (;;) {
            if (!gzgets(f_, buf, sizeof(buf))) return !line.empty();
            const size_t n = std::strlen(buf);
            if (n && buf[n - 1] == '\n') {
                line.append(buf, n - 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            line.append(buf, n);
        }
    }

private:
    gzFile f_;
};

// Open a listed file; one the pass gzipped since it was listed is read from its .gz
std::unique_ptr<LineReader> open_lines(const fs::path& p) {
    auto r = std::make_unique<LineReader>(p);
    if (!r->ok() && p.extension() == ".csv") r = std::make_unique<LineReader>(fs::path(p.string() + ".gz"));
    return r;
}

void append_file(const fs::path& f, const char* header, const std::string& line) {
    std::error_code ec;
    const bool fresh = !fs::exists(f, ec);
    std::ofstream out(f, std::ios::app | std::ios::binary);
    if (!out) return;
    if (fresh) out << header << "\n";
    out << line << "\n";
}

// one measurement row into today's file
void append_row(const LogSpec& s, const std::string& row) {
    append_file(log_dir(s) / day_file_name(s.prefix, utc_now()), s.header, row);
}

void event(const LogSpec& s, const std::string& what, const std::string& detail) {
    std::cout << (&s == &HOST_LOG ? "Host: " : "WB board: ") << what << " " << detail << std::endl;
    const std::string now = utc_now();
    const fs::path f = log_dir(s) / day_file_name(s.events, now);
    std::error_code ec;
    const uintmax_t size = fs::exists(f, ec) ? fs::file_size(f, ec) : 0;
    if (ec || size >= EVENTS_DAY_CAP) return;   // a day of flapping alarms: the rest of the day is not written
    const std::string line = now + "," + what + ",\"" + detail + "\"";
    append_file(f, EVENTS_HEADER, line);
    if (size + line.size() + 1 >= EVENTS_DAY_CAP)
        append_file(f, EVENTS_HEADER, now + ",log_events,\"5 MB of events today: the rest of this UTC day is not logged\"");
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

// ---- retention: plain for Keep days, gzip for Then compressed more, deleted ----
std::atomic<long> last_pass_day{-1};
std::atomic<long long> last_pass_ms{0};   // steady clock

long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// One log's pass: its daily files by the shared rule; an older build's files
// (no date in the name) by their modification time - kept for the Keep + Then
// compressed days, never compressed; finished exports after an hour
void pass_log(const LogSpec& s, long today, int keep, int zip) {
    std::vector<logret::File> files;
    for (const auto& f : day_files(s)) files.push_back({f.path.string(), f.day, f.kind});
    logret::run_pass(files, today, keep, zip, s.label);
    const std::string exports = std::string(s.tag) + "_export_";
    const auto now = fs::file_time_type::clock::now();
    for (const auto& [p, n] : folder_entries(s)) {
        const bool exp = n.rfind(exports, 0) == 0;
        if (!exp && !legacy_name(s.legacy, n) && !legacy_name(s.legacy_events, n)) continue;
        std::error_code ec;
        const auto age = now - fs::last_write_time(p, ec);
        const auto limit = exp ? std::chrono::hours(1) : std::chrono::hours(24LL * (keep + zip));
        if (!ec && age > limit && fs::remove(p, ec) && !exp)
            std::cout << s.label << ": deleted " << p.string() << " (an older version's file, older than " << keep << " + "
                      << zip << " days)" << std::endl;
    }
}

// Both logs in one pass (they share the settings), on its own thread, one at a
// time: once the settings are replayed (wb_board_settings_restored), then on a
// Keep / Then compressed change, at the UTC day change and every hour (sampler)
logret::Scheduler& passes() {
    static logret::Scheduler* p = new logret::Scheduler([] {   // never destroyed (a pass may run at exit)
        const long today = utc_day();
        last_pass_day = today;
        last_pass_ms = steady_ms();
        const int keep = log_keep_days.load(), zip = log_zip_days.load();
        pass_log(WB_LOG, today, keep, zip);
        pass_log(HOST_LOG, today, keep, zip);
    });
    return *p;
}

// ---- the computer itself (CPU temperature / load, memory, storage) ----
std::mutex host_mtx;
std::map<std::string, std::string> host_state;      // event keys -> last value
std::string host_fields;                            // the JSON fields for system_status.hw

// Raspberry Pi firmware flags (under-voltage, frequency capping, throttling:
// bits 0-3 now, 16-19 since boot), -1 where there is none. The sampler's
// thread only: the Pi 5 reads them with a child process
int read_throttled() {
#ifndef _WIN32
    std::ifstream f("/sys/devices/platform/soc/soc:firmware/get_throttled");
    std::string v;
    if (f && (f >> v) && !v.empty()) return static_cast<int>(std::strtol(v.c_str(), nullptr, 16));
    // Pi 5: no such file, the firmware tool is the only source ("throttled=0x0")
    static bool no_tool = false;
    if (no_tool) return -1;
    std::string out;
    kproc::run_capture({"vcgencmd", "get_throttled"}, 5000, &out, nullptr, 4096, true);
    if (const size_t eq = out.find('='); eq != std::string::npos) return static_cast<int>(std::strtol(out.c_str() + eq + 1, nullptr, 16));
    no_tool = true;  // not a Pi (or no vcgencmd): do not start it every time
#endif
    return -1;
}

// What the sampler measured last: the drive the logs are on (statfs) and the
// Pi's throttling flags - slow or blocking calls (a busy SD card, a network
// mount, vcgencmd), so they run on the sampler's thread every 10 s, never on
// the uWS loop that calls host_log_tick
struct HostSample {
    double disk_free_mb = -1, disk_total_mb = -1;
    int throttled = -1;
    bool valid = false;   // measured at least once
};
std::mutex sample_mtx;
HostSample host_sample;

void sampler() {
    for (;;) {
        HostSample h;
        std::error_code ec;
        const auto sp = fs::space(fs::current_path(ec), ec);
        if (!ec) { h.disk_free_mb = sp.available / 1048576.0; h.disk_total_mb = sp.capacity / 1048576.0; }
        h.throttled = read_throttled();
        h.valid = true;
        {
            std::lock_guard<std::mutex> lock(sample_mtx);
            host_sample = h;
        }
        // the retention pass hourly and at the UTC day change (none before the settings replay)
        if (passes().allowed() && (last_pass_day.load() != utc_day() || steady_ms() - last_pass_ms.load() >= 3600000))
            passes().request();
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

void start_sampler() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(sampler).detach(); });
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
        // generator mode of the LO synthesizer (heimdall's fw.lo_mode; absent from older builds)
        const std::string mode = str_of(fw, "lo_mode");
        if (!mode.empty() && mode != "unknown")
            track("lo_mode", mode, mode == "generator" ? "" : "LO not confirmed at full level: reception may be far weaker");
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
    char a[160];
    std::snprintf(a, sizeof(a), ",\"age_s\":%.1f,\"logging\":{\"enabled\":%s,\"period_s\":%d,\"keep_days\":%d,\"zip_days\":%d}}",
                  std::chrono::duration<double>(age).count(), log_enabled.load() ? "true" : "false",
                  log_period_s.load(), log_keep_days.load(), log_zip_days.load());
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
    if (log_keep_days.exchange(days) != days) passes().request();
    return days;
}

int wb_board_set_log_zip_days(int days) {
    days = std::clamp(days, 0, 3650);
    if (log_zip_days.exchange(days) != days) passes().request();
    return days;
}

void wb_board_settings_restored() {
    start_sampler();
    passes().allow();
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
    std::string line;
    for (const auto& f : log_files(s, from, false)) {
        const auto in = open_lines(f);
        Row r;
        while (in->next(line)) {
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
    char b[320];
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
    for (const auto& f : log_files(s, from, true)) {
        const auto in = open_lines(f);
        while (in->next(line)) {
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

    // the whole log on disk (rows and events, plain and gzipped, an older version's files):
    // bytes / files in all, and per kind; the oldest day of rows
    uintmax_t plain_b = 0, gz_b = 0;
    int plain_n = 0, gz_n = 0;
    long oldest = -1;
    std::error_code ec;
    for (const auto& f : day_files(s)) {
        if (f.kind != Kind::PLAIN && f.kind != Kind::GZ) continue;
        const uintmax_t sz = fs::file_size(f.path, ec);
        if (ec) continue;
        if (f.kind == Kind::GZ) { gz_b += sz; gz_n++; } else { plain_b += sz; plain_n++; }
        if (!f.events && (oldest < 0 || f.day < oldest)) oldest = f.day;
    }
    for (const auto& [p, n] : folder_entries(s)) {
        if (!legacy_name(s.legacy, n) && !legacy_name(s.legacy_events, n)) continue;
        const uintmax_t sz = fs::file_size(p, ec);
        if (!ec) { plain_b += sz; plain_n++; }
    }
    char od[16] = "";
    if (oldest >= 0) {
        const std::time_t t = static_cast<std::time_t>(oldest) * 86400;
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::strftime(od, sizeof(od), "%Y-%m-%d", &tm);
    }
    std::snprintf(b, sizeof(b), ",\"log\":{\"dir\":\"%s\",\"bytes\":%llu,\"files\":%d,\"oldest\":\"%s\",\"plain_bytes\":%llu,"
                  "\"plain_files\":%d,\"gz_bytes\":%llu,\"gz_files\":%d}}",
                  s.dir, static_cast<unsigned long long>(plain_b + gz_b), plain_n + gz_n, od,
                  static_cast<unsigned long long>(plain_b), plain_n, static_cast<unsigned long long>(gz_b), gz_n);
    j += b;
    return j;
}

// The export's output: a plain file, or gzip (zlib level 6)
class ExportOut {
public:
    ExportOut(const fs::path& p, bool gz) {
        if (gz) gz_ = gzopen(p.string().c_str(), "wb6");
        else plain_.open(p, std::ios::binary);
        ok_ = gz ? gz_ != nullptr : static_cast<bool>(plain_);
    }
    ~ExportOut() {
        if (gz_) gzclose(gz_);
    }
    ExportOut(const ExportOut&) = delete;
    ExportOut& operator=(const ExportOut&) = delete;
    void line(const std::string& l) {
        buf_ += l;
        buf_ += '\n';
        if (buf_.size() >= (1u << 20)) flush();
    }
    // everything written (false: a write failed - the export is not served)
    bool finish() {
        flush();
        if (gz_) {
            ok_ = gzclose(gz_) == Z_OK && ok_;
            gz_ = nullptr;
        } else {
            plain_.close();
            ok_ = ok_ && !plain_.fail();
        }
        return ok_;
    }
    bool ok() const { return ok_; }

private:
    void flush() {
        if (buf_.empty() || !ok_) { buf_.clear(); return; }
        if (gz_) ok_ = gzwrite(gz_, buf_.data(), static_cast<unsigned>(buf_.size())) == static_cast<int>(buf_.size());
        else ok_ = static_cast<bool>(plain_.write(buf_.data(), static_cast<std::streamsize>(buf_.size())));
        buf_.clear();
    }
    gzFile gz_ = nullptr;
    std::ofstream plain_;
    std::string buf_;
    bool ok_ = false;
};

std::string export_csv(const LogSpec& s, long range_s, std::string* download_name, bool gz) {
    range_s = clamp_range(range_s);
    const long long now = static_cast<long long>(std::time(nullptr));
    const long long from = now - range_s;
    static std::atomic<unsigned> serial{0};
    const std::string exports = std::string(s.tag) + "_export_";
    std::error_code ec;
    for (const auto& [p, n] : folder_entries(s)) {   // exports of earlier downloads
        if (n.rfind(exports, 0) != 0) continue;
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(p, ec);
        if (!ec && age > std::chrono::minutes(10)) fs::remove(p, ec);
    }
    const char* ext = gz ? ".csv.gz" : ".csv";
    const fs::path out = log_dir(s) / (exports + std::to_string(now) + "_" + std::to_string(serial.fetch_add(1)) + ext);
    ExportOut o(out, gz);
    if (!o.ok()) return {};
    o.line(s.header);
    std::string line;
    for (const auto& f : log_files(s, from, false)) {
        const auto in = open_lines(f);
        while (in->next(line)) {
            const long long t = parse_utc(line.c_str());
            if (t >= from && t <= now) o.line(line);
        }
    }
    if (!o.finish()) return {};
    if (download_name) {
        const char* span = range_s <= 3600 ? "1h" : range_s <= 86400 ? "24h" : range_s <= 7 * 86400 ? "7d" : "30d";
        *download_name = std::string(s.tag) + "_telemetry_" + utc_now().substr(0, 10) + "_last_" + span + ext;
    }
    return out.string();
}

}  // namespace

std::string wb_board_history_json(long range_s, bool host) { return history_json(host ? HOST_LOG : WB_LOG, range_s); }

std::string wb_board_export_csv(long range_s, std::string* download_name, bool host, bool gz) {
    return export_csv(host ? HOST_LOG : WB_LOG, range_s, download_name, gz);
}

void host_log_tick(double temp_c, double cpu_pct, double ram_used_mb, double ram_total_mb) {
    start_sampler();
    HostSample hs;
    {
        std::lock_guard<std::mutex> lock(sample_mtx);
        hs = host_sample;   // (the sampler's, at most 10 s old - never measured here)
    }
    std::lock_guard<std::mutex> lock(host_mtx);
    static Clock::time_point last_row{};
    const auto now = Clock::now();
    const double disk_free_mb = hs.disk_free_mb, disk_total_mb = hs.disk_total_mb;
    const int throttled = hs.throttled;

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
    if (hs.valid) track(HOST_LOG, host_state, "alarm_storage", a_disk, d);   // (not before the first measurement)
    if (throttled >= 0) {
        char t[64];
        std::snprintf(t, sizeof(t), "get_throttled 0x%x%s", throttled, (throttled & 0x1) ? ": under-voltage NOW" : (throttled & 0x10000) ? ": under-voltage has occurred" : "");
        track(HOST_LOG, host_state, "alarm_power", a_power, t);
    }

    char f[400];
    std::snprintf(f, sizeof(f), ",\"disk_free_mb\":%.0f,\"disk_total_mb\":%.0f,\"throttled\":%d,"
                  "\"alarms\":{\"temp\":\"%s\",\"ram\":\"%s\",\"disk\":\"%s\",\"power\":\"%s\"},"
                  "\"logging\":{\"enabled\":%s,\"period_s\":%d,\"keep_days\":%d,\"zip_days\":%d}",
                  disk_free_mb, disk_total_mb, throttled, a_temp, a_ram, a_disk, a_power,
                  log_enabled.load() ? "true" : "false", log_period_s.load(), log_keep_days.load(), log_zip_days.load());
    host_fields = f;

    // (the first row waits for the first measurement: no row with unknown storage)
    if (hs.valid && log_enabled.load() && (last_row == Clock::time_point{} || now - last_row >= std::chrono::seconds(log_period_s.load()))) {
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
