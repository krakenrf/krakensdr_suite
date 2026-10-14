// atsc3_tables.hpp - ATSC 3.0 physical layer (A/322:2024-04) constants, pilot
// tables and pilot reference generator for a BLIND receiver.
//
// Header-only, C++17, standard library only.  Every number below is taken
// from ATSC A/322:2024-04 "Physical Layer Protocol" (3 April 2024); the section
// / table it comes from is given next to it.  SISO only: MIMO pilot encoding
// (Annex L.9) and MISO pre-distortion (Section 8.2) are NOT applied here.
//
// Index conventions (A/322 Section 8.1.1):
//   * relative carrier index k = 0 .. NoC-1 (lowest-frequency active carrier = 0)
//   * absolute carrier index a = 0 .. NoCmax-1; a = k + Cred_coeff*Cunit/2
//     (Section 7.2.3).  Common continual pilots are defined in ABSOLUTE indices,
//     everything else (SP, edge, additional CP, preamble, SBS pilots and the
//     reference sequence r_k) in RELATIVE indices.
//   * carrier frequency offset from the channel centre: k' = k - (NoC-1)/2,
//     f = k' / TU (Section 8.3, psi_{m,l,k}).  NoC is always odd, so the
//     centre carrier k = (NoC-1)/2 exists.
//   * symbol index l restarts at 0 at the first symbol of every subframe and
//     counts subframe-boundary symbols too (Section 8.1.1, Section 8.3).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace atsc3_std {

// ---------------------------------------------------------------------------
// Sampling - Section 8.3: elementary period T = 1/(0.384 MHz x (16 + bsr_coefficient));
// bsr_coefficient values: Table 9.1 (2 = 6 MHz, 5 = 7 MHz, 8 = 8 MHz).
// Approximate T: Table 8.7 (0.1447 / 0.1240 / 0.1085 us).
// ---------------------------------------------------------------------------
inline constexpr double bsr_hz(int bsr_coefficient) { return 0.384e6 * (16 + bsr_coefficient); }
inline constexpr int BSR_COEFF_6MHZ = 2;   // Table 9.1
inline constexpr int BSR_COEFF_7MHZ = 5;   // Table 9.1
inline constexpr int BSR_COEFF_8MHZ = 8;   // Table 9.1

// ---------------------------------------------------------------------------
// FFT sizes - value = L1D_fft_size / L1B_first_sub_fft_size code (Table 9.10;
// code 3 reserved).  TU = 8192 T / 16384 T / 32768 T (Table 8.8).
// ---------------------------------------------------------------------------
enum class Fft : int { K8 = 0, K16 = 1, K32 = 2 };
inline constexpr int fidx(Fft f) { return static_cast<int>(f); }
inline constexpr int FFT_LEN[3] = {8192, 16384, 32768};         // Table 8.8 (TU in units of T)
inline constexpr int fft_len(Fft f) { return FFT_LEN[fidx(f)]; }

// Number of carriers - Section 7.2.3: NoC = NoCmax - Cred_coeff x Cunit,
// Cred_coeff = 0..4 (5..7 reserved, Table 9.2 / Section 9.3.3).
inline constexpr int NOC_MAX[3] = {6913, 13825, 27649};          // Section 7.2.3 / Table 7.1 (Cred_coeff = 0)
inline constexpr int C_UNIT[3]  = {96, 192, 384};                // Section 7.2.3
inline constexpr int CRED_MAX   = 4;                             // Section 7.2.3
inline constexpr int noc(Fft f, int cred) { return NOC_MAX[fidx(f)] - cred * C_UNIT[fidx(f)]; }
// absolute index of relative carrier 0 (Section 7.2.3: absolute range starts at Cred*Cunit/2)
inline constexpr int abs_offset(Fft f, int cred) { return cred * C_UNIT[fidx(f)] / 2; }
// relative index of the channel-centre carrier (k' = 0), Section 8.3
inline constexpr int centre_carrier(Fft f, int cred) { return (noc(f, cred) - 1) / 2; }
// ABSOLUTE index of the channel-centre carrier: (NoCmax-1)/2 = 3456 / 6912 / 13824.
// Derived from Sections 7.2.3 + 8.3: a = k + Cred*Cunit/2 and k_centre = (NoC-1)/2,
// so a carrier at offset k' from the centre has absolute index k' + abs_centre()
// for EVERY Cred_coeff (common CPs sit at fixed frequencies), while its relative
// index (SP grid, r_k) shifts by Cred*Cunit/2.
inline constexpr int abs_centre(Fft f) { return (NOC_MAX[fidx(f)] - 1) / 2; }
inline constexpr int relative_from_offset(Fft f, int cred, int kprime) { return kprime + centre_carrier(f, cred); }
// FFT bin (0..N-1, DC = bin 0) carrying relative carrier k (Section 8.3: the
// carrier is exp(j 2 pi k' t / TU) with TU = N T, so at t = nT it is bin k' mod N).
inline constexpr int fft_bin(Fft f, int cred, int k) {
    const int kp = k - centre_carrier(f, cred);
    const int n = fft_len(f);
    return ((kp % n) + n) % n;
}
// Table 7.1 (NoC for each Cred_coeff) - used by the self-test.
inline constexpr int NOC_TABLE_7_1[5][3] = {
    {6913, 13825, 27649}, {6817, 13633, 27265}, {6721, 13441, 26881},
    {6625, 13249, 26497}, {6529, 13057, 26113}};

