// Radiosonde decoder: weather balloon sondes on 400-406 MHz - Vaisala RS41,
// Graw DFM, Meteomodem M10 / M20, InterMet iMet-4 / iMet-54, Lockheed Martin
// LMS6, Meteo-Radiy MRZ - all side by side on the VFO's signal.
//
// Front end: two channel filters on the 48 kHz complex input (+-7 kHz for
// the 2400-4800 Bd types, +-12 kHz for the 9600 chip/s Meteomodems and the
// iMet-4's AFSK), an FM discriminator after each; every family runs its own
// receiver on every block (families.hpp) and returns a Record per frame that
// passed its checks. Here the records become sondes (type + serial): merged
// state, flight events (standard pressure levels, burst, landing), facts of
// the latest sonde, a table row and a map point per sonde (at most once a
// second each), and a talker packet per frame - the frame's first 0.24 s from
// its sync - so every sonde gets its own bearing.
//
// SONDE_DUMP (any value) in the environment prints one line per frame to
// stderr (offline comparisons).

#include "families.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace sonde {
std::unique_ptr<Family> make_rs41();
std::unique_ptr<Family> make_dfm();
std::unique_ptr<Family> make_meteomodem();
std::unique_ptr<Family> make_imet4();
std::unique_ptr<Family> make_imet54();
std::unique_ptr<Family> make_lms6();
std::unique_ptr<Family> make_mrz();

std::vector<std::unique_ptr<Family>> make_families() {
    std::vector<std::unique_ptr<Family>> f;
    f.push_back(make_rs41());
    f.push_back(make_dfm());
    f.push_back(make_meteomodem());
    f.push_back(make_imet4());
    f.push_back(make_imet54());
    f.push_back(make_lms6());
    f.push_back(make_mrz());
    return f;
}
}  // namespace sonde

namespace {

using sonde::Record;

// facts / table / map per sonde at most this often: about once a second, but
// every record of a sonde that sends a frame every 0.8 s or more (LMS6 0.87 s,
// DFM cycles of 4 frames = 0.896 s)
constexpr double PUBLISH_S = 0.75;
constexpr double MAP_TTL_S = 10800;          // landed sondes go on transmitting for hours
const double LEVELS_HPA[] = {1000, 925, 850, 700, 500, 400, 300, 250, 200, 150, 100, 70, 50, 30, 20, 10, 7, 5, 3, 2, 1};
constexpr int NLEVELS = sizeof LEVELS_HPA / sizeof LEVELS_HPA[0];
const char* FIXED_KEYS[] = {"Sonde",       "Frame",       "Time",        "Position",       "Altitude",
                            "Climb",       "Flight",      "Temperature", "Humidity",       "Dew point",
                            "Pressure",    "Wind",        "Humidity sensor temp.",         "Internal temp.",
                            "Battery",     "Satellites",  "Burst countdown",               "TX frequency",
                            "From station"};

std::string num(double v, int d) {
    if (!std::isfinite(v)) return "";
    char b[64];
    snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}
double pnan(double v) { return std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN(); }   // prints "nan"

// "YYYY-MM-DD hh:mm:ss UTC" of floor(t)
std::string utc_text(double t) {
    const int64_t s = static_cast<int64_t>(std::floor(t));
    int64_t days = s / 86400, sec = s % 86400;
    if (sec < 0) {
        sec += 86400;
        days--;
    }
    // civil from days (proleptic Gregorian)
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2);
    char b[64];
    snprintf(b, sizeof b, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld UTC", static_cast<long long>(y), static_cast<long long>(m),
             static_cast<long long>(d), static_cast<long long>(sec / 3600), static_cast<long long>(sec / 60 % 60),
             static_cast<long long>(sec % 60));
    return b;
}

std::string clean(const std::string& s) {   // control characters -> '?'
    std::string o = s;
    for (char& c : o)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = '?';
    return o;
}

struct Fix { double t, lat, lon, alt; };

struct Sonde {
    std::string key;
    Record m;                                // merged state
    double first = 0, last = 0;
    uint64_t frames = 0;
    double max_alt = NAN;
    bool burst = false, landed = false;
    double vv_s = NAN;                       // smoothed vertical speed
    double still = NAN;                      // since when |vv_s| < 1 (after burst)
    int next_level = -2;                     // -2 = not started, -1 = none left
    double pub = -1e18;
    std::deque<Fix> fixes;
};

