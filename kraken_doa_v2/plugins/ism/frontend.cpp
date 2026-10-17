#include "frontend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ism {

namespace {
constexpr double BIN_HZ = FS / 64;   // 3.75 kHz at every input rate

// the input ring: a power of 2 >= 1.6 s (a package of MAX_LEN + margins)
int64_t ring_size(double fs) {
    int64_t n = 1;
    while (static_cast<double>(n) < 1.6 * fs) n <<= 1;
    return n;
}

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

FrontEnd::FrontEnd(int decim)
    : D_(std::max(1, decim)), fs_in_(FS * D_), nfft_(64 * D_), hop_(32 * D_), max_pkgs_(D_ == 1 ? 4 : 8),
      ring_n_(ring_size(fs_in_)), ring_(static_cast<size_t>(ring_n_)), trace_(getenv("ISM_TRACE") != nullptr) {
    fin_ = kfft_alloc_complex(static_cast<size_t>(nfft_));
    fout_ = kfft_alloc_complex(static_cast<size_t>(nfft_));
    plan_ = kfft_plan_dft_1d(nfft_, fin_, fout_, KFFT_FORWARD);
    win_.resize(static_cast<size_t>(nfft_));
    P_.resize(win_.size());
    P1_.resize(win_.size());
    P2_.resize(win_.size());
    N_.resize(win_.size());
    rr_.resize(win_.size());
    tmp_.resize(win_.size());
    segs_.resize(win_.size());
    taken_.resize(win_.size());
    win_pow_ = 0;
    for (int i = 0; i < nfft_; i++) {
        win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / nfft_));
        win_pow_ += win_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
    }
    if (D_ > 1) {
        // Blackman windowed sinc, unity gain at 0 Hz
        dec_h_.resize(DEC_TAPS);
        double sum = 0;
        for (int i = 0; i < DEC_TAPS; i++) {
            const double x = i - (DEC_TAPS - 1) / 2.0, fn = DEC_CUT_HZ / fs_in_;
            const double s = x == 0 ? 2 * fn : std::sin(2 * M_PI * fn * x) / (M_PI * x);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (DEC_TAPS - 1)) + 0.08 * std::cos(4 * M_PI * i / (DEC_TAPS - 1));
            dec_h_[static_cast<size_t>(i)] = static_cast<float>(s * w);
            sum += s * w;
        }
        dec_h2_ = 0;
        for (auto& v : dec_h_) {
            v = static_cast<float>(v / sum);
            dec_h2_ += static_cast<double>(v) * v;
        }
    }
    reset();
}

FrontEnd::~FrontEnd() {
    kfft_destroy_plan(plan_);
    kfft_free(fin_);
    kfft_free(fout_);
}

void FrontEnd::reset() {
    std::fill(ring_.begin(), ring_.end(), cf{});
    for (auto& r : xring_) std::fill(r.begin(), r.end(), std::complex<int16_t>{});
    w_.clear();
    std::fill(xn_.begin(), xn_.end(), 0.0);
    std::fill(xg_.begin(), xg_.end(), 1.0);
    xn_tick_ = 0;
    broadband_ = 0;
    broadband_last_ = -1;
    born_long_.clear();
    abs_ = 0;
    hop_fill_ = 0;
    noise_init_ = false;
    init_frames_ = 0;
    pkgs_.clear();
    pkgs_.reserve(static_cast<size_t>(max_pkgs_));
    std::fill(N_.begin(), N_.end(), 0.0f);
    std::fill(P1_.begin(), P1_.end(), 0.0f);
    std::fill(P2_.begin(), P2_.end(), 0.0f);
    std::vector<cf>().swap(seg_);
}

void FrontEnd::set_threshold_db(double db) {
    thr_on_ = std::pow(10.0, db / 10);
    // a package continues on what holds 2 dB less - but never below 7.5 dB:
    // noise crosses the statistic at 6 dB once per ~3000 bin-frames, so a
    // package's region (~40 bins) saw a crossing in 93 % of the 30 ms it
    // needs to end - at the 8 dB setting packages ran to MAX_LEN_S, merged
    // transmissions and were reported > 1 s late (7.5 dB: 5 %; 8 dB: 1 %)
    thr_cont_ = std::pow(10.0, std::max(db - 2.0, 7.5) / 10);
    thr_cont_f_ = static_cast<float>(thr_cont_);
    if (static_cast<double>(thr_cont_f_) > thr_cont_) thr_cont_f_ = std::nextafter(thr_cont_f_, 0.0f);
}

