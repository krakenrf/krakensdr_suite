// ============================================
// src/utils/log_retention.cpp - plain for N days, then gzip for M more, then
// deleted: the pass and its scheduler (include/utils/log_retention.hpp)
// ============================================

#include "utils/log_retention.hpp"

#include "utils/fs_compat.hpp"

#include <unistd.h>
#include <zlib.h>

#include <cstdio>
#include <iostream>
#include <thread>

namespace logret {

// (moved from decoder_log.cpp unchanged)
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
    // fs_replace: a .gz left by an earlier try is replaced (Windows' rename()
    // refused it, so that day was never compressed). Windows also can't
    // delete a file another program has open: it stays, and the next
    // pass gzips it again
    if (!ok || fs_replace(part.c_str(), (path + ".gz").c_str()) != 0) {
        unlink(part.c_str());
        return false;
    }
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

void Scheduler::allow() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        allowed_ = true;
    }
    request();
}

bool Scheduler::allowed() {
    std::lock_guard<std::mutex> lk(mu_);
    return allowed_;
}

void Scheduler::request() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!allowed_) return;   // not before the stored days are known
        if (running_) { again_ = true; return; }
        running_ = true;
        again_ = false;
    }
    std::thread([this] {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                again_ = false;
            }
            pass_();   // reads the settings as they are now
            std::lock_guard<std::mutex> lk(mu_);
            if (!again_) { running_ = false; return; }
        }
    }).detach();
}

}  // namespace logret
