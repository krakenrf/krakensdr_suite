// Pulse radar analyser: every pulsed radar in the 2.4 MHz the tuner receives
// (L-band primary surveillance radars - airport / en-route ATC, 1215-1400 MHz -
// and any other pulsed radar the receiver can tune to: UHF wind profilers,
// 1.3 GHz profilers ...). No protocol, no content: what a radar's signal
// says about the radar itself, as an ELINT receiver would measure it.
//
//   pulses   pulse.cpp: |x|^2 against the noise floor, leading edge, width,
//            level, clipping, centre frequency, linear FM (chirp) rate
//   modes    every kind of pulse (frequency, width, chirp) is a mode
//   radars   a mode whose pulse times have a PRI structure (lib/radar_pri.cpp) is a
//            radar; another mode whose pulses come at a FIXED delay after
//            its pulses, at comparable power, belongs to the same radar
//            (frequency diversity, a short fill pulse + a long chirp). What
//            is the radar's own signal measured otherwise is folded into it:
//            pieces of its weak chirps (fading across the sweep - placed in
//            the chirp by their frequency), pulses merged with their echo
//            (on its PRI grid), echoes 10 dB down at a fixed delay, ghosts
//            20 dB down with its PRIs (an image from outside the band) - and
//            a weaker copy of a pulse soon after it is an echo, dropped
//   scan     lib/radar_scan.cpp: the pulse level over time - the main beam sweeping
//            past once per turn - gives the scan period (rpm) and, from the
//            level profile of each unclipped pass, the -3 dB beam width
//   DoA      each radar is a talker (ID separation by its signature): the
//            unclipped pulses of its main-beam passes (within doa_db of the
//            scan's peak, in a burst that stays near the peak as long as the
//            main beam's passes do) go to kraken_doa as packets of exactly
//            their samples with their own channel (freq_hz / bw_hz), so
//            every radar in the band gets its own bearing on the 🗺 Map's 📡
//            DF panel - a reflector the main beam lights elsewhere in the
//            turn (10-20 dB down, shorter near the peak) stays out of it
//
// Outputs: a table row per radar (+ details), facts for the strongest one,
// events (new radar, its scan period, a new PRI set, clipping, lost), a plot
// of the strongest radar's pulse level over one turn of its antenna.

#include "kraken_plugin.hpp"
#include "radar_pri.hpp"
#include "pulse.hpp"
#include "radar_scan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace radar;

constexpr double MODE_KEEP_S = 8;        // pulses kept per mode (PRI analysis, links)
constexpr double MODE_IDLE_S = 15;       // a mode without a radar and without pulses this long goes
constexpr size_t MAX_MODES = 64;
constexpr size_t MAX_RADARS = 32;
constexpr double PRI_WIN_S = 2.0;        // PRI analysis window
constexpr double DOA_HOLD_S = 0.3;       // a pulse waits this long before it is judged main beam or not
constexpr double DOA_MIN_DB = 15;        // ... and needs this much SNR
constexpr double DOA_RATE = 60;          // packets per second per radar, at most
constexpr double LOST_S = 60;
constexpr double LFM_SPAN_HZ = 300e3;    // a sweep this wide over the pulse = linear FM
constexpr size_t FEAT_N = 256;           // pulses a mode's shape (medians) is taken from

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}
double median(std::vector<float> v) {
    if (v.empty()) return NAN;
    auto mid = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}
std::string us_text(double w) { return w < 9.95 ? fmt("%.1f µs", w) : fmt("%.0f µs", w); }

struct Rec {
    double toa, t_a, t_b;
    float db, dbfs, freq, width, chirp, bw;
    bool clipped;
    bool piece = false;   // a piece of a weak chirp: toa = the chirp's start worked out from the piece's frequency
};

// one kind of pulse
struct Mode {
    int id = 0;
    double freq = 0, width = 0, chirp = NAN, span = 0;   // medians
    bool lfm = false;
    std::deque<Rec> recs;
    std::vector<float> hf, hw, hc;                       // last FEAT_N features
    size_t hpos = 0;
    uint64_t n = 0, pieces = 0, echoes = 0;
    double first_t = 0, last_t = 0;
    int radar = -1;
    double delay_us = 0;                                 // after the radar's timing mode's pulse (linked)
    bool alias = false;                                  // fragments of another mode's pulses
    int since = 0;
};

struct SetSeen {
    std::vector<double> set;
    double last_t = 0;
    int n = 0;
};

struct Radar {
    std::string id, label;
    std::vector<int> modes;   // mode ids; [0] = the timing mode
    PriInfo pri;
    std::vector<SetSeen> sets;
    LevelSeries lv;
    std::deque<double> beams;                  // main-beam passes (times)
    std::deque<float> beam_db;                 // ... their strongest pulse (dB over the noise)
    std::deque<double> beam_w3, beam_pk;       // ... the lobe's -3 dB width (s) and peak from the fit (NAN = none)
    double mb_db = NAN;                        // the main beam's level (a pass >= 10 dB over the radar's median level)
    double mb_dwell = 0;                       // how long a main-beam pass stays within 6 dB of the peak (s, median)
    double scan_s = 0, scan_told = 0;
    int scan_support = 0;
    bool beam_clipped = false, clip_told = false;
    float peak_db = -1e9f, peak_dbfs = -1e9f;  // strongest pulse seen (clipped ones included)
    std::deque<Rec> cand;                      // DoA candidates waiting DOA_HOLD_S
    std::deque<std::pair<double, std::pair<float, bool>>> recent;   // timing mode: (toa, (dB, clipped)), 3 s
    double tokens = 0, tok_t = 0;
    uint64_t pulses = 0, packets = 0;
    double first_t = 0, last_t = 0, last_packet_t = -1e9;
    std::deque<double> rate_t;                 // pulse times of the last 2 s (rate)
    bool announced = false, lost = false;
    double created_t = 0;
};

class RadarDecoder : public kp::Decoder {
public:
    explicit RadarDecoder(kp::Host& h) : Decoder(h) {
        host.table_columns({"Radar", "MHz", "Pulse", "PRF Hz", "PRI", "Scan s", "RPM", "Beam °", "Peak dBFS", "SNR dB",
                            "Pulses/s", "Main beam"});
        host.table_keep(keep_s_);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "sens") {
            sens_db_ = std::clamp(v.empty() ? 10.0 : atof(v.c_str()), 6.0, 25.0);
            det_.set_threshold_db(sens_db_);
        } else if (k == "doa") {
            doa_ = v != "0";
        } else if (k == "doa_db") {
            doa_db_ = std::clamp(v.empty() ? 15.0 : atof(v.c_str()), 3.0, 30.0);
        } else if (k == "doa_avg") {
            avg_s_ = std::clamp(v.empty() ? 30.0 : atof(v.c_str()), 3.0, 3600.0);
        } else if (k == "keep") {
            keep_s_ = std::clamp(v.empty() ? 3600.0 : atof(v.c_str()), 60.0, 7 * 86400.0);
            host.table_keep(keep_s_);
        }
    }

    void reset() override {
        for (auto& r : radars_) host.table_remove(r->id);
        radars_.clear();
        modes_.clear();
        det_.reset();
        det_.set_threshold_db(sens_db_);
        pulses_total_ = 0;
        clipped_total_ = 0;
        next_mode_ = 1;
        fast_t_ = slow_t_ = valid_t_ = plot_t_ = -1;
        rate_t_.clear();
    }

    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        det_.push(x, n, t0, pulses_);
        for (const Pulse& p : pulses_) {
            const int mid = on_pulse(p);
            if (dump_)
                host.log(fmt("PDW %.7f", p.toa) + fmt(" %.2f", p.width_us) + fmt(" %.1f", p.peak_db) + fmt(" %.0f", p.freq_hz) +
                         fmt(" %.2f", p.chirp) + (p.clipped ? " c" : " -") + fmt(" m%.0f", mid));
        }
        pulses_.clear();
        const double now = t0 + static_cast<double>(n) / FS;
        if (fast_t_ < 0) fast_t_ = slow_t_ = valid_t_ = plot_t_ = start_t_ = t0;
        if (now - fast_t_ >= 0.05) {
            fast_t_ = now;
            tick_fast(now);
        }
        if (now - slow_t_ >= 1.0) {
            slow_t_ = now;
            tick_slow(now);
        }
    }

