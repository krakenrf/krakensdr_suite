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
#include "pffft/pffft.h"
#include <limits>
#include <tuple>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>
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
    if (share > 0.15) return 180;   // saturated (415.1 MHz: 25 / 23 / 18 % at 49.6 / 38.6 / 28.0 dB)
    if (share > 0.01) return 120;   // (a Gaussian that clips 1 % needs ~4 dB, 10 % ~8: the probes take back the rest)
    if (share > 0.001) return 60;
    return 45;
}
// After a step down the next clean visit is an unsaturated measurement: its
// peak says how much room there is. Up to AG_PROBES visits take the hop at
// once to where its peak would sit at AG_PEAK_TARGET_DBFS (a hard clip's step
// overshoots: 415.1 MHz needs ~33 dB, an LTE hop 4-8) - no hold. A probe that
// clips goes back to the gain before it, and the next reductions of that hop
// don't probe for its hold (bursty traffic: a quiet moment's peak says too much).
constexpr float AG_PEAK_TARGET_DBFS = -12.0f;
constexpr int AG_PROBE_MIN_ROOM = 60;   // tenths of a dB: a smaller room is left to the slow raises
constexpr int AG_PROBES = 2;
// A gain write costs +1 USB transfer inside the retune with the fork's fast
// call (RTLSDR_HAS_GAIN_FAST): every hop then takes the gain it wants. Without
// it a write is ~8 transfers, 9.1-9.5 ms on the KrakenSDR - more than the
// retune -, so a step UP of less than AG_UP_MIN may be skipped, but for at
// most AG_UP_SKIP_MAX hops in a row per tuner: a deficit is carried past the
// hop that caused it for one hop, never a block (it once sat on every
// remaining hop of a tuner's block, 1.6 dB low, and on the next sweep's start).
// Down: always - it must not clip.
constexpr int AG_UP_MIN = 30;
#if defined(RTLSDR_HAS_GAIN_FAST)
constexpr int AG_UP_SKIP_MAX = 0;
#else
constexpr int AG_UP_SKIP_MAX = 1;
#endif
int up_skips[NUM_DEVICES] = {};   // the hop thread only

struct GainEntry {
    int gain = 0;                 // tenths of a dB (the tuner takes the largest list value <= min(this, ceiling))
    uint32_t clean = 0;           // visits in a row without a clipped value
    int64_t clean_since_ns = 0;
    uint32_t hold_visits = AG_HOLD_VISITS;
    int64_t hold_ns = AG_HOLD_NS;
    bool last_raise = false;      // the last change was a raise (clipping after it doubles the hold)
    float peak_max_dbfs = -200;   // the highest peak over the clean visits since the last change
    float rms_max_dbfs = -200;    // ... and the highest rms (the level a raise is sized by)
    int probes = 0;               // probe visits left after a step down
    int probe_from = -1;          // the gain before the last probe (clean there)
    int64_t no_probe_until = 0;   // a probe clipped: none until then
    int blind_from = -1;          // a table (blind) cut came from this gain: its first clean visit corrects it
};
std::map<uint32_t, GainEntry> gain_map;   // the hop thread only
// A mild clip (at most AG_MILD_SHARE of the values) costs a step down only
// when it comes again within AG_MILD_WINDOW_NS: a burst of traffic (LTE
// downlink: 0.01 - 0.1 % in one sweep in ten) must not cost a gain - and
// two writes a sweep - every time. hz -> the time of its last mild clip.
constexpr double AG_MILD_SHARE = 1e-3;
constexpr int64_t AG_MILD_WINDOW_NS = 60'000'000'000LL;
std::map<uint32_t, int64_t> mild_clips;   // the hop thread only

// One tuner's valid packets of the hop it is on (the conversion worker adds,
// the hop thread takes them at the dwell's end)
struct HopAcc {
    uint32_t hz = 0;
    int gain = -999;
    uint64_t values = 0, clipped = 0, sumsq = 0;   // sumsq of (2u - 255), per value
    int peak = 0;                                   // max |2u - 255| (255 = full scale)
    uint32_t packets = 0;
    double floor_db = 0;   // the noise floor estimates summed (dB per bin, full scale = 0), floor_n of them
    uint32_t floor_n = 0;
    // the strongest bin of the kept band (its offset from the centre, Hz, and
    // its height over the floor, dB) - of the visit's packet where it stood highest
    float pk_off_hz = 0, pk_over_db = -99;
    // the strongest bin at or above the window's lower edge (the part of this
    // band within AG_NB_REACH_HZ below the next hop's centre): its offset,
    // height over the floor and power (its bin and the two beside it, dBFS)
    float win_off_hz = 0, win_over_db = -99, win_pow_db = -200;
    uint32_t seq = 0;      // the visit (its spectrum estimate arrives later, by this number)
    bool est_queued = false;
};
std::mutex acc_mutex;
HopAcc hop_acc[NUM_DEVICES];
std::atomic<uint32_t> stats_mask{0};   // the tuners of the plan (the hop thread sets it)
std::atomic<uint32_t> stats_half_band_hz{956250};   // the kept band's half width (the plan's step / 2)
std::atomic<float> win_lo_hz{-287500.0f};   // the guess window's lower edge in a band (the plan's step - AG_NB_REACH_HZ)

// The spectrum estimate OFF the hop's path (2026-11-24, the Pi 5: computed
// before the set was counted, five tuners' Welch estimates cost ~0.66 ms a
// round - +3.0 % a sweep with Auto on and nothing to do). The conversion
// worker counts the set at once and hands the visit's first packet to this
// thread; its result (floor, strongest bins) is attached to the visit by its
// number, and the hop thread learns from the visit when it is there (it is
// used at the hop's NEXT visit, a sweep away). One that does not come in
// time is a visit without a verdict. The thread is detached and waits on a
// static condition variable: heimdall ends through platform::fast_exit
// (_Exit), so no static destructor runs under its waiter at exit (as the
// downconverter's poller).
struct EstJob {
    int t = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> bytes;
};
struct EstResult {
    uint32_t seq = 0;
    bool done = false;
    float floor = NAN;
    float pk_off_hz = 0, pk_over_db = -99, win_off_hz = 0, win_over_db = -99, win_pow_db = -200;
};
std::mutex est_mutex;
std::condition_variable est_cv;
std::deque<EstJob> est_queue;
std::vector<std::vector<uint8_t>> est_pool;     // spare buffers
EstResult est_result[NUM_DEVICES][4];            // by seq % 4
// The visit numbers count per tuner (under acc_mutex): a tuner has at most one
// visit a round, so its last four visits always have four slots and a result
// can wait AG_EST_MAX_AGE rounds whatever the tuner count. (One counter for all
// tuners stepped by the tuner count: with 4 or 8 every visit of a tuner took
// the same slot, and since a 5 ms dwell ends at the very set that is queued, a
// result is never collected in its own round - the next visit overwrote every
// one of them.)
uint32_t visit_seq[NUM_DEVICES] = {};

// Status (of the last completed sweep)
std::atomic<uint32_t> ag_reduced_hops{0}, ag_clipped_hops{0}, ag_changes{0}, ag_writes{0}, ag_write_failures{0};
std::atomic<int> ag_min_gain{-999};
// ... being counted in the sweep that runs
uint32_t ag_cur_clipped = 0, ag_cur_changes = 0, ag_cur_writes = 0;
// the gains the tuners really used on the sweep's hops (what the 8091
// headers carried): hops under the ceiling, the lowest
uint32_t ag_cur_reduced = 0;
int ag_cur_min = 1 << 30;

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

// The neighbour rule (measured at 415.1 MHz: the hop beside a saturating
// signal - 2.3 MHz from it, outside its band but inside the tuner's IF - shows
// intermodulation products at the ceiling that fall 29-44 dB for an 11 dB
// step, and the floor 10 dB up): a hop whose centre lies within
// AG_NEIGHBOUR_HZ of a hop held AG_NEIGHBOUR_DEEP or more under the ceiling
// is held at most AG_NEIGHBOUR_CAP under it while that lasts - not all the
// way down: the products are gone after ~11 dB, every dB more costs that
// hop's sensitivity.
constexpr uint32_t AG_NEIGHBOUR_HZ = 2'400'000;
constexpr int AG_NEIGHBOUR_DEEP = 180, AG_NEIGHBOUR_CAP = 120;   // tenths of a dB
// (round 10: the linearity search measures what the cap guessed - off)
constexpr bool AG_NEIGHBOUR_RULE = false;

// The gain learned for a hop centre (the ceiling if none): its own entry,
// else the lowest within AG_SWATH_HZ (a plan with other centres); then the
// neighbour rule's cap
int learned_gain(uint32_t hz, int ceiling) {
    int g = ceiling;
    auto it = gain_map.find(hz);
    if (it != gain_map.end()) {
        g = std::min(it->second.gain, ceiling);
    } else {
        for (auto lo = gain_map.lower_bound(hz > AG_SWATH_HZ ? hz - AG_SWATH_HZ : 0);
             lo != gain_map.end() && lo->first <= hz + AG_SWATH_HZ; ++lo)
            g = std::min(g, lo->second.gain);
    }
    for (auto lo = gain_map.lower_bound(hz > AG_NEIGHBOUR_HZ ? hz - AG_NEIGHBOUR_HZ : 0);
         AG_NEIGHBOUR_RULE && lo != gain_map.end() && lo->first <= hz + AG_NEIGHBOUR_HZ; ++lo) {
        if (lo->first == hz) continue;
        if (std::min(lo->second.gain, ceiling) <= ceiling - AG_NEIGHBOUR_DEEP) {
            g = std::min(g, ceiling - AG_NEIGHBOUR_CAP);
            break;
        }
    }
    return g;
}

// The R820T / R828D's real gain against its list (2026-11-23, the dev
// machine's KrakenSDR, five tuners, the DVB-T multiplexes 517-606 MHz:
// t_gaincurve.py, docs/dev/operating-modes.md "Auto gain"), dB relative to
// the top value 49.6. Below 7.7 dB nothing was measurable: continued at the
// table's own steps. Status "gain_real_db" (a client that shifts a hop's
// levels by its gain uses it; the header carries the table value).
struct RealGain { int g; float db; };
constexpr RealGain R82XX_REAL[] = {
    {0, -43.59f}, {9, -42.69f}, {14, -42.19f}, {27, -40.89f}, {37, -39.89f},   // (table steps)
    {77, -35.69f}, {87, -34.20f}, {125, -31.93f}, {144, -30.59f}, {157, -29.15f}, {166, -27.83f},
    {197, -25.23f}, {207, -23.78f}, {229, -21.18f}, {254, -19.78f}, {280, -17.03f}, {297, -15.64f},
    {328, -13.22f}, {338, -11.83f}, {364, -10.30f}, {372, -8.92f}, {386, -7.91f}, {402, -6.64f},
    {421, -5.44f}, {434, -4.20f}, {439, -3.22f}, {445, -3.22f}, {480, -1.41f}, {496, 0.0f}};

std::string real_gain_json() {
    // only for the tuners measured: an R820T / R828D on every sweep tuner
    const std::vector<int> t = sweep_tuners(sweep_state.get_plan().tuner_mask);
    if (t.empty()) return "";
    for (int i : t) {
        if (i >= static_cast<int>(devices.size()) || !devices[i] || !devices[i]->dev) return "";
        const rtlsdr_tuner ty = rtlsdr_get_tuner_type(devices[i]->dev);
        if (ty != RTLSDR_TUNER_R820T && ty != RTLSDR_TUNER_R828D) return "";
    }
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "[";
    bool first = true;
    for (const RealGain& r : R82XX_REAL) {
        o << (first ? "" : ",") << "[" << r.g / 10.0 << "," << r.db << "]";
        first = false;
    }
    o << "]";
    return o.str();
}

