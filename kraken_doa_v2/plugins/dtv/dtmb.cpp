// DTMB (GB 20600-2006 / ITU-R BT.1306 system D): 7.56 Msps (8 MHz
// channel; 6 / 7 MHz versions scaled), every signal frame = a PN frame
// header (420 / 595 / 945 chips, the chip on I and Q, root-raised-cosine
// 0.05) + a 3780-symbol body (single- or multi-carrier - unknown data here).
// ~1800 frames a second, each header a known reference:
//   1. the channel centre: the channel_mhz option, else the 8 MHz raster
//      (UHF 474 + 8k MHz, VHF 52.5 / 171 + 8k; 6 / 7 MHz: 473 / 177.5 / 529.5
//      + k bw) channels that overlap the slice, the carrier searched +-24 kHz;
//      the header mode (PN420 / PN595 / PN945) the one that correlates best,
//   2. timing: the header's reference for exactly the slice (its DTFT x the
//      RRC at the centre's offset, as for ATSC) correlated with ~27 ms of
//      antenna 0 (64k-point FFTs), the strongest peak; the frames follow at
//      the frame period,
//   3. per frame and antenna: a 512-sample window (213 us) around the header,
//      H = Y conj(R) / (|R|^2 + lambda) on the slice's bins (Hann weighted),
//      zero-padded IFFT -> the CIR (0.21 us taps).
// PN420 / PN945 headers may change phase from frame to frame (the m-sequence
// started up to 56 chips later, in a schedule over the 125 ms superframe):
// against the phase-0 reference that only moves the CIR, so every frame's
// response is aligned to the burst's running power delay profile before it
// is used (which also takes out the sample-clock drift). Constant-phase
// networks simply measure no moves.

#include "dsp_util.hpp"
#include "estimator.hpp"
#include "std/dtmb_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sfn {

