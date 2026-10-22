// ============================================
// src/signal_processing/shared_decimator.cpp
// HYBRID PARALLELIZATION: Per-antenna threading within SharedDecimator
// Per channel: NCO mixer (phase-continuous across blocks; in front of the
// first stage or folded into it) and a cascade of Kaiser low-pass
// decimation stages (planStages: the cheapest split of the factor).
// ============================================

#include "signal_processing/shared_decimator.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <future>

using namespace std;

SharedDecimator::SharedDecimator() {}

SharedDecimator::~SharedDecimator() {
    cleanup();
}

bool SharedDecimator::initialize(int num_channels) {
    lock_guard<mutex> lock(init_mutex);

    if (initialized.load()) {
        cout << "SharedDecimator: Already initialized, cleaning up first" << endl;
        cleanup();
    }

    num_channels_initialized = num_channels;
    current_bandwidth_hz = bandwidth_option_hz(DEFAULT_BANDWIDTH_INDEX);

    // One persistent filter/mixer state per channel (see header comment)
    channel_states_.clear();
    channel_states_.reserve(num_channels);
    for (int ch = 0; ch < num_channels; ch++) {
        channel_states_.push_back(std::make_unique<ChannelDecimState>());
    }

    // Create thread pool for parallel channel decimation
    channel_thread_pool = std::make_unique<ThreadPool>(num_channels);

    initialized = true;

    cout << "SharedDecimator initialized:" << endl;
    cout << "  Channels: " << num_channels << endl;
    cout << "  ThreadPool size: " << num_channels << " threads" << endl;
    cout << "  Default bandwidth: " << getBandwidthHz() << " Hz (decimation " << getDecimationFactor() << ")" << endl;
    cout << "  Multi-stage decimation, mixer in front of / folded into the first stage" << endl;

    return true;
}

void SharedDecimator::cleanup() {
    // Destroy thread pool first (waits for all tasks to complete)
    channel_thread_pool.reset();

    // Safe to drop per-channel state only after the pool is gone
    channel_states_.clear();

    initialized = false;
}

// ---- The plan: which stages, and what each costs ----------------------------
namespace {

// Cost model of the stage kernels (ns per output, Pi 5 / Cortex-A76, one
// core; fitted to explore/kern2 + tools/decim_bench, worst case ~11 % off):
// a regular stage's output R_TAP x window + R_OUT + R_M x factor, a folded
// one's (complex taps) C_..., a halfband's the regular one at factor 1 on
// its x phase + the centre tap; per input sample the mixer in front (one
// run / a halfband's two phases), or the plain copy a folded stage takes
constexpr double R_TAP = 0.1328, R_OUT = 1.134, R_M = 0.143;
constexpr double C_TAP = 0.2394, C_OUT = 3.033, C_M = 0.137;
constexpr double HB_CENTRE = 0.3;
constexpr double MIX_NS = 3.39, MIX_SPLIT_NS = 2.82, COPY_NS = 0.52;
constexpr double HUGE_COST = 1e30;   // not INFINITY: -Ofast assumes there are none

size_t pad4(size_t n) { return (n + 3) / 4 * 4; }

// Kaiser estimate for 60 dB stopband attenuation, a transition of trans (of
// the input rate) wide
int kaiser_taps(double trans) {
    int t = static_cast<int>(std::ceil(52.0 / (14.36 * trans)));
    t = std::clamp(t, 9, 2047);
    if ((t & 1) == 0) t++;
    return t;
}

// ns per INPUT sample of a stage (at its own input rate)
double cost_regular(int taps, int f) { return (R_TAP * pad4(taps) + R_OUT + R_M * f) / f; }
double cost_folded(int taps, int f) { return (C_TAP * pad4(taps) + C_OUT + C_M * f) / f; }
double cost_halfband(int taps) { return (R_TAP * pad4((taps + 1) / 2) + R_OUT + R_M + HB_CENTRE) / 2; }

}  // namespace

