// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The slicers, bit-row helpers and checks in this file follow
// rtl_433's (https://github.com/merbanan/rtl_433, GPL-2.0-or-later),
// re-implemented in C++ for the KrakenSDR plugin API (2026):
//   src/bitbuffer.c - Copyright (C) 2015 Tommy Vestermark
//   src/bit_util.c - Copyright (C) 2015 Tommy Vestermark
//   src/pulse_slicer.c - Copyright (C) 2015 Tommy Vestermark

// Bit rows, checks and the slicers (pulse trains -> bit rows).

#include "ism.hpp"

#include <algorithm>
#include <cstdlib>

namespace ism {

// ------------------------------------------------------------------- Row ---
int Row::find(const uint8_t* pat, int patbits, int from) const {
    for (int i = std::max(0, from); i + patbits <= n; i++) {
        int k = 0;
        for (; k < patbits; k++)
            if (bit(i + k) != ((pat[k >> 3] >> (7 - (k & 7))) & 1)) break;
        if (k == patbits) return i;
    }
    return -1;
}

int Row::find(uint64_t pat, int patbits, int from) const {
    if (patbits <= 0 || patbits > 64 || n < patbits) return -1;
    const uint64_t mask = patbits == 64 ? ~0ull : ((1ull << patbits) - 1);
    pat &= mask;
    int i = std::max(0, from);
    if (i + patbits > n) return -1;
    uint64_t w = get(i, patbits);
    for (;;) {
        if (w == pat) return i;
        if (i + patbits >= n) return -1;
        w = ((w << 1) | static_cast<uint64_t>(bit(i + patbits))) & mask;
        i++;
    }
}

Row Row::inverted() const {
    Row r = *this;
    for (auto& v : r.b) v = static_cast<uint8_t>(~v);
    if (r.n & 7) r.b.back() &= static_cast<uint8_t>(0xFF << (8 - (r.n & 7)));
    return r;
}

Row Row::manchester(int pos, int max, bool one_is_10) const {
    Row r;
    r.t0 = t0;
    r.t1 = t1;
    for (int i = pos; i + 1 < n && r.n < max; i += 2) {
        const int a = bit(i), c = bit(i + 1);
        if (a == c) break;
        r.push(one_is_10 ? a : c);
    }
    return r;
}

Row Row::sub(int pos, int nbits) const {
    Row r;
    r.t0 = t0;
    r.t1 = t1;
    for (int i = 0; i < nbits && pos + i < n; i++) r.push(bit(pos + i));
    return r;
}

std::string Row::hex() const {
    std::string s;
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < (n + 3) / 4; i++) s += H[get(4 * i, 4)];
    return s;
}

int Rows::count_same(size_t i) const {
    int c = 0;
    for (const auto& x : r)
        if (x.same(r[i])) c++;
    return c;
}

int Rows::repeated(int min_count, int min_bits) const {
    for (size_t i = 0; i < r.size(); i++) {
        if (r[i].n < min_bits) continue;
        if (count_same(i) >= min_count) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------- checks ---
uint8_t crc8(const uint8_t* p, int n, uint8_t poly, uint8_t init) {
    uint8_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 0x80) ? static_cast<uint8_t>((c << 1) ^ poly) : static_cast<uint8_t>(c << 1);
    }
    return c;
}

uint8_t crc8le(const uint8_t* p, int n, uint8_t poly, uint8_t init) {
    const uint8_t rp = reverse8(poly);
    uint8_t c = reverse8(init);
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint8_t>((c >> 1) ^ rp) : static_cast<uint8_t>(c >> 1);
    }
    return c;
}

uint16_t crc16(const uint8_t* p, int n, uint16_t poly, uint16_t init) {
    uint16_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(p[i] << 8);
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ poly) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

uint16_t crc16lsb(const uint8_t* p, int n, uint16_t poly, uint16_t init) {
    uint16_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ poly) : static_cast<uint16_t>(c >> 1);
    }
    return c;
}

uint8_t lfsr_digest8(const uint8_t* p, int n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t d = p[i];
        for (int k = 7; k >= 0; k--) {
            if ((d >> k) & 1) sum ^= key;
            key = (key & 1) ? static_cast<uint8_t>((key >> 1) ^ gen) : static_cast<uint8_t>(key >> 1);
        }
    }
    return sum;
}

uint8_t lfsr_digest8_reflect(const uint8_t* p, int n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    // bytes from the last to the first, bits LSB first
    for (int i = n - 1; i >= 0; i--) {
        const uint8_t d = p[i];
        for (int k = 0; k < 8; k++) {
            if ((d >> k) & 1) sum ^= key;
            key = (key & 0x80) ? static_cast<uint8_t>((key << 1) ^ gen) : static_cast<uint8_t>(key << 1);
        }
    }
    return sum;
}

