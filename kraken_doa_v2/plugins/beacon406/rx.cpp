#include "rx.hpp"

#include "kraken_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace b406 {

using kp::cf;
using cd = std::complex<double>;

namespace {

constexpr size_t RING = 1 << 18;          // 2.6 s of input
constexpr double START_DB = 10;           // a new carrier: its bin over the floor
constexpr double CONT_DB = 7;             // ... followed while the max within +-CONT_BINS stays over this
constexpr int CONT_BINS = 4;              // +-391 Hz: the biphase sidebands around the carrier
constexpr int MISS_FRAMES = 6;            // 31 ms under CONT_DB = the burst ended
constexpr double MIN_BURST_S = 0.12, MAX_BURST_S = 1.0;   // weak bursts lose the modulated part
constexpr double SEG_S = 0.62;            // demodulated: onset - 50 ms .. onset + SEG_S (a long burst is 0.52 s)
constexpr double STEADY_S = 1.2;          // longer: not a beacon burst
constexpr int SHADOW_BINS = 25;           // a new carrier this close to a >= SHADOW_DB stronger one = its spur
constexpr double SHADOW_DB = 20;
constexpr int LP_TAPS = 401;
constexpr double LP_CUT = 1500;           // channel filter: +-1.5 kHz (channels are 3 kHz apart)
constexpr double PHASE_DEV = 1.1;         // rad
// second generation (T.018 2.2.3): OQPSK, 38.4 kchip/s per arm -> main lobe +-38.4 kHz
constexpr double SGB_HALF_HZ = 30000;
constexpr int SGB_SUB = 8;                // sub-bands for the flatness test
constexpr double SGB_ON = 1.30, SGB_OFF = 1.12;   // band power over its floor (4-frame mean)
constexpr double SGB_MIN_S = 0.9, SGB_MAX_S = 1.15;   // T.018 2.2.2: 1000 +-1 ms
constexpr double SGB_MIN_EXCESS = 0.3;    // the band's mean rise over the burst (-5 dB): weaker ones were live noise
constexpr double SGB_REF_HZ = 40000;      // reference bins: this far from 406.05 MHz (the VFO's edges)
constexpr double SGB_OVER = 3;            // the band's rise >= 3x the edges' rise

const int FS_SYNC_NORMAL[9] = {0, 0, 0, 1, 0, 1, 1, 1, 1};
const int FS_SYNC_TEST[9] = {0, 1, 1, 0, 1, 0, 0, 0, 0};

double wrap(double a) { return std::remainder(a, 2 * M_PI); }

// cumulative sum of r with sample j covering [j, j + 1): integral up to t
struct Cum {
    std::vector<cd> cs;   // cs[j] = r[0] + .. + r[j - 1]
    const std::vector<cd>* r = nullptr;
    void build(const std::vector<cd>& x) {
        r = &x;
        cs.assign(x.size() + 1, cd{});
        for (size_t j = 0; j < x.size(); j++) cs[j + 1] = cs[j] + x[j];
    }
    cd at(double t) const {
        if (t <= 0) return cs[0];
        const double n = static_cast<double>(r->size());
        if (t >= n) return cs.back();
        const size_t j = static_cast<size_t>(t);
        return cs[j] + (t - static_cast<double>(j)) * (*r)[j];
    }
    cd integ(double a, double b) const { return at(b) - at(a); }
};

}  // namespace

Receiver::Receiver() {
    ring_.assign(RING, cf{});
    win_ = static_cast<float*>(fftwf_malloc(sizeof(float) * NFFT));
    fin_ = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * NFFT));
    fout_ = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * NFFT));
    plan_ = fftwf_plan_dft_1d(NFFT, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
    for (int k = 0; k < NFFT; k++) win_[k] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * k / NFFT));
    lp_ = kp::fir_lowpass(LP_TAPS, static_cast<float>(LP_CUT), static_cast<float>(FS));
    reset();
}

Receiver::~Receiver() {
    fftwf_destroy_plan(plan_);
    fftwf_free(win_);
    fftwf_free(fin_);
    fftwf_free(fout_);
}

void Receiver::reset() {
    std::fill(ring_.begin(), ring_.end(), cf{});
    abs_ = 0;
    frame_ = 0;
    pw_.assign(NFFT, 0.0f);
    nf_.assign(NFFT, 0.0f);
    claimed_.assign(NFFT, 0);
    claimed_conf_.assign(NFFT, 0);
    cands_.clear();
    jobs_.clear();
    band_floor_ = out_floor_ = 0;
    band_init_ = 0;
    wide_on_ = false;
    wide_miss_ = 0;
    band_hist_.clear();
    out_hist_.clear();
}

