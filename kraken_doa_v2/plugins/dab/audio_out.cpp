// 8 kHz output stage of the DAB audio layer (see audio_out.hpp).

#include "audio_out.hpp"
#include "dab_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dab::audio {

namespace {

constexpr double PI = 3.14159265358979323846;

// modified Bessel function of the first kind, order 0 (Kaiser window)
double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 50; k++) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

}   // namespace

bool Pcm8k::configure(int rate) {
    if (rate < 16000 || rate > 48000 || rate % OUT_RATE != 0) return false;
    rate_ = rate;
    decim_ = rate / OUT_RATE;
    // Kaiser-windowed sinc: pass band to 3.4 kHz, stop band from 4.0 kHz, 60 dB
    const double att = 60.0, f_pass = 3400.0, f_stop = 4000.0;
    const double dw = 2.0 * PI * (f_stop - f_pass) / rate;
    int n = static_cast<int>(std::ceil((att - 7.95) / (2.285 * dw))) + 1;
    // length 2 k D + 1: the group delay is a whole number of output samples
    const int q = 2 * decim_;
    n = ((n - 1 + q - 1) / q) * q + 1;
    const double beta = 0.1102 * (att - 8.7);
    const double fc = 0.5 * (f_pass + f_stop) / rate;   // cycles per sample
    taps_.assign(static_cast<size_t>(n), 0.0f);
    const double m = (n - 1) / 2.0, i0b = bessel_i0(beta);
    double sum = 0;
    std::vector<double> h(static_cast<size_t>(n));
    for (int k = 0; k < n; k++) {
        double t = k - m;
        double s = t == 0 ? 2.0 * fc : std::sin(2.0 * PI * fc * t) / (PI * t);
        double r = t / m;
        double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        h[static_cast<size_t>(k)] = s * w;
        sum += s * w;
    }
    for (int k = 0; k < n; k++) taps_[static_cast<size_t>(k)] = static_cast<float>(h[static_cast<size_t>(k)] / sum);
    x_.assign(taps_.size() - 1 + MAX_CHUNK, 0.0f);
    y_.assign(MAX_CHUNK / 2 + 1, 0.0f);
    work_.assign(HOLD + MAX_CHUNK / 2 + 1, 0.0f);
    skip_ = 0;
    return true;
}

void Pcm8k::reset() {
    std::fill(x_.begin(), x_.end(), 0.0f);
    skip_ = 0;
    tail_n_ = 0;
    fade_in_ = 0;
}

bool Pcm8k::push(const float* in, size_t n, int rate) {
    if (rate != rate_) {
        if (!configure(rate)) { rate_ = 0; return false; }
        tail_n_ = 0;
        fade_in_ = 0;
    }
    const size_t nt = taps_.size(), hist = nt - 1;
    while (n > 0) {
        const size_t c = std::min(n, MAX_CHUNK);
        for (size_t i = 0; i < c; i++) {
            float v = in[i];
            x_[hist + i] = std::isfinite(v) ? std::clamp(v, -4.0f, 4.0f) : 0.0f;
        }
        size_t ny = 0;
        size_t t = hist + skip_;
        const float* h = taps_.data();
        for (; t < hist + c; t += static_cast<size_t>(decim_)) {
            const float* xp = &x_[t + 1 - nt];
            float acc = 0.0f;
            for (size_t k = 0; k < nt; k++) acc += h[k] * xp[k];   // symmetric taps: no reversal needed
            y_[ny++] = acc;
        }
        skip_ = t - (hist + c);
        std::memmove(x_.data(), x_.data() + c, hist * sizeof(float));
        emit(y_.data(), ny);
        in += c;
        n -= c;
    }
    return true;
}

void Pcm8k::emit(const float* y, size_t n) {
    // work = held tail + new samples; the fade-in runs over the first HOLD samples after a gap
    size_t w = 0;
    for (size_t i = 0; i < tail_n_; i++) work_[w++] = tail_[i];
    for (size_t i = 0; i < n; i++) {
        float v = y[i];
        if (fade_in_ < HOLD) v *= static_cast<float>(fade_in_++) / HOLD;
        work_[w++] = std::clamp(v, -1.0f, 1.0f);
    }
    const size_t keep = std::min(w, HOLD);
    const size_t out = w - keep;
    if (out) sink_.pcm(work_.data(), out);
    std::memcpy(tail_, work_.data() + out, keep * sizeof(float));
    tail_n_ = keep;
}

void Pcm8k::emit_zeros(size_t n) {
    static const float zeros[256] = {};
    while (n > 0) {
        size_t c = std::min<size_t>(n, 256);
        sink_.pcm(zeros, c);
        n -= c;
    }
}

void Pcm8k::gap(int ms) {
    if (rate_ == 0) return;
    if (tail_n_) {   // fade the held samples out
        for (size_t i = 0; i < tail_n_; i++) tail_[i] *= static_cast<float>(tail_n_ - i) / static_cast<float>(tail_n_ + 1);
        sink_.pcm(tail_, tail_n_);
        tail_n_ = 0;
    }
    if (ms > 0) emit_zeros(static_cast<size_t>(ms) * OUT_RATE / 1000);
    std::fill(x_.begin(), x_.end(), 0.0f);
    skip_ = 0;
    fade_in_ = 0;
}

}   // namespace dab::audio
