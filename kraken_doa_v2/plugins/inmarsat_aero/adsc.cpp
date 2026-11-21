// ADS-C (FANS-1/A) inside ACARS text.
//
// ARINC 622 framing of the FANS applications: the text is "/" (optional),
// the ground station address (7 or 4 characters), ".", the IMI (3: ADS =
// ADS-C, DIS = ADS-C disconnect, AT1 / CR1 / CC1 / DR1 = CPDLC), the
// aircraft registration (7, '.' padded), then the binary message in hex,
// its last two octets a CRC: over IMI + registration + the binary
// including the CRC the ARINC CRC-16 (table of x^16 + x^12 + x^5 + 1,
// non-reflected, run LSB-side, preset 0xFFFF) leaves 0x1D0F.
//
// ADS-C downlink (ARINC 745): a sequence of groups, each a tag octet and a
// fixed-length body read MSB first: 7 basic report (lat / lon 21 bits
// two's complement, MSB = 90 deg; altitude 16 bits x 4 ft; time 15 bits x
// 0.125 s past the hour; figure of merit 7), 9 emergency basic report, 10 /
// 18 / 19 / 20 event reports (as the basic report), 12 flight ID (8 x 6-bit
// ISO 5), 13 predicted route, 14 earth reference (track 12 bits, ground
// speed 13 bits / 2 kt, vertical rate 12 bits x 16 ft/min), 15 air
// reference, 16 meteo (wind speed 9 bits / 2 kt, direction 9 bits,
// temperature 12 bits), 17 airframe ID (ICAO 24 bits), 22 intermediate and
// 23 fixed projections, 3 acknowledgement (contract number), 4 negative
// acknowledgement, 6 cancel emergency.
//
// Facts (field layout, scaling, the CRC's parameters) as libacars
// documents them (MIT licence, github.com/szpajder/libacars, adsc.c /
// arinc.c / crc.c - read as a description; this code is written from it,
// not copied). CPDLC (ASN.1 PER) is recognised and counted, not decoded.

#include "adsc.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace aero {

namespace {

uint16_t arinc_crc(const uint8_t* p, size_t n, uint16_t c) {
    static const std::array<uint16_t, 256> t = [] {
        std::array<uint16_t, 256> a{};
        for (int i = 0; i < 256; i++) {
            uint16_t v = static_cast<uint16_t>(i << 8);
            for (int k = 0; k < 8; k++) v = (v & 0x8000) ? static_cast<uint16_t>((v << 1) ^ 0x1021) : static_cast<uint16_t>(v << 1);
            a[i] = v;
        }
        return a;
    }();
    for (size_t i = 0; i < n; i++) c = static_cast<uint16_t>((c >> 8) ^ t[(c ^ p[i]) & 0xFF]);
    return c;
}

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    Bits(const uint8_t* d, size_t len) : p(d), n(len * 8) {}
    bool get(int k, uint32_t& v) {
        if (pos + static_cast<size_t>(k) > n) return false;
        v = 0;
        for (int i = 0; i < k; i++, pos++) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
        return true;
    }
};

int32_t sext(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    return static_cast<int32_t>((v ^ m) - m);
}
double coord(uint32_t v) { return sext(v, 21) * (180.0 - 90.0 / 524288.0) / 1048575.0; }
double angle12(uint32_t v) {
    double a = sext(v, 12) * (180.0 - 90.0 / 1024.0) / 2047.0;
    return a < 0 ? a + 360 : a;
}
double alt16(uint32_t v) { return sext(v, 16) * 4.0; }

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool upper_or_digit(const std::string& s, size_t a, size_t n) {
    if (a + n > s.size()) return false;
    for (size_t i = a; i < a + n; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}

}  // namespace

