#include "array_cal.hpp"

#include "geo_address.hpp"   // geo::json_parse
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace array_cal {
namespace {

using cd = std::complex<double>;
constexpr int MAXN = 8;
constexpr double C = 3e8;                   // as MUSIC's wavelength (3e8 / f): the positions live in its frame
constexpr const char* CAL_FILE = "array_cal.json";
// which packets are used
constexpr double MIN_DIST_KM = 5;           // nearer: the ADS-B position error matters, near field
constexpr double MAX_DIST_KM = 450;
// below MIN_EL_DEG: aircraft on / near the ground - their signal often only
// arrives reflected or around buildings / terrain (seen at Auckland Airport:
// five taxiing aircraft at 246 deg all read 161 deg)
constexpr double MIN_EL_DEG = 2, MAX_EL_DEG = 60;
constexpr double MIN_SNR = 10;              // dominant eigenvalue / mean of the others
constexpr int64_t PER_AIRCRAFT_MS = 500;    // at most 2 packets a second per aircraft
constexpr int SECTORS = 36;                 // 10 deg compass sectors (coverage)
constexpr size_t SECTOR_CAP = 400;          // packets kept per sector, then replaced at random
// the fit
constexpr size_t MIN_FIT_SAMPLES = 150;
constexpr int MIN_FIT_AIRCRAFT = 3;
constexpr size_t MIN_FOLD_SAMPLES = 60;
constexpr int64_t FIT_EVERY_MS = 20000;
constexpr size_t COARSE_SAMPLES = 3000;     // the rotation search runs on a subset
constexpr double EVAL_MAX_EL_DEG = 20;      // the bearing check: aircraft low enough for el = 0 steering
constexpr double LOS_DEG = 25;              // an aircraft further off the consensus than this: not line of sight
constexpr double WRONG_DEG = 10;
constexpr int GOOD_SECTOR_PACKETS = 10;     // a 30 deg sector counts as covered from this many
// MAP priors: with few aircraft from a direction the corrections stay small
constexpr double PRIOR_ROT_RAD = 0.5;       // ~30 deg
constexpr double PRIOR_TAU_NS = 2.0;        // ~40 cm of coax
constexpr double PRIOR_POS_MM = 15.0;
constexpr double MATCH_TOL_M = 0.0005;      // in use only for an array within 0.5 mm of the one calibrated
constexpr double RESID_WARN_DEG = 15;
const char* const REFIT_NOTE = "The array settings changed - fitting the packets to the new ones";

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Geo {
    int n = 0;
    double x[MAXN] = {}, y[MAXN] = {}, z[MAXN] = {};
};
bool same_geo(const Geo& a, const Geo& b) {
    if (a.n != b.n) return false;
    for (int i = 0; i < a.n; i++)
        if (std::fabs(a.x[i] - b.x[i]) > MATCH_TOL_M || std::fabs(a.y[i] - b.y[i]) > MATCH_TOL_M ||
            std::fabs(a.z[i] - b.z[i]) > MATCH_TOL_M)
            return false;
    return true;
}

struct Sample {
    cd u[MAXN];             // dominant eigenvector of the packet's covariance, |u| = 1
    double theta = 0;       // steering-frame azimuth (rad)
    double el = 0;          // elevation (rad)
    double f = 0;           // Hz
    double az_deg = 0, dist_km = 0, snr = 0;
    uint64_t ac = 0;        // the aircraft (hash of its id): cross-check folds
};

struct Params {
    double rot = 0;                          // rad, counter-clockwise
    double tau[MAXN] = {};                   // s, tau[0] = 0
    double dx[MAXN] = {}, dy[MAXN] = {};     // m
};

struct Stat { int n = 0; double med = 0, p90 = 0, wrong = 0; };
struct LeftOut { std::string id; double err; int n; };   // an aircraft left out of a fit: bearing error, packets

struct Fit {
    bool ok = false;
    std::string error, warn;
    Geo g;
    Params p;
    double resid[MAXN] = {}, gain_db[MAXN] = {}, resid_all = 0;
    int samples = 0, used = 0, aircraft = 0, sectors = 0;
    double el_min = 0, el_max = 0, f_hz = 0;
    bool cv = false;
    Stat before, after;
    int64_t t_ms = 0;
    int aircraft_used = 0;              // aircraft - left_out
    std::vector<LeftOut> left_out;      // aircraft whose bearings disagree with their positions
    int left_out_n = 0;                 // how many (also kept in the file; the list isn't)
};

struct Applied {
    bool have = false, enabled = true, rotation = true;
    Fit fit;
};

std::mutex mu;
std::atomic<uint64_t> gen{1};
std::atomic<bool> collect_on{false};
std::string state_;
Geo arr_;                    // the array the steering uses now
bool arr_known_ = false;
Geo col_g_;                  // the array the collection is for
bool col_have_g_ = false;
std::array<std::vector<Sample>, SECTORS> sec_;
size_t nsamples_ = 0;
std::map<uint64_t, int64_t> last_by_ac_;
uint64_t epoch_ = 1;         // bumps when the collection starts over
double col_offset_ = 0, col_heading_ = 0;   // the array offset / heading the packets were taken with
bool col_fixed_ = false;
uint64_t changes_ = 0;       // packets added in this epoch
int64_t last_sample_ms_ = 0;
uint64_t rng_ = 88172645463325252ull;
struct Rej { uint64_t near = 0, far = 0, low = 0, high = 0, weak = 0; } rej_;
std::map<uint64_t, std::string> ids_;   // aircraft hash -> its ICAO address (the left-out list)
std::string note_;
Fit fit_;
bool fitting_ = false;
bool kick_ = false;
Applied app_;

std::thread worker_;
std::atomic<bool> stop_{false};
std::condition_variable cv_;

uint64_t next_rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_;
}

void clear_samples_locked() {
    for (auto& s : sec_) s.clear();
    nsamples_ = 0;
    last_by_ac_.clear();
    ids_.clear();
    rej_ = {};
    changes_ = 0;
    epoch_++;
    last_sample_ms_ = 0;
    fit_ = Fit{};
}

// --- geometry -----------------------------------------------------------------
void to_ecef(double lat, double lon, double h, double* o) {
    constexpr double a = 6378137.0, e2 = 6.69437999014e-3;
    const double fl = lat * M_PI / 180, ll = lon * M_PI / 180, s = std::sin(fl);
    const double N = a / std::sqrt(1 - e2 * s * s);
    o[0] = (N + h) * std::cos(fl) * std::cos(ll);
    o[1] = (N + h) * std::cos(fl) * std::sin(ll);
    o[2] = (N * (1 - e2) + h) * s;
}

// the model's phase of every element for a source at (theta, el) - exactly the
// steering MUSIC builds (music_processor.cpp): k (x cos el cos th + y cos el
// sin th + z sin el), here with the array rotated, the elements moved and
// each one's delay
void phases(const Geo& g, const Params& p, bool rot, double f, double theta, double el, double* ph) {
    const double k = 2 * M_PI * f / C, ce = std::cos(el);
    const double dX = ce * std::cos(theta), dY = ce * std::sin(theta), dZ = std::sin(el);
    const double cr = rot ? std::cos(p.rot) : 1.0, sr = rot ? std::sin(p.rot) : 0.0;
    for (int i = 0; i < g.n; i++) {
        const double x = cr * g.x[i] - sr * g.y[i] + p.dx[i];
        const double y = sr * g.x[i] + cr * g.y[i] + p.dy[i];
        ph[i] = k * (x * dX + y * dY + g.z[i] * dZ) - 2 * M_PI * f * p.tau[i];
    }
}

// --- the fit -----------------------------------------------------------------------
// parameter vector: [rot (rad), tau_1..tau_{n-1} (ns), dx_0..dx_{n-1} (mm), dy_0..dy_{n-1} (mm)]
Eigen::VectorXd pack(const Params& p, int n) {
    Eigen::VectorXd x(3 * n);
    x(0) = p.rot;
    for (int i = 1; i < n; i++) x(i) = p.tau[i] * 1e9;
    for (int i = 0; i < n; i++) {
        x(n + i) = p.dx[i] * 1e3;
        x(2 * n + i) = p.dy[i] * 1e3;
    }
    return x;
}
Params unpack(const Eigen::VectorXd& x, int n) {
    Params p;
    p.rot = std::remainder(x(0), 2 * M_PI);
    for (int i = 1; i < n; i++) p.tau[i] = x(i) * 1e-9;
    for (int i = 0; i < n; i++) {
        p.dx[i] = x(n + i) * 1e-3;
        p.dy[i] = x(2 * n + i) * 1e-3;
    }
    return p;
}

// Starting point: the rotation by search (2 deg steps; a wrong station
// heading can be far off), each element's constant phase offset against
// element 0 by circular mean - as a delay at the median frequency
Params coarse_init(const std::vector<const Sample*>& all, const Geo& g) {
    const int n = g.n;
    std::vector<const Sample*> S;
    const size_t step = std::max<size_t>(1, all.size() / COARSE_SAMPLES);
    for (size_t i = 0; i < all.size(); i += step) S.push_back(all[i]);
    std::vector<double> fs;
    for (const Sample* s : S) fs.push_back(s->f);
    std::nth_element(fs.begin(), fs.begin() + fs.size() / 2, fs.end());
    const double fref = fs[fs.size() / 2];
    std::vector<std::array<cd, MAXN>> Z(S.size());
    double best = -1;
    Params bp;
    for (int d = -180; d < 180; d += 2) {
        Params p;
        p.rot = d * M_PI / 180;
        cd acc[MAXN] = {};
        for (size_t s = 0; s < S.size(); s++) {
            double ph[MAXN];
            phases(g, p, true, S[s]->f, S[s]->theta, S[s]->el, ph);
            for (int i = 0; i < n; i++) Z[s][i] = S[s]->u[i] * std::polar(1.0, -ph[i]);
            for (int i = 0; i < n; i++) acc[i] += Z[s][i] * std::conj(Z[s][0]);
        }
        cd off[MAXN];
        for (int i = 0; i < n; i++) off[i] = std::polar(1.0, -std::arg(acc[i]));
        double J = 0;
        for (size_t s = 0; s < S.size(); s++) {
            cd sum = 0;
            for (int i = 0; i < n; i++) sum += Z[s][i] * off[i];
            J += std::norm(sum);
        }
        if (J > best) {
            best = J;
            bp = p;
            // the data's extra phase psi = -2 pi f tau
            for (int i = 1; i < n; i++) bp.tau[i] = -std::arg(acc[i]) / (2 * M_PI * fref);
        }
    }
    return bp;
}

struct Solved {
    Params p;
    double resid[MAXN] = {}, gain_db[MAXN] = {}, resid_all = 0;
    int used = 0;
};

// Gauss-Newton on the per-element phase residuals (the common phase of each
// packet projected out), MAP with the priors, Cauchy-reweighted against
// outliers (multipath, a packet given to the wrong aircraft)
Solved solve(const std::vector<const Sample*>& S, const Geo& g, Params p) {
    const int n = g.n, P = 3 * n;
    std::vector<double> w(S.size(), 1.0), rho(S.size(), 0.0);
    Eigen::VectorXd L(P);
    L(0) = 1 / (PRIOR_ROT_RAD * PRIOR_ROT_RAD);
    for (int i = 1; i < n; i++) L(i) = 1 / (PRIOR_TAU_NS * PRIOR_TAU_NS);
    for (int i = n; i < P; i++) L(i) = 1 / (PRIOR_POS_MM * PRIOR_POS_MM);
    Solved out;
    for (int it = 0; it < 25; it++) {
        Eigen::MatrixXd A = Eigen::MatrixXd::Zero(P, P);
        Eigen::VectorXd b = Eigen::VectorXd::Zero(P);
        double sr2 = 0, sw = 0;
        double er2[MAXN] = {}, ea[MAXN] = {}, ga[MAXN] = {};
        const double cr = std::cos(p.rot), srt = std::sin(p.rot);
        for (size_t s = 0; s < S.size(); s++) {
            const Sample& x = *S[s];
            double ph[MAXN];
            phases(g, p, true, x.f, x.theta, x.el, ph);
            cd z[MAXN], sum = 0;
            double a[MAXN], asum = 0;
            for (int i = 0; i < n; i++) {
                z[i] = x.u[i] * std::polar(1.0, -ph[i]);
                sum += z[i];
                a[i] = std::norm(x.u[i]) * n;   // ~1 each
                asum += a[i];
            }
            if (!(asum > 0)) continue;
            const double c = std::arg(sum);
            double r[MAXN], rm = 0;
            for (int i = 0; i < n; i++) {
                r[i] = std::arg(z[i] * std::polar(1.0, -c));
                rm += a[i] * r[i];
            }
            rm /= asum;
            const double k = 2 * M_PI * x.f / C, ce = std::cos(x.el);
            const double dX = ce * std::cos(x.theta), dY = ce * std::sin(x.theta);
            double J[MAXN][3 * MAXN] = {};
            double Jm[3 * MAXN] = {};
            for (int i = 0; i < n; i++) {
                J[i][0] = k * ((-srt * g.x[i] - cr * g.y[i]) * dX + (cr * g.x[i] - srt * g.y[i]) * dY);
                if (i >= 1) J[i][i] = -2 * M_PI * x.f * 1e-9;
                J[i][n + i] = k * dX * 1e-3;
                J[i][2 * n + i] = k * dY * 1e-3;
                for (int q = 0; q < P; q++) Jm[q] += a[i] * J[i][q];
            }
            for (int q = 0; q < P; q++) Jm[q] /= asum;
            double rr = 0;
            for (int i = 0; i < n; i++) {
                const double ri = r[i] - rm;
                rr += a[i] * ri * ri;
                double Jt[3 * MAXN];
                for (int q = 0; q < P; q++) Jt[q] = J[i][q] - Jm[q];
                const double wa = w[s] * a[i];
                for (int q1 = 0; q1 < P; q1++) {
                    if (Jt[q1] == 0) continue;
                    b(q1) += wa * Jt[q1] * ri;
                    for (int q2 = q1; q2 < P; q2++) A(q1, q2) += wa * Jt[q1] * Jt[q2];
                }
                er2[i] += w[s] * a[i] * ri * ri;
                ea[i] += w[s] * a[i];
                ga[i] += w[s] * a[i];
            }
            rho[s] = std::sqrt(rr / asum);
            sr2 += w[s] * rr / asum;
            sw += w[s];
        }
        if (!(sw > 0)) break;
        for (int q1 = 0; q1 < P; q1++)
            for (int q2 = 0; q2 < q1; q2++) A(q1, q2) = A(q2, q1);
        // the data's precision: the residual variance (>= 1 deg)
        const double sig2 = std::max(sr2 / sw, std::pow(M_PI / 180, 2));
        A /= sig2;
        b /= sig2;
        const Eigen::VectorXd x0 = pack(p, n);
        A += L.asDiagonal();
        b -= L.cwiseProduct(x0);
        const Eigen::VectorXd d = A.ldlt().solve(b);
        bool finite = true;   // (-Ofast: is_finite_value, never Eigen's allFinite / std::isfinite)
        for (int q = 0; q < P; q++) finite = finite && is_finite_value(d(q));
        if (!finite) break;
        p = unpack(x0 + d, n);
        // the state the step was computed from
        double tot = 0, tota = 0;
        for (int i = 0; i < n; i++) {
            out.resid[i] = ea[i] > 0 ? std::sqrt(er2[i] / ea[i]) * 180 / M_PI : 0;
            out.gain_db[i] = sw > 0 ? 10 * std::log10(std::max(ga[i] / sw, 1e-6)) : 0;
            tot += er2[i];
            tota += ea[i];
        }
        out.resid_all = tota > 0 ? std::sqrt(tot / tota) * 180 / M_PI : 0;
        // Cauchy weights from the per-packet rms residual
        std::vector<double> rs(rho);
        std::nth_element(rs.begin(), rs.begin() + rs.size() / 2, rs.end());
        const double cw = std::max(2.5 * rs[rs.size() / 2], 3 * M_PI / 180);
        out.used = 0;
        for (size_t s = 0; s < S.size(); s++) {
            w[s] = 1 / (1 + std::pow(rho[s] / cw, 2));
            if (w[s] > 0.5) out.used++;
        }
        double dmax = std::fabs(d(0)) * 180 / M_PI * 100;   // 0.01 deg ~ 1
        for (int q = 1; q < P; q++) dmax = std::max(dmax, std::fabs(d(q)) * 100);   // 0.01 ns / 0.01 mm ~ 1
        if (it >= 3 && dmax < 0.1) break;
    }
    out.p = p;
    return out;
}

// The bearing MUSIC would give for a packet (1 deg grid + parabola on its
// eigenvector), with these corrections - at elevation el_rad (0 = the
// operational steering; to 0.5 deg)
struct Steer {
    std::map<int64_t, std::vector<cd>> by_key;   // 360 x n steering (conj) per kHz + elevation
};
double bearing_deg(const Geo& g, const Params& p, bool rot, const Sample& s, Steer& cache, double el_rad = 0) {
    const int n = g.n;
    const int64_t khz = static_cast<int64_t>(std::llround(s.f / 1000));
    const int64_t el2 = std::llround(el_rad * 180 / M_PI * 2);
    const int64_t key = khz * 1000 + el2;
    auto it = cache.by_key.find(key);
    if (it == cache.by_key.end()) {
        std::vector<cd> a(360 * static_cast<size_t>(n));
        for (int j = 0; j < 360; j++) {
            double ph[MAXN];
            phases(g, p, rot, khz * 1000.0, j * M_PI / 180, el2 / 2.0 * M_PI / 180, ph);
            for (int i = 0; i < n; i++) a[static_cast<size_t>(j * n + i)] = std::polar(1.0, -ph[i]);
        }
        it = cache.by_key.emplace(key, std::move(a)).first;
    }
    const std::vector<cd>& a = it->second;
    double pw[360];
    int jm = 0;
    for (int j = 0; j < 360; j++) {
        cd sum = 0;
        for (int i = 0; i < n; i++) sum += a[static_cast<size_t>(j * n + i)] * s.u[i];
        pw[j] = std::norm(sum);
        if (pw[j] > pw[jm]) jm = j;
    }
    const double y0 = pw[(jm + 359) % 360], y1 = pw[jm], y2 = pw[(jm + 1) % 360];
    const double den = y0 - 2 * y1 + y2;
    const double d = den < 0 ? std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
    return jm + d;
}

Stat stat_of(std::vector<double> e) {
    Stat s;
    if (e.empty()) return s;
    for (double& v : e) v = std::fabs(v);
    std::sort(e.begin(), e.end());
    s.n = static_cast<int>(e.size());
    s.med = e[e.size() / 2];
    s.p90 = e[std::min(e.size() - 1, static_cast<size_t>(0.9 * static_cast<double>(e.size())))];
    s.wrong = static_cast<double>(std::count_if(e.begin(), e.end(), [](double v) { return v > WRONG_DEG; })) /
              static_cast<double>(e.size());
    return s;
}

// fit on one half of the aircraft, bearing errors of the other half (el <= EVAL_MAX_EL_DEG)
void cross_check(const std::vector<const Sample*>& fit_on, const std::vector<const Sample*>& test, const Geo& g,
                 std::vector<double>* before, std::vector<double>* after) {
    const Solved r = solve(fit_on, g, coarse_init(fit_on, g));
    Steer c0, c1;
    const Params nominal;
    for (const Sample* s : test) {
        if (s->el * 180 / M_PI > EVAL_MAX_EL_DEG) continue;
        const double truth = s->theta * 180 / M_PI;
        before->push_back(std::remainder(bearing_deg(g, nominal, false, *s, c0) - truth, 360.0));
        after->push_back(std::remainder(bearing_deg(g, r.p, true, *s, c1) - truth, 360.0));
    }
}

int count_aircraft(const std::vector<const Sample*>& S) {
    std::set<uint64_t> a;
    for (const Sample* s : S) a.insert(s->ac);
    return static_cast<int>(a.size());
}

// Line of sight: every aircraft's bearing (the nominal array, at its own
// elevation) against its reported position. A blocked / reflected path - an
// aircraft taxiing behind buildings, low behind terrain - gives a bearing
// that is wrong CONSISTENTLY, which the per-packet weights can't tell from an
// array error (a whole airport of them bent a fit to match). Such aircraft
// are left out whole: those more than LOS_DEG off the consensus - the error
// that aircraft in the most DIRECTIONS agree on (an array or heading error
// shows the same way all around; a reflection only in one place - a busy
// airport's aircraft, all reflected alike, would out-vote a few good ones).
std::vector<const Sample*> line_of_sight(const std::vector<const Sample*>& S, const Geo& g,
                                         const std::map<uint64_t, std::string>& ids, std::vector<LeftOut>* left) {
    std::map<uint64_t, std::vector<double>> errs;
    std::map<uint64_t, int> sector;   // aircraft -> 10 deg compass sector (of its first packet)
    {
        Steer c;
        const Params nominal;
        for (const Sample* s : S) {
            errs[s->ac].push_back(
                std::remainder(bearing_deg(g, nominal, false, *s, c, s->el) - s->theta * 180 / M_PI, 360.0));
            sector.emplace(s->ac, static_cast<int>(s->az_deg / 10) % 36);
        }
    }
    std::map<uint64_t, std::pair<double, int>> ae;   // aircraft -> (its median error, packets)
    for (auto& [ac, v] : errs) {
        double sx = 0, sy = 0;
        for (double e : v) {
            sx += std::cos(e * M_PI / 180);
            sy += std::sin(e * M_PI / 180);
        }
        const double m = std::atan2(sy, sx) * 180 / M_PI;   // circular mean, then the median around it
        std::vector<double> d;
        for (double e : v) d.push_back(std::remainder(e - m, 360.0));
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        ae[ac] = {std::remainder(m + d[d.size() / 2], 360.0), static_cast<int>(v.size())};
    }
    // the consensus: the offset aircraft in the most sectors lie within LOS_DEG of
    // (then the most aircraft), then their median
    int best_sec = -1, best_n = -1, best_c = 0;
    for (int c = -180; c < 180; c++) {
        std::set<int> secs;
        int k = 0;
        for (const auto& [ac, e] : ae)
            if (std::fabs(std::remainder(e.first - c, 360.0)) <= LOS_DEG) {
                secs.insert(sector[ac]);
                k++;
            }
        const int ns = static_cast<int>(secs.size());
        if (ns > best_sec || (ns == best_sec && k > best_n)) {
            best_sec = ns;
            best_n = k;
            best_c = c;
        }
    }
    std::vector<double> in;
    for (const auto& [ac, e] : ae)
        if (std::fabs(std::remainder(e.first - best_c, 360.0)) <= LOS_DEG) in.push_back(std::remainder(e.first - best_c, 360.0));
    std::nth_element(in.begin(), in.begin() + in.size() / 2, in.end());
    const double cons = best_c + (in.empty() ? 0.0 : in[in.size() / 2]);
    std::set<uint64_t> drop;
    for (const auto& [ac, e] : ae)
        if (std::fabs(std::remainder(e.first - cons, 360.0)) > LOS_DEG) {
            drop.insert(ac);
            auto it = ids.find(ac);
            left->push_back({it != ids.end() ? it->second : "?", std::remainder(e.first - cons, 360.0), e.second});
        }
    std::sort(left->begin(), left->end(), [](const LeftOut& a, const LeftOut& b) { return a.n > b.n; });
    std::vector<const Sample*> keep;
    for (const Sample* s : S)
        if (!drop.count(s->ac)) keep.push_back(s);
    return keep;
}

Fit run_fit(const std::vector<Sample>& all, const Geo& g, const std::map<uint64_t, std::string>& ids) {
    Fit f;
    f.g = g;
    f.t_ms = now_ms();
    f.samples = static_cast<int>(all.size());
    std::vector<const Sample*> S;
    for (const Sample& s : all) S.push_back(&s);
    f.aircraft = count_aircraft(S);
    if (all.size() < MIN_FIT_SAMPLES || f.aircraft < MIN_FIT_AIRCRAFT) {
        f.error = "not enough aircraft yet";
        return f;
    }
    S = line_of_sight(S, g, ids, &f.left_out);
    f.left_out_n = static_cast<int>(f.left_out.size());
    f.aircraft_used = count_aircraft(S);
    if (S.size() < MIN_FIT_SAMPLES || f.aircraft_used < MIN_FIT_AIRCRAFT) {
        f.error = "not enough aircraft agree with their reported positions yet";
        return f;
    }
    // coverage: 30 deg sectors with enough packets; elevations; frequency
    int per[12] = {};
    f.el_min = 90;
    f.el_max = -90;
    std::vector<double> fs;
    for (const Sample* sp : S) {
        const Sample& s = *sp;
        per[static_cast<int>(s.az_deg / 30) % 12]++;
        f.el_min = std::min(f.el_min, s.el * 180 / M_PI);
        f.el_max = std::max(f.el_max, s.el * 180 / M_PI);
        fs.push_back(s.f);
    }
    for (int c : per) f.sectors += c >= GOOD_SECTOR_PACKETS;
    std::nth_element(fs.begin(), fs.begin() + fs.size() / 2, fs.end());
    f.f_hz = fs[fs.size() / 2];
    // cross-check: two folds by aircraft
    std::vector<const Sample*> f0, f1;
    for (const Sample* s : S) ((s->ac * 0x9E3779B97F4A7C15ull) >> 63 ? f1 : f0).push_back(s);
    if (f0.size() >= MIN_FOLD_SAMPLES && f1.size() >= MIN_FOLD_SAMPLES && count_aircraft(f0) >= 2 &&
        count_aircraft(f1) >= 2) {
        std::vector<double> before, after;
        cross_check(f0, f1, g, &before, &after);
        cross_check(f1, f0, g, &before, &after);
        f.cv = !before.empty();
        f.before = stat_of(before);
        f.after = stat_of(after);
    }
    const Solved r = solve(S, g, coarse_init(S, g));
    f.p = r.p;
    for (int i = 0; i < g.n; i++) {
        f.resid[i] = r.resid[i];
        f.gain_db[i] = r.gain_db[i];
    }
    f.resid_all = r.resid_all;
    f.used = r.used;
    f.ok = true;
    if (f.sectors < 6)
        f.warn = "aircraft from " + std::to_string(f.sectors) +
                 " of 12 directions only: the corrections hold near those - collect longer";
    if (f.resid_all > RESID_WARN_DEG) {
        char b[200];
        snprintf(b, sizeof b, "%sa large part (%.0f° rms) is not explained: multipath, antennas that differ, or a "
                 "wrong element order / radius", f.warn.empty() ? "" : "; ", f.resid_all);
        f.warn += b;
    }
    if (f.cv && f.after.med > f.before.med)
        f.warn += std::string(f.warn.empty() ? "" : "; ") + "the cross-check got worse - don't apply this one";
    return f;
}

void worker() {
    uint64_t done_epoch = 0, done_changes = 0;
    int64_t last = 0;
    std::unique_lock<std::mutex> lk(mu);
    while (!stop_.load()) {
        cv_.wait_for(lk, std::chrono::seconds(1), [] { return stop_.load() || kick_; });
        if (stop_.load()) break;
        const int64_t t = now_ms();
        const bool changed = epoch_ != done_epoch || changes_ != done_changes;
        const bool due = changed && nsamples_ >= MIN_FIT_SAMPLES && (kick_ || t - last >= FIT_EVERY_MS);
        kick_ = false;
        if (!due || !col_have_g_) continue;
        std::vector<Sample> S;
        S.reserve(nsamples_);
        for (const auto& v : sec_) S.insert(S.end(), v.begin(), v.end());
        const Geo g = col_g_;
        const std::map<uint64_t, std::string> ids = ids_;
        const uint64_t ep = epoch_;
        done_epoch = ep;
        done_changes = changes_;
        last = t;
        fitting_ = true;
        lk.unlock();
        Fit f = run_fit(S, g, ids);
        lk.lock();
        fitting_ = false;
        // (not when the collection started over, or the array settings changed, meanwhile)
        if (ep == epoch_ && same_geo(f.g, col_g_)) {
            fit_ = std::move(f);
            if (note_ == REFIT_NOTE) note_.clear();
        }
    }
}

// --- the file -----------------------------------------------------------------------
std::string num(double v, const char* f = "%.10g") {
    if (!is_finite_value(v)) return "null";
    char b[40];
    snprintf(b, sizeof b, f, v);
    return b;
}
template <class F>
std::string arr(int n, F get, const char* f = "%.10g") {
    std::string s = "[";
    for (int i = 0; i < n; i++) s += (i ? "," : "") + num(get(i), f);
    return s + "]";
}
std::string stat_json(const Stat& s) {
    if (!s.n) return "null";
    return "{\"n\":" + std::to_string(s.n) + ",\"med\":" + num(s.med, "%.2f") + ",\"p90\":" + num(s.p90, "%.2f") +
           ",\"wrong\":" + num(s.wrong, "%.4f") + "}";
}

void save_locked() {
    const Fit& f = app_.fit;
    const int n = f.g.n;
    std::ostringstream o;
    o << "{\"version\":1,\"enabled\":" << (app_.enabled ? "true" : "false")
      << ",\"rotation\":" << (app_.rotation ? "true" : "false") << ",\"t\":" << f.t_ms << ",\"f_hz\":" << num(f.f_hz)
      << ",\"n\":" << n << ",\"x\":" << arr(n, [&](int i) { return f.g.x[i]; })
      << ",\"y\":" << arr(n, [&](int i) { return f.g.y[i]; }) << ",\"z\":" << arr(n, [&](int i) { return f.g.z[i]; })
      << ",\"rot_rad\":" << num(f.p.rot) << ",\"tau_s\":" << arr(n, [&](int i) { return f.p.tau[i]; })
      << ",\"dx_m\":" << arr(n, [&](int i) { return f.p.dx[i]; })
      << ",\"dy_m\":" << arr(n, [&](int i) { return f.p.dy[i]; })
      << ",\"resid\":" << arr(n, [&](int i) { return f.resid[i]; }) << ",\"resid_all\":" << num(f.resid_all)
      << ",\"gain_db\":" << arr(n, [&](int i) { return f.gain_db[i]; }) << ",\"samples\":" << f.samples
      << ",\"used\":" << f.used << ",\"aircraft\":" << f.aircraft << ",\"aircraft_used\":" << f.aircraft_used
      << ",\"left_out_n\":" << f.left_out_n << ",\"sectors\":" << f.sectors
      << ",\"el_min\":" << num(f.el_min) << ",\"el_max\":" << num(f.el_max)
      << ",\"cv\":" << (f.cv ? "true" : "false") << ",\"before\":" << stat_json(f.before)
      << ",\"after\":" << stat_json(f.after) << ",\"warn\":\"" << json_escape(f.warn) << "\"}\n";
    const std::string tmp = std::string(CAL_FILE) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            std::cerr << "Array calibration: can't write " << tmp << std::endl;
            return;
        }
        out << o.str();
        if (!out.good()) {
            std::cerr << "Array calibration: writing " << tmp << " failed" << std::endl;
            return;
        }
    }
    if (std::rename(tmp.c_str(), CAL_FILE) != 0) std::cerr << "Array calibration: can't save " << CAL_FILE << std::endl;
}

