// FLARM / OGN tracker decoder - glider and light aircraft collision
// avoidance radios on the 868 / 869 / 915 MHz licence-free bands, for a VFO
// of the full 2.4 MHz (lock_vfo): every channel of the regional band plans
// that lies inside it is listened to at once.
//
// Radio (both protocols, as publicly documented): GFSK, 100 kchip/s, +-50
// kHz, Manchester coded (50 kbit/s; a 1 = chips 0 1), ~5 ms per packet.
//   FLARM (legacy radio packet): sync ...F531FAB6, 24 bytes, CRC-16 (CCITT,
//     init 0xFFFF, over 31 FA B6 + the 24 bytes). Bytes 0-2 are the 24-bit
//     radio ID (little-endian), byte 3 its address type - in the clear. The
//     other 20 bytes (position, altitude, speeds, aircraft type, privacy
//     flags) are encrypted with keys FLARM has not published: NOT decoded
//     here (no attempt is made) - a FLARM aircraft is shown by its ID and its
//     bearing only.
//   OGN tracker (the Open Glider Network's open protocol): sync 0AF3656C, 20
//     bytes + 6 LDPC(208,160) parity bytes, position whitened with TEA under
//     the published all-zero key - decoded (ogn.cpp), unless the owner
//     encrypted it.
// Privacy: an OGN packet with the stealth flag (OGN's "don't track me") is
// never put on the map, in the table's position columns or the data log -
// it keeps its bearing and a "no-track" note.
//
// Front end: a 256-point FFT every 256 samples (9.4 kHz bins, 107 us); the
// power in +-75 kHz around each channel against its noise floor finds the
// bursts; a burst of plausible length is mixed down from a 2.4 MHz history,
// low-passed and decimated to 400 kHz (4 samples per chip), FM-demodulated,
// summed over each chip and searched for both syncs (either polarity).
//
// ID separation: every packet the aircraft sent ITSELF (OGN relay count 0)
// is a kp::Talker packet of its address ("FLR DD1234", "OGN 3C5A21", "ICA
// 4CA2B1"), on its channel (freq_hz = the burst's measured carrier, bw_hz
// 160 kHz): kraken_doa filters that channel out of every antenna, so
// aircraft on different hop channels at the same moment keep their own
// bearings. Relayed OGN packets name nobody who sent these samples: no
// talker.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "ogn.hpp"

#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double FS = 2.4e6;
constexpr int NFFT = 256;
constexpr double BIN_HZ = FS / NFFT;
constexpr int DEC = 6;
constexpr double FS2 = FS / DEC;                 // 400 kHz
constexpr int SPC = 4;                           // samples per chip at FS2
constexpr int RING = 1 << 17;                    // 2.4 MHz history: 55 ms
constexpr int SYNC_CHIPS = 56;                   // the last 7 sync bytes' chips
constexpr int DATA_BYTES = 26;                   // FLARM 24 + CRC 2, OGN 20 + 6
constexpr int DATA_CHIPS = DATA_BYTES * 16;
constexpr int MAX_SYNC_ERR = 6;
constexpr double HALF_WIN_HZ = 75e3;             // detection window
constexpr double MIN_BURST_S = 2.0e-3, MAX_BURST_S = 14e-3;
constexpr double EDGE_HZ = 1.05e6;               // channels further from the VFO centre are not used
constexpr double AC_TIMEOUT_S = 1800;
constexpr double TALKER_BW = 160e3;

// FLARM: ...F 5 3 1 F A B 6 (the nRF905 address 31 FA B6 after the
// preamble), OGN: 0 A F 3 6 5 6 C; the last 7 bytes' chips are matched
constexpr uint32_t SYNC_FLARM = 0xF531FAB6, SYNC_OGN = 0x0AF3656C;

const std::vector<std::string> TABLE_COLS = {"Aircraft", "Protocol", "Aircraft type", "Registration", "Altitude m",
                                             "Climb m/s", "Speed km/h", "Track °", "Lat", "Lon", "Distance km",
                                             "Bearing °", "Notes", "MHz", "Signal dBFS", "Packets"};

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string hex6(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}
std::string hexs(const uint8_t* p, int n) {
    static const char H[] = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < n; i++) {
        s += H[p[i] >> 4];
        s += H[p[i] & 15];
    }
    return s;
}

inline float fast_atan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    const float a = mn / (mx + 1e-30f), s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0) r = 3.14159274f - r;
    return y < 0 ? -r : r;
}

double distance_km(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180, dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    const double a = std::sin(dla / 2) * std::sin(dla / 2) +
                     std::cos(la1 * r) * std::cos(la2 * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
double bearing_deg(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180, dlo = (lo2 - lo1) * r;
    const double y = std::sin(dlo) * std::cos(la2 * r);
    const double x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos(dlo);
    return std::fmod(std::atan2(y, x) / r + 360, 360);
}

uint16_t crc16_ccitt(uint16_t c, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        c ^= static_cast<uint16_t>(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021) : static_cast<uint16_t>(c << 1);
    }
    return c;
}

// Manchester chips (+1 / -1) of the last 7 bytes of a 32-bit sync: bit 1 = chips -1 +1
std::array<float, SYNC_CHIPS> sync_chips(uint32_t sync) {
    std::array<float, SYNC_CHIPS> t{};
    for (int i = 0; i < 28; i++) {
        const int b = (sync >> (27 - i)) & 1;
        t[static_cast<size_t>(2 * i)] = b ? -1.f : 1.f;
        t[static_cast<size_t>(2 * i + 1)] = b ? 1.f : -1.f;
    }
    return t;
}

// the regional channel plans (Hz): EU / Africa 868.2 + 868.4, New Zealand
// 869.25, Russia 868.8, India 866.0, Israel 916.2, Korea 920.9, and the
// 400 kHz grid from 902.2 MHz that North America (902.2-927.8, 65 channels)
// and Australia / South America (917.0-926.2, 24 channels) hop on
std::vector<double> plan_channels() {
    std::vector<double> c = {868.2e6, 868.4e6, 869.25e6, 868.8e6, 866.0e6, 920.9e6};
    for (int k = 0; k < 65; k++) c.push_back(902.2e6 + 0.4e6 * k);
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end()), c.end());
    return c;
}

