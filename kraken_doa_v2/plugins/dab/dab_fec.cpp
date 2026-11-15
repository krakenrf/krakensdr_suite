#include "dab_fec.hpp"

#include <algorithm>
#include <cstring>

namespace dab {

// generator polynomials over (a_i, a_i-1, ..., a_i-6) = bits 6..0 (clause 11.1.1)
static const unsigned POLY[4] = {0133, 0171, 0145, 0133};

static inline int parity7(unsigned v) { return __builtin_popcount(v & 0x7F) & 1; }

Viterbi::Viterbi() {
    // butterfly j: states 2j / 2j+1 -> j (input 0) / j + 32 (input 1). Every
    // polynomial taps a_i and a_i-6, so the four branches carry c, ~c, ~c, c
    // with c = the output of (state 2j, input 0)
    for (int j = 0; j < 32; j++)
        for (int r = 0; r < 4; r++) sign_[r][j] = parity7(static_cast<unsigned>(2 * j) & POLY[r]) ? -1 : 1;
}

void Viterbi::decode(const int8_t* soft, int nbits, uint8_t* out) {
    const int steps = nbits + 6;
    if (dec_.size() < static_cast<size_t>(steps) * 64) dec_.resize(static_cast<size_t>(steps) * 64);
    alignas(64) int32_t ma[64], mb[64];
    alignas(64) int32_t sg[4][32];
    for (int r = 0; r < 4; r++)
        for (int j = 0; j < 32; j++) sg[r][j] = sign_[r][j];
    for (int s = 0; s < 64; s++) ma[s] = -(1 << 28);
    ma[0] = 0;
    int32_t* m = ma;
    int32_t* nm = mb;
    for (int t = 0; t < steps; t++) {
        const int32_t s0 = soft[4 * t], s1 = soft[4 * t + 1], s2 = soft[4 * t + 2], s3 = soft[4 * t + 3];
        uint8_t* __restrict d = &dec_[static_cast<size_t>(t) * 64];
        for (int j = 0; j < 32; j++) {
            const int32_t B = sg[0][j] * s0 + sg[1][j] * s1 + sg[2][j] * s2 + sg[3][j] * s3;
            const int32_t a = m[2 * j], b = m[2 * j + 1];
            const int32_t p0 = a + B, p1 = b - B, q0 = a - B, q1 = b + B;
            nm[j] = p1 > p0 ? p1 : p0;
            d[j] = p1 > p0;
            nm[j + 32] = q1 > q0 ? q1 : q0;
            d[j + 32] = q1 > q0;
        }
        std::swap(m, nm);
    }
    // terminated: trace back from state 0
    int s = 0;
    for (int t = steps - 1; t >= 0; t--) {
        const int x = dec_[static_cast<size_t>(t) * 64 + s];
        if (t < nbits) out[t] = static_cast<uint8_t>(s >> 5);
        s = ((s & 31) << 1) | x;
    }
}

void conv_encode(const uint8_t* bits, int n, uint8_t* out) {
    unsigned reg = 0;
    for (int i = 0; i < n + 6; i++) {
        const unsigned a = i < n ? (bits[i] & 1u) : 0u;
        reg = ((a << 6) | (reg >> 1)) & 0x7F;
        for (int r = 0; r < 4; r++) out[4 * i + r] = static_cast<uint8_t>(parity7(reg & POLY[r]));
    }
}

bool Depuncture::build(const Profile& p) {
    pos.clear();
    int m = 0, blocks = 0;
    for (const auto& g : p.blocks) {
        if (g.second < 1 || g.second > 24 || g.first < 0) return false;
        for (int b = 0; b < g.first; b++) {
            for (int sub = 0; sub < 4; sub++)
                for (int i = 0; i < 32; i++) {
                    if (punct_bit(g.second, i)) pos.push_back(m);
                    m++;
                }
            blocks++;
        }
    }
    if (blocks * 32 != p.in_bits) return false;
    // tail: VT = 1100 1100 1100 1100 1100 1100
    for (int i = 0; i < 24; i++) {
        if ((i & 3) < 2) pos.push_back(m);
        m++;
    }
    for (int i = 0; i < p.pad; i++) pos.push_back(-1);
    mother = m;
    in_bits = p.in_bits;
    return true;
}

void Depuncture::apply(const int8_t* rx, int8_t* ms) const {
    std::memset(ms, 0, static_cast<size_t>(mother));
    const size_t n = pos.size();
    const int* __restrict pp = pos.data();
    for (size_t i = 0; i < n; i++)
        if (pp[i] >= 0) ms[pp[i]] = rx[i];
}

const std::vector<uint8_t>& prbs_bytes() {
    static const std::vector<uint8_t> t = [] {
        const int nbits = 43776;
        std::vector<uint8_t> b(nbits / 8, 0);
        unsigned r = 0x1FF;   // 9 stages, all ones
        for (int i = 0; i < nbits; i++) {
            // stage 1 = bit 0 ... stage 9 = bit 8; output = stage 9 xor stage 5
            const unsigned o = ((r >> 8) ^ (r >> 4)) & 1u;
            r = ((r << 1) | o) & 0x1FF;
            if (o) b[static_cast<size_t>(i / 8)] |= static_cast<uint8_t>(0x80 >> (i % 8));
        }
        return b;
    }();
    return t;
}

bool fib_crc_ok(const uint8_t* fib) {
    unsigned r = 0xFFFF;
    for (int i = 0; i < 30; i++) {
        for (int b = 7; b >= 0; b--) {
            const unsigned fb = ((r >> 15) ^ (static_cast<unsigned>(fib[i]) >> b)) & 1u;
            r = (r << 1) & 0xFFFF;
            if (fb) r ^= 0x1021;
        }
    }
    r = ~r & 0xFFFF;
    return r == (static_cast<unsigned>(fib[30]) << 8 | fib[31]);
}

}   // namespace dab
