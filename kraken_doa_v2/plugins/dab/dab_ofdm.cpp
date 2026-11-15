#include "dab_ofdm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace dab {

namespace {

constexpr double PI2 = 6.283185307179586476925286766559;
constexpr int64_t NONE = std::numeric_limits<int64_t>::max();
// the FFT window starts this many samples before the first path's useful part
constexpr double BACKOFF = 48;
// null symbol window: [frame start + NULL_W0, + 2400) - as late as the
// first path's phase reference allows (32 samples margin), so the previous
// frame's last symbol from transmitters up to 280 us later than the first
// path (single frequency networks: they are) stays out
constexpr double NULL_W0 = 680;
constexpr int SEARCH_BLK = 24;           // samples per energy block
constexpr int SEARCH_W = 120;            // blocks in the null window (2880 samples)
constexpr int NMAX = 410;                // whole-kHz CFO search range (bins)
constexpr double PRS_MIN = 0.12;         // phase reference correlation below this = a bad frame

inline int wrap_bin(int b) {
    b %= NFFT;
    return b < 0 ? b + NFFT : b;
}
inline double wrap_half(double v, double period) {
    v = std::fmod(v, period);
    if (v > period / 2) v -= period;
    if (v < -period / 2) v += period;
    return v;
}

}   // namespace

OfdmRx::OfdmRx() {
    fin_ = kfft_alloc_complex(NFFT);
    fout_ = kfft_alloc_complex(NFFT);
    plan_ = kfft_plan_dft_1d(NFFT, fin_, fout_, KFFT_FORWARD);
    iplan_ = kfft_plan_dft_1d(NFFT, fin_, fout_, KFFT_BACKWARD);
    ring_[0].assign(size_t(1) << RING0_LOG2, cf(0, 0));
    for (int l = 1; l <= L; l++) car_[l].assign(K, cf(0, 0));
    have_car_.assign(L + 1, 0);
    fic_.assign(3 * SYM_BITS, 0);
    msc_.assign(static_cast<size_t>(72) * SYM_BITS, 0);
    msc_ok_.assign(72, 0);
    want_.assign(72, 0);
    for (auto& v : null_car_) v.assign(K, cf(0, 0));
    prs_.resize(K);
    prs_q_.resize(K);
    static const cf Q[4] = {cf(1, 0), cf(0, 1), cf(-1, 0), cf(0, -1)};
    for (int i = 0; i < K; i++) {
        prs_q_[i] = prs_quarter(carrier_of_index(i));
        prs_[i] = Q[prs_q_[i]];
    }
    const auto& F = freq_interleave();
    fidx_.resize(K);
    for (int n = 0; n < K; n++) fidx_[n] = carrier_index(F[n]);
    work_.resize(NFFT);
    rot_.resize(NFFT);
    reset();
}

OfdmRx::~OfdmRx() {
    kfft_destroy_plan(plan_);
    kfft_destroy_plan(iplan_);
    kfft_free(fin_);
    kfft_free(fout_);
}

void OfdmRx::reset() {
    state_ = SEARCH;
    written_ = 0;
    search_from_ = 0;
    s0_ = 0;
    eps_ = 0;
    f_tot_ = 0;
    nbin_ = 0;
    bad_frames_ = nbin_votes_ = nbin_cand_ = 0;
    m_ = 1;
    for (int k = 0; k < MAX_ANT; k++) valid_from_[k] = k == 0 ? 0 : NONE;
}

