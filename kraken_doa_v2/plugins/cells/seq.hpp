#pragma once

// LTE (3GPP TS 36.211) and 5G NR (TS 38.211) downlink sequences the cells
// plugin correlates with: the primary / secondary synchronisation signals
// (PSS / SSS: physical cell ID) and LTE's cell-specific reference signals
// (CRS: in every downlink subframe, scrambled with the cell ID).
//
// Frequency indexes are subcarriers from the carrier's centre (LTE: the DC
// subcarrier is not used, so f = -1, +1 are its neighbours; NR: from the
// SS/PBCH block's subcarrier 120, no gap).

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace cells {

using cd = std::complex<double>;

// x(n+31) Gold sequence c(n), 36.211 7.2 / 38.211 5.2.1 (Nc = 1600)
inline std::vector<uint8_t> gold(uint32_t c_init, int len) {
    constexpr int NC = 1600;
    std::vector<uint8_t> x1(static_cast<size_t>(NC + len + 31), 0), x2(x1.size(), 0), c(static_cast<size_t>(len));
    x1[0] = 1;
    for (int i = 0; i < 31; i++) x2[static_cast<size_t>(i)] = (c_init >> i) & 1;
    for (int n = 0; n + 31 < static_cast<int>(x1.size()); n++) {
        x1[static_cast<size_t>(n + 31)] = (x1[static_cast<size_t>(n + 3)] + x1[static_cast<size_t>(n)]) & 1;
        x2[static_cast<size_t>(n + 31)] =
            (x2[static_cast<size_t>(n + 3)] + x2[static_cast<size_t>(n + 2)] + x2[static_cast<size_t>(n + 1)] + x2[static_cast<size_t>(n)]) & 1;
    }
    for (int n = 0; n < len; n++) c[static_cast<size_t>(n)] = (x1[static_cast<size_t>(n + NC)] + x2[static_cast<size_t>(n + NC)]) & 1;
    return c;
}

namespace lte {

constexpr int NRB_MAX = 110;

// PSS: Zadoff-Chu of root 25 / 29 / 34 (N_ID2 0 / 1 / 2), 62 values,
// n = 0..30 at f = -31..-1, n = 31..61 at f = +1..+31 (36.211 6.11.1)
inline std::vector<cd> pss(int nid2) {
    static const int root[3] = {25, 29, 34};
    const int u = root[nid2];
    std::vector<cd> d(62);
    for (int n = 0; n < 62; n++) {
        const double k = n < 31 ? static_cast<double>(n) * (n + 1) : static_cast<double>(n + 1) * (n + 2);
        d[static_cast<size_t>(n)] = std::polar(1.0, -M_PI * u * k / 63.0);
    }
    return d;
}

// the subcarrier of sync sequence element n (PSS / SSS), DC skipped
constexpr int sync_f(int n) { return n < 31 ? n - 31 : n - 30; }

// SSS (36.211 6.11.2): 62 values, subframe 0 or 5 (they differ)
inline std::vector<cd> sss(int nid1, int nid2, bool sf5) {
    auto mseq = [](int a, int b, int c, int d) {   // x(i+5) = sum of taps, x = 0 0 0 0 1
        std::vector<int> x(31, 0);
        x[4] = 1;
        for (int i = 0; i + 5 < 31; i++) {
            int v = 0;
            for (int t : {a, b, c, d})
                if (t >= 0) v ^= x[static_cast<size_t>(i + t)];
            x[static_cast<size_t>(i + 5)] = v;
        }
        std::vector<int> s(31);
        for (int i = 0; i < 31; i++) s[static_cast<size_t>(i)] = 1 - 2 * x[static_cast<size_t>(i)];
        return s;
    };
    static const std::vector<int> st = mseq(2, 0, -1, -1);    // x(i+5) = x(i+2) + x(i)
    static const std::vector<int> ct = mseq(3, 0, -1, -1);    // x(i+5) = x(i+3) + x(i)
    static const std::vector<int> zt = mseq(4, 2, 1, 0);      // x(i+5) = x(i+4) + x(i+2) + x(i+1) + x(i)
    const int qp = nid1 / 30;
    const int q = (nid1 + qp * (qp + 1) / 2) / 30;
    const int mp = nid1 + q * (q + 1) / 2;
    const int m0 = mp % 31, m1 = (m0 + mp / 31 + 1) % 31;
    std::vector<cd> d(62);
    for (int n = 0; n < 31; n++) {
        const int s0 = st[static_cast<size_t>((n + m0) % 31)], s1 = st[static_cast<size_t>((n + m1) % 31)];
        const int c0 = ct[static_cast<size_t>((n + nid2) % 31)], c1 = ct[static_cast<size_t>((n + nid2 + 3) % 31)];
        const int z0 = zt[static_cast<size_t>((n + (m0 % 8)) % 31)], z1 = zt[static_cast<size_t>((n + (m1 % 8)) % 31)];
        if (!sf5) {
            d[static_cast<size_t>(2 * n)] = s0 * c0;
            d[static_cast<size_t>(2 * n + 1)] = s1 * c1 * z0;
        } else {
            d[static_cast<size_t>(2 * n)] = s1 * c0;
            d[static_cast<size_t>(2 * n + 1)] = s0 * c1 * z1;
        }
    }
    return d;
}

// CRS of one OFDM symbol (slot ns 0..19, symbol l 0..6, normal CP): the
// reference values r(m'), m' = 0 .. 2 NRB_MAX - 1 (centred: independent of
// the carrier's bandwidth, 36.211 6.10.1)
inline std::vector<cd> crs_seq(int pci, int ns, int l) {
    const uint32_t c_init = (1u << 10) * static_cast<uint32_t>(7 * (ns + 1) + l + 1) * static_cast<uint32_t>(2 * pci + 1) +
                            static_cast<uint32_t>(2 * pci) + 1u;   // N_CP = 1 (normal)
    const auto c = gold(c_init, 4 * NRB_MAX);
    std::vector<cd> r(2 * NRB_MAX);
    const double a = 1 / std::sqrt(2.0);
    for (int m = 0; m < 2 * NRB_MAX; m++)
        r[static_cast<size_t>(m)] = cd(a * (1 - 2 * c[static_cast<size_t>(2 * m)]), a * (1 - 2 * c[static_cast<size_t>(2 * m + 1)]));
    return r;
}

// The CRS offset v of antenna port p (0..3) in symbol l (0..6) of slot ns
// (36.211 6.10.1.2), -1 = no CRS of that port in the symbol
inline int crs_v(int port, int l, int ns) {
    switch (port) {
    case 0: return l == 0 ? 0 : l == 4 ? 3 : -1;
    case 1: return l == 0 ? 3 : l == 4 ? 0 : -1;
    case 2: return l == 1 ? 3 * (ns % 2) : -1;
    case 3: return l == 1 ? 3 + 3 * (ns % 2) : -1;
    default: return -1;
    }
}

// subcarrier f (DC skipped) -> k - 6 N_RB (the 36.211 index around the centre)
constexpr int kappa_of(int f) { return f < 0 ? f : f - 1; }

// Is subcarrier f a CRS with offset v in a cell? Its index m' into crs_seq() then.
inline bool crs_at(int f, int v, int pci, int* mp) {
    if (f == 0 || v < 0) return false;
    const int kappa = kappa_of(f);
    const int s = (v + pci % 6) % 6;
    if ((((kappa - s) % 6) + 6) % 6 != 0) return false;
    *mp = (kappa - s) / 6 + NRB_MAX;
    return *mp >= 0 && *mp < 2 * NRB_MAX;
}

// Is subcarrier f (DC skipped) a CRS of antenna port p (0 / 1) in symbol l
// (0 or 4) of a cell? Its index m' into crs_seq() then.
inline bool crs_re(int f, int l, int port, int pci, int* mp) {
    if (f == 0) return false;
    const int kappa = f < 0 ? f : f - 1;   // k - 6 N_RB
    int v;
    if (l == 0) v = port == 0 ? 0 : 3;
    else if (l == 4) v = port == 0 ? 3 : 0;
    else return false;
    const int s = (v + pci % 6) % 6;
    if ((((kappa - s) % 6) + 6) % 6 != 0) return false;
    // m = (k - s) / 6, m' = m + NRB_MAX - N_RB = (kappa - s) / 6 + NRB_MAX (exact: kappa - s is a multiple of 6)
    *mp = (kappa - s) / 6 + NRB_MAX;
    return *mp >= 0 && *mp < 2 * NRB_MAX;
}

}  // namespace lte

namespace nr {

// PSS (38.211 7.4.2.2): BPSK m-sequence, 127 values at f = n - 64 from the
// SS/PBCH block's subcarrier 120 (its subcarriers 56..182)
inline std::vector<double> pss(int nid2) {
    std::vector<int> x(127 + 7, 0);
    const int init[7] = {0, 1, 1, 0, 1, 1, 1};   // x(0)..x(6)
    for (int i = 0; i < 7; i++) x[static_cast<size_t>(i)] = init[i];
    for (int i = 0; i + 7 < 127 + 7; i++) x[static_cast<size_t>(i + 7)] = (x[static_cast<size_t>(i + 4)] + x[static_cast<size_t>(i)]) & 1;
    std::vector<double> d(127);
    for (int n = 0; n < 127; n++) d[static_cast<size_t>(n)] = 1 - 2 * x[static_cast<size_t>((n + 43 * nid2) % 127)];
    return d;
}

// SSS (38.211 7.4.2.3): 127 values at the PSS's subcarriers, N_ID1 0..335
inline std::vector<double> sss(int nid1, int nid2) {
    auto mseq = [](bool second) {
        std::vector<int> x(127 + 7, 0);
        x[0] = 1;
        for (int i = 0; i + 7 < 127 + 7; i++)
            x[static_cast<size_t>(i + 7)] = second ? (x[static_cast<size_t>(i + 1)] + x[static_cast<size_t>(i)]) & 1
                                                   : (x[static_cast<size_t>(i + 4)] + x[static_cast<size_t>(i)]) & 1;
        return x;
    };
    static const std::vector<int> x0 = mseq(false), x1 = mseq(true);
    const int m0 = 15 * (nid1 / 112) + 5 * nid2, m1 = nid1 % 112;
    std::vector<double> d(127);
    for (int n = 0; n < 127; n++)
        d[static_cast<size_t>(n)] = (1 - 2 * x0[static_cast<size_t>((n + m0) % 127)]) * (1 - 2 * x1[static_cast<size_t>((n + m1) % 127)]);
    return d;
}

constexpr int sync_f(int n) { return n - 64; }

}  // namespace nr

}  // namespace cells
