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

std::string bwrap_path() {
    static const std::string p = [] {
        for (const char* c : {"/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap"})
            if (access(c, X_OK) == 0) return std::string(c);
        return std::string();
    }();
    return p;
}

// the list, re-read when the file changed (`kraken_ai.py trust` edits it)
void load_locked() {
    struct stat st {};
    if (stat(LIST, &st) != 0) {
        if (!g_loaded) g_marked.clear();
        g_loaded = true;
        return;
    }
    if (g_loaded && st.st_mtim.tv_sec == g_list_mtime.tv_sec && st.st_mtim.tv_nsec == g_list_mtime.tv_nsec) return;
    g_list_mtime = st.st_mtim;
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
    s += "],\"note\":\"plugins the AI assistant changed - kraken_doa runs them sandboxed (bwrap). "
         "python3 ai/kraken_ai.py trust <id> takes one off\"}\n";
    const std::string tmp = std::string(LIST) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << s;
    }
    if (rename(tmp.c_str(), LIST) == 0) {
        struct stat st {};
        if (stat(LIST, &st) == 0) g_list_mtime = st.st_mtim;
    }
}

// any file / folder under dir (not following links) whose ctime is >= ms
bool changed_since(const std::string& dir, int64_t ms, int depth = 0) {
    if (depth > 6) return false;
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) return false;
    if (static_cast<int64_t>(st.st_ctim.tv_sec) * 1000 + st.st_ctim.tv_nsec / 1000000 >= ms) return true;
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

std::string real(const std::string& p) {
    char b[PATH_MAX];
    return realpath(p.c_str(), b) ? std::string(b) : std::string();
}

}  // namespace

bool available() {
    static const bool ok = [] {
        const std::string b = bwrap_path();
        if (b.empty()) return false;
        std::string out;
        int st = -1;
        dig::run_and_capture({b, "--ro-bind", "/", "/", "--dev", "/dev", "--unshare-net", "--die-with-parent", "/bin/true"},
                             5000, &out, &st);
        if (st != 0) std::cerr << "AI assistant: bwrap does not work here - plugins it changed won't run" << std::endl;
        return st == 0;
    }();
    return ok;
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
    if (!root.empty())
        if (DIR* d = opendir(root.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.empty() || n[0] == '.' || n == "sdk" || n == "lib") continue;
                const std::string p = root + "/" + n;
                struct stat st {};
                if (lstat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (changed_since(p, start)) changed.push_back(n);
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
    for (const auto& id : changed)
        std::cout << "AI assistant: plugin " << id << " was changed - it runs sandboxed from now on" << std::endl;
}

std::vector<std::string> wrap(const std::vector<std::string>& argv, const std::vector<std::string>& rw) {
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
