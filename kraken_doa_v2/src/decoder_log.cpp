#include "decoder_log.hpp"
#include "decoder_log_names.hpp"
#include "playback.hpp"

#include "digital/digital_decoder.hpp"
#include "globals.hpp"
#include "utils/disk_util.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/log_retention.hpp"
#include "utils/parse_num.hpp"

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace declog {

namespace {

using diskutil::absolute;
using diskutil::disk_space;
using diskutil::mkdirs;

enum : unsigned { T_EVENT = 1, T_MESSAGE = 2, T_POSITION = 4, T_RAW = 8, T_INCIDENT = 16, T_VOICE = 32 };
const std::pair<const char*, unsigned> TYPE_NAMES[] = {
    {"event", T_EVENT}, {"message", T_MESSAGE}, {"position", T_POSITION}, {"raw", T_RAW}, {"incident", T_INCIDENT},
    {"voice", T_VOICE}};
constexpr size_t MAX_BUFFER = 32u << 20;   // records kept while the disk can't be written (then dropped) - all files together
constexpr int64_t IDLE_CLOSE_MS = 600000;  // a file without a record for 10 min is closed (KRAKEN_DECLOG_IDLE_S: tests)
constexpr double GZ_RATIO_ASSUMED = 6;     // the page's estimate while no .gz file exists to measure

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string local_date(time_t t) {
    struct tm tm{};
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

// 2026-10-06T18:44:01.123+13:00
std::string iso_time(int64_t ms) {
    const time_t t = static_cast<time_t>(ms / 1000);
    struct tm tm{};
    localtime_r(&t, &tm);
    char b[40], z[8];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(z, sizeof z, "%z", &tm);   // +1300
    char out[64];
    snprintf(out, sizeof out, "%s.%03d%.3s:%.2s", b, static_cast<int>(ms % 1000), z, z + 3);
    return out;
}

// a gzip file's uncompressed size (its ISIZE trailer, mod 4 GiB); 0 = unknown
uint64_t gz_isize(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    unsigned char b[4] = {0, 0, 0, 0};
    uint64_t n = 0;
    if (fseek(f, -4, SEEK_END) == 0 && fread(b, 1, 4, f) == 4)
        n = b[0] | (static_cast<uint64_t>(b[1]) << 8) | (static_cast<uint64_t>(b[2]) << 16) | (static_cast<uint64_t>(b[3]) << 24);
    fclose(f);
    return n;
}

std::string num(double v, const char* f) {
    if (!is_finite_value(v)) return "";
    char b[32];
    snprintf(b, sizeof b, f, v);
    return b;
}

bool is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct LogFile {
    std::string path, folder, date;   // folder "" = an old combined file in <dir>
    Kind kind = Kind::NONE;
};

// every file of ours in dir (only_date "" = all days): <dir>/<name>/<name>-D.jsonl[.gz[.part]] in the
// folders whose name the writer could have made, and the old <dir>/decoders-D.jsonl[.gz[.part]].
// Anything else in the folder is never listed - so never touched
std::vector<LogFile> list_files(const std::string& dir, const std::string& only_date = "") {
    std::vector<LogFile> v;
    std::vector<std::string> subs;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.') continue;
            Kind k;
            const std::string date = log_file_date(COMBINED, name, &k);
            if (!date.empty()) {
                if (only_date.empty() || date == only_date) v.push_back({dir + "/" + name, "", date, k});
            } else if (name == TRANSCRIPTS || (name != CLIPS && safe_folder_name(name) == name)) {
                subs.push_back(name);
            }
        }
        closedir(d);
    }
    for (const auto& sub : subs) {
        const std::string sp = dir + "/" + sub;
        if (!is_dir(sp)) continue;
        if (DIR* d = opendir(sp.c_str())) {
            while (dirent* e = readdir(d)) {
                Kind k;
                const std::string date = log_file_date(sub, e->d_name, &k);
                if (!date.empty() && (only_date.empty() || date == only_date)) v.push_back({sp + "/" + e->d_name, sub, date, k});
            }
            closedir(d);
        }
    }
    return v;
}

struct OpenFile {
    FILE* f = nullptr;
    std::string day, path;
    int64_t last_ms = 0;   // last written (idle files are closed)
};

struct State {
    // settings + the buffers (record handler, commands, status)
    std::mutex mu;
    bool enabled = false;   // "Allow logging to disk"
    // DECODER_LOG_TYPES as last set: only T_VOICE (the transcripts' Logging)
    // decides anything; T_RAW migrates the VFOs stored before the per-VFO
    // logging mode (legacy_raw); the rest is kept for the status / an older page
    unsigned types = T_EVENT | T_MESSAGE | T_POSITION | T_INCIDENT | T_VOICE;
    int days = 7;       // Keep: plain days (0 = forever)
    int zip_days = 30;  // Then compressed: gzip days after those (0 = none)
    // DECODER_LOG_POS_S: the old global position interval - what a VFO stored
    // without its own pos_s gets (control_handler's DECIMATORS replay)
    int pos_s = 10;
    std::string dir = DEFAULT_DIR;
    std::map<std::string, std::string> bufs;   // folder name -> its records not yet written
    size_t buf_bytes = 0;                       // all of bufs (the MAX_BUFFER cap is for all together)
    uint64_t dropped = 0;
    std::unordered_map<std::string, int64_t> last_pos;   // vfo|plugin|id -> last logged
    std::string error;
    // the files (the worker; flush / set_dir / stop with the io lock)
    std::mutex io;
    std::map<std::string, OpenFile> open;   // folder name -> its file of the day
    int64_t idle_ms = IDLE_CLOSE_MS;
    std::atomic<int> open_n{0};   // files open now (the status; the worker counts them)
    // status cache (worker)
    uint64_t today_bytes = 0;     // on disk, today's files
    int64_t since_ms = 0;         // logging (today) since - for the per-day estimate
    int files = 0;
    uint64_t total_bytes = 0;
    int gz_files = 0;             // gz files measured for gz_ratio
    double gz_ratio = 0;          // their uncompressed / compressed size (0 = none)
    int compressing = 0;
    int64_t retention_ms = 0, scan_ms = 0;
    std::atomic<bool> rescan{false};
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> isize_cache;   // gz path -> (size, isize)
};
State& S() {
    static State* s = new State;   // never destroyed: the worker may run while the process exits
    return *s;
}

// mu held
void append_locked(State& s, const std::string& folder, const std::string& line) {
    if (s.buf_bytes + line.size() + 1 > MAX_BUFFER) { s.dropped++; return; }
    std::string& b = s.bufs[folder];
    b += line;
    b += '\n';
    s.buf_bytes += line.size() + 1;
}

// t_ms: the record's time (0 = now)
std::string head(int vfo, double rf_hz, const std::string& plugin, const char* type, int64_t t_ms = 0) {
    std::string h = "{\"t\":\"" + iso_time(t_ms > 0 ? t_ms : now_ms()) + "\",\"vfo\":" + std::to_string(vfo);
    if (rf_hz > 0) h += ",\"mhz\":" + num(rf_hz / 1e6, "%.6f");
    return h + ",\"dec\":\"" + json_escape(plugin) + "\",\"type\":\"" + type + "\"";
}

void close_file(OpenFile& of) {
    if (of.f) fclose(of.f);
    of.f = nullptr;
}

void close_all_locked(State& s) {
    for (auto& kv : s.open) close_file(kv.second);
    s.open.clear();
    s.open_n = 0;
}

// write data to folder's file of `day` (opened as needed); io lock held
bool write_locked(State& s, const std::string& dir, const std::string& folder, const std::string& day,
                  const std::string& data, std::string* err) {
    OpenFile& of = s.open[folder];
    if (!of.f || of.day != day) {
        close_file(of);
        const std::string fd = dir + "/" + folder;
        if (!mkdirs(fd)) { *err = "cannot create the folder " + fd; return false; }
        of.path = fd + "/" + log_file_name(folder, day, Kind::PLAIN);
        of.f = fopen(of.path.c_str(), "ab");
        if (!of.f) { *err = "cannot write " + of.path + ": " + strerror(errno); return false; }
        of.day = day;
    }
    if (fwrite(data.data(), 1, data.size(), of.f) != data.size() || fflush(of.f) != 0) {
        *err = "writing " + of.path + " failed: " + strerror(errno);
        close_file(of);
        return false;
    }
    of.last_ms = now_ms();
    return true;
}

// the buffers -> their files of `day` (worker / set_dir / stop / flush), one append + fflush per
// file; io lock held
void flush_locked(State& s, const std::string& day) {
    std::map<std::string, std::string> bufs;
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        bufs.swap(s.bufs);
        s.buf_bytes = 0;
        dir = s.dir;
    }
    if (bufs.empty()) return;
    uint64_t fr = 0, tot = 0;
    std::string err;
    std::map<std::string, std::string> left;   // not written: kept for the next try
    if (disk_space(dir, &fr, &tot) && fr < static_cast<uint64_t>(MIN_FREE_MB) << 20) {
        err = "less than " + std::to_string(MIN_FREE_MB) + " MB free on the disk - not writing";
        left.swap(bufs);
    } else {
        for (auto& kv : bufs) {
            std::string e;
            if (!write_locked(s, dir, kv.first, day, kv.second, &e)) {
                if (err.empty()) err = e;
                left[kv.first].swap(kv.second);
            }
        }
    }
    std::lock_guard<std::mutex> lk(s.mu);
    if (left.empty()) {
        if (s.error.rfind("writing", 0) == 0 || s.error.rfind("cannot", 0) == 0 || s.error.rfind("less than", 0) == 0)
            s.error.clear();
        return;
    }
    if (s.error != err) std::cerr << "Decoder log: " << err << std::endl;
    s.error = err;
    // (up to the buffer limit, ahead of what came meanwhile)
    for (auto& kv : left) {
        std::string& b = s.bufs[kv.first];
        if (kv.second.size() + s.buf_bytes <= MAX_BUFFER) {
            s.buf_bytes += kv.second.size();
            b.insert(0, kv.second);
        } else {
            s.dropped += static_cast<uint64_t>(std::count(kv.second.begin(), kv.second.end(), '\n'));
        }
        if (b.empty()) s.bufs.erase(kv.first);
    }
}

