#include "decoder_log.hpp"

#include "digital/digital_decoder.hpp"
#include "globals.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
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

enum : unsigned { T_EVENT = 1, T_MESSAGE = 2, T_POSITION = 4, T_RAW = 8, T_INCIDENT = 16 };
const std::pair<const char*, unsigned> TYPE_NAMES[] = {
    {"event", T_EVENT}, {"message", T_MESSAGE}, {"position", T_POSITION}, {"raw", T_RAW}, {"incident", T_INCIDENT}};
constexpr size_t MAX_BUFFER = 32u << 20;   // records kept while the disk can't be written (then dropped)

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

// days since 1970-01-01 of a civil date (no time zone involved)
long day_number(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
long day_number(const std::string& ymd) {
    int y, m, d;
    return sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) == 3 ? day_number(y, m, d) : -1;
}

// decoders-YYYY-MM-DD.jsonl[.gz] -> its date, "" for any other name
std::string log_date(const std::string& name, bool* gz) {
    static const char pre[] = "decoders-";
    if (name.size() < 25 || name.compare(0, 9, pre) != 0) return "";
    const std::string d = name.substr(9, 10);
    for (size_t i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? d[i] != '-' : (d[i] < '0' || d[i] > '9')) return "";
    const std::string rest = name.substr(19);
    if (rest == ".jsonl") { *gz = false; return d; }
    if (rest == ".jsonl.gz") { *gz = true; return d; }
    return "";
}

bool mkdirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        cur += "/";
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string absolute(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return p;
    return std::string(cwd) + "/" + p;
}

// free / total bytes of the file system holding path (or its nearest existing parent)
bool disk_space(std::string path, uint64_t* free_b, uint64_t* total_b) {
    path = absolute(path);
    for (int i = 0; i < 64 && !path.empty(); i++) {
        struct statvfs v{};
        if (statvfs(path.c_str(), &v) == 0) {
            *free_b = static_cast<uint64_t>(v.f_bavail) * v.f_frsize;
            *total_b = static_cast<uint64_t>(v.f_blocks) * v.f_frsize;
            return true;
        }
        const size_t sl = path.rfind('/');
        path = sl == 0 ? "/" : (sl == std::string::npos ? "" : path.substr(0, sl));
    }
    return false;
}

// /proc/mounts escapes " " as \040
std::string unescape_mount(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            o += static_cast<char>(strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

// gzip file -> file.gz (then the original is removed); false keeps the original
bool gzip_file(const std::string& path) {
    FILE* in = fopen(path.c_str(), "rb");
    if (!in) return false;
    const std::string part = path + ".gz.part";
    gzFile out = gzopen(part.c_str(), "wb6");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        const size_t n = fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (gzwrite(out, buf.data(), static_cast<unsigned>(n)) != static_cast<int>(n)) { ok = false; break; }
    }
    fclose(in);
    ok = gzclose(out) == Z_OK && ok;
    if (!ok || rename(part.c_str(), (path + ".gz").c_str()) != 0) {
        unlink(part.c_str());
        return false;
    }
    unlink(path.c_str());
    return true;
}

std::string num(double v, const char* f) {
    if (!is_finite_value(v)) return "";
    char b[32];
    snprintf(b, sizeof b, f, v);
    return b;
}

struct State {
    // settings + the buffer (record handler, commands, status)
    std::mutex mu;
    bool enabled = false;
    unsigned types = T_EVENT | T_MESSAGE | T_POSITION | T_INCIDENT;
    int days = 7;
    int pos_s = 10;
    std::string dir = DEFAULT_DIR;
    std::string buf;
    uint64_t dropped = 0;
    std::unordered_map<std::string, int64_t> last_pos;   // vfo|plugin|id -> last logged
    std::string error;
    // the file (the worker; set_dir / stop with io lock)
    std::mutex io;
    FILE* f = nullptr;
    std::string file_day, file_path;
    // status cache (worker)
    uint64_t today_bytes = 0;     // on disk, today's file
    int64_t since_ms = 0;         // logging (today) since - for the per-day estimate
    int files = 0;
    uint64_t total_bytes = 0;
    int compressing = 0;
    int64_t retention_ms = 0, scan_ms = 0;
};
State& S() {
    static State* s = new State;   // never destroyed: the worker may run while the process exits
    return *s;
}

void append_locked(State& s, const std::string& line) {
    if (s.buf.size() + line.size() > MAX_BUFFER) { s.dropped++; return; }
    s.buf += line;
    s.buf += '\n';
}

std::string head(int vfo, double rf_hz, const std::string& plugin, const char* type) {
    std::string h = "{\"t\":\"" + iso_time(now_ms()) + "\",\"vfo\":" + std::to_string(vfo);
    if (rf_hz > 0) h += ",\"mhz\":" + num(rf_hz / 1e6, "%.6f");
    return h + ",\"dec\":\"" + json_escape(plugin) + "\",\"type\":\"" + type + "\"";
}

// the day's file names in dir: date -> gz?
std::vector<std::pair<std::string, bool>> list_logs(const std::string& dir) {
    std::vector<std::pair<std::string, bool>> v;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            bool gz = false;
            const std::string date = log_date(e->d_name, &gz);
            if (!date.empty()) v.emplace_back(date, gz);
        }
        closedir(d);
    }
    return v;
}

