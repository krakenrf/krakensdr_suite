// AIS decoder (marine Automatic Identification System), both channels in one
// VFO: 161.975 MHz (AIS 1 / "A") and 162.025 MHz (AIS 2 / "B") at -25 / +25
// kHz of a 100 kHz VFO on 162.000 MHz (picking the decoder tunes it there).
//
// Physical layer: GMSK, BT 0.4, 9600 bit/s, +-2.4 kHz deviation. Bits are
// NRZI coded (0 = level change, 1 = none) and framed as HDLC: ramp-up, 24-bit
// training sequence 0101..., flag 0x7E, data with a 0 stuffed after five 1s,
// bytes sent LSB first, FCS = CRC-16/X.25, flag. One message = 1..5 slots of
// 26.7 ms (a position report fills one).
//
// Demodulator per channel: mix to 0 Hz -> 8 kHz low-pass, decimated to 50
// kHz -> FM discriminator -> 5.5 kHz low-pass -> eight bit-timing phases
// (5.21 samples per bit, linear interpolation), each with its own slicer
// (DC = running mean of its samples), NRZI and HDLC deframer -> CRC. A frame
// decoded at several phases is reported once.
//
// Every frame's samples are reported as a kp::Talker packet of its MMSI, on
// its channel's frequency: kraken_doa filters that channel out of every
// antenna and gives each ship (base station, aid to navigation) its own DoA.

#include "ais_msg.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using kp::cf;
using kp::Fir;
using kp::FmDemod;
using kp::Nco;
using kp::fir_lowpass;

constexpr double FS = 100000;                // VFO "100 kHz" (2.4 MHz / 24: no resampling)
constexpr int DEC = 2;
constexpr double FS_CH = FS / DEC;           // per channel
constexpr double BAUD = 9600;
constexpr double SPB = FS_CH / BAUD;         // 5.21 samples per bit
constexpr int NPH = 8;                       // bit timing phases
constexpr int LP1_TAPS = 49, LP2_TAPS = 21;
constexpr int MAX_BITS = 5 * 256;            // 5 slots
constexpr int MIN_BITS = 72 + 16;            // shortest message (72 bits) + FCS
// Slicer thresholds (the carrier offset on the discriminator), two banks of
// NPH phases each: a running mean of the slicer's own samples over ~64 bits
// (best on IQ - the offset is steady), and the mean over 48 bits centred on
// the bit (follows baseline wander: discriminator audio from a scanner)
constexpr float DC_ALPHA = 1.0f / 64;
constexpr double DC_WIN = 48 * SPB;
constexpr double VESSEL_TIMEOUT_S = 30 * 60;
constexpr double MAP_TTL_S = 30 * 60;
const double CH_OFF[2] = {-25000, 25000};
const char* CH_NAME[2] = {"A", "B"};
const char* CH_MHZ[2] = {"161.975", "162.025"};
// a channel sample index -> input sample index: the decimating low-pass (its
// centre), the discriminator (between two samples) and the second low-pass
constexpr double CH_DELAY = (LP2_TAPS - 1) / 2.0 + 0.5;
constexpr double IN_DELAY = (LP1_TAPS - 1) / 2.0;

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

uint16_t crc_x25(const uint8_t* p, int n) {
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c ^ 0xFFFF;
}

struct Frame {
    int ch;
    std::vector<uint8_t> bytes;   // without the FCS
    double t_open, t_close;       // channel sample index: end of the opening / closing flag
};

