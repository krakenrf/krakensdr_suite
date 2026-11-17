// dab_gen - synthetic DAB / DAB+ (EN 300 401 transmission mode I) single
// frequency network on kraken_doa's uniform circular array: the test signal
// generator of the dab plugin, written from the ETSI standards alone as an
// independent check of the receiver.
//
// What it writes: a complete ensemble (FIC with FIG 0/0, 0/1, 0/2, 0/5, 0/9,
// 0/10, 0/17, 0/22, 1/0, 1/1, 2/0; MSC sub-channels with UEP / EEP
// convolutional coding, energy dispersal, time interleaving over 16 CIFs, CIF
// multiplexing, frequency interleaving, pi/4-DQPSK, the phase reference and
// null symbols), audio services that decode to a known tone each (DAB:
// MPEG-1 / MPEG-2 LSF Layer II with ScF-CRC and PAD; DAB+: AAC-LC 960 in
// audio super frames with Fire code, AU CRCs and RS(120,110)), a dynamic
// label per service (short X-PAD), and per transmitter its TII (MainId /
// SubId carriers in the null symbol of every other frame). N transmitters
// send the same ensemble, each from its own bearing, delay, level and phase,
// received on a UCA (ANT0 on +x, the others CLOCKWISE; bearings as the MUSIC
// plot shows them, CCW from ANT0 - as plugins/sdk/tools/uca_scene.py), with
// white noise, an optional carrier frequency offset, sample clock offset and
// offset of the block from the file's centre. Generated at 2.048 MHz and
// resampled (Kaiser windowed sinc, ~90 dB) to the output rate.
//
// Build (from kraken_doa_v2/plugins):
//   g++ -std=c++20 -O2 -Idab -Isdk dab/tools/dab_gen.cpp -o dab_gen
// Run:
//   dab_gen --scene dab/tools/scene_sfn3.json --out out.cu8          (2.4 MHz, 5 antennas, + out.json)
//   dab_gen --scene S.json --rate 2048000 --channels 1 --out ref.u8   (rtl_sdr style, for reference receivers)
//   dab_gen --scene S.json --describe truth.json --dump DIR           (ground truth; DIR/<sid>.mp2 / .loas / .au)
//   dab_gen --selftest
// Options (override the scene): --seconds S | --frames N (rounded to whole
// multiples of 10 transmission frames = 0.96 s), --rate HZ, --channels M,
// --radius M, --rf HZ, --snr DB, --seed N, --cfo HZ, --sco PPM,
// --offset-khz K, --station LAT,LON, --format cu8|cf32
//
// Scene file: see the README notes (plugins/dab/README.md) - keys "rf_hz",
// "seconds", "rate", "channels", "radius_m", "snr_db", "seed", "cfo_hz",
// "sco_ppm", "offset_khz", "station", "ensemble" {...}, "services" [...],
// "transmitters" [...] (per transmitter "tii_pair_phase": "k" = the standard's
// phi_k on both carriers of a TII pair, default, or "k+1" = phi_k+1 on the
// second, as some transmitters on the air do).
//
// Tables: dab_gen_tables.hpp (transcribed by a script from the EN 300 401 /
// TS 103 466 PDFs), dab_gen_aac_tables.hpp (AAC Huffman code books, Apache-2.0,
// from VisualOn's vo-aacenc). Portable C++20 (Linux / macOS clang / MinGW).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dab_gen_audio.hpp"
#include "dab_gen_json.hpp"

using namespace dabgen;
using cf = std::complex<float>;
using cd = std::complex<double>;

namespace {

// ------------------------------------------------------------ constants
constexpr int kFft = 2048;           // Tu = 2048 T
constexpr int kGuard = 504;          // Delta
constexpr int kNull = 2656;          // TNULL
constexpr int kSym = 2552;           // Ts
constexpr int kFrame = 196608;       // TF
constexpr int kK = 1536;             // carriers
constexpr int kL = 76;               // symbols after the null symbol
constexpr double kFs = 2048000.0;    // 1/T
constexpr int kCifBits = 55296;
constexpr int kLoopFrames = 10;      // loop granularity: LCM of CIF (1), LSF (2), super frame (5), TII (8) CIFs

// ------------------------------------------------------------ PRBS (10.1)
std::vector<uint8_t> prbs(size_t n) {
    std::vector<uint8_t> v(n);
    unsigned r = 0x1FF;
    for (size_t i = 0; i < n; i++) {
        const unsigned b = ((r >> 8) ^ (r >> 4)) & 1u;
        v[i] = static_cast<uint8_t>(b);
        r = ((r << 1) | b) & 0x1FFu;
    }
    return v;
}
const std::vector<uint8_t>& prbs_cached() {
    static const std::vector<uint8_t> p = prbs(kCifBits);
    return p;
}

// ------------------------------------------------------------ convolutional code (11)
struct Profile {
    std::vector<std::pair<int, int>> blocks;   // (number of 128-bit blocks, PI)
    int pad = 0;
};

std::vector<uint8_t> conv_encode(const std::vector<uint8_t>& a, const Profile& pr) {
    const int64_t I = static_cast<int64_t>(a.size());
    std::vector<uint8_t> u(static_cast<size_t>(4 * I + 24));
    auto A = [&](int64_t i) -> int { return (i >= 0 && i < I) ? a[static_cast<size_t>(i)] : 0; };
    for (int64_t i = 0; i < I + 6; i++) {
        u[static_cast<size_t>(4 * i + 0)] = static_cast<uint8_t>(A(i) ^ A(i - 2) ^ A(i - 3) ^ A(i - 5) ^ A(i - 6));
        u[static_cast<size_t>(4 * i + 1)] = static_cast<uint8_t>(A(i) ^ A(i - 1) ^ A(i - 2) ^ A(i - 3) ^ A(i - 6));
        u[static_cast<size_t>(4 * i + 2)] = static_cast<uint8_t>(A(i) ^ A(i - 1) ^ A(i - 4) ^ A(i - 6));
        u[static_cast<size_t>(4 * i + 3)] = static_cast<uint8_t>(A(i) ^ A(i - 2) ^ A(i - 3) ^ A(i - 5) ^ A(i - 6));
    }
    std::vector<uint8_t> out;
    int64_t pos = 0;
    int64_t nblocks = 0;
    for (const auto& [n, pi] : pr.blocks) {
        nblocks += n;
        const char* v = kPunct[pi];
        for (int b = 0; b < n; b++)
            for (int sub = 0; sub < 4; sub++)
                for (int i = 0; i < 32; i++, pos++)
                    if (v[i] == '1') out.push_back(u[static_cast<size_t>(pos)]);
    }
    if (nblocks * 128 != 4 * I) throw std::runtime_error("protection profile does not match the bit rate");
    for (int i = 0; i < 24; i++, pos++)
        if (kPunctTail[i] == '1') out.push_back(u[static_cast<size_t>(pos)]);
    for (int i = 0; i < pr.pad; i++) out.push_back(0);
    return out;
}

// ------------------------------------------------------------ frequency interleaving (14.6.1)
const std::vector<int>& freq_map() {
    static const std::vector<int> F = [] {
        std::vector<int> f;
        int p = 0;
        for (int i = 0; i < 2048; i++) {
            if (i) p = (13 * p + 511) % 2048;
            if (p >= 256 && p <= 1792 && p != 1024) f.push_back(p - 1024);
        }
        return f;
    }();
    return F;
}

// ------------------------------------------------------------ phase reference (14.3.2)
double prs_phase(int k) {
    for (const auto& r : kPrsTab)
        if (k >= r[0] && k <= r[1]) return kPi / 2 * (kPrsH[r[3]][k - r[2]] + r[4]);
    throw std::runtime_error("PRS carrier");
}

// ------------------------------------------------------------ TII (14.8.1)
std::vector<int> tii_carriers(int c, int p) {   // first carrier of each active pair
    std::vector<int> v;
    for (int b = 0; b < 8; b++)
        if (kTiiPattern[p][b] == '1')
            for (int base : {-768, -384, 1, 385}) v.push_back(base + 2 * c + 48 * b);
    std::sort(v.begin(), v.end());
    return v;
}

// ------------------------------------------------------------ FFT (radix 2, inverse = +j)
void ifft(std::vector<cd>& a) {
    const int n = static_cast<int>(a.size());
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(j)]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / len;
        const cd wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            cd w(1, 0);
            for (int j = 0; j < len / 2; j++) {
                const cd u = a[static_cast<size_t>(i + j)], v = a[static_cast<size_t>(i + j + len / 2)] * w;
                a[static_cast<size_t>(i + j)] = u + v;
                a[static_cast<size_t>(i + j + len / 2)] = u - v;
                w *= wl;
            }
        }
    }
}

