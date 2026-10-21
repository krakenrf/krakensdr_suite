// Conversions shared by the radiosonde families: GPS / calendar time, WGS-84
// geodesy, the ICAO standard atmosphere, humidity helpers and the CRCs.

#include "sonde.hpp"

#include <cstring>

namespace sonde {

namespace {
constexpr double D2R = M_PI / 180;
// WGS-84
constexpr double WGS_A = 6378137.0;
constexpr double WGS_B = 6356752.314245;
constexpr double WGS_E2 = 1 - (WGS_B * WGS_B) / (WGS_A * WGS_A);     // first eccentricity squared
constexpr double WGS_EP2 = (WGS_A * WGS_A) / (WGS_B * WGS_B) - 1;    // second eccentricity squared

// MSB-first 16-bit CRC with polynomial 0x1021
uint16_t crc1021(uint16_t c, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>(c << 1 ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}
}  // namespace

// --- time ---------------------------------------------------------------------
double gps_to_unix(double week, double tow_s, double leap_s) { return GPS_EPOCH_UNIX + week * 604800.0 + tow_s - leap_s; }

int64_t days_from_civil(int y, int m, int d) {
    // proleptic Gregorian, March-based years (any correct method will do)
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double unix_time(int y, int mo, int d, int h, int mi, double s) {
    return static_cast<double>(days_from_civil(y, mo, d)) * 86400.0 + h * 3600.0 + mi * 60.0 + s;
}

// --- geodesy -------------------------------------------------------------------
void ecef_to_geodetic(double x, double y, double z, double* lat_deg, double* lon_deg, double* h) {
    const double p = std::hypot(x, y);
    const double lon = std::atan2(y, x);
    // Bowring's closed form as the start, then two fixed-point steps with the
    // height (mm-exact at any sonde height)
    const double th = std::atan2(z * WGS_A, p * WGS_B);
    const double st = std::sin(th), ct = std::cos(th);
    double lat = std::atan2(z + WGS_EP2 * WGS_B * st * st * st, p - WGS_E2 * WGS_A * ct * ct * ct);
    double hh = 0;
    for (int it = 0; it < 3; it++) {
        const double sl = std::sin(lat);
        const double N = WGS_A / std::sqrt(1 - WGS_E2 * sl * sl);
        const double cl = std::cos(lat);
        hh = std::fabs(cl) > 1e-9 ? p / cl - N : std::fabs(z) - WGS_B;
        lat = std::atan2(z, p * (1 - WGS_E2 * N / (N + hh)));
    }
    *lat_deg = lat / D2R;
    *lon_deg = lon / D2R;
    *h = hh;
}

void geodetic_to_ecef(double lat_deg, double lon_deg, double h, double* x, double* y, double* z) {
    const double la = lat_deg * D2R, lo = lon_deg * D2R;
    const double sl = std::sin(la);
    const double N = WGS_A / std::sqrt(1 - WGS_E2 * sl * sl);
    *x = (N + h) * std::cos(la) * std::cos(lo);
    *y = (N + h) * std::cos(la) * std::sin(lo);
    *z = (N * (1 - WGS_E2) + h) * sl;
}

void ecef_to_enu_velocity(double lat_deg, double lon_deg, double vx, double vy, double vz, double* e, double* n, double* u) {
    const double la = lat_deg * D2R, lo = lon_deg * D2R;
    const double sla = std::sin(la), cla = std::cos(la), slo = std::sin(lo), clo = std::cos(lo);
    *e = -slo * vx + clo * vy;
    *n = -sla * clo * vx - sla * slo * vy + cla * vz;
    *u = cla * clo * vx + cla * slo * vy + sla * vz;
}

void speed_dir(double e, double n, double* speed, double* dir) {
    *speed = std::sqrt(e * e + n * n);
    double d = std::atan2(e, n) / D2R;
    if (d <= 0) d += 360;   // (-0 too: no "-0.0" degrees)
    if (d >= 360) d -= 360;
    *dir = d;
}

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double dla = (lat2 - lat1) * D2R, dlo = (lon2 - lon1) * D2R;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                     std::cos(lat1 * D2R) * std::cos(lat2 * D2R) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(y, x) / D2R;
    if (b < 0) b += 360;
    if (b >= 360) b -= 360;
    return b;
}

// --- atmosphere / humidity -----------------------------------------------------
double isa_pressure_hpa(double h) {
    // ICAO Doc 7488 / US Std Atmosphere 1976 below 51 km: base height, base
    // pressure, base temperature, lapse rate; above 51 km the 47 km layer goes on
    struct Layer { double hb, pb, tb, lr; };
    static const Layer L[] = {{0, 1013.25, 288.15, -0.0065},
                              {11000, 226.321, 216.65, 0.0},
                              {20000, 54.7489, 216.65, 0.001},
                              {32000, 8.68019, 228.65, 0.0028},
                              {47000, 1.10906, 270.65, 0.0}};
    if (!std::isfinite(h)) return NAN;
    const double k = 9.80665 * 0.0289644 / 8.31446;
    int i = 0;
    for (int j = 1; j < 5; j++)
        if (h >= L[j].hb) i = j;
    const Layer& l = L[i];
    if (l.lr == 0) return l.pb * std::exp(-k * (h - l.hb) / l.tb);
    return l.pb * std::pow(1 + l.lr * (h - l.hb) / l.tb, -k / l.lr);
}

double dew_point(double t, double rh) {
    if (!std::isfinite(t) || !std::isfinite(rh) || rh <= 0) return NAN;
    const double g = std::log(rh / 100) + 17.62 * t / (243.12 + t);
    return 243.12 * g / (17.62 - g);
}

double svp_hyland_wexler(double t) {
    return std::exp(-5800.2206 / t + 1.3914993 + 6.5459673 * std::log(t) - 0.048640239 * t + 4.1764768e-5 * t * t -
                    1.4452093e-8 * t * t * t);
}

// --- checks --------------------------------------------------------------------
uint16_t crc16_ccitt_false(const uint8_t* p, size_t n) { return crc1021(0xFFFF, p, n); }
uint16_t crc16_aug_ccitt(const uint8_t* p, size_t n) { return crc1021(0x1D0F, p, n); }
uint16_t crc16_xmodem(const uint8_t* p, size_t n) { return crc1021(0, p, n); }

uint16_t crc16_modbus(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>(c >> 1 ^ 0xA001) : static_cast<uint16_t>(c >> 1);
    }
    return c;
}

uint32_t crc32_mpeg2(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint32_t>(p[i]) << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? c << 1 ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

// The Meteomodem frame check (a 16-bit LFSR fed every byte rotated, see
// SPEC 7.1): one byte 0x64 gives 0x003E
uint16_t meteomodem_check(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned x = p[i];
        unsigned r = ((x >> 1) | (x << 7)) & 0xFF;   // rotate right by one
        r ^= r >> 2;
        const unsigned c0 = c & 1, c1 = c >> 1 & 1, c2 = c >> 2 & 1, c3 = c >> 3 & 1, c4 = c >> 4 & 1, c5 = c >> 5 & 1;
        unsigned lo = r ^ ((c >> 7) & 0xFF) ^ ((c >> 9) & 0x3F) ^ (c & 0x3F);
        lo ^= (c0 ^ c2 ^ c4) << 6;
        lo ^= (c1 ^ c3 ^ c5) << 7;
        c = static_cast<uint16_t>((c & 0xFF) << 8 | (lo & 0xFF));
    }
    return c;
}

float f32le(const uint8_t* p) {
    const uint32_t u = u32le(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string hex_upper(const uint8_t* p, size_t n) {
    static const char* H = "0123456789ABCDEF";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = H[p[i] >> 4];
        s[2 * i + 1] = H[p[i] & 15];
    }
    return s;
}

}  // namespace sonde