// One bit-timing phase of one channel: slicer + NRZI + HDLC deframer
class Slicer {
public:
    double t = 0;                 // next sampling instant (channel sample index)
    void reset(double t0) {
        t = t0;
        dc_ = 0;
        lvl_ = 0;
        sr_ = 0;
        in_ = false;
        n_ = 0;
    }
    bool centred = false;         // threshold: the centred mean (else its own running mean)
    // one bit sampled at time t: v = the discriminator, dc = its centred mean;
    // true + *f when a frame with a good CRC closed
    bool bit(float v, float dc, int ch, Frame* f) {
        dc_ += DC_ALPHA * (v - dc_);
        const int lvl = v > (centred ? dc : dc_);
        const int b = lvl == lvl_ ? 1 : 0;   // NRZI
        lvl_ = lvl;
        sr_ = static_cast<uint8_t>((sr_ << 1) | b);
        if (sr_ == 0x7E) {   // a flag ends here
            bool got = false;
            if (in_ && n_ - 7 >= MIN_BITS && (n_ - 7) % 8 == 0) got = frame(ch, f);
            in_ = true;
            n_ = ones_ = 0;
            t_open_ = t;
            return got;
        }
        if (!in_) return false;
        if ((sr_ & 0x7F) == 0x7F) {   // seven 1s: abort / idle
            in_ = false;
            return false;
        }
        if (b) {
            ones_++;
            buf_[n_++] = 1;
        } else if (ones_ == 5) {
            ones_ = 0;   // stuffed
        } else {
            ones_ = 0;
            buf_[n_++] = 0;
        }
        if (n_ >= MAX_BITS + 16) in_ = false;
        return false;
    }

private:
    float dc_ = 0;
    int lvl_ = 0;
    uint8_t sr_ = 0;
    bool in_ = false;
    int n_ = 0, ones_ = 0;
    double t_open_ = 0;
    uint8_t buf_[MAX_BITS + 24];

    bool frame(int ch, Frame* f) {
        const int nb = (n_ - 7) / 8;   // the closing flag's first 7 bits went in as data
        if (nb < MIN_BITS / 8) return false;
        std::vector<uint8_t> by(static_cast<size_t>(nb));
        for (int k = 0; k < nb; k++) {
            uint8_t v = 0;
            for (int j = 0; j < 8; j++) v |= static_cast<uint8_t>(buf_[8 * k + j] << j);   // LSB first
            by[static_cast<size_t>(k)] = v;
        }
        const uint16_t fcs = static_cast<uint16_t>(by[static_cast<size_t>(nb - 2)] | (by[static_cast<size_t>(nb - 1)] << 8));
        if (crc_x25(by.data(), nb - 2) != fcs) return false;
        by.resize(static_cast<size_t>(nb - 2));
        f->ch = ch;
        f->bytes = std::move(by);
        f->t_open = t_open_;
        f->t_close = t;
        return true;
    }
};

class Chan {
public:
    int idx = 0;
    Nco nco;
    std::vector<float> h1;
    std::vector<cf> ring;         // doubled ring for the decimating low-pass
    size_t rpos = 0;
    uint64_t nin = 0;             // input samples since reset
    FmDemod fm{static_cast<float>(FS_CH)};
    Fir<float> lp2;
    std::vector<float> d;         // discriminator (Hz), channel samples base.. base + size
    std::vector<double> cs;       // running sum of d: cs[i] = d[0] + .. + d[i-1] (same base)
    std::vector<float> pw;        // power, same indexes (signal level of a frame)
    int64_t base = 0;
    Slicer sl[2 * NPH];
    float noise = 0;              // power floor (running minimum-ish)