// The retention pass (background, one at a time - logret::Scheduler): each finished day of each
// file by its age - plain for `days`, gzip for `zip_days` more, then deleted (retention_action).
// Deletions first, then the compressions one by one (logret::run_pass, the rule every daily log
// shares)
void run_pass(const std::string& dir, int keep, int zip, bool deletions) {
    const long today = day_number(local_date(time(nullptr)));
    std::vector<logret::File> files;
    for (const auto& f : list_files(dir)) files.push_back({f.path, day_number(f.date), f.kind});
    State& s = S();
    logret::run_pass(files, today, keep, zip, "Decoder log", [&s](int n) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.compressing += n;
    }, deletions);
    s.rescan = true;
}

// the deletion grace after a settings change: 10 min, as the telemetry logs'
// (KRAKEN_LOGRET_GRACE_S shortens it for tests - the same variable)
std::chrono::milliseconds delete_grace() {
    const char* e = getenv("KRAKEN_LOGRET_GRACE_S");
    const int v = e ? atoi(e) : 0;
    return std::chrono::seconds(v >= 1 && v <= 3600 ? v : 600);
}

// a pass with the settings as they are now: at startup (once the settings are replayed), the day
// change and every 10 min; a Keep / Then compressed / folder change a few seconds after the last
// one (a replay sets Keep and Then compressed one after the other - logret::Scheduler). After a
// change the passes only compress; what the new rule deletes is deleted 10 min after the LAST
// change (the grace, as the telemetry logs': a person setting Keep and then Then compressed, or
// two separate commands, must not get the deletions of the rule in between). A request during a
// pass runs another after it; none before settings_restored() (the stored Keep days), none
// during a debug playback (its settings are a sandbox - the stored ones are replayed first)
logret::Scheduler& passes() {
    static logret::Scheduler* p = new logret::Scheduler([](bool deletions) {   // never destroyed (a pass may run at exit)
        State& st = S();
        std::string dir;
        int keep, zip;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            dir = st.dir;
            keep = st.days;
            zip = st.zip_days;
        }
        run_pass(dir, keep, zip, deletions);
    }, [] { return playback::active(); }, logret::Scheduler::DEBOUNCE, delete_grace());
    return *p;
}
void request_pass() { passes().request(); }
// a Keep / Then compressed / folder change: the debounced pass and the deletion grace - not for
// the startup replay (before settings_restored() no pass runs, and the first one applies the
// stored values as they are, like the telemetry logs' stored settings): a receiver that never
// runs 10 min would otherwise never delete a day
void setting_changed() {
    if (passes().allowed()) passes().request_after_change();
}

