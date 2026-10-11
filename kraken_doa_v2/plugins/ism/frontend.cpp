#include "frontend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ism {

namespace {
constexpr double BIN_HZ = FS / FrontEnd::NFFT;
inline int signed_bin(int k) { return k < FrontEnd::NFFT / 2 ? k : k - FrontEnd::NFFT; }

// runs of a two-state signal: (state, length); short runs merged away
struct Run {
    bool on;
    int64_t start;
    int64_t len;
};
void merge_glitches(std::vector<Run>& r, int64_t g) {
    if (r.size() < 3) return;
    std::vector<Run> o;
    o.reserve(r.size());
    for (size_t i = 0; i < r.size(); i++) {
        Run x = r[i];
        if (x.len < g && !o.empty() && i + 1 < r.size()) {
            // swallowed by its neighbours (which have the other state)
            o.back().len += x.len + r[i + 1].len;
            i++;
            continue;
        }
        if (!o.empty() && o.back().on == x.on) {
            o.back().len += x.len;
            continue;
        }
        o.push_back(x);
    }
    r.swap(o);
}

// OOK only: a dropout < 40 us next to a pulse at least twice as long (the
// envelope of a weak or fading carrier dipping under the threshold) and a
// spike < 22 us inside a gap are merged away - the glitch merge alone (18
// us) left 500 us pulses split into 75 us pieces (Rubicson recordings)
void merge_dropouts(std::vector<Run>& r) {
    const int64_t g2 = us2s(40), sp = us2s(22);
    std::vector<Run> o;
    o.reserve(r.size());
    for (size_t i = 0; i < r.size(); i++) {
        const Run& x = r[i];
        if (!o.empty() && i + 1 < r.size() &&
            ((!x.on && x.len < g2 && std::max(o.back().len, r[i + 1].len) >= 2 * x.len) || (x.on && x.len < sp))) {
            o.back().len += x.len + r[i + 1].len;
            i++;
            continue;
        }
        o.push_back(x);
    }
    r.swap(o);
}

std::vector<Pulse> to_pulses(const std::vector<Run>& r) {
    std::vector<Pulse> p;
    for (size_t i = 0; i < r.size(); i++) {
        if (r[i].on) {
            Pulse q;
            q.t = r[i].start;
            q.on = static_cast<int32_t>(r[i].len);
            q.off = i + 1 < r.size() ? static_cast<int32_t>(std::min<int64_t>(r[i + 1].len, 1 << 28)) : (1 << 28);
            p.push_back(q);
        } else if (i == 0) {
            Pulse q;   // the train starts with a gap
            q.t = r[0].start;
            q.on = 0;
            q.off = static_cast<int32_t>(std::min<int64_t>(r[0].len, 1 << 28));
            p.push_back(q);
        }
    }
    if (!p.empty()) p.back().off = 1 << 28;   // the end of the package
    return p;
}
}  // namespace

FrontEnd::FrontEnd() : ring_(RING), trace_(getenv("ISM_TRACE") != nullptr) {
    fin_ = fftwf_alloc_complex(NFFT);
    fout_ = fftwf_alloc_complex(NFFT);
    plan_ = fftwf_plan_dft_1d(NFFT, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
    win_pow_ = 0;
    for (int i = 0; i < NFFT; i++) {
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / NFFT));
        win_pow_ += win_[i] * win_[i];
    }
    reset();
}

FrontEnd::~FrontEnd() {
    fftwf_destroy_plan(plan_);
    fftwf_free(fin_);
    fftwf_free(fout_);
}

void FrontEnd::reset() {
    std::fill(ring_.begin(), ring_.end(), cf{});
    abs_ = 0;
    hop_fill_ = 0;
    noise_init_ = false;
    init_frames_ = 0;
    pkgs_.clear();
    pkgs_.reserve(MAX_PKGS);
    std::fill(std::begin(N_), std::end(N_), 0.0f);
    std::fill(std::begin(P1_), std::end(P1_), 0.0f);
    std::fill(std::begin(P2_), std::end(P2_), 0.0f);
}

