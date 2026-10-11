// OOK weather sensors and a remote, one small protocol each:
//   Nexus (and FreeTec NC-7345, NX-3980, Solight TE82S, TFA 30.3209.02,
//   Technoline TX9116, Digoo DG-R8H), Nexus sauna sensor, Rubicson (TFA
//   30.3197, inFactory ZX-7382 / PT-310), Solight TE44 / EMOS E0107T,
//   Prologue (FreeTec NC-7104, ThermoPro TX2 bit for bit), Ambient Weather
//   F007TH (F012TH, TFA 30.3208.02, SwitchDoc F016TH), inFactory NC-3982
//   (nor-tec, DAY), ThermoPro TP08 / TP12 / TP20, Ambient Weather TX-8300 /
//   TFA 30.3211.02, Silvercrest remote.
//
// protocol notes: rtl_433 documentation; own implementation.

#include "frontend.hpp"
#include "ism.hpp"

#include <algorithm>
#include <cstdlib>

namespace ism {
namespace {

void widen(Msg& m, const Row& r) {
    m.t0 = std::min(m.t0, r.t0);
    m.t1 = std::max(m.t1, r.t1);
}
void widen_same(Msg& m, const Rows& msg, const Row& r) {
    for (const auto& x : msg.r)
        if (x.same(r)) widen(m, x);
}
int s12(int v) { return (v & 0x800) ? v - 0x1000 : v; }

// the front end cuts packages longer than its limit: the repeats of a long
// transmission are then spread over two bursts
bool burst_was_cut(const Pkt& pk) {
    return static_cast<double>(pk.burst.b - pk.burst.a) >= FrontEnd::MAX_LEN_S * FS;
}

// Rubicson / Solight check: CRC-8 poly 0x31 init 0x6C over bits 0..27 padded
// with 4 zero bits, compared with the 8 bits at 28..35
bool rubicson_crc_ok(const Row& r) {
    const uint8_t d[4] = {r.byte(0), r.byte(8), r.byte(16), static_cast<uint8_t>(r.get(24, 4) << 4)};
    return crc8(d, 4, 0x31, 0x6C) == r.byte(28);
}

// ------------------------------------------------------------- Nexus ---
// OOK PPM: 500 us pulses, gap 1000 = 0 / 2000 = 1, ~4 ms between the 12
// copies of 36 bits: id:8 | batt_ok:1 test:1 channel:2 | temp:12 signed C*10 |
// 1111 | humidity:8 (0 = none). No check at all: identical repeats (WEAK),
// the constant nibble and plausibility. A row that passes the Rubicson CRC
// is left to that decoder.
void decode_nexus(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 1000, 2000, 3000, 5000);
    for (const auto& msg : rows.split()) {
        const int ri = msg.repeated(3, 36);
        if (ri < 0) continue;
        const Row& r = msg[static_cast<size_t>(ri)];
        if (r.n > 37) continue;
        const auto b = r.bytes(0, 5);
        if ((b[0] == 0 && b[2] == 0 && b[3] == 0) || (b[0] == 0xFF && b[2] == 0xFF && b[3] == 0xFF)) continue;
        const int chan = (b[1] >> 4) & 3;
        if ((b[3] & 0xF0) == 0xF0 && chan != 3) {
            if (rubicson_crc_ok(r)) continue;
            const int hum = ((b[3] & 0x0F) << 4) | (b[4] >> 4);
            if (hum > 100) continue;
            const double tc = s12(((b[1] & 0x0F) << 8) | b[2]) * 0.1;
            Msg& m = emit(out, r, hum ? "Nexus-TH" : "Nexus-T", Kind::WEATHER, "REPEAT");
            widen_same(m, msg, r);
            m.id = std::to_string(b[0]);
            m.num("channel", chan + 1);
            m.num("battery_ok", (b[1] >> 7) & 1);
            m.num("temperature_C", tc, "%.1f");
            if (hum) m.num("humidity", hum);
            if ((b[1] >> 6) & 1) m.num("test", 1);
            continue;
        }
        // sauna sensor: id:8 | batt_ok:1 test:1 11 | 1111 | temp:16 signed C*10 | 0001
        if ((b[1] & 0x0F) == 0x0F && chan == 3 && b[0] != 0 && (b[4] & 0x10) &&
            !(b[0] == 0xFF && b[2] == 0xFF && b[3] == 0xFF)) {
            const int t = static_cast<int16_t>((b[2] << 8) | b[3]);
            Msg& m = emit(out, r, "Nexus-Sauna", Kind::WEATHER, "REPEAT");
            widen_same(m, msg, r);
            m.id = std::to_string(b[0]);
            m.num("channel", 4);
            m.num("battery_ok", (b[1] >> 7) & 1);
            m.num("temperature_C", t * 0.1, "%.1f");
            if ((b[1] >> 6) & 1) m.num("test", 1);
        }
    }
}

// ---------------------------------------------------- Rubicson, Solight ---
// The Nexus layout with a CRC in place of the humidity: id:8 | batt_ok:1 0
// channel:2 | temp:12 signed C*10 | 1111 | CRC-8 (poly 0x31, init 0x6C).
// Rubicson: 36 bits (inFactory variants 37 / 38), 3 identical rows.
// Solight TE44: the same payload with slightly different timing and exactly
// 37-bit rows; it only gets a say when the Rubicson reading of the package
// found nothing (as in rtl_433, where Rubicson runs first).
bool rubicson_row(const Rows& msg, int minb, int maxb, const char* model, std::vector<Msg>& out) {
    const int ri = msg.repeated(3, 36);
    if (ri < 0) return false;
    const Row& r = msg[static_cast<size_t>(ri)];
    if (r.n < minb || r.n > maxb) return false;
    if (r.get(24, 4) != 0xF || !rubicson_crc_ok(r)) return false;
    const auto b = r.bytes(0, 3);
    Msg& m = emit(out, r, model, Kind::WEATHER, "CRC");
    widen_same(m, msg, r);
    m.id = std::to_string(b[0]);
    m.num("channel", ((b[1] >> 4) & 3) + 1);
    if (model[0] == 'R') m.num("battery_ok", (b[1] >> 7) & 1);
    m.num("temperature_C", s12(((b[1] & 0x0F) << 8) | b[2]) * 0.1, "%.1f");
    return true;
}

void decode_rubicson(Pkt& pk, std::vector<Msg>& out) {
    bool any = false;
    for (const auto& msg : pk.ppm(Mod::OOK, 1000, 2000, 3000, 4800).split())
        any |= rubicson_row(msg, 36, 38, "Rubicson-Temperature", out);
    if (any) return;
    for (const auto& msg : pk.ppm(Mod::OOK, 972, 1932, 3000, 6000).split())
        rubicson_row(msg, 37, 37, "Solight-TE44", out);
}

// ----------------------------------------------------------- Prologue ---
// OOK PPM: ~500 us pulses, gap 2000 = 0 / 4000 = 1, ~9 ms between the 7
// copies of 36 bits: type:4 (9 or 5) | id:8 | batt:1 button:1 channel:2 |
// temp:12 signed C*10 | humidity:8 (0xCC = none). No check (WEAK); 4
// identical rows. The ThermoPro TX2 sends exactly this with the battery bit
// inverted - it cannot be told apart, so (as rtl_433 does by default) it is
// reported as Prologue-TH.
void decode_prologue(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 2000, 4000, 7000, 10000);
    dbg_rows("prologue", rows);
    for (const auto& msg : rows.split()) {
        if (msg[0].n > 0 && msg[0].n <= 8) continue;   // an 8-bit preamble: other protocol family
        // 4 identical rows (3 give false positives on other sensors); a
        // package the front end cut at its length limit may hold only 3
        const int ri = msg.repeated(burst_was_cut(pk) ? 3 : 4, 36);
        if (ri < 0) continue;
        const Row& r = msg[static_cast<size_t>(ri)];
        if (r.n > 37) continue;
        const auto b = r.bytes(0, 5);
        const int type = b[0] >> 4;
        if (type != 9 && type != 5) continue;
        const int hum = ((b[3] & 0x0F) << 4) | (b[4] >> 4);
        if (hum > 100 && hum != 0xCC) continue;   // not a humidity: some other sensor's repeats
        Msg& m = emit(out, r, "Prologue-TH", Kind::WEATHER, "REPEAT");
        widen_same(m, msg, r);
        m.id = std::to_string(((b[0] & 0x0F) << 4) | (b[1] >> 4));
        m.num("subtype", type);
        m.num("channel", (b[1] & 3) + 1);
        m.num("battery_ok", (b[1] >> 3) & 1);
        m.num("temperature_C", static_cast<int16_t>((b[2] << 8) | (b[3] & 0xF0)) / 16 * 0.1, "%.1f");
        if (hum != 0xCC) m.num("humidity", hum);
        m.num("button", (b[1] >> 2) & 1);
        if ((b[1] >> 2) & 1) m.event = "button";
    }
}

