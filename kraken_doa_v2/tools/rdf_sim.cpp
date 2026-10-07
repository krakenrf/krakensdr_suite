// Drive simulator for the mobile DF heat map (rdf_engine): Monte-Carlo drives
// around a hidden transmitter, comparing this program's algorithm with a
// re-implementation of the KrakenSDR Android app's "CAL" heat map choices.
//
//   g++ -std=c++20 -O2 -Iinclude tools/rdf_sim.cpp src/rdf_engine.cpp -o rdf_sim
//   ./rdf_sim [runs] [minutes] [seed]
//
// World model per run: transmitter 1.5-6 km from the start; the vehicle
// drives straight road segments (150-900 m, 8-20 m/s) and turns (12 deg/s)
// toward the transmitter's bearing +- up to 110 deg, as a hunter would.
// DoA frames at 5 Hz, each the MUSIC-like pseudospectrum (1 deg bins,
// relative to the array, unit circle CCW) of:
//   - the true bearing + 2.5 deg noise, measured LAG_S late (the frame is
//     computed from samples 0.4 s old when it is stamped)
//   - a mounting offset of the array (uniform +-5 deg, the same all run)
//   - multipath: 25 % of the road (stretches of 150-600 m) the strongest
//     peak is a reflection 15-70 deg off (fixed per stretch), the true one
//     weaker beside it; elsewhere a weak ghost peak somewhere random
// The heading the receiver knows: GPS course = true track + 1.5 deg noise,
// lagging 1 s (so it is wrong while turning), only valid above 2 m/s.

#include "rdf_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <vector>

using namespace rdf;

