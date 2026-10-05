#pragma once

// AX.25 UI frame + APRS information field parsing (no DSP here).
//
// AX.25 v2.2 address field: 7 bytes per address, callsign characters shifted
// left by one, SSID byte = 0b CRR SSID E (E = 1 on the last address, H/C bit
// = 0x80 = "has been repeated" on digipeater addresses). Up to 8 digipeaters.
// APRS (APRS Protocol Reference 1.0.1, ch. 5-10): the first info byte is the
// data type identifier; positions are uncompressed (DDMM.hhN/DDDMM.hhW),
// base-91 compressed, or Mic-E (latitude in the destination callsign).

#include <cstdint>
#include <cstdio>
#include <string>

namespace aprs {

struct Frame {
    std::string src, dst, path;   // path: "WIDE1-1*,WIDE2-1"
    bool ui = false;              // UI frame, PID 0xF0 (no layer 3)
    std::string info;             // raw information field
    // decoded APRS content
    std::string type;             // "position", "Mic-E", "object", "status", ...
    bool has_pos = false;
    double lat = 0, lon = 0;
    char sym_table = 0, sym_code = 0;
    std::string name;             // object / item name, message addressee
    std::string comment;          // text after the position / status / message
};

// Validates and splits the address field. Returns false if the bytes cannot
// be an AX.25 address field (bad characters, extension bits).
inline bool parse_address(const uint8_t* b, int len, Frame& f, int& hdr_len) {
    auto call = [&](const uint8_t* a, std::string& out) -> bool {
        char s[12];
        int n = 0;
        bool space = false;
        for (int i = 0; i < 6; i++) {
            if (a[i] & 1) return false;   // extension bit only in the SSID byte
            char c = static_cast<char>(a[i] >> 1);
            if (c == ' ') { space = true; continue; }
            if (space) return false;       // padding only at the end
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
            s[n++] = c;
        }
        if (n == 0) return false;
        int ssid = (a[6] >> 1) & 15;
        if (ssid) n += snprintf(s + n, sizeof s - n, "-%d", ssid);
        out.assign(s, n);
        return true;
    };
    if (len < 16) return false;
    if (!call(b, f.dst) || !call(b + 7, f.src)) return false;
    if (b[6] & 1) return false;   // there must be a source address after dst
    int pos = 14;
    bool last = b[13] & 1;
    int digis = 0;
    f.path.clear();
    while (!last) {
        if (++digis > 8 || pos + 7 > len) return false;
        std::string d;
        if (!call(b + pos, d)) return false;
        if (!f.path.empty()) f.path += ',';
        f.path += d;
        if (b[pos + 6] & 0x80) f.path += '*';
        last = b[pos + 6] & 1;
        pos += 7;
    }
    hdr_len = pos;
    return true;
}

namespace detail {

inline bool digit(char c) { return c >= '0' && c <= '9'; }

// "4903.50N" / "07201.75W" (ambiguity: digits may be spaces). Returns false
// if malformed or out of range.
inline bool uncompressed(const std::string& s, size_t p, Frame& f) {
    if (s.size() < p + 19) return false;
    const char* q = s.c_str() + p;
    char t[20];
    for (int i = 0; i < 19; i++) t[i] = q[i] == ' ' ? '0' : q[i];
    // lat: DDMM.hhN
    for (int i : {0, 1, 2, 3, 5, 6})
        if (!digit(t[i])) return false;
    if (t[4] != '.' || (t[7] != 'N' && t[7] != 'S')) return false;
    // lon: DDDMM.hhE at 9
    for (int i : {9, 10, 11, 12, 13, 15, 16})
        if (!digit(t[i])) return false;
    if (t[14] != '.' || (t[17] != 'E' && t[17] != 'W')) return false;
    double lat = (t[0] - '0') * 10 + (t[1] - '0') + ((t[2] - '0') * 10 + (t[3] - '0') + (t[5] - '0') / 10.0 + (t[6] - '0') / 100.0) / 60.0;
    double lon = (t[9] - '0') * 100 + (t[10] - '0') * 10 + (t[11] - '0') +
                 ((t[12] - '0') * 10 + (t[13] - '0') + (t[15] - '0') / 10.0 + (t[16] - '0') / 100.0) / 60.0;
    if (lat > 90 || lon > 180) return false;
    f.lat = t[7] == 'S' ? -lat : lat;
    f.lon = t[17] == 'W' ? -lon : lon;
    f.sym_table = q[8];
    f.sym_code = q[18];
    f.has_pos = true;
    f.comment = s.substr(p + 19);
    return true;
}

// base-91 compressed: T YYYY XXXX $ cs T (13 bytes)
inline bool compressed(const std::string& s, size_t p, Frame& f) {
    if (s.size() < p + 13) return false;
    const char* q = s.c_str() + p;
    char t = q[0];
    if (!(t == '/' || t == '\\' || (t >= 'A' && t <= 'Z') || (t >= 'a' && t <= 'j'))) return false;
    long y = 0, x = 0;
    for (int i = 0; i < 4; i++) {
        int a = q[1 + i] - 33, b = q[5 + i] - 33;
        if (a < 0 || a > 90 || b < 0 || b > 90) return false;
        y = y * 91 + a;
        x = x * 91 + b;
    }
    double lat = 90.0 - y / 380926.0, lon = -180.0 + x / 190463.0;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    f.lat = lat;
    f.lon = lon;
    f.sym_table = t;
    f.sym_code = q[9];
    f.has_pos = true;
    f.comment = s.substr(p + 13);
    return true;
}

inline bool position(const std::string& s, size_t p, Frame& f) {
    if (p >= s.size()) return false;
    if (digit(s[p]) || s[p] == ' ') return uncompressed(s, p, f);
    return compressed(s, p, f);
}

// Mic-E (APRS 1.0.1 ch. 10): latitude, N/S, longitude offset and E/W in the
// 6 destination characters; longitude, speed, course in info bytes 1..8.
inline bool mic_e(const std::string& dst_call, const std::string& s, Frame& f) {
    std::string d = dst_call.substr(0, dst_call.find('-'));
    if (d.size() != 6 || s.size() < 9) return false;
    int dig[6];
    bool hi[6];   // the "P-Z" (or A-K custom) range of each character
    for (int i = 0; i < 6; i++) {
        char c = d[i];
        if (c >= '0' && c <= '9') { dig[i] = c - '0'; hi[i] = false; }
        else if (c >= 'A' && c <= 'J') { dig[i] = c - 'A'; hi[i] = true; }
        else if (c >= 'P' && c <= 'Y') { dig[i] = c - 'P'; hi[i] = true; }
        else if (c == 'K' || c == 'Z') { dig[i] = 0; hi[i] = true; }
        else if (c == 'L') { dig[i] = 0; hi[i] = false; }
        else return false;
        // A-K only valid in the message bits (first three characters)
        if (i >= 3 && c >= 'A' && c <= 'K') return false;
    }
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + dig[4] / 10.0 + dig[5] / 100.0) / 60.0;
    bool north = hi[3], lon100 = hi[4], west = hi[5];
    int deg = static_cast<unsigned char>(s[1]) - 28;
    if (lon100) deg += 100;
    if (deg >= 180 && deg <= 189) deg -= 80;
    else if (deg >= 190 && deg <= 199) deg -= 190;
    int min = static_cast<unsigned char>(s[2]) - 28;
    if (min >= 60) min -= 60;
    int hun = static_cast<unsigned char>(s[3]) - 28;
    if (deg < 0 || deg > 179 || min < 0 || min > 59 || hun < 0 || hun > 99 || lat > 90) return false;
    double lon = deg + (min + hun / 100.0) / 60.0;
    f.lat = north ? lat : -lat;
    f.lon = west ? -lon : lon;
    f.sym_code = s[7];
    f.sym_table = s[8];
    f.has_pos = true;
    int sp = (static_cast<unsigned char>(s[4]) - 28) * 10 + (static_cast<unsigned char>(s[5]) - 28) / 10;
    if (sp >= 800) sp -= 800;
    int crs = ((static_cast<unsigned char>(s[5]) - 28) % 10) * 100 + (static_cast<unsigned char>(s[6]) - 28);
    if (crs >= 400) crs -= 400;
    char b[48];
    if (sp > 0 && sp < 800 && crs >= 0 && crs <= 360) {
        snprintf(b, sizeof b, "%d km/h %d deg ", static_cast<int>(sp * 1.852 + 0.5), crs);
        f.comment = b;
    }
    f.comment += s.substr(9);
    return true;
}

inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

}  // namespace detail

