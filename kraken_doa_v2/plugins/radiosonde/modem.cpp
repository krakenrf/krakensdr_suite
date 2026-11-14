// Meteomodem M10 and M20 (families "M10", "M20").
//
// 2-FSK, biphase-mark line code (every bit cell starts with a level change, a
// 1 has another in its middle), bytes MSB first, a burst once a second: a run
// of zero bits, the frame sync, the frame (length byte, type byte, data, a
// 16-bit check). Two receivers on the wide discriminator - 9615 chips/s for
// the M10 (a little above its nominal 9600: it decodes the real sondes
// better), 9600 for the M20 - with one duplicate window, both dispatching by
// the frame's type byte. After the sync (taken one chip late, so chip pairs are
// Manchester symbols) a data bit is 1 when two consecutive symbols are equal:
// the polarity drops out. Field layouts from ra-firmware (DF9DQ,
// github.com/einergehtnochrein/ra-firmware); the M10 temperature and the M20
// humidity-sensor temperature / humidity computations below follow its
// src/modem/m10metrology.c and m20payload.c closely, under its licence:
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
//
// The M10 humidity model and the M20 main air temperature (the M10 network on
// the M20 ADC) are facts published by the rs1729/RS project (see README).

#include "families.hpp"

#include <algorithm>
#include <cstdio>

namespace sonde {

namespace {

const char* MODEM_SYNC = "10011001100110010100110010011001";
constexpr int NSYM = 1936;                   // 121 bytes: the longest frame + margin
constexpr int NBYTES = NSYM / 16;

// The NTC of the M10 network: series resistor Rs and parallel Rp per range
double ntc_temperature(int range, double a, const double* Rp, double k0) {
    static const double Rs[3] = {12.1e3, 36.5e3, 475e3};
    if (range < 0 || range > 2 || !(a > 0)) return NAN;
    const double y = (4095 - a) / a - Rs[range] / Rp[range];
    if (!(y > 0)) return NAN;
    const double R = Rs[range] / y;
    if (!(R > 0)) return NAN;
    const double L = std::log(R);
    return 1 / (1.07303516e-3 + 2.41296733e-4 * L + 2.26744154e-6 * L * L + 6.52855181e-8 * L * L * L) - k0;
}

// GPS week as the Meteomodems send it (10-bit rollover) + time of week -> UTC
double modem_utc(int week, double tow_s, double leap) {
    if (week < 1304) week += 1024;
    if (!(week > 1304 && week < 4000) || !(tow_s >= 0 && tow_s < 604800)) return NAN;
    return gps_to_unix(week, tow_s, leap);
}

class Meteomodem : public Family {
public:
    Meteomodem() {
        FskParams p;
        p.sync = MODEM_SYNC;
        p.max_err = 2;
        p.nsym = NSYM;
        p.phases = 6;
        p.rate = 9615;
        m10_.init(p);
        p.rate = 9600;
        m20_.init(p);
    }

    void reset() override {
        m10_.reset();
        m20_.reset();
        dup_.reset();
    }

    void process(const float*, const float* wide, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        caps_.clear();
        m10_.process(wide, n, idx, caps_);
        m20_.process(wide, n, idx, caps_);
        std::stable_sort(caps_.begin(), caps_.end(), [](const FskCapture& a, const FskCapture& b) { return a.pos_end < b.pos_end; });
        for (const auto& c : caps_) {
            if (dup_.near(c.pos0, 0.3)) continue;
            if (decode(c, ctx, out)) dup_.accept(c.pos0);
        }
    }

private:
    FskRx m10_, m20_;
    std::vector<FskCapture> caps_;
    DupWindow dup_;

    bool decode(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        uint8_t b[NBYTES] = {};
        int prev = 0;
        for (int i = 0; i < NBYTES * 8; i++) {
            const int sym = c.sym[static_cast<size_t>(2 * i + 1)] - c.sym[static_cast<size_t>(2 * i)] >= 0 ? 1 : 0;
            const int bit = i == 0 ? 0 : (sym == prev ? 1 : 0);
            prev = sym;
            b[i / 8] = static_cast<uint8_t>(b[i / 8] | bit << (7 - i % 8));
        }
        const int n = b[0], type = b[1];
        const bool m20 = type == 0x20 && n >= 0x43 && n <= 0x59;
        const bool m10 = (type == 0x9F || type == 0xAF || type == 0x8F) && n >= 0x64 && n <= 0x78;
        if (!m10 && !m20) return false;
        if (meteomodem_check(b, static_cast<size_t>(n - 1)) != u16be(b + n - 1)) return false;
        Record r;
        if (m10 ? !decode_m10(b, r) : !decode_m20(b, r)) return false;
        set_span(r, c, ctx);
        if (ctx.raw) r.hex = hex_upper(b, static_cast<size_t>(n + 1));
        out.push_back(std::move(r));
        return true;
    }

