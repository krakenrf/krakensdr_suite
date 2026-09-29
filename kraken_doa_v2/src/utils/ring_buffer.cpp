#include "utils/ring_buffer.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

AudioRingBuffer::AudioRingBuffer(size_t size) : RingBufferBase<float>(size) {}

size_t AudioRingBuffer::write(const float* samples, size_t count) {
    if (count == 0) return 0;

    total_writes.fetch_add(1);

    size_t current_write = write_pos.load(std::memory_order_acquire);
    size_t current_read = read_pos.load(std::memory_order_acquire);

    size_t used_space = (current_write - current_read) & size_mask;
    size_t available_space = buffer.size() - used_space - 1;

    size_t to_write = std::min(count, available_space);

    if (to_write < count) {
        size_t dropped = count - to_write;
        overruns.fetch_add(dropped);

        // Log ring buffer overruns (buffer full)
        static int overrun_log = 0;
        if (++overrun_log % 10 == 0) {
            std::cout << "RingBuffer OVERRUN: Dropped " << dropped << "/" << count
                      << " samples (buffer " << (used_space * 100 / buffer.size()) << "% full)" << std::endl;
        }
    }

    for (size_t i = 0; i < to_write; i++) {
        buffer[(current_write + i) & size_mask] = samples[i];
    }

    write_pos.store((current_write + to_write) & size_mask, std::memory_order_release);
    return to_write;
}

void AudioRingBuffer::read(float* output, size_t count) {
    if (count == 0) return;

    total_reads.fetch_add(1);

    // A pending clear() drops everything written so far (consumer side only)
    if (flush_requested.exchange(false, std::memory_order_acq_rel)) {
        read_pos.store(write_pos.load(std::memory_order_acquire), std::memory_order_release);
    }

    size_t current_write = write_pos.load(std::memory_order_acquire);
    size_t current_read = read_pos.load(std::memory_order_acquire);

    size_t available = (current_write - current_read) & size_mask;
    size_t to_read = std::min(count, available);

    if (to_read > 0) {
        size_t end_space = buffer.size() - current_read;
        if (to_read <= end_space) {
            memcpy(output, &buffer[current_read], to_read * sizeof(float));
        } else {
            memcpy(output, &buffer[current_read], end_space * sizeof(float));
            memcpy(output + end_space, &buffer[0], (to_read - end_space) * sizeof(float));
        }
    }

    if (to_read < count) {
        size_t missing = count - to_read;
        memset(output + to_read, 0, missing * sizeof(float));
        underruns.fetch_add(missing);

        // Log ring buffer underruns (buffer empty)
        static int underrun_log = 0;
        if (++underrun_log % 10 == 0) {
            std::cout << "RingBuffer UNDERRUN: Missing " << missing << "/" << count
                      << " samples (buffer " << (available * 100 / buffer.size()) << "% full)" << std::endl;
        }
    }

    read_pos.store((current_read + to_read) & size_mask, std::memory_order_release);
}
