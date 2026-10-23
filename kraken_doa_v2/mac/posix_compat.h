#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KrakenRF Inc. - MIT so that decoder plugins of any licence
// can link it (licence text: kraken_doa_v2/plugins/sdk/LICENSE, root LICENSE)
// Linux functions macOS lacks (or spells differently), with their Linux
// meaning, so Linux code compiles unchanged on macOS. Force-included into
// every C++ file of a macOS build (`-include mac/posix_compat.h`, see the
// Makefiles); never included elsewhere. Like win/posix_compat.h, only shims
// whose behaviour MATCHES belong here - sockets are in net_compat.hpp, things
// that behave differently are ported where they are used. Identical copy in
// heimdall_v2/mac and kraken_doa_v2/mac.
#if defined(__APPLE__) && defined(__cplusplus)

#include <bit>
#include <cerrno>
#include <fcntl.h>
#include <type_traits>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

// pipe2(): pipe() + the flags set afterwards (not atomic against a fork() on
// another thread in between - every spawn here also closes fds above stderr
// in the child or runs through posix_spawn's file actions)
inline int pipe2(int fds[2], int flags) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        if (((flags & O_CLOEXEC) && fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0) ||
            ((flags & O_NONBLOCK) && fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) != 0)) {
            const int e = errno;
            close(fds[0]);
            close(fds[1]);
            errno = e;
            return -1;
        }
    }
    return 0;
}

// pthread_setname_np(thread, name): macOS names only the calling thread
// (pthread_setname_np(name)); every caller here names itself
inline int pthread_setname_np(pthread_t thread, const char* name) {
    if (!pthread_equal(thread, pthread_self())) return ESRCH;
    return pthread_setname_np(name);
}

// fdatasync: not declared on macOS - fsync (data + metadata; both hand the
// data to the drive, neither forces its cache: that is F_FULLFSYNC)
inline int fdatasync(int fd) { return fsync(fd); }

// page-cache hints (Linux): no per-range equivalent - accepted, no effect
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

// struct stat's timespec fields: st_mtimespec & co. on macOS
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

// std::bit_cast: in the compiler (__builtin_bit_cast) but missing from the
// libc++ of older Apple clang (macOS 12's Command Line Tools, clang 14.0.0)
#if !defined(__cpp_lib_bit_cast)
namespace std {
template <class To, class From>
constexpr To bit_cast(const From& from) noexcept {
    static_assert(sizeof(To) == sizeof(From) && is_trivially_copyable_v<To> && is_trivially_copyable_v<From>,
                  "bit_cast: sizes / types");
    return __builtin_bit_cast(To, from);
}
}  // namespace std
#endif

#endif
