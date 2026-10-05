#include "ai_manager.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.hpp"
#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "digital/dig_plugin.hpp"
#include "globals.hpp"
#include "networking/websocket_server.hpp"
#include "utils/json_escape.hpp"

extern char** environ;

namespace {

constexpr const char* BRIDGE = "ai/kraken_ai.py";
constexpr const char* SESSIONS = "ai/sessions";
constexpr size_t LOG_MAX = 300;
constexpr size_t BUNDLE_FILE_MAX = 512 * 1024;
constexpr size_t BUNDLE_TOTAL_MAX = 1024 * 1024;   // < the WebSocket backpressure limit

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

// Files a plugin bundle may carry: plain names, source / docs only
bool bundle_name_ok(const std::string& n) {
    if (n.empty() || n.size() > 64 || n[0] == '.') return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    auto ends = [&](const char* x) {
        size_t l = strlen(x);
        return n.size() > l && n.compare(n.size() - l, l, x) == 0;
    };
    return ends(".cpp") || ends(".hpp") || ends(".h") || ends(".md") || ends(".txt") || ends(".json");
}

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

// cached `kraken_ai.py status` (CLI path + version)
std::mutex g_probe_mu;
std::string g_probe;
int64_t g_probe_ms = 0;
std::atomic<bool> g_probing{false};

void probe_async() {
    if (g_probing.exchange(true)) return;
    std::thread([] {
        std::string out;
        dig::run_and_capture({"/usr/bin/env", "python3", BRIDGE, "status"}, 30000, &out);
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
    if (pid_ > 0) killpg(pid_, SIGKILL);
    if (thread_.joinable()) thread_.detach();
}

bool AiManager::enabled() const {
    std::string v;
    return config_value("enabled", v) && v == "true";
}

void AiManager::init() {
    mkdir(SESSIONS, 0755);
    // newest session (names start with a timestamp)
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
    probe_async();
}

void AiManager::load_session(const std::string& id) {
    std::string j = read_file(std::string(SESSIONS) + "/" + id + "/session.json");
    std::string v;
    std::lock_guard<std::mutex> lk(mu_);
    session_ = id;
    analysis_ = signal_name_ = suggested_id_ = plugin_id_ = session_state_ = "";
    session_freq_ = 0;
    session_vfo_ = -1;
    if (json_find(j, "analysis", v)) analysis_ = v;
    if (json_find(j, "signal_name", v)) signal_name_ = v;
    if (json_find(j, "suggested_plugin_id", v)) suggested_id_ = v;
    if (json_find(j, "plugin_id", v)) plugin_id_ = v;
    if (json_find(j, "state", v)) session_state_ = v;
    if (json_find(j, "freq_hz", v)) session_freq_ = atof(v.c_str());
    if (json_find(j, "vfo", v)) session_vfo_ = atoi(v.c_str());
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
                std::string v, freq = "0", vfo = "-1";
                std::ostringstream o;
                if (json_find(j, "freq_hz", v)) freq = std::to_string(static_cast<long long>(atof(v.c_str())));
                if (json_find(j, "vfo", v) && !v.empty() && v != "null") vfo = std::to_string(atoi(v.c_str()));
                o << "{\"id\":\"" << json_escape(id) << "\",\"freq_hz\":" << freq << ",\"vfo\":" << vfo;
                for (const char* k : {"signal_name", "created", "state", "plugin_id", "suggested_plugin_id"}) {
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
                                                    "\",\"error\":\"no such session\"}}");
            return;
        }
        std::string v, freq = "0";
        if (json_find(j, "freq_hz", v)) freq = std::to_string(static_cast<long long>(atof(v.c_str())));
        std::ostringstream o;
        o << "{\"ai_session\":{\"id\":\"" << json_escape(id) << "\",\"freq_hz\":" << freq;
        for (const char* k : {"signal_name", "created", "state", "plugin_id", "suggested_plugin_id"}) {
            v.clear();
            json_find(j, k, v);
            o << ",\"" << k << "\":\"" << json_escape(v) << "\"";
        }
        // the conversation: chat.json (written by the bridge - checked, the
        // agent can edit files in its session folder); older sessions only
        // have the analysis
        std::string chat = read_file(dir + "/chat.json", 4 << 20);
        size_t p = chat.find_first_not_of(" \t\r\n");
        if (p == std::string::npos || chat[p] != '[' || !json_valid(chat)) {
            std::string analysis;
            json_find(j, "analysis", analysis);
            char mhz[32];
            snprintf(mhz, sizeof mhz, "%.5f", atof(freq.c_str()) / 1e6);
            chat = "[{\"role\":\"user\",\"kind\":\"investigate\",\"text\":\"Investigate the signal at " +
                   std::string(mhz) + " MHz\"}";
            if (!analysis.empty())
                chat += ",{\"role\":\"assistant\",\"kind\":\"investigate\",\"text\":\"" + json_escape(analysis) + "\"}";
            chat += "]";
        }
        o << ",\"chat\":" << chat;
        // the agent's steps: the last 400 lines of activity.jsonl
        std::string act = read_file(dir + "/activity.jsonl", 8 << 20);
        std::vector<std::string> lines;
        std::stringstream ss(act);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty() || line[0] != '{' || line.size() > 20000) continue;
            lines.push_back(line);
        }
        size_t from = lines.size() > 400 ? lines.size() - 400 : 0;
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
    if (!valid_session_id(id)) return "bad session id";
    if (access((std::string(SESSIONS) + "/" + id + "/session.json").c_str(), R_OK) != 0) return "no such session";
    if (busy_.load()) return "the AI Signal Lab is busy - stop the running job first";
    load_session(id);
    WebSocketServer::broadcast_json_message(state_json(false));
    return "";
}

