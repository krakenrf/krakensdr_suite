#include "sdr_pipeline.hpp"
#include "pipeline_control.hpp"
#include "downconverter.hpp"
#include "../core/utils.hpp"
#include "../core/config.hpp"
#include "../core/buffer_pool.hpp"
#include "../net/tcp_data_server.hpp"  // Need full definition to call methods
#include "../net/rtl_tcp_server.hpp"   // Need full definition to call methods
#include <iostream>
#include <algorithm>

// Global L2 buffer
moodycamel::BlockingConcurrentQueue<std::vector<ComplexBuffer>> l2_buffer;
std::atomic<size_t> l2_buffer_size{0};

// OPTIMIZED: Buffer pool to eliminate L2 copy overhead
// Pool size = 4 allows: 1 being filled, 1 in queue, 1 being processed, 1 spare
BufferPool<std::vector<ComplexBuffer>> l2_buffer_pool(4);

// ---- L2-raw staging (#4: decouple the time-critical L1 drain from heavy DSP) -
// The sample drain (sample_processor) now does ONLY the cheap aligned L1->raw
// collection and hands each aligned raw set off here; the conversion_worker does
// the heavy IQ conversion / compensation / TCP serialization. This keeps USB
// keep-up independent of DSP load: if conversion falls behind, whole ALIGNED
// raw sets drop here (coherence-safe), and L1 never backs up.
//
// The INNER SampleBuffers are owned by the per-device sample_pools (the callback
// acquires them, the drain moves them in, the conversion worker returns each to
// devices[i]->sample_pool). Only the OUTER vector is recycled via this pool.
moodycamel::BlockingConcurrentQueue<std::vector<SampleBuffer>> l2_raw_buffer;
std::atomic<size_t> l2_raw_buffer_size{0};
BufferPool<std::vector<SampleBuffer>> l2_raw_buffer_pool(L2_RAW_MAX + 4);

// C5: runtime L2-raw depth cap. Starts at the tighter L2_RAW_CAL_MAX (calibration
// runs from boot) so the lag/phase loops see fresher data under conversion-worker
// saturation; restored to L2_RAW_MAX by complete_phase_calibration_locked() once
// CONVERGED, and set back to L2_RAW_CAL_MAX whenever a recal starts
// (handle_settings_change / recover_coherence). The pools stay sized at L2_RAW_MAX.
std::atomic<size_t> l2_raw_cap{L2_RAW_CAL_MAX};

// Return an aligned raw set's inner buffers to their per-device pools and the
// outer vector to the raw pool. element i always belongs to devices[i] (aligned
// collection order), which is the invariant that keeps pool ownership correct.
static void recycle_raw_set(std::vector<SampleBuffer>& set) {
    const size_t n = std::min(set.size(), devices.size());
    for (size_t i = 0; i < n; i++) {
        if (devices[i]) devices[i]->sample_pool.release(std::move(set[i]));
    }
    l2_raw_buffer_pool.release(std::move(set));
}

// ---- L1 flush protocol ----
// Coherence needs the N-th L1 buffer of every device to be the same instant, so
// a flush (coherence recovery, lag reset, scanner retune) must drop exactly the
// same packets on every device. Two ways it used to slip a channel by a whole
// packet:
//  - the flush emptied the queues one by one while the USB callbacks kept
//    delivering: a packet round arriving mid-flush (the devices' copies land
//    up to ~ms apart) was dropped on some devices and kept on others;
//  - the drain, a second consumer of the same queues, discarded a set that
//    straddled the flush, including post-flush packets on the devices it
//    collected after the flush.
// So the drain - the only L1 consumer - performs every flush itself:
// clear_l1_buffer() files a request and waits; the drain drops its partial set
// and empties all queues at a quiet point (every device has delivered the same
// round and the next is still ~half a period away).
static std::atomic<uint32_t> flush_requests{0};    // bumped by clear_l1_buffer()
static std::atomic<uint32_t> flush_completed{0};   // last request the drain served
static std::atomic<bool> drain_active{false};
static std::atomic<std::thread::id> drain_thread_id{};

