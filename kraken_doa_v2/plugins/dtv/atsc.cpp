// ATSC 1.0 (A/53 Part 2, 8-VSB): the field sync segment (one every 24.2 ms)
// starts with 728 known symbols - segment sync, PN511, three PN63 (the
// middle one inverted in every other field), the VSB mode bits. The rest of
// the segment (reserved / precode) and every data segment are unknown.
//
// The VFO sees 2.4 MHz of the 6 MHz channel. The reference is built for
// exactly that slice in the frequency domain: the symbols' spectrum (DTFT)
// times the root-raised-cosine (alpha 0.1152), at the channel centre's
// offset from the VFO - no time-domain resampling, so nothing aliases.
//   1. the channel centre: the channel_mhz option, else the 6 MHz raster
//      (US / Korea / Canada / Mexico) channels that overlap the slice; the
//      pilot (2.69 MHz below the centre) gives it exactly when the slice holds
//      it, else a carrier search +-24 kHz (stations sit up to ~20 kHz off)
//   2. timing: the reference correlated with ~27 ms (more than one field) of
//      antenna 0 at once (64k-point FFTs), the strongest peak
//   3. per field sync and antenna: a window of 2048 samples (853 us) around
//      it, H = Y conj(R) / (|R|^2 + lambda) on the slice's bins (Hann
//      weighted, the pilot notched), zero-padded IFFT -> the CIR (0.21 us
//      taps). The data symbols around the sync are noise here; averaging the
//      fields (14 per burst) and bursts takes care of it.
// The field parity (which PN63 is inverted) is decided per burst; the
// sample-clock drift within a burst is measured from the fields' impulse
// responses and taken out, so they line up.

#include "dsp_util.hpp"
#include "estimator.hpp"
#include "std/atsc_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sfn {

namespace {

constexpr double RATE = 2.4e6;
constexpr int NF = 1024;                 // CIR window (427 us: less of the unknown data around the sync)
constexpr int PRE = 240;                 // window start before the strongest path (100 us)
constexpr int P = 2048;                  // CIR taps (zero-padded)
constexpr int KNOWN = atsc_std::kFsReservedStart;   // 728 known symbols
constexpr int ACQ = 65536;               // acquisition FFT
constexpr double EDGE_HZ = 0.45 * RATE;  // the slice's usable band
constexpr double MIN_PEAKY = 40;         // acquisition |corr|^2 peak / mean
constexpr int LOST_BURSTS = 4;
const double RS = atsc_std::kSymbolRateHz;
const double FIELD = atsc_std::kFieldSymbols / RS * RATE;           // 58074.4 samples
const double PILOT = atsc_std::kPilotFromChannelCentreHz;           // -2.690559 MHz
const double FN = RS / 4;                                           // Nyquist frequency of the RRC
const double HALF_BW = FN * (1 + atsc_std::kRollOff);               // 3.0 MHz

std::string fmtd(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}

// root-raised-cosine amplitude response at g Hz from the channel centre
double rrc(double g) {
    const double a = atsc_std::kRollOff, x = std::fabs(g);
    if (x <= FN * (1 - a)) return 1;
    if (x >= FN * (1 + a)) return 0;
    return std::sqrt(0.5 * (1 + std::cos(M_PI * (x - FN * (1 - a)) / (2 * a * FN))));
}

// 6 MHz raster channel centres (Hz) whose band overlaps rf +- 1.2 MHz
std::vector<double> raster(double rf) {
    std::vector<double> c;
    auto add = [&](double lo) {
        const double ctr = lo + 3e6;
        if (std::fabs(ctr - rf) < 3e6 + 0.9e6) c.push_back(ctr);
    };
    for (int n = 2; n <= 4; n++) add(54e6 + 6e6 * (n - 2));
    for (int n = 5; n <= 6; n++) add(76e6 + 6e6 * (n - 5));
    for (int n = 7; n <= 13; n++) add(174e6 + 6e6 * (n - 7));
    for (int n = 14; n <= 83; n++) add(470e6 + 6e6 * (n - 14));
    return c;
}

int channel_number(double centre) {
    auto near = [&](double lo, int n0, int n1, int first) {
        for (int n = n0; n <= n1; n++)
            if (std::fabs(lo + 6e6 * (n - first) + 3e6 - centre) < 100e3) return n;
        return 0;
    };
    int n = near(54e6, 2, 4, 2);
    if (!n) n = near(76e6, 5, 6, 5);
    if (!n) n = near(174e6, 7, 13, 7);
    if (!n) n = near(470e6, 14, 83, 14);
    return n;
}

class AtscEstimator : public Estimator {
public:
    explicit AtscEstimator(kp::Host& h) : host_(h) {
        // the known symbols: levels (no pilot); the middle PN63 apart
        const auto s = atsc_std::field_sync_symbols(true);
        lv_.assign(s.begin(), s.begin() + KNOWN);
        mid_.assign(KNOWN, 0.0);
        for (int n = atsc_std::kFsPn63Start[1]; n < atsc_std::kFsPn63Start[1] + atsc_std::kFsPn63Len; n++) {
            mid_[static_cast<size_t>(n)] = lv_[static_cast<size_t>(n)];
            lv_[static_cast<size_t>(n)] = 0;
        }
    }
    std::string name() const override { return "ATSC 1.0"; }
    std::string describe() const override {
        if (!locked_) return "";
        return std::string("ATSC 1.0 8-VSB") + (chan_ > 0 ? ", channel " + std::to_string(chan_) : "");
    }
    void reset() override {
        locked_ = false;
        fails_ = 0;
        chan_ = 0;
        rf_ = NAN;
    }
    // everything else about the signal is fixed by the standard (A/53)
    std::vector<std::pair<std::string, std::string>> info() const override {
        if (!locked_) return {};
        return {{"Modulation", "8-VSB, 10.762 Msymbol/s"},
                {"Coding", "RS(207,187) + rate 2/3 trellis"},
                {"Net bit rate", "19.39 Mbit/s (fixed by the standard)"}};
    }
    double centre_hz() const override { return locked_ && std::isfinite(rf_) ? rf_ + centre_ : NAN; }
    double bw_mhz() const override { return locked_ ? 6 : 0; }

