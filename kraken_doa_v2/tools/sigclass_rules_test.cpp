// Signal ID: invariant tests of the service's rules (include/sigclass_rules.hpp;
// not part of kraken_doa). Every rule is also run in deliberately broken
// versions (mutants) that the checks must catch.
// Build and run (from kraken_doa_v2):
//   g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/sigclass_rules_test.cpp -o /tmp/sigclass_rules_test
//   /tmp/sigclass_rules_test [ROUNDS] [SEED]   (default 200000 12345: ~30 s)
// Exit 0: every invariant held on every input and every mutant was caught.
//
// The verdict does not depend on the seed: each mutant has DIRECTED inputs -
// fixed sequences, the same for every seed - that expose it, and it counts as
// caught only when they do (a mutant the random inputs catch only now and
// then once passed with one seed and failed with another). The random inputs
// (ROUNDS, SEED) add coverage for the real rules; their counts for the
// mutants are printed for information.
#include "sigclass_rules.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace R = sigsvc::rules;

int g_fails = 0;
FILE* g_trace = nullptr;   // (a debugging aid: every look of sim())
// one line per check: a real rule must hold on every input (directed and
// random); a mutant must be caught by its directed inputs
void report(const char* name, bool mutant, int dir_bad, int dir_n, int rnd_bad, int rnd_n, const std::string& first) {
    const bool ok = mutant ? dir_bad > 0 : (dir_bad == 0 && rnd_bad == 0);
    if (!ok) g_fails++;
    std::printf("%s %-50s directed %d / %d, random %d / %d violate%s%s\n", ok ? "PASS" : "FAIL", name, dir_bad, dir_n, rnd_bad, rnd_n,
                first.empty() ? "" : " - first: ", first.c_str());
}

// --- the overlap rule (drop_overlap_duplicates) ---------------------------------
struct T {   // the fields of the service's PubTrack the rules read, + strings
    int id = 0, tuner = 0, hits = 0;
    double hz = 0, bw = 0, seen_real = 0;
    float snr = 0;
    bool fresh = false;
    bool sent = false;   // (its id was in the previous publish - INV-J)
    std::string cls, name;
    bool operator==(const T& o) const {
        return id == o.id && tuner == o.tuner && hits == o.hits && snr == o.snr && hz == o.hz && bw == o.bw && seen_real == o.seen_real &&
               fresh == o.fresh && sent == o.sent && cls == o.cls && name == o.name;
    }
};

using Dedupe = std::function<void(std::vector<T>&)>;

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
    for (size_t i = 0; i < pub.size(); i++) {
        if (drop[i]) continue;
        // pub[w] = std::move(pub[i]) with w == i: the release build's self-move left the strings empty - made
        // explicit here (what a self-move does is up to the library and its debug mode)
        if (w == i) pub[i].cls.clear(), pub[i].name.clear();
        else pub[w] = std::move(pub[i]);
        w++;
    }
    pub.resize(w);
}
// M2: the old pairwise scan with a correct compaction (only the scan's holes)
void m2_pairwise(std::vector<T>& pub) {
    std::sort(pub.begin(), pub.end(), [](const T& a, const T& b) { return a.hz < b.hz; });
    std::vector<uint8_t> drop(pub.size(), 0);
    for (size_t i = 0; i < pub.size(); i++)
        for (size_t j = i + 1; j < pub.size() && pub[j].hz - pub[i].hz < 0.5 * std::max(pub[i].bw, pub[j].bw) + 500.0; j++) {
            if (drop[i] || drop[j] || !R::same_signal(pub[i], pub[j])) continue;
            drop[R::better(pub[i], pub[j]) ? j : i] = 1;
        }
    std::vector<T> out;
    for (size_t i = 0; i < pub.size(); i++)
        if (!drop[i]) out.push_back(pub[i]);
    pub = out;
}
// M3: one tuner's tracks may drop each other (no tuner test)
void m3_no_tuner(std::vector<T>& pub) {
    std::vector<std::pair<int, int>> map;   // (made distinct per track, then restored)
    for (size_t i = 0; i < pub.size(); i++) {
        map.push_back({int(i) + 1000, pub[i].tuner});
        pub[i].tuner = int(i) + 1000;
    }
    R::drop_overlap_duplicates(pub);
    for (T& k : pub)
        for (auto& m : map)
            if (m.first == k.tuner) {
                k.tuner = m.second;
                break;
            }
}
// M4: the worse one kept
void m4_worse(std::vector<T>& pub) {
    for (T& k : pub) k.fresh = !k.fresh, k.snr = -k.snr, k.seen_real = -k.seen_real, k.hits = -k.hits;
    R::drop_overlap_duplicates(pub);
    for (T& k : pub) k.fresh = !k.fresh, k.snr = -k.snr, k.seen_real = -k.seen_real, k.hits = -k.hits;
}

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
            if (R::same_signal(out[i], out[j])) return "two kept tracks are one signal";
    std::set<int> tuners;
    for (const T& k : in) tuners.insert(k.tuner);
    for (const T& s : in) {
        if (ids.count(s.id)) continue;
        if (tuners.size() == 1) return "a single tuner's track dropped";
        bool ok = false;
        for (const T& k : out) ok = ok || (k.tuner != s.tuner && R::same_signal(k, s) && R::better(k, s));
        if (!ok) return "a dropped track without a better kept view of it on another tuner (id " + std::to_string(s.id) + ")";
    }
    std::vector<T> again = out;
    f(again);
    if (!(again.size() == out.size() && std::equal(again.begin(), again.end(), out.begin()))) return "not idempotent";
    return "";
}

T mk(int id, int tuner, double hz, double bw, float snr, bool fresh) {
    T x;
    x.id = id;
    x.tuner = tuner;
    x.hz = hz;
    x.bw = bw;
    x.snr = snr;
    x.fresh = fresh;
    x.seen_real = 1000;
    x.hits = 3;
    x.cls = "class-" + std::to_string(id);
    x.name = "Class name number " + std::to_string(id);   // (longer than the SSO buffer)
    return x;
}
// directed inputs: each exposes one of the mutants (and the real rule passes them)
std::vector<std::vector<T>> dedupe_directed() {
    return {
        // M1: a kept track in front of the first dropped one (its strings emptied)
        {mk(1, 0, 99e6, 2000, 10, true), mk(2, 0, 100e6, 2000, 10, true), mk(3, 1, 100e6, 2000, 5, false)},
        // M2: a chain a ~ b ~ c (not a ~ c), each better than the one below: the pairwise scan drops a and b
        {mk(1, 0, 100e6, 2000, 10, false), mk(2, 1, 100e6 + 1400, 2000, 20, false), mk(3, 2, 100e6 + 2800, 2000, 30, false)},
        // M3: one tuner's two views of one signal
        {mk(1, 0, 100e6, 2000, 10, true), mk(2, 0, 100e6 + 100, 2000, 12, true)},
        // M4: two tuners' views, one fresh
        {mk(1, 0, 100e6, 2000, 10, true), mk(2, 1, 100e6 + 100, 2000, 10, false)},
    };
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
                x.sent = rng() % 5 == 0;   // (sent in the previous publish - INV-J)
                x.name = "Class name number " + std::to_string(x.id);
                v.push_back(x);
            }
        }
    }
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}