static int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Empty every device's L1 (buffers back to their pools). With wait_quiet, first
// wait - at most two packet periods - for a point where the active devices'
// latest packets all belong to the same round and the next round isn't due for
// another half period, so no round can straddle the flush. (If the devices'
// arrival skew ever exceeds half a period there is no such point: flush anyway
// after the timeout, as before.)
static void flush_l1_now(int num_elements, bool wait_quiet) {
    if (wait_quiet && num_elements > 0) {
        const int64_t period = static_cast<int64_t>(1e9 * NUM_SAMPLES / SAMPLE_RATE);
        const int64_t deadline = steady_now_ns() + 2 * period;
        while (steady_now_ns() < deadline) {
            int64_t lo = INT64_MAX, hi = 0;
            for (int i = 0; i < num_elements && i < static_cast<int>(devices.size()); i++) {
                if (!devices[i] || !devices[i]->dev) continue;
                const int64_t t = devices[i]->last_arrival_ns.load(std::memory_order_acquire);
                lo = std::min(lo, t); hi = std::max(hi, t);
            }
            if (hi == 0 || (hi - lo < period / 2 && steady_now_ns() - hi < period / 2)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    for (const auto& device : devices) {
        if (!device) continue;
        SampleBuffer discard;
        while (device->l1_buffer.try_dequeue(discard)) {
            device->l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);
            device->sample_pool.release(std::move(discard));  // Return to pool
        }
        // No store(0) - see clear_l2_raw_buffer(); a phantom here is fatal
        // (callback would see l1 size == SIZE_MAX and drop every packet).
    }
}

void samples_to_complex_with_compensation(const uint8_t* samples, int count, int channel,
                                          ComplexBuffer& out) {
    // Validate channel index
    if (channel < 0 || channel >= NUM_DEVICES) {
        std::cerr << "ERROR: Invalid channel " << channel
                  << " in samples_to_complex_with_compensation" << std::endl;
        out.assign(count, Complex(0.0f, 0.0f));
        return;
    }

    // OPTIMIZED: Lock-free atomic load - no mutex!
    // This is the hot path (called millions of times/sec)
    Complex comp = phase_compensation->compensation_vector.load(channel);

    // Forward (feed-forward) S2P compensation: removes the differential through-
    // response (S21) of the per-channel RF chain (cable / filter / LNA) that
    // sits in FRONT of the KrakenSDR, OUTSIDE the noise-source calibration loop.
    // It is folded into the SAME per-chunk scalar as the closed-loop phase vector
    // (one extra atomic load + complex multiply per CHUNK, zero per-sample cost)
    // and kept in a SEPARATE vector so it survives the identity resets the
    // closed-loop vector undergoes on every recalibration / retune. Off by
    // default; gated like the per-bin EQ (enabled relaxed, then ready acquire).
    // NOT applied while the noise source is on: the noise is injected inside
    // the KrakenSDR, after the external chain, so the calibration must measure
    // the internal path alone. Applied during calibration, the closed-loop
    // vector measured it too and cancelled its phase (only |fwd| survived).
    if (forward_comp.enabled.load(std::memory_order_relaxed) &&
        forward_comp.ready.load(std::memory_order_acquire) &&
        !bias_tee_enabled.load(std::memory_order_relaxed)) {
        comp *= forward_comp.vector.load(channel);
    }

    static thread_local ComplexBuffer tmp;
    tmp.resize(count);

    const uint8_t* __restrict p   = samples;
    Complex*      __restrict dst  = tmp.data();
    const float*  __restrict lut  = iq_lut();

    const float cr = comp.real();
    const float ci = comp.imag();

    // Wideband variant, high-side injection (LO = IF + RF): the mixer hands us
    // the conjugated baseband - mirrored spectrum, negated steering phases.
    // Un-mirror it at the source by negating Q, so our own correlation/phase
    // cal, the TCP broadcast and the DoA client all see the signal upright
    // with the true RF phase relationships. One multiply per sample, folded
    // into the auto-vectorized loop.
    const float qs = downconverter.spectral_inversion_active() ? -1.0f : 1.0f;

    for (int i = 0; i < count; ++i) {
        const float I = lut[p[0]];
        const float Q = qs * lut[p[1]];

        // Complex multiply: (I + jQ) * (cr + jci)
        const float re = std::fmaf(I, cr, -Q * ci); // I*cr - Q*ci
        const float im = std::fmaf(I, ci,  Q * cr); // I*ci + Q*cr

        dst[i] = Complex{re, im};
        p += 2;
    }

    // Software fractional-delay FIR (8-tap windowed sinc, group delay 3+d).
    // Runs on EVERY channel so they all share the same base group delay; the
    // kernel is a pure delta until a correction is commanded. This gives
    // deterministic sub-sample alignment - hardware register writes kick the
    // lag by a random +/-0.2-0.3 samples, so the final trim must be digital.
    //
    // `out` is the caller's (pooled) buffer, written in place so a recycled
    // buffer's capacity is reused. When the per-bin EQ stage follows, the lag
    // FIR writes to a thread-local intermediate and the EQ writes into `out`.
    auto& dev = devices[channel];
    const bool eq_active = per_bin_cal.enabled.load(std::memory_order_relaxed) &&
                           per_bin_cal.ready.load(std::memory_order_acquire);
    static thread_local ComplexBuffer fir_tmp;
    ComplexBuffer& fir_out = eq_active ? fir_tmp : out;
    fir_out.resize(count);
    {
        const float d = dev->compensation.frac_delay.load(std::memory_order_relaxed);
        if (d != dev->frac_coeff_for) {
            float g = 0.0f;
            for (int n = 0; n < 8; n++) {
                const float x = static_cast<float>(n) - 3.0f - d;
                const float s = (std::abs(x) < 1e-6f) ? 1.0f
                    : std::sin(static_cast<float>(M_PI) * x) / (static_cast<float>(M_PI) * x);
                const float w = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * (n + 0.5f) / 8.0f);
                dev->frac_coeff[n] = s * w;
                g += dev->frac_coeff[n];
            }
            if (std::abs(g) > 1e-6f) {
                for (auto& c : dev->frac_coeff) c /= g;  // unity DC gain
            }
            dev->frac_coeff_for = d;
        }

        const auto& h = dev->frac_coeff;
        auto& tail = dev->frac_tail;
        const Complex* __restrict x = tmp.data();
        Complex* __restrict y = fir_out.data();

        // Head: taps reach into the previous chunk's tail
        const int head = std::min(7, count);
        for (int i = 0; i < head; i++) {
            Complex acc(0.0f, 0.0f);
            for (int n = 0; n < 8; n++) {
                const int j = i - n;
                acc += h[n] * ((j >= 0) ? x[j] : tail[7 + j]);
            }
            y[i] = acc;
        }
        // Body: pure convolution, auto-vectorizable
        for (int i = head; i < count; i++) {
            Complex acc(0.0f, 0.0f);
            for (int n = 0; n < 8; n++) {
                acc += h[n] * x[i - n];
            }
            y[i] = acc;
        }
        // Save the last 7 input samples for the next chunk
        if (count >= 7) {
            for (int t = 0; t < 7; t++) tail[t] = x[count - 7 + t];
        }
    }

    // ---- Optional per-bin phase-equalizer FIR (final stage) ----
    // Only runs when the user enabled per-bin mode AND this channel's equalizer
    // has been designed. Off by default => this whole block is skipped and the
    // hot path is just the scalar multiply + lag FIR above. The equalizer is a
    // complex FIR_LEN-tap filter that flattens the residual frequency-dependent
    // phase across the band; reference channel's filter is a pure delta, so all
    // channels share the same FIR_LEN/2 group delay and stay aligned.
    if (eq_active) {
        constexpr int L = PerBinCalibration::FIR_LEN;
        out.resize(count);
        const Complex* __restrict h    = dev->eq_coeff.data();      // L taps
        Complex*       __restrict etail = dev->eq_tail.data();      // L-1 carry-over
        const Complex* __restrict x    = fir_out.data();
        Complex*       __restrict y    = out.data();

        const int head = std::min(L - 1, count);
        for (int i = 0; i < head; i++) {
            Complex acc(0.0f, 0.0f);
            for (int n = 0; n < L; n++) {
                const int j = i - n;
                acc += h[n] * ((j >= 0) ? x[j] : etail[(L - 1) + j]);
            }
            y[i] = acc;
        }
        for (int i = head; i < count; i++) {
            Complex acc(0.0f, 0.0f);
            for (int n = 0; n < L; n++) {
                acc += h[n] * x[i - n];
            }
            y[i] = acc;
        }
        if (count >= L - 1) {
            for (int t = 0; t < L - 1; t++) etail[t] = x[count - (L - 1) + t];
        }
    }
}

// Minimal, time-critical L1 -> L2-raw drain (#4). Does ONLY the cheap aligned
// collection: pull one block from each device's L1 in lockstep and hand the
// aligned raw set to the conversion_worker. No IQ conversion, no compensation,
// no TCP serialization here, so USB keep-up is independent of DSP load. Runs at
// realtime priority (below the USB readers).
void sample_processor(const std::vector<std::unique_ptr<SDRDevice>>& devices) {
    set_thread_realtime("sample-drain", RT_PRIO_SAMPLE_DRAIN);

    static bool first_run = true;

    // Serve flush requests from here on (see flush_l1_now); stop on every exit
    drain_thread_id.store(std::this_thread::get_id());
    drain_active.store(true, std::memory_order_release);
    struct DrainActiveGuard { ~DrainActiveGuard() { drain_active.store(false, std::memory_order_release); } } drain_guard;

    // Release only the slots actually filled: recycling an unfilled
    // (moved-from, zero-capacity) slot used to plant empty buffers in the pools.
    auto recycle_partial = [](std::vector<SampleBuffer>& set, int filled) {
        set.resize(static_cast<size_t>(std::max(filled, 0)));
        recycle_raw_set(set);
    };
    // A pending clear_l1_buffer() request: drop the partial set (its buffers
    // are pre-flush) and flush every queue at a quiet point.
    auto serve_flush = [](int n) {
        const uint32_t req = flush_requests.load(std::memory_order_acquire);
        if (req == flush_completed.load(std::memory_order_relaxed)) return false;
        flush_l1_now(n, true);
        flush_completed.store(req, std::memory_order_release);
        return true;
    };

    while (global_running && pipeline_running.load(std::memory_order_acquire)) {
        // Re-read per pass: the thread is restarted by a reconfiguration, but a
        // stale count here would index closed channels for a whole run.
        const int num_elements = std::min(active_num_elements.load(),
                                          static_cast<int>(devices.size()));

        // Outer vector recycled from the pool; inner buffers are filled by the
        // L1 dequeues below (each owned by its device's sample_pool).
        serve_flush(num_elements);

        auto raw_set = l2_raw_buffer_pool.acquire();
        raw_set.resize(num_elements);
        bool flushed_mid_set = false;

        // -------- Phase 1: Collect in device order (only active elements) --------
        for (int i = 0; i < num_elements; i++) {
            const auto& device = devices[i];

            // Short timeout so we can notice global_running. A single device that
            // keeps timing out while the others produce is starving the set (and
            // overflowing their L1): bound the wait, then declare coherence lost
            // so the watchdog flushes+recalibrates instead of spinning forever on
            // a stuck dongle.
            int timeouts = 0;
            while (!device->l1_buffer.wait_dequeue_timed(raw_set[i], std::chrono::milliseconds(100))) {
                if (!global_running || !pipeline_running.load(std::memory_order_acquire)) {
                    recycle_partial(raw_set, i);
                    return;
                }
                if (flush_requests.load(std::memory_order_acquire) !=
                    flush_completed.load(std::memory_order_relaxed)) {
                    break;  // served below, with the partial set dropped
                }
                if (++timeouts >= STUCK_DEVICE_MAX_TIMEOUTS) {
                    signal_coherence_lost("sample drain: a device stalled (no data)");
                    timeouts = 0;  // re-alert periodically if it stays stuck
                }
            }
            if (flush_requests.load(std::memory_order_acquire) !=
                flush_completed.load(std::memory_order_relaxed)) {
                // slot i holds a dequeued buffer unless the wait above broke out
                // empty-handed; either way everything collected is pre-flush
                const bool got = !raw_set[i].empty();
                if (got) device->l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                recycle_partial(raw_set, got ? i + 1 : i);
                serve_flush(num_elements);
                flushed_mid_set = true;
                break;
            }

            if (!global_running || !pipeline_running.load(std::memory_order_acquire)) {
                device->l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                recycle_partial(raw_set, i + 1);
                return;
            }
            device->l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);

            if (first_run) {
                std::cout << "Sample processor: Position " << i
                     << " -> Channel " << device->index
                     << " (Serial: " << device->serial_number
                     << ", Physical device: " << device->device_id
                     << ") buffer size=" << raw_set[i].size() << std::endl;

                if (device->index != i) [[unlikely]] {
                    std::cerr << "ERROR: Device index mismatch! Position " << i
                         << " has device with index " << device->index << std::endl;
                }
            }
        }

        if (first_run) {
            first_run = false;
            std::cout << "Sample processor: Successfully collected first sample set from "
                 << num_elements << " active channels" << std::endl;
        }

        if (flushed_mid_set) continue;  // partial set already dropped with the flush

        // -------- Phase 2: Hand the aligned raw set to the conversion worker -----
        // If the worker has fallen behind, drop the OLDEST whole aligned set.
        // Dropping a full set is coherence-safe (every channel loses the same
        // set, so the next set stays aligned) - unlike the old per-device L1
        // drop, which shifted one channel by a packet and silently desynced.
        if (l2_raw_buffer_size.load(std::memory_order_relaxed) >= l2_raw_cap.load(std::memory_order_relaxed)) {
            std::vector<SampleBuffer> discard;
            if (l2_raw_buffer.try_dequeue(discard)) {
                l2_raw_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                recycle_raw_set(discard);
            }
        }
        l2_raw_buffer.enqueue(std::move(raw_set));
        l2_raw_buffer_size.fetch_add(1, std::memory_order_relaxed);
    }
}