// Per stage only frequencies that alias onto the final wanted band (+-0.4 x
// the bandwidth, the 80 % usable-bandwidth convention) need to be
// attenuated, so early stages get wide transition bands and few taps while
// the last stage makes the sharp cut at a low rate - adequate anti-aliasing
// at ANY total factor (the old single stage was capped at 127 taps, grossly
// under-dimensioned above factor ~63). The last stage stops from 0.6 x the
// bandwidth (-6 dB at its edge): with the bandwidth = the rate that is where
// the aliases begin, as it always was; a narrower bandwidth in a wider rate
// (the rate holds it - the next divisor of 2.4 MHz) is cut there too, so the
// VFO passes its bandwidth and no more, and its gain (each stage 1 / (2
// cutoff)) is SAMPLE_RATE / bandwidth for every width, = D for the
// efficient sizes.
//
// The split of D into stages of 2..8 (and their order) is the cheapest by
// the cost model (above): every ordered split, memoised over the factor that
// remains - its input rate follows from it. It was largest factor first:
// D = 2-8 then ran one long sharp stage at 2.4 MHz (300 kHz: 145 taps at
// 8:1 - now 4:1 25 taps + a halfband of 39); a factor-2 stage cut at a
// quarter of its input rate may run as a halfband; the mixer goes in front
// of the first stage or into its taps, whichever is cheaper (folding wins
// where the first stage's taps per input are few - every narrow width).
SharedDecimator::Plan SharedDecimator::planStages(int bandwidth_hz) {
    bandwidth_hz = clamp_bandwidth_hz(bandwidth_hz);
    const int D = decimation_for_bandwidth(bandwidth_hz);
    Plan plan;

    // The bandwidth fills its output rate (bandwidth_is_efficient)
    const bool exact = static_cast<long long>(bandwidth_hz) * D == SAMPLE_RATE_HZ;
    if (exact && D == 1) {   // the whole band: mixer only
        plan.cost_ns = MIX_NS;
        return plan;
    }

    const double final_rate = static_cast<double>(SAMPLE_RATE_HZ) / D;
    const double bw = bandwidth_hz;
    const double pass = 0.4 * bw;
    // The stage of factor f taking the remaining factor rem (its input rate =
    // final_rate x rem): its stopband edge, whether its cut-off is centred
    // (a quarter of its input rate - may run as a halfband), its taps
    auto stop_of = [&](int f, int rem) {
        const double s = final_rate * rem / f - pass;
        return f == rem ? std::min(s, 0.6 * bw) : s;
    };
    auto centred = [&](int f, int rem) { return f == 2 && (f != rem || exact); };
    auto taps_of = [&](int f, int rem, bool half) {
        int t = kaiser_taps((stop_of(f, rem) - pass) / (final_rate * rem));
        if (half) {
            while (t % 4 != 3) t += 2;   // 4m+3: the outermost taps nonzero (2047 = 4 x 511 + 3)
        }
        return t;
    };

    // best[rem] = the cheapest stages for the remaining factor rem after the
    // first one; filled in increasing rem (each needs only smaller ones)
    enum Kind { REG = 0, HALF = 1, FOLD = 2 };
    std::vector<double> best(D + 1, HUGE_COST);
    std::vector<int> best_f(D + 1, 0), best_k(D + 1, REG);
    best[1] = 0.0;
    for (int rem = 2; rem <= D; rem++) {
        if (D % rem) continue;
        const double r = static_cast<double>(rem) / D;   // its input rate / 2.4 MHz
        for (int f = 8; f >= 2; f--) {                   // ties: the larger factor first
            if (rem % f) continue;
            const double tail = best[rem / f];
            double c = r * cost_regular(taps_of(f, rem, false), f) + tail;
            if (c < best[rem]) best[rem] = c, best_f[rem] = f, best_k[rem] = REG;
            if (centred(f, rem)) {
                c = r * cost_halfband(taps_of(f, rem, true)) + tail;
                if (c < best[rem]) best[rem] = c, best_f[rem] = f, best_k[rem] = HALF;
            }
        }
    }

    // The first stage (input 2.4 MHz) carries the mixer: in front of it, into
    // a halfband's two phases, or folded into its taps. D = 1 here = a width
    // of 1.2 - 2.4 MHz: one stage of factor 1 (a plain full-rate filter)
    double first = HUGE_COST;
    int first_f = 0, first_k = REG;
    for (int f = (D == 1 ? 1 : 8); f >= 1; f--) {
        if (D % f || (f == 1 && D != 1)) continue;
        const double tail = best[D / f];
        const int t = taps_of(f, D, false);
        double c = cost_regular(t, f) + MIX_NS + tail;
        if (c < first) first = c, first_f = f, first_k = REG;
        c = cost_folded(t, f) + COPY_NS + tail;
        if (c < first) first = c, first_f = f, first_k = FOLD;
        if (centred(f, D)) {
            c = cost_halfband(taps_of(f, D, true)) + MIX_SPLIT_NS + tail;
            if (c < first) first = c, first_f = f, first_k = HALF;
        }
    }
    plan.cost_ns = first;
    plan.fold = first_k == FOLD;

    for (int rem = D, f = first_f, k = first_k; rem >= 1 && f >= 1;) {
        const bool half = k == HALF;
        StageDesign d{f, half, {}};
        const int t = taps_of(f, rem, half);
        const double cutoff = half ? 0.25 : 0.5 * (pass + stop_of(f, rem)) / (final_rate * rem);
        d.taps.resize(t);
        liquid_firdes_kaiser(t, static_cast<float>(cutoff), 60.0f, 0.0f, d.taps.data());
        if (half) {
            // The sinc's zeros, exactly
            const int c = (t - 1) / 2;
            for (int i = 0; i < t; i++) {
                if (i != c && ((i - c) & 1) == 0) d.taps[i] = 0.0f;
            }
        }
        plan.stages.push_back(std::move(d));
        rem /= f;
        if (rem <= 1) break;
        f = best_f[rem];
        k = best_k[rem];
    }
    return plan;
}