namespace {

constexpr double RATE = 2.4e6;
constexpr int NF = 512;                  // CIR window
constexpr int PRE = 128;                 // window start before the reference header position (53 us)
constexpr int P = 1024;                  // CIR taps (zero-padded)
constexpr int ACQ = 65536;               // acquisition FFT
constexpr double EDGE_HZ = 0.45 * RATE;
constexpr double MIN_PEAKY = 40;
constexpr int LOST_BURSTS = 4;
constexpr double PROCESS_S = 0.1;        // of each burst (~180 frames)
constexpr int ALIGN_TAPS = 80;           // a frame's CIR may move this far from the burst's first (schedule: 112 chips = 71 taps)

std::string fmtd(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

struct Mode {
    const dtmb_std::HeaderMode* hm;
    double bw_mhz;
    double rs;                           // symbol rate
};

class DtmbEstimator : public Estimator {
public:
    explicit DtmbEstimator(kp::Host& h) : host_(h) {}
    std::string name() const override { return "DTMB"; }
    std::string describe() const override {
        if (!locked_) return "";
        char b[96];
        snprintf(b, sizeof b, "DTMB PN%d, %g MHz%s", mode_.hm->header_len, mode_.bw_mhz,
                 moves_ > 0.5 ? ", phase varying" : "");
        return b;
    }
    void reset() override {
        locked_ = false;
        fails_ = 0;
        moves_ = 0;
        rf_ = NAN;
    }
    // the frame header's PN length gives the guard; the frame body's system
    // information (constellation, code rate, interleaver) is not read
    std::vector<std::pair<std::string, std::string>> info() const override {
        if (!locked_) return {};
        const int L = mode_.hm->header_len;
        return {{"Frame header", "PN" + std::to_string(L) + fmtd(" (guard %.1f us", L / mode_.rs * 1e6) +
                                     (L == 595 ? ", fixed PN)" : moves_ > 0.5 ? ", phase varying)" : ", constant phase)")},
                {"Symbol rate", fmtd("%.2f Msymbol/s", mode_.rs / 1e6)},
                {"Frame", fmtd("%.0f us", (L + 3780) / mode_.rs * 1e6) + " (header + 3780-symbol body)"}};
    }
    double centre_hz() const override { return locked_ && std::isfinite(rf_) ? rf_ + centre_ : NAN; }
    double bw_mhz() const override { return locked_ ? mode_.bw_mhz : 0; }

    double detect(const Burst& b, const EstOptions& o) override {
        const auto& x = input(b);
        if (x.empty() || x[0].size() < static_cast<size_t>(ACQ + NF)) return 0;
        Fft& fa = fft_.get(ACQ, -1);
        std::copy_n(x[0].data(), ACQ, fa.data());
        fa.run();
        const std::vector<kp::cf> Y(fa.data(), fa.data() + ACQ);
        double best = 0, bc = 0;
        Mode bm{};
        for (double bw : {8.0, 7.0, 6.0}) {
            if (o.bw_mhz > 0 && std::fabs(o.bw_mhz - bw) > 0.01) continue;
            const double rs = dtmb_std::kSymbolRateHz * bw / 8;
            std::vector<double> cands;
            if (std::isfinite(o.channel_hz) && std::isfinite(b.rf_hz)) cands.push_back(o.channel_hz - b.rf_hz);
            else if (std::isfinite(b.rf_hz))
                for (double c : raster(b.rf_hz, bw)) cands.push_back(c - b.rf_hz);
            else cands.push_back(0);
            for (const auto* hm : {&dtmb_std::kPN420, &dtmb_std::kPN945, &dtmb_std::kPN595}) {
                const Mode md{hm, bw, rs};
                for (double c : cands) {
                    acq_ref(md, c);
                    const double b0 = best;
                    double lc = c;
                    auto try_d = [&](double d) {
                        long q;
                        const double pk = acq_corr(Y, d, &q);
                        if (pk > best) {
                            best = pk;
                            bc = c + d;
                            bm = md;
                            lc = c + d;
                        }
                    };
                    for (int d = -24; d <= 24; d += 6) try_d(d * 1e3);
                    if (best > b0) {
                        const double d1 = lc - c;
                        for (int d = -2; d <= 2; d++) try_d(d1 + d * 1e3);
                        const double d2 = lc - c;
                        for (int d = -2; d <= 2; d++) try_d(d2 + d * 250.0);
                    }
                }
            }
        }
        if (o.verbose && bm.hm)
            host_.log("DTMB: PN" + std::to_string(bm.hm->header_len) + fmtd(" %g MHz", bm.bw_mhz) + ", centre " +
                      fmtd("%+.1f kHz", bc / 1e3) + ", header peak/mean " + fmtd("%.0f", best));
        if (best < MIN_PEAKY || !bm.hm) return 0;
        mode_ = bm;
        centre_ = bc;
        wref_ok_ = false;
        locked_ = true;
        fails_ = 0;
        moves_ = 0;
        return std::clamp((std::log10(best) - 1.5) / 1.0, 0.0, 1.0);
    }

    bool process(const Burst& b, const EstOptions& o, const Sink& sink) override {
        if (!locked_) return false;
        const auto& x = input(b);
        const int m = static_cast<int>(x.size());
        const long n = m ? std::min<long>(static_cast<long>(x[0].size()), static_cast<long>(PROCESS_S * RATE) + ACQ) : 0;
        if (n < ACQ + NF) return fail();
        Fft& fa = fft_.get(ACQ, -1);
        std::copy_n(x[0].data(), ACQ, fa.data());
        fa.run();
        const std::vector<kp::cf> Y(fa.data(), fa.data() + ACQ);
        acq_ref(mode_, centre_);
        long q = 0;
        const double pk = acq_corr(Y, 0, &q);
        if (pk < MIN_PEAKY / 2) return fail();
        if (!wref_ok_) build_window_ref();
        const double frame = dtmb_std::frame_len(*mode_.hm) / mode_.rs * RATE;
        std::vector<double> frames;
        for (double p = static_cast<double>(q) - frame * std::floor(static_cast<double>(q) / frame); p - PRE + NF <= n &&
                                                                                                     p < PROCESS_S * RATE;
             p += frame)
            if (p - PRE >= 0) frames.push_back(p);
        if (frames.size() < 4) return fail();
        CirSnapshot cs;
        cs.m = m;
        cs.ntap = P;
        cs.tap_us = 1e6 * NF / (RATE * P);
        cs.h.assign(static_cast<size_t>(m) * P, kp::cf(0, 0));
        std::vector<kp::cf> h;
        const size_t nfr = frames.size();
        const double chip_taps = RATE / mode_.rs * P / NF;   // taps per chip
        // pass 1 (antenna 0): how far each frame's response moved from frame 0's (chips)
        std::vector<double> meas(nfr, 0.0), pdp0(P), pdp(P);
        for (size_t j = 0; j < nfr; j++) {
            cir(x[0], frames[j], 0, &h);
            auto& d = j == 0 ? pdp0 : pdp;
            for (int t = 0; t < P; t++) d[static_cast<size_t>(t)] = std::norm(h[static_cast<size_t>(t)]);
            if (j > 0) meas[j] = pdp_shift(pdp0, pdp) / chip_taps;
        }
        // the header phase schedule: the burst's frames from some frame l0 of
        // the superframe on (either direction - the standard's bit order is
        // the one uncertain point), or constant phase. The model most frames
        // agree with (within a chip) gives the exact moves; a burst no model
        // fits uses the frames' own measurements.
        std::vector<double> pred(nfr, 0.0);
        auto hits_of = [&](const std::vector<double>& p) {
            int n = 0;
            for (size_t j = 0; j < nfr; j++) n += std::fabs(meas[j] - p[j]) < 1.0;
            return n;
        };
        int best = hits_of(pred);
        bool varying = false;
        if (mode_.hm->cyclic) {
            const int F = mode_.hm->frames_per_superframe, N = mode_.hm->core_len;
            std::vector<int> adv(static_cast<size_t>(F));
            for (int l = 0; l < F; l++) {
                const int a = dtmb_std::phase_advance(*mode_.hm, l);
                adv[static_cast<size_t>(l)] = a > N / 2 ? a - N : a;
            }
            std::vector<double> p(nfr);
            for (int l0 = 0; l0 < F; l0++)
                for (int dir : {-1, 1}) {
                    for (size_t j = 0; j < nfr; j++)
                        p[j] = dir * (adv[static_cast<size_t>((l0 + static_cast<int>(j)) % F)] - adv[static_cast<size_t>(l0)]);
                    const int n = hits_of(p);
                    if (n > best) {
                        best = n;
                        pred = p;
                        varying = true;
                    }
                }
        }
        if (best < 0.6 * static_cast<double>(nfr)) pred = meas;
        // the sample-clock drift: a line through the agreeing frames' residuals
        double sj = 0, sr = 0, sjj = 0, sjr = 0, cnt = 0;
        for (size_t j = 0; j < nfr; j++) {
            const double r = meas[j] - pred[j];
            if (std::fabs(r) >= 1.0) continue;
            sj += j;
            sr += r;
            sjj += static_cast<double>(j * j);
            sjr += j * r;
            cnt++;
        }
        double da = 0, db = 0;
        if (cnt >= 3) {
            db = (cnt * sjr - sj * sr) / std::max(1e-9, cnt * sjj - sj * sj);
            da = (sr - db * sj) / cnt;
        }
        int snaps = 0;
        for (size_t j = 0; j < nfr; j++) {
            const double corr = (pred[j] + da + db * static_cast<double>(j)) * chip_taps * NF / P;   // samples later
            for (int a = 0; a < m; a++) {
                cir(x[static_cast<size_t>(a)], frames[j], corr, &h);
                std::copy(h.begin(), h.end(), cs.h.begin() + static_cast<long>(a) * P);
            }
            cs.t0 = b.t0 + (frames[j] - PRE) / RATE;
            cs.t1 = cs.t0 + NF / RATE;
            sink(cs);
            snaps++;
        }
        moves_ = 0.7 * moves_ + 0.3 * (varying ? 1.0 : 0.0);
        fit_ = static_cast<double>(best) / static_cast<double>(nfr);
        fails_ = 0;
        rf_ = b.rf_hz;
        if (std::isfinite(b.rf_hz)) host_.fact("Channel centre", fmtd("%.4f MHz", (b.rf_hz + centre_) / 1e6));
        host_.fact("Frame header", "PN" + std::to_string(mode_.hm->header_len) + fmtd(", peak %.0f x the mean", pk) +
                                       (moves_ > 0.5 ? ", phase varying" : ", constant phase"));
        if (o.verbose)
            host_.log("DTMB: " + std::to_string(snaps) + " frames, peak/mean " + fmtd("%.0f", pk) + fmtd(", varying %.2f", moves_) +
                      fmtd(", schedule fit %.2f", fit_));
        return true;
    }

private:
    kp::Host& host_;
    FftCache fft_;
    bool locked_ = false;
    int fails_ = 0;
    Mode mode_{};
    double centre_ = 0;
    double rf_ = NAN;                      // the VFO centre of the last burst
    double moves_ = 0;                     // phase-varying headers (decaying, 0..1)
    double fit_ = 0;                       // frames the header model fitted (last burst)
    std::vector<std::vector<kp::cf>> rs_;
    std::vector<kp::cf> acq_;
    std::vector<std::complex<double>> wref_;
    std::vector<double> wgt_;
    double wlam_ = 1;
    bool wref_ok_ = false;

    bool fail() {
        if (++fails_ >= LOST_BURSTS) locked_ = false;
        return locked_;
    }

    const std::vector<std::vector<kp::cf>>& input(const Burst& b) {
        if (std::fabs(b.rate - RATE) < 1) return b.ch;
        Resampler r(b.rate, RATE);
        rs_.assign(b.ch.size(), {});
        for (size_t a = 0; a < b.ch.size(); a++) r.run(b.ch[a].data(), b.ch[a].size(), &rs_[a]);
        return rs_;
    }

    static std::vector<double> raster(double rf, double bw) {
        std::vector<double> bases;
        if (bw > 7.5) bases = {474e6, 171e6, 52.5e6};
        else if (bw > 6.5) bases = {177.5e6, 529.5e6};
        else bases = {473e6, 57e6};
        std::vector<double> c;
        for (double base : bases) {
            const double k = std::round((rf - base) / (bw * 1e6));
            for (int d = -1; d <= 1; d++) {
                const double ctr = base + (k + d) * bw * 1e6;
                if (std::fabs(ctr - rf) < bw * 0.5e6 + 0.9e6 && std::find(c.begin(), c.end(), ctr) == c.end()) c.push_back(ctr);
            }
        }
        return c;
    }

    static double bin_hz(int k, int n) { return (k < n / 2 ? k : k - n) * RATE / n; }

    double rrc(double g) const {
        const double a = dtmb_std::kRollOff, fn = mode_.rs / 2, x = std::fabs(g);
        if (x <= fn * (1 - a)) return 1;
        if (x >= fn * (1 + a)) return 0;
        return std::sqrt(0.5 * (1 + std::cos(M_PI * (x - fn * (1 - a)) / (2 * a * fn))));
    }

    // the header's spectrum (phase 0, chips on I and Q) at g Hz from the
    // channel centre, first chip at t = 0
    std::complex<double> dtft(const std::vector<int>& chips, double g) const {
        const std::complex<double> w = std::polar(1.0, -2 * M_PI * g / mode_.rs);
        std::complex<double> s = 0;
        for (size_t i = chips.size(); i-- > 0;) s = s * w + static_cast<double>(chips[i]);
        return s * std::complex<double>(1, 1);
    }

    // the slice's reference on n bins, the window weights (Hann over the usable band)
    void slice_ref(int n, double centre, std::vector<std::complex<double>>* r, std::vector<double>* wgt) const {
        r->assign(static_cast<size_t>(n), 0.0);
        wgt->assign(static_cast<size_t>(n), 0.0);
        const double half = mode_.rs / 2 * (1 + dtmb_std::kRollOff);
        const double lo = std::max(-EDGE_HZ, centre - half), hi = std::min(EDGE_HZ, centre + half);
        if (hi - lo < 200e3) return;
        const auto chips = dtmb_std::reference_header(*mode_.hm);
        for (int k = 0; k < n; k++) {
            const double f = bin_hz(k, n);
            if (f < lo || f > hi) continue;
            const double p = rrc(f - centre);
            if (p < 0.05) continue;
            (*r)[static_cast<size_t>(k)] = p * dtft(chips, f - centre);
            (*wgt)[static_cast<size_t>(k)] = 0.5 - 0.5 * std::cos(2 * M_PI * (f - lo) / (hi - lo));
        }
    }

    void acq_ref(const Mode& md, double centre) {
        mode_ = md;
        std::vector<std::complex<double>> r;
        std::vector<double> w;
        constexpr int NR = 2048;   // the header is < 400 samples: 2048 bins sample its spectrum densely enough
        slice_ref(NR, centre, &r, &w);
        Fft& ti = fft_.get(NR, +1);
        for (int k = 0; k < NR; k++) {
            const std::complex<double> v = w[static_cast<size_t>(k)] > 0 ? r[static_cast<size_t>(k)] : 0.0;
            ti.data()[k] = kp::cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        ti.run();
        Fft& fr = fft_.get(ACQ, -1);
        std::fill(fr.data(), fr.data() + ACQ, kp::cf(0, 0));
        for (int i = 0; i < NR; i++) {
            const int t = i < NR / 2 ? i : i - NR;
            fr.data()[(t % ACQ + ACQ) % ACQ] = ti.data()[i] / static_cast<float>(NR);
        }
        fr.run();
        acq_.resize(ACQ);
        for (int k = 0; k < ACQ; k++) acq_[static_cast<size_t>(k)] = std::conj(fr.data()[k]);
    }

    double acq_corr(const std::vector<kp::cf>& Y, double d, long* q) {
        const int sh = static_cast<int>(std::lround(d * ACQ / RATE));
        Fft& c = fft_.get(ACQ, +1);
        for (int k = 0; k < ACQ; k++) c.data()[k] = Y[static_cast<size_t>((k + sh + ACQ) % ACQ)] * acq_[static_cast<size_t>(k)];
        c.run();
        double mx = 0, sum = 0;
        long at = 0;
        const long lim = ACQ - NF;
        for (long i = 0; i < ACQ; i++) {
            const double v = std::norm(c.data()[i]);
            sum += v;
            if (i < lim && v > mx) {
                mx = v;
                at = i;
            }
        }
        *q = at;
        return sum > 0 ? mx / (sum / ACQ) : 0;
    }

    void build_window_ref() {
        slice_ref(NF, centre_, &wref_, &wgt_);
        double s = 0;
        int c = 0;
        for (int k = 0; k < NF; k++)
            if (wgt_[static_cast<size_t>(k)] > 0) {
                s += std::norm(wref_[static_cast<size_t>(k)]);
                c++;
            }
        // PN420 / PN945 repeat their m-sequence (1.85 / 1.65 periods): their
        // correlation has echoes one period away, which only a near-full
        // division removes (-18 dB at 0.03 vs -11 dB at 0.5)
        wlam_ = c ? (mode_.hm->cyclic ? 0.03 : 0.5) * s / c : 1;
        wref_ok_ = true;
    }

    // the CIR of one antenna for the header at sample p, shifted by `shift`
    // samples: P taps, the header at tap PRE * P / NF
    void cir(const std::vector<kp::cf>& x, double p, double shift, std::vector<kp::cf>* h) {
        const long ws = static_cast<long>(std::floor(p - PRE));
        const double ts = p - static_cast<double>(ws) + shift;
        Fft& f = fft_.get(NF, -1);
        std::copy_n(x.data() + ws, NF, f.data());
        f.run();
        Fft& ti = fft_.get(P, +1);
        std::fill(ti.data(), ti.data() + P, kp::cf(0, 0));
        // bin k (signed) of the reference delayed to ts: R e^(-j 2 pi k ts / NF); divided
        // out, the response moved to PRE: x e^(+j 2 pi k ts / NF) e^(-j 2 pi k PRE / NF)
        const std::complex<double> step = std::polar(1.0, 2 * M_PI * (ts - PRE) / NF);
        for (int half = 0; half < 2; half++) {
            // k = 0 .. NF/2-1, then k = -1 .. -NF/2
            std::complex<double> rot = half == 0 ? 1.0 : std::conj(step);
            const std::complex<double> st = half == 0 ? step : std::conj(step);
            for (int i = 0; i < NF / 2; i++, rot *= st) {
                const int k = half == 0 ? i : NF - 1 - i;
                const double w = wgt_[static_cast<size_t>(k)];
                if (w <= 0) continue;
                const std::complex<double> r = wref_[static_cast<size_t>(k)];
                const std::complex<double> v = std::complex<double>(f.data()[k]) * std::conj(r) * rot * (w / (std::norm(r) + wlam_));
                const int kk = k < NF / 2 ? k : k - NF + P;
                ti.data()[kk] = kp::cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
            }
        }
        ti.run();
        h->assign(ti.data(), ti.data() + P);
    }

    // the shift (taps) of profile b against a, within +-ALIGN_TAPS, parabola-refined
    static double pdp_shift(const std::vector<double>& a, const std::vector<double>& b) {
        constexpr int L = ALIGN_TAPS;
        std::vector<double> c(2 * L + 1);
        for (int l = -L; l <= L; l++) {
            double s = 0;
            for (int t = 0; t < P; t++) s += a[static_cast<size_t>(t)] * b[static_cast<size_t>(((t + l) % P + P) % P)];
            c[static_cast<size_t>(l + L)] = s;
        }
        int bl = 0;
        for (int l = -L; l <= L; l++)
            if (c[static_cast<size_t>(l + L)] > c[static_cast<size_t>(bl + L)]) bl = l;
        if (bl == -L || bl == L) return bl;
        const double y0 = c[static_cast<size_t>(bl + L - 1)], y1 = c[static_cast<size_t>(bl + L)], y2 = c[static_cast<size_t>(bl + L + 1)];
        const double den = y0 - 2 * y1 + y2;
        return bl + (den < 0 ? 0.5 * (y0 - y2) / den : 0);
    }
};

}  // namespace

std::unique_ptr<Estimator> make_dtmb(kp::Host& h) { return std::make_unique<DtmbEstimator>(h); }

}  // namespace sfn