bool get_arr(const geo::Json& j, const char* key, int n, double* out, double lim) {
    const geo::Json* a = j.get(key);
    if (!a || a->type != geo::Json::ARR || static_cast<int>(a->a.size()) != n) return false;
    for (int i = 0; i < n; i++) {
        if (a->a[static_cast<size_t>(i)].type != geo::Json::NUM) return false;
        out[i] = a->a[static_cast<size_t>(i)].n;
        if (!is_finite_value(out[i]) || std::fabs(out[i]) > lim) return false;
    }
    return true;
}
Stat get_stat(const geo::Json& j, const char* key) {
    Stat s;
    const geo::Json* o = j.get(key);
    if (!o || o->type != geo::Json::OBJ) return s;
    auto g = [&](const char* k) { const geo::Json* v = o->get(k); return v && v->type == geo::Json::NUM ? v->n : 0.0; };
    s.n = static_cast<int>(g("n"));
    s.med = g("med");
    s.p90 = g("p90");
    s.wrong = g("wrong");
    return s;
}

void load_locked() {
    std::ifstream in(CAL_FILE);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    geo::Json j;
    if (!geo::json_parse(ss.str(), &j) || j.type != geo::Json::OBJ) {
        std::cerr << "Array calibration: " << CAL_FILE << " unreadable - ignored" << std::endl;
        return;
    }
    auto numv = [&](const char* k, double d) { const geo::Json* v = j.get(k); return v && v->type == geo::Json::NUM ? v->n : d; };
    auto boolv = [&](const char* k, bool d) { const geo::Json* v = j.get(k); return v && v->type == geo::Json::BOOL ? v->b : d; };
    Applied a;
    Fit& f = a.fit;
    const int n = static_cast<int>(numv("n", 0));
    if (n < 3 || n > MAXN) {
        std::cerr << "Array calibration: " << CAL_FILE << " has no valid element count - ignored" << std::endl;
        return;
    }
    f.g.n = n;
    double tau[MAXN], dx[MAXN], dy[MAXN];
    if (!get_arr(j, "x", n, f.g.x, 100) || !get_arr(j, "y", n, f.g.y, 100) || !get_arr(j, "z", n, f.g.z, 100) ||
        !get_arr(j, "tau_s", n, tau, 1e-7) || !get_arr(j, "dx_m", n, dx, 0.5) || !get_arr(j, "dy_m", n, dy, 0.5)) {
        std::cerr << "Array calibration: " << CAL_FILE << " incomplete - ignored" << std::endl;
        return;
    }
    for (int i = 0; i < n; i++) {
        f.p.tau[i] = tau[i];
        f.p.dx[i] = dx[i];
        f.p.dy[i] = dy[i];
    }
    f.p.rot = numv("rot_rad", 0);
    if (!is_finite_value(f.p.rot) || std::fabs(f.p.rot) > 2 * M_PI) return;
    get_arr(j, "resid", n, f.resid, 1e6);
    get_arr(j, "gain_db", n, f.gain_db, 1e6);
    f.resid_all = numv("resid_all", 0);
    f.t_ms = static_cast<int64_t>(numv("t", 0));
    f.f_hz = numv("f_hz", 0);
    f.samples = static_cast<int>(numv("samples", 0));
    f.used = static_cast<int>(numv("used", 0));
    f.aircraft = static_cast<int>(numv("aircraft", 0));
    f.aircraft_used = static_cast<int>(numv("aircraft_used", f.aircraft));
    f.left_out_n = static_cast<int>(std::clamp(numv("left_out_n", 0), 0.0, 100000.0));
    f.sectors = static_cast<int>(numv("sectors", 0));
    f.el_min = numv("el_min", 0);
    f.el_max = numv("el_max", 0);
    f.cv = boolv("cv", false);
    f.before = get_stat(j, "before");
    f.after = get_stat(j, "after");
    f.warn = j.str("warn");
    f.ok = true;
    a.have = true;
    a.enabled = boolv("enabled", true);
    a.rotation = boolv("rotation", true);
    app_ = a;
    std::cout << "Array calibration loaded (" << n << " elements, " << (a.enabled ? "in use" : "off") << ")" << std::endl;
}