    double detect(const Burst& b, const EstOptions& o) override {
        const auto& x = input(b);
        if (x.empty() || x[0].size() < static_cast<size_t>(ACQ + NF)) return 0;
        psd(x[0]);
        // candidate channel centres (offset from the VFO centre)
        std::vector<double> cands;
        const bool rf_ok = std::isfinite(b.rf_hz);
        if (std::isfinite(o.channel_hz) && rf_ok) cands.push_back(o.channel_hz - b.rf_hz);
        else if (rf_ok)
            for (double c : raster(b.rf_hz)) cands.push_back(c - b.rf_hz);
        if (cands.empty()) {
            // no RF: a strong line in the slice is the pilot, else the VFO sits on the centre
            const double f = strongest_line();
            if (std::isfinite(f)) cands.push_back(f - PILOT);
            cands.push_back(0);
        }
        Fft& fa = fft_.get(ACQ, -1);
        std::copy_n(x[0].data(), ACQ, fa.data());
        fa.run();
        const std::vector<kp::cf> Y(fa.data(), fa.data() + ACQ);
        // the sync is 68 us long: a carrier 3 kHz off loses 0.6 dB - a 6 kHz grid,
        // then 1 kHz, then 250 Hz around the best (the received spectrum shifted)
        double best = 0, bc = 0;
        bool bpilot = false;
        for (double c0 : cands) {
            double c = c0;
            const bool pil = pilot_refine(&c);
            acq_ref(c, pil);
            auto try_d = [&](double d) {
                long q;
                const double pk = acq_corr(Y, d, &q);
                if (pk > best) {
                    best = pk;
                    bc = c + d;
                    bpilot = pil;
                }
            };
            if (pil) {
                try_d(0);
                continue;
            }
            const double b0 = best;
            for (int d = -24; d <= 24; d += 6) try_d(d * 1e3);
            if (best > b0) {
                const double d1 = bc - c;
                for (int d = -2; d <= 2; d++) try_d(d1 + d * 1e3);
                const double d2 = bc - c;
                for (int d = -2; d <= 2; d++) try_d(d2 + d * 250.0);
            }
        }
        if (best <= 0) return 0;
        if (o.verbose)
            host_.log("ATSC: centre " + fmtd("%+.1f kHz", bc / 1e3) + (bpilot ? " (pilot)" : "") + ", sync peak/mean " +
                      fmtd("%.0f", best));
        if (best < MIN_PEAKY) return 0;
        centre_ = bc;
        pilot_ = bpilot;
        wref_centre_ = NAN;
        locked_ = true;
        fails_ = 0;
        chan_ = rf_ok ? channel_number(b.rf_hz + bc) : 0;
        return std::clamp((std::log10(best) - 1.5) / 1.0, 0.0, 1.0);
    }

