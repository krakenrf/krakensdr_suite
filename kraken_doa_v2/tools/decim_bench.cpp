// VFO decimator benchmark + checks (arbitrary bandwidths, shared_decimator.cpp)
//
// For each bandwidth: the CPU of the cascade per input sample of ONE antenna
// (as % of a core at 2.4 MS/s), the output rate / bandwidth (what the rest of
// the pipeline - MUSIC, demodulators, decoders - pays per sample), the gain
// and the frequency response (tones through the real SharedDecimator).
//   old  = the cascade before arbitrary bandwidths: complex taps (cccf),
//          only the BANDWIDTH_OPTIONS sizes
//   new  = SharedDecimator::designStages on real taps (crcf), any width
// Build (from kraken_doa_v2, after `make` built liquid-dsp), one line:
//   g++ -std=c++20 -Ofast -march=native -Iinclude -isystem liquid-dsp/build-kraken/include
//       tools/decim_bench.cpp src/signal_processing/shared_decimator.cpp
//       liquid-dsp/build-kraken/libliquid.a -lpthread -o decim_bench
// Run: taskset -c 3 ./decim_bench [seconds per measurement, default 0.4]
#include "signal_processing/shared_decimator.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using cf = std::complex<float>;

namespace {

constexpr size_t BLOCK = 16384;   // one heimdall packet per antenna

struct Cascade {
    bool complex_taps;
    std::vector<firdecim_crcf> r;
    std::vector<firdecim_cccf> c;
    std::vector<int> fac;
    std::vector<cf> a, b;
    Cascade(int bw_hz, bool complex_taps_) : complex_taps(complex_taps_) {
        for (auto& d : SharedDecimator::designStages(bw_hz)) {
            fac.push_back(d.factor);
            if (complex_taps) {
                std::vector<liquid_float_complex> h(d.taps.size());
                for (size_t i = 0; i < h.size(); i++) h[i] = liquid_float_complex{d.taps[i], 0.0f};
                c.push_back(firdecim_cccf_create(d.factor, h.data(), static_cast<unsigned>(h.size())));
            } else {
                r.push_back(firdecim_crcf_create(d.factor, d.taps.data(), static_cast<unsigned>(d.taps.size())));
            }
        }
    }
    ~Cascade() {
        for (auto q : r) firdecim_crcf_destroy(q);
        for (auto q : c) firdecim_cccf_destroy(q);
    }
    // n (multiple of the total factor) samples -> out
    size_t run(const cf* in, size_t n, std::vector<cf>& out) {
        if (fac.empty()) {
            out.assign(in, in + n);
            return n;
        }
        const cf* src = in;
        size_t cur = n;
        for (size_t s = 0; s < fac.size(); s++) {
            const size_t on = cur / static_cast<size_t>(fac[s]);
            std::vector<cf>& dst = (s + 1 == fac.size()) ? out : ((s % 2 == 0) ? a : b);
            dst.resize(on);
            auto* x = const_cast<liquid_float_complex*>(reinterpret_cast<const liquid_float_complex*>(src));
            auto* y = reinterpret_cast<liquid_float_complex*>(dst.data());
            if (complex_taps) firdecim_cccf_execute_block(c[s], x, static_cast<unsigned>(on), y);
            else firdecim_crcf_execute_block(r[s], x, static_cast<unsigned>(on), y);
            src = dst.data();
            cur = on;
        }
        return cur;
    }
};

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ns of cascade time per input sample (whole blocks, D-aligned like the decimator's carry)
double bench(int bw_hz, bool complex_taps, const std::vector<cf>& x, double secs) {
    Cascade q(bw_hz, complex_taps);
    const size_t D = static_cast<size_t>(decimation_for_bandwidth(bw_hz));
    const size_t n = x.size() / D * D;
    std::vector<cf> out;
    for (int i = 0; i < 3; i++) q.run(x.data(), n, out);   // warm up
    size_t done = 0;
    const double t0 = now_s();
    double t = t0;
    while (t - t0 < secs) {
        for (int i = 0; i < 8; i++) q.run(x.data(), n, out);
        done += 8 * n;
        t = now_s();
    }
    return (t - t0) * 1e9 / static_cast<double>(done);
}

// max |new - old| / rms(old) on the same input (efficient sizes: same taps)
double old_new_diff(int bw_hz, const std::vector<cf>& x) {
    Cascade o(bw_hz, true), n(bw_hz, false);
    const size_t D = static_cast<size_t>(decimation_for_bandwidth(bw_hz));
    const size_t len = x.size() / D * D;
    std::vector<cf> yo, yn;
    double md = 0, p = 0;
    for (int rep = 0; rep < 4; rep++) {
        o.run(x.data(), len, yo);
        n.run(x.data(), len, yn);
        for (size_t i = 0; i < yo.size(); i++) {
            md = std::max(md, static_cast<double>(std::abs(yo[i] - yn[i])));
            p += std::norm(yo[i]);
        }
    }
    const double rms = std::sqrt(p / std::max<size_t>(1, 4 * yo.size()));
    return rms > 0 ? md / rms : 0;
}

// gain of a tone at f_hz (from the VFO centre) through the real SharedDecimator
// (mixer included, VFO offset 0), relative to SAMPLE_RATE / bandwidth
double response_db(int bw_hz, double f_hz) {
    SharedDecimator dec;
    dec.initialize(1);
    std::vector<cf> x(BLOCK);
    const double w = 2 * M_PI * f_hz / SAMPLE_RATE;
    double ph = 0;
    double p = 0;
    size_t cnt = 0;
    const int blocks = std::max(8, static_cast<int>(40.0 * decimation_for_bandwidth(bw_hz) * 64 / BLOCK) + 8);
    for (int b = 0; b < blocks; b++) {
        for (auto& v : x) {
            v = cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            ph += w;
        }
        ph = std::fmod(ph, 2 * M_PI);
        const cf* ptr = x.data();
        const size_t len = x.size();
        auto r = dec.decimateMultiChannel(&ptr, &len, 1, bw_hz, 0.0f);
        if (b < blocks / 2 || r.channels.empty()) continue;   // filters settled
        for (auto& v : r.channels[0].samples) p += std::norm(v), cnt++;
    }
    const double g = std::sqrt(p / std::max<size_t>(1, cnt));
    const double nominal = static_cast<double>(SAMPLE_RATE) / bw_hz;
    return 20 * std::log10(std::max(g / nominal, 1e-12));
}

}  // namespace

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 0.4;
    std::mt19937 rng(1);
    std::normal_distribution<float> g(0.0f, 0.3f);
    std::vector<cf> x(BLOCK * 4);
    for (auto& v : x) v = cf(g(rng), g(rng));

