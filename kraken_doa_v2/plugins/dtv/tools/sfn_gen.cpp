// Synthetic single frequency network for testing the dtv plugin offline:
// one broadcast signal (DVB-T, ISDB-T, ATSC 1.0 ...) sent by several
// transmitters, each from its own bearing with its own delay and power,
// received on kraken_doa's UCA (clockwise, ANT0 on +x, MUSIC's steering
// exp(+j 2 pi (x cos az + y sin az) / lambda), c = 3e8), seen through a VFO
// of `rate` Hz whose centre is `offset` Hz from the channel centre, plus
// independent noise per antenna. Writes a multichannel cf32 recording
// (sample-major: antenna 0..m-1 of sample 0, ...) and its .json sidecar.
// DVB-T carries real TPS frames (16-QAM 2/3, the guard / mode generated,
// cell ID --cell, BCH parity) and ISDB-T real TMCC (layer A 1 segment QPSK
// 2/3 + partial reception, layer B 12 segments 64-QAM 3/4, --ews 1 sets the
// emergency warning start flag; the TMCC parity bits are left 0).
//
// Build (from kraken_doa_v2/plugins):
//   g++ -std=c++20 -O2 -Idtv -Isdk dtv/tools/sfn_gen.cpp -o /tmp/sfn_gen
// Run:
//   sfn_gen --std dvbt|isdbt|dvbt2|atsc|atsc3|dtmb [--mode 2k|8k] [--gi 4|8|16|32] [--bw 8] [--secs 3]
//           [--rate 2.4e6] [--offset HZ] [--rf HZ] [--radius M] [--snr DB] [--cell ID] [--ews 0|1]
//           [--tx az,delay_us,db[,doppler_hz]]... --out FILE.cf32

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "std/atsc3_tables.hpp"
#include "std/atsc_tables.hpp"
#include "std/dtmb_tables.hpp"
#include "std/dvbt2_tables.hpp"
#include "std/dvbt_tables.hpp"
#include "std/isdbt_tables.hpp"

using cd = std::complex<double>;
using cf = std::complex<float>;

namespace {

std::mt19937 rng(12345);
std::normal_distribution<double> gauss(0, 1);
std::uniform_int_distribution<int> bit(0, 1);

// inverse DFT of a centred carrier array (carrier index c at frequency c / Tu)
// into nfft time samples, + cyclic prefix
void ofdm_symbol(const std::vector<std::pair<int, cd>>& cells, int nfft, int ncp, std::vector<cd>* out) {
    std::vector<cd> X(static_cast<size_t>(nfft));
    for (const auto& [c, v] : cells) X[static_cast<size_t>(((c % nfft) + nfft) % nfft)] += v;
    // naive inverse FFT by recursion would be slow: radix-2 iterative
    int n = nfft;
    std::vector<cd> a = X;
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(j)]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const cd w = std::polar(1.0, 2 * M_PI / len);
        for (int i = 0; i < n; i += len) {
            cd wn = 1;
            for (int j = 0; j < len / 2; j++) {
                const cd u = a[static_cast<size_t>(i + j)], v = a[static_cast<size_t>(i + j + len / 2)] * wn;
                a[static_cast<size_t>(i + j)] = u + v;
                a[static_cast<size_t>(i + j + len / 2)] = u - v;
                wn *= w;
            }
        }
    }
    const double sc = 1.0 / std::sqrt(static_cast<double>(nfft));
    out->clear();
    for (int i = nfft - ncp; i < nfft; i++) out->push_back(a[static_cast<size_t>(i)] * sc);
    for (int i = 0; i < nfft; i++) out->push_back(a[static_cast<size_t>(i)] * sc);
}

cd qam16() {
    const int lv[4] = {-3, -1, 1, 3};
    return cd(lv[bit(rng) * 2 + bit(rng)], lv[bit(rng) * 2 + bit(rng)]) / std::sqrt(10.0);
}

// --- sources: next block of the common signal at the source rate ---------------
struct Source {
    double fs = 0;
    std::function<void(std::vector<cd>*)> next;   // appends one block
};

