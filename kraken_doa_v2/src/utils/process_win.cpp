// kproc on Windows (include/utils/process.hpp). What Windows does differently
// and how it is mapped onto the Linux semantics:
//  - no fork / posix_spawn: CreateProcessW with ONE UTF-16 command line, the
//    argv quoted with the rules the C runtime (and Python) parse it back by
//  - no closefrom(3): bInheritHandles = TRUE would hand the child EVERY
//    inheritable handle (C runtime files, Winsock sockets - a plugin holding
//    the 8091 connection open is the bug spawn_fds.hpp fixed on Linux), so
//    PROC_THREAD_ATTRIBUTE_HANDLE_LIST names exactly the child's std handles.
//    That also makes spawns from several threads at once safe: another
//    spawn's inheritable pipe ends are not in this child's list
//  - pipes have no O_NONBLOCK and no poll(): the parent's end of each pipe is
//    the server end of a uniquely named OVERLAPPED pipe (the child gets an
//    ordinary synchronous client end, so its read() / write() work as they
//    are). One overlapped read is kept outstanding per output pipe and one
//    overlapped write (a copy of <= 1 MiB) per stdin pipe: a write handed to
//    the system counts as accepted, like bytes sitting in a Linux pipe
//    buffer; "would block" = that write still in flight. wait_io() waits on
//    the OVERLAPPED events (= poll()). Measured ~3 GB/s, no helper threads
//    per child (one for all: StdinCloser, a stdin closed during a write)
//  - no signals / process groups: every child is in a Job object of its own
//    (KILL_ON_JOB_CLOSE), which holds everything it starts. kill() =
//    TerminateProcess, or TerminateJobObject for a kill_tree child (the
//    killpg of Linux); terminate() = CTRL_BREAK to a kill_tree child on our
//    console (Python's SIGBREAK), otherwise the same as kill(). Plugins and
//    helpers die with kraken_doa even when it crashes: its job handles close
//    (on Linux they exit on stdin EOF; a hung one would survive on Windows)
//  - exit codes are 32-bit, crashes are NTSTATUS codes: mapped onto
//    ExitStatus' pseudo-signals (0xC0000005 -> 11 ...)
#ifdef _WIN32

#include "utils/process.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

