// Separation by CODE for the lora plugin's DoA (multichannel: every antenna
// of the coherent array, kp::Info::multichannel).
//
// LoRa is chirp spread spectrum: a demodulated symbol IS its own code. For
// every symbol the receiver decided (its window, bin, fractional offset - on
// antenna 0's channel), each antenna's 2.4 MHz input is correlated with
// exactly that chirp (a matched filter: the symbol's own frequency sweep,
// incl. its wrap, the channel's offset and the receiver's de-rotation) - y[k]
// = this transmitter's complex amplitude at antenna k. Another LoRa signal in
// the same samples - another SF / BW, or the same SF at another timing /
// offset - stays spread (processing gain 2^SF), noise and every other signal
// in the 2.4 MHz too; the y vectors of a packet's symbols give ITS spatial
// covariance (code_cov), even while other transmitters overlap it in time
// and frequency. A model error (a fraction of a bin off) only lowers the
// correlation - it is common to all antennas, so the direction is unbiased.
//
// The antennas' input is kept for RAW_S in 8 bits per I / Q (the RTL's own
// resolution: kraken_doa's samples are (u8 - 127.5) / 127.5 minus a DC
// estimate) - 21 MB for 5 antennas, enough for the search's probes, which
// decode up to 0.8 s of history.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lora_ds {

using cf = std::complex<float>;

constexpr int RAW_LOG2 = 21;          // 2^21 samples = 0.87 s at 2.4 MHz per antenna
constexpr int MAX_ANT = 8;

// every antenna's input, 8-bit I / Q, indexed like the channelizer's input
// (sample n of the front end since its reset)
struct RawRing {
    int m = 0;                        // antennas held (0 = none: single-channel input)
    int64_t cap = int64_t(1) << RAW_LOG2;
    int64_t abs = 0;                  // input index of the next sample
    int64_t lo = 0;                   // the first one held (start)
    std::vector<uint8_t> buf;         // antenna k: buf[(k * cap + (n & (cap - 1))) * 2 + {0, 1}]

    void off() { m = 0; }
    // start holding m antennas from input index `at`
    void start(int m_, int64_t at) {
        if (m_ != m || buf.empty()) {
            m = m_;
            buf.assign(static_cast<size_t>(m) * static_cast<size_t>(cap) * 2, 128);
        }
        abs = at;
        lo = at;
    }
    void push(const cf* const* x, size_t n) {
        const int64_t mask = cap - 1;
        for (int k = 0; k < m; k++) {
            uint8_t* base = &buf[static_cast<size_t>(k) * static_cast<size_t>(cap) * 2];
            const float* s = reinterpret_cast<const float*>(x[k]);
            size_t i = 0;
            while (i < n) {   // contiguous pieces up to the ring's end
                const int64_t p = (abs + static_cast<int64_t>(i)) & mask;
                const size_t len = std::min(n - i, static_cast<size_t>(cap - p));
                uint8_t* __restrict d = base + 2 * p;
                const float* __restrict v = s + 2 * i;
                // back to the RTL's grid: v x 127.5 + 127.5, rounded (exact for
                // kraken_doa's and the offline host's 8-bit samples)
                for (size_t j = 0; j < 2 * len; j++) {
                    int q = static_cast<int>(v[j] * 127.5f + 128.0f);   // >= 0: rounded
                    q = q < 0 ? 0 : q > 255 ? 255 : q;
                    d[j] = static_cast<uint8_t>(q);
                }
                i += len;
            }
        }
        abs += static_cast<int64_t>(n);
    }
};

