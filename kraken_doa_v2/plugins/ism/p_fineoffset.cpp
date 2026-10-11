// Fine Offset Electronics sensors and their many rebrands (Ecowitt, Ambient
// Weather, Froggit, Misol, Telldus, Digitech, ...):
//   OOK PWM 433 MHz: WH2 / WH2A / WH5 / Telldus-Proove / TFA 30.3225 /
//     Rosenborg 66796, WH0530, WH1080 / WH3080 (weather, radio time, UV and
//     light), WH1050 / TFA Stratos, Ecowitt WH53 / WH0280 / WH0281A
//   FSK NRZ 433/868/915 MHz (aa.. 2d d4 framing): WH24 / WH65B / WS69 /
//     HP1000, WH25 / WH32 / WH32B / WN32B, WH0290 / WH41 PM2.5, WH51 soil,
//     WN34 / WN34D / WN38, WH52 soil, WH1080 FSK, Ambient WH31E / Ecowitt
//     WH31B thermo-hygro (+ their radio-clock frame), WH40 / WN20 rain,
//     WS68 wind / light / UV
//
// protocol notes: rtl_433 documentation; own implementation.
//
// The common check is CRC-8 with polynomial 0x31 (MSB first, init 0) and, on
// most FSK devices, an 8-bit sum of the bytes after it.

#include "ism.hpp"

#include <algorithm>
#include <array>

