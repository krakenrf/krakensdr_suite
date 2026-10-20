// See platform.hpp. Identical in heimdall_v2/src/core/ and
// kraken_doa_v2/src/utils/ except for the #include line below.
#include "platform.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cwchar>
#include <string>
#else
#include <climits>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace platform {

#ifdef _WIN32

namespace {

void (*g_on_stop)(int) = nullptr;
HANDLE g_stop_requested = nullptr;   // set on every stop request (wait_stop_request)
std::atomic<bool> g_urgent{false};
UINT g_saved_output_cp = 0, g_saved_input_cp = 0;
DWORD g_saved_in_mode = 0, g_saved_out_mode = 0;
bool g_in_mode_saved = false, g_out_mode_saved = false;

std::string narrow(const wchar_t* w, int len = -1) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    if (len < 0 && !s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

void fire(int sig) {
    if (g_on_stop) g_on_stop(sig);
    if (g_stop_requested) SetEvent(g_stop_requested);
}

BOOL WINAPI ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
            fire(SIGINT);
            return TRUE;
        case CTRL_BREAK_EVENT:
            fire(SIGBREAK);
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:    // not sent once user32 is loaded (both
        case CTRL_SHUTDOWN_EVENT:  // apps): endsession_proc gets those
            // returning ends the process at once: hold this (system) thread
            // while main runs its shutdown and ends the process itself
            // (fast_exit); Windows kills it after ~5 s regardless
            g_urgent = true;
            fire(SIGTERM);
            Sleep(INFINITE);
            return TRUE;
        default:
            return FALSE;
    }
}

// Logoff / shutdown / restart (a Windows Update restart at night too). A
// console process with user32 loaded - both apps import it (libuv, shell32)
// - counts as a GUI process: CTRL_LOGOFF_EVENT / CTRL_SHUTDOWN_EVENT never
// come, the session end goes to its top-level windows instead. Without one
// the process was just terminated (heimdall: dongles not closed, a noise
// source left on).
LRESULT CALLBACK endsession_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYENDSESSION:
            // not a stop yet: another program can still cancel the session
            // end after this, and the receiver would have stopped for nothing
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                // it ends, and the process with it once this returns: hold it
                // like CTRL_CLOSE_EVENT while main shuts down and ends the
                // process itself. The reason is what the shutdown screen
                // shows if that takes long (Windows asks after ~5 s).
                ShutdownBlockReasonCreate(hwnd, L"Stopping the receiver");
                g_urgent = true;
                fire(SIGTERM);
                Sleep(INFINITE);
            }
            return 0;
        case WM_CLOSE:
            // taskkill without /F: a stop like SIGTERM (the default would
            // destroy the window, and the session end with it)
            fire(SIGTERM);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void start_endsession_window() {
    std::thread([] {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSW wc{};
        wc.lpfnWndProc = endsession_proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"KrakenSDR_EndSession";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        // hidden (never shown) and top-level: a message-only window
        // (HWND_MESSAGE) gets no session-end messages
        if (!CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr))
            return;
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }).detach();
}

uint64_t ft64(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

}  // namespace

void install_stop_handler(void (*on_stop)(int sig)) {
    g_on_stop = on_stop;
    if (!g_stop_requested) g_stop_requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // a parent (MSYS bash, a CREATE_NEW_PROCESS_GROUP launcher) can hand Ctrl+C
    // down as IGNORED: turn its processing back on, then add the handler
    SetConsoleCtrlHandler(nullptr, FALSE);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    start_endsession_window();
    // the launcher's / `krakensdr stop`'s request: the same as SIGTERM. The
    // urgent one (the launcher's logoff / shutdown / closed window - it is
    // ended a few seconds later, and its job object kills us with it) is a
    // stop like CTRL_CLOSE_EVENT.
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\KrakenSDR_Stop_" + pid).c_str());
    HANDLE urgent = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\KrakenSDR_StopUrgent_" + pid).c_str());
    if (ev || urgent) {
        std::thread([ev, urgent] {
            HANDLE w[2];
            DWORD n = 0;
            if (urgent) w[n++] = urgent;   // first: both set = urgent
            if (ev) w[n++] = ev;
            while (n) {
                const DWORD r = WaitForMultipleObjects(n, w, FALSE, INFINITE);
                if (r >= WAIT_OBJECT_0 + n) return;
                const bool is_urgent = w[r - WAIT_OBJECT_0] == urgent;
                if (is_urgent) g_urgent = true;
                fire(SIGTERM);
                if (is_urgent) return;
                n = urgent ? 1 : 0;   // a logoff can still follow a normal stop
            }
        }).detach();
    }
}

