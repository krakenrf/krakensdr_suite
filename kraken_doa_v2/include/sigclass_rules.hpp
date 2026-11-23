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
// snapshot (fresh), then one with a class, then the stronger (each tuner has
// its own antenna: live, one MPT1327 control channel read 15 dB SNR on CH0
// and 3.6 dB on CH1 - the newer CH1 view, p 0.40, hid CH0's sure label),
// then the more recently seen, then the more often seen (then the lower
// tuner, the lower frequency: a strict order)
template <class T>
bool better(const T& a, const T& b) {
    if (a.fresh != b.fresh) return a.fresh;
    if (a.cls.empty() != b.cls.empty()) return !a.cls.empty();
    if (a.snr != b.snr) return a.snr > b.snr;
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

// a tuner as the scheduler sees it. The band fields belong to the BAND the
// tuner stands for (tuners on one band carry the same values; regroup()
// says where they go when tuners move)
struct TunerSlot {
    uint64_t hz = 0;               // its centre (0 = none)
    int64_t settle_until_ms = 0;   // retuned: no look before this
    uint64_t looks = 0;            // looks at its band since the band's start (mode change, switch-on, retune)
    int64_t last_seq = -1;         // the look number of the band's latest look (-1: none; a new band: its tuner's own) - the tokens
    int64_t turn_seq = -1;         // the band's place in the turn: its latest look, a new band's start (the oldest goes first)
    int64_t prio_seq = INT64_MIN;  // the TUNER's own: the look number of its latest look ahead of the turn (as its
                                   // band's lead) - never copied between tuners: a tuner hopping onto another
                                   // tuner's band and off again got that band's token and overtook twice
    int64_t ready_ms = 0;          // when the band began waiting for its first look (first come, first served)
};

// tuners within this of each other are on one band: one look serves them all
constexpr uint64_t SAME_BAND_HZ = 1000;

inline uint64_t hz_dist(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

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
            if (rep[j] == int(j) && hz_dist(t[i].hz, t[j].hz) <= SAME_BAND_HZ) {
                rep[i] = int(j);
                break;
            }
    }
    return rep;
}

// After tuners moved: which old slot's tracker each representative takes
// (src; -1 a new, empty one) and whether it continues that slot's BAND (same
// - then also its band fields; else the band is new). old_band_hz[s]: the
// band slot s's tracker served (0: none - not a representative). A band that
// stays occupied keeps its tracker whichever tuner now stands for it (it holds
// absolute Hz: no gap in its labels when a lower tuner joins or the lead
// leaves); a tuner that moved to a band nobody was on keeps its own tracker -
// the old band's tracks stay out of sight and come back if it returns - unless
// another tuner continues that old band, then it starts empty. Invariants
// (tested, INV-E): a representative on a band some old slot served continues
// exactly that slot; no tracker goes to two; a representative not continuing
// a band gets a tracker of no band now occupied.
struct Regroup {
    std::vector<int> src;
    std::vector<uint8_t> same;
};
inline Regroup regroup(const std::vector<uint64_t>& old_band_hz, const std::vector<TunerSlot>& t, const std::vector<int>& rep) {
    const size_t M = t.size();
    Regroup g{std::vector<int>(M, -1), std::vector<uint8_t>(M, 0)};
    std::vector<uint8_t> taken(old_band_hz.size(), 0);
    for (size_t r = 0; r < M; r++) {
        if (rep[r] != int(r)) continue;
        for (size_t s = 0; s < old_band_hz.size(); s++)
            if (!taken[s] && old_band_hz[s] != 0 && hz_dist(old_band_hz[s], t[r].hz) <= SAME_BAND_HZ) {
                g.src[r] = int(s);
                g.same[r] = 1;
                taken[s] = 1;
                break;
            }
    }
    for (size_t r = 0; r < M; r++)
        if (rep[r] == int(r) && g.src[r] < 0 && r < old_band_hz.size() && !taken[r]) {
            g.src[r] = int(r);   // (its own: the old band's tracks out of sight)
            taken[r] = 1;
        }
    return g;
}

