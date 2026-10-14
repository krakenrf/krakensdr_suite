// DVB-T2 (EN 302 755): FFT 1k..32k, seven guard intervals, scattered pilot
// patterns PP1..PP8 (Dx 3..24, Dy 2..16), normal or extended carrier mode,
// channels of 1.7 / 5 / 6 / 7 / 8 MHz. Differences from DVB-T that matter
// here:
//  - every T2 frame (up to 250 ms) starts with a P1 symbol (2048 T: a 1k
//    symbol A with frequency-shifted copies of its parts C before and B
//    after it), which resets the symbol grid - found from its time structure
//    alone (works on any slice): x(n) x*(n + 542 T) over C and x(n) x*(n -
//    482 T) over B, both derotated by the 1 / (1024 T) shift. Frames are
//    contiguous, so the P1s give every symbol before and after them,
//  - the NP2 P2 symbols after P1 carry other pilots: skipped, as is a
//    frame's last symbol (a frame closing symbol in many modes),
//  - pilot value = +-A_SP from w_(k+Kext) XOR pn_l: the frame PN flips the
//    sign of a whole symbol (searched when the Dy symbols of a comb are
//    combined, see ofdm.hpp),
//  - extended carrier mode only adds carriers at the edges with the same
//    pilots: one numbering (normal mode's, extended carriers at k < 0 and
//    k > Kmax) covers both.

#include "ofdm.hpp"
#include "std/dvbt2_tables.hpp"

#include <algorithm>
#include <cmath>

namespace sfn {

namespace {

constexpr double P1_THRESHOLD = 0.4;     // (|C sum| + |B sum|) / energy of a P1 at least
constexpr double P1_SPACING_S = 0.005;   // P1s at least this far apart

class Dvbt2Std : public OfdmStandard {
public:
    std::string name() const override { return "DVB-T2"; }

    std::vector<OfdmMode> modes(double bw_mhz) const override {
        std::vector<OfdmMode> v;
        for (const auto& bwd : dvbt2_std::BANDWIDTHS) {
            if (bwd.mhz > 9) continue;   // 10 MHz: professional only
            if (bw_mhz > 0 && std::fabs(bwd.mhz - bw_mhz) > 0.01) continue;
            for (int fi = 0; fi < dvbt2_std::NUM_FFT; fi++)
                for (int gi = 0; gi < dvbt2_std::NUM_GI; gi++) {
                    const int glen = dvbt2_std::GI_LEN_T[fi][gi];
                    if (glen == 0 || dvbt2_std::SISO_PP_ALLOWED[fi][gi] == 0) continue;
                    OfdmMode m;
                    m.label = std::to_string(dvbt2_std::FFT_K[fi]) + "k GI " + std::to_string(dvbt2_std::GI_NUM[gi]) + "/" +
                              std::to_string(dvbt2_std::GI_DEN[gi]);
                    m.bw_mhz = bwd.mhz;
                    m.fs = dvbt2_std::sample_rate_hz(bwd.mhz);
                    m.nfft = dvbt2_std::FFT_N[fi];
                    m.ncp = glen;
                    m.kmin = -dvbt2_std::KEXT[fi];
                    m.kmax = dvbt2_std::KMAX_NORMAL[fi] + dvbt2_std::KEXT[fi];
                    m.kc = dvbt2_std::KMAX_NORMAL[fi] / 2;
                    v.push_back(m);
                }
        }
        return v;
    }

    std::vector<PilotPattern> patterns(const OfdmMode& m) const override {
        std::vector<PilotPattern> v;
        const int fi = fft_index(m), gi = gi_index(m);
        if (fi < 0 || gi < 0) return v;
        for (int pp = 1; pp <= 8; pp++)
            if (dvbt2_std::SISO_PP_ALLOWED[fi][gi] & dvbt2_std::PPM(pp))
                v.push_back({"PP" + std::to_string(pp), dvbt2_std::DX[pp - 1], dvbt2_std::DY[pp - 1]});
        return v;
    }

    // normal-mode numbering: k mod (Dx Dy) = Dx (l mod Dy), extended carriers included
    bool is_pilot(const OfdmMode& m, const PilotPattern& p, int k, int ph) const override {
        if (k < m.kmin || k > m.kmax) return false;
        const int M = p.dx * p.dy;
        return ((k % M) + M) % M == p.dx * ph;
    }

    kp::cf pilot_value(const OfdmMode& m, const PilotPattern& p, int k) const override {
        const int fi = fft_index(m);
        const int idx = k + dvbt2_std::KEXT[fi];
        const auto& w = dvbt2_std::w_table();
        if (idx < 0 || idx >= static_cast<int>(w.size())) return kp::cf(0, 0);
        int pp = 1;
        for (int q = 1; q <= 8; q++)
            if (dvbt2_std::DX[q - 1] == p.dx && dvbt2_std::DY[q - 1] == p.dy) pp = q;
        const float a = static_cast<float>(dvbt2_std::ASP[pp - 1]);
        return kp::cf(w[static_cast<size_t>(idx)] ? -a : a, 0);
    }

    bool symbol_signs() const override { return true; }
    int symbol_sign(int l) const override { return l >= 0 && dvbt2_std::pn(l) ? -1 : 1; }
    bool framed() const override { return true; }
    int preamble_T() const override { return dvbt2_std::TP1_T; }
    int preamble_symbols(const OfdmMode& m) const override {
        const int fi = fft_index(m);
        return fi < 0 ? 0 : dvbt2_std::NP2[fi];
    }
    bool closing_symbol() const override { return true; }

