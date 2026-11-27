#include "per_bin_wls.hpp"
#include <algorithm>
#include <cmath>

// The design measured on branch eq-fast (2026-11-26; tuned on two recorded
// calibrations, then applied unchanged to seven others; hardware A/B in
// docs/dev/calibration.md *A faster per-bin EQ*). Double precision, a
// hand-written Cholesky:
//   K = 128 groups of B = N/K bins, group k centred on bin k*B; bins within
//   DCX of DC are left out of every sum (CH0's DC spike and its zero-padding
//   leakage into the odd bins +-1, +-3);
//   gate: group k usable if its CH0 power R_k > GATE x the median of R_k over
//   0 < |f_k| <= 0.3 (a median, not a fraction of the PEAK bin, which a weak
//   noise source's DC spike sets: then every bin was gated off and the old
//   design silently corrected nothing);
//   per channel c >= 1, S_k = sum of acc[c] over group k: in-band (|f_k| <= fb,
//   usable, k != 0, |S_k| > 0) weight 1 and target conj(S_k) / |S_k|, the DC
//   group weight 0, every other group weight EPS and target 1;
//   minimise sum_k w_k |sum_n h[n] e^{-j 2 pi f_k (n - D)} - T_k|^2
//            + LAM sum_k w_k |h - delta_D|^2,
//   normal equations G h = b (G Hermitian Toeplitz, positive definite), solved
//   by Cholesky G = C C^H.
namespace {
constexpr int K = 128;
constexpr int DCX = 4;
constexpr double GATE = 0.1;
constexpr double EPS = 1e-3;
constexpr double LAM = 1e-4;
constexpr double TWO_PI = 6.283185307179586476925286766559;
}

