// Sweep mode: the hop sequence, the plan and its status (sweep.hpp).
//
// One hop (every tuner of the round together):
//   retuning_in_progress = 1 -> downstream flush -> retune (parallel,
//   for_each_tuner; sweep_tune: the library's fast path when it has one) ->
//   settle -> gate (sweep_gate_l1: each dongle drops the packets that can
//   hold a sample from before the settle's end) + downstream flush ->
//   retuning_in_progress = 0 -> dwell: sweep_hop_packets(dwell_ms) valid
//   sets out on 8091, then at once the next hop. When the sweep stops moving
//   tuners (mode left, paused, a tuner out of the plan or on its only hop,
//   the pipeline down), finish_fast_tunes ends their fast tunes.
//
// The guarantee the client builds on: a packet with retuning_in_progress = 0
// holds only samples taken at the header's per-channel frequencies, after the
// settle. Why it holds:
//  - The flag is stored (seq_cst) before any tuner frequency changes, and the
//    new frequencies are stored (seq_cst) only after the tuner took them. The
//    8091 header reads the per-channel frequencies FIRST and the flag after
//    (tcp_data_server.cpp): a header that read a new frequency then reads the
//    flag as 1 (or a later 0 - see the next point).
//  - The header is built while the conversion worker holds l2_flush_mutex,
//    with the set's flush epoch unchanged since before the set was dequeued
//    (sdr_pipeline.cpp). The gate's downstream flush takes that mutex too,
//    and the flag is cleared only after it: a header that reads the flag as
//    0 after hop h's clear was built after hop h's gate had completed, so its
//    set was dequeued after the gate - and the drain collected it after the
//    gate too (it drops its partial set when it serves one; sets it finished
//    before are flushed downstream). So each of its packets is one the gate
//    kept: of a dongle's packets numbered past the first s whose
//    earliest-arrival line f(s) reached T (retune end + settle). The
//    frequencies it read are hop h's (h + 1's would make it read the flag
//    as 1: they are stored after h + 1 set the flag).
//  - The bound (proof at serve_gate_now, sdr_pipeline.cpp): every sample of
//    a kept packet was taken at or after T - L* - 33 us, L* = the fastest
//    USB delivery among that dongle's last RTL_USB_BUF_COUNT + 16 packets.
//    The window outlasts any backlog a stalled reader can deliver (at most
//    RTL_USB_BUF_COUNT packets), so it always holds packets a running
//    reader delivered and L* is the USB floor (~1 ms on a PC) - unless the
//    reader runs steadily behind (every packet of the window late), which
//    nothing here bounds. So the samples see SWEEP_SETTLE_MS minus L*: the
//    settle has to cover the host's USB latency (the contrast test, docs).
//  - A gain change in sweep mode (sweep_note_tuner_change) and the mode's
//    entry set the flag the same way; whenever hopping stops (paused, plan
//    not runnable) the thread finishes the settle and the gate before it
//    clears the flag. The flag stays 1 only while a dongle delivers nothing
//    (no set can be formed then anyway): status "stalled_tuners", a log
//    line every 10 s, the round and the counter stand still, and the gate is
//    retried until the stream is back.
//
// Timing (docs/dev/operating-modes.md, "Hop timing"): a hop costs the retune,
// the packet that straddles its end (+ settle), and the dwell's packets -
// nothing else waits: the gate needs no cross-dongle quiet point, the dwell
// ends with its last packet, the next retune starts then.
//
// No output per hop (a sweep makes ~30 hops a second): failed retunes are
// counted and logged at most every 10 s. The flushes are the quiet variants
// and never count as coherence losses.

#include "sweep.hpp"
#include "for_each_tuner.hpp"
#include "sdr_init.hpp"
#include "sdr_pipeline.hpp"
#include "pipeline_control.hpp"
#include "../core/config.hpp"
#include "../core/settings.hpp"
#include <rtl-sdr.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace {

std::mutex wake_mutex;
std::condition_variable wake_cv;
// Serializes the sweep's L1 flushes with leaving the mode: after
// sweep_quiesce_flushes() no sweep flush can still run (each re-checks the
// mode under it), so the re-pairing flush on the way back to coherent is the
// last one (set_operating_mode). Never held while taking settings_mutex.
std::mutex flush_mutex;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool in_sweep() { return operating_mode.load() == OperatingMode::SWEEP; }

// Tuners whose last tune was the fast path (bit per channel): their PLL
// autotune clock stays at 128 kHz until finish_fast_tunes. Written under
// settings_mutex + device_io_mutex.
std::atomic<uint32_t> fast_pending{0};

// THE tune call of a hop (one tuner; on its own thread on Linux, under
// settings_mutex + device_io_mutex): true = the tuner took `hz`. Kept in this
// one place so the library's call can be swapped. The fork's fast path where
// it has one (worker F, 2026-11-21: KEEP_AUTOTUNE leaves the 128 kHz
// autotune clock - 4 USB transfers instead of 6 -, NO_DUMMY_READ skips the
// dummy read - 3; the PLL lock is still checked); a failure retries once on
// the full path (as the wideband drag's fallback does). `fast` = the tuner
// was left on the fast path's state (finish_fast_tunes ends it).
bool sweep_tune(rtlsdr_dev_t* dev, uint32_t hz, bool& fast) {
    fast = false;
#if defined(RTLSDR_TUNE_NO_DUMMY_READ)
    if (rtlsdr_set_center_freq_fast(dev, hz, RTLSDR_TUNE_KEEP_AUTOTUNE | RTLSDR_TUNE_NO_DUMMY_READ) >= 0) {
        fast = true;
        return true;
    }
#elif defined(RTLSDR_TUNE_KEEP_AUTOTUNE)
    if (rtlsdr_set_center_freq_fast(dev, hz, RTLSDR_TUNE_KEEP_AUTOTUNE) >= 0) {
        fast = true;
        return true;
    }
#else
    if (rtlsdr_set_center_freq(dev, hz) >= 0) return true;
#endif
    if (rtlsdr_set_center_freq(dev, hz) >= 0) return true;   // the full path (lock check, 8 kHz autotune)
#ifdef RTLSDR_TUNE_KEEP_AUTOTUNE
    // both failed: the driver may have left the 128 kHz autotune clock
    // pending (it is set before the lock check, cleared only by the 8 kHz
    // write) - counted as pending, so the finish runs for this tuner too
    fast = true;
#endif
    return false;
}

