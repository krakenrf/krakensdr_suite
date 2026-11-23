// ============================================
// src/utils/log_retention.cpp - plain for N days, then gzip for M more, then
// deleted: the gzip, the pass and its scheduler (include/utils/log_retention.hpp)
// ============================================

#include "utils/log_retention.hpp"

#include "utils/fs_compat.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <thread>

namespace logret {

namespace {

// the folder of a path ("." for a bare name; '/' or, on Windows, '\\')
std::string dir_of(const std::string& path) {
#ifdef _WIN32
    const size_t s = path.find_last_of("/\\");
#else
    const size_t s = path.rfind('/');
#endif
    return s == std::string::npos ? "." : path.substr(0, s ? s : 1);
}

// flush a file's data to the disk (Windows: _commit, win/posix_compat.h)
bool fsync_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}

// Does the decompressed content of the gzip file `gz` END with the bytes of
// `plain`? 1 yes, 0 no, -1 the gzip file is damaged (it can't be read to its
// end). Streams both (a decoder log day can be hundreds of MB): one pass for
// the length, one comparing the tail.
int gz_ends_with(const std::string& gz, const std::string& plain) {
    struct stat st{};
    if (::stat(plain.c_str(), &st) != 0) return 0;
    const uint64_t p_size = static_cast<uint64_t>(st.st_size);
    std::vector<char> a(1 << 16), b(1 << 16);
    uint64_t total = 0;
    {
        gzFile g = gzopen(gz.c_str(), "rb");
        if (!g) return -1;
        int n;
        while ((n = gzread(g, a.data(), static_cast<unsigned>(a.size()))) > 0) total += static_cast<uint64_t>(n);
        const int rc = gzclose(g);
        if (n < 0 || rc != Z_OK) return -1;
    }
    if (total < p_size) return 0;
    gzFile g = gzopen(gz.c_str(), "rb");
    FILE* f = fopen(plain.c_str(), "rb");
    if (!g || !f) {
        if (g) gzclose(g);
        if (f) fclose(f);
        return -1;
    }
    int result = 1;
    uint64_t skip = total - p_size;
    while (skip > 0) {
        const int n = gzread(g, a.data(), static_cast<unsigned>(std::min<uint64_t>(skip, a.size())));
        if (n <= 0) { result = -1; break; }
        skip -= static_cast<uint64_t>(n);
    }
    while (result == 1) {
        const int n = gzread(g, a.data(), static_cast<unsigned>(a.size()));
        if (n < 0) { result = -1; break; }
        if (n == 0) break;
        if (fread(b.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n) || std::memcmp(a.data(), b.data(), static_cast<size_t>(n)) != 0)
            result = 0;
    }
    gzclose(g);
    fclose(f);
    return result;
}

// copy a file's bytes as they are (the existing gzip members into the part)
bool copy_file(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    std::vector<char> buf(1 << 16);
    bool ok = true;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
        if (fwrite(buf.data(), 1, n, out) != n) { ok = false; break; }
    ok = !ferror(in) && ok;
    fclose(in);
    return fclose(out) == 0 && ok;
}

}  // namespace

