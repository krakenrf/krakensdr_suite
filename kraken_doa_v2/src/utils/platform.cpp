// See platform.hpp. Identical in heimdall_v2/src/core/ and
// kraken_doa_v2/src/utils/ except for the #include line below.
#include "utils/platform.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cwchar>
#include <string>
#else
#include <climits>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/sysctl.h>
#else
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#endif
#endif

namespace platform {

namespace {
// One live process below this one. `start` tells a reused pid from the
// process that had it before (any unit, only compared for equality).
struct ProcCpu {
    int64_t pid;
    uint64_t start;
    uint64_t cpu_ns;
};
bool list_descendants(std::vector<ProcCpu>* out);   // per platform, below; without this process
}  // namespace

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

bool process_cpu_ns(uint64_t* ns) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return false;
    *ns = (ft64(k) + ft64(u)) * 100;   // 100 ns units
    return true;
}

int cpu_count() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? static_cast<int>(si.dwNumberOfProcessors) : 1;
}

namespace {
bool list_descendants(std::vector<ProcCpu>* out) {
    struct Row {
        DWORD pid, ppid;
        bool taken;
    };
    std::vector<Row> rows;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        rows.push_back({pe.th32ProcessID, pe.th32ParentProcessID, false});
    CloseHandle(snap);

    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return false;
    struct Node {
        DWORD pid;
        uint64_t created;
    };
    const DWORD self = GetCurrentProcessId();
    std::vector<Node> tree{{self, ft64(c)}};
    for (size_t i = 0; i < tree.size(); i++) {
        const Node parent = tree[i];
        for (Row& r : rows) {
            if (r.taken || r.ppid != parent.pid || r.pid == self || r.pid == 0) continue;
            const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid);
            if (!h) continue;
            const BOOL ok = GetProcessTimes(h, &c, &e, &k, &u);
            CloseHandle(h);
            // Windows keeps a parent pid after that process is gone and gives
            // the number to a new one: a "child" older than its parent
            // belonged to the pid's previous owner
            if (!ok || ft64(c) < parent.created) continue;
            r.taken = true;
            tree.push_back({r.pid, ft64(c)});
            out->push_back({static_cast<int64_t>(r.pid), ft64(c), (ft64(k) + ft64(u)) * 100});
        }
    }
    return true;
}
}  // namespace

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

#else  // Linux (+ macOS where noted)

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
#ifdef __APPLE__
    char raw[PATH_MAX], buf[PATH_MAX];
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, buf)) return ".";
    std::string p(buf);
#else
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    std::string p(buf, static_cast<size_t>(n));
#endif
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

#ifdef __APPLE__
bool cpu_times(uint64_t* idle, uint64_t* total) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return false;
    *idle = info.cpu_ticks[CPU_STATE_IDLE];
    *total = *idle + info.cpu_ticks[CPU_STATE_USER] + info.cpu_ticks[CPU_STATE_SYSTEM] + info.cpu_ticks[CPU_STATE_NICE];
    return true;
}

// available = what macOS can hand out without swapping: free + speculative +
// inactive + purgeable pages (close to MemAvailable on Linux)
bool mem_info(uint64_t* total_bytes, uint64_t* available_bytes) {
    uint64_t total = 0;
    size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) != 0) return false;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
        host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
            KERN_SUCCESS)
        return false;
    const uint64_t pages =
        static_cast<uint64_t>(vm.free_count) + vm.speculative_count + vm.inactive_count + vm.purgeable_count;
    *total_bytes = total;
    *available_bytes = std::min<uint64_t>(pages * page, total);
    return true;
}

float cpu_temperature_c() { return -1.0f; }  // needs the SMC (IOKit) - not read

namespace {
bool list_descendants(std::vector<ProcCpu>* out) {
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
    size_t len = 0;
    if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0) return false;
    std::vector<kinfo_proc> procs((len + len / 8) / sizeof(kinfo_proc) + 16);   // room for new ones
    len = procs.size() * sizeof(kinfo_proc);
    if (sysctl(mib, 3, procs.data(), &len, nullptr, 0) != 0) return false;
    procs.resize(len / sizeof(kinfo_proc));

