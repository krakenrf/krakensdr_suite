#pragma once

// DSP helpers of the SFN plugin: FFT plans (kraken_fft.hpp), an arbitrary-ratio resampler,
// frequency shifting

#include "kraken_fft.hpp"

#include <cmath>
#include <complex>
#include <map>
#include <memory>
#include <vector>

#include "kraken_plugin.hpp"

namespace sfn {

using cf = kp::cf;

// In-place complex FFT of one size (any size: kraken_fft.hpp runs the ones
// pffft can't do directly through Bluestein). sign -1 = forward, +1 =
// backward (no scaling)
class Fft {
public:
    Fft(int n, int sign) : n_(n) {
        buf_ = reinterpret_cast<cf*>(kfft_malloc(sizeof(kfft_complex) * static_cast<size_t>(n)));
        plan_ = kfft_plan_dft_1d(n, reinterpret_cast<kfft_complex*>(buf_), reinterpret_cast<kfft_complex*>(buf_),
                                 sign < 0 ? KFFT_FORWARD : KFFT_BACKWARD);
    }
    ~Fft() {
        kfft_destroy_plan(plan_);
        kfft_free(buf_);
    }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
    int size() const { return n_; }
    cf* data() { return buf_; }
    void run() { kfft_execute(plan_); }

private:
    int n_;
    cf* buf_;
    kfft_plan plan_;
};

// FFT plans by (size, sign), created on first use
class FftCache {
public:
    Fft& get(int n, int sign) {
        auto& p = plans_[{n, sign}];
        if (!p) p = std::make_unique<Fft>(n, sign);
        return *p;
    }

private:
    std::map<std::pair<int, int>, std::unique_ptr<Fft>> plans_;
};

// x * exp(j 2 pi f n / fs), phase continuous from phase0 (radians, updated)
inline void shift_freq(cf* x, size_t n, double f, double fs, double* phase0) {
    const double w = 2 * M_PI * f / fs;
    double ph = *phase0;
    // recompute the exact phasor every 1024 samples (no drift)
    for (size_t i = 0; i < n; i += 1024) {
        const size_t k = std::min<size_t>(1024, n - i);
        cf rot = std::polar(1.0f, static_cast<float>(ph));
        const cf step = std::polar(1.0f, static_cast<float>(w));
        for (size_t j = 0; j < k; j++) {
            const float re = x[i + j].real() * rot.real() - x[i + j].imag() * rot.imag();
            const float im = x[i + j].real() * rot.imag() + x[i + j].imag() * rot.real();
            x[i + j] = cf(re, im);
            const float rr = rot.real() * step.real() - rot.imag() * step.imag();
            const float ri = rot.real() * step.imag() + rot.imag() * step.real();
            rot = cf(rr, ri);
        }
        ph = std::remainder(ph + w * static_cast<double>(k), 2 * M_PI);
    }
    *phase0 = ph;
}

// Arbitrary-ratio resampler (windowed sinc, polyphase table): out[n] =
// x(n * in_rate / out_rate) - a whole block at once (no state: the SFN
// estimators work burst by burst). Low-pass at 0.45 x the lower rate.
class Resampler {
public:
    Resampler(double in_rate, double out_rate) : step_(in_rate / out_rate) {
        const double fc = 0.45 * std::min(1.0, 1.0 / step_);   // cycles per input sample
        table_.resize(static_cast<size_t>(PHASES + 1) * TAPS);
        for (int ph = 0; ph <= PHASES; ph++) {
            const double frac = static_cast<double>(ph) / PHASES;
            double sum = 0;
            for (int k = 0; k < TAPS; k++) {
                const double x = k - (TAPS / 2 - 1) - frac;   // distance from the output point
                const double s = std::fabs(x) < 1e-12 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
                const double wpos = (x + TAPS / 2.0) / TAPS;   // 0..1 across the taps
                const double w = 0.42 - 0.5 * std::cos(2 * M_PI * wpos) + 0.08 * std::cos(4 * M_PI * wpos);   // Blackman
                table_[static_cast<size_t>(ph * TAPS + k)] = static_cast<float>(s * w);
                sum += s * w;
            }
            for (int k = 0; k < TAPS; k++) table_[static_cast<size_t>(ph * TAPS + k)] /= static_cast<float>(sum);
        }
    }
    // resample x[0..n) -> out (output sample 0 = input sample 0)
    void run(const cf* x, size_t n, std::vector<cf>* out) const {
        const size_t nout = static_cast<size_t>(std::floor((static_cast<double>(n) - 1) / step_)) + 1;
        out->resize(nout);
        cf* o = out->data();
        size_t i = 0;
        // interior outputs four at a time: eight independent sums (one output's
        // re / im were two serial chains of 24 multiply-adds - ~1.4x faster),
        // each over the taps in the same order as one at a time
        for (; i + 4 <= nout; i += 4) {
            long k0[4];
            const float* h[4];
            for (int q = 0; q < 4; q++) tap(i + static_cast<size_t>(q), &k0[q], &h[q]);
            if (k0[0] < 0 || k0[3] + TAPS > static_cast<long>(n)) {
                for (int q = 0; q < 4; q++) o[i + static_cast<size_t>(q)] = one(x, n, k0[q], h[q]);
                continue;
            }
            const float* x0 = reinterpret_cast<const float*>(x + k0[0]);
            const float* x1 = reinterpret_cast<const float*>(x + k0[1]);
            const float* x2 = reinterpret_cast<const float*>(x + k0[2]);
            const float* x3 = reinterpret_cast<const float*>(x + k0[3]);
            float r0 = 0, m0 = 0, r1 = 0, m1 = 0, r2 = 0, m2 = 0, r3 = 0, m3 = 0;
            for (int k = 0; k < TAPS; k++) {
                r0 += x0[2 * k] * h[0][k];
                m0 += x0[2 * k + 1] * h[0][k];
                r1 += x1[2 * k] * h[1][k];
                m1 += x1[2 * k + 1] * h[1][k];
                r2 += x2[2 * k] * h[2][k];
                m2 += x2[2 * k + 1] * h[2][k];
                r3 += x3[2 * k] * h[3][k];
                m3 += x3[2 * k + 1] * h[3][k];
            }
            o[i] = cf(r0, m0);
            o[i + 1] = cf(r1, m1);
            o[i + 2] = cf(r2, m2);
            o[i + 3] = cf(r3, m3);
        }
        for (; i < nout; i++) {
            long k0;
            const float* h;
            tap(i, &k0, &h);
            o[i] = one(x, n, k0, h);
        }
    }
    double step() const { return step_; }

private:
    static constexpr int PHASES = 256, TAPS = 24;
    // output i: its first input sample and its phase's taps
    void tap(size_t i, long* k0, const float** h) const {
        const double p = static_cast<double>(i) * step_;
        const long ip = static_cast<long>(std::floor(p));
        const double frac = p - static_cast<double>(ip);
        // lround(frac PHASES) (>= 0: half away from zero) without the libm
        // call; x - its integer part is exact
        const double xr = frac * PHASES;
        const long xi = static_cast<long>(xr);
        const int ph = static_cast<int>(xi + (xr - static_cast<double>(xi) >= 0.5 ? 1 : 0));
        *h = table_.data() + static_cast<size_t>(ph) * TAPS;
        *k0 = ip - (TAPS / 2 - 1);
    }
    static cf one(const cf* x, size_t n, long k0, const float* h) {
        float re = 0, im = 0;
        if (k0 >= 0 && k0 + TAPS <= static_cast<long>(n)) {
            const float* xf = reinterpret_cast<const float*>(x + k0);
            for (int k = 0; k < TAPS; k++) {
                re += xf[2 * k] * h[k];
                im += xf[2 * k + 1] * h[k];
            }
        } else {
            for (int k = 0; k < TAPS; k++) {
                const long j = k0 + k;
                if (j < 0 || j >= static_cast<long>(n)) continue;
                re += x[j].real() * h[k];
                im += x[j].imag() * h[k];
            }
        }
        return cf(re, im);
    }
    double step_;
    std::vector<float> table_;
};

}  // namespace sfn
