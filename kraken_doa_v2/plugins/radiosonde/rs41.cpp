// Vaisala RS41 (family "RS41").
//
// GFSK 4800 bit/s, NRZ, one frame a second; bytes LSB first, whitened with a
// 64-byte mask by frame byte index. Frame: 8-byte header (the sync), the
// parity of two interleaved RS(255,231) codewords (frame bytes 8..55), the
// frame type (56: 0x0F = 320 bytes, 0xF0 = 518 bytes with auxiliary data),
// then sub-blocks id / length / data / CRC-16 to the end. The status block
// carries a 16-byte fragment of the 816-byte calibration table (51 fragments,
// cycling): TX frequency, names, the sensor constants of temperature,
// humidity and pressure. Facts of the frame layout and the sensor model from
// ra-firmware (DF9DQ, github.com/einergehtnochrein/ra-firmware); the
// temperature / humidity / pressure computation below follows its
// src/rs41/rs41metrology.c closely, under its licence:
//
//   Copyright (c) 2017, einergehtnochrein
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//   * Neither the name of the copyright holder nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
//   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "families.hpp"

#include <algorithm>
#include <cstring>

namespace sonde {

namespace {

const char* RS41_SYNC = "0000100001101101010100111000100001000100011010010100100000011111";
const uint8_t RS41_HEADER[8] = {0x86, 0x35, 0xF4, 0x40, 0x93, 0xDF, 0x1A, 0x60};
const uint8_t RS41_WHITEN[64] = {
    0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
    0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
    0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
    0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};
constexpr int FRAME_MAX = 518;
constexpr int NSYM = (FRAME_MAX - 8) * 8;   // 4080: the longest frame after the header
constexpr int NFRAG = 51;
constexpr size_t MAX_TABLES = 8;            // calibration tables kept (by serial, least recently heard dropped)

// one sonde's calibration table
struct CalTable {
    std::string serial;
    uint8_t t[NFRAG * 16] = {};
    bool have[NFRAG] = {};
    int n = 0;                              // distinct fragments received
    int countdown = -2;                     // fragment 50 bytes 0..1 as last received (-2 = never)
    bool all(int a, int b) const {
        for (int i = a; i <= b; i++)
            if (!have[i]) return false;
        return true;
    }
    float f(int off) const { return f32le(t + off); }
    int16_t s16(int off) const { return static_cast<int16_t>(u16le(t + off)); }
    std::string text(int off, int n) const {   // printable characters up to a NUL
        std::string s;
        for (int i = 0; i < n; i++) {
            const uint8_t c = t[off + i];
            if (c == 0) break;
            if (c >= 0x20 && c <= 0x7E) s += static_cast<char>(c);
        }
        return s;
    }
};

// saturation vapour pressure with ra-firmware's temperature pre-transform
double svp_rs41(double t_c) {
    const double tk = t_c + 273.15;
    const double tt = -0.4931358 + 1.0046094296 * tk - 1.3746454e-5 * tk * tk + 1.2743214e-8 * tk * tk * tk;
    return svp_hyland_wexler(tt);
}

class Rs41 : public Family {
public:
    Rs41() {
        FskParams p;
        p.rate = 4800;
        p.sync = RS41_SYNC;
        p.max_err = 4;
        p.nsym = NSYM;
        p.phases = 8;
        rx_.init(p);
    }

    void reset() override {
        rx_.reset();
        dup_.reset();
        last_counter_ = -1;
        serial_.clear();
        tables_.clear();
        last_h_ = NAN;
    }

    void process(const float* narrow, const float*, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        caps_.clear();
        rx_.process(narrow, n, idx, caps_);
        for (const auto& c : caps_) decode(c, ctx, out);
    }

private:
    FskRx rx_;
    ReedSolomon rs_{0x11D, 0, 1, 24};
    std::vector<FskCapture> caps_;
    DupWindow dup_;
    long long last_counter_ = -1;
    std::string serial_;                    // the latest serial heard
    std::list<CalTable> tables_;            // most recently heard first
    double last_h_ = NAN;                   // the latest valid GPS height

    CalTable& table(const std::string& serial) {
        for (auto it = tables_.begin(); it != tables_.end(); ++it)
            if (it->serial == serial) {
                tables_.splice(tables_.begin(), tables_, it);
                return tables_.front();
            }
        tables_.emplace_front();
        tables_.front().serial = serial;
        if (tables_.size() > MAX_TABLES) tables_.pop_back();
        return tables_.front();
    }

