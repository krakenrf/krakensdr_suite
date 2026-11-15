// SPDX-License-Identifier: MIT
// See version_info.hpp. Identical in heimdall_v2/src/core/ and
// kraken_doa_v2/src/utils/ except for the #include line below.
#include "version_info.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

// The compiled-in answer: the Makefiles write build_version.h (into their
// object folder, on the include path of this file only) with
//   #define KRAKEN_BUILD_VERSION "count=294\nhash=63a4917\n..."
// from scripts/version.sh --header; rewritten only when it changes, so a new
// commit recompiles this one file. A build without it (CMake, a copied tree
// without scripts/) has none.
#if defined(__has_include)
#if __has_include("build_version.h")
#include "build_version.h"
#endif
#endif
#ifndef KRAKEN_BUILD_VERSION
#define KRAKEN_BUILD_VERSION ""
#endif

namespace appver {

namespace {

constexpr const char* DOT = " \xC2\xB7 ";   // " · "
constexpr int GIT_TIMEOUT_MS = 5000;        // rev-parse / log / rev-list: milliseconds
constexpr int STATUS_TIMEOUT_MS = 15000;    // diff stats every tracked file (a Pi's SD card)
constexpr auto CHECK_EVERY = std::chrono::seconds(10);
constexpr auto FULL_EVERY = std::chrono::minutes(5);

std::mutex mtx;
Runner runner;
Info current;                 // guarded by mtx
std::string current_json;     // to_json(current), guarded by mtx
bool resolved = false;

std::mutex thread_mtx;
std::condition_variable thread_cv;
bool thread_stop = false;
std::thread* worker = nullptr;   // never destroyed: a joinable std::thread at exit would terminate()

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> lines_of(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) v.push_back(trim(l));
    return v;
}

// Field checks: what reaches a page or a log is plain ASCII of a known shape
bool valid_hash(const std::string& h) {
    if (h.size() < 4 || h.size() > 40) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string clean_date(const std::string& d) {
    std::string o;
    for (char c : d) {
        if (o.size() >= 40) break;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == ':' || c == 'T' || c == 'Z') o += c;
    }
    return o;
}

std::string clean_branch(const std::string& b) {
    std::string o;
    for (char c : b) {
        if (o.size() >= 100) break;
        const unsigned char u = static_cast<unsigned char>(c);
        o += (u > 0x20 && u < 0x7F && c != '"' && c != '\\') ? c : '?';
    }
    return o;
}

long long parse_count(const std::string& s) {
    if (s.empty() || s.size() > 12) return -1;
    for (char c : s)
        if (c < '0' || c > '9') return -1;
    return std::strtoll(s.c_str(), nullptr, 10);
}

std::string esc(const std::string& s) {   // fields are already clean; quotes and controls anyway
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

Info from_build() {
    Info v;
    if (parse_kv(KRAKEN_BUILD_VERSION, &v)) v.source = "build";
    return v;
}

bool from_file(Info* out) {
    std::ifstream f("VERSION", std::ios::binary);
    if (!f) return false;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > 4096 || !parse_kv(s, out)) return false;
    out->source = "file";
    return true;
}

// The work tree of the cwd, if it is this suite's. *stamp_paths: the files
// whose change means "ask again" (HEAD, the index, the reflog of HEAD).
bool from_git(Info* out, std::vector<std::string>* stamp_paths) {
    Runner run;
    {
        std::lock_guard<std::mutex> lk(mtx);
        run = runner;
    }
    if (!run) return false;
    std::string o;
    if (!run({"git", "rev-parse", "--show-toplevel", "--absolute-git-dir", "--is-shallow-repository",
              "--abbrev-ref", "HEAD"}, GIT_TIMEOUT_MS, &o))
        return false;
    const std::vector<std::string> l = lines_of(o);
    if (l.size() < 4 || l[0].empty() || l[1].empty()) return false;
    // another repository the program happens to run in (a data folder inside
    // some project) is not the suite's version
    std::error_code ec;
    const std::filesystem::path top(l[0]);
    if (!std::filesystem::is_directory(top / "heimdall_v2", ec) || !std::filesystem::is_directory(top / "kraken_doa_v2", ec))
        return false;
    Info v;
    v.shallow = l[2] == "true";
    v.branch = l[3] == "HEAD" ? std::string() : clean_branch(l[3]);
    if (!run({"git", "log", "-1", "--abbrev=7", "--format=%h%n%cI", "HEAD"}, GIT_TIMEOUT_MS, &o)) return false;
    const std::vector<std::string> lg = lines_of(o);
    if (lg.size() < 2 || !valid_hash(lg[0])) return false;
    v.hash = lg[0];
    v.date = clean_date(lg[1]);
    if (!v.shallow && run({"git", "rev-list", "--count", "HEAD"}, GIT_TIMEOUT_MS, &o)) v.count = parse_count(trim(o));
    // Tracked files that differ from the commit (staged or not; untracked
    // ones don't count), the index not rewritten (--no-optional-locks: never
    // races the user's own git commands for index.lock). Not `git status`:
    // for a file with a clean filter (.gitattributes: settings JSON, secrets
    // blanked) it trusts a size change and says " M" although the filtered
    // content is the same; diff compares the content (a touched file, the
    // same content: not modified either)
    if (run({"git", "--no-optional-locks", "diff", "--no-ext-diff", "--name-only", "--ignore-submodules=all",
             "HEAD", "--"}, STATUS_TIMEOUT_MS, &o))
        v.modified = !trim(o).empty();
    v.source = "git";
    *out = v;
    stamp_paths->clear();
    for (const char* f : {"HEAD", "index", "logs/HEAD"}) stamp_paths->push_back(l[1] + "/" + f);
    return true;
}

// mtime + size of each path ("" for a missing one)
std::string stamp_of(const std::vector<std::string>& paths) {
    std::string s;
    for (const auto& p : paths) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (!ec) s += std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        s += '/';
        const auto n = std::filesystem::file_size(p, ec);
        if (!ec) s += std::to_string(static_cast<unsigned long long>(n));
        s += ';';
    }
    return s;
}

