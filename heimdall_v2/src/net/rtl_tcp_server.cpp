#include "rtl_tcp_server.hpp"
#include "tcp_common.hpp"  // For socket headers and utilities
#include "../sdr/sdr_device.hpp"  // Need full SDRDevice definition
#include "../core/config.hpp"
#include "../core/utils.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>  // For memcpy

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
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) return false;
    
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(RTL_TCP_PORT);
    
    if (bind(server_socket, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_socket, 1) < 0) {
        close(server_socket);
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
        close(server_socket);
        server_socket = -1;
    }
    if (client_socket >= 0) {
        close(client_socket);
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
    
    // Tuner type as uint32_t big-endian (1 = E4000)
    uint32_t tuner_type = htonl(1);
    memcpy(rtl_tcp_header + 4, &tuner_type, 4);
    
    // Tuner gain count as uint32_t big-endian
    uint32_t gain_count = htonl(29);
    memcpy(rtl_tcp_header + 8, &gain_count, 4);
    
    send(client_socket, rtl_tcp_header, 12, MSG_NOSIGNAL);
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
    std::vector<uint8_t> rtl_tcp_data;
    rtl_tcp_data.reserve(samples.size() * 2);
    
    // Use conservative scaling to prevent overloading
    constexpr float scale_factor = 80.0f;
    
    for (const auto& sample : samples) {
        // Clamp and convert to uint8 (0-255, 127.5 = zero)
        int i_val = static_cast<int>(sample.real() * scale_factor + 127.5f);
        int q_val = static_cast<int>(sample.imag() * scale_factor + 127.5f);
        
        rtl_tcp_data.push_back(static_cast<uint8_t>(clamp(i_val, 0, 255)));
        rtl_tcp_data.push_back(static_cast<uint8_t>(clamp(q_val, 0, 255)));
    }
    
    // Add to buffer queue (like original rtl_tcp.c callback)
    add_to_buffer(rtl_tcp_data.data(), rtl_tcp_data.size());
}

void RtlTcpServer::server_loop() {
    while (running) {
        std::cout << "RTL-TCP: Waiting for client connection..." << std::endl;
        
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
        
        if (client_socket < 0) {
            if (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        
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
        
        close(client_socket);
        client_socket = -1;
        clear_buffers();
        
        std::cout << "RTL-TCP: Client disconnected" << std::endl;
    }
}

void RtlTcpServer::worker_loop() {
    fd_set writefds;
    timeval tv;
    
    while (running && client_socket >= 0) {
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
        
        while (bytes_left > 0 && running && client_socket >= 0) {
            // Use select to check if socket is ready for writing
            FD_ZERO(&writefds);
            FD_SET(client_socket, &writefds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int r = select(client_socket + 1, nullptr, &writefds, nullptr, &tv);
            if (r > 0 && FD_ISSET(client_socket, &writefds)) {
                ssize_t bytes_sent = send(client_socket, 
                                        current_buffer->data.data() + index, 
                                        bytes_left, MSG_NOSIGNAL);
                
                if (bytes_sent <= 0) {
                    std::cout << "RTL-TCP: Send failed, client disconnected" << std::endl;
                    delete current_buffer;
                    return;
                }
                
                bytes_left -= bytes_sent;
                index += bytes_sent;
            } else if (r == 0) {
                // Timeout - continue trying (don't disconnect immediately like original)
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