// ---- The linearity search (round 10; docs "Auto gain", "The linearity search") ----
//
// Clipping is a late symptom: the tuner's front end compresses and makes
// products well before a value reaches 0 / 255 - often driven by a carrier
// OUTSIDE the hop's sampled band (the decimation filter takes the carrier
// away, its damage stays). Per hop, a probe is a normal visit at a
// neighbouring gain (sent as valid, its gain in the header, the 8091 trial
// gain flag on), compared with the hop's visits at its own gain. The
// clipping rule stays the fast guard and the ceiling of the search.
//
// (2026-11-24, from the orchestrator's manual scan at 405-428 MHz) The
// test is LINEARITY, not the floor's value: beside 415.64 MHz (a carrier
// 1.8 MHz below the 417.43 MHz hop's centre, outside its sampled band) the
// hop's power stays pinned from 49.6 down to 33.8 dB and its carrier is
// compressed by up to 12 dB, the floor referred to the input LOOKS best at
// the top - and the carrier's SNR is 8.6 dB better at 33.8. The wanted gain
// is the highest at which the chain is linear: between the hop's gain and a
// probe (a real step dG of at least AG_LIN_MIN_STEP), every quantity well
// above the converter's own noise must change by dG - the visit's power
// (rms) and its floor (the 20th percentile), each with the converter's own
// noise subtracted.
//
// dG is not the fixed curve's: the real step depends on the frequency (the
// orchestrator's whole-range scan, 2026-11-24: 49.6 -> 48.0, -1.4 dB on the
// curve, raises the output by ~1 dB at 86 - 98 MHz and lowers it by 2 dB at
// 406 MHz), so the curve alone calls the FM band non-linear. The step is
// LEARNED: per 10 MHz bucket and gain pair, each hop's measured step from
// its valid pairs (all tuners: on the full plan a band is one tuner's; the
// table is cleared when the plan or its tuners change) - the median of the
// OTHER hops' (>= AG_STEP_MIN_HOPS) over the bucket and its two neighbours is
// the reference, believed within AG_STEP_TRUST of the curve (else the
// curve: a whole band saturated; the gross test needs no fine reference).
constexpr float AG_LIN_DEV = 1.5f;        // dB: a deviation from the step at least this ...
constexpr float AG_LIN_REL = 0.5f;        // ... (the power: or this share of the step) is non-linear
// compression: short by this share of the step (with 0.5, 417.43 MHz on one
// tuner sat at 36.4 following 1.5 of 2.9 dB - 1.4 short, inside 1.45; a
// linear hop with the curve 1.3 dB long on that step still passes 1.3)
constexpr float AG_LIN_REL_COMP = 0.45f;
constexpr float AG_LIN_DEV_UP = 1.0f;     // dB: an up-probe's power compressed this much (or a quarter of the step) is non-linear
constexpr float AG_LIN_CURVE_SLACK = 1.0f; // dB more while the reference is the curve (nothing learned yet)
constexpr float AG_LIN_GROSS = 4.0f;      // dB: a floor this far off decides alone ...
constexpr float AG_LIN_GROSS_STEP = 4.0f; // ... as does a step of at least this
constexpr float AG_LIN_COMP = 0.5f;       // ... that the power follows by less than this share
constexpr float AG_LIN_MIN_STEP = 2.5f;   // dB real: a probe's smallest step (the list has 0 dB steps)
constexpr float AG_LIN_OVER = 10.0f;      // dB: a test counts when its quantity (converter subtracted) is this far over the converter's own
constexpr float AG_LIN_CAUSE_DB = -15.0f; // dBFS rms at the higher gain: a compression verdict needs this ...
constexpr uint32_t AG_LIN_CAUSE_HZ = 3'000'000;   // ... in the hop or within this of its centre (-20 .. -10 alike on the reference)
constexpr float AG_LIN_BURST = 4.0f;
constexpr float AG_LIN_STEADY = 0.75f;
constexpr int AG_LIN_NO_VERDICT = 3;
constexpr double AG_LIN_HEAVY_CLIP = 0.01;
// The first guess from the lower neighbour's level (2026-11-24, the
// orchestrator's two measurements): a saturated hop's linear limit is where
// the carrier of the hop BELOW it reads about -2 dBFS in band - 415.11 MHz at
// -1.1 dBFS rms at 29.7 dB, 417.02 (that carrier 1.38 MHz below its centre)
// linear up to 29.7; 415.52 at -2.3 dBFS at 33.8, 417.43 (1.79 MHz below)
// linear up to 33.8. The IF filter gives ~0 dB there.
constexpr float AG_NB_LEVEL_DBFS = -2.0f;  // the neighbour's level at the guess, at most
constexpr float AG_NB_RISE_DB = 3.0f;      // the neighbour's level up this much since the guess: guess again
constexpr float AG_NB_REACH_HZ = 2.2e6f;   // the neighbour's carrier at most this far below the hop's centre
constexpr float AG_NB_CARRIER_DB = 20.0f;
constexpr float AG_NB_FOLLOW_HZ = 300e3f;  // the guess's carrier: still there when a window carrier is this close  // a strongest bin this far over the floor is a carrier (its position counts)
// The clipping rule's cut from the LEVEL: a moderate clip (at most
// AG_LEVEL_CUT_MAX of the values) on a visit whose rms is at least
// AG_LEVEL_CUT_MIN_RMS (a level, not a burst inside a quiet visit) takes the
// smallest step down that brings the rms to AG_LEVEL_TARGET_DBFS by the real
// gain curve, at least one list step (415.11 MHz: -1.1 dBFS at 29.7 and 2 - 4 %
// clipped in a strong minute - 12 dB by the table, 29.7 -> 16.6, where its
// carrier over floor is 10 dB worse than at 22.9)
constexpr double AG_LEVEL_CUT_MAX = 0.05;
// (-6: a carrier near full scale, crest under ~6 dB; with -12 the GSM / LTE
// hops - bursty, crest 10 dB and more - came down one list step per clipped
// visit: 944.87 MHz in 10 steps)
constexpr float AG_LEVEL_CUT_MIN_RMS = -6.0f, AG_LEVEL_TARGET_DBFS = -7.0f;
// the correction of a blind cut: a carrier-like clean visit (rms at least
// this, crest - peak over rms - at most this)
constexpr float AG_BLIND_MIN_RMS = -20.0f, AG_BLIND_MAX_CREST = 8.0f;
uint32_t plan_step_hz = 0;                 // the plan's step (the hop thread) // a baseline clipped more than this (1 %) is the clipping rule's alone      // pairs in a row without a verdict end a search    // dB: a quantity whose brackets change more (median of 8) judges nothing      // dB: more power at the lower gain than this: a burst, no pair
// the converter's own noise (no input), as the estimators measure it (the
// KrakenSDR at gain 0, the whole range, 2026-11-24: rms -45.0 dBFS, floor -46.5)
constexpr float AG_CONV_P_DB = -45.0f;    // the visit's power, dB against full scale (127.5 a component)
constexpr float AG_CONV_F_DB = -46.5f;    // the floor (the 20th percentile of the bins), same scale
constexpr uint32_t AG_STEP_BUCKET_HZ = 10'000'000;
constexpr int AG_STEP_HOPS = 32, AG_STEP_MIN_HOPS = 3;   // hops a bucket keeps; other hops a reference needs
constexpr float AG_STEP_TRUST = 2.5f;
constexpr float AG_FLOOR_BRACKET = 1.0f;  // a quantity's pair counts when the visits before and after the probe agree this well
constexpr float AG_FLOOR_JUMP = 3.0f;     // a baseline this much over the hop's slow average twice: the search reopens, down
constexpr float AG_FLOOR_DROP = 1.5f;     // ... this much under it (a hop held down): the search reopens, up
constexpr float AG_FLOOR_VAR = 1.5f;      // ... on a hop whose floor is steady (median visit-to-visit change)
constexpr int AG_FLOOR_STEP_BIG = 60, AG_FLOOR_STEP = 30;   // search steps, tenths of a dB (the list's)
#if defined(RTLSDR_HAS_GAIN_FAST)
constexpr uint32_t AG_PROBE_PERIOD = 50;  // settled: each hop probed once in this many sweeps (~2 %)
#else
constexpr uint32_t AG_PROBE_PERIOD = 200; // (~0.5 %: two ~9 ms writes a probe)
#endif
constexpr uint32_t AG_PROBE_HOLD_MAX = 8;

