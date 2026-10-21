#pragma once

// Reed-Solomon codes over GF(2^8), errors only: syndromes, Berlekamp-Massey,
// Chien search, Forney. The code is given by its field polynomial, the first
// consecutive root and the root spacing: roots beta^(fcr + j), j = 0 ..
// nroots-1, beta = alpha^prim, alpha = x (2). A codeword is 255 coefficients,
// cw[i] = the coefficient of x^i; a shortened code keeps the positions from
// `used` on at zero (a "correction" there is a failure).
//   RS41:  0x11D, fcr 0,   prim 1,  24 roots
//   LMS6:  0x187, fcr 112, prim 11, 32 roots (CCSDS, conventional basis)

#include <cstdint>

namespace sonde {

class ReedSolomon {
public:
    ReedSolomon(unsigned poly, int fcr, int prim, int nroots);
    // corrects cw in place; returns the number of corrected symbols, or -1
    // (uncorrectable, or the result doesn't verify - cw is then unchanged)
    int decode(uint8_t* cw, int used) const;
    // systematic encoding: cw[nroots ..] holds the data, cw[0 .. nroots-1]
    // gets the parity (data x^nroots mod g(x)); positions from `used` on are zero
    void encode(uint8_t* cw, int used) const;
    int nroots() const { return nroots_; }

private:
    int fcr_, prim_, nroots_;
    uint8_t exp_[512];
    int log_[256];
    uint8_t gen_[33];             // generator polynomial, gen_[k] = coefficient of x^k

    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; }   // alpha^e
    bool syndromes(const uint8_t* cw, int used, uint8_t* s) const;      // true = all zero
};

}  // namespace sonde
