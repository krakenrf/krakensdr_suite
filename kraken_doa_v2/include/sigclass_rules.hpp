#pragma once

// Signal ID (docs/dev/signal-classifier.md, Runtime): the service's rules for
// which tracks the pages get when several tuners look at overlapping bands,
// as pure functions - sigclass_service.cpp uses them, and
// tools/sigclass_rules_test.cpp checks their invariants on random inputs.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace sigsvc::rules {

// Two tracks of different tuners are one signal (wideband scan's overlapping
// edges, independent tuners on the same band): centres closer than half the
// wider one's width + 500 Hz, widths within 3x.
template <class T>
bool same_signal(const T& a, const T& b) {
    if (a.tuner == b.tuner) return false;
    const double d = a.hz > b.hz ? a.hz - b.hz : b.hz - a.hz;
    if (!(d < 0.5 * std::max(a.bw, b.bw) + 500.0)) return false;
    const double ra = std::max(a.bw, 1.0) / std::max(b.bw, 1.0);
    return ra <= 3.0 && ra >= 1.0 / 3.0;
}

// the better of two views of one signal first: the one in its tuner's latest
// snapshot (fresh), then the more recently seen, then the more often seen
// (then the lower tuner, the lower frequency: a strict order)
template <class T>
bool better(const T& a, const T& b) {
    if (a.fresh != b.fresh) return a.fresh;
    if (a.seen_real != b.seen_real) return a.seen_real > b.seen_real;
    if (a.hits != b.hits) return a.hits > b.hits;
    if (a.tuner != b.tuner) return a.tuner < b.tuner;
    return a.hz < b.hz;
}

// One signal seen by two (or more) tuners is sent once: the tracks are taken
// best first, each unless a same_signal() one was taken already. Result
// sorted by hz. Invariants (tested): a subset of the input, every kept track
// unchanged (a self-move once emptied the strings of every track in front of
// the first dropped one); no two kept tracks are same_signal(); every
// dropped one has a kept same_signal() one of another tuner that is better();
// one tuner's tracks never drop each other; sorted by hz.
template <class T>
void drop_overlap_duplicates(std::vector<T>& pub) {
    const auto by_hz = [](const T& a, const T& b) { return a.hz < b.hz; };
    if (pub.size() < 2) return;
    bool multi = false;
    double max_bw = 0.0;
    for (const T& k : pub) {
        multi = multi || k.tuner != pub[0].tuner;
        max_bw = std::max(max_bw, k.bw);
    }
    if (!multi) {
        std::sort(pub.begin(), pub.end(), by_hz);
        return;
    }
    std::vector<size_t> order(pub.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return better(pub[a], pub[b]); });
    const double reach = 0.5 * max_bw + 500.0;   // (no partner is farther)
    std::vector<std::pair<double, size_t>> taken;  // (hz, index), sorted by hz
    std::vector<uint8_t> keep(pub.size(), 0);
    for (size_t i : order) {
        const auto lo = std::lower_bound(taken.begin(), taken.end(), std::make_pair(pub[i].hz - reach, size_t(0)));
        bool dup = false;
        for (auto it = lo; it != taken.end() && it->first <= pub[i].hz + reach && !dup; ++it)
            dup = same_signal(pub[it->second], pub[i]);
        if (dup) continue;
        keep[i] = 1;
        taken.insert(std::upper_bound(taken.begin(), taken.end(), std::make_pair(pub[i].hz, i)), std::make_pair(pub[i].hz, i));
    }
    size_t w = 0;
    for (size_t i = 0; i < pub.size(); i++) {
        if (!keep[i]) continue;
        if (w != i) pub[w] = std::move(pub[i]);   // (never a self-move)
        w++;
    }
    pub.resize(w);
    std::sort(pub.begin(), pub.end(), by_hz);
}

// --- tuners in turn (wideband scan, independent mode, generic RTL-SDRs) -------

