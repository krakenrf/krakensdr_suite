// VFO decimator benchmark + checks (arbitrary bandwidths, shared_decimator.cpp)
//
// For each bandwidth: the CPU of the cascade per input sample of ONE antenna
// (as % of a core at 2.4 MS/s), the output rate / bandwidth (what the rest of
// the pipeline - MUSIC, demodulators, decoders - pays per sample), the gain
// and the frequency response (tones through the real SharedDecimator).
// The same taps (SharedDecimator::designStages) in three implementations:
//   cccf  = liquid firdecim_cccf, complex taps - the cascade before
//           arbitrary bandwidths (only the BANDWIDTH_OPTIONS sizes)
//   crcf  = liquid firdecim_crcf, real taps (the first arbitrary-width build)
//   block = SharedDecimator::FirStage, the block FIR it runs now
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

enum class Impl { CCCF, CRCF, BLOCK };

struct Cascade {
    Impl impl;
    std::vector<firdecim_crcf> r;
    std::vector<firdecim_cccf> c;
    std::vector<SharedDecimator::FirStage> blk;
    std::vector<int> fac;
    std::vector<cf> a, b;
    Cascade(int bw_hz, Impl impl_) : impl(impl_) {
        for (auto& d : SharedDecimator::designStages(bw_hz)) {
            fac.push_back(d.factor);
            if (impl == Impl::CCCF) {
                std::vector<liquid_float_complex> h(d.taps.size());
                for (size_t i = 0; i < h.size(); i++) h[i] = liquid_float_complex{d.taps[i], 0.0f};
                c.push_back(firdecim_cccf_create(d.factor, h.data(), static_cast<unsigned>(h.size())));
            } else if (impl == Impl::CRCF) {
                r.push_back(firdecim_crcf_create(d.factor, d.taps.data(), static_cast<unsigned>(d.taps.size())));
            } else {
                blk.emplace_back(d.factor, d.taps);
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
        if (impl == Impl::BLOCK) {   // as SharedDecimator::decimateChannel runs them
            std::copy(in, in + n, blk[0].append(n));
            for (size_t s = 0; s < blk.size(); s++) {
                const size_t on = blk[s].pending();
                cf* dst;
                if (s + 1 == blk.size()) {
                    out.resize(on);
                    dst = out.data();
                } else {
                    dst = blk[s + 1].append(on);
                }
                blk[s].produce(dst);
            }
            return out.size();
        }
        const cf* src = in;
        size_t cur = n;
        for (size_t s = 0; s < fac.size(); s++) {
            const size_t on = cur / static_cast<size_t>(fac[s]);
            std::vector<cf>& dst = (s + 1 == fac.size()) ? out : ((s % 2 == 0) ? a : b);
            dst.resize(on);
            auto* x = const_cast<liquid_float_complex*>(reinterpret_cast<const liquid_float_complex*>(src));
            auto* y = reinterpret_cast<liquid_float_complex*>(dst.data());
            if (impl == Impl::CCCF) firdecim_cccf_execute_block(c[s], x, static_cast<unsigned>(on), y);
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

// ns of cascade time per input sample (whole blocks, D-aligned)
double bench(int bw_hz, Impl impl, const std::vector<cf>& x, double secs) {
    Cascade q(bw_hz, impl);
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

// max |b - a| / rms(a) on the same input (the same taps; D-aligned blocks
// give both the same outputs per block)
double diff_rms(int bw_hz, Impl ia, Impl ib, const std::vector<cf>& x) {
    Cascade qa(bw_hz, ia), qb(bw_hz, ib);
    const size_t D = static_cast<size_t>(decimation_for_bandwidth(bw_hz));
    const size_t len = x.size() / D * D;
    std::vector<cf> ya, yb;
    double md = 0, p = 0;
    size_t cnt = 0;
    for (int rep = 0; rep < 4; rep++) {
        qa.run(x.data(), len, ya);
        qb.run(x.data(), len, yb);
        if (ya.size() != yb.size()) return 1e9;   // they must line up exactly
        for (size_t i = 0; i < ya.size(); i++) {
            md = std::max(md, static_cast<double>(std::abs(ya[i] - yb[i])));
            p += std::norm(ya[i]);
        }
        cnt += ya.size();
    }
    const double rms = std::sqrt(p / std::max<size_t>(1, cnt));
    return rms > 0 ? md / rms : 0;
}

// SharedDecimator itself (offset 0: no mixer) fed blocks of odd lengths - its
// stages keep what they haven't used up - vs the liquid crcf cascade on the
// whole input at once: max |diff| / rms over the common stream (1e9 = the
// streams differ in length by more than one sample)
double stream_diff(int bw_hz, const std::vector<cf>& x) {
    SharedDecimator dec;
    dec.initialize(1);
    std::vector<cf> got;
    const size_t lens[] = {16384, 1000, 7, 333, 4096, 12345, 1, 9999};
    size_t at = 0;
    for (int k = 0; at < x.size(); k++) {
        size_t n = std::min(lens[k % 8], x.size() - at);
        const cf* p = x.data() + at;
        auto r = dec.decimateMultiChannel(&p, &n, 1, bw_hz, 0.0f);
        if (!r.channels.empty()) got.insert(got.end(), r.channels[0].samples.begin(), r.channels[0].samples.end());
        at += n;
    }
    Cascade ref(bw_hz, Impl::CRCF);
    std::vector<cf> want;
    const size_t D = static_cast<size_t>(decimation_for_bandwidth(bw_hz));
    ref.run(x.data(), x.size() / D * D, want);
    const size_t n = std::min(got.size(), want.size());
    if (got.size() > want.size() + 1 || want.size() > got.size() + 1) return 1e9;
    double md = 0, p = 0;
    for (size_t i = 0; i < n; i++) md = std::max(md, static_cast<double>(std::abs(got[i] - want[i]))), p += std::norm(want[i]);
    const double rms = std::sqrt(p / std::max<size_t>(1, n));
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

    std::cout.setstate(std::ios::failbit);   // SharedDecimator's own log lines

    std::printf("# 1) the old sizes (BANDWIDTH_OPTIONS): ns per input sample per antenna (%% of a core = ns x 0.24)\n");
    std::printf("%10s %5s %8s %8s %8s %8s %7s %10s %10s\n", "bw_Hz", "D", "cccf", "crcf", "block", "block_%", "speed", "diff/rms",
                "stream");
    double sc = 0, sr = 0, sb = 0, worst_diff = 0;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        const int bw = bandwidth_option_hz(i);
        const double c = bench(bw, Impl::CCCF, x, secs), r = bench(bw, Impl::CRCF, x, secs), b = bench(bw, Impl::BLOCK, x, secs);
        sc += c, sr += r, sb += b;
        const double d1 = diff_rms(bw, Impl::CCCF, Impl::BLOCK, x), d2 = stream_diff(bw, x);
        worst_diff = std::max({worst_diff, d1, d2});
        std::printf("%10d %5d %8.3f %8.3f %8.3f %8.2f %6.2fx %10.2e %10.2e\n", bw, decimation_for_bandwidth(bw), c, r, b, b * 0.24, c / b,
                    d1, d2);
    }
    std::printf("# mean over the old sizes: cccf %.3f ns, crcf %.3f ns, block %.3f ns (%.2fx the cccf, %.2fx the crcf)\n"
                "# (speed and diff/rms: block vs cccf, the decimator before arbitrary widths; stream: SharedDecimator fed\n"
                "#  blocks of odd lengths vs the crcf cascade on the whole input) - worst difference %.2e of rms\n\n",
                sc / NUM_BANDWIDTH_OPTIONS, sr / NUM_BANDWIDTH_OPTIONS, sb / NUM_BANDWIDTH_OPTIONS, sc / sb, sr / sb, worst_diff);

    std::printf("# 2) arbitrary widths (block): cascade cost, output rate / bandwidth, the nearest old size's cost\n");
    std::printf("%10s %5s %10s %6s %9s %7s %12s %9s %8s\n", "bw_Hz", "D", "rate_Hz", "rate/bw", "ns", "%", "near_old_Hz", "near_ns",
                "cost/old");
    double worst_ratio = 0;
    for (double l = std::log10(1000.0); l <= std::log10(2.4e6) + 1e-9; l += 0.05) {
        const int bw = clamp_bandwidth_hz(std::pow(10.0, l));
        const int D = decimation_for_bandwidth(bw);
        const double n = bench(bw, Impl::BLOCK, x, secs);
        const int near = bandwidth_option_hz(nearest_bandwidth_index(bw));
        const double nn = bench(near, Impl::BLOCK, x, secs);
        const double rate = SAMPLE_RATE / D;
        worst_ratio = std::max(worst_ratio, rate / bw);
        std::printf("%10d %5d %10.0f %6.3f %9.3f %7.2f %12d %9.3f %7.2fx\n", bw, D, rate, rate / bw, n, n * 0.24, near, nn, n / nn);
    }
    std::printf("# worst output rate / bandwidth: %.3f\n\n", worst_ratio);

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
