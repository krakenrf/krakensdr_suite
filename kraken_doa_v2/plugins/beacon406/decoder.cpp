// 406 MHz Cospas-Sarsat distress beacons: EPIRB (ships), ELT (aircraft), PLB
// (personal), SSAS (ship security), from the public C/S T.001 (first
// generation) and T.018 (second generation, detected only) specifications.
//
// The VFO sits on 406.050 MHz at 100 kHz (picking the decoder tunes it):
// every 406.0 - 406.1 MHz channel at once. rx.cpp finds each burst (160 ms
// carrier + 112 / 144 bits at 400 bit/s, biphase-L +-1.1 rad) on its own
// channel and demodulates it; bch.hpp checks / corrects BCH-1 (3 errors) and
// BCH-2 (2); cs406.cpp decodes the protocols. A burst with the self-test
// frame sync is a TEST - never an alert; any other is reported as a distress
// alert ("⚠") unless its protocol is a test / orbitography one.
//
// ID separation: each beacon's 15-hex ID is a kp::Talker packet per burst
// (its 160 ms carrier + the first bits: 0.24 s) on its own channel (freq_hz / bw_hz 3 kHz), kept for hours: every beacon
// gets its own DoA lobe - direction finding is how beacons are found.
//
// The decoder reports nothing to anyone: a heard distress beacon must be
// reported to the rescue coordination centre by the user (README).

#include "bch.hpp"
#include "cs406.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "rx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using b406::FS;

constexpr double KEEP_DEFAULT_S = 24 * 3600;
constexpr double ALERT_REPEAT_S = 300;       // a distress beacon's ⚠ event again after this long
constexpr double SGB_CENTRE_HZ = 406.050e6;
constexpr double CHAN_BW_HZ = 3000;          // a first-generation channel (3 kHz raster)

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}

// the first-generation channel raster (C/S T.012): 406.022 + 3 kHz steps
std::string channel_name(double rf) {
    if (!std::isfinite(rf)) return "";
    const double k = std::round((rf - 406.022e6) / 3000);
    const double nom = 406.022e6 + 3000 * k;
    if (k < 0 || k > 25 || std::fabs(rf - nom) > 1500) return fmt("%.4f MHz", rf / 1e6);
    char b[48];
    snprintf(b, sizeof b, "%.3f MHz (ch %c)", nom / 1e6, static_cast<char>('A' + static_cast<int>(k)));
    return b;
}

struct BurstRec {
    double t;            // host.time()
    double rf;           // Hz (NAN = unknown)
    double offset;       // Hz in the input
    double cn0;
    int corrected;
    bool self_test;
};

struct Beacon {
    std::string id;      // 15-hex
    b406::Message msg;   // the latest
    double first = 0, last = 0;
    int bursts = 0, self_tests = 0, normal = 0;
    double last_normal = -1e9, last_alert = -1e9, last_test_event = -1e9;
    std::deque<BurstRec> hist;
    double interval_sum = 0;
    int intervals = 0;
    double prev_t = -1e9;
    bool on_map = false;
    double table_t = -1e9;
    bool distress() const { return normal > 0 && !msg.test && !msg.orbitography; }
};

class Beacon406 : public kp::Decoder {
public:
    explicit Beacon406(kp::Host& h) : Decoder(h) {
        host.table_columns(COLS);
        host.table_keep(keep_s_);
    }

    void reset() override {
        rx_.reset();
        for (auto& [k, b] : beacons_) {
            host.table_remove(k);
            if (b.on_map) host.map_remove(k);
        }
        beacons_.clear();
        recent_.clear();
        sgb_n_ = 0;
        bursts_ = bch_fail_ = 0;
        host.table_columns(COLS);
        house_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "doa_avg") avg_s_ = value.empty() ? 1.0 : std::clamp(atof(value.c_str()), 0.5, 7200.0);
        else if (key == "keep_h") {
            keep_s_ = value.empty() ? KEEP_DEFAULT_S : std::clamp(atof(value.c_str()) * 3600, 600.0, 7 * 86400.0);
            host.table_keep(keep_s_);
        }
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        blk_abs_ = rx_.abs();
        const double rf = host.rf_hz();
        double sgb = 0;
        if (std::isfinite(rf)) sgb = host.rf_inverted() ? rf - SGB_CENTRE_HZ : SGB_CENTRE_HZ - rf;
        rx_.set_sgb_centre(std::fabs(sgb) < 40000 ? sgb : 0);
        rx_.verbose = host.verbose();
        std::vector<b406::Burst> bursts;
        std::vector<b406::WideBurst> wide;
        rx_.process(x, n, &bursts, &wide);
        for (auto& l : rx_.log) host.log(l);
        rx_.log.clear();
        for (auto& b : bursts) on_burst(b);
        for (auto& w : wide) on_wide(w);
        housekeeping();
    }

