// Meshtastic over LoRa: the clear 16-byte PacketHeader, AES-128-CTR decrypt
// with the PUBLISHED default channel key only, and the Data protobuf
// (portnum + payload: TEXT_MESSAGE, POSITION, NODEINFO, TELEMETRY, ROUTING).
//
// Only the public default channel ("AQ==" / PSK 0x01 ->
// d4f1bb3a20290759f0bcffabcf4e6901) is decrypted - this key is documented by
// the Meshtastic project and anything sent with it is readable by anyone. A
// packet on any other channel is NOT decrypted: only its clear header is
// shown. Private keys are never guessed or brute-forced.
//
// Header / nonce / portnum layout from the Meshtastic protocol documentation
// (meshtastic.org, meshtastic/meshtastic-sdk). Clean-room implementation.
#pragma once
#include "aes.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mesh {

// Meshtastic names / texts are UTF-8 (emoji are common in node names): valid
// UTF-8 is kept, control characters, DEL and invalid bytes become "\xNN"
// (kp::printable escapes every non-ASCII byte - a real node "🐐Public MQTT
// Gate" showed as "\xF0\x9F\x90\x90Public MQTT Gate")
inline std::string utf8_printable(const std::string& s) {
    std::string o;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    auto esc = [&o](unsigned char c) {
        static const char* hx = "0123456789ABCDEF";
        o += "\\x";
        o += hx[c >> 4];
        o += hx[c & 15];
    };
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c >= 32 && c < 127) { o += static_cast<char>(c); i++; continue; }
        size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = len && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) ok = (p[i + k] & 0xC0) == 0x80;
        if (ok && len == 3) {
            const unsigned c1 = p[i + 1];
            ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);   // overlong / surrogates
        }
        if (ok && len == 4) {
            const unsigned c1 = p[i + 1];
            ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        if (ok && len == 2 && c == 0xC2 && p[i + 1] < 0xA0) ok = false;   // C1 controls
        if (ok) {
            o.append(s, i, len);
            i += len;
        } else {
            esc(c);
            i++;
        }
    }
    return o;
}

