// 121.5 / 243 MHz swept-tone distress beacons: the homing signal of ELTs
// (aircraft), EPIRBs (ships), PLBs (personal) and man-overboard devices, and
// the 121.5 MHz homer of every 406 MHz beacon. From the public texts: ITU-R
// M.690-3 (AM, depth >= 0.85, duty >= 33 %, an upward or downward sweep over
// >= 700 Hz inside 300 - 1600 Hz, 2 - 4 sweeps a second, >= 30 % of the power
// within +-30 Hz of the carrier), ETSI EN 302 152-1 8.2 (121.5 MHz +-50 ppm,
// downward sweep, duty 33 - 55 %, interrupted <= 2 s for the 406 MHz burst,
// an optional Morse letter after it), UK CAA Specification 16 (keyed carrier,
// CW periods), C/S T.001 4.5.3 (homing never at the same time as 406 MHz).
//
// The VFO sits on 121.500 MHz at 32 kHz (picking the decoder tunes it): every
// carrier within +-12.5 kHz. rx.cpp finds each carrier, follows it and tells
// a swept tone from AM voice, a bare carrier and steady tones. A carrier with
// a confirmed sweep is a beacon - on 121.5 / 243 MHz a DISTRESS beacon ("⚠").
//
// ID separation: the signal has no ID. Each beacon is told apart by its
// carrier (and sweep): a kp::Talker packet every 0.5 s - 0.24 s of its
// carrier, freq_hz = the carrier, bw_hz = 100 Hz (the sidebands start 300 Hz
// out) - so every beacon gets its own bearing, also beacons on the air at the
// same time a few kHz apart.
//
// The decoder reports nothing to anyone: a heard distress beacon must be
// reported to the rescue coordination centre by the user (README).

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "rx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

using b121::FS;

constexpr double KEEP_DEFAULT_S = 24 * 3600;
constexpr double ALERT_REPEAT_S = 300;     // a distress beacon's ⚠ event again after this long
constexpr double CONFIRM_S = 5;            // sweep heard this long before the ⚠ (a self-test is shorter)
constexpr double ACTIVE_S = 3;             // a beacon with a sweep within this long is on the air
constexpr double LOST_S = 30;
constexpr double SHORT_GONE_S = 15;        // a short one is reported after this long without it
constexpr double CHAN_BW_HZ = 100;         // the carrier's channel for the DoA
constexpr double MHZ_121 = 121.5e6, MHZ_243 = 243.0e6;
// a beacon this close to 121.5 / 243 MHz is a distress beacon: +-50 ppm is the specification, but old
// beacons drift further - the whole VFO around 121.5 (+-12.5 kHz) counts; a training beacon is further away
constexpr double TOL_121 = 15000, TOL_243 = 25000;
// measurable at all: depth / duty only from a strong carrier with no other one near it
constexpr double DEPTH_CN0 = 60, DUTY_CN0 = 48, NEIGHBOUR_MIN_HZ = 5000;

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}

std::string utc(time_t t) {
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char b[32];
    strftime(b, sizeof b, "%H:%M:%S UTC", &tmv);
    return b;
}

std::string dur(double s) {
    if (s < 60) return fmt("%.0f s", s);
    if (s < 3600) return fmt("%.0f min ", std::floor(s / 60)) + fmt("%.0f s", std::fmod(s, 60));
    return fmt("%.0f h ", std::floor(s / 3600)) + fmt("%.0f min", std::fmod(s / 60, 60));
}

const char* dir_arrow(int d) {
    return d == b121::DIR_DOWN ? "↓" : d == b121::DIR_UP ? "↑" : d == b121::DIR_UPDOWN ? "↕" : "?";
}
const char* dir_word(int d) {
    return d == b121::DIR_DOWN ? "downward" : d == b121::DIR_UP ? "upward" : d == b121::DIR_UPDOWN ? "up and down" : "?";
}

struct Beacon {
    std::string id;
    int chan = -1;                 // the receiver channel following it (-1 = none now)
    double off = 0;                // carrier, Hz from the VFO's 0 Hz
    double first_t = 0, last_t = -1e9, heard_s = 0;
    double on_first = NAN, on_last = NAN;   // its carrier's first / last moment heard
    time_t first_wall = 0, last_wall = 0;
    b121::Sweep sw;
    double depth = NAN, duty = NAN, cn0 = NAN, cn0_max = NAN;
    int packets = 0;
    std::deque<std::pair<double, double>> pauses;   // (host time, length)
    std::deque<std::pair<double, double>> cw;
    std::map<std::string, int> morse;
    std::string morse_code, morse_key;
    double morse_unit = 0;
    bool announced = false, short_done = false, lost = false, homer_said = false, keyed_said = false, cw_said = false;
    std::map<std::string, bool> morse_said;
    double last_alert = -1e9, last_raw = -1e9, table_t = -1e9;
    std::string table_status;
    std::deque<std::pair<double, float>> strength;
};