uint16_t lfsr_digest16(const uint8_t* p, int n, uint16_t gen, uint16_t key) {
    uint16_t sum = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t d = p[i];
        for (int k = 7; k >= 0; k--) {
            if ((d >> k) & 1) sum ^= key;
            key = (key & 1) ? static_cast<uint16_t>((key >> 1) ^ gen) : static_cast<uint16_t>(key >> 1);
        }
    }
    return sum;
}

int add_bytes(const uint8_t* p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return s;
}
int xor_bytes(const uint8_t* p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s ^= p[i];
    return s;
}
int add_nibbles(const uint8_t* p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += (p[i] >> 4) + (p[i] & 15);
    return s;
}
int parity8(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}
int parity_bytes(const uint8_t* p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s ^= parity8(p[i]);
    return s;
}
uint8_t reverse8(uint8_t v) {
    v = static_cast<uint8_t>((v & 0xF0) >> 4 | (v & 0x0F) << 4);
    v = static_cast<uint8_t>((v & 0xCC) >> 2 | (v & 0x33) << 2);
    v = static_cast<uint8_t>((v & 0xAA) >> 1 | (v & 0x55) << 1);
    return v;
}

// --------------------------------------------------------------- slicers ---
namespace {

// The rows of the message in progress, kept like a bit buffer: row 0 comes
// into being with the first bit or row break; add_row() always adds a row
// (an empty one too), add_sync() only after a non-empty row
struct Builder {
    Rows out;
    std::vector<Row> rows;
    int msg = 0;
    void add_bit(int v, int64_t ts, int64_t te) {
        if (rows.empty()) rows.emplace_back();
        Row& r = rows.back();
        if (r.n == 0) r.t0 = ts;
        r.push(v);
        r.t1 = te;
    }
    void add_row() {
        if (rows.empty()) rows.emplace_back();
        rows.emplace_back();
    }
    void add_sync() {
        if (rows.empty()) rows.emplace_back();
        if (rows.back().n > 0) rows.emplace_back();
    }
    bool cur_empty() const { return rows.empty() || rows.back().n == 0; }
    bool any_rows() const { return !rows.empty(); }
    bool has_data() const { return (!rows.empty() && rows[0].n > 0) || rows.size() > 1; }
    void end_msg() {
        for (auto& r : rows) {
            r.msg = msg;
            out.r.push_back(std::move(r));
        }
        rows.clear();
        msg++;
    }
    void clear() { rows.clear(); }
};

// pulse / gap n of a train as one alternating symbol stream
inline int symbol(const std::vector<Pulse>& ps, size_t n) { return n & 1 ? ps[n >> 1].off : ps[n >> 1].on; }
inline int64_t symbol_t(const std::vector<Pulse>& ps, size_t n) {
    return n & 1 ? ps[n >> 1].t + ps[n >> 1].on : ps[n >> 1].t;
}

}  // namespace

std::vector<Rows> Rows::split() const {
    std::vector<Rows> v;
    for (const auto& x : r) {
        if (v.empty() || v.back().r.back().msg != x.msg) v.emplace_back();
        v.back().r.push_back(x);
    }
    return v;
}