// One hop's linearity search. A probe's pair is BRACKETED: the baseline
// visit before it and the one after it must agree (AG_FLOOR_BRACKET, each
// quantity on its own), the probe is compared with their mean - a hop whose
// content moves by itself yields few valid pairs (so few decisions), one
// whose cause comes and goes slowly still yields pairs in each state. A
// decision needs two agreeing valid pairs, or one gross one.
struct FloorEntry {
    int g = -1;                  // the measured rule's gain (tenths of a dB); -1: none (the clipping rule's)
    bool search = true;          // still searching (since the switch went on, or a trigger)
    int step = AG_FLOOR_STEP_BIG;
    // the latest baseline visit (at the hop's own gain): power and floor in
    // the samples, dB; F_in = floor - the real gain (the triggers)
    float base_p = NAN, base_f = NAN, base_fin = NAN;
    float lvl_in = NAN;          // the last visit's power referred to the input (any gain, clipped too)
    int last_clip_g = -1, last_ceil = -1;   // the clipping rule's gain and the ceiling at the last visit
    // the level: this hop's power referred to the input over its last 4
    // visits that were not heavily clipped (the lower neighbour's first
    // guess reads it), the tuner it was taken on
    float lvl_ring[4] = {NAN, NAN, NAN, NAN};
    uint32_t lvl_n = 0;
    int tuner = -1;
    bool nb_done = false;        // the first guess from the lower neighbour's level was made ...
    float nb_lvl = NAN;          // ... at this neighbour level
    int nb_cap = 1 << 30;        // ... and gave this gain: an up-probe with nothing testable stays under it
    float car_off_hz = 0, car_over_db = -99;   // the last not heavily clipped visit's strongest bin
    // the window's carrier (>= AG_NB_CARRIER_DB over the floor) of the last two
    // such visits: present, offset, level referred to the input
    bool wc_ok[2] = {false, false};
    float wc_off[2] = {0, 0}, wc_lvl[2] = {-200, -200};
    uint32_t wc_n = 0;
    float nb_off = 0;            // the guess's carrier offset (in the neighbour's band)
    bool nb_flat = false;        // the guess came from flat content (the band's total), not a carrier
    uint32_t base_sweep = ~0u;
    float scatter = 0;           // the median of the last 8 |F_in - the visit before| at the hop's gain
    float dif[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int dif_n = 0;
    float ref = NAN, ref_p = NAN;   // slow averages of the baselines' F_in and power referred to the input
    int jump = 0, drop = 0;
    bool up_search = false;      // the search goes up (after a drop; the bisection), not down
    int up_lim = 1 << 30;        // the bisection: below this gain (the one a big step down left, non-linear)
    bool moved_big = false;      // this down search moved by a big step (then the bisection follows)
    bool clip_lift = false;      // this up search was started by a raise of the clipping rule (may pass its gain)
    bool next_up = false;        // settled probes alternate down / up
    uint32_t hold = 1;           // the settled period's multiplier: doubles when a probe confirms the gain (or a raise went
                                 // non-linear), back to 1 on a decision or a trigger
    int no_verdict = 0;          // pairs in a row without a verdict
    bool last_up = false;        // the last accepted move was up
    int probe_g = -999;          // the probe this sweep (floor_wanted), its sweep
    uint32_t probe_sweep = ~0u;
    // a probe waiting for its closing baseline
    int pend_g = -999;
    float pend_p = 0, pend_f = 0, pend_bp = 0, pend_bf = 0;
    uint32_t pend_sweep = ~0u;
    bool pend_clipped = false;
    int agree_dir = 0, agree_g = -999, agree_n = 0;   // pairs that agreed so far (+1 down, -1 up)
    float agree_dev = 0;         // ... the last one's deviation from the step
    float br_p[8] = {}, br_f[8] = {};   // the last 8 brackets' changes (power, floor), dB
    uint32_t br_n = 0;
    uint8_t bracket_hist = 0;    // the last 8 brackets, 1 = no quantity bracketed (the content moved by itself)
    int bracket_n = 0;
};
std::map<uint32_t, FloorEntry> floor_map;   // the hop thread only
uint32_t sweep_idx = 0;                     // sweeps completed (the hop thread)
uint32_t ag_cur_probes = 0, ag_cur_floor_decisions = 0, ag_cur_floor_hops = 0, ag_cur_search = 0, ag_cur_pairs = 0,
         ag_cur_bad_pairs = 0, ag_cur_nonlinear = 0, ag_cur_learned = 0, ag_cur_untestable = 0, ag_cur_guesses = 0;
std::vector<float> ag_cur_scatter;
std::atomic<uint32_t> ag_probes{0}, ag_floor_decisions{0}, ag_floor_hops{0}, ag_search_hops{0}, ag_pairs{0},
    ag_bad_pairs{0}, ag_nonlinear{0}, ag_learned{0}, ag_untestable{0}, ag_guesses{0};
std::atomic<float> ag_floor_scatter{0.0f};
std::atomic<bool> ag_probe_sweep{false};   // the sweep running is a whole-plan probe sweep (search phase)
// the tuners whose gain now is a probe's (a trial gain): the 8091 header's
// bits 14.. (read with the gains, before the flag)
std::atomic<uint32_t> trial_mask{0};

// The real gain of tuner t at table value g, dB (the R820T / R828D curve,
// interpolated; other tuners: the table)
float real_gain_db(int t, int g) {
    if (t < static_cast<int>(devices.size()) && devices[t] && devices[t]->dev) {
        const rtlsdr_tuner ty = rtlsdr_get_tuner_type(devices[t]->dev);
        if (ty == RTLSDR_TUNER_R820T || ty == RTLSDR_TUNER_R828D) {
            const size_t n = sizeof(R82XX_REAL) / sizeof(R82XX_REAL[0]);
            if (g <= R82XX_REAL[0].g) return R82XX_REAL[0].db + (g - R82XX_REAL[0].g) / 10.0f;
            for (size_t i = 1; i < n; i++)
                if (g <= R82XX_REAL[i].g) {
                    const RealGain& a = R82XX_REAL[i - 1];
                    const RealGain& b = R82XX_REAL[i];
                    return a.db + (b.db - a.db) * static_cast<float>(g - a.g) / static_cast<float>(b.g - a.g);
                }
            return R82XX_REAL[n - 1].db + (g - R82XX_REAL[n - 1].g) / 10.0f;
        }
    }
    return g / 10.0f;
}

float over_converter(float x, float c);

// The gain the measured rule wants for this visit of hop hz (<= clip_g, the
// clipping rule's): the hop's own gain, or this sweep's probe. The same
// answer for the same sweep (asked twice: before and under the lock).
int floor_wanted(int t, uint32_t hz, int clip_g) {
    const std::vector<int>& L = gains_of(t);
    auto it = floor_map.find(hz);
    if (it == floor_map.end()) return clip_g;   // a first visit: a baseline at the clipping rule's gain
    FloorEntry& e = it->second;
    const int base_g = quantize_down(L, e.g >= 0 ? std::min(e.g, clip_g) : clip_g);
    if (e.probe_sweep == sweep_idx) return e.probe_g != -999 ? e.probe_g : base_g;
    e.probe_sweep = sweep_idx;
    e.probe_g = -999;
    // a probe needs the baseline visit just before it and none pending
    if (e.base_sweep != sweep_idx - 1 || e.pend_g != -999) {
        static const bool trace = std::getenv("HEIMDALL_AG_TRACE") != nullptr;
        if (trace && e.agree_n > 0)
            std::cout << "AGW " << sweep_idx << " hz " << hz << " base " << base_g << " agree " << e.agree_n << " base_sweep "
                      << e.base_sweep << " pend " << e.pend_g << std::endl;
        return base_g;
    }
    bool due = false, down = true;
    int step = AG_FLOOR_STEP;
    // Up-probes stay under the clipping rule's gain - except in the up search
    // that a raise of the clipping rule started (its hold was clean: the
    // fake's keyed carrier pumped the hop up and down when every up-probe
    // could pass it): then up to the ceiling, past that gain. Such a probe
    // tests clipping too (one that clips is refused), and one that passes
    // lifts the clipping rule's gain with it (floor_lift_clip) - the one way
    // up for both rules. Not while the clipping rule blocks probes (a probe
    // of its own clipped).
    // A hop that clipped again after a raise (its hold doubled: a keyed
    // carrier) climbs at the clipping rule's own pace - one raise a hold.
    int ceil_q = (e.search && e.up_search && e.clip_lift) ? quantize_down(L, current_gain.load()) : clip_g;
    if (auto gm = gain_map.find(hz);
        gm != gain_map.end() && (now_ns() < gm->second.no_probe_until || gm->second.hold_visits > AG_HOLD_VISITS))
        ceil_q = clip_g;
    if (e.search) {
        if (e.up_search && base_g >= std::min(ceil_q, e.up_lim)) { e.search = false; e.up_search = false; return base_g; }   // back at the top
        due = (sweep_idx & 1u) == 1u;   // the whole plan, in alternate sweeps
        step = e.step;
        down = !e.up_search;
    } else {
        // (a hop held under the clipping rule's gain is probed more often:
        // when its cause has gone it comes back sooner)
        const uint32_t period = (base_g < ceil_q ? std::max<uint32_t>(AG_PROBE_PERIOD / 3, 8) : AG_PROBE_PERIOD) * e.hold;
        due = (sweep_idx + (hz / 1000u) * 2654435761u % period) % period == 0 || e.agree_n > 0;
        down = e.agree_n > 0 ? e.agree_dir > 0 : (!e.next_up || base_g >= ceil_q);
    }
    if (!due) return base_g;
    int pg;
    const float rb = real_gain_db(t, base_g);
    bool again = e.agree_n > 0 && e.agree_g != -999;
    if (again) {
        pg = std::min(e.agree_dir > 0 ? clip_g : ceil_q, e.agree_g);   // the same probe again (the second pair)
        // ... unless the hop's gain has moved past it meanwhile (the clipping
        // rule stepped down onto it): then the agreement is void - on the
        // hardware it asked for a probe at the hop's own gain forever, and
        // 417.43 MHz never had another pair
        if ((e.agree_dir > 0 && pg >= base_g) || (e.agree_dir < 0 && pg <= base_g)) {
            e.agree_n = 0;
            e.agree_dir = 0;
            e.agree_g = -999;
            again = false;
            down = e.search ? !e.up_search : (!e.next_up || base_g >= ceil_q);
        }
    }
    if (again) {
    } else if (down) {
        // at least AG_LIN_MIN_STEP real dB down (the list has steps of 0 dB)
        // (a smaller step when the big one would not be testable: the quantity
        // stays over the validity level for a step of 2.5 dB more often)
        auto down_to = [&](int st) {
            int q = quantize_down(L, base_g - st);
            while (q > (L.empty() ? 0 : L.front()) && rb - real_gain_db(t, q) < AG_LIN_MIN_STEP) q = quantize_down(L, q - 1);
            return q;
        };
        auto testable = [&](int q) {
            const float d = rb - real_gain_db(t, q);
            return (std::isfinite(e.base_p) && over_converter(e.base_p, AG_CONV_P_DB) - d >= AG_CONV_P_DB + AG_LIN_OVER) ||
                   (std::isfinite(e.base_f) && over_converter(e.base_f, AG_CONV_F_DB) - d >= AG_CONV_F_DB + AG_LIN_OVER);
        };
        pg = down_to(step);
        if (e.agree_n == 0 && !testable(pg)) pg = down_to(std::min(step, AG_FLOOR_STEP));
        if (e.agree_n == 0 && !testable(pg)) pg = down_to(1);
        // A probe that can't tell anything only costs (a visit several dB
        // lower where every dB is sensitivity): when the baseline's power and
        // floor, taken down by the step, would both be under the validity
        // level, there is no down-probe (the FM band, the quiet L band: 3 of
        // 44 and 0 of 400 hops testable on the bracketed reference). A search
        // ends there - the hop stays; settled, the next probe is an up one.
        if (e.agree_n == 0) {
            if (!testable(pg)) {
                if (e.search) { e.search = false; e.up_search = false; e.moved_big = false; }
                e.next_up = true;
                ag_cur_untestable++;
                return base_g;
            }
        }
    } else {
        // (a search: its step - the bisection's is small, below the gain it
        // left; settled: a hop held down probes a big step, one at the top a small one)
        const int top = e.search ? std::min(ceil_q, quantize_down(L, e.up_lim - 1)) : ceil_q;
        pg = std::min(top, quantize_down(L, base_g + (e.search ? step : base_g < ceil_q ? AG_FLOOR_STEP_BIG : AG_FLOOR_STEP)));
        while (pg < top && real_gain_db(t, pg) - rb < AG_LIN_MIN_STEP) pg = std::min(top, next_up(L, pg));
        // (straight to the top when what would be left over is a step too small
        // to test - 48.0 -> 49.6 near a cause is refused, and 419.34 MHz stopped
        // at 48.0 after its guess was released)
        if (pg < top && real_gain_db(t, top) - real_gain_db(t, pg) < AG_LIN_MIN_STEP) pg = top;
        if (pg <= base_g && e.search) { e.search = false; e.up_search = false; return base_g; }   // nothing left to try
    }
    if (pg == base_g) return base_g;
    e.probe_g = pg;
    return pg;
}

// Whether gain g on hop hz this sweep is the hop's probe (the trial flag)
bool floor_is_trial(uint32_t hz, int g) {
    auto it = floor_map.find(hz);
    return it != floor_map.end() && it->second.probe_sweep == sweep_idx && it->second.probe_g == g;
}

// The learned steps: (10 MHz bucket, the higher gain, the lower gain) -> the
// last AG_STEP_RING measured steps of valid pairs (power and floor, the
// converter subtracted; the hop thread only)
struct StepHop {
    uint32_t hz;
    float v;   // that hop's measured step (the mean of its last two pairs)
};
struct StepRing {
    std::vector<StepHop> h;   // one value per hop (at most AG_STEP_HOPS)
};
// (bucket, the higher gain, the lower gain, 0 = power / 1 = floor)
std::map<std::tuple<uint32_t, int, int, int>, StepRing> step_map;   // cleared when the plan or its tuners change

// The reference step of quantity q (0 power, 1 floor: each its own - a
// strong carrier and the floor need not move alike) for a pair of hop hz
// between gains hi > lo: the median
// of the OTHER hops' measured steps in its bucket and the two beside it -
// with at least AG_STEP_MIN_HOPS of them (a saturated hop must not teach its
// own reference its wrong step: on a narrow plan the buckets hold 2 - 3
// hops), within AG_STEP_TRUST of the curve's dg; else dg
float step_ref(uint32_t hz, int hi, int lo, int q, float dg, bool* learned = nullptr) {
    const uint32_t b = hz / AG_STEP_BUCKET_HZ;
    float v[3 * AG_STEP_HOPS];
    int n = 0;
    for (uint32_t k = b > 0 ? b - 1 : 0; k <= b + 1; k++) {
        auto it = step_map.find({k, hi, lo, q});
        if (it == step_map.end()) continue;
        for (const StepHop& s : it->second.h)
            if (s.hz != hz) v[n++] = s.v;
    }
    if (learned) *learned = false;
    if (n < AG_STEP_MIN_HOPS) return dg;
    std::nth_element(v, v + n / 2, v + n);
    const float med = v[n / 2];
    if (std::fabs(med - dg) > AG_STEP_TRUST) return dg;
    if (learned) *learned = true;
    return med;
}
void step_note(uint32_t hz, int hi, int lo, int q, float m) {
    if (step_map.size() > 4096) step_map.clear();
    std::vector<StepHop>& h = step_map[{hz / AG_STEP_BUCKET_HZ, hi, lo, q}].h;
    for (StepHop& s : h)
        if (s.hz == hz) { s.v = 0.5f * (s.v + m); return; }
    if (h.size() >= AG_STEP_HOPS) h.erase(h.begin());
    h.push_back({hz, m});
}

// A cause for compression near hop hz: a hop within AG_LIN_CAUSE_HZ whose
// last visit's power, referred to the input and taken to the real gain
// rg_hi, reaches AG_LIN_CAUSE_DB (clipped visits count: they are strong)
bool floor_cause(uint32_t hz, float rg_hi);

// x dB with the converter's own noise c dB taken away (-inf: not over it)
float over_converter(float x, float c) {
    return x > c + 0.3f ? 10.0f * std::log10(std::pow(10.0f, x / 10.0f) - std::pow(10.0f, c / 10.0f))
                        : -std::numeric_limits<float>::infinity();
}

// An up-probe past the clipping rule's gain passed (clean and linear): the
// clipping rule takes that gain too - a clip after it is a clip after a
// raise (its hold doubles)
void floor_lift_clip(uint32_t hz, int g) {
    auto it = gain_map.find(hz);
    if (it == gain_map.end()) return;
    // (kept at the ceiling too: the entry carries the hold - erased here, a
    // keyed carrier found a fresh 20 s hold each time and pumped the hop up
    // and down; the clipping rule forgets it after a clean hold up there)
    GainEntry& c = it->second;
    if (g <= c.gain) return;
    c.gain = g;
    c.last_raise = true;
    c.clean = 0;
    c.peak_max_dbfs = -200; c.rms_max_dbfs = -200;
    c.probes = 0;
    c.probe_from = -1;
    ag_cur_changes++;
}

// A pair without a verdict (a quantity over the converter but unbracketed
// or unsteady - the LTE block -, a failed bracket, the reliability gate)
// still uses the search up: after AG_LIN_NO_VERDICT in a row the search ends
// and the hop stays (on the first hardware run 16 - 18 such hops probed 6 -
// 8 dB down every other sweep for good); settled, the period doubles, as
// for a confirming probe.
void floor_no_verdict(FloorEntry& e) {
    if (++e.no_verdict < AG_LIN_NO_VERDICT) return;
    e.no_verdict = 0;
    if (e.search) { e.search = false; e.up_search = false; e.moved_big = false; }
    else e.hold = std::min(e.hold * 2, AG_PROBE_HOLD_MAX);
    e.agree_n = 0;
    e.agree_dir = 0;
    e.agree_g = -999;
}

// A bracketed pair of hop hz. hi / lo: the higher and the lower gain of the
// pair (one of them the probe, the other the mean of the baselines around
// it), power and floor in the samples; bp / bf: whether each quantity's
// baselines agreed.
void floor_pair(FloorEntry& e, int t, uint32_t hz, int base_g, int clip_g, int pg, float p_hi, float p_lo, float f_hi,
                float f_lo, bool bp, bool bf, bool clipped) {
    const int dir = pg < base_g ? 1 : -1;   // +1: a probe down
    // more power at the lower gain by AG_LIN_BURST: a burst in one of the
    // visits (a pager keyed in the probe), not physics - no pair, and it
    // counts as a failed bracket
    if (p_lo > p_hi + AG_LIN_BURST) {
        ag_cur_bad_pairs++;
        e.bracket_hist |= 1u;
        return;
    }
    ag_cur_pairs++;
    const int g_hi = std::max(base_g, pg), g_lo = std::min(base_g, pg);
    const float dg = real_gain_db(t, g_hi) - real_gain_db(t, g_lo);
    // the tests that count: bracketed, and the quantity (the converter's
    // noise subtracted) well over the converter's own at the lower gain
    const float ps_lo = over_converter(p_lo, AG_CONV_P_DB), fs_lo = over_converter(f_lo, AG_CONV_F_DB);
    const bool lvl_p = ps_lo >= AG_CONV_P_DB + AG_LIN_OVER, lvl_f = fs_lo >= AG_CONV_F_DB + AG_LIN_OVER;
    // Steady: a quantity whose brackets' median change (the last 8) is over
    // AG_LIN_STEADY moves by itself (LTE load, a self-varying floor): a
    // bracket that agreed by chance and a probe that didn't made "agreeing"
    // non-linear pairs there on the fake - such a quantity judges nothing
    auto med8 = [&](const float* v) {
        const int n = static_cast<int>(std::min<uint32_t>(e.br_n, 8));
        if (n < 3) return 0.0f;
        float w[8];
        std::copy(v, v + n, w);
        std::nth_element(w, w + n / 2, w + n);
        return w[n / 2];
    };
    const bool steady_p = med8(e.br_p) <= AG_LIN_STEADY, steady_f = med8(e.br_f) <= AG_LIN_STEADY;
    const bool vp = bp && lvl_p && steady_p;
    const bool vf = bf && lvl_f && steady_f;
    // a quantity over the converter whose bracket failed: this pair can't
    // call the step linear (it can still be non-linear by the other)
    const bool unknown = (lvl_p && !vp) || (lvl_f && !vf);
    const float mp = vp ? over_converter(p_hi, AG_CONV_P_DB) - ps_lo : 0.0f;
    const float mf = vf ? over_converter(f_hi, AG_CONV_F_DB) - fs_lo : 0.0f;
    // the reference: the learned step (this pair's own values among them)
    if (vp) step_note(hz, g_hi, g_lo, 0, mp);
    if (vf) step_note(hz, g_hi, g_lo, 1, mf);
    bool learned_p = false, learned_f = false;
    const float r = step_ref(hz, g_hi, g_lo, 0, dg, &learned_p);    // the power's
    const float rf = step_ref(hz, g_hi, g_lo, 1, dg, &learned_f);   // the floor's
    if (learned_p || learned_f) ag_cur_learned++;
    // One-sided, when only the HIGHER gain's value is well over the
    // converter: the lower one's raw value (converter included) bounds its
    // own from above, so (higher - raw lower) is a lower bound on the step -
    // a floor that falls faster than the gain (third-order products going
    // into the converter's noise) is seen; a step that looks too small near
    // the converter is not judged (the FM band: the tuner's noise barely
    // over the converter's)
    const float ps_hi = over_converter(p_hi, AG_CONV_P_DB), fs_hi = over_converter(f_hi, AG_CONV_F_DB);
    const float steep_p = (!vp && bp && steady_p && ps_hi >= AG_CONV_P_DB + AG_LIN_OVER) ? (ps_hi - p_lo) - r : 0.0f;
    const float steep_f = (!vf && bf && steady_f && fs_hi >= AG_CONV_F_DB + AG_LIN_OVER) ? (fs_hi - f_lo) - rf : 0.0f;
    // Compression ("follows less than the step") needs a cause: content of
    // at least AG_LIN_CAUSE_DB rms at the higher gain in this hop or one
    // within the IF's reach (the orchestrator's bracketed reference,
    // 2026-11-24: 26.87 MHz at -35.5 dBFS and 633.13 at -32.1 follow a short
    // step with nothing strong near - the gain's own shape there, not
    // saturation; 417.02 has -9.0 itself, 944.87 -9 .. -13 and a clipping
    // neighbour). Products ("more than the step") need none.
    const bool cause = p_hi >= AG_LIN_CAUSE_DB || floor_cause(hz, real_gain_db(t, g_hi));
    // (against the curve, not a learned step, AG_LIN_CURVE_SLACK more on the
    // fixed tolerance: the curve is off by up to 1.3 dB a step - 5.5 against
    // 4.2 dB at 174 - 380 MHz)
    // Products (more than the step): max(1.5 dB, half the step) for the
    // power, 1.5 dB for the floor. Compression (less than the step, a cause
    // near): the SMALLER of 1.5 dB and half the step - compression adds up
    // over a step, a bigger step earns no bigger tolerance (with the larger,
    // 37.2 -> 33.8 following 0.7 of 2.9 dB read linear on the hardware, the
    // hop stayed 2.8 dB compressed). +AG_LIN_CURVE_SLACK on the 1.5 dB while
    // the reference is the curve. A raise must be clearly linear: an
    // up-probe's compression tolerance is at most AG_LIN_DEV_UP (the power:
    // or a quarter of the step) + the slack - with half the step the fake's
    // 417.43 MHz came up 28.0 -> 32.8, 2.1 dB compressed.
    const float slack_p = learned_p ? 0.0f : AG_LIN_CURVE_SLACK, slack_f = learned_f ? 0.0f : AG_LIN_CURVE_SLACK;
    const float tol_p = std::max(AG_LIN_DEV, AG_LIN_REL * r) + slack_p;
    const float tol_f = AG_LIN_DEV + slack_f;
    float tol_cp = std::min(AG_LIN_DEV + slack_p, AG_LIN_REL_COMP * r);
    float tol_cf = std::min(AG_LIN_DEV + slack_f, AG_LIN_REL_COMP * rf);
    if (dir < 0) {
        tol_cp = std::min(tol_cp, std::max(AG_LIN_DEV_UP, 0.5f * AG_LIN_REL * r) + slack_p);
        tol_cf = std::min(tol_cf, AG_LIN_DEV_UP + slack_f);
    }
    const bool nl_p = (vp && ((mp - r >= tol_p) || (cause && r - mp >= tol_cp))) || steep_p >= tol_p;
    const bool nl_f = (vf && ((mf - rf >= tol_f) || (cause && rf - mf >= tol_cf))) || steep_f >= tol_f;
    // gross (one pair decides): a big step the power follows by less than
    // half (no fine reference needed), or a floor far off
    const bool gross = (vp && cause && dg >= AG_LIN_GROSS_STEP && mp < AG_LIN_COMP * dg) ||
                       (vf && (mf - rf >= AG_LIN_GROSS || (cause && rf - mf >= AG_LIN_GROSS))) ||
                       steep_f >= AG_LIN_GROSS || steep_p >= AG_LIN_GROSS;
    // A step under AG_LIN_MIN_STEP (only up to the ceiling: 48.0 -> 49.6 is
    // 1.4 dB on the curve, -1 .. 2 dB in fact) can't show compression: with
    // a cause near, it is not taken (the hop stays 1.4 dB low rather than
    // compressed - the fake's 413.61 MHz came up 3.5 dB compressed that way)
    const bool untestable = dg < AG_LIN_MIN_STEP && cause;
    const bool nonlinear = nl_p || nl_f || gross || untestable;
    if (nonlinear) ag_cur_nonlinear++;
    // HEIMDALL_AG_TRACE=1: one stdout line per pair (a diagnostic; stdout is
    // kept only on a terminal or with HEIMDALL_VERBOSE_LOG=1)
    static const bool trace = std::getenv("HEIMDALL_AG_TRACE") != nullptr;
    if (trace)
        std::cout << "AGT " << sweep_idx << " hz " << hz << " t " << t << " base " << base_g << " pg " << pg << " clip " << clip_g
                  << " dg " << dg << " r " << r << (learned_p ? "L" : "C") << " rf " << rf << (learned_f ? "L" : "C") << " P " << p_hi << "/" << p_lo << " F " << f_hi << "/" << f_lo
                  << " vp " << vp << " vf " << vf << " mp " << mp << " mf " << mf << " sp " << steep_p << " sf " << steep_f
                  << " nl " << nonlinear << " gross " << gross << " unk " << unknown << " cause " << cause << " search " << e.search << " up " << e.up_search
                  << " bn " << e.bracket_n << " bh " << int(e.bracket_hist) << " cap " << e.nb_cap << " hold " << e.hold << std::endl;
    auto take = [&](int g) {
        if (g > clip_g) {
            // (an up-probe past the clipping rule's gain, clean: that rule
            // takes the gain too - and that is not a raise of its own, which
            // would hold the hop back and start an up search again: 499.26
            // MHz climbed in a sawtooth that way on the hardware)
            floor_lift_clip(hz, g);
            e.last_clip_g = g;
        }
        e.g = (g >= clip_g) ? -1 : g;
        e.agree_n = 0;
        e.agree_dir = 0;
        e.agree_g = -999;
        ag_cur_floor_decisions++;
    };
    // two pairs agree: the same probe, and (down) values within AG_LIN_DEV -
    // a burst can't make two by chance
    auto agreed = [&]() {
        const float dev = std::max(vp ? std::fabs(r - mp) : -99.0f, vf ? std::fabs(mf - rf) : -99.0f);
        if (e.agree_dir == dir && e.agree_g == pg && (dir < 0 || std::fabs(dev - e.agree_dev) <= AG_LIN_DEV)) {
            e.agree_dev = dev;
            return ++e.agree_n >= 2;
        }
        e.agree_dir = dir;
        e.agree_g = pg;
        e.agree_n = 1;
        e.agree_dev = dev;
        return false;
    };
    auto reset = [&]() { e.agree_n = 0; e.agree_dir = 0; e.agree_g = -999; };
    // An up-probe where nothing can be tested (no quantity both over the
    // converter and bracketed and steady) and that is not non-linear: the clip
    // test alone decides, as before this rule - a raise of the clipping rule
    // must not be lost because the hop's content moves by itself (415.11 MHz
    // on the hardware: cut to 8.7 dB by a heavy clip, its power alternating
    // -20.3 / -17.4 dBFS from visit to visit, every up-probe without a
    // verdict - it stayed at 8.7 for the whole run)
    // ("can't be tested": each quantity under the validity level or unsteady -
    // not merely this pair's bracket failing: 417.43 MHz, its power pinned
    // but one bracket off, went up 33.8 -> 38.6 by the clip test alone)
    // (and not past the lower neighbour's level guess: with nothing testable
    // the guess is the better knowledge - a hop with weak content beside a
    // saturating carrier went straight back up into it on the fake)
    const bool up_untestable = dir < 0 && !nonlinear && (!lvl_p || !steady_p) && (!lvl_f || !steady_f) && pg <= e.nb_cap;
    if (unknown && !nonlinear && !up_untestable) {
        e.bracket_hist |= 1u;
        floor_no_verdict(e);
        return;
    }
    // Decisions only on a hop whose brackets mostly hold (at most a quarter
    // of the last 8 failed - content that moves by itself fails most); one
    // gross pair decides alone only when the last 4 held
    const int nb = std::min(e.bracket_n, 8);
    const int fails = std::popcount(static_cast<unsigned>(e.bracket_hist & ((1u << nb) - 1u)));
    if (!(nb >= 1 && fails * 4 <= nb) && !up_untestable) { floor_no_verdict(e); return; }
    const bool alone_ok = nb >= 4 && (e.bracket_hist & 0x0Fu) == 0;
    e.no_verdict = 0;
    if (dir > 0) {   // the step from the probe up to the hop's gain
        if (nonlinear && ((gross && alone_ok) || agreed())) {
            // (down again after a raise: the period doubles; else a decision starts it over)
            e.hold = e.last_up ? std::min(e.hold * 2, AG_PROBE_HOLD_MAX) : 1;
            e.last_up = false;
            take(pg);
            // (the same evidence must not cost a step twice: a mild clip the
            // clipping rule noted at the old gain is spent by this move)
            mild_clips.erase(hz);
            e.search = true;      // on down while it stays non-linear
            e.up_search = false;
            e.step = gross ? AG_FLOOR_STEP_BIG : AG_FLOOR_STEP;
            e.up_lim = base_g;    // (the gain left is non-linear: the bisection stays under it)
            e.clip_lift = false;
            e.moved_big = e.moved_big || base_g - pg > AG_FLOOR_STEP + 5;
        } else if (nonlinear) {
            // the first of two: the same probe again at its next slot
        } else {
            reset();
            e.next_up = true;
            if (!e.search) e.hold = std::min(e.hold * 2, AG_PROBE_HOLD_MAX);   // it confirmed where the hop is
            if (e.search && !e.up_search && e.step > AG_FLOOR_STEP) {
                // linear over a BIG step: compression that a 6 dB step hides
                // inside its tolerance (min(1.5 dB + slack, half the step))
                // still shows over a small one - the search goes on with a
                // small step from here (on the hardware 417.43 MHz passed
                // 37.2 -> 29.7 following 4.5 of 6.7 dB and then waited 26 - 50
                // sweeps for the settled probes to find 37.2 -> 33.8 non-linear)
                e.step = AG_FLOOR_STEP;
            } else if (e.search && !e.up_search) {
                // linear below: the hop's gain holds - after a big step down,
                // back up by small steps while that stays linear (the bisection)
                if (e.moved_big) {
                    e.up_search = true;
                    e.step = AG_FLOOR_STEP;
                } else {
                    e.search = false;
                }
                e.moved_big = false;
            }
        }
    } else {         // the step from the hop's gain up to the probe
        // everything near the converter's own noise (more gain can only help),
        // or nothing testable: one clean pair is enough
        if (pg > e.nb_cap && !vp && !vf) {
            // past the lower neighbour's level guess with nothing to test: stays
            reset();
            e.next_up = false;
            if (e.up_search) { e.search = false; e.up_search = false; }
            else e.hold = std::min(e.hold * 2, AG_PROBE_HOLD_MAX);
            return;
        }
        const bool none_valid = (!lvl_p && !lvl_f) || up_untestable;
        if (!clipped && !nonlinear && (none_valid || agreed())) {
            e.last_up = true;
            e.hold = 1;
            take(pg);   // (an up search goes on up while it stays linear)
            if (!e.search) {
                // a settled hop that came up: on up by big steps while that
                // stays linear and clean (over-reduced hops come back in a
                // few probe sweeps, not one step a probe period)
                e.search = true;
                e.up_search = true;
                e.step = AG_FLOOR_STEP_BIG;
                e.up_lim = 1 << 30;
                e.clip_lift = false;
            }
        } else if (!clipped && !nonlinear) {
            // the first of two
        } else {
            reset();
            e.next_up = false;
            if (e.up_search && e.step > AG_FLOOR_STEP) {
                e.step = std::max(AG_FLOOR_STEP, e.step / 2);   // refused: the step below (a level-sized raise's fall-back)
            } else if (e.up_search) {
                e.search = false;
                e.up_search = false;
            } else {
                e.hold = std::min(e.hold * 2, AG_PROBE_HOLD_MAX);
            }
        }
    }
}

bool floor_cause(uint32_t hz, float rg_hi) {
    for (auto it = floor_map.lower_bound(hz > AG_LIN_CAUSE_HZ ? hz - AG_LIN_CAUSE_HZ : 0);
         it != floor_map.end() && it->first <= hz + AG_LIN_CAUSE_HZ; ++it)
        if (std::isfinite(it->second.lvl_in) && it->second.lvl_in + rg_hi >= AG_LIN_CAUSE_DB) return true;
    return false;
}

// A visit -> the hop's entry (the hop thread, after the dwell)
void floor_learn(int t, const HopAcc& a, int clip_g) {
    if (a.floor_n == 0 || a.gain < 0 || a.values == 0) return;
    const std::vector<int>& L = gains_of(t);
    const float f = static_cast<float>(a.floor_db / a.floor_n);
    const float p = 10.0f * std::log10(static_cast<float>(static_cast<double>(a.sumsq) / a.values) / (2.0f * 127.5f * 127.5f) + 1e-30f);
    const float rg = real_gain_db(t, a.gain);
    const float f_in = f - rg, p_in = p - rg;
    auto [it, fresh] = floor_map.try_emplace(a.hz);
    FloorEntry& e = it->second;
    if (fresh && floor_map.size() > AG_MAP_MAX) { floor_map.clear(); return; }
    e.lvl_in = p_in;   // (every visit, clipped ones too: the cause scan)
    e.tuner = t;
    if (static_cast<double>(a.clipped) / a.values <= AG_LIN_HEAVY_CLIP) {
        e.lvl_ring[e.lvl_n++ % 4] = p_in;
        e.car_off_hz = a.pk_off_hz;
        e.car_over_db = a.pk_over_db;
        const uint32_t k = e.wc_n++ % 2;
        e.wc_ok[k] = a.win_over_db >= AG_NB_CARRIER_DB;
        e.wc_off[k] = a.win_off_hz;
        e.wc_lvl[k] = a.win_pow_db - rg;
    }
    // ONE way up: when the clipping rule raises its own gain (its probes
    // after a step down, its +3 dB after a clean hold), the hop stays where
    // it is and the raise becomes this rule's up search - each step up a
    // probe that must be linear where anything is testable and unclipped.
    // (Not a raised ceiling: the common gain going up lifts every hop at once.)
    {
        const int ceil_q = quantize_down(L, current_gain.load());
        if (e.last_clip_g >= 0 && clip_g > e.last_clip_g && e.last_clip_g < e.last_ceil && ceil_q == e.last_ceil &&
            (e.g < 0 || e.g >= e.last_clip_g)) {
            e.g = e.last_clip_g;
            e.search = true;
            e.up_search = true;
            e.step = std::max(AG_FLOOR_STEP, clip_g - e.last_clip_g);   // (one probe to the clipping rule's target)
            e.up_lim = 1 << 30;
            e.clip_lift = true;
            e.agree_n = 0;
            e.agree_dir = 0;
            e.agree_g = -999;
        }
        e.last_clip_g = clip_g;
        e.last_ceil = ceil_q;
    }
    const int base_g = quantize_down(L, e.g >= 0 ? std::min(e.g, clip_g) : clip_g);
    const bool clipped = static_cast<double>(a.clipped) / a.values > AG_CLIP_SHARE;
    if (e.search) ag_cur_search++;
    if (e.probe_sweep == sweep_idx && e.probe_g != -999 && a.gain == e.probe_g && a.gain != base_g) {
        // a probe: waits for the baseline after it
        ag_cur_probes++;
        e.pend_g = a.gain;
        e.pend_p = p;
        e.pend_f = f;
        e.pend_bp = e.base_p;
        e.pend_bf = e.base_f;
        e.pend_sweep = sweep_idx;
        // (an up-probe must also leave the clipping rule's headroom: its peak
        // at most AG_RAISE_HEADROOM_DBFS - 415.11 MHz passed an up-probe at
        // 28.0 dB with an unclipped carrier at -2.6 dBFS rms, and its next
        // visits there clipped 1 - 5 %)
        const float peak_dbfs = a.peak > 0 ? 20.0f * std::log10(static_cast<float>(a.peak) / 255.0f) : -200.0f;
        e.pend_clipped = clipped || (a.gain > base_g && peak_dbfs > AG_RAISE_HEADROOM_DBFS);
        return;
    }
    static const bool trace_l = std::getenv("HEIMDALL_AG_TRACE") != nullptr;
    if (trace_l && e.agree_n > 0)
        std::cout << "AGL " << sweep_idx << " hz " << a.hz << " gain " << a.gain << " base " << base_g << " clip " << clip_g
                  << " share " << static_cast<double>(a.clipped) / a.values << " probe " << e.probe_g << "/" << e.probe_sweep
                  << " pend " << e.pend_g << std::endl;
    if (a.gain != base_g) {
        // neither (the gain changed meanwhile: the clipping rule stepped) - a
        // pending probe is cancelled, and that is a pair without a verdict
        if (e.pend_g != -999) { e.pend_g = -999; floor_no_verdict(e); }
        return;
    }
    // A baseline visit. One clipped heavily (over AG_LIN_HEAVY_CLIP) is the
    // clipping rule's alone: a pending probe is cancelled (a pair without a
    // verdict). A MILD clip (a few values at 0 / 255) is still a baseline:
    // compression with a few clipped values is the case this test is for,
    // and its verdict (down) goes the way the clipping rule would, only
    // sooner. (Until 2026-11-24 any clip over 0.01 % dropped the baseline:
    // 417.43 MHz, clipping 0.06 - 0.1 % now and then at 43.4 - 49.6, never
    // closed a pair after its first move and sat 7 dB compressed at 43.4 for
    // a whole one-tuner run - the orchestrator's acceptance set on 1aa6ecc.)
    if (static_cast<double>(a.clipped) / a.values > AG_LIN_HEAVY_CLIP) {
        if (e.pend_g != -999) { e.pend_g = -999; floor_no_verdict(e); }
        return;
    }
    if (e.base_sweep == sweep_idx - 1 && std::isfinite(e.base_fin)) {
        // (a median: one jump - a cause keyed up - doesn't move it; content
        // that moves by itself from visit to visit does)
        e.dif[e.dif_n % 8] = std::fabs(f_in - e.base_fin);
        e.dif_n++;
        float v[8];
        const int n = std::min(e.dif_n, 8);
        std::copy(e.dif, e.dif + n, v);
        std::nth_element(v, v + n / 2, v + n);
        e.scatter = v[n / 2];
        ag_cur_scatter.push_back(e.scatter);
    }
    // the triggers: two baselines in a row AG_FLOOR_JUMP over the hop's slow
    // average - its floor or its power referred to the input (a cause keyed
    // up nearby) - reopen the search, down; two AG_FLOOR_DROP under it on a
    // hop held down (the cause has gone): up
    const bool steady = std::isfinite(e.ref) && std::isfinite(e.ref_p) && !e.search && e.scatter <= AG_FLOOR_VAR;
    if (steady && (f_in > e.ref + AG_FLOOR_JUMP || p_in > e.ref_p + AG_FLOOR_JUMP)) {
        if (++e.jump >= 2) {
            e.jump = 0; e.search = true; e.up_search = false; e.step = AG_FLOOR_STEP_BIG;
            e.hold = 1;
            e.up_lim = 1 << 30;
            e.moved_big = false;
            e.clip_lift = false;
            e.ref = f_in;
            e.ref_p = p_in;
        }
    } else {
        e.jump = 0;
    }
    if (steady && (f_in < e.ref - AG_FLOOR_DROP || p_in < e.ref_p - AG_FLOOR_DROP) && base_g < clip_g) {
        if (++e.drop >= 2) {
            e.drop = 0; e.search = true; e.up_search = true; e.ref = f_in; e.ref_p = p_in;
            e.hold = 1;
            e.step = AG_FLOOR_STEP_BIG;
            e.up_lim = 1 << 30;
            e.clip_lift = false;
        }
    } else {
        e.drop = 0;
    }
    if (e.pend_g != -999) {
        if (e.pend_sweep == sweep_idx - 1 && std::isfinite(e.pend_bp) && std::isfinite(e.pend_bf)) {
            const bool bp = std::fabs(p - e.pend_bp) <= AG_FLOOR_BRACKET;
            const bool bf = std::fabs(f - e.pend_bf) <= AG_FLOOR_BRACKET;
            const bool held = bp || bf;
            // the bracket's own change per quantity: its median over the last
            // 8 says whether that quantity is steady enough to judge at all
            e.br_p[e.br_n % 8] = std::fabs(p - e.pend_bp);
            e.br_f[e.br_n % 8] = std::fabs(f - e.pend_bf);
            e.br_n++;
            e.bracket_hist = static_cast<uint8_t>((e.bracket_hist << 1) | (held ? 0u : 1u));
            e.bracket_n = std::min(e.bracket_n + 1, 8);
            if (held) {
                const float pb = 0.5f * (p + e.pend_bp), fb = 0.5f * (f + e.pend_bf);
                if (e.pend_g < base_g)
                    floor_pair(e, t, a.hz, base_g, clip_g, e.pend_g, pb, e.pend_p, fb, e.pend_f, bp, bf, e.pend_clipped);
                else
                    floor_pair(e, t, a.hz, base_g, clip_g, e.pend_g, e.pend_p, pb, e.pend_f, fb, bp, bf, e.pend_clipped);
            } else {
                ag_cur_bad_pairs++;   // the content moved by itself: no pair
                floor_no_verdict(e);
                // four failed brackets of the last four: no search here (it would probe every other sweep for good)
                if ((e.bracket_hist & 0x0Fu) == 0x0Fu && e.search) {
                    e.search = false;
                    e.agree_n = 0;
                }
            }
        } else {
            floor_no_verdict(e);   // (a probe whose bracket can't close)
        }
        e.pend_g = -999;
    }
    // (after a decision the baseline is at the new gain: base_g above was the old one)
    const int now_g = quantize_down(L, e.g >= 0 ? std::min(e.g, clip_g) : clip_g);
    if (now_g == base_g) {
        e.base_p = p;
        e.base_f = f;
        e.base_fin = f_in;
        e.base_sweep = sweep_idx;
        if (!std::isfinite(e.ref)) { e.ref = f_in; e.ref_p = p_in; }
        else if (e.jump == 0 && e.drop == 0) { e.ref = 0.9f * e.ref + 0.1f * f_in; e.ref_p = 0.9f * e.ref_p + 0.1f * p_in; }
    } else {
        e.ref = e.ref_p = NAN;   // (a new gain: new references)
        e.br_n = 0;              // ... and a new steadiness history (bursts at 49.6 made 417.43 MHz's steady power at 37.2 unsteady)
        e.base_sweep = ~0u;      // the next visit at the new gain is the first baseline
        e.base_p = e.base_f = e.base_fin = NAN;
    }
    // The first guess from the lower neighbour's level (the same tuner): the
    // highest list gain at which its carrier, referred to the input, would
    // read at most AG_NB_LEVEL_DBFS - at the hop's first visits, and again when
    // that level has risen by AG_NB_RISE_DB (a transmitter keyed up). Only
    // ever down from where the hop is; the rule's probes go on from there
    // (up-probes bring it back where the guess was too low - a carrier low in
    // the neighbour's band may be attenuated more)
    if (plan_step_hz > 0 && a.hz > plan_step_hz) {
        auto nb = floor_map.find(a.hz - plan_step_hz);
        // Only a carrier within AG_NB_REACH_HZ below this hop's centre: there
        // the IF filter gave 0 dB (carriers 1.38 and 1.79 MHz below); at 2.34 -
        // 3.7 MHz below it was falling (nothing on the reference day at a
        // would-clip level). The level is that carrier's OWN (its bin and the
        // two beside it; the strongest in the window, AG_NB_CARRIER_DB over the
        // floor) - not the band's total, which a stronger carrier outside the
        // window would lend it (418.93 / 419.34 MHz below a band with 416.59
        // outside and the keyed 417.86 inside: guessed to 29.7 for 17 min).
        // A band of flat content (no carrier): its total, at the band's
        // upper edge. Carriers only outside the window: no guess.
        bool have = false, carrier = false;
        float lv = -1e9f, off = 0;
        if (nb != floor_map.end() && nb->second.tuner == t && nb->second.lvl_n > 0) {
            const FloorEntry& n = nb->second;
            for (int k = 0; k < 2; k++)
                if (n.wc_ok[k] && n.wc_lvl[k] > lv) { lv = n.wc_lvl[k]; off = n.wc_off[k]; have = true; carrier = true; }
            if (!have && n.car_over_db < AG_NB_CARRIER_DB) {
                for (uint32_t k = 0; k < std::min<uint32_t>(n.lvl_n, 4); k++) lv = std::max(lv, n.lvl_ring[k]);
                off = static_cast<float>(plan_step_hz) / 2;
                have = true;
            }
        }
        if (nb != floor_map.end() && nb->second.tuner == t && (have || e.nb_cap < (1 << 30))) {
            // the guess's carrier gone (none within AG_NB_FOLLOW_HZ of its offset)
            // or 3 dB down, in both of the neighbour's last two visits: no cap
            // from it (its level at the guess is kept: a carrier that comes and
            // goes guesses again only on a new high)
            if (e.nb_done && e.nb_cap < (1 << 30)) {
                const FloorEntry& n = nb->second;
                bool held = false;
                if (!e.nb_flat) {
                    for (int k = 0; k < 2; k++)
                        if (n.wc_ok[k] && std::fabs(n.wc_off[k] - e.nb_off) <= AG_NB_FOLLOW_HZ &&
                            n.wc_lvl[k] > e.nb_lvl - AG_NB_RISE_DB)
                            held = true;
                } else {
                    held = have && !carrier && lv > e.nb_lvl - AG_NB_RISE_DB;
                }
                static const bool trace_r = std::getenv("HEIMDALL_AG_TRACE") != nullptr;
                if (trace_r)
                    std::cout << "AGR " << sweep_idx << " hz " << a.hz << " held " << held << " wc " << n.wc_ok[0] << "/" << n.wc_off[0]
                              << "/" << n.wc_lvl[0] << " " << n.wc_ok[1] << "/" << n.wc_off[1] << "/" << n.wc_lvl[1] << " nb " << e.nb_off
                              << "/" << e.nb_lvl << std::endl;
                if (!held) {
                    e.nb_cap = 1 << 30;
                    if (e.g >= 0) {
                        // held under it by the guess: back up now (an up search
                        // in the probe sweeps), not at the settled period
                        e.search = true;
                        e.up_search = true;
                        e.step = AG_FLOOR_STEP_BIG;
                        e.up_lim = 1 << 30;
                        e.clip_lift = false;
                        e.agree_n = 0;
                        e.agree_dir = 0;
                        e.agree_g = -999;
                    }
                }
            }
            if (have && (!e.nb_done || lv >= e.nb_lvl + AG_NB_RISE_DB)) {
                static const bool trace_g = std::getenv("HEIMDALL_AG_TRACE") != nullptr;
                if (trace_g)
                    std::cout << "AGG " << sweep_idx << " hz " << a.hz << " nb_lvl " << lv << " off " << off
                              << (carrier ? " carrier" : " flat") << " nb_rms_in " << nb->second.lvl_in << " now " << now_g
                              << std::endl;
                e.nb_done = true;
                e.nb_lvl = lv;
                e.nb_off = off;
                e.nb_flat = !carrier;
                int g = now_g;
                while (g > (L.empty() ? 0 : L.front()) && lv + real_gain_db(t, g) > AG_NB_LEVEL_DBFS) g = quantize_down(L, g - 1);
                e.nb_cap = g < now_g ? g : (1 << 30);
                // (not for less than the smallest probe step: 49.6 -> 48.0 could
                // never be undone - a step that small can't be tested)
                if (g < now_g && real_gain_db(t, now_g) - real_gain_db(t, g) >= AG_LIN_MIN_STEP) {
                    e.g = g;
                    e.search = true;      // the rule goes on from the guess: its first down probe ends it where linear
                    e.up_search = false;
                    e.step = AG_FLOOR_STEP;
                    e.moved_big = false;
                    e.up_lim = 1 << 30;
                    e.agree_n = 0;
                    e.agree_dir = 0;
                    e.agree_g = -999;
                    e.pend_g = -999;
                    e.base_sweep = ~0u;
                    e.base_p = e.base_f = e.base_fin = NAN;
                    e.ref = e.ref_p = NAN;
                    e.br_n = 0;
                    ag_cur_guesses++;
                }
            }
        }
    }
    if (e.g >= 0 && quantize_down(L, std::min(e.g, clip_g)) < clip_g) ag_cur_floor_hops++;
}

// The visits waiting for their spectrum estimate (the hop thread only)
struct FloorPending {
    int t = 0;
    HopAcc a;
    int clip_g = 0;
    uint32_t sweep = 0;
    int age = 0;
};
std::vector<FloorPending> floor_pending;
uint32_t ag_cur_late = 0;
std::atomic<uint32_t> ag_late{0};
// Learn from the visits whose estimate has come (in their order; with the
// sweep they were taken in), at each round's end; one not there after
// AG_EST_MAX_AGE rounds is a visit without a verdict
constexpr int AG_EST_MAX_AGE = 4;
void floor_pending_run() {
    size_t w = 0;
    for (size_t i = 0; i < floor_pending.size(); i++) {
        FloorPending& fp = floor_pending[i];
        EstResult r;
        {
            std::lock_guard<std::mutex> lk(est_mutex);
            r = est_result[fp.t][fp.a.seq % 4];
        }
        if (r.seq == fp.a.seq && r.done) {
            if (std::isfinite(r.floor)) {
                fp.a.floor_db = r.floor;
                fp.a.floor_n = 1;
                fp.a.pk_off_hz = r.pk_off_hz;
                fp.a.pk_over_db = r.pk_over_db;
                fp.a.win_off_hz = r.win_off_hz;
                fp.a.win_over_db = r.win_over_db;
                fp.a.win_pow_db = r.win_pow_db;
            }
            const uint32_t now_sweep = sweep_idx;
            sweep_idx = fp.sweep;   // (it is learned as of the sweep it was taken in)
            floor_learn(fp.t, fp.a, fp.clip_g);
            sweep_idx = now_sweep;
        } else if (r.seq != fp.a.seq || ++fp.age > AG_EST_MAX_AGE) {
            ag_cur_late++;
        } else {
            if (w != i) floor_pending[w] = std::move(fp);
            w++;
        }
    }
    floor_pending.resize(w);
}

// The gain tuner t takes for hop centre hz now: the common gain unless auto
// is active (then the clipping rule's gain, and the linearity search's at or
// under it, on the tuner's own list)
int wanted_gain(int t, uint32_t hz) {
    const int ceiling = current_gain.load();
    if (!auto_gain_on.load() || ceiling < 0) return ceiling;
    const int clip_g = quantize_down(gains_of(t), learned_gain(hz, ceiling));
    return floor_wanted(t, hz, clip_g);
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
        // a probe's raise that clips (even mildly): back to where it was clean
        if (it != gain_map.end() && it->second.probe_from >= 0 && a.gain > it->second.probe_from) {
            GainEntry& e = it->second;
            e.gain = std::min(e.gain, e.probe_from);
            e.probes = 0;
            e.probe_from = -1;
            e.no_probe_until = now + e.hold_ns;
            e.hold_visits = std::min(e.hold_visits * 2, AG_HOLD_VISITS_MAX);
            e.hold_ns = std::min(e.hold_ns * 2, AG_HOLD_NS_MAX);
            e.last_raise = false;
            e.clean = 0;
            e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
            ag_cur_changes++;
            return;
        }
        if (share <= AG_MILD_SHARE) {
            auto m = mild_clips.find(a.hz);
            if (m == mild_clips.end() || now - m->second > AG_MILD_WINDOW_NS) {
                if (mild_clips.size() >= AG_MAP_MAX) mild_clips.clear();
                mild_clips[a.hz] = now;   // the first: noted, no change yet
                if (it != gain_map.end()) { it->second.clean = 0; it->second.peak_max_dbfs = -200; it->second.rms_max_dbfs = -200; }
                return;
            }
            mild_clips.erase(m);          // the second within the window: down
        }
        const int lowest = L.empty() ? 0 : L.front();
        if (a.gain <= lowest) return;   // nothing lower to take
        const bool fresh = it == gain_map.end();
        if (fresh) {
            if (gain_map.size() >= AG_MAP_MAX) gain_map.clear();
            it = gain_map.emplace(a.hz, GainEntry{}).first;
        }
        GainEntry& e = it->second;
        int down = std::max(lowest, quantize_down(L, a.gain - ag_down_step(share)));
        bool blind = true;
        {
            const float rms = 10.0f * std::log10(static_cast<float>(static_cast<double>(a.sumsq) / a.values) /
                                                 (2.0f * 127.5f * 127.5f) + 1e-30f);
            if (share <= AG_LEVEL_CUT_MAX && rms >= AG_LEVEL_CUT_MIN_RMS) {
                // from the level: the smallest step down to AG_LEVEL_TARGET_DBFS, at least one list step
                const float need = rms - AG_LEVEL_TARGET_DBFS, rg = real_gain_db(t, a.gain);
                int g = quantize_down(L, a.gain - 1);
                while (g > lowest && rg - real_gain_db(t, g) < need) g = quantize_down(L, g - 1);
                down = std::max(lowest, g);
                blind = false;
            }
        }
        // (a visit taken before the last change must not undo it)
        e.gain = fresh ? down : std::min(e.gain, down);
        e.blind_from = blind ? std::max(e.blind_from, a.gain) : -1;
        e.probes = now >= e.no_probe_until ? AG_PROBES : 0;
        if (e.last_raise) {   // it clipped again after a raise: wait longer next time
            e.hold_visits = std::min(e.hold_visits * 2, AG_HOLD_VISITS_MAX);
            e.hold_ns = std::min(e.hold_ns * 2, AG_HOLD_NS_MAX);
        }
        e.probe_from = -1;
        e.last_raise = false;
        e.clean = 0;
        e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
        ag_cur_changes++;
        return;
    }
    if (it == gain_map.end()) return;   // at the ceiling and fine
    GainEntry& e = it->second;
    if (a.gain != quantize_down(L, learned_gain(a.hz, ceiling))) return;   // taken with another gain (changed meanwhile)
    if (a.clipped > 0) { e.clean = 0; e.peak_max_dbfs = -200; e.rms_max_dbfs = -200; return; }   // a few clipped values: hold, no raise
    // The first clean visit after a table (blind) cut measures the carrier's
    // level: the gain whose predicted rms stays at AG_LEVEL_TARGET_DBFS is then
    // known - one move there (at least a list step up, never above the gain
    // before the cut), once per cut, and not on a hop whose hold has doubled
    // (a keyed carrier keeps the slow pace). 415.52 MHz on the hardware: cut
    // blind 49.6 -> 29.7 -> 16.6 by 28 % / 13 % clips, clean at 25.4 with rms
    // -7.4 dBFS (carrier over floor 71.3 against 64.9 at 16.6), it sat at
    // 16.6 for a whole run.
    if (e.blind_from > a.gain) {
        const int from = std::min(e.blind_from, ceiling);
        e.blind_from = -1;
        const float rms = 10.0f * std::log10(static_cast<float>(static_cast<double>(a.sumsq) / a.values) /
                                             (2.0f * 127.5f * 127.5f) + 1e-30f);
        if (e.hold_visits == AG_HOLD_VISITS && rms >= AG_BLIND_MIN_RMS && peak_dbfs - rms <= AG_BLIND_MAX_CREST) {
            const float rg = real_gain_db(t, a.gain);
            int g = a.gain;
            for (int c = next_up(L, a.gain); c > g && c <= from; c = next_up(L, c)) {
                if (rms + real_gain_db(t, c) - rg > AG_LEVEL_TARGET_DBFS) break;
                g = c;
            }
            if (g > a.gain) {
                ag_cur_changes++;
                e.gain = g;
                e.probes = 0;
                e.probe_from = -1;
                e.clean = 0;
                e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
                e.last_raise = true;   // (a clip at the new gain is a clip after a raise: the hold doubles)
                // (not a raise of the clipping rule's for the linearity rule's
                // one way up: the level is measured - no up search from the old gain)
                if (auto fe = floor_map.find(a.hz); fe != floor_map.end()) fe->second.last_clip_g = quantize_down(L, std::min(g, ceiling));
                return;
            }
        }
    }
    if (e.probes > 0) {
        // settling after a step down: up at once by the room the peak leaves
        e.probes--;
        const int room = static_cast<int>(std::floor((AG_PEAK_TARGET_DBFS - peak_dbfs) * 10.0f));
        const int target = std::min(ceiling, quantize_down(L, a.gain + room));
        if (target - a.gain >= AG_PROBE_MIN_ROOM) {
            ag_cur_changes++;
            e.probe_from = a.gain;
            e.gain = target;   // (kept at the ceiling too: a clip there probes back, it doesn't start over)
            e.clean = 0;
            e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
            return;
        }
        e.probes = 0;   // no room: settled
    }
    e.probe_from = -1;   // clean at this gain: it holds
    if (e.clean++ == 0) e.clean_since_ns = now;
    e.peak_max_dbfs = std::max(e.peak_max_dbfs, peak_dbfs);
    e.rms_max_dbfs = std::max(e.rms_max_dbfs, 10.0f * std::log10(static_cast<float>(static_cast<double>(a.sumsq) / a.values) /
                                                                 (2.0f * 127.5f * 127.5f) + 1e-30f));
    if (e.clean < e.hold_visits || now - e.clean_since_ns < e.hold_ns) return;
    if (std::min(e.gain, ceiling) >= ceiling) { gain_map.erase(it); return; }   // at the ceiling and settled: forgotten
    // The raise's SIZE from the level (when it comes stays the hold's): the
    // highest list gain whose predicted rms - the hold's highest, by the real
    // gain curve - stays at AG_LEVEL_TARGET_DBFS and peak under the headroom,
    // at most the ceiling, at least one list step. A hop whose transmitter
    // has left returns to the ceiling in one raise (413.19 MHz on the
    // hardware: -37.7 dBFS at 22.9 for 7 min, +3 dB a hold); a steady carrier
    // goes straight to its level gain; one still near full scale gets no raise.
    const float rg = real_gain_db(t, a.gain);
    int up = a.gain;
    for (int c = next_up(L, a.gain); c > up && c <= ceiling; c = next_up(L, c)) {
        const float d = real_gain_db(t, c) - rg;
        if (e.rms_max_dbfs + d > AG_LEVEL_TARGET_DBFS || e.peak_max_dbfs + d > AG_RAISE_HEADROOM_DBFS) break;
        up = c;
    }
    if (up <= a.gain) {
        // not this hold: a new one, with its own peaks (the highest peak
        // never decayed - one loud clean visit, a carrier keyed while the
        // hop was clean, blocked every raise for good)
        e.clean = 0;
        e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
        return;
    }
    ag_cur_changes++;
    // (back at the ceiling the entry is kept: it carries the hold - erased, a
    // keyed carrier found a fresh hold each time; it is forgotten after a
    // clean hold up there)
    e.gain = std::min(up, ceiling);
    e.last_raise = true;
    e.clean = 0;
    e.peak_max_dbfs = -200; e.rms_max_dbfs = -200;
}

