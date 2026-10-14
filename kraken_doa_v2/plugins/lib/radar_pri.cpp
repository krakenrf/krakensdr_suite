#include "radar_pri.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace radar {

namespace {

double tol_of(double v) { return std::max(1.5, 0.0012 * v); }

struct Cluster {
    double sum = 0, sum2 = 0;
    int n = 0;
    double mean() const { return sum / n; }
    double sd() const { return std::sqrt(std::max(0.0, sum2 / n - mean() * mean())); }
};

std::string us(double v) {
    char b[32];
    snprintf(b, sizeof b, v >= 100 ? "%.0f" : "%.1f", v);
    return b;
}

// smallest period p of seq (labels, -1 = unknown) with >= frac of the
// comparisons equal (at least min_cmp of them)
int find_period(const std::vector<int>& seq, int pmin, int pmax, double frac, int min_cmp) {
    for (int p = pmin; p <= pmax; p++) {
        int eq = 0, cmp = 0;
        for (size_t i = 0; i + static_cast<size_t>(p) < seq.size(); i++) {
            const int a = seq[i], b = seq[i + static_cast<size_t>(p)];
            if (a < 0 || b < 0) continue;
            cmp++;
            if (a == b) eq++;
        }
        if (cmp >= std::max(min_cmp, p) && eq >= frac * cmp) return p;
    }
    return 0;
}

// rotation of cyc that is largest in its values (a canonical start)
std::vector<double> canonical(const std::vector<double>& cyc) {
    std::vector<double> best = cyc;
    for (size_t r = 1; r < cyc.size(); r++) {
        std::vector<double> c(cyc.begin() + static_cast<long>(r), cyc.end());
        c.insert(c.end(), cyc.begin(), cyc.begin() + static_cast<long>(r));
        if (c > best) best = c;
    }
    return best;
}

}  // namespace