void run_dedupe(int rounds, uint64_t seed) {
    const std::vector<std::tuple<const char*, Dedupe, bool>> cases = {
        {"drop_overlap_duplicates", [](std::vector<T>& v) { R::drop_overlap_duplicates(v); }, false},
        {"mutant: 9751af3's overlap code (self-move)", m1_old, true},
        {"mutant: pairwise scan", m2_pairwise, true},
        {"mutant: no tuner test", m3_no_tuner, true},
        {"mutant: the worse view kept", m4_worse, true},
    };
    const auto dir = dedupe_directed();
    for (const auto& [name, f, mutant] : cases) {
        int db = 0, rb = 0;
        std::string first;
        for (const auto& in : dir) {
            const std::string e = check_dedupe(in, f);
            if (!e.empty() && !db++) first = e;
        }
        std::mt19937_64 rng(seed);
        for (int r = 0; r < rounds; r++) {
            const std::string e = check_dedupe(random_tracks(rng), f);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(name, mutant, db, int(dir.size()), rb, rounds, first);
    }
}

// --- the sent view is sticky (INV-J) -----------------------------------------------
constexpr double STICKY_TEST_DB = R::STICKY_DB + 0.05;   // (clearly over the margin, float rounding aside)
// mutants: run the real rule on a changed copy, keep the originals of the views it keeps
Dedupe on_copy(const std::function<void(T&)>& change) {
    return [change](std::vector<T>& pub) {
        std::vector<T> c = pub;
        for (T& k : c) change(k);
        R::drop_overlap_duplicates(c);
        std::vector<T> out;
        for (const T& k : c)
            for (const T& o : pub)
                if (o.id == k.id) out.push_back(o);
        pub = out;
    };
}
const Dedupe m_not_sticky = on_copy([](T& k) { k.sent = false; });                      // 1e8871d..be18a73
const Dedupe m_no_margin = on_copy([](T& k) { if (k.sent) k.snr += 1000.0f; });          // the sent view always stays
const Dedupe m_over_fresh = on_copy([](T& k) { if (k.sent) k.fresh = true; });           // ... even stale against a fresh one
const Dedupe m_over_class = on_copy([](T& k) { if (k.sent && k.cls.empty()) k.cls = "x"; });   // ... or without a class

std::set<int> ids_of(const std::vector<T>& v) {
    std::set<int> s;
    for (const T& k : v) s.insert(k.id);
    return s;
}
std::vector<T> marked(std::vector<T> in, const std::set<int>& sent) {
    for (T& k : in) k.sent = sent.count(k.id) > 0;
    return in;
}
std::vector<T> run_rule(const Dedupe& f, std::vector<T> in) {
    f(in);
    return in;
}

// x0: views (none marked). Publishes again and again, each the views fed back as sent and every SNR moved by
// less than 1.5 dB from x0 (pairwise < STICKY_DB): the same views every time; the earlier invariants each time
std::string check_sticky_steady(const std::vector<T>& x0_in, const Dedupe& f, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> J(-1.49, 1.49);
    const std::vector<T> x0 = marked(x0_in, {});   // (chosen without marks: the SNRs then are the reference)
    const std::set<int> y0 = ids_of(run_rule(f, x0));
    std::set<int> prev = y0;
    for (int r = 0; r < 8; r++) {
        std::vector<T> x = marked(x0, prev);
        for (T& k : x) k.snr = float(double(k.snr) + J(rng));
        const std::string e = check_dedupe(x, f);
        if (!e.empty()) return e;
        prev = ids_of(run_rule(f, x));
        if (prev != y0) return "the view sent changed with SNRs within the margin (INV-J: no alternation)";
    }
    return "";
}

// signals seen by 2-3 tuners (one view each, the same centre), none chained: the sent view S and the best other O.
// Within the margin S stays; O 3 dB over S, S gone, S stale while O fresh, S without a class while O has one: O
std::vector<T> random_multiview(std::mt19937_64& rng) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int signals = 1 + int(rng() % 8), tuners = 2 + int(rng() % 3);
    std::vector<T> v;
    int id = 1;
    for (int s = 0; s < signals; s++) {
        const double hz = 416e6 + s * 100e3, bw = 1000 + 20000 * U(rng);
        const int views = 2 + int(rng() % uint64_t(tuners - 1));
        for (int t = 0; t < views; t++) {
            T x = mk(id++, t, hz, bw, float(5.0 + 30.0 * U(rng)), true);
            x.seen_real = 1000.0 + std::floor(U(rng) * 3.0);
            x.hits = 1 + int(rng() % 4);
            v.push_back(x);
        }
    }
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}
std::string check_takeover(const std::vector<T>& x0, const Dedupe& f, std::mt19937_64& rng) {
    const std::set<int> y0 = ids_of(run_rule(f, x0));
    // per signal (hz): S = the one sent, O = a random other view
    std::map<long long, std::vector<size_t>> by;   // (a signal's views: within 5 kHz - the test inputs space signals 100 kHz)
    for (size_t i = 0; i < x0.size(); i++) by[std::llround(x0[i].hz / 10000.0)].push_back(i);
    for (const auto& [hz, ix] : by) {
        size_t s = SIZE_MAX;
        for (size_t i : ix)
            if (y0.count(x0[i].id)) s = i;
        if (s == SIZE_MAX) return "a signal not sent";
        std::vector<size_t> others;
        for (size_t i : ix)
            if (i != s) others.push_back(i);
        if (others.empty()) return "a signal with one view (the test input)";
        const size_t o = others[rng() % others.size()];
        const int sid = x0[s].id, oid = x0[o].id;
        const auto sent_now = [&](std::vector<T> x) {   // the view of this signal sent now
            const std::set<int> y = ids_of(run_rule(f, marked(std::move(x), y0)));
            int n = 0, which = -1;
            for (size_t i : ix)
                if (y.count(x0[i].id)) n++, which = x0[i].id;
            return n == 1 ? which : -1;
        };
        std::vector<T> x = x0;
        // the others all within the margin below or above S: S stays
        for (size_t i : others) x[i].snr = float(double(x0[s].snr) + 2.9 * (2.0 * (double(rng() % 1000) / 1000.0) - 1.0));
        if (sent_now(x) != sid) return "the sent view lost to one less than 3 dB stronger (INV-J)";
        // (c) O 3 dB over S, the rest below S
        x = x0;
        for (size_t i : others) x[i].snr = float(double(x0[s].snr) - 1.0);
        x[o].snr = float(double(x0[s].snr) + STICKY_TEST_DB);
        if (sent_now(x) != oid) return "a view 3 dB stronger did not take over (INV-J c)";
        // (a) S gone; S stale while O fresh
        x = x0;
        for (size_t i : others) x[i].snr = float(double(x0[s].snr) - 5.0);
        x[o].snr = x0[s].snr - 2.0f;
        std::vector<T> gone;
        for (size_t i = 0; i < x.size(); i++)
            if (i != s) gone.push_back(x[i]);
        {
            const std::set<int> y = ids_of(run_rule(f, marked(gone, y0)));
            if (!y.count(oid)) return "the sent view went, the other not sent (INV-J a)";
        }
        x[s].fresh = false;
        for (size_t i : others)
            if (i != o) x[i].fresh = false;
        if (sent_now(x) != oid) return "the sent view stale, a fresh one not sent (INV-J a)";
        // (b) S without a class while O has one
        x = x0;
        for (size_t i : others) x[i].snr = x0[s].snr - 2.0f, x[i].cls.clear();
        x[o].cls = "class-o";
        x[s].cls.clear();
        if (sent_now(x) != oid) return "the sent view without a class, a classed one not sent (INV-J b)";
    }
    return "";
}

// the live case: two tuners on one signal 0.2 dB apart, the lead swapping with every look
std::vector<T> sticky_live() {
    T a = mk(1, 0, 416090000, 6500, 26.6f, true), b = mk(2, 1, 416090050, 6400, 26.4f, true);
    a.cls = "dmr", b.cls = "mpt1327";
    return {a, b};
}