bool stop_is_urgent() { return g_urgent.load(); }

void wait_stop_request(int max_ms) {
    if (g_stop_requested) WaitForSingleObject(g_stop_requested, static_cast<DWORD>(max_ms < 0 ? 0 : max_ms));
    else std::this_thread::sleep_for(std::chrono::milliseconds(max_ms));
}

bool stdout_is_terminal() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
}

StdoutKind stdout_kind() {
    static const StdoutKind kind = [] {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return StdoutKind::Redirected;
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) return StdoutKind::Console;
        if (GetFileType(h) == FILE_TYPE_PIPE) {
            // mintty (MSYS2, Git Bash, Cygwin) talks to programs through a
            // named pipe "\msys-<id>-ptyN-to-master" / "\cygwin-..."
            alignas(FILE_NAME_INFO) char buf[sizeof(FILE_NAME_INFO) + MAX_PATH * sizeof(wchar_t)];
            auto* info = reinterpret_cast<FILE_NAME_INFO*>(buf);
            if (GetFileInformationByHandleEx(h, FileNameInfo, info, sizeof buf)) {
                std::wstring n(info->FileName, info->FileNameLength / sizeof(wchar_t));
                if ((n.find(L"msys-") != std::wstring::npos || n.find(L"cygwin-") != std::wstring::npos) &&
                    n.find(L"-pty") != std::wstring::npos)
                    return StdoutKind::Pty;
            }
        }
        return StdoutKind::Redirected;
    }();
    return kind;
}

void console_init() {
    const bool out_console = stdout_is_terminal();
    DWORD m = 0;
    const bool err_console = GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &m) != 0;
    if (out_console || err_console) {
        if (!g_saved_output_cp) g_saved_output_cp = GetConsoleOutputCP();
        if (!g_saved_input_cp) g_saved_input_cp = GetConsoleCP();
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (GetConsoleMode(in, &in_mode)) {
        if (!g_in_mode_saved) {
            g_saved_in_mode = in_mode;
            g_in_mode_saved = true;
        }
        // ENABLE_PROCESSED_INPUT stays on: it is what turns Ctrl+C into CTRL_C_EVENT
        SetConsoleMode(in, (in_mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT);
    }
}

void fast_exit(int code) {
    std::fflush(nullptr);
    console_restore();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    _Exit(code);   // not reached
}

std::string exe_dir() {
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n);
    const size_t k = p.find_last_of(L"\\/");
    const std::wstring d = k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
    return narrow(d.c_str(), static_cast<int>(d.size()));
}

void chdir_to_exe_dir_if_missing(const char* marker) {
    if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) return;
    const std::string dir = exe_dir();
    if (GetFileAttributesA((dir + "\\" + marker).c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (SetCurrentDirectoryA(dir.c_str()))
        std::fprintf(stderr, "Working directory: %s (where %s is)\n", dir.c_str(), marker);
}

bool console_enable_vt() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode)) return false;
    if (!g_out_mode_saved) {
        g_saved_out_mode = mode;
        g_out_mode_saved = true;
    }
    if (!g_saved_output_cp) g_saved_output_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    // no DISABLE_NEWLINE_AUTO_RETURN: "\n" keeps meaning CR+LF, as a Linux tty's onlcr
    if (!SetConsoleMode(out, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                                 ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return false;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (GetConsoleMode(err, &mode))
        SetConsoleMode(err, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    return true;
}

void console_restore() {
    if (g_out_mode_saved) SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_saved_out_mode);
    if (g_in_mode_saved) SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_saved_in_mode | ENABLE_EXTENDED_FLAGS);
    if (g_saved_output_cp) SetConsoleOutputCP(g_saved_output_cp);
    if (g_saved_input_cp) SetConsoleCP(g_saved_input_cp);
}