std::string AiManager::delete_session(const std::string& id) {
    if (!valid_session_id(id)) return "bad session id";
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (busy_.load() && id == session_) return "the AI is working on that session - stop it first";
    }
    std::thread([this, id] {
        std::string out;
        dig::run_and_capture({"/usr/bin/env", "python3", BRIDGE, "delete", "--session", std::string(SESSIONS) + "/" + id},
                             30000, &out);
        bool current = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            current = id == session_;
            if (current) {
                session_.clear();
                analysis_ = signal_name_ = suggested_id_ = plugin_id_ = session_state_ = "";
                session_freq_ = 0;
                session_vfo_ = -1;
            }
        }
        std::cout << "AI Signal Lab: deleted session " << id << std::endl;
        sessions_async();
        if (current) WebSocketServer::broadcast_json_message(state_json(false));
    }).detach();
    return "";
}

std::string AiManager::start_job(const std::string& kind, const std::vector<std::string>& argv,
                                 const std::string& label) {
    if (busy_.load()) return "the AI Signal Lab is busy - stop the running job first";
    if (thread_.joinable()) thread_.join();   // previous job's reader (already done)
    signal(SIGPIPE, SIG_IGN);
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return "pipe failed";
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);   // own group: Stop kills the agent too
    posix_spawnattr_setpgroup(&at, 0);
    std::vector<char*> av;
    for (const auto& s : argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, av[0], &fa, &at, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(p[1]);
    if (rc != 0) {
        close(p[0]);
        return std::string("cannot start ") + argv[0] + ": " + strerror(rc);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        pid_ = pid;
        job_kind_ = kind;
        job_label_ = label;
        job_started_ms_ = wall_ms();
        log_.clear();
    }
    busy_ = true;
    std::cout << "AI Signal Lab: started " << kind << " (" << label << ")" << std::endl;
    on_line(kind, "{\"ev\":\"start\",\"label\":\"" + json_escape(label) + "\"}");
    WebSocketServer::broadcast_json_message(state_json(false));
    thread_ = std::thread([this, fd = p[0], pid, kind] { reader(fd, pid, kind); });
    return "";
}

void AiManager::reader(int fd, pid_t pid, std::string kind) {
    std::string buf;
    char tmp[8192];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
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
    close(fd);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    finished(kind, st);
}

