#include "ai_manager.hpp"
#include "ai_map.hpp"
#include "ai_mcp.hpp"
#include "ai_sandbox.hpp"
#include "geo_address.hpp"   // geo::json_parse
#include "settings_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <openssl/rand.h>   // random_hex: Windows has no /dev/urandom
#include <random>
#endif

#include "config.hpp"
#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "digital/dig_plugin.hpp"
#include "globals.hpp"
#include "networking/websocket_server.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include "utils/process.hpp"

#ifndef _WIN32
extern char** environ;   // (Windows: a macro of <stdlib.h>)
#endif

namespace {

constexpr const char* BRIDGE = "ai/kraken_ai.py";
constexpr const char* SESSIONS = "ai/sessions";
constexpr size_t LOG_MAX = 400;
constexpr const char* MARKS = "ai/identifications.json";
constexpr size_t MARKS_MAX = 1000;

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string read_file(const std::string& path, size_t max = 4 << 20) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > max) s.resize(max);
    return s;
}

// ai/ai_config.json, written by `kraken_ai.py setup` (KRAKEN_AI_CONFIG
// overrides the path - the bridge honours the same variable)
std::string config_path() {
    const char* e = std::getenv("KRAKEN_AI_CONFIG");
    return (e && *e) ? std::string(e) : std::string("ai/ai_config.json");
}

bool config_value(const std::string& key, std::string& out) {
    std::string j = read_file(config_path(), 65536);
    return !j.empty() && json_find(j, key, out);
}

// the answer to a question asked while the assistant is off: how to enable it
// (Windows: python.org's Python is started with its py launcher, and there
// may be no python3 at all)
#ifdef _WIN32
constexpr const char* NOT_ENABLED =
    "the AI assistant is not enabled on this receiver (run on this PC, in the kraken_doa_v2 folder: py -3 "
    "ai/kraken_ai.py setup)";
#else
constexpr const char* NOT_ENABLED =
    "the AI assistant is not enabled on this receiver (run on the Pi: python3 ai/kraken_ai.py setup)";
#endif

// Strict JSON syntax check (RFC 8259) - files the agent could have edited are
// embedded in messages only when they parse
struct JsonCheck {
    const std::string& s;
    size_t p = 0;
    int depth = 0;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(p, n, w) != 0) return false;
        p += n;
        return true;
    }
    bool str() {
        if (p >= s.size() || s[p] != '"') return false;
        for (p++; p < s.size(); p++) {
            unsigned char c = static_cast<unsigned char>(s[p]);
            if (c == '"') { p++; return true; }
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++p >= s.size()) return false;
                char e = s[p];
                if (e == 'u') {
                    for (int k = 0; k < 4; k++)
                        if (++p >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[p]))) return false;
                } else if (!strchr("\"\\/bfnrt", e)) {
                    return false;
                }
            }
        }
        return false;
    }
    bool num() {
        size_t b = p;
        if (p < s.size() && s[p] == '-') p++;
        if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
        while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
        if (p < s.size() && s[p] == '.') {
            p++;
            if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            p++;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
            if (p >= s.size() || !std::isdigit(static_cast<unsigned char>(s[p]))) return false;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) p++;
        }
        return p > b;
    }
    bool value() {
        ws();
        if (p >= s.size() || ++depth > 64) return false;
        bool ok;
        char c = s[p];
        if (c == '{') {
            p++;
            ws();
            ok = true;
            if (p < s.size() && s[p] == '}') p++;
            else
                for (;;) {
                    ws();
                    if (!str()) { ok = false; break; }
                    ws();
                    if (p >= s.size() || s[p] != ':') { ok = false; break; }
                    p++;
                    if (!value()) { ok = false; break; }
                    ws();
                    if (p < s.size() && s[p] == ',') { p++; continue; }
                    if (p < s.size() && s[p] == '}') { p++; break; }
                    ok = false;
                    break;
                }
        } else if (c == '[') {
            p++;
            ws();
            ok = true;
            if (p < s.size() && s[p] == ']') p++;
            else
                for (;;) {
                    if (!value()) { ok = false; break; }
                    ws();
                    if (p < s.size() && s[p] == ',') { p++; continue; }
                    if (p < s.size() && s[p] == ']') { p++; break; }
                    ok = false;
                    break;
                }
        } else if (c == '"') {
            ok = str();
        } else if (c == 't') {
            ok = lit("true");
        } else if (c == 'f') {
            ok = lit("false");
        } else if (c == 'n') {
            ok = lit("null");
        } else {
            ok = num();
        }
        depth--;
        return ok;
    }
};