void FrontEnd::set_threshold_db(double db) {
    thr_on_ = std::pow(10.0, db / 10);
    thr_cont_ = std::pow(10.0, (db - 2.0) / 10);
}

double FrontEnd::noise_floor_db() const {
    float t[NFFT];
    std::copy(std::begin(N_), std::end(N_), t);
    std::nth_element(t, t + NFFT / 2, t + NFFT);
    return 10 * std::log10(std::max(1e-20f, t[NFFT / 2] / win_pow_));
}

void FrontEnd::push(const cf* x, size_t n, std::vector<Burst>* out) {
    for (size_t i = 0; i < n; i++) {
        ring_[static_cast<size_t>(abs_ & (RING - 1))] = x[i];
        abs_++;
        if (++hop_fill_ == HOP) {
            hop_fill_ = 0;
            if (abs_ >= NFFT) frame(out);
        }
    }
}

void FrontEnd::frame(std::vector<Burst>* out) {
    const int64_t fs = abs_ - NFFT;
    for (int i = 0; i < NFFT; i++) {
        const cf v = at(fs + i) * win_[i];
        fin_[i][0] = v.real();
        fin_[i][1] = v.imag();
    }
    fftwf_execute(plan_);
    for (int k = 0; k < NFFT; k++) P_[k] = fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];

    if (!noise_init_) {
        // the first frames: every bin starts at the median bin (a burst
        // occupies a few bins, so the median is noise)
        float t[NFFT];
        std::copy(std::begin(P_), std::end(P_), t);
        std::nth_element(t, t + NFFT / 2, t + NFFT);
        const float mean = std::max(1e-20f, t[NFFT / 2] / 0.6931f);
        for (int k = 0; k < NFFT; k++) N_[k] = init_frames_ == 0 ? mean : 0.7f * N_[k] + 0.3f * mean;
        if (++init_frames_ >= 8) noise_init_ = true;
    }

    // the statistic per bin: 3 adjacent bins of this frame and of the one
    // before the previous (64 samples apart, not overlapping) - 6 bins' worth
    // of noise keeps the tail thin enough for a 10 dB threshold over 64 bins x
    // 7500 frames/s. rr[j]: bins in frequency order (j = signed bin + 32)
    float rr[NFFT];
    for (int j = 0; j < NFFT; j++) {
        const int k = (j - NFFT / 2) & (NFFT - 1);
        const int a = (k + NFFT - 1) & (NFFT - 1), c = (k + 1) & (NFFT - 1);
        rr[j] = (P_[a] + P_[k] + P_[c] + P2_[a] + P2_[k] + P2_[c]) / (2 * (N_[a] + N_[k] + N_[c]));
    }
    std::copy(std::begin(P1_), std::end(P1_), P2_);
    std::copy(std::begin(P_), std::end(P_), P1_);

    // the noise floor follows each bin's power while it holds no signal; a
    // bin that stays high (a carrier that never stops) is absorbed in ~10 s
    if (noise_init_)
        for (int k = 0; k < NFFT; k++) {
            if (P_[k] < 4 * N_[k]) N_[k] += 0.005f * (P_[k] - N_[k]);
            else N_[k] *= 1.0001f;
            N_[k] = std::max(N_[k], 1e-20f);
        }

    const int64_t fe = abs_;
    const int64_t gap_end = static_cast<int64_t>(GAP_END_S * FS), max_len = static_cast<int64_t>(MAX_LEN_S * FS);
    const int64_t learn = static_cast<int64_t>(LEARN_S * FS), together = static_cast<int64_t>(TOGETHER_S * FS);

    // the active parts of the band: runs of bins over the continuation
    // threshold (a one-bin gap bridged), in signed bins
    struct Seg { int lo, hi; float peak; };
    Seg segs[NFFT];
    int nseg = 0;
    for (int j = 0; j < NFFT;) {
        if (rr[j] <= thr_cont_) {
            j++;
            continue;
        }
        int e = j;
        float pk = rr[j];
        while (e + 1 < NFFT && (rr[e + 1] > thr_cont_ || (e + 2 < NFFT && rr[e + 2] > thr_cont_))) {
            e++;
            pk = std::max(pk, rr[e]);
        }
        segs[nseg++] = {j - NFFT / 2, e - NFFT / 2, pk};
        j = e + 1;
    }
    if (trace_ && nseg)
        for (int i = 0; i < nseg; i++)
            fprintf(stderr, "frame %lld seg %d..%d peak %.1f dB pkgs %zu\n", static_cast<long long>(fs), segs[i].lo,
                    segs[i].hi, 10 * std::log10(segs[i].peak), pkgs_.size());

    // Each segment belongs to the package whose frequency region (+-
    // MARGIN_BINS) it touches; a strong one elsewhere starts a package of
    // its own - another device transmitting at the same time on another
    // frequency, demodulated (and its DoA filtered) on its own. A package's
    // region grows only in its first LEARN_S: that is where the second tone
    // of a wide FSK transmitter joins it
    auto centre2 = [](int lo, int hi) { return lo + hi; };   // twice the centre bin
    auto take = [&](Pkg* p, const Seg& s) {
        p->last = fe;
        if (fe - p->first <= learn && s.peak > thr_on_) {   // noise near the edge must not widen it
            p->lo = std::min(p->lo, s.lo);
            p->hi = std::max(p->hi, s.hi);
        }
        for (int sb = std::max(-NFFT / 2, s.lo - 1); sb <= std::min(NFFT / 2 - 1, s.hi + 1); sb++) {
            const int k = sb & (NFFT - 1);
            const float d = P_[k] - N_[k];
            if (d > 0) p->acc[k] += d;
        }
    };
    // 1. segments in an open package's region
    bool taken[NFFT] = {};
    float strongest = 0;   // the strongest segment of an open package in this frame
    for (int i = 0; i < nseg; i++) {
        const Seg& s = segs[i];
        Pkg* p = nullptr;
        int best_d = 1 << 30;
        for (auto& q : pkgs_) {
            if (s.hi < q.lo - MARGIN_BINS || s.lo > q.hi + MARGIN_BINS) continue;
            const int d = std::abs(centre2(s.lo, s.hi) - centre2(q.lo, q.hi));
            if (d < best_d) {
                best_d = d;
                p = &q;
            }
        }
        if (!p) continue;
        take(p, s);
        taken[i] = true;
        strongest = std::max(strongest, s.peak);
    }
    // 2. the others: in its first LEARN_S a package takes what appears within
    // FSK_SPAN_BINS (the second tone of a wide FSK burst - also a much weaker
    // one); after that a strong segment elsewhere is another device and
    // starts a package of its own - unless it is > 20 dB under an active
    // package (the splatter of a strong OOK transmitter's keying)
    for (int i = 0; i < nseg; i++) {
        if (taken[i]) continue;
        const Seg& s = segs[i];
        Pkg* p = nullptr;
        for (auto& q : pkgs_)
            if (fe - q.first <= learn && s.peak > thr_on_ &&
                std::abs(centre2(s.lo, s.hi) - centre2(q.lo, q.hi)) <= 2 * FSK_SPAN_BINS) {
                p = &q;
                break;
            }
        if (!p && s.peak > thr_on_ && s.peak >= 0.01f * strongest && static_cast<int>(pkgs_.size()) < MAX_PKGS) {
            pkgs_.emplace_back();
            p = &pkgs_.back();
            p->a = fs;
            p->first = fe;
            p->lo = s.lo;
            p->hi = s.hi;
            std::fill(std::begin(p->acc), std::end(p->acc), 0.0f);
        }
        if (p) take(p, s);
    }

    // packages that ended; the others of the same transmitter (the tones of
    // a wide FSK burst that started apart) end with them and are merged
    for (size_t i = 0; i < pkgs_.size();) {
        if (!(fe - pkgs_[i].last > gap_end || pkgs_[i].last - pkgs_[i].a > max_len)) {
            i++;
            continue;
        }
        Pkg p = pkgs_[i];
        pkgs_.erase(pkgs_.begin() + static_cast<long>(i));
        for (size_t j = 0; j < pkgs_.size();) {
            const Pkg& r = pkgs_[j];
            if (std::llabs(r.last - p.last) <= together && r.a < p.last && p.a < r.last &&
                std::abs(centre2(r.lo, r.hi) - centre2(p.lo, p.hi)) <= 2 * FSK_SPAN_BINS) {
                p.a = std::min(p.a, r.a);
                p.last = std::max(p.last, r.last);
                p.lo = std::min(p.lo, r.lo);
                p.hi = std::max(p.hi, r.hi);
                for (int k = 0; k < NFFT; k++) p.acc[k] += r.acc[k];
                pkgs_.erase(pkgs_.begin() + static_cast<long>(j));
            } else {
                j++;
            }
        }
        finish(p, out);
    }
}

