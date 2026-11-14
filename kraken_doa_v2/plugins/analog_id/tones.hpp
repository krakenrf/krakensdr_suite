#pragma once

// CTCSS tone list and DCS code helpers (ETSI TS 103 236 V1.1.1, "CTCSS and
// DCSS signalling system": table 1 = the CTCSS code frequencies, 4.2 = the
// DCS codeword, table 2 = the 83 DCS codes with their bit patterns).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ctcss {

// --- CTCSS ------------------------------------------------------------------
struct ToneDef {
    float hz;
    bool standard;     // in ETSI TS 103 236 table 1 / TIA-603 (else a tone radios offer beyond it)
};

// ETSI table 1 (38 EIA tones + 69.3) and the non-standard tones radios offer
// (150.0 = NATO; 159.8 ... 254.1 = the "NS" tones of most amateur / PMR radios)
inline const std::vector<ToneDef>& tone_table() {
    static const std::vector<ToneDef> t = {
        {67.0f, true},   {69.3f, true},   {71.9f, true},   {74.4f, true},   {77.0f, true},   {79.7f, true},
        {82.5f, true},   {85.4f, true},   {88.5f, true},   {91.5f, true},   {94.8f, true},   {97.4f, true},
        {100.0f, true},  {103.5f, true},  {107.2f, true},  {110.9f, true},  {114.8f, true},  {118.8f, true},
        {123.0f, true},  {127.3f, true},  {131.8f, true},  {136.5f, true},  {141.3f, true},  {146.2f, true},
        {150.0f, false}, {151.4f, true},  {156.7f, true},  {159.8f, false}, {162.2f, true},  {165.5f, false},
        {167.9f, true},  {171.3f, false}, {173.8f, true},  {177.3f, false}, {179.9f, true},  {183.5f, false},
        {186.2f, true},  {189.9f, false}, {192.8f, true},  {196.6f, false}, {199.5f, false}, {203.5f, true},
        {206.5f, false}, {210.7f, true},  {218.1f, true},  {225.7f, true},  {229.1f, false}, {233.6f, true},
        {241.8f, true},  {250.3f, true},  {254.1f, false}};
    return t;
}

// The nearest listed tone within tol (relative; encoders are within
// +-0.75 %), or nullptr
inline const ToneDef* nearest_tone(float hz, float tol = 0.011f) {
    const ToneDef* best = nullptr;
    float bd = 1e9f;
    for (const auto& t : tone_table()) {
        const float d = std::fabs(hz - t.hz) / t.hz;
        if (d < bd) { bd = d; best = &t; }
    }
    return bd <= tol ? best : nullptr;
}

inline std::string tone_name(float hz) {
    char b[16];
    snprintf(b, sizeof b, "%.1f", hz);
    return b;
}

// --- DCS --------------------------------------------------------------------
// A codeword is 23 bits sent at 134.4 bit/s, bit 1 first: bits 1-9 the code
// (3 octal digits, bit 1 = LSB), bits 10-12 fixed 0, 0, 1, bits 13-23 the
// check bits of the cyclic Golay (23,12) code. As a polynomial with bit 1 =
// x^0 ... bit 23 = x^22 every codeword is a multiple of
// g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75) - checked against
// all 83 bit patterns of the ETSI table. Polarity: a 1 is a positive
// frequency shift ("normal"); the same waveform read inverted is the
// "inverted" code (023N = 047I).
constexpr uint32_t GOLAY_G = 0xC75;
constexpr int DCS_BITS = 23;
constexpr uint32_t MASK23 = (1u << 23) - 1;

// w: bit i = the i-th bit in time order (bit 0 = sent first = x^0)
inline uint32_t golay_syndrome(uint32_t w) {
    for (int b = 22; b >= 11; b--)
        if (w & (1u << b)) w ^= GOLAY_G << (b - 11);
    return w & 0x7FF;
}
inline bool golay_ok(uint32_t w) { return golay_syndrome(w & MASK23) == 0; }

// rotate a 23-bit time-order word so that it starts r bits later
inline uint32_t rot23(uint32_t w, int r) {
    r %= 23;
    if (r == 0) return w & MASK23;
    return ((w >> r) | (w << (23 - r))) & MASK23;
}