class Radiosonde : public kp::Decoder {
public:
    explicit Radiosonde(kp::Host& h) : Decoder(h) {
        narrow_.set_taps(kp::fir_lowpass(41, 7000.0f, static_cast<float>(sonde::FS)));
        wide_.set_taps(kp::fir_lowpass(31, 12000.0f, static_cast<float>(sonde::FS)));
        fam_ = sonde::make_families();
        dump_ = std::getenv("SONDE_DUMP") != nullptr;
        host.table_columns({"Type", "Serial", "Alt m", "Climb m/s", "Temp °C", "RH %", "hPa", "Wind", "km"});
    }

    void reset() override {
        narrow_.reset();
        wide_.reset();
        fmn_.reset();
        fmw_.reset();
        for (auto& f : fam_) f->reset();
        idx_ = 0;
        sondes_.clear();
    }

    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        dn_.resize(n);
        dw_.resize(n);
        for (size_t i = 0; i < n; i++) {
            dn_[i] = fmn_.push(narrow_.push(x[i]));
            dw_[i] = fmw_.push(wide_.push(x[i]));
        }
        sonde::Ctx ctx;
        ctx.clk.t0 = t0;
        ctx.clk.idx0 = idx_;
        ctx.raw = host.raw_wanted();
        recs_.clear();
        for (auto& f : fam_) f->process(dn_.data(), dw_.data(), n, idx_, ctx, recs_);
        idx_ += static_cast<int64_t>(n);
        std::stable_sort(recs_.begin(), recs_.end(), [](const Record& a, const Record& b) {
            const double ta = std::isfinite(a.t_end) ? a.t_end : 1e300, tb = std::isfinite(b.t_end) ? b.t_end : 1e300;
            return ta < tb;
        });
        for (auto& r : recs_) record(r, t0);
    }

