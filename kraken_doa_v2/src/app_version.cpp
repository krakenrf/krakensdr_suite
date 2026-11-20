// kraken_doa's side of the version indicator - see app_version.hpp and
// docs/dev/version.md
#include "app_version.hpp"

#include "utils/json_escape.hpp"
#include "utils/process.hpp"
#include "utils/version_info.hpp"

#include <cctype>
#include <chrono>
#include <mutex>

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto STALE_AFTER = std::chrono::seconds(30);   // heimdall broadcasts its status at 2 Hz
constexpr size_t MAX_PENDING = 64 * 1024;

std::mutex mtx;
std::string pending;          // the line being received
std::string heimdall_json;    // appver::to_json of the last one
Clock::time_point heimdall_at;

// The object value of a key of the TOP-LEVEL object ("" if absent or not an
// object): heimdall's status also nests objects of its own
std::string top_level_object(const std::string& s, const std::string& key) {
    int depth = 0;
    size_t p = 0;
    while (p < s.size()) {
        const char c = s[p];
        if (c == '"') {
            std::string tok;
            const size_t after = json_read_string(s, p, tok);
            size_t q = after;
            while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) q++;
            if (depth == 1 && tok == key && q < s.size() && s[q] == ':') {
                q++;
                while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) q++;
                if (q >= s.size() || s[q] != '{') return {};
                int d = 0;
                for (size_t e = q; e < s.size(); e++) {
                    if (s[e] == '"') {
                        std::string t;
                        e = json_read_string(s, e, t) - 1;   // the loop's ++ steps past the closing quote
                        continue;
                    }
                    if (s[e] == '{') d++;
                    else if (s[e] == '}' && --d == 0) return s.substr(q, e - q + 1);
                }
                return {};
            }
            p = after;
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    return {};
}

// Caller holds mtx
void handle_line(const std::string& line) {
    const std::string obj = top_level_object(line, "version");
    if (obj.empty()) return;   // command replies, an older heimdall
    // the fields only, re-checked and the text rebuilt (appver::parse_kv)
    std::string kv, v;
    for (const char* k : {"count", "hash", "date", "branch"})
        if (json_find(obj, k, v)) kv += std::string(k) + "=" + v + "\n";
    if (json_find(obj, "modified", v)) kv += std::string("modified=") + (v == "true" ? "1" : "0") + "\n";
    if (json_find(obj, "shallow", v)) kv += std::string("shallow=") + (v == "true" ? "1" : "0") + "\n";
    appver::Info info;
    if (!appver::parse_kv(kv, &info)) {
        info = appver::Info();   // heimdall doesn't know its version either
    }
    info.source = "unknown";
    if (json_find(obj, "source", v) && (v == "git" || v == "file" || v == "build")) info.source = v;
    heimdall_json = appver::to_json(info);
    heimdall_at = Clock::now();
}

bool run_git(const std::vector<std::string>& argv, int timeout_ms, std::string* out) {
    kproc::ExitStatus st;
    // search_path: git from PATH (Windows: never the cwd); Console::None - a leaf program
    return kproc::run_capture(argv, timeout_ms, out, &st, 1u << 20, true, kproc::Console::None);
}

}  // namespace

void app_version_init() { appver::set_runner(run_git); }

void heimdall_version_ingest(const char* data, size_t n) {
    std::lock_guard<std::mutex> lock(mtx);
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            handle_line(pending);
            pending.clear();
        } else if (pending.size() < MAX_PENDING) {
            pending.push_back(data[i]);
        }
    }
    if (pending.size() >= MAX_PENDING) pending.clear();
}

std::string heimdall_version_json() {
    std::lock_guard<std::mutex> lock(mtx);
    if (heimdall_json.empty() || Clock::now() - heimdall_at > STALE_AFTER) return {};
    return heimdall_json;
}
