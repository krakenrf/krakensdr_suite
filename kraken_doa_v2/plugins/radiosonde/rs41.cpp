// Vaisala RS41 (RS41-SG / -SGP / -SGM): GFSK 4800 baud, NRZ, bytes LSB
// first, whitened with a 64-byte XOR mask. A frame is 320 bytes (518 with
// auxiliary data, e.g. an ozone sonde): 8-byte header, 48 bytes of
// Reed-Solomon parity (two RS(255,231) codewords over the even / odd bytes
// from offset 56), a frame-type byte, then blocks of id, length, data and a
// CRC-16/CCITT (little endian):
//   0x79 status: frame number, serial, battery, one 16-byte fragment of the
//        calibration table (51 fragments cycle, one per frame)
//   0x7A measurements: 12 x 24-bit sensor frequencies (temperature, humidity,
//        humidity-sensor temperature, pressure: each with two references)
//   0x7C GPS week / time of week, 0x7D raw GPS, 0x7B ECEF position + velocity
//   0x82 / 0x83 (newer firmware): ECEF + UTC date/time, GNSS satellites
//   0x7E auxiliary instrument data, 0x76 padding, 0x80 encrypted (RS41-SGM)
// Temperature, humidity and pressure come from the frequencies through the
// calibration table (platinum resistor + reference resistors, capacitive
// humidity sensor with its own temperature and a 7x6 calibration matrix,
// pressure polynomial on the -SGP).
//
// Protocol and calibration model as documented by the open-source radiosonde
// community (rs1729/RS "rs41mod", DF9DQ); this is an independent
// implementation, validated against the radiosonde_auto_rx test recordings.

#include "rs.hpp"
#include "sonde.hpp"

#include <array>
#include <cstdio>
#include <memory>

namespace sonde {
namespace {

constexpr int FRAME_MAX = 518, FRAME_STD = 320;
const char* HEADER = "0000100001101101010100111000100001000100011010010100100000011111";
const uint8_t MASK[64] = {0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
                          0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
                          0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
                          0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};

class Rs41 : public Type {
public:
    explicit Rs41(double fs) : rs_(0x11D, 0, 1, 24) {
        FskConfig c;
        c.baud = 4800;
        for (const char* p = HEADER; *p; p++) c.sync.push_back(*p == '1');
        c.max_sync_err = 4;
        c.frame_syms = (FRAME_MAX - 8) * 8;
        rx_.init(c, fs);
        fs_ = fs;
        rx_.on_frame = [this](const FskFrame& f) { frame(f); };
        reset();
    }
    void push(const float* narrow, const float*, size_t n, double t0) override {
        t_ = t0;
        for (size_t i = 0; i < n; i++) rx_.push(narrow[i]);
    }
    void reset() override {
        rx_.reset();
        serial_.clear();
        cal_.fill(0);
        have_.fill(false);
        last_no_ = -1;
        last_t0_ = -1e12;
        last_alt_ = NAN;
        week_ = -1;
        freq_khz_ = 0;
        subtype_.clear();
        board_.clear();
        fw_ = 0;
        bt_ = kt_ = cd_ = -1;
        bk_ = false;
    }

private:
    void frame(const FskFrame& f);
    bool have(std::initializer_list<int> frags) const {
        for (int i : frags)
            if (!have_[i]) return false;
        return true;
    }
    float cf(int off) const { return f32le(&cal_[off]); }
    double temperature(uint32_t f, uint32_t f1, uint32_t f2, int co, int calt) const;
    double humidity(uint32_t f, uint32_t f1, uint32_t f2, double t, double th, double p) const;
    double pressure(uint32_t f, uint32_t f1, uint32_t f2, double tp) const;
    void status(const uint8_t* d, Frame& o);
    void meas(const uint8_t* d, Frame& o);

    FskRx rx_;
    ReedSolomon rs_;
    double fs_ = 48000, t_ = 0;
    std::string serial_;
    std::array<uint8_t, 51 * 16> cal_{};
    std::array<bool, 51> have_{};
    long last_no_ = -1;
    double last_t0_ = -1e12, last_alt_ = NAN;
    int week_ = -1;
    int freq_khz_ = 0;
    std::string subtype_, board_, typ_tmp_;
    int fw_ = 0;
    long bt_ = -1, kt_ = -1, cd_ = -1;
    bool bk_ = false;
};

void Rs41::frame(const FskFrame& f) {
    uint8_t fr[FRAME_MAX] = {0};
    // header (already matched) + the soft symbols, bytes LSB first, de-whitened
    for (int i = 0; i < 8; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) b |= (HEADER[i * 8 + k] == '1') << k;
        fr[i] = b ^ MASK[i];
    }
    for (int i = 8; i < FRAME_MAX; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++) b |= (f.soft[(i - 8) * 8 + k] > 0) << k;
        fr[i] = b ^ MASK[i % 64];
    }
    // frame type: 0x0F standard (320), 0xF0 extended (518) - majority of its bits
    int ft = 0;
    for (int i = 0; i < 4; i++) ft += ((fr[56] >> i) & 1) - ((fr[56] >> (i + 4)) & 1);
    const int len = ft >= 0 ? FRAME_STD : FRAME_MAX;
    for (int i = len; i < FRAME_MAX; i++) fr[i] = 0;