std::string path_of(const std::string& dir, const std::string& date, bool gz) {
    return dir + "/decoders-" + date + ".jsonl" + (gz ? ".gz" : "");
}

// gzip the finished days (a crash across midnight leaves one) - in the background
void compress_old(const std::string& dir) {
    const std::string today = local_date(time(nullptr));
    std::vector<std::string> todo;
    for (const auto& lf : list_logs(dir))
        if (!lf.second && lf.first < today) todo.push_back(path_of(dir, lf.first, false));
    if (todo.empty()) return;
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.compressing += static_cast<int>(todo.size());
    }
    std::thread([todo] {
        for (const auto& p : todo) {
            if (!gzip_file(p)) std::cerr << "Decoder log: cannot gzip " << p << std::endl;
            std::lock_guard<std::mutex> lk(S().mu);
            S().compressing--;
        }
    }).detach();
}

// delete the days older than `days` (only decoders-YYYY-MM-DD.jsonl[.gz])
void retention(const std::string& dir, int days) {
    if (days <= 0) return;
    const long today = day_number(local_date(time(nullptr)));
    for (const auto& lf : list_logs(dir)) {
        const long dn = day_number(lf.first);
        if (dn >= 0 && today - dn > days) {
            const std::string p = path_of(dir, lf.first, lf.second);
            if (unlink(p.c_str()) == 0) std::cout << "Decoder log: deleted " << p << " (older than " << days << " days)" << std::endl;
        }
    }
}

// write data to the day's file (opened as needed); io lock held
bool write_locked(State& s, const std::string& dir, const std::string& data, std::string* err) {
    const std::string today = local_date(time(nullptr));
    if (!s.f || s.file_day != today) {
        if (s.f) fclose(s.f);
        s.f = nullptr;
        if (!mkdirs(dir)) { *err = "cannot create the folder " + dir; return false; }
        s.file_path = path_of(dir, today, false);
        s.f = fopen(s.file_path.c_str(), "ab");
        if (!s.f) { *err = "cannot write " + s.file_path + ": " + strerror(errno); return false; }
        s.file_day = today;
    }
    if (fwrite(data.data(), 1, data.size(), s.f) != data.size() || fflush(s.f) != 0) {
        *err = "writing " + s.file_path + " failed: " + strerror(errno);
        fclose(s.f);
        s.f = nullptr;
        return false;
    }
    return true;
}

// buffer -> disk (worker / set_dir / stop); io lock held
void flush_locked(State& s) {
    std::string data, dir;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        data.swap(s.buf);
        dir = s.dir;
    }
    if (data.empty()) return;
    uint64_t fr = 0, tot = 0;
    std::string err;
    if (disk_space(dir, &fr, &tot) && fr < static_cast<uint64_t>(MIN_FREE_MB) << 20) {
        err = "less than " + std::to_string(MIN_FREE_MB) + " MB free on the disk - not writing";
    } else if (write_locked(s, dir, data, &err)) {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.error.rfind("writing", 0) == 0 || s.error.rfind("cannot", 0) == 0 || s.error.rfind("less than", 0) == 0)
            s.error.clear();
        return;
    }
    // not written: keep it for the next try (up to the buffer limit)
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.error != err) std::cerr << "Decoder log: " << err << std::endl;
    s.error = err;
    if (data.size() + s.buf.size() <= MAX_BUFFER) s.buf.insert(0, data);
    else s.dropped++;
}

