// UAT decoder (978 MHz Universal Access Transceiver, RTCA DO-282B), for a
// VFO of the full 2.4 MHz bandwidth - the US's second ADS-B link (general
// aviation below 18000 ft) and its ground stations' uplink (FIS-B weather /
// NOTAMs, TIS-B traffic).
//
// Physical layer: binary CPFSK at 1.041667 Mbit/s, modulation index 0.6 (a 1
// = +312.5 kHz), so 2.304 samples per bit at 2.4 MHz. A message starts with
// a 36-bit sync - 0xEACDDA4E2 for ADS-B, its complement for the ground
// uplink - and is protected by Reed-Solomon codes over GF(256) (field
// polynomial 0x187, roots alpha^120..): basic ADS-B RS(30,18), long ADS-B
// RS(48,34), uplink 6 x RS(92,72) byte-interleaved.
//
// Demodulator: power in 32-sample blocks against the noise floor gates the
// work (nothing but |x|^2 on an empty channel); in a hot stretch the FM
// discriminator (phase step per sample), smoothed over two samples, is
// sampled at fractional bit centres and correlated with the sync (one
// correlation finds both: + = ADS-B, - = uplink); the best timing within a
// fifth of a sample, the sync's own levels give the slicing centre, a
// Gardner loop follows the bit clock over an uplink's 4.3 ms; RS decides.
//
// ID separation: every ADS-B message an aircraft (or vehicle / beacon)
// sends itself is a kp::Talker packet of its address (exact span: sync to
// the last parity bit); TIS-B / ADS-R messages are sent BY a ground station
// about other aircraft, so they are packets of that station ("GS site N",
// the TIS-B site ID), like its uplink frames - never of the aircraft they
// describe. The whole band is the channel (the signal is 1.3 MHz wide).

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "msg.hpp"
#include "rs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double FS = 2.4e6;
constexpr double BITRATE = 1e6 / 0.96;
constexpr double SPB = FS / BITRATE;             // 2.304 samples per bit
constexpr int SYNC_BITS = 36;
constexpr uint64_t SYNC_ADSB = 0xEACDDA4E2ull;
constexpr int BITS_BASIC = 240, BITS_LONG = 384, BITS_UPLINK = 4416;
constexpr int BLK = 32;                          // gating block (samples)
constexpr int NEED = static_cast<int>((SYNC_BITS + BITS_UPLINK + 4) * SPB) + 4 * BLK;
constexpr float GATE = 2.0f;                     // block power over the noise floor (3 dB)
constexpr float CORR_MIN = 11.0f;                // |sync correlation| (rad): ~half a full match
constexpr int MAX_SYNC_ERRORS = 4;
constexpr double AIRCRAFT_TIMEOUT_S = 120;       // UAT sends once a second
constexpr double POS_TIMEOUT_S = 60;
constexpr double GS_TIMEOUT_S = 600;
constexpr double AC_AVG_S = 5, GS_AVG_S = 20;    // talker packet averaging

const std::vector<std::string> TABLE_COLS = {"Address", "Source", "Callsign", "Category", "Squawk", "Altitude ft",
                                             "GNSS alt ft", "V/S ft/min", "GS kt", "Track °", "Lat", "Lon",
                                             "Distance km", "Bearing °", "Air / ground", "Emergency", "NIC / NACp",
                                             "Signal dBFS", "Messages", "Position age s"};

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string hex_bytes(const uint8_t* b, int n) {
    static const char H[] = "0123456789abcdef";
    std::string s(static_cast<size_t>(2 * n), '0');
    for (int i = 0; i < n; i++) {
        s[2 * static_cast<size_t>(i)] = H[b[i] >> 4];
        s[2 * static_cast<size_t>(i) + 1] = H[b[i] & 15];
    }
    return s;
}
std::string hex6(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}

// |error| < 0.0016 rad
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

struct Aircraft {
    std::string id;                  // "A1B2C3" (ICAO) / "~A1B2C3" (other address types)
    uint32_t addr = 0;
    int aq = 0;
    bool direct = false, tisb = false, adsr = false;   // heard from it / via TIS-B / ADS-R
    int site = 0;                    // the TIS-B site that reported it
    double first = 0, last = 0;
    uint64_t msgs = 0;
    float rssi = -99;
    std::string callsign;
    int squawk = -1, cat = 0, emergency = 0, nacp = -1, nic = -1;
    bool ident = false;
    float alt_baro = NAN, alt_geo = NAN, vrate = NAN, gs = NAN, track = NAN;
    double alt_t = -1e9, vel_t = -1e9;
    bool ground = false;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    double map_t = -1e9, table_t = -1e9;
    bool on_map = false, in_table = false, said_cs = false, said_pos = false, said_cat = false;
    int said_emergency = 0;
    std::vector<std::pair<std::string, std::string>> detail;
};

struct Product {
    uint64_t count = 0;
    double last = 0;
    std::string when;                // product time "dd hh:mm" as sent
};
struct GroundStation {
    std::string id;                  // "GS site 11" (or "GS slot 7" without a site ID)
    int site = 0, slot = 0;
    double lat = NAN, lon = NAN;
    bool pos_valid = false, utc = false;
    double first = 0, last = 0;
    uint64_t uplinks = 0, tisb = 0;
    float rssi = -99;
    std::map<int, Product> products;
    std::deque<std::string> texts;   // latest text records (METAR / TAF ...)
    double table_t = -1e9, map_t = -1e9;
    bool said_products = false;
};