class Beacon121 : public kp::Decoder {
public:
    explicit Beacon121(kp::Host& h) : Decoder(h) {
        host.table_columns(COLS);
        host.table_keep(keep_s_);
    }

    void reset() override {
        rx_.reset();
        for (auto& [k, b] : beacons_) host.table_remove(k);
        beacons_.clear();
        others_.clear();
        chan_beacon_.clear();
        chan_seen_.clear();
        host.table_columns(COLS);
        reports_ = 0;
        house_t_ = plot_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "doa_avg") avg_s_ = value.empty() ? 2.0 : std::clamp(atof(value.c_str()), 0.5, 7200.0);
        else if (key == "keep_h") {
            keep_s_ = value.empty() ? KEEP_DEFAULT_S : std::clamp(atof(value.c_str()) * 3600, 600.0, 7 * 86400.0);
            host.table_keep(keep_s_);
        }
    }

    void process(const kp::cf* x, size_t n) override {
        now_ = host.time();
        blk_abs_ = rx_.abs();
        rx_.verbose = debug_;
        std::vector<b121::Report> reps;
        rx_.process(x, n, &reps);
        for (auto& l : rx_.log) host.log(l);
        rx_.log.clear();
        for (auto& r : reps) on_report(r);
        housekeeping();
    }

