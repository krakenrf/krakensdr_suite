#pragma once

// BSD sockets on Linux, Winsock on Windows. Every socket call of the servers
// goes through the sock_* functions there (the plain BSD calls on Linux):
// on Windows close() on a socket silently does nothing, and Winsock never
// sets errno, which the back-pressure checks (EAGAIN) depend on.
#include "../core/net_compat.hpp"
#include <errno.h>
#include <vector>
#include <cstdint>
#include <string>

// Simple TCP Client structure
struct TcpClient {
    int socket;
    bool active;
    std::string rx_buffer;  // partial-command accumulator (control server)
    std::string tx_buffer;  // pending output, flushed when the socket is writable
                            // (control server; the data server keeps the unsent
                            // tail of its in-flight packet here). Never block a
                            // server thread on a client that has stopped reading.

    TcpClient(int s);
    ~TcpClient();
};

// Socket helper functions
bool set_socket_nonblocking(int socket);
bool set_socket_reuse(int socket);

// Why a listening socket couldn't be set up, for the caller's error message,
// from the errno of the failed bind()/listen() (start() keeps it): "" on
// Linux, where a busy port is the realistic cause and the messages say so.
// Windows: " (<reason>)" - a port inside an excluded port range (Hyper-V,
// WSL2 and Docker reserve blocks of them) fails as "access denied", not as
// "in use", and gets a hint how to see those ranges.
std::string listen_error_detail(int err);
