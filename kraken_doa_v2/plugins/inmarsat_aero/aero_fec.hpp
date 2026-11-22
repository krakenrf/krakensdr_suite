#pragma once

// Inmarsat Classic Aero channel coding (ICAO Annex 10 Vol III, Part I,
// chapter 4, 4.4): the rate 1/2 K = 7 convolutional code (G1 = 1 + X^2 +
// X^3 + X^5 + X^6 = 133 octal, G2 = 1 + X + X^2 + X^3 + X^6 = 171 octal,
// G1's bit first; 8.4 kbit/s C channel punctured to 2/3: G1 G2 G1), the
// 64-row block interleaver (rows permuted j = 27 i mod 64), the 15-stage
// scrambler (1 + X + X^15, preset 1101 0010 1011 001) and the signal
// units' CRC (x^16 + x^12 + x^5 + 1 as in X.25: CRC-16/X-25).

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace aero {

// tap masks over (input, D, D^2, ...): bit k = delay k
constexpr uint32_t G1 = 0x6D;   // 1 + X^2 + X^3 + X^5 + X^6
constexpr uint32_t G2 = 0x4F;   // 1 + X + X^2 + X^3 + X^6

inline int parity7(uint32_t v) { return __builtin_popcount(v) & 1; }

// Streaming soft-decision Viterbi decoder for the K = 7 code. Soft input
// per coded bit: > 0 = "0", < 0 = "1", 0 = erasure (punctured). Decided
// bits come out TRACE bits late (push() returns how many are ready, read
// them with take()); flush() traces back from the best state and releases
// everything. Starts with every state equally likely (a continuous channel
// joined at any point).
class Viterbi {
public:
    static constexpr int NS = 64;
    static constexpr int TRACE = 64;     // decision depth before a bit is released
    static constexpr int CHUNK = 64;     // bits released per traceback
    Viterbi() {
        for (int s = 0; s < NS; s++)
            for (int b = 0; b < 2; b++) {
                const uint32_t reg = (static_cast<uint32_t>(s) << 1) | static_cast<uint32_t>(b);
                out_[s][b][0] = parity7(reg & G1);
                out_[s][b][1] = parity7(reg & G2);
            }
        reset();
    }
    // zero_start: the encoder starts from state 0 (bursts: the encoder is
    // reset at the start of each burst's information field)
    void reset(bool zero_start = false) {
        pm_.fill(zero_start ? -1e6f : 0.0f);
        pm_[0] = 0;
        hist_.clear();
        out_bits_.clear();
        nsteps_ = 0;
    }
    // one coded pair (G1, G2)
    void push(float s1, float s2) {
        std::array<float, NS> nm;
        uint64_t dec = 0;
        // state s = last six inputs, bit 0 the newest; next state ns =
        // ((s << 1) | b) & 63; ns has two predecessors: (ns >> 1) | (x << 5)
        for (int ns = 0; ns < NS; ns++) {
            const int b = ns & 1;
            const int p0 = ns >> 1, p1 = p0 | 32;
            const float m0 = pm_[p0] + (out_[p0][b][0] ? -s1 : s1) + (out_[p0][b][1] ? -s2 : s2);
            const float m1 = pm_[p1] + (out_[p1][b][0] ? -s1 : s1) + (out_[p1][b][1] ? -s2 : s2);
            if (m1 > m0) {
                nm[ns] = m1;
                dec |= 1ull << ns;
            } else {
                nm[ns] = m0;
            }
        }
        float mx = nm[0];
        for (int s = 1; s < NS; s++) mx = std::max(mx, nm[s]);
        for (int s = 0; s < NS; s++) pm_[s] = nm[s] - mx;
        hist_.push_back(dec);
        nsteps_++;
        if (static_cast<int>(hist_.size()) >= TRACE + CHUNK) traceback(CHUNK);
    }
    // release everything still held (end of a burst / loss of sync)
    void flush() {
        if (!hist_.empty()) traceback(static_cast<int>(hist_.size()));
    }
    // the decisions not released yet, traced back from the best state now
    // (no look-ahead for the newest ones), WITHOUT releasing them: a frame
    // can be decoded the moment it is in, instead of when the next one is
    void peek(std::vector<uint8_t>& dst) const {
        dst.clear();
        int s = 0;
        for (int k = 1; k < NS; k++)
            if (pm_[k] > pm_[s]) s = k;
        const int L = static_cast<int>(hist_.size());
        dst.resize(static_cast<size_t>(L));
        for (int t = L - 1; t >= 0; t--) {
            dst[static_cast<size_t>(t)] = static_cast<uint8_t>(s & 1);
            const int x = static_cast<int>((hist_[static_cast<size_t>(t)] >> s) & 1);
            s = (s >> 1) | (x << 5);
        }
    }
    size_t ready() const { return out_bits_.size(); }
    // moves the decided bits out (oldest first)
    void take(std::vector<uint8_t>& dst) {
        dst.insert(dst.end(), out_bits_.begin(), out_bits_.end());
        out_bits_.clear();
    }
    uint64_t steps() const { return nsteps_; }   // pairs pushed since reset
    // the best path metric minus the runner-up's, scaled by the steps: a
    // quality figure (not used for decisions)
private:
    std::array<std::array<std::array<uint8_t, 2>, 2>, NS> out_{};
    std::array<float, NS> pm_{};
    std::vector<uint64_t> hist_;
    std::vector<uint8_t> out_bits_;
    uint64_t nsteps_ = 0;

    // trace back through the whole history from the best state and output
    // its oldest `n` decisions
    void traceback(int n) {
        int s = 0;
        for (int k = 1; k < NS; k++)
            if (pm_[k] > pm_[s]) s = k;
        const int L = static_cast<int>(hist_.size());
        std::vector<uint8_t> bits(L);
        for (int t = L - 1; t >= 0; t--) {
            bits[t] = static_cast<uint8_t>(s & 1);
            const int x = static_cast<int>((hist_[t] >> s) & 1);
            s = (s >> 1) | (x << 5);
        }
        out_bits_.insert(out_bits_.end(), bits.begin(), bits.begin() + n);
        hist_.erase(hist_.begin(), hist_.begin() + n);
    }
};

// rate 1/2 encoder (generator, tests): appends G1, G2 per input bit
struct Encoder {
    uint32_t reg = 0;
    void push(int b, std::vector<uint8_t>& out) {
        reg = ((reg << 1) | static_cast<uint32_t>(b & 1)) & 0x7F;
        out.push_back(static_cast<uint8_t>(parity7(reg & G1)));
        out.push_back(static_cast<uint8_t>(parity7(reg & G2)));
    }
};

// the scrambler sequence (XORed onto the information bits before FEC);
// reset at the start of each frame's / burst's information field
class Scrambler {
public:
    Scrambler() { reset(); }
    void reset() { st_ = 0x4D4B; }   // stages 1..15 = 1101 0010 1011 001 (stage 1 = bit 0)
    int next() {
        const int fb = (st_ & 1) ^ ((st_ >> 14) & 1);
        st_ = ((st_ << 1) | static_cast<uint32_t>(fb)) & 0x7FFF;
        return fb;
    }
private:
    uint32_t st_ = 0;
};

// CRC-16/X-25 over bits in transmission order (bit 1 of each octet first)
inline uint16_t crc_x25_bits(const uint8_t* bits, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        const int x = (c ^ bits[i]) & 1;
        c >>= 1;
        if (x) c ^= 0x8408;
    }
    return static_cast<uint16_t>(c ^ 0xFFFF);
}
// the same over octets (bit 1 = LSB first)
inline uint16_t crc_x25(const uint8_t* p, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0x8408) : static_cast<uint16_t>(c >> 1);
    }
    return static_cast<uint16_t>(c ^ 0xFFFF);
}
// a signal unit of `len` octets (12, or 19 on the R channel, 6 for a T
// burst identifier) passes its CRC: the last two octets carry it, the
// low-order octet first
inline bool su_crc_ok(const uint8_t* p, int len) {
    const uint16_t rx = static_cast<uint16_t>(p[len - 2] | (p[len - 1] << 8));
    return crc_x25(p, len - 2) == rx;
}
inline void set_su_crc(uint8_t* p, int len) {
    const uint16_t c = crc_x25(p, len - 2);
    p[len - 2] = static_cast<uint8_t>(c & 0xFF);
    p[len - 1] = static_cast<uint8_t>(c >> 8);
}
// bits (transmission order, LSB of each octet first) -> octets
inline void pack_lsb(const uint8_t* bits, int nbytes, uint8_t* out) {
    for (int i = 0; i < nbytes; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) v |= static_cast<uint8_t>((bits[8 * i + k] & 1) << k);
        out[i] = v;
    }
}

// 64-row block interleaver: coded bit k = column k / 64, row k % 64; row i
// is sent as row 27 i mod 64; rows go out one after the other. Returns the
// index in the received block of coded bit k.
inline int deinterleave_index(int k, int cols) {
    const int row = k & 63, col = k >> 6;
    return ((row * 27) & 63) * cols + col;
}

}  // namespace aero
