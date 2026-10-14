#include "pulse.hpp"

#include <algorithm>
#include <complex>

namespace radar {

namespace {
constexpr double LONG_DB = 6.0;      // the long sum's threshold: this much below the short one's
constexpr double STRONG_DB = 20.0;   // a pulse this strong is measured on the raw power (no smoothing)
}

Detector::Detector() : hx_(static_cast<size_t>(HN)), hp_(static_cast<size_t>(HN), 0.0f) {
    blocks_.reserve(NBLOCKS);
}

void Detector::reset() {
    std::fill(hp_.begin(), hp_.end(), 0.0f);
    n_ = 0;
    blk_acc_ = 0;
    blk_n_ = 0;
    blocks_.clear();
    noise_ = 0;
    thr3_ = thr24_ = 0;
    in_seg_ = false;
    long_segs_ = 0;
}

void Detector::set_threshold_db(double db) {
    thr_db_ = db;
    thresholds();
}

void Detector::thresholds() {
    if (noise_ <= 0) return;
    thr3_ = static_cast<float>(noise_ * W_S * std::pow(10.0, thr_db_ / 10));
    thr24_ = static_cast<float>(noise_ * W_L * std::pow(10.0, (thr_db_ - LONG_DB) / 10));
}

// In chunks that don't wrap the ring: the samples and their power stored
// (vectorised), the 64-sample noise means, the two sums (a float chain each,
// exact again at every chunk start) and the segment state machine
void Detector::push(const kp::cf* x, size_t n, double t0, std::vector<Pulse>& out) {
    t_ref_ = t0 - static_cast<double>(n_) / FS;
    constexpr size_t CH = 4096;
    size_t i = 0;
    while (i < n) {
        const size_t k0 = static_cast<size_t>(n_ & HM);
        const size_t len = std::min({n - i, static_cast<size_t>(HN) - k0, CH});
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
                    const double med = std::max<double>(*mid, 1e-14);
                    noise_ = noise_ > 0 ? 0.7 * noise_ + 0.3 * med : med;
                    blocks_.clear();
                    thresholds();
                }
            }
        }
        if (thr3_ > 0) {
            // the sums of the samples before the chunk's first, exact; then running
            const int64_t a = n_;
            float s3 = 0, s24 = 0;
            for (int q = 1; q < W_L; q++) {
                const float v = hp_[static_cast<size_t>((a - q) & HM)];
                s24 += v;
                if (q < W_S) s3 += v;
            }
            const float t3 = thr3_, t24 = thr24_;
            for (size_t j = 0; j < len; j++) {
                const float p = P[j];
                s3 += p;
                s24 += p;
                const bool act = s3 > t3 || s24 > t24;
                s3 -= j >= W_S - 1 ? P[j - (W_S - 1)] : hp_[static_cast<size_t>((a + static_cast<int64_t>(j) - (W_S - 1)) & HM)];
                s24 -= j >= W_L - 1 ? P[j - (W_L - 1)] : hp_[static_cast<size_t>((a + static_cast<int64_t>(j) - (W_L - 1)) & HM)];
                if (!act && !in_seg_) continue;
                const int64_t nn = a + static_cast<int64_t>(j);
                if (act) {
                    if (!in_seg_) {
                        in_seg_ = true;
                        seg_a_ = nn;
                    }
                    last_act_ = nn;
                    if (nn - seg_a_ > MAX_LEN) {   // a carrier / interference, not a pulse
                        long_segs_++;
                        seg_a_ = nn;
                    }
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

// The pulses of one active run [a, b] (absolute indices; the sums trail the
// samples by up to W_L).
// Pulse support: the 3-sample mean over the short threshold OR the 9-sample
// mean over the long one (a weak chirp's sidelobe pulse dips under the short
// threshold every few us as it fades across its sweep). Runs of support
// apart by more than SPLIT are separate pulses. Each one's edges: the
// OUTERMOST -6 dB crossings of its peak (on the raw power for a strong
// short pulse, the 3-sample mean, or the 9-sample mean for a weak one),
// corrected for the smoothing (W / 4 per side for a rectangular pulse)
void Detector::segment(int64_t a, int64_t b, std::vector<Pulse>& out) {
    const int64_t ra = std::max<int64_t>(a - W_L - 4, n_ - HN + 16);
    const int64_t rb = b + 5;   // b <= n_ - GAP - 1
    if (rb - ra < 3) return;
    const int len = static_cast<int>(rb - ra);
    const float thr_s = static_cast<float>(noise_ * std::pow(10.0, thr_db_ / 10));
    const float thr_l = static_cast<float>(noise_ * std::pow(10.0, (thr_db_ - LONG_DB) / 10));
    const size_t first_out = out.size();
    auto Pc = [&](int j) { return P(ra + std::clamp(j, 0, len - 1)); };
    // 3- and 9-sample means of the power (centred)
    e_.resize(static_cast<size_t>(len));
    e9_.resize(static_cast<size_t>(len));
    float acc = 0;
    for (int j = -4; j <= 4; j++) acc += Pc(j);
    for (int j = 0; j < len; j++) {
        e_[static_cast<size_t>(j)] = (Pc(j - 1) + Pc(j) + Pc(j + 1)) / 3;
        e9_[static_cast<size_t>(j)] = acc / 9;
        acc += Pc(j + 5) - Pc(j - 4);
    }
    constexpr int SPLIT = 10;   // 4.2 us
    std::vector<std::pair<int, int>> runs;
    for (int j = 0; j < len; j++) {
        if (e_[static_cast<size_t>(j)] < thr_s && e9_[static_cast<size_t>(j)] < thr_l) continue;
        if (!runs.empty() && j - runs.back().second <= SPLIT) runs.back().second = j;
        else runs.emplace_back(j, j);
    }
    if (runs.size() > 8) runs.resize(8);
    Pulse p;
    for (auto [u0, u1] : runs) {
        float pk3 = 0;
        for (int j = u0; j <= u1; j++) pk3 = std::max(pk3, e_[static_cast<size_t>(j)]);
        // which envelope: raw power (a strong short pulse), 3- or 9-sample mean
        const int W = pk3 < thr_s ? 9 : (u1 - u0 <= 12 && pk3 >= 10 * thr_s) ? 1 : 3;
        // only the long sum's support: a long pulse (>= 10 us), not the tail
        // of another one or a few noisy samples
        if (W == 9 && u1 - u0 < W_L) continue;
        auto env = [&](int j) { return W == 9 ? e9_[static_cast<size_t>(j)] : W == 3 ? e_[static_cast<size_t>(j)] : Pc(j); };
        const int r0 = std::max(0, u0 - 4), r1 = std::min(len - 1, u1 + 4);
        float pk = 0;
        for (int j = r0; j <= r1; j++) pk = std::max(pk, env(j));
        if (!(pk > 0)) continue;
        const float lvl = pk / 4;
        int left = r0, right = r1;
        while (left < r1 && env(left) < lvl) left++;
        while (right > left && env(right) < lvl) right--;
        double le = left, te = right;
        if (left > 0) {
            const double e0 = env(left - 1), e1 = env(left);
            le = left - 1 + std::clamp((lvl - e0) / std::max(e1 - e0, 1e-20), 0.0, 1.0);
        }
        if (right < len - 1) {
            const double e0 = env(right), e1 = env(right + 1);
            te = right + std::clamp((e0 - lvl) / std::max(e0 - e1, 1e-20), 0.0, 1.0);
        }
        const double bias = W / 4.0;
        if (te - le > 4 * bias) {
            le += bias;
            te -= bias;
        }
        if (measure(static_cast<double>(ra) + le, static_cast<double>(ra) + te, &p)) out.push_back(p);
    }
    // in time order
    std::sort(out.begin() + static_cast<long>(first_out), out.end(), [](const Pulse& x, const Pulse& y) { return x.toa < y.toa; });
}

bool Detector::measure(double le, double te, Pulse* p) {
    const double width_s = te - le;
    if (width_s < 0.7) return false;   // < 0.3 us: a noise spike
    const int64_t a = static_cast<int64_t>(std::ceil(le)), b = std::max(a, static_cast<int64_t>(std::floor(te)));
    // raw samples of the pulse (+ the edge samples)
    float pkraw = 0;
    bool clipped = false;
    for (int64_t j = a - 1; j <= b + 1; j++) {
        const size_t q = static_cast<size_t>(j & HM);
        pkraw = std::max(pkraw, hp_[q]);
        if (std::fabs(hx_[q].real()) >= clip_ || std::fabs(hx_[q].imag()) >= clip_) clipped = true;
    }
    // frequency: phase steps over the pulse; LFM: 6-step pieces fitted against time
    std::complex<double> z{0, 0};
    double sw = 0, st = 0, sf = 0, stt = 0, stf = 0;
    std::vector<double>& fj = fit_f_;
    std::vector<double>& tj = fit_t_;
    std::vector<double>& wj = fit_w_;
    fj.clear();
    tj.clear();
    wj.clear();
    std::complex<double> blk{0, 0};
    int nb = 0;
    double prev_f = NAN;
    for (int64_t j = a - 1; j <= b; j++) {
        const std::complex<double> d =
            std::complex<double>(hx_[static_cast<size_t>((j + 1) & HM)]) * std::conj(std::complex<double>(hx_[static_cast<size_t>(j & HM)]));
        z += d;
        blk += d;
        if (++nb == 6) {
            double f = std::arg(blk) / (2 * M_PI) * FS;
            if (std::isfinite(prev_f)) {   // unwrap against the previous piece
                while (f - prev_f > FS / 2) f -= FS;
                while (f - prev_f < -FS / 2) f += FS;
            }
            prev_f = f;
            const double w = std::abs(blk);
            fj.push_back(f);
            tj.push_back(static_cast<double>(j - a) - 1.5);   // centre of the 6 steps (in samples)
            wj.push_back(w);
            sw += w;
            st += w * tj.back();
            sf += w * f;
            stt += w * tj.back() * tj.back();
            stf += w * tj.back() * f;
            blk = 0;
            nb = 0;
        }
    }
    p->freq_hz = std::arg(z) / (2 * M_PI) * FS;
    p->chirp = NAN;
    p->fit_rms = NAN;
    if (fj.size() >= 3 && sw > 0) {
        const double tm = st / sw, fm = sf / sw;
        const double var = stt / sw - tm * tm;
        if (var > 0) {
            const double rate = (stf / sw - tm * fm) / var;   // Hz per sample
            double r2 = 0;
            for (size_t i = 0; i < fj.size(); i++) {
                const double e = fj[i] - (fm + rate * (tj[i] - tm));
                r2 += wj[i] * e * e;
            }
            p->chirp = rate * FS / 1e6;   // Hz per us
            p->fit_rms = std::sqrt(r2 / sw);
        }
    }
    p->width_us = width_s / FS * 1e6;
    p->span_hz = std::isfinite(p->chirp) ? p->chirp * p->width_us : 0;
    p->bw_hz = std::clamp(std::fabs(p->span_hz) + 2e6 / p->width_us, 50e3, FS);
    p->toa = t_of(le);
    p->peak_db = 10 * std::log10(std::max<double>(pkraw, 1e-20) / noise_);
    p->peak_dbfs = 10 * std::log10(std::max<double>(pkraw, 1e-20));
    p->clipped = clipped;
    p->t_a = t_of(le - MARGIN);
    p->t_b = t_of(te + MARGIN);
    return true;
}

}  // namespace radar
