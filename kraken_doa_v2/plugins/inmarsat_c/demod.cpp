// Coherent BPSK demodulator for the Inmarsat-C TDM: see demod.hpp
#include "demod.hpp"

#include <algorithm>
#include <cmath>

namespace inmc {

namespace {
constexpr int MF_SPAN = 4;        // RRC span, symbols each side
constexpr int HIST = 64;          // matched-filter output ring (power of 2)
constexpr double TWO_PI = 6.283185307179586;
// loops (per symbol): Costas ~10 Hz, timing ~1 % of the symbol rate
constexpr float COSTAS_KP = 0.022f, COSTAS_KI = 2.5e-4f;
constexpr float TIMING_KP = 0.02f, TIMING_KI = 2e-5f;
}  // namespace

BpskDemod::BpskDemod(double fs) : fs_(fs), sps_nom_(fs / 1200.0) {
    acq_buf_.reserve(NACQ);
    win_.resize(NACQ);
    for (int i = 0; i < NACQ; i++) win_[i] = 0.5f - 0.5f * std::cos(2 * kp::PI * i / (NACQ - 1));
    fin_ = kfft_alloc_complex(NACQ);
    fout_ = kfft_alloc_complex(NACQ);
    plan_ = kfft_plan_dft_1d(NACQ, fin_, fout_, KFFT_FORWARD);
    // spectrum template: raised cosine (alpha 1) PSD over +-1200 Hz, zero mean
    const double df = fs_ / NACQ;
    const int w = static_cast<int>(std::lround(1200 / df));
    tmpl_.resize(2 * w + 1);
    double mean = 0;
    for (int m = -w; m <= w; m++) {
        const double c = std::cos(kp::PI * m * df / 2400.0);
        tmpl_[m + w] = static_cast<float>(c * c);
        mean += c * c;
    }
    mean /= tmpl_.size();
    for (float& t : tmpl_) t -= static_cast<float>(mean);
    mf_.set_taps(kp::fir_rrc(static_cast<int>(std::lround(sps_nom_)), MF_SPAN, 1.0f));
    hist_.assign(HIST, kp::cf(0, 0));
    reset();
}

BpskDemod::~BpskDemod() {
    kfft_destroy_plan(plan_);
    kfft_free(fin_);
    kfft_free(fout_);
}

void BpskDemod::reset() {
    state_ = ACQ;
    acq_buf_.clear();
    acq_start_ = n_in_;
    lock_ = 0;
    unlocked_symbols_ = 0;
}

double BpskDemod::carrier_hz() const { return freq_ * fs_ / TWO_PI; }

float BpskDemod::esn0_db() const {
    const float s = sig_ - noise_;
    if (s <= 0 || noise_ <= 0) return -99;
    return 10 * std::log10(s / (2 * noise_));
}

void BpskDemod::process(const kp::cf* x, size_t n, std::vector<Symbol>& out) {
    for (size_t i = 0; i < n; i++) {
        if (state_ == ACQ) {
            if (acq_buf_.empty()) acq_start_ = n_in_;
            acq_buf_.push_back(x[i]);
            n_in_++;
            if (static_cast<int>(acq_buf_.size()) == NACQ) {
                acquire();
                if (state_ == TRACK) {
                    std::vector<kp::cf> buf;
                    buf.swap(acq_buf_);
                    for (const kp::cf& v : buf) {
                        if (state_ != TRACK) break;
                        track_sample(v, out);
                    }
                } else {
                    // slide by half a block: the next try overlaps this one
                    acq_buf_.erase(acq_buf_.begin(), acq_buf_.begin() + NACQ / 2);
                    acq_start_ += NACQ / 2;
                }
            }
        } else {
            n_in_++;
            track_sample(x[i], out);
        }
    }
}

void BpskDemod::acquire() {
    acq_count_++;
    const int N = NACQ;
    const double df = fs_ / N;
    // 1) coarse: spectrum shape
    for (int i = 0; i < N; i++) {
        fin_[i][0] = acq_buf_[i].real() * win_[i];
        fin_[i][1] = acq_buf_[i].imag() * win_[i];
    }
    kfft_execute(plan_);
    std::vector<float> P(N);   // centred: P[k + N/2] = bin k
    for (int k = 0; k < N; k++) {
        const int c = (k + N / 2) % N;
        P[c] = fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];
    }
    const int w = static_cast<int>(tmpl_.size() / 2);
    const int lim = std::min(static_cast<int>(search_hz_ / df), N / 2 - w - 1);
    auto score = [&](int c) {
        double s = 0;
        const float* p = &P[N / 2 + c - w];
        for (int m = 0; m <= 2 * w; m++) s += tmpl_[m] * p[m];
        return s;
    };
    int best = 0;
    double bs = -1e300;
    for (int c = -lim; c <= lim; c += 4) {
        const double s = score(c);
        if (s > bs) bs = s, best = c;
    }
    for (int c = std::max(-lim, best - 4); c <= std::min(lim, best + 4); c++) {
        const double s = score(c);
        if (s > bs) bs = s, best = c;
    }
    const double f0 = best * df;
    // 2) fine: the squared signal's line at 2 (fc - f0)
    const double w0 = -TWO_PI * f0 / fs_;
    for (int i = 0; i < N; i++) {
        const kp::cf r(static_cast<float>(std::cos(w0 * i)), static_cast<float>(std::sin(w0 * i)));
        const kp::cf y = acq_buf_[i] * r;
        const kp::cf q = y * y;
        fin_[i][0] = q.real() * win_[i];
        fin_[i][1] = q.imag() * win_[i];
    }
    kfft_execute(plan_);
    auto pw = [&](int k) {
        const int b = (k % N + N) % N;
        return static_cast<double>(fout_[b][0]) * fout_[b][0] + static_cast<double>(fout_[b][1]) * fout_[b][1];
    };
    const int kmax = static_cast<int>(120 / df);
    int kb = 0;
    double pb = -1;
    for (int k = -kmax; k <= kmax; k++)
        if (pw(k) > pb) pb = pw(k), kb = k;
    // noise reference: the squared spectrum between 300 Hz and 2.4 kHz from the line
    double nref = 0;
    int nn = 0;
    for (int k = static_cast<int>(300 / df); k < static_cast<int>(2400 / df); k += 3) {
        nref += pw(kb + k) + pw(kb - k);
        nn += 2;
    }
    nref /= std::max(1, nn);
    acq_peak_db_ = 10 * std::log10(pb / std::max(nref, 1e-30));
    if (acq_peak_db_ < 11) return;   // no BPSK carrier here
    // parabolic interpolation of the peak
    const double a = pw(kb - 1), b = pb, c = pw(kb + 1);
    double d = 0;
    if (a + c - 2 * b < 0) d = 0.5 * (a - c) / (a + c - 2 * b);
    const double fc = f0 + (kb + d) * df / 2;
    freq_ = TWO_PI * fc / fs_;
    phase_ = 0;
    state_ = TRACK;
    track_start_ = acq_start_;
    mf_.reset();
    std::fill(hist_.begin(), hist_.end(), kp::cf(0, 0));
    mf_count_ = 0;
    sps_ = sps_nom_;
    next_ = 2 * MF_SPAN * sps_nom_;   // first strobe once the filter is full
    prev_sym_ = kp::cf(0, 0);
    amp2_ = 0;
    sig_ = noise_ = 0;
    lock_ = 0.5f;
    symbols_ = 0;
    unlocked_symbols_ = 0;
}