namespace {
constexpr double PI = 3.14159265358979323846, D2R = PI / 180;
constexpr double DT = 0.2;          // 5 Hz frames
constexpr double LAG_S = 0.4;       // frame geometry this much older than its stamp
constexpr double HEAD_LAG_S = 1.0;  // GPS course lags the true heading
constexpr int SPEC_N = 360;

double wrap(double a) { a = std::fmod(a, 360.0); return a < 0 ? a + 360 : a; }
double wdiff(double a, double b) { return std::remainder(a - b, 360.0); }

struct State { double t, lat, lon, heading, speed; };

// MUSIC-like peak: sharp, 1/(1 + q - cos) shape
void add_peak(std::vector<float>& s, double at_deg, double height, double width_deg) {
    const double q = 1 - std::cos(width_deg * D2R);
    for (int i = 0; i < SPEC_N; i++) {
        const double d = (i - at_deg) * D2R;
        s[i] += static_cast<float>(height * q / (1 + q - std::cos(d)));
    }
}

struct Result { double err_mine = 1e9, err_base = 1e9, r95 = 0, r95b = 0; bool covered = false, covered_b = false; int recs = 0; double off_true = 0, off_est = 0; };

// ---- the Android app's choices (HeatMap.kt CAL mode + DistanceGatedAggregator)
struct BaseGate {
    // dB-domain, confidence-weighted average of north-rotated lobes; 20 m gate
    std::vector<double> sum = std::vector<double>(360, 0.0);
    double wsum = 0, slat = 0, slon = 0, lat0 = 0, lon0 = 0;
    int n = 0;
    bool add(double lat, double lon, double heading, const std::vector<float>& spec, double conf, Record* out) {
        // spec -> dB above min, rotated to north: north[k] = dB[(heading - k) mod 360]
        float mx = *std::max_element(spec.begin(), spec.end());
        std::vector<double> db(360);
        double mn = 1e9;
        for (int i = 0; i < 360; i++) { db[i] = std::max(-100.0, 10 * std::log10(std::max(static_cast<double>(spec[i] / mx), 1e-10))); mn = std::min(mn, db[i]); }
        if (n == 0) { lat0 = lat; lon0 = lon; }
        const double w = std::max(conf, 0.0) + 0.001;
        for (int k = 0; k < 360; k++) {
            const double b = wrap(heading - k);
            const int i0 = static_cast<int>(b);
            const double f = b - i0;
            sum[k] += w * ((db[i0 % 360] * (1 - f) + db[(i0 + 1) % 360] * f) - mn);
        }
        wsum += w; slat += lat; slon += lon; n++;
        if (distance_m(lat0, lon0, lat, lon) < 20) return false;
        // record: dB -> linear (normalised to max), 3 deg blur, unit mass, on 0.5 deg bins
        std::vector<double> lin(360);
        double dmx = -1e9;
        for (int k = 0; k < 360; k++) dmx = std::max(dmx, sum[k] / wsum);
        for (int k = 0; k < 360; k++) lin[k] = std::exp((sum[k] / wsum - dmx) * std::log(10.0) / 10);
        std::vector<double> bl(360, 0.0);
        double tot = 0;
        for (int k = 0; k < 360; k++) {
            double s = 0, ks = 0;
            for (int j = -7; j <= 7; j++) { const double g = std::exp(-0.5 * j * j / 9.0); s += g * lin[(k + j + 360) % 360]; ks += g; }
            tot += bl[k] = s / ks;
        }
        for (int k = 0; k < BINS; k++) out->p[k] = static_cast<float>(bl[k / 2] / tot / 2);
        out->lat = slat / n; out->lon = slon / n; out->frames = n; out->conf = 0;
        std::fill(sum.begin(), sum.end(), 0.0);
        wsum = slat = slon = 0; n = 0;
        return true;
    }
};

Result run(std::mt19937& rng, double minutes) {
    std::uniform_real_distribution<double> U(0, 1);
    std::normal_distribution<double> N(0, 1);
    const double lat0 = -36.85, lon0 = 174.76;
    const double tx_d = 1500 + 4500 * U(rng), tx_b = 360 * U(rng);
    double txlat, txlon;
    from_enu(lat0, lon0, tx_d * std::sin(tx_b * D2R), tx_d * std::cos(tx_b * D2R), &txlat, &txlon);
    const bool clean = getenv("SIM_CLEAN") != nullptr;
    const double mount = clean ? 0 : -5 + 10 * U(rng);

    // the drive
    std::vector<State> path;
    double lat = lat0, lon = lon0, hdg = 360 * U(rng), spd = 8 + 12 * U(rng);
    double seg_left = 150 + 750 * U(rng), target = hdg;
    for (double t = 0; t < minutes * 60; t += DT) {
        path.push_back({t, lat, lon, hdg, spd});
        if (std::fabs(wdiff(target, hdg)) > 0.5) {
            hdg = wrap(hdg + std::clamp(wdiff(target, hdg), -12 * DT, 12 * DT));   // turning
        } else if ((seg_left -= spd * DT) <= 0) {
            seg_left = 150 + 750 * U(rng);
            const double to_tx = bearing_deg(lat, lon, txlat, txlon);
            target = wrap(to_tx + (U(rng) * 2 - 1) * 110);
            spd = 8 + 12 * U(rng);
            // close to the transmitter: drive past / around it
            if (distance_m(lat, lon, txlat, txlon) < 300) target = wrap(to_tx + 90 + 180 * U(rng));
        }
        double e, n;
        to_enu(lat0, lon0, lat, lon, &e, &n);
        e += spd * DT * std::sin(hdg * D2R);
        n += spd * DT * std::cos(hdg * D2R);
        from_enu(lat0, lon0, e, n, &lat, &lon);
    }
    // multipath stretches (by distance driven)
    struct Mp { double from, to, off; };
    std::vector<Mp> mps;
    double dist_total = 0;
    for (size_t i = 1; i < path.size(); i++) dist_total += path[i].speed * DT;
    for (double d = 0; d < dist_total;) {
        const double len = 150 + 450 * U(rng);
        static const double mp_frac = getenv("SIM_MP") ? atof(getenv("SIM_MP")) : 0.25;
        if (!clean && U(rng) < mp_frac) mps.push_back({d, d + len, (U(rng) < 0.5 ? -1 : 1) * (15 + 55 * U(rng))});
        d += len * (1 + 2 * U(rng));
    }
    auto state_at = [&](double t) {
        const double x = std::clamp(t / DT, 0.0, static_cast<double>(path.size() - 1));
        return path[static_cast<size_t>(x)];
    };

    Options mine;
    auto envd = [](const char* k, double d) { const char* v = getenv(k); return v ? atof(v) : d; };
    mine.alpha = envd("SIM_ALPHA", mine.alpha);
    mine.ess_cap = envd("SIM_ESS", mine.ess_cap);
    mine.eps = envd("SIM_EPS", mine.eps);
    if (getenv("SIM_NOMARG")) mine.marginalize_offsets = false;
    Solver solver(mine, 10, 256);
    GateCfg gc;
    gc.max_gate_m = envd("SIM_GATE_MAX", gc.max_gate_m);
    gc.min_gate_m = envd("SIM_GATE_MIN", gc.min_gate_m);
    gc.default_range_m = envd("SIM_RANGE0", gc.default_range_m);
    Gate gate(gc);
    Options bo;
    bo.eps = 0.05; bo.alpha = 0.25; bo.ess_cap = 25;
    bo.offsets_deg.clear();
    for (int d = -6; d <= 6; d++) bo.offsets_deg.push_back(d);
    bo.marginalize_offsets = false; bo.interpolate = false;
    Solver base(bo, 5.5, 100);
    BaseGate bgate;

    double driven = 0;
    std::deque<std::pair<double, State>> gps;   // what the receiver knows: (time, state with lagged course)
    for (size_t fi = 0; fi < path.size(); fi++) {
        const double t = path[fi].t;
        if (fi) driven += path[fi].speed * DT;
        // GPS course: lagged + noise; known now
        State g = path[fi];
        g.heading = clean ? path[fi].heading : wrap(state_at(t - HEAD_LAG_S).heading + 1.5 * N(rng));
        gps.push_back({t, g});
        if (gps.size() > 50) gps.pop_front();
        // the DoA frame stamped t describes the geometry at t - LAG_S
        const State tr = state_at(clean ? t : t - LAG_S);
        const double tau = bearing_deg(tr.lat, tr.lon, txlat, txlon);
        double rel = wrap(tr.heading - tau - mount + (clean ? 0 : 2.5 * N(rng)));   // unit circle, relative to the array
        std::vector<float> spec(SPEC_N, 0.f);
        const Mp* mp = nullptr;
        for (const auto& m : mps) if (driven >= m.from && driven < m.to) mp = &m;
        if (mp) {
            add_peak(spec, wrap(rel + mp->off), 1.0, 4);
            add_peak(spec, rel, 0.45, 4);
        } else {
            add_peak(spec, rel, 1.0, 4);
            add_peak(spec, wrap(rel + 40 + 280 * U(rng)), 0.12, 6);
        }
        for (auto& v : spec) v += static_cast<float>(0.02 * U(rng));
        const double conf = mp ? 0.4 : 0.8;
        if (g.speed < 2) continue;
        // mine: position + heading interpolated to the frame's time (stamp - assumed lag 0.3 s)
        const double tq = clean ? t : t - 0.3;
        State a = gps.front().second;
        for (const auto& [gt, gs] : gps) if (gt <= tq) a = gs;
        Lobe p;
        const double sigma = std::sqrt(2.0 * 2.0 + std::pow(1.5 + 8 / std::max(g.speed, 2.0), 2));
        if (north_lobe(spec.data(), SPEC_N, 1.0, a.heading, sigma, &p)) {
            Record r;
            const Estimate& e = solver.estimate();
            const double R = e.valid && e.mode_mass > 0.5 ? distance_m(a.lat, a.lon, e.lat, e.lon) : 0;
            if (gate.add(static_cast<int64_t>(t * 1000), a.lat, a.lon, p, static_cast<float>(conf), R, &r)) solver.add(r);
        }
        // Android: the latest fix, no alignment
        Record rb;
        if (bgate.add(g.lat, g.lon, g.heading, spec, conf, &rb)) base.add(rb);
    }
    Result res;
    const Estimate& e = solver.estimate();
    const Estimate& b = base.estimate();
    if (e.valid) {
        res.err_mine = getenv("SIM_MAP") ? distance_m(e.peak_lat, e.peak_lon, txlat, txlon) : distance_m(e.lat, e.lon, txlat, txlon);
        res.r95 = e.r95_m;
        res.covered = res.err_mine <= std::max(e.r95_m, 1.0);
        res.off_est = e.offset_deg;
    }
    if (b.valid) {
        res.err_base = distance_m(b.peak_lat, b.peak_lon, txlat, txlon);
        res.r95b = b.r95_m;
        res.covered_b = res.err_base <= std::max(b.r95_m, 1.0);
    }
    res.recs = e.records;
    res.off_true = -mount;   // measured = true - offset  <=>  offset = -mount
    return res;
}

double pct(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))];
}
}  // namespace

