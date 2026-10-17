#pragma once

#include "../core/types.hpp"
#include "../external/concurrentqueue/blockingconcurrentqueue.h"
#include <complex>
#include <memory>
#include <vector>

struct PFFFT_Setup;

// RAII FFT plan: one pffft setup (external/pffft) for CORRELATION_SIZE
// complex points + its buffers. Results are FFTW's: natural bin order,
// unscaled forward (e^-) and inverse. Not thread-safe - one per thread at a
// time, through FftPool
class FftPlan {
private:
    PFFFT_Setup* setup;
    // 64-byte aligned (pffft_aligned_malloc), CORRELATION_SIZE complex each.
    // The forward transform runs in place in `output`; pffft's scratch is one
    // buffer per thread (fft_plan.cpp) - a correlation set walks 5-9 plans,
    // and each 256 KB buffer it touches is memory traffic (the set is
    // memory-bound on the Pi, not arithmetic-bound)
    std::complex<float> *output, *correlation_freq, *correlation_time;

public:
    FftPlan();
    ~FftPlan();

    FftPlan(const FftPlan&) = delete;
    FftPlan& operator=(const FftPlan&) = delete;
    FftPlan(FftPlan&&) = delete;
    FftPlan& operator=(FftPlan&&) = delete;

    void forward(const ComplexBuffer& data, int offset = 0);
    std::complex<float>* get_output();
    std::complex<float>* get_correlation_freq();
    std::complex<float>* get_correlation_time();
    void cross_correlate(const std::complex<float>* ref_fft, const std::complex<float>* sig_fft);

    // Inverse-transform an arbitrary CORRELATION_SIZE frequency response into
    // the time domain (used to design the per-bin equalizer FIR). Copies
    // freq_in into the internal correlation_freq buffer, runs the inverse plan,
    // and returns the (unnormalized, scaled by CORRELATION_SIZE) time output.
    std::complex<float>* inverse_transform(const std::complex<float>* freq_in);
};

// Simple FFT pool for thread safety
class FftPool {
private:
    std::vector<std::unique_ptr<FftPlan>> plans;
    moodycamel::BlockingConcurrentQueue<FftPlan*> available;

public:
    FftPool(int size = NUM_DEVICES + 1);

    FftPlan* acquire();
    void release(FftPlan* plan);
};

// Global FFT pool instance
extern FftPool fft_pool;