// The 83 codes of ETSI table 2 (= TIA-603) and the 21 more most radios offer
// (the 104-code list)
inline bool dcs_etsi(int code) {
    static const int c[] = {023, 025, 026, 031, 032, 043, 047, 051, 054, 065, 071, 072, 073, 074, 0114, 0115, 0116,
                            0125, 0131, 0132, 0134, 0143, 0152, 0155, 0156, 0162, 0165, 0172, 0174, 0205, 0223, 0226,
                            0243, 0244, 0245, 0251, 0261, 0263, 0265, 0271, 0306, 0311, 0315, 0331, 0343, 0346, 0351,
                            0364, 0365, 0371, 0411, 0412, 0413, 0423, 0431, 0432, 0445, 0464, 0465, 0466, 0503, 0506,
                            0516, 0532, 0546, 0565, 0606, 0612, 0624, 0627, 0631, 0632, 0654, 0662, 0664, 0703, 0712,
                            0723, 0731, 0732, 0734, 0743, 0754};
    for (int x : c)
        if (x == code) return true;
    return false;
}
inline bool dcs_extended(int code) {
    static const int c[] = {036, 053, 0122, 0145, 0212, 0225, 0246, 0252, 0255, 0266, 0274,
                            0325, 0332, 0356, 0446, 0452, 0454, 0455, 0462, 0523, 0526};
    for (int x : c)
        if (x == code) return true;
    return false;
}

// The code(s) a 23-bit periodic pattern (any rotation of a codeword) reads
// as: every rotation whose bits 10-12 are 0, 0, 1
inline std::vector<int> dcs_alignments(uint32_t w) {
    std::vector<int> out;
    for (int r = 0; r < 23; r++) {
        const uint32_t v = rot23(w, r);
        if (((v >> 9) & 7u) == 4u) out.push_back(static_cast<int>(v & 0x1FF));   // bit 10 = 0, 11 = 0, 12 = 1
    }
    return out;
}

struct DcsName {
    int code = -1;           // octal value
    bool inverted = false;   // the waveform reads as this code inverted
    int other = -1;          // the same waveform read the other way (-1 = none on the lists)
    bool listed = false;     // on the ETSI / 104-code lists
    std::string id() const {
        char b[24];
        snprintf(b, sizeof b, "%03o%c", code, inverted ? 'I' : 'N');
        return b;
    }
    std::string other_id() const {
        if (other < 0) return "";
        char b[24];
        snprintf(b, sizeof b, "%03o%c", other, inverted ? 'N' : 'I');
        return b;
    }
};

// Name a received pattern w (time order, bit = 1 for a positive shift):
// the ETSI codes first, normal before inverted, then the 104-code list,
// then any alignment (non-standard). code -1 = no alignment at all
inline DcsName dcs_name(uint32_t w) {
    const std::vector<int> n = dcs_alignments(w), i = dcs_alignments(~w & MASK23);
    auto pick = [](const std::vector<int>& v, bool (*f)(int)) {
        int best = -1;
        for (int c : v)
            if (f(c) && (best < 0 || c < best)) best = c;
        return best;
    };
    DcsName r;
    int c;
    if ((c = pick(n, dcs_etsi)) >= 0) r = {c, false, -1, true};
    else if ((c = pick(i, dcs_etsi)) >= 0) r = {c, true, -1, true};
    else if ((c = pick(n, dcs_extended)) >= 0) r = {c, false, -1, true};
    else if ((c = pick(i, dcs_extended)) >= 0) r = {c, true, -1, true};
    else if (!n.empty()) r = {*std::min_element(n.begin(), n.end()), false, -1, false};
    else if (!i.empty()) r = {*std::min_element(i.begin(), i.end()), true, -1, false};
    else return r;
    // the equivalent the other way round (023N = 047I)
    const std::vector<int>& o = r.inverted ? n : i;
    int oc = pick(o, dcs_etsi);
    if (oc < 0) oc = pick(o, dcs_extended);
    r.other = oc;
    return r;
}

// The time-order pattern of a code (normal polarity) - for tests
inline uint32_t dcs_word(int code) {
    uint32_t d = (static_cast<uint32_t>(code) & 0x1FF) | (4u << 9);
    for (uint32_t p = 0; p < 2048; p++) {
        const uint32_t w = d | (p << 12);
        if (golay_ok(w)) return w;
    }
    return 0;
}

}  // namespace ctcss