namespace kproc {

namespace {

constexpr size_t MAX_WRITE_CHUNK = 1u << 20;   // one overlapped write in flight, at most this much
constexpr size_t READ_CHUNK = 64u << 10;       // one overlapped read outstanding per output pipe
constexpr size_t DEFAULT_PIPE_BYTES = 64u << 10;

// ---- text ------------------------------------------------------------------
std::wstring widen(const std::string& s, bool* ok = nullptr) {
    if (ok) *ok = true;
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {   // not UTF-8: U+FFFD for the bad bytes, and tell the caller
        if (ok) *ok = false;
        flags = 0;
        n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return {};
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, flags, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string env_utf8(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got < n ? got : 0);
    return narrow(v);
}

bool ends_ci(const std::string& s, const char* suffix) {
    const size_t l = std::strlen(suffix);
    return s.size() >= l && _strnicmp(s.c_str() + s.size() - l, suffix, l) == 0;
}

// a bare program name ("python3", "curl"): looked up on PATH
bool is_bare(const std::string& s) { return s.find_first_of("/\\:") == std::string::npos; }

bool file_exists(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &a) &&
           !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

// glibc's texts, so "cannot start: ..." reads as on Linux (the UCRT's
// strerror says "Arg list too long", "Unknown error" ...)
const char* errno_text(int e) {
    switch (e) {
        case ENOENT: return "No such file or directory";
        case EACCES: return "Permission denied";
        case ENOEXEC: return "Exec format error";
        case E2BIG: return "Argument list too long";
        case ENOMEM: return "Cannot allocate memory";
        case ENAMETOOLONG: return "File name too long";
        case EILSEQ: return "Invalid or incomplete multibyte or wide character";
        case EINVAL: return "Invalid argument";
        case ETXTBSY: return "Text file busy";
        case EMFILE: return "Too many open files";
        case EBADF: return "Bad file descriptor";
        case EAGAIN: return "Resource temporarily unavailable";
        case EIO: return "Input/output error";
        default: return std::strerror(e);
    }
}

int errno_from_win(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_DIRECTORY:   // the cwd doesn't exist
        case ERROR_MOD_NOT_FOUND:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_ELEVATION_REQUIRED:
        case ERROR_VIRUS_INFECTED:   // Defender blocked it
        case ERROR_VIRUS_DELETED:
            return EACCES;
        case ERROR_BAD_EXE_FORMAT:
        case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        case ERROR_EXE_MARKED_INVALID:
        case ERROR_BAD_FORMAT:
        case ERROR_INVALID_EXE_SIGNATURE:
            return ENOEXEC;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_COMMITMENT_LIMIT:
            return ENOMEM;
        case ERROR_SHARING_VIOLATION:   // the exe is being written (a build)
        case ERROR_LOCK_VIOLATION:
            return ETXTBSY;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_NO_SYSTEM_RESOURCES:
            return EAGAIN;
        default:
            return EINVAL;
    }
}

std::string win_error_text(int en, DWORD we) {
    return std::string(errno_text(en)) + " (Windows error " + std::to_string(we) + ")";
}

void warn_once(std::atomic<bool>& flag, const std::string& msg) {
    if (!flag.exchange(true)) std::cerr << "kproc: " << msg << std::endl;
}

// ---- command line (the C runtime's / CommandLineToArgvW's rules) ------------
// argv[0] is parsed differently (no backslash escapes; up to the next quote),
// so it is only put in quotes - a quote inside it can't be passed at all
bool append_argv0(std::wstring& cmd, const std::wstring& a) {
    if (a.find(L'"') != std::wstring::npos) return false;
    if (a.empty() || a.find_first_of(L" \t") != std::wstring::npos) cmd += L'"' + a + L'"';
    else cmd += a;
    return true;
}

// 2n backslashes + quote -> n backslashes and the quote ends the argument;
// 2n+1 backslashes + quote -> n backslashes and a literal quote; backslashes
// not followed by a quote are literal
void append_quoted(std::wstring& cmd, const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') {
            bs++;
            i++;
        }
        if (i == a.size()) {
            cmd.append(bs * 2, L'\\');   // the closing quote follows
            break;
        }
        if (a[i] == L'"') {
            cmd.append(bs * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(bs, L'\\');
            cmd += a[i];
        }
    }
    cmd += L'"';
}

// ---- environment block -------------------------------------------------------
std::wstring env_name(const std::wstring& v) {
    const size_t eq = v.find(L'=', 1);   // "=C:=C:\x" (per-drive cwd) has a leading '='
    return v.substr(0, eq);
}

bool name_eq(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// our environment with env_unset removed and env_set added / replaced (names
// case-insensitive, as Windows treats them), sorted as CreateProcess wants
bool build_env(const SpawnOptions& o, std::vector<wchar_t>* blk) {
    std::vector<std::wstring> vars;
    if (LPWCH e = GetEnvironmentStringsW()) {
        for (LPWCH p = e; *p; p += std::wcslen(p) + 1) vars.emplace_back(p);
        FreeEnvironmentStringsW(e);
    }
    bool ok = true;
    std::vector<std::wstring> drop;
    for (const auto& n : o.env_unset) drop.push_back(widen(n, &ok));
    std::vector<std::wstring> add;
    for (const auto& kv : o.env_set) {
        bool k1 = true, k2 = true;
        drop.push_back(widen(kv.first, &k1));
        add.push_back(widen(kv.first, &k1) + L"=" + widen(kv.second, &k2));
        ok = ok && k1 && k2;
    }
    if (!ok) return false;
    vars.erase(std::remove_if(vars.begin(), vars.end(),
                              [&](const std::wstring& v) {
                                  const std::wstring n = env_name(v);
                                  for (const auto& d : drop)
                                      if (name_eq(n, d)) return true;
                                  return false;
                              }),
               vars.end());
    vars.insert(vars.end(), add.begin(), add.end());
    std::stable_sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        const std::wstring na = env_name(a), nb = env_name(b);
        return CompareStringOrdinal(na.c_str(), static_cast<int>(na.size()), nb.c_str(), static_cast<int>(nb.size()),
                                    TRUE) == CSTR_LESS_THAN;
    });
    blk->clear();
    for (const auto& v : vars) {
        blk->insert(blk->end(), v.begin(), v.end());
        blk->push_back(L'\0');
    }
    if (blk->empty()) blk->push_back(L'\0');
    blk->push_back(L'\0');
    return true;
}

// ---- jobs ----------------------------------------------------------------------
// One per child. KILL_ON_JOB_CLOSE: its processes die when the last handle to
// the job closes (kraken_doa exiting or crashing, or close() of the Process);
// DIE_ON_UNHANDLED_EXCEPTION: a crashing child ends at once, no WER dialog.
// Not one job shared by all children: a Microsoft Store app's process
// (WindowsApps\python3.exe) starts in a job of its own, and a job that
// already holds processes can't take it (ERROR_ACCESS_DENIED) - worse, an
// empty shared job taking it first is nested under that app's job and then
// refuses every other child (ERROR_INVALID_PARAMETER; measured). A fresh
// job always takes the child, nested under whatever job it started in.
HANDLE make_kill_job() {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (!j) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li)) {
        CloseHandle(j);
        return nullptr;
    }
    return j;
}

std::atomic<bool> g_warned_job{false}, g_warned_python{false};

bool have_console() {
    DWORD l[4];
    return GetConsoleProcessList(l, 4) != 0;
}

// ---- pipes -------------------------------------------------------------------
std::atomic<uint64_t> g_pipe_counter{0};

uint64_t random64() {
    static std::mutex mu;
    static std::mt19937_64 gen = [] {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        uint64_t seed = static_cast<uint64_t>(q.QuadPart) ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
        try {
            std::random_device rd;
            seed ^= (static_cast<uint64_t>(rd()) << 32) ^ rd();
        } catch (...) {
        }
        return std::mt19937_64(seed);
    }();
    std::lock_guard<std::mutex> lk(mu);
    return gen();
}

