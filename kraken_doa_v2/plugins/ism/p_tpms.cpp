// Tyre pressure sensors (TPMS), part 1: the PCM-sliced ones, i.e. NRZ bit
// streams with Manchester or differential Manchester line coding after a
// preamble - Toyota (Pacific PMV-C210), Pacific PMV-107J, Ford, Citroen /
// PSA, Renault, Hyundai VDO, Kia, Hyundai Elantra 2012, Jansite TY02S,
// Jansite Solar, Abarth 124 Spider (VDO TG1C) / Shenzhen EGQ Q85, unbranded
// truck SolarTPMS, Nissan, BMW Gen4/5 + Audi pressure alert, Schrader
// SMD3MA4 / NIS315G3.
//
// protocol notes: rtl_433 documentation; own implementation.
//
// Conventions: FSK pulse = the higher tone = 1. "mc" below is Manchester
// over half-bit pairs from a position: "01" -> 1 / "10" -> 0 (or the
// opposite, which is the same as decoding an inverted row), ending at the
// first equal pair; "dmc" is differential Manchester with a transition at
// every cell start, 1 = no mid-cell transition.

#include "ism.hpp"

#include <algorithm>

namespace ism {
namespace {

// Manchester from bit pos: up to max output bits; a pair with equal halves
// ends it (that pair is read but gives no bit). one_is_10: "10" -> 1.
// *end = position after the last pair read. Bits past the row read as 0.
Row mc(const Row& r, int pos, int max, bool one_is_10, int* end = nullptr) {
    Row o;
    o.t0 = r.t0;
    o.t1 = r.t1;
    const int lim = std::min(r.n, pos + 2 * max);
    int i = pos;
    while (i < lim) {
        const int a = r.bit(i), c = r.bit(i + 1);
        i += 2;
        if (a == c) break;
        o.push(one_is_10 ? a : c);
    }
    if (end) *end = i;
    return o;
}

// Differential Manchester from bit pos (a clock transition at the start of
// every cell; 1 = both halves equal, 0 = a mid-cell transition). The cell
// alignment is found first: while the half-bits alternate they are 0s in
// either alignment; the first two equal half-bits mark a cell. Ends at a
// missing clock transition. *end = position after the last half-bit read.
Row dmc(const Row& r, int pos, int max, int* end) {
    Row o;
    o.t0 = r.t0;
    o.t1 = r.t1;
    const int lim = std::min(r.n, pos + 2 * max);
    int i = pos, prev = 0;
    bool aligned = false;
    while (i < lim) {
        const int a = r.bit(i), b = r.bit(i + 1), c = r.bit(i + 2);
        i += 2;
        if (a != b && b != c) {
            o.push(0);
            continue;
        }
        if (a != b) {           // b, c equal: a cell starts at b
            prev = a;
            i -= 1;
        } else {                // a, b equal: a cell starts at a
            prev = 1 - a;
            i -= 2;
        }
        aligned = true;
        break;
    }
    while (aligned && i < lim) {
        const int a = r.bit(i++);
        if (a == prev) break;   // no clock transition
        const int b = r.bit(i++);
        o.push(a == b ? 1 : 0);
        prev = b;
    }
    *end = i;
    return o;
}

// every match of a preamble in a row, from the start, the next search STEP
// bits after a match (as a decoder walks a bit buffer)
template <class F>
void scan(const Row& r, uint64_t pat, int pbits, int need, int step, F f) {
    for (int p = r.find(pat, pbits, 0); p >= 0 && p + need <= r.n; p = r.find(pat, pbits, p + step)) f(p);
}

Msg& tpms(std::vector<Msg>& out, const Row& r, const char* model, const char* check, const std::string& id) {
    Msg& m = emit(out, r, model, Kind::TPMS, check);
    m.id = id;
    m.str("type", "TPMS");
    return m;
}

// ------------------------------------------------- Toyota (PMV-C210) ---
// FSK 52 us NRZ. Sync 01010101001111, then 72 bits differential Manchester
// (104 us cells): id 32, status bit, pressure 8 (0.25 PSI, -7 PSI), temp 8
// (C + 40), status 7, inverted pressure 8, CRC-8 poly 0x07 init 0x80.
void decode_toyota(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 150) continue;
        // 12 bits from sync bit 3 to the first data half-bit (decoding starts on it)
        scan(r, 0xA9E, 12, 11 + 144, 2, [&](int p) {
            int end;
            const Row d = dmc(r, p + 11, 80, &end);
            if (end - (p + 11) < 144 || d.n < 72) return;
            const auto b = d.bytes(0, 9);
            if (crc8(b.data(), 8, 0x07, 0x80) != b[8]) return;
            const int pr = ((b[4] & 0x7F) << 1) | (b[5] >> 7);
            const int t = ((b[5] & 0x7F) << 1) | (b[6] >> 7);
            if (pr != (b[7] ^ 0xFF)) return;
            const int status = (b[4] & 0x80) | (b[6] & 0x7F);
            Msg& m = tpms(out, r, "Toyota", "CRC", hexs(d.get(0, 32), 8));
            m.num("status", status, "%.0f");
            m.num("pressure_PSI", pr * 0.25 - 7.0, "%.2f");
            m.num("temperature_C", t - 40.0, "%.0f");
        });
    }
}

