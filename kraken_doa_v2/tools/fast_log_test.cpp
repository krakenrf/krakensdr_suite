// fast_log2 / fast_db10 (include/utils/fast_log.hpp) vs std::log2: the
// largest error over every float from 1e-15 (the spectra's floor) to 1e6 in
// steps of a few ulps + 10 M random powers - in log2 units and in units of
// the float result's ulp, next to std::log2f's own - and the time of a
// spectrum's 16384-bin FFT-output -> dB loop in three shapes: the old one
// (the DC-centring index wrapped with a mask: not vectorizable, so a libm
// call per bin), the two halves as straight loops with std::log2f (glibc
// vectorizes those with its vector log2f; MinGW has none) and with fast_db10.
// Build (from kraken_doa_v2): g++ -std=c++20 -Ofast -march=native -Iinclude
//     tools/fast_log_test.cpp -o fast_log_test
#include "utils/fast_log.hpp"

#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {
constexpr float LOG10_2 = 0.30102999566398119521f;
constexpr int N = 16384;

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// the old loop shape (fft_processor.cpp before 2026-10-22)
__attribute__((noinline)) void db_old(const float (*in)[2], float* out, float scale) {
    for (int i = 0; i < N; i++) {
        const int k = (i + N / 2) & (N - 1);
        const float p = (in[k][0] * in[k][0] + in[k][1] * in[k][1]) * scale + 1e-15f;
        out[i] = 10.0f * (std::log2f(p) * LOG10_2);
    }
}
template <bool FAST>
__attribute__((noinline)) void db_halves(const float (*in)[2], float* out, float scale) {
    for (int h = 0; h < 2; h++) {
        const float (*src)[2] = in + (h ? 0 : N / 2);
        float* dst = out + (h ? N / 2 : 0);
        for (int i = 0; i < N / 2; i++) {
            const float p = (src[i][0] * src[i][0] + src[i][1] * src[i][1]) * scale + 1e-15f;
            dst[i] = FAST ? fast_db10(p) : 10.0f * (std::log2f(p) * LOG10_2);
        }
    }
}

// (towards FLT_MAX, not INFINITY: -Ofast assumes there are no infinities)
double ulp(float y) { return std::fabs(static_cast<double>(std::nextafter(y, FLT_MAX)) - y); }
}  // namespace

int main() {
    double worst = 0, worst_x = 0, worst_lib = 0, worst_ulp = 0, worst_lib_ulp = 0;
    uint64_t checked = 0;
    auto check = [&](float x) {
        const double exact = std::log2(static_cast<double>(x));
        const float f = fast_log2(x), l = std::log2f(x);
        const double e = std::fabs(f - exact), el = std::fabs(l - exact);
        if (e > worst) worst = e, worst_x = x;
        worst_lib = std::max(worst_lib, el);
        worst_ulp = std::max(worst_ulp, e / ulp(f));
        worst_lib_ulp = std::max(worst_lib_ulp, el / ulp(l));
        checked++;
    };
    uint32_t b0, b1;
    const float lo = 1e-15f, hi = 1e6f;
    std::memcpy(&b0, &lo, 4);
    std::memcpy(&b1, &hi, 4);
    for (uint32_t b = b0; b <= b1; b += 7) {
        float x;
        std::memcpy(&x, &b, 4);
        check(x);
    }
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> u(-15.0, 6.0);
    for (int i = 0; i < 10000000; i++) check(static_cast<float>(std::pow(10.0, u(rng))));
    std::printf("%llu values: fast_log2 worst |error| %.3g (%.3g dB, %.2f ulp of the result, at %g); "
                "std::log2f %.3g (%.2f ulp)\n",
                static_cast<unsigned long long>(checked), worst, worst * 3.0103, worst_ulp, worst_x, worst_lib, worst_lib_ulp);

    std::vector<float> in(2 * N), a(N), b(N), c(N);
    std::normal_distribution<float> g(0.0f, 1.0f);
    for (auto& v : in) v = g(rng) * 100.0f;
    const auto* bins = reinterpret_cast<const float (*)[2]>(in.data());
    const float scale = 1.0f / (static_cast<float>(N) * N);
    auto time = [&](void (*fn)(const float (*)[2], float*, float), float* out) {
        for (int i = 0; i < 50; i++) fn(bins, out, scale);
        int reps = 0;
        const double t0 = now_s();
        while (now_s() - t0 < 0.5) fn(bins, out, scale), reps++;
        return (now_s() - t0) * 1e9 / (static_cast<double>(reps) * N);
    };
    const double t_old = time(db_old, a.data()), t_lib = time(db_halves<false>, b.data()), t_fast = time(db_halves<true>, c.data());
    double md = 0;
    for (int i = 0; i < N; i++) md = std::max(md, static_cast<double>(std::fabs(a[i] - c[i])));
    std::printf("16384-bin dB loop, ns per bin: old shape %.2f, halves + log2f %.2f, halves + fast_db10 %.2f "
                "(%.1fx the old); max |dB difference| fast vs old %.3g\n",
                t_old, t_lib, t_fast, t_old / t_fast, md);
    // pass: within 1e-5 dB of the exact value everywhere (~5 ulps at most)
    return worst * 3.0103 <= 1e-5 ? 0 : 1;
}