std::string SharedDecimator::Plan::describe() const {
    std::string s;
    for (size_t i = 0; i < stages.size(); i++) {
        if (i) s += ' ';
        s += std::to_string(stages[i].factor);
        if (i == 0 && fold) s += 'F';
        if (stages[i].half) s += 'h';
        s += ':' + std::to_string(stages[i].taps.size());
    }
    return s.empty() ? "mixer only" : s;
}

bool SharedDecimator::buildStages(ChannelDecimState& st, int bandwidth_hz) {
    st.destroyStages();
    st.fold = false;

    Plan plan = planStages(bandwidth_hz);
    for (auto& d : plan.stages) {
        if (d.taps.empty() || d.factor < 1) {
            cerr << "SharedDecimator: bad stage design (factor " << d.factor << ", "
                 << d.taps.size() << " taps)" << endl;
            st.destroyStages();
            return false;
        }
        st.stages.emplace_back(d.factor, d.taps, d.half);
    }
    st.fold = plan.fold && !st.stages.empty() && !st.stages.front().half;
    return true;
}

// ---- The stage kernels (see FirStage in the header) -------------------------
namespace {

using cf = complex<float>;
typedef float v4f __attribute__((vector_size(16)));
#if defined(__clang__)
#define SHUF4(a, b, i0, i1, i2, i3) __builtin_shufflevector(a, b, i0, i1, i2, i3)
#else
typedef int v4i __attribute__((vector_size(16)));
#define SHUF4(a, b, i0, i1, i2, i3) __builtin_shuffle(a, b, (v4i){i0, i1, i2, i3})
#endif

inline v4f ld4(const float* p) { v4f v; std::memcpy(&v, p, sizeof v); return v; }
inline void st4(float* p, v4f v) { std::memcpy(p, &v, sizeof v); }
// [A0 + A2, A1 + A3, B0 + B2, B1 + B3]: two outputs' I / Q from their sums
inline v4f pair_sum(v4f A, v4f B) { return SHUF4(A, B, 0, 1, 4, 5) + SHUF4(A, B, 2, 3, 6, 7); }

// Outputs j .. j+3 of a call (j a multiple of 4) / output j: into one run
// (a), or two phases (output j to a[j / 2] when j is even, else b[j / 2])
template <bool SPLIT> inline void put4(float* a, float* b, size_t j, v4f y01, v4f y23) {
    if constexpr (SPLIT) {
        st4(a + j, SHUF4(y01, y23, 0, 1, 4, 5));
        st4(b + j, SHUF4(y01, y23, 2, 3, 6, 7));
    } else {
        st4(a + 2 * j, y01);
        st4(a + 2 * j + 4, y23);
    }
}
template <bool SPLIT> inline void put1(float* a, float* b, size_t j, float re, float im) {
    float* p = SPLIT ? ((j & 1) ? b : a) + 2 * (j >> 1) : a + 2 * j;
    p[0] = re;
    p[1] = im;
}

// n outputs of real taps t2 (nf floats: the window's taps, each twice) on
// the windows w0 + j x step floats; HB: + hc x oc[j] (a halfband's centre)
template <bool SPLIT, bool HB>
void fir_real(const float* t2, size_t nf, const float* w0, size_t step, size_t n,
              const float* oc, float hc, float* a, float* b) {
    size_t j = 0;
    for (; j + 4 <= n; j += 4) {
        const float* p0 = w0 + j * step;
        const float* p1 = p0 + step;
        const float* p2 = p1 + step;
        const float* p3 = p2 + step;
        v4f a0 = {0, 0, 0, 0}, a1 = a0, b0 = a0, b1 = a0, c0 = a0, c1 = a0, d0 = a0, d1 = a0;
        for (size_t i = 0; i < nf; i += 8) {
            const v4f t0 = ld4(t2 + i), t1 = ld4(t2 + i + 4);
            a0 += t0 * ld4(p0 + i), a1 += t1 * ld4(p0 + i + 4);
            b0 += t0 * ld4(p1 + i), b1 += t1 * ld4(p1 + i + 4);
            c0 += t0 * ld4(p2 + i), c1 += t1 * ld4(p2 + i + 4);
            d0 += t0 * ld4(p3 + i), d1 += t1 * ld4(p3 + i + 4);
        }
        v4f y01 = pair_sum(a0 + a1, b0 + b1), y23 = pair_sum(c0 + c1, d0 + d1);
        if constexpr (HB) {
            const v4f c = {hc, hc, hc, hc};
            y01 += c * ld4(oc + 2 * j);
            y23 += c * ld4(oc + 2 * j + 4);
        }
        put4<SPLIT>(a, b, j, y01, y23);
    }
    for (; j < n; j++) {
        const float* p = w0 + j * step;
        v4f s0 = {0, 0, 0, 0}, s1 = s0;
        for (size_t i = 0; i < nf; i += 8) s0 += ld4(t2 + i) * ld4(p + i), s1 += ld4(t2 + i + 4) * ld4(p + i + 4);
        const v4f s = s0 + s1;
        float re = s[0] + s[2], im = s[1] + s[3];
        if constexpr (HB) re += hc * oc[2 * j], im += hc * oc[2 * j + 1];
        put1<SPLIT>(a, b, j, re, im);
    }
}

// The complex tap g = gr + j gi times a sample: A = sum gr x, B = sum gi x
// lane-wise (I, Q, I, Q) -> (A_I - B_Q) + j (A_Q + B_I)
inline cf cplx(v4f A, v4f B) { return cf((A[0] + A[2]) - (B[1] + B[3]), (A[1] + A[3]) + (B[0] + B[2])); }

// n outputs of complex taps (gr / gi: re / im each twice), each turned by
// the mixer's phase ph (times ps per output, renormalised every 1024)
template <bool SPLIT>
void fir_cplx(const float* gr, const float* gi, size_t nf, const float* w0, size_t step, size_t n,
              cf& ph, cf ps, float* a, float* b) {
    cf p = ph;
    size_t j = 0;
    for (; j + 4 <= n; j += 4) {
        const float* p0 = w0 + j * step;
        const float* p1 = p0 + step;
        const float* p2 = p1 + step;
        const float* p3 = p2 + step;
        v4f A0 = {0, 0, 0, 0}, A1 = A0, A2 = A0, A3 = A0, B0 = A0, B1 = A0, B2 = A0, B3 = A0;
        for (size_t i = 0; i < nf; i += 4) {
            const v4f r = ld4(gr + i), q = ld4(gi + i);
            const v4f x0 = ld4(p0 + i), x1 = ld4(p1 + i), x2 = ld4(p2 + i), x3 = ld4(p3 + i);
            A0 += r * x0, B0 += q * x0, A1 += r * x1, B1 += q * x1;
            A2 += r * x2, B2 += q * x2, A3 += r * x3, B3 += q * x3;
        }
        const cf y0 = cplx(A0, B0) * p;
        p *= ps;
        const cf y1 = cplx(A1, B1) * p;
        p *= ps;
        const cf y2 = cplx(A2, B2) * p;
        p *= ps;
        const cf y3 = cplx(A3, B3) * p;
        p *= ps;
        if ((j & 1020) == 0) p /= std::abs(p);
        put4<SPLIT>(a, b, j, v4f{y0.real(), y0.imag(), y1.real(), y1.imag()},
                    v4f{y2.real(), y2.imag(), y3.real(), y3.imag()});
    }
    for (; j < n; j++) {
        const float* q0 = w0 + j * step;
        v4f A = {0, 0, 0, 0}, B = A;
        for (size_t i = 0; i < nf; i += 4) {
            const v4f x = ld4(q0 + i);
            A += ld4(gr + i) * x, B += ld4(gi + i) * x;
        }
        const cf y = cplx(A, B) * p;
        p *= ps;
        put1<SPLIT>(a, b, j, y.real(), y.imag());
    }
    const float mag = std::abs(p);
    if (mag > 0.0f) p /= mag;
    ph = p;
}

// The VFO's mixer at the input rate: x down by the oscillator (phase, times
// step per sample) into d. Its phase persists across blocks - a phase step
// at a block boundary is audible as clicks in FM and contaminates MUSIC
// snapshots that span blocks
void mix_into(const cf* x, size_t n, SharedDecimator::Dst d, cf& phase, cf step) {
    cf p = phase;
    if (!d.b) {
        for (size_t i = 0; i < n; i++) {
            d.a[i] = x[i] * p;
            p *= step;
            if ((i & 0x3FF) == 0x3FF) p /= std::abs(p);   // no magnitude drift
        }
    } else {
        size_t i = 0, k = 0;
        for (; i + 2 <= n; i += 2, k++) {
            d.a[k] = x[i] * p;
            p *= step;
            d.b[k] = x[i + 1] * p;
            p *= step;
            if ((i & 0x3FE) == 0x3FE) p /= std::abs(p);
        }
        if (i < n) {
            d.a[k] = x[i] * p;
            p *= step;
        }
    }
    // Normalize before saving so drift can't compound over blocks
    const float mag = std::abs(p);
    if (mag > 0.0f) p /= mag;
    phase = p;
}

void copy_into(const cf* x, size_t n, SharedDecimator::Dst d) {
    if (!d.b) {
        if (n) std::memcpy(static_cast<void*>(d.a), x, n * sizeof(cf));
        return;
    }
    size_t i = 0, k = 0;
    for (; i + 2 <= n; i += 2, k++) d.a[k] = x[i], d.b[k] = x[i + 1];
    if (i < n) d.a[k] = x[i];
}

}  // namespace

