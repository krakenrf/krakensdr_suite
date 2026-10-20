#include "digital/dig_plugin.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "digital/dig_report.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"

namespace dig {

namespace {

// a regular file we may run (kproc: on Windows also the .exe extension -
// MinGW's access(X_OK) is true for any existing file)
bool is_exe(const std::string& path) { return kproc::is_executable(path); }

// `decoder --info` must answer within this. Windows: 10 s - the first start
// of a freshly built exe waits for the antivirus to scan it (seconds)
#ifdef _WIN32
constexpr int INFO_TIMEOUT_MS = 10000;
#else
constexpr int INFO_TIMEOUT_MS = 3000;
#endif

// How the user builds the plugins - the "not built" hints the web UI shows.
// Windows: build_windows.bat (make itself runs only in the MSYS2 UCRT64 shell)
#ifdef _WIN32
constexpr const char* BUILD_HINT = "run build_windows.bat";
#else
constexpr const char* BUILD_HINT = "run make in kraken_doa_v2";
#endif

#ifdef _WIN32
// Windows: kproc::python_argv() finds the interpreter by running candidates
// (py -3, python3.exe ...: seconds) the first time it is called. The startup
// scan resolves it on a thread of its own, so the first test-signal
// generation (PLAYBACK_GEN, on the uWS loop) doesn't wait for it there
std::once_flag g_python_warm;
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry r;
    return r;
}

std::string PluginRegistry::dir() {
    const char* e = std::getenv("KRAKEN_PLUGIN_DIR");
    return (e && *e) ? std::string(e) : std::string("plugins");
}

bool PluginRegistry::valid_id(const std::string& id) { return valid_plugin_id(id); }

void PluginRegistry::scan_async(std::function<void()> done) {
    std::thread([this, done = std::move(done)] {
        scan();
        if (done) done();
    }).detach();
}

// stdin /dev/null, stdout captured (first 1 MB), stderr /dev/null; killed at
// the deadline. A plugin is a leaf process (Windows: no console of its own)
bool run_and_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out,
                     kproc::ExitStatus* exit_status, bool search_path) {
    return kproc::run_capture(argv, timeout_ms, out, exit_status, 1u << 20, search_path, kproc::Console::None);
}

