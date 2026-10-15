// Wildlife tracking transmitters (VHF collars / tags): pulsed CW anywhere in
// the VFO, at the VFO's own rate (Info::sample_rate 0) - a 2.4 MHz VFO for a
// band survey, or a narrow one on a single collar.
//
// - pulse.cpp: ~250 Hz FFT bins, floor per bin, a short and a ~14 ms
//   detector, each pulse measured on the raw samples (edges, carrier to a
//   few Hz, power, C/N0)
// - per frequency (+-700 Hz) a CHANNEL; on it a pulse TRAIN (a collar: its
//   period from the pulses with misses / extras, ppm, mode) and / or CODED
//   tags (Lotek / Motus style: bursts of 4 pulses, the 3 gaps + the burst
//   interval = the tag)
// - modes: normal / incubating / MORTALITY by the rate (NZ DOC convention:
//   30 / 48 / 60 (80 kakapo) ppm) or a rate change (x2 = mortality); the
//   NZ data streams (time since death, Egg Timer / Chick Timer) decoded
//
// ID separation: each collar / tag is a transmitter - every pulse a
// kp::Talker packet with its exact span and its own 1 kHz channel
// (freq_hz / bw_hz), so collars in one VFO get their own bearings; averaged
// over doa_avg, kept for keep_h hours.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "pulse.hpp"
#include "train.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr double FREQ_TOL_HZ = 700;       // one channel
constexpr double GROUP_GAP_S = 0.16;      // pulses this close may be a coded burst (Lotek: 0.1-0.3 s for 4)
constexpr double QUIET_S = 0.3;           // a coded burst stands alone
constexpr double TRAIN_WIN_S = 30;        // pulses used to find a train
constexpr double PMIN = 0.4, PMAX = 8.0;  // train periods (150 .. 7.5 ppm)
constexpr int CONFIRM_PULSES = 4;
constexpr double GAP_TOL_MS = 2.0;        // a coded tag's gaps
constexpr double BI_MIN = 1.5, BI_MAX = 120;
constexpr double STREAM_END_S = 40;

struct PRec {
    double t0 = 0, t1 = 0;   // host time (s)
    double freq = 0;         // Hz
    float pdb = 0, cn0 = 0;
    bool grid = false;
    int gsize = 1;           // pulses in its group (< GROUP_GAP_S apart)
};

struct Tx {                  // one transmitter (collar / tag)
    std::string id, name;
    bool coded = false;
    double freq = 0;         // tracked carrier (Hz from the input's 0 Hz)
    double first_t = 0, last_t = 0;
    uint64_t pulses = 0;
    float cn0 = 0, pdb = 0;
    double width_ms = 0;
    std::deque<PRec> hist;   // recent pulses (train: incl. extras)
    bool lost = false;
    // train
    double period = 0, t_ref = 0, jitter = 0;
    double ppm_first = 0, ppm_prev = 0;
    std::string mode, mode_note;
    int switch_votes = 0;
    double switch_to = 0;
    double last_est_t = -1e9;
    // data stream
    std::vector<wl::StreamEvent> stream;
    int run = 0;
    double run_t = 0;
    double last_pulse_t = -1e9;
    bool in_stream = false;
    int normals_since_r = 0;
    std::string code_text, code_kind;
    std::vector<std::pair<std::string, std::string>> code_fields;
    int code_repeats = 0;
    double code_t = 0;
    // coded tag
    double gaps[3] = {0, 0, 0};
    double bi = 0;
    std::vector<double> bursts;   // burst start times
    bool confirmed = false;
    std::deque<std::pair<double, float>> levels;   // (t, C/N0) for the history
    double pub_t = -1e9;
    int half_hits = 0;       // pulses half way between its slots
    float cn0_med = 0;       // running medians of its on-grid pulses
    double width_med = 0;
};

struct Chan {
    double freq = 0;
    std::deque<PRec> recent;  // all pulses, last TRAIN_WIN_S
    std::vector<PRec> grp;    // a possible coded burst, waiting
    int train = -1;           // tx index
    double last_t = -1e9;
};

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

class Wildlife : public kp::Decoder {
public:
    explicit Wildlife(kp::Host& h) : Decoder(h) {
        rate_ = host.sample_rate();
        if (!(rate_ > 0)) rate_ = 48000;
        det_.configure(rate_);
        host.table_columns({"Collar", "Name", "MHz", "Ch", "ppm", "Pulse", "Mode", "Code", "C/N0", "Level", "Pulses"});
        host.table_keep(keep_h_ * 3600);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "names") parse_names(v);
        else if (k == "profile") profile_ = v.empty() ? "nz" : v;
        else if (k == "doa_avg") doa_avg_ = std::clamp(atof(v.c_str()), 1.0, 600.0);
        else if (k == "keep_h") {
            keep_h_ = std::clamp(atof(v.c_str()), 0.02, 168.0);
            host.table_keep(keep_h_ * 3600);
        } else if (k == "chan_bw") chan_bw_ = std::clamp(atof(v.c_str()), 300.0, 5000.0);
        else if (k == "sens") det_.set_sensitivity(atoi(v.c_str()));
        else if (k == "focus") focus_ = v;
        else if (k == "verbose") verbose_ = v == "1";
        if (k == "names")
            for (auto& t : tx_) {
                t.name = name_for(t);
                if (t.confirmed) publish(t, true);
            }
    }

    void reset() override {
        det_.configure(rate_);
        tx_.clear();
        ch_.clear();
        t_base_ = NAN;
        last_pub_ = -1e9;
        n_pulses_ = 0;
    }

    void process(const kp::cf* x, size_t n) override {
        if (!std::isfinite(t_base_)) t_base_ = host.time() - static_cast<double>(det_.samples()) / rate_;
        rf_ = host.rf_hz();
        inv_ = host.rf_inverted();
        update_gates();
        pulses_.clear();
        det_.push(x, n, pulses_);
        if (!pulses_.empty()) take(pulses_);
        const double now = host.time() + static_cast<double>(n) / rate_;
        tick(now);
    }

