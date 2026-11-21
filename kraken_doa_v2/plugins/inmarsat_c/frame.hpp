#pragma once
// Inmarsat-C TDM frame layer: unique word, de-interleaver, soft Viterbi,
// descrambler. One frame = 10368 BPSK symbols at 1200 symbols/s (8.64 s)
// carrying 640 bytes (639 information bytes + a zero flush byte).
//
// Structure (Inmarsat-C SDM Vol. 1 Ch. 4 3.1.1 as published, every item
// checked on a real NCS common channel recording - README "Sources"):
// - 640 bytes in 160 groups of 4 bytes; each group is inverted or left as
//   it is by a fixed 160-bit sequence (the same in every frame). Measured:
//   the sequence obeys s[i] = s[i-4] ^ s[i-5] ^ s[i-6] ^ s[i-8] (x^8 + x^4 +
//   x^3 + x^2 + 1) from s[0..7] = 0000 0001.
// - bits sent bit 1 (LSB) to bit 8 of each byte into a rate-1/2 K=7
//   convolutional encoder, G1 = 1011011, G2 = 1111001 (the leftmost tap is
//   the current input; G1's symbol first), starting and ending in state 0:
//   10240 symbols.
// - interleave matrix 64 rows x 162 columns: columns 0 and 1 hold the unique
//   word (one bit per row), the encoder symbols fill columns 2..161 column by
//   column (64 symbols per column). Rows are sent one after the other, in
//   the order i = (j * 39) mod 64 (transmitted row j = matrix row i).
// - the unique word in transmission order: 07EA CDDA 4E2F 28C2, bit 0 -> +1.

#include <array>
#include <cstdint>
#include <vector>

#include "kraken_dsp.hpp"

namespace inmc {

constexpr int SYMBOL_RATE = 1200;
constexpr int ROWS = 64;
constexpr int COLS = 162;
constexpr int FRAME_SYMS = ROWS * COLS;        // 10368 = 8.64 s
constexpr int DATA_SYMS = ROWS * (COLS - 2);   // 10240
constexpr int FRAME_BITS = DATA_SYMS / 2;      // 5120
constexpr int FRAME_BYTES = FRAME_BITS / 8;    // 640
constexpr int INFO_BYTES = FRAME_BYTES - 1;    // 639 (+ the flush byte)
constexpr uint64_t UW_TX = 0x07EACDDA4E2F28C2ULL;
// kp::conv_encode / viterbi_decode convention: bit 0 of a polynomial = the
// newest input bit. SDM G(1) = 1011011 (leftmost = current input) -> 0x6D.
constexpr uint32_t POLY_G1 = 0x6D;
constexpr uint32_t POLY_G2 = 0x4F;

// +1 / -1 of unique-word bit j (transmitted row j), bit 0 -> +1
inline float uw_sign(int j) { return ((UW_TX >> (63 - j)) & 1) ? -1.0f : 1.0f; }

// The 160 group-inversion bits of the scrambler.
inline const std::array<uint8_t, 160>& scramble_groups() {
    static const std::array<uint8_t, 160> g = [] {
        std::array<uint8_t, 160> s{};
        s[7] = 1;
        for (int i = 8; i < 160; i++) s[i] = s[i - 4] ^ s[i - 5] ^ s[i - 6] ^ s[i - 8];
        return s;
    }();
    return g;
}

// Symbol position in the transmitted frame of data symbol e (0..10239).
inline int data_symbol_pos(int e) {
    const int row = e % ROWS, col = 2 + e / ROWS;
    const int j = (row * 23) % ROWS;   // inverse of i = 39 j mod 64
    return j * COLS + col;
}

// Position of the two unique-word symbols of transmitted row j: j*162 + {0,1}.

struct FrameResult {
    std::array<uint8_t, FRAME_BYTES> bytes{};
    int symbol_errors = 0;   // hard decisions the decoded path disagrees with (channel errors corrected)
};

// soft[0..10367]: one frame from its first symbol, polarity resolved
// (> 0 = bit 0), any scale.
inline FrameResult decode_frame(const float* soft) {
    static const std::vector<uint32_t> polys{POLY_G1, POLY_G2};
    static const std::vector<int> pos = [] {
        std::vector<int> p(DATA_SYMS);
        for (int e = 0; e < DATA_SYMS; e++) p[e] = data_symbol_pos(e);
        return p;
    }();
    std::vector<float> d(DATA_SYMS);
    for (int e = 0; e < DATA_SYMS; e++) d[e] = soft[pos[e]];
    const std::vector<uint8_t> bits = kp::viterbi_decode(d.data(), FRAME_BITS, 7, polys, true);
    FrameResult r;
    // re-encode: the number of corrected channel symbols (a quality figure)
    const std::vector<uint8_t> re = kp::conv_encode(bits.data(), FRAME_BITS, 7, polys);
    for (int e = 0; e < DATA_SYMS; e++)
        if ((d[e] < 0) != (re[e] != 0)) r.symbol_errors++;
    const auto& g = scramble_groups();
    for (int k = 0; k < FRAME_BYTES; k++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) v |= static_cast<uint8_t>((bits[k * 8 + b] & 1) << b);   // bit 1 = LSB first
        if (g[k / 4]) v ^= 0xFF;
        r.bytes[k] = v;
    }
    return r;
}

}  // namespace inmc
