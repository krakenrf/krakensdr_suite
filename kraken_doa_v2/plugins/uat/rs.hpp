#pragma once

// Reed-Solomon RS(255, 255-nroots) over GF(2^8), errors only:
// Berlekamp-Massey -> Chien search -> Forney. Parameters as in the CCSDS /
// Karn convention: field polynomial, first consecutive root (fcr) and root
// spacing (prim): the code's roots are beta^(fcr+j), beta = alpha^prim.
//
// Codeword convention here: cw[i] = coefficient of x^i, i = 0..254 (a
// systematic code keeps its parity in cw[0..nroots-1], the message above;
// a shortened code has zeros at the top).
//   UAT (DO-282B): poly 0x187, fcr 120, prim 1, 12 / 14 / 20 roots (RS(30,18), RS(48,34), RS(92,72))
// (the same code as plugins/radiosonde/rs.cpp)

#include <cstdint>

namespace uat {

class ReedSolomon {
public:
    ReedSolomon(unsigned gfpoly, int fcr, int prim, int nroots);
    // corrects cw[0..254] in place: number of symbols corrected, -1 = too many errors
    int decode(uint8_t* cw) const;
    // true if every syndrome is 0
    bool check(const uint8_t* cw) const;
private:
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t pow_beta(int e) const;    // beta^e
    void syndromes(const uint8_t* cw, uint8_t* s) const;
    uint8_t exp_[512];
    int log_[256];
    int fcr_, prim_, nroots_;
};

}  // namespace uat