void worker() {
    State& s = S();
    int64_t last_flush = now_ms();
    std::string last_day = local_date(time(nullptr));
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int64_t t = now_ms();
        bool enabled;
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            enabled = s.enabled;
            dir = s.dir;
        }
        const std::string today = local_date(time(nullptr));
        std::lock_guard<std::mutex> io(s.io);
        // midnight: the rest of yesterday into yesterday's files, they are closed, then the pass
        // (yesterday stays plain while Keep >= 1; the older days move on by one)
        if (today != last_day) {
            flush_locked(s, last_day);
            close_all_locked(s);
            last_day = today;
            {
                std::lock_guard<std::mutex> lk(s.mu);
                s.since_ms = 0;
            }
            s.retention_ms = t;
            request_pass();
            s.scan_ms = 0;
        }
        if (t - last_flush >= FLUSH_S * 1000) {
            last_flush = t;
            flush_locked(s, today);
            if (!enabled) {
                close_all_locked(s);
            } else {
                // an idle decoder holds no file open
                for (auto it = s.open.begin(); it != s.open.end();) {
                    if (!it->second.f || t - it->second.last_ms >= s.idle_ms) {
                        close_file(it->second);
                        it = s.open.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        {
            int n = 0;
            for (const auto& kv : s.open) n += kv.second.f != nullptr;
            s.open_n = n;
        }
        if (t - s.retention_ms >= 600000) {
            s.retention_ms = t;
            request_pass();
        }
        // status: today's size, all logs, the measured gzip ratio
        if (s.rescan.exchange(false)) s.scan_ms = 0;
        if (t - s.scan_ms >= 15000) {
            s.scan_ms = t;
            int files = 0, gzn = 0;
            uint64_t total = 0, today_b = 0, gz_c = 0, gz_u = 0;
            std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> cache;
            for (const auto& lf : list_files(dir)) {
                if (lf.kind != Kind::PLAIN && lf.kind != Kind::GZ) continue;
                struct stat st{};
                if (stat(lf.path.c_str(), &st) != 0) continue;
                const uint64_t sz = static_cast<uint64_t>(st.st_size);
                files++;
                total += sz;
                // (today's old combined file is no longer written to: not today's growth)
                if (lf.date == today && lf.kind == Kind::PLAIN && !lf.folder.empty()) today_b += sz;
                if (lf.kind == Kind::GZ && sz > 0) {
                    auto c = s.isize_cache.find(lf.path);
                    const uint64_t u = c != s.isize_cache.end() && c->second.first == sz ? c->second.second : gz_isize(lf.path);
                    cache[lf.path] = {sz, u};
                    if (u >= sz) {   // (an ISIZE that wrapped at 4 GiB, or a damaged file: not measured)
                        gzn++;
                        gz_c += sz;
                        gz_u += u;
                    }
                }
            }
            s.isize_cache.swap(cache);
            std::lock_guard<std::mutex> lk(s.mu);
            s.files = files;
            s.total_bytes = total;
            s.today_bytes = today_b;
            s.gz_files = gzn;
            s.gz_ratio = gz_c ? static_cast<double>(gz_u) / static_cast<double>(gz_c) : 0;
        }
    }
}

}  // namespace

void start() {
    if (const char* e = getenv("KRAKEN_DECLOG_IDLE_S")) {   // tests: close idle files sooner
        const int v = atoi(e);
        if (v >= 1 && v <= 3600) S().idle_ms = static_cast<int64_t>(v) * 1000;
    }
    dig::set_record_handler([](dig::DigitalDecoder* d, const dig::LogRecord& r) {
        if (playback::active()) return;   // debug playback: test transmissions aren't logged
        State& s = S();
        unsigned bit = 0;
        for (const auto& tn : TYPE_NAMES)
            if (!strcmp(tn.first, r.type)) bit = tn.second;
        // the VFO decoder's Logging decides: Off nothing, Log what the decoder reports (the
        // repeated messages only while its "Show all messages" is on), Log All also raw frames
        // (and the decoder then reports the repeated messages - digital_decoder.cpp)
        const int mode = d ? d->log_mode() : dig::LOG_OFF;
        if (mode == dig::LOG_OFF || !bit || (bit == T_RAW && mode != dig::LOG_ALL)) return;
        const int vfo = d ? d->vfo() : -1;
        const int pos_s = d ? d->pos_interval_s() : 0;
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.enabled) return;
        std::string line = head(vfo, r.rf_hz, r.plugin, r.type);
        if (bit == T_POSITION && r.point) {
            const dig::MapPoint& p = *r.point;
            // at most one per object every pos_s - the VFO decoder's "Positions every"
            const int64_t t = now_ms();
            int64_t& last = s.last_pos[std::to_string(vfo) + "|" + r.plugin + "|" + p.id];
            if (t - last < static_cast<int64_t>(pos_s > 0 ? pos_s : s.pos_s) * 1000) return;
            last = t;
            if (s.last_pos.size() > 20000) s.last_pos.clear();
            line += ",\"id\":\"" + json_escape(p.id) + "\",\"label\":\"" + json_escape(p.label) + "\",\"kind\":\"" +
                    json_escape(p.kind) + "\",\"lat\":" + num(p.lat, "%.6f") + ",\"lon\":" + num(p.lon, "%.6f");
            const std::string a = num(p.alt_m, "%.0f"), k = num(p.speed_kmh, "%.1f"), h = num(p.heading, "%.1f");
            if (!a.empty()) line += ",\"alt_m\":" + a;
            if (!k.empty()) line += ",\"kmh\":" + k;
            if (!h.empty()) line += ",\"hdg\":" + h;
            // the decoder's details ("Key: value" lines of the map popup -
            // ADS-B: squawk, vertical rate, IAS, emergency, category...)
            std::string info;
            std::stringstream is(p.info);
            std::string il;
            while (std::getline(is, il)) {
                const size_t c = il.find(": ");
                if (c == std::string::npos || c == 0) continue;
                info += std::string(info.empty() ? "" : ",") + "\"" + json_escape(il.substr(0, c)) + "\":\"" +
                        json_escape(il.substr(c + 2)) + "\"";
            }
            if (!info.empty()) line += ",\"info\":{" + info + "}";
        } else if (bit == T_MESSAGE) {
            line += ",\"from\":\"" + json_escape(r.from) + "\",\"text\":\"" + json_escape(r.text) + "\"";
        } else if (bit == T_RAW) {
            line += ",\"data\":\"" + json_escape(r.text) + "\"";
        } else {
            line += ",\"text\":\"" + json_escape(r.text) + "\"";
        }
        append_locked(s, safe_folder_name(r.plugin), line + "}");
    });
    std::thread(worker).detach();
}

