#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
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