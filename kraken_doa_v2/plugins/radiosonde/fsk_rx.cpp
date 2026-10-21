// Binary FSK frame receiver (see fsk_rx.hpp).

#include "fsk_rx.hpp"

#include "sonde.hpp"

#include <algorithm>

namespace sonde {

namespace {
constexpr float LEVEL_ALPHA = 1.0f / 32;   // slicing level, per symbol
constexpr float GARDNER_GAIN = 0.02f;
constexpr int64_t RESUM_EVERY = 65536;
}  // namespace

void FskRx::init(const FskParams& p) {
    p_ = p;
    sps_ = FS / p.rate;
    L_ = std::max(2, static_cast<int>(std::lround(sps_)));
    const std::string s = p.sync.size() > 64 ? p.sync.substr(p.sync.size() - 64) : p.sync;
    S_ = static_cast<int>(s.size());
    pat_ = 0;
    for (char c : s) pat_ = pat_ << 1 | (c == '1' ? 1u : 0u);
    mask_ = S_ >= 64 ? ~0ull : (1ull << S_) - 1;
    h_.assign(static_cast<size_t>(p.phases), Hyp{});
    for (int k = 0; k < p.phases; k++) {
        h_[static_cast<size_t>(k)].off = k * sps_ / p.phases;
        h_[static_cast<size_t>(k)].c.sym.resize(static_cast<size_t>(p.nsym));
    }
    xr_.assign(64, 0.0f);
    yr_.assign(RING, 0.0f);
    reset();
}

void FskRx::reset() {
    for (auto& h : h_) {
        std::vector<float> keep = std::move(h.c.sym);
        const double off = h.off;
        h = Hyp{};
        h.off = off;
        h.c.sym = std::move(keep);
    }
    std::fill(xr_.begin(), xr_.end(), 0.0f);
    std::fill(yr_.begin(), yr_.end(), 0.0f);
    sum_ = 0;
    n_ = 0;
    start_ = -1;
    resum_ = 0;
}

void FskRx::process(const float* x, size_t n, int64_t idx, std::vector<FskCapture>& out) {
    done_.clear();
    for (size_t a = 0; a < n; a += CHUNK) {
        const size_t m = std::min<size_t>(CHUNK, n - a);
        for (size_t i = 0; i < m; i++) {
            const int64_t k = idx + static_cast<int64_t>(a + i);
            if (start_ < 0) {
                start_ = k;
                origin_ = static_cast<double>(k + L_);
            }
            const float v = x[a + i];
            const float old = n_ >= L_ ? xr_[static_cast<size_t>((k - L_) & 63)] : 0.0f;
            xr_[static_cast<size_t>(k & 63)] = v;
            sum_ += static_cast<double>(v) - old;
            n_++;
            if (++resum_ >= RESUM_EVERY) {   // re-sum the window: no slow rounding drift
                resum_ = 0;
                double s = 0;
                for (int j = 0; j < L_ && j < n_; j++) s += xr_[static_cast<size_t>((k - j) & 63)];
                sum_ = s;
            }
            yr_[static_cast<size_t>(k & (RING - 1))] = static_cast<float>(sum_ / L_);
        }
        const int64_t newest = idx + static_cast<int64_t>(a + m) - 1;
        if (newest < start_ + L_) continue;
        for (auto& h : h_) run(h, newest, done_);
    }
    std::stable_sort(done_.begin(), done_.end(), [](const FskCapture& u, const FskCapture& v) { return u.pos_end < v.pos_end; });
    for (auto& c : done_) out.push_back(c);
}

void FskRx::run(Hyp& h, int64_t newest, std::vector<FskCapture>& out) {
    const double lim = static_cast<double>(newest) - 1;   // a position needs its next sample
    for (;;) {
        if (h.cap) {
            const double pos = h.cpos;
            if (pos > lim) return;
            const float dv = h.c.inverted ? -h.c.dev : h.c.dev;
            const float v = (at(pos) - h.c.centre) / dv;
            if (h.cn > 0 && (v > 0) != (h.cprev > 0)) {
                // Gardner at a symbol change: the output half a symbol before
                // should be at the centre (positive error = sampling late)
                const float mid = (at(pos - sps_ / 2) - h.c.centre) / dv;
                const float e = std::clamp(mid * (v - h.cprev), -2.0f, 2.0f);
                h.cpos -= GARDNER_GAIN * e * sps_ / 4;
            }
            h.c.sym[static_cast<size_t>(h.cn++)] = v;
            h.cprev = v;
            if (h.cn >= p_.nsym) {
                h.c.pos_end = pos;
                out.push_back(h.c);
                h.cap = false;
                h.count = 0;
                h.level = h.c.centre;
                h.have_level = true;
                // back on the fixed grid, after the capture
                h.j = static_cast<int64_t>(std::floor((pos - origin_ - h.off) / sps_)) + 1;
                continue;
            }
            h.cpos += sps_;
            continue;
        }
        const double pos = origin_ + h.off + static_cast<double>(h.j) * sps_;
        if (pos > lim) return;
        h.j++;
        const float v = at(pos);
        if (!h.have_level) {
            h.level = v;
            h.have_level = true;
        }
        const uint64_t hard = v > h.level ? 1u : 0u;
        h.level += LEVEL_ALPHA * (v - h.level);
        h.reg = (h.reg << 1 | hard) & mask_;
        h.soft[h.w] = v;
        h.w = (h.w + 1) & 63;
        if (h.count < S_) h.count++;
        if (h.count < S_) continue;
        const int d = __builtin_popcountll(h.reg ^ pat_);
        const int di = __builtin_popcountll(h.reg ^ (~pat_ & mask_));
        if (d > p_.max_err && di > p_.max_err) continue;
        // levels from the sync's own samples
        double s1 = 0, s0 = 0;
        int n1 = 0, n0 = 0;
        for (int jj = 0; jj < S_; jj++) {
            const float sv = h.soft[(h.w - S_ + jj) & 63];
            if (pat_ >> (S_ - 1 - jj) & 1) {
                s1 += sv;
                n1++;
            } else {
                s0 += sv;
                n0++;
            }
        }
        if (!n1 || !n0) continue;
        const double m1 = s1 / n1, m0 = s0 / n0;
        const double dev = (m1 - m0) / 2;
        if (std::fabs(dev) < 0.001) continue;
        h.c.centre = static_cast<float>((m1 + m0) / 2);
        h.c.dev = static_cast<float>(std::fabs(dev));
        h.c.inverted = dev < 0;
        h.c.errors = dev < 0 ? di : d;
        h.cap = true;
        h.cn = 0;
        h.cprev = 0;
        h.cpos = pos + sps_;
        h.c.pos0 = h.cpos;
        h.c.pos_sync = h.cpos - S_ * sps_;
    }
}

}  // namespace sonde
