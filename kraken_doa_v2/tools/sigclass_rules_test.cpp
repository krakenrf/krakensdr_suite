// Signal ID: randomized invariant tests of the service's rules
// (include/sigclass_rules.hpp; not part of kraken_doa). Every rule is also run
// in deliberately broken versions (mutants) that the checks must catch.
// Build and run (from kraken_doa_v2):
//   g++ -std=c++20 -O2 -Wall -Wextra -Iinclude tools/sigclass_rules_test.cpp -o /tmp/sigclass_rules_test
//   /tmp/sigclass_rules_test [ROUNDS] [SEED]   (default 200000: ~25 s; the schedule model runs ROUNDS / 100 random
//   runs of 300-900 s each - the rare mutants need that many)
// Exit 0: every invariant held on every input and every mutant was caught.
#include "sigclass_rules.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
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

// --- the scheduler: a model of the worker loop on the real rules ---------------
// (update_bands, pick_band, note_look, Budget as sigclass_service.cpp uses them:
// the rest before a look unless it is a first look the debt allows, a capture of
// 560 ms that a retune of its tuner aborts, the analysis, the plan)
using sigsvc::rules::TunerSlot;
using sigsvc::rules::Regroup;
using Pick = std::function<int(const std::vector<TunerSlot>&, const std::vector<int>&, int&, int64_t, bool&)>;
using RegroupFn = std::function<Regroup(const std::vector<uint64_t>&, const std::vector<TunerSlot>&, const std::vector<int>&)>;

uint64_t dist(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

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
    return sigsvc::rules::pick_band(u, rep, rr, now, first);
}
int pick_no_first(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now, bool& first) {
    std::vector<TunerSlot> u = t;
    for (auto& s : u) s.looks += 1;
    return sigsvc::rules::pick_band(u, rep, rr, now, first);
}
int pick_no_settle(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t, bool& first) {
    return sigsvc::rules::pick_band(t, rep, rr, INT64_MAX / 2, first);
}
// fb5cf77's trackers: each stays with its tuner (a band's tracks lost when its lead changes)
Regroup regroup_stay(const std::vector<uint64_t>& old_band_hz, const std::vector<TunerSlot>& t, const std::vector<int>& rep) {
    Regroup g{std::vector<int>(t.size(), -1), std::vector<uint8_t>(t.size(), 0)};
    for (size_t r = 0; r < t.size(); r++)
        if (rep[r] == int(r)) {
            g.src[r] = int(r);
            g.same[r] = r < old_band_hz.size() && old_band_hz[r] && dist(old_band_hz[r], t[r].hz) <= sigsvc::rules::SAME_BAND_HZ;
        }
    return g;
}

struct SimOpt {
    Pick pick = sigsvc::rules::pick_band;
    bool reset_debt_on_enable = false;   // the mutant of 9751af3..1e8871d
    bool toggles = false;                // Signal ID switched off / on now and then
    bool retunes = true;
    bool copy_token = false;
    bool new_band_first = false;         // mutant: a new band's turn from before every look (not from its start)
};

