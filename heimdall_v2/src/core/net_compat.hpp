#pragma once
// Socket portability: BSD sockets (Linux, macOS) <-> Winsock 2 (Windows). Header-only;
// the SAME file in heimdall_v2/src/core/ and kraken_doa_v2/include/utils/ - keep the two
// identical (each app builds on its own).
//
// Sockets stay in a plain `int` as on Linux: a Winsock SOCKET is a kernel
// handle value (far inside int range in practice) and INVALID_SOCKET converts
// to -1, so `fd < 0` checks and int fields keep working. The sock_* calls are
// the BSD calls on Linux - same behaviour as calling them directly - and the
// Winsock ones on Windows with errno set from WSAGetLastError(), so callers'
// `errno == EAGAIN` / strerror(errno) handling works unchanged on both.
// Files, pipes and the console are NOT sockets on Windows: never pass a
// socket to read()/write()/close() or a file descriptor to sock_*().
//
// Windows differences handled here:
//  - SO_REUSEADDR lets ANOTHER process bind a port that is already in use
//    (port hijacking), so a second heimdall would silently share 8091 instead
//    of failing. sock_setsockopt(SO_REUSEADDR, 1) sets SO_EXCLUSIVEADDRUSE
//    instead: bind() fails on a busy port, as on Linux. (A restart can rebind
//    a port whose old connections are in TIME_WAIT without SO_REUSEADDR.)
//  - SO_RCVTIMEO / SO_SNDTIMEO take a DWORD of milliseconds, not a timeval:
//    sock_setsockopt converts a timeval.
//  - no SIGPIPE (MSG_NOSIGNAL = 0); MSG_DONTWAIT doesn't exist - sock_recv
//    emulates it with a zero-timeout select(); SHUT_* = SD_*.
//  - a non-blocking connect() fails with WSAEWOULDBLOCK: reported as
//    EINPROGRESS, as on Linux.
//  - fd_set is a LIST of up to FD_SETSIZE sockets (not a bitmap indexed by
//    the fd), so `fd >= FD_SETSIZE` says nothing there: see sock_fits_fd_set.
//  - sockets created here are not inherited by child processes
//    (WSA_FLAG_NO_HANDLE_INHERIT, the SOCK_CLOEXEC equivalent).
#include <cerrno>
#include <cstddef>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <sys/types.h>

// EWOULDBLOCK has its own value in the UCRT (140) but equals EAGAIN on Linux,
// and Linux code tests either one: the same value here, as on Linux.
#undef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
// not a Winsock flag: interpreted (and removed) by sock_recv
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40000000
#endif
// socket()/accept4() type flags: interpreted (and removed) by sock_socket / sock_accept4
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x02000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x04000000
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif

inline int sock_errno_from_wsa(int e) {
    switch (e) {
        case WSAEWOULDBLOCK: return EAGAIN;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAEINTR: return EINTR;
        case WSAECONNRESET: return ECONNRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAENOTCONN: return ENOTCONN;
        case WSAEISCONN: return EISCONN;
        case WSAESHUTDOWN: return EPIPE;
        case WSAENOTSOCK: return ENOTSOCK;
        case WSAEBADF: return EBADF;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAEACCES: return EACCES;
        case WSAEINVAL: return EINVAL;
        case WSAEMFILE: return EMFILE;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAENETDOWN: return ENETDOWN;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAENETRESET: return ENETRESET;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAEHOSTDOWN: return EHOSTUNREACH;
        case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
        case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
        case WSAEOPNOTSUPP: return EOPNOTSUPP;
        case WSAEFAULT: return EFAULT;
        case WSANOTINITIALISED: return EINVAL;
        default: return EIO;
    }
}
inline void sock_set_errno() { errno = sock_errno_from_wsa(WSAGetLastError()); }
// after send/recv: an SO_RCVTIMEO / SO_SNDTIMEO timeout is WSAETIMEDOUT on
// Windows but EAGAIN on Linux, where callers treat it as "nothing yet, retry"
// (a timed-out recv on Windows 2000+ leaves the socket usable). Only then:
// on a socket without that timeout WSAETIMEDOUT is a dead connection
// (retransmission / keepalive gave up) - ETIMEDOUT, as on Linux.
inline void sock_set_io_errno(int fd, int timeout_opt) {
    const int e = WSAGetLastError();
    if (e == WSAETIMEDOUT) {
        DWORD ms = 0;
        int len = sizeof ms;
        if (getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, timeout_opt, reinterpret_cast<char*>(&ms), &len) == 0 &&
            ms != 0) {
            errno = EAGAIN;
            return;
        }
    }
    errno = sock_errno_from_wsa(e);
}

