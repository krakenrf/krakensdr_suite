// ISM band devices (315 / 345 / 433 / 868 / 915 MHz): weather stations and
// thermometers, tyre pressure sensors, key fobs and remotes, door / motion /
// smoke sensors, energy meters - the short OOK / FSK bursts of the licence-
// free bands, decoded at 240 kHz (+-120 kHz around the VFO).
//
// Every decoded transmission is reported as a kp::Talker packet of its
// device (model + id), with the burst's own carrier frequency and bandwidth:
// kraken_doa filters exactly that device's channel out of every antenna and
// gives each device its own DoA (📡 DF panel on the 🗺 Map), one bearing
// per transmission in its history. Signals no protocol knows are reported as
// unknown devices when they repeat a code (see generic.cpp).
//
// Pipeline: frontend.cpp (bursts -> OOK / FSK pulse trains) -> bits.cpp
// (slicers) -> p_*.cpp (protocols) -> here (devices, table, events, DoA).

#include "frontend.hpp"
#include "ism.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace {

using ism::Kind;
using ism::Msg;

constexpr double DEVICE_TTL_S = 3600;       // a device not heard for this long is forgotten
constexpr size_t MAX_DEVICES = 400;
constexpr double MAX_SPAN_S = 0.24;         // a talker packet (kraken_doa takes up to 0.25 s)

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::WEATHER: return "Weather";
        case Kind::TPMS: return "TPMS";
        case Kind::REMOTE: return "Remote";
        case Kind::SECURITY: return "Security";
        case Kind::ENERGY: return "Energy";
        case Kind::UNKNOWN: return "Unknown";
        default: return "Sensor";
    }
}
const char* kind_icon(Kind k) {
    switch (k) {
        case Kind::WEATHER: return "🌡";
        case Kind::TPMS: return "🛞";
        case Kind::REMOTE: return "🔑";
        case Kind::SECURITY: return "🚨";
        case Kind::ENERGY: return "⚡";
        case Kind::UNKNOWN: return "❓";
        default: return "📟";
    }
}

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

struct Device {
    std::string key, tid;
    std::string model, id;
    Kind kind = Kind::OTHER;
    double first = 0, last = 0;
    uint64_t count = 0;
    Msg msg;                        // the latest message
    std::string summary;
    double rf_hz = NAN;             // the device's own frequency (Hz), NAN = unknown
    double fc = 0;                  // offset in the input (Hz)
    float snr = NAN;
    bool batt_low_told = false;
    double table_t = -1e9;
    bool in_table = false;
};

class IsmDecoder : public kp::Decoder {
public:
    explicit IsmDecoder(kp::Host& h) : Decoder(h) {
        host.table_columns(COLS);
        fe_.set_threshold_db(thr_db_);
    }

    void reset() override {
        fe_.reset();
        for (auto& [k, d] : dev_)
            if (d.in_table) host.table_remove(d.tid);
        dev_.clear();
        classes_.clear();
        total_ = undecoded_ = 0;
        house_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "units") imperial_ = value == "us";
        else if (key == "doa_avg") avg_s_ = value.empty() ? 1.0 : std::clamp(atof(value.c_str()), 0.2, 3600.0);
        else if (key == "unknown") unknown_ = value != "0";
        else if (key == "sens") {
            thr_db_ = value.empty() ? 10.0 : std::clamp(atof(value.c_str()), 6.0, 30.0);
            fe_.set_threshold_db(thr_db_);
        }
    }

    void process(const kp::cf* x, size_t n) override {
        t_off_ = host.time() - static_cast<double>(fe_.abs_pos()) / ism::FS;
        now_ = host.time();
        bursts_.clear();
        fe_.push(reinterpret_cast<const ism::cf*>(x), n, &bursts_);
        for (const auto& b : bursts_) burst(b);
        now_ = host.time() + static_cast<double>(n) / ism::FS;
        housekeeping();
    }

