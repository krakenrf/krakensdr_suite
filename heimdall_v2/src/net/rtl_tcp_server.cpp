#include "rtl_tcp_server.hpp"
#include "tcp_common.hpp"  // For socket headers and utilities
#include "../sdr/sdr_device.hpp"  // Need full SDRDevice definition
#include "../core/config.hpp"
#include "../core/utils.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>  // For memcpy
#include <cmath>    // std::trunc

RtlTcpServer::BufferNode::BufferNode(const uint8_t* buf, size_t len) : data(buf, buf + len), next(nullptr) {}

RtlTcpServer::RtlTcpServer(int channel) : server_socket(-1), client_socket(-1),
                                buffer_head(nullptr), buffer_tail(nullptr) {
    rtl_tcp_channel.store(channel);
}

RtlTcpServer::~RtlTcpServer() {
    stop();
}

void RtlTcpServer::set_source_channel(int channel) {
    const int num_active = active_num_elements.load();
    if (channel >= 0 && channel < num_active) {
        int old_channel = rtl_tcp_channel.exchange(channel);
        if (old_channel != channel) {
            std::cout << "RTL-TCP: Changed source channel from " << old_channel
                 << " to " << channel << " (Serial: "
                 << (channel < static_cast<int>(devices.size()) ? devices[channel]->serial_number : "Unknown")
                 << ")" << std::endl;
        }
    } else {
        std::cerr << "RTL-TCP: Invalid channel " << channel
             << " (must be 0-" << (num_active - 1) << ")" << std::endl;
    }
}

int RtlTcpServer::get_source_channel() const {
    return rtl_tcp_channel.load();
}

bool RtlTcpServer::start() {
    server_socket = sock_socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) return false;
    
    // Exclusive on Windows (sock_setsockopt, see set_socket_reuse): there a
    // plain SO_REUSEADDR co-bound 1234 with a stock rtl_tcp.exe (which sets
    // it too) instead of failing, and SDR# connections landed on either one
    int opt = 1;
    sock_setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RTL_TCP_PORT);
    
    if (sock_bind(server_socket, (sockaddr*)&addr, sizeof(addr)) < 0 || sock_listen(server_socket, 1) < 0) {
        const int err = errno;  // for main's message (listen_error_detail)
        sock_close(server_socket);
        server_socket = -1;  // stop() (the destructor) must not close the number again
        errno = err;
        return false;
    }
    
    server_thread = std::thread(&RtlTcpServer::server_loop, this);
    
    std::cout << "RTL-TCP Server listening on port " << RTL_TCP_PORT 
         << " (streaming channel " << rtl_tcp_channel.load() << ")" << std::endl;
    return true;
}

void RtlTcpServer::stop() {
    running = false;
    if (server_socket >= 0) {
        // Linux: the shutdown wakes the accept(); close() alone doesn't.
        // Windows: the shutdown fails (not connected) and it is the
        // closesocket() that wakes it.
        sock_shutdown(server_socket, SHUT_RDWR);
        sock_close(server_socket);
        server_socket = -1;
    }
    if (client_socket >= 0) {
        sock_close(client_socket);
        client_socket = -1;
    }
    
    buffer_cv.notify_all();
    
    if (server_thread.joinable()) server_thread.join();
    if (worker_thread.joinable()) worker_thread.join();
    
    clear_buffers();
}

void RtlTcpServer::send_dongle_info() {
    // Send proper RTL-TCP dongle info header (12 bytes)
    uint8_t rtl_tcp_header[12];
    memcpy(rtl_tcp_header, "RTL0", 4);  // Magic
    
    // Tuner type as uint32_t big-endian: 5 = R820T (RTLSDR_TUNER_R820T), what
    // the KrakenSDR actually has - it used to claim an E4000 (1), whose gain
    // table clients then offered
    uint32_t tuner_type = htonl(5);
    memcpy(rtl_tcp_header + 4, &tuner_type, 4);
    
    // Tuner gain count as uint32_t big-endian (the R820T's 29 steps)
    uint32_t gain_count = htonl(29);
    memcpy(rtl_tcp_header + 8, &gain_count, 4);
    
    sock_send(client_socket, rtl_tcp_header, 12, MSG_NOSIGNAL);
}

