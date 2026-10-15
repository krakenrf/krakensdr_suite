#pragma once
// Child processes (decoder plugins, the Voice to Text helper, the AI bridge,
// curl downloads, the test-signal generator) must not inherit kraken_doa's
// sockets and files: a plugin's copy of the 8091 data connection kept it open
// after kraken_doa had reconnected, and the 8092 / gpsd sockets went the same
// way. Every spawn closes all fds above stderr in the child (the sockets are
// also created close-on-exec, which covers a libc without closefrom).
#include <spawn.h>
#include <unistd.h>

namespace spawn_fds {

// "close every fd >= 3" - add it as the LAST file action, after the dup2s
// onto 0-2 (their source fds are >= 3)
inline void close_above_stdio(posix_spawn_file_actions_t* fa) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(fa, 3);
#else
    (void)fa;
#endif
}

// the same in a fork()ed child, right before exec
inline void close_above_stdio_in_child() {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    closefrom(3);
#endif
}

}  // namespace spawn_fds