void FrontEnd::finish(const Pkg& p, std::vector<Burst>* out) {
    if (p.last - p.a < static_cast<int64_t>(0.0008 * FS)) return;   // a click
    // the signal's bins: >= 8 % of the package's peak within its region; the
    // burst is centred on the middle of them (between the two FSK tones)
    const int r0 = std::max(-NFFT / 2, p.lo - MARGIN_BINS), r1 = std::min(NFFT / 2 - 1, p.hi + MARGIN_BINS);
    float peak = 0;
    for (int sb = r0; sb <= r1; sb++) peak = std::max(peak, p.acc[sb & (NFFT - 1)]);
    if (!(peak > 0)) return;
    int lo = r1, hi = r0;
    for (int sb = r0; sb <= r1; sb++) {
        if (p.acc[sb & (NFFT - 1)] < 0.08f * peak) continue;
        lo = std::min(lo, sb);
        hi = std::max(hi, sb);
    }
    const double fc = (lo + hi) / 2.0 * BIN_HZ;
    const double bw = (hi - lo + 1) * BIN_HZ;
    const int64_t m = static_cast<int64_t>(MARGIN_S * FS);
    int64_t a = p.a - m, b = p.last + m;
    a = std::max<int64_t>({a, 0, abs_ - RING + 1});
    b = std::min(b, abs_);
    if (b <= a) return;
    Burst bu;
    demod(a, b, fc, bw, &bu);
    if (bu.ook.empty() && bu.fsk.empty()) return;
    out->push_back(std::move(bu));
}