// one random run; "" or the first violated invariant
std::string sim(const SimOpt& o, std::mt19937_64& rng) {
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const int M = 2 + int(rng() % 7);
    const double ana_s = 0.02 + 0.38 * U(rng);
    const int64_t CAPTURE = 560, SETTLE = 500;
    const double dur_s = 300 + 600 * U(rng);
    // the tuners: own bands, some sharing one; 1-4 retuned periodically (0.3-20 s), hopping
    // among bands - now and then onto another tuner's
    std::vector<uint64_t> base(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) base[size_t(i)] = U(rng) < 0.2 && i ? base[size_t(i - 1)] : 100000000ull + uint64_t(i) * 5000000ull;
    std::vector<int64_t> period(static_cast<size_t>(M), 0), phase(size_t(M), 0);
    const int nret = o.retunes ? 1 + int(rng() % 4) : 0;
    for (int k = 0; k < nret; k++) {
        const int i = int(rng() % uint64_t(M));
        period[size_t(i)] = int64_t(300 + (20000 - 300) * U(rng) * U(rng));
        phase[size_t(i)] = int64_t(U(rng) * double(period[size_t(i)]));
    }
    const bool random_extra = o.retunes && U(rng) < 0.5;
    std::vector<int64_t> extra_at;   // random one-off retunes (tuner, time)
    std::vector<int> extra_tuner;
    std::vector<uint64_t> extra_hz;
    if (random_extra)
        for (int k = 0; k < 20; k++) {
            extra_at.push_back(int64_t(U(rng) * dur_s * 1000));
            extra_tuner.push_back(int(rng() % uint64_t(M)));
            extra_hz.push_back(U(rng) < 0.3 ? base[rng() % uint64_t(M)] : 200000000ull + (rng() % 9) * 3000000ull);
        }
    const auto hz_at = [&](int i, int64_t t) -> uint64_t {
        uint64_t h = base[size_t(i)];
        if (period[size_t(i)]) {
            const int64_t k = (t + phase[size_t(i)]) / period[size_t(i)];
            const int64_t m = k % 7;
            h = m == 6 ? base[size_t((i + 1) % M)] : base[size_t(i)] + uint64_t(m) * 300000ull;   // (m 6: onto the next tuner's band)
        }
        for (size_t e = 0; e < extra_at.size(); e++)
            if (extra_tuner[e] == i && t >= extra_at[e] && t < extra_at[e] + 30000) h = extra_hz[e];
        return h;
    };
    std::vector<int64_t> toggle_at;
    if (o.toggles)
        for (int k = 0; k < 6; k++) toggle_at.push_back(int64_t(U(rng) * dur_s * 1000));
    std::sort(toggle_at.begin(), toggle_at.end());

    std::vector<TunerSlot> slots;
    std::vector<uint64_t> band_hz;
    std::vector<int> reps;
    sigsvc::rules::Budget budget;
    budget.share = 0.04;
    budget.min_s = 1.0;
    budget.max_s = 10.0;
    double ewma = 0;
    int64_t now = 0, last_start = -1, seq = 0;
    int rr = 0;
    double used = 0, excess = 0, max_cap = 0;
    int epoch_restart = 0;
    // per look: who was ready (tuner, hz) and with what retune epoch; who was picked
    struct LookRec {
        int64_t t;
        int pick;
        int restart;
        std::vector<std::pair<int, uint64_t>> ready;
        std::vector<int> epoch;
    };
    std::vector<LookRec> L;
    std::vector<int> epoch(static_cast<size_t>(M), 0);
    std::vector<uint64_t> last_hz(static_cast<size_t>(M), 0);
    // INV-C: overtakes of a waiting band by later-waiting ones
    std::map<std::tuple<int, int, int>, int> over;   // (waiting band, its retune epoch, the later one) -> overtakes
    size_t ti = 0;
    while (now < int64_t(dur_s * 1000)) {
        if (ti < toggle_at.size() && now >= toggle_at[ti]) {   // off for 2-20 s, then on: the bands start again
            now += int64_t(2000 + 18000 * U(rng));
            ti++;
            slots.clear();
            reps.clear();
            band_hz.clear();
            epoch_restart++;
            if (o.reset_debt_on_enable) {
                budget.debt_s = 0;
                last_start = -1;
            }
            continue;
        }
        std::vector<uint64_t> hz(static_cast<size_t>(M));
        for (int i = 0; i < M; i++) {
            hz[size_t(i)] = hz_at(i, now);
            if (hz[size_t(i)] != last_hz[size_t(i)]) epoch[size_t(i)]++;
            last_hz[size_t(i)] = hz[size_t(i)];
        }
        bool moved = false;
        sigsvc::rules::update_bands(slots, band_hz, reps, hz, now, SETTLE, o.new_band_first ? -1 : seq, moved);
        if (o.copy_token)   // (mutant: the token goes with the band - a tuner hopping onto one and off again takes it along)
            for (int i = 0; i < M; i++)
                if (reps[size_t(i)] >= 0 && reps[size_t(i)] != i) slots[size_t(i)].prio_seq = std::max(slots[size_t(i)].prio_seq, slots[size_t(reps[size_t(i)])].prio_seq) == slots[size_t(i)].prio_seq ? slots[size_t(reps[size_t(i)])].prio_seq : slots[size_t(reps[size_t(i)])].prio_seq;
        int rr_next = rr;
        bool first = false;
        const int ch = o.pick(slots, reps, rr_next, now, first);
        if (ch >= 0 && (reps[size_t(ch)] != ch || slots[size_t(ch)].hz == 0 || now < slots[size_t(ch)].settle_until_ms))
            return "picked a tuner that is not a ready band";
        if (ch < 0) {
            now += 20;
            continue;
        }
        if (last_start >= 0) {
            const double elapsed = double(now - last_start) / 1000.0;
            const bool skip = first && budget.may_skip(elapsed, ewma, 8.0 * ewma);
            if (!skip && elapsed < budget.planned_s) {
                now += std::max<int64_t>(1, int64_t(std::ceil((budget.planned_s - elapsed) * 1000.0)));
                continue;
            }
        }
        const int64_t cycle0 = now;
        rr = rr_next;
        bool aborted = false;
        for (int64_t t = now; t <= now + CAPTURE; t += 7)
            if (hz_at(ch, t) != slots[size_t(ch)].hz) {
                now = t;
                aborted = true;
                break;
            }
        if (aborted) {
            slots[size_t(ch)].settle_until_ms = now + SETTLE;
            continue;
        }
        // a look: record who was ready at its decision
        LookRec rec{cycle0, ch, epoch_restart, {}, epoch};
        for (int i = 0; i < M; i++)
            if (reps[size_t(i)] == i && slots[size_t(i)].hz && cycle0 >= slots[size_t(i)].settle_until_ms) rec.ready.push_back({i, slots[size_t(i)].hz});
        // INV-C: the waiting bands this pick overtakes
        for (int w = 0; w < M; w++) {
            if (w == ch || reps[size_t(w)] != w || !slots[size_t(w)].hz || cycle0 < slots[size_t(w)].settle_until_ms || slots[size_t(w)].looks) continue;
            if (slots[size_t(ch)].looks == 0 && slots[size_t(ch)].ready_ms > slots[size_t(w)].ready_ms) {
                if (++over[std::make_tuple(w, epoch[size_t(w)], ch)] > 1) return "a waiting band overtaken twice by one that began waiting later";
            }
        }
        L.push_back(rec);
        now += CAPTURE;
        if (last_start >= 0) budget.started(double(cycle0 - last_start) / 1000.0, ewma);
        last_start = cycle0;
        now += int64_t(ana_s * 1000);
        used += ana_s;
        excess += std::max(0.0, ana_s - 10.0 * 0.04);
        ewma = ewma > 0 ? ewma + 0.3 * (ana_s - ewma) : ana_s;
        max_cap = std::max(max_cap, 8.0 * ewma);
        budget.plan(ewma);
        sigsvc::rules::note_look(slots, reps, ch, first, seq++);
        // a waiting band that got its look: its overtakes are over
        for (auto it = over.begin(); it != over.end();) it = std::get<0>(it->first) == ch ? over.erase(it) : std::next(it);
        // INV-G: the analysis within the share, the debt's cap, the clamp's excess and the latest look
        if (used > 0.04 * (double(now) / 1000.0) + max_cap + excess + ana_s + 1e-6) return "more analysis than the budget allows (INV-G)";
    }
    // INV-A: a band ready through 2R consecutive looks (R the tuners standing for a ready band in them) and not retuned gets one
    for (size_t k = 0; k < L.size(); k++) {
        size_t len = 2 * std::max<size_t>(1, L[k].ready.size());
        for (int it = 0; it < 8; it++) {
            size_t R = 1;
            std::set<int> who;   // (R: the tuners standing for a ready band at any look in the stretch - each its own token)
            for (size_t j = k; j < std::min(L.size(), k + len); j++)
                for (const auto& q : L[j].ready) who.insert(q.first);
            R = std::max<size_t>(1, who.size());
            if (2 * R <= len) break;
            len = 2 * R;
        }
        if (k + len > L.size()) break;
        for (const auto& b : L[k].ready) {
            bool through = true, got = false;
            for (size_t j = k; j < k + len && through; j++) {
                through = L[j].restart == L[k].restart && L[j].epoch[size_t(b.first)] == L[k].epoch[size_t(b.first)] &&
                          std::find(L[j].ready.begin(), L[j].ready.end(), b) != L[j].ready.end();
                got = got || L[j].pick == b.first;
            }
            if (through && !got) return "a ready band without a look in 2R looks (INV-A)";
        }
    }
    return "";
}

