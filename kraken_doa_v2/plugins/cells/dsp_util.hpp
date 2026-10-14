#pragma once

// DSP helpers of the cells plugin: FFTW plans, frequency shifting

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

#include "kraken_plugin.hpp"

namespace cells {

using cf = kp::cf;

// In-place complex FFT of one size (FFTW_ESTIMATE: MEASURE plans take
// tens of seconds on a busy Pi). sign -1 = forward, +1 = backward (no scaling)
class Fft {
public:
    Fft(int n, int sign) : n_(n) {
        buf_ = reinterpret_cast<cf*>(fftwf_malloc(sizeof(fftwf_complex) * static_cast<size_t>(n)));
        plan_ = fftwf_plan_dft_1d(n, reinterpret_cast<fftwf_complex*>(buf_), reinterpret_cast<fftwf_complex*>(buf_),
                                  sign < 0 ? FFTW_FORWARD : FFTW_BACKWARD, FFTW_ESTIMATE);
    }
    ~Fft() {
        fftwf_destroy_plan(plan_);
        fftwf_free(buf_);
    }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
    int size() const { return n_; }
    cf* data() { return buf_; }
    void run() { fftwf_execute(plan_); }

private:
    int n_;
    cf* buf_;
    fftwf_plan plan_;
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

// Low-pass FIR for decimating by D: Blackman-windowed sinc, cut-off at half
// the output rate, unit gain at DC, 14 D + 1 taps - the transition runs
// from 0.3 to 0.7 x the output rate: flat and alias-free within +-0.3 x it,
// within ~1 dB and with aliases attenuated out to +-0.4 x it
inline std::vector<float> decim_taps(int D) {
    const int L = 14 * D + 1, c = L / 2;
    const double fc = 0.5 / D;   // cycles per input sample
    std::vector<float> h(static_cast<size_t>(L));
    double s = 0;
    for (int i = 0; i < L; i++) {
        const double t = i - c;
        const double sinc = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (L - 1)) + 0.08 * std::cos(4 * M_PI * i / (L - 1));
        h[static_cast<size_t>(i)] = static_cast<float>(sinc * w);
        s += sinc * w;
    }
    for (auto& v : h) v = static_cast<float>(v / s);
    return h;
}

// x low-pass filtered with h and every D-th sample kept, centred (no delay):
// y[i] = sum_k h[k] x[i D + k - L/2], zeros outside x - so y[i] is x's
// sample i D
inline void decimate(const cf* x, size_t n, int D, const std::vector<float>& h, std::vector<cf>* y) {
    const long L = static_cast<long>(h.size()), c = L / 2, N = static_cast<long>(n);
    const size_t no = (n + static_cast<size_t>(D) - 1) / static_cast<size_t>(D);
    y->resize(no);
    for (size_t i = 0; i < no; i++) {
        const long s0 = static_cast<long>(i) * D - c;
        const long k0 = std::max(0L, -s0), k1 = std::min(L, N - s0);
        float re = 0, im = 0;
        for (long k = k0; k < k1; k++) {
            const cf v = x[s0 + k];
            re += h[static_cast<size_t>(k)] * v.real();
            im += h[static_cast<size_t>(k)] * v.imag();
        }
        (*y)[i] = cf(re, im);
    }
}

}  // namespace cells
