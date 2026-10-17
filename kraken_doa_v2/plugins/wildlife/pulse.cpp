#include "pulse.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wl {

namespace {
constexpr double TARGET_BIN_HZ = 250;   // FFT bin width aimed at
constexpr double LONG_S = 0.014;         // the long detector's integration
constexpr double RING_S = 0.6;           // raw samples kept (a pulse is measured from them)
constexpr double MAX_SPAN_S = 0.25;      // longer = a carrier switched on, not a pulse
constexpr double MIN_WIDTH_S = 0.001, MAX_WIDTH_S = 0.15;
constexpr double MARGIN_S = 0.003;
constexpr double R1_HZ = 32000;          // the measurement's rate (after the boxcar)
constexpr double FIR_CUT_HZ = 1000;      // its +-1 kHz channel
// thresholds (power over floor): short = one frame (exponential: e^-T per
// test), long = the mean of L frames (50 % overlapped Hann frames); gate =
// inside a known collar's expected window
constexpr float T_SHORT = 19.0f, T_LONG = 5.0f, T_GATE_LONG = 4.0f, T_GATE_SHORT = 14.0f;
constexpr float T_CONT = 3.0f;           // keeps a running pulse (either detector)
constexpr float FLOOR_ALPHA_S = 1.0f;    // floor time constant (s)
constexpr float FLOOR_GATE = 4.0f;       // a bin over 4x its floor doesn't update it ...
constexpr float FLOOR_CREEP_DB_S = 1.0f; // ... but creeps it up (a steady carrier)
}  // namespace

PulseDetector::~PulseDetector() {
    if (plan_) kfft_destroy_plan(plan_);
    if (fin_) kfft_free(fin_);
    if (fout_) kfft_free(fout_);
}

void PulseDetector::configure(double rate) {
    if (plan_) kfft_destroy_plan(plan_);
    if (fin_) kfft_free(fin_);
    if (fout_) kfft_free(fout_);
    plan_ = nullptr;
    rate_ = rate;
    const int lg = std::clamp(static_cast<int>(std::lround(std::log2(rate / TARGET_BIN_HZ))), 5, 14);
    N_ = 1 << lg;
    hop_ = N_ / 2;
    L_ = std::max(1, static_cast<int>(std::lround(LONG_S * rate / hop_)));
    win_.resize(static_cast<size_t>(N_));
    win_pow_ = 0;
    for (int i = 0; i < N_; i++) {
        win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N_));
        win_pow_ += win_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
    }
    fin_ = kfft_alloc_complex(static_cast<size_t>(N_));
    fout_ = kfft_alloc_complex(static_cast<size_t>(N_));
    // (pffft plans are made at once - FFTW_MEASURE planned for 6 s (256
    // points) / 23 s (8192) on the Pi 5 before the plugin read its first
    // sample, and kraken_doa dropped what came meanwhile)
    plan_ = kfft_plan_dft_1d(N_, fin_, fout_, KFFT_FORWARD);
    size_t cap = 1;
    while (cap < static_cast<size_t>(rate * RING_S)) cap <<= 1;
    ring_.assign(cap, kp::cf(0, 0));
    mask_ = cap - 1;
    used_.assign(static_cast<size_t>(N_), 0);
    for (int k = 0; k < N_; k++) used_[static_cast<size_t>(k)] = std::fabs(bin_freq(k)) <= 0.46 * rate ? 1 : 0;
    reset();
}

void PulseDetector::set_sensitivity(int s) { sens_ = std::clamp(s, -2, 2); }

void PulseDetector::reset() {
    abs_ = 0;
    next_frame_ = static_cast<uint64_t>(N_);
    frames_ = 0;
    P_.assign(static_cast<size_t>(N_), 0.0f);
    floor_.assign(static_cast<size_t>(N_), 0.0f);
    sum_.assign(static_cast<size_t>(N_), 0.0f);
    hist_.assign(static_cast<size_t>(N_) * static_cast<size_t>(L_), 0.0f);
    act_.clear();
    dead_.clear();
    gates_.clear();
    std::fill(ring_.begin(), ring_.end(), kp::cf(0, 0));
}

