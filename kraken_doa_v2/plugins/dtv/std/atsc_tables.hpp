// atsc_tables.hpp - ATSC 1.0 8-VSB training-signal constants.
//
// Every value is taken from ATSC A/53 Part 2:2011 "RF/Transmission System
// Characteristics" (15 December 2011); the section / equation / table /
// figure is given next to each one. Values marked DERIVED are computed from
// the document's own numbers (the derivation is in atsc_spec.md), not printed
// in it. Header-only, C++17, standard library only.
//
// Level convention: symbol levels BEFORE pilot addition (sync = +/-5, data =
// +/-1, +/-3, +/-5, +/-7). The pilot is a DC of +1.25 added to EVERY symbol
// (data and sync), A/53-2 Sec. 5.4.2 - see with_pilot().
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace atsc_std {

// ---------------------------------------------------------------- timing ---
// Eq. (1), Sec. 5.1: Sr (MHz) = 4.5/286 x 684 = 10.76... MHz
constexpr double kSymbolRateHz = 4.5e6 / 286.0 * 684.0;   // 10 762 237.762... Hz
constexpr int kSegmentSymbols = 832;     // Sec. 5.1, 5.3.1, Fig. 5.8
constexpr int kSegmentSyncSymbols = 4;   // Sec. 3.4, 5.3.1, Fig. 5.8
constexpr int kSegmentDataSymbols = 828; // Sec. 3.4, 5.1, Fig. 5.8
constexpr int kFieldSegments = 313;      // Sec. 3.4, 5.1, 5.3.2 (1 field sync + 312 data)
constexpr int kFieldsPerFrame = 2;       // Sec. 5.1
constexpr int kFrameSegments = 626;      // eq. (3), Sec. 5.1: frame = fseg/626
// Eq. (2): fseg = Sr/832 = 12.94... x 10^3 segments/s
constexpr double kSegmentRateHz = kSymbolRateHz / kSegmentSymbols;
// DERIVED: field rate = fseg/313 (~41.33 Hz); doc: field ~24.2 ms (Sec. 5.3.2, Fig. 5.2)
constexpr double kFieldRateHz = kSegmentRateHz / kFieldSegments;
// Eq. (3): frame = fseg/626 = 20.66... frames/s
constexpr double kFrameRateHz = kSegmentRateHz / kFrameSegments;
constexpr int kFieldSymbols = kSegmentSymbols * kFieldSegments;   // 260 416 (DERIVED)

// ---------------------------------------------------------------- levels ---
// Sec. 5.4.1 / Fig. 5.6: 8 data levels; Sec. 5.4.1, 5.3.1, Fig. 5.8: sync = -5 / +5
constexpr std::array<int, 8> kDataLevels8Vsb = {-7, -5, -3, -1, 1, 3, 5, 7};
constexpr double kSyncLevel = 5.0;
// Sec. 5.4.2: pilot = DC level 1.25 added to every symbol (data and sync)
constexpr double kPilotDc = 1.25;
// Sec. 5.4.2: pilot power "approximately 11.3 dB below the average data signal power"
constexpr double kPilotBelowDataDb = 11.3;
// Sec. 5.3.1: Data Segment Sync = (+5, -5, -5, +5)
constexpr std::array<double, 4> kSegmentSync = {+5.0, -5.0, -5.0, +5.0};

// 16-VSB high data rate mode (Sec. 6): same field sync structure (Sec. 6.2.5
// "See Section 5.3.2"), but nominal sync levels -9 / +9 and pilot DC 2.5
// (Sec. 6.3.1, 6.3.2, Fig. 6.1). Not used by terrestrial 8-VSB.
constexpr double kSyncLevel16Vsb = 9.0;
constexpr double kPilotDc16Vsb = 2.5;

// -------------------------------------------------------------- spectrum ---
constexpr double kChannelBandwidthHz = 6.0e6;  // Sec. 5, Fig. 5.11, Fig. 5.12
// Sec. 5.4.3 and Fig. 5.12: linear-phase root-raised-cosine, alpha = 0.1152
constexpr double kRollOff = 0.1152;
// Fig. 5.11 / 5.12: 5.38 MHz between the band-edge (0.7 Tx / 0.5 overall) points,
// transition half-width d = 0.31 MHz
constexpr double kNyquistBandFigureHz = 5.38e6;
constexpr double kTransitionDFigureHz = 0.31e6;
// Sec. 5.4.2: pilot "nominally 309 kHz from the lower band edge", at the
// suppressed-carrier frequency, in phase with the suppressed carrier
constexpr double kPilotFromLowerEdgeNominalHz = 309.0e3;
// DERIVED from footnote 4 + Sec. 5.1 + eq. (1): (6 MHz - Sr/2)/2 = 309.440559... kHz
constexpr double kPilotFromLowerEdgeHz = (kChannelBandwidthHz - kSymbolRateHz / 2.0) / 2.0;
// DERIVED: lower band edge = channel centre - 3 MHz  =>  pilot = centre - Sr/4
// = -2.690559... MHz. The data spectrum lies ABOVE the pilot (lower sideband
// removed, Sec. 5.4 / Fig. 5.11): carrier .. carrier + Sr/2, centred on the
// channel centre.
constexpr double kPilotFromChannelCentreHz = kPilotFromLowerEdgeHz - kChannelBandwidthHz / 2.0;

// ------------------------------------------------- field sync layout (0-based)
// Fig. 5.9 and Sec. 5.3.2.1 - 5.3.2.6: 4 + 511 + 63 + 63 + 63 + 24 + 92 + 12 = 832
constexpr int kFsSegSyncStart = 0;    // 4 symbols, Sec. 5.3.1 / 5.3.2
constexpr int kFsPn511Start = 4;      // 511 symbols, Sec. 5.3.2.1
constexpr int kFsPn511Len = 511;
constexpr int kFsPn63Len = 63;        // Sec. 5.3.2.2
constexpr std::array<int, 3> kFsPn63Start = {515, 578, 641};  // the 2nd (578) = "middle"
constexpr int kFsVsbModeStart = 704;  // 24 symbols, Sec. 5.3.2.3
constexpr int kFsVsbModeLen = 24;
constexpr int kFsReservedStart = 728; // 104 symbols incl. precode, Sec. 5.3.2.4
constexpr int kFsReservedLen = 104;   //   numbered 1..104 in transmit order (Sec. 5.3.2.4)
constexpr int kFsReservedOnlyLen = 92;          // "92 symbols of the last 104" (Sec. 5.3.2.4)
constexpr int kFsEnhSignalStart = 810;          // reserved symbols 83..92 (Sec. 5.3.2.6)
constexpr int kFsEnhSignalLen = 10;
constexpr int kFsPrecodeStart = 820;            // last 12 symbols (Sec. 5.3.2.5, Fig. 5.9)
constexpr int kFsPrecodeLen = 12;
static_assert(kFsPn63Start[0] == kFsPn511Start + kFsPn511Len, "layout");
static_assert(kFsVsbModeStart == kFsPn63Start[2] + kFsPn63Len, "layout");
static_assert(kFsReservedStart == kFsVsbModeStart + kFsVsbModeLen, "layout");
static_assert(kFsReservedStart + kFsReservedLen == kSegmentSymbols, "layout");
static_assert(kFsEnhSignalStart == kFsReservedStart + 82, "layout");
static_assert(kFsPrecodeStart == kFsReservedStart + kFsReservedOnlyLen, "layout");
static_assert(kFsPrecodeStart + kFsPrecodeLen == kSegmentSymbols, "layout");

// ------------------------------------------------------- PN generators -----
// Sec. 5.3.2.1 / Fig. 5.10: PN511 = X^9 + X^7 + X^6 + X^4 + X^3 + X + 1,
// pre-load '010000000'. Sec. 5.3.2.2 / Fig. 5.10: PN63 = X^6 + X + 1,
// pre-load '100111'. Fig. 5.10 register: cells drawn left->right holding the
// pre-load digits in that order, "Shift ->" to the right, output = rightmost
// cell, feedback (XOR of the tapped cell outputs) into the leftmost cell.
// PN511 taps (1-based cells, from the left): 2, 3, 5, 6, 8, 9. PN63 taps: 5, 6.
constexpr const char* kPn511Preload = "010000000";
constexpr const char* kPn63Preload = "100111";
constexpr std::array<int, 6> kPn511Taps = {2, 3, 5, 6, 8, 9};
constexpr std::array<int, 2> kPn63Taps = {5, 6};

// The sequences exactly as printed in Sec. 5.3.2.1 and Sec. 5.3.2.2 (first
// transmitted bit first). '1' -> level +5, '0' -> level -5.
constexpr const char* kPn511Printed =
    "0000 0001 0111 1111 1100 1010 1010 1110 0110 0110 1000 1000 1001 1110 0001 1101 "
    "0111 1101 0011 0101 0011 1011 0011 1010 0100 0101 1000 1111 0010 0001 0100 0111 "
    "1100 1111 0101 0001 0100 1100 0011 0001 0000 0100 0011 1111 0000 0101 0100 0000 "
    "1100 1111 1110 1110 1010 1001 0110 0110 0011 0111 0111 1011 0100 1010 0100 1110 "
    "0111 0001 0111 0100 0011 0100 1111 1011 0001 0101 1011 1100 1101 1010 1110 1101 "
    "1001 0110 1101 1100 1001 0010 1110 0011 1001 0111 1010 0011 0101 1000 0100 1101 "
    "1111 0001 0010 1011 1100 0110 0101 0000 1000 1100 0001 1110 1111 1101 0110 1010 "
    "1100 1001 1001 0001 1101 1100 0010 1101 0000 0110 1100 0000 1001 0000 0001 110";
constexpr const char* kPn63Printed =
    "1110 0100 1011 0111 0110 0110 1010 1111 1100 0001 0000 1100 0101 0011 1101 000";

// ------------------------------------------------------- VSB mode bits -----
// Table 5.3 (Sec. 5.3.2.3), first byte, second byte, third byte; '1' -> +5,
// '0' -> -5. Transmit order assumed = table reading order (first byte first,
// leftmost bit first) - the document does not state the bit order explicitly.
constexpr const char* kVsbMode8VsbTrellis = "0000 1010 0101 1111 0101 1010";
constexpr const char* kVsbMode16Vsb = "0000 1111 0000 1111 1100 0011";

// ------------------------------------------------------------ helpers ------
inline std::string strip_bits(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p)
        if (*p == '0' || *p == '1') out.push_back(*p);
    return out;
}

