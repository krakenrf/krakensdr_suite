// P25 Phase 2 two-slot TDMA MAC layer (TIA-102.BBAC-A): one timeslot at a
// time from the physical-layer receiver (p25p2_slot.hpp).
//
//  - outbound (base station, continuous): the ISCH that describes the slot
//    (S-ISCH, or the I-ISCH (40,9,16) coset code by nearest codeword) ->
//    superframe slot, ultraframe count, LCH type; the superframe position is
//    kept from the running slot index between good ISCHs
//  - inbound (a subscriber's bursts): no ISCH - the superframe position comes
//    from which scrambling window makes a burst pass RS + CRC, the SACCH DUID
//    and the burst cadence
//  - DUID (8,4,4) by nearest codeword; burst bit maps; descrambling (PN44,
//    seed WACN / system ID / NAC: the options, a control channel's
//    NET_STS_BCST, or recovered blindly from one clean scrambled FACCH /
//    SACCH burst by linear algebra over GF(2)); Reed-Solomon (63,35) derived
//    codes with the punctured parity as erasures; CRC-12 / CRC-16; only then
//    the MAC PDU and its messages
//  - voice: the 4V / 2V cycle, AMBE+2 frames in the DMR order through the
//    user's mbelib (AmbeStream), ESS (RS (44,16)) decoded progressively with
//    missing ESS-B segments as erasures, encrypted calls muted
//  - ID separation: outbound, a transmission per logical channel (talker
//    channel = LCH + 1) named by the source unit (on a base station's output
//    every radio has the site's bearing); inbound, every checked burst is a
//    PACKET of the radio that sent it, with exactly its samples
//
// Facts that the fact sheet marks INFERRED / SINGLE are kept in one named
// place each (search for "[INFERRED" / "[SINGLE").

#include "p25p2.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace dig {