    bool process(const Burst& b, const EstOptions& o, const Sink& sink) override {
        if (!locked_) return false;
        const auto& x = input(b);
        const int m = static_cast<int>(x.size());
        const long n = m ? static_cast<long>(x[0].size()) : 0;
        if (n < ACQ + NF) return fail();
        // the carrier: the pilot again when the slice holds it (it moves with the tuner's error)
        if (pilot_) {
            psd(x[0]);
            double c = centre_;
            if (pilot_refine(&c) && std::fabs(c - centre_) < 2e3) centre_ = c;
        }
        // timing: the strongest field sync in the first 27 ms
        Fft& fa = fft_.get(ACQ, -1);
        std::copy_n(x[0].data(), ACQ, fa.data());
        fa.run();
        const std::vector<kp::cf> Y(fa.data(), fa.data() + ACQ);
        long q = 0;
        acq_ref(centre_, pilot_);
        const double pk = acq_corr(Y, 0, &q);
        if (pk < MIN_PEAKY / 2) return fail();
        // the slice's reference (both parities) for the window with the sync at t = 0
        build_window_ref(centre_);
        std::vector<double> fields;
        for (double p = static_cast<double>(q) - FIELD * std::floor(static_cast<double>(q) / FIELD); p - PRE + NF <= n; p += FIELD)
            if (p - PRE >= 0) fields.push_back(p);
        if (fields.size() < 2) return fail();
        // parity: which reference fits field 0, 2, ... (the others alternate)
        int par0 = 0;
        {
            double s[2] = {0, 0};
            for (size_t j = 0; j < fields.size() && j < 6; j++)
                for (int par = 0; par < 2; par++) s[par] += peak(x[0], fields[j], static_cast<int>((par + j) % 2));
            par0 = s[1] > s[0] ? 1 : 0;
        }
        // clock drift within the burst: each field's power delay profile vs
        // the first one's (antenna 0), a straight line through the shifts
        std::vector<double> pdp0, pdp, h0;
        std::vector<kp::cf> h;
        std::vector<double> sh(fields.size(), 0.0);
        for (size_t j = 0; j < fields.size(); j++) {
            cir(x[0], fields[j], static_cast<int>((par0 + j) % 2), 0, &h);
            pdp.resize(P);
            for (int t = 0; t < P; t++) pdp[static_cast<size_t>(t)] = std::norm(h[static_cast<size_t>(t)]);
            if (j == 0) pdp0 = pdp;
            else sh[j] = pdp_shift(pdp0, pdp);
        }
        double sj = 0, ss = 0, sjj = 0, sjs = 0;
        for (size_t j = 0; j < fields.size(); j++) {
            sj += j;
            ss += sh[j];
            sjj += static_cast<double>(j * j);
            sjs += j * sh[j];
        }
        const double nf = static_cast<double>(fields.size());
        const double slope = (nf * sjs - sj * ss) / std::max(1e-9, nf * sjj - sj * sj);   // taps per field
        drift_ppm_ = slope * (static_cast<double>(NF) / P) / FIELD * 1e6;
        CirSnapshot cs;
        cs.m = m;
        cs.ntap = P;
        cs.tap_us = 1e6 * NF / (RATE * P);
        cs.h.assign(static_cast<size_t>(m) * P, kp::cf(0, 0));
        int snaps = 0;
        for (size_t j = 0; j < fields.size(); j++) {
            const double corr = slope * static_cast<double>(j) * NF / P;    // samples: field j's sync that much later
            for (int a = 0; a < m; a++) {
                cir(x[static_cast<size_t>(a)], fields[j], static_cast<int>((par0 + j) % 2), corr, &h);
                std::copy(h.begin(), h.end(), cs.h.begin() + static_cast<long>(a) * P);
            }
            cs.t0 = b.t0 + (fields[j] - PRE) / RATE;
            cs.t1 = cs.t0 + NF / RATE;
            sink(cs);
            snaps++;
        }
        fails_ = 0;
        rf_ = b.rf_hz;
        if (std::isfinite(b.rf_hz)) host_.fact("Channel centre", fmtd("%.4f MHz", (b.rf_hz + centre_) / 1e6));
        host_.fact("Pilot", pilot_ ? "in the slice (carrier from it)" : "outside the slice (carrier searched)");
        host_.fact("Field sync", fmtd("peak %.0f x the mean", pk) + fmtd(", clock %+.2f ppm", drift_ppm_));
        if (o.verbose)
            host_.log("ATSC: " + std::to_string(snaps) + " fields, peak/mean " + fmtd("%.0f", pk) + ", parity " +
                      std::to_string(par0) + fmtd(", drift %.3f taps/field", slope));
        return true;
    }

private:
    kp::Host& host_;
    FftCache fft_;
    std::vector<double> lv_, mid_;         // known symbol levels without / only the middle PN63
    bool locked_ = false, pilot_ = false;
    int fails_ = 0, chan_ = 0;
    double centre_ = 0;                    // channel centre - VFO centre (Hz)
    double rf_ = NAN;                      // the VFO centre of the last burst
    double drift_ppm_ = 0;
    std::vector<double> psd_;              // 8192 bins, natural order
    std::vector<std::vector<kp::cf>> rs_;
    // the window reference (NF bins, sync at t = 0) per parity, its weights
    std::vector<std::complex<double>> wref_[2];
    std::vector<double> wgt_;              // Hann over the usable bins x pilot notch (0 = unused)
    double wlam_ = 0;
    double wref_centre_ = NAN;

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