void run_sticky(int rounds, uint64_t seed) {
    const std::vector<std::tuple<const char*, Dedupe, bool>> cases = {
        {"the sent view sticky (INV-J)", [](std::vector<T>& v) { R::drop_overlap_duplicates(v); }, false},
        {"mutant: be18a73's rule (not sticky)", m_not_sticky, true},
        {"mutant: sticky without the 3 dB margin", m_no_margin, true},
        {"mutant: sticky over a fresh view", m_over_fresh, true},
        {"mutant: sticky over a classed view", m_over_class, true},
    };
    for (const auto& [name, f, mutant] : cases) {
        int db = 0, dn = 0, rb = 0;
        std::string first;
        const auto dir = [&](const std::string& e) {
            dn++;
            if (!e.empty() && !db++) first = e;
        };
        {   // directed: the live case (the SNRs swap by 0.4 dB between publishes) ...
            std::mt19937_64 g(1);
            std::vector<T> x = sticky_live();
            std::set<int> prev = ids_of(run_rule(f, x));
            std::string e;
            for (int r = 0; r < 6 && e.empty(); r++) {
                std::swap(x[0].snr, x[1].snr);
                const std::set<int> y = ids_of(run_rule(f, marked(x, prev)));
                if (y != prev) e = "the view sent alternates with the SNRs 0.2 dB apart (INV-J)";
                prev = y;
            }
            dir(e);
            dir(check_sticky_steady(sticky_live(), f, g));
            // ... and the takeovers on fixed signals: two and three views, a chain a ~ b ~ c
            std::mt19937_64 g2(2);
            dir(check_takeover(sticky_live(), f, g2));
            std::vector<T> three = {mk(1, 0, 100e6, 5000, 20, true), mk(2, 1, 100e6, 5000, 19, true), mk(3, 2, 100e6, 5000, 18, true)};
            dir(check_takeover(three, f, g2));
            std::vector<T> chain = {mk(1, 0, 100e6, 2000, 20, true), mk(2, 1, 100e6 + 1400, 2000, 21, true), mk(3, 2, 100e6 + 2800, 2000, 20.5f, true)};
            dir(check_sticky_steady(chain, f, g2));
        }
        std::mt19937_64 rng(seed);
        for (int r = 0; r < rounds / 20; r++) {
            const std::string e = r % 2 ? check_takeover(random_multiview(rng), f, rng) : check_sticky_steady(random_tracks(rng), f, rng);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(name, mutant, db, dn, rb, rounds / 20, first);
    }
}

// --- bands (band_reps) -----------------------------------------------------------
using R::TunerSlot;
uint64_t dist(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
using Reps = std::function<std::vector<int>(const std::vector<TunerSlot>&)>;
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
        if (dist(t[i].hz, t[size_t(r)].hz) > R::SAME_BAND_HZ) return "a tuner on another band than its representative";
        for (size_t j = 0; j < t.size(); j++)
            if (j != i && rep[i] == int(i) && rep[j] == int(j) && t[j].hz && dist(t[i].hz, t[j].hz) <= R::SAME_BAND_HZ)
                return "two representatives on one band";
    }
    return "";
}
std::vector<int> reps_chain(const std::vector<TunerSlot>& t) {   // mutant: joins any earlier tuner, not its band's representative
    std::vector<int> rep(t.size(), -1);
    for (size_t i = 0; i < t.size(); i++) {
        if (!t[i].hz) continue;
        rep[i] = int(i);
        for (size_t j = 0; j < i; j++)
            if (t[j].hz && dist(t[i].hz, t[j].hz) <= R::SAME_BAND_HZ) {
                rep[i] = int(j);
                break;
            }
    }
    return rep;
}
std::vector<TunerSlot> slots_of(const std::vector<uint64_t>& hz) {
    std::vector<TunerSlot> t(hz.size());
    for (size_t i = 0; i < hz.size(); i++) t[i].hz = hz[i];
    return t;
}
std::vector<TunerSlot> random_slots(std::mt19937_64& rng) {
    const int M = 1 + int(rng() % 8);
    std::vector<TunerSlot> t(static_cast<size_t>(M));
    const uint64_t base[] = {416815000, 416815000, 416815500, 416816200, 416760000, 859440000, 98000000, 0};
    for (auto& s : t) {
        s.hz = base[rng() % 8] + (rng() % 4 == 0 ? rng() % 1500 : 0);
        if (s.hz < 1000000) s.hz = 0;
    }
    return t;
}
void run_reps(int rounds, uint64_t seed) {
    // directed: 800 Hz steps - tuner 2 is within 1 kHz of tuner 1 (on tuner 0's band) but not of tuner 0
    const std::vector<std::vector<TunerSlot>> dir = {slots_of({100000000, 100000800, 100001600}), slots_of({0, 5, 100000000, 100000900})};
    const std::vector<std::pair<const char*, Reps>> rc = {{"band_reps", R::band_reps}, {"mutant: reps chained", reps_chain}};
    for (size_t c = 0; c < rc.size(); c++) {
        int db = 0, rb = 0;
        std::string first;
        for (const auto& in : dir) {
            const std::string e = check_reps(in, rc[c].second);
            if (!e.empty() && !db++) first = e;
        }
        std::mt19937_64 rng(seed);
        for (int r = 0; r < rounds; r++) {
            const std::string e = check_reps(random_slots(rng), rc[c].second);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(rc[c].first, c > 0, db, int(dir.size()), rb, rounds, first);
    }
}

// --- the worker loop: a model on the real rules -----------------------------------
// Faithful to sigclass_service.cpp's worker: a restart (Signal ID switched on, a
// mode change) at its loop top; coherent mode looks at one tuner every period
// (never ahead); with tuners in turn update_bands before each pick, the
// "every band settling" wait, the budget's rest (unless a first look the debt
// allows; a restart event ends it), a capture of 560 ms that a retune of its
// tuner aborts (its settle, update_bands, then the wait for its settle - once,
// L3) or a restart event aborts, the analysis, note_look.
using R::Regroup;
using Pick = std::function<int(const std::vector<TunerSlot>&, const std::vector<int>&, int&, int64_t, bool&)>;
using RegroupFn = std::function<Regroup(const std::vector<uint64_t>&, const std::vector<TunerSlot>&, const std::vector<int>&)>;

// fb5cf77's pick: a band without a look always first, in cursor order, the cursor moved past it
int pick_fb5(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    const int M = int(t.size());
    first = false;
    if (!M) return -1;
    rr = ((rr % M) + M) % M;
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++)
        for (int k = 0; k < M; k++) {
            const int i = (rr + k) % M;
            if (rep[size_t(i)] != i || now < t[size_t(i)].settle_until_ms) continue;
            if (pass == 0 && t[size_t(i)].looks != 0) continue;
            pick = i;
            first = pass == 0;
            break;
        }
    if (pick >= 0) rr = (pick + 1) % M;
    return pick;
}
int pick_no_token(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    std::vector<TunerSlot> u = t;
    for (auto& s : u) s.prio_seq = INT64_MIN;
    return R::pick_band(u, rep, rr, now, first);
}
int pick_no_first(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    std::vector<TunerSlot> u = t;
    for (auto& s : u) s.looks += 1;
    return R::pick_band(u, rep, rr, now, first);
}
// 97f8d78's pick: ties in cursor order only (with the restart that keeps the bands: does the tie rule matter?)
int pick_cursor_ties(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now_ms, bool& first_look) {
    const int M = int(t.size());
    first_look = false;
    if (M == 0) return -1;
    rr = ((rr % M) + M) % M;
    const auto ready = [&](int i) { return rep[size_t(i)] == i && t[size_t(i)].hz != 0 && now_ms >= t[size_t(i)].settle_until_ms; };
    const auto token = [&](int i) {
        for (int j = 0; j < M; j++)
            if (j != i && ready(j) && !(t[size_t(j)].last_seq > t[size_t(i)].prio_seq)) return false;
        return true;
    };
    int pick = -1;
    for (int k = 0; k < M; k++) {
        const int i = (rr + k) % M;
        if (!ready(i) || t[size_t(i)].looks != 0 || !token(i)) continue;
        if (pick < 0 || t[size_t(i)].ready_ms < t[size_t(pick)].ready_ms) pick = i;
    }
    if (pick >= 0) {
        first_look = true;
        return pick;
    }
    for (int k = 0; k < M; k++) {
        const int i = (rr + k) % M;
        if (!ready(i)) continue;
        if (pick < 0 || t[size_t(i)].turn_seq < t[size_t(pick)].turn_seq) pick = i;
    }
    if (pick >= 0) rr = (pick + 1) % M;
    return pick;
}
int pick_no_settle(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t, bool& first) {
    return R::pick_band(t, rep, rr, INT64_MAX / 2, first);
}
// fb5cf77's trackers: each stays with its tuner (a band's tracks lost when its lead changes)
Regroup regroup_stay(const std::vector<uint64_t>& old_band_hz, const std::vector<TunerSlot>& t, const std::vector<int>& rep) {
    Regroup g{std::vector<int>(t.size(), -1), std::vector<uint8_t>(t.size(), 0)};
    for (size_t r = 0; r < t.size(); r++)
        if (rep[r] == int(r)) {
            g.src[r] = int(r);
            g.same[r] = r < old_band_hz.size() && old_band_hz[r] && dist(old_band_hz[r], t[r].hz) <= R::SAME_BAND_HZ;
        }
    return g;
}

enum EvKind { EV_RETUNE, EV_OFF, EV_COHERENT };
struct Ev {
    int64_t t;
    int kind;
    int tuner;       // EV_RETUNE
    uint64_t hz;     // EV_RETUNE
    int64_t dur;     // EV_OFF: Signal ID off for dur; EV_COHERENT: coherent mode for dur
};
struct Scen {
    std::string name;
    int M = 5;
    double ana_s = 0.3;
    double dur_s = 600;
    std::vector<uint64_t> base;   // the tuners' frequencies at the start
    std::vector<Ev> ev;           // sorted by time
};
struct SimOpt {
    Pick pick = R::pick_band;
    bool reset_debt_on_enable = false;   // mutant of 9751af3..1e8871d
    bool copy_token = false;             // mutant: the token goes with the band
    bool new_band_first = false;         // mutant: a new band's turn from before every look (not from its start)
    bool restart_clears = false;         // mutant: 97f8d78's restart (the slots cleared: tokens, latest looks)
    bool restart_tuner_only = false;     // mutant: a restart that keeps only each tuner's latest look and token (every band new)
    bool retry = true;                   // L3: a capture a retune aborted is tried again once its tuner settled
};
struct SimOut {
    std::string err;
    std::vector<int64_t> looks;   // per tuner (as its band's lead)
    int64_t attempts = 0, aborted = 0, n = 0;
};

constexpr int64_t CAPTURE_MS = 560, SETTLE_MS = 500;

