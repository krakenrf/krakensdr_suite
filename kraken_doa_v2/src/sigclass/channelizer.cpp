// Signal classifier: the front end (long FFTs, fine PSD, short spectrogram)
// and the overlap-save channelizer that cuts the views from the long FFTs.
//
// Views 0..2: per long block, the M = LONG_N / D bins around the view centre
// times a zero-phase Kaiser low-pass (flat to 0.4 x the view rate, >= 60 dB
// down from 0.5 x), an inverse FFT of M points (= filter + mix + decimate by
// D), the LONG_OV2 / D samples at each end dropped (overlap-save: the filter
// reaches LONG_OV2 input samples), and the block's mixer phase restored so
// the blocks join without a seam. View 3 (the full rate) is the snapshot
// mixed down with an exact table NCO.

#include "sc_internal.hpp"

#include <chrono>
#include <cstring>

namespace sigclass::detail {

namespace {

constexpr double PI = 3.14159265358979323846;

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 200; k++) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

int pos_mod(int64_t a, int64_t m) {
    int64_t r = a % m;
    return int(r < 0 ? r + m : r);
}

}  // namespace

// ---------------------------------------------------------------- front end

void FrontEnd::run(size_t n_samples, double fs_hz, int spec_hop) {
    n = n_samples;
    fs = fs_hz;
    hop = std::max(64, spec_hop);
    long_fft.init(LONG_N, false);
    spec_fft.init(SPEC_N, false);
    tmp.resize(LONG_N);
    tmp2.resize(LONG_N);
    if (w2cum.size() != size_t(LONG_N) + 1) {
        w2cum.assign(size_t(LONG_N) + 1, 0.0);
        for (int i = 0; i < LONG_N; i++) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * PI * double(i) / LONG_N);
            w2cum[size_t(i) + 1] = w2cum[size_t(i)] + w * w;
        }
    }

    nblocks = int((n + LONG_HOP - 1) / LONG_HOP);
    if (nblocks < 1) nblocks = 1;
    blocks.resize(size_t(nblocks) * LONG_N);
    psd.assign(LONG_N, 0.0f);
    acc.assign(LONG_N, 0.0f);
    double wnorm = 0.0;
    for (int b = 0; b < nblocks; b++) {
        const int64_t s0 = int64_t(b) * LONG_HOP - LONG_OV2;
        const int64_t lo = std::max<int64_t>(0, s0), hi = std::min<int64_t>(int64_t(n), s0 + LONG_N);
        std::fill(tmp.begin(), tmp.end(), cf(0.0f, 0.0f));
        if (hi > lo) std::memcpy(tmp.data() + (lo - s0), x.data() + lo, size_t(hi - lo) * sizeof(cf));
        cf* X = blocks.data() + size_t(b) * LONG_N;
        long_fft.fwd(tmp.data(), X);
        // the periodic Hann window's energy over the samples that exist
        if (hi > lo) wnorm += w2cum[size_t(hi - s0)] - w2cum[size_t(lo - s0)];
        // Hann in the frequency domain: Xw[k] = X[k]/2 - (X[k-1] + X[k+1])/4
        {
            const cf w0 = 0.5f * X[0] - 0.25f * (X[LONG_N - 1] + X[1]);
            acc[size_t(LONG_N / 2)] += std::norm(w0);
            const cf wl = 0.5f * X[LONG_N - 1] - 0.25f * (X[LONG_N - 2] + X[0]);
            acc[size_t(LONG_N / 2 - 1)] += std::norm(wl);
        }
        // (two straight halves: the centred index without a branch)
        float* a1 = acc.data() + LONG_N / 2;
        for (int k = 1; k < LONG_N / 2; k++) {
            const cf w = 0.5f * X[k] - 0.25f * (X[k - 1] + X[k + 1]);
            a1[k] += std::norm(w);
        }
        float* a0 = acc.data() - LONG_N / 2;
        for (int k = LONG_N / 2; k < LONG_N - 1; k++) {
            const cf w = 0.5f * X[k] - 0.25f * (X[k - 1] + X[k + 1]);
            a0[k] += std::norm(w);
        }
    }
    const double inv = wnorm > 0.0 ? 1.0 / wnorm : 0.0;
    for (int i = 0; i < LONG_N; i++) psd[size_t(i)] = float(double(acc[size_t(i)]) * inv);

    // short spectrogram: SPEC_N-point Hann frames every hop samples
    const auto ts0 = std::chrono::steady_clock::now();
    if (spec_win.size() != size_t(SPEC_N)) {
        spec_win.resize(SPEC_N);
        for (int i = 0; i < SPEC_N; i++) spec_win[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * PI * i / SPEC_N));
    }
    nframes = n >= size_t(SPEC_N) ? int((n - SPEC_N) / size_t(hop)) + 1 : 0;
    spec.resize(size_t(nframes) * SPEC_N);
    spec_blk.resize(size_t(nframes) * SPEC_BLKS);
    macc.assign(SPEC_N, 0.0f);
    const float wn = 1.0f / (0.375f * SPEC_N);
    for (int t = 0; t < nframes; t++) {
        const cf* src = x.data() + size_t(t) * size_t(hop);
        for (int i = 0; i < SPEC_N; i++) tmp[size_t(i)] = src[i] * spec_win[size_t(i)];
        spec_fft.fwd(tmp.data(), tmp2.data());
        float* row = spec.data() + size_t(t) * SPEC_N;
        const cf* X = tmp2.data();
        for (int k = 0; k < SPEC_N / 2; k++) row[k + SPEC_N / 2] = std::norm(X[k]) * wn;
        for (int k = SPEC_N / 2; k < SPEC_N; k++) row[k - SPEC_N / 2] = std::norm(X[k]) * wn;
        for (int k = 0; k < SPEC_N; k++) macc[size_t(k)] += row[k];
        double* bs = spec_blk.data() + size_t(t) * SPEC_BLKS;
        for (int q = 0; q < SPEC_BLKS; q++) {
            float a = 0.0f;
            for (int k = q * SPEC_BLK; k < (q + 1) * SPEC_BLK; k++) a += row[k];
            bs[q] = a;
        }
    }
    spec_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts0).count();
    spec_mean.assign(SPEC_N, 0.0f);
    if (nframes > 0)
        for (int k = 0; k < SPEC_N; k++) spec_mean[size_t(k)] = macc[size_t(k)] / float(nframes);
}