std::string fit_json(const Fit& f) {
    const int n = f.g.n;
    std::ostringstream o;
    o << "{\"ok\":" << (f.ok ? "true" : "false") << ",\"error\":\"" << json_escape(f.error) << "\",\"t\":" << f.t_ms
      << ",\"age_s\":" << num((now_ms() - f.t_ms) / 1000.0, "%.0f") << ",\"samples\":" << f.samples
      << ",\"aircraft\":" << f.aircraft << ",\"aircraft_used\":" << f.aircraft_used << ",\"left_out\":[";
    for (size_t i = 0; i < f.left_out.size() && i < 20; i++)
        o << (i ? "," : "") << "{\"id\":\"" << json_escape(f.left_out[i].id) << "\",\"err\":" << num(f.left_out[i].err, "%.0f")
          << ",\"n\":" << f.left_out[i].n << "}";
    o << "],\"left_out_n\":" << f.left_out_n;
    if (f.ok) {
        o << ",\"f_mhz\":" << num(f.f_hz / 1e6, "%.3f") << ",\"n\":" << n << ",\"used\":" << f.used
          << ",\"sectors\":" << f.sectors << ",\"el\":[" << num(f.el_min, "%.1f")
          << "," << num(f.el_max, "%.1f") << "],\"rot\":" << num(f.p.rot * 180 / M_PI, "%.2f")
          << ",\"tau_ps\":" << arr(n, [&](int i) { return f.p.tau[i] * 1e12; }, "%.1f")
          << ",\"dx_mm\":" << arr(n, [&](int i) { return f.p.dx[i] * 1e3; }, "%.1f")
          << ",\"dy_mm\":" << arr(n, [&](int i) { return f.p.dy[i] * 1e3; }, "%.1f")
          << ",\"resid\":" << arr(n, [&](int i) { return f.resid[i]; }, "%.1f")
          << ",\"resid_all\":" << num(f.resid_all, "%.1f")
          << ",\"gain_db\":" << arr(n, [&](int i) { return f.gain_db[i]; }, "%.2f")
          << ",\"cv\":" << (f.cv ? "true" : "false") << ",\"before\":" << stat_json(f.before)
          << ",\"after\":" << stat_json(f.after) << ",\"warn\":\"" << json_escape(f.warn) << "\"";
    }
    o << "}";
    return o.str();
}

}  // namespace

