#pragma once
// One MSC sub-channel (EN 300 401 clauses 12, 11.3, 10.3): its CUs out of
// every CIF, time de-interleaving over 16 CIFs, de-puncturing (UEP / EEP
// profile), Viterbi, energy dispersal removed -> one logical frame (24 ms,
// kbps * 3 bytes) per CIF.

#include "dab_fec.hpp"
#include "dab_tables.hpp"

#include <cstdint>
#include <vector>

namespace dab {

class MscDecoder {
public:
    // false = an invalid profile (nothing decoded)
    bool configure(int start_cu, const Protection& p);
    void reset();   // keeps the configuration
    void clear();   // no sub-channel
    bool active() const { return size_cu_ > 0; }
    int start_cu() const { return start_; }
    int size_cu() const { return size_cu_; }
    const Protection& protection() const { return prot_; }
    int frame_bytes() const { return dp_.in_bits / 8; }

    // the sub-channel's soft bits of the CIF with absolute index cif (size_cu * 64),
    // nullptr = this CIF was not received. Returns 1 when a logical frame was
    // decoded into out (frame_bytes()), -1 when one was due but too much of it
    // was missing (lost), 0 while the de-interleaver fills.
    int push_cif(const int8_t* bits, int64_t cif, uint8_t* out);
    // channel bit errors of the last decoded frame (re-encoded)
    int last_bit_errors = 0, last_bits = 0;
private:
    Protection prot_;
    int start_ = 0, size_cu_ = 0, nbits_ = 0;
    Depuncture dp_;
    Viterbi vit_;
    std::vector<int8_t> ring_;          // 16 CIFs x nbits_
    int64_t slot_cif_[16];
    std::vector<int8_t> frame_, mother_;
    std::vector<uint8_t> dec_, reenc_;
    std::vector<int8_t> zero_;
};

}   // namespace dab