// a tuner as the scheduler sees it
struct TunerSlot {
    uint64_t hz = 0;               // its centre (0 = none)
    int64_t settle_until_ms = 0;   // retuned: no look before this
    uint64_t looks = 0;            // looks at its band since the band's start (mode change, switch-on, retune)
};

// tuners within this of each other are on one band: one look serves them all
constexpr uint64_t SAME_BAND_HZ = 1000;

// per tuner the tuner that stands for its band (the lowest-numbered one on
// it; -1: no frequency). Invariants (tested): rep[rep[i]] == rep[i];
// rep[i] <= i; |hz[i] - hz[rep[i]]| <= SAME_BAND_HZ; two representatives
// are never on one band.
inline std::vector<int> band_reps(const std::vector<TunerSlot>& t) {
    std::vector<int> rep(t.size(), -1);
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].hz == 0) continue;
        rep[i] = int(i);
        for (size_t j = 0; j < i; j++)
            if (rep[j] == int(j) && (t[i].hz > t[j].hz ? t[i].hz - t[j].hz : t[j].hz - t[i].hz) <= SAME_BAND_HZ) {
                rep[i] = int(j);
                break;
            }
    }
    return rep;
}

// The next band to look at, from the round-robin cursor rr (advanced past
// it): a band nobody has looked at since its start first (first_look = true),
// else the next one in turn. -1: none ready (no frequency, or settling after
// a retune). Invariants (tested): the pick is a representative with a
// frequency, not settling; when a ready band has no look yet the pick is one
// of those; with nothing changing, between two looks at a band every other
// ready band gets exactly one.
inline int pick_band(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now_ms, bool& first_look) {
    const int M = int(t.size());
    first_look = false;
    if (M == 0) return -1;
    rr = ((rr % M) + M) % M;
    int pick = -1;
    for (int pass = 0; pass < 2 && pick < 0; pass++) {
        for (int k = 0; k < M; k++) {
            const int i = (rr + k) % M;
            if (rep[size_t(i)] != i || now_ms < t[size_t(i)].settle_until_ms) continue;
            if (pass == 0 && t[size_t(i)].looks != 0) continue;
            pick = i;
            first_look = pass == 0;
            break;
        }
    }
    if (pick >= 0) rr = (pick + 1) % M;
    return pick;
}

// The CPU budget: analysis takes at most `share` of the wall time - after a
// look the next starts `plan()` seconds after this one's start (the analysis
// time / share, clamped to [min_s, max_s], as before). A band's FIRST look
// may skip that rest (its labels show a round earlier); the analysis it did
// not wait for is a debt, at most `debt_cap_s`, paid back by the following
// periods (each at most doubled). Invariants (tested): with no first looks
// the periods are exactly the old rule's; the debt stays within [0, the
// largest cap so far] (the cap follows the analysis time); the analysis over
// any stretch is within share x the time + the cap (+ the max_s clamp's
// excess, as before); normal looks pay the debt back (where the max_s clamp
// leaves room: analysis <= max_s x share / 2 - beyond, it only blocks first
// looks).
struct Budget {
    double share = 0.04, min_s = 1.0, max_s = 10.0;
    double debt_s = 0.0;
    double planned_s = 0.0;   // the period planned after the latest look
    double plan(double ana_s) {
        const double r = std::min(debt_s, ana_s);
        planned_s = std::clamp((ana_s + r) / share, min_s, max_s);
        return planned_s;
    }
    // may the next look start already, elapsed_s after the latest one's start?
    bool may_skip(double elapsed_s, double ana_s, double debt_cap_s) const {
        return debt_s + std::max(0.0, ana_s - elapsed_s * share) <= debt_cap_s;
    }
    // the next look starts elapsed_s after the latest one's start
    void started(double elapsed_s, double ana_s) {
        const double covered = elapsed_s * share;
        if (elapsed_s >= planned_s - 1e-3) debt_s = std::max(0.0, debt_s - std::max(0.0, covered - ana_s));   // waited: paid
        else debt_s = std::max(0.0, debt_s + ana_s - covered);                                               // early: owed
    }
};

}  // namespace sigsvc::rules
