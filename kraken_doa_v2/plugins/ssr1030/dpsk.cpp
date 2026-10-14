#include "dpsk.hpp"

#include "front.hpp"

#include <algorithm>
#include <cmath>

namespace ssr {

namespace {

constexpr double T = 0.25;   // chip (us)
constexpr int L = 5;         // chips in the MLSE state
constexpr int NST = 1 << L;

using cd = std::complex<double>;

// The receiver's step response, measured on live interrogations at 1030 MHz
// (KrakenSDR: R820T + RTL2832 at 2.4 MS/s through heimdall), t = -1 .. 4 us
// in 1/32 us steps; re-fitted at run time from checked frames
const float S_DEFAULT[][2] = {
#include "s_default.h"
};

// complex Cholesky solve of A x = b (A Hermitian positive definite, n x n row-major); false if not PD
bool chol_solve(std::vector<cd>& A, std::vector<cd>& b, int n) {
    for (int j = 0; j < n; j++) {
        double d = A[static_cast<size_t>(j * n + j)].real();
        for (int k = 0; k < j; k++) d -= std::norm(A[static_cast<size_t>(j * n + k)]);
        if (!(d > 0)) return false;
        const double ljj = std::sqrt(d);
        A[static_cast<size_t>(j * n + j)] = ljj;
        for (int i = j + 1; i < n; i++) {
            cd s = A[static_cast<size_t>(i * n + j)];
            for (int k = 0; k < j; k++) s -= A[static_cast<size_t>(i * n + k)] * std::conj(A[static_cast<size_t>(j * n + k)]);
            A[static_cast<size_t>(i * n + j)] = s / ljj;
        }
    }
    for (int i = 0; i < n; i++) {   // L y = b
        cd s = b[static_cast<size_t>(i)];
        for (int k = 0; k < i; k++) s -= A[static_cast<size_t>(i * n + k)] * b[static_cast<size_t>(k)];
        b[static_cast<size_t>(i)] = s / A[static_cast<size_t>(i * n + i)].real();
    }
    for (int i = n - 1; i >= 0; i--) {   // L^H x = y
        cd s = b[static_cast<size_t>(i)];
        for (int k = i + 1; k < n; k++) s -= std::conj(A[static_cast<size_t>(k * n + i)]) * b[static_cast<size_t>(k)];
        b[static_cast<size_t>(i)] = s / A[static_cast<size_t>(i * n + i)].real();
    }
    return true;
}

// 3 x 3 complex least squares via the normal equations (Cramer-free Gauss)
bool solve3(cd M[3][4]) {
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (std::abs(M[r][c]) > std::abs(M[p][c])) p = r;
        if (std::abs(M[p][c]) < 1e-30) return false;
        for (int j = 0; j < 4; j++) std::swap(M[c][j], M[p][j]);
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            const cd f = M[r][c] / M[c][c];
            for (int j = c; j < 4; j++) M[r][j] -= f * M[c][j];
        }
    }
    for (int c = 0; c < 3; c++) M[c][3] /= M[c][c];
    return true;
}

}  // namespace

Dpsk::Dpsk() {
    static_assert(sizeof(S_DEFAULT) / sizeof(S_DEFAULT[0]) == static_cast<size_t>(J), "default response table size");
    S0_.resize(static_cast<size_t>(J));
    for (int j = 0; j < J; j++) S0_[static_cast<size_t>(j)] = {S_DEFAULT[j][0], S_DEFAULT[j][1]};
    S0_[0] = 0;
    S0_[static_cast<size_t>(J - 1)] = 1;
    S_ = S0_;
    reset_learning();
}

void Dpsk::reset_learning() {
    const size_t N = static_cast<size_t>(J - 2);
    AA_.assign(N * N, 0);
    Ab_.assign(N, 0);
    wsum_ = 0;
    learned_ = since_ = 0;
    S_ = S0_;
}

std::complex<double> Dpsk::s_at(double t) const {
    if (t <= -TA) return 0;
    if (t >= TB) return 1;
    const double f = (t + TA) * GRID;
    int j = static_cast<int>(f);
    if (j > J - 2) j = J - 2;
    const double w = f - j;
    const auto& a = S_[static_cast<size_t>(j)];
    const auto& b = S_[static_cast<size_t>(j + 1)];
    return {a.real() + w * (b.real() - a.real()), a.imag() + w * (b.imag() - a.imag())};
}

