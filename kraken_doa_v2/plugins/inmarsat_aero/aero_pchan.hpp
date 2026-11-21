#pragma once

// P channel (ground earth station -> aircraft, TDM, continuous) frame
// layer: ICAO Annex 10 Vol III 4.4.2 and figures 4-1 / 4-2.
//
// A frame (500 ms, or 1 s at 1.2 and 2 s at 0.6 kbit/s; 16 frames = the
// 8 s superframe) is: unique word (32 bits; A-QPSK: the 32 on I and Q
// each, interleaved = 64 bits), format ID (4 bits, 0001), superframe
// marker (4 bits 1111 in frame 0 else 0000, frame number, frame number
// again), dummy bits (16 at 4.8, 178 at 10.5 kbit/s: 0001 repeated) and
// the information field: 1 to 3 interleaver blocks of 64 rows; inside,
// the signal units (96 bits each) scrambled, then coded (rate 1/2, K = 7;
// the encoder runs on from frame to frame) and interleaved.
//
// Sync: the unique word (hard decisions, a few errors allowed) found
// anywhere, then expected one frame later (flywheel, more errors
// allowed); three missed in a row = lost. The UW also fixes the phase
// ambiguities: A-BPSK's polarity (differential detection leaves only
// that) and, for A-QPSK, each branch's polarity and which bit is on I.

#include "aero_demod.hpp"
#include "aero_fec.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aero {

// the P channel's 32-bit unique word, first transmitted bit first
constexpr uint32_t UW_P = 0xE15AE893u;   // 1110 0001 0101 1010 1110 1000 1001 0011

struct PRate {
    double rate;
    Mod mod;
    int uw;       // UW bits (32, or 64 for A-QPSK)
    int dummy;    // dummy bits after the header
    int blocks;   // interleaver blocks
    int cols;     // columns per block
    int sus;      // signal units per frame
    int frame;    // bits per frame incl. the UW
    int frames_per_superframe;
};

const PRate* p_rate(double rate);   // nullptr if not a P channel rate

struct PFrame {
    double t0 = 0, t1 = 0;          // input sample times of the frame's first bit and its end
    int frame_no = -1;              // in the superframe
    bool sf_start = false;
    bool header_ok = false;         // format ID 0001 and the frame number twice alike
    int uw_errors = 0;
    int sus = 0, sus_ok = 0;
};

class PChannel {
public:
    // one checked signal unit (12 octets incl. CRC), the frame it came in
    std::function<void(const uint8_t* su, const PFrame& f, int index)> on_su;
    // a frame's SUs were all handled
    std::function<void(const PFrame& f)> on_frame;

    void start(const PRate& r);
    void stop();
    void push(const SoftBit* b, size_t n);
    bool synced() const { return synced_; }
    int missed() const { return missed_; }        // UWs missed in a row while synced
    uint64_t frames() const { return frames_; }   // frames decoded since start
    uint64_t uws() const { return uws_; }
    const PRate& rate() const { return r_; }

private:
    PRate r_{};
    bool on_ = false;
    std::vector<float> s_;      // soft bits (A-BPSK: after differential detection)
    std::vector<double> t_;     // their times
    int64_t base_ = 0;          // absolute index of s_[0]
    int64_t next_uw_ = -1;      // expected start of the next UW (absolute), -1 = searching
    int64_t search_from_ = 0;
    bool synced_ = false;
    int missed_ = 0;
    uint64_t frames_ = 0, uws_ = 0;
    float prev_ = 0;
    bool have_prev_ = false;
    std::vector<float> uwpat_;  // +-1 per UW bit (bit 0 = +1), length r_.uw

    // Viterbi and the frames waiting for their decoded bits
    Viterbi vit_;
    std::vector<uint8_t> dec_;
    int64_t dec_base_ = 0;      // decoded-bit index of dec_[0]
    struct Pending {
        PFrame f;
        int64_t start;          // decoded-bit index of its first information bit
    };
    std::vector<Pending> pending_;
    int64_t vit_steps_ = 0;     // pairs pushed into vit_ since its reset

    int64_t idx_end() const { return base_ + static_cast<int64_t>(s_.size()); }
    bool match_uw(int64_t p, int max_err, int& err, float& sa, float& sb) const;
    float soft_corr(int64_t p) const;
    std::vector<float> facc_;   // soft UW correlation averaged per position in the frame
    bool cand_soft_ = false;    // the candidate came from the averaged correlation
    void frame_at(int64_t p, int err, float sa, float sb);
    void drain(bool flush);
    void trim();
};

}  // namespace aero