private:
    double rate_ = 48000;
    wl::PulseDetector det_;
    std::vector<wl::Pulse> pulses_;
    std::vector<Tx> tx_;
    std::vector<Chan> ch_;
    std::deque<PRec> recent_all_;   // splatter check
    double t_base_ = NAN, rf_ = NAN;
    bool inv_ = false;
    double last_pub_ = -1e9, last_facts_ = -1e9, last_plot_ = -1e9;
    uint64_t n_pulses_ = 0, n_twins_ = 0;
    std::string profile_ = "nz", focus_;
    double doa_avg_ = 10, keep_h_ = 12, chan_bw_ = 1000;
    bool verbose_ = false;
    std::vector<std::pair<double, std::string>> names_num_;   // MHz -> name
    std::map<std::string, std::string> names_id_;

    double t_of(double s) const { return t_base_ + s / rate_; }
    double abs_mhz(double f) const {
        if (!std::isfinite(rf_)) return NAN;
        return (inv_ ? rf_ - f : rf_ + f) / 1e6;
    }

    void parse_names(const std::string& v) {
        names_num_.clear();
        names_id_.clear();
        size_t a = 0;
        while (a < v.size()) {
            size_t b = v.find_first_of(",;\n", a);
            if (b == std::string::npos) b = v.size();
            std::string item = v.substr(a, b - a);
            a = b + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
                return s;
            };
            std::string key = trim(item.substr(0, eq)), name = kp::printable(trim(item.substr(eq + 1)));
            if (key.empty() || name.empty()) continue;
            if (name.size() > 40) name.resize(40);
            char* end = nullptr;
            const double mhz = strtod(key.c_str(), &end);
            if (end && *end == '\0' && mhz > 1 && mhz < 6000) names_num_.push_back({mhz, name});
            else names_id_[key] = name;
        }
    }
    std::string name_for(const Tx& t) const {
        auto it = names_id_.find(t.id);
        if (it != names_id_.end()) return it->second;
        const double m = abs_mhz(t.freq);
        if (std::isfinite(m)) {
            double best = 0.0015;
            std::string nm;
            for (const auto& kv : names_num_)
                if (std::fabs(kv.first - m) <= best) {
                    best = std::fabs(kv.first - m);
                    nm = kv.second;
                }
            if (!nm.empty()) return nm;
        }
        return "";
    }

    // NZ wildlife channels: 160.120 + 0.010 n MHz (Telonics TR4 channels 0-47 general use, 48-99 special licence)
    static std::string nz_channel(double mhz) {
        if (!std::isfinite(mhz) || mhz < 160.115 || mhz > 161.115) return "";
        const long n = std::lround((mhz - 160.120) / 0.010);
        if (n < 0 || n > 99 || std::fabs(mhz - (160.120 + 0.010 * n)) > 0.0025) return "";
        return std::to_string(n);
    }

    std::string make_id(double f, const std::string& suffix) const {
        const double m = abs_mhz(f);
        std::string base = std::isfinite(m) ? fmt("%.3f", m) : fmt("%+.1fk", f / 1e3);
        base += suffix;
        std::string id = base;
        for (int k = 0; k < 26; k++) {
            bool clash = false;
            for (const auto& t : tx_) clash = clash || t.id == id;
            if (!clash) break;
            id = base + static_cast<char>('b' + k);
        }
        return id.substr(0, 32);
    }

    // ---- pulses in -----------------------------------------------------
    void take(std::vector<wl::Pulse>& ps) {
        // strongest first: a pulse 20 dB under an overlapping one within 6 kHz is its splatter
        std::sort(ps.begin(), ps.end(), [](const wl::Pulse& a, const wl::Pulse& b) { return a.power_db > b.power_db; });
        std::vector<PRec> keep;
        for (const auto& p : ps) {
            PRec r;
            r.t0 = t_of(p.t0);
            r.t1 = t_of(p.t1);
            r.freq = p.freq;
            r.pdb = p.power_db;
            r.cn0 = p.cn0_db;
            bool spl = false;
            auto check = [&](const PRec& o) {
                const double ov = std::min(o.t1, r.t1) - std::max(o.t0, r.t0);
                const double df = std::fabs(o.freq - r.freq);
                // splatter of a much stronger pulse, or the same pulse twice
                if (ov > 0.5 * (r.t1 - r.t0) &&
                    ((df < 6000 && o.pdb > r.pdb + 20) || (df < 3000 && o.pdb > r.pdb + 10) || df < 300))
                    spl = true;
            };
            for (const auto& o : keep) check(o);
            for (const auto& o : recent_all_) check(o);
            if (spl) continue;
            keep.push_back(r);
        }
        // twins: pulses starting and ending together (15 ms) 0.6-10 kHz apart at a
        // similar level are one data / FM burst's lines (FSK tones), not
        // collars (live: a ~6 kHz wide burst every 2.5 s near 160.40 MHz
        // read as three "collars" in lockstep)
        std::vector<uint8_t> twin(keep.size(), 0);
        // and pieces of a multi-tone data burst: within 60 ms of each other,
        // 0.6-8.5 kHz apart (live: a ~5 kHz wide burst near 160.40 MHz every
        // 2.5 s came out as tone pieces of 1-47 ms at four frequencies).
        // Collars on neighbouring 10 kHz channels are never twins; collars
        // closer than 8.5 kHz lose the pulses that happen to coincide
        auto is_twin = [](const PRec& a, const PRec& b) {
            const double df = std::fabs(a.freq - b.freq);
            if (df <= 600 || df >= 8500 || std::fabs(a.pdb - b.pdb) >= 10) return false;
            return (std::fabs(a.t0 - b.t0) < 0.015 && std::fabs(a.t1 - b.t1) < 0.015) ||
                   (std::min(a.t1, b.t1) > std::max(a.t0, b.t0) - 0.06);
        };
        for (size_t i = 0; i < keep.size(); i++) {
            for (size_t j = i + 1; j < keep.size(); j++)
                if (is_twin(keep[i], keep[j])) twin[i] = twin[j] = 1;
            for (const auto& o : recent_all_)
                if (is_twin(keep[i], o)) twin[i] = 1;
        }
        std::vector<PRec> k2;
        for (size_t i = 0; i < keep.size(); i++)
            if (!twin[i]) k2.push_back(keep[i]);
            else n_twins_++;
        // twins still count as recent (their partner may come next batch)
        for (size_t i = 0; i < keep.size(); i++)
            if (twin[i]) recent_all_.push_back(keep[i]);
        keep.swap(k2);
        std::sort(keep.begin(), keep.end(), [](const PRec& a, const PRec& b) { return a.t0 < b.t0; });
        for (const auto& r : keep) {
            recent_all_.push_back(r);
            n_pulses_++;
            if (verbose_)
                host.log(fmt("pulse %.4f", r.t0) + fmt(" %.1f ms", (r.t1 - r.t0) * 1e3) + fmt(" %+.1f Hz", r.freq) +
                         fmt(" %.1f dBHz", r.cn0));
            on_pulse(r);
        }
        if (!keep.empty())
            while (!recent_all_.empty() && recent_all_.front().t1 < keep.back().t0 - 1.0) recent_all_.pop_front();
    }

    Chan& chan_for(double f) {
        Chan* best = nullptr;
        double bd = FREQ_TOL_HZ;
        for (auto& c : ch_)
            if (std::fabs(c.freq - f) < bd) {
                bd = std::fabs(c.freq - f);
                best = &c;
            }
        if (best) return *best;
        if (ch_.size() >= 300) {   // the stalest goes
            auto it = std::min_element(ch_.begin(), ch_.end(), [](const Chan& a, const Chan& b) { return a.last_t < b.last_t; });
            const int gone = static_cast<int>(it - ch_.begin());
            ch_.erase(it);
            for (auto& c : ch_)
                if (c.train > gone) c.train--;
            (void)gone;
        }
        ch_.push_back({});
        ch_.back().freq = f;
        return ch_.back();
    }

    void on_pulse(const PRec& r) {
        Chan& c = chan_for(r.freq);
        // a pulse close to the previous one may belong to a coded burst
        if (!c.grp.empty() && r.t0 - c.grp.back().t0 > GROUP_GAP_S) close_group(c);
        c.grp.push_back(r);
        c.last_t = r.t1;
        if (c.grp.size() > 6) close_group(c);
    }

    // the group is complete (or can't be a coded burst): a coded tag's
    // burst, or pulses of the channel's train
    void close_group(Chan& c) {
        if (c.grp.empty()) return;
        std::vector<PRec> g;
        g.swap(c.grp);
        // a fragment starting within 12 ms of a longer pulse is part of it
        for (size_t i = 0; i < g.size();) {
            bool frag = false;
            for (size_t j = 0; j < g.size() && !frag; j++)
                frag = j != i && std::fabs(g[j].t0 - g[i].t0) < 0.012 && (g[j].t1 - g[j].t0) > (g[i].t1 - g[i].t0);
            if (frag) g.erase(g.begin() + static_cast<long>(i));
            else i++;
        }
        double prev = -1e9;
        for (const auto& p : c.recent) prev = std::max(prev, p.t0);
        const bool train_here = c.train >= 0 && c.train < static_cast<int>(tx_.size()) && tx_[static_cast<size_t>(c.train)].confirmed;
        bool coded = g.size() == 4 && g.front().t0 - prev > QUIET_S && g.back().t0 - g.front().t0 < 0.4;
        if (coded && train_here) {
            // on a collar's channel only a coded tag already known there (a
            // data stream's digit burst of 4 pulses is the collar's)
            bool known = false;
            for (const auto& x : tx_)
                if (x.coded && x.confirmed && std::fabs(x.freq - c.freq) <= FREQ_TOL_HZ) {
                    bool same = true;
                    for (int i = 0; i < 3; i++)
                        same = same && std::fabs(x.gaps[i] - (g[static_cast<size_t>(i) + 1].t0 - g[static_cast<size_t>(i)].t0) * 1e3) <= GAP_TOL_MS;
                    known = known || same;
                }
            coded = known;
        }
        for (auto& p : g) p.gsize = static_cast<int>(g.size());
        for (const auto& p : g) {
            c.recent.push_back(p);
            while (!c.recent.empty() && c.recent.front().t0 < p.t0 - TRAIN_WIN_S) c.recent.pop_front();
        }
        if (coded) {
            coded_burst(c, g);
            return;
        }
        for (const auto& p : g) train_pulse(c, p);
    }

    static bool on_grid(const Tx& t, double t0) {
        if (!(t.period > 0)) return false;
        const double q = (t0 - t.t_ref) / t.period;
        return std::fabs((q - std::round(q)) * t.period) <= wl::grid_tol(t.period);
    }

    // ---- coded tags (Lotek / Motus style) ------------------------------
    void coded_burst(Chan& c, const std::vector<PRec>& g) {
        double gp[3];
        for (int i = 0; i < 3; i++) gp[i] = (g[static_cast<size_t>(i) + 1].t0 - g[static_cast<size_t>(i)].t0) * 1e3;
        const double t = g.front().t0;
        Tx* best = nullptr;
        for (auto& x : tx_) {
            if (!x.coded || std::fabs(x.freq - c.freq) > FREQ_TOL_HZ) continue;
            bool same = true;
            for (int i = 0; i < 3; i++) same = same && std::fabs(x.gaps[i] - gp[i]) <= GAP_TOL_MS;
            if (!same) continue;
            if (x.bi > 0) {
                const double k = std::round((t - x.bursts.back()) / x.bi);
                if (k >= 1 && std::fabs(t - x.bursts.back() - k * x.bi) <= 0.03 + 0.005 * k) { best = &x; break; }
                continue;   // another tag with these gaps (aliased: other burst interval)
            }
            best = &x;
        }
        if (!best) {
            if (tx_.size() >= 400) drop_stalest();
            tx_.push_back({});
            best = &tx_.back();
            best->coded = true;
            best->freq = c.freq;
            best->first_t = t;
            for (int i = 0; i < 3; i++) best->gaps[i] = gp[i];
        }
        Tx& x = *best;
        for (int i = 0; i < 3; i++) x.gaps[i] += 0.2 * (gp[i] - x.gaps[i]);
        x.bursts.push_back(t);
        if (x.bursts.size() > 50) x.bursts.erase(x.bursts.begin());
        for (const auto& p : g) add_hist(x, p);
        if (!x.confirmed && x.bursts.size() >= 3) {
            // burst interval: the shortest interval every interval is a multiple of
            double dmin = 1e9;
            for (size_t i = 1; i < x.bursts.size(); i++) dmin = std::min(dmin, x.bursts[i] - x.bursts[i - 1]);
            for (int div = 1; div <= 3 && x.bi == 0; div++) {
                const double bi = dmin / div;
                if (bi < BI_MIN || bi > BI_MAX) continue;
                bool ok = true;
                double sk = 0, sd = 0;
                for (size_t i = 1; i < x.bursts.size(); i++) {
                    const double d = x.bursts[i] - x.bursts[i - 1], k = std::round(d / bi);
                    if (k < 1 || std::fabs(d - k * bi) > 0.03 + 0.005 * k) ok = false;
                    sk += k;
                    sd += d;
                }
                if (ok) x.bi = sd / sk;
            }
            if (x.bi > 0) {
                x.confirmed = true;
                char b[80];
                snprintf(b, sizeof b, " %.1f/%.1f/%.1f@%.1f", x.gaps[0], x.gaps[1], x.gaps[2], x.bi);
                x.id = make_id(x.freq, b);
                x.name = name_for(x);
                x.mode = "coded";
                host.event("🐾 New coded tag " + x.id + (x.name.empty() ? "" : " (" + x.name + ")") +
                               fmt(": 4-pulse bursts every %.2f s", x.bi) + fmt(", %.1f dBHz", g.front().cn0),
                           3600);
            }
        } else if (x.bi > 0 && x.bursts.size() >= 2) {
            const double d = x.bursts.back() - x.bursts[x.bursts.size() - 2], k = std::round(d / x.bi);
            if (k >= 1 && k <= 4) x.bi += 0.2 * (d / k - x.bi);
        }
        if (x.confirmed) {
            for (const auto& p : g) report(x, p);
            note_heard(x, g.back());
        }
    }

    // ---- pulse trains --------------------------------------------------
    void train_pulse(Chan& c, const PRec& p) {
        if (c.train >= 0 && c.train < static_cast<int>(tx_.size())) {
            Tx& t = tx_[static_cast<size_t>(c.train)];
            PRec q = p;
            q.grid = on_grid(t, p.t0);
            // an extra pulse (off the grid) that doesn't look like this
            // collar's - far weaker or shorter - is a fragment / noise: dropped
            if (t.pulses >= 4 && (p.cn0 < t.cn0_med - 8 || (p.t1 - p.t0) * 1e3 < 0.4 * t.width_med)) return;
            // a weak collar found at twice its period: its other pulses come
            // half way between (two of them, single, of its length = halve it)
            // (isolated ones: a data stream's fast pulses are no slots)
            if (!q.grid && !t.in_stream && t.period >= 2 * PMIN && p.gsize == 1 && p.t0 - t.last_pulse_t > 0.4 * t.period &&
                std::fabs((p.t1 - p.t0) * 1e3 - t.width_med) < 0.5 * t.width_med) {
                const double qh = (p.t0 - t.t_ref) / t.period;
                if (std::fabs((qh - std::floor(qh) - 0.5) * t.period) <= wl::grid_tol(t.period / 2)) {
                    if (++t.half_hits >= 2) {
                        // just found: it was always this fast; else its rate doubled
                        const double old_ppm = 60.0 / t.period;
                        t.period /= 2;
                        t.half_hits = 0;
                        if (t.pulses < 12) classify(t, 0, true);
                        else classify(t, old_ppm, false);
                        publish(t, true);
                    }
                    q.grid = on_grid(t, p.t0);
                }
            }
            if (q.grid) {
                const double k = std::round((p.t0 - t.t_ref) / t.period);
                if (k >= 1 && k <= 4) t.period += 0.05 * ((p.t0 - t.t_ref) / k - t.period);
                t.t_ref = p.t0;
            }
            add_hist(t, q);
            stream_step(t, q);
            rate_check(t, p.t0);
            report(t, q);
            note_heard(t, q);
            return;
        }
        // discovery: a period among the channel's recent pulses
        std::vector<double> ts;
        // a train of single pulses of a collar's length (a coded tag's few-ms
        // pulses and its bursts - also with some missed - are no train)
        auto single = [](const PRec& r) { return r.gsize == 1 && r.t1 - r.t0 >= 0.004; };
        if (!single(p)) return;
        for (const auto& r : c.recent)
            if (r.t0 > p.t0 - TRAIN_WIN_S && single(r)) ts.push_back(r.t0);
        if (static_cast<int>(ts.size()) < CONFIRM_PULSES) return;
        const wl::Period P = wl::find_period(ts, PMIN, PMAX);
        if (P.support < CONFIRM_PULSES || P.score < CONFIRM_PULSES - 0.5 || P.slots > 3 * P.support) return;
        // a transmitter's crystal keeps time: the pulses sit on the grid to a few ms
        if (P.jitter > 0.0025 + 0.001 * P.period) return;
        // a collar's pulses all have about one length (a data burst's tone
        // pieces don't): the median absolute deviation <= 30 % of the median
        {
            std::vector<double> w;
            for (const auto& r : c.recent) {
                if (r.t0 <= p.t0 - TRAIN_WIN_S || !single(r)) continue;
                const double q = (r.t0 - P.t_ref) / P.period;
                if (std::fabs((q - std::round(q)) * P.period) <= wl::grid_tol(P.period)) w.push_back(r.t1 - r.t0);
            }
            if (w.size() >= 3) {
                std::vector<double> v(w);
                std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
                const double med = v[v.size() / 2];
                for (auto& x : v) x = std::fabs(x - med);
                std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
                if (v[v.size() / 2] > 0.3 * med) return;
            }
        }
        // and pulses briefly: 10-60 ms every 0.4-8 s (a line of a steadily
        // modulated FM signal "pulses" with a far larger duty cycle)
        {
            std::vector<double> w;
            for (const auto& r : c.recent)
                if (r.t0 > p.t0 - TRAIN_WIN_S && single(r)) w.push_back(r.t1 - r.t0);
            std::nth_element(w.begin(), w.begin() + static_cast<long>(w.size() / 2), w.end());
            if (w[w.size() / 2] > 0.1 * P.period) return;
        }
        // the latest pulse must be on it (the grid is followed from here)
        const double qq = (p.t0 - P.t_ref) / P.period;
        if (std::fabs((qq - std::round(qq)) * P.period) > wl::grid_tol(P.period)) return;
        if (tx_.size() >= 400) drop_stalest();
        tx_.push_back({});
        Tx& t = tx_.back();
        c.train = static_cast<int>(tx_.size()) - 1;
        t.freq = c.freq;
        t.first_t = ts.front();
        t.period = P.period;
        t.t_ref = p.t0;
        t.jitter = P.jitter;
        t.confirmed = true;
        t.id = make_id(c.freq, "");
        t.name = name_for(t);
        for (const auto& r : c.recent) {
            PRec q = r;
            q.grid = on_grid(t, r.t0);
            add_hist(t, q);
        }
        t.pulses = static_cast<uint64_t>(P.support);
        t.ppm_first = 60.0 / t.period;
        t.last_pulse_t = p.t0;
        classify(t, 0, true);
        const std::string ch = nz_channel(abs_mhz(t.freq));
        host.event("🐾 New collar " + t.id + (ch.empty() ? "" : " (NZ ch " + ch + ")") +
                       (t.name.empty() ? "" : " - " + t.name) + fmt(": %.1f ppm", 60.0 / t.period) +
                       fmt(", %.0f ms pulses", (p.t1 - p.t0) * 1e3) + fmt(", %.1f dBHz", p.cn0) + " - " + t.mode,
                   3600);
        report(t, p);
        note_heard(t, p);
    }

    // a rate change: the best period over the last 12 s, if it beats the
    // current one three times running
    void rate_check(Tx& t, double now) {
        if (now - t.last_est_t < 0.5 * t.period) return;
        t.last_est_t = now;
        std::vector<double> ts;
        for (const auto& r : t.hist)
            if (r.t0 > now - 12.0) ts.push_back(r.t0);
        if (ts.size() < 6 || ts.back() - ts.front() < 6.0) return;
        const wl::Period P = wl::find_period(ts, PMIN, PMAX);
        const double rel = P.period / t.period;
        // a new rate fills its grid (a data stream's bursts don't) over >= 8 s
        int on_new = 0;
        double first_on = 1e18;
        for (double x : ts) {
            const double q = (x - P.t_ref) / std::max(P.period, 1e-3);
            if (std::fabs((q - std::round(q)) * P.period) <= wl::grid_tol(P.period)) {
                on_new++;
                first_on = std::min(first_on, x);
            }
        }
        const bool filled = P.period > 0 && P.support >= 6 && P.support >= 0.8 * P.slots && ts.back() - first_on >= 8.0;
        (void)on_new;
        if (!filled || std::fabs(rel - 1) < 0.08) {
            t.switch_votes = 0;
            return;
        }
        // the current grid's own score over the same pulses
        int on = 0;
        for (double x : ts) on += on_grid(t, x);
        const double span = ts.back() - ts.front();
        const double cur = on - 0.5 * (span / t.period + 1 - on);
        if (P.score <= cur + 1) {
            t.switch_votes = 0;
            return;
        }
        if (t.switch_votes && std::fabs(P.period / t.switch_to - 1) > 0.05) t.switch_votes = 0;
        t.switch_to = P.period;
        if (++t.switch_votes < 3) return;
        t.switch_votes = 0;
        const double old_ppm = 60.0 / t.period;
        t.period = P.period;
        t.t_ref = P.t_ref;
        t.jitter = P.jitter;
        classify(t, old_ppm, false);
    }

    void classify(Tx& t, double old_ppm, bool first) {
        const double ppm = 60.0 / t.period;
        std::string m = "normal";
        const double q = old_ppm > 0 ? ppm / old_ppm : 1.0;
        auto near = [&](double a, double tol) { return std::fabs(ppm - a) <= tol; };
        if (profile_ == "nz") {
            if (near(48, 4)) m = "incubating";
            else if (near(60, 5) || near(80, 6)) m = "MORTALITY";
        }
        if (!first) {
            if ((q > 1.8 && q < 2.25) || (q > 2.4 && q < 2.9)) m = "MORTALITY";
            else if (q > 1.45 && q < 1.75 && m == "normal") m = profile_ == "nz" && old_ppm > 26 && old_ppm < 34 ? "incubating" : "fast";
            else if (q < 0.7 && m == "normal") m = "normal";
        }
        const std::string old = t.mode;
        t.mode = m;
        t.ppm_prev = old_ppm;
        if (first) {
            if (m == "MORTALITY")
                host.event("⚠ MORTALITY? " + label_name(t) + fmt(" pulses at %.0f ppm", ppm) +
                               " - the NZ mortality rate (double the normal 30 ppm): the animal may be dead or the collar off",
                           3600);
            return;
        }
        if (m == "MORTALITY" && old != "MORTALITY")
            host.event("⚠ MORTALITY: " + label_name(t) + fmt(" rate %.0f", old_ppm) + fmt(" -> %.0f ppm", ppm) +
                           " - no movement for the transmitter's mortality time (dead, or the collar came off)",
                       600);
        else if (m != old)
            host.event("🐾 " + label_name(t) + ": " + old + " -> " + m + fmt(" (%.0f", old_ppm) + fmt(" -> %.0f ppm)", ppm), 600);
        else
            host.event("🐾 " + label_name(t) + fmt(": rate %.1f", old_ppm) + fmt(" -> %.1f ppm", ppm), 600);
        publish(t, true);
    }

    std::string label_name(const Tx& t) const { return t.id + (t.name.empty() ? "" : " (" + t.name + ")"); }

    // ---- data streams (NZ: time since death, Egg / Chick Timer) ---------
    void stream_step(Tx& t, const PRec& p) {
        const double d = p.t0 - t.last_pulse_t;
        t.last_pulse_t = p.t0;
        const bool close = d < std::min(0.5 * t.period, 1.0);
        if (close) {
            t.run++;
            return;
        }
        // the run before this pulse is complete
        if (t.run >= 1) {
            if (t.run >= 2) {
                if (!t.in_stream) {
                    t.in_stream = true;
                    t.stream.clear();
                }
                t.stream.push_back({t.run, t.run_t});
                t.normals_since_r = 0;
            } else if (t.in_stream) {
                t.stream.push_back({1, t.run_t});
                if (++t.normals_since_r >= 12) end_stream(t);
            }
        }
        t.run = 1;
        t.run_t = p.t0;
        if (t.in_stream && !t.stream.empty() && p.t0 - t.stream.front().t > 400) end_stream(t);   // kakapo: 11 outputs ~3 min
    }

    void end_stream(Tx& t) {
        t.in_stream = false;
        // drop the trailing normal pulses
        while (!t.stream.empty() && t.stream.back().run == 1) t.stream.pop_back();
        t.stream.push_back({1, 0});
        t.stream.push_back({1, 0});
        const wl::StreamResult r = wl::decode_stream(t.stream);
        if (verbose_) {
            std::string ev;
            for (const auto& e : t.stream) ev += e.run == 1 ? "." : " " + std::to_string(e.run) + " ";
            host.log(t.id + ": stream" + ev);
        }
        t.stream.clear();
        if (!r.ok) {
            if (verbose_) host.log(t.id + ": a data stream that didn't decode");
            return;
        }
        const bool same = r.text == t.code_text;
        t.code_repeats = same ? t.code_repeats + 1 : 1;
        t.code_text = r.text;
        t.code_kind = r.kind;
        t.code_fields = r.fields;
        t.code_t = host.time();
        const std::string what = r.kind == "time-since-death" ? "mortality data" :
                                 r.kind == "chick-timer" ? "Chick Timer data" :
                                 r.kind == "egg-timer" ? "Egg Timer data" : "code";
        if (same) host.event("🐾 " + label_name(t) + ": " + what + " again (confirmed): " + r.text, 300);
        else host.event((r.kind == "time-since-death" ? "⚠ " : "🐾 ") + label_name(t) + ": " + what + " - " + r.text, 60);
        if (host.raw_wanted()) {
            std::string raw = t.id + " " + r.scheme;
            for (int v : r.numbers) raw += " " + std::to_string(v);
            host.raw(raw);
        }
        publish(t, true);
    }

    // ---- output --------------------------------------------------------
    void add_hist(Tx& t, const PRec& p) {
        t.hist.push_back(p);
        while (t.hist.size() > 400 || (!t.hist.empty() && t.hist.front().t0 < p.t0 - 900)) t.hist.pop_front();
        t.freq += (t.pulses < 8 ? 0.3 : 0.1) * (p.freq - t.freq);
    }

    void note_heard(Tx& t, const PRec& p) {
        // running medians (a step towards each new value)
        const double w = (p.t1 - p.t0) * 1e3;
        if (t.pulses == 0 || t.width_med == 0) {
            t.cn0_med = p.cn0;
            t.width_med = w;
        } else if (p.grid || t.coded) {
            t.cn0_med += std::clamp(p.cn0 - t.cn0_med, -0.5f, 0.5f);
            t.width_med += std::clamp(w - t.width_med, -1.0, 1.0);
        }
        t.pulses++;
        t.last_t = p.t1;
        t.cn0 = p.cn0;
        t.pdb = p.pdb;
        t.width_ms = (p.t1 - p.t0) * 1e3;
        t.levels.push_back({p.t0, p.cn0});
        while (t.levels.size() > 600 || (!t.levels.empty() && t.levels.front().first < p.t0 - 600)) t.levels.pop_front();
        if (t.lost) {
            t.lost = false;
            host.event("🐾 " + label_name(t) + " heard again" + fmt(" (%.1f dBHz)", p.cn0), 60);
        }
        publish(t, false);
    }

    std::string talker_label(const Tx& t) const {
        std::string l = t.name.empty() ? "" : t.name + " · ";
        if (t.coded) l += fmt("coded · %.2f s bursts", t.bi);
        else l += fmt("%.1f ppm · ", 60.0 / t.period) + t.mode;
        return l.substr(0, 64);
    }

    void report(const Tx& t, const PRec& p) {
        if (!t.confirmed) return;
        host.valid(p.t0, p.t1);
        kp::Talker k;
        k.id = t.id;
        k.label = talker_label(t);
        // the pulse's own samples, just inside its (half-power) edges
        k.start_s = p.t0 + 0.0001;
        k.end_s = std::min(p.t1 - 0.0001, k.start_s + 0.24);
        if (k.end_s <= k.start_s) return;
        k.packet = true;
        k.freq_hz = p.freq;
        k.bw_hz = chan_bw_;
        k.avg_s = doa_avg_;
        k.keep_s = keep_h_ * 3600;
        host.talker(k);
    }

    void publish(Tx& t, bool force) {
        if (!t.confirmed) return;
        // a row at most once a second (a data stream sends 4 pulses / s)
        const double now = t.last_t;
        if (!force && now - t.pub_t < 1.0) return;
        t.pub_t = now;
        const double m = abs_mhz(t.freq);
        std::string code = t.code_text.empty() ? (t.coded ? fmt("%.1f/", t.gaps[0]) + fmt("%.1f/", t.gaps[1]) +
                                                                fmt("%.1f ms", t.gaps[2]) + fmt(" @%.2f s", t.bi)
                                                          : "")
                                               : t.code_text + (t.code_repeats > 1 ? " ✓" : "");
        host.table_row(t.id, {t.id, t.name, std::isfinite(m) ? fmt("%.4f", m) : fmt("%+.0f Hz", t.freq),
                              nz_channel(m), t.coded ? "" : fmt("%.1f", 60.0 / t.period), fmt("%.0f ms", t.width_ms),
                              t.lost ? t.mode + " (lost)" : t.mode, code, fmt("%.1f dBHz", t.cn0),
                              fmt("%.1f dB", t.pdb), std::to_string(t.pulses)});
        std::vector<std::pair<std::string, std::string>> d;
        d.push_back({"Carrier", std::isfinite(m) ? fmt("%.6f MHz", m) : fmt("%+.1f Hz from the VFO centre", t.freq)});
        if (!nz_channel(m).empty())
            d.push_back({"NZ channel", nz_channel(m) + (std::stoi(nz_channel(m)) > 47 ? " (special licence)" : "") +
                                           fmt(", %+.0f Hz off it", (m - (160.120 + 0.010 * std::stoi(nz_channel(m)))) * 1e6)});
        if (t.coded) {
            d.push_back({"Coded tag", fmt("4 pulses, gaps %.2f / ", t.gaps[0]) + fmt("%.2f / ", t.gaps[1]) +
                                          fmt("%.2f ms", t.gaps[2])});
            d.push_back({"Burst interval", fmt("%.3f s", t.bi)});
            d.push_back({"Note", "Lotek / Motus style: the gaps + burst interval name the tag (Lotek's code table "
                                 "itself is not public)"});
        } else {
            d.push_back({"Period", fmt("%.1f ms", t.period * 1e3) + fmt(" (%.2f ppm)", 60.0 / t.period) +
                                       fmt(", jitter %.1f ms", t.jitter * 1e3)});
            if (t.ppm_prev > 0) d.push_back({"Previous rate", fmt("%.1f ppm", t.ppm_prev)});
            d.push_back({"Mode", t.mode + (profile_ == "nz" ? " (NZ: 30 normal, 48 incubating, 60 / 80 mortality)" : "")});
        }
        // drift over the history
        if (t.hist.size() >= 6) {
            const double dt = t.hist.back().t0 - t.hist.front().t0;
            if (dt > 30) {
                // medians of the first and the last quarter
                const size_t q = t.hist.size() / 4;
                std::vector<double> a, b;
                for (size_t i = 0; i < q; i++) {
                    a.push_back(t.hist[i].freq);
                    b.push_back(t.hist[t.hist.size() - 1 - i].freq);
                }
                std::nth_element(a.begin(), a.begin() + static_cast<long>(q / 2), a.end());
                std::nth_element(b.begin(), b.begin() + static_cast<long>(q / 2), b.end());
                const double span = (t.hist[t.hist.size() - 1 - q / 2].t0 - t.hist[q / 2].t0);
                if (span > 10) d.push_back({"Drift", fmt("%+.1f Hz/min", (b[q / 2] - a[q / 2]) / span * 60)});
            }
        }
        d.push_back({"Pulse", fmt("%.1f ms", t.width_ms) + fmt(", %.1f dBHz", t.cn0) + fmt(", level %.1f dB", t.pdb)});
        for (const auto& f : t.code_fields) d.push_back({f.first, f.second});
        if (!t.code_text.empty()) d.push_back({"Data stream", t.code_kind + (t.code_repeats > 1 ? " (confirmed)" : "")});
        // signal strength history (walking DF): the last pulses
        std::string hs;
        const double tl = t.levels.empty() ? 0 : t.levels.back().first;
        int n = 0;
        for (auto it = t.levels.rbegin(); it != t.levels.rend() && n < 20; ++it, ++n)
            hs += (n ? ", " : "") + fmt("%.0f", it->second) + fmt(" (-%.0f s)", tl - it->first);
        d.push_back({"C/N0 history (dBHz)", hs});
        host.table_detail(t.id, d);
    }

    void drop_stalest() {
        auto it = std::min_element(tx_.begin(), tx_.end(), [](const Tx& a, const Tx& b) { return a.last_t < b.last_t; });
        const int gone = static_cast<int>(it - tx_.begin());
        tx_.erase(it);
        for (auto& c : ch_) {
            if (c.train == gone) c.train = -1;
            else if (c.train > gone) c.train--;
        }
    }

    void update_gates() {
        std::vector<wl::Gate> g;
        const double s_now = static_cast<double>(det_.samples());
        const double now = t_of(s_now);
        for (const auto& t : tx_) {
            if (t.coded || !t.confirmed || !(t.period > 0) || t.lost) continue;
            const double k = std::ceil((now - 0.05 - t.t_ref) / t.period);
            const double tn = t.t_ref + std::max(1.0, k) * t.period;
            const double tol = wl::grid_tol(t.period);
            wl::Gate gg;
            gg.freq = t.freq;
            gg.from = static_cast<uint64_t>(std::max(0.0, (tn - tol - t_base_) * rate_));
            gg.to = static_cast<uint64_t>(std::max(0.0, (tn + tol + 0.05 - t_base_) * rate_));
            g.push_back(gg);
        }
        det_.set_gates(g);
    }

    void tick(double now) {
        for (auto& c : ch_)
            if (!c.grp.empty() && now - c.grp.back().t1 > GROUP_GAP_S + 0.02) close_group(c);
        for (auto& t : tx_) {
            if (!t.confirmed || t.lost) continue;
            const double quiet = t.coded ? std::max(30.0, 4 * t.bi) : std::max(30.0, 12 * t.period);
            if (now - t.last_t > quiet) {
                t.lost = true;
                host.event("🐾 " + label_name(t) + " lost" + fmt(" (last heard %.0f s ago)", now - t.last_t), 60);
                publish(t, true);
            }
            if (t.in_stream && !t.stream.empty() && now - t.stream.back().t > STREAM_END_S) end_stream(t);
        }
        if (now - last_facts_ >= 1.0) {
            last_facts_ = now;
            facts(now);
        }
        if (now - last_plot_ >= 1.0) {
            last_plot_ = now;
            plot(now);
        }
    }

    void facts(double now) {
        int conf = 0, heard = 0;
        const Tx* best = nullptr;
        for (const auto& t : tx_) {
            if (!t.confirmed) continue;
            conf++;
            if (now - t.last_t < 60) {
                heard++;
                if (!best || t.cn0 > best->cn0) best = &t;
            }
        }
        host.fact("Collars", std::to_string(conf) + (conf ? " (" + std::to_string(heard) + " heard in the last minute)" : ""));
        host.fact("Strongest", best ? label_name(*best) + fmt(", %.1f dBHz", best->cn0) : "");
        if (verbose_)
            host.fact("Rejected", "not narrow " + std::to_string(det_.rejected_wide()) + ", length " +
                                      std::to_string(det_.rejected_len()) + ", edges " + std::to_string(det_.rej_edge_) +
                                      ", level " + std::to_string(det_.rej_var_) + ", FM " + std::to_string(det_.rej_fm_));
        host.fact("Pulses", std::to_string(n_pulses_) + " measured" +
                               (n_twins_ ? ", " + std::to_string(n_twins_) + " dropped as data / FM lines" : ""));
        host.fact("Detector", fmt("%.0f Hz bins", det_.bin_hz()) + fmt(" at %.0f kHz", rate_ / 1e3) +
                                  fmt(", pulses from ~%.0f dBHz", det_.cn0_ref()) +
                                  (det_.blanked_frames() ? ", " + std::to_string(det_.blanked_frames()) + " impulse frames blanked" : ""));
        if (std::isfinite(rf_)) {
            const double lo = abs_mhz(-0.46 * rate_), hi = abs_mhz(0.46 * rate_);
            const double a = std::min(lo, hi), b = std::max(lo, hi);
            std::string band;
            if (b >= 160.12 && a <= 160.59) band = "NZ wildlife band 160.12-160.59 MHz (ch 0-47)";
            else if (b >= 160.60 && a <= 161.11) band = "NZ wildlife ch 48-99 (special licence)";
            else if ((b >= 148 && a <= 152) || (b >= 159 && a <= 161) || (b >= 164 && a <= 167) || (b >= 216 && a <= 220))
                band = "a wildlife tracking band (US 148-152 / 159-161 / 164-167 / 216-220 MHz)";
            if (b >= 166.38 && a <= 166.38) band += std::string(band.empty() ? "" : ", ") + "Motus 166.380 MHz";
            if (b >= 150.1 && a <= 150.1) band += std::string(band.empty() ? "" : ", ") + "Motus 150.100 MHz";
            if (b >= 151.5 && a <= 151.5) band += std::string(band.empty() ? "" : ", ") + "Motus 151.500 MHz";
            host.fact("Band", band.empty() ? fmt("%.3f", a) + fmt("-%.3f MHz (not a wildlife band)", b) : band);
        }
    }

    // the focus collar's (else the latest heard's) C/N0 per pulse, last 2 min
    void plot(double now) {
        const Tx* f = nullptr;
        for (const auto& t : tx_) {
            if (!t.confirmed || t.levels.empty()) continue;
            if (!focus_.empty() && (t.id == focus_ || t.name == focus_)) { f = &t; break; }
            if (!f || t.last_t > f->last_t) f = &t;
        }
        if (!f || now - f->last_t > 600) return;
        kp::Plot p;
        p.title = "Pulse strength - " + label_name(*f);
        p.x_unit = "s";
        p.y_unit = "dBHz";
        p.x0 = -120;
        p.dx = 1;
        p.y.assign(121, NAN);
        float last = f->levels.front().second;
        size_t j = 0;
        for (int i = 0; i <= 120; i++) {
            const double ts = now - 120 + i;
            while (j < f->levels.size() && f->levels[j].first <= ts) last = f->levels[j++].second;
            p.y[static_cast<size_t>(i)] = last;
        }
        p.threshold = det_.cn0_ref();
        p.markers.push_back({0, f->id, f->id});
        host.plot(p);
    }
};

}  // namespace