// -------------------------------------------------------- Ambient F007TH ---
// OOK Manchester, 1 kbit/s (500 us half bits); the 6-byte message is sent 3
// times in one row, each after a preamble ending ...0000 0001 0100 (or
// 1111 1101 0100 for the repeats); the message starts 8 bits into that
// 12-bit pattern: model:8 | id:8 | batt_low:1 channel:3 temp:12 (F*10 + 400)
// | humidity:8 | LFSR digest (gen 0x98, key 0x3E) xor 0x64
bool f007_at(const Row& r, int pos, std::vector<Msg>& out) {
    if (pos + 8 + 48 > r.n) return false;
    const auto b = r.bytes(pos + 8, 6);
    if ((lfsr_digest8(b.data(), 5, 0x98, 0x3E) ^ 0x64) != b[5]) return false;
    const int hum = b[4];
    const double tf = ((((b[2] & 0x0F) << 8) | b[3]) - 400) * 0.1;
    if (hum > 100 || tf < -40.0 || tf >= 344.0) return false;
    Msg& m = emit(out, r, "Ambientweather-F007TH", Kind::WEATHER, "CRC");
    m.id = std::to_string(b[1]);
    m.num("channel", ((b[2] >> 4) & 7) + 1);
    m.num("battery_ok", (b[2] & 0x80) ? 0 : 1);
    m.num("temperature_F", tf, "%.1f");
    m.num("humidity", hum);
    return true;
}