kp::cf Receiver::at(uint64_t i) const { return ring_[i & (RING - 1)]; }

void Receiver::process(const cf* x, size_t n, std::vector<Burst>* out, std::vector<WideBurst>* wide) {
    for (size_t i = 0; i < n; i++) {
        ring_[abs_ & (RING - 1)] = x[i];
        abs_++;
        if (abs_ >= static_cast<uint64_t>(NFFT) && (abs_ - NFFT) % HOP == 0) frame(out, wide);
    }
}

void Receiver::frame(std::vector<Burst>* out, std::vector<WideBurst>* wide) {
    const uint64_t e = abs_;
    for (int k = 0; k < NFFT; k++) {
        const cf v = at(e - NFFT + static_cast<uint64_t>(k)) * win_[k];
        fin_[k][0] = v.real();
        fin_[k][1] = v.imag();
    }
    fftwf_execute(plan_);
    for (int k = 0; k < NFFT; k++) {
        const int idx = (k + NFFT / 2) % NFFT;   // bin b at index b + NFFT/2
        pw_[static_cast<size_t>(idx)] = fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];
    }
    // floor per bin: mean of the first frames, then a gated mean (~1.7 s)
    if (frame_ < 32) {
        const float a = 1.0f / static_cast<float>(frame_ + 1);
        for (int k = 0; k < NFFT; k++) nf_[k] += a * (pw_[k] - nf_[k]);
    }
    const float start = static_cast<float>(std::pow(10.0, START_DB / 10));
    const float cont = static_cast<float>(std::pow(10.0, CONT_DB / 10));
    auto ratio = [&](int idx) { return nf_[idx] > 0 ? pw_[idx] / nf_[idx] : 0.0f; };

    std::fill(claimed_.begin(), claimed_.end(), 0);
    std::vector<Cand> done;
    for (auto& c : cands_) {
        const int cb = static_cast<int>(std::lround(c.bin));
        float best = 0;
        for (int k = std::max(1, cb - CONT_BINS); k <= std::min(NFFT - 2, cb + CONT_BINS); k++) best = std::max(best, ratio(k));
        if (best >= start) c.hits++;
        if (best >= cont) {
            c.f1 = frame_;
            c.miss = 0;
            c.peak = std::max(c.peak, best);
            // the carrier's bin: the strongest frame of its first 150 ms (the unmodulated part)
            if (frame_ - c.f0 < 29) {
                int pb = cb;
                for (int k = std::max(1, cb - 3); k <= std::min(NFFT - 2, cb + 3); k++)
                    if (pw_[k] > pw_[pb]) pb = k;
                if (pw_[pb] > c.bestp && pb > 0 && pb < NFFT - 1) {
                    const double l = std::log(std::max(pw_[pb - 1], 1e-30f)), m = std::log(std::max(pw_[pb], 1e-30f)),
                                 r = std::log(std::max(pw_[pb + 1], 1e-30f));
                    const double den = l - 2 * m + r;
                    c.bin = pb + (den < 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0);
                    c.bestp = pw_[pb];
                }
            }
        } else {
            c.miss++;
        }
        if (static_cast<double>(frame_ - c.f0) * HOP / FS > STEADY_S) c.steady = true;
        for (int k = std::max(0, cb - CONT_BINS - 3); k <= std::min(NFFT - 1, cb + CONT_BINS + 3); k++) claimed_[k] = 1;
    }
    for (auto it = cands_.begin(); it != cands_.end();) {
        if (frame_ - it->f0 == 3 && it->hits < 3) {   // not confirmed: noise
            it = cands_.erase(it);
            continue;
        }
        if (it->miss > MISS_FRAMES) {
            done.push_back(*it);
            it = cands_.erase(it);
        } else {
            ++it;
        }
    }
    // new carriers (local maxima over START_DB, strongest first, not next to a much stronger one)
    std::vector<std::pair<float, int>> peaks;
    for (int k = 2; k < NFFT - 2; k++)
        if (!claimed_[k] && ratio(k) >= start && pw_[k] >= pw_[k - 1] && pw_[k] >= pw_[k + 1]) peaks.push_back({ratio(k), k});
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [rk, k] : peaks) {
        if (claimed_[k] || cands_.size() >= 16) continue;
        // a spur of a stronger carrier: next to one far stronger, or starting during its burst
        // (the biphase modulation's harmonics, 400 Hz x odd)
        bool shadow = false;
        for (const auto& c : cands_)
            if (std::fabs(c.bin - k) <= SHADOW_BINS &&
                (c.peak > rk * std::pow(10.0, SHADOW_DB / 10) || (c.peak > rk * 4 && frame_ - c.f0 >= 8)))
                shadow = true;
        Cand c;
        c.f0 = c.f1 = frame_;
        const double l = std::log(std::max(pw_[k - 1], 1e-30f)), m = std::log(std::max(pw_[k], 1e-30f)),
                     r = std::log(std::max(pw_[k + 1], 1e-30f));
        const double den = l - 2 * m + r;
        c.bin = k + (den < 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0);
        c.bestp = pw_[k];
        c.peak = rk;
        c.hits = 1;
        c.steady = shadow;   // never demodulated
        cands_.push_back(c);
        for (int j = std::max(0, k - CONT_BINS - 3); j <= std::min(NFFT - 1, k + CONT_BINS + 3); j++) claimed_[j] = 1;
    }
    // floor: bins that are no carrier's, gated at 3x
    if (frame_ >= 32)
        for (int k = 0; k < NFFT; k++) {
            if (claimed_[k]) continue;
            const float r = ratio(k);
            nf_[k] += (r < 3 ? 0.004f : 0.0002f) * (pw_[k] - nf_[k]);
        }
    // the SGB band without the confirmed carriers' bins (excluding every new
    // candidate would drop exactly the high bins of a spread-spectrum burst)
    std::fill(claimed_conf_.begin(), claimed_conf_.end(), 0);
    for (const auto& c : cands_) {
        if (frame_ - c.f0 < 3 || c.hits < 3) continue;
        const int cb = static_cast<int>(std::lround(c.bin));
        for (int k = std::max(0, cb - CONT_BINS - 3); k <= std::min(NFFT - 1, cb + CONT_BINS + 3); k++) claimed_conf_[k] = 1;
    }
    wide_frame(wide);
    frame_++;
    for (const auto& c : done) {
        const double dur = static_cast<double>(c.f1 - c.f0) * HOP / FS + 0.5 * NFFT / FS;
        if (verbose) {
            char s[160];
            snprintf(s, sizeof s, "carrier %+.1f Hz: frames %lld..%lld (%.3f s), peak %.1f dB%s", (c.bin - NFFT / 2) * FS / NFFT,
                     static_cast<long long>(c.f0), static_cast<long long>(c.f1), dur, 10 * std::log10(std::max(1e-9f, c.peak)),
                     c.steady ? ", spur / steady" : "");
            log.push_back(s);
        }
        if (c.steady || dur < MIN_BURST_S || dur > MAX_BURST_S) continue;
        const double onset = static_cast<double>(NFFT + static_cast<uint64_t>(c.f0) * HOP) - 0.6 * NFFT;
        jobs_.push_back({static_cast<uint64_t>(std::max(0.0, onset + SEG_S * FS)), c});
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (abs_ < it->first) {
            ++it;
            continue;
        }
        Burst b;
        carriers_++;
        if (demod(it->second, &b)) out->push_back(std::move(b));
        it = jobs_.erase(it);
    }
}

