#pragma once

// Per-decimator beamformed-FFT result.
//
// Each decimator runs its own Beamformer steered to its own DoA, so each one
// also produces its own beamformed spectrum slice for the UI overlay and for
// beamformed squelch. This struct holds one decimator's latest beamformed FFT.
// It contains a mutex/atomics, so it is non-copyable/non-movable and must be
// owned in place (DecimatorInstance holds it by value, accessed via shared_ptr).

#include <atomic>
#include <mutex>
#include <complex>
#include <vector>

struct BeamformedFFTData {
    mutable std::mutex mutex;               // Guards magnitudes / averaged (mutable: readers hold const refs)
    std::vector<float> magnitudes;          // Raw dB (display order, DC-centered)
    std::vector<float> averaged;            // EWMA dB
    std::atomic<float> center_freq_hz{0.0f};
    std::atomic<float> bandwidth_hz{0.0f};
    std::atomic<bool> valid{false};         // True once a block has been produced
    // Narrow-bandwidth accumulation (process_beamformed_fft): blocks shorter
    // than the FFT minimum collect here until there are enough samples.
    // Guarded by mutex; reset when the rate or centre frequency changes.
    std::vector<std::complex<float>> pending;
    float pending_rate_hz = 0.0f, pending_center_hz = 0.0f;

    BeamformedFFTData() = default;
    BeamformedFFTData(const BeamformedFFTData&) = delete;
    BeamformedFFTData& operator=(const BeamformedFFTData&) = delete;
};
