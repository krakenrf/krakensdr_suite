#include "dig_fec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dig {

uint64_t bits_to_u64(const uint8_t* b, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[i] & 1);
    return v;
}

void u64_to_bits(uint64_t v, int n, uint8_t* out) {
    for (int i = 0; i < n; i++) out[i] = (v >> (n - 1 - i)) & 1;
}

void pack_bits(const uint8_t* b, int nbits, uint8_t* bytes) {
    for (int i = 0; i < nbits / 8; i++) bytes[i] = static_cast<uint8_t>(bits_to_u32(b + 8 * i, 8));
}

int popcount64(uint64_t v) { return __builtin_popcountll(v); }

int BlockCode::decode(uint32_t rx, uint32_t* msg, int max_errors) const {
    int best = 99;
    uint32_t best_m = 0;
    for (uint32_t m = 0; m < cw_.size(); m++) {
        int d = __builtin_popcount(cw_[m] ^ rx);
        if (d < best) {
            best = d;
            best_m = m;
            if (d == 0) break;
        }
    }
    *msg = best_m;
    return best <= max_errors ? best : -1;
}

// ---------------------------------------------------------------------------
// Block codes
// ---------------------------------------------------------------------------
static uint32_t golay23_parity(uint32_t data12) {
    // data * x^11 mod g(x), g = x^11+x^10+x^6+x^5+x^4+x^2+1 (0xC75)
    uint32_t r = data12 << 11;
    for (int i = 22; i >= 11; i--)
        if (r & (1u << i)) r ^= 0xC75u << (i - 11);
    return r & 0x7FF;
}

static uint32_t golay24_enc(uint32_t d) {
    uint32_t p = golay23_parity(d);
    uint32_t cw23 = (d << 11) | p;
    return (cw23 << 1) | (__builtin_popcount(cw23) & 1);
}

const BlockCode& golay24() {
    static const BlockCode c(24, 12, golay24_enc);
    return c;
}
const BlockCode& golay20() {
    static const BlockCode c(20, 8, [](uint32_t d) { return golay24_enc(d) & 0xFFFFF; });
    return c;
}
const BlockCode& golay18() {
    static const BlockCode c(18, 6, [](uint32_t d) { return golay24_enc(d) & 0x3FFFF; });
    return c;
}

const BlockCode& qr16_7() {
    // cyclic (15,7) with g = x^8+x^5+x^4+x^3+1 (0x139) + overall parity
    static const BlockCode c(16, 7, [](uint32_t d) {
        uint32_t r = d << 8;
        for (int i = 14; i >= 8; i--)
            if (r & (1u << i)) r ^= 0x139u << (i - 8);
        uint32_t cw15 = (d << 8) | (r & 0xFF);
        return (cw15 << 1) | (__builtin_popcount(cw15) & 1);
    });
    return c;
}

// Hamming-style codes given as parity equations over the message bits
// d[0..k-1] (d[0] = first transmitted bit). Parity bit j = XOR of d[i] for
// each i listed in eq[j]; parity bits follow the message.
template <size_t NP>
static uint32_t parity_encode(uint32_t m, int k, const std::array<std::vector<int>, NP>& eq) {
    uint32_t cw = m;
    for (size_t j = 0; j < NP; j++) {
        int p = 0;
        for (int i : eq[j]) p ^= (m >> (k - 1 - i)) & 1;
        cw = (cw << 1) | p;
    }
    return cw;
}

