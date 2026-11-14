// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KrakenRF Inc. - part of the KrakenSDR `flarm` decoder
// plugin, licensed under the GNU General Public License version 3 (LICENSE in
// this folder; README.md "Licence").

// FLARM and OGN tracker packets: the collision-avoidance / tracking radios
// of gliders and light aircraft, 868 MHz (Europe, Africa, New Zealand) and
// the 902-928 MHz hopping bands.
//
// The VFO sits on its tuner's centre with the full 2.4 MHz (lock_vfo); every
// channel of the band plans inside it is watched at once (receiver.cpp) -
// at 868.8 MHz that is 868.2 / 868.4 (Europe's two), 868.8 and 869.25 (New
// Zealand's). No channel inside: the receiver is retuned once to the band
// (option band, from the station's location by default).
//
// FLARM: only the radio ID and its address type are read - the rest of the
// packet is encrypted with keys FLARM has not published and is not decoded
// or analysed in any way. OGN trackers: the open protocol (packet.cpp) -
// position, altitude, climb, speed, track, aircraft type, info strings
// (personal ones are never kept), status. A tracker that asks not to be
// tracked (stealth / "no-track") never gets a position anywhere - only its
// bearing; an owner-encrypted one gets its address + bearing.
//
// ID separation: every packet an aircraft sends ITSELF is a kp::Talker
// packet of its address ("FLR DD1234", "ICA C81A2B", "OGN 3C5A21", "RND
// ..."), its samples from the first matched sync chip to the last data chip,
// on its own channel (freq_hz = the measured carrier, 160 kHz wide): aircraft
// on different channels at the same moment keep their own bearings. A packet
// with a relay count was re-sent by another tracker - it names nobody who
// sent these samples: no talker.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "packet.hpp"
#include "receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

using flarm::FS;

constexpr double LISTEN_HZ = 1.05e6;       // plan channels this close to the VFO centre are received
constexpr double REBUILD_HZ = 1e3;         // RF moved this far: a new channel list
constexpr double SAME_CHANNEL_HZ = 1e3;    // an extra channel this close to a plan channel is that one
constexpr double FACTS_S = 2.0;
constexpr double HOUSE_S = 2.0;
constexpr double FORGET_S = 1800;          // no packet for this long: the aircraft is forgotten
constexpr double SHOW_AGE_S = 300;         // a position older than this isn't shown / mapped
constexpr double LABEL_ALT_AGE_S = 120;    // ... nor its altitude in the label
constexpr double TALKER_POS_AGE_S = 0.5;   // the talker gets the position only from this very packet
constexpr double REFRESH_S = 0.999;        // a table row / map point at most about once a second
constexpr double CHAN_BW_HZ = 160e3;
constexpr double TALKER_AVG_S = 10, TALKER_KEEP_S = 3600;
constexpr double MAP_TTL_S = 300;
constexpr double EARTH_KM = 6371.0;
constexpr const char* SEP = " \xC2\xB7 ";   // " · "

struct Band {
    const char* key;
    double tune_hz;
    const char* name;
};
const Band BANDS[] = {
    {"eu", 868.8e6, "Europe / Africa / New Zealand (868.2, 868.4, 869.25 MHz)"},
    {"nz", 868.8e6, "New Zealand (869.25 MHz; 868.2 / 868.4 too)"},
    {"au", 921.6e6, "Australia / South America (917-926.2 MHz hopping)"},
    {"us", 915.2e6, "North America (902-928 MHz hopping)"},
    {"il", 916.6e6, "Israel (916.2 MHz)"},
};

const Band* band_by_key(const std::string& k) {
    for (const Band& b : BANDS)
        if (k == b.key) return &b;
    return nullptr;
}

// The band from where the station is (first match wins)
const Band& band_from_station(bool known, double lat, double lon) {
    if (!known) return BANDS[0];
    if (lat > -50 && lat < -30 && lon > 164 && lon < 180) return BANDS[1];
    if (lat > -45 && lat < -9 && lon > 110 && lon < 160) return BANDS[2];
    if (lon > -170 && lon < -30) return lat < -20 ? BANDS[2] : BANDS[3];
    if (lon > 34 && lon < 36 && lat > 29 && lat < 34) return BANDS[4];
    return BANDS[0];
}