private:
    const std::vector<std::string> COLS = {"Model", "ID", "Type", "Ch", "Reading", "Battery", "MHz", "SNR dB", "Msgs"};
    ism::FrontEnd fe_;
    std::vector<ism::Burst> bursts_;
    double t_off_ = 0, now_ = 0;
    bool imperial_ = false, unknown_ = true;
    double avg_s_ = 1.0, thr_db_ = 10.0;
    std::map<std::string, Device> dev_;
    // unknown signals without a repeated code, grouped by their signal class:
    // a class becomes a device only when it is heard again within 10 min (a
    // one-off fragment, collision or wake-up burst doesn't)
    std::map<std::string, double> classes_;
    bool class_seen_again(const std::string& id) {
        auto it = classes_.find(id);
        const bool again = it != classes_.end() && now_ - it->second < 600;
        if (classes_.size() > 500) classes_.clear();
        classes_[id] = now_;
        return again;
    }
    uint64_t total_ = 0, undecoded_ = 0, bursts_seen_ = 0;
    double house_t_ = -1e9;
    const bool dump_ = getenv("ISM_DUMP") != nullptr;   // offline debugging: every burst's pulses

    void dump(const ism::Burst& b) {
        std::string s = fmt("burst %.4f s", t_of(b.a)) + fmt(" .. %.4f", t_of(b.b)) + fmt(" fc %+.1f kHz", b.fc / 1e3) +
                        fmt(" bw %.0f kHz", b.bw / 1e3) + fmt(" snr %.1f dB", b.snr_db) +
                        fmt(" fsk_sep %.1f kHz", b.fsk_sep / 1e3) + fmt(" ook %.0f", static_cast<double>(b.ook.size())) +
                        fmt(" fsk %.0f pulses", static_cast<double>(b.fsk.size()));
        host.log(s);
        for (int t = 0; t < 2; t++) {
            const auto& ps = t ? b.fsk : b.ook;
            if (ps.empty()) continue;
            std::string l = t ? "  FSK" : "  OOK";
            int k = 0;
            for (const auto& p : ps) {
                l += fmt(" %.0f", ism::s2us(p.on)) + fmt("/%.0f", p.off >= (1 << 27) ? -1.0 : ism::s2us(p.off));
                if (++k >= 400) { l += " ..."; break; }
            }
            host.log(l);
        }
    }

    double t_of(int64_t s) const { return t_off_ + static_cast<double>(s) / ism::FS; }

    // ------------------------------------------------------------ a burst ---
    void burst(const ism::Burst& b) {
        bursts_seen_++;
        if (dump_) dump(b);
        ism::Pkt pk(b);
        std::vector<Msg> msgs;
        auto run = [&](ism::Strength s) {
            for (const auto& p : ism::registry())
                if (p.strength == s) p.fn(pk, msgs);
        };
        run(ism::Strength::STRONG);
        run(ism::Strength::MEDIUM);
        if (msgs.empty()) run(ism::Strength::WEAK);
        // one message per device and package (rows repeat), spans joined
        std::vector<Msg> uniq;
        for (auto& m : msgs) {
            bool dup = false;
            for (auto& u : uniq)
                if (u.model == m.model && u.id == m.id) {
                    u.t0 = std::min(u.t0, m.t0);
                    u.t1 = std::max(u.t1, m.t1);
                    dup = true;
                    break;
                }
            if (!dup) uniq.push_back(std::move(m));
        }
        if (uniq.empty()) {
            undecoded_++;
            Msg m;
            if (unknown_ && ism::analyse_unknown(pk, &m) && (m.check[0] || class_seen_again(m.id)))
                uniq.push_back(std::move(m));
            else if (host.verbose() && b.snr_db >= 12)
                host.event(fmt("Undecoded burst: %.0f ms", 1e3 * (b.b - b.a) / ism::FS) +
                               fmt(", %+.1f kHz", b.fc / 1e3) + fmt(", %.0f dB", b.snr_db) +
                               (b.fsk_sep > 0 ? fmt(", FSK %.0f kHz apart", b.fsk_sep / 1e3) : std::string(", OOK")) +
                               fmt(", %.0f pulses", static_cast<double>(b.fsk_sep > 0 ? b.fsk.size() : b.ook.size())),
                           0.0);
        }
        // the whole burst is one device's when it alone decoded from it
        int64_t ba = b.b, bb = b.a;
        for (const auto& p : b.ook) {
            ba = std::min(ba, p.t);
            bb = std::max<int64_t>(bb, p.t + p.on);
        }
        for (auto& m : uniq) {
            if (uniq.size() == 1 && bb > ba) {
                m.t0 = std::min(m.t0, ba);
                m.t1 = std::max(m.t1, bb);
            }
            message(m, b);
        }
    }

    // ------------------------------------------------------- a message ---
    void message(const Msg& m, const ism::Burst& b) {
        total_++;
        const std::string key = m.model + "|" + m.id;
        auto it = dev_.find(key);
        const bool fresh = it == dev_.end();
        if (fresh) {
            if (dev_.size() >= MAX_DEVICES) forget_oldest();
            it = dev_.emplace(key, Device{}).first;
            Device& d = it->second;
            d.key = key;
            d.model = m.model;
            d.id = m.id;
            d.first = now_;
            std::string tid = m.id.empty() ? m.model : m.model + " " + m.id;
            if (tid.size() > 32) tid = tid.substr(0, 32);
            d.tid = tid;
        }
        Device& d = it->second;
        d.kind = m.kind;
        d.msg = m;
        d.count++;
        d.last = now_;
        d.fc = b.fc;
        d.snr = std::isfinite(d.snr) ? d.snr + 0.3f * (static_cast<float>(b.snr_db) - d.snr) : static_cast<float>(b.snr_db);
        const double rf = host.rf_hz();
        d.rf_hz = std::isfinite(rf) ? (host.rf_inverted() ? rf - b.fc : rf + b.fc) : NAN;
        d.summary = summarize(m);

        // the confirmed samples: valid frame + this device's DoA
        const int64_t span_max = static_cast<int64_t>(MAX_SPAN_S * ism::FS);
        int64_t a = m.t0, e = m.t1;
        if (e - a > span_max) a = e - span_max;   // the latest part: still in kraken_doa's ring
        if (e > a) {
            if (m.kind != Kind::UNKNOWN || m.check[0]) host.valid(t_of(a), t_of(e));
            kp::Talker t;
            t.id = d.tid;
            t.label = label(d);
            t.start_s = t_of(a);
            t.end_s = t_of(e);
            t.packet = true;
            t.freq_hz = b.fc;
            t.bw_hz = std::clamp(b.bw + 10e3, 25e3, 160e3);
            t.avg_s = avg_s_;
            host.talker(t);
        }

        const std::string who = std::string(kind_icon(m.kind)) + " " + d.tid;
        if (fresh)
            host.event("New " + std::string(kind_name(m.kind)) + ": " + d.tid +
                           (d.summary.empty() ? "" : " - " + d.summary) + mhz_text(d), 5.0);
        if (!m.event.empty())
            host.event((m.alarm ? "⚠ " : "") + who + ": " + m.event + (d.summary.empty() ? "" : " (" + d.summary + ")"),
                       3.0);
        const double batt = m.getn("battery_ok");
        if (std::isfinite(batt)) {
            if (batt < 0.5 && !d.batt_low_told) {
                d.batt_low_told = true;
                host.event("⚠ Low battery: " + d.tid, 60.0);
            } else if (batt >= 0.5) {
                d.batt_low_told = false;
            }
        }
        if (host.verbose()) host.event(who + ": " + (d.summary.empty() ? m.json() : d.summary), 0.0);
        if (host.raw_wanted()) host.raw(m.json() + fmt(" %.4f MHz", std::isfinite(d.rf_hz) ? d.rf_hz / 1e6 : b.fc / 1e6) +
                                        fmt(" %.1f dB", b.snr_db));
        host.fact("Last message", who + (d.summary.empty() ? "" : " - " + d.summary));
        if (now_ - d.table_t >= 1.0) {
            d.table_t = now_;
            d.in_table = true;
            host.table_row(d.tid, cells(d));
        }
        update_facts();
    }

    std::string mhz_text(const Device& d) const {
        return std::isfinite(d.rf_hz) ? fmt(" on %.3f MHz", d.rf_hz / 1e6) : fmt(" at %+.1f kHz", d.fc / 1e3);
    }

    std::string label(const Device& d) const {
        std::string s = d.summary;
        if (s.empty()) s = kind_name(d.kind);
        if (s.size() > 64) s = s.substr(0, 61) + "...";
        return s;
    }

    std::vector<std::string> cells(const Device& d) const {
        const Msg& m = d.msg;
        std::string ch = m.gets("channel");
        const double batt = m.getn("battery_ok");
        std::string bat = std::isfinite(batt) ? (batt >= 0.5 ? "OK" : "LOW") : "";
        if (bat.empty() && m.get("battery_V")) bat = m.gets("battery_V") + " V";
        return {d.model, d.id, kind_name(d.kind), ch, d.summary, bat,
                std::isfinite(d.rf_hz) ? fmt("%.3f", d.rf_hz / 1e6) : fmt("%+.1f k", d.fc / 1e3),
                std::isfinite(d.snr) ? fmt("%.0f", d.snr) : "", std::to_string(d.count)};
    }

    // --------------------------------------------------------- reading ---
    std::string temp(double c) const {
        return imperial_ ? fmt("%.1f °F", c * 9 / 5 + 32) : fmt("%.1f °C", c);
    }
    std::string speed_ms(double ms) const {
        return imperial_ ? fmt("%.1f mph", ms * 2.23694) : fmt("%.1f m/s", ms);
    }
    std::string summarize(const Msg& m) const {
        std::vector<std::string> p;
        auto has = [&](const char* k) { return std::isfinite(m.getn(k)); };
        // temperatures (also several probes)
        for (const auto& v : m.f) {
            if (!v.is_num || !std::isfinite(v.num)) continue;
            const std::string& k = v.key;
            if (k.rfind("temperature", 0) == 0) {
                const bool f = k.size() > 2 && k.compare(k.size() - 2, 2, "_F") == 0;
                std::string t = temp(f ? (v.num - 32) * 5 / 9 : v.num);
                if (k.find("_1_") != std::string::npos || k.find("_2_") != std::string::npos ||
                    k.find("_3_") != std::string::npos)
                    t = k.substr(12, 1) + ": " + t;
                p.push_back(t);
            }
        }
        if (has("humidity")) p.push_back(fmt("%.0f %%", m.getn("humidity")));
        if (has("pressure_kPa"))
            p.push_back(imperial_ ? fmt("%.1f PSI", m.getn("pressure_kPa") * 0.145038) : fmt("%.0f kPa", m.getn("pressure_kPa")));
        else if (has("pressure_PSI"))
            p.push_back(imperial_ ? fmt("%.1f PSI", m.getn("pressure_PSI")) : fmt("%.0f kPa", m.getn("pressure_PSI") / 0.145038));
        else if (has("pressure_bar"))
            p.push_back(imperial_ ? fmt("%.1f PSI", m.getn("pressure_bar") * 14.5038) : fmt("%.2f bar", m.getn("pressure_bar")));
        if (has("pressure_hPa")) p.push_back(fmt("%.1f hPa", m.getn("pressure_hPa")));
        double w = m.getn("wind_avg_m_s");
        if (!std::isfinite(w) && has("wind_avg_km_h")) w = m.getn("wind_avg_km_h") / 3.6;
        if (!std::isfinite(w) && has("wind_avg_mi_h")) w = m.getn("wind_avg_mi_h") / 2.23694;
        double g = m.getn("wind_max_m_s");
        if (!std::isfinite(g) && has("wind_max_km_h")) g = m.getn("wind_max_km_h") / 3.6;
        if (!std::isfinite(g) && has("wind_max_mi_h")) g = m.getn("wind_max_mi_h") / 2.23694;
        if (std::isfinite(w)) {
            std::string s = "wind " + speed_ms(w);
            if (has("wind_dir_deg")) s += fmt(" %.0f°", m.getn("wind_dir_deg"));
            if (std::isfinite(g)) s += " gust " + speed_ms(g);
            p.push_back(s);
        } else if (std::isfinite(g)) {
            p.push_back("gust " + speed_ms(g));
        }
        double r = m.getn("rain_mm");
        if (!std::isfinite(r) && has("rain_in")) r = m.getn("rain_in") * 25.4;
        if (std::isfinite(r)) p.push_back(imperial_ ? fmt("rain %.2f in", r / 25.4) : fmt("rain %.1f mm", r));
        if (has("rain_rate_mm_h")) p.push_back(fmt("%.1f mm/h", m.getn("rain_rate_mm_h")));
        if (has("uv")) p.push_back(fmt("UV %.1f", m.getn("uv")));
        if (has("uvi")) p.push_back(fmt("UVI %.1f", m.getn("uvi")));
        if (has("light_lux")) p.push_back(fmt("%.0f lux", m.getn("light_lux")));
        if (has("moisture")) p.push_back(fmt("moisture %.0f %%", m.getn("moisture")));
        if (has("pm2_5_ug_m3")) p.push_back(fmt("PM2.5 %.0f µg/m³", m.getn("pm2_5_ug_m3")));
        if (has("strike_count")) p.push_back(fmt("%.0f strikes", m.getn("strike_count")));
        if (has("storm_dist_km")) p.push_back(fmt("storm %.0f km", m.getn("storm_dist_km")));
        if (has("power_W")) p.push_back(fmt("%.0f W", m.getn("power_W")));
        for (int i = 0; i < 3; i++) {
            const std::string k = "power" + std::to_string(i) + "_W";
            if (has(k.c_str())) p.push_back(fmt("%.0f W", m.getn(k.c_str())));
        }
        if (has("current_A")) p.push_back(fmt("%.2f A", m.getn("current_A")));
        if (has("energy_kWh")) p.push_back(fmt("%.2f kWh", m.getn("energy_kWh")));
        // states / buttons (text or numbers)
        static const char* STATE_KEYS[] = {"button", "buttons", "cmd", "command", "key", "state", "event", "alarm",
                                           "tamper", "closed", "motion", "smoke", "contact_open", "flags", "status",
                                           "code", "tristate", "counter", "encrypted"};
        for (const char* k : STATE_KEYS) {
            const ism::Val* v = m.get(k);
            if (!v) continue;
            const std::string s = m.gets(k);
            if (s.empty()) continue;
            p.push_back(std::string(k) + " " + s);
        }
        std::string s;
        for (const auto& x : p) {
            if (s.size() + x.size() > 120) break;
            if (!s.empty()) s += " · ";
            s += x;
        }
        return s;
    }

    // ------------------------------------------------------ housekeeping ---
    void forget(Device& d) {
        if (d.in_table) host.table_remove(d.tid);
    }
    void forget_oldest() {
        auto old = dev_.begin();
        for (auto it = dev_.begin(); it != dev_.end(); ++it)
            if (it->second.last < old->second.last) old = it;
        if (old != dev_.end()) {
            forget(old->second);
            dev_.erase(old);
        }
    }
    void housekeeping() {
        if (now_ - house_t_ < 5.0) return;
        house_t_ = now_;
        for (auto it = dev_.begin(); it != dev_.end();) {
            if (now_ - it->second.last > DEVICE_TTL_S) {
                forget(it->second);
                it = dev_.erase(it);
                continue;
            }
            ++it;
        }
        update_facts();
    }
    void update_facts() {
        int per[7] = {};
        for (const auto& [k, d] : dev_) per[static_cast<int>(d.kind)]++;
        std::string s = std::to_string(dev_.size());
        std::string sub;
        for (int k = 0; k < 7; k++)
            if (per[k]) sub += std::string(sub.empty() ? "" : ", ") + std::to_string(per[k]) + " " +
                               kind_name(static_cast<Kind>(k));
        host.fact("Devices", s + (sub.empty() ? "" : " (" + sub + ")"));
        host.fact("Messages", std::to_string(total_));
        host.fact("Bursts", std::to_string(bursts_seen_) + " (" + std::to_string(undecoded_) + " not decoded)");
        host.fact("Noise floor", fmt("%.1f dBFS", fe_.noise_floor_db()));
    }
};

}  // namespace

