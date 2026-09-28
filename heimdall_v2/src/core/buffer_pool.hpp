#pragma once

#include "../external/concurrentqueue/blockingconcurrentqueue.h"
#include <memory>
#include <vector>

// Lock-free buffer pool for eliminating copies in hot paths
// Used to recycle L2 buffer allocations with move semantics
template<typename T>
class BufferPool {
private:
    moodycamel::ConcurrentQueue<T> available;
    size_t pool_size;

public:
    explicit BufferPool(size_t size) : pool_size(size) {
        // Seed the pool with default-constructed buffers
        for (size_t i = 0; i < size; ++i) {
            available.enqueue(T());
        }
    }

    // Get a buffer from the pool (allocates a fresh one if exhausted).
    T acquire() {
        T buffer;
        // Try non-blocking first for performance
        if (available.try_dequeue(buffer)) {
            return buffer;
        }

        // Pool exhausted - allocate new buffer (rare case)
        // This maintains correctness even under high load
        return T();
    }

    // Non-allocating acquire: returns false if the pool is exhausted instead of
    // mallocing. Used on the RTL USB callback hot path, which must never block
    // in the allocator (a stall there overflows the RTL2832 FIFO). Exhaustion is
    // then handled as a coherence-loss event, not a silent allocation.
    bool try_acquire(T& out) {
        return available.try_dequeue(out);
    }

    // Return a buffer to the pool
    void release(T&& buffer) {
        available.enqueue(std::move(buffer));
    }

    size_t size() const { return pool_size; }
};
