// Lockheed Martin LMS6-403 (family "LMS6").
//
// 2-FSK 4800 symbols/s, continuous, CCSDS concatenated coding: the rate-1/2
// K = 7 convolutional code (G1 = 171, G2 = 133 inverted) over a stream of
// ASM 1A CF FC 1D + an RS(255,223) codeword + a zero tail byte (codeword bytes
// into the encoder LSB first). The zero tail puts the encoder in state 0
// before every ASM, so the ASM's 64 channel symbols are fixed: they are the
// sync, and the Viterbi decoder starts from the known tail + ASM. The 223 data
// bytes of consecutive codewords form one byte stream holding the frames
// ("$T" 00 00, 223 bytes, CRC-16). Standards: CCSDS 131.0-B; frame facts from
// ra-firmware (DF9DQ, BSD-3-Clause). A second receiver looks for the LMSX-403's
// frame inside its codeword (the old plugin's layout, untested: README).

#include "families.hpp"

#include "kraken_dsp.hpp"

#include <algorithm>
#include <cstring>

namespace sonde {

namespace {

const char* LMS6_ASM_SYMBOLS = "0101011000001000000111001001011100011010101001110011110100111110";
constexpr uint32_t ASM = 0x1ACFFC1D;
constexpr int NSYM = 4096;                   // codeword + tail: 256 bytes x 16 symbols
constexpr int LMSX_BYTES = 40 + 255 + 1;
constexpr int LMSX_NSYM = LMSX_BYTES * 16;
const std::vector<uint32_t> POLYS = {0x4F, 0x6D};   // G1, G2 (kp:: tap order: bit 0 = newest)

class Lms6 : public Family {
public:
    Lms6() {
        FskParams p;
        p.rate = 4800;
        p.sync = LMS6_ASM_SYMBOLS;
        p.max_err = 6;
        p.nsym = NSYM;
        p.phases = 8;
        rx_.init(p);
        p.rate = 4797.8;
        p.nsym = LMSX_NSYM;
        rxx_.init(p);
        // the tail + ASM through the encoder, as perfect soft values
        uint8_t bits[40] = {};
        for (int i = 0; i < 32; i++) bits[8 + i] = ASM >> (31 - i) & 1;
        const auto enc = kp::conv_encode(bits, 40, 7, POLYS);
        prefix_.resize(enc.size());
        for (size_t i = 0; i < enc.size(); i++) prefix_[i] = enc[i] ? -1.0f : 1.0f;
    }

    void reset() override {
        rx_.reset();
        rxx_.reset();
        dup_.reset();
        dupx_.reset();
        stream_.clear();
        last_cw_ = -1e18;
    }

    void process(const float* narrow, const float*, size_t n, int64_t idx, const Ctx& ctx, std::vector<Record>& out) override {
        caps_.clear();
        rx_.process(narrow, n, idx, caps_);
        for (const auto& c : caps_) {
            if (dup_.near(c.pos0, 0.3)) continue;
            if (codeword(c, ctx, out)) dup_.accept(c.pos0);
        }
        caps_.clear();
        rxx_.process(narrow, n, idx, caps_);
        for (const auto& c : caps_) {
            if (dupx_.near(c.pos0, 0.3)) continue;
            lmsx(c, ctx, out);
        }
    }

private:
    FskRx rx_, rxx_;
    ReedSolomon rs_{0x187, 112, 11, 32};
    std::vector<FskCapture> caps_;
    DupWindow dup_, dupx_;
    std::vector<float> prefix_;
    std::vector<uint8_t> stream_;            // RS data bytes of consecutive codewords
    double last_cw_ = -1e18;                 // first-symbol position of the last good codeword

    // Viterbi over tail + ASM + the capture; false unless the ASM comes out exactly
    bool viterbi(const FskCapture& c, std::vector<uint8_t>& bytes) {
        const size_t n = c.sym.size();
        std::vector<float> soft(prefix_.size() + n);
        std::copy(prefix_.begin(), prefix_.end(), soft.begin());
        for (size_t i = 0; i < n; i += 2) {
            soft[prefix_.size() + i] = -c.sym[i];          // G1 as sent
            soft[prefix_.size() + i + 1] = c.sym[i + 1];   // G2 was inverted
        }
        const int nbits = static_cast<int>(soft.size() / 2);
        const auto bits = kp::viterbi_decode(soft.data(), nbits, 7, POLYS, false);
        uint32_t a = 0;
        for (int i = 0; i < 32; i++) a = a << 1 | bits[static_cast<size_t>(8 + i)];
        if (a != ASM) return false;
        bytes.assign(static_cast<size_t>((nbits - 40) / 8), 0);
        for (size_t j = 0; j < bytes.size(); j++) {
            unsigned v = 0;
            for (int b = 0; b < 8; b++) v |= static_cast<unsigned>(bits[40 + 8 * j + static_cast<size_t>(b)]) << b;   // LSB first
            bytes[j] = static_cast<uint8_t>(v);
        }
        return true;
    }

    // RS(255,223) of 255 bytes, the first = the highest-order coefficient
    bool rs255(uint8_t* b) const {
        uint8_t cw[255];
        for (int j = 0; j < 255; j++) cw[254 - j] = b[j];
        if (rs_.decode(cw, 255) < 0) return false;
        for (int j = 0; j < 255; j++) b[j] = cw[254 - j];
        return true;
    }

