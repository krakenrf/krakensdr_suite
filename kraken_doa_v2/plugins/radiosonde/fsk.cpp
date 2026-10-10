#include "fsk.hpp"

#include <algorithm>

namespace sonde {

void FskRx::init(const FskConfig& c, double fs) {
    c_ = c;
    fs_ = fs;
    sps_ = fs / c.baud;
    L_ = std::max(2, static_cast<int>(std::lround(sps_)));
    win_.assign(L_, 0.0f);
    wpos_ = 0;
    sum_ = 0;
    mlen_ = static_cast<int>(std::ceil(4 * sps_)) + 8;
    ma_.assign(mlen_, 0.0f);
    n_ = 0;
    // the last (up to) 64 sync symbols: bit 0 = the last one transmitted
    const int len = std::min<int>(64, static_cast<int>(c.sync.size()));
    slen_ = len;
    sync_ = 0;
    for (int i = static_cast<int>(c.sync.size()) - len; i < static_cast<int>(c.sync.size()); i++)
        sync_ = (sync_ << 1) | (c.sync[i] & 1);
    mask_ = len >= 64 ? ~0ull : ((1ull << len) - 1);
    ph_.assign(c.nph, Phase{});
    for (int k = 0; k < c.nph; k++) {
        Phase& p = ph_[k];
        p.next = L_ + sps_ * k / c.nph;
        p.hist.assign(std::max(1, slen_), 0.0f);
        p.frame.assign(std::max(1, c.frame_syms), 0.0f);
    }
}

void FskRx::reset() {
    if (c_.baud > 0) init(c_, sps_ * c_.baud);
}

float FskRx::ma_at(double t) const {
    const int64_t i0 = static_cast<int64_t>(std::floor(t));
    const float f = static_cast<float>(t - i0);
    const float a = ma_[((i0 % mlen_) + mlen_) % mlen_];
    const float b = ma_[(((i0 + 1) % mlen_) + mlen_) % mlen_];
    return a + f * (b - a);
}

void FskRx::push(float x) {
    sum_ += x - win_[wpos_];
    win_[wpos_] = x;
    if (++wpos_ == L_) wpos_ = 0;
    // the running sum drifts by rounding: refresh it now and then
    if ((n_ & 0xFFFF) == 0) {
        double s = 0;
        for (float v : win_) s += v;
        sum_ = s;
    }
    ma_[n_ % mlen_] = static_cast<float>(sum_ / L_);
    n_++;
    // a symbol is sampled once the interpolation's later neighbour exists.
    // The search grid of each phase stays fixed (its own offset within a
    // symbol); a capture follows its own clock, so the timing loop running on
    // whatever follows a frame can't pull every phase to the same instant
    const double last = static_cast<double>(n_ - 2);
    for (Phase& p : ph_) {
        while (p.cap && p.ct <= last) {
            capture(p, ma_at(p.ct));
            p.ct += sps_;
        }
        while (p.next <= last) {
            if (!p.cap) search(p, ma_at(p.next));
            p.next += sps_;
        }
    }
}

void FskRx::search(Phase& p, float v) {
    {
        // slicing level before a sync: a running mean of this phase's samples
        // (the preamble / whitened data are balanced)
        p.dc += (v - p.dc) * (1.0f / 32);
        p.hist[p.hpos] = v;
        if (++p.hpos == slen_) p.hpos = 0;
        p.reg = (p.reg << 1) | (v > p.dc ? 1u : 0u);
        if (++p.count < slen_) return;
        const int d = __builtin_popcountll((p.reg ^ sync_) & mask_);
        const int di = __builtin_popcountll((p.reg ^ ~sync_) & mask_);
        if (d > c_.max_sync_err && di > c_.max_sync_err) return;
        // levels of the sync's 1s and 0s -> carrier offset and deviation
        double s1 = 0, s0 = 0;
        int n1 = 0, n0 = 0;
        for (int i = 0; i < slen_; i++) {
            const float h = p.hist[(p.hpos + i) % slen_];   // oldest first
            const int b = (sync_ >> (slen_ - 1 - i)) & 1;
            if (b) { s1 += h; n1++; } else { s0 += h; n0++; }
        }
        if (!n1 || !n0) return;
        const float m1 = static_cast<float>(s1 / n1), m0 = static_cast<float>(s0 / n0);
        const float dev = (m1 - m0) / 2;   // < 0 when the sync arrived inverted
        if (std::fabs(dev) < 1e-3f) return;
        p.cap = true;
        p.inv = dev < 0;
        p.dc = (m1 + m0) / 2;
        p.dev = dev;
        p.got = 0;
        p.ct = p.next + sps_;
        p.t0 = p.ct;
        p.sync_err = std::min(d, di);
        p.prev = (v - p.dc) / p.dev;
    }
}

void FskRx::capture(Phase& p, float v) {
    const float s = (v - p.dc) / p.dev;
    p.frame[p.got++] = s;
    // Gardner timing error at a symbol change: the matched filter's value
    // half way between the two samples is 0 when the timing is right
    if ((s > 0) != (p.prev > 0)) {
        const float mid = (ma_at(p.ct - sps_ / 2) - p.dc) / p.dev;
        float e = (s - p.prev) * mid;                          // > 0: sampling late
        e = std::max(-2.0f, std::min(2.0f, e));
        p.ct -= c_.timing_gain * e * sps_ / 4;
    }
    p.prev = s;
    if (p.got < c_.frame_syms) return;
    p.cap = false;
    p.count = 0;   // the next sync search starts afresh (p.dc: the frame's level)
    if (on_frame) {
        FskFrame f;
        f.soft = p.frame.data();
        f.n = p.got;
        f.t0 = p.t0;
        f.dc_hz = p.dc;
        f.dev_hz = std::fabs(p.dev);
        f.sync_err = p.sync_err;
        f.inverted = p.inv;
        f.t_start = time_at(p.t0 - slen_ * sps_);
        f.t_end = time_at(p.ct);
        on_frame(f);
    }
}

}  // namespace sonde
