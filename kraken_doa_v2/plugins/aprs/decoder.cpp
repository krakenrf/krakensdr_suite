// APRS / AX.25 1200 bit/s packet radio decoder (Bell 202 AFSK on NBFM).
//
// Physical layer: NBFM carrier, audio-frequency shift keying with mark =
// 1200 Hz, space = 2200 Hz, 1200 baud, continuous phase (Bell 202). Bits are
// NRZI coded (0 = tone change, 1 = no change) and framed as HDLC: 0x7E flags,
// a 0 stuffed after five consecutive 1s, bytes LSB first, FCS = CRC-16/X.25
// (poly 0x1021 reflected, init 0xFFFF, xorout 0xFFFF, sent low byte first).
// Link layer: AX.25 v2.2 UI frames (control 0x03, PID 0xF0) carrying the APRS
// information field (APRS Protocol Reference 1.0.1) - see aprs_parse.hpp.
//
// Demodulator: FM discriminator -> DC removal -> quadrature correlators for
// both tones over one bit -> three slicers with different mark/space weights
// (transmitters with / without pre-emphasis give a tilted tone balance) ->
// per-slicer DPLL bit clock -> NRZI -> HDLC -> CRC. A frame decoded by more
// than one slicer is reported once.

#include "aprs_parse.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr double FS = 19200;   // 16 samples per bit
constexpr int SPB = 16;
constexpr int TABLEN = 96;     // 1200 Hz: 6 cycles, 2200 Hz: 11 cycles in 96 samples
constexpr int MAX_FRAME = 340; // bytes incl. FCS (AX.25 max info 256 + header)
constexpr int MIN_FRAME = 18;  // dst + src + ctl + pid + FCS = 18

uint16_t crc_x25(const uint8_t* p, int n) {
    static const std::array<uint16_t, 256> tab = [] {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            uint16_t c = static_cast<uint16_t>(i);
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) c = (c >> 8) ^ tab[(c ^ p[i]) & 0xFF];
    return c ^ 0xFFFF;
}

// One slicer: decision weights, bit clock, NRZI + HDLC deframer.
class Slicer {
public:
    explicit Slicer(float space_gain) : g_(space_gain) {}

    void reset() {
        prev_ = 0;
        phase_ = 0;
        last_level_ = 0;
        ones_ = 0;
        in_frame_ = false;
        nbits_ = 0;
    }

    // mark/space: tone magnitudes. Returns true when a frame with valid FCS
    // is in frame()/frame_len().
    bool push(float mark, float space) {
        float d = mark - g_ * space;   // > 0: mark
        bool got = false;
        // DPLL: a transition should fall half-way between two sampling points
        if ((d > 0) != (prev_ > 0)) {
            float frac = prev_ / (prev_ - d);           // crossing position, 0..1 sample back
            float pos = phase_ - (1.0f - frac) / SPB;   // phase at the crossing
            float err = pos - 0.5f;
            phase_ -= (in_frame_ ? 0.2f : 0.5f) * err;
        }
        prev_ = d;
        phase_ += 1.0f / SPB;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            got = on_bit(d > 0 ? 1 : 0);
        }
        return got;
    }

    const uint8_t* frame() const { return bytes_; }
    int frame_len() const { return flen_; }

private:
    float g_;
    float prev_ = 0, phase_ = 0;
    int last_level_ = 0;
    int ones_ = 0;
    bool in_frame_ = false;
    int nbits_ = 0;
    uint8_t bits_[MAX_FRAME * 8 + 8];
    uint8_t bytes_[MAX_FRAME];
    int flen_ = 0;

    bool on_bit(int level) {
        int b = level == last_level_ ? 1 : 0;   // NRZI
        last_level_ = level;
        if (b) {
            if (++ones_ > 6) {   // abort / idle
                in_frame_ = false;
                return false;
            }
        } else {
            int o = ones_;
            ones_ = 0;
            if (o == 6) return flag();
            if (o == 5) return false;   // stuffed zero
        }
        if (!in_frame_) return false;
        if (nbits_ >= MAX_FRAME * 8 + 7) {
            in_frame_ = false;
            return false;
        }
        bits_[nbits_++] = static_cast<uint8_t>(b);
        return false;
    }

    // closing / opening flag: the 0 and six 1s of the flag are in bits_
    bool flag() {
        bool ok = false;
        int n = nbits_ - 7;
        if (in_frame_ && n >= MIN_FRAME * 8 && n % 8 == 0) {
            int len = n / 8;
            for (int i = 0; i < len; i++) {
                uint8_t v = 0;
                for (int k = 0; k < 8; k++) v |= bits_[i * 8 + k] << k;
                bytes_[i] = v;
            }
            uint16_t fcs = static_cast<uint16_t>(bytes_[len - 2] | (bytes_[len - 1] << 8));
            if (crc_x25(bytes_, len - 2) == fcs) {
                flen_ = len - 2;
                ok = true;
            }
        }
        in_frame_ = true;
        nbits_ = 0;
        return ok;
    }
};