// The tuners in `mask` whose fast tunes are pending: rtlsdr_finish_tune (0 =
// done, no transfer if nothing is pending; 1 = PLL not locked, < 0 = I/O
// error -> their frequency once more on the full path). Under settings_mutex.
// in_mode: still in sweep mode - the writes are covered by the 8091 flag
// (on before, the settle + gate after, as a gain change), and a tuner whose
// full retune fails gets frequency 0. Out of the mode the next mode owns the
// frequencies (the return to coherent retunes every tuner on the full path
// after this anyway).
void finish_fast_tunes(uint32_t mask, bool in_mode, const char* why) {
    mask &= fast_pending.load();
    if (!mask) return;
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    mask &= fast_pending.load();
    if (!mask) return;
    fast_pending.fetch_and(~mask);
    if (in_mode) sweep_note_tuner_change();
    int idx[NUM_DEVICES], rc[NUM_DEVICES] = {};
    char fixed[NUM_DEVICES] = {};
    uint32_t f_of[NUM_DEVICES] = {};
    size_t k = 0;
    for (int t = 0; t < NUM_DEVICES && t < static_cast<int>(devices.size()); t++)
        if ((mask & (1u << t)) && devices[t] && devices[t]->dev) idx[k++] = t;
    for_each_tuner(k, [&](size_t i) {
        rtlsdr_dev_t* dev = devices[idx[i]]->dev;
#ifdef RTLSDR_TUNE_KEEP_AUTOTUNE
        rc[i] = rtlsdr_finish_tune(dev);
#endif
        if (rc[i] == 0) return;
        uint32_t f = wideband_config.get_tuner_frequency(idx[i]);
        if (!f) f = rtlsdr_get_center_freq(dev);
        f_of[i] = f;
        fixed[i] = f != 0 && (rtlsdr_set_center_freq(dev, f) >= 0 || rtlsdr_set_center_freq(dev, f) >= 0);
    });
    if (in_mode) sweep_note_tuner_change_done();
    for (size_t i = 0; i < k; i++) {
        if (rc[i] == 0) continue;
        std::cerr << "Sweep: tuner " << idx[i] << (rc[i] == 1 ? " PLL not locked" : " I/O error")
                  << " at the end of its fast retunes (" << why << ", " << rc[i] << ") - "
                  << (fixed[i] ? "retuned on the full path" : "full retune FAILED") << " at " << f_of[i] / 1e6
                  << " MHz" << std::endl;
        if (!fixed[i] && in_mode) {
            wideband_config.set_tuner_frequency(idx[i], 0);
            sweep_state.failed_mask.fetch_or(1u << idx[i]);
            sweep_state.failed_retunes.fetch_add(1);
        }
    }
}

// The sweep's gate (L1 per dongle, then downstream) for samples from
// `from_ns` on: 1 done, 0 not yet (a dongle delivered nothing, the stream
// isn't valid yet - try again), -1 nothing done (the mode was left). A
// stalled dongle is logged at once, then at most every 10 s.
int sweep_gate(int64_t from_ns) {
    std::lock_guard<std::mutex> lk(flush_mutex);
    if (!in_sweep()) return -1;
    const int r = sweep_gate_l1(from_ns);
    if (r == 0) {
        static int64_t last_log_ns = -20'000'000'000LL;
        const uint32_t m = sweep_gate_stalled(), slow = sweep_gate_slow();
        auto list = [](uint32_t mask) {
            std::string t;
            for (int i = 0; i < NUM_DEVICES; i++)
                if (mask & (1u << i)) t += (t.empty() ? "" : ",") + std::to_string(i);
            return t;
        };
        if (now_ns() - last_log_ns > 10'000'000'000LL) {
            last_log_ns = now_ns();
            if (m)
                std::cerr << "Sweep: tuner(s) " << list(m) << " delivered no data for 100 ms or more - no valid sweep "
                          "data (the 8091 flag stays 1) until every tuner streams again" << std::endl;
            else if (slow)
                std::cerr << "Sweep: tuner(s) " << list(slow) << " deliver, but their packets don't reach the hop's "
                          "start in 400 ms (packets lost?) - no valid sweep data (the 8091 flag stays 1)" << std::endl;
            else
                std::cerr << "Sweep: a hop's gate failed (the drain did not serve it) - the 8091 flag stays 1"
                          << std::endl;
        }
    }
    return r;
}

// Sleep until change_seq moves away from `seq`, at most `ns` (and at most
// 50 ms: global_running is cleared by a signal handler, which can't notify)
void wait_for_change(uint32_t seq, int64_t ns) {
    ns = std::clamp<int64_t>(ns, 0, 50'000'000);
    std::unique_lock<std::mutex> lk(wake_mutex);
    wake_cv.wait_for(lk, std::chrono::nanoseconds(ns),
                     [seq]() { return sweep_state.change_seq.load() != seq; });
}

// Whether the plan `plan_gen` still hops (not: the mode left, paused, the
// plan changed, exit); `p` gets the plan in force
bool still_hopping(uint32_t plan_gen, SweepPlan& p) {
    if (!global_running.load() || !in_sweep() || sweep_state.plan_generation.load() != plan_gen) return false;
    p = sweep_state.get_plan();
    return p.run;
}

// Wait until the settle (from the last retune - a gain change moves it) of
// the plan in force has passed; false as soon as this plan stops hopping. A
// settle change takes effect at once (re-read on every wake).
bool wait_settle(uint32_t plan_gen) {
    for (;;) {
        const uint32_t seq = sweep_state.change_seq.load();
        SweepPlan p;
        if (!still_hopping(plan_gen, p)) return false;
        const int64_t left = sweep_state.last_retune_ns.load() + static_cast<int64_t>(p.settle_ms) * 1'000'000 - now_ns();
        if (left <= 0) return true;
        wait_for_change(seq, left);
    }
}

// The dwell: until sweep_hop_packets(dwell_ms) sets have gone out as valid
// since `valid_from` (sweep_state.valid_sets, counted by the conversion
// worker - sweep_note_valid_set wakes this wait when the count is reached),
// or SWEEP_DWELL_GRACE_MS longer than they take from `start_ns` (a stalled
// stream must not stop the sweep). false as soon as this plan stops hopping.
// A dwell change takes effect at once.
std::atomic<uint32_t> valid_target{~0u};   // wake the dwell when valid_sets reaches it
std::atomic<int64_t> last_valid_ns{0};      // the last set counted valid (sweep_note_valid_set)
// A dwell in which no set has gone out for this long ends early: the stream
// stopped (a dongle silent - the drain then waits for it with every other
// dongle's L1 filling); the next hop's gate finds the stall and keeps every
// L1 empty (gate_hold)
constexpr int64_t DWELL_STALL_NS = 100'000'000;
bool wait_dwell(uint32_t plan_gen, uint32_t valid_from, int64_t start_ns) {
    const int64_t period_ns = static_cast<int64_t>(1e9 * NUM_SAMPLES / SAMPLE_RATE);
    for (;;) {
        const uint32_t seq = sweep_state.change_seq.load();
        SweepPlan p;
        if (!still_hopping(plan_gen, p)) { valid_target.store(~0u); return false; }
        const uint32_t want = sweep_hop_packets(p.dwell_ms);
        valid_target.store(valid_from + want);
        if (sweep_state.valid_sets.load() - valid_from >= want) { valid_target.store(~0u); return true; }
        const int64_t left = start_ns + want * period_ns + static_cast<int64_t>(SWEEP_DWELL_GRACE_MS) * 1'000'000 -
                             now_ns();
        if (left <= 0) { valid_target.store(~0u); return true; }
        const int64_t quiet = now_ns() - std::max(last_valid_ns.load(), start_ns);
        if (quiet > DWELL_STALL_NS + period_ns) { valid_target.store(~0u); return true; }
        std::unique_lock<std::mutex> lk(wake_mutex);
        wake_cv.wait_for(lk, std::chrono::nanoseconds(std::min<int64_t>(left, 20'000'000)), [&]() {
            return sweep_state.change_seq.load() != seq || sweep_state.valid_sets.load() - valid_from >= want;
        });
    }
}

