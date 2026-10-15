#include "networking/tcp_client.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <cerrno>

TCPClient::~TCPClient() { 
    disconnect(); 
}

bool TCPClient::connect_to_port(int port, const std::string& host) {
    if (connected) return true;
    
    socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return false;
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    
    if (connect(socket_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(socket_fd);
        socket_fd = -1;
        return false;
    }

    // Bound blocking recv() to 500 ms so receive loops can observe the
    // global `running` flag and shut down promptly (recv returns -1/EAGAIN
    // on timeout; callers treat that as "no data yet", not disconnect).
    timeval tv{};
    tv.tv_usec = 500 * 1000;
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    connected = true;
    return true;
}

void TCPClient::disconnect() {
    if (socket_fd >= 0) {
        close(socket_fd);
        socket_fd = -1;
    }
    connected = false;
}

bool TCPClient::send_data(const void* data, size_t size) {
    if (!connected) return false;

    // Loop until the whole message is written: on a non-blocking socket a
    // short send() or EAGAIN is normal when the buffer is momentarily full,
    // not a connection failure. Bailing mid-message would leave the server
    // with a truncated command.
    const char* p = static_cast<const char*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t sent = send(socket_fd, p, remaining, MSG_NOSIGNAL);
        if (sent > 0) {
            p += sent;
            remaining -= static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Wait briefly for writability instead of dropping the command
            pollfd pfd{socket_fd, POLLOUT, 0};
            if (poll(&pfd, 1, 200) > 0) {
                continue;
            }
        }
        disconnect();
        return false;
    }
    return true;
}

ssize_t TCPClient::receive_data(void* buffer, size_t size) {
    if (!connected) return -1;
    return recv(socket_fd, buffer, size, 0);
}

void TCPClient::set_nonblocking(bool nonblock) {
    if (socket_fd >= 0) {
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (nonblock) {
            fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
        } else {
            fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
        }
    }
}

bool TCPClient::is_connected() const { 
    return connected; 
}

int TCPClient::get_socket() const { 
    return socket_fd; 
}