class Uat : public kp::Decoder {
public:
    explicit Uat(kp::Host& h)
        : Decoder(h), rs_basic_(0x187, 120, 1, 12), rs_long_(0x187, 120, 1, 14), rs_up_(0x187, 120, 1, 20) {
        for (int k = 0; k < SYNC_BITS; k++) sync_s_[k] = ((SYNC_ADSB >> (SYNC_BITS - 1 - k)) & 1) ? 1.f : -1.f;
        x_.reserve(1 << 16);
        host.table_columns(TABLE_COLS);
    }

    void reset() override {
        x_.clear();
        d_.clear();
        pw_.clear();
        dok_.clear();
        hot_.clear();
        a0_ = 0;
        pos_ = 0;
        noise_ = 0;
        ac_.clear();
        gs_.clear();
        host.table_columns(TABLE_COLS);
        house_t_ = summary_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "tisb") show_tisb_ = value != "0";
        else if (key == "fisb") log_text_ = value == "1";
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        const size_t old = x_.size();
        blk_abs_ = a0_ + old;
        x_.insert(x_.end(), x, x + n);
        // gate: mean power of every complete 32-sample block (blocks are
        // aligned to the buffer start, which only moves by whole blocks)
        const size_t nb = x_.size() / BLK;
        for (size_t b = hot_.size(); b < nb; b++) {
            float s = 0;
            for (int i = 0; i < BLK; i++) s += std::norm(x_[b * BLK + i]);
            pw_.push_back(s / BLK);
            hot_.push_back(0);
            dok_.push_back(0);
        }
        update_noise(old / BLK);
        const float thr = GATE * noise_;
        for (size_t b = old / BLK; b < nb; b++) hot_[b] = pw_[b] > thr;
        d_.resize(x_.size(), 0.f);

        const size_t end = x_.size() > static_cast<size_t>(NEED) ? x_.size() - NEED : 0;
        size_t j = std::max<size_t>(pos_, 2);
        while (j < end) {
            const size_t b = j / BLK;
            if (!hot_[b] && !hot_[b + 1]) { j = (b + 1) * BLK; continue; }
            ensure_d(j, j + 100);
            const float c = corr(static_cast<double>(j));
            if (std::fabs(c) < CORR_MIN) { j++; continue; }
            // the peak within the next samples, then the timing to 0.1 sample
            ensure_d(j, j + 110);
            double best_t = j;
            float best = c;
            for (int k = 1; k <= 6; k++) {
                const float ck = corr(static_cast<double>(j + k));
                if (std::fabs(ck) > std::fabs(best)) { best = ck; best_t = static_cast<double>(j + k); }
            }
            const double base = best_t;
            for (int k = -5; k <= 5; k++) {
                if (k == 0) continue;
                const double t = base + 0.1 * k;
                const float ck = corr(t);
                if (std::fabs(ck) > std::fabs(best) && (ck > 0) == (best > 0)) { best = ck; best_t = t; }
            }
            const int used = try_frame(best_t, best < 0);
            j = used > 0 ? static_cast<size_t>(best_t + used * SPB) : static_cast<size_t>(base) + 1;
        }
        // keep what a sync candidate still needs (whole blocks)
        size_t drop = std::min(j, end);
        drop = drop > 4 * BLK ? (drop - 4 * BLK) / BLK * BLK : 0;
        if (drop) {
            x_.erase(x_.begin(), x_.begin() + static_cast<long>(drop));
            d_.erase(d_.begin(), d_.begin() + static_cast<long>(drop));
            pw_.erase(pw_.begin(), pw_.begin() + static_cast<long>(drop / BLK));
            hot_.erase(hot_.begin(), hot_.begin() + static_cast<long>(drop / BLK));
            dok_.erase(dok_.begin(), dok_.begin() + static_cast<long>(drop / BLK));
            a0_ += drop;
        }
        pos_ = j - drop;
        housekeeping();
    }