PriInfo analyze_pri(const std::vector<double>& toas) {
    PriInfo r;
    if (toas.size() < 9) return r;
    r.t0 = toas.front();
    r.t1 = toas.back();
    std::vector<double> d;   // consecutive differences (us), NAN = a break
    d.reserve(toas.size());
    for (size_t i = 0; i + 1 < toas.size(); i++) {
        const double v = (toas[i + 1] - toas[i]) * 1e6;
        d.push_back(v >= 100 && v <= 30000 ? v : NAN);   // PRF 33 Hz .. 10 kHz (closer pulses: a group / an echo)
    }
    std::vector<double> s;
    for (double v : d)
        if (std::isfinite(v)) s.push_back(v);
    const int N = static_cast<int>(s.size());
    r.diffs = N;
    if (N < 8) return r;
    std::sort(s.begin(), s.end());
    std::vector<Cluster> cl;
    for (double v : s) {
        if (cl.empty() || v - cl.back().mean() > tol_of(v)) cl.emplace_back();
        cl.back().sum += v;
        cl.back().sum2 += v * v;
        cl.back().n++;
    }
    // the radar's own PRIs vs missed pulses (sums of 2-4 PRIs)
    std::vector<int> basic;
    int harm_n = 0;
    const int min_n = std::max(3, static_cast<int>(std::ceil(0.04 * N)));
    for (size_t c = 0; c < cl.size(); c++) {
        const double v = cl[c].mean(), t = tol_of(v);
        bool harm = false;
        for (size_t i = 0; i < basic.size() && !harm; i++) {
            const Cluster& A = cl[static_cast<size_t>(basic[i])];
            for (size_t j = i; j < basic.size() && !harm; j++) {
                const Cluster& B = cl[static_cast<size_t>(basic[j])];
                const double lim = 0.6 * std::min(A.n, B.n);
                if (std::fabs(v - A.mean() - B.mean()) <= 2 * t && cl[c].n <= lim) harm = true;
                for (size_t k = j; k < basic.size() && !harm; k++) {
                    const Cluster& C = cl[static_cast<size_t>(basic[k])];
                    if (std::fabs(v - A.mean() - B.mean() - C.mean()) <= 3 * t && cl[c].n <= 0.6 * std::min(lim / 0.6, double(C.n)))
                        harm = true;
                }
            }
            for (int k = 4; k <= 8 && !harm; k++)
                if (std::fabs(v - k * A.mean()) <= k * t && cl[c].n <= 0.6 * A.n) harm = true;
        }
        // several missed pulses in a stagger / block pattern: any sum of 4-6
        // of the PRIs (with repeats; the levels found so far, up to 6 of them)
        if (!harm && basic.size() >= 2 && basic.size() <= 6) {
            int minn = 1 << 30;
            for (int b : basic) minn = std::min(minn, cl[static_cast<size_t>(b)].n);
            if (cl[c].n <= 0.6 * minn) {
                std::vector<double> L;
                for (int b : basic) L.push_back(cl[static_cast<size_t>(b)].mean());
                // sums of k terms from L (non-decreasing index), k = 4..6
                auto any_sum = [&](auto&& self, size_t from, int left, double acc, int k) -> bool {
                    if (left == 0) return std::fabs(v - acc) <= k * t;
                    for (size_t i = from; i < L.size(); i++)
                        if (acc + L[i] <= v + k * t && self(self, i, left - 1, acc + L[i], k)) return true;
                    return false;
                };
                for (int k = 4; k <= 6 && !harm; k++) harm = any_sum(any_sum, 0, k, 0.0, k);
            }
        }
        if (harm) {
            harm_n += cl[c].n;
            continue;
        }
        if (cl[c].n >= min_n) basic.push_back(static_cast<int>(c));
    }
    int basic_n = 0;
    for (int b : basic) basic_n += cl[static_cast<size_t>(b)].n;
    r.coverage = static_cast<double>(basic_n + harm_n) / N;
    const bool levels_ok = !basic.empty() && basic.size() <= 16 && r.coverage >= 0.6 && basic_n >= std::max(6, static_cast<int>(0.4 * N));
    if (!levels_ok) {
        // random jitter around one value?
        const double m = s[s.size() / 2];
        double sum = 0, sum2 = 0;
        int n = 0;
        for (double v : s)
            if (v >= 0.7 * m && v <= 1.3 * m) {
                sum += v;
                sum2 += v * v;
                n++;
            }
        if (n >= 8 && n >= 0.6 * N) {
            r.ok = true;
            r.kind = "jitter";
            r.mean_pri = sum / n;
            r.jitter_pct = 100 * std::sqrt(std::max(0.0, sum2 / n - r.mean_pri * r.mean_pri)) / r.mean_pri;
            r.levels = {r.mean_pri};
            r.counts = {n};
            r.coverage = static_cast<double>(n) / N;
        }
        return r;
    }
    r.ok = true;
    // label every difference with its level
    std::vector<int> lab(d.size(), -1);
    double msum = 0;
    int mn = 0;
    for (size_t i = 0; i < d.size(); i++) {
        if (!std::isfinite(d[i])) continue;
        for (size_t b = 0; b < basic.size(); b++) {
            const Cluster& C = cl[static_cast<size_t>(basic[b])];
            if (std::fabs(d[i] - C.mean()) <= 1.5 * tol_of(d[i]) + 2 * C.sd()) {
                lab[i] = static_cast<int>(b);
                msum += d[i];
                mn++;
                break;
            }
        }
    }
    r.mean_pri = mn ? msum / mn : cl[static_cast<size_t>(basic[0])].mean();
    // a PRI set switched inside the window: some level only in one part of it
    if (basic.size() >= 2) {
        for (size_t b = 0; b < basic.size(); b++) {
            int first = -1, last = -1;
            for (size_t i = 0; i < lab.size(); i++)
                if (lab[i] == static_cast<int>(b)) {
                    if (first < 0) first = static_cast<int>(i);
                    last = static_cast<int>(i);
                }
            if (first >= 0 && (last - first) < 0.4 * static_cast<double>(lab.size())) r.straddles = true;
        }
    }
    auto level = [&](int b) { return cl[static_cast<size_t>(basic[static_cast<size_t>(b)])].mean(); };
    if (basic.size() == 1) {
        const Cluster& C = cl[static_cast<size_t>(basic[0])];
        r.kind = "constant";
        r.levels = {C.mean()};
        r.counts = {C.n};
        r.jitter_pct = 100 * C.sd() / C.mean();
        return r;
    }
    // runs of one level
    struct Run { int lab, len; bool complete; };
    std::vector<Run> runs;
    for (size_t i = 0; i < lab.size();) {
        size_t j = i;
        while (j < lab.size() && lab[j] == lab[i]) j++;
        if (lab[i] >= 0) {
            const bool complete = i > 0 && j < lab.size() && lab[i - 1] >= 0 && lab[j] >= 0;
            runs.push_back({lab[i], static_cast<int>(j - i), complete});
        } else {
            runs.push_back({-1, static_cast<int>(j - i), false});
        }
        i = j;
    }
    std::vector<int> clen;
    for (const Run& u : runs)
        if (u.complete) clen.push_back(u.len);
    if (clen.size() >= 2) {
        std::vector<int> c2 = clen;
        std::nth_element(c2.begin(), c2.begin() + static_cast<long>(c2.size() / 2), c2.end());
        if (c2[c2.size() / 2] >= 3) {
            std::map<int, int> h;
            for (int v : clen) h[v]++;
            int best = 0, bn = 0;
            for (auto& [v, n] : h)
                if (n > bn) {
                    bn = n;
                    best = v;
                }
            r.kind = "blocks";
            r.block = best;
            // the order of the levels: the run labels' period (breaks left
            // out - a missed pulse splits a block in two runs of one level)
            std::vector<int> rl;
            for (const Run& u : runs)
                if (u.lab >= 0 && (rl.empty() || rl.back() != u.lab)) rl.push_back(u.lab);
            int p = find_period(rl, 1, 8, 0.8, 3);
            if (p <= 1) p = static_cast<int>(basic.size());
            std::vector<double> cyc;
            for (size_t i = rl.size(); i-- > 0 && cyc.size() < static_cast<size_t>(p);) cyc.insert(cyc.begin(), level(rl[i]));
            if (cyc.size() != static_cast<size_t>(p)) {
                cyc.clear();
                for (size_t b = 0; b < basic.size(); b++) cyc.push_back(level(static_cast<int>(b)));
            }
            r.levels = canonical(cyc);
            for (double v : r.levels)
                for (size_t b = 0; b < basic.size(); b++)
                    if (std::fabs(level(static_cast<int>(b)) - v) < 1e-9) r.counts.push_back(cl[static_cast<size_t>(basic[b])].n);
            return r;
        }
    }
    const int p = find_period(lab, 2, 32, 0.85, 6);
    if (p) {
        // one period of valid labels, from the end
        std::vector<double> cyc;
        for (size_t i = lab.size(); i-- > 0 && cyc.size() < static_cast<size_t>(p);) {
            if (lab[i] < 0) {
                cyc.clear();
                continue;
            }
            cyc.insert(cyc.begin(), level(lab[i]));
        }
        if (cyc.size() == static_cast<size_t>(p)) {
            r.kind = "stagger";
            r.levels = canonical(cyc);
            for (double v : r.levels)
                for (size_t b = 0; b < basic.size(); b++)
                    if (std::fabs(level(static_cast<int>(b)) - v) < 1e-9) r.counts.push_back(cl[static_cast<size_t>(basic[b])].n);
            return r;
        }
    }
    r.kind = "mixed";
    for (size_t b = 0; b < basic.size(); b++) {
        r.levels.push_back(level(static_cast<int>(b)));
        r.counts.push_back(cl[static_cast<size_t>(basic[b])].n);
    }
    return r;
}