void OfdmRx::push(const cf* const* x, int m, size_t n) {
    if (n == 0) return;
    m = std::max(1, std::min(m, MAX_ANT));
    for (int k = 0; k < m; k++) {
        const int lg = k == 0 ? RING0_LOG2 : RINGK_LOG2;
        if (ring_[k].empty()) ring_[k].assign(size_t(1) << lg, cf(0, 0));
        if (valid_from_[k] == NONE) valid_from_[k] = written_;
        const int64_t mask = (int64_t(1) << lg) - 1;
        size_t i = 0;
        while (i < n) {
            const int64_t p = (written_ + static_cast<int64_t>(i)) & mask;
            const size_t len = std::min(n - i, static_cast<size_t>(mask + 1 - p));
            std::memcpy(&ring_[k][static_cast<size_t>(p)], x[k] + i, len * sizeof(cf));
            i += len;
        }
    }
    for (int k = m; k < MAX_ANT; k++) valid_from_[k] = NONE;
    m_ = m;
    written_ += static_cast<int64_t>(n);

    for (int guard = 0; guard < 64; guard++) {
        if (state_ == SEARCH) {
            const int64_t lo = written_ - (int64_t(1) << RING0_LOG2) + 64;
            if (search_from_ < lo) search_from_ = lo;
            if (written_ - search_from_ < 2 * FRAME + 4000) break;
            if (!search()) search_from_ += FRAME;
            continue;
        }
        // TRACK: the whole frame in the buffer?
        if (written_ < static_cast<int64_t>(std::ceil(s0_)) + FRAME + 64) break;
        if (s0_ - 64 < static_cast<double>(written_ - (int64_t(1) << RING0_LOG2))) {   // fell behind
            state_ = SEARCH;
            search_from_ = written_ - 2 * FRAME - 8000;
            if (on_sync) on_sync(false);
            continue;
        }
        track_frame();
    }
}

bool OfdmRx::have(int ant, int64_t from, int64_t to) const {
    if (ant < 0 || ant >= MAX_ANT || ring_[ant].empty() || valid_from_[ant] == NONE) return false;
    const int64_t cap = static_cast<int64_t>(ring_[ant].size());
    return from >= valid_from_[ant] && from >= written_ - cap && to <= written_ && from >= 0;
}

void OfdmRx::copy_window(int ant, int64_t w, cf* dst) const {
    const std::vector<cf>& r = ring_[ant];
    const int64_t mask = static_cast<int64_t>(r.size()) - 1;
    int64_t p = w & mask;
    size_t done = 0;
    while (done < static_cast<size_t>(NFFT)) {
        const size_t len = std::min(static_cast<size_t>(NFFT) - done, static_cast<size_t>(mask + 1 - p));
        std::memcpy(dst + done, &r[static_cast<size_t>(p)], len * sizeof(cf));
        done += len;
        p = 0;
    }
}