const BlockCode& hamming_15_11() {
    static const std::array<std::vector<int>, 4> eq = {{
        {0, 1, 2, 3, 5, 7, 8}, {1, 2, 3, 4, 6, 8, 9}, {2, 3, 4, 5, 7, 9, 10}, {0, 1, 2, 4, 6, 7, 10}}};
    static const BlockCode c(15, 11, [](uint32_t m) { return parity_encode(m, 11, eq); });
    return c;
}
const BlockCode& hamming_13_9() {
    static const std::array<std::vector<int>, 4> eq = {{
        {0, 1, 3, 5, 6}, {0, 1, 2, 4, 6, 7}, {0, 1, 2, 3, 5, 7, 8}, {0, 2, 4, 5, 8}}};
    static const BlockCode c(13, 9, [](uint32_t m) { return parity_encode(m, 9, eq); });
    return c;
}
const BlockCode& hamming_16_11() {
    static const std::array<std::vector<int>, 5> eq = {{
        {0, 1, 2, 3, 5, 7, 8}, {1, 2, 3, 4, 6, 8, 9}, {2, 3, 4, 5, 7, 9, 10},
        {0, 1, 2, 4, 6, 7, 10}, {0, 2, 5, 6, 8, 9, 10}}};
    static const BlockCode c(16, 11, [](uint32_t m) { return parity_encode(m, 11, eq); });
    return c;
}
const BlockCode& hamming_10_6() {
    static const std::array<std::vector<int>, 4> eq = {{
        {0, 1, 2, 5}, {0, 1, 3, 5}, {0, 2, 3, 4}, {1, 2, 3, 4}}};
    static const BlockCode c(10, 6, [](uint32_t m) { return parity_encode(m, 6, eq); });
    return c;
}
const BlockCode& hamming_7_4() {
    // ETSI TS 102 361-1 Annex B.3.5 (CACH TACT): h2 = d0^d1^d2,
    // h1 = d1^d2^d3, h0 = d0^d1^d3
    static const std::array<std::vector<int>, 3> eq = {{{0, 1, 2}, {1, 2, 3}, {0, 1, 3}}};
    static const BlockCode c(7, 4, [](uint32_t m) { return parity_encode(m, 4, eq); });
    return c;
}

const BlockCode& rm_30_14() {
    // EN 300 392-2 8.2.3.2: systematic, 14 message bits then 16 parity bits
    static const uint8_t gen[14][16] = {
        {1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0},
        {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}, {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0},
        {1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 0}, {0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0},
        {0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0}, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1},
        {1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1}, {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1},
        {0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1}, {0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1},
        {0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 1, 1}, {0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1}};
    static const BlockCode c(30, 14, [](uint32_t m) {
        uint32_t par = 0;
        for (int i = 0; i < 14; i++) {
            if (!((m >> (13 - i)) & 1)) continue;
            uint32_t row = 0;
            for (int j = 0; j < 16; j++) row = (row << 1) | gen[i][j];
            par ^= row;
        }
        return (m << 16) | par;
    });
    return c;
}

// ---------------------------------------------------------------------------
// Reed-Solomon GF(64)
// ---------------------------------------------------------------------------
namespace {
struct GF64 {
    uint8_t exp[128];
    uint8_t log[64];
    GF64() {
        int x = 1;
        for (int i = 0; i < 63; i++) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x40) x ^= 0x43;
        }
        for (int i = 63; i < 128; i++) exp[i] = exp[i - 63];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 63 - log[b]) % 63] : 0; }
    uint8_t pw(int e) const { e %= 63; if (e < 0) e += 63; return exp[e]; }
};
const GF64& gf64() { static const GF64 g; return g; }
}  // namespace

void rs64_encode(uint8_t* sym, int n, int nroots) {
    const GF64& g = gf64();
    // generator g(x) = prod_{i=1..nroots} (x - a^i)
    std::vector<uint8_t> gen(nroots + 1, 0);
    gen[0] = 1;
    for (int i = 1; i <= nroots; i++) {
        uint8_t r = g.pw(i);
        for (int j = i; j > 0; j--) gen[j] = gen[j - 1] ^ g.mul(gen[j], r);
        gen[0] = g.mul(gen[0], r);
    }
    // gen[j] is the coefficient of x^j; systematic division
    int k = n - nroots;
    std::vector<uint8_t> rem(nroots, 0);   // rem[0] = highest degree
    for (int i = 0; i < k; i++) {
        uint8_t fb = sym[i] ^ rem[0];
        for (int j = 0; j < nroots - 1; j++) rem[j] = rem[j + 1] ^ g.mul(fb, gen[nroots - 1 - j]);
        rem[nroots - 1] = g.mul(fb, gen[0]);
    }
    for (int j = 0; j < nroots; j++) sym[k + j] = rem[j];
}

