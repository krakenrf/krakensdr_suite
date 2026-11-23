// Signal ID: randomized invariant tests of the service's rules
// (include/sigclass_rules.hpp; not part of kraken_doa). Every rule is also run
// in deliberately broken versions (mutants) that the checks must catch.
// Build and run (from kraken_doa_v2):
//   g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/sigclass_rules_test.cpp -o /tmp/sigclass_rules_test
//   /tmp/sigclass_rules_test [ROUNDS] [SEED]
// Exit 0: every invariant held on every input and every mutant was caught.
#include "sigclass_rules.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <tuple>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

struct T {   // the fields of the service's PubTrack the rules read, + strings
    int id = 0, tuner = 0, hits = 0;
    double hz = 0, bw = 0, seen_real = 0;
    float snr = 0;
    bool fresh = false;
    std::string cls, name;
    bool operator==(const T& o) const {
        return id == o.id && tuner == o.tuner && hits == o.hits && snr == o.snr && hz == o.hz && bw == o.bw && seen_real == o.seen_real &&
               fresh == o.fresh && cls == o.cls && name == o.name;
    }
};

using Dedupe = std::function<void(std::vector<T>&)>;

// --- mutants of drop_overlap_duplicates --------------------------------------
// M1: the shipped code of 9751af3 (compaction by self-move, early scan end)
void m1_old(std::vector<T>& pub) {
    std::sort(pub.begin(), pub.end(), [](const T& a, const T& b) { return a.hz < b.hz; });
    std::vector<uint8_t> drop(pub.size(), 0);
    for (size_t i = 0; i < pub.size(); i++)
        for (size_t j = i + 1; j < pub.size() && pub[j].hz - pub[i].hz < 0.5 * std::max(pub[i].bw, pub[j].bw) + 500.0; j++) {
            const T &a = pub[i], &b = pub[j];
            if (a.tuner == b.tuner || drop[i] || drop[j]) continue;
            const double ra = std::max(a.bw, 1.0) / std::max(b.bw, 1.0);
            if (ra > 3.0 || ra < 1.0 / 3.0) continue;
            const bool keep_a = a.fresh != b.fresh ? a.fresh : a.seen_real != b.seen_real ? a.seen_real > b.seen_real : a.hits >= b.hits;
            drop[keep_a ? j : i] = 1;
        }
    size_t w = 0;
    for (size_t i = 0; i < pub.size(); i++)
        if (!drop[i]) pub[w++] = std::move(pub[i]);
    pub.resize(w);
}
// M2: the old pairwise scan with a correct compaction (only the scan's holes)
void m2_pairwise(std::vector<T>& pub) {
    std::sort(pub.begin(), pub.end(), [](const T& a, const T& b) { return a.hz < b.hz; });
    std::vector<uint8_t> drop(pub.size(), 0);
    for (size_t i = 0; i < pub.size(); i++)
        for (size_t j = i + 1; j < pub.size() && pub[j].hz - pub[i].hz < 0.5 * std::max(pub[i].bw, pub[j].bw) + 500.0; j++) {
            if (drop[i] || drop[j] || !sigsvc::rules::same_signal(pub[i], pub[j])) continue;
            drop[sigsvc::rules::better(pub[i], pub[j]) ? j : i] = 1;
        }
    std::vector<T> out;
    for (size_t i = 0; i < pub.size(); i++)
        if (!drop[i]) out.push_back(pub[i]);
    pub = out;
}
// M3: one tuner's tracks may drop each other (no tuner test)
void m3_no_tuner(std::vector<T>& pub) {
    for (T& k : pub) k.tuner += 100;   // (made distinct per track, then restored)
    std::vector<int> orig;
    for (size_t i = 0; i < pub.size(); i++) {
        orig.push_back(pub[i].tuner - 100);
        pub[i].tuner = int(i) + 1000;
    }
    std::vector<std::pair<int, int>> map;
    for (size_t i = 0; i < pub.size(); i++) map.push_back({pub[i].tuner, orig[i]});
    sigsvc::rules::drop_overlap_duplicates(pub);
    for (T& k : pub)
        for (auto& m : map)
            if (m.first == k.tuner) k.tuner = m.second;
}
// M4: the worse one kept
void m4_worse(std::vector<T>& pub) {
    for (T& k : pub) k.fresh = !k.fresh, k.snr = -k.snr, k.seen_real = -k.seen_real, k.hits = -k.hits;
    sigsvc::rules::drop_overlap_duplicates(pub);
    for (T& k : pub) k.fresh = !k.fresh, k.snr = -k.snr, k.seen_real = -k.seen_real, k.hits = -k.hits;
}