KRAKEN_PLUGIN(Wildlife, {.id = "wildlife",
                         .name = "Wildlife tracking collars (VHF pulse)",
                         .description = "VHF wildlife transmitters (kiwi / kakapo / deer collars, bird tags): pulsed CW "
                                        "anywhere in the VFO - a 2.4 MHz VFO surveys the band, a narrow one follows one "
                                        "collar. Per collar: frequency to a few Hz (NZ channel), ppm, pulse, mode "
                                        "(normal / incubating / MORTALITY), NZ time-since-death and Egg / Chick Timer "
                                        "data, Lotek / Motus-style coded tags, signal strength history; a DoA per collar",
                         .version = "1.0",
                         .sample_rate = 0,
                         .min_vfo_rate = 5000,
                         .author = "KrakenSDR",
                         .options = {{"names", "Collar names", "", "",
                                      "Known collars: MHz=name or id=name, comma separated (e.g. 160.512=Kiwi Bob, "
                                      "160.734=Kaka 3) - shown in the table and the 📡 DF list"},
                                     {"profile", "Pulse-rate meaning", "nz",
                                      "nz=NZ (30 normal / 48 incubating / 60 + 80 mortality)|change=Rate changes only "
                                      "(x2 = mortality)",
                                      "How a collar's rate is read: the NZ DOC convention, or only a change of rate "
                                      "while it is heard (other countries' collars)"},
                                     {"doa_avg", "DoA per collar", "10", "3=3 s|10=10 s|30=30 s|120=2 minutes",
                                      "How long each collar's pulses are averaged for its bearing (walking: short)"},
                                     {"keep_h", "Keep collars", "12", "1=1 hour|12=12 hours|24=24 hours|168=7 days",
                                      "How long a collar stays listed (table, 📡 DF list) after its last pulse"},
                                     {"chan_bw", "DoA channel", "1000", "500=500 Hz|1000=1 kHz|2000=2 kHz",
                                      "The bandwidth around each collar's carrier its bearing is computed from"},
                                     {"sens", "Sensitivity", "0", "-1=Lower (fewer false pulses)|0=Normal|1=Higher (+2 dB)",
                                      "Pulse detection thresholds"},
                                     {"focus", "Strength plot", "", "",
                                      "The collar (id or name) whose pulse strength is plotted; empty = the one heard last"}},
                         .map = false,
                         .manual_only = true,
                         .talkers = true})
