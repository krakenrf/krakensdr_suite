#include "turbo.hpp"

#include "lte_tables.hpp"

#include <algorithm>
#include <cmath>

namespace cells::fec {

namespace {

// constituent encoder state: bit 2 = s1 (latest), bit 1 = s2, bit 0 = s3
inline int s1(int s) { return (s >> 2) & 1; }
inline int s2(int s) { return (s >> 1) & 1; }
inline int s3(int s) { return s & 1; }
// input u from state s: feedback a = u + s2 + s3, parity z = a + s1 + s3, next state (a, s1, s2)
inline int fb(int s, int u) { return u ^ s2(s) ^ s3(s); }
inline int par(int s, int u) { return fb(s, u) ^ s1(s) ^ s3(s); }
inline int next(int s, int u) { return (fb(s, u) << 2) | (s1(s) << 1) | s2(s); }

constexpr int PERM_TC[32] = {0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
                             1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31};

// one constituent encoder over K bits + its 3 tail steps: x (K + 3), z (K + 3)
void rsc(const std::vector<uint8_t>& u, std::vector<uint8_t>* x, std::vector<uint8_t>* z) {
    const size_t K = u.size();
    x->assign(K + 3, 0);
    z->assign(K + 3, 0);
    int s = 0;
    for (size_t k = 0; k < K + 3; k++) {
        const int b = k < K ? (u[k] & 1) : (s2(s) ^ s3(s));   // tail: the input that feeds back 0
        (*x)[k] = static_cast<uint8_t>(b);
        (*z)[k] = static_cast<uint8_t>(par(s, b));
        s = next(s, b);
    }
}

// max-log BCJR of one constituent code: Lx systematic (K + 3, tail included),
// Lz parity (K + 3), La a-priori (K); returns the extrinsic (K) and the APP (K)
void bcjr(const std::vector<float>& Lx, const std::vector<float>& Lz, const std::vector<float>& La, std::vector<float>* Le,
          std::vector<float>* Lapp) {
    const int K = static_cast<int>(La.size()), T = K + 3;
    constexpr float NEG = -1e30f;
    std::vector<float> A(static_cast<size_t>((T + 1) * 8), NEG), B(static_cast<size_t>((T + 1) * 8), NEG);
    A[0] = 0;
    // branch metric of input u, parity p at step k (LLR > 0 = 0)
    auto gam = [&](int k, int u, int p) {
        const float lu = Lx[static_cast<size_t>(k)] + (k < K ? La[static_cast<size_t>(k)] : 0.0f);
        return 0.5f * ((u ? -lu : lu) + (p ? -Lz[static_cast<size_t>(k)] : Lz[static_cast<size_t>(k)]));
    };
    for (int k = 0; k < T; k++) {
        float* a1 = &A[static_cast<size_t>((k + 1) * 8)];
        const float* a0 = &A[static_cast<size_t>(k * 8)];
        for (int s = 0; s < 8; s++) {
            if (a0[s] <= NEG / 2) continue;
            for (int u = 0; u < 2; u++) {
                const float m = a0[s] + gam(k, u, par(s, u));
                const int ns = next(s, u);
                if (m > a1[ns]) a1[ns] = m;
            }
        }
        const float mx = *std::max_element(a1, a1 + 8);
        for (int s = 0; s < 8; s++)
            if (a1[s] > NEG / 2) a1[s] -= mx;
    }
    B[static_cast<size_t>(T * 8)] = 0;   // terminated in state 0
    for (int k = T - 1; k >= 0; k--) {
        float* b0 = &B[static_cast<size_t>(k * 8)];
        const float* b1 = &B[static_cast<size_t>((k + 1) * 8)];
        for (int s = 0; s < 8; s++)
            for (int u = 0; u < 2; u++) {
                const int ns = next(s, u);
                if (b1[ns] <= NEG / 2) continue;
                const float m = b1[ns] + gam(k, u, par(s, u));
                if (m > b0[s]) b0[s] = m;
            }
        const float mx = *std::max_element(b0, b0 + 8);
        for (int s = 0; s < 8; s++)
            if (b0[s] > NEG / 2) b0[s] -= mx;
    }
    Le->assign(static_cast<size_t>(K), 0.0f);
    Lapp->assign(static_cast<size_t>(K), 0.0f);
    for (int k = 0; k < K; k++) {
        float m0 = NEG, m1 = NEG;
        const float* a0 = &A[static_cast<size_t>(k * 8)];
        const float* b1 = &B[static_cast<size_t>((k + 1) * 8)];
        for (int s = 0; s < 8; s++) {
            if (a0[s] <= NEG / 2) continue;
            for (int u = 0; u < 2; u++) {
                const int ns = next(s, u);
                if (b1[ns] <= NEG / 2) continue;
                const float m = a0[s] + gam(k, u, par(s, u)) + b1[ns];
                if (u) m1 = std::max(m1, m);
                else m0 = std::max(m0, m);
            }
        }
        const float app = m0 - m1;
        (*Lapp)[static_cast<size_t>(k)] = app;
        (*Le)[static_cast<size_t>(k)] = app - Lx[static_cast<size_t>(k)] - La[static_cast<size_t>(k)];
    }
}

}  // namespace

int turbo_k(int B) {
    for (int i = 0; i < cells::lte::QPP_N; i++)
        if (cells::lte::QPP[i][0] >= B) return cells::lte::QPP[i][0];
    return 0;
}

std::vector<int> qpp(int K) {
    for (int i = 0; i < cells::lte::QPP_N; i++) {
        if (cells::lte::QPP[i][0] != K) continue;
        const long f1 = cells::lte::QPP[i][1], f2 = cells::lte::QPP[i][2];
        std::vector<int> p(static_cast<size_t>(K));
        for (long j = 0; j < K; j++) p[static_cast<size_t>(j)] = static_cast<int>((f1 * j + f2 * j % K * j) % K);
        return p;
    }
    return {};
}

void turbo_encode(const std::vector<uint8_t>& c, std::vector<uint8_t> d[3]) {
    const int K = static_cast<int>(c.size());
    const auto pi = qpp(K);
    std::vector<uint8_t> ci(static_cast<size_t>(K));
    for (int i = 0; i < K; i++) ci[static_cast<size_t>(i)] = c[static_cast<size_t>(pi[static_cast<size_t>(i)])];
    std::vector<uint8_t> x, z, xp, zp;
    rsc(c, &x, &z);
    rsc(ci, &xp, &zp);
    for (int s = 0; s < 3; s++) d[s].assign(static_cast<size_t>(K + 4), 0);
    for (int k = 0; k < K; k++) {
        d[0][static_cast<size_t>(k)] = x[static_cast<size_t>(k)];
        d[1][static_cast<size_t>(k)] = z[static_cast<size_t>(k)];
        d[2][static_cast<size_t>(k)] = zp[static_cast<size_t>(k)];
    }
    const size_t k = static_cast<size_t>(K);
    d[0][k] = x[k];
    d[0][k + 1] = z[k + 1];
    d[0][k + 2] = xp[k];
    d[0][k + 3] = zp[k + 1];
    d[1][k] = z[k];
    d[1][k + 1] = x[k + 2];
    d[1][k + 2] = zp[k];
    d[1][k + 3] = xp[k + 2];
    d[2][k] = x[k + 1];
    d[2][k + 1] = z[k + 2];
    d[2][k + 2] = xp[k + 1];
    d[2][k + 3] = zp[k + 2];
}

std::vector<std::pair<int, int>> turbo_rate_match(int K, int F, int E, int rv) {
    const int D = K + 4, C = 32, R = (D + C - 1) / C, KP = R * C, ND = KP - D;
    // the circular buffer: v0, then v1 / v2 interlaced; (-1, -1) = dummy / filler
    std::vector<std::pair<int, int>> w(static_cast<size_t>(3 * KP), {-1, -1});
    auto entry = [&](int str, int y) -> std::pair<int, int> {
        if (y < ND) return {-1, -1};
        const int i = y - ND;
        if (str < 2 && i < F) return {-1, -1};
        return {str, i};
    };
    for (int k = 0; k < KP; k++) {
        const int y01 = PERM_TC[k / R] + C * (k % R);
        const int y2 = (PERM_TC[k / R] + C * (k % R) + 1) % KP;
        w[static_cast<size_t>(k)] = entry(0, y01);
        w[static_cast<size_t>(KP + 2 * k)] = entry(1, y01);
        w[static_cast<size_t>(KP + 2 * k + 1)] = entry(2, y2);
    }
    const int Ncb = 3 * KP;
    const int k0 = R * (2 * ((Ncb + 8 * R - 1) / (8 * R)) * rv + 2);
    std::vector<std::pair<int, int>> e;
    e.reserve(static_cast<size_t>(E));
    for (long j = 0; static_cast<int>(e.size()) < E && j < 4L * Ncb + E; j++) {
        const auto& p = w[static_cast<size_t>((k0 + j) % Ncb)];
        if (p.first >= 0) e.push_back(p);
    }
    return e;
}

std::vector<uint8_t> turbo_decode(const std::vector<float> s[3], int K, int iters, const std::function<bool(const std::vector<uint8_t>&)>& ok,
                                  int* used_iters) {
    const auto pi = qpp(K);
    if (pi.empty() || static_cast<int>(s[0].size()) < K + 4) return {};
    const size_t k = static_cast<size_t>(K);
    // systematic / parity of both constituents (tails from 36.212 5.1.3.2.2)
    std::vector<float> x1(k + 3), z1(k + 3), x2(k + 3), z2(k + 3);
    for (size_t i = 0; i < k; i++) {
        x1[i] = s[0][i];
        z1[i] = s[1][i];
        x2[i] = s[0][static_cast<size_t>(pi[i])];
        z2[i] = s[2][i];
    }
    x1[k] = s[0][k];
    z1[k] = s[1][k];
    x1[k + 1] = s[2][k];
    z1[k + 1] = s[0][k + 1];
    x1[k + 2] = s[1][k + 1];
    z1[k + 2] = s[2][k + 1];
    x2[k] = s[0][k + 2];
    z2[k] = s[1][k + 2];
    x2[k + 1] = s[2][k + 2];
    z2[k + 1] = s[0][k + 3];
    x2[k + 2] = s[1][k + 3];
    z2[k + 2] = s[2][k + 3];
    std::vector<float> La1(k, 0.0f), La2(k), Le1, Le2, A1, A2;
    std::vector<uint8_t> bits(k);
    constexpr float SCALE = 0.7f;   // extrinsic scaling (max-log)
    int it = 0;
    for (it = 1; it <= iters; it++) {
        bcjr(x1, z1, La1, &Le1, &A1);
        for (size_t i = 0; i < k; i++) La2[i] = SCALE * Le1[static_cast<size_t>(pi[i])];
        bcjr(x2, z2, La2, &Le2, &A2);
        for (size_t i = 0; i < k; i++) {
            La1[static_cast<size_t>(pi[i])] = SCALE * Le2[i];
            bits[static_cast<size_t>(pi[i])] = A2[i] < 0 ? 1 : 0;
        }
        if (ok && ok(bits)) break;
    }
    if (used_iters) *used_iters = std::min(it, iters);
    return bits;
}

}  // namespace cells::fec
