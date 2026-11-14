#include "ai_sandbox.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "digital/dig_plugin.hpp"
#include "geo_address.hpp"   // geo::json_parse
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"

namespace aisandbox {
namespace {

constexpr const char* LIST = "ai/state/sandboxed_plugins.json";
constexpr const char* TURN = "ai/state/turn";   // the running turn's start (ms)

std::mutex g_mu;
std::set<std::string> g_marked;
struct timespec g_list_mtime {};
bool g_loaded = false;
std::atomic<bool> g_turn{false};
std::atomic<int64_t> g_turn_start_ms{0};

#ifdef __APPLE__
// macOS: the kernel's sandbox (Seatbelt) through sandbox-exec, with a profile
// made per call (what Claude Code's own sandbox uses there). Deprecated as a
// command but present on every macOS release so far
constexpr const char* SANDBOX_EXEC = "/usr/bin/sandbox-exec";

// a string literal of the profile language (SBPL): "..." with \ and \" escaped
std::string sbpl_str(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

// bwrap's --tmpfs /tmp: a private temporary folder, one per kraken_doa run
// (made on first use, mode 0700), handed to the program as TMPDIR - the shared
// /tmp itself stays read-only
std::string private_tmp() {
    static const std::string d = [] {
        char t[] = "/private/tmp/kraken_sandbox.XXXXXX";
        return mkdtemp(t) ? std::string(t) : std::string();
    }();
    return d;
}

// everything readable, nothing writable but rw + the private TMPDIR + the
// usual devices, no network (Unix sockets included), no signals to other
// processes (bwrap: --unshare-net / --unshare-ipc; its own session there)
std::string sbpl_profile(const std::vector<std::string>& rw) {
    std::string p = "(version 1)\n(allow default)\n(deny network*)\n(deny file-write*)\n"
                    "(allow file-write* (literal \"/dev/null\") (literal \"/dev/zero\") (literal \"/dev/tty\")"
                    " (literal \"/dev/dtracehelper\") (regex #\"^/dev/fd/\"))\n";
    std::vector<std::string> dirs = rw;
    if (!private_tmp().empty()) dirs.push_back(private_tmp());
    for (const auto& d : dirs)
        if (!d.empty()) p += "(allow file-write* (subpath " + sbpl_str(d) + "))\n";
    p += "(deny signal)\n(allow signal (target self))\n(allow signal (target children))\n"
         "(deny ipc-posix-shm-write-create)\n";
    return p;
}
#elif !defined(_WIN32)
std::string bwrap_path() {
    static const std::string p = [] {
        for (const char* c : {"/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap"})
            if (access(c, X_OK) == 0) return std::string(c);
        return std::string();
    }();
    return p;
}
#endif

#ifdef _WIN32
// struct stat has no st_mtim / st_ctim on Windows: the mtime in ms (fs_compat)
struct timespec list_mtime(const struct stat&) {
    const int64_t ms = fs_mtime_ms(LIST);
    struct timespec t {};
    t.tv_sec = static_cast<time_t>(ms / 1000);
    t.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
    return t;
}
#else
struct timespec list_mtime(const struct stat& st) { return st.st_mtim; }
#endif

// the list, re-read when the file changed (`kraken_ai.py trust` edits it)
void load_locked() {
    struct stat st {};
    if (stat(LIST, &st) != 0) {
        if (!g_loaded) g_marked.clear();
        g_loaded = true;
        return;
    }
    const struct timespec mt = list_mtime(st);
    if (g_loaded && mt.tv_sec == g_list_mtime.tv_sec && mt.tv_nsec == g_list_mtime.tv_nsec) return;
    g_list_mtime = mt;
    g_loaded = true;
    std::ifstream f(LIST);
    std::stringstream ss;
    ss << f.rdbuf();
    geo::Json j;
    if (!geo::json_parse(ss.str(), &j) || j.type != geo::Json::OBJ) return;   // keep what we had
    std::set<std::string> m;
    if (const geo::Json* a = j.get("plugins"); a && a->type == geo::Json::ARR)
        for (const auto& e : a->a)
            if (e.type == geo::Json::STR && !e.s.empty()) m.insert(e.s);
    g_marked = std::move(m);
}

void save_locked() {
    mkdir("ai", 0755);
    mkdir("ai/state", 0755);
    std::string s = "{\"plugins\":[";
    bool first = true;
    for (const auto& id : g_marked) {
        s += (first ? "\"" : ",\"") + json_escape(id) + "\"";
        first = false;
    }
#ifdef _WIN32
    s += "],\"note\":\"plugins the AI assistant changed - kraken_doa doesn't run them (Windows has no sandbox). "
         "py -3 ai/kraken_ai.py trust <id> takes one off\"}\n";
#elif defined(__APPLE__)
    s += "],\"note\":\"plugins the AI assistant changed - kraken_doa runs them sandboxed (sandbox-exec). "
         "python3 ai/kraken_ai.py trust <id> takes one off\"}\n";
#else
    s += "],\"note\":\"plugins the AI assistant changed - kraken_doa runs them sandboxed (bwrap). "
         "python3 ai/kraken_ai.py trust <id> takes one off\"}\n";
#endif
    const std::string tmp = std::string(LIST) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << s;
    }
    if (fs_replace(tmp, LIST) == 0) {   // rename(); Windows: also over the old list
        struct stat st {};
        if (stat(LIST, &st) == 0) g_list_mtime = list_mtime(st);
    }
}

// any file / folder under dir (not following links) whose ctime is >= ms.
// Windows has no ctime (st_ctime is the creation time, which NTFS even puts
// back on a file re-created under the same name): the later of the
// modification and creation times, in seconds - the turn's 2 s of slack
// covers the rounding (and the assistant has no shell there to set one back)
bool changed_since(const std::string& dir, int64_t ms, int depth = 0) {
    if (depth > 6) return false;
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) return false;
#ifdef _WIN32
    if (static_cast<int64_t>(std::max(st.st_mtime, st.st_ctime)) * 1000 >= ms) return true;
#else
    if (static_cast<int64_t>(st.st_ctim.tv_sec) * 1000 + st.st_ctim.tv_nsec / 1000000 >= ms) return true;
#endif
    if (!S_ISDIR(st.st_mode)) return false;
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool hit = false;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (changed_since(dir + "/" + e->d_name, ms, depth + 1)) {
            hit = true;
            break;
        }
    }
    closedir(d);
    return hit;
}

