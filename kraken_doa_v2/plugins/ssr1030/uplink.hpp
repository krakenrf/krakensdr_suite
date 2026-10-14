#pragma once

// Mode S uplink formats (ICAO Annex 10 Vol IV 3.1.2): the 56 / 112 bits of
// an interrogation's P6, its parity and the fields of each UF.
//
// Address / parity (AP): the 24 parity bits of the first 32 / 88 bits
// (generator 0xFFF409, as the downlink) XOR the address - ENCODED for the
// uplink: the address bits are the coefficients x^47 .. x^24 of A(x) G(x)
// (checked on live interrogations here: UF11 all-calls give the encoded
// all-ones address, discretes the addresses of aircraft heard on 1090 MHz).
// So an all-call / broadcast (address FFFFFF) can be CHECKED; a discrete
// interrogation's AP just gives the address it was sent to.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ssr {

constexpr uint32_t ALL_CALL = 0xFFFFFF;

uint32_t crc24(const uint8_t* bits, int n);         // parity of n unpacked bits (0/1), MSB first
uint32_t encode_address(uint32_t a);                 // uplink encoding of an address
uint32_t decode_address(uint32_t enc);               // its inverse

struct Uplink {
    int nbits = 0;
    uint8_t b[112] = {};      // unpacked bits
    int uf = -1;
    uint32_t ap = 0;          // the AP field as received
    uint32_t address = 0;     // the address it is sent to (decode_address(ap ^ parity))
    bool verified = false;    // all-call / broadcast with a matching parity
    int corrected = 0;        // ... after correcting this many bits (one wrong run of chips)
    bool broadcast = false;   // address FFFFFF (UF11 all-call, UF16 / UF20 / 21 broadcast)
    // interrogator identity it carries: II (0-15) or SI (1-63) code, -1 = none
    int ii = -1, si = -1;
    // ACAS (UF16): the sender's own address from MU (bits 65-88), its UDS
    uint32_t acas_sender = 0;
    int uds = -1;
    uint32_t field(int first, int len) const;   // bits first..first+len-1 (1-based like the Annex), MSB first
    std::string kind() const;                    // "UF11 all-call", "UF4 surveillance, altitude", ...
    std::vector<std::pair<std::string, std::string>> fields() const;   // label / value of every field
    std::string hex() const;
};

// fill uf / ap / address / verified / ii / si / acas fields from the bits
void parse_uplink(Uplink& u);

}  // namespace ssr
