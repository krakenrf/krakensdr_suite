#include "sdr_device.hpp"
#include "../core/types.hpp"

// Defined in sdr_pipeline.cpp. Cheap, alloc-free, callable from the USB callback.
extern void signal_coherence_lost(const char* reason);

SDRDevice::SDRDevice(int id, int idx, const std::string& serial)
    : device_id(id), index(idx), serial_number(serial) {
    for (auto& n : ring_n) n.store(UINT64_MAX, std::memory_order_relaxed);
}

// Runs on the per-device librtlsdr USB reader thread. Must be fast and strictly
// allocation-free: any stall here (an allocator lock, a per-device drop that
// desyncs the channels) corrupts coherence or overflows the RTL2832 FIFO.
void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* sdr = static_cast<SDRDevice*>(ctx);
    if (!sdr->running || !global_running) return;

    // Stamp AFTER the packet is in L1 (or dropped): clear_l1_buffer's quiet
    // point must mean "every device's latest round is already enqueued".
    // Stamped at callback entry, the flush could fire while the last device
    // was still copying its buffer, which then landed after the flush - that
    // device kept a packet the others lost (a one-packet slip).
    auto stamp = [sdr]() {
        // prev before last: the flush re-reads last to detect a torn pair
        const int64_t last = sdr->last_arrival_ns.load(std::memory_order_relaxed);
        sdr->prev_arrival_ns.store(last, std::memory_order_relaxed);
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        sdr->last_arrival_ns.store(now, std::memory_order_release);
        return std::pair<int64_t, bool>(now, last == 0);
    };

    // L1 full => the sample drain fell behind on THIS device. The old behaviour
    // dropped the oldest buffer for this one device, shifting it a full packet
    // relative to the others and SILENTLY destroying coherence. Instead, drop
    // the incoming buffer and raise a coherence-loss event: the watchdog then
    // does a coordinated whole-set flush + recalibration (which is the only
    // coherence-safe way to drop), turning a silent desync into a recoverable,
    // visible one.
    if (sdr->l1_buffer_size.load(std::memory_order_relaxed) >= SDRDevice::MAX_BUFFER_SIZE) {
        signal_coherence_lost("L1 overflow: sample drain fell behind");
        stamp();
        return;
    }

    // Never malloc in the callback. On pool exhaustion (same back-pressure
    // condition) drop this buffer and signal, rather than allocating and risking
    // an allocator stall that overflows the USB FIFO.
    SampleBuffer buffer;
    if (!sdr->sample_pool.try_acquire(buffer)) {
        signal_coherence_lost("sample pool exhausted in RTL callback");
        stamp();
        return;
    }

    // Reuses the pooled buffer's capacity (fixed len = NUM_SAMPLES*2), so no
    // allocation after the pool warms up.
    buffer.assign(buf, buf + len);
    sdr->l1_buffer.enqueue(std::move(buffer));
    sdr->l1_buffer_size.fetch_add(1, std::memory_order_relaxed);
    const auto [now, first] = stamp();
    // This packet's number and arrival stamp (sdr_device.hpp), a seqlock
    // per slot; l1_put last: a reader that sees n + 1 there finds the stamp
    const uint64_t n = sdr->l1_put.load(std::memory_order_relaxed);
    if (first) sdr->stream_first.store(n, std::memory_order_relaxed);
    const size_t slot = static_cast<size_t>(n % SDRDevice::ARRIVAL_RING);
    sdr->ring_n[slot].store(UINT64_MAX, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sdr->ring_t[slot].store(now, std::memory_order_relaxed);
    sdr->ring_n[slot].store(n, std::memory_order_release);
    sdr->l1_put.store(n + 1, std::memory_order_release);
}