// P6's start by a grid search; P1 / P2 / P6 amplitudes by least squares over
// the known part (up to just before the first data chip's precursor)
void Dpsk::fit_pre(const std::vector<cd>& y, const std::vector<double>& tn, double df, double* t6_out, cd* a_out) {
    double best = 1e300;
    const size_t n = y.size();
    std::vector<cd> yr(n);
    for (size_t i = 0; i < n; i++) yr[i] = y[i] * std::polar(1.0, -2 * M_PI * df * 1e-6 * tn[i]);
    for (int g = 0; g <= 40; g++) {
        const double t6 = 3.25 + 0.0125 * g, t1 = t6 - 3.5;
        cd M[3][4] = {};
        double yy = 0;
        for (size_t i = 0; i < n; i++) {
            const double t = tn[i];
            if (t <= -1.5) continue;
            if (t >= t6 + 1.45) break;
            const cd c[3] = {s_at(t - t1) - s_at(t - t1 - 0.8), s_at(t - t1 - 2.0) - s_at(t - t1 - 2.8),
                             s_at(t - t6) - 2.0 * s_at(t - t6 - 1.25)};
            for (int p = 0; p < 3; p++) {
                for (int q = 0; q < 3; q++) M[p][q] += std::conj(c[p]) * c[q];
                M[p][3] += std::conj(c[p]) * yr[i];
            }
            yy += std::norm(yr[i]);
        }
        cd W[3][4];
        std::copy(&M[0][0], &M[0][0] + 12, &W[0][0]);
        if (!solve3(W)) continue;
        // residual = yy - a^H (A^H y)
        cd ay = 0;
        for (int p = 0; p < 3; p++) ay += std::conj(W[p][3]) * M[p][3];
        const double r = yy - ay.real();
        if (r < best) {
            best = r;
            *t6_out = t6;
            for (int p = 0; p < 3; p++) a_out[p] = W[p][3];
        }
    }
}

