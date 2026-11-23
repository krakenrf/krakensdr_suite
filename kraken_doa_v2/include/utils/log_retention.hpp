#pragma once

// Retention of the logs that keep one file per day: a finished day stays
// plain for N days, then is gzipped in place for M more, then deleted. One
// implementation for every such log - the decoder data log (decoder_log.cpp,
// docs/dev/decoder-log.md) and the Wideband board / host telemetry logs
// (wb_board.cpp, docs/dev/wideband-variant.md); each log only lists its own
// files (its names, its folder) and hands them to run_pass.
//
// The rule (retention_action) is pure and header-only, so a harness tests it
// on its own; gzip_file, the pass and its scheduler are in
// src/utils/log_retention.cpp.

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace logret {

// the plain day file, its gzip (<file>.gz), a gzip a crash interrupted (<file>.gz.part)
enum class Kind { NONE, PLAIN, GZ, PART };

enum class Action { KEEP, COMPRESS, DELETE };

// days since 1970-01-01 of a civil date (proleptic Gregorian, no time zone involved)
inline long day_number(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// What the retention pass does with one of a log's files, `age` days old
// (today 0, yesterday 1 - by the date in its name): keep = N, zip = M.
//   N = 0: keep forever - nothing is compressed or deleted.
//   plain: kept while age <= N; gzipped while age <= N + M; then deleted
//          (M = 0: deleted after N, the old rule).
//   gz:    kept while age <= N + M (a day an older version gzipped early
//          stays gz - never decompressed); then deleted.
//   part:  a gzip a crash interrupted (the plain file is still there and is
//          compressed again) - removed; today's never.
// Today's files (age <= 0, also a clock set back) are never touched.
inline Action retention_action(long age, Kind kind, int keep, int zip) {
    if (age <= 0 || kind == Kind::NONE) return Action::KEEP;
    if (kind == Kind::PART) return Action::DELETE;
    if (keep <= 0) return Action::KEEP;
    const long last = static_cast<long>(keep) + (zip > 0 ? zip : 0);
    if (age > last) return Action::DELETE;
    if (kind == Kind::PLAIN && age > keep) return Action::COMPRESS;
    return Action::KEEP;
}

// path -> path.gz (zlib level 6, written as path.gz.part, then moved over any
// earlier path.gz with fs_replace, then the original is removed); false keeps
// the original (and leaves no part)
bool gzip_file(const std::string& path);

// One of a log's files, as its lister found it
struct File {
    std::string path;
    long day = -1;            // day_number of the date in its name (-1: not a date - never touched)
    Kind kind = Kind::NONE;
};

struct PassStats {
    int deleted = 0, compressed = 0, failed = 0;
};

// One retention pass over `files` (only the log's own names): each by
// retention_action(today - day, kind, keep, zip). The deletions first, then
// the compressions one file at a time (a crash leaves the plain file - and
// perhaps a .gz.part, removed by the next pass - never neither). `label`
// starts the log lines ("Decoder log: gzipped ..."); compressing(n) is told
// how many gzips start (+n) and each that ends (-1) - for a status line.
PassStats run_pass(const std::vector<File>& files, long today, int keep, int zip, const char* label,
                   const std::function<void(int)>& compressing = {});

// Runs a log's pass on a detached thread, one at a time; a request during a
// pass runs one more after it. Nothing runs before allow(): the settings
// replay must have set the stored Keep / Then compressed days first (a pass
// with the defaults could compress or delete days the stored settings keep).
// The pass function reads the settings itself, when it starts. A Scheduler
// must outlive its passes: make it a never-destroyed static.
class Scheduler {
public:
    explicit Scheduler(std::function<void()> pass) : pass_(std::move(pass)) {}
    void allow();     // the settings are known: passes may run - and one runs now
    void request();   // a pass now, or after the running one (ignored before allow())
    bool allowed();

private:
    std::mutex mu_;
    bool allowed_ = false, running_ = false, again_ = false;
    std::function<void()> pass_;
};

}  // namespace logret
