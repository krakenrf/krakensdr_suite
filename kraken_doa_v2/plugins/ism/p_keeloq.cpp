// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/hcs200.c - Copyright (C) 2019, 667bdrm
//   src/devices/hcs361.c - Copyright (C) 2024 Ethan Halsall
//   src/devices/hcs362.c - Copyright (C) 2024 Christian W. Zuckschwerdt
//     <zany@triq.net>

// Microchip KeeLoq code-hopping encoders: HCS200 / HCS300, HCS361, HCS362 -
// car and garage remotes (315 / 433 MHz).
//
// The code word is sent LSB first: 32-bit encrypted hop code, 28-bit serial,
// 4 button bits (in the order S3 S0 S1 S2), the low-battery bit, then
// device-specific status bits. PWM with an elementary period Te: 1 = Te on +
// 2 Te off, 0 = 2 Te on + Te off. A preamble of Te pulses and a header gap
// come first. The hop code is reported, not decrypted.

#include "ism.hpp"

namespace ism {
namespace {

uint32_t lsb_first(const Row& r, int pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v |= static_cast<uint32_t>(r.bit(pos + i)) << i;
    return v;
}

int button_of(const Row& r) {
    // received S3, S0, S1, S2 -> value S3 S2 S1 S0
    return r.bit(60) * 8 + r.bit(63) * 4 + r.bit(62) * 2 + r.bit(61);
}

bool all_ones(const Row& r, int from, int to) {
    for (int i = from; i < to; i++)
        if (!r.bit(i)) return false;
    return true;
}

// HCS361 preamble: >= 6 ones (PWM), or >= 12 alternating bits from a 1 (VPWM)
bool preamble_ok(const Row& p) {
    if (p.n >= 6 && all_ones(p, 0, p.n)) return true;
    if (p.n < 12) return false;
    for (int i = 0; i < p.n; i++)
        if (p.bit(i) != !(i & 1)) return false;
    return true;
}

// HCS361 / HCS362 2-bit CRC over bits 0..64: CRC1' = CRC0 ^ d,
// CRC0' = CRC0 ^ d ^ CRC1, both starting at 0. Returns CRC0 << 1 | CRC1
// (the order they are sent in: bit 65 = CRC0, bit 66 = CRC1).
int crc2(const Row& r, bool flip64) {
    int c0 = 0, c1 = 0;
    for (int i = 0; i < 65; i++) {
        int d = r.bit(i);
        if (i == 64 && flip64) d ^= 1;
        const int n1 = c0 ^ d, n0 = c0 ^ d ^ c1;
        c0 = n0;
        c1 = n1;
    }
    return c0 << 1 | c1;
}

void keeloq_common(Msg& m, const Row& w, int id_digits) {
    m.id = HEXs(lsb_first(w, 32, 28), id_digits);
}

// ------------------------------------------------------------ HCS200/300 ---
// Preamble 12 x (Te on, Te off) = twelve 1s, a header gap of ~10 Te (new
// row), 66 bits: ... | VLOW (64) | repeat flag (65). No check: the exact
// preamble + 66-bit length is the structure.
void hcs200_rows(const Rows& rs, std::vector<Msg>& out) {
    for (size_t i = 1; i < rs.size(); i++) {
        const Row& w = rs[i];
        const Row& pre = rs[i - 1];
        if (w.n != 66 || pre.msg != w.msg) continue;
        if (pre.n < 11 || pre.n > 13 || !all_ones(pre, 0, pre.n)) continue;
        bool ff = true;
        for (int k = 8; k < 64; k++)
            if (!w.bit(k)) { ff = false; break; }
        if (ff) continue;
        const uint32_t serial = lsb_first(w, 32, 28);
        if (serial == 0 || serial == 0x0FFFFFFF) continue;
        const int btn = button_of(w);
        Msg& m = emit(out, w, "Microchip-HCS200", Kind::REMOTE, "");
        keeloq_common(m, w, 7);
        m.t0 = pre.t0;
        m.num("battery_ok", !w.bit(64));
        m.num("button", btn);
        m.num("learn", w.get(60, 4) == 0xF);
        m.num("repeat", w.bit(65));
        m.str("encrypted", HEXs(lsb_first(w, 0, 32), 8));
        m.event = "button " + std::to_string(btn);
    }
}

void decode_hcs200(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() >= 78) hcs200_rows(pk.pwm(Mod::OOK, 370, 772, 1500, 9000, 0, 152), out);
    if (out.empty() && pk.pulses(Mod::FSK).size() >= 78)
        hcs200_rows(pk.pwm(Mod::FSK, 370, 772, 1500, 9000, 0, 152), out);
}

// ---------------------------------------------------------------- HCS361 ---
// Six configurations (wake-up / sync, 833 or 1667 bit/s, or VPWM where
// pulses and gaps both carry bits). 67 bits: ... | VLOW (64) | CRC0 | CRC1.
// The CRC is also accepted with VLOW flipped (it may change after the CRC
// was formed). rtl_433 reports the VLOW bit itself as battery_ok - kept so.
bool hcs361_rows(const Rows& rs, std::vector<Msg>& out) {
    bool any = false;
    for (size_t i = 0; i < rs.size(); i++) {
        const Row& w = rs[i];
        if (w.n != 67) continue;
        // a preamble row before it: Te pulses (all 1s) or, VPWM, 1010...
        if (i == 0 || rs[i - 1].msg != w.msg || !preamble_ok(rs[i - 1])) continue;
        bool ff = true;
        for (int k = 0; k < 64; k++)
            if (!w.bit(k)) { ff = false; break; }
        if (ff) continue;
        const int rx = w.bit(65) << 1 | w.bit(66);
        if (rx != crc2(w, false) && rx != crc2(w, true)) continue;
        const uint32_t serial = lsb_first(w, 32, 28);
        if (serial == 0) continue;
        const int btn = button_of(w);
        Msg& m = emit(out, w, "Microchip-HCS361", Kind::REMOTE, "CRC");
        keeloq_common(m, w, 8);
        m.t0 = rs[i - 1].t0;
        m.num("battery_ok", w.bit(64));
        m.num("button", btn);
        m.str("encrypted", HEXs(lsb_first(w, 0, 32), 8));
        m.event = "button " + std::to_string(btn);
        any = true;
    }
    return any;
}

void decode_hcs361(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 40) return;   // PIWM: ~2 bits per pulse
    if (hcs361_rows(pk.pwm(Mod::OOK, 400, 800, 1200, 7200, 4000, 140), out)) return;
    if (hcs361_rows(pk.pwm(Mod::OOK, 200, 400, 600, 13600, 2000, 70), out)) return;
    if (hcs361_rows(pk.pwm(Mod::OOK, 200, 400, 1200, 6800, 0, 140), out)) return;
    if (hcs361_rows(pk.pwm(Mod::OOK, 100, 200, 600, 6600, 0, 70), out)) return;
    if (hcs361_rows(pk.piwm(Mod::OOK, 400, 800, 45600, 140), out)) return;
    hcs361_rows(pk.piwm(Mod::OOK, 200, 400, 45200, 70), out);
}