int rs64_decode(uint8_t* sym, int n, int nroots) {
    const GF64& g = gf64();
    // syndromes S_j = r(a^(j+1)), r(x) = sum sym[i] x^(n-1-i)
    std::vector<uint8_t> S(nroots, 0);
    bool clean = true;
    for (int j = 0; j < nroots; j++) {
        uint8_t s = 0;
        uint8_t a = g.pw(j + 1);
        for (int i = 0; i < n; i++) s = g.mul(s, a) ^ sym[i];
        S[j] = s;
        if (s) clean = false;
    }
    if (clean) return 0;
    // Berlekamp-Massey
    std::vector<uint8_t> C(nroots + 1, 0), B(nroots + 1, 0), T;
    C[0] = B[0] = 1;
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int r = 0; r < nroots; r++) {
        uint8_t d = S[r];
        for (int i = 1; i <= L; i++) d ^= g.mul(C[i], S[r - i]);
        if (d == 0) { m++; continue; }
        T = C;
        uint8_t coef = g.div(d, b);
        for (int i = m; i <= nroots; i++) C[i] ^= g.mul(coef, B[i - m]);
        if (2 * L <= r) {
            L = r + 1 - L;
            B = T;
            b = d;
            m = 1;
        } else {
            m++;
        }
    }
    if (L == 0 || 2 * L > nroots) return -1;
    // Chien search over the n positions; position i has locator X = a^(n-1-i)
    std::vector<int> pos;
    for (int i = 0; i < n; i++) {
        int e = n - 1 - i;
        uint8_t xinv = g.pw(-e);
        uint8_t v = 0;
        uint8_t p = 1;
        for (int j = 0; j <= L; j++) { v ^= g.mul(C[j], p); p = g.mul(p, xinv); }
        if (v == 0) pos.push_back(i);
    }
    if (static_cast<int>(pos.size()) != L) return -1;
    // Forney: error evaluator Omega = S(x) C(x) mod x^nroots
    std::vector<uint8_t> Om(nroots, 0);
    for (int i = 0; i < nroots; i++)
        for (int j = 0; j <= std::min(i, L); j++) Om[i] ^= g.mul(C[j], S[i - j]);
    for (int i : pos) {
        int e = n - 1 - i;
        uint8_t X = g.pw(e), xinv = g.pw(-e);
        uint8_t num = 0, p = 1;
        for (int j = 0; j < nroots; j++) { num ^= g.mul(Om[j], p); p = g.mul(p, xinv); }
        // C'(x^-1): odd terms
        uint8_t den = 0;
        for (int j = 1; j <= L; j += 2) den ^= g.mul(C[j], g.pw(-e * (j - 1)));
        if (den == 0) return -1;
        // fcr = 1: e = X^(1-fcr) * Omega(X^-1) / C'(X^-1) = Omega/C'
        (void)X;
        sym[i] ^= g.div(num, den);
    }
    return L;
}