    static bool decode_m10(const uint8_t* b, Record& r) {
        r.family = "M10";
        r.model = b[1] == 0xAF ? "M10+" : b[1] == 0x8F ? "M2K2" : "M10";
        const int s0 = b[0x5D], s2 = b[0x5F], s3 = b[0x60], s4 = b[0x61];
        char sn[40];
        snprintf(sn, sizeof sn, "%X%02d-%X-%d%04d", s2 >> 4, s2 & 0x0F, s0 & 0x0F, s4 >> 5, s3 + 256 * (s4 & 0x1F));
        r.serial = sn;
        r.sats = b[0x1E];
        // position: 4+ satellites and a clean GPS status
        if (r.sats >= 4 && (b[2] & 0x0F) == 0) {
            r.lat = static_cast<int32_t>(u32be(b + 0x0E)) * (360.0 / 4294967296.0);
            r.lon = static_cast<int32_t>(u32be(b + 0x12)) * (360.0 / 4294967296.0);
            r.alt = static_cast<int32_t>(u32be(b + 0x16)) / 1000.0;
            const double ve = static_cast<int16_t>(u16be(b + 4)) / 200.0;
            const double vn = static_cast<int16_t>(u16be(b + 6)) / 200.0;
            r.vv = static_cast<int16_t>(u16be(b + 8)) / 200.0;
            speed_dir(ve, vn, &r.vh, &r.hdg);
        }
        const int leap = b[0x1F];
        r.utc = modem_utc(u16be(b + 0x20), u32be(b + 0x0A) / 1000.0, leap <= 30 ? leap : GPS_UTC_LEAP_S);
        // temperature (ra-firmware's network: 3 MOhm on range 2, -273.16)
        static const double RP[3] = {1e20, 330e3, 3e6};
        const int range = b[0x3E];
        if (range < 3) {
            const double a = static_cast<double>(u16le(b + 0x3F)) - 0xA000;
            const double t = ntc_temperature(range, a, RP, 273.16);
            if (t > -120 && t < 60) r.T = t;
        }
        r.batt = u16le(b + 0x45) * 0.00668;
        // humidity: a capacitor timed against a 55 %RH reference (approximate)
        const double c55 = u24le(b + 0x32) / 1000.0, cc = u24le(b + 0x35) / 1000.0;
        if (c55 > 0 && std::isfinite(r.T)) {
            double rh = (cc / c55 - 0.8955) / 0.002;
            if (r.T < 0) rh += -r.T / 5.5;
            if (r.T < -30) rh *= 1 + (-30 - r.T) / 75;
            if (rh > -20 && rh < 120) {
                r.RH = std::clamp(rh, 0.0, 100.0);
                r.extras.emplace_back("Humidity model", "approximate (M10)");
            }
        }
        return true;
    }

    static bool decode_m20(const uint8_t* b, Record& r) {
        r.family = "M20";
        r.model = "M20";
        const int B0 = b[0x12], B1 = b[0x13], B2 = b[0x14];
        if (!B0 && !B1 && !B2) return false;   // no serial
        char sn[40];
        snprintf(sn, sizeof sn, "%d%02d-%d-%d%04d", (B0 & 0x7F) / 12, (B0 & 0x7F) % 12 + 1, 1 + (B0 >> 7) + 2 * (B1 & 3),
                 B2 >> 7, (B1 >> 2) + 64 * (B2 & 0x7F));
        r.serial = sn;
        // position
        const double lat = static_cast<int32_t>(u32be(b + 0x1C)) / 1e6;
        const double lon = static_cast<int32_t>(u32be(b + 0x20)) / 1e6;
        const double alt = i24(u24be(b + 8)) / 100.0;
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0) && alt > -100) {
            r.lat = lat;
            r.lon = lon;
            r.alt = alt;
            const double ve = static_cast<int16_t>(u16be(b + 0x0B)) / 100.0;
            const double vn = static_cast<int16_t>(u16be(b + 0x0D)) / 100.0;
            r.vv = static_cast<int16_t>(u16be(b + 0x18)) / 100.0;
            speed_dir(ve, vn, &r.vh, &r.hdg);
        }
        r.utc = modem_utc(u16be(b + 0x1A), u24be(b + 0x0F), GPS_UTC_LEAP_S);
        // humidity-sensor temperature (Steinhart-Hart, 22.1 kOhm divider)
        const double au = u16le(b + 6);
        if (au > 0 && au < 4095) {
            const double R = 22100 * au / (4095 - au);
            const double L = std::log(R);
            r.TU = 1 / (1.4592243e-3 + 2.41157e-4 * L + 8.4543096e-8 * L * L * L) - 273.15;
        }
        // humidity
        const double h = u16le(b + 2), cal = u16le(b + 0x2F);
        if (std::isfinite(r.TU) && h > 0 && h < 48000) {
            const double K = 6.4e8 / (cal + 80000);
            const double x = 4.16e9 / ((h + 80000) * K * (1 - 5.8e-4 * (r.TU - 25)));
            const double rh = 10.087 * x * x * x - 211.62 * x * x + 1388.2 * x - 2797;
            if (rh > -20 && rh < 120) r.RH = std::clamp(rh, 0.0, 100.0);
        }
        // main air temperature: the M10 NTC network on the 12-bit value + range
        // (2 MOhm on range 2, -273.15: the M20's own circuit)
        static const double RP[3] = {1e20, 330e3, 2e6};
        const unsigned v = u16le(b + 4);
        const int range = v > 8191 ? 2 : v > 4095 ? 1 : 0;
        const double a = v - (range == 2 ? 8192.0 : range == 1 ? 4096.0 : 0.0);
        const double t = ntc_temperature(range, a, RP, 273.15);
        if (t > -120 && t < 60) r.T = t;
        // pressure (0 = no sensor); firmware >= 7 sends a fraction byte at 0x16
        const unsigned praw = u16le(b + 0x24);
        if (praw) {
            const int fw = b[0x2B] == 0 ? b[0x43] : -1;
            const double p = fw >= 7 && fw <= 0x20 ? (praw * 256.0 + b[0x16]) / 4096 : praw / 16.0;
            if (p > 0 && p < 1100) r.P = p;
        }
        r.batt = b[0x26] * 3.0 / 228;
        return true;
    }
};

}  // namespace

std::unique_ptr<Family> make_meteomodem() { return std::make_unique<Meteomodem>(); }

}  // namespace sonde