// The plan channels (Hz), sorted: Europe's / Russia's / New Zealand's 868 MHz
// ones, 866.0, 920.9 and the 902.2 + 0.4 k MHz grid of the hopping bands
// (Israel's 916.2, Australia / South America 917.0 - 926.2, the US all 65)
const std::vector<int64_t>& plan_channels() {
    static const std::vector<int64_t> plan = [] {
        std::vector<int64_t> v = {866000000, 868200000, 868400000, 868800000, 869250000, 920900000};
        for (int k = 0; k <= 64; k++) v.push_back(902200000 + 400000LL * k);
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        return v;
    }();
    return plan;
}

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

std::string talker_id(int addr_type, uint32_t addr) {
    static const char* const PREFIX[4] = {"RND", "ICA", "FLR", "OGN"};
    char b[16];
    snprintf(b, sizeof b, "%s %06X", PREFIX[addr_type & 3], addr & 0xFFFFFF);
    return b;
}

std::string hex(const uint8_t* b, int n) {
    static const char* const H = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < n; i++) {
        s.push_back(H[b[i] >> 4]);
        s.push_back(H[b[i] & 15]);
    }
    return s;
}

// at most n bytes, not ending inside a UTF-8 sequence
std::string cut_utf8(std::string s, size_t n) {
    if (s.size() <= n) return s;
    s.resize(n);
    size_t i = s.size();
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) i--;
    if (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0xC0) {
        // the lead byte of a sequence: complete only if all its bytes are there
        const unsigned char lead = static_cast<unsigned char>(s[i - 1]);
        const size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (s.size() - (i - 1) < len) s.resize(i - 1);
    }
    return s;
}

double haversine_km(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180;
    const double a = std::pow(std::sin((la2 - la1) * r / 2), 2) +
                     std::cos(la1 * r) * std::cos(la2 * r) * std::pow(std::sin((lo2 - lo1) * r / 2), 2);
    return 2 * EARTH_KM * std::asin(std::min(1.0, std::sqrt(a)));
}

double bearing_deg(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180;
    const double y = std::sin((lo2 - lo1) * r) * std::cos(la2 * r);
    const double x = std::cos(la1 * r) * std::sin(la2 * r) - std::sin(la1 * r) * std::cos(la2 * r) * std::cos((lo2 - lo1) * r);
    double b = std::atan2(y, x) / r;
    if (b < 0) b += 360;
    return b;
}

struct Aircraft {
    std::string id;
    bool has_flarm = false, has_ogn = false;
    int addr_type = 0;
    double first = 0, last = 0;
    int own = 0, relayed = 0;
    double signal_db = NAN;          // own packets: the first, then an EMA (0.2)
    double mhz = NAN;                // the last own packet's channel
    bool stealth = false, encrypted = false;
    bool emergency_told = false;
    int type = -1;                   // -1 = not known yet
    bool has_pos = false;
    double lat = NAN, lon = NAN, pos_t = 0;
    double alt = NAN;
    bool alt_known = false;          // a 3D fix
    double palt = NAN;
    bool palt_ok = false;
    double speed = NAN, track = NAN, climb = NAN, turn = NAN;
    std::map<std::string, std::string> info;   // by name: sorted byte-wise
    std::string status;
    double row_t = -1e18, map_t = -1e18;
    bool on_map = false;
};

class FlarmDecoder : public kp::Decoder {
public:
    explicit FlarmDecoder(kp::Host& h) : Decoder(h) {
        host.table_columns(COLS);
        host.table_keep(FORGET_S);
        if (const char* d = std::getenv("FLARM_DUMP"); d && *d) rx_.set_dump_dir(d);
        rx_.set_threshold_db(sens_db_);
    }