// DVB-T TPS bits s0..s67 of frame f (0..3 = frames 1..4; EN 300 744 4.6.2):
// 16-QAM, non-hierarchical, code rate 2/3, the guard / mode generated, cell
// ID high byte in frames 1 / 3, low byte in 2 / 4, BCH(67,53) parity
std::array<uint8_t, 68> dvbt_tps_bits(int f, bool m8k, int gi_den, int cell) {
    std::array<uint8_t, 68> s{};
    int i = 1;
    auto put = [&](uint32_t v, int n) {
        for (int b = n - 1; b >= 0; b--) s[static_cast<size_t>(i++)] = static_cast<uint8_t>((v >> b) & 1u);
    };
    put(f % 2 == 0 ? dvbt_std::TPS_SYNC_FRAMES_1_3 : dvbt_std::TPS_SYNC_FRAMES_2_4, 16);
    put(31, 6);                                                   // length: with the cell ID
    put(static_cast<uint32_t>(f), 2);
    put(1, 2);                                                    // 16-QAM
    put(0, 3);                                                    // non-hierarchical
    put(1, 3);                                                    // HP 2/3
    put(0, 3);                                                    // LP
    put(gi_den == 32 ? 0 : gi_den == 16 ? 1 : gi_den == 8 ? 2 : 3, 2);
    put(m8k ? 1 : 0, 2);
    put(static_cast<uint32_t>(f % 2 == 0 ? (cell >> 8) & 0xFF : cell & 0xFF), 8);
    put(0, 6);                                                    // DVB-H + reserved
    // parity: (s1..s53) x^14 mod g(x), g = x^14 + x^9 + x^8 + x^6 + x^5 + x^4 + x^2 + x + 1
    uint32_t r = 0;
    for (int k = 1; k <= 53; k++) {
        const uint32_t fb = ((r >> 13) & 1u) ^ s[static_cast<size_t>(k)];
        r = (r << 1) & 0x3FFF;
        if (fb) r ^= 0x0377;
    }
    put(r, 14);
    return s;
}

Source dvbt_source(bool m8k, int gi_den, int bw, int cell) {
    Source s;
    s.fs = dvbt_std::sample_rate_hz(bw);
    const int nfft = m8k ? 8192 : 2048, ncp = nfft / gi_den;
    const int kmax = m8k ? dvbt_std::KMAX_8K : dvbt_std::KMAX_2K, kc = kmax / 2;
    std::vector<std::array<uint8_t, 68>> tps;
    for (int f = 0; f < 4; f++) tps.push_back(dvbt_tps_bits(f, m8k, gi_den, cell));
    auto l = std::make_shared<int>(0);
    s.next = [=](std::vector<cd>* out) {
        std::vector<std::pair<int, cd>> cells;
        const int sym = *l % 68;
        const auto& tb = tps[static_cast<size_t>((*l / 68) % 4)];
        for (int k = 0; k <= kmax; k++) {
            cd v;
            if (dvbt_std::is_scattered(k, sym) || dvbt_std::is_continual(k, m8k)) v = dvbt_std::pilot_value(k);
            else if (dvbt_std::is_tps(k, m8k)) v = dvbt_std::tps_value(k, sym, tb);
            else v = qam16();
            cells.push_back({k - kc, v});
        }
        std::vector<cd> t;
        ofdm_symbol(cells, nfft, ncp, &t);
        out->insert(out->end(), t.begin(), t.end());
        ++*l;
    };
    return s;
}

// ISDB-T TMCC bits B0..B203 of frame f (ARIB STD-B31 3.15): sync word w0 /
// w1 alternating, synchronous segments, ISDB-T, no parameter switching,
// layer A 1 segment QPSK 2/3 interleave code 2 (partial reception), layer B
// 12 segments 64-QAM 3/4 code 1, no layer C, next = current, reserved bits
// 1, parity 0
std::array<uint8_t, 204> isdbt_tmcc_bits(int f, bool ews) {
    std::array<uint8_t, 204> b{};
    int i = 1;
    auto put = [&](uint32_t v, int n) {
        for (int k = n - 1; k >= 0; k--) b[static_cast<size_t>(i++)] = static_cast<uint8_t>((v >> k) & 1u);
    };
    const char* w = f % 2 == 0 ? isdbt_std::kTmccSyncW0 : isdbt_std::kTmccSyncW1;
    for (int k = 0; k < 16; k++) put(static_cast<uint32_t>(w[k] - '0'), 1);
    put(0, 3);                                                    // synchronous segments
    put(0, 2);                                                    // ISDB-T
    put(15, 4);                                                   // no switching
    put(ews ? 1 : 0, 1);
    for (int rep = 0; rep < 2; rep++) {                           // current, next
        put(1, 1);                                                // partial reception
        put(1, 3), put(1, 3), put(2, 3), put(1, 4);               // A: QPSK 2/3, I code 2, 1 segment
        put(3, 3), put(2, 3), put(1, 3), put(12, 4);              // B: 64-QAM 3/4, I code 1, 12 segments
        put(7, 3), put(7, 3), put(7, 3), put(15, 4);              // C: unused
    }
    put(7, 3);                                                    // phase shift correction
    put(0xFFF, 12);                                               // reserved
    return b;
}