float PulseDetector::cn0_ref() const {
    // the long detector's threshold for a pulse filling L frames: per-frame
    // SNR (T_LONG - 1) in one bin; C/N0 = SNR x ENBW (1.5 bins) x bin
    const float s = (T_LONG - 1.0f) * std::pow(10.0f, -0.2f * static_cast<float>(sens_));
    return 10.0f * std::log10(s * 1.5f * static_cast<float>(rate_ / N_));
}

void PulseDetector::push(const kp::cf* x, size_t n, std::vector<Pulse>& out) {
    if (!plan_) return;
    for (size_t i = 0; i < n; i++) {
        ring_[static_cast<size_t>(abs_ & mask_)] = x[i];
        abs_++;
        if (abs_ == next_frame_) {
            frame(out);
            next_frame_ += static_cast<uint64_t>(hop_);
        }
    }
}

void PulseDetector::frame(std::vector<Pulse>& out) {
    const uint64_t end = abs_;
    const uint64_t s0 = end - static_cast<uint64_t>(N_);
    for (int i = 0; i < N_; i++) {
        const kp::cf v = ring_[static_cast<size_t>((s0 + static_cast<uint64_t>(i)) & mask_)] * win_[static_cast<size_t>(i)];
        fin_[i][0] = v.real();
        fin_[i][1] = v.imag();
    }
    kfft_execute(plan_);
    const float fps = static_cast<float>(rate_ / hop_);
    const float a = frames_ < 32 ? 1.0f / static_cast<float>(frames_ + 1) : 1.0f / (FLOOR_ALPHA_S * fps);
    const float creep = std::pow(10.0f, FLOOR_CREEP_DB_S / 10.0f / fps);
    const float gain = std::pow(10.0f, 0.2f * static_cast<float>(sens_));   // +1 = 2 dB lower thresholds
    const float ts = T_SHORT / gain, tl = 1.0f + (T_LONG - 1.0f) / gain;
    const float invL = 1.0f / static_cast<float>(L_);
    float* __restrict hrow = hist_.data() + static_cast<size_t>(frames_ % static_cast<uint64_t>(L_)) * static_cast<size_t>(N_);
    float* __restrict P = P_.data();
    float* __restrict F = floor_.data();
    float* __restrict S = sum_.data();
    const bool warm = frames_ >= static_cast<uint64_t>(std::max(32, L_ + 4));
    const bool init = frames_ < 32;
    const float tsl = tl * static_cast<float>(L_);
    const float* __restrict X = reinterpret_cast<const float*>(fout_);
    const uint8_t* __restrict U = used_.data();
    cand_.clear();
    for (int k = 0; k < N_; k++) {
        const float re = X[2 * k], im = X[2 * k + 1];
        const float p = re * re + im * im;
        P[k] = p;
        const float sk = S[k] + p - hrow[k];
        S[k] = sk;
        hrow[k] = p;
        const float f0 = F[k];
        const float f = std::max((init | (p < FLOOR_GATE * f0)) ? f0 + a * (p - f0) : f0 * creep, 1e-30f);
        F[k] = f;
        if ((p > ts * f) | (sk > tsl * f)) [[unlikely]]
            if (warm && U[k]) cand_.push_back(k);
    }
    // a known collar's expected window: lower thresholds there
    std::vector<int> gated;
    if (warm)
        for (const Gate& g : gates_) {
            if (end < g.from || end > g.to + static_cast<uint64_t>(N_)) continue;
            const int kc = static_cast<int>(std::lround(g.freq / (rate_ / N_)));
            for (int d = -2; d <= 2; d++) {
                const int k = ((kc + d) % N_ + N_) % N_;
                if (!used_[static_cast<size_t>(k)]) continue;
                if (P[k] > T_GATE_SHORT / gain * F[k] || S[k] * invL > (1.0f + (T_GATE_LONG - 1.0f) / gain) * F[k]) {
                    gated.push_back(k);
                    cand_.push_back(k);
                }
            }
        }
    frames_++;
    // broadband frame (an impulse): nothing from it
    int nused = 0;
    for (int k = 0; k < N_; k += 16) nused += used_[static_cast<size_t>(k)];
    const size_t lim = std::max<size_t>(12, static_cast<size_t>(nused) * 16 / 4);   // a quarter of the band at once
    if (cand_.size() > lim) {
        blanked_++;
        cand_.clear();
    }
    std::sort(cand_.begin(), cand_.end());
    cand_.erase(std::unique(cand_.begin(), cand_.end()), cand_.end());
    auto ratio = [&](int k) {
        k = (k % N_ + N_) % N_;
        return std::max(P[k] / ts, S[k] * invL / tl) / F[k];
    };
    const uint64_t fi = frames_ - 1;
    for (int k : cand_) {
        // local maximum over +-2 bins
        const float r = ratio(k);
        bool mx = true;
        for (int d = -2; d <= 2 && mx; d++)
            if (d && ratio(k + d) > r) mx = false;
        if (!mx) continue;
        // a narrow line: the bins 4..10 away on both sides stay low
        float side = 0;
        for (int d = 4; d <= 10; d++) side += P[((k + d) % N_ + N_) % N_] / F[((k + d) % N_ + N_) % N_] +
                                              P[((k - d) % N_ + N_) % N_] / F[((k - d) % N_ + N_) % N_];
        side /= 14.0f;
        const float pk = std::max(P[k], S[k] * invL) / F[k];
        const bool narrow = side < std::max(2.5f, 0.12f * pk);
        const bool g = std::find(gated.begin(), gated.end(), k) != gated.end();
        Active* hit = nullptr;
        for (Active& ac : act_) {
            int d = std::abs(ac.bin - k);
            d = std::min(d, N_ - d);
            if (d <= 3) { hit = &ac; break; }
        }
        if (!hit) {
            act_.push_back({});
            hit = &act_.back();
            hit->bin = k;
            hit->f_first = fi;
            hit->narrow = narrow;
        }
        hit->f_last = fi;
        hit->gated = hit->gated || g;
        if (pk > hit->peak) {
            hit->peak = pk;
            hit->bin = k;
            hit->f_peak = fi;
            hit->narrow = narrow;
            // parabolic offset of the peak (log power)
            const float l0 = std::log(P[((k - 1) % N_ + N_) % N_] + 1e-30f), l1 = std::log(P[k] + 1e-30f),
                        l2 = std::log(P[(k + 1) % N_] + 1e-30f);
            const float den = l0 - 2 * l1 + l2;
            hit->pk_off = den < 0 ? std::clamp(0.5f * (l0 - l2) / den, -0.5f, 0.5f) : 0.0f;
        }
    }
    // continuation: a running pulse stays on while its bins hold T_CONT
    for (Active& ac : act_) {
        if (ac.f_last == fi) continue;
        for (int d = -1; d <= 1; d++) {
            const int k = ((ac.bin + d) % N_ + N_) % N_;
            if (std::max(P[k], S[k] * invL) > T_CONT * F[k]) { ac.f_last = fi; break; }
        }
    }
    // finished: 2 frames without
    for (size_t i = 0; i < act_.size();) {
        if (fi > act_[i].f_last + 2) {
            const Active ac = act_[i];
            act_.erase(act_.begin() + static_cast<long>(i));
            finish(ac, out);
        } else if ((fi - act_[i].f_first) * static_cast<uint64_t>(hop_) > static_cast<uint64_t>(MAX_SPAN_S * rate_)) {
            // a carrier: wait until it ends, but measure nothing
            act_[i].narrow = false;
            act_[i].f_first = fi - static_cast<uint64_t>(MAX_SPAN_S * rate_ / hop_);
            i++;
        } else {
            i++;
        }
    }
    // pulses reported lately (a duplicate from the long sum's memory)
    dead_.erase(std::remove_if(dead_.begin(), dead_.end(),
                               [&](const Seen& d) { return d.t1 + rate_ < static_cast<double>(abs_); }),
                dead_.end());
    // the floor median, for the facts (every ~1 s)
    if (frames_ % static_cast<uint64_t>(std::max(1.0f, fps)) == 0) {
        std::vector<float> v;
        for (int k = 0; k < N_; k += std::max(1, N_ / 256))
            if (used_[static_cast<size_t>(k)]) v.push_back(F[k]);
        if (!v.empty()) {
            std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
            floor_med_db_ = 10.0f * std::log10(v[v.size() / 2] / win_pow_);
        }
    }
}