// Fibonacci LFSR exactly as drawn in Fig. 5.10; returns 0/1 output bits.
template <std::size_t NT>
inline std::vector<int> lfsr_bits(const char* preload, const std::array<int, NT>& taps,
                                  std::size_t n) {
    std::vector<int> reg;
    for (const char* p = preload; *p; ++p) reg.push_back(*p == '1' ? 1 : 0);
    std::vector<int> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(reg.back());                // output = rightmost cell
        int fb = 0;
        for (int t : taps) fb ^= reg[t - 1];      // XOR of tapped cells
        for (std::size_t c = reg.size() - 1; c > 0; --c) reg[c] = reg[c - 1];  // shift right
        reg[0] = fb;                              // feedback into leftmost cell
    }
    return out;
}

inline std::vector<int> pn511_bits() { return lfsr_bits(kPn511Preload, kPn511Taps, 511); }
inline std::vector<int> pn63_bits() { return lfsr_bits(kPn63Preload, kPn63Taps, 63); }

inline std::vector<int> bits_to_pm1(const std::vector<int>& b) {
    std::vector<int> v(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) v[i] = b[i] ? +1 : -1;
    return v;
}

// +/-1 sequences ('1' -> +1, '0' -> -1); multiply by kSyncLevel (5) for the
// transmitted level (Sec. 5.3.2.1 / 5.3.2.2).
inline std::vector<int> pn511() { return bits_to_pm1(pn511_bits()); }
inline std::vector<int> pn63() { return bits_to_pm1(pn63_bits()); }

