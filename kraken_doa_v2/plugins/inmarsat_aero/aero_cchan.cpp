#include "aero_cchan.hpp"

#include <algorithm>
#include <cmath>

namespace aero {

namespace {
// 4.4.5.3.1.1: I 1010 1011 0011 0111 0110 1001 0011 1000 1011 1100 1010 0011 0000,
//              Q 0000 1100 0101 0011 1101 0001 1100 1001 0110 1110 1100 1101 0101
constexpr uint64_t UW_I = 0xAB376938BCA30ull;   // 52 bits, first transmitted = bit 51
constexpr uint64_t UW_Q = 0x0C53D1C96ECD5ull;
inline int uwi(int k) { return static_cast<int>((UW_I >> (51 - k)) & 1); }
inline int uwq(int k) { return static_cast<int>((UW_Q >> (51 - k)) & 1); }
constexpr int SEARCH_ERR = 8, TRACK_ERR = 26;
constexpr float ACC_THR = 0.5f, ACC_ONE = 0.4f;
constexpr int NINFO = 2730;   // decoded bits per frame (2714 + 16 dummy)
}  // namespace

void CChannel::start() {
    on_ = true;
    s_.clear();
    t_.clear();
    base_ = 0;
    next_uw_ = -1;
    search_from_ = 0;
    synced_ = cand_soft_ = false;
    missed_ = 0;
    frames_ = 0;
    facc_.assign(FRAME, 0.0f);
    restart_code();
    pending_.clear();
}

void CChannel::stop() {
    if (on_) drain(true);
    on_ = false;
    synced_ = false;
}

void CChannel::restart_code() {
    vit_.reset();
    dec_.clear();
    dec_base_ = 0;
    vit_steps_ = 0;
}

bool CChannel::match_uw(int64_t p, int max_err, int& err, float& sa, float& sb) const {
    const float* s = &s_[static_cast<size_t>(p - base_)];
    int ea = 0, eb = 0;
    for (int k = 0; k < 52; k++) {
        ea += (s[2 * k] < 0) != uwi(k);
        eb += (s[2 * k + 1] < 0) != uwq(k);
    }
    sa = ea <= 26 ? 1.0f : -1.0f;
    sb = eb <= 26 ? 1.0f : -1.0f;
    err = std::min(ea, 52 - ea) + std::min(eb, 52 - eb);
    return err <= max_err;
}

float CChannel::soft_corr(int64_t p) const {
    const float* s = &s_[static_cast<size_t>(p - base_)];
    float ca = 0, cb = 0, m = 0;
    for (int k = 0; k < 52; k++) {
        ca += s[2 * k] * (uwi(k) ? -1.0f : 1.0f);
        cb += s[2 * k + 1] * (uwq(k) ? -1.0f : 1.0f);
        m += std::fabs(s[2 * k]) + std::fabs(s[2 * k + 1]);
    }
    return m > 0 ? (std::fabs(ca) + std::fabs(cb)) / m : 0;
}

void CChannel::push(const SoftBit* b, size_t n) {
    if (!on_) return;
    for (size_t i = 0; i < n; i++) {
        s_.push_back(b[i].d);
        t_.push_back(b[i].t);
    }
    for (;;) {
        if (next_uw_ >= 0) {
            if (idx_end() < next_uw_ + FRAME + 1) break;
            const int lim = (synced_ || cand_soft_) ? TRACK_ERR : SEARCH_ERR;
            cand_soft_ = false;
            int err = 0, best_err = 1 << 30;
            float sa = 1, sb = 1, bsa = 1, bsb = 1;
            int64_t best = -1;
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
                    drain(true);
                    restart_code();
                }
                frame_at(best, best_err, bsa, bsb);
                synced_ = true;
                missed_ = 0;
                next_uw_ = best + FRAME;
            } else if (synced_) {
                // the to-aircraft C channel is a burst (a call): it ends
                missed_++;
                drain(true);
                restart_code();
                if (missed_ >= 2) {
                    synced_ = false;
                    search_from_ = next_uw_;
                    next_uw_ = -1;
                } else {
                    next_uw_ += FRAME;
                }
            } else {
                search_from_ = next_uw_ + 1;
                next_uw_ = -1;
            }
            continue;
        }
        const int64_t last = idx_end() - UWB;
        int64_t p = std::max(search_from_, base_);
        bool found = false;
        for (; p <= last; p++) {
            int err;
            float sa, sb;
            if (match_uw(p, SEARCH_ERR, err, sa, sb)) {
                found = true;
                break;
            }
            const float c = soft_corr(p);
            float& acc = facc_[static_cast<size_t>(((p % FRAME) + FRAME) % FRAME)];
            acc = 0.5f * acc + 0.5f * c;
            if (acc >= ACC_THR && c >= ACC_ONE) {
                found = true;
                cand_soft_ = true;
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

void CChannel::frame_at(int64_t p, int err, float sa, float sb) {
    const size_t off = static_cast<size_t>(p - base_);
    Pending pd;
    pd.f.t0 = t_[off];
    const size_t endi = off + FRAME - 1;
    pd.f.t1 = t_[endi] + (t_[endi] - t_[endi - 1]);
    pd.f.uw_errors = err;
    auto soft = [&](int j) {
        const float v = s_[off + static_cast<size_t>(UWB + j)];
        return (j & 1) ? v * sb : v * sa;
    };
    std::vector<float> coded(4096);
    for (int b = 0; b < 16; b++)
        for (int k = 0; k < 256; k++) coded[static_cast<size_t>(256 * b + k)] = soft(256 * b + deinterleave_index(k, 4));
    // depuncture: per two input bits G1 G2 G1 (the second G2 was dropped)
    pd.start = vit_steps_;
    for (int i = 0; i + 2 < 4095; i += 3) {
        vit_.push(coded[static_cast<size_t>(i)], coded[static_cast<size_t>(i + 1)]);
        vit_.push(coded[static_cast<size_t>(i + 2)], 0.0f);
    }
    vit_steps_ += NINFO;
    pending_.push_back(pd);
    drain(false);
}

void CChannel::drain(bool flush) {
    if (flush) vit_.flush();
    vit_.take(dec_);
    const int64_t have = dec_base_ + static_cast<int64_t>(dec_.size());
    size_t done = 0;
    for (auto& pd : pending_) {
        if (pd.start + NINFO > have) break;
        done++;
        frames_++;
        const uint8_t* bits = &dec_[static_cast<size_t>(pd.start - dec_base_)];
        std::array<uint8_t, NINFO> d{};
        Scrambler sc;
        for (int i = 0; i < NINFO; i++) d[static_cast<size_t>(i)] = static_cast<uint8_t>(bits[i] ^ sc.next());
        CFrame& f = pd.f;
        std::array<uint8_t, 288> sb{};
        int refs = 0;
        for (int k = 0; k < 25; k++) {
            const int g = k * 109;
            refs += d[static_cast<size_t>(g)] != 1;
            for (int i = 0; i < 96; i++) f.voice[static_cast<size_t>(k)][static_cast<size_t>(i)] = d[static_cast<size_t>(g + 1 + i)];
            if (k < 24)
                for (int i = 0; i < 12; i++) sb[static_cast<size_t>(12 * k + i)] = d[static_cast<size_t>(g + 97 + i)];
        }
        refs += d[2713] != 1;
        f.ref_errors = refs;
        for (int k = 0; k < 3; k++) {
            uint8_t su[12];
            pack_lsb(&sb[static_cast<size_t>(96 * k)], 12, su);
            if (su_crc_ok(su, 12)) {
                f.sus_ok++;
                if (on_su) on_su(su, f);
            }
        }
        if (on_frame) on_frame(f);
    }
    if (flush) done = pending_.size();
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(done));
    const int64_t keep = pending_.empty() ? have : pending_.front().start;
    if (keep > dec_base_) {
        const size_t drop = static_cast<size_t>(std::min<int64_t>(keep - dec_base_, static_cast<int64_t>(dec_.size())));
        dec_.erase(dec_.begin(), dec_.begin() + static_cast<long>(drop));
        dec_base_ += static_cast<int64_t>(drop);
    }
}

void CChannel::trim() {
    int64_t need = next_uw_ >= 0 ? next_uw_ - 2 : search_from_ - 2;
    need = std::min(need, idx_end() - 2 * FRAME);
    if (need - base_ > 4 * FRAME) {
        const size_t drop = static_cast<size_t>(need - base_);
        s_.erase(s_.begin(), s_.begin() + static_cast<long>(drop));
        t_.erase(t_.begin(), t_.begin() + static_cast<long>(drop));
        base_ += static_cast<int64_t>(drop);
    }
}

}  // namespace aero
