#include "rdf_engine.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <numeric>

namespace rdf {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double D2R = PI / 180.0, R2D = 180.0 / PI;
constexpr double EARTH_R = 6371008.8;

double wrap360(double a) {
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}
}  // namespace

// ---------------------------------------------------------------- geodesy

double distance_m(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * D2R, p2 = lat2 * D2R, dp = p2 - p1, dl = (lon2 - lon1) * D2R;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * EARTH_R * std::asin(std::min(1.0, std::sqrt(a)));
}

double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return wrap360(std::atan2(y, x) * R2D);
}

void to_enu(double lat0, double lon0, double lat, double lon, double* e, double* n) {
    const double d = distance_m(lat0, lon0, lat, lon);
    const double b = bearing_deg(lat0, lon0, lat, lon) * D2R;
    *e = d * std::sin(b);
    *n = d * std::cos(b);
}

void from_enu(double lat0, double lon0, double e, double n, double* lat, double* lon) {
    const double d = std::hypot(e, n) / EARTH_R, b = std::atan2(e, n);
    const double p1 = lat0 * D2R, l1 = lon0 * D2R;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    *lat = p2 * R2D;
    *lon = std::fmod(l2 * R2D + 540.0, 360.0) - 180.0;
}

// ---------------------------------------------------------------- lobes

bool north_lobe(const float* spec, int n, double res_deg, double heading_deg, double sigma_deg, Lobe* out) {
    if (!spec || n < 8 || !(res_deg > 0)) return false;
    float mx = 0;
    for (int i = 0; i < n; i++) {
        if (!(spec[i] >= 0) || spec[i] > 3e38f) return false;
        mx = std::max(mx, spec[i]);
    }
    if (!(mx > 0)) return false;
    // bin k = compass bearing tau; the array sees it at unit-circle angle
    // heading - tau (its forward axis = the heading, angles counter-clockwise)
    std::array<double, BINS> v;
    for (int k = 0; k < BINS; k++) {
        const double b = wrap360(heading_deg - k * BIN_DEG);
        const double x = b / res_deg;
        int i0 = static_cast<int>(x);
        const double f = x - i0;
        i0 %= n;
        v[k] = (spec[i0] * (1 - f) + spec[(i0 + 1) % n] * f) / mx;
    }
    // bearing uncertainty: circular Gaussian blur
    const double sb = std::max(sigma_deg, 0.25) / BIN_DEG;
    const int half = static_cast<int>(std::ceil(3 * sb));
    std::vector<double> ker(2 * half + 1);
    double ks = 0;
    for (int j = -half; j <= half; j++) ks += ker[j + half] = std::exp(-0.5 * j * j / (sb * sb));
    double tot = 0;
    for (int k = 0; k < BINS; k++) {
        double s = 0;
        for (int j = -half; j <= half; j++) s += ker[j + half] * v[(k + j + BINS) % BINS];
        (*out)[k] = static_cast<float>(s / ks);
        tot += s / ks;
    }
    if (!(tot > 0) || !std::isfinite(tot)) return false;
    for (auto& x : *out) x = static_cast<float>(x / tot);
    return true;
}

double lobe_peak_deg(const Lobe& p) {
    int m = 0;
    for (int k = 1; k < BINS; k++)
        if (p[k] > p[m]) m = k;
    const double a = p[(m + BINS - 1) % BINS], b = p[m], c = p[(m + 1) % BINS];
    const double den = a - 2 * b + c;
    const double d = den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return wrap360((m + d) * BIN_DEG);
}

// ---------------------------------------------------------------- gate

void Gate::reset() {
    n_ = 0;
    slat_ = slon_ = sconf_ = 0;
    tsum_ = 0;
    sum_.fill(0);
    moved_m_ = 0;
}

