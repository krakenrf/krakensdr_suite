// InterMet iMet-54 (family "iMet-54").
//
// 2-FSK 4800 bit/s, every byte an 8N1 UART character. Before each frame at
// least ten 00 55 pairs, 24 24 24 24 and a header byte; then the main block:
// 14 groups of 8 bytes, each group bit-interleaved over 8 Hamming(8,4)
// codewords = 4 decoded bytes (56 bytes: serial, time, position, PTU, flags,
// CRC-32), then a second block. Layout and coding from ra-firmware (DF9DQ,
// BSD-3-Clause). The firmware on the test recording sends no CRC that matches
// any variant: its frames are taken when no codeword needed a correction.

#include "families.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace sonde {

namespace {

// the 40 bits of 8N1 "00 55 24 24" (the last preamble pair + two sync bytes)
const char* IMET54_SYNC = "0000000001010101010100010010010001001001";
// codeword byte (bit order of the de-interleaver) of each nibble
const uint8_t IMET54_HAM[16] = {0x00, 0x87, 0x99, 0x1E, 0xAA, 0x2D, 0x33, 0xB4, 0x4B, 0xCC, 0xD2, 0x55, 0xE1, 0x66, 0x78, 0xFF};
constexpr int NSYM = 2200;                   // 220 UART bytes
constexpr int SKIP = 3;                      // the two remaining 24s + the header
constexpr int MAIN_GROUPS = 14;              // 112 bytes -> 56
constexpr int B2_GROUPS = 13;                // the first 104 bytes of block 2 -> 52
constexpr uint32_t NOT_AVAILABLE = 0x4E6E6B28;   // float 1e9

// one group of 8 received bytes -> 4 bytes; returns corrections, -1 = a
// codeword more than one bit from every codeword
int decode_group(const uint8_t* in, uint8_t* out) {
    int fixes = 0;
    uint8_t nib[8];
    for (int m = 0; m < 8; m++) {
        unsigned cw = 0;
        for (int i = 0; i < 8; i++) cw |= ((in[i] >> (7 - m)) & 1u) << i;
        int best = -1, bd = 9;
        for (int k = 0; k < 16; k++) {
            const int d = __builtin_popcount(IMET54_HAM[k] ^ cw);
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        if (bd > 1) return -1;
        fixes += bd;
        nib[m] = static_cast<uint8_t>(best);
    }
    for (int t = 0; t < 4; t++) out[t] = static_cast<uint8_t>(nib[2 * t + 1] << 4 | nib[2 * t]);
    return fixes;
}

class Imet54 : public Family {
public:
    Imet54() {
        FskParams p;
        p.rate = 4798;
        p.sync = IMET54_SYNC;
        p.max_err = 3;
        p.nsym = NSYM;
        p.phases = 8;
        rx_.init(p);
    }

    void reset() override {
        rx_.reset();
        dup_.reset();
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

    bool decode(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        // UART: of every 10 bits keep 1..8 (LSB first)
        uint8_t u[NSYM / 10];
        for (int j = 0; j < NSYM / 10; j++) {
            unsigned v = 0;
            for (int b = 0; b < 8; b++) v |= (c.sym[static_cast<size_t>(10 * j + 1 + b)] > 0 ? 1u : 0u) << b;
            u[j] = static_cast<uint8_t>(v);
        }
        const uint8_t header = u[SKIP - 1];
        uint8_t d[4 * (MAIN_GROUPS + B2_GROUPS)];
        int fixes = 0;
        bool main_ok = true, all_ok = true;
        for (int g = 0; g < MAIN_GROUPS + B2_GROUPS; g++) {
            const int f = decode_group(u + SKIP + 8 * g, d + 4 * g);
            if (f < 0) {
                all_ok = false;
                if (g < 13) main_ok = false;   // main bytes 0..51
                std::memset(d + 4 * g, 0, 4);
            } else {
                fixes += f;
            }
        }
        if (!main_ok) return false;
        // the documented CRC-32 (header + main bytes 0..51), else no correction at all
        uint8_t cb[53];
        cb[0] = header;
        std::memcpy(cb + 1, d, 52);
        const bool crc = crc32_mpeg2(cb, 53) == u32le(d + 52);
        if (!crc && !(all_ok && fixes == 0)) return false;
        const uint16_t flags = u16le(d + 40);
        if ((flags & 0x0030) != 0x0030) return false;
        const uint32_t tv = u32le(d + 4);
        if (tv > 235959999) return false;
        const auto latlon = [](int32_t v) {
            const int deg = v / 1000000;
            const double min = (v - deg * 1000000) / 1e4;
            return deg + min / 60;
        };
        const double lat = latlon(static_cast<int32_t>(u32le(d + 8))), lon = latlon(static_cast<int32_t>(u32le(d + 12)));
        const double alt = static_cast<int32_t>(u32le(d + 16)) / 10.0;
        if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || !(alt > -400 && alt < 60000)) return false;

        Record r;
        r.family = "iMet-54";
        r.serial = std::to_string(u32le(d));
        const int hh = static_cast<int>(tv / 10000000), mm = static_cast<int>(tv / 100000 % 100);
        const double ss = (tv % 100000) / 1000.0;
        r.tod = hh * 3600.0 + mm * 60.0 + ss;
        if (!(lat == 0 && lon == 0)) {
            r.lat = lat;
            r.lon = lon;
            r.alt = alt;
        }
        const uint32_t ft = u32le(d + 28), fh = u32le(d + 32), fu = u32le(d + 36);
        const double T = ft == NOT_AVAILABLE ? NAN : f32le(d + 28);
        const double RHs = fh == NOT_AVAILABLE ? NAN : f32le(d + 32);
        const double TU = fu == NOT_AVAILABLE ? NAN : f32le(d + 36);
        if (T > -120 && T < 80) r.T = T;
        if (TU > -120 && TU < 80) r.TU = TU;
        // humidity measured at the sensor's temperature, moved to the air
        // temperature: only with all three
        if (std::isfinite(RHs) && std::isfinite(r.T) && std::isfinite(r.TU)) {
            const double rh = std::clamp(RHs, 0.0, 100.0);
            r.RH = std::clamp(rh * svp_hyland_wexler(r.TU + 273.15) / svp_hyland_wexler(r.T + 273.15), 0.0, 100.0);
        }
        const int32_t pr = static_cast<int32_t>(u32le(d + 24)), pt = static_cast<int32_t>(u32le(d + 20));
        if (pr != 0) r.P = pr / 100.0;
        if (pt != 0) r.extras.emplace_back("Pressure sensor temp.", fmt2(pt / 100.0));
        // the 108 decoded bytes as big-endian 32-bit words (the raw text's order)
        uint8_t be[108];
        for (int i = 0; i < 108; i++) be[i] = d[4 * (i / 4) + 3 - i % 4];
        bool zero = true;
        for (int i = 0x2C; i <= 0x51; i++)
            if (be[i]) zero = false;
        r.model = zero && (flags & 0x0F0F) == 0 && ft == NOT_AVAILABLE && fh == NOT_AVAILABLE && fu == NOT_AVAILABLE ? "iMet-50" : "iMet-54";
        set_span(r, c, ctx);
        if (ctx.raw) r.hex = hex_upper(be, sizeof be);
        out.push_back(std::move(r));
        return true;
    }

    static std::string fmt2(double v) {
        char b[48];
        snprintf(b, sizeof b, "%.2f °C", v);
        return b;
    }
};

}  // namespace

std::unique_ptr<Family> make_imet54() { return std::make_unique<Imet54>(); }

}  // namespace sonde