// DMR RS(12,9), GF(256) poly 0x11D, generator x^3 + 14x^2 + 56x + 64
bool rs129_check(const uint8_t* d) {
    static uint8_t exp_t[512], log_t[256];
    static bool init = false;
    if (!init) {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp_t[i] = static_cast<uint8_t>(x);
            log_t[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp_t[i] = exp_t[i - 255];
        init = true;
    }
    auto mul = [&](uint8_t a, uint8_t b) -> uint8_t {
        return (a && b) ? exp_t[log_t[a] + log_t[b]] : 0;
    };
    static const uint8_t poly[3] = {64, 56, 14};
    uint8_t par[3] = {0, 0, 0};
    for (int i = 0; i < 9; i++) {
        uint8_t db = d[i] ^ par[2];
        par[2] = par[1] ^ mul(poly[2], db);
        par[1] = par[0] ^ mul(poly[1], db);
        par[0] = mul(poly[0], db);
    }
    return d[9] == par[2] && d[10] == par[1] && d[11] == par[0];
}

// ---------------------------------------------------------------------------
// CRCs
// ---------------------------------------------------------------------------
uint16_t crc16_ccitt_bits(const uint8_t* bits, int nbits, uint16_t crc) {
    for (int i = 0; i < nbits; i++) {
        crc ^= static_cast<uint16_t>((bits[i] & 1) << 15);
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

uint16_t crc16_x25(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
    }
    return static_cast<uint16_t>(~crc);
}

uint16_t crc16_ccitt_bytes(const uint8_t* data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return static_cast<uint16_t>(~crc);
}

uint32_t crc_bits_ones(const uint8_t* bits, int nbits, int width, uint32_t poly) {
    const uint32_t mask = (1u << width) - 1, top = 1u << (width - 1);
    uint32_t crc = mask;
    for (int i = 0; i < nbits; i++) {
        bool fb = ((crc & top) != 0) ^ (bits[i] & 1);
        crc = (crc << 1) & mask;
        if (fb) crc ^= poly;
    }
    return crc & mask;
}

int dmr_crc5(const uint8_t* lc) {
    int total = 0;
    for (int i = 0; i < 9; i++) total += static_cast<int>(bits_to_u32(lc + 8 * i, 8));
    return total % 31;
}

// ---------------------------------------------------------------------------
// Convolutional codes
// ---------------------------------------------------------------------------
Bits conv_encode(const uint8_t* bits, int n, int K, const std::vector<uint32_t>& polys) {
    Bits out;
    out.reserve(static_cast<size_t>(n) * polys.size());
    uint32_t reg = 0;
    const uint32_t mask = (1u << K) - 1;
    for (int i = 0; i < n; i++) {
        reg = ((reg << 1) | (bits[i] & 1)) & mask;
        for (uint32_t p : polys) out.push_back(__builtin_popcount(reg & p) & 1);
    }
    return out;
}

Bits viterbi_decode(const float* soft, int nout, int K, const std::vector<uint32_t>& polys,
                    bool terminated) {
    const int nstates = 1 << (K - 1);
    const int R = static_cast<int>(polys.size());
    const float INF = std::numeric_limits<float>::max() / 4;
    std::vector<float> metric(nstates, INF), next(nstates);
    metric[0] = 0.0f;
    // decision[t][state] = bit shifted out (the oldest register bit) of the
    // survivor predecessor
    std::vector<uint8_t> decision(static_cast<size_t>(nout) * nstates);
    // precompute expected outputs for (state, input)
    std::vector<uint32_t> outbits(nstates * 2);
    for (int s = 0; s < nstates; s++)
        for (int in = 0; in < 2; in++) {
            uint32_t reg = (static_cast<uint32_t>(s) << 1) | in;
            uint32_t o = 0;
            for (int j = 0; j < R; j++) o |= (__builtin_popcount(reg & polys[j]) & 1) << j;
            outbits[s * 2 + in] = o;
        }
    for (int t = 0; t < nout; t++) {
        std::fill(next.begin(), next.end(), INF);
        const float* y = soft + static_cast<size_t>(t) * R;
        for (int s = 0; s < nstates; s++) {
            if (metric[s] >= INF) continue;
            for (int in = 0; in < 2; in++) {
                uint32_t o = outbits[s * 2 + in];
                float bm = 0.0f;
                // correlation metric: expected +1 for bit 0, -1 for bit 1
                for (int j = 0; j < R; j++) bm -= ((o >> j) & 1) ? -y[j] : y[j];
                int ns = ((s << 1) | in) & (nstates - 1);
                float m = metric[s] + bm;
                if (m < next[ns]) {
                    next[ns] = m;
                    decision[static_cast<size_t>(t) * nstates + ns] = static_cast<uint8_t>((s >> (K - 2)) & 1);
                }
            }
        }
        metric.swap(next);
    }
    int s = 0;
    if (!terminated) s = static_cast<int>(std::min_element(metric.begin(), metric.end()) - metric.begin());
    Bits out(nout);
    for (int t = nout - 1; t >= 0; t--) {
        out[t] = s & 1;
        int old = decision[static_cast<size_t>(t) * nstates + s];
        s = (s >> 1) | (old << (K - 2));
    }
    return out;
}

bool p25_trellis_half(const float* sym98, uint8_t* out12, float* metric_out) {
    static const uint8_t ilv[98] = {
        0, 1, 8, 9, 16, 17, 24, 25, 32, 33, 40, 41, 48, 49, 56, 57, 64, 65, 72, 73, 80, 81, 88, 89, 96, 97,
        2, 3, 10, 11, 18, 19, 26, 27, 34, 35, 42, 43, 50, 51, 58, 59, 66, 67, 74, 75, 82, 83, 90, 91,
        4, 5, 12, 13, 20, 21, 28, 29, 36, 37, 44, 45, 52, 53, 60, 61, 68, 69, 76, 77, 84, 85, 92, 93,
        6, 7, 14, 15, 22, 23, 30, 31, 38, 39, 46, 47, 54, 55, 62, 63, 70, 71, 78, 79, 86, 87, 94, 95};
    static const uint8_t enc[16] = {0, 15, 12, 3, 4, 11, 8, 7, 13, 2, 1, 14, 9, 6, 5, 10};
    static const int8_t pts[16][2] = {
        {+1, -1}, {-1, -1}, {+3, -3}, {-3, -3}, {-3, -1}, {+3, -1}, {-1, -3}, {+1, -3},
        {-3, +3}, {+3, +3}, {-1, +1}, {+1, +1}, {+1, +3}, {-1, +3}, {+3, +1}, {-3, +1}};
    float d[98];
    for (int i = 0; i < 98; i++) d[ilv[i]] = sym98[i];
    const float INF = 1e30f;
    float metric[4] = {0, INF, INF, INF};
    uint8_t prev[49][4];
    for (int t = 0; t < 49; t++) {
        float nm[4] = {INF, INF, INF, INF};
        for (int s = 0; s < 4; s++) {
            if (metric[s] >= INF) continue;
            for (int in = 0; in < 4; in++) {
                if (t == 48 && in != 0) continue;   // flush dibit
                const int8_t* p = pts[enc[s * 4 + in]];
                float e0 = d[2 * t] - p[0], e1 = d[2 * t + 1] - p[1];
                float m = metric[s] + e0 * e0 + e1 * e1;
                if (m < nm[in]) { nm[in] = m; prev[t][in] = static_cast<uint8_t>(s); }
            }
        }
        std::copy(nm, nm + 4, metric);
    }
    // traceback from state 0 (final input 0)
    uint8_t dib[49];
    int s = 0;
    for (int t = 48; t >= 0; t--) { dib[t] = static_cast<uint8_t>(s); s = prev[t][s]; }
    for (int i = 0; i < 12; i++)
        out12[i] = static_cast<uint8_t>((dib[4 * i] << 6) | (dib[4 * i + 1] << 4) | (dib[4 * i + 2] << 2) | dib[4 * i + 3]);
    if (metric_out) *metric_out = metric[0];
    return true;
}

// ---------------------------------------------------------------------------
// DMR BPTC / embedded LC
// ---------------------------------------------------------------------------
int bptc_196_96(const uint8_t* raw, uint8_t* out) {
    uint8_t d[196];
    for (int a = 0; a < 196; a++) d[a] = raw[(a * 181) % 196];
    int fixed = 0;
    bool ok = false;
    for (int pass = 0; pass < 5; pass++) {
        bool changed = false;
        ok = true;
        // columns: Hamming(13,9)
        for (int c = 0; c < 15; c++) {
            uint32_t w = 0;
            for (int a = 0; a < 13; a++) w = (w << 1) | d[c + 1 + 15 * a];
            uint32_t m;
            int e = hamming_13_9().decode(w, &m, 1);
            if (e < 0) { ok = false; continue; }
            if (e > 0) {
                uint32_t cw = hamming_13_9().encode(m);
                for (int a = 0; a < 13; a++) d[c + 1 + 15 * a] = (cw >> (12 - a)) & 1;
                fixed += e;
                changed = true;
            }
        }
        // rows 0..8: Hamming(15,11)
        for (int r = 0; r < 9; r++) {
            uint8_t* p = d + r * 15 + 1;
            uint32_t w = bits_to_u32(p, 15), m;
            int e = hamming_15_11().decode(w, &m, 1);
            if (e < 0) { ok = false; continue; }
            if (e > 0) {
                u64_to_bits(hamming_15_11().encode(m), 15, p);
                fixed += e;
                changed = true;
            }
        }
        if (!changed) break;
    }
    if (!ok) return -1;
    int n = 0;
    for (int a = 4; a <= 11; a++) out[n++] = d[a];
    for (int r = 1; r < 9; r++)
        for (int a = r * 15 + 1; a <= r * 15 + 11; a++) out[n++] = d[a];
    return fixed;
}

bool dmr_embedded_lc(const uint8_t* raw, uint8_t* out72) {
    uint8_t d[128] = {0};
    int b = 0;
    for (int a = 0; a < 128; a++) {
        d[b] = raw[a];
        b += 16;
        if (b > 127) b -= 127;
    }
    for (int r = 0; r < 7; r++) {
        uint8_t* p = d + 16 * r;
        uint32_t m;
        int e = hamming_16_11().decode(bits_to_u32(p, 16), &m, 1);
        if (e < 0) return false;
        if (e > 0) u64_to_bits(hamming_16_11().encode(m), 16, p);
    }
    for (int c = 0; c < 16; c++) {
        int par = 0;
        for (int r = 0; r < 8; r++) par ^= d[c + 16 * r];
        if (par) return false;
    }
    int n = 0;
    for (int a = 0; a < 11; a++) out72[n++] = d[a];
    for (int a = 16; a < 27; a++) out72[n++] = d[a];
    for (int r = 2; r < 7; r++)
        for (int a = 16 * r; a < 16 * r + 10; a++) out72[n++] = d[a];
    int crc = (d[42] << 4) | (d[58] << 3) | (d[74] << 2) | (d[90] << 1) | d[106];
    return dmr_crc5(out72) == crc;
}

// ---------------------------------------------------------------------------
// TETRA
// ---------------------------------------------------------------------------
uint32_t tetra_scramb_init(uint32_t mcc, uint32_t mnc, uint32_t cc) {
    uint32_t v = (cc & 0x3F) | ((mnc & 0x3FFF) << 6) | ((mcc & 0x3FF) << 20);
    return (v << 2) | 3;
}

void tetra_scramble(uint8_t* bits, int n, uint32_t lfsr) {
    // taps 32 26 23 22 16 12 11 10 8 7 5 4 2 1 (EN 300 392-2 8.2.5)
    auto st = [](uint32_t x, int y) { return (x >> (32 - y)) & 1; };
    for (int i = 0; i < n; i++) {
        uint32_t bit = st(lfsr, 32) ^ st(lfsr, 26) ^ st(lfsr, 23) ^ st(lfsr, 22) ^ st(lfsr, 16) ^
                       st(lfsr, 12) ^ st(lfsr, 11) ^ st(lfsr, 10) ^ st(lfsr, 8) ^ st(lfsr, 7) ^
                       st(lfsr, 5) ^ st(lfsr, 4) ^ st(lfsr, 2) ^ st(lfsr, 1);
        lfsr = (lfsr >> 1) | (bit << 31);
        bits[i] ^= static_cast<uint8_t>(bit);
    }
}

void tetra_block_deinterleave(const uint8_t* in, uint8_t* out, int K, int a) {
    for (int i = 1; i <= K; i++) {
        int k = 1 + (a * i) % K;
        out[i - 1] = in[k - 1];
    }
}

Bits tetra_rcpc23_decode(const float* soft3, int n3, int n2) {
    // depuncture rate 2/3 (P = 1,2,5, t = 3, period 8) into the rate-1/4
    // mother code, then Viterbi K=5 with G1..G4 (EN 300 392-2 8.2.3.1)
    std::vector<float> mother(static_cast<size_t>(n2) * 4, 0.0f);
    static const int P[4] = {0, 1, 2, 5};
    for (int j = 1; j <= n3; j++) {
        int k = 8 * ((j - 1) / 3) + P[j - 3 * ((j - 1) / 3)];
        if (k - 1 < static_cast<int>(mother.size())) mother[k - 1] = soft3[j - 1];
    }
    return viterbi_decode(mother.data(), n2, 5, {0x13, 0x1D, 0x17, 0x1B}, true);
}

bool tetra_crc_ok(const uint8_t* type2, int k1) {
    return crc16_ccitt_bits(type2, k1 + 16, 0xFFFF) == 0x1D0F;
}

}  // namespace dig