bool Gate::add(int64_t t_ms, double lat, double lon, const Lobe& p, float conf, double est_range_m, Record* out) {
    if (n_ > 0 && t_ms - tlast_ > cfg_.max_gap_s * 1000) reset();   // a pause: the window's geometry is lost
    if (n_ == 0) {
        lat0_ = lat;
        lon0_ = lon;
        t0_ = t_ms;
        const double R = est_range_m > 0 ? est_range_m : cfg_.default_range_m;
        gate_m_ = std::clamp(R * std::tan(cfg_.gate_sigma_deg * D2R), cfg_.min_gate_m, cfg_.max_gate_m);
    }
    // repeated views of one bearing multiply: sum the per-frame log
    // probabilities (with an outlier floor, so one bad frame can't veto)
    for (int k = 0; k < BINS; k++) sum_[k] += std::log(0.95 * p[k] + 0.05 / BINS);
    slat_ += lat;
    slon_ += lon;
    sconf_ += conf;
    tsum_ += t_ms - t0_;
    n_++;
    tlast_ = t_ms;
    moved_m_ = distance_m(lat0_, lon0_, lat, lon);
    if (moved_m_ >= gate_m_ && n_ >= cfg_.min_frames) {
        out->t_ms = t0_ + tsum_ / n_;
        out->lat = slat_ / n_;
        out->lon = slon_ / n_;
        out->conf = static_cast<float>(sconf_ / n_);
        out->frames = n_;
        // the window's bearing: normalised geometric mean of its frames
        const double mx = *std::max_element(sum_.begin(), sum_.end());
        double tot = 0;
        for (int k = 0; k < BINS; k++) tot += std::exp((sum_[k] - mx) / n_);
        for (int k = 0; k < BINS; k++) out->p[k] = static_cast<float>(std::exp((sum_[k] - mx) / n_) / tot);
        reset();
        return true;
    }
    if (t_ms - t0_ > cfg_.max_window_s * 1000) reset();   // crawling / circling on the spot
    return false;
}

// ---------------------------------------------------------------- grid

Grid::Grid(const Options& o, double lat0, double lon0, double half_m, int n)
    : o_(o), lat0_(lat0), lon0_(lon0), half_(half_m), cell_(2 * half_m / n), n_(n),
      k_(static_cast<int>(o.offsets_deg.size())) {
    if (k_ == 0) {
        o_.offsets_deg = {0};
        k_ = 1;
    }
    L_.assign(static_cast<size_t>(k_) * n_ * n_, 0.f);
    e_.resize(static_cast<size_t>(n_) * n_);
    nn_.resize(e_.size());
    for (int j = 0; j < n_; j++)
        for (int i = 0; i < n_; i++) {
            e_[j * n_ + i] = static_cast<float>(-half_ + (i + 0.5) * cell_);
            nn_[j * n_ + i] = static_cast<float>(-half_ + (j + 0.5) * cell_);
        }
    double ls = 0;
    for (double d : o_.offsets_deg) {
        const double sd = std::max(o_.offset_prior_sd_deg, 0.1);
        lprior_.push_back(-0.5 * d * d / (sd * sd));
    }
    double mx = *std::max_element(lprior_.begin(), lprior_.end());
    for (double& x : lprior_) ls += std::exp(x - mx);
    for (double& x : lprior_) x -= mx + std::log(ls);
}

void Grid::clear() {
    std::fill(L_.begin(), L_.end(), 0.f);
    records_ = 0;
}

void Grid::add(const Record& r) {
    // log-likelihood ratio per bin against a uniform bearing
    std::array<float, BINS + 1> ell;
    for (int k = 0; k < BINS; k++)
        ell[k] = static_cast<float>(o_.alpha * std::log((1 - o_.eps) * r.p[k] * BINS + o_.eps));
    ell[BINS] = ell[0];
    const float near_v = static_cast<float>(o_.alpha * o_.near_value);
    // the record in the grid's metric frame; true north there is rotated
    // from grid north by gamma (meridian convergence of the projection)
    double er, nr;
    to_enu(lat0_, lon0_, r.lat, r.lon, &er, &nr);
    double la2, lo2;
    from_enu(lat0_, lon0_, er, nr + 1000.0, &la2, &lo2);
    const double gamma = bearing_deg(r.lat, r.lon, la2, lo2);   // true bearing of grid north
    const float near2 = static_cast<float>(std::pow(std::max(o_.near_m, 1.5 * cell_), 2));
    const size_t cells = static_cast<size_t>(n_) * n_;
    std::vector<float> xoff(k_);
    for (int k = 0; k < k_; k++) xoff[k] = static_cast<float>((gamma - o_.offsets_deg[k]) / BIN_DEG);
    const float fer = static_cast<float>(er), fnr = static_cast<float>(nr);
    constexpr float INV_BIN = static_cast<float>(BINS / (2 * PI));
    for (size_t c = 0; c < cells; c++) {
        const float dx = e_[c] - fer, dy = nn_[c] - fnr;
        float* lp = &L_[c];
        if (dx * dx + dy * dy < near2) {
            for (int k = 0; k < k_; k++) lp[k * cells] += near_v;
            continue;
        }
        // grid bearing (rad -> bins) + per offset: the lobe at (bearing - offset)
        const float xb = std::atan2(dx, dy) * INV_BIN;
        for (int k = 0; k < k_; k++) {
            float x = xb + xoff[k];
            x -= BINS * std::floor(x / BINS);
            int i = static_cast<int>(x);
            if (i >= BINS) i -= BINS;
            float v;
            if (o_.interpolate) {
                const float f = x - i;
                v = ell[i] + f * (ell[i + 1] - ell[i]);
            } else {
                int j = static_cast<int>(x + 0.5f);
                v = ell[j >= BINS ? j - BINS : j];
            }
            lp[k * cells] += v;
        }
    }
    records_++;
}