bool console_size(int* cols, int* rows) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) return false;
    *cols = info.srWindow.Right - info.srWindow.Left + 1;
    *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    return *cols > 0 && *rows > 0;
}

bool cpu_times(uint64_t* idle, uint64_t* total) {
    FILETIME i, k, u;
    if (!GetSystemTimes(&i, &k, &u)) return false;
    *idle = ft64(i);
    *total = ft64(k) + ft64(u);   // kernel time includes the idle time
    return true;
}

bool mem_info(uint64_t* total_bytes, uint64_t* available_bytes) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms)) return false;
    *total_bytes = ms.ullTotalPhys;
    *available_bytes = ms.ullAvailPhys;
    return true;
}

float cpu_temperature_c() { return -1.0f; }

std::string host_name() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return {};
    return narrow(buf, static_cast<int>(n));
}

std::string host_fqdn() {
    wchar_t buf[512];
    DWORD n = 512;
    if (!GetComputerNameExW(ComputerNameDnsFullyQualified, buf, &n)) return {};
    std::string f = narrow(buf, static_cast<int>(n));
    return f == host_name() ? std::string() : f;
}

void keep_system_awake() { SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED); }

void raise_process_priority() { SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS); }

const char* os_name() { return "windows"; }

#else  // Linux

void install_stop_handler(void (*on_stop)(int sig)) {
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    // SIGHUP too: tmux kill-session (run.sh stop, the systemd unit's stop)
    // and a closed terminal deliver it - without a handler it killed the
    // process outright, skipping its orderly shutdown
    signal(SIGHUP, on_stop);
}

bool stop_is_urgent() { return false; }

void wait_stop_request(int max_ms) { std::this_thread::sleep_for(std::chrono::milliseconds(max_ms)); }

bool stdout_is_terminal() { return isatty(STDOUT_FILENO); }

StdoutKind stdout_kind() { return isatty(STDOUT_FILENO) ? StdoutKind::Console : StdoutKind::Redirected; }

void console_init() {}

void fast_exit(int code) { _Exit(code); }

std::string exe_dir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    std::string p(buf, static_cast<size_t>(n));
    const size_t k = p.find_last_of('/');
    return k == std::string::npos ? std::string(".") : (k == 0 ? std::string("/") : p.substr(0, k));
}

void chdir_to_exe_dir_if_missing(const char*) {}

bool console_enable_vt() { return isatty(STDOUT_FILENO); }

void console_restore() {}

bool console_size(int* cols, int* rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) return false;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return true;
}

bool cpu_times(uint64_t* idle, uint64_t* total) {
    std::ifstream f("/proc/stat");
    std::string cpu;
    uint64_t v[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // user nice system idle iowait irq softirq steal
    if (!(f >> cpu) || cpu != "cpu") return false;
    for (auto& x : v) f >> x;
    if (!f) return false;
    *idle = v[3] + v[4];
    *total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    return true;
}

bool mem_info(uint64_t* total_bytes, uint64_t* available_bytes) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    uint64_t total_kb = 0, avail_kb = 0;
    bool have_t = false, have_a = false;
    while (std::getline(f, line) && !(have_t && have_a)) {
        std::istringstream ss(line);
        std::string key;
        uint64_t kb = 0;
        ss >> key >> kb;
        if (key == "MemTotal:") { total_kb = kb; have_t = true; }
        else if (key == "MemAvailable:") { avail_kb = kb; have_a = true; }
    }
    if (!have_t) return false;
    *total_bytes = total_kb * 1024;
    *available_bytes = avail_kb * 1024;
    return true;
}

float cpu_temperature_c() {
    std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
    long long milli = 0;
    if (!(f >> milli)) return -1.0f;
    return static_cast<float>(milli) / 1000.0f;
}

std::string host_name() {
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return {};
    buf[sizeof buf - 1] = '\0';
    return buf;
}

std::string host_fqdn() { return {}; }

void keep_system_awake() {}

void raise_process_priority() {}

const char* os_name() { return "linux"; }

#endif

}  // namespace platform