// ------------------------------------------------------------ PMV-107J ---
// FSK 100 us NRZ, 315 MHz. Preamble 1111110, then 66 bits differential
// Manchester: id 28, battery low, counter 2, unknown, rapid change, failed,
// pressure 8 ((P - 40) * 2.48 kPa), inverted pressure 8, temp 8 (C + 40),
// CRC-8 poly 0x13 init 0 over the 58 bits before it (zero padded in front).
void decode_pmv107j(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 100, 100, 0, 250);
    for (const Row& r : rs.r) {
        if (r.n < 130) continue;
        scan(r, 0x3E, 6, 6 + 132, 2, [&](int p) {
            int end;
            const Row d = dmc(r, p + 6, 70, &end);
            if (end - (p + 6) < 134 || d.n < 66) return;
            uint8_t b[9];
            b[0] = static_cast<uint8_t>(d.get(0, 2));
            for (int i = 1; i < 9; i++) b[i] = d.byte(2 + 8 * (i - 1));
            if (crc8(b, 8, 0x13, 0x00) != b[8]) return;
            if (b[5] != (b[6] ^ 0xFF)) return;
            const uint32_t id = static_cast<uint32_t>(d.get(0, 28));
            const int st = static_cast<int>(d.get(28, 6));
            const int batt_low = (st >> 5) & 1, counter = (st >> 3) & 3, rapid = (st >> 1) & 1, failed = st & 1;
            Msg& m = tpms(out, r, "PMV-107J", "CRC", hexs(id, 8));
            m.num("status", st, "%.0f");
            m.num("battery_ok", batt_low ? 0 : 1, "%.0f");
            m.num("counter", counter, "%.0f");
            m.num("rapid_change", rapid, "%.0f");
            m.str("failed", failed ? "FAIL" : "OK");
            m.num("pressure_kPa", (b[5] - 40.0) * 2.48, "%.2f");
            m.num("temperature_C", b[7] - 40.0, "%.0f");
            if (failed) {
                m.event = "sensor failure";
                m.alarm = true;
            } else if (rapid) {
                m.event = "rapid pressure change";
                m.alarm = true;
            }
        });
    }
}

// --------------------------------------------------------------- Ford ---
// FSK 52 us NRZ, preamble 55 55 55 56, 64 bits Manchester ("10" = 1): id 32,
// pressure 8 (+ a 9th bit in the flags, 0.25 PSI), temp 8 (C + 56, not a
// temperature when bit 7 set), flags 8, sum of bytes 0..6.
void decode_ford(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 140) continue;
        scan(r, 0x5556, 16, 16 + 128, 15, [&](int p) {
            const Row d = mc(r, p + 16, 160, true);
            if (d.n < 64) return;
            const auto b = d.bytes(0, 8);
            if ((add_bytes(b.data(), 7) & 0xFF) != b[7]) return;
            int learn = 0, moving = 0;
            switch (b[6] & 0x4C) {
                case 0x08: learn = 1; break;
                case 0x04: break;
                case 0x44: moving = 1; break;
                default: return;   // an unknown flag pattern
            }
            if (b[6] & 0x90) return;
            const int psi4 = ((b[6] & 0x20) << 3) | b[4];
            Msg& m = tpms(out, r, "Ford", "CHECKSUM", hexs(d.get(0, 32), 8));
            m.num("pressure_PSI", psi4 * 0.25, "%.2f");
            if (!(b[5] & 0x80)) m.num("temperature_C", (b[5] & 0x7F) - 56.0, "%.0f");
            m.num("moving", moving, "%.0f");
            m.num("learn", learn, "%.0f");
            m.str("code", hexs(d.get(32, 24), 6));
            m.str("unknown", "00");
            m.str("unknown_3", hexs(b[6] & 3, 1));
        });
    }
}