// the samples a..b mixed down by f0 and low-pass filtered (Hamming windowed
// sinc, cut-off fcut, taps ~ k fs / fcut); y[i] = input sample a + i
void FrontEnd::filter(int64_t a, int64_t b, double f0, double fcut, double k, std::vector<cf>* y, double* h2) const {
    int L = static_cast<int>(std::lround(k * FS / fcut)) | 1;
    L = std::clamp(L, 15, 161);
    std::vector<float> h(static_cast<size_t>(L));
    double sum = 0;
    for (int i = 0; i < L; i++) {
        const double x = i - (L - 1) / 2.0, fn = fcut / FS;
        const double s = x == 0 ? 2 * fn : std::sin(2 * M_PI * fn * x) / (M_PI * x);
        h[static_cast<size_t>(i)] = static_cast<float>(s * (0.54 - 0.46 * std::cos(2 * M_PI * i / (L - 1))));
        sum += h[static_cast<size_t>(i)];
    }
    *h2 = 0;
    for (auto& v : h) {
        v = static_cast<float>(v / sum);
        *h2 += v * v;
    }
    const int half = L / 2;
    const size_t n = static_cast<size_t>(b - a);
    std::vector<cf> xm(n + 2 * static_cast<size_t>(half));
    const double w0 = -2 * M_PI * f0 / FS;
    const std::complex<double> rot(std::cos(w0), std::sin(w0));
    const double ph0 = std::remainder(w0 * static_cast<double>(a - half), 2 * M_PI);
    std::complex<double> ph(std::cos(ph0), std::sin(ph0));
    for (size_t j = 0; j < xm.size(); j++) {
        const int64_t s = a - half + static_cast<int64_t>(j);
        const cf v = s >= 0 && s > abs_ - RING && s < abs_ ? at(s) : cf{};   // outside the ring: silence
        xm[j] = v * cf(static_cast<float>(ph.real()), static_cast<float>(ph.imag()));
        ph *= rot;
        if ((j & 1023) == 1023) ph /= std::abs(ph);
    }
    y->resize(n);
    for (size_t i = 0; i < n; i++) {
        float re = 0, im = 0;
        const cf* q = xm.data() + i;
        for (int t = 0; t < L; t++) {
            re += q[t].real() * h[static_cast<size_t>(t)];
            im += q[t].imag() * h[static_cast<size_t>(t)];
        }
        (*y)[i] = cf(re, im);
    }
}