// At a sweep's end: the status of that sweep - the gains its hops were
// really taken with (as the 8091 headers said), not what the map wants
void ag_publish() {
    const int ceiling = current_gain.load();
    ag_probes.store(ag_cur_probes);
    ag_floor_decisions.store(ag_cur_floor_decisions);
    ag_floor_hops.store(ag_cur_floor_hops);
    ag_search_hops.store(ag_cur_search);
    ag_pairs.store(ag_cur_pairs);
    ag_nonlinear.store(ag_cur_nonlinear);
    ag_learned.store(ag_cur_learned);
    ag_untestable.store(ag_cur_untestable);
    ag_guesses.store(ag_cur_guesses);
    ag_late.store(ag_cur_late);
    ag_cur_late = 0;
    ag_cur_nonlinear = ag_cur_learned = ag_cur_untestable = ag_cur_guesses = 0;
    ag_bad_pairs.store(ag_cur_bad_pairs);
    if (!ag_cur_scatter.empty()) {
        auto m = ag_cur_scatter.begin() + static_cast<long>(ag_cur_scatter.size() / 2);
        std::nth_element(ag_cur_scatter.begin(), m, ag_cur_scatter.end());
        ag_floor_scatter.store(*m);
    }
    ag_cur_scatter.clear();
    ag_cur_probes = ag_cur_floor_decisions = ag_cur_floor_hops = ag_cur_search = ag_cur_pairs = ag_cur_bad_pairs = 0;
    sweep_idx++;
    // the next sweep: a whole-plan probe sweep while any hop still searches
    ag_probe_sweep.store(auto_gain_on.load() && ag_search_hops.load() > 0 && (sweep_idx & 1u) == 1u);
    ag_reduced_hops.store(ag_cur_reduced);
    ag_min_gain.store(ag_cur_reduced ? std::min(ag_cur_min, ceiling) : ceiling);
    ag_cur_reduced = 0;
    ag_cur_min = 1 << 30;
    ag_clipped_hops.store(ag_cur_clipped);
    ag_changes.store(ag_cur_changes);
    ag_writes.store(ag_cur_writes);
    ag_cur_clipped = ag_cur_changes = ag_cur_writes = 0;
}

