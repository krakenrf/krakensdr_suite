// Synthetic LTE / 5G NR cells on one carrier for testing the cells plugin
// offline: every cell from its own bearing (+ an optional reflection), with
// its own frame timing, carrier offset and power, received on kraken_doa's
// UCA (clockwise, ANT0 on +x, MUSIC's steering exp(+j 2 pi (x cos az + y sin
// az) / lambda)) through a 2.4 MHz VFO, plus independent noise per antenna.
// Writes a multichannel cf32 recording (sample-major: antenna 0..m-1 of
// sample 0, ...) and its .json sidecar (rate, channels, rf_hz, radius_m).
//
// The OFDM symbols are synthesised directly at the output rate (2.4 MHz =
// 160 samples per 15 kHz symbol): a symbol's (fractional) start and a path's
// delay are phase ramps over the subcarriers, so cyclic prefixes and delays
// are exact; only subcarriers inside the VFO's +-1.15 MHz are generated.
//
// Build (from kraken_doa_v2/plugins):
//   g++ -std=c++20 -O2 -Icells -Isdk -I../external/pffft cells/tools/cell_gen.cpp \
//       lib/build/libkrakendig.a -o /tmp/cell_gen        (pffft is in the lib)
// Run:
//   cell_gen [--secs 3] [--rf HZ] [--lte_off HZ] [--nr_off HZ] [--rxoff HZ] [--nrscs 15|30]
//            [--nrb 25] [--load 0.3] [--ssbper 20] [--snr DB] [--radius M] [--seed N]
//            [--fs 2.4e6] [--vis 1.15e6]   (a wider receiver: e.g. --fs 15.36e6 --vis 6.5e6;
//                                           the rate must be a whole number of samples per 15 kHz)
//            --cell TECH,PCI,AZ,DB[,TOFF_US[,CFO_HZ[,PORTS_OR_BEAMS[,R_AZ,R_DELAY_US,R_DB]]]] ... --out FILE.cf32
//   TECH: lte (FDD), ltetdd (TDD configuration 1), nr. LTE: PORTS 1 / 2 CRS
//   antenna ports; NR: BEAMS 1..4 SS/PBCH blocks per burst (case A / C
//   positions 2, 8, 16, 22), each a little weaker than the one before

#include "ofdm.hpp"
#include "seq.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cells;

