// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. The decoders in this file follow rtl_433's
// (https://github.com/merbanan/rtl_433, GPL-2.0-or-later), re-implemented in
// C++ for the KrakenSDR plugin API (2026):
//   src/devices/efergy_e2_classic.c - Copyright (C) 2015 Robert Högberg
//     <robert.hogberg@gmail.com>
//   src/devices/current_cost.c - Copyright (C) 2015 Emmanuel Navarro
//     <enavarro222@gmail.com>; CurrentCost EnviR added by Neil Cowburn
//     <git@neilcowburn.com>

// Energy monitors: Efergy e2 classic (current clamp) and CurrentCost TX /
// OptiSmart / EnviR (power and impulse-counter transmitters).

#include "ism.hpp"

namespace ism {
namespace {

// ------------------------------------------------------------ Efergy e2 ---
// FSK, the width of each higher-tone pulse is the bit (~64 us = 1, ~136 us =
// 0), a ~500 us pulse syncs. 8 bytes: 00 | id lo | id hi | flags | current
// hi | current lo | exponent | sum. Both tones carry the bits (the lower one
// complemented), so a frame may arrive inverted: it must start with 0000 or
// 1111. Check: the 8-bit sum of bytes 0..6.
// Sliced here from the pulses (1 below 100 us, 0 below 180 us) so that one
// pulse merged with the next one (the lower-tone gap between them missed:
// ~180..450 us) can be tried as each of the four bit pairs; the checksum
// decides, and only a single passing pair is taken.
bool efergy_frame(const std::vector<int>& bits, const Row& span, std::vector<Msg>& out) {
    if (bits.size() < 64 || bits.size() > 65) return false;
    Row row = span;
    row.b.clear();
    row.n = 0;
    for (int v : bits) row.push(v);
    const int top = row.get(0, 4);
    if (top != 0 && top != 0xF) return false;
    std::vector<uint8_t> b = row.bytes(0, 8);
    if (top == 0xF)
        for (auto& v : b) v = static_cast<uint8_t>(~v);
    int zeros = 0;
    for (auto v : b) zeros += v == 0;
    if (zeros > 5) return false;
    const int sum = add_bytes(b.data(), 7);
    if (sum == 0 || (sum & 0xFF) != b[7]) return false;
    const int fact = 15 - static_cast<int8_t>(b[6]);
    if (fact < 7 || fact > 23) return false;
    const double amps = ((b[4] << 8) | b[5]) / static_cast<double>(1 << fact);
    const int id = (b[2] << 8) | b[1];
    Msg& m = emit(out, row, "Efergy-e2CT", Kind::ENERGY, "CHECKSUM");
    m.id = std::to_string(id);
    m.num("battery_ok", (b[3] >> 6) & 1);
    m.num("current", amps, "%.2f");
    m.num("interval", (((b[3] >> 4) & 3) + 1) * 6);
    m.str("learn", (b[3] & 0x80) ? "YES" : "NO");
    return true;
}

void decode_efergy(Pkt& pk, std::vector<Msg>& out) {
    const auto& ps = pk.pulses(Mod::FSK);
    if (ps.size() < 64) return;
    const int one_max = us2s(100), zero_max = us2s(318), gap_max = us2s(200);   // 318: the sync above
    for (size_t i = 0; i < ps.size(); i++) {
        // a sync pulse, then the bits up to a long lower-tone gap
        if (ps[i].on <= zero_max || ps[i].on > us2s(800) || ps[i].off > gap_max) continue;
        std::vector<int> bits;
        int merged = -1;
        Row span;
        span.t0 = ps[i].t;
        size_t k = i + 1;
        for (; k < ps.size(); k++) {
            const Pulse& p = ps[k];
            if (p.on <= one_max) bits.push_back(1);
            else if (p.on <= us2s(180)) bits.push_back(0);
            else if (p.on <= us2s(450) && merged < 0) {
                merged = static_cast<int>(bits.size());
                bits.push_back(1);
                bits.push_back(1);
            } else break;
            span.t1 = p.t + p.on;
            if (p.off > gap_max) break;
        }
        bool ok = false;
        if (merged < 0) {
            ok = efergy_frame(bits, span, out);
        } else {
            // the pair that passes, if exactly one does
            int pass = 0, which = -1;
            for (int v = 0; v < 4; v++) {
                bits[static_cast<size_t>(merged)] = v >> 1;
                bits[static_cast<size_t>(merged + 1)] = v & 1;
                std::vector<Msg> tmp;
                if (efergy_frame(bits, span, tmp)) {
                    pass++;
                    which = v;
                }
            }
            if (pass == 1) {
                bits[static_cast<size_t>(merged)] = which >> 1;
                bits[static_cast<size_t>(merged + 1)] = which & 1;
                ok = efergy_frame(bits, span, out);
            }
        }
        if (ok) i = k;
    }
}

// ----------------------------------------------------------- CurrentCost ---
// FSK NRZ, 250 us per half bit; the bits are taken inverted (higher tone =
// 0). Classic transmitters: a 45-bit start pattern, EnviR: 0x55 x4 + sync
// word 0x2DD4 (searched inverted, one bit early). Then Manchester ("01" = 1
// on the inverted bits): byte 0 high nibble 0 = power meter (12-bit id, three
// 15-bit watt values each with a valid flag), 4 = impulse counter (12-bit id,
// sensor type, 32-bit count). No checksum: the long start pattern is the
// only proof.
void decode_currentcost(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::FSK).size() < 40) return;
    static const uint8_t envir[6] = {0x55, 0x55, 0x55, 0x55, 0xa4, 0x57};
    static const uint8_t classic[6] = {0xcc, 0xcc, 0xcc, 0xce, 0x91, 0x5d};
    const Rows& rs = pk.pcm(Mod::FSK, 250, 250, 0, 8000, 0);
    for (const Row& raw : rs.r) {
        if (raw.n < 45 + 128) continue;
        const Row row = raw.inverted();
        bool is_envir = false;
        int start = -1;
        const int pe = row.find(envir, 48);
        if (pe >= 0 && pe + 47 + 112 <= row.n) {
            is_envir = true;
            start = pe + 47;
        } else {
            const int pc = row.find(classic, 45);
            if (pc < 0 || pc + 45 + 112 > row.n) continue;
            start = pc + 45;
        }
        const Row d = row.manchester(start, 100000, false);
        if (d.n < 64) continue;
        const auto b = d.bytes(0, 8);
        const int type = b[0] >> 4;
        const int id = ((b[0] & 0x0F) << 8) | b[1];
        if (type == 0) {
            Msg& m = emit(out, raw, is_envir ? "CurrentCost-EnviR" : "CurrentCost-TX", Kind::ENERGY, "");
            m.id = std::to_string(id);
            const char* keys[3] = {"power0_W", "power1_W", "power2_W"};
            for (int k = 0; k < 3; k++) {
                const uint8_t hi = b[static_cast<size_t>(2 + 2 * k)], lo = b[static_cast<size_t>(3 + 2 * k)];
                m.num(keys[k], (hi & 0x80) ? (((hi & 0x7F) << 8) | lo) : 0);
            }
        } else if (type == 4) {
            Msg& m = emit(out, raw, is_envir ? "CurrentCost-EnviRCounter" : "CurrentCost-Counter", Kind::ENERGY, "");
            m.id = std::to_string(id);
            m.num("subtype", b[3]);
            m.num("power0", static_cast<double>((static_cast<uint32_t>(b[4]) << 24) | (b[5] << 16) | (b[6] << 8) | b[7]),
                  "%.0f");
        }
    }
}

}  // namespace
ISM_PROTOCOL("Efergy e2 classic", Strength::MEDIUM, decode_efergy);
ISM_PROTOCOL("CurrentCost", Strength::MEDIUM, decode_currentcost);
}  // namespace ism