// ------------------------------------------------------------ Citroen ---
// FSK 52 us NRZ, preamble 55 55 55 56, 80 bits Manchester ("10" = 1): state 8
// (not checked), id 32, flags 4, repeat 4, pressure 8 (1.364 kPa), temp 8
// (C + 50), battery 8, XOR of bytes 1..9 = 0.
void decode_citroen(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 170) continue;
        scan(r, 0x5556, 16, 16 + 158, 2, [&](int p) {
            const Row d = mc(r, p + 16, 88, true);
            if (d.n < 80) return;
            const auto b = d.bytes(0, 10);
            if (b[6] == 0 || b[7] == 0) return;
            if (xor_bytes(b.data() + 1, 9) != 0) return;
            Msg& m = tpms(out, r, "Citroen", "CHECKSUM", hexs(d.get(8, 32), 8));
            m.str("state", hexs(b[0], 2));
            m.num("flags", b[5] >> 4, "%.0f");
            m.num("repeat", b[5] & 15, "%.0f");
            m.num("pressure_kPa", b[6] * 1.364, "%.3f");
            m.num("temperature_C", b[7] - 50.0, "%.0f");
            m.num("maybe_battery", b[8], "%.0f");
        });
    }
}

// ------------------------------------------------------------ Renault ---
// FSK 52 us NRZ, preamble 55 55 55 56, 72 bits Manchester ("10" = 1): flags 6,
// pressure 10 (0.75 kPa), temp 8 (C + 30), id 24 little-endian, 16 unknown,
// CRC-8 poly 0x07 init 0.
void decode_renault(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 150) continue;
        scan(r, 0x5556, 16, 16 + 142, 15, [&](int p) {
            const Row d = mc(r, p + 16, 160, true);
            if (d.n < 72) return;
            const auto b = d.bytes(0, 9);
            if (crc8(b.data(), 8, 0x07, 0x00) != b[8]) return;
            const uint32_t id = static_cast<uint32_t>(b[5]) << 16 | b[4] << 8 | b[3];
            Msg& m = tpms(out, r, "Renault", "CRC", hexs(id, 6));
            m.str("flags", hexs(b[0] >> 2, 2));
            m.num("pressure_kPa", (((b[0] & 3) << 8) | b[1]) * 0.75, "%.2f");
            m.num("temperature_C", b[2] - 30.0, "%.0f");
        });
    }
}

// -------------------------------------------------------- Hyundai VDO ---
// FSK 52 us NRZ, preamble 55 55 55 56, 80 bits Manchester ("10" = 1): state 8,
// id 32, flags 4, repeat 4, pressure 8 (1.375 kPa), temp 8 (C + 50), battery?
// 8, CRC-8 poly 0x07 init 0xAA over bytes 0..8.
void decode_hyundai_vdo(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 180) continue;
        scan(r, 0x55555556, 32, 32 + 158, 2, [&](int p) {
            const Row d = mc(r, p + 32, 80, true);
            if (d.n < 80) return;
            const auto b = d.bytes(0, 10);
            if (crc8(b.data(), 9, 0x07, 0xAA) != b[9]) return;
            Msg& m = tpms(out, r, "Hyundai-VDO", "CRC", hexs(d.get(8, 32), 8));
            m.num("state", b[0], "%.0f");
            m.num("flags", b[5] >> 4, "%.0f");
            m.num("repeat", b[5] & 15, "%.0f");
            m.num("pressure_kPa", b[6] * 1.375, "%.3f");
            m.num("temperature_C", b[7] - 50.0, "%.0f");
            m.num("maybe_battery", b[8], "%.0f");
        });
    }
}