// Hopping stopped (paused, the pipeline down) with the flag on: the tuners
// may have moved less than a settle ago - finish that settle and the gate
// before the stream counts as valid again.
void settle_and_clear_flag() {
    if (!sweep_state.retuning_in_progress.load()) return;
    int64_t settled_from = 0, from_ns = 0;
    for (;;) {
        if (!global_running.load() || !in_sweep()) return;
        settled_from = sweep_state.last_retune_ns.load();
        from_ns = settled_from + static_cast<int64_t>(sweep_state.get_plan().settle_ms) * 1'000'000;
        const int64_t left = from_ns - now_ns();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(left, 50'000'000)));
    }
    if (sweep_gate(from_ns) != 1) return;   // (again on the next pass)
    // Another change (a gain change, the mode left and entered again) since
    // the flush started moved last_retune_ns: leave the flag to the next
    // pass. Under settings_mutex, which every change that sets it holds.
    std::lock_guard<std::mutex> lock(settings_mutex);
    if (in_sweep() && sweep_state.last_retune_ns.load() == settled_from)
        sweep_state.retuning_in_progress.store(false);
}

// Failed retunes: counted; logged at once, then at most every 10 s
void report_failures(int failed, int tuner, uint64_t hz) {
    // (failed_retunes / failed_mask are updated by the hop itself)
    static int64_t last_log_ns = -20'000'000'000LL;
    static uint32_t unreported = 0;
    if (failed == 0) return;
    unreported += static_cast<uint32_t>(failed);
    const int64_t now = now_ns();
    if (now - last_log_ns < 10'000'000'000LL) return;
    last_log_ns = now;
    std::cerr << "Sweep: " << unreported << " tuner retune(s) failed twice (latest: tuner " << tuner << " to "
              << hz / 1e6 << " MHz) - their 8091 frequency is 0 (no valid data) until a retune succeeds"
              << std::endl;
    unreported = 0;
}

// ---- The hop timeline (status "timing": averages of the last TIMING_HOPS
// hops, reset when the plan, dwell or settle changes; no output per hop) ----
// Milliseconds from the hop's start unless noted
struct HopTiming {
    float flag = 0;         // the flag on + the downstream flush done
    float locked = 0;       // settings_mutex + device_io_mutex taken
    float retune = 0;       // for_each_tuner: every tuner of the round retuned (spawn + join)
    float tune_max = 0;     // the slowest single tuner's tune call (incl. a retry), ms
    float settle = 0;       // retune end -> the settle's end, ms
    float gate = 0;         // settle end -> the gate (L1 per dongle + downstream) returned, ms
    float clear = 0;        // gate returned -> the flag cleared, ms
    float first_valid = -1; // the flag cleared -> the first valid set broadcast, ms (-1: not seen)
    float dwell = 0;        // the flag cleared -> the dwell's end, ms
    float round = 0;        // the hop's start -> its dwell's end
    uint32_t valid = 0;     // sets broadcast as valid in the hop
    bool moved = false;     // a tuner was retuned (else the retune fields are 0)
    float gain_max = 0;     // auto gain: the slowest gain write of the hop, ms (0: none)
};
constexpr int TIMING_HOPS = 64;
std::mutex timing_mutex;
HopTiming timing_ring[TIMING_HOPS];
uint32_t timing_count = 0;          // recorded since the last reset
// the first valid set after a flag clear (stamped by sweep_note_valid_set)
std::atomic<uint32_t> first_valid_mark{0};
std::atomic<int64_t> first_valid_ns{0};

void timing_record(const HopTiming& h) {
    std::lock_guard<std::mutex> lk(timing_mutex);
    timing_ring[timing_count % TIMING_HOPS] = h;
    timing_count++;
}

void timing_reset() {
    std::lock_guard<std::mutex> lk(timing_mutex);
    timing_count = 0;
}

float ms_between(int64_t a, int64_t b) { return static_cast<float>(b - a) / 1e6f; }

std::string timing_json() {
    std::lock_guard<std::mutex> lk(timing_mutex);
    const uint32_t n = std::min<uint32_t>(timing_count, TIMING_HOPS);
    double flag = 0, locked = 0, retune = 0, tune_max = 0, settle = 0, gate = 0, clear = 0, fv = 0, dwell = 0,
           round = 0, valid = 0;
    uint32_t moved = 0, fv_n = 0, gain_n = 0;
    double gain = 0;
    float round_min = 0, round_max = 0;
    uint32_t valid_min = 0, valid_max = 0;
    for (uint32_t i = 0; i < n; i++) {
        const HopTiming& h = timing_ring[i];
        if (h.moved) {
            moved++;
            flag += h.flag; locked += h.locked; retune += h.retune; tune_max += h.tune_max;
        }
        settle += h.settle; gate += h.gate; clear += h.clear; dwell += h.dwell; round += h.round; valid += h.valid;
        if (h.first_valid >= 0) { fv += h.first_valid; fv_n++; }
        if (h.gain_max > 0) { gain += h.gain_max; gain_n++; }
        round_min = i ? std::min(round_min, h.round) : h.round;
        round_max = i ? std::max(round_max, h.round) : h.round;
        valid_min = i ? std::min(valid_min, h.valid) : h.valid;
        valid_max = i ? std::max(valid_max, h.valid) : h.valid;
    }
    auto avg = [](double s, uint32_t k) { return k ? s / k : 0.0; };
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"hops\":" << n << ",\"moved\":" << moved << ",\"flag_ms\":" << avg(flag, moved)
      << ",\"locked_ms\":" << avg(locked, moved) << ",\"retune_ms\":" << avg(retune, moved)
      << ",\"tune_max_ms\":" << avg(tune_max, moved) << ",\"settle_ms\":" << avg(settle, n)
      << ",\"gate_ms\":" << avg(gate, n) << ",\"clear_ms\":" << avg(clear, n)
      << ",\"first_valid_ms\":" << avg(fv, fv_n) << ",\"dwell_ms\":" << avg(dwell, n)
      << ",\"round_ms\":" << avg(round, n) << ",\"round_min_ms\":" << round_min << ",\"round_max_ms\":" << round_max
      << ",\"valid_per_hop\":" << avg(valid, n) << ",\"valid_min\":" << valid_min << ",\"valid_max\":" << valid_max
      << ",\"gain_hops\":" << gain_n << ",\"gain_write_ms\":" << avg(gain, gain_n)
      << "}";
    return o.str();
}

std::string tuners_json(const std::vector<int>& t) {
    std::string s = "[";
    for (size_t i = 0; i < t.size(); i++) s += (i ? "," : "") + std::to_string(t[i]);
    return s + "]";
}

// ---- Auto gain (docs/dev/operating-modes.md, "Auto gain") ----
//
// A gain per hop centre frequency (gain_map), learned from that hop's valid
// packets of the sweeps before: the common gain (current_gain) is the
// ceiling; a hop whose values clip (I or Q at 0 / 255) goes down - by a step
// that grows with how much clips - from its next visit on; it comes back up
// by AG_RAISE only after a hold without a single clipped value and with its
// peaks AG_RAISE_HEADROOM below full scale. A raise that is followed by
// clipping doubles the hold (a transmitter that keys on and off must not
// make the gain pump). No extra hop, dwell or measuring pass: the gain is
// written with the hop's retune, and only when it differs from that tuner's
// gain now (gain_writes). Only the hop thread touches the map; the
// conversion worker hands each tuner's statistics over in hop_acc.
std::atomic<bool> auto_gain_on{false};

constexpr double AG_CLIP_SHARE = 1e-4;          // more than 0.01 % of the values clipped: down
constexpr int AG_RAISE = 30;                    // tenths of a dB
constexpr double AG_RAISE_HEADROOM_DBFS = -6.0; // the peak after a raise at most this
constexpr uint32_t AG_HOLD_VISITS = 8, AG_HOLD_VISITS_MAX = 256;
constexpr int64_t AG_HOLD_NS = 20'000'000'000LL, AG_HOLD_NS_MAX = 640'000'000'000LL;
constexpr size_t AG_MAP_MAX = 200000;           // entries (hop centres) kept
constexpr uint32_t AG_SWATH_HZ = 1'200'000;     // a new centre takes the lowest gain learned within this

// The down step (tenths of a dB) for the share of clipped values: a hop
// that clips hard says nothing about how far it is over (its RMS saturates -
// 415.1 MHz read -2.6 dBFS at 49.6, 38.6 and 28.0 dB alike), so the step is
// large there and the next visits refine it
int ag_down_step(double share) {
    if (share > 0.01) return 180;
    if (share > 0.001) return 90;
    return 45;
}
// A gain write costs ~9 ms on the KrakenSDR (5-6 USB control transfers -
// more than the fast retune itself), so a tuner goes UP only when its next
// hop wants at least this much more (down: always - it must not clip)
constexpr int AG_UP_MIN = 30;

struct GainEntry {
    int gain = 0;                 // tenths of a dB (the tuner takes the largest list value <= min(this, ceiling))
    uint32_t clean = 0;           // visits in a row without a clipped value
    int64_t clean_since_ns = 0;
    uint32_t hold_visits = AG_HOLD_VISITS;
    int64_t hold_ns = AG_HOLD_NS;
    bool last_raise = false;      // the last change was a raise (clipping after it doubles the hold)
    float peak_max_dbfs = -200;   // the highest peak over the clean visits since the last change
};
std::map<uint32_t, GainEntry> gain_map;   // the hop thread only

// One tuner's valid packets of the hop it is on (the conversion worker adds,
// the hop thread takes them at the dwell's end)
struct HopAcc {
    uint32_t hz = 0;
    int gain = -999;
    uint64_t values = 0, clipped = 0, sumsq = 0;   // sumsq of (2u - 255), per value
    int peak = 0;                                   // max |2u - 255| (255 = full scale)
    uint32_t packets = 0;
};
std::mutex acc_mutex;
HopAcc hop_acc[NUM_DEVICES];
std::atomic<uint32_t> stats_mask{0};   // the tuners of the plan (the hop thread sets it)

// Status (of the last completed sweep)
std::atomic<uint32_t> ag_reduced_hops{0}, ag_clipped_hops{0}, ag_changes{0}, ag_writes{0}, ag_write_failures{0};
std::atomic<int> ag_min_gain{-999};
// ... being counted in the sweep that runs
uint32_t ag_cur_clipped = 0, ag_cur_changes = 0, ag_cur_writes = 0;

// Each tuner's gain list (rtlsdr_get_tuner_gains, tenths of a dB, ascending),
// cached per handle (an element-count change reopens the devices)
struct GainList { rtlsdr_dev_t* dev = nullptr; std::vector<int> g; };
GainList gain_lists[NUM_DEVICES];
const std::vector<int>& gains_of(int t) {
    GainList& l = gain_lists[t];
    rtlsdr_dev_t* dev = (t < static_cast<int>(devices.size()) && devices[t]) ? devices[t]->dev : nullptr;
    if (l.dev != dev || l.g.empty()) {
        l.dev = dev;
        l.g.clear();
        if (dev) {
            const int n = rtlsdr_get_tuner_gains(dev, nullptr);
            if (n > 0) {
                l.g.resize(static_cast<size_t>(n));
                if (rtlsdr_get_tuner_gains(dev, l.g.data()) != n) l.g.clear();
                std::sort(l.g.begin(), l.g.end());
            }
        }
    }
    return l.g;
}
// The largest list value <= g (the lowest if none); g itself without a list
int quantize_down(const std::vector<int>& L, int g) {
    if (L.empty()) return g;
    auto it = std::upper_bound(L.begin(), L.end(), g);
    return it == L.begin() ? L.front() : *(it - 1);
}
// The smallest list value > g (g if none)
int next_up(const std::vector<int>& L, int g) {
    auto it = std::upper_bound(L.begin(), L.end(), g);
    return it == L.end() ? g : *it;
}

// The gain learned for a hop centre (the ceiling if none): its own entry,
// else the lowest within AG_SWATH_HZ (a plan with other centres)
int learned_gain(uint32_t hz, int ceiling) {
    auto it = gain_map.find(hz);
    if (it != gain_map.end()) return std::min(it->second.gain, ceiling);
    int g = ceiling;
    for (auto lo = gain_map.lower_bound(hz > AG_SWATH_HZ ? hz - AG_SWATH_HZ : 0);
         lo != gain_map.end() && lo->first <= hz + AG_SWATH_HZ; ++lo)
        g = std::min(g, lo->second.gain);
    return g;
}

// The gain tuner t takes for hop centre hz now: the common gain unless auto
// is active (then the learned gain, on the tuner's own list)
int wanted_gain(int t, uint32_t hz) {
    const int ceiling = current_gain.load();
    if (!auto_gain_on.load() || ceiling < 0) return ceiling;
    return quantize_down(gains_of(t), learned_gain(hz, ceiling));
}

// A hop visit's statistics -> its entry (the hop thread, after the dwell)
void ag_learn(int t, const HopAcc& a, int64_t now) {
    const int ceiling = current_gain.load();
    if (ceiling < 0 || a.values == 0 || a.gain < 0) return;
    const std::vector<int>& L = gains_of(t);
    const double share = static_cast<double>(a.clipped) / static_cast<double>(a.values);
    const float peak_dbfs = a.peak > 0 ? 20.0f * std::log10(static_cast<float>(a.peak) / 255.0f) : -200.0f;
    auto it = gain_map.find(a.hz);
    if (share > AG_CLIP_SHARE) {
        ag_cur_clipped++;
        const int lowest = L.empty() ? 0 : L.front();
        if (a.gain <= lowest) return;   // nothing lower to take
        const bool fresh = it == gain_map.end();
        if (fresh) {
            if (gain_map.size() >= AG_MAP_MAX) gain_map.clear();
            it = gain_map.emplace(a.hz, GainEntry{}).first;
        }
        GainEntry& e = it->second;
        const int down = std::max(lowest, quantize_down(L, a.gain - ag_down_step(share)));
        // (a visit taken before the last change must not undo it)
        e.gain = fresh ? down : std::min(e.gain, down);
        if (e.last_raise) {   // it clipped again after a raise: wait longer next time
            e.hold_visits = std::min(e.hold_visits * 2, AG_HOLD_VISITS_MAX);
            e.hold_ns = std::min(e.hold_ns * 2, AG_HOLD_NS_MAX);
        }
        e.last_raise = false;
        e.clean = 0;
        e.peak_max_dbfs = -200;
        ag_cur_changes++;
        return;
    }
    if (it == gain_map.end()) return;   // at the ceiling and fine
    GainEntry& e = it->second;
    if (a.gain != quantize_down(L, std::min(e.gain, ceiling))) return;   // taken with another gain (changed meanwhile)
    if (a.clipped > 0) { e.clean = 0; e.peak_max_dbfs = -200; return; }   // a few clipped values: hold, no raise
    if (e.clean++ == 0) e.clean_since_ns = now;
    e.peak_max_dbfs = std::max(e.peak_max_dbfs, peak_dbfs);
    if (e.clean < e.hold_visits || now - e.clean_since_ns < e.hold_ns) return;
    const int up = std::min(ceiling, std::max(next_up(L, a.gain), quantize_down(L, a.gain + AG_RAISE)));
    const float rise_db = static_cast<float>(up - a.gain) / 10.0f;
    if (up <= a.gain || e.peak_max_dbfs + rise_db > AG_RAISE_HEADROOM_DBFS) return;
    ag_cur_changes++;
    if (up >= ceiling) { gain_map.erase(it); return; }   // back at the ceiling
    e.gain = up;
    e.last_raise = true;
    e.clean = 0;
    e.peak_max_dbfs = -200;
}

// At a sweep's end: the status of that sweep (the plan's hops)
void ag_publish(const SweepPlan& p) {
    const int ceiling = current_gain.load();
    uint32_t reduced = 0;
    int lowest = ceiling;
    if (auto_gain_on.load() && ceiling >= 0 && !gain_map.empty()) {
        for (uint32_t h = 0; h < p.steps; h++) {
            const int g = learned_gain(static_cast<uint32_t>(p.first_center_hz + h * p.step_hz), ceiling);
            if (g < ceiling) { reduced++; lowest = std::min(lowest, g); }
        }
    }
    ag_reduced_hops.store(reduced);
    ag_min_gain.store(lowest);
    ag_clipped_hops.store(ag_cur_clipped);
    ag_changes.store(ag_cur_changes);
    ag_writes.store(ag_cur_writes);
    ag_cur_clipped = ag_cur_changes = ag_cur_writes = 0;
}

// One gain write (under device_io_mutex, on the tuner's own thread): once
// more on failure. The tuner is in manual gain mode (the common gain is).
bool write_gain(rtlsdr_dev_t* dev, int g) {
    return rtlsdr_set_tuner_gain(dev, g) >= 0 || rtlsdr_set_tuner_gain(dev, g) >= 0;
}

}  // namespace

