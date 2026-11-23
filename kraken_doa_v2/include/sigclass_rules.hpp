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

}  // namespace sigsvc::rules