bool json_valid(const std::string& s) {
    JsonCheck j{s};
    if (!j.value()) return false;
    j.ws();
    return j.p == s.size();
}

// A short `kraken_ai.py <command>` run; its stdout. python3 is looked up on
// PATH (what /usr/bin/env did); Windows: the interpreter kproc found
// (python_argv), on a hidden console of its own - the bridge starts console
// programs (claude --version), and under a console-less parent each would
// open a window
bool run_bridge(std::vector<std::string> args, int timeout_ms, std::string* out) {
    return kproc::run_capture(kproc::python_cmd(std::move(args)), timeout_ms, out, nullptr, 1u << 20, true,
                              kproc::Console::Hidden);
}

#ifdef _WIN32
// A Windows command line holds at most 32766 characters; the screen context
// (the page's view + every VFO) can be more once quoted - ask() then hands it
// over in a file. Upper bound per argument: its bytes, every " and \ escaped,
// quotes, space.
size_t cmdline_estimate(const std::vector<std::string>& argv) {
    size_t n = 0;
    for (const auto& a : argv)
        n += a.size() + static_cast<size_t>(std::count(a.begin(), a.end(), '"') + std::count(a.begin(), a.end(), '\\')) + 3;
    return n;
}
#endif

// cached `kraken_ai.py status` (CLI path + version)
std::mutex g_probe_mu;
std::string g_probe;
int64_t g_probe_ms = 0;
std::atomic<bool> g_probing{false};

void probe_async() {
    if (g_probing.exchange(true)) return;
    std::thread([] {
        std::string out;
        run_bridge({BRIDGE, "status"}, 30000, &out);
        {
            std::lock_guard<std::mutex> lk(g_probe_mu);
            g_probe = out;
            g_probe_ms = wall_ms();
        }
        g_probing = false;
        WebSocketServer::broadcast_json_message(AiManager::instance().state_json(false));
    }).detach();
}

}  // namespace

AiManager& AiManager::instance() {
    static AiManager m;
    return m;
}

AiManager::~AiManager() {
    std::shared_ptr<kproc::Process> p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p = proc_;
    }
    if (p) p->kill();   // the whole group (kill_tree)
    if (thread_.joinable()) thread_.detach();
}

bool AiManager::enabled() const {
    std::string v;
    return config_value("enabled", v) && v == "true";
}


namespace {

std::string fmt_num(double v, int decimals) {
    if (!is_finite_value(v)) return "null";
    char b[48];
    snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// text from a tool / page: valid UTF-8 cut to max bytes, control characters
// (but \n) dropped
std::string clean_text(const std::string& in, size_t max) {
    std::string out;
    for (size_t i = 0; i < in.size() && out.size() < max;) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > in.size()) { i++; continue; }
        bool ok = true;
        for (size_t k = 1; k < n; k++)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (!ok) { i++; continue; }
        if (out.size() + n > max) break;
        if (n == 1 && (c < 0x20 || c == 0x7F) && c != '\n') { if (c == '\t') out += ' '; i++; continue; }
        out.append(in, i, n);
        i += n;
    }
    return out;
}

std::string random_hex(size_t bytes) {
    std::string out;
#ifdef _WIN32
    // no /dev/urandom: OpenSSL's CSPRNG (the system's BCryptGenRandom)
    std::vector<unsigned char> r(bytes);
    if (bytes && RAND_bytes(r.data(), static_cast<int>(bytes)) != 1) {
        std::random_device rd;   // (rand_s)
        for (auto& c : r) c = static_cast<unsigned char>(rd());
    }
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) {
        const unsigned char c = r[i];
        out += hx[c >> 4];
        out += hx[c & 15];
    }
#else
    std::ifstream f("/dev/urandom", std::ios::binary);
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) {
        unsigned char c = static_cast<unsigned char>(f.get());
        out += hx[c >> 4];
        out += hx[c & 15];
    }
#endif
    return out;
}

}  // namespace

void AiManager::init() {
    mkdir("ai", 0755);
    mkdir(SESSIONS, 0755);
    // newest conversation (names start with a timestamp)
    std::string newest;
    if (DIR* d = opendir(SESSIONS)) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n[0] == '.') continue;
            if (access((std::string(SESSIONS) + "/" + n + "/session.json").c_str(), R_OK) == 0 && n > newest)
                newest = n;
        }
        closedir(d);
    }
    if (!newest.empty()) load_session(newest);
    {
        std::lock_guard<std::mutex> lk(mu_);
        fresh_ = true;   // a restart starts a new conversation (the last one is in the history)
    }
    load_marks();
    aimap::load();
    aisandbox::recover();
    probe_async();
}