inline const uint8_t DEFAULT_KEY[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                        0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

struct Header {
    uint32_t to = 0, from = 0, id = 0;
    uint8_t flags = 0, chan_hash = 0, next_hop = 0, relay_node = 0;
    int hop_limit() const { return flags & 0x7; }
    bool want_ack() const { return flags & 0x8; }
    bool via_mqtt() const { return flags & 0x10; }
    int hop_start() const { return (flags >> 5) & 0x7; }
};

inline bool parse_header(const uint8_t* p, size_t n, Header& h) {
    if (n < 16) return false;
    auto u32 = [&](int o) { return static_cast<uint32_t>(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (uint32_t(p[o + 3]) << 24)); };
    h.to = u32(0);
    h.from = u32(4);
    h.id = u32(8);
    h.flags = p[12];
    h.chan_hash = p[13];
    h.next_hop = p[14];
    h.relay_node = p[15];
    return true;
}

// AES-CTR decrypt the payload (everything after the 16-byte header) in place
// with the default key. nonce = id(8, LE) + from(4, LE) + 0000.
inline void decrypt_default(const Header& h, std::vector<uint8_t>& payload) {
    uint8_t nonce[16] = {};
    for (int i = 0; i < 4; i++) nonce[i] = static_cast<uint8_t>(h.id >> (8 * i));
    for (int i = 0; i < 4; i++) nonce[8 + i] = static_cast<uint8_t>(h.from >> (8 * i));
    uint8_t key[16];
    std::memcpy(key, DEFAULT_KEY, 16);
    if (!payload.empty()) lora_aes::ctr_xor(key, nonce, payload.data(), payload.size());
}

// Minimal protobuf reader for the Data message and the simple inner types.
struct PbReader {
    const uint8_t* p;
    size_t n, i = 0;
    PbReader(const uint8_t* d, size_t len) : p(d), n(len) {}
    bool eof() const { return i >= n; }
    bool varint(uint64_t& v) {
        v = 0;
        int sh = 0;
        while (i < n) {
            const uint8_t b = p[i++];
            v |= static_cast<uint64_t>(b & 0x7F) << sh;
            if (!(b & 0x80)) return true;
            sh += 7;
            if (sh > 63) return false;
        }
        return false;
    }
    bool field(int& fnum, int& wire) {
        uint64_t t;
        if (!varint(t)) return false;
        fnum = static_cast<int>(t >> 3);
        wire = static_cast<int>(t & 7);
        return true;
    }
    bool bytes(std::vector<uint8_t>& out) {
        uint64_t len;
        if (!varint(len) || i + len > n) return false;
        out.assign(p + i, p + i + len);
        i += len;
        return true;
    }
    bool fixed32(uint32_t& v) {
        if (i + 4 > n) return false;
        v = static_cast<uint32_t>(p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (uint32_t(p[i + 3]) << 24));
        i += 4;
        return true;
    }
    bool skip(int wire) {
        if (wire == 0) { uint64_t v; return varint(v); }
        if (wire == 2) { std::vector<uint8_t> b; return bytes(b); }
        if (wire == 5) { if (i + 4 > n) return false; i += 4; return true; }
        if (wire == 1) { if (i + 8 > n) return false; i += 8; return true; }
        return false;
    }
};

enum Port { TEXT = 1, POSITION = 3, NODEINFO = 4, ROUTING = 5, TELEMETRY = 67, ADMIN = 6, WAYPOINT = 8, TRACEROUTE = 70 };

struct Decoded {
    bool ok = false;        // the protobuf parsed as a Data message
    int portnum = -1;
    std::string text;       // TEXT_MESSAGE
    bool has_pos = false;
    double lat = 0, lon = 0;
    int alt_m = 0;
    std::string longname, shortname;
    int hwmodel = -1;
    double batt = -1, volt = -1, temp = -1e9, humidity = -1;   // telemetry
    std::string summary;    // a short human description
};

inline const char* port_name(int p) {
    switch (p) {
        case TEXT: return "Text";
        case POSITION: return "Position";
        case NODEINFO: return "NodeInfo";
        case ROUTING: return "Routing";
        case TELEMETRY: return "Telemetry";
        case ADMIN: return "Admin";
        case WAYPOINT: return "Waypoint";
        case TRACEROUTE: return "Traceroute";
        default: return "App";
    }
}

inline void parse_position(const uint8_t* d, size_t n, Decoded& out) {
    PbReader r(d, n);
    int f, w;
    while (!r.eof() && r.field(f, w)) {
        if (f == 1 && w == 5) { uint32_t v = 0; r.fixed32(v); out.lat = static_cast<int32_t>(v) / 1e7; out.has_pos = true; }
        else if (f == 2 && w == 5) { uint32_t v = 0; r.fixed32(v); out.lon = static_cast<int32_t>(v) / 1e7; out.has_pos = true; }
        else if (f == 3 && w == 0) { uint64_t v; r.varint(v); out.alt_m = static_cast<int32_t>(v); }
        else if (!r.skip(w)) break;
    }
}

inline void parse_nodeinfo(const uint8_t* d, size_t n, Decoded& out) {
    PbReader r(d, n);
    int f, w;
    while (!r.eof() && r.field(f, w)) {
        if (f == 2 && w == 2) { std::vector<uint8_t> b; r.bytes(b); out.longname.assign(b.begin(), b.end()); }
        else if (f == 3 && w == 2) { std::vector<uint8_t> b; r.bytes(b); out.shortname.assign(b.begin(), b.end()); }
        else if (f == 6 && w == 0) { uint64_t v; r.varint(v); out.hwmodel = static_cast<int>(v); }
        else if (!r.skip(w)) break;
    }
}

// Decode a (decrypted) Data protobuf: field 1 = portnum (varint), 2 = payload.
inline Decoded parse_data(const std::vector<uint8_t>& dec) {
    Decoded out;
    PbReader r(dec.data(), dec.size());
    int f, w;
    std::vector<uint8_t> inner;
    bool got_port = false;
    while (!r.eof() && r.field(f, w)) {
        if (f == 1 && w == 0) { uint64_t v; if (!r.varint(v)) return out; out.portnum = static_cast<int>(v); got_port = true; }
        else if (f == 2 && w == 2) { if (!r.bytes(inner)) return out; }
        else if (!r.skip(w)) return out;
    }
    if (!got_port || out.portnum < 0 || out.portnum > 511) return out;
    out.ok = true;
    if (out.portnum == TEXT) {
        for (uint8_t c : inner) out.text += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    } else if (out.portnum == POSITION) {
        parse_position(inner.data(), inner.size(), out);
    } else if (out.portnum == NODEINFO) {
        parse_nodeinfo(inner.data(), inner.size(), out);
    }
    return out;
}

}  // namespace mesh