// Heavy DSP stage (#4): consume aligned raw sets, convert uint8 IQ -> complex
// with phase/lag/EQ compensation (parallel across channels), broadcast every set
// to the TCP/RTL-TCP streamers in order, and push the converted set to L2 for
// the correlation processor. Decoupled from the drain so it can fall behind
// (dropping whole sets upstream at L2-raw) without ever stalling USB reads. The
// per-channel FIR state stays correct: sets are processed in order on this one
// thread, and the parallel loop touches each channel's state from exactly one task.
void conversion_worker(const std::vector<std::unique_ptr<SDRDevice>>& devices,
                       TcpDataServer* tcp_data_server,
                       RtlTcpServer* rtl_tcp_server) {
    // Name only, NORMAL priority on purpose. This stage allocates (ComplexBuffers,
    // the ~160 KB TCP packet) and takes the TCP clients_mutex / glibc arena lock;
    // at realtime priority those become unbounded priority-inversion sources
    // against normal-priority web/FFTW threads. It does not need RT: if it falls
    // behind, whole aligned sets drop at L2-raw (coherence-safe) and only the
    // converted-output rate dips - USB keep-up is unaffected (that is the drain).
    set_thread_realtime("convert", 0);

    while (global_running && pipeline_running.load(std::memory_order_acquire)) {
        std::vector<SampleBuffer> raw_set;
        if (!l2_raw_buffer.wait_dequeue_timed(raw_set, std::chrono::milliseconds(100))) {
            if (!global_running || !pipeline_running.load(std::memory_order_acquire)) return;
            continue;  // timeout: nothing to convert yet
        }
        if (!global_running || !pipeline_running.load(std::memory_order_acquire)) {
            recycle_raw_set(raw_set);
            return;
        }
        l2_raw_buffer_size.fetch_sub(1, std::memory_order_relaxed);

        // -------- Convert (in place into the pooled set) --------
        // Channel count comes from the SET, not a captured constant: the drain
        // sizes each aligned set to the element count it collected under.
        const int num_elements = static_cast<int>(std::min(raw_set.size(), devices.size()));

        auto complex_samples = l2_buffer_pool.acquire();
        complex_samples.resize(num_elements);
        for (int i = 0; i < num_elements; ++i) {
            auto& sb = raw_set[i];
            // sb.size() is bytes of interleaved IQ; divide by 2 for complex count
            samples_to_complex_with_compensation(
                sb.data(), static_cast<int>(sb.size()) / 2, devices[i]->index,
                complex_samples[i]);
        }

        // -------- Broadcast / handoff (every set, in order) --------
        if (tcp_data_server) tcp_data_server->broadcast_data(complex_samples);
        if (rtl_tcp_server)  rtl_tcp_server->broadcast_data(complex_samples);

        // -------- Push to L2 (drop oldest if full - coherence-safe) --------
        if (l2_buffer_size.load(std::memory_order_relaxed) >= BUFFER_SIZE) {
            std::vector<ComplexBuffer> discard;
            if (l2_buffer.try_dequeue(discard)) {
                l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                l2_buffer_pool.release(std::move(discard));
            }
        }
        l2_buffer.enqueue(std::move(complex_samples));
        l2_buffer_size.fetch_add(1, std::memory_order_relaxed);

        // -------- Recycle the raw set (inner buffers -> per-device pools) --------
        recycle_raw_set(raw_set);
    }
}

