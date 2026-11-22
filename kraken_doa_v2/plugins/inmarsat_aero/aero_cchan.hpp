#pragma once

// C channel (circuit mode, single channel per carrier: voice) at 8.4
// kbit/s, A-QPSK: ICAO Annex 10 Vol III 4.4.5 and figure 4-9 bis.
//
// Frame (500 ms, 4200 bits): unique word - 52 bits on I and a different 52
// on Q, interleaved = 104 bits - then 16 interleaver blocks of 64 x 4
// (4096 bits). Inside, before interleaving: 4095 bits of the rate 2/3 code
// (the K = 7 code's G1 G2 G1 for every two bits, G2 of the second dropped)
// and one dummy bit; before coding, 2730 scrambled bits: a reference bit
// (1), then 25 x (96-bit primary field = one 20 ms voice frame, a
// reference bit... ) interleaved with 24 x 12-bit sub-band fields (three
// 96-bit signal units per frame for signalling: call progress, telephony
// acknowledgements, fill-in), a last reference bit and 16 dummy bits.
// Exact layout: group k (0..24) = reference bit, primary field k, then
// (k < 24) sub-band field k; then the final reference bit.

#include "aero_demod.hpp"
#include "aero_fec.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace aero {

struct CFrame {
    double t0 = 0, t1 = 0;     // input times of the frame
    int uw_errors = 0;
    int ref_errors = 0;        // of the 26 reference bits (all "1")
    int sus_ok = 0;
    // the 25 voice frames (96 bits each, transmission order)
    std::array<std::array<uint8_t, 96>, 25> voice{};
};

class CChannel {
public:
    std::function<void(const uint8_t* su12, const CFrame& f)> on_su;
    std::function<void(const CFrame& f)> on_frame;

    void start();
    void stop();
    void push(const SoftBit* b, size_t n);
    bool synced() const { return synced_; }
    uint64_t frames() const { return frames_; }

    static constexpr int FRAME = 4200, UWB = 104;

private:
    bool on_ = false;
    std::vector<float> s_;
    std::vector<double> t_;
    int64_t base_ = 0, next_uw_ = -1, search_from_ = 0;
    bool synced_ = false, cand_soft_ = false;
    int missed_ = 0;
    uint64_t frames_ = 0;
    std::vector<float> facc_;
    Viterbi vit_;
    std::vector<uint8_t> dec_;
    int64_t dec_base_ = 0, vit_steps_ = 0;
    struct Pending {
        CFrame f;
        int64_t start;
    };
    std::vector<Pending> pending_;

    int64_t idx_end() const { return base_ + static_cast<int64_t>(s_.size()); }
    bool match_uw(int64_t p, int max_err, int& err, float& sa, float& sb) const;
    float soft_corr(int64_t p) const;
    void frame_at(int64_t p, int err, float sa, float sb);
    void drain(bool flush);
    void restart_code();
    void trim();
};

}  // namespace aero