void stop() {
    State& s = S();
    std::lock_guard<std::mutex> io(s.io);
    flush_locked(s, local_date(time(nullptr)));
    close_all_locked(s);
}

void flush() {
    State& s = S();
    std::lock_guard<std::mutex> io(s.io);
    flush_locked(s, local_date(time(nullptr)));
}

void settings_restored() {
    passes().allow();
}

void set_enabled(bool on) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (on && !s.enabled) s.since_ms = 0;
    s.enabled = on;
    dig::set_log_allowed(on);   // Log All's raw frames / repeated messages
}

bool set_types(const std::string& csv) {
    unsigned t = 0;
    std::stringstream ss(csv);
    std::string x;
    while (std::getline(ss, x, ',')) {
        if (x.empty()) continue;
        bool known = false;
        for (const auto& tn : TYPE_NAMES)
            if (x == tn.first) { t |= tn.second; known = true; }
        if (!known) return false;
    }
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.types = t;
    return true;
}

bool legacy_raw() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return (s.types & T_RAW) != 0;
}

void set_days(int days) {
    State& s = S();
    bool changed;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        changed = s.days != std::clamp(days, 0, 3650);
        s.days = std::clamp(days, 0, 3650);
    }
    if (changed) setting_changed();
}

void set_zip_days(int days) {
    State& s = S();
    bool changed;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        changed = s.zip_days != std::clamp(days, 0, 3650);
        s.zip_days = std::clamp(days, 0, 3650);
    }
    if (changed) setting_changed();
}