namespace {
// the signal level: the median of what is above half the (robust) peak
float level_hi(const std::vector<float>& env) {
    std::vector<float> sub;
    sub.reserve(env.size() / 3 + 1);
    for (size_t i = 0; i < env.size(); i += 3) sub.push_back(env[i]);
    if (sub.empty()) return 0;
    std::sort(sub.begin(), sub.end());
    const float peak = sub[std::min(sub.size() - 1, static_cast<size_t>(0.995 * sub.size()))];
    const auto it = std::upper_bound(sub.begin(), sub.end(), 0.5f * peak);
    return it == sub.end() ? peak : sub[static_cast<size_t>((it - sub.begin()) + (sub.end() - it) / 2)];
}
// instantaneous frequency (Hz), 3-sample mean
std::vector<float> discriminator(const std::vector<cf>& y) {
    const size_t n = y.size();
    std::vector<float> d(n, 0.0f), dm(n, 0.0f);
    for (size_t i = 1; i < n; i++) d[i] = std::arg(y[i] * std::conj(y[i - 1])) * static_cast<float>(FS / (2 * M_PI));
    for (size_t i = 1; i + 1 < n; i++) dm[i] = (d[i - 1] + d[i] + d[i + 1]) / 3;
    return dm;
}
}  // namespace

double FrontEnd::sigma(double h2) const {
    float t[NFFT];
    std::copy(std::begin(N_), std::end(N_), t);
    std::nth_element(t, t + NFFT / 2, t + NFFT);
    return std::sqrt(std::max(1e-30, static_cast<double>(t[NFFT / 2]) / win_pow_ * h2));
}