private:
    const std::vector<std::string> COLS = {"Status", "Beacon", "Carrier MHz", "Offset", "Sweep", "Sweeps/s",
                                           "Depth", "Duty", "Pauses", "C/N0 dBHz", "Heard"};
    b121::Receiver rx_;
    std::map<std::string, Beacon> beacons_;
    std::map<int, std::string> chan_beacon_;   // channel serial -> beacon id
    std::map<int, double> chan_seen_;          // channel serial -> host time of its last report
    struct Other { double off, cn0, t; std::string why; };
    std::map<int, Other> others_;              // carriers without a sweep
    double now_ = 0;
    uint64_t blk_abs_ = 0;
    // the channel / evaluation debug lines (stderr): an environment variable, not host.verbose() - that
    // is "Show all messages" / Logging "Log All" now, and these are diagnostics every 0.5 s per carrier
    const bool debug_ = getenv("B121_DEBUG") != nullptr;
    double avg_s_ = 2.0, keep_s_ = KEEP_DEFAULT_S;
    int reports_ = 0;
    double house_t_ = -1e9, plot_t_ = -1e9;

    double to_time(double abs_idx) const { return now_ + (abs_idx - static_cast<double>(blk_abs_)) / FS; }
    double rf_of(double off) const {
        const double rf = host.rf_hz();
        if (!std::isfinite(rf)) return NAN;
        return rf + off;
    }
    // 1 = 121.5 MHz, 2 = 243 MHz, 3 = another frequency, 0 = unknown (no RF)
    int freq_class(double off) const {
        const double rf = rf_of(off);
        if (!std::isfinite(rf)) return 0;
        if (std::fabs(rf - MHZ_121) <= TOL_121) return 1;
        if (std::fabs(rf - MHZ_243) <= TOL_243) return 2;
        return 3;
    }
    std::string freq_text(double off) const {
        const double rf = rf_of(off);
        if (std::isfinite(rf)) return fmt("%.5f MHz", rf / 1e6);
        return fmt("%+.2f kHz", off / 1e3);
    }
    std::string offset_text(double off) const {
        const double rf = rf_of(off);
        const int c = freq_class(off);
        if (c == 1 || c == 2) {
            const double nom = c == 1 ? MHZ_121 : MHZ_243;
            return fmt("%+.2f kHz", (rf - nom) / 1e3) + fmt(" (%+.0f ppm)", (rf - nom) / nom * 1e6);
        }
        return fmt("%+.2f kHz in the VFO", off / 1e3);
    }
    std::string new_id(double off) const {
        const double rf = rf_of(off);
        std::string base = std::isfinite(rf) ? fmt("%.4f", rf / 1e6) : fmt("%+.2fk", off / 1e3);
        std::string id = base;
        for (char s = 'b'; beacons_.count(id) && s <= 'z'; s++) id = base + s;
        return id;
    }
    bool active(const Beacon& b) const { return now_ - b.last_t <= ACTIVE_S; }
    double on_air(const Beacon& b) const {
        return std::isfinite(b.on_first) && std::isfinite(b.on_last) ? b.on_last - b.on_first : 0;
    }
    // a beacon, not a test: its sweep heard >= 5 s - or, a weak one heard now and then, >= 2 s of
    // sweep over >= 15 s on the air
    bool confirmed(const Beacon& b) const { return b.heard_s >= CONFIRM_S || (b.heard_s >= 2 && on_air(b) >= 15); }
    bool distress(const Beacon& b) const {
        const int c = freq_class(b.off);
        return c != 3 && confirmed(b);
    }
    bool homer406(const Beacon& b) const {
        // pauses of <= 2.5 s some tens of seconds apart: the homer stops for its 406 MHz burst
        if (keyed(b) || pause_starts(b).size() < 2) return false;
        for (const auto& p : b.pauses)
            if (p.second > 2.5) return false;
        const double iv = pause_interval(b);
        // (first generation every 50 s; second-generation beacons burst more often at first)
        return iv >= 8 && iv <= 150;
    }
    // the pauses' starts, a pause within 4 s after another one (a Morse letter's last gap) left out
    std::vector<double> pause_starts(const Beacon& b) const {
        std::vector<double> s;
        double prev = -1e9;
        for (const auto& p : b.pauses) {
            if (p.first - prev > 4 || keyed_raw(b)) s.push_back(p.first);
            prev = p.first + p.second;
        }
        return s;
    }
    double pause_interval(const Beacon& b) const {
        const std::vector<double> s = pause_starts(b);
        std::vector<double> d;
        for (size_t i = 1; i < s.size(); i++) d.push_back(s[i] - s[i - 1]);
        if (d.empty()) return NAN;
        std::sort(d.begin(), d.end());
        return d[d.size() / 2];
    }
    // a keyed carrier: >= 4 intervals under 6 s, all within 20 % of their median
    bool keyed_raw(const Beacon& b) const {
        if (b.pauses.size() < 5) return false;
        std::vector<double> d;
        for (size_t i = 1; i < b.pauses.size(); i++) d.push_back(b.pauses[i].first - b.pauses[i - 1].first);
        std::vector<double> s = d;
        std::sort(s.begin(), s.end());
        const double med = s[s.size() / 2];
        if (!(med < 6)) return false;
        for (double v : d)
            if (std::fabs(v - med) > 0.2 * med) return false;
        return true;
    }
    bool keyed(const Beacon& b) const { return keyed_raw(b); }
    std::string sweep_text(const b121::Sweep& s) const {
        if (s.rate_hz <= 0) return "";
        if (s.dir == b121::DIR_UP) return fmt("↑ %.0f", s.lo_hz) + fmt("-%.0f Hz", s.hi_hz);
        if (s.dir == b121::DIR_UPDOWN) return fmt("↕ %.0f", s.lo_hz) + fmt("-%.0f Hz", s.hi_hz);
        return fmt("↓ %.0f", s.hi_hz) + fmt("-%.0f Hz", s.lo_hz);
    }
    std::string kind_text(const Beacon& b) const {
        const int c = freq_class(b.off);
        std::string k = homer406(b) ? "406 MHz beacon's homer" : "ELT / EPIRB / PLB";
        if (c == 2) k += " (243 MHz)";
        if (c == 3) k = "swept-tone beacon (not on 121.5 / 243 MHz)";
        return k;
    }
    std::string status(const Beacon& b) const {
        const int c = freq_class(b.off);
        if (!confirmed(b)) return active(b) ? "confirming" : on_air(b) < CONFIRM_S + 1 ? "short (test?)" : "weak, not confirmed";
        if (c == 3) return "training / test frequency?";
        return c == 2 ? "⚠ DISTRESS 243" : "⚠ DISTRESS";
    }
    std::string describe(const Beacon& b) const {
        std::string s = kind_text(b) + " " + b.id + ": carrier " + freq_text(b.off);
        const int c = freq_class(b.off);
        if (c == 1 || c == 2) s += " (" + offset_text(b.off) + ")";
        s += ", swept tone " + sweep_text(b.sw) + fmt(", %.1f sweeps/s", b.sw.rate_hz);
        if (std::isfinite(b.cn0)) s += fmt(", C/N0 %.0f dBHz", b.cn0);
        return s;
    }
    std::string label(const Beacon& b) const {
        const int c = freq_class(b.off);
        std::string s = distress(b) ? "⚠ " : c == 3 ? "training? " : status(b) + " · ";
        s += homer406(b) ? "406 homer" : c == 2 ? "243 beacon" : c == 3 ? "beacon" : "121.5 beacon";
        s += std::string(" · ") + dir_arrow(b.sw.dir) + fmt(" %.1f/s", b.sw.rate_hz);
        if (s.size() > 60) s.resize(60);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
        return s;
    }

    void on_report(const b121::Report& r) {
        reports_++;
        chan_seen_[r.chan] = now_;
        const double t = to_time(r.eval_abs);
        if (debug_ && (r.sweep.ok || r.carrier_share > 0.5))
            host.log(fmt("ch %.0f ", r.chan) + fmt("%+.1f Hz ", r.freq_hz) + fmt("cn0 %.1f ", r.cn0) +
                     (r.sweep.ok ? "SWEEP " + sweep_text(r.sweep) + fmt(" %.2f/s", r.sweep.rate_hz) + fmt(" rho %.3f", r.sweep.rho) +
                                       fmt(" mono %.2f", r.sweep.mono) + fmt(" lit %.2f", r.sweep.lit) + fmt(" peak %.1f dB", r.sweep.peak_db) +
                                       fmt(" depth %.3f", r.depth) + fmt(" duty %.3f", r.duty)
                                 : "no sweep: " + r.sweep.why) +
                     fmt(" passes %.0f", r.passes));
        if (debug_ && r.sweep.ok && getenv("B121_TRACE")) {
            std::string tr = "trace";
            for (float v : r.sweep.trace) tr += std::isfinite(v) ? fmt(" %.0f", v) : " -";
            host.log(tr);
        }
        auto it = chan_beacon_.find(r.chan);
        if (it == chan_beacon_.end()) {
            if (r.passes < 2) {
                if (r.carrier_share > 0.2) others_[r.chan] = {r.freq_hz, r.cn0, now_, r.sweep.why};
                return;
            }
            others_.erase(r.chan);
            attach(r, t);
            it = chan_beacon_.find(r.chan);
            if (it == chan_beacon_.end()) return;
        }
        Beacon& b = beacons_[it->second];
        b.off = r.freq_hz;
        if (std::isfinite(r.first_on_abs) && !(b.on_first <= to_time(r.first_on_abs))) {
            b.on_first = to_time(r.first_on_abs);
            b.first_wall = std::time(nullptr) - static_cast<time_t>(std::max(0.0, t - b.on_first));
        }
        if (r.carrier_share > 0) b.last_wall = std::time(nullptr);
        if (std::isfinite(r.last_on_abs)) b.on_last = std::max(std::isfinite(b.on_last) ? b.on_last : -1e300, to_time(r.last_on_abs));
        if (r.sweep.ok && (b.sw.rate_hz <= 0 || r.carrier_share >= 0.2)) b.sw = r.sweep;   // (a short one: its last sweep seen)
        if (std::isfinite(r.cn0) && r.carrier_share > 0.3) {
            b.cn0 = r.cn0;
            if (!(b.cn0_max >= r.cn0)) b.cn0_max = r.cn0;
            b.strength.push_back({t, static_cast<float>(r.cn0)});
            while (!b.strength.empty() && t - b.strength.front().first > 120) b.strength.pop_front();
        }
        // a sweep in the window AND the carrier in the last 0.5 s (the 6 s window still holds a sweep
        // for seconds after a short transmission ended)
        if (r.sweep.ok && r.carrier_share >= 0.2) {
            const double gap = t - b.last_t;
            b.heard_s += std::clamp(gap, 0.0, 0.5);
            b.last_t = t;
            if (b.lost) {
                b.lost = false;
                host.event("Beacon " + b.id + " heard again: " + describe(b), 0.0);
            }
            const bool clear = !(r.neighbour_hz < NEIGHBOUR_MIN_HZ);
            if (std::isfinite(r.depth) && clear && b.cn0 >= DEPTH_CN0) b.depth = r.depth;
            if (std::isfinite(r.duty) && clear && b.cn0 >= DUTY_CN0) b.duty = r.duty;
        }
        // pauses, keying, CW periods, Morse
        for (const auto& p : r.pauses) {
            b.pauses.push_back({to_time(p.start_abs), p.len_s});
            while (b.pauses.size() > 12) b.pauses.pop_front();
        }
        for (const auto& c : r.cw) {
            b.cw.push_back({to_time(c.start_abs), c.len_s});
            while (b.cw.size() > 12) b.cw.pop_front();
        }
        if (!r.morse.empty()) {
            const int n = ++b.morse[r.morse];
            b.morse_code = r.morse_code;
            b.morse_key = r.morse_key;
            b.morse_unit = r.morse_unit_s;
            if (n == 2 && !b.morse_said[r.morse])
                host.event("Beacon " + b.id + ": Morse '" + r.morse + "' (" + r.morse_code + ", keyed " + r.morse_key +
                               fmt(", %.0f ms dot)", r.morse_unit_s * 1e3) +
                               (r.morse == "P" ? " - the identifier 406 MHz PLBs send on their homer" : ""),
                           0.0);
            if (n >= 2) b.morse_said[r.morse] = true;
        }
        const bool on_air = active(b);
        if (on_air) {
            // valid spans (the Digital squelch) and a DoA packet on the carrier's channel
            double best_a = 0, best_e = 0;
            for (const auto& run : r.on_runs) {
                const double a = to_time(run.first), e = to_time(run.second);
                if (e - a < 0.05) continue;
                host.valid(a, e);
                if (e - a > best_e - best_a || e > best_e) best_a = a, best_e = e;
            }
            if (best_e - best_a >= 0.1) talker_packet(b, std::max(best_a + 0.01, best_e - 0.25), best_e - 0.01);
        }
        events(b);
        if (host.raw_wanted() && r.sweep.ok && now_ - b.last_raw >= 10) {
            b.last_raw = now_;
            char s[200];
            snprintf(s, sizeof s, "B121 %s %s %.2f/s %.0f-%.0f", b.id.c_str(), dir_word(b.sw.dir), b.sw.rate_hz, b.sw.lo_hz,
                     b.sw.hi_hz);
            std::string line = s;
            // depth / duty only where measured, C/N0 where known (no "nan" in the log)
            if (std::isfinite(b.depth)) line += fmt(" m%.2f", b.depth);
            if (std::isfinite(b.duty)) line += fmt(" d%.2f", b.duty);
            if (std::isfinite(b.cn0)) line += fmt(" cn0 %.1f", b.cn0);
            host.raw(line);
        }
        // the table row about once a second (sooner when its status changed)
        const std::string st = status(b);
        if (now_ - b.table_t >= 1 || st != b.table_status) outputs(b);
    }

    void attach(const b121::Report& r, double t) {
        // a beacon heard before on (about) this carrier and not followed now: the same one
        Beacon* found = nullptr;
        for (auto& [k, b] : beacons_) {
            if (b.chan >= 0) continue;
            if (std::fabs(b.off - r.freq_hz) < 300 && (!found || std::fabs(b.off - r.freq_hz) < std::fabs(found->off - r.freq_hz)))
                found = &b;
        }
        if (!found) {
            const std::string id = new_id(r.freq_hz);
            Beacon& b = beacons_[id];
            b.id = id;
            b.first_t = t - r.sweep.period_s * 4;
            b.first_wall = std::time(nullptr);
            b.last_t = t;
            found = &b;
        }
        found->chan = r.chan;
        found->off = r.freq_hz;
        chan_beacon_[r.chan] = found->id;
    }

    void events(Beacon& b) {
        if (!active(b)) return;
        const int c = freq_class(b.off);
        if (confirmed(b)) {
            if (c == 3) {
                if (now_ - b.last_alert > 1800) {
                    host.event("Swept-tone beacon " + b.id + " on " + freq_text(b.off) +
                                   " - off the 121.5 / 243 MHz distress frequencies: a training / exercise beacon? Swept tone " +
                                   sweep_text(b.sw) + fmt(", %.1f sweeps/s", b.sw.rate_hz) +
                                   (std::isfinite(b.cn0) ? fmt(", C/N0 %.0f dBHz", b.cn0) : ""),
                               0.0);
                    b.last_alert = now_;
                }
            } else if (now_ - b.last_alert > ALERT_REPEAT_S) {
                host.event(std::string("⚠ DISTRESS BEACON") + (c == 2 ? " on 243 MHz" : c == 1 ? " on 121.5 MHz" : "") + ": " +
                               describe(b) + " - report it to the rescue coordination centre (time, frequency, bearing)",
                           0.0);
                b.last_alert = now_;
                b.announced = true;
            }
        }
        if (!b.homer_said && confirmed(b) && homer406(b)) {
            b.homer_said = true;
            const auto& p = b.pauses.back();
            host.event("Beacon " + b.id + fmt(": pauses every %.1f s", pause_interval(b)) + fmt(" for %.2f s", p.second) +
                           " - the 121.5 MHz homer of a 406 MHz beacon; its 406 MHz bursts carry the beacon's ID "
                           "(406 MHz decoder on 406.0-406.1 MHz)",
                       0.0);
        }
        if (!b.keyed_said && keyed(b)) {
            b.keyed_said = true;
            double on = pause_interval(b), off = 0;
            for (const auto& p : b.pauses) off += p.second;
            off /= static_cast<double>(b.pauses.size());
            host.event("Beacon " + b.id + fmt(": keyed carrier, %.2f s on", on - off) + fmt(" / %.2f s off", off), 0.0);
        }
        if (!b.cw_said && b.cw.size() >= 2) {
            b.cw_said = true;
            host.event("Beacon " + b.id + fmt(": unmodulated carrier for %.1f s", b.cw.back().second) +
                           fmt(" every %.1f s", b.cw.back().first - b.cw[b.cw.size() - 2].first),
                       0.0);
        }
    }

    void talker_packet(Beacon& b, double a, double e) {
        if (e - a < 0.05) return;
        kp::Talker t;
        t.id = b.id;
        t.label = label(b);
        t.start_s = a;
        t.end_s = std::min(e, a + 0.24);
        t.packet = true;
        t.freq_hz = b.off;
        t.bw_hz = CHAN_BW_HZ;
        t.avg_s = avg_s_;
        t.keep_s = keep_s_;
        host.talker(t);
        b.packets++;
    }

    std::string spec_text(const Beacon& b) const {
        std::vector<std::string> bad;
        const int c = freq_class(b.off);
        if (c == 1 || c == 2) {
            const double nom = c == 1 ? MHZ_121 : MHZ_243;
            const double ppm = (rf_of(b.off) - nom) / nom * 1e6;
            if (std::fabs(ppm) > 50) bad.push_back(fmt("carrier %+.0f ppm (> 50)", ppm));
        }
        if (b.sw.rate_hz < 1.9 || b.sw.rate_hz > 4.1) bad.push_back(fmt("%.2f sweeps/s (2 - 4)", b.sw.rate_hz));
        if (b.sw.hi_hz - b.sw.lo_hz < 660) bad.push_back(fmt("sweep %.0f Hz wide (>= 700)", b.sw.hi_hz - b.sw.lo_hz));
        if (b.sw.lo_hz < 260 || b.sw.hi_hz > 1650) bad.push_back(fmt("sweep %.0f", b.sw.lo_hz) + fmt("-%.0f Hz (inside 300 - 1600)", b.sw.hi_hz));
        if (b.depth < 0.83) bad.push_back(fmt("modulation factor %.2f (>= 0.85)", b.depth));
        if (b.duty < 0.31 || b.duty > 0.57) bad.push_back(fmt("duty cycle %.0f %% (33 - 55)", b.duty * 100));
        if (bad.empty()) return "as specified (the parts measured)";
        std::string s;
        for (const auto& x : bad) s += (s.empty() ? "" : "; ") + x;
        return s;
    }

    void outputs(Beacon& b) {
        b.table_t = now_;
        b.table_status = status(b);
        std::string pauses;
        if (keyed(b)) pauses = "keyed";
        else if (!b.pauses.empty()) {
            const double iv = pause_interval(b);
            pauses = fmt("%.0f", static_cast<double>(b.pauses.size())) + (std::isfinite(iv) ? fmt(" / %.0f s", iv) : "");
        }
        host.table_row(b.id, {status(b), b.id, std::isfinite(rf_of(b.off)) ? fmt("%.5f", rf_of(b.off) / 1e6) : "",
                              offset_text(b.off), sweep_text(b.sw), fmt("%.2f", b.sw.rate_hz),
                              std::isfinite(b.depth) ? fmt("%.2f", b.depth) : "",
                              std::isfinite(b.duty) ? fmt("%.0f %%", b.duty * 100) : "", pauses,
                              std::isfinite(b.cn0) ? fmt("%.1f", b.cn0) : "",
                              dur(std::isfinite(b.on_last) && std::isfinite(b.on_first) ? b.on_last - b.on_first : b.heard_s)});
        std::vector<std::pair<std::string, std::string>> det;
        det.push_back({"Status", status(b) + (distress(b) ? " - report it to the rescue coordination centre" : "")});
        det.push_back({"What", kind_text(b)});
        det.push_back({"Carrier", freq_text(b.off) + " · " + offset_text(b.off)});
        det.push_back({"Sweep", std::string(dir_word(b.sw.dir)) + fmt(", %.0f", b.sw.lo_hz) + fmt(" - %.0f Hz", b.sw.hi_hz) +
                                    fmt(" (%.0f Hz wide)", b.sw.hi_hz - b.sw.lo_hz)});
        det.push_back({"Repetition", fmt("%.2f sweeps/s", b.sw.rate_hz) + fmt(" (period %.0f ms)", b.sw.period_s * 1e3)});
        det.push_back({"Modulation factor", std::isfinite(b.depth) ? fmt("%.2f", b.depth)
                                                                   : fmt("not measured (needs C/N0 >= %.0f dBHz, no carrier within 5 kHz)", DEPTH_CN0)});
        det.push_back({"Duty cycle", std::isfinite(b.duty) ? fmt("%.0f %%", b.duty * 100)
                                                          : fmt("not measured (needs C/N0 >= %.0f dBHz, tones under 700 Hz)", DUTY_CN0)});
        det.push_back({"Against the standards", spec_text(b)});
        if (!b.pauses.empty()) {
            const auto& p = b.pauses.back();
            std::string s = fmt("%.0f", static_cast<double>(b.pauses.size())) + fmt(" heard, last %.2f s long", p.second);
            const double iv = pause_interval(b);
            if (std::isfinite(iv)) s += fmt(", every %.1f s", iv);
            if (keyed(b)) s += " - a keyed carrier";
            else if (homer406(b)) s += " - the homer stops for its 406 MHz burst";
            det.push_back({"Carrier pauses", s});
        }
        if (!b.cw.empty())
            det.push_back({"Unmodulated periods", fmt("%.0f", static_cast<double>(b.cw.size())) + fmt(", last %.1f s", b.cw.back().second)});
        for (const auto& [l, n] : b.morse)
            if (n >= 2) det.push_back({"Morse", "'" + l + "' (" + b.morse_code + ") heard " + fmt("%.0f x", n) + ", keyed " + b.morse_key});
        det.push_back({"C/N0", fmt("%.1f dBHz", b.cn0) + fmt(" (strongest %.1f)", b.cn0_max)});
        det.push_back({"First heard", utc(b.first_wall)});
        det.push_back({"Last heard", utc(b.last_wall)});
        if (std::isfinite(b.on_last) && std::isfinite(b.on_first)) det.push_back({"On the air", dur(b.on_last - b.on_first) + " (first to last heard)"});
        det.push_back({"Sweep heard for", dur(b.heard_s)});
        det.push_back({"DoA packets", fmt("%.0f", b.packets) + " (its carrier, 100 Hz)"});
        host.table_detail(b.id, det);
    }

    void plots() {
        if (now_ - plot_t_ < 1) return;
        // the strongest beacon on the air
        const Beacon* f = nullptr;
        for (const auto& [k, b] : beacons_)
            if (active(b) && (!f || b.cn0 > f->cn0)) f = &b;
        if (!f) return;
        plot_t_ = now_;
        // the page shows one plot per VFO in the decoder tab - the plugin's unnamed one (named plots only
        // beside a video display): the strength over time, the walking-DF aid (as wildlife's)
        if (f->strength.size() >= 2) {
            kp::Plot p;
            p.title = "Strength of " + f->id + " (C/N0, last 2 min)";
            p.x_unit = "s";
            p.y_unit = "dBHz";
            const double t1 = f->strength.back().first;
            p.x0 = -120;
            p.dx = 0.5;
            p.y.assign(241, NAN);
            for (const auto& [t, v] : f->strength) {
                const int i = static_cast<int>(std::lround((t - t1 + 120) / 0.5));
                if (i >= 0 && i < 241) p.y[static_cast<size_t>(i)] = v;
            }
            float last = NAN;
            for (auto& v : p.y) {
                if (std::isfinite(v)) last = v;
                else v = last;
            }
            float first = NAN;
            for (float v : p.y)
                if (std::isfinite(v)) { first = v; break; }
            for (auto& v : p.y)
                if (!std::isfinite(v)) v = first;
            host.plot(p);
        }
    }

    void update_facts() {
        int n_d = 0, n_on = 0;
        const Beacon* last_d = nullptr;
        const Beacon* strongest = nullptr;
        for (const auto& [k, b] : beacons_) {
            if (!confirmed(b)) continue;
            if (distress(b)) {
                n_d++;
                if (!last_d || b.last_t > last_d->last_t) last_d = &b;
            }
            if (active(b)) {
                n_on++;
                if (!strongest || b.cn0 > strongest->cn0) strongest = &b;
            }
        }
        int n_conf = 0;
        for (const auto& [k, b] : beacons_) n_conf += confirmed(b);
        host.fact("Beacons heard", fmt("%.0f", n_conf) + (n_d ? fmt(" (%.0f ⚠ distress)", n_d) : "") + fmt(", %.0f on the air", n_on));
        host.fact("⚠ Distress", last_d ? describe(*last_d) + " - report it to the rescue coordination centre" : "");
        host.fact("Strongest", strongest ? strongest->id + fmt(" · C/N0 %.1f dBHz", strongest->cn0) : "");
        std::string oth;
        int n_o = 0;
        for (const auto& [c, o] : others_) {
            if (now_ - o.t > 10) continue;
            n_o++;
            if (n_o <= 4) oth += (oth.empty() ? "" : ", ") + freq_text(o.off) + (std::isfinite(o.cn0) ? fmt(" (%.0f dBHz)", o.cn0) : "");
        }
        host.fact("Carriers without a sweep", n_o ? fmt("%.0f: ", n_o) + oth + " - voice, carriers, tones: not beacons" : "");
        host.fact("Receiver", fmt("%.0f channels followed", rx_.channels()) + (std::isfinite(rx_.noise_var()) ? "" : " · no noise floor yet"));
    }

    void housekeeping() {
        if (now_ - house_t_ < 1) return;
        house_t_ = now_;
        // channels that stopped reporting: their beacon is free again
        for (auto it = chan_seen_.begin(); it != chan_seen_.end();) {
            if (now_ - it->second > 2) {
                auto cb = chan_beacon_.find(it->first);
                if (cb != chan_beacon_.end()) {
                    auto b = beacons_.find(cb->second);
                    if (b != beacons_.end() && b->second.chan == it->first) b->second.chan = -1;
                    chan_beacon_.erase(cb);
                }
                others_.erase(it->first);
                it = chan_seen_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = beacons_.begin(); it != beacons_.end();) {
            Beacon& b = it->second;
            const double gone = now_ - b.last_t;
            if (!b.short_done && !confirmed(b) && gone > SHORT_GONE_S && on_air(b) < CONFIRM_S + 1) {
                b.short_done = true;
                host.event(fmt("Short swept-tone transmission (%.1f s", b.on_last - b.on_first) + ") on " + freq_text(b.off) +
                               ": " + sweep_text(b.sw) + fmt(", %.1f sweeps/s", b.sw.rate_hz) +
                               " - a beacon test? (beacon self-tests send <= 3 sweeps / 1 s of homing)",
                           0.0);
                outputs(b);
            }
            if (confirmed(b) && !b.lost && gone > LOST_S) {
                b.lost = true;
                host.event("Beacon " + b.id + fmt(" not heard for %.0f s", gone) + fmt(" (last C/N0 %.0f dBHz)", b.cn0), 0.0);
                outputs(b);
            }
            if (gone > keep_s_) {
                host.table_remove(it->first);
                it = beacons_.erase(it);
            } else {
                ++it;
            }
        }
        update_facts();
        plots();
    }
};

}  // namespace