// One gain write (under device_io_mutex, on the tuner's own thread): once
// more on failure. The tuner is in manual gain mode (the common gain is).
// The fork's fast gain write where it has one (worker F, 2026-11-23,
// RTLSDR_HAS_GAIN_FAST: a dummy read + one I2C write of LNA and mixer,
// against rtlsdr_set_tuner_gain's ~8 transfers - 9.3 ms on the KrakenSDR;
// the same gain table, rounding and register cache), else today's call.
bool write_gain(rtlsdr_dev_t* dev, int g) {
#if defined(RTLSDR_HAS_GAIN_FAST)
    if (rtlsdr_set_tuner_gain_fast(dev, g, 0) >= 0) return true;
#endif
    return rtlsdr_set_tuner_gain(dev, g) >= 0 || rtlsdr_set_tuner_gain(dev, g) >= 0;
}

#if defined(RTLSDR_HAS_GAIN_FAST) && defined(RTLSDR_TUNE_NO_DUMMY_READ)
// A hop that changes both: the fast retune and the gain in the same hold of
// the I2C repeater (one USB transfer more than the retune alone). A failure
// doesn't say which part failed: the frequency on the full path, then the
// gain once more (rewriting an unchanged gain is a no-op in the driver's cache).
bool sweep_tune_gain(rtlsdr_dev_t* dev, uint32_t hz, int g, bool& fast, char& gain_ok) {
    fast = false;
    if (rtlsdr_set_center_freq_gain_fast(dev, hz, g, RTLSDR_TUNE_KEEP_AUTOTUNE | RTLSDR_TUNE_NO_DUMMY_READ) >= 0) {
        fast = true;
        gain_ok = 1;
        return true;
    }
    const bool ok = rtlsdr_set_center_freq(dev, hz) >= 0 || rtlsdr_set_center_freq(dev, hz) >= 0;
    fast = !ok;   // (both failed: the 128 kHz autotune clock may be pending - as sweep_tune)
    gain_ok = write_gain(dev, g);
    return ok;
}
#endif

}  // namespace

