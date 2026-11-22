// A connection's byte counters from the kernel (NET_STATS:, docs/dev/spectrum-views.md
// "Network rate"). Linux only (TCP_INFO); elsewhere there is no counter.
#pragma once

#include <cstdint>

namespace net_stats {

struct Counters {
    uint64_t tx_acked = 0;   // tcpi_bytes_acked: sent and acknowledged by the peer
    uint64_t tx_sent = 0;    // tcpi_bytes_sent: sent incl. retransmissions (has_sent)
    uint64_t rx = 0;         // tcpi_bytes_received
    bool has_sent = false;   // the running kernel fills tcpi_bytes_sent (4.19+)
};

// false = no counter on this platform / socket (the page then counts by itself)
bool read(int fd, Counters& out);

}  // namespace net_stats
