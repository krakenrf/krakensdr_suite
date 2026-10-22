#pragma once
// RDS data link (EN 50067 / IEC 62106-1 annex B / C): 104-bit groups of four
// 26-bit blocks, each 16 information bits + a 10-bit check word = the
// remainder of m(x) x^10 / g(x), g = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1,
// XOR an offset word naming the block (A, B, C, C' (version B groups), D).
// So a received word w(x) of block k has w(x) mod g(x) == offset_k when it is
// error-free.
//
// BlockSync: the soft bit stream -> block / group synchronisation (three
// offset words in their order at 26-bit spacing, as EN 50067 C.1.1
// suggests), then per block: the offset check, else the correction of a
// burst of up to 3 bits (a symbol error is two adjacent bit errors after the
// differential decoding) - only where the soft bits were the least reliable
// of the block -, else flipping 1..3 of the 5 least reliable bits (soft
// decision). Every corrected block is marked; the group decoder takes data
// from a corrected block only when it is confirmed by a second identical
// copy. Sync is dropped when 40 of the last 50 blocks failed (C.1.2), and a
// sync sequence at another bit position while every block fails moves to it
// (a slipped bit).

#include "rds_demod.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace rds {

enum : uint16_t { OFF_A = 0x0FC, OFF_B = 0x198, OFF_C = 0x168, OFF_CP = 0x350, OFF_D = 0x1B4 };
constexpr uint32_t POLY = 0x5B9;   // g(x), x^10 included

// w(x) mod g(x) of a 26-bit word
inline uint16_t syndrome26(uint32_t w) {
    uint32_t r = w & 0x3FFFFFF;
    for (int b = 25; b >= 10; b--)
        if (r & (1u << b)) r ^= POLY << (b - 10);
    return static_cast<uint16_t>(r & 0x3FF);
}
// the check word of 16 information bits for an offset (the transmitter's side)
inline uint32_t encode_block(uint16_t info, uint16_t offset) {
    const uint32_t m = static_cast<uint32_t>(info) << 10;
    return m | (syndrome26(m) ^ offset);
}

struct Block {
    uint16_t v = 0;
    uint8_t st = 0;        // 0 = missing / failed, 1 = ok, 2 = corrected
    uint8_t nfix = 0;      // bits corrected
};

struct Group {
    Block b[4];
    bool cprime = false;   // block 3 carried offset C'
    double t0 = 0, t1 = 0; // MPX sample time of the group's first bit / its end
    int good() const { return (b[0].st != 0) + (b[1].st != 0) + (b[2].st != 0) + (b[3].st != 0); }
    bool clean() const { return b[0].st == 1 && b[1].st == 1 && b[2].st == 1 && b[3].st == 1; }
};

class BlockSync {
public:
    BlockSync() {
        // burst error patterns 1, 11, 101, 111 at every position: syndrome -> pattern
        burst_.fill(0);
        for (uint32_t pat : {1u, 3u, 5u, 7u})
            for (int s = 0; s < 26; s++) {
                const uint32_t e = (pat << s) & 0x3FFFFFF;
                if ((e >> s) != pat) continue;   // cut off at the top
                const uint16_t syn = syndrome26(e);
                if (syn && !burst_[syn]) burst_[syn] = e;
            }
        reset();
    }

    void reset() {
        reg_ = 0;
        nbits_ = 0;
        synced_ = false;
        npulse_ = 0;
        fails50_ = 0;
        hist_ = 0;
        cur_ = Group();
        consec_fail_ = 0;
        blk_ = 0;
        left_ = 0;
    }

    bool synced() const { return synced_; }
    // changes with every (re)acquisition of the sync
    uint32_t generation() const { return gen_; }
    // block error rate over the last 50 blocks (0..1; 1 while not synced)
    float bler() const { return synced_ ? static_cast<float>(fails50_) / 50.0f : 1.0f; }
    uint64_t blocks_ok = 0, blocks_fixed = 0, blocks_bad = 0;

