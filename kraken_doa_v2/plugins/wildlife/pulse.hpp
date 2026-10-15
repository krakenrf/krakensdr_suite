#pragma once
// Pulse detector for wildlife tracking transmitters (pulsed CW): anywhere in
// the input band, at any input rate (the VFO's own - a narrow VFO on one
// collar or a 2.4 MHz band survey).
//
// - short FFTs (~200-300 Hz bins, 50 % overlap, Hann), a noise floor per bin
//   (gated mean; creeps up under a steady carrier, so a birdie or a CW
//   interferer stops being a "pulse" within seconds)
// - two detectors per bin: one frame (short pulses: Lotek coded tags' few
//   ms) and the sum of ~14 ms of frames (a collar's 10-40 ms pulse, weak)
// - broadband frames (ignition / power line impulses: many bins at once)
//   blanked; a hit must be a narrow line (its neighbouring bins quiet)
// - a finished pulse is measured on the raw samples: mixed to its bin,
//   summed to ~32 kHz, a +-1 kHz FIR; leading / trailing edge at the
//   midpoint between noise and pulse power, frequency from the phase slope,
//   power, C/N0
#include "kraken_plugin.hpp"

#include <complex>
#include <cstdint>
#include <utility>
#include <vector>

#include <fftw3.h>

namespace wl {

struct Pulse {
    double t0 = 0, t1 = 0;     // leading / trailing edge (input sample index, fractional)
    double freq = 0;           // carrier, Hz from the input's 0 Hz
    float power_db = 0;        // pulse power (input units^2, dB)
    float cn0_db = 0;          // carrier to noise density (dBHz)
    float peak_ratio = 0;      // detection statistic (power over floor)
    bool gated = false;        // found by a tracking gate (lower threshold)
    double width_ms(double rate) const { return (t1 - t0) / rate * 1e3; }
};

// a window where a known collar's next pulse is expected: a lower threshold
struct Gate {
    double freq = 0;           // Hz
    uint64_t from = 0, to = 0; // input samples
};

class PulseDetector {
public:
    PulseDetector() = default;
    ~PulseDetector();
    PulseDetector(const PulseDetector&) = delete;
    PulseDetector& operator=(const PulseDetector&) = delete;

    void configure(double rate);
    void reset();
    // n samples in; finished pulses appended to out
    void push(const kp::cf* x, size_t n, std::vector<Pulse>& out);
    void set_gates(const std::vector<Gate>& g) { gates_ = g; }
    // sensitivity: 0 = normal, +1 = more (more false pulses), -1 = less
    void set_sensitivity(int s);

    double rate() const { return rate_; }
    double bin_hz() const { return rate_ / N_; }
    uint64_t samples() const { return abs_; }
    float floor_median_db() const { return floor_med_db_; }   // per bin, dB
    float cn0_ref() const;                                     // dBHz of a pulse at the detection limit
    uint64_t blanked_frames() const { return blanked_; }
    uint64_t rejected_wide() const { return rej_wide_; }
    uint64_t rejected_len() const { return rej_len_; }
    // pulses that didn't look keyed: on outside the edges / level not steady / frequency moving
    uint64_t rej_edge_ = 0, rej_var_ = 0, rej_fm_ = 0;

private:
    struct Active {
        int bin = 0;            // peak bin (FFT index order 0..N-1)
        uint64_t f_first = 0, f_last = 0, f_peak = 0;
        float peak = 0;          // best ratio
        float pk_off = 0;        // parabolic offset of the peak bin at the peak frame
        bool narrow = true;
        bool gated = false;
    };
    void frame(std::vector<Pulse>& out);
    void finish(const Active& a, std::vector<Pulse>& out);
    double bin_freq(double k) const { return (k < N_ / 2 ? k : k - N_) * rate_ / N_; }

    double rate_ = 0;
    int N_ = 0, hop_ = 0, L_ = 1;
    std::vector<float> win_;
    float win_pow_ = 1;          // sum w^2
    fftwf_complex* fin_ = nullptr;
    fftwf_complex* fout_ = nullptr;
    fftwf_plan plan_ = nullptr;
    // raw ring (power of two)
    std::vector<kp::cf> ring_;
    uint64_t mask_ = 0;
    uint64_t abs_ = 0;           // samples received
    uint64_t next_frame_ = 0;    // sample index where the next frame ENDS
    uint64_t frames_ = 0;
    // per bin
    std::vector<float> P_, floor_, sum_;
    std::vector<float> hist_;    // L_ x N_ frame powers (sliding sum)
    std::vector<uint8_t> used_;  // bins inside the usable band
    std::vector<int> cand_;
    std::vector<Active> act_;
    struct Seen { int bin; double t0, t1; };
    std::vector<Seen> dead_;   // the pulses reported lately (bin, edges in samples)
    std::vector<Gate> gates_;
    float t_short_ = 0, t_long_ = 0, t_gate_ = 0, t_cont_ = 0;
    int sens_ = 0;
    float floor_med_db_ = -200;
    uint64_t blanked_ = 0, rej_wide_ = 0, rej_len_ = 0;
    // refinement scratch
    std::vector<kp::cf> z_, w_;
    std::vector<float> e_;
};

}  // namespace wl
