// krakensdr.exe - start the KrakenSDR v2 stack on Windows: the run.sh of the
// Linux build, with the same behaviour where Windows allows it.
//
//   krakensdr.exe                 Heimdall + KrakenSDR DoA client, each with its
//                                 live dashboard: one Windows Terminal window
//                                 split top/bottom (the tmux layout of run.sh),
//                                 or two console windows without Windows Terminal
//   krakensdr.exe stop            stop everything (both apps, their supervisors,
//                                 a headless run) cleanly, force after 10 s
//   krakensdr.exe --headless      no dashboards: both apps log to logs\*.log and
//                                 this console follows the logs; Ctrl+C stops
//   flags (combinable, as run.sh): --wideband / -w, --kerberos, --kerberos_sw,
//                                 --ext_noise, plus --no-browser, --no-wt,
//                                 --wait-timeout <s>
//   env (as run.sh): WAIT_TIMEOUT (300, 0 = forever), RESTART_DELAY (5),
//                    NO_TMUX=1 (= --headless), WIDEBAND / KERBEROS /
//                    KERBEROS_SW / EXT_NOISE = 1, NO_BROWSER=1, NO_WT=1
//
// Pieces (each mirrors a run.sh function):
//  - supervisor (`__supervise <name> <exe> args...`): runs the app in its pane
//    and restarts it after a crash; a clean exit (0) or a requested stop
//    (Ctrl+C, closing the window, `krakensdr stop`) ends it. The app sits in
//    a job object, so killing the supervisor can't orphan an app that holds
//    the dongles.
//  - client pane (`__client_pane`): waits for heimdall's phase convergence -
//    read from the 8091 data port, as run.sh's python probe did - then
//    supervises the client and opens the browser once its web UI is up.
//  - stop / the stale sweep: every heimdall.exe / kraken_doa.exe / supervisor
//    / headless launcher of this user is asked to stop through its named stop
//    event "Local\KrakenSDR_Stop_<pid>" (the apps create theirs at startup and
//    shut down exactly as on SIGTERM), and terminated after 10 s. A launcher
//    about to be ended itself (logoff, its window closed) sets
//    "Local\KrakenSDR_StopUrgent_<pid>" instead: the apps' short shutdown.
// Windows has no signals to forward: a pane's Ctrl+C reaches the app and the
// supervisor alike (one console), closing the window sends both
// CTRL_CLOSE_EVENT (about 5 s to finish), and the stop event replaces SIGTERM.
// Logoff / shutdown arrive as WM_ENDSESSION at a hidden window (user32 is
// loaded, so the console handler never gets them).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int DATA_PORT = 8091;          // heimdall TCP data port (carries phase_state)
constexpr int CLIENT_WEB_PORT = 8080;    // kraken_doa web UI (https)
constexpr uint32_t TCP_MAGIC = 0x4D434851;   // 'MCHQ'
constexpr uint32_t CONVERGED = 4;
const wchar_t* const HEIMDALL_SUPERVISOR_MUTEX = L"Local\\KrakenSDR_heimdall_supervisor";
const wchar_t* const WT_WINDOW = L"krakensdr";

std::atomic<bool> g_stopping{false};
HANDLE g_stop_event = nullptr;           // our own "Local\KrakenSDR_Stop_<pid>"
// the apps this process started (supervisor: [0]; headless: heimdall +
// kraken_doa), for a stop that ends this process: closing the window, logoff
std::atomic<DWORD> g_child_pids[2] = {};
bool g_color = false;

// ---------------------------------------------------------------------------
// strings, output
// ---------------------------------------------------------------------------
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const char* c_bold() { return g_color ? "\033[1m" : ""; }
const char* c_grn() { return g_color ? "\033[32m" : ""; }
const char* c_yel() { return g_color ? "\033[33m" : ""; }
const char* c_red() { return g_color ? "\033[31m" : ""; }
const char* c_cyn() { return g_color ? "\033[36m" : ""; }
const char* c_rst() { return g_color ? "\033[0m" : ""; }

void say(const std::string& m) { std::printf("%s%s==>%s %s%s%s\n", c_cyn(), c_bold(), c_rst(), c_bold(), m.c_str(), c_rst()); std::fflush(stdout); }
void ok(const std::string& m) { std::printf("%s   \xE2\x9C\x93 %s%s\n", c_grn(), m.c_str(), c_rst()); std::fflush(stdout); }
void warn(const std::string& m) { std::printf("%s   ! %s%s\n", c_yel(), m.c_str(), c_rst()); std::fflush(stdout); }
void err(const std::string& m) { std::fprintf(stderr, "%s%sError:%s %s\n", c_red(), c_bold(), c_rst(), m.c_str()); }

void setup_console() {
    SetConsoleOutputCP(CP_UTF8);
    for (DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        HANDLE h = GetStdHandle(which);
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) g_color = true;
        }
    }
}

std::string last_error_text(DWORD e = GetLastError()) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string s = buf ? narrow(buf) : "error " + std::to_string(e);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

// Exits a restart can't fix: the exe can't run on this PC at all.
const char* fatal_exit_reason(DWORD rc) {
    switch (rc) {
        case 0xC000001Du:
            // a clean rebuild: objects built for the other CPU would be linked in again
            return "illegal instruction: the exe was built for a newer CPU than this one (the default build\n"
                   "     needs AVX2) - rebuild clean for this CPU: in a Command Prompt run\n"
                   "     set WIN_MARCH=-march=x86-64-v2   then   build_windows.bat clean   (see WINDOWS.md)";
        case 0xC0000135u:
            return "a DLL it needs is missing (the exes are static - was it built outside build_windows.bat?)";
        case 0xC0000139u:
            return "a DLL entry point is missing (a wrong DLL on PATH - the exes are meant to be static)";
        case 0xC0000142u:
            return "DLL initialization failed";
        default:
            return nullptr;
    }
}