// the median noise floor of `count` bins from signed bin lo
double FrontEnd::median_floor(int lo, int count) const {
    std::vector<float> t;
    t.reserve(static_cast<size_t>(count));
    for (int sb = lo; sb < lo + count; sb++) t.push_back(N_[ix(sb)]);
    std::nth_element(t.begin(), t.begin() + count / 2, t.end());
    return t[static_cast<size_t>(count / 2)];
}

double FrontEnd::noise_floor_db() const {
    const float m = static_cast<float>(median_floor(-nfft_ / 2, nfft_));
    return 10 * std::log10(std::max(1e-20f, m / win_pow_));
}

void FrontEnd::set_antennas(int m) {
    M_ = std::max(1, m);
    if (M_ == 1) {
        std::vector<std::vector<std::complex<int16_t>>>().swap(xring_);
        std::vector<std::vector<cf>>().swap(X_);
    } else {
        xring_.assign(static_cast<size_t>(M_ - 1), std::vector<std::complex<int16_t>>(static_cast<size_t>(ring_n_)));
        X_.assign(static_cast<size_t>(M_), std::vector<cf>(static_cast<size_t>(nfft_)));
    }
    xn_.assign(static_cast<size_t>(M_), 0.0);
    xg_.assign(static_cast<size_t>(M_), 1.0);
    xtmp_.resize(static_cast<size_t>(nfft_));
    reset();   // (the packages' covariances, the rings)
}

void FrontEnd::push_multi(const cf* const* x, int m, size_t n, std::vector<Burst>* out) {
    if (m < 2 || D_ == 1) {
        push(x[0], n, out);
        return;
    }
    if (m != M_) set_antennas(m);
    auto s16 = [](float v) { return static_cast<int16_t>(std::clamp(v * XSCALE, -32767.0f, 32767.0f)); };
    size_t i = 0;
    while (i < n) {
        const size_t at0 = static_cast<size_t>(abs_ & (ring_n_ - 1));
        const size_t k = std::min({n - i, static_cast<size_t>(hop_ - hop_fill_), static_cast<size_t>(ring_n_) - at0});
        std::copy(x[0] + i, x[0] + i + k, ring_.begin() + static_cast<long>(at0));
        for (int c = 1; c < M_; c++) {
            std::complex<int16_t>* d = xring_[static_cast<size_t>(c - 1)].data() + at0;
            const cf* q = x[c] + i;
            for (size_t j = 0; j < k; j++) d[j] = {s16(q[j].real()), s16(q[j].imag())};
        }
        i += k;
        abs_ += static_cast<int64_t>(k);
        hop_fill_ += static_cast<int>(k);
        if (hop_fill_ == hop_) {
            hop_fill_ = 0;
            if (abs_ >= nfft_) frame(out);
        }
    }
}

void FrontEnd::push(const cf* x, size_t n, std::vector<Burst>* out) {
    if (M_ != 1) set_antennas(1);
    size_t i = 0;
    while (i < n) {
        // up to the next hop or the ring's end, in one copy
        const size_t at0 = static_cast<size_t>(abs_ & (ring_n_ - 1));
        const size_t k = std::min({n - i, static_cast<size_t>(hop_ - hop_fill_), static_cast<size_t>(ring_n_) - at0});
        std::copy(x + i, x + i + k, ring_.begin() + static_cast<long>(at0));
        i += k;
        abs_ += static_cast<int64_t>(k);
        hop_fill_ += static_cast<int>(k);
        if (hop_fill_ == hop_) {
            hop_fill_ = 0;
            if (abs_ >= nfft_) frame(out);
        }
    }
}

