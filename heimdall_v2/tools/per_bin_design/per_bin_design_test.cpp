// per_bin_design_test.cpp - checks heimdall's per-bin EQ design (src/dsp/per_bin_wls.cpp) on synthetic
// cross-spectra shaped like the measured ones (docs/dev/calibration.md *Band-edge phase*: a constant, a
// residual-lag slope and a curve rising towards the tuner's IF corner near +0.45 fs, a few degrees):
//   1. the design flattens each channel's phase over |f| <= 0.40 fs (and keeps |H| near 1 there);
//   2. CH0 is a pure delta at DELAY;
//   3. band_phase_deg (logged per channel: the band-average phase the taps leave, pairwise) agrees
//      with an independent computation over every bin;
//   4. a weak noise source whose DC spike stands 25 dB above the rest still gets an EQ (the median gate;
//      the old gate, 5 % of the peak bin, gated every bin then and the EQ silently did nothing);
//   5. it refuses with no noise source and with half the band dead.
// Build and run, from heimdall_v2/:
//   g++ -O3 -std=c++20 -Isrc/dsp -o /tmp/per_bin_design_test tools/per_bin_design/per_bin_design_test.cpp
//       src/dsp/per_bin_wls.cpp          (one command line)
//   /tmp/per_bin_design_test             exit status 0 = every check passed
#include "per_bin_wls.hpp"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

using cd = std::complex<double>;
constexpr int N = 32768, NCH = 5, L = 12, D = L / 2;
constexpr double FB = 0.42, PI = 3.14159265358979323846;

static double freq(int k) { return (k < N / 2 ? k : k - N) / static_cast<double>(N); }

struct Scene {
    std::vector<std::vector<std::complex<float>>> acc;
    std::vector<double> ref;
    std::vector<std::vector<double>> phase;   // [ch][bin] the true phase (rad) of channel ch against CH0
};

// noise-source power: flat to 0.45 fs, a steep roll-off beyond, a DC spike of `dc_db` over the flat level
static Scene make_scene(double dc_db, double dead_above, unsigned seed) {
    Scene s;
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0.0, 1.0);
    s.acc.assign(NCH, std::vector<std::complex<float>>(N));
    s.ref.assign(N, 0.0);
    s.phase.assign(NCH, std::vector<double>(N, 0.0));
    for (int k = 0; k < N; k++) {
        const double f = freq(k), af = std::abs(f);
        double p = af <= 0.45 ? 1.0 : std::pow(10.0, -(af - 0.45) / 0.02);
        if (std::min(k, N - k) <= 2) p *= std::pow(10.0, dc_db / 10.0);
        if (af > dead_above) p *= 1e-9;
        s.ref[k] = p;
    }
    for (int c = 0; c < NCH; c++) {
        const double c0 = c ? 0.2 * c : 0.0, slope = 0.01 * c, curve = c ? (2.0 + 1.5 * c) : 0.0;
        for (int k = 0; k < N; k++) {
            const double f = freq(k);
            // deg: constant + a residual lag + a curve rising towards the +0.45 fs corner + a gentle bow
            const double deg = c0 + slope * 360.0 * f + curve * std::exp((f - 0.45) / 0.04) - 0.5 * c * f * f * 10.0;
            const double ph = deg * PI / 180.0;
            s.phase[c][k] = ph;
            const cd noise = 0.01 * cd(g(rng), g(rng));
            const cd x = s.ref[k] * std::polar(1.0, ph) * (1.0 + noise);
            s.acc[c][k] = std::complex<float>(static_cast<float>(x.real()), static_cast<float>(x.imag()));
        }
    }
    return s;
}