// ISDB-T, all segments coherent (16QAM data): scattered pilots every 12
// carriers moving 3 per symbol, values from the whole-band PRBS; the top
// carrier a continual pilot; TMCC carriers DBPSK (B'0 = Wi, B'k = B'k-1 xor
// Bk), AC carriers random BPSK
Source isdbt_source(int mode, int gi_den, bool ews) {
    Source s;
    s.fs = isdbt_std::kIfftSampleRateHz;
    const int nfft = isdbt_std::fft_size(mode), ncp = nfft / gi_den;
    const int K = isdbt_std::total_carriers(mode), kc = isdbt_std::center_carrier(mode);
    const auto w = isdbt_std::prbs_w_band(mode);
    // the carriers' kind: 1 = TMCC, 2 = AC
    std::vector<int> kind(static_cast<size_t>(K), 0);
    for (int seg = 0; seg < isdbt_std::kNumSegments; seg++) {
        const int k0 = isdbt_std::segment_first_carrier(mode, seg);
        for (int c : isdbt_std::tmcc_carriers(mode, seg, false)) kind[static_cast<size_t>(k0 + c)] = 1;
        for (int c : isdbt_std::ac1_carriers(mode, seg, false)) kind[static_cast<size_t>(k0 + c)] = 2;
    }
    std::vector<std::array<uint8_t, 204>> tmcc = {isdbt_tmcc_bits(0, ews), isdbt_tmcc_bits(1, ews)};
    auto l = std::make_shared<int>(0);
    auto bp = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(K), 0);   // B' per TMCC carrier
    s.next = [=](std::vector<cd>* out) {
        std::vector<std::pair<int, cd>> cells;
        const int sym = *l % 204;
        const auto& tb = tmcc[static_cast<size_t>((*l / 204) % 2)];
        for (int k = 0; k < K; k++) {
            cd v;
            if (k == K - 1 || k % 12 == 3 * (*l % 4)) v = cd(w[static_cast<size_t>(k)] ? -4.0 / 3 : 4.0 / 3, 0);
            else if (kind[static_cast<size_t>(k)] == 1) {
                uint8_t& p = (*bp)[static_cast<size_t>(k)];
                p = sym == 0 ? w[static_cast<size_t>(k)] : static_cast<uint8_t>(p ^ tb[static_cast<size_t>(sym)]);
                v = cd(p ? -4.0 / 3 : 4.0 / 3, 0);
            } else if (kind[static_cast<size_t>(k)] == 2) v = cd(bit(rng) ? -4.0 / 3 : 4.0 / 3, 0);
            else v = qam16();
            cells.push_back({k - kc, v});
        }
        std::vector<cd> t;
        ofdm_symbol(cells, nfft, ncp, &t);
        out->insert(out->end(), t.begin(), t.end());
        ++*l;
    };
    return s;
}