void FrontEnd::frame(std::vector<Burst>* out) {
    const int n = nfft_;
    const int64_t fs = abs_ - n;
    const size_t r0 = static_cast<size_t>(fs & (ring_n_ - 1));
    // The per-bin loops below are plain loops over __restrict pointers (no
    // aliasing check needed): the vectoriser takes them at -O3 - this
    // plugin's CXXFLAGS (2.4 MHz: 6.8 -> ~4 % of a core); the results are
    // the same as -O2's (element-wise, no reordering)
    if (M_ > 1) {
        // every antenna, each brought to the antennas' mean noise (xg_): P =
        // their mean power per bin, X_ = each one's spectrum (normalised -
        // the packages' covariance). Every 16th frame each antenna's noise
        // (its median bin power) is followed (~1 s)
        float* __restrict P = P_.data();
        std::fill(P, P + n, 0.0f);
        const int h = n / 2;
        const bool tick = (xn_tick_++ & 15) == 0;
        const bool contig = r0 + static_cast<size_t>(n) <= ring_.size();
        for (int c = 0; c < M_; c++) {
            // windowed into the FFT input (plain loops over __restrict
            // pointers - vectorised -, a wrap around the ring's end aside)
            const float* __restrict w = win_.data();
            float* __restrict fi = &fin_[0][0];
            if (contig && c == 0) {
                const float* __restrict q = reinterpret_cast<const float*>(ring_.data() + r0);
                for (int i = 0; i < n; i++) {
                    fi[2 * i] = q[2 * i] * w[i];
                    fi[2 * i + 1] = q[2 * i + 1] * w[i];
                }
            } else if (contig) {
                const int16_t* __restrict q = reinterpret_cast<const int16_t*>(xring_[static_cast<size_t>(c - 1)].data() + r0);
                const float sc = 1.0f / XSCALE;
                for (int i = 0; i < n; i++) {
                    fi[2 * i] = static_cast<float>(q[2 * i]) * sc * w[i];
                    fi[2 * i + 1] = static_cast<float>(q[2 * i + 1]) * sc * w[i];
                }
            } else {
                for (int i = 0; i < n; i++) {
                    const cf v = (c == 0 ? at(fs + i) : xat(c, fs + i)) * win_[static_cast<size_t>(i)];
                    fin_[i][0] = v.real();
                    fin_[i][1] = v.imag();
                }
            }
            kfft_execute(plan_);
            const float* __restrict fo = &fout_[0][0];
            cf* __restrict X = X_[static_cast<size_t>(c)].data();
            float* __restrict tp = xtmp_.data();
            const float g = static_cast<float>(xg_[static_cast<size_t>(c)]), sg = std::sqrt(g);
            for (int j = 0; j < n; j++) {
                const int k = j < h ? j + h : j - h;   // frequency order
                const float pw = fo[2 * k] * fo[2 * k] + fo[2 * k + 1] * fo[2 * k + 1];
                tp[j] = pw;
                P[j] += g * pw;
                X[j] = cf(fo[2 * k] * sg, fo[2 * k + 1] * sg);
            }
            if (tick) {
                std::nth_element(xtmp_.begin(), xtmp_.begin() + n / 2, xtmp_.end());
                const double med = std::max(1e-20, static_cast<double>(xtmp_[static_cast<size_t>(n / 2)]));
                double& xn = xn_[static_cast<size_t>(c)];
                xn = xn > 0 ? xn + 0.02 * (med - xn) : med;
            }
        }
        const float inv = 1.0f / static_cast<float>(M_);
        for (int j = 0; j < n; j++) P[j] *= inv;
        if (tick) {
            double mean = 0;
            for (double v : xn_) mean += v;
            mean /= M_;
            for (int c = 0; c < M_; c++) xg_[static_cast<size_t>(c)] = mean / xn_[static_cast<size_t>(c)];
        }
    } else if (r0 + static_cast<size_t>(n) <= ring_.size()) {   // contiguous (nearly always)
        const float* __restrict w = win_.data();
        float* __restrict fi = &fin_[0][0];
        const float* __restrict q = reinterpret_cast<const float*>(ring_.data() + r0);
        for (int i = 0; i < n; i++) {
            fi[2 * i] = q[2 * i] * w[i];
            fi[2 * i + 1] = q[2 * i + 1] * w[i];
        }
    } else {
        for (int i = 0; i < n; i++) {
            const cf v = at(fs + i) * win_[static_cast<size_t>(i)];
            fin_[i][0] = v.real();
            fin_[i][1] = v.imag();
        }
    }
    if (M_ == 1) kfft_execute(plan_);
    const float* rr = rr_.data();
    {   // (the buffers are only accessed through these pointers in here)
        float* __restrict P = P_.data();
        const float* __restrict P2 = P2_.data();
        float* __restrict N = N_.data();
        float* __restrict r = rr_.data();
        const float* __restrict fo = &fout_[0][0];
        // power in frequency order (FFT bin k = signed bin k, or k - n above n / 2)
        const int h = n / 2;
        if (M_ == 1) {
            for (int j = 0; j < h; j++) {
                const int k = j + h;
                P[j] = fo[2 * k] * fo[2 * k] + fo[2 * k + 1] * fo[2 * k + 1];
            }
            for (int k = 0; k < h; k++) P[k + h] = fo[2 * k] * fo[2 * k] + fo[2 * k + 1] * fo[2 * k + 1];
        }

        if (!noise_init_) {
            // the first frames: every bin starts at the median bin (a burst
            // occupies a few bins, so the median is noise)
            std::copy(P, P + n, tmp_.begin());
            std::nth_element(tmp_.begin(), tmp_.begin() + n / 2, tmp_.end());
            const float mean = std::max(1e-20f, tmp_[static_cast<size_t>(n / 2)] / 0.6931f);
            for (int k = 0; k < n; k++) N[k] = init_frames_ == 0 ? mean : 0.7f * N[k] + 0.3f * mean;
            if (++init_frames_ >= 8) noise_init_ = true;
        }

        // the statistic per bin: 3 adjacent bins of this frame and of the one
        // before the previous (64 D samples apart, not overlapping) - 6 bins'
        // worth of noise keeps the tail thin enough for a 10 dB threshold over
        // 64 bins x 7500 frames/s. Neighbours wrap around the band's edges
        auto stat = [&](int a, int k, int c) {
            return (P[a] + P[k] + P[c] + P2[a] + P2[k] + P2[c]) / (2 * (N[a] + N[k] + N[c]));
        };
        r[0] = stat(n - 1, 0, 1);
        for (int k = 1; k < n - 1; k++)
            r[k] = (P[k - 1] + P[k] + P[k + 1] + P2[k - 1] + P2[k] + P2[k + 1]) / (2 * (N[k - 1] + N[k] + N[k + 1]));
        r[n - 1] = stat(n - 2, n - 1, 0);
    }
    P2_.swap(P1_);   // P2 = the frame before
    std::copy(P_.begin(), P_.end(), P1_.begin());

    // the noise floor follows each bin's power while it holds no signal; a
    // bin that stays high (a carrier that never stops) is absorbed in ~10 s
    if (noise_init_) {
        const float* __restrict P = P_.data();
        float* __restrict N = N_.data();
        for (int k = 0; k < n; k++) {   // (branch-free)
            const float up = N[k] + 0.005f * (P[k] - N[k]), dn = N[k] * 1.0001f;
            N[k] = std::max(P[k] < 4 * N[k] ? up : dn, 1e-20f);
        }
    }
    float* P = P_.data();
    float* N = N_.data();

    const int64_t fe = abs_;
    const int64_t gap_end = static_cast<int64_t>(GAP_END_S * fs_in_), max_len = static_cast<int64_t>(MAX_LEN_S * fs_in_);
    const int64_t learn = static_cast<int64_t>(LEARN_S * fs_in_), together = static_cast<int64_t>(TOGETHER_S * fs_in_);

    // the active parts of the band: runs of bins over the continuation
    // threshold (a one-bin gap bridged), in signed bins
    Seg* segs = segs_.data();
    int nseg = 0;
    // most frames have nothing over the threshold: no scan (thr_cont_f_ = the
    // largest float <= thr_cont_, the same test for a float)
    int above = 0;
    for (int k = 0; k < n; k++) above |= rr[k] > thr_cont_f_;
    for (int j = 0; j < n && above;) {
        if (rr[j] <= thr_cont_) {
            j++;
            continue;
        }
        int e = j;
        float pk = rr[j];
        while (e + 1 < n && (rr[e + 1] > thr_cont_ || (e + 2 < n && rr[e + 2] > thr_cont_))) {
            e++;
            pk = std::max(pk, rr[e]);
        }
        segs[nseg++] = {j - n / 2, e - n / 2, pk};
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
        for (int sb = std::max(-n / 2, s.lo - 1); sb <= std::min(n / 2 - 1, s.hi + 1); sb++) {
            const size_t k = ix(sb);
            const float d = P[k] - N[k];
            if (d <= 0) continue;
            p->acc[k] += d;
            if (M_ > 1) {   // the spatial covariance of the bin's spectra
                std::complex<double>* R = p->R.data();
                for (int a = 0; a < M_; a++) {
                    const std::complex<double> xa = X_[static_cast<size_t>(a)][k];
                    for (int c = a; c < M_; c++) R[a * M_ + c] += xa * std::conj(std::complex<double>(X_[static_cast<size_t>(c)][k]));
                }
            }
        }
    };
    // 1. segments in an open package's region
    char* taken = taken_.data();
    std::fill(taken_.begin(), taken_.end(), 0);
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
    born_now_.clear();
    // (the splatter test below also counts what starts in this frame: a
    // strong OOK transmitter's first key-down splatters over the band, and
    // with only the open packages to compare with, its splatter started
    // packages of its own - 3 or more at once, then all taken for broadband
    // interference, the device's too)
    for (int i = 0; i < nseg; i++)
        if (!taken[i] && segs[i].peak > thr_on_) strongest = std::max(strongest, segs[i].peak);
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
        if (!p && s.peak > thr_on_ && s.peak >= 0.01f * strongest && static_cast<int>(pkgs_.size()) < max_pkgs_) {
            pkgs_.emplace_back();
            born_now_.push_back(pkgs_.size() - 1);
            p = &pkgs_.back();
            p->a = fs;
            p->first = fe;
            p->lo = s.lo;
            p->hi = s.hi;
            p->acc.assign(static_cast<size_t>(n), 0.0f);
            if (M_ > 1) p->R.assign(static_cast<size_t>(M_ * M_), {});
        }
        if (p) take(p, s);
    }

    if (born_now_.size() >= 3)
        for (size_t i : born_now_) pkgs_[i].born = static_cast<int>(born_now_.size());
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
                for (size_t k = 0; k < p.acc.size(); k++) p.acc[k] += r.acc[k];
                for (size_t k = 0; k < p.R.size() && k < r.R.size(); k++) p.R[k] += r.R[k];
                p.born = std::max(p.born, r.born);
                pkgs_.erase(pkgs_.begin() + static_cast<long>(j));
            } else {
                j++;
            }
        }
        // one of >= 3 born together, of which >= 3 lasted 5 ms or more (those
        // ended and those still open): broadband, not a device
        int long_ones = 0;
        if (p.born >= 3) {
            const int64_t five = static_cast<int64_t>(0.005 * fs_in_);
            int* done_long = nullptr;
            for (auto& bl : born_long_)
                if (bl.first == p.a) done_long = &bl.second;
            if (!done_long) {
                born_long_.emplace_back(p.a, 0);
                if (born_long_.size() > 64) born_long_.erase(born_long_.begin());
                done_long = &born_long_.back().second;
            }
            if (p.last - p.a >= five) ++*done_long;
            long_ones = *done_long;
            for (const auto& q : pkgs_)
                if (q.a == p.a && q.born >= 3 && q.last - q.a >= five) long_ones++;
        }
        if (long_ones >= 3) {
            if (trace_)
                fprintf(stderr, "broadband: package %.4f .. %.4f s, bins %d..%d, %d born together\n",
                        static_cast<double>(p.a) / fs_in_, static_cast<double>(p.last) / fs_in_, p.lo, p.hi, p.born);
            if (p.a != broadband_last_) {
                broadband_++;
                broadband_last_ = p.a;
            }
            continue;
        }
        finish(p, out);
    }
}