void AiManager::load_session(const std::string& id) {
    std::string j = read_file(std::string(SESSIONS) + "/" + id + "/session.json");
    std::string v;
    std::lock_guard<std::mutex> lk(mu_);
    session_ = id;
    title_ = session_state_ = "";
    if (json_find(j, "title", v)) title_ = v;
    else if (json_find(j, "signal_name", v)) title_ = v;   // an AI Signal Lab investigation
    if (json_find(j, "state", v)) session_state_ = v;
    fresh_ = false;
}

bool AiManager::valid_session_id(const std::string& id) {
    if (id.empty() || id.size() > 64 || id[0] == '.') return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return id.find("..") == std::string::npos;
}

void AiManager::sessions_async() {
    std::thread([] {
        struct Row { std::string id, json; };
        std::vector<Row> rows;
        if (DIR* d = opendir(SESSIONS)) {
            while (dirent* e = readdir(d)) {
                std::string id = e->d_name;
                if (!valid_session_id(id)) continue;
                std::string j = read_file(std::string(SESSIONS) + "/" + id + "/session.json", 1 << 20);
                if (j.empty()) continue;
                std::string v, title, freq = "0";
                std::ostringstream o;
                if (json_find(j, "title", v)) title = v;
                // an AI Signal Lab investigation: its frequency + signal name
                if (json_find(j, "freq_hz", v)) freq = std::to_string(static_cast<long long>(atof(v.c_str())));
                if (title.empty() && json_find(j, "signal_name", v) && !v.empty()) title = v;
                if (title.empty() && freq != "0") title = fmt_num(atof(freq.c_str()) / 1e6, 5) + " MHz";
                o << "{\"id\":\"" << json_escape(id) << "\",\"title\":\"" << json_escape(title) << "\",\"freq_hz\":" << freq;
                for (const char* k : {"created", "state", "updated"}) {
                    v.clear();
                    json_find(j, k, v);
                    o << ",\"" << k << "\":\"" << json_escape(v) << "\"";
                }
                o << "}";
                rows.push_back({id, o.str()});
            }
            closedir(d);
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id > b.id; });   // newest first
        std::string msg = "{\"ai_sessions\":[";
        for (size_t i = 0; i < rows.size(); i++) msg += (i ? "," : "") + rows[i].json;
        msg += "]}";
        WebSocketServer::broadcast_json_message(msg);
    }).detach();
}

void AiManager::session_async(const std::string& id) {
    if (!valid_session_id(id)) return;
    std::thread([id] {
        const std::string dir = std::string(SESSIONS) + "/" + id;
        std::string j = read_file(dir + "/session.json", 4 << 20);
        if (j.empty()) {
            WebSocketServer::broadcast_json_message("{\"ai_session\":{\"id\":\"" + json_escape(id) +
                                                    "\",\"error\":\"no such conversation\"}}");
            return;
        }
        std::string v, title;
        if (json_find(j, "title", v)) title = v;
        else if (json_find(j, "signal_name", v)) title = v;
        std::ostringstream o;
        o << "{\"ai_session\":{\"id\":\"" << json_escape(id) << "\",\"title\":\"" << json_escape(title) << "\"";
        for (const char* k : {"created", "state"}) {
            v.clear();
            json_find(j, k, v);
            o << ",\"" << k << "\":\"" << json_escape(v) << "\"";
        }
        // the conversation: chat.json (written by the bridge - checked, the
        // agent can edit files in its folder); an old AI Signal Lab session
        // without one: its analysis
        std::string chat = read_file(dir + "/chat.json", 4 << 20);
        size_t p = chat.find_first_not_of(" \t\r\n");
        if (p == std::string::npos || chat[p] != '[' || !json_valid(chat)) {
            std::string analysis, freq = "0";
            json_find(j, "analysis", analysis);
            if (json_find(j, "freq_hz", v)) freq = v;
            chat = "[{\"role\":\"user\",\"kind\":\"investigate\",\"text\":\"Investigate the signal at " +
                   fmt_num(atof(freq.c_str()) / 1e6, 5) + " MHz\"}";
            if (!analysis.empty())
                chat += ",{\"role\":\"assistant\",\"kind\":\"investigate\",\"text\":\"" + json_escape(analysis) + "\"}";
            chat += "]";
        }
        o << ",\"chat\":" << chat;
        // the agent's steps: the last 600 lines of activity.jsonl
        std::string act = read_file(dir + "/activity.jsonl", 8 << 20);
        std::vector<std::string> lines;
        std::stringstream ss(act);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] != '{' || line.size() > 20000) continue;
            lines.push_back(line);
        }
        size_t from = lines.size() > 600 ? lines.size() - 600 : 0;
        o << ",\"activity\":[";
        bool first = true;
        for (size_t i = from; i < lines.size(); i++) {
            if (!json_valid(lines[i])) continue;
            o << (first ? "" : ",") << lines[i];
            first = false;
        }
        o << "]}}";
        WebSocketServer::broadcast_json_message(o.str());
    }).detach();
}