// exit codes: a crash is an NTSTATUS (0xC0000005 access violation, ...)
std::string status_text(DWORD rc) {
    char b[64];
    if (rc >= 0xC0000000u) std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(rc));
    else std::snprintf(b, sizeof b, "%lu", static_cast<unsigned long>(rc));
    return b;
}

// ---------------------------------------------------------------------------
// paths
// ---------------------------------------------------------------------------
std::wstring self_path() {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    return p;
}
std::wstring dir_of(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}
std::wstring root_dir() { return dir_of(self_path()); }
std::wstring heimdall_dir() { return root_dir() + L"\\heimdall_v2"; }
std::wstring kraken_dir() { return root_dir() + L"\\kraken_doa_v2"; }
std::wstring heimdall_exe() { return heimdall_dir() + L"\\heimdall.exe"; }
std::wstring kraken_exe() { return kraken_dir() + L"\\kraken_doa.exe"; }
std::wstring log_dir() { return root_dir() + L"\\logs"; }
bool file_exists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// "X:\..." or "\\server\..." - not relative to the current directory / drive
bool is_absolute_dir(const std::wstring& d) {
    auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    return (d.size() >= 3 && iswalpha(d[0]) && d[1] == L':' && sep(d[2])) || (d.size() >= 2 && sep(d[0]) && sep(d[1]));
}

// Never searches the current directory: SearchPathW(nullptr, ...) looks there
// before PATH, so a wt.exe in the folder krakensdr.exe was started from (e.g.
// Downloads) would run. Windows Terminal's App Execution Alias first, then
// the absolute PATH entries.
std::wstring find_on_path(const wchar_t* exe) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH && is_absolute_dir(std::wstring(buf, n))) {
        const std::wstring alias = std::wstring(buf, n) + L"\\Microsoft\\WindowsApps\\" + exe;
        if (file_exists(alias)) return alias;
    }
    std::wstring path(32767, L'\0');
    n = GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size()));
    path.resize(n < path.size() ? n : 0);
    std::wstring dirs;
    for (size_t b = 0; b < path.size();) {
        size_t e = path.find(L';', b);
        if (e == std::wstring::npos) e = path.size();
        std::wstring d = path.substr(b, e - b);
        if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
        if (is_absolute_dir(d)) dirs += (dirs.empty() ? L"" : L";") + d;   // drops "", ".", relative names
        b = e + 1;
    }
    if (dirs.empty()) return {};
    n = SearchPathW(dirs.c_str(), exe, nullptr, MAX_PATH, buf, nullptr);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

// ---------------------------------------------------------------------------
// processes
// ---------------------------------------------------------------------------
// CommandLineToArgvW quoting rules
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q.push_back(L'"'); }
        else { q.append(bs, L'\\'); q.push_back(a[i]); }
    }
    q.push_back(L'"');
    return q;
}

std::wstring join_cmdline(const std::vector<std::wstring>& argv) {
    std::wstring c;
    for (const auto& a : argv) c += (c.empty() ? L"" : L" ") + quote_arg(a);
    return c;
}

std::wstring stop_event_name(DWORD pid) { return L"Local\\KrakenSDR_Stop_" + std::to_wstring(pid); }
std::wstring urgent_stop_event_name(DWORD pid) { return L"Local\\KrakenSDR_StopUrgent_" + std::to_wstring(pid); }

void create_own_stop_event() {
    if (!g_stop_event) g_stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_event_name(GetCurrentProcessId()).c_str());
}

// urgent: we are ended in a few seconds (logoff, our window closed) - the app
// saves first and waits less, as when its own console closes. An app without
// that event gets the normal one.
bool request_stop(DWORD pid, bool urgent = false) {
    HANDLE ev = urgent ? OpenEventW(EVENT_MODIFY_STATE, FALSE, urgent_stop_event_name(pid).c_str()) : nullptr;
    if (!ev) ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_event_name(pid).c_str());
    if (!ev) return false;
    SetEvent(ev);
    CloseHandle(ev);
    return true;
}

HANDLE make_kill_on_close_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    return job;
}

struct Child {
    HANDLE proc = nullptr;
    DWORD pid = 0;
};