// ---------------------------------------------------------------- Kia ---
// FSK 50 us NRZ, preamble ED 71, 69 bits Manchester ("01" = 1): unknown 4,
// pressure 8 (PSI * 5), temp 8 (C + 50), id 32, unknown 12, then the top 5
// bits of a CRC-8 (poly 0x07, init 0x76) over the first 64 bits.
void decode_kia(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 50, 50, 0, 200);
    for (const Row& r : rs.r) {
        if (r.n < 150) continue;
        scan(r, 0xED71, 16, 16 + 137, 2, [&](int p) {
            int end;
            const Row d = mc(r, p + 16, 138, false, &end);
            if (end - (p + 16) < 138 || d.n < 69) return;
            const auto b = d.bytes(0, 9);
            if (crc8(b.data(), 8, 0x07, 0x76) != (b[8] & 0xF8)) return;
            Msg& m = tpms(out, r, "Kia", "CRC", hexs(d.get(20, 32), 8));
            m.str("unknown1", hexs(d.get(0, 4), 2));
            m.str("unknown2", hexs(b[7], 3));
            m.num("pressure_PSI", d.get(4, 8) / 5.0, "%.1f");
            m.num("temperature_C", static_cast<double>(d.get(12, 8)) - 50.0, "%.0f");
            std::string raw;
            for (int i = 0; i < 9; i++) raw += hexs(b[static_cast<size_t>(i)], 2);
            m.str("raw", raw);
        });
    }
}

// -------------------------------------------------------- Elantra2012 ---
// TRW GQ4-44T, 315 MHz. FSK 49 us NRZ, preamble 71 55, 64 bits Manchester
// ("01" = 1): pressure 8 (kPa - 60), temp 8 (C + 50), id 32, flags 8,
// CRC-8 poly 0x07 init 0.
void decode_elantra2012(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 49, 49, 0, 200);
    for (const Row& r : rs.r) {
        if (r.n < 140) continue;
        scan(r, 0x7155, 16, 16 + 126, 15, [&](int p) {
            const Row d = mc(r, p + 16, 64, false);
            if (d.n < 64) return;
            const auto b = d.bytes(0, 8);
            if (crc8(b.data(), 8, 0x07, 0x00) != 0) return;
            Msg& m = tpms(out, r, "Elantra2012", "CRC", hexs(d.get(16, 32), 8));
            m.num("pressure_kPa", b[0] + 60.0, "%.0f");
            m.num("temperature_C", b[1] - 50.0, "%.0f");
            m.num("battery_ok", (b[6] & 2) ? 0 : 1, "%.0f");
            m.num("triggered", b[6] & 1, "%.0f");
            m.num("storage", (b[6] >> 2) & 1, "%.0f");
            char fs[8];
            snprintf(fs, sizeof fs, "%x", b[6]);
            m.str("flags", fs);
        });
    }
}

// ---------------------------------------------------- Jansite TY02S ---
// FSK 52 us NRZ, preamble ...55 55 56, 56 bits Manchester ("10" = 1): id 28,
// flags 4, pressure 8 (1.7 kPa), temp 8 (C + 50), and a check byte of
// unknown algorithm. No check: a code must come twice in the package (the
// sensor sends each frame twice back to back).
void decode_jansite(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    struct C { uint64_t v; const Row* r; };
    std::vector<C> seen;
    for (const Row& r : rs.r) {
        if (r.n < 130) continue;
        scan(r, 0x555556, 24, 24 + 110, 2, [&](int p) {
            const Row d = mc(r, p + 24, 56, true);
            if (d.n < 56) return;
            seen.push_back({d.get(0, 56), &r});
        });
    }
    for (size_t i = 0; i < seen.size(); i++) {
        bool twice = false;
        for (size_t j = 0; j < seen.size(); j++)
            if (j != i && seen[j].v == seen[i].v) twice = true;
        if (!twice) continue;
        const uint64_t v = seen[i].v;
        const int id = static_cast<int>(v >> 28);
        if (id == 0 || id == 0xFFFFFFF) continue;
        Msg& m = tpms(out, *seen[i].r, "Jansite", "REPEAT", hexs(static_cast<uint64_t>(id), 7));
        m.num("flags", static_cast<double>((v >> 24) & 15), "%.0f");
        m.num("pressure_kPa", static_cast<double>((v >> 16) & 0xFF) * 1.7, "%.1f");
        m.num("temperature_C", static_cast<double>((v >> 8) & 0xFF) - 50.0, "%.0f");
        m.str("code", hexs(v, 14));
        return;
    }
}