void close_locked(State& s) {
    if (s.f) fclose(s.f);
    s.f = nullptr;
}

void worker() {
    State& s = S();
    int64_t last_flush = now_ms();
    std::string last_day = local_date(time(nullptr));
    {
        std::string dir;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            dir = s.dir;
        }
        compress_old(dir);
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const int64_t t = now_ms();
        bool enabled;
        std::string dir;
        int days;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            enabled = s.enabled;
            dir = s.dir;
            days = s.days;
        }
        const std::string today = local_date(time(nullptr));
        std::lock_guard<std::mutex> io(s.io);
        // midnight: the rest of yesterday into yesterday's file, then gzip it
        if (today != last_day) {
            if (s.f && s.file_day == last_day) {
                std::string data;
                {
                    std::lock_guard<std::mutex> lk(s.mu);
                    data.swap(s.buf);
                }
                if (!data.empty()) {
                    fwrite(data.data(), 1, data.size(), s.f);
                    fflush(s.f);
                }
            }
            close_locked(s);
            last_day = today;
            {
                std::lock_guard<std::mutex> lk(s.mu);
                s.since_ms = 0;
            }
            compress_old(dir);
            retention(dir, days);
            s.scan_ms = 0;
        }
        if (t - last_flush >= FLUSH_S * 1000) {
            last_flush = t;
            flush_locked(s);
            if (!enabled) close_locked(s);
        }
        if (t - s.retention_ms >= 600000) {
            s.retention_ms = t;
            retention(dir, days);
        }
        // status: today's size, all logs
        if (t - s.scan_ms >= 15000) {
            s.scan_ms = t;
            int files = 0;
            uint64_t total = 0, today_b = 0;
            for (const auto& lf : list_logs(dir)) {
                struct stat st{};
                if (stat(path_of(dir, lf.first, lf.second).c_str(), &st) != 0) continue;
                files++;
                total += static_cast<uint64_t>(st.st_size);
                if (lf.first == today && !lf.second) today_b = static_cast<uint64_t>(st.st_size);
            }
            std::lock_guard<std::mutex> lk(s.mu);
            s.files = files;
            s.total_bytes = total;
            s.today_bytes = today_b;
        }
    }
}

}  // namespace

void start() {
    dig::set_record_handler([](dig::DigitalDecoder* d, const dig::LogRecord& r) {
        State& s = S();
        unsigned bit = 0;
        for (const auto& tn : TYPE_NAMES)
            if (!strcmp(tn.first, r.type)) bit = tn.second;
        const int vfo = d ? d->vfo() : -1;
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.enabled || !(s.types & bit)) return;
        std::string line = head(vfo, r.rf_hz, r.plugin, r.type);
        if (bit == T_POSITION && r.point) {
            const dig::MapPoint& p = *r.point;
            // at most one per object every pos_s
            const int64_t t = now_ms();
            int64_t& last = s.last_pos[std::to_string(vfo) + "|" + r.plugin + "|" + p.id];
            if (t - last < static_cast<int64_t>(s.pos_s) * 1000) return;
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
        append_locked(s, line + "}");
    });
    std::thread(worker).detach();
}

void stop() {
    State& s = S();
    std::lock_guard<std::mutex> io(s.io);
    flush_locked(s);
    close_locked(s);
}

void set_enabled(bool on) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (on && !s.enabled) s.since_ms = 0;
    s.enabled = on;
    dig::set_raw_wanted(on && (s.types & T_RAW));
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
    dig::set_raw_wanted(s.enabled && (t & T_RAW));
    return true;
}

void set_days(int days) {
    State& s = S();
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.days = std::clamp(days, 0, 3650);
        dir = s.dir;
        days = s.days;
    }
    std::lock_guard<std::mutex> io(s.io);
    retention(dir, days);
    s.scan_ms = 0;
}

void set_pos_interval(int seconds) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.pos_s = std::clamp(seconds, 1, 3600);
}

bool set_dir(const std::string& path, std::string* err) {
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
    std::lock_guard<std::mutex> io(s.io);
    flush_locked(s);   // what is buffered still goes to the old folder
    close_locked(s);
    int days;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.dir = path;
        s.error.clear();
        days = s.days;
    }
    compress_old(path);
    retention(path, days);
    s.scan_ms = 0;
    return true;
}

