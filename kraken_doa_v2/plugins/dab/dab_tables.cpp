#include "dab_tables.hpp"

#include "dab_tables_gen.hpp"

#include <cmath>
#include <cstdio>
#include <set>

namespace dab {

int prs_quarter(int k) {
    for (const auto& r : gen::TABLE23)
        if (k >= r.kmin && k <= r.kmax) return (gen::TABLE24[r.i][k - r.kp] + r.n) & 3;
    return 0;
}

const std::vector<int>& freq_interleave() {
    static const std::vector<int> F = [] {
        std::vector<int> f;
        f.reserve(K);
        int pi = 0;
        for (int i = 0; i < 2048; i++) {
            if (i > 0) pi = (13 * pi + 511) % 2048;
            if (pi >= 256 && pi <= 1792 && pi != 1024) f.push_back(pi - 1024);
        }
        return f;
    }();
    return F;
}

bool tii_bit(int p, int b) {
    if (p < 0 || p >= 70 || b < 0 || b > 7) return false;
    return gen::TII_PATTERN[p][b] == '1';
}

unsigned tii_mask(int p) {
    unsigned m = 0;
    for (int b = 0; b < 8; b++)
        if (tii_bit(p, b)) m |= 1u << b;
    return m;
}

int tii_pattern_of(unsigned mask) {
    static const std::vector<int> lut = [] {
        std::vector<int> t(256, -1);
        for (int p = 0; p < 70; p++) t[tii_mask(p)] = p;
        return t;
    }();
    return lut[mask & 255];
}

bool punct_bit(int pi, int i) {
    if (pi < 1 || pi > 24 || i < 0 || i > 31) return false;
    return gen::PUNCT[pi][i] == '1';
}

int punct_ones(int pi) {
    int n = 0;
    for (int i = 0; i < 32; i++) n += punct_bit(pi, i);
    return n;
}

bool protection_complete(Protection& p) {
    if (p.uep) {
        if (p.table_index < 0 || p.table_index > 63) return false;
        const auto& e = gen::TABLE8[p.table_index];
        p.size_cu = e.size_cu;
        p.level = e.level;
        p.kbps = e.kbps;
        return true;
    }
    static const int cuA[4] = {12, 8, 6, 4}, cuB[4] = {27, 21, 18, 15};
    if (p.level < 1 || p.level > 4 || p.size_cu < 1 || p.size_cu > 864) return false;
    if (p.option == 0) {
        if (p.size_cu % cuA[p.level - 1]) return false;
        p.kbps = 8 * (p.size_cu / cuA[p.level - 1]);
        return true;
    }
    if (p.option == 1) {
        if (p.size_cu % cuB[p.level - 1]) return false;
        p.kbps = 32 * (p.size_cu / cuB[p.level - 1]);
        return true;
    }
    return false;
}

std::string protection_name(const Protection& p) {
    char b[32];
    if (p.uep) snprintf(b, sizeof b, "UEP %d", p.level);
    else snprintf(b, sizeof b, "EEP %d-%c", p.level, p.option == 0 ? 'A' : 'B');
    return b;
}

bool coding_profile(const Protection& p, Profile& out) {
    out = Profile{};
    if (p.uep) {
        for (int i = 0; i < gen::TABLE15_N; i++) {
            const auto& e = gen::TABLE15[i];
            if (e.kbps != p.kbps || e.level != p.level) continue;
            for (int j = 0; j < 4; j++)
                if (e.L[j] > 0) out.blocks.push_back({e.L[j], e.PI[j]});
            out.pad = e.pad;
            out.in_bits = 24 * p.kbps;
            return true;
        }
        return false;
    }
    if (p.option == 0) {
        const int n = p.kbps / 8;
        if (n < 1) return false;
        switch (p.level) {
            case 4: out.blocks = {{4 * n - 3, 3}, {2 * n + 3, 2}}; break;
            case 3: out.blocks = {{6 * n - 3, 8}, {3, 7}}; break;
            case 2:
                if (n == 1) out.blocks = {{5, 13}, {1, 12}};
                else out.blocks = {{2 * n - 3, 14}, {4 * n + 3, 13}};
                break;
            case 1: out.blocks = {{6 * n - 3, 24}, {3, 23}}; break;
            default: return false;
        }
        out.in_bits = 192 * n;
        return true;
    }
    const int n = p.kbps / 32;
    if (n < 1) return false;
    static const int pi1[4] = {10, 6, 4, 2};
    if (p.level < 1 || p.level > 4) return false;
    out.blocks = {{24 * n - 3, pi1[p.level - 1]}, {3, pi1[p.level - 1] - 1}};
    out.in_bits = 768 * n;
    return true;
}

std::string country_name(int ecc, int cid) {
    for (int i = 0; i < gen::COUNTRIES_N; i++)
        if (gen::COUNTRIES[i].ecc == ecc && gen::COUNTRIES[i].cid == cid) return gen::COUNTRIES[i].name;
    return "";
}

std::string language_name(int code) {
    for (int i = 0; i < gen::LANGUAGES_N; i++)
        if (gen::LANGUAGES[i].code == code) return code == 0 ? "" : gen::LANGUAGES[i].name;
    return "";
}

std::string pty_name(int code, int inter_table) {
    if (code < 0 || code > 31) return "";
    return inter_table == 2 ? gen::PTY_TABLE13[code] : gen::PTY_TABLE12[code];
}

std::string announcement_names(unsigned flags) {
    std::string s;
    for (int b = 0; b < 16; b++) {
        if (!(flags & (1u << b))) continue;
        const char* n = gen::ANNOUNCEMENT[b];
        if (!s.empty()) s += ", ";
        s += *n ? std::string(n) : "type " + std::to_string(b);
    }
    return s;
}

std::string user_app_name(int ua) {
    // TS 101 756 table 16
    switch (ua) {
        case 0x002: return "SlideShow";
        case 0x004: return "TPEG";
        case 0x007: return "SPI";
        case 0x009: return "DMB";
        case 0x00D: return "Filecasting";
        case 0x00E: return "FIS";
        case 0x44A: return "Journaline";
        default: {
            char b[24];
            snprintf(b, sizeof b, "application 0x%03X", ua);
            return b;
        }
    }
}

std::string dscty_name(int d) {
    // TS 101 756 table 2b
    switch (d) {
        case 5: return "TDC";
        case 24: return "MPEG-2 TS";
        case 60: return "MOT";
        case 61: return "proprietary";
        default: return "DSCTy " + std::to_string(d);
    }
}

std::string band_block(double rf_hz) {
    char b[48];
    if (!std::isfinite(rf_hz) || rf_hz <= 0) return "";
    const double khz = rf_hz / 1e3;
    for (const auto& blk : gen::BAND3)
        if (std::fabs(khz - blk.khz) < 8.0) {
            snprintf(b, sizeof b, "%s %.3f MHz", blk.name, blk.khz / 1e3);
            return b;
        }
    snprintf(b, sizeof b, "%.3f MHz", rf_hz / 1e6);
    return b;
}

static void put_utf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string label_to_utf8(const uint8_t* p, int n, int charset) {
    std::string s;
    if (charset == 6) {   // UCS-2, big endian (BMP only)
        for (int i = 0; i + 1 < n; i += 2) {
            const unsigned cp = static_cast<unsigned>(p[i]) << 8 | p[i + 1];
            if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
            put_utf8(s, cp);
        }
    } else if (charset == 15) {
        for (int i = 0; i < n; i++)
            if (p[i]) s += static_cast<char>(p[i]);
    } else {   // 0: complete EBU Latin based repertoire (others: shown as that too)
        for (int i = 0; i < n; i++) {
            const unsigned cp = gen::EBU_LATIN[p[i]];
            if (cp) put_utf8(s, cp);
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && s[a] == ' ') a++;
    return s.substr(a);
}

std::string printable_utf8(const std::string& s) {
    std::string o;
    const size_t n = s.size();
    auto esc = [&o](unsigned char c) {
        char b[8];
        snprintf(b, sizeof b, "\\x%02X", c);
        o += b;
    };
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F) {
            o += static_cast<char>(c);
            i++;
            continue;
        }
        int len = 0;
        unsigned cp = 0, minv = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; minv = 0x80; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; minv = 0x800; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; minv = 0x10000; }
        bool ok = len > 0 && i + static_cast<size_t>(len) <= n;
        for (int j = 1; ok && j < len; j++) {
            const unsigned char d = static_cast<unsigned char>(s[i + static_cast<size_t>(j)]);
            if ((d & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (d & 0x3F);
        }
        if (ok && (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (cp >= 0x80 && cp < 0xA0)))
            ok = false;
        if (ok) {
            o.append(s, i, static_cast<size_t>(len));
            i += static_cast<size_t>(len);
        } else {
            esc(c);
            i++;
        }
    }
    return o;
}

std::string utf8_truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t e = n;
    while (e > 0 && (static_cast<unsigned char>(s[e]) & 0xC0) == 0x80) e--;
    return s.substr(0, e);
}

