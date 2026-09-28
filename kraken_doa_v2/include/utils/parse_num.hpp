#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

// Finiteness from the IEEE-754 exponent bits (all ones = inf or NaN). The
// client builds with -Ofast, which implies -ffinite-math-only: the compiler
// then assumes inf/NaN never occur and folds std::isfinite()/std::isnan() to
// constants, so they cannot be used to reject hostile input here. Integer
// tests on the bit pattern are not affected by fast-math.
inline bool is_finite_value(float v) {
    return (std::bit_cast<uint32_t>(v) & 0x7F800000u) != 0x7F800000u;
}
inline bool is_finite_value(double v) {
    return (std::bit_cast<uint64_t>(v) & 0x7FF0000000000000ull) != 0x7FF0000000000000ull;
}

// std::stof / std::stod that also reject inf and nan. Both parse "inf" and
// "nan" without throwing, and a non-finite value from a control message or a
// config file breaks everything downstream of it (angle wraps that never
// terminate, band plans that never end, invalid JSON in the settings file).
// Every caller already treats a throw as "ignore the malformed value".
inline float stof_finite(const std::string& s) {
    const float v = std::stof(s);
    if (!is_finite_value(v)) throw std::invalid_argument("non-finite number: " + s);
    return v;
}

inline double stod_finite(const std::string& s) {
    const double v = std::stod(s);
    if (!is_finite_value(v)) throw std::invalid_argument("non-finite number: " + s);
    return v;
}

// Wrap an angle into [0, 360) in O(1), however large it is (a subtract-360
// loop never terminates once 360 falls below the float's precision).
// Non-finite input maps to 0.
inline float wrap_degrees(float deg) {
    if (!is_finite_value(deg)) return 0.0f;
    float w = std::fmod(deg, 360.0f);
    if (w < 0.0f) w += 360.0f;
    return (w >= 360.0f) ? 0.0f : w;  // -tiny + 360 can round up to 360
}
