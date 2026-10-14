#pragma once

// LTE / NR OFDM numerology at the plugin's input rate. 2.4 MHz is 160
// samples per 15 kHz symbol (80 at 30 kHz), so no resampling is needed - but
// the cyclic prefixes are fractional there (11.25 / 12.5 samples): an FFT
// window starts at a whole sample and the fraction is put back as a phase
// ramp (SymFft).

#include "dsp_util.hpp"

#include <cmath>
#include <unordered_map>
#include <vector>

namespace cells {

struct Numerology {
    double fs = 2.4e6;
    int mu = 0;                                     // 0 = 15 kHz (LTE, NR case A), 1 = 30 kHz
    double scs() const { return 15e3 * (1 << mu); }
    int nfft() const { return static_cast<int>(std::lround(fs / scs())); }
    // the subcarrier spacing must be a whole number of FFT bins
    bool usable() const { return fs > 0 && std::fabs(fs / scs() - nfft()) < 1e-6 && nfft() >= 64; }
    double ts() const { return fs / 30.72e6; }      // input samples per LTE basic time unit Ts
    int syms_per_half() const { return 7 << mu; }   // symbols per 0.5 ms
    double half_ms() const { return 0.5e-3 * fs; }
    double cp() const { return (144 >> mu) * ts(); }   // a normal cyclic prefix
    double sym_len() const { return ((2048 >> mu) + (144 >> mu)) * ts(); }
    // start of symbol l's cyclic prefix within a 0.5 ms half-subframe (the
    // first symbol of each 0.5 ms has a cyclic prefix 16 Ts longer)
    double sym_start(int l) const {
        const double N = 2048 >> mu, c = 144 >> mu;
        return (l == 0 ? 0.0 : (N + c + 16) + (l - 1) * (N + c)) * ts();
    }
    // start of its useful part
    double useful(int l) const { return sym_start(l) + ((144 >> mu) + (l == 0 ? 16 : 0)) * ts(); }
};

// The spectrum of one OFDM symbol: Y[k + K] for subcarriers k = -K..K of the
// symbol whose useful part starts at input sample s (fractional). The FFT
// window starts `early` samples before s (inside the cyclic prefix, so
// precursors and timing errors don't reach into the previous symbol); the
// result is referred to s (the early window's phase ramp put back): a path
// arriving exactly at s has a flat phase.
class SymFft {
public:
    SymFft(int nfft, int K) : n_(nfft), K_(K), fft_(nfft, -1) {}
    int K() const { return K_; }
    bool run(const cf* x, size_t len, double s, double early, cf* Y) {
        const long i0 = static_cast<long>(std::floor(s - early));
        if (i0 < 0 || i0 + n_ > static_cast<long>(len)) return false;
        std::copy(x + i0, x + i0 + n_, fft_.data());
        fft_.run();
        const std::vector<cf>& r = ramp(s - static_cast<double>(i0));
        const cf* F = fft_.data();
        for (int k = -K_; k <= K_; k++) Y[k + K_] = F[(k + n_) % n_] * r[static_cast<size_t>(k + K_)];
        return true;
    }

private:
    int n_, K_;
    Fft fft_;
    std::unordered_map<long, std::vector<cf>> ramps_;
    // e^{j 2 pi k d / N} (d = s - window start); d comes in multiples of
    // 1/64 sample at 2.4 MHz - a small cache
    const std::vector<cf>& ramp(double d) {
        const long key = std::lround(d * 1024);
        auto it = ramps_.find(key);
        if (it != ramps_.end()) return it->second;
        if (ramps_.size() > 4096) ramps_.clear();
        std::vector<cf> r(static_cast<size_t>(2 * K_ + 1));
        for (int k = -K_; k <= K_; k++)
            r[static_cast<size_t>(k + K_)] = std::polar(1.0f, static_cast<float>(2 * M_PI * k * (static_cast<double>(key) / 1024) / n_));
        return ramps_.emplace(key, std::move(r)).first->second;
    }
};

}  // namespace cells