namespace ism {
namespace {

using Bytes = std::array<uint8_t, 26>;

inline int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
inline bool crc_ok(const uint8_t* p, int n) { return crc8(p, n, 0x31, 0x00) == 0; }   // n incl. the CRC byte
inline bool sum_ok(const uint8_t* p, int n) { return (add_bytes(p, n) & 0xFF) == p[n]; }
inline std::string hexbytes(const uint8_t* p, int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += hexs(p[i], 2);
    return s;
}
inline std::string clock_str(int y, int mo, int d, int h, int mi, int s, const char* z = "") {
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d%s", y, mo, d, h, mi, s, z);
    return b;
}
inline int leading_ones(const Row& r) {
    int k = 0;
    while (k < r.n && r.bit(k)) k++;
    return k;
}

// ============================================================ OOK PWM ======
// WH2 family: 433.92 MHz, ~500 us pulse = 1, ~1500 us = 0, ~1 ms gaps; two
// copies per transmission. A run of 7..9 ones, then 5 bytes (6 for WH2A /
// TFA 30.3225): type nibble 4, 8-bit id, 12-bit temperature, humidity,
// CRC-8 of the 4 bytes before it. The preamble length tells the model: 8
// ones WH2, 7 WH5 (or WH2A with 6 bytes), 9 Telldus.
void decode_wh2(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pwm(Mod::OOK, 500, 1500, 0, 1200, 0, 160);
    for (const auto& r : rs.r) {
        const int ones = leading_ones(r);
        const int len = r.n - ones;
        if (ones < 7 || ones > 9 || (len != 40 && len != 48)) continue;
        const std::vector<uint8_t> b = r.bytes(ones, len / 8);
        if ((b[0] >> 4) != 4) continue;
        if (crc8(b.data(), 4, 0x31, 0) != b[4]) continue;
        const int id = ((b[0] & 15) << 4) | (b[1] >> 4);
        const int raw = ((b[1] & 15) << 8) | b[2];
        const int hum = b[3];
        const char* model;
        double temp = (raw & 0x800 ? -1 : 1) * (raw & 0x7FF) * 0.1;
        int batt = -1;
        if (len == 48) {
            if (b[3] == 0xFF) {
                if (((b[0] + b[1] + b[2] + b[3] + b[4]) & 0xFF) != b[5]) continue;
                model = "TFA-303225";
                batt = (raw & 0x800) ? 0 : 1;
                temp = ((raw & 0x7FF) - 400) * 0.1;
            } else {
                if (ones != 7) continue;
                model = "Fineoffset-WH2A";
            }
        } else if (ones == 8) {
            model = "Fineoffset-WH2";
        } else if (ones == 9) {
            model = "Fineoffset-TelldusProove";
        } else {
            // WH5 and Rosenborg 66796 are the same on air; the WH5's
            // offset temperature is only plausible within -40..60 C
            const double t5 = (raw - 400) * 0.1;
            if (t5 >= -40.0 && t5 <= 60.0) {
                model = "Fineoffset-WH5";
                temp = t5;
            } else {
                model = "Rosenborg-66796";
            }
        }
        Msg& m = emit(out, r, model, Kind::WEATHER, "CRC");
        m.id = std::to_string(id);
        m.num("id", id);
        if (batt >= 0) m.num("battery_ok", batt);
        m.num("temperature_C", temp, "%.1f");
        if (hum != 0xFF) m.num("humidity", hum);
    }
}
ISM_PROTOCOL("Fine Offset WH2/WH5/Telldus", Strength::STRONG, decode_wh2);

// WH0530 temperature / rain: same timing, 7 ones + 8 bytes: type nibble 3,
// id, battery + 11-bit temperature (+400, 0.1 C), rain count (little endian,
// 0.3 mm), 0xFF, CRC-8 of bytes 0..5, sum of bytes 0..6.
void decode_wh0530(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pwm(Mod::OOK, 500, 1500, 0, 1200, 0, 160);
    for (const auto& r : rs.r) {
        const int ones = leading_ones(r);
        if (ones < 6 || ones > 8 || r.n - ones != 64) continue;
        const std::vector<uint8_t> b = r.bytes(ones, 8);
        if ((b[0] >> 4) != 3) continue;
        if (crc8(b.data(), 6, 0x31, 0) != b[6] || !sum_ok(b.data(), 7)) continue;
        const int id = ((b[0] & 15) << 4) | (b[1] >> 4);
        Msg& m = emit(out, r, "Fineoffset-WH0530", Kind::WEATHER, "CRC");
        m.id = std::to_string(id);
        m.num("id", id).num("battery_ok", (b[1] & 0x08) ? 0 : 1);
        m.num("temperature_C", ((((b[1] & 7) << 8) | b[2]) - 400) * 0.1, "%.1f");
        m.num("rain_mm", ((b[4] << 8) | b[3]) * 0.3, "%.1f");
    }
}
ISM_PROTOCOL("Fine Offset WH0530", Strength::STRONG, decode_wh0530);

// WH1080 / WH3080 field layouts (shared by the OOK and the FSK version):
// br[1..10] = type nibble (A weather, B radio time, 7 UV / light) ... CRC-8
// of br[1..9] in br[10] (br[1..6] / br[7] for UV).
bool wh1080_emit(std::vector<Msg>& out, const Row& r, const uint8_t* br, bool fsk, bool pre8) {
    const int type = br[1] >> 4;
    if (type == 0x7) {
        if (crc8(br + 1, 6, 0x31, 0) != br[7]) return false;
        const int light = (br[4] << 16) | (br[5] << 8) | br[6];
        Msg& m = emit(out, r, "Fineoffset-WHx080", Kind::WEATHER, "CRC");
        const int uvid = ((br[1] & 15) << 4) | (br[2] >> 4);
        m.id = "";
        m.num("subtype", 2).num("uv_sensor_id", uvid).str("uv_status", br[3] == 0x55 ? "OK" : "ERROR");
        m.num("uv_index", br[2] & 15).num("lux", light * 0.1, "%.1f").num("wm", light / (pre8 ? 6830.0 : 1265.8), "%.3f");
        return true;
    }
    if (type != 0xA && type != 0xB) return false;
    if (crc8(br + 1, 9, 0x31, 0) != br[10]) return false;
    const int id = ((br[1] & 15) << 4) | (br[2] >> 4);
    Msg& m = emit(out, r, "Fineoffset-WHx080", Kind::WEATHER, "CRC");
    m.id = std::to_string(id);
    if (type == 0xB) {
        m.num("subtype", 1).num("id", id).str("signal", (br[2] & 15) == 10 ? "DCF77" : "WWVB/MSF");
        const int hh = ((br[3] >> 4) & 3) * 10 + (br[3] & 15);
        const int mo = ((br[7] >> 4) & 1) * 10 + (br[7] & 15);
        m.str("radio_clock", clock_str(2000 + bcd(br[6]), mo, bcd(br[8]), hh, bcd(br[4]), bcd(br[5])));
        return true;
    }
    static const int dirs[16] = {0, 23, 45, 68, 90, 113, 135, 158, 180, 203, 225, 248, 270, 293, 315, 338};
    double temp;
    if (fsk) {
        const int raw = ((br[2] & 15) << 8) | br[3];
        temp = (raw & 0x800 ? -1 : 1) * (raw & 0x7FF) * 0.1;
    } else {
        temp = ((((br[2] & 3) << 8) | br[3]) - 400) * 0.1;
    }
    m.num("subtype", 0).num("id", id).num("battery_ok", (br[9] >> 4) == 1 ? 0 : 1);
    m.num("temperature_C", temp, "%.1f").num("humidity", br[4]).num("wind_dir_deg", dirs[br[9] & 15]);
    m.num("wind_avg_km_h", br[5] * 0.34 * 3.6, "%.3f").num("wind_max_km_h", br[6] * 0.34 * 3.6, "%.3f");
    m.num("rain_mm", (((br[7] & 15) << 8) | br[8]) * 0.3, "%.1f");
    return true;
}

// WH1080 / WH3080 OOK: ~544 us = 1, ~1524 us = 0; 8 (or 7) ones, then 10
// bytes (weather / time) or 7 bytes (UV / light).
// WH1050: same timing, 8 (or 7) ones + 9 bytes: type nibble 5 weather / 6
// time, CRC-8 over all 9 = 0.
// Ecowitt WH53: ~500 / ~1480 us; 7 ones, then 0x53, id, channel + 10-bit
// temperature, 0xFF, CRC-8 (over 0x53..CRC = 0).
void decode_ook_1ms(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pwm(Mod::OOK, 544, 1524, 0, 2800);
    for (const auto& r : rs.r) {
        if (r.n < 50) continue;
        const int ones = leading_ones(r);
        // WH1080 / WH3080: the preamble ends 7 or 8 bits in (the type
        // nibble A / B starts with a 1, so the run of ones says little)
        bool done = false;
        for (int pre : {8, 7}) {
            if (ones < pre) continue;
            const int len = r.n - pre;
            uint8_t br[11];
            br[0] = 0xFF;
            if (len >= 80 && len <= 91) {
                for (int i = 0; i < 10; i++) br[i + 1] = r.byte(pre + 8 * i);
                if ((br[1] >> 4) != 7 && wh1080_emit(out, r, br, false, pre == 8)) { done = true; break; }
            } else if (len == 56) {
                for (int i = 0; i < 7; i++) br[i + 1] = r.byte(pre + 8 * i);
                if ((br[1] >> 4) == 7 && wh1080_emit(out, r, br, false, pre == 8)) { done = true; break; }
            }
        }
        if (done) continue;
        // WH1050
        if (ones >= 6 && ones <= 8 && r.n - ones == 72) {
            const std::vector<uint8_t> b = r.bytes(ones, 9);
            const int type = b[0] >> 4;
            if ((type == 5 || type == 6) && crc_ok(b.data(), 9)) {
                const int id = ((b[0] & 15) << 4) | (b[1] >> 4);
                Msg& m = emit(out, r, "Fineoffset-WH1050", Kind::WEATHER, "CRC");
                m.id = std::to_string(id);
                m.num("id", id).num("msg_type", type).num("battery_ok", (b[1] & 4) ? 0 : 1);
                if (type == 5) {
                    m.num("temperature_C", ((((b[1] & 3) << 8) | b[2]) - 400) * 0.1, "%.1f").num("humidity", b[3]);
                    m.num("wind_avg_km_h", b[4] * 0.34 * 3.6, "%.3f").num("wind_max_km_h", b[5] * 0.34 * 3.6, "%.3f");
                    m.num("rain_mm", ((b[6] << 8) | b[7]) * 0.3, "%.1f");
                } else {
                    const int hh = ((b[2] >> 4) & 3) * 10 + (b[2] & 15);
                    const int mo = ((b[6] >> 4) & 1) * 10 + (b[6] & 15);
                    m.str("radio_clock", clock_str(2000 + bcd(b[5]), mo, bcd(b[7]), hh, bcd(b[3]), bcd(b[4])));
                }
                continue;
            }
        }
        // WH53: 4 header ones + 0x53, then 5 more bytes
        for (int p = r.find(0xF53, 12); p >= 0; p = r.find(0xF53, 12, p + 1)) {
            if (r.n - p < 52) break;
            const std::vector<uint8_t> b = r.bytes(p + 4, 6);
            if (!crc_ok(b.data(), 6)) continue;
            const int ch = (b[2] >> 4) + 1;
            if (ch > 3 || (b[2] & 0x0C) || b[4] != 0xFF) continue;
            Msg& m = emit(out, r, "Ecowitt-WH53", Kind::WEATHER, "CRC");
            m.id = std::to_string(b[1]);
            m.num("id", b[1]).num("channel", ch);
            m.num("temperature_C", ((((b[2] & 3) << 8) | b[3]) - 400) * 0.1, "%.1f");
            break;
        }
    }
}
ISM_PROTOCOL("Fine Offset WH1080/WH1050, Ecowitt WH53", Strength::STRONG, decode_ook_1ms);

// ============================================================ FSK NRZ ======
// All FSK models: ~58 us (56 us Ambient/Ecowitt WH31E family) NRZ, preamble
// aa.., sync 2d d4, then a family byte: the payload is dispatched on it.

// WH24 / WH65B / WS69 outdoor array: 0x24, 17 bytes; CRC-8 of bytes 0..14 in
// 15, sum of 0..15 in 16 (WS69: 8 more bytes with pressure, CRC in 23, sum
// in 24). WH24 and WH65B are the same frame with other scale factors; the
// WH65B (915 MHz) sends a longer preamble.
bool fo_wh24(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail, int pre_bits) {
    if (b[0] != 0x24 || avail < 17 || !crc_ok(b.data(), 16) || !sum_ok(b.data(), 16)) return false;
    const bool ws69 = avail >= 25 && crc_ok(b.data(), 24) && sum_ok(b.data(), 24);
    const bool wh65b = ws69 || pre_bits >= 43;
    const double F = wh65b ? 0.51 : 1.12, R = wh65b ? 0.254 : 0.3;
    const int dir = b[2] | ((b[3] & 0x80) << 1);
    const int traw = ((b[3] & 7) << 8) | b[4];
    const int speed = b[6] | ((b[3] & 0x10) << 4);
    const int uv = (b[10] << 8) | b[11];
    const int light = (b[12] << 16) | (b[13] << 8) | b[14];
    Msg& m = emit(out, r, ws69 ? "Fineoffset-WS69" : wh65b ? "Fineoffset-WH65B" : "Fineoffset-WH24", Kind::WEATHER, "CRC");
    m.id = std::to_string(b[1]);
    m.num("id", b[1]).num("battery_ok", (b[3] & 0x08) ? 0 : 1);
    if (traw != 0x7FF) m.num("temperature_C", (traw - 400) * 0.1, "%.1f");
    if (b[5] != 0xFF) m.num("humidity", b[5]);
    if (ws69) {
        const int p = (b[17] << 16) | (b[18] << 8) | b[19];
        if (p < 0x1FFFF) m.num("pressure_hPa", p * 0.01, "%.2f");
    }
    if (dir != 0x1FF) m.num("wind_dir_deg", dir);
    if (speed != 0x1FF) m.num("wind_avg_m_s", speed / 8.0 * F, "%.2f");
    if (b[7] != 0xFF) m.num("wind_max_m_s", b[7] * F, "%.2f");
    m.num("rain_mm", ((b[8] << 8) | b[9]) * R, "%.1f");
    if (uv != 0xFFFF) {
        static const int uvt[13] = {432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029};
        int uvi = 0;
        while (uvi < 13 && uvt[uvi] < uv) uvi++;
        m.num("uv", uv).num("uvi", uvi);
    }
    if (light != 0xFFFFFF) m.num("light_lux", light * 0.1, "%.1f");
    return true;
}

// WH25 / WH32B / WN32B (type nibble E) and WH32 (D) indoor T/H(/P): 8
// bytes, sum of 0..5 in 6; the WH25 adds the nibble-swapped XOR of 0..5 in 7
// (WH32B: 0). id, battery, 10-bit temperature (+400), humidity, pressure.
bool fo_wh25(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    const int type = b[0] >> 4;
    if ((type != 0xE && type != 0xD) || avail < 8 || !sum_ok(b.data(), 6)) return false;
    const int x = xor_bytes(b.data(), 6);
    const uint8_t xs = static_cast<uint8_t>(((x & 15) << 4) | (x >> 4));
    const char* model;
    if (type == 0xD) {
        if (b[7] != 0) return false;
        model = "Fineoffset-WH32";
    } else if (b[7] == xs && xs != 0) {
        model = "Fineoffset-WH25";
    } else if (b[7] == 0) {
        model = "Fineoffset-WH32B";
    } else {
        return false;
    }
    const int id = ((b[0] & 15) << 4) | (b[1] >> 4);
    const int p = (b[4] << 8) | b[5];
    Msg& m = emit(out, r, model, Kind::WEATHER, "CRC");
    m.id = std::to_string(id);
    m.num("id", id).num("battery_ok", (b[1] & 0x08) ? 0 : 1);
    m.num("temperature_C", ((((b[1] & 3) << 8) | b[2]) - 400) * 0.1, "%.1f").num("humidity", b[3]);
    if (p != 0xFFFF) m.num("pressure_hPa", p * 0.1, "%.1f");
    return true;
}

// WH0290 / WH41 PM2.5 (0x41 / 0x42): CRC-8 of 0..5 in 6, sum of 0..6 in 7.
bool fo_wh0290(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    if ((b[0] != 0x41 && b[0] != 0x42) || avail < 8 || !crc_ok(b.data(), 7) || !sum_ok(b.data(), 7)) return false;
    const int bars = (((b[2] >> 6) & 1) << 2) | (b[4] >> 6);
    Msg& m = emit(out, r, "Fineoffset-WH0290", Kind::WEATHER, "CRC");
    m.id = std::to_string(b[1]);
    m.num("id", b[1]).num("battery_ok", bars * 0.2, "%.1f");
    m.num("pm2_5_ug_m3", ((((b[2] & 0x3F) << 8) | b[3]) / 10)).num("estimated_pm10_0_ug_m3", ((((b[4] & 0x3F) << 8) | b[5]) / 10));
    m.num("family", b[0]).num("unknown1", (b[2] >> 7) & 1);
    return true;
}

// WH51 soil moisture (0x51), 14 bytes: CRC-8 of 0..11 in 12, sum of 0..12 in 13.
bool fo_wh51(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    if (b[0] != 0x51 || avail < 14 || !crc_ok(b.data(), 13) || !sum_ok(b.data(), 13)) return false;
    const int v = b[4] & 0x1F;
    const double bok = v >= 16 ? 1.0 : v == 15 ? 0.9 : v == 14 ? 0.5 : v == 13 ? 0.1 : 0.0;
    Msg& m = emit(out, r, "Fineoffset-WH51", Kind::WEATHER, "CRC");
    m.id = hexbytes(&b[1], 3);
    m.str("id", m.id).num("battery_ok", bok, "%.1f").num("battery_mV", v * 100).num("moisture", b[6]);
    m.num("boost", b[4] >> 5).num("ad_raw", ((b[7] & 1) << 8) | b[8]);
    return true;
}

// WN34 / WN34D (0x34), WN38 black globe (0x38): 9 bytes, CRC-8 of 0..6 in 7,
// sum of 0..7 in 8; 12-bit two's complement temperature (-40 C offset
// except on the WN34D / WN38 subtype 4).
bool fo_wn34(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    if ((b[0] != 0x34 && b[0] != 0x38) || avail < 9 || !crc_ok(b.data(), 8) || !sum_ok(b.data(), 8)) return false;
    const int id = (b[1] << 16) | (b[2] << 8) | b[3];
    const int sub = b[4] >> 4;
    int t = ((b[4] & 15) << 8) | b[5];
    if (t & 0x800) t -= 0x1000;
    const double temp = sub == 4 ? t * 0.1 : t * 0.1 - 40.0;
    const int mv = (b[6] & 0x7F) * 20;
    const int bars = mv > 1440 ? 5 : mv > 1380 ? 4 : mv > 1300 ? 3 : mv > 1200 ? 2 : 1;
    Msg& m = emit(out, r, b[0] == 0x38 ? "Fineoffset-WN38" : sub == 4 ? "Fineoffset-WN34D" : "Fineoffset-WN34",
                  Kind::WEATHER, "CRC");
    m.id = std::to_string(id);
    m.num("id", id).num("battery_ok", (bars - 1) * 0.25, "%.2f").num("battery_mV", mv).num("temperature_C", temp, "%.1f");
    return true;
}

// WH52 soil moisture / temperature / conductivity (0xA2), 24 bytes: CRC-8 of
// 0..21 in 22, sum of 0..22 in 23.
bool fo_wh52(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    if (b[0] != 0xA2 || avail < 24 || !crc_ok(b.data(), 23) || !sum_ok(b.data(), 23)) return false;
    const int mv = b[20] * 20;
    Msg& m = emit(out, r, "Fineoffset-WH52", Kind::WEATHER, "CRC");
    m.id = hexbytes(&b[1], 3);
    m.str("id", m.id).num("battery_ok", std::clamp((mv - 1300) / 300.0, 0.0, 1.0), "%.2f").num("battery_mV", mv);
    m.num("boost", b[4] >> 5).num("temperature_C", (((b[4] & 0x1F) << 8) | b[5]) * 0.1 - 40.0, "%.1f");
    m.num("moisture", b[6]).num("conductivity_uS_cm", (((b[8] & 15) << 16) | (b[9] << 8) | b[10]) / 25.6, "%.3f");
    return true;
}

// WH1080 / WH3080 FSK: 10 bytes after the sync, laid out as the OOK frame.
bool fo_wh1080(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    const int type = b[0] >> 4;
    if (type != 0xA && type != 0xB && type != 0x7) return false;
    if (avail < (type == 7 ? 7 : 10)) return false;
    uint8_t br[11];
    br[0] = 0xFF;
    for (int i = 0; i < 10; i++) br[i + 1] = b[static_cast<size_t>(i)];
    return wh1080_emit(out, r, br, true, true);
}

// Ambient Weather WH31E (0x30) / Ecowitt WH31B (0x37) thermo-hygro: CRC-8
// over 0..5 = 0, sum of 0..5 in 6; their radio-clock frame (0x52); Ecowitt
// WH40 (0x40) and WN20 (0x20) rain gauges, WS68 (0x68) wind / light / UV.
bool fo_wh31e(std::vector<Msg>& out, const Row& r, const Bytes& b, int avail) {
    switch (b[0]) {
    case 0x30:
    case 0x37: {
        if (avail < 7 || !crc_ok(b.data(), 6) || !sum_ok(b.data(), 6)) return false;
        Msg& m = emit(out, r, b[0] == 0x30 ? "AmbientWeather-WH31E" : "AmbientWeather-WH31B", Kind::WEATHER, "CRC");
        m.id = std::to_string(b[1]);
        m.num("id", b[1]).num("channel", ((b[2] >> 4) & 7) + 1).num("battery_ok", (b[2] & 4) ? 0 : 1);
        m.num("temperature_C", ((((b[2] & 3) << 8) | b[3]) - 400) * 0.1, "%.1f").num("humidity", b[4]);
        m.str("data", hexbytes(&b[6], 5));
        return true;
    }
    case 0x52: {
        if (avail < 11 || !crc_ok(b.data(), 10) || !sum_ok(b.data(), 10)) return false;
        Msg& m = emit(out, r, "AmbientWeather-WH31E", Kind::OTHER, "CRC");
        m.id = std::to_string(b[1]);
        m.num("id", b[1]).num("data", b[2]);
        m.str("radio_clock", clock_str(2000 + bcd(b[3]), ((b[4] >> 4) & 1) * 10 + (b[4] & 15),
                                       ((b[5] >> 4) & 3) * 10 + (b[5] & 15), ((b[6] >> 4) & 3) * 10 + (b[6] & 15),
                                       ((b[7] >> 4) & 7) * 10 + (b[7] & 15), ((b[8] >> 4) & 7) * 10 + (b[8] & 15), "Z"));
        return true;
    }
    case 0x40: {
        if (avail < 9 || !crc_ok(b.data(), 8) || !sum_ok(b.data(), 8)) return false;
        const int id = ((b[1] & 15) << 16) | (b[2] << 8) | b[3];
        const int v = b[4] & 0x1F;
        Msg& m = emit(out, r, "EcoWitt-WH40", Kind::WEATHER, "CRC");
        m.id = std::to_string(id);
        m.num("id", id);
        if (v) {
            const int lvl = v <= 9 ? 0 : std::min(100, 100 * (v - 9) / 6);
            m.num("battery_ok", lvl * 0.01, "%.2f").num("battery_V", v * 0.1, "%.1f");
        }
        m.num("rain_mm", ((b[5] << 8) | b[6]) * 0.1, "%.1f").str("data", hexbytes(&b[9], 5));
        return true;
    }
    case 0x20: {
        if (avail < 10 || !crc_ok(b.data(), 9) || !sum_ok(b.data(), 9)) return false;
        const int id = (b[2] << 8) | b[3];
        const int lvl = b[4] <= 90 ? 0 : std::min(100, 100 * (b[4] - 90) / 60);
        Msg& m = emit(out, r, "EcoWitt-WN20", Kind::WEATHER, "CRC");
        m.id = std::to_string(id);
        m.num("id", id).num("battery_ok", lvl > 0 ? 1 : 0).num("battery_V", b[4] * 0.02, "%.2f").num("battery_pct", lvl);
        m.num("rain_mm", ((b[5] << 8) | b[6]) * 0.1, "%.1f").str("data", hexbytes(&b[10], 5));
        return true;
    }
    case 0x68: {
        if (avail < 16 || !crc_ok(b.data(), 15) || !sum_ok(b.data(), 15)) return false;
        const int id = (b[2] << 8) | b[3];
        Msg& m = emit(out, r, "EcoWitt-WS68", Kind::WEATHER, "CRC");
        m.id = std::to_string(id);
        m.num("id", id).num("battery_ok", b[6] > 0x20 ? 1 : 0).num("battery_raw", b[6]);
        m.num("light_lux", ((b[4] << 8) | b[5]) * 10).num("uvi", static_cast<int>(b[13] * 0.1));
        m.num("wind_avg_m_s", ((((b[7] >> 4) & 1) << 8) | b[10]) * 0.1, "%.1f");
        m.num("wind_max_m_s", ((((b[7] >> 6) & 1) << 8) | b[12]) * 0.1, "%.1f");
        m.num("wind_dir_deg", (((b[7] >> 5) & 1) << 8) | b[11]);
        m.str("data", hexbytes(&b[16], 1) + hexs(b[17] >> 4, 1));
        return true;
    }
    default:
        return false;
    }
}

int fsk_row(std::vector<Msg>& out, const Row& r) {
    int n = 0;
    for (int p = r.find(0xAA2DD4, 24); p >= 0; p = r.find(0xAA2DD4, 24, p + 1)) {
        const int s = p + 24;
        const int avail = std::min(26, (r.n - s) / 8);
        if (avail < 7) break;
        Bytes b{};
        for (int i = 0; i < avail; i++) b[static_cast<size_t>(i)] = r.byte(s + 8 * i);
        // the preamble length: alternating bits before the sync
        int pre = 0;
        while (p - pre - 1 >= 0 && r.bit(p - pre - 1) != r.bit(p - pre)) pre++;
        pre += 8;
        if (fo_wh24(out, r, b, avail, pre) || fo_wh25(out, r, b, avail) || fo_wh0290(out, r, b, avail) ||
            fo_wh51(out, r, b, avail) || fo_wn34(out, r, b, avail) || fo_wh52(out, r, b, avail) ||
            fo_wh31e(out, r, b, avail) || fo_wh1080(out, r, b, avail)) {
            n++;
            p = s + 8 * 7;   // past this frame (a burst may hold 2..6 copies)
        }
    }
    return n;
}

// The ~120 kHz deviation puts one tone outside the 240 kHz band when the
// VFO is ~60 kHz off the sensor's frequency; the burst then shows as OOK on
// the other tone (inverted when that is the lower one): searched when the
// FSK slicing gave nothing.
void decode_fsk(Pkt& pk, std::vector<Msg>& out) {
    int n = 0;
    for (const auto& r : pk.pcm(Mod::FSK, 58, 58, 0, 20000).r)
        if (r.n >= 80) n += fsk_row(out, r);
    if (n) return;
    for (const auto& r : pk.pcm(Mod::OOK, 58, 58, 0, 20000).r)
        if (r.n >= 80 && fsk_row(out, r) == 0) fsk_row(out, r.inverted());
}
ISM_PROTOCOL("Fine Offset / Ecowitt FSK", Strength::STRONG, decode_fsk);

}  // namespace
}  // namespace ism