// ---------------------------------------------------- Jansite Solar ---
// FSK 51 us NRZ, 88 bits Manchester ("10" = 1) incl. the sync DD 33 (raw
// A6 A6 5A): id 24, unknown 8, temp 8 (C + 55), pressure 8 (1.6 kPa),
// unknown 8, CRC-16 poly 0x8005 init 0 (big-endian) over the 7 bytes.
void decode_jansite_solar(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 51, 51, 0, 5000);
    for (const Row& r : rs.r) {
        if (r.n < 170) continue;
        scan(r, 0xA6A65A, 24, 174, 2, [&](int p) {
            const Row d = mc(r, p, 88, true);
            if (d.n < 88) return;
            const auto b = d.bytes(0, 11);
            if ((b[0] << 8 | b[1]) != 0xDD33) return;
            if (crc16(b.data() + 2, 7, 0x8005, 0x0000) != (b[9] << 8 | b[10])) return;
            Msg& m = tpms(out, r, "Jansite-Solar", "CRC", hexs(d.get(16, 24), 6));
            m.num("flags", b[5], "%.0f");
            m.num("pressure_kPa", b[7] * 1.6, "%.1f");
            m.num("temperature_C", b[6] - 55.0, "%.0f");
            std::string code;
            for (int i = 2; i < 11; i++) code += hexs(b[static_cast<size_t>(i)], 2);
            m.str("code", code);
        });
    }
}

// ------------------------------- Abarth 124 Spider (VDO TG1C) / EGQ Q85 ---
// FSK 52 us NRZ, preamble ...55 55 56, Manchester ("10" = 1): id 32, flags 8,
// pressure 8, temp 8, status 8, XOR check (bytes 0..8 XOR to 0). TG1C: 72
// bits, 1.38 kPa, C + 50. Q85: 96 bits (+ 0x40 + CRC-16/CCITT-FALSE, LE),
// 3 kPa, C + 55. Told apart by the length of the message.
void decode_abarth124(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        const bool tg1c = r.n > 150 && r.n < 210, q85 = r.n > 210 && r.n < 400;
        if (!tg1c && !q85) continue;
        const int nb = tg1c ? 72 : 96;
        scan(r, 0x555556, 24, 24 + 2 * nb - 2, 2, [&](int p) {
            const Row d = mc(r, p + 24, nb, true);
            if (d.n < nb) return;
            const auto b = d.bytes(0, nb / 8);
            if (xor_bytes(b.data(), 9) != 0) return;
            const double tc = tg1c ? b[6] - 50.0 : b[6] - 55.0;
            if (tg1c ? (tc < -50 || tc > 125) : (tc < -20 || tc > 80)) return;
            if (q85 && crc16(b.data(), 10, 0x1021, 0xFFFF) != (b[11] << 8 | b[10])) return;
            Msg& m = tpms(out, r, tg1c ? "Abarth-124Spider" : "Shenzhen-EGQQ85", tg1c ? "CHECKSUM" : "CRC",
                          hexs(d.get(0, 32), 8));
            m.str("flags", hexs(b[4], 2));
            m.num("pressure_kPa", b[5] * (tg1c ? 1.38 : 3.0), "%.2f");
            m.num("temperature_C", tc, "%.0f");
            m.num("status", b[7], "%.0f");
        });
    }
}

// -------------------------------------------------- truck SolarTPMS ---
// FSK 52 us NRZ, long 55..56 preamble, 76 bits Manchester ("10" = 1): state
// 4, id 32, wheel 8, flags 4 (4 = pressure alert, 3 = battery ok), pressure
// 12 (kPa), temp 8, XOR of the 9 bytes after the state = 0.
void decode_truck(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 52, 52, 0, 150);
    for (const Row& r : rs.r) {
        if (r.n < 170) continue;
        scan(r, 0x555556, 24, 24 + 150, 2, [&](int p) {
            const Row d = mc(r, p + 24, 76, true);
            if (d.n < 76) return;
            const auto b = d.bytes(4, 9);
            if (!b[0] && !b[1] && !b[2] && !b[3]) return;
            if (xor_bytes(b.data(), 9) != 0) return;
            const int flags = b[5] >> 4;
            Msg& m = tpms(out, r, "Truck", "CHECKSUM", hexs(d.get(4, 32), 8));
            m.num("wheel", b[4], "%.0f");
            m.num("pressure_kPa", ((b[5] & 15) << 8) | b[6], "%.0f");
            m.num("temperature_C", b[7], "%.0f");
            if (flags & 4) {
                m.num("pressure_alert", 1, "%.0f");
                m.event = "pressure alert";
                m.alarm = true;
            }
            m.num("battery_ok", (flags & 3) == 3 ? 1 : 0, "%.0f");
            m.str("flags", hexs(static_cast<uint64_t>(flags), 1));
        });
    }
}