    // one soft bit; true + *g when a group's 4 blocks are done
    bool push(const SoftBit& sb, Group* g) {
        reg_ = ((reg_ << 1) | (sb.v > 0 ? 1u : 0u)) & 0x3FFFFFF;
        rel_[nbits_ % 26] = std::fabs(sb.v);
        t_[nbits_ % 26] = sb.t;
        nbits_++;
        if (nbits_ < 26) return false;
        // a sync pulse here? (needed to acquire, and while the blocks fail)
        if (!synced_ || consec_fail_ >= 10) {
            const uint16_t s = syndrome26(reg_);
            const int k = block_of(s);
            if (k >= 0 && find_sequence(k)) {
                // not synced, or the blocks keep failing and a sequence runs at
                // another bit position (a slipped bit) / another block number
                if (!synced_ || (nbits_ - 26) % 26 != aligned_mod_ || (left_ == 0 && k != blk_)) {
                    synced_ = true;
                    gen_++;
                    aligned_mod_ = (nbits_ - 26) % 26;
                    fails50_ = 0;
                    hist_ = 0;
                    consec_fail_ = 0;
                    // this word is block k: take it, continue with the next
                    cur_ = Group();
                    blk_ = k;
                    left_ = 0;
                }
            }
        }
        if (!synced_) return false;
        if (left_ > 0) {
            left_--;
            return false;
        }
        left_ = 25;
        return block(g);
    }

private:
    std::array<uint32_t, 1024> burst_;
    uint32_t gen_ = 0;
    uint32_t reg_ = 0;
    uint64_t nbits_ = 0;
    float rel_[26] = {};
    double t_[26] = {};
    bool synced_ = false;
    uint64_t aligned_mod_ = 0;
    struct Pulse { int k; uint64_t bit; };
    Pulse pulses_[8];
    int npulse_ = 0;
    int fails50_ = 0;
    uint64_t hist_ = 0;        // the last 50 blocks: 1 = failed
    int consec_fail_ = 0;
    Group cur_;
    int blk_ = 0;              // the block number expected next (0..3)
    int left_ = 0;             // bits until the next block ends

    static int block_of(uint16_t s) {
        switch (s) {
            case OFF_A: return 0;
            case OFF_B: return 1;
            case OFF_C: case OFF_CP: return 2;
            case OFF_D: return 3;
            default: return -1;
        }
    }

    // three offset words in order at 26-bit spacing (<= 6 blocks apart)
    bool find_sequence(int k) {
        const uint64_t bit = nbits_;
        bool found = false;
        for (int j = 0; j < npulse_ && !found; j++) {
            const uint64_t d2 = bit - pulses_[j].bit;
            if (d2 == 0 || d2 % 26 || d2 / 26 > 6 || (pulses_[j].k + d2 / 26) % 4 != static_cast<uint64_t>(k)) continue;
            for (int i = 0; i < npulse_; i++) {
                if (i == j) continue;
                const uint64_t d1 = pulses_[j].bit - pulses_[i].bit;
                if (pulses_[j].bit <= pulses_[i].bit || d1 % 26 || d1 / 26 > 6) continue;
                if ((pulses_[i].k + d1 / 26) % 4 == static_cast<uint64_t>(pulses_[j].k)) {
                    found = true;
                    break;
                }
            }
        }
        if (npulse_ == 8) {
            for (int i = 1; i < 8; i++) pulses_[i - 1] = pulses_[i];
            npulse_ = 7;
        }
        pulses_[npulse_++] = {k, bit};
        return found;
    }

    void note(bool failed) {
        if (hist_ >> 49 & 1) fails50_--;
        hist_ = ((hist_ << 1) | (failed ? 1u : 0u)) & ((1ull << 50) - 1);
        if (failed) fails50_++;
        consec_fail_ = failed ? consec_fail_ + 1 : 0;
        if (fails50_ >= 40) {
            synced_ = false;
            npulse_ = 0;
        }
    }

    // reliabilities of the word's bits, bit 25 (first received) .. bit 0
    float rel_of(int bitpos) const { return rel_[(nbits_ - 1 - static_cast<uint64_t>(bitpos)) % 26]; }

