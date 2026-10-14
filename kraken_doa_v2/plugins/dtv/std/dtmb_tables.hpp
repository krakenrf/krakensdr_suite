// dtmb_tables.hpp - DTMB (GB 20600-2006, ITU-R BT.1306 System D) frame-header
// PN sequences and frame constants, reconstructed from PUBLIC sources (the
// standard text itself was not available). See dtmb_spec.md for every source
// URL and the confidence of each item. Header-only, C++17, standard library only.
//
// Confidence legend used below:
//   [HIGH]  several independent sources agree
//   [MED]   consistent sources, but they may share one interpretation step
//   [LOW]   one source only / indirect
//
// Conventions
//   - Bits are LFSR output bits b[n] in transmission order; the LFSR recurrence
//     for G(x) = 1 + sum x^k is  b[n] = XOR_k b[n-k]  (x^k = delay of k chips),
//     which is exactly the Fibonacci LFSR drawn in Keysight's ADS DTMB library
//     (taps D1,D5,D6,D8 / D3,D10 / D2,D7,D8,D9, output from the last cell).
//   - The initial-state string ("10110000" ...) is taken as the FIRST output
//     chips of the frame header in order (prefix chip 0 first). [MED] - this is
//     the reading used by every source that lists concrete sequences / shifts
//     (dtmb-sdr appendix vectors, Thomson and CN101309251A shift tables); the
//     mirrored reading (first state bit output last) would give the same
//     m-sequence family with a different start and the schedule running the
//     other way - see dtmb_spec.md "Remaining uncertainty".
//   - Chip mapping: bit 0 -> +1, bit 1 -> -1 (NRZ). [HIGH]
//   - Complex header symbol: the same chip on I and Q, i.e. chip*(1+j)/sqrt(2)
//     times sqrt(header/body power ratio) for a unit-power frame body. [HIGH]

#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dtmb_std {

// ---------------------------------------------------------------- system --
// 8 MHz channel (the common case). BT.1306 also lists 6/7 MHz variants that
// scale the symbol rate to 5.67 / 6.62 Msps with identical frame structure.
inline constexpr double kSymbolRateHz        = 7.56e6;   // [HIGH] header and body share it
inline constexpr double kRollOff             = 0.05;     // [MED-HIGH] SRRC
inline constexpr double kChannelBandwidthHz  = 8.0e6;    // [HIGH]
inline constexpr double kUsedBandwidthHz     = 7.56e6;   // [HIGH] (BT.1306 "used bandwidth")
inline constexpr int    kFrameBodySymbols    = 3780;     // [HIGH] 500 us
inline constexpr int    kSystemInfoSymbols   = 36;       // [HIGH]
inline constexpr int    kDataSymbols         = 3744;     // [HIGH]
inline constexpr double kSubcarrierSpacingHz = 2000.0;   // [HIGH] C=3780 mode only
inline constexpr int    kSuperframeSymbols   = 945000;   // [HIGH] 125 ms at 7.56 Msps
inline constexpr double kSuperframeSeconds   = 0.125;    // [HIGH]
inline constexpr int    kSuperframesPerMinute = 480;     // [HIGH]
inline constexpr int    kMinuteFramesPerDay  = 1440;     // [HIGH] day frame starts 00:00:00

// ----------------------------------------------------------- header modes --
struct HeaderMode {
    int mode_number;          // frame header mode 1 / 2 / 3                  [HIGH]
    int header_len;           // 420 / 595 / 945                             [HIGH]
    int prefix_len;           // pre-amble: 82 / 0 / 217 (= core tail)        [HIGH]
    int core_len;             // 255 / 595 / 511 (595 = truncated window)     [HIGH]
    int postfix_len;          // post-amble: 83 / 0 / 217 (= core head)       [HIGH]
    int lfsr_order;           // 8 / 10 / 9                                   [HIGH]
    int period;               // m-sequence period 255 / 1023 / 511           [HIGH]
    int taps[4];              // k of the x^k terms of G(x) (0 = unused)      [HIGH]
    const char* state0;       // frame-0 / constant-phase initial state       [HIGH value, MED bit order]
    int frames_per_superframe;// 225 / 216 / 200                              [HIGH]
    int schedule_turn;        // 112 / 0 / 100 (index of the reflection)       [HIGH]
    double header_to_body_power; // 2 (+3 dB) / 1 / 2                         [HIGH]
    bool cyclic;              // prefix/postfix are cyclic extensions          [HIGH]
};