void AiManager::on_line(const std::string& kind, const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    std::string ev;
    // the bridge's own JSON events pass through; anything else (make output,
    // a Python traceback) becomes a text event
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
            std::string k;
            json_find(line, "kind", k);
            bool ok = json_find(line, "ok", v) && v == "true";
            if (k == "investigate") {
                if (json_find(line, "analysis", v)) analysis_ = v;
                if (json_find(line, "signal_name", v)) signal_name_ = v;
                if (json_find(line, "plugin_id", v)) suggested_id_ = v;
                session_state_ = ok ? "investigated" : "failed";
            } else if (k == "create") {
                if (json_find(line, "plugin_id", v)) plugin_id_ = v;
                session_state_ = ok ? "plugin ready" : "plugin failed";
            }
        }
        seq_++;
        msg = "{\"seq\":" + std::to_string(seq_) + ",\"kind\":\"" + json_escape(kind) + "\",\"t\":" +
              std::to_string(wall_ms()) + ",\"e\":" + line + "}";
        log_.push_back(msg);
        while (log_.size() > LOG_MAX) log_.pop_front();
    }
    WebSocketServer::broadcast_json_message("{\"ai_event\":" + msg + "}");
}

void AiManager::finished(const std::string& kind, int st) {
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    on_line(kind, "{\"ev\":\"exit\",\"code\":" + std::to_string(code) + "}");
    {
        std::lock_guard<std::mutex> lk(mu_);
        pid_ = -1;
    }
    busy_ = false;
    std::cout << "AI Signal Lab: " << kind << " finished (" << code << ")" << std::endl;
    if (kind == "create" || kind == "build" || kind == "import") {
        dig::PluginRegistry::instance().scan();   // this reader thread, not the uWS loop
        WebSocketServer::broadcast_json_message(plugins_message());
    }
    if (kind == "investigate" || kind == "create" || kind == "ask") {
        sessions_async();
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sid = session_;
        }
        if (!sid.empty()) session_async(sid);   // the updated conversation
    }
    WebSocketServer::broadcast_json_message(state_json(false));
}

void AiManager::cancel() {
    pid_t pid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pid = pid_;
    }
    if (pid <= 0 || !busy_.load()) return;
    killpg(pid, SIGTERM);
    std::thread([this, pid] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> lk(mu_);
        if (pid_ == pid) killpg(pid, SIGKILL);
    }).detach();
}

std::string AiManager::investigate(int vfo_id, double freq_hz, double seconds, const std::string& instructions) {
    if (!enabled()) return "the AI Signal Lab is not enabled on this receiver (run: python3 ai/kraken_ai.py setup)";
    auto list = decimator_manager.getDecimatorInfoList();
    const DecimatorManager::DecimatorInfo* vfo = nullptr;
    for (const auto& d : list)
        if (d.id == vfo_id) vfo = &d;
    if (!vfo) return "no such VFO";
    const double center = ChannelManager::get_frequency(0);
    const double vfo_freq = center + vfo->frequency_offset_hz;
    if (freq_hz <= 0) freq_hz = vfo_freq;
    if (std::fabs(freq_hz - center) > SAMPLE_RATE / 2 - 5000)
        return "the frequency is outside the receiver's current span - tune the receiver there first";
    seconds = std::clamp(seconds, 2.0, 60.0);
    double rate = std::clamp(vfo->bandwidth_mhz * 1e6, 12500.0, 600000.0);

    // receiver context for the agent
    std::ostringstream ctx;
    ctx << "{\"vfo_id\":" << vfo->id << ",\"vfo_freq_hz\":" << static_cast<long long>(vfo_freq)
        << ",\"vfo_offset_hz\":" << vfo->frequency_offset_hz << ",\"vfo_rate_hz\":" << rate
        << ",\"demod\":\"" << DecimatorManager::demodModeToString(vfo->demod_mode) << "\""
        << ",\"center_freq_hz\":" << static_cast<long long>(center)
        << ",\"num_elements\":" << active_num_elements.load()
        << ",\"squelch_db\":" << vfo->squelch_level;
    if (auto inst = decimator_manager.getDecimator(vfo_id)) {
        if (auto dd = inst->getDigital(); dd && dd->mode() != dig::Mode::OFF) ctx << ",\"digital\":" << dd->status_json(0, 0);
    }
    ctx << ",\"plugins\":" << dig::PluginRegistry::instance().list_json() << "}";

    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
    std::string sid = std::string(stamp) + "-" + std::to_string(static_cast<long long>(freq_hz / 1000)) + "kHz";
    std::string sdir = std::string(SESSIONS) + "/" + sid;
    mkdir(SESSIONS, 0755);
    char fbuf[64], rbuf[32], sbuf[32];
    snprintf(fbuf, sizeof fbuf, "%.0f", freq_hz);
    snprintf(rbuf, sizeof rbuf, "%.0f", rate);
    snprintf(sbuf, sizeof sbuf, "%.0f", seconds);
    std::vector<std::string> argv{"python3", BRIDGE, "investigate", "--session", sdir, "--freq", fbuf, "--rate", rbuf,
                                  "--seconds", sbuf, "--context-json", ctx.str()};
    if (!instructions.empty()) {
        argv.push_back("--instructions");
        argv.push_back(instructions);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (busy_.load()) return "the AI Signal Lab is busy - stop the running job first";
        session_ = sid;
        session_freq_ = freq_hz;
        session_vfo_ = vfo_id;
        analysis_.clear();
        signal_name_.clear();
        suggested_id_.clear();
        plugin_id_.clear();
        session_state_ = "investigating";
    }
    char label[96];
    snprintf(label, sizeof label, "investigating %.5f MHz (VFO %d)", freq_hz / 1e6, vfo_id);
    return start_job("investigate", argv, label);
}

