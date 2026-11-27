// convert_blocks_test.cpp - checks heimdall's block-wise channel conversion (namespace convblk in
// src/sdr/sdr_pipeline.cpp, the text between the CONVBLK markers, compiled here verbatim):
//   1. every block size gives the SAME output, bit for bit, over three consecutive sets with the
//      trim's and the EQ's history carried between them (the first set fills the trim's history,
//      as after a flush), with the EQ off and on - block sizes down to 1 sample, shorter than
//      the filters' history;
//   2. the output against a double-precision computation of the same chain.
// Build and run, from heimdall_v2/:
//   sed -n '/CONVBLK-BEGIN/,/CONVBLK-END/p' src/sdr/sdr_pipeline.cpp > /tmp/convblk.inc
//   g++ -O3 -march=native -fno-math-errno -fcx-fortran-rules -std=c++20
//       -DCONVBLK_INC='"/tmp/convblk.inc"' -o /tmp/convert_blocks_test tools/convert_blocks/convert_blocks_test.cpp
//   (one command line)
//   /tmp/convert_blocks_test        (on a Pi: -mcpu=native for -march=native)
// Exit status 0 = every check passed.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

using Complex = std::complex<float>;
#include CONVBLK_INC

constexpr int N = 16384, SETS = 3;

struct Inputs {
    std::vector<uint8_t> iq;             // SETS sets of N interleaved samples
    float lut[256];
    Complex comp;
    float qs;
    float trim_h[convblk::TRIM_TAPS];
    Complex eq_h[convblk::EQ_TAPS];
};

static Inputs make_inputs(unsigned seed, bool inverted) {
    Inputs in;
    std::mt19937 rng(seed);
    in.iq.resize(2 * static_cast<size_t>(N) * SETS);
    for (auto& v : in.iq) v = static_cast<uint8_t>(rng() & 255);
    for (int i = 0; i < 256; i++) in.lut[i] = (static_cast<float>(i) - 127.5f) / 127.5f;
    in.comp = std::polar(1.0f, 0.7f);
    in.qs = inverted ? -1.0f : 1.0f;
    // heimdall's trim design at d = 0.3: Hann-windowed sinc, unity DC gain
    float g = 0;
    for (int n = 0; n < convblk::TRIM_TAPS; n++) {
        const double x = n - 7 - 0.3;
        const double s = std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (n + 0.5) / convblk::TRIM_TAPS);
        in.trim_h[n] = static_cast<float>(s * w);
        g += in.trim_h[n];
    }
    for (auto& h : in.trim_h) h /= g;
    std::normal_distribution<float> nd(0.0f, 0.03f);
    for (auto& h : in.eq_h) h = Complex(nd(rng), nd(rng));
    in.eq_h[16] += Complex(0.95f, 0.1f);
    return in;
}

// heimdall's way: SETS calls of convblk::convert with the state carried
static std::vector<Complex> run(const Inputs& in, int block, bool eq_on) {
    Complex trim_tail[convblk::TRIM_HIST] = {};
    Complex eq_tail[convblk::EQ_HIST] = {};
    std::vector<Complex> xbuf, ebuf, out(static_cast<size_t>(N) * SETS);
    convblk::LegacyEq eq{in.eq_h, eq_tail, &ebuf};
    for (int s = 0; s < SETS; s++) {
        convblk::convert(in.iq.data() + 2 * static_cast<size_t>(N) * s, N, block, in.lut, in.comp, in.qs, in.trim_h,
                         trim_tail, s == 0, eq_on ? &eq : nullptr, xbuf, out.data() + static_cast<size_t>(N) * s);
    }
    return out;
}

// the same chain in double precision over the whole stream
static std::vector<std::complex<double>> reference(const Inputs& in, bool eq_on) {
    const int T = N * SETS;
    std::vector<std::complex<double>> x(T), t(T), y(T);
    const std::complex<double> c(in.comp.real(), in.comp.imag());
    for (int i = 0; i < T; i++) {
        const double I = in.lut[in.iq[2 * i]], Q = in.qs * in.lut[in.iq[2 * i + 1]];
        x[i] = std::complex<double>(I, Q) * c;
    }
    for (int i = 0; i < T; i++) {   // trim: history before sample 0 = sample 0 (fill_tail)
        std::complex<double> a = 0;
        for (int n = 0; n < convblk::TRIM_TAPS; n++) a += static_cast<double>(in.trim_h[n]) * x[std::max(i - n, 0)];
        t[i] = a;
    }
    if (!eq_on) return t;
    for (int i = 0; i < T; i++) {   // EQ: history before sample 0 = 0 (a fresh eq_tail)
        std::complex<double> a = 0;
        for (int n = 0; n < convblk::EQ_TAPS; n++)
            if (i - n >= 0) a += std::complex<double>(in.eq_h[n].real(), in.eq_h[n].imag()) * t[i - n];
        y[i] = a;
    }
    return y;
}

int main() {
    int fails = 0;
    const int blocks[] = {0, 16384, 4096, 2048, 1000, 333, 31, 16, 15, 7, 1};
    for (int inv = 0; inv < 2; inv++) {
        const Inputs in = make_inputs(11 + inv, inv == 1);
        for (int eq_on = 0; eq_on < 2; eq_on++) {
            const std::vector<Complex> base = run(in, 0, eq_on);
            const auto ref = reference(in, eq_on);
            double rmax = 0, emax = 0;
            for (size_t i = 0; i < base.size(); i++) {
                rmax = std::max(rmax, std::abs(ref[i]));
                emax = std::max(emax, std::abs(std::complex<double>(base[i].real(), base[i].imag()) - ref[i]));
            }
            const bool ref_ok = emax / rmax < 2e-6;
            fails += !ref_ok;
            std::printf("Q %s, EQ %s: whole set vs double reference: max error %.2e of max |y| %s\n",
                        inv ? "negated" : "as is", eq_on ? "on " : "off", emax / rmax, ref_ok ? "ok" : "FAIL");
            for (int b : blocks) {
                const std::vector<Complex> o = run(in, b, eq_on);
                size_t diff = 0;
                for (size_t i = 0; i < o.size(); i++) diff += (o[i] != base[i]);
                fails += diff != 0;
                std::printf("    block %5d: %s (%zu of %zu samples differ)\n", b, diff ? "FAIL" : "identical",
                            diff, o.size());
            }
        }
    }
    std::printf("%s\n", fails ? "FAILED" : "all checks passed");
    return fails ? 1 : 0;
}