namespace {

// --- timing -------------------------------------------------------------------
constexpr int64_t RATE = 6000 * P25P2_SPS;          // receiver samples per second (48 kHz)
constexpr int64_t GAP = RATE / 2;                   // bursts of a transmission stopped this long: over
constexpr int64_t CALL_TIMEOUT = 2 * RATE;          // no checked burst of a call for this long: it ended
constexpr int64_t PENDING_MAX = RATE * 6 / 5;       // unnamed inbound bursts kept this long (host keeps 1.5 s)
constexpr int64_t ACTIVE_FOR = 2 * RATE;
constexpr int64_t STATE_FOR = 10 * RATE;            // channel state facts kept this long after the last checked burst

// [SINGLE 8.2, Q4] inbound scrambler = the outbound seed advanced by this
// many LFSR steps. Used only as a hypothesis to test (the inbound scrambler
// state is otherwise solved blindly from the bursts).
constexpr int INBOUND_SEED_ADVANCE = 243;

constexpr int64_t floor_div(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
constexpr int64_t mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

int parity64(uint64_t v) { return popcount64(v) & 1; }

std::string hex(uint64_t v, int digits) {
    char b[32];
    snprintf(b, sizeof b, "%0*llX", digits, static_cast<unsigned long long>(v));
    return b;
}

std::string hex_bytes(const uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += hex(p[i], 2);
    return s;
}

// --- PN44 scrambler (8.1) ---------------------------------------------------------
// State = the next 44 output bits b_n .. b_n+43, b_n in bit 43. b_n+44 =
// b_n ^ b_n+10 ^ b_n+24 ^ b_n+29 ^ b_n+35 ^ b_n+40.
constexpr uint64_t MASK44 = (1ULL << 44) - 1;
constexpr uint64_t FWD_TAPS = (1ULL << 43) | (1ULL << 33) | (1ULL << 19) | (1ULL << 14) | (1ULL << 8) | (1ULL << 3);
constexpr uint64_t BACK_TAPS = (1ULL << 0) | (1ULL << 34) | (1ULL << 20) | (1ULL << 15) | (1ULL << 9) | (1ULL << 4);

uint64_t pn_step(uint64_t s) { return ((s << 1) | static_cast<uint64_t>(parity64(s & FWD_TAPS))) & MASK44; }
uint64_t pn_back(uint64_t s) { return (s >> 1) | (static_cast<uint64_t>(parity64(s & BACK_TAPS)) << 43); }
uint64_t pn_jump(uint64_t s, int64_t k) {
    for (; k > 0; k--) s = pn_step(s);
    for (; k < 0; k++) s = pn_back(s);
    return s;
}
void pn_window(uint64_t s, uint8_t* out360) {
    for (int j = 0; j < 360; j++) {
        out360[j] = static_cast<uint8_t>((s >> 43) & 1);
        s = pn_step(s);
    }
}
uint64_t make_seed(int wacn, int sysid, int nac) {
    uint64_t s = (static_cast<uint64_t>(wacn & 0xFFFFF) << 24) | (static_cast<uint64_t>(sysid & 0xFFF) << 12) |
                 static_cast<uint64_t>(nac & 0xFFF);
    return s ? s : MASK44;   // [SINGLE 8.2, Q6] the all-zero seed is replaced by all ones
}

// m[j]: PN bit j of a window as a GF(2) function of its 44-bit start state
const std::array<uint64_t, 360>& pn_masks() {
    static const std::array<uint64_t, 360> m = [] {
        std::array<uint64_t, 360> t{};
        for (int j = 0; j < 44; j++) t[j] = 1ULL << (43 - j);
        for (int j = 44; j < 360; j++) t[j] = t[j - 44] ^ t[j - 34] ^ t[j - 20] ^ t[j - 15] ^ t[j - 9] ^ t[j - 4];
        return t;
    }();
    return m;
}

// --- ISCH (4) ------------------------------------------------------------------
constexpr uint64_t S_ISCH = 0x575D57F7FFULL;
struct IschTable {
    uint64_t cw[512];
    IschTable() {
        static const uint64_t G[9] = {0x8816CE36D7ULL, 0x201DFD4F64ULL, 0x100F4B1758ULL, 0x0C00DED18EULL, 0x020807F7FFULL,
                                      0x09048D9B72ULL, 0x009DA3A171ULL, 0x0058CBAA4EULL, 0x00343D8597ULL};
        for (int m = 0; m < 512; m++) {
            uint64_t c = 0x184229D461ULL;
            for (int r = 0; r < 9; r++)
                if ((m >> (8 - r)) & 1) c ^= G[r];
            cw[m] = c;
        }
    }
};
const IschTable& isch_table() {
    static const IschTable t;
    return t;
}

// --- DUID (5) -------------------------------------------------------------------
uint8_t duid_cw(int info) {
    static const uint8_t G[4] = {0x8D, 0x4B, 0x2E, 0x17};
    uint8_t c = 0;
    for (int r = 0; r < 4; r++)
        if ((info >> (3 - r)) & 1) c ^= G[r];
    return c;
}
// nearest codeword: -1 when more than 1 bit away (2 = undecidable)
int duid_decode(uint8_t rx) {
    int best = -1, bd = 9;
    for (int i = 0; i < 16; i++) {
        int d = popcount64(static_cast<uint64_t>(rx ^ duid_cw(i)));
        if (d < bd) { bd = d; best = i; }
    }
    return bd <= 1 ? best : -1;
}
enum Duid { D_4V = 0x0, D_SACCH_S = 0x3, D_LCCH_S = 0x4, D_2V = 0x6, D_FACCH_S = 0x9, D_SACCH = 0xC, D_LCCH = 0xD, D_FACCH = 0xF };
bool duid_scrambled(int d) { return d == D_SACCH_S || d == D_LCCH_S || d == D_FACCH_S; }

// --- Golay(23,12) of the AMBE+2 frame's c0 (7.3): errors of the nearest codeword
struct Golay23 {
    uint8_t w[2048];
    static uint32_t synd(uint32_t v) {   // v (23 bits, bit 22 = x^22) mod g
        for (int i = 22; i >= 11; i--)
            if (v & (1u << i)) v ^= 0xC75u << (i - 11);
        return v & 0x7FF;
    }
    Golay23() {
        memset(w, 4, sizeof w);
        w[0] = 0;
        for (int a = 0; a < 23; a++) {
            uint32_t ea = 1u << a;
            w[synd(ea)] = std::min<uint8_t>(w[synd(ea)], 1);
            for (int b = a + 1; b < 23; b++) {
                uint32_t eb = ea | (1u << b);
                w[synd(eb)] = std::min<uint8_t>(w[synd(eb)], 2);
                for (int c = b + 1; c < 23; c++) {
                    uint32_t ec = eb | (1u << c);
                    w[synd(ec)] = std::min<uint8_t>(w[synd(ec)], 3);
                }
            }
        }
    }
    int errors(uint32_t v23) const { return w[synd(v23)]; }
};
const Golay23& golay23c() {
    static const Golay23 g;
    return g;
}

// --- burst layouts (6.1, 6.2) and their codes (7) --------------------------------------
enum Kind { K_OECI = 0, K_IOEMI, K_SOEMI, K_IEMI, K_IECI, K_N };
struct Layout {
    int K = 0, R = 0;            // info / transmitted parity hexbits (RS (K+28) shortened, last 28-R punctured)
    int pdu_bits = 0, crc_w = 0; // the CRC covers the first pdu_bits of the info field
    int ninfo = 0, npar = 0;     // bits
    int16_t info[210] = {}, par[168] = {};
};
void add_syms(int16_t* dst, int* n, int a, int b) {
    for (int s = a; s <= b; s++) {
        dst[(*n)++] = static_cast<int16_t>(2 * s);
        dst[(*n)++] = static_cast<int16_t>(2 * s + 1);
    }
}
const Layout& layout(int k) {
    static const std::array<Layout, K_N> t = [] {
        std::array<Layout, K_N> L{};
        auto mk = [](Layout& l, int K, int R, int pdu, int crc, std::initializer_list<std::pair<int, int>> inf,
                     std::initializer_list<std::pair<int, int>> par) {
            l.K = K;
            l.R = R;
            l.pdu_bits = pdu;
            l.crc_w = crc;
            for (auto& p : inf) add_syms(l.info, &l.ninfo, p.first, p.second);
            for (auto& p : par) add_syms(l.par, &l.npar, p.first, p.second);
        };
        mk(L[K_OECI], 30, 22, 164, 16, {{11, 46}, {48, 101}}, {{102, 131}, {133, 168}});
        mk(L[K_IOEMI], 30, 22, 168, 12, {{11, 46}, {48, 101}}, {{102, 131}, {133, 168}});
        mk(L[K_SOEMI], 26, 19, 144, 12, {{11, 46}, {48, 78}, {100, 110}}, {{111, 131}, {133, 168}});
        mk(L[K_IEMI], 26, 20, 144, 12, {{28, 45}, {47, 82}, {84, 107}}, {{108, 131}, {133, 168}});
        // [INFERRED 7.2, Q10] inbound LCCH: CRC-16 over the first 120 of 136 bits
        mk(L[K_IECI], 23, 23, 120, 16, {{28, 45}, {47, 82}, {84, 98}}, {{99, 131}, {133, 168}});
        return L;
    }();
    return t[static_cast<size_t>(k)];
}

// RS-decode the (descrambled) burst bits b[360] of a layout -> info bits.
// Returns the corrected hexbits, -1 = uncorrectable or the CRC failed.
// rel (optional): per-symbol reliability of soft symbols - when the plain
// decode fails, the least reliable hexbits are tried as erasures too
// (generalized minimum distance decoding, 4 more attempts at most).
int decode_once(const Layout& l, const uint8_t* sym_in, const int* extra, int nextra, uint8_t* info_bits) {
    uint8_t sym[63];
    const int n = l.K + 28;
    memcpy(sym, sym_in, static_cast<size_t>(n));
    int eras[28], ne = 0;
    for (int h = l.K + l.R; h < n; h++) eras[ne++] = h;
    for (int i = 0; i < nextra && ne < 28; i++) eras[ne++] = extra[i];
    int r = rs64_decode_erasures(sym, n, 28, eras, ne);
    if (r < 0) return -1;
    for (int h = 0; h < l.K; h++) u64_to_bits(sym[h], 6, info_bits + 6 * h);
    uint32_t crc = bits_to_u32(info_bits + l.pdu_bits, l.crc_w);
    uint32_t want = l.crc_w == 16 ? crc_bits(info_bits, l.pdu_bits, 16, 0x1021, 0, 0xFFFF)
                                  : crc_bits(info_bits, l.pdu_bits, 12, 0x897, 0, 0xFFF);
    if (crc != want) return -1;
    return r - (28 - l.R);   // punctured symbols are not corrections
}
int decode_block(const Layout& l, const uint8_t* b, uint8_t* info_bits, const float* rel = nullptr) {
    uint8_t sym[63] = {};
    for (int h = 0; h < l.K; h++) {
        int v = 0;
        for (int k = 0; k < 6; k++) v = (v << 1) | b[l.info[6 * h + k]];
        sym[h] = static_cast<uint8_t>(v);
    }
    for (int h = 0; h < l.R; h++) {
        int v = 0;
        for (int k = 0; k < 6; k++) v = (v << 1) | b[l.par[6 * h + k]];
        sym[l.K + h] = static_cast<uint8_t>(v);
    }
    int r = decode_once(l, sym, nullptr, 0, info_bits);
    if (r >= 0 || !rel) return r;
    // hexbit reliability = its least reliable symbol
    std::pair<float, int> hr[52];
    const int nt = l.K + l.R;
    for (int h = 0; h < nt; h++) {
        const int16_t* pos = h < l.K ? l.info + 6 * h : l.par + 6 * (h - l.K);
        float m = 1e9f;
        for (int k = 0; k < 6; k += 2) m = std::min(m, rel[pos[k] / 2]);
        hr[h] = {m, h};
    }
    std::sort(hr, hr + nt);
    int extra[12];
    for (int i = 0; i < 12; i++) extra[i] = hr[i].second;
    for (int m = 2; m <= 8 && (28 - l.R) + m <= 24; m += 2)
        if ((r = decode_once(l, sym, extra, m, info_bits)) >= 0) return r;
    return -1;
}

// unused-bit helper kept for the old call sites
// Blind scrambler-state solve (8.6): for r = c ^ pn(W), the binary RS
// syndrome (re-encoded parity ^ received parity) = L W over GF(2). L is
// reduced once per layout (Gauss-Jordan, row combinations kept); per burst
// W = 44 parities of the syndrome, and the surplus rows must all be 0.
struct Blind {
    int rank = 0, nsur = 0;
    uint64_t comb[44][3] = {};     // state bit c = parity(comb[c] & syndrome)
    uint64_t sur[168][3] = {};     // must be 0
    explicit Blind(const Layout& l) {
        const auto& m = pn_masks();
        const int nr = l.npar;
        // parity bits of the unit info vectors
        std::vector<std::array<uint64_t, 3>> E(static_cast<size_t>(l.ninfo));
        for (int i = 0; i < l.ninfo; i++) {
            uint8_t sym[63] = {};
            sym[i / 6] = static_cast<uint8_t>(1 << (5 - i % 6));
            rs64_encode(sym, l.K + 28, 28);
            std::array<uint64_t, 3> e{};
            for (int t = 0; t < nr; t++)
                if ((sym[l.K + t / 6] >> (5 - t % 6)) & 1) e[static_cast<size_t>(t / 64)] |= 1ULL << (t % 64);
            E[static_cast<size_t>(i)] = e;
        }
        std::vector<uint64_t> row(static_cast<size_t>(nr));
        std::vector<std::array<uint64_t, 3>> cb(static_cast<size_t>(nr));
        for (int t = 0; t < nr; t++) {
            uint64_t v = m[static_cast<size_t>(l.par[t])];
            for (int i = 0; i < l.ninfo; i++)
                if ((E[static_cast<size_t>(i)][static_cast<size_t>(t / 64)] >> (t % 64)) & 1) v ^= m[static_cast<size_t>(l.info[i])];
            row[static_cast<size_t>(t)] = v;
            cb[static_cast<size_t>(t)] = {};
            cb[static_cast<size_t>(t)][static_cast<size_t>(t / 64)] = 1ULL << (t % 64);
        }
        int piv_row[44];
        for (int c = 0; c < 44; c++) piv_row[c] = -1;
        int r0 = 0;
        for (int c = 0; c < 44 && r0 < nr; c++) {
            const uint64_t bit = 1ULL << c;
            int p = -1;
            for (int r = r0; r < nr; r++)
                if (row[static_cast<size_t>(r)] & bit) { p = r; break; }
            if (p < 0) continue;
            std::swap(row[static_cast<size_t>(p)], row[static_cast<size_t>(r0)]);
            std::swap(cb[static_cast<size_t>(p)], cb[static_cast<size_t>(r0)]);
            for (int r = 0; r < nr; r++)
                if (r != r0 && (row[static_cast<size_t>(r)] & bit)) {
                    row[static_cast<size_t>(r)] ^= row[static_cast<size_t>(r0)];
                    for (int k = 0; k < 3; k++) cb[static_cast<size_t>(r)][static_cast<size_t>(k)] ^= cb[static_cast<size_t>(r0)][static_cast<size_t>(k)];
                }
            piv_row[c] = r0++;
        }
        rank = r0;
        for (int c = 0; c < 44; c++)
            if (piv_row[c] >= 0)
                for (int k = 0; k < 3; k++) comb[c][k] = cb[static_cast<size_t>(piv_row[c])][static_cast<size_t>(k)];
        for (int r = r0; r < nr; r++, nsur++)
            for (int k = 0; k < 3; k++) sur[nsur][k] = cb[static_cast<size_t>(r)][static_cast<size_t>(k)];
        // what flipping one coded bit does to the reduced syndrome
        for (int i = 0; i < l.ninfo; i++) {
            const auto& e = E[static_cast<size_t>(i)];
            uint64_t v[3] = {e[0], e[1], e[2]};
            red_bit[l.info[i]] = reduce(v);
        }
        for (int t = 0; t < nr; t++) {
            uint64_t v[3] = {};
            v[t / 64] = 1ULL << (t % 64);
            red_bit[l.par[t]] = reduce(v);
        }
        for (int k = 0; k < 180; k++) {
            bool coded = false;
            for (int i = 0; i < l.ninfo; i++) coded |= l.info[i] / 2 == k;
            for (int t = 0; t < nr; t++) coded |= l.par[t] / 2 == k;
            if (coded) syms[nsyms++] = static_cast<int16_t>(k);
        }
    }
    struct Red { uint64_t w = 0, s0 = 0, s1 = 0; };
    Red red_bit[360];
    int16_t syms[180] = {};
    int nsyms = 0;
    Red reduce(const uint64_t* s) const {
        Red r;
        for (int k = 0; k < nsur; k++)
            if ((popcount64(sur[k][0] & s[0]) + popcount64(sur[k][1] & s[1]) + popcount64(sur[k][2] & s[2])) & 1)
                (k < 64 ? r.s0 : r.s1) |= 1ULL << (k % 64);
        for (int c = 0; c < 44; c++)
            if ((popcount64(comb[c][0] & s[0]) + popcount64(comb[c][1] & s[1]) + popcount64(comb[c][2] & s[2])) & 1) r.w |= 1ULL << c;
        return r;
    }
    // b = the received (scrambled) burst bits. true + *W when consistent.
    // Without errors the surplus equations hold at once. Otherwise symbol
    // changes are searched whose syndrome change makes them hold (70+
    // surplus bits: a false fit is out of the question): up to 2 changes of
    // any coded symbol to any other level; with soft symbols (rel / alt =
    // each symbol's reliability and second-best dibit) also 3-4 changes
    // among the 16 least reliable, each to its second-best level.
    bool solve(const Layout& l, const uint8_t* b, uint64_t* W, const float* rel = nullptr, const uint8_t* alt = nullptr,
               bool search = false) const {
        if (rank < 44) return false;
        uint8_t sym[63] = {};
        for (int h = 0; h < l.K; h++) {
            int v = 0;
            for (int k = 0; k < 6; k++) v = (v << 1) | b[l.info[6 * h + k]];
            sym[h] = static_cast<uint8_t>(v);
        }
        rs64_encode(sym, l.K + 28, 28);
        uint64_t s[3] = {};
        for (int t = 0; t < l.npar; t++) {
            int enc = (sym[l.K + t / 6] >> (5 - t % 6)) & 1;
            if (enc ^ b[l.par[t]]) s[t / 64] |= 1ULL << (t % 64);
        }
        const Red base = reduce(s);
        if (!base.s0 && !base.s1) {
            *W = base.w;
            return true;
        }
        if (!search && !rel) return false;
        // candidate symbol changes
        struct D { uint64_t s0, s1, w; int16_t sym; float rel; bool second; };
        D d[3 * 180];
        int nd = 0;
        for (int i = 0; i < nsyms; i++) {
            const int k = syms[i];
            const int cur = b[2 * k] << 1 | b[2 * k + 1];
            for (int a = 0; a < 4; a++) {
                if (a == cur) continue;
                D x{0, 0, 0, static_cast<int16_t>(k), rel ? rel[k] : 0.f, rel && a == alt[k]};
                if ((a ^ cur) & 2) { x.s0 ^= red_bit[2 * k].s0; x.s1 ^= red_bit[2 * k].s1; x.w ^= red_bit[2 * k].w; }
                if ((a ^ cur) & 1) { x.s0 ^= red_bit[2 * k + 1].s0; x.s1 ^= red_bit[2 * k + 1].s1; x.w ^= red_bit[2 * k + 1].w; }
                d[nd++] = x;
            }
        }
        for (int i = 0; i < nd; i++)
            if (d[i].s0 == base.s0 && d[i].s1 == base.s1) { *W = base.w ^ d[i].w; return true; }
        // two changes: sorted by syndrome, look up the complement of each
        std::sort(d, d + nd, [](const D& x, const D& y) { return x.s0 != y.s0 ? x.s0 < y.s0 : x.s1 < y.s1; });
        for (int i = 0; i < nd; i++) {
            const uint64_t t0 = base.s0 ^ d[i].s0, t1 = base.s1 ^ d[i].s1;
            int lo = 0, hi = nd;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (d[mid].s0 < t0 || (d[mid].s0 == t0 && d[mid].s1 < t1)) lo = mid + 1; else hi = mid;
            }
            for (int j = lo; j < nd && d[j].s0 == t0 && d[j].s1 == t1; j++)
                if (d[j].sym != d[i].sym) { *W = base.w ^ d[i].w ^ d[j].w; return true; }
        }
        if (!rel) return false;
        // three / four changes among the least reliable symbols (each to its second-best level)
        std::sort(d, d + nd, [](const D& x, const D& y) { return x.second != y.second ? x.second : x.rel < y.rel; });
        int T = 0;
        while (T < nd && T < 16 && d[T].second) T++;
        for (int a = 0; a < T; a++)
            for (int bb = a + 1; bb < T; bb++)
                for (int c = bb + 1; c < T; c++) {
                    const uint64_t s0 = base.s0 ^ d[a].s0 ^ d[bb].s0 ^ d[c].s0, s1 = base.s1 ^ d[a].s1 ^ d[bb].s1 ^ d[c].s1;
                    if (!s0 && !s1) { *W = base.w ^ d[a].w ^ d[bb].w ^ d[c].w; return true; }
                    for (int e = c + 1; e < T; e++)
                        if (s0 == d[e].s0 && s1 == d[e].s1) { *W = base.w ^ d[a].w ^ d[bb].w ^ d[c].w ^ d[e].w; return true; }
                }
        return false;
    }
};
const Blind& blind(int k) {
    static const Blind b0(layout(K_OECI)), b1(layout(K_IOEMI)), b2(layout(K_SOEMI)), b3(layout(K_IEMI)), b4(layout(K_IECI));
    static const Blind* t[K_N] = {&b0, &b1, &b2, &b3, &b4};
    return *t[k];
}

// --- MAC message lengths (9.3) ----------------------------------------------------
// octets including the opcode; 0 = unknown (stop), -1 = from its own length octet.
// [GPL-fact 9.3: the length table; 9 of its lengths CONFIRMED-RF]
struct MsgLen {
    int8_t len[256];
    MsgLen() {
        memset(len, 0, sizeof len);
        static const int t[][2] = {
            {0x01, 7}, {0x02, 8}, {0x03, 7}, {0x05, 16}, {0x08, -1}, {0x21, 14}, {0x22, 15}, {0x25, 15}, {0x30, 5},
            {0x31, 7}, {0x40, 9}, {0x41, 7}, {0x42, 9}, {0x44, 9}, {0x45, 10}, {0x46, 9}, {0x48, 10}, {0x49, 10},
            {0x4A, 7}, {0x4C, 10}, {0x52, 8}, {0x53, 9}, {0x54, 9}, {0x55, 7}, {0x58, 10}, {0x5A, 7}, {0x5C, 10},
            {0x5D, 8}, {0x5E, 14}, {0x5F, 7}, {0x60, 9}, {0x61, 9}, {0x64, 9}, {0x67, 9}, {0x68, 10}, {0x6A, 7},
            {0x6B, 10}, {0x6C, 10}, {0x6D, 7}, {0x6F, 9}, {0x70, 9}, {0x71, 18}, {0x72, 9}, {0x73, 9}, {0x74, 9},
            {0x75, 9}, {0x76, 10}, {0x77, 13}, {0x78, 9}, {0x79, 9}, {0x7A, 9}, {0x7B, 11}, {0x7C, 9}, {0x7D, 9},
            {0xC0, 11}, {0xC3, 8}, {0xC4, 15}, {0xC5, 14}, {0xC6, 15}, {0xC7, 18}, {0xC8, 12}, {0xC9, 12},
            {0xCB, 18}, {0xCC, 14}, {0xCD, 18}, {0xCE, 18}, {0xCF, 18}, {0xD6, 9}, {0xD8, 14}, {0xD9, 18},
            {0xDA, 11}, {0xDB, 18}, {0xDC, 14}, {0xDE, 18}, {0xDF, 11}, {0xE0, 18}, {0xE4, 17}, {0xE5, 14},
            {0xE8, 16}, {0xE9, 8}, {0xEA, 11}, {0xEC, 13}, {0xF2, 16}, {0xF3, 14}, {0xFA, 11}, {0xFB, 13},
            {0xFC, 11}, {0xFE, 15}};
        for (const auto& e : t) len[e[0]] = static_cast<int8_t>(e[1]);
        for (int op = 0x80; op <= 0xBF; op++) len[op] = -1;   // manufacturer: octet 3
    }
};
const MsgLen& msg_len() {
    static const MsgLen m;
    return m;
}
const char* msg_name(int op) {
    switch (op) {
        case 0x01: return "group voice channel user";
        case 0x02: return "unit-to-unit voice channel user";
        case 0x03: return "telephone interconnect voice channel user";
        case 0x05: return "group voice channel grant update (multiple)";
        case 0x10: return "multi-fragment continuation";
        case 0x11: return "indirect group paging";
        case 0x12: return "individual paging";
        case 0x21: return "group voice channel user (extended)";
        case 0x22: return "unit-to-unit voice channel user (extended)";
        case 0x25: return "group voice channel grant update (multiple, explicit)";
        case 0x30: return "power control signal quality";
        case 0x31: return "MAC release";
        case 0x79: return "secondary control channel broadcast";
        case 0xD6: return "SNDCP data channel announcement";
        default: return nullptr;
    }
}
// Phase 1 TSBK opcodes whose 8-octet bodies P25Proto::tsbk() parses (9.4:
// partition 01 = the Phase 1 body); the others are shown raw (verbose)
bool phase1_parses(int op) {
    switch (op) {
        case 0x00: case 0x02: case 0x04: case 0x20: case 0x27: case 0x2F: case 0x30: case 0x33: case 0x34:
        case 0x35: case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3D: return true;
        default: return false;
    }
}

// outbound / inbound slot -> LCH (2.1)
int out_lch(int s) { return s == 10 ? 1 : s == 11 ? 0 : (s & 1); }

}  // namespace

