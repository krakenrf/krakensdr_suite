#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KrakenRF Inc. - MIT so that decoder plugins of any licence
// can link it (licence text: kraken_doa_v2/plugins/sdk/LICENSE, root LICENSE)
// POSIX functions the MinGW-w64 UCRT runtime lacks, with their Linux meaning,
// so Linux code compiles unchanged on Windows. Force-included into every C++
// file of a Windows build (`-include win/posix_compat.h`, see the Makefiles);
// never included on Linux. Only shims whose behaviour MATCHES belong here -
// what behaves differently on Windows (sockets: net_compat.hpp; child
// processes; rename over an existing file; mmap; /proc ...) is ported where
// it is used. Identical copy in heimdall_v2/win and kraken_doa_v2/win.
//
// The Windows builds also define _FILE_OFFSET_BITS=64 (off_t, lseek, stat's
// st_size, fseeko are 32-bit otherwise: a 5 GB lseek silently lands at 705
// MB), WIN32_LEAN_AND_MEAN + NOMINMAX (windows.h without min/max macros and
// the rarely used headers) and _WIN32_WINNT = Windows 10.
#if defined(_WIN32) && defined(__cplusplus)

// localtime_r / gmtime_r / asctime_r / ctime_r from MinGW's time.h
#ifndef _POSIX_THREAD_SAFE_FUNCTIONS
#define _POSIX_THREAD_SAFE_FUNCTIONS 200112L
#endif

#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Signals Windows doesn't have. signal() refuses an unknown number (SIG_ERR,
// no effect), which is what Linux code installing a handler / SIG_IGN for
// them needs on Windows: there is no SIGPIPE (a write to a closed socket or
// pipe just fails), no SIGHUP (closing the console sends CTRL_CLOSE_EVENT -
// see the apps' shutdown handling). kill() is not shimmed: processes are
// ported where they are used.
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGUSR1
#define SIGUSR1 10
#endif
#ifndef SIGUSR2
#define SIGUSR2 12
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif

// open() flags: close-on-exec = not inherited by child processes
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NOATIME
#define O_NOATIME 0
#endif

// a TU built without the Makefiles' -D_FILE_OFFSET_BITS=64 would read
// recordings past 2 GiB from wrong offsets - stop the build instead
static_assert(sizeof(off_t) == 8 && sizeof(((struct stat*)nullptr)->st_size) == 8,
              "Windows builds need -D_FILE_OFFSET_BITS=64 (see the Makefiles)");

// mkdir(path, mode): Windows directories have no POSIX mode bits
inline int mkdir(const char* path, mode_t) { return _mkdir(path); }

// no symbolic links in the POSIX sense: lstat = stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
inline int lstat(const char* path, struct stat* st) { return stat(path, st); }

// fsync / fdatasync: flush the file's data to the disk (FlushFileBuffers)
inline int fsync(int fd) { return _commit(fd); }
inline int fdatasync(int fd) { return _commit(fd); }

// page-cache hints: no Windows equivalent per range - accepted, no effect
#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_RANDOM 1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED 3
#define POSIX_FADV_DONTNEED 4
#define POSIX_FADV_NOREUSE 5
#endif
inline int posix_fadvise(int, off_t, off_t, int) { return 0; }
// write-back scheduling hint (Linux): the data still reaches the disk when
// the file is closed or fsync'ed - accepted, no effect
#ifndef SYNC_FILE_RANGE_WAIT_BEFORE
#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#define SYNC_FILE_RANGE_WRITE 2
#define SYNC_FILE_RANGE_WAIT_AFTER 4
#endif
inline int sync_file_range(int, off_t, off_t, unsigned int) { return 0; }

// positional read / write (win/win_init.cpp: ReadFile / WriteFile at an
// offset on the fd's handle). Unlike POSIX they also move the fd's file
// position - don't mix them with read()/write() on the same fd.
extern "C" ssize_t kraken_pread(int fd, void* buf, size_t n, long long off);
extern "C" ssize_t kraken_pwrite(int fd, const void* buf, size_t n, long long off);
inline ssize_t pread(int fd, void* buf, size_t n, off_t off) { return kraken_pread(fd, buf, n, off); }
inline ssize_t pwrite(int fd, const void* buf, size_t n, off_t off) { return kraken_pwrite(fd, buf, n, off); }

// std::this_thread::sleep_for / sleep_until precision. libstdc++'s inline
// sleep_for calls ::nanosleep, which winpthreads rounds up to the 15.6 ms
// system tick even after timeBeginPeriod(1) (measured: sleep_for(1ms) = 15.6
// ms, 50 ms = 62 ms, 100 us returns at once) - the drain's 100 us poll became
// a busy spin, its "500 ms" flush wait 7.8 s, the lag servo's 50 ms tick
// 62.5 ms. Every nanosleep (this header comes before <thread>) goes to
// kraken_nanosleep in win/win_init.cpp: a high-resolution waitable timer.
#include <pthread.h>
extern "C" int kraken_nanosleep(const struct timespec* request, struct timespec* remain);
#define nanosleep kraken_nanosleep

// UTC broken-down time -> time_t
inline time_t timegm(struct tm* t) { return _mkgmtime(t); }

// environment
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!name || !*name || std::strchr(name, '=')) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && std::getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
inline int unsetenv(const char* name) { return _putenv_s(name, "") == 0 ? 0 : -1; }

