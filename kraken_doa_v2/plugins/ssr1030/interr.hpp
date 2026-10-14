#pragma once

// Interrogations from pulses (ICAO Annex 10 Vol IV 3.1.1 / 3.1.2):
//   Mode A / B / C / D   P1, P3 (0.8 us) 8 / 17 / 21 / 25 us apart, equal level
//     + P2 (SLS) 2 us after P1, from the control antenna: in the main beam
//       >= 9 dB below P1, in the sidelobes above it (transponders stay quiet)
//     + P4 2 us after P3: short (0.8 us) = Mode A/C-only all-call, long
//       (1.6 us) = Mode A/C/S all-call (intermode)
//     + S1 2 us before P1 (lower): ACAS whisper-shout suppression pulse
//   Mode S               P1, P2 (2 us, equal level) + P6 3.5 us after P1:
//                        16.25 us (56 bits) / 30.25 us (112 bits) of DPSK
// A pulse waits 40 us before it is tried as a P1 (its partners must be in);
// pulses that take part in an interrogation aren't tried again.

#include "front.hpp"
#include "uplink.hpp"
#include "dpsk.hpp"

#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace ssr {

enum Mode { MODE_A = 0, MODE_B, MODE_C, MODE_D, MODE_S };

struct Interr {
    double p1 = 0;            // P1's leading edge (absolute sample index, fractional)
    double t = 0;             // ... as Host::time()
    double end = 0;           // the end of its last pulse (absolute sample index)
    int mode = MODE_A;
    int p4 = 0;               // 0 none, 1 short (A/C-only all-call), 2 long (A/C/S all-call)
    float db = 0;             // P1 dB over the noise
    float p2_db = NAN;        // P2 against P1 (dB), NAN = no P2
    float s1_db = NAN;        // S1 against P1 (dB), NAN = none
    bool clipped = false;
    // Mode S
    int nbits = 0;
    float p6_db = 0;
    bool decoded = false;     // DPSK decoded (the bits are a guess unless up.verified)
    Uplink up;
    DpskResult dr;
    // the interrogator it was given to (decoder.cpp), -1 = none
    int src = -1;
    int kind() const;         // a small code for patterns: A, C, B, D (+P4 s / L), S56, S112
    const char* name() const; // "A", "C+P4L", "S56" ...
};

class Assembler {
public:
    void reset() {
        q_.clear();
        used_.clear();
        head_ = 0;
    }
    // pulses in time order; finished interrogations appended to out. now = samples pushed so far
    void push(const std::vector<Pulse>& ps, int64_t now, std::vector<Interr>& out);

private:
    std::deque<Pulse> q_;
    std::deque<uint8_t> used_;
    size_t head_ = 0;   // the next pulse to try as a P1
    int find(size_t from, double at, double tol, bool longp, float ref_db, float max_ddb) const;
};

}  // namespace ssr