// ==================================================================================
struct P25p2Proto::Impl {
    RxContext& ctx;
    P25Proto& p1;
    explicit Impl(RxContext& c, P25Proto& p) : ctx(c), p1(p) { reset(); }

    // --- options
    int u_wacn = -1, u_sysid = -1, u_nac = -1;
    int lch_opt = 0;

    // --- the current slot
    const P25p2Slot* sl = nullptr;
    uint8_t bits[360] = {};
    uint8_t pn[360] = {};
    uint8_t work[360] = {};
    // outbound soft symbols: reliability (distance to the nearest decision
    // threshold) and the second most likely dibit; inbound: decisions only
    float rel[180] = {};
    uint8_t alt[180] = {};
    const float* relp = nullptr;
    int64_t now = 0;
    int64_t last_valid = -1;

    // --- outbound superframe position
    int phase = -1;            // slot index of superframe slot 0, mod 12
    int phase_conf = 0;
    int uf_ref = -1;           // ultraframe count of superframe number 0 ((index - phase) / 12)
    int lch_type[2] = {-1, -1};   // from the I-ISCH: 0 VCH, 1 DCH, 3 LCCH
    int lch_cc[2] = {-1, -1};     // outbound LCH carries: 1 control channel (OECI), 0 traffic, -1 not seen

    // --- scrambling seed
    enum { SRC_OPTION = 0, SRC_CC, SRC_PHASE1, SRC_BLIND, SRC_INBOUND, SRC_N };
    bool cand_ok[SRC_N] = {};
    uint64_t cand[SRC_N] = {};
    int cur = -1;              // the seed that worked last (index into cand)
    uint64_t win[12] = {};     // its 12 slot windows' start states
    int shown_src = -1;        // the identity facts last reported
    uint64_t shown_seed = 0;
    bool scrambled_seen = false;
    // scrambled bursts in a row the known seed / inbound reference failed on:
    // the blind solve runs only while there is none or it stopped working
    static constexpr int BLIND_AFTER = 4;
    int seed_fail = 0, in_fail = 0;

    // --- inbound scrambler / superframe position
    bool in_ref = false;
    uint64_t in_W = 0;         // window start state of the reference burst
    int64_t in_i = 0;          // its slot index
    uint16_t in_alive = 0xFFF; // superframe slot of the reference burst: candidates
    int in_mode = 0;           // window model (in_cands)
    int in_phase_cad = -1;     // inbound superframe position from the burst cadence (-1 unknown)
    std::string in_hyp_said;

    // --- logical channels: [0], [1] outbound LCH; [2], [3] inbound lanes (index parity of the radio's traffic bursts)
    struct Chan {
        bool inbound = false;
        int lch = -1;              // outbound: 0 / 1; inbound: from the phase (-1 unknown)
        // call
        bool call = false;
        int64_t call_last = -1;
        int64_t ended_at = -1;          // the last call's end (MAC_END_PTT ...) and who it was
        uint32_t ended_src = 0, ended_dst = 0;
        uint32_t src = 0, dst = 0;
        bool group = true, src_named = false, dst_known = false;
        int so = 0, alg = -1, kid = 0;  // alg -1 = not known
        bool so_known = false;          // so came from a voice channel user message
        std::string desc, state, state_key;
        // outbound talker (a transmission on channel lch + 1)
        int64_t t_start = -1, t_end = 0, lc_end = 0;
        std::string tid, tcall;
        bool treported = false;
        // inbound talker (packets)
        uint32_t unit = 0;
        int64_t last_burst = -1;
        struct Pend { int64_t a, b; float f; };
        Pend pend[48];
        int npend = 0;
        int64_t sacch_at = INT64_MIN, traffic_at = INT64_MIN;
        // voice cycle / ESS
        int64_t cyc_ref = INT64_MIN;   // traffic ordinal of the cycle's 2V position
        uint8_t ess[16] = {};
        int ess_have = 0;
        int64_t ess_cycle = INT64_MIN;
        int scr_vote = 0;              // voice: > 0 scrambled seen more often
        std::unique_ptr<AmbeStream> ambe;
        int64_t last_voice = INT64_MIN / 2;   // sample index of its last voice burst played
    };
    Chan ch[4];
    int voice_ch = -1;
    std::map<std::string, std::string> facts;

    // =========================================================================
    void reset() {
        phase = -1;
        phase_conf = 0;
        uf_ref = -1;
        lch_type[0] = lch_type[1] = -1;
        lch_cc[0] = lch_cc[1] = -1;
        for (int i = 0; i < SRC_N; i++) cand_ok[i] = false;
        cur = -1;
        seed_fail = 0;
        shown_src = -1;
        scrambled_seen = false;
        set_network(u_wacn, u_sysid, u_nac);
        in_reset();
        for (int i = 0; i < 4; i++) {
            ch[i] = Chan();
            ch[i].inbound = i >= 2;
            ch[i].lch = i < 2 ? i : -1;
        }
        voice_ch = -1;
        facts.clear();
        shown_mode = shown_scr = shown_type = -1;
        last_valid = -1;
        out_valid_end = -1;
        now = 0;
    }
    void in_reset() {
        in_ref = false;
        in_alive = 0xFFF;
        in_mode = 0;
        in_phase_cad = -1;
        in_fail = 0;
    }
    void grid_lost() {
        // slot indexes start over: everything tied to them
        phase = -1;
        phase_conf = 0;
        uf_ref = -1;
        in_reset();
        for (auto& c : ch) {
            c.cyc_ref = INT64_MIN;
            c.ess_have = 0;
            c.sacch_at = c.traffic_at = INT64_MIN;
        }
    }
    void set_network(int w, int s, int n) {
        u_wacn = w;
        u_sysid = s;
        u_nac = n;
        cand_ok[SRC_OPTION] = w >= 0 && s >= 0 && n >= 0;
        if (cand_ok[SRC_OPTION]) {
            cand[SRC_OPTION] = make_seed(w, s, n);
            if (cur < 0 || cur == SRC_OPTION) use_seed(SRC_OPTION);
        } else if (cur == SRC_OPTION) {
            cur = -1;
        }
    }

    void fact(const std::string& k, const std::string& v) {
        auto it = facts.find(k);
        if (it != facts.end() && it->second == v) return;
        facts[k] = v;
        if (v.empty()) ctx.report->erase(Mode::P25, k);
        else ctx.report->set(Mode::P25, k, v);
    }
    void event(const std::string& e, double dedup) { ctx.report->event(Mode::P25, e, dedup); }
    bool verbose() const { return ctx.opts && ctx.opts->verbose; }

