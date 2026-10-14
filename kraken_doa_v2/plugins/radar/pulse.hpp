#pragma once

// Pulse detector of the radar plugin: the whole 2.4 MHz the tuner receives,
// one complex sample in, pulse descriptor words (PDWs) out.
//
// Per sample only |x|^2 and two running sums: 3 samples (1.25 us, short
// pulses) and 24 samples (10 us: long pulses - a 60 us chirp in the radar's
// sidelobes is ~10 dB per sample and fades across its sweep; summed over 10
// us it is clean) against the noise floor (median of 64-sample means: robust
// to pulses, CW and the 8-bit quantisation). A run of samples where either
// sum is over its threshold is a segment; it is measured on the raw samples
// kept in a ring. Inside it, runs of the 3-sample mean over the short
// threshold, apart by more than 4 us, are separate pulses (a pulse group);
// a segment only the long sum saw is one weak pulse (9-sample mean).
//
// Measured per pulse: the OUTERMOST half-amplitude (-6 dB) crossings of the
// smoothed power - a chirp that fades across its sweep (multipath, the
// tuner's ripple) keeps its true width - corrected for the smoothing, so a
// rectangular pulse's width is exact; leading edge, width, peak level
// (dB over the noise and dBFS), clipping (an I or Q sample at the ADC's full
// scale), centroid frequency (phase steps over the pulse), linear FM rate
// (frequency of 6-sample pieces fitted against time) and its residual.

#include "kraken_plugin.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace radar {

constexpr double FS = 2.4e6;

struct Pulse {
    double toa = 0;          // Host::time() of the leading edge (half amplitude)
    double width_us = 0;     // half-amplitude width
    double peak_db = 0;      // peak power over the noise floor (dB)
    double peak_dbfs = 0;    // peak power, dB re full scale (|x| = 1)
    double freq_hz = 0;      // centroid frequency in the input band (Hz)
    double chirp = NAN;      // linear FM rate (Hz per us; NAN = too short to tell)
    double span_hz = 0;      // chirp x width: the sweep over the pulse
    double fit_rms = NAN;    // rms of the frequency fit's residual (Hz)
    double bw_hz = 0;        // occupied bandwidth (the DoA's channel filter)
    bool clipped = false;    // an I / Q sample at full scale: the level is a lower bound, the phases are bent
    double t_a = 0, t_b = 0; // Host::time() of the DoA span: the pulse + margins (whole covariance chunks)
};

class Detector {
public:
    Detector();
    void reset();
    // threshold of the short sum over the noise (dB); the long one is 6 dB lower
    void set_threshold_db(double db);
    // ADC full scale of one I / Q component (the input is (u8 - 127.5) / 127.5)
    void set_clip_level(double v) { clip_ = static_cast<float>(v); }
    // n samples; t0 = Host::time() of the first. Finished pulses are appended to out
    void push(const kp::cf* x, size_t n, double t0, std::vector<Pulse>& out);
    double noise() const { return noise_; }          // mean noise power per sample (0 = not known yet)
    uint64_t long_segments() const { return long_segs_; }   // runs > MAX_LEN (CW / interference)
    uint64_t samples() const { return static_cast<uint64_t>(n_); }

private:
    static constexpr int HBITS = 15;
    static constexpr int64_t HN = int64_t(1) << HBITS;   // history: 13.7 ms (fits the L2 cache)
    static constexpr int64_t HM = HN - 1;
    static constexpr int W_S = 3, W_L = 24;              // the two sums
    static constexpr int64_t GAP = 6;                    // a segment ends after this many quiet samples
    static constexpr int64_t MAX_LEN = 4800;             // 2 ms: longer = not a pulse
    static constexpr int NBLK = 64;                      // noise: means of this many samples
    static constexpr size_t NBLOCKS = 1536;              // ... median over this many (41 ms)
    static constexpr int MARGIN = 24;                    // DoA span margin each side (samples)

    std::vector<kp::cf> hx_;
    std::vector<float> hp_;
    int64_t n_ = 0;                  // samples pushed (absolute index of the next one)
    double t_ref_ = 0;               // Host::time() of absolute sample 0
    float blk_acc_ = 0;
    int blk_n_ = 0;
    std::vector<float> blocks_;
    double noise_ = 0;
    double thr_db_ = 10;
    float thr3_ = 0, thr24_ = 0;     // thresholds of the sums (0 = no noise yet)
    float clip_ = 0.96f;
    bool in_seg_ = false;
    int64_t seg_a_ = 0, last_act_ = 0;
    uint64_t long_segs_ = 0;
    std::vector<float> e_, e9_;      // scratch: envelopes of a segment
    std::vector<double> fit_f_, fit_t_, fit_w_;   // scratch: the LFM fit

    void thresholds();
    void segment(int64_t a, int64_t b, std::vector<Pulse>& out);
    // the pulse whose half-amplitude crossings are le / te (absolute, fractional)
    bool measure(double le, double te, Pulse* p);
    float P(int64_t abs_i) const { return hp_[static_cast<size_t>(abs_i & HM)]; }
    double t_of(double abs_i) const { return t_ref_ + abs_i / FS; }
};

}  // namespace radar
