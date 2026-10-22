// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/fordremote.c - rtl_433 contributors
//   src/devices/somfy_rts.c - Copyright (C) 2020 Matthias Schulz
//     <mschulz@seemoo.tu-darmstadt.de>
//   src/devices/newkaku.c - rtl_433 contributors
//   src/devices/nexa.c - Copyright (C) 2017 Christian Juncker Brædstrup
//   src/devices/proove.c - Copyright (C) 2016 Ask Jakobsen, Christian Juncker
//     Brædstrup
//   src/devices/kerui.c - Copyright (C) 2016 Karl Lattimer
//   src/devices/chuango.c - Copyright (C) 2015 Tommy Vestermark
//   src/devices/smoke_gs558.c - Copyright (C) 2017 Christian W. Zuckschwerdt
//     <zany@triq.net>
//   src/devices/generic_motion.c - Copyright (C) 2015 Christian W.
//     Zuckschwerdt <zany@triq.net>

// Remotes and simple sensors with a protocol of their own: Ford car key,
// Somfy RTS (blinds), self-learning KlikAanKlikUit / Nexa / Proove (HomeEasy
// style), and fixed-code sensors on the EV1527 waveform whose codes carry a
// meaning: Kerui, Chuango, the GS 558 smoke detector and a generic 433.3 MHz
// motion sensor. Plain EV1527 / PT2262 codes are p_fixedcode.cpp's.

#include "ism.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace ism {
namespace {

// ------------------------------------------------------------------ Ford ---
// OOK differential Manchester, 250 us half bit. A packet = rows of 1, 9
// (eight 0s and a 1) and 1 bit, then >= 78 data bits: a 24-bit id and a
// code byte (rolling code, not decrypted). No check: the row structure only.
void decode_ford(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 40) return;
    const Rows& rs = pk.dmc(Mod::OOK, 250, 500, 4000, 50);
    for (size_t i = 3; i < rs.size(); i++) {
        const Row& d = rs[i];
        if (d.n < 78 || rs[i - 3].msg != d.msg) continue;
        if (rs[i - 3].n != 1 || rs[i - 1].n != 1 || rs[i - 2].n != 9 || rs[i - 2].get(0, 8) != 0) continue;
        const uint32_t id = static_cast<uint32_t>(d.get(0, 24));
        const int code = d.byte(56);
        Msg& m = emit(out, d, "Ford-CarRemote", Kind::REMOTE, "");
        m.t0 = rs[i - 3].t0;
        m.id = std::to_string(id);
        m.num("code", code);
        m.event = "code " + std::to_string(code);
    }
}

// ----------------------------------------------------------------- Somfy ---
// OOK, unit ~604 us, sliced NRZ. Preamble: wake-up pulse (first frame only),
// hardware syncs (4 units on, 4 off; 2 in the first frame, ~7 in repeats), a
// software sync (8 on, 1 off), then 56 Manchester bits ("01" = 1). The 7
// bytes are chained by XOR (each one XOR the received byte before it); after
// undoing that: key | command << 4 | checksum | counter (16, big endian) |
// address (24, little endian). Check: all 14 nibbles XOR to 0.
const char* somfy_ctrl(int c) {
    static const char* t[16] = {"? (0)", "My (1)", "Up (2)", "My + Up (3)", "Down (4)", "My + Down (5)",
                                "Up + Down (6)", "My + Up + Down (7)", "Prog (8)", "Sun + Flag (9)", "Flag (10)",
                                "? (11)", "? (12)", "? (13)", "? (14)", "? (15)"};
    return t[c & 15];
}
const char* somfy_seed(int c) {
    static const char* t[16] = {"? (0)", "? (1)", "? (2)", "? (3)", "? (4)", "Stop (5)", "Up (6)", "? (7)",
                                "Down (8)", "? (9)", "? (10)", "? (11)", "Prog (12)", "? (13)", "? (14)", "? (15)"};
    return t[c & 15];
}