    // --- seeds ---------------------------------------------------------------
    void use_seed(int src) {
        cur = src;
        uint64_t s = cand[src];
        for (int k = 0; k < 12; k++) {
            win[k] = s;
            s = pn_jump(s, 360);
        }
    }
    void show_identity() {
        if (cur < 0) return;
        uint64_t s = cand[cur];
        static const char* from[SRC_N] = {"the options", "the TDMA control channel (network status)",
                                          "the Phase 1 control channel", "blind recovery from a scrambled traffic burst",
                                          "the inbound bursts' scrambling (243-step inbound offset assumed, NAC = colour code)"};
        if (cur == shown_src && s == shown_seed) return;
        shown_src = cur;
        shown_seed = s;
        int wacn = static_cast<int>(s >> 24), sys = static_cast<int>((s >> 12) & 0xFFF), nac = static_cast<int>(s & 0xFFF);
        ctx.report->set(Mode::P25, "WACN", hex(static_cast<uint64_t>(wacn), 5));
        ctx.report->set(Mode::P25, "System ID", hex(static_cast<uint64_t>(sys), 3));
        ctx.report->set(Mode::P25, "NAC", "0x" + hex(static_cast<uint64_t>(nac), 3));
        fact("Network ID from", from[cur]);
        event("Network: WACN " + hex(static_cast<uint64_t>(wacn), 5) + " sys " + hex(static_cast<uint64_t>(sys), 3) +
                  " NAC 0x" + hex(static_cast<uint64_t>(nac), 3) + " (from " + from[cur] + ")",
              30.0);
    }
    // the network identity Phase 1 learned on this input
    void phase1_identity() {
        if (cand_ok[SRC_PHASE1]) return;
        if (p1.wacn() >= 0 && p1.sysid() >= 0 && p1.nac() >= 0) {
            cand[SRC_PHASE1] = make_seed(p1.wacn(), p1.sysid(), p1.nac());
            cand_ok[SRC_PHASE1] = true;
            if (cur < 0 && seed_allowed(cand[SRC_PHASE1])) use_seed(SRC_PHASE1);
        }
    }
    bool seed_allowed(uint64_t s) const {
        if (u_nac >= 0 && static_cast<int>(s & 0xFFF) != u_nac) return false;
        if (u_wacn >= 0 && static_cast<int>(s >> 24) != u_wacn) return false;
        if (u_sysid >= 0 && static_cast<int>((s >> 12) & 0xFFF) != u_sysid) return false;
        return true;
    }

    // The user's NAC option and a MAC_END_PTT naming another colour code
    // (bits 12-23, = the NAC on every real call end seen) - an unscrambled
    // burst says nothing else about its system
    bool other_nac(const uint8_t* info) const {
        if (u_nac < 0 || (info[0] << 2 | info[1] << 1 | info[2]) != 2) return false;
        return static_cast<int>(bits_to_u32(info + 12, 12)) != u_nac;
    }

    // XOR the window starting at state W into bits -> work
    void descramble(uint64_t W) {
        pn_window(W, pn);
        for (int j = 0; j < 360; j++) work[j] = bits[j] ^ pn[j];
    }

    // A FACCH / SACCH / LCCH burst: (descramble), RS, CRC -> info. slot =
    // the superframe slot (-1 unknown). Returns false if it did not decode.
    bool block(int kind, bool scr, int slot, uint8_t* info, uint64_t* used_W) {
        const Layout& l = layout(kind);
        if (!scr) return decode_block(l, bits, info, relp) >= 0;
        scrambled_seen = true;
        if (!sl->inbound) {
            // the seed in use, then the other known ones
            if (cur >= 0) {
                if (slot >= 0) {
                    descramble(win[slot]);
                    if (decode_block(l, work, info, relp) >= 0) {
                        *used_W = win[slot];
                        seed_fail = 0;
                        show_identity();
                        return true;
                    }
                } else {
                    for (int k = 0; k < 12; k++) {
                        descramble(win[k]);
                        if (decode_block(l, work, info, relp) >= 0) {
                            *used_W = win[k];
                            found_slot = k;
                            seed_fail = 0;
                            show_identity();
                            return true;
                        }
                    }
                }
            }
            for (int src = 0; src < SRC_N; src++) {
                if (!cand_ok[src] || src == cur || slot < 0 || !seed_allowed(cand[src])) continue;
                uint64_t W = pn_jump(cand[src], 360LL * slot);
                descramble(W);
                if (decode_block(l, work, info, relp) >= 0) {
                    use_seed(src);
                    seed_fail = 0;
                    show_identity();
                    *used_W = W;
                    return true;
                }
            }
            // blind (8.6): one clean burst gives the window's start state
            if (cur >= 0 && ++seed_fail < BLIND_AFTER) return false;
            uint64_t W;
            if (!blind(kind).solve(l, bits, &W, rel, alt)) return false;
            descramble(W);
            if (decode_block(l, work, info, relp) < 0) return false;
            *used_W = W;
            if (slot >= 0) {
                uint64_t seed = pn_jump(W, -360LL * slot);
                // the burst's own scrambling names its system: with the
                // user's NAC / WACN / system ID set, another system's burst
                // is not decoded (as Phase 1's NAC option drops its frames)
                if (!seed_allowed(seed)) return false;
                cand[SRC_BLIND] = seed;
                cand_ok[SRC_BLIND] = true;
                use_seed(SRC_BLIND);
                show_identity();
            } else if (u_nac >= 0 || u_wacn >= 0 || u_sysid >= 0) {
                // superframe position unknown: some slot's window must give an allowed seed
                bool any = false;
                for (int k = 0; k < 12 && !any; k++) any = seed_allowed(pn_jump(W, -360LL * k));
                if (!any) return false;
            }
            return true;
        }
        return in_block(kind, l, info, used_W);
    }

    // --- inbound scrambler -----------------------------------------------------
    // The inbound scrambler state is solved from the bursts themselves; how
    // the windows of different slots relate is a hypothesis each: in_mode 1
    // = 360 x superframe slot apart from one superframe start state (the
    // outbound rule 8.3, [INFERRED for inbound, Q4]), 2 = one fixed window
    // for every slot, 0 = not known yet (both tried).
    // Window candidates of the burst at slot index j from the reference
    // burst; the sequence restarts every superframe, so a slot distance d
    // means d or d - 12 superframe slots depending on where the reference is
    // (in_alive = its possible superframe slots).
    int in_cands(int64_t j, uint64_t* st, uint16_t* allow, int8_t* model) const {
        if (!in_ref) return 0;
        int d = static_cast<int>(mod(j - in_i, 12));
        int n = 0;
        if (in_mode != 2) {
            uint16_t m1 = 0, m2 = 0;
            for (int s = 0; s < 12; s++) (s + d < 12 ? m1 : m2) |= static_cast<uint16_t>(1u << s);
            m1 &= in_alive;
            m2 &= in_alive;
            if (m1) { st[n] = pn_jump(in_W, 360LL * d); allow[n] = m1; model[n++] = 1; }
            if (m2) { st[n] = pn_jump(in_W, 360LL * (d - 12)); allow[n] = m2; model[n++] = 1; }
        }
        if (in_mode != 1 && (d != 0 || in_mode == 2)) { st[n] = in_W; allow[n] = 0xFFF; model[n++] = 2; }
        return n;
    }
    void in_set_ref(uint64_t W, int64_t j, uint16_t alive) {
        in_ref = true;
        in_W = W;
        in_i = j;
        in_mode = 0;
        in_alive = alive ? alive : 0xFFF;
        in_resolve();
    }
    // a burst at distance d != 0 decoded with candidate model m
    void in_learn(int model, uint16_t allow, int64_t j) {
        if (mod(j - in_i, 12) == 0) return;   // the same superframe slot says nothing about the model
        if (model == 2 && in_mode != 2) {
            in_mode = 2;
            in_alive = 0xFFF;   // a fixed window carries no superframe position
            if (verbose()) event("inbound scrambling: one fixed window for every slot", 60.0);
        } else if (model == 1) {
            in_mode = 1;
            if (in_alive & allow) in_alive &= allow;
        }
        in_resolve();
    }
    int in_phase_scr() const {
        if (!in_ref || in_mode != 1 || popcount64(in_alive) != 1) return -1;
        int s = 0;
        while (!((in_alive >> s) & 1)) s++;
        return static_cast<int>(mod(in_i - s, 12));
    }
    // inbound superframe position (slot index of superframe slot 0, mod 12):
    // the cadence first, else the scrambling windows
    int in_phase() const { return in_phase_cad >= 0 ? in_phase_cad : in_phase_scr(); }
    void in_resolve() {
        if (in_ref && in_mode != 2 && in_phase_cad >= 0) {
            uint16_t m = static_cast<uint16_t>(1u << mod(in_i - in_phase_cad, 12));
            if (in_alive & m) in_alive = m;
            else if (in_mode == 1 && verbose() && in_hyp_said != "phase")
                event("inbound: the burst cadence and the scrambling windows disagree on the superframe position", 60.0);
        }
        in_hypothesis();
    }
    // [SINGLE 8.2, Q4]: inbound superframe start state = outbound seed + 243
    // steps? Tested, never assumed: with the inbound superframe position known
    // and an outbound seed, compare; without an outbound seed derive the seed
    // it would imply and use it only when its NAC equals a checked colour code.
    int checked_cc = -1;
    void in_hypothesis() {
        if (!in_ref) return;
        uint64_t S0;
        if (in_mode == 2) S0 = in_W;
        else {
            const int p = in_phase();
            if (p < 0) return;
            S0 = pn_jump(in_W, -360LL * mod(in_i - p, 12));
        }
        std::string said;
        if (cur >= 0) {
            bool ok = pn_jump(cand[cur], INBOUND_SEED_ADVANCE) == S0;
            said = ok ? "inbound scrambling = outbound seed + 243 steps (confirmed)"
                      : "inbound scrambling: NOT the outbound seed + 243 steps (solved from the bursts instead)";
            if (ok) show_identity();
        } else {
            uint64_t seed = pn_jump(S0, -INBOUND_SEED_ADVANCE);
            if (checked_cc >= 0 && static_cast<int>(seed & 0xFFF) == checked_cc && seed_allowed(seed)) {
                cand[SRC_INBOUND] = seed;
                cand_ok[SRC_INBOUND] = true;
                use_seed(SRC_INBOUND);
                show_identity();
                said = "inbound scrambling: seed from the inbound bursts (243-step offset, NAC = the colour code)";
            }
        }
        if (!said.empty() && said != in_hyp_said) {
            in_hyp_said = said;
            if (verbose()) event(said, 60.0);
        }
    }
    bool in_block(int kind, const Layout& l, uint8_t* info, uint64_t* used_W) {
        const int64_t j = sl->index;
        uint64_t st[3];
        uint16_t allow[3];
        int8_t model[3];
        int n = in_cands(j, st, allow, model);
        for (int k = 0; k < n; k++) {
            descramble(st[k]);
            if (decode_block(l, work, info, relp) >= 0) {
                in_learn(model[k], allow[k], j);
                *used_W = st[k];
                in_fail = 0;
                return true;
            }
        }
        if (in_ref && ++in_fail < BLIND_AFTER) return false;
        // no reference yet: the outbound seed hypothesis, then blind
        if (!in_ref && cur >= 0) {
            uint64_t H = pn_jump(cand[cur], INBOUND_SEED_ADVANCE);
            for (int s = 0; s < 12; s++) {
                uint64_t W = pn_jump(H, 360LL * s);
                descramble(W);
                if (decode_block(l, work, info, relp) >= 0) {
                    in_set_ref(W, j, static_cast<uint16_t>(1u << s));
                    *used_W = W;
                    in_fail = 0;
                    return true;
                }
            }
        }
        uint64_t W;
        if (!blind(kind).solve(l, bits, &W, nullptr, nullptr, true)) return false;
        descramble(W);
        if (decode_block(l, work, info, relp) < 0) return false;
        *used_W = W;
        in_fail = 0;
        if (!in_ref) {
            in_set_ref(W, j, 0xFFF);
        } else {
            // how does it relate to the reference? (d or d - 12 superframe slots, or the same window)
            int d = static_cast<int>(mod(j - in_i, 12));
            uint16_t m1 = 0, m2 = 0;
            for (int s = 0; s < 12; s++) (s + d < 12 ? m1 : m2) |= static_cast<uint16_t>(1u << s);
            if (d && W == in_W) in_learn(2, 0xFFF, j);
            else if (pn_jump(in_W, 360LL * d) == W) in_learn(1, m1, j);
            else if (d && pn_jump(in_W, 360LL * (d - 12)) == W) in_learn(1, m2, j);
            else in_set_ref(W, j, 0xFFF);   // another system / the reference was wrong: start over
        }
        return true;
    }

