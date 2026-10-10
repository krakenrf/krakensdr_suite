// Lockheed Martin LMS6-403 (and the LMSX-403 variant): 2-FSK 4800 baud
// carrying a rate-1/2, K=7 convolutional code (polynomials 0x4F / 0x6D, the
// second output inverted), bytes LSB first. Once a second a CCSDS-style
// block: attached sync marker 00 58 F3 3F B8, then a Reed-Solomon (255,223)
// codeword (field 0x187, first root 112, root spacing 11). The 223 data
// bytes of consecutive blocks form a stream of 223-byte LMS frames: sync
// 24 54 00 00 (or ..05), serial, frame number, GPS time of week, position,
// velocity, a CRC-16 (poly 0x1021, init 0) over the first 221 bytes. A frame
// usually spans two blocks. LMSX: sync 24 46 05 00 near the start of a
// longer block, the frame inside it, other position / velocity scaling.
// No temperature / humidity in the frames.
//
// Decoding: the known sync bytes are prepended as perfect symbols, so the
// soft-decision Viterbi starts in the right encoder state.
//
// Protocol as documented by the open-source radiosonde community (rs1729/RS
// "lms6Xmod"); independent implementation, validated against the
// radiosonde_auto_rx LMS6-400 test recording (LMSX-403: untested).

#include "kraken_dsp.hpp"
#include "rs.hpp"
#include "sonde.hpp"

#include <cstdio>
#include <deque>
#include <memory>

