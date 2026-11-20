// Sweep mode: the hop sequence, the plan and its status (sweep.hpp).
//
// One hop (every tuner of the round together):
//   retuning_in_progress = 1 -> downstream flush -> retune (parallel,
//   for_each_tuner, full path) -> settle -> L1 + downstream flush ->
//   retuning_in_progress = 0 -> dwell
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
//    (sdr_pipeline.cpp). The post-settle flush takes that mutex too, and the
//    flag is cleared only after it: a header that reads the flag as 0 after
//    hop h's clear was built after hop h's post-settle flush had completed,
//    so its set was dequeued after that flush - its packets reached L1 after
//    the drain had emptied it at the end of the settle. The frequencies it
//    read are hop h's (h + 1's would make it read the flag as 1: they are
//    stored after h + 1 set the flag).
//  - A packet arriving in L1 just after the flush started sampling up to one
//    USB packet (NUM_SAMPLES / SAMPLE_RATE, 6.8 ms) earlier, so the settle
//    the samples really see is SWEEP_SETTLE_MS minus up to a packet: that is
//    what the settle measurement (docs) tests, through the real pipeline.
//  - A gain change in sweep mode (sweep_note_tuner_change) and the mode's
//    entry set the flag the same way; whenever hopping stops (paused, plan
//    not runnable) the thread finishes the settle and the flush before it
//    clears the flag, so a stuck 1 can't make a client drop everything.
//
// No output per hop (a sweep makes ~20 hops a second): failed retunes are
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
#include <condition_variable>
#include <iostream>
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

// The sweep's L1 + downstream flush; false (nothing done) once the mode was left
bool sweep_flush() {
    std::lock_guard<std::mutex> lk(flush_mutex);
    if (!in_sweep()) return false;
    clear_l1_buffer_quiet();
    return true;
}

// Sleep until change_seq moves away from `seq`, at most `ns` (and at most
// 50 ms: global_running is cleared by a signal handler, which can't notify)
void wait_for_change(uint32_t seq, int64_t ns) {
    ns = std::clamp<int64_t>(ns, 0, 50'000'000);
    std::unique_lock<std::mutex> lk(wake_mutex);
    wake_cv.wait_for(lk, std::chrono::nanoseconds(ns),
                     [seq]() { return sweep_state.change_seq.load() != seq; });
}

// Wait until the settle (from the last retune - a gain change moves it) or
// the dwell (from `start_ns`) of the plan in force has passed. false as soon
// as this plan stops hopping: the mode left, paused, the plan changed, exit.
// A dwell / settle change takes effect at once (re-read on every wake).
// The dwell also lasts until at least one set has gone out as valid since
// `valid_from` (sweep_state.valid_sets) - at most SWEEP_DWELL_GRACE_MS more:
// the first valid set reaches 8091 up to a USB packet (6.8 ms) after the
// flush, so a short dwell would otherwise often end with none.
bool wait_phase(bool settle, int64_t start_ns, uint32_t plan_gen, uint32_t valid_from = 0) {
    for (;;) {
        const uint32_t seq = sweep_state.change_seq.load();
        if (!global_running.load() || !in_sweep() || sweep_state.plan_generation.load() != plan_gen) return false;
        const SweepPlan p = sweep_state.get_plan();
        if (!p.run) return false;
        const int64_t start = settle ? sweep_state.last_retune_ns.load() : start_ns;
        const int64_t end = start + static_cast<int64_t>(settle ? p.settle_ms : p.dwell_ms) * 1'000'000;
        const int64_t now = now_ns();
        int64_t left = end - now;
        if (left <= 0) {
            if (settle || sweep_state.valid_sets.load() != valid_from ||
                now - end >= static_cast<int64_t>(SWEEP_DWELL_GRACE_MS) * 1'000'000)
                return true;
            left = 1'000'000;   // poll for the first valid set
        }
        wait_for_change(seq, left);
    }
}

// Hopping stopped (paused, the pipeline down) with the flag on: the tuners
// may have moved less than a settle ago - finish that settle and the flush
// before the stream counts as valid again.
void settle_and_clear_flag() {
    if (!sweep_state.retuning_in_progress.load()) return;
    int64_t settled_from = 0;
    for (;;) {
        if (!global_running.load() || !in_sweep()) return;
        settled_from = sweep_state.last_retune_ns.load();
        const int64_t end = settled_from + static_cast<int64_t>(sweep_state.get_plan().settle_ms) * 1'000'000;
        const int64_t left = end - now_ns();
        if (left <= 0) break;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(left, 50'000'000)));
    }
    if (!sweep_flush()) return;
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

std::string tuners_json(const std::vector<int>& t) {
    std::string s = "[";
    for (size_t i = 0; i < t.size(); i++) s += (i ? "," : "") + std::to_string(t[i]);
    return s + "]";
}

}  // namespace

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
      << ",\"l1_unquiet_flushes\":" << l1_unquiet_flushes() << "}";
    return o.str();
}

void sweep_notify() {
    {
        std::lock_guard<std::mutex> lk(wake_mutex);
        sweep_state.change_seq.fetch_add(1);
    }
    wake_cv.notify_all();
}

void sweep_prepare_entry() {
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
}

