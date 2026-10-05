#pragma once

// Header-only DSP / bit helpers for decoder plugins. Plain C++ (no external
// dependencies) so a plugin stays readable; liquid-dsp (<liquid/liquid.h>)
// and FFTW3 (<fftw3.h>, float API fftwf_*) are linked as well for anything
// heavier. Conventions: bits are UNPACKED (one bit per uint8_t, 0/1, first
// transmitted bit first) unless a function says otherwise.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace kp {

using cf = std::complex<float>;
constexpr float PI = 3.14159265358979f;

// --- Filter design --------------------------------------------------------------
// Windowed-sinc low-pass (Blackman), unity DC gain. cutoff_hz = -6 dB point.
inline std::vector<float> fir_lowpass(int ntaps, float cutoff_hz, float fs) {
    std::vector<float> h(ntaps);
    const float fc = cutoff_hz / fs;
    const float m = (ntaps - 1) / 2.0f;
    float sum = 0;
    for (int i = 0; i < ntaps; i++) {
        float t = i - m;
        float s = t == 0 ? 2 * fc : std::sin(2 * PI * fc * t) / (PI * t);
        float w = 0.42f - 0.5f * std::cos(2 * PI * i / (ntaps - 1)) + 0.08f * std::cos(4 * PI * i / (ntaps - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (float& v : h) v /= sum;
    return h;
}

// Root-raised-cosine, sps samples/symbol, span symbols each side, unity DC gain
inline std::vector<float> fir_rrc(int sps, int span, float beta) {
    int n = 2 * span * sps + 1;
    std::vector<float> h(n);
    float sum = 0;
    for (int i = 0; i < n; i++) {
        float t = static_cast<float>(i - span * sps) / sps;
        float v;
        if (std::fabs(t) < 1e-6f) v = 1 - beta + 4 * beta / PI;
        else if (beta > 0 && std::fabs(std::fabs(t) - 1 / (4 * beta)) < 1e-6f)
            v = beta / std::sqrt(2.0f) * ((1 + 2 / PI) * std::sin(PI / (4 * beta)) + (1 - 2 / PI) * std::cos(PI / (4 * beta)));
        else
            v = (std::sin(PI * t * (1 - beta)) + 4 * beta * t * std::cos(PI * t * (1 + beta))) /
                (PI * t * (1 - 16 * beta * beta * t * t));
        h[i] = v;
        sum += v;
    }
    for (float& v : h) v /= sum;
    return h;
}

// Gaussian pulse (GMSK / GFSK), bt = bandwidth-time product, unity DC gain
inline std::vector<float> fir_gaussian(int sps, int span, float bt) {
    int n = 2 * span * sps + 1;
    std::vector<float> h(n);
    float a = std::sqrt(std::log(2.0f) / 2) / bt, sum = 0;
    for (int i = 0; i < n; i++) {
        float t = static_cast<float>(i - span * sps) / sps;
        h[i] = std::exp(-(PI * t / a) * (PI * t / a));
        sum += h[i];
    }
    for (float& v : h) v /= sum;
    return h;
}

// Moving-average (integrate & dump) taps
inline std::vector<float> fir_boxcar(int n) { return std::vector<float>(n, 1.0f / n); }

// FIR filter with real taps over T = float or cf
template <typename T>
class Fir {
public:
    Fir() = default;
    explicit Fir(std::vector<float> taps) { set_taps(std::move(taps)); }
    void set_taps(std::vector<float> taps) {
        h_ = std::move(taps);
        buf_.assign(2 * h_.size(), T{});
        pos_ = 0;
    }
    T push(T x) {   // returns the filtered sample
        const size_t n = h_.size();
        if (n == 0) return x;
        buf_[pos_] = buf_[pos_ + n] = x;   // doubled ring: contiguous window
        pos_ = (pos_ + 1) % n;
        T acc{};
        const T* w = &buf_[pos_];          // oldest .. newest
        for (size_t i = 0; i < n; i++) acc += w[i] * h_[n - 1 - i];
        return acc;
    }
    void reset() { std::fill(buf_.begin(), buf_.end(), T{}); pos_ = 0; }
    size_t delay() const { return h_.empty() ? 0 : (h_.size() - 1) / 2; }
private:
    std::vector<float> h_;
    std::vector<T> buf_;
    size_t pos_ = 0;
};

// --- Demodulators -----------------------------------------------------------------
// FM discriminator: instantaneous frequency in Hz
class FmDemod {
public:
    explicit FmDemod(float fs = 48000) : k_(fs / (2 * PI)) {}
    float push(cf x) {
        float f = std::arg(x * std::conj(prev_)) * k_;
        prev_ = x;
        return f;
    }
    void reset() { prev_ = cf(1, 0); }
private:
    float k_;
    cf prev_{1, 0};
};

// Numerically controlled oscillator / mixer: shift a signal by -hz
// (mix(x) moves a component at +hz down to 0 Hz)
class Nco {
public:
    Nco(float hz = 0, float fs = 48000) { set(hz, fs); }
    void set(float hz, float fs) { w_ = -2.0 * M_PI * hz / fs; }
    cf mix(cf x) {
        cf r = x * cf(static_cast<float>(std::cos(ph_)), static_cast<float>(std::sin(ph_)));
        ph_ = std::remainder(ph_ + w_, 2.0 * M_PI);
        return r;
    }
private:
    double w_ = 0, ph_ = 0;
};

// Goertzel tone detector over blocks of n samples (real input)
class Goertzel {
public:
    Goertzel(float hz, float fs, int n) : n_(n), c_(2 * std::cos(2 * PI * hz / fs)) {}
    // returns true when a block completed; power() is then valid
    bool push(float x) {
        float s = x + c_ * s1_ - s2_;
        s2_ = s1_; s1_ = s;
        if (++k_ < n_) return false;
        p_ = (s1_ * s1_ + s2_ * s2_ - c_ * s1_ * s2_) / (n_ * n_);
        s1_ = s2_ = 0; k_ = 0;
        return true;
    }
    float power() const { return p_; }
private:
    int n_, k_ = 0;
    float c_, s1_ = 0, s2_ = 0, p_ = 0;
};

// --- Symbol timing ------------------------------------------------------------------
// Zero-crossing clock recovery for 2/4-level FSK on a FILTERED discriminator
// (or any baseband PAM). push() returns true when a symbol was sampled
// (value in symbol()). Timing is pulled so level transitions fall half way
// between sampling instants. centre() tracks the signal's DC (carrier
// offset in Hz on a discriminator) from the sampled symbols.
class ClockRecovery {
public:
    ClockRecovery(float sps = 10, float gain = 0.05f) : sps_(sps), gain_(gain) {}
    void set_sps(float sps) { sps_ = sps; }
    bool push(float x) {
        bool out = false;
        float xc = x - centre_;
        // transition: the ideal crossing sits at phase 0.5
        if ((xc > 0) != (prev_ > 0) && have_prev_) {
            // fractional crossing position
            float frac = prev_ / (prev_ - xc);   // 0..1 from the previous sample
            float ph_cross = phase_ + frac / sps_;
            if (ph_cross >= 1) ph_cross -= 1;
            phase_ -= gain_ * (ph_cross - 0.5f);
        }
        phase_ += 1.0f / sps_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
            sym_ = x;
            // slow DC tracker on the decided symbols (works for balanced data)
            centre_ += 0.002f * (x - centre_);
            out = true;
        }
        prev_ = xc;
        have_prev_ = true;
        return out;
    }
    float symbol() const { return sym_; }        // raw sampled value
    float centre() const { return centre_; }
    void set_centre(float c) { centre_ = c; }
    void reset() { phase_ = 0; prev_ = 0; have_prev_ = false; centre_ = 0; }
private:
    float sps_, gain_;
    float phase_ = 0, prev_ = 0, sym_ = 0, centre_ = 0;
    bool have_prev_ = false;
};

// --- Bits ---------------------------------------------------------------------------
inline int popcount64(uint64_t v) { return __builtin_popcountll(v); }

// MSB-first unpacked bits -> integer (n <= 64)
inline uint64_t bits_to_uint(const uint8_t* b, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[i] & 1);
    return v;
}
inline void uint_to_bits(uint64_t v, int n, uint8_t* out) {
    for (int i = 0; i < n; i++) out[i] = (v >> (n - 1 - i)) & 1;
}
// Unpacked bits -> bytes (MSB first); nbits rounded up
inline std::vector<uint8_t> pack_bits(const uint8_t* b, int nbits) {
    std::vector<uint8_t> out((nbits + 7) / 8, 0);
    for (int i = 0; i < nbits; i++) out[i / 8] |= (b[i] & 1) << (7 - i % 8);
    return out;
}

// Sliding sync-word detector on a bit stream (pattern up to 64 bits).
// push(bit) returns the Hamming distance of the last nbits to the pattern;
// also checks the inverted pattern via inverted_distance().
class SyncWord {
public:
    SyncWord(uint64_t pattern, int nbits)
        : pat_(pattern), n_(nbits), mask_(nbits >= 64 ? ~0ull : ((1ull << nbits) - 1)) {}
    int push(uint8_t bit) {
        reg_ = ((reg_ << 1) | (bit & 1)) & mask_;
        if (count_ < n_) count_++;
        return count_ < n_ ? n_ : popcount64(reg_ ^ pat_);
    }
    int inverted_distance() const { return count_ < n_ ? n_ : popcount64(reg_ ^ (~pat_ & mask_)); }
    uint64_t reg() const { return reg_; }
    void reset() { reg_ = 0; count_ = 0; }
private:
    uint64_t pat_;
    int n_;
    uint64_t mask_, reg_ = 0;
    int count_ = 0;
};

// --- CRC ------------------------------------------------------------------------------
// Generic MSB-first CRC over unpacked bits (width <= 32)
inline uint32_t crc_bits(const uint8_t* bits, int nbits, int width, uint32_t poly, uint32_t init = 0,
                         uint32_t xorout = 0) {
    const uint32_t top = 1u << (width - 1);
    const uint32_t mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
    uint32_t r = init & mask;
    for (int i = 0; i < nbits; i++) {
        bool fb = ((r & top) != 0) ^ (bits[i] & 1);
        r = (r << 1) & mask;
        if (fb) r ^= poly;
    }
    return (r ^ xorout) & mask;
}
// Byte-wise CRC with reflection options (CRC catalogue parameters)
inline uint32_t crc_bytes(const uint8_t* data, size_t len, int width, uint32_t poly, uint32_t init,
                          bool refin, bool refout, uint32_t xorout) {
    auto reflect = [](uint32_t v, int n) {
        uint32_t r = 0;
        for (int i = 0; i < n; i++) r |= ((v >> i) & 1) << (n - 1 - i);
        return r;
    };
    const uint32_t top = 1u << (width - 1);
    const uint32_t mask = width == 32 ? 0xFFFFFFFFu : ((1u << width) - 1);
    uint32_t r = init & mask;
    for (size_t k = 0; k < len; k++) {
        uint8_t byte = refin ? static_cast<uint8_t>(reflect(data[k], 8)) : data[k];
        for (int i = 7; i >= 0; i--) {
            bool fb = ((r & top) != 0) ^ ((byte >> i) & 1);
            r = (r << 1) & mask;
            if (fb) r ^= poly;
        }
    }
    if (refout) r = reflect(r, width);
    return (r ^ xorout) & mask;
}

// --- Block codes ----------------------------------------------------------------------
// Any (n,k) block code with k <= 20, decoded by nearest-codeword search
// (maximum likelihood; fine for a few hundred words per second). Codeword
// layout: first transmitted bit = bit (n-1). enc(msg) -> codeword.
class BlockCode {
public:
    template <typename Enc>
    BlockCode(int n, int k, Enc enc) : n_(n), k_(k) {
        cw_.resize(size_t(1) << k);
        for (uint32_t m = 0; m < cw_.size(); m++) cw_[m] = enc(m);
    }
    // corrected bit count, or -1 if more than max_errors; *msg = message
    int decode(uint32_t rx, uint32_t* msg, int max_errors) const {
        int best = 99;
        uint32_t bm = 0;
        for (uint32_t m = 0; m < cw_.size(); m++) {
            int d = __builtin_popcount(cw_[m] ^ rx);
            if (d < best) { best = d; bm = m; if (!d) break; }
        }
        *msg = bm;
        return best <= max_errors ? best : -1;
    }
    uint32_t encode(uint32_t m) const { return cw_[m]; }
private:
    int n_, k_;
    std::vector<uint32_t> cw_;
};

// Remainder of a polynomial division (systematic cyclic / BCH codes):
// returns data * x^(deg) mod g, g given with its top bit (e.g. 0x769 for
// POCSAG's BCH(31,21) x^10+x^9+x^8+x^6+x^5+x^3+1)
inline uint32_t poly_mod(uint64_t data, int data_bits, uint32_t g, int deg) {
    uint64_t r = data << deg;
    for (int i = data_bits + deg - 1; i >= deg; i--)
        if (r & (1ull << i)) r ^= static_cast<uint64_t>(g) << (i - deg);
    return static_cast<uint32_t>(r & ((1ull << deg) - 1));
}

// --- Convolutional codes ------------------------------------------------------------------
// Rate 1/R convolutional encoder; polys = tap masks over (input, D, D^2, ...)
// with bit 0 = input
inline std::vector<uint8_t> conv_encode(const uint8_t* bits, int n, int K, const std::vector<uint32_t>& polys) {
    std::vector<uint8_t> out;
    uint32_t reg = 0, mask = (1u << K) - 1;
    for (int i = 0; i < n; i++) {
        reg = ((reg << 1) | (bits[i] & 1)) & mask;
        for (uint32_t p : polys) out.push_back(__builtin_popcount(reg & p) & 1);
    }
    return out;
}
// Soft-decision Viterbi. soft[i] per coded bit: > 0 = "0", < 0 = "1",
// 0 = erasure (punctured). Returns nout decoded bits. terminated = the
// encoder ends in state 0.
inline std::vector<uint8_t> viterbi_decode(const float* soft, int nout, int K, const std::vector<uint32_t>& polys,
                                           bool terminated) {
    const int ns = 1 << (K - 1), R = static_cast<int>(polys.size());
    const float INF = std::numeric_limits<float>::max() / 4;
    std::vector<float> metric(ns, INF), next(ns);
    metric[0] = 0;
    std::vector<uint8_t> dec(static_cast<size_t>(nout) * ns);
    std::vector<uint32_t> ob(ns * 2);
    for (int s = 0; s < ns; s++)
        for (int in = 0; in < 2; in++) {
            uint32_t reg = (static_cast<uint32_t>(s) << 1) | in, o = 0;
            for (int j = 0; j < R; j++) o |= (__builtin_popcount(reg & polys[j]) & 1) << j;
            ob[s * 2 + in] = o;
        }
    for (int t = 0; t < nout; t++) {
        std::fill(next.begin(), next.end(), INF);
        const float* y = soft + static_cast<size_t>(t) * R;
        for (int s = 0; s < ns; s++) {
            if (metric[s] >= INF) continue;
            for (int in = 0; in < 2; in++) {
                uint32_t o = ob[s * 2 + in];
                float bm = 0;
                for (int j = 0; j < R; j++) bm -= ((o >> j) & 1) ? -y[j] : y[j];
                int nx = ((s << 1) | in) & (ns - 1);
                if (metric[s] + bm < next[nx]) {
                    next[nx] = metric[s] + bm;
                    dec[static_cast<size_t>(t) * ns + nx] = static_cast<uint8_t>((s >> (K - 2)) & 1);
                }
            }
        }
        metric.swap(next);
    }
    int s = terminated ? 0 : static_cast<int>(std::min_element(metric.begin(), metric.end()) - metric.begin());
    std::vector<uint8_t> out(nout);
    for (int t = nout - 1; t >= 0; t--) {
        out[t] = s & 1;
        s = (s >> 1) | (dec[static_cast<size_t>(t) * ns + s] << (K - 2));
    }
    return out;
}

// --- Text -----------------------------------------------------------------------------
// Printable rendering of decoded bytes ("\xNN" for control characters)
inline std::string printable(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c >= 32 && c < 127) o += static_cast<char>(c);
        else { char b[8]; snprintf(b, sizeof b, "\\x%02X", c); o += b; }
    }
    return o;
}

}  // namespace kp
