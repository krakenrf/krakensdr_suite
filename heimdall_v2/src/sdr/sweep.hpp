#pragma once

// Sweep mode (OperatingMode::SWEEP): the chosen tuners hop through the plan
// (sweep_state, core/types.hpp) so a client stitches one wide spectrum out
// of the hops. The interface is fixed in
// kraken_doa_v2/docs/dev/sweep-mode.md ("The contract"); heimdall's design,
// decisions and measurements: docs/dev/operating-modes.md, "Sweep mode".

#include "../core/types.hpp"
#include <cstdint>
#include <string>
#include <vector>

// A set_sweep request: each part optional (has_*), the three plan fields only
// together.
struct SweepRequest {
    bool has_plan = false;
    uint64_t first_center_hz = 0;
    uint64_t step_hz = 0;
    uint32_t steps = 0;
    bool has_tuners = false;
    std::vector<int> tuners;
    bool has_dwell = false;
    uint32_t dwell_ms = 0;
    bool has_settle = false;
    uint32_t settle_ms = 0;
    bool has_run = false;
    bool run = true;
};

// Checks every field of `req`, then applies all of it at once ("" = done)
// or nothing (the reason). A plan or tuner change restarts the sweep at
// round 0; every change is saved (settings::note_mode's debounce). Accepted
// in every mode, used in sweep mode.
std::string apply_sweep_request(const SweepRequest& req);
// (validate_sweep_plan: core/types.hpp, shared with the settings file)

// The contract's status object, {"first_center_hz":..,"round_ms":..}
std::string sweep_status_json();
// The tuners taking part (ascending), from the plan's mask and the element
// count: every tuner when none of the mask's is there
std::vector<int> sweep_tuners(uint32_t tuner_mask);
// Rounds R of a plan: ceil(H / min(K, H)) for H hops and K tuners
uint32_t sweep_rounds(uint32_t steps, size_t tuners);
// The valid USB packets (sets) a hop delivers per tuner at a dwell: the
// fewest whole packets that hold dwell_ms of samples, ceil(dwell_ms x
// SAMPLE_RATE / (1000 x NUM_SAMPLES)), at least 1 (5 ms -> 1, 10 -> 2,
// 20 -> 3, 50 -> 8, 100 -> 15 at 2.4 MS/s, 16384 samples)
constexpr uint32_t sweep_hop_packets(uint32_t dwell_ms) {
    const uint64_t num = static_cast<uint64_t>(dwell_ms) * static_cast<uint64_t>(SAMPLE_RATE);
    const uint64_t den = 1000ULL * static_cast<uint64_t>(NUM_SAMPLES);
    const uint64_t n = (num + den - 1) / den;
    return n < 1 ? 1u : static_cast<uint32_t>(n);
}

// set_operating_mode() entering SWEEP (settings_mutex held), BEFORE the mode
// is stored: round 0, running, the 8091 retuning flag on until the first
// hop has settled (the tuners are wherever the previous mode left them)
void sweep_prepare_entry();
// Wake the sweep thread's waits (mode entered / left, plan changed)
void sweep_notify();
// A gain change in sweep mode (update_sdr_settings, settings_mutex held):
// _change() before the tuners are written (the flag on), _change_done()
// after (the settle + flush that make the stream valid again run from there)
void sweep_note_tuner_change();
void sweep_note_tuner_change_done();
// The conversion worker sent a set as valid (retuning flag 0 before and after
// its header): counted in sweep_state.valid_sets (the dwell's measure)
void sweep_note_valid_set();
// After the mode left SWEEP (settings_mutex held): wait until no sweep L1
// flush runs any more - none starts after this (set_operating_mode's
// re-pairing flush on the way back to coherent is then the last one)
void sweep_quiesce_flushes();

// ---- Auto gain (8092 set_sweep_auto_gain; docs/dev/operating-modes.md,
// "Auto gain"): a gain per hop frequency, learned from the hops' valid
// samples; the common gain is the ceiling. Not saved. ----
// Switch it (settings_mutex taken here). Off in sweep mode: every tuner back
// at the common gain at once (under the 8091 flag).
void set_sweep_auto_gain(bool enable);
bool sweep_auto_gain();          // the switch
// The switch on, sweep mode, and a manual common gain (with the tuner's own
// AGC, -1, there is no ceiling: auto does nothing)
bool sweep_auto_gain_active();
// The conversion worker, for a set it sent as valid in sweep mode: each
// tuner's frequency and gain as the header read them (before the flag)
void sweep_note_hop_stats(const std::vector<SampleBuffer>& raw_set, const uint32_t* hz, const int* gain, int n);
// Leaving sweep mode (set_operating_mode, settings_mutex held, the mode still
// SWEEP): every tuner whose gain differs back at the common gain, under the flag
void sweep_restore_common_gain();

// The sweep thread (started once from main): hops while the mode is SWEEP,
// the plan runs and the pipeline is up; idle otherwise.
void sweep_thread();
