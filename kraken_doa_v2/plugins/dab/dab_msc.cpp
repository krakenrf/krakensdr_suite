#include "dab_msc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dab {

// clause 12, table 21: bit i of the interleaver output in CIF r comes from logical frame r - D[i mod 16]
static const int TI_DELAY[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

bool MscDecoder::Coding::build(const Protection& p) {
    Profile pr;
    nbits = 0;
    if (!coding_profile(p, pr) || !dp.build(pr)) return false;
    if (static_cast<int>(dp.pos.size()) != 64 * p.size_cu) return false;
    prot = p;
    nbits = 64 * p.size_cu;
    return true;
}

bool MscDecoder::configure(int start_cu, const Protection& p) {
    if (active() && !pend_ && start_cu == a_cur_.start && p == cur_.prot) return true;
    clear();
    if (start_cu < 0 || start_cu + p.size_cu > 864 || !cur_.build(p)) {
        cur_.nbits = 0;
        return false;
    }
    a_cur_ = {start_cu, p.size_cu};
    a_last_ = a_cur_;
    if (ring_.empty()) ring_.assign(static_cast<size_t>(16) * CIF_BITS, 0);
    reset();
    return true;
}

bool MscDecoder::reconfigure(int start_cu, const Protection& p, int64_t r0) {
    if (!active()) return configure(start_cu, p);
    if (pend_) {   // the previous one is not complete yet: take it over at once
        cur_ = nxt_;
        a_cur_ = a_nxt_;
        pend_ = false;
    }
    if (start_cu == a_cur_.start && p == cur_.prot) return true;
    if (start_cu < 0 || start_cu + p.size_cu > 864 || !nxt_.build(p)) {
        clear();
        return false;
    }
    a_nxt_ = {start_cu, p.size_cu};
    r0_ = r0;
    // annex D e): a sub-channel that shrinks is coded with the new profile from r0 - 15
    lf0_ = p.size_cu < cur_.prot.size_cu ? r0 - 15 : r0;
    pend_ = true;
    return true;
}

void MscDecoder::reset() {
    for (auto& s : slot_cif_) s = -1000;
    first_cif_ = -1;
}

void MscDecoder::clear() {
    cur_.nbits = nxt_.nbits = 0;
    cur_.prot = nxt_.prot = Protection{};
    a_cur_ = a_nxt_ = a_last_ = Alloc{};
    pend_ = false;
    reset();
}

void MscDecoder::alloc(int64_t cif, int* start, int* size) const {
    const Alloc& a = pend_ && cif >= r0_ ? a_nxt_ : a_cur_;
    *start = a.start;
    *size = a.size;
}

int MscDecoder::push_cif(const int8_t* bits, int64_t cif) {
    if (!active()) return 0;
    if (first_cif_ < 0) first_cif_ = cif;
    const Alloc& a = pend_ && cif >= r0_ ? a_nxt_ : a_cur_;
    a_last_ = a;
    const int len = 64 * a.size;
    // a bit position beyond a CIF's allocation reads as zero (padding / erasure)
    const int span = std::max({cur_.nbits, pend_ ? nxt_.nbits : 0, len});
    const int slot = static_cast<int>(cif & 15);
    int8_t* dst = &ring_[static_cast<size_t>(slot) * CIF_BITS];
    if (bits) {
        std::memcpy(dst, bits, static_cast<size_t>(len));
        if (span > len) std::memset(dst + len, 0, static_cast<size_t>(span - len));
        slot_cif_[slot] = cif;
    } else {
        slot_cif_[slot] = -1000;
    }
    // logical frame cif - 15 is complete now, coded with the profile in force for it
    const int64_t r = cif - 15;
    const Coding& c = pend_ && r >= lf0_ ? nxt_ : cur_;
    const int res = decode(c, r);
    // the reconfiguration is complete once both the allocation and the coding have moved
    if (pend_ && cif >= r0_ && r >= lf0_) {
        cur_ = nxt_;
        a_cur_ = a_nxt_;
        pend_ = false;
    }
    return res;
}

int MscDecoder::decode(const Coding& c, int64_t r) {
    const int nb = c.nbits;
    const int8_t* src[16];
    int present = 0;
    for (int dd = 0; dd < 16; dd++) {
        const int s = static_cast<int>((r + dd) & 15);
        const bool ok = slot_cif_[s] == r + dd;
        present += ok;
        src[dd] = ok ? &ring_[static_cast<size_t>(s) * CIF_BITS] : nullptr;
    }
    // Missing CIFs are erasures. Decoded only while the bits present still carry the
    // information with some margin (code rate / share present <= 0.8; at least half
    // the CIFs): EEP 2-A (rate 3/8) needs 8 of the 16, EEP 3-A (1/2) 10, EEP 4-A (3/4) 15.
    // A frame that began before the first CIF pushed is not due yet.
    const int need = std::clamp(static_cast<int>(std::ceil(16.0 * c.dp.in_bits / (0.8 * nb))), 8, 12);
    if (present < need) return r < first_cif_ ? 0 : -1;
    if (frame_.size() < static_cast<size_t>(nb)) frame_.assign(static_cast<size_t>(nb), 0);
    // missing CIFs: erasures (a zero slot)
    if (zero_.empty()) zero_.assign(static_cast<size_t>(CIF_BITS), 0);
    for (int dd = 0; dd < 16; dd++)
        if (!src[dd]) src[dd] = zero_.data();
    int8_t* __restrict f = frame_.data();
    for (int i = 0; i < nb; i++) f[i] = src[TI_DELAY[i & 15]][i];
    mother_.resize(static_cast<size_t>(c.dp.mother));
    dec_.resize(static_cast<size_t>(c.dp.in_bits));
    reenc_.resize(static_cast<size_t>(c.dp.mother));
    c.dp.apply(f, mother_.data());
    vit_.decode(mother_.data(), c.dp.in_bits, dec_.data());
    // channel bit errors (re-encoded)
    conv_encode(dec_.data(), c.dp.in_bits, reenc_.data());
    last_bits = last_bit_errors = 0;
    for (int i = 0; i < nb; i++) {
        const int p = c.dp.pos[static_cast<size_t>(i)];
        if (p < 0 || f[i] == 0) continue;
        last_bits++;
        last_bit_errors += (f[i] < 0) != (reenc_[static_cast<size_t>(p)] != 0);
    }
    const auto& prbs = prbs_bytes();
    const int nbytes = c.dp.in_bits / 8;
    out_.resize(static_cast<size_t>(nbytes));
    for (int b = 0; b < nbytes; b++) {
        unsigned v = 0;
        for (int i = 0; i < 8; i++) v = (v << 1) | dec_[static_cast<size_t>(8 * b + i)];
        out_[static_cast<size_t>(b)] = static_cast<uint8_t>(v ^ prbs[static_cast<size_t>(b)]);
    }
    out_bytes_ = nbytes;
    return 1;
}

}   // namespace dab