std::string AiManager::select_session(const std::string& id) {
    if (!valid_session_id(id)) return "bad conversation id";
    if (access((std::string(SESSIONS) + "/" + id + "/session.json").c_str(), R_OK) != 0) return "no such conversation";
    if (busy_.load()) return "the assistant is answering - stop it first";
    load_session(id);
    WebSocketServer::broadcast_json_message(state_json(false));
    return "";
}

void AiManager::new_conversation() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (busy_.load()) return;
        fresh_ = true;
        session_.clear();
        title_.clear();
        session_state_.clear();
        log_.clear();
    }
    WebSocketServer::broadcast_json_message(state_json(true));
}

std::string AiManager::delete_session(const std::string& id) {
    if (!valid_session_id(id)) return "bad conversation id";
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (busy_.load() && id == session_) return "the assistant is answering in that conversation - stop it first";
    }
    std::thread([this, id] {
        std::string out;
        run_bridge({BRIDGE, "delete", "--session", std::string(SESSIONS) + "/" + id}, 30000, &out);
        bool current = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            current = id == session_;
            if (current) {
                session_.clear();
                title_ = session_state_ = "";
                fresh_ = true;
            }
        }
        std::cout << "AI assistant: deleted conversation " << id << std::endl;
        sessions_async();
        if (current) WebSocketServer::broadcast_json_message(state_json(false));
    }).detach();
    return "";
}

std::string AiManager::start_job(const std::string& kind, const std::vector<std::string>& argv,
                                 const std::string& label) {
    if (busy_.load()) return "the assistant is busy - stop the running answer first";
    if (thread_.joinable()) thread_.join();   // previous job's reader (already done)
    // no interpreter (Windows: kproc::python_argv found none)
    if (argv.empty()) return "Python 3 was not found - install it, or set KRAKEN_PYTHON to python.exe";
    auto proc = std::make_shared<kproc::Process>();
    kproc::SpawnOptions o;
    o.argv = argv;
    o.search_path = true;
    o.in = kproc::Stdio::Null;
    o.out = kproc::Stdio::Pipe;
    o.err = kproc::Stdio::SameAsStdout;
    o.kill_tree = true;   // own group: Stop kills the agent too
    o.console = kproc::Console::Share;
    // the environment + where the MCP server is (the bridge writes the
    // agent's --mcp-config from these; never on a command line): every
    // KRAKEN_MCP_* we inherited is left out
    for (char** e = environ; e && *e; e++)
        if (strncmp(*e, "KRAKEN_MCP_", 11) == 0) {
            const char* eq = strchr(*e, '=');
            o.env_unset.emplace_back(*e, eq ? static_cast<size_t>(eq - *e) : strlen(*e));
        }
    o.env_set = {{"KRAKEN_MCP_URL", ai_mcp::url()}, {"KRAKEN_MCP_TOKEN", ai_mcp::token()}};
#ifdef _WIN32
    // UTF-8 in the bridge's own Python children too (sigtool.py, the agent's
    // scripts): their pipes / files use the ANSI code page otherwise
    o.env_set.push_back({"PYTHONUTF8", "1"});
#endif
    std::string e;
    if (!proc->spawn(o, &e)) return std::string("cannot start ") + argv[0] + ": " + e;
    {
        std::lock_guard<std::mutex> lk(mu_);
        proc_ = proc;
        job_kind_ = kind;
        job_label_ = label;
        job_started_ms_ = wall_ms();
    }
    busy_ = true;
    if (kind == "chat") aisandbox::turn_started(wall_ms());
    std::cout << "AI assistant: started " << kind << " (" << label << ")" << std::endl;
    on_line(kind, "{\"ev\":\"start\",\"label\":\"" + json_escape(label) + "\"}");
    WebSocketServer::broadcast_json_message(state_json(false));
    thread_ = std::thread([this, proc, kind] { reader(proc, kind); });
    return "";
}

