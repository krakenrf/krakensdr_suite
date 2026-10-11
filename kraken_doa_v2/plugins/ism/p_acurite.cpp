// Acurite sensors:
//   "TXR" family (one framing, a message type in byte 2): 592TXR / 592TX /
//   06002RM tower, 5-n-1 (VN1TXC, 06004RM, 06014RM), 3-n-1 (Notos), 06045M
//   lightning, Atlas 7-n-1 (with / without lightning), 515 fridge / freezer,
//   899 rain, 1190 / 1192 leak, Optimus 6-in-1 (06188M);
//   609TXC, 606TX / Technoline TX960, 986 and 985 fridge / freezer, 590TX,
//   00275RM / 00276RM room monitor.
//
// protocol notes: rtl_433 documentation; own implementation.

#include "ism.hpp"

#include <algorithm>
#include <cstdlib>

namespace ism {
namespace {

void widen(Msg& m, const Row& r) {
    m.t0 = std::min(m.t0, r.t0);
    m.t1 = std::max(m.t1, r.t1);
}

std::string bytes_hex(const std::vector<uint8_t>& b, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && i < b.size(); i++) s += hexs(b[i], 2);
    return s;
}

int s12(int v) { return (v & 0x800) ? v - 0x1000 : v; }

// ---------------------------------------------------------------- TXR ---
// OOK PWM: 220 us pulse (+392 gap) / 408 us pulse (+204 gap), after
// complementing a long pulse = 1; 4 sync pulses (620 us) before each copy,
// 3 copies. Bytes: CCxxxxxx IIIIIIII pBMMMMMM ... K: channel (11 A, 10 B, 00 C),
// id, an even parity bit in the MSB of every byte from byte 2 on, battery
// (1 = OK), the 6-bit message type and a byte sum as the last byte. rtl_433
// checks the parity only as the XOR over all those bytes; so does this.

const char* chan_letter(uint8_t b0) {
    static const char* const L[4] = {"C", "", "B", "A"};
    return L[b0 >> 6];
}

// 5-n-1 / Optimus wind direction code -> degrees
const double WIND_DIR_5N1[16] = {315.0, 247.5, 292.5, 270.0, 337.5, 225.0, 0.0,   202.5,
                                 67.5,  135.0, 90.0,  112.5, 45.0,  157.5, 22.5,  180.0};

int txr_len(int type) {
    switch (type) {
    case 0x04: case 0x01: return 7;
    case 0x2F: return 9;
    case 0x08: case 0x09: return 6;
    case 0x31: case 0x38: case 0x20: case 0x30: return 8;
    case 0x05: case 0x06: case 0x07: return 8;
    case 0x25: case 0x26: case 0x27: return 10;
    case 0x3A: case 0x3B: return 10;
    default: return 0;
    }
}

// one row -> at most one message; false = not valid
bool txr_row(const Row& r, std::vector<Msg>& out) {
    const int nbytes = r.n / 8;
    if (nbytes < 6 || nbytes > 10) return false;
    const auto b = r.bytes(0, nbytes);
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[static_cast<size_t>(nbytes - 1)] == 0) return false;
    const int type = b[2] & 0x3F;
    const int E = txr_len(type);
    if (E == 0 || nbytes < E) return false;
    if ((add_bytes(b.data(), E - 1) & 0xFF) != b[static_cast<size_t>(E - 1)]) return false;
    if (type != 0x20 && parity_bytes(b.data() + 2, E - 3) != 0) return false;
    const char* ch = chan_letter(b[0]);
    if (!*ch) return false;   // 01 is no channel
    const int batt = (b[2] >> 6) & 1;
    const int id14 = ((b[0] & 0x3F) << 8) | b[1];