// The tuners' frequencies now (hz; 0 = none): a retuned tuner (not a first
// sight) settles settle_ms; the bands (reps), and - when anything changed
// (moved) - which tracker each band continues (the result's src, to apply to
// the trackers) and its band fields: a band that goes on keeps its looks, its
// last look; a new band starts at 0 looks (a first look ahead of its turn,
// waiting from its settle's end) with its turn counted from now (seq_now:
// behind every band looked before it); a tuner's token is its own (prio_seq
// is never copied); the tuners sharing a band carry its other fields.
inline Regroup update_bands(std::vector<TunerSlot>& slots, std::vector<uint64_t>& band_hz, std::vector<int>& reps,
                            const std::vector<uint64_t>& hz, int64_t now_ms, int64_t settle_ms, int64_t seq_now, bool& moved) {
    const size_t M = hz.size();
    moved = slots.size() != M || reps.size() != M;
    slots.resize(M);
    band_hz.resize(M, 0);
    const std::vector<TunerSlot> before = slots;
    for (size_t i = 0; i < M; i++) {
        TunerSlot& s = slots[i];
        if (hz[i] == s.hz) continue;
        if (s.hz != 0 && hz[i] != 0) s.settle_until_ms = now_ms + settle_ms;
        s.hz = hz[i];
        moved = true;
    }
    if (!moved) return Regroup{};
    const std::vector<int> nr = band_reps(slots);
    Regroup g = regroup(band_hz, slots, nr);
    for (size_t i = 0; i < M; i++) {
        if (nr[i] != int(i)) continue;
        TunerSlot& s = slots[i];
        if (g.same[i]) {
            const TunerSlot& o = before[size_t(g.src[i])];
            s.looks = o.looks;
            s.last_seq = o.last_seq;
            s.turn_seq = o.turn_seq;
            s.ready_ms = o.ready_ms;
        } else {
            s.looks = 0;
            s.ready_ms = std::max(now_ms, s.settle_until_ms);
            s.turn_seq = seq_now;   // (its turn counts from now: behind the bands looked before - INV-A; its
                                    // last look stays the tuner's own - the tokens: a never-looked band would block them)
        }
        band_hz[i] = s.hz;
    }
    for (size_t i = 0; i < M; i++) {
        const int r = nr[i];
        if (r == int(i)) continue;
        band_hz[i] = 0;
        if (r < 0) continue;
        slots[i].looks = slots[size_t(r)].looks;
        slots[i].last_seq = slots[size_t(r)].last_seq;
        slots[i].turn_seq = slots[size_t(r)].turn_seq;
        slots[i].ready_ms = slots[size_t(r)].ready_ms;
    }
    reps = nr;
    return g;
}

// The next band to look at. -1: none ready (no frequency, or settling after
// a retune). Two passes:
//  - a band that has had no look since its start (switch-on, mode change,
//    retune) is taken AHEAD of its turn (first_look = true; the cursor stays)
//    - first come first served (ready_ms) - but only while it holds its
//    token: a band that took a look ahead of its turn gets the next one only
//    after every other ready band has had a look since (prio_seq < their
//    last_seq). A tuner retuned again and again (a scanner, a user stepping a
//    pane) once took every other look: 120 s with one tuner retuned every
//    2.8 s gave looks [1, 42, 41, 8, 1]
//  - else the ready band whose latest look is the oldest (turn_seq; a
//    new band's counts from its start, ties in cursor order from rr; rr moves
//    past it). With nothing retuned this is the old round robin exactly.
// Invariants (tested, R = ready bands): INV-A every band ready and not
// retuned through 2R consecutive looks gets one in them; INV-B a single
// retune, nothing else changing, gets its first look before any other band's
// next one; INV-C a band waiting for its first look is overtaken at most once
// by each band that began waiting later; INV-D without retunes the order is
// the cursor's round robin; the pick is a representative with a frequency,
// not settling.
inline int pick_band(const std::vector<TunerSlot>& t, const std::vector<int>& rep, int& rr, int64_t now_ms, bool& first_look) {
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
    for (int k = 0; k < M; k++) {   // ahead of the turn
        const int i = (rr + k) % M;
        if (!ready(i) || t[size_t(i)].looks != 0 || !token(i)) continue;
        if (pick < 0 || t[size_t(i)].ready_ms < t[size_t(pick)].ready_ms) pick = i;
    }
    if (pick >= 0) {
        first_look = true;
        return pick;
    }
    for (int k = 0; k < M; k++) {   // the oldest look
        const int i = (rr + k) % M;
        if (!ready(i)) continue;
        if (pick < 0 || t[size_t(i)].turn_seq < t[size_t(pick)].turn_seq) pick = i;
    }
    if (pick >= 0) rr = (pick + 1) % M;
    return pick;
}

// a look at band `ch` (number seq) happened: its band fields on every tuner on it
inline void note_look(std::vector<TunerSlot>& t, const std::vector<int>& rep, int ch, bool first_look, int64_t seq) {
    for (size_t i = 0; i < t.size(); i++) {
        if (rep[i] != ch) continue;
        t[i].looks++;
        t[i].last_seq = seq;
        t[i].turn_seq = seq;
        if (first_look && int(i) == ch) t[i].prio_seq = seq;   // (the lead's own)
    }
}

// the longest a track nobody sees keeps its label (INV-F): wall time since it
// was last seen, whatever its band's looks
constexpr double TRACK_CEILING_S = 180.0;


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
