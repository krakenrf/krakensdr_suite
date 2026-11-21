#pragma once

// Coherent demodulator for the Classic Aero modulations (ICAO Annex 10 Vol
// III, 4.1.1.1 / 4.3.1):
//
// - A-BPSK (0.6 / 1.2 / 2.4 kbit/s): every bit turns the carrier phase by
//   -90 deg ("0") or +90 deg ("1"); the stream is root-raised-cosine
//   filtered at the bit rate. Seen from an axis that turns by 90 deg per
//   bit it is BPSK in which a "1" keeps the sign and a "0" flips it.
// - A-QPSK (4.8 / 8.4 / 10.5 kbit/s): offset QPSK; the bits go alternately
//   on I (0 / 180 deg) and Q (90 / 270 deg), Q half a symbol (one bit) late.
//
// Both are taken as one bit per bit period on a turning axis: a_n = j^n
// (A-BPSK) or j^(n mod 2) (A-QPSK). The decision variable is the
// interpolated matched-filter output projected onto a_n, after the carrier
// phase. Loops: a decision-directed Costas loop (error = sign(d) x the
// quadrature component, second order) and a Gardner timing loop (A-QPSK:
// a branch's samples two bits apart and the sample between them; A-BPSK:
// adjacent bits and the half-bit sample). Fractional timing by cubic
// interpolation of the matched filter's output. The carrier frequency
// comes from the acquisition (aero_acq.hpp); the loop's frequency is moved
// into the input mixer every 32 bits.

#include "kraken_dsp.hpp"

#include <cmath>
#include <complex>
#include <functional>
#include <vector>

namespace aero {

using cf = std::complex<float>;

enum class Mod { ABPSK, AQPSK };

struct DemodCfg {
    Mod mod = Mod::AQPSK;
    double rate = 10500;    // bit/s
    double fs = 24000;      // input rate
    int decim = 1;          // input decimation before the matched filter
    float rolloff = 0.6f;   // matched filter roll-off
    float pll_bw = 0.0012f; // loop noise bandwidth x bit period, tracking
    float tim_gain = 1.2e-4f;
};

// One bit out of the demodulator: soft value (+ = the axis' positive
// direction, ~ +-1), its time in input samples (since the demodulator's
// start), and the squared amplitude of the quadrature part (noise figure).
struct SoftBit {
    float d;
    double t;
};

class Demod {
public:
    void start(const DemodCfg& c, double f0_hz, double t0_samples);
    void stop() { on_ = false; }
    bool running() const { return on_; }
    // input at cfg.fs; t0 = the input sample index of x[0]
    void process(const cf* x, size_t n, std::vector<SoftBit>& out);
    double carrier_hz() const { return f_nco_ + nu_ * rate_ / (2 * M_PI); }
    float quality() const { return q_; }   // E|d| / sqrt(E d^2): ~1 locked, 0.8 noise
    uint64_t bits() const { return nbits_; }
    const DemodCfg& cfg() const { return cfg_; }
    void shift_carrier(double hz) {
        f_nco_ += hz;
        w_ = -2 * M_PI * f_nco_ / cfg_.fs;
    }
    void set_tsign(float s) { tsign_ = s; }

private:
    DemodCfg cfg_;
    bool on_ = false;
    double rate_ = 0, fsc_ = 0, sps_ = 0;   // matched-filter rate, samples per bit
    double f_nco_ = 0, ph_nco_ = 0, w_ = 0;
    double t0_ = 0;                          // input sample index of the first input
    float tsign_ = 1;                        // Gardner sign (A-BPSK)
    // decimator
    std::vector<float> hd_;
    std::vector<cf> dbuf_;
    int dpos_ = 0, dcnt_ = 0;
    // matched filter
    std::vector<float> hm_;
    std::vector<cf> mbuf_;
    int mpos_ = 0;
    // matched-filter output ring
    static constexpr int RING = 64;
    cf ring_[RING];
    int64_t nmf_ = 0;          // outputs so far
    double tn_ = 0;            // next bit instant (matched-filter output index)
    double delay_in_ = 0;      // input samples from a bit's instant to its matched-filter output
    // loops
    double theta_ = 0, nu_ = 0;
    float kp_ = 0, ki_ = 0, kt_ = 0;
    float amp_ = 1, q_ = 0.8f, ad_ = 1, ad2_ = 1;
    float dprev_[2] = {0, 0};
    cf yprev_{0, 0};
    uint64_t nbits_ = 0;
    int axis_ = 0;

    cf interp(double t) const;
    void bit(std::vector<SoftBit>& out);
    void gains(float bw);
};

std::vector<float> rrc_taps(double sps, int span, double beta);

}  // namespace aero
