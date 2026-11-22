// A connection's byte counters from the kernel (NET_STATS:). Linux only: TCP_INFO
// on the socket's descriptor (<linux/tcp.h> here alone - glibc's <netinet/tcp.h>
// may lack the newer fields and the two headers don't mix). Elsewhere: no counter.
#include "networking/net_stats.hpp"

#if defined(__linux__)
#include <cstddef>
#include <cstring>
#include <linux/tcp.h>
#include <netinet/in.h>   // IPPROTO_TCP (not <netinet/tcp.h>: it clashes with <linux/tcp.h>)
#include <sys/socket.h>
#endif

namespace net_stats {

#if defined(__linux__)
bool read(int fd, Counters& out) {
    if (fd < 0) return false;
    struct tcp_info ti;
    std::memset(&ti, 0, sizeof ti);
    socklen_t len = sizeof ti;
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &len) != 0) return false;
    // the running kernel fills as much of the struct as it knows: a field past
    // what it returned is not there (bytes_acked / received 4.1+, bytes_sent 4.19+)
    auto has = [len](size_t off, size_t size) { return static_cast<size_t>(len) >= off + size; };
    if (!has(offsetof(struct tcp_info, tcpi_bytes_received), sizeof ti.tcpi_bytes_received)) return false;
    out.tx_acked = ti.tcpi_bytes_acked;
    out.rx = ti.tcpi_bytes_received;
    out.has_sent = has(offsetof(struct tcp_info, tcpi_bytes_sent), sizeof ti.tcpi_bytes_sent);
    out.tx_sent = out.has_sent ? ti.tcpi_bytes_sent : 0;
    return true;
}
#else
bool read(int, Counters&) { return false; }
#endif

}  // namespace net_stats