std::string self_test() {
    std::string err;
    auto fail = [&err](const std::string& s) { err += (err.empty() ? "" : "; ") + s; };
    // frequency interleaver: 1536 distinct carriers, table 25's first / last entries
    const auto& F = freq_interleave();
    std::set<int> ks(F.begin(), F.end());
    if (F.size() != 1536 || ks.size() != 1536 || ks.count(0) || *ks.begin() != -768 || *ks.rbegin() != 768)
        fail("frequency interleaver");
    else if (F[0] != -513 || F[1] != -14 || F[2] != 329 || F[3] != 692 || F[4] != -733 || F[9] != 43 ||
             F[1533] != 652 || F[1534] != 606 || F[1535] != 197)
        fail("frequency interleaver vs table 25");
    // puncturing vectors: 8 + PI ones, and each vector contains the previous one
    for (int pi = 1; pi <= 24; pi++) {
        if (punct_ones(pi) != 8 + pi) fail("puncturing vector " + std::to_string(pi) + " weight");
        for (int i = 0; pi > 1 && i < 32; i++)
            if (punct_bit(pi - 1, i) && !punct_bit(pi, i)) fail("puncturing vector " + std::to_string(pi) + " nesting");
    }
    // TII: 70 distinct patterns with exactly 4 of 8 bits
    std::set<unsigned> masks;
    for (int p = 0; p < 70; p++) {
        if (__builtin_popcount(tii_mask(p)) != 4) fail("TII pattern " + std::to_string(p));
        masks.insert(tii_mask(p));
    }
    if (masks.size() != 70) fail("TII patterns not distinct");
    // figure 64: c = 1, p = 11 -> carriers -670, -622, -526, -478 (first quarter)
    {
        std::vector<int> got;
        for (int b = 0; b < 8; b++)
            if (tii_bit(11, b)) got.push_back(tii_carrier(1, b, 0));
        if (got != std::vector<int>({-670, -622, -526, -478})) fail("TII figure 64");
    }
    // UEP: every table 8 entry has a table 15 profile whose length is its size in CUs
    for (int i = 0; i < 64; i++) {
        Protection p;
        p.uep = true;
        p.table_index = i;
        Profile pr;
        if (!protection_complete(p) || !coding_profile(p, pr)) {
            fail("UEP index " + std::to_string(i) + " has no profile");
            continue;
        }
        int bits = 12 + pr.pad, blocks = 0;
        for (auto& b : pr.blocks) {
            bits += b.first * 4 * (8 + b.second);
            blocks += b.first;
        }
        if (bits != 64 * p.size_cu) fail("UEP index " + std::to_string(i) + " length");
        if (blocks * 32 != pr.in_bits) fail("UEP index " + std::to_string(i) + " blocks");
    }
    // EEP: sizes for n = 1..30 (8 kbit/s steps) and 1..10 (32 kbit/s)
    static const int cuA[4] = {12, 8, 6, 4}, cuB[4] = {27, 21, 18, 15};
    for (int opt = 0; opt < 2; opt++)
        for (int lev = 1; lev <= 4; lev++)
            for (int n = 1; n <= (opt ? 10 : 30); n++) {
                Protection p;
                p.uep = false;
                p.option = opt;
                p.level = lev;
                p.size_cu = n * (opt ? cuB[lev - 1] : cuA[lev - 1]);
                Profile pr;
                if (!protection_complete(p) || !coding_profile(p, pr)) {
                    fail("EEP profile");
                    continue;
                }
                int bits = 12, blocks = 0;
                for (auto& b : pr.blocks) {
                    bits += b.first * 4 * (8 + b.second);
                    blocks += b.first;
                }
                if (bits != 64 * p.size_cu || blocks * 32 != pr.in_bits) fail("EEP length");
            }
    // phase reference: every carrier has a row in table 23
    for (int k = -768; k <= 768; k++) {
        if (!k) continue;
        bool found = false;
        for (const auto& r : gen::TABLE23)
            if (k >= r.kmin && k <= r.kmax && k - r.kp >= 0 && k - r.kp < 32) found = true;
        if (!found) fail("table 23 misses carrier " + std::to_string(k));
    }
    // EBU Latin: the ASCII range maps to itself except the substitutions of table C.1
    if (gen::EBU_LATIN[0x41] != 0x41 || gen::EBU_LATIN[0x24] != 0x142 || gen::EBU_LATIN[0xAB] != 0x24 ||
        gen::EBU_LATIN[0x5C] != 0x16E || gen::EBU_LATIN[0x80] != 0xE1)
        fail("EBU Latin table");
    return err;
}

}   // namespace dab