// --- the checks ----------------------------------------------------------------
std::string check_dedupe(const std::vector<T>& in, const Dedupe& f) {
    std::vector<T> out = in;
    f(out);
    std::set<int> ids;
    for (const T& k : out) {
        const T* src = nullptr;
        for (const T& s : in)
            if (s.id == k.id) src = &s;
        if (!src) return "a track not in the input";
        if (!(*src == k)) return "a kept track changed (id " + std::to_string(k.id) + ")";
        if (!ids.insert(k.id).second) return "a track twice";
    }
    for (size_t i = 1; i < out.size(); i++)
        if (out[i].hz < out[i - 1].hz) return "not sorted by hz";
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = i + 1; j < out.size(); j++)
            if (sigsvc::rules::same_signal(out[i], out[j])) return "two kept tracks are one signal";
    std::set<int> tuners;
    for (const T& k : in) tuners.insert(k.tuner);
    for (const T& s : in) {
        if (ids.count(s.id)) continue;
        if (tuners.size() == 1) return "a single tuner's track dropped";
        bool ok = false;
        for (const T& k : out) ok = ok || (k.tuner != s.tuner && sigsvc::rules::same_signal(k, s) && sigsvc::rules::better(k, s));
        if (!ok) return "a dropped track without a better kept view of it on another tuner (id " + std::to_string(s.id) + ")";
    }
    std::vector<T> again = out;
    f(again);
    if (!(again.size() == out.size() && std::equal(again.begin(), again.end(), out.begin()))) return "not idempotent";
    return "";
}

std::vector<T> random_tracks(std::mt19937_64& rng) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int tuners = 1 + int(rng() % 5);
    const int signals = int(rng() % 25);
    std::vector<T> v;
    int id = 1;
    for (int s = 0; s < signals; s++) {
        // a signal at some frequency (clustered: neighbours and overlaps happen)
        const double hz = 416e6 + std::floor(U(rng) * 400.0) * 1000.0 + (U(rng) < 0.3 ? U(rng) * 3000.0 : 0.0);
        const double bw = std::pow(10.0, 2.0 + 3.5 * U(rng));
        for (int t = 0; t < tuners; t++) {
            if (U(rng) < 0.4) continue;   // this tuner doesn't see it
            const int views = U(rng) < 0.1 ? 2 : 1;   // (a tuner's tracker splits it sometimes)
            for (int k = 0; k < views; k++) {
                T x;
                x.id = id++;
                x.tuner = t;
                x.hz = hz + (U(rng) - 0.5) * 0.4 * bw;
                x.bw = bw * (0.5 + U(rng));
                x.fresh = U(rng) < 0.5;
                x.seen_real = 1000.0 + std::floor(U(rng) * 4.0);   // (ties happen)
                x.hits = 1 + int(rng() % 4);
                x.snr = float(rng() % 3 == 0 ? 10 : rng() % 40);   // (ties happen)
                x.cls = rng() % 8 == 0 ? std::string() : "c" + std::to_string(x.id % 7);   // (sometimes a view without a class)
                x.name = "Class name number " + std::to_string(x.id);   // (longer than the SSO buffer)
                v.push_back(x);
            }
        }
    }
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}

// --- bands and the scheduler -------------------------------------------------
using sigsvc::rules::TunerSlot;
using Reps = std::function<std::vector<int>(const std::vector<TunerSlot>&)>;
using Pick = std::function<int(const std::vector<TunerSlot>&, const std::vector<int>&, int&, int64_t, bool&)>;

uint64_t dist(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

std::string check_reps(const std::vector<TunerSlot>& t, const Reps& f) {
    const std::vector<int> rep = f(t);
    if (rep.size() != t.size()) return "size";
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].hz == 0) {
            if (rep[i] != -1) return "a tuner without a frequency has a band";
            continue;
        }
        const int r = rep[i];
        if (r < 0 || r > int(i)) return "rep out of range";
        if (rep[size_t(r)] != r) return "a representative that stands for another";
        if (dist(t[i].hz, t[size_t(r)].hz) > sigsvc::rules::SAME_BAND_HZ) return "a tuner on another band than its representative";
        for (size_t j = 0; j < t.size(); j++)
            if (j != i && rep[i] == int(i) && rep[j] == int(j) && t[j].hz && dist(t[i].hz, t[j].hz) <= sigsvc::rules::SAME_BAND_HZ)
                return "two representatives on one band";
    }
    return "";
}

