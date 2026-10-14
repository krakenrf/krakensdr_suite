#pragma once

// Cell search: which LTE / NR cells are on the carrier, and their timing.
//
// 1. cp_offset(): the signal's carrier offset modulo the subcarrier spacing,
//    from the cyclic prefixes (every symbol's prefix repeats its end: x(n + N)
//    conj(x(n)) turns by 2 pi f N / fs there). The sum is folded over 0.5 ms
//    and its mean removed (a CW tone / DC adds the same at every position).
// 2. PSS fold: every antenna's samples correlated with the three PSS of the
//    standard (time domain, one OFDM symbol), |c|^2 folded modulo the PSS
//    period (LTE 5 ms, NR 20 ms = the default SS burst period): a cell =
//    a peak in the fold of its N_ID2.
// 3. SSS per PSS peak: at every occurrence the PSS gives the channel on the
//    sync subcarriers and the SSS symbol (LTE: one before the PSS (FDD) or
//    three (TDD); NR: two after) is equalised with it, summed over the
//    antennas and occurrences, and tested against every N_ID1 (LTE: x both
//    halves of the frame, x FDD / TDD). Cells sharing the N_ID2 and the
//    timing (synchronised networks) share the PSS peak: the best hypothesis
//    is projected out and the rest tested again (successive cancellation).

#include "ofdm.hpp"
#include "seq.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace cells {

enum class Tech { LTE, NR };

struct CellHit {
    Tech tech = Tech::LTE;
    int pci = 0;
    int nid2 = 0;
    bool tdd = false;                   // LTE
    double score = 0;                   // SSS detection score (~N(0,1) where there's no cell)
    double z = 0;                       // the best hypothesis against all of the peak's (robust z-score)
    int round = 0;                      // found after cancelling this many stronger cells at its PSS peak
    double df_hz = 0;                   // carrier offset left over (from the SSS's phase against the PSS)
    double frame0 = 0;                  // LTE: start of a radio frame (input samples, <= 0)
    std::vector<double> pss;            // NR: useful starts of its PSS occurrences (all its SSB beams)
    double pss_mod = 0;                 // its (strongest) PSS position modulo the fold period
    int beams = 1;                      // NR: SSB positions in the period with this cell
};

struct SearchParams {
    Tech tech = Tech::LTE;
    Numerology num;
    double carrier_off = 0;             // Hz: the carrier (LTE) / SSB (NR) centre from the VFO centre
    double vis_hz = 1.05e6;             // the VFO's usable half-width
    double band_hz = 0;                 // a decimated input's own passband around the carrier / SSB centre (0 = none)
    int fold_ants = 2;                  // antennas correlated for the PSS fold
    double min_score = 4;               // the SSS score (white-noise scale: only a sanity floor)
    double min_z = 0;                   // robust z-score of the best SSS hypothesis
    int max_peaks = 8;                  // PSS peaks per N_ID2 tested
    int max_per_peak = 4;               // cells per PSS peak (successive cancellation)
    int max_occ = 32;                   // PSS occurrences per peak in the SSS test
    int duplex = -1;                    // LTE: 0 = only FDD, 1 = only TDD (a locked carrier's), -1 = both
};

struct SearchOut {
    std::vector<CellHit> cells;
    std::vector<float> fold;            // max over N_ID2 of fold / median, one PSS period
    int period = 0;                     // input samples
    double rate = 0;                    // the input's sample rate (the fold's)
    int peaks = 0;                      // PSS peaks tested
};

// carrier offset (Hz) modulo the subcarrier spacing; strength = the CP
// correlation's coherence (0..1, ~0 without an OFDM signal)
double cp_offset(const std::vector<const cf*>& x, size_t n, const Numerology& num, double* strength);

// where a burst's time goes (verbose output)
struct Prof {
    double cp = 0, mix = 0, fold = 0, sss = 0, quick = 0, full = 0;
    int quick_n = 0, full_n = 0, peaks = 0;
};
inline Prof& prof() {
    static Prof p;
    return p;
}
inline double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// x: the antennas, mixed so that the carrier / SSB centre is at 0 Hz
SearchOut search(const std::vector<const cf*>& x, size_t n, const SearchParams& p);

// subcarrier k (signed, from the carrier / SSB centre) inside the VFO's usable band?
inline bool visible(int k, double scs, double carrier_off, double vis_hz) { return std::fabs(k * scs + carrier_off) <= vis_hz; }

}  // namespace cells
