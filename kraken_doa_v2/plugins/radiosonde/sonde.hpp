#pragma once

// Shared definitions of the radiosonde plugin: what one decoded frame
// carries (Frame), the interface every sonde type implements (Type), and
// small helpers (geodesy, GPS time, meteorology, byte access, CRC).

#include "fsk.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace sonde {

// One frame that passed its checks. NAN / "" / -1 = not in this frame (the
// plugin keeps the last known value per sonde).
struct Frame {
    std::string type;                 // family shown to the user: "RS41", "DFM", "M10", ...
    std::string subtype;              // "RS41-SGP", "DFM-17", "" = unknown yet
    std::string serial;               // "" = not known yet (some types need several frames)
    long frame_no = -1;
    double utc = NAN;                 // seconds since 1970 (UTC), NAN = no date / time
    double tod = NAN;                 // time of day (s, UTC) when only that is known
    double lat = NAN, lon = NAN;
    double alt = NAN;                 // m (GPS: above the ellipsoid / MSL as the sonde sends it)
    double vh = NAN;                  // horizontal speed m/s
    double heading = NAN;             // direction of travel, degrees true
    double vv = NAN;                  // vertical speed m/s (+ = up)
    int sats = -1;
    double temp = NAN;                // air temperature C
    double rh = NAN;                  // relative humidity %
    double pressure = NAN;            // measured pressure hPa
    double temp_rh = NAN;             // humidity sensor temperature C
    double temp_int = NAN;            // internal / board temperature C
    double batt = NAN;                // battery V
    long burst_timer = -1;            // s until the burst / kill timer fires (-1 = off / unknown)
    double tx_mhz = NAN;              // frequency the sonde is configured for
    std::vector<std::pair<std::string, std::string>> extra;   // more "key: value" for the popup / facts
    float dc_hz = NAN;                // carrier offset measured on the frame (AFC)
    double t = 0;                     // input time (s) of the frame
    double t_start = NAN, t_end = NAN; // input time (s) of its first sample / end, NAN = not known (Host::valid(start, end))
    std::string raw;                  // the frame in hex (decoder data log)
};

// A sonde type: owns its demodulator(s) and decoder state. The plugin feeds
// it the FM discriminator (Hz, at the plugin's rate) - narrow and wide
// channel versions - and gets every frame that passed its checks.
class Type {
public:
    virtual ~Type() = default;
    virtual void push(const float* narrow, const float* wide, size_t n, double t0) = 0;
    virtual void reset() = 0;
    std::function<void(Frame&)> out;
    bool want_raw = false;
};

// --- bytes / bits ------------------------------------------------------------------
inline uint16_t u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t u24le(const uint8_t* p) { return p[0] | p[1] << 8 | static_cast<uint32_t>(p[2]) << 16; }
inline uint32_t u32le(const uint8_t* p) { return u24le(p) | static_cast<uint32_t>(p[3]) << 24; }
inline int16_t i16le(const uint8_t* p) { return static_cast<int16_t>(u16le(p)); }
inline int32_t i32le(const uint8_t* p) { return static_cast<int32_t>(u32le(p)); }
inline uint16_t u16be(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t u24be(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 16 | p[1] << 8 | p[2]; }
inline uint32_t u32be(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | u24be(p + 1); }
inline int16_t i16be(const uint8_t* p) { return static_cast<int16_t>(u16be(p)); }
inline int32_t i32be(const uint8_t* p) { return static_cast<int32_t>(u32be(p)); }
inline float f32le(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB first) - RS41 / RS92 blocks
inline uint16_t crc16_ccitt(const uint8_t* d, size_t n, uint16_t crc = 0xFFFF) {
    for (size_t i = 0; i < n; i++) {
        crc ^= static_cast<uint16_t>(d[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

inline std::string hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789ABCDEF";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = H[d[i] >> 4]; s[2 * i + 1] = H[d[i] & 15]; }
    return s;
}

// --- geodesy ---------------------------------------------------------------------------
// ECEF (m) -> WGS84 latitude, longitude (deg), height above the ellipsoid (m)
void ecef_to_geo(const double xyz[3], double* lat, double* lon, double* alt);
// ECEF velocity (m/s) at lat/lon -> east, north, up
void ecef_vel_to_enu(const double v[3], double lat, double lon, double* ve, double* vn, double* vu);
// east/north (m/s) -> speed, direction of travel (deg)
inline void en_to_speed_dir(double ve, double vn, double* vh, double* dir) {
    *vh = std::hypot(ve, vn);
    double d = std::atan2(ve, vn) * 180 / M_PI;
    *dir = d < 0 ? d + 360 : d;
}

// --- time ---------------------------------------------------------------------------------
// seconds since 1970 of a civil date (UTC)
double utc_seconds(int y, int mo, int d, int h, int mi, double s);
// GPS week + seconds of week -> UTC seconds since 1970 (leap seconds subtracted)
double gps_to_utc(int week, double tow);
constexpr int GPS_LEAP_S = 18;   // GPS - UTC since 2017-01-01

// --- meteorology ----------------------------------------------------------------------
// saturation vapour pressure over water (hPa), Magnus / Sonntag-type fit
inline double svp_hpa(double t_c) { return 6.112 * std::exp(17.62 * t_c / (243.12 + t_c)); }
// saturation vapour pressure over water (Pa), Hyland & Wexler - the sondes'
// humidity sensors are scaled from their own temperature to the air's with it
inline double svp_hw(double t_c) {
    const double t = t_c + 273.15;
    return std::exp(-5800.2206 / t + 1.3914993 + 6.5459673 * std::log(t) - 4.8640239e-2 * t + 4.1764768e-5 * t * t -
                    1.4452093e-8 * t * t * t);
}
// dew point (C) from temperature (C) and relative humidity (%)
inline double dew_point(double t_c, double rh) {
    if (!(rh > 0)) return NAN;
    const double g = std::log(rh / 100) + 17.62 * t_c / (243.12 + t_c);
    return 243.12 * g / (17.62 - g);
}
// pressure (hPa) of the ICAO standard atmosphere at a geopotential height (m)
double isa_pressure(double h);

}  // namespace sonde