struct Band {
    const char* key;
    const char* name;
    double tune_hz;   // receiver centre that puts the band's channels inside 2.4 MHz
};
const Band BANDS[] = {
    {"eu", "Europe / Africa / New Zealand (868.2, 868.4, 869.25 MHz)", 868.8e6},
    {"nz", "New Zealand (869.25 MHz; 868.2 / 868.4 too)", 868.8e6},
    {"au", "Australia / South America (917-926.2 MHz hopping)", 921.6e6},
    {"us", "North America (902-928 MHz hopping)", 915.2e6},
    {"il", "Israel (916.2 MHz)", 916.6e6},
};

struct Win {
    double f = 0;            // centre, Hz from the VFO centre
    int bins[24];
    int nb = 0;
    float floor = 0;
    int init = 0;
    bool on = false;
    uint64_t start = 0, last = 0;
    float peak = 0;
};

struct Cand {
    int win;
    uint64_t f0, f1;         // FFT frames
    float snr;
    uint64_t made;
};

struct Aircraft {
    std::string id;
    bool flarm = false, ogn = false;
    int addr_type = 0;
    double first = 0, last = 0;
    uint64_t packets = 0, relayed = 0;
    float rssi = -99;
    double mhz = 0;
    // OGN
    bool stealth = false, encrypted = false, emergency = false;
    int acft_type = -1;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    int alt_m = 0, baro_alt_m = 0;
    bool has_alt = false, has_baro = false;
    double climb = NAN, speed = NAN, heading = NAN, turn = NAN;
    std::map<std::string, std::string> info;
    std::string status;
    double table_t = -1e9, map_t = -1e9;
    bool on_map = false, said_emerg = false;
};

class Flarm : public kp::Decoder {
public:
    explicit Flarm(kp::Host& h) : Decoder(h) {
        ring_.assign(RING, kp::cf(0, 0));
        fin_ = fftwf_alloc_complex(NFFT);
        fout_ = fftwf_alloc_complex(NFFT);
        plan_ = fftwf_plan_dft_1d(NFFT, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
        for (int i = 0; i < NFFT; i++) hann_[i] = 0.5f - 0.5f * std::cos(2 * M_PI * (i + 0.5) / NFFT);
        // channel filter for the 400 kHz demodulator: windowed sinc, 110 kHz
        const int nt = 49;
        lp_.resize(nt);
        double s = 0;
        for (int k = 0; k < nt; k++) {
            const double m = k - (nt - 1) / 2.0, x = 2 * 110e3 / FS;
            const double sinc = m == 0 ? x : std::sin(M_PI * x * m) / (M_PI * m);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * k / (nt - 1)) + 0.08 * std::cos(4 * M_PI * k / (nt - 1));
            lp_[static_cast<size_t>(k)] = static_cast<float>(sinc * w);
            s += sinc * w;
        }
        for (auto& v : lp_) v = static_cast<float>(v / s);
        tf_ = sync_chips(SYNC_FLARM);
        to_ = sync_chips(SYNC_OGN);
        plans_ = plan_channels();
        host.table_columns(TABLE_COLS);
        host.table_keep(AC_TIMEOUT_S);
    }
    ~Flarm() override {
        fftwf_destroy_plan(plan_);
        fftwf_free(fin_);
        fftwf_free(fout_);
    }

    void reset() override {
        abs_ = 0;
        fill_ = 0;
        frame_ = 0;
        wins_.clear();
        cands_.clear();
        recent_.clear();
        ac_.clear();
        rf_used_ = NAN;
        host.table_columns(TABLE_COLS);
        summary_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "band") {
            if (value != band_) tune_asked_ = false;
            band_ = value.empty() ? "auto" : value;
        } else if (key == "tune") {
            tune_ = value != "0";
        } else if (key == "sens") {
            const double db = value.empty() ? 6 : std::atof(value.c_str());
            thr_ = static_cast<float>(std::pow(10.0, std::clamp(db, 3.0, 20.0) / 10));
        } else if (key == "extra") {
            extra_hz_ = std::atof(value.c_str()) * 1e6;
            rf_used_ = NAN;   // rebuild the windows
        }
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        blk_abs_ = abs_;
        setup_windows();
        for (size_t i = 0; i < n; i++) ring_[(abs_ + i) & (RING - 1)] = x[i];
        ring_end_ = abs_ + n;
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, static_cast<size_t>(NFFT - fill_));
            for (size_t k = 0; k < take; k++) {
                const kp::cf v = x[i + k] * hann_[fill_ + k];
                fin_[fill_ + k][0] = v.real();
                fin_[fill_ + k][1] = v.imag();
            }
            fill_ += static_cast<int>(take);
            i += take;
            if (fill_ == NFFT) {
                fill_ = 0;
                frame();
            }
        }
        abs_ += n;
        housekeeping();
    }