// INV-B: steady bands, then one tuner retuned onto a free band: after its settle its first look
// comes before any other band's next look. INV-D: without retunes the cursor's round robin.
std::string check_single_retune(const Pick& pick, std::mt19937_64& rng) {
    const int M = 2 + int(rng() % 7);
    std::vector<TunerSlot> slots;
    std::vector<uint64_t> band_hz;
    std::vector<int> reps;
    std::vector<uint64_t> hz(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) hz[size_t(i)] = 100000000ull + uint64_t(i) * 5000000ull;
    int64_t now = 0, seq = 0;
    int rr = 0;
    bool moved = false;
    const int rounds = 2 + int(rng() % 4);
    std::vector<int> order;
    for (int k = 0; k < rounds * M; k++) {
        sigsvc::rules::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
        bool first = false;
        const int p = pick(slots, reps, rr, now, first);
        if (p < 0) return "no pick";
        order.push_back(p);
        sigsvc::rules::note_look(slots, reps, p, first, seq++);
        now += 1000;
    }
    for (size_t k = 0; k < order.size(); k++)
        if (order[k] != int(k % size_t(M))) return "without retunes not the round robin (INV-D)";
    const int x = int(rng() % uint64_t(M));
    hz[size_t(x)] = 300000000ull;
    sigsvc::rules::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
    now += 500;
    // (one more of the others may come while it settles - then the first look)
    sigsvc::rules::update_bands(slots, band_hz, reps, hz, now, 500, seq, moved);
    bool first = false;
    const int p = pick(slots, reps, rr, now, first);
    if (p != x) return "a single retune's first look not before the others' next (INV-B)";
    return "";
}