// The 832 symbol levels (before pilot) of a Data Field Sync segment.
// odd_field: per Sec. 5.3.2.6 an "odd data field" is one whose field sync has
// the NON-inverted (middle) PN63; an even field has it inverted (Sec. 5.3.2.2).
// Mode = 8 VSB trellis coded (Table 5.3). Indices 728..831 (92 reserved +
// 12 precode = copies of the previous segment's last 12 data symbols) are NaN.
inline std::vector<double> field_sync_symbols_mode(bool odd_field, const char* mode_bits,
                                                   double level = kSyncLevel) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> s(kSegmentSymbols, nan);
    for (int i = 0; i < kSegmentSyncSymbols; ++i)
        s[kFsSegSyncStart + i] = kSegmentSync[i] / kSyncLevel * level;
    const std::vector<int> p511 = pn511();
    for (int i = 0; i < kFsPn511Len; ++i) s[kFsPn511Start + i] = level * p511[i];
    const std::vector<int> p63 = pn63();
    for (int k = 0; k < 3; ++k) {
        const double sign = (k == 1 && !odd_field) ? -1.0 : 1.0;  // middle PN63 inverted
        for (int i = 0; i < kFsPn63Len; ++i) s[kFsPn63Start[k] + i] = sign * level * p63[i];
    }
    const std::string mode = strip_bits(mode_bits);
    for (int i = 0; i < kFsVsbModeLen && i < static_cast<int>(mode.size()); ++i)
        s[kFsVsbModeStart + i] = (mode[i] == '1') ? level : -level;
    return s;
}

inline std::vector<double> field_sync_symbols(bool odd_field) {
    return field_sync_symbols_mode(odd_field, kVsbMode8VsbTrellis, kSyncLevel);
}

// As field_sync_symbols(), plus reserved symbols 83..92 (indices 810..819)
// set to "no enhancements" per Sec. 5.3.2.6 (Context Number 0, all
// Enhancement Bits 0 => every bit 0 => -5 in odd fields, +5 in even fields).
// CAUTION: Sec. 5.3.2.4 instead RECOMMENDS filling the 92 reserved symbols
// with a continuation of PN63 when only 8-VSB is present; which one a given
// transmitter sends is not determined by this document.
inline std::vector<double> field_sync_symbols_no_enh(bool odd_field) {
    std::vector<double> s = field_sync_symbols(odd_field);
    for (int i = 0; i < kFsEnhSignalLen; ++i)
        s[kFsEnhSignalStart + i] = odd_field ? -kSyncLevel : +kSyncLevel;
    return s;
}

// Adds the pilot DC (Sec. 5.4.2) to every known symbol; NaN stays NaN.
inline std::vector<double> with_pilot(std::vector<double> s, double dc = kPilotDc) {
    for (double& v : s)
        if (!std::isnan(v)) v += dc;
    return s;
}

}  // namespace atsc_std