private:
    std::vector<kp::cf> ring_;
    fftwf_complex *fin_, *fout_;
    fftwf_plan plan_;
    float hann_[NFFT];
    float pw_[NFFT];
    std::vector<float> lp_;
    std::array<float, SYNC_CHIPS> tf_{}, to_{};
    std::vector<double> plans_;
    uint64_t abs_ = 0, blk_abs_ = 0, frame_ = 0, ring_end_ = 0;
    int fill_ = 0;
    std::vector<Win> wins_;
    std::vector<Cand> cands_;
    std::vector<std::pair<uint64_t, double>> recent_;   // decided bursts: start frame, channel
    double rf_used_ = NAN, now_ = 0, extra_hz_ = 0;
    std::string band_ = "auto";
    bool tune_ = true, tune_asked_ = false;
    float thr_ = 3.98f;      // 6 dB
    std::unordered_map<std::string, Aircraft> ac_;
    uint64_t n_flarm_ = 0, n_ogn_ = 0, n_bursts_ = 0, n_broadband_ = 0;
    double summary_t_ = -1e9;
    std::string channels_text_;
    bool grid_ = false;
    const char* dump_dir_ = getenv("FLARM_DUMP");

    std::string band_key() const {
        if (band_ != "auto") return band_;
        double la, lo;
        if (host.station(&la, &lo)) {
            if (la < -30 && la > -50 && lo > 164 && lo < 180) return "nz";
            if (la < -9 && la > -45 && lo > 110 && lo < 160) return "au";
            if (lo < -30 && lo > -170) return la < -20 ? "au" : "us";   // South America: 915-928 MHz like Australia
            if (lo > 34 && lo < 36 && la > 29 && la < 34) return "il";
        }
        return "eu";
    }

    // the detection windows: every plan channel inside the VFO (from the RF),
    // or a 100 kHz grid when the RF is unknown
    void setup_windows() {
        const double rf = host.rf_hz();
        if (std::isfinite(rf) == std::isfinite(rf_used_) && (!std::isfinite(rf) || std::fabs(rf - rf_used_) < 1e3) &&
            !wins_.empty())
            return;
        rf_used_ = rf;
        wins_.clear();
        cands_.clear();
        recent_.clear();
        std::vector<double> offs;
        grid_ = !std::isfinite(rf);
        if (grid_) {
            for (int k = -10; k <= 10; k++) offs.push_back(k * 100e3);
        } else {
            for (double c : plans_)
                if (std::fabs(c - rf) <= EDGE_HZ) offs.push_back(c - rf);
            if (extra_hz_ > 0 && std::fabs(extra_hz_ - rf) <= EDGE_HZ) offs.push_back(extra_hz_ - rf);
        }
        channels_text_.clear();
        for (double f : offs) {
            Win w;
            w.f = f;
            const int c = static_cast<int>(std::lround(f / BIN_HZ));
            const int h = static_cast<int>(HALF_WIN_HZ / BIN_HZ);
            for (int b = c - h; b <= c + h && w.nb < 24; b++) w.bins[w.nb++] = (b + NFFT) % NFFT;
            wins_.push_back(w);
            if (!grid_) channels_text_ += (channels_text_.empty() ? "" : ", ") + fmt("%.3f", (rf + f) / 1e6);
        }
        if (grid_) channels_text_ = "every 100 kHz of the VFO (the RF is unknown)";
        if (!grid_ && offs.empty()) {
            channels_text_ = "none - no FLARM / OGN channel inside the VFO";
            if (tune_ && !tune_asked_) {
                tune_asked_ = true;
                const std::string key = band_key();
                for (const Band& b : BANDS)
                    if (key == b.key) {
                        if (host.tune(b.tune_hz))
                            host.event(fmt("Tuning to %.3f MHz: ", b.tune_hz / 1e6) + b.name, 30.0);
                        break;
                    }
            }
        }
        host.fact("Channels", channels_text_);
    }

    void frame() {
        fftwf_execute(plan_);
        for (int b = 0; b < NFFT; b++) pw_[b] = fout_[b][0] * fout_[b][0] + fout_[b][1] * fout_[b][1];
        const uint64_t f = frame_++;
        for (size_t wi = 0; wi < wins_.size(); wi++) {
            Win& w = wins_[wi];
            float e = 0;
            for (int k = 0; k < w.nb; k++) e += pw_[w.bins[k]];
            if (w.init < 64) {   // learn the floor (median-ish: the lower of start / now)
                w.floor = w.init == 0 ? e : std::min(w.floor * 1.05f + 1e-30f, std::max(e, w.floor * 0.5f));
                w.init++;
                continue;
            }
            const bool act = e > w.floor * thr_;
            if (!act && e < w.floor * 2.0f) w.floor += 0.004f * (e - w.floor);
            else if (!act) w.floor += 0.0005f * (e - w.floor);
            if (act) {
                if (!w.on) {
                    w.on = true;
                    w.start = f;
                    w.peak = e;
                }
                w.last = f;
                w.peak = std::max(w.peak, e);
            } else if (w.on && f > w.last + 2) {
                w.on = false;
                const double dur = (w.last - w.start + 1) * NFFT / FS;
                if (dur >= MIN_BURST_S && dur <= MAX_BURST_S)
                    cands_.push_back({static_cast<int>(wi), w.start, w.last, w.peak / std::max(w.floor, 1e-30f), f});
            }
            if (w.on && (f - w.start) * NFFT / FS > MAX_BURST_S + 0.01) w.on = false;   // a carrier, not a packet
        }
        // decide 4 frames after a burst ended: the strongest of the bursts
        // that overlap in time within 250 kHz is demodulated; bursts that
        // start together (+-2 frames) on 3+ channels spanning >= 600 kHz are
        // one broadband event (the calibration noise source after a retune,
        // impulse noise), not packets
        while (!recent_.empty() && recent_.front().first + 400 < f) recent_.erase(recent_.begin());
        for (size_t ci = 0; ci < cands_.size();) {
            Cand& c = cands_[ci];
            if (f < c.made + 4) { ci++; continue; }
            const double fc = wins_[static_cast<size_t>(c.win)].f;
            bool best = true;
            int together = 0;
            double lo = fc, hi = fc;
            auto with = [&](uint64_t f0, double fr) {
                if (f0 + 2 < c.f0 || f0 > c.f0 + 2) return;
                together++;
                lo = std::min(lo, fr);
                hi = std::max(hi, fr);
            };
            for (const Cand& o : cands_) {
                if (&o == &c) continue;
                with(o.f0, wins_[static_cast<size_t>(o.win)].f);
                if (o.f1 < c.f0 || o.f0 > c.f1) continue;
                if (std::fabs(wins_[static_cast<size_t>(o.win)].f - fc) > 250e3) continue;
                if (o.snr > c.snr) best = false;
            }
            for (const auto& r : recent_) with(r.first, r.second);
            if (together >= 2 && hi - lo >= 600e3) n_broadband_++;
            else if (best) demod(c);
            recent_.push_back({c.f0, fc});
            cands_.erase(cands_.begin() + static_cast<long>(ci));
        }
    }

    double time_of_abs(double a) const { return now_ + (a - static_cast<double>(blk_abs_)) / FS; }

    void demod(const Cand& c) {
        n_bursts_++;
        const Win& w = wins_[static_cast<size_t>(c.win)];
        const int64_t a0 = static_cast<int64_t>(c.f0) * NFFT - 3 * NFFT, a1 = static_cast<int64_t>(c.f1 + 1) * NFFT + 3 * NFFT;
        const int64_t oldest = static_cast<int64_t>(ring_end_) - RING + 64;
        if (a0 < oldest || a0 < 0) return;
        const int ntaps = static_cast<int>(lp_.size());
        const int nout = static_cast<int>((a1 - a0 - ntaps) / DEC);
        const bool long_enough = nout >= SPC * (SYNC_CHIPS + DATA_CHIPS) + 8;
        if (!long_enough && !dump_dir_) return;
        // mix down (phasor recursion) + decimate by 6
        static thread_local std::vector<kp::cf> mixed, y;
        static thread_local std::vector<float> d, m;
        const size_t nin = static_cast<size_t>(a1 - a0);
        mixed.resize(nin);
        const double wv = -2 * M_PI * w.f / FS;
        std::complex<double> ph = std::polar(1.0, wv * static_cast<double>(a0)), st = std::polar(1.0, wv);
        for (size_t i = 0; i < nin; i++) {
            mixed[i] = ring_[(static_cast<uint64_t>(a0) + i) & (RING - 1)] * kp::cf(static_cast<float>(ph.real()), static_cast<float>(ph.imag()));
            ph *= st;
            if ((i & 1023) == 1023) ph /= std::abs(ph);
        }
        y.resize(static_cast<size_t>(nout));
        for (int o = 0; o < nout; o++) {
            const kp::cf* p = mixed.data() + static_cast<size_t>(o) * DEC;
            float re = 0, im = 0;
            for (int k = 0; k < ntaps; k++) {
                re += lp_[static_cast<size_t>(k)] * p[k].real();
                im += lp_[static_cast<size_t>(k)] * p[k].imag();
            }
            y[static_cast<size_t>(o)] = kp::cf(re, im);
        }
        d.assign(static_cast<size_t>(nout), 0.f);
        for (int o = 1; o < nout; o++) {
            const kp::cf z = y[static_cast<size_t>(o)] * std::conj(y[static_cast<size_t>(o - 1)]);
            d[static_cast<size_t>(o)] = fast_atan2(z.imag(), z.real());
        }
        m.assign(static_cast<size_t>(nout), 0.f);
        for (int o = 0; o + SPC <= nout; o++) m[static_cast<size_t>(o)] = d[static_cast<size_t>(o)] + d[static_cast<size_t>(o + 1)] + d[static_cast<size_t>(o + 2)] + d[static_cast<size_t>(o + 3)];
        // sync search: both protocols, both polarities
        const int last = nout - SPC * (SYNC_CHIPS + DATA_CHIPS) - SPC;
        struct Best { float c = 0; int n = -1; } bf, bo;
        for (int n = 1; n < last; n++) {
            float cf = 0, co = 0;
            for (int k = 0; k < SYNC_CHIPS; k++) {
                const float v = m[static_cast<size_t>(n + SPC * k)];
                cf += tf_[static_cast<size_t>(k)] * v;
                co += to_[static_cast<size_t>(k)] * v;
            }
            if (std::fabs(cf) > std::fabs(bf.c)) bf = {cf, n};
            if (std::fabs(co) > std::fabs(bo.c)) bo = {co, n};
        }
        const float level = 10.0f * std::log10(std::max(mean_power(y, std::max(bf.n, bo.n), SPC * (SYNC_CHIPS + DATA_CHIPS)), 1e-12f));
        if (dump_dir_) {
            // FLARM_DUMP=dir: every demodulated burst - its 400 kHz baseband
            // to dir/burst_N.cf32 and a log line (offline / live diagnosis)
            const double full = SYNC_CHIPS * SPC * 0.785;   // |correlation| of a clean sync
            char fn[512];
            snprintf(fn, sizeof fn, "%s/burst_%05llu_%.3f.cf32", dump_dir_, static_cast<unsigned long long>(n_bursts_),
                     std::isfinite(rf_used_) ? (rf_used_ + w.f) / 1e6 : w.f / 1e6);
            if (FILE* fo = fopen(fn, "wb")) {
                fwrite(y.data(), sizeof(kp::cf), y.size(), fo);
                fclose(fo);
            }
            host.log(fmt("burst %.0f", static_cast<double>(n_bursts_)) + fmt(" t=%.3f", time_of_abs(static_cast<double>(a0))) +
                     fmt(" f=%.3f MHz", std::isfinite(rf_used_) ? (rf_used_ + w.f) / 1e6 : w.f / 1e6) +
                     fmt(" dur=%.1f ms", (c.f1 - c.f0 + 1) * NFFT / FS * 1e3) +
                     fmt(" snr=%.1f dB", 10 * std::log10(std::max(c.snr, 1e-3f))) + fmt(" flarm=%.2f", std::fabs(bf.c) / full) +
                     fmt(" ogn=%.2f", std::fabs(bo.c) / full) + fmt(" level=%.1f", level));
        }
        if (!long_enough) return;
        const bool ogn_first = std::fabs(bo.c) > std::fabs(bf.c);
        for (int pass = 0; pass < 2; pass++) {
            const bool is_ogn = (pass == 0) == ogn_first;
            const Best& b = is_ogn ? bo : bf;
            if (b.n < 0) continue;
            const auto& tpl = is_ogn ? to_ : tf_;
            const float pol = b.c > 0 ? 1.f : -1.f;
            // chip errors on the sync (DC = the mean over the sync)
            float dc = 0;
            for (int k = 0; k < SYNC_CHIPS; k++) dc += m[static_cast<size_t>(b.n + SPC * k)];
            dc /= SYNC_CHIPS;
            int err = 0;
            for (int k = 0; k < SYNC_CHIPS; k++)
                err += ((m[static_cast<size_t>(b.n + SPC * k)] - dc) * pol > 0) != (tpl[static_cast<size_t>(k)] > 0);
            if (err > MAX_SYNC_ERR) continue;
            for (int dn : {0, -1, 1}) {
                const int n0 = b.n + dn;
                if (try_packet(is_ogn, n0, pol, m, d, a0, w, level, c.snr)) return;
            }
        }
    }

    static float mean_power(const std::vector<kp::cf>& y, int a, int n) {
        float s = 0;
        int k = 0;
        for (int i = std::max(a, 0); i < a + n && i < static_cast<int>(y.size()); i++, k++) s += std::norm(y[static_cast<size_t>(i)]);
        return k ? s / k : 0;
    }

    bool try_packet(bool is_ogn, int n0, float pol, const std::vector<float>& m, const std::vector<float>& d, int64_t a0,
                    const Win& w, float level, float snr) {
        const int base = n0 + SPC * SYNC_CHIPS;
        float soft[DATA_BYTES * 8];
        uint8_t pkt[DATA_BYTES] = {};
        float amp = 0;
        for (int i = 0; i < DATA_BYTES * 8; i++) {
            const float c1 = m[static_cast<size_t>(base + SPC * 2 * i)], c2 = m[static_cast<size_t>(base + SPC * (2 * i + 1))];
            soft[i] = pol * (c2 - c1);          // > 0: chips "0 1" = a 1
            amp += std::fabs(soft[i]);
            if (soft[i] > 0) pkt[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
        }
        amp /= DATA_BYTES * 8;
        // the burst's carrier: the discriminator's mean over the packet (Manchester has no DC)
        double dsum = 0;
        const int e = base + SPC * DATA_CHIPS;
        for (int i = n0; i < e; i++) dsum += d[static_cast<size_t>(i)];
        const double off_hz = dsum / (e - n0) * FS2 / (2 * M_PI);
        // the packet's samples: its first sync chip .. its last chip (2.4 MHz
        // input index: decimator output o is centred on a0 + 6 o + 24)
        const double s_abs = static_cast<double>(a0) + DEC * (n0 - 0.5) + (lp_.size() - 1) / 2.0;
        const double e_abs = s_abs + DEC * SPC * (SYNC_CHIPS + DATA_CHIPS);
        const double ts = time_of_abs(s_abs + DEC * SPC * 0.5), te = time_of_abs(e_abs - DEC * SPC * 0.5);
        const double mhz = std::isfinite(rf_used_) ? (rf_used_ + w.f) / 1e6 : NAN;
        if (!is_ogn) {
            // CRC over the nRF905 address bytes 31 FA B6 + 24 bytes; also
            // the inverted payload (a radio's opposite Manchester convention)
            static const uint8_t addr[3] = {0x31, 0xFA, 0xB6};
            for (int inv = 0; inv < 2; inv++) {
                uint8_t q[DATA_BYTES];
                for (int i = 0; i < DATA_BYTES; i++) q[i] = inv ? static_cast<uint8_t>(~pkt[i]) : pkt[i];
                const uint16_t crc = crc16_ccitt(crc16_ccitt(0xFFFF, addr, 3), q, 24);
                if (crc != ((q[24] << 8) | q[25])) continue;
                n_flarm_++;
                host.valid(ts, te);
                on_flarm(q, ts, te, w.f + off_hz, level, mhz, snr, inv);
                return true;
            }
            return false;
        }
        int fixed = 0;
        if (ogn::ldpc_errors(pkt) != 0) {
            float llr[208];
            const float sc = 2.0f / std::max(amp, 1e-6f);
            // LDPC bit i = byte i / 8, bit i % 8 from the LSB; on air MSB first
            for (int i = 0; i < 208; i++) llr[i] = -soft[(i >> 3) * 8 + (7 - (i & 7))] * sc;
            fixed = ogn::ldpc_decode(llr, pkt);
            if (fixed < 0 || fixed > 16) return false;
        }
        ogn::Packet p;
        if (!ogn::decode(pkt, p)) return false;
        n_ogn_++;
        host.valid(ts, te);
        on_ogn(p, pkt, ts, te, w.f + off_hz, level, mhz, fixed);
        return true;
    }

    Aircraft& get(const std::string& id, int addr_type) {
        Aircraft& a = ac_[id];
        if (a.packets == 0 && a.relayed == 0) {
            a.id = id;
            a.addr_type = addr_type;
            a.first = now_;
        }
        return a;
    }

    void heard(Aircraft& a, float level, double mhz) {
        a.last = now_;
        a.rssi = a.packets == 0 ? level : a.rssi + 0.2f * (level - a.rssi);
        a.packets++;
        if (std::isfinite(mhz)) a.mhz = mhz;
    }

    void talker(const Aircraft& a, double ts, double te, double f_hz) {
        kp::Talker t;
        t.id = a.id;
        t.label = label(a);
        t.start_s = ts;
        t.end_s = te;
        t.packet = true;
        t.freq_hz = f_hz;
        t.bw_hz = TALKER_BW;
        t.avg_s = 10;
        t.keep_s = 3600;
        // the position it sent with this packet - for the array calibration -
        // only when it may be tracked and has a 3-D fix
        if (a.ogn && !a.stealth && std::isfinite(a.lat) && now_ - a.pos_t < 0.5 && a.has_alt) {
            t.lat = a.lat;
            t.lon = a.lon;
            t.alt_m = a.alt_m;
        }
        host.talker(t);
    }

    std::string label(const Aircraft& a) const {
        std::string s;
        auto it = a.info.find("Registration");
        if (it == a.info.end()) it = a.info.find("Competition ID");
        if (it != a.info.end()) s = it->second;
        if (a.ogn && !a.stealth && a.has_alt && now_ - a.pos_t < 120)
            s += (s.empty() ? "" : " · ") + fmt("%.0f m", a.alt_m);
        if (a.acft_type > 0) s += (s.empty() ? "" : " · ") + std::string(ogn::acft_type_name(a.acft_type));
        if (a.stealth) s += (s.empty() ? "" : " · ") + std::string("no-track");
        if (s.empty()) s = a.flarm ? "FLARM" : "OGN";
        return s;
    }

    void on_flarm(const uint8_t* q, double ts, double te, double f_hz, float level, double mhz, float snr, int inv) {
        const uint32_t addr = q[0] | (q[1] << 8) | (q[2] << 16);
        const int at = (q[3] >> 4) & 3;
        const std::string id = std::string(ogn::addr_prefix(at)) + " " + hex6(addr);
        Aircraft& a = get(id, at);
        const bool fresh = a.packets == 0;
        a.flarm = true;
        heard(a, level, mhz);
        if (fresh)
            host.event("New FLARM aircraft " + id + (std::isfinite(mhz) ? fmt(" on %.3f MHz", mhz) : "") +
                           " - its position is encrypted (not decoded): ID and bearing only", 5.0);
        talker(a, ts, te, f_hz);
        if (host.raw_wanted())
            host.raw("FLARM " + hexs(q, 26) + (std::isfinite(mhz) ? fmt(" %.3f", mhz) : "") + fmt(" %.1f", level));
        if (host.verbose())
            host.event("FLARM " + id + " type byte " + hexs(q + 3, 1) + (inv ? " (inverted)" : "") +
                           (std::isfinite(mhz) ? fmt(" %.3f MHz", mhz) : "") + fmt(" %.1f dBFS", level) +
                           fmt(" %.0f dB", 10 * std::log10(std::max(snr, 1e-3f))), 0.0);
        outputs(a);
    }

    void on_ogn(const ogn::Packet& p, const uint8_t* pkt, double ts, double te, double f_hz, float level, double mhz,
                int fixed) {
        const std::string id = std::string(ogn::addr_prefix(p.addr_type)) + " " + hex6(p.addr);
        Aircraft& a = get(id, p.addr_type);
        const bool fresh = a.packets == 0 && a.relayed == 0;
        a.ogn = true;
        a.encrypted = p.encrypted;
        if (p.relay) {
            // relayed by another tracker: not this aircraft's own signal
            a.relayed++;
            a.last = now_;
        } else {
            heard(a, level, mhz);
        }
        if (!p.encrypted && !p.non_pos) {
            a.stealth = p.stealth;
            a.acft_type = p.acft_type;
            if (p.pos) {
                a.lat = p.lat;
                a.lon = p.lon;
                a.pos_t = now_;
                a.alt_m = p.alt_m;
                a.has_alt = p.fix_mode == 1;
                a.has_baro = p.has_baro;
                a.baro_alt_m = p.baro_alt_m;
                a.speed = p.speed_ms;
                a.heading = p.heading;
                a.climb = p.has_climb ? p.climb_ms : NAN;
                a.turn = p.has_turn ? p.turn_dps : NAN;
            }
        } else if (!p.encrypted && p.report_type == 1) {
            for (const auto& [k, v] : p.info) a.info[k] = kp::printable(v);
        } else if (!p.encrypted && p.report_type == 0) {
            a.status = "firmware " + std::to_string(p.firmware) + ", hardware " + std::to_string(p.hardware) +
                       fmt(", battery %.2f V", p.voltage) + fmt(", %.0f dBm", p.tx_dbm) +
                       fmt(", %.0f satellites", p.satellites);
        }
        if (p.emergency && !a.said_emerg) {
            a.said_emerg = true;
            host.event("⚠ EMERGENCY flag from " + id + (a.stealth || !std::isfinite(a.lat) ? "" : where(a)), 30.0);
        }
        if (fresh)
            host.event("New OGN tracker " + id + (p.addr_type == 2 ? " (FLARM address)" : "") +
                           (p.encrypted ? " - encrypted by its owner: ID and bearing only"
                            : p.stealth ? " - no-track (stealth): its position is not shown" : "") +
                           (p.relay ? " (relayed)" : "") + (std::isfinite(mhz) ? fmt(" on %.3f MHz", mhz) : ""), 5.0);
        if (!p.relay) talker(a, ts, te, f_hz);
        if (host.raw_wanted())
            host.raw("OGN " + hexs(pkt, 26) + (std::isfinite(mhz) ? fmt(" %.3f", mhz) : "") + fmt(" %.1f", level));
        if (host.verbose()) {
            std::string s = "OGN " + id;
            if (p.encrypted) s += " encrypted";
            else if (!p.non_pos)
                s += (a.stealth ? std::string(" no-track") : fmt(" %.5f", p.lat) + fmt(",%.5f", p.lon)) +
                     fmt(" %.0f m", p.alt_m) + " " + ogn::acft_type_name(p.acft_type);
            else s += " report " + std::to_string(p.report_type);
            if (p.relay) s += " relayed";
            if (fixed) s += " (" + std::to_string(fixed) + " bits corrected)";
            host.event(s + fmt(" %.1f dBFS", level), 0.0);
        }
        outputs(a);
    }

    std::string where(const Aircraft& a) const {
        return fmt(" at %.4f", a.lat) + fmt(", %.4f", a.lon) + (a.has_alt ? fmt(", %.0f m", a.alt_m) : "");
    }

    void outputs(Aircraft& a) {
        const bool trackable = a.ogn && !a.stealth && !a.encrypted;
        const bool pos = trackable && std::isfinite(a.lat) && now_ - a.pos_t < 300;
        if (pos && now_ - a.map_t >= 1.0) {
            a.map_t = now_;
            kp::MapPoint p;
            p.id = a.id;
            p.lat = a.lat;
            p.lon = a.lon;
            p.label = label(a).substr(0, 40);
            const int t = a.acft_type;
            p.kind = (t == 4 || t == 6 || t == 7) ? "person" : (t == 11 || t == 12) ? "balloon" : t == 14 ? "vehicle"
                   : t == 15 ? "point" : "aircraft";
            p.heading = std::isfinite(a.heading) ? static_cast<float>(a.heading) : NAN;
            if (a.has_alt) p.altitude_m = static_cast<float>(a.alt_m);
            if (std::isfinite(a.speed)) p.speed_kmh = static_cast<float>(a.speed * 3.6);
            std::string info = "ID: " + a.id + "\nProtocol: OGN tracker";
            if (a.acft_type >= 0) info += std::string("\nAircraft type: ") + ogn::acft_type_name(a.acft_type);
            for (const auto& [k, v] : a.info) info += "\n" + k + ": " + v;
            if (a.has_alt) info += fmt("\nAltitude: %.0f m (GNSS)", a.alt_m);
            if (a.has_baro) info += fmt("\nPressure altitude: %.0f m", a.baro_alt_m);
            if (std::isfinite(a.climb)) info += fmt("\nClimb: %+.1f m/s", a.climb);
            if (std::isfinite(a.speed)) info += fmt("\nGround speed: %.0f km/h", a.speed * 3.6);
            if (std::isfinite(a.heading)) info += fmt("\nTrack: %.0f°", a.heading);
            if (std::isfinite(a.turn)) info += fmt("\nTurn rate: %+.1f °/s", a.turn);
            if (a.mhz > 0) info += fmt("\nChannel: %.3f MHz", a.mhz);
            info += fmt("\nSignal: %.1f dBFS", a.rssi);
            p.info = info;
            p.ttl_s = 300;
            host.map_point(p);
            a.on_map = true;
        } else if (a.on_map && !trackable) {
            host.map_remove(a.id);
            a.on_map = false;
        }
        if (now_ - a.table_t < 1.0) return;
        a.table_t = now_;
        double slat, slon, dist = NAN, brg = NAN;
        if (pos && host.station(&slat, &slon)) {
            dist = distance_km(slat, slon, a.lat, a.lon);
            brg = bearing_deg(slat, slon, a.lat, a.lon);
        }
        std::string proto = a.flarm && a.ogn ? "FLARM + OGN" : a.flarm ? "FLARM" : "OGN";
        std::string notes;
        if (a.flarm && !a.ogn) notes = "position encrypted (not decoded)";
        if (a.encrypted) notes = "encrypted by its owner";
        if (a.stealth) notes = "no-track (stealth)";
        if (a.relayed) notes += std::string(notes.empty() ? "" : "; ") + std::to_string(a.relayed) + " relayed";
        auto reg = a.info.find("Registration");
        auto cn = a.info.find("Competition ID");
        std::string r = reg != a.info.end() ? reg->second : "";
        if (cn != a.info.end()) r += (r.empty() ? "" : " / ") + cn->second;
        const bool show = pos;
        host.table_row(a.id, {a.id, proto, a.acft_type >= 0 ? ogn::acft_type_name(a.acft_type) : "", r,
                              show && a.has_alt ? fmt("%.0f", a.alt_m) : "",
                              show && std::isfinite(a.climb) ? fmt("%+.1f", a.climb) : "",
                              show && std::isfinite(a.speed) ? fmt("%.0f", a.speed * 3.6) : "",
                              show && std::isfinite(a.heading) ? fmt("%.0f", a.heading) : "",
                              show ? fmt("%.5f", a.lat) : "", show ? fmt("%.5f", a.lon) : "",
                              std::isfinite(dist) ? fmt("%.1f", dist) : "", std::isfinite(brg) ? fmt("%.0f", brg) : "",
                              notes, a.mhz > 0 ? fmt("%.3f", a.mhz) : "", fmt("%.1f", a.rssi),
                              std::to_string(a.packets)});
        std::vector<std::pair<std::string, std::string>> det = {{"ID", a.id}, {"Protocol", proto}};
        for (const auto& [k, v] : a.info) det.push_back({k, v});
        if (!a.status.empty()) det.push_back({"Status", a.status});
        if (show && a.has_baro) det.push_back({"Pressure altitude", fmt("%.0f m", a.baro_alt_m)});
        if (show && std::isfinite(a.turn)) det.push_back({"Turn rate", fmt("%+.1f °/s", a.turn)});
        if (a.flarm)
            det.push_back({"FLARM payload", "encrypted with keys FLARM has not published - not decoded (ID and DoA only)"});
        if (a.stealth) det.push_back({"Privacy", "the tracker asks not to be tracked: no position shown, mapped or logged"});
        host.table_detail(a.id, det);
    }

    void housekeeping() {
        if (now_ - summary_t_ < 2.0) return;
        summary_t_ = now_;
        int nf = 0, no = 0;
        for (auto it = ac_.begin(); it != ac_.end();) {
            Aircraft& a = it->second;
            if (now_ - a.last > AC_TIMEOUT_S) {
                if (a.on_map) host.map_remove(a.id);
                host.table_remove(a.id);
                it = ac_.erase(it);
                continue;
            }
            if (a.on_map && now_ - a.pos_t > 300) {
                host.map_remove(a.id);
                a.on_map = false;
            }
            (a.ogn ? no : nf)++;
            ++it;
        }
        host.fact("Aircraft", std::to_string(nf) + " FLARM, " + std::to_string(no) + " OGN tracker");
        host.fact("Packets", std::to_string(n_flarm_) + " FLARM, " + std::to_string(n_ogn_) + " OGN (" +
                                 std::to_string(n_bursts_) + " bursts demodulated, " + std::to_string(n_broadband_) +
                                 " broadband events skipped)");
        host.fact("Channels", channels_text_);
    }
};

}  // namespace

