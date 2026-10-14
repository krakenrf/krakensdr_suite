#pragma once

// Antenna scan of one radar: its pulse levels in 5 ms bins over the last
// 41 s. A rotating antenna's main beam sweeps past once per turn: a burst of
// pulses 20-50 dB over its sidelobes. Bursts within 6 dB of the strongest
// that stay up there about as long as the longest one are main-beam passes
// (a strong reflection the beam lights elsewhere in the turn is shorter up
// there - and when the main beam clips the receiver, its peak level no longer
// tells it apart); their spacing is the scan period.

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace radar {

struct Burst {
    double t = 0;           // the main beam's centre (Host::time())
    double from = 0, to = 0;
    float peak_db = 0;      // its strongest pulse (dB over the noise)
    bool clipped = false;   // ... clipped the ADC (the true peak is higher)
    bool open = false;      // may still go on (ends near the newest bin)
    double dwell = 0;       // time its pulses stayed within rel_db of the strongest (s)
};

class LevelSeries {
public:
    static constexpr double BIN_S = 0.005;
    static constexpr int NB = 8192;
    LevelSeries() : lv_(NB, NONE), clip_(NB, 0) {}
    void add(double t, float db, bool clipped);
    // the strongest bin in [t0, t1] (NONE = nothing)
    float max_over(double t0, double t1) const;
    // time (s) the bins in [t0, t1] are at or above level
    double dwell_over(double t0, double t1, float level) const;
    // main-beam bursts in the kept span: bins within rel_db of the strongest,
    // gaps up to max_gap_s bridged
    std::vector<Burst> bursts(double rel_db, double max_gap_s) const;
    // (time, level) of every bin with a pulse in [t0, t1]
    void bins(double t0, double t1, std::vector<std::pair<double, float>>& out) const;
    // median level of the bins with a pulse (NONE = none)
    float median_level() const;
    double newest_t() const { return newest_ < 0 ? -1 : (static_cast<double>(newest_) + 0.5) * BIN_S; }
    static constexpr float NONE = -1e9f;

private:
    std::vector<float> lv_;
    std::vector<uint8_t> clip_;
    int64_t newest_ = -1;      // absolute bin index of the newest bin
    bool kept(int64_t b) const { return newest_ >= 0 && b <= newest_ && b > newest_ - NB; }
};

// Scan period from main-beam times (ascending): the spacing most beams
// repeat at (+-max(40 ms, 1.5 %)); refined by a fit over the chain of beams.
// 0 = not known (fewer than 3 beams agree). support = beams in the chain
double scan_period(const std::deque<double>& beams, int* support);

// Fits a Gaussian main lobe (a parabola in dB) to (time, level dB) points of
// unclipped pulses around a main beam: centre, peak level (extrapolated when
// the top clipped) and the -3 dB width (s). false if it doesn't look like a lobe
bool fit_lobe(const std::vector<std::pair<double, float>>& pts, double* t0, double* peak_db, double* width3_s);

}  // namespace radar