// ---- SampleBuf -----------------------------------------------------------
complex<float>* SharedDecimator::SampleBuf::grow(size_t k) {
    if (n + k > v.size()) v.resize(std::max(n + k, 2 * v.size()));   // zeroes only on a new high
    complex<float>* p = v.data() + n;
    n += k;
    return p;
}

void SharedDecimator::SampleBuf::drop(size_t k) {
    k = std::min(k, n);
    if (k == 0) return;
    std::memmove(static_cast<void*>(v.data()), v.data() + k, (n - k) * sizeof(complex<float>));
    n -= k;
}

void SharedDecimator::SampleBuf::zeros(size_t k) {
    n = 0;
    std::fill_n(grow(k), k, complex<float>(0.0f, 0.0f));
}

// ---- FirStage --------------------------------------------------------------
SharedDecimator::FirStage::FirStage(int m, const std::vector<float>& taps, bool halfband)
    : M(std::max(1, m)), half(halfband && m == 2 && taps.size() % 4 == 3) {
    const size_t n = taps.size();
    if (half) {
        // x phase: taps 0, 2, 4 .. (the even ones back from the newest
        // sample); the centre (odd, (n - 1) / 2) on the o phase, m + 1 = (n
        // + 1) / 4 of its samples back from the output's newest
        L = pad4((n + 1) / 2);
        h.assign(L, 0.0f);
        for (size_t i = 0; i < L; i++) {
            const size_t k = 2 * (L - 1 - i);   // full-rate samples back from the newest
            if (k < n) h[i] = taps[k];
        }
        hc = taps[(n - 1) / 2];
        cd = (n + 1) / 4;
    } else {
        L = pad4(n);
        h.assign(L, 0.0f);
        for (size_t i = 0; i < L; i++) {
            const size_t k = L - 1 - i;
            if (k < n) h[i] = taps[k];
        }
    }
    t2.resize(2 * L);
    for (size_t i = 0; i < L; i++) t2[2 * i] = t2[2 * i + 1] = h[i];
    reset();
}

