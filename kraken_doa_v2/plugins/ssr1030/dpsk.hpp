#pragma once

// Mode S uplink DPSK at 2.4 MHz: the 56 / 112 data chips of P6 are 0.25 us
// (4 Mchip/s, a phase reversal before a chip = a ONE) - 0.6 samples per
// chip, and the receiver passes only about +-1.1 MHz of the +-4 MHz the
// chips occupy. Chip by chip it can't be read; as a whole it can: the
// interrogation is a known waveform up to its chips (P1, P2, P6 with the
// sync phase reversal 1.25 us after its start), so every sample is a known
// linear function of the chip signs - through the receiver's response.
//
// The response is the receiver's complex STEP response S(t) (the R820T /
// RTL2832 filters + whatever reaches the antenna with the edge: multipath):
// an edge of the transmitted envelope with jump e at time u adds e S(t - u)
// to every sample. A chip c_k (+-1) adds c_k (S(t - t_k) - S(t - t_k - T)).
// A maximum-likelihood sequence estimator (Viterbi over the chips, 32
// states, per-survivor: older chips from each path's own history) finds the
// chips; the frame's P6 timing comes from a fit of the known part, its
// carrier offset decision-directed from the decoded frame (two passes).
//
// S starts as a table measured on this receiver type (live interrogations,
// see the README) and is re-fitted from every frame whose parity CHECKS
// (all-calls, broadcasts: a known address) - least squares of the whole
// waveform with the decoded chips, slowly forgetting.

#include "kraken_plugin.hpp"

#include <complex>
#include <cstdint>
#include <deque>
#include <vector>

namespace ssr {

struct DpskResult {
    int nbits = 0;
    uint8_t bits[112] = {};      // unpacked
    double t1 = 0, t6 = 0;       // P1 / P6 leading edges, us from the window's P1 estimate
    double df = 0;               // carrier offset (Hz) used
    std::complex<double> a1, a2, a6;   // amplitudes of P1, P2, P6 (full scale)
    double rel_err = 1;          // residual / signal energy of the whole fit
    double p2_db = 0;            // P2 against P1 (dB)
};

class Dpsk {
public:
    Dpsk();
    void reset_learning();
    // x[0..n): samples around the interrogation; p1 = P1's leading edge in
    // sample units (fractional) - at least 2 us of samples before it and
    // 3.5 + P6 + 4.5 us after
    void decode(const kp::cf* x, int n, double p1, int nbits, DpskResult& r);
    // a frame whose bits are known to be right (whole) - or the known part
    // of any strong one (P1, P2, P6 up to its first data chip, added with a
    // lower weight): the response is re-fitted with them
    void learn(const kp::cf* x, int n, double p1, const DpskResult& r, bool whole);
    int learned() const { return learned_; }
    int refits() const { return refits_; }
    const std::vector<std::complex<float>>& response() const { return S_; }   // t = -1 .. 4 us, 1/32 us

private:
    static constexpr double TA = 1.0, TB = 4.0;   // S(t) = 0 before -TA, = 1 after TB (us)
    static constexpr int GRID = 32;               // grid points per us
    static constexpr int J = static_cast<int>((TA + TB) * GRID) + 1;
    std::vector<std::complex<float>> S_, S0_;
    // normal equations of the interior grid values (J - 2), forgetting
    std::vector<std::complex<double>> AA_, Ab_;
    double wsum_ = 0;
    int learned_ = 0, since_ = 0, refits_ = 0;

    std::complex<double> s_at(double t) const;   // S(t), t in us
    // MLSE scratch
    struct Path {
        uint64_t c[2];   // chip signs, bit = 1 -> -1
        double m;
    };
    std::vector<Path> paths_, next_;
    std::vector<std::complex<double>> y_, G_;
    std::vector<int> smp_, kmin_, kmax_;

    void fit_pre(const std::vector<std::complex<double>>& y, const std::vector<double>& tn, double df, double* t6,
                 std::complex<double>* a);
    double mlse(const std::vector<std::complex<double>>& y, const std::vector<double>& tn, double t6, std::complex<double> a6,
                int nbits, uint64_t* chips);
    void add_rows(std::vector<std::complex<double>>& AA, std::vector<std::complex<double>>& Ab, const kp::cf* x, int n, double p1,
                  const DpskResult& r, bool whole, double wt) const;
    bool solve(const std::vector<std::complex<double>>& AA, const std::vector<std::complex<double>>& Ab);
};

}  // namespace ssr