    void decode(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        uint8_t f[FRAME_MAX];
        std::memcpy(f, RS41_HEADER, 8);
        for (int j = 0; j < FRAME_MAX - 8; j++) {
            unsigned v = 0;
            for (int b = 0; b < 8; b++) v |= (c.sym[static_cast<size_t>(8 * j + b)] > 0 ? 1u : 0u) << b;   // LSB first
            f[8 + j] = static_cast<uint8_t>(v ^ RS41_WHITEN[(8 + j) & 63]);
        }
        // frame length from the type byte (before any correction)
        const int lo = __builtin_popcount(f[56] & 0x0F), hi = __builtin_popcount(f[56] >> 4);
        const int len = lo >= hi ? 320 : 518;
        if (len < FRAME_MAX) std::memset(f + len, 0, static_cast<size_t>(FRAME_MAX - len));
        // the two Reed-Solomon codewords: x^k = parity byte k, x^(24+i) = byte 56 + 2i + c
        const int ndata = len == 320 ? 132 : 231;
        uint8_t cw[2][255];
        bool ok = true;
        for (int k = 0; k < 2; k++) {
            std::memset(cw[k], 0, 255);
            for (int i = 0; i < 24; i++) cw[k][i] = f[8 + 24 * k + i];
            for (int i = 0; i < ndata; i++) cw[k][24 + i] = f[56 + 2 * i + k];
            if (rs_.decode(cw[k], 24 + ndata) < 0) ok = false;
        }
        if (ok)
            for (int k = 0; k < 2; k++) {
                for (int i = 0; i < 24; i++) f[8 + 24 * k + i] = cw[k][i];
                for (int i = 0; i < ndata; i++) f[56 + 2 * i + k] = cw[k][24 + i];
            }
        // else: the frame as received - every sub-block still has its own CRC

        Record r;
        r.family = "RS41";
        int passed = 0;
        bool have_status = false, have_meas = false, short_meas = false, encrypted = false;
        int crypto = 0, frag = -1;
        const uint8_t* meas = nullptr;
        std::string serial;
        int sats7b = -1, sats83 = -1;
        bool pos_ok = false;
        double ex = 0, ey = 0, ez = 0, vx = 0, vy = 0, vz = 0;
        for (int p = 57; p + 4 <= len;) {
            const int id = f[p], L = f[p + 1];
            if (p + 4 + L > len) break;
            const uint8_t* d = f + p + 2;
            const uint16_t crc = u16le(d + L);
            p += 4 + L;
            if (crc16_ccitt_false(d, static_cast<size_t>(L)) != crc) continue;
            passed++;
            switch (id) {
                case 0x79:   // status + calibration fragment
                    if (L < 40) break;
                    have_status = true;
                    r.frame = u16le(d);
                    for (int i = 0; i < 8; i++) {
                        const uint8_t ch = d[2 + i];
                        serial += ch >= 0x20 && ch <= 0x7E ? static_cast<char>(ch) : '?';
                    }
                    r.batt = d[10] * 0.1;
                    crypto = d[15];
                    frag = d[23];
                    if (frag < NFRAG) {
                        CalTable& t = table(serial);
                        if (!t.have[frag]) {
                            t.have[frag] = true;
                            t.n++;
                        }
                        std::memcpy(t.t + 16 * frag, d + 24, 16);
                        if (frag == 50) t.countdown = static_cast<int16_t>(u16le(d + 24));
                    }
                    break;
                case 0x7A:   // measurements
                    if (L < 42) break;
                    have_meas = true;
                    short_meas = false;
                    meas = d;
                    break;
                case 0x7F:   // short measurements (RS41-SGM, unencrypted): 3 channels
                    if (L < 27) break;
                    if (!have_meas) {
                        have_meas = true;
                        short_meas = true;
                        meas = d;
                    }
                    break;
                case 0x7C: {   // GPS week + time of week
                    if (L < 6) break;
                    r.utc = gps_to_unix(u16le(d), u32le(d + 2) / 1000.0);
                    break;
                }
                case 0x7B:   // GPS ECEF position + velocity
                    if (L < 19) break;
                    sats7b = d[18];
                    ex = static_cast<int32_t>(u32le(d)) / 100.0;
                    ey = static_cast<int32_t>(u32le(d + 4)) / 100.0;
                    ez = static_cast<int32_t>(u32le(d + 8)) / 100.0;
                    vx = static_cast<int16_t>(u16le(d + 12)) / 100.0;
                    vy = static_cast<int16_t>(u16le(d + 14)) / 100.0;
                    vz = static_cast<int16_t>(u16le(d + 16)) / 100.0;
                    pos_ok = sats7b > 0;
                    break;
                case 0x82: {   // GNSS position + UTC date / time (newer firmware)
                    if (L < 25) break;
                    ex = static_cast<int32_t>(u32le(d)) / 100.0;
                    ey = static_cast<int32_t>(u32le(d + 4)) / 100.0;
                    ez = static_cast<int32_t>(u32le(d + 8)) / 100.0;
                    vx = static_cast<int16_t>(u16le(d + 12)) / 100.0;
                    vy = static_cast<int16_t>(u16le(d + 14)) / 100.0;
                    vz = static_cast<int16_t>(u16le(d + 16)) / 100.0;
                    pos_ok = true;
                    const int y = u16le(d + 18), mo = d[20], dd = d[21], hh = d[22], mi = d[23], ss = d[24];
                    if (y > 2000 && mo >= 1 && mo <= 12 && dd >= 1 && dd <= 31 && hh < 24 && mi < 60 && ss < 61) {
                        double s = ss;
                        if (L > 25 && d[25] < 100) s += d[25] / 100.0;   // hundredths of a second
                        r.utc = unix_time(y, mo, dd, hh, mi, s);
                    }
                    break;
                }
                case 0x83: {   // GNSS satellite info
                    if (L < 41) break;
                    int k = 0;
                    for (int i = 25; i <= 40; i++)
                        if (d[i] & 0x70) k++;
                    sats83 = k;
                    break;
                }
                case 0x7E: {   // auxiliary instrument data
                    if (L < 1) break;
                    const int m = std::min(L, 24);
                    r.extras.emplace_back("Aux data", hex_upper(d, static_cast<size_t>(m)) + (L > 24 ? "..." : ""));
                    break;
                }
                case 0x78:
                case 0x80:   // encrypted (RS41-SGM)
                    encrypted = true;
                    break;
                default:
                    break;
            }
        }
        if (have_status) serial_ = serial;
        if (!passed || serial_.empty()) return;
        // the same frame again (another timing phase), or the same counter
        if (dup_.near(c.pos0, 0.25) || (r.frame >= 0 && r.frame == last_counter_)) return;
        dup_.accept(c.pos0);
        if (r.frame >= 0) last_counter_ = r.frame;

        r.serial = serial_;
        if (encrypted) r.extras.emplace_back("Encrypted", "yes (RS41-SGM: position and PTU not decodable)");
        const bool sgm = encrypted || (crypto >= 1 && crypto <= 4);

        // position
        if (pos_ok && (ex != 0 || ey != 0 || ez != 0)) {
            double la, lo2, h;
            ecef_to_geodetic(ex, ey, ez, &la, &lo2, &h);
            if (h > -100 && h < 80000) {
                r.lat = la;
                r.lon = lo2;
                r.alt = h;
                double e, nn, u;
                ecef_to_enu_velocity(la, lo2, vx, vy, vz, &e, &nn, &u);
                speed_dir(e, nn, &r.vh, &r.hdg);
                r.vv = u;
                last_h_ = h;
            }
        }
        if (sats7b >= 0) r.sats = sats7b;
        else if (sats83 >= 0) r.sats = sats83;

        // calibration: names, TX frequency, timers, PTU
        CalTable& t = table(serial_);
        std::string variant, board;
        if (t.have[33] && t.have[34]) variant = t.text(0x218, 10);
        if (t.have[34]) board = t.text(0x222, 10);
        r.model = !variant.empty() ? variant : sgm ? "RS41-SGM" : "RS41";
        if (t.have[0]) r.txf = 400.0 + (u16le(t.t + 0x002) / 64) * 0.01;
        if (!board.empty()) {
            const uint32_t fw = t.have[1] ? u32le(t.t + 0x015) : 0;
            r.extras.emplace_back("Mainboard", board + (fw ? " fw " + std::to_string(fw) : ""));
        }
        if (t.have[2]) {
            const int kill = t.s16(0x027);
            if (kill != -1) r.extras.emplace_back("Kill timer", std::to_string(kill) + " s");
        }
        if (t.have[49]) {
            const int bt = t.s16(0x316);
            if (bt > 0)
                r.extras.emplace_back("Burst timer", std::to_string(bt) + " s" + (t.have[2] && t.t[0x02B] ? " (burst kill on)" : ""));
        }
        if (t.n < NFRAG) r.extras.emplace_back("Calibration", std::to_string(t.n) + "/51 fragments");
        if (t.countdown != -2 && t.countdown != -1) r.countdown = t.countdown;
        if (have_meas) ptu(r, t, meas, short_meas, sgm);

        set_span(r, c, ctx);
        if (ctx.raw) r.hex = hex_upper(f, static_cast<size_t>(len));
        out.push_back(std::move(r));
    }