// two-bit-group sum of 37 data bits behind a leading pad bit (SMD3MA4 /
// NIS315G3 / Nissan): the bits as 19 pairs, added up
int pair_sum(const Row& d, int lead) {
    int s = lead * 2 + d.bit(0);
    for (int i = 1; i + 1 < 37; i += 2) s += 2 * d.bit(i) + d.bit(i + 1);
    return s;
}

// ------------------------------------------------------------- Nissan ---
// FSK 120 us NRZ, 315 MHz, preamble F5 55 55 55 E, 37 bits Manchester ("10" =
// 1): mode 3, id 24, pressure 8 ((PSI + 3) * 4), 2 more; the 2-bit pair sum
// (a 0 in front) is 3 mod 4.
void decode_nissan(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 120, 120, 0, 250);
    for (const Row& r : rs.r) {
        if (r.n < 100) continue;
        scan(r, 0xF5555555E, 36, 36 + 72, 1, [&](int p) {
            const Row d = mc(r, p + 36, 113, true);
            if (d.n < 37) return;
            if ((pair_sum(d, 0) & 3) != 3) return;
            const uint32_t id = static_cast<uint32_t>(d.get(3, 24));
            if (id == 0 || id == 0xFFFFFF) return;
            Msg& m = tpms(out, r, "Nissan", "CHECKSUM", hexs(id, 6));
            m.num("mode", static_cast<double>(d.get(0, 3)), "%.0f");
            m.num("pressure_PSI", static_cast<double>(d.get(27, 8)) / 4.0 - 3.0, "%.2f");
            m.num("unknown", static_cast<double>(d.get(35, 2)), "%.0f");
        });
    }
}

// ----------------------------------- Schrader SMD3MA4 / NIS315G3 ---
// OOK 120 us NRZ, 315 MHz. Preamble F5555555E: its last two bits are the
// Manchester code ("10" = 1) of a fixed 1, then 37 bits: flags 3 (0 learn,
// 3 pressure change, 5 wake, 7 driving), id 24, pressure 8, check 2. The
// 2-bit pair sum of all 38 bits is 1 mod 4. Two sensor kinds send this:
// Subaru SMD3MA4 (PSI * 5) and Nissan/Infiniti/Renault NIS315G3 (PSI * 4) -
// both reported, as the frame does not tell them apart.
void smd3ma4_family(Pkt& pk, std::vector<Msg>& out, bool nis) {
    const Rows& rs = pk.pcm(Mod::OOK, 120, 120, 0, 480);
    for (const Row& r : rs.r) {
        if (r.n < 94 || r.n >= 120) continue;
        const int f = r.find(0x555E, 16, 0);
        if (f < 0) continue;
        const int pos = f + 14;
        if (pos + 75 > r.n) continue;
        const Row d = mc(r, pos, 38, true);
        if (d.n < 38) continue;
        // the 38 bits from the fixed 1: pairs (1, b1) .. (b36, b37)
        int s = 0;
        for (int i = 0; i < 38; i += 2) s += 2 * d.bit(i) + d.bit(i + 1);
        if ((s & 3) != 1) continue;
        const int flags = static_cast<int>(d.get(1, 3));
        const uint32_t id = static_cast<uint32_t>(d.get(4, 24));
        if (id == 0 || id == 0xFFFFFF) continue;
        const int praw = static_cast<int>(d.get(28, 8));
        Msg& m = tpms(out, r, nis ? "Schrader-NIS315G3" : "Schrader-SMD3MA4", "PARITY", HEXs(id, 6));
        m.num("flags", flags, "%.0f");
        if (flags == 0) m.num("learn", 1, "%.0f");
        if (flags == 3) {
            m.num("alarm", 1, "%.0f");
            m.event = "sudden pressure change";
            m.alarm = true;
        }
        if (flags == 5) m.num("wakeup", 1, "%.0f");
        m.num("pressure_PSI", praw * (nis ? 0.25 : 0.2), "%.1f");
    }
}
void decode_smd3ma4(Pkt& pk, std::vector<Msg>& out) { smd3ma4_family(pk, out, false); }
void decode_nis315g3(Pkt& pk, std::vector<Msg>& out) { smd3ma4_family(pk, out, true); }