void signal_coherence_lost(const char* reason) {
    // Latch the flag (set exactly once per outstanding event) and tally it. The
    // watchdog clears the flag when it begins recovery, so a desync that recurs
    // after recovery re-arms cleanly. Logging is throttled to the first setter so
    // a storm of callback overflows doesn't flood the console.
    bool expected = false;
    if (coherence_lost.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        uint32_t n = coherence_event_count.fetch_add(1, std::memory_order_relaxed) + 1;
        std::cerr << "!! COHERENCE LOST (event #" << n << "): " << reason
                  << " - flushing and recalibrating" << std::endl;
    }
}

// Flush the aligned L2-raw staging queue, returning inner buffers to their
// per-device pools. Folded into clear_l1_buffer()/clear_l2_buffer() below so
// every existing stale-data flush (frequency change, scanner retune, lag reset,
// coherence recovery) also drains L2-raw - otherwise stale raw sets sitting
// between L1 and L2 would be converted and leak downstream after the flush.
void clear_l2_raw_buffer() {
    std::vector<SampleBuffer> discard;
    while (l2_raw_buffer.try_dequeue(discard)) {
        l2_raw_buffer_size.fetch_sub(1, std::memory_order_relaxed);
        recycle_raw_set(discard);
    }
    // NO store(0): these size counters are maintained purely by matched
    // fetch_add (on enqueue) / fetch_sub (on every dequeue, including the loop
    // above), so they track the true depth and never underflow. A store(0) here
    // would race a concurrent producer enqueue/consumer dequeue and could leave a
    // phantom item whose later fetch_sub wraps the size_t to SIZE_MAX -
    // permanently defeating the depth cap and (for L1) wedging the callback's
    // "L1 full" check so every packet is dropped.
}