SimOut sim(const Scen& sc, const SimOpt& o) {
    SimOut out;
    const int M = sc.M;
    out.looks.assign(size_t(M), 0);
    const int64_t dur = int64_t(sc.dur_s * 1000);
    std::vector<uint64_t> hz = sc.base;
    std::vector<int> epoch(size_t(M), 0);   // retunes per tuner
    size_t ei = 0;
    int64_t off_until = -1, coh_until = -1;
    bool has_retunes = false;
    std::vector<int64_t> wakes;   // when a rest / a capture ends early: Signal ID off, a mode change
    for (const Ev& e : sc.ev) {
        if (e.kind == EV_RETUNE) has_retunes = true;
        if (e.kind != EV_RETUNE) wakes.push_back(e.t);
        if (e.kind == EV_COHERENT) wakes.push_back(e.t + e.dur);
    }
    std::sort(wakes.begin(), wakes.end());
    const auto world = [&](int64_t t) {
        while (ei < sc.ev.size() && sc.ev[ei].t <= t) {
            const Ev& e = sc.ev[ei++];
            if (e.kind == EV_RETUNE) {
                if (hz[size_t(e.tuner)] != e.hz) epoch[size_t(e.tuner)]++;
                hz[size_t(e.tuner)] = e.hz;
            } else if (e.kind == EV_OFF) {
                off_until = std::max(off_until, e.t + e.dur);
            } else {
                coh_until = std::max(coh_until, e.t + e.dur);
            }
        }
    };
    const auto next_wake = [&](int64_t t) {
        const auto it = std::upper_bound(wakes.begin(), wakes.end(), t);
        return it == wakes.end() ? INT64_MAX : *it;
    };
    std::vector<TunerSlot> slots;
    std::vector<uint64_t> band_hz;
    std::vector<int> reps;
    R::Budget budget;
    budget.share = 0.04;
    budget.min_s = 1.0;
    budget.max_s = 10.0;
    double ewma = 0, used = 0, excess = 0, max_cap = 0;
    int64_t now = 0, last_start = -1, seq = 0;
    int rr = 0, last_mode = -1, restarts = 0, retried = -1;
    bool was_off = false;
    struct LookRec {
        int pick;
        std::vector<std::pair<int, uint64_t>> ready;
        std::vector<int> epoch;
    };
    std::vector<LookRec> L;
    // INV-C: overtakes of a waiting band by later-waiting ones
    std::map<std::tuple<int, int, int, int>, int> over;   // (waiting band, its retune epoch, the restart, the later one) -> overtakes
    const auto restart = [&]() {
        restarts++;
        if (o.restart_clears) {
            slots.clear();
            reps.clear();
            band_hz.clear();
        } else if (o.restart_tuner_only) {
            for (TunerSlot& x : slots) {
                const int64_t last = x.last_seq, prio = x.prio_seq;
                x = TunerSlot{};
                x.last_seq = last;
                x.prio_seq = prio;
            }
            reps.clear();
            band_hz.clear();
        } else {
            R::restart_bands(slots, now);
        }
    };
    const auto update = [&]() {
        bool moved = false;
        R::update_bands(slots, band_hz, reps, hz, now, SETTLE_MS, o.new_band_first ? -1 : seq, moved);
        if (o.copy_token)   // (the token goes with the band: a tuner hopping onto one and off again takes it along)
            for (int i = 0; i < M; i++)
                if (reps[size_t(i)] >= 0 && reps[size_t(i)] != i) slots[size_t(i)].prio_seq = slots[size_t(reps[size_t(i)])].prio_seq;
    };
    const auto look_done = [&](int64_t cycle0) {   // the budget's side of a look
        if (last_start >= 0) budget.started(double(cycle0 - last_start) / 1000.0, ewma);
        last_start = cycle0;
        now += int64_t(sc.ana_s * 1000);
        used += sc.ana_s;
        excess += std::max(0.0, sc.ana_s - 10.0 * 0.04);
        ewma = ewma > 0 ? ewma + 0.3 * (sc.ana_s - ewma) : sc.ana_s;
        max_cap = std::max(max_cap, 8.0 * ewma);
        budget.plan(ewma);
        // INV-G: the analysis within the share, the debt's cap, the clamp's excess and the latest look
        if (used > 0.04 * (double(now) / 1000.0) + max_cap + excess + sc.ana_s + 1e-6) out.err = "more analysis than the budget allows (INV-G)";
    };
    while (now < dur && out.err.empty()) {
        world(now);
        if (now < off_until) {   // off: idle until switched on again
            now = off_until;
            was_off = true;
            continue;
        }
        if (was_off) {   // switched on: the bands start again (the budget goes on)
            was_off = false;
            restart();
            if (o.reset_debt_on_enable) {
                budget.debt_s = 0;
                last_start = -1;
            }
        }
        const int mode = now < coh_until ? 0 : 2;
        if (mode != last_mode) {
            last_mode = mode;
            restart();
        }
        if (mode == 0) {   // coherent: one tuner, every period, never ahead
            if (last_start >= 0) {
                const double elapsed = double(now - last_start) / 1000.0;
                if (elapsed < budget.planned_s) {
                    now = std::min(now + std::max<int64_t>(1, int64_t(std::ceil((budget.planned_s - elapsed) * 1000.0))), next_wake(now));
                    continue;
                }
            }
            const int64_t cycle0 = now, w = next_wake(now);
            if (w <= now + CAPTURE_MS) {
                now = w;
                continue;
            }
            now += CAPTURE_MS;
            look_done(cycle0);
            continue;
        }
        update();
        int rr_next = rr;
        bool first = false;
        const int ch = o.pick(slots, reps, rr_next, now, first);
        if (ch >= 0 && (reps[size_t(ch)] != ch || slots[size_t(ch)].hz == 0 || now < slots[size_t(ch)].settle_until_ms)) {
            out.err = "picked a tuner that is not a ready band";
            break;
        }
        if (ch < 0) {
            int64_t wait = -1;
            for (int i = 0; i < M; i++)
                if (reps[size_t(i)] == i && slots[size_t(i)].settle_until_ms > now)
                    wait = wait < 0 ? slots[size_t(i)].settle_until_ms - now : std::min(wait, slots[size_t(i)].settle_until_ms - now);
            now = std::min(now + (wait >= 0 ? std::max<int64_t>(wait, 20) : 1000), next_wake(now));
            continue;
        }
        if (last_start >= 0) {
            const double elapsed = double(now - last_start) / 1000.0;
            const bool skip = first && budget.may_skip(elapsed, ewma, 8.0 * ewma);
            if (!skip && elapsed < budget.planned_s) {
                now = std::min(now + std::max<int64_t>(1, int64_t(std::ceil((budget.planned_s - elapsed) * 1000.0))), next_wake(now));
                continue;
            }
        }
        const int64_t cycle0 = now;
        rr = rr_next;
        out.attempts++;
        // the capture: aborted by a retune of its tuner or a restart event
        int64_t abort_t = INT64_MAX;
        {
            uint64_t h = hz[size_t(ch)];
            for (size_t k = ei; k < sc.ev.size() && sc.ev[k].t <= now + CAPTURE_MS; k++) {
                const Ev& e = sc.ev[k];
                if (e.kind == EV_RETUNE && e.tuner == ch && e.hz != h) {
                    abort_t = e.t;
                    break;
                }
            }
        }
        const int64_t w = next_wake(now);
        if (w <= now + CAPTURE_MS && w <= abort_t) {
            now = w;
            continue;
        }
        if (abort_t != INT64_MAX) {
            now = abort_t;
            world(now);
            out.aborted++;
            slots[size_t(ch)].settle_until_ms = now + SETTLE_MS;
            update();   // (age_out)
            if (o.retry && retried != ch) {   // L3: wait for it to settle, then choose again (once)
                retried = ch;
                now = std::min(now + SETTLE_MS, next_wake(now));
            } else {
                retried = -1;
            }
            continue;
        }
        retried = -1;
        // a look: record who was ready at its decision
        LookRec rec{ch, {}, epoch};
        for (int i = 0; i < M; i++)
            if (reps[size_t(i)] == i && slots[size_t(i)].hz && cycle0 >= slots[size_t(i)].settle_until_ms) rec.ready.push_back({i, slots[size_t(i)].hz});
        for (int wb = 0; wb < M; wb++) {   // INV-C: the waiting bands this pick overtakes
            if (wb == ch || reps[size_t(wb)] != wb || !slots[size_t(wb)].hz || cycle0 < slots[size_t(wb)].settle_until_ms || slots[size_t(wb)].looks)
                continue;
            if (slots[size_t(ch)].looks == 0 && slots[size_t(ch)].ready_ms > slots[size_t(wb)].ready_ms &&
                ++over[std::make_tuple(wb, epoch[size_t(wb)], restarts, ch)] > 1) {
                out.err = "a waiting band overtaken twice by one that began waiting later (INV-C)";
                break;
            }
        }
        L.push_back(rec);
        out.looks[size_t(ch)]++;
        now += CAPTURE_MS;
        look_done(cycle0);
        if (g_trace) {
            std::fprintf(g_trace, "look %lld t %lld pick %d first %d restarts %d ready", (long long)seq, (long long)cycle0, ch, int(first), restarts);
            for (const auto& q : rec.ready) std::fprintf(g_trace, " %d", q.first);
            for (int i = 0; i < M; i++)
                std::fprintf(g_trace, " | %d: rep %d looks %llu last %lld turn %lld prio %lld rdy %lld st %lld", i, reps[size_t(i)], (unsigned long long)slots[size_t(i)].looks, (long long)slots[size_t(i)].last_seq, (long long)slots[size_t(i)].turn_seq, (long long)(slots[size_t(i)].prio_seq == INT64_MIN ? -9 : slots[size_t(i)].prio_seq), (long long)slots[size_t(i)].ready_ms, (long long)slots[size_t(i)].settle_until_ms);
            std::fprintf(g_trace, "\n");
        }
        R::note_look(slots, reps, ch, first, seq++);
        for (auto it = over.begin(); it != over.end();) it = std::get<0>(it->first) == ch ? over.erase(it) : std::next(it);
    }
    out.n = int64_t(L.size());
    if (!out.err.empty()) return out;
    // INV-H (nothing retuned; restarts at any rate): in every stretch of >= 4R looks every band gets >= 1 / (2R) of them.
    // Stretches of 4R .. 8R - 1 looks suffice: a longer one splits into such pieces.
    if (!has_retunes && !L.empty()) {
        const size_t Rn = L[0].ready.size();
        std::vector<std::vector<int>> pre(size_t(M), std::vector<int>(L.size() + 1, 0));
        for (size_t j = 0; j < L.size(); j++)
            for (int b = 0; b < M; b++) pre[size_t(b)][j + 1] = pre[size_t(b)][j] + (L[j].pick == b ? 1 : 0);
        for (size_t k = 0; k < L.size(); k++)
            for (size_t len = 4 * Rn; len < 8 * Rn && k + len <= L.size(); len++)
                for (const auto& b : L[k].ready) {
                    const int got = pre[size_t(b.first)][k + len] - pre[size_t(b.first)][k];
                    if (size_t(2 * Rn) * size_t(got) < len) {
                        out.err = "a band with less than 1 / (2R) of a stretch's looks across restarts (INV-H)";
                        return out;
                    }
                }
        // ... and what the rules give: between two looks of a band every other band has one (a round robin of the
        // latest looks - so every R consecutive looks hold every band)
        std::vector<int64_t> last(size_t(M), -1);
        for (size_t j = 0; j < L.size(); j++) {
            const int p = L[j].pick;
            if (last[size_t(p)] >= 0)
                for (const auto& b : L[j].ready)
                    if (b.first != p && last[size_t(b.first)] < last[size_t(p)]) {
                        out.err = "two looks of a band without one of another across restarts (INV-H, the round robin)";
                        return out;
                    }
            last[size_t(p)] = int64_t(j);
        }
    }
    // INV-A: a band ready through 2R consecutive looks (R the tuners standing for a ready band in them) and not retuned gets one
    for (size_t k = 0; k < L.size(); k++) {
        size_t len = 2 * std::max<size_t>(1, L[k].ready.size());
        for (int it = 0; it < 8; it++) {
            std::set<int> who;   // (R: the tuners standing for a ready band at any look in the stretch - each its own token)
            for (size_t j = k; j < std::min(L.size(), k + len); j++)
                for (const auto& q : L[j].ready) who.insert(q.first);
            const size_t Rn = std::max<size_t>(1, who.size());
            if (2 * Rn <= len) break;
            len = 2 * Rn;
        }
        if (k + len > L.size()) break;
        for (const auto& b : L[k].ready) {
            bool through = true, got = false;
            for (size_t j = k; j < k + len && through; j++) {
                through = L[j].epoch[size_t(b.first)] == L[k].epoch[size_t(b.first)] &&
                          std::find(L[j].ready.begin(), L[j].ready.end(), b) != L[j].ready.end();
                got = got || L[j].pick == b.first;
            }
            if (through && !got) {
                out.err = "a ready band without a look in 2R looks (INV-A)";
                if (g_trace) std::fprintf(g_trace, "INV-A: band %d from look %zu, %zu looks\n", b.first, k, len);
                return out;
            }
        }
    }
    return out;
}