private:
    const std::vector<std::string> COLS = {"Status", "Beacon ID", "Type", "Country", "Identification", "Position",
                                           "Homing", "Channel", "Bursts", "Interval s", "C/N0 dBHz"};
    b406::Receiver rx_;
    std::map<std::string, Beacon> beacons_;
    struct Seen { std::string hex; double t; };
    std::deque<Seen> recent_;
    double now_ = 0;
    uint64_t blk_abs_ = 0;
    double avg_s_ = 1.0, keep_s_ = KEEP_DEFAULT_S;
    int sgb_n_ = 0, bursts_ = 0, bch_fail_ = 0;
    double house_t_ = -1e9, sgb_last_alert_ = -1e9;

    double to_time(double abs_idx) const { return now_ + (abs_idx - static_cast<double>(blk_abs_)) / FS; }
    double rf_of(double off) const {
        const double rf = host.rf_hz();
        if (!std::isfinite(rf)) return NAN;
        return host.rf_inverted() ? rf - off : rf + off;
    }

    void on_burst(const b406::Burst& b) {
        std::vector<uint8_t> bits = b.bits;
        int c1 = b406::bch1().correct(&bits[24]);
        // 3 corrections is where a 4-error burst can land on another codeword (a
        // phantom beacon ID): only when every corrected bit was a weak decision
        if (c1 == 3)
            for (size_t k = 24; k < 106; k++)
                if (bits[k] != b.bits[k] && std::fabs(b.soft[k]) > 0.8f) c1 = -1;
        if (c1 < 0) {
            bch_fail_++;
            if (host.verbose())
                host.event("Burst on " + channel_desc(b.freq_hz) + ": BCH-1 failed (not decoded)" +
                               (b.self_test ? " [self-test sync]" : ""), 0.0);
            update_facts();
            return;
        }
        bool lng = bits[24] == 1;
        int c2 = 0;
        // a long message's self-test may stop after bit 112 (T.001 4.5.4): nothing after it
        const bool truncated = lng && b.tail_q < 0.3;
        if (lng && !truncated) c2 = b406::bch2().correct(&bits[106]);
        if (!lng || truncated) bits.resize(112);
        if (truncated) c2 = -2;
        b406::Message m = b406::parse(bits, c2);
        // the same burst found twice (two candidates of one carrier)
        for (const auto& s : recent_)
            if (s.hex == m.hex_msg && std::fabs(s.t - to_time(b.t_mod)) < 0.2) return;
        recent_.push_back({m.hex_msg, to_time(b.t_mod)});
        while (recent_.size() > 32) recent_.pop_front();
        bursts_++;

        const double t_mod = to_time(b.t_mod);
        const double t_car = to_time(b.t_carrier);
        const double t_end = t_mod + (lng && !truncated ? 144 : 112) * b.bit_s;
        host.valid(t_car, t_end);

        const bool isnew = !beacons_.count(m.hex15);
        Beacon& bc = beacons_[m.hex15];
        if (isnew) {
            bc.id = m.hex15;
            bc.first = now_;
        }
        const bool had_pos = bc.msg.has_pos;
        const double old_lat = bc.msg.lat, old_lon = bc.msg.lon;
        bc.msg = m;
        bc.last = now_;
        bc.bursts++;
        if (b.self_test) bc.self_tests++;
        else bc.normal++;
        if (bc.prev_t > -1e8) {
            const double dt = t_mod - bc.prev_t;
            if (dt > 5 && dt < 600) {
                bc.interval_sum += dt;
                bc.intervals++;
            }
        }
        bc.prev_t = t_mod;
        bc.hist.push_back({t_mod, rf_of(b.freq_hz), b.freq_hz, b.snr_db, c1 + std::max(0, c2), b.self_test});
        while (bc.hist.size() > 12) bc.hist.pop_front();

        // DoA: this burst's samples on its channel - the 160 ms carrier + the first bits
        // (a packet is <= 0.24 s: one per burst)
        talker_packet(bc, b, t_car + 0.005, std::min(t_car + 0.245, t_end));

        const std::string what = describe(bc);
        const std::string ch = channel_desc(b.freq_hz);
        if (b.self_test) {
            if (now_ - bc.last_test_event > 60)
                host.event("Self-test burst (NOT an alert): " + what + " · " + ch, 0.0);
            bc.last_test_event = now_;
        } else if (m.test || m.orbitography) {
            if (isnew || now_ - bc.last_test_event > 600)
                host.event(std::string(m.orbitography ? "Orbitography / reference beacon" : "Test-protocol beacon") +
                               " (not a distress alert): " + what + " · " + ch, 0.0);
            bc.last_test_event = now_;
        } else if (now_ - bc.last_alert > ALERT_REPEAT_S) {
            host.event(std::string("⚠ ") + (m.security ? "SHIP SECURITY ALERT" : "DISTRESS BEACON") + ": " + what + " · " +
                           ch + (m.has_pos ? " · " + b406::fmt_latlon(m.lat, m.lon) : " · no position") +
                           " - report it to the rescue coordination centre",
                       0.0);
            bc.last_alert = now_;
        }
        if (!b.self_test) bc.last_normal = now_;
        if (isnew && b.self_test) host.event("New beacon " + m.hex15 + " (self-test)", 0.0);
        if (had_pos && m.has_pos && (std::fabs(old_lat - m.lat) > 1e-4 || std::fabs(old_lon - m.lon) > 1e-4))
            host.event("Beacon " + m.hex15 + " position now " + b406::fmt_latlon(m.lat, m.lon), 0.0);
        if (host.verbose()) {
            char s[256];
            snprintf(s, sizeof s, "%s burst %s: %s, BCH %d/%d corrected, C/N0 %.1f dBHz, bit %.3f ms, %s", b.self_test ? "self-test" : "normal",
                     m.hex_msg.c_str(), ch.c_str(), c1, c2, b.snr_db, b.bit_s * 1e3, b.inverted ? "inverted phase" : "phase as T.001");
            host.event(s, 0.0);
        }
        if (host.raw_wanted())
            host.raw(std::string(b.self_test ? "FFFED0" : "FFFE2F") + m.hex_msg + fmt(" %.1f", b.snr_db) +
                     (b.self_test ? " self-test" : ""));
        bc.table_t = -1e9;
        outputs(bc);
        update_facts();
    }

    std::string channel_desc(double off) const {
        const double rf = rf_of(off);
        if (std::isfinite(rf)) return channel_name(rf);
        return fmt("%+.1f kHz", off / 1e3);
    }

    std::string describe(const Beacon& bc) const {
        const auto& m = bc.msg;
        std::string s = m.type;
        if (!m.country_name.empty()) s += " (" + m.country_name + ")";
        else s += " (country " + std::to_string(m.country) + ")";
        if (!m.ident.empty()) s += " " + m.ident;
        s += ", ID " + m.hex15;
        return s;
    }

    std::string status(const Beacon& bc) const {
        const auto& m = bc.msg;
        if (bc.distress() && now_ - bc.last_normal < keep_s_) return m.security ? "⚠ SECURITY ALERT" : "⚠ DISTRESS";
        if (m.orbitography) return "orbitography";
        if (m.test && bc.normal > 0) return "test protocol";
        return "self-test";
    }

    std::string label(const Beacon& bc) const {
        const auto& m = bc.msg;
        std::string st = bc.distress() ? "⚠ " : (bc.normal ? "test · " : "self-test · ");
        std::string s = st + m.type;
        if (!m.country_name.empty()) s += " · " + m.country_name;
        if (!m.ident.empty()) s += " · " + m.ident;
        if (s.size() > 60) s.resize(60);
        // a cut UTF-8 sequence at the end
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
        return s;
    }

    void talker_packet(const Beacon& bc, const b406::Burst& b, double a, double e) {
        if (e - a < 0.02) return;
        kp::Talker t;
        t.id = bc.id;
        t.label = label(bc);
        t.start_s = a;
        t.end_s = std::min(e, a + 0.24);
        t.packet = true;
        t.freq_hz = b.freq_hz;
        t.bw_hz = CHAN_BW_HZ;
        t.avg_s = avg_s_;
        t.keep_s = keep_s_;
        host.talker(t);
    }

    void on_wide(const b406::WideBurst& w) {
        sgb_n_++;
        const double a = to_time(w.t0), e = to_time(w.t1);
        host.valid(a, e);
        const double rf = rf_of(w.centre_hz);
        const std::string ch = std::isfinite(rf) ? fmt("%.3f MHz", rf / 1e6) : fmt("%+.1f kHz", w.centre_hz / 1e3);
        if (now_ - sgb_last_alert_ > ALERT_REPEAT_S)
            host.event(fmt("⚠ Second-generation beacon burst (spread spectrum, %.1f s) on ", e - a) + ch +
                           fmt(", %.1f dB in band - not decoded: its ID and distress / test status are unknown", w.snr_db),
                       0.0);
        sgb_last_alert_ = now_;
        // its DoA: 0.24 s from the middle of the burst (no ID decoded: one talker for every SGB)
        kp::Talker t;
        t.id = "SGB";
        t.label = "2nd-gen beacon (ID not decoded)";
        const double mid = 0.5 * (a + e);
        t.start_s = mid - 0.12;
        t.end_s = mid + 0.12;
        t.packet = true;
        t.freq_hz = w.centre_hz;
        t.bw_hz = w.bw_hz;
        t.avg_s = 1;
        t.keep_s = keep_s_;
        host.talker(t);
        host.table_row("SGB", {"⚠ 2nd gen (not decoded)", "SGB", "?", "", "", "", "", ch, std::to_string(sgb_n_), "",
                               fmt("%.1f dB", w.snr_db)});
        update_facts();
    }

    void outputs(Beacon& bc) {
        const auto& m = bc.msg;
        double isum = bc.intervals ? bc.interval_sum / bc.intervals : NAN;
        const auto& h = bc.hist.back();
        std::string pos = m.has_pos ? b406::fmt_latlon(m.lat, m.lon) : "";
        host.table_row(bc.id, {status(bc), bc.id, m.type, m.country_name.empty() ? std::to_string(m.country) : m.country_name,
                               m.ident, pos, m.homing_text, channel_desc(h.offset), std::to_string(bc.bursts),
                               std::isfinite(isum) ? fmt("%.1f", isum) : "", fmt("%.1f", h.cn0)});
        std::vector<std::pair<std::string, std::string>> det;
        det.push_back({"Status", status(bc) + (bc.distress() ? " - report it to the rescue coordination centre" : "")});
        det.push_back({"Beacon ID (15 hex)", bc.id});
        det.push_back({"Message", m.hex_msg + (m.long_msg ? " (long)" : " (short)")});
        det.push_back({"Protocol", m.protocol});
        det.push_back({"Country", std::to_string(m.country) + (m.country_name.empty() ? "" : " " + m.country_name)});
        for (const auto& f : m.fields) det.push_back(f);
        if (m.has_pos) {
            det.push_back({"Position", b406::fmt_latlon(m.lat, m.lon)});
            det.push_back({"Position encoding", m.pos_text});
        } else if (!m.pos_text.empty()) {
            det.push_back({"Position", m.pos_text});
        }
        if (!m.pos_source.empty()) det.push_back({"Position source", m.pos_source});
        if (!m.homing_text.empty()) det.push_back({"Homing", m.homing_text});
        if (!m.emergency.empty()) det.push_back({"Non-protected bits", m.emergency});
        if (m.long_msg && m.pdf2_errors == -1) det.push_back({"PDF-2", "BCH-2 failed: its fields are not shown"});
        if (m.long_msg && m.pdf2_errors == -2) det.push_back({"PDF-2", "not sent (self-test stopped after bit 112)"});
        det.push_back({"Bursts", std::to_string(bc.bursts) + " (" + std::to_string(bc.normal) + " normal, " +
                                     std::to_string(bc.self_tests) + " self-test)"});
        if (std::isfinite(isum)) det.push_back({"Mean interval", fmt("%.1f s (first generation: 50 s +-5 %)", isum)});
        for (auto it = bc.hist.rbegin(); it != bc.hist.rend(); ++it) {
            char s[160];
            snprintf(s, sizeof s, "%s%s, C/N0 %.1f dBHz, %d bit(s) corrected", it->self_test ? "self-test, " : "",
                     channel_desc(it->offset).c_str(), it->cn0, it->corrected);
            det.push_back({fmt("Burst at %.1f s", it->t), s});
        }
        host.table_detail(bc.id, det);
        if (m.has_pos) {
            kp::MapPoint p;
            p.id = bc.id;
            p.lat = m.lat;
            p.lon = m.lon;
            p.label = (bc.distress() ? "⚠ " : "") + m.type + (m.ident.empty() ? "" : " " + m.ident);
            p.kind = m.map_kind;
            std::string info = "Status: " + status(bc) + "\nBeacon ID: " + bc.id + "\nType: " + m.type +
                               "\nCountry: " + (m.country_name.empty() ? std::to_string(m.country) : m.country_name);
            if (!m.ident.empty()) info += "\nIdentification: " + m.ident;
            info += "\nPosition: " + m.pos_text;
            if (!m.homing_text.empty()) info += "\nHoming: " + m.homing_text;
            info += "\nChannel: " + channel_desc(h.offset) + "\nBursts: " + std::to_string(bc.bursts);
            p.info = info;
            p.ttl_s = std::min(keep_s_, 86400.0);
            host.map_point(p);
            bc.on_map = true;
        } else if (bc.on_map) {
            host.map_remove(bc.id);
            bc.on_map = false;
        }
    }

    void update_facts() {
        int distress = 0;
        const Beacon* last = nullptr;
        const Beacon* last_d = nullptr;
        for (const auto& [k, b] : beacons_) {
            if (b.distress()) {
                distress++;
                if (!last_d || b.last_normal > last_d->last_normal) last_d = &b;
            }
            if (!last || b.last > last->last) last = &b;
        }
        host.fact("Beacons heard", std::to_string(beacons_.size()) + (distress ? " (" + std::to_string(distress) + " ⚠ distress)" : ""));
        host.fact("⚠ Distress", last_d ? describe(*last_d) + " - report it to the rescue coordination centre" : "");
        if (last) host.fact("Last burst", describe(*last) + " · " + status(*last));
        host.fact("Bursts decoded", std::to_string(bursts_) + (bch_fail_ ? " (" + std::to_string(bch_fail_) + " failed BCH)" : ""));
        if (sgb_n_) host.fact("Second-generation bursts", std::to_string(sgb_n_) + " (detected, not decoded)");
    }

    void housekeeping() {
        if (now_ - house_t_ < 5) return;
        house_t_ = now_;
        bool changed = false;
        for (auto it = beacons_.begin(); it != beacons_.end();) {
            if (now_ - it->second.last > keep_s_) {
                host.table_remove(it->first);
                if (it->second.on_map) host.map_remove(it->first);
                it = beacons_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) update_facts();
    }
};

}  // namespace