void PluginRegistry::scan() {
#ifdef _WIN32
    std::call_once(g_python_warm, [] { std::thread([] { (void)kproc::python_argv(); }).detach(); });
#endif
    std::lock_guard<std::mutex> scan_lk(scan_mu_);
    std::vector<PluginInfo> found;
    const std::string base = dir();
    if (DIR* d = opendir(base.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string id = e->d_name;
            if (!valid_id(id)) continue;
            const std::string pdir = base + "/" + id;
            struct stat st{};
            if (stat(pdir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            PluginInfo pi;
            pi.id = id;
            pi.name = id;
            pi.exe = kproc::exe_path(pdir + "/build/decoder");
            const bool has_src = access((pdir + "/decoder.cpp").c_str(), R_OK) == 0;
            if (!has_src && !is_exe(pi.exe)) continue;
            // newest source file
            int64_t src_mtime = 0;
            if (DIR* sd = opendir(pdir.c_str())) {
                while (dirent* f = readdir(sd)) {
                    std::string n = f->d_name;
                    auto ends = [&](const char* x) {
                        size_t l = strlen(x);
                        return n.size() > l && n.compare(n.size() - l, l, x) == 0;
                    };
                    if (ends(".cpp") || ends(".hpp") || ends(".h"))
                        src_mtime = std::max(src_mtime, fs_mtime_ms(pdir + "/" + n));
                }
                closedir(sd);
            }
            if (!is_exe(pi.exe)) {
                pi.error = std::string("not built (") + BUILD_HINT + ")";
            } else {
                pi.mtime = fs_mtime_ms(pi.exe);
                pi.stale = src_mtime > pi.mtime;
                std::string js;
                if (!run_and_capture({pi.exe, "--info"}, INFO_TIMEOUT_MS, &js)) {
                    pi.error = "--info failed (rebuild the plugin)";
                } else {
                    std::string v;
                    if (json_find(js, "id", v) && v != id) pi.error = "id mismatch: the source says \"" + v + "\"";
                    if (json_find(js, "name", v) && !v.empty()) pi.name = v;
                    if (json_find(js, "description", v)) pi.description = v;
                    if (json_find(js, "version", v)) pi.version = v;
                    if (json_find(js, "author", v)) pi.author = v;
                    if (json_find(js, "sample_rate", v)) pi.sample_rate = atof(v.c_str());
                    if (json_find(js, "min_vfo_rate", v)) pi.min_vfo_rate = atof(v.c_str());
                    if (json_find(js, "map", v)) pi.map = v == "true";
                    if (json_find(js, "manual_only", v)) pi.manual_only = v == "true";
                    if (json_find(js, "voice", v)) pi.voice = v == "true";
                    if (json_find(js, "messages", v)) pi.messages = v == "true";
                    if (json_find(js, "talkers", v)) pi.talkers = v == "true";
                    if (json_find(js, "array", v)) pi.array = v == "true";
                    if (json_find(js, "array_burst_s", v)) pi.array_burst_s = std::clamp(atof(v.c_str()), 0.01, 60.0);
                    if (json_find(js, "array_period_s", v)) pi.array_period_s = std::clamp(atof(v.c_str()), 0.01, 600.0);
                    if (pi.array) pi.manual_only = true;   // never in Auto detect (all antennas, wide VFOs)
                    if (json_find(js, "fixed_freq_hz", v)) pi.fixed_freq_hz = std::max(0.0, atof(v.c_str()));
                    if (json_find(js, "lock_vfo", v)) pi.lock_vfo = v == "true";
                    // "whole_band": the same lock, as cells / dtv builds from before the merge call it
                    if (json_find(js, "whole_band", v) && v == "true") pi.lock_vfo = true;
                    if (json_find(js, "multichannel", v)) pi.multichannel = v == "true";
                    if (json_find(js, "tunes", v)) pi.tunes = v == "true";
                    if (json_find(js, "squelch_hold_s", v)) pi.squelch_hold_s = std::clamp(atof(v.c_str()), 0.0, 10.0);
                    // options: one per line, tab-separated key/label/default/choices/help
                    if (json_find(js, "options_tsv", v)) {
                        std::stringstream ls(v);
                        std::string line;
                        while (std::getline(ls, line) && pi.options.size() < 16) {
                            std::vector<std::string> f;
                            std::stringstream fs(line);
                            std::string x;
                            while (std::getline(fs, x, '\t')) f.push_back(x);
                            f.resize(5);
                            bool ok = !f[0].empty() && f[0].size() <= 32;
                            for (char c : f[0])
                                ok &= (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
                            if (ok) pi.options.push_back({f[0], f[1], f[2], f[3], f[4]});
                        }
                    }
                    // 0 = the VFO's own rate (kp::Info::sample_rate)
                    if (!(pi.sample_rate == 0 || (pi.sample_rate >= 1000 && pi.sample_rate <= 2.4e6))) {
                        pi.error = "bad sample_rate";
                        pi.sample_rate = 48000;
                    }
                    pi.built = pi.error.empty();
                }
            }
            found.push_back(std::move(pi));
        }
        closedir(d);
    }
    std::sort(found.begin(), found.end(), [](const PluginInfo& a, const PluginInfo& b) { return a.id < b.id; });
    std::lock_guard<std::mutex> lk(mu_);
    auto key = [](const std::vector<PluginInfo>& l) {
        std::string k;
        for (const auto& p : l) k += p.id + "@" + std::to_string(p.mtime) + (p.built ? "+" : "-") + ";";
        return k;
    };
    if (key(found) != key(plugins_)) generation_++;
    plugins_ = std::move(found);
    scanned_ = true;
}

bool PluginRegistry::get(const std::string& id, PluginInfo* out) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : plugins_)
        if (p.id == id) {
            *out = p;
            out->auto_detect = !auto_off_.count(id);
            return true;
        }
    return false;
}

std::vector<PluginInfo> PluginRegistry::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PluginInfo> l = plugins_;
    for (auto& p : l) p.auto_detect = !auto_off_.count(p.id);
    return l;
}

bool PluginRegistry::set_auto(const std::string& id, bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    bool changed = on ? auto_off_.erase(id) > 0 : auto_off_.insert(id).second;
    if (changed) generation_++;
    return changed;
}

void PluginRegistry::set_auto_off(const std::string& comma_list) {
    std::set<std::string> off;
    std::stringstream ss(comma_list);
    std::string id;
    while (std::getline(ss, id, ','))
        if (valid_id(id)) off.insert(id);   // ids of plugins not installed here are kept
    std::lock_guard<std::mutex> lk(mu_);
    if (off != auto_off_) {
        auto_off_ = std::move(off);
        generation_++;
    }
}

std::string PluginRegistry::auto_off_list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string s;
    for (const auto& id : auto_off_) s += (s.empty() ? "" : ",") + id;
    return s;
}

