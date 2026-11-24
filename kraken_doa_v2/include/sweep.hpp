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

#include <atomic>
#include <cstdint>
#include <functional>
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
// SWEEP_GAINS:<f1,...,fn>|<g0,...,gn> (2026-11-24, gain by region): n <= 64 points
// (Hz, strictly rising) cutting the axis into segments, n + 1 gains (dB, 0 - 50; not
// the AGC); "|" = no table (the common gain everywhere). Kept by frequency whatever
// the plan does, one per receiver, saved and replayed
std::string set_gains(const std::string& v);
// {"command":"set_sweep_gains",...} for heimdall: sent after every set_sweep and on
// a change in sweep mode. note_gains_asked() before the control socket is drained
// (an answer to an earlier table then doesn't count as this one's), note_gains_sent()
// after it, right before the send (an older heimdall's "Unknown command" from then on
// says it has no table)
std::string gains_command();
void note_gains_asked();
void note_gains_sent();
// heimdall's answer changed the table (it applied other values): the command to
// save, once ("" = nothing)
std::string take_gains_save();
// a table is in force (or asked of a heimdall that hasn't answered yet): the 8091
// header's channel 0 gain is then a hop's, not the common gain
bool regions_active();
// heimdall's control lines (its replies and its status): its measured gain curve,
// its answer to set_sweep_gains
void ingest_control_reply(const char* data, size_t n);
// the state message has to go again (heimdall's answer changed it): taken by the
// server's tick
bool take_state_dirty();
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
// gain_db: each channel's gain in the 8091 header (the gain its samples were
// taken with; nullptr = unknown)
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const float* freq_hz, const float* gain_db,
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
    // SWEEP_FULL_Z: a deflated copy of each part, made by the compressor thread
    // (z_submit) and freed with the set. Sized before the hand-over; part k's
    // copy is read only once zstate[k] says READY (release / acquire)
    mutable std::vector<std::string> zpart;
    mutable std::unique_ptr<std::atomic<uint8_t>[]> zstate;   // Z_PENDING / Z_READY / Z_RAW
    mutable std::atomic<bool> z_dropped{false};               // the compressor left it (a newer hand-over)
    mutable int64_t z_t0_ms = 0;                               // when it was handed over
    mutable size_t z_before = 0;                               // bytes handed over ahead of it (the deadline counts them)
};
enum : uint8_t { Z_PENDING = 0, Z_READY = 1, Z_RAW = 2 };
// SWEEP_FULL_Z (2026-11-23): hand this tick's sets to the compressor thread
// (zlib, Z_HUFFMAN_ONLY, one stream a part of FULL_PART_VALUES); a newer
// hand-over replaces what it has not done (no queue). wake is called from that
// thread when a part is ready (the server posts its pump to the uWS loop)
void z_submit(const std::vector<std::shared_ptr<const FullSet>>& sets);
void z_set_wake(std::function<void()> wake);
// the latest a socket waits for part k's deflated copy: the hand-over + a tick
// + the bytes ahead of it (smaller sets of the same hand-over) and its parts up
// to k at Z_MIN_RATE (half the Pi 5's measured 56-64 MB/s)
int64_t z_deadline_ms(const FullSet& s, size_t k);
int64_t z_now_ms();
// SWEEP_FULL:<n>: 1 = every bin; n >= 2 = at most n values (n < FULL_MIN_VALUES
// taken as FULL_MIN_VALUES): pool = the smallest power of two with
// ceil(bins / pool) <= n - one level of the store's max pyramid
constexpr uint32_t FULL_MIN_VALUES = 1024;
uint32_t full_pool(uint32_t n);   // for the running grid (1 = every bin)
// pool = a power of two (full_pool); nullptr = no data on this grid yet
std::shared_ptr<const FullSet> build_full(uint32_t pool = 1);
// several pools from one state of the store (one lock): the sets of one tick
std::vector<std::shared_ptr<const FullSet>> build_fulls(const std::vector<uint32_t>& pools);
constexpr size_t FULL_PART_VALUES = 524288;   // a type 11 part's values at most
// one type 11 part: flags 1 = last part, 2 = snapshot, 4 = paused; with z (the
// part's deflated copy) flag 8 and the zlib stream in place of the n values
std::string full_part(const FullSet& s, uint8_t flags, uint32_t set_id, size_t first, size_t n, const std::string* z = nullptr);
uint32_t grid_id();   // the current grid's id (0 = none)

// The levels of a window max-pooled to at most `points` points (dB over the
// reference; NaN = never measured) - the AI assistant's get_spectrum
bool pooled_db(double lo_hz, double hi_hz, int points, std::vector<float>& db, double* first_hz,
               double* step_hz, double* bin_hz);

// The engine's worker thread (main.cpp)
void start();
void stop();

}  // namespace sweep