// --- scenarios ----------------------------------------------------------------------
std::vector<uint64_t> spaced(int M) {
    std::vector<uint64_t> b(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) b[size_t(i)] = 100000000ull + uint64_t(i) * 5000000ull;
    return b;
}
void sort_ev(Scen& s) {
    std::stable_sort(s.ev.begin(), s.ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
}
// tuner i stepped every `every` ms by 300 kHz (7 steps), from `t0`
void add_scan(Scen& s, int i, int64_t every, int64_t t0) {
    int k = 0;
    for (int64_t t = t0; t < int64_t(s.dur_s * 1000); t += every) s.ev.push_back({t, EV_RETUNE, i, s.base[size_t(i)] + uint64_t(++k % 7) * 300000ull, 0});
}
Scen sc_scan(double ana) {   // one tuner retuned every 2.8 s (the reviewer's `scan`; fb5cf77 [1, 42, 41, 8, 1])
    Scen s{"scan", 5, ana, 600, spaced(5), {}};
    add_scan(s, 1, 2800, 2800);
    sort_ev(s);
    return s;
}
Scen sc_all() {   // four tuners retuned every 2 s (staggered), one steady
    Scen s{"four scanned", 5, 0.3, 600, spaced(5), {}};
    for (int i = 1; i < 5; i++) add_scan(s, i, 2000, 2000 + 500 * i);
    sort_ev(s);
    return s;
}
Scen sc_swap(double ana, bool jitter) {   // tuners 1 and 2 swap their bands every 2.8 s (+-20 %: a fixed sequence)
    Scen s{jitter ? "swap 2.8 s +-20 %" : "swap 2.8 s", 5, ana, 1200, spaced(5), {}};
    std::mt19937_64 g(7);   // (fixed: the same jitter for every seed)
    uint64_t a = s.base[1], b = s.base[2];
    for (int64_t t = 2800; t < int64_t(s.dur_s * 1000); t += jitter ? int64_t(2240 + g() % 1121) : 2800) {
        std::swap(a, b);
        s.ev.push_back({t, EV_RETUNE, 1, a, 0});
        s.ev.push_back({t, EV_RETUNE, 2, b, 0});
    }
    sort_ev(s);
    return s;
}
Scen sc_toggle(double ana, int64_t every, int M) {   // Signal ID off for `every`, on for `every` (no retunes)
    Scen s{"toggled every " + std::to_string(every) + " ms, M " + std::to_string(M), M, ana, 1200, spaced(M), {}};
    for (int64_t t = every; t < int64_t(s.dur_s * 1000); t += 2 * every) s.ev.push_back({t, EV_OFF, 0, 0, every});
    return s;
}
Scen sc_mode(double ana, int64_t every) {   // independent for `every`, coherent for every / 3 (no retunes)
    Scen s{"mode alternating every " + std::to_string(every) + " ms", 5, ana, 1200, spaced(5), {}};
    for (int64_t t = every; t < int64_t(s.dur_s * 1000); t += every + every / 3) s.ev.push_back({t, EV_COHERENT, 0, 0, every / 3});
    return s;
}
Scen sc_shared_toggle() {   // tuners 1-3 on one band, toggled every 1.1 s
    Scen s = sc_toggle(0.1, 1100, 6);
    s.name = "shared band, toggled every 1100 ms";
    s.base[2] = s.base[1];
    s.base[3] = s.base[1] + 600;
    return s;
}
// The rare mutants' sequences: random runs of the model the mutant failed (seed / run in the name), shrunk
// (fewer events, shorter, the analysis rounded where it still failed) while the mutant fails and the real rules hold. They
// hang on the model's exact timing: a change of the model or of the rules may need them found again
// (a mutant then shows "directed 0": the test fails rather than pass by luck).
// "the token copied with the band": a tuner hopping onto another tuner's band and off again took that
// band's token along and overtook a waiting band twice (INV-C)
Scen sc_copy1() {
    return {"token copy 1 (seed 1 run 90)", 5, 0.3, 40, {100000000ull, 105000000ull, 110000000ull, 115000000ull, 115000000ull}, {
        {1576, EV_RETUNE, 2, 110600000ull, 0},
        {2361, EV_RETUNE, 4, 115300000ull, 0},
        {2781, EV_RETUNE, 2, 110900000ull, 0},
        {3142, EV_RETUNE, 1, 115000000ull, 0},
        {3986, EV_RETUNE, 2, 111200000ull, 0},
        {11383, EV_RETUNE, 4, 206000000ull, 0},
        {13626, EV_RETUNE, 2, 111500000ull, 0},
        {14831, EV_RETUNE, 2, 115000000ull, 0},
        {16036, EV_RETUNE, 2, 110000000ull, 0},
    }};
}
Scen sc_copy2() {
    return {"token copy 2 (seed 2 run 682)", 7, 0.11031007470174936, 135,
            {100000000ull, 105000000ull, 110000000ull, 110000000ull, 110000000ull, 125000000ull, 125000000ull}, {
        {85377, EV_RETUNE, 4, 110600000ull, 0},
        {85526, EV_RETUNE, 6, 125600000ull, 0},
        {95037, EV_RETUNE, 4, 110300000ull, 0},
        {95466, EV_RETUNE, 6, 125900000ull, 0},
        {108011, EV_RETUNE, 2, 110600000ull, 0},
        {109887, EV_RETUNE, 6, 125000000ull, 0},
        {115131, EV_RETUNE, 6, 126200000ull, 0},
        {119064, EV_RETUNE, 6, 125000000ull, 0},
        {119187, EV_RETUNE, 4, 110600000ull, 0},
        {120060, EV_RETUNE, 2, 110900000ull, 0},
        {120375, EV_RETUNE, 6, 125300000ull, 0},
        {126930, EV_RETUNE, 6, 100000000ull, 0},
        {130779, EV_RETUNE, 4, 111500000ull, 0},
        {130863, EV_RETUNE, 6, 125600000ull, 0},
    }};
}
// "a new band first in the turn order": retuned bands queued ahead of a steady one (INV-A)
Scen sc_newfirst1() {
    return {"new band first 1 (seed 1 run 34)", 7, 0.11020039892453273, 230,
            {100000000ull, 105000000ull, 110000000ull, 115000000ull, 120000000ull, 125000000ull, 130000000ull}, {
        {188240, EV_RETUNE, 5, 126200000ull, 0},
        {191323, EV_RETUNE, 4, 221000000ull, 0},
        {192063, EV_RETUNE, 3, 120000000ull, 0},
        {193187, EV_RETUNE, 5, 126500000ull, 0},
        {203081, EV_RETUNE, 5, 125000000ull, 0},
        {206519, EV_RETUNE, 3, 115300000ull, 0},
        {208028, EV_RETUNE, 5, 125300000ull, 0},
        {212975, EV_RETUNE, 5, 125600000ull, 0},
    }};
}
Scen sc_newfirst2() {
    return {"new band first 2 (seed 1 run 64)", 7, 0.125, 445,
            {100000000ull, 100000000ull, 110000000ull, 115000000ull, 120000000ull, 125000000ull, 130000000ull}, {
        {399208, EV_RETUNE, 1, 101200000ull, 0},
        {400784, EV_RETUNE, 5, 125300000ull, 0},
        {405907, EV_RETUNE, 1, 101500000ull, 0},
        {407326, EV_RETUNE, 6, 218000000ull, 0},
        {407919, EV_RETUNE, 5, 125600000ull, 0},
        {422189, EV_RETUNE, 5, 126200000ull, 0},
        {426004, EV_RETUNE, 1, 100300000ull, 0},
        {429324, EV_RETUNE, 5, 126500000ull, 0},
        {432703, EV_RETUNE, 1, 100600000ull, 0},
        {437326, EV_RETUNE, 6, 130000000ull, 0},
        {439402, EV_RETUNE, 1, 100900000ull, 0},
    }};
}
// "ties in cursor order": three restarts, nothing retuned - a band two looks while another has none, below
// 1 / (2R) of a stretch (INV-H)
Scen sc_ties1() {
    return {"ties 1 (seed 1 run 235)", 6, 0.025, 40, {100000000ull, 105000000ull, 110000000ull, 110000000ull, 120000000ull, 125000000ull}, {
        {28916, EV_COHERENT, 0, 0ull, 440},
        {30190, EV_OFF, 0, 0ull, 528},
        {37544, EV_COHERENT, 0, 0ull, 597},
    }};
}
Scen sc_ties2() {
    return {"ties 2 (seed 1 run 11)", 7, 0.05, 450, spaced(7), {
        {15436, EV_OFF, 0, 0ull, 8931},
        {430812, EV_OFF, 0, 0ull, 16214},
    }};
}
// "a restart keeping the tuners' latest looks only" (every band new): a tuner on another's band took its
// lead's latest look at each restart and was "the oldest" again and again (INV-A)
Scen sc_tuneronly() {
    return {"tuner-only restart (seed 12345 run 697)", 4, 0.4, 340, {100000000ull, 100000000ull, 110000000ull, 115000000ull}, {
        {260540, EV_RETUNE, 1, 101500000ull, 0},
        {267904, EV_COHERENT, 0, 0ull, 1346},
        {269625, EV_RETUNE, 2, 111200000ull, 0},
        {270296, EV_RETUNE, 1, 100000000ull, 0},
        {270546, EV_RETUNE, 2, 110000000ull, 0},
        {273289, EV_COHERENT, 0, 0ull, 1346},
        {278674, EV_COHERENT, 0, 0ull, 1346},
        {280063, EV_RETUNE, 2, 110900000ull, 0},
        {280984, EV_RETUNE, 2, 115000000ull, 0},
        {281291, EV_RETUNE, 2, 110000000ull, 0},
        {284059, EV_COHERENT, 0, 0ull, 1346},
        {285282, EV_RETUNE, 2, 115000000ull, 0},
        {291422, EV_RETUNE, 2, 111500000ull, 0},
        {294829, EV_COHERENT, 0, 0ull, 1346},
        {296027, EV_RETUNE, 2, 115000000ull, 0},
        {296334, EV_RETUNE, 2, 110000000ull, 0},
        {305599, EV_COHERENT, 0, 0ull, 1346},
        {306772, EV_RETUNE, 2, 115000000ull, 0},
        {307079, EV_RETUNE, 2, 110000000ull, 0},
        {307386, EV_RETUNE, 2, 110300000ull, 0},
        {307693, EV_RETUNE, 2, 110600000ull, 0},
    }};
}
// five tuners retuned in a burst every 7 s (the "no token" mutant: a retuned band's first looks again and again)
Scen sc_burst() {
    Scen s{"retunes in a burst", 6, 0.1, 300, spaced(6), {}};
    for (int64_t t = 5000; t < int64_t(s.dur_s * 1000); t += 7000)
        for (int i = 1; i < 6; i++) s.ev.push_back({t + 400 * i, EV_RETUNE, i, s.base[size_t(i)] + uint64_t((t / 7000) % 5 + 1) * 200000ull, 0});
    sort_ev(s);
    return s;
}

Scen random_scen(std::mt19937_64& rng, bool retunes) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    Scen s;
    s.name = "random";
    s.M = 2 + int(rng() % 7);
    s.ana_s = 0.02 + 0.38 * U(rng);
    s.dur_s = 300 + 600 * U(rng);
    const int M = s.M;
    const int64_t dur = int64_t(s.dur_s * 1000);
    // the tuners: own bands, some sharing one; 1-4 retuned periodically (0.3-20 s), hopping among bands - now and
    // then onto another tuner's; random one-off retunes for 30 s
    s.base.resize(size_t(M));
    for (int i = 0; i < M; i++) s.base[size_t(i)] = U(rng) < 0.2 && i ? s.base[size_t(i - 1)] : 100000000ull + uint64_t(i) * 5000000ull;
    if (retunes) {
        std::vector<int64_t> period(size_t(M), 0), phase(size_t(M), 0);
        const int nret = 1 + int(rng() % 4);
        for (int k = 0; k < nret; k++) {
            const int i = int(rng() % uint64_t(M));
            period[size_t(i)] = int64_t(300 + (20000 - 300) * U(rng) * U(rng));
            phase[size_t(i)] = int64_t(U(rng) * double(period[size_t(i)]));
        }
        std::vector<int64_t> x_at;
        std::vector<int> x_tuner;
        std::vector<uint64_t> x_hz;
        if (U(rng) < 0.5)
            for (int k = 0; k < 20; k++) {
                x_at.push_back(int64_t(U(rng) * double(dur)));
                x_tuner.push_back(int(rng() % uint64_t(M)));
                x_hz.push_back(U(rng) < 0.3 ? s.base[rng() % uint64_t(M)] : 200000000ull + (rng() % 9) * 3000000ull);
            }
        const auto hz_at = [&](int i, int64_t t) -> uint64_t {
            uint64_t h = s.base[size_t(i)];
            if (period[size_t(i)]) {
                const int64_t m = ((t + phase[size_t(i)]) / period[size_t(i)]) % 7;
                h = m == 6 ? s.base[size_t((i + 1) % M)] : s.base[size_t(i)] + uint64_t(m) * 300000ull;   // (m 6: onto the next tuner's band)
            }
            for (size_t e = 0; e < x_at.size(); e++)
                if (x_tuner[e] == i && t >= x_at[e] && t < x_at[e] + 30000) h = x_hz[e];
            return h;
        };
        for (int i = 0; i < M; i++) s.base[size_t(i)] = hz_at(i, 0);
        // the change points
        for (int i = 0; i < M; i++) {
            std::set<int64_t> at;
            if (period[size_t(i)])
                for (int64_t t = period[size_t(i)] - phase[size_t(i)]; t < dur; t += period[size_t(i)]) at.insert(t);
            for (size_t e = 0; e < x_at.size(); e++)
                if (x_tuner[e] == i) at.insert(x_at[e]), at.insert(x_at[e] + 30000);
            uint64_t h = s.base[size_t(i)];
            for (int64_t t : at) {
                if (t <= 0 || t >= dur) continue;
                const uint64_t n = hz_at(i, t);
                if (n != h) s.ev.push_back({t, EV_RETUNE, i, n, 0});
                h = n;
            }
        }
    }
    // restarts: none, a few toggles, periodic toggles, periodic mode changes, or a random mix at any rate
    switch (rng() % 5) {
        case 0: break;
        case 1:
            for (int k = 0; k < 6; k++) s.ev.push_back({int64_t(U(rng) * double(dur)), EV_OFF, 0, 0, int64_t(2000 + 18000 * U(rng))});
            break;
        case 2: {
            const int64_t every = int64_t(300 + 9700 * U(rng) * U(rng)), off = int64_t(100 + double(every) * U(rng));
            for (int64_t t = every; t < dur; t += every + off) s.ev.push_back({t, EV_OFF, 0, 0, off});
            break;
        }
        case 3: {
            const int64_t every = int64_t(300 + 9700 * U(rng) * U(rng));
            for (int64_t t = every; t < dur; t += every + every / 3) s.ev.push_back({t, EV_COHERENT, 0, 0, std::max<int64_t>(1, every / 3)});
            break;
        }
        default:
            for (int64_t t = int64_t(200 + 8000 * U(rng)); t < dur; t += int64_t(200 + 8000 * U(rng)))
                s.ev.push_back({t, U(rng) < 0.5 ? EV_OFF : EV_COHERENT, 0, 0, int64_t(50 + 4000 * U(rng) * U(rng))});
            break;
    }
    sort_ev(s);
    return s;
}