void RtlTcpServer::broadcast_data(const std::vector<ComplexBuffer>& channel_data) {
    if (client_socket < 0 || channel_data.empty()) return;
    
    int current_source = rtl_tcp_channel.load();
    if (current_source < 0 || static_cast<size_t>(current_source) >= channel_data.size()) {
        return;
    }
    
    // Get samples for the current source channel
    const ComplexBuffer& samples = channel_data[current_source];
    if (samples.empty()) return;
    
    // Convert complex float to rtl_tcp uint8 IQ format
    std::vector<uint8_t> rtl_tcp_data(samples.size() * 2);

    // Use conservative scaling to prevent overloading
    constexpr float scale_factor = 80.0f;

    // Clamp and convert to uint8 (0-255, 127.5 = zero). Rounded, not
    // truncated: the int cast was a -0.5 LSB DC bias on the stream. I and Q
    // alike over the interleaved floats, into space sized once, as on 8091
    // (lroundf + a push_back per byte kept it scalar). Half away from zero
    // for v >= 0 (below 0 clamps to 0 either way): the same byte as
    // the old clamp((int)lroundf(v), 0, 255) for every non-NaN v in int's
    // range (checked over every float input; beyond it the old cast wrapped,
    // this clamps).
    const size_t n = rtl_tcp_data.size();
    const float* __restrict src = reinterpret_cast<const float*>(samples.data());
    uint8_t* __restrict dst = rtl_tcp_data.data();
    for (size_t k = 0; k < n; k++) {
        const float v = src[k] * scale_factor + 127.5f;
        const float t = std::trunc(v);
        const float r = (v - t >= 0.5f) ? t + 1.0f : t;
        dst[k] = static_cast<uint8_t>(static_cast<int>(clamp(r, 0.0f, 255.0f)));
    }
    
    // Add to buffer queue (like original rtl_tcp.c callback)
    add_to_buffer(rtl_tcp_data.data(), rtl_tcp_data.size());
}

