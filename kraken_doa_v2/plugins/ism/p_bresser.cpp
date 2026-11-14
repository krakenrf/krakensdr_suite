// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/bresser_5in1.c - Copyright (C) 2018 Daniel Krueger; Copyright
//     (C) 2019 Christian W. Zuckschwerdt <zany@triq.net>
//   src/devices/bresser_6in1.c - Copyright (C) 2019 Christian W. Zuckschwerdt
//     <zany@triq.net>
//   src/devices/bresser_7in1.c - Copyright (C) 2019 Christian W. Zuckschwerdt
//     <zany@triq.net>
//   src/devices/bresser_3ch.c - Copyright (C) 2015 Christian W. Zuckschwerdt
//     <zany@triq.net>

// Bresser weather sensors: 5-in-1 (26-byte frame) incl. the Professional Rain
// Gauge, the 6-in-1 family (new 5-in-1, 7-in-1 indoor, soil, pool, 3-in-1
// wind gauge, T/H sensors; Froggit WH6000, Ventus C8488A, Ambient TX-31xx),
// the 7-in-1 / 8-in-1 / 3-in-1 stations and air-quality sensors (PM, CO2,
// HCHO/VOC), and the 3CH thermo-hygro sensor (also Renkforce DM-7511).

#include "ism.hpp"

#include <array>

namespace ism {
namespace {

// The FSK models send 868.3 / 915 MHz FSK with ~120 kHz between the tones,
// NRZ at ~122 us per bit, preamble aa.., sync 2d d4.
template <class F>
void each_pcm_row(Pkt& pk, int bit_us, int reset_us, F&& fn) {
    for (const auto& r : pk.pcm(Mod::FSK, bit_us, bit_us, 0, reset_us).r) fn(r);
}

// every position just after "aa 2d d4" (or, with sync16, just "2d d4": the
// end of the preamble is the part most often damaged) in the row
template <class F>
void each_sync(const Row& r, int min_after, F&& fn, bool sync16 = false) {
    const uint64_t pat = sync16 ? 0x2DD4 : 0xAA2DD4;
    const int pn = sync16 ? 16 : 24;
    for (int p = r.find(pat, pn); p >= 0; p = r.find(pat, pn, p + 1)) {
        const int s = p + pn;
        if (r.n - s < min_after) return;
        if (fn(s)) return;
    }
}

inline int bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 15); }

// --------------------------------------------------------------- 5-in-1 ---
// 26 bytes after the sync: bytes 0..12 are the bitwise complement of bytes
// 13..25 (the data half), byte 13 = the number of set bits in 14..25. With
// that much redundancy one damaged byte of the complement half is accepted
// when the data half's bit count is right. Wind gust binary, wind average /
// temperature / humidity / rain BCD with the top digit in the following byte.
void decode_5in1(Pkt& pk, std::vector<Msg>& out) {
    each_pcm_row(pk, 124, 25000, [&](const Row& r) {
        each_sync(r, 201, [&](int s) {
            const std::vector<uint8_t> b = r.bytes(s, 26);
            int bad = 0;
            for (int i = 0; i < 13; i++)
                if ((b[static_cast<size_t>(i)] ^ b[static_cast<size_t>(i + 13)]) != 0xFF) bad++;
            int pop = 0;
            for (int i = 14; i < 26; i++) pop += __builtin_popcount(b[static_cast<size_t>(i)]);
            if (bad > 1 || pop != b[13]) return false;
            const int id = b[14];
            const int stype = b[15] & 0x7F;
            const bool temp_ok = (b[20] & 15) <= 9;
            int traw = (b[20] & 15) + (b[20] >> 4) * 10 + (b[21] & 15) * 100;
            if (b[25] & 15) traw = -traw;
            const bool hum_ok = (b[22] & 15) <= 9;
            const int hum = bcd2(b[22]);
            const double dir = (b[17] >> 4) * 22.5;
            const double gust = (((b[17] & 15) << 8) | b[16]) * 0.1;
            const double avg = ((b[18] & 15) + (b[18] >> 4) * 10 + (b[19] & 15) * 100) * 0.1;
            double rain = ((b[23] & 15) + (b[23] >> 4) * 10 + (b[24] & 15) * 100 + (b[24] >> 4) * 1000) * 0.1;
            const bool batt_low = b[25] & 0x80;
            const bool pro_rain = stype >= 0x39 && stype <= 0x3B;
            if (pro_rain) rain *= 2.5;
            Msg& m = emit(out, r, pro_rain ? "Bresser-ProRainGauge" : "Bresser-5in1", Kind::WEATHER, "CHECKSUM");
            m.id = std::to_string(id);
            m.num("id", id).num("battery_ok", batt_low ? 0 : 1);
            if (temp_ok) m.num("temperature_C", traw * 0.1, "%.1f");
            if (!pro_rain) {
                if (hum_ok) m.num("humidity", hum);
                m.num("wind_max_m_s", gust, "%.1f").num("wind_avg_m_s", avg, "%.1f").num("wind_dir_deg", dir, "%.1f");
            }
            m.num("rain_mm", rain, "%.1f");
            return true;
        }, true);
    });
}
ISM_PROTOCOL("Bresser 5-in-1", Strength::STRONG, decode_5in1);