std::vector<Scen> directed_scens() {
    return {sc_scan(0.3),       sc_scan(0.1),        sc_all(),           sc_swap(0.1, false), sc_swap(0.1, true),
            sc_swap(0.3, true), sc_toggle(0.1, 3000, 5), sc_toggle(0.3, 3000, 5), sc_toggle(0.4, 3000, 8), sc_toggle(0.1, 700, 5),
            sc_mode(0.1, 4000), sc_mode(0.3, 2000),  sc_shared_toggle(), sc_burst(),
            sc_copy1(),         sc_copy2(),          sc_newfirst1(),     sc_newfirst2(),      sc_ties1(),
            sc_ties2(),         sc_tuneronly()};
}

// L3 (a model check, not an invariant): a band whose tuner is retuned every 2.8 s (+-20 %) gets at least 40 %
// the looks of a steady band - its aborted capture is tried again once the tuner settled
std::string check_l3(const Scen& s, const SimOut& r) {
    if (s.name.rfind("swap", 0) != 0) return "";
    const double steady = double(r.looks[0] + r.looks[3] + r.looks[4]) / 3.0;
    for (int i : {1, 2})
        if (double(r.looks[size_t(i)]) < 0.4 * steady)
            return "a band retuned every 2.8 s got " + std::to_string(r.looks[size_t(i)]) + " looks, a steady one " +
                   std::to_string(int(steady)) + " (L3, " + s.name + ")";
    return "";
}

void run_sched(int rounds, uint64_t seed) {
    const std::vector<Scen> dir = directed_scens();
    const int ns = std::max(20, rounds / 200);   // random runs of 300-900 s each
    const auto run = [&](const char* name, bool mutant, const SimOpt& o, bool l3) {
        int db = 0, rb = 0;
        std::string first;
        for (const Scen& s : dir) {
            const SimOut r = sim(s, o);
            const std::string e = !r.err.empty() ? r.err + " [" + s.name + "]" : l3 ? check_l3(s, r) : "";
            if (!e.empty() && !db++) first = e;
        }
        std::mt19937_64 rng(seed);
        const int n = mutant ? ns / 4 : ns;
        for (int k = 0; k < n; k++) {
            const Scen s = random_scen(rng, k % 4 != 3);   // (a quarter without retunes: INV-H)
            const SimOut r = sim(s, o);
            if (!r.err.empty() && !rb++ && first.empty()) first = r.err;
        }
        report(name, mutant, db, int(dir.size()), rb, n, first);
    };
    SimOpt real;
    run("schedule (retunes, restarts; INV-A, C, G, H)", false, real, true);
    SimOpt m;
    m.pick = pick_fb5;
    run("mutant: fb5cf77's pick (first looks always)", true, m, false);
    m.pick = pick_no_token;
    run("mutant: no token", true, m, false);
    m.pick = pick_no_settle;
    run("mutant: no settling", true, m, false);
    m = SimOpt{};
    m.new_band_first = true;
    run("mutant: a new band first in the turn order", true, m, false);
    m = SimOpt{};
    m.copy_token = true;
    run("mutant: the token copied with the band", true, m, false);
    m = SimOpt{};
    m.reset_debt_on_enable = true;
    run("mutant: the debt reset on enable", true, m, false);
    m = SimOpt{};
    m.restart_clears = true;
    run("mutant: 97f8d78's restart (slots cleared)", true, m, false);
    m = SimOpt{};
    m.restart_tuner_only = true;
    run("mutant: a restart keeping the tuners' latest looks only", true, m, false);
    m = SimOpt{};
    m.pick = pick_cursor_ties;
    run("mutant: ties in cursor order (the restart as now)", true, m, false);
    m = SimOpt{};
    m.retry = false;
    run("mutant: 97f8d78's abort (no retry after the settle)", true, m, true);
}