KRAKEN_PLUGIN(IsmDecoder,
              {.id = "ism",
               .name = "ISM devices (433 MHz etc.)",
               .description = "Weather stations and thermometers, tyre pressure sensors (TPMS), key fobs and remotes, "
                              "door / motion / smoke sensors and energy meters on 315 / 345 / 433 / 868 / 915 MHz "
                              "(OOK and FSK, +-120 kHz around the VFO). Each device gets its own DoA - one bearing "
                              "per transmission - in the 🗺 Map's 📡 DF panel; signals no protocol knows show as "
                              "unknown devices when they repeat a code",
               .version = "1.0",
               .sample_rate = ism::FS,
               .min_vfo_rate = 200000,
               .author = "KrakenSDR",
               .options = {{"units", "Units", "si", "si=Metric (°C, kPa, m/s)|us=US (°F, PSI, mph)", ""},
                           {"doa_avg", "DoA per device", "1",
                            "1=Each transmission alone|60=Averaged over 1 min|600=Averaged over 10 min",
                            "How a device's transmissions are combined into its bearing: each one alone (the latest "
                            "transmission's lobe; earlier ones in its history) or averaged - better for a fixed sensor"},
                           {"unknown", "Unknown signals", "1", "1=Show as unknown devices|0=Ignore",
                            "Bursts no protocol decodes are shown as unknown devices when they repeat a code (so they "
                            "get a DoA of their own too)"},
                           {"sens", "Burst detection", "10", "8=8 dB (sensitive)|10=10 dB|13=13 dB|16=16 dB (strong only)",
                            "How far above the noise a burst must rise to be demodulated"}},
               .map = false,
               .manual_only = true,
               .talkers = true})