// --------------------------------------------------------------- channelizer

Channelizer::Channelizer() {
    Fft big(LONG_N, false);
    avec<cf> h(LONG_N), H(LONG_N);
    for (int v = 0; v < 3; v++) {
        const int D = VIEW_DECIM[v];
        const int M = LONG_N / D;
        // Kaiser-windowed sinc, cut-off 0.45 x the view rate, transition
        // 0.4 .. 0.5 x the view rate, 60 dB
        const double A = 60.0, beta = 0.1102 * (A - 8.7);
        const double dw = 2.0 * PI * 0.1 / D;
        int L = int(std::ceil((A - 8.0) / (2.285 * dw))) + 1;
        if (L % 2 == 0) L++;
        L = std::min(L, 2 * LONG_OV2 + 1);
        const int half = (L - 1) / 2;
        const double fc = 0.45 / D;   // cycles per input sample
        std::fill(h.begin(), h.end(), cf(0.0f, 0.0f));
        double sum = 0.0;
        std::vector<double> taps(static_cast<size_t>(L));
        const double i0b = bessel_i0(beta);
        for (int m = -half; m <= half; m++) {
            const double s = m == 0 ? 2.0 * fc : std::sin(2.0 * PI * fc * m) / (PI * m);
            const double r = double(m) / half;
            const double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            taps[size_t(m + half)] = s * w;
            sum += s * w;
        }
        for (int m = -half; m <= half; m++)
            h[size_t(pos_mod(m, LONG_N))] = cf(float(taps[size_t(m + half)] / sum), 0.0f);
        big.fwd(h.data(), H.data());
        h_[v].resize(size_t(M));
        for (int j = -M / 2; j < M / 2; j++) h_[v][size_t(j + M / 2)] = H[size_t(pos_mod(j, LONG_N))].real();
        ifft_[v].init(M, false);
    }
    nco_.resize(LONG_N);
    for (int k = 0; k < LONG_N; k++) {
        const double ph = -2.0 * PI * double(k) / LONG_N;
        nco_[size_t(k)] = cf(float(std::cos(ph)), float(std::sin(ph)));
    }
}

