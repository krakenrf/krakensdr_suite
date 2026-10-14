// ATSC 3.0 (A/322): OFDM 8k / 16k / 32k at 0.384 MHz x (16 + bsr) (6.912 /
// 8.064 / 9.216 MHz for 6 / 7 / 8 MHz channels), twelve guard intervals,
// scattered pilot patterns SP3_2 .. SP32_4. Differences from DVB-T that
// matter here:
//  - frames start with a bootstrap (another numerology) and may carry excess
//    guard samples per symbol (time-aligned frames): the symbols are tracked
//    one by one (ofdm.hpp tracked()),
//  - the pilot reference sequence r_k runs over the RELATIVE carrier index
//    and doesn't depend on the symbol: dy symbols combine into one comb,
//  - the reduced-carrier setting (Cred 0..4) moves relative index 0 by
//    Cred x Cunit / 2 carriers: the carrier-offset search finds the relative
//    numbering as it is; only the reported channel centre needs Cred, taken
//    as the one that puts the centre on the 6 / 7 / 8 MHz raster,
//  - preamble and subframe-boundary symbols carry pilots on k mod Dx = 0
//    with the same r_k: harmless in the combs.
// The scattered pilots must be boosted (boost >= 1; SP3_2 needs >= 2) - the
// pattern phase of each symbol is found from the boosted carriers.

#include "ofdm.hpp"
#include "std/atsc3_tables.hpp"

#include <cmath>
#include <cstdio>

namespace sfn {

namespace {

class Atsc3Std : public OfdmStandard {
public:
    std::string name() const override { return "ATSC 3.0"; }

    std::vector<OfdmMode> modes(double bw_mhz) const override {
        std::vector<OfdmMode> v;
        const struct { double mhz; int bsr; } bws[] = {{6, atsc3_std::BSR_COEFF_6MHZ}, {7, atsc3_std::BSR_COEFF_7MHZ}, {8, atsc3_std::BSR_COEFF_8MHZ}};
        for (const auto& bw : bws) {
            if (bw_mhz > 0 && std::fabs(bw.mhz - bw_mhz) > 0.01) continue;
            for (int fi = 0; fi < 3; fi++) {
                const auto f = static_cast<atsc3_std::Fft>(fi);
                for (int gi = 1; gi <= 12; gi++) {
                    if (!atsc3_std::gi_allowed(f, gi)) continue;
                    OfdmMode m;
                    m.label = std::to_string(atsc3_std::FFT_LEN[fi] / 1024) + "k GI" + std::to_string(gi) + "_" +
                              std::to_string(atsc3_std::GI_SAMPLES[gi]);
                    m.bw_mhz = bw.mhz;
                    m.fs = atsc3_std::bsr_hz(bw.bsr);
                    m.nfft = atsc3_std::FFT_LEN[fi];
                    m.ncp = atsc3_std::GI_SAMPLES[gi];
                    m.kmin = 0;
                    m.kmax = atsc3_std::NOC_MAX[fi] - 1;
                    m.kc = (atsc3_std::NOC_MAX[fi] - 1) / 2;
                    v.push_back(m);
                }
            }
        }
        return v;
    }

    std::vector<PilotPattern> patterns(const OfdmMode& m) const override {
        std::vector<PilotPattern> v;
        const int fi = fft_index(m), gi = gi_index(m);
        if (fi < 0 || gi < 0) return v;
        for (int sp = 0; sp < atsc3_std::SP_COUNT; sp++)
            if (atsc3_std::sp_allowed(static_cast<atsc3_std::Fft>(fi), gi, sp))
                v.push_back({atsc3_std::SP_PATTERNS[sp].name, atsc3_std::SP_PATTERNS[sp].dx, atsc3_std::SP_PATTERNS[sp].dy});
        return v;
    }

    kp::cf pilot_value(const OfdmMode& m, const PilotPattern&, int k) const override {
        if (k < 0 || k > m.kmax) return kp::cf(0, 0);
        return kp::cf(static_cast<float>(atsc3_std::ref_bpsk(k)), 0);
    }

    bool tracked() const override { return true; }
    // excess guard samples per symbol: generous (frame length / symbols)
    int max_excess_T() const override { return 2048; }
    // the symbol-by-symbol tracking leaves ghosts 24-28 dB down (synthetic
    // tests) in one antenna's CIR
    double profile_floor_db() const override { return 22; }

    // the reduced-carrier setting whose centre carrier lands nearest the raster
    double centre_bins(const OfdmMode& m, int k_off, double eps, double rf_hz, double df) const override {
        const int fi = fft_index(m);
        double best = m.kc - k_off + eps, bd = 1e18;
        for (int cred = 0; cred <= atsc3_std::CRED_MAX && fi >= 0; cred++) {
            const double bins = atsc3_std::centre_carrier(static_cast<atsc3_std::Fft>(fi), cred) - k_off + eps;
            if (!std::isfinite(rf_hz)) return bins;
            const double f = rf_hz + bins * df;
            // US / Korea 6 MHz (473 + 6k), Europe 7 / 8 MHz (177.5 + 7k, 474 + 8k)
            const double raster = m.bw_mhz < 6.5 ? 6e6 : m.bw_mhz < 7.5 ? 7e6 : 8e6;
            const double base = m.bw_mhz < 6.5 ? 473e6 : m.bw_mhz < 7.5 ? 177.5e6 : 474e6;
            const double d = std::fabs(std::remainder(f - base, raster));
            if (d < bd) {
                bd = d;
                best = bins;
            }
        }
        return best;
    }

    std::string describe(const OfdmMode& m, const PilotPattern& p) const override {
        char bw[16];
        snprintf(bw, sizeof bw, "%g", m.bw_mhz);
        return m.label + ", " + bw + " MHz" + (p.label.empty() ? "" : ", " + p.label);
    }

private:
    static int fft_index(const OfdmMode& m) {
        for (int fi = 0; fi < 3; fi++)
            if (atsc3_std::FFT_LEN[fi] == m.nfft) return fi;
        return -1;
    }
    static int gi_index(const OfdmMode& m) {
        for (int gi = 1; gi <= 12; gi++)
            if (atsc3_std::GI_SAMPLES[gi] == m.ncp) return gi;
        return -1;
    }
};

}  // namespace

std::unique_ptr<Estimator> make_atsc3(kp::Host& h) { return std::make_unique<OfdmEstimator>(h, std::make_unique<Atsc3Std>()); }

}  // namespace sfn