void Receiver::wide_frame(std::vector<WideBurst>* wide) {
    // the SGB band (+-30 kHz of 406.05 MHz) and, as the reference, the VFO's edges
    // >= 40 kHz from it: a burst raises the band, a change of the receiver's own
    // level (gain, interference, calibration) raises both
    const double bin_hz = FS / NFFT;
    const int k0 = std::max(1, static_cast<int>(std::lround((sgb_hz_ - SGB_HALF_HZ) / bin_hz)) + NFFT / 2);
    const int k1 = std::min(NFFT - 2, static_cast<int>(std::lround((sgb_hz_ + SGB_HALF_HZ) / bin_hz)) + NFFT / 2);
    if (k1 - k0 < 2 * SGB_SUB) return;
    const int edge = static_cast<int>(SGB_REF_HZ / bin_hz), lim = static_cast<int>(0.47 * NFFT);
    // medians: a few narrowband carriers (first-generation beacons) don't move them,
    // a spread-spectrum burst moves every bin
    const int kc = static_cast<int>(std::lround(sgb_hz_ / bin_hz)) + NFFT / 2;
    std::vector<float>& vb = med_b_;
    std::vector<float>& ve = med_e_;
    vb.clear();
    ve.clear();
    for (int k = 1; k < NFFT - 1; k++) {
        if (k == NFFT / 2) continue;   // DC
        if (k >= k0 && k <= k1) vb.push_back(pw_[k]);
        else if (std::abs(k - kc) >= edge && std::abs(k - NFFT / 2) <= lim) ve.push_back(pw_[k]);
    }
    if (vb.size() < 16 || ve.size() < 16) return;
    auto median = [](std::vector<float>& v, size_t a, size_t b) {
        std::nth_element(v.begin() + static_cast<long>(a), v.begin() + static_cast<long>((a + b) / 2), v.begin() + static_cast<long>(b));
        return static_cast<double>(v[(a + b) / 2]);
    };
    // per sub-band medians first (nth_element reorders within each range only)
    std::vector<double> subm(SGB_SUB);
    const size_t wsub = vb.size() / SGB_SUB;
    for (int q = 0; q < SGB_SUB; q++) subm[static_cast<size_t>(q)] = median(vb, q * wsub, (q + 1) * wsub);
    const double p = median(vb, 0, vb.size()), f = 1, po = median(ve, 0, ve.size()), fo = 1;
    if (f <= 0 || fo <= 0) return;
    band_hist_.push_back(static_cast<float>(p));
    out_hist_.push_back(static_cast<float>(po));
    if (band_hist_.size() > 4) band_hist_.erase(band_hist_.begin());
    if (out_hist_.size() > 4) out_hist_.erase(out_hist_.begin());
    double pm = 0, om = 0;
    for (float v : band_hist_) pm += v;
    for (float v : out_hist_) om += v;
    pm /= static_cast<double>(band_hist_.size());
    om /= static_cast<double>(out_hist_.size());
    if (band_init_ < 32) {
        ++band_init_;
        band_floor_ += (pm - band_floor_) / band_init_;
        out_floor_ += (om - out_floor_) / band_init_;
        return;
    }
    const double ein = band_floor_ > 0 ? pm / band_floor_ - 1 : 0, eout = out_floor_ > 0 ? om / out_floor_ - 1 : 0;
    const bool lit = ein > SGB_ON - 1 && ein > SGB_OVER * std::max(0.0, eout) + 0.1;
    if (getenv("B406_WIDE_TRACE") && frame_ % 20 == 0) {
        char t[120];
        snprintf(t, sizeof t, "wide frame %lld: band %+.2f edges %+.2f%s", static_cast<long long>(frame_), ein, eout, wide_on_ ? " (on)" : "");
        log.push_back(t);
    }
    if (!wide_on_) {
        if (lit) {
            wide_on_ = true;
            wide_f0_ = wide_f1_ = frame_;
            wide_miss_ = 0;
            wide_sub_.assign(SGB_SUB, 0.0);
            wide_excess_ = wide_out_ = 0;
        } else {
            band_floor_ += (ein < 0.15 ? 0.01 : 0.0005) * (pm - band_floor_);
            out_floor_ += (eout < 0.15 ? 0.01 : 0.0005) * (om - out_floor_);
        }
    }
    if (!wide_on_) return;
    if (ein > SGB_OFF - 1 && ein > eout) {
        wide_f1_ = frame_;
        wide_miss_ = 0;
        for (int q = 0; q < SGB_SUB; q++) wide_sub_[static_cast<size_t>(q)] += subm[static_cast<size_t>(q)] / band_floor_ - 1;
        wide_excess_ += ein;
        wide_out_ += eout;
    } else if (++wide_miss_ > 6) {
        wide_on_ = false;
        const double dur = static_cast<double>(wide_f1_ - wide_f0_) * HOP / FS + 0.5 * NFFT / FS;
        double mean = 0, mx = 0;
        int nlit = 0;
        for (double v : wide_sub_) mean += v / SGB_SUB;
        for (double v : wide_sub_) {
            mx = std::max(mx, v);
            if (v > 0.3 * mean) nlit++;
        }
        const int nfr = static_cast<int>(wide_f1_ - wide_f0_ + 1);
        const bool flat = mean > 0 && nlit >= SGB_SUB - 2 && mx < 4 * mean;
        const bool above = wide_excess_ > SGB_OVER * std::max(0.0, wide_out_) && wide_excess_ / nfr >= SGB_MIN_EXCESS;
        if (dur >= SGB_MIN_S && dur <= SGB_MAX_S && flat && above) {
            WideBurst wb;
            const uint64_t e0 = NFFT + static_cast<uint64_t>(wide_f0_) * HOP, e1 = NFFT + static_cast<uint64_t>(wide_f1_) * HOP;
            wb.t0 = static_cast<double>(e0) - NFFT / 2.0;
            wb.t1 = static_cast<double>(e1) - NFFT / 2.0;
            wb.centre_hz = sgb_hz_;
            wb.bw_hz = 2 * SGB_HALF_HZ;
            wb.snr_db = 10 * std::log10(std::max(1e-9, wide_excess_ / nfr));
            wide->push_back(wb);
        } else if (verbose) {
            char b[200];
            snprintf(b, sizeof b, "wide burst rejected: %.2f s, %d/%d sub-bands lit, max/mean %.1f, band +%.2f vs edges %+.2f", dur,
                     nlit, SGB_SUB, mean > 0 ? mx / mean : 0.0, wide_excess_ / nfr, wide_out_ / nfr);
            log.push_back(b);
        }
        band_hist_.clear();
        out_hist_.clear();
    }
}