int main(int argc, char** argv) {
    const int runs = argc > 1 ? atoi(argv[1]) : 30;
    const double minutes = argc > 2 ? atof(argv[2]) : 15;
    std::mt19937 rng(argc > 3 ? atoi(argv[3]) : 1);
    std::vector<double> em, eb, offerr;
    int covered = 0, covered_b = 0;
    for (int i = 0; i < runs; i++) {
        Result r = run(rng, minutes);
        em.push_back(r.err_mine);
        eb.push_back(r.err_base);
        covered += r.covered;
        covered_b += r.covered_b;
        offerr.push_back(std::fabs(r.off_est - r.off_true));
        printf("run %2d: records %4d  error: this %6.0f m (r95 %5.0f m%s)  android-style %6.0f m   offset true %+5.1f est %+5.1f\n",
               i, r.recs, r.err_mine, r.r95, r.covered ? ", inside" : ", OUTSIDE", r.err_base, r.off_true, r.off_est);
    }
    printf("\n%d drives of %.0f min\n", runs, minutes);
    printf("  this program : median %5.0f m  p75 %5.0f m  p90 %5.0f m   truth inside the 95%% region: %d/%d\n",
           pct(em, .5), pct(em, .75), pct(em, .9), covered, runs);
    printf("  android-style: median %5.0f m  p75 %5.0f m  p90 %5.0f m   truth inside its 95%% region: %d/%d\n",
           pct(eb, .5), pct(eb, .75), pct(eb, .9), covered_b, runs);
    printf("  offset estimate error: median %.1f deg  p90 %.1f deg\n", pct(offerr, .5), pct(offerr, .9));
    return 0;
}
