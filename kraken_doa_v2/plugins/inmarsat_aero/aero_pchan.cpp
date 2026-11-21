#include "aero_pchan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aero {

namespace {
// Table 4-10 / 4-11 and figures 4-1 / 4-2
const PRate RATES[] = {
    {600, Mod::ABPSK, 32, 0, 3, 6, 6, 1200, 4},
    {1200, Mod::ABPSK, 32, 0, 2, 9, 6, 1200, 8},
    {2400, Mod::ABPSK, 32, 0, 1, 18, 6, 1200, 16},
    {4800, Mod::AQPSK, 64, 16, 1, 36, 12, 2400, 16},
    {10500, Mod::AQPSK, 64, 178, 1, 78, 26, 5250, 16},
};
constexpr int SEARCH_ERR_BPSK = 2, SEARCH_ERR_QPSK = 6;
constexpr int TRACK_ERR_BPSK = 8, TRACK_ERR_QPSK = 16;
}  // namespace

const PRate* p_rate(double rate) {
    for (const auto& r : RATES)
        if (std::fabs(r.rate - rate) < 1) return &r;
    return nullptr;
}

void PChannel::start(const PRate& r) {
    r_ = r;
    on_ = true;
    s_.clear();
    t_.clear();
    base_ = 0;
    next_uw_ = -1;
    search_from_ = 0;
    synced_ = false;
    missed_ = 0;
    frames_ = uws_ = 0;
    have_prev_ = false;
    vit_.reset();
    dec_.clear();
    dec_base_ = 0;
    pending_.clear();
    vit_steps_ = 0;
}

void PChannel::stop() {
    if (on_) drain(true);
    on_ = false;
    synced_ = false;
}

bool PChannel::match_uw(int64_t p, int max_err, int& err, float& sa, float& sb) const {
    const float* s = &s_[static_cast<size_t>(p - base_)];
    if (r_.mod == Mod::ABPSK) {
        int e = 0;
        for (int k = 0; k < 32; k++) {
            const int bit = s[k] < 0;
            e += bit != static_cast<int>((UW_P >> (31 - k)) & 1);
        }
        sa = sb = e <= 16 ? 1.0f : -1.0f;
        err = std::min(e, 32 - e);
        return err <= max_err;
    }
    int ea = 0, eb = 0;
    for (int k = 0; k < 32; k++) {
        const int u = static_cast<int>((UW_P >> (31 - k)) & 1);
        ea += (s[2 * k] < 0) != u;
        eb += (s[2 * k + 1] < 0) != u;
    }
    sa = ea <= 16 ? 1.0f : -1.0f;
    sb = eb <= 16 ? 1.0f : -1.0f;
    err = std::min(ea, 32 - ea) + std::min(eb, 32 - eb);
    return err <= max_err;
}

void PChannel::push(const SoftBit* b, size_t n) {
    if (!on_) return;
    for (size_t i = 0; i < n; i++) {
        float v = b[i].d;
        if (r_.mod == Mod::ABPSK) {
            // differential detection: a "1" keeps the sign (+90 deg on the
            // turning axis), a "0" flips it; soft value + = "0"
            const float cur = v;
            if (!have_prev_) {
                prev_ = cur;
                have_prev_ = true;
                continue;
            }
            const float m = std::min(std::fabs(cur), std::fabs(prev_));
            v = (cur * prev_ > 0) ? -m : m;
            prev_ = cur;
        }
        s_.push_back(v);
        t_.push_back(b[i].t);
    }
    const bool bpsk = r_.mod == Mod::ABPSK;
    for (;;) {
        if (next_uw_ >= 0) {
            // the whole frame (and one bit either side) must be in
            if (idx_end() < next_uw_ + r_.frame + 1) break;
            int err = 0, best_err = 1 << 30;
            float sa = 1, sb = 1, bsa = 1, bsb = 1;
            int64_t best = -1;
            const int lim = synced_ ? (bpsk ? TRACK_ERR_BPSK : TRACK_ERR_QPSK) : (bpsk ? SEARCH_ERR_BPSK : SEARCH_ERR_QPSK);
            for (int d : {0, -1, 1}) {
                const int64_t p = next_uw_ + d;
                if (p < base_ || (!synced_ && d != 0)) continue;
                if (match_uw(p, lim, err, sa, sb) && err < best_err) {
                    best_err = err;
                    best = p;
                    bsa = sa;
                    bsb = sb;
                }
            }
            if (best >= 0) {
                if (best != next_uw_ || !synced_) {
                    // not where the previous frame ended: the code stream broke
                    drain(true);
                    vit_.reset();
                    dec_.clear();
                    dec_base_ = 0;
                    vit_steps_ = 0;
                }
                frame_at(best, best_err, bsa, bsb);
                synced_ = true;
                missed_ = 0;
                next_uw_ = best + r_.frame;
            } else if (synced_) {
                missed_++;
                drain(true);
                vit_.reset();
                dec_.clear();
                dec_base_ = 0;
                vit_steps_ = 0;
                if (missed_ >= 3) {
                    synced_ = false;
                    search_from_ = next_uw_;
                    next_uw_ = -1;
                } else {
                    next_uw_ += r_.frame;
                }
            } else {
                // a candidate that did not hold: search on after it
                search_from_ = next_uw_ + 1;
                next_uw_ = -1;
            }
            continue;
        }
        // searching
        const int64_t last = idx_end() - r_.uw;
        int64_t p = std::max(search_from_, base_);
        bool found = false;
        for (; p <= last; p++) {
            int err;
            float sa, sb;
            if (match_uw(p, bpsk ? SEARCH_ERR_BPSK : SEARCH_ERR_QPSK, err, sa, sb)) {
                found = true;
                break;
            }
        }
        if (!found) {
            search_from_ = std::max<int64_t>(last + 1, base_);
            break;
        }
        next_uw_ = p;
    }
    trim();
}