// carriers -> time samples with a cyclic prefix of `cp` (sample n = IFFT[(n - cp) mod 2048])
void ofdm(const std::vector<cd>& z /* index k + 768 */, int cp, int len, cf* out) {
    std::vector<cd> X(kFft, cd(0, 0));
    for (int k = -768; k <= 768; k++)
        if (k) X[static_cast<size_t>((k + kFft) % kFft)] = z[static_cast<size_t>(k + 768)];
    ifft(X);
    const double sc = 1.0 / std::sqrt(static_cast<double>(kK));
    for (int n = 0; n < len; n++) {
        const cd v = X[static_cast<size_t>(((n - cp) % kFft + kFft) % kFft)] * sc;
        out[n] = cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
}

// ------------------------------------------------------------ helpers
std::string hex(unsigned v, int w) {
    char b[16];
    std::snprintf(b, sizeof b, "%0*X", w, v);
    return b;
}
std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else
            o += static_cast<char>(c);
    }
    return o + "\"";
}
unsigned parse_hex(const std::string& s) { return static_cast<unsigned>(std::stoul(s, nullptr, 16)); }

// great-circle destination (deg, compass bearing deg, km)
std::pair<double, double> destination(double lat, double lon, double brg, double km) {
    const double R = 6371.0088, d = km / R, b = brg * kPi / 180, p1 = lat * kPi / 180, l1 = lon * kPi / 180;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    double lo = l2 * 180 / kPi;
    while (lo > 180) lo -= 360;
    while (lo < -180) lo += 360;
    return {p2 * 180 / kPi, lo};
}

// ------------------------------------------------------------ ensemble model
struct Service {
    unsigned sid = 0;
    std::string label, short_label, ext_label, dls, type = "dab", prot = "3";
    int pty = 0, lang = 9, kbps = 128, rate_hz = 48000, subchid = 0, start = -1, cu = 0;
    double tone_req = 1000, tone = 0;
    // sub-channel organisation
    bool uep = true;
    int uep_index = 0, eep_option = 0, eep_level = 0;
    Profile profile;
    std::string prot_desc;
    std::vector<std::vector<uint8_t>> lframes;   // per CIF of the loop (I bits as bytes)
    std::vector<std::vector<uint8_t>> coded;     // per CIF: convolutional codeword (bits)
    int ascty() const { return type == "dabplus" ? 63 : 0; }
};

struct Transmitter {
    std::string name;
    int main_id = 1, sub_id = 1;
    double bearing = 0, delay_us = 0, level_db = 0, phase_deg = 0, range_km = 20;
    double lat = 0, lon = 0;
    // the phase reference value on the second carrier of each TII pair:
    // false = phi_k of the first carrier (EN 300 401 clause 14.8, both carriers
    // alike), true = phi_k+1 (seen on several transmitters on the air)
    bool pair_k1 = false;
    std::vector<cf> tii;   // null symbol samples (kNull)
};

struct Ensemble {
    unsigned eid = 0x10BC, ecc = 0xE0;
    std::string label = "Kraken DAB", short_label = "Kraken", ext_label;
    int lto = 0, inter_table = 1;
    int64_t utc_ms = 0;   // ms since 1970 at frame 0's null symbol
    std::vector<Service> svc;
    int cif0 = 0;
};

// MJD from a civil date
int mjd(int y, int m, int d) {
    // days since 1970-01-01 (Howard Hinnant's algorithm), MJD 40587 = 1970-01-01
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 40587;
}

// abbreviated label flags: the characters of `shortl`, in order, inside `label`
uint16_t char_flags(const std::string& label, const std::string& shortl) {
    uint16_t f = 0;
    size_t p = 0;
    int n = 0;
    for (size_t i = 0; i < label.size() && i < 16 && p < shortl.size() && n < 8; i++)
        if (label[i] == shortl[p]) {
            f |= static_cast<uint16_t>(0x8000u >> i);
            p++;
            n++;
        }
    if (p < shortl.size() || shortl.empty()) {
        f = 0;
        for (size_t i = 0; i < label.size() && i < 8; i++) f |= static_cast<uint16_t>(0x8000u >> i);
    }
    return f;
}

void setup_protection(Service& s) {
    const std::string& p = s.prot;
    if (p.size() == 1 && p[0] >= '1' && p[0] <= '5') {
        if (s.type == "dabplus") throw std::runtime_error("DAB+ needs EEP protection (1A..4A, 1B..4B)");
        const int lvl = p[0] - '0';
        for (const auto& u : kUep)
            if (u.kbps == s.kbps && u.level == lvl) {
                s.uep = true;
                s.uep_index = u.index;
                s.cu = u.cu;
                s.profile.blocks.clear();
                for (int q = 0; q < 4; q++)
                    if (u.L[q]) s.profile.blocks.push_back({u.L[q], u.PI[q]});
                s.profile.pad = u.pad;
                s.prot_desc = "UEP " + std::to_string(lvl) + " (table 8 index " + std::to_string(u.index) + ")";
                return;
            }
        throw std::runtime_error("no UEP profile for " + std::to_string(s.kbps) + " kbit/s level " + p);
    }
    if (p.size() == 2 && p[0] >= '1' && p[0] <= '4' && (p[1] == 'A' || p[1] == 'B' || p[1] == 'a' || p[1] == 'b')) {
        const int lvl = p[0] - '0';
        const bool B = p[1] == 'B' || p[1] == 'b';
        s.uep = false;
        s.eep_option = B ? 1 : 0;
        s.eep_level = lvl;
        s.profile.blocks.clear();
        s.profile.pad = 0;
        if (!B) {
            if (s.kbps % 8) throw std::runtime_error("EEP-A: bit rate a multiple of 8 kbit/s");
            const int n = s.kbps / 8;
            static const int cu[5] = {0, 12, 8, 6, 4};
            s.cu = cu[lvl] * n;
            if (lvl == 4) s.profile.blocks = {{4 * n - 3, 3}, {2 * n + 3, 2}};
            if (lvl == 3) s.profile.blocks = {{6 * n - 3, 8}, {3, 7}};
            if (lvl == 2) s.profile.blocks = n == 1 ? std::vector<std::pair<int, int>>{{5, 13}, {1, 12}}
                                                    : std::vector<std::pair<int, int>>{{2 * n - 3, 14}, {4 * n + 3, 13}};
            if (lvl == 1) s.profile.blocks = {{6 * n - 3, 24}, {3, 23}};
        } else {
            if (s.kbps % 32) throw std::runtime_error("EEP-B: bit rate a multiple of 32 kbit/s");
            const int n = s.kbps / 32;
            static const int cu[5] = {0, 27, 21, 18, 15};
            static const int pi1[5] = {0, 10, 6, 4, 2};
            s.cu = cu[lvl] * n;
            s.profile.blocks = {{24 * n - 3, pi1[lvl]}, {3, pi1[lvl] - 1}};
        }
        s.prot_desc = "EEP " + std::to_string(lvl) + "-" + (B ? "B" : "A");
        return;
    }
    throw std::runtime_error("protection \"" + p + "\": 1..5 (UEP) or 1A..4A / 1B..4B (EEP)");
}

// ------------------------------------------------------------ FIGs
struct Fig {
    std::vector<uint8_t> b;
};
Fig fig0(int ext, const std::vector<uint8_t>& body, int cn = 0, int oe = 0, int pd = 0) {
    Fig f;
    if (body.size() + 1 > 29) throw std::runtime_error("FIG 0/" + std::to_string(ext) + " too long");
    f.b.push_back(static_cast<uint8_t>((0 << 5) | (body.size() + 1)));
    f.b.push_back(static_cast<uint8_t>((cn << 7) | (oe << 6) | (pd << 5) | ext));
    f.b.insert(f.b.end(), body.begin(), body.end());
    return f;
}
void put16(std::vector<uint8_t>& v, unsigned x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

Fig fig1(int ext, unsigned id, const std::string& label, const std::string& shortl) {
    Fig f;
    f.b.push_back(static_cast<uint8_t>((1 << 5) | 21));
    f.b.push_back(static_cast<uint8_t>((0 << 4) | ext));   // charset 0000 (EBU Latin), Rfu, extension
    put16(f.b, id);
    for (int i = 0; i < 16; i++) f.b.push_back(i < static_cast<int>(label.size()) ? static_cast<uint8_t>(label[static_cast<size_t>(i)]) : 0);
    put16(f.b, char_flags(label, shortl));
    return f;
}

// FIG 2 (Ver 1, UTF-8), the label in two segments
std::vector<Fig> fig2(int ext, unsigned id, const std::string& utf8) {
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>((0 << 7) | (1 << 4) | 0));   // UTF-8, segment count 1 (= 2 segments), text control 0
    for (size_t i = 0; i < utf8.size() && i < 32; i++) data.push_back(static_cast<uint8_t>(utf8[i]));
    const size_t half = (data.size() + 1) / 2;
    std::vector<Fig> out;
    for (int seg = 0; seg < 2; seg++) {
        Fig f;
        const size_t a = seg ? half : 0, e = seg ? data.size() : half;
        const size_t len = 1 + 2 + (e - a);
        f.b.push_back(static_cast<uint8_t>((2 << 5) | len));
        f.b.push_back(static_cast<uint8_t>((0 << 7) | (seg << 4) | (1 << 3) | ext));   // toggle 0, segment index, Ver 1
        put16(f.b, id);
        f.b.insert(f.b.end(), data.begin() + static_cast<long>(a), data.begin() + static_cast<long>(e));
        out.push_back(f);
    }
    return out;
}