namespace sonde {
namespace {

const uint8_t ASM[5] = {0x00, 0x58, 0xF3, 0x3F, 0xB8};
constexpr int K = 7;
constexpr uint32_t POLY_A = 0x4F, POLY_B = 0x6D;
constexpr int FRM = 223;
constexpr int LMS6_BYTES = 255 + 1;          // codeword + tail, after the sync
constexpr int LMSX_BYTES = 40 + 255 + 1;     // the LMSX frame can start up to 40 bytes in

// chips of the sync marker (A, not B per data bit), for the Viterbi prefix
std::vector<float> asm_chips() {
    std::vector<float> c;
    uint32_t reg = 0;
    for (uint8_t byte : ASM)
        for (int k = 0; k < 8; k++) {
            reg = ((reg << 1) | ((byte >> k) & 1)) & 0x7F;
            const int a = __builtin_popcount(reg & POLY_A) & 1, b = __builtin_popcount(reg & POLY_B) & 1;
            c.push_back(a ? 1.0f : -1.0f);
            c.push_back(b ? -1.0f : 1.0f);   // transmitted inverted
        }
    return c;
}

uint16_t crc16_0(const uint8_t* d, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(d[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

int32_t i24be(const uint8_t* p) {
    int32_t v = static_cast<int32_t>(u24be(p));
    return v > 0x7FFFFF ? v - 0x1000000 : v;
}

class Lms6 : public Type {
public:
    explicit Lms6(double fs) : fs_(fs), rs_(0x187, 112, 11, 32), pre_(asm_chips()) {
        FskConfig c;
        c.baud = 4800;
        for (size_t i = 16; i < pre_.size(); i++) c.sync.push_back(pre_[i] > 0);   // the chips of 58 F3 3F B8
        c.max_sync_err = 6;
        c.frame_syms = LMS6_BYTES * 16;
        rx_[0].init(c, fs);
        c.frame_syms = LMSX_BYTES * 16;
        c.baud = 4797.8;
        rx_[1].init(c, fs);
        rx_[0].on_frame = [this](const FskFrame& f) { block(f, false); };
        rx_[1].on_frame = [this](const FskFrame& f) { block(f, true); };
    }
    void push(const float* narrow, const float*, size_t n, double t0) override {
        rx_[0].clock(t0);
        rx_[1].clock(t0);
        for (size_t i = 0; i < n; i++) {
            rx_[0].push(narrow[i]);
            rx_[1].push(narrow[i]);
        }
    }
    void reset() override {
        rx_[0].reset();
        rx_[1].reset();
        stream_.clear();
        last_t0_[0] = last_t0_[1] = -1e12;
    }

private:
    void block(const FskFrame& f, bool x);
    void frame(const uint8_t* fr, bool x, float dc);

    double fs_;
    double blk_ts_ = NAN, blk_te_ = NAN;   // the FSK block the frame's last bytes came in (its time span)
    FskRx rx_[2];
    ReedSolomon rs_;
    std::vector<float> pre_;
    std::deque<uint8_t> stream_;      // LMS6: data bytes of consecutive blocks
    double last_t0_[2] = {-1e12, -1e12};
    double prev_block_t0_ = -1e12;
};

void Lms6::block(const FskFrame& f, bool x) {
    const int nbytes = x ? LMSX_BYTES : LMS6_BYTES;
    if (std::fabs(f.t0 - last_t0_[x]) < 0.3 * fs_) return;    // decoded already (another timing phase)
    blk_ts_ = f.t_start;   // the frames completed by this block: its time span
    blk_te_ = f.t_end;
    // soft input for the Viterbi: > 0 = "0"; the second chip of a pair is inverted
    const int nbits = (5 + nbytes) * 8;
    std::vector<float> soft(2 * nbits);
    for (size_t i = 0; i < pre_.size(); i++) soft[i] = (i & 1) ? pre_[i] : -pre_[i];
    for (int i = 0; i < 2 * nbytes * 8; i++) soft[pre_.size() + i] = (i & 1) ? f.soft[i] : -f.soft[i];
    const std::vector<uint8_t> bits = kp::viterbi_decode(soft.data(), nbits, K, {POLY_A, POLY_B}, false);
    std::vector<uint8_t> b(5 + nbytes, 0);
    for (int i = 0; i < nbits; i++) b[i / 8] |= bits[i] << (i % 8);
    if (std::memcmp(b.data(), ASM, 5) != 0) return;

    int pos = 5;   // where the codeword starts
    if (x) {
        // LMSX: the frame sync right after the block sync, or 35 / 40 bytes later
        static const uint8_t SX[4] = {0x24, 0x46, 0x05, 0x00};
        pos = -1;
        for (int o : {0, 35, 40})
            if (std::memcmp(&b[5 + o], SX, 4) == 0) { pos = 5 + o; break; }
        if (pos < 0) return;
    }
    // Reed-Solomon: transmitted highest power first
    uint8_t cw[255];
    for (int j = 0; j < 255; j++) cw[254 - j] = b[pos + j];
    const int corr = rs_.decode(cw);
    if (corr < 0) {
        if (!x) stream_.clear();       // the stream is broken here
        return;
    }
    for (int j = 0; j < 255; j++) b[pos + j] = cw[254 - j];
    last_t0_[x] = f.t0;
    if (x) {
        frame(&b[pos], true, f.dc_hz);
        return;
    }
    // LMS6: append to the stream (blocks are 1 s apart; a gap restarts it)
    if (std::fabs(f.t0 - prev_block_t0_ - fs_) > 0.2 * fs_) stream_.clear();
    prev_block_t0_ = f.t0;
    stream_.insert(stream_.end(), b.begin() + pos, b.begin() + pos + FRM);
    while (stream_.size() >= static_cast<size_t>(FRM)) {
        // find the frame sync 24 54 00 00 / 24 54 00 05
        size_t s = 0;
        while (s + 4 <= stream_.size() &&
               !(stream_[s] == 0x24 && stream_[s + 1] == 0x54 && stream_[s + 2] == 0x00 && (stream_[s + 3] == 0x00 || stream_[s + 3] == 0x05)))
            s++;
        stream_.erase(stream_.begin(), stream_.begin() + static_cast<long>(s));
        if (stream_.size() < static_cast<size_t>(FRM)) break;
        uint8_t fr[FRM];
        for (int i = 0; i < FRM; i++) fr[i] = stream_[i];
        stream_.erase(stream_.begin(), stream_.begin() + FRM);
        frame(fr, false, f.dc_hz);
    }
}

void Lms6::frame(const uint8_t* fr, bool x, float dc) {
    if (crc16_0(fr, 221) != u16be(fr + 221)) return;
    Frame o;
    o.type = "LMS6";
    o.subtype = x ? "LMSX-403" : fr[3] == 0x05 ? "LMS6-403-2" : "LMS6-403";
    o.dc_hz = dc;
    o.t_start = blk_ts_;
    o.t_end = blk_te_;
    o.serial = std::to_string(u32be(fr + 4) & 0xFFFFFF);
    o.frame_no = u16be(fr + 8);
    // GPS time of week (no week number): time of day, UTC
    double tow;
    if (x) {
        uint64_t w = static_cast<uint64_t>(u32be(fr + 10)) << 32 | u32be(fr + 14);
        double d;
        std::memcpy(&d, &w, 8);
        tow = d;
    } else {
        tow = u32be(fr + 10) / 1000.0;
    }
    if (tow >= 0 && tow < 604800) o.tod = std::fmod(tow - GPS_LEAP_S + 86400, 86400);
    const double lat = x ? i32be(fr + 18) / 1e7 : i32be(fr + 18) * 90.0 / (1 << 30);
    const double lon = x ? i32be(fr + 22) / 1e7 : i32be(fr + 22) * 90.0 / (1 << 30);
    const double alt = x ? i32be(fr + 26) / 100.0 : i32be(fr + 26) / 1000.0;
    if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -200 && alt < 60000 && !(lat == 0 && lon == 0)) {
        o.lat = lat; o.lon = lon; o.alt = alt;
        if (x) {
            o.vh = i16be(fr + 30) / 100.0;
            o.heading = i16be(fr + 32) / 100.0;
            o.vv = i16be(fr + 34) / 100.0;
        } else {
            en_to_speed_dir(i24be(fr + 30) / 1000.0, i24be(fr + 33) / 1000.0, &o.vh, &o.heading);
            o.vv = i24be(fr + 36) / 1000.0;
        }
    }
    if (want_raw) o.raw = hex(fr, FRM);
    if (out) out(o);
}

}  // namespace

std::unique_ptr<Type> make_lms6(double fs) { return std::make_unique<Lms6>(fs); }

}  // namespace sonde