void decode_f007(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.manchester_zerobit(Mod::OOK, 500, 2400);
    for (const auto& r : rows.r) {
        if (r.n < 60) continue;
        bool done = false;
        for (int p = r.find(0x014, 12); p >= 0 && !done; p = r.find(0x014, 12, p + 1)) done = f007_at(r, p, out);
        for (int p = r.find(0xFD4, 12); p >= 0 && !done; p = r.find(0xFD4, 12, p + 1)) done = f007_at(r, p, out);
    }
}

// ----------------------------------------------------------- inFactory ---
// OOK PPM: 4 x 1000 us preamble pulses, a 500 us pulse + 8 ms gap, then 40
// bits (500 us pulses, gap 2000 = 0 / 4000 = 1) and a 16 ms gap; 6 copies.
// id:8 | crc:4 button:1 batt_low:1 x:2 | temp:12 (F*10 + 900) | humidity BCD
// tens:4 units:4 | x:2 channel:2. CRC-4 (x^4+x+1) over the 4 bytes with the
// CRC nibble replaced by the last byte's low nibble, then xor the humidity
// units nibble. Only 4 bits of check: two copies must agree.
bool infactory_ok(const Row& r) {
    if (r.n < 40 || r.n > 42) return false;
    auto b = r.bytes(0, 5);
    const int chan = b[4] & 3;
    if (chan == 0) return false;
    uint8_t d[4] = {b[0], static_cast<uint8_t>(((b[4] & 0x0F) << 4) | (b[1] & 0x0F)), b[2], b[3]};
    // CRC-4, generator x^4 + x + 1, MSB first, init 0
    unsigned c = 0;
    for (uint8_t v : d) {
        c ^= v;
        for (int k = 0; k < 8; k++) c = (c & 0x80) ? ((c << 1) ^ 0x130) : (c << 1);
    }
    const unsigned crc = ((c >> 4) & 0x0F) ^ (b[4] >> 4);
    if (crc != static_cast<unsigned>(b[1] >> 4)) return false;
    const int hum = (b[3] & 0x0F) * 10 + (b[4] >> 4);
    const double tf = (((b[2] << 4) | (b[3] >> 4)) - 900) * 0.1;
    return hum <= 100 && tf >= -40.0 && tf <= 158.0;
}