private:
    kp::Fir<kp::cf> narrow_, wide_;
    kp::FmDemod fmn_{static_cast<float>(sonde::FS)}, fmw_{static_cast<float>(sonde::FS)};
    std::vector<float> dn_, dw_;
    std::vector<std::unique_ptr<sonde::Family>> fam_;
    std::vector<Record> recs_;
    int64_t idx_ = 0;
    bool dump_ = false;
    std::map<std::string, Sonde> sondes_;

    static std::string type_of(const Record& r) { return r.model.empty() ? r.family : r.model; }

    void record(Record& r, double tin) {
        if (r.serial.empty()) return;
        if (dump_)
            fprintf(stderr,
                    "FRAME %s %s %lld t=%.2f tod=%.0f utc=%.0f lat=%.5f lon=%.5f alt=%.1f vh=%.2f hdg=%.1f vv=%.2f T=%.1f RH=%.1f "
                    "P=%.1f batt=%.2f\n",
                    r.family.c_str(), r.serial.c_str(), r.frame, tin, pnan(r.tod), pnan(r.utc), pnan(r.lat), pnan(r.lon), pnan(r.alt),
                    pnan(r.vh), pnan(r.hdg), pnan(r.vv), pnan(r.T), pnan(r.RH), pnan(r.P), pnan(r.batt));
        if (std::isfinite(r.t_first) && std::isfinite(r.t_end) && r.t_end > r.t_first) host.valid(r.t_first, r.t_end);
        else host.valid();
        if (std::isfinite(r.offset)) host.freq_error(static_cast<float>(r.offset));
        if (!r.hex.empty() && host.raw_wanted()) host.raw(r.family + " " + r.serial + " " + r.hex);

        const std::string key = r.family + " " + r.serial;
        const bool fresh = sondes_.find(key) == sondes_.end();
        Sonde& s = sondes_[key];
        derive_velocity(s, r, tin);
        if (fresh) {
            s.key = key;
            s.first = tin;
            host.event("🎈 New " + type_of(r) + " " + r.serial, 60.0);
        }
        merge(s, r, tin);
        flight_events(s, r, tin);
        talker(s, r);
        if (tin - s.pub >= PUBLISH_S) {
            s.pub = tin;
            publish(s);
        }
    }

    // horizontal / vertical speed from the fixes up to 10 s back (types
    // without a velocity field)
    static void derive_velocity(Sonde& s, Record& r, double tin) {
        if (!std::isfinite(r.lat) || !std::isfinite(r.alt) || !std::isfinite(r.lon)) return;
        double t = std::isfinite(r.utc) ? r.utc : std::isfinite(r.tod) ? r.tod : tin;
        if (!s.fixes.empty() && t < s.fixes.back().t - 43000) t += 86400;   // time of day past midnight
        while (!s.fixes.empty() && (s.fixes.front().t < t - 10 || s.fixes.front().t > t)) s.fixes.pop_front();
        if (!std::isfinite(r.vh) && !s.fixes.empty() && t - s.fixes.front().t >= 3 && (r.sats < 0 || r.sats >= 5)) {
            const Fix& o = s.fixes.front();
            const double dt = t - o.t;
            const double vn = (r.lat - o.lat) * 111320 / dt;
            const double ve = (r.lon - o.lon) * 111320 * std::cos(r.lat * M_PI / 180) / dt;
            sonde::speed_dir(ve, vn, &r.vh, &r.hdg);
            r.vv = (r.alt - o.alt) / dt;
        }
        s.fixes.push_back({t, r.lat, r.lon, r.alt});
    }

    static void merge(Sonde& s, const Record& r, double tin) {
        Record& m = s.m;
        auto txt = [](std::string& a, const std::string& b) {
            if (!b.empty()) a = b;
        };
        auto numv = [](double& a, double b) {
            if (std::isfinite(b)) a = b;
        };
        txt(m.family, r.family);
        txt(m.model, r.model);
        txt(m.serial, r.serial);
        if (r.frame >= 0) m.frame = r.frame;
        if (r.sats >= 0) m.sats = r.sats;
        if (r.countdown >= 0) m.countdown = r.countdown;
        numv(m.utc, r.utc);
        numv(m.tod, r.tod);
        numv(m.T, r.T);
        numv(m.RH, r.RH);
        numv(m.P, r.P);
        numv(m.TU, r.TU);
        numv(m.Ti, r.Ti);
        numv(m.batt, r.batt);
        numv(m.txf, r.txf);
        numv(m.offset, r.offset);
        // position and motion belong together
        if (std::isfinite(r.lat) && std::isfinite(r.lon)) {
            m.lat = r.lat;
            m.lon = r.lon;
            m.alt = r.alt;
            m.vh = r.vh;
            m.hdg = r.hdg;
            m.vv = r.vv;
        }
        bool cal = false;
        for (const auto& [k, v] : r.extras) {
            if (k == "Calibration") cal = true;
            auto it = std::find_if(m.extras.begin(), m.extras.end(), [&](const auto& e) { return e.first == k; });
            if (it != m.extras.end()) it->second = v;
            else m.extras.emplace_back(k, v);
        }
        if (!cal)
            m.extras.erase(std::remove_if(m.extras.begin(), m.extras.end(), [](const auto& e) { return e.first == "Calibration"; }),
                           m.extras.end());
        if (std::isfinite(r.alt)) s.max_alt = std::isfinite(s.max_alt) ? std::max(s.max_alt, r.alt) : r.alt;
        if (std::isfinite(r.vv)) s.vv_s = std::isfinite(s.vv_s) ? s.vv_s + 0.2 * (r.vv - s.vv_s) : r.vv;
        s.last = tin;
        s.frames++;
    }

    void flight_events(Sonde& s, const Record& r, double tin) {
        const Record& m = s.m;
        if (!s.burst) {
            // standard pressure levels passed on the way up
            bool est = false;
            double p = m.P;
            if (!std::isfinite(p) && std::isfinite(r.alt)) {
                p = sonde::isa_pressure_hpa(r.alt);
                est = true;
            }
            if (std::isfinite(p)) {
                if (s.next_level == -2) {
                    s.next_level = -1;
                    for (int i = 0; i < NLEVELS; i++)
                        if (LEVELS_HPA[i] < p) {
                            s.next_level = i;
                            break;
                        }
                }
                while (s.next_level >= 0 && p <= LEVELS_HPA[s.next_level] && s.vv_s > 0.5) {
                    std::string e = "📈 " + m.serial + " " + num(LEVELS_HPA[s.next_level], 0) + " hPa" + (est ? " (est.)" : "") +
                                    ": " + num(m.alt, 0) + " m";
                    if (std::isfinite(m.T)) e += ", " + num(m.T, 1) + " °C";
                    if (std::isfinite(m.RH))
                        e += ", RH " + num(m.RH, 0) + " %, dew point " + num(sonde::dew_point(m.T, m.RH), 1) + " °C";
                    if (std::isfinite(m.vh) && std::isfinite(m.hdg))
                        e += ", wind " + num(std::fmod(m.hdg + 180, 360), 0) + "° " + num(m.vh * 3.6, 0) + " km/h";
                    host.event(e, 600.0);
                    s.next_level = s.next_level + 1 < NLEVELS ? s.next_level + 1 : -1;
                }
            }
            if (s.max_alt > 3000 && m.alt < s.max_alt - 300 && s.vv_s < -3) {
                s.burst = true;
                host.event("⚠ " + m.serial + " burst at " + num(s.max_alt, 0) + " m - descending", 3600.0);
            }
        } else if (!s.landed && std::isfinite(s.vv_s)) {
            if (std::fabs(s.vv_s) < 1) {
                if (!std::isfinite(s.still)) s.still = tin;
                else if (tin - s.still > 30) {
                    s.landed = true;
                    host.event("🪂 " + m.serial + " landed at " + num(m.lat, 5) + ", " + num(m.lon, 5) + " (" + num(m.alt, 0) + " m)",
                               3600.0);
                }
            } else {
                s.still = NAN;
            }
        }
    }

    // the frame's beginning, from its sync, as a packet of this sonde
    void talker(const Sonde& s, const Record& r) {
        if (!std::isfinite(r.t_first) || !std::isfinite(r.t_end) || !(r.t_end > r.t_first)) return;
        if (r.family == "iMet-4") return;   // no serial on the air: one talker would merge them all
        kp::Talker t;
        t.id = s.key.substr(0, 32);
        t.label = type_of(s.m) + (std::isfinite(s.m.alt) ? " · " + num(s.m.alt, 0) + " m" : "");
        t.start_s = r.t_first + 0.002;
        t.end_s = std::min(r.t_end - 0.002, t.start_s + 0.24);
        if (!(t.end_s > t.start_s)) return;
        t.packet = true;
        t.freq_hz = 0;   // the host's AFC centres the sonde
        t.bw_hz = r.family == "M10" || r.family == "M20" || r.family.rfind("iMet-4", 0) == 0 ? 24000 : 15000;
        t.avg_s = 10;
        t.keep_s = 10800;
        host.talker(t);
    }

    std::vector<std::pair<std::string, std::string>> rows(const Sonde& s) const {
        const Record& m = s.m;
        std::vector<std::pair<std::string, std::string>> v;
        auto add = [&](const char* k, const std::string& val) {
            if (!val.empty()) v.emplace_back(k, val);
        };
        add("Sonde", type_of(m) + " " + m.serial);
        if (m.frame >= 0) add("Frame", std::to_string(m.frame));
        if (std::isfinite(m.utc)) add("Time", utc_text(m.utc));
        else if (std::isfinite(m.tod)) {
            const int64_t t = static_cast<int64_t>(std::floor(m.tod)) % 86400;
            char b[32];
            snprintf(b, sizeof b, "%02d:%02d:%02d UTC", static_cast<int>(t / 3600), static_cast<int>(t / 60 % 60), static_cast<int>(t % 60));
            add("Time", b);
        }
        if (std::isfinite(m.lat) && std::isfinite(m.lon)) add("Position", num(m.lat, 5) + ", " + num(m.lon, 5));
        if (std::isfinite(m.alt))
            add("Altitude", num(m.alt, 0) + " m" + (s.max_alt > m.alt + 50 ? " (max " + num(s.max_alt, 0) + " m)" : ""));
        if (std::isfinite(m.vv)) add("Climb", num(m.vv, 1) + " m/s");
        std::string flight;
        if (s.landed) flight = "landed";
        else if (s.burst) flight = "descending (burst at " + num(s.max_alt, 0) + " m)";
        else if (!std::isfinite(s.vv_s)) flight = "";
        else if (s.vv_s > 1) flight = "ascending";
        else if (s.vv_s < -1) flight = "descending";
        else flight = "level / on the ground";
        add("Flight", flight);
        if (std::isfinite(m.T)) add("Temperature", num(m.T, 1) + " °C");
        if (std::isfinite(m.RH)) add("Humidity", num(m.RH, 1) + " %");
        const double dp = sonde::dew_point(m.T, m.RH);
        if (std::isfinite(dp)) add("Dew point", num(dp, 1) + " °C");
        if (std::isfinite(m.P)) add("Pressure", num(m.P, 1) + " hPa");
        else if (std::isfinite(m.alt)) add("Pressure", num(sonde::isa_pressure_hpa(m.alt), 1) + " hPa (from altitude)");
        if (std::isfinite(m.vh) && std::isfinite(m.hdg))
            add("Wind", "from " + num(std::fmod(m.hdg + 180, 360), 0) + "° at " + num(m.vh * 3.6, 1) + " km/h (" + num(m.vh, 1) + " m/s)");
        if (std::isfinite(m.TU)) add("Humidity sensor temp.", num(m.TU, 1) + " °C");
        if (std::isfinite(m.Ti)) add("Internal temp.", num(m.Ti, 1) + " °C");
        if (std::isfinite(m.batt)) add("Battery", num(m.batt, 2) + " V");
        if (m.sats >= 0) add("Satellites", std::to_string(m.sats));
        if (m.countdown >= 0)
            add("Burst countdown", std::to_string(m.countdown / 3600) + " h " + std::to_string(m.countdown / 60 % 60) + " min");
        if (std::isfinite(m.txf)) add("TX frequency", num(m.txf, 3) + " MHz");
        double sl, so;
        if (std::isfinite(m.lat) && std::isfinite(m.lon) && host.station(&sl, &so)) {
            const double d = sonde::distance_km(sl, so, m.lat, m.lon);
            add("From station", num(d, d < 10 ? 1 : 0) + " km, " + num(sonde::bearing_deg(sl, so, m.lat, m.lon), 0) + "°");
        }
        for (const auto& [k, val] : m.extras)
            if (!val.empty()) v.emplace_back(clean(k), clean(val));
        return v;
    }

    void publish(const Sonde& s) {
        const Record& m = s.m;
        const auto rw = rows(s);
        for (const char* k : FIXED_KEYS)
            if (std::none_of(rw.begin(), rw.end(), [&](const auto& e) { return e.first == k; })) host.fact(k, "");
        for (const auto& [k, v] : rw) host.fact(k, v);
        host.fact("Sondes heard", std::to_string(sondes_.size()));

        // table row
        double sl, so;
        const bool st = std::isfinite(m.lat) && std::isfinite(m.lon) && host.station(&sl, &so);
        std::string wind;
        if (std::isfinite(m.vh) && std::isfinite(m.hdg))
            wind = num(std::fmod(m.hdg + 180, 360), 0) + "° " + num(m.vh * 3.6, 0) + " km/h";
        const std::string hpa = std::isfinite(m.P) ? num(m.P, 1) : num(sonde::isa_pressure_hpa(m.alt), 1);
        host.table_row(s.key, {type_of(m), m.serial, num(m.alt, 0), num(m.vv, 1), num(m.T, 1), num(m.RH, 0), hpa, wind,
                               st ? num(sonde::distance_km(sl, so, m.lat, m.lon), 1) : ""});

        // map point
        if (std::isfinite(m.lat) && std::isfinite(m.lon)) {
            kp::MapPoint p;
            p.id = s.key;
            p.lat = m.lat;
            p.lon = m.lon;
            p.label = m.serial;
            p.kind = "balloon";
            p.heading = std::isfinite(m.hdg) ? static_cast<float>(m.hdg) : NAN;
            p.altitude_m = std::isfinite(m.alt) ? static_cast<float>(m.alt) : NAN;
            p.speed_kmh = std::isfinite(m.vh) ? static_cast<float>(m.vh * 3.6) : NAN;
            for (const auto& [k, v] : rw) p.info += k + ": " + v + "\n";
            p.ttl_s = MAP_TTL_S;
            host.map_point(p);
        }
    }
};

}  // namespace

KRAKEN_PLUGIN(Radiosonde, {.id = "radiosonde",
                           .name = "Radiosonde",
                           .description = "Weather balloon sondes (400-406 MHz): Vaisala RS41, Graw DFM, Meteomodem M10 / M20, "
                                          "iMet-4 / iMet-54, LMS6, MRZ - position, altitude, temperature, humidity, pressure, wind",
                           .version = "2.0",
                           .sample_rate = sonde::FS,
                           .min_vfo_rate = 24000,
                           .author = "KrakenSDR",
                           .map = true,
                           .talkers = true})