// A pipe for one std stream: *server = our end (overlapped, not inheritable),
// *client = the child's end (synchronous, inheritable - only ever listed in
// that child's handle list). The name is unique (pid, counter, 64 random
// bits) and the pipe has one instance: our CreateFileW succeeding proves no
// other process connected to it first.
bool make_pipe(bool parent_writes, size_t bytes, HANDLE* server, HANDLE* client, DWORD* werr) {
    const DWORD size = static_cast<DWORD>(std::min<size_t>(bytes ? bytes : DEFAULT_PIPE_BYTES, 64u << 20));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    for (int attempt = 0; attempt < 8; attempt++) {
        wchar_t name[128];
        swprintf(name, 128, L"\\\\.\\pipe\\krakensdr-%lu-%llu-%016llx", static_cast<unsigned long>(GetCurrentProcessId()),
                 static_cast<unsigned long long>(++g_pipe_counter), static_cast<unsigned long long>(random64()));
        HANDLE s = CreateNamedPipeW(
            name, (parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, size, size, 0, nullptr);
        if (s == INVALID_HANDLE_VALUE) {
            *werr = GetLastError();
            if (*werr == ERROR_ACCESS_DENIED || *werr == ERROR_PIPE_BUSY) continue;   // the name is taken
            return false;
        }
        // the child's end may also change its own pipe state (as libuv's)
        HANDLE c = CreateFileW(name, parent_writes ? (GENERIC_READ | FILE_WRITE_ATTRIBUTES) : (GENERIC_WRITE | FILE_READ_ATTRIBUTES),
                               0, &sa, OPEN_EXISTING, 0, nullptr);
        if (c == INVALID_HANDLE_VALUE) {
            *werr = GetLastError();
            CloseHandle(s);
            if (*werr == ERROR_PIPE_BUSY) continue;   // someone else connected first
            return false;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD ce = 0;
        if (!ov.hEvent || !ConnectNamedPipe(s, &ov)) ce = ov.hEvent ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        if (ce == ERROR_IO_PENDING) {   // not expected: the client end is open already
            DWORD x;
            if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(s, &ov, &x, FALSE)) {
                ce = 0;
            } else {
                CancelIoEx(s, &ov);
                GetOverlappedResult(s, &ov, &x, TRUE);
                ce = ERROR_PIPE_NOT_CONNECTED;
            }
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        if (ce != 0 && ce != ERROR_PIPE_CONNECTED) {
            *werr = ce;
            CloseHandle(c);
            CloseHandle(s);
            return false;
        }
        *server = s;
        *client = c;
        return true;
    }
    return false;
}

// our end of the child's stdout or stderr: one overlapped read outstanding
struct ReadPipe {
    HANDLE h = nullptr;
    OVERLAPPED ov{};
    bool pending = false, eof = false, error = false;
    std::vector<char> buf;
    size_t have = 0, off = 0;   // completed, not yet handed out: buf[off, have)

    void open(HANDLE server) {
        h = server;
        ov = OVERLAPPED{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset; ReadFile resets it
        pending = eof = error = false;
        have = off = 0;
        buf.resize(READ_CHUNK);
        start();
    }
    bool has_data() const { return off < have; }
    void start() {
        if (!h || pending || eof || error || has_data()) return;
        have = off = 0;
        if (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov)) {
            pending = true;   // done already; collect() picks it up
            return;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            pending = true;
            return;
        }
        if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_HANDLE_EOF) eof = true;
        else error = true;
        SetEvent(ov.hEvent);   // waiters wake up for it, like POLLHUP
    }
    void collect() {
        if (!pending) return;
        DWORD n = 0;
        if (GetOverlappedResult(h, &ov, &n, FALSE)) {
            pending = false;
            have = n;
            off = 0;
            return;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_IO_INCOMPLETE) return;
        pending = false;
        if (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_HANDLE_EOF) eof = true;
        else if (e != ERROR_OPERATION_ABORTED) error = true;
    }
    // never free a buffer / OVERLAPPED the system may still write to
    void close() {
        if (h) {
            if (pending) {
                DWORD x;
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &x, TRUE);
            }
            CloseHandle(h);
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        h = nullptr;
        ov = OVERLAPPED{};
        pending = false;
        have = off = 0;
        std::vector<char>().swap(buf);
    }
};

// our end of the child's stdin: one overlapped write in flight
struct WritePipe {
    HANDLE h = nullptr;
    OVERLAPPED ov{};
    bool pending = false, broken = false;
    std::vector<char> buf;
    size_t off = 0, len = 0;   // the write in flight: buf[off, len)

    void open(HANDLE server) {
        h = server;
        ov = OVERLAPPED{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        pending = broken = false;
        off = len = 0;
    }
    void issue() {
        if (WriteFile(h, buf.data() + off, static_cast<DWORD>(len - off), nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)
            pending = true;
        else
            broken = true;   // ERROR_NO_DATA / ERROR_BROKEN_PIPE: the child closed its stdin or died
    }
    void collect() {
        if (!pending) return;
        DWORD n = 0;
        if (GetOverlappedResult(h, &ov, &n, FALSE)) {
            pending = false;
            off += n;
            if (off < len) issue();   // a short write: the rest was accepted already, send it too
            return;
        }
        if (GetLastError() == ERROR_IO_INCOMPLETE) return;
        pending = false;
        broken = true;
    }
    // the final close (the Process is closed / its child killed): a write
    // still in flight is cancelled
    void close() {
        if (h) {
            collect();
            if (pending) {   // the child doesn't read: cancel, then wait until the system let go of buf
                DWORD x;
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &x, TRUE);
            }
            CloseHandle(h);
        }
        if (ov.hEvent) CloseHandle(ov.hEvent);
        h = nullptr;
        ov = OVERLAPPED{};
        pending = false;
        off = len = 0;
        std::vector<char>().swap(buf);
    }
};

// close_stdin() with a write still in flight: on Linux the bytes write()
// took sit in the pipe and the child reads them before its EOF, so the pipe
// may only be closed once that write has completed - cancelling it lost up to
// 1 MiB the caller had been told was written (measured). The pipe (with its
// OVERLAPPED and buffer, which the system still uses) goes to this one thread,
// which closes it when the write completes - or fails: the child died / was
// killed. close_stdin() doesn't block, as close() on Linux.
class StdinCloser {
public:
    static StdinCloser& get() {
        static StdinCloser* c = new StdinCloser;   // never destroyed: its thread may run until exit
        return *c;
    }
    void adopt(std::unique_ptr<WritePipe> w) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            pend_.push_back(std::move(w));
        }
        SetEvent(wake_);
    }

private:
    StdinCloser() : wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        std::thread([this] { run(); }).detach();
    }
    void run() {
        for (;;) {
            HANDLE hs[MAXIMUM_WAIT_OBJECTS];
            DWORD n = 0;
            hs[n++] = wake_;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (const auto& w : pend_) {
                    if (n == MAXIMUM_WAIT_OBJECTS) break;   // the rest: checked on every wakeup
                    hs[n++] = w->ov.hEvent;
                }
            }
            WaitForMultipleObjects(n, hs, FALSE, n > 1 ? 1000 : INFINITE);
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = pend_.begin(); it != pend_.end();) {
                WritePipe& w = **it;
                w.collect();   // also sends the rest of a short write
                if (w.pending && !w.broken) {
                    ++it;
                    continue;
                }
                w.close();   // EOF for the child now
                it = pend_.erase(it);
            }
        }
    }
    std::mutex mu_;
    std::vector<std::unique_ptr<WritePipe>> pend_;
    HANDLE wake_;
};

