#pragma once

// Differential BPSK signalling carriers (DVB-T TPS, ISDB-T TMCC): every
// signalling carrier of a symbol carries the same bit, sent as "the phase
// flips against the previous symbol". Per pair of consecutive symbols the
// carriers' products y(l) y*(l-1) all point the same way - up to the common
// phase step every carrier makes from symbol to symbol (a carrier offset of
// whole carriers times the guard interval, a residual offset). That step
// comes from reference carriers with the same value in every symbol (DVB-T's
// continual pilots: exact), or else from the squared products (modulo pi:
// the caller settles the sign from known bits).

#include "ofdm.hpp"

#include <cmath>
#include <complex>
#include <vector>

namespace sfn {

// soft[j] for symbol j: > 0 = the phase flipped from symbol j - 1 (bit 1),
// magnitude ~ confidence; valid[j] = 0 where symbol j doesn't follow j - 1
// or no carrier was seen in both
inline void dbpsk_soft(const OfdmStandard::SigBurst& s, std::vector<float>* soft, std::vector<char>* valid) {
    const size_t n = s.sig.size();
    soft->assign(n, 0.0f);
    valid->assign(n, 0);
    std::vector<std::complex<double>> d(n, 0.0);
    std::vector<double> e(n, 0.0);
    std::complex<double> sq = 0;   // the squared products, for the step without references
    for (size_t j = 1; j < n; j++) {
        if (!s.follows[j] || s.sig[j].size() != s.sig[j - 1].size()) continue;
        int k = 0;
        for (size_t i = 0; i < s.sig[j].size(); i++) {
            const std::complex<double> a(s.sig[j][i]), b(s.sig[j - 1][i]);
            if (std::norm(a) == 0 || std::norm(b) == 0) continue;
            const std::complex<double> p = a * std::conj(b);
            d[j] += p;
            e[j] += std::abs(p);
            sq += p * p / std::max(std::abs(p), 1e-30);
            k++;
        }
        if (k > 0) (*valid)[j] = 1;
    }
    const std::complex<double> u_sq = std::abs(sq) > 0 ? std::polar(1.0, std::arg(sq) / 2) : std::complex<double>(1, 0);
    for (size_t j = 1; j < n; j++) {
        if (!(*valid)[j]) continue;
        // the reference carriers inside the slice (0 = outside), else the squares
        std::complex<double> u = u_sq, r = 0;
        if (j < s.ref.size())
            for (size_t i = 0; i < s.ref[j].size() && i < s.ref[j - 1].size(); i++)
                r += std::complex<double>(s.ref[j][i]) * std::conj(std::complex<double>(s.ref[j - 1][i]));
        if (std::abs(r) > 0) u = r / std::abs(r);
        (*soft)[j] = static_cast<float>(-(d[j] * std::conj(u)).real() / std::max(e[j], 1e-30));
    }
}

// the hard bits of a run of soft values, MSB first
inline uint32_t hard_bits(const std::vector<float>& soft, size_t from, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (soft[from + static_cast<size_t>(i)] > 0 ? 1u : 0u);
    return v;
}

// a run of n valid symbols from `from`
inline bool all_valid(const std::vector<char>& valid, size_t from, size_t n) {
    if (from + n > valid.size()) return false;
    for (size_t i = from; i < from + n; i++)
        if (!valid[i]) return false;
    return true;
}

}  // namespace sfn
