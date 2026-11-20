// SPDX-License-Identifier: MIT
#pragma once
// The suite's version, taken from git (docs/dev/version.md). The same file in
// heimdall_v2/src/core/ and kraken_doa_v2/include/utils/ (version_info.cpp
// next to each: identical but for its #include line) - keep them identical.
//
// Text: "r<commits on HEAD> · <hash>", e.g. "r294 · 63a4917". A shallow clone
// counts only the commits it has, so there it is "<commit date> · <hash>"
// ("2026-11-16 · 63a4917"). " · modified" is appended when tracked files
// differ from the commit. Nothing is ever bumped by hand: every commit is a
// new version.
//
// Where it comes from - the first that answers:
//   git      the work tree the program runs in (the cwd), when it is this
//            suite's (it has heimdall_v2/ and kraken_doa_v2/)
//   file     ./VERSION (cwd-relative; the Docker build and windows/package.sh
//            write it - neither has git at run time)
//   build    compiled in by the Makefile (scripts/version.sh --header)
//   unknown
// The git answer is re-read in the background, so a `git pull` that changes
// only a web page (no rebuild) still shows the new version.
#include <functional>
#include <string>
#include <vector>

namespace appver {

struct Info {
    std::string text;          // "" = unknown
    long long count = -1;      // commits on HEAD; -1 = not known (shallow clone)
    std::string hash;          // abbreviated commit hash (at least 7 hex digits)
    std::string date;          // commit date, ISO 8601 with its UTC offset
    std::string branch;        // "" = detached HEAD / not known
    bool modified = false;     // tracked files differ from the commit
    bool shallow = false;
    std::string source;        // "git" | "file" | "build" | "unknown"
    std::string build;         // the compiled-in text, when it differs from `text`
};

// Runs argv (argv[0] = "git", looked up on PATH) without a shell, stdin and
// stderr to the null device; true + its stdout when it exited 0 in time.
// Each app passes its own process helper (kraken_doa: kproc).
using Runner = std::function<bool(const std::vector<std::string>& argv, int timeout_ms, std::string* out)>;

void set_runner(Runner run);   // before resolve_now / start
void resolve_now();            // blocking (a few git calls): --version
// Background thread: resolves, logs "Version: ..." once (stdout), then
// re-resolves when git's HEAD / index / reflog change (checked every 10 s)
// and every 5 minutes (a file edited in the work tree). Never blocks callers.
void start();
void stop();                   // joins the thread (optional at exit)

Info get();                    // the current answer (copied under a mutex)
std::string json();            // the "version" object of the status JSONs
std::string describe();        // --version: the text, then a detail line

// Building blocks
std::string format_text(const Info& v);                  // the text from the fields
bool parse_kv(const std::string& kv, Info* out);        // VERSION file format (key=value lines)
std::string to_json(const Info& v);                      // {"text":..,"count":..,...}

}  // namespace appver