    // proc_pid_rusage counts in Mach time units (1 ns on Intel, 125/3 ns on
    // Apple silicon)
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{1, 1};
        mach_timebase_info(&t);
        if (t.denom == 0) t.numer = t.denom = 1;
        return t;
    }();
    const pid_t self = getpid();
    std::vector<pid_t> tree{self};
    std::vector<bool> taken(procs.size(), false);
    for (size_t i = 0; i < tree.size(); i++) {
        const pid_t parent = tree[i];
        for (size_t j = 0; j < procs.size(); j++) {
            const kinfo_proc& kp = procs[j];
            const pid_t pid = kp.kp_proc.p_pid;
            if (taken[j] || kp.kp_eproc.e_ppid != parent || pid == self || pid <= 0) continue;
            taken[j] = true;
            tree.push_back(pid);
            rusage_info_v2 ri;
            if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) != 0) continue;
            const timeval& st = kp.kp_proc.p_starttime;
            out->push_back({static_cast<int64_t>(pid),
                            static_cast<uint64_t>(st.tv_sec) * 1000000ull + static_cast<uint64_t>(st.tv_usec),
                            (ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom});
        }
    }
    return true;
}
}  // namespace
#else
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

namespace {
// /proc/<pid>/stat of every process (a few hundred small reads), then the
// ones below this process by their parent pids. /proc lists processes, not
// threads, and a child's parent is the process, whichever thread started it.
bool list_descendants(std::vector<ProcCpu>* out) {
    struct Row {
        long pid, ppid;
        unsigned long long ticks, start;
        bool taken;
    };
    std::vector<Row> rows;
    DIR* dir = opendir("/proc");
    if (!dir) return false;
    char path[64], buf[1024];
    while (const dirent* e = readdir(dir)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::snprintf(path, sizeof path, "/proc/%.20s/stat", e->d_name);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;   // exited meanwhile
        const ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        // "pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt
        // majflt cmajflt utime stime cutime cstime priority nice threads
        // itrealvalue starttime ..."; comm may hold spaces and ')'
        const char* p = std::strrchr(buf, ')');
        Row r{};
        unsigned long long ut = 0, st = 0;
        if (!p || std::sscanf(p + 1,
                              " %*c %ld %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %*d %*d %llu",
                              &r.ppid, &ut, &st, &r.start) != 4)
            continue;
        r.pid = std::atol(e->d_name);
        r.ticks = ut + st;
        rows.push_back(r);
    }
    closedir(dir);

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    const long self = getpid();
    std::vector<long> tree{self};
    for (size_t i = 0; i < tree.size(); i++) {
        const long parent = tree[i];
        for (Row& r : rows) {
            if (r.taken || r.ppid != parent || r.pid == self) continue;
            r.taken = true;
            tree.push_back(r.pid);
            out->push_back({r.pid, r.start, r.ticks * 1000000000ull / static_cast<unsigned long long>(hz)});
        }
    }
    return true;
}
}  // namespace
#endif

bool process_cpu_ns(uint64_t* ns) {
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return false;
    *ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    return true;
}

int cpu_count() {
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? static_cast<int>(n) : 1;
}

std::string host_name() {
    char buf[256];
    if (gethostname(buf, sizeof buf) != 0) return {};
    buf[sizeof buf - 1] = '\0';
    return buf;
}

std::string host_fqdn() { return {}; }

#ifdef __APPLE__
// a Mac idle-sleeps after a few minutes (laptops and desktops alike): the USB
// streams stop, and every wake costs a full recalibration. The assertion
// (what `caffeinate -i` takes) lasts until the process ends; the display may
// still sleep
void keep_system_awake() {
    static IOPMAssertionID id = kIOPMNullAssertionID;
    if (id == kIOPMNullAssertionID)
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
                                    CFSTR("KrakenSDR receiving"), &id);
}
#else
void keep_system_awake() {}
#endif

void raise_process_priority() {}

#ifdef __APPLE__
const char* os_name() { return "macos"; }   // the UIs show the Linux texts for anything but "windows"
#else
const char* os_name() { return "linux"; }
#endif

#endif

bool process_tree_cpu_ns(uint64_t* ns) {
    static std::mutex mtx;
    static std::vector<ProcCpu> prev;    // the descendants at the previous call, sorted by pid
    static uint64_t descendants_ns = 0;  // what they used between the calls so far
    uint64_t self = 0;
    if (!process_cpu_ns(&self)) return false;
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<ProcCpu> cur;
    if (list_descendants(&cur)) {
        std::sort(cur.begin(), cur.end(), [](const ProcCpu& a, const ProcCpu& b) { return a.pid < b.pid; });
        for (const ProcCpu& c : cur) {
            const auto p = std::lower_bound(prev.begin(), prev.end(), c.pid,
                                            [](const ProcCpu& a, int64_t pid) { return a.pid < pid; });
            // a process seen for the first time only sets its baseline: its
            // whole CPU time so far in one step would show as a spike
            if (p != prev.end() && p->pid == c.pid && p->start == c.start && c.cpu_ns > p->cpu_ns)
                descendants_ns += c.cpu_ns - p->cpu_ns;
        }
        prev = std::move(cur);
    }
    *ns = self + descendants_ns;
    return true;
}

}  // namespace platform
