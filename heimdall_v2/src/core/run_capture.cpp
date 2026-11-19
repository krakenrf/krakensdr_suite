// See run_capture.hpp
#include "run_capture.hpp"

#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>

#include <cwchar>   // _wcsicmp
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

constexpr size_t MAX_OUT = 1u << 20;

int ms_left(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// The C runtime's command-line rules (CommandLineToArgvW / the UCRT's parser)
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; i++) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') {
            i++;
            backslashes++;
        }
        if (i == a.size()) {
            r.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(backslashes * 2 + 1, L'\\');
        } else {
            r.append(backslashes, L'\\');
        }
        r += a[i];
    }
    r += L'"';
    return r;
}

// <name>.exe in a PATH directory; "" if none
std::wstring find_on_path(const std::string& name) {
    const DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring path(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(L"PATH", &path[0], n);
    path.resize(got < n ? got : 0);
    std::wstring file = widen(name);
    if (file.size() < 4 || _wcsicmp(file.c_str() + file.size() - 4, L".exe") != 0) file += L".exe";
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
        if (dir.empty()) continue;
        if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
        const std::wstring cand = dir + file;
        const DWORD attr = GetFileAttributesW(cand.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return cand;
    }
    return std::wstring();
}

#endif

}  // namespace

#ifdef _WIN32

bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out) {
    out->clear();
    if (argv.empty()) return false;
    const std::wstring exe = find_on_path(argv[0]);
    if (exe.empty()) return false;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    auto close_ends = [&] {
        if (wr) CloseHandle(wr);
        if (nul_in != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
        if (nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);
        wr = nullptr;
        nul_in = nul_err = INVALID_HANDLE_VALUE;
    };
    if (nul_in == INVALID_HANDLE_VALUE || nul_err == INVALID_HANDLE_VALUE) {
        close_ends();
        CloseHandle(rd);
        return false;
    }

    // exactly these three handles are inherited
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[3] = {nul_in, wr, nul_err};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        close_ends();
        CloseHandle(rd);
        return false;
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul_in;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = nul_err;
    si.lpAttributeList = attrs;

    std::wstring cmd = quote_arg(exe);
    for (size_t i = 1; i < argv.size(); i++) cmd += L" " + quote_arg(widen(argv[i]));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                   &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    close_ends();   // the child has its copies; EOF comes when it (and its children) close them
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    CloseHandle(pi.hThread);

    // Anonymous pipes have no timeout of their own: peek, read what is there
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;   // broken pipe: EOF
        if (avail > 0) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(rd, buf, avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf)), &got, nullptr) || got == 0) break;
            if (out->size() < MAX_OUT) out->append(buf, got);
            continue;
        }
        if (ms_left(deadline) == 0) {
            timed_out = true;
            break;
        }
        Sleep(5);
    }
    CloseHandle(rd);
    if (timed_out || WaitForSingleObject(pi.hProcess, static_cast<DWORD>(ms_left(deadline))) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

#else  // Linux, macOS

bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out) {
    out->clear();
    if (argv.empty()) return false;
    int fds[2];
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
#else
    if (pipe(fds) != 0) return false;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);   // the dup has no close-on-exec
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);   // what isn't close-on-exec (sockets, USB fds)
#endif
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    for (;;) {
        const int left = ms_left(deadline);
        if (left == 0) {
            timed_out = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int r = poll(&p, 1, left);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;   // deadline checked above
        char buf[4096];
        const ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            if (out->size() < MAX_OUT) out->append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;   // EOF (or an error)
    }
    close(fds[0]);

    // reap; still running at the deadline (or after a timeout): killed
    int st = 0;
    for (;;) {
        const pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) return false;   // ECHILD: reaped elsewhere - status unknown
        if (timed_out || ms_left(deadline) == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            return false;
        }
        usleep(2000);
    }
    return !timed_out && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

#endif
