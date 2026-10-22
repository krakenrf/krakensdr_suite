#pragma once
// RDS physical layer (EN 50067 / IEC 62106-1 section 1): a 57 kHz DSB-SC
// subcarrier (or an RDS2 data stream at 66.5 / 71.25 / 76 kHz) carrying
// 1187.5 bit/s, differentially encoded, every bit a biphase symbol (a +
// and a - half-symbol, each shaped by H(f) = cos(pi f td / 4), i.e. a
// root-raised cosine of roll-off 1 at 2375 half-symbols/s).
//
// RdsDemod: MPX (the FM discriminator, Hz, real) -> mixed to 0 Hz -> 19 kHz
// (8 samples per half-symbol) -> RRC matched filter -> Gardner symbol timing
// (phase-free) -> Costas carrier loop -> biphase pairing -> differential
// decoding. Out: soft data bits (sign = bit, size = reliability, ~1 on a
// clean signal) with the MPX sample time of each bit's centre.

#include "kraken_dsp.hpp"

#include <liquid/liquid.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace rds {

using kp::cf;

constexpr double BIT_RATE = 1187.5;
constexpr double HALF_RATE = 2 * BIT_RATE;    // 2375 half-symbols/s
constexpr double DEMOD_RATE = 19000;          // 8 samples per half-symbol
constexpr int SPS = 8;
constexpr int MF_SPAN = 4;                    // RRC taps: +-4 half-symbols

// atan2 to ~1e-5 rad (a minimax polynomial for atan on [0, 1]): in the FM
// discriminator at 480 kHz that is < 1 Hz of deviation. Branch-free, so the
// discriminator's loop vectorises
inline float fast_atan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    const float a = mn / (mx + 1e-30f);
    const float s = a * a;
    float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f - s * 0.01172120f)))));
    r = ay > ax ? 1.57079633f - r : r;
    r = x < 0 ? 3.14159265f - r : r;
    return y < 0 ? -r : r;
}

// e^{-j w n}, by recurrence (renormalised now and then)
class Phasor {
public:
    void set(double hz, double fs) {
        const double w = -2 * M_PI * hz / fs;
        step_ = std::complex<double>(std::cos(w), std::sin(w));
        v_ = {1, 0};
        n_ = 0;
    }
    cf next() {
        const cf r(static_cast<float>(v_.real()), static_cast<float>(v_.imag()));
        v_ *= step_;
        if (++n_ == 4096) {
            n_ = 0;
            v_ /= std::abs(v_);
        }
        return r;
    }
private:
    std::complex<double> v_{1, 0}, step_{1, 0};
    int n_ = 0;
};

// e^{-j w n} from a table when the frequency repeats within a few thousand
// samples (fs and hz on a common grid: 57 kHz at 480 kHz repeats every 160),
// else by the recurrence; mix() runs over blocks (the table loop vectorises)
class Mixer {
public:
    void set(double hz, double fs) {
        tab_.clear();
        pos_ = 0;
        rec_.set(hz, fs);
        // the period: the smallest n with n * hz / fs whole
        for (long n = 1; n <= 8192; n++) {
            const double c = n * hz / fs;
            if (std::fabs(c - std::round(c)) < 1e-9 * n) {
                tab_.resize(static_cast<size_t>(n));
                for (long i = 0; i < n; i++) {
                    const double p = -2 * M_PI * hz / fs * static_cast<double>(i);
                    tab_[static_cast<size_t>(i)] = cf(static_cast<float>(std::cos(p)), static_cast<float>(std::sin(p)));
                }
                break;
            }
        }
    }
    template <typename T>
    void mix(const T* x, cf* y, size_t n) {
        if (tab_.empty()) {
            for (size_t i = 0; i < n; i++) y[i] = x[i] * rec_.next();
            return;
        }
        const size_t L = tab_.size();
        size_t i = 0;
        while (i < n) {
            const size_t k = std::min(n - i, L - pos_);
            const cf* t = &tab_[pos_];
            for (size_t j = 0; j < k; j++) y[i + j] = x[i + j] * t[j];
            i += k;
            pos_ = (pos_ + k) % L;
        }
    }
private:
    std::vector<cf> tab_;
    size_t pos_ = 0;
    Phasor rec_;
};

struct SoftBit {
    float v = 0;      // > 0: data bit 1; |v| = reliability
    double t = 0;     // MPX sample index of the bit's centre
};

class RdsDemod {
public:
    RdsDemod() = default;
    RdsDemod(const RdsDemod&) = delete;
    RdsDemod& operator=(const RdsDemod&) = delete;
    ~RdsDemod() {
        if (rs_) msresamp_crcf_destroy(rs_);
        if (dump_) fclose(dump_);
    }