// --- in use -------------------------------------------------------------------------
uint64_t generation() { return gen.load(std::memory_order_relaxed); }

bool corrections(int n, const double* x, const double* y, double* dx, double* dy, double* tau_s) {
    std::lock_guard<std::mutex> lk(mu);
    if (!app_.have || !app_.enabled || app_.fit.g.n != n || n > MAXN) return false;
    const Fit& f = app_.fit;
    for (int i = 0; i < n; i++)
        if (std::fabs(f.g.x[i] - x[i]) > MATCH_TOL_M || std::fabs(f.g.y[i] - y[i]) > MATCH_TOL_M) return false;
    const double cr = app_.rotation ? std::cos(f.p.rot) : 1.0, sr = app_.rotation ? std::sin(f.p.rot) : 0.0;
    for (int i = 0; i < n; i++) {
        dx[i] = cr * x[i] - sr * y[i] + f.p.dx[i] - x[i];
        dy[i] = sr * x[i] + cr * y[i] + f.p.dy[i] - y[i];
        tau_s[i] = f.p.tau[i];
    }
    return true;
}

// --- collecting ---------------------------------------------------------------------
bool collecting() { return collect_on.load(std::memory_order_relaxed); }

void set_state(const char* state) {
    std::lock_guard<std::mutex> lk(mu);
    state_ = state;
}

