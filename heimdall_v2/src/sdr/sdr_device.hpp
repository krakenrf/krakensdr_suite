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
    ChannelCompensation compensation;
    mutable std::mutex compensation_mutex;

    // Memory pool for the RTL callback (alloc-free hot path). A buffer can be
    // in flight in L1 (up to MAX_BUFFER_SIZE) AND in the L2-raw staging queue (up
    // to L2_RAW_MAX sets, one buffer per device each) at the same time, plus a
    // couple in transit, so the pool must cover both - otherwise it would exhaust
    // under backlog and spuriously signal coherence loss.
    BufferPool<SampleBuffer> sample_pool{MAX_BUFFER_SIZE + L2_RAW_MAX + 4};

    // Software fractional-delay FIR state (owned by the sample pipeline
    // thread). The kernel starts as a pure delta at tap 3 (= uniform integer
    // group delay on every channel, preserving relative alignment) and is
    // recomputed whenever compensation.frac_delay changes.
    std::array<float, 8> frac_coeff{{0, 0, 0, 1, 0, 0, 0, 0}};
    std::array<Complex, 7> frac_tail{};
    float frac_coeff_for = 0.0f;

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
