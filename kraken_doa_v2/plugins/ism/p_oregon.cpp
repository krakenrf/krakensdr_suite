// Oregon Scientific weather sensors, protocol versions 2.1 and 3:
//   v2.1: THGR122N / THGR228N / THGR968, THN132N / THR228N / AWR129, THN129 /
//         RTHN129, RTGN318 / RTGN129 / RTGR328N, THGR328N, BTHR918 / BHTR968 /
//         BTHGN129 (barometer), WGR968 (wind), RGR968 (rain), UVR128 (UV)
//   v3:   THGR810 / THGN801, THN802, UV800 (UVN800), PCR800 (rain), WGR800 (wind)
// plus the version 1 thermometers ("Oregon-v1") and the Oregon SL109H
// thermo-hygro sensor (a different, PPM protocol).
//
// protocol notes: rtl_433 documentation; own implementation.
//
// v2.1 / v3: 433.92 MHz OOK Manchester, ~1024 bit/s (half bit ~488 us).
// Nibbles are sent LSB first; n[0..3] = sensor id, n4 channel, n5 n6 rolling
// id (id = n6 n5), n7 flags (bit 2 = battery low), then the readings, then
// an 8-bit sum of the nibbles before it, low nibble first. v2.1 sends every
// bit twice (inverted copy first) after a 16-bit preamble and the sync
// nibble 0xA; v3 sends single bits after a run of zeros (as sliced) + the
// sync nibble.

#include "ism.hpp"

#include <array>

