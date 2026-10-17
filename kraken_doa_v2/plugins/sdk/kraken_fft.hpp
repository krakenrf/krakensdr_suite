#pragma once
// kraken_fft.hpp - the FFT of kraken_doa and of every decoder plugin: PFFFT
// (kraken_doa_v2/external/pffft, FFTPACK / BSD-style licence - it replaced
// FFTW, GPL) behind FFTW-like plans. Header-only; link pffft.c +
// pffft_common.c (kraken_doa's Makefile does; plugins get them in
// lib/build/libkrakendig.a).
//
//   kfft_complex* in  = kfft_alloc_complex(n);   // float[2] per sample, 64-byte aligned
//   kfft_complex* out = kfft_alloc_complex(n);
//   kfft_plan p = kfft_plan_dft_1d(n, in, out, KFFT_FORWARD);   // or KFFT_BACKWARD
//   ... fill in[i][0] (re) / in[i][1] (im) ...
//   kfft_execute(p);                             // out = DFT of in
//   kfft_destroy_plan(p); kfft_free(in); kfft_free(out);
//
// Results are FFTW's: forward X[k] = sum x[n] e^(-2 pi i k n / N), backward
// with +, neither scaled (backward(forward(x)) = N x), bins in natural order
// 0 .. N-1 (negative frequencies in the upper half). ANY N >= 1: pffft takes
// N = 2^a 3^b 5^c that are multiples of 16 itself (the direct path); every
// other N goes through Bluestein's algorithm on a pffft of a valid size M >=
// 2N - 1 (~3 transforms of M - prefer 2/3/5-smooth sizes; kfft_direct(n)
// says). in == out (in place) is fine. Buffers need no particular alignment:
// unaligned ones are copied through an aligned scratch (allocate with
// kfft_alloc_* to avoid the copy).
//
// kfft_plan_dft_r2c_1d(n, float* in, kfft_complex* out): n real samples ->
// n/2 + 1 bins, FFTW's r2c layout (pffft's real transform for multiples of 32
// with 2/3/5 factors, else a complex transform of the real input).
//
// kfft_execute_dft(p, in, out) / kfft_execute_dft_r2c(p, in, out): the plan
// on other arrays (FFTW's new-array execute).
//
// Plans are cheap to make (no measuring, no wisdom file) and may be made on
// any thread. ONE plan must not be executed by two threads at once (it owns
// its scratch) - give each thread its own, as with FFTW's bound arrays.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include "pffft.h"

typedef float kfft_complex[2];

enum { KFFT_FORWARD = -1, KFFT_BACKWARD = +1 };

struct kfft_plan_s;
typedef kfft_plan_s* kfft_plan;

