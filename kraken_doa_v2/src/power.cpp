// ⏻ Shut down / ↻ Restart from the web page - see power.hpp and
// docs/dev/power.md (design, platforms, privileges, tests).
#include "power.hpp"

#include "config.hpp"
#include "control_handler.hpp"
#include "decoder_log.hpp"
#include "doa_logger.hpp"
#include "globals.hpp"
#include "incidents.hpp"
#include "networking/tcp_client.hpp"
#include "networking/websocket_server.hpp"
#include "recorder.hpp"
#include "settings_store.hpp"
#include "signal_processing/noise_floor.hpp"
#include "utils/json_escape.hpp"
#include "utils/process.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace power {
namespace {

enum Action { A_RESTART = 0, A_REBOOT = 1, A_SHUTDOWN = 2, A_COUNT = 3 };
constexpr const char* NAMES[A_COUNT] = {"restart", "reboot", "shutdown"};

struct State {
    std::mutex mu;
    bool disabled = false;                 // KRAKEN_NO_POWER
    bool dry = false;                      // KRAKEN_POWER_DRY_RUN
    bool allowed[A_COUNT] = {true, true, true};   // KRAKEN_POWER
    int restart_code = 0;                  // KRAKEN_RESTART_EXIT; 0 = nothing restarts us
    bool probed = false;                   // reboot / shutdown found out
    std::vector<std::string> cmd[A_COUNT]; // reboot / shutdown: the command; empty = not possible
    std::string how[A_COUNT];              // how it is done / why not (the startup log line)
    int busy = -1;                         // the action in progress
    std::string phase;                     // "preparing" (recordings, files) | "running" (the command ran)
    long long last_ms = -1000000;          // when the last attempt ended (repeats)
    uint64_t gen = 0;                      // the attempt (its timeout)
};
State& S() {
    static State s;
    return s;
}
std::atomic<int> g_exit{0};

constexpr long long REPEAT_MS = 3000;           // a new attempt this soon after a failed one is refused
constexpr int RECORDING_WAIT_MS = 20000;        // a recording finishing its queue + metadata
constexpr int STILL_UP_S = 120;                 // the OS took the command but this process still runs
constexpr int DRY_RUN_S = 5;                    // a dry run "shuts down" for this long

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

std::string join(const std::vector<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) s += (s.empty() ? "" : " ") + a;
    return s;
}

// offered on this installation (KRAKEN_POWER, the supervisor, the probe)
bool available_locked(const State& s, int a) {
    if (s.disabled || !s.allowed[a]) return false;
    if (a == A_RESTART) return s.dry || s.restart_code > 0;
    return s.dry || (s.probed && !s.cmd[a].empty());
}

// {"power":{"actions":[...],"busy":"","phase":"","dry_run":false[,"error":..,"for":<action>][,"note":..]}}
// "for" = the action the error is about (a page shows an error only for its own request)
std::string message_locked(const State& s, const std::string& error = "", const std::string& note = "",
                           const std::string& for_action = "") {
    std::string m = "{\"power\":{\"actions\":[";
    bool first = true;
    for (int a = 0; a < A_COUNT; a++) {
        if (!available_locked(s, a)) continue;
        m += std::string(first ? "" : ",") + "\"" + NAMES[a] + "\"";
        first = false;
    }
    m += "],\"busy\":\"" + std::string(s.busy >= 0 ? NAMES[s.busy] : "") + "\",\"phase\":\"" +
         (s.busy >= 0 ? s.phase : "") + "\",\"dry_run\":" + (s.dry ? "true" : "false");
    if (!error.empty()) m += ",\"error\":\"" + json_escape(error) + "\",\"for\":\"" + json_escape(for_action) + "\"";
    if (!note.empty()) m += ",\"note\":\"" + json_escape(note) + "\"";
    return m + "}}";
}

void broadcast_locked(const State& s, const std::string& error = "", const std::string& note = "",
                      const std::string& for_action = "") {
    WebSocketServer::broadcast_json_message(message_locked(s, error, note, for_action));
}

