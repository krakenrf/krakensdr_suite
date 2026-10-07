#include "modes.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace modes {

namespace {
const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 16;
            for (int k = 0; k < 8; k++) c = (c & 0x800000) ? (c << 1) ^ 0xFFF409 : c << 1;
            a[i] = c & 0xFFFFFF;
        }
        return a;
    }();
    return t;
}

// syndrome -> bit of a 112-bit message
const std::unordered_map<uint32_t, int>& error_table() {
    static const std::unordered_map<uint32_t, int> t = [] {
        std::unordered_map<uint32_t, int> m;
        for (int b = 0; b < 112; b++) {
            uint8_t msg[14] = {};
            msg[b >> 3] = static_cast<uint8_t>(0x80 >> (b & 7));
            m[syndrome(msg, 112)] = b;
        }
        return m;
    }();
    return t;
}

double cpr_mod(double a, double b) {
    double r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}
int cpr_mod(int a, int b) {
    int r = a % b;
    return r < 0 ? r + b : r;
}
double wrap180(double lon) {
    lon = std::fmod(lon + 180.0, 360.0);
    if (lon < 0) lon += 360.0;
    return lon - 180.0;
}
}  // namespace

uint32_t crc24(const uint8_t* msg, int nbytes) {
    const auto& t = crc_table();
    uint32_t c = 0;
    for (int i = 0; i < nbytes; i++) c = ((c << 8) ^ t[((c >> 16) ^ msg[i]) & 0xFF]) & 0xFFFFFF;
    return c;
}

uint32_t syndrome(const uint8_t* msg, int nbits) {
    const int n = nbits / 8;
    uint32_t parity = (static_cast<uint32_t>(msg[n - 3]) << 16) | (static_cast<uint32_t>(msg[n - 2]) << 8) | msg[n - 1];
    return crc24(msg, n - 3) ^ parity;
}

uint32_t bit_syndrome(int b) {
    static const std::array<uint32_t, 112> t = [] {
        std::array<uint32_t, 112> a{};
        for (int i = 0; i < 112; i++) {
            uint8_t msg[14] = {};
            msg[i >> 3] = static_cast<uint8_t>(0x80 >> (i & 7));
            a[i] = syndrome(msg, 112);
        }
        return a;
    }();
    return (b >= 0 && b < 112) ? t[b] : 0;
}

int single_bit_error(uint32_t syn) {
    const auto& t = error_table();
    auto it = t.find(syn);
    return it == t.end() ? -1 : it->second;
}

namespace {
// Gillham (Mode C) code: 500 ft Gray code D2 D4 A1 A2 A4 B1 B2 B4, 100 ft
// part C1 C2 C4 (1..5, reflected when the 500 ft count is odd)
float gillham(int a1, int a2, int a4, int b1, int b2, int b4, int c1, int c2, int c4, int d2, int d4) {
    int gray = (d2 << 7) | (d4 << 6) | (a1 << 5) | (a2 << 4) | (a4 << 3) | (b1 << 2) | (b2 << 1) | b4;
    int n500 = 0;
    for (int g = gray; g; g >>= 1) n500 ^= g;   // Gray -> binary
    int c = (c1 << 2) | (c2 << 1) | c4;
    int n100;
    switch (c) {
        case 1: n100 = 1; break;   // 001
        case 3: n100 = 2; break;   // 011
        case 2: n100 = 3; break;   // 010
        case 6: n100 = 4; break;   // 110
        case 4: n100 = 5; break;   // 100
        default: return NAN;
    }
    if (n500 & 1) n100 = 6 - n100;
    float alt = static_cast<float>(n500 * 500 + n100 * 100 - 1300);
    return alt < -1200 ? NAN : alt;
}
}  // namespace

// AC13: C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4
float ac13_altitude_ft(uint32_t ac) {
    if (ac == 0) return NAN;
    const int m = (ac >> 6) & 1, q = (ac >> 4) & 1;
    if (m) return NAN;   // metric altitude: not used in practice
    if (q) {
        uint32_t n = ((ac & 0x1F80) >> 2) | ((ac & 0x0020) >> 1) | (ac & 0x000F);
        return static_cast<float>(static_cast<int>(n) * 25 - 1000);
    }
    auto b = [&](int pos) { return static_cast<int>((ac >> (12 - pos)) & 1); };   // pos 0 = C1
    return gillham(b(1), b(3), b(5), b(7), b(9), b(11), b(0), b(2), b(4), b(10), b(12));
}

// AC12 (ADS-B): C1 A1 C2 A2 C4 A4 B1 Q B2 D2 B4 D4 (the M bit left out)
float ac12_altitude_ft(uint32_t ac) {
    if (ac == 0) return NAN;
    if ((ac >> 4) & 1) {
        uint32_t n = ((ac & 0x0FE0) >> 1) | (ac & 0x000F);
        return static_cast<float>(static_cast<int>(n) * 25 - 1000);
    }
    // re-insert M = 0 and decode as AC13
    uint32_t ac13 = ((ac & 0x0FC0) << 1) | (ac & 0x003F);
    return ac13_altitude_ft(ac13);
}

