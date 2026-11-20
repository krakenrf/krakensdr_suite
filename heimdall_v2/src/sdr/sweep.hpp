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
// "" if the three plan fields are valid (hop centres inside the tuner range,
// step SWEEP_STEP_MIN_HZ .. SWEEP_STEP_MAX_HZ, 1 .. SWEEP_STEPS_MAX hops)
std::string validate_sweep_plan(uint64_t first_center_hz, uint64_t step_hz, uint64_t steps);

// The contract's status object, {"first_center_hz":..,"round_ms":..}
std::string sweep_status_json();
// The tuners taking part (ascending), from the plan's mask and the element
// count: every tuner when none of the mask's is there
std::vector<int> sweep_tuners(uint32_t tuner_mask);
// Rounds R of a plan: ceil(H / min(K, H)) for H hops and K tuners
uint32_t sweep_rounds(uint32_t steps, size_t tuners);

// set_operating_mode() entering SWEEP (settings_mutex held), BEFORE the mode
// is stored: round 0, running, the 8091 retuning flag on until the first
// hop has settled (the tuners are wherever the previous mode left them)
void sweep_prepare_entry();
// Wake the sweep thread's waits (mode entered / left, plan changed)
void sweep_notify();
// A gain change in sweep mode (update_sdr_settings, settings_mutex held),
// before the tuners are written: the stream is invalid until the next settle
// + flush
void sweep_note_tuner_change();

// The sweep thread (started once from main): hops while the mode is SWEEP,
// the plan runs and the pipeline is up; idle otherwise.
void sweep_thread();