void AiManager::reader(std::shared_ptr<kproc::Process> proc, std::string kind) {
    std::string buf;
    char tmp[8192];
    for (;;) {
        const ptrdiff_t n = proc->read(kproc::Stream::Out, tmp, sizeof tmp, -1);   // blocks
        if (n <= 0) break;   // EOF (or an error)
        buf.append(tmp, static_cast<size_t>(n));
        size_t p;
        while ((p = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, p);
            buf.erase(0, p + 1);
            if (!line.empty()) on_line(kind, line);
        }
        if (buf.size() > (1u << 20)) buf.clear();
    }
    if (!buf.empty()) on_line(kind, buf);
    const kproc::ExitStatus st = proc->wait();
    proc->close();
    finished(kind, st);
}

void AiManager::on_line(const std::string& kind, const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    std::string ev;
    // the bridge's own JSON events pass through; anything else (a Python
    // traceback) becomes a text event
    if (line.size() >= 2 && line.front() == '{' && line.back() == '}' && json_find(line, "ev", ev) && json_valid(line)) {
        // keep it
    } else {
        if (line.size() > 2000) line.resize(2000);
        line = "{\"ev\":\"text\",\"text\":\"" + json_escape(line) + "\"}";
        ev = "text";
    }
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (ev == "done") {
            std::string v;
            last_result_ = line;
            if (json_find(line, "title", v) && !v.empty()) title_ = v;
            session_state_ = json_find(line, "ok", v) && v == "true" ? "answered" : "failed";
        } else if (ev == "session") {
            std::string v;
            if (json_find(line, "title", v) && !v.empty()) title_ = v;
        }
        seq_++;
        msg = "{\"seq\":" + std::to_string(seq_) + ",\"kind\":\"" + json_escape(kind) + "\",\"t\":" +
              std::to_string(wall_ms()) + ",\"e\":" + line + "}";
        // streamed text pieces aren't kept for pages opened later (the
        // complete text follows as one "text" event)
        if (ev != "delta") {
            log_.push_back(msg);
            while (log_.size() > LOG_MAX) log_.pop_front();
        }
    }
    WebSocketServer::broadcast_json_message("{\"ai_event\":" + msg + "}");
}

void AiManager::finished(const std::string& kind, const kproc::ExitStatus& st) {
    const int code = st.shell_code();   // exit status, or 128 + the signal
    on_line(kind, "{\"ev\":\"exit\",\"code\":" + std::to_string(code) + "}");
    std::string sid, result;
    {
        std::lock_guard<std::mutex> lk(mu_);
        proc_.reset();
        sid = session_;
        result = last_result_;
        view_ = "{}";
        running_dir_.clear();
    }
    if (kind == "chat") aisandbox::turn_finished();
    busy_ = false;
    std::cout << "AI assistant: " << kind << " finished (" << code << ")" << std::endl;
    // a turn that touched plugin folders (the bridge lists them): the plugin
    // list again - running decoders restart on a rebuilt executable
    std::string touched;
    if (json_find(result, "plugins", touched) && touched.size() > 2) {
        dig::PluginRegistry::instance().scan();   // this reader thread, not the uWS loop
        WebSocketServer::broadcast_json_message(plugins_message());
    }
    if (kind == "chat") {
        sessions_async();
        if (!sid.empty()) session_async(sid);   // the updated conversation
    }
    WebSocketServer::broadcast_json_message(state_json(false));
}

void AiManager::cancel() {
    std::shared_ptr<kproc::Process> p;
    {
        std::lock_guard<std::mutex> lk(mu_);
        p = proc_;
    }
    if (!p || !busy_.load()) return;
    // SIGTERM to the group (Windows: CTRL_BREAK when it shares our console,
    // else its job is ended at once), 3 s later SIGKILL
    p->terminate();
    std::thread([this, p] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> lk(mu_);
        if (proc_ == p) p->kill();
    }).detach();
}

