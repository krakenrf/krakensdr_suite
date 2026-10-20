#include "networking/tcp_client.hpp"
#include "utils/net_compat.hpp"   // sockets: BSD on Linux, Winsock on Windows
#include <cerrno>

TCPClient::~TCPClient() {
    disconnect();
}

bool TCPClient::connect_to_port(int port, const std::string& host) {
    if (connected) return true;

    socket_fd = sock_socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());

    // heimdall on this machine (127.x): Windows retries the SYN of a refused
    // connect for ~2 s - while heimdall is down that froze the web UI for
    // ~4 s per browser command (send_control_command runs on the uWS loop
    // thread and tries twice). Refused at once, as on Linux (no-op there).
    if ((ntohl(addr.sin_addr.s_addr) >> 24) == 127) sock_loopback_fast_refuse(socket_fd);

    if (sock_connect(socket_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        sock_close(socket_fd);
        socket_fd = -1;
        return false;
    }

    // Bound blocking recv() to 500 ms so receive loops can observe the
    // global `running` flag and shut down promptly (recv returns -1/EAGAIN
    // on timeout; callers treat that as "no data yet", not disconnect).
    timeval tv{};
    tv.tv_usec = 500 * 1000;
    sock_setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef _WIN32
    // Control commands are small and sent one by one (a spectrum drag: ~20/s):
    // with Nagle each waited for the ACK of the previous one, which Windows
    // delays up to ~200 ms
    const int nodelay = 1;
    sock_setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#endif

    connected = true;
    return true;
}

void TCPClient::disconnect() {
    if (socket_fd >= 0) {
        sock_close(socket_fd);
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
        ssize_t sent = sock_send(socket_fd, p, remaining, MSG_NOSIGNAL);
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
            pollfd pfd{};   // field-wise: pollfd::fd is a SOCKET on Windows
            pfd.fd = socket_fd;
            pfd.events = POLLOUT;
            if (sock_poll(&pfd, 1, 200) > 0) {
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
    const ssize_t n = sock_recv(socket_fd, buffer, size, 0);
#ifdef _WIN32
    // Windows resets (RST) the connections of a process that ends, where
    // Linux closes them (FIN): heimdall exiting / crashing is ECONNRESET
    // here, EOF there. Reported as EOF, so the callers drop the connection
    // as on Linux (the control-socket drain only does on 0).
    if (n < 0 && (errno == ECONNRESET || errno == ECONNABORTED)) return 0;
#endif
    return n;
}

void TCPClient::set_nonblocking(bool nonblock) {
    if (socket_fd >= 0) {
        sock_set_nonblocking(socket_fd, nonblock);
    }
}

bool TCPClient::is_connected() const { 
    return connected; 
}

int TCPClient::get_socket() const { 
    return socket_fd; 
}