    void init(int i) {
        idx = i;
        nco.set(static_cast<float>(CH_OFF[i]), static_cast<float>(FS));
        h1 = fir_lowpass(LP1_TAPS, 8000.0f, static_cast<float>(FS));
        lp2.set_taps(fir_lowpass(LP2_TAPS, 5500.0f, static_cast<float>(FS_CH)));
        reset();
    }
    void reset() {
        ring.assign(2 * h1.size(), cf{});
        rpos = 0;
        nin = 0;
        fm.reset();
        lp2.reset();
        d.clear();
        cs.assign(1, 0.0);
        pw.clear();
        base = 0;
        for (int p = 0; p < 2 * NPH; p++) {
            sl[p].reset(2.0 + (p % NPH) * SPB / NPH);
            sl[p].centred = p >= NPH;
        }
        noise = 0;
    }
    // input samples -> discriminator samples; frames to *out
    void process(const cf* x, size_t n, std::vector<Frame>* out) {
        const size_t L = h1.size();
        for (size_t i = 0; i < n; i++) {
            const cf v = nco.mix(x[i]);
            ring[rpos] = ring[rpos + L] = v;
            rpos = (rpos + 1) % L;
            if (nin++ % DEC) continue;
            cf acc{};
            const cf* w = &ring[rpos];
            for (size_t k = 0; k < L; k++) acc += w[k] * h1[L - 1 - k];
            const float f = std::clamp(fm.push(acc), -12000.0f, 12000.0f);
            d.push_back(lp2.push(f));
            cs.push_back(cs.back() + d.back());
            const float p = std::norm(acc);
            pw.push_back(p);
            noise = noise <= 0 ? p : (p < noise ? noise + 0.01f * (p - noise) : noise + 0.0001f * (p - noise));
        }
        const int64_t end = base + static_cast<int64_t>(d.size());
        Frame fr;
        const int64_t half = static_cast<int64_t>(DC_WIN / 2);
        for (auto& s : sl)
            while (s.t + 1 + static_cast<double>(half) < static_cast<double>(end)) {
                const double tt = s.t;
                const int64_t i0 = static_cast<int64_t>(std::floor(tt));
                if (i0 - half < base) {
                    s.t += SPB;
                    continue;
                }
                const size_t k = static_cast<size_t>(i0 - base);
                const float a = d[k], b = d[k + 1];
                const float v = a + static_cast<float>(tt - static_cast<double>(i0)) * (b - a);
                const double dc = (cs[k + static_cast<size_t>(half) + 1] - cs[k - static_cast<size_t>(half)]) /
                                  static_cast<double>(2 * half + 1);
                if (s.bit(v, static_cast<float>(dc), idx, &fr)) out->push_back(fr);
                s.t += SPB;
            }
        // keep the samples a frame's signal level needs
        const size_t keep = static_cast<size_t>((MAX_BITS + 64) * SPB);
        if (d.size() > 2 * keep) {
            const size_t drop = d.size() - keep;
            d.erase(d.begin(), d.begin() + static_cast<long>(drop));
            cs.erase(cs.begin(), cs.begin() + static_cast<long>(drop));
            pw.erase(pw.begin(), pw.begin() + static_cast<long>(drop));
            base += static_cast<int64_t>(drop);
        }
    }
    // mean power over channel samples [a, b) in dB, relative to full scale
    float level_db(double a, double b) const {
        int64_t i0 = std::max<int64_t>(base, static_cast<int64_t>(a)), i1 = std::min<int64_t>(base + static_cast<int64_t>(pw.size()), static_cast<int64_t>(b));
        if (i1 <= i0) return NAN;
        double s = 0;
        for (int64_t i = i0; i < i1; i++) s += pw[static_cast<size_t>(i - base)];
        return static_cast<float>(10 * std::log10(std::max(1e-12, s / static_cast<double>(i1 - i0))));
    }
};

struct Vessel {
    uint32_t mmsi = 0;
    std::string name, callsign, dest, eta, cls, aton;
    uint32_t imo = 0;
    int type = 0, status = -1, bow = 0, stern = 0, port = 0, stbd = 0;
    float draught = NAN, sog = NAN, cog = NAN, hdg = NAN, rot = NAN, alt = NAN;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    double first = 0, last = 0;
    uint64_t msgs = 0, ch_msgs[2] = {0, 0};
    int last_ch = 0;
    float rssi = NAN;
    bool on_map = false, in_table = false, said_name = false;
    double map_t = -1e9, table_t = -1e9, alert_t = -1e9;
};