std::string PluginRegistry::list_json() const {
    auto l = list();
    std::ostringstream o;
    o << "[";
    for (size_t i = 0; i < l.size(); i++) {
        const auto& p = l[i];
        o << (i ? "," : "") << "{\"id\":\"" << json_escape(p.id) << "\",\"name\":\"" << json_escape(p.name)
          << "\",\"description\":\"" << json_escape(p.description) << "\",\"version\":\"" << json_escape(p.version)
          << "\",\"author\":\"" << json_escape(p.author) << "\",\"sample_rate\":" << p.sample_rate
          << ",\"min_vfo_rate\":" << p.min_vfo_rate << ",\"auto\":" << (p.auto_detect ? "true" : "false")
          << ",\"map\":" << (p.map ? "true" : "false") << ",\"manual_only\":" << (p.manual_only ? "true" : "false")
          << ",\"fixed_freq_hz\":" << static_cast<long long>(p.fixed_freq_hz) << ",\"lock_vfo\":" << (p.lock_vfo ? "true" : "false")
          << ",\"voice\":" << (p.voice ? "true" : "false")
          << ",\"messages\":" << (p.messages ? "true" : "false") << ",\"talkers\":" << (p.talkers ? "true" : "false")
          << ",\"array\":" << (p.array ? "true" : "false") << ",\"tunes\":" << (p.tunes ? "true" : "false") << ",\"options\":[";
        for (size_t k = 0; k < p.options.size(); k++) {
            const auto& op = p.options[k];
            o << (k ? "," : "") << "{\"key\":\"" << json_escape(op.key) << "\",\"label\":\"" << json_escape(op.label)
              << "\",\"default\":\"" << json_escape(op.def) << "\",\"help\":\"" << json_escape(op.help)
              << "\",\"choices\":[";
            // "v=Label|v=Label" -> [["v","Label"],...]
            std::stringstream cs(op.choices);
            std::string c;
            bool fc = true;
            while (std::getline(cs, c, '|')) {
                if (c.empty()) continue;
                size_t eq = c.find('=');
                std::string val = c.substr(0, eq), lab = eq == std::string::npos ? c : c.substr(eq + 1);
                o << (fc ? "" : ",") << "[\"" << json_escape(val) << "\",\"" << json_escape(lab) << "\"]";
                fc = false;
            }
            o << "]}";
        }
        o << "],\"built\":" << (p.built ? "true" : "false")
          << ",\"stale\":" << (p.stale ? "true" : "false") << ",\"error\":\"" << json_escape(p.error) << "\"}";
    }
    o << "]";
    return o.str();
}

std::string codec_status_message() {
    const std::string tool = kproc::exe_path(PluginRegistry::dir() + "/lib/build/codecs");
    std::string out;
    if (!is_exe(tool) || !run_and_capture({tool}, 5000, &out))
        return std::string("{\"codecs\":{\"checked\":false,\"error\":\"codec check not built (") + BUILD_HINT + ")\"}}";
    std::ostringstream o;
    o << "{\"codecs\":{\"checked\":true";
    for (const char* k : {"imbe", "ambe", "acelp"}) {
        std::string ok, st;
        json_find(out, std::string(k) + "_ok", ok);
        json_find(out, k, st);
        o << ",\"" << k << "_ok\":" << (ok == "true" ? "true" : "false") << ",\"" << k << "\":\"" << json_escape(st) << "\"";
    }
    o << "}}";
    return o.str();
}

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
bool PluginProcess::start(const std::string& exe, std::string* err) {
    return start(std::vector<std::string>{exe, "--serve"}, err);
}

bool PluginProcess::start(const std::vector<std::string>& args, std::string* err) {
    if (args.empty()) { *err = "no command"; return false; }
    stop();
    kproc::SpawnOptions o;
    o.argv = args;
    o.in = o.out = o.err = kproc::Stdio::Pipe;   // our ends non-blocking
    // a bigger stdin pipe: ~3 s of 38.4 kHz samples, fewer and larger writes
    // (array plugins send ~17 MB bursts); best effort on Linux - the system
    // limit (pipe-max-size, 1 MB by default) rules
    o.in_pipe_bytes = 1u << 20;
    o.out_pipe_bytes = 1u << 20;   // Windows: a plugin bursting tables / plots doesn't block on 64 KB
    o.err_pipe_bytes = 64u << 10;
    o.console = kproc::Console::None;   // Windows: a leaf - no console host per plugin
    std::string e;
    if (!proc_.spawn(o, &e)) {
        *err = "cannot start: " + e;
        return false;
    }
    reaped_ = false;
    exit_status_ = kproc::ExitStatus{};
    wbuf_.clear();
    wpos_ = 0;
    rbuf_.clear();
    ebuf_.clear();
    proto_error_ = false;
    {
        std::lock_guard<std::mutex> lk(tail_mu_);
        tail_.clear();
    }
    return true;
}