void note_array(int n, const double* x, const double* y, const double* z) {
    Geo g;
    g.n = std::clamp(n, 0, MAXN);
    for (int i = 0; i < g.n; i++) {
        g.x[i] = x[i];
        g.y[i] = y[i];
        g.z[i] = z[i];
    }
    std::lock_guard<std::mutex> lk(mu);
    arr_ = g;
    arr_known_ = true;
    if (g.n < 3) return;
    if (col_have_g_ && g.n != col_g_.n) {
        if (nsamples_) note_ = "The element count changed - the collection started over";
        clear_samples_locked();
    } else if (col_have_g_ && !same_geo(g, col_g_)) {
        // the packets still hold: fitted again against the new positions
        fit_ = Fit{};
        if (nsamples_) {
            note_ = REFIT_NOTE;
            changes_++;
            kick_ = true;
            cv_.notify_all();
        }
    }
    col_g_ = g;
    col_have_g_ = true;
}

void add(const Eigen::MatrixXcd& R, const std::string& id, double lat, double lon, double alt_m, double st_lat,
         double st_lon, double st_alt_m, double heading_deg, bool heading_fixed, double offset_deg, double freq_hz) {
    if (!collecting()) return;
    const int n = static_cast<int>(R.rows());
    if (n < 3 || n > MAXN || R.cols() != n || !(freq_hz > 0)) return;
    const uint64_t ac = std::hash<std::string>{}(id);
    const int64_t t = now_ms();
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!col_have_g_ || col_g_.n != n) return;
        auto it = last_by_ac_.find(ac);
        if (it != last_by_ac_.end() && t - it->second < PER_AIRCRAFT_MS) return;
    }
    // where the aircraft is, seen from the station (east / north / up)
    double a[3], b[3];
    to_ecef(st_lat, st_lon, st_alt_m, a);
    to_ecef(lat, lon, alt_m, b);
    const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const double fl = st_lat * M_PI / 180, ll = st_lon * M_PI / 180;
    const double e = -std::sin(ll) * dx + std::cos(ll) * dy;
    const double nn = -std::sin(fl) * std::cos(ll) * dx - std::sin(fl) * std::sin(ll) * dy + std::cos(fl) * dz;
    const double up = std::cos(fl) * std::cos(ll) * dx + std::cos(fl) * std::sin(ll) * dy + std::sin(fl) * dz;
    const double horiz = std::hypot(e, nn);
    const double dist_km = horiz / 1000;
    const double el = std::atan2(up, horiz) * 180 / M_PI;
    double az = std::atan2(e, nn) * 180 / M_PI;
    if (az < 0) az += 360;
    // the packet's response: dominant eigenvector + how far it stands out
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> es(R);
    double snr = 0;
    Sample s;
    if (es.info() == Eigen::Success) {
        const auto& ev = es.eigenvalues();
        double rest = 0;
        for (int i = 0; i < n - 1; i++) rest += std::max(0.0, ev(i));
        rest /= (n - 1);
        snr = ev(n - 1) / std::max(rest, 1e-12);
        const Eigen::VectorXcd v = es.eigenvectors().col(n - 1).normalized();
        for (int i = 0; i < n; i++) s.u[i] = v(i);
    }
    std::lock_guard<std::mutex> lk(mu);
    if (!collecting() || !col_have_g_ || col_g_.n != n) return;
    // the angle reference the earlier packets were taken with (a compass
    // heading varies a little; a static one only when the user edits it)
    if (nsamples_) {
        const char* why = nullptr;
        if (std::fabs(std::remainder(offset_deg - col_offset_, 360.0)) > 0.5) why = "The array offset changed";
        else if (heading_fixed != col_fixed_) why = "The heading source changed";
        else if (heading_fixed && std::fabs(std::remainder(heading_deg - col_heading_, 360.0)) > 0.5)
            why = "The station heading changed";
        if (why) {
            clear_samples_locked();
            note_ = std::string(why) + " - the collection started over";
        }
    }
    if (!nsamples_) {
        col_offset_ = offset_deg;
        col_heading_ = heading_deg;
        col_fixed_ = heading_fixed;
    }
    const double ref_deg = heading_deg - offset_deg;
    if (dist_km < MIN_DIST_KM) { rej_.near++; return; }
    if (dist_km > MAX_DIST_KM) { rej_.far++; return; }
    if (el < MIN_EL_DEG) { rej_.low++; return; }
    if (el > MAX_EL_DEG) { rej_.high++; return; }
    if (!is_finite_value(snr) || snr < MIN_SNR) { rej_.weak++; return; }
    s.theta = std::remainder(ref_deg - az, 360.0) * M_PI / 180;
    s.el = el * M_PI / 180;
    s.f = freq_hz;
    s.az_deg = az;
    s.dist_km = dist_km;
    s.snr = snr;
    s.ac = ac;
    last_by_ac_[ac] = t;
    if (!ids_.count(ac)) ids_[ac] = id.substr(0, 16);
    if (last_by_ac_.size() > 5000)
        for (auto it = last_by_ac_.begin(); it != last_by_ac_.end();)
            it = t - it->second > 600000 ? last_by_ac_.erase(it) : std::next(it);
    auto& v = sec_[static_cast<size_t>(az / 10) % SECTORS];
    if (v.size() < SECTOR_CAP) {
        v.push_back(s);
        nsamples_++;
    } else {
        v[next_rand() % v.size()] = s;   // full: a random old one makes room (keeps hours of traffic mixed)
    }
    changes_++;
    last_sample_ms_ = t;
}