// ID13: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4
int squawk(uint32_t id) {
    auto b = [&](int pos) { return static_cast<int>((id >> (12 - pos)) & 1); };
    int a = b(5) * 4 + b(3) * 2 + b(1);
    int bb = b(11) * 4 + b(9) * 2 + b(7);
    int c = b(4) * 4 + b(2) * 2 + b(0);
    int d = b(12) * 4 + b(10) * 2 + b(8);
    return a * 1000 + bb * 100 + c * 10 + d;
}

std::string callsign(const uint8_t* me) {
    static const char* cs = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    std::string s;
    for (int i = 0; i < 8; i++) s += cs[bits(me, 9 + i * 6, 14 + i * 6)];
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

float surface_speed_kt(int mov) {
    if (mov <= 0 || mov > 124) return NAN;
    if (mov == 1) return 0;
    if (mov == 124) return 175;
    static const int movs[] = {2, 9, 13, 39, 94, 109, 124};
    static const float kts[] = {0.125f, 1, 2, 15, 70, 100, 175};
    int i = 1;
    while (movs[i] <= mov) i++;
    float step = (kts[i] - kts[i - 1]) / static_cast<float>(movs[i] - movs[i - 1]);
    return kts[i - 1] + static_cast<float>(mov - movs[i - 1]) * step;
}

int cpr_nl(double lat) {
    lat = std::fabs(lat);
    if (lat < 1e-9) return 59;
    if (lat > 87.0) return 1;
    if (lat == 87.0) return 2;
    const double a = 1.0 - std::cos(M_PI / 30.0);   // 1 - cos(pi / (2 NZ)), NZ = 15
    const double c = std::cos(M_PI / 180.0 * lat);
    return static_cast<int>(std::floor(2.0 * M_PI / std::acos(1.0 - a / (c * c))));
}

bool cpr_global(int lat_e, int lon_e, int lat_o, int lon_o, bool odd_latest, bool surface, bool have_ref,
                double ref_lat, double ref_lon, double* lat, double* lon) {
    if (surface && !have_ref) return false;
    const double span = surface ? 90.0 : 360.0;
    const double fle = lat_e / 131072.0, flo = lat_o / 131072.0;
    const double fne = lon_e / 131072.0, fno = lon_o / 131072.0;
    const int j = static_cast<int>(std::floor(59.0 * fle - 60.0 * flo + 0.5));
    double rle = span / 60.0 * (cpr_mod(j, 60) + fle);
    double rlo = span / 59.0 * (cpr_mod(j, 59) + flo);
    if (surface) {
        // the solution is in [0, 90): the one nearest the reference
        auto pick = [&](double r) {
            double best = r, bd = 1e9;
            for (double c : {r, r - 90.0}) {
                if (std::fabs(c - ref_lat) < bd) { bd = std::fabs(c - ref_lat); best = c; }
            }
            return best;
        };
        rle = pick(rle);
        rlo = pick(rlo);
    } else {
        if (rle >= 270.0) rle -= 360.0;
        if (rlo >= 270.0) rlo -= 360.0;
    }
    if (rle < -90 || rle > 90 || rlo < -90 || rlo > 90) return false;
    if (cpr_nl(rle) != cpr_nl(rlo)) return false;   // the pair straddles a zone boundary
    const double rlat = odd_latest ? rlo : rle;
    const int nl = cpr_nl(rlat);
    const int ni = std::max(nl - (odd_latest ? 1 : 0), 1);
    const int m = static_cast<int>(std::floor(fne * (nl - 1) - fno * nl + 0.5));
    double rlon = span / ni * (cpr_mod(m, ni) + (odd_latest ? fno : fne));
    if (surface) {
        double best = 0, bd = 1e9;
        for (int k = 0; k < 4; k++) {
            double c = wrap180(rlon + 90.0 * k);
            double d = std::fabs(wrap180(c - ref_lon));
            if (d < bd) { bd = d; best = c; }
        }
        rlon = best;
    } else {
        rlon = wrap180(rlon);
    }
    *lat = rlat;
    *lon = rlon;
    return true;
}

bool cpr_local(int cpr_lat, int cpr_lon, bool odd, bool surface, double ref_lat, double ref_lon, double* lat,
               double* lon) {
    const double span = surface ? 90.0 : 360.0;
    const double fl = cpr_lat / 131072.0, fo = cpr_lon / 131072.0;
    const double dlat = span / (odd ? 59.0 : 60.0);
    const double j = std::floor(ref_lat / dlat) + std::floor(0.5 + cpr_mod(ref_lat, dlat) / dlat - fl);
    const double rlat = dlat * (j + fl);
    if (rlat < -90 || rlat > 90 || std::fabs(rlat - ref_lat) > dlat / 2) return false;
    const int ni = std::max(cpr_nl(rlat) - (odd ? 1 : 0), 1);
    const double dlon = span / ni;
    const double m = std::floor(ref_lon / dlon) + std::floor(0.5 + cpr_mod(ref_lon, dlon) / dlon - fo);
    const double rlon = wrap180(dlon * (m + fo));
    if (std::fabs(wrap180(rlon - ref_lon)) > dlon / 2) return false;
    *lat = rlat;
    *lon = rlon;
    return true;
}

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double r = M_PI / 180.0;
    double dlat = (lat2 - lat1) * r, dlon = (lon2 - lon1) * r;
    double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
               std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

}  // namespace modes