struct FicBuilder {
    const Ensemble* E = nullptr;
    const std::vector<Transmitter>* T = nullptr;

    Fig f00(int cif) const {
        std::vector<uint8_t> b;
        put16(b, E->eid);
        const int hi = (cif / 250) % 20, lo = cif % 250;
        // change flags 00, Al 0, CIF count high (5) / low (8)
        b.push_back(static_cast<uint8_t>((0 << 6) | (0 << 5) | hi));
        b.push_back(static_cast<uint8_t>(lo));
        return fig0(0, b);
    }
    std::vector<Fig> f01() const {
        std::vector<Fig> out;
        std::vector<uint8_t> b;
        for (const auto& s : E->svc) {
            std::vector<uint8_t> e;
            const unsigned w = (static_cast<unsigned>(s.subchid) << 10) | static_cast<unsigned>(s.start);
            put16(e, w);
            if (s.uep) e.push_back(static_cast<uint8_t>((0 << 7) | (0 << 6) | s.uep_index));
            else {
                const unsigned lf = (1u << 15) | (static_cast<unsigned>(s.eep_option) << 12) |
                                    (static_cast<unsigned>(s.eep_level - 1) << 10) | static_cast<unsigned>(s.cu);
                put16(e, lf);
            }
            if (b.size() + e.size() + 1 > 29) {
                out.push_back(fig0(1, b));
                b.clear();
            }
            b.insert(b.end(), e.begin(), e.end());
        }
        if (!b.empty()) out.push_back(fig0(1, b));
        return out;
    }
    std::vector<Fig> f02() const {
        std::vector<Fig> out;
        std::vector<uint8_t> b;
        for (const auto& s : E->svc) {
            std::vector<uint8_t> e;
            put16(e, s.sid);
            e.push_back(static_cast<uint8_t>((0 << 7) | (0 << 4) | 1));   // PP 0, CAId 0, 1 component
            // TMId 00 (stream audio), ASCTy, SubChId, P/S = primary, CA 0
            const unsigned c = (0u << 14) | (static_cast<unsigned>(s.ascty()) << 8) | (static_cast<unsigned>(s.subchid) << 2) | (1u << 1);
            put16(e, c);
            if (b.size() + e.size() + 1 > 29) {
                out.push_back(fig0(2, b));
                b.clear();
            }
            b.insert(b.end(), e.begin(), e.end());
        }
        if (!b.empty()) out.push_back(fig0(2, b));
        return out;
    }
    Fig f05() const {
        std::vector<uint8_t> b;
        for (const auto& s : E->svc) {
            b.push_back(static_cast<uint8_t>((0 << 7) | s.subchid));   // short form
            b.push_back(static_cast<uint8_t>(s.lang));
        }
        return fig0(5, b);
    }
    Fig f09() const {
        std::vector<uint8_t> b;
        const int a = std::abs(E->lto);
        b.push_back(static_cast<uint8_t>((0 << 7) | (0 << 6) | ((E->lto < 0 ? 1 : 0) << 5) | (a & 31)));
        b.push_back(static_cast<uint8_t>(E->ecc));
        b.push_back(static_cast<uint8_t>(E->inter_table));
        return fig0(9, b);
    }
    Fig f10(int64_t ms) const {
        const int64_t day = ms / 86400000, rem = ms % 86400000;
        const unsigned mj = static_cast<unsigned>(40587 + day);
        const unsigned h = static_cast<unsigned>(rem / 3600000), mi = static_cast<unsigned>(rem / 60000 % 60),
                       se = static_cast<unsigned>(rem / 1000 % 60), msx = static_cast<unsigned>(rem % 1000);
        BitWriter w;
        w.put(0, 1);    // Rfu
        w.put(mj, 17);  // MJD
        w.put(0, 1);    // LSI
        w.put(0, 1);    // Rfa
        w.put(1, 1);    // UTC long form
        w.put(h, 5);
        w.put(mi, 6);
        w.put(se, 6);
        w.put(msx, 10);
        return fig0(10, w.bytes);
    }
    Fig f17() const {
        std::vector<uint8_t> b;
        for (const auto& s : E->svc) {
            put16(b, s.sid);
            b.push_back(0);   // S/D 0, Rfa1, Rfu1, Rfa2 (high bits)
            b.push_back(static_cast<uint8_t>(s.pty & 31));
        }
        return fig0(17, b);
    }
    // FIG 0/22 TII database, as EN 300 401 V1.4.1 clause 8.1.9 defined it (the
    // feature was removed in V2.1.1): per MainId a main identifier field
    // (M/S 0, MainId, latitude / longitude coarse 16 bits + fine 4 bits) and a
    // sub-identifier field (M/S 1, MainId, Rfu, number of SubId fields; per
    // SubId: SubId 5, TD 11 (us), latitude / longitude offset 16 bits each)
    std::vector<Fig> f22() const {
        std::map<int, std::vector<const Transmitter*>> by;
        for (const auto& t : *T) by[t.main_id].push_back(&t);
        std::vector<Fig> out;
        for (const auto& [mid, list] : by) {
            double la = 0, lo = 0;
            for (auto* t : list) {
                la += t->lat;
                lo += t->lon;
            }
            la /= static_cast<double>(list.size());
            lo /= static_cast<double>(list.size());
            const long vla = std::lround(la * 524288.0 / 90.0), vlo = std::lround(lo * 524288.0 / 180.0);
            for (size_t a = 0; a < list.size(); a += 4) {
                BitWriter w;
                if (a == 0) {
                    w.put(0, 1);
                    w.put(static_cast<uint64_t>(mid), 7);
                    w.put(static_cast<uint64_t>((vla >> 4) & 0xFFFF), 16);
                    w.put(static_cast<uint64_t>((vlo >> 4) & 0xFFFF), 16);
                    w.put(static_cast<uint64_t>(vla & 15), 4);
                    w.put(static_cast<uint64_t>(vlo & 15), 4);
                }
                const size_t n = std::min<size_t>(4, list.size() - a);
                w.put(1, 1);
                w.put(static_cast<uint64_t>(mid), 7);
                w.put(0, 5);
                w.put(n, 3);
                for (size_t k = a; k < a + n; k++) {
                    const Transmitter* t = list[k];
                    w.put(static_cast<uint64_t>(t->sub_id), 5);
                    w.put(static_cast<uint64_t>(std::clamp<long>(std::lround(t->delay_us), 0, 2047)), 11);
                    w.put(static_cast<uint64_t>(std::lround((t->lat - la) * 524288.0 / 90.0) & 0xFFFF), 16);
                    w.put(static_cast<uint64_t>(std::lround((t->lon - lo) * 524288.0 / 180.0) & 0xFFFF), 16);
                }
                out.push_back(fig0(22, w.bytes, a == 0 ? 0 : 1, 0, 0));   // C/N: start / continuation of the entry
            }
        }
        return out;
    }