inline void* kfft_malloc(size_t bytes) {
    void* p = pffft_aligned_malloc(bytes ? bytes : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
inline void kfft_free(void* p) {
    if (p) pffft_aligned_free(p);
}
inline kfft_complex* kfft_alloc_complex(size_t n) {
    return static_cast<kfft_complex*>(kfft_malloc(sizeof(kfft_complex) * n));
}
inline float* kfft_alloc_real(size_t n) {
    return static_cast<float*>(kfft_malloc(sizeof(float) * n));
}

// pffft transforms n complex points directly (no Bluestein)
inline bool kfft_direct(int n) { return n > 0 && pffft_is_valid_size(n, PFFFT_COMPLEX); }

struct kfft_plan_s {
    int n = 0;
    int sign = KFFT_FORWARD;
    bool r2c = false;
    const float* in = nullptr;      // bound arrays (kfft_execute)
    float* out = nullptr;
    // direct
    PFFFT_Setup* setup = nullptr;   // complex n, or real n (r2c)
    float* work = nullptr;
    float* tmp = nullptr;           // aligned copy of in / out (complex) or the r2c output
    // Bluestein (complex sizes pffft doesn't take)
    int m = 0;
    PFFFT_Setup* bsetup = nullptr;
    float* chirp = nullptr;         // n complex: e^(sign i pi k^2 / n)
    float* bfilt = nullptr;         // m complex, pffft's z-domain: FFT of conj(chirp) on +-k
    float* bbuf = nullptr;          // m complex
    float* bwork = nullptr;         // m complex
    // r2c of a size pffft's real transform doesn't take: a complex plan
    kfft_plan cplx = nullptr;
    float* cin = nullptr;           // 2n
    float* cout = nullptr;          // 2n
};

inline void kfft_destroy_plan(kfft_plan p);

namespace kfft_detail {

inline bool aligned(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 15u) == 0; }

inline float* falloc(size_t n) { return static_cast<float*>(kfft_malloc(sizeof(float) * n)); }

inline PFFFT_Setup* new_setup(int n, pffft_transform_t t) {
    PFFFT_Setup* s = pffft_new_setup(n, t);
    if (!s) throw std::bad_alloc();
    return s;
}

// a complex plan of n points without bound arrays
inline kfft_plan make_complex(int n, int sign) {
    kfft_plan p = new kfft_plan_s;
    p->n = n;
    p->sign = sign < 0 ? KFFT_FORWARD : KFFT_BACKWARD;
    try {
        if (kfft_direct(n)) {
            p->setup = new_setup(n, PFFFT_COMPLEX);
            p->work = falloc(2 * static_cast<size_t>(n));
            p->tmp = falloc(2 * static_cast<size_t>(n));
        } else {
            const int m = pffft_nearest_transform_size(2 * n - 1, PFFFT_COMPLEX, 1);
            p->m = m;
            p->bsetup = new_setup(m, PFFFT_COMPLEX);
            p->chirp = falloc(2 * static_cast<size_t>(n));
            p->bfilt = falloc(2 * static_cast<size_t>(m));
            p->bbuf = falloc(2 * static_cast<size_t>(m));
            p->bwork = falloc(2 * static_cast<size_t>(m));
            // chirp[k] = e^(sign i pi k^2 / n), the angle from k^2 mod 2n
            // (exact in integers: no precision lost for large k)
            const uint64_t two_n = 2 * static_cast<uint64_t>(n);
            for (int k = 0; k < n; k++) {
                const uint64_t r = (static_cast<uint64_t>(k) * static_cast<uint64_t>(k)) % two_n;
                const double a = p->sign * 3.14159265358979323846 * static_cast<double>(r) / n;
                p->chirp[2 * k] = static_cast<float>(std::cos(a));
                p->chirp[2 * k + 1] = static_cast<float>(std::sin(a));
            }
            // b[j] = conj(chirp[|j|]) for -(n-1) <= j <= n-1, circular in m
            std::memset(p->bbuf, 0, sizeof(float) * 2 * static_cast<size_t>(m));
            for (int k = 0; k < n; k++) {
                const float re = p->chirp[2 * k], im = -p->chirp[2 * k + 1];
                p->bbuf[2 * k] = re;
                p->bbuf[2 * k + 1] = im;
                if (k > 0) {
                    p->bbuf[2 * (m - k)] = re;
                    p->bbuf[2 * (m - k) + 1] = im;
                }
            }
            pffft_transform(p->bsetup, p->bbuf, p->bfilt, p->bwork, PFFFT_FORWARD);
        }
    } catch (...) {
        kfft_destroy_plan(p);
        throw;
    }
    return p;
}

// out = DFT(in) for a complex plan; in / out may alias, any alignment
inline void run_complex(kfft_plan p, const float* in, float* out) {
    const size_t n2 = 2 * static_cast<size_t>(p->n);
    if (p->setup) {
        const pffft_direction_t dir = p->sign < 0 ? PFFFT_FORWARD : PFFFT_BACKWARD;
        if (aligned(in) && aligned(out)) {
            pffft_transform_ordered(p->setup, in, out, p->work, dir);
        } else {
            std::memcpy(p->tmp, in, sizeof(float) * n2);
            pffft_transform_ordered(p->setup, p->tmp, p->tmp, p->work, dir);
            std::memcpy(out, p->tmp, sizeof(float) * n2);
        }
        return;
    }
    // Bluestein: X[k] = chirp[k] * sum_j (x chirp)[j] conj(chirp)[k - j]
    const int n = p->n, m = p->m;
    float* a = p->bbuf;
    const float* w = p->chirp;
    for (int k = 0; k < n; k++) {
        const float xr = in[2 * k], xi = in[2 * k + 1];
        const float wr = w[2 * k], wi = w[2 * k + 1];
        a[2 * k] = xr * wr - xi * wi;
        a[2 * k + 1] = xr * wi + xi * wr;
    }
    std::memset(a + 2 * static_cast<size_t>(n), 0, sizeof(float) * 2 * static_cast<size_t>(m - n));
    pffft_transform(p->bsetup, a, a, p->bwork, PFFFT_FORWARD);
    pffft_zconvolve_scale(p->bsetup, a, p->bfilt, a, 1.0f / static_cast<float>(m));
    pffft_transform(p->bsetup, a, a, p->bwork, PFFFT_BACKWARD);
    for (int k = 0; k < n; k++) {
        const float yr = a[2 * k], yi = a[2 * k + 1];
        const float wr = w[2 * k], wi = w[2 * k + 1];
        out[2 * k] = yr * wr - yi * wi;
        out[2 * k + 1] = yr * wi + yi * wr;
    }
}

// n real -> n/2 + 1 complex (FFTW's r2c layout)
inline void run_r2c(kfft_plan p, const float* in, float* out) {
    const int n = p->n;
    if (p->setup) {
        if (aligned(in)) {
            pffft_transform_ordered(p->setup, in, p->tmp, p->work, PFFFT_FORWARD);
        } else {
            std::memcpy(p->tmp, in, sizeof(float) * static_cast<size_t>(n));
            pffft_transform_ordered(p->setup, p->tmp, p->tmp, p->work, PFFFT_FORWARD);
        }
        // pffft's ordered real output: F(0), F(n/2), then re / im of F(1 .. n/2 - 1)
        const float* t = p->tmp;
        const float dc = t[0], nyq = t[1];
        for (int k = 1; k < n / 2; k++) {
            out[2 * k] = t[2 * k];
            out[2 * k + 1] = t[2 * k + 1];
        }
        out[0] = dc;
        out[1] = 0.0f;
        out[2 * (n / 2)] = nyq;
        out[2 * (n / 2) + 1] = 0.0f;
        return;
    }
    for (int i = 0; i < n; i++) {
        p->cin[2 * i] = in[i];
        p->cin[2 * i + 1] = 0.0f;
    }
    run_complex(p->cplx, p->cin, p->cout);
    std::memcpy(out, p->cout, sizeof(float) * 2 * static_cast<size_t>(n / 2 + 1));
}

}  // namespace kfft_detail

inline kfft_plan kfft_plan_dft_1d(int n, kfft_complex* in, kfft_complex* out, int sign) {
    if (n < 1) return nullptr;
    kfft_plan p = kfft_detail::make_complex(n, sign);
    p->in = reinterpret_cast<const float*>(in);
    p->out = reinterpret_cast<float*>(out);
    return p;
}

inline kfft_plan kfft_plan_dft_r2c_1d(int n, float* in, kfft_complex* out) {
    if (n < 1) return nullptr;
    kfft_plan p = new kfft_plan_s;
    p->n = n;
    p->r2c = true;
    p->in = in;
    p->out = reinterpret_cast<float*>(out);
    try {
        if (pffft_is_valid_size(n, PFFFT_REAL)) {
            p->setup = kfft_detail::new_setup(n, PFFFT_REAL);
            p->work = kfft_detail::falloc(static_cast<size_t>(n));
            p->tmp = kfft_detail::falloc(static_cast<size_t>(n));
        } else {
            p->cplx = kfft_detail::make_complex(n, KFFT_FORWARD);
            p->cin = kfft_detail::falloc(2 * static_cast<size_t>(n));
            p->cout = kfft_detail::falloc(2 * static_cast<size_t>(n));
        }
    } catch (...) {
        kfft_destroy_plan(p);
        throw;
    }
    return p;
}

inline void kfft_execute_dft(kfft_plan p, const kfft_complex* in, kfft_complex* out) {
    if (!p || p->r2c) return;
    kfft_detail::run_complex(p, reinterpret_cast<const float*>(in), reinterpret_cast<float*>(out));
}

inline void kfft_execute_dft_r2c(kfft_plan p, const float* in, kfft_complex* out) {
    if (!p || !p->r2c) return;
    kfft_detail::run_r2c(p, in, reinterpret_cast<float*>(out));
}

inline void kfft_execute(kfft_plan p) {
    if (!p) return;
    if (p->r2c) kfft_detail::run_r2c(p, p->in, p->out);
    else kfft_detail::run_complex(p, p->in, p->out);
}

inline void kfft_destroy_plan(kfft_plan p) {
    if (!p) return;
    if (p->setup) pffft_destroy_setup(p->setup);
    if (p->bsetup) pffft_destroy_setup(p->bsetup);
    kfft_free(p->work);
    kfft_free(p->tmp);
    kfft_free(p->chirp);
    kfft_free(p->bfilt);
    kfft_free(p->bbuf);
    kfft_free(p->bwork);
    kfft_destroy_plan(p->cplx);
    kfft_free(p->cin);
    kfft_free(p->cout);
    delete p;
}

// The SIMD pffft was built with ("NEON", "SSE1", "scalar" ...) - for logs
inline const char* kfft_simd() { return pffft_simd_arch(); }
