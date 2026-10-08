// Radiosonde decoder: weather balloon sondes on 400-406 MHz, every common
// type in one plugin - they run side by side on the VFO's signal and the one
// whose frames pass their checks is shown:
//   Vaisala RS41 (-SG / -SGP / -SGM), Graw DFM-06 / -09 / -17, Meteomodem
//   M10 / M20, InterMet iMet-4 and iMet-54, Lockheed Martin LMS6, Meteo-Radiy MRZ
//
// Front end: the VFO's complex baseband at 48 kHz -> two channel filters
// (+-7 kHz for the 2400-4800 baud types, +-12 kHz for the 9600 baud
// M10 / M20 and the AFSK iMet-4) -> FM discriminators (Hz). Each type's
// receiver (fsk.cpp, or iMet-4's AFSK modem) finds its own sync and decodes
// with its own FEC / CRC (rs41.cpp, dfm.cpp, ...).
//
// Out: per sonde the latest of everything it reported (Frame): position and
// motion -> a 🗺 map point (kind "balloon"), the weather (temperature,
// humidity, dew point, pressure measured or from the standard atmosphere at
// the GPS altitude, wind = the balloon's horizontal motion) -> the panel's
// facts and table; events: a new sonde, each standard pressure level passed
// on the way up (a sounding: level, height, temperature, humidity, dew
// point, wind), burst, landing.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "sonde.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sonde {
std::unique_ptr<Type> make_rs41(double fs);
std::unique_ptr<Type> make_dfm(double fs);
std::unique_ptr<Type> make_m10(double fs);
std::unique_ptr<Type> make_imet54(double fs);
std::unique_ptr<Type> make_lms6(double fs);
std::unique_ptr<Type> make_mrz(double fs);
std::unique_ptr<Type> make_imet4(double fs);
}

