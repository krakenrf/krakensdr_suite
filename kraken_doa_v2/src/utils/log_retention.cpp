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
#include <map>
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
// `plain`? 1 yes, 0 no, -1 the gzip file is damaged (gz_intact). Streams both
// (a decoder log day can be hundreds of MB): one pass for the length (and the
// check), one comparing the tail.
int gz_ends_with(const std::string& gz, const std::string& plain) {
    struct stat st{};
    if (::stat(plain.c_str(), &st) != 0) return 0;
    const uint64_t p_size = static_cast<uint64_t>(st.st_size);
    uint64_t total = 0;
    if (!gz_intact(gz, [&](const char*, size_t n) { total += n; return true; })) return -1;
    if (total < p_size) return 0;
    FILE* f = fopen(plain.c_str(), "rb");
    if (!f) return -1;
    uint64_t skip = total - p_size;
    bool same = true;
    std::vector<char> b(1 << 16);
    const bool ok = gz_intact(gz, [&](const char* d, size_t n) {
        const size_t s0 = static_cast<size_t>(std::min<uint64_t>(skip, n));
        skip -= s0;
        d += s0;
        n -= s0;
        while (n > 0) {
            const size_t k = std::min(n, b.size());
            if (fread(b.data(), 1, k, f) != k || std::memcmp(d, b.data(), k) != 0) { same = false; return false; }
            d += k;
            n -= k;
        }
        return true;
    });
    fclose(f);
    if (!same) return 0;
    return ok ? 1 : -1;
}

// Files found damaged in this run, by path: the size and modification time then. Not read again
// (each pass would inflate it to find the same) until it changes; each said once
struct Damaged {
    std::mutex mu;
    std::map<std::string, std::pair<int64_t, int64_t>> seen;
};
Damaged& damaged() {
    static Damaged* d = new Damaged;
    return *d;
}
std::pair<int64_t, int64_t> stamp_of(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return {-1, -1};
    return {static_cast<int64_t>(st.st_size), fs_mtime_ms(path)};
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

bool gz_intact(const std::string& path, const std::function<bool(const char*, size_t)>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {   // gzip only: plain text is not "intact"
        fclose(f);
        return false;
    }
    std::vector<unsigned char> in(1 << 16), o(1 << 16);
    bool ok = true, mid = false;   // mid: inside a member (an end of file there = cut short)
    bool eof = false;
    while (ok) {
        if (zs.avail_in == 0 && !eof) {
            const size_t n = fread(in.data(), 1, in.size(), f);
            if (n == 0) eof = true;
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
        }
        if (zs.avail_in == 0 && eof) break;
        zs.next_out = o.data();
        zs.avail_out = static_cast<uInt>(o.size());
        const int rc = inflate(&zs, Z_NO_FLUSH);
        const size_t have = o.size() - zs.avail_out;
        if (rc == Z_STREAM_END) {
            mid = false;
            inflateReset(&zs);   // another member may follow - or the end of the file
        } else if (rc == Z_OK || rc == Z_BUF_ERROR) {
            mid = true;
        } else {
            ok = false;   // Z_DATA_ERROR: not a gzip header (zeros, garbage, plain text) or a damaged member
        }
        if (ok && have && out && !out(reinterpret_cast<const char*>(o.data()), have)) {
            ok = false;   // (the caller stopped the walk)
            break;
        }
    }
    ok = ok && !mid && !ferror(f);
    inflateEnd(&zs);
    fclose(f);
    return ok;
}

bool gzip_file(const std::string& path) {
    const std::string gz = path + ".gz", part = path + ".gz.part", dir = dir_of(path);
    struct stat st{};
    const bool have_gz = ::stat(gz.c_str(), &st) == 0;
    if (have_gz) {
        Damaged& dm = damaged();
        {
            std::lock_guard<std::mutex> lk(dm.mu);
            auto it = dm.seen.find(gz);
            if (it != dm.seen.end() && it->second == stamp_of(gz)) return false;   // found damaged before, unchanged
        }
        const int r = gz_ends_with(gz, path);
        if (r < 0) {
            // never touched: rewriting it could lose what is still readable, and a member added after
            // damaged bytes is hidden from every reader. Said once per file per run, then remembered
            std::lock_guard<std::mutex> lk(dm.mu);
            const bool first = dm.seen.find(gz) == dm.seen.end();
            dm.seen[gz] = stamp_of(gz);
            if (first) std::cerr << gz << " is damaged (cut short, or bytes after its last gzip member): " << path
                                 << " stays a plain file beside it" << std::endl;
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
                   const std::function<void(int)>& compressing, bool deletions) {
    PassStats st;
    std::vector<std::string> todo;
    for (const auto& f : files) {
        if (f.day < 0) continue;
        const Action a = retention_action(today - f.day, f.kind, keep, zip);
        if (a == Action::DELETE && (deletions || f.kind == Kind::PART)) {
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
            static std::mutex mu;   // once per file per run (it would repeat every pass)
            static std::set<std::string>* said = new std::set<std::string>;
            std::lock_guard<std::mutex> lk(mu);
            if (said->insert(p).second) std::cerr << label << ": cannot gzip " << p << " (kept as it is; said once)" << std::endl;
        }
        if (compressing) compressing(-1);
    }
    return st;
}

Scheduler::Scheduler(std::function<void(bool)> pass, std::function<bool()> hold, std::chrono::milliseconds debounce,
                     std::chrono::milliseconds delete_grace)
    : pass_(std::move(pass)), hold_(std::move(hold)), debounce_(debounce), grace_(delete_grace) {
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
    const auto t = std::chrono::steady_clock::now();
    due_ = t + debounce_;
    if (grace_.count() > 0) {   // the deletions wait for the last change + the grace
        changed_ = t;
        full_due_ = t + grace_;
    }
    cv_.notify_all();
}

void Scheduler::run() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (!allowed_ || (!now_ && !due_ && !full_due_)) {
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
        if (!now_ && !due_ && full_due_ && t < *full_due_) {   // only the deletions' pass is left: at its time
            cv_.wait_until(lk, *full_due_);
            continue;
        }
        // deletions only once the grace after the last change is over
        const bool deletions = !changed_ || t - *changed_ >= grace_;
        now_ = false;
        due_.reset();
        if (deletions) {
            changed_.reset();
            full_due_.reset();
        }
        lk.unlock();
        pass_(deletions);   // reads the settings as they are now
        lk.lock();
    }
}

}  // namespace logret