    void reset() override {
        rx_.reset();
        acft_.clear();
        built_ = false;
        host.table_columns(COLS);
        facts_t_ = -1e18;
        house_t_ = -1e18;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "band") {
            const std::string b = value.empty() ? "auto" : value;
            if (b != band_) tune_asked_ = false;   // a new band: tune again (once)
            band_ = b;
        } else if (key == "tune") {
            tune_on_ = value != "0";
        } else if (key == "sens") {
            sens_db_ = value.empty() ? 6.0 : std::clamp(std::atof(value.c_str()), 3.0, 20.0);
            rx_.set_threshold_db(sens_db_);
        } else if (key == "extra") {
            const double mhz = std::atof(value.c_str());
            const double hz = mhz > 0 ? std::round(mhz * 1e6) : 0;
            if (hz != extra_hz_) built_ = false;
            extra_hz_ = hz;
        }
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        channels();
        rx_.process(x, n, now_, &pk_);
        for (const auto& l : rx_.log) host.log(l);
        rx_.log.clear();
        for (const auto& p : pk_) on_packet(p);
        pk_.clear();
        housekeeping();
        if (now_ - facts_t_ >= FACTS_S) {
            facts_t_ = now_;
            facts();
        }
    }

private:
    const std::vector<std::string> COLS = {"Aircraft",   "Protocol",  "Aircraft type", "Registration", "Altitude m", "Climb m/s",
                                           "Speed km/h", "Track °",   "Lat",           "Lon",          "Distance km", "Bearing °",
                                           "Notes",      "MHz",       "Signal dBFS",   "Packets"};
    flarm::Receiver rx_;
    std::vector<flarm::RxPacket> pk_;
    std::map<std::string, Aircraft> acft_;
    double now_ = 0;
    // options
    std::string band_ = "auto";
    bool tune_on_ = true, tune_asked_ = false;
    double sens_db_ = 6.0, extra_hz_ = 0;
    // the channel list's basis
    bool built_ = false;
    double built_rf_ = NAN, built_extra_ = 0;
    std::string chan_text_;
    // counters since the plugin started (reset() keeps them)
    uint64_t flarm_pk_ = 0, ogn_pk_ = 0;
    double facts_t_ = -1e18, house_t_ = -1e18;

    // --- channels and tuning -------------------------------------------------------
    void channels() {
        const double rf = host.rf_hz();
        const bool known = std::isfinite(rf) && rf > 0;
        bool rebuild = !built_ || known != std::isfinite(built_rf_) || extra_hz_ != built_extra_ ||
                       (known && std::fabs(rf - built_rf_) >= REBUILD_HZ);
        // no channel inside: look again on every block
        if (!rebuild && known && rx_.windows().empty()) rebuild = !list_for(rf).empty();
        if (rebuild) {
            std::vector<flarm::Window> w = known ? list_for(rf) : grid();
            std::string text;
            if (!known) {
                text = "every 100 kHz of the VFO (the RF is unknown)";
            } else if (w.empty()) {
                text = "none - no FLARM / OGN channel inside the VFO";
            } else {
                for (const auto& c : w) text += (text.empty() ? "" : ", ") + fmt("%.3f", c.mhz);
            }
            rx_.set_windows(w);
            built_ = true;
            built_rf_ = known ? rf : NAN;
            built_extra_ = extra_hz_;
            host.fact("Channels", text);
            chan_text_ = text;
        }
        if (known && rx_.windows().empty()) maybe_tune();
    }

    std::vector<flarm::Window> list_for(double rf) const {
        std::vector<flarm::Window> w;
        for (int64_t ch : plan_channels())
            if (std::fabs(static_cast<double>(ch) - rf) <= LISTEN_HZ) w.push_back({static_cast<double>(ch) - rf, static_cast<double>(ch) / 1e6});
        if (extra_hz_ > 0 && std::fabs(extra_hz_ - rf) <= LISTEN_HZ) {
            bool dup = false;
            for (const auto& c : w) dup |= std::fabs(c.off_hz + rf - extra_hz_) < SAME_CHANNEL_HZ;
            if (!dup) w.push_back({extra_hz_ - rf, extra_hz_ / 1e6});
        }
        return w;
    }

    // the RF unknown: a window every 100 kHz
    static std::vector<flarm::Window> grid() {
        std::vector<flarm::Window> w;
        for (int k = -10; k <= 10; k++) w.push_back({k * 1e5, NAN});
        return w;
    }

    void maybe_tune() {
        if (!tune_on_ || tune_asked_) return;
        tune_asked_ = true;
        const Band* b = band_by_key(band_);
        if (!b) {
            double lat = 0, lon = 0;
            const bool st = host.station(&lat, &lon);
            b = &band_from_station(st, lat, lon);
        }
        if (host.tune(b->tune_hz)) host.event(fmt("Tuning to %.3f MHz: ", b->tune_hz / 1e6) + b->name, 30);
    }

    // --- packets -------------------------------------------------------------------
    bool trackable(const Aircraft& a) const { return a.has_ogn && !a.stealth && !a.encrypted; }
    bool shown(const Aircraft& a) const { return trackable(a) && a.has_pos && now_ - a.pos_t < SHOW_AGE_S; }

    std::string registration(const Aircraft& a) const {
        auto r = a.info.find("Registration"), c = a.info.find("Competition ID");
        if (r != a.info.end() && c != a.info.end()) return r->second + " / " + c->second;
        if (r != a.info.end()) return r->second;
        if (c != a.info.end()) return c->second;
        return "";
    }

    std::string label(const Aircraft& a) const {
        std::string s;
        auto add = [&](const std::string& p) { s += (s.empty() ? "" : SEP) + p; };
        auto r = a.info.find("Registration");
        if (r == a.info.end()) r = a.info.find("Competition ID");
        if (r != a.info.end()) add(r->second);
        if (trackable(a) && a.alt_known && a.has_pos && now_ - a.pos_t < LABEL_ALT_AGE_S) add(fmt("%.0f m", a.alt));
        if (a.type > 0) add(pkt::type_name(a.type));
        if (a.stealth) add("no-track");
        if (s.empty()) s = a.has_flarm ? "FLARM" : "OGN";
        return s;
    }

    static void apply(Aircraft& a, const pkt::OgnContent& c, double now) {
        if (c.report == -1) {
            a.stealth = c.stealth;
            a.type = c.acft_type;
            if (c.stealth) {
                // a no-track tracker's position is not even kept: a later packet
                // without the flag (and without a fix) can't bring it back
                a.has_pos = false;
                a.lat = a.lon = a.alt = a.palt = NAN;
                a.alt_known = a.palt_ok = false;
                a.speed = a.track = a.climb = a.turn = NAN;
            } else if (c.fix) {
                a.has_pos = true;
                a.lat = c.lat;
                a.lon = c.lon;
                a.pos_t = now;
                a.alt = c.alt_m;
                a.alt_known = c.fix3d;
                a.palt = c.palt_m;
                a.palt_ok = c.baro;
                a.speed = c.speed_ms;
                a.track = c.track_deg;
                a.climb = c.climb_ms;
                a.turn = c.turn_dps;
            }
        } else if (c.report == 1) {
            for (const auto& [k, v] : c.info) a.info[k] = v;
        } else if (c.report == 0) {
            a.status = c.status;
        }
    }

    void on_packet(const flarm::RxPacket& p) {
        const pkt::Frame& f = p.frame;
        const bool ogn = f.proto == pkt::Proto::OGN;
        const bool own = !ogn || f.relay == 0;
        const std::string id = talker_id(f.addr_type, f.addr);
        const double t0 = rx_.time_of(p.t0), t1 = rx_.time_of(p.t1);
        const double mhz = rx_.windows()[static_cast<size_t>(p.win)].mhz;   // NAN: the RF is unknown
        const std::string on_mhz = std::isfinite(mhz) ? fmt(" on %.3f MHz", mhz) : "";
        (ogn ? ogn_pk_ : flarm_pk_)++;

        Aircraft& a = acft_[id];
        if (a.id.empty()) {
            a.id = id;
            a.first = now_;
        }
        const bool first_own = a.own == 0;
        const bool first_any = a.own == 0 && a.relayed == 0;
        a.addr_type = f.addr_type;
        a.last = now_;
        if (own) {
            a.signal_db = a.own == 0 ? p.level_db : a.signal_db + 0.2 * (p.level_db - a.signal_db);
            a.own++;
            if (std::isfinite(mhz)) a.mhz = mhz;
        } else {
            a.relayed++;
        }
        const bool position = ogn && !f.encrypted && f.ogn.report == -1;
        if (ogn) {
            a.has_ogn = true;
            a.encrypted = f.encrypted;
            if (!f.encrypted) apply(a, f.ogn, now_);
        } else {
            a.has_flarm = true;
        }

        host.valid(t0, t1);
        if (ogn && f.emergency && !a.emergency_told) {
            a.emergency_told = true;
            std::string s = "\xE2\x9A\xA0 EMERGENCY flag from " + id;   // "⚠"
            if (a.has_pos && !a.stealth) {
                char b[64];
                snprintf(b, sizeof b, " at %.4f, %.4f", a.lat, a.lon);
                s += b;
                if (a.alt_known) s += fmt(", %.0f m", a.alt);
            }
            host.event(s, 30);
        }
        if (!ogn && first_own) {
            host.event("New FLARM aircraft " + id + on_mhz + " - its position is encrypted (not decoded): ID and bearing only", 5);
        } else if (ogn && first_any) {
            std::string s = "New OGN tracker " + id;
            if (f.addr_type == 2) s += " (FLARM address)";
            if (f.encrypted) s += " - encrypted by its owner: ID and bearing only";
            else if (position && f.ogn.stealth) s += " - no-track (stealth): its position is not shown";
            if (!own) s += " (relayed)";
            host.event(s + on_mhz, 5);
        }
        if (own) talker(a, p, t0, t1);
        if (host.raw_wanted())
            host.raw(std::string(ogn ? "OGN " : "FLARM ") + hex(f.b, pkt::BYTES) + (std::isfinite(mhz) ? fmt(" %.3f", mhz) : "") +
                     fmt(" %.1f", p.level_db));
        if (host.verbose()) verbose_line(a, p, mhz);
        refresh(a);
    }

    void talker(const Aircraft& a, const flarm::RxPacket& p, double t0, double t1) {
        kp::Talker t;
        t.id = a.id;
        t.label = label(a);
        t.start_s = t0;
        t.end_s = t1;
        t.packet = true;
        t.freq_hz = p.carrier_hz;
        t.bw_hz = CHAN_BW_HZ;
        t.avg_s = TALKER_AVG_S;
        t.keep_s = TALKER_KEEP_S;
        if (trackable(a) && a.has_pos && a.alt_known && now_ - a.pos_t < TALKER_POS_AGE_S) {
            t.lat = a.lat;
            t.lon = a.lon;
            t.alt_m = a.alt;
        }
        host.talker(t);
    }

    void verbose_line(const Aircraft& a, const flarm::RxPacket& p, double mhz) {
        const pkt::Frame& f = p.frame;
        char b[96];
        std::string s;
        if (f.proto == pkt::Proto::FLARM) {
            snprintf(b, sizeof b, " type byte %02X", f.b[3]);
            s = "FLARM " + a.id + b + (f.inverted ? " (inverted)" : "") + (std::isfinite(mhz) ? fmt(" %.3f MHz", mhz) : "");
            snprintf(b, sizeof b, " %.1f dBFS %.0f dB", p.level_db, p.snr_db);
            host.event(s + b, 0);
            return;
        }
        s = "OGN " + a.id;
        if (f.encrypted) {
            s += " encrypted";
        } else if (f.ogn.report == -1) {
            // a no-track tracker: nothing of its position, not even the altitude
            if (a.stealth) {
                host.event(s + " no-track", 0);
                return;
            }
            snprintf(b, sizeof b, " %.5f,%.5f %.0f m ", f.ogn.lat, f.ogn.lon, f.ogn.alt_m);
            s += b + std::string(pkt::type_name(f.ogn.acft_type));
        } else {
            s += " report " + std::to_string(f.ogn.report);
        }
        if (f.relay) s += " relayed";
        if (f.corrected) s += " (" + std::to_string(f.corrected) + " bits corrected)";
        host.event(s + fmt(" %.1f dBFS", p.level_db), 0);
    }

    // --- table / map -----------------------------------------------------------------
    void refresh(Aircraft& a) {
        if (shown(a)) {
            if (now_ - a.map_t >= REFRESH_S) {
                host.map_point(map_point(a));
                a.map_t = now_;
                a.on_map = true;
            }
        } else if (a.on_map && !trackable(a)) {
            host.map_remove(a.id);
            a.on_map = false;
        }
        if (now_ - a.row_t >= REFRESH_S) {
            a.row_t = now_;
            row(a);
        }
    }

    static const char* kind_of(int type) {
        switch (type) {
            case 4: case 6: case 7: return "person";
            case 11: case 12: return "balloon";
            case 14: return "vehicle";
            case 15: return "point";
            default: return "aircraft";
        }
    }

    kp::MapPoint map_point(const Aircraft& a) const {
        kp::MapPoint m;
        m.id = a.id;
        m.lat = a.lat;
        m.lon = a.lon;
        m.label = cut_utf8(label(a), 40);
        m.kind = kind_of(a.type);
        m.heading = std::isfinite(a.track) ? static_cast<float>(a.track) : NAN;
        if (a.alt_known) m.altitude_m = static_cast<float>(a.alt);
        if (std::isfinite(a.speed)) m.speed_kmh = static_cast<float>(a.speed * 3.6);
        m.ttl_s = MAP_TTL_S;
        std::string info = "ID: " + a.id + "\nProtocol: OGN tracker";
        if (a.type >= 0) info += std::string("\nAircraft type: ") + pkt::type_name(a.type);
        for (const auto& [k, v] : a.info) info += "\n" + k + ": " + v;
        if (a.alt_known) info += fmt("\nAltitude: %.0f m (GNSS)", a.alt);
        if (a.palt_ok) info += fmt("\nPressure altitude: %.0f m", a.palt);
        if (std::isfinite(a.climb)) info += fmt("\nClimb: %+.1f m/s", a.climb);
        if (std::isfinite(a.speed)) info += fmt("\nGround speed: %.0f km/h", a.speed * 3.6);
        if (std::isfinite(a.track)) info += fmt("\nTrack: %.0f\xC2\xB0", a.track);
        if (std::isfinite(a.turn)) info += fmt("\nTurn rate: %+.1f \xC2\xB0/s", a.turn);
        if (std::isfinite(a.mhz)) info += fmt("\nChannel: %.3f MHz", a.mhz);
        if (std::isfinite(a.signal_db)) info += fmt("\nSignal: %.1f dBFS", a.signal_db);
        m.info = info;
        return m;
    }

    void row(const Aircraft& a) {
        const char* proto = a.has_flarm && a.has_ogn ? "FLARM + OGN" : a.has_flarm ? "FLARM" : "OGN";
        const bool sh = shown(a);
        double slat = 0, slon = 0;
        const bool st = sh && host.station(&slat, &slon);
        std::string notes;
        if (a.has_flarm && !a.has_ogn) notes = "position encrypted (not decoded)";
        if (a.encrypted) notes = "encrypted by its owner";
        if (a.stealth) notes = "no-track (stealth)";
        if (a.relayed > 0) notes += (notes.empty() ? "" : "; ") + std::to_string(a.relayed) + " relayed";
        host.table_row(a.id, {a.id,
                              proto,
                              a.type >= 0 ? pkt::type_name(a.type) : "",
                              registration(a),
                              sh && a.alt_known ? fmt("%.0f", a.alt) : "",
                              sh && std::isfinite(a.climb) ? fmt("%+.1f", a.climb) : "",
                              sh && std::isfinite(a.speed) ? fmt("%.0f", a.speed * 3.6) : "",
                              sh && std::isfinite(a.track) ? fmt("%.0f", a.track) : "",
                              sh ? fmt("%.5f", a.lat) : "",
                              sh ? fmt("%.5f", a.lon) : "",
                              st ? fmt("%.1f", haversine_km(slat, slon, a.lat, a.lon)) : "",
                              st ? fmt("%.0f", bearing_deg(slat, slon, a.lat, a.lon)) : "",
                              notes,
                              std::isfinite(a.mhz) ? fmt("%.3f", a.mhz) : "",
                              std::isfinite(a.signal_db) ? fmt("%.1f", a.signal_db) : "",
                              std::to_string(a.own)});
        std::vector<std::pair<std::string, std::string>> det = {{"ID", a.id}, {"Protocol", proto}};
        for (const auto& [k, v] : a.info) det.emplace_back(k, v);
        if (!a.status.empty()) det.emplace_back("Status", a.status);
        if (sh && a.palt_ok) det.emplace_back("Pressure altitude", fmt("%.0f m", a.palt));
        if (sh && std::isfinite(a.turn)) det.emplace_back("Turn rate", fmt("%+.1f \xC2\xB0/s", a.turn));
        if (a.has_flarm)
            det.emplace_back("FLARM payload", "encrypted with keys FLARM has not published - not decoded (ID and DoA only)");
        if (a.stealth)
            det.emplace_back("Privacy", "the tracker asks not to be tracked: no position shown, mapped or logged");
        host.table_detail(a.id, det);
    }

    void housekeeping() {
        if (now_ - house_t_ < HOUSE_S) return;
        house_t_ = now_;
        for (auto it = acft_.begin(); it != acft_.end();) {
            Aircraft& a = it->second;
            if (now_ - a.last >= FORGET_S) {
                if (a.on_map) host.map_remove(a.id);
                host.table_remove(a.id);
                it = acft_.erase(it);
                continue;
            }
            if (a.on_map && now_ - a.pos_t > SHOW_AGE_S) {
                host.map_remove(a.id);
                a.on_map = false;
            }
            ++it;
        }
    }

    void facts() {
        int nf = 0, no = 0;
        for (const auto& [k, a] : acft_) (a.has_ogn ? no : nf)++;
        host.fact("Channels", chan_text_);
        host.fact("Aircraft", std::to_string(nf) + " FLARM, " + std::to_string(no) + " OGN tracker");
        host.fact("Packets", std::to_string(flarm_pk_) + " FLARM, " + std::to_string(ogn_pk_) + " OGN (" +
                                 std::to_string(rx_.demodulated()) + " bursts demodulated, " +
                                 std::to_string(rx_.broadband()) + " broadband events skipped)");
    }
};

}  // namespace