// The matched filter of one symbol on every antenna. p = input index (may be
// fractional) of the symbol window's start, ls = the window in input samples
// (2^SF / BW x the input rate), off_c / bw_c = the channel's centre / the
// LoRa bandwidth in cycles per input sample, u = the receiver's bin + its
// fractional de-rotation (the dechirped tone, in bins of BW / 2^SF), n_bins =
// 2^SF, inv = inverted IQ (the receiver reads the channel conjugated). The
// model (the receiver's own, in its view): an up-chirp from -BW/2 + u BW/N,
// wrapping at +BW/2 back by BW, continuous phase. Returns the antennas filled
// (0: not all of the window is held).
inline int despread_sym(const RawRing& r, double p, double ls, double off_c, double bw_c, double u, int n_bins, bool inv,
                        cf* y) {
    if (r.m < 2) return 0;
    const int64_t n0 = static_cast<int64_t>(std::ceil(p - 1e-9));
    const int64_t n1 = static_cast<int64_t>(std::ceil(p + ls - 1e-9)) - 1;   // last sample inside [p, p + ls)
    if (n1 <= n0 || n0 < std::max(r.lo, r.abs - r.cap) || n1 >= r.abs) return 0;
    const double s = inv ? -1.0 : 1.0;
    const double tw = ls * (1.0 - u / n_bins);              // the wrap (input samples after p)
    const double f0 = -0.5 * bw_c + u * bw_c / n_bins;      // cycles / sample at the window start
    const double kq = bw_c / (2.0 * ls);
    // the model's phase (cycles) t samples after p, and its step to t + 1
    auto phase = [&](double t) {
        double v = off_c * t + s * (f0 * t + kq * t * t - (t >= tw ? bw_c * (t - tw) : 0.0));
        return v - std::floor(v);
    };
    auto step = [&](double t, bool wrapped) {
        double v = off_c + s * (f0 + kq * (2.0 * t + 1.0) - (wrapped ? bw_c : 0.0));
        return v - std::floor(v);
    };
    const double dd = s * 2.0 * kq;                         // the step's own step (cycles)
    const cf ddc(static_cast<float>(std::cos(-2 * M_PI * dd)), static_cast<float>(std::sin(-2 * M_PI * dd)));
    constexpr int CH = 128;
    float gr[CH], gi[CH], xr[CH], xi[CH];
    double acc_re[MAX_ANT] = {}, acc_im[MAX_ANT] = {};
    const int64_t mask = r.cap - 1;
    const int m = std::min(r.m, MAX_ANT);
    for (int64_t n = n0; n <= n1;) {
        const double t = static_cast<double>(n) - p;
        const bool wrapped = t >= tw;
        int64_t e = std::min(n1 + 1, n + CH);
        if (!wrapped && tw < static_cast<double>(e) - p) e = std::max(n + 1, static_cast<int64_t>(std::ceil(p + tw - 1e-9)));
        const int64_t idx = n & mask;
        e = std::min(e, n + (r.cap - idx));                 // not across the ring's end
        const int len = static_cast<int>(e - n);
        // the conjugate model, e^{-j 2 pi phase}: recursion within the piece
        const double ph = phase(t), st = step(t, wrapped);
        cf g(static_cast<float>(std::cos(-2 * M_PI * ph)), static_cast<float>(std::sin(-2 * M_PI * ph)));
        cf d(static_cast<float>(std::cos(-2 * M_PI * st)), static_cast<float>(std::sin(-2 * M_PI * st)));
        for (int i = 0; i < len; i++) {
            gr[i] = g.real();
            gi[i] = g.imag();
            g *= d;
            d *= ddc;
        }
        for (int i = len; i < CH; i++) gr[i] = gi[i] = 0;   // a short piece: the lanes past it add nothing
        for (int k = 0; k < m; k++) {
            const uint8_t* q = &r.buf[(static_cast<size_t>(k) * static_cast<size_t>(r.cap) + static_cast<size_t>(idx)) * 2];
            for (int i = 0; i < len; i++) {
                xr[i] = q[2 * i] - 127.5f;
                xi[i] = q[2 * i + 1] - 127.5f;
            }
            for (int i = len; i < CH; i++) xr[i] = xi[i] = 0;
            // 8 independent lanes (no reassociation needed: the compiler can
            // put them in vector registers)
            float sr[8] = {}, si[8] = {};
            for (int i = 0; i < CH; i += 8)
                for (int l = 0; l < 8; l++) {
                    sr[l] += xr[i + l] * gr[i + l] - xi[i + l] * gi[i + l];
                    si[l] += xr[i + l] * gi[i + l] + xi[i + l] * gr[i + l];
                }
            double tr = 0, ti = 0;
            for (int l = 0; l < 8; l++) {
                tr += sr[l];
                ti += si[l];
            }
            acc_re[k] += tr;
            acc_im[k] += ti;
        }
        n = e;
    }
    const double sc = 1.0 / (127.5 * static_cast<double>(n1 - n0 + 1));
    for (int k = 0; k < m; k++) y[k] = cf(static_cast<float>(acc_re[k] * sc), static_cast<float>(acc_im[k] * sc));
    return m;
}