std::string AiManager::ask(const std::string& text_in, const std::string& view_json, bool new_conv, const std::string& page) {
    if (!enabled()) return NOT_ENABLED;
    if (busy_.load()) return "the assistant is answering - wait or press Stop";
    const std::string text = clean_text(text_in, 8000);
    if (text.find_first_not_of(" \n") == std::string::npos) return "empty question";
    std::string view = view_json;
    {
        size_t p = view.find_first_not_of(" \t\r\n");
        if (view.size() > 16384 || p == std::string::npos || view[p] != '{' || !json_valid(view)) view = "{}";
    }
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (new_conv) fresh_ = true;
        if (fresh_ || session_.empty() || !valid_session_id(session_)) {
            char stamp[32];
            time_t t = time(nullptr);
            strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
            sid = stamp;
            for (int k = 2; access((std::string(SESSIONS) + "/" + sid).c_str(), F_OK) == 0 && k < 100; k++)
                sid = std::string(stamp) + "-" + std::to_string(k);
            session_ = sid;
            title_.clear();
            log_.clear();
            fresh_ = false;
        } else {
            sid = session_;
        }
        session_state_ = "answering";
        view_ = view;
        asker_page_ = valid_page_id(page) ? page : std::string();
        if (!asker_page_.empty()) last_page_ = asker_page_;
        running_dir_ = std::string(SESSIONS) + "/" + sid;
    }
    mkdir(SESSIONS, 0755);
    mkdir((std::string(SESSIONS) + "/" + sid).c_str(), 0755);
    // what is on screen, for the agent's first look (the same as its
    // get_screen tool)
    const std::string ctx = ai_mcp::screen_json(view);
    std::vector<std::string> args{BRIDGE, "chat", "--session", std::string(SESSIONS) + "/" + sid,
                                  "--text", text, "--context-json", ctx};
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!model_.empty()) args.insert(args.end(), {"--model", model_});
        if (!effort_.empty()) args.insert(args.end(), {"--effort", effort_});
    }
#ifdef _WIN32
    // too long for a Windows command line: the context in a file of the
    // conversation folder, which the bridge reads and deletes (--context-file)
    if (cmdline_estimate(args) > 32000) {
        const std::string cf = std::string(SESSIONS) + "/" + sid + "/context.json";
        std::ofstream f(cf, std::ios::binary | std::ios::trunc);
        f << ctx;
        f.close();
        if (f) {
            args[6] = "--context-file";   // in place of --context-json
            args[7] = cf;
        } else {
            args[7] = "{}";
        }
    }
#endif
    const std::vector<std::string> argv = kproc::python_cmd(std::move(args));   // python3 + args
    std::string label = text.substr(0, 80);
    std::string err = start_job("chat", argv, label);
    if (!err.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        session_state_ = "failed";
        view_ = "{}";
        running_dir_.clear();
    }
    return err;
}

std::string AiManager::test() {
    if (!enabled()) return NOT_ENABLED;
    return start_job("test", kproc::python_cmd({BRIDGE, "test"}), "testing the LLM connection");
}

void AiManager::set_readonly(bool on) {
    readonly_ = on;
    WebSocketServer::broadcast_json_message(state_json(false));
}

// a Claude model alias or full name ("sonnet", "claude-opus-5-5", "...[1m]")
bool AiManager::valid_model(const std::string& m) {
    if (m.empty()) return true;
    if (m.size() > 64 || !std::isalnum(static_cast<unsigned char>(m[0]))) return false;
    for (char c : m)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_' || c == '[' || c == ']')) return false;
    return true;
}
bool AiManager::valid_effort(const std::string& e) {
    return e.empty() || e == "low" || e == "medium" || e == "high" || e == "xhigh" || e == "max";
}
bool AiManager::set_model(const std::string& m) {
    if (!valid_model(m)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        model_ = m;
    }
    WebSocketServer::broadcast_json_message(state_json(false));
    return true;
}
bool AiManager::set_effort(const std::string& e) {
    if (!valid_effort(e)) return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        effort_ = e;
    }
    WebSocketServer::broadcast_json_message(state_json(false));
    return true;
}
bool AiManager::valid_page_id(const std::string& id) {
    if (id.size() < 8 || id.size() > 32) return false;
    for (char c : id)
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}
void AiManager::note_page(const std::string& id) {
    if (!valid_page_id(id)) return;
    std::lock_guard<std::mutex> lk(mu_);
    last_page_ = id;
}
std::string AiManager::target_page() const {
    std::lock_guard<std::mutex> lk(mu_);
    return busy_.load() && !asker_page_.empty() ? asker_page_ : last_page_;
}

std::string AiManager::view_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    return view_.empty() ? "{}" : view_;
}

std::string AiManager::conversation_dir() const {
    std::lock_guard<std::mutex> lk(mu_);
    return busy_.load() ? running_dir_ : std::string();
}

std::string AiManager::plugins_message() {
    return "{\"plugins\":" + dig::PluginRegistry::instance().list_json() + ",\"plugin_dir\":\"" +
           json_escape(dig::PluginRegistry::dir()) + "\"}";
}

