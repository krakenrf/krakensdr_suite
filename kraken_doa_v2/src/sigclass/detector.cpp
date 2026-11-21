// Signal classifier: the detector - every signal of a snapshot as a
// Detection (centre, occupied bandwidth, SNR, duty, burst flag).
//
// 1. The fine PSD (20 Hz bins, the front end's Hann-windowed long FFTs) with
//    the DC zone bridged, summed to base bins (8 fine bins, 160 Hz).
// 2. Noise floor, frequency dependent (RTL-SDR passbands droop): per cell of
//    30 base bins a low percentile; cells within 1 dB of the current floor
//    are noise, the floor is the median of nearby noise cells and is bridged
//    linearly across signal cells (so a 1.5 MHz plateau doesn't lift it);
//    started from a low percentile of the band's inner 80 %; level-corrected
//    to the median of the noise bins. The spread of the noise bins sets the
//    detection threshold (sigma1).
// 3. Continuous signals: base bins over 1 + Z sigma1 / sqrt(s) after a
//    boxcar of s = 1, 4, 16, 64 bins over the ratios clipped at
//    1 + 3 (T - 1) (so a box only fires when a third of it holds power: two
//    strong neighbours can't bridge the gap between them), each mask eroded
//    by s/6 (the most such a box smears), united; runs = regions; a deep
//    valley splits a region. Gaps with excess power between runs are
//    closed; carrier + two symmetric sidebands (AM), a comb of evenly spaced
//    lines (chirp preamble, pulse train, MFSK) and two tones whose
//    short-frame powers are anti-correlated (FSK) become one region; a narrow
//    strong peak well off the centre of a wide region (a carrier on a DAB
//    block) without a mirror image becomes a region of its own.
// 4. Each region: the band holding 98 % of its power over the floor (1 % cut
//    at each side) = bw, its power centroid = offset, its mean excess = SNR;
//    duty and max-hold from the short spectrogram.
// 5. Bursts the mean misses (ADS-B, DME, short ISM bursts): the short
//    spectrogram's frames, regions found so far masked, at box widths of 1 to
//    256 short bins against Gamma thresholds for a family-wise false-alarm
//    rate; the bins hit, the frames that hit them, their mean spectrum ->
//    centre and width.
// 6. Band filling: when nothing covers most of the band, the autocorrelation
//    of what is left (detections replaced by the floor) at lags of 8 us to
//    2.5 ms - a cyclic prefix, a TV line, a symbol repeat - says the "floor"
//    is a wideband signal: one detection for the whole band.

#include "sc_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>