    // --- channels, calls, talkers -----------------------------------------------
    int lane_lch(int lane) const {   // inbound lane (index parity of traffic bursts) -> LCH
        const int p = in_phase();
        if (p < 0) return -1;
        return (lane & 1) == ((p + 1) & 1) ? 0 : 1;
    }
    std::string name(const Chan& c) const {
        if (!c.inbound) return "LCH " + std::to_string(c.lch);
        return c.lch >= 0 ? "Inbound LCH " + std::to_string(c.lch) : std::string("Inbound");
    }
    bool wanted(const Chan& c) const { return lch_opt == 0 || c.lch < 0 || c.lch == lch_opt - 1; }
    std::string tcall_of(const Chan& c) const {
        if (!c.dst_known) return "voice call";
        return (c.group ? "TG " : "unit call to ") + std::to_string(c.dst) + ((c.so & 0x80) ? " EMERGENCY" : "");
    }
    bool encrypted(const Chan& c) const { return (c.alg >= 0 && c.alg != 0x80) || (c.so & 0x40); }
    // Played only when known to be clear: ALGID 0x80 (MAC_PTT / ESS) or a
    // voice channel user's service options without "protected". A call joined
    // by its voice alone (or named by a vendor message without service
    // options) stays muted until then - on real encrypted calls joined after
    // their MAC_PTT the first voice bursts came before the ESS named the
    // algorithm and played as noise
    bool clear_known(const Chan& c) const { return c.alg == 0x80 || (c.alg < 0 && c.so_known && !(c.so & 0x40)); }
    bool muted(const Chan& c) const { return encrypted(c) || !clear_known(c); }
    std::string make_desc(const Chan& c) const {
        if (!c.dst_known) return "";   // a call joined by its voice / ESS: who it is comes with the signalling
        std::string d = c.group ? "TG " + std::to_string(c.dst) + " <- " + std::to_string(c.src)
                                : "Unit " + std::to_string(c.src) + " -> unit " + std::to_string(c.dst);
        if (!c.src_named) d = c.group ? "TG " + std::to_string(c.dst) : "unit call to " + std::to_string(c.dst);
        if (c.alg >= 0 && c.alg != 0x80) d += " [" + P25Proto::algorithm_name(c.alg) + "]";
        else if (c.so & 0x40) d += " [encrypted]";
        if (c.so & 0x80) d += " [EMERGENCY]";
        return d;
    }
    void set_state(Chan& c, const std::string& st) {
        const std::string key = name(c);
        if (c.state == st && c.state_key == key) return;
        if (!c.state_key.empty() && c.state_key != key) fact(c.state_key, "");   // an inbound lane's LCH became known
        c.state = st;
        c.state_key = key;
        if (wanted(c)) fact(key, st);
    }
    void call_refresh(Chan& c) {
        std::string d = make_desc(c);
        if (d != c.desc) {
            // one event per talker: "[encrypted]" (service options) becoming
            // the algorithm's name (MAC_PTT / ESS) changes the state, not the call
            const std::string key = d.substr(0, d.find(" ["));
            const bool same = !c.desc.empty() && key == c.desc.substr(0, c.desc.find(" [")) &&
                              c.desc.find("[EMERGENCY]") == std::string::npos && d.find("[EMERGENCY]") == std::string::npos;
            c.desc = d;
            if (wanted(c) && !d.empty() && !same) event(name(c) + " voice: " + d, 5.0);
        }
        if (!c.desc.empty()) set_state(c, "Voice: " + c.desc);
        else set_state(c, encrypted(c) ? "Voice [" + (c.alg >= 0 && c.alg != 0x80 ? P25Proto::algorithm_name(c.alg) : std::string("encrypted")) + "]" : "Voice");
        if (c.alg >= 0)
            fact("Encryption", c.alg == 0x80 ? "clear" : P25Proto::algorithm_name(c.alg) + ", key 0x" + hex(static_cast<uint64_t>(c.kid), 4));
    }
    // a checked burst of the channel's call (opens it)
    void call_touch(Chan& c) {
        if (!c.call) {
            c.call = true;
            c.src = c.dst = 0;
            c.src_named = false;
            c.dst_known = false;
            c.group = true;
            c.so = 0;
            c.so_known = false;
            c.alg = -1;
            c.desc.clear();
            if (c.ambe) c.ambe->reset();
        }
        c.call_last = sl ? sl->start : now;
    }
    // so_valid: so is the message's own service options (false: c.so passed through)
    void call_user(Chan& c, uint32_t src, uint32_t dst, bool group, int so, bool name_src, bool have_dst = true,
                   bool so_valid = true) {
        call_touch(c);
        if (have_dst) {
            c.dst = dst;
            c.dst_known = true;
            c.group = group;
            c.so = so;
            if (so_valid) c.so_known = true;
        }
        if (name_src && src) {
            c.src = src;
            c.src_named = true;
            ctx.report->set(Mode::P25, "Last source", std::to_string(src));
        }
        if (have_dst && group) ctx.report->set(Mode::P25, "Last talkgroup", std::to_string(dst));
        call_refresh(c);
        if (name_src && src) {
            if (!c.inbound) talker_set(c, src);
            else unit_set(c, src);
        } else if (!c.inbound && !c.tid.empty() && have_dst) {
            c.tcall = tcall_of(c) + " · LCH " + std::to_string(c.lch) + " · via site";
        }
    }
    void call_end(Chan& c, const char* st) {
        if (c.call) {
            c.ended_at = sl ? sl->start : now;
            c.ended_src = c.src;
            c.ended_dst = c.dst;
        }
        if (c.call && !c.desc.empty() && wanted(c)) event(name(c) + " call ended: " + c.desc, 2.0);
        c.call = false;
        c.desc.clear();
        c.ess_have = 0;
        c.cyc_ref = INT64_MIN;
        c.alg = -1;
        if (voice_ch == (&c - ch) && ctx.voice_state) ctx.voice_state("");
        if (st) set_state(c, st);
        if (!c.inbound) talker_end(c, c.t_end);
        else unit_end(c);
    }

    // outbound talkers: a transmission per LCH, channel = LCH + 1 (as DMR's slots)
    void talker_begin(Chan& c, int64_t a) {
        if (c.t_start >= 0 && a - c.t_end <= GAP) return;
        talker_end(c, c.t_end);
        c.t_start = a;
        c.t_end = a;
    }
    void talker_burst(Chan& c) {
        talker_begin(c, sl->start);
        c.t_end = std::max(c.t_end, sl->end);
        if (!ctx.talker) return;
        const int chn = c.lch + 1;
        if (c.tid.empty()) {
            ctx.talker("?", "", c.t_start, c.t_end, chn);
        } else {
            ctx.talker(c.tid, c.tcall, c.t_start, c.t_end, chn);
        }
        c.treported = true;
    }
    void talker_set(Chan& c, uint32_t src) {
        talker_begin(c, sl->start);
        const std::string id = std::to_string(src);
        if (!c.tid.empty() && c.tid != id) {
            // another radio without an end in between: the bursts since the old
            // one was last named can't be told apart - leave them out
            talker_end(c, c.lc_end);
            c.t_start = sl->start;
            c.t_end = sl->start;
        }
        c.tid = id;
        c.tcall = tcall_of(c) + " · LCH " + std::to_string(c.lch) + " · via site";   // the talker label
        c.lc_end = sl->end;
    }
    void talker_end(Chan& c, int64_t at) {
        if (c.treported && ctx.talker_end) ctx.talker_end(at, c.lch + 1);
        c.treported = false;
        c.tid.clear();
        c.t_start = -1;
    }

    // inbound talkers: every checked burst is a packet of its radio, exactly
    // the samples its modulated symbols 6..173 occupy; bursts whose radio is
    // not named yet wait (up to PENDING_MAX) for its ID
    void packet(const Chan& c, int64_t a, int64_t b, float f) {
        if (!ctx.packet || !c.unit) return;
        RxContext::Packet p;
        p.id = std::to_string(c.unit);
        p.label = (c.group ? "TG " : "unit call to ") + std::to_string(c.dst) + " · inbound" +
                  (c.lch >= 0 ? " LCH " + std::to_string(c.lch) : "");
        if (!c.dst) p.label = c.lch >= 0 ? "inbound LCH " + std::to_string(c.lch) : "inbound";
        p.start = a;
        p.end = b;
        p.freq_hz = std::isfinite(f) && std::fabs(f) < 6000 ? f : 0.0;
        p.bw_hz = 12500;
        p.keep_s = 1800;
        ctx.packet(p);
    }
    void in_burst(Chan& c) {
        const int64_t a = sl->start + 6 * P25P2_SPS, b = sl->start + 174 * P25P2_SPS;
        if (c.last_burst >= 0 && a - c.last_burst > GAP) unit_end(c);
        c.last_burst = b;
        if (c.unit) {
            packet(c, a, b, sl->freq_hz);
            return;
        }
        if (c.npend == 48) {
            std::memmove(c.pend, c.pend + 1, sizeof(Chan::Pend) * 47);
            c.npend--;
        }
        c.pend[c.npend++] = {a, b, sl->freq_hz};
    }
    void unit_set(Chan& c, uint32_t u) {
        if (c.unit == u) return;
        if (c.unit) c.npend = 0;   // another radio without an end: the waiting bursts are nobody's for sure
        c.unit = u;
        const int64_t oldest = (sl ? sl->start : now) - PENDING_MAX;
        for (int i = 0; i < c.npend; i++)
            if (c.pend[i].a >= oldest) packet(c, c.pend[i].a, c.pend[i].b, c.pend[i].f);
        c.npend = 0;
    }
    void unit_end(Chan& c) {
        c.unit = 0;
        c.npend = 0;
    }

