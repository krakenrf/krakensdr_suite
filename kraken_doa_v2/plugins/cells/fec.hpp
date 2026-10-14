#pragma once

// Channel coding pieces of LTE (3GPP TS 36.212) the cells plugin decodes -
// and the synthetic generator encodes, so both sides share them:
//   CRCs (5.1.1): CRC16 (PBCH, DCI), CRC24A / 24B (transport blocks)
//   tail-biting convolutional code K = 7, rate 1/3 (5.1.3.1: PBCH, DCI)
//   rate matching for the convolutional code (5.1.4.2: sub-block
//   interleaver, bit collection, circular-buffer bit selection)
// Bits are uint8_t 0 / 1, MSB (the first bit sent) first. Soft bits are
// floats, > 0 = more likely 0.

#include <cstdint>
#include <vector>

namespace cells::fec {

// generator polynomials without the leading D^len term, e.g. CRC16 (D^16 +
// D^12 + D^5 + 1) = 0x1021
constexpr uint32_t CRC16 = 0x1021, CRC24A = 0x864CFB, CRC24B = 0x800063;

// the CRC of n bits (len = 16 / 24): parity bits p_0 .. p_len-1 as an
// integer, p_0 the MSB
inline uint32_t crc(const uint8_t* bits, int n, uint32_t poly, int len) {
    uint32_t r = 0;
    const uint32_t top = 1u << (len - 1), mask = (len == 32) ? 0xFFFFFFFFu : ((1u << len) - 1);
    for (int i = 0; i < n; i++) {
        const uint32_t fb = ((r & top) ? 1u : 0u) ^ (bits[i] & 1u);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return r;
}

// tail-biting convolutional encoder, K = 7, G0 = 133, G1 = 171, G2 = 165
// (octal; the MSB taps the current input): 3 output streams of the input's length
void conv_encode(const std::vector<uint8_t>& c, std::vector<uint8_t> d[3]);

// Tail-biting Viterbi (wrap-around: the trellis is run 3 times round the
// circle, the middle pass kept). s[k][i] = soft bit of stream i at step k.
// Returns the decoded bits.
std::vector<uint8_t> viterbi_tb(const std::vector<float> s[3]);

// Rate matching of the convolutional code: the circular-buffer position
// (w index) of every coded bit, in the order the E bits are sent: e_k is
// stream sel[k].first, bit sel[k].second (d^(stream)_bit). D = coded bits
// per stream.
std::vector<std::pair<int, int>> conv_rate_match(int D, int E);

}  // namespace cells::fec