// Starts argv[0] with the given working directory. out = a handle for stdout
// + stderr (stdin = NUL; only those handles are inherited), or nullptr: the
// child shares this console. extra_env: NAME=value pairs added for the child.
bool spawn(const std::vector<std::wstring>& argv, const std::wstring& cwd, HANDLE out, DWORD flags, HANDLE job,
           const std::vector<std::pair<std::wstring, std::wstring>>& extra_env, Child* c, std::string* why) {
    std::wstring cmd = join_cmdline(argv);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    BOOL inherit = FALSE;
    HANDLE nul_in = nullptr;
    std::vector<char> attr_buf;
    HANDLE inh[2];
    if (out) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul_in;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = out;
        inh[0] = nul_in;
        inh[1] = out;
        SIZE_T sz = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
        attr_buf.resize(sz);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz);
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh, sizeof inh, nullptr,
                                  nullptr);
        inherit = TRUE;
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    // environment: ours plus extra_env (set around CreateProcess; single-threaded here)
    std::vector<std::pair<std::wstring, std::wstring>> saved;
    for (const auto& kv : extra_env) {
        wchar_t old[32767];
        DWORD n = GetEnvironmentVariableW(kv.first.c_str(), old, 32767);
        saved.push_back({kv.first, n ? std::wstring(old, n) : std::wstring(1, L'\x01')});
        SetEnvironmentVariableW(kv.first.c_str(), kv.second.c_str());
    }
    BOOL okp = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, inherit, flags | CREATE_SUSPENDED, nullptr,
                              cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DWORD e = GetLastError();
    for (const auto& kv : saved)
        SetEnvironmentVariableW(kv.first.c_str(), kv.second == std::wstring(1, L'\x01') ? nullptr : kv.second.c_str());
    if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (nul_in) CloseHandle(nul_in);
    if (!okp) {
        if (why) *why = last_error_text(e);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   // best effort
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess;
    c->pid = pi.dwProcessId;
    return true;
}

DWORD exit_code_of(HANDLE p) {
    DWORD rc = 1;
    GetExitCodeProcess(p, &rc);
    return rc;
}

bool still_running(HANDLE p) { return p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT; }

// sleep that a stop (Ctrl+C, console close, stop event) cuts short
void stoppable_sleep(int ms) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (!g_stopping) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) break;
        DWORD left = static_cast<DWORD>(std::min<ULONGLONG>(end - now, 100));
        if (g_stop_event && WaitForSingleObject(g_stop_event, left) == WAIT_OBJECT_0) g_stopping = true;
        else if (!g_stop_event) Sleep(left);
    }
}

// ---------------------------------------------------------------------------
// stop / stale sweep
// ---------------------------------------------------------------------------
struct ProcEntry {
    DWORD pid;
    std::wstring exe;
};

std::vector<ProcEntry> stack_processes() {
    std::vector<ProcEntry> v;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return v;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;
        const wchar_t* n = pe.szExeFile;
        if (!_wcsicmp(n, L"heimdall.exe") || !_wcsicmp(n, L"kraken_doa.exe") || !_wcsicmp(n, L"krakensdr.exe"))
            v.push_back({pe.th32ProcessID, n});
    }
    CloseHandle(snap);
    return v;
}

// Asks every heimdall / kraken_doa / supervisor / headless launcher of this
// user to stop, waits up to 10 s for the clean shutdown (heimdall releases
// the dongles), then terminates what is left. Returns how many were stopped.
int stop_stack(bool announce) {
    struct Target {
        HANDLE h;
        DWORD pid;
        std::wstring exe;
        bool asked;
    };
    std::vector<Target> targets;
    std::vector<std::string> foreign;
    for (const auto& p : stack_processes()) {
        HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        if (!h) {
            if (GetLastError() == ERROR_ACCESS_DENIED)
                foreign.push_back("     " + std::to_string(p.pid) + "  " + narrow(p.exe));
            continue;
        }
        const bool asked = request_stop(p.pid);
        // a krakensdr.exe without a stop event is not part of a running stack
        // (e.g. another `krakensdr stop`, or a launcher that is just starting panes)
        if (!asked && !_wcsicmp(p.exe.c_str(), L"krakensdr.exe")) {
            CloseHandle(h);
            continue;
        }
        targets.push_back({h, p.pid, p.exe, asked});
    }
    if (!foreign.empty()) {
        warn("Running as another user (stop them from an elevated prompt or Task Manager):");
        for (const auto& f : foreign) std::printf("%s\n", f.c_str());
    }
    if (targets.empty()) return 0;
    if (announce) warn("Stopping running KrakenSDR processes:");
    for (const auto& t : targets)
        std::printf("     %6lu  %s%s\n", static_cast<unsigned long>(t.pid), narrow(t.exe).c_str(),
                    t.asked ? "" : "  (no stop event - will be terminated)");
    std::fflush(stdout);
    const ULONGLONG end = GetTickCount64() + 10000;
    bool any_alive = true;
    while (GetTickCount64() < end) {
        any_alive = false;
        for (const auto& t : targets)
            if (t.asked && still_running(t.h)) any_alive = true;
        if (!any_alive) break;
        Sleep(200);
    }
    bool forced = false;
    for (const auto& t : targets) {
        if (still_running(t.h)) {
            TerminateProcess(t.h, 1);
            forced = true;
        }
    }
    if (forced) {
        warn("Still running after 10s - force-killed.");
        Sleep(500);
    }
    for (const auto& t : targets) CloseHandle(t.h);
    ok("Stopped.");
    return static_cast<int>(targets.size());
}

// ---------------------------------------------------------------------------
// console control: Ctrl+C / Ctrl+Break / closing the window; logoff /
// shutdown through a hidden window
// ---------------------------------------------------------------------------
// the main thread acts on it: supervise() passes it on, the headless run
// cleans up, the convergence wait and the sleeps end
void request_own_stop() {
    g_stopping = true;
    if (g_stop_event) SetEvent(g_stop_event);
}

// The process ends as soon as the handler returns, and the job object then
// kills the apps: pass an urgent stop on and give their clean shutdown
// (heimdall releases the dongles) time first. The apps are asked before our
// own stop, whose normal request (the main thread's) must not reach them
// first. A handle of our own - the main thread closes its handle once the app
// has ended.
void stop_all_and_wait(DWORD ms) {
    HANDLE w[2];
    DWORD n = 0;
    for (auto& slot : g_child_pids) {
        const DWORD pid = slot.load();
        if (!pid) continue;
        request_stop(pid, true);
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) w[n++] = h;
    }
    request_own_stop();
    if (n) WaitForMultipleObjects(n, w, TRUE, ms);
    for (DWORD i = 0; i < n; i++) CloseHandle(w[i]);
}