// --- control ----------------------------------------------------------------------------
void start() {
    {
        std::lock_guard<std::mutex> lk(mu);
        load_locked();
    }
    gen++;
    stop_ = false;
    worker_ = std::thread(worker);
}

void stop() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void set_collect(bool on) {
    collect_on = on;
    std::lock_guard<std::mutex> lk(mu);
    if (on) note_.clear();
    else kick_ = true;   // a last fit with everything collected
    cv_.notify_all();
}

void reset_samples() {
    std::lock_guard<std::mutex> lk(mu);
    clear_samples_locked();
    note_.clear();
}

bool apply(std::string* err) {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!fit_.ok) {
            *err = "no calibration result yet";
            return false;
        }
        const bool rot = app_.have ? app_.rotation : true;
        app_ = Applied{};
        app_.have = true;
        app_.enabled = true;
        app_.rotation = rot;
        app_.fit = fit_;
        save_locked();
    }
    gen++;
    std::cout << "Array calibration applied" << std::endl;
    return true;
}

void set_use(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!app_.have || app_.enabled == on) return;
        app_.enabled = on;
        save_locked();
    }
    gen++;
}

void set_rotation(bool on) {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!app_.have || app_.rotation == on) return;
        app_.rotation = on;
        save_locked();
    }
    gen++;
}

