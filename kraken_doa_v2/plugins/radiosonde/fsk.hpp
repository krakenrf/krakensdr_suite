#pragma once

// Binary FSK frame receiver for one sonde type, on the FM discriminator.
//
// Matched filter: a moving average over one symbol (integrate and dump - the
// optimum for rectangular FSK on a discriminator, and nearly so for GFSK).
// NPH timing phases sample it at NPH evenly spaced offsets within a symbol,
// each with its own hard-symbol shift register compared against the sync
// pattern (both polarities: the IQ can arrive conjugated). A phase whose
// register matches captures the frame: carrier offset (DC) and deviation are
// measured on the sync symbols, then frame_syms soft symbols are read with a
// Gardner timing loop (the sonde's clock is never exactly the nominal rate -
// M10 9615 vs M20 9600 baud would drift several symbols across a frame).
// Several phases usually catch the same frame; the type decoder keeps the
// first one that passes its checks (Frame::t0 tells them apart).
//
// Soft symbols are normalised: +1 / -1 = the sync's "1" / "0" levels.

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace sonde {

struct FskConfig {
    double baud = 4800;               // symbol (chip) rate
    std::vector<uint8_t> sync;        // sync symbols as transmitted (0/1), <= 64 used
    int max_sync_err = 4;             // Hamming distance accepted on the sync
    int frame_syms = 0;               // symbols read after the sync
    int nph = 8;                      // timing phases
    float timing_gain = 0.02f;        // Gardner loop gain (fraction of a symbol per symbol, at full error)
};

struct FskFrame {
    const float* soft = nullptr;      // frame_syms soft symbols (after the sync)
    int n = 0;
    double t0 = 0;                    // sample index (input rate) of the first frame symbol
    float dc_hz = 0;                  // carrier offset measured on the sync
    float dev_hz = 0;                 // deviation (half the distance between the levels)
    int sync_err = 0;                 // symbol errors in the sync
    bool inverted = false;            // matched the inverted sync
};

class FskRx {
public:
    FskRx() = default;
    FskRx(const FskConfig& c, double fs) { init(c, fs); }
    void init(const FskConfig& c, double fs);
    void reset();
    // one discriminator sample (Hz)
    void push(float x);
    std::function<void(const FskFrame&)> on_frame;
    double sps() const { return sps_; }

private:
    struct Phase {
        double next = 0;              // sample index of the next symbol sample (fixed grid: sync search)
        double ct = 0;                // the capture's own sample time (follows the timing loop)
        uint64_t reg = 0;             // last hard symbols (bit 0 = newest)
        int count = 0;
        std::vector<float> hist;      // last sync_len soft samples (ring)
        int hpos = 0;
        // capture in progress
        bool cap = false;
        bool inv = false;
        float dc = 0, dev = 1;
        int got = 0;
        double t0 = 0;
        int sync_err = 0;
        std::vector<float> frame;
        float prev = 0;               // previous symbol sample (normalised) for the timing loop
    };
    float ma_at(double t) const;      // moving average at a fractional sample index
    void search(Phase& p, float v);
    void capture(Phase& p, float v);

    FskConfig c_;
    double sps_ = 10;
    int L_ = 10;                      // moving average length
    std::vector<float> win_;          // moving average input ring
    int wpos_ = 0;
    double sum_ = 0;
    std::vector<float> ma_;           // moving average output ring (for interpolation)
    int mlen_ = 0;
    int64_t n_ = 0;                   // samples pushed
    uint64_t sync_ = 0, mask_ = 0;
    int slen_ = 0;
    std::vector<Phase> ph_;
};

}  // namespace sonde