namespace {

using kp::cf;
constexpr double FS = 48000;
constexpr double MAP_TTL_S = 3 * 3600;      // a sonde on the ground keeps sending for hours
const double LEVELS[] = {1000, 925, 850, 700, 500, 400, 300, 250, 200, 150, 100, 70, 50, 30, 20, 10, 7, 5, 3, 2, 1};

std::string fmt(double v, int dec) {
    if (!std::isfinite(v)) return "";
    char b[32];
    snprintf(b, sizeof b, "%.*f", dec, v);
    return b;
}
// decoded text for the panel: control characters out (kp::printable would
// also escape UTF-8 such as the degree sign; kraken_doa checks the UTF-8)
std::string clean(const std::string& s) {
    std::string o;
    for (unsigned char c : s) o += (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    return o;
}
std::string utc_text(double s) {
    if (!std::isfinite(s)) return "";
    const time_t t = static_cast<time_t>(std::floor(s));
    struct tm tm;
    gmtime_r(&t, &tm);
    char b[40];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S UTC", &tm);
    return b;
}
std::string tod_text(double s) {
    if (!std::isfinite(s)) return "";
    const long t = static_cast<long>(std::floor(s)) % 86400;
    char b[24];
    snprintf(b, sizeof b, "%02ld:%02ld:%02ld UTC", t / 3600, t / 60 % 60, t % 60);
    return b;
}

// everything known about one sonde (the last value of each field)
struct Sonde {
    sonde::Frame f;                  // merged
    std::string key;                 // type + serial
    double first_t = 0, last_t = 0, last_out = -1e9;
    long frames = 0;
    double max_alt = NAN;
    bool burst = false, landed = false;
    double vv_avg = NAN;             // smoothed vertical speed
    double stable_since = NAN;       // landed check: when it stopped moving
    int next_level = -1;             // index into LEVELS of the next standard level on the way up
    double last_p = NAN;
    struct Fix { double t, lat, lon, alt; };
    std::deque<Fix> fixes;           // the last ~10 s of fixes (velocity of types without one)
};

class Radiosonde : public kp::Decoder {
public:
    explicit Radiosonde(kp::Host& h) : Decoder(h) {
        lp_n_.set_taps(kp::fir_lowpass(41, 7000, FS));
        lp_w_.set_taps(kp::fir_lowpass(31, 12000, FS));
        types_.push_back(sonde::make_rs41(FS));
        types_.push_back(sonde::make_dfm(FS));
        types_.push_back(sonde::make_m10(FS));
        types_.push_back(sonde::make_imet54(FS));
        types_.push_back(sonde::make_lms6(FS));
        types_.push_back(sonde::make_mrz(FS));
        types_.push_back(sonde::make_imet4(FS));
        for (auto& t : types_) t->out = [this](sonde::Frame& f) { on_frame(f); };
        host.table_columns({"Type", "Serial", "Alt m", "Climb m/s", "Temp °C", "RH %", "hPa", "Wind", "km"});
    }

    void process(const cf* x, size_t n) override {
        nb_.resize(n);
        wb_.resize(n);
        for (size_t i = 0; i < n; i++) {
            nb_[i] = fm_n_.push(lp_n_.push(x[i]));
            wb_[i] = fm_w_.push(lp_w_.push(x[i]));
        }
        const double t0 = host.time();
        bool raw = host.raw_wanted();
        for (auto& t : types_) {
            t->want_raw = raw;
            t->push(nb_.data(), wb_.data(), n, t0);
        }
    }

    void reset() override {
        for (auto& t : types_) t->reset();
        lp_n_.reset(); lp_w_.reset(); fm_n_.reset(); fm_w_.reset();
        sondes_.clear();
    }

private:
    void on_frame(sonde::Frame& f);
    void publish(Sonde& s);
    void events(Sonde& s, const sonde::Frame& f);

    kp::Fir<cf> lp_n_, lp_w_;
    kp::FmDemod fm_n_{static_cast<float>(FS)}, fm_w_{static_cast<float>(FS)};
    std::vector<float> nb_, wb_;
    std::vector<std::unique_ptr<sonde::Type>> types_;
    std::map<std::string, Sonde> sondes_;
    // offline comparison with other decoders: SONDE_DUMP=1 decoder --file ... prints every frame
    const bool dump_ = getenv("SONDE_DUMP") != nullptr;
};

// merge a frame into the sonde's state
template <typename T>
static void take(T& dst, const T& src) {
    if constexpr (std::is_floating_point_v<T>) { if (std::isfinite(src)) dst = src; }
    else if constexpr (std::is_same_v<T, std::string>) { if (!src.empty()) dst = src; }
    else { if (src >= 0) dst = src; }
}

void Radiosonde::on_frame(sonde::Frame& f) {
    if (f.serial.empty()) return;
    f.t = host.time();
    if (dump_) {   // offline comparison (SONDE_DUMP=1): one line per frame
        fprintf(stderr, "FRAME %s %s %ld t=%.2f tod=%.0f utc=%.0f lat=%.5f lon=%.5f alt=%.1f vh=%.2f hdg=%.1f vv=%.2f T=%.1f RH=%.1f P=%.1f batt=%.2f\n",
                f.type.c_str(), f.serial.c_str(), f.frame_no, f.t, f.tod, f.utc, f.lat, f.lon, f.alt, f.vh, f.heading, f.vv, f.temp, f.rh,
                f.pressure, f.batt);
    }
    host.valid();
    if (std::isfinite(f.dc_hz)) host.freq_error(f.dc_hz);
    if (!f.raw.empty() && host.raw_wanted()) host.raw(f.type + " " + f.serial + " " + f.raw);

    const std::string key = f.type + " " + f.serial;
    const bool fresh = !sondes_.count(key);
    Sonde& s = sondes_[key];
    // types without velocity (iMet-54, iMet-4's basic GPS packet): from the
    // fix 3..10 s earlier - over one second GPS jitter would look like speed
    if (std::isfinite(f.lat) && std::isfinite(f.alt)) {
        double ft = std::isfinite(f.utc) ? f.utc : std::isfinite(f.tod) ? f.tod : f.t;
        if (!s.fixes.empty() && ft < s.fixes.back().t - 43000) ft += 86400;   // time of day wrapped
        while (!s.fixes.empty() && (ft - s.fixes.front().t > 10 || ft < s.fixes.front().t)) s.fixes.pop_front();
        // (not from a 3-4 satellite fix: a receiver still converging moves tens of metres)
        if (!std::isfinite(f.vh) && !s.fixes.empty() && ft - s.fixes.front().t >= 3 && (f.sats < 0 || f.sats >= 5)) {
            const Sonde::Fix& p = s.fixes.front();
            const double dt = ft - p.t;
            const double vn = (f.lat - p.lat) * 111320 / dt;
            const double ve = (f.lon - p.lon) * 111320 * std::cos(f.lat * M_PI / 180) / dt;
            sonde::en_to_speed_dir(ve, vn, &f.vh, &f.heading);
            f.vv = (f.alt - p.alt) / dt;
        }
        s.fixes.push_back({ft, f.lat, f.lon, f.alt});
    }
    if (fresh) {
        s.key = key;
        s.first_t = f.t;
        host.event("🎈 New " + (f.subtype.empty() ? f.type : f.subtype) + " " + f.serial, 60);
    }
    s.last_t = f.t;
    s.frames++;
    sonde::Frame& m = s.f;
    take(m.type, f.type); take(m.subtype, f.subtype); take(m.serial, f.serial);
    take(m.frame_no, f.frame_no); take(m.utc, f.utc); take(m.tod, f.tod);
    // position + motion belong together: a frame without a fix keeps the last one
    if (std::isfinite(f.lat) && std::isfinite(f.lon)) {
        m.lat = f.lat; m.lon = f.lon; m.alt = f.alt; m.vh = f.vh; m.heading = f.heading; m.vv = f.vv;
    }
    take(m.sats, f.sats); take(m.temp, f.temp); take(m.rh, f.rh); take(m.pressure, f.pressure);
    take(m.temp_rh, f.temp_rh); take(m.temp_int, f.temp_int); take(m.batt, f.batt);
    take(m.burst_timer, f.burst_timer); take(m.tx_mhz, f.tx_mhz);
    for (auto& kv : f.extra) {
        auto it = std::find_if(m.extra.begin(), m.extra.end(), [&](auto& e) { return e.first == kv.first; });
        if (it != m.extra.end()) it->second = kv.second; else m.extra.push_back(kv);
    }
    // extras the sonde no longer reports (e.g. "Calibration 50/51") go away
    m.extra.erase(std::remove_if(m.extra.begin(), m.extra.end(), [&](auto& e) {
                      return std::none_of(f.extra.begin(), f.extra.end(), [&](auto& x) { return x.first == e.first; }) &&
                             e.first == "Calibration";
                  }), m.extra.end());
    if (std::isfinite(f.alt)) s.max_alt = std::isfinite(s.max_alt) ? std::max(s.max_alt, f.alt) : f.alt;
    if (std::isfinite(f.vv)) s.vv_avg = std::isfinite(s.vv_avg) ? s.vv_avg + 0.2 * (f.vv - s.vv_avg) : f.vv;
    events(s, f);
    if (f.t - s.last_out >= 0.9) publish(s);
}

// standard levels passed on the way up, burst, landing
void Radiosonde::events(Sonde& s, const sonde::Frame& f) {
    const sonde::Frame& m = s.f;
    // pressure: measured, else the standard atmosphere at the GPS altitude
    double p = m.pressure;
    const bool est = !std::isfinite(p);
    if (est && std::isfinite(f.alt)) p = sonde::isa_pressure(f.alt);
    if (std::isfinite(p) && !s.burst) {
        if (s.next_level < 0) {   // first fix: the next level above it
            s.next_level = 0;
            while (s.next_level < static_cast<int>(std::size(LEVELS)) && LEVELS[s.next_level] >= p) s.next_level++;
        }
        while (s.next_level < static_cast<int>(std::size(LEVELS)) && p <= LEVELS[s.next_level] && s.vv_avg > 0.5) {
            std::string e = "📈 " + m.serial + " " + fmt(LEVELS[s.next_level], 0) + " hPa" + (est ? " (est.)" : "") + ": " +
                            fmt(m.alt, 0) + " m";
            if (std::isfinite(m.temp)) e += ", " + fmt(m.temp, 1) + " °C";
            if (std::isfinite(m.rh)) e += ", RH " + fmt(m.rh, 0) + " %, dew point " + fmt(sonde::dew_point(m.temp, m.rh), 1) + " °C";
            if (std::isfinite(m.vh) && std::isfinite(m.heading))
                e += ", wind " + fmt(std::fmod(m.heading + 180, 360), 0) + "° " + fmt(m.vh * 3.6, 0) + " km/h";
            host.event(e, 600);
            s.next_level++;
        }
        s.last_p = p;
    }
    if (!s.burst && std::isfinite(s.max_alt) && std::isfinite(m.alt) && s.max_alt > 3000 && m.alt < s.max_alt - 300 &&
        s.vv_avg < -3) {
        s.burst = true;
        host.event("⚠ " + m.serial + " burst at " + fmt(s.max_alt, 0) + " m - descending", 3600);
    }
    if (s.burst && !s.landed && std::isfinite(s.vv_avg)) {
        if (std::fabs(s.vv_avg) < 1.0) {
            if (!std::isfinite(s.stable_since)) s.stable_since = f.t;
            if (f.t - s.stable_since > 30) {
                s.landed = true;
                host.event("🪂 " + m.serial + " landed at " + fmt(m.lat, 5) + ", " + fmt(m.lon, 5) + " (" + fmt(m.alt, 0) + " m)", 3600);
            }
        } else {
            s.stable_since = NAN;
        }
    }
}

void Radiosonde::publish(Sonde& s) {
    s.last_out = s.f.t;
    const sonde::Frame& m = s.f;
    double p = m.pressure;
    const bool pest = !std::isfinite(p) && std::isfinite(m.alt);
    if (pest) p = sonde::isa_pressure(m.alt);
    const double dew = sonde::dew_point(m.temp, m.rh);
    const double wind_from = std::isfinite(m.heading) ? std::fmod(m.heading + 180, 360) : NAN;
    double dist = NAN, brg = NAN, slat, slon;
    if (std::isfinite(m.lat) && host.station(&slat, &slon)) {
        const double r = M_PI / 180, dla = (m.lat - slat) * r, dlo = (m.lon - slon) * r;
        const double a = std::sin(dla / 2) * std::sin(dla / 2) + std::cos(slat * r) * std::cos(m.lat * r) * std::sin(dlo / 2) * std::sin(dlo / 2);
        dist = 6371 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
        brg = std::fmod(std::atan2(std::sin(dlo) * std::cos(m.lat * r),
                                   std::cos(slat * r) * std::sin(m.lat * r) - std::sin(slat * r) * std::cos(m.lat * r) * std::cos(dlo)) / r + 360, 360);
    }
    const std::string phase = s.landed ? "landed" : s.burst ? "descending (burst at " + fmt(s.max_alt, 0) + " m)"
                              : !std::isfinite(s.vv_avg) ? "" : s.vv_avg > 1 ? "ascending" : s.vv_avg < -1 ? "descending" : "level / on the ground";

    // "key: value" rows, used for the facts and the map popup
    std::vector<std::pair<std::string, std::string>> rows;
    auto add = [&](const std::string& k, const std::string& v) { if (!v.empty()) rows.push_back({k, v}); };
    add("Sonde", (m.subtype.empty() ? m.type : m.subtype) + " " + m.serial);
    add("Frame", m.frame_no >= 0 ? std::to_string(m.frame_no) : "");
    add("Time", std::isfinite(m.utc) ? utc_text(m.utc) : tod_text(m.tod));
    add("Position", std::isfinite(m.lat) ? fmt(m.lat, 5) + ", " + fmt(m.lon, 5) : "");
    add("Altitude", std::isfinite(m.alt) ? fmt(m.alt, 0) + " m" + (std::isfinite(s.max_alt) && s.max_alt > m.alt + 50 ? " (max " + fmt(s.max_alt, 0) + " m)" : "") : "");
    add("Climb", std::isfinite(m.vv) ? fmt(m.vv, 1) + " m/s" : "");
    add("Flight", phase);
    add("Temperature", std::isfinite(m.temp) ? fmt(m.temp, 1) + " °C" : "");
    add("Humidity", std::isfinite(m.rh) ? fmt(m.rh, 1) + " %" : "");
    add("Dew point", std::isfinite(dew) ? fmt(dew, 1) + " °C" : "");
    add("Pressure", std::isfinite(p) ? fmt(p, 1) + " hPa" + (pest ? " (from altitude)" : "") : "");
    add("Wind", std::isfinite(wind_from) && std::isfinite(m.vh) ? "from " + fmt(wind_from, 0) + "° at " + fmt(m.vh * 3.6, 1) + " km/h (" + fmt(m.vh, 1) + " m/s)" : "");
    add("Humidity sensor temp.", std::isfinite(m.temp_rh) ? fmt(m.temp_rh, 1) + " °C" : "");
    add("Internal temp.", std::isfinite(m.temp_int) ? fmt(m.temp_int, 1) + " °C" : "");
    add("Battery", std::isfinite(m.batt) ? fmt(m.batt, 2) + " V" : "");
    add("Satellites", m.sats >= 0 ? std::to_string(m.sats) : "");
    add("Burst countdown", m.burst_timer >= 0 ? std::to_string(m.burst_timer / 3600) + " h " + std::to_string(m.burst_timer / 60 % 60) + " min" : "");
    add("TX frequency", std::isfinite(m.tx_mhz) ? fmt(m.tx_mhz, 3) + " MHz" : "");
    add("From station", std::isfinite(dist) ? fmt(dist, dist < 10 ? 1 : 0) + " km, " + fmt(brg, 0) + "°" : "");
    for (auto& kv : m.extra) add(clean(kv.first), clean(kv.second));

    // facts: this (the latest) sonde; rows another sonde set but this one lacks are cleared
    static const char* ALL[] = {"Sonde", "Frame", "Time", "Position", "Altitude", "Climb", "Flight", "Temperature", "Humidity",
                                "Dew point", "Pressure", "Wind", "Humidity sensor temp.", "Internal temp.", "Battery", "Satellites",
                                "Burst countdown", "TX frequency", "From station"};
    for (const char* k : ALL)
        if (std::none_of(rows.begin(), rows.end(), [&](auto& r) { return r.first == k; })) host.fact(k, "");
    for (auto& r : rows) host.fact(r.first, r.second);
    host.fact("Sondes heard", std::to_string(sondes_.size()));

    host.table_row(s.key, {m.subtype.empty() ? m.type : m.subtype, m.serial, fmt(m.alt, 0), fmt(m.vv, 1), fmt(m.temp, 1), fmt(m.rh, 0),
                           fmt(p, 1), std::isfinite(wind_from) ? fmt(wind_from, 0) + "° " + fmt(m.vh * 3.6, 0) + " km/h" : "",
                           fmt(dist, 1)});

    if (std::isfinite(m.lat) && std::isfinite(m.lon)) {
        kp::MapPoint mp;
        mp.id = s.key;
        mp.lat = m.lat;
        mp.lon = m.lon;
        mp.label = m.serial;
        mp.kind = "balloon";
        mp.heading = std::isfinite(m.heading) ? static_cast<float>(m.heading) : NAN;
        mp.altitude_m = std::isfinite(m.alt) ? static_cast<float>(m.alt) : NAN;
        mp.speed_kmh = std::isfinite(m.vh) ? static_cast<float>(m.vh * 3.6) : NAN;
        for (auto& r : rows) mp.info += r.first + ": " + r.second + "\n";
        mp.ttl_s = MAP_TTL_S;
        host.map_point(mp);
    }
}

}  // namespace

KRAKEN_PLUGIN(Radiosonde, {.id = "radiosonde",
                           .name = "Radiosonde",
                           .description = "Weather balloon sondes (400-406 MHz): Vaisala RS41, Graw DFM, Meteomodem M10 / M20, "
                                          "iMet-4 / iMet-54, LMS6, MRZ - position, altitude, temperature, humidity, pressure, wind",
                           .version = "1.0",
                           .sample_rate = FS,
                           .min_vfo_rate = 24000,
                           .author = "KrakenSDR",
                           .map = true})