void SharedDecimator::FirStage::reset() {
    // L - 1 zeros of history: the first output's window ends at the first
    // sample, as liquid's window starts out
    x.zeros(L - 1);
    pos = L - 1;
    if (half) {
        o.zeros(cd);
        par = 0;
    }
    ph = complex<float>(1.0f, 0.0f);   // the mixer's phase 1 at the first sample
}

SharedDecimator::Dst SharedDecimator::FirStage::take(size_t n) {
    if (!half) return Dst{x.grow(n), nullptr};
    SampleBuf& first = par ? o : x;
    SampleBuf& second = par ? x : o;
    complex<float>* a = first.grow((n + 1) / 2);
    complex<float>* b = second.grow(n / 2);
    par ^= static_cast<unsigned>(n & 1);
    return Dst{a, b};
}

size_t SharedDecimator::FirStage::pending() const {
    if (half) return x.n > pos ? x.n - pos : 0;
    return pos < x.n ? (x.n - 1 - pos) / static_cast<size_t>(M) + 1 : 0;
}

void SharedDecimator::FirStage::produce(Dst out) {
    const size_t n = pending();
    const size_t at = pos - (L - 1);   // the next output's window starts at x[at]
    const float* w0 = reinterpret_cast<const float*>(x.data()) + 2 * at;
    const size_t step = 2 * (half ? 1 : static_cast<size_t>(M));
    float* a = reinterpret_cast<float*>(out.a);
    float* b = reinterpret_cast<float*>(out.b);
    if (n) {
        if (folded) {
            if (b) fir_cplx<true>(g2r.data(), g2i.data(), 2 * L, w0, step, n, ph, ps, a, b);
            else fir_cplx<false>(g2r.data(), g2i.data(), 2 * L, w0, step, n, ph, ps, a, b);
        } else if (half) {
            // o[at + j] = output j's centre sample (o holds cd of history)
            const float* oc = reinterpret_cast<const float*>(o.data()) + 2 * at;
            if (b) fir_real<true, true>(t2.data(), 2 * L, w0, step, n, oc, hc, a, b);
            else fir_real<false, true>(t2.data(), 2 * L, w0, step, n, oc, hc, a, b);
        } else {
            if (b) fir_real<true, false>(t2.data(), 2 * L, w0, step, n, nullptr, 0.0f, a, b);
            else fir_real<false, false>(t2.data(), 2 * L, w0, step, n, nullptr, 0.0f, a, b);
        }
    }
    pos += n * (half ? 1 : static_cast<size_t>(M));
    // Keep only what a later output needs: its window's first sample
    // onwards (halfband: and the o samples from its centre on - the same
    // count, so o[i] stays output i's centre)
    const size_t k = std::min(pos - (L - 1), x.n);
    x.drop(k);
    if (half) o.drop(k);
    pos -= k;
}