std::string check_pick(std::vector<TunerSlot> t, const Reps& rf, const Pick& pf, std::mt19937_64& rng) {
    const int M = int(t.size());
    const std::vector<int> rep = rf(t);
    const int64_t now = 1000;
    std::vector<int> ready;
    for (int i = 0; i < M; i++)
        if (rep[size_t(i)] == i && now >= t[size_t(i)].settle_until_ms) ready.push_back(i);
    int rr = int(rng() % uint64_t(M));
    bool first = false;
    const int p = pf(t, rep, rr, now, first);
    if (rr < 0 || rr >= M) return "cursor out of range";
    if (ready.empty()) return p == -1 ? "" : "a pick with nothing ready";
    if (p < 0) return "no pick with a band ready";
    if (rep[size_t(p)] != p) return "picked a tuner that does not stand for its band";
    if (t[size_t(p)].hz == 0) return "picked a tuner without a frequency";
    if (now < t[size_t(p)].settle_until_ms) return "picked a settling tuner";
    bool fresh_ready = false;
    for (int i : ready) fresh_ready = fresh_ready || t[size_t(i)].looks == 0;
    if (fresh_ready && !(first && t[size_t(p)].looks == 0)) return "a band without a look waits behind one with looks";
    if (!fresh_ready && first) return "first_look without a new band";
    // fairness: nothing changes, the looks count up - after the first looks,
    // between two looks at a band every other ready band gets exactly one
    t[size_t(p)].looks++;
    const auto all_seen = [&]() {
        for (int i : ready)
            if (t[size_t(i)].looks == 0) return false;
        return true;
    };
    for (int k = 0; !all_seen(); k++) {   // the first looks: at most one per ready band
        if (k >= M) return "the first looks did not end within a round";
        bool f = false;
        const int q = pf(t, rep, rr, now, f);
        if (q < 0) return "no pick in the sequence";
        t[size_t(q)].looks++;
    }
    std::vector<int> seq;
    for (size_t k = 0; k < 3 * ready.size(); k++) {
        bool f = false;
        const int q = pf(t, rep, rr, now, f);
        if (q < 0) return "no pick in the sequence";
        if (f) return "a first look after every band had one";
        t[size_t(q)].looks++;
        seq.push_back(q);
    }
    // every window of |ready| picks is a permutation of the ready bands
    for (size_t a = 0; a + ready.size() <= seq.size(); a++) {
        std::set<int> w(seq.begin() + long(a), seq.begin() + long(a + ready.size()));
        if (w.size() != ready.size() || !std::equal(w.begin(), w.end(), std::set<int>(ready.begin(), ready.end()).begin()))
            return "unfair: a band skipped or looked at twice in a round";
    }
    return "";
}

std::vector<TunerSlot> random_slots(std::mt19937_64& rng) {
    const int M = 1 + int(rng() % 8);
    std::vector<TunerSlot> t(static_cast<size_t>(M));
    const uint64_t base[] = {416815000, 416815000, 416815500, 416816200, 416760000, 859440000, 98000000, 0};
    for (auto& s : t) {
        s.hz = base[rng() % 8] + (rng() % 4 == 0 ? rng() % 1500 : 0);
        if (s.hz < 1000000) s.hz = 0;
        s.settle_until_ms = rng() % 4 == 0 ? 1500 : 0;
        s.looks = rng() % 3 == 0 ? 0 : 1 + rng() % 5;
    }
    return t;
}

std::vector<int> reps_chain(const std::vector<TunerSlot>& t) {   // mutant: joins any earlier tuner, not its band's representative
    std::vector<int> rep(t.size(), -1);
    for (size_t i = 0; i < t.size(); i++) {
        if (!t[i].hz) continue;
        rep[i] = int(i);
        for (size_t j = 0; j < i; j++)
            if (t[j].hz && dist(t[i].hz, t[j].hz) <= sigsvc::rules::SAME_BAND_HZ) {
                rep[i] = int(j);
                break;
            }
    }
    return rep;
}
int pick_plain_rr(const std::vector<TunerSlot>& t, const std::vector<int>&, int& rr, int64_t now, bool& first) {   // the old round robin
    const int M = int(t.size());
    first = false;
    for (int k = 0; k < M; k++) {
        const int i = (rr + k) % M;
        if (t[size_t(i)].hz && now >= t[size_t(i)].settle_until_ms) {
            rr = (i + 1) % M;
            first = t[size_t(i)].looks == 0;
            return i;
        }
    }
    return -1;
}
int pick_no_settle(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t, bool& first) {
    return sigsvc::rules::pick_band(t, rep, rr, INT64_MAX / 2, first);
}
int pick_no_first(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    std::vector<TunerSlot> u = t;
    for (auto& s : u) s.looks += 1;
    const int p = sigsvc::rules::pick_band(u, rep, rr, now, first);
    first = p >= 0 && t[size_t(p)].looks == 0;
    return p;
}
int pick_stuck(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    const int keep = rr;
    const int p = sigsvc::rules::pick_band(t, rep, rr, now, first);
    rr = keep;
    return p;
}