// INV-B: steady bands, then one tuner retuned onto a free band: after its settle its first look
// comes before any other band's next look. INV-D: without retunes the cursor's round robin.
std::string check_single_retune(const Pick& pick, int M, int rounds, int x) {
    std::vector<TunerSlot> slots;
    std::vector<uint64_t> band_hz;
    std::vector<int> reps;
    std::vector<uint64_t> hz = spaced(M);
    int64_t now = 0, seq = 0;
    int rr = 0;
    bool moved = false;
    std::vector<int> order;
    for (int k = 0; k < rounds * M; k++) {
        R::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
        bool first = false;
        const int p = pick(slots, reps, rr, now, first);
        if (p < 0) return "no pick";
        order.push_back(p);
        R::note_look(slots, reps, p, first, seq++);
        now += 1000;
    }
    for (size_t k = 0; k < order.size(); k++)
        if (order[k] != int(k % size_t(M))) return "without retunes not the round robin (INV-D)";
    hz[size_t(x)] = 300000000ull;
    R::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
    now += 500;
    R::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
    bool first = false;
    const int p = pick(slots, reps, rr, now, first);
    if (p != x) return "a single retune's first look not before the others' next (INV-B)";
    return "";
}

// INV-E: a band that stays occupied continues its tracker
std::string check_regroup(const RegroupFn& rg, const std::vector<uint64_t>& hz0, const std::vector<uint64_t>& hz1) {
    const int M = int(hz0.size());
    const std::vector<TunerSlot> t0 = slots_of(hz0), t1 = slots_of(hz1);
    const std::vector<int> r0 = R::band_reps(t0), r1 = R::band_reps(t1);
    std::vector<uint64_t> old_band(size_t(M), 0);
    for (int i = 0; i < M; i++)
        if (r0[size_t(i)] == i) old_band[size_t(i)] = hz0[size_t(i)];
    const Regroup g = rg(old_band, t1, r1);
    std::set<int> used;
    for (int r = 0; r < M; r++) {
        if (r1[size_t(r)] != r) continue;
        int s_band = -1;
        for (int s = 0; s < M; s++)
            if (old_band[size_t(s)] && dist(old_band[size_t(s)], hz1[size_t(r)]) <= R::SAME_BAND_HZ) s_band = s;
        if (s_band >= 0 && (g.src[size_t(r)] != s_band || !g.same[size_t(r)])) return "a band that stays occupied lost its tracker (INV-E)";
        if (g.src[size_t(r)] >= 0 && !used.insert(g.src[size_t(r)]).second) return "a tracker given to two bands";
        if (s_band < 0 && g.src[size_t(r)] >= 0) {   // a new band: a tracker of no band occupied now
            const uint64_t ob = old_band[size_t(g.src[size_t(r)])];
            for (int q = 0; q < M; q++)
                if (r1[size_t(q)] == q && ob && dist(ob, hz1[size_t(q)]) <= R::SAME_BAND_HZ) return "a new band got another occupied band's tracker";
        }
    }
    return "";
}

