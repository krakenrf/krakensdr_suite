#include "aero_acq.hpp"

#include <algorithm>
#include <cmath>

namespace aero {

Acquisition::Acquisition(double fs, std::vector<double> rates_wide, std::vector<double> rates_narrow, double max_off,
                         int navg)
    : fs_(fs), max_off_(max_off) {
    setup(wide_, fs, 16384, navg, std::move(rates_wide));
    setup(narrow_, fs / 2, 16384, navg, std::move(rates_narrow));
    hlp_ = kp::fir_lowpass(31, 4000, static_cast<float>(fs));
    lbuf_.assign(2 * hlp_.size(), cf(0, 0));
    hwide_ = kp::fir_lowpass(21, 7500, static_cast<float>(fs));
    wbuf_.assign(2 * hwide_.size(), cf(0, 0));
    reset();
}

Acquisition::~Acquisition() {
    for (Band* b : {&wide_, &narrow_}) {
        if (b->plan) kfft_destroy_plan(b->plan);
        if (b->in) kfft_free(b->in);
        if (b->out) kfft_free(b->out);
    }
}

void Acquisition::setup(Band& b, double fs, int n, int navg, std::vector<double> rates) {
    b.fs = fs;
    b.n = n;
    b.navg = navg;
    b.rates = std::move(rates);
    if (b.rates.empty()) return;
    b.in = kfft_alloc_complex(n);
    b.out = kfft_alloc_complex(n);
    b.plan = kfft_plan_dft_1d(n, b.in, b.out, KFFT_FORWARD);
    b.acc.assign(n, 0.0f);
    b.sq.assign(n, cf(0, 0));
    b.spec.assign(static_cast<size_t>(navg), std::vector<float>(n, 0.0f));
}

void Acquisition::reset() {
    for (Band* b : {&wide_, &narrow_}) {
        b->fill = 0;
        b->nacc = 0;
        b->since = 0;
        b->nspec = 0;
        b->head = 0;
        b->total = 0;
        b->ready = false;
        std::fill(b->acc.begin(), b->acc.end(), 0.0f);
        std::fill(b->sq.begin(), b->sq.end(), cf(0, 0));
    }
    std::fill(lbuf_.begin(), lbuf_.end(), cf(0, 0));
    std::fill(wbuf_.begin(), wbuf_.end(), cf(0, 0));
    lpos_ = lph_ = wpos_ = 0;
    hits_.clear();
}

double Acquisition::seconds_per_result() const {
    const Band& b = narrow_.rates.empty() ? wide_ : narrow_;
    return b.n * (b.navg + 1) / 2 / b.fs;
}

void Acquisition::push(Band& b, cf v) {
    if (b.rates.empty()) return;
    // the squared signal into a ring of n; every n/2 samples (once n are
    // in) an FFT of the last n, Hann-windowed
    b.sq[static_cast<size_t>(b.fill)] = v * v;
    b.fill = (b.fill + 1) % b.n;
    b.total++;
    if (++b.since < b.n / 2 || b.total < b.n) return;
    b.since = 0;
    for (int i = 0; i < b.n; i++) {
        const float w = 0.5f - 0.5f * std::cos(2 * kp::PI * static_cast<float>(i) / static_cast<float>(b.n));
        const cf s = b.sq[static_cast<size_t>((b.fill + i) % b.n)] * w;
        b.in[i][0] = s.real();
        b.in[i][1] = s.imag();
    }
    kfft_execute(b.plan);
    std::vector<float>& sp = b.spec[static_cast<size_t>(b.head)];
    for (int k = 0; k < b.n; k++) sp[static_cast<size_t>(k)] = b.out[k][0] * b.out[k][0] + b.out[k][1] * b.out[k][1];
    b.head = (b.head + 1) % b.navg;
    b.nspec = std::min(b.nspec + 1, b.navg);
    // the sum of the last nspec spectra
    std::fill(b.acc.begin(), b.acc.end(), 0.0f);
    for (int j = 0; j < b.nspec; j++) {
        const std::vector<float>& s = b.spec[static_cast<size_t>((b.head - 1 - j + b.navg) % b.navg)];
        for (int k = 0; k < b.n; k++) b.acc[static_cast<size_t>(k)] += s[static_cast<size_t>(k)];
    }
    b.nacc = b.nspec;
    search(b);
    b.ready = true;
}

void Acquisition::search(Band& b) {
    const int n = b.n;
    std::vector<float> tmp(b.acc);
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    // the noise level; floored at 2 % of the mean, so that a band with no
    // noise in it (a recording made from receiver audio) doesn't make every
    // ratio huge
    double mean = 0;
    for (float v : b.acc) mean += v;
    mean /= n;
    const float med = std::max({tmp[n / 2], static_cast<float>(0.02 * mean), 1e-30f});
    const double df = b.fs / n;
    auto bin = [&](double f) {   // FFT bin of frequency f (natural order)
        long k = std::lround(f / df);
        return static_cast<int>(((k % n) + n) % n);
    };
    auto pk = [&](int k, int& at) {   // the strongest of the bins around k
        float m = -1;
        for (int d = -1; d <= 1; d++) {
            const int j = ((k + d) % n + n) % n;
            if (b.acc[j] > m) {
                m = b.acc[j];
                at = j;
            }
        }
        return m;
    };
    auto refine = [&](int k) {   // parabolic peak interpolation, Hz
        const float a = b.acc[((k - 1) % n + n) % n], c = b.acc[k], e = b.acc[(k + 1) % n];
        const float den = a - 2 * c + e;
        double d = den != 0 ? 0.5 * (a - e) / den : 0;
        d = std::clamp(d, -0.5, 0.5);
        double f = (k + d) * df;
        if (f >= b.fs / 2) f -= b.fs;
        return f;
    };
    for (double R : b.rates) {
        AcqHit best;
        best.k = b.nacc;
        best.rate = R;
        best.snr_db = -100;
        const double lim = std::min(max_off_, (b.fs / 2 - R / 2) / 2 - 2 * df);
        for (double f2c = -2 * lim; f2c <= 2 * lim; f2c += df) {   // 2 fc in steps of one bin
            int a1 = 0, a2 = 0;
            const float p1 = pk(bin(f2c + R / 2), a1), p2 = pk(bin(f2c - R / 2), a2);
            // both lines well above the floor, scored by their mean
            if (std::min(p1, p2) < 4 * med) continue;
            // and alike: the two lines of A-BPSK / A-QPSK are equal (+- a
            // few dB of noise); off air, a carrier with a strong unmodulated
            // part (another system, 1545.476 MHz on I-4 F1) has one line 20 -
            // 25 dB over everything else, and with the mean as the score it
            // paired with any noise bin R away
            if (std::max(p1, p2) > 6 * std::min(p1, p2)) continue;
            const float sdb = 10 * std::log10(0.5f * (p1 + p2) / med);
            if (sdb > best.snr_db) {
                const double f1 = refine(a1), fm = refine(a2);
                // the two lines must be R apart (+- 2 bins)
                if (std::fabs((f1 - fm) - R) > 2.5 * df) continue;
                best.snr_db = sdb;
                best.f0 = (f1 + fm) / 4;
            }
        }
        // replace this rate's earlier result
        bool found = false;
        for (auto& h : hits_)
            if (h.rate == R) {
                h = best;
                found = true;
            }
        if (!found) hits_.push_back(best);
    }
}

bool Acquisition::process(const cf* x, size_t n) {
    bool any = false;
    const size_t nl = hlp_.size();
    const size_t nw = hwide_.size();
    for (size_t i = 0; i < n; i++) {
        const cf v = x[i];
        // wide search behind a +-7 kHz low-pass (the 10.5 kbit/s signal
        // and +-2.5 kHz of offset): less noise into the squaring
        wbuf_[wpos_] = wbuf_[wpos_ + nw] = v;
        wpos_ = (wpos_ + 1) % static_cast<int>(nw);
        {
            cf acc(0, 0);
            const cf* p = &wbuf_[wpos_];
            for (size_t k = 0; k < nw; k++) acc += p[k] * hwide_[nw - 1 - k];
            push(wide_, acc);
        }
        if (!narrow_.rates.empty()) {
            lbuf_[lpos_] = lbuf_[lpos_ + nl] = v;
            lpos_ = (lpos_ + 1) % static_cast<int>(nl);
            if (++lph_ == 2) {
                lph_ = 0;
                cf acc(0, 0);
                const cf* p = &lbuf_[lpos_];
                for (size_t k = 0; k < nl; k++) acc += p[k] * hlp_[nl - 1 - k];
                push(narrow_, acc);
            }
        }
        if (wide_.ready || narrow_.ready) {
            wide_.ready = narrow_.ready = false;
            any = true;
        }
    }
    return any;
}

}  // namespace aero
