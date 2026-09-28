#include "utils/iq_converter.hpp"

// Main conversion - optimized simple scalar version
// After extensive benchmarking (45 variants tested), this simple implementation
// is 8.5% faster than hand-optimized NEON and beats all library implementations.
// See opt_iqconv/COMPREHENSIVE_RESULTS.md for details.
void IQConverter::convert_uint8_to_complex_float(
    const uint8_t* input,
    std::complex<float>* output,
    size_t num_samples,
    bool dc_correction
) {
    if (!input || !output || num_samples == 0) {
        return;
    }

    const float off = 127.5f;
    const float scl = 1.0f / 127.5f;

    if (dc_correction) {
        // Two-pass DC correction: calculate mean, then convert with correction
        // Both passes auto-vectorize well

        // Pass 1: Calculate mean I and Q
        float sum_i = 0.0f;
        float sum_q = 0.0f;

        for (size_t i = 0; i < num_samples; i++) {
            sum_i += float(input[i*2]);
            sum_q += float(input[i*2+1]);
        }

        const float mean_i = sum_i / float(num_samples);
        const float mean_q = sum_q / float(num_samples);

        // Pass 2: Convert with DC correction
        for (size_t i = 0; i < num_samples; i++) {
            output[i] = {
                (float(input[i*2]) - mean_i) * scl,
                (float(input[i*2+1]) - mean_q) * scl
            };
        }
    } else {
        // Original fast path without DC correction
        for (size_t i = 0; i < num_samples; i++) {
            output[i] = {
                (float(input[i*2]) - off) * scl,
                (float(input[i*2+1]) - off) * scl
            };
        }
    }
}