const Rows& Pkt::pwm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int sync_us, int tol_us) {
    const Key k{1, static_cast<int>(m), short_us, long_us, gap_us, reset_us, sync_us, tol_us};
    if (const Rows* r = find(k)) return *r;
    const int s_short = us2s(short_us), s_long = us2s(long_us), s_gap = us2s(gap_us), s_reset = us2s(reset_us),
              s_sync = us2s(sync_us), s_tol = us2s(tol_us);
    // windows for 1 / 0 / sync (both bounds exclusive)
    int one_l, one_u, zero_l, zero_u, sync_l = 0, sync_u = 0;
    if (s_tol > 0) {
        one_l = s_short - s_tol;
        one_u = s_short + s_tol;
        zero_l = s_long - s_tol;
        zero_u = s_long + s_tol;
        if (s_sync > 0) {
            sync_l = s_sync - s_tol;
            sync_u = s_sync + s_tol;
        }
    } else if (s_sync <= 0) {
        one_l = 0;
        one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1;
        zero_u = 1 << 30;
    } else if (s_sync < s_short) {
        sync_l = 0;
        sync_u = (s_sync + s_short) / 2 + 1;
        one_l = sync_u - 1;
        one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1;
        zero_u = 1 << 30;
    } else if (s_sync < s_long) {
        one_l = 0;
        one_u = (s_short + s_sync) / 2 + 1;
        sync_l = one_u - 1;
        sync_u = (s_sync + s_long) / 2 + 1;
        zero_l = sync_u - 1;
        zero_u = 1 << 30;
    } else {
        one_l = 0;
        one_u = (s_short + s_long) / 2 + 1;
        zero_l = one_u - 1;
        zero_u = (s_long + s_sync) / 2 + 1;
        sync_l = zero_u - 1;
        sync_u = 1 << 30;
    }
    Builder b;
    const auto& ps = pulses(m);
    for (size_t n = 0; n < ps.size(); n++) {
        const Pulse& p = ps[n];
        const int w = p.on;
        const int64_t te = p.t + w;
        if (w > one_l && w < one_u) b.add_bit(1, p.t, te);
        else if (w > zero_l && w < zero_u) b.add_bit(0, p.t, te);
        else if (w > sync_l && w < sync_u) b.add_sync();
        else if (w <= one_l) {
        }   // too short: ignored
        else b.add_row();
        if ((n + 1 == ps.size() || p.off > s_reset) && b.any_rows()) b.end_msg();
        else if (s_gap > 0 && p.off > s_gap && b.any_rows() && !b.cur_empty()) b.add_row();
    }
    return put(k, std::move(b.out));
}

const Rows& Pkt::ppm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int sync_us, int tol_us) {
    const Key k{2, static_cast<int>(m), short_us, long_us, gap_us, reset_us, sync_us, tol_us};
    if (const Rows* r = find(k)) return *r;
    const int s_short = us2s(short_us), s_long = us2s(long_us), s_gap = us2s(gap_us), s_reset = us2s(reset_us),
              s_sync = us2s(sync_us), s_tol = us2s(tol_us);
    int zero_l, zero_u, one_l, one_u, sync_l = 0, sync_u = 0;
    if (s_tol > 0) {
        zero_l = s_short - s_tol;
        zero_u = s_short + s_tol;
        one_l = s_long - s_tol;
        one_u = s_long + s_tol;
        if (s_sync > 0) {
            sync_l = s_sync - s_tol;
            sync_u = s_sync + s_tol;
        }
    } else {
        zero_l = 0;
        zero_u = (s_short + s_long) / 2 + 1;
        one_l = zero_u - 1;
        one_u = s_gap ? s_gap : s_reset;
    }
    Builder b;
    const auto& ps = pulses(m);
    for (size_t n = 0; n < ps.size(); n++) {
        const Pulse& p = ps[n];
        const int g = p.off;
        const int64_t te = p.t + p.on + std::min(g, s_long * 2);
        if (g > zero_l && g < zero_u) b.add_bit(0, p.t, te);
        else if (g > one_l && g < one_u) b.add_bit(1, p.t, te);
        else if (g > sync_l && g < sync_u) b.add_sync();
        else if (g < s_reset) b.add_row();
        if ((n + 1 == ps.size() || g >= s_reset) && b.has_data()) b.end_msg();
    }
    return put(k, std::move(b.out));
}

