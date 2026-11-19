#pragma once
// DAB audio layer: bit reading and the CRCs of the audio standards.
//
//   crc16_ccitt   G(x) = x^16 + x^12 + x^5 + 1, registers preset to 1, the
//                 result complemented (EN 300 401 clause 7.4.5.0 - X-PAD data
//                 groups / dynamic label; TS 102 563 clause 5.2 - au_crc)
//   crc16_mpa     G(x) = x^16 + x^15 + x^2 + 1, preset to 1, not complemented
//                 (TS 103 466 annex B.2 - the Layer II header CRC)
//   crc8_scf      G(x) = x^8 + x^4 + x^3 + x^2 + 1, preset to 0 (TS 103 466
//                 annex B.3 - the ScF-CRC)
//   fire_code     G(x) = x^16 + x^14 + x^13 + x^12 + x^11 + x^5 + x^3 + x^2 + x
//                 + 1, preset to 0 (TS 102 563 clause 5.2 - header_firecode)
// All are the shift register of EN 300 401 annex E, MSb first.

#include <cstddef>
#include <cstdint>

namespace dab::audio {

// MSb-first reader over a byte buffer; reads past the end return zeros and
// set overrun (the caller checks it - all input is hostile)
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), nbits_(n * 8) {}
    uint32_t get(int bits) {   // bits <= 24
        uint32_t v = 0;
        for (int i = 0; i < bits; i++) {
            v <<= 1;
            if (pos_ < nbits_) v |= (d_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
            else overrun_ = true;
            pos_++;
        }
        return v;
    }
    size_t pos() const { return pos_; }
    bool overrun() const { return overrun_; }
private:
    const uint8_t* d_;
    size_t nbits_;
    size_t pos_ = 0;
    bool overrun_ = false;
};

// Generic MSb-first CRC register update over `bits` bits of `v` (bits <= 32)
template <int W>
inline uint32_t crc_bits(uint32_t reg, uint32_t poly, uint32_t v, int bits) {
    const uint32_t top = 1u << (W - 1), mask = (W == 32) ? 0xFFFFFFFFu : ((1u << W) - 1u);
    for (int i = bits - 1; i >= 0; i--) {
        uint32_t in = (v >> i) & 1u;
        uint32_t fb = ((reg & top) ? 1u : 0u) ^ in;
        reg = (reg << 1) & mask;
        if (fb) reg ^= poly;
    }
    return reg;
}

uint16_t crc16_ccitt(const uint8_t* d, size_t n);   // complemented, see above
uint16_t fire_code(const uint8_t* d, size_t n);     // header_firecode over d[0..n)

}   // namespace dab::audio