    void init(double fs, double fc) {
        fs_ = fs;
        fc_ = fc;
        r_ = DEMOD_RATE / fs;
        if (rs_) msresamp_crcf_destroy(rs_);
        rs_ = msresamp_crcf_create(static_cast<float>(r_), 60.0f);
        rs_delay_ = measured_delay(r_);
        mf_.set_taps(kp::fir_rrc(SPS, MF_SPAN, 1.0f));
        reset();
    }
    double fc() const { return fc_; }

    void reset() {
        nco_.set(fc_, fs_);
        if (rs_) msresamp_crcf_reset(rs_);
        mf_.reset();
        y_.clear();
        ybase_ = 0;
        tau_ = SPS * MF_SPAN * 2 + 4;
        tf_ = 0;
        theta_ = cfreq_ = 0;
        amp_ = 0;
        k_ = 0;
        par_ = 0;
        epar_[0] = epar_[1] = 0;
        prev_ = cf{};
        prev_re_ = 0;
        have_b_ = false;
        prev_b_ = 0;
        sig_ = noi_ = 0;
        in_n_ = 0;
    }

    // n input samples (the real MPX, or a complex band with the subcarrier at
    // fc); x[0] is input sample index n0. Soft bits to *out, their times in
    // input sample indexes
    template <typename T>
    void push(const T* x, size_t n, int64_t n0, std::vector<SoftBit>* out) {
        if (!rs_ || n == 0) return;
        if (in_n_ == 0) in0_ = n0;   // the first sample this demodulator saw
        in_n_ += n;
        mix_.resize(n);
        nco_.mix(x, mix_.data(), n);
        rso_.resize(static_cast<size_t>(n * r_) + 16);
        unsigned int ny = 0;
        msresamp_crcf_execute(rs_, mix_.data(), static_cast<unsigned int>(n), rso_.data(), &ny);
        for (unsigned int i = 0; i < ny; i++) y_.push_back(mf_.push(rso_[i]));
        strobes(out);
        // keep a few half-symbols for the interpolator
        if (y_.size() > 4096) {
            const size_t drop = y_.size() - 64;
            y_.erase(y_.begin(), y_.begin() + static_cast<long>(drop));
            ybase_ += static_cast<int64_t>(drop);
        }
    }

    // half-symbol amplitude after the matched filter (MPX Hz scale: the
    // subcarrier's peak deviation is about amp() x INJ_PER_AMP)
    float amp() const { return amp_; }
    // strobe SNR (dB): mean |Re|^2 over the variance around it (Re and Im)
    float snr_db() const { return noi_ > 0 ? 10 * std::log10(std::max(1e-6f, sig_ / noi_)) : -99; }
    float carrier_hz() const { return static_cast<float>(cfreq_ * HALF_RATE / (2 * M_PI)); }
    FILE* dump_ = nullptr;   // debug: every strobe

private:
    double fs_ = 240000, fc_ = 57000, r_ = 1;
    Mixer nco_;
    msresamp_crcf rs_ = nullptr;
    float rs_delay_ = 0;
    kp::Fir<cf> mf_;
    std::vector<cf> mix_, rso_;
    std::vector<cf> y_;          // matched-filter output; y_[0] = global index ybase_
    int64_t ybase_ = 0;
    double tau_ = 0, tf_ = 0;    // next strobe (global MF index), timing loop rate term
    double theta_ = 0, cfreq_ = 0;
    float amp_ = 0;
    uint64_t k_ = 0;             // half-symbols so far
    int par_ = 0;
    float epar_[2] = {0, 0};
    cf prev_{};
    float prev_re_ = 0;
    bool have_b_ = false;
    float prev_b_ = 0;
    float sig_ = 0, noi_ = 0;
    int64_t in0_ = 0;
    uint64_t in_n_ = 0;

    // the resampler's delay in output samples, measured: a narrowband pulse
    // (a Gaussian of 0.15 ms, ~1 kHz wide) through a fresh copy, its centroid
    // at the output
    static float measured_delay(double r) {
        msresamp_crcf q = msresamp_crcf_create(static_cast<float>(r), 60.0f);
        const int n = static_cast<int>(4000 / r);
        const double c = 0.5 * n, sig = 0.15e-3 * DEMOD_RATE / r;   // input samples
        std::vector<cf> x(static_cast<size_t>(n)), y(static_cast<size_t>(n * r) + 64);
        for (int i = 0; i < n; i++) x[static_cast<size_t>(i)] = static_cast<float>(std::exp(-0.5 * std::pow((i - c) / sig, 2)));
        unsigned int ny = 0;
        msresamp_crcf_execute(q, x.data(), static_cast<unsigned int>(n), y.data(), &ny);
        msresamp_crcf_destroy(q);
        double s = 0, sw = 0;
        for (unsigned int i = 0; i < ny; i++) {
            const double a = std::norm(y[i]);
            s += a;
            sw += a * i;
        }
        return s > 0 ? static_cast<float>(sw / s - c * r) : 0.0f;
    }