void set_sweep_auto_gain(bool enable) {
    std::lock_guard<std::mutex> lock(settings_mutex);
    const bool was = auto_gain_on.exchange(enable);
    if (was && !enable && in_sweep()) sweep_restore_common_gain();
    sweep_notify();
}

bool sweep_auto_gain() { return auto_gain_on.load(); }

uint32_t sweep_trial_mask() { return auto_gain_on.load() && in_sweep() ? trial_mask.load() : 0u; }

bool sweep_auto_gain_active() { return auto_gain_on.load() && in_sweep() && current_gain.load() >= 0; }

// A packet's noise floor (the conversion thread): a Welch estimate over
// FLOOR_FRAMES Hann frames of FLOOR_N points (pffft) and the FLOOR_PCT
// percentile of the bins in the kept band (|f| < the plan's step / 2, DC +-2
// bins out), dB per bin against full scale. A low percentile: carriers -
// keyed or not - sit above it. ~8 FFTs of 1024 a visit.
constexpr int FLOOR_N = 1024, FLOOR_FRAMES = 8;
constexpr float FLOOR_PCT = 0.20f;
struct PacketCarrier {
    float pk_off_hz = 0, pk_over_db = -99;      // the band's strongest bin
    float win_off_hz = 0, win_over_db = -99, win_pow_db = -200;   // the window's
};
static float packet_floor_db(const uint8_t* b, size_t nb, PacketCarrier& pc, float win_lo_hz) {
    static PFFFT_Setup* setup = pffft_new_setup(FLOOR_N, PFFFT_COMPLEX);
    static float* in = static_cast<float*>(pffft_aligned_malloc(2 * FLOOR_N * sizeof(float)));
    static float* out = static_cast<float*>(pffft_aligned_malloc(2 * FLOOR_N * sizeof(float)));
    static float* work = static_cast<float*>(pffft_aligned_malloc(2 * FLOOR_N * sizeof(float)));
    static float win[FLOOR_N];
    static float wsum = [] {
        float sum = 0;
        for (int i = 0; i < FLOOR_N; i++) {
            win[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * (i + 0.5f) / FLOOR_N);
            sum += win[i] * win[i];
        }
        return sum;
    }();
    static float acc[FLOOR_N];
    static std::vector<float> kept;
    if (!setup || !in || !out || !work) return NAN;
    const int frames = std::min<int>(FLOOR_FRAMES, static_cast<int>(nb / (2 * FLOOR_N)));
    if (frames < 1) return NAN;
    std::fill(acc, acc + FLOOR_N, 0.0f);
    for (int f = 0; f < frames; f++) {
        const uint8_t* p = b + static_cast<size_t>(f) * 2 * FLOOR_N;
        for (int i = 0; i < FLOOR_N; i++) {
            in[2 * i] = (static_cast<float>(p[2 * i]) - 127.5f) * win[i];
            in[2 * i + 1] = (static_cast<float>(p[2 * i + 1]) - 127.5f) * win[i];
        }
        pffft_transform_ordered(setup, in, out, work, PFFFT_FORWARD);
        for (int i = 0; i < FLOOR_N; i++) acc[i] += out[2 * i] * out[2 * i] + out[2 * i + 1] * out[2 * i + 1];
    }
    const int half = std::min<int>(FLOOR_N / 2 - 1, static_cast<int>(static_cast<double>(stats_half_band_hz.load()) *
                                                                     FLOOR_N / SAMPLE_RATE));
    kept.clear();
    for (int i = 3; i <= half; i++) { kept.push_back(acc[i]); kept.push_back(acc[FLOOR_N - i]); }
    if (kept.size() < 16) return NAN;
    auto k = kept.begin() + static_cast<long>(kept.size() * FLOOR_PCT);
    std::nth_element(kept.begin(), k, kept.end());
    const float norm = static_cast<float>(frames) * wsum * 127.5f * 127.5f;
    const float fl = *k;
    if (fl > 0) {
        const float bin_hz = static_cast<float>(SAMPLE_RATE) / FLOOR_N;
        int best = 0, wbest = 0;
        float bv = 0, wv = 0;
        auto at = [&](int off) { return acc[(off + FLOOR_N) % FLOOR_N]; };
        for (int off = -half; off <= half; off++) {
            if (off > -3 && off < 3) continue;
            const float v = at(off);
            if (v > bv) { bv = v; best = off; }
            if (static_cast<float>(off) * bin_hz >= win_lo_hz && v > wv) { wv = v; wbest = off; }
        }
        pc.pk_off_hz = static_cast<float>(best) * bin_hz;
        pc.pk_over_db = 10.0f * std::log10(bv / fl + 1e-30f);
        if (wv > 0) {
            pc.win_off_hz = static_cast<float>(wbest) * bin_hz;
            pc.win_over_db = 10.0f * std::log10(wv / fl + 1e-30f);
            // a tone's power: the main lobe's three bins over (norm x N) - the
            // Hann window's energy spreads over them (Parseval)
            const float s3 = at(wbest - 1) + wv + at(wbest + 1);
            pc.win_pow_db = 10.0f * std::log10(s3 / (norm * FLOOR_N) + 1e-30f);
        }
    }
    return 10.0f * std::log10(fl / norm + 1e-30f);
}