void decode_somfy(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 40) return;
    const Rows& rs = pk.pcm(Mod::OOK, 604, 604, 3000, 10000, 20);
    for (const Row& row : rs.r) {
        if (row.n < 24 + 112) continue;
        // a hardware sync, then the software sync: 1111 0000 + 7..9 ones + 0
        int pos = -1, at = -1;
        for (int ones : {8, 7, 9}) {
            const uint64_t pat = (0xF0ull << (ones + 1)) | (((1ull << ones) - 1) << 1);
            const int p = row.find(pat, 8 + ones + 1);
            if (p >= 0 && p + 8 + ones + 1 + 112 <= row.n) {
                pos = p + 8 + ones + 1;
                at = p;
                break;
            }
        }
        if (pos < 0) continue;
        const Row d = row.manchester(pos, 80, false);
        if (d.n < 56) continue;
        auto b = d.bytes(0, 7);
        for (int i = 6; i > 0; i--) b[static_cast<size_t>(i)] ^= b[static_cast<size_t>(i - 1)];
        int x = xor_bytes(b.data(), 7);
        if (((x >> 4) ^ x) & 15) continue;
        // hardware syncs before the software sync: 2 in the first frame
        int hw = 0;
        for (int q = at; q >= 0 && row.get(q, 8) == 0xF0; q -= 8) hw++;
        const int seed = b[0], ctrl = b[1] >> 4;
        const uint32_t addr = (static_cast<uint32_t>(b[6]) << 16) | (b[5] << 8) | b[4];
        const std::string cs = ctrl == 0xF ? somfy_seed(seed) : somfy_ctrl(ctrl);
        Msg& m = emit(out, row, "Somfy-RTS", Kind::REMOTE, "CHECKSUM");
        m.id = std::to_string(addr);
        m.str("control", cs);
        m.num("counter", (b[2] << 8) | b[3]);
        m.num("retransmission", hw > 2);
        std::string ev = cs.substr(0, cs.find(" ("));
        for (auto& c : ev) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        m.event = ev;
    }
}

// --------------------------------------------- KlikAanKlikUit / Nexa / Proove
// OOK PPM, T ~250..300 us pulses: gap ~T = 0, ~5 T = 1, a ~10 T sync gap
// before the frame, ~40 T after it. Every data bit is two symbols: 01 = 0,
// 10 = 1 (11 in the on/off position = dim, KAKU). 32 bits: 26-bit id | group
// | on | 4 bits unit (KAKU) or channel and unit, both inverted (Nexa,
// Proove); 72-symbol frames carry 4 more bits (dim level / Nexa "on").
// No checksum: 32 valid symbol pairs after a sync gap. The three rtl_433
// decoders take the same frames; one message is made here: KAKU when the
// timing is nearer its nominal 1400 / 2650+ us (long / sync gap), else Nexa
// for 72-symbol frames and Proove for 64-symbol ones.
void decode_kaku(Pkt& pk, std::vector<Msg>& out) {
    const auto& ps = pk.pulses(Mod::OOK);
    if (ps.size() < 64) return;
    const Rows& rs = pk.ppm(Mod::OOK, 285, 1350, 0, 3200, 2700, 250);
    for (const Row& row : rs.r) {
        if (row.n != 64 && row.n != 72) continue;
        // the pulse that starts the row, the sync gap before it
        auto it = std::lower_bound(ps.begin(), ps.end(), row.t0, [](const Pulse& p, int64_t t) { return p.t < t; });
        if (it == ps.begin() || it == ps.end() || it->t != row.t0) continue;
        const Pulse& sy = *(it - 1);
        const double sync_us = s2us(sy.off);
        if (sync_us < 2400 || sync_us > 2950) continue;
        bool dim = false, ok = true;
        uint64_t d = 0;
        const int nb = row.n / 2;
        for (int k = 0; k < nb; k++) {
            const int a = row.bit(2 * k), c = row.bit(2 * k + 1);
            if (a == c) {
                if (k == 27 && a == 1) { dim = true; d = d << 1 | 1; continue; }
                ok = false;
                break;
            }
            d = d << 1 | static_cast<uint64_t>(a);
        }
        if (!ok) continue;
        // the long gaps' width
        std::vector<int> lg;
        for (auto p = it; p != ps.end() && p->t <= row.t1; ++p)
            if (s2us(p->off) > 800 && s2us(p->off) < 2000) lg.push_back(p->off);
        if (lg.empty()) continue;
        std::nth_element(lg.begin(), lg.begin() + static_cast<long>(lg.size() / 2), lg.end());
        const double long_us = s2us(lg[lg.size() / 2]);
        const uint64_t w = nb == 36 ? d >> 4 : d;
        const uint32_t id = static_cast<uint32_t>(w >> 6);
        const int group = (w >> 5) & 1, on = (w >> 4) & 1, low = w & 15;
        const bool kaku = dim || (long_us > 1350 && sync_us > 2770);
        Msg* mp;
        if (kaku) {
            Msg& m = emit(out, row, "KlikAanKlikUit-Switch", Kind::REMOTE, "");
            m.id = std::to_string(id);
            m.num("unit", low);
            m.str("group_call", group ? "Yes" : "No");
            m.str("command", on ? "On" : "Off");
            m.str("dim", dim ? "Yes" : "No");
            m.num("dim_value", nb == 36 ? static_cast<double>(d & 15) : 0.0);
            m.event = dim ? "dim" : on ? "on" : "off";
            mp = &m;
        } else {
            Msg& m = emit(out, row, nb == 36 ? "Nexa-Security" : "Proove-Security", Kind::SECURITY, "");
            m.id = std::to_string(id);
            m.num("channel", (low >> 2) ^ 3);
            m.str("state", on ? "ON" : "OFF");
            m.num("unit", (low & 3) ^ 3);
            m.num("group", group);
            m.event = on ? "on" : "off";
            mp = &m;
        }
        if (group) mp->event += " (group)";
    }
}

