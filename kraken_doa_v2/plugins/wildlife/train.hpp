#pragma once
// Pulse-train analysis of wildlife transmitters: the period of a train
// with missed pulses and extra (coded) ones, and the coded data streams
// the NZ kiwi / kakapo transmitters send.
#include <string>
#include <vector>

namespace wl {

struct Period {
    double period = 0;     // s, 0 = none
    double t_ref = 0;      // a pulse on the grid (the latest)
    int support = 0;       // pulses on the grid
    int slots = 0;         // grid slots in the span
    double jitter = 0;     // rms residual (s)
    double score = -1e9;
};
// The best period in [pmin, pmax] for these leading edges (sorted, s):
// candidates from pairwise intervals / 1..4, scored by the pulses on the
// grid minus half the empty slots (so P/2 and 2P lose against P)
Period find_period(const std::vector<double>& t, double pmin, double pmax);
// tolerance of a pulse to its grid slot
inline double grid_tol(double period) { return std::max(0.025, 0.035 * period); }

// One data stream of a train, as runs of close pulses (R, length >= 2)
// and single normal pulses (N): the digit codes NZ transmitters send every
// ~10 minutes (DOC Kiwi Best Practice Manual 6.4.3 / 6.4.4; Wildtech
// kakapo Egg Timer manual V6.3): each digit = its value + 2 pulses.
//   double-beep scheme (mortality time-since-death): R(2) N*(d+2) R(2) ...
//   burst scheme (Egg Timer / Chick Timer): R(d+2) per digit, tens and units
//   back to back (N 0..1 between them), outputs apart (N ~5)
struct StreamEvent {
    int run = 1;           // 1 = a normal pulse, >= 2 = a run of close pulses
    double t = 0;
};
struct StreamResult {
    bool ok = false;
    std::string scheme;              // "double-beep" | "burst"
    std::vector<int> numbers;        // decoded values (digits for double-beep)
    std::string kind;                // "time-since-death" | "chick-timer" | "egg-timer" | "code"
    std::string text;                // human readable
    std::vector<std::pair<std::string, std::string>> fields;
};
StreamResult decode_stream(const std::vector<StreamEvent>& ev);

}  // namespace wl
