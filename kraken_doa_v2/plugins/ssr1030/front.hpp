#pragma once

// Pulse front end of the ssr1030 plugin: 2.4 MHz complex samples in, the
// pulses of 1030 MHz interrogations out.
//
// SSR / Mode S pulses are 0.8 us (P1, P2, P3, S1, short P4) or 1.6 us (long
// P4) - two to four samples at 2.4 MHz - and P1 -> P2, S1 -> P1, P3 -> P4
// are only 2 us apart, so they are measured on the RAW power, sample by
// sample: a pulse = a local peak, its edges where the power crosses a
// quarter of that peak (half amplitude), interpolated. The Mode S data
// pulse P6 (16.25 / 30.25 us of DPSK) dips between its chips: it is found on
// the 5-sample mean instead (a run of >= 10 us).
//
// The noise is the median of 64-sample means (pulses, the 8-bit
// quantisation); the samples are kept in a ring for the DPSK decoder.

#include "kraken_plugin.hpp"

#include <cstdint>
#include <vector>

namespace ssr {

constexpr double FS = 2.4e6;
constexpr double US = 1e6 / FS;   // one sample in us

struct Pulse {
    double le = 0, te = 0;      // half-amplitude crossings (absolute sample index, fractional)
    float peak = 0;             // peak power (full scale = 1)
    float db = 0;               // ... dB over the noise
    bool clipped = false;       // an I / Q sample at the ADC's full scale
    bool longp = false;         // a long pulse (Mode S P6, maybe with P2 merged in front)
    float med = 0;              // long pulses: median power over the run
    double width_us() const { return (te - le) * US; }
};

class FrontEnd {
public:
    FrontEnd();
    void reset();
    void set_threshold_db(double db);
    // n samples; finished pulses are appended to out (in time order)
    void push(const kp::cf* x, size_t n, std::vector<Pulse>& out);
    // a sample still in the ring (absolute index; older than RING samples = garbage)
    const kp::cf& at(int64_t i) const { return hx_[static_cast<size_t>(i & HM)]; }
    int64_t count() const { return n_; }            // samples pushed so far
    double noise() const { return noise_; }          // mean noise power per sample (0 = not known yet)
    static constexpr int64_t RING = int64_t(1) << 15;   // 13.7 ms

private:
    static constexpr int64_t HM = RING - 1;
    static constexpr int GAP = 5;                    // quiet samples that end a segment
    static constexpr int NBLK = 64;
    static constexpr size_t NBLOCKS = 768;           // noise: median over 20 ms
    static constexpr int64_t MAX_SEG = 2400;         // 1 ms: longer = a carrier, not pulses

    std::vector<kp::cf> hx_;
    std::vector<float> hp_;
    int64_t n_ = 0;
    float blk_acc_ = 0;
    int blk_n_ = 0;
    std::vector<float> blocks_;
    double noise_ = 0;
    double thr_db_ = 10;
    float thr_ = 0;
    float clip_ = 0.96f;
    bool in_seg_ = false;
    int64_t seg_a_ = 0, last_act_ = 0;
    std::vector<float> m5_;
    std::vector<uint8_t> used_;

    float P(int64_t i) const { return hp_[static_cast<size_t>(i & HM)]; }
    void segment(int64_t a, int64_t b, std::vector<Pulse>& out);
    bool clipped(int64_t a, int64_t b) const;
};

}  // namespace ssr
