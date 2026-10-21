#pragma once

// Binary FSK frame receiver: the FM discriminator (Hz) in, frames of soft
// symbols out.
//
// A one-symbol running mean (the matched filter for rectangular frequency
// pulses) is read at fractional positions. P timing hypotheses sample it on
// FIXED grids offset k/P of a symbol from each other, each slicing against its
// own running level and keeping its last S hard symbols. When those match the
// frame sync (or its complement: an inverted spectrum) within E errors, the
// sync's own samples give the levels - centre = carrier offset, deviation (its
// sign = polarity) - and a capture of N symbols starts one symbol later on its
// own clock, tracked with a Gardner detector at symbol changes. The grids are
// never moved by the tracking (a grid pulled onto noise after a frame would
// miss the next sync). One frame is usually captured by several hypotheses;
// the families keep the first that passes their checks.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sonde {

struct FskParams {
    double rate = 4800;          // symbols / s
    std::string sync;            // '0' / '1' in transmission order (the last <= 64 are used)
    int max_err = 2;             // E
    int nsym = 0;                // N: symbols captured after the sync
    int phases = 8;              // P
};

struct FskCapture {
    std::vector<float> sym;      // N soft symbols, +1 = the sync's "1" level
    double pos0 = 0;             // sample position of the first captured symbol
    double pos_end = 0;          // the capture's final sampling position
    double pos_sync = 0;         // pos0 - S symbols: the sync's first sample
    float centre = 0;            // Hz: the carrier offset
    float dev = 0;               // |deviation|, Hz
    int errors = 0;              // sync errors
    bool inverted = false;
};

class FskRx {
public:
    void init(const FskParams& p);
    void reset();
    // n discriminator samples, the first at sample index `idx` (consecutive
    // calls must be contiguous); finished captures are appended in the order
    // they finished
    void process(const float* x, size_t n, int64_t idx, std::vector<FskCapture>& out);
    double sps() const { return sps_; }

private:
    static constexpr int RING = 1 << 14;   // filter outputs kept (>> one chunk + a few symbols)
    static constexpr int CHUNK = 4096;

    struct Hyp {
        double off = 0;                    // grid offset in samples (k/P of a symbol)
        int64_t j = 0;                     // next grid index: position = origin + off + j * sps
        float level = 0;
        bool have_level = false;
        uint64_t reg = 0;                  // last S hard symbols, newest in bit 0
        float soft[64] = {};               // their samples
        int w = 0;                         // soft[] write index
        int count = 0;                     // symbols since start / the last capture
        bool cap = false;                  // capturing
        double cpos = 0;                   // capture: next sampling position
        int cn = 0;
        float cprev = 0;
        FskCapture c;
    };

    FskParams p_;
    double sps_ = 10;
    int L_ = 2;                            // running-mean length
    int S_ = 32;
    uint64_t pat_ = 0, mask_ = 0;
    std::vector<Hyp> h_;
    std::vector<FskCapture> done_;         // finished in this call (sorted, then handed over)
    std::vector<float> xr_, yr_;           // input / filter output rings (by sample index)
    double sum_ = 0;
    int64_t n_ = 0;                        // samples seen since reset (next index - start)
    int64_t start_ = -1;                   // index of the first sample since reset
    int64_t resum_ = 0;
    double origin_ = 0;                    // grid origin (sample index)

    float y(int64_t i) const { return yr_[static_cast<size_t>(i & (RING - 1))]; }
    float at(double pos) const {
        const double f = std::floor(pos);
        const int64_t i = static_cast<int64_t>(f);
        const float a = y(i), b = y(i + 1);
        return a + static_cast<float>(pos - f) * (b - a);
    }
    void run(Hyp& h, int64_t newest, std::vector<FskCapture>& out);
};

}  // namespace sonde