HANDLE dup_inheritable(HANDLE h) {
    HANDLE d = nullptr;
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS)) return nullptr;
    return d;
}

HANDLE open_nul(bool write) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE h = CreateFileW(L"NUL", write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, 0, nullptr);
    return h == INVALID_HANDLE_VALUE ? nullptr : h;
}

// exit code -> ExitStatus (the pseudo-signals of process.hpp)
ExitStatus map_exit(DWORD code, bool kill_sent, bool term_sent, bool break_sent) {
    ExitStatus s;
    s.raw = code;
    auto sig = [&](int n, bool ours) {
        s.kind = ExitStatus::Kind::Signaled;
        s.signal = n;
        s.killed_by_us = ours;
        return s;
    };
    if (kill_sent && code == 137) return sig(9, true);
    if (term_sent && code == 143) return sig(15, true);
    if (break_sent && code == 0xC000013Au) return sig(15, true);   // the default answer to our CTRL_BREAK
    switch (code) {
        case 0xC0000005u:   // access violation
        case 0xC00000FDu:   // stack overflow
            return sig(11, false);
        case 0xC0000006u:   // in-page error (a mapped file went away)
            return sig(7, false);
        case 0xC0000409u:   // fail-fast: abort(), an uncaught C++ exception, a /GS check
        case 0xE06D7363u:   // an unhandled MSVC C++ exception
            return sig(6, false);
        case 0xC000001Du:   // illegal instruction (built with -march=native elsewhere)
        case 0xC0000096u:   // privileged instruction
            return sig(4, false);
        case 0x80000003u:   // breakpoint
            return sig(5, false);
        case 0xC000013Au:   // Ctrl+C / Ctrl+Break
            return sig(2, false);
        default:
            break;
    }
    if (code >= 0xC000008Eu && code <= 0xC0000095u) return sig(8, false);   // float / integer errors
    if (code >= 0xC0000000u && code < 0xD0000000u) return sig(0, false);     // any other NTSTATUS error
    s.kind = ExitStatus::Kind::Exited;
    s.code = static_cast<int>(code);
    return s;
}

}  // namespace

// ---- executables / PATH ----------------------------------------------------
std::string exe_path(const std::string& path_without_suffix) {
    return ends_ci(path_without_suffix, ".exe") ? path_without_suffix : path_without_suffix + EXE_SUFFIX;
}

bool is_executable(const std::string& path) {
    // an App Execution Alias (WindowsApps\python3.exe) is a reparse point
    // GetFileAttributesEx reports as a file - CreateProcess runs it
    return (ends_ci(path, ".exe") || ends_ci(path, ".com")) && file_exists(path);
}

