// Meteomodem M10 and M20: 2-FSK Manchester, 9615 (M10) / 9600 (M20) baud,
// differentially coded (a data bit is 1 when the Manchester bit equals the
// one before - so the polarity doesn't matter), bytes msb first, big-endian
// fields. Frame: length byte (0x64 M10, 0x45 M20), type byte (0x9F M10,
// 0xAF M10+, 0x8F M2K2, 0x20 M20), data, a 16-bit check (a shift-and-xor
// checksum) - M20 has a second one over its first 22 bytes.
//   M10: GPS velocity (1/200 m/s), time of week (ms), lat / lon (2^32 per
//        360 deg), altitude (mm), satellites, leap seconds, week; NTC
//        temperature (three ranges), capacitive humidity timed against a
//        55 %RH reference, battery
//   M20: altitude (cm, 24 bit), velocity (cm/s), time of week (s), lat / lon
//        (1e-6 deg), week; temperature, humidity (calibrated), humidity sensor
//        temperature, pressure (newer firmware), battery
//
// Protocol as documented by the open-source radiosonde community (rs1729/RS
// "m10mod", "mXXmod", DF9DQ); independent implementation, validated against
// the radiosonde_auto_rx test recordings.

#include "sonde.hpp"

#include <cstdio>
#include <memory>