void log_summary_locked(const State& s) {
    std::string on, off;
    for (int a = 0; a < A_COUNT; a++) {
        std::string why;
        if (!s.allowed[a]) why = "KRAKEN_POWER";
        else if (a == A_RESTART) why = s.restart_code > 0 ? "exit status " + std::to_string(s.restart_code)
                                                           : "not started by run.sh, Docker or krakensdr.exe";
        else why = s.how[a];
        if (s.dry && s.allowed[a]) why = "DRY RUN, " + why;
        std::string& to = available_locked(s, a) ? on : off;
        to += (to.empty() ? "" : ", ") + std::string(NAMES[a]) + " (" + why + ")";
    }
    std::cout << "Power buttons: " << (on.empty() ? "none" : on) << (off.empty() ? "" : " - not offered: " + off)
              << std::endl;
}

// what a dry run logs when this user may not run the real command
std::vector<std::string> default_cmd(int a) {
#if defined(_WIN32)
    return {"shutdown.exe", a == A_REBOOT ? "/r" : "/s", "/t", "0"};
#elif defined(__APPLE__)
    return {"sudo", "-n", "/sbin/shutdown", a == A_REBOOT ? "-r" : "-h", "now"};
#else
    return {"systemctl", a == A_REBOOT ? "reboot" : "poweroff"};
#endif
}

// ---- finding out what this user may do --------------------------------------------------
#ifndef _WIN32
bool exists(const char* p) { return access(p, F_OK) == 0; }
std::string first_existing(std::initializer_list<const char*> paths) {
    for (const char* p : paths)
        if (exists(p)) return p;
    return "";
}
// sudo -n -l <command>: exit 0 = this user may run exactly that without a
// password (a rule from install-pi-service.sh, or the Pi's NOPASSWD ALL)
std::string sudo_path() { return first_existing({"/usr/bin/sudo", "/bin/sudo"}); }
bool sudo_allows(const std::vector<std::string>& argv) {
    const std::string sudo = sudo_path();
    if (sudo.empty()) return false;
    std::vector<std::string> a{sudo, "-n", "-l"};
    a.insert(a.end(), argv.begin(), argv.end());
    std::string out;
    return kproc::run_capture(a, 5000, &out);
}
#endif

#if defined(__linux__)
bool in_container() {
    const char* c = std::getenv("container");   // podman / systemd-nspawn
    return exists("/.dockerenv") || exists("/run/.containerenv") || (c && *c);
}
// logind's own answer for this process (polkit: an active local session may,
// a service without a session usually may not): "yes" | "no" | "challenge" | "na"
bool logind_allows(const std::string& busctl, const char* method) {
    if (busctl.empty()) return false;
    std::string out;
    if (!kproc::run_capture({busctl, "call", "--timeout=3", "org.freedesktop.login1", "/org/freedesktop/login1",
                             "org.freedesktop.login1.Manager", method},
                            5000, &out))
        return false;
    return out.find("\"yes\"") != std::string::npos;
}
#endif

#ifdef _WIN32
// SeShutdownPrivilege in this process's token (standard users have it on a
// desktop Windows; shutdown.exe enables it itself)
bool win_can_shut_down() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    LUID luid{};
    bool found = false;
    if (LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &luid)) {
        DWORD n = 0;
        GetTokenInformation(tok, TokenPrivileges, nullptr, 0, &n);
        std::vector<unsigned char> buf(n ? n : 1);
        if (n && GetTokenInformation(tok, TokenPrivileges, buf.data(), n, &n)) {
            const auto* tp = reinterpret_cast<const TOKEN_PRIVILEGES*>(buf.data());
            for (DWORD i = 0; i < tp->PrivilegeCount; i++)
                if (tp->Privileges[i].Luid.LowPart == luid.LowPart && tp->Privileges[i].Luid.HighPart == luid.HighPart)
                    found = true;
        }
    }
    CloseHandle(tok);
    return found;
}
std::string win_shutdown_exe() {
    wchar_t w[MAX_PATH];
    const UINT n = GetSystemDirectoryW(w, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), s.data(), len, nullptr, nullptr);
    return s + "\\shutdown.exe";
}
#endif