void set_pos_interval(int seconds) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.pos_s = std::clamp(seconds, 1, 3600);
}

int pos_interval() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.pos_s;
}

bool set_dir(const std::string& path_in, std::string* err) {
#ifdef _WIN32
    // one spelling per folder ("E:\logs", "E:/logs/" -> "E:/logs"), and the
    // characters no Windows file name can hold (':' only after a drive letter)
    const std::string path = fs_normalize_dir(path_in);
    if (path.find_first_of("*?\"<>|") != std::string::npos || (path.find(':') != std::string::npos && path.find(':') != 1) ||
        path.find(':', 2) != std::string::npos) {
        *err = "Windows folder names can't hold * ? \" < > | :";
        return false;
    }
    // a share: Windows would sign in to the host a page names, as this user
    // (fs_compat.hpp). The settings replay too: a stored share leaves the
    // default folder
    if (fs_is_network_path(path)) {
        const char* e = getenv("KRAKEN_ALLOW_NETWORK_DIRS");
        if (!e || std::string(e) != "1") {
            std::cerr << "Decoder log: folder " << path << " refused - a network share" << std::endl;
            *err = "network shares (\\\\server\\share) are refused - map the share to a drive letter, "
                   "or start kraken_doa with KRAKEN_ALLOW_NETWORK_DIRS=1";
            return false;
        }
    }
#else
    const std::string& path = path_in;
#endif
    if (path.empty() || path.size() > 255) { *err = "folder name empty or too long"; return false; }
    State& s = S();
    {
        // the settings replay at startup: no folder is created before logging is on
        std::lock_guard<std::mutex> lk(s.mu);
        if (path == s.dir) return true;
    }
    for (unsigned char c : path)
        if (c < 0x20 || c == 0x7F) { *err = "control character in the folder name"; return false; }
    if (!mkdirs(path)) { *err = "cannot create the folder " + path; return false; }
    const std::string probe = path + "/.kraken_write_test";
    FILE* f = fopen(probe.c_str(), "wb");
    if (!f) { *err = "cannot write to " + path + ": " + strerror(errno); return false; }
    fclose(f);
    unlink(probe.c_str());
    {
        std::lock_guard<std::mutex> io(s.io);
        flush_locked(s, local_date(time(nullptr)));   // what is buffered still goes to the old folder
        close_all_locked(s);
        std::lock_guard<std::mutex> lk(s.mu);
        s.dir = path;
        s.error.clear();
        s.scan_ms = 0;
    }
    setting_changed();
    return true;
}