namespace {

uint64_t mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

long fdiv(long a, long b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
long fmod_l(long a, long b) { return ((a % b) + b) % b; }

struct Path {
    double az = 0, delay_us = 0, db = 0;
};

struct CellCfg {
    std::string tech;       // lte, ltetdd, nr
    int pci = 0;
    double toff_us = 0, cfo = 0;
    int ports = 1, beams = 1;
    std::vector<Path> paths;
    int index = 0;
};

struct Gen {
    double fs = 2.4e6, load = 0.3, vis = 1.15e6;
    int nrb = 25, nr_mu = 0, ssbper_ms = 20;
    double lte_off = 0, nr_off = 0, rxoff = 0;
    uint64_t seed = 1;
};

const cd QPSK[4] = {cd(M_SQRT1_2, M_SQRT1_2), cd(-M_SQRT1_2, M_SQRT1_2), cd(M_SQRT1_2, -M_SQRT1_2), cd(-M_SQRT1_2, -M_SQRT1_2)};

// the cell's subcarrier values of global symbol g (f = subcarrier from the
// carrier / SSB centre); X[f + F] for f = -F..F
void cell_symbol(const Gen& G, const CellCfg& c, long g, int F, std::vector<cd>* X) {
    X->assign(static_cast<size_t>(2 * F + 1), cd(0, 0));
    auto data = [&](int f, int port) {
        const uint64_t h = mix64(G.seed ^ mix64(static_cast<uint64_t>(c.index) * 1000003ull + static_cast<uint64_t>(g) * 7919ull +
                                                static_cast<uint64_t>(f + 4096) * 31ull + static_cast<uint64_t>(port)));
        if (static_cast<double>(h >> 11) / 9007199254740992.0 >= G.load) return cd(0, 0);
        return QPSK[h & 3];
    };
    auto set = [&](int f, cd v) {
        if (std::abs(f) <= F) (*X)[static_cast<size_t>(f + F)] = v;
    };
    if (c.tech == "nr") {
        const int S = 7 << G.nr_mu;
        const long per = static_cast<long>(G.ssbper_ms) * 2 * S;   // symbols per SSB period
        const long sp = fmod_l(g, per);
        const int first[4] = {2, 8, 16, 22};
        int beam = -1, i = 0;
        for (int b = 0; b < c.beams && b < 4; b++)
            if (sp >= first[b] && sp < first[b] + 4) {
                beam = b;
                i = static_cast<int>(sp - first[b]);
            }
        const int half_bw = 150;   // 25 RBs
        for (int f = -half_bw; f < half_bw; f++)
            if (beam < 0 || f < -120 || f > 119) set(f, data(f, 0));
        if (beam >= 0) {
            const double bg = std::pow(10.0, -2.0 * beam / 20);   // each beam 2 dB weaker
            const int nid1 = c.pci / 3, nid2 = c.pci % 3;
            if (i == 0) {
                const auto d = nr::pss(nid2);
                for (int n = 0; n < 127; n++) set(nr::sync_f(n), bg * d[static_cast<size_t>(n)]);
            } else if (i == 2) {
                const auto d = nr::sss(nid1, nid2);
                for (int n = 0; n < 127; n++) set(nr::sync_f(n), bg * d[static_cast<size_t>(n)]);
                for (int f = -120; f <= -73; f++) set(f, bg * data(f, 1));
                for (int f = 72; f <= 119; f++) set(f, bg * data(f, 1));
            } else {
                for (int f = -120; f <= 119; f++) set(f, bg * data(f, 1));   // PBCH (+ its DMRS, here random too)
            }
        }
        return;
    }
    // LTE, normal cyclic prefix
    const bool tdd = c.tech == "ltetdd";
    const long h = fdiv(g, 7);
    const int l = static_cast<int>(fmod_l(g, 7));
    const int ns = static_cast<int>(fmod_l(h, 20));
    const int sf = ns / 2;
    if (tdd) {
        // configuration 1: D S U U D D S U U D; special subframe: DwPTS 10 symbols
        static const char cfg[11] = "DSUUDDSUUD";
        if (cfg[sf] == 'U') return;
        if (cfg[sf] == 'S' && (ns % 2 == 1) && l >= 3) return;
    }
    const int half = 6 * G.nrb;
    // data (port 0); the reference signals and sync signals overwrite it
    for (int f = -half; f <= half; f++)
        if (f != 0) set(f, data(f, 0));
    std::vector<cd> port1(static_cast<size_t>(2 * F + 1), cd(0, 0));
    if (l == 0 || l == 4) {
        const auto r = lte::crs_seq(c.pci, ns, l);
        for (int f = -half; f <= half; f++) {
            for (int p = 0; p < 2; p++) {
                int mp = 0;
                if (!lte::crs_re(f, l, p, c.pci, &mp)) continue;
                if (p < c.ports) {
                    if (p == 0) set(f, r[static_cast<size_t>(mp)]);
                    else {
                        set(f, 0);   // port 0 is silent on port 1's reference elements
                        if (std::abs(f) <= F) port1[static_cast<size_t>(f + F)] = r[static_cast<size_t>(mp)];
                    }
                }
            }
        }
    }
    const bool sync_sym = tdd ? ((ns == 1 || ns == 11) && l == 6) || ((ns == 2 || ns == 12) && l == 2)
                              : (ns == 0 || ns == 10) && (l == 5 || l == 6);
    if (sync_sym) {
        for (int f = -36; f <= 36; f++) set(f, 0);
        const bool pss = tdd ? l == 2 : l == 6;
        const auto d = pss ? lte::pss(c.pci % 3) : lte::sss(c.pci / 3, c.pci % 3, ns >= 10);
        for (int n = 0; n < 62; n++) set(lte::sync_f(n), d[static_cast<size_t>(n)]);
    }
    if (ns == 1 && l < 4)   // PBCH (random here)
        for (int f = -36; f <= 36; f++) {
            int mp;
            bool crs = false;
            for (int p = 0; p < 2; p++) crs = crs || lte::crs_re(f, l, p, c.pci, &mp);
            if (f != 0 && !crs) set(f, data(f, 2));
        }
    // the second antenna port goes through its own channel (same place: the
    // same direction, another complex gain)
    if (c.ports > 1) {
        const cd g1 = std::polar(1.0, 1.234 + 0.37 * c.pci);
        for (size_t i = 0; i < port1.size(); i++) (*X)[i] += g1 * port1[i];
    }
}

}  // namespace

int main(int argc, char** argv) {
    Gen G;
    std::string out = "/tmp/cells.cf32";
    double secs = 3, rf = 806e6, radius = -1, snr = 20;
    int M = 5;
    std::vector<CellCfg> cells_;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--secs") secs = std::stod(val());
        else if (a == "--rf") rf = std::stod(val());
        else if (a == "--lte_off") G.lte_off = std::stod(val());
        else if (a == "--nr_off") G.nr_off = std::stod(val());
        else if (a == "--rxoff") G.rxoff = std::stod(val());
        else if (a == "--nrscs") G.nr_mu = std::stoi(val()) == 30 ? 1 : 0;
        else if (a == "--nrb") G.nrb = std::stoi(val());
        else if (a == "--fs") G.fs = std::stod(val());
        else if (a == "--vis") G.vis = std::stod(val());
        else if (a == "--load") G.load = std::stod(val());
        else if (a == "--ssbper") G.ssbper_ms = std::stoi(val());
        else if (a == "--snr") snr = std::stod(val());
        else if (a == "--radius") radius = std::stod(val());
        else if (a == "--seed") G.seed = std::stoull(val());
        else if (a == "--out") out = val();
        else if (a == "--cell") {
            const std::string v = val();
            CellCfg c;
            char tech[16] = {0};
            double az = 0, db = 0, raz = 0, rd = 0, rdb = 0;
            int pb = 1;
            const int k = std::sscanf(v.c_str(), "%15[a-z],%d,%lf,%lf,%lf,%lf,%d,%lf,%lf,%lf", tech, &c.pci, &az, &db, &c.toff_us, &c.cfo, &pb, &raz,
                                      &rd, &rdb);
            if (k < 4) {
                fprintf(stderr, "bad --cell %s\n", v.c_str());
                return 2;
            }
            c.tech = tech;
            c.ports = c.beams = pb;
            c.paths.push_back({az, 0, db});
            if (k >= 10) c.paths.push_back({raz, rd, rdb});
            c.index = static_cast<int>(cells_.size());
            cells_.push_back(c);
        } else {
            fprintf(stderr, "unknown %s\n", a.c_str());
            return 2;
        }
    }
    if (cells_.empty()) {
        cells_.push_back({"lte", 101, 0, 0, 1, 1, {{40, 0, 0}}, 0});
        cells_.push_back({"lte", 102, 1234.5, 30, 2, 1, {{200, 0, -4}}, 1});
        cells_.push_back({"lte", 287, 3001.2, -20, 1, 1, {{300, 0, -9}}, 2});
    }
    const double lambda = 3e8 / rf;
    if (radius <= 0) radius = 0.425 * lambda;
    std::mt19937 rng(static_cast<unsigned>(G.seed));
    std::normal_distribution<double> gauss(0, 1);
    const double nsd = std::sqrt(std::pow(10.0, -snr / 10) / 2);