class Aprs : public kp::Decoder {
public:
    explicit Aprs(kp::Host& h) : Decoder(h), lp_(kp::fir_lowpass(41, 7000, FS)), fm_(FS) {
        for (int i = 0; i < TABLEN; i++) {
            double t = static_cast<double>(i) / FS;
            m_[i] = kp::cf(std::cos(2 * M_PI * 1200 * t), -std::sin(2 * M_PI * 1200 * t));
            s_[i] = kp::cf(std::cos(2 * M_PI * 2200 * t), -std::sin(2 * M_PI * 2200 * t));
        }
        // space weights: flat, TX pre-emphasis (+5 dB space), de-emphasised
        for (float g : {1.0f, 0.55f, 1.8f}) slicers_.emplace_back(g);
        reset();
    }

    void reset() override {
        lp_.reset();
        fm_.reset();
        dc_ = 0;
        idx_ = 0;
        hist_.fill({});
        msum_ = ssum_ = {0, 0};
        recompute_ = 0;
        for (auto& s : slicers_) s.reset();
    }

    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        for (size_t i = 0; i < n; i++) {
            now_ = t0 + static_cast<double>(i) / FS;   // a frame found below ended here
            float f = fm_.push(lp_.push(x[i]));
            dc_ += 0.0005f * (f - dc_);   // carrier offset (time constant ~0.1 s)
            float a = f - dc_;
            int ti = idx_ % TABLEN;
            kp::cf pm = a * m_[ti], ps = a * s_[ti];
            int hi = idx_ % SPB;
            msum_ += pm - hist_[hi][0];
            ssum_ += ps - hist_[hi][1];
            hist_[hi] = {pm, ps};
            if (++idx_ == TABLEN * 1000) idx_ = 0;
            if (++recompute_ >= 4096) {   // limit float drift of the running sums
                recompute_ = 0;
                msum_ = ssum_ = {0, 0};
                for (auto& h : hist_) { msum_ += h[0]; ssum_ += h[1]; }
            }
            float mark = std::abs(msum_), space = std::abs(ssum_);
            for (auto& s : slicers_)
                if (s.push(mark, space)) on_frame(s.frame(), s.frame_len());
        }
    }

