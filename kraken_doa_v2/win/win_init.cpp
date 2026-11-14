// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KrakenRF Inc. - MIT so that decoder plugins of any licence
// can link it (licence text: kraken_doa_v2/plugins/sdk/LICENSE, root LICENSE)

// Windows process setup, linked into every .exe of the suite (Windows builds
// only - the Makefiles add it next to win/app.rc). Runs as the first C++
// static initializer, before any file is opened:
//  - BINARY file mode by default, as on Linux: open() / fopen() / iostreams
//    without an explicit text/binary flag, and stdin / stdout / stderr, no
//    longer translate \n <-> \r\n (or stop at ^Z). Without it a settings file,
//    a recording or a plugin's stdin/stdout sample stream opened the POSIX way
//    would be silently corrupted. (MinGW's binmode.o does this for msvcrt; the
//    UCRT toolchain's binmode.o is empty, so it is done here explicitly.)
//  - 1 ms timer resolution for the Win32 waits (Sleep, WaitForSingleObject,
//    moodycamel's semaphores, libuv / uWS timers), which otherwise round up
//    to the 15.6 ms system tick; and no Windows 11 power throttling, which
//    may ignore that request for a process without a visible window and run
//    it at efficiency clocks. std::this_thread::sleep_for goes through
//    kraken_nanosleep below instead (see win/posix_compat.h).
//  - Winsock started: every socket call - and gethostname / getaddrinfo -
//    fails with WSANOTINITIALISED before WSAStartup (heimdall opens its
//    8091 / 8092 / 1234 servers before uWS / libuv would start it).
//  - no "There is no disk in the drive" dialog box when a drive is probed
//    (free space / the drive list of the recorder and decoder log), and no
//    Windows Error Reporting / abort() dialog: a crashed plugin or app must
//    exit at once so its supervisor (kraken_doa, krakensdr.exe) restarts it.
//  - a POSIX TZ ("Pacific/Auckland", set by MSYS2 / Git Bash or by hand) is
//    dropped: the C runtime only understands "NZST-12NZDT" style values and
//    took that one as UTC, so local-time file names and logs were off.
//    Without TZ it uses the Windows time zone.
// Identical copy in heimdall_v2/win and kraken_doa_v2/win.
#ifdef _WIN32
#include <winsock2.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <timeapi.h>

#include <cerrno>
#include <cstring>

namespace {

struct WinProcessInit {
    WinProcessInit() {
        _set_fmode(_O_BINARY);
        _setmode(0, _O_BINARY);
        _setmode(1, _O_BINARY);
        _setmode(2, _O_BINARY);
        timeBeginPeriod(1);
        PROCESS_POWER_THROTTLING_STATE pt{};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        pt.StateMask = 0;   // = never throttle either
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        if (const char* tz = getenv("TZ"); tz && strchr(tz, '/')) {
            _putenv("TZ=");
            _tzset();
        }
    }
    ~WinProcessInit() { timeEndPeriod(1); }
};

WinProcessInit win_process_init __attribute__((init_priority(101)));

// one high-resolution timer per thread (Windows 10 1803+; Sleep() otherwise)
struct SleepTimer {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~SleepTimer() {
        if (h) CloseHandle(h);
    }
};

}  // namespace

// nanosleep with sub-millisecond resolution (measured: 1 ms -> ~1.5 ms,
// 100 us -> ~0.5 ms). Never interrupted, so *remain is always zero.
// libstdc++ passes the SAME timespec as request and remain: read it first.
extern "C" int kraken_nanosleep(const struct timespec* request, struct timespec* remain) {
    if (!request || request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    const long long ns = static_cast<long long>(request->tv_sec) * 1000000000LL + request->tv_nsec;
    if (remain) {
        remain->tv_sec = 0;
        remain->tv_nsec = 0;
    }
    if (ns <= 0) return 0;
    thread_local SleepTimer timer;
    if (timer.h) {
        LARGE_INTEGER due;
        due.QuadPart = -((ns + 99) / 100);   // relative, in 100 ns units
        if (SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer.h, INFINITE) == WAIT_OBJECT_0)
            return 0;
    }
    Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    return 0;
}

// pread / pwrite on a C runtime fd (win/posix_compat.h)
extern "C" ssize_t kraken_pread(int fd, void* buf, size_t n, long long off) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE || off < 0) {
        errno = off < 0 ? EINVAL : EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(static_cast<unsigned long long>(off) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(off) >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(n > (1u << 30) ? (1u << 30) : n), &got, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(got);
}

extern "C" ssize_t kraken_pwrite(int fd, const void* buf, size_t n, long long off) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE || off < 0) {
        errno = off < 0 ? EINVAL : EBADF;
        return -1;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(static_cast<unsigned long long>(off) & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<unsigned long long>(off) >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, static_cast<DWORD>(n > (1u << 30) ? (1u << 30) : n), &put, &ov)) {
        errno = GetLastError() == ERROR_DISK_FULL ? ENOSPC : EIO;
        return -1;
    }
    return static_cast<ssize_t>(put);
}
#endif
