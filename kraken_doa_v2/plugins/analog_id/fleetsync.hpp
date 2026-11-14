#pragma once

// Kenwood FleetSync coding. The field layout as the public decoders
// document it (Matthew Kaufman's fsync-encode-decode and sdrtrunk's
// FleetSync classes, both GPL - read as documentation of the format's facts
// only; no code of theirs is used):
//
//   FFSK, a "1" = 1200 Hz, a "0" = 1800 Hz (1200 bit/s) or 2400 Hz (2400
//   bit/s), no precoding. Preamble 1010..., sync 0x23EB (also seen: 0x052B)
//   - the decoders match 0xAAAA23EB / 0xAAAA052B. Then 64-bit blocks, each
//   an MPT 1327 codeword: 48 data bits (6 bytes), the 15 check bits of the
//   BCH(63,48) code MPT 1327 uses with its last bit inverted, and an even
//   parity bit over the block (the same check as plugins/mpt1327's cw_ok).
//   A message is every block that checks, back to back (ANI / status: one;
//   another fleet: two; data / GPS: more).
//   Block 1: byte 0 = active-low flags (emergency 0x40, lone worker 0x10,
//   paging 0x04, end of transmission 0x02) or, in a status message, the
//   status number (bits 7..1); byte 1 = type flags (ANI 0x80, status 0x40,
//   acknowledge 0x20, GPS 0x02, another fleet 0x01); byte 2 = fleet - 99;
//   bytes 3-5 = from unit - 999 (12 bits), to unit - 999 (12 bits).
//   FleetSync II sends each block's 4 nibbles as 4 BCH(15,7) codewords
//   (x^8 + x^4 + x^2 + x + 1, bit 14 = x^0), 4 blocks per FleetSync block.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

namespace fleetsync {

constexpr uint32_t SYNC_A = 0xAAAA23EBu, SYNC_B = 0xAAAA052Bu;
constexpr int SYNC_BITS = 32;

// A block as received, bit 63 = the first bit after the sync: an MPT 1327
// codeword - information bits 63..16, check bits 15..1 = the remainder of
// info(x) x^15 mod g(x), g = x^15 + x^14 + x^13 + x^11 + x^4 + x^2 + 1
// (0xE815), with its lowest bit inverted, and bit 0 making the number of
// ones even
inline bool block_ok(uint64_t cw) {
    if (std::popcount(cw) & 1) return false;
    uint64_t r = (cw >> 16) << 15;
    for (int i = 62; i >= 15; i--)
        if ((r >> i) & 1) r ^= static_cast<uint64_t>(0xE815) << (i - 15);
    return ((r ^ 1) & 0x7FFF) == ((cw >> 1) & 0x7FFF);
}

// a checked block's 6 data bytes onto the message
inline void append_data(uint64_t cw, std::vector<uint8_t>& out) {
    for (int k = 0; k < 6; k++) out.push_back(static_cast<uint8_t>(cw >> (56 - 8 * k)));
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
    // a payload (type 0x42: text / data): its length in message bytes 9-10
    // (big-endian), the text in the blocks from the third on (message byte
    // 12 on), each block's 6 bytes last one first - the layout as the public
    // decoders read it (not confirmed on a real radio)
    std::string payload() const {
        if ((type() & 0xFE) != 0x42 || bytes.size() < 11) return "";
        const size_t want = std::min<size_t>(static_cast<size_t>((bytes[9] << 8) | bytes[10]), 256);
        std::string p;
        for (size_t blk = 12; blk + 6 <= bytes.size() && p.size() < want; blk += 6)
            for (int k = 5; k >= 0 && p.size() < want; k--) p += static_cast<char>(bytes[blk + k]);
        return p;
    }
};

inline std::string unit_name(int fleet, int unit) {
    auto part = [](int v, int none) { return v == none || v < 0 ? std::string("all") : std::to_string(v); };
    return part(fleet, 99) + "-" + part(unit, 999);
}

}  // namespace fleetsync
