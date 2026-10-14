#include "search.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace cells {

namespace {

constexpr int FOLD_FFT = 1024;          // overlap-save block of the PSS correlation
constexpr double MIN_SEP_SAMPLES = 12;  // PSS peaks closer than this are one cell's (multipath, sidelobes)
constexpr double COTIMED_SAMPLES = 24;  // cells this close in timing share the SSS windows: a found one is cancelled at the others
constexpr int CANCEL_SMOOTH = 5;        // subcarriers each side: the detected cell's channel in the cancellation
constexpr int PSS_SMOOTH = 4;           // subcarriers each side: the PSS channel estimate
constexpr int IMAGE_EXEMPT_SAMPLES = 4;  // another N_ID2's peak this close: judged by the SSS, not as an image
constexpr double IMAGE_MARGIN = 3;      // a PSS peak must be this far above the images stronger peaks leave there

double median_of(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t h = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(h), v.end());
    return v[h];
}

// the time-domain PSS (one useful symbol, N samples) of a sync sequence on
// the visible subcarriers
std::vector<cf> pss_template(const std::vector<cd>& d, const std::vector<int>& f, const std::vector<int>& vis, int N) {
    std::vector<cf> t(static_cast<size_t>(N));
    for (int i = 0; i < N; i++) {
        cd s = 0;
        for (int vi : vis) s += d[static_cast<size_t>(vi)] * std::polar(1.0, 2 * M_PI * f[static_cast<size_t>(vi)] * i / N);
        t[static_cast<size_t>(i)] = cf(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    }
    return t;
}

// how far the best hypothesis stands out from all of them (median / MAD):
// the score's own scale assumes white noise, but the other cells' sync
// signals make the hypotheses' spread wider than that
double robust_z(const std::vector<double>& v, double best) {
    if (v.size() < 8) return 0;
    const double med = median_of(v);
    std::vector<double> dev(v.size());
    for (size_t i = 0; i < v.size(); i++) dev[i] = std::fabs(v[i] - med);
    const double mad = median_of(dev);
    return mad > 0 ? (best - med) / (1.4826 * mad) : 0;
}

struct Peak {
    int u;
    int pos;
    double v;
};

}  // namespace

double cp_offset(const std::vector<const cf*>& x, size_t n, const Numerology& num, double* strength) {
    const int N = num.nfft();
    const int L = std::max(2, static_cast<int>(std::floor(num.cp())));
    const int P = static_cast<int>(std::lround(num.half_ms()));
    *strength = 0;
    if (x.empty() || n < static_cast<size_t>(4 * P + N + L)) return 0;
    std::vector<cd> G(static_cast<size_t>(P), cd(0, 0));
    double pw = 0;
    for (const cf* a : x) {
        cd run = 0;
        for (size_t i = 0; i + static_cast<size_t>(N) < n; i++) {
            run += cd(a[i + static_cast<size_t>(N)]) * std::conj(cd(a[i]));
            if (i >= static_cast<size_t>(L)) {
                const size_t o = i - static_cast<size_t>(L);
                run -= cd(a[o + static_cast<size_t>(N)]) * std::conj(cd(a[o]));
            }
            if (i + 1 >= static_cast<size_t>(L)) G[(i + 1 - static_cast<size_t>(L)) % static_cast<size_t>(P)] += run;
            pw += std::norm(a[i]);
        }
    }
    cd mean = 0;
    for (const cd& g : G) mean += g;
    mean /= static_cast<double>(P);
    double mx = 0;
    for (auto& g : G) {
        g -= mean;
        mx = std::max(mx, std::abs(g));
    }
    if (!(mx > 0)) return 0;
    cd sum = 0;
    for (const cd& g : G)
        if (std::abs(g) > 0.5 * mx) sum += g;
    // coherence: the CP window's correlation at the strongest position
    // against the window's power (pw counts every sample of every antenna)
    const double per_window = pw / static_cast<double>(n) * L * (static_cast<double>(n) / P);
    *strength = per_window > 0 ? mx / per_window : 0;
    return std::arg(sum) * num.scs() / (2 * M_PI);
}