void set_sweep_auto_gain(bool enable) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    const bool was = auto_gain_on.exchange(enable);
    if (was && !enable && in_sweep()) sweep_restore_common_gain();
    sweep_notify();
}

bool sweep_auto_gain() { return auto_gain_on.load(); }

bool sweep_auto_gain_active() { return auto_gain_on.load() && in_sweep() && current_gain.load() >= 0; }

void sweep_note_hop_stats(const std::vector<SampleBuffer>& raw_set, const uint32_t* hz, const int* gain, int n) {
    const uint32_t mask = stats_mask.load(std::memory_order_relaxed);
    for (int t = 0; t < n && t < NUM_DEVICES && t < static_cast<int>(raw_set.size()); t++) {
        if (!(mask & (1u << t)) || hz[t] == 0 || gain[t] < 0) continue;
        const uint8_t* b = raw_set[t].data();
        const size_t nb = raw_set[t].size();
        uint64_t clipped = 0, ss = 0;
        int peak = 0;
        for (size_t k = 0; k < nb; k++) {
            const int d = 2 * static_cast<int>(b[k]) - 255;
            const int ad = d < 0 ? -d : d;
            clipped += ad == 255;
            ss += static_cast<uint32_t>(d * d);
            peak = ad > peak ? ad : peak;
        }
        std::lock_guard<std::mutex> lk(acc_mutex);
        HopAcc& a = hop_acc[t];
        if (a.hz != hz[t] || a.gain != gain[t]) a = HopAcc{hz[t], gain[t]};
        a.values += nb;
        a.clipped += clipped;
        a.sumsq += ss;
        a.peak = std::max(a.peak, peak);
        a.packets++;
    }
}

