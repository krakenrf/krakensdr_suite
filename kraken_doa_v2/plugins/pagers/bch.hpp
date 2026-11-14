// BCH(31,21) + even parity - the 32-bit codeword of POCSAG (ITU-R M.584) and
// of FLEX. g(x) = x^10+x^9+x^8+x^6+x^5+x^3+1.
//
// POCSAG sends a codeword from bit 31 down: 21 data bits (x^30 first), the 10
// check bits, the even-parity bit last. FLEX sends the same code with every
// field the other way round: its 21 information bits LSB first, then the
// check bits, then the parity bit - so a FLEX word whose bit k is the k-th
// bit received, bit-reversed, is a POCSAG-layout codeword (checked on a real
// FLEX recording: every frame information word decodes with no error).
#pragma once

#include <cstdint>
#include <vector>

namespace pagers {

constexpr uint32_t BCH_G = 0x769;   // degree 10

inline uint32_t bch_syndrome(uint32_t cw31) {
    for (int i = 30; i >= 10; i--)
        if (cw31 & (1u << i)) cw31 ^= BCH_G << (i - 10);
    return cw31 & 0x3FF;
}

// syndrome -> error pattern for every 1- and 2-bit error of the 31-bit word
inline const std::vector<uint32_t>& bch_table() {
    static const std::vector<uint32_t> t = [] {
        std::vector<uint32_t> v(1024, 0);
        for (int i = 0; i < 31; i++) {
            v[bch_syndrome(1u << i)] = 1u << i;
            for (int j = i + 1; j < 31; j++) {
                uint32_t e = (1u << i) | (1u << j);
                v[bch_syndrome(e)] = e;
            }
        }
        return v;
    }();
    return t;
}

// Corrects up to 2 bit errors in a POCSAG-layout codeword (bits 31..1 = the
// BCH word, bit 0 = parity). Returns the number of corrected bits, or -1 if
// uncorrectable. The even-parity bit catches 3-bit errors the BCH decoder
// would "correct" into the wrong word.
inline int bch_correct(uint32_t& cw) {
    uint32_t c31 = cw >> 1;
    uint32_t s = bch_syndrome(c31);
    int fixed = 0;
    if (s) {
        uint32_t e = bch_table()[s];
        if (!e) return -1;
        c31 ^= e;
        fixed = __builtin_popcount(e);
    }
    bool parity_ok = ((__builtin_popcount(c31) + (cw & 1)) & 1) == 0;
    if (!parity_ok && fixed == 2) return -1;
    cw = (c31 << 1) | (__builtin_popcount(c31) & 1);
    return fixed + (parity_ok ? 0 : 1);
}

inline uint32_t reverse32(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

// A FLEX codeword (bit k = the k-th bit received): corrects it in place,
// returns the corrected bit count or -1. Its information = w & 0x1FFFFF.
inline int flex_correct(uint32_t& w) {
    uint32_t cw = reverse32(w);
    int n = bch_correct(cw);
    if (n >= 0) w = reverse32(cw);
    return n;
}

}  // namespace pagers