void PChannel::frame_at(int64_t p, int err, float sa, float sb) {
    uws_++;
    const size_t off = static_cast<size_t>(p - base_);
    PFrame f;
    f.t0 = t_[off];
    const size_t endi = off + static_cast<size_t>(r_.frame) - 1;
    f.t1 = t_[endi] + (t_[endi] - t_[endi - 1]);
    f.uw_errors = err;
    auto soft = [&](int j) {   // frame bit j after the UW, polarity fixed
        const float v = s_[off + static_cast<size_t>(r_.uw + j)];
        return (j & 1) ? v * sb : v * sa;
    };
    int hdr = 0;
    for (int j = 0; j < 16; j++) hdr = (hdr << 1) | (soft(j) < 0 ? 1 : 0);
    const int fi = hdr >> 12, mark = (hdr >> 8) & 15, fn1 = (hdr >> 4) & 15, fn2 = hdr & 15;
    f.header_ok = fi == 1 && fn1 == fn2 && (mark == 0 || mark == 15) && fn1 < r_.frames_per_superframe &&
                  ((mark == 15) == (fn1 == 0));
    f.frame_no = f.header_ok ? fn1 : -1;
    f.sf_start = f.header_ok && mark == 15;
    f.sus = r_.sus;
    // information field: deinterleave each block into the Viterbi
    Pending pd;
    pd.f = f;
    pd.start = vit_steps_;
    int j0 = 16 + r_.dummy;
    std::vector<float> blk;
    for (int b = 0; b < r_.blocks; b++) {
        const int nb = 64 * r_.cols;
        blk.resize(nb);
        for (int k = 0; k < nb; k++) blk[k] = soft(j0 + k);
        for (int k = 0; k < nb; k += 2)
            vit_.push(blk[deinterleave_index(k, r_.cols)], blk[deinterleave_index(k + 1, r_.cols)]);
        vit_steps_ += nb / 2;
        j0 += nb;
    }
    pending_.push_back(pd);
    drain(false);
}

void PChannel::drain(bool flush) {
    if (flush) vit_.flush();
    vit_.take(dec_);
    const int64_t have = dec_base_ + static_cast<int64_t>(dec_.size());
    const int nbits = r_.sus * 96;
    size_t done = 0;
    for (auto& pd : pending_) {
        if (pd.start + nbits > have) break;
        done++;
        frames_++;
        const uint8_t* bits = &dec_[static_cast<size_t>(pd.start - dec_base_)];
        std::vector<uint8_t> d(static_cast<size_t>(nbits));
        Scrambler sc;
        for (int i = 0; i < nbits; i++) d[static_cast<size_t>(i)] = static_cast<uint8_t>(bits[i] ^ sc.next());
        PFrame& f = pd.f;
        for (int k = 0; k < r_.sus; k++) {
            uint8_t su[12];
            pack_lsb(&d[static_cast<size_t>(96 * k)], 12, su);
            if (su_crc_ok(su, 12)) {
                f.sus_ok++;
                if (on_su) on_su(su, f, k);
            }
        }
        if (on_frame) on_frame(f);
    }
    if (flush) done = pending_.size();   // a frame without all its bits is dropped
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(done));
    // keep only what pending frames still need
    const int64_t keep = pending_.empty() ? have : pending_.front().start;
    if (keep > dec_base_) {
        const size_t drop = static_cast<size_t>(std::min<int64_t>(keep - dec_base_, static_cast<int64_t>(dec_.size())));
        dec_.erase(dec_.begin(), dec_.begin() + static_cast<long>(drop));
        dec_base_ += static_cast<int64_t>(drop);
    }
}

void PChannel::trim() {
    // keep the bits from the earliest one still needed
    int64_t need = next_uw_ >= 0 ? next_uw_ - 2 : search_from_ - 2;
    need = std::min(need, idx_end() - static_cast<int64_t>(r_.frame) * 2);
    if (need - base_ > 4 * r_.frame) {
        const size_t drop = static_cast<size_t>(need - base_);
        s_.erase(s_.begin(), s_.begin() + static_cast<long>(drop));
        t_.erase(t_.begin(), t_.begin() + static_cast<long>(drop));
        base_ += static_cast<int64_t>(drop);
    }
}

}  // namespace aero
