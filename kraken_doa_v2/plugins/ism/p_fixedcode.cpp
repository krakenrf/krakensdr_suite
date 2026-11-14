// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 KrakenRF Inc.
// Part of the KrakenSDR `ism` decoder plugin, licensed under the GNU General
// Public License version 2 or (at your option) any later version - see LICENSE
// in this folder. KrakenSDR's own code, written from the EV1527 / PT2262
// chips' coding (it does not follow rtl_433's generic_remote.c).

// Fixed-code remotes and sensors: EV1527 / HS1527 / RT1527 (20-bit address +
// 4 data bits) and PT2262 / SC2262 (12 tri-state digits) - the coding of
// most cheap 315 / 433 MHz key fobs, door contacts, PIR and water sensors,
// doorbells and wall switches.
//
// OOK, a clock Te set by the chip's resistor (~200..600 us, so it is
// measured, not assumed): every bit = 4 Te with the carrier on for 1 Te (0)
// or 3 Te (1); a frame = 24 bits + a sync (1 Te on, 31 Te off), repeated
// while the button is held / the sensor sends. PT2262 sends each tri-state
// digit as two such bits: 00 = '0', 11 = '1', 01 = 'F' (floating).
// No checksum: a code counts when two frames of the package agree exactly.

#include "ism.hpp"

#include <algorithm>

namespace ism {
namespace {

void decode(Pkt& pk, std::vector<Msg>& out) {
    const auto& ps = pk.pulses(Mod::OOK);
    if (ps.size() < 26) return;
    // frames: the 24 data pulses after a sync (a pulse + a long gap), the
    // next frame's sync pulse (or the end of the burst) after them
    struct Frame { uint32_t code; int64_t t0, t1; double te; };
    std::vector<Frame> fr;
    for (size_t i = 1; i + 24 <= ps.size(); i++) {
        const Pulse& sy = ps[i - 1];
        if (!(sy.off > 6 * sy.on && sy.off > us2s(2500))) continue;
        // the 24 bit periods, the duty cycle of each
        std::vector<int> per(24);
        uint32_t code = 0;
        bool ok = true;
        double sum_short = 0, nshort = 0, sum_long = 0, nlong = 0;
        for (int k = 0; k < 24 && ok; k++) {
            const Pulse& p = ps[i + static_cast<size_t>(k)];
            const int period = p.on + p.off;
            per[static_cast<size_t>(k)] = period;
            const double duty = static_cast<double>(p.on) / period;
            int bit;
            if (duty > 0.08 && duty < 0.42) bit = 0;
            else if (duty > 0.58 && duty < 0.92) bit = 1;
            else { ok = false; break; }
            if (bit == 0) { sum_short += p.on; nshort++; }
            else { sum_long += p.on; nlong++; }
            code = (code << 1) | static_cast<uint32_t>(bit);
        }
        if (!ok || nshort == 0 || nlong == 0) continue;
        const double ws = sum_short / nshort, wl = sum_long / nlong;
        if (wl < 2.0 * ws || wl > 4.5 * ws) continue;
        // periods consistent
        std::vector<int> sp = per;
        std::nth_element(sp.begin(), sp.begin() + 12, sp.end());
        const double med = sp[12];
        for (int k = 0; k < 24; k++)
            if (std::abs(per[static_cast<size_t>(k)] - med) > 0.3 * med) ok = false;
        if (!ok) continue;
        const double te = med / 4;
        // the sync: ~1 Te on, a long gap (8..50 Te)
        if (std::fabs(sy.on - te) > 0.6 * te || sy.off < 8 * te || sy.off > 50 * te) continue;
        if (i + 24 < ps.size() && !(ps[i + 24].off > 6 * te)) continue;
        const Pulse& tail = ps[i + 23];
        fr.push_back({code, ps[i].t, tail.t + tail.on, s2us(te)});
    }
    if (fr.size() < 2) return;
    // a code seen twice
    for (size_t a = 0; a < fr.size(); a++) {
        int n = 0;
        int64_t t0 = fr[a].t0, t1 = fr[a].t1;
        for (const auto& f : fr)
            if (f.code == fr[a].code) {
                n++;
                t0 = std::min(t0, f.t0);
                t1 = std::max(t1, f.t1);
            }
        if (n < 2) continue;
        const uint32_t c = fr[a].code;
        if (c == 0 || c == 0xFFFFFF) return;
        // PT2262 when every digit pair is 00 / 11 / 01
        bool tri = true;
        std::string ts;
        for (int d = 11; d >= 0; d--) {
            const int v = (c >> (2 * d)) & 3;
            if (v == 2) { tri = false; break; }
            ts += v == 0 ? '0' : v == 3 ? '1' : 'F';
        }
        Row r;
        r.t0 = t0;
        r.t1 = t1;
        if (tri) {
            Msg& m = emit(out, r, "PT2262", Kind::REMOTE, "REPEAT");
            m.id = ts.substr(0, 8);
            m.str("tristate", ts);
            m.str("data", ts.substr(8));
            m.str("code", hexs(c, 6));
            m.num("te_us", std::round(fr[a].te), "%.0f");
            m.event = "data " + ts.substr(8);
        } else {
            Msg& m = emit(out, r, "EV1527", Kind::REMOTE, "REPEAT");
            m.id = hexs(c >> 4, 5);
            m.str("key", hexs(c & 15, 1));
            m.str("code", hexs(c, 6));
            m.num("te_us", std::round(fr[a].te), "%.0f");
            m.event = "key " + hexs(c & 15, 1);
        }
        return;   // one code per package
    }
}

}  // namespace
ISM_PROTOCOL("EV1527 / PT2262 fixed code", Strength::WEAK, decode);
}  // namespace ism
