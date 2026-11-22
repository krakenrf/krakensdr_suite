#include "sdr_pipeline.hpp"
#include "pipeline_control.hpp"
#include "downconverter.hpp"
#include "sweep.hpp"
#include "../core/utils.hpp"
#include "../core/config.hpp"
#include "../core/buffer_pool.hpp"
#include "../net/tcp_data_server.hpp"  // Need full definition to call methods
#include "../net/rtl_tcp_server.hpp"   // Need full definition to call methods
#include <iostream>
#include <algorithm>
#include <climits>
#include <mutex>
#include <string>

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
// whether the flush the drain served last found its quiet point (stored
// before flush_completed, read after it)
static std::atomic<bool> last_served_quiet{true};
static std::atomic<std::thread::id> drain_thread_id{};

// The sweep's per-hop gate (sweep_gate_l1, sweep mode only), served by the
// drain like a flush: every dongle drops its packets that can hold a sample
// from before gate_from_ns, each on its own - no cross-device quiet point:
// in sweep mode no channel's packet has to pair with another's, and with
// spread buffer timing that point never comes (serve_gate_now;
// docs/dev/operating-modes.md, "Hop timing").
static std::atomic<uint32_t> gate_requests{0};
static std::atomic<uint32_t> gate_completed{0};
static std::atomic<int64_t> gate_from_ns{0};     // stored before gate_requests is bumped
static std::atomic<int> gate_last_outcome{0};    // GateOutcome, stored before gate_completed

// a flush or a gate waits for the drain
static bool l1_request_pending() {
    return flush_requests.load(std::memory_order_acquire) != flush_completed.load(std::memory_order_relaxed) ||
           gate_requests.load(std::memory_order_acquire) != gate_completed.load(std::memory_order_relaxed);
}

// Downstream flush epoch. Every flush (clear_l1_buffer / clear_l2_buffer)
// bumps it under l2_flush_mutex while emptying L2-raw and L2. The conversion
// worker notes the epoch BEFORE it dequeues a raw set and only broadcasts /
// pushes the converted set if no flush happened meanwhile (the L2 push is
// checked under the same mutex). Without it the set the worker was holding
// across a flush - and one the drain finished just after the L2-raw clear -
// reached L2 after the flush: pre-retune / pre-recovery data measured by the
// fresh calibration.
static std::mutex l2_flush_mutex;
static std::atomic<uint32_t> l2_flush_epoch{0};

static int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Empty every device's L1 (buffers back to their pools). With wait_quiet, first
// wait - at most four packet periods - for a point where the active devices'
// latest packets all belong to the same round and the next round isn't due for
// another half period, so no round can straddle the flush:
//  - every device's latest arrival within half a period of the others and of now
//  - every device's last gap about one period (0.5-1.5): a reader thread that
//    stalled delivers its backlog late and then in a burst, so its latest
//    packet can be an OLDER round arriving next to the others' newer one -
//    flushed then, it keeps one packet fewer (a one-packet slip)
// If there is no such point (arrival skew above half a period, a reader stuck
// in catch-up) flush anyway after the timeout, as before, with a warning.
// L1 flushes that found no quiet point (a channel may have slipped by a packet)
static std::atomic<uint32_t> l1_unquiet_flush_count{0};
uint32_t l1_unquiet_flushes() { return l1_unquiet_flush_count.load(std::memory_order_relaxed); }