// --------------------------------------------------------------- 6-in-1 ---
// 18 bytes after the sync (2d d4; the preamble end is not required): B0B1 =
// LFSR-16 digest (gen 0x8810, key 0x5412) of B2..B16, and B2..B17 sum to
// 0xFF (mod 256). B2..B5 id, B6 type / startup / channel; alternating frames
// carry temperature + humidity + UV or the rain counter (B16 bit 0 set);
// wind in every frame.
void decode_6in1(Pkt& pk, std::vector<Msg>& out) {
    static const int moist[16] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99};
    each_pcm_row(pk, 124, 25000, [&](const Row& r) {
        each_sync(r, 144, [&](int s) {
            std::vector<uint8_t> b = r.bytes(s, 18);
            const uint16_t dig = lfsr_digest16(&b[2], 15, 0x8810, 0x5412);
            if (dig != ((b[0] << 8) | b[1])) return false;
            if ((add_bytes(&b[2], 16) & 0xFF) != 0xFF) return false;
            const uint32_t id = (static_cast<uint32_t>(b[2]) << 24) | (b[3] << 16) | (b[4] << 8) | b[5];
            const int stype = b[6] >> 4, startup = (b[6] >> 3) & 1, chan = b[6] & 7;
            const int batt = (b[13] >> 1) & 1;
            const bool temp_ok = b[12] <= 0x99 && (b[13] & 0xF0) <= 0x90;
            const int traw = (b[12] >> 4) * 100 + (b[12] & 15) * 10 + (b[13] >> 4);
            double temp = ((b[13] >> 3) & 1) ? (traw - 1000) * 0.1 : traw * 0.1;
            if (temp < -50.0) temp = -traw * 0.1;   // 3-in-1 wind gauge
            const int hum = bcd2(b[14]);
            const uint8_t u15 = static_cast<uint8_t>(~b[15]), u16 = static_cast<uint8_t>(~b[16]);
            bool uv_ok = (b[16] & 15) == 0 && u15 <= 0x99 && (u16 & 0xF0) <= 0x90;
            const double uvi = ((u15 >> 4) * 100 + (u15 & 15) * 10 + (u16 >> 4)) * 0.1;
            const int flags = b[16] & 15;
            const uint8_t w7 = static_cast<uint8_t>(~b[7]), w8 = static_cast<uint8_t>(~b[8]),
                          w9 = static_cast<uint8_t>(~b[9]);
            bool wind_ok = w7 <= 0x99 && w8 <= 0x99 && w9 <= 0x99;
            const double gust = ((w7 >> 4) * 100 + (w7 & 15) * 10 + (w8 >> 4)) * 0.1;
            const double avg = ((w9 >> 4) * 100 + (w9 & 15) * 10 + (w8 & 15)) * 0.1;
            const int dir = (b[10] >> 4) * 100 + (b[10] & 15) * 10 + (b[11] >> 4);
            const bool rain_ok = b[16] & 1;
            int rraw = 0;
            for (int i = 12; i <= 14; i++) rraw = rraw * 100 + bcd2(static_cast<uint8_t>(~b[static_cast<size_t>(i)]));
            if (stype == 2 || stype == 4) wind_ok = uv_ok = false;
            int moisture = -1;
            if (stype == 4 && temp_ok && hum >= 1 && hum <= 16) moisture = moist[hum - 1];
            Msg& m = emit(out, r, "Bresser-6in1", Kind::WEATHER, "CRC");
            m.id = std::to_string(static_cast<int32_t>(id));
            m.num("id", static_cast<int32_t>(id)).num("channel", chan);
            if (!rain_ok) m.num("battery_ok", batt);
            if (temp_ok) m.num("temperature_C", temp, "%.1f");
            if (temp_ok && moisture < 0) m.num("humidity", hum);
            m.num("sensor_type", stype);
            if (moisture >= 0) m.num("moisture", moisture);
            if (wind_ok)
                m.num("wind_max_m_s", gust, "%.1f").num("wind_avg_m_s", avg, "%.1f").num("wind_dir_deg", dir);
            if (rain_ok) m.num("rain_mm", rraw * 0.1, "%.1f");
            if (uv_ok) m.num("uvi", uvi, "%.1f");
            if (startup) m.num("startup", startup);
            m.num("flags", flags);
            return true;
        }, true);
    });
}
ISM_PROTOCOL("Bresser 6-in-1", Strength::STRONG, decode_6in1);

