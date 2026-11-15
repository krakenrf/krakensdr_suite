#pragma once
// One MSC sub-channel (EN 300 401 clauses 12, 11.3, 10.3): its CUs out of
// every CIF, time de-interleaving over 16 CIFs, de-puncturing (UEP / EEP
// profile), Viterbi, energy dispersal removed -> one logical frame (24 ms,
// kbps * 3 bytes) per CIF.
//
// A multiplex reconfiguration (clause 6.5, annex D) is applied seamlessly:
// the new CU allocation holds for the CIFs from the reconfiguration CIF r0 on,
// the new protection profile for the logical frames from r0 on - from r0 - 15
// when the sub-channel shrinks (the encoder switches 15 CIFs early so that
// the bits it delays by up to 15 CIFs still fit the old, larger allocation).
// The de-interleaver takes bit i of logical frame r from CIF r + D(i), at the
// same bit position, whatever that CIF's allocation: positions beyond a
// CIF's allocation are zero padding (annex D).

#include "dab_fec.hpp"
#include "dab_tables.hpp"

#include <cstdint>
#include <vector>

namespace dab {

class MscDecoder {
public:
    // A new sub-channel from the next CIF on (the de-interleaver starts empty).
    // false = an invalid profile (nothing decoded)
    bool configure(int start_cu, const Protection& p);
    // A multiplex reconfiguration of the same sub-channel at CIF r0 (call it
    // before pushing CIF r0): see above. false = an invalid profile (then the
    // sub-channel is cleared).
    bool reconfigure(int start_cu, const Protection& p, int64_t r0);
    void reset();   // keeps the configuration
    void clear();   // no sub-channel
    bool active() const { return cur_.nbits > 0; }
    // the allocation of the CIF most recently pushed (or the current one)
    int start_cu() const { return a_last_.start; }
    int size_cu() const { return a_last_.size; }
    // the CU allocation of CIF cif (the scheduled one from r0 on)
    void alloc(int64_t cif, int* start, int* size) const;
    // the protection of the logical frames decoded from now on
    const Protection& protection() const { return pend_ ? nxt_.prot : cur_.prot; }

    // the sub-channel's soft bits of CIF cif (64 x its allocation's CUs, see
    // alloc()), nullptr = this CIF was not received (erasures). CIFs are
    // pushed in order; a skipped CIF counts as not received. Returns 1 when a
    // logical frame was decoded (frame(), frame_bytes()), -1 when one was due
    // but too much of it was missing (lost), 0 while the de-interleaver fills.
    int push_cif(const int8_t* bits, int64_t cif);
    const uint8_t* frame() const { return out_.data(); }
    int frame_bytes() const { return out_bytes_; }
    // channel bit errors of the last decoded frame (re-encoded)
    int last_bit_errors = 0, last_bits = 0;

private:
    struct Alloc {
        int start = 0, size = 0;
    };
    struct Coding {
        Protection prot;
        Depuncture dp;
        int nbits = 0;   // = 64 x its size in CUs
        bool build(const Protection& p);
    };
    Coding cur_, nxt_;
    Alloc a_cur_, a_nxt_, a_last_;
    bool pend_ = false;
    int64_t r0_ = 0;              // CIF of the new allocation
    int64_t lf0_ = 0;             // first logical frame coded with nxt_
    int64_t first_cif_ = -1;      // first CIF pushed since configure / reset
    std::vector<int8_t> ring_;    // 16 CIFs x CIF_BITS
    int64_t slot_cif_[16];
    Viterbi vit_;
    std::vector<int8_t> frame_, mother_, zero_;
    std::vector<uint8_t> dec_, reenc_, out_;
    int out_bytes_ = 0;
    int decode(const Coding& c, int64_t r);
};

}   // namespace dab