void sweep_restore_common_gain() {
    const int g = current_gain.load();
    std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
    int idx[NUM_DEVICES];
    size_t k = 0;
    for (int t = 0; t < NUM_DEVICES && t < static_cast<int>(devices.size()); t++)
        if (devices[t] && devices[t]->dev && wideband_config.get_tuner_gain(t) != g && g >= 0) idx[k++] = t;
    if (k == 0) return;
    const bool in_mode = in_sweep();
    if (in_mode) sweep_note_tuner_change();
    char ok[NUM_DEVICES] = {};
    for_each_tuner(k, [&](size_t i) { ok[i] = write_gain(devices[idx[i]]->dev, g); });
    for (size_t i = 0; i < k; i++) {
        wideband_config.tuner_gains[idx[i]] = g;
        if (!ok[i]) std::cerr << "Sweep: tuner " << idx[i] << " failed to return to the common gain" << std::endl;
    }
    if (in_mode) sweep_note_tuner_change_done();
}

std::vector<int> sweep_tuners(uint32_t tuner_mask) {
    const int n = std::clamp(active_num_elements.load(), 0, std::min<int>(NUM_DEVICES, static_cast<int>(devices.size())));
    std::vector<int> t;
    for (int i = 0; i < n; i++)
        if (tuner_mask & (1u << i)) t.push_back(i);
    if (t.empty())
        for (int i = 0; i < n; i++) t.push_back(i);
    return t;
}

uint32_t sweep_rounds(uint32_t steps, size_t tuners) {
    if (steps == 0 || tuners == 0) return 0;
    const uint32_t k = static_cast<uint32_t>(std::min<size_t>(tuners, steps));
    return (steps + k - 1) / k;
}

std::string apply_sweep_request(const SweepRequest& req) {
    if (req.has_plan) {
        if (std::string e = validate_sweep_plan(req.first_center_hz, req.step_hz, req.steps); !e.empty()) return e;
    }
    uint32_t mask = 0;
    if (req.has_tuners) {
        const int n = std::min(active_num_elements.load(), NUM_DEVICES);
        if (req.tuners.empty()) return "tuners: at least one";
        for (int t : req.tuners) {
            if (t < 0 || t >= n) return "tuners: " + std::to_string(t) + " is not a tuner (0-" + std::to_string(n - 1) + ")";
            if (mask & (1u << t)) return "tuners: " + std::to_string(t) + " given twice";
            mask |= 1u << t;
        }
    }
    if (req.has_dwell && (req.dwell_ms < SWEEP_DWELL_MIN_MS || req.dwell_ms > SWEEP_DWELL_MAX_MS))
        return "dwell_ms must be " + std::to_string(SWEEP_DWELL_MIN_MS) + "-" + std::to_string(SWEEP_DWELL_MAX_MS);
    if (req.has_settle && req.settle_ms > SWEEP_SETTLE_MAX_MS)
        return "settle_ms must be 0-" + std::to_string(SWEEP_SETTLE_MAX_MS);

    bool restart = false, persist = false;
    {
        // settings_mutex first (the hop thread's lock order: it reads the
        // plan under settings_mutex): the round / sweeps reset can't land
        // between the thread's plan check and its round store
        std::lock_guard<std::mutex> slock(settings_mutex);
        std::lock_guard<std::mutex> lock(sweep_state.mutex);
        SweepPlan& p = sweep_state.plan;
        if (req.has_plan && (p.first_center_hz != req.first_center_hz || p.step_hz != req.step_hz ||
                             p.steps != req.steps)) {
            p.first_center_hz = req.first_center_hz;
            p.step_hz = req.step_hz;
            p.steps = req.steps;
            restart = true;
        }
        if (req.has_tuners && p.tuner_mask != mask) {
            p.tuner_mask = mask;
            restart = true;
        }
        if (req.has_dwell && p.dwell_ms != req.dwell_ms) { p.dwell_ms = req.dwell_ms; persist = true; }
        if (req.has_settle && p.settle_ms != req.settle_ms) { p.settle_ms = req.settle_ms; persist = true; }
        if (req.has_run) p.run = req.run;   // not saved
        if (restart) {
            // the generation first: the thread checks it under this mutex
            // before it stores a round or counts a sweep
            sweep_state.plan_generation.fetch_add(1);
            sweep_state.round.store(0);
            sweep_state.sweeps.store(0);
            persist = true;
        }
    }
    sweep_notify();
    if (persist) settings::note_sweep();   // only what the file holds
    return "";
}

