#pragma once

#include <complex>
#include <atomic>
#include <array>
#include <bit>
#include <cstdint>

// Lock-free atomic complex number for hot-path phase compensation.
// Both floats are packed into ONE 64-bit atomic: with separate real/imag
// atomics a hot-path read racing a calibration write could pair the new real
// part with the old imaginary part (a torn vector - wrong gain and phase for
// that block). A single 64-bit load/store is lock-free on aarch64 and x86_64.
struct AtomicComplex {
    std::atomic<uint64_t> bits;

    static uint64_t pack(const std::complex<float>& v) {
        return (static_cast<uint64_t>(std::bit_cast<uint32_t>(v.real())) << 32) |
               std::bit_cast<uint32_t>(v.imag());
    }
    static std::complex<float> unpack(uint64_t b) {
        return {std::bit_cast<float>(static_cast<uint32_t>(b >> 32)),
                std::bit_cast<float>(static_cast<uint32_t>(b))};
    }

    AtomicComplex() : bits(pack({1.0f, 0.0f})) {}

    AtomicComplex(float r, float i) : bits(pack({r, i})) {}

    // Fast lock-free read (hot path - called millions of times/sec)
    inline std::complex<float> load_relaxed() const {
        return unpack(bits.load(std::memory_order_relaxed));
    }

    // Thread-safe write (cold path - called during calibration only)
    inline void store_release(const std::complex<float>& value) {
        bits.store(pack(value), std::memory_order_release);
    }

    // Prevent copying (atomics aren't copyable)
    AtomicComplex(const AtomicComplex&) = delete;
    AtomicComplex& operator=(const AtomicComplex&) = delete;

    // Allow move construction for initialization
    AtomicComplex(AtomicComplex&& other)
        : bits(other.bits.load(std::memory_order_relaxed)) {}
};
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "AtomicComplex needs a lock-free 64-bit atomic for the hot path");

// Lock-free phase compensation vector
// Aligned for cache efficiency
template<size_t N>
struct alignas(64) AtomicCompensationVector {
    std::array<AtomicComplex, N> data;  // each element default-initializes to identity (1, 0)

    // Fast read for hot path (no locks!)
    inline std::complex<float> load(size_t index) const {
        return data[index].load_relaxed();
    }

    // Thread-safe write for calibration (cold path)
    inline void store(size_t index, const std::complex<float>& value) {
        data[index].store_release(value);
    }
};