BOOL WINAPI console_handler(DWORD type) {
    // the apps in this console got the same event; Windows allows about 5 s
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        stop_all_and_wait(4500);
    else
        request_own_stop();
    return TRUE;   // handled: Ctrl+C reaches the app itself (same console)
}

// Logoff / shutdown / restart (a Windows Update restart at night too): user32
// is loaded (SHELL32 imports it), so Windows treats the launcher as a GUI
// process - CTRL_LOGOFF_EVENT / CTRL_SHUTDOWN_EVENT never reach
// console_handler; the session end comes to its top-level windows instead.
LRESULT CALLBACK endsession_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYENDSESSION:
            return TRUE;   // not a stop yet: the session end can still be cancelled
        case WM_ENDSESSION:
            // the process ends as soon as this returns
            if (wp) stop_all_and_wait(10000);
            return 0;
        case WM_CLOSE:
            // taskkill without /F; the default would destroy the window
            request_own_stop();
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void start_endsession_window() {
    // asked before the apps (default level 0x280): a supervisor knows the stop
    // is wanted before its app ends, so it doesn't restart it
    SetProcessShutdownParameters(0x281, 0);
    std::thread([] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = endsession_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"KrakenSDR_Launcher_EndSession";
        RegisterClassW(&wc);
        // top-level, never shown: a message-only window (HWND_MESSAGE) gets no session-end messages
        if (!CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance,
                             nullptr))
            return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }).detach();
}

// watches our stop event (krakensdr stop) -> the same as Ctrl+C
void start_stop_watcher() {
    create_own_stop_event();
    std::thread([] {
        WaitForSingleObject(g_stop_event, INFINITE);
        g_stopping = true;
    }).detach();
}

// ---------------------------------------------------------------------------
// convergence probe (run.sh's python): connect to 8091, read the 16-byte
// big-endian header magic 'MCHQ' | num_channels | num_samples | phase_state.
// Each fresh connection starts on a packet boundary.
// ---------------------------------------------------------------------------
uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// true + the header fields when a header was read
bool probe_header(uint32_t* magic, uint32_t* phase) {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(DATA_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool connected = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv{5, 0};
        connected = select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w);
    }
    unsigned char buf[16];
    int got = 0;
    if (connected) {
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD to = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        while (got < 16) {
            int n = recv(s, reinterpret_cast<char*>(buf) + got, 16 - got, 0);
            if (n <= 0) break;
            got += n;
        }
    }
    closesocket(s);
    if (got < 16) return false;
    *magic = be32(buf);
    *phase = be32(buf + 12);
    return true;
}

bool port_open(int port) {
    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool okc = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    closesocket(s);
    return okc;
}

// 0 = converged, 1 = timed out, 2 = heimdall is gone, 3 = heimdall runs in
// wideband scan / independent mode (no calibration there), 4 = stop requested,
// 5 = heimdall is in KerberosSDR manual-calibration mode
int wait_for_convergence(int timeout_s, const std::function<bool()>& heimdall_alive) {
    static const char* NAMES[] = {"WAIT-LAG", "MEASURING", "APPLYING", "VERIFYING", "CONVERGED", "COOLDOWN", "PER-BIN"};
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (g_stopping) { std::fprintf(stderr, "\n"); return 4; }
        const double el = (GetTickCount64() - start) / 1000.0;
        if (timeout_s > 0 && el > timeout_s) { std::fprintf(stderr, "\n"); return 1; }
        if (!heimdall_alive()) { std::fprintf(stderr, "\n"); return 2; }
        uint32_t magic = 0, phase = 0;
        if (probe_header(&magic, &phase)) {
            if (magic == TCP_MAGIC && (phase & 0xC00)) {
                std::fprintf(stderr, "\r   Heimdall is in %s mode - no calibration to wait for\n",
                             (phase & 0x800) ? "independent" : "wideband scan");
                return 3;
            }
            // nothing calibrates on its own there; --kerberos_sw ends up in it
            // when its antenna switches are missing (always on Windows: no Pi GPIOs)
            if (magic == TCP_MAGIC && (phase & 0x100)) {
                std::fprintf(stderr,
                             "\r   Heimdall is in KerberosSDR manual-calibration mode - no automatic calibration to "
                             "wait for\n");
                return 5;
            }
            phase &= 0xFF;   // high bits carry mode / KerberosSDR flags; the low byte is the state
            if (magic == TCP_MAGIC) {
                char nb[16];
                std::snprintf(nb, sizeof nb, "%u", phase);
                std::fprintf(stderr, "\r   [%3ds] Heimdall phase: %-10s", static_cast<int>(el), phase < 7 ? NAMES[phase] : nb);
                std::fflush(stderr);
                if (phase == CONVERGED) { std::fprintf(stderr, "\n"); return 0; }
            }
        } else {
            std::fprintf(stderr, "\r   [%3ds] waiting for Heimdall data port %d...", static_cast<int>(el), DATA_PORT);
            std::fflush(stderr);
        }
        stoppable_sleep(1000);
    }
}