private:
    Detector det_;
    std::vector<Pulse> pulses_;
    std::vector<std::unique_ptr<Mode>> modes_;
    std::vector<std::unique_ptr<Radar>> radars_;
    int next_mode_ = 1;
    double sens_db_ = 10, doa_db_ = 15, avg_s_ = 30, keep_s_ = 3600;
    bool doa_ = true;
    const bool dump_ = getenv("RADAR_DUMP") != nullptr;   // offline debugging: every pulse to stderr
    uint64_t pulses_total_ = 0, clipped_total_ = 0;
    std::deque<double> rate_t_;
    double fast_t_ = -1, slow_t_ = -1, valid_t_ = -1, plot_t_ = -1, start_t_ = 0;
    bool packet_since_valid_ = false;

    // --- frequencies --------------------------------------------------------
    double rf_of(double f) const {   // RF of an offset in the input band (NAN = unknown)
        const double rf = host.rf_hz();
        if (!std::isfinite(rf)) return NAN;
        return host.rf_inverted() ? rf - f : rf + f;
    }
    std::string mhz(double f, int dec = 3) const {
        const double r = rf_of(f);
        char b[48];
        if (std::isfinite(r)) snprintf(b, sizeof b, "%.*f", dec, r / 1e6);
        else snprintf(b, sizeof b, "%+.*f (offset)", dec, f / 1e6);
        return b;
    }

    Mode* mode_by_id(int id) {
        for (auto& m : modes_)
            if (m->id == id) return m.get();
        return nullptr;
    }

    // --- pulses -> modes -------------------------------------------------------
    static bool matches(const Mode& m, const Pulse& p, double* score) {
        const double W = m.width;
        const double tw_lo = std::max(1.0, 0.35 * W), tw_hi = std::max(1.0, 0.2 * W);
        if (p.width_us < W - tw_lo || p.width_us > W + tw_hi) return false;
        const double tf = std::max({60e3, 0.25 * std::fabs(m.span), 0.25e6 / std::max(W, 0.3)});
        const double df = std::fabs(p.freq_hz - m.freq);
        if (df > tf) return false;
        if (m.lfm) {
            if (!std::isfinite(p.chirp) || p.chirp * m.chirp <= 0 || std::fabs(p.chirp - m.chirp) > 0.3 * std::fabs(m.chirp))
                return false;
        } else if (std::isfinite(p.chirp) && std::fabs(p.span_hz) > 2 * LFM_SPAN_HZ) {
            return false;
        }
        *score = df / tf + std::fabs(p.width_us - W) / std::max(tw_lo, tw_hi);
        return true;
    }

    // A piece of a chirp of LFM mode m (a weak chirp fades across its sweep
    // and is seen in pieces): the same sweep rate, a shorter pulse whose
    // frequencies lie inside m's sweep. Its frequency says where in the sweep
    // it is: *start = Host::time() where the whole chirp began
    static bool piece_of(const Mode& m, const Pulse& p, double* start) {
        if (!m.lfm || !std::isfinite(p.chirp) || p.chirp * m.chirp <= 0 || std::fabs(p.chirp - m.chirp) > 0.35 * std::fabs(m.chirp))
            return false;
        if (p.width_us > 0.9 * m.width) return false;
        const double half = std::fabs(m.span) / 2, piece_half = std::fabs(p.chirp) * p.width_us / 2;
        if (std::fabs(p.freq_hz - m.freq) + piece_half > half + 300e3) return false;
        // sweep: f(tau) = F - S/2 + k tau; the piece's centre frequency at tau0 + w/2
        double tau0 = (p.freq_hz - m.freq + m.span / 2) / m.chirp - p.width_us / 2;
        tau0 = std::clamp(tau0, 0.0, std::max(0.0, m.width - p.width_us));
        *start = p.toa - tau0 * 1e-6;
        return true;
    }

    void update_medians(Mode& m) {
        m.freq = median(m.hf);
        m.width = median(m.hw);
        if (!m.hc.empty()) m.chirp = median(m.hc);
        m.span = std::isfinite(m.chirp) ? m.chirp * m.width : 0;
        m.lfm = std::fabs(m.span) >= LFM_SPAN_HZ;
    }

    int on_pulse(const Pulse& p) {
        pulses_total_++;
        if (p.clipped) clipped_total_++;
        rate_t_.push_back(p.toa);
        Mode* best = nullptr;
        double bs = 1e9;
        // a piece of a radar's chirp goes to that radar first
        double start = 0;
        for (auto& m : modes_) {
            double st;
            if (m->radar >= 0 && !m->alias && piece_of(*m, p, &st) && (!best || m->n > best->n)) {
                best = m.get();
                start = st;
            }
        }
        if (best) return on_piece(*best, p, start);
        for (auto& m : modes_) {
            double s;
            if (matches(*m, p, &s) && s < bs) {
                bs = s;
                best = m.get();
            }
        }
        if (!best) {
            if (modes_.size() >= MAX_MODES) {
                // the stalest mode that isn't a radar's makes room
                auto it = modes_.end();
                for (auto i = modes_.begin(); i != modes_.end(); ++i)
                    if ((*i)->radar < 0 && (it == modes_.end() || (*i)->last_t < (*it)->last_t)) it = i;
                if (it == modes_.end()) return -1;
                modes_.erase(it);
            }
            auto m = std::make_unique<Mode>();
            m->id = next_mode_++;
            m->freq = p.freq_hz;
            m->width = p.width_us;
            m->chirp = p.chirp;
            m->span = p.span_hz;
            m->lfm = std::isfinite(p.chirp) && std::fabs(p.span_hz) >= LFM_SPAN_HZ;
            m->first_t = p.toa;
            best = m.get();
            modes_.push_back(std::move(m));
        }
        Mode& m = *best;
        Rec r{p.toa, p.t_a, p.t_b, static_cast<float>(p.peak_db), static_cast<float>(p.peak_dbfs), static_cast<float>(p.freq_hz),
              static_cast<float>(p.width_us), static_cast<float>(p.chirp), static_cast<float>(p.bw_hz), p.clipped};
        // an echo: the same pulse again, weaker, soon after (a reflection -
        // terrain, buildings - arriving later than the direct pulse). Not a
        // pulse of the radar's train, and its direction is the reflector's
        if (!m.recs.empty()) {
            const Rec& q = m.recs.back();
            const double lim = m.radar >= 0 && radars_[static_cast<size_t>(m.radar)]->pri.ok
                                   ? 0.3 * radars_[static_cast<size_t>(m.radar)]->pri.mean_pri * 1e-6
                                   : 300e-6;
            if (p.toa - q.toa < lim && r.db < q.db - 3) {
                m.echoes++;
                return m.id;
            }
        }
        m.recs.push_back(r);
        m.n++;
        m.last_t = p.toa;
        // features for the medians (clipped pulses' frequency / chirp are bent: not used)
        if (!p.clipped) {
            if (m.hf.size() < FEAT_N) {
                m.hf.push_back(r.freq);
                m.hw.push_back(r.width);
            } else {
                m.hf[m.hpos] = r.freq;
                m.hw[m.hpos] = r.width;
                m.hpos = (m.hpos + 1) % FEAT_N;
            }
            if (std::isfinite(p.chirp)) {
                if (m.hc.size() < FEAT_N) m.hc.push_back(r.chirp);
                else m.hc[m.n % FEAT_N] = r.chirp;
            }
            if (m.hf.size() < 16 || ++m.since >= 32) {
                m.since = 0;
                update_medians(m);
            }
        }
        if (m.radar >= 0 && m.radar < static_cast<int>(radars_.size())) radar_pulse(*radars_[static_cast<size_t>(m.radar)], m, r);
        return m.id;
    }

    // a piece of mode m's chirp: one pulse of m per chirp (more pieces of the
    // same chirp only count for its level); not used for the PRI or the
    // mode's shape. Its own samples are the radar's signal: a DoA candidate
    int on_piece(Mode& m, const Pulse& p, double start) {
        Rec r{start, p.t_a, p.t_b, static_cast<float>(p.peak_db), static_cast<float>(p.peak_dbfs), static_cast<float>(p.freq_hz),
              static_cast<float>(p.width_us), static_cast<float>(p.chirp), static_cast<float>(p.bw_hz), p.clipped, true};
        m.pieces++;
        const bool same_chirp = !m.recs.empty() && std::fabs(m.recs.back().toa - start) < 0.5e-6 * m.width;
        if (!same_chirp) {
            m.recs.push_back(r);
            m.last_t = start;
        }
        radar_pulse(*radars_[static_cast<size_t>(m.radar)], m, r);
        return m.id;
    }

    void radar_pulse(Radar& R, const Mode& m, const Rec& r) {
        R.pulses++;
        R.last_t = r.toa;
        R.rate_t.push_back(r.toa);
        R.lv.add(r.toa, r.db, r.clipped);
        R.peak_db = std::max(R.peak_db, r.db);
        R.peak_dbfs = std::max(R.peak_dbfs, r.dbfs);
        if (!R.modes.empty() && m.id == R.modes[0] && !r.piece) R.recent.push_back({r.toa, {r.db, r.clipped}});
        // DoA: whole pulses of the radar's own kinds (not pieces of weak
        // chirps, not alias modes - fragments / echoes of its pulses)
        if (doa_ && !r.clipped && !r.piece && !m.alias && r.db >= DOA_MIN_DB) {
            R.cand.push_back(r);
            if (R.cand.size() > 4000) R.cand.pop_front();
        }
        if (R.lost) {
            R.lost = false;
            host.event("📡 Radar " + R.id + " heard again", 5);
        }
    }

    // --- every 50 ms: main-beam pulses -> DoA packets ---------------------------
    void tick_fast(double now) {
        for (auto& rp : radars_) {
            Radar& R = *rp;
            if (R.cand.empty() || R.cand.front().toa >= now - DOA_HOLD_S) continue;
            float ref15 = R.lv.max_over(now - 15, now);
            if (std::isfinite(R.mb_db)) ref15 = std::max(ref15, static_cast<float>(R.mb_db));
            // only once the main beam has passed twice (the scan's peak known:
            // in the first turn a sidelobe / reflection burst may come before
            // it) - or after 30 s, for a radar seen one way only (not rotating)
            const bool gate = R.beams.size() >= 2 || now - R.created_t > 30;
            while (!R.cand.empty() && R.cand.front().toa < now - DOA_HOLD_S) {
                const Rec c = R.cand.front();
                R.cand.pop_front();
                if (!gate) continue;
                // within doa_db of the scan's peak AND in a main-beam pass (the
                // level around it near the scan's peak, for about as long as
                // the main beam's passes stay there): a reflector lit by the
                // main beam elsewhere in the turn can be only ~10 dB under the
                // main beam, but its burst is no main beam
                const float local = R.lv.max_over(c.toa - 0.15, c.toa + 0.15);
                const float ref = std::max(ref15, local);
                if (c.db < ref - doa_db_ || local < ref - 6) continue;
                if (R.mb_dwell > 0 && R.lv.dwell_over(c.toa - 0.3, c.toa + 0.3, ref - 6) < 0.5 * R.mb_dwell) continue;
                R.tokens = std::min(DOA_RATE * 0.5, R.tokens + (c.toa - R.tok_t) * DOA_RATE);
                R.tok_t = c.toa;
                if (R.tokens < 1) continue;
                R.tokens -= 1;
                kp::Talker t;
                t.id = R.id;
                t.label = R.label;
                t.packet = true;
                t.start_s = c.t_a;
                t.end_s = c.t_b;
                t.freq_hz = c.freq;
                t.bw_hz = c.bw;
                t.avg_s = avg_s_;
                t.keep_s = keep_s_;
                host.talker(t);
                host.valid(c.t_a, c.t_b);
                R.packets++;
                R.last_packet_t = c.toa;
                packet_since_valid_ = true;
            }
        }
        // "receiving" while radars are heard (the main-beam pulses above count already)
        if (now - valid_t_ >= 0.25) {
            if (!packet_since_valid_)
                for (auto& rp : radars_)
                    if (now - rp->last_t < 0.5 && rp->announced) {
                        host.valid();
                        break;
                    }
            packet_since_valid_ = false;
            valid_t_ = now;
        }
    }

    // --- every second: radars, PRI, scan, outputs ------------------------------
    void tick_slow(double now) {
        while (!rate_t_.empty() && rate_t_.front() < now - 2) rate_t_.pop_front();
        // modes: forget old pulses, drop idle modes that are no radar's
        for (auto& m : modes_)
            while (!m->recs.empty() && m->recs.front().toa < now - MODE_KEEP_S) m->recs.pop_front();
        modes_.erase(std::remove_if(modes_.begin(), modes_.end(),
                                    [&](const std::unique_ptr<Mode>& m) { return m->radar < 0 && now - m->last_t > MODE_IDLE_S; }),
                     modes_.end());
        for (int k = 0; k < 8 && merge_modes(); k++) {}
        // new radars / links
        for (auto& mp : modes_) {
            Mode& m = *mp;
            if (m.radar >= 0) continue;
            int recent = 0;
            for (auto it = m.recs.rbegin(); it != m.recs.rend() && it->toa > now - 4; ++it) recent++;
            if (recent < 20) continue;
            if (try_link(m)) continue;
            std::vector<double> toas;
            for (const Rec& r : m.recs)
                if (r.toa > now - 3) toas.push_back(r.toa);
            const PriInfo pi = analyze_pri(toas);
            if (!pi.ok || (pi.kind == "mixed" && pi.coverage < 0.8)) continue;
            if (try_alias(m, pi)) continue;
            if (radars_.size() >= MAX_RADARS) continue;
            new_radar(m, pi, now);
        }
        fold_pieces();
        for (size_t i = 0; i < radars_.size(); i++) radar_update(*radars_[i], now);
        if (dump_)
            for (auto& m : modes_) {
                int rc = 0;
                for (auto it = m->recs.rbegin(); it != m->recs.rend() && it->toa > now - 1; ++it) rc++;
                if (rc >= 5)
                    host.log(fmt("MODE %.0f", m->id) + fmt(" n1s %.0f", rc) + fmt(" f %.0f", m->freq) + fmt(" w %.2f", m->width) +
                             fmt(" c %.2f", m->chirp) + (m->lfm ? " lfm" : " -") + fmt(" radar %.0f", m->radar) + (m->alias ? " alias" : ""));
            }
        facts(now);
        if (now - plot_t_ >= 2) {
            plot_t_ = now;
            plot(now);
        }
    }

    // a mode whose pulses keep a fixed delay to a radar's timing pulses is part of that radar
    bool try_link(Mode& m) {
        for (size_t ri = 0; ri < radars_.size(); ri++) {
            Radar& R = *radars_[ri];
            Mode* tm = mode_by_id(R.modes[0]);
            if (!tm || tm->recs.size() < 8) continue;
            std::vector<double> tt;   // the timing mode's whole pulses (pieces' starts are estimates)
            std::vector<float> tl;
            for (const Rec& q : tm->recs)
                if (!q.piece) {
                    tt.push_back(q.toa);
                    tl.push_back(q.db);
                }
            if (tt.size() < 8) continue;
            std::vector<float> dl, dlev;
            for (const Rec& r : m.recs) {
                auto it = std::lower_bound(tt.begin(), tt.end(), r.toa);
                double best = 1e9;
                size_t bi = 0;
                if (it != tt.end()) {
                    best = *it - r.toa;
                    bi = static_cast<size_t>(it - tt.begin());
                }
                if (it != tt.begin() && std::fabs(r.toa - *std::prev(it)) < std::fabs(best)) {
                    best = *std::prev(it) - r.toa;
                    bi = static_cast<size_t>(it - tt.begin()) - 1;
                }
                dl.push_back(static_cast<float>(-best * 1e6));   // m's pulse after the timing pulse: > 0
                dlev.push_back(r.db - tl[bi]);
            }
            if (dl.size() < 10) continue;
            const double md = median(dl);
            int in = 0;
            for (float v : dl)
                if (std::fabs(v - md) <= 1.5) in++;
            const double pri = R.pri.ok ? R.pri.mean_pri : 1000;
            if (in < 0.7 * static_cast<double>(dl.size()) || std::fabs(md) > 0.45 * pri || std::fabs(md) < 0.5) continue;
            // a pulse of the group is sent with comparable power; a pulse
            // 10 dB or more below the timing pulse at a fixed delay is its echo
            if (median(dlev) < -10) {
                m.radar = static_cast<int>(ri);
                m.alias = true;
                m.delay_us = md;
                R.modes.push_back(m.id);
                return true;
            }
            m.radar = static_cast<int>(ri);
            m.delay_us = md;
            R.modes.push_back(m.id);
            host.event("📡 Radar " + R.id + " also sends " + pulse_text(m) + " at " + mhz(m.freq, 2) + " MHz, " +
                           fmt("%.1f µs ", std::fabs(md)) + (md > 0 ? "after" : "before") + " its " + pulse_text(*tm) +
                           " pulses (frequency diversity / pulse group)",
                       60);
            return true;
        }
        return false;
    }

    // the same kind of pulse (full-match tolerances, both ways round)
    static bool same_kind(const Mode& a, const Mode& b) {
        if (a.lfm != b.lfm) return false;
        const double W = a.width;
        if (std::fabs(a.width - b.width) > std::max(1.0, 0.15 * W)) return false;
        const double tf = std::max({60e3, 0.25 * std::fabs(a.span), 0.25e6 / std::max(W, 0.3)});
        if (std::fabs(a.freq - b.freq) > tf) return false;
        return !a.lfm || (a.chirp * b.chirp > 0 && std::fabs(a.chirp - b.chirp) <= 0.3 * std::fabs(a.chirp));
    }

    // Two modes that turned out to be one kind of pulse (the second one made
    // before the first one's shape had settled): the smaller is merged into
    // the larger. A radar's own pulse kinds stay (links / folding decide)
    bool merge_modes() {
        for (size_t i = 0; i < modes_.size(); i++)
            for (size_t j = 0; j < modes_.size(); j++) {
                Mode& a = *modes_[i];
                Mode& b = *modes_[j];
                if (i == j || a.n < b.n) continue;
                if (b.radar >= 0 && (!b.alias || b.radar != a.radar)) continue;
                if (!same_kind(a, b)) continue;
                std::deque<Rec> all;
                std::merge(a.recs.begin(), a.recs.end(), b.recs.begin(), b.recs.end(), std::back_inserter(all),
                           [](const Rec& x, const Rec& y) { return x.toa < y.toa; });
                a.recs.swap(all);
                a.n += b.n;
                a.pieces += b.pieces;
                a.echoes += b.echoes;
                a.last_t = std::max(a.last_t, b.last_t);
                a.first_t = std::min(a.first_t, b.first_t);
                if (b.radar >= 0) {
                    auto& v = radars_[static_cast<size_t>(b.radar)]->modes;
                    v.erase(std::remove(v.begin(), v.end(), b.id), v.end());
                }
                modes_.erase(modes_.begin() + static_cast<long>(j));
                return true;
            }
        return false;
    }

    // A radar made of pieces of another radar's chirps (found first, while
    // only its weak sidelobe pulses came) is that radar: folded into it
    void fold_pieces() {
        for (size_t b = 0; b < radars_.size(); b++) {
            const Mode* tb = mode_by_id(radars_[b]->modes[0]);
            if (!tb) continue;
            for (size_t a = 0; a < radars_.size(); a++) {
                const Mode* ta = mode_by_id(radars_[a]->modes[0]);
                if (a == b || !ta) continue;
                // B is A's when: its pulses are pieces of A's chirps, or come on
                // A's PRI grid, or it turns with A's antenna on A's frequency
                // (a reflector lit by A's beam)
                const Radar& RA = *radars_[a];
                const Radar& RB = *radars_[b];
                const bool same_scan = RA.scan_s > 0 && RB.scan_s > 0 && std::fabs(RA.scan_s - RB.scan_s) < 0.005 * RA.scan_s &&
                                       std::fabs(tb->freq - ta->freq) <= std::max(std::fabs(ta->span) / 2, 300e3) + 150e3;
                if (!mode_piece_of(*ta, *tb) && !(ta->n > tb->n && (same_scan || belongs(*tb, RB.pri, RA)))) continue;
                Radar& A = *radars_[a];
                for (int id : radars_[b]->modes) {
                    Mode* m = mode_by_id(id);
                    if (!m) continue;
                    m->radar = static_cast<int>(a);
                    m->alias = true;
                    m->delay_us = NAN;
                    A.modes.push_back(id);
                }
                host.table_remove(radars_[b]->id);
                host.talker_forget(radars_[b]->id);
                if (dump_) host.log("folded radar " + radars_[b]->id + " into " + A.id);
                radars_.erase(radars_.begin() + static_cast<long>(b));
                for (auto& m : modes_)
                    if (m->radar > static_cast<int>(b)) m->radar--;
                return;   // one per second is plenty
            }
        }
    }

    // every PRI a radar uses (all its sets)
    static std::vector<double> levels_of(const Radar& R) {
        std::vector<double> L = R.pri.ok ? R.pri.set() : std::vector<double>{};
        for (const SetSeen& ss : R.sets) L.insert(L.end(), ss.set.begin(), ss.set.end());
        return L;
    }

    // Share of mode m's recent pulses that come one PRI (or two) of radar R
    // after one of R's own pulses: they ARE R's pulses, measured otherwise -
    // a pulse merged with its echo (longer, its sweep bent), a widened copy
    double on_grid(const Mode& m, const Radar& R) {
        const Mode* tm = mode_by_id(R.modes[0]);
        const std::vector<double> L = levels_of(R);
        if (!tm || L.empty() || tm == &m) return 0;
        std::vector<double> tt;
        for (const Rec& q : tm->recs)
            if (!q.piece) tt.push_back(q.toa);
        int n = 0, hit = 0;
        for (const Rec& r : m.recs) {
            if (r.piece) continue;
            auto it = std::lower_bound(tt.begin(), tt.end(), r.toa - 20e-6);
            if (it == tt.begin()) continue;
            const double d = (r.toa - *std::prev(it)) * 1e6;
            n++;
            bool h = false;
            for (size_t i = 0; i < L.size() && !h; i++) {
                if (std::fabs(d - L[i]) <= 3) h = true;
                for (size_t j = i; j < L.size() && !h; j++)
                    if (std::fabs(d - L[i] - L[j]) <= 4) h = true;
            }
            if (h) hit++;
        }
        return n >= 10 ? static_cast<double>(hit) / n : 0;
    }

    // mode `small` is made of pieces of LFM mode `big`'s chirps (the same
    // sweep rate, shorter, its sweep inside big's)
    static bool mode_piece_of(const Mode& big, const Mode& small) {
        if (!big.lfm || !std::isfinite(small.chirp) || small.chirp * big.chirp <= 0 ||
            std::fabs(small.chirp - big.chirp) > 0.35 * std::fabs(big.chirp) || small.width > 0.9 * big.width)
            return false;
        return std::fabs(small.freq - big.freq) + std::fabs(small.chirp) * small.width / 2 <= std::fabs(big.span) / 2 + 300e3;
    }

    // Mode m (with PRI analysis pi) is radar R's own pulses seen otherwise -
    // not another radar: pieces of R's chirps, pulses on R's PRI grid, or a
    // train with R's PRIs (or, jittered, R's mean PRI) on R's frequency; or a
    // ghost of R - most of its PRIs R's and 20 dB or more under R (an echo, an
    // image of R from outside the band)
    bool belongs(const Mode& m, const PriInfo& pi, const Radar& R) {
        const Mode* tm = mode_by_id(R.modes[0]);
        if (!tm || tm == &m) return false;
        if (mode_piece_of(*tm, m) || on_grid(m, R) >= 0.6) return true;
        const std::vector<double> L = levels_of(R);
        if (L.empty() || !pi.ok) return false;
        auto match = [&](double l) {
            for (size_t a = 0; a < L.size(); a++) {
                if (std::fabs(l - L[a]) <= 3) return true;
                for (size_t b = a; b < L.size(); b++)
                    if (std::fabs(l - L[a] - L[b]) <= 4) return true;
            }
            return false;
        };
        const std::vector<double> s = pi.kind == "jitter" ? std::vector<double>{pi.mean_pri} : pi.set();
        size_t nm = 0;
        for (double l : s) nm += match(l);
        const bool near = std::fabs(m.freq - tm->freq) <= std::max(std::fabs(tm->span) / 2, 300e3) + 150e3;
        if (near && !s.empty() && (nm == s.size() || (pi.kind == "jitter" && R.pri.ok &&
                                                       std::fabs(pi.mean_pri - R.pri.mean_pri) < 0.15 * R.pri.mean_pri)))
            return true;
        std::vector<float> lv;
        for (const Rec& r : m.recs) lv.push_back(r.db);
        const bool weaker = !lv.empty() && median(lv) < R.peak_db - 20;
        if (weaker && pi.kind == "jitter" && R.pri.ok && std::fabs(pi.mean_pri - R.pri.mean_pri) < 0.15 * R.pri.mean_pri) return true;
        return weaker && 2 * nm >= s.size();
    }

    bool try_alias(Mode& m, const PriInfo& pi) {
        for (size_t ri = 0; ri < radars_.size(); ri++) {
            Radar& R = *radars_[ri];
            if (!belongs(m, pi, R)) continue;
            m.radar = static_cast<int>(ri);
            m.alias = true;
            m.delay_us = NAN;
            R.modes.push_back(m.id);
            return true;
        }
        return false;
    }

    void new_radar(Mode& m, const PriInfo& pi, double now) {
        auto R = std::make_unique<Radar>();
        // the id: its frequency to 0.1 MHz - a chirp's measured centre moves
        // by tens of kHz with fading, and the id must be the same after a
        // restart (its DF history); radars closer together get "#2"...
        std::string base = mhz(m.freq, 1) + " MHz";
        std::string id = base;
        for (int k = 2;; k++) {
            bool taken = false;
            for (auto& o : radars_) taken = taken || o->id == id;
            if (!taken) break;
            id = base + " #" + std::to_string(k);
        }
        R->id = id.substr(0, 32);
        R->modes.push_back(m.id);
        R->pri = pi;
        R->first_t = m.first_t;
        R->created_t = now;
        if (!pi.straddles && pi.kind != "jitter") R->sets.push_back({pi.set(), now, 1});
        m.radar = static_cast<int>(radars_.size());
        radars_.push_back(std::move(R));
        Radar& r = *radars_.back();
        // the mode's pulses so far count for the radar (levels, scan)
        for (const Rec& rec : m.recs) {
            r.pulses++;
            r.lv.add(rec.toa, rec.db, rec.clipped);
            r.peak_db = std::max(r.peak_db, rec.db);
            r.peak_dbfs = std::max(r.peak_dbfs, rec.dbfs);
            r.recent.push_back({rec.toa, {rec.db, rec.clipped}});
            r.rate_t.push_back(rec.toa);
            r.last_t = rec.toa;
        }
    }

    // --- per radar, every second --------------------------------------------------
    void radar_update(Radar& R, double now) {
        while (!R.rate_t.empty() && R.rate_t.front() < now - 2) R.rate_t.pop_front();
        while (!R.recent.empty() && R.recent.front().first < now - 3) R.recent.pop_front();
        Mode* tm = mode_by_id(R.modes[0]);
        // PRI: the timing mode's last PRI_WIN_S (longer when it is sparse)
        if (tm) {
            std::vector<double> toas;
            for (const Rec& r : tm->recs)
                if (r.toa > now - PRI_WIN_S && !r.piece) toas.push_back(r.toa);
            if (toas.size() < 13) {
                toas.clear();
                for (const Rec& r : tm->recs)
                    if (r.toa > now - 6 && !r.piece) toas.push_back(r.toa);
            }
            const PriInfo pi = analyze_pri(toas);
            if (pi.ok && !pi.straddles && (pi.kind != "mixed" || pi.coverage >= 0.8)) {
                R.pri = pi;
            }
            // PRI sets the radar switches between (a jittered PRI has none)
            if (pi.ok && !pi.straddles && pi.kind != "jitter" && (pi.kind != "mixed" || pi.coverage >= 0.8)) {
                const std::vector<double> s = pi.set();
                bool found = false;
                for (SetSeen& ss : R.sets)
                    if (same_set(ss.set, s)) {
                        ss.last_t = now;
                        ss.n++;
                        found = true;
                    }
                if (!found) {
                    R.sets.push_back({s, now, 1});
                    if (R.announced && R.sets.size() > 1)
                        host.event("📡 Radar " + R.id + ": PRI set " + set_text(s) + " (also " + other_sets(R, s) + ")", 600);
                }
                // sets not seen for 2 min go
                R.sets.erase(std::remove_if(R.sets.begin(), R.sets.end(), [&](const SetSeen& ss) { return now - ss.last_t > 120; }),
                             R.sets.end());
            }
        }
        scan_update(R, now);
        R.label = label(R);
        if (!R.announced && R.pri.ok && now - R.created_t >= 3) {
            R.announced = true;
            host.event("📡 New radar " + R.id + ": " + pulses_text(R) + ", PRF " + fmt("%.0f Hz", 1e6 / R.pri.mean_pri) + " (PRI " +
                           R.pri.text() + ")" + band_note(R),
                       3600);
        }
        if (!R.lost && R.announced && now - R.last_t > LOST_S) {
            R.lost = true;
            host.event("📡 Radar " + R.id + " not heard for " + fmt("%.0f s", LOST_S), 5);
        }
        if (R.announced && now - R.last_t < keep_s_) row(R, now);
    }

    void scan_update(Radar& R, double now) {
        // main-beam passes: bursts within 6 dB of the strongest of the last 41 s
        const std::vector<Burst> bs = R.lv.bursts(6, 0.2);
        // ... that stay up there about as long as the longest one: a reflector
        // lit by the main beam elsewhere in the turn is up there much shorter
        // (when the main beam clips, its level is no longer higher than theirs)
        double maxdwell = 0;
        for (const Burst& b : bs) maxdwell = std::max(maxdwell, b.dwell);
        for (const Burst& b : bs) {
            if (b.open || b.dwell < 0.5 * maxdwell) continue;
            bool known = false;
            for (double t : R.beams) known = known || std::fabs(t - b.t) < 0.4;
            if (known) continue;
            // the main lobe's profile: the timing mode's unclipped pulses from
            // the top of the pass (or its clipped part) outwards while within
            // 12 dB of it - not the sidelobes beyond the lobe's first nulls
            std::vector<std::pair<double, float>> pts;
            const auto& rc = R.recent;
            float top = -1e9f;   // the timing pulses' own top (the bins also hold the radar's other pulse kinds)
            for (const auto& [t, v] : rc)
                if (t >= b.from - 0.05 && t <= b.to + 0.05) top = std::max(top, v.first);
            long c_lo = -1, c_hi = -1;
            for (long i = 0; i < static_cast<long>(rc.size()); i++) {
                const auto& [t, v] = rc[static_cast<size_t>(i)];
                if (t < b.from - 0.05 || t > b.to + 0.05) continue;
                if (v.second || v.first >= top - 1) {
                    if (c_lo < 0) c_lo = i;
                    c_hi = i;
                }
            }
            if (c_lo >= 0) {
                for (long i = c_lo; i <= c_hi; i++)
                    if (!rc[static_cast<size_t>(i)].second.second) pts.emplace_back(rc[static_cast<size_t>(i)].first, rc[static_cast<size_t>(i)].second.first);
                for (int dir = -1; dir <= 1; dir += 2) {
                    int below = 0;
                    for (long i = dir < 0 ? c_lo - 1 : c_hi + 1; i >= 0 && i < static_cast<long>(rc.size()); i += dir) {
                        const auto& [t, v] = rc[static_cast<size_t>(i)];
                        if (std::fabs(t - b.t) > 0.3) break;
                        if (v.second) continue;
                        if (v.first < top - (b.clipped ? 12 : 10)) {
                            if (++below >= 2) break;
                            continue;
                        }
                        below = 0;
                        pts.emplace_back(t, v.first);
                    }
                }
                std::sort(pts.begin(), pts.end());
            }
            // (a clipped top hides the lobe's peak: its width can't be measured)
            double t0 = b.t, pk = b.peak_db, w = 0;
            const bool fit = !b.clipped && fit_lobe(pts, &t0, &pk, &w);
            if (dump_)
                host.log(R.id + fmt(" burst t=%.3f", b.t) + fmt(" peak %.1f", b.peak_db) + (b.clipped ? " clipped" : "") +
                         fmt(" pts %.0f", static_cast<double>(pts.size())) + fmt(" top %.1f", top) +
                         (fit ? fmt(" fit t0 %.3f", t0) + fmt(" pk %.1f", pk) + fmt(" w3 %.1f ms", w * 1e3) : " no fit"));
            {
                const double tb = fit ? t0 : b.t;
                size_t at = 0;
                while (at < R.beams.size() && R.beams[at] < tb) at++;
                R.beams.insert(R.beams.begin() + static_cast<long>(at), tb);
                R.beam_db.insert(R.beam_db.begin() + static_cast<long>(at), b.peak_db);
                R.beam_w3.insert(R.beam_w3.begin() + static_cast<long>(at), fit ? w : NAN);
                R.beam_pk.insert(R.beam_pk.begin() + static_cast<long>(at), fit ? pk : NAN);
            }
            R.beam_clipped = b.clipped;
            if (b.clipped && !R.clip_told && R.announced) {
                R.clip_told = true;
                host.event("⚠ Radar " + R.id + ": its main beam clips the receiver (8-bit ADC at full scale) - the "
                           "bearing uses the unclipped pulses on the beam's flanks; a lower gain gives more of them",
                           3600);
            }
            if (host.raw_wanted())
                host.raw(R.id + " main beam t=" + fmt("%.3f", fit ? t0 : b.t) + fmt(" peak %.1f dB", b.peak_db) +
                         (b.clipped ? " clipped" : "") + (fit ? fmt(" width3 %.1f ms", w * 1e3) : "") + " PRI " + R.pri.text());
        }
        while (!R.beams.empty() && R.beams.front() < now - 600) {
            R.beams.pop_front();
            R.beam_db.pop_front();
            R.beam_w3.pop_front();
            R.beam_pk.pop_front();
        }
        // passes taken while only weaker bursts had come (the first turn: a
        // sidelobe / reflection burst before the main beam) are no main beams
        const float L = R.lv.max_over(now - 40, now);
        std::vector<float> dw;
        for (size_t i = 0; i < R.beams.size();) {
            const bool recent = R.beams[i] > now - 40;
            const double d = recent ? R.lv.dwell_over(R.beams[i] - 0.3, R.beams[i] + 0.3, L - 6) : 0;
            if (recent && (R.beam_db[i] < L - 6 || d < 0.5 * maxdwell)) {
                R.beams.erase(R.beams.begin() + static_cast<long>(i));
                R.beam_db.erase(R.beam_db.begin() + static_cast<long>(i));
                R.beam_w3.erase(R.beam_w3.begin() + static_cast<long>(i));
                R.beam_pk.erase(R.beam_pk.begin() + static_cast<long>(i));
            } else {
                if (recent) dw.push_back(static_cast<float>(d));
                i++;
            }
        }
        R.mb_dwell = dw.empty() ? 0 : median(dw);
        // a main beam stands out of the radar's usual level (its sidelobes)
        const float med = R.lv.median_level();
        R.mb_db = NAN;
        for (size_t i = 0; i < R.beams.size(); i++)
            if (R.beams[i] > now - 40 && R.beam_db[i] >= med + 10) R.mb_db = std::isfinite(R.mb_db) ? std::max<double>(R.mb_db, R.beam_db[i]) : R.beam_db[i];
        int sup = 0;
        const double P = scan_period(R.beams, &sup);
        if (P > 0) {
            R.scan_s = P;
            R.scan_support = sup;
            if (!(R.scan_told > 0)) {
                R.scan_told = P;
                if (R.announced)
                    host.event("📡 Radar " + R.id + ": antenna scan " + fmt("%.2f s", P) + fmt(" (%.1f rpm)", 60 / P) +
                                   (beam_deg(R) > 0 ? fmt(", beam %.1f°", beam_deg(R)) : ""),
                               600);
            } else if (std::fabs(P - R.scan_told) > 0.03 * R.scan_told) {
                host.event("📡 Radar " + R.id + ": scan period changed " + fmt("%.2f", R.scan_told) + fmt(" → %.2f s", P), 60);
                R.scan_told = P;
            }
        }
    }

    // --- texts ----------------------------------------------------------------
    static std::string pulse_text(const Mode& m) {
        std::string t = us_text(m.width);
        if (m.lfm) t += fmt(" LFM %+.2f MHz", m.span / 1e6);
        return t;
    }
    std::string pulses_text(const Radar& R) {
        std::string t;
        for (size_t i = 0; i < R.modes.size(); i++) {
            const Mode* m = mode_by_id(R.modes[i]);
            if (!m || m->alias) continue;
            t += (t.empty() ? "" : " + ") + pulse_text(*m);
            if (i) t += fmt(" (%+.0f µs)", m->delay_us);
        }
        return t;
    }
    std::string other_sets(const Radar& R, const std::vector<double>& s) {
        std::string t;
        for (const SetSeen& ss : R.sets)
            if (!same_set(ss.set, s)) t += (t.empty() ? "" : ", ") + set_text(ss.set);
        return t;
    }
    // median of the last (up to) 9 passes' fitted values (0 = none)
    static double last_fits(const std::deque<double>& d) {
        std::vector<float> v;
        for (size_t i = d.size(); i-- > 0 && v.size() < 9;)
            if (std::isfinite(d[i])) v.push_back(static_cast<float>(d[i]));
        return v.empty() ? 0 : median(v);
    }
    static double beam_w3(const Radar& R) { return last_fits(R.beam_w3); }
    static double beam_deg(const Radar& R) { return R.scan_s > 0 && beam_w3(R) > 0 ? beam_w3(R) / R.scan_s * 360 : 0; }
    std::string band_note(const Radar& R) {
        const Mode* m = mode_by_id(R.modes[0]);
        const double f = m ? rf_of(m->freq) : NAN;
        if (!std::isfinite(f)) return "";
        if (f >= 1215e6 && f <= 1400e6) return " - L-band surveillance radar band";
        if (f >= 420e6 && f <= 450e6) return " - UHF radar band";
        if (f >= 902e6 && f <= 928e6) return " - 915 MHz (wind profilers)";
        return "";
    }
    std::string label(const Radar& R) {
        std::string t;
        if (R.pri.ok) t = fmt("PRF %.0f Hz", 1e6 / R.pri.mean_pri);
        const Mode* m = mode_by_id(R.modes[0]);
        if (m) t += (t.empty() ? "" : " · ") + us_text(m->width) + (m->lfm ? " LFM" : "");
        if (R.scan_s > 0) t += fmt(" · %.2f s", R.scan_s);
        return t.substr(0, 64);
    }
    std::string pri_text(const Radar& R) {
        std::string t = R.pri.text();
        const std::string o = R.pri.ok ? other_sets(R, R.pri.set()) : "";
        if (!o.empty()) t += "; also " + o;
        return t;
    }
    std::string freq_text(const Radar& R) {
        std::string t;
        for (int id : R.modes) {
            const Mode* m = mode_by_id(id);
            if (!m || m->alias) continue;
            t += (t.empty() ? "" : " + ") + mhz(m->freq, 3);
        }
        return t;
    }
    // where the antenna points now, from the last main beam (degrees it turned since)
    double pointing(const Radar& R, double now) const {
        if (!(R.scan_s > 0) || R.beams.empty()) return NAN;
        return std::fmod((now - R.beams.back()) / R.scan_s * 360, 360);
    }

    void row(Radar& R, double now) {
        const double rate = static_cast<double>(R.rate_t.size()) / std::clamp(now - R.first_t, 0.5, 2.0);
        const double bd = beam_deg(R);
        const std::string mb = R.beams.empty() ? "" : fmt("%.1f s ago", now - R.beams.back());
        host.table_row(R.id, {R.id, freq_text(R), pulses_text(R), R.pri.ok ? fmt("%.0f", 1e6 / R.pri.mean_pri) : "",
                              pri_text(R), R.scan_s > 0 ? fmt("%.2f", R.scan_s) : "", R.scan_s > 0 ? fmt("%.1f", 60 / R.scan_s) : "",
                              bd > 0 ? fmt("%.1f", bd) : "", fmt("%.1f", R.peak_dbfs) + (R.beam_clipped ? " (clipped)" : ""),
                              fmt("%.0f", R.peak_db), fmt("%.0f", rate), mb});
        std::vector<std::pair<std::string, std::string>> d;
        int k = 0;
        for (int id : R.modes) {
            const Mode* m = mode_by_id(id);
            if (!m) continue;
            std::string v = us_text(m->width) + " at " + mhz(m->freq, 3) + " MHz";
            if (m->lfm)
                v += fmt(", linear FM %+.2f MHz", m->span / 1e6) + fmt(" (%+.1f kHz/µs, ", m->chirp / 1e3) +
                     (m->span < 0 ? "down)" : "up)");
            else
                v += ", no frequency modulation seen";
            if (k && !m->alias) v += fmt(", %.1f µs ", std::fabs(m->delay_us)) + (m->delay_us > 0 ? "after" : "before") + " pulse 1";
            if (m->alias) v = "weak pieces of pulse 1 (" + v + ")";
            v += fmt(" - %.0f pulses", static_cast<double>(m->n));
            d.push_back({m->alias ? "Fragments" : "Pulse " + std::to_string(++k), v});
        }
        if (R.pri.ok) {
            d.push_back({"PRI", R.pri.text() + fmt(" (%.0f %% of the intervals explained)", 100 * R.pri.coverage)});
            d.push_back({"PRF", fmt("%.1f Hz", 1e6 / R.pri.mean_pri) + fmt(" (mean PRI %.1f µs)", R.pri.mean_pri)});
            const Mode* m0 = mode_by_id(R.modes[0]);
            if (m0) d.push_back({"Duty cycle", fmt("%.2f %%", 100 * m0->width / R.pri.mean_pri)});
        }
        if (R.sets.size() > 1) {
            std::string s;
            for (const SetSeen& ss : R.sets) s += (s.empty() ? "" : "; ") + set_text(ss.set) + fmt(" (%.0f×)", ss.n);
            d.push_back({"PRI sets", s + " - the radar switches between them"});
        }
        if (R.scan_s > 0) {
            d.push_back({"Antenna scan", fmt("%.3f s", R.scan_s) + fmt(" = %.2f rpm", 60 / R.scan_s) +
                                             fmt(" (from %.0f main-beam passes)", R.scan_support)});
            const double pt = pointing(R, now);
            if (std::isfinite(pt)) d.push_back({"Antenna now", fmt("%.0f° past pointing here", pt)});
        } else {
            d.push_back({"Antenna scan", now - R.first_t < 30 ? fmt("measuring (%.0f main-beam passes so far)", R.beams.size())
                                                              : "no regular main beam (not rotating, or only its sidelobes reach us)"});
        }
        if (beam_w3(R) > 0)
            d.push_back({"Main beam", fmt("%.1f ms at -3 dB", beam_w3(R) * 1e3) + (bd > 0 ? fmt(" = %.2f°", bd) : "")});
        else if (R.beam_clipped)
            d.push_back({"Main beam", "clips the receiver: its width and peak can't be measured at this gain"});
        if (last_fits(R.beam_pk) > 0) d.push_back({"Main beam level", fmt("%.1f dB over the noise", last_fits(R.beam_pk))});
        d.push_back({"Strongest pulse", fmt("%.1f dBFS", R.peak_dbfs) + fmt(", %.1f dB over the noise", R.peak_db) +
                                            (R.beam_clipped ? " - clipped" : "")});
        d.push_back({"DoA", doa_ ? fmt("%.0f main-beam pulses reported", static_cast<double>(R.packets)) +
                                      fmt(" (unclipped, within %.0f dB of the scan's peak)", doa_db_)
                                 : "off"});
        d.push_back({"Heard", fmt("%.0f pulses", static_cast<double>(R.pulses)) + fmt(", for %.0f s", now - R.first_t)});
        host.table_detail(R.id, d);
    }

    void facts(double now) {
        host.fact("Pulses", fmt("%.0f /s", static_cast<double>(rate_t_.size()) / std::clamp(now - start_t_, 0.5, 2.0)) +
                                (pulses_total_ ? fmt(" (%.1f %% clipped)", 100.0 * static_cast<double>(clipped_total_) / static_cast<double>(pulses_total_)) : ""));
        if (det_.noise() > 0) host.fact("Noise floor", fmt("%.1f dBFS", 10 * std::log10(det_.noise())));
        if (det_.long_segments()) host.fact("Carriers", fmt("%.0f runs longer than 2 ms (CW / interference, not pulses)", static_cast<double>(det_.long_segments())));
        int n = 0;
        Radar* top = nullptr;
        std::string list;
        for (auto& r : radars_) {
            if (!r->announced || now - r->last_t > keep_s_) continue;
            n++;
            list += (list.empty() ? "" : ", ") + r->id;
            if (!top || r->peak_db > top->peak_db) top = r.get();
        }
        host.fact("Radars", n ? std::to_string(n) + " (" + list + ")" : "none yet");
        if (!top) return;
        host.fact("Radar", top->id + (now - top->last_t > 5 ? " (not heard now)" : ""));
        host.fact("Pulse", pulses_text(*top));
        if (top->pri.ok) {
            host.fact("PRF", fmt("%.1f Hz", 1e6 / top->pri.mean_pri));
            host.fact("PRI", pri_text(*top));
        }
        host.fact("Scan", top->scan_s > 0 ? fmt("%.2f s", top->scan_s) + fmt(" (%.1f rpm)", 60 / top->scan_s) +
                                                (beam_deg(*top) > 0 ? fmt(", beam %.1f°", beam_deg(*top)) : "")
                                          : "measuring");
        host.fact("Level", fmt("%.1f dBFS", top->peak_dbfs) + fmt(" (%.0f dB over the noise)", top->peak_db) +
                               (top->beam_clipped ? " - main beam clips: lower the gain" : ""));
    }

    // the strongest radar's pulse level over one turn of its antenna
    void plot(double now) {
        Radar* top = nullptr;
        for (auto& r : radars_)
            if (r->announced && now - r->last_t < 30 && (!top || r->peak_db > top->peak_db)) top = r.get();
        if (!top) return;
        Radar& R = *top;
        std::vector<std::pair<double, float>> b;
        kp::Plot p;
        p.y_unit = "dB";
        const float ref = R.lv.max_over(now - 15, now);
        p.threshold = ref - doa_db_;
        if (R.scan_s > 0 && !R.beams.empty()) {
            const double P = R.scan_s, tb = R.beams.back();
            R.lv.bins(now - 2 * P, now, b);
            p.title = "Radar " + R.id + ": pulse level over one turn (0° = main beam here)";
            p.x_unit = "°";
            p.x0 = -180;
            p.dx = 2;
            p.y.assign(180, NAN);
            for (auto& [t, v] : b) {
                double az = std::remainder((t - tb) / P * 360, 360);
                const int i = std::clamp(static_cast<int>((az + 180) / 2), 0, 179);
                if (!(p.y[static_cast<size_t>(i)] >= v)) p.y[static_cast<size_t>(i)] = v;
            }
            p.markers.push_back({0, "main beam", R.id});
        } else {
            R.lv.bins(now - 20, now, b);
            p.title = "Radar " + R.id + ": pulse level, last 20 s";
            p.x_unit = "s";
            p.x0 = -20;
            p.dx = 0.1;
            p.y.assign(200, NAN);
            for (auto& [t, v] : b) {
                const int i = std::clamp(static_cast<int>((t - (now - 20)) / 0.1), 0, 199);
                if (!(p.y[static_cast<size_t>(i)] >= v)) p.y[static_cast<size_t>(i)] = v;
            }
        }
        // empty bins: the plot's floor
        float lo = 1e9f;
        for (float v : p.y)
            if (std::isfinite(v)) lo = std::min(lo, v);
        if (lo > 1e8f) return;
        for (float& v : p.y)
            if (!std::isfinite(v)) v = std::max(0.0f, lo - 3);
        p.y_min = 0;
        host.plot(p);
    }
};

}  // namespace

