// Alarm-system sensors: Honeywell / Ademco 5800 series (also 2GIG DW10/DW11,
// GB1 and the Resolution RE208 repeater, 345 MHz), Interlogix / GE / UTC
// (319.5 MHz) and DSC (433 MHz) contacts, motion and smoke sensors.
//
// protocol notes: rtl_433 documentation; own implementation.

#include "ism.hpp"

#include <algorithm>

namespace ism {
namespace {

// ------------------------------------------------------------- Honeywell ---
// OOK, Manchester with 136 us half bits, sliced as plain NRZ and decoded here
// ("01" = 1). Preamble 0xFFFE, then channel (4) | id (20) | event (8) |
// CRC-16 (16, MSB first, init 0; polynomial 0x8005, 0x8050 on the 2GIG
// channels 2/4/9/A/C). The end of the preamble (raw 0x555556) is searched at
// every position - noise before the frame can fake earlier matches.
void decode_honeywell(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 30) return;
    const Rows& rs = pk.pcm(Mod::OOK, 136, 136, 0, 408, 0);
    for (const Row& row : rs.r) {
        if (row.n < 120) continue;
        for (int p = row.find(0x555556, 24); p >= 0 && p + 24 < row.n; p = row.find(0x555556, 24, p + 1)) {
            const Row d = row.manchester(p + 24, 96, false);
            if (d.n < 48) continue;
            const auto b = d.bytes(0, 6);
            const int channel = b[0] >> 4;
            const int id = ((b[0] & 0x0F) << 16) | (b[1] << 8) | b[2];
            const int crc = (b[4] << 8) | b[5];
            if (id == 0 && crc == 0) continue;
            const bool gig = channel == 0x2 || channel == 0x4 || channel == 0x9 || channel == 0xA || channel == 0xC;
            if (crc16(b.data(), 4, gig ? 0x8050 : 0x8005, 0) != crc) continue;
            const int ev = b[3];
            const bool contact = ev & 0x80, tamper = ev & 0x40, reed = ev & 0x20, alarm = ev & 0x10,
                       batt_low = ev & 0x08, heartbeat = ev & 0x04;
            Msg& m = emit(out, row, "Honeywell-Security", Kind::SECURITY, "CRC");
            m.id = std::to_string(id);
            m.num("channel", channel);
            m.num("event", ev);
            m.str("state", contact ? "open" : "closed");
            m.num("contact_open", contact);
            m.num("reed_open", reed);
            m.num("alarm", alarm);
            m.num("tamper", tamper);
            m.num("battery_ok", !batt_low);
            m.num("heartbeat", heartbeat);
            m.event = alarm ? "alarm" : contact ? "open" : "closed";
            if (heartbeat && !alarm) m.event = std::string("heartbeat, ") + (contact ? "open" : "closed");
            if (batt_low && !alarm) m.event += ", low battery";
            m.alarm = alarm || tamper;
            break;
        }
    }
}

// ------------------------------------------------------------ Interlogix ---
// OOK PPM (~122 us pulses; gap ~122 = 0, ~244 = 1). Front porch, 12 sync
// zeros, a start 1, then the message: 20-bit id + 4-bit device type (both
// LSB first), trigger count, battery, the input latch / level bits and two
// parity bits (even and odd positions, each must make an odd sum). Also
// checked: message bit 42 (the old stop bit) = 1 and the pulses all short.
const char* interlogix_type(int t) {
    switch (t) {
    case 0xA: return "contact";
    case 0xF: return "keyfob";
    case 0x4: return "motion";
    case 0x6: return "heat";
    case 0x9: return "glass";
    case 0xD: return "glass";
    case 0xE: return "freeze";
    case 0x2: return "smoke";
    case 0x3: return "panic";
    default: return nullptr;
    }
}

void decode_interlogix(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 50) return;
    const Rows& rs = pk.ppm(Mod::OOK, 122, 244, 0, 500, 0, 0);
    for (const Row& row : rs.r) {
        if (row.n < 56 || row.n > 66) continue;
        const int p = row.find(0x01, 8);
        if (p < 0 || p + 8 + 42 > row.n) continue;
        const Row m46 = row.sub(p + 8, 46);
        const auto b = m46.bytes(0, 6);   // the last 2 bits zero
        if ((b[0] == 0 && b[1] == 0 && b[2] == 0) || (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF)) continue;
        if ((b[3] == 0 && b[4] == 0 && b[5] == 0) || (b[3] == 0xFF && b[4] == 0xFF && b[5] == 0xFF)) continue;
        int even = 0, odd = 0;
        for (int i = 0; i < 42; i++) (i & 1 ? odd : even) ^= m46.bit(i);
        if (even != 1 || odd != 1) continue;
        if (!m46.bit(42)) continue;   // the old stop bit: 1 in every capture
        // PPM of equal short pulses: at most a few others (front porch, the
        // long RF-on bit) inside the frame
        {
            const auto& ps = pk.pulses(Mod::OOK);
            auto it = std::lower_bound(ps.begin(), ps.end(), row.t0,
                                       [](const Pulse& q, int64_t t) { return q.t < t; });
            int odd_w = 0;
            for (; it != ps.end() && it->t < row.t1; ++it)
                if (s2us(it->on) < 60 || s2us(it->on) > 220) odd_w++;
            if (odd_w > 3) continue;
        }
        // 24 bits sent LSB first: the id with the device type as top digit
        uint32_t ser = 0;
        for (int i = 0; i < 24; i++) ser |= static_cast<uint32_t>(m46.bit(i)) << i;
        const int type = static_cast<int>(ser >> 20);
        const char* sub = interlogix_type(type);
        if (!sub) continue;
        const std::string id = hexs(ser, 6);
        const bool keyfob = type == 0xF;
        bool sw[5];
        int lowbat = 0;
        if (keyfob) {
            const int v = m46.bit(28) * 4 + m46.bit(29) * 2 + m46.bit(30);
            const int which[5] = {2, 4, 6, 1, 5};
            for (int k = 0; k < 5; k++) sw[k] = v != which[k];   // true = OPEN
        } else {
            lowbat = m46.bit(27);
            const int pos[5] = {29, 31, 33, 35, 37};
            for (int k = 0; k < 5; k++) sw[k] = m46.bit(pos[k]);
        }
        Msg& m = emit(out, row, "Interlogix-Security", Kind::SECURITY, "PARITY");
        m.id = id;
        m.str("subtype", sub);
        m.str("raw_message", hexs((static_cast<uint32_t>(b[3]) << 16) | (b[4] << 8) | b[5], 6));
        m.num("battery_ok", !lowbat);
        const char* keys[5] = {"switch1", "switch2", "switch3", "switch4", "switch5"};
        for (int k = 0; k < 5; k++) m.str(keys[k], sw[k] ? "OPEN" : "CLOSED");
        if (keyfob) {
            for (int k = 0; k < 5; k++)
                if (!sw[k]) m.event = "button " + std::to_string(k + 1);
        } else if (sw[2]) {
            m.event = "tamper";
            m.alarm = true;
        } else if (sw[0]) {
            const std::string s = sub;
            m.event = s == "contact" ? "open" : s == "motion" ? "motion" : "alarm";
            m.alarm = s != "contact" && s != "motion";
        } else if (std::string(sub) == "contact") {
            m.event = "closed";
        }
        if (lowbat && m.event.empty()) m.event = "low battery";
        break;
    }
}

