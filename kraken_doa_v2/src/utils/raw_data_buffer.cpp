#include "utils/raw_data_buffer.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>

using namespace std;
using namespace chrono;

RawDataBuffer::RawDataBuffer(size_t max_packets, size_t max_memory_mb, milliseconds max_age) 
    : max_queue_size(max_packets), max_memory_mb(max_memory_mb), max_packet_age(max_age) {
    
    cout << "RawDataBuffer initialized:" << endl;
    cout << "  Max packets: " << max_queue_size << endl;
    cout << "  Max memory: " << max_memory_mb << " MB" << endl;
    cout << "  Max packet age: " << max_packet_age.count() << " ms" << endl;
    cout << "  IQ data pre-converted to float for efficiency" << endl;
}

RawDataBuffer::~RawDataBuffer() {
    shutdown();
}

void RawDataBuffer::drop_front(atomic<size_t>& drop_counter) {
    // Discard the oldest packet (caller must hold lock)
    total_memory_bytes.fetch_sub(packet_queue.front().memory_usage());
    packet_queue.pop_front();
    drop_counter.fetch_add(1);
}

void RawDataBuffer::cleanup_stale_packets() {
    // Remove packets that are too old (caller must hold lock). Oldest first,
    // so stop at the first fresh one.
    while (!packet_queue.empty() && packet_queue.front().is_stale(max_packet_age)) {
        drop_front(packets_dropped_stale);
    }
}

void RawDataBuffer::enforce_memory_limit() {
    // Remove oldest packets if memory usage exceeds limit (caller must hold lock)
    size_t max_bytes = max_memory_mb * 1024 * 1024;
    while (total_memory_bytes.load() > max_bytes && !packet_queue.empty()) {
        drop_front(packets_dropped_full);
    }
}

void RawDataBuffer::enforce_queue_size_limit() {
    // Remove oldest packets if queue size exceeds limit (caller must hold lock)
    while (packet_queue.size() > max_queue_size) {
        drop_front(packets_dropped_full);
    }
}

bool RawDataBuffer::push_packet(RawDataPacket&& packet) {
    if (shutdown_flag.load()) {
        return false;
    }
    
    lock_guard<mutex> lock(queue_mutex);
    
    // Cleanup stale packets first
    cleanup_stale_packets();
    
    // Add new packet
    size_t packet_size = packet.memory_usage();
    packet_queue.push_back(std::move(packet));
    total_memory_bytes.fetch_add(packet_size);
    packets_added.fetch_add(1);
    
    // Enforce limits (may remove the packet we just added if it's too big)
    enforce_memory_limit();
    enforce_queue_size_limit();
    
    // Notify waiting consumers
    data_available.notify_one();
    
    return true;
}

bool RawDataBuffer::try_pop_packet(RawDataPacket& packet, milliseconds timeout) {
    unique_lock<mutex> lock(queue_mutex);
    
    if (!data_available.wait_for(lock, timeout, [this]() {
        return shutdown_flag.load() || !packet_queue.empty();
    })) {
        return false; // Timeout
    }

    if (shutdown_flag.load() || packet_queue.empty()) {
        return false;
    }

    packet = std::move(packet_queue.front());
    packet_queue.pop_front();

    total_memory_bytes.fetch_sub(packet.memory_usage());
    packets_consumed.fetch_add(1);
    
    return true;
}

void RawDataBuffer::clear() {
    lock_guard<mutex> lock(queue_mutex);
    packet_queue.clear();
    total_memory_bytes = 0;
}

void RawDataBuffer::shutdown() {
    shutdown_flag = true;
    {
        lock_guard<mutex> lock(queue_mutex);
        data_available.notify_all();
    }
}

size_t RawDataBuffer::size() const {
    lock_guard<mutex> lock(queue_mutex);
    return packet_queue.size();
}

bool RawDataBuffer::empty() const {
    lock_guard<mutex> lock(queue_mutex);
    return packet_queue.empty();
}

bool RawDataBuffer::full() const {
    lock_guard<mutex> lock(queue_mutex);
    return packet_queue.size() >= max_queue_size;
}

float RawDataBuffer::fill_percentage() const {
    lock_guard<mutex> lock(queue_mutex);
    return (static_cast<float>(packet_queue.size()) / max_queue_size) * 100.0f;
}

RawDataBuffer::Stats RawDataBuffer::get_stats() const {
    lock_guard<mutex> lock(queue_mutex);
    
    Stats stats;
    stats.packets_added = packets_added.load();
    stats.packets_consumed = packets_consumed.load();
    stats.packets_dropped_full = packets_dropped_full.load();
    stats.packets_dropped_stale = packets_dropped_stale.load();
    stats.current_queue_size = packet_queue.size();
    stats.memory_usage_bytes = total_memory_bytes.load();
    stats.fill_percentage = (static_cast<float>(packet_queue.size()) / max_queue_size) * 100.0f;
    
    return stats;
}

void RawDataBuffer::reset_stats() {
    packets_added = 0;
    packets_consumed = 0;
    packets_dropped_full = 0;
    packets_dropped_stale = 0;
}

void RawDataBuffer::print_stats(const string& prefix) const {
    auto stats = get_stats();
    
    cout << prefix << " Stats:" << endl;
    cout << "  Queue: " << stats.current_queue_size << "/" << max_queue_size 
         << " (" << fixed << setprecision(1) << stats.fill_percentage << "%)" << endl;
    cout << "  Memory: " << fixed << setprecision(2) << (stats.memory_usage_bytes / (1024.0f * 1024.0f)) 
         << "/" << max_memory_mb << " MB" << endl;
    cout << "  Added: " << stats.packets_added << ", Consumed: " << stats.packets_consumed << endl;
    cout << "  Dropped (full): " << stats.packets_dropped_full 
         << ", Dropped (stale): " << stats.packets_dropped_stale << endl;
}

void RawDataBuffer::set_max_queue_size(size_t size) {
    lock_guard<mutex> lock(queue_mutex);
    max_queue_size = size;
    enforce_queue_size_limit();
}

void RawDataBuffer::set_max_memory_mb(size_t mb) {
    lock_guard<mutex> lock(queue_mutex);
    max_memory_mb = mb;
    enforce_memory_limit();
}

void RawDataBuffer::set_max_packet_age(milliseconds age) {
    lock_guard<mutex> lock(queue_mutex);
    max_packet_age = age;
    cleanup_stale_packets();
}