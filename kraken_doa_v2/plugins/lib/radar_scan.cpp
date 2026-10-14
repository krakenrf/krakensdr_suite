#include "radar_scan.hpp"

#include <algorithm>
#include <cmath>

namespace radar {

namespace {
size_t slot(int64_t b) { return static_cast<size_t>(((b % LevelSeries::NB) + LevelSeries::NB) % LevelSeries::NB); }
}

void LevelSeries::add(double t, float db, bool clipped) {
    const int64_t b = static_cast<int64_t>(std::floor(t / BIN_S));
    if (newest_ < 0) newest_ = b;
    if (b > newest_) {
        for (int64_t k = std::max(newest_ + 1, b - NB + 1); k <= b; k++) {
            lv_[slot(k)] = NONE;
            clip_[slot(k)] = 0;
        }
        newest_ = b;
    }
    if (!kept(b)) return;
    const size_t i = slot(b);
    if (db > lv_[i]) lv_[i] = db;
    if (clipped) clip_[i] = 1;
}

float LevelSeries::max_over(double t0, double t1) const {
    if (newest_ < 0) return NONE;
    const int64_t b0 = std::max(static_cast<int64_t>(std::floor(t0 / BIN_S)), newest_ - NB + 1);
    const int64_t b1 = std::min(static_cast<int64_t>(std::floor(t1 / BIN_S)), newest_);
    float m = NONE;
    for (int64_t b = b0; b <= b1; b++) m = std::max(m, lv_[slot(b)]);
    return m;
}

double LevelSeries::dwell_over(double t0, double t1, float level) const {
    if (newest_ < 0) return 0;
    const int64_t b0 = std::max(static_cast<int64_t>(std::floor(t0 / BIN_S)), newest_ - NB + 1);
    const int64_t b1 = std::min(static_cast<int64_t>(std::floor(t1 / BIN_S)), newest_);
    int n = 0;
    for (int64_t b = b0; b <= b1; b++) n += lv_[slot(b)] >= level;
    return n * BIN_S;
}

void LevelSeries::bins(double t0, double t1, std::vector<std::pair<double, float>>& out) const {
    out.clear();
    if (newest_ < 0) return;
    const int64_t b0 = std::max(static_cast<int64_t>(std::floor(t0 / BIN_S)), newest_ - NB + 1);
    const int64_t b1 = std::min(static_cast<int64_t>(std::floor(t1 / BIN_S)), newest_);
    for (int64_t b = b0; b <= b1; b++)
        if (lv_[slot(b)] > NONE) out.emplace_back((static_cast<double>(b) + 0.5) * BIN_S, lv_[slot(b)]);
}

float LevelSeries::median_level() const {
    std::vector<float> v;
    for (float x : lv_)
        if (x > NONE) v.push_back(x);
    if (v.empty()) return NONE;
    auto mid = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

std::vector<Burst> LevelSeries::bursts(double rel_db, double max_gap_s) const {
    std::vector<Burst> out;
    if (newest_ < 0) return out;
    const int64_t first = newest_ - NB + 1;
    float L = NONE;
    for (int64_t b = first; b <= newest_; b++) L = std::max(L, lv_[slot(b)]);
    if (L <= NONE) return out;
    const float thr = L - static_cast<float>(rel_db);
    const int64_t gap = static_cast<int64_t>(std::lround(max_gap_s / BIN_S));
    int64_t a = -1, last = -1;
    auto close = [&](int64_t b0, int64_t b1) {
        Burst u;
        u.from = static_cast<double>(b0) * BIN_S;
        u.to = static_cast<double>(b1 + 1) * BIN_S;
        u.peak_db = NONE;
        double ct = 0;
        int cn = 0;
        for (int64_t b = b0; b <= b1; b++) {
            u.peak_db = std::max(u.peak_db, lv_[slot(b)]);
            if (clip_[slot(b)]) {
                ct += (static_cast<double>(b) + 0.5) * BIN_S;
                cn++;
            }
        }
        if (cn) {
            u.clipped = true;
            u.t = ct / cn;
        } else {
            double sw = 0, st = 0;
            for (int64_t b = b0; b <= b1; b++) {
                const float v = lv_[slot(b)];
                if (v < u.peak_db - 3) continue;
                const double w = std::pow(10.0, (v - u.peak_db) / 10.0);
                sw += w;
                st += w * (static_cast<double>(b) + 0.5) * BIN_S;
            }
            u.t = st / sw;
        }
        u.open = (newest_ - b1) * BIN_S < 0.3;
        for (int64_t b = b0; b <= b1; b++)
            if (lv_[slot(b)] >= thr) u.dwell += BIN_S;
        out.push_back(u);
    };
    for (int64_t b = first; b <= newest_; b++) {
        if (lv_[slot(b)] < thr) continue;
        if (a >= 0 && b - last <= gap) {
            last = b;
            continue;
        }
        if (a >= 0) close(a, last);
        a = last = b;
    }
    if (a >= 0) close(a, last);
    return out;
}

double scan_period(const std::deque<double>& beams, int* support) {
    if (support) *support = 0;
    const size_t n = beams.size();
    if (n < 3) return 0;
    auto tol = [](double p) { return std::max(0.04, 0.015 * p); };
    std::vector<double> cands;
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n && j <= i + 3; j++) {
            const double d = beams[j] - beams[i];
            if (d >= 0.8 && d <= 30) cands.push_back(d);
        }
    double best = 0;
    int best_s = 0;
    for (double P : cands) {
        int s = 0;
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++) {
                const double d = beams[j] - beams[i] - P;
                if (std::fabs(d) <= tol(P)) {
                    s++;
                    break;
                }
                if (d > tol(P)) break;
            }
        if (s > best_s || (s == best_s && P < best)) {
            best_s = s;
            best = P;
        }
    }
    if (best_s < 2) return 0;
    // the chain back from the newest beam, skipping up to 3 missed passes
    std::vector<std::pair<double, double>> kt;   // (turn number, time)
    double cur = beams.back();
    int k = 0;
    kt.emplace_back(0, cur);
    for (size_t i = n - 1; i-- > 0;) {
        const double d = cur - beams[i];
        const double m = std::round(d / best);
        if (m < 1 || m > 4) {
            if (m > 4) break;
            continue;
        }
        if (std::fabs(d - m * best) > tol(best) * m) continue;
        k -= static_cast<int>(m);
        cur = beams[i];
        kt.emplace_back(k, cur);
    }
    if (kt.size() < 3) return 0;
    double sk = 0, st = 0, skk = 0, skt = 0;
    for (auto& [kk, t] : kt) {
        sk += kk;
        st += t;
        skk += kk * kk;
        skt += kk * t;
    }
    const double nn = static_cast<double>(kt.size());
    const double var = skk / nn - (sk / nn) * (sk / nn);
    if (!(var > 0)) return 0;
    if (support) *support = static_cast<int>(kt.size());
    return (skt / nn - (sk / nn) * (st / nn)) / var;
}