// Returns whether the flush found its quiet point (true without wait_quiet
// or with no device: nothing to pair)
static bool flush_l1_now(int num_elements, bool wait_quiet, bool warn = true) {
    bool found_quiet = true;
    if (wait_quiet && num_elements > 0) {
        const int64_t period = static_cast<int64_t>(1e9 * NUM_SAMPLES / SAMPLE_RATE);
        const int64_t deadline = steady_now_ns() + 4 * period;
        bool quiet = false;
        while (!quiet && steady_now_ns() < deadline) {
            int64_t lo = INT64_MAX, hi = 0;
            bool steady = true;
            for (int i = 0; i < num_elements && i < static_cast<int>(devices.size()); i++) {
                if (!devices[i] || !devices[i]->dev) continue;
                const auto& d = devices[i];
                int64_t t, prev;
                do {  // prev/last are two stores: re-read until the pair is consistent
                    t = d->last_arrival_ns.load(std::memory_order_acquire);
                    prev = d->prev_arrival_ns.load(std::memory_order_acquire);
                } while (d->last_arrival_ns.load(std::memory_order_acquire) != t);
                lo = std::min(lo, t); hi = std::max(hi, t);
                const int64_t gap = t - prev;
                if (prev != 0 && (gap < period / 2 || gap > period + period / 2)) steady = false;
            }
            // No device delivered for two periods: the stream is stopped
            // (readers joined / not started yet) - nothing can straddle.
            // Two, not one: a round up to 1.5 periods late is normal jitter
            // (see `steady`), and flushing as it lands would slip a channel.
            const bool stopped = hi != 0 && steady_now_ns() - hi > 2 * period;
            quiet = hi == 0 || stopped ||
                    (steady && hi - lo < period / 2 && steady_now_ns() - hi < period / 2);
            if (!quiet) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        found_quiet = quiet;
        if (!quiet && warn) l1_unquiet_flush_count.fetch_add(1, std::memory_order_relaxed);
        if (!quiet && warn) {
            static int64_t last_warn_ns = 0;  // flush callers are serialized by the drain
            const int64_t now = steady_now_ns();
            if (now - last_warn_ns > 10'000'000'000LL) {
                last_warn_ns = now;
                std::cerr << "L1 flush: no quiet point between USB packet rounds (uneven delivery) - "
                             "flushing anyway, a channel may slip by a packet" << std::endl;
            }
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
    return found_quiet;
}

// The sweep's gate, on the drain (the only L1 consumer): for each active
// dongle, drop every packet that can hold a sample taken before `from_ns`
// (T: the hop's retune end + settle), and nothing it does not have to.
//
// What a packet holds is decided from when its samples were TAKEN, not from
// when it happened to arrive. Packet n of a stream ends at E_n (its last
// sample) and arrives at A_n = E_n + L_n, with a latency L_n >= 0 (never
// negative; large for a packet a held-up reader delivers late or in a
// burst). In the dongle's own time its full packets end one period P apart:
// E_s - E_n >= (s - n) P (1 - eps) for s > n - samples the RTL2832 loses
// (a FIFO overflow) and a packet the callback drops only make the
// difference larger (a SHORT packet would make it smaller: the line starts
// again after one, sdr_device.cpp); eps is the sample clock's error against
// the host clock (crystal, <= 100 ppm). So for every packet n <= s:
//     E_s >= A_n + (s - n) P - L_n - d,   d <= LINE_PACKETS P eps (33 us).
// The earliest-arrival line through the stamps (the callback numbers and
// stamps every packet, sdr_device.hpp)
//     f(s) = min over the LINE_PACKETS packets n = s - W + 1 .. s of (A_n + (s - n) P)
// is at most every one of those terms, hence
//     E_s >= f(s) - L* - d,   L* = min over the same packets of L_n:
// the fastest delivery among them. Which is small by construction: a
// stalled reader delivers at most RTL_USB_BUF_COUNT packets late (the
// transfers that completed while it stood; later samples are lost in the
// FIFO), and the window is longer (W = RTL_USB_BUF_COUNT + 16) and complete
// - all W packets readable and none before a stream (re)start or a short
// transfer, else f(s) counts as "before T" -, so it always holds packets a
// running reader delivered: L* is the USB transfer's floor (~1 ms on a PC).
// Not bounded by anything here: a reader that runs steadily behind (every
// packet of the window late) - then L* is that lateness, and the settle has
// to cover it. The gate drops every packet up to and including the first s
// with f(s) >= T, per dongle, and keeps s + 1 on; the first sample of
// packet s + 1 is the one after E_s, so
//     every sample of a kept packet was taken at or after T - L* - d.
// f is non-decreasing (stamps are in order), so "the first s" is found by
// a scan; all packets with f < T are dropped as they arrive. Indices: the
// head of L1 is l1_put - l1_buffer_size, read in that order (the callback
// counts the size first): never later than the true head, so a race drops
// one packet more, never one too few. Allocation-free; the callback only
// stamps.
// Outcome: GATE_DONE; GATE_STALLED - a dongle delivered nothing for
// GATE_STALL_NS (gate_stalled_mask says which; after
// STUCK_DEVICE_MAX_TIMEOUTS x 100 ms of silence it is reported as the
// drain's stuck device, as outside the gate); GATE_SLOW - packets arrive but
// the line hasn't passed T within GATE_MAX_NS (gate_slow_mask: lost
// packets, a stream restart's gap - the line catches up after W packets);
// GATE_LEFT - the mode was left. After a failed gate the drain keeps every
// L1 empty until the next gate or the mode is left (gate_hold).
enum GateOutcome { GATE_DONE, GATE_STALLED, GATE_SLOW, GATE_LEFT };
static constexpr int GATE_LINE_PACKETS = SDRDevice::LINE_PACKETS;
static_assert(GATE_LINE_PACKETS > RTL_USB_BUF_COUNT, "the gate's window must outlast a stalled reader's backlog");
static_assert(SDRDevice::ARRIVAL_RING >= 2 * GATE_LINE_PACKETS, "the stamp ring must hold two windows");
static constexpr int64_t GATE_STALL_NS = 100'000'000;
static constexpr int64_t GATE_MAX_NS = 400'000'000;
// a dongle past its straddler keeps at most this many packets while the gate
// waits for another (no L1 can approach its overflow during a gate)
static constexpr size_t GATE_KEEP_PACKETS = 16;
static std::atomic<uint32_t> gate_stalled_mask{0};
static std::atomic<uint32_t> gate_slow_mask{0};
// after a failed gate (sweep mode): the drain drops every packet of every
// dongle until the next gate is served or the mode is left
static std::atomic<bool> gate_hold{false};
static std::atomic<uint32_t> gate_late_count{0};
static std::atomic<uint32_t> gate_failed_count{0};
uint32_t sweep_gate_stalled() { return gate_stalled_mask.load(std::memory_order_relaxed); }
uint32_t sweep_gate_slow() { return gate_slow_mask.load(std::memory_order_relaxed); }
void sweep_gate_reset_diagnostics() {
    gate_stalled_mask.store(0, std::memory_order_relaxed);
    gate_slow_mask.store(0, std::memory_order_relaxed);
}
uint32_t sweep_gates_late() { return gate_late_count.load(std::memory_order_relaxed); }
uint32_t sweep_gates_failed() { return gate_failed_count.load(std::memory_order_relaxed); }

// Arrival stamp of packet n of `d` (0: not in the ring any more / being written)
static int64_t arrival_of(const SDRDevice& d, uint64_t n) {
    const size_t slot = static_cast<size_t>(n % SDRDevice::ARRIVAL_RING);
    const uint64_t q1 = d.ring_n[slot].load(std::memory_order_acquire);
    const int64_t t = d.ring_t[slot].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t q2 = d.ring_n[slot].load(std::memory_order_relaxed);
    return (q1 == n && q2 == n) ? t : 0;
}

// f(s): the earliest-arrival line at packet s over its whole window;
// INT64_MIN ("before T") if the window is not complete: fewer than W packets
// since the stream's first / a short one, or a stamp not readable (the ring
// overwrote it - the packet is that old)
static int64_t line_at(const SDRDevice& d, uint64_t s, double period_ns) {
    const uint64_t first = d.stream_first.load(std::memory_order_relaxed);
    if (s + 1 < first + GATE_LINE_PACKETS) return INT64_MIN;
    int64_t f = INT64_MAX;
    for (uint64_t n = s + 1 - GATE_LINE_PACKETS; n <= s; n++) {
        const int64_t a = arrival_of(d, n);
        if (a == 0) return INT64_MIN;
        f = std::min(f, a + static_cast<int64_t>(static_cast<double>(s - n) * period_ns));
    }
    return f;
}

static GateOutcome serve_gate_now(int num_elements, int64_t from_ns) {
    const double period_ns = 1e9 * NUM_SAMPLES / SAMPLE_RATE;
    const int64_t start = steady_now_ns();
    const int n = std::min({num_elements, static_cast<int>(devices.size()), NUM_DEVICES});
    bool done[NUM_DEVICES] = {};
    uint64_t head[NUM_DEVICES] = {};   // number of the packet at the head of L1
    for (int i = 0; i < n; i++) {
        if (!devices[i] || !devices[i]->dev) { done[i] = true; continue; }
        const uint64_t put = devices[i]->l1_put.load(std::memory_order_acquire);
        const size_t size = devices[i]->l1_buffer_size.load(std::memory_order_relaxed);
        head[i] = put >= size ? put - size : 0;
    }
    auto drop = [](SDRDevice& d, uint64_t count) {
        SampleBuffer discard;
        for (uint64_t k = 0; k < count && d.l1_buffer.try_dequeue(discard); k++) {
            d.l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);
            d.sample_pool.release(std::move(discard));
        }
    };
    for (;;) {
        if (operating_mode.load() != OperatingMode::SWEEP || !global_running.load()) return GATE_LEFT;
        bool all = true;
        uint32_t silent = 0;
        const int64_t now = steady_now_ns();
        for (int i = 0; i < n; i++) {
            if (done[i]) {
                // past its straddler: what it delivers now is valid, but
                // while another dongle holds the gate up it piles up -
                // keep the newest GATE_KEEP_PACKETS (later packets are as
                // valid as earlier ones; sweep mode pairs nothing)
                const size_t depth = devices[i]->l1_buffer_size.load(std::memory_order_relaxed);
                if (depth > GATE_KEEP_PACKETS) drop(*devices[i], depth - GATE_KEEP_PACKETS);
                continue;
            }
            SDRDevice& d = *devices[i];
            const uint64_t put = d.l1_put.load(std::memory_order_acquire);
            // the first packet s >= head - 1 with f(s) >= T among those
            // arrived: packets before the head are gone already (the
            // partial set the drain held when it served the gate - the one
            // that woke it -, sets before it are flushed downstream), so if
            // the last of them ends past T nothing more goes
            uint64_t s = head[i] > 0 ? head[i] - 1 : 0;
            int64_t f = INT64_MIN;
            for (; s < put; s++) {
                f = line_at(d, s, period_ns);
                if (f >= from_ns) break;
            }
            if (s < put) {
                if (s >= head[i]) drop(d, s + 1 - head[i]);
                head[i] = s + 1;
                done[i] = true;
                // a straddler delivered more than half a period after the line
                if (const int64_t a = arrival_of(d, s); a && a - f > static_cast<int64_t>(period_ns / 2))
                    gate_late_count.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            drop(d, put - head[i]);   // all of them before T's packet: they go in any case
            head[i] = put;
            all = false;
            const int64_t last = d.last_arrival_ns.load(std::memory_order_acquire);
            if (now - std::max(last, start) > GATE_STALL_NS) silent |= 1u << i;
            // the drain's stuck-device alarm (its wait loop is not reached
            // while a gate is pending)
            static int64_t last_stuck_ns = 0;   // (the drain thread only)
            if (last && now - last > STUCK_DEVICE_MAX_TIMEOUTS * 100'000'000LL &&
                now - last_stuck_ns > STUCK_DEVICE_MAX_TIMEOUTS * 100'000'000LL) {
                last_stuck_ns = now;   // re-alerted as often as the drain's own alarm
                signal_coherence_lost("sample drain: a device stalled (no data)");
            }
        }
        if (all) {
            gate_stalled_mask.store(0, std::memory_order_relaxed);
            gate_slow_mask.store(0, std::memory_order_relaxed);
            return GATE_DONE;
        }
        uint32_t pending = 0;
        for (int i = 0; i < n; i++) if (!done[i]) pending |= 1u << i;
        if (silent || now - start > GATE_MAX_NS) {
            // failed: the hop thread tries again; until then nothing of
            // this hop can be kept - every L1 stays empty (gate_hold)
            gate_stalled_mask.store(silent, std::memory_order_relaxed);
            gate_slow_mask.store(pending & ~silent, std::memory_order_relaxed);
            gate_failed_count.fetch_add(1, std::memory_order_relaxed);
            gate_hold.store(true, std::memory_order_relaxed);
            return silent ? GATE_STALLED : GATE_SLOW;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// One read of everything the conversion applies set-wide (see SetConversionState).
static void snapshot_conversion_state(SetConversionState& st, int num_elements) {
    // Forward (feed-forward) S2P compensation: removes the differential through-
    // response (S21) of the per-channel RF chain (cable / filter / LNA) that
    // sits in FRONT of the KrakenSDR, OUTSIDE the noise-source calibration loop.
    // It is folded into the SAME per-chunk scalar as the closed-loop phase vector
    // (zero per-sample cost) and kept in a SEPARATE vector so it survives the
    // identity resets the closed-loop vector undergoes on every recalibration /
    // retune. Off by default; gated like the per-bin EQ (enabled relaxed, then
    // ready acquire).
    // NOT applied while the noise source is on: the noise is injected inside
    // the KrakenSDR, after the external chain, so the calibration must measure
    // the internal path alone. Applied during calibration, the closed-loop
    // vector measured it too and cancelled its phase (only |fwd| survived).
    const bool fwd = forward_comp.enabled.load(std::memory_order_relaxed) &&
                     forward_comp.ready.load(std::memory_order_acquire) &&
                     !bias_tee_enabled.load(std::memory_order_relaxed);
    // Independent / sweep mode: every tuner is its own receiver - no array
    // correction
    const OperatingMode mode = operating_mode.load(std::memory_order_relaxed);
    const bool independent = mode == OperatingMode::INDEPENDENT || mode == OperatingMode::SWEEP;
    for (int ch = 0; ch < num_elements && ch < NUM_DEVICES; ch++) {
        if (independent) { st.comp[ch] = Complex(1.0f, 0.0f); continue; }
        // Lock-free atomic load - no mutex
        st.comp[ch] = phase_compensation->compensation_vector.load(ch);
        if (fwd) st.comp[ch] *= forward_comp.vector.load(ch);
    }
    // Wideband variant, high-side injection (LO = IF + RF): the mixer hands us
    // the conjugated baseband - mirrored spectrum, negated steering phases.
    // Un-mirror it at the source by negating Q, so our own correlation/phase
    // cal, the TCP broadcast and the DoA client all see the signal upright
    // with the true RF phase relationships.
    st.spectral_inversion = downconverter.spectral_inversion_active();
    st.eq_active = per_bin_cal.enabled.load(std::memory_order_relaxed) &&
                   per_bin_cal.ready.load(std::memory_order_acquire);
}

void samples_to_complex_with_compensation(const uint8_t* samples, int count, int channel,
                                          const SetConversionState& st, ComplexBuffer& out) {
    // Validate channel index
    if (channel < 0 || channel >= NUM_DEVICES) {
        std::cerr << "ERROR: Invalid channel " << channel
                  << " in samples_to_complex_with_compensation" << std::endl;
        out.assign(count, Complex(0.0f, 0.0f));
        return;
    }

    const Complex comp = st.comp[channel];  // phase vector x forward comp (per set)

    static thread_local ComplexBuffer tmp;
    tmp.resize(count);

    const uint8_t* __restrict p   = samples;
    Complex*      __restrict dst  = tmp.data();
    const float*  __restrict lut  = iq_lut();

    const float cr = comp.real();
    const float ci = comp.imag();

    // High-side spectral inversion: negate Q. One multiply per sample, folded
    // into the auto-vectorized loop.
    const float qs = st.spectral_inversion ? -1.0f : 1.0f;

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
    const bool eq_active = st.eq_active;
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
        // Body: pure convolution over the interleaved floats (real taps, so I
        // and Q filter alike: float k's tap n is float k - 2n). Written on
        // floats because clang doesn't vectorize the std::complex form (6x
        // slower on macOS); one statement per multiply and per add, in the
        // std::complex loop's order, so no FMA is contracted and the output
        // stays bit-identical to it.
        {
            const float* __restrict xf = reinterpret_cast<const float*>(x);
            float* __restrict yf = reinterpret_cast<float*>(y);
            const float h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3];
            const float h4 = h[4], h5 = h[5], h6 = h[6], h7 = h[7];
            for (int k = 2 * head; k < 2 * count; k++) {
                float a = 0.0f, m;
                m = h0 * xf[k];      a += m;
                m = h1 * xf[k - 2];  a += m;
                m = h2 * xf[k - 4];  a += m;
                m = h3 * xf[k - 6];  a += m;
                m = h4 * xf[k - 8];  a += m;
                m = h5 * xf[k - 10]; a += m;
                m = h6 * xf[k - 12]; a += m;
                m = h7 * xf[k - 14]; a += m;
                yf[k] = a;
            }
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

static void flush_downstream();

// Startup flush, once per pipeline start, before the drain's first set: as
// soon as every active stream has delivered two packets, flush at a quiet
// point (flush_l1_now). Without it the drain paired each stream's buffers by
// count from that stream's own first packet, so a stream whose first transfer
// went out late started that many samples behind - beyond the correlator's
// +-NUM_SAMPLES when a reader was held up (one CPU core, a slow buffer
// mapping): no lag peak at all. Flushed at a quiet point, the buffers pair by
// arrival time, which keeps the startup lag within a packet (about half a
// packet when the streams' packet times lie within half a period); smaller
// lags are left as they are (docs/dev/calibration.md, *Stream start*). Two packets, so the quiet point's inter-arrival check has a
// gap to look at. A stream silent for STARTUP_FLUSH_WAIT_MS: no flush - the
// stuck-device check handles it.
static void startup_flush(int num_elements) {
    const int64_t start = steady_now_ns();
    num_elements = std::min(num_elements, std::min(static_cast<int>(devices.size()), NUM_DEVICES));
    // Each stream's first packet: last_arrival_ns holds it until the second
    // arrives a packet period later. The spread is the startup lag (first lag
    // ~= -2.4 x difference in us), logged for checking a machine's start.
    int64_t first[NUM_DEVICES] = {};
    for (;;) {
        if (!global_running || !pipeline_running.load(std::memory_order_acquire)) return;
        bool all = true;
        for (int i = 0; i < num_elements; i++) {
            if (!devices[i] || !devices[i]->dev) continue;
            if (!first[i] && devices[i]->prev_arrival_ns.load(std::memory_order_acquire) == 0)
                first[i] = devices[i]->last_arrival_ns.load(std::memory_order_acquire);
            if (devices[i]->prev_arrival_ns.load(std::memory_order_acquire) == 0) all = false;
        }
        if (all) break;
        if (steady_now_ns() - start > STARTUP_FLUSH_WAIT_MS * 1'000'000LL) {
            std::cerr << "Sample drain: a USB stream delivered nothing within " << STARTUP_FLUSH_WAIT_MS
                      << " ms of the start - no startup flush" << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    // A slip at a non-quiet point is harmless here - nothing is calibrated yet
    flush_l1_now(num_elements, true, false);
    flush_downstream();

    int64_t lo = INT64_MAX;
    for (int i = 0; i < num_elements; i++)
        if (first[i]) lo = std::min(lo, first[i]);
    std::string per;
    for (int i = 0; i < num_elements; i++) {
        if (!devices[i] || !devices[i]->dev) continue;
        per += (per.empty() ? "" : " ") + (first[i] ? std::to_string((first[i] - lo) / 1000) : std::string("?"));
    }
    std::cout << "Sample drain: first packets at " << per << " us (channel order), startup flush done" << std::endl;
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
    // A pending sweep gate first, then a flush: a flush filed meanwhile (the
    // re-pairing one on the way out of sweep mode) is served after the gate,
    // so it is the last word. A gate still pending once the mode was left
    // (its requester gave up) drops nothing from L1 - but when it is served
    // mid-collection, the partial set (devices 0 .. i) is still dropped: a
    // one-packet slip of those channels, which the paired flush on every
    // way back to coherent repairs.
    auto serve_flush = [](int n) {
        bool served = false;
        if (const uint32_t g = gate_requests.load(std::memory_order_acquire);
            g != gate_completed.load(std::memory_order_relaxed)) {
            gate_hold.store(false, std::memory_order_relaxed);   // the gate itself drains (or sets it again)
            const GateOutcome o = operating_mode.load() != OperatingMode::SWEEP
                                      ? GATE_LEFT : serve_gate_now(n, gate_from_ns.load(std::memory_order_relaxed));
            gate_last_outcome.store(o, std::memory_order_relaxed);
            gate_completed.store(g, std::memory_order_release);
            served = true;
        }
        const uint32_t req = flush_requests.load(std::memory_order_acquire);
        if (req == flush_completed.load(std::memory_order_relaxed)) return served;
        last_served_quiet.store(flush_l1_now(n, true), std::memory_order_relaxed);
        flush_completed.store(req, std::memory_order_release);
        return true;
    };

    startup_flush(std::min(active_num_elements.load(), static_cast<int>(devices.size())));

    while (global_running && pipeline_running.load(std::memory_order_acquire)) {
        // Re-read per pass: the thread is restarted by a reconfiguration, but a
        // stale count here would index closed channels for a whole run.
        const int num_elements = std::min(active_num_elements.load(),
                                          static_cast<int>(devices.size()));

        // Outer vector recycled from the pool; inner buffers are filled by the
        // L1 dequeues below (each owned by its device's sample_pool).
        serve_flush(num_elements);

        // Sweep mode after a failed gate (a dongle silent or its line short
        // of T): nothing can be kept until the next gate, so every L1 is
        // emptied as packets come - none can overflow while the gate is
        // retried, however long that takes (serve_gate_now)
        if (gate_hold.load(std::memory_order_relaxed)) {
            if (operating_mode.load() != OperatingMode::SWEEP) {
                gate_hold.store(false, std::memory_order_relaxed);
            } else {
                for (int i = 0; i < num_elements; i++) {
                    if (!devices[i]) continue;
                    SampleBuffer discard;
                    while (devices[i]->l1_buffer.try_dequeue(discard)) {
                        devices[i]->l1_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                        devices[i]->sample_pool.release(std::move(discard));
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }
        }

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
            // Sweep mode waits in 5 ms slices: a hop's gate is filed while the
            // drain may be waiting here for a silent dongle, and every other
            // dongle's L1 fills until it notices (100 ms slices elsewhere;
            // the stuck-device alarm counts the time waited either way)
            const int slice_ms = operating_mode.load(std::memory_order_relaxed) == OperatingMode::SWEEP ? 5 : 100;
            int waited_ms = 0;
            while (!device->l1_buffer.wait_dequeue_timed(raw_set[i], std::chrono::milliseconds(slice_ms))) {
                if (!global_running || !pipeline_running.load(std::memory_order_acquire)) {
                    recycle_partial(raw_set, i);
                    return;
                }
                if (l1_request_pending()) {
                    break;  // served below, with the partial set dropped
                }
                waited_ms += slice_ms;
                if (waited_ms >= STUCK_DEVICE_MAX_TIMEOUTS * 100) {
                    signal_coherence_lost("sample drain: a device stalled (no data)");
                    waited_ms = 0;  // re-alert periodically if it stays stuck
                }
            }
            if (l1_request_pending()) {
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
    // against normal-priority web/correlation threads. It does not need RT: if it falls
    // behind, whole aligned sets drop at L2-raw (coherence-safe) and only the
    // converted-output rate dips - USB keep-up is unaffected (that is the drain).
    set_thread_realtime("convert", 0);

    SetConversionState conv_state;
    while (global_running && pipeline_running.load(std::memory_order_acquire)) {
        // Read BEFORE the dequeue: a set dequeued before a flush must carry
        // the pre-flush epoch (see l2_flush_epoch)
        const uint32_t epoch = l2_flush_epoch.load(std::memory_order_acquire);
        std::vector<SampleBuffer> raw_set;
        // Sweep mode waits in short slices: a wait that spans a flush keeps
        // the epoch read before it, so the first set after every hop's flush
        // (the first valid one, a packet period later) was dropped as
        // "converted across a flush" - a whole packet of every hop.
        // Re-reading the epoch every 2 ms drops nothing that wasn't dequeued
        // after the flush (every dequeue still follows its own epoch read).
        const auto wait = operating_mode.load(std::memory_order_relaxed) == OperatingMode::SWEEP
                              ? std::chrono::milliseconds(SWEEP_CONVERT_WAIT_MS) : std::chrono::milliseconds(100);
        if (!l2_raw_buffer.wait_dequeue_timed(raw_set, wait)) {
            if (!global_running || !pipeline_running.load(std::memory_order_acquire)) return;
            continue;  // timeout: nothing to convert yet
        }
        // Matched with the drain's fetch_add on EVERY dequeue - including the
        // stop path below, which used to return first and leave a phantom +1
        // in the depth counter after each pipeline stop (element-count change).
        l2_raw_buffer_size.fetch_sub(1, std::memory_order_relaxed);
        if (!global_running || !pipeline_running.load(std::memory_order_acquire)) {
            recycle_raw_set(raw_set);
            return;
        }

        // -------- Convert (in place into the pooled set) --------
        // Channel count comes from the SET, not a captured constant: the drain
        // sizes each aligned set to the element count it collected under.
        const int num_elements = static_cast<int>(std::min(raw_set.size(), devices.size()));

        auto complex_samples = l2_buffer_pool.acquire();
        complex_samples.resize(num_elements);
        snapshot_conversion_state(conv_state, num_elements);
        for (int i = 0; i < num_elements; ++i) {
            auto& sb = raw_set[i];
            // sb.size() is bytes of interleaved IQ; divide by 2 for complex count
            samples_to_complex_with_compensation(
                sb.data(), static_cast<int>(sb.size()) / 2, devices[i]->index,
                conv_state, complex_samples[i]);
        }

        // -------- Broadcast + push to L2, unless flushed meanwhile --------
        // Under the flush mutex with the epoch re-checked, so a flush can't
        // land between the check and the handoff: a set converted across a
        // flush goes neither to the TCP taps nor to L2 (calibration). Safe to
        // broadcast under it: the data server reads the phase state with a
        // try_lock (get_phase_compensation_state_nonblocking), and its
        // clients_mutex / the RTL-TCP buffer lock never wait on a flush.
        {
            std::lock_guard<std::mutex> flush_lock(l2_flush_mutex);
            if (l2_flush_epoch.load(std::memory_order_relaxed) != epoch) {
                l2_buffer_pool.release(std::move(complex_samples));
            } else {
                // Sweep mode: count the sets sent as valid (retuning flag 0
                // before and after the header was built, so the header said
                // 0 too) - a hop's dwell lasts until one has gone out
                const bool sweep = operating_mode.load(std::memory_order_relaxed) == OperatingMode::SWEEP;
                // Auto gain's statistics: each tuner's frequency and gain read
                // BEFORE the flag (as the header does) - with the flag 0 then
                // and after the broadcast they are this set's (a hop stores
                // its new ones only after it set the flag)
                const bool stats = sweep && sweep_auto_gain_active();
                uint32_t st_hz[NUM_DEVICES];
                int st_gain[NUM_DEVICES];
                if (stats)
                    for (int i = 0; i < num_elements && i < NUM_DEVICES; i++) {
                        st_hz[i] = wideband_config.get_tuner_frequency(i);
                        st_gain[i] = wideband_config.get_tuner_gain(i);
                    }
                const bool valid_before = sweep && !sweep_state.retuning_in_progress.load();
                if (tcp_data_server) tcp_data_server->broadcast_data(complex_samples);
                if (rtl_tcp_server)  rtl_tcp_server->broadcast_data(complex_samples);
                if (valid_before && !sweep_state.retuning_in_progress.load()) {
                    // (before the count: the hop thread, woken by it, finds
                    // the dwell's statistics complete)
                    if (stats) sweep_note_hop_stats(raw_set, st_hz, st_gain, num_elements);
                    sweep_note_valid_set();
                }

                // Drop oldest if full - coherence-safe
                if (l2_buffer_size.load(std::memory_order_relaxed) >= BUFFER_SIZE) {
                    std::vector<ComplexBuffer> discard;
                    if (l2_buffer.try_dequeue(discard)) {
                        l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
                        l2_buffer_pool.release(std::move(discard));
                    }
                }
                l2_buffer.enqueue(std::move(complex_samples));
                l2_buffer_size.fetch_add(1, std::memory_order_relaxed);
            }
        }

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

// Everything downstream of L1: bump the flush epoch, empty L2-raw and L2.
static void flush_downstream() {
    std::lock_guard<std::mutex> flush_lock(l2_flush_mutex);
    clear_l2_raw_buffer();
    std::vector<ComplexBuffer> discard;
    while (l2_buffer.try_dequeue(discard)) {
        l2_buffer_size.fetch_sub(1, std::memory_order_relaxed);
        l2_buffer_pool.release(std::move(discard));  // Return to pool
    }
    // No store(0): see clear_l2_raw_buffer (avoids the size_t underflow race).
    // Epoch AFTER the clears: bumped first, a worker could read the new epoch
    // and still dequeue a stale set the clear hadn't reached yet
    l2_flush_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// quiet: nothing on stdout, and the "did not respond" fault throttled (the
// re-pairing flush on leaving sweep mode)
static bool clear_l1_buffer_impl(bool quiet) {
    if (!quiet) std::cout << "Clearing L1 buffers for all devices..." << std::endl;
    // The drain performs the flush (see flush_l1_now); wait for it. Flush
    // directly when there is no drain (pipeline stopped) or it doesn't answer.
    bool served = false, found_quiet = true;
    if (drain_active.load(std::memory_order_acquire) &&
        drain_thread_id.load() != std::this_thread::get_id()) {
        const uint32_t req = flush_requests.fetch_add(1, std::memory_order_acq_rel) + 1;
        for (int waited_ms = 0; waited_ms < 500 && !served; waited_ms++) {
            if (static_cast<int32_t>(flush_completed.load(std::memory_order_acquire) - req) >= 0) {
                served = true;
                found_quiet = last_served_quiet.load(std::memory_order_relaxed);
                break;
            }
            if (!drain_active.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!served) {
            // throttled against the last LOG line (an exchange on every
            // occurrence kept a fault recurring faster than 10 s silent)
            static std::atomic<int64_t> last_warn_ns{-20'000'000'000LL};
            const int64_t now = steady_now_ns();
            const bool log = !quiet || now - last_warn_ns.load() > 10'000'000'000LL;
            if (log) last_warn_ns.store(now);
            if (log) std::cerr << "L1 flush: sample drain did not respond - flushing directly" << std::endl;
        }
    }
    if (!served) found_quiet = flush_l1_now(active_num_elements.load(), true);
    // AFTER the L1 flush: a set the drain completed before serving it went
    // into L2-raw, and anything already downstream is older still
    flush_downstream();
    return found_quiet;
}

void clear_l1_buffer() { clear_l1_buffer_impl(false); }
bool clear_l1_buffer_paired() { return clear_l1_buffer_impl(true); }

void sweep_gates_quiesce() {
    // (the mode has left SWEEP and no gate can be filed any more: the
    // caller holds sweep.cpp's flush_mutex) Every gate filed is served by
    // the drain before this returns - a stale one (its requester gave up
    // when the mode was left) is completed without touching L1 but still
    // drops the partial set it interrupted - so the re-pairing flush that
    // follows is the last thing to touch the pairing. No drain: nothing to
    // serve them, nothing streams.
    gate_hold.store(false, std::memory_order_relaxed);
    const int64_t give_up = steady_now_ns() + 1'000'000'000LL;
    while (gate_completed.load(std::memory_order_acquire) != gate_requests.load(std::memory_order_acquire)) {
        if (!drain_active.load(std::memory_order_acquire) || steady_now_ns() > give_up) {
            if (drain_active.load(std::memory_order_acquire))
                std::cerr << "Sweep: the sample drain did not serve a pending gate within 1 s of leaving sweep mode"
                          << std::endl;
            gate_completed.store(gate_requests.load(std::memory_order_acquire), std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

int sweep_gate_l1(int64_t from_ns) {
    int result = -2;   // 1 done, 0 not (a dongle stalled, too slow, no answer), -1 mode left, -2 no drain
    if (drain_active.load(std::memory_order_acquire) && drain_thread_id.load() != std::this_thread::get_id()) {
        gate_from_ns.store(from_ns, std::memory_order_relaxed);
        const uint32_t req = gate_requests.fetch_add(1, std::memory_order_acq_rel) + 1;
        // the drain starts it at its next packet (or its 100 ms wait's end);
        // the gate itself ends within GATE_MAX_NS
        const int64_t give_up = std::max(steady_now_ns(), from_ns) + 600'000'000LL;
        for (;;) {
            if (static_cast<int32_t>(gate_completed.load(std::memory_order_acquire) - req) >= 0) {
                const int o = gate_last_outcome.load(std::memory_order_relaxed);
                result = o == GATE_DONE ? 1 : o == GATE_LEFT ? -1 : 0;
                break;
            }
            if (!drain_active.load(std::memory_order_acquire)) { result = -2; break; }
            // leaving the mode: don't wait (the drain completes it without effect)
            if (operating_mode.load() != OperatingMode::SWEEP) { result = -1; break; }
            if (steady_now_ns() > give_up) {
                result = 0;
                gate_failed_count.fetch_add(1, std::memory_order_relaxed);
                static std::atomic<int64_t> last_warn_ns{-20'000'000'000LL};
                const int64_t now = steady_now_ns();
                if (now - last_warn_ns.load() > 10'000'000'000LL) {
                    last_warn_ns.store(now);
                    std::cerr << "Sweep: the sample drain did not serve a hop's gate in 600 ms" << std::endl;
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    if (result == -2) {
        // no drain (the pipeline stopped or not started): nothing streams,
        // so nothing can straddle - empty L1 directly
        flush_l1_now(active_num_elements.load(), true, false);
        result = 1;
    }
    // AFTER L1, as every flush: what the drain completed before the gate is
    // in L2-raw or in the conversion worker's hands
    flush_downstream();
    return result;
}

void clear_l2_buffer() {
    std::cout << "Clearing entire L2 buffer..." << std::endl;
    flush_downstream();  // L2-raw too, so it can't refill L2
}

void clear_l2_buffer_quiet() { flush_downstream(); }