const Rows& Pkt::pcm(Mod m, int short_us, int long_us, int gap_us, int reset_us, int tol_us) {
    const Key k{3, static_cast<int>(m), short_us, long_us, gap_us, reset_us, tol_us, 0};
    if (const Rows* r = find(k)) return *r;
    const auto& ps = pulses(m);
    const int s_short = us2s(short_us), s_long = us2s(long_us), s_reset = us2s(reset_us), s_gap0 = us2s(gap_us);
    int s_tol = us2s(tol_us);
    if (s_short <= 0 || s_long <= 0) return put(k, Rows());
    const int gap_limit = s_gap0 ? s_gap0 : s_reset;
    const int max_zeros = gap_limit / s_long;
    if (s_tol <= 0) s_tol = s_long / 4;
    const bool rz = s_short != s_long;
    double f_short = 1.0 / s_short, f_long = 1.0 / s_long;
    // the exact bit width: from a preamble (a run of single bits), else from
    // every width within the tolerance
    int min_count = rz ? 4 : 12, preamble = 0;
    for (size_t n = 0; rz && n < ps.size(); n++) {
        int sw = 0, lw = 0, cnt = 0;
        while (n < ps.size() && std::abs(ps[n].on - s_short) <= s_tol &&
               std::abs(ps[n].on + ps[n].off - s_long) <= s_tol) {
            sw += ps[n].on;
            lw += ps[n].on + ps[n].off;
            cnt++;
            n++;
        }
        if (cnt >= min_count) {
            f_long = static_cast<double>(cnt) / lw;
            f_short = static_cast<double>(cnt) / sw;
            min_count = cnt;
            preamble = cnt;
        }
    }
    if (rz && !preamble) {
        int sw = 0, lw = 0, cnt = 0;
        for (const auto& p : ps)
            if (std::abs(p.on - s_short) <= s_tol && std::abs(p.on + p.off - s_long) <= s_tol) {
                sw += p.on;
                lw += p.on + p.off;
                cnt++;
            }
        if (cnt > 8) {
            f_long = static_cast<double>(cnt) / lw;
            f_short = static_cast<double>(cnt) / sw;
        }
    }
    for (size_t n = 0; !rz && n < ps.size(); n++) {
        int w = 0, cnt = 0;
        while (n < ps.size() && static_cast<int>(ps[n].on * f_short + 0.5) == 1 &&
               static_cast<int>(ps[n].off * f_long + 0.5) == 1) {
            w += ps[n].on + ps[n].off;
            cnt += 2;
            n++;
        }
        if (cnt >= min_count) {
            f_short = f_long = static_cast<double>(cnt) / w;
            min_count = cnt;
            preamble = cnt;
        }
    }
    if (!rz && !preamble) {
        int w = 0, cnt = 0;
        for (const auto& p : ps) {
            if (std::abs(p.on - s_short) <= s_tol) { w += p.on; cnt += 1; }
            if (std::abs(p.on - 2 * s_short) <= s_tol) { w += p.on; cnt += 2; }
            if (std::abs(p.off - s_long) <= s_tol) { w += p.off; cnt += 1; }
            if (std::abs(p.off - 2 * s_long) <= s_tol) { w += p.off; cnt += 2; }
        }
        if (cnt > 20) f_short = f_long = static_cast<double>(cnt) / w;
    }
    Builder b;
    for (size_t n = 0; n < ps.size(); n++) {
        const Pulse& p = ps[n];
        const int highs = static_cast<int>(p.on * f_short + 0.5);
        int lows = static_cast<int>((p.off + s_short - s_long) * f_long + 0.5);
        const double bl = 1.0 / f_long, bh = 1.0 / f_short;
        for (int i = 0; i < highs; i++)
            b.add_bit(1, p.t + static_cast<int64_t>(i * bh), p.t + static_cast<int64_t>((i + 1) * bh));
        lows = std::min(lows, max_zeros);
        const int64_t g0 = p.t + p.on;
        for (int i = 0; i < lows; i++)
            b.add_bit(0, g0 + static_cast<int64_t>(i * bl), g0 + static_cast<int64_t>((i + 1) * bl));
        if (rz && std::abs(p.on - s_short) > s_tol) b.clear();   // corrupt: the message so far is dropped
        else if (p.off > gap_limit && p.off <= s_reset) b.add_row();
        if ((n + 1 == ps.size() || p.off > s_reset) && b.has_data()) b.end_msg();
    }
    return put(k, std::move(b.out));
}

const Rows& Pkt::manchester_zerobit(Mod m, int short_us, int reset_us, int tol_us) {
    const Key k{5, static_cast<int>(m), short_us, reset_us, tol_us, 0, 0, 0};
    if (const Rows* r = find(k)) return *r;
    const int h = us2s(short_us), s_reset = us2s(reset_us), s_tol = us2s(tol_us);
    const double h15 = 1.5 * h;
    const auto& ps = pulses(m);
    Builder b;
    int since = 0;   // time since the last mid-bit edge
    if (!ps.empty()) b.add_bit(0, ps[0].t, ps[0].t);   // the implied first 0 (a rising edge)
    for (size_t n = 0; n < ps.size(); n++) {
        const Pulse& p = ps[n];
        const int64_t gs = p.t + p.on;
        if (s_tol > 0 && (p.on < h - s_tol || p.on > 2 * h + s_tol || p.off < h - s_tol || p.off > 2 * h + s_tol)) {
            if (p.on > h15 && p.on <= 2 * h + s_tol) b.add_bit(1, p.t, gs);
            b.add_row();
            b.add_bit(0, gs, gs);
            since = 0;
        } else if (p.on + since > h15) {
            b.add_bit(1, p.t, gs);
            since = 0;
        } else {
            since += p.on;
        }
        if ((n + 1 == ps.size() || p.off > s_reset) && b.any_rows()) {
            b.end_msg();
            if (n + 1 < ps.size()) b.add_bit(0, ps[n + 1].t, ps[n + 1].t);   // the next message's implied 0
            since = 0;
        } else if (p.off + since > h15) {
            b.add_bit(0, gs, gs + p.off);
            since = 0;
        } else {
            since += p.off;
        }
    }
    return put(k, std::move(b.out));
}