class Ais : public kp::Decoder {
public:
    explicit Ais(kp::Host& h) : Decoder(h) {
        for (int i = 0; i < 2; i++) ch_[i].init(i);
        host.table_columns(COLS);
    }

    void reset() override {
        for (auto& c : ch_) c.reset();
        abs0_ = 0;
        for (auto& [m, v] : ves_) forget(v);
        ves_.clear();
        recent_.clear();
        house_t_ = -1e9;
        host.table_columns(COLS);
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "range") range_km_ = std::max(0.0, atof(value.c_str()));
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        blk_abs_ = abs0_;
        std::vector<Frame> frames;
        for (auto& c : ch_) c.process(x, n, &frames);
        abs0_ += n;
        std::sort(frames.begin(), frames.end(), [](const Frame& a, const Frame& b) { return a.t_close < b.t_close; });
        for (auto& f : frames) on_frame(f);
        housekeeping();
    }

private:
    const std::vector<std::string> COLS = {"MMSI", "Name", "Callsign", "Type", "Class", "Status", "SOG kn", "COG °",
                                           "HDG °", "Lat", "Lon", "Distance km", "Bearing °", "Destination",
                                           "Length m", "Ch", "Signal dB", "Messages"};
    Chan ch_[2];
    uint64_t abs0_ = 0, blk_abs_ = 0;   // input samples since reset: before this block / at its first sample
    double now_ = 0;
    double range_km_ = 500;
    std::map<uint32_t, Vessel> ves_;
    struct Seen { int ch; uint16_t crc; double t; };
    std::deque<Seen> recent_;           // frames already reported (several phases decode one)
    uint64_t total_ = 0, nmea_seq_ = 0;
    double house_t_ = -1e9;

    // channel sample index of channel c -> host.time()
    double to_time(double t) const {
        const double in = DEC * (t - CH_DELAY) - IN_DELAY;   // input sample index since reset
        return now_ + (in - static_cast<double>(blk_abs_)) / FS;
    }

    bool station(double* la, double* lo) const { return host.station(la, lo); }

    void on_frame(const Frame& f) {
        const uint16_t crc = crc_x25(f.bytes.data(), static_cast<int>(f.bytes.size()));
        for (const auto& s : recent_)
            if (s.ch == f.ch && s.crc == crc && std::fabs(s.t - f.t_close) < 4 * SPB) return;
        recent_.push_back({f.ch, crc, f.t_close});
        while (recent_.size() > 64) recent_.pop_front();

        std::vector<uint8_t> bits;
        bits.reserve(f.bytes.size() * 8);
        for (uint8_t b : f.bytes)
            for (int j = 7; j >= 0; j--) bits.push_back((b >> j) & 1);
        const ais::Bits m(bits);
        if (m.size() < 38) return;
        const int type = static_cast<int>(m.u(0, 6));
        const uint32_t mmsi = m.u(8, 30);
        if (type < 1 || type > 27 || mmsi == 0) return;
        host.valid();
        total_++;
        // the transmission: training (24 bits) + flag + data + FCS + flag
        const double a = f.t_open - 31.5 * SPB, b = f.t_close + 0.5 * SPB;
        const float lvl = ch_[f.ch].level_db(a, b);

        Vessel& v = vessel(mmsi);
        v.msgs++;
        v.ch_msgs[f.ch]++;
        v.last_ch = f.ch;
        v.last = now_;
        if (std::isfinite(lvl)) v.rssi = std::isfinite(v.rssi) ? v.rssi + 0.3f * (lvl - v.rssi) : lvl;

        std::string what = decode(v, m, type);

        // this transmission's samples: the ship's own DoA (kp::Talker::packet)
        kp::Talker t;
        t.id = std::to_string(mmsi);
        t.label = label(v);
        t.start_s = to_time(a);
        t.end_s = to_time(b);
        t.packet = true;
        t.freq_hz = CH_OFF[f.ch];
        t.bw_hz = 16000;
        t.avg_s = 20;
        host.talker(t);

        if (host.verbose())
            host.event(std::string("[") + CH_NAME[f.ch] + "] type " + std::to_string(type) + " " + std::to_string(mmsi) +
                       (v.name.empty() ? "" : " " + v.name) + (what.empty() ? "" : ": " + what), 0.0);
        if (host.raw_wanted())
            host.raw(ais::nmea(bits, CH_NAME[f.ch], static_cast<int>(nmea_seq_++)) + fmt(" %.1f", lvl));
        host.fact("Last message", std::to_string(mmsi) + (v.name.empty() ? "" : " " + v.name) + " (type " +
                                      std::to_string(type) + ", channel " + CH_NAME[f.ch] + ")");
        update_outputs(v);
    }

    Vessel& vessel(uint32_t mmsi) {
        auto it = ves_.find(mmsi);
        if (it != ves_.end()) return it->second;
        Vessel& v = ves_[mmsi];
        v.mmsi = mmsi;
        v.first = now_;
        const uint32_t pre = mmsi / 1000000;
        if (pre == 970) v.cls = "AIS-SART";
        else if (pre == 972) v.cls = "MOB";
        else if (pre == 974) v.cls = "EPIRB-AIS";
        else if (mmsi / 10000000 == 0) v.cls = "base station";
        else if (mmsi / 10000000 == 99) v.cls = "aid to navigation";
        else if (mmsi / 1000000 == 111) v.cls = "SAR aircraft";
        host.event("New " + std::string(v.cls.empty() ? "vessel" : v.cls) + " " + std::to_string(mmsi), 5.0);
        return v;
    }

    bool pos_ok(double lat, double lon) const {
        double sl, so;
        if (range_km_ > 0 && station(&sl, &so) && distance_km(sl, so, lat, lon) > range_km_) return false;
        return true;
    }
    void set_pos(Vessel& v, double lat, double lon) {
        if (!pos_ok(lat, lon)) return;
        v.lat = lat;
        v.lon = lon;
        v.pos_t = now_;
    }

    // one message -> the vessel; returns a short description (verbose log)
    std::string decode(Vessel& v, const ais::Bits& m, int type) {
        double lat, lon;
        switch (type) {
            case 1: case 2: case 3: {
                if (v.cls.empty() || v.cls == "Class B") v.cls = "Class A";
                v.status = static_cast<int>(m.u(38, 4));
                const int rot = m.s(42, 8);
                v.rot = rot == -128 ? NAN : static_cast<float>((rot < 0 ? -1 : 1) * std::pow(rot / 4.733, 2));
                speed_course(v, m.u(50, 10), m.u(116, 12), m.u(128, 9));
                if (ais::position(m, 61, 89, &lat, &lon)) set_pos(v, lat, lon);
                return ais::nav_status(v.status);
            }
            case 4: case 11:
                if (v.cls.empty()) v.cls = "base station";
                if (ais::position(m, 79, 107, &lat, &lon)) set_pos(v, lat, lon);
                return type == 4 ? "base station report" : "UTC date response";
            case 5:
                if (v.cls.empty() || v.cls == "Class B") v.cls = "Class A";
                v.imo = m.u(40, 30);
                set_name(v, m.text(112, 20), m.text(70, 7));
                v.type = static_cast<int>(m.u(232, 8));
                dims(v, m, 240);
                v.draught = m.u(294, 8) ? m.u(294, 8) / 10.0f : NAN;
                v.dest = m.text(302, 20);
                if (m.u(274, 4) && m.u(278, 5)) {
                    char b[32];
                    snprintf(b, sizeof b, "%02u-%02u %02u:%02u", m.u(274, 4), m.u(278, 5), m.u(283, 5), m.u(288, 6));
                    v.eta = b;
                }
                return "static and voyage data" + (v.dest.empty() ? std::string() : ", to " + v.dest);
            case 9:
                v.cls = "SAR aircraft";
                v.alt = m.u(38, 12) < 4095 ? static_cast<float>(m.u(38, 12)) : NAN;
                v.sog = m.u(50, 10) < 1023 ? static_cast<float>(m.u(50, 10)) : NAN;
                v.cog = m.u(116, 12) < 3600 ? m.u(116, 12) / 10.0f : NAN;
                if (ais::position(m, 61, 89, &lat, &lon)) set_pos(v, lat, lon);
                return "SAR aircraft position";
            case 18:
                if (v.cls.empty()) v.cls = "Class B";
                speed_course(v, m.u(46, 10), m.u(112, 12), m.u(124, 9));
                if (ais::position(m, 57, 85, &lat, &lon)) set_pos(v, lat, lon);
                return "Class B position";
            case 19:
                if (v.cls.empty()) v.cls = "Class B";
                speed_course(v, m.u(46, 10), m.u(112, 12), m.u(124, 9));
                if (ais::position(m, 57, 85, &lat, &lon)) set_pos(v, lat, lon);
                set_name(v, m.text(143, 20), "");
                v.type = static_cast<int>(m.u(263, 8));
                dims(v, m, 271);
                return "Class B extended position";
            case 21:
                v.cls = "aid to navigation";
                v.aton = ais::aton_type(static_cast<int>(m.u(38, 5)));
                set_name(v, m.text(43, 20), "");
                if (ais::position(m, 164, 192, &lat, &lon)) set_pos(v, lat, lon);
                return v.aton;
            case 24:
                if (v.cls.empty()) v.cls = "Class B";
                if (m.u(38, 2) == 0) {
                    set_name(v, m.text(40, 20), "");
                    return "static data A";
                }
                v.type = static_cast<int>(m.u(40, 8));
                set_name(v, "", m.text(90, 7));
                dims(v, m, 132);
                return "static data B";
            case 27:
                if (v.cls.empty()) v.cls = "Class A";
                v.status = static_cast<int>(m.u(40, 4));
                v.sog = m.u(79, 6) < 63 ? static_cast<float>(m.u(79, 6)) : NAN;
                v.cog = m.u(85, 9) < 511 ? static_cast<float>(m.u(85, 9)) : NAN;
                if (ais::position(m, 44, 62, &lat, &lon, 18, 17, 600.0)) set_pos(v, lat, lon);
                return "long-range position";
            default:
                return "";
        }
    }

    static void speed_course(Vessel& v, uint32_t sog, uint32_t cog, uint32_t hdg) {
        v.sog = sog < 1023 ? sog / 10.0f : NAN;
        v.cog = cog < 3600 ? cog / 10.0f : NAN;
        v.hdg = hdg < 360 ? static_cast<float>(hdg) : NAN;
    }
    static void dims(Vessel& v, const ais::Bits& m, int at) {
        v.bow = static_cast<int>(m.u(at, 9));
        v.stern = static_cast<int>(m.u(at + 9, 9));
        v.port = static_cast<int>(m.u(at + 18, 6));
        v.stbd = static_cast<int>(m.u(at + 24, 6));
    }
    void set_name(Vessel& v, const std::string& name, const std::string& call) {
        if (!name.empty()) v.name = name;
        if (!call.empty()) v.callsign = call;
        if (!v.said_name && !v.name.empty()) {
            v.said_name = true;
            host.event(std::to_string(v.mmsi) + " is " + v.name + (v.callsign.empty() ? "" : " (" + v.callsign + ")"), 5.0);
        }
    }

    static double distance_km(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
        const double h = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
        return 6371 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
    }
    static double bearing(double la1, double lo1, double la2, double lo2) {
        const double r = M_PI / 180;
        const double y = std::sin((lo2 - lo1) * r) * std::cos(la2 * r);
        const double x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos((lo2 - lo1) * r);
        return std::fmod(std::atan2(y, x) / r + 360, 360);
    }

    std::string label(const Vessel& v) const { return !v.name.empty() ? v.name : v.callsign; }
    std::string type_text(const Vessel& v) const {
        if (!v.aton.empty()) return v.aton;
        std::string t = ais::ship_type(v.type);
        return t;
    }
    std::string kind(const Vessel& v) const {
        if (v.cls == "base station") return "station";
        if (v.cls == "aid to navigation") return "point";
        if (v.cls == "SAR aircraft") return "aircraft";
        if (v.cls == "AIS-SART" || v.cls == "MOB" || v.cls == "EPIRB-AIS") return "person";
        return "ship";
    }
    static std::string num(float v, const char* f) { return std::isfinite(v) ? fmt(f, v) : ""; }

    std::string info_text(const Vessel& v) const {
        std::string s = "MMSI: " + std::to_string(v.mmsi);
        if (!v.name.empty()) s += "\nName: " + v.name;
        if (!v.callsign.empty()) s += "\nCallsign: " + v.callsign;
        if (v.imo) s += "\nIMO: " + std::to_string(v.imo);
        if (!v.cls.empty()) s += "\nClass: " + v.cls;
        const std::string tt = type_text(v);
        if (!tt.empty()) s += "\nType: " + tt;
        if (v.status >= 0 && v.status != 15) s += std::string("\nStatus: ") + ais::nav_status(v.status);
        if (std::isfinite(v.sog)) s += "\nSpeed: " + fmt("%.1f kn", v.sog);
        if (std::isfinite(v.cog)) s += "\nCourse: " + fmt("%.1f°", v.cog);
        if (std::isfinite(v.hdg)) s += "\nHeading: " + fmt("%.0f°", v.hdg);
        if (std::isfinite(v.rot) && v.rot != 0) s += "\nRate of turn: " + fmt("%.0f°/min", v.rot);
        if (std::isfinite(v.alt)) s += "\nAltitude: " + fmt("%.0f m", v.alt);
        if (v.bow + v.stern > 0) s += "\nSize: " + std::to_string(v.bow + v.stern) + " x " + std::to_string(v.port + v.stbd) + " m";
        if (std::isfinite(v.draught)) s += "\nDraught: " + fmt("%.1f m", v.draught);
        if (!v.dest.empty()) s += "\nDestination: " + v.dest;
        if (!v.eta.empty()) s += "\nETA: " + v.eta + " UTC";
        s += std::string("\nChannel: ") + CH_NAME[v.last_ch] + " (" + CH_MHZ[v.last_ch] + " MHz)";
        if (std::isfinite(v.rssi)) s += "\nSignal: " + fmt("%.1f dBFS", v.rssi);
        s += "\nMessages: " + std::to_string(v.msgs);
        return s;
    }

    void update_outputs(Vessel& v) {
        // distress beacons / active SART status: once every 10 min
        const bool sart = v.cls == "AIS-SART" || v.cls == "MOB" || v.cls == "EPIRB-AIS" || v.status == 14;
        if (sart && now_ - v.alert_t > 600) {
            v.alert_t = now_;
            std::string w = "⚠ " + std::string(v.cls == "MOB" ? "MOB beacon" : v.cls == "EPIRB-AIS" ? "EPIRB" : "AIS-SART") +
                            " active: " + std::to_string(v.mmsi) + (v.name.empty() ? "" : " " + v.name);
            if (std::isfinite(v.lat)) w += fmt(" at %.5f", v.lat) + fmt(", %.5f", v.lon);
            host.event(w, 30.0);
        }
        const bool has_pos = std::isfinite(v.lat) && now_ - v.pos_t < MAP_TTL_S;
        if (has_pos && now_ - v.map_t >= 1.0) {
            v.map_t = now_;
            kp::MapPoint p;
            p.id = std::to_string(v.mmsi);
            p.lat = v.lat;
            p.lon = v.lon;
            p.label = label(v).empty() ? p.id : label(v);
            p.kind = kind(v);
            p.heading = std::isfinite(v.hdg) ? v.hdg : (std::isfinite(v.sog) && v.sog > 0.5f ? v.cog : NAN);
            if (std::isfinite(v.sog)) p.speed_kmh = v.sog * 1.852f;
            if (std::isfinite(v.alt)) p.altitude_m = v.alt;
            p.info = info_text(v);
            p.ttl_s = MAP_TTL_S;
            host.map_point(p);
            v.on_map = true;
        }
        if (now_ - v.table_t >= 1.0) {
            v.table_t = now_;
            v.in_table = true;
            host.table_row(std::to_string(v.mmsi), cells(v));
        }
    }

    std::vector<std::string> cells(const Vessel& v) const {
        std::string dist, brg;
        double sl, so;
        if (std::isfinite(v.lat) && station(&sl, &so)) {
            dist = fmt("%.1f", distance_km(sl, so, v.lat, v.lon));
            brg = fmt("%.0f", bearing(sl, so, v.lat, v.lon));
        }
        return {std::to_string(v.mmsi), v.name, v.callsign, type_text(v), v.cls,
                v.status >= 0 && v.status != 15 ? ais::nav_status(v.status) : "",
                num(v.sog, "%.1f"), num(v.cog, "%.1f"), num(v.hdg, "%.0f"),
                std::isfinite(v.lat) ? fmt("%.5f", v.lat) : "", std::isfinite(v.lon) ? fmt("%.5f", v.lon) : "",
                dist, brg, v.dest, v.bow + v.stern > 0 ? std::to_string(v.bow + v.stern) : "",
                CH_NAME[v.last_ch], num(v.rssi, "%.1f"), std::to_string(v.msgs)};
    }

    void forget(Vessel& v) {
        const std::string id = std::to_string(v.mmsi);
        if (v.on_map) host.map_remove(id);
        if (v.in_table) host.table_remove(id);
    }

    void housekeeping() {
        if (now_ - house_t_ < 5.0) return;
        house_t_ = now_;
        for (auto it = ves_.begin(); it != ves_.end();) {
            if (now_ - it->second.last > VESSEL_TIMEOUT_S) {
                forget(it->second);
                it = ves_.erase(it);
            } else {
                ++it;
            }
        }
        uint64_t a = 0, b = 0;
        for (const auto& [m, v] : ves_) {
            a += v.ch_msgs[0];
            b += v.ch_msgs[1];
        }
        host.fact("Stations heard", std::to_string(ves_.size()));
        host.fact("Messages", std::to_string(total_) + " (A " + std::to_string(a) + ", B " + std::to_string(b) + ")");
    }
};

}  // namespace

KRAKEN_PLUGIN(Ais, {.id = "ais",
                    .name = "AIS (ships)",
                    .description = "Marine AIS on both channels (161.975 / 162.025 MHz, GMSK 9600 bit/s): MMSI, name, "
                                   "callsign, type, position, speed, course, destination; base stations, aids to "
                                   "navigation, SAR aircraft, AIS-SART / MOB / EPIRB. Picking it tunes the VFO to "
                                   "162.000 MHz at 100 kHz; ships appear on the 🗺 Map, each with its own DoA",
                    .version = "1.0",
                    .sample_rate = FS,
                    .min_vfo_rate = FS,
                    .author = "KrakenSDR",
                    .options = {{"range", "Max range", "500", "100=100 km|200=200 km|500=500 km|0=No limit",
                                 "Positions further than this from the station (Station Information) are rejected "
                                 "as decoding errors"}},
                    .map = true,
                    .manual_only = true,
                    .fixed_freq_hz = 162.0e6,
                    .talkers = true})
