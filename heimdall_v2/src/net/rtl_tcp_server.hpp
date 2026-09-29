#pragma once

#include "../core/types.hpp"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class RtlTcpServer {
private:
    int server_socket, client_socket;
    std::thread server_thread, worker_thread;
    std::atomic<bool> running{true};
    // The streamed channel is the global rtl_tcp_channel (types.hpp): one
    // store, so every path that selects a channel (web UI, control port,
    // element-count change) reaches the stream. A private copy here was only
    // updated by some of them - the web UI's picker never took effect.
    
    // Simple linked list buffer structure (like original rtl_tcp.c)
    struct BufferNode {
        std::vector<uint8_t> data;
        BufferNode* next;
        BufferNode(const uint8_t* buf, size_t len);
    };
    
    BufferNode* buffer_head;
    BufferNode* buffer_tail;
    std::mutex buffer_mutex;
    std::condition_variable buffer_cv;
    std::atomic<int> buffer_count{0};
    static constexpr int MAX_BUFFER_COUNT = 500;
    
    void server_loop();
    void worker_loop();
    void add_to_buffer(const uint8_t* data, size_t len);
    void clear_buffers();
    void send_dongle_info();
    
public:
    RtlTcpServer(int channel = 0);
    ~RtlTcpServer();
    
    bool start();
    void stop();
    void set_source_channel(int channel);
    int get_source_channel() const;
    void broadcast_data(const std::vector<ComplexBuffer>& channel_data);

    // Whether a client is currently connected (single-client server), for the
    // status dashboard. Benign racy read of the socket fd — display only.
    bool has_client() const { return client_socket >= 0; }
};