void record_incident(int vfo, int log_mode, const std::string& plugin, const std::string& address, double lat,
                     double lon, const std::string& precision, const std::string& confidence, const std::string& text) {
    if (playback::active()) return;
    if (log_mode == dig::LOG_OFF) return;   // the source VFO's Logging decides (Log / Log All)
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.enabled) return;
    // (in the file of the decoder whose message it was)
    append_locked(s, safe_folder_name(plugin),
                  head(vfo, 0, plugin, "incident") + ",\"address\":\"" + json_escape(address) + "\",\"lat\":" +
                      num(lat, "%.6f") + ",\"lon\":" + num(lon, "%.6f") + ",\"precision\":\"" +
                      json_escape(precision) + "\",\"confidence\":\"" + json_escape(confidence) +
                      "\",\"text\":\"" + json_escape(text) + "\"}");
}

void record_voice(int vfo, double rf_hz, int64_t t_ms, const std::string& source, const std::string& fields) {
    if (playback::active()) return;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.enabled || !(s.types & T_VOICE)) return;
    // every transcript in one folder, whichever decoder / demod it came from
    append_locked(s, TRANSCRIPTS, head(vfo, rf_hz, source, "voice", t_ms) + fields + "}");
}

std::vector<DayFile> day_files(const std::string& dir, const std::string& date) {
    std::vector<DayFile> out;
    for (const auto& f : list_files(dir, date))
        if (f.kind == Kind::PLAIN || f.kind == Kind::GZ) out.push_back({f.path, f.folder, f.kind == Kind::GZ});
    std::sort(out.begin(), out.end(), [](const DayFile& a, const DayFile& b) { return a.path < b.path; });
    return out;
}