    struct PathRt {
        std::vector<cd> a;   // gain x steering per antenna
        double D;            // delay (samples) incl. the cell's frame offset
    };
    struct CellRt {
        Numerology num;
        int N, K0, F;
        double fr;           // residual frequency (Hz) after the whole-bin shift
        std::vector<PathRt> paths;
        Fft* ifft;
    };
    std::vector<Fft*> ffts;
    std::vector<CellRt> rt;
    for (auto& c : cells_) {
        CellRt r;
        r.num = Numerology{G.fs, c.tech == "nr" ? G.nr_mu : 0};
        r.N = r.num.nfft();
        const double scs = r.num.scs();
        const double off = (c.tech == "nr" ? G.nr_off : G.lte_off);
        r.K0 = static_cast<int>(std::lround(off / scs));
        r.fr = off - r.K0 * scs + G.rxoff + c.cfo;
        r.F = static_cast<int>(std::ceil((G.vis + std::fabs(off)) / scs)) + 1;
        for (const auto& p : c.paths) {
            PathRt pr;
            const double th = p.az * M_PI / 180;
            const cd g = std::polar(std::pow(10.0, p.db / 20), 2 * M_PI * std::uniform_real_distribution<double>(0, 1)(rng));
            for (int k = 0; k < M; k++) {
                const double ea = -2 * M_PI * k / M;
                pr.a.push_back(g * std::polar(1.0, 2 * M_PI * radius / lambda * std::cos(th - ea)));
            }
            pr.D = (c.toff_us + p.delay_us) * 1e-6 * G.fs;
            r.paths.push_back(pr);
        }
        ffts.push_back(new Fft(r.N, +1));
        r.ifft = ffts.back();
        rt.push_back(r);
    }