KRAKEN_PLUGIN(Beacon121, {.id = "beacon121",
                          .name = "121.5 / 243 MHz distress beacons (swept tone)",
                          .category = "Marine & distress",
                          .description = "The swept-tone AM homing signal of ELTs, EPIRBs, PLBs and man-overboard devices "
                                         "on 121.5 MHz (243 MHz military) - also the homer of every 406 MHz beacon: each "
                                         "beacon by its carrier, sweep direction / range / rate, modulation factor and duty "
                                         "cycle, pauses (406 MHz bursts), keying, Morse letter; a DoA per beacon. AM voice, "
                                         "bare carriers and steady tones are not beacons. Picking it tunes the VFO to 121.500 "
                                         "MHz at 32 kHz (243 MHz: tune the receiver there). A distress beacon must be reported "
                                         "to the rescue coordination centre - this decoder reports nothing itself",
                          .version = "1.0",
                          .sample_rate = FS,
                          .min_vfo_rate = FS,
                          .author = "KrakenSDR",
                          .options = {{"doa_avg", "DoA per beacon", "2", "1=Each packet (0.5 s)|2=2 seconds|10=10 seconds|60=1 minute",
                                       "How long each beacon's carrier packets (one every 0.5 s) are averaged for its bearing "
                                       "(short follows a moving search; long steadies a weak one)"},
                                      {"keep_h", "Keep beacons", "24", "1=1 hour|6=6 hours|24=24 hours|168=7 days",
                                       "How long a beacon stays listed (table, 📡 DF list) after it was last heard"}},
                          .manual_only = true,
                          .fixed_freq_hz = 121.5e6,
                          .talkers = true,
                          .squelch_hold_s = 1.0})
