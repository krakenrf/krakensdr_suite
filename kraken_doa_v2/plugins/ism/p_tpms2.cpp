// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/schraeder.c - Copyright (C) 2016 Benjamin Larsson and 2017
//     Christian W. Zuckschwerdt <zany@triq.net>
//   src/devices/tpms_gm.c - Copyright (C) 2025 Eric Blevins
//   src/devices/steelmate.c - Copyright (C) 2016 Benjamin Larsson; Copyright
//     (C) 2016 John Jore; Copyright (C) 2025 Bruno OCTAU (ProfBoc75)

// Tyre pressure sensors (TPMS), part 2: the ones sliced as Manchester with
// mid-bit edges (zero-bit slicer: high->low = 1, a 0 in front of every
// row) - Schrader (generic, EG53MA4, MRXBC5A4 / BMW), GM aftermarket and
// Steelmate.

#include "ism.hpp"

#include <algorithm>
#include <cstdlib>

namespace ism {
namespace {

Msg& tpms(std::vector<Msg>& out, const Row& r, const char* model, const char* check, const std::string& id) {
    Msg& m = emit(out, r, model, Kind::TPMS, check);
    m.id = id;
    m.str("type", "TPMS");
    return m;
}

// The payload's start in a row of about the nominal length: where it is
// nominally, or counted back from the row's end (an edge bit lost or added
// in front); -1 = neither. lead_bits before it must equal lead.
int payload_at(const Row& r, int nominal, int nominal_len, int tail, int lead_bits, uint64_t lead) {
    if (std::abs(r.n - nominal_len) > 2) return -1;
    const int cand[2] = {nominal, r.n - (nominal_len - nominal)};
    for (int p : cand)
        if (p >= lead_bits && p + nominal_len - nominal - tail <= r.n && r.get(p - lead_bits, lead_bits) == lead)
            return p;
    return -1;
}

// ---------------------------------------------------- Schrader generic ---
// OOK, half bit 120 us. A frame is 68 bits with the slicer's leading 0: 0 +
// three 1s of sync, then 8 bytes: F nibble + flags 8 + id 28 + pressure 8
// (25 mbar) + temp 8 (C + 50) + CRC-8 poly 0x07 init 0xF0 over bytes 0..6.
void decode_schrader(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.manchester_zerobit(Mod::OOK, 120, 480);
    for (const Row& r : rs.r) {
        const int p = payload_at(r, 4, 68, 0, 3, 0x7);   // after the 111 sync
        if (p < 0) continue;
        const auto b = r.bytes(p, 8);
        if ((b[0] >> 4) != 0xF || crc8(b.data(), 7, 0x07, 0xF0) != b[7]) continue;
        const uint32_t id = static_cast<uint32_t>(r.get(p + 12, 28));
        Msg& m = tpms(out, r, "Schrader", "CRC", HEXs(id, 7));
        m.str("flags", hexs(((b[0] & 15) << 4) | (b[1] >> 4), 2));
        m.num("pressure_kPa", b[5] * 2.5, "%.1f");
        m.num("temperature_C", b[6] - 50.0, "%.0f");
    }
}

// ----------------------------------------------------- Schrader EG53MA4 ---
// OOK, half bit 123 us, 315 MHz. 120 bits with the leading 0: 40 bits of
// zeros, then flags 32, id 24, pressure 8 (25 mbar), temp 8 (deg F), and the
// sum of the 9 bytes before it.
void decode_eg53ma4(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.manchester_zerobit(Mod::OOK, 123, 300);
    for (const Row& r : rs.r) {
        const int p = payload_at(r, 40, 120, 0, 32, 0);
        if (p < 0) continue;
        const auto b = r.bytes(p, 10);
        if (!b[1] && !b[2] && !b[4] && !b[5] && !b[7] && !b[8]) continue;
        if ((add_bytes(b.data(), 9) & 0xFF) != b[9]) continue;
        Msg& m = tpms(out, r, "Schrader-EG53MA4", "CHECKSUM", HEXs(r.get(p + 32, 24), 6));
        m.str("flags", hexs(r.get(p, 32), 8));
        m.num("pressure_kPa", b[7] * 2.5, "%.1f");
        m.num("temperature_F", b[8], "%.0f");
    }
}

// --------------------------------------------- Schrader MRXBC5A4 (BMW) ---
// OOK, half bit 123 us. 61 bits with the leading 0: 0 + 15 ones (wake, sync,
// start), flags 3, id 24, pressure 9 (kPa), a 2-bit check C1C2, temp 7
// (C + 50). Check over id + pressure + C1C2 (35 bits): ones at even offsets
// + 2 * all ones - 1 = C1C2 (mod 4). Plus plausible pressure / temperature.
void decode_mrxbc5a4(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.manchester_zerobit(Mod::OOK, 123, 800);
    for (const auto& msg : rs.split()) {
        if (msg.size() != 1) continue;
        const Row& r = msg[0];
        if (r.n != 61 || r.get(0, 16) != 0x7FFF) continue;
        const int D = 16;   // data bit 0
        const uint32_t id = static_cast<uint32_t>(r.get(D + 3, 24));
        if (id == 0 || id == 0xFFFFFF) continue;
        int even = 0, ones = 0;
        for (int i = 0; i < 35; i++)
            if (r.bit(D + 3 + i)) {
                ones++;
                if ((i & 1) == 0) even++;
            }
        if (((even + 2 * ones - 1) & 3) != static_cast<int>(r.get(D + 36, 2))) continue;
        const int flags = static_cast<int>(r.get(D, 3));
        const int kpa = static_cast<int>(r.get(D + 27, 9));
        const int t = static_cast<int>(r.get(D + 38, 7)) - 50;
        if (kpa > 450 || t < -40 || t > 85) continue;
        Msg& m = tpms(out, r, "Schrader-MRXBC5A4", "PARITY", HEXs(id, 6));
        m.str("flags", hexs(static_cast<uint64_t>(flags), 1));
        m.str("sleep", flags == 2 ? "True" : "False");
        m.num("pressure_kPa", kpa, "%.0f");
        m.num("temperature_C", t, "%.0f");
    }
}

// ------------------------------------------------------ GM aftermarket ---
// OOK, half bit 120 us, 315 MHz. 130 bits with the leading 0: 48 zeros,
// flags 16, id 40, pressure 8 (2.75 kPa), temp 8 (C + 60), sum of the 9
// bytes after the zeros.
void decode_gm(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.manchester_zerobit(Mod::OOK, 120, 15600);
    for (const auto& msg : rs.split()) {
        if (msg.size() != 1) continue;
        const Row& r = msg[0];
        const int p = payload_at(r, 48, 130, 2, 40, 0);   // 2 trailing bits
        if (p < 0) continue;
        const auto b = r.bytes(p, 10);
        bool zero = true;
        for (int i = 0; i < 9; i++) zero = zero && !b[static_cast<size_t>(i)];
        if (zero) continue;
        if ((add_bytes(b.data(), 9) & 0xFF) != b[9]) continue;
        const int flags = b[0] << 8 | b[1];
        const uint64_t id = r.get(p + 16, 40);
        Msg& m = tpms(out, r, "GM-Aftermarket", "CHECKSUM", std::to_string(id & 0xFFFFFFFFull));
        m.num("flags", flags, "%.0f");
        m.num("learn_mode", ((flags >> 8) & 1) == 0 && ((flags >> 1) & 1) == 0 && (flags & 1) == 0 ? 1 : 0, "%.0f");
        m.num("battery_ok", (flags >> 5) & 1 ? 0 : 1, "%.0f");
        m.num("pressure_kPa", b[7] * 2.75, "%.2f");
        m.num("temperature_C", b[8] - 60.0, "%.0f");
    }
}

// ----------------------------------------------------------- Steelmate ---
// FSK, half bit 50 us. 72 bits (often behind a run of zeros): sync 16, the
// rest inverted and each byte sent LSB first: preamble 0x01, id 16,
// pressure 8 (bar * 32), temp 8 (C + 50), battery 8 (mV = 3900 - 10 v;
// FF = fast leak, FE = slow leak), sum of the 6 bytes from the preamble.
void decode_steelmate(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.manchester_zerobit(Mod::FSK, 50, 120);
    for (const Row& r : rs.r) {
        if (r.n < 72) continue;
        const int p = r.find(0x00007F, 24, 0);
        if (p < 0 || p > r.n - 72 || r.n - (p + 72) > 2) continue;
        uint8_t b[9];
        for (int i = 0; i < 9; i++) b[i] = reverse8(static_cast<uint8_t>(~r.byte(p + 8 * i)));
        if ((add_bytes(b + 2, 6) & 0xFF) != b[8]) continue;
        Msg& m = tpms(out, r, "Steelmate", "CHECKSUM", "0x" + hexs(static_cast<uint64_t>(b[3] << 8 | b[4]), 4));
        m.num("pressure_kPa", b[5] * 3.125, "%.3f");
        m.num("temperature_C", b[6] - 50, "%.0f");
        if (b[7] < 0xFE) m.num("battery_mV", 3900 - 10 * b[7], "%.0f");
        if (b[7] == 0xFF || b[7] == 0xFE) {
            m.str("alarm", b[7] == 0xFF ? "fast leak" : "slow leak");
            m.event = b[7] == 0xFF ? "fast leak" : "slow leak";
            m.alarm = true;
        }
    }
}

}  // namespace

ISM_PROTOCOL("Schrader TPMS", Strength::STRONG, decode_schrader);
ISM_PROTOCOL("Schrader EG53MA4 TPMS", Strength::MEDIUM, decode_eg53ma4);
ISM_PROTOCOL("Schrader MRXBC5A4 TPMS", Strength::MEDIUM, decode_mrxbc5a4);
ISM_PROTOCOL("GM aftermarket TPMS", Strength::MEDIUM, decode_gm);
ISM_PROTOCOL("Steelmate TPMS", Strength::MEDIUM, decode_steelmate);

}  // namespace ism