bool adsc_decode(const std::string& label, const std::string& text, AdscReport& r) {
    // find the IMI after a 7- or 4-character ground address
    static const char* imis[] = {".ADS", ".DIS", ".AT1", ".CR1", ".CC1", ".DR1"};
    size_t imi = std::string::npos;
    const char* which = nullptr;
    for (const char* m : imis) {
        const size_t p = text.find(m);
        if (p != std::string::npos && (imi == std::string::npos || p < imi)) {
            imi = p;
            which = m + 1;
        }
    }
    if (imi == std::string::npos) return false;
    // the ground address right before it (7 characters, else 4), after a
    // '/' or the text's start (H1 messages put a sublabel / MFI first)
    size_t gl = 0;
    for (size_t l : {static_cast<size_t>(7), static_cast<size_t>(4)})
        if (imi >= l && upper_or_digit(text, imi - l, l) && (imi == l || text[imi - l - 1] == '/' || text[imi - l - 1] == ' ')) {
            gl = l;
            break;
        }
    if (gl == 0) return false;
    const size_t start = imi - gl;
    const size_t prot = imi + 1;   // IMI + registration + hex
    if (prot + 10 + 4 > text.size()) return false;
    std::vector<uint8_t> buf(text.begin() + static_cast<long>(prot), text.begin() + static_cast<long>(prot + 10));
    size_t h = prot + 10;
    std::vector<uint8_t> bin;
    while (h + 1 < text.size()) {
        const int a = hexval(text[h]), b = hexval(text[h + 1]);
        if (a < 0 || b < 0) break;
        bin.push_back(static_cast<uint8_t>(a << 4 | b));
        h += 2;
    }
    if (bin.size() < 3) return false;
    buf.insert(buf.end(), bin.begin(), bin.end());
    if (arinc_crc(buf.data(), buf.size(), 0xFFFF) != 0x1D0F) return false;
    bin.resize(bin.size() - 2);
    const std::string gs = text.substr(start, gl);
    const std::string reg = text.substr(prot + 3, 7);
    const std::string w(which);
    // downlink: label B6 (ADS-C) / BA (CPDLC) or a numeric block; uplink: A6 / AA
    const bool down = !label.empty() && label[0] == 'B';
    char b[200];
    if (w != "ADS") {
        const char* what = w == "DIS"   ? "ADS-C disconnect"
                           : w == "AT1" ? "CPDLC message"
                           : w == "CR1" ? "CPDLC connect request"
                           : w == "CC1" ? "CPDLC connect confirm"
                                        : "CPDLC disconnect request";
        snprintf(b, sizeof b, "%s, %s %s, %zu octets (not decoded)", what, down ? "to" : "from", gs.c_str(), bin.size());
        r.summary = b;
        return true;
    }
    if (!down) {
        snprintf(b, sizeof b, "contract request from %s, %zu octets", gs.c_str(), bin.size());
        r.summary = b;
        return true;
    }
    Bits bs(bin.data(), bin.size());
    std::string sum;
    uint32_t tag;
    while (bs.get(8, tag)) {
        uint32_t v, v2, v3;
        switch (tag) {
        case 3:   // acknowledgement: contract number
            if (!bs.get(8, v)) return !sum.empty();
            snprintf(b, sizeof b, "ack contract %u; ", v);
            sum += b;
            break;
        case 7: case 9: case 10: case 18: case 19: case 20: {   // basic report (+ events)
            uint32_t la, lo, al, ts, fom;
            if (!(bs.get(21, la) && bs.get(21, lo) && bs.get(16, al) && bs.get(15, ts) && bs.get(7, fom))) return !sum.empty();
            r.has_pos = true;
            r.lat = coord(la);
            r.lon = coord(lo);
            r.alt_ft = alt16(al);
            const char* kind = tag == 7 ? "" : tag == 9 ? "⚠ EMERGENCY " : tag == 10 ? "lateral deviation event " : tag == 18 ? "vertical rate event " : tag == 19 ? "altitude range event " : "waypoint change event ";
            const double sec = ts * 0.125;
            snprintf(b, sizeof b, "%s%.4f %.4f, %.0f ft, %02d:%04.1f past the hour; ", kind, r.lat, r.lon, r.alt_ft,
                     static_cast<int>(sec / 60), sec - 60 * static_cast<int>(sec / 60));
            sum += b;
            snprintf(b, sizeof b, "\nReport time: %02d:%04.1f past the hour\nFigure of merit: %u", static_cast<int>(sec / 60),
                     sec - 60 * static_cast<int>(sec / 60), (fom >> 1) & 7);
            r.info += b;
            if (tag == 9) r.info += "\nEmergency: yes";
            break;
        }
        case 12: {   // flight ID
            std::string id;
            for (int i = 0; i < 8; i++) {
                if (!bs.get(6, v)) return !sum.empty();
                if ((v & 0x20) == 0) v += 0x40;
                id += static_cast<char>(v);
            }
            while (!id.empty() && id.back() == ' ') id.pop_back();
            r.info += "\nFlight ID: " + id;
            sum += "flight " + id + "; ";
            break;
        }
        case 13: {   // predicted route: next and next+1 waypoints
            uint32_t a1, o1, h1, e1, a2, o2, h2;
            if (!(bs.get(21, a1) && bs.get(21, o1) && bs.get(16, h1) && bs.get(14, e1) && bs.get(21, a2) && bs.get(21, o2) &&
                  bs.get(16, h2)))
                return !sum.empty();
            snprintf(b, sizeof b, "\nNext waypoint: %.4f %.4f, %.0f ft, in %u s\nNext + 1: %.4f %.4f, %.0f ft", coord(a1), coord(o1),
                     alt16(h1), e1, coord(a2), coord(o2), alt16(h2));
            r.info += b;
            break;
        }
        case 14: case 15: {   // earth / air reference
            uint32_t inv, hd, sp, vs;
            if (!(bs.get(1, inv) && bs.get(12, hd) && bs.get(13, sp) && bs.get(12, vs))) return !sum.empty();
            if (tag == 14) {
                if (!inv) r.track = angle12(hd);
                r.gs_kt = sp / 2.0;
                snprintf(b, sizeof b, "\nTrack: %.1f deg%s\nGround speed: %.1f kt\nVertical rate: %d ft/min", angle12(hd),
                         inv ? " (invalid)" : "", sp / 2.0, sext(vs, 12) * 16);
                r.info += b;
                char s2[64];
                snprintf(s2, sizeof s2, "track %.0f, %.0f kt; ", angle12(hd), sp / 2.0);
                sum += s2;
            } else {
                snprintf(b, sizeof b, "\nTrue heading: %.1f deg%s\nMach: %.3f", angle12(hd), inv ? " (invalid)" : "", sp / 2000.0);
                r.info += b;
            }
            break;
        }
        case 16: {   // meteo
            uint32_t ws, dinv, wd, t;
            if (!(bs.get(9, ws) && bs.get(1, dinv) && bs.get(9, wd) && bs.get(12, t))) return !sum.empty();
            double dir = sext(wd, 9) * (180.0 - 90.0 / 128.0) / 255.0;
            if (dir < 0) dir += 360;
            snprintf(b, sizeof b, "\nWind: %.0f deg / %.1f kt%s\nTemperature: %.2f C", dir, ws / 2.0, dinv ? " (direction invalid)" : "",
                     sext(t, 12) * (512.0 - 256.0 / 1024.0) / 2047.0);
            r.info += b;
            break;
        }
        case 17:   // airframe ID
            if (!bs.get(24, v)) return !sum.empty();
            snprintf(b, sizeof b, "\nAirframe ICAO: %06X", v);
            r.info += b;
            break;
        case 22:   // intermediate projection
            if (!(bs.get(16, v) && bs.get(1, v2) && bs.get(12, v3) && bs.get(16, v2) && bs.get(14, v))) return !sum.empty();
            break;
        case 23: {   // fixed projection
            uint32_t a1, o1, h1, e1;
            if (!(bs.get(21, a1) && bs.get(21, o1) && bs.get(16, h1) && bs.get(14, e1))) return !sum.empty();
            snprintf(b, sizeof b, "\nFixed projection: %.4f %.4f, %.0f ft, in %u s", coord(a1), coord(o1), alt16(h1), e1);
            r.info += b;
            break;
        }
        case 6:
            sum += "cancel emergency; ";
            break;
        default:
            // an unknown group: its length is unknown - stop here
            if (sum.empty()) {
                snprintf(b, sizeof b, "group %u (not decoded)", tag);
                sum = b;
            }
            r.summary = sum;
            return true;
        }
    }
    while (!sum.empty() && (sum.back() == ' ' || sum.back() == ';')) sum.pop_back();
    r.summary = sum;
    return true;
}

}  // namespace aero
