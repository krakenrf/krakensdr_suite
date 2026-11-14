// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/lacrosse_tx35.c - rtl_433 contributors
//   src/devices/lacrosse_tx22uit.c - Copyright (C) 2020 Caz Yokoyama, caz at
//     caztech dot com

// LaCrosse "IT+" sensors: TX29-IT / TFA 30.3159.IT / TX25U (17.24 kbit/s),
// TX35DTH-IT / TFA 30.3155 (9.58 kbit/s), and the TX22U-IT / Technoline
// TX22-IT multi sensor (8.62 kbit/s; FSK, also an OOK variant).

#include "ism.hpp"

#include <algorithm>
#include <cmath>

namespace ism {
namespace {

// 868 / 915 MHz FSK with ~120 kHz between the tones, NRZ, preamble aa..,
// sync 2d d4.
//
// The bit period (us) of an NRZ burst: a low percentile of its pulse and gap
// widths, most of which are single bits. The slicer adapts its bit width to
// the preamble, so this keeps a frame at another bit rate (e.g. a Bresser or
// Fine Offset frame with the same sync) out of these decoders.
double nrz_bit_us(const std::vector<Pulse>& ps) {
    std::vector<int> d;
    for (size_t i = 1; i + 1 < ps.size(); i++) {
        d.push_back(ps[i].on);
        d.push_back(ps[i].off);
    }
    if (d.size() < 16) return 0;
    const size_t k = d.size() * 3 / 10;
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(k), d.end());
    return s2us(d[k]);
}
inline bool rate_ok(const std::vector<Pulse>& ps, double bit_us, double tol) {
    return std::fabs(nrz_bit_us(ps) - bit_us) <= tol * bit_us;
}

// ------------------------------------------------------ TX29-IT / TX35 ---
// Preamble ..1010 (its end is often damaged: not required), sync 2d d4, then
// 40 bits starting with the model nibble 9:
// 9 | id (6 bit) | new battery | ? | temperature (3 BCD digits, +40 C,
// 0.1 C) | weak battery | humidity (7 bit; 0x6A = none, 0x7D = TX25U probe
// channel) | CRC-8 (poly 0x31, init 0) of the first 4 bytes.
void it_plus(Pkt& pk, std::vector<Msg>& out, int bit_us, const char* model) {
    // TX29 ~58 us, TX35 ~104 us per bit
    if (!rate_ok(pk.pulses(Mod::FSK), bit_us == 55 ? 57.5 : 104.5, 0.08)) return;
    for (const auto& r : pk.pcm(Mod::FSK, bit_us, bit_us, 0, 4000).r) {
        for (int p = r.find(0x2DD49, 20); p >= 0; p = r.find(0x2DD49, 20, p + 1)) {
            const int s = p + 16;
            if (r.n - s < 40) break;
            const std::vector<uint8_t> b = r.bytes(s, 5);
            if (crc8(b.data(), 4, 0x31, 0x00) != b[4]) continue;
            int id = ((b[0] & 15) << 2) | (b[1] >> 6);
            const int newbatt = (b[1] >> 5) & 1;
            const double temp = 10 * (b[1] & 15) + (b[2] >> 4) + 0.1 * (b[2] & 15) - 40.0;
            const int batt_low = b[3] >> 7, hum = b[3] & 0x7F;
            if (hum == 0x7D) id += 0x40;
            Msg& m = emit(out, r, model, Kind::WEATHER, "CRC");
            m.id = std::to_string(id);
            m.num("id", id).num("battery_ok", batt_low ? 0 : 1).num("newbattery", newbatt);
            m.num("temperature_C", temp, "%.1f");
            if (hum != 0x6A && hum != 0x7D) m.num("humidity", hum);
            break;
        }
    }
}
void decode_tx29(Pkt& pk, std::vector<Msg>& out) { it_plus(pk, out, 55, "LaCrosse-TX29IT"); }
void decode_tx35(Pkt& pk, std::vector<Msg>& out) { it_plus(pk, out, 105, "LaCrosse-TX35DTHIT"); }
ISM_PROTOCOL("LaCrosse TX29-IT", Strength::STRONG, decode_tx29);
ISM_PROTOCOL("LaCrosse TX35DTH-IT", Strength::STRONG, decode_tx35);

// ------------------------------------------------------------ TX22U-IT ---
// aa aa 2d d4, then id, flags, 1..3 or 5 "quartets" (type nibble + 12 bits:
// 0 temperature BCD (-400, 0.1 C), 1 humidity BCD, 2 rain count x 0.518 mm,
// 3 wind direction nibble x 22.5 + speed byte 0.1 km/h, 4 gust 0.1 km/h),
// CRC-8 (poly 0x31, init 0) over id..last quartet, an end byte 0x00.
void decode_tx22u(Pkt& pk, std::vector<Msg>& out) {
    bool done = false;
    auto row = [&](const Row& r) {
        if (done) return;
        const int p = r.find(0xAAAA2DD4, 32);
        if (p < 0) return;
        const int s = p + 32;
        const int size = std::min(64, (r.n - s) / 8);
        const std::vector<uint8_t> b = r.bytes(s, size);
        // the number of quartets: 5 (acquisition phase) or 1..3, the one whose
        // CRC fits. The low nibble of the flags byte counts them (seen in
        // every known frame) - required here: a CRC also "fits" a longer
        // frame when only zeros follow, and the IT+ frame (same sync, 4 bytes
        // + CRC-8 0x31) would otherwise read as a 1-quartet frame
        int nq = 0;
        for (int n : {5, 3, 2, 1}) {
            const int len = 2 + 2 * n;
            if (len + 2 <= size && (b[1] & 15) == n && crc8(b.data(), len, 0x31, 0x00) == b[static_cast<size_t>(len)] &&
                b[static_cast<size_t>(len + 1)] == 0x00) {
                nq = n;
                break;
            }
        }
        if (!nq) return;
        int temp = -1, hum = -1, rain = -1, dir = -1, speed = -1, gust = -1, known = 0;
        for (int q = 0; q < nq; q++) {
            const uint8_t a = b[static_cast<size_t>(2 + 2 * q)], c = b[static_cast<size_t>(3 + 2 * q)];
            const int bcd3 = (a & 15) * 100 + (c >> 4) * 10 + (c & 15), bin12 = ((a & 15) << 8) | c;
            switch (a >> 4) {
            case 0: temp = bcd3; known++; break;
            case 1: hum = bcd3; known++; break;
            case 2: rain = bin12; known++; break;
            case 3: dir = static_cast<int>((a & 15) * 22.5); speed = c; known++; break;
            case 4: gust = bin12; known++; break;
            default: break;
            }
        }
        if (!known) return;
        done = true;
        Msg& m = emit(out, r, "LaCrosse-TX22UIT", Kind::WEATHER, "CRC");
        m.id = std::to_string(b[0]);
        m.num("id", b[0]).num("flags", b[1]);
        const double tc = (temp - 400) * 0.1;
        if (temp >= 0 && tc > -40.0 && tc <= 70.0) m.num("temperature_C", tc, "%.1f");
        if (hum > 0 && hum <= 100) m.num("humidity", hum);
        if (rain >= 0) m.num("rain_mm", rain * 0.518, "%.2f");
        if (speed >= 0 && speed * 0.1 <= 200.0) m.num("wind_avg_km_h", speed * 0.1, "%.1f");
        if (gust >= 0 && gust * 0.1 <= 200.0) m.num("wind_gust_km_h", gust * 0.1, "%.1f");
        if (dir >= 0 && dir <= 360) m.num("wind_dir_deg", dir);
    };
    // ~116 us per bit; the OOK transmitter (~120 us) sends the bits inverted
    if (rate_ok(pk.pulses(Mod::FSK), 116, 0.12))
        for (const auto& r : pk.pcm(Mod::FSK, 116, 116, 0, 5900).r) row(r);
    if (!done && rate_ok(pk.pulses(Mod::OOK), 118, 0.12))
        for (const auto& r : pk.pcm(Mod::OOK, 120, 120, 0, 5000).r) row(r.inverted());
}
ISM_PROTOCOL("LaCrosse TX22U-IT", Strength::STRONG, decode_tx22u);

}  // namespace
}  // namespace ism
