#include "rdf_mapper.hpp"

#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "doa_logger.hpp"
#include "globals.hpp"
#include "rdf_engine.hpp"
#include "signal_processing/music_processor.hpp"
#include "station_info.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace rdfmap {

namespace {

const char* const SESSION_FILE = "rdf_session.bin";
constexpr double BASE_SIGMA_DEG = 2.0;     // MUSIC / site multipath spread of one frame
constexpr int LOBE_BINS = 720;             // display lobe: 0.5 deg bins (a MUSIC peak is a few degrees wide)
constexpr size_t MAX_LINES = 150;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

double wrap360(double a) {
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360 : a;
}

std::string b64(const uint8_t* d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += i + 1 < n ? T[(v >> 6) & 63] : '=';
        o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

std::string num(double v, int prec) {
    if (!is_finite_value(v)) return "null";
    char b[40];
    snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

// --- the station's track ----------------------------------------------------
struct Fix {
    int64_t t;          // when it was valid (local clock)
    double lat, lon, hdg, spd;
    bool hvalid, compass, fixed;   // fixed = static location
};

// --- shared state (mu) --------------------------------------------------------
struct Shared {
    std::mutex mu;
    // station
    bool have_fix = false;
    Fix last{};
    std::string state = "starting";       // why frames are (not) used
    // per VFO display + gate + estimate snapshot
    struct V {
        double mhz = 0;
        std::string lobe;                  // b64 of LOBE_BINS u8, "" = none
        double peak = NAN;                 // compass deg
        float conf = 0;
        int64_t lobe_t = 0;
        bool held = false;                 // the lobe's heading is not current (stopped, GPS course)
        int frames = 0;                    // in the open window
        double gate_m = 0, moved_m = 0;
        int records = 0;
        rdf::Estimate est;
        std::string grid;                  // the last {"rdf_grid":..}
        uint64_t grid_ver = 0, sent_ver = 0;
    };
    std::map<int, V> v;
    bool full = true;
};
Shared& SH() {
    static Shared* s = new Shared;   // never destroyed: threads may run while the process exits
    return *s;
}

std::atomic<bool> g_enabled{true};
std::atomic<double> g_range_km{10.0};
std::atomic<bool> g_stop{false};

// --- the engine's work queue ------------------------------------------------
struct Job {
    enum Kind { RECORD, RESET, RANGE, HELLO, REMOVE } kind;
    int vfo = -1;
    double freq = 0;
    rdf::Record rec{};
};
std::mutex q_mu;
std::condition_variable q_cv;
std::deque<Job> q;
void push(Job j) {
    {
        std::lock_guard<std::mutex> lk(q_mu);
        if (q.size() > 2000) q.pop_front();
        q.push_back(std::move(j));
    }
    q_cv.notify_one();
}

std::thread sampler_thr, engine_thr;

// --- sampler -------------------------------------------------------------------
struct SVfo {
    rdf::Gate gate{rdf::GateCfg{}};
    int64_t last_stamp = 0;
    double freq = 0;
};

// position / heading at time t from the track (false: no usable fix then)
bool fix_at(const std::deque<Fix>& tr, int64_t t, Fix* out, double* yaw_dps) {
    if (tr.empty()) return false;
    if (tr.back().fixed) {
        *out = tr.back();
        *yaw_dps = 0;
        return true;
    }
    if (t > tr.back().t + 1500 || t < tr.front().t - 1000) return false;
    size_t i = 0;
    while (i + 1 < tr.size() && tr[i + 1].t <= t) i++;
    const Fix& a = tr[i];
    const Fix& b = i + 1 < tr.size() ? tr[i + 1] : tr[i];
    const double f = b.t > a.t ? std::clamp(static_cast<double>(t - a.t) / (b.t - a.t), 0.0, 1.0) : 0.0;
    *out = a;
    out->t = t;
    out->lat = a.lat + f * (b.lat - a.lat);
    out->lon = a.lon + f * (b.lon - a.lon);
    out->spd = a.spd + f * (b.spd - a.spd);
    out->hdg = wrap360(a.hdg + f * std::remainder(b.hdg - a.hdg, 360.0));
    out->hvalid = a.hvalid && b.hvalid;
    // turn rate over the second around t
    const Fix* p0 = &tr.front();
    const Fix* p1 = &tr.back();
    for (const auto& x : tr) {
        if (x.t <= t - 500) p0 = &x;
        if (x.t >= t + 500) { p1 = &x; break; }
    }
    *yaw_dps = p1->t > p0->t ? std::fabs(std::remainder(p1->hdg - p0->hdg, 360.0)) / ((p1->t - p0->t) / 1000.0) : 0;
    return true;
}

// the live lobe for the map, drawn like the MUSIC DoA plot (PolarPlot): the
// pseudospectrum LINEAR, min..max -> 0..255, in the north frame, 0.5 deg bins
std::string display_lobe(const std::vector<float>& spec, double res, double hdg, double* peak) {
    float mx = 0, mn = spec.empty() ? 0 : spec[0];
    int im = 0;
    for (size_t i = 0; i < spec.size(); i++) {
        if (spec[i] > mx) { mx = spec[i]; im = static_cast<int>(i); }
        mn = std::min(mn, spec[i]);
    }
    if (!(mx > mn)) return "";
    uint8_t u[LOBE_BINS];
    const int n = static_cast<int>(spec.size());
    for (int k = 0; k < LOBE_BINS; k++) {
        const double tau = k * 360.0 / LOBE_BINS;
        const double x = wrap360(hdg - tau) / res;
        int i0 = static_cast<int>(x);
        const double f = x - i0;
        i0 %= n;
        const double v = (spec[i0] * (1 - f) + spec[(i0 + 1) % n] * f - mn) / (mx - mn);
        u[k] = static_cast<uint8_t>(std::lround(255 * std::clamp(v, 0.0, 1.0)));
    }
    // sub-bin peak (parabola) in the array frame -> compass
    const double a = spec[(im + n - 1) % n], b = spec[im], c = spec[(im + 1) % n];
    const double den = a - 2 * b + c;
    const double d = den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    *peak = wrap360(hdg - (im + d) * res);
    return b64(u, LOBE_BINS);
}

void sampler() {
    std::deque<Fix> track;
    std::map<int, SVfo> vs;
    int64_t last_fix_stamp = -1;
    double last_lat = 0, last_lon = 0, last_hdg = -1;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int64_t t = now_ms();
        // the track
        StationLocation sl = station_info.resolve();
        const bool fixed = sl.source == LocationSource::STATIC;
        const bool has = !(sl.lat == 0.0 && sl.lon == 0.0) && is_finite_value(sl.lat) && is_finite_value(sl.lon);
        if (has) {
            const bool fresh = fixed || sl.timestamp_ms != last_fix_stamp || sl.lat != last_lat || sl.lon != last_lon ||
                               sl.heading != last_hdg;
            if (fresh) {
                last_fix_stamp = sl.timestamp_ms;
                last_lat = sl.lat;
                last_lon = sl.lon;
                last_hdg = sl.heading;
                Fix f{fixed ? t : t - static_cast<int64_t>(FIX_LATENCY_S * 1000), sl.lat, sl.lon, sl.heading, sl.speed,
                      sl.heading_valid, sl.from_compass, fixed};
                if (fixed) track.clear();
                track.push_back(f);
            }
        }
        while (!track.empty() && t - track.front().t > 20000) track.pop_front();
        const bool fix_ok = has && !track.empty() && (fixed || t - track.back().t < 3000);

        // why frames are (not) used now
        const bool coherent = !multi_tuner_mode();
        std::string state;
        if (!coherent) state = "not_coherent";
        else if (!doa_enabled.load()) state = "doa_off";
        else if (!fix_ok) state = "no_fix";
        else if (doa_is_calibrating()) state = "calibrating";
        else state = "ok";

        // the VFOs
        std::set<int> seen;
        struct Disp { int id; double mhz; std::string lobe; double peak; float conf; bool held; int frames; double gate, moved; };
        std::vector<Disp> disp;
        std::map<int, rdf::Estimate> ests;
        {
            std::lock_guard<std::mutex> lk(SH().mu);
            for (const auto& [id, v] : SH().v) ests[id] = v.est;
        }
        std::string frame_state = state;
        if (coherent && doa_enabled.load()) {
            const double centre = ChannelManager::get_frequency(active_channel.load());
            for (const auto& dec : decimator_manager.getAllDecimators()) {
                if (!dec || !dec->music_processor || dec->being_deleted.load()) continue;
                auto& mp = *dec->music_processor;
                if (!mp.isEnabled()) continue;
                const int id = dec->id;
                seen.insert(id);
                SVfo& s = vs[id];
                const double freq = centre + dec->frequency_offset_hz;
                if (s.freq == 0 || std::fabs(freq - s.freq) > RETUNE_RESET_HZ) {
                    // a new VFO / another signal: its map starts over (or resumes a saved session)
                    const bool first = s.freq == 0;
                    s.freq = freq;
                    s.gate.reset();
                    push({first ? Job::HELLO : Job::RESET, id, freq, {}});
                }
                if (mp.isResultStale()) continue;
                const int64_t stamp = mp.getResultStampMs();
                if (stamp == 0 || stamp == s.last_stamp) continue;
                s.last_stamp = stamp;
                auto spec_d = mp.getPseudospectrum();
                const int n = mp.getNumAngles();
                const double res = mp.getAngularResolution();
                if (static_cast<int>(spec_d.size()) != n || n < 8 || !(res > 0)) continue;
                std::vector<float> spec(n);
                for (int i = 0; i < n; i++) spec[i] = static_cast<float>(std::fabs(spec_d(i)));
                const float conf = mp.getPeakAngleWithConfidence().second;
                // the vehicle when the frame's samples were taken
                Fix f;
                double yaw = 0;
                const int64_t tq = stamp - static_cast<int64_t>(FRAME_LAG_S * 1000);
                if (!fix_ok || !fix_at(track, tq, &f, &yaw) || !f.hvalid) {
                    if (state == "ok") frame_state = fix_ok ? "no_heading" : "no_fix";
                    continue;
                }
                Disp d{id, freq / 1e6, "", NAN, conf, false, s.gate.frames(), s.gate.gate_m(), s.gate.moved_m()};
                d.lobe = display_lobe(spec, res, f.hdg, &d.peak);
                const bool gps_course = !f.compass && !f.fixed;
                d.held = gps_course && f.spd < MIN_SPEED_MPS;
                // usable for the heat map?
                std::string why;
                if (state != "ok") why = state;
                else if (!g_enabled.load()) why = "off";
                else if (f.fixed) why = "static";
                else if (gps_course && f.spd < MIN_SPEED_MPS) why = "slow";
                else if (yaw > (gps_course ? YAW_LIMIT_GPS_DPS : YAW_LIMIT_COMPASS_DPS)) why = "turning";
                if (why.empty()) {
                    const double hs = f.compass ? 4.0 : 1.5 + 8.0 / std::max(f.spd, MIN_SPEED_MPS);
                    rdf::Lobe p;
                    if (rdf::north_lobe(spec.data(), n, res, f.hdg, std::hypot(BASE_SIGMA_DEG, hs), &p)) {
                        const rdf::Estimate& e = ests[id];
                        const double R = e.valid && e.mode_mass > 0.5 ? rdf::distance_m(f.lat, f.lon, e.lat, e.lon) : 0;
                        rdf::Record r;
                        if (s.gate.add(tq, f.lat, f.lon, p, conf, R, &r)) push({Job::RECORD, id, freq, r});
                        d.frames = s.gate.frames();
                        d.gate = s.gate.gate_m();
                        d.moved = s.gate.moved_m();
                    }
                } else if (why != "off") {
                    frame_state = why;
                }
                disp.push_back(std::move(d));
            }
        }
        // VFOs gone (or DoA off for them)
        for (auto it = vs.begin(); it != vs.end();) {
            if (!seen.count(it->first)) {
                push({Job::REMOVE, it->first, 0, {}});
                it = vs.erase(it);
            } else {
                ++it;
            }
        }
        std::lock_guard<std::mutex> lk(SH().mu);
        Shared& S = SH();
        S.have_fix = fix_ok;
        if (!track.empty()) S.last = track.back();
        S.state = frame_state;
        for (auto it = S.v.begin(); it != S.v.end();)
            if (!seen.count(it->first)) it = S.v.erase(it);
            else ++it;
        for (const auto& d : disp) {
            auto& v = S.v[d.id];
            v.mhz = d.mhz;
            v.lobe = d.lobe;
            v.peak = d.peak;
            v.conf = d.conf;
            v.held = d.held;
            v.lobe_t = t;
            v.frames = d.frames;
            v.gate_m = d.gate;
            v.moved_m = d.moved;
        }
        for (int id : seen) S.v[id];   // VFOs without a frame yet
        for (auto& [id, v] : S.v)
            if (t - v.lobe_t > 5000) v.lobe.clear();   // no frames (squelch closed): no lobe
    }
}

// --- engine --------------------------------------------------------------------
struct EVfo {
    rdf::Solver solver;
    double freq = 0;
    uint64_t built_ver = ~0ull;
    int64_t built_ms = 0;
    bool dirty = true;
    explicit EVfo(double range_km) : solver(rdf::Options{}, range_km, 256) {}
};

// saved sessions not (yet) claimed by a VFO: frequency -> (saved, records)
struct Pending { int64_t saved_ms; std::vector<rdf::Record> recs; };

// record on disk: t, lat, lon, conf, frames + the lobe as u8 log probability
// relative to its peak over 20 nats
void write_rec(std::ostream& o, const rdf::Record& r) {
    o.write(reinterpret_cast<const char*>(&r.t_ms), 8);
    o.write(reinterpret_cast<const char*>(&r.lat), 8);
    o.write(reinterpret_cast<const char*>(&r.lon), 8);
    o.write(reinterpret_cast<const char*>(&r.conf), 4);
    o.write(reinterpret_cast<const char*>(&r.frames), 4);
    const float mx = *std::max_element(r.p.begin(), r.p.end());
    uint8_t u[rdf::BINS];
    for (int k = 0; k < rdf::BINS; k++)
        u[k] = static_cast<uint8_t>(std::lround(255 * std::clamp(1 + std::log(std::max(r.p[k] / mx, 1e-12f)) / 20.0, 0.0, 1.0)));
    o.write(reinterpret_cast<const char*>(u), rdf::BINS);
}
bool read_rec(std::istream& in, rdf::Record* r) {
    uint8_t u[rdf::BINS];
    in.read(reinterpret_cast<char*>(&r->t_ms), 8);
    in.read(reinterpret_cast<char*>(&r->lat), 8);
    in.read(reinterpret_cast<char*>(&r->lon), 8);
    in.read(reinterpret_cast<char*>(&r->conf), 4);
    in.read(reinterpret_cast<char*>(&r->frames), 4);
    in.read(reinterpret_cast<char*>(u), rdf::BINS);
    if (!in || !is_finite_value(r->lat) || !is_finite_value(r->lon)) return false;
    double tot = 0;
    for (int k = 0; k < rdf::BINS; k++) tot += r->p[k] = static_cast<float>(std::exp((u[k] / 255.0 - 1) * 20));
    for (auto& x : r->p) x = static_cast<float>(x / tot);
    return true;
}

void save_session(const std::map<int, std::unique_ptr<EVfo>>& ev, const std::map<int64_t, Pending>& pending) {
    const std::string tmp = std::string(SESSION_FILE) + ".tmp";
    std::ofstream o(tmp, std::ios::binary);
    if (!o) return;
    o.write("KRDF1\n", 6);
    const int64_t t = now_ms();
    auto block = [&](double freq, int64_t saved, const std::vector<rdf::Record>& recs) {
        if (recs.empty()) return;
        const uint32_t n = static_cast<uint32_t>(recs.size());
        o.write(reinterpret_cast<const char*>(&freq), 8);
        o.write(reinterpret_cast<const char*>(&saved), 8);
        o.write(reinterpret_cast<const char*>(&n), 4);
        for (const auto& r : recs) write_rec(o, r);
    };
    for (const auto& [id, e] : ev) block(e->freq, t, e->solver.records());
    for (const auto& [f, p] : pending) block(static_cast<double>(f), p.saved_ms, p.recs);
    o.close();
    if (o) rename(tmp.c_str(), SESSION_FILE);
}

void load_session(std::map<int64_t, Pending>* pending) {
    std::ifstream in(SESSION_FILE, std::ios::binary);
    char magic[6];
    if (!in.read(magic, 6) || memcmp(magic, "KRDF1\n", 6) != 0) return;
    const int64_t t = now_ms();
    for (;;) {
        double freq;
        int64_t saved;
        uint32_t n;
        if (!in.read(reinterpret_cast<char*>(&freq), 8) || !in.read(reinterpret_cast<char*>(&saved), 8) ||
            !in.read(reinterpret_cast<char*>(&n), 4) || n > 100000)
            break;
        Pending p{saved, {}};
        rdf::Record r;
        for (uint32_t i = 0; i < n && read_rec(in, &r); i++) p.recs.push_back(r);
        if (t - saved < SESSION_MAX_AGE_H * 3600e3 && !p.recs.empty())
            (*pending)[static_cast<int64_t>(std::llround(freq / 1000.0)) * 1000] = std::move(p);
    }
}

std::string grid_json(const char* name, const rdf::Grid* g) {
    if (!g || !g->records()) return std::string("\"") + name + "\":null";
    double nlat, nlon, slat, slon;
    rdf::from_enu(g->lat0(), g->lon0(), -g->half_m(), g->half_m(), &nlat, &nlon);
    rdf::from_enu(g->lat0(), g->lon0(), g->half_m(), -g->half_m(), &slat, &slon);
    const auto r = g->raster(NATS_RANGE);
    return std::string("\"") + name + "\":{\"n\":" + std::to_string(g->n()) + ",\"cell_m\":" + num(g->cell_m(), 1) +
           ",\"nw\":[" + num(nlat, 6) + "," + num(nlon, 6) + "],\"se\":[" + num(slat, 6) + "," + num(slon, 6) +
           "],\"data\":\"" + b64(r.data(), r.size()) + "\"}";
}

std::string build_grid(int id, const EVfo& e) {
    const rdf::Solver& s = e.solver;
    const rdf::Estimate& est = s.estimate();
    std::ostringstream o;
    o << "{\"rdf_grid\":{\"vfo\":" << id << ",\"mhz\":" << num(e.freq / 1e6, 4) << ",\"records\":"
      << s.records().size() << ",\"range_km\":" << num(s.range_km(), 1) << ",\"nats\":" << num(NATS_RANGE, 0) << ","
      << grid_json("coarse", s.coarse()) << "," << grid_json("fine", s.fine()) << ",\"est\":";
    if (est.valid)
        o << "{\"lat\":" << num(est.lat, 6) << ",\"lon\":" << num(est.lon, 6) << ",\"r50\":" << num(est.r50_m, 0)
          << ",\"r95\":" << num(est.r95_m, 0) << ",\"a\":" << num(est.ell_a_m, 0) << ",\"b\":" << num(est.ell_b_m, 0)
          << ",\"ang\":" << num(est.ell_ang_deg, 1) << ",\"mass\":" << num(est.mode_mass, 3)
          << ",\"edge\":" << (est.at_edge ? "true" : "false") << ",\"off\":" << num(est.offset_deg, 1)
          << ",\"off_sd\":" << num(est.offset_sd_deg, 1) << "}";
    else
        o << "null";
    // the latest records' bearings (lines on the map)
    o << ",\"lines\":[";
    const auto& rs = s.records();
    for (size_t i = rs.size() > MAX_LINES ? rs.size() - MAX_LINES : 0, first = 1; i < rs.size(); i++, first = 0)
        o << (first ? "" : ",") << "[" << num(rs[i].lat, 6) << "," << num(rs[i].lon, 6) << ","
          << num(rdf::lobe_peak_deg(rs[i].p), 1) << "," << (rs[i].t_ms / 1000) << "]";
    o << "]}}";
    return o.str();
}

void engine() {
    std::map<int, std::unique_ptr<EVfo>> ev;
    std::map<int64_t, Pending> pending;
    load_session(&pending);
    if (!pending.empty()) std::cout << "RDF map: " << pending.size() << " saved session(s) to resume" << std::endl;
    int64_t saved_ms = now_ms();
    bool dirty_file = false;
    auto claim = [&](EVfo& e, double freq) {
        // a saved session on this frequency (+-5 kHz) resumes
        for (auto it = pending.begin(); it != pending.end(); ++it)
            if (std::fabs(it->first - freq) <= 5000) {
                std::cout << "RDF map: resuming " << it->second.recs.size() << " records at "
                          << num(freq / 1e6, 4) << " MHz" << std::endl;
                e.solver.load(std::move(it->second.recs));
                pending.erase(it);
                return;
            }
    };
    auto park = [&](EVfo& e) {
        // keep a VFO's records for a VFO that comes back on that frequency
        if (e.solver.records().empty() || e.freq <= 0) return;
        pending[static_cast<int64_t>(std::llround(e.freq / 1000.0)) * 1000] = {now_ms(), e.solver.records()};
    };
    while (!g_stop.load()) {
        std::deque<Job> jobs;
        {
            std::unique_lock<std::mutex> lk(q_mu);
            q_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !q.empty() || g_stop.load(); });
            jobs.swap(q);
        }
        const double range = g_range_km.load();
        for (auto& j : jobs) {
            auto& slot = ev[j.vfo];
            if (j.kind == Job::REMOVE) {
                if (slot) park(*slot);
                ev.erase(j.vfo);
                dirty_file = true;
                continue;
            }
            if (j.kind == Job::RANGE) {
                for (auto& [id, e] : ev)
                    if (e) { e->solver.set_range_km(range); e->dirty = true; }
                if (!slot) ev.erase(j.vfo);
                continue;
            }
            if (!slot) slot = std::make_unique<EVfo>(range);
            EVfo& e = *slot;
            switch (j.kind) {
                case Job::HELLO:
                    e.freq = j.freq;
                    if (e.solver.records().empty()) claim(e, j.freq);
                    e.dirty = true;
                    break;
                case Job::RESET:
                    if (j.freq > 0 && j.freq != e.freq) park(e);   // retuned: keep the old signal's session
                    e.solver.reset();
                    if (j.freq > 0) {
                        e.freq = j.freq;
                        claim(e, j.freq);
                    }
                    e.dirty = true;
                    dirty_file = true;
                    break;
                case Job::RECORD:
                    if (e.freq == 0) e.freq = j.freq;
                    e.solver.add(j.rec);
                    e.dirty = true;
                    dirty_file = true;
                    break;
                default:
                    break;
            }
        }
        // grid messages (at most every 2 s per VFO) + estimate snapshots
        const int64_t t = now_ms();
        for (auto& [id, e] : ev) {
            if (!e || !e->dirty || t - e->built_ms < 2000) continue;
            e->dirty = false;
            e->built_ms = t;
            std::string msg = build_grid(id, *e);
            std::lock_guard<std::mutex> lk(SH().mu);
            auto& v = SH().v[id];
            v.grid = std::move(msg);
            v.grid_ver++;
            v.est = e->solver.estimate();
            v.records = static_cast<int>(e->solver.records().size());
        }
        if (dirty_file && t - saved_ms > 30000) {
            save_session(ev, pending);
            saved_ms = t;
            dirty_file = false;
        }
    }
    save_session(ev, pending);
}

}  // namespace