// DVB-T2 SISO: frames of P1 (S1 = 000 T2 SISO), NP2 P2 symbols, data symbols
// with scattered / continual / edge pilots, the frame closing symbol where the
// mode uses one; data cells 16QAM
Source dvbt2_source(int fftK, int gi, int pp, double bw, bool ext, int lf) {
    Source s;
    s.fs = dvbt2_std::sample_rate_hz(bw);
    const int fi = dvbt2_std::fft_index(fftK);
    const int N = dvbt2_std::FFT_N[fi], ncp = dvbt2_std::GI_LEN_T[fi][gi];
    const int kmax = dvbt2_std::kmax(fftK, ext), kc = dvbt2_std::centre_carrier(fftK, ext);
    const int np2 = dvbt2_std::NP2[fi];
    const bool fc = dvbt2_std::frame_closing_used(fftK, gi, pp);
    const auto p1 = dvbt2_std::p1_time_domain(0, 0);
    if (ncp == 0 || !dvbt2_std::siso_allowed(fftK, gi, pp)) {
        fprintf(stderr, "DVB-T2: GI index %d / PP%d not allowed with %dk\n", gi, pp, fftK);
        exit(2);
    }
    s.next = [=](std::vector<cd>* out) {
        out->insert(out->end(), p1.begin(), p1.end());
        for (int l = 0; l < lf; l++) {
            std::vector<std::pair<int, cd>> cells;
            for (int k = 0; k <= kmax; k++) {
                cd v;
                if (l < np2) v = dvbt2_std::p2_pilot_value(k, l, fftK, ext);
                else if (fc && l == lf - 1) v = dvbt2_std::fc_symbol_pilot_value(k, l, pp, fftK, ext);
                else v = dvbt2_std::pilot_value(k, l, pp, fftK, ext);
                if (std::norm(v) < 1e-12) v = qam16();
                cells.push_back({k - kc, v});
            }
            std::vector<cd> t;
            ofdm_symbol(cells, N, ncp, &t);
            out->insert(out->end(), t.begin(), t.end());
        }
    };
    return s;
}

// DTMB at 1 sample per chip (no pulse shaping: keep the VFO inside the
// channel's flat band): PN header (phase varying over the superframe or
// constant) + 3780 single-carrier 16QAM body symbols per frame
Source dtmb_source(int header, bool vary) {
    Source s;
    s.fs = dtmb_std::kSymbolRateHz;
    const auto& hm = dtmb_std::header_mode(header);
    auto l = std::make_shared<int>(0);
    s.next = [=, &hm](std::vector<cd>* out) {
        const int phase = vary ? *l % hm.frames_per_superframe : 0;
        for (int c : dtmb_std::pn_header(header, phase)) out->push_back(dtmb_std::header_symbol(hm, c));
        for (int i = 0; i < dtmb_std::kFrameBodySymbols; i++) out->push_back(qam16());
        ++*l;
    };
    return s;
}

// ATSC 3.0 SISO, one subframe per frame: a 2 ms stand-in for the bootstrap
// (noise at the data power - another numerology, it only has to break the
// symbol grid), npre preamble symbols (pilots k mod Dx_pre = 0 + common CPs;
// the first at Cred 4), nsym data symbols with scattered / continual / edge
// pilots and `excess` extra guard samples each; data cells 16QAM
Source atsc3_source(int fftK, int gi, int sp, int boost, int cred, int bsr, int nsym, int npre, int excess) {
    Source s;
    s.fs = atsc3_std::bsr_hz(bsr);
    const auto f = fftK == 8 ? atsc3_std::Fft::K8 : fftK == 16 ? atsc3_std::Fft::K16 : atsc3_std::Fft::K32;
    const int fi = atsc3_std::fidx(f), N = atsc3_std::FFT_LEN[fi], ncp = atsc3_std::GI_SAMPLES[gi];
    if (!atsc3_std::sp_allowed(f, gi, sp)) {
        fprintf(stderr, "ATSC 3.0: %s not allowed with %dk GI%d\n", atsc3_std::SP_PATTERNS[sp].name, fftK, gi);
        exit(2);
    }
    const atsc3_std::PreambleCfg* pc = nullptr;
    for (const auto& c : atsc3_std::PREAMBLE_CFG)
        if (c.fft == f && c.gi_samples == ncp && c.dx <= atsc3_std::SP_PATTERNS[sp].dx && atsc3_std::SP_PATTERNS[sp].dx % c.dx == 0) {
            pc = &c;
            break;
        }
    const long boot = std::lround(0.002 * s.fs);
    s.next = [=](std::vector<cd>* out) {
        for (long i = 0; i < boot; i++) out->push_back(cd(gauss(rng), gauss(rng)) * std::sqrt(0.5 * 0.8));
        auto symbol = [&](int cr, int extra, const std::function<cd(int)>& pilot) {
            const int noc = atsc3_std::noc(f, cr), kc = (noc - 1) / 2;
            std::vector<std::pair<int, cd>> cells;
            for (int k = 0; k < noc; k++) {
                cd v = pilot(k);
                if (std::norm(v) < 1e-12) v = qam16();
                cells.push_back({k - kc, v});
            }
            std::vector<cd> t;
            ofdm_symbol(cells, N, ncp + extra, &t);
            out->insert(out->end(), t.begin(), t.end());
        };
        for (int p = 0; p < npre && pc; p++) {
            const int cr = p == 0 ? atsc3_std::FIRST_PREAMBLE_CRED : cred;
            symbol(cr, 0, [&](int k) { return atsc3_std::preamble_pilot_value(*pc, cr, k); });
        }
        for (int l = 0; l < nsym; l++) symbol(cred, excess, [&](int k) { return atsc3_std::pilot_value(f, cred, sp, boost, k, l); });
    };
    return s;
}