// Per-survivor Viterbi over the chips. y: P6 with P1 / P2 removed (and the
// carrier offset), tn: its times (us, P1 frame). Chips 0-4 = +1, 5-6 = -1
// (the sync reversal), 7 .. 7 + nbits - 1 data, then 2 guard chips = the
// last data chip. Returns the metric; chips[] = the best path's signs
double Dpsk::mlse(const std::vector<cd>& y, const std::vector<double>& tn, double t6, cd a6, int nbits, uint64_t* chips) {
    const int K = nbits + 9;
    smp_.clear();
    kmin_.clear();
    kmax_.clear();
    const size_t n = y.size();
    for (size_t i = 0; i < n; i++) {
        const double x = tn[i] - t6;
        if (x < 1.25 - 0.6 || x > K * T + TB + T) continue;
        int kx = static_cast<int>(std::floor((x + TA) / T));
        if (kx < 7) continue;   // only known chips: the same for every path
        kx = std::min(kx, K - 1);
        const int kn = std::max(0, static_cast<int>(std::ceil((x - TB - T) / T)));
        smp_.push_back(static_cast<int>(i));
        kmin_.push_back(kn);
        kmax_.push_back(kx);
    }
    const size_t ns = smp_.size();
    // G[s][j - kmin] = a6 g(x - j T)
    std::vector<int> off(ns + 1, 0);
    for (size_t s = 0; s < ns; s++) off[s + 1] = off[s] + (kmax_[s] - kmin_[s] + 1);
    G_.resize(static_cast<size_t>(off[ns]));
    for (size_t s = 0; s < ns; s++) {
        const double x = tn[static_cast<size_t>(smp_[s])] - t6;
        for (int j = kmin_[s]; j <= kmax_[s]; j++)
            G_[static_cast<size_t>(off[s] + j - kmin_[s])] = a6 * (s_at(x - j * T) - s_at(x - j * T - T));
    }
    auto bit = [](const Path& p, int k) { return static_cast<int>((p.c[k >> 6] >> (k & 63)) & 1); };
    // the one start path: chips 5, 6 negative
    paths_.assign(1, Path{{(uint64_t(1) << 5) | (uint64_t(1) << 6), 0}, 0.0});
    std::vector<int> pstate(1, 0b00011);   // chips 2..6
    std::vector<int> nstate;
    size_t si = 0;
    int best_of[NST];
    for (int k = 7; k < K; k++) {
        const bool data = k < 7 + nbits;
        next_.clear();
        nstate.clear();
        std::fill(best_of, best_of + NST, -1);
        // the samples of this step
        const size_t s0 = si;
        while (si < ns && kmax_[si] == k) si++;
        for (size_t pi = 0; pi < paths_.size(); pi++) {
            const Path& p = paths_[pi];
            // the prediction from the chips before k (same for both choices)
            cd base[8];
            cd gk[8];
            const size_t cnt = std::min<size_t>(si - s0, 8);
            for (size_t q = 0; q < cnt; q++) {
                const size_t s = s0 + q;
                cd acc = 0;
                const cd* g = &G_[static_cast<size_t>(off[s])];
                for (int j = kmin_[s]; j < k; j++) acc += bit(p, j) ? -g[j - kmin_[s]] : g[j - kmin_[s]];
                base[q] = acc;
                gk[q] = g[k - kmin_[s]];
            }
            const int nchoice = data ? 2 : 1;
            for (int ch = 0; ch < nchoice; ch++) {
                const int b = data ? ch : bit(p, k - 1);
                double m = p.m;
                for (size_t q = 0; q < cnt; q++) {
                    const cd pred = b ? base[q] - gk[q] : base[q] + gk[q];
                    m += std::norm(y[static_cast<size_t>(smp_[s0 + q])] - pred);
                }
                const int st = ((pstate[pi] << 1) | b) & (NST - 1);
                const int cur = best_of[st];
                if (cur >= 0 && next_[static_cast<size_t>(cur)].m <= m) continue;
                Path np = p;
                if (b) np.c[k >> 6] |= uint64_t(1) << (k & 63);
                np.m = m;
                if (cur >= 0) {
                    next_[static_cast<size_t>(cur)] = np;
                } else {
                    best_of[st] = static_cast<int>(next_.size());
                    next_.push_back(np);
                    nstate.push_back(st);
                }
            }
        }
        paths_.swap(next_);
        pstate.swap(nstate);
    }
    size_t bi = 0;
    for (size_t i = 1; i < paths_.size(); i++)
        if (paths_[i].m < paths_[bi].m) bi = i;
    chips[0] = paths_[bi].c[0];
    chips[1] = paths_[bi].c[1];
    return paths_[bi].m;
}