std::string AiManager::create(const std::string& plugin_id, const std::string& instructions) {
    if (!enabled()) return "the AI Signal Lab is not enabled on this receiver";
    if (!dig::PluginRegistry::valid_id(plugin_id)) return "plugin id: 1-32 characters a-z 0-9 _ - (not \"sdk\")";
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sid = session_;
        if (sid.empty() || analysis_.empty()) return "investigate a signal first";
    }
    std::vector<std::string> argv{"python3", BRIDGE, "create", "--session", std::string(SESSIONS) + "/" + sid,
                                  "--plugin", plugin_id};
    if (!instructions.empty()) {
        argv.push_back("--instructions");
        argv.push_back(instructions);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        plugin_id_ = plugin_id;
        session_state_ = "creating";
    }
    return start_job("create", argv, "creating decoder plugin " + plugin_id);
}

std::string AiManager::ask(const std::string& question) {
    if (!enabled()) return "the AI Signal Lab is not enabled on this receiver";
    std::string sid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sid = session_;
    }
    if (sid.empty()) return "investigate a signal first";
    return start_job("ask", {"python3", BRIDGE, "ask", "--session", std::string(SESSIONS) + "/" + sid, "--text", question},
                     "follow-up question");
}

std::string AiManager::test() {
    if (!enabled()) return "the AI Signal Lab is not enabled on this receiver (run: python3 ai/kraken_ai.py setup)";
    return start_job("test", {"python3", BRIDGE, "test"}, "testing the LLM connection");
}

std::string AiManager::build_plugin(const std::string& plugin_id) {
    if (!enabled()) return "building plugins from the web UI needs the AI Signal Lab enabled (python3 ai/kraken_ai.py setup)";
    if (!dig::PluginRegistry::valid_id(plugin_id)) return "bad plugin id";
    return start_job("build", {"make", "-C", dig::PluginRegistry::dir(), "PLUGIN=" + plugin_id}, "building plugin " + plugin_id);
}

std::string AiManager::export_plugin(const std::string& id, std::string* bundle) {
    if (!dig::PluginRegistry::valid_id(id)) return "bad plugin id";
    std::string pdir = dig::PluginRegistry::dir() + "/" + id;
    std::ostringstream o;
    o << "{\"kraken_plugin\":1,\"id\":\"" << json_escape(id) << "\",\"exported\":\"";
    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", localtime(&t));
    o << stamp << "\",\"files\":{";
    DIR* d = opendir(pdir.c_str());
    if (!d) return "no such plugin";
    std::vector<std::string> names;
    while (dirent* e = readdir(d))
        if (bundle_name_ok(e->d_name)) names.push_back(e->d_name);
    closedir(d);
    std::sort(names.begin(), names.end());
    size_t total = 0;
    bool first = true;
    for (const auto& n : names) {
        std::string c = read_file(pdir + "/" + n, BUNDLE_FILE_MAX + 1);
        if (c.size() > BUNDLE_FILE_MAX) continue;
        total += c.size();
        if (total > BUNDLE_TOTAL_MAX) return "plugin too large to export";
        o << (first ? "" : ",") << "\"" << json_escape(n) << "\":\"" << json_escape(c) << "\"";
        first = false;
    }
    o << "}}";
    if (first) return "the plugin has no source files";
    *bundle = o.str();
    return "";
}

