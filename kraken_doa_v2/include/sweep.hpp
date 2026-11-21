#pragma once

// Sweep mode (docs/dev/sweep-mode.md): heimdall hops the chosen tuners through
// a frequency range and this engine stitches the hops into ONE wide spectrum -
// the sweep array, a fixed grid of FFT bins over [start, stop] - that every
// page gets cut to its own window (binary type 10). Nothing is demodulated.
//
// Threads: the settings and the frames belong to the uWS loop (every command
// runs there); the data receiver hands each 8091 packet's bytes of the chosen
// tuners to the engine's worker thread (a copy into a fixed ring of slots, no
// allocation per packet); the worker FFTs them, averages each hop over its
// dwell and writes the hop's centre bins into the array when the hop ends.
// The array and its max-pyramid (the frames' pooling) are shared under one
// short mutex.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sweep {

// --- settings (uWS loop thread) -----------------------------------------------------
// Each returns the setting in its command form as applied (SWEEP_RANGE:... -
// what is saved). Throws std::invalid_argument for a value that is no number.
std::string set_range(double start_mhz, double stop_mhz);
std::string set_tuners(const std::string& list);   // "0,1,4" | "all"; unknown indices dropped
std::string set_res(const std::string& v);         // "4096" | "auto" | "0"
std::string set_dwell(double ms);
void set_running(bool on);   // SWEEP_RUN (not saved)
bool running();

// The client entered / left sweep mode (ControlHandler::apply_operating_mode_state,
// after the mode flags changed): the engine consumes packets only while active
void on_mode(bool active);
// The tuner count changed (the 8091 header): the plan follows. True = the
// plan heimdall gets changed
bool on_elements();
// The plan heimdall gets changed since the last call (any setter or
// on_elements): the caller sends plan_command() if in sweep mode
bool take_plan_changed();

// The common gain (dB, -1 = AGC) - heimdall's header in sweep mode, or a
// GAIN: command; the sweep message carries it (no type 7 header does in
// sweep mode). True = it changed: send the sweep message again.
// from_header: the 8091 header's, which trails a GAIN: command by a few
// packets - a different value within 1.5 s of a command is not taken
bool note_gain(float db, bool from_header);

// heimdall's {"command":"set_sweep",...} for the current plan (run included)
std::string plan_command();
// {"sweep":{...}} - the state every page gets on connect and on every change
std::string state_json();
// for the dashboard / the AI assistant: "87.500-108.000 MHz, N 16384, ..."
std::string summary();
// sweep bins of the current grid, and whether any is measured
int64_t bins();

// --- the data receiver thread -------------------------------------------------------
// Every 8091 packet while the client is in sweep mode: channel-major uint8 IQ,
// the header's per-channel frequency, frequency_change_counter (+1 per round),
// current_group_index (the round) and retuning_in_progress
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const float* freq_hz,
               uint32_t counter, uint32_t round, bool retuning);

// --- frames (uWS loop) -----------------------------------------------------------------
struct View {
    uint32_t seq = 0;
    double lo_hz = 0, hi_hz = 0;   // absolute; lo >= hi = the whole range
    uint32_t points = 0;           // the plot's width (0 = SPECTRUM_LEGACY_POINTS)
};
// What changed since a socket's last frame (websocket_server.cpp)
uint64_t data_version();     // + 1 per hop written (and per level rewrite)
uint32_t completions();      // sweeps completed since the engine started (any grid)
// Binary type 10 (the contract's table) for one view; completes = flag 2. With
// overview_points (SWEEP_OVERVIEW:<points>, 16 ... SPECTRUM_VIEW_MAX_POINTS), a
// completed sweep and a view narrower than the whole range, *overview gets the
// overview frame (flag 16 | 2: the whole range as a whole-range view of that many
// points gets it, same seq / sweeps) to send BEFORE the frame; else it is cleared
std::string build_frame(const View& v, bool completes, uint32_t overview_points = 0, std::string* overview = nullptr);

// SWEEP_FULL (2026-11-21): the whole store at the sweep's own bins as u8 levels
// (type 10's scale), for type 11. Made once, shared by every socket sending it
struct FullSet {
    uint32_t grid = 0, sweeps = 0;
    double f0_hz = 0, step_hz = 0;   // the centre of value 0, the spacing
    uint16_t pool = 1;               // bins per value
    std::vector<uint8_t> lv;
    double lock_ms = 0;              // how long the store's lock was held for it
};
// nullptr = no data on this grid yet
std::shared_ptr<const FullSet> build_full();
constexpr size_t FULL_PART_VALUES = 524288;   // a type 11 part's values at most
// one type 11 part: flags 1 = last part, 2 = snapshot, 4 = paused
std::string full_part(const FullSet& s, uint8_t flags, uint32_t set_id, size_t first, size_t n);
uint32_t grid_id();   // the current grid's id (0 = none)

// The levels of a window max-pooled to at most `points` points (dB over the
// reference; NaN = never measured) - the AI assistant's get_spectrum
bool pooled_db(double lo_hz, double hi_hz, int points, std::vector<float>& db, double* first_hz,
               double* step_hz, double* bin_hz);

// The engine's worker thread (main.cpp)
void start();
void stop();

}  // namespace sweep