// G255(x)  = 1 + x + x^5 + x^6 + x^8
// G1023(x) = 1 + x^3 + x^10       (PN595 = first 595 chips of this m-sequence)
// G511(x)  = 1 + x^2 + x^7 + x^8 + x^9
inline constexpr HeaderMode kPN420{1, 420,  82, 255,  83,  8,  255, {1, 5, 6, 8}, "10110000",   225, 112, 2.0, true};
inline constexpr HeaderMode kPN595{2, 595,   0, 595,   0, 10, 1023, {3, 10, 0, 0}, "0000000001", 216,   0, 1.0, false};
inline constexpr HeaderMode kPN945{3, 945, 217, 511, 217,  9,  511, {2, 7, 8, 9}, "111110111",  200, 100, 2.0, true};

// Accepts the frame-header mode number (1, 2, 3) or the header length (420, 595, 945).
inline const HeaderMode& header_mode(int mode) {
    switch (mode) {
    case 1: case 420: return kPN420;
    case 2: case 595: return kPN595;
    case 3: case 945: return kPN945;
    }
    throw std::invalid_argument("dtmb_std: mode must be 1/2/3 or 420/595/945");
}

inline constexpr int frame_len(const HeaderMode& m) { return m.header_len + kFrameBodySymbols; } // 4200/4375/4725

// Raw LFSR output bits (0/1) starting from an initial state given as a bit
// string, first output chip first. count may exceed the period (it repeats).
inline std::vector<int> lfsr_bits(const HeaderMode& m, const std::string& state, int count) {
    if (static_cast<int>(state.size()) != m.lfsr_order)
        throw std::invalid_argument("dtmb_std: state length != LFSR order");
    std::vector<int> b(static_cast<size_t>(count > m.lfsr_order ? count : m.lfsr_order));
    for (int i = 0; i < m.lfsr_order; ++i) {
        if (state[i] != '0' && state[i] != '1') throw std::invalid_argument("dtmb_std: state must be 0/1");
        b[i] = state[i] - '0';
    }
    for (size_t n = static_cast<size_t>(m.lfsr_order); n < b.size(); ++n) {
        int v = 0;
        for (int t : m.taps) if (t) v ^= b[n - static_cast<size_t>(t)];
        b[n] = v;
    }
    b.resize(static_cast<size_t>(count));
    return b;
}

// +/-1 chips (0 -> +1, 1 -> -1) of the phase-0 header, i.e. the header of
// superframe frame 0 and the header of EVERY frame in constant-phase mode.
inline std::vector<int> reference_header(const HeaderMode& m) {
    std::vector<int> c = lfsr_bits(m, m.state0, m.header_len);
    for (int& v : c) v = v ? -1 : +1;
    return c;
}

// Core (one period, 255 / 511 chips; for PN595 the whole 595 header) of the phase-0 header.
inline std::vector<int> reference_core(const HeaderMode& m) {
    std::vector<int> h = reference_header(m);
    return std::vector<int>(h.begin() + m.prefix_len, h.begin() + m.prefix_len + m.core_len);
}

// Cyclic ADVANCE a (0..core_len-1) of the header of superframe frame `phase`
// relative to the phase-0 header in variable-phase mode:
//     header_phase[n] == periodic_reference[n + a]   for every header index n.
// Schedule [HIGH: Thomson US8311163 / EP2195993, ASTRI US8693606, CN101309251A,
// dtmb-sdr all agree; direction MED - see the bit-order note above]:
//   l even, l <= T : delayed by l/2            (a = -l/2)
//   l odd,  l <  T : advanced by (l+1)/2       (a = +(l+1)/2)
//   l > T          : same as frame 2T - l      (T = 112 for PN420, 100 for PN945)
// so the shift walks 0, +1, -1, +2, -2, ... out to +-56 (+-50) at the middle of
// the superframe and back: only 113 (PN420) / 101 (PN945) distinct headers.
// PN595 always returns 0 (one fixed header).
inline int phase_advance(const HeaderMode& m, int phase) {
    if (phase < 0 || phase >= m.frames_per_superframe)
        throw std::invalid_argument("dtmb_std: phase index out of range for this mode");
    if (!m.cyclic) return 0;
    int l = phase;
    if (l > m.schedule_turn) l = 2 * m.schedule_turn - l;
    if (l == 0) return 0;
    if (l % 2 == 0) return (m.core_len - l / 2) % m.core_len;
    return (l + 1) / 2;
}