void PluginProcess::request_stop() {
    proc_.close_stdin();   // EOF: the plugin exits on its own
}

void PluginProcess::stop() {
    request_stop();
    // up to 300 ms to exit by itself, then killed
    if (proc_.started() && !reaped_) {
        if (!proc_.wait_for(300, &exit_status_)) {
            proc_.kill();
            exit_status_ = proc_.wait();
        }
        reaped_ = true;
    }
    proc_.close();
    std::string().swap(wbuf_);   // its memory too (clear() keeps the capacity)
    wpos_ = 0;
}

// Reaped, the pipes stay open until stop(): poll() still reads what the
// plugin wrote before it exited
bool PluginProcess::alive(kproc::ExitStatus* status) {
    if (proc_.started() && !reaped_ && proc_.try_wait(&exit_status_)) reaped_ = true;
    if (!proc_.started() || reaped_) {
        if (status) *status = exit_status_;
        return false;
    }
    return true;
}

// Writes what the pipe takes. The written part is skipped by an offset and
// dropped in one go (when all is written, or once it is most of the buffer):
// erasing every write from the front moved the whole rest each time -
// quadratic in what is queued (an array plugin's burst queues ~17 MB, and
// the pipe takes 64 KB - 1 MB at a time).
void PluginProcess::flush() {
    while (proc_.stdin_open() && wpos_ < wbuf_.size()) {
        const ptrdiff_t w = proc_.write_some(wbuf_.data() + wpos_, wbuf_.size() - wpos_);
        if (w < 0) {   // EPIPE: it died, alive() reports it
            wbuf_.clear();
            wpos_ = 0;
            break;
        }
        if (w == 0) break;   // the pipe is full
        wpos_ += static_cast<size_t>(w);
    }
    if (wpos_ >= wbuf_.size()) {
        wbuf_.clear();
        wpos_ = 0;
    } else if (wpos_ > (1u << 20) && wpos_ > wbuf_.size() / 2) {
        wbuf_.erase(0, wpos_);
        wpos_ = 0;
    }
}

bool PluginProcess::send(uint32_t type, const void* data, size_t len, size_t max_backlog) {
    if (!proc_.stdin_open()) return false;
    flush();
    // samples (sent with a limit - SAMPLES, SAMPLES_MC, SAMPLES_ARRAY): dropped
    // while more than max_backlog bytes wait for the plugin. Only SAMPLES used
    // to be limited: an array plugin that fell behind queued its ~17 MB
    // bursts without end (kraken_doa grew to 2.3 GB)
    if (max_backlog && wbuf_.size() - wpos_ > max_backlog) return false;
    uint32_t hdr[2] = {type, static_cast<uint32_t>(len)};
    wbuf_.append(reinterpret_cast<const char*>(hdr), 8);
    if (len) wbuf_.append(static_cast<const char*>(data), len);
    flush();
    return true;
}

bool PluginProcess::read_stdout() {
    char buf[65536];
    const ptrdiff_t n = proc_.read_some(kproc::Stream::Out, buf, sizeof buf);
    if (n <= 0) return false;   // nothing now / EOF
    rbuf_.insert(rbuf_.end(), buf, buf + n);
    return rbuf_.size() < (4u << 20);
}

void PluginProcess::read_stderr() {
    char buf[65536];
    const ptrdiff_t n = proc_.read_some(kproc::Stream::Err, buf, sizeof buf);
    if (n <= 0) return;
    ebuf_.append(buf, static_cast<size_t>(n));
    size_t p;
    std::lock_guard<std::mutex> lk(tail_mu_);
    while ((p = ebuf_.find('\n')) != std::string::npos) {
        std::string line = ebuf_.substr(0, std::min<size_t>(p, 300));
        ebuf_.erase(0, p + 1);
        if (line.empty()) continue;
        tail_.push_back(line);
        if (tail_.size() > 30) tail_.pop_front();
    }
    if (ebuf_.size() > 4096) ebuf_.clear();
}

std::vector<std::string> PluginProcess::stderr_tail() const {
    std::lock_guard<std::mutex> lk(tail_mu_);
    return {tail_.begin(), tail_.end()};
}

void PluginProcess::memcpy_u32(uint32_t* d, const char* s) { std::memcpy(d, s, 4); }

void PluginProcess::wait_io(int ms) { proc_.wait_io(ms, true, !wbuf_.empty()); }

}  // namespace dig