void sweep_thread() {
    std::cout << "Sweep thread started (inactive until sweep mode)" << std::endl;
    uint32_t plan_gen_seen = ~0u;
    uint32_t r = 0;                          // the round to hop to next
    std::vector<int> move_tuner;             // this round's tuners and frequencies
    std::vector<uint32_t> move_hz;
    std::vector<char> move_ok;

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
            // (outside sweep mode nothing reads the flag; entering sets it)
            if (in_sweep()) settle_and_clear_flag();
            wait_for_change(seq, 50'000'000);
            continue;
        }
        if (pg != plan_gen_seen) {
            plan_gen_seen = pg;
            r = 0;
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
        for (size_t k = 0; k < kp; k++) {
            const uint64_t b0 = static_cast<uint64_t>(k) * H / kp, b1 = static_cast<uint64_t>(k + 1) * H / kp;
            const uint32_t hz = static_cast<uint32_t>(p.first_center_hz + (b0 + r) * p.step_hz);
            // a tuner already there isn't retuned (K' = H: every round is the
            // same - the stream then runs on without a gap); one whose last
            // retune failed is (its frequency entry is 0)
            if (b0 + r < b1 && wideband_config.get_tuner_frequency(sel[k]) != hz) {
                move_tuner.push_back(sel[k]);
                move_hz.push_back(hz);
            }
        }
        move_ok.assign(move_tuner.size(), 0);
        // the flag may be on without a retune: the mode just entered, a gain
        // change - then the settle + flush below run all the same
        const bool retune = !move_tuner.empty();

        const int64_t t0 = now_ns();
        // 1) flag, 2) downstream flush (what is queued is from the old hop
        //    and would be sent flagged anyway)
        if (retune) {
            sweep_state.retuning_in_progress.store(true);
            clear_l2_buffer_quiet();
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
                for_each_tuner(move_tuner.size(), [&](size_t i) {
                    const int t = move_tuner[i];
                    if (t < 0 || t >= static_cast<int>(devices.size()) || !devices[t] || !devices[t]->dev) return;
                    rtlsdr_dev_t* dev = devices[t]->dev;
                    // the full path (lock check, 8 kHz autotune afterwards); one retry
                    move_ok[i] = rtlsdr_set_center_freq(dev, move_hz[i]) >= 0 ||
                                 rtlsdr_set_center_freq(dev, move_hz[i]) >= 0;
                });
                // Only now the new frequencies (the header reads them before
                // the flag). A tuner that failed is NOT on its old frequency
                // (the R820T / E4000 drivers fail after N / SDM are written,
                // librtlsdr then reports 0): its entry is 0, which no client
                // takes for a hop, until a retune succeeds
                uint32_t failed_now = 0;
                for (size_t i = 0; i < move_tuner.size(); i++) {
                    wideband_config.set_tuner_frequency(move_tuner[i], move_ok[i] ? move_hz[i] : 0);
                    if (!move_ok[i]) failed_now |= 1u << move_tuner[i];
                }
                uint32_t moved = 0;
                for (int t : move_tuner) moved |= 1u << t;
                sweep_state.failed_mask.store((sweep_state.failed_mask.load() & ~moved) | failed_now);
                if (failed_now)
                    sweep_state.failed_retunes.fetch_add(static_cast<uint32_t>(std::popcount(failed_now)));
                sweep_state.last_retune_ns.store(now_ns());
                for (int t : move_tuner) note_tuner_retuned(t);   // (also: --kerberos marks the calibration stale)
            }
            if (!cancelled) {
                // round and counter only for the plan in force (a set_sweep
                // resets them under sweep_state.mutex)
                std::lock_guard<std::mutex> plock(sweep_state.mutex);
                if (sweep_state.plan_generation.load() == pg) sweep_state.round.store(r);
                sweep_state.frequency_change_counter.fetch_add(1);
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

        // 4) settle, 5) flush - again if a gain change came in meanwhile
        //    (it moved last_retune_ns) -, 6) flag off, 7) dwell
        //    The flag is cleared under settings_mutex, which every change
        //    that sets it holds (the mode entry, a gain change, the next
        //    hop's retune): none can slip in between the check and the clear
        uint32_t valid_from = sweep_state.valid_sets.load();
        bool settled = !retune && !sweep_state.retuning_in_progress.load();
        for (int pass = 0; pass < 8 && !settled; pass++) {
            if (!wait_phase(true, 0, pg)) break;
            const int64_t settled_from = sweep_state.last_retune_ns.load();
            if (!sweep_flush()) break;
            std::lock_guard<std::mutex> lock(settings_mutex);
            if (in_sweep() && sweep_state.plan_generation.load() == pg &&
                sweep_state.last_retune_ns.load() == settled_from) {
                sweep_state.retuning_in_progress.store(false);
                valid_from = sweep_state.valid_sets.load();
                settled = true;
            }
        }
        if (!settled) continue;
        if (!wait_phase(false, now_ns(), pg, valid_from)) continue;

        sweep_state.round_ms.store(static_cast<uint32_t>((now_ns() - t0) / 1'000'000));
        if (++r >= R) {
            r = 0;
            std::lock_guard<std::mutex> plock(sweep_state.mutex);
            if (sweep_state.plan_generation.load() == pg) sweep_state.sweeps.fetch_add(1);
        }
    }
    std::cout << "Sweep thread exiting" << std::endl;
}
