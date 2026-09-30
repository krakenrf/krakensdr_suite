#pragma once

#include "../core/types.hpp"
#include "tcp_common.hpp"
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>

class RtlTcpServer; // Forward declaration

class TcpControlServer {
private:
    int server_socket;
    std::vector<std::unique_ptr<TcpClient>> clients;
    std::mutex clients_mutex;
    std::thread server_thread;
    std::atomic<bool> running{true};
    RtlTcpServer* rtl_tcp_server_ref = nullptr;
    
    void control_loop();
    void handle_client_data(TcpClient* client);
    std::string process_command(const std::string& json_str);
    
public:
    TcpControlServer();
    ~TcpControlServer();
    
    bool start();
    void stop();
    void broadcast_status();
    std::string build_status_json(bool as_reply);
    void set_rtl_tcp_server(RtlTcpServer* server) { rtl_tcp_server_ref = server; }

    // Live connected-client count for the status dashboard.
    size_t client_count() { std::lock_guard<std::mutex> lk(clients_mutex); return clients.size(); }
};
