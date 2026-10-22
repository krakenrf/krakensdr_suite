// VFO decimator benchmark + checks (shared_decimator.cpp)
//
// For each bandwidth: the plan (SharedDecimator::planStages - its stages,
// "8F:31 5:23 2h:39" = factor 8 with the mixer folded in, 31 taps, ...,
// factor 2 as a halfband), the cost model's estimate and the measured cost
// of SharedDecimator::decimateChannel per input sample of ONE antenna (%
// of a core at 2.4 MS/s = ns x 0.24) with the VFO on the tuner centre (no
// mixer) and off it (the mixer in front / folded into the first stage), the
// output rate / bandwidth (what the rest of the pipeline - MUSIC,
// demodulators, decoders - pays per sample), and the decimator fed blocks of
// odd lengths (1, 7, 31, 333, 1025 ... - every stage keeps what it hasn't
// used up; a first stage filtering straight from the block reaches back into
// its history) against a plain double-precision reference of the same plan (the mixer at
// the input rate, every stage a full convolution): max |diff| / rms. Then
// several VFOs in one pass over the antennas (decimateVfos) against each
// VFO alone (must be bit for bit the same), and the frequency response
// (tones through the real SharedDecimator, with the VFO on the centre and
// 300 kHz off it).
// Build (from kraken_doa_v2, after `make` built liquid-dsp), one line:
//   g++ -std=c++20 -Ofast -march=native -Iinclude -isystem liquid-dsp/build-kraken/include
//       tools/decim_bench.cpp src/signal_processing/shared_decimator.cpp
//       liquid-dsp/build-kraken/libliquid.a -lpthread -o decim_bench
// Run: taskset -c 3 ./decim_bench [seconds per measurement, default 0.3]
#include "signal_processing/shared_decimator.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <vector>

using cf = std::complex<float>;
using cd = std::complex<double>;

