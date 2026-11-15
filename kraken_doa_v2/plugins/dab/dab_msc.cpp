#include "dab_msc.hpp"

#include <algorithm>
#include <cstring>

namespace dab {

// clause 12, table 21: bit i of the interleaver output in CIF r comes from logical frame r - D[i mod 16]
static const int TI_DELAY[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

bool MscDecoder::configure(int start_cu, const Protection& p) {
    if (active() && start_cu == start_ && p == prot_) return true;
    clear();
    Profile pr;
    if (!coding_profile(p, pr) || !dp_.build(pr)) return false;
    if (static_cast<int>(dp_.pos.size()) != 64 * p.size_cu || start_cu < 0 || start_cu + p.size_cu > 864) return false;
    prot_ = p;
    start_ = start_cu;
    size_cu_ = p.size_cu;
    nbits_ = 64 * p.size_cu;
    ring_.assign(static_cast<size_t>(16) * static_cast<size_t>(nbits_), 0);
    frame_.assign(static_cast<size_t>(nbits_), 0);
    mother_.assign(static_cast<size_t>(dp_.mother), 0);
    dec_.assign(static_cast<size_t>(dp_.in_bits), 0);
    reenc_.assign(static_cast<size_t>(dp_.mother), 0);
    reset();
    return true;
}

void MscDecoder::reset() {
    for (auto& s : slot_cif_) s = -1000;
}

void MscDecoder::clear() {
    size_cu_ = 0;
    nbits_ = 0;
    prot_ = Protection{};
    reset();
}

int MscDecoder::push_cif(const int8_t* bits, int64_t cif, uint8_t* out) {
    if (!active()) return 0;
    const int slot = static_cast<int>(cif & 15);
    if (bits) {
        std::memcpy(&ring_[static_cast<size_t>(slot) * static_cast<size_t>(nbits_)], bits, static_cast<size_t>(nbits_));
        slot_cif_[slot] = cif;
    } else {
        slot_cif_[slot] = -1000;
    }
    // logical frame cif - 15 is complete now
    const int64_t r = cif - 15;
    const int8_t* src[16];
    int present = 0;
    for (int dd = 0; dd < 16; dd++) {
        const int s = static_cast<int>((r + dd) & 15);
        const bool ok = slot_cif_[s] == r + dd;
        present += ok;
        src[dd] = ok ? &ring_[static_cast<size_t>(s) * static_cast<size_t>(nbits_)] : nullptr;
    }
    if (present < 12) return present == 0 || slot_cif_[static_cast<int>(r & 15)] < -1 ? 0 : -1;
    if (zero_.size() < static_cast<size_t>(nbits_)) zero_.assign(static_cast<size_t>(nbits_), 0);
    for (int dd = 0; dd < 16; dd++)
        if (!src[dd]) src[dd] = zero_.data();
    int8_t* __restrict f = frame_.data();
    for (int i = 0; i < nbits_; i++) f[i] = src[TI_DELAY[i & 15]][i];
    dp_.apply(f, mother_.data());
    vit_.decode(mother_.data(), dp_.in_bits, dec_.data());
    // channel bit errors (re-encoded)
    conv_encode(dec_.data(), dp_.in_bits, reenc_.data());
    last_bits = last_bit_errors = 0;
    for (int i = 0; i < nbits_; i++) {
        const int p = dp_.pos[static_cast<size_t>(i)];
        if (p < 0 || f[i] == 0) continue;
        last_bits++;
        last_bit_errors += (f[i] < 0) != (reenc_[static_cast<size_t>(p)] != 0);
    }
    const auto& prbs = prbs_bytes();
    const int nbytes = dp_.in_bits / 8;
    for (int b = 0; b < nbytes; b++) {
        unsigned v = 0;
        for (int i = 0; i < 8; i++) v = (v << 1) | dec_[static_cast<size_t>(8 * b + i)];
        out[b] = static_cast<uint8_t>(v ^ prbs[static_cast<size_t>(b)]);
    }
    return 1;
}

}   // namespace dab