// ---------------------------------------------------------------- HCS362 ---
// 69 bits: ... | VLOW (64) | CRC0 | CRC1 | QUEUE0 | QUEUE1. PWM mode: a
// preamble of twelve 1s then the word. Manchester mode: a preamble of 12
// (Te on, Te off), a sync gap, a start bit and the word, Manchester with
// on-then-off = 1, a stop bit breaking the coding.
void hcs362_word(const Row& w, const Row& pre, std::vector<Msg>& out) {
    bool ff = true;
    for (int k = 8; k < 64; k++)
        if (!w.bit(k)) { ff = false; break; }
    if (ff) return;
    const int rx = w.bit(65) << 1 | w.bit(66);
    if (rx != crc2(w, false)) return;
    const int btn = button_of(w);
    Msg& m = emit(out, w, "Microchip-HCS362", Kind::REMOTE, "CRC");
    keeloq_common(m, w, 7);
    m.t0 = pre.t0;
    m.num("battery_ok", !w.bit(64));
    m.num("button", btn);
    m.num("repeat", w.bit(67) + 2 * w.bit(68));
    m.str("encrypted", HEXs(lsb_first(w, 0, 32), 8));
    m.event = "button " + std::to_string(btn);
}

void decode_hcs362(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 40) return;
    const Rows& pw = pk.pwm(Mod::OOK, 200, 400, 550, 900, 0, 50);
    for (size_t i = 1; i < pw.size(); i++) {
        const Row& w = pw[i];
        const Row& pre = pw[i - 1];
        if (w.n != 69 || pre.msg != w.msg || pre.n != 12 || !all_ones(pre, 0, 12)) continue;
        hcs362_word(w, pre, out);
    }
    const Rows& mc = pk.pcm(Mod::OOK, 214, 214, 600, 900, 50);
    for (size_t i = 1; i < mc.size(); i++) {
        const Row& raw = mc[i];
        const Row& pre = mc[i - 1];
        if (pre.msg != raw.msg || pre.n < 24 || pre.n > 32 || pre.get(0, 24) != 0xAAAAAA) continue;
        if (raw.n < 142 || raw.n > 148 || raw.get(0, 2) != 2) continue;
        const Row w = raw.manchester(2, 72, true);
        if (w.n < 69) continue;
        hcs362_word(w, pre, out);
    }
}

}  // namespace
ISM_PROTOCOL("Microchip HCS200/HCS300 KeeLoq", Strength::MEDIUM, decode_hcs200);
ISM_PROTOCOL("Microchip HCS361 KeeLoq", Strength::MEDIUM, decode_hcs361);
ISM_PROTOCOL("Microchip HCS362 KeeLoq", Strength::MEDIUM, decode_hcs362);
}  // namespace ism