std::string sweep_status_json() {
    const SweepPlan p = sweep_state.get_plan();
    const std::vector<int> t = sweep_tuners(p.tuner_mask);
    std::ostringstream o;
    o << "{\"first_center_hz\":" << p.first_center_hz << ",\"step_hz\":" << p.step_hz << ",\"steps\":" << p.steps
      << ",\"tuners\":" << tuners_json(t) << ",\"dwell_ms\":" << p.dwell_ms << ",\"settle_ms\":" << p.settle_ms
      << ",\"run\":" << (p.run ? "true" : "false") << ",\"round\":" << sweep_state.round.load()
      << ",\"rounds\":" << sweep_rounds(p.steps, t.size()) << ",\"sweeps\":" << sweep_state.sweeps.load()
      << ",\"round_ms\":" << sweep_state.round_ms.load()
      // beyond the contract's fields (diagnostics)
      << ",\"retuning\":" << (in_sweep() && sweep_state.retuning_in_progress.load() ? "true" : "false")
      << ",\"failed_retunes\":" << sweep_state.failed_retunes.load()
      << ",\"failed_tuners\":" << [] {
             std::vector<int> f;
             const uint32_t m = sweep_state.failed_mask.load();
             for (int i = 0; i < NUM_DEVICES; i++) if (m & (1u << i)) f.push_back(i);
             return tuners_json(f);
         }()
      << ",\"l1_unquiet_flushes\":" << l1_unquiet_flushes()
      // (added 2026-11-21) the valid packets a hop delivers per tuner at
      // this dwell; the tuners that stalled the last gate (no data: the flag
      // stays 1 until they stream again), gates that failed, packets the gate
      // met more than half a period late; the hop timeline (averages of the
      // last hops)
      << ",\"hop_packets\":" << sweep_hop_packets(p.dwell_ms)
      << ",\"stalled_tuners\":" << [] {
             std::vector<int> f;
             const uint32_t m = sweep_gate_stalled();
             for (int i = 0; i < NUM_DEVICES; i++) if (m & (1u << i)) f.push_back(i);
             return tuners_json(f);
         }()
      << ",\"l1_depth\":" << [] {   // packets in each active dongle's L1 now (diagnostic)
             std::vector<int> f;
             const int n = std::min<int>(active_num_elements.load(), static_cast<int>(devices.size()));
             for (int i = 0; i < n; i++)
                 f.push_back(devices[i] ? static_cast<int>(devices[i]->l1_buffer_size.load()) : 0);
             return tuners_json(f);
         }()
      // packets each dongle has delivered into L1 (the callback's count) and
      // where its latest delivery lies against dongle 0's, us within
      // (-P/2, P/2] (diagnostics: a buffer lost on USB, the spread of the
      // dongles' buffer boundaries)
      << ",\"l1_put\":[" << [] {
             std::string s;
             const int n = std::min<int>(active_num_elements.load(), static_cast<int>(devices.size()));
             for (int i = 0; i < n; i++)
                 s += (i ? "," : "") + std::to_string(devices[i] ? devices[i]->l1_put.load() : 0);
             return s;
         }() << "]"
      << ",\"arrival_phase_us\":" << [] {
             std::vector<int> f;
             const int n = std::min<int>(active_num_elements.load(), static_cast<int>(devices.size()));
             const int64_t period = static_cast<int64_t>(1e9 * NUM_SAMPLES / SAMPLE_RATE);
             const int64_t t0 = n > 0 && devices[0] ? devices[0]->last_arrival_ns.load() : 0;
             for (int i = 0; i < n; i++) {
                 int64_t d = devices[i] ? devices[i]->last_arrival_ns.load() - t0 : 0;
                 d %= period;
                 if (d > period / 2) d -= period;
                 if (d <= -period / 2) d += period;
                 f.push_back(static_cast<int>(d / 1000));
             }
             return tuners_json(f);
         }()
      << ",\"slow_tuners\":" << [] {
             std::vector<int> f;
             const uint32_t m = sweep_gate_slow();
             for (int i = 0; i < NUM_DEVICES; i++) if (m & (1u << i)) f.push_back(i);
             return tuners_json(f);
         }()
      << ",\"gates_failed\":" << sweep_gates_failed() << ",\"gates_late\":" << sweep_gates_late()
      // auto gain (round 9; the counts of the last completed sweep)
      << ",\"auto_gain\":" << (auto_gain_on.load() ? "true" : "false")
      << ",\"auto_gain_active\":" << (sweep_auto_gain_active() ? "true" : "false")
      << ",\"gain_ceiling_db\":" << current_gain.load() / 10.0
      << ",\"gain_reduced_hops\":" << ag_reduced_hops.load()
      << ",\"gain_min_db\":" << (ag_min_gain.load() == -999 ? current_gain.load() : ag_min_gain.load()) / 10.0
      << ",\"clipped_hops\":" << ag_clipped_hops.load()
      << ",\"gain_changes\":" << ag_changes.load() << ",\"gain_writes\":" << ag_writes.load()
      << ",\"gain_write_failures\":" << ag_write_failures.load()
      << ",\"timing\":" << timing_json() << "}";
    return o.str();
}

void sweep_note_valid_set() {
    const uint32_t n = sweep_state.valid_sets.fetch_add(1) + 1;
    const int64_t t = now_ns();
    last_valid_ns.store(t, std::memory_order_relaxed);
    if (n == first_valid_mark.load(std::memory_order_relaxed)) first_valid_ns.store(t, std::memory_order_relaxed);
    // the dwell's last packet is out: wake the hop thread (the lock pairs
    // with its predicate check, so the wake can't fall between check and wait)
    const uint32_t target = valid_target.load();
    if (target != ~0u && static_cast<int32_t>(n - target) >= 0) {
        { std::lock_guard<std::mutex> lk(wake_mutex); }
        wake_cv.notify_all();
    }
}

void sweep_notify() {
    {
        std::lock_guard<std::mutex> lk(wake_mutex);
        sweep_state.change_seq.fetch_add(1);
    }
    wake_cv.notify_all();
}

void sweep_prepare_entry() {
    sweep_gate_reset_diagnostics();   // no stall of an earlier visit shown
    {
        std::lock_guard<std::mutex> lock(sweep_state.mutex);
        sweep_state.plan.run = true;   // entering the mode runs (as the client's SWEEP_RUN)
        sweep_state.round.store(0);
        sweep_state.sweeps.store(0);
        sweep_state.round_ms.store(0);
        sweep_state.plan_generation.fetch_add(1);
    }
    sweep_state.last_retune_ns.store(now_ns());
    sweep_state.retuning_in_progress.store(true);
}

void sweep_note_tuner_change() {
    sweep_state.retuning_in_progress.store(true);
    sweep_state.last_retune_ns.store(now_ns());
}

void sweep_note_tuner_change_done() {
    // the settle runs from here: the writes may take longer than settle +
    // flush (Windows tunes one by one)
    sweep_state.last_retune_ns.store(now_ns());
}

void sweep_quiesce_flushes() {
    std::lock_guard<std::mutex> lk(flush_mutex);
    // every gate ever filed served (a stale one included) before the
    // re-pairing flush that follows this - nothing after it touches the pairing
    sweep_gates_quiesce();
    // (settings_mutex held: set_operating_mode on the way back to coherent,
    // before return_to_coherent_locked retunes every tuner and the
    // recalibration starts) the sweep's fast tunes end here
    finish_fast_tunes(~0u, false, "sweep mode left");
}

