// dvbt_tables.hpp - DVB-T (ETSI EN 300 744 V1.6.2, 2015-10) reference-signal
// constants for a receiver that generates the known pilots.
// Header-only, C++17, standard library only.
//
// Every value below is taken from the standard; the clause / table / page it
// comes from is given next to it. Items marked DERIVED are arithmetic
// consequences of the standard's text, not values printed in it.
// 2K and 8K modes only (the DVB-H 4K mode of annex F is not covered).
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace dvbt_std {

// ---------------------------------------------------------------------------
// OFDM parameters - clause 4.4, Table 4 (p. 26), Table 5 (p. 27)
// ---------------------------------------------------------------------------
inline constexpr int KMIN = 0;              // Table 4: Kmin = 0 (both modes)
inline constexpr int K_2K = 1705;           // Table 4: number of carriers K, 2K
inline constexpr int K_8K = 6817;           // Table 4: number of carriers K, 8K
inline constexpr int KMAX_2K = 1704;        // Table 4 / clause 4.4
inline constexpr int KMAX_8K = 6816;        // Table 4 / clause 4.4

// Table 5: duration of the useful part TU = 2048 x T (2K), 8192 x T (8K).
// DERIVED: an FFT sampled at 1/T has these sizes (annex D only says "N samples").
inline constexpr int TU_IN_T_2K = 2048;
inline constexpr int TU_IN_T_8K = 8192;
inline constexpr int FFT_SIZE_2K = TU_IN_T_2K;
inline constexpr int FFT_SIZE_8K = TU_IN_T_8K;

// clause 4.4 / k': k' = k - (Kmax + Kmin)/2 is the carrier index relative to
// the centre frequency fc (p. 26); annex D.2 (p. 46-47): the k' = 0 carrier is
// emitted at fc. DERIVED: centre carrier index.
inline constexpr int KCENTER_2K = (KMAX_2K + KMIN) / 2;   // 852
inline constexpr int KCENTER_8K = (KMAX_8K + KMIN) / 2;   // 3408

// Data carriers per symbol (constant from symbol to symbol) - clause 4.5.1 (p. 27)
inline constexpr int DATA_CARRIERS_2K = 1512;
inline constexpr int DATA_CARRIERS_8K = 6048;

// Frame structure - clause 4.4 (p. 25), clause 4.6.1 (p. 30)
inline constexpr int SYMBOLS_PER_FRAME = 68;      // symbols numbered 0..67
inline constexpr int FRAMES_PER_SUPERFRAME = 4;   // frames numbered 1..4 (Table 10)

// Guard interval fractions Delta/TU - Table 5 (p. 27); TPS Table 14 (p. 33)
// signals them as s36 s37: 00 = 1/32, 01 = 1/16, 10 = 1/8, 11 = 1/4.
inline constexpr std::array<int, 4> GUARD_DENOMINATORS = {4, 8, 16, 32};  // 1/4,1/8,1/16,1/32
// Guard length in units of T: TU_IN_T / denominator (Table 5: 8K 2048/1024/512/256 T,
// 2K 512/256/128/64 T).
inline constexpr int guard_in_T(int tu_in_T, int guard_den) { return tu_in_T / guard_den; }

// Elementary period T in microseconds, as a fraction num/den - clause 4.4 (p. 26):
// "7/64 us for 8 MHz channels, 1/8 us for 7 MHz channels, 7/48 us for 6 MHz
//  channels and 7/40 us for 5 MHz channels" (also annex E p. 48, Table G.1 p. 63).
// Annex E: clock frequency 64/7 MHz (8 MHz ch), 48/7 MHz (6 MHz), 8 MHz (7 MHz).
// DERIVED: all four equal T = 7 / (8 * B) us with B the channel bandwidth in MHz.
struct Fraction { int num; int den; };
inline constexpr Fraction elementary_period_us_frac(int bw_mhz)
{
    return bw_mhz == 8 ? Fraction{7, 64}
         : bw_mhz == 7 ? Fraction{1, 8}
         : bw_mhz == 6 ? Fraction{7, 48}
         : bw_mhz == 5 ? Fraction{7, 40}
         : Fraction{0, 1};                     // not defined by the standard
}
inline constexpr double elementary_period_us(int bw_mhz)
{
    const Fraction f = elementary_period_us_frac(bw_mhz);
    return static_cast<double>(f.num) / static_cast<double>(f.den);
}
// Sample rate 1/T in Hz (DERIVED from T): 8 MHz -> 64e6/7, 7 -> 8e6, 6 -> 48e6/7, 5 -> 40e6/7.
inline constexpr double sample_rate_hz(int bw_mhz)
{
    const Fraction f = elementary_period_us_frac(bw_mhz);
    return f.num ? 1e6 * static_cast<double>(f.den) / static_cast<double>(f.num) : 0.0;
}
// Carrier spacing 1/TU in Hz (DERIVED; Table 4 prints the rounded 1 116 Hz / 4 464 Hz
// for 8 MHz, Tables E.1 / E.4 / G.1 the other bandwidths).
inline constexpr double carrier_spacing_hz(int bw_mhz, bool mode8k)
{
    return sample_rate_hz(bw_mhz) / (mode8k ? TU_IN_T_8K : TU_IN_T_2K);
}
// Baseband frequency of carrier k relative to fc: k' / TU with
// k' = k - (Kmax + Kmin)/2 (clause 4.4, p. 26). Annex D.3: the highest radiated
// frequency carries c(Kmax), i.e. frequency increases with k.
inline constexpr double carrier_offset_hz(int k, int bw_mhz, bool mode8k)
{
    const int kc = mode8k ? KCENTER_8K : KCENTER_2K;
    return (k - kc) * carrier_spacing_hz(bw_mhz, mode8k);
}

// ---------------------------------------------------------------------------
// Continual pilot carriers - clause 4.5.4, Table 7 (p. 29)
// ---------------------------------------------------------------------------
inline constexpr std::array<int, 45> CONTINUAL_2K = {
       0,   48,   54,   87,  141,  156,  192,  201,  255,  279,  282,  333,
     432,  450,  483,  525,  531,  618,  636,  714,  759,  765,  780,  804,
     873,  888,  918,  939,  942,  969,  984, 1050, 1101, 1107, 1110, 1137,
    1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704};

inline constexpr std::array<int, 177> CONTINUAL_8K = {
       0,   48,   54,   87,  141,  156,  192,  201,  255,  279,  282,  333,
     432,  450,  483,  525,  531,  618,  636,  714,  759,  765,  780,  804,
     873,  888,  918,  939,  942,  969,  984, 1050, 1101, 1107, 1110, 1137,
    1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704, 1752, 1758, 1791,
    1845, 1860, 1896, 1905, 1959, 1983, 1986, 2037, 2136, 2154, 2187, 2229,
    2235, 2322, 2340, 2418, 2463, 2469, 2484, 2508, 2577, 2592, 2622, 2643,
    2646, 2673, 2688, 2754, 2805, 2811, 2814, 2841, 2844, 2850, 2910, 2973,
    3027, 3081, 3195, 3387, 3408, 3456, 3462, 3495, 3549, 3564, 3600, 3609,
    3663, 3687, 3690, 3741, 3840, 3858, 3891, 3933, 3939, 4026, 4044, 4122,
    4167, 4173, 4188, 4212, 4281, 4296, 4326, 4347, 4350, 4377, 4392, 4458,
    4509, 4515, 4518, 4545, 4548, 4554, 4614, 4677, 4731, 4785, 4899, 5091,
    5112, 5160, 5166, 5199, 5253, 5268, 5304, 5313, 5367, 5391, 5394, 5445,
    5544, 5562, 5595, 5637, 5643, 5730, 5748, 5826, 5871, 5877, 5892, 5916,
    5985, 6000, 6030, 6051, 6054, 6081, 6096, 6162, 6213, 6219, 6222, 6249,
    6252, 6258, 6318, 6381, 6435, 6489, 6603, 6795, 6816};

// ---------------------------------------------------------------------------
// TPS carriers - clause 4.6.0, Table 8 (p. 30)
// ---------------------------------------------------------------------------
inline constexpr std::array<int, 17> TPS_2K = {
      34,   50,  209,  346,  413,  569,  595,  688,  790,  901, 1073, 1219,
    1262, 1286, 1469, 1594, 1687};

inline constexpr std::array<int, 68> TPS_8K = {
      34,   50,  209,  346,  413,  569,  595,  688,  790,  901, 1073, 1219,
    1262, 1286, 1469, 1594, 1687, 1738, 1754, 1913, 2050, 2117, 2273, 2299,
    2392, 2494, 2605, 2777, 2923, 2966, 2990, 3173, 3298, 3391, 3442, 3458,
    3617, 3754, 3821, 3977, 4003, 4096, 4198, 4309, 4481, 4627, 4670, 4694,
    4877, 5002, 5095, 5146, 5162, 5321, 5458, 5525, 5681, 5707, 5800, 5902,
    6013, 6185, 6331, 6374, 6398, 6581, 6706, 6799};

// TPS synchronization word s1..s16 (s1 = MSB here, "left most bit is sent
// first") - clause 4.6.2.2 (p. 31), Table 9:
// frames 1 and 3 of a super-frame: 0011010111101110
// frames 2 and 4 of a super-frame: 1100101000010001 (bitwise complement)
inline constexpr uint16_t TPS_SYNC_FRAMES_1_3 = 0x35EE;   // 0011 0101 1110 1110
inline constexpr uint16_t TPS_SYNC_FRAMES_2_4 = 0xCA11;   // 1100 1010 0001 0001

// ---------------------------------------------------------------------------
// Reference sequence w_k - clause 4.5.2, Figure 10 (p. 27-28)
// ---------------------------------------------------------------------------
// Generator polynomial X^11 + X^2 + 1; 11 one-bit delays all initialised to 1;
// Figure 10: feedback = XOR of the outputs of delay 9 and delay 11, fed into
// delay 1; the sequence is read at the output of delay 11.
// "The PRBS is initialized so that the first output bit from the PRBS coincides
//  with the first active carrier. A new value is generated by the PRBS on every
//  used carrier (whether or not it is a pilot)."  -> w_k = output bit number k,
// k = Kmin = 0 first. The standard's check value: "PRBS sequence starts:
// 1111111111100..." (Figure 10).
// w_k depends on k only (the pilot formulas of 4.5.3 / 4.5.4 / 4.6.3 contain no
// l or m), so the same sequence is used in every symbol and frame; the 2K
// sequence is the first 1705 values of the 8K one.
inline std::vector<uint8_t> prbs_w(int kmax)
{
    std::vector<uint8_t> w;
    if (kmax < 0) return w;
    w.resize(static_cast<size_t>(kmax) + 1);
    uint16_t reg = 0x7FF;                 // bit (i-1) = content of delay i, i = 1..11
    for (int k = 0; k <= kmax; ++k) {
        const uint8_t out = static_cast<uint8_t>((reg >> 10) & 1u);           // delay 11
        const uint16_t fb = static_cast<uint16_t>(((reg >> 8) ^ (reg >> 10)) & 1u); // d9 ^ d11
        w[static_cast<size_t>(k)] = out;
        reg = static_cast<uint16_t>(((reg << 1) | fb) & 0x7FFu);
    }
    return w;
}

namespace detail {
inline const std::vector<uint8_t>& w8k()
{
    static const std::vector<uint8_t> w = prbs_w(KMAX_8K);
    return w;
}
} // namespace detail

// ---------------------------------------------------------------------------
// Pilot positions
// ---------------------------------------------------------------------------
// Scattered pilots - clause 4.5.3 (p. 28): in symbol l (0..67) the carriers
// k = Kmin + 3 x (l mod 4) + 12p, p integer >= 0, k in [Kmin; Kmax].
// This overload does not know Kmax: the caller keeps k <= Kmax.
inline bool is_scattered(int k, int l)
{
    if (k < KMIN || l < 0) return false;
    return (k - KMIN) % 12 == 3 * (l % 4);
}
inline bool is_scattered(int k, int l, int kmax)
{
    return k <= kmax && is_scattered(k, l);
}

// Continual pilots - Table 7 (present in every symbol). mode8k != 0 -> 8K list.
inline bool is_continual(int k, int mode8k)
{
    if (mode8k) return std::binary_search(CONTINUAL_8K.begin(), CONTINUAL_8K.end(), k);
    return std::binary_search(CONTINUAL_2K.begin(), CONTINUAL_2K.end(), k);
}

// TPS carriers - Table 8 (present in every symbol).
inline bool is_tps(int k, int mode8k)
{
    if (mode8k) return std::binary_search(TPS_8K.begin(), TPS_8K.end(), k);
    return std::binary_search(TPS_2K.begin(), TPS_2K.end(), k);
}

// ---------------------------------------------------------------------------
// Pilot / TPS cell values
// ---------------------------------------------------------------------------
inline int prbs_bit(int k)
{
    if (k < KMIN || k > KMAX_8K) return -1;
    return detail::w8k()[static_cast<size_t>(k)];
}

// Scattered (4.5.3, p. 28) and continual (4.5.4, p. 29) pilots, identical rule:
//   Re{c} = 4/3 x 2 (1/2 - w_k),  Im{c} = 0   ->  w_k = 0: +4/3, w_k = 1: -4/3
// Boosted: E[c c*] = 16/9 (4.5.5, p. 29); data cells have E[c c*] = 1.
// k must be in [0; 6816] (2K uses the same w_k for k <= 1704); returns 0 otherwise.
inline std::complex<double> pilot_value(int k)
{
    const int w = prbs_bit(k);
    if (w < 0) return {0.0, 0.0};
    return {4.0 / 3.0 * 2.0 * (0.5 - static_cast<double>(w)), 0.0};
}

// TPS - clause 4.6.3 (p. 34): normal power E[c c*] = 1, DBPSK, all TPS carriers
// of a symbol carry the same bit s_l. First symbol of a frame (l = 0):
//   Re{c} = 2 (1/2 - w_k), Im{c} = 0          (absolute reference, +1 / -1)
// Symbols l > 0: s_l = 0 -> c(l) = c(l-1); s_l = 1 -> c(l) = -c(l-1).
// The DBPSK is re-initialised at the start of every TPS block (= frame).
inline std::complex<double> tps_initial_value(int k)
{
    const int w = prbs_bit(k);
    if (w < 0) return {0.0, 0.0};
    return {2.0 * (0.5 - static_cast<double>(w)), 0.0};
}
// TPS cell of carrier k in symbol l given the frame's TPS bits s[0..67]
// (s[0] is the initialisation bit and is not used for the value).
inline std::complex<double> tps_value(int k, int l, const std::array<uint8_t, 68>& s)
{
    std::complex<double> c = tps_initial_value(k);
    for (int i = 1; i <= l && i < SYMBOLS_PER_FRAME; ++i)
        if (s[static_cast<size_t>(i)] & 1u) c = -c;
    return c;
}

} // namespace dvbt_std