// INV-E: random bands, random moves: a band that stays occupied continues its tracker
std::string check_regroup(const RegroupFn& rg, std::mt19937_64& rng) {
    const int M = 2 + int(rng() % 7);
    const uint64_t pool[] = {100000000, 105000000, 105000400, 110000000, 115000000, 120000000};
    std::vector<uint64_t> hz0(static_cast<size_t>(M)), hz1(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) hz0[size_t(i)] = pool[rng() % 6];
    for (int i = 0; i < M; i++) hz1[size_t(i)] = rng() % 3 == 0 ? pool[rng() % 6] : hz0[size_t(i)];
    std::vector<TunerSlot> t0(static_cast<size_t>(M)), t1(static_cast<size_t>(M));
    for (int i = 0; i < M; i++) t0[size_t(i)].hz = hz0[size_t(i)], t1[size_t(i)].hz = hz1[size_t(i)];
    const std::vector<int> r0 = sigsvc::rules::band_reps(t0), r1 = sigsvc::rules::band_reps(t1);
    std::vector<uint64_t> old_band(static_cast<size_t>(M), 0);
    for (int i = 0; i < M; i++)
        if (r0[size_t(i)] == i) old_band[size_t(i)] = hz0[size_t(i)];
    const Regroup g = rg(old_band, t1, r1);
    std::set<int> used;
    for (int r = 0; r < M; r++) {
        if (r1[size_t(r)] != r) continue;
        int s_band = -1;
        for (int s = 0; s < M; s++)
            if (old_band[size_t(s)] && dist(old_band[size_t(s)], hz1[size_t(r)]) <= sigsvc::rules::SAME_BAND_HZ) s_band = s;
        if (s_band >= 0 && (g.src[size_t(r)] != s_band || !g.same[size_t(r)])) return "a band that stays occupied lost its tracker (INV-E)";
        if (g.src[size_t(r)] >= 0 && !used.insert(g.src[size_t(r)]).second) return "a tracker given to two bands";
        if (s_band < 0 && g.src[size_t(r)] >= 0) {   // a new band: a tracker of no band occupied now
            const uint64_t ob = old_band[size_t(g.src[size_t(r)])];
            for (int q = 0; q < M; q++)
                if (r1[size_t(q)] == q && ob && dist(ob, hz1[size_t(q)]) <= sigsvc::rules::SAME_BAND_HZ)
                    return "a new band got another occupied band's tracker";
        }
    }
    return "";
}

