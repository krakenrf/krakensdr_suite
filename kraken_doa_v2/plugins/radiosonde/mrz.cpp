// Meteo-Radiy MRZ (MRZ-N1 / MP3-H1): 2-FSK Manchester, 2399 baud (chips
// "10" = 1, "01" = 0), bytes msb first. Frame: AA BF 35, a sub-frame counter,
// time of day (h m s), ECEF position (cm) + velocity (cm/s) and satellites -
// or, in the other layout (bytes 30..31 = FFFF), latitude / longitude
// (1e-6 deg), altitude (cm), speed and direction - then temperature and
// humidity (1/100), raw ADC values, a second counter and one 32-bit
// configuration word (the counter says which: NTC / ADC calibration, the
// serial in two parts (12, 13), the date (15, DDMMYY)); a CRC-16 (reflected
// 0xA001, init 0xFFFF) over counter..config.
//
// Protocol as documented by the open-source radiosonde community (rs1729/RS
// "mp3h1mod"); independent implementation, validated against the
// radiosonde_auto_rx test recording.

#include "sonde.hpp"

#include <memory>

namespace sonde {
namespace {

const char* HEADER = "100110011001100110011001100110011001" "10101010";
constexpr int NBYTES = 52;

uint16_t crc16_rev(const uint8_t* d, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0xA001) : static_cast<uint16_t>(c >> 1);
    }
    return c;
}

class Mrz : public Type {
public:
    explicit Mrz(double fs) : fs_(fs) {
        FskConfig c;
        c.baud = 2399;
        for (const char* p = HEADER; *p; p++) c.sync.push_back(*p == '1');
        c.max_sync_err = 3;
        c.frame_syms = 800;
        rx_.init(c, fs);
        rx_.on_frame = [this](const FskFrame& f) { frame(f); };
        reset();
    }
    void push(const float* narrow, const float*, size_t n, double t0) override {
        rx_.clock(t0);
        for (size_t i = 0; i < n; i++) rx_.push(narrow[i]);
    }
    void reset() override {
        rx_.reset();
        last_t0_ = -1e12;
        snc_ = snd_ = 0;
        y_ = mo_ = d_ = 0;
    }

private:
    void frame(const FskFrame& f);
    FskRx rx_;
    double fs_, last_t0_ = -1e12;
    uint32_t snc_ = 0, snd_ = 0;
    int y_ = 0, mo_ = 0, d_ = 0;
};

void Mrz::frame(const FskFrame& f) {
    if (std::fabs(f.t0 - last_t0_) < 0.3 * fs_) return;
    // bits: the header's 22 (Manchester of its chips), then the frame's
    uint8_t bits[8 * (NBYTES + 2)] = {0};
    int nb = 0;
    for (int i = 0; HEADER[2 * i]; i++) bits[nb++] = HEADER[2 * i] == '1';
    for (int i = 0; nb < static_cast<int>(sizeof bits) && 2 * i + 1 < f.n; i++) bits[nb++] = f.soft[2 * i] - f.soft[2 * i + 1] > 0;
    uint8_t b[NBYTES] = {0};
    for (int i = 0; i < NBYTES; i++)
        for (int k = 0; k < 8; k++) b[i] = static_cast<uint8_t>(b[i] << 1 | bits[8 + 8 * i + k]);
    if (b[0] != 0xAA || b[1] != 0xBF || b[2] != 0x35) return;
    const bool latlon = u16le(b + 30) == 0xFFFF;
    const int crclen = latlon ? 42 : 45, ofs = latlon ? -3 : 0;
    if (crc16_rev(b + 3, crclen) != u16le(b + crclen + 3)) return;
    last_t0_ = f.t0;

    // configuration word of this sub-frame
    const int sub = b[3] & 0xF;
    const uint32_t cfg = u32le(b + 44 + ofs);
    if (sub == 0xC) { if (snc_ && cfg != snc_) snd_ = 0; snc_ = cfg; }
    if (sub == 0xD) { if (snd_ && cfg != snd_) snc_ = 0; snd_ = cfg; }
    if (sub == 0xF) { y_ = 2000 + cfg % 100; mo_ = cfg / 100 % 100; d_ = cfg / 10000 % 100; }
    if (!snc_ || !snd_) return;   // the serial isn't complete yet

    Frame o;
    o.type = "MRZ";
    o.subtype = "MRZ";
    o.dc_hz = f.dc_hz;
    o.t_start = f.t_start;
    o.t_end = f.t_end;
    o.serial = std::to_string(snc_) + "-" + std::to_string(snd_);
    const int h = b[4], mi = b[5], s = b[6];
    if (h < 24 && mi < 60 && s < 61) {
        if (y_ > 2000 && mo_ >= 1 && mo_ <= 12 && d_ >= 1) o.utc = utc_seconds(y_, mo_, d_, h, mi, s);
        else o.tod = h * 3600 + mi * 60 + s;
    }
    if (latlon) {
        const double lat = i32le(b + 7) * 1e-6, lon = i32le(b + 11) * 1e-6, alt = i32le(b + 15) * 1e-2;
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -1000 && alt < 80000) {
            o.lat = lat; o.lon = lon; o.alt = alt;
            o.vh = i16le(b + 19) / 100.0;
            o.heading = u16le(b + 21) / 100.0;
        }
        o.sats = b[23];
    } else {
        const double xyz[3] = {i32le(b + 8) / 100.0, i32le(b + 12) / 100.0, i32le(b + 16) / 100.0};
        double lat, lon, alt;
        ecef_to_geo(xyz, &lat, &lon, &alt);
        if (alt > -1000 && alt < 80000 && (xyz[0] || xyz[1] || xyz[2])) {
            o.lat = lat; o.lon = lon; o.alt = alt;
            const double v[3] = {i16le(b + 20) / 100.0, i16le(b + 22) / 100.0, i16le(b + 24) / 100.0};
            double ve, vn, vu;
            ecef_vel_to_enu(v, lat, lon, &ve, &vn, &vu);
            en_to_speed_dir(ve, vn, &o.vh, &o.heading);
            o.vv = vu;
        }
        o.sats = b[26];
    }
    const double t = i16le(b + 29 + ofs) / 100.0, rh = i16le(b + 31 + ofs) / 100.0;
    if (t > -120 && t < 80) o.temp = t;
    if (rh >= 0 && rh <= 100) o.rh = rh;
    if (want_raw) o.raw = hex(b, crclen + 5);
    if (out) out(o);
}

}  // namespace

std::unique_ptr<Type> make_mrz(double fs) { return std::make_unique<Mrz>(fs); }

}  // namespace sonde