bool gzip_file(const std::string& path) {
    const std::string gz = path + ".gz", part = path + ".gz.part", dir = dir_of(path);
    struct stat st{};
    const bool have_gz = ::stat(gz.c_str(), &st) == 0;
    if (have_gz) {
        const int r = gz_ends_with(gz, path);
        if (r < 0) {
            // never touched: rewriting it could lose what is still readable, and adding a member after
            // a damaged one hides it from every reader. Said once per file per run
            static std::mutex mu;
            static std::set<std::string> said;
            std::lock_guard<std::mutex> lk(mu);
            if (said.insert(gz).second)
                std::cerr << gz << " is damaged: " << path << " stays a plain file beside it" << std::endl;
            return false;
        }
        if (r == 1) {   // a crash between the move and the unlink: the gz holds these rows already
            fs_fsync_dir(dir);
            unlink(path.c_str());
            return true;
        }
    }
    FILE* in = fopen(path.c_str(), "rb");
    if (!in) return false;
    // the part: an existing gz's members first (an earlier pass's rows - kept), then this file
    if (have_gz && !copy_file(gz, part)) {
        fclose(in);
        unlink(part.c_str());
        return false;
    }
    gzFile out = gzopen(part.c_str(), have_gz ? "ab6" : "wb6");
    if (!out) { fclose(in); unlink(part.c_str()); return false; }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        const size_t n = fread(buf.data(), 1, buf.size(), in);
        if (n == 0) break;
        if (gzwrite(out, buf.data(), static_cast<unsigned>(n)) != static_cast<int>(n)) { ok = false; break; }
    }
    ok = !ferror(in) && ok;
    fclose(in);
    ok = gzclose(out) == Z_OK && ok;
    // durable before the original goes: the part's data on the disk (ext4's delayed allocation
    // otherwise leaves a renamed but empty file after a power cut), then the rename. fs_replace: a
    // .gz left by an earlier try is replaced (Windows' rename() refused it). Windows also can't
    // delete a file another program has open: it stays, and the next pass finds it in the gz
    ok = ok && fsync_file(part);
    if (!ok || fs_replace(part.c_str(), gz.c_str()) != 0) {
        unlink(part.c_str());
        return false;
    }
    fs_fsync_dir(dir);
    unlink(path.c_str());
    return true;
}

PassStats run_pass(const std::vector<File>& files, long today, int keep, int zip, const char* label,
                   const std::function<void(int)>& compressing) {
    PassStats st;
    std::vector<std::string> todo;
    for (const auto& f : files) {
        if (f.day < 0) continue;
        const Action a = retention_action(today - f.day, f.kind, keep, zip);
        if (a == Action::DELETE) {
            if (unlink(f.path.c_str()) == 0 && f.kind != Kind::PART) {
                st.deleted++;
                std::cout << label << ": deleted " << f.path << " (older than " << keep << " + " << zip << " days)"
                          << std::endl;
            }
        } else if (a == Action::COMPRESS) {
            todo.push_back(f.path);
        }
    }
    if (!todo.empty() && compressing) compressing(static_cast<int>(todo.size()));
    for (const auto& p : todo) {
        if (gzip_file(p)) {
            st.compressed++;
            std::cout << label << ": gzipped " << p << std::endl;
        } else {
            st.failed++;
            std::cerr << label << ": cannot gzip " << p << std::endl;
        }
        if (compressing) compressing(-1);
    }
    return st;
}

Scheduler::Scheduler(std::function<void()> pass, std::function<bool()> hold, std::chrono::milliseconds debounce)
    : pass_(std::move(pass)), hold_(std::move(hold)), debounce_(debounce) {
    std::thread([this] { run(); }).detach();
}

void Scheduler::allow() {
    std::lock_guard<std::mutex> lk(mu_);
    allowed_ = true;
    now_ = true;
    cv_.notify_all();
}

bool Scheduler::allowed() {
    std::lock_guard<std::mutex> lk(mu_);
    return allowed_;
}

void Scheduler::request() {
    std::lock_guard<std::mutex> lk(mu_);
    now_ = true;
    cv_.notify_all();
}

void Scheduler::request_after_change() {
    std::lock_guard<std::mutex> lk(mu_);
    due_ = std::chrono::steady_clock::now() + debounce_;
    cv_.notify_all();
}

void Scheduler::run() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (!allowed_ || (!now_ && !due_)) {
            cv_.wait(lk);
            continue;
        }
        const auto t = std::chrono::steady_clock::now();
        if (hold_ && hold_()) {   // a debug playback: not now (looked at once a second)
            held_ = true;
            cv_.wait_for(lk, std::chrono::seconds(1));
            continue;
        }
        if (held_) {   // the hold ended: the stored settings are replayed first - one debounce from now
            held_ = false;
            if (!due_ || *due_ < t + debounce_) due_ = t + debounce_;
            continue;
        }
        if (due_ && t < *due_) {   // a changed setting waits (a request() now joins it)
            cv_.wait_until(lk, *due_);
            continue;
        }
        now_ = false;
        due_.reset();
        lk.unlock();
        pass_();   // reads the settings as they are now
        lk.lock();
    }
}

}  // namespace logret
