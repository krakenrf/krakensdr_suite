#pragma once

// sigclass internals: aligned buffers, pffft plans, small numeric helpers.
// Not a public header (include/sigclass/sigclass.hpp is).

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#if defined(_WIN32) && !defined(PFFFT_STATIC_DEFINE)
#define PFFFT_STATIC_DEFINE
#endif
#include "pffft.h"

namespace sigclass::detail {

using cf = std::complex<float>;

// std::vector storage from pffft_aligned_malloc (SIMD-aligned, as pffft wants)
template <class T>
struct PfAlloc {
    using value_type = T;
    PfAlloc() = default;
    template <class U>
    PfAlloc(const PfAlloc<U>&) {}
    T* allocate(size_t n) {
        void* p = pffft_aligned_malloc(n ? n * sizeof(T) : 1);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { pffft_aligned_free(p); }
    template <class U>
    bool operator==(const PfAlloc<U>&) const { return true; }
};
template <class T>
using avec = std::vector<T, PfAlloc<T>>;

// One pffft size + direction-agnostic plan with its own work buffer. Complex
// transforms are in natural order (pffft_transform_ordered); a real forward
// transform of n gives n floats: [DC, Nyquist, re1, im1, ..., re(n/2-1), im(n/2-1)].
class Fft {
public:
    Fft() = default;
    Fft(int n, bool real) { init(n, real); }
    ~Fft() { reset(); }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;
    Fft(Fft&& o) noexcept { *this = std::move(o); }
    Fft& operator=(Fft&& o) noexcept {
        if (this != &o) {
            reset();
            s_ = o.s_; n_ = o.n_; real_ = o.real_; work_ = std::move(o.work_);
            o.s_ = nullptr; o.n_ = 0;
        }
        return *this;
    }
    bool init(int n, bool real) {
        if (s_ && n == n_ && real == real_) return true;
        reset();
        if (n <= 0 || !pffft_is_valid_size(n, real ? PFFFT_REAL : PFFFT_COMPLEX)) return false;
        s_ = pffft_new_setup(n, real ? PFFFT_REAL : PFFFT_COMPLEX);
        if (!s_) return false;
        n_ = n;
        real_ = real;
        work_.assign(size_t(real ? n : 2 * n), 0.0f);
        return true;
    }
    int size() const { return n_; }
    bool valid() const { return s_ != nullptr; }
    void fwd(const cf* in, cf* out) {
        pffft_transform_ordered(s_, reinterpret_cast<const float*>(in), reinterpret_cast<float*>(out),
                                work_.data(), PFFFT_FORWARD);
    }
    void inv(const cf* in, cf* out) {
        pffft_transform_ordered(s_, reinterpret_cast<const float*>(in), reinterpret_cast<float*>(out),
                                work_.data(), PFFFT_BACKWARD);
    }
    void fwd_real(const float* in, float* out) {
        pffft_transform_ordered(s_, in, out, work_.data(), PFFFT_FORWARD);
    }
    void inv_real(const float* in, float* out) {
        pffft_transform_ordered(s_, in, out, work_.data(), PFFFT_BACKWARD);
    }

private:
    void reset() {
        if (s_) pffft_destroy_setup(s_);
        s_ = nullptr;
        n_ = 0;
    }
    PFFFT_Setup* s_ = nullptr;
    int n_ = 0;
    bool real_ = false;
    avec<float> work_;
};

inline bool finite_f(float v) {
    return (std::bit_cast<uint32_t>(v) & 0x7F800000u) != 0x7F800000u;
}
inline bool finite_d(double v) {
    return (std::bit_cast<uint64_t>(v) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}
// clamp, mapping NaN / inf to lo (NaN fails both comparisons)
inline float clampf(float v, float lo, float hi) {
    if (!finite_f(v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}
inline float db10(double x) { return float(10.0 * std::log10(std::max(x, 1e-30))); }

// atan2 from Abramowitz & Stegun 4.4.49 (|error| <= 1e-5 rad), deterministic
inline float fast_atan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx <= 0.0f) return 0.0f;
    const float a = mn / mx;
    const float s = a * a;
    float r = a * (0.9998660f + s * (-0.3302995f + s * (0.1801410f + s * (-0.0851330f + s * 0.0208351f))));
    if (ay > ax) r = 1.57079632679f - r;
    if (x < 0.0f) r = 3.14159265359f - r;
    return y < 0.0f ? -r : r;
}

// q-quantile (0..1) of v (a copy is partially sorted in scratch)
inline float quantile(const float* v, size_t n, double q, std::vector<float>& scratch) {
    if (n == 0) return 0.0f;
    scratch.assign(v, v + n);
    size_t k = size_t(std::clamp(q, 0.0, 1.0) * double(n - 1) + 0.5);
    std::nth_element(scratch.begin(), scratch.begin() + k, scratch.end());
    return scratch[k];
}

// Regularised upper incomplete gamma Q(a, x) (series below a + 1, Legendre's
// continued fraction above it, evaluated with the modified Lentz method)
inline double gamma_q(double a, double x) {
    if (x <= 0.0) return 1.0;
    const double lg = std::lgamma(a);
    if (x < a + 1.0) {
        double sum = 1.0 / a, term = sum, ap = a;
        for (int i = 0; i < 500; i++) {
            ap += 1.0;
            term *= x / ap;
            sum += term;
            if (std::fabs(term) < std::fabs(sum) * 1e-14) break;
        }
        const double p = sum * std::exp(-x + a * std::log(x) - lg);
        return std::clamp(1.0 - p, 0.0, 1.0);
    }
    const double tiny = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    for (int i = 1; i < 500; i++) {
        const double an = -double(i) * (double(i) - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < tiny) d = tiny;
        c = b + an / c;
        if (std::fabs(c) < tiny) c = tiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < 1e-14) break;
    }
    return std::clamp(std::exp(-x + a * std::log(x) - lg) * h, 0.0, 1.0);
}

// t such that P(G > t) = p for G ~ Gamma(shape kappa) with mean 1 (the
// threshold on an average of kappa exponential powers, relative to its mean)
inline double gamma_threshold(double kappa, double p) {
    kappa = std::max(kappa, 0.05);
    p = std::clamp(p, 1e-300, 0.5);
    double lo = 1.0, hi = 2.0;
    while (gamma_q(kappa, kappa * hi) > p && hi < 1e6) hi *= 2.0;
    for (int i = 0; i < 80; i++) {
        const double mid = 0.5 * (lo + hi);
        if (gamma_q(kappa, kappa * mid) > p) lo = mid; else hi = mid;
    }
    return hi;
}

}  // namespace sigclass::detail