std::string find_in_path(const std::string& name) {
    if (name.empty()) return "";
    if (!is_bare(name)) {
        if (is_executable(name)) return name;
        return is_executable(name + ".exe") ? name + ".exe" : "";
    }
    std::vector<std::string> cands;
    if (ends_ci(name, ".exe") || ends_ci(name, ".com")) cands.push_back(name);
    else cands = {name + ".exe", name + ".com"};
    const std::string path = env_utf8(L"PATH");
    size_t b = 0;
    for (;;) {
        const size_t e = path.find(PATH_LIST_SEP, b);
        std::string dir = path.substr(b, e == std::string::npos ? std::string::npos : e - b);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
        if (!dir.empty()) {   // never the current directory (CreateProcess' own search would try it first)
            if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
            for (const auto& c : cands)
                if (is_executable(dir + c)) return dir + c;
        }
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return "";
}

namespace {

bool python_works(const std::vector<std::string>& prefix) {
    std::vector<std::string> a = prefix;
    a.insert(a.end(), {"-c", "import sys; sys.exit(sys.version_info[0] != 3)"});
    std::string out;
    // Hidden, not None: py.exe (or a venv's redirector) is no leaf - the
    // python.exe it starts opens a visible console window under a detached parent
    return run_capture(a, 5000, &out, nullptr, 4096, false, Console::Hidden);
}

std::vector<std::string> find_python() {
    // -X utf8: Python writes UTF-8 to pipes and files (the ANSI code page
    // otherwise - the first emoji in an AI message killed the bridge), also
    // with -I, which ignores PYTHONUTF8
    if (std::string p = env_utf8(L"KRAKEN_PYTHON"); !p.empty()) {
        if (is_bare(p)) p = find_in_path(p);
        if (!p.empty() && python_works({p})) return {p, "-X", "utf8"};
        warn_once(g_warned_python, "KRAKEN_PYTHON does not run Python 3 - looking for another interpreter");
    }
    std::string py = find_in_path("py");
    if (py.empty()) {
        const std::string root = env_utf8(L"SystemRoot");
        if (!root.empty() && is_executable(root + "\\py.exe")) py = root + "\\py.exe";
    }
    if (!py.empty() && python_works({py, "-3"})) return {py, "-3", "-X", "utf8"};
    // python3.exe / python.exe - the Microsoft Store stub (exit 9009) fails the probe
    for (const char* n : {"python3", "python"}) {
        const std::string p = find_in_path(n);
        if (!p.empty() && python_works({p})) return {p, "-X", "utf8"};
    }
    return {};
}

}  // namespace

const std::vector<std::string>& python_argv() {
    static const std::vector<std::string> v = find_python();
    return v;
}

std::vector<std::string> python_cmd(std::vector<std::string> script_and_args) {
    const auto& py = python_argv();
    if (py.empty()) return {};
    std::vector<std::string> v = py;
    v.insert(v.end(), std::make_move_iterator(script_and_args.begin()), std::make_move_iterator(script_and_args.end()));
    return v;
}

void lower_own_priority() { SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); }

// ---- ExitStatus --------------------------------------------------------------
std::string ExitStatus::signal_name() const {
    if (killed_by_us) return signal == 9 ? "killed" : "terminated";
    char hex[16];
    snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(raw));
    const char* what = nullptr;
    switch (raw) {
        case 0xC0000005u: what = "access violation"; break;
        case 0xC0000409u: what = "abort / fail-fast"; break;
        case 0xC00000FDu: what = "stack overflow"; break;
        case 0xC000001Du: what = "illegal instruction"; break;
        case 0xC0000096u: what = "privileged instruction"; break;
        case 0xC0000006u: what = "in-page error"; break;
        case 0xC0000135u: what = "a DLL is missing"; break;
        case 0xC0000139u: what = "DLL entry point missing"; break;
        case 0xC0000142u: what = "DLL init failed"; break;
        case 0xC0000017u: what = "out of memory"; break;
        case 0xC000013Au: what = "Ctrl+C / Ctrl+Break"; break;
        case 0xC0000094u: what = "integer divide by zero"; break;
        case 0xC0000095u: what = "integer overflow"; break;
        case 0x80000003u: what = "breakpoint"; break;
        case 0xE06D7363u: what = "unhandled C++ exception"; break;
        default:
            if (raw >= 0xC000008Eu && raw <= 0xC0000093u) what = "floating-point error";
            break;
    }
    return what ? std::string(what) + ", " + hex : std::string("NTSTATUS ") + hex;
}

std::string ExitStatus::describe() const {
    return signaled() ? "crashed (" + signal_name() + ")" : "exited with status " + std::to_string(code);
}

// ---- PipePair ----------------------------------------------------------------
struct PipePair::Impl {
    HANDLE r = nullptr, w = nullptr;   // not inheritable; spawn() hands a child an inheritable duplicate
};

PipePair::PipePair() : d_(std::make_unique<Impl>()) {}
PipePair::~PipePair() { close(); }

bool PipePair::create(size_t buffer_bytes, std::string* err) {
    close();
    if (!CreatePipe(&d_->r, &d_->w, nullptr, static_cast<DWORD>(std::min<size_t>(buffer_bytes, 64u << 20)))) {
        const DWORD e = GetLastError();
        d_->r = d_->w = nullptr;
        if (err) *err = "pipe failed: " + win_error_text(errno_from_win(e), e);
        return false;
    }
    return true;
}

void PipePair::close() {
    if (d_->r) CloseHandle(d_->r);
    if (d_->w) CloseHandle(d_->w);
    d_->r = d_->w = nullptr;
}

// ---- Process -----------------------------------------------------------------
struct Process::Impl {
    // proc / job / reaped / the *_sent flags under mu: kill() and
    // terminate() may run on another thread than the one driving the Process
    std::mutex mu;
    HANDLE proc = nullptr;
    HANDLE job = nullptr;            // its own job: everything it starts (closing it kills what is left)
    std::atomic<long> pid{-1};
    bool reaped = false;
    bool kill_tree = false;          // kill() / terminate() end the whole job, not just the process
    bool break_ok = false;           // kill_tree child on our console: terminate() = CTRL_BREAK
    bool kill_sent = false, term_sent = false, break_sent = false;
    ExitStatus status;
    std::unique_ptr<WritePipe> in = std::make_unique<WritePipe>();   // heap: close_stdin() may hand it on
    ReadPipe out, err;