void clear_l1_buffer() {
    std::cout << "Clearing L1 buffers for all devices..." << std::endl;
    clear_l2_raw_buffer();  // L2-raw holds data drained from L1; flush it too
    // The drain performs the flush (see flush_l1_now); wait for it. Flush
    // directly when there is no drain (pipeline stopped) or it doesn't answer.
    if (drain_active.load(std::memory_order_acquire) &&
        drain_thread_id.load() != std::this_thread::get_id()) {
        const uint32_t req = flush_requests.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (int waited_ms = 0; waited_ms < 500; waited_ms++) {
            if (static_cast<int32_t>(flush_completed.load(std::memory_order_acquire) - req) >= 0) return;
            if (!drain_active.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cerr << "L1 flush: sample drain did not respond - flushing directly" << std::endl;
    }
    flush_l1_now(active_num_elements.load(), true);
}

void clear_l2_buffer() {
    std::cout << "Clearing entire L2 buffer..." << std::endl;
    clear_l2_raw_buffer();  // flush the upstream staging too, so it can't refill L2
    std::vector<ComplexBuffer> discard;
    while (l2_buffer.try_dequeue(discard)) {
        l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
        l2_buffer_pool.release(std::move(discard));  // Return to pool
    }
    // No store(0): see clear_l2_raw_buffer (avoids the size_t underflow race).
}
