#pragma once

// MDC1200 packet coding (Motorola's PTT ID / status / emergency signalling).
// The format as the public decoders document it (Matthew Kaufman's
// mdc-encode-decode and sdrtrunk, both GPL - read as documentation of the
// format's facts only, no code of theirs is used - and the opcode list of the
// OneOfEleven UV-K5 firmware, Apache-2.0):
//
//   1200 bit/s MSK, XOR-precoded: a bit that differs from the one before is
//   one and a half cycles of 1800 Hz, an equal one one cycle of 1200 Hz.
//   Preamble (0x55... = all 1800 Hz), sync 0x07092A446F (40 bits, MSB
//   first), then a block of 112 bits: 7 bytes (op, arg, unit ID high, low,
//   CRC low, high, 0) + 7 parity bytes of a rate 1/2 convolutional code
//   (p[t] = d[t] ^ d[t-2] ^ d[t-5] ^ d[t-6], bits LSB first), interleaved
//   16 x 7. CRC-16 (poly 0x1021 reflected = 0x8408, init 0, out ^ 0xFFFF)
//   over op, arg, unit. Ops 0x35 / 0x55 are double packets: a second block
//   (4 more bytes + CRC) follows at once.

#include <cstdint>
#include <cstdio>
#include <string>

namespace mdc {

constexpr uint64_t SYNC = 0x07092A446FULL;   // 40 bits, sent MSB first
constexpr int SYNC_BITS = 40;
constexpr int BLOCK_BITS = 112;

inline uint16_t crc(const uint8_t* p, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0x8408) : static_cast<uint16_t>(c >> 1);
    }
    return static_cast<uint16_t>(c ^ 0xFFFF);
}

// transmitted bit order: coded bit t (byte t / 8, bit t % 8 LSB first) is
// sent as bit interleave(t) of the block (16 columns x 7 rows)
inline int interleave_pos(int t) { return (t % 7) * 16 + t / 7; }

// The received block's 112 bits (time order) -> 7 data bytes, single errors
// of the convolutional code corrected (majority of the 4 syndromes a data
// bit touches). Returns the number of bits corrected, -1 = CRC wrong.
inline int decode_block(const uint8_t bits[BLOCK_BITS], uint8_t d7[7]) {
    uint8_t c[112];
    for (int t = 0; t < 112; t++) c[t] = bits[interleave_pos(t)];
    uint8_t d[56], p[56], s[62] = {};
    for (int t = 0; t < 56; t++) {
        d[t] = c[t];
        p[t] = c[56 + t];
    }
    auto dat = [&](int t) { return t >= 0 ? d[t] : 0; };
    for (int t = 0; t < 56; t++) s[t] = p[t] ^ dat(t) ^ dat(t - 2) ^ dat(t - 5) ^ dat(t - 6);
    int fixed = 0;
    for (int t = 0; t < 56; t++) {
        const int taps[4] = {t, t + 2, t + 5, t + 6};
        int n = 0;
        for (int k : taps) n += k < 56 ? s[k] : 0;
        if (n >= 3) {
            d[t] ^= 1;
            for (int k : taps)
                if (k < 56) s[k] ^= 1;
            fixed++;
        }
    }
    for (int i = 0; i < 7; i++) {
        d7[i] = 0;
        for (int j = 0; j < 8; j++) d7[i] |= static_cast<uint8_t>(d[i * 8 + j] << j);
    }
    const uint16_t want = static_cast<uint16_t>(d7[4] | (d7[5] << 8));
    return crc(d7, 4) == want ? fixed : -1;
}

inline bool is_double(uint8_t op) { return op == 0x35 || op == 0x55; }

// What a packet says, and whose ID it carries: the radio sending it (the
// transmitter - a talker) or the radio it is addressed to (sent by a
// console / another unit)
struct Meaning {
    std::string text;          // "PTT ID", "Status 3", "Radio check" ...
    bool sender = false;       // unit = the transmitting radio
    bool target = false;       // unit = the addressed radio
    bool emergency = false;
};

inline std::string hex4(unsigned v) {
    char b[8];
    snprintf(b, sizeof b, "%04X", v & 0xFFFF);
    return b;
}

// op / arg -> meaning (the double packets' second block: caller / ack)
inline Meaning meaning(uint8_t op, uint8_t arg) {
    Meaning m;
    char b[64];
    switch (op) {
    case 0x00: m.text = "Emergency"; m.sender = m.emergency = true; break;
    case 0x01: m.text = "PTT ID"; m.sender = true; break;
    case 0x03: case 0x30: m.text = "Radio check acknowledge"; m.sender = true; break;
    case 0x06: snprintf(b, sizeof b, "Status %u (response)", arg); m.text = b; m.sender = true; break;
    case 0x07: case 0x47: snprintf(b, sizeof b, "Message %u", arg); m.text = b; m.sender = true; break;
    case 0x0B: m.text = arg == 0x0C ? "Radio enable acknowledge" : "Radio inhibit acknowledge"; m.sender = true; break;
    case 0x11: m.text = "Remote monitor"; m.target = true; break;
    case 0x20: m.text = "Emergency acknowledge"; m.target = true; break;
    case 0x22: m.text = "Status request"; m.target = true; break;
    case 0x23: m.text = "Acknowledge"; m.target = true; break;
    case 0x2B: m.text = arg == 0x0C ? "Radio enable" : "Radio inhibit"; m.target = true; break;
    case 0x35: m.text = "Call"; break;   // double packet: refined by call_meaning()
    case 0x40: case 0x41: m.text = "Request to talk"; m.sender = true; break;
    case 0x46: snprintf(b, sizeof b, "Status %u", arg); m.text = b; m.sender = true; break;
    case 0x63: m.text = "Radio check"; m.target = true; break;
    default:
        snprintf(b, sizeof b, "op %02X arg %02X", op, arg);
        m.text = b;
        break;
    }
    return m;
}

// a 0x35 double packet: unit = the called radio, the second block's last
// two bytes the caller (or, in an acknowledge, the radio answering)
inline std::string call_meaning(uint8_t e0, uint8_t e1) {
    if (e0 == 0xA0) return "Call alert acknowledge";
    if (e0 == 0x83 || e0 == 0x81) return "Call alert";
    if (e0 == 0x82 || e0 == 0x80) return "Selective call";
    char b[48];
    snprintf(b, sizeof b, "Call (%02X %02X)", e0, e1);
    return b;
}

}  // namespace mdc