void SharedDecimator::FirStage::fold(double w) {
    folded = w != 0.0 && !half;
    if (!folded) return;
    // g = h e^{+jwk} (k samples back from the window's newest); the mixer
    // phase 1 at the next sample to come - the next output's newest sample
    // is pos - x.n samples later (all pending outputs are made by now)
    g2r.resize(2 * L);
    g2i.resize(2 * L);
    for (size_t i = 0; i < L; i++) {
        const double a = w * static_cast<double>(L - 1 - i);
        g2r[2 * i] = g2r[2 * i + 1] = static_cast<float>(h[i] * std::cos(a));
        g2i[2 * i] = g2i[2 * i + 1] = static_cast<float>(h[i] * std::sin(a));
    }
    const double d = static_cast<double>(pos) - static_cast<double>(x.n);
    ph = complex<float>(static_cast<float>(std::cos(-w * d)), static_cast<float>(std::sin(-w * d)));
    ps = complex<float>(static_cast<float>(std::cos(-w * M)), static_cast<float>(std::sin(-w * M)));
}

// The mixer's rad per input sample for an offset (none within 1 Hz)
static double mixer_w(float offset_hz) {
    return std::fabs(offset_hz) > 1.0f ? 2.0 * M_PI * offset_hz / SAMPLE_RATE_HZ : 0.0;
}

// Input samples per pass through the cascade (decimateChannel): 8 KB of
// complex floats - the chunk + the stages' buffers stay in the core's L1.
// Pi 5, kraken_doa with 1 / 2 / 3 VFOs (DoA, 5 antennas): 512 and 1024 the
// same, 2048 +1-2 % of a core, 4096 +2-7, the whole block (16384) +8-30
static constexpr size_t DECIM_CHUNK = 1024;

// Per-channel state: only one pool task touches a given channel at a time
// (one task per channel per block, blocks sequential), so no locking needed.
SharedDecimator::ChannelDecimState* SharedDecimator::ensureChannelState(
    int channel, int bandwidth_hz, float freq_offset_hz) {

    ChannelDecimState& st = *channel_states_[channel];

    if (st.bandwidth_hz != bandwidth_hz) {
        // Bandwidth (and with it the factor / the filters) changed - rebuild
        // the cascade. Filter history and mixer phase are only meaningful for
        // unchanged settings.
        if (!buildStages(st, bandwidth_hz)) {
            st.decimation_factor = 0;
            st.bandwidth_hz = 0;
            return nullptr;
        }
        st.decimation_factor = decimation_for_bandwidth(bandwidth_hz);
        st.bandwidth_hz = bandwidth_hz;
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
        if (st.fold) st.stages.front().fold(mixer_w(freq_offset_hz));
    } else if (fabs(st.freq_offset_hz - freq_offset_hz) > 0.1f) {
        // Offset changed: the low-pass stages are offset-independent, so
        // only the mixer retunes (phase continuity is meaningless across a
        // retune). This also removes the old cache-quantization bug where a
        // filter built for one offset served requests up to 1 kHz away. The
        // few samples the stages hold mixed with the old offset go through
        // like their history (a transient; MUSIC drops the frames around an
        // offset change anyway; a folded first stage holds RAW samples - its
        // history is mixed with the new offset) - the stream stays
        // sample-continuous.
        st.freq_offset_hz = freq_offset_hz;
        st.mixer_phase = complex<float>(1.0f, 0.0f);
        if (st.fold) st.stages.front().fold(mixer_w(freq_offset_hz));
    }
    return &st;
}


// Bandwidth management methods
void SharedDecimator::setBandwidthHz(int hz) {
    // (no log line: a bar edge being dragged sets it ~12 times a second; the
    // control handler logs the applied width)
    current_bandwidth_hz = clamp_bandwidth_hz(hz);
}

void SharedDecimator::setFrequencyOffset(float offset_hz) {
    current_freq_offset_hz = offset_hz;
}