    ReadPipe& rp(Stream s) { return s == Stream::Err ? err : out; }
    void reap_locked() {
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        status = map_exit(code, kill_sent, term_sent, break_sent);
        reaped = true;
    }
    void hard_kill_locked(UINT code) {
        if (kill_tree && job) TerminateJobObject(job, code);
        else TerminateProcess(proc, code);   // as kill(pid) on Linux: what it started lives on (until close())
    }
};

Process::Process() : d_(std::make_unique<Impl>()) {}
Process::~Process() { close(); }

bool Process::started() const { return d_->proc != nullptr; }

long Process::pid() const { return d_->pid.load(); }

bool Process::spawn(const SpawnOptions& o, std::string* err, int* err_code) {
    auto fail = [&](int code, const std::string& text) {
        if (err) *err = text;
        if (err_code) *err_code = code;
        return false;
    };
    close();
    if (o.argv.empty()) return fail(EINVAL, "no command");

    // the program: CreateProcess gets its exact file name (with an
    // application name it appends no ".exe" and searches nothing)
    std::string prog = o.argv[0];
    if (o.search_path && is_bare(prog)) {
        prog = find_in_path(prog);
        if (prog.empty()) return fail(ENOENT, errno_text(ENOENT));
    } else if (!ends_ci(prog, ".exe") && !file_exists(prog) && file_exists(prog + ".exe")) {
        prog += ".exe";
    }
    // cmd.exe parses a batch file's arguments again by its own rules
    // (the CVE-2024-24576 class): refused, as nothing here needs one
    if (ends_ci(prog, ".bat") || ends_ci(prog, ".cmd"))
        return fail(ENOEXEC, std::string(errno_text(ENOEXEC)) + " (batch files are not run)");
    bool ok = true, ok1 = true;
    const std::wstring app = widen(prog, &ok);
    std::wstring cmd;
    if (!append_argv0(cmd, widen(o.argv[0], &ok1))) return fail(EINVAL, "a quote in the program name");
    ok = ok && ok1;
    for (size_t i = 1; i < o.argv.size(); i++) {
        cmd += L' ';
        append_quoted(cmd, widen(o.argv[i], &ok1));
        ok = ok && ok1;
    }
    if (!ok) return fail(EILSEQ, std::string(errno_text(EILSEQ)) + " (an argument is not UTF-8)");
    if (cmd.size() > 32766) return fail(E2BIG, errno_text(E2BIG));   // CreateProcess' limit, NUL included
    std::wstring wcwd;
    if (!o.cwd.empty()) wcwd = widen(o.cwd);
    std::vector<wchar_t> envblk;
    if ((!o.env_set.empty() || !o.env_unset.empty()) && !build_env(o, &envblk))
        return fail(EILSEQ, std::string(errno_text(EILSEQ)) + " (an environment variable is not UTF-8)");

    // the child's std handles (inheritable) and our pipe ends
    HANDLE child[3] = {nullptr, nullptr, nullptr};
    bool own[3] = {true, true, true};
    HANDLE srv_in = nullptr, srv_out = nullptr, srv_err = nullptr;
    auto cleanup = [&] {
        for (int i = 0; i < 3; i++)
            if (child[i] && own[i]) CloseHandle(child[i]);
        for (HANDLE h : {srv_in, srv_out, srv_err})
            if (h) CloseHandle(h);
    };
    DWORD werr = 0;
    const DWORD std_id[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    const Stdio mode[3] = {o.in, o.out, o.err};
    for (int i = 0; i < 3; i++) {
        switch (mode[i]) {
            case Stdio::Inherit:
                child[i] = dup_inheritable(GetStdHandle(std_id[i]));
                if (!child[i]) child[i] = open_nul(i != 0);   // we have no such stream: NUL
                break;
            case Stdio::Null:
                child[i] = open_nul(i != 0);
                break;
            case Stdio::Pipe: {
                HANDLE* srv = i == 0 ? &srv_in : i == 1 ? &srv_out : &srv_err;
                const size_t bytes = i == 0 ? o.in_pipe_bytes : i == 1 ? o.out_pipe_bytes : o.err_pipe_bytes;
                if (!make_pipe(i == 0, bytes, srv, &child[i], &werr)) {
                    *srv = nullptr;
                    child[i] = nullptr;
                    cleanup();
                    return fail(errno_from_win(werr), "pipe failed: " + win_error_text(errno_from_win(werr), werr));
                }
                break;
            }
            case Stdio::SameAsStdout:
                if (i != 2) {
                    cleanup();
                    return fail(EINVAL, "SameAsStdout is for stderr only");
                }
                child[2] = child[1];
                own[2] = false;
                break;
            case Stdio::Handle: {
                const PipePair* pp = i == 0 ? o.in_pair : i == 1 ? o.out_pair : nullptr;
                HANDLE end = !pp ? nullptr : i == 0 ? pp->d_->r : pp->d_->w;
                child[i] = dup_inheritable(end);
                if (!child[i]) {
                    cleanup();
                    return fail(EBADF, "no pipe pair");
                }
                break;
            }
        }
        if (!child[i]) {
            werr = GetLastError();
            cleanup();
            return fail(errno_from_win(werr), "cannot open NUL: " + win_error_text(errno_from_win(werr), werr));
        }
    }

    // exactly these handles are inherited - each one once (a duplicate entry
    // fails the CreateProcess with ERROR_INVALID_PARAMETER)
    HANDLE list[3];
    size_t nlist = 0;
    for (HANDLE h : child)
        if (std::find(list, list + nlist, h) == list + nlist) list[nlist++] = h;
    SIZE_T alsz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &alsz);
    std::vector<char> albuf(alsz);
    auto al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(albuf.data());
    if (!InitializeProcThreadAttributeList(al, 1, 0, &alsz)) {
        werr = GetLastError();
        cleanup();
        return fail(errno_from_win(werr), win_error_text(errno_from_win(werr), werr));
    }
    if (!UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, nlist * sizeof(HANDLE), nullptr, nullptr)) {
        werr = GetLastError();
        DeleteProcThreadAttributeList(al);
        cleanup();
        return fail(errno_from_win(werr), win_error_text(errno_from_win(werr), werr));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = child[0];
    si.StartupInfo.hStdOutput = child[1];
    si.StartupInfo.hStdError = child[2];
    si.lpAttributeList = al;

    Console con = o.console;
    if (con == Console::Auto)
        con = (o.in == Stdio::Inherit || o.out == Stdio::Inherit || o.err == Stdio::Inherit) ? Console::Share : Console::None;
    if (con == Console::Share && !have_console()) con = Console::Hidden;   // none to share
    // suspended until it is in its job: it can't start anything outside it
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
    if (con == Console::Hidden) flags |= CREATE_NO_WINDOW;
    else if (con == Console::None) flags |= DETACHED_PROCESS;
    if (o.priority == Priority::Below) flags |= BELOW_NORMAL_PRIORITY_CLASS;
    else if (o.priority == Priority::Idle) flags |= IDLE_PRIORITY_CLASS;
    // its own console process group: our console's Ctrl+C doesn't reach it
    // (a separate pgid on Linux) and terminate() can send it a CTRL_BREAK
    if (o.kill_tree) flags |= CREATE_NEW_PROCESS_GROUP;

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                                        envblk.empty() ? nullptr : envblk.data(), wcwd.empty() ? nullptr : wcwd.c_str(),
                                        &si.StartupInfo, &pi);
    werr = created ? 0 : GetLastError();
    DeleteProcThreadAttributeList(al);
    for (int i = 0; i < 3; i++)   // the child has its own copies now
        if (child[i] && own[i]) CloseHandle(child[i]);
    child[0] = child[1] = child[2] = nullptr;
    if (!created) {
        cleanup();
        return fail(errno_from_win(werr), win_error_text(errno_from_win(werr), werr));
    }

    HANDLE job = make_kill_job();
    if (!job || !AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD je = GetLastError();
        const std::string base = prog.substr(prog.find_last_of("/\\") + 1);
        warn_once(g_warned_job, "cannot put " + base + " in a job (Windows error " + std::to_string(je) +
                                    ") - it may outlive kraken_doa" +
                                    (o.kill_tree ? ", and stopping it ends only its first process" : ""));
        if (job) CloseHandle(job);
        job = nullptr;
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        werr = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        cleanup();
        return fail(errno_from_win(werr), win_error_text(errno_from_win(werr), werr));
    }
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->proc = pi.hProcess;
        d_->job = job;
        d_->pid = static_cast<long>(pi.dwProcessId);
        d_->reaped = false;
        d_->kill_tree = o.kill_tree;
        d_->break_ok = o.kill_tree && con == Console::Share;
        d_->kill_sent = d_->term_sent = d_->break_sent = false;
        d_->status = ExitStatus{};
    }
    if (srv_in) d_->in->open(srv_in);
    if (srv_out) d_->out.open(srv_out);
    if (srv_err) d_->err.open(srv_err);
    return true;
}