// --------------------------------------------------------------- 7-in-1 ---
// 25 bytes after the sync, whitened with 0xAA (type / startup / channel read
// from the raw byte 6). Digest: LFSR-16 (gen 0x8810, key 0xBA95) of X2..X24,
// xor X0X1 must give 0x6DF1. Weather types 1 / 12 / 13, PM 8, CO2 10,
// HCHO/VOC 11; values BCD.
void decode_7in1(Pkt& pk, std::vector<Msg>& out) {
    each_pcm_row(pk, 124, 25000, [&](const Row& r) {
        each_sync(r, 21 * 8 + 1, [&](int s) {
            const std::vector<uint8_t> raw = r.bytes(s, 25);
            if (raw[21] == 0x00) return false;
            const int stype = raw[6] >> 4, nstartup = (raw[6] >> 3) & 1, chan = raw[6] & 7;
            std::array<uint8_t, 25> x{};
            for (size_t i = 0; i < 25; i++) x[i] = raw[i] ^ 0xAA;
            const uint16_t dig = lfsr_digest16(&x[2], 23, 0x8810, 0xBA95);
            if ((((x[0] << 8) | x[1]) ^ dig) != 0x6DF1) return false;
            auto d = [&](int byte, bool hi) { return hi ? x[static_cast<size_t>(byte)] >> 4 : x[static_cast<size_t>(byte)] & 15; };
            const int id = (x[2] << 8) | x[3];
            const bool batt_low = (x[15] & 0x06) == 0x06;
            if (stype == 1 || stype == 12 || stype == 13) {
                const int wdir = d(4, 1) * 100 + d(4, 0) * 10 + d(5, 1);
                const int gust = d(7, 1) * 100 + d(7, 0) * 10 + d(8, 1);
                const int wavg = d(8, 0) * 100 + d(9, 1) * 10 + d(9, 0);
                int rain = 0, light = 0;
                for (int i = 10; i <= 12; i++) rain = rain * 100 + d(i, 1) * 10 + d(i, 0);
                for (int i = 17; i <= 19; i++) light = light * 100 + d(i, 1) * 10 + d(i, 0);
                const int traw = d(14, 1) * 100 + d(14, 0) * 10 + d(15, 1);
                const double temp = traw > 600 ? (traw - 1000) * 0.1 : traw * 0.1;
                const int hum = d(16, 1) * 10 + d(16, 0);
                const int uv = d(20, 1) * 100 + d(20, 0) * 10 + d(21, 1);
                const bool wl = stype != 12;
                Msg& m = emit(out, r, "Bresser-7in1", Kind::WEATHER, "CRC");
                m.id = std::to_string(id);
                m.num("id", id);
                if (!nstartup) m.num("startup", 1);
                m.num("temperature_C", temp, "%.1f").num("humidity", hum);
                if (wl) m.num("wind_max_m_s", gust * 0.1, "%.1f").num("wind_avg_m_s", wavg * 0.1, "%.1f").num("wind_dir_deg", wdir);
                m.num("rain_mm", rain * 0.1, "%.1f");
                if (wl) m.num("light_klx", light * 0.001, "%.3f").num("light_lux", light, "%.0f").num("uvi", uv * 0.1, "%.1f");
                if (stype == 13 && d(23, 1) < 10) m.num("temperature_1_C", d(22, 1) * 10 + d(22, 0) + d(23, 1) * 0.1, "%.1f");
                m.num("battery_ok", batt_low ? 0 : 1);
                return true;
            }
            if (stype == 8) {
                const int pm25 = d(10, 0) * 1000 + d(11, 1) * 100 + d(11, 0) * 10 + d(12, 1);
                const int pm10 = d(12, 0) * 1000 + d(13, 1) * 100 + d(13, 0) * 10 + d(14, 1);
                Msg& m = emit(out, r, "Bresser-7in1", Kind::WEATHER, "CRC");
                m.id = std::to_string(id);
                m.num("id", id).num("channel", chan);
                if (!nstartup) m.num("startup", 1);
                m.num("battery_ok", batt_low ? 0 : 1);
                if (d(10, 0) != 15) m.num("pm2_5_ug_m3", pm25);
                if (d(12, 0) != 15) m.num("pm10_0_ug_m3", pm10);
                return true;
            }
            if (stype == 10 || stype == 11) {
                const int v = d(4, 1) * 1000 + d(4, 0) * 100 + d(5, 1) * 10 + d(5, 0);
                const bool init = d(5, 0) == 15;
                Msg& m = emit(out, r, stype == 10 ? "Bresser-CO2" : "Bresser-HCHOVOC", Kind::WEATHER, "CRC");
                m.id = std::to_string(id);
                m.num("id", id).num("channel", chan);
                if (!nstartup) m.num("startup", 1);
                m.num("battery_ok", batt_low ? 0 : 1);
                if (stype == 10) {
                    if (!init) m.num("co2_ppm", v);
                } else {
                    if (!init) m.num("hcho_ppb", v);
                    const int voc = d(22, 0);
                    if (voc != 15) m.num("voc_level", voc);
                }
                return true;
            }
            return false;
        });
    });
}
ISM_PROTOCOL("Bresser 7-in-1", Strength::STRONG, decode_7in1);

