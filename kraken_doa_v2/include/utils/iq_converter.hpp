#pragma once

#include <complex>
#include <cstdint>
#include <cstddef>

// Optimized IQ data converter
// Uses simple scalar implementation which is 8.5% faster than hand-optimized SIMD
// and beats all library implementations (tested 45 variants).
// See opt_iqconv/COMPREHENSIVE_RESULTS.md for detailed benchmarking results.
class IQConverter {
public:
    // Convert uint8 IQ data to complex float
    // Input: interleaved I/Q bytes (I0, Q0, I1, Q1, ...)
    // Output: complex float samples normalized to [-1.0, 1.0]
    // Formula: normalized = (uint8_value - 127.5) / 127.5
    // DC correction: If enabled, calculates mean I/Q and subtracts to remove center spike
    static void convert_uint8_to_complex_float(
        const uint8_t* input,
        std::complex<float>* output,
        size_t num_samples,
        bool dc_correction = true
    );

    // DC correction with a slowly tracked offset: each packet's mean only
    // nudges `dc_i` / `dc_q` (caller-owned, per channel; `seeded` false = the
    // first packet's mean seeds them - a flag, since -Ofast assumes no NaNs)
    // by `k`. Subtracting each packet's OWN mean (above) was a notch about one
    // packet rate wide (~150 Hz at 16384 samples / 2.4 MSPS) that removed a CW
    // beacon or AM carrier parked near the centre; k = 0.05 per ~7 ms packet
    // puts the corner near 1 Hz while still following the RTL DC offset.
    static void convert_uint8_to_complex_float_tracked_dc(
        const uint8_t* input,
        std::complex<float>* output,
        size_t num_samples,
        float& dc_i,
        float& dc_q,
        bool& seeded,
        float k = 0.05f
    );
};