// --- the budget ----------------------------------------------------------------
struct BudgetLike {   // the interface of rules::Budget, for mutants
    std::function<double(double)> plan;
    std::function<bool(double, double, double)> may_skip;
    std::function<void(double, double)> started;
    std::function<double()> debt;
};
BudgetLike wrap(std::shared_ptr<sigsvc::rules::Budget> b) {
    return {[b](double a) { return b->plan(a); }, [b](double e, double a, double c) { return b->may_skip(e, a, c); },
            [b](double e, double a) { b->started(e, a); }, [b]() { return b->debt_s; }};
}

// a random run of looks: analysis a per look, capture 0.55 s, first looks at random
std::string check_budget(const std::function<BudgetLike()>& make, std::mt19937_64& rng, bool with_first) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    BudgetLike b = make();
    const double share = 0.04, mn = 1.0, mx = 10.0, cap_looks = 8.0;
    double a = 0.005 + 0.6 * U(rng) * U(rng);
    double t = 0.0, used = 0.0, excess = 0.0;
    double start = 0.0, max_cap = 0.0;   // (the cap follows the analysis time: the debt stays under the largest one so far)
    for (int k = 0; k < 400; k++) {
        if (U(rng) < 0.05) a = 0.005 + 0.6 * U(rng) * U(rng);   // (the load changes)
        // the look: capture then analysis
        start = t;
        used += a;
        excess += std::max(0.0, a - mx * share);
        const double busy = 0.55 + a;
        const double period = b.plan(a);
        if (period < mn - 1e-9 || period > mx + 1e-9) return "a period outside [min, max]";
        const double cap = cap_looks * a;
        max_cap = std::max(max_cap, cap);
        const bool first = with_first && U(rng) < 0.3;
        double next;
        if (first && b.may_skip(busy, a, cap)) next = start + busy;
        else next = start + std::max(busy, period);
        if (!with_first && std::abs((next - start) - std::max(busy, std::clamp(a / share, mn, mx))) > 1e-9)
            return "without first looks the period is not the old rule's";
        b.started(next - start, a);
        if (b.debt() < -1e-12) return "negative debt";
        if (b.debt() > max_cap + 1e-9) return "the debt beyond its cap";
        t = next;
        // the analysis so far within the budget (+ the debt's cap, the max clamp's excess, the latest look)
        if (used > share * t + cap_looks * 0.605 + excess + a + 1e-6) return "more analysis than the budget allows";
    }
    // the debt is paid back: normal looks only (each repays up to its own analysis)
    const int need = int(std::ceil(b.debt() / a)) + 2;
    for (int k = 0; k < need; k++) {
        const double period = b.plan(a);
        b.started(std::max(0.55 + a, period), a);
    }
    // (only where the 10 s clamp leaves room for it: an analysis over max_s x share / 2 = 0.2 s
    // cannot pay back - the old rule exceeds the share there too; the debt then just blocks first looks)
    if (a <= 0.5 * mx * share && b.debt() > 1e-9) return "the debt not paid back by normal looks";
    return "";
}

}  // namespace