    // the 12 FIBs of transmission frame m (3 per CIF); FIG 0/0 first in FIB 0.
    // MCI (0/1, 0/2) every frame; SI in a cycle of 5 frames (0.48 s, more often
    // than the once a second the standard asks for)
    std::vector<std::array<uint8_t, 32>> fibs(int m, int cif) const {
        std::vector<Fig> figs;
        figs.push_back(f00(cif));
        for (auto& f : f01()) figs.push_back(f);
        for (auto& f : f02()) figs.push_back(f);
        const int slot = m % 5;
        if (slot == 0) {
            figs.push_back(fig1(0, E->eid, E->label, E->short_label));
            figs.push_back(f09());
            figs.push_back(f10(E->utc_ms + static_cast<int64_t>(m) * 96));
        } else if (slot == 1) {
            for (const auto& s : E->svc) figs.push_back(fig1(1, s.sid, s.label, s.short_label));
        } else if (slot == 2) {
            figs.push_back(f17());
            figs.push_back(f05());
            figs.push_back(f10(E->utc_ms + static_cast<int64_t>(m) * 96));
        } else if (slot == 3) {
            if (!E->ext_label.empty())
                for (auto& f : fig2(0, E->eid, E->ext_label)) figs.push_back(f);
            for (const auto& s : E->svc)
                if (!s.ext_label.empty())
                    for (auto& f : fig2(1, s.sid, s.ext_label)) figs.push_back(f);
        } else {
            for (auto& f : f22()) figs.push_back(f);
        }
        std::vector<std::array<uint8_t, 32>> out(12);
        std::vector<int> used(12, 0);
        int cur = 0;
        for (const auto& f : figs) {
            while (cur < 12 && used[static_cast<size_t>(cur)] + static_cast<int>(f.b.size()) > 30) cur++;
            if (cur >= 12) throw std::runtime_error("FIC full");
            auto& fb = out[static_cast<size_t>(cur)];
            std::copy(f.b.begin(), f.b.end(), fb.begin() + used[static_cast<size_t>(cur)]);
            used[static_cast<size_t>(cur)] += static_cast<int>(f.b.size());
        }
        for (int i = 0; i < 12; i++) {
            auto& fb = out[static_cast<size_t>(i)];
            int u = used[static_cast<size_t>(i)];
            if (u < 30) fb[static_cast<size_t>(u++)] = 0xFF;   // end marker
            for (; u < 30; u++) fb[static_cast<size_t>(u)] = 0;
            const uint16_t c = crc16_ccitt_inv(fb.data(), 30);
            fb[30] = static_cast<uint8_t>(c >> 8);
            fb[31] = static_cast<uint8_t>(c & 0xFF);
        }
        return out;
    }
};

// ------------------------------------------------------------ the transmitter
struct Generator {
    Ensemble E;
    std::vector<Transmitter> tx;
    int frames = 50;
    int ncif() const { return 4 * frames; }
    Profile fic_profile{{{21, 16}, {3, 15}}, 0};
    std::vector<std::vector<uint8_t>> fic_bits;   // per frame: 9216 bits (B')
    std::string dump_dir;
    RsEncoder rs;

    void build_audio() {
        const int N = ncif();
        for (auto& s : E.svc) {
            s.lframes.assign(static_cast<size_t>(N), {});
            const std::string dls = s.dls;
            const int charset = 0;
            std::vector<uint8_t> dump_mp2, dump_loas, dump_au;
            if (s.type == "dab") {
                Mp2Encoder enc;
                enc.init(s.rate_hz, s.kbps, s.tone_req, 0.25);
                s.tone = enc.tone_hz;
                const bool lsf = s.rate_hz == 24000;
                const int nfr = lsf ? N / 2 : N;
                const auto pads = dls_schedule(dls, charset, nfr);
                for (int f = 0; f < nfr; f++) {
                    uint8_t x[4], fp[2];
                    pad_bytes(pads[static_cast<size_t>(f)], x, fp);
                    const auto fr = enc.frame(x, 4, fp);
                    dump_mp2.insert(dump_mp2.end(), fr.begin(), fr.end());
                    if (!lsf) s.lframes[static_cast<size_t>(f)] = fr;
                    else {
                        const size_t h = fr.size() / 2;
                        s.lframes[static_cast<size_t>(2 * f)] = std::vector<uint8_t>(fr.begin(), fr.begin() + static_cast<long>(h));
                        s.lframes[static_cast<size_t>(2 * f + 1)] = std::vector<uint8_t>(fr.begin() + static_cast<long>(h), fr.end());
                    }
                }
            } else {
                const int sidx = s.kbps / 8;
                const int nau = 6, total = 110 * sidx - 11 - 2 * nau;
                const int ausz = total / nau, rem = total % nau;
                AacEncoder enc;
                // budget: the smallest AU minus DSE (2 + 6 bytes), SCE header 7 bits, END
                enc.init(s.tone_req, 0.25 * 32768.0, 8 * (ausz - 8) - 7 - 3 - 8);
                s.tone = enc.tone_hz;
                const int nsf = N / 5;
                const auto pads = dls_schedule(dls, charset, static_cast<int64_t>(nsf) * nau);
                for (int sf = 0; sf < nsf; sf++) {
                    std::vector<std::vector<uint8_t>> aus;
                    for (int a = 0; a < nau; a++) {
                        uint8_t x[4], fp[2];
                        pad_bytes(pads[static_cast<size_t>(sf * nau + a)], x, fp);
                        std::vector<uint8_t> pad(x, x + 4);
                        pad.push_back(fp[0]);
                        pad.push_back(fp[1]);
                        aus.push_back(enc.au(pad, ausz + (a < rem ? 1 : 0)));
                        const auto& au = aus.back();
                        // test dumps: LOAS / LATM (AudioSpecificConfig in every frame) and length-prefixed raw AUs
                        BitWriter l;
                        l.put(0, 1);         // useSameStreamMux
                        l.put(0, 1);         // audioMuxVersion
                        l.put(1, 1);         // allStreamsSameTimeFraming
                        l.put(0, 6);         // numSubFrames
                        l.put(0, 4);         // numProgram
                        l.put(0, 3);         // numLayer
                        l.put(2, 5);         // AOT AAC LC
                        l.put(3, 4);         // 48 kHz
                        l.put(1, 4);         // mono
                        l.put(1, 1);         // frameLengthFlag: 960
                        l.put(0, 1);         // dependsOnCoreCoder
                        l.put(0, 1);         // extensionFlag
                        l.put(0, 3);         // frameLengthType
                        l.put(0xFF, 8);      // latmBufferFullness
                        l.put(0, 1);         // otherDataPresent
                        l.put(0, 1);         // crcCheckPresent
                        size_t left = au.size();
                        while (left >= 255) {
                            l.put(255, 8);
                            left -= 255;
                        }
                        l.put(left, 8);
                        for (uint8_t c : au) l.put(c, 8);
                        l.align();
                        dump_loas.push_back(0x56);
                        dump_loas.push_back(static_cast<uint8_t>(0xE0 | (l.bytes.size() >> 8)));
                        dump_loas.push_back(static_cast<uint8_t>(l.bytes.size() & 0xFF));
                        dump_loas.insert(dump_loas.end(), l.bytes.begin(), l.bytes.end());
                        dump_au.push_back(static_cast<uint8_t>(au.size() >> 8));
                        dump_au.push_back(static_cast<uint8_t>(au.size() & 0xFF));
                        dump_au.insert(dump_au.end(), au.begin(), au.end());
                    }
                    const auto O = superframe(sidx, aus, rs);
                    for (int j = 0; j < 5; j++)
                        s.lframes[static_cast<size_t>(5 * sf + j)] =
                            std::vector<uint8_t>(O.begin() + j * 24 * sidx, O.begin() + (j + 1) * 24 * sidx);
                }
            }
            if (!dump_dir.empty()) {
                auto wr = [&](const std::string& ext, const std::vector<uint8_t>& d) {
                    if (d.empty()) return;
                    const std::string p = dump_dir + "/" + hex(s.sid, 4) + ext;
                    std::FILE* f = std::fopen(p.c_str(), "wb");
                    if (!f) throw std::runtime_error("cannot write " + p);
                    std::fwrite(d.data(), 1, d.size(), f);
                    std::fclose(f);
                };
                wr(".mp2", dump_mp2);
                wr(".loas", dump_loas);
                wr(".au", dump_au);
            }
        }
    }

    // energy dispersal + convolutional coding of every logical frame
    void encode_msc() {
        const auto& pr = prbs_cached();
        for (auto& s : E.svc) {
            s.coded.assign(s.lframes.size(), {});
            for (size_t r = 0; r < s.lframes.size(); r++) {
                const auto& by = s.lframes[r];
                if (static_cast<int>(by.size()) != s.kbps * 3) throw std::runtime_error("logical frame size");
                std::vector<uint8_t> a(by.size() * 8);
                for (size_t i = 0; i < a.size(); i++) a[i] = static_cast<uint8_t>(((by[i >> 3] >> (7 - (i & 7))) & 1) ^ pr[i]);
                s.coded[r] = conv_encode(a, s.profile);
                if (static_cast<int>(s.coded[r].size()) != 64 * s.cu) throw std::runtime_error("codeword length");
            }
        }
    }