KRAKEN_PLUGIN(Flarm, {.id = "flarm",
                      .name = "FLARM / OGN trackers (868 / 915 MHz)",
                      .description = "Glider / light aircraft collision avoidance radios: FLARM (shown by its radio ID "
                                     "and bearing - its position is encrypted and not decoded) and OGN trackers "
                                     "(open protocol: position, altitude, climb, aircraft type, registration). The VFO "
                                     "sits on its tuner's centre with the full bandwidth and every channel of the band "
                                     "plans inside it is received (EU / NZ 868.2, 868.4, 869.25 MHz: tune to 868.8 MHz). "
                                     "Each aircraft gets its own DoA in the 🗺 Map's 📡 DF panel",
                      .version = "1.0",
                      .sample_rate = FS,
                      .min_vfo_rate = FS,
                      .author = "KrakenSDR",
                      .options = {{"band", "Band", "auto",
                                   "auto=From the station location|eu=Europe / Africa (868 MHz)|nz=New Zealand (869.25 MHz)|"
                                   "au=Australia / S. America (917-926 MHz)|us=North America (902-928 MHz)|il=Israel (916.2 MHz)",
                                   "Where the receiver is tuned when no FLARM / OGN channel is inside the VFO (every "
                                   "known channel inside it is received whatever this says). 868.8 MHz covers Europe's "
                                   "and New Zealand's channels; the hopping bands get 5-6 of their channels"},
                                  {"tune", "Tune to the band", "1", "1=On|0=Off",
                                   "Retune the receiver (once) to the band when the VFO holds none of its channels"},
                                  {"sens", "Burst detection", "6", "4=4 dB (sensitive)|6=6 dB|9=9 dB|12=12 dB (strong only)",
                                   "How far above the noise a channel's power must rise to demodulate a burst"},
                                  {"extra", "Extra channel MHz", "", "",
                                   "A channel of another band plan to listen to as well (MHz, inside the VFO)"}},
                      .map = true,
                      .manual_only = true,
                      .talkers = true,
                      .lock_vfo = true,
                      .tunes = true})