static cd response(const std::vector<cd>& h, double f) {
    cd r(0.0, 0.0);
    for (int n = 0; n < L; n++) r += h[n] * std::polar(1.0, -2.0 * PI * f * (n - D));
    return r;
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %-74s %s\n", what, ok ? "ok" : "FAIL");
        fails += !ok;
    };
    for (int pass = 0; pass < 2; pass++) {
        const double dc_db = pass ? 25.0 : 0.0;
        std::printf("== noise source %s\n", pass ? "weak: its DC spike 25 dB above the rest" : "normal");
        const Scene s = make_scene(dc_db, 1.0, 7 + pass);
        std::vector<std::vector<cd>> taps;
        PerBinWlsInfo info;
        const bool ok = design_per_bin_wls(s.acc, s.ref, NCH, L, FB, taps, &info);
        check(ok, "the design succeeds");
        if (!ok) { std::printf("    refused: %s\n", info.why.c_str()); continue; }
        bool delta = true;
        for (int n = 0; n < L; n++) delta &= taps[0][n] == cd(n == D ? 1.0 : 0.0, 0.0);
        check(delta, "CH0 is a delta at DELAY");
        // the old gate (5 % of the peak bin) on this scene: how many bins it would have kept
        double peak = 0.0;
        for (double v : s.ref) peak = std::max(peak, v);
        int kept_old = 0;
        for (double v : s.ref) kept_old += v > 0.05 * peak;
        std::printf("    groups usable %d/128, in-band %d/%d; the old peak gate would keep %d of %d bins\n",
                    info.usable, info.inband_usable, info.inband, kept_old, N);
        for (int c = 1; c < NCH; c++) {
            double before = 0, after = 0, gmin = 1e9, gmax = 0;
            double mean_before = 0;
            int nb = 0;
            for (int k = 0; k < N; k++) if (std::abs(freq(k)) <= 0.40 && std::min(k, N - k) > 4) { mean_before += s.phase[c][k]; nb++; }
            mean_before /= nb;
            cd full(0.0, 0.0);
            for (int k = 0; k < N; k++) {
                const double f = freq(k);
                const cd hk = response(taps[c], f);
                if (std::min(k, N - k) > 4) full += cd(s.acc[c][k].real(), s.acc[c][k].imag()) * hk;
                if (std::abs(f) > 0.40 || std::min(k, N - k) <= 4) continue;
                before = std::max(before, std::abs(s.phase[c][k] - mean_before) * 180.0 / PI);
                after = std::max(after, std::abs(std::arg(hk * std::polar(1.0, s.phase[c][k]))) * 180.0 / PI);
                gmin = std::min(gmin, std::abs(hk));
                gmax = std::max(gmax, std::abs(hk));
            }
            const double band = std::arg(full) * 180.0 / PI;
            char line[200];
            std::snprintf(line, sizeof line, "CH%d: phase over |f| <= 0.40 fs %.2f -> %.2f deg (< 0.5), |H| %.3f-%.3f", c,
                          before, after, gmin, gmax);
            check(after < 0.5 && gmin > 0.95 && gmax < 1.05, line);
            std::snprintf(line, sizeof line, "CH%d: band_phase_deg %.3f vs every bin %.3f deg (within 0.05)", c,
                          info.band_phase_deg[c], band);
            check(std::abs(info.band_phase_deg[c] - band) < 0.05, line);
        }
    }
    std::printf("== refusals\n");
    {
        Scene s = make_scene(0.0, 1.0, 3);
        for (auto& v : s.ref) v = 0.0;
        std::vector<std::vector<cd>> taps;
        PerBinWlsInfo info;
        const bool ok = design_per_bin_wls(s.acc, s.ref, NCH, L, FB, taps, &info);
        check(!ok && taps.empty(), "no noise source: refused, taps untouched");
        std::printf("    why: %s\n", info.why.c_str());
    }
    {
        const Scene s = make_scene(0.0, 0.2, 4);
        std::vector<std::vector<cd>> taps;
        PerBinWlsInfo info;
        const bool ok = design_per_bin_wls(s.acc, s.ref, NCH, L, FB, taps, &info);
        check(!ok, "half the band dead (nothing above 0.2 fs): refused");
        std::printf("    why: %s\n", info.why.c_str());
    }
    std::printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
