// FFT channelizer for the lora plugin: ONE forward FFT of the 2.4 MHz input
// (overlap-save, 4608 points, hop 3456) feeds every watched channel; each
// channel takes the bins around its centre through a flat / raised-cosine
// frequency response and an inverse FFT of 2 x BW / 2.4 MHz of the size -
// so its output is at EXACTLY 2 x BW (the CSS receiver's hop grid must be a
// whole symbol), zero-phase (no delay: output sample i = input time i /
// (2 BW)). The centre's offset from the bin grid (< 260 Hz) is mixed out at
// the output rate. It replaced a per-channel mixer + decimating FIR + cubic
// interpolator at 2.4 MHz (~3.5 % of a Pi 5 core per channel; the AU915
// window around 927.5 MHz watches 12 channels).
#pragma once
#include <complex>
#include <cmath>
#include <cstring>
#include <vector>

#include "kraken_fft.hpp"

namespace lora_chan {

using cf = std::complex<float>;

constexpr int NFFT = 4608;            // 2.4 MHz / 4608 = 520.83 Hz bins; 2 BW / 2.4 MHz x 4608 is whole for BW 125 / 250 / 500 kHz
constexpr int OVL = 1152;             // >> the channel filters' impulse response
constexpr int HOP = NFFT - OVL;

// The shared front end: collects HOP new samples, runs the forward FFT
struct Front {
    kfft_complex* in = nullptr;
    kfft_complex* X = nullptr;
    kfft_plan plan = nullptr;
    int fill = 0;
    int64_t blocks = 0;
    Front() {
        in = kfft_alloc_complex(NFFT);
        X = kfft_alloc_complex(NFFT);
        std::memset(in, 0, sizeof(kfft_complex) * NFFT);
        plan = kfft_plan_dft_1d(NFFT, in, X, KFFT_FORWARD);
    }
    ~Front() {
        kfft_destroy_plan(plan);
        kfft_free(in);
        kfft_free(X);
    }
    Front(const Front&) = delete;
    Front& operator=(const Front&) = delete;
    void reset() {
        std::memset(in, 0, sizeof(kfft_complex) * NFFT);
        fill = 0;
        blocks = 0;
    }
    // push input; on_block() is called for every finished FFT (X valid)
    template <class F> void push(const cf* x, size_t n, F&& on_block) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, static_cast<size_t>(HOP - fill));
            std::memcpy(in + OVL + fill, x + i, take * sizeof(kfft_complex));
            fill += static_cast<int>(take);
            i += take;
            if (fill == HOP) {
                kfft_execute(plan);
                on_block();
                std::memmove(in, in + HOP, OVL * sizeof(kfft_complex));
                fill = 0;
                blocks++;
            }
        }
    }
};

// the oversampling that gives a whole number of output samples per block
// (M = NFFT x os x bw / fs must be a multiple of 4: OVL and HOP are 1/4 and
// 3/4 of NFFT)
inline int os_for(double fs, double bw) {
    for (int os = 2; os <= 16; os *= 2) {
        const double m = NFFT * os * bw / fs;
        const long mi = std::lround(m);
        if (std::fabs(m - mi) < 1e-3 && mi % 4 == 0) return os;
    }
    return 0;
}

// One channel: centre offset from the input's 0 Hz, output rate os x bw
struct Chan {
    int M = 0, k0 = 0, keep0 = 0;
    std::vector<float> G;             // response per output bin, FFT order
    kfft_complex* Y = nullptr;
    kfft_complex* y = nullptr;
    kfft_plan plan = nullptr;
    std::complex<double> blk_ph{1, 0}, blk_rot{1, 0};   // the carrier's phase at each block start
    std::complex<double> nco{1, 0}, nco_rot{1, 0};      // residual (off-grid) offset, at the output rate
    int renorm = 0;
    ~Chan() { release(); }
    void release() {
        if (plan) kfft_destroy_plan(plan);
        if (Y) kfft_free(Y);
        if (y) kfft_free(y);
        plan = nullptr;
        Y = y = nullptr;
    }
    // output rate os x bw (os 2; the caller passes 4 where 2 x bw isn't a
    // whole multiple of 4 bins - 7.8 kHz: `os_for`)
    void init(double fs, double bw, double off_hz, int os = 2) {
        release();
        M = static_cast<int>(std::lround(NFFT * os * bw / fs));
        keep0 = OVL * M / NFFT;
        const double df = fs / NFFT;
        k0 = static_cast<int>(std::lround(off_hz / df));
        const double resid = off_hz - k0 * df;
        G.assign(static_cast<size_t>(M), 0.f);
        for (int m = 0; m < M; m++) {
            const int mm = m < M / 2 ? m : m - M;          // FFT order -> signed bin
            const double f = std::fabs(mm * df);
            const double a = 0.52 * bw, b = 0.75 * bw;
            double g = f <= a ? 1.0 : f >= b ? 0.0 : 0.5 * (1 + std::cos(M_PI * (f - a) / (b - a)));
            G[static_cast<size_t>(m)] = static_cast<float>(g / NFFT);   // the FFT is unnormalised
        }
        Y = kfft_alloc_complex(M);
        y = kfft_alloc_complex(M);
        plan = kfft_plan_dft_1d(M, Y, y, KFFT_BACKWARD);
        blk_ph = {1, 0};
        blk_rot = std::polar(1.0, -2 * M_PI * static_cast<double>(k0) * HOP / NFFT);
        nco = {1, 0};
        nco_rot = std::polar(1.0, -2 * M_PI * resid / (os * bw));
        renorm = 0;
    }
    // one block of the front end -> HOP x M / NFFT output samples
    void block(const kfft_complex* X, std::vector<cf>& out) {
        for (int m = 0; m < M; m++) {
            const int mm = m < M / 2 ? m : m - M;
            const int k = ((k0 + mm) % NFFT + NFFT) % NFFT;
            const float g = G[static_cast<size_t>(m)];
            Y[m][0] = X[k][0] * g;
            Y[m][1] = X[k][1] * g;
        }
        kfft_execute(plan);
        // the IFFT's time origin is the block start: rotate by the carrier's
        // phase there (as a mixer running since the first sample would)
        const std::complex<double> bp = blk_ph * (OVL ? std::polar(1.0, -2 * M_PI * static_cast<double>(k0) * (-OVL) / NFFT) : 1.0);
        const cf bpf(static_cast<float>(bp.real()), static_cast<float>(bp.imag()));
        for (int j = keep0; j < M; j++) {
            const cf v = cf(y[j][0], y[j][1]) * bpf;
            out.push_back(v * cf(static_cast<float>(nco.real()), static_cast<float>(nco.imag())));
            nco *= nco_rot;
        }
        blk_ph *= blk_rot;
        if (++renorm == 64) {
            renorm = 0;
            blk_ph /= std::abs(blk_ph);
            nco /= std::abs(nco);
        }
    }
};

}  // namespace lora_chan
