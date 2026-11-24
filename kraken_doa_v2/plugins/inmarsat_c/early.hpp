#pragma once
// Early frame recognition: the unique word runs through the whole frame (two
// symbols at the start of each of the 64 transmitted rows, one row every
// 162 symbols = 0.135 s), so the TDM - and where its frame starts - shows
// long before a whole frame is in. For every possible frame phase (10368)
// this keeps the correlation of the rows received so far with the unique
// word, updated per symbol (64 additions: the new symbol pair is row j of
// the phase 162 j earlier), using each row's newest occurrence only.

#include <cmath>
#include <cstdint>
#include <vector>

#include "frame.hpp"

namespace inmc {

class UwEarly {
public:
    struct Best {
        int phase = -1;      // frame start modulo FRAME_SYMS
        float corr = 0;      // normalised correlation (+-1 = every symbol agrees), sign = polarity
        float second = 0;    // the best |corr| elsewhere (more than 3 symbols away)
        int rows = 0;        // rows that went into it
    };

    UwEarly() : c_(FRAME_SYMS, 0.0f), e_(FRAME_SYMS, 0.0f), r_(FRAME_SYMS, 0) {}

    void reset(int64_t first_real) {
        std::fill(c_.begin(), c_.end(), 0.0f);
        std::fill(e_.begin(), e_.end(), 0.0f);
        std::fill(r_.begin(), r_.end(), 0);
        first_ = first_real;
    }

    // after symbol n (absolute index) was written into ring[n % ring_n]
    void push(int64_t n, const float* ring, int64_t ring_n) {
        const int64_t p = n - 1;   // the pair (p, n)
        if (p < first_) return;
        const float a = ring[p % ring_n], b = ring[n % ring_n];
        const int64_t q = p - FRAME_SYMS;   // the same pair one frame earlier (replaced)
        const bool had = q >= first_;
        float a0 = 0, b0 = 0;
        if (had) a0 = ring[q % ring_n], b0 = ring[(q + 1) % ring_n];
        const float s = a + b, e = a * a + b * b, s0 = a0 + b0, e0 = a0 * a0 + b0 * b0;
        int phi = static_cast<int>(p % FRAME_SYMS);
        for (int j = 0; j < ROWS; j++) {
            const float u = uw_sign(j);
            c_[phi] += u * (s - s0);
            e_[phi] += e - e0;
            if (!had) r_[phi]++;
            phi -= COLS;
            if (phi < 0) phi += FRAME_SYMS;
        }
    }

    Best best() const {
        Best b;
        float bv = 0;
        int bi = -1;
        for (int i = 0; i < FRAME_SYMS; i++) {
            if (r_[i] == 0 || e_[i] <= 0) continue;
            const float v = std::fabs(c_[i]) / std::sqrt(2.0f * r_[i] * e_[i]);
            if (v > bv) bv = v, bi = i;
        }
        if (bi < 0) return b;
        float sv = 0;
        for (int i = 0; i < FRAME_SYMS; i++) {
            int d = std::abs(i - bi);
            d = std::min(d, FRAME_SYMS - d);
            if (d <= 3 || r_[i] == 0 || e_[i] <= 0) continue;
            const float v = std::fabs(c_[i]) / std::sqrt(2.0f * r_[i] * e_[i]);
            if (v > sv) sv = v;
        }
        b.phase = bi;
        b.corr = c_[bi] / std::sqrt(2.0f * r_[bi] * e_[bi]);
        b.second = sv;
        b.rows = r_[bi];
        return b;
    }

private:
    std::vector<float> c_, e_;
    std::vector<int> r_;
    int64_t first_ = 0;
};

}  // namespace inmc