namespace {

constexpr size_t BLOCK = 16384;     // one heimdall packet per antenna
constexpr float OFFSET_HZ = 37000.0f;

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ns of decimateChannel time per input sample (one antenna, blocks of BLOCK)
double bench(int bw_hz, float offset_hz, const std::vector<cf>& x, double secs) {
    SharedDecimator dec;
    dec.initialize(1);
    const size_t nb = x.size() / BLOCK;
    size_t k = 0;
    auto run = [&] {
        dec.decimateChannel(x.data() + (k++ % nb) * BLOCK, BLOCK, 0, bw_hz, offset_hz);
    };
    for (int i = 0; i < 8; i++) run();   // warm up
    size_t done = 0;
    const double t0 = now_s();
    double t = t0;
    while (t - t0 < secs) {
        for (int i = 0; i < 16; i++) run();
        done += 16 * BLOCK;
        t = now_s();
    }
    return (t - t0) * 1e9 / static_cast<double>(done);
}

// The mixer's rad per input sample as the decimator realises it: its float
// step (in front), or the folded stage's float step per output / its factor
double realised_w(const SharedDecimator::Plan& p, float offset_hz) {
    if (std::fabs(offset_hz) <= 1.0f) return 0.0;
    const double w = 2.0 * M_PI * offset_hz / SAMPLE_RATE_HZ;
    if (p.fold && !p.stages.empty() && !p.stages[0].half) {
        const int M = p.stages[0].factor;
        const cf ps(static_cast<float>(std::cos(-w * M)), static_cast<float>(std::sin(-w * M)));
        return -std::arg(cd(ps.real(), ps.imag())) / M;
    }
    const cf s(static_cast<float>(std::cos(-w)), static_cast<float>(std::sin(-w)));
    return -std::arg(cd(s.real(), s.imag()));
}

// The plan's cascade in double precision on the whole input: the mixer at
// the input rate (phase 1 at sample 0), then every stage y[j] = sum_k
// taps[k] x[j M - k] (zeros before the first sample)
std::vector<cd> reference(const SharedDecimator::Plan& p, const std::vector<cf>& in, double w) {
    std::vector<cd> x(in.size());
    for (size_t n = 0; n < in.size(); n++) x[n] = cd(in[n].real(), in[n].imag()) * std::polar(1.0, -w * static_cast<double>(n));
    for (auto& s : p.stages) {
        const size_t M = static_cast<size_t>(s.factor), L = s.taps.size();
        std::vector<cd> y(x.empty() ? 0 : (x.size() - 1) / M + 1);
        for (size_t j = 0; j < y.size(); j++) {
            cd acc = 0;
            const size_t nj = j * M;
            for (size_t k = 0; k < L && k <= nj; k++) acc += static_cast<double>(s.taps[k]) * x[nj - k];
            y[j] = acc;
        }
        x.swap(y);
    }
    return x;
}

// SharedDecimator fed blocks of odd lengths vs the reference: max |diff| /
// rms (1e9 = the streams differ in length)
double stream_check(int bw_hz, float offset_hz, const std::vector<cf>& x) {
    SharedDecimator dec;
    dec.initialize(1);
    std::vector<cf> got;
    // around typical tap counts (a block shorter than a window: the windows
    // that reach back into the history) and the 1024-sample chunks
    const size_t lens[] = {16384, 1000, 7, 333, 4096, 12345, 1, 9999, 2, 3, 30, 31, 63, 64, 65, 145, 146, 1023, 1024, 1025};
    const size_t nlens = sizeof lens / sizeof lens[0];
    size_t at = 0;
    for (size_t k = 0; at < x.size(); k++) {
        size_t n = std::min(lens[k % nlens], x.size() - at);
        const cf* p = x.data() + at;
        auto r = dec.decimateMultiChannel(&p, &n, 1, bw_hz, offset_hz);
        if (!r.channels.empty()) got.insert(got.end(), r.channels[0].samples.begin(), r.channels[0].samples.end());
        at += n;
    }
    const auto plan = SharedDecimator::planStages(bw_hz);
    const auto want = reference(plan, x, realised_w(plan, offset_hz));
    if (got.size() != want.size()) return 1e9;
    double md = 0, pw = 0;
    for (size_t i = 0; i < want.size(); i++) {
        md = std::max(md, std::abs(cd(got[i].real(), got[i].imag()) - want[i]));
        pw += std::norm(want[i]);
    }
    const double rms = std::sqrt(pw / std::max<size_t>(1, want.size()));
    return rms > 0 ? md / rms : 0;
}

// SharedDecimator::decimateVfos (every VFO in one pass over each input,
// chunk by chunk) against each VFO's own decimateMultiChannel, blocks of odd
// lengths: 4 antennas, VFOs of every kind - folded, the mixer in front, a
// halfband first stage, the whole band, a full-rate filter - and two on one
// antenna alone (independent mode: they share that antenna's task with the
// others). The largest |difference| of any output sample: 0 = bit for bit
// (1e9 = a count or a length differs)
double shared_check(const std::vector<cf>& x) {
    constexpr size_t NA = 4;
    struct V {
        int bw;
        float off;
        int ant;   // -1 = every antenna, else that one alone
    };
    const V vs[] = {{12500, 100000.0f, -1}, {240000, -300000.0f, -1}, {1200000, 0.0f, -1}, {2400000, 250000.0f, -1},
                    {1500000, 37000.0f, -1}, {26000, 0.0f, -1}, {33333, 450000.0f, 2}, {1000, -620000.0f, 2}};
    constexpr size_t NV = sizeof vs / sizeof vs[0];
    SharedDecimator shared[NV], alone[NV];
    for (size_t v = 0; v < NV; v++) shared[v].initialize(NA), alone[v].initialize(NA);
    // the antennas: x from different points (each its own array)
    constexpr size_t LEN = 3 * BLOCK;
    std::vector<std::vector<cf>> ant(NA, std::vector<cf>(LEN));
    for (size_t a = 0; a < NA; a++) {
        for (size_t i = 0; i < LEN; i++) ant[a][i] = x[(i + a * 5003) % x.size()];
    }
    const size_t lens[] = {16384, 1000, 7, 333, 4096, 12345, 1, 1023, 1025, 9999, 2, 64};
    const size_t nlens = sizeof lens / sizeof lens[0];
    double worst = 0;
    size_t at = 0;
    for (size_t k = 0; at < LEN; k++) {
        const size_t n = std::min(lens[k % nlens], LEN - at);
        SharedDecimator::MultiChannelDecimated got[NV];
        SharedDecimator::VfoBlock blocks[NV];
        for (size_t v = 0; v < NV; v++) {
            SharedDecimator::VfoBlock& b = blocks[v];
            b.decimator = &shared[v];
            b.bandwidth_hz = vs[v].bw;
            b.freq_offset_hz = vs[v].off;
            b.out = &got[v];
            for (size_t a = 0; a < NA; a++) {
                if (vs[v].ant >= 0 && static_cast<size_t>(vs[v].ant) != a) continue;
                b.ptrs[b.num_channels] = ant[a].data() + at;
                b.lens[b.num_channels] = n;
                b.num_channels++;
            }
        }
        SharedDecimator::decimateVfos(blocks, NV);
        for (size_t v = 0; v < NV; v++) {
            const auto want = alone[v].decimateMultiChannel(blocks[v].ptrs, blocks[v].lens, blocks[v].num_channels,
                                                            vs[v].bw, vs[v].off);
            if (want.min_samples != got[v].min_samples || want.channels.size() != got[v].channels.size()) return 1e9;
            for (size_t c = 0; c < want.channels.size(); c++) {
                const auto& w = want.channels[c].samples;
                const auto& g = got[v].channels[c].samples;
                if (w.size() != g.size()) return 1e9;
                for (size_t i = 0; i < w.size(); i++) worst = std::max(worst, static_cast<double>(std::abs(w[i] - g[i])));
            }
        }
        at += n;
    }
    return worst;
}

// Gain of a tone f_hz from the VFO centre (the VFO offset_hz from the tuner
// centre) through the real SharedDecimator, relative to SAMPLE_RATE / bandwidth
double response_db(int bw_hz, double f_hz, float offset_hz = 0.0f) {
    SharedDecimator dec;
    dec.initialize(1);
    std::vector<cf> x(BLOCK);
    const double w = 2 * M_PI * (f_hz + offset_hz) / SAMPLE_RATE;
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
        auto r = dec.decimateMultiChannel(&ptr, &len, 1, bw_hz, offset_hz);
        if (b < blocks / 2 || r.channels.empty()) continue;   // filters settled
        for (auto& v : r.channels[0].samples) p += std::norm(v), cnt++;
    }
    const double g = std::sqrt(p / std::max<size_t>(1, cnt));
    const double nominal = static_cast<double>(SAMPLE_RATE) / bw_hz;
    return 20 * std::log10(std::max(g / nominal, 1e-12));
}

}  // namespace

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 0.3;
    std::mt19937 rng(1);
    std::normal_distribution<float> g(0.0f, 0.3f);
    std::vector<cf> x(BLOCK * 4);
    for (auto& v : x) v = cf(g(rng), g(rng));
    std::vector<cf> xs(BLOCK * 3);   // the stream checks
    for (auto& v : xs) v = cf(g(rng), g(rng));

    std::cout.setstate(std::ios::failbit);   // SharedDecimator's own log lines

    std::printf("# ns per input sample per antenna (%% of a core = ns x 0.24): the model's estimate (offset != 0),\n"
                "# measured on the tuner centre / %.0f kHz off it; check = max |diff| / rms vs the reference (centre, off)\n",
                OFFSET_HZ / 1e3);
    std::printf("%10s %5s %7s %8s %8s %8s %9s %9s  %s\n", "bw_Hz", "D", "rate/bw", "model", "centre", "offset", "chk_c", "chk_o",
                "plan");
    double worst = 0, sum_c = 0, sum_o = 0, sum_m = 0;
    int cnt = 0;
    auto row = [&](int bw) {
        const auto plan = SharedDecimator::planStages(bw);
        const int D = decimation_for_bandwidth(bw);
        const double c = bench(bw, 0.0f, x, secs), o = bench(bw, OFFSET_HZ, x, secs);
        const double k0 = stream_check(bw, 0.0f, xs), k1 = stream_check(bw, OFFSET_HZ, xs);
        worst = std::max({worst, k0, k1});
        sum_c += c, sum_o += o, sum_m += plan.cost_ns, cnt++;
        std::printf("%10d %5d %7.3f %8.3f %8.3f %8.3f %9.1e %9.1e  %s\n", bw, D, SAMPLE_RATE / D / bw, plan.cost_ns, c, o, k0, k1,
                    plan.describe().c_str());
    };
    std::printf("# 1) the efficient sizes (bandwidth = rate: every old BANDWIDTH_OPTIONS size is one)\n");
    for (int d : efficient_decimations()) row(SAMPLE_RATE_HZ / d);
    std::printf("# mean: model %.3f, centre %.3f, offset %.3f ns\n", sum_m / cnt, sum_c / cnt, sum_o / cnt);
    sum_c = sum_o = sum_m = 0, cnt = 0;
    std::printf("# 2) arbitrary widths\n");
    for (double l = std::log10(1000.0); l <= std::log10(2.4e6) + 1e-9; l += 0.1) {
        const int bw = clamp_bandwidth_hz(std::pow(10.0, l));
        if (!bandwidth_is_efficient(bw)) row(bw);
    }
    for (int bw : {250000, 500000, 700000, 1000000, 1250000, 1500000, 2000000}) row(bw);
    std::printf("# mean: model %.3f, centre %.3f, offset %.3f ns; worst check %.2e of rms\n\n", sum_m / cnt, sum_c / cnt,
                sum_o / cnt, worst);

    std::printf("# 3) decimateVfos - 8 VFOs, 4 antennas in one pass - vs each VFO alone: max |diff| %.3g (0 = bit for bit)\n\n",
                shared_check(xs));

    std::printf("# 4) response (dB re SAMPLE_RATE / bandwidth) of a tone f from the VFO centre (the band = +-0.5 bw)\n");
    std::printf("%10s %7s %8s %8s %8s %8s %8s %8s %10s\n", "bw_Hz", "vfo_kHz", "f=0", "0.3bw", "0.4bw", "0.5bw", "0.6bw", "0.8bw",
                "rate-0.3bw");
    for (int bw : {1000, 1700, 6250, 12500, 15000, 25000, 33333, 100000, 250000, 300000, 333333, 600000, 1000000, 1200000, 1500000,
                   2000000}) {
        for (float off : {0.0f, 300000.0f}) {
            if (off != 0.0f && bw > 1000000) continue;   // the tones stay inside the input's +-1.2 MHz
            const double rate = SAMPLE_RATE / decimation_for_bandwidth(bw);
            // the last column: a tone outside the band that would alias onto -0.3 bw
            // (within the input's +-1.2 MHz)
            const double alias_f = std::min(rate - 0.3 * bw, 1.19e6 - off);
            std::printf("%10d %7.0f %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f\n", bw, off / 1e3, response_db(bw, 0, off),
                        response_db(bw, 0.3 * bw, off), response_db(bw, 0.4 * bw, off), response_db(bw, 0.5 * bw, off),
                        response_db(bw, 0.6 * bw, off), response_db(bw, 0.8 * bw, off), response_db(bw, alias_f, off));
        }
    }
    std::printf("# (above 1.5 MHz the 0.8bw tone is past the input's +-1.2 MHz: it wraps into the band)\n");
    return 0;
}
