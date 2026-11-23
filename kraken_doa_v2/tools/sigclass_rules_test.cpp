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
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

struct T {   // the fields of the service's PubTrack the rules read, + strings
    int id = 0, tuner = 0, hits = 0;
    double hz = 0, bw = 0, seen_real = 0;
    bool fresh = false;
    std::string cls, name;
    bool operator==(const T& o) const {
        return id == o.id && tuner == o.tuner && hits == o.hits && hz == o.hz && bw == o.bw && seen_real == o.seen_real &&
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
    for (T& k : pub) k.fresh = !k.fresh, k.seen_real = -k.seen_real, k.hits = -k.hits;
    sigsvc::rules::drop_overlap_duplicates(pub);
    for (T& k : pub) k.fresh = !k.fresh, k.seen_real = -k.seen_real, k.hits = -k.hits;
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
                x.cls = "c" + std::to_string(x.id % 7);
                x.name = "Class name number " + std::to_string(x.id);   // (longer than the SSO buffer)
                v.push_back(x);
            }
        }
    }
    std::shuffle(v.begin(), v.end(), rng);
    return v;
}

}  // namespace

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
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