private:
    kp::Fir<kp::cf> lp_;
    kp::FmDemod fm_;
    double now_ = 0;   // Host::time() of the sample being processed
    float dc_ = 0;
    std::array<kp::cf, TABLEN> m_, s_;
    std::array<std::array<kp::cf, 2>, SPB> hist_{};
    kp::cf msum_, ssum_;
    int idx_ = 0, recompute_ = 0;
    std::vector<Slicer> slicers_;

    // duplicate suppression across slicers
    uint16_t last_crc_ = 0;
    int last_len_ = 0;
    double last_t_ = -10;

    long frames_ = 0;
    std::set<std::string> stations_;

    // APRS symbol (code) -> map marker
    static const char* marker(char code) {
        switch (code) {
            case '>': case 'k': case 'u': case 'v': case 'j': case '<': case 'b': case 'R': case 'U': case 'f': case 'a':
                return "vehicle";
            case '[': return "person";
            case 's': case 'Y': case 'C': return "ship";
            case '\'': case '^': case 'X': case 'g': return "aircraft";
            case '-': case '#': case '&': case '_': case 'r': case 'y': case 'n': return "station";
            default: return "point";
        }
    }

    // A position report -> the 🗺 Map (an object / item under its own name;
    // a killed object is taken off)
    void map_point(const aprs::Frame& f) {
        const bool obj = f.type.rfind("object", 0) == 0 || f.type.rfind("item", 0) == 0;
        const std::string id = obj && !f.name.empty() ? kp::printable(f.name) : f.src;
        if (f.type.find("killed") != std::string::npos) { host.map_remove(id); return; }
        if (!(std::fabs(f.lat) <= 90 && std::fabs(f.lon) <= 180) || (f.lat == 0 && f.lon == 0)) return;
        kp::MapPoint p;
        p.id = id;
        p.lat = f.lat;
        p.lon = f.lon;
        p.label = id;
        p.kind = marker(f.sym_code);
        // course / speed data extension "ccc/sss" (degrees, knots)
        const std::string& c = f.comment;
        if (c.size() >= 7 && c[3] == '/' && isdigit(static_cast<unsigned char>(c[0])) &&
            isdigit(static_cast<unsigned char>(c[1])) && isdigit(static_cast<unsigned char>(c[2])) &&
            isdigit(static_cast<unsigned char>(c[4])) && isdigit(static_cast<unsigned char>(c[5])) &&
            isdigit(static_cast<unsigned char>(c[6]))) {
            int crs = atoi(c.substr(0, 3).c_str()), spd = atoi(c.substr(4, 3).c_str());
            if (crs >= 1 && crs <= 360) p.heading = static_cast<float>(crs % 360);
            if (spd > 0) p.speed_kmh = spd * 1.852f;
        }
        char pos[48];
        snprintf(pos, sizeof pos, "%.5f, %.5f", f.lat, f.lon);
        p.info = std::string(obj ? "Object: " + id + "\nFrom: " : "Callsign: ") + f.src + "\nType: " + f.type +
                 "\nPath: " + f.dst + (f.path.empty() ? "" : "," + f.path) +
                 (f.sym_table > 32 && f.sym_code > 32 ? std::string("\nSymbol: ") + f.sym_table + f.sym_code : "") +
                 (c.empty() ? "" : "\nComment: " + kp::printable(c.substr(0, 120)));
        p.ttl_s = 3600;
        host.map_point(p);
    }

    void on_frame(const uint8_t* b, int len) {
        uint16_t c = crc_x25(b, len);
        double t = host.time();
        if (c == last_crc_ && len == last_len_ && t - last_t_ < 0.3) return;

        aprs::Frame f;
        int hdr = 0;
        if (!aprs::parse_address(b, len, f, hdr)) return;   // CRC ok but not AX.25
        if (hdr + 1 > len) return;
        uint8_t ctl = b[hdr];
        f.ui = (ctl & 0xEF) == 0x03 && hdr + 2 <= len && b[hdr + 1] == 0xF0;
        if (f.ui) f.info.assign(reinterpret_cast<const char*>(b + hdr + 2), len - hdr - 2);
        last_crc_ = c;
        last_len_ = len;
        last_t_ = t;
        aprs::parse_info(f);

        // the frame's samples: it ended now; len bytes (+ ~2 % bit stuffing) and
        // the two flags at 1200 bit/s before that (Host::valid(start, end) - the
        // Digital squelch)
        host.valid(now_ - (len * 8 * 1.02 + 16) / 1200.0, now_);
        frames_++;
        stations_.insert(f.src);

        std::string head = f.src + ">" + f.dst + (f.path.empty() ? "" : "," + f.path);
        std::string text = head + "  " + f.type;
        char pos[96] = "";
        if (f.has_pos) {
            snprintf(pos, sizeof pos, "%.5f, %.5f", f.lat, f.lon);
            text += std::string(" ") + pos;
            if (f.sym_table > 32 && f.sym_code > 32)
                text += std::string(" [") + f.sym_table + f.sym_code + "]";
        }
        if (!f.name.empty()) text += " " + std::string(f.type == "message" || f.type == "bulletin" ? "to " : "") + kp::printable(f.name);
        if (!f.comment.empty()) text += ": " + kp::printable(f.comment.substr(0, 120));
        if (host.verbose() && !f.ui) {
            char ctlhex[24];
            snprintf(ctlhex, sizeof ctlhex, " (control 0x%02X)", ctl);
            text += ctlhex;
        }
        host.event(text, 1.0);
        // the decoder data log: the packet in TNC2 monitor format
        if (host.raw_wanted()) host.raw(head + ":" + kp::printable(f.info));

        host.fact("Last station", f.src);
        host.fact("Last path", f.dst + (f.path.empty() ? "" : "," + f.path));
        host.fact("Last packet type", f.type);
        if (f.has_pos) {
            host.fact("Last position", f.src + (f.name.empty() ? "" : " (" + kp::printable(f.name) + ")") + ": " + pos);
        }
        host.fact("Last info", kp::printable(f.info.substr(0, 200)));
        if (f.has_pos) map_point(f);
        host.fact("Frames", std::to_string(frames_));
        host.fact("Stations heard", std::to_string(stations_.size()));
        host.freq_error(dc_);
    }
};

}  // namespace

KRAKEN_PLUGIN(Aprs, {.id = "aprs",
                     .name = "APRS (AX.25 1200)",
                     .description = "APRS / AX.25 packet, 1200 bit/s Bell 202 AFSK on NBFM: callsigns, path, positions",
                     .version = "1.0",
                     .sample_rate = FS,
                     .min_vfo_rate = 16000,
                     .author = "AI Signal Lab",
                     .map = true})
