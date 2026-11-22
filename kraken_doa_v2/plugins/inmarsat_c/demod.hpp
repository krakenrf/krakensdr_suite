#pragma once
// Coherent BPSK demodulator for the 1200 symbols/s Inmarsat-C TDM.
//
// Acquisition: 8192 samples (0.85 s at 9600 Hz) - the carrier's coarse
// place from the spectrum's shape (a raised-cosine template, alpha = 1,
// slid over +-3.6 kHz), then the fine carrier from the line the squared
// signal (BPSK -> 2 fc) shows within +-60 Hz of it.
// Tracking: NCO + RRC matched filter (alpha 1, measured on the real signal's
// spectrum) + Gardner symbol timing (cubic interpolation, a sample-clock
// integrator) + a decision-directed Costas loop (2nd order, ~10 Hz).
// Output: one soft symbol per 1/1200 s (real part, normalised to ~ +-1;
// the 180 degree ambiguity is resolved by the unique word).

#include <complex>
#include <cstdint>
#include <vector>

#include "kraken_dsp.hpp"
#include "kraken_fft.hpp"

namespace inmc {

struct Symbol {
    float soft = 0;      // in-phase, ~ +-1
    float quad = 0;      // quadrature (noise / phase error)
    double pos = 0;      // input sample index of the symbol's centre
};

class BpskDemod {
public:
    explicit BpskDemod(double fs);
    ~BpskDemod();
    BpskDemod(const BpskDemod&) = delete;
    BpskDemod& operator=(const BpskDemod&) = delete;

    void reset();                         // back to acquisition
    void process(const kp::cf* x, size_t n, std::vector<Symbol>& out);

    bool tracking() const { return state_ == TRACK; }
    double carrier_hz() const;            // current carrier estimate (Hz from 0 Hz of the input)
    float lock_quality() const { return lock_; }   // ~1 clean .. ~0 noise
    float esn0_db() const;                // from the symbol scatter
    double sps_estimate() const { return sps_; }
    int acquisitions() const { return acq_count_; }
    double last_acq_peak_db() const { return acq_peak_db_; }
    int last_acq_samples() const { return acq_used_; }
    // Shift the carrier estimate by df Hz (the host moved the input - not used now)
    void set_search_limit(double hz) { search_hz_ = hz; }

private:
    enum State { ACQ, TRACK };
    struct Stage;
    void acquire(Stage& st);
    void track_sample(kp::cf x, std::vector<Symbol>& out);
    kp::cf interp(double idx) const;

    double fs_;
    double sps_nom_;
    State state_ = ACQ;
    double search_hz_ = 3600;

    // acquisition: a quick 0.21 s try for strong carriers, then 0.85 s blocks
    static constexpr int NQUICK = 2048;
    static constexpr int NACQ = 8192;
    static constexpr double QUICK_DB = 20.0;   // the squared-signal line over its noise, 2048 points
    static constexpr double FULL_DB = 11.0;    // ... 8192 points
    struct Stage {
        Stage(int n, double fs, double thr);
        ~Stage();
        Stage(const Stage&) = delete;
        Stage& operator=(const Stage&) = delete;
        int N;
        double thr_db;
        std::vector<float> win, tmpl;
        kfft_complex* in = nullptr;
        kfft_complex* out = nullptr;
        kfft_plan plan = nullptr;
    };
    Stage quick_, full_;
    bool quick_tried_ = false;
    int acq_used_ = 0;
    std::vector<kp::cf> acq_buf_;
    int acq_count_ = 0;
    double acq_peak_db_ = 0;
    int64_t acq_start_ = 0;   // input index of acq_buf_[0]

    // tracking
    double phase_ = 0, freq_ = 0;   // NCO, rad / sample
    kp::Fir<kp::cf> mf_;
    std::vector<kp::cf> hist_;      // matched-filter output ring
    int64_t mf_count_ = 0;          // matched-filter outputs so far (since TRACK)
    int64_t track_start_ = 0;       // input index of the first sample tracked
    double next_ = 0;               // next symbol strobe (matched-filter index)
    double sps_ = 8;
    kp::cf prev_sym_{0, 0};
    float amp2_ = 1;                // mean |symbol|^2
    float lock_ = 0, sig_ = 1, noise_ = 1;
    int64_t symbols_ = 0;
    int unlocked_symbols_ = 0;
    int64_t n_in_ = 0;              // input samples so far
};

}  // namespace inmc