void decode_infactory(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 2000, 4000, 0, 5000, 500, 750);
    dbg_rows("infactory", rows);
    std::vector<const Row*> ok;
    for (const auto& r : rows.r)
        if (infactory_ok(r)) ok.push_back(&r);
    for (size_t i = 0; i < ok.size(); i++) {
        int same = 0;
        for (const Row* x : ok)
            if (x->get(0, 40) == ok[i]->get(0, 40)) same++;
        if (same < 2) continue;
        const Row& r = *ok[i];
        const auto b = r.bytes(0, 5);
        Msg& m = emit(out, r, "inFactory-TH", Kind::WEATHER, "CRC");
        for (const Row* x : ok)
            if (x->get(0, 40) == r.get(0, 40)) widen(m, *x);
        m.id = std::to_string(b[0]);
        m.num("channel", b[4] & 3);
        m.num("battery_ok", (b[1] >> 2) & 1 ? 0 : 1);
        m.num("button", (b[1] >> 3) & 1);
        m.num("temperature_F", (((b[2] << 4) | (b[3] >> 4)) - 900) * 0.1, "%.1f");
        m.num("humidity", (b[3] & 0x0F) * 10 + (b[4] >> 4));
        if ((b[1] >> 3) & 1) m.event = "button";
        return;
    }
}

// ------------------------------------------------------- ThermoPro TP12 ---
// OOK PPM: ~480 us pulses, gap 500 = 0 / 1500 = 1, 16 rows of 41 bits (the
// last one 40). id:8 | t1 low:8 | t1 high:4 t2 high:4 | t2 low:8 | digest
// (reflected, gen 0x51, key 0x04) | 1. Temperatures C*10 + 200.
void decode_tp12(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 500, 1500, 2000, 4000);
    for (const auto& msg : rows.split()) {
        const int need = msg.size() > 5 ? 5 : 2;
        int ri = -1;
        for (size_t i = 0; i < msg.size() && ri < 0; i++) {
            const Row& r = msg[i];
            if (r.n != 41) continue;
            int c = 0;
            for (const auto& x : msg.r)
                if (x.n >= 40 && x.get(0, 40) == r.get(0, 40)) c++;
            if (c >= need) ri = static_cast<int>(i);
        }
        if (ri < 0) continue;
        const Row& r = msg[static_cast<size_t>(ri)];
        const auto b = r.bytes(0, 5);
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) continue;
        if (lfsr_digest8_reflect(b.data(), 4, 0x51, 0x04) != b[4]) continue;
        const int t1 = ((b[2] & 0xF0) << 4) | b[1];
        const int t2 = ((b[2] & 0x0F) << 8) | b[3];
        Msg& m = emit(out, r, "Thermopro-TP12", Kind::WEATHER, "CRC");
        for (const auto& x : msg.r)
            if (x.n >= 40 && x.get(0, 40) == r.get(0, 40)) widen(m, x);
        m.id = std::to_string(b[0]);
        m.num("temperature_1_C", (t1 - 200) * 0.1, "%.1f");
        m.num("temperature_2_C", (t2 - 200) * 0.1, "%.1f");
    }
}