void start() {
    g_stop = false;
    sampler_thr = std::thread(sampler);
    engine_thr = std::thread(engine);
}

void stop() {
    g_stop = true;
    q_cv.notify_all();
    if (sampler_thr.joinable()) sampler_thr.join();
    if (engine_thr.joinable()) engine_thr.join();
}

void set_enabled(bool on) { g_enabled = on; }

bool set_range_km(double km) {
    if (!(km >= 1 && km <= 50)) return false;
    g_range_km = km;
    push({Job::RANGE, -1, 0, {}});
    return true;
}

void reset(int vfo) {
    if (vfo >= 0) {
        push({Job::RESET, vfo, 0, {}});
        return;
    }
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(SH().mu);
        for (const auto& [id, v] : SH().v) ids.push_back(id);
    }
    for (int id : ids) push({Job::RESET, id, 0, {}});
}

void request_full() {
    std::lock_guard<std::mutex> lk(SH().mu);
    SH().full = true;
}

std::string status_message() {
    Shared& S = SH();
    std::lock_guard<std::mutex> lk(S.mu);
    std::ostringstream o;
    o << "{\"rdf\":{\"on\":" << (g_enabled.load() ? "true" : "false") << ",\"range_km\":" << num(g_range_km.load(), 1)
      << ",\"state\":\"" << S.state << "\",\"station\":";
    if (S.have_fix)
        o << "{\"lat\":" << num(S.last.lat, 6) << ",\"lon\":" << num(S.last.lon, 6) << ",\"hdg\":"
          << (S.last.hvalid ? num(S.last.hdg, 1) : "null") << ",\"spd\":" << num(S.last.spd, 1) << ",\"src\":\""
          << (S.last.fixed ? "static" : S.last.compass ? "compass" : "gps") << "\"}";
    else
        o << "null";
    o << ",\"vfos\":[";
    bool first = true;
    for (const auto& [id, v] : S.v) {
        o << (first ? "" : ",") << "{\"id\":" << id << ",\"mhz\":" << num(v.mhz, 4) << ",\"lobe\":"
          << (v.lobe.empty() ? "null" : "\"" + v.lobe + "\"") << ",\"peak\":" << (v.lobe.empty() ? "null" : num(v.peak, 1))
          << ",\"conf\":" << num(v.conf, 2) << ",\"held\":" << (v.held ? "true" : "false") << ",\"frames\":" << v.frames
          << ",\"gate_m\":" << num(v.gate_m, 0) << ",\"moved_m\":" << num(v.moved_m, 0) << ",\"records\":" << v.records
          << "}";
        first = false;
    }
    o << "]}}";
    return o.str();
}

std::vector<std::string> grid_messages() {
    Shared& S = SH();
    std::lock_guard<std::mutex> lk(S.mu);
    std::vector<std::string> out;
    for (auto& [id, v] : S.v) {
        if (v.grid.empty()) continue;
        if (S.full || v.sent_ver != v.grid_ver) {
            out.push_back(v.grid);
            v.sent_ver = v.grid_ver;
        }
    }
    S.full = false;
    return out;
}

}  // namespace rdfmap