bool Receiver::demod(const Cand& c, Burst* b) {
    const double bin_hz = FS / NFFT;
    const double fc = (c.bin - NFFT / 2) * bin_hz;
    // the carrier's start: its first frame's window held enough of it
    const double onset = static_cast<double>(NFFT + static_cast<uint64_t>(c.f0) * HOP) - 0.6 * NFFT;
    const double end = onset + SEG_S * FS;
    const double oldest = static_cast<double>(abs_) - static_cast<double>(RING) + 1;
    const int64_t s0 = static_cast<int64_t>(std::max(oldest, onset - 0.05 * FS));
    const int64_t s1 = static_cast<int64_t>(std::min(static_cast<double>(abs_), end + 0.03 * FS));
    if (s1 - s0 < static_cast<int64_t>(0.3 * FS)) return false;
    // mix to 0 Hz, low-pass, decimate to FSD
    const size_t len = static_cast<size_t>(s1 - s0);
    std::vector<cf> mx(len);
    const double w = -2 * M_PI * fc / FS;
    for (size_t j = 0; j < len; j++) {
        const double ph = std::remainder(w * static_cast<double>(s0 + static_cast<int64_t>(j)), 2 * M_PI);
        mx[j] = at(static_cast<uint64_t>(s0) + j) * cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    if (len < static_cast<size_t>(LP_TAPS) + DEC) return false;
    const size_t nout = (len - LP_TAPS) / DEC + 1;
    std::vector<cd> y(nout);
    for (size_t n = 0; n < nout; n++) {
        cf acc{};
        const cf* p = &mx[n * DEC];
        for (int k = 0; k < LP_TAPS; k++) acc += p[k] * lp_[static_cast<size_t>(k)];
        y[n] = cd(acc.real(), acc.imag());
    }
    const double base = static_cast<double>(s0) + (LP_TAPS - 1) / 2.0;   // input index of y[0]
    const double on_n = (onset - base) / DEC;
    const double end_n = (end - base) / DEC;
    // carrier frequency: the burst's strongest line (the 160 ms carrier + the residual carrier)
    double df = 0;
    {
        const int a0 = std::max(0, static_cast<int>(on_n)), a1 = std::min(static_cast<int>(nout), static_cast<int>(end_n));
        const int NF = 16384;
        if (a1 - a0 < 1000 || a1 - a0 > NF) return false;
        std::vector<cf> fi(NF, cf{}), fo(NF);
        for (int n = a0; n < a1; n++) fi[static_cast<size_t>(n - a0)] = cf(static_cast<float>(y[static_cast<size_t>(n)].real()), static_cast<float>(y[static_cast<size_t>(n)].imag()));
        fftwf_plan p = fftwf_plan_dft_1d(NF, reinterpret_cast<fftwf_complex*>(fi.data()), reinterpret_cast<fftwf_complex*>(fo.data()), FFTW_FORWARD, FFTW_ESTIMATE);
        fftwf_execute(p);
        fftwf_destroy_plan(p);
        const int span = static_cast<int>(250.0 / (FSD / NF));
        int bk = 0;
        float bp = -1;
        for (int k = -span; k <= span; k++) {
            const float v = std::norm(fo[static_cast<size_t>((k + NF) % NF)]);
            if (v > bp) {
                bp = v;
                bk = k;
            }
        }
        const double l = std::log(std::max(1e-30f, std::norm(fo[static_cast<size_t>((bk - 1 + NF) % NF)]))), m0 = std::log(std::max(1e-30f, bp)),
                     r0 = std::log(std::max(1e-30f, std::norm(fo[static_cast<size_t>((bk + 1 + NF) % NF)])));
        const double den = l - 2 * m0 + r0;
        df = (bk + (den < 0 ? std::clamp(0.5 * (l - r0) / den, -0.5, 0.5) : 0.0)) * FSD / NF;
    }
    std::vector<cd> wv(nout);
    for (size_t n = 0; n < nout; n++) {
        const double ph = -2 * M_PI * df * static_cast<double>(n) / FSD;
        wv[n] = y[n] * cd(std::cos(ph), std::sin(ph));
    }
    Cum cum;
    cum.build(wv);

    // bit sync (15 ones) + frame sync, both patterns, +-1 % bit rate - non-coherent:
    // sum of template x signal = A e^(j theta) j sin(1.1) C, whatever the carrier phase
    int tmpl[2][24];
    for (int v = 0; v < 2; v++)
        for (int k = 0; k < 24; k++) tmpl[v][k] = k < 15 ? 1 : (v ? FS_SYNC_TEST : FS_SYNC_NORMAL)[k - 15];
    auto corr = [&](double t0, double T, int v) {
        cd s{};
        for (int k = 0; k < 24; k++) {
            const double a = t0 + k * T;
            const cd d = cum.integ(a, a + T / 2) - cum.integ(a + T / 2, a + T);
            s += tmpl[v][k] ? d : -d;
        }
        return s;
    };
    // the same in 6 coherent pieces of 4 bits, their magnitudes summed: a
    // carrier whose phase wanders over the 60 ms (a real recording: ~0.3 rad
    // per bit) no longer cancels its own correlation
    auto corr_seg = [&](double t0, double T, int v) {
        double s = 0;
        for (int g = 0; g < 24; g += 4) {
            cd p{};
            for (int k = g; k < g + 4; k++) {
                const double a = t0 + k * T;
                const cd d = cum.integ(a, a + T / 2) - cum.integ(a + T / 2, a + T);
                p += tmpl[v][k] ? d : -d;
            }
            s += std::abs(p);
        }
        return s;
    };
    // the sync's position, bit period and pattern - by the coherent
    // correlation (best at a weak, steady carrier) and by the one in 4-bit
    // pieces (a carrier whose phase wanders); both are demodulated when they
    // disagree and the one whose sync bits come out cleaner is taken
    struct Peak {
        double m = 0, t0 = 0, T = SPB;
        int v = 0;
    } pc, ps;
    const int t_lo = std::max(0, static_cast<int>(on_n + 0.085 * FSD)),
              t_hi = std::min(static_cast<int>(nout) - static_cast<int>(30 * SPB), static_cast<int>(on_n + 0.27 * FSD));
    for (double T : {SPB * 0.99, SPB, SPB * 1.01})
        for (int v = 0; v < 2; v++)
            for (int t0 = t_lo; t0 <= t_hi; t0++) {
                const double c = std::abs(corr(t0, T, v)), g = corr_seg(t0, T, v);
                if (c > pc.m) pc = {c, static_cast<double>(t0), T, v};
                if (g > ps.m) ps = {g, static_cast<double>(t0), T, v};
            }
    if (pc.m <= 0 || ps.m <= 0) return false;
    {
        const double l = std::abs(corr(pc.t0 - 1, pc.T, pc.v)), rr = std::abs(corr(pc.t0 + 1, pc.T, pc.v));
        const double den = l - 2 * pc.m + rr;
        if (den < 0) pc.t0 += std::clamp(0.5 * (l - rr) / den, -0.5, 0.5);
    }
    {
        const double l = corr_seg(ps.t0 - 1, ps.T, ps.v), rr = corr_seg(ps.t0 + 1, ps.T, ps.v);
        const double den = l - 2 * ps.m + rr;
        if (den < 0) ps.t0 += std::clamp(0.5 * (l - rr) / den, -0.5, 0.5);
    }
    auto attempt = [&](double bt0, double bT, int bv, Burst* b) -> bool {
        // the carrier: 150 .. 12 ms before the first bit
        const int c0 = std::max(1, static_cast<int>(bt0 - 0.150 * FSD)), c1 = static_cast<int>(bt0 - 0.012 * FSD);
        if (c1 - c0 < static_cast<int>(0.08 * FSD)) return false;
        // in 20 ms pieces: amplitude = their mean magnitude, noise = the scatter
        // around each piece's own mean, phase = the last piece's (next to the
        // bits) - a carrier phase step or wander inside the 138 ms (a real
        // recording has one) no longer shrinks A, inflates the noise (C/N0 read
        // 32 dBHz on a ~60 dB signal) or starts the phase loop off
        const int piece = static_cast<int>(0.020 * FSD);
        double asum = 0, noise = 0;
        int npc = 0;
        cd last{};
        for (int p0 = c0; p0 + piece / 2 <= c1; p0 += piece) {
            const int p1 = std::min(c1, p0 + piece);
            cd m{};
            for (int n = p0; n < p1; n++) m += wv[static_cast<size_t>(n)];
            m /= static_cast<double>(p1 - p0);
            for (int n = p0; n < p1; n++) noise += std::norm(wv[static_cast<size_t>(n)] - m);
            asum += std::abs(m);
            npc++;
            last = m;
        }
        noise /= static_cast<double>(c1 - c0);
        const double A = npc ? asum / npc : 0;
        if (A <= 0) return false;
        const double th = std::arg(last);
        std::vector<cd> r(nout);
        for (size_t n = 0; n < nout; n++) r[n] = wv[n] * cd(std::cos(-th), std::sin(-th)) / A;
        cum.build(r);
        const double norm0 = 24 * bT * std::sin(PHASE_DEV);
        const cd sc = corr(bt0, bT, bv);
        const double best = sc.imag();   // + = biphase-L as T.001 (1 = +1.1 rad first)
        const double strength = corr_seg(bt0, bT, bv) / norm0;
        if (strength < 0.4) {
            if (verbose) {
                char s[160];
                snprintf(s, sizeof s, "carrier %+.1f Hz: sync corr %.2f of a clean one at %.1f ms (T %.2f) - too weak", fc + df,
                         strength, (bt0 - on_n) / FSD * 1e3, bT);
                log.push_back(s);
            }
            return false;
        }
        // the phase sense from the carrier's phase - checked against the bit sync below
        double pol = best >= 0 ? 1.0 : -1.0;

        // bits with timing + phase tracking
        b->bits.assign(144, 0);
        b->soft.assign(144, 0.0f);
        double t = bt0, T = bT, phi = 0, nu = 0;
        double amp = std::sin(PHASE_DEV);
        {
            double s = 0;
            for (int k = 0; k < 24; k++) {
                const double a = bt0 + k * bT;
                s += std::fabs((cum.integ(a, a + bT / 2) - cum.integ(a + bT / 2, a + bT)).imag());
            }
            amp = std::max(0.05, s / 24 / bT);
        }
        int nb = 144;
        // per bit, for the second pass: the half-bit integrals as received (the
        // first pass's timing) and the amplitude then
        std::vector<cd> raw1(144), raw2(144);
        std::vector<double> amp_k(144, 0.0), T_k(144, SPB);
        for (int k = 0; k < 144; k++) {
            if (t + T + 2 >= static_cast<double>(nout)) {
                nb = k;
                break;
            }
            const double mid = t + T / 2;
            const cd rot(std::cos(-phi), std::sin(-phi));
            raw1[static_cast<size_t>(k)] = cum.integ(t, mid);
            raw2[static_cast<size_t>(k)] = cum.integ(mid, t + T);
            amp_k[static_cast<size_t>(k)] = amp;
            T_k[static_cast<size_t>(k)] = T;
            const cd h1 = raw1[static_cast<size_t>(k)] * rot, h2 = raw2[static_cast<size_t>(k)] * rot;
            const double D = pol * (h1 - h2).imag();
            const double soft = D / (amp * T);
            b->soft[static_cast<size_t>(k)] = static_cast<float>(soft);
            b->bits[static_cast<size_t>(k)] = D > 0;
            // phase: the bit's mean is the residual carrier (cos 1.1, real)
            const cd P = h1 + h2;
            if (std::abs(P) > 0) {
                const double e = wrap(std::arg(P));
                nu += 0.01 * e;
                phi += 0.25 * e + nu;
            }
            // timing: the mid-bit transition
            const double W = pol * (cum.integ(mid - T / 4, mid + T / 4) * rot).imag() * (D > 0 ? 1 : -1);
            double tau = std::clamp(W / (2 * amp), -T / 4, T / 4);
            if (k < 24 || std::fabs(soft) > 0.3) {
                amp += 0.05 * (std::fabs(D) / T - amp);
            } else {
                tau = 0;
            }
            t += T + 0.3 * tau;
            T += 0.01 * tau;
            T = std::clamp(T, SPB * 0.985, SPB * 1.015);
        }
        if (nb < 112) return false;
        // second pass: the carrier phase of each bit from the bits around it
        // (+-PH_W, non-causal - the means of biphase bits are the residual
        // carrier): a phase step or fast wander of the carrier costs <= PH_W
        // bits instead of the loop's settling. A real recording (sigidwiki's
        // EPIRB, 2015) has three such steps; the loop lost PDF-2 there. Kept
        // when it is the more confident of the two (mean |soft|)
        {
            constexpr int PH_W = 2;
            std::vector<float> s2(static_cast<size_t>(nb));
            double a1 = 0, a2 = 0;
            for (int k = 0; k < nb; k++) {
                cd ref{};
                for (int j = std::max(0, k - PH_W); j <= std::min(nb - 1, k + PH_W); j++)
                    ref += raw1[static_cast<size_t>(j)] + raw2[static_cast<size_t>(j)];
                const double ra = std::abs(ref);
                const cd rot = ra > 0 ? std::conj(ref) / ra : cd(1, 0);
                const double D = pol * ((raw1[static_cast<size_t>(k)] - raw2[static_cast<size_t>(k)]) * rot).imag();
                s2[static_cast<size_t>(k)] = static_cast<float>(D / (amp_k[static_cast<size_t>(k)] * T_k[static_cast<size_t>(k)]));
                a1 += std::fabs(b->soft[static_cast<size_t>(k)]);
                a2 += std::fabs(s2[static_cast<size_t>(k)]);
            }
            if (a2 > a1)
                for (int k = 0; k < nb; k++) {
                    b->soft[static_cast<size_t>(k)] = s2[static_cast<size_t>(k)];
                    b->bits[static_cast<size_t>(k)] = s2[static_cast<size_t>(k)] > 0;
                }
            if (verbose) {
                char s[120];
                snprintf(s, sizeof s, "phase per bit: mean |soft| loop %.2f, +-%d bits %.2f -> %s", a1 / nb, PH_W, a2 / nb,
                         a2 > a1 ? "per bit" : "loop");
                log.push_back(s);
            }
        }
        double sum_abs = 0, sum_sq = 0;
        for (int k = 0; k < 112; k++) {
            const double v = b->soft[static_cast<size_t>(k)];
            sum_abs += std::fabs(v);
            sum_sq += v * v;
        }
        int bs_err = 0, fs_err = 0;
        for (int k = 0; k < 15; k++) bs_err += b->bits[static_cast<size_t>(k)] != 1;
        // the phase sense came from the sync correlation at the carrier's phase;
        // when that phase was off by > 90 deg (a real recording's carrier drifts
        // through the 160 ms) every bit comes out inverted - the 15-bit bit sync
        // (all ones) settles it once the bits are tracked
        if (bs_err >= 8) {
            pol = -pol;
            for (int k = 0; k < nb; k++) {
                b->soft[static_cast<size_t>(k)] = -b->soft[static_cast<size_t>(k)];
                b->bits[static_cast<size_t>(k)] = !b->bits[static_cast<size_t>(k)];
            }
            bs_err = 15 - bs_err;
        }
        // normal or self-test: the frame sync the decoded bits match (they differ
        // in 8 of 9 bits; the correlation's pick could follow a wandering phase)
        int fe[2] = {0, 0};
        for (int v = 0; v < 2; v++)
            for (int k = 0; k < 9; k++) fe[v] += b->bits[static_cast<size_t>(15 + k)] != tmpl[v][15 + k];
        bv = fe[1] < fe[0] ? 1 : 0;
        fs_err = fe[bv];
        if (verbose) {
            char s[200];
            snprintf(s, sizeof s, "carrier %+.1f Hz: sync corr %.2f, bit sync errors %d, frame sync %s errors %d, T %.3f", fc + df,
                     strength, bs_err, bv ? "self-test" : "normal", fs_err, T);
            log.push_back(s);
        }
        if (getenv("B406_BITS")) {   // offline diagnosis: every demodulated burst's soft bits
            fprintf(stderr, "B406_BITS carrier %+.1f Hz T %.3f A %.4g pol %+.0f bs_err %d fs_err %d:", fc + df, T, A, pol, bs_err, fs_err);
            for (int k = 0; k < nb; k++) fprintf(stderr, " %.2f", b->soft[static_cast<size_t>(k)]);
            fprintf(stderr, "\n");
        }
        if (bs_err > 2 || fs_err > 1) return false;
        const double mean = sum_abs / 112, var = sum_sq / 112 - mean * mean;
        double tq = 0;
        for (int k = 112; k < nb; k++) tq += std::fabs(b->soft[static_cast<size_t>(k)]);
        b->tail_q = nb > 112 ? tq / (nb - 112) : 0;
        b->self_test = bv == 1;
        b->sync_errors = bs_err + fs_err;
        b->inverted = pol < 0;
        b->freq_hz = fc + df;
        b->t_mod = base + bt0 * DEC;
        b->t_carrier = b->t_mod - 0.160 * FS;
        b->bit_s = T / FSD;
        b->level_db = 10 * std::log10(A * A);
        // C/N0: the channel filter passes ~2 x LP_CUT of noise
        b->snr_db = 10 * std::log10(std::max(1e-12, A * A / std::max(1e-30, noise / (2 * LP_CUT))));
        b->ber_q = var > 0 ? mean / std::sqrt(var) : 99;
        return true;
    };
    Burst b1, b2;
    const bool ok1 = attempt(pc.t0, pc.T, pc.v, &b1);
    const bool same = std::fabs(ps.t0 - pc.t0) < 1.5 && ps.T == pc.T;
    const bool ok2 = !same && attempt(ps.t0, ps.T, ps.v, &b2);
    if (!ok1 && !ok2) return false;
    if (ok1 && ok2) {
        const bool second = b2.sync_errors < b1.sync_errors || (b2.sync_errors == b1.sync_errors && b2.ber_q > b1.ber_q);
        *b = second ? b2 : b1;
    } else {
        *b = ok1 ? b1 : b2;
    }
    return true;
}

}  // namespace b406