namespace sigclass::detail {

namespace {

constexpr double HANN_VAR = 1.944;   // variance inflation of a sum of adjacent Hann bins

double kappa_bins(int w) { return w <= 1 ? 1.0 : double(w) / HANN_VAR; }

struct Run {
    int a, b;
};

void runs_of(const std::vector<uint8_t>& m, int n, std::vector<Run>& out, int close_gap = 0) {
    out.clear();
    int i = 0;
    while (i < n) {
        if (!m[size_t(i)]) {
            i++;
            continue;
        }
        int j = i;
        while (j + 1 < n && m[size_t(j + 1)]) j++;
        if (!out.empty() && i - out.back().b - 1 <= close_gap) out.back().b = j;
        else out.push_back({i, j});
        i = j + 1;
    }
}

// bins of x (mean 1 under noise; one bin ~ Gamma(kappa1) / kappa1) that are
// significant at a box width of 1, 4, 16, 64 or 256 bins (family-wise p
// per scale); each bin clipped like the continuous detector, masks eroded
// by a sixth of a box and united
void mask_multiscale(const double* x, int n, double kappa1, double p, std::vector<uint8_t>& mask) {
    mask.assign(size_t(n), 0);
    std::vector<double> pc(static_cast<size_t>(n) + 1);
    std::vector<uint8_t> m(static_cast<size_t>(n));
    std::vector<Run> runs;
    for (int s : {1, 4, 16, 64, 256}) {
        if (s > 1 && s > n) break;
        const double kap = s == 1 ? kappa1 : kappa1 * s / HANN_VAR;
        const double T = gamma_threshold(kap, p / std::max(1, n / std::max(1, s / 2)));
        const double cap = s == 1 ? 1e30 : 1.0 + 3.0 * (T - 1.0);
        pc[0] = 0.0;
        for (int i = 0; i < n; i++) pc[size_t(i) + 1] = pc[size_t(i)] + std::min(x[i], cap);
        for (int i = 0; i < n; i++) {
            const int a = std::max(0, i - s / 2), b = std::min(n, a + s);
            m[size_t(i)] = (pc[size_t(b)] - pc[size_t(a)]) / double(b - a) > T;
        }
        runs_of(m, n, runs);
        for (const Run& r : runs)
            for (int i = r.a + s / 6; i <= r.b - s / 6; i++) mask[size_t(i)] = 1;
    }
}

}  // namespace

// ------------------------------------------------------------------ floor

void Detector::estimate_floor(const FrontEnd& fe, const Config& cfg) {
    (void)cfg;
    const double fs = fe.fs;
    std::vector<float> m(NCELL, 0.0f), Fc(NCELL, 0.0f), nf(NCELL, 0.0f);
    std::vector<uint8_t> cv(NCELL, 0), noise(NCELL, 0);
    std::vector<float> vals;
    vals.reserve(CELL_BASE);
    for (int c = 0; c < NCELL; c++) {
        vals.clear();
        for (int i = c * CELL_BASE; i < (c + 1) * CELL_BASE; i++)
            if (valid_[size_t(i)]) vals.push_back(std::log10(std::max(pb_[size_t(i)], 1e-30f)));
        if (vals.size() >= 10) {
            m[size_t(c)] = quantile(vals.data(), vals.size(), 0.3, scratch_);
            cv[size_t(c)] = 1;
        }
    }
    vals.clear();
    for (int c = 0; c < NCELL; c++) {
        const double f = ((c + 0.5) * CELL_BASE * BASE_BINS - LONG_N / 2) * fs / LONG_N;
        if (cv[size_t(c)] && std::fabs(f) < 0.4 * fs) vals.push_back(m[size_t(c)]);
    }
    if (vals.empty())
        for (int c = 0; c < NCELL; c++)
            if (cv[size_t(c)]) vals.push_back(m[size_t(c)]);
    const float g = vals.empty() ? -10.0f : quantile(vals.data(), vals.size(), 0.1, scratch_);
    std::fill(Fc.begin(), Fc.end(), g);

    for (int it = 0; it < 6; it++) {
        const float tau = it == 0 ? 0.3f : 0.1f;   // log10: 3 dB, then 1 dB
        int nn = 0;
        for (int c = 0; c < NCELL; c++) {
            noise[size_t(c)] = cv[size_t(c)] && m[size_t(c)] < Fc[size_t(c)] + tau;
            nn += noise[size_t(c)];
        }
        if (nn == 0) break;
        for (int c = 0; c < NCELL; c++) {
            if (!noise[size_t(c)]) continue;
            vals.clear();
            for (int d = std::max(0, c - 4); d <= std::min(NCELL - 1, c + 4); d++)
                if (noise[size_t(d)]) vals.push_back(m[size_t(d)]);
            nf[size_t(c)] = quantile(vals.data(), vals.size(), 0.5, scratch_);
        }
        // bridge the signal cells linearly, flat beyond the outermost noise cells
        int prev = -1;
        for (int c = 0; c <= NCELL; c++) {
            if (c < NCELL && !noise[size_t(c)]) continue;
            if (prev < 0) {
                for (int d = 0; d < c && c < NCELL; d++) nf[size_t(d)] = nf[size_t(c)];
            } else {
                const int hi = c < NCELL ? c : -1;
                for (int d = prev + 1; d < (hi < 0 ? NCELL : hi); d++) {
                    if (hi < 0) nf[size_t(d)] = nf[size_t(prev)];
                    else {
                        const float t = float(d - prev) / float(hi - prev);
                        nf[size_t(d)] = (1.0f - t) * nf[size_t(prev)] + t * nf[size_t(hi)];
                    }
                }
            }
            prev = c < NCELL ? c : prev;
        }
        Fc = nf;
    }

    // to fine bins (per fine bin = per base bin / BASE_BINS), linear between cell centres
    std::vector<float> lin(NCELL);
    for (int c = 0; c < NCELL; c++) lin[size_t(c)] = std::pow(10.0f, Fc[size_t(c)]) / BASE_BINS;
    floor_f_.resize(LONG_N);
    const int cw = CELL_BASE * BASE_BINS;
    for (int i = 0; i < LONG_N; i++) {
        const float u = (float(i) + 0.5f) / float(cw) - 0.5f;
        int c0 = int(std::floor(u));
        float t = u - float(c0);
        if (c0 < 0) { c0 = 0; t = 0.0f; }
        if (c0 >= NCELL - 1) { c0 = NCELL - 2; t = 1.0f; }
        floor_f_[size_t(i)] = (1.0f - t) * lin[size_t(c0)] + t * lin[size_t(c0) + 1];
    }
    fb_.resize(NBASE);
    for (int i = 0; i < NBASE; i++) {
        double s = 0.0;
        for (int k = 0; k < BASE_BINS; k++) s += floor_f_[size_t(i * BASE_BINS + k)];
        fb_[size_t(i)] = float(s);
    }
    // level correction and the noise spread from the bins of the noise cells
    vals.clear();
    for (int c = 0; c < NCELL; c++) {
        if (!noise[size_t(c)]) continue;
        for (int i = c * CELL_BASE; i < (c + 1) * CELL_BASE; i++)
            if (valid_[size_t(i)] && fb_[size_t(i)] > 0.0f) vals.push_back(pb_[size_t(i)] / fb_[size_t(i)]);
    }
    float med = 1.0f;
    sigma1_ = 0.1;
    if (vals.size() >= 50) {
        med = quantile(vals.data(), vals.size(), 0.5, scratch_);
        if (!(med > 0.0f)) med = 1.0f;
        for (float& v : vals) v = std::fabs(v / med - 1.0f);
        sigma1_ = std::max(0.01, 1.4826 * quantile(vals.data(), vals.size(), 0.5, scratch_));
    }
    for (float& f : floor_f_) f *= med;
    for (float& f : fb_) f *= med;
}

// band power series of short bins k0..k1: e_t = power / floor - 1
void Detector::band_series(const FrontEnd& fe, int k0, int k1, std::vector<float>& e, double* sigma) const {
    k0 = std::clamp(k0, 0, SPEC_N - 1);
    k1 = std::clamp(k1, k0, SPEC_N - 1);
    double fl = 0.0;
    for (int k = k0; k <= k1; k++) fl += fs_floor_[size_t(k)];
    const double inv = fl > 0.0 ? 1.0 / fl : 0.0;
    e.resize(size_t(fe.nframes));
    // whole blocks from the block sums, the ragged ends bin by bin
    const int q0 = (k0 + SPEC_BLK - 1) / SPEC_BLK, q1 = (k1 + 1) / SPEC_BLK;   // blocks q0 .. q1-1 inside
    for (int t = 0; t < fe.nframes; t++) {
        const float* row = fe.spec.data() + size_t(t) * SPEC_N;
        double s = 0.0;
        if (q1 > q0) {
            const double* bs = fe.spec_blk.data() + size_t(t) * SPEC_BLKS;
            for (int q = q0; q < q1; q++) s += bs[q];
            for (int k = k0; k < q0 * SPEC_BLK; k++) s += row[k];
            for (int k = q1 * SPEC_BLK; k <= k1; k++) s += row[k];
        } else {
            for (int k = k0; k <= k1; k++) s += row[k];
        }
        e[size_t(t)] = float(s * inv - 1.0);
    }
    if (sigma) *sigma = 1.0 / std::sqrt(kappa_bins(k1 - k0 + 1));
}

// --------------------------------------------------------------- measure

bool Detector::measure(const FrontEnd& fe, const Region& rg, Cand& c, const Config& cfg) {
    (void)cfg;
    const double df = fe.fs / LONG_N;
    const int i0 = std::max(0, (rg.a - 1) * BASE_BINS), i1 = std::min(LONG_N - 1, (rg.b + 2) * BASE_BINS - 1);
    scratch2_.assign(size_t(i1 - i0 + 1), 0.0f);
    double E = 0.0;
    for (int i = i0; i <= i1; i++) {
        const float fl = floor_f_[size_t(i)] * rg.floor_mult;
        float ex = std::max(p_[size_t(i)] - fl, 0.0f);
        for (size_t q = 0; q < rg.clip.size(); q++)
            if (i >= rg.clip[q].first && i <= rg.clip[q].second)
                ex = std::min(ex, std::max(rg.clip_level[q] - 1.0f, 0.0f) * fl);
        scratch2_[size_t(i - i0)] = ex;
        E += ex;
    }
    if (!(E > 0.0)) return false;
    double cum = 0.0;
    int lo = i0, hi = i1;
    bool have_lo = false;
    for (int i = i0; i <= i1; i++) {
        cum += scratch2_[size_t(i - i0)];
        if (!have_lo && cum >= 0.01 * E) { lo = i; have_lo = true; }
        if (cum >= 0.99 * E) { hi = i; break; }
    }
    double se = 0.0, sf = 0.0, sfl = 0.0;
    for (int i = lo; i <= hi; i++) {
        const double ex = scratch2_[size_t(i - i0)];
        se += ex;
        sf += ex * double(i - LONG_N / 2);
        sfl += floor_f_[size_t(i)] * rg.floor_mult;
    }
    if (!(se > 0.0)) return false;
    const double cbin = sf / se;
    c.det.offset_hz = cbin * df;
    c.det.bw_hz = double(hi - lo + 1) * df;
    c.det.snr_db = clampf(db10(se / std::max(sfl, 1e-30)), -20.0f, 120.0f);
    c.det.peak_db = c.det.snr_db;
    c.det.duty = 1.0f;
    c.centre_bin = int(std::lround(cbin));
    return true;
}

bool Detector::spectro_stats(const FrontEnd& fe, Cand& c, bool continuous) {
    if (fe.nframes < 4) return false;
    const double dfs = fe.fs / SPEC_N;
    const double flo = c.det.offset_hz - 0.5 * c.det.bw_hz, fhi = c.det.offset_hz + 0.5 * c.det.bw_hz;
    int k0 = int(std::floor((flo + 0.5 * fe.fs) / dfs)), k1 = int(std::floor((fhi + 0.5 * fe.fs) / dfs));
    if (k1 - k0 < 2) { k0--; k1++; }   // a narrow signal leaks into the neighbouring bins
    std::vector<float> e;
    double sig = 1.0;
    band_series(fe, k0, k1, e, &sig);
    const float q99 = quantile(e.data(), e.size(), 0.99, scratch_);
    const float q25 = quantile(e.data(), e.size(), 0.25, scratch_);
    double mean = 0.0;
    for (float v : e) mean += v;
    mean /= std::max<size_t>(1, e.size());
    // on = above the noise and above a small fraction of the loud frames (a
    // noise-like or voice-modulated signal's frame power varies a lot); a
    // signal whose quiet frames still show it is continuous
    const bool steady = q25 > std::max(2.0 * sig, 0.25 * mean);
    const float th = steady ? std::max(float(3.0 * sig), 0.08f * q99) : std::max(float(4.0 * sig), 0.08f * q99);
    // frame by frame the signal shows: on >= 10 % of the time clearly above
    // the frame noise, or sparse bursts far above it
    const float q90 = quantile(e.data(), e.size(), 0.90, scratch_);
    float emax = 0.0f;
    for (float v : e) emax = std::max(emax, v);
    // (the noise's own maximum over the frames: the Gamma quantile at 1e-3 / T)
    const int bw_bins = k1 - k0 + 1;
    const auto key = std::make_pair(-bw_bins, 1e-3 / std::max(1, fe.nframes));   // (negative: not a burst-box key)
    auto it = thr_cache_.find(key);
    if (it == thr_cache_.end()) it = thr_cache_.emplace(key, gamma_threshold(kappa_bins(bw_bins), key.second)).first;
    const double nmax = it->second - 1.0;
    const bool visible = q90 > 6.0 * sig || emax > 2.0 * nmax;
    if (visible) {
        int on = 0;
        float mx = 0.0f;
        double son = 0.0;
        for (float v : e) {
            if (v > th) { on++; son += v; }
            mx = std::max(mx, v);
        }
        c.det.duty = float(on) / float(e.size());
        c.det.peak_db = clampf(db10(mx), -20.0f, 120.0f);
        if (continuous) {
            if (c.det.duty < 0.5f && c.det.duty > 0.0f)
                c.det.snr_db = clampf(c.det.snr_db - db10(c.det.duty), -20.0f, 120.0f);
            if (c.det.duty < 0.25f && c.det.duty > 0.0f) {
                // the time average under-states a bursty signal's width: measure
                // it in the frames where it is on (short-spectrogram resolution)
                const int w = k1 - k0 + 1;
                const int a = std::max(0, k0 - w), b = std::min(SPEC_N - 1, k1 + w);
                std::vector<double> cond(static_cast<size_t>(b - a + 1), 0.0);
                for (int t = 0; t < fe.nframes; t++) {
                    if (!(e[size_t(t)] > th)) continue;
                    const float* row = fe.spec.data() + size_t(t) * SPEC_N;
                    for (int k = a; k <= b; k++) cond[size_t(k - a)] += row[k];
                }
                double E = 0.0;
                for (int k = a; k <= b; k++) {
                    cond[size_t(k - a)] = std::max(0.0, cond[size_t(k - a)] / std::max(on, 1) - fs_floor_[size_t(k)]);
                    E += cond[size_t(k - a)];
                }
                if (E > 0.0) {
                    double cum = 0.0;
                    int lo = a, hi = b;
                    bool hl = false;
                    for (int k = a; k <= b; k++) {
                        cum += cond[size_t(k - a)];
                        if (!hl && cum >= 0.01 * E) { lo = k; hl = true; }
                        if (cum >= 0.99 * E) { hi = k; break; }
                    }
                    // (less the Hann window's own spread of about two bins)
                    const double bwc = std::max(1.0, double(hi - lo + 1) - 2.0) * dfs;
                    if (bwc > 1.2 * c.det.bw_hz) {
                        double se = 0.0, sf = 0.0;
                        for (int k = lo; k <= hi; k++) {
                            se += cond[size_t(k - a)];
                            sf += cond[size_t(k - a)] * (k + 0.5 - SPEC_N / 2);
                        }
                        c.det.bw_hz = bwc;
                        if (se > 0.0) {
                            c.det.offset_hz = sf / se * dfs;
                            c.centre_bin = int(std::lround(c.det.offset_hz / (fe.fs / LONG_N)));
                        }
                    }
                }
            }
            c.det.peak_db = std::max(c.det.peak_db, c.det.snr_db);
        } else if (on > 0) {
            c.det.snr_db = clampf(db10(son / on), -20.0f, 120.0f);
        }
    } else {
        c.det.duty = 1.0f;
        c.det.peak_db = c.det.snr_db;
    }
    c.det.bursty = c.det.duty < 0.5f;
    return visible;
}

// ------------------------------------------------------------------ bursts

void Detector::burst_search(const FrontEnd& fe, const Config& cfg, const std::vector<Cand>& cont,
                            std::vector<Cand>& out) {
    const int T = fe.nframes;
    if (T < 8) return;
    const double fs = fe.fs, dfs = fs / SPEC_N;
    std::vector<uint8_t> masked(SPEC_N, 0);
    for (int k = 0; k < SPEC_N; k++) {
        const double f = (k + 0.5 - SPEC_N / 2) * dfs;
        if (std::fabs(f) > (0.5 - cfg.edge_guard) * fs) masked[size_t(k)] = 1;
    }
    masked[SPEC_N / 2] = 1;   // DC
    // the continuous detections (and a quarter of their width each side) are not searched again
    for (const Cand& c : cont) {
        const double flo = c.det.offset_hz - 0.5 * c.det.bw_hz, fhi = c.det.offset_hz + 0.5 * c.det.bw_hz;
        // (a bursty one: its full width, for the splatter of its switching)
        const int g = std::max(1, int(std::ceil((c.det.bursty ? 1.0 : 0.25) * c.det.bw_hz / dfs)));
        const int k0 = int(std::floor((flo + 0.5 * fs) / dfs)) - g, k1 = int(std::floor((fhi + 0.5 * fs) / dfs)) + g;
        for (int k = std::max(0, k0); k <= std::min(SPEC_N - 1, k1); k++) masked[size_t(k)] = 1;
    }
    // each bin against its own long-term mean (or the floor, if higher): only
    // what is bursty relative to its average hits (not a continuous skirt)
    std::vector<float> inv_floor(SPEC_N);
    for (int k = 0; k < SPEC_N; k++) {
        const float ref = std::max(fs_floor_[size_t(k)], fe.spec_mean.empty() ? 0.0f : fe.spec_mean[size_t(k)]);
        inv_floor[size_t(k)] = ref > 0.0f ? 1.0f / ref : 0.0f;
    }
    std::vector<int> ucount(SPEC_N + 1, 0);
    for (int k = 0; k < SPEC_N; k++) ucount[size_t(k) + 1] = ucount[size_t(k)] + (masked[size_t(k)] ? 0 : 1);

    static const int scales[] = {1, 4, 16, 64, 256};
    constexpr int NS = 5;
    // thresholds per scale and number of unmasked bins u: the box mean is
    // 1 + (mean of the u unmasked bins - 1) * u / s
    std::vector<float> thr[NS];
    for (int si = 0; si < NS; si++) {
        const int s = scales[si], stride = std::max(1, s / 2);
        const int nbox = (SPEC_N - s) / stride + 1;
        const double p = std::max(1e-12, double(cfg.burst_fa) / (double(T) * nbox));
        thr[si].assign(size_t(s) + 1, 1e30f);
        // the exact threshold at a few u, interpolated in between (it is smooth in u)
        std::vector<int> us;
        for (int u = 1; u <= s; u = u < 4 ? u + 1 : int(u * 1.5)) us.push_back(u);
        if (us.back() != s) us.push_back(s);
        std::vector<double> tv(us.size());
        for (size_t q = 0; q < us.size(); q++) {
            const auto key = std::make_pair(us[q], p);
            auto it = thr_cache_.find(key);
            if (it == thr_cache_.end()) it = thr_cache_.emplace(key, gamma_threshold(kappa_bins(us[q]), p)).first;
            tv[q] = it->second;
        }
        for (size_t q = 0; q + 1 < us.size() || q == 0; q++) {
            const int ua = us[q], ub = q + 1 < us.size() ? us[q + 1] : us[q];
            for (int u = ua; u <= ub; u++) {
                const double t = ub > ua ? double(u - ua) / (ub - ua) : 0.0;
                const double th = (1.0 - t) * tv[q] + t * tv[std::min(q + 1, us.size() - 1)];
                thr[si][size_t(u)] = float(1.0 + (th - 1.0) * double(u) / s);
            }
            if (q + 1 >= us.size()) break;
        }
    }

    struct Hit {
        int t, k0, k1;
    };
    std::vector<Hit> hits;
    // the threshold of every box on the sum scale (1e30: all masked)
    std::vector<double> boxthr[NS];
    for (int si = 0; si < NS; si++) {
        const int s = scales[si], stride = std::max(1, s / 2);
        for (int k0 = 0; k0 + s <= SPEC_N; k0 += stride) {
            const int u = ucount[size_t(k0 + s)] - ucount[size_t(k0)];
            boxthr[si].push_back(u == 0 ? 1e300 : double(thr[si][size_t(u)]) * s);
        }
    }
    std::vector<double> pre(SPEC_N + 1);   // double: a strong bin must not swamp its neighbours' sums
    std::vector<float> rrow(SPEC_N);
    std::vector<uint8_t> hitbin(SPEC_N, 0);
    for (int t = 0; t < T && hits.size() < 400000; t++) {
        const float* row = fe.spec.data() + size_t(t) * SPEC_N;
        for (int k = 0; k < SPEC_N; k++) rrow[size_t(k)] = masked[size_t(k)] ? 1.0f : row[k] * inv_floor[size_t(k)];
        pre[0] = 0.0;
        for (int k = 0; k < SPEC_N; k++) pre[size_t(k) + 1] = pre[size_t(k)] + rrow[size_t(k)];
        for (int si = 0; si < NS; si++) {
            const int s = scales[si], stride = std::max(1, s / 2);
            const double* bt = boxthr[si].data();
            int j = 0;
            for (int k0 = 0; k0 + s <= SPEC_N; k0 += stride, j++) {
                if (pre[size_t(k0 + s)] - pre[size_t(k0)] > bt[j]) {
                    hits.push_back({t, k0, k0 + s - 1});
                    for (int k = k0; k < k0 + s; k++)
                        if (!masked[size_t(k)]) hitbin[size_t(k)] = 1;
                }
            }
        }
    }
    if (hits.empty()) return;
    // masked bins (a detection inside a burst hump: ADS-B's carrier line) don't split a run
    for (int k = 0; k < SPEC_N; k++) {
        if (!masked[size_t(k)] || hitbin[size_t(k)]) continue;
        int j = k;
        while (j + 1 < SPEC_N && masked[size_t(j + 1)]) j++;
        if (k > 0 && j + 1 < SPEC_N && hitbin[size_t(k - 1)] && hitbin[size_t(j + 1)] && j - k + 1 <= 64)
            for (int q = k; q <= j; q++) hitbin[size_t(q)] = 1;
        k = j;
    }
    std::vector<Run> runs;
    runs_of(hitbin, SPEC_N, runs, 2);
    std::vector<int> frames;
    std::vector<double> cond;
    std::vector<uint8_t> hot;
    std::vector<Run> sub;
    for (const Run& r : runs) {
        frames.clear();
        for (const Hit& h : hits)
            if (h.k1 >= r.a && h.k0 <= r.b) frames.push_back(h.t);
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        if (frames.empty()) continue;
        const int w = r.b - r.a + 1;
        cond.assign(size_t(w), 0.0);
        for (int t : frames) {
            const float* row = fe.spec.data() + size_t(t) * SPEC_N;
            for (int k = r.a; k <= r.b; k++)
                cond[size_t(k - r.a)] += masked[size_t(k)] ? 1.0 : double(row[k] * inv_floor[size_t(k)]);
        }
        for (double& v : cond) v /= double(frames.size());
        // masked stretches (a detection inside the hump): bridged linearly
        for (int k = 0; k < w; k++) {
            if (!masked[size_t(r.a + k)]) continue;
            int j = k;
            while (j + 1 < w && masked[size_t(r.a + j + 1)]) j++;
            const double l = k > 0 ? cond[size_t(k - 1)] : (j + 1 < w ? cond[size_t(j + 1)] : 1.0);
            const double rv = j + 1 < w ? cond[size_t(j + 1)] : l;
            for (int q = k; q <= j; q++) cond[size_t(q)] = l + (rv - l) * double(q - k + 1) / double(j - k + 2);
            k = j;
        }
        // the hit frames' mean spectrum: where it shows the burst (smoothed
        // at several widths - a mean of a few frames is noisy bin by bin)
        const double kap = 0.7 * double(frames.size());
        const double tc = gamma_threshold(kap, 1e-3 / w);
        mask_multiscale(cond.data(), w, kap, 1e-3, hot);
        runs_of(hot, w, sub, 2);
        if (sub.empty()) {
            int km = 0;
            for (int k = 1; k < w; k++)
                if (cond[size_t(k)] > cond[size_t(km)]) km = k;
            if (cond[size_t(km)] > 1.0 + 0.5 * (tc - 1.0)) sub.push_back({km, km});
        }
        for (const Run& s : sub) {
            const int a = std::max(0, s.a - 1), b = std::min(w - 1, s.b + 1);
            double E = 0.0;
            for (int k = a; k <= b; k++) E += std::max(cond[size_t(k)] - 1.0, 0.0);
            if (!(E > 0.0)) continue;
            double cum = 0.0;
            int lo = a, hi = b;
            bool hl = false;
            for (int k = a; k <= b; k++) {
                cum += std::max(cond[size_t(k)] - 1.0, 0.0);
                if (!hl && cum >= 0.01 * E) { lo = k; hl = true; }
                if (cum >= 0.99 * E) { hi = k; break; }
            }
            double se = 0.0, sf = 0.0;
            for (int k = lo; k <= hi; k++) {
                const double ex = std::max(cond[size_t(k)] - 1.0, 0.0);
                se += ex;
                sf += ex * (double(k + r.a) + 0.5 - SPEC_N / 2);
            }
            if (!(se > 0.0)) continue;
            Cand c;
            c.det.offset_hz = sf / se * dfs;
            c.det.bw_hz = std::max(1.0, double(hi - lo + 1) - 2.0) * dfs;   // less the window's spread
            c.det.snr_db = clampf(db10(se / (hi - lo + 1)), -20.0f, 120.0f);
            c.det.peak_db = c.det.snr_db;
            c.det.duty = float(frames.size()) / float(T);
            c.centre_bin = int(std::lround(c.det.offset_hz / (fs / LONG_N)));
            // hits that its own band doesn't show frame by frame came from elsewhere
            if (!spectro_stats(fe, c, false)) continue;
            c.det.bursty = true;
            out.push_back(c);
        }
    }
}

// ------------------------------------------------------------ band filling

bool Detector::band_fill(const FrontEnd& fe, const Config& cfg, const std::vector<Cand>& found, Cand& c, int force) {
    const double fs = fe.fs, df = fs / LONG_N;
    for (const Cand& f : found)
        if (f.det.bw_hz >= 0.6 * fs) return false;
    fill_fft_.init(LONG_N, false);
    fbuf_.resize(LONG_N);
    scratch2_.assign(p_.begin(), p_.end());
    const int edge = int((0.5 - cfg.edge_guard) * LONG_N);   // |i - N/2| beyond this: floor
    for (int i = 0; i < LONG_N; i++)
        if (std::abs(i - LONG_N / 2) > edge) scratch2_[size_t(i)] = floor_f_[size_t(i)];
    for (const Cand& f : found) {
        const double w = f.det.bw_hz / df;
        const double half = 0.5 * w + std::max(10.0, 0.25 * w);
        const double cb = f.det.offset_hz / df + LONG_N / 2;
        for (int i = std::max(0, int(cb - half)); i <= std::min(LONG_N - 1, int(cb + half)); i++)
            scratch2_[size_t(i)] = floor_f_[size_t(i)];
    }
    for (int i = 0; i < LONG_N; i++)
        fbuf_[size_t((i + LONG_N / 2) % LONG_N)] = cf(scratch2_[size_t(i)], 0.0f);
    acf_.resize(LONG_N);
    avec<cf>& acf = acf_;
    fill_fft_.inv(fbuf_.data(), acf.data());
    const double r0 = std::abs(acf[0]);
    if (!(r0 > 0.0)) return false;
    // (from 25 us: shorter lags hold the smooth correlation of an uneven floor, no CP is that short)
    const int l0 = std::max(2, int(std::ceil(25e-6 * fs))), l1 = std::min(LONG_N / 2 - 1, int(2.5e-3 * fs));
    double best = 0.0;
    int lbest = l0;
    for (int l = l0; l <= l1; l++) {
        const double v = double(std::abs(acf[size_t(l)])) / r0;
        if (v > best) { best = v; lbest = l; }
    }
    // a cyclic prefix / line period is a PEAK at its lag; coloured noise (a
    // front end's uneven floor, residual skirts) decays smoothly: the peak must
    // stand out of the lags around it (their median, +-25 % of the lag)
    double prom = 0.0;
    {
        scratch_.clear();
        const int a = std::max(l0, int(0.75 * lbest)), b = std::min(l1, int(1.25 * lbest) + 1);
        for (int l = a; l <= b; l++)
            if (std::abs(l - lbest) > 2) scratch_.push_back(float(std::abs(acf[size_t(l)]) / r0));
        if (!scratch_.empty()) {
            std::nth_element(scratch_.begin(), scratch_.begin() + long(scratch_.size() / 2), scratch_.end());
            prom = best / std::max(double(scratch_[scratch_.size() / 2]), 1e-9);
        }
    }
    fill_corr_ = best;
    if (std::getenv("SIGCLASS_DEBUG_FILL")) std::fprintf(stderr, "band_fill: corr %.4f at %d samples, prominence %.2f%s\n", best, lbest, prom, force ? " (forced)" : "");
    // (an irregular comb of lines - OFDM pilots - still needs the peak to stand out:
    // a busy band of narrow carriers (a trunked radio site, 30+ lines) has no CP peak)
    if (force == 0 && !(best > cfg.band_fill_corr && prom >= cfg.band_fill_prominence)) return false;
    if (force == 1 && !(prom >= cfg.band_fill_prominence)) return false;
    // (bursty pieces kept: a pulsed band - SSR replies - can reach 4.5 there)
    if (force == 3 && !(prom >= 1.25 * cfg.band_fill_prominence)) return false;
    // the whole band: centroid and 98 % width of the power inside the guards
    double E = 0.0;
    for (int i = LONG_N / 2 - edge; i <= LONG_N / 2 + edge; i++) E += p_[size_t(i)];
    double cum = 0.0, se = 0.0, sf = 0.0;
    int lo = LONG_N / 2 - edge, hi = LONG_N / 2 + edge;
    bool hl = false;
    for (int i = LONG_N / 2 - edge; i <= LONG_N / 2 + edge; i++) {
        cum += p_[size_t(i)];
        if (!hl && cum >= 0.01 * E) { lo = i; hl = true; }
        if (cum >= 0.99 * E) { hi = i; break; }
    }
    for (int i = lo; i <= hi; i++) {
        se += p_[size_t(i)];
        sf += double(p_[size_t(i)]) * (i - LONG_N / 2);
    }
    c = Cand{};
    c.det.offset_hz = se > 0.0 ? sf / se * df : 0.0;
    c.det.bw_hz = double(hi - lo + 1) * df;
    c.det.snr_db = 0.0f;
    c.det.peak_db = 0.0f;
    c.det.duty = 1.0f;
    c.det.fills_band = true;
    c.det.view = NUM_VIEWS - 1;
    c.centre_bin = int(std::lround(c.det.offset_hz / df));
    return true;
}

// ----------------------------------------------------------------- segment

// regions of base bins lo..hi where rr (power / reference) shows a signal:
// multi-scale masks, gaps with power in them closed, then the merge rules
void Detector::segment(const FrontEnd& fe, const Config& cfg, const float* rr, int lo, int hi, float min_ratio,
                       std::vector<Region>& regs) {
    regs.clear();
    const int n = hi - lo + 1;
    if (n < 1) return;
    const double fs = fe.fs, df = fs / LONG_N;
    prefix_.assign(size_t(n) + 1, 0.0);
    for (int i = 0; i < n; i++) prefix_[size_t(i) + 1] = prefix_[size_t(i)] + rr[lo + i];
    mask_.assign(size_t(n), 0);
    std::vector<Run> runs;
    std::vector<double> pc(static_cast<size_t>(n) + 1);
    static const int scales[] = {1, 4, 16, 64};
    for (int s : scales) {
        if (s > 1 && s > n / 2) break;
        const double T = std::max(1.0 + cfg.detect_z * sigma1_ / std::sqrt(double(s)), double(min_ratio));
        // each bin clipped at 1 + 3 (T - 1): a box is significant only when
        // power is spread over it - strong neighbours at its two ends (two
        // channels with a gap) or one strong line can't make it so
        const double cap = s == 1 ? 1e30 : 1.0 + 3.0 * (T - 1.0);
        pc[0] = 0.0;
        for (int i = 0; i < n; i++) pc[size_t(i) + 1] = pc[size_t(i)] + std::min(double(rr[lo + i]), cap);
        mask2_.assign(size_t(n), 0);
        for (int i = 0; i < n; i++) {
            const int a = std::max(0, i - s / 2), b = std::min(n, a + s);
            const double mean = (pc[size_t(b)] - pc[size_t(a)]) / double(b - a);
            mask2_[size_t(i)] = mean > T;
        }
        runs_of(mask2_, n, runs);
        // with the clipping a box needs a third of its width filled, so it
        // smears at most s/6 bins beyond a signal's edge: erode by that
        const int e = s / 6;
        for (const Run& r : runs)
            for (int i = r.a + e; i <= r.b - e; i++) mask_[size_t(i)] = 1;
    }
    for (int i = 0; i < n; i++) mask_[size_t(i)] &= valid_[size_t(lo + i)];
    runs_of(mask_, n, runs);
    auto power_of = [&](int a, int b) {   // absolute base-bin indices
        double s = 0.0;
        for (int i = a; i <= b; i++) s += rr[i] - 1.0;
        return s;
    };
    const double gap_level = std::max(0.0, 0.5 * (double(min_ratio) - 1.0));
    for (const Run& r0 : runs) {
        const Run r{r0.a + lo, r0.b + lo};
        // a gap with excess power left in it is part of the signal
        if (!regs.empty()) {
            Region& L = regs.back();
            const int gap = r.a - L.b - 1;
            const int wmin = std::min(L.b - L.a + 1, r.b - r.a + 1);
            const int wsum = (L.b - L.a + 1) + (r.b - r.a + 1);
            bool merge = gap <= 1;
            if (!merge && gap <= std::max({2, wmin / 4, wsum / 2})) {
                // (the valley split below separates real neighbours again)
                const double gm = (prefix_[size_t(r.a - lo)] - prefix_[size_t(L.b - lo) + 1]) / gap;
                merge = gm > 1.0 + std::max(3.0 * sigma1_ / std::sqrt(double(gap)), gap_level);
            }
            if (merge) {
                L.b = r.b;
                continue;
            }
        }
        Region g;
        g.a = r.a;
        g.b = r.b;
        regs.push_back(g);
    }
    // a deep valley splits a region (two adjacent channels whose skirts
    // meet above the floor); the pair rules below join halves that belong
    // together (FSK tones, DSB sidebands)
    for (size_t gi = 0; gi < regs.size(); gi++) {
        Region& g = regs[gi];
        const int W = g.b - g.a + 1;
        if (W < 12) continue;
        const int win = std::max(3, (W / 16) | 1);
        std::vector<double> sm(static_cast<size_t>(W));
        double tot = 0.0;
        for (int i = 0; i < W; i++) {
            const int a = std::max(0, i - win / 2), b = std::min(W - 1, i + win / 2);
            sm[size_t(i)] = (prefix_[size_t(g.a - lo + b) + 1] - prefix_[size_t(g.a - lo + a)]) / double(b - a + 1) - 1.0;
            tot += std::max(0.0, double(rr[g.a + i]) - 1.0);
        }
        std::vector<double> lmax(static_cast<size_t>(W)), rmax(static_cast<size_t>(W)), lpow(static_cast<size_t>(W) + 1, 0.0);
        for (int i = 0; i < W; i++) {
            lmax[size_t(i)] = std::max(i ? lmax[size_t(i - 1)] : -1e30, sm[size_t(i)]);
            lpow[size_t(i) + 1] = lpow[size_t(i)] + std::max(0.0, double(rr[g.a + i]) - 1.0);
        }
        for (int i = W - 1; i >= 0; i--) rmax[size_t(i)] = std::max(i < W - 1 ? rmax[size_t(i + 1)] : -1e30, sm[size_t(i)]);
        int best = -1;
        double bestq = 0.0;
        for (int i = win; i < W - win; i++) {
            if (sm[size_t(i)] > sm[size_t(i - 1)] || sm[size_t(i)] > sm[size_t(i + 1)]) continue;
            const double pk = std::min(lmax[size_t(i)], rmax[size_t(i)]);
            const double v = std::max(sm[size_t(i)], 1e-6);
            if (pk <= 0.0 || v > 0.12 * pk) continue;
            const double pl = lpow[size_t(i)], pr = tot - lpow[size_t(i)];
            if (pl < 0.1 * tot || pr < 0.1 * tot) continue;
            if (pk / v > bestq) { bestq = pk / v; best = i; }
        }
        if (best < 0) continue;
        Region r2;
        r2.a = g.a + best + 1;
        r2.b = g.b;
        g.b = g.a + best - 1;
        regs.insert(regs.begin() + long(gi) + 1, r2);
        gi--;   // look at the left part again
    }
    for (Region& g : regs) g.power = power_of(g.a, g.b);

    // carrier + two symmetric sidebands -> one (AM with a tone, DSB)
    for (size_t i = 0; i + 2 < regs.size();) {
        const Region &L = regs[i], &C = regs[i + 1], &R = regs[i + 2];
        const double cl = 0.5 * (L.a + L.b), cc = 0.5 * (C.a + C.b), cr = 0.5 * (R.a + R.b);
        const double dl = cc - cl, dr = cr - cc;
        const int wl = L.b - L.a + 1, wc = C.b - C.a + 1, wr = R.b - R.a + 1;
        const bool sym = std::fabs(dl - dr) <= 0.2 * 0.5 * (dl + dr) + 2.0;
        const bool carrier = C.power >= std::max(L.power, R.power) && wc <= 2 * std::min(wl, wr) + 2;
        const bool pair = std::fabs(db10(std::max(L.power, 1e-9) / std::max(R.power, 1e-9))) < 6.0f;
        const bool close = (R.b - L.a + 1) * df * BASE_BINS <= 40e3;
        if (sym && carrier && pair && close) {
            regs[i].b = R.b;
            regs[i].power = power_of(regs[i].a, regs[i].b);
            regs.erase(regs.begin() + long(i) + 1, regs.begin() + long(i) + 3);
        } else {
            i++;
        }
    }
    // a comb of evenly spaced lines (<= 5 kHz apart) -> one signal: a chirp
    // preamble, a pulse train, MFSK, AM with a tone (a missing line is allowed)
    {
        const double dfb = df * BASE_BINS;
        auto cen = [](const Region& g) { return 0.5 * (g.a + g.b); };
        for (size_t i = 0; i + 2 < regs.size();) {
            const double d0 = cen(regs[i + 1]) - cen(regs[i]);
            if (d0 * dfb > 5e3 || d0 < 2.0) { i++; continue; }
            size_t j = i + 1;
            while (j + 1 < regs.size()) {
                const double d = cen(regs[j + 1]) - cen(regs[j]);
                if (std::fabs(d - d0) > 0.2 * d0 + 1.0 && std::fabs(d - 2.0 * d0) > 0.2 * d0 + 1.0) break;
                j++;
            }
            bool ok = j - i + 1 >= 3;
            double pmax = 0.0, pmin = 1e300;
            for (size_t k = i; ok && k <= j; k++) {
                ok = regs[k].b - regs[k].a + 1 <= int(std::ceil(d0));
                pmax = std::max(pmax, regs[k].power);
                pmin = std::min(pmin, regs[k].power);
            }
            if (ok && pmin >= 0.01 * pmax) {
                regs[i].b = regs[j].b;
                regs[i].power = power_of(regs[i].a, regs[i].b);
                regs[i].comb = true;
                regs.erase(regs.begin() + long(i) + 1, regs.begin() + long(j) + 1);
            } else {
                i++;
            }
        }
    }
    // ... and evenly spaced lines continuing outward from a wide region's edge
    // (the tails of a periodic signal's line spectrum: a chirp preamble) join it
    {
        const double dfb = df * BASE_BINS;
        auto cen = [](const Region& g) { return 0.5 * (g.a + g.b); };
        auto narrow = [&](const Region& g, double d) { return g.b - g.a + 1 <= int(std::ceil(d)); };
        for (size_t i = 0; i < regs.size(); i++) {
            if (regs[i].b - regs[i].a + 1 < 64) continue;
            for (int dir = -1; dir <= 1; dir += 2) {
                // the lines next to region i on this side
                std::vector<size_t> run;
                for (long k = long(i) + dir; k >= 0 && k < long(regs.size()); k += dir) run.push_back(size_t(k));
                if (run.size() < 2) continue;
                const double d0 = std::fabs(cen(regs[run[1]]) - cen(regs[run[0]]));
                if (d0 < 2.0 || d0 * dfb > 5e3 || !narrow(regs[run[0]], d0) || !narrow(regs[run[1]], d0)) continue;
                const double edge = dir > 0 ? regs[run[0]].a - regs[i].b : regs[i].a - regs[run[0]].b;
                if (edge > 2.0 * d0) continue;
                size_t m = 2;
                while (m < run.size() && narrow(regs[run[m]], d0) &&
                       std::fabs(std::fabs(cen(regs[run[m]]) - cen(regs[run[m - 1]])) - d0) <= 0.2 * d0 + 1.0)
                    m++;
                const size_t last = run[m - 1];
                if (dir > 0) regs[i].b = regs[last].b;
                else regs[i].a = regs[last].a;
                regs[i].comb = true;
                const size_t lo_i = dir > 0 ? i + 1 : last, hi_i = dir > 0 ? last : i - 1;
                regs.erase(regs.begin() + long(lo_i), regs.begin() + long(hi_i) + 1);
                if (dir < 0) i -= m;
                regs[i].power = power_of(regs[i].a, regs[i].b);
            }
        }
    }
    // two parts whose short-frame powers move together - anti-correlated (FSK
    // tones) or correlated (DSB sidebands) - and look alike -> one
    if (fe.nframes >= 16) {
        const double dfs = fs / SPEC_N;
        auto kbin = [&](int base) { return int(std::floor((base + 0.5) * BASE_BINS * double(SPEC_N) / LONG_N)); };
        std::vector<float> ea, eb;
        for (size_t i = 0; i + 1 < regs.size();) {
            const Region &A = regs[i], &B = regs[i + 1];
            const int wa = A.b - A.a + 1, wb = B.b - B.a + 1;
            const int gap = B.a - A.b - 1;
            bool merge = false;
            const bool shape = std::max(wa, wb) <= 3 * std::min(wa, wb) &&
                               std::fabs(db10(std::max(A.power, 1e-9) / std::max(B.power, 1e-9))) < 6.0f &&
                               gap <= 3 * std::max(wa, wb) && (B.b - A.a + 1) * df * BASE_BINS <= 40e3;
            const int a0 = kbin(A.a), a1 = kbin(A.b), b0 = kbin(B.a), b1 = kbin(B.b);
            if (shape && b0 > a1) {
                band_series(fe, a0, a1, ea, nullptr);
                band_series(fe, b0, b1, eb, nullptr);
                double ma = 0, mb = 0;
                for (size_t t = 0; t < ea.size(); t++) { ma += ea[t]; mb += eb[t]; }
                ma /= double(ea.size());
                mb /= double(eb.size());
                double sab = 0, saa = 0, sbb = 0;
                for (size_t t = 0; t < ea.size(); t++) {
                    const double x = ea[t] - ma, y = eb[t] - mb;
                    sab += x * y; saa += x * x; sbb += y * y;
                }
                const double rho = saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0.0;
                const double z = rho * std::sqrt(double(ea.size()));
                merge = (rho < -0.15 && z < -4.0) || (rho > 0.5 && z > 6.0);
            }
            (void)dfs;
            if (merge) {
                regs[i].b = B.b;
                regs[i].power = power_of(regs[i].a, regs[i].b);
                regs.erase(regs.begin() + long(i) + 1);
            } else {
                i++;
            }
        }
    }
}

// ------------------------------------------------------------------ detect

void Detector::detect(const FrontEnd& fe, const Config& cfg, std::vector<Cand>& out) {
    out.clear();
    fill_corr_ = 0.0;
    const double fs = fe.fs, df = fs / LONG_N;
    const int dc = LONG_N / 2;
    p_.assign(fe.psd.begin(), fe.psd.end());
    // bridge the DC zone
    {
        const int h = std::max(1, int(std::ceil(cfg.dc_guard_hz / df)));
        double l = 0.0, r = 0.0;
        int nl = 0, nr = 0;
        for (int i = dc - h - 40; i < dc - h; i++) if (i >= 0) { l += p_[size_t(i)]; nl++; }
        for (int i = dc + h + 1; i <= dc + h + 40; i++) if (i < LONG_N) { r += p_[size_t(i)]; nr++; }
        l = nl ? l / nl : 0.0;
        r = nr ? r / nr : 0.0;
        for (int i = std::max(0, dc - h); i <= std::min(LONG_N - 1, dc + h); i++) {
            const double t = double(i - (dc - h)) / double(2 * h);
            p_[size_t(i)] = float((1.0 - t) * l + t * r);
        }
    }
    pb_.resize(NBASE);
    valid_.resize(NBASE);
    for (int i = 0; i < NBASE; i++) {
        double s = 0.0;
        for (int k = 0; k < BASE_BINS; k++) s += p_[size_t(i * BASE_BINS + k)];
        pb_[size_t(i)] = float(s);
        const double f = ((i + 0.5) * BASE_BINS - LONG_N / 2) * df;
        valid_[size_t(i)] = std::fabs(f) <= (0.5 - cfg.edge_guard) * fs;
    }
    estimate_floor(fe, cfg);
    r_.resize(NBASE);
    for (int i = 0; i < NBASE; i++)
        r_[size_t(i)] = valid_[size_t(i)] && fb_[size_t(i)] > 0.0f ? pb_[size_t(i)] / fb_[size_t(i)] : 1.0f;
    fs_floor_.resize(SPEC_N);
    for (int k = 0; k < SPEC_N; k++) {
        const int i = std::clamp(int((k + 0.5) * LONG_N / SPEC_N), 0, LONG_N - 1);
        fs_floor_[size_t(k)] = floor_f_[size_t(i)];
    }

    // continuous detection: the whole band against the floor ...
    std::vector<Region> regs;
    segment(fe, cfg, r_.data(), 0, NBASE - 1, 1.0f, regs);
    // ... and inside each wide region against floor x the region's own
    // plateau: a narrow signal on a wide one (a carrier on a DAB block, NFM
    // next to a broadband hump) is a signal of its own when it is narrow,
    // holds little of the region's power, sits off its centre (not an AM
    // carrier) and has no mirror image about it (not FSK tones)
    std::vector<Region> extra, subs;
    for (Region& g : regs) {
        const int W = g.b - g.a + 1;
        if (W < 64 || g.comb) continue;   // (a comb's lines are the signal itself)
        const int sub = std::max(8, W / 8);
        const int nsub = (W + sub - 1) / sub;
        std::vector<float> pl(static_cast<size_t>(nsub));
        for (int q = 0; q < nsub; q++) {
            const int a = g.a + q * sub, b = std::min(g.b, a + sub - 1);
            pl[size_t(q)] = quantile(r_.data() + a, size_t(b - a + 1), 0.5, scratch_);
        }
        auto plateau = [&](int i) {
            const float u = float(i - g.a) / float(sub) - 0.5f;
            int q0 = int(std::floor(u));
            float t = u - float(q0);
            if (q0 < 0) { q0 = 0; t = 0.0f; }
            if (q0 >= nsub - 1) { q0 = std::max(0, nsub - 2); t = nsub > 1 ? 1.0f : 0.0f; }
            const float a = pl[size_t(q0)], b = pl[size_t(std::min(q0 + 1, nsub - 1))];
            return std::max(1.0f, (1.0f - t) * a + t * b);
        };
        rr_.assign(r_.begin(), r_.end());
        for (int i = g.a; i <= g.b; i++) rr_[size_t(i)] = r_[size_t(i)] / plateau(i);
        segment(fe, cfg, rr_.data(), g.a, g.b, 2.0f, subs);
        double sw = 0.0, swi = 0.0;
        for (int i = g.a; i <= g.b; i++) {
            const double x = std::max(0.0, double(r_[size_t(i)]) - 1.0);
            sw += x;
            swi += x * i;
        }
        const double cg = sw > 0.0 ? swi / sw : 0.5 * (g.a + g.b);
        struct Peak {
            int u, v;
            double cp, pr, lev;
        };
        std::vector<Peak> cand;
        const int margin = std::max(2, W / 50);   // the plateau is poor at the region's ends
        for (const Region& p : subs) {
            if (p.b - p.a + 1 > W / 8 || p.a < g.a + margin || p.b > g.b - margin) continue;
            double pr = 0.0, lev = 0.0;
            for (int i = p.a; i <= p.b; i++) {
                pr += r_[size_t(i)] - plateau(i);
                lev += plateau(i);
            }
            lev /= double(p.b - p.a + 1);
            const double cp = 0.5 * (p.a + p.b);
            if (pr < 0.3 * g.power && std::fabs(cp - cg) > 0.1 * W) cand.push_back({p.a, p.b, cp, pr, lev});
        }
        for (size_t k = 0; k < cand.size(); k++) {
            bool mirrored = false;
            for (size_t m = 0; m < cand.size() && !mirrored; m++)
                mirrored = m != k && std::fabs((cand[k].cp - cg) + (cand[m].cp - cg)) <= std::max(2.0, 0.05 * W) &&
                           std::fabs(db10(cand[k].pr / std::max(cand[m].pr, 1e-9))) < 6.0f;
            if (mirrored) continue;
            Region n;
            n.a = std::max(g.a, cand[k].u - 1);
            n.b = std::min(g.b, cand[k].v + 1);
            n.power = cand[k].pr;
            n.floor_mult = float(cand[k].lev);   // measured against the plateau, not the floor
            extra.push_back(n);
            g.clip.push_back({n.a * BASE_BINS, n.b * BASE_BINS + BASE_BINS - 1});
            g.clip_level.push_back(float(cand[k].lev));
        }
    }

    std::vector<Cand> cont;
    for (const Region& g : regs) {
        Cand c;
        if (measure(fe, g, c, cfg)) {
            spectro_stats(fe, c, true);
            cont.push_back(c);
        }
    }
    for (const Region& n : extra) {
        Cand c;
        if (measure(fe, n, c, cfg) && c.det.snr_db >= 3.0f) {   // (against the plateau)
            spectro_stats(fe, c, true);
            cont.push_back(c);
        }
    }

    std::vector<Cand> bursts;
    burst_search(fe, cfg, cont, bursts);
    const double dfs = fs / SPEC_N;
    auto kof = [&](double f) { return int(std::floor((f + 0.5 * fs) / dfs)); };
    std::vector<float> en, eb;
    auto corr = [&](const Detection& a, const Detection& b) {
        band_series(fe, kof(a.offset_hz - 0.5 * a.bw_hz), kof(a.offset_hz + 0.5 * a.bw_hz), en, nullptr);
        band_series(fe, kof(b.offset_hz - 0.5 * b.bw_hz), kof(b.offset_hz + 0.5 * b.bw_hz), eb, nullptr);
        double ma = 0, mb = 0;
        for (size_t t = 0; t < en.size(); t++) { ma += en[t]; mb += eb[t]; }
        ma /= double(std::max<size_t>(1, en.size()));
        mb /= double(std::max<size_t>(1, eb.size()));
        double sab = 0, saa = 0, sbb = 0;
        for (size_t t = 0; t < en.size(); t++) {
            const double x = en[t] - ma, y = eb[t] - mb;
            sab += x * y; saa += x * x; sbb += y * y;
        }
        return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0.0;
    };
    // neighbouring bursts that come and go together are one (the two lobes
    // of a Manchester / PPM pulse train around its spectral null)
    if (bursts.size() > 1 && fe.nframes >= 16) {
        std::sort(bursts.begin(), bursts.end(), [](const Cand& a, const Cand& b) { return a.det.offset_hz < b.det.offset_hz; });
        for (size_t i = 0; i + 1 < bursts.size();) {
            Detection& A = bursts[i].det;
            const Detection& B = bursts[i + 1].det;
            const double gap = (B.offset_hz - 0.5 * B.bw_hz) - (A.offset_hz + 0.5 * A.bw_hz);
            if (gap <= std::max(A.bw_hz, B.bw_hz) && corr(A, B) > 0.5) {
                const double lo = std::min(A.offset_hz - 0.5 * A.bw_hz, B.offset_hz - 0.5 * B.bw_hz);
                const double hi = std::max(A.offset_hz + 0.5 * A.bw_hz, B.offset_hz + 0.5 * B.bw_hz);
                const double wa = A.bw_hz * std::pow(10.0, A.snr_db / 10.0), wb = B.bw_hz * std::pow(10.0, B.snr_db / 10.0);
                A.offset_hz = (A.offset_hz * wa + B.offset_hz * wb) / std::max(wa + wb, 1e-30);
                A.offset_hz = std::clamp(A.offset_hz, lo, hi);
                A.bw_hz = hi - lo;
                A.snr_db = std::max(A.snr_db, B.snr_db);
                A.peak_db = std::max(A.peak_db, B.peak_db);
                A.duty = std::max(A.duty, B.duty);
                bursts[i].centre_bin = int(std::lround(A.offset_hz / df));
                bursts.erase(bursts.begin() + long(i) + 1);
            } else {
                i++;
            }
        }
    }
    // a bursty narrow detection inside a burst hump, on and off with it, is
    // part of it (the carrier line and sidelines of a pulse train)
    if (!bursts.empty() && fe.nframes >= 16) {
        for (size_t i = 0; i < cont.size();) {
            const Detection& N = cont[i].det;
            bool fold = false;
            for (const Cand& B : bursts) {
                if (fold || !N.bursty || N.bw_hz > 0.25 * B.det.bw_hz ||
                    std::fabs(N.offset_hz - B.det.offset_hz) > 0.5 * B.det.bw_hz)
                    continue;
                fold = corr(N, B.det) > 0.5;
            }
            if (fold) cont.erase(cont.begin() + long(i));
            else i++;
        }
    }
    // a burst hump around a stronger continuous signal, at most ten times its
    // width, is that signal seen through the short frames (its masked band
    // bridged) plus its skirt or a faint pedestal beside it - not a signal of
    // its own (a 4-FSK channel's modulation skirt found as a 40 kHz "burst")
    for (size_t i = 0; i < bursts.size();) {
        const Detection& B = bursts[i].det;
        bool skirt = false;
        for (const Cand& n : cont) {
            const Detection& N = n.det;
            skirt = skirt || (!N.bursty && N.peak_db >= B.peak_db + 3.0f && B.bw_hz <= 10.0 * N.bw_hz &&
                              std::fabs(N.offset_hz - B.offset_hz) < 0.5 * B.bw_hz);
        }
        if (skirt) bursts.erase(bursts.begin() + long(i));
        else i++;
    }
    out = cont;
    out.insert(out.end(), bursts.begin(), bursts.end());
    {
        // A wideband signal over most of the band - found (>= 0.6 fs) or band-filling -
        // shows lines over its own level (DVB-T / ISDB-T / ATSC 3.0 continual pilots,
        // a TV signal's line-rate harmonics) and faint bumps of its own spectrum over
        // the "floor" the detector took from it: those are part of it, not signals.
        // Inside its band (a band-filling one: anywhere inside the guards - its edge
        // pilots lie outside its 98 % band) narrow continuous detections go when
        // there are several of them or they are weak (< 6 dB; a lone strong carrier
        // on top stays), and continuous wider bumps under 3 dB go.
        const double lim = (0.5 - cfg.edge_guard) * fs;
        auto is_line = [&](const Detection& d) { return d.bw_hz <= 3000.0 && !d.bursty && !d.fills_band && std::fabs(d.offset_hz) < lim; };
        bool have_wide = false;
        for (const Cand& f : out) have_wide = have_wide || f.det.bw_hz >= 0.6 * fs;
        Cand c;
        bool fill = cfg.band_fill_test && !have_wide && band_fill(fe, cfg, out, c, 0);
        bool comb_tv = false;   // filled by a regular line comb (a TV signal): all its narrow pieces are it
        if (cfg.band_fill_test && !have_wide) {
            // a comb of >= 10 lines spread over half the band or more and nothing wide
            // under it: the pilots of an OFDM signal whose cyclic prefix is too short
            // (or whose lines were the evidence: a TV signal's harmonics) for the test
            int nl = 0;
            double a = 1e30, b = -1e30;
            std::vector<double> lf;
            for (const Cand& f : out)
                if (is_line(f.det) && f.det.snr_db >= 6.0f) {
                    nl++;
                    a = std::min(a, f.det.offset_hz);
                    b = std::max(b, f.det.offset_hz);
                    lf.push_back(f.det.offset_hz);
                }
            // ... or >= 10 lines on one regular spacing (>= 5 kHz; 8 and 40 % of the gaps
            // within 10 % of 1-4 spacings): a TV signal's line-rate comb
            bool regular = false;
            if (nl >= 10) {
                std::sort(lf.begin(), lf.end());
                std::vector<double> d;
                for (size_t k = 1; k < lf.size(); k++)
                    if (lf[k] - lf[k - 1] > 2000.0) d.push_back(lf[k] - lf[k - 1]);
                if (d.size() >= 8) {
                    std::vector<double> ds(d);
                    std::nth_element(ds.begin(), ds.begin() + long(ds.size() / 2), ds.end());
                    const double sp = ds[ds.size() / 2];
                    int ok = 0;
                    for (double g : d) {
                        const double m = std::round(g / sp);
                        ok += m >= 1 && m <= 4 && std::fabs(g - m * sp) <= 0.1 * sp;
                    }
                    regular = sp >= 5000.0 && ok >= std::max(8.0, 0.4 * double(d.size()));
                }
            }
            if (std::getenv("SIGCLASS_DEBUG_FILL")) std::fprintf(stderr, "band_fill: comb of %d lines over %.0f Hz, regular %d\n", nl, b - a, int(regular));
            if (!fill && nl >= 10 && (b - a >= lim || regular)) fill = band_fill(fe, cfg, out, c, regular ? 2 : 1);
            comb_tv = fill && regular;
            // ... or >= 16 bursty pieces <= 60 kHz wide spread over >= 0.6 fs: a lightly
            // loaded OFDM carrier (LTE / NR resource blocks come and go) - the test then
            // keeps the pieces' power (they are the signal; masking them left nothing)
            if (!fill) {
                int nb = 0;
                double ba = 1e30, bb = -1e30;
                std::vector<Cand> keep;
                for (const Cand& f : out) {
                    if (f.det.bursty && f.det.bw_hz <= 60e3 && !f.det.fills_band) {
                        nb++;
                        ba = std::min(ba, f.det.offset_hz);
                        bb = std::max(bb, f.det.offset_hz);
                    } else {
                        keep.push_back(f);
                    }
                }
                if (std::getenv("SIGCLASS_DEBUG_FILL")) std::fprintf(stderr, "band_fill: %d bursty pieces over %.0f Hz\n", nb, bb - ba);
                if (nb >= 16 && bb - ba >= 0.6 * fs) fill = band_fill(fe, cfg, keep, c, 3);
            }
        }
        if (fill) out.push_back(c);
        for (const Cand& w : std::vector<Cand>(out)) {
            if (!(w.det.fills_band || w.det.bw_hz >= 0.6 * fs)) continue;
            const double lo = w.det.fills_band ? -lim : w.det.offset_hz - 0.5 * w.det.bw_hz;
            const double hi = w.det.fills_band ? lim : w.det.offset_hz + 0.5 * w.det.bw_hz;
            auto inside = [&](const Detection& d) { return d.offset_hz > lo && d.offset_hz < hi; };
            int nl = 0;
            for (const Cand& f : out) nl += f.det.bw_hz <= 3000.0 && !f.det.fills_band && (w.det.fills_band || !f.det.bursty) && inside(f.det) ? 1 : 0;
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [&](const Cand& f) {
                                         const Detection& d = f.det;
                                         if (d.fills_band || d.bw_hz >= 0.6 * fs || !inside(d)) return false;
                                         // a band-filling signal's own pilots fade with its echoes (bursty)
                                         // and its echoes ripple its spectrum by several dB: under it,
                                         // lines (also bursty ones) and anything below 10 dB go
                                         const bool line = d.bw_hz <= 3000.0 && std::fabs(d.offset_hz) < lim;
                                         // (bursty pieces under it too: a TV signal's video bursts)
                                         if (w.det.fills_band && comb_tv) return d.bw_hz < 50e3;
                                         if (w.det.fills_band) return (line && nl >= 3) || d.snr_db < 10.0f || d.bursty;
                                         if (d.bursty) return false;
                                         if (is_line(d)) return nl >= 3 || d.snr_db < 6.0f;
                                         return d.snr_db < 3.0f;
                                     }),
                      out.end());
        }
    }
    // pieces of one burst signal: a bursty detection inside a burst signal at least
    // four times wider and about as strong (its max-hold within 3 dB or more) is
    // part of it (ADS-B / DME / UAT pulse spectra found piecewise at narrower box
    // widths); a strong narrow burst inside a faint wide hump stays its own
    for (size_t i = 0; i < out.size();) {
        const Detection& d = out[i].det;
        bool part = false;
        if (d.bursty && !d.fills_band) {
            for (size_t j = 0; j < out.size() && !part; j++) {
                const Detection& w = out[j].det;
                part = j != i && w.bursty && !w.fills_band && w.bw_hz >= 4.0 * d.bw_hz &&
                       std::fabs(d.offset_hz - w.offset_hz) <= 0.5 * w.bw_hz && w.peak_db >= d.peak_db - 3.0f;
            }
        }
        if (part) out.erase(out.begin() + long(i));
        else i++;
    }
    // the splatter of a strong burst signal (an OOK device's keying sidebands, a
    // DME pulse spectrum's lobes): a detection >= 6 dB weaker (max-hold), at most
    // half as wide, within three widths (30 - 150 kHz) of the burst's band, whose power
    // comes and goes with it (the short spectrogram), is part of it
    if (fe.nframes >= 16) {
        std::vector<uint8_t> drop(out.size(), 0);
        for (size_t j = 0; j < out.size(); j++) {
            const Detection& s = out[j].det;
            if (!s.bursty || s.fills_band) continue;
            const double reach = 0.5 * s.bw_hz + std::min(std::max(3.0 * s.bw_hz, 30e3), 150e3);
            for (size_t i = 0; i < out.size(); i++) {
                const Detection& w = out[i].det;
                if (i == j || drop[i] || w.fills_band || w.bw_hz > 0.5 * s.bw_hz || w.peak_db > s.peak_db - 6.0f) continue;
                if (std::fabs(w.offset_hz - s.offset_hz) > reach) continue;
                // (a faint "continuous" one 20 dB under it: its average over the snapshot -
                // too faint for the short frames' correlation to show)
                if ((!w.bursty && w.snr_db < 1.0f && s.peak_db - w.peak_db >= 20.0f) || corr(w, s) > 0.6) drop[i] = 1;
            }
        }
        size_t k = 0;
        for (size_t i = 0; i < out.size(); i++)
            if (!drop[i]) out[k++] = out[i];
        out.resize(k);
    }
    // discrete lines and bursty specks in the skirt of a much stronger signal
    // (a periodic signal's sidelines, switching splatter) are part of it:
    // narrow (<= 1 kHz) or bursty, at most a fifth of its width, >= 25 dB
    // below it in total power (density x width), outside its band within
    // half its width (at most 60 kHz) of its edge
    auto total_db = [](const Detection& d) { return d.snr_db + db10(std::max(d.bw_hz, 1.0)); };
    for (size_t i = 0; i < out.size();) {
        const Detection& w = out[i].det;
        bool skirt = false;
        if ((w.bw_hz <= 1000.0 || w.bursty) && !w.fills_band) {
            for (size_t j = 0; j < out.size() && !skirt; j++) {
                const Detection& s = out[j].det;
                if (j == i || s.fills_band || w.bw_hz > 0.2 * s.bw_hz) continue;
                // outside its band, at most half its width (<= 60 kHz) beyond the edge
                const double edge = std::fabs(w.offset_hz - s.offset_hz) - 0.5 * s.bw_hz;
                skirt = edge >= 0.0 && edge <= std::min(0.5 * s.bw_hz, 60e3) && total_db(s) - total_db(w) >= 25.0f;
            }
        }
        if (skirt) out.erase(out.begin() + long(i));
        else i++;
    }
    // the IQ image of a strong signal (the tuner's gain / phase imbalance mirrors it to -f,
    // 25-40 dB down): a detection whose mirrored band (-f) holds >= 15 dB more power than
    // its own band (the fine PSD: also a strong signal's pilot line, not only its mean) and
    // that lies >= 20 kHz from the centre may be an image - not a transmitter (an image
    // carries the signal's modulation: the model called it by the signal's class). The
    // image is the conjugate copy, so the analyzer confirms it by a projection before
    // dropping it (a transmitter of its own near the mirror frequency - a control channel
    // 1.5 kHz off the mirror of a voice channel 28 dB stronger - is independent)
    {
        const double df = fs / LONG_N;
        auto band_pow = [&](double lo, double hi) {
            const int a = std::clamp(int(std::floor(lo / df)) + LONG_N / 2, 0, LONG_N - 1);
            const int b = std::clamp(int(std::ceil(hi / df)) + LONG_N / 2, 0, LONG_N - 1);
            double p = 0.0;
            for (int k = a; k <= b; k++) p += fe.psd[size_t(k)];
            return p;
        };
        for (Cand& c : out) {
            const Detection& w = c.det;
            if (w.fills_band || std::fabs(w.offset_hz) < std::max(w.bw_hz, 20e3)) continue;
            const double h = 0.5 * std::max(w.bw_hz, 2.0 * df);
            const double pw = band_pow(w.offset_hz - h, w.offset_hz + h);
            const double pm = band_pow(-w.offset_hz - h, -w.offset_hz + h);
            c.maybe_image = pm >= 31.6 * pw && pw > 0.0;
        }
    }
    // strongest first - a band-filling detection (peak 0 dB by definition) before all: the
    // max_candidates cut must not drop it
    std::stable_sort(out.begin(), out.end(), [](const Cand& a, const Cand& b) {
        if (a.det.fills_band != b.det.fills_band) return a.det.fills_band;
        return a.det.peak_db > b.det.peak_db;
    });
    // (image candidates do not count: the analyzer drops the confirmed ones and then cuts)
    {
        size_t k = 0;
        int real = 0;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i].maybe_image || real < cfg.max_candidates) {
                real += out[i].maybe_image ? 0 : 1;
                out[k++] = out[i];
            }
        out.resize(k);
    }
    for (Cand& c : out) {
        if (!c.det.fills_band) c.det.view = Analyzer::view_for_bandwidth(c.det.bw_hz, fs, cfg.view_occupancy);
        c.centre_bin = std::clamp(c.centre_bin, -LONG_N / 2, LONG_N / 2 - 1);
    }
}

}  // namespace sigclass::detail