// Winsock must be started before any socket call - also gethostname() and
// getaddrinfo(). win/win_init.cpp does it at process start (before main);
// calling this again is harmless (reference counted).
inline bool sock_startup() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

inline int sock_socket(int domain, int type, int protocol) {
    const bool nonblock = (type & SOCK_NONBLOCK) != 0;
    type &= ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    SOCKET s = WSASocketW(domain, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        sock_set_errno();
        return -1;
    }
    if (nonblock) {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }
    return static_cast<int>(s);
}

inline int sock_close(int fd) {
    if (closesocket(static_cast<SOCKET>(fd)) != 0) {
        sock_set_errno();
        return -1;
    }
    return 0;
}

inline bool sock_set_nonblocking(int fd, bool on = true) {
    u_long v = on ? 1 : 0;
    if (ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &v) != 0) {
        sock_set_errno();
        return false;
    }
    return true;
}

inline int sock_accept4(int fd, sockaddr* addr, socklen_t* len, int flags) {
    SOCKET s = accept(static_cast<SOCKET>(fd), addr, len);
    if (s == INVALID_SOCKET) {
        sock_set_errno();
        return -1;
    }
    // an accepted socket takes the listening socket's attributes; keep it out
    // of child processes in any case, and blocking unless asked otherwise
    // (a non-blocking listener would otherwise hand out non-blocking sockets,
    // unlike accept() on Linux)
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    u_long nb = (flags & SOCK_NONBLOCK) ? 1 : 0;
    ioctlsocket(s, FIONBIO, &nb);
    return static_cast<int>(s);
}
inline int sock_accept(int fd, sockaddr* addr, socklen_t* len) { return sock_accept4(fd, addr, len, 0); }

inline int sock_bind(int fd, const sockaddr* addr, socklen_t len) {
    if (bind(static_cast<SOCKET>(fd), addr, len) != 0) {
        sock_set_errno();
        return -1;
    }
    return 0;
}

inline int sock_listen(int fd, int backlog) {
    if (listen(static_cast<SOCKET>(fd), backlog) != 0) {
        sock_set_errno();
        return -1;
    }
    return 0;
}

inline int sock_connect(int fd, const sockaddr* addr, socklen_t len) {
    if (connect(static_cast<SOCKET>(fd), addr, len) != 0) {
        const int e = WSAGetLastError();
        errno = e == WSAEWOULDBLOCK ? EINPROGRESS : sock_errno_from_wsa(e);
        return -1;
    }
    return 0;
}

inline int sock_shutdown(int fd, int how) {
    if (shutdown(static_cast<SOCKET>(fd), how) != 0) {
        sock_set_errno();
        return -1;
    }
    return 0;
}

inline ssize_t sock_send(int fd, const void* buf, size_t len, int flags) {
    flags &= ~(MSG_NOSIGNAL | MSG_DONTWAIT);
    const int n = send(static_cast<SOCKET>(fd), static_cast<const char*>(buf),
                       static_cast<int>(len > 0x7fffffff ? 0x7fffffff : len), flags);
    if (n == SOCKET_ERROR) {
        sock_set_io_errno(fd, SO_SNDTIMEO);
        return -1;
    }
    return n;
}

inline ssize_t sock_sendto(int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen) {
    flags &= ~(MSG_NOSIGNAL | MSG_DONTWAIT);
    const int n = sendto(static_cast<SOCKET>(fd), static_cast<const char*>(buf),
                         static_cast<int>(len > 0x7fffffff ? 0x7fffffff : len), flags, to, tolen);
    if (n == SOCKET_ERROR) {
        sock_set_io_errno(fd, SO_SNDTIMEO);
        return -1;
    }
    return n;
}