#ifdef _WIN32
// Windows: the receiver's own plugin files changed since ms - sdk/, lib/ or a
// file directly in plugins/ (Makefile, *.mk ...). The agent's permission
// rules refuse every write there (Claude Code 2.1.304 refused case, 8.3,
// trailing dot / space, "..", and stream variants of those paths too - tested
// 2026-10-20) and it has no shell; should a rule ever be got round anyway,
// nothing would sandbox what the next build makes from them (turn_finished)
bool core_changed_since(const std::string& root, int64_t ms) {
    if (changed_since(root + "/sdk", ms) || changed_since(root + "/lib", ms)) return true;
    DIR* d = opendir(root.c_str());
    if (!d) return false;
    bool hit = false;
    while (dirent* e = readdir(d)) {
        const std::string p = root + "/" + e->d_name;
        struct stat st {};
        if (e->d_name[0] == '.' || lstat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
        if (changed_since(p, ms)) {
            hit = true;
            break;
        }
    }
    closedir(d);
    return hit;
}
#endif

std::string real(const std::string& p) {
    char b[PATH_MAX];
#ifdef _WIN32
    // '/'-separated: the paths are taken apart at '/' below (realpath = _fullpath, '\')
    return realpath(p.c_str(), b) ? fs_normalize_dir(b) : std::string();
#else
    return realpath(p.c_str(), b) ? std::string(b) : std::string();
#endif
}

}  // namespace

bool available() {
#ifdef _WIN32
    return false;   // no bwrap on Windows (NO_SANDBOX_FIX)
#elif defined(__APPLE__)
    static const bool ok = [] {
        if (access(SANDBOX_EXEC, X_OK) != 0) return false;
        std::string out;
        kproc::ExitStatus st;
        dig::run_and_capture({SANDBOX_EXEC, "-p", sbpl_profile({}), "/usr/bin/true"}, 5000, &out, &st);
        if (!st.ok()) std::cerr << "AI assistant: sandbox-exec does not work here - plugins it changed won't run" << std::endl;
        return st.ok();
    }();
    return ok;
#else
    static const bool ok = [] {
        const std::string b = bwrap_path();
        if (b.empty()) return false;
        std::string out;
        kproc::ExitStatus st;   // ok() = exited with 0 (the wait status 0); none if it couldn't start
        dig::run_and_capture({b, "--ro-bind", "/", "/", "--dev", "/dev", "--unshare-net", "--die-with-parent", "/bin/true"},
                             5000, &out, &st);
        if (!st.ok()) std::cerr << "AI assistant: bwrap does not work here - plugins it changed won't run" << std::endl;
        return st.ok();
    }();
    return ok;
#endif
}

bool must_sandbox_dir(const std::string& plugin_dir) {
    const std::string dir = real(plugin_dir);
    if (dir.empty()) return false;
    const std::string id = dir.substr(dir.rfind('/') + 1);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        load_locked();
        if (g_marked.count(id)) return true;
    }
    // during a turn: changed since it started (a mid-turn rebuild)
    return g_turn.load() && changed_since(dir, g_turn_start_ms.load());
}

