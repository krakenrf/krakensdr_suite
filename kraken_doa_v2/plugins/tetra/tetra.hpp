#pragma once

// TETRA downlink receiver (tetra.cpp) on 72 kHz RRC-filtered samples

#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"
#include "dig_vocoder.hpp"

namespace dig {

// ---------------------------------------------------------------------------
// TETRA downlink (EN 300 392-2), pi/4-DQPSK 18 ksym/s
// ---------------------------------------------------------------------------
class TetraReceiver {
public:
    explicit TetraReceiver(RxContext& c);
    // matched-filtered complex samples, 72 kHz (4 samples/symbol)
    void process(const std::complex<float>* x, size_t n);
    void reset();
private:
    RxContext& ctx_;
    std::vector<std::complex<float>> hist_;   // recent samples
    int64_t hist_base_ = 0;
    // timeslot tracking
    int64_t slot_start_ = -1;     // sample index of the current slot start
    bool conj_ = false;           // spectrum inverted (I/Q swapped source)
    float dphi_ = 0.0f;           // per-symbol rotation (frequency offset)
    int locked_slots_ = 0;
    int miss_ = 0;
    // cell
    bool have_cell_ = false;
    uint32_t mcc_ = 0, mnc_ = 0, cc_ = 0;
    int tn_ = 0, fn_ = 0, mn_ = 0;
    uint32_t scramb_ = 0;
    std::string last_sysinfo_;
    // voice: TCH/S of one traffic timeslot -> the user-built ETSI codec
    int slot_usage_[5] = {-1, -1, -1, -1, -1};   // AACH usage marker per timeslot
    int voice_tn_ = 0;
    int64_t voice_tn_ms_ = 0;
    int64_t encrypted_ms_ = 0;       // last encrypted MAC-RESOURCE seen
    int64_t last_tch_ms_ = 0;
    bool codec_flushed_ = true;
    std::unique_ptr<TetraCodec> codec_;
    std::string last_call_;
    void voice_slot(const uint8_t* bits);
    void voice_idle();
    void search();
    bool demod_slot(int64_t start, int phase, float dphi, uint8_t* bits510, float* soft510);
    void decode_slot(const uint8_t* bits, const float* soft);
    bool decode_block(const float* soft, int n345, int k1, int a, uint32_t scr, std::vector<uint8_t>& type1);
    void bsch(const uint8_t* t1);
    void mac_pdus(const uint8_t* t1, int k1, const char* chan);
    void aach(const uint8_t* bits30);
    void sysinfo(const uint8_t* t1);
};

}  // namespace dig