// ------------------------------------------- BMW Gen4/5, Audi alert ---
// FSK 25 us NRZ, preamble AA 59, Manchester ("10" = 1): brand 8, id 32,
// pressure 8 (2.45 kPa), temp 8 (C + 52), [3 flag bytes - BMW only], CRC-8
// poly 0x2F init 0xAA over all bytes incl. the CRC = 0. 11 bytes BMW, 8 the
// Audi pressure alert.
void decode_bmw(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rs = pk.pcm(Mod::FSK, 25, 25, 0, 100);
    for (const auto& msg : rs.split()) {
        if (msg.size() != 1) continue;
        const Row& r = msg[0];
        const int p = r.find(0xAA59, 16, 0);
        if (p < 0) continue;
        const Row d = mc(r, p + 16, 88, true);
        const int L = d.n >= 88 ? 11 : d.n >= 64 ? 8 : 0;
        if (!L) continue;
        const auto b = d.bytes(0, L);
        if (crc8(b.data(), L, 0x2F, 0xAA) != 0) continue;
        const bool bmw = L == 11;
        Msg& m = tpms(out, r, bmw ? "BMW-GEN5" : "Audi-PressureAlert", "CRC", hexs(d.get(8, 32), 8));
        if (!bmw) {
            m.str("alert", "Alert Pressure increase/decrease !");
            m.event = "pressure increase/decrease";
            m.alarm = true;
        }
        m.num("brand", b[0], "%.0f");
        m.num("pressure_kPa", b[5] * 2.45, "%.2f");
        m.num("temperature_C", b[6] - 52.0, "%.0f");
        if (bmw) {
            m.num("flags1", b[7], "%.0f");
            m.num("flags2", b[8], "%.0f");
            m.num("flags3", b[9], "%.0f");
        }
        std::string s;
        for (int i = 0; i < L; i++) s += hexs(b[static_cast<size_t>(i)], 2);
        m.str("msg", s);
    }
}

}  // namespace

ISM_PROTOCOL("Toyota TPMS", Strength::STRONG, decode_toyota);
ISM_PROTOCOL("PMV-107J TPMS", Strength::STRONG, decode_pmv107j);
ISM_PROTOCOL("Ford TPMS", Strength::MEDIUM, decode_ford);
ISM_PROTOCOL("Citroen TPMS", Strength::MEDIUM, decode_citroen);
ISM_PROTOCOL("Renault TPMS", Strength::STRONG, decode_renault);
ISM_PROTOCOL("Hyundai VDO TPMS", Strength::STRONG, decode_hyundai_vdo);
ISM_PROTOCOL("Kia TPMS", Strength::MEDIUM, decode_kia);
ISM_PROTOCOL("Elantra2012 TPMS", Strength::STRONG, decode_elantra2012);
ISM_PROTOCOL("Jansite TPMS", Strength::WEAK, decode_jansite);
ISM_PROTOCOL("Jansite Solar TPMS", Strength::STRONG, decode_jansite_solar);
ISM_PROTOCOL("Abarth 124 / EGQ Q85 TPMS", Strength::MEDIUM, decode_abarth124);
ISM_PROTOCOL("Truck TPMS", Strength::MEDIUM, decode_truck);
ISM_PROTOCOL("Nissan TPMS", Strength::MEDIUM, decode_nissan);
ISM_PROTOCOL("Schrader SMD3MA4 TPMS", Strength::MEDIUM, decode_smd3ma4);
ISM_PROTOCOL("Schrader NIS315G3 TPMS", Strength::MEDIUM, decode_nis315g3);
ISM_PROTOCOL("BMW / Audi TPMS", Strength::STRONG, decode_bmw);

}  // namespace ism