void FrontEnd::finish(const Pkg& p, std::vector<Burst>* out) {
    if (p.last - p.a < static_cast<int64_t>(0.0008 * fs_in_)) return;   // a click
    // the signal's bins: >= 8 % of the package's peak within its region; the
    // burst is centred on the middle of them (between the two FSK tones)
    const int r0 = std::max(-nfft_ / 2, p.lo - MARGIN_BINS), r1 = std::min(nfft_ / 2 - 1, p.hi + MARGIN_BINS);
    float peak = 0;
    for (int sb = r0; sb <= r1; sb++) peak = std::max(peak, p.acc[ix(sb)]);
    if (!(peak > 0)) return;
    int lo = r1, hi = r0;
    for (int sb = r0; sb <= r1; sb++) {
        if (p.acc[ix(sb)] < 0.08f * peak) continue;
        lo = std::min(lo, sb);
        hi = std::max(hi, sb);
    }
    const double fc = (lo + hi) / 2.0 * BIN_HZ;
    const double bw = (hi - lo + 1) * BIN_HZ;
    // every antenna: the main eigenvector of the package's covariance (power
    // iteration; the upper triangle was summed) = the device's array response
    // -> y = w^H x, |w| = 1 (the noise stays one antenna's), antenna 0's
    // weight real (the phase follows antenna 0's)
    w_.clear();
    double comb_db = 0;
    if (M_ > 1 && p.R.size() == static_cast<size_t>(M_ * M_)) {
        std::vector<std::complex<double>> R(p.R);
        for (int a = 0; a < M_; a++)
            for (int c = 0; c < a; c++) R[a * M_ + c] = std::conj(R[c * M_ + a]);
        double tr = 0;
        for (int a = 0; a < M_; a++) tr += R[a * M_ + a].real();
        if (tr > 0) {
            std::vector<std::complex<double>> v(static_cast<size_t>(M_), 1.0 / std::sqrt(static_cast<double>(M_))), t(v.size());
            double lam = 0;
            for (int it = 0; it < 40; it++) {
                double nn = 0;
                for (int a = 0; a < M_; a++) {
                    std::complex<double> sum = 0;
                    for (int c = 0; c < M_; c++) sum += R[a * M_ + c] * v[static_cast<size_t>(c)];
                    t[static_cast<size_t>(a)] = sum;
                    nn += std::norm(sum);
                }
                nn = std::sqrt(nn);
                if (!(nn > 0)) break;
                lam = nn;
                for (int a = 0; a < M_; a++) v[static_cast<size_t>(a)] = t[static_cast<size_t>(a)] / nn;
            }
            const double a0 = std::abs(v[0]);
            if (lam > 0 && a0 > 1e-9) {
                // the covariance was of the normalised antennas: their weights
                // v, applied to the antennas as they are: v sqrt(g) - the
                // combination's noise is then the antennas' mean (the floor's)
                const std::complex<double> ph = std::conj(v[0]) / a0;
                w_.resize(static_cast<size_t>(M_));
                for (int a = 0; a < M_; a++)
                    w_[static_cast<size_t>(a)] = cf(v[static_cast<size_t>(a)] * ph * std::sqrt(xg_[static_cast<size_t>(a)]));
                comb_db = 10 * std::log10(lam / std::max(1e-30, R[0].real()));
            }
        }
    }
    const int64_t m = static_cast<int64_t>(MARGIN_S * fs_in_);
    int64_t a = p.a - m, b = p.last + m;
    a = std::max<int64_t>({a, 0, abs_ - ring_n_ + 1});
    b = std::min(b, abs_);
    if (b <= a) return;
    Burst bu;
    if (D_ == 1) {
        seg_f_ = 0;
        seg_noise_ = static_cast<double>(median_floor(-nfft_ / 2, nfft_)) / win_pow_;
        demod(a, b, fc, bw, &bu);
    } else {
        // to FS around the package's centre; the noise: the floor of the 64
        // bins (240 kHz) around it - the band isn't flat over 2.4 MHz
        const int64_t a2 = (a + D_ - 1) / D_, b2 = b / D_;
        if (b2 <= a2) return;
        const int c = static_cast<int>(std::lround(fc / BIN_HZ));
        seg_noise_ =
            static_cast<double>(median_floor(std::clamp(c - 32, -nfft_ / 2, nfft_ / 2 - 64), 64)) / win_pow_ * dec_h2_;
        constexpr int64_t PAD = 96;   // > the demodulator's filters' half length (161 / 2)
        make_segment(a2 - PAD, b2 + PAD, fc);
        demod(a2, b2, 0.0, bw, &bu);
    }
    if (bu.ook.empty() && bu.fsk.empty()) return;
    bu.antennas = w_.empty() ? 1 : M_;
    bu.comb_db = comb_db;
    out->push_back(std::move(bu));
}

