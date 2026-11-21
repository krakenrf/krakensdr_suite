#pragma once
// DAB channel coding (EN 300 401 clauses 10, 11, 12, annex E): the K = 7,
// rate 1/4 mother code (133, 171, 145, 133 octal) with its puncturing,
// a soft-decision Viterbi decoder for it, the energy dispersal PRBS and the
// FIB CRC.
//
// Soft bits are int8: > 0 = "0", < 0 = "1", 0 = erased / punctured.

#include "dab_tables.hpp"

#include <cstdint>
#include <vector>

namespace dab {

// Viterbi decoder of the mother code, terminated (6 zero tail bits): nbits
// information bits from 4 * (nbits + 6) soft bits. Buffers are kept between
// calls (no allocation once grown). Returns the path metric margin (a
// quality figure: the winning metric minus the mean, per bit).
class Viterbi {
public:
    Viterbi();
    void decode(const int8_t* soft, int nbits, uint8_t* out_bits);
private:
    std::vector<uint8_t> dec_;   // 64 decisions per step
    int8_t sign_[4][32];         // +1 / -1: output bit r of butterfly j for input 0 from the even state
};

// The convolutional encoder (re-encoding for the pre-Viterbi bit error count)
// -> 4 * (n + 6) mother code bits
void conv_encode(const uint8_t* bits, int n, uint8_t* out);

// Depuncturing map of a coding profile: for every transmitted bit, its
// position in the mother codeword (4 I + 24 bits); padding bits get -1.
struct Depuncture {
    std::vector<int> pos;        // transmitted bit -> mother position (or -1)
    int mother = 0;              // 4 I + 24
    int in_bits = 0;             // I
    bool build(const Profile& p);
    // mother codeword soft bits (erasures where punctured) from the transmitted ones
    void apply(const int8_t* rx, int8_t* mother_soft) const;
};

// Energy dispersal PRBS (x^9 + x^5 + 1, all ones): bit i (packed MSB first)
const std::vector<uint8_t>& prbs_bytes();   // 43776 / 8 bytes, enough for any sub-channel

// FIB CRC (x^16 + x^12 + x^5 + 1, init all ones, complemented): true if the
// 32-byte FIB's last 2 bytes match its first 30
bool fib_crc_ok(const uint8_t* fib);

}   // namespace dab
