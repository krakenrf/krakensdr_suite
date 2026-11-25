#pragma once

#include "../core/types.hpp"
#include "../core/buffer_pool.hpp"
#include "../../external/concurrentqueue/blockingconcurrentqueue.h"
#include <rtl-sdr.h>
#include <thread>
#include <string>
#include <array>

struct SDRDevice {
    rtlsdr_dev_t* dev = nullptr;
    int device_id, index;
    std::string serial_number;
    moodycamel::BlockingConcurrentQueue<SampleBuffer> l1_buffer;
    static constexpr size_t MAX_BUFFER_SIZE = BUFFER_SIZE;
    std::thread async_thread;
    std::atomic<bool> running{true}, init_success{false};
    // This handle streams into usbfs zero-copy buffers (rtlsdr_prealloc_async
    // mapped them in init_rtlsdr_device); the dashboard's "zerocopy"
    std::atomic<bool> zero_copy{false};
    // Set by the USB reader thread once it is past rtlsdr_read_async (or never
    // entered it). stop_pipeline_threads() re-sends the cancel until it is set.
    std::atomic<bool> async_exited{true};
    std::atomic<size_t> l1_buffer_size{0}; // Track size atomically
    // steady_clock ns at which this device's most recent USB packet was
    // enqueued in L1 (or dropped) - stamped at the END of the callback. clear_l1_buffer() flushes at a
    // moment when every device has delivered the same packet round, so a flush
    // can't land between two devices' copies of one packet (a one-packet slip).
    // prev_arrival_ns is the stamp before it: the flush also requires each
    // device's last gap to be about one packet period, since a reader thread
    // catching up after a stall delivers an OLD round late, which the
    // last-arrival times alone can't tell from the current one.
    std::atomic<int64_t> last_arrival_ns{0};
    std::atomic<int64_t> prev_arrival_ns{0};
    // Every packet enqueued in L1 is numbered (l1_put = how many so far,
    // stored after the packet is in L1 and its stamp below is written) and
    // its arrival stamp kept in a ring: sweep mode's gate draws each stream's
    // earliest-arrival line through them (sdr_pipeline.cpp, serve_gate_now).
    // ring_n[slot] = the number of the packet whose stamp ring_t[slot] holds
    // (UINT64_MAX while it is being written). stream_first = the number of
    // the first packet after the stamps were reset (a stream start) or after
    // a short transfer (the line can't run across it). Written by the
    // callback only.
    // LINE_PACKETS: the gate's window. A stalled reader delivers at most
    // RTL_USB_BUF_COUNT packets late (the transfers that completed meanwhile;
    // later samples are lost in the RTL2832's FIFO, which only makes the
    // stream newer than its line), so a window of more than that always
    // holds packets a running reader delivered - the line runs through one
    // of them, never through a burst (serve_gate_now). The ring holds two
    // windows, so a packet's whole window is readable while it can still be
    // in L1 unjudged (an unreadable window counts as "before T").
    static constexpr int LINE_PACKETS = RTL_USB_BUF_COUNT + 16;
    static constexpr int ARRIVAL_RING = 2 * LINE_PACKETS + 32;
    std::atomic<uint64_t> l1_put{0};
    std::atomic<uint64_t> stream_first{0};
    std::array<std::atomic<int64_t>, ARRIVAL_RING> ring_t{};
    std::array<std::atomic<uint64_t>, ARRIVAL_RING> ring_n{};
    ChannelCompensation compensation;
    mutable std::mutex compensation_mutex;

    // Memory pool for the RTL callback (alloc-free hot path). A buffer can be
    // in flight in L1 (up to MAX_BUFFER_SIZE) AND in the L2-raw staging queue (up
    // to L2_RAW_MAX sets, one buffer per device each) at the same time, plus a
    // couple in transit, so the pool must cover both - otherwise it would exhaust
    // under backlog and spuriously signal coherence loss.
    BufferPool<SampleBuffer> sample_pool{MAX_BUFFER_SIZE + L2_RAW_MAX + 4};

    // Software fractional-delay FIR state (owned by the sample pipeline
    // thread): FRAC_TAPS taps, group delay FRAC_CENTRE + frac_delay. The
    // kernel starts as a pure delta at tap FRAC_CENTRE (= uniform integer
    // group delay on every channel, preserving relative alignment) and is
    // recomputed whenever compensation.frac_delay changes. 16 taps since
    // 2026-11-26 (8 before: its phase error reached 11.8 deg at 0.4 fs -
    // docs/dev/calibration.md *Fractional-delay trim filter*).
    static constexpr int FRAC_TAPS = 16;
    static constexpr int FRAC_CENTRE = FRAC_TAPS / 2 - 1;  // 7
    static constexpr std::array<float, FRAC_TAPS> frac_delta() {
        std::array<float, FRAC_TAPS> a{};
        a[FRAC_CENTRE] = 1.0f;
        return a;
    }
    std::array<float, FRAC_TAPS> frac_coeff = frac_delta();
    std::array<Complex, FRAC_TAPS - 1> frac_tail{};
    float frac_coeff_for = 0.0f;
    // the flush epoch frac_tail was saved under (conversion thread only): a
    // set after a flush starts a new stream piece - in sweep mode another
    // hop, another frequency and gain - so its tail is the set's own first
    // sample, not the last samples before the flush (those reached the first
    // FRAC_CENTRE output samples: with the 8-tap filter of the time, a hop's
    // first valid packet held 3 samples of the previous one - with auto gain,
    // values clipped at another gain)
    uint32_t frac_tail_epoch = ~0u;

    // Optional per-bin phase-equalizer FIR (complex taps). Designed once at the
    // end of calibration when per-bin mode is on; applied as the final hot-path
    // stage only while per_bin_cal.enabled && per_bin_cal.ready (a single shared
    // publish flag, so the whole channel set switches in/out atomically).
    std::array<Complex, PerBinCalibration::FIR_LEN> eq_coeff{};
    std::array<Complex, PerBinCalibration::FIR_LEN - 1> eq_tail{};

    SDRDevice(int id, int idx, const std::string& serial);
};

// RTL-SDR callback function
void rtlsdr_callback(unsigned char* buf, uint32_t len, void* ctx);