    // --- ISCH / superframe ----------------------------------------------------------
    int cur_slot() const { return phase >= 0 ? static_cast<int>(mod(sl->index - phase, 12)) : -1; }
    int64_t sf_number(int64_t index) const { return floor_div(index - phase, 12); }
    int uf_of(int64_t index) const { return (phase >= 0 && uf_ref >= 0) ? static_cast<int>(mod(uf_ref + sf_number(index), 4)) : -1; }
    void isch(const P25p2Slot& s) {
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < 10; i++) lo = (lo << 2) | SymSrc::dibit(s.sym[i]);
        int slot = -1, uf = -1, type = -1;
        bool s_isch = false;
        const auto& t = isch_table();
        if (s.have_pre) {
            for (int i = 0; i < 10; i++) hi = (hi << 2) | SymSrc::dibit(s.pre[i]);
            uint64_t v = (hi << 20) | lo;
            int best = 41, bm = -1;
            for (int m = 0; m < 512; m++) {
                int d = popcount64(v ^ t.cw[m]);
                if (d < best) { best = d; bm = m; }
            }
            int ds = popcount64(v ^ S_ISCH);
            if (ds <= 6 && ds < best) s_isch = true;
            else if (best <= 6) {
                int ty = (bm >> 7) & 3, chn = (bm >> 5) & 3, loc = (bm >> 3) & 3;
                if (ty != 2 && chn <= 1 && loc <= 2) {
                    slot = 4 * loc + chn;
                    uf = bm & 3;
                    type = ty;
                }
            }
        } else {
            // only the ISCH's second half (the first slot of a grid): exact match only
            int hits = 0, bm = -1;
            for (int m = 0; m < 512; m++)
                if ((t.cw[m] & 0xFFFFF) == lo) { hits++; bm = m; }
            if ((S_ISCH & 0xFFFFF) == lo) s_isch = true;
            else if (hits == 1) {
                int ty = (bm >> 7) & 3, chn = (bm >> 5) & 3, loc = (bm >> 3) & 3;
                if (ty != 2 && chn <= 1 && loc <= 2) {
                    slot = 4 * loc + chn;
                    uf = bm & 3;
                    type = ty;
                }
            }
        }
        if (slot >= 0) {
            int p = static_cast<int>(mod(s.index - slot, 12));
            if (phase < 0 || p == phase) {
                phase = p;
                phase_conf = std::min(phase_conf + 1, 8);
            } else if ((phase_conf -= 2) <= 0) {
                phase = p;
                phase_conf = 1;
                uf_ref = -1;
            }
            if (p == phase) {
                uf_ref = static_cast<int>(mod(uf - sf_number(s.index), 4));
                lch_type[out_lch(slot)] = type;
            }
        } else if (s_isch && phase >= 0) {
            int sl2 = static_cast<int>(mod(s.index - phase, 12));
            bool fits = sl2 == 2 || sl2 == 3 || sl2 == 6 || sl2 == 7 || sl2 == 10 || sl2 == 11;
            if (!fits && --phase_conf <= 0) { phase = -1; phase_conf = 0; uf_ref = -1; }
        }
    }

    // --- voice (10) --------------------------------------------------------------
    // c0 Golay errors of the frames of a 4V / 2V in bits b
    static int frame_c0(const uint8_t* b, int start_sym) {
        uint32_t v = 0;
        // c0(23..1) -> bits 22..0
        char fr[4][24] = {{0}};
        for (int k = 0; k < 36; k++) {
            const int s = start_sym + k;
            fr[mbe_tables::rW[k]][mbe_tables::rX[k]] = static_cast<char>(b[2 * s]);
            fr[mbe_tables::rY[k]][mbe_tables::rZ[k]] = static_cast<char>(b[2 * s + 1]);
        }
        for (int i = 23; i >= 1; i--) v = (v << 1) | static_cast<uint32_t>(fr[0][i]);
        return golay23c().errors(v);
    }
    // returns true when the burst counted (checked voice frames of an open call)
    bool voice(Chan& c, int duid, bool have_win, uint64_t W, int64_t ord) {
        const bool in = sl->inbound;
        const bool v2 = duid == D_2V;
        const int nf = v2 ? 2 : 4;
        static const int out_vf[4] = {11, 48, 96, 133}, in_vf[4] = {10, 47, 96, 133};
        const int* vf = in ? in_vf : out_vf;
        int e_clear[4], e_scr[4] = {9, 9, 9, 9}, tc = 0, ts = 99;
        for (int f = 0; f < nf; f++) tc += e_clear[f] = frame_c0(bits, vf[f]);
        // [INFERRED 8.4, Q3] voice bursts scrambled like FACCH / SACCH? Tried
        // both; the frames' own Golay words decide
        if (have_win) {
            descramble(W);
            ts = 0;
            for (int f = 0; f < nf; f++) ts += e_scr[f] = frame_c0(work, vf[f]);
        }
        bool scr = have_win && (ts < tc || (ts == tc && c.scr_vote > 0));
        if (have_win && ts != tc) c.scr_vote = std::clamp(c.scr_vote + (ts < tc ? 1 : -1), -8, 8);
        const uint8_t* b = scr ? work : bits;
        const int* e = scr ? e_scr : e_clear;
        int good = 0, tot = 0, worst = 0;
        for (int f = 0; f < nf; f++) {
            if (e[f] <= 1) good++;
            tot += e[f];
            worst = std::max(worst, e[f]);
        }
        bool ok = v2 ? (worst <= 2 && tot <= 2) : (good >= 2 && tot <= 6);
        // ESS
        bool ess_ok = false;
        if (!v2) {
            if (ord != INT64_MIN && c.cyc_ref != INT64_MIN) {
                int64_t rel = ord - c.cyc_ref - 1;
                int pos = static_cast<int>(mod(rel, 5));
                int64_t cyc = floor_div(rel, 5);
                if (pos == 4) {
                    c.cyc_ref = INT64_MIN;   // a 4V where the 2V belongs: the cycle position was wrong
                } else if (ok) {
                    if (cyc != c.ess_cycle) { c.ess_cycle = cyc; c.ess_have = 0; }
                    for (int h = 0; h < 4; h++)
                        c.ess[4 * pos + h] = static_cast<uint8_t>(bits_to_u32(b + 2 * 84 + 6 * h, 6));
                    c.ess_have |= 1 << pos;
                }
            }
        } else {
            uint8_t cw[44];
            for (int h = 0; h < 28; h++) {
                int v = 0;
                for (int k = 0; k < 6; k++) {
                    int bit = 6 * h + k;   // ESS-A: symbols 84-131 then 133-168
                    int s = bit < 96 ? 84 + bit / 2 : 133 + (bit - 96) / 2;
                    v = (v << 1) | b[2 * s + (bit & 1)];
                }
                cw[16 + h] = static_cast<uint8_t>(v);
            }
            bool same_cycle = false;
            if (ord != INT64_MIN && c.cyc_ref != INT64_MIN) {
                int64_t rel = ord - c.cyc_ref - 1;
                same_cycle = mod(rel, 5) == 4 && floor_div(rel, 5) == c.ess_cycle;
            }
            int eras[16], ne = 0;
            uint8_t t[44];
            memcpy(t, cw, sizeof t);
            for (int sgi = 0; sgi < 4; sgi++)
                for (int h = 0; h < 4; h++) {
                    if (same_cycle && (c.ess_have >> sgi) & 1) t[4 * sgi + h] = c.ess[4 * sgi + h];
                    else eras[ne++] = 4 * sgi + h;
                }
            int r = rs64_decode_erasures(t, 44, 28, eras, ne);
            if (r < 0 && ne < 16) {   // the collected segments may be the wrong ones: ESS-A alone
                memcpy(t, cw, sizeof t);
                ne = 0;
                for (int h = 0; h < 16; h++) eras[ne++] = h;
                r = rs64_decode_erasures(t, 44, 28, eras, ne);
            }
            if (r >= 0) {
                uint8_t eb[96], by[12];
                for (int h = 0; h < 16; h++) u64_to_bits(t[h], 6, eb + 6 * h);
                pack_bits(eb, 96, by);
                int alg = by[0], kid = (by[1] << 8) | by[2];
                bool mi_zero = true;
                for (int i = 3; i < 12; i++) mi_zero &= by[i] == 0;
                // sanity: clear = ALGID 0x80, KID 0, MI 0. ESS-A alone (12 of 28
                // parity symbols left) only updates a call that checked
                // signalling or clean voice frames opened
                if ((alg != 0x80 || (kid == 0 && mi_zero)) && (c.call || ok || ne <= 8)) {
                    ess_ok = true;
                    call_touch(c);
                    c.alg = alg;
                    c.kid = kid;
                    call_refresh(c);
                }
            }
            if (ord != INT64_MIN) {
                c.cyc_ref = ord;
                c.ess_cycle = INT64_MIN;
                c.ess_have = 0;
            }
            ok = ok || ess_ok;
        }
        if (!ok || !c.call) return false;
        valid_slot(!in);
        call_touch(c);
        if (!in) talker_burst(c);
        else in_burst(c);
        play(c, b, vf, nf);
        return true;
    }
    void play(Chan& c, const uint8_t* b, const int* vf, int nf) {
        if (!ctx.voice_wanted || !ctx.voice_wanted()) return;
        if (!wanted(c)) return;
        const int idx = static_cast<int>(&c - ch);
        // sample time, not the wall clock: an offline test runs far faster
        // than real time (and the live input can stall)
        const int64_t t = sl ? sl->start : now;
        if (lch_opt == 0) {
            // the first to talk keeps the audio until it has been quiet for a
            // second - except that a muted call (encrypted, or not known to
            // be clear) does not hold it against a clear one
            if (voice_ch >= 0 && voice_ch != idx && t - ch[voice_ch].last_voice < RATE &&
                !(muted(ch[voice_ch]) && !muted(c)))
                return;
            voice_ch = idx;
        }
        c.last_voice = t;
        std::string who = name(c) + (c.desc.empty() ? "" : " " + c.desc);
        if (!MbeLib::instance().available()) {
            if (ctx.voice_state) ctx.voice_state(who + ": AMBE+2 codec not installed (README: Digital voice codecs)");
            return;
        }
        if (encrypted(c)) {
            if (ctx.voice_state) ctx.voice_state(who + ": encrypted, muted");
            return;
        }
        if (!clear_known(c)) {
            if (ctx.voice_state) ctx.voice_state(who + ": encryption not known yet, muted");
            return;
        }
        if (!c.ambe) c.ambe = std::make_unique<AmbeStream>(false);
        float pcm[4 * 160];
        int clean = 0;
        for (int f = 0; f < nf; f++) {
            char fr[4][24] = {{0}};
            for (int k = 0; k < 36; k++) {
                const int s = vf[f] + k;
                fr[mbe_tables::rW[k]][mbe_tables::rX[k]] = static_cast<char>(b[2 * s]);
                fr[mbe_tables::rY[k]][mbe_tables::rZ[k]] = static_cast<char>(b[2 * s + 1]);
            }
            if (c.ambe->decode(fr, pcm + 160 * f) <= 2) clean++;
        }
        if (clean == 0) return;
        if (ctx.voice_channel) ctx.voice_channel(c.lch >= 0 ? c.lch + 1 : 0);
        if (ctx.voice) ctx.voice(Mode::P25, pcm, static_cast<size_t>(160 * nf));
        if (ctx.voice_state)
            ctx.voice_state(who + ": playing (AMBE+2, " + std::to_string(clean) + "/" + std::to_string(nf) + " frames clean)");
    }

    // --- a slot that passed its checks ------------------------------------------
    bool slot_valid = false;
    int64_t out_valid_end = -1;   // end of the last outbound slot that passed its checks
    int shown_mode = -1, shown_scr = -1, shown_type = -1;
    int found_slot = -1;
    void valid_slot(bool outbound) {
        if (slot_valid) return;
        slot_valid = true;
        last_valid = sl->start;
        if (outbound) {
            out_valid_end = std::max(out_valid_end, sl->end);
            report_valid(ctx, Mode::P25, sl->start, sl->end);
            if (ctx.freq_error) ctx.freq_error(Mode::P25, sl->freq_hz);
        } else {
            report_valid(ctx, Mode::P25, sl->start + 6 * P25P2_SPS, sl->start + 174 * P25P2_SPS);
        }
        // facts only when they change (no strings built per slot)
        const int mode = outbound ? 1 : 2, scr = scrambled_seen ? (cur >= 0 ? 2 : 1) : 0;
        if (mode != shown_mode) {
            shown_mode = mode;
            fact("Mode", outbound ? "Phase 2 TDMA" : "Phase 2 TDMA (inbound)");
        }
        if (scr != shown_scr) {
            shown_scr = scr;
            fact("Scrambling", scr == 2 ? "on (seed known)" : scr == 1 ? "on (seed not known yet)" : "off");
        }
    }

    // --- MAC PDUs (9) --------------------------------------------------------------
    struct Where {
        int kind;          // K_*
        Chan* c;           // the logical channel (nullptr: unknown)
        int uf;            // ultraframe count of the burst's superframe (-1 unknown)
        int64_t ord;       // traffic ordinal (INT64_MIN unknown)
        bool no_call = false;   // in a MAC_IDLE / MAC_HANGTIME PDU: a voice user message describes the call that ended
    };
    void mac_pdu(const uint8_t* info, const Where& w) {
        const Layout& l = layout(w.kind);
        const int nb = l.pdu_bits;
        uint8_t by[24] = {};
        // octets of the PDU; the outbound LCCH's last 12 bits are a trailer
        // (152-163, [INFERRED 12, Q9] the colour code - not used)
        const int noct = w.kind == K_OECI ? 19 : nb / 8;
        for (int i = 0; i < noct; i++) by[i] = static_cast<uint8_t>(bits_to_u32(info + 8 * i, 8));
        const int opc = by[0] >> 5, off = (by[0] >> 2) & 7;
        Chan* c = w.c;
        const bool lcch = w.kind == K_OECI || w.kind == K_IECI;
        if (lcch && c) set_state(*c, "Control channel");
        if (c && !c->inbound) lch_cc[c->lch] = lcch ? 1 : 0;
        {
            // per carrier: an LCCH on one or both LCHs, or two voice channels
            const bool cc0 = lch_cc[0] == 1, cc1 = lch_cc[1] == 1;
            const int other = lch_cc[cc0 ? 1 : 0];
            const int type = sl->inbound ? (lcch ? 1 : 2) : cc0 && cc1 ? 3 : (cc0 || cc1) ? (other == 0 ? 4 : 5) + (cc0 ? 0 : 2) : lcch ? 9 : 10;
            if (type != shown_type) {
                shown_type = type;
                static const char* names[11] = {"", "Control channel (TDMA, inbound)", "Voice (TDMA, inbound)", "Control channel (TDMA, dual LCCH)",
                                                "Control channel (TDMA, LCH 0) + voice channel", "Control channel (TDMA, LCH 0)",
                                                "Control channel (TDMA, LCH 1) + voice channel", "Control channel (TDMA, LCH 1)", "",
                                                "Control channel (TDMA)", "Voice (TDMA)"};
                fact("Channel type", names[type]);
            }
        }
        // [GPL-fact 9.1] PDU opcodes 1 PTT, 2 END_PTT, 4 ACTIVE, 6 HANGTIME (0 SIGNAL / 3 IDLE CONFIRMED-RF)
        switch (opc) {
            case 1: {   // MAC_PTT [GPL-fact layout, 9.2]
                if (!c || lcch) break;
                int alg = by[10], kid = (by[11] << 8) | by[12];
                uint32_t src = (static_cast<uint32_t>(by[13]) << 16) | (by[14] << 8) | by[15];
                uint32_t grp = (static_cast<uint32_t>(by[16]) << 8) | by[17];
                if (c->call && c->src_named && src && src != c->src) call_end(*c, nullptr);
                call_touch(*c);
                c->alg = alg;
                c->kid = kid;
                // [INFERRED 9.1] offset = this LCH's non-SACCH bursts before the first 4V
                if (w.ord != INT64_MIN && off <= 4) {
                    c->cyc_ref = w.ord + off;
                    c->ess_have = 0;
                    c->ess_cycle = INT64_MIN;
                }
                // a unit-to-unit call's MAC_PTT carries group address 0: the
                // voice channel user message says who is called
                call_user(*c, src, grp, true, c->so, true, grp != 0, false);
                break;
            }
            case 2: {   // MAC_END_PTT [GPL-fact layout, 9.2: colour code 12-23, source 104-127]
                if (!c || lcch) break;
                int cc = ((by[1] & 0x0F) << 8) | by[2];
                checked_cc = cc;
                uint32_t src = (static_cast<uint32_t>(by[13]) << 16) | (by[14] << 8) | by[15];
                if (c->inbound && src && src != 0xFFFFFF && !c->unit) unit_set(*c, src);
                if (c->inbound) in_burst(*c);   // its own burst, then the end
                call_end(*c, "Call ended");
                if (verbose()) event(name(*c) + " end of transmission, colour code 0x" + hex(static_cast<uint64_t>(cc), 3), 2.0);
                break;
            }
            case 3:     // MAC_IDLE
                if (c && !lcch) {
                    if (c->call) call_end(*c, "Idle");
                    else set_state(*c, "Idle");
                }
                {
                    Where w2 = w;
                    w2.no_call = true;
                    messages(by, noct, w2);
                }
                break;
            case 6:     // MAC_HANGTIME
                if (c && !lcch) {
                    if (c->call) call_end(*c, "Hangtime");
                    else set_state(*c, "Hangtime");
                }
                {
                    Where w2 = w;
                    w2.no_call = true;
                    messages(by, noct, w2);
                }
                break;
            case 0:     // MAC_SIGNAL
            case 4:     // MAC_ACTIVE
                messages(by, noct, w);
                break;
            default:
                if (verbose()) event("MAC PDU opcode " + std::to_string(opc) + ": " + hex_bytes(by, noct), 5.0);
                break;
        }
    }
    void messages(const uint8_t* by, int noct, const Where& w) {
        int i = 1;
        const auto& L = msg_len();
        while (i < noct) {
            const int op = by[i];
            if (op == 0x00) break;   // null information: the rest is padding
            int len = L.len[op];
            if (op == 0x08 && i + 1 < noct) len = by[i + 1];
            else if (len < 0 && i + 2 < noct) len = by[i + 2] & 0x3F;
            // [MEASURED on two Motorola systems' traffic channels, not in the
            // fact sheet] MFID 0x90 opcodes 0x80 / 0x83 carry no length in
            // octet 3 (0x00 / 0x40): they are 8 / 7 octets - after all 792
            // seen the next octet was a null, 0x80 or 0x83
            if (len == 0 && (op == 0x80 || op == 0x83) && i + 2 < noct && by[i + 1] == 0x90) len = op == 0x80 ? 8 : 7;
            if (len <= 0 || i + len > noct) {
                if (verbose()) event("MAC message 0x" + hex(static_cast<uint64_t>(op), 2) + " (length unknown): " +
                                         hex_bytes(by + i, noct - i), 5.0);
                break;
            }
            message(by + i, len, w);
            i += len;
        }
    }
    // a voice channel user message of the call that just ended (a SACCH
    // between / after the MAC_END_PTTs) does not open it again
    bool just_ended(const Chan& c, uint32_t src, uint32_t dst) const {
        return !c.call && c.ended_at >= 0 && sl && sl->start - c.ended_at < GAP && dst == c.ended_dst &&
               (src == c.ended_src || !src);
    }
    void raw(const uint8_t* m, int len, const char* what = nullptr) {
        if (!verbose()) return;
        const char* n = what ? what : msg_name(m[0]);
        std::string s = "MAC " + std::string(n ? n : "message") + " 0x" + hex(m[0], 2) + ": " + hex_bytes(m + 1, len - 1);
        event(s, 5.0);
    }
    void message(const uint8_t* m, int len, const Where& w) {
        const int op = m[0];
        const int part = op >> 6;
        Chan* c = w.c;
        // the outbound SACCH of an ultraframe's last superframe names the
        // INBOUND talker (2.2): it does not relabel the outbound call
        const bool names = !(c && !c->inbound && w.kind == K_IOEMI && w.uf != 0 && w.uf != 1 && w.uf != 2);
        if (part == 2) {   // manufacturer specific
            // [MEASURED, not in the fact sheet] Motorola (MFID 0x90) 0x80 =
            // talkgroup (octets 4-5) + source unit (6-8) of the LCH's call:
            // its unit matched the same LCH's MAC_PTT / voice channel user in
            // 81 of 82 checkable cases on two systems (the other one came before
            // the superframe position was known; source 0 in hang time and in
            // the ultraframe's last superframe). Motorola systems send
            // it on the SACCH where others send a voice channel user message,
            // so it names the talker of a call joined after its MAC_PTT.
            if (op == 0x80 && len == 8 && m[1] == 0x90 && (m[2] & 0x3F) == 0 && c && !c->inbound && !w.no_call) {
                const uint32_t grp = (static_cast<uint32_t>(m[3]) << 8) | m[4];
                const uint32_t src = (static_cast<uint32_t>(m[5]) << 16) | (m[6] << 8) | m[7];
                if (src && grp && !just_ended(*c, src, grp)) call_user(*c, src, grp, true, c->so, names, true, false);
            }
            if (verbose()) event("MAC manufacturer message MFID 0x" + hex(m[1], 2) + " opcode 0x" + hex(static_cast<uint64_t>(op), 2) +
                                     ": " + hex_bytes(m + 3, len - 3), 5.0);
            return;
        }
        switch (op) {
            case 0x08: return;   // null avoid zero bias (padding)
            // [GPL-fact 9.4] layouts of 0x01 / 0x21 / 0x02 / 0x05 / 0x31; 0x22 INFERRED from its length
            case 0x01:
            case 0x21: {   // group voice channel user
                if (!c || w.no_call) return raw(m, len);
                uint32_t grp = (static_cast<uint32_t>(m[2]) << 8) | m[3];
                uint32_t src = (static_cast<uint32_t>(m[4]) << 16) | (m[5] << 8) | m[6];
                if (just_ended(*c, src, grp)) return;
                call_user(*c, src, grp, true, m[1], names);
                return;
            }
            case 0x02:
            case 0x22: {   // unit-to-unit voice channel user
                if (!c || w.no_call) return raw(m, len);
                uint32_t dst = (static_cast<uint32_t>(m[2]) << 16) | (m[3] << 8) | m[4];
                uint32_t src = (static_cast<uint32_t>(m[5]) << 16) | (m[6] << 8) | m[7];
                if (just_ended(*c, src, dst)) return;
                call_user(*c, src, dst, false, m[1], names);
                return;
            }
            case 0x05: {   // group voice channel grant update, multiple (implicit)
                std::string e = "Grant update:";
                for (int k = 0; k < 3; k++) {
                    const uint8_t* q = m + 1 + 5 * k;
                    uint32_t chn = (static_cast<uint32_t>(q[1]) << 8) | q[2], grp = (static_cast<uint32_t>(q[3]) << 8) | q[4];
                    if (!chn && !grp) continue;
                    e += std::string(k ? "," : "") + " TG " + std::to_string(grp) + " on " + p1.chan_str(chn, true);
                    if (q[0] & 0x40) e += " [encrypted]";
                    if (q[0] & 0x80) e += " [EMERGENCY]";
                }
                event(e, 10.0);
                return;
            }
            case 0x31: {   // MAC_RELEASE
                uint32_t tgt = (static_cast<uint32_t>(m[2]) << 16) | (m[3] << 8) | m[4];
                int cc = ((m[5] & 0x0F) << 8) | m[6];
                checked_cc = cc;
                event("Release: unit " + std::to_string(tgt) + ((m[1] & 0x80) ? " (forced)" : "") +
                          ((m[1] & 0x40) ? " (talker preemption)" : ""), 5.0);
                return;
            }
            default: break;
        }
        // partition 01 = the Phase 1 TSBK body (9.4: CONFIRMED-RF for the broadcasts,
        // [INFERRED] for the grants)
        if (part == 1 && len >= 9 && phase1_parses(op & 0x3F)) {
            uint8_t t[10];
            t[0] = static_cast<uint8_t>(op & 0x3F);
            t[1] = 0x00;
            memcpy(t + 2, m + 1, 8);
            p1.tsbk(t, true);
            if (op == 0x7B && len >= 11) {   // NET_STS_BCST + the colour code (= the scrambling NAC)
                int cc = ((m[9] & 0x0F) << 8) | m[10];
                checked_cc = cc;
                int wacn = static_cast<int>(((m[2] << 12) | (m[3] << 4) | (m[4] >> 4)) & 0xFFFFF);
                int sys = ((m[4] & 0x0F) << 8) | m[5];
                uint64_t seed = make_seed(wacn, sys, cc);
                if (!cand_ok[SRC_CC] || cand[SRC_CC] != seed) {
                    cand[SRC_CC] = seed;
                    cand_ok[SRC_CC] = true;
                    if ((cur < 0 || cur == SRC_BLIND || cur == SRC_PHASE1 || cur == SRC_CC) && seed_allowed(seed)) use_seed(SRC_CC);
                }
                ctx.report->set(Mode::P25, "NAC", "0x" + hex(static_cast<uint64_t>(cc), 3));
                if (cur == SRC_CC) show_identity();
            }
            return;
        }
        raw(m, len);
    }

    // --- one slot ----------------------------------------------------------------
    void slot(const P25p2Slot& s) {
        sl = &s;
        slot_valid = false;
        now = std::max(now, s.end);
        if (s.grid_new) grid_lost();
        for (int k = 0; k < 180; k++) {
            const float x = s.sym[k];
            uint8_t d = SymSrc::dibit(x);
            bits[2 * k] = d >> 1;
            bits[2 * k + 1] = d & 1;
            // nearest threshold (-2, 0, 2) and the level just across it
            const float t = x >= 1.0f ? 2.0f : (x >= -1.0f ? 0.0f : -2.0f);
            rel[k] = std::fabs(x - t);
            alt[k] = SymSrc::dibit(x >= t ? t - 1.0f : t + 1.0f);
        }
        relp = s.inbound ? nullptr : rel;
        phase1_identity();
        if (s.inbound) inbound(s);
        else outbound(s);
        sl = nullptr;
    }
    void outbound(const P25p2Slot& s) {
        isch(s);
        // After a slip of the sample stream the receiver re-anchors its grid
        // on the slot before the new sync, which it may already have
        // delivered on the old grid: a slot that mostly lies inside one that
        // already passed its checks is the same burst again (no second
        // event, voice frame or talker extension)
        if (s.start < out_valid_end - P25P2_SLOT_SAMPLES / 2) return;
        const int slot = cur_slot();
        uint8_t du = static_cast<uint8_t>((bits[20] << 7) | (bits[21] << 6) | (bits[94] << 5) | (bits[95] << 4) |
                                          (bits[264] << 3) | (bits[265] << 2) | (bits[338] << 1) | bits[339]);
        const int duid = duid_decode(du);
        if (duid < 0) return;
        const int lch = slot >= 0 ? out_lch(slot) : -1;
        Chan* c = lch >= 0 ? &ch[lch] : nullptr;
        const int64_t ord = (slot >= 0 && slot < 10) ? sf_number(s.index) * 5 + slot / 2 : INT64_MIN;
        int kind = -1;
        bool scr = duid_scrambled(duid);
        switch (duid) {
            case D_4V:
            case D_2V: {
                if (!c || slot >= 10) return;
                if (lch_type[lch] == 3) return;   // the I-ISCH says LCCH
                const bool have = cur >= 0;
                voice(*c, duid, have, have ? win[slot] : 0, ord);
                return;
            }
            case D_SACCH: case D_SACCH_S: kind = K_IOEMI; break;
            case D_FACCH: case D_FACCH_S: kind = K_SOEMI; break;
            case D_LCCH: case D_LCCH_S: kind = K_OECI; break;
            default: return;
        }
        uint8_t info[210];
        uint64_t W = 0;
        found_slot = -1;
        if (!block(kind, scr, slot, info, &W)) return;
        if (other_nac(info)) return;
        if (slot < 0 && found_slot >= 0) {
            // the scrambling window that worked says where the superframe is
            phase = static_cast<int>(mod(s.index - found_slot, 12));
            phase_conf = 1;
        }
        valid_slot(true);
        if (kind == K_IOEMI && slot >= 0 && slot < 10) c = nullptr;   // a SACCH outside the inverted slots: LCH unsure
        if (c && c->call && kind != K_OECI) talker_burst(*c);   // (an END_PTT ends it after this burst)
        mac_pdu(info, {kind, c, uf_of(s.index), ord});
        if (c && c->call && kind != K_OECI) talker_burst(*c);
    }
    void inbound(const P25p2Slot& s) {
        uint8_t du = static_cast<uint8_t>((bits[92] << 7) | (bits[93] << 6) | (bits[166] << 5) | (bits[167] << 4) |
                                          (bits[264] << 3) | (bits[265] << 2) | (bits[338] << 1) | bits[339]);
        const int duid = duid_decode(du);
        if (duid < 0) return;
        const bool sacch = duid == D_SACCH || duid == D_SACCH_S;
        const int lane = static_cast<int>((s.index + (sacch ? 1 : 0)) & 1);
        Chan& c = ch[2 + lane];
        // [INFERRED 11 (c)] cadence of a checked burst: LCH 0's SACCH (slot 10)
        // follows its traffic burst of slot 9 directly, LCH 1's (slot 11)
        // precedes its slot 0
        auto cadence = [&] {
            if (sacch) {
                if (c.traffic_at == s.index - 1) in_cadence(s.index, 10);
                c.sacch_at = s.index;
                in_constrain_sacch(s.index);
            } else if (duid == D_4V || duid == D_2V || duid == D_FACCH || duid == D_FACCH_S) {
                if (c.sacch_at == s.index - 1) in_cadence(s.index - 1, 11);
                c.traffic_at = s.index;
            }
            c.lch = lane_lch(lane);
        };
        c.lch = lane_lch(lane);
        const int ph = in_phase();
        const int64_t ord = ph >= 0 ? [&] {
            int sl2 = static_cast<int>(mod(s.index - ph, 12));
            return sl2 < 10 ? floor_div(s.index - ph, 12) * 5 + sl2 / 2 : INT64_MIN;
        }() : INT64_MIN;
        int kind = -1;
        bool scr = duid_scrambled(duid);
        switch (duid) {
            case D_4V:
            case D_2V: {
                uint64_t st[3];
                uint16_t allow[3];
                int8_t model[3];
                const int n = in_cands(s.index, st, allow, model);
                // several candidate windows: the one whose frames check out best
                uint64_t Wb = 0;
                bool have = n > 0;
                int best = 1 << 20;
                for (int k = 0; k < n; k++) {
                    static const int vfi[4] = {10, 47, 96, 133};
                    descramble(st[k]);
                    int e = 0;
                    for (int f = 0; f < (duid == D_2V ? 2 : 4); f++) e += frame_c0(work, vfi[f]);
                    if (e < best) { best = e; Wb = st[k]; }
                }
                if (voice(c, duid, have, Wb, ord)) cadence();
                return;
            }
            case D_SACCH: case D_SACCH_S:
            case D_FACCH: case D_FACCH_S: kind = K_IEMI; break;
            case D_LCCH: case D_LCCH_S: kind = K_IECI; break;
            default: return;
        }
        uint8_t info[210];
        uint64_t W = 0;
        if (!block(kind, scr, -1, info, &W)) return;
        if (other_nac(info)) return;
        cadence();
        valid_slot(false);
        if (kind == K_IECI) {
            // a random-access control message of some radio: no lane state
            mac_pdu(info, {kind, nullptr, -1, INT64_MIN});
            return;
        }
        const int opc = info[0] << 2 | info[1] << 1 | info[2];
        const int64_t a = s.start + 6 * P25P2_SPS;
        if (c.last_burst >= 0 && a - c.last_burst > GAP) unit_end(c);
        mac_pdu(info, {kind, &c, -1, ord});
        if (opc != 2) {   // END_PTT reported its burst itself
            if (c.call || c.unit) in_burst(c);
            if (c.call) call_touch(c);
        }
    }
    void in_cadence(int64_t sacch_index, int s) {
        int p = static_cast<int>(mod(sacch_index - s, 12));
        if (in_phase_cad != p) {
            in_phase_cad = p;
            in_resolve();
        }
    }
    // the inbound SACCH occupies superframe slot 10 or 11 (2.1)
    void in_constrain_sacch(int64_t j) {
        if (!in_ref || in_mode == 2) return;
        int d = static_cast<int>(mod(j - in_i, 12));
        uint16_t m = static_cast<uint16_t>((1u << mod(10 - d, 12)) | (1u << mod(11 - d, 12)));
        if (in_alive & m) in_alive &= m;
        in_resolve();
    }

    void tick(int64_t t) {
        now = std::max(now, t);
        for (auto& c : ch) {
            if (!c.inbound && c.t_start >= 0 && now - c.t_end > GAP) talker_end(c, c.t_end);
            if (c.inbound && c.last_burst >= 0 && now - c.last_burst > GAP && (c.unit || c.npend)) unit_end(c);
            if (c.call && c.call_last >= 0 && now - c.call_last > CALL_TIMEOUT) call_end(c, "Idle");
        }
        // no Phase 2 for a while (the carrier is gone, or the channel carries
        // Phase 1 now): its channel state facts are no longer true; the
        // network identity stays
        if (shown_mode >= 0 && last_valid >= 0 && now - last_valid > STATE_FOR) {
            fact("Mode", "");
            fact("Scrambling", "");
            for (auto& c : ch) {
                if (!c.state_key.empty()) fact(c.state_key, "");
                c.state.clear();
                c.state_key.clear();
            }
            shown_mode = shown_scr = -1;
        }
    }
};

// ==================================================================================
P25p2Proto::P25p2Proto(RxContext& c, P25Proto& phase1) : d_(std::make_unique<Impl>(c, phase1)) {
    // precompute the tables now, not on the first slot
    (void)isch_table();
    (void)golay23c();
    for (int k = 0; k < K_N; k++) (void)blind(k);
    (void)msg_len();
}
P25p2Proto::~P25p2Proto() = default;
void P25p2Proto::p2_slot(const P25p2Slot& s) { d_->slot(s); }
void P25p2Proto::p2_grid_lost() { d_->grid_lost(); }
void P25p2Proto::reset() { d_->reset(); }
void P25p2Proto::tick(int64_t now) { d_->tick(now); }
void P25p2Proto::set_network(int wacn, int sysid, int nac) { d_->set_network(wacn, sysid, nac); }
void P25p2Proto::set_lch(int which) { d_->lch_opt = (which >= 0 && which <= 2) ? which : 0; }
bool P25p2Proto::active() const { return d_->last_valid >= 0 && d_->now - d_->last_valid < ACTIVE_FOR; }

}  // namespace dig
