#pragma once

// Radiosonde plugin: what every family decoder hands to the plugin (one
// Record per frame that passed its checks), the clock that turns sample
// positions into Host::time(), and the conversions the families share
// (time, WGS-84 geodesy, standard atmosphere, humidity, CRCs).

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonde {

constexpr double FS = 48000;   // the plugin's input rate

// One decoded frame. Absent: numbers NAN, text empty, counters negative.
struct Record {
    std::string family;          // "RS41", "DFM", "M10", "M20", "iMet-4", "iMet-54", "LMS6", "MRZ"
    std::string model;           // "RS41-SG", "DFM-09" ... ("" = not known yet)
    std::string serial;          // "" = not known yet: the frame is dropped
    long long frame = -1;        // frame counter
    double utc = NAN;            // seconds since 1970 (UTC)
    double tod = NAN;            // time of day, seconds UTC (when no date is known)
    double lat = NAN, lon = NAN, alt = NAN;   // degrees, metres
    double vh = NAN;             // horizontal speed, m/s
    double hdg = NAN;            // direction of travel, degrees true
    double vv = NAN;             // vertical speed, m/s (+ up)
    int sats = -1;
    double T = NAN;              // air temperature, degC
    double RH = NAN;             // relative humidity, %
    double P = NAN;              // measured pressure, hPa
    double TU = NAN;             // humidity-sensor temperature, degC
    double Ti = NAN;             // internal temperature, degC
    double batt = NAN;           // V
    long long countdown = -1;    // burst / kill countdown, s
    double txf = NAN;            // configured TX frequency, MHz
    std::vector<std::pair<std::string, std::string>> extras;
    double offset = NAN;         // carrier offset, Hz (+ = above the VFO centre)
    double t_first = NAN, t_end = NAN;   // Host::time() of the frame's first sample / its end
    std::string hex;             // the frame for the decoder data log (only while raw is wanted)
};

// Sample positions (discriminator sample index since the decoder's reset)
// -> Host::time(): the block that is being processed started at idx0 / t0
struct Clock {
    double t0 = 0;
    int64_t idx0 = 0;
    double at(double idx) const { return t0 + (idx - static_cast<double>(idx0)) / FS; }
};

// What a family's process() gets besides its samples
struct Ctx {
    Clock clk;
    bool raw = false;            // build Record::hex
};

// --- time ---------------------------------------------------------------------
constexpr double GPS_EPOCH_UNIX = 315964800;   // 1980-01-06 00:00:00 UTC
constexpr double GPS_UTC_LEAP_S = 18;          // GPS - UTC since 2017-01-01 (IERS Bulletin C)
double gps_to_unix(double week, double tow_s, double leap_s = GPS_UTC_LEAP_S);
// days since 1970-01-01 of a proleptic Gregorian date
int64_t days_from_civil(int y, int m, int d);
double unix_time(int y, int mo, int d, int h, int mi, double s);

// --- geodesy (WGS-84) ----------------------------------------------------------
void ecef_to_geodetic(double x, double y, double z, double* lat_deg, double* lon_deg, double* h);
void geodetic_to_ecef(double lat_deg, double lon_deg, double h, double* x, double* y, double* z);
// ECEF velocity -> local east / north / up at lat / lon (degrees)
void ecef_to_enu_velocity(double lat_deg, double lon_deg, double vx, double vy, double vz, double* e, double* n, double* u);
// speed (m/s) and direction of travel (degrees true, 0..360) from east / north
void speed_dir(double e, double n, double* speed, double* dir);
// great-circle distance (km, R 6371 km) and initial bearing (degrees)
double distance_km(double lat1, double lon1, double lat2, double lon2);
double bearing_deg(double lat1, double lon1, double lat2, double lon2);

// --- atmosphere / humidity -------------------------------------------------------
double isa_pressure_hpa(double h_m);            // ICAO standard atmosphere
double dew_point(double t_c, double rh);        // Magnus (WMO), NAN if rh <= 0
double svp_hyland_wexler(double t_k);           // saturation vapour pressure over water, Pa

// --- checks ------------------------------------------------------------------------
uint16_t crc16_ccitt_false(const uint8_t* p, size_t n);   // poly 0x1021, init 0xFFFF
uint16_t crc16_aug_ccitt(const uint8_t* p, size_t n);     // poly 0x1021, init 0x1D0F
uint16_t crc16_xmodem(const uint8_t* p, size_t n);        // poly 0x1021, init 0
uint16_t crc16_modbus(const uint8_t* p, size_t n);        // poly 0x8005 reflected, init 0xFFFF
uint32_t crc32_mpeg2(const uint8_t* p, size_t n);         // poly 0x04C11DB7, init 0xFFFFFFFF, no reflection
uint16_t meteomodem_check(const uint8_t* p, size_t n);    // M10 / M20 frame check

// --- little helpers ------------------------------------------------------------------
inline uint16_t u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t u24le(const uint8_t* p) { return static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16); }
inline uint32_t u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline uint16_t u16be(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t u24be(const uint8_t* p) { return static_cast<uint32_t>(p[0] << 16 | p[1] << 8 | p[2]); }
inline uint32_t u32be(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
           static_cast<uint32_t>(p[3]);
}
inline int32_t i24(uint32_t v) { return static_cast<int32_t>(v << 8) >> 8; }
float f32le(const uint8_t* p);
std::string hex_upper(const uint8_t* p, size_t n);

}  // namespace sonde