kp::cf BpskDemod::interp(double idx) const {
    // cubic (Catmull-Rom) between hist[i] and hist[i+1], i = floor(idx)
    const int64_t i = static_cast<int64_t>(std::floor(idx));
    const float t = static_cast<float>(idx - i);
    const kp::cf y0 = hist_[(i - 1) & (HIST - 1)], y1 = hist_[i & (HIST - 1)];
    const kp::cf y2 = hist_[(i + 1) & (HIST - 1)], y3 = hist_[(i + 2) & (HIST - 1)];
    const kp::cf a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const kp::cf a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const kp::cf a2 = -0.5f * y0 + 0.5f * y2;
    return ((a0 * t + a1) * t + a2) * t + y1;
}

void BpskDemod::track_sample(kp::cf x, std::vector<Symbol>& out) {
    const kp::cf r(static_cast<float>(std::cos(phase_)), static_cast<float>(-std::sin(phase_)));
    phase_ += freq_;
    if (phase_ > kp::PI) phase_ -= TWO_PI;
    else if (phase_ < -kp::PI) phase_ += TWO_PI;
    const kp::cf m = mf_.push(x * r);
    hist_[mf_count_ & (HIST - 1)] = m;
    mf_count_++;
    if (mf_count_ - 1 < static_cast<int64_t>(std::floor(next_)) + 2) return;
    // a symbol strobe
    const kp::cf s = interp(next_);
    const kp::cf mid = interp(next_ - sps_ / 2);
    if (amp2_ <= 0) amp2_ = std::norm(s) + 1e-20f;
    const float g = 1.0f / std::sqrt(amp2_);
    // Gardner
    const float te = std::real((prev_sym_ - s) * std::conj(mid)) * g * g;
    const float tec = std::clamp(te, -1.0f, 1.0f);
    sps_ += TIMING_KI * tec;
    sps_ = std::clamp(sps_, sps_nom_ * 0.995, sps_nom_ * 1.005);
    const double strobe = next_;
    next_ += sps_ + TIMING_KP * tec;
    prev_sym_ = s;
    // Costas (decision directed)
    const float pe = std::clamp((s.real() >= 0 ? s.imag() : -s.imag()) * g, -1.0f, 1.0f);
    phase_ += COSTAS_KP * pe;
    freq_ += COSTAS_KI * pe / sps_;
    amp2_ = 0.995f * amp2_ + 0.005f * std::norm(s);
    const float re2 = s.real() * s.real() * g * g, im2 = s.imag() * s.imag() * g * g;
    if (symbols_ == 0) sig_ = re2, noise_ = im2;
    sig_ = 0.998f * sig_ + 0.002f * re2;
    noise_ = 0.998f * noise_ + 0.002f * im2;
    lock_ = (sig_ - noise_) / std::max(sig_ + noise_, 1e-9f);
    symbols_++;
    Symbol o;
    o.soft = s.real() * g;
    o.quad = s.imag() * g;
    o.pos = static_cast<double>(track_start_) + strobe - MF_SPAN * sps_nom_;
    out.push_back(o);
    // lost: back to acquisition after 5 s without lock
    if (symbols_ > 2400 && lock_ < 0.15f) {
        if (++unlocked_symbols_ > 6000) reset();
    } else {
        unlocked_symbols_ = 0;
    }
}

}  // namespace inmc
