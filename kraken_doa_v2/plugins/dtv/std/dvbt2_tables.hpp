// dvbt2_tables.hpp - DVB-T2 pilot / preamble constants for blind reception.
//
// Source: ETSI EN 302 755 V1.4.1 (2015-07), "Frame structure channel coding and
// modulation for a second generation digital terrestrial television
// broadcasting system (DVB-T2)". Every number below is taken from that
// document; the clause / table is given next to it. See dvbt2_spec.md.
//
// Header-only, C++17, standard library only.
//
// Conventions used throughout:
//   fft   : FFT size, either in K (1, 2, 4, 8, 16, 32) or as N (1024 .. 32768)
//   pp    : scattered pilot pattern number 1..8 (PP1..PP8)
//   gi    : guard-interval index 0..6 = 1/128, 1/32, 1/16, 19/256, 1/8, 19/128, 1/4
//   k     : OFDM carrier index, Kmin = 0 .. Kmax of the mode in use (clause 9.5).
//           In extended carrier mode k is the EXTENDED-mode index (0 .. 6912 /
//           13920 / 27840); the normal-mode carrier k_n sits at k = k_n + Kext.
//   l     : OFDM symbol index within the T2-frame, l = 0 for the FIRST P2 symbol
//           (P1 is not counted; clauses 8.3.2, 9.5)
//   ext   : true = extended carrier mode (8K, 16K, 32K only; table 66)
//
// SISO only, except where a function says otherwise (MISO modifications:
// clause 9.2.8, summarised in dvbt2_spec.md).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dvbt2_std {

// ---------------------------------------------------------------------------
// FFT-size dependent parameters
// ---------------------------------------------------------------------------

inline int fft_index(int fft) {
    switch (fft) {
        case 1: case 1024: return 0;
        case 2: case 2048: return 1;
        case 4: case 4096: return 2;
        case 8: case 8192: return 3;
        case 16: case 16384: return 4;
        case 32: case 32768: return 5;
        default: break;
    }
    throw std::invalid_argument("dvbt2_std: fft must be 1,2,4,8,16,32 or 1024..32768");
}

inline constexpr int NUM_FFT = 6;
inline constexpr int FFT_K[NUM_FFT] = {1, 2, 4, 8, 16, 32};
// Table 66: duration TU in elementary periods T (= FFT size N)
inline constexpr int FFT_N[NUM_FFT] = {1024, 2048, 4096, 8192, 16384, 32768};
// Table 66: number of carriers Ktotal, normal / extended carrier mode (0 = NA)
inline constexpr int KTOTAL_NORMAL[NUM_FFT] = {853, 1705, 3409, 6817, 13633, 27265};
inline constexpr int KTOTAL_EXT[NUM_FFT] = {0, 0, 0, 6913, 13921, 27841};
// Table 66: Kmin = 0 in every mode; Kmax normal / extended (-1 = NA)
inline constexpr int KMIN = 0;
inline constexpr int KMAX_NORMAL[NUM_FFT] = {852, 1704, 3408, 6816, 13632, 27264};
inline constexpr int KMAX_EXT[NUM_FFT] = {-1, -1, -1, 6912, 13920, 27840};
// Table 66: carriers added on each side in extended mode, Kext (also used in
// the pilot sequence definition in normal mode - table 66 note 2)
inline constexpr int KEXT[NUM_FFT] = {0, 0, 0, 48, 144, 288};
// Table 61: continual-pilot groups used (CP1..CPn) and Kmod (0 = NA for 32K)
inline constexpr int NUM_CP_GROUPS[NUM_FFT] = {1, 2, 3, 4, 5, 6};
inline constexpr int KMOD[NUM_FFT] = {1632, 1632, 3264, 6528, 13056, 0};
// Table 51: number of P2 symbols NP2
inline constexpr int NP2[NUM_FFT] = {16, 8, 4, 2, 1, 1};
// Table 62: continual pilot amplitude ACP (4K: (4*sqrt(2))/3)
inline const double ACP[NUM_FFT] = {4.0 / 3.0, 4.0 / 3.0, 4.0 * std::sqrt(2.0) / 3.0,
                                    8.0 / 3.0, 8.0 / 3.0, 8.0 / 3.0};
// Table H.1 / table 48 "TR cells": number of reserved carriers NTR
inline constexpr int NTR[NUM_FFT] = {10, 18, 36, 72, 144, 288};
// Clause 9.5 / 9.8.2.4: P1 duration TP1 = 2048 T
inline constexpr int TP1_T = 2048;

inline int kext(int fft) { return KEXT[fft_index(fft)]; }
inline bool has_extended_mode(int fft) { return KEXT[fft_index(fft)] > 0; }
inline int kmax(int fft, bool ext = false) {
    const int fi = fft_index(fft);
    if (ext && KMAX_EXT[fi] < 0)
        throw std::invalid_argument("dvbt2_std: extended carrier mode only exists for 8K/16K/32K");
    return ext ? KMAX_EXT[fi] : KMAX_NORMAL[fi];
}
inline int ktotal(int fft, bool ext = false) { return kmax(fft, ext) + 1; }
// Clause 9.5: k' = k - (Kmax + Kmin)/2 is the carrier index relative to the
// RF centre frequency fc, i.e. the carrier ON the centre is k = Kmax/2.
inline int centre_carrier(int fft, bool ext = false) { return (kmax(fft, ext) + KMIN) / 2; }

// ---------------------------------------------------------------------------
// Elementary period T (table 65) and guard intervals (table 67)
// ---------------------------------------------------------------------------

struct Bandwidth {
    double mhz;  // nominal channel bandwidth
    int t_num;   // T = t_num / t_den microseconds
    int t_den;
};
// Table 65 (10 MHz: professional applications only, see its note)
inline constexpr Bandwidth BANDWIDTHS[6] = {
    {1.7, 71, 131}, {5.0, 7, 40}, {6.0, 7, 48}, {7.0, 1, 8}, {8.0, 7, 64}, {10.0, 7, 80}};
inline double elementary_period_us(double bw_mhz) {
    for (const auto& b : BANDWIDTHS)
        if (std::fabs(b.mhz - bw_mhz) < 1e-9) return double(b.t_num) / double(b.t_den);
    throw std::invalid_argument("dvbt2_std: bandwidth must be 1.7, 5, 6, 7, 8 or 10 MHz");
}
inline double sample_rate_hz(double bw_mhz) { return 1e6 / elementary_period_us(bw_mhz); }  // 1/T

inline constexpr int NUM_GI = 7;
// Guard interval fractions Delta/TU (clause 9.7): 1/128 1/32 1/16 19/256 1/8 19/128 1/4
inline constexpr int GI_NUM[NUM_GI] = {1, 1, 1, 19, 1, 19, 1};
inline constexpr int GI_DEN[NUM_GI] = {128, 32, 16, 256, 8, 128, 4};
// Table 67: guard interval duration in T, [fft index][gi index], 0 = NA (not allowed)
inline constexpr int GI_LEN_T[NUM_FFT][NUM_GI] = {
    /* 1K  */ {0, 0, 64, 0, 128, 0, 256},
    /* 2K  */ {0, 64, 128, 0, 256, 0, 512},
    /* 4K  */ {0, 128, 256, 0, 512, 0, 1024},
    /* 8K  */ {64, 256, 512, 608, 1024, 1216, 2048},
    /* 16K */ {128, 512, 1024, 1216, 2048, 2432, 4096},
    /* 32K */ {256, 1024, 2048, 2432, 4096, 4864, 0}};
inline int gi_length_T(int fft, int gi) { return GI_LEN_T[fft_index(fft)][gi]; }
inline int symbol_length_T(int fft, int gi) { return FFT_N[fft_index(fft)] + gi_length_T(fft, gi); }