    // CIF r of the loop: time interleaving (12, table 21) over the periodic
    // sequence of codewords, sub-channels at their start addresses, the
    // unused CUs PRBS padding (13)
    void cif_bits(int r, std::vector<uint8_t>& d) const {
        static const int delay[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        const auto& pr = prbs_cached();
        d.assign(pr.begin(), pr.end());
        const int N = ncif();
        for (const auto& s : E.svc) {
            const int M = 64 * s.cu, base = 64 * s.start;
            for (int i = 0; i < M; i++) {
                const int rr = ((r - delay[i & 15]) % N + N) % N;
                d[static_cast<size_t>(base + i)] = s.coded[static_cast<size_t>(rr)][static_cast<size_t>(i)];
            }
        }
    }

    void build_fic() {
        FicBuilder fb{&E, &tx};
        const auto& pr = prbs_cached();
        fic_bits.assign(static_cast<size_t>(frames), {});
        for (int m = 0; m < frames; m++) {
            const int cif = (E.cif0 + 4 * m) % 5000;
            const auto fibs = fb.fibs(m, cif);
            std::vector<uint8_t> bp;
            for (int c = 0; c < 4; c++) {
                std::vector<uint8_t> a(768);
                for (int i = 0; i < 768; i++) {
                    const auto& fib = fibs[static_cast<size_t>(3 * c + i / 256)];
                    const int k = i % 256;
                    a[static_cast<size_t>(i)] = static_cast<uint8_t>(((fib[static_cast<size_t>(k >> 3)] >> (7 - (k & 7))) & 1) ^ pr[static_cast<size_t>(i)]);
                }
                const auto cw = conv_encode(a, fic_profile);
                if (cw.size() != 2304) throw std::runtime_error("FIC codeword");
                bp.insert(bp.end(), cw.begin(), cw.end());
            }
            fic_bits[static_cast<size_t>(m)] = bp;
        }
    }

    bool tii_frame(int m) const { return ((E.cif0 + 4 * m) % 8) < 4; }

    // main signal of frame m (no TII): null, PRS, 3 FIC + 72 MSC symbols
    void frame_samples(int m, std::vector<cf>& out) const {
        out.assign(kFrame, cf(0, 0));
        std::vector<cd> z(kK + 1, cd(0, 0));
        for (int k = -768; k <= 768; k++)
            if (k) z[static_cast<size_t>(k + 768)] = std::polar(1.0, prs_phase(k));
        int pos = kNull;
        ofdm(z, kGuard, kSym, out.data() + pos);
        pos += kSym;
        const auto& F = freq_map();
        std::vector<uint8_t> cifs[4];
        for (int c = 0; c < 4; c++) cif_bits(4 * m + c, cifs[c]);
        const auto& fic = fic_bits[static_cast<size_t>(m)];
        const double r2 = std::sqrt(0.5);
        for (int l = 2; l <= kL; l++) {
            auto bit = [&](int n) -> int {
                if (l <= 4) return fic[static_cast<size_t>((l - 2) * 3072 + n)];
                const int64_t ip = static_cast<int64_t>(l - 5) * 3072 + n;
                return cifs[ip / kCifBits][static_cast<size_t>(ip % kCifBits)];
            };
            for (int n = 0; n < kK; n++) {
                const cd q((1 - 2 * bit(n)) * r2, (1 - 2 * bit(n + kK)) * r2);
                z[static_cast<size_t>(F[static_cast<size_t>(n)] + 768)] *= q;
            }
            ofdm(z, kGuard, kSym, out.data() + pos);
            pos += kSym;
        }
    }

    void build_tii() {
        for (auto& t : tx) {
            std::vector<cd> z(kK + 1, cd(0, 0));
            for (int k0 : tii_carriers(t.sub_id, t.main_id)) {
                z[static_cast<size_t>(k0 + 768)] = std::polar(1.0, prs_phase(k0));
                z[static_cast<size_t>(k0 + 1 + 768)] = std::polar(1.0, prs_phase(t.pair_k1 ? k0 + 1 : k0));
            }
            t.tii.assign(kNull, cf(0, 0));
            ofdm(z, kNull - kFft, kNull, t.tii.data());
        }
    }
};

// ------------------------------------------------------------ fast gaussian noise
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed) {
        uint64_t z = seed + 0x9E3779B97F4A7C15ull;
        for (auto& x : s) {
            z += 0x9E3779B97F4A7C15ull;
            uint64_t y = z;
            y = (y ^ (y >> 30)) * 0xBF58476D1CE4E5B9ull;
            y = (y ^ (y >> 27)) * 0x94D049BB133111EBull;
            x = y ^ (y >> 31);
        }
    }
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t next() {
        const uint64_t r = s[0] + s[3], t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return r;
    }
    double uni() { return (static_cast<double>(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    cd cgauss() {   // unit variance per component
        const double r = std::sqrt(-2.0 * std::log(uni())), a = 2 * kPi * uni();
        return {r * std::cos(a), r * std::sin(a)};
    }
};

double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 50; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

// ------------------------------------------------------------ self test
int selftest() {
    int bad = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
        if (!ok) bad++;
    };
    const auto p = prbs(16);
    const int t12[16] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0};
    check(std::equal(p.begin(), p.end(), t12), "PRBS = EN 300 401 table 12");
    const auto& F = freq_map();
    std::vector<int> sorted(F.begin(), F.end());
    std::sort(sorted.begin(), sorted.end());
    bool perm = sorted.size() == 1536;
    for (int i = 0, k = -768; perm && i < 1536; i++, k++) {
        if (k == 0) k++;
        perm = sorted[static_cast<size_t>(i)] == k;
    }
    check(perm, "frequency interleaver: 1536 distinct carriers");
    // table 25 (Pi(13) printed 1 076 there, but dn = 1 067 and k = 43)
    check(F[0] == -513 && F[1] == -14 && F[2] == 329 && F[3] == 692 && F[4] == -733 && F[5] == 13 && F[6] == 680 &&
              F[7] == 273 && F[8] == -36 && F[9] == 43 && F[10] == 85 && F[11] == -432 && F[12] == -318 &&
              F[1533] == 652 && F[1534] == 606 && F[1535] == 197,
          "frequency interleaver = table 25 samples");
    const auto tc = tii_carriers(1, 11);
    const int fig64[] = {-670, -622, -526, -478, -286, -238, -142, -94, 99, 147, 243, 291, 483, 531, 627, 675};
    check(tc.size() == 16 && std::equal(tc.begin(), tc.end(), fig64), "TII c=1 p=11 = figure 64");
    Profile fp{{{21, 16}, {3, 15}}, 0};
    check(conv_encode(std::vector<uint8_t>(768, 0), fp).size() == 2304, "FIC codeword 2304 bits");
    // convolutional code: a single 1 gives the generator taps 133, 171, 145, 133 (octal)
    {
        std::vector<uint8_t> a(32, 0);
        a[0] = 1;
        Profile all{{{1, 24}}, 0};
        const auto u = conv_encode(a, all);
        unsigned g[4] = {0, 0, 0, 0};
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 4; j++) g[j] = (g[j] << 1) | u[static_cast<size_t>(4 * i + j)];
        // u_i responds a_0 -> taps reversed: x_{0,i} = a_i + a_{i-2}...: bits for i = 0..6 = 1011011 = 0133
        check(g[0] == 0133 && g[1] == 0171 && g[2] == 0145 && g[3] == 0133, "mother code generators 133 171 145 133");
    }
    {
        Service s;
        for (const auto& u : kUep) {
            s.type = "dab";
            s.kbps = u.kbps;
            s.prot = std::to_string(u.level);
            setup_protection(s);
            std::vector<uint8_t> a(static_cast<size_t>(u.kbps * 24), 0);
            if (static_cast<int>(conv_encode(a, s.profile).size()) != 64 * u.cu) bad++;
        }
        int eep_ok = 1;
        for (int lvl = 1; lvl <= 4; lvl++)
            for (int n = 1; n <= 24; n++)
                for (char o : {'A', 'B'}) {
                    s.type = "dabplus";
                    s.kbps = o == 'A' ? 8 * n : 32 * n;
                    if (s.kbps > 384) continue;
                    s.prot = std::to_string(lvl) + o;
                    setup_protection(s);
                    std::vector<uint8_t> a(static_cast<size_t>(s.kbps * 24), 0);
                    if (static_cast<int>(conv_encode(a, s.profile).size()) != 64 * s.cu) eep_ok = 0;
                }
        check(eep_ok, "every UEP / EEP profile fills its sub-channel exactly");
    }
    {
        RsEncoder rs;
        uint8_t c[120];
        for (int i = 0; i < 110; i++) c[i] = static_cast<uint8_t>(i * 37 + 11);
        rs.parity(c, c + 110);
        bool z = true;
        for (int j = 0; j < 10; j++) z = z && rs.syndrome(c, j) == 0;
        check(z, "RS(120,110): codeword has roots alpha^0..alpha^9");
    }
    {
        // MDCT (forward as in AacEncoder) -> standard IMDCT (2/N, n0 = (N/2+1)/2), sine window, OLA
        const int Nh = 960, W = 1920;
        std::vector<double> x(3 * Nh);
        for (int n = 0; n < 3 * Nh; n++) x[static_cast<size_t>(n)] = std::cos(0.0123 * n * n / 100.0) + 0.3 * std::sin(0.7 * n);
        std::vector<double> y(3 * Nh, 0.0);
        for (int fr = 0; fr < 2; fr++) {
            std::vector<double> X(Nh, 0.0);
            for (int k = 0; k < Nh; k++)
                for (int n = 0; n < W; n++)
                    X[static_cast<size_t>(k)] += 2 * std::sin(kPi / W * (n + 0.5)) * x[static_cast<size_t>(fr * Nh + n)] *
                                                 std::cos(2 * kPi / W * (n + (W / 2.0 + 1) / 2.0) * (k + 0.5));
            for (int n = 0; n < W; n++) {
                double s = 0;
                for (int k = 0; k < Nh; k++) s += X[static_cast<size_t>(k)] * std::cos(2 * kPi / W * (n + (W / 2.0 + 1) / 2.0) * (k + 0.5));
                y[static_cast<size_t>(fr * Nh + n)] += std::sin(kPi / W * (n + 0.5)) * 2.0 / W * s;
            }
        }
        double e = 0;
        for (int n = Nh; n < 2 * Nh; n++) e = std::max(e, std::fabs(y[static_cast<size_t>(n)] - x[static_cast<size_t>(n)]));
        check(e < 1e-9, "MDCT -> ISO IMDCT 960 reconstructs (TDAC, scale)");
    }
    {
        // Layer II CRC-16 G = x16+x15+x2+1 / ScF CRC-8 / FIB CRC: known vectors
        const uint8_t v[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        check(crc_bytes(v, 9, 0x1021, 16, 0xFFFF) == 0x29B1, "CRC-16 x16+x12+x5+1 init FFFF ('123456789' = 29B1)");
        check(crc_bytes(v, 9, 0x8005, 16, 0xFFFF) == 0xAEE7, "CRC-16 x16+x15+x2+1 init FFFF ('123456789' = AEE7)");
    }
    std::printf("%s\n", bad ? "SELFTEST FAILED" : "selftest passed");
    return bad ? 1 : 0;
}

