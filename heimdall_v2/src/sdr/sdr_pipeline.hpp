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

// Settings the conversion reads ONCE per aligned set (conversion_worker), so
// every channel of a set is converted the same way: read per channel, a set
// straddling a change (noise on/off, per-bin EQ, spectral inversion) came out
// with e.g. a 16-sample EQ delay or a conjugation on only some channels.
struct SetConversionState {
    Complex comp[NUM_DEVICES];  // closed-loop phase vector x forward S2P comp
    bool spectral_inversion = false;
    bool eq_active = false;
};

// Sample conversion and processing
// Writes `count` compensated samples into `out` (resized in place, so a pooled
// buffer's capacity is reused).
void samples_to_complex_with_compensation(const uint8_t* samples, int count, int channel,
                                          const SetConversionState& st, ComplexBuffer& out);

// L1 -> L2-raw drain (cheap, time-critical, realtime priority).
void sample_processor(const std::vector<std::unique_ptr<SDRDevice>>& devices);

// L2-raw -> convert/compensate -> broadcast -> L2 (heavy DSP stage).
void conversion_worker(const std::vector<std::unique_ptr<SDRDevice>>& devices,
                       TcpDataServer* tcp_data_server = nullptr,
                       RtlTcpServer* rtl_tcp_server = nullptr);

// Buffer management
// Flush every device's L1 at a packet-round boundary, performed by the sample
// drain (the only L1 consumer) so all devices drop exactly the same packets.
// Blocks until done (<= ~2 packet periods). A per-device flush is deliberately
// not offered: it would desync that channel by design.
void clear_l1_buffer();
void clear_l2_buffer();
void clear_l2_raw_buffer();
// The downstream flush without the stdout line (each sweep hop starts with
// one; docs/dev/operating-modes.md "Sweep mode")
void clear_l2_buffer_quiet();
// The L1 flush without the stdout line, returning whether it found its quiet point - i.e. every
// stream lost the same packet round and the sets are paired again (false: it
// flushed anyway after four packet periods, a channel may have slipped)
bool clear_l1_buffer_paired();
// L1 flushes since startup that found no quiet point between packet rounds
// (flushed anyway: a channel may have slipped by a whole packet)
uint32_t l1_unquiet_flushes();
// Sweep mode's per-hop flush (sweep.cpp; the sample drain serves it): every
// active device drops its L1 up to and including its first packet stamped
// after `from_ns` (the hop's retune end + settle) with a steady gap - the
// last packet that can hold a sample from before then - each device on its
// own (no cross-device quiet point: nothing pairs buffers in sweep mode),
// then the downstream flush. Blocks until then (about a packet period after
// from_ns). false: not every device got there (one stalled, the drain did
// not answer in 500 ms) - the stream is not valid yet. Without a running
// drain it empties L1 directly (nothing streams) and returns true.
bool sweep_gate_l1(int64_t from_ns);
// Gates that accepted a device's packet without a steady gap (after four
// periods of irregular delivery; the counterpart of l1_unquiet_flushes)
uint32_t sweep_gates_unsteady();

// Coherence-loss signalling. Cheap and callable from any thread (including the
// alloc-free RTL callback): sets the coherence_lost flag exactly once per event,
// bumps coherence_event_count, and logs the reason. The actual heavy recovery
// (flush + recalibration) is performed by the coherence watchdog, never here.
void signal_coherence_lost(const char* reason);