    // try to make the word carry offset `off`; *fix = bits changed (0 ok)
    bool correct(uint32_t w, uint16_t off, uint32_t* out, int* fix) const {
        const uint16_t s = syndrome26(w) ^ off;
        if (s == 0) {
            *out = w;
            *fix = 0;
            return true;
        }
        // the reliabilities, sorted: the 6th weakest is the burst limit
        float r[26];
        for (int i = 0; i < 26; i++) r[i] = rel_of(i);
        float srt[26];
        std::copy(r, r + 26, srt);
        std::nth_element(srt, srt + 5, srt + 26);
        const float lim = srt[5];
        const uint32_t e = burst_[s];
        if (e) {
            bool weak = true;
            for (int i = 0; i < 26 && weak; i++)
                if ((e >> i & 1) && r[i] > lim) weak = false;
            if (weak) {
                *out = w ^ e;
                *fix = __builtin_popcount(e);
                return true;
            }
        }
        // soft decision: 1..3 of the 5 weakest bits
        int idx[26];
        for (int i = 0; i < 26; i++) idx[i] = i;
        std::partial_sort(idx, idx + 5, idx + 26, [&](int a, int b) { return r[a] < r[b]; });
        uint16_t ss[5];
        for (int i = 0; i < 5; i++) ss[i] = syndrome26(1u << idx[i]);
        for (int w1 = 1; w1 <= 3; w1++)
            for (int m = 1; m < 32; m++) {
                if (__builtin_popcount(static_cast<unsigned>(m)) != w1) continue;
                uint16_t t = 0;
                uint32_t pat = 0;
                for (int i = 0; i < 5; i++)
                    if (m >> i & 1) {
                        t ^= ss[i];
                        pat |= 1u << idx[i];
                    }
                if (t == s) {
                    *out = w ^ pat;
                    *fix = w1;
                    return true;
                }
            }
        return false;
    }

    bool block(Group* g) {
        const uint32_t w = reg_;
        Block b;
        uint32_t fixed = 0;
        int nfix = 0;
        bool ok = false;
        if (blk_ == 2) {
            // C (version A) or C' (version B): the group's block B says which, when received
            const bool know = cur_.b[1].st != 0;
            const bool ver_b = know && (cur_.b[1].v >> 11 & 1);
            const uint16_t first = (know && ver_b) ? OFF_CP : OFF_C, second = first == OFF_C ? OFF_CP : OFF_C;
            if (correct(w, first, &fixed, &nfix)) {
                ok = true;
                cur_.cprime = first == OFF_CP;
            } else if (!know && correct(w, second, &fixed, &nfix)) {
                ok = true;
                cur_.cprime = second == OFF_CP;
            }
        } else {
            static const uint16_t OFF[4] = {OFF_A, OFF_B, OFF_C, OFF_D};
            ok = correct(w, OFF[blk_], &fixed, &nfix);
        }
        if (ok) {
            b.v = static_cast<uint16_t>(fixed >> 10);
            b.st = nfix ? 2 : 1;
            b.nfix = static_cast<uint8_t>(nfix);
            (nfix ? blocks_fixed : blocks_ok)++;
        } else {
            blocks_bad++;
        }
        if (blk_ == 0) cur_.t0 = t_[(nbits_ - 26) % 26] - 0.5 * BIT_SAMPLES;
        cur_.b[blk_] = b;
        note(!ok);
        bool done = false;
        if (blk_ == 3) {
            cur_.t1 = t_[(nbits_ - 1) % 26] + 0.5 * BIT_SAMPLES;
            // a group that started before sync (no block A time) still counts
            if (cur_.t0 <= 0) cur_.t0 = cur_.t1 - 104 * BIT_SAMPLES;
            *g = cur_;
            done = true;
            cur_ = Group();
        }
        blk_ = (blk_ + 1) & 3;
        return done;
    }

public:
    // the length of a bit in MPX samples (set by the owner: fs / 1187.5)
    double BIT_SAMPLES = 202.1;
};

}  // namespace rds