    // temperature, humidity, pressure from the measurement block and the table
    void ptu(Record& r, const CalTable& t, const uint8_t* m, bool short_block, bool sgm) {
        auto ratio = [&](int ch) -> double {   // normalised reading of channel ch
            const double v = u24le(m + 9 * ch), lo = u24le(m + 9 * ch + 3), hi = u24le(m + 9 * ch + 6);
            return hi == lo ? NAN : (v - lo) / (hi - lo);
        };
        const bool refs = t.all(3, 4);
        const double R_lo = t.f(0x03D), R_hi = t.f(0x041), C_lo = t.f(0x045), C_hi = t.f(0x049);
        double T = NAN, TU = NAN, P = NAN;
        // air temperature
        const double r0 = ratio(0);
        if (refs && t.all(5, 7) && std::isfinite(r0)) {
            const double R = R_lo + (R_hi - R_lo) * r0;
            const double x = R * t.f(0x059);
            const double T0 = t.f(0x04D) + t.f(0x051) * x + t.f(0x055) * x * x;
            double poly = 0, pw = 1;
            for (int k = 0; k < 6; k++) {
                poly += t.f(0x05D + 4 * k) * pw;
                pw *= T0;
            }
            const double v = T0 + poly;
            if (v > -100 && v < 70) T = v;
        }
        // humidity-sensor temperature
        const double r2 = ratio(2);
        if (refs && t.all(18, 20) && std::isfinite(r2)) {
            const double R = R_lo + (R_hi - R_lo) * r2;
            const double x = R * t.f(0x131);
            const double v = t.f(0x125) + t.f(0x129) * x + t.f(0x12D) * x * x;
            if (v > -100 && v < 70) TU = v;
        }
        // pressure (RS41-SGP)
        if (!short_block && !sgm && t.all(37, 42)) {
            const double r3 = ratio(3);
            if (std::isfinite(r3) && r3 != 0) {
                const double tp = static_cast<int16_t>(u16le(m + 38)) / 100.0;
                auto Pk = [&](int k) { return static_cast<double>(t.f(0x25E + 4 * k)); };
                double w[6];
                w[0] = Pk(0) + Pk(7) * tp + Pk(11) * tp * tp + Pk(15) * tp * tp * tp;
                w[1] = Pk(1) + Pk(8) * tp + Pk(12) * tp * tp + Pk(16) * tp * tp * tp;
                w[2] = Pk(2) + Pk(9) * tp + Pk(13) * tp * tp + Pk(17) * tp * tp * tp;
                w[3] = Pk(3) + Pk(10) * tp + Pk(14) * tp * tp;
                w[4] = Pk(4);
                w[5] = Pk(5);
                const double X = Pk(6) / r3;
                double p = 0, xp = 1;
                for (int k = 0; k < 6; k++) {
                    p += w[k] * xp;
                    xp *= X;
                }
                if (p > 0 && p < 1100) P = p;
            }
        }
        // relative humidity
        const double r1 = ratio(1);
        if (std::isfinite(T) && std::isfinite(TU) && std::isfinite(r1) && t.all(3, 20) && t.all(42, 46)) {
            // the humidity model's own sensor temperature
            double Trh = TU, pw = 1;
            for (int k = 0; k < 6; k++) {
                Trh += t.f(0x135 + 4 * k) * pw;
                pw *= TU;
            }
            const double C = C_lo + (C_hi - C_lo) * r1;
            double Cp = (C / t.f(0x075) - 1) * t.f(0x079);
            double pr = P;
            if (!std::isfinite(pr)) pr = std::isfinite(last_h_) ? isa_pressure_hpa(last_h_) : 30.0;
            const double tau = (Trh - 20) / 180, pi = pr / 1000;
            double corr = 0, cpi = 1;
            for (int i = 0; i < 3; i++) {
                double l = 0, tj = 1;
                for (int j = 0; j < 4; j++) {
                    l += t.f(0x2BA + 4 * (4 * i + j)) * tj;
                    tj *= tau;
                }
                const double q = t.f(0x2A6 + 4 * i);
                corr += l * q * (pi / (1 + q * pi) - cpi / (1 + q));
                cpi *= Cp;
            }
            Cp -= corr;
            double rh = 0, cj = 1;
            for (int j = 0; j < 7; j++) {
                double tk = 1;
                for (int k = 0; k < 6; k++) {
                    rh += t.f(0x07D + 4 * (6 * j + k)) * cj * tk;
                    tk *= tau;
                }
                cj *= Cp;
            }
            rh *= svp_rs41(TU) / svp_rs41(T);
            if (std::isfinite(rh)) r.RH = std::clamp(rh, 0.0, 100.0);
        }
        r.T = T;
        r.TU = TU;
        r.P = P;
    }
};

}  // namespace

std::unique_ptr<Family> make_rs41() { return std::make_unique<Rs41>(); }

}  // namespace sonde
