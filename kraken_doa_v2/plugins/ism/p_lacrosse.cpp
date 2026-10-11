// LaCrosse TX141 family: TX141TH-Bv2 (also TFA 30.3221.02 / 30.3249.02, ORIA
// WA50B), TX141-Bv2, TX141-Bv3 (TX141-Bv4, TFA 30.3243.02), TX141B and the
// TX141W / TX145wsdth "Breeze" (TFA 30.3222.02 / 30.3251.10).
//
// protocol notes: rtl_433 documentation; own implementation.
//
// Framing (all members): OOK PWM, bit period ~625 us (TX141W ~730 us); every
// packet starts with 4 sync pulses (~750-990 us on and off) and the packet
// is sent 4..12 times back to back. After complementing the sliced bits a
// long pulse is 1. The syncs break the rows, so one row = one packet.
//
// Checks: TX141TH = 8-bit LFSR digest (reflected, gen 0x31, key 0xF4) over
// the first 4 bytes; TX141W = CRC-8 poly 0x31 over 8 bytes (residue 0) plus a
// fixed 5-bit preamble. TX141-Bv2 / -Bv3 / TX141B have no check that is
// known: they are accepted on identical repeats only (WEAK).

#include "ism.hpp"

#include <algorithm>

namespace ism {
namespace {

const Rows& tx141_rows(Pkt& pk) { return pk.pwm(Mod::OOK, 208, 417, 625, 1700, 833, 0); }

// rows of one message, complemented
std::vector<Row> invert_all(const Rows& m) {
    std::vector<Row> v;
    v.reserve(m.size());
    for (const auto& r : m.r) v.push_back(r.inverted());
    return v;
}

bool same_prefix(const Row& a, const Row& b, int nbits) {
    if (a.n < nbits || b.n < nbits) return false;
    for (int i = 0; i < nbits; i += 8) {
        const int k = std::min(8, nbits - i);
        if (a.get(i, k) != b.get(i, k)) return false;
    }
    return true;
}

std::string yes_no(int v) { return v ? "Yes" : "No"; }

// TX141TH-Bv2: 40 bits (41 on the -Bv3 hardware revision)
//   id:8 | batt_low:1 test:1 channel:2 temp:12 (C*10 + 500) | hum:8 | digest:8
void decode_tx141th(const std::vector<Row>& rows, std::vector<Msg>& out) {
    int best = -1, best_count = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        if (r.n != 40 && r.n != 41) continue;
        const auto b = r.bytes(0, 5);
        if (lfsr_digest8_reflect(b.data(), 4, 0x31, 0xF4) != b[4]) continue;
        const int id = b[0];
        const int hum = b[3];
        const double t = ((((b[1] & 0x0F) << 8) | b[2]) - 500) * 0.1;
        if (id == 0 || hum == 0 || hum > 100 || t < -40.0 || t > 140.0) continue;
        int c = 0;
        for (const auto& x : rows)
            if (same_prefix(r, x, 40)) c++;
        if (c > best_count) {
            best = static_cast<int>(i);
            best_count = c;
        }
    }
    // repeats, or a single CRC-valid row of a short transmission
    if (best < 0 || (best_count < 2 && rows.size() > 4)) return;
    const Row& r = rows[static_cast<size_t>(best)];
    if (r.n == 41 && rows.size() > 13) return;   // -Bv3 rows only come in short bursts
    const auto b = r.bytes(0, 5);
    const int test = (b[1] >> 6) & 1;
    Msg& m = emit(out, r, "LaCrosse-TX141THBv2", Kind::WEATHER, "CRC");
    for (const auto& x : rows)
        if (same_prefix(r, x, 40)) {
            m.t0 = std::min(m.t0, x.t0);
            m.t1 = std::max(m.t1, x.t1);
        }
    m.id = std::to_string(b[0]);
    m.num("channel", (b[1] >> 4) & 3);
    m.num("battery_ok", (b[1] & 0x80) ? 0 : 1);
    m.num("temperature_C", ((((b[1] & 0x0F) << 8) | b[2]) - 500) * 0.1, "%.1f");
    m.num("humidity", b[3]);
    m.str("test", yes_no(test));
    if (test) m.event = "test button";
}

// TX141W / TX145wsdth: 65 bits
//   pre:5 (00001) id:19 | batt_low:1 test:1 channel:2 type:4 | v1:12 v2:12 | crc:8 | 1 spare
//   type 1: temp (C*10 + 500), humidity; type 2: wind km/h*10, direction deg
void decode_tx141w(const std::vector<Row>& rows, std::vector<Msg>& out) {
    for (const auto& r : rows) {
        if (r.n < 64 || r.n > 66) continue;
        const auto b = r.bytes(0, 8);
        if ((b[0] >> 3) != 0x01) continue;
        if (crc8(b.data(), 8, 0x31, 0x00) != 0) continue;
        const int type = b[3] & 0x0F;
        if (type != 1 && type != 2) continue;
        const int id = ((b[0] & 0x07) << 16) | (b[1] << 8) | b[2];
        const int v1 = (b[4] << 4) | (b[5] >> 4);
        const int v2 = ((b[5] & 0x0F) << 8) | b[6];
        const int test = (b[3] >> 6) & 1;
        Msg& m = emit(out, r, "LaCrosse-TX141W", Kind::WEATHER, "CRC");
        for (const auto& x : rows)
            if (x.same(r)) {
                m.t0 = std::min(m.t0, x.t0);
                m.t1 = std::max(m.t1, x.t1);
            }
        m.id = std::to_string(id);
        m.num("channel", (b[3] >> 4) & 3);
        m.num("battery_ok", (b[3] & 0x80) ? 0 : 1);
        if (type == 1) {
            m.num("temperature_C", (v1 - 500) * 0.1, "%.1f");
            m.num("humidity", v2);
        } else {
            m.num("wind_avg_km_h", v1 * 0.1, "%.1f");
            m.num("wind_dir_deg", v2);
        }
        m.num("test", test);
        if (test) m.event = "test button";
        return;   // one message per rtl_433 message
    }
}

void decode_strong(Pkt& pk, std::vector<Msg>& out) {
    const Rows& all = tx141_rows(pk);
    if (all.empty()) return;
    for (const auto& msg : all.split()) {
        if (msg.size() < 1) continue;
        bool any = false;
        for (const auto& r : msg.r)
            if (r.n >= 40) any = true;
        if (!any) continue;
        const auto rows = invert_all(msg);
        decode_tx141th(rows, out);
        decode_tx141w(rows, out);
    }
}

// TX141-Bv2 (37 bits), TX141-Bv3 (33 bits), TX141B (32 bits): no check
//   id:8 | batt_ok:1 test:1 channel:2 temp:12 (C*10 + 500) | unknown...
void decode_weak(Pkt& pk, std::vector<Msg>& out) {
    const Rows& all = tx141_rows(pk);
    if (all.empty()) return;
    for (const auto& msg : all.split()) {
        if (msg.size() < 3) continue;
        const auto rows = invert_all(msg);
        Rows rs;
        rs.r = rows;
        const int need = rows.size() > 5 ? 5 : 3;
        const int ri = rs.repeated(need, 32);
        if (ri < 0) continue;
        const Row& r = rows[static_cast<size_t>(ri)];
        if (r.n > 39) continue;
        const auto b = r.bytes(0, 3);
        const int id = b[0];
        const double t = ((((b[1] & 0x0F) << 8) | b[2]) - 500) * 0.1;
        if (id == 0 || t < -40.0 || t > 140.0) continue;
        const char* model = r.n >= 37 ? "LaCrosse-TX141Bv2" : r.n == 32 ? "LaCrosse-TX141B" : "LaCrosse-TX141Bv3";
        const int test = (b[1] >> 6) & 1;
        Msg& m = emit(out, r, model, Kind::WEATHER, "REPEAT");
        for (const auto& x : rows)
            if (x.same(r)) {
                m.t0 = std::min(m.t0, x.t0);
                m.t1 = std::max(m.t1, x.t1);
            }
        m.id = std::to_string(id);
        if (r.n != 32) m.num("channel", (b[1] >> 4) & 3);
        m.num("battery_ok", (b[1] & 0x80) ? 1 : 0);   // inverse of the TH's flag
        m.num("temperature_C", t, "%.1f");
        m.str("test", yes_no(test));
        if (test) m.event = "test button";
    }
}

}  // namespace
ISM_PROTOCOL("LaCrosse TX141TH-Bv2 / TX141W / TX145wsdth", Strength::STRONG, decode_strong);
ISM_PROTOCOL("LaCrosse TX141-Bv2 / -Bv3 / TX141B", Strength::WEAK, decode_weak);
}  // namespace ism