// KerberosSDR manual calibration: the client starts without a calibration
void warn_uncalibrated() {
    warn("DoA output is INVALID until you calibrate: disconnect all antennas,");
    warn("then press 'Full Recalibration' in the heimdall web UI (:8070).");
}

// ---------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------
struct Options {
    bool wideband = false, kerberos = false, kerberos_sw = false, ext_noise = false;
    bool headless = false, no_browser = false, no_wt = false, pause_on_error = false;
    int wait_timeout = 300;
    int restart_delay = 5;
    bool kb_manual() const { return kerberos && !kerberos_sw; }
    std::vector<std::wstring> heimdall_flags() const {
        std::vector<std::wstring> f;
        if (wideband) f.push_back(L"--wideband");
        if (kerberos_sw) f.push_back(L"--kerberos_sw");
        else if (kerberos) f.push_back(L"--kerberos");
        if (ext_noise) f.push_back(L"--ext_noise");
        return f;
    }
    std::vector<std::wstring> client_flags() const {
        std::vector<std::wstring> f;
        if (wideband) f.push_back(L"--wideband");
        return f;
    }
};

bool env_on(const char* n) {
    const char* v = std::getenv(n);
    return v && std::strcmp(v, "1") == 0;
}

void usage(FILE* f) {
    std::fprintf(f,
        "Usage:\n"
        "  krakensdr.exe                  start Heimdall + the KrakenSDR DoA client, each with its\n"
        "                                 dashboard (Windows Terminal split, or two console windows);\n"
        "                                 the client waits for Heimdall's phase convergence\n"
        "  krakensdr.exe stop             stop everything (cleanly; force after 10 s)\n"
        "  krakensdr.exe --wideband       KrakenSDR Wideband variant (both apps; WIDEBAND=1 too)\n"
        "  krakensdr.exe --kerberos       KerberosSDR, manual calibration only: the client starts\n"
        "                                 at once - calibrate from the heimdall web UI (:8070)\n"
        "  krakensdr.exe --kerberos_sw    KerberosSDR with CKOVAL antenna switches (automatic\n"
        "                                 calibration; needs a Raspberry Pi - falls back to --kerberos)\n"
        "  krakensdr.exe --ext_noise      add-on array with its own noise source on the CH0 bias tee\n"
        "  krakensdr.exe --headless       no dashboards; logs in logs\\ (NO_TMUX=1 too)\n"
        "  --wait-timeout <s>             convergence wait (default 300, 0 = forever; WAIT_TIMEOUT)\n"
        "  --no-browser                   don't open the client web UI (NO_BROWSER=1)\n"
        "  --no-wt                        separate console windows even with Windows Terminal (NO_WT=1)\n"
        "\n"
        "  Heimdall http://localhost:8070    Client https://localhost:8080   (DoA page :8081)\n");
}

// parses leading flags; returns false (after printing why) on a bad one
bool parse_flags(int argc, wchar_t** argv, int first, Options* o, std::vector<std::wstring>* rest) {
    if (const char* t = std::getenv("WAIT_TIMEOUT")) o->wait_timeout = std::atoi(t);
    if (const char* r = std::getenv("RESTART_DELAY")) o->restart_delay = std::max(0, std::atoi(r));
    o->wideband = env_on("WIDEBAND");
    o->kerberos = env_on("KERBEROS");
    o->kerberos_sw = env_on("KERBEROS_SW");
    o->ext_noise = env_on("EXT_NOISE");
    o->headless = env_on("NO_TMUX");
    o->no_browser = env_on("NO_BROWSER");
    o->no_wt = env_on("NO_WT");
    for (int i = first; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--wideband" || a == L"-w") o->wideband = true;
        else if (a == L"--kerberos") o->kerberos = true;
        else if (a == L"--kerberos_sw" || a == L"--kerberos-sw") o->kerberos_sw = true;
        else if (a == L"--ext_noise" || a == L"--ext-noise") o->ext_noise = true;
        else if (a == L"--headless") o->headless = true;
        else if (a == L"--no-browser") o->no_browser = true;
        else if (a == L"--no-wt") o->no_wt = true;
        else if (a == L"--pause-on-error") o->pause_on_error = true;
        else if (a == L"--wait-timeout" && i + 1 < argc) o->wait_timeout = _wtoi(argv[++i]);
        else if (rest) rest->push_back(a);
        else {
            err("unknown option '" + narrow(a) + "'");
            usage(stderr);
            return false;
        }
    }
    if (o->kerberos_sw) o->kerberos = true;
    if (o->ext_noise && (o->wideband || o->kerberos)) {
        err("--ext_noise cannot be combined with --wideband or --kerberos(_sw)");
        return false;
    }
    return true;
}

void pause_if(bool cond) {
    if (!cond) return;
    std::printf("\nPress Enter to close this window...");
    std::fflush(stdout);
    std::getchar();
}

