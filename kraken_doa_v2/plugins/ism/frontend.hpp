#pragma once

// Front end: finds bursts in the 240 kHz band and demodulates each one.
//
// Detection: a 64-point FFT (3.75 kHz bins, Hann) every 32 samples; per bin
// a noise floor (mean of the bin's power while it holds no signal). A bin is
// active when it and its two neighbours, in this frame and the one 64
// samples before, hold `threshold` dB more than their floor. Runs of active
// bins are assigned to packages by frequency: up to MAX_PKGS at once, so two
// devices transmitting at the same time on different frequencies become two
// bursts (each demodulated - and its DoA filtered - on its own). A package
// runs until GAP_END of quiet in its region (OOK gaps inside a package are
// much shorter), or MAX_LEN, and is centred on the middle of the bins it
// occupied - one carrier for OOK, between the two tones for FSK.
//
// Demodulation (two passes): a wide filter finds the carrier / the two FSK
// tones (2-means of the discriminator), then a filter centred on them gives
//   OOK: envelope > half the signal level (hysteresis, >= 3x the noise)
//        -> pulses (carrier on) and gaps;
//   FSK: pulses (the higher tone) and gaps - from the discriminator sliced
//        between the tracked tones, or for tones >= WIDE_FSK_HZ apart from
//        the two tones' own filtered levels compared.
// Runs shorter than GLITCH_US are merged away.

#include "ism.hpp"

#include <fftw3.h>

#include <vector>

namespace ism {

class FrontEnd {
public:
    FrontEnd();
    ~FrontEnd();
    FrontEnd(const FrontEnd&) = delete;
    FrontEnd& operator=(const FrontEnd&) = delete;

    void reset();
    void set_threshold_db(double db);
    // samples in; finished bursts are appended to *out
    void push(const cf* x, size_t n, std::vector<Burst>* out);
    int64_t abs_pos() const { return abs_; }   // input samples since reset
    double noise_floor_db() const;             // per-sample noise power, dB (full scale = 0)

    static constexpr int NFFT = 64, HOP = 32;
    static constexpr int RING = 1 << 19;                  // 2.18 s
    static constexpr double GAP_END_S = 0.030;            // quiet that ends a package
    static constexpr double MAX_LEN_S = 1.20;             // longer ones are cut (a held key fob; Prologue: 7 copies in 0.95 s)
    static constexpr double MARGIN_S = 0.0025;            // demodulated before / after
    static constexpr double GLITCH_US = 18;
    static constexpr double DIP_HOLD_US = 20;             // FSK: a carrier dip this short keeps the tone
    static constexpr double WIDE_FSK_HZ = 50e3;           // tones further apart: compared by level, not discriminator
    static constexpr int MAX_PKGS = 4;                    // packages (devices) at once
    static constexpr int MARGIN_BINS = 6;                 // a package takes what is within 22.5 kHz of its region
    static constexpr int FSK_SPAN_BINS = 40;              // the two tones of one transmitter: <= 150 kHz apart
    static constexpr double LEARN_S = 0.050;              // a package's region grows for 50 ms (CurrentCost: 33 ms of one tone)
    static constexpr double TOGETHER_S = 0.002;           // packages ending this close: one transmitter's tones

private:
    std::vector<cf> ring_;
    int64_t abs_ = 0;
    int hop_fill_ = 0;
    fftwf_complex* fin_ = nullptr;
    fftwf_complex* fout_ = nullptr;
    fftwf_plan plan_ = nullptr;
    float win_[NFFT];
    float win_pow_ = 0;            // sum of w^2
    float P_[NFFT], P1_[NFFT], P2_[NFFT], N_[NFFT];   // this frame, the 2 before, noise floor
    bool noise_init_ = false;
    int init_frames_ = 0;
    double thr_on_ = 10, thr_cont_ = 8;   // ratios
    const bool trace_;                    // ISM_TRACE: the detector's segments to stderr

    // the packages in progress
    struct Pkg {
        int64_t a = 0;             // first active frame start
        int64_t first = 0;         // its end (when the region stops growing: + LEARN_S)
        int64_t last = 0;          // last active frame end
        int lo = 0, hi = 0;        // frequency region (signed bins)
        float acc[NFFT];           // power over the noise floor, summed over its active frames
    };
    std::vector<Pkg> pkgs_;

    void frame(std::vector<Burst>* out);
    void finish(const Pkg& p, std::vector<Burst>* out);
    void demod(int64_t a, int64_t b, double fc, double bw, Burst* out) const;
    void filter(int64_t a, int64_t b, double f0, double fcut, double k, std::vector<cf>* y, double* h2) const;
    double sigma(double h2) const;   // noise rms after a filter with sum(h^2) = h2
    cf at(int64_t s) const { return ring_[static_cast<size_t>(s & (RING - 1))]; }
};

}  // namespace ism
