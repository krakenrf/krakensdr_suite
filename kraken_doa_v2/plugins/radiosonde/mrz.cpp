// Meteo-Radiy MRZ (family "MRZ").
//
// 2-FSK 2400 chips/s Manchester (bit 1 = chips 10), bytes MSB first: the sync
// AA BF 35 after an AA preamble, then a 47-byte packet (time, ECEF position +
// velocity, the sonde's own temperature / humidity / pressure, one of 16
// calibration words, CRC-16/MODBUS). Packets come ~0.34 s apart, usually each
// twice. Layout from ra-firmware (DF9DQ, BSD-3-Clause). The sonde is named by
// calibration words 12 (sonde serial) and 13 (sensor serial).

#include "families.hpp"

#include <cstring>

namespace sonde {

namespace {

const char* MRZ_SYNC = "100110011001100110011010101010100101101001100110";   // Manchester of AA BF 35
constexpr int NBYTES = 47;
constexpr int NSYM = NBYTES * 16;

class Mrz : public Family {
public:
    Mrz() {
        FskParams p;
        p.rate = 2399;
        p.sync = MRZ_SYNC;
        p.max_err = 3;
        p.nsym = NSYM;
        p.phases = 8;
        rx_.init(p);
    }

    void reset() override {
        rx_.reset();
        dup_.reset();
        for (auto& h : have_) h = false;
    }

    void process(const float* narrow, const float*, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        caps_.clear();
        rx_.process(narrow, n, idx, caps_);
        for (const auto& c : caps_) {
            if (dup_.near(c.pos0, 0.3)) continue;
            if (decode(c, ctx, out)) dup_.accept(c.pos0);
        }
    }

private:
    FskRx rx_;
    std::vector<FskCapture> caps_;
    DupWindow dup_;
    uint32_t word_[16] = {};
    bool have_[16] = {};

    bool decode(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        uint8_t f[3 + NBYTES] = {0xAA, 0xBF, 0x35};
        uint8_t* p = f + 3;
        for (int i = 0; i < NBYTES * 8; i++) {
            const int bit = c.sym[static_cast<size_t>(2 * i)] - c.sym[static_cast<size_t>(2 * i + 1)] > 0 ? 1 : 0;
            p[i / 8] = static_cast<uint8_t>(p[i / 8] | bit << (7 - i % 8));
        }
        if (crc16_modbus(p, 45) != u16le(p + 45)) return false;
        // the calibration word of this packet (index 1..16 -> word 0..15)
        const int ci = p[40];
        if (ci >= 1 && ci <= 16) {
            const int w = ci - 1;
            const uint32_t v = u32le(p + 41);
            if ((w == 12 || w == 13) && have_[w] && word_[w] != v) have_[w == 12 ? 13 : 12] = false;   // another sonde
            word_[w] = v;
            have_[w] = true;
        }
        if (!have_[12] || !have_[13]) return false;   // no serial yet (the packet counts as decoded)
        Record r;
        r.family = "MRZ";
        r.model = "MRZ";
        r.serial = std::to_string(word_[12]) + "-" + std::to_string(word_[13]);
        const int hh = p[1], mm = p[2], ss = p[3];
        if (hh < 24 && mm < 60 && ss < 61) {
            const uint32_t dt = word_[15];
            const int day = static_cast<int>(dt / 10000), mon = static_cast<int>(dt / 100 % 100), yr = 2000 + static_cast<int>(dt % 100);
            if (have_[15] && yr > 2000 && mon >= 1 && mon <= 12 && day >= 1 && day <= 31)
                r.utc = unix_time(yr, mon, day, hh, mm, ss);
            else
                r.tod = hh * 3600.0 + mm * 60.0 + ss;
        }
        r.sats = p[23];
        const double x = static_cast<int32_t>(u32le(p + 5)) / 100.0, y = static_cast<int32_t>(u32le(p + 9)) / 100.0,
                     z = static_cast<int32_t>(u32le(p + 13)) / 100.0;
        if (r.sats > 0 && (x != 0 || y != 0 || z != 0)) {
            double la, lo, h;
            ecef_to_geodetic(x, y, z, &la, &lo, &h);
            if (h > -1000 && h < 80000) {
                r.lat = la;
                r.lon = lo;
                r.alt = h;
                double e, n, u;
                ecef_to_enu_velocity(la, lo, static_cast<int16_t>(u16le(p + 17)) / 100.0, static_cast<int16_t>(u16le(p + 19)) / 100.0,
                                     static_cast<int16_t>(u16le(p + 21)) / 100.0, &e, &n, &u);
                speed_dir(e, n, &r.vh, &r.hdg);
                r.vv = u;
            }
        }
        const double t = static_cast<int16_t>(u16le(p + 26)) / 100.0;
        if (t > -120 && t < 80) r.T = t;
        const double rh = static_cast<int16_t>(u16le(p + 28)) / 100.0;
        if (rh >= 0 && rh <= 100) r.RH = rh;
        const int16_t pr = static_cast<int16_t>(u16le(p + 30));
        if (pr != -1 && pr > 0) r.P = pr * 0.2;
        set_span(r, c, ctx);
        if (ctx.raw) r.hex = hex_upper(f, sizeof f);
        out.push_back(std::move(r));
        return true;
    }
};

}  // namespace

std::unique_ptr<Family> make_mrz() { return std::make_unique<Mrz>(); }

}  // namespace sonde
