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
}

void Acquisition::reset() {
    for (Band* b : {&wide_, &narrow_}) {
        b->fill = 0;
        b->nacc = 0;
        b->ready = false;
        std::fill(b->acc.begin(), b->acc.end(), 0.0f);
    }
    std::fill(lbuf_.begin(), lbuf_.end(), cf(0, 0));
    lpos_ = lph_ = 0;
    hits_.clear();
}

double Acquisition::seconds_per_result() const {
    const Band& b = narrow_.rates.empty() ? wide_ : narrow_;
    return b.n * b.navg / b.fs;
}

void Acquisition::push(Band& b, cf v) {
    if (b.rates.empty()) return;
    // Hann window on the squared signal
    const int i = b.fill;
    const float w = 0.5f - 0.5f * std::cos(2 * kp::PI * static_cast<float>(i) / static_cast<float>(b.n));
    const cf s = v * v * w;
    b.in[i][0] = s.real();
    b.in[i][1] = s.imag();
    if (++b.fill < b.n) return;
    b.fill = 0;
    kfft_execute(b.plan);
    for (int k = 0; k < b.n; k++) b.acc[k] += b.out[k][0] * b.out[k][0] + b.out[k][1] * b.out[k][1];
    if (++b.nacc < b.navg) return;
    search(b);
    b.nacc = 0;
    std::fill(b.acc.begin(), b.acc.end(), 0.0f);
    b.ready = true;
}

void Acquisition::search(Band& b) {
    const int n = b.n;
    std::vector<float> tmp(b.acc);
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    const float med = std::max(tmp[n / 2], 1e-30f);
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
        best.rate = R;
        best.snr_db = -100;
        const double lim = std::min(max_off_, (b.fs / 2 - R / 2) / 2 - 2 * df);
        for (double f2c = -2 * lim; f2c <= 2 * lim; f2c += df) {   // 2 fc in steps of one bin
            int a1 = 0, a2 = 0;
            const float p1 = pk(bin(f2c + R / 2), a1), p2 = pk(bin(f2c - R / 2), a2);
            const float s = std::min(p1, p2);
            const float sdb = 10 * std::log10(s / med);
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
    for (size_t i = 0; i < n; i++) {
        const cf v = x[i];
        push(wide_, v);
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