    std::ofstream fo(out, std::ios::binary);
    const long total = static_cast<long>(secs * G.fs);
    const long B = 24000;
    std::vector<cd> acc(static_cast<size_t>(B) * M);
    std::vector<cd> X;
    std::vector<cf> row(static_cast<size_t>(B) * M);
    for (long n0 = 0; n0 < total; n0 += B) {
        std::fill(acc.begin(), acc.end(), cd(0, 0));
        for (size_t ci = 0; ci < cells_.size(); ci++) {
            const CellCfg& c = cells_[ci];
            CellRt& r = rt[ci];
            const Numerology& num = r.num;
            const int S = num.syms_per_half(), N = r.N;
            const double half = num.half_ms();
            for (const auto& pr : r.paths) {
                // symbols overlapping [n0, n0 + B)
                const long h0 = static_cast<long>(std::floor((n0 - pr.D) / half)) - 1;
                for (long g = h0 * S;; g++) {
                    const long h = fdiv(g, S);
                    const int l = static_cast<int>(fmod_l(g, S));
                    const double u = h * half + num.useful(l) + pr.D;
                    const double st = h * half + num.sym_start(l) + pr.D;
                    if (st >= n0 + B) break;
                    if (u + N <= n0) continue;
                    cell_symbol(G, c, g, r.F, &X);
                    const long ui = static_cast<long>(std::floor(u));
                    const double uf = u - static_cast<double>(ui);
                    cf* Z = r.ifft->data();
                    std::fill(Z, Z + N, cf(0, 0));
                    for (int f = -r.F; f <= r.F; f++) {
                        const cd v = X[static_cast<size_t>(f + r.F)];
                        if (v == cd(0, 0)) continue;
                        const double fhz = f * num.scs() + (c.tech == "nr" ? G.nr_off : G.lte_off);
                        if (std::fabs(fhz) > G.vis) continue;
                        const int bn = f + r.K0;
                        if (bn <= -N / 2 || bn >= N / 2) continue;
                        const cd z = v * std::polar(1.0, -2 * M_PI * bn * uf / N);
                        Z[(bn + N) % N] += cf(static_cast<float>(z.real()), static_cast<float>(z.imag()));
                    }
                    r.ifft->run();
                    const cd sym_ph = std::polar(1.0 / std::sqrt(static_cast<double>(N)), 2 * M_PI * r.K0 * u / N);
                    const long a0 = std::max(n0, static_cast<long>(std::ceil(st))), a1 = std::min(n0 + B, static_cast<long>(std::ceil(u + N)));
                    for (long nn = a0; nn < a1; nn++) {
                        const cf zv = Z[fmod_l(nn - ui, N)];
                        const cd v = cd(zv.real(), zv.imag()) * sym_ph * std::polar(1.0, std::remainder(2 * M_PI * r.fr * static_cast<double>(nn) / G.fs, 2 * M_PI));
                        cd* o = &acc[static_cast<size_t>(nn - n0) * M];
                        for (int k = 0; k < M; k++) o[k] += pr.a[static_cast<size_t>(k)] * v;
                    }
                }
            }
        }
        const long nb = std::min(B, total - n0);
        for (long i = 0; i < nb * M; i++)
            row[static_cast<size_t>(i)] = cf(static_cast<float>(acc[static_cast<size_t>(i)].real() + nsd * gauss(rng)),
                                             static_cast<float>(acc[static_cast<size_t>(i)].imag() + nsd * gauss(rng)));
        fo.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(nb * M * sizeof(cf)));
    }
    std::ofstream js(out.substr(0, out.rfind('.')) + ".json");
    js << "{\"rate\": " << static_cast<long>(G.fs) << ", \"channels\": " << M << ", \"rf_hz\": " << static_cast<long>(rf)
       << ", \"radius_m\": " << radius << "}\n";
    printf("%s: %.2f s, %d antennas at %.0f Hz, RF %.6f MHz, UCA r %.4f m, LTE carrier %+.1f kHz, NR SSB %+.1f kHz from the VFO centre\n",
           out.c_str(), total / G.fs, M, G.fs, rf / 1e6, radius, G.lte_off / 1e3, G.nr_off / 1e3);
    for (const auto& c : cells_) {
        printf("  %-6s PCI %4d  az %6.1f  %5.1f dB  frame %+9.1f us  cfo %+6.0f Hz  %s %d", c.tech.c_str(), c.pci, c.paths[0].az, c.paths[0].db,
               c.toff_us, c.cfo, c.tech == "nr" ? "beams" : "ports", c.ports);
        if (c.paths.size() > 1) printf("  + reflection az %.1f at %+.2f us, %.1f dB", c.paths[1].az, c.paths[1].delay_us, c.paths[1].db);
        printf("\n");
    }
    for (auto* f : ffts) delete f;
    return 0;
}