// A packet's spatial covariance from its de-spread symbols (sym_y[i * m + k]):
// symbols whose power is far from the packet's median are left out (a symbol
// decided wrong - the bin held another transmitter's energy, or none), then
// those that don't fit the packet's main direction; R = sum y y^H of the
// rest. coherence = the main eigenvalue / the trace (1 = one clean
// direction, 1/m = noise). false: too few symbols / no clear direction.
struct CovStats {
    int used = 0, total = 0;
    double coherence = 0;
    std::vector<std::complex<double>> u;   // the main eigenvector (unit): the packet's spatial signature
};
inline bool code_cov(int m, const std::vector<cf>& ys, std::vector<cf>& R, CovStats* st = nullptr) {
    if (m < 2 || m > MAX_ANT) return false;
    const size_t n = ys.size() / static_cast<size_t>(m);
    if (st) *st = CovStats{0, static_cast<int>(n), 0};
    if (n < 4) return false;
    std::vector<double> pw(n);
    for (size_t i = 0; i < n; i++) {
        double p = 0;
        for (int k = 0; k < m; k++) p += std::norm(ys[i * static_cast<size_t>(m) + static_cast<size_t>(k)]);
        pw[i] = p;
    }
    std::vector<double> srt = pw;
    std::nth_element(srt.begin(), srt.begin() + static_cast<long>(n / 2), srt.end());
    const double med = srt[n / 2];
    if (!(med > 0)) return false;
    std::vector<char> keep(n, 0);
    for (size_t i = 0; i < n; i++) keep[i] = pw[i] >= 0.25 * med && pw[i] <= 4.0 * med;
    using cd = std::complex<double>;
    auto build = [&](std::vector<cd>& C) {
        C.assign(static_cast<size_t>(m * m), 0.0);
        int c = 0;
        for (size_t i = 0; i < n; i++) {
            if (!keep[i]) continue;
            c++;
            const cf* v = &ys[i * static_cast<size_t>(m)];
            for (int a = 0; a < m; a++)
                for (int b = 0; b < m; b++) C[static_cast<size_t>(a * m + b)] += cd(v[a]) * std::conj(cd(v[b]));
        }
        return c;
    };
    // the main eigenvector (power iteration) and its share of the trace
    auto main_vec = [&](const std::vector<cd>& C, std::vector<cd>& u) {
        u.assign(static_cast<size_t>(m), 0.0);
        int big = 0;
        for (int a = 1; a < m; a++)
            if (C[static_cast<size_t>(a * m + a)].real() > C[static_cast<size_t>(big * m + big)].real()) big = a;
        for (int a = 0; a < m; a++) u[static_cast<size_t>(a)] = C[static_cast<size_t>(a * m + big)];
        double lam = 0;
        std::vector<cd> v(static_cast<size_t>(m));
        for (int it = 0; it < 60; it++) {
            double nn = 0;
            for (int a = 0; a < m; a++) nn += std::norm(u[static_cast<size_t>(a)]);
            nn = std::sqrt(nn);
            if (!(nn > 0)) return 0.0;
            for (auto& x : u) x /= nn;
            for (int a = 0; a < m; a++) {
                v[static_cast<size_t>(a)] = 0;
                for (int b = 0; b < m; b++) v[static_cast<size_t>(a)] += C[static_cast<size_t>(a * m + b)] * u[static_cast<size_t>(b)];
            }
            lam = 0;
            for (int a = 0; a < m; a++) lam += (std::conj(u[static_cast<size_t>(a)]) * v[static_cast<size_t>(a)]).real();
            u.swap(v);
        }
        double nn = 0;
        for (auto& x : u) nn += std::norm(x);
        nn = std::sqrt(nn);
        if (nn > 0)
            for (auto& x : u) x /= nn;
        double tr = 0;
        for (int a = 0; a < m; a++) tr += C[static_cast<size_t>(a * m + a)].real();
        return tr > 0 ? lam / tr : 0.0;
    };
    std::vector<cd> C, u;
    if (build(C) < 4) return false;
    main_vec(C, u);
    // the symbols' fit to that direction: |u^H y|^2 / |y|^2
    std::vector<double> fit(n, 0.0), fs;
    for (size_t i = 0; i < n; i++) {
        if (!keep[i]) continue;
        cd dot = 0;
        for (int k = 0; k < m; k++) dot += std::conj(u[static_cast<size_t>(k)]) * cd(ys[i * static_cast<size_t>(m) + static_cast<size_t>(k)]);
        fit[i] = std::norm(dot) / pw[i];
        fs.push_back(fit[i]);
    }
    std::nth_element(fs.begin(), fs.begin() + static_cast<long>(fs.size() / 2), fs.end());
    const double mfit = fs[fs.size() / 2];
    for (size_t i = 0; i < n; i++)
        if (keep[i] && fit[i] < mfit - 0.3) keep[i] = 0;
    const int used = build(C);
    if (used < 4) return false;
    const double coh = main_vec(C, u);
    if (st) {
        st->used = used;
        st->coherence = coh;
        st->u = u;
    }
    if (coh < 0.4) return false;
    R.resize(static_cast<size_t>(m * m));
    for (size_t i = 0; i < R.size(); i++) R[i] = cf(static_cast<float>(C[i].real()), static_cast<float>(C[i].imag()));
    return true;
}

}  // namespace lora_ds