void record_incident(int vfo, const std::string& plugin, const std::string& address, double lat, double lon,
                     const std::string& precision, const std::string& confidence, const std::string& text) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.enabled || !(s.types & T_INCIDENT)) return;
    append_locked(s, head(vfo, 0, plugin, "incident") + ",\"address\":\"" + json_escape(address) + "\",\"lat\":" +
                         num(lat, "%.6f") + ",\"lon\":" + num(lon, "%.6f") + ",\"precision\":\"" +
                         json_escape(precision) + "\",\"confidence\":\"" + json_escape(confidence) +
                         "\",\"text\":\"" + json_escape(text) + "\"}");
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
    std::ostringstream mounts;
    {
        struct Mount { std::string dev, mp, type; };
        std::vector<Mount> all;
        std::ifstream pm("/proc/mounts");
        std::string line, mount_of_dir = "/";
        while (std::getline(pm, line)) {
            std::istringstream ls(line);
            Mount m;
            ls >> m.dev >> m.mp >> m.type;
            m.mp = unescape_mount(m.mp);
            if (m.mp.empty()) continue;
            if (dir_abs.compare(0, m.mp.size(), m.mp) == 0 &&
                (dir_abs.size() == m.mp.size() || dir_abs[m.mp.size()] == '/' || m.mp == "/") &&
                m.mp.size() > mount_of_dir.size())
                mount_of_dir = m.mp;
            all.push_back(m);
        }
        bool first = true;
        std::vector<std::string> listed;
        mounts << "[";
        for (const auto& m : all) {
            // the folder's own drive is always listed
            const bool want = m.mp == mount_of_dir ||
                              ((m.mp == "/" || m.mp.rfind("/media/", 0) == 0 || m.mp.rfind("/mnt/", 0) == 0 ||
                                m.mp.rfind("/run/media/", 0) == 0 || m.mp.rfind("/srv", 0) == 0) &&
                               m.type != "tmpfs" && m.type != "devtmpfs" && m.type != "squashfs" && m.type != "overlay" &&
                               m.type != "autofs" && m.type != "proc" && m.type != "sysfs" &&
                               // statvfs on a dead network mount blocks (this runs on the uWS loop)
                               m.type.rfind("nfs", 0) != 0 && m.type != "cifs" && m.type != "smb3" &&
                               m.type.rfind("fuse.", 0) != 0);
            if (!want || std::find(listed.begin(), listed.end(), m.mp) != listed.end()) continue;
            listed.push_back(m.mp);
            uint64_t mf = 0, mt = 0;
            if (!disk_space(m.mp, &mf, &mt) || mt == 0) continue;
            mounts << (first ? "" : ",") << "{\"path\":\"" << json_escape(m.mp) << "\",\"dev\":\"" << json_escape(m.dev)
                   << "\",\"type\":\"" << json_escape(m.type) << "\",\"free\":" << mf << ",\"total\":" << mt << "}";
            first = false;
        }
        mounts << "],\"mount\":\"" << json_escape(mount_of_dir) << "\"";
    }
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
    const uint64_t today_b = s.today_bytes + s.buf.size();
    const int64_t elapsed = std::min<int64_t>(since_midnight, t - (s.since_ms ? s.since_ms : t));
    const double per_day = s.enabled && elapsed > 60000 ? static_cast<double>(today_b) / elapsed * 86400000.0 : 0;
    std::ostringstream o;
    o << "{\"declog\":{\"enabled\":" << (s.enabled ? "true" : "false") << ",\"types\":\"" << types
      << "\",\"days\":" << s.days << ",\"pos_s\":" << s.pos_s << ",\"flush_s\":" << FLUSH_S << ",\"dir\":\""
      << json_escape(s.dir) << "\",\"dir_abs\":\"" << json_escape(dir_abs) << "\",\"free\":" << (have_space ? fr : 0)
      << ",\"total\":" << (have_space ? tot : 0) << ",\"today\":\"" << local_date(tt) << "\",\"today_bytes\":" << today_b
      << ",\"per_day\":" << static_cast<uint64_t>(per_day) << ",\"files\":" << s.files << ",\"total_bytes\":"
      << s.total_bytes + s.buf.size() << ",\"buffered\":" << s.buf.size() << ",\"dropped\":" << s.dropped
      << ",\"compressing\":" << s.compressing << ",\"error\":\"" << json_escape(s.error) << "\",\"mounts\":"
      << mounts.str() << "}}";
    return o.str();
}

}  // namespace declog
