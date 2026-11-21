#pragma once
// DAB+ transport error protection (ETSI TS 102 563 clause 6): the shortened
// Reed-Solomon RS(120, 110, t = 5) code over GF(2^8), P(x) = x^8 + x^4 + x^3 +
// x^2 + 1, alpha = 2, generator g(x) = prod_{i=0..9} (x + alpha^i), derived
// from RS(255, 245) by 135 leading zero bytes. Byte 0 of a code word is the
// highest-order coefficient (the first information byte), bytes 110..119 the
// parity.

#include <cstddef>
#include <cstdint>

namespace dab::audio {

class Rs120 {
public:
    static constexpr int N = 120, K = 110, NPAR = 10, T = 5;
    // Corrects cw[0..119] in place. Returns the number of corrected bytes
    // (0 = was a code word), or -1 when uncorrectable (cw is left as received).
    // An uncorrectable verdict: the Berlekamp-Massey locator has degree > 5,
    // or its roots are not all distinct positions inside the shortened word.
    static int decode(uint8_t* cw);
    // Systematic encoding (tests): parity of data[0..109] into par[0..9]
    static void encode(const uint8_t* data, uint8_t* par);
};

}   // namespace dab::audio