// reboot / shutdown: the command each would run, or why not (worker thread)
void probe() {
    std::vector<std::string> cmd[A_COUNT];
    std::string how[A_COUNT];
    bool want[A_COUNT];
    {
        std::lock_guard<std::mutex> lk(S().mu);
        for (int a = 0; a < A_COUNT; a++) want[a] = S().allowed[a];
    }
#if defined(_WIN32)
    const std::string exe = win_shutdown_exe();
    const bool can = !exe.empty() && win_can_shut_down();
    for (int a = A_REBOOT; a < A_COUNT; a++) {
        if (!want[a]) continue;
        if (can) {
            // /t 0 without /f: an application with unsaved work may still hold the shutdown up
            cmd[a] = {exe, a == A_REBOOT ? "/r" : "/s", "/t", "0"};
            how[a] = "shutdown.exe";
        } else {
            how[a] = "this user has no shutdown privilege";
        }
    }
#elif defined(__APPLE__)
    // shutdown(8) needs root; without a password only through a sudo rule
    for (int a = A_REBOOT; a < A_COUNT; a++) {
        if (!want[a]) continue;
        const std::vector<std::string> c{"/sbin/shutdown", a == A_REBOOT ? "-r" : "-h", "now"};
        if (sudo_allows(c)) {
            cmd[a] = {sudo_path(), "-n"};
            cmd[a].insert(cmd[a].end(), c.begin(), c.end());
            how[a] = "sudo";
        } else {
            how[a] = "needs a sudo rule for /sbin/shutdown";
        }
    }
#elif defined(__linux__)
    const std::string systemctl = first_existing({"/usr/bin/systemctl", "/bin/systemctl"});
    const std::string busctl = first_existing({"/usr/bin/busctl", "/bin/busctl"});
    for (int a = A_REBOOT; a < A_COUNT; a++) {
        if (!want[a]) continue;
        const char* verb = a == A_REBOOT ? "reboot" : "poweroff";
        if (in_container()) {
            how[a] = "in a container";
        } else if (systemctl.empty() || !exists("/run/systemd/system")) {
            how[a] = "not a systemd system";
        } else if (logind_allows(busctl, a == A_REBOOT ? "CanReboot" : "CanPowerOff")) {
            cmd[a] = {systemctl, verb};
            how[a] = "systemctl";
        } else if (sudo_allows({systemctl, verb})) {
            cmd[a] = {sudo_path(), "-n", systemctl, verb};
            how[a] = "sudo";
        } else {
            how[a] = "this user may not (install-pi-service.sh adds a sudo rule)";
        }
    }
#else
    for (int a = A_REBOOT; a < A_COUNT; a++) how[a] = "not on this system";
#endif
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    for (int a = A_REBOOT; a < A_COUNT; a++) {
        s.cmd[a] = cmd[a];
        s.how[a] = how[a];
    }
    s.probed = true;
    log_summary_locked(s);
    broadcast_locked(s);   // pages open since the start get the buttons now
}

// ---- doing it --------------------------------------------------------------------------
// THE one place a power command (reboot / shutdown) is run. A dry run only
// logs it. "" = the command took it, else the error.
std::string run_power_command(const std::vector<std::string>& argv, bool dry) {
    const std::string line = join(argv);
    if (dry) {
        std::cout << "Power: dry run - would run: " << line << std::endl;
        return "";
    }
    std::cout << "Power: running " << line << std::endl;
    kproc::SpawnOptions o;
    o.argv = argv;
    o.in = kproc::Stdio::Null;
    o.out = kproc::Stdio::Pipe;
    o.err = kproc::Stdio::SameAsStdout;
    o.console = kproc::Console::None;
    kproc::Process p;
    std::string err;
    if (!p.spawn(o, &err)) return "cannot run " + argv[0] + ": " + err;
    std::string out;
    char buf[1024];
    const long long t0 = now_ms();
    for (;;) {
        const ptrdiff_t n = p.read(kproc::Stream::Out, buf, sizeof buf, 500);
        if (n > 0) {
            if (out.size() < 2000) out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 || now_ms() - t0 > 20000) break;   // EOF / error, or it hangs
    }
    kproc::ExitStatus st;
    if (!p.wait_for(5000, &st)) {
        p.kill();
        return line + ": no answer within 25 s";
    }
    if (st.ok()) return "";
    for (char& c : out)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return line + " " + st.describe() + (out.empty() ? "" : ": " + out.substr(0, 300));
}