    if (type == 0x04) {   // tower
        const int hum = b[3] & 0x7F;
        const int t = ((b[4] & 0x7F) << 7) | (b[5] & 0x7F);
        const double tc = (t - 1000) * 0.1;
        if ((hum > 100 && hum != 127) || tc < -40.0 || tc > 70.0) return false;
        Msg& m = emit(out, r, "Acurite-Tower", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(id14);
        m.str("channel", ch);
        m.num("battery_ok", batt);
        m.num("temperature_C", tc, "%.1f");
        if (hum != 127) m.num("humidity", hum);
        if (t & 0x3800) {
            m.num("exception", 1);
            m.str("raw_msg", bytes_hex(b, 7));
        }
        return true;
    }
    if (type == 0x01) {   // leak detector
        const int wet = (b[3] >> 4) & 1;
        Msg& m = emit(out, r, "Acurite-Leak", Kind::SECURITY, "CHECKSUM");
        m.id = std::to_string(id14);
        m.str("channel", ch);
        m.num("battery_ok", batt);
        m.num("leak_detected", wet);
        if (wet) {
            m.event = "leak detected";
            m.alarm = true;
        }
        return true;
    }
    if (type == 0x2F) {   // 06045M lightning
        const int hum = b[3] & 0x7F;
        const int t = ((b[4] & 0x1F) << 7) | (b[5] & 0x7F);
        const double tf = (t - 1480) * 0.1;
        if (hum > 100 || tf < -40.0 || tf > 158.0) return false;
        Msg& m = emit(out, r, "Acurite-6045M", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(id14);
        m.str("channel", ch);
        m.num("battery_ok", batt);
        m.num("temperature_F", tf, "%.1f");
        m.num("humidity", hum);
        m.num("strike_count", ((b[6] & 0x7F) << 1) | ((b[7] >> 6) & 1));
        m.num("storm_dist", b[7] & 0x1F);
        m.num("active", (b[4] >> 6) & 1);
        m.num("rfi", (b[7] >> 5) & 1);
        m.num("exception", (b[4] & 0x20) ? 1 : 0);
        m.str("raw_msg", bytes_hex(b, static_cast<size_t>(std::min(15, (r.n + 7) / 8))));
        return true;
    }
    if (type == 0x08 || type == 0x09) {   // 515 refrigerator / freezer
        const int t = ((b[3] & 0x7F) << 7) | (b[4] & 0x7F);
        const double tf = (t - 1480) * 0.1;
        if (tf < -40.0 || tf > 158.0) return false;
        Msg& m = emit(out, r, "Acurite-515", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(id14);
        m.str("channel", std::string(ch) + (type == 0x08 ? "R" : "F"));
        m.num("battery_ok", batt);
        m.num("temperature_F", tf, "%.1f");
        if (t & 0x3000) {
            m.num("exception", 1);
            m.str("raw_msg", bytes_hex(b, 6));
        }
        return true;
    }
    if (type == 0x31 || type == 0x38) {   // 5-n-1
        const int id = ((b[0] & 0x0F) << 8) | b[1];
        const int w = ((b[3] & 0x1F) << 3) | ((b[4] & 0x70) >> 4);
        const double kmh = w > 0 ? w * 0.8278 + 1.0 : 0.0;
        if (type == 0x38) {
            const int t = ((b[4] & 0x0F) << 7) | (b[5] & 0x7F);
            const double tf = (t - 400) * 0.1;
            const int hum = b[6] & 0x7F;
            if (tf < -40.0 || tf > 158.0 || hum > 100) return false;
            Msg& m = emit(out, r, "Acurite-5n1", Kind::WEATHER, "CHECKSUM");
            m.id = std::to_string(id);
            m.num("message_type", type);
            m.str("channel", ch);
            m.num("sequence_num", (b[0] >> 4) & 3);
            m.num("battery_ok", batt);
            m.num("wind_avg_km_h", kmh, "%.3f");
            m.num("temperature_F", tf, "%.1f");
            m.num("humidity", hum);
        } else {
            Msg& m = emit(out, r, "Acurite-5n1", Kind::WEATHER, "CHECKSUM");
            m.id = std::to_string(id);
            m.num("message_type", type);
            m.str("channel", ch);
            m.num("sequence_num", (b[0] >> 4) & 3);
            m.num("battery_ok", batt);
            m.num("wind_avg_km_h", kmh, "%.3f");
            m.num("wind_dir_deg", WIND_DIR_5N1[b[4] & 0x0F], "%.1f");
            m.num("rain_in", (((b[5] & 0x7F) << 7) | (b[6] & 0x7F)) * 0.01, "%.2f");
        }
        return true;
    }
    if (type == 0x20) {   // 3-n-1
        const int hum = b[3] & 0x7F;
        const int t = ((b[4] & 0x1F) << 7) | (b[5] & 0x7F);
        const double tf = (t - 1480) * 0.1;
        if (hum > 100 || tf < -40.0 || tf > 158.0) return false;
        Msg& m = emit(out, r, "Acurite-3n1", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(id14);
        m.num("message_type", type);
        m.str("channel", ch);
        m.num("sequence_num", (b[0] >> 4) & 3);
        m.num("battery_ok", batt);
        m.num("wind_avg_mi_h", b[6] & 0x7F, "%.1f");
        m.num("temperature_F", tf, "%.1f");
        m.num("humidity", hum);
        return true;
    }
    if (type == 0x30) {   // 899 rain
        Msg& m = emit(out, r, "Acurite-Rain899", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(id14);
        m.num("channel", b[0] >> 6);   // a number here, not a letter
        m.num("battery_ok", batt);
        m.num("rain_mm", (((b[5] & 0x7F) << 7) | (b[6] & 0x7F)) * 0.254, "%.3f");
        return true;
    }
    if (type == 0x3A || type == 0x3B) {   // Optimus 6-in-1
        const int mph = ((b[3] & 0x7F) << 1) | ((b[4] >> 6) & 1);
        if (mph > 200) return false;
        if (type == 0x3B) {
            const int t = ((b[4] & 0x0F) << 7) | (b[5] & 0x7F);
            const double tf = (t - 400) * 0.1;
            const int hum = b[6] & 0x7F;
            if (tf < -40.0 || tf > 158.0 || hum > 100) return false;
        }
        Msg& m = emit(out, r, "Acurite-Optimus", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(((b[0] & 0x0F) << 8) | b[1]);
        m.str("channel", ch);
        m.num("sequence_num", (b[0] >> 4) & 3);
        m.num("battery_ok", batt);
        m.num("wind_avg_mi_h", mph, "%.1f");
        m.num("wind_avg_km_h", mph * 1.609344, "%.3f");
        if (type == 0x3B) {
            m.num("temperature_F", ((((b[4] & 0x0F) << 7) | (b[5] & 0x7F)) - 400) * 0.1, "%.1f");
            m.num("humidity", b[6] & 0x7F);
        } else {
            m.num("wind_dir_deg", WIND_DIR_5N1[b[4] & 0x0F], "%.1f");
            m.num("rain_in", (((b[5] & 0x03) << 7) | (b[6] & 0x7F)) * 0.01, "%.2f");
        }
        m.str("raw_msg", bytes_hex(b, 10));
        return true;
    }
    // Atlas
    const int id10 = ((b[0] & 0x03) << 8) | b[1];
    const int mph = ((b[3] & 0x7F) << 1) | ((b[4] >> 6) & 1);
    if (mph > 200) return false;
    int exception = 0;
    double tf = NAN, dir = NAN;
    int hum = -1, lux = -1;
    if (type == 0x05 || type == 0x25) {
        tf = ((((b[4] & 0x0F) << 7) | (b[5] & 0x7F)) - 400) * 0.1;
        if (b[4] & 0x30) exception++;
        if (tf < -40.0 || tf > 158.0) return false;
        hum = b[6] & 0x7F;
        if (hum > 100) return false;
        if (hum == 0) exception++;
    } else if (type == 0x06 || type == 0x26) {
        dir = ((b[4] & 0x1F) << 5) | ((b[5] & 0x7C) >> 2);
        if (b[4] & 0x30) exception++;
        if (dir > 360) return false;
    } else {
        lux = ((b[5] & 0x7F) << 7) | (b[6] & 0x7F);
        if (lux > 12000) return false;
    }
    Msg& m = emit(out, r, "Acurite-Atlas", Kind::WEATHER, "CHECKSUM");
    m.id = std::to_string(id10);
    m.str("channel", ch);
    m.num("sequence_num", (b[0] >> 2) & 3);
    m.num("battery_ok", batt);
    m.num("message_type", type);
    m.num("wind_avg_mi_h", mph, "%.1f");
    if (std::isfinite(tf)) {
        m.num("temperature_F", tf, "%.1f");
        m.num("humidity", hum);
    }
    if (std::isfinite(dir)) {
        m.num("wind_dir_deg", dir, "%.1f");
        m.num("rain_in", (((b[5] & 0x03) << 7) | (b[6] & 0x7F)) * 0.01, "%.2f");
    }
    if (lux >= 0) {
        m.num("uvi", b[4] & 0x0F);
        m.num("lux", lux * 10);
    }
    if (type >= 0x25) {
        m.num("strike_count", ((b[7] & 0x7F) << 2) | ((b[8] & 0x60) >> 5));
        m.num("strike_distance", b[8] & 0x1F);
    }
    m.num("exception", exception);
    m.str("raw_msg", bytes_hex(b, static_cast<size_t>(std::min(15, (r.n + 7) / 8))));
    return true;
}

void decode_txr(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.pwm(Mod::OOK, 220, 408, 500, 4000, 620, 0);
    for (const auto& x : rows.r) {
        if (x.n < 48 || x.n > 90) continue;
        txr_row(x.inverted(), out);
    }
}

// ------------------------------------------------------------- 609TXC ---
// OOK PPM, 500 us pulses, gap 1000 = 0 / 2000 = 1, a 9 ms gap before the
// 40 data bits. id:8 | status:4 (bit 3 = battery low) temp:12 signed C*10 |
// humidity:8 | sum of the 4 bytes:8
// The ~6 copies, 11 ms apart, are in one burst: with only an 8-bit sum
// two identical copies are required.
void decode_609(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 1000, 2000, 3000, 10000);
    dbg_rows("609", rows);
    for (const auto& r : rows.r) {
        if (r.n != 40) continue;
        const auto b = r.bytes(0, 5);
        const int s = add_bytes(b.data(), 4);
        if (s == 0 || (s & 0xFF) != b[4]) continue;
        const int hum = b[3];
        const double tc = s12(((b[1] & 0x0F) << 8) | b[2]) * 0.1;
        if (hum > 100 || tc < -50.0 || tc > 80.0) continue;
        int copies = 0;
        for (const auto& x : rows.r)
            if (x.n >= 39 && x.n <= 41 && x.get(0, 39) == r.get(0, 39)) copies++;
        if (copies < 2) continue;
        const int status = b[1] >> 4;
        Msg& m = emit(out, r, "Acurite-609TXC", Kind::WEATHER, "CHECKSUM");
        for (const auto& x : rows.r)
            if (x.same(r)) widen(m, x);
        m.id = std::to_string(b[0]);
        m.num("battery_ok", (status & 8) ? 0 : 1);
        m.num("temperature_C", tc, "%.1f");
        m.num("humidity", hum);
        m.num("status", status);
    }
}

// -------------------------------------------------------------- 606TX ---
// OOK PPM, ~430 us pulses, gap 2000 = 0 / 4000 = 1, 9 ms between the 6
// copies. id:8 | batt_ok:1 button:1 channel:2 temp:12 signed C*10 | LFSR
// digest (gen 0x98, key 0xF1) over the 3 bytes
void decode_606(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 2000, 4000, 7000, 10000);
    for (const auto& msg : rows.split()) {
        const int ri = msg.repeated(3, 32);
        if (ri < 0) continue;
        const Row& r = msg[static_cast<size_t>(ri)];
        if (r.n > 33) continue;
        const auto b = r.bytes(0, 4);
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) continue;
        if (lfsr_digest8(b.data(), 3, 0x98, 0xF1) != b[3]) continue;
        Msg& m = emit(out, r, "Acurite-606TX", Kind::WEATHER, "CHECKSUM");
        for (const auto& x : msg.r)
            if (x.same(r)) widen(m, x);
        m.id = std::to_string(b[0]);
        m.num("channel", ((b[1] >> 4) & 3) + 1);
        m.num("battery_ok", (b[1] >> 7) & 1);
        m.num("button", (b[1] >> 6) & 1);
        m.num("temperature_C", s12(((b[1] & 0x0F) << 8) | b[2]) * 0.1, "%.1f");
        if ((b[1] >> 6) & 1) m.event = "button";
    }
}

// ---------------------------------------------------------- 986 / 985 ---
// OOK PPM, bytes sent LSB first. 986: temp(sign+magnitude F) id:16 status
// crc; 985: 2 preamble bytes in front. CRC-8 poly 0x07 over the 4 data bytes
// (in the reversed bytes, LSB-first CRC); the last bit is often lost, so a
// CRC that matches with that bit set to 1 is accepted too.
int sign_mag(uint8_t v) { return (v & 0x80) ? -(v & 0x7F) : v; }

void decode_986(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 520, 880, 1280, 4000);
    for (const auto& r : rows.r) {
        if (r.n < 39 || r.n > 43) continue;
        const auto raw = r.bytes(0, 5);
        if ((raw[0] == 0xFF && raw[1] == 0xFF && raw[2] == 0xFF) || (raw[0] == 0 && raw[1] == 0 && raw[2] == 0)) continue;
        uint8_t br[5];
        for (int i = 0; i < 5; i++) br[i] = reverse8(raw[static_cast<size_t>(i)]);
        const uint8_t c = crc8le(br, 4, 0x07, 0);
        if (c != br[4] && c != (br[4] | 0x80)) continue;
        const int sensor = (br[3] & 1) + 1;
        const int status = br[3] >> 1;
        Msg& m = emit(out, r, "Acurite-986", Kind::WEATHER, "CRC");
        m.id = std::to_string((br[1] << 8) | br[2]);
        m.str("channel", sensor == 2 ? "2F" : "1R");
        m.num("battery_ok", (status & 1) ? 0 : 1);
        m.num("temperature_F", sign_mag(br[0]), "%.0f");
        m.num("status", status);
    }
}

void decode_985(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 556, 1104, 4000, 7636);
    for (const auto& r : rows.r) {
        if (r.n < 55 || r.n > 59) continue;
        const auto raw = r.bytes(0, 7);
        if (raw[2] == 0 && raw[3] == 0 && raw[4] == 0 && raw[5] == 0 && raw[6] == 0) continue;
        uint8_t br[7];
        for (int i = 0; i < 7; i++) br[i] = reverse8(raw[static_cast<size_t>(i)]);
        const uint8_t c = crc8le(br + 2, 4, 0x07, 0);
        if (c != br[6] && c != (br[6] | 0x80)) continue;
        const int tf = sign_mag(br[2]);
        if (!((tf >= -40 && tf <= 104) || tf == 127 || tf == -127)) continue;
        const int status = br[5];
        const int sensor = (status & 1) + 1;
        const int low = sensor == 2 ? (status >> 2) & 1 : (status >> 1) & 1;
        Msg& m = emit(out, r, "Acurite-985", Kind::WEATHER, "CRC");
        m.id = std::to_string((br[3] << 8) | br[4]);
        m.str("channel", sensor == 2 ? "2F" : "1R");
        m.num("battery_ok", low ? 0 : 1);
        m.num("temperature_F", tf, "%.0f");
        m.num("status", status);
    }
}

// -------------------------------------------------------------- 590TX ---
// OOK PPM, 500 us pulses, gap 500 = 0 / 1500 = 1, a 3 ms sync gap, 24 bits
// (+ a trailing 0), 14 copies. id:7 batt_ok:1 | x:2 channel:2 value:12
// signed; value 0..100 = humidity, else temperature (C*10 + 500). Check:
// odd parity over the first 10 bits only, so it needs repeats (WEAK).
void decode_590(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 500, 1500, 2000, 3500);
    for (const auto& msg : rows.split()) {
        const int ri = msg.repeated(3, 25);
        if (ri < 0) continue;
        const Row& r = msg[static_cast<size_t>(ri)];
        if (r.n != 25) continue;
        const auto b = r.bytes(0, 4);
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) continue;
        if (parity8(b[0]) ^ parity8(static_cast<uint8_t>(b[1] >> 6)) ^ 1) continue;   // odd parity wanted
        const int v = s12(((b[1] & 0x0F) << 8) | b[2]);
        Msg& m = emit(out, r, "Acurite-590TX", Kind::WEATHER, "PARITY");
        for (const auto& x : msg.r)
            if (x.same(r)) widen(m, x);
        m.id = std::to_string(b[0] & 0xFE);
        m.num("channel", (b[1] >> 4) & 3);
        m.num("battery_ok", b[0] & 1);
        if (v >= 0 && v <= 100) m.num("humidity", v);
        else m.num("temperature_C", (v - 500) * 0.1, "%.1f");
    }
}