// Table 46: maximum frame length LF in OFDM symbols, 8 MHz, [fft][gi], 0 = NA
inline constexpr int MAX_LF_8MHZ[NUM_FFT][NUM_GI] = {
    /* 1K  */ {0, 0, 2098, 0, 1982, 0, 1784},
    /* 2K  */ {0, 1081, 1049, 0, 991, 0, 892},
    /* 4K  */ {0, 540, 524, 0, 495, 0, 446},
    /* 8K  */ {276, 270, 262, 259, 247, 242, 223},
    /* 16K */ {138, 135, 131, 129, 123, 121, 111},
    /* 32K */ {68, 66, 64, 64, 60, 60, 0}};
// Table 55: maximum PN sequence length NPN (= max LF) per FFT, 8 MHz channel
inline constexpr int MAX_NPN_8MHZ[NUM_FFT] = {2098, 1081, 540, 276, 138, 68};

// ---------------------------------------------------------------------------
// Scattered pilot patterns
// ---------------------------------------------------------------------------

// Table 57: DX (separation of pilot-bearing carriers), DY (symbols per SP sequence)
inline constexpr int DX[8] = {3, 6, 6, 12, 12, 24, 24, 6};
inline constexpr int DY[8] = {4, 2, 4, 2, 4, 2, 4, 16};
// Table 60: scattered pilot amplitude ASP (PP1,PP2: 4/3; PP3,PP4: 7/4; PP5-PP8: 7/3)
inline constexpr double ASP[8] = {4.0 / 3.0, 4.0 / 3.0, 7.0 / 4.0, 7.0 / 4.0,
                                  7.0 / 3.0, 7.0 / 3.0, 7.0 / 3.0, 7.0 / 3.0};
// Table 63: P2 pilot amplitude AP2
inline const double AP2_32K_SISO = std::sqrt(37.0) / 5.0;
inline const double AP2_OTHER = std::sqrt(31.0) / 5.0;  // all other modes incl. 32K MISO