// heimdall restarts itself ({"command":"restart"} on its control port: it
// shuts down and exits with the status its supervisor restarts on). Its own
// connection: the shared control socket's replies are discarded.
std::string heimdall_restart() {
    TCPClient c;
    if (!c.connect_to_port(TCP_CONTROL_PORT))
        return "heimdall is not running (no answer on its control port " + std::to_string(TCP_CONTROL_PORT) + ")";
    const std::string cmd = "{\"command\":\"restart\"}\n";
    if (!c.send_data(cmd.data(), cmd.size())) return "heimdall's control port closed";
    std::string rx;
    char buf[4096];
    const long long t0 = now_ms();
    while (now_ms() - t0 < 4000) {
        const ssize_t n = c.receive_data(buf, sizeof buf);   // 500 ms timeout
        if (n == 0) break;
        if (n < 0) {
            // the 500 ms timeout (EAGAIN); anything else: try again until the deadline
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        rx.append(buf, static_cast<size_t>(n));
        size_t nl;
        while ((nl = rx.find('\n')) != std::string::npos) {
            const std::string line = rx.substr(0, nl);
            rx.erase(0, nl + 1);
            std::string st, msg;
            if (!json_find(line, "status", st)) continue;   // its 2 Hz status broadcast
            if (st == "success") return "";
            json_find(line, "message", msg);
            if (msg == "Unknown command") msg = "this heimdall is too old for it";
            return "heimdall refused the restart: " + (msg.empty() ? st : msg);
        }
        if (rx.size() > 65536) rx.clear();
    }
    return "heimdall did not answer the restart request";
}

void fail(int a, uint64_t gen, const std::string& err) {
    std::cerr << "Power: " << NAMES[a] << " failed: " << err << std::endl;
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.gen != gen) return;
    s.busy = -1;
    s.last_ms = now_ms();
    broadcast_locked(s, err, "", NAMES[a]);
}

// the rest of an accepted command (a worker thread: waits, runs programs)
void sequence(int a, uint64_t gen) {
    State& s = S();
    bool dry;
    int code;
    std::vector<std::string> cmd;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        dry = s.dry;
        code = s.restart_code > 0 ? s.restart_code : 75;
        cmd = s.cmd[a];
    }
    // 1. the recordings (asked to stop on the loop thread) write the rest of
    //    their queue and their metadata
    const long long t0 = now_ms();
    while (rec::writing() && now_ms() - t0 < RECORDING_WAIT_MS)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (rec::writing()) std::cerr << "Power: the recording is still being finished - going on" << std::endl;
    // 2. what the shutdown writes anyway, now (the OS may give the apps
    //    only a few seconds; SIGTERM still runs the full shutdown)
    SettingsStore::flush();
    declog::flush();
    incidents::save_now();
    nfloor::save_now();
    std::cout << "Power: recordings finished, settings and logs written" << std::endl;

    // 3. the action
    std::string err;
    if (a == A_RESTART) {
        if (dry)
            std::cout << "Power: dry run - would send {\"command\":\"restart\"} to heimdall (127.0.0.1:" << TCP_CONTROL_PORT
                      << "), then exit with status " << code << std::endl;
        else
            err = heimdall_restart();
    } else if (!cmd.empty()) {
        err = run_power_command(cmd, dry);
    } else if (dry) {
        // the probe found no way for this user: what the command would be
        std::cout << "Power: dry run - this user may not run it, the command would be: " << join(default_cmd(a))
                  << std::endl;
    } else {
        err = "not possible on this installation";
    }
    if (!err.empty()) return fail(a, gen, err);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.gen != gen) return;
        s.phase = "running";
        broadcast_locked(s);
    }
    if (a == A_RESTART && !dry) {
        // main's shutdown (as on SIGTERM), then exit(code): the supervisor
        // starts kraken_doa again; heimdall is restarting already
        std::cout << "Power: restarting - heimdall accepted, kraken_doa exits with status " << code << std::endl;
        g_exit.store(code);
        running.store(false);
        return;
    }
    // the OS stops this process now (SIGTERM / the session end). If it
    // doesn't (an inhibitor, a dry run), the pages get their buttons back.
    const int wait_s = dry ? DRY_RUN_S : STILL_UP_S;
    for (int i = 0; i < wait_s * 10 && running.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!running.load()) return;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.gen != gen || s.busy != a) return;
    s.busy = -1;
    s.last_ms = now_ms();
    if (dry) {
        broadcast_locked(s, "", "Dry run - nothing was done");
    } else {
        std::cerr << "Power: still running " << STILL_UP_S << " s after " << join(cmd) << std::endl;
        broadcast_locked(s, "Still running " + std::to_string(STILL_UP_S / 60) + " minutes after the " + NAMES[a] +
                                " command", "", NAMES[a]);
    }
}

}  // namespace