std::string folder() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.dir;
}

int keep_days() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.days;
}

bool voice_logged() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.enabled && (s.types & T_VOICE);
}

std::string status_message() {
    State& s = S();
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        dir = s.dir;
    }
    uint64_t fr = 0, tot = 0;
    const bool have_space = disk_space(dir, &fr, &tot);
    const std::string dir_abs = absolute(dir);
    // drives to offer: / and what is mounted under /media, /mnt, /run/media, /srv
    std::string mount_of_dir;
    const std::string mounts = diskutil::mounts_json(dir_abs, &mount_of_dir) + ",\"mount\":\"" + json_escape(mount_of_dir) + "\"";
    std::lock_guard<std::mutex> lk(s.mu);
    std::string types;
    for (const auto& tn : TYPE_NAMES)
        if (s.types & tn.second) types += (types.empty() ? "" : ",") + std::string(tn.first);
    // per-day estimate from today's growth while logging
    const int64_t t = now_ms();
    time_t tt = static_cast<time_t>(t / 1000);
    struct tm tm{};
    localtime_r(&tt, &tm);
    const int64_t since_midnight = (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) * 1000LL;
    if (s.enabled && s.since_ms == 0) s.since_ms = t;
    const uint64_t today_b = s.today_bytes + s.buf_bytes;
    const int64_t elapsed = std::min<int64_t>(since_midnight, t - (s.since_ms ? s.since_ms : t));
    const double per_day = s.enabled && elapsed > 60000 ? static_cast<double>(today_b) / elapsed * 86400000.0 : 0;
    // what the log holds at steady state with these settings: today + Keep plain days + Then
    // compressed gzip days (the measured ratio, else the assumed ~6x); 0 = no end (Keep 0)
    const double ratio = s.gz_ratio > 1 ? s.gz_ratio : GZ_RATIO_ASSUMED;
    const double steady = s.days > 0 ? per_day * (1 + s.days) + per_day / ratio * s.zip_days : 0;
    std::ostringstream o;
    // voice: the transcripts' Logging (a page that finds it knows the VFO decoders have a
    // Logging of their own - an older receiver filtered by the types instead). zip_days /
    // pos_per_vfo: a page that finds them shows "Then compressed" and the per-decoder
    // "Positions every" (an older receiver: the global field)
    o << "{\"declog\":{\"enabled\":" << (s.enabled ? "true" : "false") << ",\"types\":\"" << types
      << "\",\"voice\":" << ((s.types & T_VOICE) ? "true" : "false") << ",\"days\":" << s.days
      << ",\"zip_days\":" << s.zip_days << ",\"pos_s\":" << s.pos_s << ",\"pos_per_vfo\":true,\"flush_s\":" << FLUSH_S
      << ",\"layout\":\"folders\",\"dir\":\"" << json_escape(s.dir) << "\",\"dir_abs\":\"" << json_escape(dir_abs)
      << "\",\"free\":" << (have_space ? fr : 0) << ",\"total\":" << (have_space ? tot : 0) << ",\"today\":\""
      << local_date(tt) << "\",\"today_bytes\":" << today_b << ",\"per_day\":" << static_cast<uint64_t>(per_day)
      << ",\"steady_bytes\":" << static_cast<uint64_t>(steady) << ",\"gz_ratio\":" << num(s.gz_ratio, "%.2f")
      << ",\"gz_measured\":" << s.gz_files << ",\"files\":" << s.files << ",\"total_bytes\":" << s.total_bytes + s.buf_bytes
      << ",\"buffered\":" << s.buf_bytes << ",\"dropped\":" << s.dropped << ",\"compressing\":" << s.compressing
      << ",\"open_files\":" << s.open_n.load() << ",\"error\":\"" << json_escape(s.error) << "\",\"mounts\":" << mounts << "}}";
    return o.str();
}

}  // namespace declog