KRAKEN_PLUGIN(RadarDecoder,
              {.id = "radar",
               .name = "Pulse radar (L-band ATC / surveillance)",
               .category = "Aviation",
               .description = "Pulsed radars in the 2.4 MHz the tuner receives - airport and en-route surveillance radars "
                              "(1215-1400 MHz), wind profilers and other pulse radars: each radar's pulses (width, "
                              "frequency, linear FM chirp), PRF and PRI pattern (stagger, blocks, jitter, switching PRI "
                              "sets), antenna scan period and beam width. Tune the receiver to the radar (the VFO sits on "
                              "the tuner's centre with the full bandwidth). Every radar gets its own DoA from its main-beam "
                              "pulses in the 🗺 Map's 📡 DF panel",
               .version = "1.0",
               .sample_rate = 2400000,
               .min_vfo_rate = 2400000,
               .author = "KrakenSDR",
               .options = {{"sens", "Pulse threshold", "10", "8=8 dB|10=10 dB|12=12 dB|15=15 dB|20=20 dB",
                            "How far above the noise a pulse must rise (long pulses: 6 dB less, averaged over 10 µs). "
                            "Lower finds weaker radars and more of their sidelobe pulses"},
                           {"doa", "DoA per radar", "1", "1=On|0=Off",
                            "Report each radar's main-beam pulses for its own bearing on the 🗺 Map (DF panel)"},
                           {"doa_db", "DoA from", "15", "6=Main beam top (6 dB)|10=Main beam (10 dB)|15=Main beam (15 dB)|20=Main beam + near sidelobes (20 dB)",
                            "Pulses within this much of the scan's strongest are used for the bearing (and only "
                            "unclipped ones): the main beam pointing at us comes straight from the radar; weaker pulses "
                            "can be reflections the beam lit elsewhere"},
                           {"doa_avg", "DoA averaging", "30", "10=10 s|30=30 s|120=2 min|600=10 min",
                            "How long a radar's main-beam pulses are averaged into its bearing (a pass every few seconds)"},
                           {"keep", "Keep radars", "3600", "600=10 min|3600=1 hour|86400=24 hours",
                            "How long a radar stays in the table and the DF list after it was last heard"}},
               .manual_only = true,
               .talkers = true,
               .lock_vfo = true})