    // Reed-Solomon over the even and the odd bytes from 56 (parity at 8 / 32)
    uint8_t cw[2][255];
    for (int c = 0; c < 2; c++) {
        std::memset(cw[c], 0, 255);
        for (int i = 0; i < 24; i++) cw[c][i] = fr[8 + 24 * c + i];
        for (int i = 0; 56 + 2 * i + c < FRAME_MAX && 24 + i < 255; i++) cw[c][24 + i] = fr[56 + 2 * i + c];
    }
    const int e0 = rs_.decode(cw[0]), e1 = rs_.decode(cw[1]);
    if (e0 >= 0 && e1 >= 0) {
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 24; i++) fr[8 + 24 * c + i] = cw[c][i];
            for (int i = 0; 56 + 2 * i + c < len && 24 + i < 255; i++) fr[56 + 2 * i + c] = cw[c][24 + i];
        }
    }

    Frame o;
    o.type = "RS41";
    o.dc_hz = f.dc_hz;
    bool any = false, have_pos = false;
    double ecef[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    int pos = 57;
    while (pos + 4 <= len) {
        const int id = fr[pos], bl = fr[pos + 1];
        if (pos + 2 + bl + 2 > len) break;
        const uint8_t* d = fr + pos + 2;
        if (u16le(d + bl) != crc16_ccitt(d, bl)) {
            pos += 2 + bl + 2;
            continue;   // this block is damaged - the others may still be fine
        }
        any = true;
        switch (id) {
        case 0x79:   // status
            if (bl >= 40) status(d, o);
            break;
        case 0x7A:   // measurements (PTU)
            if (bl >= 42) meas(d, o);
            break;
        case 0x7C:   // GPS week + time of week
            if (bl >= 6) {
                week_ = u16le(d);
                o.utc = gps_to_utc(week_, u32le(d + 2) / 1000.0);
            }
            break;
        case 0x7B:   // ECEF position (cm) + velocity (cm/s), satellites
            if (bl >= 19) {
                for (int k = 0; k < 3; k++) {
                    ecef[k] = i32le(d + 4 * k) / 100.0;
                    vel[k] = i16le(d + 12 + 2 * k) / 100.0;
                }
                o.sats = d[18];
                have_pos = true;
            }
            break;
        case 0x82:   // newer firmware: ECEF + UTC date and time
            if (bl >= 26) {
                for (int k = 0; k < 3; k++) {
                    ecef[k] = i32le(d + 4 * k) / 100.0;
                    vel[k] = i16le(d + 12 + 2 * k) / 100.0;
                }
                have_pos = true;
                const int y = u16le(d + 18), mo = d[20], dd = d[21], h = d[22], mi = d[23];
                double s = d[24] + (d[25] < 100 ? d[25] / 100.0 : 0);
                if (y > 2000 && mo >= 1 && mo <= 12) o.utc = utc_seconds(y, mo, dd, h, mi, s);
            }
            break;
        case 0x83: {  // newer firmware: GNSS satellites - status nibbles
            if (bl >= 41) {
                int n = 0;
                for (int j = 0; j < 16; j++) n += ((d[25 + j] & 0xF) != 0) + ((d[25 + j] >> 4) != 0);
                o.sats = n;
            }
            break;
        }
        case 0x7E:   // auxiliary instrument (XDATA)
            o.extra.push_back({"Aux data", hex(d, std::min(bl, 24)) + (bl > 24 ? "..." : "")});
            break;
        case 0x80:
            o.extra.push_back({"Encrypted", "yes (RS41-SGM: position and PTU not decodable)"});
            if (subtype_.empty()) subtype_ = "RS41-SGM";
            break;
        default:
            break;
        }
        pos += 2 + bl + 2;
    }
    if (!any || serial_.empty()) return;          // nothing checked out / no status yet
    // the same frame caught by another timing phase (frames are 1 s apart)
    if (std::fabs(f.t0 - last_t0_) < 0.25 * fs_) return;
    if (o.frame_no >= 0 && o.frame_no == last_no_) return;
    last_t0_ = f.t0;
    if (o.frame_no >= 0) last_no_ = o.frame_no;

    if (have_pos) {
        double lat, lon, alt;
        ecef_to_geo(ecef, &lat, &lon, &alt);
        if (alt > -1000 && alt < 80000 && (ecef[0] || ecef[1] || ecef[2])) {
            o.lat = lat; o.lon = lon; o.alt = alt;
            last_alt_ = alt;
            double ve, vn, vu;
            ecef_vel_to_enu(vel, lat, lon, &ve, &vn, &vu);
            en_to_speed_dir(ve, vn, &o.vh, &o.heading);
            o.vv = vu;
        }
    }
    o.serial = serial_;
    o.subtype = subtype_.empty() ? "RS41" : subtype_;
    if (freq_khz_) o.tx_mhz = freq_khz_ / 1000.0;
    if (!board_.empty()) o.extra.push_back({"Mainboard", board_ + (fw_ ? " fw " + std::to_string(fw_) : "")});
    if (cd_ >= 0 && cd_ != 0xFFFF) o.burst_timer = cd_;
    if (kt_ >= 0 && kt_ != 0xFFFF) o.extra.push_back({"Kill timer", std::to_string(kt_) + " s"});
    if (bt_ > 0) o.extra.push_back({"Burst timer", std::to_string(bt_) + " s" + (bk_ ? " (burst kill on)" : "")});
    int ncal = 0;
    for (bool h : have_) ncal += h;
    if (ncal < 51) o.extra.push_back({"Calibration", std::to_string(ncal) + "/51 fragments"});
    if (want_raw) o.raw = hex(fr, len);
    if (out) out(o);
}