// The estimator thread: one visit's spectrum at a time, normal priority
static void est_thread_main() {
    for (;;) {
        EstJob j;
        {
            std::unique_lock<std::mutex> lk(est_mutex);
            est_cv.wait(lk, [] { return !est_queue.empty(); });
            j = std::move(est_queue.front());
            est_queue.pop_front();
        }
        PacketCarrier pc;
        const float fl = packet_floor_db(j.bytes.data(), j.bytes.size(), pc, win_lo_hz.load(std::memory_order_relaxed));
        std::lock_guard<std::mutex> lk(est_mutex);
        EstResult& r = est_result[j.t][j.seq % 4];
        if (r.seq == j.seq) {
            r.floor = fl;
            r.pk_off_hz = pc.pk_off_hz;
            r.pk_over_db = pc.pk_over_db;
            r.win_off_hz = pc.win_off_hz;
            r.win_over_db = pc.win_over_db;
            r.win_pow_db = pc.win_pow_db;
            r.done = true;
        }
        if (est_pool.size() < 16) est_pool.push_back(std::move(j.bytes));
    }
}
void est_start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(est_thread_main).detach(); });
}

void sweep_note_hop_stats(const std::vector<SampleBuffer>& raw_set, const uint32_t* hz, const int* gain, int n) {
    est_start();
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
        bool queue_it = false;
        uint32_t seq = 0;
        {
            std::lock_guard<std::mutex> lk(acc_mutex);
            HopAcc& a = hop_acc[t];
            if (a.hz != hz[t] || a.gain != gain[t]) {
                a = HopAcc{hz[t], gain[t]};
                a.seq = ++visit_seq[t];
            }
            if (!a.est_queued) { a.est_queued = true; queue_it = true; seq = a.seq; }
            a.values += nb;
            a.clipped += clipped;
            a.sumsq += ss;
            a.peak = std::max(a.peak, peak);
            a.packets++;
        }
        if (queue_it) {
            // the visit's first packet goes to the estimator (what it reads of it)
            const size_t take = std::min<size_t>(nb, 2 * FLOOR_N * FLOOR_FRAMES);
            std::lock_guard<std::mutex> lk(est_mutex);
            EstJob j;
            j.t = t;
            j.seq = seq;
            if (!est_pool.empty()) { j.bytes = std::move(est_pool.back()); est_pool.pop_back(); }
            j.bytes.assign(b, b + take);
            {
                EstResult& r = est_result[t][seq % 4];
                r = EstResult{};
                r.seq = seq;
            }
            if (est_queue.size() < 64) est_queue.push_back(std::move(j));
            est_cv.notify_one();
        }
    }
}