std::vector<double> PriInfo::set() const {
    std::vector<double> s;
    for (double v : levels)
        if (std::none_of(s.begin(), s.end(), [&](double u) { return std::fabs(u - v) <= tol_of(v); })) s.push_back(v);
    std::sort(s.begin(), s.end());
    return s;
}

std::string PriInfo::text() const {
    if (!ok) return "";
    std::string t;
    auto list = [&](const char* sep) {
        std::string l;
        for (size_t i = 0; i < levels.size(); i++) l += (i ? sep : "") + us(levels[i]);
        return l + " us";
    };
    char b[64];
    if (kind == "constant") {
        t = list("");
        if (jitter_pct >= 0.5) {
            snprintf(b, sizeof b, " ± %.1f %%", jitter_pct);
            t += b;
        }
    } else if (kind == "blocks") {
        t = list(" / ") + ", blocks of " + std::to_string(block);
    } else if (kind == "stagger") {
        t = "stagger " + std::to_string(levels.size()) + ": " + list(" / ");
    } else if (kind == "jitter") {
        snprintf(b, sizeof b, "~%s us ± %.1f %% (jittered)", us(mean_pri).c_str(), jitter_pct);
        t = b;
    } else {
        t = list(", ") + " (no fixed order)";
    }
    return t;
}

bool same_set(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::fabs(a[i] - b[i]) > 2 * tol_of(a[i])) return false;
    return true;
}

std::string set_text(const std::vector<double>& s) {
    std::string t;
    for (size_t i = 0; i < s.size(); i++) t += (i ? " / " : "") + us(s[i]);
    return t + " us";
}

}  // namespace radar