void PulseDetector::finish(const Active& ac, std::vector<Pulse>& out) {
    if (!ac.narrow) {
        rej_wide_++;
        return;
    }
    // the samples: from the first frame the long detector could have seen
    // the pulse in, to the last frame it was on
    const int64_t first = static_cast<int64_t>(ac.f_first) - L_ + 1;
    const int64_t e_first = static_cast<int64_t>(N_) + std::max<int64_t>(0, first) * hop_;
    const int64_t margin = static_cast<int64_t>(MARGIN_S * rate_);
    int64_t sa = e_first - N_ - margin;
    int64_t sb = static_cast<int64_t>(N_) + static_cast<int64_t>(ac.f_last) * hop_ + margin;
    const int64_t oldest = static_cast<int64_t>(abs_) - static_cast<int64_t>(ring_.size()) + 1;
    sa = std::max(sa, std::max<int64_t>(0, oldest));
    sb = std::min<int64_t>(sb, static_cast<int64_t>(abs_));
    if (sb - sa > static_cast<int64_t>(MAX_SPAN_S * rate_) + 2 * margin || sb - sa < static_cast<int64_t>(0.004 * rate_)) {
        rej_len_++;
        return;
    }
    const double fc = bin_freq(ac.bin + ac.pk_off);
    // mix + boxcar to ~32 kHz
    const int D1 = std::max(1, static_cast<int>(rate_ / R1_HZ));
    const double r1 = rate_ / D1;
    const size_t M = static_cast<size_t>((sb - sa) / D1);
    z_.assign(M, kp::cf(0, 0));
    const double w = -2 * M_PI * fc / rate_;
    std::complex<double> rot = std::polar(1.0, std::remainder(w * static_cast<double>(sa), 2 * M_PI));
    const std::complex<double> step = std::polar(1.0, w);
    for (size_t m = 0; m < M; m++) {
        std::complex<double> acc(0, 0);
        for (int i = 0; i < D1; i++) {
            const kp::cf v = ring_[static_cast<size_t>(static_cast<uint64_t>(sa + static_cast<int64_t>(m) * D1 + i) & mask_)];
            acc += std::complex<double>(v.real(), v.imag()) * rot;
            rot *= step;
        }
        rot /= std::abs(rot);
        z_[m] = kp::cf(static_cast<float>(acc.real() / D1), static_cast<float>(acc.imag() / D1));
    }
    // +-1 kHz Hamming sinc at r1
    const int T = 2 * static_cast<int>(std::ceil(1.65 * r1 / FIR_CUT_HZ)) + 1;
    if (static_cast<int>(M) < T + 8) {
        rej_len_++;
        return;
    }
    std::vector<float> h(static_cast<size_t>(T));
    double hs = 0;
    for (int i = 0; i < T; i++) {
        const double xx = i - (T - 1) / 2.0, fcn = FIR_CUT_HZ / r1;
        const double s = xx == 0 ? 2 * fcn : std::sin(2 * M_PI * fcn * xx) / (M_PI * xx);
        h[static_cast<size_t>(i)] = static_cast<float>(s * (0.54 - 0.46 * std::cos(2 * M_PI * i / (T - 1))));
        hs += h[static_cast<size_t>(i)];
    }
    for (auto& v : h) v = static_cast<float>(v / hs);
    const size_t half = static_cast<size_t>(T / 2);
    const size_t nw = M - 2 * half;
    w_.assign(nw, kp::cf(0, 0));
    for (size_t m = 0; m < nw; m++) {
        kp::cf acc(0, 0);
        for (int t = 0; t < T; t++) acc += z_[m + static_cast<size_t>(t)] * h[static_cast<size_t>(t)];
        w_[m] = acc;   // centred on z_[m + half]
    }
    // power: light (~0.3 ms, the edges) and heavy (~2 ms, finding a weak pulse) smoothing
    std::vector<float> pw(nw);
    for (size_t m = 0; m < nw; m++) pw[m] = std::norm(w_[m]);
    auto smooth = [&](size_t K, std::vector<float>& o) {
        o.assign(nw, 0.0f);
        double run = 0;
        for (size_t m = 0; m < nw + K / 2; m++) {
            if (m < nw) run += pw[m];
            if (m >= K) run -= pw[m - K];
            if (m >= K / 2 && m - K / 2 < nw) o[m - K / 2] = static_cast<float>(run / static_cast<double>(K));
        }
    };
    smooth(std::max<size_t>(1, static_cast<size_t>(0.0003 * r1)), e_);
    std::vector<float> eh;
    smooth(std::max<size_t>(1, static_cast<size_t>(0.002 * r1)), eh);
    // the noise in this channel from the bin's floor: N0 x r1 x sum h^2
    double hh = 0;
    for (float v : h) hh += static_cast<double>(v) * v;
    const int kb = ((ac.bin % N_) + N_) % N_;
    const double n0 = floor_[static_cast<size_t>(kb)] / (win_pow_ * rate_);   // per Hz
    const float n2 = static_cast<float>(std::max(n0 * r1 * hh, 1e-30));
    // the pulses = maximum-sum runs: first of (heavy - 2 x noise) - a weak
    // pulse - then of (light - the midpoint between the noise and that
    // pulse's power) around it - the edges. A coded burst's 4 pulses (one
    // span) one after the other
    auto best_run = [&](const std::vector<float>& e, float th, const std::vector<uint8_t>& taken, size_t lo, size_t hi,
                        size_t* a, size_t* b) {
        double cur = 0, best = 0;
        size_t ca = lo;
        bool any = false;
        for (size_t m = lo; m < hi; m++) {
            if (taken[m]) { cur = 0; ca = m + 1; continue; }
            if (cur <= 0) { cur = 0; ca = m; }
            cur += e[m] - th;
            if (cur > best) { best = cur; *a = ca; *b = m; any = true; }
        }
        return any ? best : 0.0;
    };
    std::vector<uint8_t> taken(nw, 0);
    const kp::cf* wv = w_.data();
    int found = 0, dups = 0;
    float a2_first = 0;
    const size_t ms3 = static_cast<size_t>(0.006 * r1), ms1 = static_cast<size_t>(0.005 * r1);
    const float ehmax = *std::max_element(eh.begin(), eh.end());
    for (int it = 0; it < 6; it++) {
        size_t ca = 0, cb = 0;
        // high enough for strong pulses not to be joined across their gaps
        // (a coded burst), 3 dB over the noise for weak ones
        float hth = std::max(2.0f * n2, 0.3f * ehmax);
        if (found) hth = std::max(hth, 0.3f * a2_first);
        if (best_run(eh, hth, taken, 0, nw, &ca, &cb) <= 0) break;
        double s = 0;
        for (size_t m = ca; m <= cb; m++) s += e_[m];
        const float a2 = static_cast<float>(s / static_cast<double>(cb - ca + 1));
        // significant: excess power x sqrt(independent samples, ~2 kHz)
        const double sig = (a2 - n2) / n2 * std::sqrt(std::max(1.0, (cb - ca + 1) / r1 * 2000.0));
        if (a2 < 1.6f * n2 || sig < (found ? 6.0 : 4.0) || (found && a2 < 0.4f * a2_first)) break;
        const float th = 0.5f * (a2 + n2);
        size_t a = 0, b = 0;
        const size_t lo = ca > ms1 ? ca - ms1 : 0, hi = std::min(nw, cb + ms1 + 1);
        if (best_run(e_, th, taken, lo, hi, &a, &b) <= 0) break;
        for (size_t m = (a > ms3 ? a - ms3 : 0); m <= std::min(nw - 1, b + ms3); m++) taken[m] = 1;
        if (a == 0 || b + 1 >= nw) {   // runs off the measured span: not a clean pulse
            rej_len_++;
            continue;
        }
        const double lead = static_cast<double>(a) - 1.0 + (th - e_[a - 1]) / std::max(1e-30f, e_[a] - e_[a - 1]);
        const double trail = static_cast<double>(b) + (e_[b] - th) / std::max(1e-30f, e_[b] - e_[b + 1]);
        const double width_s = (trail - lead) / r1;
        if (width_s < MIN_WIDTH_S || width_s > MAX_WIDTH_S) {
            rej_len_++;
            continue;
        }
        // the same pulse again (a new detection from the long sum's memory of it)
        bool dup = false;
        const double lead_in = static_cast<double>(sa) + (lead + static_cast<double>(half)) * D1;
        for (const auto& d : dead_) {
            int dd = std::abs(d.bin - kb);
            dd = std::min(dd, N_ - dd);
            dup = dup || (dd <= 3 && lead_in > d.t0 - 0.001 * rate_ && lead_in < d.t1 + 0.001 * rate_);
        }
        if (!found) a2_first = a2;
        if (dup) {   // it still sets the level the others must reach
            found++;
            dups++;
            continue;
        }
        // frequency from the phase slope over the core, power over it
        const size_t in = static_cast<size_t>(0.0007 * r1);
        size_t c0 = a + in, c1 = b > in ? b - in : b;
        if (c1 <= c0 + 2) { c0 = a; c1 = b; }
        std::complex<double> ph(0, 0), ph1(0, 0), ph2(0, 0);
        double pwr = 0;
        size_t np = 0;
        const size_t cm = (c0 + c1) / 2;
        for (size_t m = c0 + 1; m <= c1; m++) {
            const std::complex<double> d = std::complex<double>(wv[m]) * std::conj(std::complex<double>(wv[m - 1]));
            ph += d;
            (m <= cm ? ph1 : ph2) += d;
            pwr += pw[m];
            np++;
        }
        // a keyed carrier switches on and off within ~1 ms: 2-5 ms outside
        // its edges a strong pulse is down at the noise (a line of an FM /
        // data signal fades in and out over tens of ms)
        if (a2 > 8 * n2) {
            auto mean_e = [&](double from, double to) {
                const long i0 = std::max<long>(0, static_cast<long>(from)), i1 = std::min<long>(static_cast<long>(nw) - 1, static_cast<long>(to));
                double sm = 0;
                long k = 0;
                for (long i = i0; i <= i1; i++, k++) sm += e_[static_cast<size_t>(i)];
                return k ? sm / k : 0.0;
            };
            // (2-5 ms out: the FIR and a collar's ~1 ms keying ramp reach ~1.5 ms)
            const double o1 = mean_e(lead - 0.005 * r1, lead - 0.002 * r1), o2 = mean_e(trail + 0.002 * r1, trail + 0.005 * r1);
            if (std::max(o1, o2) > n2 + 0.35 * (a2 - n2)) {
                rej_edge_++;
                continue;
            }
        }
        // ... and holds its level (an FSK burst's tone comes and goes with
        // the bits): the power's variance inside against what the noise gives
        {
            double sm = 0, sq = 0;
            size_t k = 0;
            for (size_t m = c0; m <= c1; m++, k++) {
                sm += pw[m];
                sq += static_cast<double>(pw[m]) * pw[m];
            }
            if (k >= 8) {
                const double mean = sm / static_cast<double>(k), var = sq / static_cast<double>(k) - mean * mean;
                const double S = std::max(mean - n2, 0.0);
                if (var > 2.0 * (2 * S * n2 + static_cast<double>(n2) * n2) + 0.1 * S * S) {
                    rej_var_++;
                    continue;
                }
            }
        }
        // a keyed carrier keeps its frequency through the pulse (an FM voice
        // / data signal's "pulse" doesn't): the two halves within 2 / T + 60 Hz
        if (width_s > 0.006) {
            const double f1 = std::arg(ph1) * r1 / (2 * M_PI), f2 = std::arg(ph2) * r1 / (2 * M_PI);
            if (std::fabs(f1 - f2) > 60 + 2 / width_s) {
                rej_fm_++;
                continue;
            }
        }
        Pulse p;
        // w_[m] is centred on z_[m + half] = input sample sa + (m + half) D1 + (D1 - 1) / 2
        auto to_in = [&](double m) { return static_cast<double>(sa) + (m + static_cast<double>(half)) * D1 + (D1 - 1) / 2.0; };
        p.t0 = to_in(lead);
        p.t1 = to_in(trail);
        p.freq = fc + std::arg(ph) * r1 / (2 * M_PI);
        const double pon = np ? pwr / static_cast<double>(np) : a2;
        const double ps = std::max(pon - n2, 0.05 * pon);
        p.power_db = static_cast<float>(10 * std::log10(std::max(ps, 1e-30)));
        p.cn0_db = static_cast<float>(10 * std::log10(std::max(ps, 1e-30) / std::max(n0, 1e-30)));
        p.peak_ratio = ac.peak;
        p.gated = ac.gated;
        out.push_back(p);
        dead_.push_back({kb, p.t0, p.t1});
        found++;
    }
    if (found == dups) rej_len_++;
}

}  // namespace wl