// realpath: absolute path of an EXISTING file (no symlinks to resolve)
inline char* realpath(const char* path, char* resolved) {
    struct stat st;
    if (!path || stat(path, &st) != 0) return nullptr;   // errno from stat
    char* r = _fullpath(resolved, path, resolved ? _MAX_PATH : 0);
    if (!r) errno = ENAMETOOLONG;
    return r;
}

// strptime: the glibc conversions this code base uses (and the common rest):
// %Y %y %C %m %d %e %H %I %M %S %p %j %b %B %h %a %A %T %R %D %F %n %t %%,
// whitespace matching any amount of whitespace. Returns a pointer past the
// parsed text, or nullptr when the input doesn't match the format.
inline const char* kraken_strptime_num(const char* s, int width, int lo, int hi, int* out) {
    while (std::isspace(static_cast<unsigned char>(*s))) s++;
    bool neg = false;
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    int v = 0, n = 0;
    while (n < width && std::isdigit(static_cast<unsigned char>(*s))) {
        v = v * 10 + (*s++ - '0');
        n++;
    }
    if (n == 0) return nullptr;
    if (neg) v = -v;
    if (v < lo || v > hi) return nullptr;
    *out = v;
    return s;
}
inline const char* kraken_strptime_name(const char* s, const char* const* names, int count, int* out) {
    while (std::isspace(static_cast<unsigned char>(*s))) s++;
    for (int i = 0; i < count; i++) {
        const size_t full = std::strlen(names[i]);
        if (_strnicmp(s, names[i], full) == 0) {
            *out = i;
            return s + full;
        }
    }
    for (int i = 0; i < count; i++) {   // abbreviation: first 3 letters
        if (_strnicmp(s, names[i], 3) == 0) {
            *out = i;
            return s + 3;
        }
    }
    return nullptr;
}
inline char* strptime(const char* s, const char* f, struct tm* tm) {
    static const char* const MON[] = {"January", "February", "March",     "April",   "May",      "June",
                                      "July",    "August",   "September", "October", "November", "December"};
    static const char* const DAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    int v = 0;
    int century = -1, year2 = -1;
    bool pm = false, have_pm = false;
    while (s && *f) {
        if (std::isspace(static_cast<unsigned char>(*f))) {
            while (std::isspace(static_cast<unsigned char>(*s))) s++;
            f++;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) return nullptr;
            s++;
            f++;
            continue;
        }
        f++;
        if (*f == 'E' || *f == 'O') f++;   // locale modifiers: ignored
        switch (*f) {
            case 'Y': if ((s = kraken_strptime_num(s, 4, 0, 9999, &v))) tm->tm_year = v - 1900; break;
            case 'y': if ((s = kraken_strptime_num(s, 2, 0, 99, &v))) year2 = v; break;
            case 'C': if ((s = kraken_strptime_num(s, 2, 0, 99, &v))) century = v; break;
            case 'm': if ((s = kraken_strptime_num(s, 2, 1, 12, &v))) tm->tm_mon = v - 1; break;
            case 'd': case 'e': if ((s = kraken_strptime_num(s, 2, 1, 31, &v))) tm->tm_mday = v; break;
            case 'H': if ((s = kraken_strptime_num(s, 2, 0, 23, &v))) tm->tm_hour = v; break;
            case 'I': if ((s = kraken_strptime_num(s, 2, 1, 12, &v))) tm->tm_hour = v % 12; break;
            case 'M': if ((s = kraken_strptime_num(s, 2, 0, 59, &v))) tm->tm_min = v; break;
            case 'S': if ((s = kraken_strptime_num(s, 2, 0, 61, &v))) tm->tm_sec = v; break;
            case 'j': if ((s = kraken_strptime_num(s, 3, 1, 366, &v))) tm->tm_yday = v - 1; break;
            case 'b': case 'B': case 'h': if ((s = kraken_strptime_name(s, MON, 12, &v))) tm->tm_mon = v; break;
            case 'a': case 'A': if ((s = kraken_strptime_name(s, DAY, 7, &v))) tm->tm_wday = v; break;
            case 'p':
                while (std::isspace(static_cast<unsigned char>(*s))) s++;
                if (_strnicmp(s, "AM", 2) == 0) { have_pm = true; pm = false; s += 2; }
                else if (_strnicmp(s, "PM", 2) == 0) { have_pm = true; pm = true; s += 2; }
                else s = nullptr;
                break;
            case 'T': s = strptime(s, "%H:%M:%S", tm); break;
            case 'R': s = strptime(s, "%H:%M", tm); break;
            case 'D': s = strptime(s, "%m/%d/%y", tm); break;
            case 'F': s = strptime(s, "%Y-%m-%d", tm); break;
            case 'n': case 't': while (std::isspace(static_cast<unsigned char>(*s))) s++; break;
            case '%': if (*s == '%') s++; else s = nullptr; break;
            default: return nullptr;   // a conversion this shim doesn't know
        }
        if (*f) f++;
    }
    if (!s) return nullptr;
    if (year2 >= 0) tm->tm_year = (century >= 0 ? century * 100 + year2 : (year2 < 69 ? 2000 + year2 : 1900 + year2)) - 1900;
    else if (century >= 0) tm->tm_year = century * 100 - 1900;
    if (have_pm && pm) tm->tm_hour += 12;
    return const_cast<char*>(s);
}

#endif  // _WIN32 && __cplusplus