inline ssize_t sock_recv(int fd, void* buf, size_t len, int flags) {
    if (flags & MSG_DONTWAIT) {
        // readable now (data, EOF or an error)? otherwise EAGAIN, as a
        // non-blocking recv on Linux - without changing the socket's mode
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(static_cast<SOCKET>(fd), &rs);
        timeval tv{0, 0};
        const int r = select(0, &rs, nullptr, nullptr, &tv);
        if (r == SOCKET_ERROR) {
            sock_set_errno();
            return -1;
        }
        if (r == 0) {
            errno = EAGAIN;
            return -1;
        }
        flags &= ~MSG_DONTWAIT;
    }
    const int n = recv(static_cast<SOCKET>(fd), static_cast<char*>(buf),
                       static_cast<int>(len > 0x7fffffff ? 0x7fffffff : len), flags);
    if (n == SOCKET_ERROR) {
        sock_set_io_errno(fd, SO_RCVTIMEO);
        return -1;
    }
    return n;
}

inline ssize_t sock_recvfrom(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) {
    flags &= ~MSG_DONTWAIT;
    const int n = recvfrom(static_cast<SOCKET>(fd), static_cast<char*>(buf),
                           static_cast<int>(len > 0x7fffffff ? 0x7fffffff : len), flags, from, fromlen);
    if (n == SOCKET_ERROR) {
        sock_set_io_errno(fd, SO_RCVTIMEO);
        return -1;
    }
    return n;
}

inline int sock_setsockopt(int fd, int level, int name, const void* val, socklen_t len) {
    if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == sizeof(timeval)) {
        const timeval* tv = static_cast<const timeval*>(val);
        DWORD ms = static_cast<DWORD>(tv->tv_sec) * 1000u + static_cast<DWORD>(tv->tv_usec / 1000);
        if (ms == 0 && (tv->tv_sec != 0 || tv->tv_usec != 0)) ms = 1;   // < 1 ms is not "no timeout"
        if (setsockopt(static_cast<SOCKET>(fd), level, name, reinterpret_cast<const char*>(&ms), sizeof ms) != 0) {
            sock_set_errno();
            return -1;
        }
        return 0;
    }
    if (level == SOL_SOCKET && name == SO_REUSEADDR) {
        // see the header comment: exclusive, so a busy port fails to bind
        int on = (len >= static_cast<socklen_t>(sizeof(int)) && *static_cast<const int*>(val)) ? 1 : 0;
        if (!on) return 0;
        if (setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on),
                       sizeof on) != 0) {
            sock_set_errno();
            return -1;
        }
        return 0;
    }
    if (setsockopt(static_cast<SOCKET>(fd), level, name, static_cast<const char*>(val), len) != 0) {
        sock_set_errno();
        return -1;
    }
    return 0;
}

inline int sock_getsockopt(int fd, int level, int name, void* val, socklen_t* len) {
    if (getsockopt(static_cast<SOCKET>(fd), level, name, static_cast<char*>(val), len) != 0) {
        sock_set_errno();
        return -1;
    }
    if (level == SOL_SOCKET && name == SO_ERROR && *len >= static_cast<socklen_t>(sizeof(int))) {
        int* e = static_cast<int*>(val);
        if (*e) *e = sock_errno_from_wsa(*e);   // a WSA code -> its errno value
    }
    return 0;
}

// poll() on sockets only (WSAPoll; POLLPRI is not supported)
inline int sock_poll(pollfd* fds, unsigned long n, int timeout_ms) {
    const int r = WSAPoll(fds, n, timeout_ms);
    if (r == SOCKET_ERROR) {
        sock_set_errno();
        return -1;
    }
    return r;
}