// ---------------------------------------------------------------------------
// supervisor
// ---------------------------------------------------------------------------
// runs cmd in this console and restarts it after a crash:
//   exit 0 (clean shutdown) / a requested stop -> done
//   anything else (crash, init error)          -> restart after restart_delay s
int supervise(const std::wstring& name, const std::vector<std::wstring>& cmd, const Options& o) {
    HANDLE job = make_kill_on_close_job();
    DWORD rc = 0;
    for (;;) {
        Child c;
        std::string why;
        if (!spawn(cmd, L"", nullptr, 0, job, {}, &c, &why)) {
            err("cannot start " + narrow(cmd[0]) + ": " + why);
            rc = 1;
        } else {
            g_child_pids[0] = c.pid;
            HANDLE w[2] = {c.proc, g_stop_event};
            DWORD r = WaitForMultipleObjects(g_stop_event ? 2 : 1, w, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 + 1) {
                // `krakensdr stop` asked us: pass it on, wait for the clean shutdown
                g_stopping = true;
                request_stop(c.pid);
                WaitForSingleObject(c.proc, INFINITE);
            }
            rc = exit_code_of(c.proc);
            g_child_pids[0] = 0;
            CloseHandle(c.proc);
        }
        if (g_stopping || rc == 0) break;
        if (const char* why = fatal_exit_reason(rc)) {
            err(narrow(name) + " cannot run (status " + status_text(rc) + "): " + why);
            break;
        }
        warn(narrow(name) + " exited (status " + status_text(rc) + ") - restarting in " + std::to_string(o.restart_delay) +
             "s (Ctrl+C to stay stopped)");
        stoppable_sleep(o.restart_delay * 1000);
        if (g_stopping) break;
    }
    if (job) CloseHandle(job);
    return static_cast<int>(rc);
}

int supervise_entry(int argc, wchar_t** argv) {
    // __supervise <name> <exe> [app args...] (launcher options first: --pause-on-error)
    Options o;
    int i = 2;
    while (i < argc && std::wstring(argv[i]) == L"--pause-on-error") { o.pause_on_error = true; i++; }
    if (argc - i < 2) return 2;
    std::wstring name = argv[i++];
    std::vector<std::wstring> cmd(argv + i, argv + argc);
    if (const char* r = std::getenv("RESTART_DELAY")) o.restart_delay = std::max(0, std::atoi(r));
    SetConsoleTitleW(name.c_str());
    HANDLE role = nullptr;
    if (_wcsicmp(name.c_str(), L"Heimdall") == 0) role = CreateMutexW(nullptr, FALSE, HEIMDALL_SUPERVISOR_MUTEX);
    start_stop_watcher();
    SetConsoleCtrlHandler(console_handler, TRUE);
    start_endsession_window();
    say(narrow(name) + " (restarted after a crash; Ctrl+C or `krakensdr stop` stops it)");
    int rc = supervise(name, cmd, o);
    if (role) CloseHandle(role);
    pause_if(o.pause_on_error && rc != 0 && !g_stopping);
    return rc;
}

// ---------------------------------------------------------------------------
// client pane: convergence wait, then the supervised client
// ---------------------------------------------------------------------------
bool heimdall_supervisor_alive(ULONGLONG started, bool* seen) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, HEIMDALL_SUPERVISOR_MUTEX);
    if (m) {
        CloseHandle(m);
        *seen = true;
        return true;
    }
    // panes start together: give heimdall's supervisor time to come up
    return !*seen && GetTickCount64() - started < 20000;
}