// --------------------------------------------- Ambient Weather TX-8300 ---
// OOK PPM with long pulses: ~1970 us on, gap ~1940 = 0 / ~3890 = 1; 74 bits:
// 0, a repeat counter bit, then 9 bytes: humidity BCD:8 | batt:2 channel:2
// minus:1 id:7 | temp BCD tens:4 units:4 tenths:4 | bytes 4..7 = bytes 0..3
// inverted | a nibble-sum checksum byte. The first data bit is unreliable
// and is taken from the inverted copy.
void decode_tx8300(Pkt& pk, std::vector<Msg>& out) {
    const Rows& rows = pk.ppm(Mod::OOK, 2000, 4000, 6500, 8000);
    for (const auto& r : rows.r) {
        if (r.n != 74) continue;
        auto d = r.bytes(2, 9);
        for (int i = 4; i < 8; i++) d[static_cast<size_t>(i)] ^= 0xFF;
        d[0] = static_cast<uint8_t>((d[0] & 0x7F) | (d[4] & 0x80));
        if (d[0] != d[4] || d[1] != d[5] || d[2] != d[6] || d[3] != d[7]) continue;
        int x = 0, y = 0;
        for (int i = 0; i < 4; i++) {
            x += (d[static_cast<size_t>(i)] & 0x0F) + (d[static_cast<size_t>(i)] >> 4);
            y += (d[static_cast<size_t>(i)] & 0x05) + ((d[static_cast<size_t>(i)] & 0x50) >> 4);
        }
        if ((((~x & 0x0F) << 4) | (~y & 0x0F)) != d[8]) continue;
        double t = (d[2] & 0x0F) * 10 + (d[3] >> 4) + (d[3] & 0x0F) * 0.1;
        if (d[1] & 0x08) t = -t;
        const int h1 = d[0] >> 4, h0 = d[0] & 0x0F;
        Msg& m = emit(out, r, "AmbientWeather-TX8300", Kind::WEATHER, "CHECKSUM");
        m.id = std::to_string(((d[1] & 0x07) << 4) | (d[2] >> 4));
        m.num("channel", (d[1] >> 4) & 3);
        m.num("battery", d[1] >> 6);
        m.num("temperature_C", t, "%.1f");
        if (h1 <= 9 && h0 <= 9) m.num("humidity", h1 * 10 + h0);
    }
}

// --------------------------------------------------- Silvercrest remote ---
// OOK PWM: 264 us pulse = 1, 744 us = 0, ~8 ms between the 33-bit copies.
// 0x7C 0x26 header, a 4-bit button code in byte 2 and its check nibble
// (a fixed table) in byte 3.
void decode_silvercrest(Pkt& pk, std::vector<Msg>& out) {
    static const uint8_t CHK[16] = {2, 3, 0, 1, 4, 5, 7, 6, 0xC, 0xD, 0xF, 0xE, 8, 9, 0xB, 0xA};
    const Rows& rows = pk.pwm(Mod::OOK, 264, 744, 5000, 12000);
    for (const auto& msg : rows.split()) {
        for (const auto& r : msg.r) {
            if (r.n != 33 || r.byte(0) != 0x7C || r.byte(8) != 0x26) continue;
            const int cmd = static_cast<int>(r.get(20, 4));
            if (r.get(28, 4) != CHK[cmd]) continue;
            Msg& m = emit(out, r, "Silvercrest-Remote", Kind::REMOTE, "");
            widen_same(m, msg, r);
            m.num("button", cmd);
            m.event = "button " + std::to_string(cmd);
            break;
        }
    }
}

}  // namespace
ISM_PROTOCOL("Nexus / Nexus sauna", Strength::WEAK, decode_nexus);
ISM_PROTOCOL("Rubicson / Solight TE44", Strength::STRONG, decode_rubicson);
ISM_PROTOCOL("Prologue / ThermoPro TX2", Strength::WEAK, decode_prologue);
ISM_PROTOCOL("Ambient Weather F007TH", Strength::STRONG, decode_f007);
ISM_PROTOCOL("inFactory NC-3982", Strength::MEDIUM, decode_infactory);
ISM_PROTOCOL("ThermoPro TP12", Strength::STRONG, decode_tp12);
ISM_PROTOCOL("Ambient Weather TX-8300", Strength::STRONG, decode_tx8300);
ISM_PROTOCOL("Silvercrest remote", Strength::MEDIUM, decode_silvercrest);
}  // namespace ism
