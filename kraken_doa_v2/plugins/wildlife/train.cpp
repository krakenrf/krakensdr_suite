#include "train.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace wl {

namespace {
Period eval(const std::vector<double>& t, double P, double anchor) {
    Period r;
    if (!(P > 0)) return r;
    const double tol = grid_tol(P);
    std::set<long> slots;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int n = 0;
    long nmin = 0;
    for (double ti : t) {
        if (ti > anchor + tol) continue;
        const double q = (ti - anchor) / P;
        const long k = std::lround(q);
        if (std::fabs((q - static_cast<double>(k)) * P) > tol) continue;
        if (!slots.insert(k).second) continue;
        nmin = std::min(nmin, k);
        sx += static_cast<double>(k);
        sy += ti;
        sxx += static_cast<double>(k) * static_cast<double>(k);
        sxy += static_cast<double>(k) * ti;
        n++;
    }
    r.support = n;
    r.slots = static_cast<int>(-nmin) + 1;
    r.score = n - 0.5 * (r.slots - n);
    r.period = P;
    r.t_ref = anchor;
    // least squares t = a + k P over the supported pulses
    const double den = n * sxx - sx * sx;
    if (n >= 3 && den > 0) {
        const double Pf = (n * sxy - sx * sy) / den;
        const double a = (sy - Pf * sx) / n;
        if (Pf > 0.5 * P && Pf < 1.5 * P) {
            r.period = Pf;
            r.t_ref = a;   // the grid at k = 0 (the anchor's slot)
            double ss = 0;
            for (double ti : t) {
                const double q = (ti - a) / Pf;
                const long k = std::lround(q);
                const double e = (q - static_cast<double>(k)) * Pf;
                if (std::fabs(e) <= tol && slots.count(k)) ss += e * e;
            }
            r.jitter = std::sqrt(ss / n);
        }
    }
    return r;
}
}  // namespace

Period find_period(const std::vector<double>& t, double pmin, double pmax) {
    Period best;
    const size_t n = t.size();
    if (n < 3) return best;
    std::vector<double> cand;
    for (size_t j = 1; j < n; j++)
        for (size_t i = j >= 7 ? j - 7 : 0; i < j; i++) {
            const double d = t[j] - t[i];
            for (int k = 1; k <= 4; k++) {
                const double P = d / k;
                if (P >= pmin && P <= pmax) cand.push_back(P);
            }
        }
    std::sort(cand.begin(), cand.end());
    // merge near-equal candidates (0.5 %)
    std::vector<double> uniq;
    for (double P : cand)
        if (uniq.empty() || P > uniq.back() * 1.005) uniq.push_back(P);
    for (size_t a = n >= 3 ? n - 3 : 0; a < n; a++)
        for (double P : uniq) {
            Period r = eval(t, P, t[a]);
            if (r.score > best.score + 1e-9 || (std::fabs(r.score - best.score) < 1e-9 && r.period > best.period)) best = r;
        }
    return best;
}

StreamResult decode_stream(const std::vector<StreamEvent>& ev) {
    StreamResult res;
    std::vector<size_t> R;
    bool burst = false;
    for (size_t i = 0; i < ev.size(); i++)
        if (ev[i].run >= 2) {
            R.push_back(i);
            burst = burst || ev[i].run >= 3;
        }
    if (R.size() < 2) return res;
    char b[160];
    if (!burst) {
        // double beeps between the digits; the normal pulses count
        res.scheme = "double-beep";
        std::vector<int> d;
        for (size_t k = 1; k < R.size(); k++) {
            int c = 0;
            for (size_t i = R[k - 1] + 1; i < R[k]; i++) c += ev[i].run == 1;
            if (c < 2 || c > 11) return res;   // not a digit: a pulse lost / not a stream
            d.push_back(c - 2);
        }
        res.numbers = d;
        res.ok = true;
        int v = 0;
        for (int x : d) v = v * 10 + x;
        std::string ds;
        for (int x : d) ds += static_cast<char>('0' + x);
        if (d.size() == 3) {
            res.kind = "time-since-death";
            const int since = v + 24;
            snprintf(b, sizeof b, "%d h in mortality mode (no movement for %d h = %d d %d h)", v, since, since / 24, since % 24);
            res.text = b;
            res.fields = {{"Hours in mortality mode", std::to_string(v)},
                          {"Since the last movement", std::to_string(since) + " h (mortality mode starts after 24 h still)"}};
        } else {
            res.kind = "code";
            res.text = "digits " + ds;
            res.fields = {{"Digits", ds}};
        }
        return res;
    }
    // bursts of fast pulses = digits; tens and units back to back
    res.scheme = "burst";
    std::vector<std::vector<int>> nums;
    for (size_t k = 0; k < R.size(); k++) {
        const int dg = ev[R[k]].run - 2;
        if (dg < 0 || dg > 9) return res;
        int between = 0;
        if (k) for (size_t i = R[k - 1] + 1; i < R[k]; i++) between += ev[i].run == 1;
        if (k == 0 || between > 1 || nums.back().size() >= 2) nums.push_back({});
        nums.back().push_back(dg);
    }
    for (const auto& nd : nums) {
        int v = 0;
        for (int x : nd) v = v * 10 + x;
        res.numbers.push_back(v);
    }
    res.ok = true;
    static const char* chick[8] = {"Days since incubation started", "Days since hatching",
                                   "Days since nest abandonment", "Hours since last emergence",
                                   "Battery (weeks left)", "Active 10-min blocks last night",
                                   "Active 10-min blocks 2 nights ago", "Active 10-min blocks (4-night mean)"};
    static const char* egg[11] = {"Days since change of state", "Last incubation (days)", "Activity yesterday",
                                  "Activity 2 days ago", "Activity 3 days ago", "Activity 4 days ago",
                                  "Activity 5 days ago", "Activity 6 days ago", "Activity 7 days ago",
                                  "Long-term mean activity", "Battery (weeks left)"};
    std::string list;
    for (size_t i = 0; i < res.numbers.size(); i++) list += (i ? " " : "") + std::to_string(res.numbers[i]);
    if (res.numbers.size() == 8) {
        res.kind = "chick-timer";
        for (size_t i = 0; i < 8; i++) res.fields.push_back({chick[i], std::to_string(res.numbers[i])});
        snprintf(b, sizeof b, "incubating %d d, hatched %d d, abandoned %d d, emerged %d h ago, battery %d wk",
                 res.numbers[0], res.numbers[1], res.numbers[2], res.numbers[3], res.numbers[4]);
        res.text = b;
    } else if (res.numbers.size() == 11) {
        res.kind = "egg-timer";
        for (size_t i = 0; i < 11; i++) res.fields.push_back({egg[i], std::to_string(res.numbers[i])});
        snprintf(b, sizeof b, "%d d since change of state, last incubation %d d, activity %d, battery %d wk",
                 res.numbers[0], res.numbers[1], res.numbers[2], res.numbers[10]);
        res.text = b;
    } else {
        res.kind = "code";
        res.text = "numbers " + list;
        res.fields = {{"Numbers", list}};
    }
    return res;
}

}  // namespace wl