void run_bands(int rounds, uint64_t seed) {
    // INV-B / INV-D: directed M 2..8, every tuner retuned once; random M, rounds and tuner
    for (int c = 0; c < 2; c++) {
        const Pick pick = c ? Pick(pick_no_first) : Pick(R::pick_band);
        int db = 0, dn = 0, rb = 0;
        std::string first;
        for (int M = 2; M <= 8; M++)
            for (int x = 0; x < M; x++, dn++) {
                const std::string e = check_single_retune(pick, M, 2, x);
                if (!e.empty() && !db++) first = e;
            }
        std::mt19937_64 rng(seed);
        for (int r = 0; r < rounds / 10; r++) {
            const int M = 2 + int(rng() % 7), rd = 2 + int(rng() % 4), x = int(rng() % uint64_t(M));
            const std::string e = check_single_retune(pick, M, rd, x);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(c ? "mutant: no first looks ahead (INV-B)" : "single retune / no retunes (INV-B, INV-D)", c > 0, db, dn, rb, rounds / 10, first);
    }
    // INV-E: directed - the lead leaves a shared band, a lower tuner joins one; random moves
    const uint64_t A = 100000000, B = 105000000, C = 110000000, D = 115000000;
    const std::vector<std::pair<std::vector<uint64_t>, std::vector<uint64_t>>> dir = {
        {{A, B, B, D}, {A, C, B, D}},   // tuner 1 (the lead of B) leaves: tuner 2 continues B
        {{A, B, B, D}, {B, B, B, D}},   // tuner 0 joins B (the lower one leads)
        {{A, B, C, D}, {A, C, B, D}},   // tuners 1 and 2 swap
    };
    const uint64_t pool[] = {100000000, 105000000, 105000400, 110000000, 115000000, 120000000};
    for (int c = 0; c < 2; c++) {
        const RegroupFn rg = c ? RegroupFn(regroup_stay) : RegroupFn(R::regroup);
        int db = 0, rb = 0;
        std::string first;
        for (const auto& d : dir) {
            const std::string e = check_regroup(rg, d.first, d.second);
            if (!e.empty() && !db++) first = e;
        }
        std::mt19937_64 rng(seed);
        for (int r = 0; r < rounds; r++) {
            const int M = 2 + int(rng() % 7);
            std::vector<uint64_t> hz0(static_cast<size_t>(M)), hz1(static_cast<size_t>(M));
            for (int i = 0; i < M; i++) hz0[size_t(i)] = pool[rng() % 6];
            for (int i = 0; i < M; i++) hz1[size_t(i)] = rng() % 3 == 0 ? pool[rng() % 6] : hz0[size_t(i)];
            const std::string e = check_regroup(rg, hz0, hz1);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(c ? "mutant: fb5cf77's trackers stay with the tuner" : "regroup (INV-E)", c > 0, db, int(dir.size()), rb, rounds, first);
    }
}

// --- the published track ids (INV-I) --------------------------------------------------
// A model of the service's trackers: each tracker instance numbers its own tracks from 1 (a new one starts
// at 1 again; clear() keeps the count, as sigclass::Tracker), the trackers move with their bands
// (update_bands' src, as update_slots does), a restart clears them, a look adds and expires tracks.
enum IdMode { ID_REAL, ID_SLOT, ID_PER_TRACKER, ID_BOOK_BY_SLOT };
struct MTrk {
    int64_t uid = 0;
    int next_id = 1;
    std::vector<int> live;
    std::vector<R::TrackPubId> book;
    int64_t own_next = 0;   // (ID_PER_TRACKER: the tracker's own counter)
};
struct IdModel {
    IdMode mode;
    int M;
    std::vector<uint64_t> hz;
    std::vector<TunerSlot> slots;
    std::vector<uint64_t> band_hz;
    std::vector<int> reps;
    std::vector<MTrk> tt;
    std::vector<std::vector<R::TrackPubId>> slot_book;   // (ID_BOOK_BY_SLOT)
    int64_t uids = 0, next_pub = 0, now = 0, seq = 0;
    std::map<int64_t, std::pair<int64_t, int>> owner;   // published id -> (tracker, track): (b)
    std::map<std::pair<int64_t, int>, int64_t> given;   // (tracker, track) -> published id: (c)
    IdModel(IdMode m, std::vector<uint64_t> f) : mode(m), M(int(f.size())), hz(std::move(f)), tt(size_t(M)), slot_book(size_t(M)) {
        for (MTrk& x : tt) x.uid = ++uids;
        regroup_now();
    }
    void regroup_now() {
        bool moved = false;
        const Regroup g = R::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
        if (!moved) return;
        std::vector<MTrk> old = tt;
        for (int i = 0; i < M; i++) {
            const int src = g.src[size_t(i)];
            if (src >= 0) tt[size_t(i)] = old[size_t(src)];
            else {
                tt[size_t(i)] = MTrk{};
                tt[size_t(i)].uid = ++uids;
            }
        }
    }
    void restart() {   // (switch-on, mode change: the trackers cleared - Tracker::clear keeps its count)
        for (MTrk& x : tt) x.live.clear(), x.book.clear();
        for (auto& b : slot_book) b.clear();
        R::restart_bands(slots, now);
        regroup_now();
    }
    void look(int r, int add, int drop, std::mt19937_64* rng) {   // a look at band r: some tracks go, some come
        MTrk& x = tt[size_t(r)];
        for (int k = 0; k < drop && !x.live.empty(); k++) x.live.erase(x.live.begin() + long(rng ? (*rng)() % x.live.size() : 0));
        for (int k = 0; k < add; k++) x.live.push_back(x.next_id++);
        if (mode == ID_PER_TRACKER) R::follow_ids(x.book, x.live, x.own_next);
        else if (mode == ID_BOOK_BY_SLOT) R::follow_ids(slot_book[size_t(r)], x.live, next_pub);
        else R::follow_ids(x.book, x.live, next_pub);
        seq++;
    }
    // what the pages get now; "" or the violated part of INV-I
    std::string publish() {
        std::set<int64_t> ids;
        for (int r = 0; r < M; r++) {
            if (reps[size_t(r)] != r) continue;
            const MTrk& x = tt[size_t(r)];
            for (int id : x.live) {
                int64_t pub = -1;
                if (mode == ID_SLOT) pub = int64_t(id) * 8 + r;
                else pub = R::pub_id(mode == ID_BOOK_BY_SLOT ? slot_book[size_t(r)] : x.book, id);
                if (pub < 0) continue;   // (not published: the service skips a track its book has not seen)
                if (pub >= (int64_t(1) << 53)) return "an id beyond 2^53";
                if (!ids.insert(pub).second) return "two tracks with one id at one time (INV-I a)";
                const auto key = std::make_pair(x.uid, id);
                const auto o = owner.find(pub);
                if (o != owner.end() && o->second != key) return "an id published for two tracks (INV-I b)";
                owner[pub] = key;
                const auto gv = given.find(key);
                if (gv != given.end() && gv->second != pub) return "a track's id changed (INV-I c)";
                given[key] = pub;
            }
        }
        return "";
    }
};

// directed: a band's lead leaves and comes back, two tuners swap, a restart, a tuner on a new band
std::string ids_directed(IdMode mode) {
    const uint64_t A = 100000000, B = 105000000, C = 110000000, D = 115000000, E = 120000000;
    IdModel m(mode, {A, B, B, C});
    std::string e;
    const auto step = [&]() {
        if (e.empty()) e = m.publish();
    };
    const auto look_all = [&](int add) {
        for (int r = 0; r < m.M; r++)
            if (m.reps[size_t(r)] == r) m.look(r, add, 0, nullptr);
        step();
    };
    look_all(2);
    m.hz[1] = D;   // the lead of B leaves: tuner 2 continues B (its tracker moves)
    m.regroup_now();
    step();
    look_all(1);
    m.hz[1] = B;   // and comes back (leads B again)
    m.regroup_now();
    step();
    look_all(1);
    std::swap(m.hz[1], m.hz[3]);   // tuners 1 and 3 swap their bands
    m.regroup_now();
    step();
    look_all(1);
    m.hz[0] = E;   // tuner 0 to a band nobody was on (keeps its tracker), tuner 3 onto A: a new tracker there
    m.hz[3] = A;
    m.regroup_now();
    step();
    look_all(2);
    m.restart();
    look_all(2);
    return e;
}

std::string ids_random(IdMode mode, std::mt19937_64& rng) {
    const uint64_t pool[] = {100000000, 105000000, 105000400, 110000000, 115000000, 120000000, 0};
    const int M = 2 + int(rng() % 7);
    std::vector<uint64_t> f(static_cast<size_t>(M));
    for (auto& h : f) h = pool[rng() % 6];
    IdModel m(mode, f);
    for (int step = 0; step < 200; step++) {
        switch (rng() % 8) {
            case 0: case 1: m.hz[rng() % uint64_t(M)] = pool[rng() % 7]; break;               // a retune (0: no frequency)
            case 2: std::swap(m.hz[rng() % uint64_t(M)], m.hz[rng() % uint64_t(M)]); break;  // a swap
            case 3: if (rng() % 8 == 0) m.restart(); break;
            default: break;
        }
        m.now += 100;
        m.regroup_now();
        std::vector<int> r;
        for (int i = 0; i < M; i++)
            if (m.reps[size_t(i)] == i) r.push_back(i);
        if (!r.empty()) m.look(r[rng() % r.size()], int(rng() % 4), int(rng() % 3), &rng);
        const std::string e = m.publish();
        if (!e.empty()) return e;
    }
    return "";
}

void run_ids(int rounds, uint64_t seed) {
    const std::vector<std::pair<const char*, IdMode>> cases = {
        {"published track ids (INV-I)", ID_REAL},
        {"mutant: 97f8d78's id (tracker id x 8 + slot)", ID_SLOT},
        {"mutant: ids counted per tracker", ID_PER_TRACKER},
        {"mutant: the id book stays with the slot", ID_BOOK_BY_SLOT},
    };
    for (size_t c = 0; c < cases.size(); c++) {
        std::string first = ids_directed(cases[c].second);
        const int db = first.empty() ? 0 : 1;
        std::mt19937_64 rng(seed);
        int rb = 0;
        for (int r = 0; r < rounds / 100; r++) {
            const std::string e = ids_random(cases[c].second, rng);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(cases[c].first, c > 0, db, 1, rb, rounds / 100, first);
    }
}

// --- the budget ------------------------------------------------------------------------
struct BudgetLike {   // the interface of rules::Budget, for mutants
    std::function<double(double)> plan;
    std::function<bool(double, double, double)> may_skip;
    std::function<void(double, double)> started;
    std::function<double()> debt;
};
BudgetLike wrap(std::shared_ptr<R::Budget> b) {
    return {[b](double a) { return b->plan(a); }, [b](double e, double a, double c) { return b->may_skip(e, a, c); },
            [b](double e, double a) { b->started(e, a); }, [b]() { return b->debt_s; }};
}

// a run of 400 looks: analysis a per look (changing now and then), capture 0.55 s; first looks where `first(k)`
std::string check_budget(const std::function<BudgetLike()>& make, const std::function<double(int)>& ana,
                         const std::function<bool(int)>& first_at, bool with_first) {
    BudgetLike b = make();
    const double share = 0.04, mn = 1.0, mx = 10.0, cap_looks = 8.0;
    double a = ana(0);
    double t = 0.0, used = 0.0, excess = 0.0, max_cap = 0.0;
    for (int k = 0; k < 400; k++) {
        a = ana(k);
        const double start = t;
        used += a;
        excess += std::max(0.0, a - mx * share);
        const double busy = 0.55 + a;
        const double period = b.plan(a);
        if (period < mn - 1e-9 || period > mx + 1e-9) return "a period outside [min, max]";
        const double cap = cap_looks * a;
        max_cap = std::max(max_cap, cap);
        const bool first = with_first && first_at(k);
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

void run_budget(int rounds, uint64_t seed) {
    const auto real = []() { return wrap(std::make_shared<R::Budget>()); };
    // mutants: first looks without a debt; no payback; the cap ignored
    const auto no_debt = []() {
        auto b = std::make_shared<R::Budget>();
        BudgetLike w = wrap(b);
        w.started = [b](double e, double a) {
            if (e >= b->planned_s - 1e-3) b->started(e, a);
        };
        return w;
    };
    const auto no_payback = []() {
        auto b = std::make_shared<R::Budget>();
        BudgetLike w = wrap(b);
        w.plan = [b](double a) {
            b->planned_s = std::clamp(a / b->share, b->min_s, b->max_s);
            return b->planned_s;
        };
        return w;
    };
    const auto no_cap = []() {
        auto b = std::make_shared<R::Budget>();
        BudgetLike w = wrap(b);
        w.may_skip = [](double, double, double) { return true; };
        return w;
    };
    // directed: 0.1 s analysis, every look / every 3rd look a first look; 0.03 s, every look
    const std::vector<std::pair<double, int>> dir = {{0.1, 1}, {0.1, 3}, {0.03, 1}, {0.15, 2}};
    const std::vector<std::tuple<const char*, std::function<BudgetLike()>, bool, bool>> bc = {
        {"Budget (no first looks: the old rule)", real, false, false},
        {"Budget (first looks)", real, true, false},
        {"mutant: first looks without a debt", no_debt, true, true},
        {"mutant: no payback", no_payback, true, true},
        {"mutant: the debt cap ignored", no_cap, true, true},
    };
    for (const auto& [name, mk, wf, mut] : bc) {
        int db = 0, rb = 0;
        std::string first;
        for (const auto& d : dir) {
            const double a = d.first;
            const int every = d.second;
            const std::string e = check_budget(mk, [a](int) { return a; }, [every](int k) { return k % every == 0; }, wf);
            if (!e.empty() && !db++) first = e;
        }
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<double> U(0.0, 1.0);
        for (int r = 0; r < rounds / 20; r++) {
            std::vector<double> as(400);
            std::vector<uint8_t> fs(400);
            double a = 0.005 + 0.6 * U(rng) * U(rng);
            for (int k = 0; k < 400; k++) {
                if (U(rng) < 0.05) a = 0.005 + 0.6 * U(rng) * U(rng);   // (the load changes)
                as[size_t(k)] = a;
                fs[size_t(k)] = U(rng) < 0.3;
            }
            const std::string e = check_budget(mk, [&](int k) { return as[size_t(k)]; }, [&](int k) { return fs[size_t(k)] != 0; }, wf);
            if (!e.empty() && !rb++ && first.empty()) first = e;
        }
        report(name, mut, db, int(dir.size()), rb, rounds / 20, first);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 200000;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 12345;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("rounds %d, seed %llu\n", rounds, static_cast<unsigned long long>(seed));
    run_dedupe(rounds, seed);
    run_sticky(rounds, seed);
    run_reps(rounds, seed);
    run_bands(rounds, seed);
    run_sched(rounds, seed);
    run_ids(rounds, seed);
    run_budget(rounds, seed);
    std::printf("%s\n", g_fails ? "FAILED" : "all passed");
    return g_fails ? 1 : 0;
}