// ------------------------------------------------------------------- DSC ---
// OOK return-to-zero: a 500 us bit, a 1 = 250 us of carrier in its second
// half. 4 sync ones, then 5 bytes with a 1 before each of bytes 1..4:
// status | ESN (24 bits) | CRC-8 (reflected, poly 0xF5, init 0x3D: run
// over all 5 bytes it gives 0). The WS4945 family uses doubled timing.
void dsc_rows(const Rows& rs, std::vector<Msg>& out) {
    for (const Row& row : rs.r) {
        if (row.n < 48 || row.n > 70) continue;
        if (!row.get(0, 4) || !row.bit(12) || !row.bit(21) || !row.bit(30) || !row.bit(39)) continue;
        uint8_t b[5];
        b[0] = static_cast<uint8_t>(row.get(4, 8));
        b[1] = static_cast<uint8_t>(row.get(13, 8));
        b[2] = static_cast<uint8_t>(row.get(22, 8));
        b[3] = static_cast<uint8_t>(row.get(31, 8));
        b[4] = static_cast<uint8_t>(row.get(40, 8));
        if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF) continue;
        if (crc8le(b, 5, 0xF5, 0x3D) != 0) continue;
        const int st = b[0];
        const uint32_t esn = (static_cast<uint32_t>(b[1]) << 16) | (b[2] << 8) | b[3];
        const bool closed = st & 0x02, event = !(st & 0x40), batt_low = st & 0x08, xact = st & 0x20;
        const bool xt1 = !(st & 0x01), xt2 = st & 0x10, tamper = xt1 || xt2;
        const bool exc = !(st & 0x80) || (st & 0x04);
        Msg& m = emit(out, row, "DSC-Security", Kind::SECURITY, "CRC");
        m.id = std::to_string(esn);
        m.num("closed", closed);
        m.num("event", event);
        m.num("tamper", tamper);
        m.num("battery_ok", !batt_low);
        m.num("xactivity", xact);
        m.num("xtamper1", xt1);
        m.num("xtamper2", xt2);
        m.num("exception", exc);
        m.str("esn", hexs(esn, 6));
        m.num("status", st);
        m.str("status_hex", hexs(static_cast<uint64_t>(st), 2));
        if (tamper) {
            m.event = "tamper";
            m.alarm = true;
        } else {
            m.event = event ? (closed ? "closed" : "open") : "heartbeat";
            if (batt_low) m.event += ", low battery";
        }
    }
}

void decode_dsc(Pkt& pk, std::vector<Msg>& out) {
    if (pk.pulses(Mod::OOK).size() < 10) return;
    dsc_rows(pk.pcm(Mod::OOK, 250, 500, 0, 5000, 0), out);
    dsc_rows(pk.pcm(Mod::OOK, 536, 1072, 0, 9000, 0), out);
}

}  // namespace
ISM_PROTOCOL("Honeywell 5800 / 2GIG", Strength::STRONG, decode_honeywell);
ISM_PROTOCOL("Interlogix / GE", Strength::MEDIUM, decode_interlogix);
ISM_PROTOCOL("DSC security", Strength::STRONG, decode_dsc);
}  // namespace ism
