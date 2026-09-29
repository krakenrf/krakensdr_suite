#pragma once

#include <vector>
#include <atomic>
#include <cstring>

template<typename T>
class RingBufferBase {
protected:
    std::vector<T> buffer;
    std::atomic<size_t> write_pos{0};
    std::atomic<size_t> read_pos{0};
    size_t size_mask;
    
    std::atomic<size_t> total_writes{0};
    std::atomic<size_t> total_reads{0};
    std::atomic<size_t> overruns{0};
    std::atomic<size_t> underruns{0};
    // Set by clear(); the consumer applies it (see clear()).
    std::atomic<bool> flush_requested{false};
    
public:
    RingBufferBase(size_t size);
    virtual ~RingBufferBase() = default;
    
    size_t available() const;
    size_t capacity() const;
    float fill_percentage() const;
    void get_stats(size_t& writes, size_t& reads, size_t& over, size_t& under) const;
    void reset_stats();
    void clear();
};

class AudioRingBuffer : public RingBufferBase<float> {
public:
    AudioRingBuffer(size_t size);
    size_t write(const float* samples, size_t count);
    void read(float* output, size_t count);
};

// Template implementations
template<typename T>
RingBufferBase<T>::RingBufferBase(size_t size) {
    size_t actual_size = 1;
    while (actual_size < size) actual_size <<= 1;
    
    buffer.resize(actual_size);
    size_mask = actual_size - 1;
}

template<typename T>
size_t RingBufferBase<T>::available() const {
    return (write_pos.load(std::memory_order_acquire) - read_pos.load(std::memory_order_acquire)) & size_mask;
}

template<typename T>
size_t RingBufferBase<T>::capacity() const { 
    return buffer.size(); 
}

template<typename T>
float RingBufferBase<T>::fill_percentage() const {
    return (available() * 100.0f) / capacity();
}

template<typename T>
void RingBufferBase<T>::get_stats(size_t& writes, size_t& reads, size_t& over, size_t& under) const {
    writes = total_writes.load();
    reads = total_reads.load();
    over = overruns.load();
    under = underruns.load();
}

template<typename T>
void RingBufferBase<T>::reset_stats() {
    total_writes = 0;
    total_reads = 0;
    overruns = 0;
    underruns = 0;
}

// Discard everything buffered. Safe from ANY thread: it only raises a flag,
// and the consumer applies it at its next read by moving read_pos up to
// write_pos. The ring is single-producer/single-consumer - each position has
// exactly one writer. Storing both positions from a third thread (the old
// clear) could land between a read's loads and its read_pos store, leaving
// read_pos ahead of write_pos: ~64K stale samples then looked available
// (seconds of old audio replayed) and the writer saw a full ring.
template<typename T>
void RingBufferBase<T>::clear() {
    flush_requested.store(true, std::memory_order_release);
}