void RtlTcpServer::server_loop() {
    while (running) {
        std::cout << "RTL-TCP: Waiting for client connection..." << std::endl;
        
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        // sock_accept: on Windows also not inherited by child processes
        client_socket = sock_accept(server_socket, (sockaddr*)&client_addr, &addr_len);
        
        if (client_socket < 0) {
            if (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        
        // Keepalive: a client that vanished without a FIN (powered off, Wi-Fi
        // dropped) held the single slot until the kernel gave up on the
        // send - 15-30 min. Now it's found within ~25 s (10 s idle, 3 probes
        // 5 s apart; Windows 10 before 1709 can't set the probe count: ~60 s).
        sock_set_keepalive(client_socket, 10, 5, 3);
        ignored_cmds_logged = 0;
        cmd_carry_len = 0;

        std::cout << "RTL-TCP: Client connected from " << inet_ntoa(client_addr.sin_addr) 
             << ":" << ntohs(client_addr.sin_port) 
             << " (streaming channel " << rtl_tcp_channel.load() << ")" << std::endl;
        
        // Send dongle info immediately
        send_dongle_info();
        
        // Start worker thread for this client
        worker_thread = std::thread(&RtlTcpServer::worker_loop, this);
        
        // Wait for worker to finish (client disconnected)
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
        
        sock_close(client_socket);
        client_socket = -1;
        clear_buffers();
        
        std::cout << "RTL-TCP: Client disconnected" << std::endl;
    }
}

// rtl_tcp clients send 5-byte commands (1 byte id + uint32 BE value). This
// port is a read-only tap of ONE channel of a coherent array: frequency, gain
// and sample rate belong to heimdall (web UI / control port), and SDR#/GQRX
// send their own on connect - applying them would retune the whole DF array
// and force a recalibration. So they're read (a client's close is noticed
// here, instead of only by a failed send) and logged once per type.
bool RtlTcpServer::read_client_commands() {
    // Bounded: a client flooding commands mustn't starve its own stream
    for (int reads = 0; reads < 8; reads++) {
        uint8_t buf[5 + 510];  // carry-over + a whole number of commands
        std::memcpy(buf, cmd_carry, cmd_carry_len);
        // MSG_DONTWAIT: the socket stays blocking for the sends below
        // (Winsock has no such flag - sock_recv checks readability first)
        const ssize_t n = sock_recv(client_socket, buf + cmd_carry_len, 510, MSG_DONTWAIT);
        if (n == 0) return false;  // orderly close
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        const size_t total = cmd_carry_len + static_cast<size_t>(n);
        const size_t whole = total - total % 5;
        // A command split across reads is completed by the next one
        cmd_carry_len = total - whole;
        std::memcpy(cmd_carry, buf + whole, cmd_carry_len);
        for (size_t i = 0; i < whole; i += 5) {
            const uint8_t cmd = buf[i];
            const uint32_t val = (uint32_t(buf[i + 1]) << 24) | (uint32_t(buf[i + 2]) << 16) |
                                 (uint32_t(buf[i + 3]) << 8) | buf[i + 4];
            if (cmd >= 32 || (ignored_cmds_logged & (1u << cmd))) continue;
            ignored_cmds_logged |= 1u << cmd;
            const char* what = cmd == 1 ? "set frequency" : cmd == 2 ? "set sample rate"
                             : cmd == 3 ? "set gain mode" : cmd == 4 ? "set gain"
                             : cmd == 5 ? "set frequency correction" : "command";
            std::cout << "RTL-TCP: client " << what << " (" << int(cmd) << ", value " << val
                      << ") ignored - this port only streams; tune the array from the web UI" << std::endl;
            if (cmd == 2 && val != static_cast<uint32_t>(SAMPLE_RATE)) {
                std::cerr << "RTL-TCP: client asked for " << val << " S/s, the stream is "
                          << static_cast<uint32_t>(SAMPLE_RATE) << " S/s - set the client to match" << std::endl;
            }
        }
    }
    return true;  // still connected; the rest is read on the next call
}

void RtlTcpServer::worker_loop() {
    fd_set writefds;
    timeval tv;
    
    while (running && client_socket >= 0) {
        if (!read_client_commands()) {
            std::cout << "RTL-TCP: client closed the connection" << std::endl;
            return;
        }

        BufferNode* current_buffer = nullptr;
        
        // Wait for data (like original rtl_tcp.c)
        {
            std::unique_lock<std::mutex> lock(buffer_mutex);
            if (!buffer_head) {
                if (buffer_cv.wait_for(lock, std::chrono::seconds(1)) == std::cv_status::timeout) {
                    continue;
                }
            }
            
            if (buffer_head) {
                current_buffer = buffer_head;
                buffer_head = buffer_head->next;
                if (!buffer_head) {
                    buffer_tail = nullptr;
                }
                buffer_count--;
            }
        }
        
        if (!current_buffer) continue;
        
        // Send data
        size_t bytes_left = current_buffer->data.size();
        size_t index = 0;
        auto last_progress = std::chrono::steady_clock::now();
        
        while (bytes_left > 0 && running && client_socket >= 0) {
            // Use select to check if socket is ready for writing
            FD_ZERO(&writefds);
            FD_SET(client_socket, &writefds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int r = sock_select(client_socket + 1, nullptr, &writefds, nullptr, &tv);
            if (r > 0 && FD_ISSET(client_socket, &writefds)) {
                ssize_t bytes_sent = sock_send(client_socket,
                                        current_buffer->data.data() + index, 
                                        bytes_left, MSG_NOSIGNAL);
                
                if (bytes_sent <= 0) {
                    std::cout << "RTL-TCP: Send failed, client disconnected" << std::endl;
                    delete current_buffer;
                    return;
                }
                
                bytes_left -= bytes_sent;
                index += bytes_sent;
                last_progress = std::chrono::steady_clock::now();
            } else if (r == 0) {
                // Timeout - keep trying a while (don't disconnect immediately
                // like the original), but a client that hasn't read anything
                // for 10 s is gone or wedged: free the single slot
                if (std::chrono::steady_clock::now() - last_progress > std::chrono::seconds(10)) {
                    std::cout << "RTL-TCP: client stopped reading for 10 s, disconnecting" << std::endl;
                    delete current_buffer;
                    return;
                }
                continue;
            } else {
                // Error
                std::cout << "RTL-TCP: Select error, client disconnected" << std::endl;
                delete current_buffer;
                return;
            }
        }
        
        delete current_buffer;
    }
}

void RtlTcpServer::add_to_buffer(const uint8_t* data, size_t len) {
    auto* new_node = new BufferNode(data, len);
    
    std::lock_guard<std::mutex> lock(buffer_mutex);
    
    // If buffer is full, drop oldest (like original rtl_tcp.c)
    if (buffer_count >= MAX_BUFFER_COUNT && buffer_head) {
        auto* old_head = buffer_head;
        buffer_head = buffer_head->next;
        delete old_head;
        buffer_count--;
        
        if (!buffer_head) {
            buffer_tail = nullptr;
        }
    }
    
    // Add new buffer to tail
    if (buffer_tail) {
        buffer_tail->next = new_node;
    } else {
        buffer_head = new_node;
    }
    buffer_tail = new_node;
    buffer_count++;
    
    buffer_cv.notify_one();
}

void RtlTcpServer::clear_buffers() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    while (buffer_head) {
        auto* next = buffer_head->next;
        delete buffer_head;
        buffer_head = next;
    }
    buffer_tail = nullptr;
    buffer_count = 0;
}