void open_browser_when_up(bool enabled) {
    if (!enabled) return;
    std::thread([] {
        for (int i = 0; i < 120 && !g_stopping; i++) {
            if (port_open(CLIENT_WEB_PORT)) {
                ShellExecuteW(nullptr, L"open", L"https://localhost:8080", nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            Sleep(1000);
        }
    }).detach();
}

int client_pane_entry(int argc, wchar_t** argv) {
    Options o;
    if (!parse_flags(argc, argv, 2, &o, nullptr)) return 2;
    SetConsoleTitleW(L"KrakenSDR DoA");
    start_stop_watcher();
    SetConsoleCtrlHandler(console_handler, TRUE);
    start_endsession_window();
    if (o.kb_manual()) {
        warn("KerberosSDR manual-calibration mode: starting the client immediately.");
        warn_uncalibrated();
        stoppable_sleep(2000);
    } else {
        std::printf("%sWaiting for Heimdall phase convergence%s...%s\n", c_bold(),
                    o.wait_timeout == 0 ? " (no timeout)" : (" (timeout " + std::to_string(o.wait_timeout) + "s)").c_str(),
                    c_rst());
        std::fflush(stdout);
        const ULONGLONG started = GetTickCount64();
        bool seen = false;
        int r = wait_for_convergence(o.wait_timeout, [&] { return heimdall_supervisor_alive(started, &seen); });
        switch (r) {
            case 0: ok("Phase converged - DF output is valid. Starting client..."); stoppable_sleep(1000); break;
            case 3: ok("Starting client..."); break;
            case 5: warn_uncalibrated(); stoppable_sleep(2000); break;
            case 2:
                warn("Heimdall has stopped (see its pane) - not starting the client.");
                warn("Start the stack again with: krakensdr.exe");
                pause_if(o.pause_on_error);
                return 1;
            case 4: return 0;
            default:
                warn("No convergence within " + std::to_string(o.wait_timeout) + "s. Starting client anyway");
                warn("(DoA output stays invalid until Heimdall converges - watch the Heimdall pane).");
                stoppable_sleep(3000);
                break;
        }
    }
    if (g_stopping) return 0;
    SetCurrentDirectoryW(kraken_dir().c_str());
    std::vector<std::wstring> cmd{kraken_exe()};
    for (const auto& f : o.client_flags()) cmd.push_back(f);
    open_browser_when_up(!o.no_browser);
    say("KrakenSDR DoA client (restarted after a crash; Ctrl+C or `krakensdr stop` stops it)");
    int rc = supervise(L"KrakenSDR DoA client", cmd, o);
    pause_if(o.pause_on_error && rc != 0 && !g_stopping);
    return rc;
}

// ---------------------------------------------------------------------------
// windowed launch (default)
// ---------------------------------------------------------------------------
int launch_windowed(const Options& o) {
    const std::wstring self = self_path();
    std::vector<std::wstring> sup{self, L"__supervise"};
    std::vector<std::wstring> hcmd{L"Heimdall", heimdall_exe()};
    for (const auto& f : o.heimdall_flags()) hcmd.push_back(f);
    std::vector<std::wstring> pane{self, L"__client_pane", L"--wait-timeout", std::to_wstring(o.wait_timeout)};
    if (o.wideband) pane.push_back(L"--wideband");
    if (o.kerberos_sw) pane.push_back(L"--kerberos_sw");
    else if (o.kerberos) pane.push_back(L"--kerberos");
    if (o.no_browser) pane.push_back(L"--no-browser");

    const std::wstring wt = o.no_wt ? std::wstring() : find_on_path(L"wt.exe");
    if (!wt.empty()) {
        say("Launching the stack in Windows Terminal (window '" + narrow(WT_WINDOW) + "')");
        // wt [-w name] new-tab <opts> <cmdline> ; split-pane -H <opts> <cmdline>
        std::vector<std::wstring> a{wt, L"-w", WT_WINDOW, L"new-tab", L"--title", L"Heimdall", L"--suppressApplicationTitle",
                                    L"-d", heimdall_dir()};
        a.insert(a.end(), sup.begin(), sup.end());
        a.insert(a.end(), hcmd.begin(), hcmd.end());
        a.push_back(L";");
        for (const wchar_t* s : {L"split-pane", L"-H", L"--title", L"KrakenSDR DoA", L"--suppressApplicationTitle", L"-d"})
            a.push_back(s);
        a.push_back(kraken_dir());
        a.insert(a.end(), pane.begin(), pane.end());
        Child c;
        std::string why;
        if (!spawn(a, root_dir(), nullptr, 0, nullptr, {}, &c, &why)) {
            err("cannot start Windows Terminal: " + why + " (try --no-wt)");
            return 1;
        }
        CloseHandle(c.proc);
    } else {
        say("Launching the stack in two console windows");
        std::vector<std::wstring> h = sup;
        h.push_back(L"--pause-on-error");
        h.insert(h.end(), hcmd.begin(), hcmd.end());
        pane.push_back(L"--pause-on-error");
        Child c1, c2;
        std::string why;
        if (!spawn(h, heimdall_dir(), nullptr, CREATE_NEW_CONSOLE, nullptr, {}, &c1, &why)) {
            err("cannot start heimdall's window: " + why);
            return 1;
        }
        CloseHandle(c1.proc);
        if (!spawn(pane, kraken_dir(), nullptr, CREATE_NEW_CONSOLE, nullptr, {}, &c2, &why)) {
            err("cannot start the client's window: " + why);
            return 1;
        }
        CloseHandle(c2.proc);
    }
    std::printf("\n  %sThe stack is starting.%s\n"
                "    %s : Heimdall server dashboard\n"
                "    %s : convergence wait -> KrakenSDR DoA client dashboard\n\n"
                "  Heimdall  http://localhost:8070      Client  https://localhost:8080  (DoA :8081)\n\n"
                "  Stop everything:  %skrakensdr.exe stop%s   (or Ctrl+C in each pane / close the window)\n\n",
                c_bold(), c_rst(), wt.empty() ? "Window 1" : "Top   ", wt.empty() ? "Window 2" : "Bottom", c_bold(),
                c_rst());
    return 0;
}

// ---------------------------------------------------------------------------
// headless: both apps log to logs\*.log, this console follows the logs
// ---------------------------------------------------------------------------
struct LogTail {
    LogTail(std::wstring p, std::string t) : path(std::move(p)), tag(std::move(t)) {}
    std::wstring path;
    std::string tag;
    LONGLONG pos = 0;
    std::string partial;
    std::deque<std::string> last;   // the last lines, for error reports
    void poll(bool print) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart < pos) pos = 0;
        LARGE_INTEGER p;
        p.QuadPart = pos;
        SetFilePointerEx(h, p, nullptr, FILE_BEGIN);
        char buf[65536];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) {
            pos += n;
            partial.append(buf, n);
            size_t k;
            while ((k = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, k);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                partial.erase(0, k + 1);
                if (print) std::printf("[%s] %s\n", tag.c_str(), line.c_str());
                last.push_back(line);
                if (last.size() > 15) last.pop_front();
            }
        }
        CloseHandle(h);
        std::fflush(stdout);
    }
    void dump() {
        poll(false);
        for (const auto& l : last) std::printf("     %s\n", l.c_str());
    }
};

HANDLE open_log(const std::wstring& path) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void stop_child(Child& c) {
    if (!c.proc) return;
    request_stop(c.pid);
    // heimdall's clean shutdown can take up to ~7 s (settings lock + USB readers)
    if (WaitForSingleObject(c.proc, 10000) != WAIT_OBJECT_0) TerminateProcess(c.proc, 1);
    WaitForSingleObject(c.proc, 2000);
}