// ------------------------------------------------------------ 00275RM ---
// OOK PWM 232 / 420 us (+ sync 632 us), complemented, 88-bit rows sent 3
// times (a bit-wise majority of the three is tried too). CRC-16 (LSB-first,
// poly 0xB2 reflected form, init 0xD0) over all 11 bytes = 0.
// id = bytes 0,1,3; byte 2: bit 6 battery OK, bit 0 model (1 = 00275rm);
// temp 12 bits (C*10 + 1000); probe type 2 bits; humidity 7 bits; probe data
void decode_00275(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.pwm(Mod::OOK, 232, 420, 520, 708, 632, 0);
    for (const auto& msg : rows.split()) {
        std::vector<Row> cand;
        for (const auto& x : msg.r)
            if (x.n == 88) cand.push_back(x.inverted());
        if (cand.empty()) continue;
        if (cand.size() >= 3) {
            Row v = cand[0];
            for (size_t i = 0; i < 11; i++)
                v.b[i] = static_cast<uint8_t>((cand[0].b[i] & cand[1].b[i]) | (cand[1].b[i] & cand[2].b[i]) |
                                              (cand[2].b[i] & cand[0].b[i]));
            v.t1 = cand[2].t1;
            cand.push_back(v);
        }
        for (const auto& r : cand) {
            const auto b = r.bytes(0, 11);
            if (crc16lsb(b.data(), 11, 0x00B2, 0x00D0) != 0) continue;
            const int probe = b[5] & 3;
            const int t = (b[4] << 4) | (b[5] >> 4);
            const int pt = ((b[7] & 0x0F) << 8) | b[8];
            Msg& m = emit(out, r, (b[2] & 1) ? "Acurite-00275rm" : "Acurite-00276rm", Kind::WEATHER, "CRC");
            for (const auto& x : cand) widen(m, x);
            m.id = std::to_string((b[0] << 16) | (b[1] << 8) | b[3]);
            m.num("subtype", probe);
            m.num("battery_ok", (b[2] >> 6) & 1);
            m.num("temperature_C", (t - 1000) * 0.1, "%.1f");
            m.num("humidity", ((b[6] & 0x1F) << 2) | (b[7] >> 6));
            if (probe == 1) m.num("water", (b[7] & 0x0F) == 15 ? 1 : 0);
            if (probe == 2 || probe == 3) m.num("temperature_1_C", (pt - 1000) * 0.1, "%.1f");
            if (probe == 3) m.num("humidity_1", b[9] & 0x7F);
            break;
        }
    }
}

}  // namespace
ISM_PROTOCOL("Acurite TXR family (592TXR, 5n1, 3n1, 6045M, Atlas, 515, 899, 1190, Optimus)", Strength::MEDIUM,
             decode_txr);
ISM_PROTOCOL("Acurite 609TXC", Strength::MEDIUM, decode_609);
ISM_PROTOCOL("Acurite 606TX", Strength::STRONG, decode_606);
ISM_PROTOCOL("Acurite 986", Strength::STRONG, decode_986);
ISM_PROTOCOL("Acurite 985", Strength::STRONG, decode_985);
ISM_PROTOCOL("Acurite 590TX", Strength::WEAK, decode_590);
ISM_PROTOCOL("Acurite 00275rm / 00276rm", Strength::STRONG, decode_00275);
}  // namespace ism