std::string AiManager::state_json(bool with_log) const {
    std::string backend = "claude", model, probe;
    int64_t probe_ms;
    std::string v;
    if (config_value("backend", v)) backend = v;
    if (config_value("model", v)) model = v;
    {
        std::lock_guard<std::mutex> lk(g_probe_mu);
        probe = g_probe;
        probe_ms = g_probe_ms;
    }
    if (wall_ms() - probe_ms > 300000) probe_async();   // installed / logged in meanwhile?
    std::string cli, version, sandbox, sandbox_why, d_model, d_name, d_from, d_effort, d_info;
    json_find(probe, "cli", cli);
    json_find(probe, "version", version);
    json_find(probe, "sandbox", sandbox);
    json_find(probe, "sandbox_why", sandbox_why);
    // what "Default" in the dock's model / effort lists runs (kraken_ai.py cli_defaults)
    json_find(probe, "default_model", d_model);
    json_find(probe, "default_model_name", d_name);
    json_find(probe, "default_model_from", d_from);
    json_find(probe, "effort_fixed", d_effort);
    json_find(probe, "model_info", d_info);
    std::ostringstream o;
    std::lock_guard<std::mutex> lk(mu_);
    o << "{\"ai_state\":{\"enabled\":" << (enabled() ? "true" : "false") << ",\"backend\":\"" << json_escape(backend)
      << "\",\"model\":\"" << json_escape(model) << "\",\"model_sel\":\"" << json_escape(model_) << "\",\"effort\":\""
      << json_escape(effort_) << "\",\"defaults\":{\"model\":\"" << json_escape(d_model) << "\",\"name\":\""
      << json_escape(d_name) << "\",\"from\":\"" << json_escape(d_from) << "\",\"effort\":\"" << json_escape(d_effort)
      << "\",\"info\":\"" << json_escape(d_info) << "\"},\"cli\":\"" << json_escape(cli) << "\",\"version\":\""
      << json_escape(version) << "\",\"sandbox\":" << (sandbox == "true" ? "true" : "false") << ",\"sandbox_why\":\""
      << json_escape(sandbox_why) << "\",\"token\":" << (WebSocketServer::auth_enabled() ? "true" : "false")
      << ",\"busy\":" << (busy_.load() ? "true" : "false") << ",\"readonly\":" << (readonly_.load() ? "true" : "false")
      << ",\"job\":{\"kind\":\"" << json_escape(job_kind_) << "\",\"label\":\"" << json_escape(job_label_)
      << "\",\"started\":" << job_started_ms_ << "}"
      << ",\"session\":{\"id\":\"" << json_escape(fresh_ ? std::string() : session_) << "\",\"title\":\""
      << json_escape(fresh_ ? std::string() : title_) << "\",\"state\":\"" << json_escape(session_state_) << "\"}"
      << ",\"seq\":" << seq_;
    if (with_log) {
        o << ",\"log\":[";
        bool first = true;
        for (const auto& l : log_) {
            o << (first ? "" : ",") << l;
            first = false;
        }
        o << "]";
    }
    o << "}}";
    return o.str();
}

// --- identifications (ai/identifications.json) ---

std::string AiManager::mark_json(const Mark& m) {
    std::ostringstream o;
    o << "{\"id\":\"" << json_escape(m.id) << "\",\"hz\":" << fmt_num(m.hz, 0) << ",\"bw\":" << fmt_num(m.bw, 0)
      << ",\"name\":\"" << json_escape(m.name) << "\",\"summary\":\"" << json_escape(m.summary) << "\",\"conv\":\""
      << json_escape(m.conv) << "\",\"t\":" << m.t << ",\"brg\":" << (m.brg >= 0 ? fmt_num(m.brg, 1) : "null")
      << ",\"conf\":\"" << json_escape(m.conf) << "\"}";
    return o.str();
}