void Dpsk::decode(const kp::cf* x, int n, double p1, int nbits, DpskResult& r) {
    const int K = nbits + 9;
    y_.resize(static_cast<size_t>(n));
    std::vector<double> tn(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
        y_[static_cast<size_t>(i)] = cd(x[i].real(), x[i].imag());
        tn[static_cast<size_t>(i)] = (i - p1) * US;
    }
    double df = 0, best = 1e300;
    std::vector<cd> yd(static_cast<size_t>(n));
    for (int pass = 0; pass < 2; pass++) {
        double t6 = 3.5;
        cd a[3] = {};
        fit_pre(y_, tn, df, &t6, a);
        const double t1 = t6 - 3.5;
        for (int i = 0; i < n; i++) {
            const double t = tn[static_cast<size_t>(i)];
            yd[static_cast<size_t>(i)] = y_[static_cast<size_t>(i)] * std::polar(1.0, -2 * M_PI * df * 1e-6 * t) -
                                         a[0] * (s_at(t - t1) - s_at(t - t1 - 0.8)) - a[1] * (s_at(t - t1 - 2.0) - s_at(t - t1 - 2.8));
        }
        uint64_t ch[2] = {};
        mlse(yd, tn, t6, a[2], nbits, ch);
        auto c = [&](int k) { return k < 0 || k >= K ? 0.0 : (((ch[k >> 6] >> (k & 63)) & 1) ? -1.0 : 1.0); };
        // the whole model -> residual, and the carrier offset still in it
        double err = 0, sig = 0, sw = 0, st = 0, sp = 0, stt = 0, stp = 0;
        cd zsum = 0;
        std::vector<std::pair<double, cd>> zs;
        for (int i = 0; i < n; i++) {
            const double t = tn[static_cast<size_t>(i)];
            if (t < t1 - 1.0 || t > t6 + K * T + 1.5) continue;
            cd m = a[0] * (s_at(t - t1) - s_at(t - t1 - 0.8)) + a[1] * (s_at(t - t1 - 2.0) - s_at(t - t1 - 2.8));
            const int j0 = std::max(0, static_cast<int>(std::ceil((t - t6 - TB - T) / T)));
            const int j1 = std::min(K - 1, static_cast<int>(std::floor((t - t6 + TA) / T)));
            for (int j = j0; j <= j1; j++) m += a[2] * c(j) * (s_at(t - t6 - j * T) - s_at(t - t6 - j * T - T));
            const cd yr = y_[static_cast<size_t>(i)] * std::polar(1.0, -2 * M_PI * df * 1e-6 * t);
            err += std::norm(yr - m);
            sig += std::norm(m);
            const cd z = yr * std::conj(m);
            zs.push_back({t, z});
            zsum += z;
        }
        const double rel = sig > 0 ? err / sig : 1;
        if (rel < best) {
            best = rel;
            r.nbits = nbits;
            for (int i = 0; i < nbits; i++) r.bits[i] = c(7 + i) != c(6 + i) ? 1 : 0;
            r.t1 = t1;
            r.t6 = t6;
            r.df = df;
            r.a1 = a[0];
            r.a2 = a[1];
            r.a6 = a[2];
            r.rel_err = rel;
            r.p2_db = 20 * std::log10(std::max(std::abs(a[1]), 1e-9) / std::max(std::abs(a[0]), 1e-9));
        }
        if (pass == 1) break;
        // phase drift: the weighted slope of arg(z / mean z) over time
        double wmax = 0;
        for (auto& [t, z] : zs) wmax = std::max(wmax, std::abs(z));
        for (auto& [t, z] : zs) {
            const double w = std::abs(z);
            if (w < 0.25 * wmax) continue;
            const double ph = std::arg(z * std::conj(zsum));
            sw += w;
            st += w * t;
            sp += w * ph;
            stt += w * t * t;
            stp += w * t * ph;
        }
        const double var = sw > 0 ? stt / sw - (st / sw) * (st / sw) : 0;
        if (!(var > 1e-6)) break;
        const double slope = (stp / sw - (st / sw) * (sp / sw)) / var;   // rad / us
        df += slope / (2 * M_PI) * 1e6;
        if (!std::isfinite(df) || std::fabs(df) > 50e3) break;
    }
}

