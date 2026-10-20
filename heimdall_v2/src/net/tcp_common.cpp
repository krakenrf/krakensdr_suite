#include "tcp_common.hpp"
#include <cstring>

TcpClient::TcpClient(int s) : socket(s), active(true) {}

TcpClient::~TcpClient() { 
    if (socket >= 0) sock_close(socket);
}

bool set_socket_nonblocking(int socket) {
    return sock_set_nonblocking(socket);
}

// SO_REUSEADDR: a restart can bind at once although connections of the last
// run are still in TIME_WAIT. On Windows the same option would let a second
// process bind the busy port too (each connection then lands on either one),
// so sock_setsockopt makes it SO_EXCLUSIVEADDRUSE there: bind() still fails
// on a port that is in use, which heimdall relies on (busy 8091/8092 = exit 1).
bool set_socket_reuse(int socket) {
    int opt = 1;
    return sock_setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) >= 0;
}

std::string listen_error_detail(int err) {
#ifdef _WIN32
    if (err == 0) return std::string();
    std::string s = std::string(" (") + std::strerror(err);
    if (err == EACCES) {
        s += " - the port may be reserved by Windows, see"
             " 'netsh interface ipv4 show excludedportrange protocol=tcp'";
    }
    return s + ")";
#else
    (void)err;
    return std::string();
#endif
}