// Two passes: a wide one finds the carrier / the two FSK tones; the second
// is centred on them (exactly between the tones) with a filter just wide
// enough - centred on the detector's power-weighted estimate, a tone near
// the filter's edge was attenuated and the envelope dipped on every bit
void FrontEnd::demod(int64_t a, int64_t b, double fc, double bw, Burst* out) const {
    const size_t n = static_cast<size_t>(b - a);
    std::vector<cf> y;
    double h2;
    filter(a, b, fc, std::clamp(bw / 2 + 25e3, 35e3, 115e3), 6, &y, &h2);
    std::vector<float> env(n);
    for (size_t i = 0; i < n; i++) env[i] = std::abs(y[i]);
    double hi = level_hi(env), sig = sigma(h2);
    std::vector<float> dm = discriminator(y);
    const float mthr1 = static_cast<float>(std::max(0.35 * hi, 3 * sig));
    std::vector<float> fv;
    fv.reserve(n / 2);
    for (size_t i = 2; i + 2 < n; i += 2)
        if (env[i] > mthr1 && env[i - 2] > mthr1 && env[i + 2] > mthr1) fv.push_back(dm[i]);
    if (fv.size() < 20) return;
    // two tones? (2-means)
    std::vector<float> srt = fv;
    std::sort(srt.begin(), srt.end());
    double c1 = srt[static_cast<size_t>(0.8 * srt.size())], c2 = srt[static_cast<size_t>(0.2 * srt.size())];
    size_t n1 = 0, n2 = 0;
    for (int it = 0; it < 8; it++) {
        const double mid = (c1 + c2) / 2;
        double s1 = 0, s2 = 0;
        n1 = n2 = 0;
        for (float v : fv) {
            if (v > mid) { s1 += v; n1++; }
            else { s2 += v; n2++; }
        }
        if (!n1 || !n2) break;
        c1 = s1 / n1;
        c2 = s2 / n2;
    }
    const double sep = c1 - c2;
    // two tones whose middle lies in the occupied band (a noisy
    // discriminator near the band edge can suggest one far outside)
    const bool fsk = n1 && n2 && sep > 4000 && std::min(n1, n2) > 0.08 * fv.size() &&
                     std::fabs((c1 + c2) / 2) <= bw / 2 + 10e3 && std::fabs(fc + (c1 + c2) / 2) < FS / 2 - 5e3;
    // pass 2: centred between the tones, or on the carrier (the median
    // frequency while on - within the occupied band)
    const double med = srt[srt.size() / 2];
    const double f0 = fsk ? fc + (c1 + c2) / 2 : std::fabs(med) <= std::max(bw / 2, 10e3) ? fc + med : fc;
    const double fcut = fsk ? std::clamp(sep / 2 + 25e3, 30e3, 115e3) : std::clamp(bw / 2 + 10e3, 18e3, 70e3);
    filter(a, b, f0, fcut, 7, &y, &h2);
    for (size_t i = 0; i < n; i++) env[i] = std::abs(y[i]);
    hi = level_hi(env);
    sig = sigma(h2);
    out->a = a;
    out->b = b;
    out->fc = f0;
    out->bw = fsk ? std::max(bw, sep + 20e3) : bw;
    out->fsk_sep = fsk ? sep : 0;
    out->snr_db = 20 * std::log10(std::max(1e-12, hi / sig));
    const int64_t glitch = std::max(2, us2s(GLITCH_US));
    const int hold = std::max(2, us2s(DIP_HOLD_US));   // FSK: a carrier dip this short keeps the tone

    // OOK: the envelope against half the signal level
    const float thr = static_cast<float>(std::max(0.5 * hi, 3 * sig)), thr_off = 0.75f * thr;
    std::vector<Run> runs;
    {
        bool on = false;
        int64_t st = 0;
        for (size_t i = 0; i < n; i++) {
            const bool nv = on ? env[i] >= thr_off : env[i] > thr;
            if (nv != on || i == 0) {
                if (i > 0) runs.push_back({on, a + st, static_cast<int64_t>(i) - st});
                on = nv;
                st = static_cast<int64_t>(i);
            }
        }
        runs.push_back({on, a + st, static_cast<int64_t>(n) - st});
        if (!runs.empty() && !runs.front().on) runs.erase(runs.begin());
        merge_glitches(runs, glitch);
        merge_dropouts(runs);
        if (!runs.empty() && !runs.back().on) runs.pop_back();
        out->ook = to_pulses(runs);
    }
    if (!fsk) return;

    if (sep >= WIDE_FSK_HZ) {
        // Wide FSK (HopeRF-style +-60 kHz): each tone through its own filter,
        // each normalised by its own level, the stronger one wins. A tone near
        // the band edge is attenuated by the VFO's filter - the discriminator
        // then read noise there, a level comparison doesn't care
        const double tcut = std::clamp(0.35 * sep, 15e3, 45e3);
        std::vector<cf> yh, yl;
        double h2h, h2l;
        filter(a, b, fc + c1, tcut, 7, &yh, &h2h);
        filter(a, b, fc + c2, tcut, 7, &yl, &h2l);
        std::vector<float> eh(n), el(n);
        for (size_t i = 0; i < n; i++) {
            eh[i] = std::abs(yh[i]);
            el[i] = std::abs(yl[i]);
        }
        const double ah = std::max(1e-12, static_cast<double>(level_hi(eh))),
                     al = std::max(1e-12, static_cast<double>(level_hi(el)));
        const double nh = 3 * sigma(h2h) / ah, nl = 3 * sigma(h2l) / al;
        runs.clear();
        int state = -1;   // -1 carrier off, 0 low, 1 high
        int dip = 0;      // carrier-off samples in a row
        int64_t st = 0;
        for (size_t i = 0; i < n; i++) {
            const double rh = eh[i] / ah, rl = el[i] / al;
            int ns;
            if (rh + rl < 0.4 || (rh < nh && rl < nl)) {
                ns = state >= 0 && ++dip <= hold ? state : -1;   // a short dip keeps the tone
            } else {
                dip = 0;
                if (state == 1) ns = rl > 1.15 * rh ? 0 : 1;
                else if (state == 0) ns = rh > 1.15 * rl ? 1 : 0;
                else ns = rh > rl ? 1 : 0;
            }
            if (ns != state || i == 0) {
                if (i > 0) runs.push_back({state == 1, a + st, static_cast<int64_t>(i) - st});
                state = ns;
                st = static_cast<int64_t>(i);
            }
        }
        runs.push_back({state == 1, a + st, static_cast<int64_t>(n) - st});
        while (!runs.empty() && !runs.front().on) {
            const size_t k = static_cast<size_t>(runs.front().start - a);
            if (eh[k] / ah + el[k] / al >= 0.4) break;
            runs.erase(runs.begin());
        }
        merge_glitches(runs, glitch);
        while (!runs.empty() && !runs.back().on) runs.pop_back();
        out->fsk = to_pulses(runs);
        return;
    }

    // FSK: the discriminator inside the envelope; the tones are now at
    // about +-sep / 2, the slicer's two levels track drift
    dm = discriminator(y);
    const float mthr = static_cast<float>(std::max(0.35 * hi, 3 * sig));
    runs.clear();
    double e1 = sep / 2, e2 = -sep / 2;
    int state = -1;   // -1 carrier off, 0 low, 1 high
    int dip = 0;      // carrier-off samples in a row
    int64_t st = 0;
    for (size_t i = 0; i < n; i++) {
        int ns;
        if (env[i] <= mthr) {
            ns = state >= 0 && ++dip <= hold ? state : -1;   // a short dip keeps the tone
        } else {
            dip = 0;
            const double mid = (e1 + e2) / 2, hy = 0.1 * (e1 - e2);
            if (state == 1) ns = dm[i] < mid - hy ? 0 : 1;
            else if (state == 0) ns = dm[i] > mid + hy ? 1 : 0;
            else ns = dm[i] > mid ? 1 : 0;
            if (ns == 1) e1 += 0.01 * (dm[i] - e1);
            else e2 += 0.01 * (dm[i] - e2);
            if (e1 - e2 < 0.3 * sep) {   // lost: back to the start values
                e1 = sep / 2;
                e2 = -sep / 2;
            }
        }
        if (ns != state || i == 0) {
            if (i > 0) runs.push_back({state == 1, a + st, static_cast<int64_t>(i) - st});
            state = ns;
            st = static_cast<int64_t>(i);
        }
    }
    runs.push_back({state == 1, a + st, static_cast<int64_t>(n) - st});
    // the train is the transmission: the carrier-off ends trimmed
    while (!runs.empty() && !runs.front().on && env[static_cast<size_t>(runs.front().start - a)] <= mthr)
        runs.erase(runs.begin());
    merge_glitches(runs, glitch);
    while (!runs.empty() && !runs.back().on) runs.pop_back();
    out->fsk = to_pulses(runs);
}

}  // namespace ism