private:
    uat::ReedSolomon rs_basic_, rs_long_, rs_up_;
    float sync_s_[SYNC_BITS];
    std::vector<kp::cf> x_;          // input since a0_
    std::vector<float> d_;           // discriminator: phase step x[n-1] -> x[n] (rad), where dok_
    std::vector<float> pw_;          // block mean power
    std::vector<uint8_t> hot_, dok_;
    uint64_t a0_ = 0, blk_abs_ = 0;
    size_t pos_ = 0;
    float noise_ = 0;
    double now_ = 0;
    bool show_tisb_ = true, log_text_ = false;
    const bool dump_ = getenv("UAT_DUMP") != nullptr;   // offline: every message's fields
    std::unordered_map<std::string, Aircraft> ac_;
    std::map<std::string, GroundStation> gs_;
    uint64_t n_adsb_ = 0, n_up_ = 0, n_tisb_ = 0, mark_ = 0;
    double rate_t_ = 0, rate_ = 0, house_t_ = -1e9, summary_t_ = -1e9;

    // noise floor: the lowest mean of 32 consecutive blocks (1024 samples) of
    // this call - UAT is bursty, the gaps are noise - smoothed
    void update_noise(size_t b0) {
        float lo = 0;
        bool have = false;
        for (size_t b = b0; b + 32 <= pw_.size(); b += 32) {
            float s = 0;
            for (int i = 0; i < 32; i++) s += pw_[b + i];
            if (!have || s < lo) { lo = s; have = true; }
        }
        if (!have) return;
        lo /= 32;
        noise_ = noise_ <= 0 ? lo : noise_ + 0.05f * (lo - noise_);
    }

    void ensure_d(size_t a, size_t b) {
        b = std::min(b, x_.size() - 1);
        for (size_t blk = a / BLK; blk <= b / BLK && blk < dok_.size(); blk++) {
            if (dok_[blk]) continue;
            dok_[blk] = 1;
            const size_t s = std::max<size_t>(blk * BLK, 1), e = std::min(x_.size(), (blk + 1) * BLK);
            for (size_t i = s; i < e; i++) {
                const kp::cf z = x_[i] * std::conj(x_[i - 1]);
                d_[i] = fast_atan2(z.imag(), z.real());
            }
        }
    }

    // the discriminator smoothed over two samples (centred on sample time t),
    // linearly interpolated
    inline float d2(double t) const {
        const size_t i = static_cast<size_t>(t);
        const float f = static_cast<float>(t - static_cast<double>(i));
        const float a = 0.5f * (d_[i] + d_[i + 1]), b = 0.5f * (d_[i + 1] + d_[i + 2]);
        return a + f * (b - a);
    }

    // sync correlation with the first bit starting at sample time t0
    float corr(double t0) const {
        float c = 0;
        for (int k = 0; k < SYNC_BITS; k++) c += sync_s_[k] * d2(t0 + (k + 0.5) * SPB);
        return c;
    }

    double time_of(double idx) const { return now_ + (static_cast<double>(a0_) + idx - static_cast<double>(blk_abs_)) / FS; }

    // demodulates and checks a frame whose sync begins at t0; returns the bits
    // it took (sync + data) or 0
    int try_frame(double t0, bool uplink) {
        ensure_d(static_cast<size_t>(t0), static_cast<size_t>(t0 + (SYNC_BITS + 2) * SPB) + 4);
        float v[SYNC_BITS];
        float s1 = 0, s0 = 0;
        int n1 = 0, n0 = 0;
        for (int k = 0; k < SYNC_BITS; k++) {
            v[k] = d2(t0 + (k + 0.5) * SPB);
            const bool one = (sync_s_[k] > 0) != uplink;
            if (one) { s1 += v[k]; n1++; } else { s0 += v[k]; n0++; }
        }
        const float m1 = s1 / n1, m0 = s0 / n0, centre = 0.5f * (m1 + m0), dev = 0.5f * (m1 - m0);
        if (dev < 0.25f) return 0;            // not a 312.5 kHz FSK (0.82 rad / sample)
        int err = 0;
        for (int k = 0; k < SYNC_BITS; k++) err += ((v[k] > centre) != ((sync_s_[k] > 0) != uplink));
        if (err > MAX_SYNC_ERRORS) return 0;
        const int nbits = uplink ? BITS_UPLINK : BITS_LONG;
        ensure_d(static_cast<size_t>(t0), static_cast<size_t>(t0 + (SYNC_BITS + nbits + 2) * SPB) + 4);
        // data bits, Gardner timing loop
        static thread_local std::vector<uint8_t> bytes;
        bytes.assign(static_cast<size_t>(nbits / 8), 0);
        double t = t0 + (SYNC_BITS + 0.5) * SPB;
        float pv = v[SYNC_BITS - 1];
        const float inv = 1.0f / (dev * dev);
        for (int i = 0; i < nbits; i++) {
            const float y = d2(t);
            const bool bit = y > centre;
            if (bit != (pv > centre)) {
                const float mid = d2(t - 0.5 * SPB) - centre;
                const float e = std::clamp((pv - y) * mid * inv, -1.0f, 1.0f);
                t += 0.03 * e;
            }
            if (bit) bytes[static_cast<size_t>(i >> 3)] |= static_cast<uint8_t>(0x80 >> (i & 7));
            pv = y;
            t += SPB;
        }
        const float level = 10.0f * std::log10(std::max(mean_power(t0, 64), 1e-12f));
        if (uplink) return uplink_frame(bytes.data(), t0, level) ? SYNC_BITS + BITS_UPLINK : 0;
        return adsb_frame(bytes.data(), t0, level);
    }

    float mean_power(double t0, int n) const {
        const size_t a = static_cast<size_t>(t0);
        float s = 0;
        int k = 0;
        for (size_t i = a; i < a + static_cast<size_t>(n) && i < x_.size(); i++, k++) s += std::norm(x_[i]);
        return k ? s / k : 0;
    }

    // RS decode of n on-air bytes (data first) with nroots parity: true = corrected
    static bool rs_fix(const uat::ReedSolomon& rs, uint8_t* c, int n, int* fixed) {
        uint8_t cw[255] = {};
        for (int i = 0; i < n; i++) cw[n - 1 - i] = c[i];
        const int r = rs.decode(cw);
        if (r < 0) return false;
        for (int i = n; i < 255; i++)
            if (cw[i]) return false;          // "corrected" into the shortened (zero) part: too many errors
        for (int i = 0; i < n; i++) c[i] = cw[n - 1 - i];
        *fixed = r;
        return true;
    }

    int adsb_frame(uint8_t* b, double t0, float level) {
        int fixed = 0, len = 0, bits = 0;
        uint8_t c[48];
        std::memcpy(c, b, 48);
        if (rs_fix(rs_long_, c, 48, &fixed) && (c[0] >> 3) != 0) {
            len = 34;
            bits = BITS_LONG;
        } else {
            std::memcpy(c, b, 30);
            if (!rs_fix(rs_basic_, c, 30, &fixed) || (c[0] >> 3) != 0) return 0;
            len = 18;
            bits = BITS_BASIC;
        }
        uat::AdsbMsg m;
        if (!uat::decode_adsb(c, len, m)) return 0;
        if (dump_) host.log(uat::dump(m));
        const double ts = time_of(t0), te = time_of(t0 + (SYNC_BITS + bits) * SPB);
        host.valid(ts, te);
        n_adsb_++;
        if (host.raw_wanted()) {
            host.raw("-" + hex_bytes(c, len) + ";rs=" + std::to_string(fixed) + fmt(";ss=%.1f", level));
        }
        on_adsb(m, ts, te, level, fixed);
        return SYNC_BITS + bits;
    }

    bool uplink_frame(const uint8_t* b, double t0, float level) {
        uint8_t pl[432];
        int total = 0;
        for (int blk = 0; blk < 6; blk++) {
            uint8_t c[92];
            for (int i = 0; i < 92; i++) c[i] = b[i * 6 + blk];
            int fixed = 0;
            if (!rs_fix(rs_up_, c, 92, &fixed)) return false;
            std::memcpy(pl + 72 * blk, c, 72);
            total += fixed;
        }
        uat::UplinkMsg u;
        if (!uat::decode_uplink(pl, u)) return false;
        if (dump_) host.log(uat::dump(u));
        const double ts = time_of(t0), te = time_of(t0 + (SYNC_BITS + BITS_UPLINK) * SPB);
        host.valid(ts, te);
        n_up_++;
        if (host.raw_wanted()) {
            host.raw("+" + hex_bytes(pl, 432) + ";rs=" + std::to_string(total) + fmt(";ss=%.1f", level));
        }
        on_uplink(u, ts, te, level);
        return true;
    }

    static std::string gs_id(int site, int slot) {
        return site ? "GS site " + std::to_string(site) : "GS slot " + std::to_string(slot);
    }
    GroundStation* station_by_site(int site) {
        for (auto& [k, g] : gs_)
            if (g.site == site) return &g;
        return nullptr;
    }

    void talker_packet(const std::string& id, const std::string& label, double ts, double te, double avg,
                       double keep, double lat = NAN, double lon = NAN, double alt_m = NAN) {
        kp::Talker t;
        t.id = id;
        t.label = label;
        t.start_s = ts;
        t.end_s = te;
        t.packet = true;
        t.avg_s = avg;
        t.keep_s = keep;
        t.lat = lat;
        t.lon = lon;
        t.alt_m = alt_m;
        host.talker(t);
    }

    void on_adsb(const uat::AdsbMsg& m, double ts, double te, float level, int fixed) {
        const bool self = uat::aq_self(m.aq);
        const bool tisb = uat::aq_tisb(m.aq);
        const std::string id = std::string(m.aq == uat::AQ_ADSB_ICAO || m.aq == uat::AQ_TISB_ICAO || m.aq == uat::AQ_ADSR ? "" : "~") + hex6(m.addr);
        if (!self) {
            n_tisb_++;
            // the ground station is the transmitter: its packet
            if (tisb && m.site) {
                GroundStation* g = station_by_site(m.site);
                const std::string gid = g ? g->id : gs_id(m.site, 0);
                if (g) { g->tisb++; g->last = now_; }
                talker_packet(gid, g ? "UAT ground station · slot " + std::to_string(g->slot) : "UAT ground station",
                              ts, te, GS_AVG_S, 1800, g ? g->lat : NAN, g ? g->lon : NAN);
            }
            if (!show_tisb_) return;
        }
        Aircraft& a = ac_[id];
        const bool fresh = a.msgs == 0;
        if (fresh) {
            a.id = id;
            a.addr = m.addr;
            a.first = now_;
        }
        a.aq = self ? m.aq : (a.direct ? a.aq : m.aq);
        a.direct |= self;
        a.tisb |= tisb;
        a.adsr |= m.aq == uat::AQ_ADSR;
        if (tisb) a.site = m.site;
        a.last = now_;
        a.msgs++;
        a.rssi = a.msgs == 1 ? level : a.rssi + 0.2f * (level - a.rssi);
        a.nic = m.nic;
        if (m.pos_valid && (m.lat != 0 || m.lon != 0)) {
            a.lat = m.lat;
            a.lon = m.lon;
            a.pos_t = now_;
        }
        if (m.alt_kind == 1) { a.alt_baro = static_cast<float>(m.alt_ft); a.alt_t = now_; }
        if (m.alt_kind == 2) { a.alt_geo = static_cast<float>(m.alt_ft); a.alt_t = now_; }
        if (m.sec_alt_kind == 1) a.alt_baro = static_cast<float>(m.sec_alt_ft);
        if (m.sec_alt_kind == 2) a.alt_geo = static_cast<float>(m.sec_alt_ft);
        a.ground = m.ag == 2;
        if (m.speed_ok) { a.gs = static_cast<float>(m.speed_kt); a.vel_t = now_; }
        if (m.track_kind) a.track = static_cast<float>(m.track);
        if (m.vr_kind) a.vrate = static_cast<float>(m.vrate_fpm);
        if (m.has_ms) {
            a.cat = m.cat;
            a.emergency = m.emergency;
            a.nacp = m.nacp;
            a.ident = m.ident;
            if (m.cs_callsign) {
                if (!m.callsign.empty()) a.callsign = kp::printable(m.callsign);
            } else if (m.callsign.size() == 4 && std::all_of(m.callsign.begin(), m.callsign.end(),
                                                              [](char ch) { return ch >= '0' && ch <= '7'; })) {
                a.squawk = std::atoi(m.callsign.c_str());
            }
            a.detail = {{"Address qualifier", uat::aq_name(m.aq)},
                        {"UAT version", std::to_string(m.version)},
                        {"SIL", std::to_string(m.sil)},
                        {"NACp / NACv", std::to_string(m.nacp) + " / " + std::to_string(m.nacv)},
                        {"NIC baro", std::to_string(m.nic_baro)},
                        {"Transmit MSO", std::to_string(m.mso)},
                        {"Capabilities", std::string(m.cdti ? "CDTI " : "") + (m.acas ? "ACAS " : "") +
                                             (m.acas_ra ? "RA-active " : "") + (m.ident ? "IDENT " : "") +
                                             (m.atc ? "ATC-services " : "")},
                        {"Heading reference", m.hdg_magnetic ? "magnetic" : "true"}};
        }
        if (fresh)
            host.event(std::string(self ? "New aircraft " : "New TIS-B / ADS-R target ") + id + " (" +
                           uat::aq_name(m.aq) + (tisb && m.site ? ", site " + std::to_string(m.site) : "") + ")",
                       5.0);
        if (self) {
            // the aircraft itself transmitted these samples
            std::string label = a.callsign;
            if (std::isfinite(a.alt_baro) && !a.ground) label += (label.empty() ? "" : " · ") + fmt("%.0f ft", a.alt_baro);
            double plat = NAN, plon = NAN, palt = NAN;
            // its position in THIS message (sent at this time): for the array
            // calibration when it is accurate (NACp >= 8: < 93 m)
            if (m.pos_valid && a.nacp >= 8 && !a.ground) {
                plat = m.lat;
                plon = m.lon;
                const float alt = std::isfinite(a.alt_geo) ? a.alt_geo : a.alt_baro;
                if (std::isfinite(alt)) palt = alt * 0.3048;
                else plat = plon = NAN;
            }
            talker_packet(id, label, ts, te, AC_AVG_S, 600, plat, plon, palt);
        }
        if (host.verbose())
            host.event(fmt("type %.0f ", m.type) + id + " " + uat::aq_name(m.aq) +
                           (m.pos_valid ? fmt(" %.5f", m.lat) + fmt(",%.5f", m.lon) : "") +
                           (m.alt_kind ? fmt(" %.0f ft", m.alt_ft) : "") + (m.has_ms ? " " + m.callsign : "") +
                           fmt(" %.1f dBFS", level) + (fixed ? " (" + std::to_string(fixed) + " RS fixes)" : ""),
                       0.0);
        update_outputs(a);
    }

    void on_uplink(const uat::UplinkMsg& u, double ts, double te, float level) {
        const std::string id = gs_id(u.site, u.slot);
        GroundStation& g = gs_[id];
        const bool fresh = g.uplinks == 0;
        if (fresh) {
            g.id = id;
            g.first = now_;
        }
        g.site = u.site;
        g.slot = u.slot;
        g.utc = u.utc;
        g.pos_valid = u.pos_valid;
        if (u.lat != 0 || u.lon != 0) {
            g.lat = u.lat;
            g.lon = u.lon;
        }
        g.last = now_;
        g.uplinks++;
        g.rssi = g.uplinks == 1 ? level : g.rssi + 0.2f * (level - g.rssi);
        std::vector<std::string> new_products;
        for (const auto& f : u.frames) {
            if (!f.is_fisb) continue;
            const auto& a = f.fisb;
            Product& p = g.products[a.product];
            if (p.count == 0) new_products.push_back(std::to_string(a.product) + " " + uat::product_name(a.product));
            p.count++;
            p.last = now_;
            char w[32];
            if (a.month > 0) snprintf(w, sizeof w, "%02d/%02d %02d:%02d", a.month, a.day, a.hour, a.minute);
            else snprintf(w, sizeof w, "%02d:%02d", a.hour, a.minute);
            p.when = w;
            if (uat::product_is_text(a.product) && !a.data.empty()) {
                for (const std::string& r0 : uat::text_records(a.data.data(), a.data.size())) {
                    const std::string rec = kp::printable(r0).substr(0, 400);
                    if (std::find(g.texts.begin(), g.texts.end(), rec) != g.texts.end()) continue;
                    g.texts.push_back(rec);
                    if (g.texts.size() > 40) g.texts.pop_front();
                    if (log_text_) host.event(id + ": " + rec.substr(0, 300), 600.0);
                }
            }
        }
        if (fresh)
            host.event("New UAT ground station " + id + " (slot " + std::to_string(u.slot) + ")" +
                           (u.lat != 0 || u.lon != 0 ? fmt(" at %.4f", u.lat) + fmt(", %.4f", u.lon) : ""),
                       5.0);
        if (!new_products.empty()) {
            std::string s;
            for (const auto& p : new_products) s += (s.empty() ? "" : ", ") + p;
            host.event(id + ": FIS-B " + s, 60.0);
        }
        talker_packet(id, "UAT ground station · slot " + std::to_string(u.slot), ts, te, GS_AVG_S, 1800,
                      u.lat != 0 || u.lon != 0 ? u.lat : NAN, u.lat != 0 || u.lon != 0 ? u.lon : NAN);
        if (host.verbose()) {
            std::string s;
            for (const auto& f : u.frames)
                if (f.is_fisb) s += " " + std::to_string(f.fisb.product);
            host.event("uplink " + id + fmt(" %.4f", u.lat) + fmt(",%.4f", u.lon) + " products" + s +
                           fmt(" %.1f dBFS", level), 0.0);
        }
        gs_outputs(g);
    }

    bool station(double* lat, double* lon) const { return host.station(lat, lon); }

    std::string alt_text(const Aircraft& a) const {
        if (a.ground) return "on ground";
        if (std::isfinite(a.alt_baro)) return fmt("%.0f ft", a.alt_baro);
        if (std::isfinite(a.alt_geo)) return fmt("%.0f ft GNSS", a.alt_geo);
        return "";
    }
    std::string source(const Aircraft& a) const {
        std::string s = a.direct ? (a.aq == uat::AQ_VEHICLE ? "vehicle" : a.aq == uat::AQ_BEACON ? "beacon" : "ADS-B") : "";
        if (a.tisb) s += std::string(s.empty() ? "" : " + ") + "TIS-B" + (a.site ? " site " + std::to_string(a.site) : "");
        if (a.adsr) s += std::string(s.empty() ? "" : " + ") + "ADS-R";
        return s;
    }

    std::vector<std::string> cells(const Aircraft& a) const {
        const bool pos = std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S;
        double slat, slon, dist = NAN, brg = NAN;
        if (pos && station(&slat, &slon)) {
            dist = distance_km(slat, slon, a.lat, a.lon);
            brg = bearing_deg(slat, slon, a.lat, a.lon);
        }
        const std::string cc = uat::category_code(a.cat);
        return {a.id, source(a), a.callsign, cc.empty() ? "" : cc + " " + uat::category_name(a.cat),
                a.squawk >= 0 ? fmt("%04.0f", a.squawk) : "",
                std::isfinite(a.alt_baro) ? fmt("%.0f", a.alt_baro) : "",
                std::isfinite(a.alt_geo) ? fmt("%.0f", a.alt_geo) : "",
                std::isfinite(a.vrate) && !a.ground ? fmt("%+.0f", a.vrate) : "",
                std::isfinite(a.gs) ? fmt("%.0f", a.gs) : "", std::isfinite(a.track) ? fmt("%.0f", a.track) : "",
                pos ? fmt("%.5f", a.lat) : "", pos ? fmt("%.5f", a.lon) : "",
                std::isfinite(dist) ? fmt("%.1f", dist) : "", std::isfinite(brg) ? fmt("%.0f", brg) : "",
                a.ground ? "ground" : "air", a.emergency ? uat::emergency_name(a.emergency) : "",
                std::to_string(std::max(a.nic, 0)) + (a.nacp >= 0 ? " / " + std::to_string(a.nacp) : ""),
                fmt("%.1f", a.rssi), std::to_string(a.msgs), pos ? fmt("%.0f", now_ - a.pos_t) : ""};
    }

    std::string label(const Aircraft& a) const { return a.callsign.empty() ? a.id : a.callsign; }
    std::string where(const Aircraft& a) const {
        if (!std::isfinite(a.lat)) return "";
        std::string s = fmt(" at %.4f", a.lat) + fmt(", %.4f", a.lon);
        const std::string alt = alt_text(a);
        return s + (alt.empty() ? "" : ", " + alt);
    }

    void update_outputs(Aircraft& a) {
        if (!a.callsign.empty() && !a.said_cs) {
            a.said_cs = true;
            const std::string cc = uat::category_code(a.cat);
            host.event(a.id + " is " + a.callsign + (cc.empty() ? "" : " (" + cc + " " + uat::category_name(a.cat) + ")"),
                       5.0);
        }
        const bool emerg = a.emergency || a.squawk == 7500 || a.squawk == 7600 || a.squawk == 7700;
        if (emerg) {
            const int key = a.emergency ? a.emergency : a.squawk;
            if (key != a.said_emergency) {
                a.said_emergency = key;
                const std::string why = a.emergency ? uat::emergency_name(a.emergency)
                                       : a.squawk == 7500 ? "hijack" : a.squawk == 7600 ? "radio failure" : "emergency";
                host.event("⚠ EMERGENCY " + label(a) + " (" + a.id + "): " + why +
                               (a.squawk >= 0 ? fmt(", squawk %04.0f", a.squawk) : "") + where(a),
                           30.0);
            }
        } else if (a.said_emergency) {
            a.said_emergency = 0;
            host.event("⚠ " + label(a) + " (" + a.id + ") emergency over" + where(a), 30.0);
        }
        if (!a.said_cat && a.cat) {
            a.said_cat = true;
            static const int odd[] = {6, 10, 11, 12, 14, 15};
            for (int c : odd)
                if (a.cat == c)
                    host.event("⚠ " + label(a) + " (" + a.id + ") is a " + uat::category_name(a.cat) + " (category " +
                                   uat::category_code(a.cat) + ")" + where(a),
                               30.0);
        }
        const bool has_pos = std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S;
        if (has_pos && !a.said_pos) {
            a.said_pos = true;
            std::string t = label(a) + " first position " + fmt("%.4f", a.lat) + ", " + fmt("%.4f", a.lon);
            const std::string alt = alt_text(a);
            if (!alt.empty()) t += ", " + alt;
            double slat, slon;
            if (station(&slat, &slon)) t += fmt(", %.0f km away", distance_km(slat, slon, a.lat, a.lon));
            host.event(t, 5.0);
        }
        if (has_pos && now_ - a.map_t >= 0.5) {
            a.map_t = now_;
            kp::MapPoint p;
            p.id = a.id;
            p.lat = a.lat;
            p.lon = a.lon;
            p.label = label(a);
            p.kind = (a.cat == 17 || a.cat == 18 || a.aq == uat::AQ_VEHICLE) ? "vehicle"
                   : a.aq == uat::AQ_BEACON || a.cat >= 19 ? "point" : "aircraft";
            p.heading = std::isfinite(a.track) ? a.track : NAN;
            if (a.ground) p.altitude_m = 0;
            else if (std::isfinite(a.alt_baro)) p.altitude_m = a.alt_baro * 0.3048f;
            else if (std::isfinite(a.alt_geo)) p.altitude_m = a.alt_geo * 0.3048f;
            if (std::isfinite(a.gs)) p.speed_kmh = a.gs * 1.852f;
            std::string info = "Address: " + a.id + "\nSource: " + source(a);
            auto line = [&](const char* k, const std::string& v) { if (!v.empty()) info += std::string("\n") + k + ": " + v; };
            line("Callsign", a.callsign);
            const std::string cc = uat::category_code(a.cat);
            if (!cc.empty()) line("Category", cc + " " + uat::category_name(a.cat));
            line("Altitude", alt_text(a));
            if (std::isfinite(a.alt_geo) && std::isfinite(a.alt_baro)) line("GNSS altitude", fmt("%.0f ft", a.alt_geo));
            if (std::isfinite(a.gs)) line("Ground speed", fmt("%.0f kt", a.gs));
            if (std::isfinite(a.track)) line("Track", fmt("%.0f°", a.track));
            if (std::isfinite(a.vrate) && !a.ground) line("Vertical rate", fmt("%+.0f ft/min", a.vrate));
            if (a.squawk >= 0) line("Squawk", fmt("%04.0f", a.squawk));
            if (a.emergency) line("Emergency", uat::emergency_name(a.emergency));
            line("Signal", fmt("%.1f dBFS", a.rssi) + " · " + std::to_string(a.msgs) + " messages");
            p.info = info;
            p.ttl_s = POS_TIMEOUT_S;
            host.map_point(p);
            a.on_map = true;
        }
        if (now_ - a.table_t >= 1.0) {
            a.table_t = now_;
            a.in_table = true;
            host.table_row(a.id, cells(a));
            if (!a.detail.empty()) host.table_detail(a.id, a.detail);
        }
    }

    void gs_outputs(GroundStation& g) {
        double slat, slon, dist = NAN, brg = NAN;
        const bool pos = std::isfinite(g.lat);
        if (pos && station(&slat, &slon)) {
            dist = distance_km(slat, slon, g.lat, g.lon);
            brg = bearing_deg(slat, slon, g.lat, g.lon);
        }
        if (now_ - g.table_t >= 1.0) {
            g.table_t = now_;
            host.table_row(g.id, {g.id, "ground station · slot " + std::to_string(g.slot), "",
                                  std::to_string(g.products.size()) + " FIS-B products", "", "", "", "", "", "",
                                  pos ? fmt("%.5f", g.lat) : "", pos ? fmt("%.5f", g.lon) : "",
                                  std::isfinite(dist) ? fmt("%.1f", dist) : "", std::isfinite(brg) ? fmt("%.0f", brg) : "",
                                  "ground", "", "", fmt("%.1f", g.rssi), std::to_string(g.uplinks + g.tisb), ""});
            std::vector<std::pair<std::string, std::string>> det = {
                {"TIS-B site ID", std::to_string(g.site)},
                {"Slot", std::to_string(g.slot)},
                {"Position", pos ? fmt("%.5f", g.lat) + ", " + fmt("%.5f", g.lon) + (g.pos_valid ? "" : " (marked not valid)") : "-"},
                {"UTC coupled", g.utc ? "yes" : "no"},
                {"Uplink frames", std::to_string(g.uplinks)},
                {"TIS-B messages", std::to_string(g.tisb)}};
            for (const auto& [pid, p] : g.products)
                det.push_back({"Product " + std::to_string(pid), std::string(uat::product_name(pid)) + " · " +
                                                                 std::to_string(p.count) + " APDUs · " + p.when});
            for (auto it = g.texts.rbegin(); it != g.texts.rend() && det.size() < 190; ++it) det.push_back({"Text", *it});
            host.table_detail(g.id, det);
        }
        if (pos && now_ - g.map_t >= 5.0) {
            g.map_t = now_;
            kp::MapPoint p;
            p.id = g.id;
            p.lat = g.lat;
            p.lon = g.lon;
            p.label = g.id;
            p.kind = "station";
            std::string pr;
            for (const auto& [pid, q] : g.products) pr += (pr.empty() ? "" : ", ") + std::to_string(pid);
            p.info = "UAT ground station\nTIS-B site ID: " + std::to_string(g.site) + "\nSlot: " +
                     std::to_string(g.slot) + "\nFIS-B products: " + pr + "\nSignal: " + fmt("%.1f dBFS", g.rssi);
            p.ttl_s = 1800;
            host.map_point(p);
        }
    }

    void housekeeping() {
        if (now_ - house_t_ < 1.0) return;
        house_t_ = now_;
        if (now_ - rate_t_ >= 5.0) {
            const uint64_t tot = n_adsb_ + n_up_;
            rate_ = (tot - mark_) / std::max(1e-3, now_ - rate_t_);
            mark_ = tot;
            rate_t_ = now_;
        }
        int n = 0, npos = 0, ntis = 0;
        for (auto it = ac_.begin(); it != ac_.end();) {
            Aircraft& a = it->second;
            if (now_ - a.last > AIRCRAFT_TIMEOUT_S) {
                if (a.on_map) host.map_remove(a.id);
                if (a.in_table) host.table_remove(a.id);
                it = ac_.erase(it);
                continue;
            }
            if (a.on_map && now_ - a.pos_t >= POS_TIMEOUT_S) {
                host.map_remove(a.id);
                a.on_map = false;
            }
            (a.direct ? n : ntis)++;
            if (std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S) npos++;
            ++it;
        }
        for (auto it = gs_.begin(); it != gs_.end();) {
            if (now_ - it->second.last > GS_TIMEOUT_S) {
                host.table_remove(it->first);
                host.map_remove(it->first);
                it = gs_.erase(it);
            } else {
                ++it;
            }
        }
        if (now_ - summary_t_ < 2.0) return;
        summary_t_ = now_;
        host.fact("Aircraft", std::to_string(n) + " ADS-B, " + std::to_string(ntis) + " TIS-B / ADS-R (" +
                                  std::to_string(npos) + " with position)");
        std::string g;
        for (const auto& [id, s] : gs_) g += (g.empty() ? "" : ", ") + id;
        host.fact("Ground stations", gs_.empty() ? "none heard" : g);
        host.fact("Messages", std::to_string(n_adsb_) + " ADS-B, " + std::to_string(n_up_) + " uplink" +
                                  fmt(" (%.1f/s)", rate_));
        host.fact("Noise floor", fmt("%.1f dBFS", 10.0 * std::log10(std::max(noise_, 1e-12f))));
    }
};

}  // namespace