// ------------------------------------------------------------ scene
struct Scene {
    double rf = 227.360e6, seconds = 4.8, rate = 2.4e6, radius = 0.12, snr = 20, cfo = 0, sco = 0, offset_khz = 0;
    int channels = 5, frames = 0;
    uint64_t seed = 1;
    double lat = 0, lon = 0;
    std::string format = "cu8";
};

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void default_scene(dabjson::Value& v) {
    v = dabjson::parse(R"({
 "rf_hz": 227360000, "seconds": 4.8, "snr_db": 20, "seed": 1,
 "ensemble": {"eid": "10BC", "ecc": "E0", "label": "Kraken DAB", "short": "Kraken", "ext_label": "Kräken DAB Tëst",
              "lto_half_hours": 2, "utc": "2026-11-16T12:00:00"},
 "services": [
  {"sid": "1001", "label": "Kraken Classic", "short": "Classic", "type": "dab", "kbps": 128, "protection": "3",
   "pty": 14, "lang": 9, "tone_hz": 1125, "dls": "Kraken Classic: MPEG Layer II, 1125 Hz test tone"},
  {"sid": "1002", "label": "Kraken Plus", "short": "Plus", "type": "dabplus", "kbps": 64, "protection": "3A",
   "pty": 10, "lang": 9, "tone_hz": 1750, "dls": "Kraken Plus: DAB+ AAC, 1750 Hz test tone"}
 ],
 "transmitters": [{"name": "TX1", "main": 12, "sub": 3, "bearing": 40, "delay_us": 0, "level_db": 0, "range_km": 20}]
})");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string scene_path, out_path, describe_path, dump_dir;
        std::map<std::string, std::string> ov;
        for (int i = 1; i < argc; i++) {
            const std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--selftest") return selftest();
            if (a == "--scene") scene_path = val();
            else if (a == "--out") out_path = val();
            else if (a == "--describe") describe_path = val();
            else if (a == "--dump") dump_dir = val();
            else if (a == "--seconds" || a == "--frames" || a == "--rate" || a == "--channels" || a == "--radius" ||
                     a == "--rf" || a == "--snr" || a == "--seed" || a == "--cfo" || a == "--sco" || a == "--offset-khz" ||
                     a == "--station" || a == "--format")
                ov[a.substr(2)] = val();
            else if (a == "-h" || a == "--help") {
                std::printf("dab_gen --scene S.json --out OUT.cu8|OUT.cf32 [--describe D.json] [--dump DIR]\n"
                            "        [--seconds S|--frames N] [--rate HZ] [--channels M] [--radius M] [--rf HZ]\n"
                            "        [--snr DB] [--seed N] [--cfo HZ] [--sco PPM] [--offset-khz K] [--station LAT,LON]\n"
                            "        [--format cu8|cf32]      dab_gen --selftest\n");
                return 0;
            } else
                throw std::runtime_error("unknown option " + a);
        }
        dabjson::Value sc;
        if (!scene_path.empty()) sc = dabjson::parse(read_file(scene_path));
        else default_scene(sc);

        Scene S;
        S.rf = sc.num("rf_hz", S.rf);
        S.seconds = sc.num("seconds", S.seconds);
        S.frames = static_cast<int>(sc.num("frames", 0));
        S.rate = sc.num("rate", S.rate);
        S.channels = static_cast<int>(sc.num("channels", S.channels));
        S.radius = sc.num("radius_m", S.radius);
        S.snr = sc.num("snr_db", S.snr);
        S.seed = static_cast<uint64_t>(sc.num("seed", 1));
        S.cfo = sc.num("cfo_hz", 0);
        S.sco = sc.num("sco_ppm", 0);
        S.offset_khz = sc.num("offset_khz", 0);
        S.format = sc.str("format", "");
        if (const auto* st = sc.get("station"); st && st->kind == dabjson::Value::Arr && st->a.size() >= 2) {
            S.lat = st->a[0].n;
            S.lon = st->a[1].n;
        }
        auto ovd = [&](const char* k, double& v) {
            if (ov.count(k)) v = std::stod(ov[k]);
        };
        ovd("seconds", S.seconds);
        ovd("rate", S.rate);
        ovd("radius", S.radius);
        ovd("rf", S.rf);
        ovd("snr", S.snr);
        ovd("cfo", S.cfo);
        ovd("sco", S.sco);
        ovd("offset-khz", S.offset_khz);
        if (ov.count("frames")) S.frames = std::stoi(ov["frames"]);
        if (ov.count("seconds") && !ov.count("frames")) S.frames = 0;
        if (ov.count("channels")) S.channels = std::stoi(ov["channels"]);
        if (ov.count("seed")) S.seed = std::stoull(ov["seed"]);
        if (ov.count("format")) S.format = ov["format"];
        if (ov.count("station")) {
            const std::string s = ov["station"];
            const size_t c = s.find(',');
            if (c == std::string::npos) throw std::runtime_error("--station LAT,LON");
            S.lat = std::stod(s.substr(0, c));
            S.lon = std::stod(s.substr(c + 1));
        }
        if (S.format.empty()) S.format = (out_path.size() > 5 && out_path.substr(out_path.size() - 5) == ".cf32") ? "cf32" : "cu8";
        if (S.channels < 1 || S.channels > 16) throw std::runtime_error("channels 1..16");
        if (S.rate < 1.6e6 || S.rate > 20e6) throw std::runtime_error("rate 1.6 .. 20 MHz");
        if (std::fabs(S.offset_khz * 1e3) + 768e3 > 0.48 * S.rate) throw std::runtime_error("offset puts the block outside the output band");
        int frames = S.frames > 0 ? S.frames : static_cast<int>(std::lround(S.seconds / 0.096));
        frames = std::max(kLoopFrames, (frames + kLoopFrames / 2) / kLoopFrames * kLoopFrames);

        Generator G;
        G.frames = frames;
        G.dump_dir = dump_dir;
        Ensemble& E = G.E;
        const dabjson::Value* ens = sc.get("ensemble");
        dabjson::Value empty;
        empty.kind = dabjson::Value::Obj;
        if (!ens) ens = &empty;
        E.eid = parse_hex(ens->str("eid", "10BC"));
        E.ecc = parse_hex(ens->str("ecc", "E0"));
        E.label = ens->str("label", "Kraken DAB").substr(0, 16);
        E.short_label = ens->str("short", "Kraken");
        E.ext_label = ens->str("ext_label", "");
        E.lto = static_cast<int>(ens->num("lto_half_hours", 0));
        E.inter_table = static_cast<int>(ens->num("inter_table", 1));
        E.cif0 = (static_cast<int>(ens->num("cif0", 0)) / 40 * 40) % 5000;
        {
            const std::string u = ens->str("utc", "2026-11-16T12:00:00");
            int y = 2026, mo = 11, d = 16, h = 12, mi = 0, se = 0;
            std::sscanf(u.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
            E.utc_ms = (static_cast<int64_t>(mjd(y, mo, d) - 40587) * 86400 + h * 3600 + mi * 60 + se) * 1000;
        }
        const dabjson::Value* svcs = sc.get("services");
        if (!svcs || svcs->kind != dabjson::Value::Arr || svcs->a.empty()) throw std::runtime_error("scene: no services");
        int next_cu = 0, next_id = 0;
        for (const auto& v : svcs->a) {
            Service s;
            s.sid = parse_hex(v.str("sid", hex(0x1001 + static_cast<unsigned>(E.svc.size()), 4)));
            s.label = v.str("label", "Service " + std::to_string(E.svc.size() + 1)).substr(0, 16);
            s.short_label = v.str("short", "");
            s.ext_label = v.str("ext_label", "");
            s.type = v.str("type", "dab");
            if (s.type != "dab" && s.type != "dabplus") throw std::runtime_error("service type dab or dabplus");
            s.kbps = static_cast<int>(v.num("kbps", s.type == "dab" ? 128 : 64));
            s.prot = v.str("protection", s.type == "dab" ? "3" : "3A");
            s.rate_hz = static_cast<int>(v.num("rate_hz", 48000));
            if (s.type == "dabplus") s.rate_hz = 48000;
            s.pty = static_cast<int>(v.num("pty", 0));
            s.lang = static_cast<int>(v.num("lang", 9));
            s.tone_req = v.num("tone_hz", 1000);
            s.dls = v.str("dls", s.label);
            s.subchid = static_cast<int>(v.num("subchid", next_id));
            next_id = std::max(next_id, s.subchid) + 1;
            setup_protection(s);
            s.start = static_cast<int>(v.num("start_cu", next_cu));
            next_cu = s.start + s.cu;
            if (next_cu > 864) throw std::runtime_error("sub-channels exceed the 864 CUs of the CIF");
            E.svc.push_back(s);
        }
        const dabjson::Value* txs = sc.get("transmitters");
        if (!txs || txs->kind != dabjson::Value::Arr || txs->a.empty()) throw std::runtime_error("scene: no transmitters");
        for (const auto& v : txs->a) {
            Transmitter t;
            t.name = v.str("name", "TX" + std::to_string(G.tx.size() + 1));
            t.main_id = static_cast<int>(v.num("main", 1));
            t.sub_id = static_cast<int>(v.num("sub", 1));
            if (t.main_id < 0 || t.main_id > 69 || t.sub_id < 1 || t.sub_id > 23) throw std::runtime_error("TII MainId 0..69, SubId 1..23 (terrestrial; V1.4.1 8.1.9)");
            t.bearing = v.num("bearing", 0);
            t.delay_us = v.num("delay_us", 0);
            t.level_db = v.num("level_db", 0);
            t.phase_deg = v.num("phase_deg", std::fmod(37.0 * static_cast<double>(G.tx.size() + 1), 360.0));
            t.range_km = v.num("range_km", 20);
            const std::string pp = v.str("tii_pair_phase", "k");
            if (pp != "k" && pp != "k+1") throw std::runtime_error("tii_pair_phase \"k\" (the standard) or \"k+1\"");
            t.pair_k1 = pp == "k+1";
            if (t.delay_us < 0 || t.delay_us > 246) throw std::runtime_error("delay_us 0..246 (the guard interval)");
            std::tie(t.lat, t.lon) = destination(S.lat, S.lon, std::fmod(360.0 - t.bearing + 720.0, 360.0), t.range_km);
            G.tx.push_back(t);
        }

        const auto t0 = std::chrono::steady_clock::now();
        G.build_audio();
        G.encode_msc();
        G.build_fic();
        G.build_tii();

        // ---- describe (stderr + optional JSON)
        std::ostringstream js;
        js << "{\n \"rate\": " << S.rate << ", \"channels\": " << S.channels << ", \"rf_hz\": " << std::llround(S.rf - S.offset_khz * 1e3)
           << ", \"block_hz\": " << std::llround(S.rf) << ", \"radius_m\": " << S.radius << ", \"frames\": " << frames
           << ", \"seconds\": " << frames * 0.096 << ", \"snr_db\": " << S.snr << ", \"cfo_hz\": " << S.cfo
           << ", \"sco_ppm\": " << S.sco << ", \"offset_khz\": " << S.offset_khz << ", \"cif0\": " << E.cif0
           << ",\n \"station\": [" << S.lat << ", " << S.lon << "],\n"
           << " \"ensemble\": {\"eid\": \"" << hex(E.eid, 4) << "\", \"ecc\": \"" << hex(E.ecc, 2) << "\", \"label\": " << json_str(E.label)
           << ", \"short\": " << json_str(E.short_label) << ", \"ext_label\": " << json_str(E.ext_label) << ", \"lto_half_hours\": " << E.lto
           << "},\n \"services\": [";
        std::fprintf(stderr, "Ensemble %s ECC %s \"%s\": %d frames (%.2f s), CIF count from %d, %zu services, %zu transmitters\n",
                     hex(E.eid, 4).c_str(), hex(E.ecc, 2).c_str(), E.label.c_str(), frames, frames * 0.096, E.cif0, E.svc.size(), G.tx.size());
        for (size_t i = 0; i < E.svc.size(); i++) {
            const auto& s = E.svc[i];
            std::fprintf(stderr, "  SId %s \"%-16s\" %-7s %3d kbit/s %5d Hz  SubCh %2d  CU %3d+%3d  %-28s tone %.1f Hz  PTy %d lang %d\n",
                         hex(s.sid, 4).c_str(), s.label.c_str(), s.type.c_str(), s.kbps, s.rate_hz, s.subchid, s.start, s.cu,
                         s.prot_desc.c_str(), s.tone, s.pty, s.lang);
            js << (i ? "," : "") << "\n  {\"sid\": \"" << hex(s.sid, 4) << "\", \"label\": " << json_str(s.label) << ", \"short\": "
               << json_str(s.short_label) << ", \"type\": \"" << s.type << "\", \"kbps\": " << s.kbps << ", \"sample_rate\": " << s.rate_hz
               << ", \"subchid\": " << s.subchid << ", \"start_cu\": " << s.start << ", \"size_cu\": " << s.cu << ", \"protection\": "
               << json_str(s.prot_desc) << ", \"uep_index\": " << (s.uep ? s.uep_index : -1) << ", \"ascty\": " << s.ascty()
               << ", \"pty\": " << s.pty << ", \"lang\": " << s.lang << ", \"tone_hz\": " << s.tone << ", \"dls\": " << json_str(s.dls) << "}";
        }
        js << "\n ],\n \"truth\": [";
        for (size_t i = 0; i < G.tx.size(); i++) {
            const auto& t = G.tx[i];
            const std::string tid = hex(E.eid, 4) + "-" + (t.main_id < 10 ? "0" : "") + std::to_string(t.main_id) + "-" +
                                    (t.sub_id < 10 ? "0" : "") + std::to_string(t.sub_id);
            std::fprintf(stderr, "  %-6s TII %s (MainId %2d SubId %2d)  bearing %6.1f  delay %7.3f us  level %+5.1f dB  phase %5.1f  %.5f %.5f%s\n",
                         t.name.c_str(), tid.c_str(), t.main_id, t.sub_id, t.bearing, t.delay_us, t.level_db, t.phase_deg, t.lat, t.lon,
                         t.pair_k1 ? "  pairs phi k/k+1" : "");
            js << (i ? "," : "") << "\n  {\"name\": " << json_str(t.name) << ", \"tid\": \"" << tid << "\", \"main\": " << t.main_id
               << ", \"sub\": " << t.sub_id << ", \"bearing\": " << t.bearing << ", \"delay_us\": " << t.delay_us << ", \"level_db\": "
               << t.level_db << ", \"phase_deg\": " << t.phase_deg << ", \"tii_pair_phase\": \"" << (t.pair_k1 ? "k+1" : "k")
               << "\", \"lat\": " << t.lat << ", \"lon\": " << t.lon << ", \"tii_carriers\": [";
            const auto tc = tii_carriers(t.sub_id, t.main_id);
            for (size_t k = 0; k < tc.size(); k++) js << (k ? ", " : "") << tc[k];
            js << "]}";
        }
        js << "\n ]\n}\n";
        if (!describe_path.empty()) {
            std::ofstream f(describe_path, std::ios::binary);
            f << js.str();
        }
        if (out_path.empty()) return 0;

        // ---- signal: per transmitter delay (fractional), resampling to S.rate, steering, noise
        const int M = S.channels;
        const double fo = S.rate, fi = kFs;
        const int64_t L = static_cast<int64_t>(frames) * kFrame;
        // with a sample clock offset the file still holds exactly one loop of the
        // ensemble (a seam error below one sample): length x (1 + ppm)
        const int64_t nout = static_cast<int64_t>(std::llround(static_cast<double>(L) * fo * (1 + S.sco * 1e-6) / fi));
        const double lambda = 299792458.0 / S.rf;
        const int K = static_cast<int>(G.tx.size());
        std::vector<std::vector<cd>> A(static_cast<size_t>(K), std::vector<cd>(static_cast<size_t>(M)));
        double ptx = 0;
        for (int k = 0; k < K; k++) {
            const auto& t = G.tx[static_cast<size_t>(k)];
            const cd g = std::polar(std::pow(10.0, t.level_db / 20), t.phase_deg * kPi / 180);
            ptx += std::pow(10.0, t.level_db / 10);
            const double b = t.bearing * kPi / 180;
            for (int a = 0; a < M; a++) {
                const double phi = -2 * kPi * a / M;
                const double ph = M == 1 ? 0.0 : 2 * kPi * S.radius / lambda * std::cos(b - phi);
                A[static_cast<size_t>(k)][static_cast<size_t>(a)] = g * std::polar(1.0, ph);
            }
        }
        // signal power of the main signal (null symbol included), from frame 0
        std::vector<cf> fr0;
        G.frame_samples(0, fr0);
        double p0 = 0;
        for (const auto& v : fr0) p0 += std::norm(v);
        p0 /= static_cast<double>(fr0.size());
        const double sigma2 = p0 * std::pow(10.0, -S.snr / 10) * fo / 1.536e6;   // noise in the full band; S/N in 1.536 MHz
        const double sigma = std::sqrt(sigma2 / 2);
        const double scale = 0.2 / std::sqrt(p0 * ptx + sigma2);                  // composite rms 0.2 of full scale
        // interpolation filter: Kaiser windowed sinc, 48 taps, beta 9, 1024 phases
        const int T = 48, P = 1024;
        const double fc = 0.5 * std::min(1.0, fo / fi), beta = 9.0;
        std::vector<float> H(static_cast<size_t>((P + 1) * T));
        for (int ph = 0; ph <= P; ph++)
            for (int j = 0; j < T; j++) {
                const double x = (j - T / 2 + 1) - static_cast<double>(ph) / P;
                const double sinc = std::fabs(x) < 1e-12 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
                const double r = x / (T / 2.0);
                const double w = std::fabs(r) <= 1 ? bessel_i0(beta * std::sqrt(1 - r * r)) / bessel_i0(beta) : 0;
                H[static_cast<size_t>(ph * T + j)] = static_cast<float>(sinc * w);
            }
        // frame cache
        std::map<int, std::vector<cf>> cache;
        auto frame = [&](int m) -> const std::vector<cf>& {
            auto it = cache.find(m);
            if (it != cache.end()) return it->second;
            if (cache.size() >= 4) cache.erase(cache.begin());
            std::vector<cf>& v = cache[m];
            G.frame_samples(m, v);
            return v;
        };
        std::FILE* fo_ = std::fopen(out_path.c_str(), "wb");
        if (!fo_) throw std::runtime_error("cannot write " + out_path);
        Rng rng(S.seed);
        const double eff = fo * (1 + S.sco * 1e-6);
        const double shift = (S.offset_khz * 1e3 + S.cfo) / fo;
        const int64_t CH = 24000;
        std::vector<std::vector<cf>> buf(static_cast<size_t>(K));
        std::vector<int64_t> b0(static_cast<size_t>(K));
        std::vector<uint8_t> o8;
        std::vector<float> of;
        std::vector<cd> acc(static_cast<size_t>(M));
        int last_pct = -1;
        for (int64_t c0 = 0; c0 < nout; c0 += CH) {
            const int64_t c1 = std::min(nout, c0 + CH);
            // input windows per transmitter
            for (int k = 0; k < K; k++) {
                const auto& t = G.tx[static_cast<size_t>(k)];
                const double d = t.delay_us * 1e-6 * fi;
                const int64_t lo = static_cast<int64_t>(std::floor(static_cast<double>(c0) / eff * fi - d)) - T;
                const int64_t hi = static_cast<int64_t>(std::floor(static_cast<double>(c1) / eff * fi - d)) + T + 2;
                auto& bb = buf[static_cast<size_t>(k)];
                bb.resize(static_cast<size_t>(hi - lo));
                b0[static_cast<size_t>(k)] = lo;
                for (int64_t i = lo; i < hi; i++) {
                    const int64_t w = ((i % L) + L) % L;
                    const int m = static_cast<int>(w / kFrame), o = static_cast<int>(w % kFrame);
                    cf v = frame(m)[static_cast<size_t>(o)];
                    if (o < kNull && G.tii_frame(m)) v += t.tii[static_cast<size_t>(o)];
                    bb[static_cast<size_t>(i - lo)] = v;
                }
            }
            o8.clear();
            of.clear();
            for (int64_t n = c0; n < c1; n++) {
                std::fill(acc.begin(), acc.end(), cd(0, 0));
                for (int k = 0; k < K; k++) {
                    const double x = static_cast<double>(n) / eff * fi - G.tx[static_cast<size_t>(k)].delay_us * 1e-6 * fi;
                    const int64_t i0 = static_cast<int64_t>(std::floor(x));
                    const double pf = (x - static_cast<double>(i0)) * P;
                    const int p0i = std::min(P - 1, static_cast<int>(pf));
                    const float a = static_cast<float>(pf - p0i);
                    const float* h0 = H.data() + p0i * T;
                    const float* h1 = h0 + T;
                    const cf* s = buf[static_cast<size_t>(k)].data() + (i0 - T / 2 + 1 - b0[static_cast<size_t>(k)]);
                    float re = 0, im = 0;
                    for (int j = 0; j < T; j++) {
                        const float h = h0[j] + a * (h1[j] - h0[j]);
                        re += s[j].real() * h;
                        im += s[j].imag() * h;
                    }
                    const cd v(re, im);
                    for (int m = 0; m < M; m++) acc[static_cast<size_t>(m)] += A[static_cast<size_t>(k)][static_cast<size_t>(m)] * v;
                }
                const cd rot = std::polar(1.0, 2 * kPi * std::fmod(shift * static_cast<double>(n), 1.0));
                for (int m = 0; m < M; m++) {
                    const cd y = (acc[static_cast<size_t>(m)] * rot + rng.cgauss() * sigma) * scale;
                    if (S.format == "cf32") {
                        of.push_back(static_cast<float>(y.real()));
                        of.push_back(static_cast<float>(y.imag()));
                    } else {
                        o8.push_back(static_cast<uint8_t>(std::clamp(std::lround(127.5 + 127.5 * y.real()), 0L, 255L)));
                        o8.push_back(static_cast<uint8_t>(std::clamp(std::lround(127.5 + 127.5 * y.imag()), 0L, 255L)));
                    }
                }
            }
            if (S.format == "cf32") std::fwrite(of.data(), sizeof(float), of.size(), fo_);
            else std::fwrite(o8.data(), 1, o8.size(), fo_);
            const int pct = static_cast<int>(100 * c1 / nout);
            if (pct / 10 != last_pct / 10) {
                std::fprintf(stderr, "  %3d %%\r", pct);
                last_pct = pct;
            }
        }
        std::fclose(fo_);
        // sidecar
        std::string side = out_path;
        const size_t dot = side.rfind('.');
        side = (dot == std::string::npos ? side : side.substr(0, dot)) + ".json";
        {
            std::ofstream f(side, std::ios::binary);
            std::string j = js.str();
            const double lsb = sigma * scale * 127.5;
            j.insert(2, "\"generator\": \"dab_gen\", \"format\": \"" + S.format + "\", \"samples\": " + std::to_string(nout) +
                            ", \"noise_lsb\": " + std::to_string(lsb) + ",\n ");
            f << j;
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "%s: %lld samples x %d antennas at %.0f Hz (%.2f s, %s), block at %+.1f kHz, CFO %+.1f Hz, SCO %+.1f ppm, "
                             "S/N %.1f dB; %.1f s\n",
                     out_path.c_str(), static_cast<long long>(nout), M, fo, static_cast<double>(nout) / fo, S.format.c_str(),
                     S.offset_khz, S.cfo, S.sco, S.snr, secs);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dab_gen: %s\n", e.what());
        return 2;
    }
}