    cf at(double t) const {   // cubic (Catmull-Rom) interpolation of y_
        const double rel = t - static_cast<double>(ybase_);
        const int64_t i = static_cast<int64_t>(std::floor(rel));
        const float m = static_cast<float>(rel - static_cast<double>(i));
        const cf a = y_[static_cast<size_t>(i - 1)], b = y_[static_cast<size_t>(i)],
                 c = y_[static_cast<size_t>(i + 1)], d = y_[static_cast<size_t>(i + 2)];
        const cf c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.0f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * m + c2) * m + c1) * m + b;
    }

    // MF output index -> MPX sample index
    double to_mpx(double t) const {
        const double rs_out = t - (mf_.delay()) - rs_delay_;
        return static_cast<double>(in0_) + rs_out / r_;
    }

    void strobes(std::vector<SoftBit>* out) {
        const double end = static_cast<double>(ybase_ + static_cast<int64_t>(y_.size()));
        while (tau_ + 3 < end) {
            if (tau_ - SPS / 2 - 2 < static_cast<double>(ybase_)) {   // fell behind the buffer (start)
                tau_ += SPS;
                continue;
            }
            const double tk = tau_;
            const cf ys = at(tk), ym = at(tk - SPS / 2.0);
            // Gardner timing error, phase-free; > 0 = sampling late
            const float a2 = std::max(amp_ * amp_, 1e-9f);
            float e = std::real((ys - prev_) * std::conj(ym)) / a2;
            e = std::clamp(e, -1.0f, 1.0f);
            tf_ -= 0.0004 * e;
            tf_ = std::clamp(tf_, -0.05, 0.05);
            tau_ += SPS * (1 + tf_) - 0.12 * e;
            // carrier: Costas on the strobe
            const cf r = ys * cf(static_cast<float>(std::cos(-theta_)), static_cast<float>(std::sin(-theta_)));
            const float mag = std::abs(r);
            amp_ = amp_ <= 0 ? mag : amp_ + 0.01f * (mag - amp_);
            float pe = (r.real() >= 0 ? r.imag() : -r.imag()) / std::max(amp_, 1e-9f);
            pe = std::clamp(pe, -1.0f, 1.0f);
            cfreq_ += 0.0008 * pe;
            cfreq_ = std::clamp(cfreq_, -0.06, 0.06);   // +-23 Hz
            theta_ = std::remainder(theta_ + cfreq_ + 0.04 * pe, 2 * M_PI);
            const float re = r.real();
            if (dump_) fprintf(dump_, "%.3f %.2f %.2f %.4f %.4f\n", tk, ys.real(), ys.imag(), theta_, e);
            const float dev = std::fabs(re) - amp_;
            sig_ += 0.005f * (re * re - sig_);
            noi_ += 0.005f * (dev * dev + r.imag() * r.imag() - noi_);
            // biphase: a bit is (first half - second half) / 2 at the right pairing
            const float d = 0.5f * (prev_re_ - re);
            epar_[k_ & 1] += std::fabs(d);
            if ((k_ & 127) == 127) {
                const int best = epar_[1] > epar_[0] ? 1 : 0;
                if (best != par_ && epar_[best] > 1.15f * epar_[par_]) {
                    par_ = best;
                    have_b_ = false;
                }
                epar_[0] = epar_[1] = 0;
            }
            if (static_cast<int>(k_ & 1) == par_ && k_ > 0) {
                if (have_b_) {
                    // differential decoding: a 1 = the symbol's sign changed
                    const float rel = std::min(std::fabs(d), std::fabs(prev_b_)) / std::max(amp_, 1e-9f);
                    SoftBit sb;
                    sb.v = (d * prev_b_ < 0 ? 1.0f : -1.0f) * std::min(rel, 2.0f);
                    sb.t = to_mpx(tk - SPS / 2.0);
                    out->push_back(sb);
                }
                prev_b_ = d;
                have_b_ = true;
            }
            prev_ = ys;
            prev_re_ = re;
            k_++;
        }
    }
};

}  // namespace rds