void Grid::marginal(std::vector<float>* m) const {
    const size_t cells = static_cast<size_t>(n_) * n_;
    m->assign(cells, 0.f);
    if (!o_.marginalize_offsets) {
        // the single best offset (the plane with the highest cell)
        int best = 0;
        float bv = -1e30f;
        for (int k = 0; k < k_; k++) {
            const float v = *std::max_element(L_.begin() + k * cells, L_.begin() + (k + 1) * cells);
            if (v > bv) { bv = v; best = k; }
        }
        std::copy(L_.begin() + best * cells, L_.begin() + (best + 1) * cells, m->begin());
        return;
    }
    for (size_t c = 0; c < cells; c++) {
        double mx = -1e300;
        for (int k = 0; k < k_; k++) mx = std::max(mx, L_[k * cells + c] + lprior_[k]);
        double s = 0;
        for (int k = 0; k < k_; k++) s += std::exp(L_[k * cells + c] + lprior_[k] - mx);
        (*m)[c] = static_cast<float>(mx + std::log(s));
    }
}

Estimate Grid::estimate() const {
    Estimate e;
    e.records = records_;
    if (records_ == 0) return e;
    const size_t cells = static_cast<size_t>(n_) * n_;
    std::vector<float> M;
    marginal(&M);
    const size_t cmax = std::max_element(M.begin(), M.end()) - M.begin();
    const double Mmax = M[cmax];
    const double s = std::min(1.0, o_.ess_cap / (o_.alpha * records_));
    std::vector<double> w(cells);
    double total = 0;
    for (size_t c = 0; c < cells; c++) total += w[c] = std::exp(s * (M[c] - Mmax));
    // the main mode: 8-connected cells around the peak above 1e-4 of it
    std::vector<char> in(cells, 0);
    std::vector<size_t> mode, stack{cmax};
    in[cmax] = 1;
    while (!stack.empty()) {
        const size_t c = stack.back();
        stack.pop_back();
        mode.push_back(c);
        const int i = static_cast<int>(c % n_), j = static_cast<int>(c / n_);
        if (i == 0 || j == 0 || i == n_ - 1 || j == n_ - 1) e.at_edge = true;
        for (int dj = -1; dj <= 1; dj++)
            for (int di = -1; di <= 1; di++) {
                const int a = i + di, b = j + dj;
                if (a < 0 || b < 0 || a >= n_ || b >= n_) continue;
                const size_t q = static_cast<size_t>(b) * n_ + a;
                if (!in[q] && w[q] >= 1e-4) {
                    in[q] = 1;
                    stack.push_back(q);
                }
            }
    }
    double mm = 0, me = 0, mn = 0;
    for (size_t c : mode) {
        mm += w[c];
        me += w[c] * e_[c];
        mn += w[c] * nn_[c];
    }
    me /= mm;
    mn /= mm;
    double cee = 0, cnn = 0, cen = 0;
    for (size_t c : mode) {
        const double de = e_[c] - me, dn = nn_[c] - mn;
        cee += w[c] * de * de;
        cnn += w[c] * dn * dn;
        cen += w[c] * de * dn;
    }
    const double cell_var = cell_ * cell_ / 12.0;   // a single cell is not a point
    cee = cee / mm + cell_var;
    cnn = cnn / mm + cell_var;
    cen /= mm;
    const double tr = cee + cnn, det = cee * cnn - cen * cen;
    const double l1 = tr / 2 + std::sqrt(std::max(0.0, tr * tr / 4 - det)), l2 = std::max(0.0, tr - l1);
    e.ell_a_m = std::sqrt(l1);
    e.ell_b_m = std::sqrt(l2);
    e.ell_ang_deg = wrap360(0.5 * std::atan2(2 * cen, cnn - cee) * R2D);   // major axis, CW from grid north
    // HPD radii (within the mode)
    std::vector<double> ws;
    ws.reserve(mode.size());
    for (size_t c : mode) ws.push_back(w[c]);
    std::sort(ws.begin(), ws.end(), std::greater<double>());
    double acc = 0;
    size_t n50 = 0, n95 = 0;
    for (size_t i = 0; i < ws.size(); i++) {
        acc += ws[i];
        if (!n50 && acc >= 0.5 * mm) n50 = i + 1;
        if (!n95 && acc >= 0.95 * mm) { n95 = i + 1; break; }
    }
    if (!n95) n95 = ws.size();
    if (!n50) n50 = n95;
    e.r50_m = std::sqrt(n50 * cell_ * cell_ / PI);
    e.r95_m = std::sqrt(n95 * cell_ * cell_ / PI);
    e.mode_mass = mm / total;
    from_enu(lat0_, lon0_, me, mn, &e.mean_lat, &e.mean_lon);
    // the point estimate: the most likely cell, refined by a parabola per axis
    // (the mode's mean is pulled along a banana-shaped posterior)
    const int ci = static_cast<int>(cmax % n_), cj = static_cast<int>(cmax / n_);
    auto sub = [&](int di, int dj) {
        const int a = ci - di, b = cj - dj, c = ci + di, d = cj + dj;
        if (a < 0 || b < 0 || c >= n_ || d >= n_ || a >= n_ || b >= n_ || c < 0 || d < 0) return 0.0;
        const double ya = M[b * n_ + a], yb = M[cmax], yc = M[d * n_ + c];
        const double den = ya - 2 * yb + yc;
        return den < 0 ? std::clamp(0.5 * (ya - yc) / den, -0.5, 0.5) : 0.0;
    };
    from_enu(lat0_, lon0_, e_[cmax] + sub(1, 0) * cell_, nn_[cmax] + sub(0, 1) * cell_, &e.lat, &e.lon);
    e.peak_lat = e.lat;
    e.peak_lon = e.lon;
    // the systematic offset: posterior over the planes
    if (k_ > 1) {
        std::vector<double> lp(k_);
        for (int k = 0; k < k_; k++) {
            double s2 = 0;
            for (size_t c = 0; c < cells; c++) s2 += std::exp(s * (L_[k * cells + c] - Mmax));
            lp[k] = (o_.marginalize_offsets ? lprior_[k] : 0.0) + std::log(std::max(s2, 1e-300));
        }
        const double mx = *std::max_element(lp.begin(), lp.end());
        double z = 0, m1 = 0, m2 = 0;
        for (int k = 0; k < k_; k++) {
            const double p = std::exp(lp[k] - mx);
            z += p;
            m1 += p * o_.offsets_deg[k];
            m2 += p * o_.offsets_deg[k] * o_.offsets_deg[k];
        }
        e.offset_deg = m1 / z;
        e.offset_sd_deg = std::sqrt(std::max(0.0, m2 / z - e.offset_deg * e.offset_deg));
    }
    e.valid = true;
    return e;
}