    std::printf("# 1) the old sizes (BANDWIDTH_OPTIONS): old cccf vs new crcf, ns per input sample, %% of a core per antenna\n");
    std::printf("%10s %5s %9s %9s %7s %7s %6s %10s\n", "bw_Hz", "D", "old_ns", "new_ns", "old_%", "new_%", "speed", "diff/rms");
    double so = 0, sn = 0;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        const int bw = bandwidth_option_hz(i);
        const double o = bench(bw, true, x, secs), n = bench(bw, false, x, secs);
        so += o, sn += n;
        std::printf("%10d %5d %9.3f %9.3f %7.2f %7.2f %5.2fx %10.2e\n", bw, decimation_for_bandwidth(bw), o, n, o * 0.24, n * 0.24, o / n,
                    old_new_diff(bw, x));
    }
    std::printf("# mean over the old sizes: old %.3f ns, new %.3f ns (%.2fx)\n\n", so / NUM_BANDWIDTH_OPTIONS, sn / NUM_BANDWIDTH_OPTIONS,
                so / sn);

    std::printf("# 2) arbitrary widths (new): cascade cost, output rate / bandwidth, the nearest old size's cost\n");
    std::printf("%10s %5s %10s %6s %9s %7s %12s %9s %8s\n", "bw_Hz", "D", "rate_Hz", "rate/bw", "new_ns", "new_%", "near_old_Hz", "near_ns",
                "cost/old");
    double worst_ratio = 0;
    for (double l = std::log10(1000.0); l <= std::log10(2.4e6) + 1e-9; l += 0.05) {
        const int bw = clamp_bandwidth_hz(std::pow(10.0, l));
        const int D = decimation_for_bandwidth(bw);
        const double n = bench(bw, false, x, secs);
        const int near = bandwidth_option_hz(nearest_bandwidth_index(bw));
        const double nn = bench(near, false, x, secs);
        const double rate = SAMPLE_RATE / D;
        worst_ratio = std::max(worst_ratio, rate / bw);
        std::printf("%10d %5d %10.0f %6.3f %9.3f %7.2f %12d %9.3f %7.2fx\n", bw, D, rate, rate / bw, n, n * 0.24, near, nn, n / nn);
    }
    std::printf("# worst output rate / bandwidth: %.3f\n\n", worst_ratio);

    std::cout.setstate(std::ios::failbit);   // SharedDecimator's own log lines
    std::printf("# 3) response (dB re SAMPLE_RATE / bandwidth) of a tone f from the VFO centre (the band = +-0.5 bw)\n");
    std::printf("%10s %8s %8s %8s %8s %8s %8s %10s\n", "bw_Hz", "f=0", "0.3bw", "0.4bw", "0.5bw", "0.6bw", "0.8bw", "rate-0.3bw");
    for (int bw : {1000, 1700, 6250, 12500, 15000, 25000, 33333, 100000, 250000, 333333, 1000000, 1500000, 2000000}) {
        const double rate = SAMPLE_RATE / decimation_for_bandwidth(bw);
        // the last column: a tone outside the band that would alias onto -0.3 bw
        // (within the input's +-1.2 MHz)
        const double alias_f = std::min(rate - 0.3 * bw, 1.19e6);
        std::printf("%10d %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f\n", bw, response_db(bw, 0), response_db(bw, 0.3 * bw),
                    response_db(bw, 0.4 * bw), response_db(bw, 0.5 * bw), response_db(bw, 0.6 * bw), response_db(bw, 0.8 * bw),
                    response_db(bw, alias_f));
    }
    std::printf("# (above 1.5 MHz the 0.8bw tone is past the input's +-1.2 MHz: it wraps into the band)\n");
    return 0;
}