// D > 1: FS samples s0..s1 of the input mixed down by f (Hz) - seg_[j] =
// sample s0 + j = input sample (s0 + j) D, low-pass DEC_CUT_HZ (Blackman,
// DEC_TAPS). The input is mixed a block at a time, then filtered at every
// D-th sample only
void FrontEnd::make_segment(int64_t s0, int64_t s1, double f) {
    const int L = static_cast<int>(dec_h_.size()), H = L / 2;
    seg0_ = s0;
    seg_f_ = f;
    seg_.assign(static_cast<size_t>(std::max<int64_t>(0, s1 - s0)), cf{});
    const double w0 = -2 * M_PI * f / fs_in_;
    const std::complex<double> rot(std::cos(w0), std::sin(w0));
    constexpr int64_t BLK = 2048;   // outputs per mixed block
    std::vector<cf> xm;
    for (int64_t j0 = s0; j0 < s1; j0 += BLK) {
        const int64_t j1 = std::min(s1, j0 + BLK);
        const int64_t i0 = j0 * D_ - H, i1 = (j1 - 1) * D_ + H + 1;   // the input samples they need
        xm.resize(static_cast<size_t>(i1 - i0));
        const double ph0 = std::remainder(w0 * static_cast<double>(i0), 2 * M_PI);
        std::complex<double> ph(std::cos(ph0), std::sin(ph0));
        for (size_t k = 0; k < xm.size(); k++) {
            const int64_t s = i0 + static_cast<int64_t>(k);
            cf v{};   // (outside the ring: silence)
            if (s >= 0 && s > abs_ - ring_n_ && s < abs_) {
                if (w_.empty()) v = at(s);
                else {   // every antenna, combined: w^H x
                    v = std::conj(w_[0]) * at(s);
                    for (int c = 1; c < M_; c++) v += std::conj(w_[static_cast<size_t>(c)]) * xat(c, s);
                }
            }
            xm[k] = v * cf(static_cast<float>(ph.real()), static_cast<float>(ph.imag()));
            ph *= rot;
            if ((k & 1023) == 1023) ph /= std::abs(ph);
        }
        const float* h = dec_h_.data();
        for (int64_t j = j0; j < j1; j++) {
            const cf* q = xm.data() + (j * D_ - H - i0);
            float re = 0, im = 0;
            for (int t = 0; t < L; t++) {
                re += q[t].real() * h[t];
                im += q[t].imag() * h[t];
            }
            seg_[static_cast<size_t>(j - s0)] = cf(re, im);
        }
    }
}

// the samples a..b (FS) mixed down by f0 and low-pass filtered (Hamming
// windowed sinc, cut-off fcut, taps ~ k fs / fcut); y[i] = sample a + i
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
        const cf v = src(a - half + static_cast<int64_t>(j));   // outside the ring / segment: silence
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

double FrontEnd::sigma(double h2) const { return std::sqrt(std::max(1e-30, seg_noise_ * h2)); }

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
                     std::fabs((c1 + c2) / 2) <= bw / 2 + 10e3 && std::fabs(fc + (c1 + c2) / 2) < FS / 2 - 5e3 &&
                     std::fabs(seg_f_ + fc + (c1 + c2) / 2) < fs_in_ / 2 - 5e3;
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
    out->fc = seg_f_ + f0;   // in the whole input band
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