void sweep_thread() {
    std::cout << "Sweep thread started (inactive until sweep mode)" << std::endl;
    uint32_t plan_gen_seen = ~0u;
    uint32_t r = 0;                          // the round to hop to next
    std::vector<int> move_tuner;             // this round's tuners and frequencies
    std::vector<uint32_t> move_hz;
    std::vector<char> move_ok;
    std::vector<int64_t> move_ns;            // each tuner's tune call, ns
    std::vector<char> move_fast;             // ... left on the fast path's state
    std::vector<char> move_tune;             // the frequency changes (else: the gain only)
    std::vector<int> move_gain;              // the gain it takes (auto gain; -999: unchanged)
    std::vector<char> move_gain_ok;
    std::vector<int64_t> move_gain_ns;       // its gain write, ns
    // the counter steps once per round: a round retried (a dongle stalled
    // its gate, a pause or plan change before its dwell) is not a new one
    bool round_open = false;
    // what the timeline averages belong to (reset when one changes)
    uint32_t timing_pg = ~0u, timing_dwell = ~0u, timing_settle = ~0u, timing_mask = ~0u;

    while (global_running.load()) {
        const uint32_t seq = sweep_state.change_seq.load();
        SweepPlan p;
        uint32_t pg;
        {
            std::lock_guard<std::mutex> lock(sweep_state.mutex);
            p = sweep_state.plan;
            pg = sweep_state.plan_generation.load();
        }
        if (!in_sweep() || !p.run || !pipeline_running.load(std::memory_order_acquire)) {
            round_open = false;
            // the sweep stopped moving tuners: their fast tunes end (also
            // when the mode was left for independent / wideband; on the way
            // to coherent sweep_quiesce_flushes did it already)
            if (fast_pending.load()) {
                std::lock_guard<std::mutex> lock(settings_mutex);
                finish_fast_tunes(~0u, in_sweep(), in_sweep() ? "sweep paused" : "sweep mode left");
            }
            // (outside sweep mode nothing reads the flag; entering sets it)
            if (in_sweep()) settle_and_clear_flag();
            wait_for_change(seq, 50'000'000);
            continue;
        }
        if (pg != plan_gen_seen) {
            plan_gen_seen = pg;
            r = 0;
            round_open = false;
        }

        // Who measures what (the contract): the k-th of the K' = min(K, H)
        // lowest selected tuners covers hops [k H / K', (k + 1) H / K');
        // in round r it goes to its hop base + r if it has one
        const std::vector<int> sel = sweep_tuners(p.tuner_mask);
        const uint32_t H = p.steps;
        const size_t kp = std::min<size_t>(sel.size(), H);
        const uint32_t R = sweep_rounds(H, sel.size());
        if (kp == 0 || R == 0) {   // (no tuner open: not reached in practice)
            settle_and_clear_flag();
            wait_for_change(seq, 50'000'000);
            continue;
        }
        if (r >= R) r = 0;
        move_tuner.clear();
        move_hz.clear();
        move_tune.clear();
        move_gain.clear();
        uint32_t hopping = 0;   // tuners that move from round to round (a block of 2+ hops)
        uint32_t plan_mask = 0;
        for (int t : sel) plan_mask |= 1u << t;
        stats_mask.store(auto_gain_on.load() ? plan_mask : 0, std::memory_order_relaxed);
        for (size_t k = 0; k < kp; k++) {
            const uint64_t b0 = static_cast<uint64_t>(k) * H / kp, b1 = static_cast<uint64_t>(k + 1) * H / kp;
            if (b1 - b0 > 1) hopping |= 1u << sel[k];
            const uint32_t hz = static_cast<uint32_t>(p.first_center_hz + (b0 + r) * p.step_hz);
            // a tuner already there isn't retuned (K' = H: every round is the
            // same - the stream then runs on without a gap); one whose last
            // retune failed is (its frequency entry is 0). Auto gain: the
            // gain this hop wants, written with the retune - and alone, under
            // the same flag, settle and gate, if only the gain differs
            if (b0 + r < b1) {
                const bool tune = wideband_config.get_tuner_frequency(sel[k]) != hz;
                const int g = wanted_gain(sel[k], hz);
                const int now_g = wideband_config.get_tuner_gain(sel[k]);
                // (a small step up isn't worth a write: the hop is taken a
                // little lower - and its header says so)
                const bool gain = g >= 0 && g != now_g &&
                                  (!auto_gain_on.load() || now_g < 0 || g < now_g || g - now_g >= AG_UP_MIN);
                if (tune || gain) {
                    move_tuner.push_back(sel[k]);
                    move_hz.push_back(hz);
                    move_tune.push_back(tune);
                    move_gain.push_back(gain ? g : -999);
                }
            }
        }
        move_ok.assign(move_tuner.size(), 0);
        move_ns.assign(move_tuner.size(), 0);
        move_fast.assign(move_tuner.size(), 0);
        move_gain_ok.assign(move_tuner.size(), 1);
        move_gain_ns.assign(move_tuner.size(), 0);
        // the flag may be on without a retune: the mode just entered, a gain
        // change - then the settle + flush below run all the same
        const bool retune = !move_tuner.empty();
        if (pg != timing_pg || p.dwell_ms != timing_dwell || p.settle_ms != timing_settle || p.tuner_mask != timing_mask) {
            timing_pg = pg; timing_dwell = p.dwell_ms; timing_settle = p.settle_ms; timing_mask = p.tuner_mask;
            timing_reset();
        }
        HopTiming tm;
        tm.moved = retune;

        const int64_t t0 = now_ns();
        int64_t t_retune_end = t0;
        // 1) flag, 2) downstream flush (what is queued is from the old hop
        //    and would be sent flagged anyway)
        if (retune) {
            sweep_state.retuning_in_progress.store(true);
            clear_l2_buffer_quiet();
            tm.flag = ms_between(t0, now_ns());
        }

        // 3) retune, with the mode and the plan re-checked under the locks
        //    every other tuning path takes: leaving the mode or changing the
        //    plan meanwhile must not be retuned over
        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(settings_mutex);
            // (and no hop while the pipeline is down or an element-count
            // change owns the devices - it holds settings_mutex while it
            // closes and reopens them, but stops the pipeline first)
            if (!global_running.load() || !in_sweep() || sweep_state.plan_generation.load() != pg ||
                !sweep_state.get_plan().run || !pipeline_running.load(std::memory_order_acquire) ||
                reconfig_in_progress.load(std::memory_order_acquire)) {
                cancelled = true;
            } else if (retune) {
                std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
                const int64_t t_locked = now_ns();
                tm.locked = ms_between(t0, t_locked);
                for_each_tuner(move_tuner.size(), [&](size_t i) {
                    const int t = move_tuner[i];
                    if (t < 0 || t >= static_cast<int>(devices.size()) || !devices[t] || !devices[t]->dev) return;
                    const int64_t a = now_ns();
                    bool fast = false;
                    move_ok[i] = move_tune[i] ? sweep_tune(devices[t]->dev, move_hz[i], fast) : 1;
                    move_fast[i] = fast;
                    // the hop's gain after its frequency (a tuner whose
                    // retune failed gets it on its next try)
                    if (move_ok[i] && move_gain[i] != -999) {
                        const int64_t g0 = now_ns();
                        move_gain_ok[i] = write_gain(devices[t]->dev, move_gain[i]);
                        move_gain_ns[i] = now_ns() - g0;
                    }
                    move_ns[i] = now_ns() - a;
                });
                for (size_t i = 0; i < move_tuner.size(); i++) {
                    if (!move_tune[i]) continue;
                    if (move_fast[i]) fast_pending.fetch_or(1u << move_tuner[i]);
                    else fast_pending.fetch_and(~(1u << move_tuner[i]));   // a full tune ends it
                }
                t_retune_end = now_ns();
                tm.retune = ms_between(t_locked, t_retune_end);
                for (int64_t ns : move_ns) tm.tune_max = std::max(tm.tune_max, static_cast<float>(ns) / 1e6f);
                for (int64_t ns : move_gain_ns) tm.gain_max = std::max(tm.gain_max, static_cast<float>(ns) / 1e6f);
                // Only now the new frequencies (the header reads them before
                // the flag). A tuner that failed is NOT on its old frequency
                // (the R820T / E4000 drivers fail after N / SDM are written,
                // librtlsdr then reports 0): its entry is 0, which no client
                // takes for a hop, until a retune succeeds
                // The gain likewise, after the tuner took it (the header
                // reads it with the frequency, before the flag). A gain
                // write that failed twice leaves the tuner's gain unknown:
                // its frequency entry 0 (no valid data) and its gain -2, so
                // the next hop writes both again.
                uint32_t failed_now = 0;
                for (size_t i = 0; i < move_tuner.size(); i++) {
                    const int t = move_tuner[i];
                    if (move_ok[i] && move_gain[i] != -999) {
                        ag_cur_writes++;
                        wideband_config.tuner_gains[t] = move_gain_ok[i] ? move_gain[i] : -2;
                        if (!move_gain_ok[i]) { move_ok[i] = 0; ag_write_failures.fetch_add(1); }
                    }
                    if (move_tune[i] || !move_ok[i]) wideband_config.set_tuner_frequency(t, move_ok[i] ? move_hz[i] : 0);
                    if (!move_ok[i]) failed_now |= 1u << t;
                }
                uint32_t moved = 0;
                for (int t : move_tuner) moved |= 1u << t;
                sweep_state.failed_mask.store((sweep_state.failed_mask.load() & ~moved) | failed_now);
                if (failed_now)
                    sweep_state.failed_retunes.fetch_add(static_cast<uint32_t>(std::popcount(failed_now)));
                sweep_state.last_retune_ns.store(now_ns());
                for (size_t i = 0; i < move_tuner.size(); i++)
                    if (move_tune[i]) note_tuner_retuned(move_tuner[i]);   // (also: --kerberos marks the calibration stale)
            }
            if (!cancelled) {
                // tuners that won't move again (out of the plan, or on the
                // only hop of their block, K' = H): their fast tunes end now,
                // under the flag (settled + gated below)
                if (const uint32_t stop = fast_pending.load() & ~hopping)
                    finish_fast_tunes(stop, true, "tuner stopped hopping");
                // round and counter only for the plan in force (a set_sweep
                // resets them under sweep_state.mutex); the counter once per round
                std::lock_guard<std::mutex> plock(sweep_state.mutex);
                if (sweep_state.plan_generation.load() == pg) sweep_state.round.store(r);
                if (!round_open) sweep_state.frequency_change_counter.fetch_add(1);
                round_open = true;
            }
        }
        if (cancelled) continue;  // the flag stays on: the next pass hops or settles + clears it
        {
            int failed = 0, ft = -1;
            uint64_t fhz = 0;
            for (size_t i = 0; i < move_tuner.size(); i++)
                if (!move_ok[i]) { failed++; ft = move_tuner[i]; fhz = move_hz[i]; }
            report_failures(failed, ft, fhz);
        }

        // 4) settle, 5) gate - again if a gain change came in meanwhile (it
        //    moved last_retune_ns) or a dongle stalled -, 6) flag off,
        //    7) dwell. The flag is cleared under settings_mutex, which every
        //    change that sets it holds (the mode entry, a gain change, the
        //    next hop's retune): none can slip in between the check and the clear
        uint32_t valid_from = sweep_state.valid_sets.load();
        bool settled = !retune && !sweep_state.retuning_in_progress.load();
        int64_t t_clear = now_ns();
        if (settled) {   // nothing to settle: the stream runs on (K' = H)
            first_valid_ns.store(0, std::memory_order_relaxed);
            first_valid_mark.store(valid_from + 1, std::memory_order_relaxed);
        }
        for (int pass = 0; pass < 8 && !settled; pass++) {
            if (!wait_settle(pg)) break;
            const int64_t t_settled = now_ns();
            const int64_t settled_from = sweep_state.last_retune_ns.load();
            const int64_t from_ns = settled_from + static_cast<int64_t>(sweep_state.get_plan().settle_ms) * 1'000'000;
            const int g = sweep_gate(from_ns);
            if (g < 0) break;
            if (g == 0) continue;   // a dongle stalled (logged, status stalled_tuners): not valid yet
            const int64_t t_gated = now_ns();
            std::lock_guard<std::mutex> lock(settings_mutex);
            if (in_sweep() && sweep_state.plan_generation.load() == pg &&
                sweep_state.last_retune_ns.load() == settled_from) {
                // (no set counts as valid while the flag is on: valid_from,
                // the mark and t_clear before the clear - every valid set of
                // the hop is counted, stamped and after t_clear)
                valid_from = sweep_state.valid_sets.load();
                first_valid_ns.store(0, std::memory_order_relaxed);
                first_valid_mark.store(valid_from + 1, std::memory_order_relaxed);
                t_clear = now_ns();
                sweep_state.retuning_in_progress.store(false);
                settled = true;
                tm.settle = ms_between(t_retune_end, t_settled);
                tm.gate = ms_between(t_settled, t_gated);
                tm.clear = ms_between(t_gated, t_clear);
            }
        }
        if (!settled) continue;
        if (!wait_dwell(pg, valid_from, t_clear)) continue;

        const int64_t t_end = now_ns();
        // Auto gain: what each tuner's valid packets of this hop showed
        // (handed over by the conversion worker before it counted them)
        if (auto_gain_on.load() && current_gain.load() >= 0) {
            for (size_t k = 0; k < kp; k++) {
                const uint64_t b0 = static_cast<uint64_t>(k) * H / kp, b1 = static_cast<uint64_t>(k + 1) * H / kp;
                if (b0 + r >= b1) continue;
                const int t = sel[k];
                const uint32_t hz = static_cast<uint32_t>(p.first_center_hz + (b0 + r) * p.step_hz);
                HopAcc a;
                {
                    std::lock_guard<std::mutex> lk(acc_mutex);
                    a = hop_acc[t];
                    hop_acc[t] = HopAcc{};
                }
                if (a.hz == hz && a.packets > 0) ag_learn(t, a, t_end);
            }
        }
        tm.dwell = ms_between(t_clear, t_end);
        tm.round = ms_between(t0, t_end);
        tm.valid = sweep_state.valid_sets.load() - valid_from;
        if (const int64_t fv = first_valid_ns.load(std::memory_order_relaxed); fv && tm.valid)
            tm.first_valid = ms_between(t_clear, fv);
        timing_record(tm);
        sweep_state.round_ms.store(static_cast<uint32_t>((t_end - t0) / 1'000'000));
        round_open = false;
        if (++r >= R) {
            r = 0;
            ag_publish(p);
            std::lock_guard<std::mutex> plock(sweep_state.mutex);
            if (sweep_state.plan_generation.load() == pg) sweep_state.sweeps.fetch_add(1);
        }
    }
    std::cout << "Sweep thread exiting" << std::endl;
}
