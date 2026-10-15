// LoRa PHY coding chain (symbols <-> bytes): Gray mapping, diagonal
// interleaving, Hamming FEC, whitening, payload CRC16 and the explicit
// header, implemented clean-room from the public reverse-engineering
// literature (Knight; Robyns et al.; Tapparel "Complete Reverse Engineering
// of LoRa PHY", EPFL; Seller/Sornin Semtech patents / AN1200.22). No GPL
// code is copied - gr-lora / gr-lora_sdr were read only as documentation.
//
// The matching transmit chain lives in tools/lora_gen.py; decoder and
// generator are verified bit-exact by round trip. The constants and bit
// orders here are an internally consistent realisation of the published
// chain and are checked against real captures where available.
#pragma once
#include <cstdint>
#include <vector>

namespace lora_code {

// ---- Gray ----
inline uint32_t gray_encode(uint32_t v) { return v ^ (v >> 1); }
inline uint32_t gray_decode(uint32_t g) {
    for (uint32_t m = g >> 1; m; m >>= 1) g ^= m;
    return g;
}

// ---- Hamming (4 data bits -> 4+CR bits), CR = 1..4 (4/5 .. 4/8) ----
// Data nibble bits d0(LSB)..d3. Parity equations as documented for LoRa.
// Codeword bit order (LSB..MSB): d0 d1 d2 d3 p0 [p1 [p2 [p3]]].
inline uint8_t hamming_encode(uint8_t data, int cr) {
    const int d0 = data & 1, d1 = (data >> 1) & 1, d2 = (data >> 2) & 1, d3 = (data >> 3) & 1;
    const int p0 = d0 ^ d1 ^ d2;         // 4/5 single parity
    const int p1 = d1 ^ d2 ^ d3;
    const int p2 = d0 ^ d1 ^ d3;
    const int p3 = d0 ^ d2 ^ d3;         // 4/8 extra parity
    uint8_t cw = static_cast<uint8_t>(data & 0xF);
    if (cr >= 1) cw |= static_cast<uint8_t>(p0 << 4);
    if (cr >= 2) cw |= static_cast<uint8_t>(p1 << 5);
    if (cr >= 3) cw |= static_cast<uint8_t>(p2 << 6);
    if (cr >= 4) cw |= static_cast<uint8_t>(p3 << 7);
    return cw;
}

// Decode a (4+cr)-bit codeword -> 4-bit nibble. ok: parity consistent (CR 1/2)
// or corrected (CR 3/4). For CR 3/4 a single-bit error is corrected.
inline uint8_t hamming_decode(uint8_t cw, int cr, bool* ok, bool* corrected = nullptr) {
    if (corrected) *corrected = false;
    const int n = 4 + cr;
    if (cr <= 2) {
        // parity / detect only: check what parity bits we have, no correction
        const int d0 = cw & 1, d1 = (cw >> 1) & 1, d2 = (cw >> 2) & 1, d3 = (cw >> 3) & 1;
        bool good = true;
        if (cr >= 1) good = good && ((cw >> 4 & 1) == (d0 ^ d1 ^ d2));
        if (cr >= 2) good = good && ((cw >> 5 & 1) == (d1 ^ d2 ^ d3));
        if (ok) *ok = good;
        return static_cast<uint8_t>(cw & 0xF);
    }
    // CR 3 (4/7) or 4 (4/8): try the codeword and all single-bit flips,
    // accept the one whose parity is consistent.
    auto parity_ok = [cr](uint8_t c) {
        const int d0 = c & 1, d1 = (c >> 1) & 1, d2 = (c >> 2) & 1, d3 = (c >> 3) & 1;
        if ((c >> 4 & 1) != (d0 ^ d1 ^ d2)) return false;
        if ((c >> 5 & 1) != (d1 ^ d2 ^ d3)) return false;
        if (cr >= 3 && (c >> 6 & 1) != (d0 ^ d1 ^ d3)) return false;
        if (cr >= 4 && (c >> 7 & 1) != (d0 ^ d2 ^ d3)) return false;
        return true;
    };
    if (parity_ok(cw)) {
        if (ok) *ok = true;
        return static_cast<uint8_t>(cw & 0xF);
    }
    for (int b = 0; b < n; b++) {
        const uint8_t t = static_cast<uint8_t>(cw ^ (1u << b));
        if (parity_ok(t)) {
            if (ok) *ok = true;
            if (corrected) *corrected = true;
            return static_cast<uint8_t>(t & 0xF);
        }
    }
    if (ok) *ok = false;   // uncorrectable (>=2 errors)
    return static_cast<uint8_t>(cw & 0xF);
}

// ---- Diagonal (de)interleaver ----
// One block: nsym = 4+cr symbols of sf_app bits each  <->  sf_app codewords
// of (4+cr) bits each. Diagonal offset (i+j) mod sf_app. Bit 0 = LSB.
inline void deinterleave(const uint16_t* sym, int cr, int sf_app, uint8_t* cw_out) {
    const int nsym = 4 + cr;
    for (int j = 0; j < sf_app; j++) {
        uint8_t cw = 0;
        for (int i = 0; i < nsym; i++) {
            const int bit = (sym[i] >> ((i + j) % sf_app)) & 1;
            cw |= static_cast<uint8_t>(bit << (nsym - 1 - i));
        }
        cw_out[j] = cw;
    }
}
inline void interleave(const uint8_t* cw, int cr, int sf_app, uint16_t* sym_out) {
    const int nsym = 4 + cr;
    for (int i = 0; i < nsym; i++) sym_out[i] = 0;
    for (int j = 0; j < sf_app; j++)
        for (int i = 0; i < nsym; i++) {
            const int bit = (cw[j] >> (nsym - 1 - i)) & 1;
            sym_out[i] |= static_cast<uint16_t>(bit << ((i + j) % sf_app));
        }
}

// ---- Whitening (payload only; the header is never whitened) ----
// 8-bit LFSR, seed 0xFF, feedback poly 0xB8 (x^8+x^6+x^5+x^4+1, reflected).
struct Whitening {
    uint8_t s = 0xFF;
    void reset() { s = 0xFF; }
    uint8_t next() {
        const uint8_t out = s;
        for (int k = 0; k < 8; k++) {
            const uint8_t fb = s & 1;
            s >>= 1;
            if (fb) s ^= 0xB8;
        }
        return out;
    }
};

// ---- CRC16 over the payload (LoRa: poly 0x1021, as Semtech's) ----
inline uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(p[i]) << 8;
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

// ---- Explicit header (5 nibbles) ----
// n0 n1 = payload length byte (n1 high); n2 = CR<<1 | has_crc; n3 n4 = the
// 5-bit header checksum (documented equations over the 12 header data bits).
struct Header {
    int length = 0;
    int cr = 1;
    bool has_crc = false;
    bool valid = false;
};
inline uint8_t header_checksum(uint8_t n0, uint8_t n1, uint8_t n2) {
    // 12 data bits: n0 (payload len high nibble), n1 (low nibble), n2 (cr|crc).
    const int b[12] = {(n0 >> 3) & 1, (n0 >> 2) & 1, (n0 >> 1) & 1, n0 & 1,
                       (n1 >> 3) & 1, (n1 >> 2) & 1, (n1 >> 1) & 1, n1 & 1,
                       (n2 >> 3) & 1, (n2 >> 2) & 1, (n2 >> 1) & 1, n2 & 1};
    const int c4 = b[0] ^ b[1] ^ b[2] ^ b[3];
    const int c3 = b[0] ^ b[4] ^ b[5] ^ b[6] ^ b[8];
    const int c2 = b[1] ^ b[4] ^ b[7] ^ b[8] ^ b[9] ^ b[10];
    const int c1 = b[2] ^ b[5] ^ b[7] ^ b[8] ^ b[9] ^ b[11];
    const int c0 = b[3] ^ b[6] ^ b[7] ^ b[9] ^ b[10] ^ b[11];
    return static_cast<uint8_t>((c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0);
}

}  // namespace lora_code
