#pragma once

// PRI (pulse repetition interval) analysis of one pulse train: the pulse
// times of one kind of pulse of one radar -> the PRIs it uses and how.
//
// Consecutive time differences are clustered (+-max(1.5 us, 0.12 %)); a
// cluster that is the sum of two or three smaller ones is a missed pulse,
// the rest are the radar's PRIs ("levels"). Their order says what kind of
// pattern it is:
//   constant  one PRI (jitter = its spread)
//   blocks    each PRI for a block of pulses, then the next (the PRF
//             switching of radars with coherent processing intervals: e.g.
//             8 x 1712 us, 8 x 1395 us, ...)
//   stagger   a fixed sequence of PRIs, pulse by pulse (stagger 3: 1100 /
//             1250 / 1180 us ...)
//   jitter    no fixed levels, but the differences cluster around one value
//             (random PRI jitter)

#include <string>
#include <vector>

namespace radar {

struct PriInfo {
    bool ok = false;
    std::string kind;                // constant | blocks | stagger | jitter
    std::vector<double> levels;      // the PRIs (us), in pattern order (blocks / stagger: one cycle)
    std::vector<int> counts;         // their differences seen
    int block = 0;                   // blocks: pulses per block
    double mean_pri = 0;             // us (PRF = 1e6 / mean_pri)
    double jitter_pct = 0;           // constant / jitter: spread (sd / mean, %)
    double coverage = 0;             // share of the differences explained (levels + missed pulses)
    int diffs = 0;                   // differences analysed
    double t0 = 0, t1 = 0;           // the pulses' time span
    bool straddles = false;          // the levels change inside the window (a PRI set switched)
    std::string text() const;        // "1712 / 1395 us in blocks of 8"
    std::vector<double> set() const; // the levels, sorted
};

// toas: pulse times (s), ascending
PriInfo analyze_pri(const std::vector<double>& toas);

// two PRI sets the same (within tolerance)?
bool same_set(const std::vector<double>& a, const std::vector<double>& b);
std::string set_text(const std::vector<double>& s);   // "1395 / 1712 us"

}  // namespace radar
