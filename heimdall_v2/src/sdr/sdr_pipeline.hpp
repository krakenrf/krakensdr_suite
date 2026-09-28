#pragma once

#include "../core/types.hpp"
#include "../core/buffer_pool.hpp"
#include "sdr_device.hpp"
#include "../external/concurrentqueue/blockingconcurrentqueue.h"
#include <memory>
#include <vector>
#include <atomic>

// Forward declarations for TCP servers
class TcpDataServer;
class RtlTcpServer;

// Global L2 buffer
extern moodycamel::BlockingConcurrentQueue<std::vector<ComplexBuffer>> l2_buffer;
extern std::atomic<size_t> l2_buffer_size;

// Global L2 buffer pool (for zero-copy operations)
extern BufferPool<std::vector<ComplexBuffer>> l2_buffer_pool;

// L2-raw staging queue (#4): aligned raw sample sets handed from the drain to
// the conversion worker. Inner SampleBuffers are owned by per-device sample_pools.
extern moodycamel::BlockingConcurrentQueue<std::vector<SampleBuffer>> l2_raw_buffer;
extern std::atomic<size_t> l2_raw_buffer_size;
extern BufferPool<std::vector<SampleBuffer>> l2_raw_buffer_pool;
extern std::atomic<size_t> l2_raw_cap;  // C5: runtime L2-raw depth cap (tighter during calibration)

// Sample conversion and processing
// Writes `count` compensated samples into `out` (resized in place, so a pooled
// buffer's capacity is reused).
void samples_to_complex_with_compensation(const uint8_t* samples, int count, int channel,
                                          ComplexBuffer& out);

// L1 -> L2-raw drain (cheap, time-critical, realtime priority).
void sample_processor(const std::vector<std::unique_ptr<SDRDevice>>& devices);

// L2-raw -> convert/compensate -> broadcast -> L2 (heavy DSP stage).
void conversion_worker(const std::vector<std::unique_ptr<SDRDevice>>& devices,
                       TcpDataServer* tcp_data_server = nullptr,
                       RtlTcpServer* rtl_tcp_server = nullptr);

// Buffer management
void clear_l1_buffer();
void clear_l1_buffer(int channel);
void clear_l2_buffer();
void clear_l2_raw_buffer();

// Coherence-loss signalling. Cheap and callable from any thread (including the
// alloc-free RTL callback): sets the coherence_lost flag exactly once per event,
// bumps coherence_event_count, and logs the reason. The actual heavy recovery
// (flush + recalibration) is performed by the coherence watchdog, never here.
void signal_coherence_lost(const char* reason);