namespace {
bool fit_once(const std::vector<std::pair<double, float>>& pts, double* t0, double* peak_db, double* width3_s, double* rms);
}

// Robust: fitted again without the points more than 3 dB off (sidelobes,
// multipath dips at the lobe's edges), up to three times
bool fit_lobe(const std::vector<std::pair<double, float>>& pts_in, double* t0, double* peak_db, double* width3_s) {
    std::vector<std::pair<double, float>> pts = pts_in;
    double rms = 0;
    for (int it = 0; it < 4; it++) {
        if (!fit_once(pts, t0, peak_db, width3_s, &rms)) return false;
        if (rms <= 1.5) break;
        // model: peak - 12 (t - t0)^2 / w^2 ... as a parabola: peak - 3 (2 (t - t0) / w)^2
        std::vector<std::pair<double, float>> keep;
        for (auto& [t, y] : pts) {
            const double x = 2 * (t - *t0) / *width3_s;
            if (std::fabs(y - (*peak_db - 3 * x * x)) <= 3) keep.emplace_back(t, y);
        }
        if (keep.size() == pts.size() || keep.size() < 5) break;
        pts.swap(keep);
    }
    return rms <= 3;
}

namespace {
bool fit_once(const std::vector<std::pair<double, float>>& pts, double* t0, double* peak_db, double* width3_s, double* rms) {
    const size_t n = pts.size();
    if (n < 5) return false;
    double tm = 0, tmin = 1e300, tmax = -1e300;
    float ymax = -1e9f;
    for (auto& [t, y] : pts) {
        tm += t;
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
        ymax = std::max(ymax, y);
    }
    tm /= static_cast<double>(n);
    // normal equations of y = a + b x + c x^2
    double S[5] = {}, T[3] = {};
    for (auto& [t, y] : pts) {
        const double x = t - tm;
        double xp = 1;
        for (int i = 0; i < 5; i++) {
            S[i] += xp;
            if (i < 3) T[i] += xp * y;
            xp *= x;
        }
    }
    double M[3][4] = {{S[0], S[1], S[2], T[0]}, {S[1], S[2], S[3], T[1]}, {S[2], S[3], S[4], T[2]}};
    for (int c = 0; c < 3; c++) {
        int p = c;
        for (int r = c + 1; r < 3; r++)
            if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
        if (std::fabs(M[p][c]) < 1e-30) return false;
        for (int j = 0; j < 4; j++) std::swap(M[c][j], M[p][j]);
        for (int r = 0; r < 3; r++) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int j = c; j < 4; j++) M[r][j] -= f * M[c][j];
        }
    }
    const double a = M[0][3] / M[0][0], b = M[1][3] / M[1][1], c = M[2][3] / M[2][2];
    if (!(c < 0)) return false;
    const double x0 = -b / (2 * c);
    const double pk = a - b * b / (4 * c);
    const double w = 2 * std::sqrt(3 / -c);
    double r2 = 0;
    for (auto& [t, y] : pts) {
        const double x = t - tm;
        const double e = y - (a + b * x + c * x * x);
        r2 += e * e;
    }
    *rms = std::sqrt(r2 / static_cast<double>(n));
    if (tm + x0 < tmin - 0.05 || tm + x0 > tmax + 0.05) return false;
    if (w < 0.002 || w > 2 || pk < ymax - 1) return false;
    *t0 = tm + x0;
    *peak_db = pk;
    *width3_s = w;
    return true;
}
}  // namespace

}  // namespace radar