// ---------------------------------------------------------------------------
// Guard intervals - Table 8.9 (samples at the baseband sampling rate).
// Index = L1D_guard_interval / L1B_first_sub_guard_interval code (Table 9.11):
// 0 reserved, 1 = GI1_192 ... 12 = GI12_4864, 13..15 reserved.
// NOTE (Section 8.5.1, 9.2.1): in time-aligned frames (L1B_frame_length_mode=0)
// every NON-Preamble symbol additionally carries L1B_excess_samples_per_symbol
// extra GI samples (0..8191, 13-bit field), so a blind receiver can measure a
// GI that is NOT in this table.
// ---------------------------------------------------------------------------
inline constexpr int GI_SAMPLES[13] = {0, 192, 384, 512, 768, 1024, 1536, 2048, 2432, 3072, 3648, 4096, 4864};
// Table 8.9 allowed-combination check marks [gi index][fft]
inline constexpr bool GI_ALLOWED[13][3] = {
    {false, false, false},                                       // 0 reserved
    {true, true, true}, {true, true, true}, {true, true, true},  // GI1..GI3
    {true, true, true}, {true, true, true}, {true, true, true},  // GI4..GI6
    {true, true, true},                                          // GI7_2048
    {false, true, true}, {false, true, true}, {false, true, true}, {false, true, true}, // GI8..GI11
    {false, false, true}};                                       // GI12_4864
inline constexpr bool gi_allowed(Fft f, int gi) { return gi >= 1 && gi <= 12 && GI_ALLOWED[gi][fidx(f)]; }

// ---------------------------------------------------------------------------
// Scattered pilot patterns - Table 8.2; index = L1D_scattered_pilot_pattern /
// L1B_first_sub_scattered_pilot_pattern code for SISO (Table 9.12; 16..31 reserved).
// ---------------------------------------------------------------------------
struct SpPattern { const char* name; int dx; int dy; };
enum SpIdx : int {
    SP3_2 = 0, SP3_4, SP4_2, SP4_4, SP6_2, SP6_4, SP8_2, SP8_4,
    SP12_2, SP12_4, SP16_2, SP16_4, SP24_2, SP24_4, SP32_2, SP32_4, SP_COUNT };
inline constexpr SpPattern SP_PATTERNS[SP_COUNT] = {
    {"SP3_2", 3, 2},   {"SP3_4", 3, 4},   {"SP4_2", 4, 2},   {"SP4_4", 4, 4},
    {"SP6_2", 6, 2},   {"SP6_4", 6, 4},   {"SP8_2", 8, 2},   {"SP8_4", 8, 4},
    {"SP12_2", 12, 2}, {"SP12_4", 12, 4}, {"SP16_2", 16, 2}, {"SP16_4", 16, 4},
    {"SP24_2", 24, 2}, {"SP24_4", 24, 4}, {"SP32_2", 32, 2}, {"SP32_4", 32, 4}};

