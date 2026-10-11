#pragma once

// Front end: finds bursts in the band and demodulates each one.
//
// Input: FS (240 kHz, the ism plugin) or D x FS (ism_wide: 2.4 MHz, D = 10).
// Detection: an FFT of 64 D points (3.75 kHz bins either way, Hann) every
// 32 D samples; per bin a noise floor (mean of the bin's power while it holds
// no signal). A bin is active when it and its two neighbours, in this frame
// and the one 64 D samples before, hold `threshold` dB more than their floor.
// Runs of active bins are assigned to packages by frequency: up to max_pkgs
// at once, so two devices transmitting at the same time on different
// frequencies become two bursts (each demodulated - and its DoA filtered -
// on its own). A package runs until GAP_END of quiet in its region (OOK gaps
// inside a package are much shorter), or MAX_LEN, and is centred on the
// middle of the bins it occupied - one carrier for OOK, between the two tones
// for FSK.
//
// Demodulation at FS: with D > 1 the package's samples are first mixed down
// by its centre and decimated to FS (`make_segment`), so the demodulator and
// every protocol work at 240 kHz whatever the input. Two passes: a wide
// filter finds the carrier / the two FSK tones (2-means of the
// discriminator), then a filter centred on them gives
//   OOK: envelope > half the signal level (hysteresis, >= 3x the noise)
//        -> pulses (carrier on) and gaps;
//   FSK: pulses (the higher tone) and gaps - from the discriminator sliced
//        between the tracked tones, or for tones >= WIDE_FSK_HZ apart from
//        the two tones' own filtered levels compared.
// Runs shorter than GLITCH_US are merged away. Burst positions are in FS
// samples (input sample / D), Burst::fc is the offset in the whole input band.

#include "ism.hpp"

#include <fftw3.h>

#include <vector>

namespace ism {

class FrontEnd {
public:
    explicit FrontEnd(int decim = 1);
    ~FrontEnd();
    FrontEnd(const FrontEnd&) = delete;
    FrontEnd& operator=(const FrontEnd&) = delete;

    void reset();
    void set_threshold_db(double db);
    // samples in (at input_rate()); finished bursts are appended to *out
    void push(const cf* x, size_t n, std::vector<Burst>* out);
    double input_rate() const { return fs_in_; }
    double abs_time() const { return static_cast<double>(abs_) / fs_in_; }   // input since reset (s)
    double noise_floor_db() const;             // per-sample noise power, dB (full scale = 0)

    static constexpr double GAP_END_S = 0.030;            // quiet that ends a package
    static constexpr double MAX_LEN_S = 1.20;             // longer ones are cut (a held key fob; Prologue: 7 copies in 0.95 s)
    static constexpr double MARGIN_S = 0.0025;            // demodulated before / after
    static constexpr double GLITCH_US = 18;
    static constexpr double DIP_HOLD_US = 20;             // FSK: a carrier dip this short keeps the tone
    static constexpr double WIDE_FSK_HZ = 50e3;           // tones further apart: compared by level, not discriminator
    static constexpr int MARGIN_BINS = 6;                 // a package takes what is within 22.5 kHz of its region
    static constexpr int FSK_SPAN_BINS = 40;              // the two tones of one transmitter: <= 150 kHz apart
    static constexpr double LEARN_S = 0.050;              // a package's region grows for 50 ms (CurrentCost: 33 ms of one tone)
    static constexpr double TOGETHER_S = 0.002;           // packages ending this close: one transmitter's tones
    static constexpr double DEC_CUT_HZ = 108e3;           // D > 1: the segment's low-pass (flat to ~88 kHz, -74 dB from 128 kHz)
    static constexpr int DEC_TAPS = 321;

private:
    const int D_;                  // decimation to FS
    const double fs_in_;           // input rate
    const int nfft_, hop_;         // 64 D, 32 D
    const int max_pkgs_;           // packages (devices) at once: 4 in 240 kHz, 8 in 2.4 MHz
    const int64_t ring_n_;         // input ring: 2.18 s at 240 kHz, 1.75 s at 2.4 MHz
    std::vector<cf> ring_;
    int64_t abs_ = 0;
    int hop_fill_ = 0;
    fftwf_complex* fin_ = nullptr;
    fftwf_complex* fout_ = nullptr;
    fftwf_plan plan_ = nullptr;
    std::vector<float> win_;
    float win_pow_ = 0;            // sum of w^2
    std::vector<float> P_, P1_, P2_, N_;   // this frame, the 2 before, noise floor (frequency order)
    bool noise_init_ = false;
    int init_frames_ = 0;
    double thr_on_ = 10, thr_cont_ = 8;   // ratios
    float thr_cont_f_ = 8;                // the largest float <= thr_cont_
    const bool trace_;                    // ISM_TRACE: the detector's segments to stderr

    // the packages in progress
    struct Pkg {
        int64_t a = 0;             // first active frame start
        int64_t first = 0;         // its end (when the region stops growing: + LEARN_S)
        int64_t last = 0;          // last active frame end
        int lo = 0, hi = 0;        // frequency region (signed bins)
        std::vector<float> acc;    // power over the noise floor, summed over its active frames
    };
    std::vector<Pkg> pkgs_;
    // per-frame scratch
    struct Seg { int lo, hi; float peak; };
    std::vector<Seg> segs_;
    std::vector<float> rr_, tmp_;
    std::vector<char> taken_;

    // what demod reads: D = 1 the ring itself, else the segment of the
    // package being finished, mixed down by seg_f_ and decimated to FS
    std::vector<cf> seg_;
    int64_t seg0_ = 0;             // FS sample of seg_[0]
    double seg_f_ = 0;             // its centre in the input band (Hz)
    double seg_noise_ = 0;         // noise power per sample of what demod reads
    std::vector<float> dec_h_;     // D > 1: the decimation filter
    double dec_h2_ = 1;            // its sum(h^2)

    void frame(std::vector<Burst>* out);
    void finish(const Pkg& p, std::vector<Burst>* out);
    void make_segment(int64_t s0, int64_t s1, double f);
    void demod(int64_t a, int64_t b, double fc, double bw, Burst* out) const;
    void filter(int64_t a, int64_t b, double f0, double fcut, double k, std::vector<cf>* y, double* h2) const;
    double sigma(double h2) const;   // noise rms after a filter with sum(h^2) = h2
    double median_floor(int lo, int count) const;   // median noise floor of `count` bins from signed bin lo
    cf at(int64_t s) const { return ring_[static_cast<size_t>(s & (ring_n_ - 1))]; }
    cf src(int64_t s) const {      // FS sample s as demod sees it (outside: silence)
        if (D_ == 1) return s >= 0 && s > abs_ - ring_n_ && s < abs_ ? at(s) : cf{};
        s -= seg0_;
        return s >= 0 && s < static_cast<int64_t>(seg_.size()) ? seg_[static_cast<size_t>(s)] : cf{};
    }
    // per-bin arrays (P, N, Pkg::acc) are in frequency order: signed bin sb at sb + nfft / 2
    size_t ix(int sb) const { return static_cast<size_t>(sb + nfft_ / 2); }
};

}  // namespace ism