// ----------------------------------------- EV1527-waveform security sensors
// 24 pulses (1 unit on + 3 off = short, 3 on + 1 off = long) and a sync (1
// unit on, ~31 off). Taken inverted (short = 0): 20-bit id | 4-bit code.

// Kerui (also HS1527 door contacts with the code pin tied): codes A motion,
// E open, 7 close, B tamper, 5 water, F low battery. Structure: 25-bit rows
// (the sync pulse last) repeated, a known code.
void decode_kerui(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 75) return;
    const Rows& rs = pk.pwm(Mod::OOK, 420, 960, 1100, 9900, 0, 180);
    for (const Rows& msg : rs.split()) {
        const int i = msg.repeated(3, 25);
        if (i < 0) continue;
        const Row& row = msg[static_cast<size_t>(i)];
        if (row.n != 25 || !row.bit(24) || row.get(0, 24) == 0) continue;
        const uint32_t c = static_cast<uint32_t>(~row.get(0, 24)) & 0xFFFFFF;
        const int cmd = c & 15;
        const char* st;
        switch (cmd) {
        case 0xA: st = "motion"; break;
        case 0xE: st = "open"; break;
        case 0x7: st = "close"; break;
        case 0xB: st = "tamper"; break;
        case 0x5: st = "water"; break;
        case 0xF: st = "battery"; break;
        default: st = nullptr;
        }
        if (!st) continue;
        Msg& m = emit(out, row, "Kerui-Security", Kind::SECURITY, "REPEAT");
        m.id = std::to_string(c >> 4);
        m.num("cmd", cmd);
        if (cmd == 0xA) m.num("motion", 1);
        if (cmd == 0xE) m.num("opened", 1);
        if (cmd == 0x7) m.num("opened", 0);
        if (cmd == 0xB) m.num("tamper", 1);
        if (cmd == 0x5) m.num("water", 1);
        if (cmd == 0xF) m.num("battery_ok", 0);
        m.str("state", st);
        m.event = cmd == 0x7 ? "closed" : cmd == 0xF ? "low battery" : st;
        m.alarm = cmd == 0xB || cmd == 0x5;
        for (const Row& r : msg.r)
            if (r.same(row)) {
                m.t0 = std::min(m.t0, r.t0);
                m.t1 = std::max(m.t1, r.t1);
            }
    }
}

// Chuango (G5 alarm, DWC/PIR/SMK/WI/KP/RC): Te ~568 us; every frame its own
// message (the ~17 ms sync gap). Structure: 25 bits with the sync pulse last,
// the same code in >= 2 frames, Chuango's long pulse width (1704 +- 180 us).
void decode_chuango(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 75) return;
    const Rows& rs = pk.pwm(Mod::OOK, 568, 1704, 0, 1800, 0, 180);
    std::map<uint32_t, std::vector<const Row*>> seen;
    for (const Row& r : rs.r) {
        if (r.n != 25 || !r.bit(24)) continue;
        const uint32_t c = static_cast<uint32_t>(~r.get(0, 24)) & 0xFFFFFF;
        if ((c >> 4) == 0) continue;
        seen[c].push_back(&r);
    }
    for (const auto& [c, rows] : seen) {
        if (rows.size() < 2) continue;
        static const char* names[16] = {"Test", "Disarm", "Alarm", "Tamper", "Home Mode", "On", "Home Mode Zone",
                                        "Normal Zone", "Arm", "?", "Single Delay Zone", "24H Zone", "Closing",
                                        "Low Battery", "?", "?"};
        const int cmd = c & 15;
        Msg& m = emit(out, *rows[0], "Chuango-Security", Kind::SECURITY, "REPEAT");
        m.t1 = rows.back()->t1;
        m.id = std::to_string(c >> 4);
        m.str("cmd", names[cmd]);
        m.num("cmd_id", cmd);
        std::string ev = names[cmd];
        for (auto& ch : ev) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        m.event = ev;
        m.alarm = cmd == 2 || cmd == 3;
    }
}