void remove() {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!app_.have) return;
        app_ = Applied{};
        std::remove(CAL_FILE);
    }
    gen++;
    std::cout << "Array calibration removed" << std::endl;
}

std::string status_message() {
    std::lock_guard<std::mutex> lk(mu);
    std::set<uint64_t> acs;
    for (const auto& v : sec_)
        for (const Sample& s : v) acs.insert(s.ac);
    std::ostringstream o;
    o << "{\"array_cal\":{\"collecting\":" << (collecting() ? "true" : "false") << ",\"state\":\""
      << json_escape(state_) << "\",\"samples\":" << nsamples_ << ",\"aircraft\":" << acs.size() << ",\"last_s\":"
      << (last_sample_ms_ ? num((now_ms() - last_sample_ms_) / 1000.0, "%.1f") : "null") << ",\"rejected\":{\"near\":"
      << rej_.near << ",\"far\":" << rej_.far << ",\"low\":" << rej_.low << ",\"high\":" << rej_.high
      << ",\"weak\":" << rej_.weak
      << "},\"sectors\":[";
    for (int i = 0; i < SECTORS; i++) o << (i ? "," : "") << sec_[static_cast<size_t>(i)].size();
    o << "],\"cap\":" << SECTOR_CAP << ",\"note\":\"" << json_escape(note_) << "\",\"array\":";
    if (arr_known_) o << "{\"n\":" << arr_.n << ",\"ok\":" << (arr_.n >= 3 ? "true" : "false") << "}";
    else o << "null";
    o << ",\"fitting\":" << (fitting_ ? "true" : "false") << ",\"min_samples\":" << MIN_FIT_SAMPLES
      << ",\"fit\":" << (fit_.ok || !fit_.error.empty() ? fit_json(fit_) : "null") << ",\"applied\":";
    if (app_.have) {
        std::string j = fit_json(app_.fit);
        j.pop_back();
        const bool match = arr_known_ && same_geo(arr_, app_.fit.g);
        o << j << ",\"enabled\":" << (app_.enabled ? "true" : "false")
          << ",\"rotation\":" << (app_.rotation ? "true" : "false") << ",\"match\":"
          << (arr_known_ ? (match ? "true" : "false") : "null") << "}";
    } else {
        o << "null";
    }
    o << "}}";
    return o.str();
}

}  // namespace array_cal
