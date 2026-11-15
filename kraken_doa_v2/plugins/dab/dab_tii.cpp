#include "dab_tii.hpp"

#include <algorithm>
#include <cmath>

namespace dab {

namespace {

using cfl = std::complex<float>;
using cd = std::complex<double>;
constexpr double PI2 = 6.283185307179586476925286766559;

struct Cand {
    int c = 0;
    unsigned mask = 0;
    double e = 0;   // mean normalised slot energy (noise = 1)
    bool keep = true;
};

// Eigen-decomposition of a Hermitian m x m matrix (row-major), m <= 8: cyclic
// complex Jacobi rotations. lam: the m real eigenvalues; V (optional): the
// eigenvectors as columns (row-major). Hermitian matrices that are not positive
// semi-definite (a covariance with the noise subtracted) are fine.
void hermitian_eig(const std::vector<cd>& R, int m, double* lam, std::vector<cd>* V) {
    std::vector<cd> A = R;
    auto at = [m](std::vector<cd>& M, int r, int c) -> cd& { return M[static_cast<size_t>(r * m + c)]; };
    if (V) {
        V->assign(static_cast<size_t>(m * m), cd(0, 0));
        for (int a = 0; a < m; a++) at(*V, a, a) = 1;
    }
    double scale = 0;
    for (const auto& x : A) scale += std::norm(x);
    for (int sweep = 0; sweep < 50 && scale > 0; sweep++) {
        double off = 0;
        for (int p = 0; p < m; p++)
            for (int q = p + 1; q < m; q++) off += std::norm(at(A, p, q));
        if (!(off > 1e-26 * scale)) break;
        for (int p = 0; p < m; p++)
            for (int q = p + 1; q < m; q++) {
                const cd apq = at(A, p, q);
                const double r = std::abs(apq);
                if (!(r > 0)) continue;
                // the 2 x 2 problem [[a_pp, r], [r, a_qq]] after taking out apq's phase: U = [[c, s e^(j phi)], [-s e^(-j phi), c]]
                const cd ph = apq / r;
                const double th = (at(A, q, q).real() - at(A, p, p).real()) / (2 * r);
                const double t = (th >= 0 ? 1.0 : -1.0) / (std::fabs(th) + std::sqrt(th * th + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                const cd sp = s * ph, sm = s * std::conj(ph);
                for (int k = 0; k < m; k++) {   // A U
                    const cd x = at(A, k, p), y = at(A, k, q);
                    at(A, k, p) = c * x - sm * y;
                    at(A, k, q) = sp * x + c * y;
                }
                for (int k = 0; k < m; k++) {   // U^H (A U)
                    const cd x = at(A, p, k), y = at(A, q, k);
                    at(A, p, k) = c * x - sp * y;
                    at(A, q, k) = sm * x + c * y;
                }
                at(A, p, q) = at(A, q, p) = 0;
                if (V)
                    for (int k = 0; k < m; k++) {
                        const cd x = at(*V, k, p), y = at(*V, k, q);
                        at(*V, k, p) = c * x - sm * y;
                        at(*V, k, q) = sp * x + c * y;
                    }
            }
    }
    for (int a = 0; a < m; a++) lam[a] = at(A, a, a).real();
}

// the main eigenvalue's share of a Hermitian matrix: lambda_max / sum |lambda_i|. For a
// covariance (positive semi-definite) that is lambda_max / trace; a noise-subtracted one
// can have negative eigenvalues - they count against it (and the share is never above 1,
// nor positive when no eigenvalue is)
double main_share(const std::vector<cd>& R, int m) {
    double lam[8];
    if (m < 1 || m > 8) return 0;
    hermitian_eig(R, m, lam, nullptr);
    double mx = lam[0], sa = 0;
    for (int a = 0; a < m; a++) {
        mx = std::max(mx, lam[a]);
        sa += std::fabs(lam[a]);
    }
    return mx > 0 && sa > 0 ? mx / sa : 0;
}

// R -> its positive semi-definite part (the negative eigenvalues set to 0); false = nothing positive
bool psd_part(std::vector<cd>& R, int m) {
    double lam[8];
    std::vector<cd> V;
    if (m < 1 || m > 8) return false;
    hermitian_eig(R, m, lam, &V);
    std::fill(R.begin(), R.end(), cd(0, 0));
    bool any = false;
    for (int i = 0; i < m; i++) {
        if (!(lam[i] > 0)) continue;
        any = true;
        for (int a = 0; a < m; a++)
            for (int b = 0; b < m; b++)
                R[static_cast<size_t>(a * m + b)] += lam[i] * V[static_cast<size_t>(a * m + i)] * std::conj(V[static_cast<size_t>(b * m + i)]);
    }
    return any;
}

}   // namespace

void Tii::reset() {
    tx_.clear();
    dropped_.clear();
    symbols = tii_symbols = 0;
}

std::vector<int> Tii::take_dropped() {
    std::vector<int> d;
    d.swap(dropped_);
    return d;
}

int Tii::strongest() const {
    int best = -1;
    double bv = 0;
    for (const auto& kv : tx_)
        if (kv.second.confirmed && kv.second.snr_lin > bv) {
            bv = kv.second.snr_lin;
            best = kv.first;
        }
    return best;
}

bool Tii::delay_us(const Tx& t, double* us) const {
    const int s = strongest();
    if (s < 0) return false;
    const Tx& r = tx_.at(s);
    if (!t.tau_ok || !r.tau_ok) return false;
    *us = (t.tau - r.tau) / 2.4;
    return true;
}

void Tii::update_delay(Tx& t, const cfl* const* car, int m, const std::vector<int>& own) {
    // own: dense indices of the code's carriers no other code used (pairs: k0, k0 + 1).
    // Two conventions for the pair's second carrier are kept apart: the
    // standard's phi_k0 (clause 14.8: A(k-1) e^(j phi_k-1) on carrier k) and
    // phi_k0+1, which some transmitters on the air use (measured: a pair phase
    // step that is coherent only that way). Each transmitter gets the one
    // its pairs agree with.
    std::vector<uint8_t> mine(K, 0);
    for (int i : own) mine[static_cast<size_t>(i)] = 1;
    static const cfl Q4[4] = {cfl(1, 0), cfl(0, -1), cfl(-1, 0), cfl(0, 1)};   // e^(-j pi/2 n)
    cd q[2] = {cd(0, 0), cd(0, 0)};
    double qa = 0;
    std::vector<int> ks;
    std::vector<cfl> zs[2];
    for (int b = 0; b < 8; b++) {
        if (!tii_bit(t.p, b)) continue;
        for (int qq = 0; qq < 4; qq++) {
            const int k0 = tii_carrier(t.c, b, qq);
            const int i0 = carrier_index(k0), i1 = carrier_index(k0 + 1);
            if (!mine[static_cast<size_t>(i0)] || !mine[static_cast<size_t>(i1)]) continue;
            const int dq = (prs_quarter(k0 + 1) - prs_quarter(k0)) & 3;
            for (int a = 0; a < m; a++) {
                const cd v = cd(car[a][i1]) * std::conj(cd(car[a][i0]));
                q[0] += v;
                q[1] += v * cd(Q4[dq]);
                qa += std::abs(v);
            }
            const cfl d0 = Q4[prs_quarter(k0) & 3], d1 = Q4[prs_quarter(k0 + 1) & 3];
            ks.push_back(k0);
            zs[0].push_back(car[0][i0] * d0);
            zs[1].push_back(car[0][i0] * d0);
            ks.push_back(k0 + 1);
            zs[0].push_back(car[0][i1] * d0);
            zs[1].push_back(car[0][i1] * d1);
        }
    }
    if (ks.empty()) return;
    constexpr double FORGET = 0.97;
    for (int v = 0; v < 2; v++) {
        t.qsum2[v] = t.qsum2[v] * FORGET + q[v];
        if (t.prof2[v].empty()) t.prof2[v].assign(PROF_N, 0.0f);
        for (auto& x : t.prof2[v]) x *= static_cast<float>(FORGET);
    }
    t.qabs = t.qabs * FORGET + qa;
    // the convention whose pair phase steps agree
    const int conv = std::abs(t.qsum2[1]) > std::abs(t.qsum2[0]) ? 1 : 0;
    t.convention = conv;
    const size_t nk = ks.size();
    std::vector<cfl> ph(nk), st(nk);
    for (size_t i = 0; i < nk; i++) {
        const double a0 = PI2 * ks[i] * PROF_LO / NFFT, a1 = PI2 * ks[i] / NFFT;
        st[i] = cfl(static_cast<float>(std::cos(a1)), static_cast<float>(std::sin(a1)));
        ph[i] = cfl(static_cast<float>(std::cos(a0)), static_cast<float>(std::sin(a0)));
    }
    for (int v = 0; v < 2; v++) {
        std::vector<cfl> p(nk);
        for (size_t i = 0; i < nk; i++) p[i] = zs[v][i] * ph[i];
        for (int j = 0; j < PROF_N; j++) {
            cfl acc(0, 0);
            for (size_t i = 0; i < nk; i++) {
                acc += p[i];
                p[i] *= st[i];
            }
            t.prof2[v][static_cast<size_t>(j)] += std::norm(acc);
        }
    }
    t.nprof++;
    t.tau_ok = false;
    if (!(t.qabs > 0)) return;
    const cd qs = t.qsum2[conv];
    const std::vector<float>& prof = t.prof2[conv];
    const double tc = -std::arg(qs) * NFFT / PI2;   // coarse, +-1200 samples
    t.tau_coarse = tc;
    t.coherence = std::abs(qs) / t.qabs;
    t.tau_fine = NAN;
    if (t.nprof < 8 || t.coherence < 0.5) return;
    const int lo = std::max(0, static_cast<int>(std::floor(tc - PROF_LO - 25)));
    const int hi = std::min(PROF_N - 1, static_cast<int>(std::ceil(tc - PROF_LO + 25)));
    if (lo >= hi) return;
    int bj = lo;
    for (int j = lo; j <= hi; j++)
        if (prof[static_cast<size_t>(j)] > prof[static_cast<size_t>(bj)]) bj = j;
    if (bj == lo || bj == hi) return;   // no peak inside the coarse window
    const double a = prof[static_cast<size_t>(bj - 1)], b = prof[static_cast<size_t>(bj)], c = prof[static_cast<size_t>(bj + 1)];
    const double den = a - 2 * b + c;
    const double off = den < 0 ? std::max(-0.5, std::min(0.5, 0.5 * (a - c) / den)) : 0;
    const double tf = PROF_LO + bj + off;
    t.tau_fine = tf;
    // the profile repeats every 50 samples: the coarse value must pick the period clearly
    // (a multipath channel moves it a little - the power-weighted mean delay)
    if (std::fabs(tf - tc) > 20) return;
    t.tau = tf;
    t.tau_ok = true;
}

void Tii::process(const cfl* const* car, int m, double now, bool want_cov, std::vector<TiiPacket>& out) {
    out.clear();
    if (m < 1) return;
    symbols++;
    pw_.assign(K, 0.0f);
    for (int a = 0; a < m; a++)
        for (int i = 0; i < K; i++) pw_[static_cast<size_t>(i)] += std::norm(car[a][i]);
    srt_ = pw_;
    std::nth_element(srt_.begin(), srt_.begin() + K / 2, srt_.end());
    const double n0 = srt_[K / 2] / (m == 1 ? 0.6931 : m - 1.0 / 3.0);   // per antenna and carrier
    if (!(n0 > 0)) return;
    // slot energies: comb c, element b, over the 4 quarters' pairs
    double E[24][8];
    for (int c = 0; c < 24; c++)
        for (int b = 0; b < 8; b++) {
            double s = 0;
            for (int q = 0; q < 4; q++) {
                const int k0 = tii_carrier(c, b, q);
                s += pw_[static_cast<size_t>(carrier_index(k0))] + pw_[static_cast<size_t>(carrier_index(k0 + 1))];
            }
            E[c][b] = s / (8.0 * m * n0);
        }
    const double ton = 1.4 + 6.0 / std::sqrt(8.0 * m);
    std::vector<Cand> cands;
    for (int c = 0; c < 24; c++) {
        int idx[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        std::sort(idx, idx + 8, [&E, c](int x, int y) { return E[c][x] > E[c][y]; });
        int na = 0;
        while (na < 8 && E[c][idx[na]] >= ton) na++;
        if (na < 4) continue;
        auto mk = [&idx](int from) {
            unsigned msk = 0;
            for (int j = from; j < from + 4; j++) msk |= 1u << idx[j];
            return msk;
        };
        auto mean_e = [&E, c, &idx](int from) {
            double s = 0;
            for (int j = from; j < from + 4; j++) s += E[c][idx[j]];
            return s / 4;
        };
        if (na == 4 || (E[c][idx[3]] - 1) >= 4 * (E[c][idx[4]] - 1)) cands.push_back({c, mk(0), mean_e(0), true});
        if (na == 8 && (E[c][idx[3]] - 1) >= 4 * (E[c][idx[4]] - 1)) cands.push_back({c, mk(4), mean_e(4), true});
    }
    // structure: both carriers of each pair, 3 of 4 quarters; not a neighbour comb's copy
    for (auto& x : cands) {
        int quarters = 0;
        for (int q = 0; q < 4; q++) {
            double s = 0;
            for (int b = 0; b < 8; b++)
                if (x.mask & (1u << b)) {
                    const int k0 = tii_carrier(x.c, b, q);
                    s += pw_[static_cast<size_t>(carrier_index(k0))] + pw_[static_cast<size_t>(carrier_index(k0 + 1))];
                }
            if (s / (8.0 * m * n0) >= 1 + 0.25 * (x.e - 1)) quarters++;
        }
        if (quarters < 3) x.keep = false;
        for (int b = 0; b < 8 && x.keep; b++) {
            if (!(x.mask & (1u << b))) continue;
            double s0 = 0, s1 = 0;
            for (int q = 0; q < 4; q++) {
                const int k0 = tii_carrier(x.c, b, q);
                s0 += pw_[static_cast<size_t>(carrier_index(k0))];
                s1 += pw_[static_cast<size_t>(carrier_index(k0 + 1))];
            }
            const double r = (s0 + m * n0) / (s1 + m * n0);
            if (r > 10 || r < 0.1) x.keep = false;
        }
    }
    for (auto& x : cands)
        for (const auto& y : cands)
            if (&x != &y && y.mask == x.mask && ((std::abs(y.c - x.c) == 1 && (y.e - 1) >= 20 * (x.e - 1)) ||
                                                   (std::abs(y.c - x.c) <= 4 && (y.e - 1) >= 316 * (x.e - 1))))
                x.keep = false;
    cands.erase(std::remove_if(cands.begin(), cands.end(), [](const Cand& c) { return !c.keep; }), cands.end());

    for (auto& kv : tx_) kv.second.seen_now = false;
    if (!cands.empty()) {
        tii_symbols++;
        for (auto& kv : tx_) kv.second.hist <<= 1;
    }
    // carriers per candidate, and those used by more than one
    std::vector<uint8_t> used(K, 0);
    std::vector<std::vector<int>> carr(cands.size());
    for (size_t n = 0; n < cands.size(); n++)
        for (int b = 0; b < 8; b++)
            if (cands[n].mask & (1u << b))
                for (int q = 0; q < 4; q++) {
                    const int k0 = tii_carrier(cands[n].c, b, q);
                    for (int d = 0; d < 2; d++) {
                        const int i = carrier_index(k0 + d);
                        carr[n].push_back(i);
                        used[static_cast<size_t>(i)]++;
                    }
                }
    // the noise covariance: carriers of slots without power
    std::vector<cd> Rn;
    int nq = 0;
    if (want_cov && m >= 2) {
        Rn.assign(static_cast<size_t>(m * m), cd(0, 0));
        for (int c = 0; c < 24; c++)
            for (int b = 0; b < 8; b++) {
                if (E[c][b] > 1.5) continue;
                for (int q = 0; q < 4; q++)
                    for (int d = 0; d < 2; d++) {
                        const int i = carrier_index(tii_carrier(c, b, q) + d);
                        for (int x = 0; x < m; x++)
                            for (int y = x; y < m; y++) Rn[static_cast<size_t>(x * m + y)] += cd(car[x][i]) * std::conj(cd(car[y][i]));
                        nq++;
                    }
            }
        for (int x = 0; x < m; x++)
            for (int y = x; y < m; y++) {
                Rn[static_cast<size_t>(x * m + y)] /= std::max(1, nq);
                Rn[static_cast<size_t>(y * m + x)] = std::conj(Rn[static_cast<size_t>(x * m + y)]);
            }
    }
    for (size_t n = 0; n < cands.size(); n++) {
        const int p = tii_pattern_of(cands[n].mask);
        if (p < 0) continue;
        const int key = p * 32 + cands[n].c;
        auto it = tx_.find(key);
        if (it == tx_.end()) {
            Tx t;
            t.p = p;
            t.c = cands[n].c;
            t.first = now;
            it = tx_.emplace(key, t).first;
        }
        Tx& t = it->second;
        t.hist |= 1u;
        t.hits++;
        t.seen_now = true;
        t.snr_lin = t.hits == 1 ? cands[n].e - 1 : 0.8 * t.snr_lin + 0.2 * (cands[n].e - 1);
        t.last_hit = now;
        if (__builtin_popcount(t.hist & 0xFFFFFu) >= 5) t.confirmed = true;
        std::vector<int> own;
        for (int i : carr[n])
            if (used[static_cast<size_t>(i)] == 1) own.push_back(i);
        update_delay(t, car, m, own);
        if (!(want_cov && m >= 2 && t.confirmed) || own.size() < 8) continue;
        // its own spatial covariance, the noise taken out
        std::vector<cd> R(static_cast<size_t>(m * m), cd(0, 0));
        for (int i : own)
            for (int x = 0; x < m; x++)
                for (int y = 0; y < m; y++) R[static_cast<size_t>(x * m + y)] += cd(car[x][i]) * std::conj(cd(car[y][i]));
        const double raw_share = main_share(R, m);
        if (nq > 0)
            for (size_t j = 0; j < R.size(); j++) R[j] -= static_cast<double>(own.size()) * Rn[j];
        const double share = main_share(R, m);
        if (raw_share < 0.5 || share < 0.8) continue;
        // what goes out is a covariance: the subtraction's negative eigenvalues (noise) set to 0
        if (!psd_part(R, m)) continue;
        TiiPacket pk;
        pk.p = p;
        pk.c = cands[n].c;
        pk.m = m;
        pk.share = share;
        pk.carriers = static_cast<int>(own.size());
        pk.cov.resize(R.size());
        for (size_t j = 0; j < R.size(); j++) pk.cov[j] = cfl(static_cast<float>(R[j].real()), static_cast<float>(R[j].imag()));
        out.push_back(std::move(pk));
    }
    for (auto it = tx_.begin(); it != tx_.end();) {
        if (now - it->second.last_hit > 20) {
            if (it->second.confirmed) dropped_.push_back(it->first);
            it = tx_.erase(it);
        } else {
            ++it;
        }
    }
}

}   // namespace dab