// ATSC 8-VSB at the symbol rate: complex baseband = symbols (-j)^n (+ pilot),
// root-raised-cosine (alpha 0.1152) at 2 samples per "Nyquist symbol"
Source atsc_source() {
    Source s;
    s.fs = atsc_std::kSymbolRateHz;
    // SRRC taps (span 64 symbols at sps 2)
    auto taps = std::make_shared<std::vector<double>>();
    const double a = 0.1152;
    for (int i = -64; i <= 64; i++) {
        const double t = i / 2.0;   // in units of the SRRC symbol (2 samples)
        double h;
        if (std::fabs(t) < 1e-9) h = 1 - a + 4 * a / M_PI;
        else if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-9)
            h = a / std::sqrt(2) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        else h = (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - std::pow(4 * a * t, 2)));
        taps->push_back(h);
    }
    auto hist = std::make_shared<std::vector<cd>>(taps->size(), cd(0, 0));
    auto n = std::make_shared<int64_t>(0);
    auto field = std::make_shared<int>(0);
    s.next = [=](std::vector<cd>* out) {
        // one field: field sync + 312 data segments
        std::vector<double> sym;
        const auto fsync = atsc_std::field_sync_symbols(*field % 2 == 1);
        for (size_t i = 0; i < fsync.size(); i++) sym.push_back(std::isnan(fsync[i]) ? (bit(rng) ? 5.0 : -5.0) : fsync[i]);
        const int lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        for (int seg = 0; seg < 312; seg++) {
            sym.push_back(5);
            sym.push_back(-5);
            sym.push_back(-5);
            sym.push_back(5);
            for (int i = 4; i < 832; i++) sym.push_back(lv[bit(rng) * 4 + bit(rng) * 2 + bit(rng)]);
        }
        ++*field;
        for (double v : sym) {
            const cd b = (v + 1.25) * std::pow(cd(0, -1), static_cast<int>(*n % 4));
            ++*n;
            hist->erase(hist->begin());
            hist->push_back(b);
            cd y = 0;
            for (size_t i = 0; i < taps->size(); i++) y += (*hist)[i] * (*taps)[taps->size() - 1 - i];
            out->push_back(y / 4.0);
        }
    };
    return s;
}

struct Tx { double az, delay_us, db, dop; };

}  // namespace