int run_sched_tests2(int rounds, uint64_t seed) {
    int fails = 0;
    const auto report = [&](const char* name, bool mutant, int bad, int n, const std::string& first) {
        const bool ok = mutant ? bad > 0 : bad == 0;
        if (!ok) fails++;
        std::printf("%s %-44s %d / %d inputs violate an invariant%s%s\n", ok ? "PASS" : "FAIL", name, bad, n,
                    first.empty() ? "" : " - first: ", first.c_str());
    };
    const auto run = [&](const char* name, bool mutant, int n, const std::function<std::string(std::mt19937_64&)>& f) {
        std::mt19937_64 rng(seed);
        int bad = 0;
        std::string first;
        for (int r = 0; r < n; r++) {
            const std::string e = f(rng);
            if (!e.empty() && !bad++) first = e;
        }
        report(name, mutant, bad, n, first);
    };
    const int ns = std::max(20, rounds / 100);   // (each a run of 300-900 s)
    SimOpt real;
    run("schedule (retunes)", false, ns, [&](std::mt19937_64& g) { return sim(real, g); });
    SimOpt tog;
    tog.toggles = true;
    run("schedule (retunes, Signal ID toggled)", false, ns, [&](std::mt19937_64& g) { return sim(tog, g); });
    SimOpt m;
    m.pick = pick_fb5;
    run("mutant: fb5cf77's pick (first looks always)", true, ns, [&](std::mt19937_64& g) { return sim(m, g); });
    m.pick = pick_no_token;
    run("mutant: no token", true, ns, [&](std::mt19937_64& g) { return sim(m, g); });
    m.pick = sigsvc::rules::pick_band;
    m.new_band_first = true;
    run("mutant: a new band first in the turn order", true, ns, [&](std::mt19937_64& g) { return sim(m, g); });
    m.new_band_first = false;
    m.copy_token = true;
    run("mutant: the token copied with the band", true, 5 * ns, [&](std::mt19937_64& g) { return sim(m, g); });
    m.copy_token = false;
    m.new_band_first = false;
    m.pick = pick_no_settle;
    run("mutant: no settling", true, ns, [&](std::mt19937_64& g) { return sim(m, g); });
    SimOpt rd = tog;
    rd.reset_debt_on_enable = true;
    run("mutant: the debt reset on enable", true, ns, [&](std::mt19937_64& g) { return sim(rd, g); });
    run("single retune / no retunes (INV-B, INV-D)", false, rounds, [&](std::mt19937_64& g) { return check_single_retune(sigsvc::rules::pick_band, g); });
    run("mutant: no first looks ahead (INV-B)", true, rounds, [&](std::mt19937_64& g) { return check_single_retune(pick_no_first, g); });
    run("regroup (INV-E)", false, rounds, [&](std::mt19937_64& g) { return check_regroup(sigsvc::rules::regroup, g); });
    run("mutant: fb5cf77's trackers stay with the tuner", true, rounds, [&](std::mt19937_64& g) { return check_regroup(regroup_stay, g); });
    return fails;
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
        if (dist(t[i].hz, t[size_t(r)].hz) > sigsvc::rules::SAME_BAND_HZ) return "a tuner on another band than its representative";
        for (size_t j = 0; j < t.size(); j++)
            if (j != i && rep[i] == int(i) && rep[j] == int(j) && t[j].hz && dist(t[i].hz, t[j].hz) <= sigsvc::rules::SAME_BAND_HZ)
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
            if (t[j].hz && dist(t[i].hz, t[j].hz) <= sigsvc::rules::SAME_BAND_HZ) {
                rep[i] = int(j);
                break;
            }
    }
    return rep;
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
        std::printf("%s %-44s %d / %d inputs violate an invariant%s%s\n", ok ? "PASS" : "FAIL", name, bad, n,
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
    fails += run_sched_tests2(rounds, seed);
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
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 200000;
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