void Rs41::status(const uint8_t* d, Frame& o) {
    o.frame_no = u16le(d);
    std::string sn;
    for (int i = 2; i < 10; i++) sn += (d[i] >= 0x20 && d[i] < 0x7F) ? static_cast<char>(d[i]) : '?';
    if (sn != serial_) {   // another sonde: forget the calibration
        serial_ = sn;
        cal_.fill(0);
        have_.fill(false);
        subtype_.clear(); board_.clear(); typ_tmp_.clear();
        freq_khz_ = 0; fw_ = 0; bt_ = kt_ = cd_ = -1; bk_ = false;
        last_no_ = -1;
    }
    o.batt = d[10] / 10.0;
    const int frag = d[23];
    const uint8_t* c = d + 24;    // the 16 bytes of this calibration fragment
    if (frag < 51) {
        if (frag != 0x32) std::memcpy(&cal_[frag * 16], c, 16);   // 0x32 changes (countdown): not part of the table
        have_[frag] = true;
        switch (frag) {
        case 0x00:                 // transmit frequency: 400 MHz + 40 kHz steps (+ 10 kHz fractions)
            freq_khz_ = 400000 + 40 * c[3] + ((c[2] & 0xC0) * 10) / 64;
            break;
        case 0x01:
            fw_ = c[5] | c[6] << 8;
            break;
        case 0x02:
            kt_ = c[7] | c[8] << 8;
            bk_ = c[11] != 0;
            break;
        case 0x21:                 // sonde type, first 8 characters
            typ_tmp_.clear();
            for (int i = 8; i < 16 && c[i]; i++) typ_tmp_ += (c[i] >= 0x20 && c[i] < 0x7F) ? static_cast<char>(c[i]) : '?';
            break;
        case 0x22:                 // its 9th character, and the mainboard
            if (!typ_tmp_.empty()) {
                if (c[0] >= 0x20 && c[0] < 0x7F) typ_tmp_ += static_cast<char>(c[0]);
                subtype_ = typ_tmp_;
            }
            board_.clear();
            for (int i = 2; i < 10 && c[i] >= 0x20 && c[i] < 0x7F; i++) board_ += static_cast<char>(c[i]);
            break;
        case 0x31:
            bt_ = c[6] | c[7] << 8;
            break;
        case 0x32:
            cd_ = c[0] | c[1] << 8;
            break;
        default:
            break;
        }
    }
}

// platinum resistor between two reference resistors (Rf1, Rf2 at 0x3D / 0x41)
double Rs41::temperature(uint32_t f, uint32_t f1, uint32_t f2, int co, int calt) const {
    if (f2 == f1) return NAN;
    const double rf1 = cf(61), rf2 = cf(65);
    const double g = (static_cast<double>(f2) - f1) / (rf2 - rf1);
    const double rb = (f1 * rf2 - f2 * rf1) / (static_cast<double>(f2) - f1);
    const double r = (f / g - rb) * cf(calt);
    return (cf(co) + cf(co + 4) * r + cf(co + 8) * r * r + cf(calt + 4)) * (1 + cf(calt + 8));
}