void OfdmRx::set_rot(double f) {
    if (std::fabs(f - f_rot_) < 1e-9) return;
    f_rot_ = f;
    const double w = -PI2 * f / FS;
    for (int m = 0; m < NFFT; m++) {
        const double a = w * m;
        rot_[m] = cf(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

void OfdmRx::fft_window(int ant, int64_t w) {
    copy_window(ant, w, work_.data());
    float* in = &fin_[0][0];
    const cf* __restrict s = work_.data();
    const cf* __restrict r = rot_.data();
    for (int m = 0; m < NFFT; m++) {
        const cf v = s[m] * r[m];
        in[2 * m] = v.real();
        in[2 * m + 1] = v.imag();
    }
    kfft_execute_dft(plan_, fin_, fout_);
}

// carriers out of fout_ (dense index), corrected for the window offset delta
// (a linear phase over the carriers) and the CFO's phase at the window start
// (relative to the frame start fs0)
static void extract(const kfft_complex* fo, int nbin, double f_tot, int64_t w, double delta, int64_t fs0, cf* out) {
    const double c0 = -PI2 * f_tot * static_cast<double>(w - fs0) / FS;
    const double step = -PI2 * delta / NFFT;
    // carrier k: phase c0 + k step
    for (int half = 0; half < 2; half++) {
        const int k0 = half == 0 ? -768 : 1;
        std::complex<double> ph(std::cos(c0 + k0 * step), std::sin(c0 + k0 * step));
        const std::complex<double> st(std::cos(step), std::sin(step));
        for (int j = 0; j < 768; j++) {
            const int k = k0 + j;
            const int b = wrap_bin(k + nbin);
            const cf y(fo[b][0], fo[b][1]);
            out[carrier_index(k)] = y * cf(static_cast<float>(ph.real()), static_cast<float>(ph.imag()));
            ph *= st;
        }
    }
}

void OfdmRx::symbol_carriers(int ant, int64_t w, double delta, int64_t fs0, cf* out) {
    fft_window(ant, w);
    extract(fout_, nbin_, f_tot_, w, delta, fs0, out);
}

double OfdmRx::useful_start(int l) const { return (T_NULL + (l - 1) * T_S + T_G) * (1.0 + eps_); }

double OfdmRx::cp_cfo(double s0, double* quality) const {
    // the end of every guard interval against the end of its symbol
    const std::vector<cf>& r = ring_[0];
    const int64_t mask = static_cast<int64_t>(r.size()) - 1;
    const int a = state_ == TRACK ? 400 : 470, b = state_ == TRACK ? 16 : 130;
    std::complex<double> acc(0, 0);
    double p0 = 0, p1 = 0;
    for (int l = 1; l <= L; l++) {
        const int64_t u = static_cast<int64_t>(std::llround(s0 + useful_start(l)));
        float cr = 0, ci = 0, q0 = 0, q1 = 0;
        const int64_t a0 = u - a, len = a - b;
        const int64_t i0 = a0 & mask, i1 = (a0 + NFFT) & mask;
        if (i0 + len <= mask + 1 && i1 + len <= mask + 1) {   // contiguous: a vectorisable loop
            const float* __restrict x0 = reinterpret_cast<const float*>(&r[static_cast<size_t>(i0)]);
            const float* __restrict x1 = reinterpret_cast<const float*>(&r[static_cast<size_t>(i1)]);
            for (int64_t n = 0; n < len; n++) {
                const float ar = x0[2 * n], ai = x0[2 * n + 1], br = x1[2 * n], bi = x1[2 * n + 1];
                cr += ar * br + ai * bi;
                ci += ai * br - ar * bi;
                q0 += ar * ar + ai * ai;
                q1 += br * br + bi * bi;
            }
        } else {
            for (int64_t n = a0; n < u - b; n++) {
                const cf x0 = r[static_cast<size_t>(n & mask)], x1 = r[static_cast<size_t>((n + NFFT) & mask)];
                const cf c = x0 * std::conj(x1);
                cr += c.real();
                ci += c.imag();
                q0 += std::norm(x0);
                q1 += std::norm(x1);
            }
        }
        acc += std::complex<double>(cr, ci);
        p0 += q0;
        p1 += q1;
    }
    if (quality) *quality = p0 > 0 && p1 > 0 ? std::abs(acc) / std::sqrt(p0 * p1) : 0;
    return -std::arg(acc) * 1000.0 / PI2;
}

// normalised differential correlation of fout_ with the phase reference at bin shift nbin
double OfdmRx::prs_corr(int nbin, double* norm) const {
    std::complex<double> acc(0, 0);
    double nn = 0;
    for (int i = 0; i + 1 < K; i++) {
        if (i == 767) continue;   // k = -1 -> 1 is not adjacent
        const int k = carrier_of_index(i);
        const int b0 = wrap_bin(k + nbin), b1 = wrap_bin(k + 1 + nbin);
        const cf y0(fout_[b0][0], fout_[b0][1]), y1(fout_[b1][0], fout_[b1][1]);
        cf v = y1 * std::conj(y0);
        const int dq = (prs_q_[i + 1] - prs_q_[i]) & 3;   // remove z_k+1 conj(z_k)
        if (dq == 1) v = cf(v.imag(), -v.real());
        else if (dq == 2) v = -v;
        else if (dq == 3) v = cf(-v.imag(), v.real());
        acc += std::complex<double>(v);
        nn += std::abs(v);
    }
    if (norm) *norm = nn;
    return nn > 0 ? std::abs(acc) / nn : 0;
}

bool OfdmRx::impulse_timing(double* tau_first, double* tau_peak) {
    // H_k = Y_k conj(z_k) (Hann window over the carriers) -> impulse response
    std::vector<float>& win = hann_;
    if (win.empty()) {
        win.resize(K);
        for (int i = 0; i < K; i++) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(PI2 * (i + 1) / (K + 1)));
    }
    std::vector<cf>& g = work_;
    std::fill(g.begin(), g.end(), cf(0, 0));
    for (int i = 0; i < K; i++) {
        const int k = carrier_of_index(i);
        const int b = wrap_bin(k + nbin_);
        const cf y(fout_[b][0], fout_[b][1]);
        g[static_cast<size_t>(wrap_bin(k))] = y * std::conj(prs_[i]) * win[i];
    }
    for (int i = 0; i < NFFT; i++) {
        fin_[i][0] = g[i].real();
        fin_[i][1] = g[i].imag();
    }
    kfft_execute_dft(iplan_, fin_, fout_);   // NB: fout_ now holds the impulse response
    std::vector<float>& p = cir_;
    p.resize(NFFT);
    int pk = 0;
    double sum = 0;
    for (int i = 0; i < NFFT; i++) {
        p[i] = fout_[i][0] * fout_[i][0] + fout_[i][1] * fout_[i][1];
        sum += p[i];
        if (p[i] > p[pk]) pk = i;
    }
    const double mean = sum / NFFT;
    if (!(p[pk] > 20 * mean)) return false;   // no clear path
    // the first path: earliest local maximum within 15 dB of the peak, up to 600 samples before it
    int first = pk;
    for (int d = 600; d > 0; d--) {
        const int i = wrap_bin(pk - d);
        const float v = p[i];
        if (v > 0.0316f * p[pk] && v >= p[wrap_bin(i - 1)] && v >= p[wrap_bin(i + 1)]) {
            first = i;
            break;
        }
    }
    auto frac = [&p](int i) {
        const double a = std::sqrt(p[wrap_bin(i - 1)]), b = std::sqrt(p[i]), c = std::sqrt(p[wrap_bin(i + 1)]);
        const double den = a - 2 * b + c;
        double o = den < 0 ? 0.5 * (a - c) / den : 0;
        return std::max(-0.5, std::min(0.5, o));
    };
    auto sgn = [](int i) { return i > NFFT / 2 ? i - NFFT : i; };
    *tau_first = sgn(first) + frac(first);
    *tau_peak = sgn(pk) + frac(pk);
    return true;
}

void OfdmRx::soft_bits(const cf* cur, const cf* prev, int8_t* out, double* mer_acc, int* mer_n) {
    std::vector<cf>& d = dbuf_;
    d.resize(K);
    float a = 0;
    for (int i = 0; i < K; i++) {
        d[i] = cur[i] * std::conj(prev[i]);
        a += std::fabs(d[i].real()) + std::fabs(d[i].imag());
    }
    const float g = a > 0 ? 48.0f * K / a : 0.0f;   // mean |soft| ~ 24
    auto q = [](float v) {
        v = std::max(-127.0f, std::min(127.0f, v));
        return static_cast<int8_t>(std::lrint(v));
    };
    for (int n = 0; n < K; n++) {
        const cf v = d[fidx_[n]];
        out[n] = q(v.real() * g);
        out[n + K] = q(v.imag() * g);
    }
    if (mer_acc) {
        // phase error against the nearest QPSK point (pi/4 + n pi/2)
        double s = 0;
        for (int i = 0; i < K; i++) {
            double e = std::atan2(d[i].imag(), d[i].real()) - 0.78539816339744831;
            e = wrap_half(e, 1.5707963267948966);
            s += e * e;
        }
        *mer_acc += s;
        *mer_n += K;
    }
}

bool OfdmRx::search() {
    const int64_t base = search_from_;
    const int nblk = (2 * FRAME + 3200) / SEARCH_BLK;
    const int span = FRAME / SEARCH_BLK;   // 9600
    e_.assign(static_cast<size_t>(nblk), 0.0f);
    const std::vector<cf>& r = ring_[0];
    const int64_t mask = static_cast<int64_t>(r.size()) - 1;
    double tot = 0;
    for (int j = 0; j < nblk; j++) {
        float s = 0;
        const int64_t p0 = base + static_cast<int64_t>(j) * SEARCH_BLK;
        for (int i = 0; i < SEARCH_BLK; i++) s += std::norm(r[static_cast<size_t>((p0 + i) & mask)]);
        e_[j] = s;
        tot += s;
    }
    const double mu = tot / nblk;
    if (!(mu > 1e-20)) return false;
    std::vector<double> c(static_cast<size_t>(nblk) + 1, 0.0);
    for (int j = 0; j < nblk; j++) c[j + 1] = c[j] + e_[j];
    double best = 1e9;
    int bj = -1;
    for (int j = 0; j < span; j++) {
        const double r1 = (c[j + SEARCH_W] - c[j]) / (SEARCH_W * mu);
        const double r2 = (c[j + span + SEARCH_W] - c[j + span]) / (SEARCH_W * mu);
        const double v = std::max(r1, r2);
        if (v < best) {
            best = v;
            bj = j;
        }
    }
    if (log) {
        char b[160];
        snprintf(b, sizeof b, "search @%lld: null dip %.3f at block %d", static_cast<long long>(base), best, bj);
        log(b);
    }
    if (bj < 0 || best > 0.65) return false;   // (noise outside the 1.536 MHz fills the dip at low SNR)
    // the 2880-sample window sits somewhere in the 3112.5-sample null symbol
    const double s0c = static_cast<double>(base) + bj * SEARCH_BLK - (T_NULL - SEARCH_W * SEARCH_BLK) / 2;
    if (!acquire(s0c)) {
        search_from_ = base + static_cast<int64_t>(bj) * SEARCH_BLK + FRAME / 2;
        return true;   // searched: continue from after this null
    }
    return true;
}

double OfdmRx::cir_quality(const std::vector<cf>& spec, int nb) {
    for (int i = 0; i < NFFT; i++) fin_[i][0] = fin_[i][1] = 0;
    for (int i = 0; i < K; i++) {
        const int k = carrier_of_index(i);
        const cf h = spec[static_cast<size_t>(wrap_bin(k + nb))] * std::conj(prs_[i]);
        fin_[wrap_bin(k)][0] = h.real();
        fin_[wrap_bin(k)][1] = h.imag();
    }
    kfft_execute_dft(iplan_, fin_, fout_);
    double pk = 0, sum = 0;
    for (int i = 0; i < NFFT; i++) {
        const double v = fout_[i][0] * fout_[i][0] + fout_[i][1] * fout_[i][1];
        sum += v;
        pk = std::max(pk, v);
    }
    return sum > 0 ? pk / (sum / NFFT) : 0;
}

bool OfdmRx::acquire(double s0c) {
    eps_ = 0;
    double q = 0;
    const double ffrac = cp_cfo(s0c, &q);
    if (q < 0.05) return false;
    set_rot(ffrac);
    const int64_t w1 = static_cast<int64_t>(std::llround(s0c + useful_start(1) - T_G / 2));
    if (!have(0, w1, w1 + NFFT)) return false;
    fft_window(0, w1);
    // whole-kHz offset: the phase reference's differential correlation at every bin shift
    std::vector<cf> v(NFFT);
    std::vector<float> va(NFFT);
    for (int b = 0; b < NFFT; b++) {
        const int b1 = wrap_bin(b + 1);
        v[b] = cf(fout_[b1][0], fout_[b1][1]) * std::conj(cf(fout_[b][0], fout_[b][1]));
        va[b] = std::abs(v[b]);
    }
    double best = 0;
    int bn = 0;
    std::vector<double> met(2 * NMAX + 1);
    for (int nb = -NMAX; nb <= NMAX; nb++) {
        std::complex<double> acc(0, 0);
        double nn = 0;
        for (int i = 0; i + 1 < K; i++) {
            if (i == 767) continue;
            const int b = wrap_bin(carrier_of_index(i) + nb);
            cf x = v[b];
            const int dq = (prs_q_[i + 1] - prs_q_[i]) & 3;
            if (dq == 1) x = cf(x.imag(), -x.real());
            else if (dq == 2) x = -x;
            else if (dq == 3) x = cf(-x.imag(), x.real());
            acc += std::complex<double>(x);
            nn += va[b];
        }
        met[nb + NMAX] = nn > 0 ? std::abs(acc) / nn : 0;
    }
    // the differential correlation repeats for shifts that keep the h pattern
    // (multiples of 32 carriers): the candidates are told apart by the
    // coherent impulse response (a flat channel only at the right shift)
    std::vector<int> order(2 * NMAX + 1);
    for (int i = 0; i <= 2 * NMAX; i++) order[i] = i - NMAX;
    std::partial_sort(order.begin(), order.begin() + 12, order.end(),
                      [&met](int a, int b) { return met[a + NMAX] > met[b + NMAX]; });
    std::vector<cf> spec(NFFT);
    for (int b = 0; b < NFFT; b++) spec[b] = cf(fout_[b][0], fout_[b][1]);
    double qbest = 0, qsecond = 0;
    for (int j = 0; j < 12; j++) {
        const int nb = order[j];
        if (met[nb + NMAX] < 0.1) break;
        const double qq = cir_quality(spec, nb);
        if (qq > qbest) {
            qsecond = qbest;
            qbest = qq;
            bn = nb;
            best = met[nb + NMAX];
        } else if (qq > qsecond) {
            qsecond = qq;
        }
    }
    if (log) {
        char b[200];
        snprintf(b, sizeof b, "acquire: CP %.3f -> %+.1f Hz, PRS %.3f at %+d kHz, impulse response peak/mean %.0f (next %.0f)", q, ffrac,
                 best, bn, qbest, qsecond);
        log(b);
    }
    if (best < 0.15 || qbest < 40 || qbest < 3.0 * qsecond) return false;
    for (int b = 0; b < NFFT; b++) {
        fout_[b][0] = spec[b].real();
        fout_[b][1] = spec[b].imag();
    }
    nbin_ = bn;
    f_tot_ = 1000.0 * bn + ffrac;
    double tf = 0, tp = 0;
    if (!impulse_timing(&tf, &tp)) return false;
    const double s0 = static_cast<double>(w1) + tf - useful_start(1);
    if (std::fabs(s0 - s0c) > 1200) return false;
    s0_ = s0;
    state_ = TRACK;
    bad_frames_ = 0;
    nbin_votes_ = 0;
    if (on_sync) on_sync(true);
    return true;
}

void OfdmRx::track_frame() {
    const int64_t fs0 = static_cast<int64_t>(std::floor(s0_));
    // 1. CFO from the cyclic prefixes of this frame
    double q = 0;
    const double fcp = cp_cfo(s0_, &q);
    if (q > 0.05) f_tot_ += 0.5 * wrap_half(fcp - f_tot_, 1000.0);
    set_rot(f_tot_ - 1000.0 * nbin_);
    // 2. phase reference symbol at the predicted timing
    const int64_t w1 = static_cast<int64_t>(std::llround(s0_ + useful_start(1) - BACKOFF));
    fft_window(0, w1);
    double nrm = 0;
    const double metric = prs_corr(nbin_, &nrm);
    const double mlo = prs_corr(nbin_ - 1, nullptr), mhi = prs_corr(nbin_ + 1, nullptr);
    if (std::max(mlo, mhi) > 1.5 * metric && std::max(mlo, mhi) > PRS_MIN) {
        const int cand = mlo > mhi ? nbin_ - 1 : nbin_ + 1;
        if (cand == nbin_cand_) nbin_votes_++;
        else {
            nbin_cand_ = cand;
            nbin_votes_ = 1;
        }
        if (nbin_votes_ >= 3) {   // the carrier moved by a whole kHz
            f_tot_ += 1000.0 * (cand - nbin_);
            nbin_ = cand;
            nbin_votes_ = 0;
        }
    } else {
        nbin_votes_ = 0;
    }
    // PRS power (for the SNR) before fout_ is reused
    double prs_pow = 0;
    for (int i = 0; i < K; i++) {
        const int b = wrap_bin(carrier_of_index(i) + nbin_);
        prs_pow += fout_[b][0] * fout_[b][0] + fout_[b][1] * fout_[b][1];
    }
    prs_pow /= K;
    std::vector<cf> prs_bins(NFFT);
    for (int b = 0; b < NFFT; b++) prs_bins[b] = cf(fout_[b][0], fout_[b][1]);
    // 3. timing from the impulse response
    const bool good = metric >= PRS_MIN;
    double tf = 0, tp = 0;
    if (good && impulse_timing(&tf, &tp)) {
        const double meas = static_cast<double>(w1) + tf - useful_start(1);
        const double err = meas - s0_;
        if (std::fabs(err) > 400) {
            s0_ = meas;
        } else {
            s0_ += 0.5 * err;
            eps_ += 0.05 * err / FRAME;
            eps_ = std::max(-2e-4, std::min(2e-4, eps_));
        }
    }
    // restore the PRS spectrum into fout_ and take its carriers with the final timing
    for (int b = 0; b < NFFT; b++) {
        fout_[b][0] = prs_bins[b].real();
        fout_[b][1] = prs_bins[b].imag();
    }
    std::fill(have_car_.begin(), have_car_.end(), 0);
    extract(fout_, nbin_, f_tot_, w1, static_cast<double>(w1) - (s0_ + useful_start(1)), fs0, car_[1].data());
    have_car_[1] = 1;
    // 4. FIC (symbols 2..4) and the wanted MSC symbols
    std::vector<uint8_t> need(L + 2, 0);
    for (int l = 2; l <= 4; l++) need[l] = 1;
    for (int l = 5; l <= L; l++)
        if (want_[l - 5]) need[l] = need[l - 1] = 1;
    std::fill(msc_ok_.begin(), msc_ok_.end(), 0);
    double mer_acc = 0;
    int mer_n = 0;
    for (int l = 2; l <= L; l++) {
        if (!need[l]) continue;
        const double u = s0_ + useful_start(l);
        const int64_t w = static_cast<int64_t>(std::llround(u - BACKOFF));
        symbol_carriers(0, w, static_cast<double>(w) - u, fs0, car_[l].data());
        have_car_[l] = 1;
        if (!have_car_[l - 1]) continue;
        if (l <= 4) {
            soft_bits(car_[l].data(), car_[l - 1].data(), &fic_[static_cast<size_t>(l - 2) * SYM_BITS], &mer_acc, &mer_n);
        } else if (want_[l - 5]) {
            soft_bits(car_[l].data(), car_[l - 1].data(), &msc_[static_cast<size_t>(l - 5) * SYM_BITS], nullptr, nullptr);
            msc_ok_[l - 5] = 1;
        }
    }
    // 5. the null symbol (TII) on every antenna that has it, and the noise floor
    Frame f;
    const int64_t w0 = static_cast<int64_t>(std::llround(s0_ + NULL_W0));
    f.null_w0 = w0;
    int nm = 0;
    for (int k = 0; k < m_; k++) {
        if (!have(k, w0, w0 + NFFT)) break;
        symbol_carriers(k, w0, static_cast<double>(w0) - (s0_ + (T_NULL - NFFT)), fs0, null_car_[k].data());
        f.null_car[k] = null_car_[k].data();
        nm++;
    }
    f.null_m = nm;
    double noise = 0;
    if (nm > 0) {
        std::vector<float>& pw = pbuf_;
        pw.resize(K);
        for (int i = 0; i < K; i++) pw[i] = std::norm(null_car_[0][i]);
        std::nth_element(pw.begin(), pw.begin() + K / 2, pw.end());
        noise = pw[K / 2] / 0.6931;   // median of an exponential = ln 2 x mean
    }
    f.null_noise = noise;
    f.start_exact = s0_;
    f.start = static_cast<int64_t>(std::llround(s0_));
    f.prs_metric = metric;
    f.snr_db = noise > 0 && prs_pow > noise ? 10 * std::log10((prs_pow - noise) / noise) : -99;
    f.mer_db = mer_n > 0 && mer_acc > 0 ? -10 * std::log10(mer_acc / mer_n) : 0;
    f.cfo_hz = f_tot_;
    f.clock_ppm = eps_ * 1e6;
    f.fic = fic_.data();
    f.msc = msc_.data();
    f.msc_ok = msc_ok_.data();
    if (good) bad_frames_ = 0;
    else bad_frames_++;
    if (on_frame) on_frame(f);
    s0_ += FRAME * (1.0 + eps_);
    if (bad_frames_ >= 2) {   // (a gap in the input: search again at once, from the data at hand)
        state_ = SEARCH;
        search_from_ = static_cast<int64_t>(s0_) - FRAME;
        if (on_sync) on_sync(false);
    }
}

}   // namespace dab