void AiManager::load_marks() {
    geo::Json j;
    std::string text = read_file(MARKS, 8 << 20);
    std::vector<Mark> out;
    if (!text.empty() && geo::json_parse(text, &j) && j.type == geo::Json::OBJ) {
        if (const geo::Json* a = j.get("marks"); a && a->type == geo::Json::ARR) {
            for (const auto& e : a->a) {
                if (e.type != geo::Json::OBJ) continue;
                Mark m;
                m.id = e.str("id");
                m.name = clean_text(e.str("name"), 80);
                m.summary = clean_text(e.str("summary"), 2000);
                m.conv = e.str("conv");
                m.conf = e.str("conf");
                if (const auto* x = e.get("hz"); x && x->type == geo::Json::NUM) m.hz = x->n;
                if (const auto* x = e.get("bw"); x && x->type == geo::Json::NUM) m.bw = x->n;
                if (const auto* x = e.get("brg"); x && x->type == geo::Json::NUM) m.brg = x->n;
                if (const auto* x = e.get("t"); x && x->type == geo::Json::NUM) m.t = static_cast<int64_t>(x->n);
                if (m.id.empty() || m.name.empty() || !(m.hz > 0)) continue;   // a damaged entry
                out.push_back(m);
            }
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    marks_ = std::move(out);
}

void AiManager::save_marks_locked() const {
    std::string s = "{\"version\":1,\"marks\":[\n";
    for (size_t i = 0; i < marks_.size(); i++) s += (i ? ",\n" : "") + mark_json(marks_[i]);
    s += "\n]}\n";
    const std::string tmp = std::string(MARKS) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << s;
        if (!f) return;
    }
    fs_replace(tmp, MARKS);   // rename(); Windows: also over the old file
}

std::string AiManager::remember(double hz, double bw_hz, const std::string& name_in, const std::string& summary_in,
                                double bearing_deg, const std::string& confidence, std::string* id_out) {
    if (!(hz >= 1e3 && hz <= 1e11)) return "frequency out of range";
    const std::string name = clean_text(name_in, 80), summary = clean_text(summary_in, 2000);
    if (name.find_first_not_of(" \n") == std::string::npos) return "a name is needed";
    std::string conf = lower(confidence);
    if (conf != "low" && conf != "medium" && conf != "high") conf = "";
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(mu_);
        Mark m;
        m.hz = std::round(hz);
        m.bw = is_finite_value(bw_hz) && bw_hz > 0 ? std::min(bw_hz, 1e8) : 0;
        m.brg = is_finite_value(bearing_deg) && bearing_deg >= 0 ? std::fmod(bearing_deg, 360.0) : -1;
        m.name = name;
        m.summary = summary;
        m.conf = conf;
        m.conv = busy_.load() ? session_ : std::string();
        m.t = wall_ms();
        // the same signal again (same name, within its half bandwidth / 2
        // kHz): updated in place
        const double tol = std::max(2000.0, m.bw / 2);
        auto it = std::find_if(marks_.begin(), marks_.end(), [&](const Mark& x) {
            return lower(x.name) == lower(name) && std::fabs(x.hz - m.hz) <= tol;
        });
        if (it != marks_.end()) {
            m.id = it->id;
            marks_.erase(it);
        } else {
            m.id = random_hex(4);
        }
        marks_.push_back(m);
        while (marks_.size() > MARKS_MAX) marks_.erase(marks_.begin());
        save_marks_locked();
        if (id_out) *id_out = m.id;
    }
    WebSocketServer::broadcast_json_message(marks_message());
    return "";
}

std::string AiManager::forget(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = std::find_if(marks_.begin(), marks_.end(), [&](const Mark& x) { return x.id == id; });
        if (it == marks_.end()) return "no such identification";
        marks_.erase(it);
        save_marks_locked();
    }
    WebSocketServer::broadcast_json_message(marks_message());
    return "";
}

std::string AiManager::identifications_json(double hz, double within_hz, const std::string& query, size_t limit) const {
    std::vector<const Mark*> sel;
    std::lock_guard<std::mutex> lk(mu_);
    const std::string q = lower(query);
    for (const auto& m : marks_) {
        if (hz > 0 && std::fabs(m.hz - hz) > std::max(within_hz, m.bw / 2)) continue;
        if (!q.empty() && lower(m.name + " " + m.summary).find(q) == std::string::npos) continue;
        sel.push_back(&m);
    }
    if (hz > 0)
        std::sort(sel.begin(), sel.end(), [hz](const Mark* a, const Mark* b) { return std::fabs(a->hz - hz) < std::fabs(b->hz - hz); });
    else
        std::reverse(sel.begin(), sel.end());   // newest first
    std::string out = "[";
    for (size_t i = 0; i < sel.size() && i < limit; i++) out += (i ? "," : "") + mark_json(*sel[i]);
    return out + "]";
}

std::string AiManager::marks_message() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out = "{\"ai_marks\":[";
    for (size_t i = 0; i < marks_.size(); i++) out += (i ? "," : "") + mark_json(marks_[i]);
    return out + "]}";
}
