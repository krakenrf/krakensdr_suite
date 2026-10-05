#pragma once

// Forward error correction, CRCs and bit helpers shared by the digital voice
// decoders (DMR, P25, D-STAR, TETRA). Bits are handled UNPACKED - one bit per
// uint8_t, value 0/1, first transmitted bit first - which keeps the protocol
// code close to the bit tables in the standards. Small block codes are
// decoded by exhaustive nearest-codeword search (at most 2^16 codewords,
// precomputed once): maximum-likelihood, no per-code syndrome tables to get
// wrong, and fast enough at the few-hundred-words-per-second rates involved.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace dig {

using Bits = std::vector<uint8_t>;

// MSB-first bit field -> integer (n <= 64)
uint64_t bits_to_u64(const uint8_t* b, int n);
inline uint32_t bits_to_u32(const uint8_t* b, int n) { return static_cast<uint32_t>(bits_to_u64(b, n)); }
void u64_to_bits(uint64_t v, int n, uint8_t* out);
// Pack MSB-first bits into bytes (len bits, multiple of 8)
void pack_bits(const uint8_t* b, int nbits, uint8_t* bytes);
int popcount64(uint64_t v);

// --- Linear block code, nearest-codeword decoding ---------------------------
// Codeword integer layout: first transmitted bit = bit (n-1).
class BlockCode {
public:
    // enc maps a k-bit message to the n-bit codeword (same layout)
    template <typename Enc>
    BlockCode(int n, int k, Enc enc) : n_(n), k_(k) {
        cw_.resize(size_t(1) << k);
        for (uint32_t m = 0; m < cw_.size(); m++) cw_[m] = enc(m);
    }
    // Returns the number of corrected bit errors, or -1 if the nearest
    // codeword is more than max_errors away. *msg receives the message.
    int decode(uint32_t rx, uint32_t* msg, int max_errors) const;
    uint32_t encode(uint32_t m) const { return cw_[m]; }
    int n() const { return n_; }
    int k() const { return k_; }
private:
    int n_, k_;
    std::vector<uint32_t> cw_;
};

// Golay(24,12,8) as used by P25 (and Golay(20,8,7) = DMR slot type, the same
// code with the top 4 message bits zero): data(12) | r(11) | parity(1), where
// r = data * x^11 mod (x^11+x^10+x^6+x^5+x^4+x^2+1)
const BlockCode& golay24();
const BlockCode& golay20();      // n=20, k=8 (DMR slot type)
const BlockCode& golay18();      // n=18, k=6 (P25 HDU, shortened Golay)
const BlockCode& qr16_7();       // DMR EMB
const BlockCode& hamming_15_11(); // DMR BPTC rows
const BlockCode& hamming_13_9();  // DMR BPTC columns
const BlockCode& hamming_16_11(); // DMR embedded LC rows
const BlockCode& hamming_10_6();  // P25 LDU link control words
const BlockCode& hamming_7_4();   // DMR CACH TACT
const BlockCode& rm_30_14();      // TETRA AACH Reed-Muller

// --- Reed-Solomon over GF(2^6), P25 (poly x^6+x+1, fcr 1) -------------------
// Shortened code, symbol 0 = highest-degree coefficient (data first, parity
// last). Corrects in place; returns the number of corrected symbols or -1.
int rs64_decode(uint8_t* sym, int n, int nroots);
void rs64_encode(uint8_t* sym, int n, int nroots);   // fills the last nroots

// DMR RS(12,9) over GF(2^8) (poly 0x11D), check only (BPTC already corrected
// the bits). data = 12 bytes after the CRC mask was removed.
bool rs129_check(const uint8_t* data12);

// --- CRCs ---------------------------------------------------------------------
// CRC-16/CCITT, MSB-first, bitwise over an unpacked bit array
uint16_t crc16_ccitt_bits(const uint8_t* bits, int nbits, uint16_t init);
// CRC-16/X-25 (reflected 0x8408, init/xorout 0xFFFF) over bytes - D-STAR header
uint16_t crc16_x25(const uint8_t* data, size_t len);
// CRC-16/CCITT MSB-first over bytes (init, final ~) - P25 TSBK, DMR CSBK
uint16_t crc16_ccitt_bytes(const uint8_t* data, size_t len, uint16_t init);
// Generic MSB-first CRC over unpacked bits, register initialised to all ones
// (NXDN CRC-6 0x27, CRC-12 0x80F, CRC-15 0x4CC5)
uint32_t crc_bits_ones(const uint8_t* bits, int nbits, int width, uint32_t poly);
// DMR embedded LC 5-bit checksum (sum of the 9 LC bytes mod 31)
int dmr_crc5(const uint8_t* lc_bits72);

// --- Convolutional codes ------------------------------------------------------
// Soft-decision Viterbi for a rate-1/N code. soft[i] for each coded bit:
// > 0 means "0", < 0 means "1", 0 = erasure (punctured). polys are tap masks
// over (input, D, D^2, ...), bit 0 = input. nout data bits are returned;
// the encoder is assumed to start in state 0 (and end there if terminated).
Bits viterbi_decode(const float* soft, int nout, int K, const std::vector<uint32_t>& polys,
                    bool terminated);
Bits conv_encode(const uint8_t* bits, int n, int K, const std::vector<uint32_t>& polys);

// P25 rate-1/2 trellis (TSBK / PDU headers): 98 received symbols (soft, in
// level units +-1/+-3, interleaved order) -> 12 bytes. Returns the path
// metric (0 = clean) via *metric.
bool p25_trellis_half(const float* sym98, uint8_t* out12, float* metric);

// --- DMR ----------------------------------------------------------------------
// BPTC(196,96): 196 info bits (both burst halves joined) -> 96 data bits.
// Returns corrected bit count, or -1 if a row/column stayed uncorrectable.
int bptc_196_96(const uint8_t* in196, uint8_t* out96);
// Embedded LC: 128 bits gathered from voice bursts B..E -> 72 LC bits.
bool dmr_embedded_lc(const uint8_t* in128, uint8_t* out72);

// --- TETRA lower MAC ----------------------------------------------------------
void tetra_scramble(uint8_t* bits, int n, uint32_t init);   // XOR in place
uint32_t tetra_scramb_init(uint32_t mcc, uint32_t mnc, uint32_t cc);
void tetra_block_deinterleave(const uint8_t* in, uint8_t* out, int K, int a);
// type-3 (rate 2/3 punctured) bits -> type-2 bits through the rate-1/4
// mother code; soft input (>0 = 0). Returns type2 bits (n2).
Bits tetra_rcpc23_decode(const float* soft3, int n3, int n2);
// CRC check of type-2 bits (k1 info + 16 CRC)
bool tetra_crc_ok(const uint8_t* type2, int k1);

}  // namespace dig