const Rows& Pkt::dmc(Mod m, int short_us, int long_us, int reset_us, int tol_us) {
    const Key k{6, static_cast<int>(m), short_us, long_us, reset_us, tol_us, 0, 0};
    if (const Rows* r = find(k)) return *r;
    const int s_short = us2s(short_us), s_long = us2s(long_us), s_reset = us2s(reset_us);
    const int s_tol = tol_us > 0 ? us2s(tol_us) : std::max(2, s_long / 4);
    const auto& ps = pulses(m);
    const size_t ns = ps.size() * 2;
    Builder b;
    for (size_t n = 0; n < ns; n++) {
        int sy = symbol(ps, n);
        const int64_t t = symbol_t(ps, n);
        if (std::abs(sy - s_short) < s_tol) {
            b.add_bit(1, t, t + sy);
            sy = n + 1 < ns ? symbol(ps, ++n) : 0;
            if (std::abs(sy - s_short) > s_tol) {
                if (sy >= s_reset - s_tol) n--;
                else if (b.any_rows() && !b.cur_empty()) b.add_row();
            } else if (b.any_rows()) {
                b.rows.back().t1 = symbol_t(ps, n) + sy;
            }
        } else if (std::abs(sy - s_long) < s_tol) {
            b.add_bit(0, t, t + sy);
        } else if (sy >= s_reset - s_tol && b.any_rows()) {
            b.end_msg();
        }
    }
    return put(k, std::move(b.out));
}

const Rows& Pkt::piwm(Mod m, int short_us, int long_us, int reset_us, int tol_us) {
    const Key k{7, static_cast<int>(m), short_us, long_us, reset_us, tol_us, 0, 0};
    if (const Rows* r = find(k)) return *r;
    const int s_short = us2s(short_us), s_long = us2s(long_us), s_reset = us2s(reset_us);
    const int s_tol = tol_us > 0 ? us2s(tol_us) : std::max(2, s_long / 4);
    const auto& ps = pulses(m);
    const size_t ns = ps.size() * 2;
    Builder b;
    for (size_t n = 0; n < ns; n++) {
        const int sy = symbol(ps, n);
        const int64_t t = symbol_t(ps, n);
        if (std::abs(sy - s_short) < s_tol) b.add_bit(1, t, t + sy);
        else if (std::abs(sy - s_long) < s_tol) b.add_bit(0, t, t + sy);
        else if (sy < s_reset && b.any_rows() && !b.cur_empty()) b.add_row();
        if ((n + 1 == ns || sy > s_reset) && b.any_rows()) b.end_msg();
    }
    return put(k, std::move(b.out));
}

// -------------------------------------------------------------- messages ---
std::string Msg::json() const {
    auto esc = [](const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                o += '\\';
                o += static_cast<char>(c);
            } else if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
        return o;
    };
    std::string s = "{\"model\":\"" + esc(model) + "\"";
    if (!id.empty()) {
        // numeric ids as numbers, like rtl_433
        const bool num = id.size() < 16 && id.find_first_not_of("0123456789") == std::string::npos &&
                         (id.size() == 1 || id[0] != '0');
        s += ",\"id\":" + (num ? id : "\"" + esc(id) + "\"");
    }
    for (const auto& v : f) {
        if (v.key == "id") continue;
        s += ",\"" + esc(v.key) + "\":";
        if (v.is_num) {
            if (!std::isfinite(v.num)) {
                s += "null";
                continue;
            }
            char b[48];
            snprintf(b, sizeof b, v.fmt, v.num);
            s += b;
        } else {
            s += "\"" + esc(v.str) + "\"";
        }
    }
    if (check && *check) s += std::string(",\"mic\":\"") + check + "\"";
    return s + "}";
}

void dbg_rows(const char* name, const Rows& rows) {
    static const char* const want = getenv("ISM_ROWS");
    if (!want || std::string(want) != name) return;
    std::string s = std::string("[") + name + "]";
    for (const auto& r : rows.r) s += " m" + std::to_string(r.msg) + "{" + std::to_string(r.n) + "}" + r.hex();
    fprintf(stderr, "%s\n", s.c_str());
}

std::vector<Protocol>& registry() {
    static std::vector<Protocol> r;
    return r;
}

}  // namespace ism
