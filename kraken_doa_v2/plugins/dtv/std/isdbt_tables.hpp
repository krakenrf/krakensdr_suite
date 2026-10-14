// isdbt_tables.hpp - ISDB-T known-signal constants and tables
//
// Source: ARIB STD-B31 Version 1.6-E2 ("Transmission System for Digital
// Terrestrial Television Broadcasting"), Part 1 Chapter 3 unless noted.
// Every constant carries its section / table reference. Values marked
// DERIVED are computed from stipulated values, not printed in the standard.
//
// Header-only, C++17, standard library only.
//
// Conventions used throughout:
//   mode   : 1, 2 or 3
//   seg    : OFDM segment NUMBER 0..12 (the standard's label, NOT the
//            frequency position)
//   pos    : frequency position 0..12 of a segment (0 = lowest frequency,
//            = segment 11)                                    (Fig. 3-32)
//   c      : carrier number within a segment, 0..carriers_per_segment-1
//            (0 = lowest frequency in the segment)   (Fig. 3-29, 3-30)
//   k      : carrier number across the whole band, 0..K-1, 0 = carrier 0
//            of segment 11                                    (Sec. 3.14.1)
//   n      : OFDM symbol number within the frame, 0..203    (Fig. 3-30)
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace isdbt_std {

// ===========================================================================
// 1. Basic OFDM parameters
// ===========================================================================

inline constexpr int kNumSegments = 13;        // Table 3-2 (Ns = 13)
inline constexpr int kSymbolsPerFrame = 204;   // Table 3-1, Table 3-2

// IFFT sampling frequency: 512/63 MHz = 8.12698... MHz (Table 3-1).
// Ch. 4 Sec. 4.3 (Ordinance): fs = 8,126,984 Hz, permissible deviation
// +/-0.3 ppm; 512/63 MHz "may be used if the permissible deviation
// requirement is met".
inline constexpr double kIfftSampleRateHz = 512.0e6 / 63.0;
inline constexpr double kIfftSampleRateOrdinanceHz = 8126984.0;

// Segment bandwidth 3000/7 kHz = 428.57... kHz (Table 3-1).
inline constexpr double kSegmentBandwidthHz = 3000.0e3 / 7.0;

constexpr bool valid_mode(int mode) { return mode >= 1 && mode <= 3; }

// Carriers per OFDM segment: 108 / 216 / 432 (Table 3-1 "Total count").
constexpr int carriers_per_segment(int mode) { return 108 << (mode - 1); }

// Data carriers per segment: 96 / 192 / 384 (Table 3-1 "Data", Sec. 3.9.5).
constexpr int data_carriers_per_segment(int mode) { return 96 << (mode - 1); }

// Total carriers K = 108*Ns+1 = 1405 / 216*Ns+1 = 2809 / 432*Ns+1 = 5617
// (Table 3-2, Sec. 3.14.1 "K"). The +1 is the continuous carrier at the
// right-hand (upper) end of the band (Sec. 3.14, Table 3-19).
constexpr int total_carriers(int mode) {
    return kNumSegments * carriers_per_segment(mode) + 1;
}

// Kc, carrier number of the RF centre frequency: 702 / 1404 / 2808
// (Sec. 3.14.1 "Kc"; Appendix (Operation Guidelines) Table 2-1). Equals (K-1)/2.
constexpr int center_carrier(int mode) { return 702 << (mode - 1); }

// Carrier spacing 250/63 kHz (3.968... kHz), 125/63 kHz (1.9841... kHz),
// 125/126 kHz (0.99206... kHz) (Table 3-1).
constexpr double carrier_spacing_hz(int mode) {
    return (250.0e3 / 63.0) / static_cast<double>(1 << (mode - 1));
}

// Effective (useful) symbol length Tu: 252 us / 504 us / 1008 us (Table 3-1).
constexpr double useful_symbol_s(int mode) {
    return 252.0e-6 * static_cast<double>(1 << (mode - 1));
}

// FFT size N = Tu * fs = 2048 / 4096 / 8192.
// DERIVED (Tu from Table 3-1 times 512/63 MHz; 252e-6 * 512e6/63 = 2048
// exactly). No "FFT size" is printed in Table 3-1; the Appendix (Operation
// Guidelines) Fig. 3-7 example shows "1symbol = 2048 clock periods" at Fs.
constexpr int fft_size(int mode) { return 2048 << (mode - 1); }

// Guard ratios 1/4, 1/8, 1/16, 1/32 (Table 3-1). Guard length in IFFT
// samples = N / den (DERIVED: Tg = ratio * Tu, e.g. mode 1 1/4 = 63 us).
inline constexpr std::array<int, 4> kGuardRatioDenominators = {4, 8, 16, 32};
constexpr int guard_samples(int mode, int guard_den) {
    return fft_size(mode) / guard_den;
}
// Symbol length in IFFT samples (DERIVED).
constexpr int symbol_samples(int mode, int guard_den) {
    return fft_size(mode) + guard_samples(mode, guard_den);
}

// Frequency offset of whole-band carrier k from the RF centre frequency
// fc (Sec. 3.14.1: Psi(n,k,t) = exp(j 2 pi (k-Kc)/Tu (t - Tg - nTs))).
constexpr double carrier_offset_hz(int mode, int k) {
    return static_cast<double>(k - center_carrier(mode)) * carrier_spacing_hz(mode);
}

// ===========================================================================
// 2. Segment arrangement (Sec. 3.14, Fig. 3-32, column order of
//    Tables 3-14 / 3-15, row order of Table 3-16)
// ===========================================================================

// Segment numbers in ascending order of frequency. Segment 0 is at the
// centre "with successively numbered segments placed alternately above and
// below that segment" (Sec. 3.14).
inline constexpr std::array<int, kNumSegments> kSegmentsInFrequencyOrder = {
    11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12};

inline std::array<int, kNumSegments> segments_in_frequency_order() {
    return kSegmentsInFrequencyOrder;
}

// Frequency position (0 = lowest) of segment number seg.
constexpr int segment_position(int seg) {
    for (int p = 0; p < kNumSegments; ++p)
        if (kSegmentsInFrequencyOrder[p] == seg) return p;
    return -1;
}

// Segment number at frequency position pos.
constexpr int segment_at_position(int pos) { return kSegmentsInFrequencyOrder[pos]; }

// Whole-band carrier number k of carrier 0 of segment seg (DERIVED from the
// definition of k in Sec. 3.14.1 + Fig. 3-32).
constexpr int segment_first_carrier(int mode, int seg) {
    return segment_position(seg) * carriers_per_segment(mode);
}

// ===========================================================================
// 3. PRBS for the pilots (Sec. 3.13.1, Fig. 3-31, Tables 3-16 / 3-17)
// ===========================================================================
//
// g(x) = X^11 + X^9 + 1. Fig. 3-31: eleven delay cells D1..D11 in a chain
// D1 -> D2 -> ... -> D11; the XOR of the outputs of D9 and D11 is fed back
// into D1; output Wi = output of D11.
//
// State encoding here: bit (i-1) of PrbsState = content of Di.
// Wi for carrier i of a segment = D11 BEFORE the i-th clock, starting from
// the segment's Table 3-16 initial value (W0 = D11 of the initial value).
// This output timing is not written out in words; it is the one that
// reproduces BOTH Table 3-16 (via its note) and Table 3-19 - see
// isdbt_selftest.cpp.
using PrbsState = std::uint16_t;

constexpr int prbs_output(PrbsState s) { return (s >> 10) & 1; }  // D11

constexpr PrbsState prbs_clock(PrbsState s) {
    const unsigned fb = ((s >> 8) ^ (s >> 10)) & 1u;               // D9 xor D11
    return static_cast<PrbsState>(((static_cast<unsigned>(s) << 1) | fb) & 0x7FFu);
}

inline constexpr PrbsState kPrbsAllOnes = 0x7FF;

// Table 3-16: Initial value of the PRBS-generating circuit, "Arranged in
// Ascending Order of Bits from Left to Right", i.e. leftmost character = D1,
// rightmost = D11. Rows exactly as printed (frequency order 11, 9, ..., 12).
// (Ordinance)
struct PrbsInitRow {
    int segment;
    const char* init[3];  // mode 1, mode 2, mode 3
};
inline constexpr PrbsInitRow kPrbsInitTable[kNumSegments] = {
    {11, {"11111111111", "11111111111", "11111111111"}},
    { 9, {"11011001111", "01101011110", "11011100101"}},
    { 7, {"01101011110", "11011100101", "10010100000"}},
    { 5, {"01000101110", "11001000010", "01110001001"}},
    { 3, {"11011100101", "10010100000", "00100011001"}},
    { 1, {"00101111010", "00001011000", "11100110110"}},
    { 0, {"11001000010", "01110001001", "00100001011"}},
    { 2, {"00010000100", "00000100100", "11100111101"}},
    { 4, {"10010100000", "00100011001", "01101010011"}},
    { 6, {"11110110000", "01100111001", "10111010010"}},
    { 8, {"00001011000", "11100110110", "01100010010"}},
    {10, {"10100100111", "00101010001", "11110100101"}},
    {12, {"01110001001", "00100001011", "00010011100"}},
};

// "D1..D11" string -> state.
constexpr PrbsState prbs_state_from_string(const char* bits) {
    PrbsState s = 0;
    for (int i = 0; i < 11; ++i)
        if (bits[i] == '1') s = static_cast<PrbsState>(s | (1u << i));
    return s;
}

// Initial PRBS state for (mode, segment number) from Table 3-16.
inline PrbsState prbs_init(int mode, int seg) {
    if (!valid_mode(mode)) throw std::invalid_argument("isdbt_std: bad mode");
    for (const auto& r : kPrbsInitTable)
        if (r.segment == seg) return prbs_state_from_string(r.init[mode - 1]);
    throw std::invalid_argument("isdbt_std: bad segment number");
}

// Wi for every carrier i = 0..carriers_per_segment-1 of segment seg.
inline std::vector<std::uint8_t> prbs_w_segment(int mode, int seg) {
    PrbsState s = prbs_init(mode, seg);
    std::vector<std::uint8_t> w(static_cast<size_t>(carriers_per_segment(mode)));
    for (auto& b : w) {
        b = static_cast<std::uint8_t>(prbs_output(s));
        s = prbs_clock(s);
    }
    return w;
}

// Wi for carrier c of segment seg (Sec. 3.13.1: "the i of Wi corresponds to
// the carrier number i of OFDM-segment").
inline int prbs_w(int mode, int seg, int c) {
    if (c < 0 || c >= carriers_per_segment(mode))
        throw std::out_of_range("isdbt_std: carrier out of segment");
    PrbsState s = prbs_init(mode, seg);
    for (int i = 0; i < c; ++i) s = prbs_clock(s);
    return prbs_output(s);
}

// Whole-band form (Table 3-16 note): all ones at carrier 0 of segment 11,
// generated continuously over all carriers up to the rightmost one.
// Returns W for whole-band carriers k = 0..K-1.
inline std::vector<std::uint8_t> prbs_w_band(int mode) {
    PrbsState s = kPrbsAllOnes;
    std::vector<std::uint8_t> w(static_cast<size_t>(total_carriers(mode)));
    for (auto& b : w) {
        b = static_cast<std::uint8_t>(prbs_output(s));
        s = prbs_clock(s);
    }
    return w;
}

// Table 3-17 (Ordinance): Wi = 1 -> (-4/3, 0); Wi = 0 -> (+4/3, 0).
// Data carriers are normalised to average power 1 (Sec. 3.9.4,
// Table 3-11: DQPSK, QPSK Z/sqrt2; 16QAM Z/sqrt10; 64QAM Z/sqrt42), so the
// pilots are 4/3 in amplitude relative to that.
inline constexpr double kPilotAmplitude = 4.0 / 3.0;
inline std::complex<double> bpsk_pilot(int w) {
    return {w ? -kPilotAmplitude : kPilotAmplitude, 0.0};
}

// Scattered-pilot value at carrier c of segment seg (Sec. 3.13.1).
// Meaningful only for synchronous-modulation segments at SP positions
// (see is_sp_carrier), but the value depends on c alone.
inline std::complex<double> sp_value(int mode, int seg, int c) {
    return bpsk_pilot(prbs_w(mode, seg, c));
}

// SP position rule (Sec. 3.12.2 + Fig. 3-30): "once every 12 carriers in
// the carrier direction, and once every 4 symbols in the symbol direction";
// Fig. 3-30: symbol 0 -> carriers 0, 12, ...; 1 -> 3; 2 -> 6; 3 -> 9;
// 4 -> 0, 12; ... 200 -> 0; 201 -> 3; 202 -> 6; 203 -> 9.
// => c mod 12 == 3*(n mod 4). (Formula written here; the standard gives it
// by figure.) Only in synchronous segments (Table 3-1: SP = 0 in
// differential segments). 204 = 51*4, so the pattern runs on unbroken
// across frames (DERIVED).
constexpr bool is_sp_carrier(int c, int n) { return (c % 12) == 3 * (n % 4); }

inline std::vector<int> sp_carriers(int mode, int n) {
    std::vector<int> v;
    for (int c = 3 * (n % 4); c < carriers_per_segment(mode); c += 12) v.push_back(c);
    return v;
}

// Number of SPs per synchronous segment: 9 / 18 / 36 (Table 3-1).
constexpr int sp_per_segment(int mode) { return 9 << (mode - 1); }

// CP (continual pilot) of a DIFFERENTIAL segment: carrier 0 (Table 3-14,
// row "CP"), BPSK by Wi as Table 3-17, same value in every symbol
// (Sec. 3.13.2). Synchronous segments have no CP (Table 3-1).
inline constexpr int kCpCarrier = 0;
inline std::complex<double> cp_value(int mode, int seg) {
    return bpsk_pilot(prbs_w(mode, seg, kCpCarrier));
}

// Table 3-19 (Ordinance): modulating signal of the rightmost continuous
// carrier (whole-band k = K-1): mode 1 (-4/3, 0), mode 2 (+4/3, 0),
// mode 3 (+4/3, 0).
inline constexpr std::array<double, 3> kRightmostCarrierI = {-4.0 / 3.0, 4.0 / 3.0, 4.0 / 3.0};
inline std::complex<double> rightmost_carrier_value(int mode) {
    return {kRightmostCarrierI[static_cast<size_t>(mode - 1)], 0.0};
}

// ===========================================================================
// 4. TMCC / AC / CP carrier positions
// ===========================================================================
// All tables below are transcribed EXACTLY as printed: one row per table
// row, 13 columns in the printed column order, which is FREQUENCY ORDER
// (segment numbers 11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12 - "Segment
// numbers are arranged in ascending order of frequency along the frequency
// axis", note under Table 3-14 (a)). Column index = segment_position(seg).
// Values are carrier numbers c within the segment. (Notification)
// Use the accessor functions further below to index by segment number.

// ---- Table 3-15: synchronous modulation (QPSK / 16QAM / 64QAM) ----------
// (a) Mode 1
inline constexpr int kSyncAC1_M1[2][13] = {
    { 10,  53,  61,  11,  20,  74,  35,  76,   4,  40,   8,   7,  98},  // AC1_1
    { 28,  83, 100, 101,  40, 100,  79,  97,  89,  89,  64,  89, 101},  // AC1_2
};
inline constexpr int kSyncTMCC_M1[1][13] = {
    { 70,  25,  17,  86,  44,  47,  49,  31,  83,  61,  85, 101,  23},  // TMCC1
};
// (b) Mode 2
inline constexpr int kSyncAC1_M2[4][13] = {
    { 10,  61,  20,  35,   4,   8,  98,  53,  11,  74,  76,  40,   7},  // AC1_1
    { 28, 100,  40,  79,  89,  64, 101,  83, 101, 100,  97,  89,  89},  // AC1_2
    {161, 119, 182, 184, 148, 115, 118, 169, 128, 143, 112, 116, 206},  // AC1_3
    {191, 209, 208, 205, 197, 197, 136, 208, 148, 187, 197, 172, 209},  // AC1_4
};
inline constexpr int kSyncTMCC_M2[2][13] = {
    { 70,  17,  44,  49,  83,  85,  23,  25,  86,  47,  31,  61, 101},  // TMCC1
    {133, 194, 155, 139, 169, 209, 178, 125, 152, 157, 191, 193, 131},  // TMCC2
};
// (c) Mode 3
inline constexpr int kSyncAC1_M3[8][13] = {
    { 10,  20,   4,  98,  11,  76,   7,  61,  35,   8,  53,  74,  40},  // AC1_1
    { 28,  40,  89, 101, 101,  97,  89, 100,  79,  64,  83, 100,  89},  // AC1_2
    {161, 182, 148, 118, 128, 112, 206, 119, 184, 115, 169, 143, 116},  // AC1_3
    {191, 208, 197, 136, 148, 197, 209, 209, 205, 197, 208, 187, 172},  // AC1_4
    {277, 251, 224, 269, 290, 256, 226, 236, 220, 314, 227, 292, 223},  // AC1_5
    {316, 295, 280, 299, 316, 305, 244, 256, 305, 317, 317, 313, 305},  // AC1_6
    {335, 400, 331, 385, 359, 332, 377, 398, 364, 334, 344, 328, 422},  // AC1_7
    {425, 421, 413, 424, 403, 388, 407, 424, 413, 352, 364, 413, 425},  // AC1_8
};
inline constexpr int kSyncTMCC_M3[4][13] = {
    { 70,  44,  83,  23,  86,  31, 101,  17,  49,  85,  25,  47,  61},  // TMCC1
    {133, 155, 169, 178, 152, 191, 131, 194, 139, 209, 125, 157, 193},  // TMCC2
    {233, 265, 301, 241, 263, 277, 286, 260, 299, 239, 302, 247, 317},  // TMCC3
    {410, 355, 425, 341, 373, 409, 349, 371, 385, 394, 368, 407, 347},  // TMCC4
};

// ---- Table 3-14: differential modulation (DQPSK) -------------------------
// (a) Mode 1
inline constexpr int kDiffCP_M1[1][13] = {
    {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},  // CP
};
inline constexpr int kDiffAC1_M1[2][13] = {
    { 10,  53,  61,  11,  20,  74,  35,  76,   4,  40,   8,   7,  98},  // AC1_1
    { 28,  83, 100, 101,  40, 100,  79,  97,  89,  89,  64,  89, 101},  // AC1_2
};
inline constexpr int kDiffAC2_M1[4][13] = {
    {  3,   3,  29,  28,  23,  30,   3,   5,  13,  72,  36,  25,  10},  // AC2_1
    { 45,  15,  41,  45,  63,  81,  72,  18,  93,  95,  48,  30,  30},  // AC2_2
    { 59,  40,  84,  81,  85,  92,  85,  57,  98, 100,  52,  42,  55},  // AC2_3
    { 77,  58,  93,  91, 105, 103,  89,  92, 102, 105,  74, 104,  81},  // AC2_4
};
inline constexpr int kDiffTMCC_M1[5][13] = {
    { 13,  25,   4,  36,  10,   7,  49,  31,  16,   5,  78,  34,  23},  // TMCC1
    { 50,  63,   7,  48,  28,  25,  61,  39,  30,  10,  82,  48,  37},  // TMCC2
    { 70,  73,  17,  55,  44,  47,  96,  47,  37,  21,  85,  54,  51},  // TMCC3
    { 83,  80,  51,  59,  47,  60,  99,  65,  74,  44,  98,  70,  68},  // TMCC4
    { 87,  93,  71,  86,  54,  87, 104,  72,  83,  61, 102, 101, 105},  // TMCC5
};
// (b) Mode 2
inline constexpr int kDiffCP_M2[1][13] = {
    {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},  // CP
};
inline constexpr int kDiffAC1_M2[4][13] = {
    { 10,  61,  20,  35,   4,   8,  98,  53,  11,  74,  76,  40,   7},  // AC1_1
    { 28, 100,  40,  79,  89,  64, 101,  83, 101, 100,  97,  89,  89},  // AC1_2
    {161, 119, 182, 184, 148, 115, 118, 169, 128, 143, 112, 116, 206},  // AC1_3
    {191, 209, 208, 205, 197, 197, 136, 208, 148, 187, 197, 172, 209},  // AC1_4
};
inline constexpr int kDiffAC2_M2[9][13] = {
    {  3,  29,  23,   3,  13,  36,  10,   3,  28,  30,   5,  72,  25},  // AC2_1
    { 45,  41,  63,  72,  93,  48,  30,  15,  45,  81,  18,  95,  30},  // AC2_2
    { 59,  84,  85,  85,  98,  52,  55,  40,  81,  92,  57, 100,  42},  // AC2_3
    { 77,  93, 105,  89, 102,  74,  81,  58,  91, 103,  92, 105, 104},  // AC2_4
    {108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108},  // AC2_5
    {111, 136, 138, 113, 180, 133, 111, 137, 131, 111, 121, 144, 118},  // AC2_6
    {123, 153, 189, 126, 203, 138, 153, 149, 171, 180, 201, 156, 138},  // AC2_7
    {148, 189, 200, 165, 208, 150, 167, 192, 193, 193, 206, 160, 163},  // AC2_8
    {166, 199, 211, 200, 213, 212, 185, 201, 213, 197, 210, 182, 189},  // AC2_9
};
inline constexpr int kDiffTMCC_M2[10][13] = {
    { 13,   4,  10,  49,  16,  78,  23,  25,  36,   7,  31,   5,  34},  // TMCC1
    { 50,   7,  28,  61,  30,  82,  37,  63,  48,  25,  39,  10,  48},  // TMCC2
    { 70,  17,  44,  96,  37,  85,  51,  73,  55,  47,  47,  21,  54},  // TMCC3
    { 83,  51,  47,  99,  74,  98,  68,  80,  59,  60,  65,  44,  70},  // TMCC4
    { 87,  71,  54, 104,  83, 102, 105,  93,  86,  87,  72,  61, 101},  // TMCC5
    {133, 144, 115, 139, 113, 142, 121, 112, 118, 157, 124, 186, 131},  // TMCC6
    {171, 156, 133, 147, 118, 156, 158, 115, 136, 169, 138, 190, 145},  // TMCC7
    {181, 163, 155, 155, 129, 162, 178, 125, 152, 204, 145, 193, 159},  // TMCC8
    {188, 167, 168, 173, 152, 178, 191, 159, 155, 207, 182, 206, 176},  // TMCC9
    {201, 194, 195, 180, 169, 209, 195, 179, 162, 212, 191, 210, 213},  // TMCC10
};
// (c) Mode 3
inline constexpr int kDiffCP_M3[1][13] = {
    {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0},  // CP
};
inline constexpr int kDiffAC1_M3[8][13] = {
    { 10,  20,   4,  98,  11,  76,   7,  61,  35,   8,  53,  74,  40},  // AC1_1
    { 28,  40,  89, 101, 101,  97,  89, 100,  79,  64,  83, 100,  89},  // AC1_2
    {161, 182, 148, 118, 128, 112, 206, 119, 184, 115, 169, 143, 116},  // AC1_3
    {191, 208, 197, 136, 148, 197, 209, 209, 205, 197, 208, 187, 172},  // AC1_4
    {277, 251, 224, 269, 290, 256, 226, 236, 220, 314, 227, 292, 223},  // AC1_5
    {316, 295, 280, 299, 316, 305, 244, 256, 305, 317, 317, 313, 305},  // AC1_6
    {335, 400, 331, 385, 359, 332, 377, 398, 364, 334, 344, 328, 422},  // AC1_7
    {425, 421, 413, 424, 403, 388, 407, 424, 413, 352, 364, 413, 425},  // AC1_8
};
inline constexpr int kDiffAC2_M3[19][13] = {
    {  3,  23,  13,  10,  28,   5,  25,  29,   3,  36,   3,  30,  72},  // AC2_1
    { 45,  63,  93,  30,  45,  18,  30,  41,  72,  48,  15,  81,  95},  // AC2_2
    { 59,  85,  98,  55,  81,  57,  42,  84,  85,  52,  40,  92, 100},  // AC2_3
    { 77, 105, 102,  81,  91,  92, 104,  93,  89,  74,  58, 103, 105},  // AC2_4
    {108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108, 108},  // AC2_5
    {111, 138, 180, 111, 131, 121, 118, 136, 113, 133, 137, 111, 144},  // AC2_6
    {123, 189, 203, 153, 171, 201, 138, 153, 126, 138, 149, 180, 156},  // AC2_7
    {148, 200, 208, 167, 193, 206, 163, 189, 165, 150, 192, 193, 160},  // AC2_8
    {166, 211, 213, 185, 213, 210, 189, 199, 200, 212, 201, 197, 182},  // AC2_9
    {216, 216, 216, 216, 216, 216, 216, 216, 216, 216, 216, 216, 216},  // AC2_10
    {245, 219, 252, 219, 246, 288, 219, 239, 229, 226, 244, 221, 241},  // AC2_11
    {257, 288, 264, 231, 297, 311, 261, 279, 309, 246, 261, 234, 246},  // AC2_12
    {300, 301, 268, 256, 308, 316, 275, 301, 314, 271, 297, 273, 258},  // AC2_13
    {309, 305, 290, 274, 319, 321, 293, 321, 318, 297, 307, 308, 320},  // AC2_14
    {324, 324, 324, 324, 324, 324, 324, 324, 324, 324, 324, 324, 324},  // AC2_15
    {352, 329, 349, 353, 327, 360, 327, 354, 396, 327, 347, 337, 334},  // AC2_16
    {369, 342, 354, 365, 396, 372, 339, 405, 419, 369, 387, 417, 354},  // AC2_17
    {405, 381, 366, 408, 409, 376, 364, 416, 424, 383, 409, 422, 379},  // AC2_18
    {415, 416, 428, 417, 413, 398, 382, 427, 429, 401, 429, 426, 405},  // AC2_19
};
inline constexpr int kDiffTMCC_M3[20][13] = {
    { 13,  10,  16,  23,  36,  31,  34,   4,  49,  78,  25,   7,   5},  // TMCC1
    { 50,  28,  30,  37,  48,  39,  48,   7,  61,  82,  63,  25,  10},  // TMCC2
    { 70,  44,  37,  51,  55,  47,  54,  17,  96,  85,  73,  47,  21},  // TMCC3
    { 83,  47,  74,  68,  59,  65,  70,  51,  99,  98,  80,  60,  44},  // TMCC4
    { 87,  54,  83, 105,  86,  72, 101,  71, 104, 102,  93,  87,  61},  // TMCC5
    {133, 115, 113, 121, 118, 124, 131, 144, 139, 142, 112, 157, 186},  // TMCC6
    {171, 133, 118, 158, 136, 138, 145, 156, 147, 156, 115, 169, 190},  // TMCC7
    {181, 155, 129, 178, 152, 145, 159, 163, 155, 162, 125, 204, 193},  // TMCC8
    {188, 168, 152, 191, 155, 182, 176, 167, 173, 178, 159, 207, 206},  // TMCC9
    {201, 195, 169, 195, 162, 191, 213, 194, 180, 209, 179, 212, 210},  // TMCC10
    {220, 265, 294, 241, 223, 221, 229, 226, 232, 239, 252, 247, 250},  // TMCC11
    {223, 277, 298, 279, 241, 226, 266, 244, 246, 253, 264, 255, 264},  // TMCC12
    {233, 312, 301, 289, 263, 237, 286, 260, 253, 267, 271, 263, 270},  // TMCC13
    {267, 315, 314, 296, 276, 260, 299, 263, 290, 284, 275, 281, 286},  // TMCC14
    {287, 320, 318, 309, 303, 277, 303, 270, 299, 321, 302, 288, 317},  // TMCC15
    {360, 355, 358, 328, 373, 402, 349, 331, 329, 337, 334, 340, 347},  // TMCC16
    {372, 363, 372, 331, 385, 406, 387, 349, 334, 374, 352, 354, 361},  // TMCC17
    {379, 371, 378, 341, 420, 409, 397, 371, 345, 394, 368, 361, 375},  // TMCC18
    {383, 389, 394, 375, 423, 422, 404, 384, 368, 407, 371, 398, 392},  // TMCC19
    {410, 396, 425, 395, 428, 426, 417, 411, 385, 411, 378, 407, 429},  // TMCC20
};

// Counts per segment (Table 3-1): synchronous AC1 2/4/8, TMCC 1/2/4;
// differential CP 1, AC1 2/4/8, AC2 4/9/19, TMCC 5/10/20.
constexpr int ac1_per_segment(int mode) { return 2 << (mode - 1); }
constexpr int sync_tmcc_per_segment(int mode) { return 1 << (mode - 1); }
constexpr int diff_tmcc_per_segment(int mode) { return 5 << (mode - 1); }
constexpr int diff_ac2_per_segment(int mode) { return mode == 1 ? 4 : mode == 2 ? 9 : 19; }

namespace detail {
template <size_t R>
inline std::vector<int> column(const int (&tab)[R][13], int seg) {
    const int pos = segment_position(seg);
    if (pos < 0) throw std::invalid_argument("isdbt_std: bad segment number");
    std::vector<int> v;
    v.reserve(R);
    for (size_t r = 0; r < R; ++r) v.push_back(tab[r][pos]);
    return v;
}
inline void check_mode(int mode) {
    if (!valid_mode(mode)) throw std::invalid_argument("isdbt_std: bad mode");
}
}  // namespace detail

// AC1 carrier numbers of segment seg (Table 3-15 for synchronous,
// Table 3-14 for differential; identical by Sec. 3.12.1 / 3.12.2).
inline std::vector<int> ac1_carriers(int mode, int seg, bool differential) {
    detail::check_mode(mode);
    if (differential) {
        if (mode == 1) return detail::column(kDiffAC1_M1, seg);
        if (mode == 2) return detail::column(kDiffAC1_M2, seg);
        return detail::column(kDiffAC1_M3, seg);
    }
    if (mode == 1) return detail::column(kSyncAC1_M1, seg);
    if (mode == 2) return detail::column(kSyncAC1_M2, seg);
    return detail::column(kSyncAC1_M3, seg);
}

// AC2 carrier numbers (differential segments only, Table 3-14; a
// synchronous segment has none - Sec. 3.12.2).
inline std::vector<int> ac2_carriers(int mode, int seg, bool differential) {
    detail::check_mode(mode);
    if (!differential) return {};
    if (mode == 1) return detail::column(kDiffAC2_M1, seg);
    if (mode == 2) return detail::column(kDiffAC2_M2, seg);
    return detail::column(kDiffAC2_M3, seg);
}

// TMCC carrier numbers (Table 3-15 synchronous, Table 3-14 differential).
inline std::vector<int> tmcc_carriers(int mode, int seg, bool differential) {
    detail::check_mode(mode);
    if (differential) {
        if (mode == 1) return detail::column(kDiffTMCC_M1, seg);
        if (mode == 2) return detail::column(kDiffTMCC_M2, seg);
        return detail::column(kDiffTMCC_M3, seg);
    }
    if (mode == 1) return detail::column(kSyncTMCC_M1, seg);
    if (mode == 2) return detail::column(kSyncTMCC_M2, seg);
    return detail::column(kSyncTMCC_M3, seg);
}

// CP carrier numbers (differential: {0}, Table 3-14; synchronous: none).
inline std::vector<int> cp_carriers(int mode, int seg, bool differential) {
    detail::check_mode(mode);
    if (segment_position(seg) < 0) throw std::invalid_argument("isdbt_std: bad segment number");
    if (!differential) return {};
    return {kCpCarrier};
}

// ===========================================================================
// 5. TMCC / AC known content (Sec. 3.13.3, 3.13.4, 3.15.2 - 3.15.5)
// ===========================================================================
// TMCC: DBPSK. B'0 = Wi (Wi of that TMCC carrier's own carrier number),
// B'k = B'(k-1) xor Bk, k = 1..203; B' = 0 -> (+4/3, 0), 1 -> (-4/3, 0)
// (Sec. 3.13.3). Table 3-20: B0 reference, B1-B16 synchronizing word,
// B17-B19 segment type (differential 111, synchronous 000), B20-B121 TMCC
// information, B122-B203 parity. w0 and w1 alternate frame by frame
// (Sec. 3.15.4).
// ASSUMPTIONS (implied, not stated in words): bit Bk is carried in OFDM
// symbol k of the frame (204 bits, 204 symbols); the leftmost character of
// w0 / w1 is B1.
inline constexpr const char* kTmccSyncW0 = "0011010111101110";  // Table 3-20, Sec. 3.15.4
inline constexpr const char* kTmccSyncW1 = "1100101000010001";  // = ~w0
inline constexpr const char* kSegTypeDifferential = "111";      // Table 3-20, Sec. 3.15.5
inline constexpr const char* kSegTypeSynchronous = "000";

// B'0..B'19 (after differential coding) of a TMCC carrier at carrier c of
// segment seg, i.e. the known part of a frame; use_w1 selects the frame's
// sync word. Modulating value = bpsk_pilot(B'k).
inline std::array<std::uint8_t, 20> tmcc_known_coded_bits(int mode, int seg, int c,
                                                          bool differential, bool use_w1) {
    std::array<std::uint8_t, 20> b{};
    b[0] = static_cast<std::uint8_t>(prbs_w(mode, seg, c));
    const char* w = use_w1 ? kTmccSyncW1 : kTmccSyncW0;
    const char* t = differential ? kSegTypeDifferential : kSegTypeSynchronous;
    for (int k = 1; k <= 19; ++k) {
        const int bk = (k <= 16) ? (w[k - 1] - '0') : (t[k - 17] - '0');
        b[static_cast<size_t>(k)] = static_cast<std::uint8_t>(b[static_cast<size_t>(k - 1)] ^ bk);
    }
    return b;
}

// AC: DBPSK; reference at the first frame symbol (symbol 0) = signal point
// of Wi (Sec. 3.13.4; Appendix (Operation Guidelines) Ch. 6: "the first OFDM frame symbol (symbol 0)
// is the reference for differential modulation for AC"). Later symbols carry
// data (stuffing bit 1 when there is no additional information).
inline std::complex<double> ac_reference_value(int mode, int seg, int c) {
    return bpsk_pilot(prbs_w(mode, seg, c));
}

// ===========================================================================
// 6. Convenience: all fixed-value pilots of OFDM symbol n across the band
// ===========================================================================
// For each whole-band carrier k carrying a value known regardless of the
// payload: SPs of synchronous segments, CPs of differential segments, the
// rightmost continuous carrier (k = K-1), and - in symbol n % 204 == 0 only,
// if include_symbol0_refs - the TMCC / AC differential references.
// differential_by_seg[seg] = true if segment NUMBER seg is DQPSK.
struct KnownCarrier {
    int k;
    std::complex<double> value;
};
inline std::vector<KnownCarrier> known_pilots(int mode, int n,
                                              const std::array<bool, kNumSegments>& differential_by_seg,
                                              bool include_symbol0_refs = false) {
    detail::check_mode(mode);
    const std::vector<std::uint8_t> wb = prbs_w_band(mode);  // == Table 3-16 per segment
    const int nseg = carriers_per_segment(mode);
    std::vector<KnownCarrier> out;
    for (int pos = 0; pos < kNumSegments; ++pos) {
        const int seg = segment_at_position(pos);
        const int k0 = pos * nseg;
        const bool diff = differential_by_seg[static_cast<size_t>(seg)];
        if (!diff) {
            for (int c : sp_carriers(mode, n)) out.push_back({k0 + c, bpsk_pilot(wb[static_cast<size_t>(k0 + c)])});
        } else {
            out.push_back({k0 + kCpCarrier, bpsk_pilot(wb[static_cast<size_t>(k0 + kCpCarrier)])});
        }
        if (include_symbol0_refs && (n % kSymbolsPerFrame) == 0) {
            std::vector<int> refs = tmcc_carriers(mode, seg, diff);
            for (int c : ac1_carriers(mode, seg, diff)) refs.push_back(c);
            for (int c : ac2_carriers(mode, seg, diff)) refs.push_back(c);
            for (int c : refs) out.push_back({k0 + c, bpsk_pilot(wb[static_cast<size_t>(k0 + c)])});
        }
    }
    out.push_back({total_carriers(mode) - 1, rightmost_carrier_value(mode)});
    return out;
}

}  // namespace isdbt_std