KRAKEN_PLUGIN(Beacon406, {.id = "beacon406",
                          .name = "406 MHz distress beacons (EPIRB / ELT / PLB)",
                          .category = "Marine & distress",
                          .description = "Cospas-Sarsat 406 MHz beacons on every 406.0-406.1 MHz channel: distress "
                                         "alerts vs self-tests, 15-hex beacon ID, country, type, MMSI / aircraft "
                                         "address / serial / call sign, the encoded position on the 🗺 Map, a DoA per "
                                         "beacon; second-generation bursts detected. Picking it tunes the VFO to "
                                         "406.050 MHz at 100 kHz. A distress beacon must be reported to the rescue "
                                         "coordination centre - this decoder reports nothing itself",
                          .version = "1.0",
                          .sample_rate = FS,
                          .min_vfo_rate = FS,
                          .author = "KrakenSDR",
                          .options = {{"doa_avg", "DoA per beacon", "1", "1=Each burst alone|300=5 minutes|1800=30 minutes",
                                       "How long each beacon's bursts are averaged for its bearing (a beacon "
                                       "bursts every ~50 s; each burst alone follows a moving search)"},
                                      {"keep_h", "Keep beacons", "24", "1=1 hour|6=6 hours|24=24 hours|168=7 days",
                                       "How long a beacon stays listed (table, map, 📡 DF list) after its last burst"}},
                          .map = true,
                          .manual_only = true,
                          .fixed_freq_hz = 406.050e6,
                          .talkers = true})