KRAKEN_PLUGIN(FlarmDecoder,
              {.id = "flarm",
               .name = "FLARM / OGN trackers (868 / 915 MHz)",
               .category = "Aviation",
               .description = "Glider / light aircraft collision avoidance radios: FLARM (shown by its radio ID and "
                              "bearing - its position is encrypted and not decoded) and OGN trackers (open protocol: "
                              "position, altitude, climb, aircraft type, registration). The VFO sits on its tuner's "
                              "centre with the full bandwidth and every channel of the band plans inside it is received "
                              "(EU / NZ 868.2, 868.4, 869.25 MHz: tune to 868.8 MHz). Each aircraft gets its own DoA in "
                              "the 🗺 Map's 📡 DF panel",
               .version = "2.0",
               .sample_rate = FS,
               .min_vfo_rate = FS,
               .author = "KrakenSDR",
               .options = {{"band", "Band", "auto",
                            "auto=From the station location|eu=Europe / Africa (868 MHz)|nz=New Zealand (869.25 MHz)|"
                            "au=Australia / S. America (917-926 MHz)|us=North America (902-928 MHz)|il=Israel (916.2 MHz)",
                            "Where the receiver is tuned when no FLARM / OGN channel is inside the VFO (every known "
                            "channel inside it is received whatever this says). 868.8 MHz covers Europe's and New "
                            "Zealand's channels; the hopping bands get 5-6 of their channels"},
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