void init() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.disabled = env_on("KRAKEN_NO_POWER");
    s.dry = env_on("KRAKEN_POWER_DRY_RUN");
    if (const char* p = std::getenv("KRAKEN_POWER"); p && *p) {
        for (bool& b : s.allowed) b = false;
        std::string list = p;
        size_t i = 0;
        while (i <= list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            std::string w = list.substr(i, j - i);
            while (!w.empty() && w.front() == ' ') w.erase(0, 1);
            while (!w.empty() && w.back() == ' ') w.pop_back();
            bool known = w.empty();
            for (int a = 0; a < A_COUNT; a++)
                if (w == NAMES[a]) s.allowed[a] = known = true;
            if (!known) std::cerr << "KRAKEN_POWER: unknown action '" << w << "' (restart, reboot, shutdown)" << std::endl;
            i = j + 1;
        }
    }
    if (const char* r = std::getenv("KRAKEN_RESTART_EXIT"); r && *r) {
        char* end = nullptr;
        const long v = std::strtol(r, &end, 10);
        // 1 is a startup failure, 126+ the shell's (not found, killed by a signal)
        if (end && *end == '\0' && v >= 2 && v <= 125) s.restart_code = static_cast<int>(v);
        else std::cerr << "KRAKEN_RESTART_EXIT must be 2-125 (got '" << r << "') - no restart button" << std::endl;
    }
    if (s.disabled) {
        s.probed = true;
        std::cout << "Power buttons: off (KRAKEN_NO_POWER)" << std::endl;
        return;
    }
    if (s.dry) std::cout << "Power: DRY RUN (KRAKEN_POWER_DRY_RUN) - the commands are only logged" << std::endl;
    if (!s.allowed[A_REBOOT] && !s.allowed[A_SHUTDOWN]) {
        s.probed = true;   // nothing to find out
        log_summary_locked(s);
        return;
    }
    // also in a dry run: it logs the command a real one would run
    std::thread(probe).detach();
}

std::string state_message() {
    std::lock_guard<std::mutex> lk(S().mu);
    return message_locked(S());
}

bool is_command(std::string_view m) { return m.starts_with("POWER:"); }

std::string handle_command(std::string_view m, bool from_page) {
    const std::string_view v = m.substr(6);
    int a = -1;
    for (int i = 0; i < A_COUNT; i++)
        if (v == NAMES[i]) a = i;
    // a page's refused request: every page gets the state again (the one
    // that asked shows why; the buttons follow what is possible)
    auto refuse = [&](const std::string& why) {
        std::cout << "Power: " << std::string(m.substr(0, 40)) << " refused (" << why << ")" << std::endl;
        if (from_page) {
            std::lock_guard<std::mutex> lk(S().mu);
            broadcast_locked(S(), "Refused: " + why, "", std::string(v.substr(0, 16)));
        }
        return why;
    };
    // the AI assistant (ai_mcp.cpp refuses it as well), the settings replay,
    // the web mapper, the scanners: never
    if (!from_page) return refuse("only the page's own buttons may do this");
    if (a < 0) return refuse("unknown action - restart, reboot or shutdown");
    State& s = S();
    uint64_t gen;
    bool dry;
    {
        std::unique_lock<std::mutex> lk(s.mu);
        std::string why;
        if (s.busy >= 0) why = std::string(NAMES[s.busy]) + " already in progress";
        else if (!available_locked(s, a)) why = "not available on this installation";
        else if (now_ms() - s.last_ms < REPEAT_MS) why = "too soon after the last attempt";
        if (!why.empty()) {
            lk.unlock();
            return refuse(why);
        }
        s.busy = a;
        s.phase = "preparing";
        gen = ++s.gen;
        dry = s.dry;
        broadcast_locked(s);
    }
    std::cout << "Power: " << NAMES[a] << " requested from a web page" << (dry ? " (dry run)" : "") << std::endl;
    // the recordings stop through their own commands, so every page sees it
    if (rec::writing()) ControlHandler::handle_websocket_message("RECORDER_STOP");
    if (doa_logger.isRunning()) ControlHandler::handle_websocket_message("LOG_STOP");
    std::thread(sequence, a, gen).detach();
    return "";
}

int exit_code() { return g_exit.load(); }

}  // namespace power
