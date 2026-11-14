// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/hideki.c - rtl_433 contributors

// Hideki TS04 thermo-hygro, anemometer, rain gauge and thermometer (also sold
// as TFA, Cresta, Bresser 5CH 7009993), 433.92 MHz.
//
// OOK bi-phase (differential Manchester) with 520 us half bits, each package
// sent 3 times. As sliced (two half-bit symbols = 1, one full-bit symbol = 0)
// a row starts with the sync 0 0000 1101; then come 9-bit groups of an
// inverted byte + even parity bit. The type follows from the number of
// groups (8 temperature, 9 rain, 10 TS04, 14 wind incl. the sync). Checks:
// parity per byte, the XOR of all bytes up to the XOR byte = 0, CRC-8 (poly
// 0x07, init 0) over all bytes incl. the CRC byte = 0. The fields are read
// from the bit-reversed bytes.

#include "ism.hpp"

#include <array>

namespace ism {
namespace {

void decode_hideki(Pkt& pk, std::vector<Msg>& out) {
    static const int dir_tab[16] = {0, 15, 13, 14, 9, 10, 12, 11, 1, 2, 4, 3, 8, 7, 5, 6};
    static const int approach_tab[4] = {0, 1, -1, 2};
    const Rows& rs = pk.dmc(Mod::OOK, 520, 1040, 4000, 240);
    for (const auto& r : rs.r) {
        const int groups = (r.n + 4) / 9;
        if (groups != 8 && groups != 9 && groups != 10 && groups != 14) continue;
        const int nb = groups - 1;   // bytes after the sync
        // the sync, allowing up to 3 lost leading bits
        uint32_t v = static_cast<uint32_t>(r.get(0, 9));
        int start = -1;
        for (int i = 0; i < 4; i++, v >>= 1)
            if (v == 0x0D) {
                start = 9 - i;
                break;
            }
        if (start < 0) continue;
        std::array<uint8_t, 13> p{};
        bool ok = true;
        for (int j = 0; j < nb && ok; j++) {
            const int o = start + 9 * j;
            const uint8_t byte = static_cast<uint8_t>(~r.get(o, 8));
            const int par = 1 - r.bit(o + 8);
            if (par != parity8(byte)) ok = false;
            p[static_cast<size_t>(j)] = byte;
        }
        if (!ok) continue;
        if (xor_bytes(p.data(), nb - 1) != 0) continue;
        if (crc8(p.data(), nb, 0x07, 0x00) != 0) continue;
        for (int j = 0; j < nb; j++) p[static_cast<size_t>(j)] = reverse8(p[static_cast<size_t>(j)]);
        if (((p[1] >> 1) & 0x1F) + 2 != nb) continue;
        int chan = (p[0] >> 5) & 7;
        if (chan >= 5) chan -= 1;
        const int rc = p[0] & 15;
        int temp = (p[4] & 15) * 100 + (p[3] >> 4) * 10 + (p[3] & 15);
        if (!((p[4] >> 7) & 1)) temp = -temp;
        int batt = (p[4] >> 6) & 1;
        const char* model = groups == 10 ? "Hideki-TS04" : groups == 14 ? "Hideki-Wind" : groups == 9 ? "Hideki-Rain" : "Hideki-Temperature";
        Msg& m = emit(out, r, model, Kind::WEATHER, "CRC");
        m.id = std::to_string(rc);
        if (groups == 9) batt = (p[1] >> 6) & 1;
        m.num("id", rc).num("channel", chan).num("battery_ok", batt);
        if (groups == 9) {
            m.num("rain_mm", ((p[4] << 8) | p[3]) * 0.7, "%.1f");
            continue;
        }
        m.num("temperature_C", temp * 0.1, "%.1f");
        if (groups == 10) m.num("humidity", (p[5] >> 4) * 10 + (p[5] & 15));
        if (groups == 14) {
            const int avg = (p[8] & 15) * 100 + (p[7] >> 4) * 10 + (p[7] & 15);
            const int gust = (p[9] >> 4) * 100 + (p[9] & 15) * 10 + (p[8] >> 4);
            m.num("wind_avg_mi_h", avg * 0.1, "%.2f").num("wind_max_mi_h", gust * 0.1, "%.2f");
            m.num("wind_approach", approach_tab[(p[10] >> 2) & 3]);
            m.num("wind_dir_deg", dir_tab[p[10] >> 4] * 22.5, "%.1f");
        }
    }
}
ISM_PROTOCOL("Hideki", Strength::STRONG, decode_hideki);

}  // namespace
}  // namespace ism