bool must_sandbox_exe(const std::string& exe) {
    const std::string root = real(dig::PluginRegistry::dir()), e = real(exe);
    if (root.empty() || e.size() <= root.size() + 1 || e.compare(0, root.size() + 1, root + "/") != 0) return false;
    const size_t slash = e.find('/', root.size() + 1);
    if (slash == std::string::npos) return false;
    return must_sandbox_dir(e.substr(0, slash));
}

bool any_marked() {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    return !g_marked.empty() || g_turn.load();
}

void turn_started(int64_t wall_ms) {
    // a little slack: ctime has the filesystem's granularity
    g_turn_start_ms = wall_ms - 2000;
    g_turn = true;
    mkdir("ai", 0755);
    mkdir("ai/state", 0755);
    std::ofstream f(TURN, std::ios::trunc);
    f << g_turn_start_ms.load() << "\n";
}

void recover() {
    std::ifstream f(TURN);
    long long ms = 0;
    if (!(f >> ms) || ms <= 0) return;
    std::cout << "AI assistant: finishing a turn that was cut off" << std::endl;
    g_turn_start_ms = ms;
    g_turn = true;
    turn_finished();
}

void turn_finished() {
    const int64_t start = g_turn_start_ms.load();
    const std::string root = real(dig::PluginRegistry::dir());
    std::vector<std::string> changed;
#ifdef _WIN32
    // the plugins' own files changed: every plugin is built from them
    const bool core = !root.empty() && core_changed_since(root, start);
    if (core)
        std::cerr << "AI assistant: plugins/sdk, plugins/lib or a file in plugins/ changed during the turn - no "
                     "plugin runs until trusted now; check the changes (git status / git diff) first" << std::endl;
    // what kraken_doa itself runs with its full rights (the same rules refuse it)
    for (const char* f : {"ai/kraken_ai.py", "ai/sigtool.py"})
        if (changed_since(f, start))
            std::cerr << "AI assistant: " << f << " changed during the turn - check it (git diff)" << std::endl;
#else
    const bool core = false;
#endif
    if (!root.empty())
        if (DIR* d = opendir(root.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.empty() || n[0] == '.' || n == "sdk" || n == "lib") continue;
                const std::string p = root + "/" + n;
                struct stat st {};
                if (lstat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (core || changed_since(p, start)) changed.push_back(n);
            }
            closedir(d);
        }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        load_locked();
        bool added = false;
        for (const auto& id : changed)
            if (g_marked.insert(id).second) added = true;
        if (added) save_locked();
    }
    g_turn = false;
    unlink(TURN);
#ifdef _WIN32
    if (core) return;   // (said once above, for all of them)
    for (const auto& id : changed)
        std::cout << "AI assistant: plugin " << id << " was changed - it doesn't run until trusted (py -3 "
                  << "ai/kraken_ai.py trust " << id << ")" << std::endl;
#else
    for (const auto& id : changed)
        std::cout << "AI assistant: plugin " << id << " was changed - it runs sandboxed from now on" << std::endl;
#endif
}

std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::vector<std::string>& rw) {
#ifdef _WIN32
    (void)argv;
    (void)rw;
    return {};   // nothing to wrap it in: the callers refuse (NO_SANDBOX_FIX)
#elif defined(__APPLE__)
    if (!available() || argv.empty()) return {};
    std::vector<std::string> dirs;
    for (const auto& d : rw) {
        const std::string r = real(d);
        if (!r.empty()) dirs.push_back(r);
    }
    std::vector<std::string> w = {SANDBOX_EXEC, "-p", sbpl_profile(dirs), "/usr/bin/env", "TMPDIR=" + private_tmp() + "/"};
    w.insert(w.end(), argv.begin(), argv.end());
    return w;
#else
    if (!available() || argv.empty()) return {};
    std::vector<std::string> w = {bwrap_path(), "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
                                  "--tmpfs", "/tmp"};
    for (const auto& d : rw) {
        const std::string r = real(d);
        if (r.empty()) continue;
        w.insert(w.end(), {"--bind", r, r});
    }
    w.insert(w.end(), {"--unshare-net", "--unshare-ipc", "--die-with-parent", "--new-session", "--"});
    w.insert(w.end(), argv.begin(), argv.end());
    return w;
#endif
}

std::string list_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    std::string s = "[";
    bool first = true;
    for (const auto& id : g_marked) {
        s += (first ? "\"" : ",\"") + json_escape(id) + "\"";
        first = false;
    }
    return s + "]";
}

}  // namespace aisandbox
