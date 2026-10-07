#pragma once

// AIS message fields (ITU-R M.1371-5): the bit layouts of the messages the
// ais plugin decodes, over a frame's payload as MSB-first bits.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ais {

class Bits {
public:
    explicit Bits(std::vector<uint8_t> b) : b_(std::move(b)) {}
    int size() const { return static_cast<int>(b_.size()); }
    bool has(int start, int len) const { return start >= 0 && len >= 0 && start + len <= size(); }
    uint32_t u(int start, int len) const {
        uint32_t v = 0;
        for (int i = 0; i < len; i++) v = (v << 1) | (has(start + i, 1) ? b_[static_cast<size_t>(start + i)] : 0);
        return v;
    }
    int32_t s(int start, int len) const {
        uint32_t v = u(start, len);
        if (len > 0 && len < 32 && (v >> (len - 1)) & 1) return static_cast<int32_t>(v) - (1 << len);
        return static_cast<int32_t>(v);
    }
    // six-bit ASCII ("@" padding and trailing spaces removed)
    std::string text(int start, int nchars) const {
        std::string t;
        for (int i = 0; i < nchars && has(start + 6 * i, 6); i++) {
            uint32_t c = u(start + 6 * i, 6);
            t += static_cast<char>(c < 32 ? c + 64 : c);
        }
        const size_t at = t.find('@');
        if (at != std::string::npos) t.resize(at);
        while (!t.empty() && t.back() == ' ') t.pop_back();
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        return t;
    }
    const std::vector<uint8_t>& raw() const { return b_; }
private:
    std::vector<uint8_t> b_;
};

// position: 1/10000 minute (28 / 27 bits); 181 / 91 deg = not available
inline bool position(const Bits& m, int lon_at, int lat_at, double* lat, double* lon, int lon_bits = 28,
                     int lat_bits = 27, double scale = 600000.0) {
    const double lo = m.s(lon_at, lon_bits) / scale, la = m.s(lat_at, lat_bits) / scale;
    if (!(std::fabs(lo) <= 180 && std::fabs(la) <= 90)) return false;
    if (lo == 0 && la == 0) return false;
    *lat = la;
    *lon = lo;
    return true;
}

inline const char* nav_status(int s) {
    static const char* n[16] = {"under way using engine", "at anchor", "not under command", "restricted manoeuvrability",
                                "constrained by draught", "moored", "aground", "engaged in fishing",
                                "under way sailing", "reserved (HSC)", "reserved (WIG)", "towing astern",
                                "pushing ahead / towing alongside", "reserved", "AIS-SART / MOB / EPIRB active",
                                "not defined"};
    return n[s & 15];
}

inline std::string ship_type(int t) {
    if (t <= 0 || t > 99) return "";
    if (t >= 20 && t <= 29) return "wing in ground";
    switch (t) {
        case 30: return "fishing";
        case 31: case 32: return "towing";
        case 33: return "dredging / underwater ops";
        case 34: return "diving ops";
        case 35: return "military ops";
        case 36: return "sailing";
        case 37: return "pleasure craft";
        case 50: return "pilot vessel";
        case 51: return "search and rescue";
        case 52: return "tug";
        case 53: return "port tender";
        case 54: return "anti-pollution";
        case 55: return "law enforcement";
        case 58: return "medical transport";
        case 59: return "noncombatant ship";
        default: break;
    }
    if (t >= 40 && t <= 49) return "high-speed craft";
    if (t >= 60 && t <= 69) return "passenger";
    if (t >= 70 && t <= 79) return "cargo";
    if (t >= 80 && t <= 89) return "tanker";
    if (t >= 90 && t <= 99) return "other";
    return "type " + std::to_string(t);
}

inline const char* aton_type(int t) {
    static const char* n[32] = {"aid to navigation", "reference point", "RACON", "fixed structure", "spare",
                                "light", "light with sectors", "leading light front", "leading light rear",
                                "beacon cardinal N", "beacon cardinal E", "beacon cardinal S", "beacon cardinal W",
                                "beacon port hand", "beacon starboard hand", "beacon preferred channel port",
                                "beacon preferred channel starboard", "beacon isolated danger", "beacon safe water",
                                "beacon special mark", "cardinal mark N", "cardinal mark E", "cardinal mark S",
                                "cardinal mark W", "port hand mark", "starboard hand mark",
                                "preferred channel port", "preferred channel starboard", "isolated danger",
                                "safe water", "special mark", "light vessel / LANBY / rig"};
    return n[t & 31];
}

// NMEA 6-bit armouring of a payload -> !AIVDM sentence(s), channel "A" / "B"
inline std::string nmea(const std::vector<uint8_t>& bits, const char* chan, int seq) {
    std::string p;
    const int fill = (6 - static_cast<int>(bits.size()) % 6) % 6;
    for (size_t i = 0; i < bits.size(); i += 6) {
        int v = 0;
        for (size_t k = 0; k < 6; k++) v = (v << 1) | (i + k < bits.size() ? bits[i + k] : 0);
        p += static_cast<char>(v < 40 ? v + 48 : v + 56);
    }
    const size_t per = 60;
    const int n = static_cast<int>((p.size() + per - 1) / per);
    std::string out;
    for (int k = 0; k < n; k++) {
        std::string body = "AIVDM," + std::to_string(n) + "," + std::to_string(k + 1) + "," +
                           (n > 1 ? std::to_string(seq % 10) : "") + "," + chan + "," + p.substr(k * per, per) + "," +
                           std::to_string(k == n - 1 ? fill : 0);
        uint8_t x = 0;
        for (char c : body) x ^= static_cast<uint8_t>(c);
        char cs[8];
        snprintf(cs, sizeof cs, "*%02X", x);
        out += (k ? " !" : "!") + body + cs;
    }
    return out;
}

}  // namespace ais