KRAKEN_PLUGIN(Uat, {.id = "uat",
                    .name = "UAT (978 MHz)",
                    .description = "UAT - the US 978 MHz ADS-B link (general aviation) and its ground stations: "
                                   "aircraft position, altitude, velocity, callsign / squawk, category, emergency; "
                                   "TIS-B / ADS-R traffic and the FIS-B products ground stations uplink. Picking it "
                                   "tunes the VFO and its tuner to 978 MHz at 2.4 MHz; aircraft and ground stations "
                                   "appear on the 🗺 Map, each with its own DoA",
                    .version = "1.0",
                    .sample_rate = FS,
                    .min_vfo_rate = 2.0e6,
                    .author = "KrakenSDR",
                    .options = {{"tisb", "TIS-B / ADS-R traffic", "1", "1=Show|0=Hide",
                                 "Aircraft that ground stations rebroadcast (TIS-B: radar targets, ADS-R: 1090 MHz "
                                 "ADS-B) in the table and on the map. Their DoA is always the ground station's"},
                                {"fisb", "FIS-B text in the log", "0", "0=Off|1=On",
                                 "Write the FIS-B text products (METAR, TAF, PIREP ...) the ground stations send to "
                                 "the event log (always in a ground station's table details)"}},
                    .map = true,
                    .manual_only = true,
                    .fixed_freq_hz = 978e6,
                    .talkers = true})