    static double bin_hz(int k, int n) { return (k < n / 2 ? k : k - n) * RATE / n; }

    void psd(const std::vector<kp::cf>& x) {
        constexpr int N = 8192;
        psd_.assign(N, 0.0);
        Fft& f = fft_.get(N, -1);
        const long n = static_cast<long>(x.size());
        const long step = std::max<long>(N, (n - N) / 40);
        for (long s = 0; s + N <= n; s += step) {
            for (int i = 0; i < N; i++)
                f.data()[i] = x[static_cast<size_t>(s + i)] * static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N));
            f.run();
            for (int i = 0; i < N; i++) psd_[static_cast<size_t>(i)] += std::norm(f.data()[i]);
        }
    }
    // mean PSD in Hz range (a, b)
    double psd_mean(double a, double b) const {
        const int N = static_cast<int>(psd_.size());
        double s = 0;
        int c = 0;
        for (int k = 0; k < N; k++) {
            const double f = bin_hz(k, N);
            if (f >= a && f <= b) {
                s += psd_[static_cast<size_t>(k)];
                c++;
            }
        }
        return c ? s / c : 0;
    }
    // the pilot within +-30 kHz of where centre *c puts it: *c from its exact frequency
    bool pilot_refine(double* c) const {
        const int N = static_cast<int>(psd_.size());
        const double pexp = *c + PILOT;
        if (N == 0 || std::fabs(pexp) > EDGE_HZ - 40e3) return false;
        int bk = -1;
        double bv = 0;
        for (int k = 0; k < N; k++) {
            const double f = bin_hz(k, N);
            if (std::fabs(f - pexp) <= 30e3 && psd_[static_cast<size_t>(k)] > bv) {
                bv = psd_[static_cast<size_t>(k)];
                bk = k;
            }
        }
        if (bk < 0) return false;
        // a line: well above the spectrum just above it (the data band starts at the pilot)
        const double around = psd_mean(bin_hz(bk, N) + 5e3, bin_hz(bk, N) + 60e3);
        if (bv < 20 * around) return false;
        const double a = psd_[static_cast<size_t>((bk + N - 1) % N)], m = bv, p = psd_[static_cast<size_t>((bk + 1) % N)];
        const double den = a - 2 * m + p;
        const double frac = den < 0 ? 0.5 * (a - p) / den : 0;
        *c = bin_hz(bk, N) + frac * RATE / N - PILOT;
        return true;
    }
    // the strongest narrow line in the slice (NAN if none clearly stands out)
    double strongest_line() const {
        const int N = static_cast<int>(psd_.size());
        if (N == 0) return NAN;
        std::vector<double> s(psd_);
        std::nth_element(s.begin(), s.begin() + N / 2, s.end());
        const double med = s[static_cast<size_t>(N / 2)];
        int bk = -1;
        for (int k = 0; k < N; k++)
            if (std::fabs(bin_hz(k, N)) < EDGE_HZ && (bk < 0 || psd_[static_cast<size_t>(k)] > psd_[static_cast<size_t>(bk)])) bk = k;
        if (bk < 0 || psd_[static_cast<size_t>(bk)] < 100 * med) return NAN;
        return bin_hz(bk, N);
    }

    // the known symbols' spectrum at g Hz from the channel centre (one parity
    // part: lv = all but the middle PN63, mid = the middle PN63), sync at t = 0
    static std::complex<double> dtft(const std::vector<double>& lv, double g) {
        // b_n = level_n (-j)^n: frequency g + RS/4 in the symbols' own frame
        const std::complex<double> w = std::polar(1.0, -2 * M_PI * (g + RS / 4) / RS);
        std::complex<double> s = 0;
        for (int i = KNOWN - 1; i >= 0; i--) s = s * w + lv[static_cast<size_t>(i)];
        return s;
    }
    // the slice's reference spectrum on n bins (sync at t = 0); parity 0 = +mid
    void slice_ref(int n, double centre, std::vector<std::complex<double>>* r0, std::vector<std::complex<double>>* r1,
                   std::vector<double>* wgt) const {
        r0->assign(static_cast<size_t>(n), 0.0);
        if (r1) r1->assign(static_cast<size_t>(n), 0.0);
        wgt->assign(static_cast<size_t>(n), 0.0);
        const double lo = std::max(-EDGE_HZ, centre - HALF_BW), hi = std::min(EDGE_HZ, centre + HALF_BW);
        if (hi - lo < 200e3) return;
        for (int k = 0; k < n; k++) {
            const double f = bin_hz(k, n);
            if (f < lo || f > hi) continue;
            const double g = f - centre;
            const double p = rrc(g);
            if (p < 0.05) continue;
            const std::complex<double> a = dtft(lv_, g), mm = r1 ? dtft(mid_, g) : 0.0;
            (*r0)[static_cast<size_t>(k)] = p * (a + mm);
            if (r1) (*r1)[static_cast<size_t>(k)] = p * (a - mm);
            // Hann over the usable band, the pilot notched (+-3 kHz)
            double w = 0.5 - 0.5 * std::cos(2 * M_PI * (f - lo) / (hi - lo));
            if (pilot_ && std::fabs(g - PILOT) < 3e3) w = 0;
            (*wgt)[static_cast<size_t>(k)] = w;
        }
    }

    // acquisition reference: conj of its spectrum on ACQ bins, without the
    // middle PN63 (either parity), the pilot notched; built from NF bins (the
    // reference is far shorter than NF samples)
    std::vector<kp::cf> acq_;
    void acq_ref(double centre, bool pilot) {
        const bool keep = pilot_;
        pilot_ = pilot;
        std::vector<std::complex<double>> r;
        std::vector<double> w;
        slice_ref(NF, centre, &r, nullptr, &w);
        pilot_ = keep;
        Fft& ti = fft_.get(NF, +1);
        for (int k = 0; k < NF; k++) {
            const std::complex<double> v = r[static_cast<size_t>(k)] * (w[static_cast<size_t>(k)] > 0 ? 1.0 : 0.0);
            ti.data()[k] = kp::cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        ti.run();   // the time-domain reference, circular around t = 0
        Fft& fr = fft_.get(ACQ, -1);
        std::fill(fr.data(), fr.data() + ACQ, kp::cf(0, 0));
        for (int i = 0; i < NF; i++) {
            const int t = i < NF / 2 ? i : i - NF;   // -NF/2 .. NF/2
            fr.data()[(t % ACQ + ACQ) % ACQ] = ti.data()[i] / static_cast<float>(NF);
        }
        fr.run();
        acq_.resize(ACQ);
        for (int k = 0; k < ACQ; k++)
            acq_[static_cast<size_t>(k)] = pilot && std::fabs(bin_hz(k, ACQ) - (centre + PILOT)) < 2e3 ? kp::cf(0, 0) : std::conj(fr.data()[k]);
    }
    // |corr|^2 peak / mean over the 27 ms and its position, the signal d Hz
    // above where the reference expects it
    double acq_corr(const std::vector<kp::cf>& Y, double d, long* q) {
        const int sh = static_cast<int>(std::lround(d * ACQ / RATE));
        Fft& c = fft_.get(ACQ, +1);
        for (int k = 0; k < ACQ; k++) c.data()[k] = Y[static_cast<size_t>((k + sh + ACQ) % ACQ)] * acq_[static_cast<size_t>(k)];
        c.run();
        double mx = 0, sum = 0;
        long at = 0;
        const long lim = ACQ - NF / 2;   // a sync whose whole reference is inside
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

    void build_window_ref(double centre) {
        if (wref_centre_ == centre && !wgt_.empty()) return;
        slice_ref(NF, centre, &wref_[0], &wref_[1], &wgt_);
        double s = 0;
        int c = 0;
        for (int k = 0; k < NF; k++)
            if (wgt_[static_cast<size_t>(k)] > 0) {
                s += std::norm(wref_[0][static_cast<size_t>(k)]);
                c++;
            }
        wlam_ = c ? 0.5 * s / c : 1;     // a bin at half the mean level or below: more matched filter than division
        wref_centre_ = centre;
    }

    // the CIR of one antenna for the field sync at sample p (fractional),
    // shifted by `shift` samples, parity par: P taps, the sync at tap PRE * P / NF
    void cir(const std::vector<kp::cf>& x, double p, int par, double shift, std::vector<kp::cf>* h) {
        const long ws = static_cast<long>(std::floor(p - PRE));
        const double ts = p - static_cast<double>(ws) + shift;   // the sync's time in the window (samples)
        Fft& f = fft_.get(NF, -1);
        std::copy_n(x.data() + ws, NF, f.data());
        f.run();
        Fft& ti = fft_.get(P, +1);
        std::fill(ti.data(), ti.data() + P, kp::cf(0, 0));
        const auto& R = wref_[par];
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
                const std::complex<double> r = R[static_cast<size_t>(k)];
                const std::complex<double> v = std::complex<double>(f.data()[k]) * std::conj(r) * rot * (w / (std::norm(r) + wlam_));
                const int kk = k < NF / 2 ? k : k - NF + P;
                ti.data()[kk] = kp::cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
            }
        }
        ti.run();
        h->assign(ti.data(), ti.data() + P);
    }

    // the strongest tap's power (parity test)
    double peak(const std::vector<kp::cf>& x, double p, int par) {
        std::vector<kp::cf> h;
        cir(x, p, par, 0, &h);
        double mx = 0;
        for (const auto& v : h) mx = std::max(mx, static_cast<double>(std::norm(v)));
        return mx;
    }

    // the shift (taps) of profile b against a, within +-8 taps, parabola-refined
    static double pdp_shift(const std::vector<double>& a, const std::vector<double>& b) {
        constexpr int L = 8;
        double c[2 * L + 1];
        for (int l = -L; l <= L; l++) {
            double s = 0;
            for (int t = 0; t < P; t++) s += a[static_cast<size_t>(t)] * b[static_cast<size_t>(((t + l) % P + P) % P)];
            c[l + L] = s;
        }
        int bl = 0;
        for (int l = -L; l <= L; l++)
            if (c[l + L] > c[bl + L]) bl = l;
        if (bl == -L || bl == L) return bl;
        const double y0 = c[bl + L - 1], y1 = c[bl + L], y2 = c[bl + L + 1], den = y0 - 2 * y1 + y2;
        return bl + (den < 0 ? 0.5 * (y0 - y2) / den : 0);
    }
};

}  // namespace

std::unique_ptr<Estimator> make_atsc(kp::Host& h) { return std::make_unique<AtscEstimator>(h); }

}  // namespace sfn