// Table 8.3 - allowed SP patterns per (GI, FFT) in SISO mode, as bit masks (bit = SpIdx).
namespace detail {
constexpr std::uint32_t b(int i) { return 1u << i; }
}  // namespace detail
inline constexpr std::uint32_t SP_ALLOWED_MASK[13][3] = {
    /* 0 reserved */ {0, 0, 0},
    /* GI1_192   */ {detail::b(SP32_2) | detail::b(SP32_4) | detail::b(SP16_2) | detail::b(SP16_4),
                     detail::b(SP32_2) | detail::b(SP32_4),
                     detail::b(SP32_2)},
    /* GI2_384   */ {detail::b(SP16_2) | detail::b(SP16_4) | detail::b(SP8_2) | detail::b(SP8_4),
                     detail::b(SP32_2) | detail::b(SP32_4) | detail::b(SP16_2) | detail::b(SP16_4),
                     detail::b(SP32_2)},
    /* GI3_512   */ {detail::b(SP12_2) | detail::b(SP12_4) | detail::b(SP6_2) | detail::b(SP6_4),
                     detail::b(SP24_2) | detail::b(SP24_4) | detail::b(SP12_2) | detail::b(SP12_4),
                     detail::b(SP24_2)},
    /* GI4_768   */ {detail::b(SP8_2) | detail::b(SP8_4) | detail::b(SP4_2) | detail::b(SP4_4),
                     detail::b(SP16_2) | detail::b(SP16_4) | detail::b(SP8_2) | detail::b(SP8_4),
                     detail::b(SP32_2) | detail::b(SP16_2)},
    /* GI5_1024  */ {detail::b(SP6_2) | detail::b(SP6_4) | detail::b(SP3_2) | detail::b(SP3_4),
                     detail::b(SP12_2) | detail::b(SP12_4) | detail::b(SP6_2) | detail::b(SP6_4),
                     detail::b(SP24_2) | detail::b(SP12_2)},
    /* GI6_1536  */ {detail::b(SP4_2) | detail::b(SP4_4),
                     detail::b(SP8_2) | detail::b(SP8_4) | detail::b(SP4_2) | detail::b(SP4_4),
                     detail::b(SP16_2) | detail::b(SP8_2)},
    /* GI7_2048  */ {detail::b(SP3_2) | detail::b(SP3_4),
                     detail::b(SP6_2) | detail::b(SP6_4) | detail::b(SP3_2) | detail::b(SP3_4),
                     detail::b(SP12_2) | detail::b(SP6_2)},
    /* GI8_2432  */ {0,
                     detail::b(SP6_2) | detail::b(SP6_4) | detail::b(SP3_2) | detail::b(SP3_4),
                     detail::b(SP12_2) | detail::b(SP6_2)},
    /* GI9_3072  */ {0, detail::b(SP4_2) | detail::b(SP4_4), detail::b(SP8_2) | detail::b(SP3_2)},
    /* GI10_3648 */ {0, detail::b(SP4_2) | detail::b(SP4_4), detail::b(SP8_2) | detail::b(SP3_2)},
    /* GI11_4096 */ {0, detail::b(SP3_2) | detail::b(SP3_4), detail::b(SP6_2) | detail::b(SP3_2)},
    /* GI12_4864 */ {0, 0, detail::b(SP6_2) | detail::b(SP3_2)}};
