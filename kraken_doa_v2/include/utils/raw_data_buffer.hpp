#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <complex>
#include "types.hpp"

// Raw packet structure for buffering complete TCP packets with pre-converted float IQ data
struct RawDataPacket {
    std::vector<std::vector<std::complex<float>>> channel_iq_data;  // Pre-converted IQ data per channel
    uint32_t num_channels;                              // Number of channels in packet
    uint32_t samples_per_channel;                       // Samples per channel
    uint32_t phase_compensation_state;                  // Phase calibration state from server
    uint32_t noise_source_active;                       // Bias tee / noise source status
    std::chrono::steady_clock::time_point timestamp;

    RawDataPacket() : num_channels(0), samples_per_channel(0),
                      phase_compensation_state(0), noise_source_active(0) {
        timestamp = std::chrono::steady_clock::now();
    }

    // Check if packet is too old (for automatic cleanup)
    bool is_stale(std::chrono::milliseconds max_age = std::chrono::milliseconds(1000)) const {
        auto age = std::chrono::steady_clock::now() - timestamp;
        return age > max_age;
    }
    
    // Calculate memory usage for this packet
    size_t memory_usage() const {
        size_t total = 0;
        for (const auto& channel : channel_iq_data) {
            total += channel.size() * sizeof(std::complex<float>);
        }
        return total;
    }
};

// Bounded queue for raw data packets with automatic old data removal.
// Every operation runs under queue_mutex (the consumer waits on it anyway), so
// a plain deque suffices. Single FIFO producer: packets are timestamped at
// construction just before push, so the oldest (stale) ones are at the front.
class RawDataBuffer {
private:
    std::deque<RawDataPacket> packet_queue;
    mutable std::mutex queue_mutex;
    std::condition_variable data_available;
    
    std::atomic<bool> shutdown_flag{false};
    
    // Configuration
    size_t max_queue_size = 100;           // Maximum packets in queue
    size_t max_memory_mb = 256;            // Maximum memory usage in MB
    std::chrono::milliseconds max_packet_age{1000}; // Maximum packet age
    
    // Statistics
    std::atomic<size_t> packets_added{0};
    std::atomic<size_t> packets_consumed{0};
    std::atomic<size_t> packets_dropped_full{0};
    std::atomic<size_t> packets_dropped_stale{0};
    std::atomic<size_t> total_memory_bytes{0};
    
    // Private methods
    void cleanup_stale_packets();
    void enforce_memory_limit();
    void enforce_queue_size_limit();
    void drop_front(std::atomic<size_t>& drop_counter);

public:
    RawDataBuffer(size_t max_packets = 100, size_t max_memory_mb = 256, 
                  std::chrono::milliseconds max_age = std::chrono::milliseconds(1000));
    ~RawDataBuffer();
    
    // Producer interface - non-blocking with automatic cleanup
    bool push_packet(RawDataPacket&& packet);

    // Consumer interface - waits up to timeout for a packet
    bool try_pop_packet(RawDataPacket& packet, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    
    // Queue management
    void clear();
    void shutdown();
    bool is_shutdown() const { return shutdown_flag.load(); }
    
    // Status and statistics
    size_t size() const;
    size_t capacity() const { return max_queue_size; }
    bool empty() const;
    bool full() const;
    float fill_percentage() const;
    size_t memory_usage_bytes() const { return total_memory_bytes.load(); }
    float memory_usage_mb() const { return memory_usage_bytes() / (1024.0f * 1024.0f); }
    
    // Statistics
    struct Stats {
        size_t packets_added;
        size_t packets_consumed;
        size_t packets_dropped_full;
        size_t packets_dropped_stale;
        size_t current_queue_size;
        size_t memory_usage_bytes;
        float fill_percentage;
    };
    
    Stats get_stats() const;
    void reset_stats();
    void print_stats(const std::string& prefix = "RawDataBuffer") const;
    
    // Configuration
    void set_max_queue_size(size_t size);
    void set_max_memory_mb(size_t mb);
    void set_max_packet_age(std::chrono::milliseconds age);
};