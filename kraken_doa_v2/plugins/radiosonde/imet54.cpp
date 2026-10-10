// InterMet iMet-54 (and iMet-50): 2-FSK 4798 baud, NRZ, bytes in 8N1
// framing (start bit, 8 data bits msb first, stop bit). Preamble 0x00 0xAA
// repeated, sync 0x24 x4 + 0x42; then 64-bit blocks transposed 8x8 (bit
// interleaving) carrying Hamming(8,4) codewords, two nibbles per byte. A
// frame is 108 bytes: serial (32 bit), time of day (HHMMSSmmm, decimal),
// latitude / longitude (degrees + minutes/100, x 1e6), altitude (dm, MSL),
// temperature, raw humidity and humidity-sensor temperature (big-endian
// float32; 1e9 = not available), status; a CRC-32 over the first 52 bytes.
// No velocity (the plugin derives it from successive positions).
//
// Protocol as documented by the open-source radiosonde community (rs1729/RS
// "imet54mod"); independent implementation, validated against the
// radiosonde_auto_rx test recording.

#include "sonde.hpp"

#include <cstdio>
#include <memory>

namespace sonde {
namespace {

// 0x00 0xAA 0x24 0x24 in 8N1, msb first
const char* HEADER = "0000000001" "0101010101" "0001001001" "0001001001";
constexpr int RAW_BITS = 2200;                  // 220 8N1 bytes after the header
constexpr int FRAME_BYTES = 108;
constexpr int CRC_POS = 0x34;
// Hamming(8,4) codeword (bits LSB first) of each nibble
const uint8_t HAM[16] = {0x00, 0x87, 0x99, 0x1E, 0xAA, 0x2D, 0x33, 0xB4, 0x4B, 0xCC, 0xD2, 0x55, 0xE1, 0x66, 0x78, 0xFF};

// CRC-32 (IEEE 802.3 polynomial, msb first, init 0, final xor 0x63D60875)
// over the first 52 bytes with each 32-bit word byte-reversed
bool crc_ok(const uint8_t* b) {
    uint32_t rem = 0;
    for (int i = 0; i < CRC_POS; i++) {
        rem ^= static_cast<uint32_t>(b[(i & ~3) + 3 - (i & 3)]) << 24;
        for (int k = 0; k < 8; k++) rem = (rem & 0x80000000u) ? (rem << 1) ^ 0x04C11DB7u : rem << 1;
    }
    return (rem ^ 0x63D60875u) == u32be(b + CRC_POS);
}

float f32be(const uint8_t* p) {
    const uint32_t v = u32be(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

class Imet54 : public Type {
public:
    explicit Imet54(double fs) : fs_(fs) {
        FskConfig c;
        c.baud = 4798;
        for (const char* p = HEADER; *p; p++) c.sync.push_back(*p == '1');
        c.max_sync_err = 3;
        c.frame_syms = RAW_BITS;
        rx_.init(c, fs);
        rx_.on_frame = [this](const FskFrame& f) { frame(f); };
    }
    void push(const float* narrow, const float*, size_t n, double t0) override {
        rx_.clock(t0);
        for (size_t i = 0; i < n; i++) rx_.push(narrow[i]);
    }
    void reset() override {
        rx_.reset();
        last_t0_ = -1e12;
    }

private:
    void frame(const FskFrame& f);
    FskRx rx_;
    double fs_, last_t0_ = -1e12;
};

void Imet54::frame(const FskFrame& f) {
    if (std::fabs(f.t0 - last_t0_) < 0.3 * fs_) return;
    // 8N1: keep the 8 data bits of every 10, skip the remaining sync (3 bytes)
    uint8_t bits[RAW_BITS];
    int nb = 0;
    for (int i = 0; i < RAW_BITS; i++)
        if (i % 10 > 0 && i % 10 < 9) bits[nb++] = f.soft[i] > 0;
    const uint8_t* in = bits + 24;
    const int blocks = (nb - 24) / 64;
    uint8_t nib[2 * FRAME_BYTES] = {0};
    int corrected = 0, n = 0;
    for (int blk = 0; blk < blocks && n < 2 * FRAME_BYTES; blk++) {
        uint8_t d[64];
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++) d[8 * j + i] = in[64 * blk + 8 * i + j];
        for (int c = 0; c < 8 && n < 2 * FRAME_BYTES; c++) {
            uint8_t byte = 0;
            for (int j = 0; j < 8; j++) byte |= d[8 * c + j] << j;
            // nearest codeword (distance <= 1 corrects one error)
            int best = -1, bd = 9;
            for (int v = 0; v < 16; v++) {
                const int dist = __builtin_popcount(HAM[v] ^ byte);
                if (dist < bd) { bd = dist; best = v; }
            }
            if (bd > 1 && n < 2 * CRC_POS) return;    // the checked part must decode
            corrected += bd;
            nib[n++] = static_cast<uint8_t>(best);
        }
    }
    uint8_t b[FRAME_BYTES];
    for (int i = 0; i < FRAME_BYTES; i++) b[i] = static_cast<uint8_t>(nib[2 * i] << 4 | nib[2 * i + 1]);
    // the CRC (or, without one, an error-free first half) and the status bits
    if (!crc_ok(b) && corrected) return;
    if ((u16be(b + 0x2A) & 0x30) != 0x30) return;
    const int32_t tv = i32be(b + 4);
    if (tv < 0 || tv > 235959999) return;
    auto deg = [](int32_t v) { const int d = v / 1000000; return d + (v / 1e6 - d) * 100.0 / 60.0; };
    const double lat = deg(i32be(b + 8)), lon = deg(i32be(b + 12)), alt = i32be(b + 16) / 10.0;
    if (std::fabs(lat) > 90 || std::fabs(lon) > 180 || alt < -400 || alt > 60000) return;
    last_t0_ = f.t0;

    Frame o;
    o.type = "iMet-54";
    o.dc_hz = f.dc_hz;
    o.t_start = f.t_start;
    o.t_end = f.t_end;
    o.serial = std::to_string(u32be(b));
    o.tod = (tv / 10000000) * 3600 + (tv / 100000 % 100) * 60 + (tv % 100000) / 1000.0;
    if (!(lat == 0 && lon == 0)) { o.lat = lat; o.lon = lon; o.alt = alt; }
    // temperature, raw humidity, humidity-sensor temperature: 1e9 = not available
    int na = 0;
    double t = NAN, rh = NAN, trh = NAN;
    if (u32be(b + 0x1C) == 0x4E6E6B28) na++; else { const float v = f32be(b + 0x1C); if (v > -120 && v < 80) t = v; }
    if (u32be(b + 0x20) == 0x4E6E6B28) na++; else rh = std::max(0.0f, std::min(100.0f, f32be(b + 0x20)));
    if (u32be(b + 0x24) == 0x4E6E6B28) na++; else { const float v = f32be(b + 0x24); if (v > -120 && v < 80) trh = v; }
    o.temp = t;
    o.temp_rh = trh;
    // humidity at the air's temperature (sensor reading at its own)
    if (std::isfinite(t) && std::isfinite(trh) && std::isfinite(rh)) o.rh = std::max(0.0, std::min(100.0, rh * svp_hw(trh) / svp_hw(t)));
    // iMet-50: no PTU block
    int sum = 0;
    for (int i = 0x2C; i < 0x52; i++) sum += b[i];
    o.subtype = (sum == 0 && (u16be(b + 0x2A) & 0xF0F) == 0 && na == 3) ? "iMet-50" : "iMet-54";
    if (want_raw) o.raw = hex(b, FRAME_BYTES);
    if (out) out(o);
}

}  // namespace

std::unique_ptr<Type> make_imet54(double fs) { return std::make_unique<Imet54>(fs); }

}  // namespace sonde