int run_sched_tests(int rounds, uint64_t seed) {
    int fails = 0;
    const auto report = [&](const char* name, bool mutant, int bad, int n, const std::string& first) {
        const bool ok = mutant ? bad > 0 : bad == 0;
        if (!ok) fails++;
        std::printf("%s %-40s %d / %d inputs violate an invariant%s%s\n", ok ? "PASS" : "FAIL", name, bad, n,
                    first.empty() ? "" : " - first: ", first.c_str());
    };
    {
        const std::vector<std::pair<const char*, Reps>> rc = {{"band_reps", sigsvc::rules::band_reps}, {"mutant: reps chained", reps_chain}};
        for (size_t c = 0; c < rc.size(); c++) {
            std::mt19937_64 rng(seed);
            int bad = 0;
            std::string first;
            for (int r = 0; r < rounds; r++) {
                const std::string e = check_reps(random_slots(rng), rc[c].second);
                if (!e.empty() && !bad++) first = e;
            }
            report(rc[c].first, c > 0, bad, rounds, first);
        }
    }
    {
        const std::vector<std::pair<const char*, Pick>> pc = {
            {"pick_band", sigsvc::rules::pick_band},
            {"mutant: the old round robin", pick_plain_rr},
            {"mutant: no settling", pick_no_settle},
            {"mutant: no first looks first", pick_no_first},
            {"mutant: the cursor stuck", pick_stuck},
        };
        for (size_t c = 0; c < pc.size(); c++) {
            std::mt19937_64 rng(seed);
            int bad = 0;
            std::string first;
            for (int r = 0; r < rounds; r++) {
                const std::vector<TunerSlot> t = random_slots(rng);
                const std::string e = check_pick(t, sigsvc::rules::band_reps, pc[c].second, rng);
                if (!e.empty() && !bad++) first = e;
            }
            report(pc[c].first, c > 0, bad, rounds, first);
        }
    }
    {
        const auto real = []() { return wrap(std::make_shared<sigsvc::rules::Budget>()); };
        // mutants: first looks without a debt; no payback; the cap ignored
        const auto no_debt = []() {
            auto b = std::make_shared<sigsvc::rules::Budget>();
            BudgetLike w = wrap(b);
            w.started = [b](double e, double a) { if (e >= b->planned_s - 1e-3) b->started(e, a); };
            return w;
        };
        const auto no_payback = []() {
            auto b = std::make_shared<sigsvc::rules::Budget>();
            BudgetLike w = wrap(b);
            w.plan = [b](double a) { b->planned_s = std::clamp(a / b->share, b->min_s, b->max_s); return b->planned_s; };
            return w;
        };
        const auto no_cap = []() {
            auto b = std::make_shared<sigsvc::rules::Budget>();
            BudgetLike w = wrap(b);
            w.may_skip = [](double, double, double) { return true; };
            return w;
        };
        const std::vector<std::tuple<const char*, std::function<BudgetLike()>, bool, bool>> bc = {
            {"Budget (no first looks: the old rule)", real, false, false},
            {"Budget (first looks)", real, true, false},
            {"mutant: first looks without a debt", no_debt, true, true},
            {"mutant: no payback", no_payback, true, true},
            {"mutant: the debt cap ignored", no_cap, true, true},
        };
        for (const auto& [name, mk, wf, mut] : bc) {
            std::mt19937_64 rng(seed);
            int bad = 0;
            std::string first;
            for (int r = 0; r < rounds / 20; r++) {
                const std::string e = check_budget(mk, rng, wf);
                if (!e.empty() && !bad++) first = e;
            }
            report(name, mut, bad, rounds / 20, first);
        }
    }
    return fails;
}

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 20000;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 12345;
    int fails = 0;
    struct Case {
        const char* name;
        Dedupe f;
        bool mutant;
    };
    const std::vector<Case> cases = {
        {"drop_overlap_duplicates", [](std::vector<T>& v) { sigsvc::rules::drop_overlap_duplicates(v); }, false},
        {"mutant 1: 9751af3's code (self-move)", m1_old, true},
        {"mutant 2: pairwise scan", m2_pairwise, true},
        {"mutant 3: no tuner test", m3_no_tuner, true},
        {"mutant 4: the worse view kept", m4_worse, true},
    };
    for (const Case& c : cases) {
        std::mt19937_64 rng(seed);
        int bad = 0;
        std::string first;
        for (int r = 0; r < rounds; r++) {
            const std::vector<T> in = random_tracks(rng);
            const std::string e = check_dedupe(in, c.f);
            if (!e.empty()) {
                if (!bad) first = e;
                bad++;
            }
        }
        const bool ok = c.mutant ? bad > 0 : bad == 0;
        if (!ok) fails++;
        std::printf("%s %-40s %d / %d inputs violate an invariant%s%s\n", ok ? "PASS" : "FAIL", c.name, bad, rounds,
                    first.empty() ? "" : " - first: ", first.c_str());
    }
    fails += run_sched_tests(rounds, seed);
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
