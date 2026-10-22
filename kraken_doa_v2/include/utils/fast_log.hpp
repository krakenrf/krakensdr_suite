#pragma once
// A log2 / dB the compiler can vectorize, for the spectra's per-bin power ->
// dB conversion (fft_processor.cpp, beamformer.cpp). The spectrum's loop
// (16384 bins per frame, ~146 frames a second) used std::log2f with the
// DC-centring index wrapped by a mask: no vectorization, a libm call per bin
// - 8.5 ns a bin on the Pi 5, ~29 % of kraken_doa's CPU with no VFO
// decimated (~2.6 % of a core per tuner FFT'd; perf, 2026-10-22). As straight loops, glibc vectorizes
// std::log2f itself (2.5 ns a bin on the Pi), MinGW doesn't (2.6 ns on the
// dev PC's Ryzen, scalar); this one vectorizes everywhere: 2.7 ns on the Pi,
// 0.39 ns on the Ryzen - one implementation, the same values on every
// platform (tools/fast_log_test.cpp).
//
// x = 2^e m: the exponent from the float's bits, the mantissa m brought into
// [sqrt(1/2), sqrt(2)), ln(m) = 2 atanh(t) with t = (m - 1) / (m + 1), |t| <=
// 0.1716, to t^7 (the next term is < 3e-8). Max error vs std::log2 2e-6
// (6e-6 dB; ~5 ulps of a result near -44, std::log2f's own ~1 ulp), against
// the display's 0.47 dB steps. Only for normal, positive x: the callers add
// a floor (1e-15) to the power.

#include <bit>
#include <cstdint>

inline float fast_log2(float x) {
    const uint32_t b = std::bit_cast<uint32_t>(x);
    const float m1 = std::bit_cast<float>((b & 0x007FFFFFu) | 0x3F800000u);   // [1, 2)
    const bool hi = m1 > 1.41421356f;
    const float m = hi ? m1 * 0.5f : m1;
    const float e = static_cast<float>(static_cast<int>(b >> 23) - 127) + (hi ? 1.0f : 0.0f);
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float ln_m = t * (2.0f + t2 * (2.0f / 3.0f + t2 * (2.0f / 5.0f + t2 * (2.0f / 7.0f))));
    return e + ln_m * 1.44269504088896341f;   // ln -> log2
}

// 10 log10(power)
inline float fast_db10(float power) {
    return 3.01029995663981195f * fast_log2(power);
}