// select() on sockets only (nfds is ignored by Winsock)
inline int sock_select(int nfds, fd_set* r, fd_set* w, fd_set* e, timeval* tv) {
    (void)nfds;
    if ((!r || r->fd_count == 0) && (!w || w->fd_count == 0) && (!e || e->fd_count == 0)) {
        // Winsock fails select() with no sockets at all; POSIX just waits
        if (tv) Sleep(static_cast<DWORD>(tv->tv_sec * 1000 + tv->tv_usec / 1000));
        return 0;
    }
    const int n = select(0, r, w, e, tv);
    if (n == SOCKET_ERROR) {
        sock_set_errno();
        return -1;
    }
    return n;
}

// TCP keepalive: probe after idle_s idle seconds, every interval_s, give up
// after count probes. Windows 10 1709+ has the Linux options (seconds); older
// builds only SIO_KEEPALIVE_VALS, whose probe count is fixed (10).
inline bool sock_set_keepalive(int fd, int idle_s, int interval_s, int count) {
    const SOCKET s = static_cast<SOCKET>(fd);
    DWORD on = 1, idle = static_cast<DWORD>(idle_s), intvl = static_cast<DWORD>(interval_s), cnt = static_cast<DWORD>(count);
    if (setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof on) != 0) return false;
    if (setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<const char*>(&idle), sizeof idle) == 0 &&
        setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<const char*>(&intvl), sizeof intvl) == 0 &&
        setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char*>(&cnt), sizeof cnt) == 0)
        return true;
    tcp_keepalive ka{1, idle * 1000u, intvl * 1000u};
    DWORD ret = 0;
    return WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, nullptr, 0, &ret, nullptr, nullptr) == 0;
}

// connect() with a time limit: 0, or -1 with errno (ETIMEDOUT when the
// limit passed). The socket is left blocking. Waits with select() on the
// write AND except sets: WSAPoll missed a refused connect before Windows 10
// 2004.
inline int sock_connect_timeout(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
    const SOCKET s = static_cast<SOCKET>(fd);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = 0;
    if (connect(s, addr, len) != 0) {
        const int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) {
            errno = sock_errno_from_wsa(e);
            rc = -1;
        } else {
            fd_set w, x;
            FD_ZERO(&w);
            FD_ZERO(&x);
            FD_SET(s, &w);
            FD_SET(s, &x);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            const int n = select(0, nullptr, &w, &x, &tv);
            if (n == 0) {
                errno = ETIMEDOUT;
                rc = -1;
            } else if (n < 0) {
                sock_set_errno();
                rc = -1;
            } else {
                int err = 0;
                int el = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el);
                if (err || FD_ISSET(s, &x)) {
                    errno = err ? sock_errno_from_wsa(err) : ECONNREFUSED;
                    rc = -1;
                }
            }
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return rc;
}

