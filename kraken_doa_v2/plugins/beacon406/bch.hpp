#pragma once

// The two BCH codes of a first-generation 406 MHz message (C/S T.001 Annex B):
// BCH-1 (82,61) over bits 25-106 (shortened from (127,106), corrects 3
// errors), BCH-2 (38,26) over bits 107-144 (shortened from (63,51), corrects
// 2). Decoded by syndrome tables: every pattern of <= t errors has its own
// syndrome (minimum distance 7 / 5), so the table lookup is exact.

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace b406 {

class Bch {
public:
    // gen: generator polynomial with its x^deg term (bit deg), n: code length
    Bch(uint32_t gen, int deg, int n, int t) : gen_(gen), deg_(deg), n_(n), t_(t) {
        syn1_.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            std::vector<uint8_t> e(static_cast<size_t>(n), 0);
            e[static_cast<size_t>(i)] = 1;
            syn1_[static_cast<size_t>(i)] = syndrome(e.data());
        }
        for (int i = 0; i < n; i++) {
            tab_[syn1_[static_cast<size_t>(i)]] = pack(i, -1);
            for (int j = i + 1; j < n; j++)
                tab_[syn1_[static_cast<size_t>(i)] ^ syn1_[static_cast<size_t>(j)]] = pack(i, j);
        }
    }
    int n() const { return n_; }
    // remainder of the codeword (bits[0] = highest power) divided by gen
    uint32_t syndrome(const uint8_t* bits) const {
        uint32_t r = 0;
        const uint32_t top = 1u << deg_;
        for (int i = 0; i < n_; i++) {
            r = (r << 1) | (bits[i] & 1);
            if (r & top) r ^= gen_;
        }
        return r;
    }
    // parity bits for k = n - deg data bits
    uint32_t parity(const uint8_t* data) const {
        uint32_t r = 0;
        const uint32_t top = 1u << deg_;
        for (int i = 0; i < n_ - deg_; i++) {
            r = (r << 1) | (data[i] & 1);
            if (r & top) r ^= gen_;
        }
        for (int i = 0; i < deg_; i++) {
            r <<= 1;
            if (r & top) r ^= gen_;
        }
        return r;
    }
    // corrects bits in place: the number of bits corrected, -1 = uncorrectable
    int correct(uint8_t* bits) const {
        const uint32_t s = syndrome(bits);
        if (s == 0) return 0;
        auto it = tab_.find(s);
        if (it != tab_.end()) {
            const int i = static_cast<int>(it->second & 0xFFFF), j = static_cast<int>(it->second >> 16) - 1;
            bits[i] ^= 1;
            if (j >= 0) bits[j] ^= 1;
            return j >= 0 ? 2 : 1;
        }
        if (t_ < 3) return -1;
        for (int k = 0; k < n_; k++) {
            auto jt = tab_.find(s ^ syn1_[static_cast<size_t>(k)]);
            if (jt == tab_.end()) continue;
            const int i = static_cast<int>(jt->second & 0xFFFF), j = static_cast<int>(jt->second >> 16) - 1;
            if (j < 0 || i == k || j == k) continue;
            bits[i] ^= 1;
            bits[j] ^= 1;
            bits[k] ^= 1;
            return 3;
        }
        return -1;
    }

private:
    uint32_t gen_;
    int deg_, n_, t_;
    std::vector<uint32_t> syn1_;
    std::unordered_map<uint32_t, uint32_t> tab_;
    static uint32_t pack(int i, int j) { return static_cast<uint32_t>(i) | (static_cast<uint32_t>(j + 1) << 16); }
};

// BCH-1: x^21+x^18+x^17+x^15+x^14+x^12+x^11+x^8+x^7+x^6+x^5+x+1
inline const Bch& bch1() {
    static const Bch b(0b1001101101100111100011u, 21, 82, 3);
    return b;
}
// BCH-2: x^12+x^10+x^8+x^5+x^4+x^3+1
inline const Bch& bch2() {
    static const Bch b(0b1010100111001u, 12, 38, 2);
    return b;
}

}  // namespace b406