// Per-channel block decimation: the mixer (or the folded first stage), then
// the stage cascade
SharedDecimator::DecimatedData SharedDecimator::decimateChannel(
    const complex<float>* iq_data, size_t num_samples, int channel_id,
    int bandwidth_hz, float freq_offset_hz) {

    DecimatedData result;
    result.channel_id = channel_id;

    bandwidth_hz = bandwidth_hz > 0 ? clamp_bandwidth_hz(bandwidth_hz) : getBandwidthHz();
    const int decimation_factor = decimation_for_bandwidth(bandwidth_hz);

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;

    if (!initialized.load() || channel_id < 0 || channel_id >= num_channels_initialized || !iq_data) {
        cerr << "SharedDecimator: Not initialized, invalid channel, or null data" << endl;
        return result;
    }

    // Per-channel persistent mixer/filter state (see ensureChannelState)
    ChannelDecimState* st = ensureChannelState(channel_id, bandwidth_hz, freq_offset_hz);
    if (!st) {
        cerr << "SharedDecimator: Failed to build decimator for channel " << channel_id << endl;
        return result;
    }

    // The input goes straight into the first stage's buffer - mixed to
    // baseband first unless the mixer is folded into that stage; the whole
    // band (no stage) straight into the result. Every stage keeps the
    // samples its next output still needs - the tail of a block that isn't a
    // multiple of its factor included - so no input sample is dropped.
    // (Dropping that tail, long ago, spliced the signal and jumped the mixer
    // phase at every block boundary - an audible buzz on any off-center VFO
    // whose factor doesn't divide the block size.)
    result.samples.clear();
    const double w = mixer_w(freq_offset_hz);
    const bool mix_front = w != 0.0 && !st->fold;
    const complex<float> step(static_cast<float>(std::cos(-w)), static_cast<float>(std::sin(-w)));
    if (st->stages.empty()) {
        result.samples.resize(num_samples);
        const Dst dst{result.samples.data(), nullptr};
        if (mix_front) mix_into(iq_data, num_samples, dst, st->mixer_phase, step);
        else copy_into(iq_data, num_samples, dst);
    } else {
        // The block in chunks of DECIM_CHUNK samples, each through the whole
        // cascade: each stage's outputs straight into the next one's
        // buffer, the last one's appended to the result. The stages' buffers
        // stay a chunk + their history long, in the core's L1 / L2 - with
        // the whole block (16384 samples = 128 KB per antenna) every VFO
        // copied each antenna's block into its first stage and read it back
        // from memory: in the live pipeline (5 antennas, every VFO's tasks
        // on other cores) that copy cost more than the filtering, and every
        // VFO more as they shared the memory bandwidth (perf 2026-10-22:
        // one 12.5 kHz VFO with DoA 28 % of a core, three 65 %; chunked 20
        // / 33 %). The outputs are the same - the stages take any length
        result.samples.reserve(num_samples / static_cast<size_t>(decimation_factor) + 2);
        for (size_t at = 0; at < num_samples; at += DECIM_CHUNK) {
            const size_t c = std::min(DECIM_CHUNK, num_samples - at);
            const Dst dst = st->stages.front().take(c);
            if (mix_front) mix_into(iq_data + at, c, dst, st->mixer_phase, step);
            else copy_into(iq_data + at, c, dst);
            for (size_t s = 0; s < st->stages.size(); s++) {
                FirStage& f = st->stages[s];
                const size_t n = f.pending();
                Dst out;
                if (s + 1 == st->stages.size()) {
                    const size_t old = result.samples.size();
                    result.samples.resize(old + n);
                    out.a = result.samples.data() + old;
                } else {
                    out = st->stages[s + 1].take(n);
                }
                f.produce(out);
            }
        }
    }

    result.num_samples = result.samples.size();
    total_samples_processed.fetch_add(num_samples);
    total_blocks_processed.fetch_add(1);

    return result;  // NRVO will optimize this
}