namespace ism {
namespace {

using Nib = std::array<int, 48>;

// nibbles from payload bits (each nibble LSB first)
int nibbles(const Row& p, int pos, Nib& n) {
    const int cnt = std::min(48, (p.n - pos) / 4);
    for (int j = 0; j < cnt; j++) {
        const int o = pos + 4 * j;
        n[static_cast<size_t>(j)] = p.bit(o) | (p.bit(o + 1) << 1) | (p.bit(o + 2) << 2) | (p.bit(o + 3) << 3);
    }
    return cnt;
}

inline int N(const Nib& n, int i) { return n[static_cast<size_t>(i)]; }

// checksum at index c: sum of n[0..c-1] (mod 256) = n[c] + 16 n[c+1]
bool sum_ok(const Nib& n, int cnt, int c) {
    if (cnt < c + 2) return false;
    int s = 0;
    for (int i = 0; i < c; i++) s += N(n, i);
    return (s & 0xFF) == N(n, c) + 16 * N(n, c + 1);
}

double temp_of(const Nib& n) {
    const double t = N(n, 10) * 10 + N(n, 9) + N(n, 8) * 0.1 + (N(n, 11) & 7) * 100;
    return (N(n, 11) & 8) ? -t : t;
}
inline int hum_of(const Nib& n) { return N(n, 13) * 10 + N(n, 12); }

Msg& head(std::vector<Msg>& out, const Row& r, const char* model, const Nib& n, bool channel = true) {
    Msg& m = emit(out, r, model, Kind::WEATHER, "CHECKSUM");
    const int id = N(n, 6) * 16 + N(n, 5);
    m.id = std::to_string(id);
    m.num("id", id);
    if (channel) m.num("channel", N(n, 4));
    m.num("battery_ok", (N(n, 7) >> 2) & 1 ? 0 : 1);
    return m;
}

void th(Msg& m, const Nib& n, bool hum = true) {
    m.num("temperature_C", temp_of(n), "%.1f");
    if (hum) m.num("humidity", hum_of(n));
}

// ------------------------------------------------------------------ v2.1 ---
// bits: the decoded payload length (pairs after the sync); the frame
// length fixes the model where one sensor id is shared. A few extra pairs
// past the frame (the row running on) are tolerated.
bool v21(std::vector<Msg>& out, const Row& r, const Nib& n, int cnt, int bits) {
    const int id = (N(n, 0) << 12) | (N(n, 1) << 8) | (N(n, 2) << 4) | N(n, 3);
    auto len_is = [&](int L) { return bits >= L && bits <= L + 6; };
    if (id == 0x1D20 || id == 0x1D30) {
        if (!len_is(68) && !len_is(76)) return false;
        if (!sum_ok(n, cnt, 15)) return false;
        const char* model = id == 0x1D30 ? "Oregon-THGR968" : bits >= 76 ? "Oregon-THGR122N" : "Oregon-THGR228N";
        th(head(out, r, model, n), n);
        return true;
    }
    if (id == 0x3D00) {
        if (bits < 94 || !sum_ok(n, cnt, 17)) return false;
        Msg& m = head(out, r, "Oregon-WGR968", n);
        m.num("wind_max_m_s", N(n, 12) + N(n, 11) * 0.1 + N(n, 13) * 0.1, "%.1f");
        m.num("wind_avg_m_s", N(n, 15) + N(n, 14) * 0.1 + N(n, 16) * 0.1, "%.1f");
        m.num("wind_dir_deg", N(n, 10) * 100 + N(n, 9) * 10 + N(n, 8));
        return true;
    }
    if (id == 0x5D60 || id == 0x5D50 || id == 0x5D53) {
        if (bits < (id == 0x5D50 ? 84 : 92) || !sum_ok(n, cnt, 19)) return false;
        const int p = N(n, 16) * 16 + N(n, 15);
        const double hpa = id == 0x5D60 ? p + 856 : id == 0x5D50 ? p + 795 : 2 * p + (N(n, 17) & 1) + 600;
        Msg& m = head(out, r, id == 0x5D60 ? "Oregon-BHTR968" : id == 0x5D50 ? "Oregon-BTHR918" : "Oregon-BTHGN129", n);
        th(m, n);
        m.num("pressure_hPa", hpa, "%.0f");
        return true;
    }
    if (id == 0x2D10) {
        if (bits < 80 || !sum_ok(n, cnt, 16)) return false;
        Msg& m = head(out, r, "Oregon-RGR968", n);
        m.num("rain_rate_mm_h", (N(n, 9) * 100 + N(n, 8) * 10 + N(n, 10)) * 0.1, "%.1f");
        m.num("rain_mm", (N(n, 15) * 10000 + N(n, 14) * 1000 + N(n, 13) * 100 + N(n, 12) * 10 + N(n, 11)) * 0.1, "%.1f");
        return true;
    }
    if (id == 0xEC40 || id == 0xEC41) {
        // 64-bit frame: THN132N; 76-bit: THR228N / AWR129 (same checksum spot)
        if (bits < 64 || !sum_ok(n, cnt, 12)) return false;
        if (bits < 76 || bits > 82 || id == 0xEC40) {
            if (id != 0xEC40 && bits < 76) return false;
        }
        const bool long_frame = bits >= 76 && bits <= 82;
        if (!long_frame) {
            if (id != 0xEC40) return false;
            if (N(n, 8) > 9 || N(n, 9) > 9 || N(n, 10) > 9) return false;
            const double t = temp_of(n);
            if (t < -50 || t > 70) return false;
            th(head(out, r, "Oregon-THN132N", n), n, false);
            return true;
        }
        th(head(out, r, id == 0xEC41 ? "Oregon-AWR129" : "Oregon-THR228N", n), n, false);
        return true;
    }
    if ((id & 0x0FFF) == 0x0CC3) {
        if (len_is(80) && sum_ok(n, cnt, 15)) {
            th(head(out, r, "Oregon-RTGN129", n), n);
            return true;
        }
        if (bits >= 173 && (id == 0xDCC3 || id == 0xCCC3 || id == 0xBCC3 || id == 0xACC3 || id == 0x9CC3) &&
            sum_ok(n, cnt, 15)) {
            th(head(out, r, "Oregon-RTGR328N", n), n);
            return true;
        }
        if (len_is(76) && sum_ok(n, cnt, 15)) {
            th(head(out, r, "Oregon-RTGN318", n), n);
            return true;
        }
        return false;   // 100-bit radio-clock frames: no reading
    }
    if (id == 0x8CE3 || id == 0x8AE3) {
        if (bits < 100 || !sum_ok(n, cnt, 21)) return false;
        Msg& m = head(out, r, "Oregon-RTGR328N", n);
        char b[40];
        snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", 2000 + N(n, 19) * 10 + N(n, 18), N(n, 16),
                 N(n, 15) * 10 + N(n, 14), N(n, 13) * 10 + N(n, 12), N(n, 11) * 10 + N(n, 10), N(n, 9) * 10 + N(n, 8));
        m.kind = Kind::OTHER;
        m.str("radio_clock", b);
        return true;
    }
    if (id == 0xCC43 || (id & 0x0FFF) == 0x0CD3) {
        if (!len_is(68) || !sum_ok(n, cnt, 12)) return false;
        th(head(out, r, id == 0xCC43 ? "Oregon-THN129" : "Oregon-RTHN129", n), n, false);
        return true;
    }
    if (id == 0xEC70) {
        // two copies in one row: 64 bits, preamble, sync, 64 bits
        if (bits < 140 || !sum_ok(n, cnt, 12)) return false;
        if (N(n, 8) > 9 || N(n, 9) > 9) return false;
        const int uvi = N(n, 9) * 10 + N(n, 8);
        if (uvi > 25) return false;
        Msg& m = head(out, r, "Oregon-UVR128", n, false);
        // rtl_433 order: id, uvi, battery_ok
        m.f.pop_back();
        m.num("uvi", uvi, "%.1f").num("battery_ok", (N(n, 7) >> 2) & 1 ? 0 : 1);
        return true;
    }
    if (id == 0xCC23) {
        if (bits < 173 || !sum_ok(n, cnt, 15)) return false;
        th(head(out, r, "Oregon-THGR328N", n), n);
        return true;
    }
    return false;
}

// -------------------------------------------------------------------- v3 ---
bool v3(std::vector<Msg>& out, const Row& r, const Nib& n, int cnt) {
    const int id = (N(n, 0) << 12) | (N(n, 1) << 8) | (N(n, 2) << 4) | N(n, 3);
    if ((id & 0xF0FF) == 0xF024 || id == 0xF8B4) {
        if (!sum_ok(n, cnt, 15)) return false;
        for (int i : {8, 9, 10, 12, 13})
            if (N(n, i) > 9) return false;
        const double t = temp_of(n);
        if (t < -50 || t > 70) return false;
        Msg& m = head(out, r, "Oregon-THGR810", n);
        th(m, n);
        if (N(n, 1) & 1) m.num("button", 1);
        return true;
    }
    if (id == 0xC844) {
        if (!sum_ok(n, cnt, 12)) return false;
        th(head(out, r, "Oregon-THN802", n), n, false);
        return true;
    }
    if (id == 0xD874) {
        if (!sum_ok(n, cnt, 13)) return false;
        head(out, r, "Oregon-UV800", n).num("uvi", N(n, 9) * 10 + N(n, 8));
        return true;
    }
    if (id == 0x2914 || id == 0x2D14) {
        if (!sum_ok(n, cnt, 18)) return false;
        if (id == 0x2914)
            for (int i = 8; i <= 17; i++)
                if (N(n, i) > 9) return false;
        Msg& m = head(out, r, id == 0x2914 ? "Oregon-PCR800" : "Oregon-PCR800a", n);
        m.num("rain_rate_in_h", (N(n, 11) * 1000 + N(n, 10) * 100 + N(n, 9) * 10 + N(n, 8)) * 0.01, "%.2f");
        m.num("rain_in", (N(n, 17) * 100000 + N(n, 16) * 10000 + N(n, 15) * 1000 + N(n, 14) * 100 + N(n, 13) * 10 + N(n, 12)) * 0.001, "%.3f");
        return true;
    }
    if (id == 0x1984 || id == 0x1994) {
        if (!sum_ok(n, cnt, 17)) return false;
        for (int i = 11; i <= 16; i++)
            if (N(n, i) > 9) return false;
        const double gust = N(n, 13) * 10 + N(n, 12) + N(n, 11) * 0.1;
        const double avg = N(n, 16) * 10 + N(n, 15) + N(n, 14) * 0.1;
        if (gust > 56 || avg > 56) return false;
        Msg& m = head(out, r, "Oregon-WGR800", n);
        m.num("wind_max_m_s", gust, "%.1f").num("wind_avg_m_s", avg, "%.1f").num("wind_dir_deg", N(n, 8) * 22.5, "%.1f");
        return true;
    }
    return false;
}

void decode_oregon(Pkt& pk, std::vector<Msg>& out) {
    // the half bit is 488 us; the received pulses run short and the gaps
    // long, by up to ~180 us, so the data-edge threshold (1.5 half bits) is
    // set from the nominal half bit
    const Rows& rs = pk.manchester_zerobit(Mod::OOK, 488, 2400);
    for (const auto& r : rs.r) {
        if (r.n < 60) continue;
        bool done = false;
        // v2.1: (the end of) the doubled preamble 01 01 .. or 10 10 .., then
        // the sync nibble as pairs 10 01 10 01 (0x99)
        for (const uint64_t pat : {0x5599ull, 0xAA99ull}) {
            for (int p = r.find(pat, 16, 8); p >= 0 && !done; p = r.find(pat, 16, p + 1)) {
                if (r.get(p - 8, 8) != (pat >> 8)) continue;
                const Row pay = r.manchester(p + 16, 173);
                if (pay.n < 64) continue;
                Nib n{};
                const int cnt = nibbles(pay, 0, n);
                if (v21(out, r, n, cnt, pay.n)) done = true;
            }
            if (done) break;
        }
        if (done) continue;
        // v3: a run of zeros, the sync 0101 (as 101 after it); also the
        // inverted form
        for (const uint64_t pat : {0x0005ull, 0xFFF5ull}) {
            for (int p = r.find(pat, 16); p >= 0 && !done; p = r.find(pat, 16, p + 1)) {
                const int s = p + 16;
                if (r.n - s < 56) break;
                Nib n{};
                const int cnt = nibbles(r, s, n);
                if (v3(out, r, n, cnt)) done = true;
            }
            if (done) break;
        }
    }
}
ISM_PROTOCOL("Oregon Scientific v2.1/v3", Strength::MEDIUM, decode_oregon);

// -------------------------------------------------------------------- v1 ---
// OOK Manchester, 2930 us per bit (carrier on in the first half = 1): 12
// preamble pulses, a ~5.8 ms sync pulse + ~5.2 ms gap (longer when the first
// bit is 0, whose off half joins it), 32 bits = 4 bytes LSB first: id /
// channel, temperature (3 BCD digits) + sign / battery, and the sum of the
// first three bytes (carry added back in).
void decode_osv1(Pkt& pk, std::vector<Msg>& out) {
    const auto& ps = pk.pulses(Mod::OOK);
    const int half = us2s(1465), sync_min = us2s(4395), reset = us2s(14000);
    for (size_t i = 6; i + 8 < ps.size(); i++) {
        const Pulse& sy = ps[i];
        if (sy.on < sync_min || sy.off < sync_min || sy.on > 3 * sync_min || sy.off > 3 * sync_min) continue;
        // a preamble before it: pulse + gap ~ one bit each
        int pre = 0;
        for (size_t k = i; k-- > 0 && pre < 12;) {
            const int per = ps[k].on + ps[k].off;
            if (ps[k].on < half / 2 || per < 3 * half / 2 || per > 5 * half) break;   // the last gap is ~4.2 ms
            pre++;
        }
        if (pre < 6) continue;
        // Manchester from the edges: a mid-bit edge comes > 1.5 half bits
        // after the previous one; falling = 1, rising = 0
        uint32_t bits = 0;
        int nb = 0;
        int since;
        if (sy.off > sy.on) {   // first bit 0: its mid-bit edge ends the gap
            nb = 1;
            since = 0;
        } else {
            since = half;   // the gap's end starts the first bit: half a bit after a (virtual) mid-bit edge
        }
        bool ok = true;
        for (size_t k = i + 1; k < ps.size() && nb < 32; k++) {
            const Pulse& p = ps[k];
            since += p.on;
            if (since > 3 * half / 2) {
                if (since > 3 * half) { ok = false; break; }
                bits |= 1u << nb;   // falling edge
                nb++;
                since = 0;
            }
            if (nb >= 32) break;
            if (p.off > reset) break;
            since += p.off;
            if (since > 3 * half / 2) {
                if (since > 3 * half) { ok = false; break; }
                nb++;   // rising edge: 0
                since = 0;
            }
        }
        if (!ok || nb < 32) continue;
        uint8_t b[4];
        for (int k = 0; k < 4; k++) b[k] = static_cast<uint8_t>(bits >> (8 * k));
        if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF) continue;
        const int s = b[0] + b[1] + b[2];
        if (b[3] == 0 || (b[3] != (s & 0xFF) + (s >> 8) && b[3] != ((s > 0x180 ? s + 1 : s) & 0xFF))) continue;
        const int n2 = b[1] & 15, n3 = b[1] >> 4, n4 = b[2] & 15, n5 = b[2] >> 4;
        double t = n4 * 10 + n3 + n2 * 0.1;
        if (n5 & 2) t = -t;
        Row r;
        r.t0 = ps[i].t;
        r.t1 = ps[std::min(ps.size() - 1, i + 33)].t;
        Msg& m = emit(out, r, "Oregon-v1", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(b[0] & 15);
        m.num("id", b[0] & 15).num("channel", ((b[0] >> 6) & 3) + 1).num("battery_ok", (n5 & 8) ? 0 : 1);
        m.num("temperature_C", t, "%.1f");
        return;
    }
}
ISM_PROTOCOL("Oregon Scientific v1", Strength::MEDIUM, decode_osv1);

// ---------------------------------------------------------------- SL109H ---
// OOK PPM, ~2 ms gap = 0, ~4 ms = 1, 38-bit frames repeated: checksum
// nibble, channel code, humidity (2 BCD digits), 12-bit temperature (0.1 C,
// two's complement), status, 8-bit id. Checksum = (channel code + the eight
// nibbles from bit 6 on) & 0xF.
void decode_sl109h(Pkt& pk, std::vector<Msg>& out) {
    // a frame counts when it occurs twice within one message
    for (const Rows& rs : pk.ppm(Mod::OOK, 2000, 4000, 5000, 10000).split()) {
        for (size_t i = 0; i < rs.size(); i++) {
            const Row& r = rs[i];
            if (r.n != 38 || rs.count_same(i) < 2) continue;
            if (r.get(0, 32) == 0) continue;
            const int a = static_cast<int>(r.get(0, 4)), c = static_cast<int>(r.get(4, 2));
            int s = c;
            for (int k = 0; k < 8; k++) s += static_cast<int>(r.get(6 + 4 * k, 4));
            if ((s & 15) != a) continue;
            const int h10 = static_cast<int>(r.get(6, 4)), h1 = static_cast<int>(r.get(10, 4));
            int t = static_cast<int>(r.get(14, 12));
            if (t & 0x800) t -= 0x1000;
            if (a == 0 && c == 0 && h10 == 0 && h1 == 0 && t == 0) continue;
            if (c == 3 || h10 > 9 || h1 > 9) continue;
            const double tc = t * 0.1;
            if (tc < -20 || tc > 60) continue;
            const int ch = c == 0 ? 3 : c;
            const int id = static_cast<int>(r.get(30, 8));
            Msg& m = emit(out, r, "Oregon-SL109H", Kind::WEATHER, "CHECKSUM");
            for (const auto& o : rs.r)
                if (o.same(r)) {
                    m.t0 = std::min(m.t0, o.t0);
                    m.t1 = std::max(m.t1, o.t1);
                }
            m.id = std::to_string(id);
            m.num("id", id).num("channel", ch).num("temperature_C", tc, "%.1f").num("humidity", h10 * 10 + h1);
            m.num("status", static_cast<int>(r.get(26, 4)));
            break;
        }
    }
}
ISM_PROTOCOL("Oregon SL109H", Strength::MEDIUM, decode_sl109h);

}  // namespace
}  // namespace ism