// For a connect to 127.0.0.1 / ::1 only: a refused connect fails at once
// instead of after Windows' ~2 s of SYN retries (a client polling for a
// server that isn't up yet). Before connect(); no-op on Linux.
inline void sock_loopback_fast_refuse(int fd) {
    TCP_INITIAL_RTO_PARAMETERS p{TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
    DWORD ret = 0;
    WSAIoctl(static_cast<SOCKET>(fd), SIO_TCP_INITIAL_RTO, &p, sizeof p, nullptr, 0, &ret, nullptr, nullptr);
}

// Can one more socket go into an fd_set? On Linux an fd_set is a bitmap, so
// the fd VALUE must be < FD_SETSIZE; on Windows it is a list of up to
// FD_SETSIZE sockets of any value, so it is the COUNT already in the set.
inline bool sock_fits_fd_set(int fd, int sockets_in_set) {
    (void)fd;
    return sockets_in_set < FD_SETSIZE;
}

#else  // POSIX

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
// macOS: no SOCK_CLOEXEC / SOCK_NONBLOCK type flags, no accept4(), no
// MSG_NOSIGNAL - the flags are interpreted (and removed) by sock_socket /
// sock_accept4 as on Windows, and every socket made there gets SO_NOSIGPIPE
// (a send to a closed peer fails with EPIPE instead of killing the process).
// TCP_KEEPALIVE is macOS's name for TCP_KEEPIDLE.
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x02000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x04000000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
// accepted = from accept(): BSD hands it the listener's O_NONBLOCK, Linux
// doesn't - cleared unless SOCK_NONBLOCK asks for it
inline int sock_apply_flags(int fd, int flags, bool accepted = false) {
    if (fd < 0) return fd;
    if (flags & SOCK_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if ((flags & SOCK_NONBLOCK) || accepted) {
        const int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, (flags & SOCK_NONBLOCK) ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    return fd;
}
#endif

inline bool sock_startup() { return true; }
#ifdef __APPLE__
inline int sock_socket(int domain, int type, int protocol) {
    const int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    return sock_apply_flags(::socket(domain, type & ~flags, protocol), flags);
}
#else
inline int sock_socket(int domain, int type, int protocol) { return ::socket(domain, type, protocol); }
#endif
inline int sock_close(int fd) { return ::close(fd); }
inline bool sock_set_nonblocking(int fd, bool on = true) {
    const int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK)) >= 0;
}
#ifdef __APPLE__
inline int sock_accept(int fd, sockaddr* addr, socklen_t* len) { return sock_apply_flags(::accept(fd, addr, len), 0, true); }
inline int sock_accept4(int fd, sockaddr* addr, socklen_t* len, int flags) {
    return sock_apply_flags(::accept(fd, addr, len), flags, true);
}
#else
inline int sock_accept(int fd, sockaddr* addr, socklen_t* len) { return ::accept(fd, addr, len); }
inline int sock_accept4(int fd, sockaddr* addr, socklen_t* len, int flags) { return ::accept4(fd, addr, len, flags); }
#endif
inline int sock_bind(int fd, const sockaddr* addr, socklen_t len) { return ::bind(fd, addr, len); }
inline int sock_listen(int fd, int backlog) { return ::listen(fd, backlog); }
inline int sock_connect(int fd, const sockaddr* addr, socklen_t len) { return ::connect(fd, addr, len); }
inline int sock_shutdown(int fd, int how) { return ::shutdown(fd, how); }
inline ssize_t sock_send(int fd, const void* buf, size_t len, int flags) { return ::send(fd, buf, len, flags); }
inline ssize_t sock_sendto(int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen) {
    return ::sendto(fd, buf, len, flags, to, tolen);
}
inline ssize_t sock_recv(int fd, void* buf, size_t len, int flags) { return ::recv(fd, buf, len, flags); }
inline ssize_t sock_recvfrom(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) {
    return ::recvfrom(fd, buf, len, flags, from, fromlen);
}
inline int sock_setsockopt(int fd, int level, int name, const void* val, socklen_t len) {
    return ::setsockopt(fd, level, name, val, len);
}
inline int sock_getsockopt(int fd, int level, int name, void* val, socklen_t* len) {
    return ::getsockopt(fd, level, name, val, len);
}
inline int sock_poll(pollfd* fds, unsigned long n, int timeout_ms) {
    return ::poll(fds, static_cast<nfds_t>(n), timeout_ms);
}
inline int sock_select(int nfds, fd_set* r, fd_set* w, fd_set* e, timeval* tv) { return ::select(nfds, r, w, e, tv); }
inline bool sock_set_keepalive(int fd, int idle_s, int interval_s, int count) {
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_s, sizeof idle_s) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_s, sizeof interval_s) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count) == 0;
}
inline int sock_connect_timeout(int fd, const sockaddr* addr, socklen_t len, int timeout_ms) {
    const int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = 0;
    if (::connect(fd, addr, len) != 0) {
        if (errno != EINPROGRESS) {
            rc = -1;
        } else {
            pollfd p{fd, POLLOUT, 0};
            const int n = ::poll(&p, 1, timeout_ms);
            if (n == 0) {
                errno = ETIMEDOUT;
                rc = -1;
            } else if (n < 0) {
                rc = -1;
            } else {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) {
                    errno = err;
                    rc = -1;
                }
            }
        }
    }
    const int e = errno;
    fcntl(fd, F_SETFL, fl);
    errno = e;
    return rc;
}
inline void sock_loopback_fast_refuse(int) {}
inline bool sock_fits_fd_set(int fd, int sockets_in_set) {
    (void)sockets_in_set;
    return fd < FD_SETSIZE;
}

#endif
