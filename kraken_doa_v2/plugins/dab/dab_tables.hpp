#pragma once
// DAB transmission mode I constants and the registered tables, behind small
// functions. The raw tables are in dab_tables_gen.hpp (machine-extracted from
// the ETSI documents' text); this file derives what the receiver needs from
// them and checks them (self_test()).

#include <cstdint>
#include <string>
#include <vector>

namespace dab {

// --- transmission mode I (EN 300 401 clause 14.2, table 22) in samples at 2.4 MHz
// (one elementary period T = 1 / 2.048 MHz = 75 / 64 samples)
constexpr double FS = 2400000.0;
constexpr int NFFT = 2400;               // Tu = 2048 T = 2400 samples: one carrier per FFT bin
constexpr int K = 1536;                  // carriers
constexpr int L = 76;                    // OFDM symbols after the null symbol
constexpr int FRAME = 230400;            // TF = 196608 T (whole samples)
constexpr double T_NULL = 3112.5;        // 2656 T
constexpr double T_S = 2990.625;         // 2552 T
constexpr double T_G = 590.625;          // 504 T (guard interval)
constexpr int CIF_BITS = 55296;          // 864 CU x 64 bits
constexpr int SYM_BITS = 2 * K;          // 3072 bits per OFDM symbol

// carrier index k (-768..-1, 1..768) <-> dense index 0..1535
inline int carrier_index(int k) { return k < 0 ? k + 768 : k + 767; }
inline int carrier_of_index(int i) { return i < 768 ? i - 768 : i - 767; }

// phase reference symbol: z_1,k = e^(j phi_k), phi_k = pi/2 (h_i,k-k' + n) (clause 14.3.2)
// returns h_i,k-k' + n mod 4 (the phase in quarter turns) for k != 0
int prs_quarter(int k);
// frequency interleaving (clause 14.6.1): QPSK symbol n -> carrier k = F(n)
const std::vector<int>& freq_interleave();   // 1536 entries, k values
// TII (clause 14.8.1): a_b(p), b = 0..7 for pattern (MainId) p = 0..69
bool tii_bit(int p, int b);
int tii_pattern_of(unsigned mask);       // 8-bit mask, bit b = a_b -> p, or -1
unsigned tii_mask(int p);
// the first carrier of the pair of comb c, element b, repetition q (0..3)
inline int tii_carrier(int c, int b, int q) {
    static const int base[4] = {-768, -384, 1, 385};
    return base[q] + 2 * c + 48 * b;
}

// puncturing vector PI (1..24): bit i (0..31) transmitted?
bool punct_bit(int pi, int i);
int punct_ones(int pi);                  // 8 + PI

// a sub-channel's error protection (FIG 0/1)
struct Protection {
    bool uep = true;                     // short form (table 8)
    int table_index = 0;                 // UEP: table 8 index
    int option = 0;                      // EEP: 0 = set A, 1 = set B
    int level = 0;                       // 1..5 (UEP), 1..4 (EEP)
    int size_cu = 0;
    int kbps = 0;
    bool operator==(const Protection&) const = default;
};
// fills kbps (and size / level for UEP) - false for invalid / reserved combinations
bool protection_complete(Protection& p);
std::string protection_name(const Protection& p);   // "UEP 3", "EEP 3-A"
// the convolutional coding profile: groups of 128-bit mother code blocks
// with their puncturing index, then the 24 tail bits (VT), then padding
struct Profile {
    std::vector<std::pair<int, int>> blocks;   // (count, PI)
    int pad = 0;
    int in_bits = 0;                           // I
};
bool coding_profile(const Protection& p, Profile& out);

// names (registered tables; "" = unknown)
std::string country_name(int ecc, int cid);
std::string language_name(int code);
std::string pty_name(int code, int inter_table);
std::string announcement_names(unsigned flags);   // "Road Traffic flash, News flash"
std::string user_app_name(int ua);
std::string dscty_name(int dscty);
std::string band_block(double rf_hz);   // "12C 227.360 MHz" (Band III), else "227.360 MHz"

// text: a FIG 1 / dynamic label character field in the given charset (0 =
// EBU Latin, 6 = UCS-2 / UTF-16 BE, 15 = UTF-8) -> UTF-8, trailing blanks off
std::string label_to_utf8(const uint8_t* p, int n, int charset);
// UTF-8 for display: control characters and invalid bytes escaped ("\xNN"
// like kp::printable), valid multi-byte characters kept
std::string printable_utf8(const std::string& s);
// at most n bytes, cut at a character boundary
std::string utf8_truncate(const std::string& s, size_t n);

// checks the tables against their known properties; "" = all fine, else what failed
std::string self_test();

}   // namespace dab
