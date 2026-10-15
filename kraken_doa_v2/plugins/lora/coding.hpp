// LoRa PHY coding chain (symbols <-> bytes) as Semtech's radios use it:
// Gray mapping, diagonal interleaving, Hamming FEC, whitening, payload CRC16
// and the explicit header. Clean-room from the public reverse-engineering
// literature (Tapparel et al., "Complete Reverse Engineering of LoRa PHY",
// EPFL 2020; Robyns et al.; Knight; Semtech AN1200.22 / SX127x datasheets).
// No GPL code is copied - gr-lora_sdr / LoRaPHY were read as documentation.
//
// Checked on REAL packets (2026-10-15: AU915 LoRaWAN gateway downlinks,
// 927.5 MHz SF10 BW500, inverted IQ): every header passes its checksum and
// all eight header codewords, the payload codewords their parity, and
// consecutive frames carry consecutive FCnt values. The first version of
// this file was only a self-consistent guess (whitening, diagonal
// direction, Gray direction, codeword bit order, header checksum, CRC were
// all different from the radios') - it decoded its own generator only.
//
// Conventions (symbol i of a block, codeword k, sf_app bits per symbol):
//   value  = (bin - 1) mod 2^SF relative to the preamble's bin (data), or
//            round((bin - 1) / 4) at reduced rate (header block, LDRO)
//   bits   = gray_map(value) = value ^ (value >> 1)
//   cw[k] bit (cw_len - 1 - i) = bit ((k - i) mod sf_app) of symbol i
//   cw bits from the first symbol: d0 d1 d2 d3 p0 [p1 p2 p3] (d0 = nibble LSB)
//   4/5: p = d0^d1^d2^d3; 4/6-4/8: p0 = d0^d1^d2, p1 = d1^d2^d3,
//        p2 = d0^d1^d3, p3 = d0^d2^d3
//   nibbles -> bytes low nibble first; payload whitened (not the header,
//   not the CRC) with the LFSR w0 = 0xFF, w' = (w << 1) | (w7^w5^w4^w3)
//   CRC = CRC16-CCITT (0x1021, init 0) of the first n-2 bytes, XOR the last
//   two bytes (byte n-1 low, n-2 high); sent low byte first
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace lora_code {

inline uint32_t gray_map(uint32_t v) { return v ^ (v >> 1); }      // receiver side
inline uint32_t gray_unmap(uint32_t g) {                           // transmitter side
    for (uint32_t m = g >> 1; m; m >>= 1) g ^= m;
    return g;
}

// ---- Hamming: nibble <-> (4 + cr)-bit codeword, bit (cw_len-1) first ----
inline uint8_t hamming_encode(uint8_t data, int cr) {
    const int d0 = data & 1, d1 = (data >> 1) & 1, d2 = (data >> 2) & 1, d3 = (data >> 3) & 1;
    if (cr == 1) return static_cast<uint8_t>(d0 << 4 | d1 << 3 | d2 << 2 | d3 << 1 | (d0 ^ d1 ^ d2 ^ d3));
    const int p0 = d0 ^ d1 ^ d2, p1 = d1 ^ d2 ^ d3, p2 = d0 ^ d1 ^ d3, p3 = d0 ^ d2 ^ d3;
    const int full = d0 << 7 | d1 << 6 | d2 << 5 | d3 << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3;
    return static_cast<uint8_t>(full >> (4 - cr));
}

// Decode a (4 + cr)-bit codeword -> nibble. ok: the parity holds (4/5, 4/6:
// detection only) or a single-bit error was corrected (4/7, 4/8).
inline uint8_t hamming_decode(uint8_t cw, int cr, bool* ok, bool* corrected = nullptr) {
    if (corrected) *corrected = false;
    const int n = 4 + cr;
    auto nib = [n](uint8_t c) {
        const int d0 = (c >> (n - 1)) & 1, d1 = (c >> (n - 2)) & 1, d2 = (c >> (n - 3)) & 1, d3 = (c >> (n - 4)) & 1;
        return static_cast<uint8_t>(d0 | d1 << 1 | d2 << 2 | d3 << 3);
    };
    auto valid = [cr](uint8_t c, uint8_t d) { return hamming_encode(d, cr) == c; };
    const uint8_t d = nib(cw);
    if (valid(cw, d)) {
        if (ok) *ok = true;
        return d;
    }
    if (cr >= 3) {
        for (int b = 0; b < n; b++) {
            const uint8_t t = static_cast<uint8_t>(cw ^ (1u << b));
            if (valid(t, nib(t))) {
                if (ok) *ok = true;
                if (corrected) *corrected = true;
                return nib(t);
            }
        }
    }
    if (ok) *ok = false;
    return d;
}

// ---- Diagonal (de)interleaver: cw_len = 4 + cr symbols <-> sf_app codewords ----
inline int imod(int a, int m) { return ((a % m) + m) % m; }
inline void deinterleave(const uint16_t* sym, int cr, int sf_app, uint8_t* cw_out) {
    const int n = 4 + cr;
    for (int k = 0; k < sf_app; k++) {
        uint8_t cw = 0;
        for (int i = 0; i < n; i++)
            cw |= static_cast<uint8_t>(((sym[i] >> imod(k - i, sf_app)) & 1) << (n - 1 - i));
        cw_out[k] = cw;
    }
}
inline void interleave(const uint8_t* cw, int cr, int sf_app, uint16_t* sym_out) {
    const int n = 4 + cr;
    for (int i = 0; i < n; i++) {
        uint16_t s = 0;
        for (int k = 0; k < sf_app; k++) s |= static_cast<uint16_t>(((cw[k] >> (n - 1 - i)) & 1) << imod(k - i, sf_app));
        sym_out[i] = s;
    }
}

// ---- Whitening (payload bytes only) ----
struct Whitening {
    uint8_t s = 0xFF;
    void reset() { s = 0xFF; }
    uint8_t next() {
        const uint8_t out = s;
        const uint8_t fb = static_cast<uint8_t>(((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1);
        s = static_cast<uint8_t>((s << 1) | fb);
        return out;
    }
};

// ---- CRC16-CCITT (poly 0x1021, init 0) ----
inline uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(p[i]) << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}
// The value LoRa sends after the payload (low byte first)
inline uint16_t payload_crc(const uint8_t* p, size_t n) {
    if (n == 0) return 0;
    if (n == 1) return p[0];
    return static_cast<uint16_t>(crc16(p, n - 2) ^ p[n - 1] ^ (static_cast<uint16_t>(p[n - 2]) << 8));
}

// ---- Explicit header (5 nibbles): length high, length low, CR<<1|CRC, chk ----
inline uint8_t header_checksum(uint8_t n0, uint8_t n1, uint8_t n2) {
    const int b[12] = {(n0 >> 3) & 1, (n0 >> 2) & 1, (n0 >> 1) & 1, n0 & 1,
                       (n1 >> 3) & 1, (n1 >> 2) & 1, (n1 >> 1) & 1, n1 & 1,
                       (n2 >> 3) & 1, (n2 >> 2) & 1, (n2 >> 1) & 1, n2 & 1};
    const int c4 = b[0] ^ b[1] ^ b[2] ^ b[3];
    const int c3 = b[0] ^ b[4] ^ b[5] ^ b[6] ^ b[11];
    const int c2 = b[1] ^ b[4] ^ b[7] ^ b[8] ^ b[10];
    const int c1 = b[2] ^ b[5] ^ b[7] ^ b[9] ^ b[10] ^ b[11];
    const int c0 = b[3] ^ b[6] ^ b[8] ^ b[9] ^ b[10] ^ b[11];
    return static_cast<uint8_t>((c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0);
}

}  // namespace lora_code
