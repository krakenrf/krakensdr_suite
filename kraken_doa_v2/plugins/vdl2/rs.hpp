#pragma once

// Reed-Solomon RS(255, 249) over GF(2^8) with errors AND erasures, as VDL
// Mode 2 uses it (ICAO Annex 10 Vol III, 6.4.3.1.2): field polynomial
// x^8 + x^7 + x^2 + x + 1, generator roots alpha^120 .. alpha^125. A short
// block sends only 2 or 4 of the 6 check octets; the others are erasures.
//
// Syndromes -> Berlekamp-Massey started from the erasure locator -> Chien
// search -> Forney; the result is checked by recomputing the syndromes.
// Codeword convention: cw[i] = the coefficient of x^i (the check octets at
// cw[0..5], the first data octet at cw[254]).

#include <cstdint>

namespace vdl2 {

class ReedSolomon {
public:
    static constexpr int NROOTS = 6;
    ReedSolomon();
    // corrects cw[0..254] in place. erasures: positions (indexes into cw).
    // cw[fill_lo .. fill_hi] must stay zero (a shortened block's virtual
    // fill; fill_lo > fill_hi = none): a correction there is a failure.
    // Returns the number of corrected symbols (erasures included), or -1
    // (uncorrectable; cw is then unchanged).
    int decode(uint8_t* cw, const int* erasures, int nerasures, int fill_lo, int fill_hi) const;

private:
    uint8_t exp_[512];
    int log_[256];
    uint8_t mul(uint8_t a, uint8_t b) const { return a && b ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; }   // alpha^e
    bool syndromes(const uint8_t* cw, uint8_t* s) const;                // true = all zero
};

}  // namespace vdl2