SearchOut search(const std::vector<const cf*>& x, size_t n, const SearchParams& p) {
    SearchOut out;
    const Numerology& num = p.num;
    const int N = num.nfft();
    const double scs = num.scs();
    const bool lte = p.tech == Tech::LTE;
    const int P = static_cast<int>(std::lround((lte ? 5e-3 : 20e-3) * num.fs));
    out.period = P;
    out.rate = num.fs;
    if (x.empty() || n < static_cast<size_t>(2 * P + 4 * N)) return out;
    const int M = static_cast<int>(x.size());
    const int A = std::max(1, std::min(p.fold_ants, M));

    // --- the sync sequences on the visible subcarriers ---
    const int nseq = lte ? 62 : 127;
    std::vector<int> f(static_cast<size_t>(nseq));
    for (int i = 0; i < nseq; i++) f[static_cast<size_t>(i)] = lte ? lte::sync_f(i) : nr::sync_f(i);
    std::vector<int> vis;
    for (int i = 0; i < nseq; i++)
        if (visible(f[static_cast<size_t>(i)], scs, p.carrier_off, p.vis_hz) && std::abs(f[static_cast<size_t>(i)]) < N / 2 &&
            (p.band_hz <= 0 || std::fabs(f[static_cast<size_t>(i)] * scs) <= p.band_hz))
            vis.push_back(i);
    if (static_cast<int>(vis.size()) < nseq / 2) return out;   // most of the sync signal outside the VFO
    std::vector<std::vector<cd>> dpss(3);
    for (int u = 0; u < 3; u++) {
        if (lte) {
            dpss[static_cast<size_t>(u)] = lte::pss(u);
        } else {
            const auto d = nr::pss(u);
            dpss[static_cast<size_t>(u)].assign(d.begin(), d.end());
        }
    }

    // --- PSS fold ---
    const double tf0 = now_s();
    // the block must be well longer than the template (one symbol, N samples):
    // FOLD_FFT up to N = 256 (every rate the decimated search uses), else 4 N
    int LB = FOLD_FFT;
    while (LB < 4 * N) LB *= 2;
    const int H = LB - N + 1;
    Fft fwd(LB, -1), inv(LB, +1);
    std::vector<std::vector<cf>> Tc(3, std::vector<cf>(static_cast<size_t>(LB)));
    std::vector<std::vector<cf>> tmpl(3);
    for (int u = 0; u < 3; u++) {
        const auto& t = tmpl[static_cast<size_t>(u)] = pss_template(dpss[static_cast<size_t>(u)], f, vis, N);
        std::fill(fwd.data(), fwd.data() + LB, cf(0, 0));
        std::copy(t.begin(), t.end(), fwd.data());
        fwd.run();
        for (int k = 0; k < LB; k++) Tc[static_cast<size_t>(u)][static_cast<size_t>(k)] = std::conj(fwd.data()[k]) / static_cast<float>(LB);
    }
    std::vector<std::vector<double>> fold(3, std::vector<double>(static_cast<size_t>(P), 0.0));
    std::vector<int> cnt(static_cast<size_t>(P), 0);
    std::vector<cf> X(static_cast<size_t>(LB));
    for (int a = 0; a < A; a++)
        for (size_t b0 = 0; b0 + static_cast<size_t>(N) <= n; b0 += static_cast<size_t>(H)) {
            const size_t take = std::min(static_cast<size_t>(LB), n - b0);
            std::copy(x[static_cast<size_t>(a)] + b0, x[static_cast<size_t>(a)] + b0 + take, fwd.data());
            std::fill(fwd.data() + take, fwd.data() + LB, cf(0, 0));
            fwd.run();
            std::copy(fwd.data(), fwd.data() + LB, X.begin());
            for (int u = 0; u < 3; u++) {
                const cf* T = Tc[static_cast<size_t>(u)].data();
                cf* Z = inv.data();
                for (int k = 0; k < LB; k++) Z[k] = X[static_cast<size_t>(k)] * T[k];
                inv.run();
                auto& fu = fold[static_cast<size_t>(u)];
                for (int i = 0; i < H; i++) {
                    const size_t pos = b0 + static_cast<size_t>(i);
                    if (pos + static_cast<size_t>(N) > n) break;
                    fu[pos % static_cast<size_t>(P)] += std::norm(inv.data()[i]);
                    if (a == 0 && u == 0) cnt[pos % static_cast<size_t>(P)]++;
                }
            }
        }
    std::vector<double> med(3);
    for (int u = 0; u < 3; u++) {
        auto& fu = fold[static_cast<size_t>(u)];
        for (int i = 0; i < P; i++) fu[static_cast<size_t>(i)] /= std::max(1, cnt[static_cast<size_t>(i)]) * A;
        med[static_cast<size_t>(u)] = std::max(median_of(fu), 1e-30);
    }
    out.fold.assign(static_cast<size_t>(P), 0.0f);
    for (int i = 0; i < P; i++) {
        double r = 0;
        for (int u = 0; u < 3; u++) r = std::max(r, fold[static_cast<size_t>(u)][static_cast<size_t>(i)] / med[static_cast<size_t>(u)]);
        out.fold[static_cast<size_t>(i)] = static_cast<float>(r);
    }
    const double tf1 = now_s();
    prof().fold += tf1 - tf0;
    // --- peaks ---
    // A strong cell's PSS shows up in every N_ID2's fold around its timing:
    // the three PSS correlate with each other (LTE: peaks ~9 dB below the
    // cell, every ~23 samples) and with themselves off the main peak. Each
    // candidate is judged against the images the stronger accepted peaks are
    // expected to leave there - X[u'][u][d]: the PSS of N_ID2 u' as received
    // (cyclic prefix + symbol) correlated with the template of u at lag d -,
    // so that the SSS tests go to cells, not images.
    const int cpi = static_cast<int>(std::lround(num.cp()));
    const int D = N + cpi;
    double E = 0;
    for (const auto& v : tmpl[0]) E += std::norm(v);
    std::vector<float> X3(static_cast<size_t>(9 * (2 * D + 1)));
    for (int us = 0; us < 3; us++)
        for (int ut = 0; ut < 3; ut++)
            for (int d = -D; d <= D; d++) {
                cd c = 0;
                for (int i = 0; i < N; i++) {
                    const int m2 = i + d;   // sample of the received PSS symbol, useful part 0..N-1, prefix -cpi..-1
                    if (m2 < -cpi || m2 >= N) continue;
                    c += cd(tmpl[static_cast<size_t>(us)][static_cast<size_t>((m2 + N) % N)]) * std::conj(cd(tmpl[static_cast<size_t>(ut)][static_cast<size_t>(i)]));
                }
                X3[static_cast<size_t>((us * 3 + ut) * (2 * D + 1) + (d + D))] = static_cast<float>(std::norm(c) / (E * E));
            }
    auto image = [&](int us, int ut, int d) {
        d = static_cast<int>(std::remainder(static_cast<double>(d), static_cast<double>(P)));
        return std::abs(d) > D ? 0.0 : static_cast<double>(X3[static_cast<size_t>((us * 3 + ut) * (2 * D + 1) + (d + D))]);
    };
    // candidates: local maxima above the Gamma(J A) background (J
    // occurrences per position, A antennas)
    const double J = static_cast<double>(n) / P;
    const double thr_rel = std::max(1.25, 1 + 8 / std::sqrt(J * A));
    std::vector<Peak> cand;
    for (int u = 0; u < 3; u++) {
        const auto& fu = fold[static_cast<size_t>(u)];
        const double thr = med[static_cast<size_t>(u)] * thr_rel;
        for (int i = 0; i < P; i++) {
            const double v = fu[static_cast<size_t>(i)];
            if (v < thr) continue;
            const double l = fu[static_cast<size_t>((i + P - 1) % P)], r = fu[static_cast<size_t>((i + 1) % P)];
            if (v >= l && v > r) cand.push_back({u, i, v});
        }
    }
    std::sort(cand.begin(), cand.end(), [](const Peak& a, const Peak& b) { return a.v > b.v; });
    // Peaks are taken strongest first while the SSS tests run (below): each
    // candidate is judged against the images of the peaks taken before it
    // (X3) and of the SSS symbols of the cells found so far - a strong cell's
    // SSS correlates with the PSS templates too, and its sequence is known
    // once the cell is found.
    std::vector<Peak> peaks;
    int per_root[3] = {0, 0, 0};
    struct SssSrc {
        double pos, f;                      // its SSS symbol's useful start, its level in the fold
        std::vector<std::vector<cd>> S;     // the SSS on the visible subcarriers, per variant (LTE: subframe 0 / 5)
    };
    std::vector<SssSrc> sss_src;
    const int nvs = static_cast<int>(vis.size());
    // the symbol s(t) = sum_q S_q e^{j 2 pi f_q t / N} (t in -cp..N: the
    // prefix is the periodic extension) at lag d against template u:
    // sum_q S_q e^{j 2 pi f_q d / N} sum_i e^{j 2 pi f_q i / N} conj(t_u(i)) over
    // the overlapping i - prefix sums PS[u][q][i] of the inner terms
    std::vector<cd> PS(static_cast<size_t>(3 * nvs * (N + 1)));
    for (int u = 0; u < 3; u++)
        for (int q = 0; q < nvs; q++) {
            cd acc = 0;
            const size_t o = static_cast<size_t>((u * nvs + q) * (N + 1));
            PS[o] = 0;
            for (int i = 0; i < N; i++) {
                acc += std::polar(1.0, 2 * M_PI * f[static_cast<size_t>(vis[static_cast<size_t>(q)])] * i / N) *
                       std::conj(cd(tmpl[static_cast<size_t>(u)][static_cast<size_t>(i)]));
                PS[o + static_cast<size_t>(i + 1)] = acc;
            }
        }
    auto sss_image = [&](const SssSrc& sc, int u, int pos) {
        const double d = std::remainder(pos - sc.pos, static_cast<double>(P));
        if (std::fabs(d) > D) return 0.0;
        const int i0 = std::clamp(static_cast<int>(std::ceil(-num.cp() - d)), 0, N), i1 = std::clamp(static_cast<int>(std::ceil(N - d)), 0, N);
        if (i1 <= i0) return 0.0;
        double sum = 0;
        for (const auto& Sv : sc.S) {
            cd c = 0;
            for (int q = 0; q < nvs; q++) {
                const size_t o = static_cast<size_t>((u * nvs + q) * (N + 1));
                c += Sv[static_cast<size_t>(q)] * std::polar(1.0, 2 * M_PI * f[static_cast<size_t>(vis[static_cast<size_t>(q)])] * d / N) *
                     (PS[o + static_cast<size_t>(i1)] - PS[o + static_cast<size_t>(i0)]);
            }
            sum += std::norm(c) / (E * E);
        }
        return sum / static_cast<double>(sc.S.size());
    };
    auto accept = [&](const Peak& pk) {
        if (per_root[pk.u] >= p.max_peaks) return false;
        double img = 0;
        for (const auto& q : peaks) {
            const int d = std::abs(q.pos - pk.pos);
            if (q.u == pk.u && std::min(d, P - d) < MIN_SEP_SAMPLES) return false;
            // another N_ID2 at the same timing: a cell there (synchronised
            // network) and the image look the same here - the SSS test decides
            if (q.u != pk.u && std::min(d, P - d) <= IMAGE_EXEMPT_SAMPLES) continue;
            img += q.v * image(q.u, pk.u, pk.pos - q.pos);
        }
        for (const auto& sc : sss_src) img += sc.f * sss_image(sc, pk.u, pk.pos);
        if (pk.v - med[static_cast<size_t>(pk.u)] < IMAGE_MARGIN * img) return false;
        peaks.push_back(pk);
        per_root[pk.u]++;
        return true;
    };

    // --- SSS per PSS peak ---
    const double ts0 = now_s();
    const int K = N / 2 - 1;
    SymFft sym(N, K);
    const int nv = static_cast<int>(vis.size());
    std::vector<std::vector<cf>> Yp(static_cast<size_t>(M), std::vector<cf>(static_cast<size_t>(2 * K + 1)));
    std::vector<std::vector<cf>> Ys = Yp;
    const double early = std::min(4.0, 0.4 * num.cp());
    std::vector<CellHit> hits;
    // The PSS channel of N_ID2 u at subcarrier i: least squares over the
    // 2 PSS_SMOOTH + 1 subcarriers around it of Y = H_0 d_0 + H_1 d_1 + H_2 d_2
    // (each channel constant there) - cells of the other N_ID2 at the same
    // timing (synchronised networks) are nulled instead of leaking in. A fixed
    // filter per u: H_u(i) = sum over q of F[u][i][q] Y(q0(i) + q)
    const int LW = 2 * PSS_SMOOTH + 1;
    std::vector<std::vector<cd>> Fl(3, std::vector<cd>(static_cast<size_t>(nv * LW), cd(0, 0)));
    std::vector<int> q0(static_cast<size_t>(nv));
    for (int i = 0; i < nv; i++) {
        const int a0 = std::clamp(i - PSS_SMOOTH, 0, std::max(0, nv - LW));
        q0[static_cast<size_t>(i)] = a0;
        const int len = std::min(LW, nv - a0);
        // G = A^H A + loading, A[q][r] = d_r(vis[a0 + q])
        cd G[3][3] = {};
        for (int r = 0; r < 3; r++)
            for (int c2 = 0; c2 < 3; c2++)
                for (int q = 0; q < len; q++) {
                    const int vi = vis[static_cast<size_t>(a0 + q)];
                    G[r][c2] += std::conj(dpss[static_cast<size_t>(r)][static_cast<size_t>(vi)]) * dpss[static_cast<size_t>(c2)][static_cast<size_t>(vi)];
                }
        for (int r = 0; r < 3; r++) G[r][r] += 0.05 * len;
        // 3 x 3 inverse (adjugate)
        const cd det = G[0][0] * (G[1][1] * G[2][2] - G[1][2] * G[2][1]) - G[0][1] * (G[1][0] * G[2][2] - G[1][2] * G[2][0]) +
                       G[0][2] * (G[1][0] * G[2][1] - G[1][1] * G[2][0]);
        cd Gi[3][3];
        Gi[0][0] = (G[1][1] * G[2][2] - G[1][2] * G[2][1]) / det;
        Gi[0][1] = (G[0][2] * G[2][1] - G[0][1] * G[2][2]) / det;
        Gi[0][2] = (G[0][1] * G[1][2] - G[0][2] * G[1][1]) / det;
        Gi[1][0] = (G[1][2] * G[2][0] - G[1][0] * G[2][2]) / det;
        Gi[1][1] = (G[0][0] * G[2][2] - G[0][2] * G[2][0]) / det;
        Gi[1][2] = (G[0][2] * G[1][0] - G[0][0] * G[1][2]) / det;
        Gi[2][0] = (G[1][0] * G[2][1] - G[1][1] * G[2][0]) / det;
        Gi[2][1] = (G[0][1] * G[2][0] - G[0][0] * G[2][1]) / det;
        Gi[2][2] = (G[0][0] * G[1][1] - G[0][1] * G[1][0]) / det;
        for (int u = 0; u < 3; u++)
            for (int q = 0; q < len; q++) {
                const int vi = vis[static_cast<size_t>(a0 + q)];
                cd c = 0;
                for (int r = 0; r < 3; r++) c += Gi[u][r] * std::conj(dpss[static_cast<size_t>(r)][static_cast<size_t>(vi)]);
                Fl[static_cast<size_t>(u)][static_cast<size_t>(i * LW + q)] = c;
            }
    }
    // w(k) = sum over the antennas of SSS x conj(PSS channel) of one occurrence
    std::vector<cd> hs(static_cast<size_t>(nv));
    auto occurrence_w = [&](double ss, int u, std::vector<cd>* w) {
        for (int a = 0; a < M; a++)
            if (!sym.run(x[static_cast<size_t>(a)], n, ss, early, Ys[static_cast<size_t>(a)].data())) return false;
        std::fill(w->begin(), w->end(), cd(0, 0));
        const auto& Fu = Fl[static_cast<size_t>(u)];
        for (int a = 0; a < M; a++) {
            const auto& Y = Yp[static_cast<size_t>(a)];
            for (int i = 0; i < nv; i++) {
                cd h = 0;
                const int a0 = q0[static_cast<size_t>(i)];
                for (int q = 0; q < LW && a0 + q < nv; q++)
                    h += Fu[static_cast<size_t>(i * LW + q)] * cd(Y[static_cast<size_t>(f[static_cast<size_t>(vis[static_cast<size_t>(a0 + q)])] + K)]);
                hs[static_cast<size_t>(i)] = h;
            }
            for (int i = 0; i < nv; i++) {
                const int k = f[static_cast<size_t>(vis[static_cast<size_t>(i)])] + K;
                (*w)[static_cast<size_t>(i)] += cd(Ys[static_cast<size_t>(a)][static_cast<size_t>(k)]) * std::conj(hs[static_cast<size_t>(i)]);
            }
        }
        return true;
    };
    auto pss_at = [&](double ps) {
        for (int a = 0; a < M; a++)
            if (!sym.run(x[static_cast<size_t>(a)], n, ps, early, Yp[static_cast<size_t>(a)].data())) return false;
        return true;
    };
    std::vector<cd> w(static_cast<size_t>(nv));
    // successive cancellation of a detected SSS d from the sum W: W(k) d(k)
    // is that cell's |channel|^2 there - smooth over the subcarriers (a
    // reflection makes it vary); its local mean (+-CANCEL_SMOOTH subcarriers)
    // times d is taken out, so the other cells' SSS stand out from what's left
    std::vector<cd> g(static_cast<size_t>(nv));
    auto cancel = [&](std::vector<cd>* Wv, const float* d) {
        for (int i = 0; i < nv; i++) g[static_cast<size_t>(i)] = (*Wv)[static_cast<size_t>(i)] * static_cast<double>(d[i]);
        for (int i = 0; i < nv; i++) {
            cd sm = 0;
            int c = 0;
            for (int q = std::max(0, i - CANCEL_SMOOTH); q <= std::min(nv - 1, i + CANCEL_SMOOTH); q++, c++) sm += g[static_cast<size_t>(q)];
            (*Wv)[static_cast<size_t>(i)] -= sm / static_cast<double>(c) * static_cast<double>(d[i]);
        }
    };

    if (lte) {
        // FDD: SSS the symbol before the PSS (slot 0 / 10, symbols 5 / 6);
        // TDD: SSS in the last symbol of subframe 0 / 5, PSS the third of 1 / 6
        const double half = num.half_ms();
        const double pss_off[2] = {num.useful(6), 2 * half + num.useful(2)};   // from the radio frame (or its half) start
        const double sss_off[2] = {num.useful(5), half + num.useful(6)};
        // SSS of every group, both halves of the frame, on the visible subcarriers
        std::map<int, std::vector<float>> tab;   // nid2 -> [nid1][variant][i]
        auto table = [&](int u) -> const std::vector<float>& {
            auto it = tab.find(u);
            if (it != tab.end()) return it->second;
            std::vector<float> t(static_cast<size_t>(168 * 2 * nv));
            for (int g = 0; g < 168; g++)
                for (int v = 0; v < 2; v++) {
                    const auto d = lte::sss(g, u, v == 1);
                    for (int i = 0; i < nv; i++)
                        t[static_cast<size_t>((g * 2 + v) * nv + i)] = static_cast<float>(d[static_cast<size_t>(vis[static_cast<size_t>(i)])].real());
                }
            return tab.emplace(u, std::move(t)).first->second;
        };
        struct Det { int pos, dup, par, nid1, nid2; };
        std::vector<Det> dets;
        auto cotimed = [&](int a, int b) {
            const int d = std::abs(a - b);
            return std::min(d, P - d) <= COTIMED_SAMPLES;
        };
        for (const auto& pk : cand) {
            if (!accept(pk)) continue;
            const auto& T = table(pk.u);
            std::vector<cd> W[2][2];
            double V[2] = {0, 0};
            int occ[2] = {0, 0};
            for (auto& d : W)
                for (auto& h : d) h.assign(static_cast<size_t>(nv), cd(0, 0));
            for (long j = 0; j < p.max_occ; j++) {
                const double pj = pk.pos + static_cast<double>(j) * P;
                if (pj + N + 1 > static_cast<double>(n)) break;
                if (!pss_at(pj)) continue;
                for (int dup = 0; dup < 2; dup++) {
                    if (p.duplex >= 0 && dup != p.duplex) continue;
                    if (!occurrence_w(pj - (pss_off[dup] - sss_off[dup]), pk.u, &w)) continue;
                    for (int i = 0; i < nv; i++) {
                        W[dup][j & 1][static_cast<size_t>(i)] += w[static_cast<size_t>(i)];
                        V[dup] += std::norm(w[static_cast<size_t>(i)]);
                    }
                    occ[dup]++;
                }
            }
            auto dot = [&](const std::vector<cd>& a, const float* d) {
                cd s = 0;
                for (int i = 0; i < nv; i++) s += a[static_cast<size_t>(i)] * static_cast<double>(d[i]);
                return s;
            };
            // the cells already found at this timing (any N_ID2): their SSS out first
            for (const auto& dt : dets) {
                if (!cotimed(dt.pos, pk.pos)) continue;
                const auto& Td = table(dt.nid2);
                for (int half_i = 0; half_i < 2; half_i++) {
                    const int v = (half_i == 0) == (dt.par == 0) ? 0 : 1;
                    cancel(&W[dt.dup][half_i], &Td[static_cast<size_t>((dt.nid1 * 2 + v) * nv)]);
                }
            }
            std::vector<double> all;
            double first_score = 1;
            for (int round = 0; round < p.max_per_peak; round++) {
                double best = -1e300;
                cd bsum = 0;
                int bd = -1, bg = 0, bp = 0;
                all.clear();
                for (int dup = 0; dup < 2; dup++) {
                    if (occ[dup] < 2 || !(V[dup] > 0)) continue;
                    const double norm = 1 / std::sqrt(V[dup] / 2);
                    for (int g = 0; g < 168; g++) {
                        const float* d0 = &T[static_cast<size_t>((g * 2 + 0) * nv)];
                        const float* d5 = &T[static_cast<size_t>((g * 2 + 1) * nv)];
                        // even occurrences are subframe 0 (parity 0) or 5 (parity 1)
                        // magnitude: a carrier offset left over turns the
                        // SSS against the PSS by 2 pi df dt - measured below
                        const cd c0 = dot(W[dup][0], d0) + dot(W[dup][1], d5);
                        const cd c1 = dot(W[dup][0], d5) + dot(W[dup][1], d0);
                        const double s0 = std::abs(c0) * norm, s1 = std::abs(c1) * norm;
                        all.push_back(s0);
                        all.push_back(s1);
                        if (s0 > best) { best = s0; bd = dup; bg = g; bp = 0; bsum = c0; }
                        if (s1 > best) { best = s1; bd = dup; bg = g; bp = 1; bsum = c1; }
                    }
                }
                const double z = robust_z(all, best);
                if (bd < 0 || best < p.min_score || z < p.min_z) break;
                CellHit h;
                h.tech = Tech::LTE;
                h.nid2 = pk.u;
                h.pci = 3 * bg + pk.u;
                h.tdd = bd == 1;
                h.score = best;
                h.z = z;
                h.round = round;
                // the SSS precedes the PSS by dt: its phase against the PSS is -2 pi df dt
                h.df_hz = -std::arg(bsum) / (2 * M_PI * (pss_off[bd] - sss_off[bd]) / num.fs);
                h.pss_mod = pk.pos;
                // the first occurrence that is subframe 0 -> a radio frame's start
                double fr = pk.pos + static_cast<double>(bp) * P - pss_off[bd];
                while (fr > 0) fr -= 2.0 * P;
                h.frame0 = fr;
                hits.push_back(h);
                dets.push_back({pk.pos, bd, bp, bg, pk.u});
                if (round == 0) first_score = best;
                SssSrc sc{pk.pos - (pss_off[bd] - sss_off[bd]), pk.v * best / first_score, {}};
                for (int v = 0; v < 2; v++) {
                    const auto d = lte::sss(bg, pk.u, v == 1);
                    std::vector<cd> sv;
                    for (int vi : vis) sv.push_back(d[static_cast<size_t>(vi)]);
                    sc.S.push_back(std::move(sv));
                }
                sss_src.push_back(std::move(sc));
                // cancel it from both halves
                for (int half_i = 0; half_i < 2; half_i++) {
                    const int v = (half_i == 0) == (bp == 0) ? 0 : 1;
                    cancel(&W[bd][half_i], &T[static_cast<size_t>((bg * 2 + v) * nv)]);
                }
            }
        }
    } else {
        // NR: SSS two symbols after the PSS (SS/PBCH block symbols 0 and 2;
        // no 0.5 ms boundary between them in any SSB pattern)
        const double dS = 2 * num.sym_len();
        std::map<int, std::vector<float>> tab;
        auto table = [&](int u) -> const std::vector<float>& {
            auto it = tab.find(u);
            if (it != tab.end()) return it->second;
            std::vector<float> t(static_cast<size_t>(336 * nv));
            for (int g = 0; g < 336; g++) {
                const auto d = nr::sss(g, u);
                for (int i = 0; i < nv; i++) t[static_cast<size_t>(g * nv + i)] = static_cast<float>(d[static_cast<size_t>(vis[static_cast<size_t>(i)])]);
            }
            return tab.emplace(u, std::move(t)).first->second;
        };
        struct Det { int pos, nid1, nid2; };
        std::vector<Det> dets;
        for (const auto& pk : cand) {
            if (!accept(pk)) continue;
            const auto& T = table(pk.u);
            std::vector<cd> W(static_cast<size_t>(nv), cd(0, 0));
            double V = 0;
            std::vector<double> occs;
            for (long j = 0; j < p.max_occ; j++) {
                const double pj = pk.pos + static_cast<double>(j) * P;
                if (pj + dS + N + 1 > static_cast<double>(n)) break;
                if (!pss_at(pj) || !occurrence_w(pj + dS, pk.u, &w)) continue;
                for (int i = 0; i < nv; i++) {
                    W[static_cast<size_t>(i)] += w[static_cast<size_t>(i)];
                    V += std::norm(w[static_cast<size_t>(i)]);
                }
                occs.push_back(pj);
            }
            if (occs.size() < 2 || !(V > 0)) continue;
            const double norm = 1 / std::sqrt(V / 2);
            for (const auto& dt : dets) {
                const int d = std::abs(dt.pos - pk.pos);
                if (std::min(d, P - d) > COTIMED_SAMPLES) continue;
                cancel(&W, &table(dt.nid2)[static_cast<size_t>(dt.nid1 * nv)]);
            }
            std::vector<double> all;
            double first_score = 1;
            for (int round = 0; round < p.max_per_peak; round++) {
                double best = -1e300;
                cd bsum = 0;
                int bg = -1;
                all.clear();
                for (int g = 0; g < 336; g++) {
                    const float* d = &T[static_cast<size_t>(g * nv)];
                    cd s = 0;
                    for (int i = 0; i < nv; i++) s += W[static_cast<size_t>(i)] * static_cast<double>(d[i]);
                    all.push_back(std::abs(s) * norm);
                    if (std::abs(s) * norm > best) { best = std::abs(s) * norm; bg = g; bsum = s; }
                }
                const double z = robust_z(all, best);
                if (bg < 0 || best < p.min_score || z < p.min_z) break;
                CellHit h;
                h.tech = Tech::NR;
                h.nid2 = pk.u;
                h.pci = 3 * bg + pk.u;
                h.score = best;
                h.z = z;
                h.round = round;
                h.df_hz = std::arg(bsum) / (2 * M_PI * dS / num.fs);   // the SSS follows the PSS by dS
                h.pss_mod = pk.pos;
                h.pss = occs;
                hits.push_back(h);
                dets.push_back({pk.pos, bg, pk.u});
                if (round == 0) first_score = best;
                SssSrc sc{pk.pos + dS, pk.v * best / first_score, {}};
                const auto d = nr::sss(bg, pk.u);
                std::vector<cd> sv;
                for (int vi : vis) sv.push_back(d[static_cast<size_t>(vi)]);
                sc.S.push_back(std::move(sv));
                sss_src.push_back(std::move(sc));
                cancel(&W, &T[static_cast<size_t>(bg * nv)]);
            }
        }
    }

    // --- one entry per cell: the strongest detection; NR: every SSB position
    // of the cell (beams), but not a second peak right next to one (the
    // correlation's sidelobes find the same SSB again) ---
    std::sort(hits.begin(), hits.end(), [](const CellHit& a, const CellHit& b) { return a.score > b.score; });
    for (const auto& h : hits) {
        CellHit* same = nullptr;
        for (auto& c : out.cells)
            if (c.pci == h.pci) same = &c;
        if (!same) {
            out.cells.push_back(h);
            continue;
        }
        if (lte) continue;
        const double sep = std::fabs(std::remainder(h.pss_mod - same->pss_mod, static_cast<double>(P)));
        bool near = sep < 2 * num.sym_len();
        for (double q : same->pss)
            for (double r : h.pss)
                if (std::fabs(q - r) < 2 * num.sym_len()) near = true;
        if (near) continue;
        same->pss.insert(same->pss.end(), h.pss.begin(), h.pss.end());
        same->beams++;
    }
    out.peaks = static_cast<int>(peaks.size());
    prof().peaks += out.peaks;
    prof().sss += now_s() - ts0;
    return out;
}

}  // namespace cells
