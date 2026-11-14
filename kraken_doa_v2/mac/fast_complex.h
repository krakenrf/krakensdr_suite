#pragma once
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KrakenRF Inc. - MIT so that decoder plugins of any licence
// can link it (licence text: kraken_doa_v2/plugins/sdk/LICENSE, root LICENSE)
// std::complex<float / double> *, *= and std::norm without libc++'s C99 inf /
// NaN recovery - macOS's stand-in for GCC's -fcx-fortran-rules. libc++ writes
// the recovery into <complex> itself (a NaN test after every multiply, isinf
// tests in norm), so no flag short of -ffinite-math-only removes it and
// complex loops don't vectorize. Division stays libc++'s: its cost is the
// range scaling, not the checks.
// Plain overloads in libc++'s namespace, which overload resolution prefers to
// its templates on an exact match - not explicit specializations: libc++ 16+
// instantiates operator* inside <complex> (constexpr operator*=), and a later
// specialization doesn't compile. The arithmetic is libc++'s minus the
// recovery. Force-included into every C++ file of a macOS build (see the
// Makefiles); identical copy in heimdall_v2/mac and kraken_doa_v2/mac.
// Measurements and the alternatives weighed: docs/dev/macos.md.
#if defined(__APPLE__) && defined(__cplusplus)

#include <complex>

#if defined(_LIBCPP_VERSION)
#define KRAKEN_FAST_COMPLEX 1
_LIBCPP_BEGIN_NAMESPACE_STD

#define KRAKEN_FAST_COMPLEX_OPS(T)                                                    \
    constexpr complex<T> operator*(const complex<T>& z, const complex<T>& w) {        \
        const T a = z.real(), b = z.imag(), c = w.real(), d = w.imag();               \
        const T ac = a * c, bd = b * d, ad = a * d, bc = b * c;                       \
        return complex<T>(ac - bd, ad + bc);                                          \
    }                                                                                 \
    constexpr complex<T>& operator*=(complex<T>& z, const complex<T>& w) {            \
        return z = z * w;                                                             \
    }                                                                                 \
    constexpr T norm(const complex<T>& z) {                                           \
        return z.real() * z.real() + z.imag() * z.imag();                             \
    }
KRAKEN_FAST_COMPLEX_OPS(float)
KRAKEN_FAST_COMPLEX_OPS(double)
#undef KRAKEN_FAST_COMPLEX_OPS

_LIBCPP_END_NAMESPACE_STD
#endif

#endif
