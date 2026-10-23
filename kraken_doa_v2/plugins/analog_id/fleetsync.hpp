#pragma once

// Kenwood FleetSync coding. The format as the public decoders document it
// (Matthew Kaufman's GPL fsync-encode-decode and sdrtrunk's FleetSync
// classes, read as documentation - not copied):
//
//   FFSK, a "1" = 1200 Hz, a "0" = 1800 Hz (1200 bit/s) or 2400 Hz (2400
//   bit/s), no precoding. Preamble 1010..., sync 0x23EB (also seen: 0x052B)
//   - the decoders match 0xAAAA23EB / 0xAAAA052B. Then 64-bit blocks: 48
//   data bits (6 bytes), a 15-bit CRC and an even parity bit over the block.
//   A message is every block that checks, back to back (ANI / status: one;
//   another fleet: two; data / GPS: more).
//   Block 1: byte 0 = active-low flags (emergency 0x40, lone worker 0x10,
//   paging 0x04, end of transmission 0x02) or, in a status message, the
//   status number (bits 7..1); byte 1 = type flags (ANI 0x80, status 0x40,
//   acknowledge 0x20, GPS 0x02, another fleet 0x01); byte 2 = fleet - 99;
//   bytes 3-5 = from unit - 999 (12 bits), to unit - 999 (12 bits).
//   FleetSync II sends each block's 4 nibbles as 4 BCH(15,7) codewords
//   (x^8 + x^4 + x^2 + x + 1, bit 14 = x^0), 4 blocks per FleetSync block.

#include <cstdint>
#include <string>
#include <vector>

namespace fleetsync {

constexpr uint32_t SYNC_A = 0xAAAA23EBu, SYNC_B = 0xAAAA052Bu;
constexpr int SYNC_BITS = 32;

// the block's check value (bits 15..1 CRC, bit 0 even parity over the 64)
// computed from w1 (bits 0-31) and w2 (bits 32-63), MSB first
inline uint32_t check(uint32_t w1, uint32_t w2) {
    uint32_t sr = 0;
    int par = 0;
    for (int b = 0; b < 48; b++) {
        const int cur = b < 32 ? (w1 >> (31 - b)) & 1 : (w2 >> (63 - b)) & 1;
        par ^= cur;
        if (cur ^ ((sr >> 15) & 1)) sr ^= 0x6815;
        sr <<= 1;
    }
    for (int b = 48; b < 63; b++) par ^= (w2 >> (63 - b)) & 1;
    return ((sr ^ 0x0002) + static_cast<uint32_t>(par)) & 0xFFFF;
}
inline bool block_ok(uint32_t w1, uint32_t w2) { return check(w1, w2) == (w2 & 0xFFFF); }

// a block for 6 data bytes (the generator / round trips)
inline void make_block(const uint8_t d[6], uint32_t* w1, uint32_t* w2) {
    *w1 = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
    *w2 = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16);
    // the CRC part does not depend on the low 16 bits; then the parity bit
    const uint32_t c = check(*w1, *w2) & 0xFFFE;
    *w2 |= c;
    if (!block_ok(*w1, *w2)) *w2 |= 1;
}

// FleetSync II: one 15-bit BCH codeword (bit 14 = x^0) - syndrome, a single
// error corrected; -1 = not a codeword
inline int bch15_syndrome(uint32_t w) {
    uint32_t r = 0;   // the remainder of c(x) mod g(x), g = x^8 + x^4 + x^2 + x + 1
    for (int p = 0; p < 15; p++) {
        if (!((w >> p) & 1)) continue;
        uint32_t t = 1;   // x^(14 - p) mod g
        for (int i = 0; i < 14 - p; i++) {
            t <<= 1;
            if (t & 0x100) t ^= 0x117;
        }
        r ^= t;
    }
    return static_cast<int>(r);
}
inline int bch15_repair(uint32_t w) {
    w &= 0x7FFF;
    const int s = bch15_syndrome(w);
    if (s == 0) return static_cast<int>(w);
    for (int p = 0; p < 15; p++)
        if (bch15_syndrome(1u << p) == s) return static_cast<int>(w ^ (1u << p));
    return -1;
}
// the codeword carrying a nibble at bits 11..8 (the generator)
inline uint32_t bch15_encode_nibble(int nib) {
    for (uint32_t w = 0; w < 0x8000; w++)
        if (((w >> 8) & 0xF) == static_cast<uint32_t>(nib & 0xF) && bch15_syndrome(w) == 0) return w;
    return 0;
}

struct Message {
    std::vector<uint8_t> bytes;   // 6 per block
    bool fs2 = false;
    int baud = 1200;
    int blocks() const { return static_cast<int>(bytes.size() / 6); }
    uint8_t flags() const { return bytes.size() > 0 ? bytes[0] : 0; }
    uint8_t type() const { return bytes.size() > 1 ? bytes[1] : 0; }
    int from_fleet() const { return bytes.size() > 2 ? bytes[2] + 99 : -1; }
    int from_unit() const { return bytes.size() > 4 ? ((bytes[3] << 4) | (bytes[4] >> 4)) + 999 : -1; }
    int to_unit() const { return bytes.size() > 5 ? (((bytes[4] & 0x0F) << 8) | bytes[5]) + 999 : -1; }
    bool other_fleet() const { return type() & 0x01; }
    int to_fleet() const { return other_fleet() ? (bytes.size() > 6 ? bytes[6] + 99 : -1) : from_fleet(); }
    bool ani() const { return type() & 0x80; }
    bool status() const { return type() & 0x40; }
    bool ack() const { return type() & 0x20; }
    bool gps() const { return type() & 0x02; }
    // active-low flags (not in a status message: there they are the number)
    bool emergency() const { return !status() && !(flags() & 0x40); }
    bool lone_worker() const { return !status() && !(flags() & 0x10); }
    bool paging() const { return !status() && !(flags() & 0x04); }
    bool eot() const { return !(flags() & 0x02); }
    int status_number() const { return flags() >> 1; }
    // a payload (type 0x42: text / data): its length in bytes 9-10, then
    // every 6 bytes in reverse order (as the reference decoder lays it out)
    std::string payload() const {
        if ((type() & 0xFE) != 0x42 || bytes.size() < 11) return "";
        const int len = (bytes[9] << 8) | bytes[10];
        std::string p;
        for (int i = 0; i < len && i < 256; i++) {
            const size_t off = static_cast<size_t>(11 + 6 * (i / 6 + 1) - (i % 6));
            if (off >= bytes.size()) break;
            p += static_cast<char>(bytes[off]);
        }
        return p;
    }
};

inline std::string unit_name(int fleet, int unit) {
    auto part = [](int v, int none) { return v == none || v < 0 ? std::string("all") : std::to_string(v); };
    return part(fleet, 99) + "-" + part(unit, 999);
}

}  // namespace fleetsync
