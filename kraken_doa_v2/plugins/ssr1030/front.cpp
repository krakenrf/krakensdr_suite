#include "front.hpp"

#include <algorithm>
#include <cmath>

namespace ssr {

FrontEnd::FrontEnd() : hx_(static_cast<size_t>(RING)), hp_(static_cast<size_t>(RING), 0.0f) { blocks_.reserve(NBLOCKS); }

void FrontEnd::reset() {
    std::fill(hp_.begin(), hp_.end(), 0.0f);
    n_ = 0;
    blk_acc_ = 0;
    blk_n_ = 0;
    blocks_.clear();
    noise_ = 0;
    thr_ = 0;
    in_seg_ = false;
}

void FrontEnd::set_threshold_db(double db) {
    thr_db_ = db;
    if (noise_ > 0) thr_ = static_cast<float>(noise_ * std::pow(10.0, thr_db_ / 10));
}

void FrontEnd::push(const kp::cf* x, size_t n, std::vector<Pulse>& out) {
    constexpr size_t CH = 4096;
    size_t i = 0;
    while (i < n) {
        const size_t k0 = static_cast<size_t>(n_ & HM);
        const size_t len = std::min({n - i, static_cast<size_t>(RING) - k0, CH});
        std::copy(x + i, x + i + len, hx_.begin() + static_cast<long>(k0));
        float* __restrict P = hp_.data() + k0;
        const float* __restrict xs = reinterpret_cast<const float*>(x + i);
        for (size_t j = 0; j < len; j++) P[j] = xs[2 * j] * xs[2 * j] + xs[2 * j + 1] * xs[2 * j + 1];
        // noise: means of NBLK samples, their median every NBLOCKS
        for (size_t j = 0; j < len;) {
            const size_t m = std::min(len - j, static_cast<size_t>(NBLK - blk_n_));
            float acc = 0;
            for (size_t q = 0; q < m; q++) acc += P[j + q];
            blk_acc_ += acc;
            blk_n_ += static_cast<int>(m);
            j += m;
            if (blk_n_ == NBLK) {
                blocks_.push_back(blk_acc_ / NBLK);
                blk_acc_ = 0;
                blk_n_ = 0;
                if (blocks_.size() >= NBLOCKS) {
                    auto mid = blocks_.begin() + static_cast<long>(blocks_.size() / 2);
                    std::nth_element(blocks_.begin(), mid, blocks_.end());
                    // the 8-bit receiver's floor: never below a quarter LSB^2
                    const double med = std::max<double>(*mid, 0.25 / (127.5 * 127.5));
                    noise_ = noise_ > 0 ? 0.7 * noise_ + 0.3 * med : med;
                    blocks_.clear();
                    thr_ = static_cast<float>(noise_ * std::pow(10.0, thr_db_ / 10));
                }
            }
        }
        if (thr_ > 0) {
            const float t = thr_;
            for (size_t j = 0; j < len; j++) {
                const bool act = P[j] > t;
                if (!act && !in_seg_) continue;
                const int64_t nn = n_ + static_cast<int64_t>(j);
                if (act) {
                    if (!in_seg_) {
                        in_seg_ = true;
                        seg_a_ = nn;
                    }
                    last_act_ = nn;
                    if (nn - seg_a_ > MAX_SEG) seg_a_ = nn - 64;   // a carrier: keep only its tail
                } else if (nn - last_act_ > GAP) {
                    segment(seg_a_, last_act_, out);
                    in_seg_ = false;
                }
            }
        }
        n_ += static_cast<int64_t>(len);
        i += len;
    }
}

bool FrontEnd::clipped(int64_t a, int64_t b) const {
    for (int64_t j = a; j <= b; j++) {
        const kp::cf& v = at(j);
        if (std::fabs(v.real()) >= clip_ || std::fabs(v.imag()) >= clip_) return true;
    }
    return false;
}

// One active run [a, b] (absolute sample indexes): its long part (P6, on the
// 5-sample mean) and its short pulses (local peaks of the raw power, edges
// at a quarter of the peak)
void FrontEnd::segment(int64_t a, int64_t b, std::vector<Pulse>& out) {
    const int64_t ra = std::max<int64_t>(a - 3, n_ - RING + 8);
    const int64_t rb = b + 3;
    if (rb - ra < 2) return;
    const int len = static_cast<int>(rb - ra + 1);
    auto p = [&](int j) { return P(ra + std::clamp(j, 0, len - 1)); };
    m5_.resize(static_cast<size_t>(len));
    used_.assign(static_cast<size_t>(len), 0);
    float acc = 0;
    for (int j = -2; j <= 2; j++) acc += p(j);
    for (int j = 0; j < len; j++) {
        m5_[static_cast<size_t>(j)] = acc / 5;
        acc += p(j + 3) - p(j - 2);
    }
    const size_t first_out = out.size();
    // long part: the 5-sample mean over the threshold (3 dB lower) for >= 10 us
    const float tl = thr_ * 0.5f;
    for (int j = 0; j < len;) {
        if (m5_[static_cast<size_t>(j)] < tl) {
            j++;
            continue;
        }
        int k = j;
        while (k + 1 < len && m5_[static_cast<size_t>(k + 1)] >= tl) k++;
        if ((k - j + 1) * US >= 10.0) {
            // median power over the run, edges at a quarter of it (raw power)
            std::vector<float> v;
            v.reserve(static_cast<size_t>(k - j + 1));
            for (int q = j + 2; q <= k - 2; q++) v.push_back(p(q));
            std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
            const float med = v[v.size() / 2];
            const float lvl = med / 4;
            int l = std::max(0, j - 3);
            while (l < k && p(l) < lvl) l++;
            // the mean bridges P1 -> P2 (1.2 us gap) and P2 -> P6 (0.7 us):
            // split at the first deep valley in the run's first 3 us - the
            // pulses before it are short ones (P1, or P2 when P1 stood apart)
            for (int q = l + 1; q < len && (q - l) * US <= 3.0; q++)
                if (p(q) < med / 12) {
                    while (q < k && p(q) < lvl) q++;
                    l = q;
                    break;
                }
            int r = std::min(len - 1, k + 3);
            while (r > l && p(r) < lvl) r--;
            double le = l, te = r;
            if (l > 0) le = l - 1 + std::clamp((lvl - p(l - 1)) / std::max(p(l) - p(l - 1), 1e-20f), 0.0f, 1.0f);
            if (r < len - 1) te = r + std::clamp((p(r) - lvl) / std::max(p(r) - p(r + 1), 1e-20f), 0.0f, 1.0f);
            float pk = 0;
            for (int q = l; q <= r; q++) pk = std::max(pk, p(q));
            Pulse u;
            u.le = static_cast<double>(ra) + le;
            u.te = static_cast<double>(ra) + te;
            u.peak = pk;
            u.med = med;
            u.db = static_cast<float>(10 * std::log10(std::max<double>(med, 1e-20) / noise_));
            u.longp = true;
            u.clipped = clipped(ra + l, ra + r);
            out.push_back(u);
            for (int q = l; q <= r; q++) used_[static_cast<size_t>(q)] = 1;
        }
        j = k + 1;
    }
    // short pulses: local peaks over the threshold, strongest first
    std::vector<std::pair<float, int>> peaks;
    for (int j = 0; j < len; j++) {
        if (used_[static_cast<size_t>(j)]) continue;
        const float v = p(j);
        if (v <= thr_) continue;
        if ((j > 0 && p(j - 1) > v) || (j + 1 < len && p(j + 1) >= v)) continue;
        peaks.push_back({v, j});
    }
    std::sort(peaks.begin(), peaks.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (auto [pk, j] : peaks) {
        if (used_[static_cast<size_t>(j)]) continue;
        const float lvl = pk / 4;
        // walk out to a quarter of the peak; stop at a valley (another pulse rising)
        int l = j;
        while (l > 0 && !used_[static_cast<size_t>(l - 1)] && p(l - 1) >= lvl && !(p(l - 1) > p(l) && p(l) < 0.5f * pk)) l--;
        int r = j;
        while (r + 1 < len && !used_[static_cast<size_t>(r + 1)] && p(r + 1) >= lvl && !(p(r + 1) > p(r) && p(r) < 0.5f * pk)) r++;
        double le = l, te = r;
        if (l > 0 && p(l - 1) < lvl) le = l - 1 + std::clamp((lvl - p(l - 1)) / std::max(p(l) - p(l - 1), 1e-20f), 0.0f, 1.0f);
        if (r + 1 < len && p(r + 1) < lvl) te = r + std::clamp((p(r) - lvl) / std::max(p(r) - p(r + 1), 1e-20f), 0.0f, 1.0f);
        for (int q = l; q <= r; q++) used_[static_cast<size_t>(q)] = 1;
        Pulse u;
        u.le = static_cast<double>(ra) + le;
        u.te = static_cast<double>(ra) + te;
        u.peak = pk;
        u.db = static_cast<float>(10 * std::log10(std::max<double>(pk, 1e-20) / noise_));
        u.clipped = clipped(ra + l, ra + r);
        out.push_back(u);
    }
    std::sort(out.begin() + static_cast<long>(first_out), out.end(), [](const Pulse& x, const Pulse& y) { return x.le < y.le; });
}

}  // namespace ssr