void Channelizer::extract(const FrontEnd& fe, int centre_bin, int view, avec<cf>& out) {
    const size_t n = fe.n;
    if (view < 0 || view >= NUM_VIEWS) view = NUM_VIEWS - 1;
    const int D = VIEW_DECIM[view];
    const size_t nout = n / size_t(D);
    out.resize(nout);
    if (nout == 0) return;
    const int c = std::clamp(centre_bin, -LONG_N / 2, LONG_N / 2 - 1);
    if (view == NUM_VIEWS - 1) {
        const int step = pos_mod(c, LONG_N);
        if (step == 0) {
            std::copy(fe.x.begin(), fe.x.begin() + long(n), out.begin());
            return;
        }
        // e^{-j 2 pi c i / LONG_N} as a block's phasors (exact table values) times the
        // block's start phasor: contiguous products the compiler vectorises
        constexpr size_t B = 4096;
        tmp_.resize(B);
        tmp2_.resize(B);
        for (size_t i = 0; i < B; i++) tmp_[i] = nco_[size_t((int64_t(i) * step) % LONG_N)];
        for (size_t b0 = 0; b0 < n; b0 += B) {
            const cf r0 = nco_[size_t((int64_t(b0 % size_t(LONG_N)) * step) % LONG_N)];
            const size_t m = std::min(B, n - b0);
            for (size_t i = 0; i < m; i++) tmp2_[i] = tmp_[i] * r0;
            const cf* x = fe.x.data() + b0;
            cf* o = out.data() + b0;
            for (size_t i = 0; i < m; i++) o[i] = x[i] * tmp2_[i];
        }
        return;
    }
    const int M = LONG_N / D;
    const int skip = LONG_OV2 / D, keep = LONG_HOP / D;
    tmp_.resize(size_t(M));
    tmp2_.resize(size_t(M));
    const float* H = h_[view].data();
    const float scale = 1.0f / float(LONG_N);
    for (int b = 0; b < fe.nblocks; b++) {
        const cf* X = fe.blocks.data() + size_t(b) * LONG_N;
        // bins c - M/2 .. c + M/2 - 1 (circular) -> IFFT bins in natural order
        int src = pos_mod(int64_t(c) - M / 2, LONG_N);
        for (int j = -M / 2; j < M / 2; j++) {
            tmp_[size_t(j < 0 ? j + M : j)] = X[size_t(src)] * H[j + M / 2];
            if (++src == LONG_N) src = 0;
        }
        ifft_[view].inv(tmp_.data(), tmp2_.data());
        const int64_t s0 = int64_t(b) * LONG_HOP - LONG_OV2;
        const cf rot = nco_[size_t(pos_mod(int64_t(c) * s0, LONG_N))] * scale;
        const size_t o0 = size_t(b) * size_t(keep);
        for (int m = 0; m < keep; m++) {
            const size_t o = o0 + size_t(m);
            if (o >= nout) break;
            out[o] = tmp2_[size_t(skip + m)] * rot;
        }
    }
}

double Channelizer::noise_var(const std::vector<float>& floor_fine, int centre_bin, int view) const {
    if (floor_fine.size() != size_t(LONG_N)) return 0.0;
    double s = 0.0;
    if (view >= NUM_VIEWS - 1) {
        for (float f : floor_fine) s += f;
        return s / LONG_N;
    }
    const int M = LONG_N / VIEW_DECIM[view];
    const float* H = h_[view].data();
    for (int j = -M / 2; j < M / 2; j++) {
        const int i = pos_mod(int64_t(centre_bin) + j + LONG_N / 2, LONG_N);
        s += double(H[j + M / 2]) * H[j + M / 2] * floor_fine[size_t(i)];
    }
    return s / LONG_N;
}

}  // namespace sigclass::detail