// ------------------------------------------------------------------ 3CH ---
// OOK PWM: 250 us pulse = 1, 500 us = 0 (as sliced; the device's bits are the
// inverse), 4 x 750 us sync pulses before each of ~15 copies of 40 bits.
// After inverting: id, battery / test / channel + temperature (12 bit,
// (raw - 900) / 10 F), humidity; byte 4 = sum of bytes 0..3.
void decode_3ch(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pwm(Mod::OOK, 250, 500, 625, 1250, 750);
    // the copies: rows of 40..42 bits agreeing in their first 40 bits
    for (size_t i = 0; i < rs.size(); i++) {
        const Row& r = rs[i];
        if (r.n < 40 || r.n > 42) continue;
        const uint64_t code = r.get(0, 40);
        int same = 0;
        int64_t t0 = r.t0, t1 = r.t1;
        for (const auto& o : rs.r)
            if (o.n >= 40 && o.n <= 42 && o.get(0, 40) == code) {
                same++;
                t0 = std::min(t0, o.t0);
                t1 = std::max(t1, o.t1);
            }
        if (same < 3) continue;
        uint8_t b[5];
        for (int k = 0; k < 5; k++) b[k] = static_cast<uint8_t>(~(code >> (32 - 8 * k)));
        if (((b[0] + b[1] + b[2] + b[3]) & 0xFF) != b[4]) continue;
        const int id = b[0], batt_low = b[1] >> 7, chan = (b[1] >> 4) & 3;
        const double tf = ((((b[1] & 15) << 8) | b[2]) - 900) * 0.1;
        const int hum = b[3];
        if (chan == 0 || hum > 100 || tf < -20.0 || tf > 160.0) continue;
        Msg& m = emit(out, r, "Bresser-3CH", Kind::WEATHER, "CHECKSUM");
        m.t0 = t0;
        m.t1 = t1;
        m.id = std::to_string(id);
        m.num("id", id).num("channel", chan).num("battery_ok", batt_low ? 0 : 1);
        m.num("temperature_F", tf, "%.2f").num("humidity", hum);
        return;
    }
}
ISM_PROTOCOL("Bresser 3CH", Strength::MEDIUM, decode_3ch);

}  // namespace
}  // namespace ism
