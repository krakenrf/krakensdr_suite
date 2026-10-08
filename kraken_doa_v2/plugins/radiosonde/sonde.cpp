#include "sonde.hpp"

namespace sonde {

void ecef_to_geo(const double xyz[3], double* lat, double* lon, double* alt) {
    // WGS84, Bowring's formula (sub-mm at sonde heights)
    constexpr double a = 6378137.0, b = 6356752.314245;
    constexpr double e2 = (a * a - b * b) / (a * a), ep2 = (a * a - b * b) / (b * b);
    const double x = xyz[0], y = xyz[1], z = xyz[2];
    const double p = std::hypot(x, y);
    const double th = std::atan2(z * a, p * b);
    const double st = std::sin(th), ct = std::cos(th);
    const double phi = std::atan2(z + ep2 * b * st * st * st, p - e2 * a * ct * ct * ct);
    const double n = a / std::sqrt(1 - e2 * std::sin(phi) * std::sin(phi));
    *lat = phi * 180 / M_PI;
    *lon = std::atan2(y, x) * 180 / M_PI;
    *alt = p / std::cos(phi) - n;
}

void ecef_vel_to_enu(const double v[3], double lat, double lon, double* ve, double* vn, double* vu) {
    const double f = lat * M_PI / 180, l = lon * M_PI / 180;
    const double sf = std::sin(f), cf = std::cos(f), sl = std::sin(l), cl = std::cos(l);
    *ve = -sl * v[0] + cl * v[1];
    *vn = -sf * cl * v[0] - sf * sl * v[1] + cf * v[2];
    *vu = cf * cl * v[0] + cf * sl * v[1] + sf * v[2];
}

double utc_seconds(int y, int mo, int d, int h, int mi, double s) {
    // days from civil (Howard Hinnant's algorithm)
    y -= mo <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097 + static_cast<long>(doe) - 719468;
    return days * 86400.0 + h * 3600.0 + mi * 60.0 + s;
}

double gps_to_utc(int week, double tow) {
    return 315964800.0 + week * 604800.0 + tow - GPS_LEAP_S;   // GPS epoch 1980-01-06
}

double isa_pressure(double h) {
    // ICAO standard atmosphere layers: base height, base pressure, base temperature, lapse rate
    struct L { double h, p, t, lapse; };
    static const L layers[] = {{0, 1013.25, 288.15, -0.0065},  {11000, 226.321, 216.65, 0.0},
                               {20000, 54.7489, 216.65, 0.001}, {32000, 8.68019, 228.65, 0.0028},
                               {47000, 1.10906, 270.65, 0.0}};
    constexpr double gmr = 9.80665 * 0.0289644 / 8.31446;
    const L* l = &layers[0];
    for (const L& x : layers)
        if (h >= x.h) l = &x;
    if (l->lapse == 0) return l->p * std::exp(-gmr * (h - l->h) / l->t);
    return l->p * std::pow(1 + l->lapse * (h - l->h) / l->t, -gmr / l->lapse);
}

}  // namespace sonde