// One resolution in the fallback order; returns true when the text changed.
bool resolve(std::vector<std::string>* stamp_paths) {
    Info v;
    stamp_paths->clear();
    if (!from_git(&v, stamp_paths) && !from_file(&v)) {
        v = from_build();
        if (v.source.empty()) v.source = "unknown";
    }
    v.text = format_text(v);
    if (v.source != "build") {
        const Info b = from_build();
        const std::string bt = format_text(b);
        if (!bt.empty() && bt != v.text) v.build = bt;
    }
    std::lock_guard<std::mutex> lk(mtx);
    const bool changed = !resolved || v.text != current.text || v.build != current.build ||
                         v.branch != current.branch || v.source != current.source;
    current = v;
    current_json = to_json(v);
    resolved = true;
    return changed;
}

std::string log_line(const Info& v) {
    std::string s = v.text.empty() ? std::string("unknown") : v.text;
    s += " (" + v.source;
    if (!v.branch.empty()) s += ", branch " + v.branch;
    if (!v.date.empty()) s += ", " + v.date;
    if (!v.build.empty()) s += ", program built from " + v.build;
    return s + ")";
}

void thread_main() {
    std::vector<std::string> paths;
    resolve(&paths);
    std::cout << "Version: " << log_line(get()) << std::endl;
    std::string stamp = stamp_of(paths);
    auto last_full = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(thread_mtx);
    while (!thread_stop) {
        thread_cv.wait_for(lk, CHECK_EVERY, [] { return thread_stop; });
        if (thread_stop) break;
        lk.unlock();
        const auto now = std::chrono::steady_clock::now();
        const std::string s = stamp_of(paths);
        if (s != stamp || now - last_full >= FULL_EVERY) {
            if (resolve(&paths)) std::cout << "Version now: " << log_line(get()) << std::endl;
            stamp = stamp_of(paths);
            last_full = now;
        }
        lk.lock();
    }
}

}  // namespace

void set_runner(Runner run) {
    std::lock_guard<std::mutex> lk(mtx);
    runner = std::move(run);
}

void resolve_now() {
    std::vector<std::string> paths;
    resolve(&paths);
}

void start() {
    std::lock_guard<std::mutex> lk(thread_mtx);
    if (worker) return;
    thread_stop = false;
    worker = new std::thread(thread_main);
}

void stop() {
    std::thread* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(thread_mtx);
        thread_stop = true;
        std::swap(t, worker);
    }
    thread_cv.notify_all();
    if (t && t->joinable()) t->join();
    delete t;
}

Info get() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!resolved) {   // before the first resolution: the compiled-in answer
        Info v = from_build();
        if (v.source.empty()) v.source = "unknown";
        v.text = format_text(v);
        return v;
    }
    return current;
}

std::string json() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (resolved) return current_json;
    }
    return to_json(get());
}

std::string describe() {
    const Info v = get();
    std::string s = (v.text.empty() ? std::string("unknown") : v.text) + "\n";
    std::string d;
    auto add = [&d](const std::string& x) { d += (d.empty() ? "" : ", ") + x; };
    if (!v.date.empty()) add("commit " + v.date);
    if (!v.branch.empty()) add("branch " + v.branch);
    if (v.shallow) add("shallow clone");
    add("from " + v.source);
    if (!v.build.empty()) add("program built from " + v.build);
    return s + d + "\n";
}

std::string format_text(const Info& v) {
    if (!valid_hash(v.hash)) return std::string();
    std::string t;
    if (v.count >= 0 && !v.shallow) t = "r" + std::to_string(v.count) + DOT;
    else if (v.date.size() >= 10) t = v.date.substr(0, 10) + DOT;
    t += v.hash;
    if (v.modified) t += std::string(DOT) + "modified";
    return t;
}

bool parse_kv(const std::string& kv, Info* out) {
    Info v;
    for (const std::string& line : lines_of(kv)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        if (k == "count") v.count = parse_count(val);
        else if (k == "hash") v.hash = val;
        else if (k == "date") v.date = clean_date(val);
        else if (k == "branch") v.branch = clean_branch(val);
        else if (k == "modified") v.modified = val == "1" || val == "true";
        else if (k == "shallow") v.shallow = val == "1" || val == "true";
    }
    if (!valid_hash(v.hash)) return false;
    v.text = format_text(v);
    *out = v;
    return true;
}

std::string to_json(const Info& v) {
    std::string s = "{\"text\":\"" + esc(v.text) + "\"";
    if (v.count >= 0) s += ",\"count\":" + std::to_string(v.count);
    if (!v.hash.empty()) s += ",\"hash\":\"" + esc(v.hash) + "\"";
    if (!v.date.empty()) s += ",\"date\":\"" + esc(v.date) + "\"";
    if (!v.branch.empty()) s += ",\"branch\":\"" + esc(v.branch) + "\"";
    s += std::string(",\"modified\":") + (v.modified ? "true" : "false");
    if (v.shallow) s += ",\"shallow\":true";
    s += ",\"source\":\"" + esc(v.source) + "\"";
    if (!v.build.empty()) s += ",\"build\":\"" + esc(v.build) + "\"";
    return s + "}";
}

}  // namespace appver
