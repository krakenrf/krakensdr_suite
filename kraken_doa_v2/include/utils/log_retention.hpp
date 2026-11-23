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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
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

// path -> path.gz (zlib level 6), durable against a power cut: written as
// path.gz.part, fsync'ed, moved to path.gz (fs_replace), the folder fsync'ed,
// only then the original removed. An existing path.gz is never lost:
//   - its rows end with exactly this file's (a crash between the move and the
//     unlink): the original is only removed;
//   - other rows (the clock was set back after a pass and the day written
//     again): this file is added as one more gzip member after them
//     (concatenated members are one valid gzip file; zcat, zgrep and gzgets
//     read them all);
//   - damaged: nothing is changed, false. Damaged = not nothing but complete
//     gzip members (gz_intact): a member cut short, bytes after the last
//     member (zeros after a power cut on FAT, garbage), not gzip at all.
//     zlib's reader stops quietly at such bytes (or reads a non-gzip file as
//     it is), so a member added after them would be invisible to zcat, gzgets
//     and the history. A file found damaged is remembered (by its size and
//     modification time) and not read again until it changes; said once.
// false keeps the original (and leaves no part).
bool gzip_file(const std::string& path);

// Is `path` nothing but complete gzip members, to its last byte? An empty
// file counts as intact (no member). Inflates it all; `out` (optional) gets
// the decompressed bytes in order and may stop the walk by returning false.
bool gz_intact(const std::string& path, const std::function<bool(const char*, size_t)>& out = {});

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
// deletions = false: no plain / gz day is deleted (the Scheduler's grace
// after a settings change); .gz.part leftovers still go. A day that can't be
// gzipped is said once per file per run (stderr).
PassStats run_pass(const std::vector<File>& files, long today, int keep, int zip, const char* label,
                   const std::function<void(int)>& compressing = {}, bool deletions = true);

// Runs a log's passes on its own thread, one at a time; a request during a
// pass runs one more after it. The pass function reads the settings itself,
// when it starts.
//   - Nothing runs before allow(): the stored Keep / Then compressed days must
//     be known first (a pass with the defaults could compress or delete days
//     the stored settings keep). Requests before it run once it comes.
//   - request(): a pass now - the startup, the day change, a timer.
//   - request_after_change(): a settings change - the pass runs `debounce`
//     after the LAST such request. A replay (RESET_SETTINGS, the end of a
//     playback) or a page sets Keep and Then compressed one after the other;
//     a pass between the two would run on half the change (Keep 30 / 0 ->
//     7 / 365 passing through 7 / 0 deletes days 8 - 30). A request() while a
//     changed setting waits joins that pass.
//   - hold (optional): while it is true no pass runs (a debug playback: the
//     settings in force are the sandbox's, the files the user's); what is
//     pending waits, and when the hold ends it runs `debounce` later - after
//     the stored settings are replayed.
//   - delete_grace (optional, 0 = none): after a settings change, passes
//     only compress until `delete_grace` after the LAST change; then a pass
//     with the deletions runs by itself. A person setting Keep and then Then
//     compressed (or a script sending them as two commands) seconds or
//     minutes apart would otherwise get the deletions of the rule in
//     between (stored 30 / none -> Keep 7 deletes days 8 - 30 before "1 year"
//     arrives) - the debounce only joins changes made within a few seconds.
//     The pass function is told whether it may delete.
// A Scheduler must outlive its thread: make it a never-destroyed static.
class Scheduler {
public:
    static constexpr std::chrono::milliseconds DEBOUNCE{5000};
    explicit Scheduler(std::function<void(bool deletions)> pass, std::function<bool()> hold = {},
                       std::chrono::milliseconds debounce = DEBOUNCE,
                       std::chrono::milliseconds delete_grace = std::chrono::milliseconds(0));
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    void allow();                  // the settings are known: passes may run - and one runs now
    void request();                // a pass now, or after the running one
    void request_after_change();   // a pass `debounce` after the last change
    bool allowed();

private:
    void run();
    std::mutex mu_;
    std::condition_variable cv_;
    bool allowed_ = false, now_ = false, held_ = false;
    std::optional<std::chrono::steady_clock::time_point> due_;
    std::optional<std::chrono::steady_clock::time_point> changed_, full_due_;   // the last change; its deletions' pass
    std::function<void(bool)> pass_;
    std::function<bool()> hold_;
    std::chrono::milliseconds debounce_, grace_;
};

}  // namespace logret