bool design_per_bin_wls(const std::vector<std::vector<std::complex<float>>>& acc, const std::vector<double>& ref,
                        int nch, int L, double fb, std::vector<std::vector<std::complex<double>>>& taps,
                        PerBinWlsInfo* info) {
    using cd = std::complex<double>;
    PerBinWlsInfo local;
    PerBinWlsInfo& inf = info ? *info : local;
    const int N = static_cast<int>(ref.size());
    if (nch < 1 || L < 2 || N < K || N % K != 0 || static_cast<int>(acc.size()) < nch) {
        inf.why = "bad input sizes";
        return false;
    }
    for (int c = 0; c < nch; c++)
        if (static_cast<int>(acc[c].size()) != N) { inf.why = "bad input sizes"; return false; }
    const int D = L / 2;
    const int B = N / K;
    auto bin_of = [&](int k, int jj) { return ((k * B + jj) % N + N) % N; };
    auto near_dc = [&](int i) { return std::min(i, N - i) <= DCX; };

    // 1. coarse CH0 power and the group frequencies
    std::vector<double> Rg(K), fk(K);
    for (int k = 0; k < K; k++) {
        fk[k] = static_cast<double>(k < K / 2 ? k : k - K) / K;
        double s = 0.0;
        for (int jj = -B / 2; jj < B / 2; jj++) {
            const int i = bin_of(k, jj);
            if (near_dc(i)) continue;
            s += ref[i];
        }
        Rg[k] = s;
    }
    // 2. the median over 0 < |f_k| <= 0.3 (an even count: the mean of the two middle values)
    std::vector<double> vals;
    for (int k = 0; k < K; k++)
        if (k != 0 && std::abs(fk[k]) <= 0.3) vals.push_back(Rg[k]);
    std::sort(vals.begin(), vals.end());
    const size_t m = vals.size();
    const double med = (m % 2) ? vals[m / 2] : 0.5 * (vals[m / 2 - 1] + vals[m / 2]);
    inf.median = med;
    if (!(med > 0.0)) { inf.why = "median CH0 power is 0"; return false; }
    std::vector<char> usable(K);
    inf.usable = inf.inband = inf.inband_usable = 0;
    for (int k = 0; k < K; k++) {
        usable[k] = Rg[k] > GATE * med;
        inf.usable += usable[k];
        if (k != 0 && std::abs(fk[k]) <= fb) {
            inf.inband++;
            inf.inband_usable += usable[k];
        }
    }
    if (2 * inf.inband_usable < inf.inband) { inf.why = "fewer than half of the in-band groups pass the gate"; return false; }

    std::vector<std::vector<cd>> out(nch, std::vector<cd>(L, cd(0.0, 0.0)));
    std::vector<double> band_phase(nch, 0.0);
    out[0][D] = cd(1.0, 0.0);
    std::vector<double> w(K);
    std::vector<cd> S(K), T(K), g(L), b(L), y(L), h(L);
    std::vector<cd> G(static_cast<size_t>(L) * L), C(static_cast<size_t>(L) * L);
    for (int c = 1; c < nch; c++) {
        // 3. coarse cross-spectrum, weights, targets
        double wsum = 0.0;
        for (int k = 0; k < K; k++) {
            cd s(0.0, 0.0);
            for (int jj = -B / 2; jj < B / 2; jj++) {
                const int i = bin_of(k, jj);
                if (near_dc(i)) continue;
                s += cd(acc[c][i].real(), acc[c][i].imag());
            }
            S[k] = s;
            const double a = std::abs(s);
            if (k != 0 && std::abs(fk[k]) <= fb && usable[k] && a > 0.0) {
                w[k] = 1.0;
                T[k] = std::conj(s) / a;
            } else {
                w[k] = (k == 0) ? 0.0 : EPS;
                T[k] = cd(1.0, 0.0);
            }
            wsum += w[k];
        }
        const double lp = LAM * wsum;
        // 4. the Toeplitz first column g[d] and the right-hand side b[r]
        for (int d = 0; d < L; d++) {
            cd s(0.0, 0.0);
            for (int k = 0; k < K; k++) s += w[k] * std::polar(1.0, TWO_PI * fk[k] * d);
            g[d] = s;
        }
        for (int r = 0; r < L; r++) {
            for (int q = 0; q < L; q++) G[r * L + q] = (r >= q) ? g[r - q] : std::conj(g[q - r]);
            G[r * L + r] += lp;
        }
        for (int r = 0; r < L; r++) {
            cd s(0.0, 0.0);
            for (int k = 0; k < K; k++) s += w[k] * T[k] * std::polar(1.0, TWO_PI * fk[k] * (r - D));
            b[r] = s;
        }
        b[D] += lp;
        // 5. Cholesky G = C C^H (C lower triangular), then C y = b, C^H h = y
        std::fill(C.begin(), C.end(), cd(0.0, 0.0));
        for (int r = 0; r < L; r++) {
            for (int q = 0; q <= r; q++) {
                cd s = G[r * L + q];
                for (int p = 0; p < q; p++) s -= C[r * L + p] * std::conj(C[q * L + p]);
                if (r == q) {
                    if (!(s.real() > 0.0)) { inf.why = "non-positive Cholesky pivot"; return false; }
                    C[r * L + r] = cd(std::sqrt(s.real()), 0.0);
                } else {
                    C[r * L + q] = s / C[q * L + q].real();
                }
            }
        }
        for (int r = 0; r < L; r++) {
            cd s = b[r];
            for (int p = 0; p < r; p++) s -= C[r * L + p] * y[p];
            y[r] = s / C[r * L + r].real();
        }
        for (int r = L - 1; r >= 0; r--) {
            cd s = y[r];
            for (int p = r + 1; p < L; p++) s -= std::conj(C[p * L + r]) * h[p];
            h[r] = s / C[r * L + r].real();
        }
        for (int n = 0; n < L; n++) {
            // heimdall builds without -ffinite-math-only, so std::isfinite is a real test
            if (!std::isfinite(h[n].real()) || !std::isfinite(h[n].imag())) { inf.why = "non-finite tap"; return false; }
            out[c][n] = h[n];
        }
        // 6. the band-average phase the taps leave: arg sum_k S_k H(f_k) over the
        //    whole band (the lag-0 cross-correlation with CH0 of the equalized
        //    stream; CH0's delay D cancels against the response's)
        cd r0(0.0, 0.0);
        for (int k = 0; k < K; k++) {
            cd hk(0.0, 0.0);
            for (int n = 0; n < L; n++) hk += h[n] * std::polar(1.0, -TWO_PI * fk[k] * (n - D));
            r0 += S[k] * hk;
        }
        band_phase[c] = (std::abs(r0) > 0.0) ? std::arg(r0) * 180.0 / 3.14159265358979323846 : 0.0;
    }
    taps.swap(out);
    inf.band_phase_deg.swap(band_phase);
    return true;
}