namespace sonde {
namespace {

const char* RAWHEAD = "10011001100110010100110010011001";
constexpr int MAX_BYTES = 0x64 + 1 + 20;

uint16_t check_step(uint16_t c, uint8_t b) {
    const int c1 = c & 0xFF;
    b = static_cast<uint8_t>((b >> 1) | ((b & 1) << 7));
    b ^= (b >> 2) & 0xFF;
    const int t6 = (c & 1) ^ ((c >> 2) & 1) ^ ((c >> 4) & 1);
    const int t7 = ((c >> 1) & 1) ^ ((c >> 3) & 1) ^ ((c >> 5) & 1);
    const int t = (c & 0x3F) | (t6 << 6) | (t7 << 7);
    int s = (c >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;
    return static_cast<uint16_t>(((c1 << 8) | ((b ^ t ^ s) & 0xFF)) & 0xFFFF);
}
uint16_t checksum(const uint8_t* d, int n, uint16_t c = 0) {
    for (int i = 0; i < n; i++) c = check_step(c, d[i]);
    return c;
}

// NTC thermistor of both: Steinhart-Hart fit, three ranges of series / parallel resistors
double ntc_temp(int scale, double adc) {
    static const double rs[3] = {12.1e3, 36.5e3, 475.0e3}, rp[3] = {1e20, 330.0e3, 2000.0e3};
    if (scale < 0 || scale > 2 || adc <= 0) return NAN;
    const double x = (4095.0 - adc) / adc;
    const double r = rs[scale] / (x - rs[scale] / rp[scale]);
    if (!(r > 0)) return NAN;
    const double l = std::log(r);
    const double t = 1 / (1.07303516e-03 + 2.41296733e-04 * l + 2.26744154e-06 * l * l + 6.52855181e-08 * l * l * l) - 273.15;
    return (t > -120 && t < 60) ? t : NAN;
}

class M10 : public Type {
public:
    explicit M10(double fs) : fs_(fs) {
        FskConfig c;
        for (const char* p = RAWHEAD; *p; p++) c.sync.push_back(*p == '1');
        c.max_sync_err = 2;
        c.frame_syms = 2 * 8 * MAX_BYTES;
        c.nph = 6;
        c.baud = 9615;   // M10
        rx_[0].init(c, fs);
        c.baud = 9600;   // M20
        rx_[1].init(c, fs);
        for (auto& r : rx_) r.on_frame = [this](const FskFrame& f) { frame(f); };
        reset();
    }
    void push(const float*, const float* wide, size_t n, double t0) override {
        rx_[0].clock(t0);
        rx_[1].clock(t0);
        for (size_t i = 0; i < n; i++) {
            rx_[0].push(wide[i]);
            rx_[1].push(wide[i]);
        }
    }
    void reset() override {
        rx_[0].reset();
        rx_[1].reset();
        last_t0_ = -1e12;
    }

private:
    void frame(const FskFrame& f);
    void m10(const uint8_t* b, int len, Frame& o);
    void m20(const uint8_t* b, int len, Frame& o);

    FskRx rx_[2];
    double fs_, last_t0_ = -1e12;
};

void M10::frame(const FskFrame& f) {
    if (std::fabs(f.t0 - last_t0_) < 0.3 * fs_) return;   // decoded already (other phase / rate)
    uint8_t b[MAX_BYTES] = {0};
    int prev = -1;
    for (int i = 0; i < 8 * MAX_BYTES; i++) {
        const int bit = f.soft[2 * i + 1] - f.soft[2 * i] >= 0;
        const int d = prev < 0 ? 0 : (bit == prev);
        prev = bit;
        b[i / 8] |= d << (7 - i % 8);
    }
    const int len = b[0];
    Frame o;
    o.dc_hz = f.dc_hz;
    o.t_start = f.t_start;
    o.t_end = f.t_end;
    if (b[1] == 0x20 && len >= 0x43 && len <= 0x45 + 20) {
        // M20: the check over the whole frame (newer firmware reuses the
        // block check's byte 0x16 for the pressure)
        if (checksum(b, len - 1) != u16be(b + len - 1)) return;
        m20(b, len, o);
    } else if ((b[1] == 0x9F || b[1] == 0xAF || b[1] == 0x8F) && len >= 0x64 && len <= 0x64 + 20) {
        if (checksum(b, len - 1) != u16be(b + len - 1)) return;
        m10(b, len, o);
    } else {
        return;
    }
    last_t0_ = f.t0;
    if (want_raw) o.raw = hex(b, len + 1);
    if (out) out(o);
}

void M10::m10(const uint8_t* b, int, Frame& o) {
    o.type = "M10";
    o.subtype = b[1] == 0xAF ? "M10+" : b[1] == 0x8F ? "M2K2" : "M10";
    // serial: "803-2-10732" (hex digit + 2 decimal, digit, digit + 4 decimal)
    const uint8_t* s = b + 0x5D;
    const unsigned w = s[3] | s[4] << 8;
    char sn[24];
    snprintf(sn, sizeof sn, "%X%02u-%X-%u%04u", s[2] >> 4, s[2] & 0xF, s[0] & 0xF, (w >> 13) & 7, w & 0x1FFF);
    o.serial = sn;
    int week = u16be(b + 0x20);
    if (week < 1304) week += 1024;
    const double tow = u32be(b + 0x0A) / 1000.0;
    const int leap = b[0x1F] <= 30 ? b[0x1F] : GPS_LEAP_S;
    if (week > 1304 && week < 4000 && tow < 604800) o.utc = gps_to_utc(week, tow) + GPS_LEAP_S - leap;
    o.sats = b[0x1E];
    const double lat = i32be(b + 0x0E) * 90.0 / (1 << 30), lon = i32be(b + 0x12) * 90.0 / (1 << 30);
    const double alt = i32be(b + 0x16) / 1000.0;
    // no fix: the M10 sends 90, 0 / its last fix with 0 satellites
    if (o.sats > 0 && std::fabs(lat) < 89.999 && std::fabs(lon) <= 180) {
        o.lat = lat; o.lon = lon; o.alt = alt;
        en_to_speed_dir(i16be(b + 0x04) / 200.0, i16be(b + 0x06) / 200.0, &o.vh, &o.heading);
        o.vv = i16be(b + 0x08) / 200.0;
    }
    o.temp = ntc_temp(b[0x3E], static_cast<double>(u16le(b + 0x3F)) - 0xA000);
    // humidity: capacitor timed against a 55 %RH reference (linear sensor), then
    // a temperature correction
    const double c55 = u24le(b + 0x32) / 1000.0, crh = u24le(b + 0x35) / 1000.0;
    if (c55 > 0 && std::isfinite(o.temp)) {
        double rh = (crh / c55 - 0.8955) / 0.002;
        if (o.temp < 0) rh += -o.temp / 5.5;
        if (o.temp < -30) rh *= 1 + (-30 - o.temp) / 75;
        if (rh > -20 && rh < 120) {
            o.rh = std::max(0.0, std::min(100.0, rh));
            o.extra.push_back({"Humidity model", "approximate (M10)"});
        }
    }
    o.batt = 2.709 * u16le(b + 0x45) * 2.5 / 1023.0;
}

void M10::m20(const uint8_t* b, int len, Frame& o) {
    o.type = "M20";
    o.subtype = "M20";
    const uint32_t sn = b[0x12] | b[0x13] << 8 | static_cast<uint32_t>(b[0x14]) << 16;
    char s[24];
    const unsigned ym = sn & 0x7F;
    snprintf(s, sizeof s, "%u%02u-%u-%u%04u", ym / 12, ym % 12 + 1, ((sn >> 7) & 7) + 1, (sn >> 23) & 1, (sn >> 10) & 0x1FFF);
    o.serial = sn ? s : "";
    if (!sn) return;
    int week = u16be(b + 0x1A);
    if (week < 1304) week += 1024;
    const double tow = u24be(b + 0x0F);
    if (week > 1304 && week < 4000 && tow < 604800) o.utc = gps_to_utc(week, tow);
    const double lat = i32be(b + 0x1C) / 1e6, lon = i32be(b + 0x20) / 1e6;
    if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0)) {
        o.lat = lat; o.lon = lon;
        o.alt = u24be(b + 0x08) / 100.0;
        en_to_speed_dir(i16be(b + 0x0B) / 100.0, i16be(b + 0x0D) / 100.0, &o.vh, &o.heading);
        o.vv = i16be(b + 0x18) / 100.0;
    }
    // temperature: 14-bit ADC whose top bits select the range
    int adc = u16le(b + 0x04), scale = 0;
    if (adc > 8191) { scale = 2; adc -= 8192; } else if (adc > 4095) { scale = 1; adc -= 4096; }
    o.temp = ntc_temp(scale, adc);
    // humidity sensor temperature (second NTC), then the calibrated humidity
    const double a2 = u16le(b + 0x06);
    if (a2 > 0 && a2 < 4095) {
        const double r = 22.1e3 / ((4095.0 - a2) / a2);
        if (r > 0) o.temp_rh = 1 / (1 / 298.15 + 1 / 3650.0 * std::log(r / 2.2e3)) - 273.15;
    }
    const unsigned hum = u16le(b + 0x02), cal = u16le(b + 0x2F);
    if (hum < 48000 && std::isfinite(o.temp_rh)) {
        double x = (hum + 80000.0) * (6.4e8 / (cal + 80000.0)) * (1 - 5.8e-4 * (o.temp_rh - 25));
        x = 4.16e9 / x;
        x = 10.087 * x * x * x - 211.62 * x * x + 1388.2 * x - 2797.0;
        if (x > -20 && x < 120) o.rh = std::max(0.0, std::min(100.0, x));
    }
    // pressure (firmware 7+ adds a low byte)
    const int fw = len >= 0x45 ? b[0x43] : b[len - 2];
    const uint32_t pv = static_cast<uint32_t>(u16le(b + 0x24)) << 8 | (fw >= 7 && fw <= 0x20 ? b[0x16] : 0);
    if (pv > 0 && pv / 4096.0 < 1100) o.pressure = pv / 4096.0;
    o.batt = b[0x26] * 3.3 / 255;
}

}  // namespace

std::unique_ptr<Type> make_m10(double fs) { return std::make_unique<M10>(fs); }

}  // namespace sonde