    std::string describe(const OfdmMode& m, const PilotPattern& p) const override {
        char bw[16];
        snprintf(bw, sizeof bw, "%g", m.bw_mhz);
        return m.label + ", " + bw + " MHz" + (p.label.empty() ? "" : ", " + p.label);
    }

    // P1: (|sum over C of x(n) x*(n + 542 T)| + |sum over B of x(n) x*(n - 482 T)|)
    // / their energy, both derotated by e^(-j 2 pi n / 1024 T)
    std::vector<double> frame_starts(const std::vector<kp::cf>& x, double rate_in, double fs) const override {
        std::vector<double> out;
        const double sc = rate_in / fs;   // samples of x per T
        const long n = static_cast<long>(x.size());
        const long lagC = std::lround(542 * sc), lagB = std::lround(482 * sc);
        const long WC = lagC, WB = lagB, offB = std::lround(1566 * sc), len = std::lround(2048 * sc);
        if (n < 2 * len) return out;
        // prefix sums of the derotated products and energies
        std::vector<double> ur(static_cast<size_t>(n + 1), 0.0), ui(ur.size(), 0.0), ue(ur.size(), 0.0);
        std::vector<double> vr(ur.size(), 0.0), vi(ur.size(), 0.0), ve(ur.size(), 0.0);
        const double w = -2 * M_PI * (fs / 1024) / rate_in;
        const float* xf = reinterpret_cast<const float*>(x.data());
        double cr = 1, ci = 0;   // e^(j w i), renormalised every 1024 samples
        const double sr = std::cos(w), si = std::sin(w);
        for (long i = 0; i < n; i++) {
            if ((i & 1023) == 0) {
                cr = std::cos(w * static_cast<double>(i));
                ci = std::sin(w * static_cast<double>(i));
            }
            const double ar = xf[2 * i], ai = xf[2 * i + 1];
            double pr = 0, pi = 0, pe = 0, qr = 0, qi = 0, qe = 0;
            if (i + lagC < n) {
                const double br = xf[2 * (i + lagC)], bi = xf[2 * (i + lagC) + 1];
                const double tr = ar * br + ai * bi, ti = ai * br - ar * bi;   // x(i) x*(i + lagC)
                pr = tr * cr - ti * ci;
                pi = tr * ci + ti * cr;
                pe = 0.5 * (ar * ar + ai * ai + br * br + bi * bi);
            }
            if (i - lagB >= 0) {
                const double br = xf[2 * (i - lagB)], bi = xf[2 * (i - lagB) + 1];
                const double tr = ar * br + ai * bi, ti = ai * br - ar * bi;   // x(i) x*(i - lagB)
                qr = tr * cr - ti * ci;
                qi = tr * ci + ti * cr;
                qe = 0.5 * (ar * ar + ai * ai + br * br + bi * bi);
            }
            const size_t k = static_cast<size_t>(i);
            ur[k + 1] = ur[k] + pr;
            ui[k + 1] = ui[k] + pi;
            ue[k + 1] = ue[k] + pe;
            vr[k + 1] = vr[k] + qr;
            vi[k + 1] = vi[k] + qi;
            ve[k + 1] = ve[k] + qe;
            const double nr = cr * sr - ci * si;
            ci = cr * si + ci * sr;
            cr = nr;
        }
        std::vector<std::pair<double, long>> cand;
        for (long n0 = 0; n0 + len <= n; n0++) {
            const size_t a = static_cast<size_t>(n0), b = static_cast<size_t>(n0 + WC);
            const size_t c = static_cast<size_t>(n0 + offB), d = static_cast<size_t>(n0 + offB + WB);
            const double e = (ue[b] - ue[a]) + (ve[d] - ve[c]);
            if (e <= 0) continue;
            const double m = (std::hypot(ur[b] - ur[a], ui[b] - ui[a]) + std::hypot(vr[d] - vr[c], vi[d] - vi[c])) / e;
            if (m >= P1_THRESHOLD) cand.push_back({m, n0});
        }
        std::sort(cand.rbegin(), cand.rend());
        const long sep = std::lround(P1_SPACING_S * rate_in);
        for (const auto& [m, n0] : cand) {
            bool near = false;
            for (double f : out)
                if (std::fabs(f - static_cast<double>(n0)) < sep) near = true;
            if (!near) out.push_back(static_cast<double>(n0));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

private:
    static int fft_index(const OfdmMode& m) {
        for (int fi = 0; fi < dvbt2_std::NUM_FFT; fi++)
            if (dvbt2_std::FFT_N[fi] == m.nfft) return fi;
        return -1;
    }
    static int gi_index(const OfdmMode& m) {
        const int fi = fft_index(m);
        if (fi < 0) return -1;
        for (int gi = 0; gi < dvbt2_std::NUM_GI; gi++)
            if (dvbt2_std::GI_LEN_T[fi][gi] == m.ncp) return gi;
        return -1;
    }
};

}  // namespace

std::unique_ptr<Estimator> make_dvbt2(kp::Host& h) { return std::make_unique<OfdmEstimator>(h, std::make_unique<Dvbt2Std>()); }

}  // namespace sfn