int run_headless(const Options& o) {
    start_stop_watcher();
    SetConsoleCtrlHandler(console_handler, TRUE);
    start_endsession_window();
    CreateDirectoryW(log_dir().c_str(), nullptr);
    LogTail ht(log_dir() + L"\\heimdall.log", "heimdall");
    LogTail kt(log_dir() + L"\\kraken_doa.log", "kraken_doa");
    HANDLE job = make_kill_on_close_job();
    Child h, k;
    auto cleanup = [&] {
        std::printf("\n");
        say("Shutting down...");
        stop_child(k);
        stop_child(h);
        ok("Stopped.");
    };
    HANDLE hlog = open_log(ht.path);
    say("Starting Heimdall server (headless; log: logs\\heimdall.log)");
    std::vector<std::wstring> hcmd{heimdall_exe()};
    for (const auto& f : o.heimdall_flags()) hcmd.push_back(f);
    std::string why;
    if (!spawn(hcmd, heimdall_dir(), hlog, 0, job, {{L"HEIMDALL_NO_TUI", L"1"}}, &h, &why)) {
        err("cannot start heimdall: " + why);
        return 1;
    }
    g_child_pids[0] = h.pid;
    CloseHandle(hlog);
    std::printf("     Web UI: http://localhost:8070\n");
    if (o.kb_manual()) {
        warn("KerberosSDR manual-calibration mode: skipping the convergence wait.");
        warn_uncalibrated();
        stoppable_sleep(2000);
    } else {
        say(std::string("Waiting for phase convergence") +
            (o.wait_timeout == 0 ? " (no timeout)" : " (timeout " + std::to_string(o.wait_timeout) + "s)") + "...");
        int r = wait_for_convergence(o.wait_timeout, [&] { return still_running(h.proc); });
        if (g_stopping || r == 4) { cleanup(); return 0; }
        if (r == 0) ok("Phase converged - DF output is valid.");
        else if (r == 3) ok("No calibration in this mode - starting the client.");
        else if (r == 5) warn_uncalibrated();
        else if (r == 2) {
            warn("Heimdall exited during startup. Last log lines:");
            ht.dump();
            err("Heimdall failed to start (see logs\\heimdall.log).");
            cleanup();
            return 1;
        } else {
            warn("No convergence in time. Last Heimdall log lines:");
            ht.dump();
            err("Timed out (check antennas/noise source, or raise WAIT_TIMEOUT).");
            cleanup();
            return 1;
        }
    }
    if (g_stopping) { cleanup(); return 0; }
    HANDLE klog = open_log(kt.path);
    say("Starting KrakenSDR DoA client (headless; log: logs\\kraken_doa.log)");
    std::vector<std::wstring> kcmd{kraken_exe()};
    for (const auto& f : o.client_flags()) kcmd.push_back(f);
    if (!spawn(kcmd, kraken_dir(), klog, 0, job, {{L"KRAKEN_DOA_NO_TUI", L"1"}}, &k, &why)) {
        err("cannot start kraken_doa: " + why);
        CloseHandle(klog);
        cleanup();
        return 1;
    }
    g_child_pids[1] = k.pid;
    CloseHandle(klog);
    std::printf("     Web UI: https://localhost:8080   (DoA page: http://localhost:8081)\n\n");
    say(std::string(c_grn()) + "Stack is up." + c_rst() + " Logs are errors-only (HEIMDALL_VERBOSE_LOG=1 /");
    say(std::string("KRAKEN_DOA_VERBOSE_LOG=1 for full output). Press ") + c_bold() + "Ctrl+C" + c_rst() +
        " to stop everything.");
    std::printf("-------------------------------------------------------------------------------\n");
    std::fflush(stdout);
    open_browser_when_up(!o.no_browser);
    ht.poll(false);
    kt.poll(false);
    while (!g_stopping && still_running(h.proc)) {
        ht.poll(true);
        kt.poll(true);
        stoppable_sleep(300);
    }
    ht.poll(true);
    kt.poll(true);
    if (g_stopping) {
        cleanup();
        return 0;
    }
    // heimdall ended on its own: pass its status on (0 = clean shutdown)
    const DWORD hrc = exit_code_of(h.proc);
    stop_child(k);
    if (hrc == 0) {
        warn("Heimdall process ended; shutting down.");
        return 0;
    }
    warn("Heimdall exited with status " + status_text(hrc) + "; shutting down. Last log lines:");
    ht.dump();
    return static_cast<int>(hrc);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setup_console();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"__supervise") return supervise_entry(argc, argv);
    if (cmd == L"__client_pane") return client_pane_entry(argc, argv);
    if (cmd == L"-h" || cmd == L"--help" || cmd == L"/?") {
        usage(stdout);
        return 0;
    }
    if (cmd == L"stop") {
        if (stop_stack(false) == 0) warn("No running KrakenSDR processes found.");
        return 0;
    }
    Options o;
    if (!parse_flags(argc, argv, 1, &o, nullptr)) return 2;
    if (!file_exists(heimdall_exe())) {
        err("heimdall not built (" + narrow(heimdall_exe()) + "). Run build_windows.bat first.");
        return 1;
    }
    if (!file_exists(kraken_exe())) {
        err("kraken_doa not built (" + narrow(kraken_exe()) + "). Run build_windows.bat first.");
        return 1;
    }
    // the default build targets x86-64-v3 (AVX2): warn early on an older CPU
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") || !__builtin_cpu_supports("bmi2"))
        warn("This CPU has no AVX2 / FMA / BMI2: apps built with the default WIN_MARCH (x86-64-v3) won't run;\n"
             "     rebuild them clean for this CPU: in a Command Prompt run\n"
             "     set WIN_MARCH=-march=x86-64-v2   then   build_windows.bat clean   (see WINDOWS.md)");
    // a previous stack holds the dongles and ports: stop it first
    stop_stack(true);
    return o.headless ? run_headless(o) : launch_windowed(o);
}