std::string AiManager::import_plugin(const std::string& j) {
    if (!enabled()) return "importing plugins (native code) needs the AI Signal Lab enabled on the Pi "
                           "(python3 ai/kraken_ai.py setup) - or copy the folder into plugins/ and run make";
    if (busy_.load()) return "the AI Signal Lab is busy";
    std::string v, id;
    if (!json_find(j, "kraken_plugin", v) || v != "1") return "not a KrakenSDR plugin bundle";
    if (!json_find(j, "id", id) || !dig::PluginRegistry::valid_id(id)) return "bad plugin id in the bundle";
    bool replace = json_find(j, "replace", v) && v == "true";
    // "files":{"name":"content",...} - walk the object's string pairs
    size_t p = j.find("\"files\"");
    if (p == std::string::npos) return "bundle has no files";
    p = j.find('{', p);
    if (p == std::string::npos) return "bundle has no files";
    p++;
    std::vector<std::pair<std::string, std::string>> files;
    size_t total = 0;
    for (;;) {
        while (p < j.size() && (std::isspace(static_cast<unsigned char>(j[p])) || j[p] == ',')) p++;
        if (p >= j.size()) return "malformed bundle";
        if (j[p] == '}') break;
        if (j[p] != '"') return "malformed bundle";
        std::string name, content;
        p = json_read_string(j, p, name);
        while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) p++;
        if (p >= j.size() || j[p] != ':') return "malformed bundle";
        p++;
        while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) p++;
        if (p >= j.size() || j[p] != '"') return "malformed bundle";
        p = json_read_string(j, p, content);
        if (!bundle_name_ok(name)) return "file name not allowed in a plugin: " + name;
        if (content.size() > BUNDLE_FILE_MAX) return "file too large: " + name;
        total += content.size();
        if (total > BUNDLE_TOTAL_MAX) return "bundle too large";
        files.emplace_back(name, content);
    }
    bool has_main = false;
    for (auto& f : files) has_main |= f.first == "decoder.cpp";
    if (!has_main) return "the bundle has no decoder.cpp";
    std::string base = dig::PluginRegistry::dir();
    std::string pdir = base + "/" + id;
    struct stat st{};
    if (stat(pdir.c_str(), &st) == 0 && !replace) return "exists";   // the page asks before replacing
    mkdir(base.c_str(), 0755);
    mkdir(pdir.c_str(), 0755);
    for (auto& f : files) {
        std::string tmp = pdir + "/." + f.first + ".tmp";
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) return "cannot write " + pdir + "/" + f.first;
            o << f.second;
        }
        if (rename(tmp.c_str(), (pdir + "/" + f.first).c_str()) != 0) return "cannot write " + f.first;
    }
    std::cout << "AI Signal Lab: imported plugin " << id << " (" << files.size() << " files)" << std::endl;
    return start_job("import", {"make", "-C", base, "PLUGIN=" + id}, "building imported plugin " + id);
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
    std::string cli, version;
    json_find(probe, "cli", cli);
    json_find(probe, "version", version);
    std::ostringstream o;
    std::lock_guard<std::mutex> lk(mu_);
    o << "{\"ai_state\":{\"enabled\":" << (enabled() ? "true" : "false") << ",\"backend\":\"" << json_escape(backend)
      << "\",\"model\":\"" << json_escape(model) << "\",\"cli\":\"" << json_escape(cli) << "\",\"version\":\""
      << json_escape(version) << "\",\"token\":" << (WebSocketServer::auth_enabled() ? "true" : "false")
      << ",\"busy\":" << (busy_.load() ? "true" : "false") << ",\"job\":{\"kind\":\"" << json_escape(job_kind_)
      << "\",\"label\":\"" << json_escape(job_label_) << "\",\"started\":" << job_started_ms_ << "}"
      << ",\"session\":{\"id\":\"" << json_escape(session_) << "\",\"freq_hz\":" << static_cast<long long>(session_freq_)
      << ",\"vfo\":" << session_vfo_ << ",\"state\":\"" << json_escape(session_state_) << "\",\"signal_name\":\""
      << json_escape(signal_name_) << "\",\"suggested_plugin_id\":\"" << json_escape(suggested_id_)
      << "\",\"plugin_id\":\"" << json_escape(plugin_id_) << "\",\"analysis\":\"" << json_escape(analysis_) << "\"}"
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
