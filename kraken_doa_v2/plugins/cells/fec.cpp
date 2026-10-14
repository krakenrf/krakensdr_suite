#include "fec.hpp"

#include <algorithm>
#include <cmath>

namespace cells::fec {

namespace {

constexpr int K = 7;                        // constraint length
constexpr int NS = 1 << (K - 1);            // 64 states
constexpr uint8_t G[3] = {0133, 0171, 0165};

// the outputs of one step: input bit u entering register state s (the 6
// previous inputs, the most recent in bit 5)
inline int out_bits(int s, int u, int g) {
    const int reg = (u << 6) | s;           // bit 6 = current input, bit 5 = previous ...
    return __builtin_popcount(reg & G[g]) & 1;
}

// the column permutation of the sub-block interleaver (Table 5.1.4-2)
constexpr int PERM_CC[32] = {1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
                             0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30};

}  // namespace

void conv_encode(const std::vector<uint8_t>& c, std::vector<uint8_t> d[3]) {
    const int n = static_cast<int>(c.size());
    // tail-biting: the register starts with the last 6 inputs
    int s = 0;
    for (int i = 0; i < K - 1; i++) s |= (c[static_cast<size_t>(n - 1 - i)] & 1) << (5 - i);
    for (int g = 0; g < 3; g++) d[g].assign(static_cast<size_t>(n), 0);
    for (int k = 0; k < n; k++) {
        const int u = c[static_cast<size_t>(k)] & 1;
        for (int g = 0; g < 3; g++) d[g][static_cast<size_t>(k)] = static_cast<uint8_t>(out_bits(s, u, g));
        s = (u << 5) | (s >> 1);
    }
}

std::vector<uint8_t> viterbi_tb(const std::vector<float> s[3]) {
    const int n = static_cast<int>(s[0].size());
    if (n == 0) return {};
    const int passes = 3, T = passes * n;
    std::vector<float> pm(NS, 0.0f), nm(NS);
    std::vector<uint8_t> dec(static_cast<size_t>(T) * NS);   // the predecessor's dropped bit per state
    // branch outputs per (state, input)
    int ob[NS][2];
    for (int st = 0; st < NS; st++)
        for (int u = 0; u < 2; u++) ob[st][u] = out_bits(st, u, 0) | (out_bits(st, u, 1) << 1) | (out_bits(st, u, 2) << 2);
    for (int t = 0; t < T; t++) {
        const int k = t % n;
        const float a = s[0][static_cast<size_t>(k)], b = s[1][static_cast<size_t>(k)], c = s[2][static_cast<size_t>(k)];
        std::fill(nm.begin(), nm.end(), -1e30f);
        for (int st = 0; st < NS; st++) {
            for (int u = 0; u < 2; u++) {
                const int o = ob[st][u];
                const float m = pm[static_cast<size_t>(st)] + ((o & 1) ? -a : a) + ((o & 2) ? -b : b) + ((o & 4) ? -c : c);
                const int ns2 = (u << 5) | (st >> 1);
                if (m > nm[static_cast<size_t>(ns2)]) {
                    nm[static_cast<size_t>(ns2)] = m;
                    dec[static_cast<size_t>(t) * NS + static_cast<size_t>(ns2)] = static_cast<uint8_t>(st & 1);
                }
            }
        }
        // keep the numbers small
        const float mx = *std::max_element(nm.begin(), nm.end());
        for (int st = 0; st < NS; st++) pm[static_cast<size_t>(st)] = nm[static_cast<size_t>(st)] - mx;
    }
    // trace back from the best final state; keep the middle pass's bits
    int st = static_cast<int>(std::max_element(pm.begin(), pm.end()) - pm.begin());
    std::vector<uint8_t> all(static_cast<size_t>(T));
    for (int t = T - 1; t >= 0; t--) {
        all[static_cast<size_t>(t)] = static_cast<uint8_t>((st >> 5) & 1);   // the input that led here
        const int dropped = dec[static_cast<size_t>(t) * NS + static_cast<size_t>(st)];
        st = ((st << 1) & (NS - 1)) | dropped;
    }
    return std::vector<uint8_t>(all.begin() + n, all.begin() + 2 * n);
}

std::vector<std::pair<int, int>> conv_rate_match(int D, int E) {
    const int C = 32, R = (D + C - 1) / C, KP = R * C, ND = KP - D;
    // w: the three interleaved streams one after the other; -1 = dummy
    std::vector<std::pair<int, int>> w;
    w.reserve(static_cast<size_t>(3 * KP));
    for (int str = 0; str < 3; str++)
        for (int k = 0; k < KP; k++) {
            const int y = PERM_CC[k / R] + C * (k % R);   // index in the row-wise matrix
            w.push_back(y < ND ? std::make_pair(-1, -1) : std::make_pair(str, y - ND));
        }
    std::vector<std::pair<int, int>> e;
    e.reserve(static_cast<size_t>(E));
    for (size_t j = 0; static_cast<int>(e.size()) < E; j++) {
        const auto& p = w[j % w.size()];
        if (p.first >= 0) e.push_back(p);
    }
    return e;
}

}  // namespace cells::fec
