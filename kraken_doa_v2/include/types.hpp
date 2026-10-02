#pragma once

#include <atomic>
#include <chrono>
#include <vector>
#include <complex>
#include <cstdint>

// Array Topology Enumeration
enum class ArrayTopology {
    UCA,     // Uniform Circular Array
    ULA,     // Uniform Linear Array
    CUSTOM   // Custom user-defined element positions
};

// ULA Forward/Backward Output Mode
// A uniform linear array has an inherent front/back (mirror) ambiguity about
// its axis: the response at angle theta equals the response at (180 - theta).
// (Also used for CUSTOM arrays whose elements all lie in one vertical plane
// facing forward, e.g. an upright patch panel - same mirror symmetry.)
// This selects which half-plane of the pseudospectrum is reported, resolving
// the ambiguity by truncating the spectrum to one side.
enum class ULAOutputMode {
    BOTH,      // Full ambiguous spectrum (mirror image kept) - default
    FORWARD,   // Forward half only  (within +/-90 deg of 0 deg)
    BACKWARD   // Backward half only (within +/-90 deg of 180 deg)
};

// Element Position for Custom Array Topology (coordinates in mm)
struct ElementPosition {
    float x_mm = 0.0f;  // X coordinate in mm
    float y_mm = 0.0f;  // Y coordinate in mm
    float z_mm = 0.0f;  // Z coordinate in mm (non-zero enables 2D MUSIC with elevation)
};

// Squelch Method Enumeration
enum class SquelchMethod {
    FFT,             // Traditional FFT peak-based squelch
    EIGENVALUE,      // Eigenvalue ratio from MUSIC covariance (λ1/mean(λ2..λN)), manual threshold
    EIGENVALUE_AUTO  // Eigenvalue ratio with a self-learned threshold: tracks the
                     // ratio's noise floor (fast attack down, slow drift up on
                     // quiet frames), threshold = floor x margin. Learning is
                     // FFT-gated: while a visible in-band FFT peak is present
                     // the floor is frozen, so a continuous transmission never
                     // teaches itself as noise (unlearned floor = threshold 0 =
                     // open), while coherent sub-floor junk (idle λ of 4-6 on
                     // some frequencies, no FFT peak) is learned and squelched.
                     // Resets and relearns whenever the VFO frequency moves.
};

// Per-Channel Information Structure
struct ChannelInfo {
    std::atomic<float> frequency_hz{100000000.0f};  // Default 100 MHz
    std::atomic<float> gain_db{49.6f};              // Default gain
    
    ChannelInfo() = default;
    ChannelInfo(const ChannelInfo& other) 
        : frequency_hz(other.frequency_hz.load()), gain_db(other.gain_db.load()) {}
    
    ChannelInfo& operator=(const ChannelInfo& other) {
        frequency_hz = other.frequency_hz.load();
        gain_db = other.gain_db.load();
        return *this;
    }
};

// FFT Work Item Structure (stores complex<float>* to avoid wasteful conversions)
struct FFTWorkItem {
    uint32_t channel;
    const std::complex<float>* data;
    size_t num_samples;  // Number of complex samples
    // persistent_data_buffer generation this item's data was written under;
    // 0 = data not from the shared buffer (no validation possible)
    uint32_t buffer_generation;
    std::chrono::steady_clock::time_point timestamp;

    FFTWorkItem() : channel(0), data(nullptr), num_samples(0), buffer_generation(0) {}
    FFTWorkItem(uint32_t ch, const std::complex<float>* ptr, size_t num, uint32_t gen = 0)
        : channel(ch), data(ptr), num_samples(num), buffer_generation(gen),
          timestamp(std::chrono::steady_clock::now()) {}
};

// Compressed Decimated FFT Structure (uint8 for 4x bandwidth reduction)
struct CompressedDecimatedFFT {
    std::vector<float> frequencies;      // Still float32 for accuracy
    std::vector<uint8_t> min_envelope;   // Compressed magnitude data
    std::vector<uint8_t> max_envelope;   // Compressed magnitude data
};
