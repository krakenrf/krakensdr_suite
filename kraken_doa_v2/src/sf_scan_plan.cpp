#include "sf_scan_plan.hpp"

#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <numeric>

namespace sfscan {
namespace {

// Entries without a bandwidth or demod of their own keep the VFO's: fitted
// as at most this wide (a VFO left at 2.4 MHz by a wideband decoder would
// otherwise give each of them a band of its own)
constexpr int64_t KEEP_FIT_MAX_HZ = 240000;

// A tuner's centre is DC: the receiver takes the DC off its samples (an
// RTL-SDR has its spike there), so a carrier right on the centre is lost - a
// band of one entry centred on it never saw it. Channels keep at least this
// much more than their half width away from the centre where they can
constexpr int64_t DC_GUARD_HZ = 3000;

// Planning rounds that move an entry across a tuner seam into a band of its
// own (spread layout); the last one keeps it where it is
constexpr int SEAM_ROUNDS = 4;

int64_t fit_width_hz(const savedfreq::ScanEntry& e, int64_t vfo_hz, const Layout& lay) {
    int64_t w = (e.bw > 0 || !e.mode.empty()) ? vfo_width_hz(e, vfo_hz) : std::min(vfo_hz, KEEP_FIT_MAX_HZ);
    return std::min<int64_t>(w, static_cast<int64_t>(2 * lay.half));
}

// how far entry i is from the centre of the tuner it is received on
double off_tuner(const savedfreq::ScanEntry& e, const Layout& lay, int64_t c) {
    return std::fabs(static_cast<double>(e.hz - (c + lay.offs[nearest_tuner(lay, c, e.hz)])));
}

// How far the nearest channel's edge (+ the guard) is from its tuner's DC
// with the band on c: >= 0 = every channel clear of it
int64_t dc_clearance(const std::vector<savedfreq::ScanEntry>& es, const std::vector<int>& idx, int64_t c,
                     const Layout& lay) {
    int64_t m = INT64_MAX;
    for (int i : idx)
        m = std::min<int64_t>(m, std::llround(off_tuner(es[i], lay, c)) - channel_width_hz(es[i]) / 2 - DC_GUARD_HZ);
    return m;
}

// How far the VFO edge nearest a usable edge of its tuner is inside it with
// the band on c: >= 0 = every VFO inside its tuner's usable part (one tuner:
// always, for the centres pick_centre tries)
double seam_slack(const std::vector<savedfreq::ScanEntry>& es, const std::vector<int>& idx, int64_t c,
                  const Layout& lay, int64_t vfo_hz) {
    double m = 1e300;
    for (int i : idx) m = std::min(m, lay.half - (off_tuner(es[i], lay, c) + fit_width_hz(es[i], vfo_hz, lay) / 2.0));
    return m;
}

// The centre (whole kHz) for VFOs spanning [lo, hi]: every tuner inside the
// tunable range, the band's tuners covering [lo, hi], every VFO inside its
// tuner's usable part, every channel clear of DC - the nearest to the middle
// that is; else (channels every few kHz) the one farthest from them; else
// (no centre keeps every VFO off a seam) the one with the least overlap
int64_t pick_centre(const std::vector<savedfreq::ScanEntry>& es, const std::vector<int>& idx, double lo, double hi,
                    const Layout& lay, uint64_t min_hz, uint64_t max_hz, int64_t vfo_hz) {
    const int64_t o0 = lay.offs.front(), o1 = lay.offs.back();
    const int64_t rmin = static_cast<int64_t>(min_hz) - o0, rmax = static_cast<int64_t>(max_hz) - o1;
    const int64_t mid = std::clamp<int64_t>(std::llround(((lo + hi) / 2 - (o0 + o1) / 2.0) / 1000.0) * 1000, rmin, rmax);
    const int64_t cmin = std::max<int64_t>(static_cast<int64_t>(std::ceil((hi - (o1 + lay.half)) / 1000.0)) * 1000, rmin);
    const int64_t cmax = std::min<int64_t>(static_cast<int64_t>(std::floor((lo - (o0 - lay.half)) / 1000.0)) * 1000, rmax);
    if (cmin > cmax) return mid;
    const int64_t m0 = std::clamp(mid, cmin, cmax);
    // feasible (every VFO inside its tuner) first, then the DC clearance;
    // among infeasible ones the most seam slack
    auto better = [](double s, int64_t d, double bs, int64_t bd) {
        const bool f = s >= -1.0, bf = bs >= -1.0;
        if (f != bf) return f;
        return f ? d > bd : s > bs;
    };
    int64_t best = m0, best_dc = dc_clearance(es, idx, m0, lay);
    double best_seam = seam_slack(es, idx, m0, lay, vfo_hz);
    auto found = [&] { return best_seam >= -1.0 && best_dc >= 0; };
    for (int64_t d = 1000; !found() && (m0 - d >= cmin || m0 + d <= cmax); d += 1000) {
        for (const int64_t c : {m0 + d, m0 - d}) {
            if (c < cmin || c > cmax) continue;
            const int64_t dc = dc_clearance(es, idx, c, lay);
            const double s = seam_slack(es, idx, c, lay, vfo_hz);
            if (better(s, dc, best_seam, best_dc)) {
                best = c;
                best_dc = dc;
                best_seam = s;
                if (found()) break;
            }
        }
    }
    return best;
}

struct Done {
    int64_t centre;
    bool has_gain;
    double gain;
    std::vector<int> idx;
};

// The bands for the entries sel (indices into ok): packed, balanced and
// centred. strict (a spread layout): an entry whose VFO would lie across a
// tuner seam at the band's centre is left over (*left - planned again, it
// gets a band of its own); one past the tunable range's end is skipped
void plan_bands(const std::vector<savedfreq::ScanEntry>& ok, const std::vector<int>& sel, const Layout& lay,
                uint64_t min_hz, uint64_t max_hz, int64_t vfo_hz, bool strict, std::vector<Done>* done,
                std::vector<int>* left, std::vector<Skipped>* skipped) {
    const double width = static_cast<double>(lay.offs.back() - lay.offs.front()) + 2 * lay.half;
    // bands as [lo, hi] (every VFO inside), grown while they fit `pack`
    struct B {
        double lo, hi;
        bool has_gain;
        double gain;
        std::vector<int> idx;
    };
    auto group = [&](double pack) {
        std::vector<B> bs;
        auto try_add = [&](B& b, int i) {
            const double h = fit_width_hz(ok[i], vfo_hz, lay) / 2.0;
            const double lo = std::min(b.lo, ok[i].hz - h), hi = std::max(b.hi, ok[i].hz + h);
            if (hi - lo > pack) return false;
            b.lo = lo;
            b.hi = hi;
            b.idx.push_back(i);
            return true;
        };
        auto open = [&](int i) {
            const double h = fit_width_hz(ok[i], vfo_hz, lay) / 2.0;
            bs.push_back({ok[i].hz - h, ok[i].hz + h, ok[i].has_gain, ok[i].gain, {i}});
        };
        // entries that set a gain: per gain, lowest first, a new band when the next doesn't fit
        std::map<double, int> last_of_gain;   // gain -> its newest band
        for (int i : sel) {
            if (!ok[i].has_gain) continue;
            auto it = last_of_gain.find(ok[i].gain);
            if (it == last_of_gain.end() || !try_add(bs[it->second], i)) {
                open(i);
                last_of_gain[ok[i].gain] = static_cast<int>(bs.size()) - 1;
            }
        }
        // the others: into the first band they fit (any gain), else a band of their own
        for (int i : sel) {
            if (ok[i].has_gain) continue;
            bool placed = false;
            for (auto& b : bs) {
                if (try_add(b, i)) { placed = true; break; }
            }
            if (!placed) open(i);
        }
        return bs;
    };
    // As few bands as the width allows - but balanced: packed to the full
    // width the first band took all it could, so its outer entries sat at the
    // very edges of the tuner's band (where it rolls off) and a middle one next
    // to DC with no room to move the centre (6 broadcast stations 0.8 MHz
    // apart: 95.8 at -840 kHz, 96.6 40 kHz from DC). The narrowest packing that
    // still needs no more bands spreads them evenly (there: pairs, +-400 kHz)
    std::vector<B> bs = group(width);
    if (bs.size() > 1) {
        double lo = 0, hi = width;
        for (int it = 0; it < 30 && hi - lo > 1000; it++) {
            const double mid = (lo + hi) / 2;
            if (group(mid).size() <= bs.size()) hi = mid;
            else lo = mid;
        }
        if (std::vector<B> b2 = group(hi); b2.size() == bs.size()) bs = std::move(b2);
    }

    // centres on whole kHz inside the tunable range, clear of the channels
    // (pick_centre); an entry the band can't hold after that (next to the
    // range's ends) is left out
    for (auto& b : bs) {
        const int64_t c = pick_centre(ok, b.idx, b.lo, b.hi, lay, min_hz, max_hz, vfo_hz);
        Done d{c, b.has_gain, b.gain, {}};
        for (int i : b.idx) {
            const double off = off_tuner(ok[i], lay, c);
            if (off > lay.half) {
                skipped->push_back({ok[i].id, ok[i].name, "too close to the end of the receiver's range"});
                continue;
            }
            if (strict && off + fit_width_hz(ok[i], vfo_hz, lay) / 2.0 > lay.half + 1.0) {
                left->push_back(i);   // across a tuner seam here
                continue;
            }
            d.idx.push_back(i);
        }
        if (!d.idx.empty()) done->push_back(std::move(d));
    }
}

}  // namespace

Layout single_layout(double span_hz) {
    Layout l;
    l.offs = {0};
    l.half = span_hz / 2;
    return l;
}

Layout spread_layout(int tuners, int64_t spacing_hz) {
    Layout l;
    const int n = std::max(tuners, 1);
    l.offs.clear();
    for (int k = 0; k < n; k++) l.offs.push_back(static_cast<int64_t>(k - n / 2) * spacing_hz);
    l.half = static_cast<double>(spacing_hz) / 2;
    return l;
}

int nearest_tuner(const Layout& lay, int64_t centre_hz, int64_t hz) {
    int best = 0;
    int64_t bd = INT64_MAX;
    for (int k = 0; k < static_cast<int>(lay.offs.size()); k++) {
        const int64_t d = std::llabs(hz - (centre_hz + lay.offs[k]));
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

int64_t vfo_width_hz(const savedfreq::ScanEntry& e, int64_t vfo_hz) {
    if (e.bw > 0) return clamp_bandwidth_hz(static_cast<double>(e.bw));   // any width, as the VFO takes it
    if (e.mode == "WBFM") return 240000;
    if (e.mode == "NBFM" || e.mode == "AM") return 16000;
    return vfo_hz;   // DIGITAL keeps the VFO's (widened for its decoder when it locks)
}

int64_t channel_width_hz(const savedfreq::ScanEntry& e) {
    if (e.bw > 0) return e.bw;
    return e.mode == "WBFM" ? 180000 : 12500;
}

Plan build_plan(std::vector<savedfreq::ScanEntry> entries, const Layout& lay, uint64_t min_hz, uint64_t max_hz,
                int64_t vfo_hz, const std::function<std::string(const std::string&)>& why_not) {
    Plan p;
    std::stable_sort(entries.begin(), entries.end(),
                     [](const savedfreq::ScanEntry& a, const savedfreq::ScanEntry& b) { return a.hz < b.hz; });
    std::vector<savedfreq::ScanEntry> ok;
    for (auto& e : entries) {
        if (e.hz < static_cast<int64_t>(min_hz) || e.hz > static_cast<int64_t>(max_hz)) {
            p.skipped.push_back({e.id, e.name, "outside the receiver's range (" + std::to_string(min_hz / 1000000) +
                                                   " - " + std::to_string(max_hz / 1000000) + " MHz)"});
            continue;
        }
        if (std::string why = why_not ? why_not(e.dec) : ""; !why.empty()) {
            p.skipped.push_back({e.id, e.name, why});
            continue;
        }
        ok.push_back(std::move(e));
    }

    // the bands; with a spread layout an entry that would sit across a tuner
    // seam is planned again (a band of its own) - SEAM_ROUNDS times, then kept
    std::vector<Done> done;
    std::vector<int> sel(ok.size());
    std::iota(sel.begin(), sel.end(), 0);
    for (int round = 0; !sel.empty(); round++) {
        std::vector<int> left;
        plan_bands(ok, sel, lay, min_hz, max_hz, vfo_hz, lay.offs.size() > 1 && round < SEAM_ROUNDS, &done, &left,
                   &p.skipped);
        sel = std::move(left);
    }
    std::stable_sort(done.begin(), done.end(), [](const Done& a, const Done& b) { return a.centre < b.centre; });

    std::vector<char> keep(ok.size(), 0);
    for (const auto& d : done)
        for (int i : d.idx) keep[i] = 1;
    std::vector<int> remap(ok.size(), -1);
    for (size_t i = 0; i < ok.size(); i++) {
        if (!keep[i]) continue;
        remap[i] = static_cast<int>(p.entries.size());
        p.entries.push_back(ok[i]);
    }
    p.band_of.assign(p.entries.size(), -1);
    for (auto& d : done) {
        Band band;
        band.centre_hz = d.centre;
        band.has_gain = d.has_gain;
        band.gain = d.gain;
        for (int i : d.idx) band.entries.push_back(remap[i]);
        std::sort(band.entries.begin(), band.entries.end());
        for (int i : band.entries) p.band_of[i] = static_cast<int>(p.bands.size());
        p.bands.push_back(std::move(band));
    }
    return p;
}

Plan build_plan(std::vector<savedfreq::ScanEntry> entries, double span_hz, uint64_t min_hz, uint64_t max_hz,
                int64_t vfo_hz, const std::function<std::string(const std::string&)>& why_not) {
    return build_plan(std::move(entries), single_layout(span_hz), min_hz, max_hz, vfo_hz, why_not);
}

bool band_fits(const Plan& p, int b, int64_t centre_hz, const Layout& lay, int64_t vfo_hz) {
    if (b < 0 || b >= static_cast<int>(p.bands.size())) return false;
    for (int i : p.bands[b].entries) {
        const auto& e = p.entries[i];
        if (off_tuner(e, lay, centre_hz) + fit_width_hz(e, vfo_hz, lay) / 2.0 > lay.half + 1.0) return false;
    }
    // ... and no channel on DC - unless no centre is clear (the plan's then)
    return std::llabs(centre_hz - p.bands[b].centre_hz) < 1000 ||
           dc_clearance(p.entries, p.bands[b].entries, centre_hz, lay) >= 0;
}

bool band_fits(const Plan& p, int b, int64_t centre_hz, double span_hz, int64_t vfo_hz) {
    return band_fits(p, b, centre_hz, single_layout(span_hz), vfo_hz);
}

float entry_level(const std::vector<float>& spec, float floor_db, double centre_hz, double rate_hz, int64_t hz,
                  int64_t channel_hz) {
    const int n = static_cast<int>(spec.size());
    if (n < 16 || !(rate_hz > 0)) return -999.0f;
    const double bin_hz = rate_hz / n;
    const double pos = (static_cast<double>(hz) - (centre_hz - rate_hz / 2)) / bin_hz;
    const double half = std::max(2.0, 0.4 * static_cast<double>(channel_hz) / bin_hz);
    int lo = static_cast<int>(std::floor(pos - half)), hi = static_cast<int>(std::ceil(pos + half));
    if (hi < 0 || lo >= n || pos < 0 || pos >= n) return -999.0f;
    lo = std::max(lo, 0);
    hi = std::min(hi, n - 1);
    float m = spec[lo];
    for (int i = lo + 1; i <= hi; i++) m = std::max(m, spec[i]);
    return m - floor_db;
}

}  // namespace sfscan