// OPTIMIZED: Multi-channel float IQ input method with precomputed coefficients
// HYBRID PARALLELIZATION: Per-antenna threading. Zero-copy core - the
// caller's sample arrays are only read and must outlive the call (all pool
// futures are collected before returning, so that just means the call site's
// data must stay alive for the duration of this function).
SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    const complex<float>* const* channel_ptrs, const size_t* channel_lens,
    size_t num_channels, int bandwidth_hz, float freq_offset_hz) {

    MultiChannelDecimated result;

    bandwidth_hz = bandwidth_hz > 0 ? clamp_bandwidth_hz(bandwidth_hz) : getBandwidthHz();
    const int decimation_factor = decimation_for_bandwidth(bandwidth_hz);

    result.decimation_used = decimation_factor;
    result.output_rate_hz = SAMPLE_RATE / decimation_factor;
    result.freq_offset_hz = freq_offset_hz;
    result.bandwidth_hz = bandwidth_hz;

    if (!initialized.load() || num_channels == 0 || !channel_ptrs || !channel_lens) {
        return result;
    }

    // The offset is exactly the caller's, for every channel of the block. (A
    // fallback to the live current_freq_offset_hz when this was ~0 - re-read
    // again inside each channel task - let channels of one block mix at
    // different offsets when a VFO moved off 0 Hz mid-block.)
    result.num_channels = min(num_channels, static_cast<size_t>(num_channels_initialized));
    if (result.num_channels != last_num_channels_) {
        for (size_t ch = 0; ch < channel_states_.size(); ch++) channel_states_[ch]->resetRuntime();
        last_num_channels_ = result.num_channels;
    }
    result.channels.resize(result.num_channels);  // Pre-allocate exact size for parallel writes

    // Find minimum number of samples across channels
    size_t min_samples = SIZE_MAX;
    for (size_t ch = 0; ch < result.num_channels; ch++) {
        min_samples = min(min_samples, channel_ptrs[ch] ? channel_lens[ch] : 0);
    }

    if (min_samples == 0 || min_samples == SIZE_MAX) {
        return result;
    }
    // Per-channel output counts come from the channels themselves: with the
    // samples the stages keep from the block before, a block yields
    // floor(N/D) or floor(N/D)+1 samples. Every channel gets the same input
    // length, so their pending samples - and output counts - stay equal;
    // min_samples is the common count.
    result.min_samples = SIZE_MAX;

    // HYBRID PARALLELIZATION: Launch parallel tasks for each antenna using ThreadPool
    vector<std::future<DecimatedData>> antenna_futures(result.num_channels);

    for (size_t ch = 0; ch < result.num_channels; ch++) {
        const complex<float>* ch_ptr = channel_ptrs[ch];
        antenna_futures[ch] = channel_thread_pool->submit(
            [this, ch, ch_ptr, min_samples, bandwidth_hz, freq_offset_hz]() -> DecimatedData {
                return decimateChannel(
                    ch_ptr,
                    min_samples,
                    static_cast<int>(ch),
                    bandwidth_hz,
                    freq_offset_hz
                );
            }
        );
    }

    // Collect results from parallel antenna processing
    for (size_t ch = 0; ch < result.num_channels; ch++) {
        DecimatedData ch_data = antenna_futures[ch].get();

        if (ch_data.samples.empty() && min_samples >= static_cast<size_t>(decimation_factor)) {
            cerr << "SharedDecimator: No samples generated for channel " << ch << endl;
            // Consumers iterate channels[ch].samples[0..min_samples); an
            // empty channel makes the whole block unusable for coherent
            // processing, so mark it as such instead of leaving a stale
            // min_samples that would read past the empty vector
            result.min_samples = 0;
            continue;
        }

        if (ch_data.num_samples < result.min_samples) {
            result.min_samples = ch_data.num_samples;
        }

        result.channels[ch] = std::move(ch_data);  // Direct assignment to pre-allocated slot
    }

    // Log performance statistics periodically
    // Shared by every VFO's decimator, which run concurrently: atomic
    static std::atomic<size_t> stats_counter{0};
    if ((stats_counter.fetch_add(1, std::memory_order_relaxed) + 1) % 1000 == 0) {
        cout << "SharedDecimator performance (hybrid parallel): "
             << "Blocks: " << getBlocksProcessed() << ", "
             << "Samples: " << getSamplesProcessed() << endl;
    }

    return result;  // NRVO will optimize this
}

// Vector overloads: only READ the input and delegate to the pointer core.
// (The old const& version deep-copied the entire multichannel block.)
SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    const vector<vector<complex<float>>>& channel_data,
    int bandwidth_hz, float freq_offset_hz) {

    size_t n = min(channel_data.size(), static_cast<size_t>(MAX_CHANNELS));
    const complex<float>* ptrs[MAX_CHANNELS] = {nullptr};
    size_t lens[MAX_CHANNELS] = {0};
    for (size_t ch = 0; ch < n; ch++) {
        ptrs[ch] = channel_data[ch].data();
        lens[ch] = channel_data[ch].size();
    }
    return decimateMultiChannel(ptrs, lens, n, bandwidth_hz, freq_offset_hz);
}

SharedDecimator::MultiChannelDecimated SharedDecimator::decimateMultiChannel(
    vector<vector<complex<float>>>&& channel_data,
    int bandwidth_hz, float freq_offset_hz) {

    return decimateMultiChannel(
        static_cast<const vector<vector<complex<float>>>&>(channel_data),
        bandwidth_hz, freq_offset_hz);
}
