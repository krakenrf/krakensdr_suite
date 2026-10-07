#include "digital/dig_plugin.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "digital/dig_report.hpp"
#include "utils/json_escape.hpp"

extern char** environ;

namespace dig {

namespace {

int64_t file_mtime_ms(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}

bool is_exe(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

std::vector<char*> c_argv(const std::vector<std::string>& a) {
    std::vector<char*> v;
    for (const auto& s : a) v.push_back(const_cast<char*>(s.c_str()));
    v.push_back(nullptr);
    return v;
}

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

bool run_and_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out, int* exit_status) {
    signal(SIGPIPE, SIG_IGN);
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    auto av = c_argv(argv);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (rc != 0) {
        close(p[0]);
        return false;
    }
    out->clear();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) { timed_out = true; break; }
        pollfd pf{p[0], POLLIN, 0};
        int pr = poll(&pf, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { timed_out = pr == 0; break; }
        char buf[4096];
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n <= 0) break;
        if (out->size() < (1u << 20)) out->append(buf, static_cast<size_t>(n));
    }
    close(p[0]);
    if (timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (exit_status) *exit_status = st;
    return !timed_out && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

void PluginRegistry::scan() {
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
            pi.exe = pdir + "/build/decoder";
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
                        src_mtime = std::max(src_mtime, file_mtime_ms(pdir + "/" + n));
                }
                closedir(sd);
            }
            if (!is_exe(pi.exe)) {
                pi.error = "not built (run make in kraken_doa_v2)";
            } else {
                pi.mtime = file_mtime_ms(pi.exe);
                pi.stale = src_mtime > pi.mtime;
                std::string js;
                if (!run_and_capture({pi.exe, "--info"}, 3000, &js)) {
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
                    if (json_find(js, "fixed_freq_hz", v)) pi.fixed_freq_hz = std::max(0.0, atof(v.c_str()));
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
                    if (!(pi.sample_rate >= 1000 && pi.sample_rate <= 2.4e6)) {
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
          << ",\"fixed_freq_hz\":" << static_cast<long long>(p.fixed_freq_hz) << ",\"voice\":" << (p.voice ? "true" : "false")
          << ",\"messages\":" << (p.messages ? "true" : "false") << ",\"talkers\":" << (p.talkers ? "true" : "false")
          << ",\"options\":[";
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
    const std::string tool = PluginRegistry::dir() + "/lib/build/codecs";
    std::string out;
    if (!is_exe(tool) || !run_and_capture({tool}, 5000, &out))
        return "{\"codecs\":{\"checked\":false,\"error\":\"codec check not built (run make in kraken_doa_v2)\"}}";
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
    stop();
    signal(SIGPIPE, SIG_IGN);
    int in[2], out[2], er[2];
    if (pipe2(in, O_CLOEXEC) != 0) { *err = "pipe failed"; return false; }
    if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); *err = "pipe failed"; return false; }
    if (pipe2(er, O_CLOEXEC) != 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        *err = "pipe failed";
        return false;
    }
    fcntl(in[1], F_SETPIPE_SZ, 1 << 20);   // ~3 s of 38.4 kHz samples; best effort
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    std::vector<std::string> args{exe, "--serve"};
    auto av = c_argv(args);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    close(er[1]);
    if (rc != 0) {
        close(in[1]); close(out[0]); close(er[0]);
        *err = std::string("cannot start: ") + strerror(rc);
        return false;
    }
    pid_ = pid;
    in_fd_ = in[1];
    out_fd_ = out[0];
    err_fd_ = er[0];
    set_nonblock(in_fd_);
    set_nonblock(out_fd_);
    set_nonblock(err_fd_);
    wbuf_.clear();
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
    if (in_fd_ >= 0) close(in_fd_);   // EOF: the plugin exits on its own
    in_fd_ = -1;
}

void PluginProcess::stop() {
    request_stop();
    if (pid_ > 0) {
        int st;
        bool gone = false;
        for (int i = 0; i < 30 && !gone; i++) {
            if (waitpid(pid_, &st, WNOHANG) == pid_) gone = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!gone) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
        }
    }
    pid_ = -1;
    if (out_fd_ >= 0) close(out_fd_);
    if (err_fd_ >= 0) close(err_fd_);
    out_fd_ = err_fd_ = -1;
    wbuf_.clear();
}

bool PluginProcess::alive(int* status) {
    if (pid_ <= 0) {
        if (status) *status = exit_status_;
        return false;
    }
    int st = 0;
    if (waitpid(pid_, &st, WNOHANG) == pid_) {
        pid_ = -1;
        exit_status_ = st;
        if (status) *status = st;
        return false;
    }
    return true;
}

void PluginProcess::flush() {
    while (in_fd_ >= 0 && !wbuf_.empty()) {
        ssize_t w = write(in_fd_, wbuf_.data(), wbuf_.size());
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) wbuf_.clear();   // EPIPE: it died, alive() reports it
            return;
        }
        wbuf_.erase(0, static_cast<size_t>(w));
    }
}

bool PluginProcess::send(uint32_t type, const void* data, size_t len, size_t max_backlog) {
    if (in_fd_ < 0) return false;
    flush();
    if (type == 1 && wbuf_.size() > max_backlog) return false;   // samples: plugin too slow
    uint32_t hdr[2] = {type, static_cast<uint32_t>(len)};
    wbuf_.append(reinterpret_cast<const char*>(hdr), 8);
    if (len) wbuf_.append(static_cast<const char*>(data), len);
    flush();
    return true;
}

bool PluginProcess::read_stdout() {
    if (out_fd_ < 0) return false;
    char buf[65536];
    ssize_t n = read(out_fd_, buf, sizeof buf);
    if (n <= 0) return false;
    rbuf_.insert(rbuf_.end(), buf, buf + n);
    return rbuf_.size() < (4u << 20);
}

void PluginProcess::read_stderr() {
    if (err_fd_ < 0) return;
    char buf[65536];
    ssize_t n = read(err_fd_, buf, sizeof buf);
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

void PluginProcess::wait_io(int ms) {
    pollfd pf[2];
    int n = 0;
    if (out_fd_ >= 0) pf[n++] = {out_fd_, POLLIN, 0};
    if (in_fd_ >= 0 && !wbuf_.empty()) pf[n++] = {in_fd_, POLLOUT, 0};
    if (n) ::poll(pf, static_cast<nfds_t>(n), ms);
}

}  // namespace dig