int main(int argc, char** argv) {
    std::string std_ = "dvbt", out = "/tmp/sfn.cf32", mode = "8k";
    int t2gi = 0, pp = 7, ext = 1, lf = 60;
    int pn = 420, vary = 1;                  // DTMB: frame header, phase varying
    int a3gi = 5, a3sp = 9, boost = 4, cred = 0, nsym = 80, npre = 2, excess = 0;   // ATSC 3.0   // DVB-T2: GI index (0 = 1/128 .. 6 = 1/4), pattern, extended carriers, frame length
    double bwd = 0;                          // channel bandwidth as a decimal (DVB-T2 1.7 MHz)
    int gi = 4, bw = 8, M = 5, cell = 0x1234, ews = 0;
    double secs = 3, rate = 2.4e6, offset = 0, rf = 600e6, radius = -1, snr = 20;
    std::vector<Tx> txs;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--std") std_ = val();
        else if (a == "--mode") mode = val();
        else if (a == "--gi") gi = std::stoi(val());
        else if (a == "--bw") {
            bwd = std::stod(val());
            bw = static_cast<int>(std::lround(bwd));
        } else if (a == "--t2gi") t2gi = std::stoi(val());
        else if (a == "--pp") pp = std::stoi(val());
        else if (a == "--ext") ext = std::stoi(val());
        else if (a == "--lf") lf = std::stoi(val());
        else if (a == "--pn") pn = std::stoi(val());
        else if (a == "--a3gi") a3gi = std::stoi(val());
        else if (a == "--sp") a3sp = std::stoi(val());
        else if (a == "--boost") boost = std::stoi(val());
        else if (a == "--cred") cred = std::stoi(val());
        else if (a == "--nsym") nsym = std::stoi(val());
        else if (a == "--npre") npre = std::stoi(val());
        else if (a == "--excess") excess = std::stoi(val());
        else if (a == "--vary") vary = std::stoi(val());
        else if (a == "--secs") secs = std::stod(val());
        else if (a == "--rate") rate = std::stod(val());
        else if (a == "--offset") offset = std::stod(val());
        else if (a == "--rf") rf = std::stod(val());
        else if (a == "--radius") radius = std::stod(val());
        else if (a == "--snr") snr = std::stod(val());
        else if (a == "--out") out = val();
        else if (a == "--cell") cell = std::stoi(val(), nullptr, 0);
        else if (a == "--ews") ews = std::stoi(val());
        else if (a == "--tx") {
            Tx t{0, 0, 0, 0};
            if (std::sscanf(val().c_str(), "%lf,%lf,%lf,%lf", &t.az, &t.delay_us, &t.db, &t.dop) < 3) return 2;
            txs.push_back(t);
        } else {
            fprintf(stderr, "unknown %s\n", a.c_str());
            return 2;
        }
    }
    if (txs.empty()) txs = {{30, 0, 0, 0}, {150, 37, -4, 0}, {260, 112, -10, 0}};
    Source src;
    if (std_ == "dvbt") src = dvbt_source(mode == "8k", gi, bw, cell);
    else if (std_ == "dtmb") src = dtmb_source(pn, vary != 0);
    else if (std_ == "atsc3")
        src = atsc3_source(std::stoi(mode), a3gi, a3sp, boost, cred, bw == 6 ? 2 : bw == 7 ? 5 : 8, nsym, npre, excess);
    else if (std_ == "dvbt2") src = dvbt2_source(std::stoi(mode), t2gi, pp, bwd > 0 ? bwd : 8.0, ext != 0, lf);
    else if (std_ == "isdbt") src = isdbt_source(mode == "1" ? 1 : mode == "2" ? 2 : 3, gi, ews != 0);
    else if (std_ == "atsc") src = atsc_source();
    else { fprintf(stderr, "unknown standard\n"); return 2; }
    const double lambda = 3e8 / rf;
    if (radius <= 0) radius = 0.425 * lambda;
    // steering x gain per transmitter / antenna
    std::vector<std::vector<cd>> A;
    std::vector<int64_t> dl;
    double ptot = 0;
    for (const auto& t : txs) {
        std::vector<cd> a(static_cast<size_t>(M));
        const double th = t.az * M_PI / 180;
        const cd g = std::polar(std::pow(10.0, t.db / 20), 2 * M_PI * (std::uniform_real_distribution<double>(0, 1)(rng)));
        for (int k = 0; k < M; k++) {
            const double ea = -2 * M_PI * k / M;
            a[static_cast<size_t>(k)] = g * std::polar(1.0, 2 * M_PI * radius / lambda * (std::cos(ea) * std::cos(th) + std::sin(ea) * std::sin(th)));
        }
        A.push_back(a);
        dl.push_back(std::lround(t.delay_us * 1e-6 * src.fs));
        ptot += std::pow(10.0, t.db / 10);
    }
    int64_t dmax = 0;
    for (int64_t d : dl) dmax = std::max(dmax, d);
    // the source, generated ahead as needed
    std::vector<cd> s;
    int64_t sbase = 0;   // absolute index of s[0]
    // the source mixed by the VFO's offset as it is generated (the channel
    // centre sits at -offset in the VFO's baseband): a transmitter's delay
    // then only adds a constant phase, folded into its steering below
    double mix_ph = 0;
    const double mix_w = -2 * M_PI * offset / src.fs;
    auto need = [&](int64_t i) {
        while (sbase + static_cast<int64_t>(s.size()) <= i) {
            const size_t a = s.size();
            src.next(&s);
            for (size_t j = a; j < s.size(); j++) {
                s[j] *= std::polar(1.0, mix_ph);
                mix_ph = std::remainder(mix_ph + mix_w, 2 * M_PI);
            }
        }
    };
    for (size_t k = 0; k < txs.size(); k++)
        for (auto& a : A[k]) a *= std::polar(1.0, mix_w * static_cast<double>(dl[k]));
    auto S = [&](int64_t i) -> cd { return i < 0 ? cd(0, 0) : s[static_cast<size_t>(i - sbase)]; };
    // signal power of the source (for the SNR)
    need(200000);
    double ps = 0;
    for (size_t i = 0; i < 200000; i++) ps += std::norm(s[i]);
    ps /= 200000;
    // resampling filter (windowed sinc, cut-off 0.45 x the output rate)
    const int TAPS = 48;
    const double step = src.fs / rate, fc = 0.45 * std::min(1.0, rate / src.fs);
    const double nsd = std::sqrt(ps * ptot / std::pow(10.0, snr / 10) / 2);
    // polyphase table: PH + 1 fractional positions
    const int PH = 1024;
    std::vector<double> poly(static_cast<size_t>(PH + 1) * TAPS);
    for (int ph = 0; ph <= PH; ph++)
        for (int j = 0; j < TAPS; j++) {
            const double x = (-TAPS / 2 + 1 + j) - static_cast<double>(ph) / PH;
            const double sinc = std::fabs(x) < 1e-12 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            poly[static_cast<size_t>(ph) * TAPS + j] =
                sinc * (0.42 + 0.5 * std::cos(M_PI * x / (TAPS / 2.0)) + 0.08 * std::cos(2 * M_PI * x / (TAPS / 2.0)));
        }
    std::ofstream f(out, std::ios::binary);
    const int64_t nout = static_cast<int64_t>(secs * rate);
    std::vector<cf> row(static_cast<size_t>(M));
    std::vector<cd> u(static_cast<size_t>(M));
    for (int64_t n = 0; n < nout; n++) {
        const double t = n * step;
        const int64_t i0 = static_cast<int64_t>(std::floor(t));
        const double fr = t - i0;
        need(i0 + TAPS);
        std::fill(row.begin(), row.end(), cf(0, 0));
        std::vector<cd> acc(static_cast<size_t>(M), cd(0, 0));
        const double* h = poly.data() + static_cast<size_t>(std::lround((t - i0) * PH)) * TAPS;
        for (size_t k = 0; k < txs.size(); k++) {
            cd v = 0;
            for (int j = 0; j < TAPS; j++) {
                const int64_t idx = i0 - TAPS / 2 + 1 + j - dl[k];
                v += S(idx) * h[j];
            }
            if (txs[k].dop != 0) v *= std::polar(1.0, 2 * M_PI * txs[k].dop * n / rate);
            for (int m = 0; m < M; m++) acc[static_cast<size_t>(m)] += A[k][static_cast<size_t>(m)] * v;
        }
        (void)fr;
        for (int m = 0; m < M; m++)
            row[static_cast<size_t>(m)] = cf(static_cast<float>(acc[static_cast<size_t>(m)].real() + nsd * gauss(rng)),
                                             static_cast<float>(acc[static_cast<size_t>(m)].imag() + nsd * gauss(rng)));
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(cf)));
        // drop source history no longer needed
        if (i0 - TAPS - dmax - sbase > 1000000) {
            const int64_t drop = i0 - TAPS - dmax - sbase - 1000;
            s.erase(s.begin(), s.begin() + drop);
            sbase += drop;
        }
    }
    std::ofstream js(out.substr(0, out.rfind('.')) + ".json");
    js << "{\"rate\": " << static_cast<long long>(rate) << ", \"channels\": " << M << ", \"rf_hz\": " << static_cast<long long>(rf)
       << ", \"radius_m\": " << radius << "}\n";
    printf("%s: %.2f s, %d antennas at %.0f Hz, RF %.3f MHz, VFO %+.0f Hz from the channel centre, UCA r %.4f m\n", out.c_str(),
           nout / rate, M, rate, rf / 1e6, offset, radius);
    for (size_t k = 0; k < txs.size(); k++)
        printf("  TX az %6.1f delay %8.3f us (%lld samples at %.0f Hz) %5.1f dB\n", txs[k].az, dl[k] / src.fs * 1e6, static_cast<long long>(dl[k]), src.fs, txs[k].db);
    return 0;
}