ptrdiff_t Process::write_some(const void* p, size_t n) {
    WritePipe& w = *d_->in;
    if (!w.h) return -1;
    w.collect();
    if (w.broken) return -1;
    if (w.pending || n == 0) return 0;   // the previous write is still in flight: "would block"
    const size_t k = std::min(n, MAX_WRITE_CHUNK);
    w.buf.assign(static_cast<const char*>(p), static_cast<const char*>(p) + k);
    w.off = 0;
    w.len = k;
    w.issue();
    if (w.broken) return -1;
    w.collect();   // done at once (room in the pipe): the next call can write again
    return static_cast<ptrdiff_t>(k);
}

ptrdiff_t Process::read_some(Stream s, void* p, size_t n) {
    ReadPipe& r = d_->rp(s);
    if (!r.h) return -1;
    for (int i = 0; i < 4; i++) {   // a few rounds: zero-byte completions
        r.collect();
        if (r.has_data()) {
            const size_t k = std::min(n, r.have - r.off);
            std::memcpy(p, r.buf.data() + r.off, k);
            r.off += k;
            if (!r.has_data()) r.start();   // the next read at once - done at once if more is buffered
            return static_cast<ptrdiff_t>(k);
        }
        if (r.eof) return -1;
        if (r.error) return -2;
        if (r.pending) return 0;
        r.start();
    }
    return 0;
}

