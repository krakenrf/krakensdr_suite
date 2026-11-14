#pragma once

// The sonde families. Each runs its own receiver on the discriminator of the
// channel filter it needs (narrow +-7 kHz: RS41, DFM, iMet-54, LMS6, MRZ; wide
// +-12 kHz: M10, M20, iMet-4) and appends a Record for every frame that passed
// its checks. They run side by side on every block.

#include "fsk_rx.hpp"
#include "rscode.hpp"
#include "sonde.hpp"

#include <array>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <vector>

namespace sonde {

class Family {
public:
    virtual ~Family() = default;
    // n samples of both discriminators (Hz), the first at sample index idx
    virtual void process(const float* narrow, const float* wide, size_t n, int64_t idx, const Ctx& ctx,
                         std::vector<Record>& out) = 0;
    virtual void reset() = 0;
};

// The decoder's set, in the order they run
std::vector<std::unique_ptr<Family>> make_families();

// --- helpers the families share ---------------------------------------------------
// the first-symbol position of the last accepted frame: captures within the
// window are the same frame caught at another timing phase
struct DupWindow {
    double last = -1e18;
    bool near(double pos, double window_s) const { return std::fabs(pos - last) < window_s * FS; }
    void accept(double pos) { last = pos; }
    void reset() { last = -1e18; }
};

// the record's carrier offset and span from a capture
inline void set_span(Record& r, const FskCapture& c, const Ctx& ctx) {
    r.offset = c.centre;
    r.t_first = ctx.clk.at(c.pos_sync);
    r.t_end = ctx.clk.at(c.pos_end);
}

}  // namespace sonde