inline constexpr bool sp_allowed(Fft f, int gi, int sp) {
    return gi >= 1 && gi <= 12 && sp >= 0 && sp < SP_COUNT && (SP_ALLOWED_MASK[gi][fidx(f)] & (1u << sp)) != 0;
}
// SP pattern usable with this FFT size for SOME guard interval (Table 8.3 column union).
inline constexpr bool sp_allowed_any_gi(Fft f, int sp) {
    for (int gi = 1; gi <= 12; ++gi) if (sp_allowed(f, gi, sp)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Scattered pilot boost - Table 9.14 (EXACT power in dB), index
// [SpIdx][L1D_scattered_pilot_boost 0..4]; 5..7 = RFU.  A_SP = 10^(dB/20)
// (Section 8.1.3.2: "any amplitude value shall be calculated from the exact power
// values listed in Table 9.14"; Table 9.15 holds only rounded amplitudes).
// Used for scattered, edge, additional continual and subframe-boundary pilots.
// ---------------------------------------------------------------------------
inline constexpr double SP_BOOST_DB[SP_COUNT][5] = {
    {0.00, 0.00, 1.40, 2.20, 2.90},   // SP3_2
    {0.00, 1.40, 2.90, 3.80, 4.40},   // SP3_4
    {0.00, 0.60, 2.10, 3.00, 3.60},   // SP4_2
    {0.00, 2.10, 3.60, 4.40, 5.10},   // SP4_4
    {0.00, 1.60, 3.10, 4.00, 4.60},   // SP6_2
    {0.00, 3.00, 4.50, 5.40, 6.00},   // SP6_4
    {0.00, 2.20, 3.80, 4.60, 5.30},   // SP8_2
    {0.00, 3.60, 5.10, 6.00, 6.60},   // SP8_4
    {0.00, 3.20, 4.70, 5.60, 6.20},   // SP12_2
    {0.00, 4.50, 6.00, 6.90, 7.50},   // SP12_4
    {0.00, 3.80, 5.30, 6.20, 6.80},   // SP16_2
    {0.00, 5.20, 6.70, 7.60, 8.20},   // SP16_4
    {0.00, 4.70, 6.20, 7.10, 7.70},   // SP24_2
    {0.00, 6.10, 7.60, 8.50, 9.10},   // SP24_4
    {0.00, 5.40, 6.90, 7.70, 8.40},   // SP32_2
    {0.00, 6.70, 8.20, 9.10, 9.70}};  // SP32_4
inline double sp_amplitude(int sp, int boost) { return std::pow(10.0, SP_BOOST_DB[sp][boost] / 20.0); }

// Common continual pilot boost - Table 8.5: A_CP (dB) = 8.52 for 8K/16K/32K
// ("dB values are exact"; linear 2.67 approximate).  NOTE: the total-power
// tables of Annex I are reproduced to 0.01 only with A_CP = 8/3 exactly
// (20log10(8/3) = 8.519 dB); 10^(8.52/20) = 2.66686 vs 8/3 = 2.66667 (0.007 %).
inline constexpr double CP_BOOST_DB = 8.52;
inline double cp_amplitude() { return std::pow(10.0, CP_BOOST_DB / 20.0); }
inline constexpr double CP_AMPLITUDE_ANNEX_I = 8.0 / 3.0;

// ---------------------------------------------------------------------------
// Preamble - Table 8.6 (exact power dB, approx amplitude) = the 32 distinct
// (FFT, GI, DX) configurations of preamble_structure (Table H.1.1).  The
// Preamble uses DY = 1 and pilots at k mod DX == 0 (Section 8.1.6.1).
// preamble_structure p (0..159; 160..255 reserved): configuration p / 5,
// L1-Basic FEC mode p % 5 + 1 (Table H.1.1 lists the 32 configurations in
// this order, 5 FEC modes each).
// ---------------------------------------------------------------------------
struct PreambleCfg { Fft fft; int gi_samples; int dx; double power_db; double amp_approx; };
inline constexpr PreambleCfg PREAMBLE_CFG[32] = {
    {Fft::K8, 192, 16, 5.30, 1.841},  {Fft::K8, 384, 8, 3.60, 1.514},   {Fft::K8, 512, 6, 2.90, 1.396},
    {Fft::K8, 768, 4, 1.80, 1.230},   {Fft::K8, 1024, 3, 0.90, 1.109},  {Fft::K8, 1536, 4, 1.80, 1.230},
    {Fft::K8, 2048, 3, 0.90, 1.109},
    {Fft::K16, 192, 32, 6.80, 2.188}, {Fft::K16, 384, 16, 5.30, 1.841}, {Fft::K16, 512, 12, 4.60, 1.698},
    {Fft::K16, 768, 8, 3.60, 1.514},  {Fft::K16, 1024, 6, 2.90, 1.396}, {Fft::K16, 1536, 4, 2.10, 1.274},
    {Fft::K16, 2048, 3, 1.30, 1.161}, {Fft::K16, 2432, 3, 1.30, 1.161}, {Fft::K16, 3072, 4, 2.10, 1.274},
    {Fft::K16, 3648, 4, 2.10, 1.274}, {Fft::K16, 4096, 3, 1.30, 1.161},
    {Fft::K32, 192, 32, 6.80, 2.188}, {Fft::K32, 384, 32, 6.80, 2.188}, {Fft::K32, 512, 24, 6.20, 2.042},
    {Fft::K32, 768, 16, 5.30, 1.841}, {Fft::K32, 1024, 12, 4.60, 1.698}, {Fft::K32, 1536, 8, 4.00, 1.585},
    {Fft::K32, 2048, 6, 3.20, 1.445}, {Fft::K32, 2432, 6, 3.20, 1.445}, {Fft::K32, 3072, 8, 4.00, 1.585},
    {Fft::K32, 3072, 3, 1.30, 1.161}, {Fft::K32, 3648, 8, 4.00, 1.585}, {Fft::K32, 3648, 3, 1.30, 1.161},
    {Fft::K32, 4096, 3, 1.30, 1.161}, {Fft::K32, 4864, 3, 1.30, 1.161}};
inline constexpr int PREAMBLE_STRUCTURE_MAX = 159;   // Table H.1.1
inline constexpr const PreambleCfg& preamble_structure_cfg(int p) { return PREAMBLE_CFG[p / 5]; }
inline constexpr int preamble_structure_l1b_mode(int p) { return p % 5 + 1; }
inline double preamble_amplitude(const PreambleCfg& c) { return std::pow(10.0, c.power_db / 20.0); }
// Section 7.2.5.1 / 8.3: the FIRST Preamble symbol always uses the minimum NoC
// of its FFT size (= Cred_coeff 4); the others use L1B_preamble_reduced_carriers.
inline constexpr int FIRST_PREAMBLE_CRED = 4;

// ---------------------------------------------------------------------------
// Pilot reference sequence r_k - Section 8.1.2, Figure 8.2.
// 13-stage shift register x1..x13, G(x) = 1 + X^9 + X^10 + X^12 + X^13,
// initial content x1..x13 = 0000 0000 1101 1, reloaded at the start of EVERY
// symbol; output r_k = x13; feedback x9 ^ x10 ^ x12 ^ x13 into x1.  k is the
// RELATIVE carrier index, so r_k is the same for every symbol (no dependence
// on l or m).  First 24 values per the standard: 1101 1000 0000 0001 0100 0000.
// ---------------------------------------------------------------------------
inline std::vector<std::uint8_t> make_reference_sequence(int n) {
    std::array<std::uint8_t, 14> x{};                  // x[1..13]
    const std::uint8_t init[13] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1};
    for (int i = 0; i < 13; ++i) x[i + 1] = init[i];
    std::vector<std::uint8_t> r(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        r[static_cast<std::size_t>(k)] = x[13];
        const std::uint8_t fb = x[9] ^ x[10] ^ x[12] ^ x[13];
        for (int i = 13; i > 1; --i) x[i] = x[i - 1];
        x[1] = fb;
    }
    return r;
}
inline const std::vector<std::uint8_t>& reference_sequence() {
    static const std::vector<std::uint8_t> r = make_reference_sequence(NOC_MAX[2]);
    return r;
}
// 2 (1/2 - r_k) = +1 for r_k = 0, -1 for r_k = 1 (Sections 8.1.3.3, 8.1.4.3, 8.1.5, 8.1.6.3, 8.1.7.3)
inline double ref_bpsk(int k) { return reference_sequence()[static_cast<std::size_t>(k)] ? -1.0 : 1.0; }

// ---------------------------------------------------------------------------
// Common continual pilots, ABSOLUTE carrier indices.
// CP32: Table D.1.1 (192 values).  CP16: Table D.1.2 (96).  CP8: Table D.1.3 (48).
// CP16(k') = ceil(CP32(2k')/2), CP8(k'') = ceil(CP32(4k'')/4) (Section 8.1.4.1).
// Positions outside the configured carriers are not transmitted (Section 8.1.4.1,
// Annex D.1); resulting counts: Table 8.4.
// ---------------------------------------------------------------------------
inline constexpr int CP32_ABS[192] = {   // Table D.1.1
    236, 316, 356, 412, 668, 716, 868, 1100, 1228, 1268, 1340, 1396,
    1876, 1916, 2140, 2236, 2548, 2644, 2716, 2860, 3004, 3164, 3236, 3436,
    3460, 3700, 3836, 4028, 4124, 4132, 4156, 4316, 4636, 5012, 5132, 5140,
    5332, 5372, 5500, 5524, 5788, 6004, 6020, 6092, 6428, 6452, 6500, 6740,
    7244, 7316, 7372, 7444, 7772, 7844, 7924, 8020, 8164, 8308, 8332, 8348,
    8788, 8804, 9116, 9140, 9292, 9412, 9436, 9604, 10076, 10204, 10340, 10348,
    10420, 10660, 10684, 10708, 11068, 11132, 11228, 11356, 11852, 11860, 11884, 12044,
    12116, 12164, 12268, 12316, 12700, 12772, 12820, 12988, 13300, 13340, 13564, 13780,
    13868, 14084, 14308, 14348, 14660, 14828, 14876, 14948, 15332, 15380, 15484, 15532,
    15604, 15764, 15788, 15796, 16292, 16420, 16516, 16580, 16940, 16964, 16988, 17228,
    17300, 17308, 17444, 17572, 18044, 18212, 18236, 18356, 18508, 18532, 18844, 18860,
    19300, 19316, 19340, 19484, 19628, 19724, 19804, 19876, 20204, 20276, 20332, 20404,
    20908, 21148, 21196, 21220, 21556, 21628, 21644, 21860, 22124, 22148, 22276, 22316,
    22508, 22516, 22636, 23012, 23332, 23492, 23516, 23524, 23620, 23812, 23948, 24188,
    24212, 24412, 24484, 24644, 24788, 24932, 25004, 25100, 25412, 25508, 25732, 25772,
    26252, 26308, 26380, 26420, 26548, 26780, 26932, 26980, 27236, 27292, 27332, 27412};
inline constexpr int CP16_ABS[96] = {    // Table D.1.2
    118, 178, 334, 434, 614, 670, 938, 1070, 1274, 1358, 1502, 1618,
    1730, 1918, 2062, 2078, 2318, 2566, 2666, 2750, 2894, 3010, 3214, 3250,
    3622, 3686, 3886, 3962, 4082, 4166, 4394, 4558, 4646, 4718, 5038, 5170,
    5210, 5342, 5534, 5614, 5926, 5942, 6058, 6134, 6350, 6410, 6650, 6782,
    6934, 7154, 7330, 7438, 7666, 7742, 7802, 7894, 8146, 8258, 8470, 8494,
    8650, 8722, 9022, 9118, 9254, 9422, 9650, 9670, 9814, 9902, 10102, 10166,
    10454, 10598, 10778, 10822, 11062, 11138, 11254, 11318, 11666, 11758, 11810, 11974,
    12106, 12242, 12394, 12502, 12706, 12866, 13126, 13190, 13274, 13466, 13618, 13666};
inline constexpr int CP8_ABS[48] = {     // Table D.1.3
    59, 167, 307, 469, 637, 751, 865, 1031, 1159, 1333, 1447, 1607,
    1811, 1943, 2041, 2197, 2323, 2519, 2605, 2767, 2963, 3029, 3175, 3325,
    3467, 3665, 3833, 3901, 4073, 4235, 4325, 4511, 4627, 4825, 4907, 5051,
    5227, 5389, 5531, 5627, 5833, 5905, 6053, 6197, 6353, 6563, 6637, 6809};
inline constexpr int CP_COUNT_MAX[3] = {48, 96, 192};
// Table 8.4: number of common CPs per [Cred_coeff][fft]
inline constexpr int COMMON_CP_COUNT[5][3] = {{48, 96, 192}, {48, 96, 192}, {47, 93, 186}, {46, 92, 184}, {45, 90, 180}};
inline const int* cp_abs_table(Fft f) {
    return f == Fft::K8 ? CP8_ABS : (f == Fft::K16 ? CP16_ABS : CP32_ABS);
}
inline bool is_common_cp(Fft f, int cred, int k) {
    if (k < 0 || k >= noc(f, cred)) return false;
    const int* t = cp_abs_table(f);
    return std::binary_search(t, t + CP_COUNT_MAX[fidx(f)], k + abs_offset(f, cred));
}
// common CPs as RELATIVE indices for this Cred_coeff (sorted)
inline std::vector<int> common_cp_relative(Fft f, int cred) {
    std::vector<int> v;
    const int* t = cp_abs_table(f);
    for (int i = 0; i < CP_COUNT_MAX[fidx(f)]; ++i) {
        const int k = t[i] - abs_offset(f, cred);
        if (k >= 0 && k < noc(f, cred)) v.push_back(k);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Additional ("scattered pilot bearing") continual pilots, RELATIVE indices.
// Table D.1.4 [fft][SpIdx] up to 3 entries, 0 = none.  Entries the standard
// puts in parentheses are flagged: "not used if Cred_coeff is an odd number".
// 8K SP32_4 is given by Table D.1.5 instead (per Cred_coeff).
// Combinations left blank in Table D.1.4: no additional CP.
// ---------------------------------------------------------------------------
struct AddCp { int k; bool not_if_odd_cred; };
inline constexpr AddCp ADD_CP[3][SP_COUNT][3] = {
    // 8K
    {{{1731, false}, {0, false}, {0, false}},          // SP3_2
     {{1731, false}, {2886, false}, {5733, false}},    // SP3_4
     {{1732, false}, {0, false}, {0, false}},          // SP4_2
     {{1732, false}, {2888, false}, {5724, false}},    // SP4_4
     {{1734, false}, {0, false}, {0, false}},          // SP6_2
     {{1734, false}, {2892, false}, {5730, false}},    // SP6_4
     {{1736, false}, {0, false}, {0, false}},          // SP8_2
     {{1736, false}, {2896, false}, {5720, false}},    // SP8_4
     {{1740, false}, {0, false}, {0, false}},          // SP12_2
     {{1740, false}, {2904, false}, {5748, false}},    // SP12_4
     {{1744, false}, {0, false}, {0, false}},          // SP16_2
     {{1744, false}, {2912, true}, {5744, true}},      // SP16_4  (2912), (5744)
     {{0, false}, {0, false}, {0, false}},             // SP24_2  blank
     {{0, false}, {0, false}, {0, false}},             // SP24_4  blank
     {{1696, true}, {0, false}, {0, false}},           // SP32_2  (1696)
     {{0, false}, {0, false}, {0, false}}},            // SP32_4  -> Table D.1.5
    // 16K
    {{{3471, false}, {0, false}, {0, false}},          // SP3_2
     {{3471, false}, {5778, false}, {11469, false}},   // SP3_4
     {{3460, false}, {0, false}, {0, false}},          // SP4_2
     {{3460, false}, {5768, false}, {11452, false}},   // SP4_4
     {{3462, false}, {0, false}, {0, false}},          // SP6_2
     {{3462, false}, {5772, false}, {11466, false}},   // SP6_4
     {{3464, false}, {0, false}, {0, false}},          // SP8_2
     {{3464, false}, {5776, false}, {11448, false}},   // SP8_4
     {{3468, false}, {0, false}, {0, false}},          // SP12_2
     {{3468, false}, {5784, false}, {11460, false}},   // SP12_4
     {{3472, false}, {0, false}, {0, false}},          // SP16_2
     {{3472, false}, {5792, false}, {11440, false}},   // SP16_4
     {{3480, false}, {0, false}, {0, false}},          // SP24_2
     {{3480, false}, {5808, false}, {11496, false}},   // SP24_4
     {{3488, false}, {0, false}, {0, false}},          // SP32_2
     {{3488, false}, {5824, true}, {11488, true}}},    // SP32_4  3488, (5824), (11488)
    // 32K (only *_2 patterns have entries; blank cells = none)
    {{{6939, false}, {0, false}, {0, false}},          // SP3_2
     {{0, false}, {0, false}, {0, false}},             // SP3_4
     {{0, false}, {0, false}, {0, false}},             // SP4_2
     {{0, false}, {0, false}, {0, false}},             // SP4_4
     {{6942, false}, {0, false}, {0, false}},          // SP6_2
     {{0, false}, {0, false}, {0, false}},             // SP6_4
     {{6920, false}, {0, false}, {0, false}},          // SP8_2
     {{0, false}, {0, false}, {0, false}},             // SP8_4
     {{6924, false}, {0, false}, {0, false}},          // SP12_2
     {{0, false}, {0, false}, {0, false}},             // SP12_4
     {{6928, false}, {0, false}, {0, false}},          // SP16_2
     {{0, false}, {0, false}, {0, false}},             // SP16_4
     {{6936, false}, {0, false}, {0, false}},          // SP24_2
     {{0, false}, {0, false}, {0, false}},             // SP24_4
     {{6944, false}, {0, false}, {0, false}},          // SP32_2
     {{0, false}, {0, false}, {0, false}}}};           // SP32_4
// Table D.1.5: 8K SP32_4 additional CPs per Cred_coeff (0 = none).
inline constexpr int ADD_CP_8K_SP32_4[5][3] = {
    {1696, 2880, 5728}, {0, 0, 0}, {1696, 0, 0}, {1696, 2880, 0}, {1696, 2880, 5728}};

// Additional CPs (relative indices) actually used for (fft, sp, Cred_coeff).
inline std::vector<int> additional_cp_relative(Fft f, int sp, int cred) {
    std::vector<int> v;
    if (f == Fft::K8 && sp == SP32_4) {
        for (int i = 0; i < 3; ++i) if (ADD_CP_8K_SP32_4[cred][i]) v.push_back(ADD_CP_8K_SP32_4[cred][i]);
    } else {
        for (int i = 0; i < 3; ++i) {
            const AddCp& e = ADD_CP[fidx(f)][sp][i];
            if (e.k == 0) continue;
            if (e.not_if_odd_cred && (cred & 1)) continue;
            v.push_back(e.k);
        }
    }
    v.erase(std::remove_if(v.begin(), v.end(), [&](int k) { return k >= noc(f, cred); }), v.end());
    return v;
}
inline bool is_additional_cp(Fft f, int sp, int cred, int k) {
    if (f == Fft::K8 && sp == SP32_4) {
        for (int i = 0; i < 3; ++i) if (ADD_CP_8K_SP32_4[cred][i] && ADD_CP_8K_SP32_4[cred][i] == k) return true;
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const AddCp& e = ADD_CP[fidx(f)][sp][i];
        if (e.k != 0 && e.k == k && !(e.not_if_odd_cred && (cred & 1))) return k < noc(f, cred);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Scattered pilot position - Section 8.1.3.1: relative carrier k of symbol l is
// a scattered pilot iff  k mod (DX DY) = DX (l mod DY).  (Data symbols only;
// no SPs in Preamble or subframe-boundary symbols, Section 8.1.3.)
// ---------------------------------------------------------------------------
inline constexpr bool is_scattered(int k, int l, int dx, int dy) {
    return k >= 0 && l >= 0 && (k % (dx * dy)) == dx * (l % dy);
}

// ---------------------------------------------------------------------------
// Cell classification + pilot values.
// ---------------------------------------------------------------------------
enum class Cell : std::uint8_t { Data = 0, Scattered, CommonCP, AdditionalCP, Edge, Preamble, Boundary };

// Data symbol (Table 8.1: SP, common CP, additional CP, edge pilots).
// Edge pilots: k = 0 and k = NoC-1 in every non-Preamble symbol (Section 8.1.5).
// Coincidences: edge/SP/additional CP all use A_SP and r_k, so their value is
// identical; common CPs never coincide with SP/edge locations for the allowed
// combinations (Section 8.1.4.1, verified by the self-test).
inline Cell data_symbol_cell(Fft f, int cred, int sp, int k, int l) {
    const int n = noc(f, cred);
    if (k < 0 || k >= n) return Cell::Data;
    if (k == 0 || k == n - 1) return Cell::Edge;
    if (is_scattered(k, l, SP_PATTERNS[sp].dx, SP_PATTERNS[sp].dy)) return Cell::Scattered;
    if (is_common_cp(f, cred, k)) return Cell::CommonCP;
    if (is_additional_cp(f, sp, cred, k)) return Cell::AdditionalCP;
    return Cell::Data;
}

// Known transmitted value c_{m,l,k} of a pilot cell of a SISO DATA symbol
// (before IFFT power normalisation 1/sqrt(P_data,m), Section 8.3 / Annex I.2);
// returns 0 for a data cell.  boost = L1D_scattered_pilot_boost (0..4).
//   SP / edge / additional CP: Re = 2 A_SP (1/2 - r_k)   (8.1.3.3, 8.1.5, 8.1.4.2)
//   common CP:                 Re = 2 A_CP (1/2 - r_k)   (8.1.4.3)
inline std::complex<double> pilot_value(Fft f, int cred, int sp, int boost, int k, int l) {
    switch (data_symbol_cell(f, cred, sp, k, l)) {
        case Cell::Scattered:
        case Cell::AdditionalCP:
        case Cell::Edge:     return {sp_amplitude(sp, boost) * ref_bpsk(k), 0.0};
        case Cell::CommonCP: return {cp_amplitude() * ref_bpsk(k), 0.0};
        default:             return {0.0, 0.0};
    }
}

// Subframe-boundary symbol (Section 8.1.7): pilots at k mod DX = 0 (k = 0 and
// NoC-1 are edge pilots, same A_SP), plus common + additional CPs (Table 8.1).
inline Cell sbs_cell(Fft f, int cred, int sp, int k) {
    const int n = noc(f, cred);
    if (k < 0 || k >= n) return Cell::Data;
    if (k == 0 || k == n - 1) return Cell::Edge;
    if (k % SP_PATTERNS[sp].dx == 0) return Cell::Boundary;
    if (is_common_cp(f, cred, k)) return Cell::CommonCP;
    if (is_additional_cp(f, sp, cred, k)) return Cell::AdditionalCP;
    return Cell::Data;   // includes the SBS null cells (Section 7.2.6.4)
}
inline std::complex<double> sbs_pilot_value(Fft f, int cred, int sp, int boost, int k) {
    switch (sbs_cell(f, cred, sp, k)) {
        case Cell::Boundary:
        case Cell::AdditionalCP:
        case Cell::Edge:     return {sp_amplitude(sp, boost) * ref_bpsk(k), 0.0};
        case Cell::CommonCP: return {cp_amplitude() * ref_bpsk(k), 0.0};
        default:             return {0.0, 0.0};
    }
}

// Preamble symbol (Section 8.1.6, Table 8.1): preamble pilots at k mod DX = 0
// (DY = 1) with A_Preamble (Table 8.6), plus common CPs.  No edge pilots, no
// additional CPs.  cred: 4 for the first Preamble symbol (Section 7.2.5.1),
// L1B_preamble_reduced_carriers for the others.
inline Cell preamble_cell(const PreambleCfg& c, int cred, int k) {
    if (k < 0 || k >= noc(c.fft, cred)) return Cell::Data;
    if (k % c.dx == 0) return Cell::Preamble;
    if (is_common_cp(c.fft, cred, k)) return Cell::CommonCP;
    return Cell::Data;
}
inline std::complex<double> preamble_pilot_value(const PreambleCfg& c, int cred, int k) {
    switch (preamble_cell(c, cred, k)) {
        case Cell::Preamble: return {preamble_amplitude(c) * ref_bpsk(k), 0.0};
        case Cell::CommonCP: return {cp_amplitude() * ref_bpsk(k), 0.0};
        default:             return {0.0, 0.0};
    }
}

// ---------------------------------------------------------------------------
// Precomputed pilot map for one data-symbol configuration: pilot carriers and
// values for each of the DY symbol phases (l mod DY).
// ---------------------------------------------------------------------------
struct DataPilotMap {
    Fft fft; int cred; int sp; int boost; int noc_; int dy;
    std::vector<std::vector<int>> k;                         // [l mod DY] pilot carriers (relative, ascending)
    std::vector<std::vector<std::complex<double>>> v;         // [l mod DY] values
    DataPilotMap(Fft f, int cred_, int sp_, int boost_)
        : fft(f), cred(cred_), sp(sp_), boost(boost_), noc_(noc(f, cred_)), dy(SP_PATTERNS[sp_].dy),
          k(static_cast<std::size_t>(dy)), v(static_cast<std::size_t>(dy)) {
        for (int ph = 0; ph < dy; ++ph)
            for (int kk = 0; kk < noc_; ++kk) {
                const std::complex<double> c = pilot_value(f, cred, sp, boost, kk, ph);
                if (data_symbol_cell(f, cred, sp, kk, ph) != Cell::Data) {
                    k[static_cast<std::size_t>(ph)].push_back(kk);
                    v[static_cast<std::size_t>(ph)].push_back(c);
                }
            }
    }
    int data_cells(int l) const { return noc_ - static_cast<int>(k[static_cast<std::size_t>(l % dy)].size()); }
};

}  // namespace atsc3_std
