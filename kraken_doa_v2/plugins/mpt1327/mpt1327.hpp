#pragma once

// MPT1327 receiver (mpt1327.cpp) on a 48 kHz FM discriminator

#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"

namespace dig {

// ---------------------------------------------------------------------------
// MPT1327 analogue trunking signalling: 1200 bit/s FFSK (1 = 1200 Hz,
// 0 = 1800 Hz) on an NBFM carrier. Control channel slots of 128 bits: the
// control channel system codeword (CCSC, whose check bits are the SYNC
// 0xC4D7) and an address codeword. Codewords: 48 bits + BCH(63,48) + parity.
// ---------------------------------------------------------------------------
class Mpt1327Receiver {
public:
    explicit Mpt1327Receiver(RxContext& c);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
private:
    RxContext& ctx_;
    SampleBuf m_;                    // per-sample tone metric (+ = 1200 Hz)
    std::complex<float> ring12_[40], ring18_[40];   // last bit's tone products
    size_t hpos_ = 0;
    std::complex<float> c12_{0, 0}, c18_{0, 0}, r12_{1, 0}, r18_{1, 0};
    float lvl12_ = 1.0f, lvl18_ = 1.0f;   // running tone levels
    std::complex<float> w12_, w18_;
    int64_t n_ = 0;
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int age = 0; int which = 0; } pk_;
    struct Job { int64_t end; int which; };
    std::deque<Job> jobs_;
    uint32_t sys_ = 0xFFFFFFFF;
    std::string last_aloha_;
    void decode_slot(int64_t sync_end, int which);
    bool codeword(int64_t first_bit_end, uint64_t* cw);   // 64 bits -> corrected 48-bit info
    void address(uint64_t info, bool control);
};

}  // namespace dig