std::vector<uint8_t> Grid::raster(double nats_range) const {
    std::vector<uint8_t> out(static_cast<size_t>(n_) * n_, 0);
    if (!records_) return out;
    std::vector<float> M;
    marginal(&M);
    const float Mmax = *std::max_element(M.begin(), M.end());
    const double s = std::min(1.0, o_.ess_cap / (o_.alpha * records_));
    for (int row = 0; row < n_; row++) {
        const int j = n_ - 1 - row;   // row 0 = north
        for (int i = 0; i < n_; i++) {
            const double v = 1 + s * (M[j * n_ + i] - Mmax) / nats_range;
            out[row * n_ + i] = static_cast<uint8_t>(std::lround(255 * std::clamp(v, 0.0, 1.0)));
        }
    }
    return out;
}

// ---------------------------------------------------------------- solver

namespace {
constexpr int FINE_N = 128;
constexpr size_t MAX_RECORDS = 4000;
}

Solver::Solver(Options o, double half_km, int n) : o_(std::move(o)), half_km_(half_km), n_(n) {}

void Solver::reset() {
    recs_.clear();
    coarse_.reset();
    fine_.reset();
    est_ = {};
    version_++;
}

void Solver::rebuild_coarse(double lat, double lon) {
    coarse_ = std::make_unique<Grid>(o_, lat, lon, half_km_ * 1000, n_);
    for (const auto& r : recs_) coarse_->add(r);
}

