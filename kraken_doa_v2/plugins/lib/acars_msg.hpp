#pragma once

// ACARS message fields (ARINC 618 block contents), shared by the plugins
// that receive ACARS: acars (VHF, 2400 bit/s MSK) and vdl2 (ACARS over
// AVLC). Header-only.
//
// A block from the mode character on: mode, address (7: the registration
// right-aligned, '.' padded; NULs = all aircraft), technical acknowledgement
// (NAK or a block ID), label (2), block ID (a digit: downlink, a letter or
// NUL: uplink), STX (ETX: no text), text, ETX / ETB. Characters are 7-bit
// ASCII with an odd parity bit, which is stripped here.

#include "kraken_dsp.hpp"

#include <cctype>
#include <cstdint>
#include <string>

namespace acars {

constexpr char STX = 0x02, ETX = 0x03, ETB = 0x17, NAK = 0x15, DEL = 0x7F;

struct Block {
    char mode = 0, ack = 0, bid = 0;
    std::string addr, label, text;   // raw characters (parity stripped)
    bool etb = false;                // ETB: more blocks of the message follow
    bool down = false;               // downlink (aircraft -> ground): a digit block ID
};

// c[0 .. n-1] = mode .. the suffix (ETX / ETB) inclusive; false if too short
inline bool parse(const uint8_t* c, int n, Block& b) {
    if (n < 13) return false;
    b = Block{};
    b.mode = static_cast<char>(c[0] & 0x7F);
    for (int i = 1; i <= 7; i++) b.addr += static_cast<char>(c[i] & 0x7F);
    b.ack = static_cast<char>(c[8] & 0x7F);
    b.label = {static_cast<char>(c[9] & 0x7F), static_cast<char>(c[10] & 0x7F)};
    b.bid = static_cast<char>(c[11] & 0x7F);
    if ((c[12] & 0x7F) == STX)
        for (int i = 13; i < n - 1; i++) b.text += static_cast<char>(c[i] & 0x7F);
    b.etb = (c[n - 1] & 0x7F) == ETB;
    b.down = b.bid >= '0' && b.bid <= '9';
    return true;
}

// the registration without its '.' padding ("" = all aircraft), printable
inline std::string registration(const std::string& a) {
    size_t i = 0;
    while (i < a.size() && (a[i] == '.' || a[i] == ' ' || a[i] == '\0')) i++;
    std::string r = a.substr(i);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\0')) r.pop_back();
    return kp::printable(r);
}

// a label as it is usually written: "_d" for the general response _DEL
inline std::string label_text(const std::string& l) {
    if (l.size() == 2 && l[0] == '_' && l[1] == DEL) return "_d";
    return kp::printable(l);
}

// names of a few common labels ("" = no name)
inline const char* label_name(const std::string& l) {
    if (l.size() == 2 && l[0] == '_' && l[1] == DEL) return "general response";
    if (l == "Q0") return "link test";
    if (l == "SQ") return "squitter";
    if (l == "SA") return "media advisory";
    if (l == "H1") return "message to / from terminal";
    return "";
}

// message text for one line: CR / LF as spaces, other controls escaped
inline std::string one_line(const std::string& s) {
    std::string r;
    for (char ch : s) {
        if (ch == '\r' || ch == '\n') {
            if (!r.empty() && r.back() != ' ') r += ' ';
        } else {
            r += ch;
        }
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return kp::printable(r);
}

// a downlink's text starts with the message sequence number (letter, two
// digits, block letter: "M01A") and the flight ID (6: "BA0123")
inline bool split_downlink(const std::string& t, std::string& msn, std::string& flight, std::string& body) {
    if (t.size() < 10) return false;
    auto al = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
    auto dg = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    if (!al(t[0]) || !dg(t[1]) || !dg(t[2]) || !al(t[3])) return false;
    for (int i = 4; i < 10; i++)
        if (!(al(t[i]) || dg(t[i]) || t[i] == ' ')) return false;
    msn = t.substr(0, 4);
    flight = t.substr(4, 6);
    while (!flight.empty() && flight.back() == ' ') flight.pop_back();
    size_t k = 0;
    while (k < flight.size() && flight[k] == ' ') k++;
    flight = flight.substr(k);
    body = t.substr(10);
    return true;
}

// a ground station's squitter (label SQ): version (2 digits), 'X', the
// network (A = ARINC, S = SITA), the station's IATA airport code
inline bool squitter(const Block& b, std::string& net, std::string& iata) {
    const std::string& t = b.text;
    if (b.label != "SQ" || t.size() < 7) return false;
    if (!std::isdigit(static_cast<unsigned char>(t[0])) || !std::isdigit(static_cast<unsigned char>(t[1])) || t[2] != 'X')
        return false;
    if (t[3] != 'A' && t[3] != 'S') return false;
    for (int i = 4; i < 7; i++)
        if (!std::isupper(static_cast<unsigned char>(t[i]))) return false;
    net = t[3] == 'A' ? "ARINC" : "SITA";
    iata = t.substr(4, 3);
    return true;
}

}  // namespace acars