// Fills type / position / name / comment from f.info.
inline void parse_info(Frame& f) {
    using namespace detail;
    const std::string& s = f.info;
    if (!f.ui) { f.type = "AX.25 (non-UI)"; return; }
    if (s.empty()) { f.type = "empty"; return; }
    switch (s[0]) {
    case '!':
    case '=':
        f.type = "position";
        if (!position(s, 1, f)) f.type = "position (unparsed)";
        break;
    case '/':
    case '@':
        f.type = "position";
        if (s.size() < 8 || !position(s, 8, f)) f.type = "position (unparsed)";
        break;
    case '`':
    case '\'':
    case 0x1c:
    case 0x1d:
        f.type = "Mic-E position";
        if (!mic_e(f.dst, s, f)) f.type = "Mic-E (unparsed)";
        break;
    case ';':
        f.type = "object";
        if (s.size() >= 18) {
            f.name = trim(s.substr(1, 9));
            if (s[10] == '_') f.type = "object (killed)";
            if (!position(s, 18, f)) f.type += " (unparsed)";
        }
        break;
    case ')': {
        f.type = "item";
        size_t e = s.find_first_of("!_", 1);
        if (e != std::string::npos && e >= 4 && e <= 10) {
            f.name = s.substr(1, e - 1);
            if (!position(s, e + 1, f)) f.type += " (unparsed)";
        }
        break;
    }
    case ':':
        f.type = "message";
        if (s.size() >= 11 && s[10] == ':') {
            f.name = trim(s.substr(1, 9));
            f.comment = s.substr(11);
            if (f.name.rfind("BLN", 0) == 0) f.type = "bulletin";
        }
        break;
    case '>': f.type = "status"; f.comment = s.substr(1); break;
    case '_': f.type = "weather"; f.comment = s.substr(1); break;
    case 'T': f.type = "telemetry"; f.comment = s.substr(1); break;
    case '$': f.type = "raw GPS"; f.comment = s; break;
    case '<': f.type = "capabilities"; f.comment = s.substr(1); break;
    case '?': f.type = "query"; f.comment = s; break;
    case '}': f.type = "third-party"; f.comment = s.substr(1); break;
    default: f.type = "other"; f.comment = s; break;
    }
    // weather station symbol: the comment carries the weather data
    if (f.has_pos && f.sym_code == '_') f.type += " + weather";
    f.comment = trim(f.comment);
}

}  // namespace aprs