inline constexpr unsigned PPM(int pp) { return 1u << (pp - 1); }
// Table 58 (SISO): allowed pilot patterns, bitmask bit (pp-1), [fft][gi]; 0 = NA
inline constexpr unsigned SISO_PP_ALLOWED[NUM_FFT][NUM_GI] = {
    /* 1K  */ {0, 0, PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 2K  */ {0, PPM(7) | PPM(4), PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 4K  */ {0, PPM(7) | PPM(4), PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 8K  */ {PPM(7), PPM(7) | PPM(4), PPM(8) | PPM(4) | PPM(5), PPM(8) | PPM(4) | PPM(5),
               PPM(2) | PPM(3) | PPM(8), PPM(2) | PPM(3) | PPM(8), PPM(1) | PPM(8)},
    /* 16K */ {PPM(7), PPM(7) | PPM(4) | PPM(6), PPM(2) | PPM(8) | PPM(4) | PPM(5),
               PPM(2) | PPM(8) | PPM(4) | PPM(5), PPM(2) | PPM(3) | PPM(8),
               PPM(2) | PPM(3) | PPM(8), PPM(1) | PPM(8)},
    /* 32K */ {PPM(7), PPM(4) | PPM(6), PPM(2) | PPM(8) | PPM(4), PPM(2) | PPM(8) | PPM(4),
               PPM(2) | PPM(8), PPM(2) | PPM(8), 0}};
// Table 59 (MISO): allowed pilot patterns, [fft][gi]; 0 = NA
inline constexpr unsigned MISO_PP_ALLOWED[NUM_FFT][NUM_GI] = {
    /* 1K  */ {0, 0, PPM(3), 0, PPM(1), 0, 0},
    /* 2K  */ {0, PPM(4) | PPM(5), PPM(3), 0, PPM(1), 0, 0},
    /* 4K  */ {0, PPM(4) | PPM(5), PPM(3), 0, PPM(1), 0, 0},
    /* 8K  */ {PPM(8) | PPM(4) | PPM(5), PPM(8) | PPM(4) | PPM(5), PPM(3) | PPM(8),
               PPM(3) | PPM(8), PPM(1) | PPM(8), PPM(1) | PPM(8), 0},
    /* 16K */ {PPM(8) | PPM(4) | PPM(5), PPM(8) | PPM(4) | PPM(5), PPM(3) | PPM(8),
               PPM(3) | PPM(8), PPM(1) | PPM(8), PPM(1) | PPM(8), 0},
    /* 32K */ {PPM(8) | PPM(4) | PPM(6), PPM(8) | PPM(4), PPM(2) | PPM(8), PPM(2) | PPM(8), 0, 0, 0}};
// Table 64 (SISO): pilot patterns for which a FRAME CLOSING symbol is used, [fft][gi]
// (MISO: frame closing symbols always used except with PP8 - clause 9.2.7.0)
inline constexpr unsigned SISO_FC_USED[NUM_FFT][NUM_GI] = {
    /* 1K  */ {0, 0, PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 2K  */ {0, PPM(7), PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 4K  */ {0, PPM(7), PPM(4) | PPM(5), 0, PPM(2) | PPM(3), 0, PPM(1)},
    /* 8K  */ {0, PPM(7), PPM(4) | PPM(5), PPM(4) | PPM(5), PPM(2) | PPM(3), PPM(2) | PPM(3), PPM(1)},
    /* 16K */ {0, PPM(7) | PPM(6), PPM(4) | PPM(5), PPM(4) | PPM(5), PPM(2) | PPM(3),
               PPM(2) | PPM(3), PPM(1)},
    /* 32K */ {0, PPM(6), PPM(4), PPM(4), PPM(2), PPM(2), 0}};

inline bool siso_allowed(int fft, int gi, int pp) { return SISO_PP_ALLOWED[fft_index(fft)][gi] & PPM(pp); }
inline bool miso_allowed(int fft, int gi, int pp) { return MISO_PP_ALLOWED[fft_index(fft)][gi] & PPM(pp); }
inline bool frame_closing_used(int fft, int gi, int pp, bool miso = false) {
    if (miso) return pp != 8;
    return SISO_FC_USED[fft_index(fft)][gi] & PPM(pp);
}

// ---------------------------------------------------------------------------
// Reference counts used by the self-test (no tone reservation)
// ---------------------------------------------------------------------------
// Table 47: active cells CP2 in one P2 symbol (SISO, MISO)
inline constexpr int CP2_SISO[NUM_FFT] = {558, 1118, 2236, 4472, 8944, 22432};
inline constexpr int CP2_MISO[NUM_FFT] = {546, 1098, 2198, 4398, 8814, 17612};
// Table 48: data cells Cdata in one normal symbol, [fft][pp-1], -1 = never used
inline constexpr int CDATA_NORMAL[NUM_FFT][8] = {
    {764, 768, 798, 804, 818, -1, -1, -1},
    {1522, 1532, 1596, 1602, 1632, -1, 1646, -1},
    {3084, 3092, 3228, 3234, 3298, -1, 3328, -1},
    {6208, 6214, 6494, 6498, 6634, -1, 6698, 6698},
    {12418, 12436, 12988, 13002, 13272, 13288, 13416, 13406},
    {-1, 24886, -1, 26022, -1, 26592, 26836, 26812}};
inline constexpr int CDATA_EXT[NUM_FFT][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {6296, 6298, 6584, 6588, 6728, -1, 6788, 6788},
    {12678, 12698, 13262, 13276, 13552, 13568, 13698, 13688},
    {-1, 25412, -1, 26572, -1, 27152, 27404, 27376}};
// Table 49: data cells NFC in the frame closing symbol, [fft][pp-1], -1 = never used
inline constexpr int NFC_NORMAL[NUM_FFT][8] = {
    {568, 710, 710, 780, 780, -1, -1, -1},
    {1136, 1420, 1420, 1562, 1562, -1, 1632, -1},
    {2272, 2840, 2840, 3124, 3124, -1, 3266, -1},
    {4544, 5680, 5680, 6248, 6248, -1, 6532, -1},
    {9088, 11360, 11360, 12496, 12496, 13064, 13064, -1},
    {-1, 22720, -1, 24992, -1, 26128, -1, -1}};
inline constexpr int NFC_EXT[NUM_FFT][8] = {
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1},
    {4608, 5760, 5760, 6336, 6336, -1, 6624, -1},
    {9280, 11600, 11600, 12760, 12760, 13340, 13340, -1},
    {-1, 23200, -1, 25520, -1, 26680, -1, -1}};

// ---------------------------------------------------------------------------
// Reference sequence (clause 9.2.2)
// ---------------------------------------------------------------------------

// Clause 9.2.2.1 / figure 49: symbol-level PRBS w_i, generator X^11 + X^2 + 1.
// 11-cell shift register initialised to all '1'; output = cell 11; the
// feedback (cell 9 XOR cell 11) enters cell 1. Sequence begins
// 1,1,1,1,1,1,1,1,1,1,1,0,0,... Returns w_0 .. w_kmax (kmax+1 values).
inline std::vector<uint8_t> prbs_w(int kmax_) {
    if (kmax_ < 0) return {};
    std::vector<uint8_t> w(static_cast<size_t>(kmax_) + 1);
    uint8_t reg[12];  // reg[1..11]
    for (int i = 1; i <= 11; ++i) reg[i] = 1;
    for (int i = 0; i <= kmax_; ++i) {
        w[static_cast<size_t>(i)] = reg[11];
        const uint8_t fb = reg[9] ^ reg[11];
        for (int c = 11; c >= 2; --c) reg[c] = reg[c - 1];
        reg[1] = fb;
    }
    return w;
}
// Largest index ever needed: extended 32K Kmax = 27840 (normal mode uses w_{k+Kext} <= 27552)
inline const std::vector<uint8_t>& w_table() {
    static const std::vector<uint8_t> w = prbs_w(KMAX_EXT[5]);
    return w;
}

// Clause 9.2.2.2 / table 56: frame-level PN sequence, 2624 chips, hex MSB first.
inline constexpr int PN_LEN = 2624;
inline constexpr const char PN_HEX[] =
    "4DC2AF7BD8C3C9A1E76C9A090AF1C3114F07FCA2808E9462E9AD7B712D6F4AC8A59BB069CC50BF1149"
    "927E6BB1C9FC8C18BB949B30CD09DDD749E704F57B41DEC7E7B176E12C5657432B51B0B812DF0E1488"
    "7E24D80C97F09374AD76270E58FE1774B2781D8D3821E393F2EA0FFD4D24DE20C05D0BA1703D10E52D"
    "61E013D837AA62D007CC2FD76D23A3E125BDE8A9A7C02A98B70251C556F6341EBDECB801AAD5D9FB8C"
    "BEA80BB619096527A8C475B3D8DB28AF8543A00EC3480DFF1E2CDA9F985B523B879007AA5D0CE58D21"
    "B18631006617F6F769EB947F924EA5161EC2C0488B63ED7993BA8EF4E552FA32FC3F1BDB19923902BC"
    "BBE5DDABB824126E08459CA6CFA0267E5294A98C632569791E60EF659AEE9518CDF08D87833690C1B7"
    "9183ED127E53360CD86514859A28B5494F51AA4882419A25A2D01A5F47AA27301E79A5370CCB3E197F";
static_assert(sizeof(PN_HEX) - 1 == PN_LEN / 4, "table 56 must hold 656 hex digits");

constexpr int hex_value(char c) {
    return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                            : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}
constexpr std::array<uint8_t, PN_LEN> make_pn_bits() {
    std::array<uint8_t, PN_LEN> b{};
    for (int i = 0; i < PN_LEN; ++i)
        b[static_cast<size_t>(i)] = static_cast<uint8_t>((hex_value(PN_HEX[i / 4]) >> (3 - i % 4)) & 1);
    return b;
}
// pn_l for l = 0 .. 2623 (pn_0 = first bit = applies to the first P2 symbol, l = 0)
inline constexpr std::array<uint8_t, PN_LEN> PN_BITS = make_pn_bits();
inline int pn(int l) {
    if (l < 0 || l >= PN_LEN) throw std::out_of_range("dvbt2_std::pn: l must be 0..2623");
    return PN_BITS[static_cast<size_t>(l)];
}

// Clause 9.2.2.0: r_{l,k} = w_{k+Kext} XOR pn_l (normal mode),
//                          w_k      XOR pn_l (extended mode)
inline int ref_seq(int k, int l, int fft, bool ext = false) {
    const int idx = ext ? k : k + kext(fft);
    return w_table().at(static_cast<size_t>(idx)) ^ pn(l);
}
// Pilot modulation (clauses 9.2.3.3, 9.2.4.4, 9.2.5, 9.2.6.3, 9.2.7.3):
// Re{c} = 2 A (1/2 - r), Im{c} = 0  ->  +A for r = 0, -A for r = 1
inline std::complex<double> pilot_modulation(double A, int r) { return {2.0 * A * (0.5 - r), 0.0}; }

// ---------------------------------------------------------------------------
// Scattered pilots (clause 9.2.3.1) - in NORMAL symbols only, i.e.
// NP2 <= l <= LF-1 (LF-2 when a frame closing symbol is present)
// ---------------------------------------------------------------------------

inline int pos_mod(int a, int m) { return ((a % m) + m) % m; }
// Normal carrier mode: k mod (DX*DY) = DX * (l mod DY)
inline bool is_scattered(int k, int l, int pp) {
    const int dx = DX[pp - 1], dy = DY[pp - 1];
    return pos_mod(k, dx * dy) == dx * pos_mod(l, dy);
}
// With range check; extended mode: (k - Kext) mod (DX*DY) = DX * (l mod DY)
inline bool is_scattered(int k, int l, int pp, int fft, bool ext) {
    if (k < KMIN || k > kmax(fft, ext)) return false;
    return is_scattered(ext ? k - kext(fft) : k, l, pp);
}

// ---------------------------------------------------------------------------
// Continual pilots (clause 9.2.4, annex G)
// ---------------------------------------------------------------------------

// Table G.1: k_i,32K for CP group g (1..6) and pattern pp (1..8): CP_G1[g-1][pp-1]
inline const std::vector<int> CP_G1[6][8] = {
    {  // CP1
        /* CP1 PP1 */ {
          116, 255, 285, 430, 518, 546, 601, 646, 744, 1662, 1893, 1995, 2322, 3309, 3351, 3567,
          3813, 4032, 5568, 5706},
        /* CP1 PP2 */ {
          116, 318, 390, 430, 474, 518, 601, 646, 708, 726, 1752, 1758, 1944, 2100, 2208, 2466,
          3792, 5322, 5454, 5640},
        /* CP1 PP3 */ {
          116, 318, 342, 426, 430, 518, 582, 601, 646, 816, 1758, 1764, 2400, 3450, 3504, 3888,
          4020, 4932, 5154, 5250, 5292, 5334},
        /* CP1 PP4 */ {
          108, 116, 144, 264, 288, 430, 518, 564, 636, 646, 828, 2184, 3360, 3396, 3912, 4032, 4932,
          5220, 5676, 5688},
        /* CP1 PP5 */ {
          108, 116, 228, 430, 518, 601, 646, 804, 1644, 1680, 1752, 1800, 1836, 3288, 3660, 4080,
          4932, 4968, 5472},
        /* CP1 PP6 */ {},
        /* CP1 PP7 */ {264, 360, 1848, 2088, 2112, 2160, 2256, 2280, 3936, 3960, 3984, 5016, 5136, 5208, 5664},
        /* CP1 PP8 */ {}},
    {  // CP2
        /* CP2 PP1 */ {
          1022, 1224, 1302, 1371, 1495, 2261, 2551, 2583, 2649, 2833, 2925, 3192, 4266, 5395, 5710,
          5881, 8164, 10568, 11069, 11560, 12631, 12946, 13954, 16745, 21494},
        /* CP2 PP2 */ {
          1022, 1092, 1369, 1416, 1446, 1495, 2598, 2833, 2928, 3144, 4410, 4800, 5710, 5881, 6018,
          6126, 10568, 11515, 12946, 13954, 15559, 16681},
        /* CP2 PP3 */ {
          1022, 1495, 2261, 2551, 2802, 2820, 2833, 2922, 4422, 4752, 4884, 5710, 8164, 10568,
          11069, 11560, 12631, 12946, 16745, 21494},
        /* CP2 PP4 */ {
          601, 1022, 1092, 1164, 1369, 1392, 1452, 1495, 2261, 2580, 2833, 3072, 4320, 4452, 5710,
          5881, 6048, 10568, 11515, 12946, 13954, 15559, 16681},
        /* CP2 PP5 */ {
          852, 1022, 1495, 2508, 2551, 2604, 2664, 2736, 2833, 3120, 4248, 4512, 4836, 5710, 5940,
          6108, 8164, 10568, 11069, 11560, 12946, 13954, 21494},
        /* CP2 PP6 */ {},
        /* CP2 PP7 */ {
          116, 430, 518, 601, 646, 1022, 1296, 1368, 1369, 1495, 2833, 3024, 4416, 4608, 4776, 5710,
          5881, 6168, 7013, 8164, 10568, 10709, 11515, 12946, 15559, 23239, 24934, 25879, 26308,
          26674},
        /* CP2 PP8 */ {}},
    {  // CP3
        /* CP3 PP1 */ {},
        /* CP3 PP2 */ {2261, 8164},
        /* CP3 PP3 */ {13954},
        /* CP3 PP4 */ {8164},
        /* CP3 PP5 */ {648, 4644, 16745},
        /* CP3 PP6 */ {},
        /* CP3 PP7 */ {456, 480, 2261, 6072, 17500},
        /* CP3 PP8 */ {}},
    {  // CP4
        /* CP4 PP1 */ {},
        /* CP4 PP2 */ {10709, 19930},
        /* CP4 PP3 */ {},
        /* CP4 PP4 */ {10709, 19930},
        /* CP4 PP5 */ {12631},
        /* CP4 PP6 */ {},
        /* CP4 PP7 */ {1008, 6120, 13954},
        /* CP4 PP8 */ {
          116, 132, 180, 430, 518, 601, 646, 1022, 1266, 1369, 1495, 2261, 2490, 2551, 2712, 2833,
          3372, 3438, 4086, 4098, 4368, 4572, 4614, 4746, 4830, 4968, 5395, 5710, 5881, 7649, 8164,
          10568, 11069, 11560, 12631, 12946, 13954, 15760, 16612, 16745, 17500, 19078, 19930, 21494,
          22867, 25879, 26308}},
    {  // CP5
        /* CP5 PP1 */ {
          1369, 7013, 7215, 7284, 7649, 7818, 8025, 8382, 8733, 8880, 9249, 9432, 9771, 10107,
          10110, 10398, 10659, 10709, 10785, 10872, 11115, 11373, 11515, 11649, 11652, 12594, 12627,
          12822, 12984, 15760, 16612, 17500, 18358, 19078, 19930, 20261, 20422, 22124, 22867, 23239,
          24934, 25879, 26308, 26674},
        /* CP5 PP2 */ {
          6744, 7013, 7020, 7122, 7308, 7649, 7674, 7752, 7764, 8154, 8190, 8856, 8922, 9504, 9702,
          9882, 9924, 10032, 10092, 10266, 10302, 10494, 10530, 10716, 11016, 11076, 11160, 11286,
          11436, 11586, 12582, 13002, 17500, 18358, 19078, 22124, 23239, 24073, 24934, 25879, 26308},
        /* CP5 PP3 */ {
          1369, 5395, 5881, 6564, 6684, 7013, 7649, 8376, 8544, 8718, 8856, 9024, 9132, 9498, 9774,
          9840, 10302, 10512, 10566, 10770, 10914, 11340, 11418, 11730, 11742, 12180, 12276, 12474,
          12486, 15760, 16612, 17500, 18358, 19078, 19930, 20261, 20422, 22124, 22867, 23239, 24934,
          25879, 26308, 26674},
        /* CP5 PP4 */ {
          6612, 6708, 7013, 7068, 7164, 7224, 7308, 7464, 7649, 7656, 7716, 7752, 7812, 7860, 8568,
          8808, 8880, 9072, 9228, 9516, 9696, 9996, 10560, 10608, 10728, 11148, 11232, 11244, 11496,
          11520, 11664, 11676, 11724, 11916, 17500, 18358, 19078, 21284, 22124, 23239, 24073, 24934,
          25879, 26308},
        /* CP5 PP5 */ {
          1369, 2261, 5395, 5881, 6552, 6636, 6744, 6900, 7032, 7296, 7344, 7464, 7644, 7649, 7668,
          7956, 8124, 8244, 8904, 8940, 8976, 9216, 9672, 9780, 10224, 10332, 10709, 10776, 10944,
          11100, 11292, 11364, 11496, 11532, 11904, 12228, 12372, 12816, 15760, 16612, 17500, 19078,
          22867, 25879},
        /* CP5 PP6 */ {
          116, 384, 408, 518, 601, 646, 672, 960, 1022, 1272, 1344, 1369, 1495, 1800, 2040, 2261,
          2833, 3192, 3240, 3768, 3864, 3984, 4104, 4632, 4728, 4752, 4944, 5184, 5232, 5256, 5376,
          5592, 5616, 5710, 5808, 5881, 6360, 6792, 6960, 7013, 7272, 7344, 7392, 7536, 7649, 7680,
          7800, 8064, 8160, 8164, 8184, 8400, 8808, 8832, 9144, 9648, 9696, 9912, 10008, 10200,
          10488, 10568, 10656, 10709, 11088, 11160, 11515, 11592, 12048, 12264, 12288, 12312, 12552,
          12672, 12946, 13954, 15559, 16681, 17500, 19078, 20422, 21284, 22124, 23239, 24934, 25879,
          26308, 26674},
        /* CP5 PP7 */ {
          6984, 7032, 7056, 7080, 7152, 7320, 7392, 7536, 7649, 7704, 7728, 7752, 8088, 8952, 9240,
          9288, 9312, 9480, 9504, 9840, 9960, 10320, 10368, 10728, 10752, 11448, 11640, 11688,
          11808, 12192, 12240, 12480, 12816, 16681, 22124},
        /* CP5 PP8 */ {
          6720, 6954, 7013, 7026, 7092, 7512, 7536, 7596, 7746, 7758, 7818, 7986, 8160, 8628, 9054,
          9096, 9852, 9924, 10146, 10254, 10428, 10704, 11418, 11436, 11496, 11550, 11766, 11862,
          12006, 12132, 12216, 12486, 12762, 18358, 20261, 20422, 22124, 23239, 24934}},
    {  // CP6
        /* CP6 PP1 */ {},
        /* CP6 PP2 */ {
          13164, 13206, 13476, 13530, 13536, 13764, 13848, 13938, 13968, 14028, 14190, 14316, 14526,
          14556, 14562, 14658, 14910, 14946, 15048, 15186, 15252, 15468, 15540, 15576, 15630, 15738,
          15840, 16350, 16572, 16806, 17028, 17064, 17250, 17472, 17784, 17838, 18180, 18246, 18480,
          18900, 18960, 19254, 19482, 19638, 19680, 20082, 20310, 20422, 20454, 20682, 20874, 21240,
          21284, 21444, 21450, 21522, 21594, 21648, 21696, 21738, 22416, 22824, 23016, 23124, 23196,
          23238, 23316, 23418, 23922, 23940, 24090, 24168, 24222, 24324, 24342, 24378, 24384, 24540,
          24744, 24894, 24990, 25002, 25194, 25218, 25260, 25566, 26674, 26944},
        /* CP6 PP3 */ {},
        /* CP6 PP4 */ {
          13080, 13152, 13260, 13380, 13428, 13572, 13884, 13956, 14004, 14016, 14088, 14232, 14304,
          14532, 14568, 14760, 14940, 15168, 15288, 15612, 15684, 15888, 16236, 16320, 16428, 16680,
          16812, 16908, 17184, 17472, 17508, 17580, 17892, 17988, 18000, 18336, 18480, 18516, 19020,
          19176, 19188, 19320, 19776, 19848, 20112, 20124, 20184, 20388, 20532, 20556, 20676, 20772,
          21156, 21240, 21276, 21336, 21384, 21816, 21888, 22068, 22092, 22512, 22680, 22740, 22800,
          22836, 22884, 23304, 23496, 23568, 23640, 24120, 24168, 24420, 24444, 24456, 24492, 24708,
          24864, 25332, 25536, 25764, 25992, 26004, 26674, 26944},
        /* CP6 PP5 */ {},
        /* CP6 PP6 */ {
          13080, 13368, 13464, 13536, 13656, 13728, 13824, 14112, 14232, 14448, 14472, 14712, 14808,
          14952, 15000, 15336, 15360, 15408, 15600, 15624, 15648, 16128, 16296, 16320, 16416, 16536,
          16632, 16824, 16848, 17184, 17208, 17280, 17352, 17520, 17664, 17736, 17784, 18048, 18768,
          18816, 18840, 19296, 19392, 19584, 19728, 19752, 19776, 20136, 20184, 20208, 20256, 21096,
          21216, 21360, 21408, 21744, 21768, 22200, 22224, 22320, 22344, 22416, 22848, 22968, 23016,
          23040, 23496, 23688, 23904, 24048, 24168, 24360, 24408, 24984, 25152, 25176, 25224, 25272,
          25344, 25416, 25488, 25512, 25536, 25656, 25680, 25752, 25992, 26016},
        /* CP6 PP7 */ {
          13416, 13440, 13536, 13608, 13704, 13752, 14016, 14040, 14112, 14208, 14304, 14376, 14448,
          14616, 14712, 14760, 14832, 14976, 15096, 15312, 15336, 15552, 15816, 15984, 16224, 16464,
          16560, 17088, 17136, 17256, 17352, 17400, 17448, 17544, 17928, 18048, 18336, 18456, 18576,
          18864, 19032, 19078, 19104, 19320, 19344, 19416, 19488, 19920, 19930, 19992, 20424, 20664,
          20808, 21168, 21284, 21360, 21456, 21816, 22128, 22200, 22584, 22608, 22824, 22848, 22944,
          22992, 23016, 23064, 23424, 23448, 23472, 23592, 24192, 24312, 24360, 24504, 24552, 24624,
          24648, 24672, 24768, 24792, 25080, 25176, 25224, 25320, 25344, 25584, 25680, 25824, 26064,
          26944},
        /* CP6 PP8 */ {
          10709, 11515, 13254, 13440, 13614, 13818, 14166, 14274, 14304, 14364, 14586, 14664, 15030,
          15300, 15468, 15474, 15559, 15732, 15774, 16272, 16302, 16428, 16500, 16662, 16681, 16872,
          17112, 17208, 17862, 18036, 18282, 18342, 18396, 18420, 18426, 18732, 19050, 19296, 19434,
          19602, 19668, 19686, 19728, 19938, 20034, 21042, 21120, 21168, 21258, 21284, 21528, 21594,
          21678, 21930, 21936, 21990, 22290, 22632, 22788, 23052, 23358, 23448, 23454, 23706, 23772,
          24048, 24072, 24073, 24222, 24384, 24402, 24444, 24462, 24600, 24738, 24804, 24840, 24918,
          24996, 25038, 25164, 25314, 25380, 25470, 25974, 26076, 26674, 26753, 26944}}
};
// Table G.2: additional continual pilots in EXTENDED carrier mode, [pp-1]
// (8K PP1/PP5 "None", 8K PP6 "NA"; 32K PP1/PP5 "NA", 32K PP3 blank)
inline const std::vector<int> CP_G2_8K[8] = {
    /* PP1 */ {},
    /* PP2 */ {6820, 6847, 6869, 6898},
    /* PP3 */ {6820, 6869},
    /* PP4 */ {6820, 6869},
    /* PP5 */ {},
    /* PP6 */ {},
    /* PP7 */ {6820, 6833, 6869, 6887, 6898},
    /* PP8 */ {6820, 6833, 6869, 6887, 6898}
};
inline const std::vector<int> CP_G2_16K[8] = {
    /* PP1 */ {13636, 13724, 13790, 13879},
    /* PP2 */ {13636, 13790},
    /* PP3 */ {13636, 13790},
    /* PP4 */ {13636, 13790},
    /* PP5 */ {13636, 13790},
    /* PP6 */ {13636, 13790},
    /* PP7 */ {13636, 13724, 13879},
    /* PP8 */ {13636, 13724, 13879}
};
inline const std::vector<int> CP_G2_32K[8] = {
    /* PP1 */ {},
    /* PP2 */ {27268, 27688},
    /* PP3 */ {},
    /* PP4 */ {27268, 27688},
    /* PP5 */ {},
    /* PP6 */ {27268, 27448, 27688, 27758},
    /* PP7 */ {27268, 27688},
    /* PP8 */ {27268, 27368, 27448, 27580, 27688, 27758}
};

// Sorted, unique continual pilot carrier indices for (fft, pp, mode):
//   k = k_i,32K mod Kmod over groups CP1..CPn of table 61 (32K: k = k_i,32K),
//   plus table G.2 in extended mode.
// NOTE (see dvbt2_spec.md): in extended mode these k are used AS IS as
// extended-mode indices (no +Kext). The standard does not spell this out;
// it is the only reading that reproduces every Cdata value of table 48
// (checked by dvbt2_selftest.cpp).
inline std::vector<int> compute_continual_positions(int fft, int pp, bool ext) {
    const int fi = fft_index(fft);
    std::vector<int> v;
    for (int g = 0; g < NUM_CP_GROUPS[fi]; ++g)
        for (int k : CP_G1[g][pp - 1]) v.push_back(KMOD[fi] ? k % KMOD[fi] : k);
    if (ext) {
        const std::vector<int>* g2 = fi == 3 ? CP_G2_8K : fi == 4 ? CP_G2_16K : fi == 5 ? CP_G2_32K : nullptr;
        if (!g2) throw std::invalid_argument("dvbt2_std: extended carrier mode only exists for 8K/16K/32K");
        for (int k : g2[pp - 1]) v.push_back(k);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}
inline const std::vector<int>& continual_positions(int fft, int pp, bool ext = false) {
    static const auto cache = [] {
        std::array<std::array<std::array<std::vector<int>, 2>, 8>, NUM_FFT> c{};
        for (int fi = 0; fi < NUM_FFT; ++fi)
            for (int p = 1; p <= 8; ++p)
                for (int e = 0; e < 2; ++e)
                    if (!e || KEXT[fi]) c[fi][p - 1][e] = compute_continual_positions(FFT_K[fi], p, e);
        return c;
    }();
    const int fi = fft_index(fft);
    if (ext && !KEXT[fi]) throw std::invalid_argument("dvbt2_std: extended carrier mode only exists for 8K/16K/32K");
    return cache[fi][pp - 1][ext ? 1 : 0];
}
inline bool is_continual(int k, int fft, int pp, bool ext = false) {
    const auto& v = continual_positions(fft, pp, ext);
    return std::binary_search(v.begin(), v.end(), k);
}

// Edge pilots (clause 9.2.5): k = Kmin and k = Kmax, in every symbol except P1/P2
inline bool is_edge(int k, int fft, bool ext = false) { return k == KMIN || k == kmax(fft, ext); }

enum class PilotType { NONE, EDGE, SCATTERED, CONTINUAL };
// Pilot type of cell (k, l) in a NORMAL data symbol (not P2, not frame closing).
// A carrier that is both continual and scattered counts as scattered (amplitude
// ASP, clause 9.2.4.3); edge and scattered pilots share ASP (clause 9.2.5).
inline PilotType data_pilot_type(int k, int l, int pp, int fft, bool ext = false) {
    if (k < KMIN || k > kmax(fft, ext)) return PilotType::NONE;
    if (is_edge(k, fft, ext)) return PilotType::EDGE;
    if (is_scattered(k, l, pp, fft, ext)) return PilotType::SCATTERED;
    if (is_continual(k, fft, pp, ext)) return PilotType::CONTINUAL;
    return PilotType::NONE;
}
// SISO pilot value c_{m,l,k} of a NORMAL data symbol (scattered / continual /
// edge pilot); returns 0 for a data cell. Before the 5/sqrt(27*Ktotal)
// normalisation of clause 9.5.
inline std::complex<double> pilot_value(int k, int l, int pp, int fft, bool ext = false) {
    double A;
    switch (data_pilot_type(k, l, pp, fft, ext)) {
        case PilotType::EDGE:
        case PilotType::SCATTERED: A = ASP[pp - 1]; break;
        case PilotType::CONTINUAL: A = ACP[fft_index(fft)]; break;
        default: return {0.0, 0.0};
    }
    return pilot_modulation(A, ref_seq(k, l, fft, ext));
}

// ---------------------------------------------------------------------------
// P2 pilots (clause 9.2.6) and P2 reserved tones (clause 9.3, table H.1)
// ---------------------------------------------------------------------------

// Table H.1: reserved carrier indices for the P2 symbol (normal-mode indices;
// extended mode: + Kext, clause 9.3), per FFT size
inline const std::vector<int> TR_P2_H1[NUM_FFT] = {
    /* 1K (10) */ {116, 130, 134, 157, 182, 256, 346, 478, 479, 532},
    /* 2K (18) */ {
      113, 124, 262, 467, 479, 727, 803, 862, 910, 946, 980, 1201, 1322, 1342, 1396, 1397, 1562,
      1565},
    /* 4K (36) */ {
      104, 116, 119, 163, 170, 173, 664, 886, 1064, 1151, 1196, 1264, 1531, 1736, 1951, 1960, 2069,
      2098, 2311, 2366, 2473, 2552, 2584, 2585, 2645, 2774, 2846, 2882, 3004, 3034, 3107, 3127,
      3148, 3191, 3283, 3289},
    /* 8K (72) */ {
      106, 109, 110, 112, 115, 118, 133, 142, 163, 184, 206, 247, 445, 461, 503, 565, 602, 656, 766,
      800, 922, 1094, 1108, 1199, 1258, 1726, 1793, 1939, 2128, 2714, 3185, 3365, 3541, 3655, 3770,
      3863, 4066, 4190, 4282, 4565, 4628, 4727, 4882, 4885, 5143, 5192, 5210, 5257, 5261, 5459,
      5651, 5809, 5830, 5986, 6020, 6076, 6253, 6269, 6410, 6436, 6467, 6475, 6509, 6556, 6611,
      6674, 6685, 6689, 6691, 6695, 6698, 6701},
    /* 16K (144) */ {
      104, 106, 107, 109, 110, 112, 113, 115, 116, 118, 119, 121, 122, 125, 128, 131, 134, 137, 140,
      143, 161, 223, 230, 398, 482, 497, 733, 809, 850, 922, 962, 1196, 1256, 1262, 1559, 1691,
      1801, 1819, 1937, 2005, 2095, 2308, 2383, 2408, 2425, 2428, 2479, 2579, 2893, 2902, 3086,
      3554, 4085, 4127, 4139, 4151, 4163, 4373, 4400, 4576, 4609, 4952, 4961, 5444, 5756, 5800,
      6094, 6208, 6658, 6673, 6799, 7208, 7682, 8101, 8135, 8230, 8692, 8788, 8933, 9323, 9449,
      9478, 9868, 10192, 10261, 10430, 10630, 10685, 10828, 10915, 10930, 10942, 11053, 11185,
      11324, 11369, 11468, 11507, 11542, 11561, 11794, 11912, 11974, 11978, 12085, 12179, 12193,
      12269, 12311, 12758, 12767, 12866, 12938, 12962, 12971, 13099, 13102, 13105, 13120, 13150,
      13280, 13282, 13309, 13312, 13321, 13381, 13402, 13448, 13456, 13462, 13463, 13466, 13478,
      13492, 13495, 13498, 13501, 13502, 13504, 13507, 13510, 13513, 13514, 13516},
    /* 32K (288) */ {
      104, 106, 107, 109, 110, 112, 113, 115, 118, 121, 124, 127, 130, 133, 136, 139, 142, 145, 148,
      151, 154, 157, 160, 163, 166, 169, 172, 175, 178, 181, 184, 187, 190, 193, 196, 199, 202, 205,
      208, 211, 404, 452, 455, 467, 509, 539, 568, 650, 749, 1001, 1087, 1286, 1637, 1823, 1835,
      1841, 1889, 1898, 1901, 2111, 2225, 2252, 2279, 2309, 2315, 2428, 2452, 2497, 2519, 3109,
      3154, 3160, 3170, 3193, 3214, 3298, 3331, 3346, 3388, 3397, 3404, 3416, 3466, 3491, 3500,
      3572, 4181, 4411, 4594, 4970, 5042, 5069, 5081, 5086, 5095, 5104, 5320, 5465, 5491, 6193,
      6541, 6778, 6853, 6928, 6934, 7030, 7198, 7351, 7712, 7826, 7922, 8194, 8347, 8350, 8435,
      8518, 8671, 8861, 8887, 9199, 9980, 10031, 10240, 10519, 10537, 10573, 10589, 11078, 11278,
      11324, 11489, 11642, 12034, 12107, 12184, 12295, 12635, 12643, 12941, 12995, 13001, 13133,
      13172, 13246, 13514, 13522, 13939, 14362, 14720, 14926, 15338, 15524, 15565, 15662, 15775,
      16358, 16613, 16688, 16760, 17003, 17267, 17596, 17705, 18157, 18272, 18715, 18994, 19249,
      19348, 20221, 20855, 21400, 21412, 21418, 21430, 21478, 21559, 21983, 21986, 22331, 22367,
      22370, 22402, 22447, 22535, 22567, 22571, 22660, 22780, 22802, 22844, 22888, 22907, 23021,
      23057, 23086, 23213, 23240, 23263, 23333, 23369, 23453, 23594, 24143, 24176, 24319, 24325,
      24565, 24587, 24641, 24965, 25067, 25094, 25142, 25331, 25379, 25465, 25553, 25589, 25594,
      25655, 25664, 25807, 25823, 25873, 25925, 25948, 26002, 26008, 26102, 26138, 26141, 26377,
      26468, 26498, 26510, 26512, 26578, 26579, 26588, 26594, 26597, 26608, 26627, 26642, 26767,
      26776, 26800, 26876, 26882, 26900, 26917, 26927, 26951, 26957, 26960, 26974, 26986, 27010,
      27013, 27038, 27044, 27053, 27059, 27061, 27074, 27076, 27083, 27086, 27092, 27094, 27098,
      27103, 27110, 27115, 27118, 27119, 27125, 27128, 27130, 27133, 27134, 27140, 27143, 27145,
      27146, 27148, 27149}
};
inline std::vector<int> tr_p2_positions(int fft, bool ext = false) {
    const int fi = fft_index(fft);
    if (ext && !KEXT[fi]) throw std::invalid_argument("dvbt2_std: extended carrier mode only exists for 8K/16K/32K");
    std::vector<int> v = TR_P2_H1[fi];
    if (ext)
        for (int& k : v) k += KEXT[fi];
    return v;
}
// P2 pilot positions:
//   SISO 32K: k mod 6 = 0; all other modes (incl. 32K MISO): k mod 3 = 0;
//   extended mode: also every k with Kmin <= k < Kmin+Kext or Kmax-Kext < k <= Kmax;
//   MISO only (clause 9.2.8): k = Kmin+1, Kmin+2, Kmax-2, Kmax-1 (extended:
//   Kmin+Kext+1, Kmin+Kext+2, Kmax-Kext-2, Kmax-Kext-1), and the cell next to
//   each P2 reserved tone k_i: k_i+1 if k_i mod 3 = 1, k_i-1 if k_i mod 3 = 2,
//   unless that neighbour is itself a reserved tone.
inline std::vector<uint8_t> compute_p2_pilot_map(int fft, bool ext, bool miso) {
    const int fi = fft_index(fft);
    const int km = kmax(fft, ext), ke = ext ? KEXT[fi] : 0;
    std::vector<uint8_t> m(static_cast<size_t>(km) + 1, 0);
    const int step = (fi == 5 && !miso) ? 6 : 3;
    for (int k = 0; k <= km; ++k) {
        if (k % step == 0) m[k] = 1;
        if (ext && (k < KMIN + ke || k > km - ke)) m[k] = 1;
    }
    if (miso) {
        const int lo = KMIN + ke, hi = km - ke;
        for (int k : {lo + 1, lo + 2, hi - 2, hi - 1}) m[k] = 1;
        const std::vector<int> S = tr_p2_positions(fft, ext);
        auto inS = [&S](int x) { return std::binary_search(S.begin(), S.end(), x); };
        for (int ki : S) {
            if (ki % 3 == 1 && !inS(ki + 1)) m[ki + 1] = 1;
            if (ki % 3 == 2 && !inS(ki - 1)) m[ki - 1] = 1;
        }
    }
    return m;
}
inline bool is_p2_pilot(int k, int fft, bool ext = false, bool miso = false) {
    static const auto cache = [] {
        std::array<std::array<std::array<std::vector<uint8_t>, 2>, 2>, NUM_FFT> c{};
        for (int fi = 0; fi < NUM_FFT; ++fi)
            for (int e = 0; e < 2; ++e)
                for (int mi = 0; mi < 2; ++mi)
                    if (!e || KEXT[fi]) c[fi][e][mi] = compute_p2_pilot_map(FFT_K[fi], e, mi);
        return c;
    }();
    if (k < KMIN || k > kmax(fft, ext)) return false;
    return cache[fft_index(fft)][ext ? 1 : 0][miso ? 1 : 0][static_cast<size_t>(k)];
}
inline double ap2(int fft, bool miso = false) { return (fft_index(fft) == 5 && !miso) ? AP2_32K_SISO : AP2_OTHER; }
// P2 pilot value (SISO, or MISO transmitter group 1); l = 0 .. NP2-1
inline std::complex<double> p2_pilot_value(int k, int l, int fft, bool ext = false, bool miso = false) {
    if (!is_p2_pilot(k, fft, ext, miso)) return {0.0, 0.0};
    return pilot_modulation(ap2(fft, miso), ref_seq(k, l, fft, ext));
}

// ---------------------------------------------------------------------------
// Frame closing symbol pilots (clause 9.2.7) - last symbol l = LF-1, only when
// frame_closing_used(); this symbol has NO scattered or continual pilots.
// ---------------------------------------------------------------------------

// k mod DX = 0 (k != Kmin, Kmax: those are edge pilots); additionally k = Kmax-1
// for 1K with PP4/PP5 and for 2K with PP7. (Kext is a multiple of every DX,
// so k mod DX is the same in normal- and extended-mode indexing.)
inline bool is_fc_pilot(int k, int fft, int pp, bool ext = false) {
    const int fi = fft_index(fft);
    const int km = kmax(fft, ext);
    if (k <= KMIN || k >= km) return false;
    if (k % DX[pp - 1] == 0) return true;
    if (k == km - 1 && ((fi == 0 && (pp == 4 || pp == 5)) || (fi == 1 && pp == 7))) return true;
    return false;
}
// Frame closing symbol pilot value (frame closing or edge pilot; amplitude ASP)
inline std::complex<double> fc_symbol_pilot_value(int k, int l, int pp, int fft, bool ext = false) {
    if (!(is_fc_pilot(k, fft, pp, ext) || is_edge(k, fft, ext))) return {0.0, 0.0};
    return pilot_modulation(ASP[pp - 1], ref_seq(k, l, fft, ext));
}

// ---------------------------------------------------------------------------
// P1 symbol (clause 9.8)
// ---------------------------------------------------------------------------

// Table 68: the 384 active carriers k_P1(0..383) of the 1K P1 symbol
// (k_P1(0..63) carry CSS_S1, k_P1(64..319) CSS_S2, k_P1(320..383) CSS_S1)
inline constexpr int P1_NUM_ACTIVE = 384;
inline constexpr int P1_ACTIVE_CARRIERS[P1_NUM_ACTIVE] = {
    44, 45, 47, 51, 54, 59, 62, 64, 65, 66, 70, 75, 78, 80, 81, 82, 84, 85, 87, 88, 89, 90, 94,
    96, 97, 98, 102, 107, 110, 112, 113, 114, 116, 117, 119, 120, 121, 122, 124, 125, 127, 131,
    132, 133, 135, 136, 137, 138, 142, 144, 145, 146, 148, 149, 151, 152, 153, 154, 158, 160, 161,
    162, 166, 171, 172, 173, 175, 179, 182, 187, 190, 192, 193, 194, 198, 203, 206, 208, 209, 210,
    212, 213, 215, 216, 217, 218, 222, 224, 225, 226, 230, 235, 238, 240, 241, 242, 244, 245, 247,
    248, 249, 250, 252, 253, 255, 259, 260, 261, 263, 264, 265, 266, 270, 272, 273, 274, 276, 277,
    279, 280, 281, 282, 286, 288, 289, 290, 294, 299, 300, 301, 303, 307, 310, 315, 318, 320, 321,
    322, 326, 331, 334, 336, 337, 338, 340, 341, 343, 344, 345, 346, 350, 352, 353, 354, 358, 363,
    364, 365, 367, 371, 374, 379, 382, 384, 385, 386, 390, 395, 396, 397, 399, 403, 406, 411, 412,
    413, 415, 419, 420, 421, 423, 424, 425, 426, 428, 429, 431, 435, 438, 443, 446, 448, 449, 450,
    454, 459, 462, 464, 465, 466, 468, 469, 471, 472, 473, 474, 478, 480, 481, 482, 486, 491, 494,
    496, 497, 498, 500, 501, 503, 504, 505, 506, 508, 509, 511, 515, 516, 517, 519, 520, 521, 522,
    526, 528, 529, 530, 532, 533, 535, 536, 537, 538, 542, 544, 545, 546, 550, 555, 558, 560, 561,
    562, 564, 565, 567, 568, 569, 570, 572, 573, 575, 579, 580, 581, 583, 584, 585, 586, 588, 589,
    591, 595, 598, 603, 604, 605, 607, 611, 612, 613, 615, 616, 617, 618, 622, 624, 625, 626, 628,
    629, 631, 632, 633, 634, 636, 637, 639, 643, 644, 645, 647, 648, 649, 650, 654, 656, 657, 658,
    660, 661, 663, 664, 665, 666, 670, 672, 673, 674, 678, 683, 684, 689, 692, 696, 698, 699, 701,
    702, 703, 704, 706, 707, 708, 712, 714, 715, 717, 718, 719, 720, 722, 723, 725, 726, 727, 729,
    733, 734, 735, 736, 738, 739, 740, 744, 746, 747, 748, 753, 756, 760, 762, 763, 765, 766, 767,
    768, 770, 771, 772, 776, 778, 779, 780, 785, 788, 792, 794, 795, 796, 801, 805, 806, 807, 809
};
// Table 69: S1 patterns (64 bits) for S1 = 000..111, S2 patterns (256 bits) for S2 = 0000..1111
inline constexpr const char* P1_S1_HEX[8] = {
    /* 000 */ "124721741D482E7B",
    /* 001 */ "47127421481D7B2E",
    /* 010 */ "217412472E7B1D48",
    /* 011 */ "742147127B2E481D",
    /* 100 */ "1D482E7B12472174",
    /* 101 */ "481D7B2E47127421",
    /* 110 */ "2E7B1D4821741247",
    /* 111 */ "7B2E481D74214712"
};
inline constexpr const char* P1_S2_HEX[16] = {
    /* 0000 */ "121D4748212E747B1D1248472E217B7412E247B721D174841DED48B82EDE7B8B",
    /* 0001 */ "4748121D747B212E48471D127B742E2147B712E2748421D148B81DED7B8B2EDE",
    /* 0010 */ "212E747B121D47482E217B741D12484721D1748412E247B72EDE7B8B1DED48B8",
    /* 0011 */ "747B212E4748121D7B742E2148471D12748421D147B712E27B8B2EDE48B81DED",
    /* 0100 */ "1D1248472E217B74121D4748212E747B1DED48B82EDE7B8B12E247B721D17484",
    /* 0101 */ "48471D127B742E214748121D747B212E48B81DED7B8B2EDE47B712E2748421D1",
    /* 0110 */ "2E217B741D124847212E747B121D47482EDE7B8B1DED48B821D1748412E247B7",
    /* 0111 */ "7B742E2148471D12747B212E4748121D7B8B2EDE48B81DED748421D147B712E2",
    /* 1000 */ "12E247B721D174841DED48B82EDE7B8B121D4748212E747B1D1248472E217B74",
    /* 1001 */ "47B712E2748421D148B81DED7B8B2EDE4748121D747B212E48471D127B742E21",
    /* 1010 */ "21D1748412E247B72EDE7B8B1DED48B8212E747B121D47482E217B741D124847",
    /* 1011 */ "748421D147B712E27B8B2EDE48B81DED747B212E4748121D7B742E2148471D12",
    /* 1100 */ "1DED48B82EDE7B8B12E247B721D174841D1248472E217B74121D4748212E747B",
    /* 1101 */ "48B81DED7B8B2EDE47B712E2748421D148471D127B742E214748121D747B212E",
    /* 1110 */ "2EDE7B8B1DED48B821D1748412E247B72E217B741D124847212E747B121D4748",
    /* 1111 */ "7B8B2EDE48B81DED748421D147B712E27B742E2148471D12747B212E4748121D"
};
// P1 timing in elementary periods T (clause 9.8.2.4.2): C = 542 T, A = 1024 T, B = 482 T
inline constexpr int P1_LEN_C = 542, P1_LEN_A = 1024, P1_LEN_B = 482;
// Clause 9.8.2.4.2: f_SH = 1/(1024 T)  (one 1K carrier spacing)

// BB-scrambler PRBS of clause 5.2.4 / figure 11: 1 + X^14 + X^15, 15-cell
// register (cell 1 = first character of 'init'); output bit = cell14 XOR
// cell15, which is also shifted into cell 1.
inline std::vector<uint8_t> bb_prbs(const std::string& init15, int n) {
    if (init15.size() != 15) throw std::invalid_argument("dvbt2_std::bb_prbs: init must have 15 bits");
    uint8_t reg[16];
    for (int i = 1; i <= 15; ++i) reg[i] = init15[static_cast<size_t>(i - 1)] == '1';
    std::vector<uint8_t> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const uint8_t b = reg[14] ^ reg[15];
        out[static_cast<size_t>(i)] = b;
        for (int c = 15; c >= 2; --c) reg[c] = reg[c - 1];
        reg[1] = b;
    }
    return out;
}

// MSS_SEQ (clause 9.8.2.2 step 1): {CSS_S1, CSS_S2, CSS_S1}, hex MSB first
inline std::array<uint8_t, P1_NUM_ACTIVE> p1_mss_seq(int s1, int s2) {
    if (s1 < 0 || s1 > 7 || s2 < 0 || s2 > 15) throw std::invalid_argument("dvbt2_std: S1 0..7, S2 0..15");
    std::array<uint8_t, P1_NUM_ACTIVE> seq{};
    auto bit = [](const char* h, int i) { return static_cast<uint8_t>((hex_value(h[i / 4]) >> (3 - i % 4)) & 1); };
    for (int i = 0; i < 64; ++i) seq[i] = seq[320 + i] = bit(P1_S1_HEX[s1], i);
    for (int i = 0; i < 256; ++i) seq[64 + i] = bit(P1_S2_HEX[s2], i);
    return seq;
}
// MSS_DIFF (step 2): DBPSK, MSS_DIFF_{-1} = +1, a '1' inverts
inline std::array<int, P1_NUM_ACTIVE> p1_mss_diff(int s1, int s2) {
    const auto seq = p1_mss_seq(s1, s2);
    std::array<int, P1_NUM_ACTIVE> d{};
    int prev = 1;
    for (int i = 0; i < P1_NUM_ACTIVE; ++i) prev = d[i] = seq[i] ? -prev : prev;
    return d;
}
// Scrambler (step 3): first 384 bits of the clause 5.2.4 PRBS with initial
// state 100111001000110; bit 0 -> +1, bit 1 -> -1
inline std::array<int, P1_NUM_ACTIVE> p1_scrambler() {
    const auto b = bb_prbs("100111001000110", P1_NUM_ACTIVE);
    std::array<int, P1_NUM_ACTIVE> s{};
    for (int i = 0; i < P1_NUM_ACTIVE; ++i) s[i] = b[static_cast<size_t>(i)] ? -1 : 1;
    return s;
}
// MSS_SCR_i = MSS_DIFF_i * 2(1/2 - PRBS_i): the +-1 value on carrier k_P1(i)
inline std::array<int, P1_NUM_ACTIVE> p1_mss_scr(int s1, int s2) {
    const auto d = p1_mss_diff(s1, s2);
    const auto s = p1_scrambler();
    std::array<int, P1_NUM_ACTIVE> r{};
    for (int i = 0; i < P1_NUM_ACTIVE; ++i) r[i] = d[i] * s[i];
    return r;
}
// P1 time-domain baseband waveform p1(t), sampled at t = n T, n = 0..2047
// (clause 9.8.2.4):
//   p1A(t) = 1/sqrt(384) * sum_i MSS_SCR_i * exp(j 2 pi (k_P1(i) - 426) t / (1024 T))
//   p1(t)  = p1A(t) exp(j 2 pi t/(1024T))            0     <= t < 542T  (C)
//            p1A(t - 542T)                           542T  <= t < 1566T (A)
//            p1A(t - 1024T) exp(j 2 pi t/(1024T))    1566T <= t < 2048T (B)
inline std::vector<std::complex<double>> p1_time_domain(int s1, int s2) {
    const auto v = p1_mss_scr(s1, s2);
    const double PI = std::acos(-1.0);
    std::vector<std::complex<double>> a(1024);
    for (int n = 0; n < 1024; ++n) {
        std::complex<double> acc = 0.0;
        for (int i = 0; i < P1_NUM_ACTIVE; ++i)
            acc += double(v[i]) * std::polar(1.0, 2.0 * PI * (P1_ACTIVE_CARRIERS[i] - 426) * n / 1024.0);
        a[static_cast<size_t>(n)] = acc / std::sqrt(384.0);
    }
    std::vector<std::complex<double>> p(TP1_T);
    for (int n = 0; n < TP1_T; ++n) {
        const std::complex<double> sh = std::polar(1.0, 2.0 * PI * n / 1024.0);
        if (n < 542) p[n] = a[n] * sh;
        else if (n < 1566) p[n] = a[n - 542];
        else p[n] = a[n - 1024] * sh;
    }
    return p;
}

}  // namespace dvbt2_std
