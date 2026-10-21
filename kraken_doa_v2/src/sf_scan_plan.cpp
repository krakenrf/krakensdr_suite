#include "sf_scan_plan.hpp"

#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>

namespace sfscan {
namespace {

// Entries without a bandwidth or demod of their own keep the VFO's: fitted
// as at most this wide (a VFO left at 2.4 MHz by a wideband decoder would
// otherwise give each of them a band of its own)
constexpr int64_t KEEP_FIT_MAX_HZ = 240000;

// The tuner's centre is DC: the receiver takes the DC off its samples (an
// RTL-SDR has its spike there), so a carrier right on the centre is lost - a
// band of one entry centred on it never saw it. Channels keep at least this
// much more than their half width away from the centre where they can
constexpr int64_t DC_GUARD_HZ = 3000;

int64_t fit_width_hz(const savedfreq::ScanEntry& e, int64_t vfo_hz, double span_hz) {
    int64_t w = (e.bw > 0 || !e.mode.empty()) ? vfo_width_hz(e, vfo_hz) : std::min(vfo_hz, KEEP_FIT_MAX_HZ);
    return std::min<int64_t>(w, static_cast<int64_t>(span_hz));
}

// How far the nearest channel's edge (+ the guard) is from DC with the tuner
// on c: >= 0 = every channel clear of it
int64_t dc_clearance(const std::vector<savedfreq::ScanEntry>& es, const std::vector<int>& idx, int64_t c) {
    int64_t m = INT64_MAX;
    for (int i : idx)
        m = std::min<int64_t>(m, std::llabs(es[i].hz - c) - channel_width_hz(es[i]) / 2 - DC_GUARD_HZ);
    return m;
}

// The centre (whole kHz) for VFOs spanning [lo, hi]: inside the tunable range,
// every VFO inside the span, every channel clear of DC - the nearest to the
// middle that is; else (channels every few kHz) the one farthest from them
int64_t pick_centre(const std::vector<savedfreq::ScanEntry>& es, const std::vector<int>& idx, double lo, double hi,
                    double span_hz, uint64_t min_hz, uint64_t max_hz) {
    const int64_t mid = std::clamp<int64_t>(std::llround((lo + hi) / 2000.0) * 1000, static_cast<int64_t>(min_hz),
                                            static_cast<int64_t>(max_hz));
    const int64_t cmin = std::max<int64_t>(static_cast<int64_t>(std::ceil((hi - span_hz / 2) / 1000.0)) * 1000,
                                           static_cast<int64_t>(min_hz));
    const int64_t cmax = std::min<int64_t>(static_cast<int64_t>(std::floor((lo + span_hz / 2) / 1000.0)) * 1000,
                                           static_cast<int64_t>(max_hz));
    if (cmin > cmax) return mid;
    const int64_t m0 = std::clamp(mid, cmin, cmax);
    int64_t best = m0, best_cl = dc_clearance(es, idx, best);
    for (int64_t d = 1000; best_cl < 0 && (m0 - d >= cmin || m0 + d <= cmax); d += 1000) {
        for (const int64_t c : {m0 + d, m0 - d}) {
            if (c < cmin || c > cmax) continue;
            const int64_t cl = dc_clearance(es, idx, c);
            if (cl > best_cl) {
                best = c;
                best_cl = cl;
                if (cl >= 0) break;
            }
        }
    }
    return best;
}

}  // namespace

int bandwidth_index_for(int64_t hz) {
    int best = 0;
    for (int i = 1; i < NUM_BANDWIDTH_OPTIONS; i++) {
        if (std::fabs(BANDWIDTH_OPTIONS[i].bandwidth_mhz * 1e6 - static_cast<double>(hz)) <
            std::fabs(BANDWIDTH_OPTIONS[best].bandwidth_mhz * 1e6 - static_cast<double>(hz)))
            best = i;
    }
    return best;
}

int64_t vfo_width_hz(const savedfreq::ScanEntry& e, int64_t vfo_hz) {
    if (e.bw > 0) return std::llround(get_bandwidth_option(bandwidth_index_for(e.bw)).bandwidth_mhz * 1e6);
    if (e.mode == "WBFM") return 240000;
    if (e.mode == "NBFM" || e.mode == "AM") return 16000;
    return vfo_hz;   // DIGITAL keeps the VFO's (widened for its decoder when it locks)
}

int64_t channel_width_hz(const savedfreq::ScanEntry& e) {
    if (e.bw > 0) return e.bw;
    return e.mode == "WBFM" ? 180000 : 12500;
}

Plan build_plan(std::vector<savedfreq::ScanEntry> entries, double span_hz, uint64_t min_hz, uint64_t max_hz,
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
            const double h = fit_width_hz(ok[i], vfo_hz, span_hz) / 2.0;
            const double lo = std::min(b.lo, ok[i].hz - h), hi = std::max(b.hi, ok[i].hz + h);
            if (hi - lo > pack) return false;
            b.lo = lo;
            b.hi = hi;
            b.idx.push_back(i);
            return true;
        };
        auto open = [&](int i) {
            const double h = fit_width_hz(ok[i], vfo_hz, span_hz) / 2.0;
            bs.push_back({ok[i].hz - h, ok[i].hz + h, ok[i].has_gain, ok[i].gain, {i}});
        };
        // entries that set a gain: per gain, lowest first, a new band when the next doesn't fit
        std::map<double, int> last_of_gain;   // gain -> its newest band
        for (int i = 0; i < static_cast<int>(ok.size()); i++) {
            if (!ok[i].has_gain) continue;
            auto it = last_of_gain.find(ok[i].gain);
            if (it == last_of_gain.end() || !try_add(bs[it->second], i)) {
                open(i);
                last_of_gain[ok[i].gain] = static_cast<int>(bs.size()) - 1;
            }
        }
        // the others: into the first band they fit (any gain), else a band of their own
        for (int i = 0; i < static_cast<int>(ok.size()); i++) {
            if (ok[i].has_gain) continue;
            bool placed = false;
            for (auto& b : bs) {
                if (try_add(b, i)) { placed = true; break; }
            }
            if (!placed) open(i);
        }
        return bs;
    };
    // As few bands as the span allows - but balanced: packed to the full span
    // the first band took all it could, so its outer entries sat at the very
    // edges of the tuner's band (where it rolls off) and a middle one next to
    // DC with no room to move the centre (6 broadcast stations 0.8 MHz apart:
    // 95.8 at -840 kHz, 96.6 40 kHz from DC). The narrowest packing that still
    // needs no more bands spreads them evenly (there: pairs, +-400 kHz)
    std::vector<B> bs = group(span_hz);
    if (bs.size() > 1) {
        double lo = 0, hi = span_hz;
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
    struct Done {
        int64_t centre;
        bool has_gain;
        double gain;
        std::vector<int> idx;
    };
    std::vector<Done> done;
    std::vector<char> keep(ok.size(), 0);
    for (auto& b : bs) {
        const int64_t c = pick_centre(ok, b.idx, b.lo, b.hi, span_hz, min_hz, max_hz);
        Done d{c, b.has_gain, b.gain, {}};
        for (int i : b.idx) {
            if (std::fabs(static_cast<double>(ok[i].hz - c)) > span_hz / 2) {
                p.skipped.push_back({ok[i].id, ok[i].name, "too close to the end of the receiver's range"});
                continue;
            }
            d.idx.push_back(i);
            keep[i] = 1;
        }
        if (!d.idx.empty()) done.push_back(std::move(d));
    }
    std::stable_sort(done.begin(), done.end(), [](const Done& a, const Done& b) { return a.centre < b.centre; });

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

bool band_fits(const Plan& p, int b, int64_t centre_hz, double span_hz, int64_t vfo_hz) {
    if (b < 0 || b >= static_cast<int>(p.bands.size())) return false;
    for (int i : p.bands[b].entries) {
        const auto& e = p.entries[i];
        const double h = fit_width_hz(e, vfo_hz, span_hz) / 2.0;
        if (std::fabs(static_cast<double>(e.hz - centre_hz)) + h > span_hz / 2 + 1.0) return false;
    }
    // ... and no channel on DC - unless no centre is clear (the plan's then)
    return std::llabs(centre_hz - p.bands[b].centre_hz) < 1000 ||
           dc_clearance(p.entries, p.bands[b].entries, centre_hz) >= 0;
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