    bool codeword(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        std::vector<uint8_t> by;
        // a codeword that fails is dropped; the next good one then comes too
        // late after the last and starts the stream afresh (another timing
        // phase of the same codeword may still decode)
        if (!viterbi(c, by) || by.size() < 255 || !rs255(by.data())) return false;
        const double gap = (c.pos0 - last_cw_) / FS;
        if (!(gap > 0.8 && gap < 1.2)) stream_.clear();
        last_cw_ = c.pos0;
        stream_.insert(stream_.end(), by.begin(), by.begin() + 223);
        frames(c, ctx, out);
        return true;
    }

    void frames(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        size_t i = 0;
        while (i + 4 <= stream_.size()) {
            const uint8_t* s = stream_.data() + i;
            if (!(s[0] == 0x24 && s[1] == 0x54 && s[2] == 0x00 && (s[3] == 0x00 || s[3] == 0x05))) {
                i++;
                continue;
            }
            if (i + 223 > stream_.size()) break;   // the rest comes with the next codeword
            if (crc16_xmodem(s, 221) != u16be(s + 221)) {
                i++;
                continue;
            }
            frame(s, c, ctx, out);
            i += 223;
        }
        stream_.erase(stream_.begin(), stream_.begin() + static_cast<long>(std::min(i, stream_.size())));
        if (stream_.size() > 4 * 223) stream_.erase(stream_.begin(), stream_.end() - 3);
    }

    static void frame(const uint8_t* s, const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        Record r;
        r.family = "LMS6";
        r.model = s[3] == 0x05 ? "LMS6-403-2" : "LMS6-403";
        r.serial = std::to_string(u32be(s + 4) & 0xFFFFFF);
        r.frame = u16be(s + 8);
        double tod = std::fmod(u32be(s + 10) / 1000.0 - GPS_UTC_LEAP_S, 86400.0);
        if (tod < 0) tod += 86400;
        r.tod = tod;
        const double lat = static_cast<int32_t>(u32be(s + 18)) * (180.0 / 2147483648.0);
        const double lon = static_cast<int32_t>(u32be(s + 22)) * (180.0 / 2147483648.0);
        const double alt = static_cast<int32_t>(u32be(s + 26)) / 1000.0;
        if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -200 && alt < 60000 && !(lat == 0 && lon == 0)) {
            r.lat = lat;
            r.lon = lon;
            r.alt = alt;
            const double ve = i24(u24be(s + 30)) / 1000.0, vn = i24(u24be(s + 33)) / 1000.0;
            speed_dir(ve, vn, &r.vh, &r.hdg);
            r.vv = i24(u24be(s + 36)) / 1000.0;   // mm/s (the recording's altitude agrees)
        }
        set_span(r, c, ctx);
        if (ctx.raw) r.hex = hex_upper(s, 223);
        out.push_back(std::move(r));
    }

    // LMSX-403: the frame sync 24 46 05 00 at 0, 35 or 40 bytes after the ASM,
    // the RS codeword from there, the frame inside it
    void lmsx(const FskCapture& c, const Ctx& ctx, std::vector<Record>& out) {
        std::vector<uint8_t> by;
        if (!viterbi(c, by)) return;
        dupx_.accept(c.pos0);   // a coded stream: its other timing phases are the same
        for (int off : {0, 35, 40}) {
            if (static_cast<size_t>(off + 255) > by.size()) continue;
            uint8_t* s = by.data() + off;
            if (!(s[0] == 0x24 && s[1] == 0x46 && s[2] == 0x05 && s[3] == 0x00)) continue;
            if (!rs255(s)) continue;
            Record r;
            r.family = "LMS6";
            r.model = "LMSX-403";
            r.serial = std::to_string(u32be(s + 4) & 0xFFFFFF);
            uint64_t u = 0;
            for (int k = 0; k < 8; k++) u = u << 8 | s[10 + k];
            double tow;
            std::memcpy(&tow, &u, 8);
            if (std::isfinite(tow) && tow >= 0) {
                if (tow >= 604800) tow /= 1000;   // ms
                if (tow < 604800) {
                    double tod = std::fmod(tow - GPS_UTC_LEAP_S, 86400.0);
                    r.tod = tod < 0 ? tod + 86400 : tod;
                }
            }
            const double lat = static_cast<int32_t>(u32be(s + 18)) * 1e-7, lon = static_cast<int32_t>(u32be(s + 22)) * 1e-7;
            const double alt = static_cast<int32_t>(u32be(s + 26)) / 100.0;
            if (std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && alt > -200 && alt < 60000 && !(lat == 0 && lon == 0)) {
                r.lat = lat;
                r.lon = lon;
                r.alt = alt;
                r.vh = static_cast<int16_t>(u16be(s + 30)) / 100.0;
                r.hdg = u16be(s + 32) / 100.0;
                r.vv = static_cast<int16_t>(u16be(s + 34)) / 100.0;
            }
            set_span(r, c, ctx);
            if (ctx.raw) r.hex = hex_upper(s, 255);
            out.push_back(std::move(r));
            return;
        }
    }
};

}  // namespace

std::unique_ptr<Family> make_lms6() { return std::make_unique<Lms6>(); }

}  // namespace sonde
