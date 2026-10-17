#include "fft_plan.hpp"
#include "../core/config.hpp"
#include "pffft/pffft.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// pffft (external/pffft, BSD-style; replaced FFTW 2026-10-17): CORRELATION_SIZE
// = 2 x NUM_SAMPLES is a power of two, a size pffft takes as it is.
// pffft_transform_ordered gives FFTW's results: natural bin order, unscaled
// in both directions, forward e^-
static_assert(CORRELATION_SIZE >= 16 && (CORRELATION_SIZE & (CORRELATION_SIZE - 1)) == 0,
              "pffft: CORRELATION_SIZE must be a power of two >= 16");

// pffft's scratch (2 x CORRELATION_SIZE floats), one per thread: FftPlans are
// used by one thread at a time (FftPool), and a transform is over before the
// next one starts
static float* thread_work() {
    struct Work {
        float* p = nullptr;
        ~Work() { if (p) pffft_aligned_free(p); }
    };
    static thread_local Work w;
    if (!w.p) {
        w.p = static_cast<float*>(pffft_aligned_malloc(CORRELATION_SIZE * sizeof(std::complex<float>)));
        if (!w.p) {
            std::fprintf(stderr, "FftPlan: out of memory\n");
            std::abort();
        }
    }
    return w.p;
}

static std::complex<float>* alloc_complex() {
    void* p = pffft_aligned_malloc(CORRELATION_SIZE * sizeof(std::complex<float>));
    if (!p) {
        std::fprintf(stderr, "FftPlan: out of memory\n");
        std::abort();
    }
    std::memset(p, 0, CORRELATION_SIZE * sizeof(std::complex<float>));
    return static_cast<std::complex<float>*>(p);
}

FftPlan::FftPlan() {
    setup = pffft_new_setup(CORRELATION_SIZE, PFFFT_COMPLEX);
    if (!setup) {
        std::fprintf(stderr, "FftPlan: pffft cannot do %d points\n", CORRELATION_SIZE);
        std::abort();
    }
    output = alloc_complex();
    correlation_freq = alloc_complex();
    correlation_time = alloc_complex();
}

FftPlan::~FftPlan() {
    if (setup) pffft_destroy_setup(setup);
    for (auto* p : {output, correlation_freq, correlation_time}) {
        if (p) pffft_aligned_free(p);
    }
}

// zero-padded copy straight into `output`, transformed in place (pffft
// allows it): only the zeros outside the copied range are written
void FftPlan::forward(const ComplexBuffer& data, int offset) {
    int samples_to_copy = std::min(static_cast<int>(data.size()), NUM_SAMPLES);
    offset = std::clamp(offset, 0, CORRELATION_SIZE - samples_to_copy);
    std::fill_n(output, offset, std::complex<float>{});
    std::copy_n(data.data(), samples_to_copy, output + offset);
    std::fill_n(output + offset + samples_to_copy, CORRELATION_SIZE - offset - samples_to_copy,
                std::complex<float>{});
    pffft_transform_ordered(setup, reinterpret_cast<const float*>(output), reinterpret_cast<float*>(output),
                            thread_work(), PFFFT_FORWARD);
}

std::complex<float>* FftPlan::get_output() {
    return output;
}

std::complex<float>* FftPlan::get_correlation_freq() {
    return correlation_freq;
}

std::complex<float>* FftPlan::get_correlation_time() {
    return correlation_time;
}

void FftPlan::cross_correlate(const std::complex<float>* ref_fft, const std::complex<float>* sig_fft) {
    auto* freq = get_correlation_freq();
    for (int i = 0; i < CORRELATION_SIZE; i++) {
        freq[i] = std::conj(ref_fft[i]) * sig_fft[i];
    }
    pffft_transform_ordered(setup, reinterpret_cast<const float*>(correlation_freq),
                            reinterpret_cast<float*>(correlation_time), thread_work(), PFFFT_BACKWARD);
}

std::complex<float>* FftPlan::inverse_transform(const std::complex<float>* freq_in) {
    auto* freq = get_correlation_freq();
    std::memcpy(freq, freq_in, CORRELATION_SIZE * sizeof(std::complex<float>));
    pffft_transform_ordered(setup, reinterpret_cast<const float*>(correlation_freq),
                            reinterpret_cast<float*>(correlation_time), thread_work(), PFFFT_BACKWARD);
    return get_correlation_time();
}

FftPool::FftPool(int size) {
    for (int i = 0; i < size; i++) {
        auto plan = std::make_unique<FftPlan>();
        available.enqueue(plan.get());
        plans.push_back(std::move(plan));
    }
}

FftPlan* FftPool::acquire() {
    FftPlan* plan;
    available.wait_dequeue(plan);
    return plan;
}

void FftPool::release(FftPlan* plan) {
    available.enqueue(plan);
}

// Global FFT pool instance
FftPool fft_pool;
