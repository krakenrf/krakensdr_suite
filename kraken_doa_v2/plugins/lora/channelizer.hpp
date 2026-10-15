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

#include <fftw3.h>

namespace lora_chan {

using cf = std::complex<float>;

constexpr int NFFT = 4608;            // 2.4 MHz / 4608 = 520.83 Hz bins; 2 BW / 2.4 MHz x 4608 is whole for BW 125 / 250 / 500 kHz
constexpr int OVL = 1152;             // >> the channel filters' impulse response
constexpr int HOP = NFFT - OVL;

// The shared front end: collects HOP new samples, runs the forward FFT
struct Front {
    fftwf_complex* in = nullptr;
    fftwf_complex* X = nullptr;
    fftwf_plan plan = nullptr;
    int fill = 0;
    int64_t blocks = 0;
    Front() {
        in = fftwf_alloc_complex(NFFT);
        X = fftwf_alloc_complex(NFFT);
        std::memset(in, 0, sizeof(fftwf_complex) * NFFT);
        plan = fftwf_plan_dft_1d(NFFT, in, X, FFTW_FORWARD, FFTW_ESTIMATE);
    }
    ~Front() {
        fftwf_destroy_plan(plan);
        fftwf_free(in);
        fftwf_free(X);
    }
    Front(const Front&) = delete;
    Front& operator=(const Front&) = delete;
    void reset() {
        std::memset(in, 0, sizeof(fftwf_complex) * NFFT);
        fill = 0;
        blocks = 0;
    }
    // push input; on_block() is called for every finished FFT (X valid)
    template <class F> void push(const cf* x, size_t n, F&& on_block) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, static_cast<size_t>(HOP - fill));
            std::memcpy(in + OVL + fill, x + i, take * sizeof(fftwf_complex));
            fill += static_cast<int>(take);
            i += take;
            if (fill == HOP) {
                fftwf_execute(plan);
                on_block();
                std::memmove(in, in + HOP, OVL * sizeof(fftwf_complex));
                fill = 0;
                blocks++;
            }
        }
    }
};

// One channel: centre offset from the input's 0 Hz, output rate 2 bw
struct Chan {
    int M = 0, k0 = 0, keep0 = 0;
    std::vector<float> G;             // response per output bin, FFT order
    fftwf_complex* Y = nullptr;
    fftwf_complex* y = nullptr;
    fftwf_plan plan = nullptr;
    std::complex<double> blk_ph{1, 0}, blk_rot{1, 0};   // the carrier's phase at each block start
    std::complex<double> nco{1, 0}, nco_rot{1, 0};      // residual (off-grid) offset, at the output rate
    int renorm = 0;
    ~Chan() { release(); }
    void release() {
        if (plan) fftwf_destroy_plan(plan);
        if (Y) fftwf_free(Y);
        if (y) fftwf_free(y);
        plan = nullptr;
        Y = y = nullptr;
    }
    void init(double fs, double bw, double off_hz) {
        release();
        M = static_cast<int>(std::lround(NFFT * 2 * bw / fs));
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
            G[static_cast<size_t>(m)] = static_cast<float>(g / NFFT);   // FFTW is unnormalised
        }
        Y = fftwf_alloc_complex(M);
        y = fftwf_alloc_complex(M);
        plan = fftwf_plan_dft_1d(M, Y, y, FFTW_BACKWARD, FFTW_ESTIMATE);
        blk_ph = {1, 0};
        blk_rot = std::polar(1.0, -2 * M_PI * static_cast<double>(k0) * HOP / NFFT);
        nco = {1, 0};
        nco_rot = std::polar(1.0, -2 * M_PI * resid / (2 * bw));
        renorm = 0;
    }
    // one block of the front end -> HOP x M / NFFT output samples
    void block(const fftwf_complex* X, std::vector<cf>& out) {
        for (int m = 0; m < M; m++) {
            const int mm = m < M / 2 ? m : m - M;
            const int k = ((k0 + mm) % NFFT + NFFT) % NFFT;
            const float g = G[static_cast<size_t>(m)];
            Y[m][0] = X[k][0] * g;
            Y[m][1] = X[k][1] * g;
        }
        fftwf_execute(plan);
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