void Solver::refresh_estimate() {
    est_ = coarse_ ? coarse_->estimate() : Estimate{};
    if (fine_) {
        Estimate f = fine_->estimate();
        if (f.valid && !f.at_edge) {
            f.mode_mass = std::min(f.mode_mass, est_.mode_mass);   // the fine grid sees only the mode
            est_ = f;
        }
    }
}

void Solver::update_fine(bool force) {
    const Estimate c = coarse_ ? coarse_->estimate() : Estimate{};
    const double ch = half_km_ * 1000;
    const bool confident = c.valid && c.records >= 5 && c.mode_mass > 0.5 && !c.at_edge && c.r95_m < ch / 3;
    if (!confident) {
        fine_.reset();
        return;
    }
    const double want = std::clamp(4 * c.r95_m, 250.0, ch / 3);
    bool rebuild = force || !fine_;
    if (fine_) {
        const double moved = distance_m(fine_->lat0(), fine_->lon0(), c.lat, c.lon);
        const double ratio = want / fine_->half_m();
        if (moved > 0.3 * fine_->half_m() || ratio > 1.6 || ratio < 1 / 1.6) rebuild = true;
    }
    if (!rebuild) return;
    fine_ = std::make_unique<Grid>(o_, c.lat, c.lon, want, FINE_N);
    for (const auto& r : recs_) fine_->add(r);
}

void Solver::add(const Record& r) {
    recs_.push_back(r);
    if (recs_.size() > MAX_RECORDS) {
        // thin the older half (every other record) and start over
        std::vector<Record> keep;
        const size_t half = recs_.size() / 2;
        for (size_t i = 0; i < recs_.size(); i++)
            if (i >= half || i % 2 == 0) keep.push_back(recs_[i]);
        recs_.swap(keep);
        rebuild_coarse(coarse_ ? coarse_->lat0() : r.lat, coarse_ ? coarse_->lon0() : r.lon);
        update_fine(true);
        refresh_estimate();
        version_++;
        return;
    }
    if (!coarse_) {
        rebuild_coarse(r.lat, r.lon);
    } else if (distance_m(coarse_->lat0(), coarse_->lon0(), r.lat, r.lon) > 0.7 * half_km_ * 1000) {
        // the vehicle is leaving the grid: re-centre on a confident estimate, else on the vehicle
        const bool conf = est_.valid && est_.mode_mass > 0.5 && !est_.at_edge;
        rebuild_coarse(conf ? est_.lat : r.lat, conf ? est_.lon : r.lon);
    } else {
        coarse_->add(r);
    }
    if (fine_) fine_->add(r);
    update_fine(false);
    refresh_estimate();
    version_++;
}

void Solver::set_range_km(double half_km) {
    half_km_ = half_km;
    if (!coarse_) return;
    rebuild_coarse(coarse_->lat0(), coarse_->lon0());
    update_fine(true);
    refresh_estimate();
    version_++;
}

void Solver::recentre(double lat, double lon) {
    if (recs_.empty()) return;
    rebuild_coarse(lat, lon);
    update_fine(true);
    refresh_estimate();
    version_++;
}

void Solver::load(std::vector<Record> recs) {
    recs_ = std::move(recs);
    coarse_.reset();
    fine_.reset();
    if (!recs_.empty()) {
        rebuild_coarse(recs_.front().lat, recs_.front().lon);
        update_fine(true);
    }
    refresh_estimate();
    version_++;
}

}  // namespace rdf