// GS 558 smoke detector (Siterwell, VisorTech RWM-460.f): a wake-up pulse +
// ~11.8 ms gap, 24 bits, two short stop pulses; repeated 8 x. Bits taken
// inverted and in reverse order: unit (5) | group id (15) | 4 more bits.
// Pairing frames are 0x555555. Structure: the stop pulses (26 / 27-bit rows
// ending in short pulses), a code in >= 3 frames.
void decode_gs558(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 60) return;
    const Rows& rs = pk.pwm(Mod::OOK, 436, 1202, 1949, 14117, 0, 0);
    for (const Rows& msg : rs.split()) {
        if (msg.size() < 3) continue;
        int learn = 0;
        std::map<uint32_t, std::vector<const Row*>> seen;
        for (const Row& r : msg.r) {
            if (r.n != 26 && r.n != 27) continue;
            if (!r.bit(24) || !r.bit(25) || (r.n == 27 && !r.bit(26))) continue;   // the stop (+ wake-up) pulses
            const uint32_t inv = static_cast<uint32_t>(~r.get(0, 24)) & 0xFFFFFF;
            if (inv == 0x555555) {
                learn++;
                continue;
            }
            seen[inv].push_back(&r);
        }
        for (const auto& [inv, rows] : seen) {
            if (rows.size() < 3) continue;
            uint32_t code = 0;
            for (int i = 0; i < 24; i++) code |= ((inv >> (23 - i)) & 1u) << i;   // reversed
            const int unit = code & 0x1F;
            const int id = (code >> 5) & 0x7FFF;
            if (id == 0 || id == 0x7FFF) continue;
            Msg& m = emit(out, *rows[0], "Smoke-GS558", Kind::SECURITY, "REPEAT");
            m.t1 = rows.back()->t1;
            m.id = std::to_string(id);
            m.num("unit", unit);
            m.num("learn", learn > 1);
            m.str("code", hexs(code, 6));
            m.event = learn > 1 ? "pairing" : "smoke alarm";
            m.alarm = learn <= 1;
        }
    }
}

// Generic 433.3 MHz motion sensor / alarm: a long warm-up pulse, then 8
// packets of 20 bits whose timing alternates between two sets (only one set
// matches these widths: 888 us = 1, 1332 us = 0). Structure: the same 20
// bits >= 3 times, not all 0 / all 1 after the first byte.
void decode_generic_motion(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 60) return;
    const Rows& rs = pk.pwm(Mod::OOK, 888, 1558, 1200, 4086, 2454, 0);
    for (const Rows& msg : rs.split()) {
        for (size_t i = 0; i < msg.size(); i++) {
            const Row& r = msg[i];
            if (r.n != 20) continue;
            const uint32_t tail = static_cast<uint32_t>(r.get(8, 12));
            if (tail == 0 || tail == 0xFFF || msg.count_same(i) < 3) continue;
            const uint32_t code = static_cast<uint32_t>(r.get(0, 20));
            Msg& m = emit(out, r, "Generic-Motion", Kind::SECURITY, "REPEAT");
            for (const Row& x : msg.r)
                if (x.same(r)) m.t1 = std::max(m.t1, x.t1);
            m.str("code", hexs(code, 5));
            m.event = "code " + hexs(code, 5);
            break;
        }
    }
}

}  // namespace
ISM_PROTOCOL("Ford car remote", Strength::MEDIUM, decode_ford);
ISM_PROTOCOL("Somfy RTS", Strength::MEDIUM, decode_somfy);
ISM_PROTOCOL("KlikAanKlikUit / Nexa / Proove", Strength::MEDIUM, decode_kaku);
ISM_PROTOCOL("Kerui security", Strength::MEDIUM, decode_kerui);
ISM_PROTOCOL("Chuango security", Strength::MEDIUM, decode_chuango);
ISM_PROTOCOL("GS 558 smoke detector", Strength::MEDIUM, decode_gs558);
ISM_PROTOCOL("Generic motion sensor", Strength::MEDIUM, decode_generic_motion);
}  // namespace ism