// capacitive sensor: capacitance from its frequency between two reference
// capacitors, a pressure / temperature correction, then the 7x6 matrix in
// (capacitance, sensor temperature); scaled from the sensor's temperature to
// the air's
double Rs41::humidity(uint32_t f, uint32_t f1, uint32_t f2, double t, double th, double p) const {
    if (f2 == f1) return NAN;
    const double fh = (static_cast<double>(f) - f1) / (static_cast<double>(f2) - f1);
    const double cap = cf(69) + (cf(73) - cf(69)) * fh;
    double cp = (cap / cf(117) - 1) * cf(121);
    const double tn = (th - 20) / 180;
    double b[6];
    b[0] = 1;
    for (int k = 1; k < 6; k++) b[k] = b[k - 1] * tn;
    if (p > 0) {
        const double pb = p / 1000;
        double cpj = 1, corr = 0;
        for (int j = 0; j < 3; j++) {
            const double hp = cf(678 + 4 * j);
            const double bp = hp * (pb / (1 + hp * pb) - cpj / (1 + hp));
            double bt = 0;
            for (int k = 0; k < 4; k++) bt += cf(698 + 4 * (4 * j + k)) * b[k];
            corr += bp * bt;
            cpj *= cp;
        }
        cp -= corr;
    }
    double rh = 0, aj = 1;
    for (int j = 0; j < 7; j++) {
        for (int k = 0; k < 6; k++) rh += aj * b[k] * cf(125 + 4 * (6 * j + k));
        aj *= cp;
    }
    rh *= svp_hw(th) / svp_hw(t);
    return std::max(0.0, std::min(100.0, rh));
}

// RS41-SGP: polynomial in the normalised frequency and the sensor's temperature
double Rs41::pressure(uint32_t f, uint32_t f1, uint32_t f2, double tp) const {
    if (f1 == f2 || f1 == f) return NAN;
    // coefficient (j, k) of a0^j a1^k: 18 floats from 0x25E, stored by k-major groups
    double c[25] = {0};
    const int order[18] = {0, 4, 8, 12, 16, 20, 24, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11};
    for (int i = 0; i < 18; i++) c[order[i]] = cf(606 + 4 * i);
    const double a0 = c[24] / ((static_cast<double>(f) - f1) / (static_cast<double>(f2) - f1));
    double p = 0, a0j = 1;
    for (int j = 0; j < 6; j++) {
        double a1k = 1;
        for (int k = 0; k < 4; k++) {
            p += a0j * a1k * c[j * 4 + k];
            a1k *= tp;
        }
        a0j *= a0;
    }
    return p;
}

void Rs41::meas(const uint8_t* d, Frame& o) {
    uint32_t m[12];
    for (int i = 0; i < 12; i++) m[i] = u24le(d + 3 * i);
    const bool hr = have({3, 4});
    double t = NAN, th = NAN, p = NAN;
    if (hr && have({4, 5, 6})) t = temperature(m[0], m[1], m[2], 77, 89);
    if (hr && have({0x12, 0x13})) th = temperature(m[6], m[7], m[8], 293, 305);
    const bool sgp = have({0x21}) && cal_[0x21F] == 'P';
    if (sgp && have({0x25, 0x26, 0x27, 0x28, 0x29, 0x2A})) p = pressure(m[9], m[10], m[11], i16le(d + 38) / 100.0);
    if (std::isfinite(t) && t > -100 && t < 70) o.temp = t;
    if (std::isfinite(th) && th > -100 && th < 70) o.temp_rh = th;
    if (std::isfinite(p) && p > 0 && p < 1100) o.pressure = p;
    // humidity: needs the matrix fragments; the pressure (measured, else the
    // standard atmosphere at the last GPS altitude) corrects it at low pressure
    const double pc = std::isfinite(o.pressure) ? o.pressure : std::isfinite(last_alt_) ? isa_pressure(last_alt_) : -1;
    if (std::isfinite(o.temp) && std::isfinite(o.temp_rh) && have({7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E}))
        o.rh = humidity(m[3], m[4], m[5], o.temp, o.temp_rh, pc);
}

}  // namespace

std::unique_ptr<Type> make_rs41(double fs) { return std::make_unique<Rs41>(fs); }

}  // namespace sonde