void sweep_restore_common_gain() {
    const int g = current_gain.load();
    trial_mask.store(0);
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
      // the gain write in use: the fork's fast call (+1 USB transfer with the
      // retune) or the ordinary one (~8 transfers, 9 ms on the KrakenSDR)
      // the linearity search (round 10; of the last completed sweep; zeros with
      // Auto off - always sent in the sweep object: a client tells a heimdall
      // with the measured rule from one without by gain_floor_hops)
      << ",\"gain_floor_hops\":" << (auto_gain_on.load() ? ag_floor_hops.load() : 0u) << ",\"gain_probes\":" << (auto_gain_on.load() ? ag_probes.load() : 0u)
      << ",\"gain_floor_decisions\":" << (auto_gain_on.load() ? ag_floor_decisions.load() : 0u) << ",\"gain_search_hops\":" << (auto_gain_on.load() ? ag_search_hops.load() : 0u)
      << ",\"floor_scatter_db\":" << (auto_gain_on.load() ? ag_floor_scatter.load() : 0.0f)
      << ",\"gain_pairs\":" << (auto_gain_on.load() ? ag_pairs.load() : 0u) << ",\"gain_pairs_unbracketed\":" << (auto_gain_on.load() ? ag_bad_pairs.load() : 0u)
      << ",\"gain_pairs_nonlinear\":" << (auto_gain_on.load() ? ag_nonlinear.load() : 0u) << ",\"gain_step_learned\":" << (auto_gain_on.load() ? ag_learned.load() : 0u)
      << ",\"gain_probes_skipped\":" << (auto_gain_on.load() ? ag_untestable.load() : 0u)
      << ",\"gain_level_guesses\":" << (auto_gain_on.load() ? ag_guesses.load() : 0u)
      << ",\"gain_est_late\":" << (auto_gain_on.load() ? ag_late.load() : 0u)
      << ",\"gain_probe_sweep\":" << (sweep_auto_gain_active() && ag_probe_sweep.load() ? "true" : "false")
#if defined(RTLSDR_HAS_GAIN_FAST)
      << ",\"gain_fast\":true"
#else
      << ",\"gain_fast\":false"
#endif
      << [] {
             const std::string c = real_gain_json();
             return c.empty() ? std::string() : ",\"gain_real_db\":" + c;
         }()
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
    std::vector<char> move_trial;            // that tuner's gain on the new hop is a probe's (the 8091 trial flag)
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
            step_map.clear();   // (the learned steps: another plan or other tuners)
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
        move_trial.clear();
        uint32_t hopping = 0;   // tuners that move from round to round (a block of 2+ hops)
        uint32_t plan_mask = 0;
        for (int t : sel) plan_mask |= 1u << t;
        stats_mask.store(auto_gain_on.load() ? plan_mask : 0, std::memory_order_relaxed);
        stats_half_band_hz.store(static_cast<uint32_t>(p.step_hz / 2), std::memory_order_relaxed);
        plan_step_hz = static_cast<uint32_t>(p.step_hz);
        win_lo_hz.store(static_cast<float>(p.step_hz) - AG_NB_REACH_HZ, std::memory_order_relaxed);
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
                // (without the fast call a small step up may wait one hop:
                // that hop is taken a little lower - and its header says so)
                bool gain = g >= 0 && g != now_g;
                if (gain && auto_gain_on.load() && now_g >= 0 && g > now_g && g - now_g < AG_UP_MIN &&
                    up_skips[sel[k]] < AG_UP_SKIP_MAX) {
                    gain = false;
                    up_skips[sel[k]]++;
                } else {
                    up_skips[sel[k]] = 0;
                }
                // the trial gain flag: the gain this visit is taken with is a probe's
                const bool trial = auto_gain_on.load() && floor_is_trial(hz, gain ? g : now_g);
                if (tune || gain) {
                    move_tuner.push_back(sel[k]);
                    move_hz.push_back(hz);
                    move_tune.push_back(tune);
                    move_gain.push_back(gain ? g : -999);
                    move_trial.push_back(trial);
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
                // The gains again, now under the lock: a set_gain (or auto
                // off) that ran while this hop waited for it has written
                // every tuner already - a write decided before it would be
                // a stale, redundant one
                for (size_t i = 0; i < move_tuner.size(); i++) {
                    if (move_gain[i] == -999) continue;
                    const int g = wanted_gain(move_tuner[i], move_hz[i]);
                    const int now_g = wideband_config.get_tuner_gain(move_tuner[i]);
                    move_gain[i] = (g >= 0 && g != now_g) ? g : -999;
                }
                for_each_tuner(move_tuner.size(), [&](size_t i) {
                    const int t = move_tuner[i];
                    if (t < 0 || t >= static_cast<int>(devices.size()) || !devices[t] || !devices[t]->dev) return;
                    const int64_t a = now_ns();
                    bool fast = false, both = false;
#if defined(RTLSDR_HAS_GAIN_FAST) && defined(RTLSDR_TUNE_NO_DUMMY_READ)
                    if (move_tune[i] && move_gain[i] != -999) {   // one call for both
                        move_ok[i] = sweep_tune_gain(devices[t]->dev, move_hz[i], move_gain[i], fast, move_gain_ok[i]);
                        both = true;
                    }
#endif
                    if (!both) {
                        move_ok[i] = move_tune[i] ? sweep_tune(devices[t]->dev, move_hz[i], fast) : 1;
                        // the hop's gain after its frequency (a tuner whose
                        // retune failed gets it on its next try)
                        if (move_ok[i] && move_gain[i] != -999) {
                            const int64_t g0 = now_ns();
                            move_gain_ok[i] = write_gain(devices[t]->dev, move_gain[i]);
                            move_gain_ns[i] = now_ns() - g0;
                        }
                    }
                    move_fast[i] = fast;
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
                    // (with the gain: the header reads both before the flag)
                    if (move_trial[i] && move_ok[i]) trial_mask.fetch_or(1u << t);
                    else trial_mask.fetch_and(~(1u << t));
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
                if (a.hz == hz && a.packets > 0) {
                    // (a linearity probe's visit - the trial flag - is not the
                    // clipping rule's: a clipped up-probe is refused there; its
                    // share must not take the hop's own gain down)
                    if (!(trial_mask.load() & (1u << t))) ag_learn(t, a, t_end);
                    // the linearity rule learns from it when its spectrum is
                    // there (floor_pending_run, below and at the next rounds)
                    floor_pending.push_back({t, a, quantize_down(gains_of(t), learned_gain(hz, current_gain.load())), sweep_idx,
                                             0});
                }
                // the status: the gain this hop was really taken with (its
                // own: a probe's visit - the trial flag - counts in gain_probes)
                const int used = wideband_config.get_tuner_gain(t);
                if (used >= 0 && used < current_gain.load() && !(trial_mask.load() & (1u << t))) {
                    ag_cur_reduced++;
                    ag_cur_min = std::min(ag_cur_min, used);
                }
            }
        }
        floor_pending_run();
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
            ag_publish();
            std::lock_guard<std::mutex> plock(sweep_state.mutex);
            if (sweep_state.plan_generation.load() == pg) sweep_state.sweeps.fetch_add(1);
        }
    }
    std::cout << "Sweep thread exiting" << std::endl;
}