// The frame as edges of the transmitted envelope (P1, P2, P6 with its
// reversals - or only up to its first data chip), every sample a linear
// function of S's grid values: rows of the normal equations (a frame's
// amplitude counts as 1)
void Dpsk::add_rows(std::vector<cd>& AA, std::vector<cd>& Ab, const kp::cf* x, int n, double p1, const DpskResult& r, bool whole,
                    double wt) const {
    const int K = r.nbits + 9;
    const int N = J - 2;
    const double a6m = std::abs(r.a6);
    if (!(a6m > 0)) return;
    std::vector<std::pair<double, cd>> edges = {{r.t1, r.a1}, {r.t1 + 0.8, -r.a1}, {r.t1 + 2.0, r.a2}, {r.t1 + 2.8, -r.a2}};
    edges.push_back({r.t6, r.a6});                  // P6 on, chips 0-4 = +1
    edges.push_back({r.t6 + 1.25, -2.0 * r.a6});    // the sync phase reversal: chips 5, 6 = -1
    double prev = -1;
    for (int k = 7; whole && k < 7 + r.nbits; k++) {
        const double c = r.bits[k - 7] ? -prev : prev;
        if (c != prev) edges.push_back({r.t6 + k * T, r.a6 * (c - prev)});
        prev = c;
    }
    if (whole) edges.push_back({r.t6 + K * T, -prev * r.a6});   // the 2 guard chips keep the last sign, then off
    // the known part only: up to just before the first data chip reaches the samples
    const double t_end = whole ? r.t6 + K * T + TB : r.t6 + 1.45;
    std::vector<std::pair<int, cd>> row;
    for (int i = 0; i < n; i++) {
        const double t = (i - p1) * US;
        if (t < r.t1 - 1.5 || t > t_end) continue;
        cd yv = cd(x[i].real(), x[i].imag()) * std::polar(1.0, -2 * M_PI * r.df * 1e-6 * t);
        row.clear();
        for (auto& [u, e] : edges) {
            const double xx = t - u;
            if (xx <= -TA) continue;
            if (xx >= TB) {
                yv -= e;
                continue;
            }
            const double f = (xx + TA) * GRID;
            int j = std::min(static_cast<int>(f), J - 2);
            const double w1 = f - j;
            const std::pair<int, double> wj[2] = {{j, 1 - w1}, {j + 1, w1}};
            for (auto [jj, w] : wj) {
                if (jj == 0) continue;
                if (jj == J - 1) {
                    yv -= e * w;
                    continue;
                }
                bool found = false;
                for (auto& [c, v] : row)
                    if (c == jj - 1) {
                        v += e * w;
                        found = true;
                        break;
                    }
                if (!found) row.push_back({jj - 1, e * w});
            }
        }
        const double sc = std::sqrt(wt) / a6m;
        for (auto& [pp, vp] : row) {
            const cd cp = std::conj(vp) * sc;
            for (auto& [q, vq] : row) AA[static_cast<size_t>(pp * N + q)] += cp * vq * sc;
            Ab[static_cast<size_t>(pp)] += cp * yv * sc;
        }
    }
}

void Dpsk::learn(const kp::cf* x, int n, double p1, const DpskResult& r, bool whole) {
    const double lam = 1.0 - 1.0 / 300;
    for (auto& v : AA_) v *= lam;
    for (auto& v : Ab_) v *= lam;
    add_rows(AA_, Ab_, x, n, p1, r, whole, whole ? 1.0 : 0.3);
    // re-fitted only with checked frames in: the known parts alone don't pin
    // the response down well enough (tried on synthetic receivers unlike the
    // table: the fit moved ~15 % of the way and decoded nothing more), so a
    // site without all-calls / broadcasts keeps the measured table
    if (!whole) return;
    learned_++;
    if (++since_ >= (refits_ ? 10 : 4)) {
        since_ = 0;
        if (solve(AA_, Ab_)) refits_++;
    }
}

// least squares for S's grid values + smoothness (second differences) + a
// weak pull towards the measured table
bool Dpsk::solve(const std::vector<cd>& AA, const std::vector<cd>& Ab) {
    const int N = J - 2;
    double tr = 0;
    for (int i = 0; i < N; i++) tr += AA[static_cast<size_t>(i * N + i)].real();
    if (!(tr > 0)) return false;
    const double lam = 0.05 * tr / N, mu = 0.004 * tr / N;
    std::vector<cd> A = AA, b = Ab;
    for (int i = 0; i + 2 < N; i++) {
        const int c[3] = {i, i + 1, i + 2};
        const double w[3] = {1, -2, 1};
        for (int p = 0; p < 3; p++)
            for (int q = 0; q < 3; q++) A[static_cast<size_t>(c[p] * N + c[q])] += lam * w[p] * w[q];
    }
    for (int i = 0; i < N; i++) {
        A[static_cast<size_t>(i * N + i)] += mu;
        b[static_cast<size_t>(i)] += mu * cd(S0_[static_cast<size_t>(i + 1)]);
    }
    if (!chol_solve(A, b, N)) return false;
    for (int i = 0; i < N; i++)
        if (!std::isfinite(b[static_cast<size_t>(i)].real()) || std::abs(b[static_cast<size_t>(i)]) > 3) return false;
    for (int i = 0; i < N; i++) S_[static_cast<size_t>(i + 1)] = std::complex<float>(b[static_cast<size_t>(i)]);
    return true;
}

}  // namespace ssr