ptrdiff_t Process::read(Stream s, void* p, size_t n, int timeout_ms) {
    ReadPipe& r = d_->rp(s);
    if (!r.h) return -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    for (;;) {
        const ptrdiff_t got = read_some(s, p, n);
        if (got != 0) return got;
        DWORD left = INFINITE;
        if (timeout_ms >= 0) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (ms <= 0) return 0;
            left = static_cast<DWORD>(ms);
        }
        if (r.pending) WaitForSingleObject(r.ov.hEvent, left);   // set when the read completes; the next round takes it
        else Sleep(1);   // nothing in flight (zero-byte reads): don't spin
    }
}

void Process::wait_io(int ms, bool want_read_out, bool want_write_in) {
    HANDLE hs[2];
    DWORD n = 0;
    if (want_read_out && d_->out.h) {
        ReadPipe& r = d_->out;
        r.collect();
        if (!r.pending) {
            r.start();
            r.collect();
        }
        if (r.has_data() || r.eof || r.error) return;   // readable now / at EOF (POLLIN / POLLHUP)
        if (r.pending) hs[n++] = r.ov.hEvent;
    }
    if (want_write_in && d_->in->h) {
        WritePipe& w = *d_->in;
        w.collect();
        if (!w.pending) return;   // can take more now (or broken: write_some says so)
        hs[n++] = w.ov.hEvent;
    }
    if (n) WaitForMultipleObjects(n, hs, FALSE, ms < 0 ? INFINITE : static_cast<DWORD>(ms));
}

bool Process::stdin_open() const { return d_->in->h != nullptr; }

// EOF: the child exits on its own - after reading what was accepted
void Process::close_stdin() {
    WritePipe& w = *d_->in;
    if (!w.h) return;
    w.collect();
    if (w.pending && !w.broken) {   // a write in flight: closed once it has completed (StdinCloser)
        StdinCloser::get().adopt(std::move(d_->in));
        d_->in = std::make_unique<WritePipe>();
        return;
    }
    w.close();
}

bool Process::try_wait(ExitStatus* st) {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->proc) return false;
    if (!d_->reaped) {
        if (WaitForSingleObject(d_->proc, 0) != WAIT_OBJECT_0) return false;
        d_->reap_locked();
    }
    if (st) *st = d_->status;
    return true;
}

ExitStatus Process::wait() {
    ExitStatus st;
    wait_for(-1, &st);
    return st;
}

bool Process::wait_for(int ms, ExitStatus* st) {
    HANDLE h;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (!d_->proc) return false;
        if (d_->reaped) {
            if (st) *st = d_->status;
            return true;
        }
        h = d_->proc;   // stays open: only close() - on this thread - closes it
    }
    if (WaitForSingleObject(h, ms < 0 ? INFINITE : static_cast<DWORD>(ms)) != WAIT_OBJECT_0) return false;
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->reaped) d_->reap_locked();
    if (st) *st = d_->status;
    return true;
}

void Process::kill() {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->proc || d_->reaped) return;
    d_->kill_sent = true;
    d_->hard_kill_locked(137);
}

void Process::terminate() {
    std::lock_guard<std::mutex> lk(d_->mu);
    if (!d_->proc || d_->reaped) return;
    // the group id must be OUR child's: aimed at a live process that leads
    // no group, a CTRL_BREAK goes to every process on the console (measured -
    // kraken_doa's neighbours, heimdall in a shared console). Ours was created
    // CREATE_NEW_PROCESS_GROUP and its pid can't be reused while we hold its
    // handle: alive, its group gets it; exited, what is left of its group
    // (nobody if none) - also measured
    if (d_->break_ok && GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(d_->pid.load()))) {
        d_->break_sent = true;   // Python: SIGBREAK; default: exit 0xC000013A
        return;
    }
    d_->term_sent = true;
    d_->hard_kill_locked(143);
}

void Process::close() {
    if (started()) {
        ExitStatus st;
        if (!try_wait(&st)) {   // not reaped: none left running behind our back (as the destructor)
            kill();
            wait();
        }
    }
    d_->in->close();
    d_->out.close();
    d_->err.close();
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->proc) CloseHandle(d_->proc);
    if (d_->job) CloseHandle(d_->job);   // KILL_ON_JOB_CLOSE: what is left of the tree goes too
    d_->proc = d_->job = nullptr;
    d_->pid = -1;
    d_->reaped = false;
    d_->kill_tree = d_->break_ok = d_->kill_sent = d_->term_sent = d_->break_sent = false;
}

// ---- run_capture -------------------------------------------------------------
bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out, ExitStatus* st,
                 size_t max_bytes, bool search_path, Console console) {
    Process p;
    SpawnOptions o;
    o.argv = argv;
    o.search_path = search_path;
    o.in = Stdio::Null;
    o.out = Stdio::Pipe;
    o.err = Stdio::Null;
    o.console = console;
    if (!p.spawn(o, nullptr)) return false;
    out->clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        const int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            timed_out = true;
            break;
        }
        char buf[4096];
        const ptrdiff_t n = p.read(Stream::Out, buf, sizeof buf, left);
        if (n == 0) {
            timed_out = true;
            break;
        }
        if (n == -2) {   // a read error: our end stays open until the reap - don't let it block on it
            p.kill();
            break;
        }
        if (n < 0) break;   // EOF
        if (out->size() < max_bytes) out->append(buf, static_cast<size_t>(n));
    }
    if (timed_out) p.kill();
    const ExitStatus s = p.wait();
    p.close();
    if (st) *st = s;
    return !timed_out && s.ok();
}

}  // namespace kproc

#endif  // _WIN32