// Header chips for an arbitrary cyclic advance (cyclic modes only; for PN595
// only advance 0 exists). Useful for a blind search over all 255 / 511 phases.
inline std::vector<int> pn_header_advance(const HeaderMode& m, int advance) {
    if (!m.cyclic) {
        if (advance != 0) throw std::invalid_argument("dtmb_std: PN595 has no cyclic phases");
        return reference_header(m);
    }
    const std::vector<int> core = reference_core(m);
    const int N = m.core_len;
    int a = advance % N; if (a < 0) a += N;
    std::vector<int> h(static_cast<size_t>(m.header_len));
    // header index n corresponds to core index (n - prefix) mod N of the phase-0 header
    for (int n = 0; n < m.header_len; ++n) {
        int k = ((n - m.prefix_len + a) % N + N) % N;
        h[static_cast<size_t>(n)] = core[static_cast<size_t>(k)];
    }
    return h;
}

// THE requested API: +/-1 chips of the whole frame header (prefix + core +
// postfix) of PN phase index `phase` = frame number within the superframe
// (0..224 PN420, 0..215 PN595 - all identical, 0..199 PN945), numbered as in the
// standard's initial-phase table. phase 0 is also the constant-phase header.
// mode: 1/2/3 or 420/595/945.
inline std::vector<int> pn_header(int mode, int phase) {
    const HeaderMode& m = header_mode(mode);
    return pn_header_advance(m, phase_advance(m, phase));
}

// The LFSR initial state that produces pn_header(mode, phase), as the first
// lfsr_order chips (first chip = MSB). For PN420 frame 0/1/2 this gives
// 0xB0/0x61/0xD8 - the values dtmb-sdr quotes from the standard's Table 2.
inline unsigned initial_state(int mode, int phase) {
    const HeaderMode& m = header_mode(mode);
    std::vector<int> h = pn_header(mode, phase);
    unsigned v = 0;
    for (int i = 0; i < m.lfsr_order; ++i) v = (v << 1) | (h[static_cast<size_t>(i)] < 0 ? 1u : 0u);
    return v;
}

// Complex baseband header symbol for a unit-average-power frame body: the chip
// on I and Q, scaled so the header's average power is 2x (PN420/945) or 1x
// (PN595) the body's. [HIGH]
inline std::complex<double> header_symbol(const HeaderMode& m, int chip) {
    const double s = std::sqrt(m.header_to_body_power) / std::sqrt(2.0);
    return {chip * s, chip * s};
}

// Channel impulse response from the received CORE window of a cyclic header
// (samples header[prefix .. prefix+N-1] at 1 sample/chip, symbol-timed), using
// the exact m-sequence inverse: with R[d] = sum_m y[(m+d) mod N] c[m],
//     R[d] = (N+1) h[d] - sum(h)   =>   h[d] = (R[d] + sum_d R[d]) / (N+1).
// Exact (cyclic) for paths delayed 0..prefix_len chips (PN420: 82 = 10.8 us,
// PN945: 217 = 28.7 us); longer delays leak the previous frame body in. Pass the
// core of the expected phase (pn_header(...) sliced) as `core`. The result is
// divided by the I/Q chip scale so a unit tap reads 1. O(N^2) reference code.
inline std::vector<std::complex<double>> cir_from_core(const HeaderMode& m,
        const std::vector<std::complex<double>>& rx_core, const std::vector<int>& core) {
    const int N = m.core_len;
    if (!m.cyclic || static_cast<int>(rx_core.size()) != N || static_cast<int>(core.size()) != N)
        throw std::invalid_argument("dtmb_std: cir_from_core needs a cyclic mode and N samples");
    std::vector<std::complex<double>> R(static_cast<size_t>(N));
    std::complex<double> sumR = 0;
    for (int d = 0; d < N; ++d) {
        std::complex<double> acc = 0;
        for (int i = 0; i < N; ++i) acc += rx_core[static_cast<size_t>((i + d) % N)] * static_cast<double>(core[static_cast<size_t>(i)]);
        R[static_cast<size_t>(d)] = acc;
        sumR += acc;
    }
    const std::complex<double> scale = header_symbol(m, 1);
    for (auto& r : R) r = (r + sumR) / (static_cast<double>(N + 1) * scale);
    return R;
}

} // namespace dtmb_std
