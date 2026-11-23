// Signal classifier: the feature vector of one view (generic signal
// processing only - spectrum shape, envelope, instantaneous frequency,
// carrier lines, periodicity, time-frequency). The layout is the table
// GROUPS below; layout_json() prints it with each group's offset and
// meaning, and its hash goes into every model file.
//
// Every value is finite and roughly O(1) for any input (silence, clipping,
// one sample): each group is normalised and the vector is clamped at the end.
//
// Samples: the whole view when it is at most FEAT_MAX samples; a longer view
// (480 k / 2.4 M at 0.5 s) is analysed in NCHUNK_WIDE chunks of CHUNK samples -
// half the most energetic chunks (bursts), half evenly spread - while the
// slow statistics (duty, bursts, the low-frequency spectra, long lags) use
// box sums of the whole view.

#include "sc_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <sstream>

namespace sigclass {

namespace {

struct Group {
    const char* name;
    int len;
    const char* desc;
};

// THE feature layout (offsets are cumulative). Changing a group's content or
// size changes the hash; bump FEATURE_LAYOUT_VERSION when a meaning changes.
constexpr Group GROUPS[] = {
    {"meta", 16, "detector + view scalars: bw/fs_v, log10 bw, snr, peak, duty, bursty, fills_band, "
                 "view on-fraction, on-SNR, flatness, asymmetry, edge steepness, view 98% bw/fs_v, "
                 "view centroid, bursts/s, median burst length"},
    {"psd", 128, "Welch PSD of the view (1024-pt Hann, pooled to 128 bins over -fs_v/2..fs_v/2), "
                 "dB re its peak, clipped at -50: v = dB/25 + 1"},
    {"env_hist", 32, "histogram of |x|/rms over 'on' samples, 0..3.2 in 0.1 steps: sqrt(density)/2"},
    {"env_mom", 12, "crest, M4/M2^2, C42', C20 line, C40 line, envelope std, on-duty (chunks), "
                    "on-runs/s, median on-run, median off-run, full-view crest, amplitude skew"},
    {"env_spec", 128, "envelope (|x|^2) spectrum 0..fs_v/2 (256-pt Welch), dB re a Gaussian-noise "
                      "envelope, /20"},
    {"env_spec_lo", 32, "envelope spectrum 2..500 Hz (box-decimated whole view), 32 log bins, max-pooled, "
                        "dB re Gaussian, /20"},
    {"if_hist", 64, "instantaneous frequency (3-term discriminator, 'on' samples, median removed) "
                    "histogram over +-0.3 fs_v: sqrt(density re uniform)/2"},
    {"if_mom", 8, "IF std/fs_v, IF kurtosis, fraction near 0, mean |dIF|/fs_v, histogram modes, "
                  "IF skew, IF MAD/fs_v, IF range"},
    {"if_spec", 128, "spectrum of the gated IF 0..fs_v/2 (256-pt Welch), dB re its median, /20"},
    {"if_spec_lo", 32, "IF spectrum 2..500 Hz (box-decimated whole view), 32 log bins, dB re median, /20"},
    {"sq", 40, "spectrum of x^2: 32 bins max-pooled over -fs_v/2..fs_v/2 (dB re median /20), then "
               "peak/median, |f_peak|/fs_v, 2nd peak/median, peak distance, line fraction, "
               "spectral flatness, and 2 reserved (0)"},
    {"quad", 40, "the same for x^4"},
    {"acf_c", 40, "|autocorrelation| of the complex view on a log lag grid 2 us .. 250 ms (40 bins, "
                  "max-pooled; lags shorter than one sample = 1)"},
    {"acf_e", 40, "the same for the envelope (mean removed)"},
    {"acf_f", 40, "the same for the gated IF (mean removed)"},
    {"lagprod", 12, "x[n] conj(x[n-d]) spectrum for d = 4, 16, 64: peak/median, |f_peak|/fs_v, "
                    "line fraction, 2nd peak/median (chirps, symbol structure)"},
    {"tf", 12, "64-pt spectrogram of the view: centroid std, mean |step|, trend, spread, "
               "instantaneous/total occupancy, frame correlation, on-frames, upward steps, "
               "power std, max step, centroid kurtosis, step/std"},
    {"sym", 134, "symbol-synchronous: the symbol rate rs = the strongest cyclic line (fs_v/200 .. fs_v/2.2, "
                 "re its neighbourhood) of |dIF| or of the envelope; per block of <= 2048 symbols the "
                 "timing phase (of 8) whose samples cluster most; 14 scalars (line dB/20, source, |dIF| "
                 "line, envelope line, rs/fs_v*4, (log10 rs - 3)/2, rs/bw, line at 2 rs, at rs/2, IF "
                 "concentration, phase-step concentration, q90 |IF|/fs_v*4, q50/q90 |IF|, symbols on), "
                 "the IF at the symbol centres (64 bins over +-0.25 fs_v), |IF| / its q90 (24 bins "
                 "0..1.5), the phase step over one symbol (32 bins -pi..pi): sqrt(density re uniform)/2"},
    {"frame_c", 48, "|autocorrelation| of the complex view on a LINEAR lag grid (frame periods): views "
                    "0-1 48 bins of 5 ms centred at 5..240 ms (box sums), views 2-3 48 bins of 22 us "
                    "from 0 to 1.056 ms (samples); max-pooled, minus the grid's median, x8"},
    {"frame_e", 48, "the same for the envelope (mean removed)"},
    {"frame_f", 48, "the same for the gated IF (mean removed)"},
    {"symseq_meta", 4, "the symbol sequence below: symbols in it / SEQ_S, its IF scale (q90 |IF|) / fs_v * 4, "
                       "share with a phase step, 0"},
    {"symseq", 2 * 1024, "the symbol sequence (layout v3; a learned front end reads it): SEQ_S = 1024 symbols at "
                         "the sym group's rate and timing, the on-air ones in time order from the first, zero-padded; "
                         "channel 0 = the IF at the symbol centre / its q90, clipped to +-1.5, / 1.5; channel 1 = the "
                         "phase step over the symbol / pi (0 where the previous symbol was off); channel-major"},
};
constexpr int NGROUPS = int(sizeof(GROUPS) / sizeof(GROUPS[0]));

constexpr int dim_of() {
    int d = 0;
    for (const Group& g : GROUPS) d += g.len;
    return d;
}
constexpr int DIM = dim_of();

// analysis parameters (part of the layout: in layout_json and the hash)
constexpr int FEAT_MAX = 65536;
constexpr int CHUNK = 4096;
// a view longer than FEAT_MAX (views 2-3 at 0.5 s) is analysed in NCHUNK_WIDE chunks
constexpr int NCHUNK_WIDE = 8;
constexpr int ON_BOX = 8;
constexpr int PSD_SEG = 1024, PSD_K = 48;
constexpr int ENV_SEG = 256, ENV_K = 64;
constexpr int SQ_SEG = 512, SQ_K = 32;
// level A segment: 2048 samples, 4096 at the full rate (OFDM symbol lags up to ~1 ms)
constexpr int ACF_SA = 2048, ACF_SA_FULL = 4096, ACF_KA = 6, ACF_NB = 2048;
constexpr int LP_SEG = 512, LP_K = 32;
constexpr int TF_N = 64, TF_FRAMES = 128;
constexpr double LO_RATE = 1200.0, LO_F0 = 2.0, LO_F1 = 500.0;
constexpr double LAG0 = 2e-6, LAG1 = 0.25;
constexpr int NLAG = 40;
// symbol-synchronous group: cyclic-line FFT size (at most), blocks of symbols, timing candidates
constexpr int SYM_NFFT = 8192, SYM_SEGS = 4, SYM_BLOCK = 2048, SYM_PHASES = 8, SYM_MAXSYM = 4096;
constexpr int SEQ_S = 1024;     // symbols in the symseq group (2 channels)
static_assert(GROUPS[NGROUPS - 1].len == 2 * SEQ_S, "symseq is the last group, 2 x SEQ_S long");
constexpr double SYM_R0 = 1.0 / 200.0, SYM_R1 = 1.0 / 2.2;   // line search range, fractions of fs_v
// linear frame-lag grids: narrow views (0-1) and wide views (2-3)
constexpr int FRAME_N = 48;
constexpr double FRAME_DT_NARROW = 5e-3, FRAME_DT_WIDE = 22e-6;
constexpr double PI = 3.14159265358979323846;

int group_offset(const char* name) {
    int off = 0;
    for (const Group& g : GROUPS) {
        if (std::string(g.name) == name) return off;
        off += g.len;
    }
    return -1;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string layout_body() {
    std::ostringstream o;
    o << "\"version\":" << FEATURE_LAYOUT_VERSION << ",\"dim\":" << DIM << ",\"views\":[";
    for (int v = 0; v < NUM_VIEWS; v++)
        o << (v ? "," : "") << "{\"view\":" << v << ",\"decim\":" << VIEW_DECIM[v]
          << ",\"rate_hz_at_2m4\":" << int(NOMINAL_FS_HZ / VIEW_DECIM[v]) << "}";
    o << "],\"params\":{\"feat_max\":" << FEAT_MAX << ",\"nchunk\":" << NCHUNK_WIDE << ",\"chunk\":" << CHUNK << ",\"on_box\":" << ON_BOX
      << ",\"psd_seg\":" << PSD_SEG << ",\"env_seg\":" << ENV_SEG << ",\"sq_seg\":" << SQ_SEG
      << ",\"acf_seg\":" << ACF_SA << ",\"acf_seg_full\":" << ACF_SA_FULL << ",\"acf_k\":" << ACF_KA << ",\"acf_nb\":" << ACF_NB << ",\"lp_seg\":" << LP_SEG
      << ",\"tf_n\":" << TF_N << ",\"lo_rate\":" << LO_RATE << ",\"lag0\":" << LAG0 << ",\"lag1\":" << LAG1
      << ",\"nlag\":" << NLAG << ",\"sym_nfft\":" << SYM_NFFT << ",\"sym_segs\":" << SYM_SEGS << ",\"sym_block\":" << SYM_BLOCK
      << ",\"sym_phases\":" << SYM_PHASES << ",\"sym_maxsym\":" << SYM_MAXSYM << ",\"seq_s\":" << SEQ_S << ",\"sym_r0\":" << SYM_R0 << ",\"sym_r1\":" << SYM_R1 << ",\"frame_n\":" << FRAME_N
      << ",\"frame_dt_narrow\":" << FRAME_DT_NARROW << ",\"frame_dt_wide\":" << FRAME_DT_WIDE
      << ",\"long_n\":" << detail::LONG_N << ",\"view_filter\":\"kaiser60 0.4-0.5\"}";
    o << ",\"groups\":[";
    int off = 0;
    for (int i = 0; i < NGROUPS; i++) {
        o << (i ? "," : "") << "{\"name\":\"" << GROUPS[i].name << "\",\"offset\":" << off << ",\"length\":"
          << GROUPS[i].len << ",\"desc\":\"" << GROUPS[i].desc << "\"}";
        off += GROUPS[i].len;
    }
    o << "]";
    return o.str();
}

}  // namespace

int feature_dim() { return DIM; }

uint64_t layout_hash() {
    static const uint64_t h = fnv1a(layout_body());
    return h;
}

std::string layout_json() {
    char hx[32];
    std::snprintf(hx, sizeof hx, "%016llx", (unsigned long long)layout_hash());
    return "{\"hash\":\"" + std::string(hx) + "\"," + layout_body() + "}";
}

namespace detail {

namespace {

struct Span {
    int start, len;   // in the compact arrays
};

void hann(int n, std::vector<float>& w) {
    w.resize(size_t(n));
    for (int i = 0; i < n; i++) w[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * PI * (i + 0.5) / n));
}

// median (of at most 8192 evenly strided values of a long vector); w: scratch
float median_of(const std::vector<float>& v, std::vector<float>& w) {
    if (v.empty()) return 0.0f;
    const size_t step = (v.size() + 8191) / 8192;
    w.clear();
    for (size_t i = 0; i < v.size(); i += step) w.push_back(v[i]);
    const size_t k = w.size() / 2;
    std::nth_element(w.begin(), w.begin() + long(k), w.end());
    return w[k];
}

}  // namespace

// profiling of the feature groups (SIGCLASS_PROF builds; otherwise empty)
enum { P_BOX, P_SET, P_PSD, P_ENV, P_ENVSPEC, P_LO, P_IF, P_SQ, P_ACF, P_SYMFIL, P_SYMRATE, P_SYMTIME, P_LAG, P_TF, P_N };
#ifdef SIGCLASS_PROF
struct prof_scope {
    prof::Scope s;
    explicit prof_scope(int g) : s(ids()[g]) {}
    static const int* ids() {
        static const int v[P_N] = {prof::reg("feat.boxsums"), prof::reg("feat.analysed_set"), prof::reg("feat.psd"),
                                   prof::reg("feat.env"), prof::reg("feat.env_spec"), prof::reg("feat.lo_spectra"),
                                   prof::reg("feat.if"), prof::reg("feat.sq_quad"), prof::reg("feat.acf_frame"),
                                   prof::reg("feat.sym_filter"), prof::reg("feat.sym_rate"), prof::reg("feat.sym_timing_seq"),
                                   prof::reg("feat.lagprod"), prof::reg("feat.tf")};
        return v;
    }
};
#define FEAT_PHASE_START(g) std::optional<prof_scope> ps_(std::in_place, g)
#define FEAT_PHASE(g) (ps_.reset(), ps_.emplace(g))
#define FEAT_PHASE_END() ps_.reset()
#else
#define FEAT_PHASE_START(g) static_assert(true, "")
#define FEAT_PHASE(g) ((void)0)
#define FEAT_PHASE_END() ((void)0)
#endif

struct Features::Impl {
    std::map<std::pair<int, bool>, Fft> plans;
    std::map<int, std::vector<float>> wins;
    // full-view box sums (ON_BOX samples)
    std::vector<float> eb8, es;
    std::vector<cf> zb8, yb8;
    std::vector<uint8_t> on8;
    // compact analysed set
    avec<cf> yc;
    std::vector<float> ec, fc, fg;
    std::vector<uint8_t> oc;
    std::vector<Span> spans;
    // work
    avec<cf> ca, cb;
    avec<float> ra, rb;
    // reused per call (no allocation once warm)
    avec<cf> pw, A, B, P, Q;
    avec<float> a, b, p, q;
    std::vector<double> Re, Rf;
    std::vector<cf> Rcc;
    std::vector<float> spec, eB, fB;
    std::vector<float> S, S2, tmpv, scratch, mscratch, seqv, seqp, ffil, efil;
    avec<cf> yfil;
    std::vector<std::pair<int, int>> segs;

    Fft& plan(int n, bool real) {
        auto it = plans.find({n, real});
        if (it == plans.end()) it = plans.emplace(std::make_pair(n, real), Fft(n, real)).first;
        return it->second;
    }
    const std::vector<float>& win(int n) {
        auto it = wins.find(n);
        if (it == wins.end()) {
            std::vector<float> w;
            hann(n, w);
            it = wins.emplace(n, std::move(w)).first;
        }
        return it->second;
    }
    void pick_segments(int seg, int K);
    void welch_c(const cf* x, int seg, int K, std::vector<float>& out);   // centred, seg bins
    void welch_r(const float* x, int seg, int K, std::vector<float>& out); // seg/2 + 1 bins
    void compute(const cf* y, size_t n, const ViewInfo& vi, float* out);
};

Features::Features() : impl_(std::make_shared<Impl>()) {}

void Features::compute(const cf* y, size_t n, const ViewInfo& vi, float* out) { impl_->compute(y, n, vi, out); }

// segments of seg samples inside the spans (a short span = one shorter segment),
// hop seg / 2, at most K of them evenly picked
void Features::Impl::pick_segments(int seg, int K) {
    std::vector<std::pair<int, int>> all;
    for (const Span& s : spans) {
        if (s.len <= 0) continue;
        if (s.len < seg) {
            all.push_back({s.start, s.len});
            continue;
        }
        const int hop = std::max(1, seg / 2);
        for (int p = 0; p + seg <= s.len; p += hop) all.push_back({s.start + p, seg});
    }
    segs.clear();
    if (all.empty()) return;
    if (int(all.size()) <= K) {
        segs = all;
        return;
    }
    for (int q = 0; q < K; q++) segs.push_back(all[size_t(std::lround(double(q) * (all.size() - 1) / (K - 1)))]);
}

void Features::Impl::welch_c(const cf* x, int seg, int K, std::vector<float>& out) {
    pick_segments(seg, K);
    out.assign(size_t(seg), 0.0f);
    if (segs.empty()) return;
    Fft& f = plan(seg, false);
    ca.resize(size_t(seg));
    cb.resize(size_t(seg));
    std::vector<float> wl;
    double wsum = 0.0;
    for (const auto& sg : segs) {
        const std::vector<float>* w = &win(seg);
        if (sg.second != seg) {
            hann(sg.second, wl);
            w = &wl;
        }
        std::fill(ca.begin(), ca.end(), cf(0.0f, 0.0f));
        double e = 0.0;
        for (int i = 0; i < sg.second; i++) {
            ca[size_t(i)] = x[sg.first + i] * (*w)[size_t(i)];
            e += double((*w)[size_t(i)]) * (*w)[size_t(i)];
        }
        wsum += e;
        f.fwd(ca.data(), cb.data());
        for (int k = 0; k < seg; k++) out[size_t((k + seg / 2) % seg)] += std::norm(cb[size_t(k)]);
    }
    const float inv = wsum > 0.0 ? float(1.0 / wsum) : 0.0f;
    for (float& v : out) v *= inv;
}

void Features::Impl::welch_r(const float* x, int seg, int K, std::vector<float>& out) {
    pick_segments(seg, K);
    out.assign(size_t(seg / 2 + 1), 0.0f);
    if (segs.empty()) return;
    Fft& f = plan(seg, true);
    ra.resize(size_t(seg));
    rb.resize(size_t(seg));
    std::vector<float> wl;
    double wsum = 0.0;
    for (const auto& sg : segs) {
        const std::vector<float>* w = &win(seg);
        if (sg.second != seg) {
            hann(sg.second, wl);
            w = &wl;
        }
        std::fill(ra.begin(), ra.end(), 0.0f);
        double e = 0.0;
        for (int i = 0; i < sg.second; i++) {
            ra[size_t(i)] = x[sg.first + i] * (*w)[size_t(i)];
            e += double((*w)[size_t(i)]) * (*w)[size_t(i)];
        }
        wsum += e;
        f.fwd_real(ra.data(), rb.data());
        out[0] += rb[0] * rb[0];
        out[size_t(seg / 2)] += rb[1] * rb[1];
        for (int k = 1; k < seg / 2; k++) out[size_t(k)] += rb[size_t(2 * k)] * rb[size_t(2 * k)] + rb[size_t(2 * k + 1)] * rb[size_t(2 * k + 1)];
    }
    const float inv = wsum > 0.0 ? float(1.0 / wsum) : 0.0f;
    for (float& v : out) v *= inv;
}

namespace {

// largest Welch size <= want that fits the data (power of two >= minimum)
int seg_for(int want, int n, int minimum) {
    int s = want;
    while (s > minimum && s > n) s /= 2;
    return s;
}

// 32 log-spaced bins f0..f1 max-pooled from a spectrum with bin spacing df (bins 0..nb-1)
void log_pool(const std::vector<float>& S, double df, double f0, double f1, int nbins, std::vector<float>& out) {
    out.assign(size_t(nbins), 0.0f);
    const double r = std::pow(f1 / f0, 1.0 / nbins);
    for (int i = 0; i < nbins; i++) {
        const double lo = f0 * std::pow(r, i), hi = lo * r;
        int k0 = int(std::ceil(lo / df)), k1 = int(std::floor(hi / df));
        if (k1 < k0) k0 = k1 = int(std::lround(std::sqrt(lo * hi) / df));
        k0 = std::max(k0, 1);
        k1 = std::min(k1, int(S.size()) - 1);
        float m = 0.0f;
        for (int k = k0; k <= k1; k++) m = std::max(m, S[size_t(k)]);
        out[size_t(i)] = m;
    }
}

struct ScalarStats {
    double n = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    void add(double x) { n++; s1 += x; s2 += x * x; s3 += x * x * x; s4 += x * x * x * x; }
    double mean() const { return n > 0 ? s1 / n : 0.0; }
    double var() const { return n > 0 ? std::max(0.0, s2 / n - mean() * mean()) : 0.0; }
    double skew() const {
        const double m = mean(), v = var();
        if (!(v > 1e-30)) return 0.0;
        return (s3 / n - 3 * m * s2 / n + 2 * m * m * m) / std::pow(v, 1.5);
    }
    double kurt() const {
        const double m = mean(), v = var();
        if (!(v > 1e-30)) return 0.0;
        const double m4 = s4 / n - 4 * m * s3 / n + 6 * m * m * s2 / n - 3 * m * m * m * m;
        return m4 / (v * v);
    }
};

}  // namespace

void Features::Impl::compute(const cf* y, size_t n_in, const ViewInfo& vi, float* out) {
    std::fill(out, out + DIM, 0.0f);
    const double fs_v = vi.fs_v > 0.0 ? vi.fs_v : 1.0;
    const Detection& d = vi.det;
    float* meta = out + group_offset("meta");
    meta[0] = clampf(float(d.bw_hz / fs_v), 0.0f, 2.0f);
    meta[1] = clampf(float((std::log10(std::max(d.bw_hz, 10.0)) - 4.0) / 2.0), -2.0f, 2.0f);
    meta[2] = clampf(d.snr_db / 30.0f, -1.0f, 4.0f);
    meta[3] = clampf(d.peak_db / 30.0f, -1.0f, 4.0f);
    meta[4] = clampf(d.duty, 0.0f, 1.0f);
    meta[5] = d.bursty ? 1.0f : 0.0f;
    meta[6] = d.fills_band ? 1.0f : 0.0f;
    // a view this short says nothing more
    const int n = int(std::min<size_t>(n_in, size_t(1) << 30));
    if (n < 64 || !y) return;

    // ---- pass over the whole view: box sums of ON_BOX samples
    const int nb8 = n / ON_BOX;
    FEAT_PHASE_START(P_BOX);
    eb8.assign(size_t(nb8), 0.0f);
    zb8.assign(size_t(nb8), cf(0.0f, 0.0f));
    yb8.assign(size_t(nb8), cf(0.0f, 0.0f));
    {
        // on interleaved floats, one box of ON_BOX samples per step (vectorisable)
        const float* f = reinterpret_cast<const float*>(y);
        for (int j = 0; j < nb8; j++) {
            const float* p = f + size_t(j) * 2 * ON_BOX;
            float e = 0.0f, sr = 0.0f, si = 0.0f, zr = 0.0f, zi = 0.0f;
            for (int i = 0; i < ON_BOX; i++) {
                const float a = p[2 * i], b = p[2 * i + 1];
                e += a * a + b * b;
                sr += a;
                si += b;
            }
            // x[i] conj(x[i-1]) within the box and across from the previous one
            const int i0 = j == 0 ? 1 : 0;
            for (int i = i0; i < ON_BOX; i++) {
                const float a = p[2 * i], b = p[2 * i + 1], c = p[2 * i - 2], d = p[2 * i - 1];
                zr += a * c + b * d;
                zi += b * c - a * d;
            }
            eb8[size_t(j)] = e;
            zb8[size_t(j)] = cf(zr, zi);
            yb8[size_t(j)] = cf(sr, si);
        }
    }
    if (nb8 < 8) return;
    double etot = 0.0;
    for (float v : eb8) etot += v;
    const double emean = etot / (double(nb8) * ON_BOX);   // mean |x|^2
    if (!(emean > 1e-20)) return;   // silence: only the detector scalars
    es.resize(size_t(nb8));
    for (int j = 0; j < nb8; j++) es[size_t(j)] = eb8[size_t(j)] / ON_BOX;
    double noise = vi.noise_var;
    // (quantiles of at most 16384 evenly strided boxes)
    {
        const size_t st = (es.size() + 16383) / 16384;
        tmpv.clear();
        for (size_t i = 0; i < es.size(); i += st) tmpv.push_back(es[i]);
    }
    if (!(noise > 0.0) || !finite_d(noise)) noise = std::max(1e-20, double(quantile(tmpv.data(), tmpv.size(), 0.1, scratch)));
    const double q90 = quantile(tmpv.data(), tmpv.size(), 0.9, scratch);
    const double shi = std::max(q90 - noise, 0.0);
    const double thr_on = noise * std::sqrt(1.0 + shi / noise);
    on8.resize(size_t(nb8));
    int non = 0;
    double eon = 0.0;
    for (int j = 0; j < nb8; j++) {
        on8[size_t(j)] = es[size_t(j)] > thr_on;
        if (on8[size_t(j)]) { non++; eon += es[size_t(j)]; }
    }
    const double on_frac = double(non) / nb8;
    meta[7] = float(on_frac);
    meta[8] = clampf(db10(non ? (eon / non) / noise - 1.0 : 1e-3) / 30.0f, -1.0f, 4.0f);
    // on / off runs in time
    {
        std::vector<float> on_runs, off_runs;
        int j = 0;
        while (j < nb8) {
            int k = j;
            while (k + 1 < nb8 && on8[size_t(k + 1)] == on8[size_t(j)]) k++;
            const float len = float(k - j + 1) * ON_BOX / float(fs_v);
            const bool interior = j > 0 && k < nb8 - 1;
            if (on8[size_t(j)]) on_runs.push_back(len);
            else if (interior) off_runs.push_back(len);
            j = k + 1;
        }
        const double T = double(n) / fs_v;
        const double rate = double(on_runs.size()) / T;
        const float mon = median_of(on_runs, mscratch), moff = median_of(off_runs, mscratch);
        float* em = out + group_offset("env_mom");
        meta[14] = clampf(float(std::log10(1.0 + rate) / 3.0), 0.0f, 3.0f);
        meta[15] = on_runs.empty() ? 0.0f : clampf(float((std::log10(std::max(mon, 1e-7f)) + 6.0) / 5.0), 0.0f, 2.0f);
        em[7] = meta[14];
        em[8] = meta[15];
        em[9] = off_runs.empty() ? 0.0f : clampf(float((std::log10(std::max(moff, 1e-7f)) + 6.0) / 5.0), 0.0f, 2.0f);
        float emx = 0.0f;
        for (float v : es) emx = std::max(emx, v);
        em[10] = clampf(db10(emx / emean) / 20.0f, -1.0f, 3.0f);
    }

    // ---- the analysed set
    FEAT_PHASE(P_SET);
    spans.clear();
    std::vector<int> chosen;
    if (n <= FEAT_MAX) {
        chosen.push_back(-1);
    } else {
        const int nch = n / CHUNK;
        const int per = CHUNK / ON_BOX;
        std::vector<std::pair<float, int>> en(static_cast<size_t>(nch));
        for (int c = 0; c < nch; c++) {
            double s = 0.0;
            for (int j = c * per; j < (c + 1) * per && j < nb8; j++) s += eb8[size_t(j)];
            en[size_t(c)] = {float(s), c};
        }
        std::vector<uint8_t> pick(static_cast<size_t>(nch), 0);
        for (int q = 0; q < NCHUNK_WIDE / 2; q++) pick[size_t(std::lround(double(q) * (nch - 1) / (NCHUNK_WIDE / 2 - 1)))] = 1;
        std::stable_sort(en.begin(), en.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int have = 0;
        for (uint8_t p : pick) have += p;
        for (const auto& e : en) {
            if (have >= NCHUNK_WIDE) break;
            if (!pick[size_t(e.second)]) { pick[size_t(e.second)] = 1; have++; }
        }
        for (int c = 0; c < nch; c++)
            if (pick[size_t(c)]) chosen.push_back(c);
    }
    size_t ntot = 0;
    for (int c : chosen) ntot += c < 0 ? size_t(n) : size_t(CHUNK);
    yc.resize(ntot);
    ec.resize(ntot);
    fc.assign(ntot, 0.0f);
    fg.assign(ntot, 0.0f);
    oc.resize(ntot);
    {
        size_t o = 0;
        for (int c : chosen) {
            const int s0 = c < 0 ? 0 : c * CHUNK, len = c < 0 ? n : CHUNK;
            spans.push_back({int(o), len});
            std::memcpy(yc.data() + o, y + s0, size_t(len) * sizeof(cf));
            for (int i = 0; i < len; i++) {
                ec[o + size_t(i)] = std::norm(y[s0 + i]);
                const int j = (s0 + i) / ON_BOX;
                oc[o + size_t(i)] = j < nb8 ? on8[size_t(j)] : 0;
            }
            // IF: energy-weighted mean phase step over 3 steps
            const float k = float(fs_v / (2.0 * PI));
            for (int i = 1; i < len; i++) {
                cf z = yc[o + size_t(i)] * std::conj(yc[o + size_t(i) - 1]);
                if (i >= 2) z += yc[o + size_t(i) - 1] * std::conj(yc[o + size_t(i) - 2]);
                if (i >= 3) z += yc[o + size_t(i) - 2] * std::conj(yc[o + size_t(i) - 3]);
                fc[o + size_t(i)] = fast_atan2(z.imag(), z.real()) * k;
            }
            o += size_t(len);
        }
    }
    const int N = int(ntot);
    // on samples (all when none is on)
    int nonc = 0;
    for (uint8_t v : oc) nonc += v;
    const bool use_all = nonc < 16;
    auto is_on = [&](int i) { return use_all || oc[size_t(i)]; };
    // IF median over on samples, gated IF
    double if_med = 0.0;
    {
        tmpv.clear();
        for (int i = 0; i < N; i++)
            if (is_on(i)) tmpv.push_back(fc[size_t(i)]);
        const float med = median_of(tmpv, mscratch);
        if_med = med;
        for (int i = 0; i < N; i++) {
            fc[size_t(i)] -= med;
            fg[size_t(i)] = is_on(i) ? fc[size_t(i)] : 0.0f;
        }
    }

    // ---- psd + spectral scalars
    FEAT_PHASE(P_PSD);
    {
        const int seg = seg_for(PSD_SEG, N, 128);
        welch_c(yc.data(), seg, PSD_K, S);
        float* g = out + group_offset("psd");
        const int per = seg / 128;
        float pmax = 0.0f;
        std::vector<float> pooled(128, 0.0f);
        for (int b = 0; b < 128; b++) {
            float s = 0.0f;
            for (int q = 0; q < per; q++) s += S[size_t(b * per + q)];
            pooled[size_t(b)] = s / float(per);
            pmax = std::max(pmax, pooled[size_t(b)]);
        }
        for (int b = 0; b < 128; b++)
            g[b] = clampf(db10(pooled[size_t(b)] / std::max(pmax, 1e-30f)), -50.0f, 0.0f) / 25.0f + 1.0f;
        // scalars from the full-resolution PSD, excess over the noise level
        std::vector<float> ex(static_cast<size_t>(seg));
        double E = 0.0;
        for (int k = 0; k < seg; k++) {
            ex[size_t(k)] = float(std::max(0.0, double(S[size_t(k)]) - noise));
            E += ex[size_t(k)];
        }
        if (E > 0.0) {
            double cum = 0.0;
            int lo = 0, hi = seg - 1;
            bool hl = false;
            for (int k = 0; k < seg; k++) {
                cum += ex[size_t(k)];
                if (!hl && cum >= 0.01 * E) { lo = k; hl = true; }
                if (cum >= 0.99 * E) { hi = k; break; }
            }
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0, lg = 0;
            for (int k = lo; k <= hi; k++) {
                const double x = ex[size_t(k)], f = double(k - seg / 2);
                s0 += x; s1 += x * f; s2 += x * f * f; s3 += x * f * f * f;
                lg += std::log(std::max(x, 1e-30));
            }
            const int nb = hi - lo + 1;
            const double mu = s1 / s0, var = std::max(s2 / s0 - mu * mu, 1e-12);
            const double am = s0 / nb, gm = std::exp(lg / nb);
            meta[9] = clampf(float(gm / std::max(am, 1e-30)), 0.0f, 1.0f);
            meta[10] = clampf(float((s3 / s0 - 3 * mu * s2 / s0 + 2 * mu * mu * mu) / std::pow(var, 1.5)), -3.0f, 3.0f);
            // edge steepness: width at -3 dB over width at -20 dB (re the peak)
            float pk = 0.0f;
            for (int k = lo; k <= hi; k++) pk = std::max(pk, ex[size_t(k)]);
            auto width_at = [&](float lev) {
                int a = -1, b = -1;
                for (int k = 0; k < seg; k++)
                    if (ex[size_t(k)] >= lev) { if (a < 0) a = k; b = k; }
                return a < 0 ? 0 : b - a + 1;
            };
            const int w3 = width_at(pk * 0.5f), w20 = width_at(pk * 0.01f);
            meta[11] = w20 > 0 ? clampf(float(w3) / float(w20), 0.0f, 1.0f) : 0.0f;
            meta[12] = clampf(float(double(nb) / seg), 0.0f, 1.0f);
            meta[13] = clampf(float(mu / seg * 10.0), -5.0f, 5.0f);
        }
    }

    // ---- envelope
    FEAT_PHASE(P_ENV);
    double m2 = 0, m4 = 0;
    {
        ScalarStats amp, env;
        for (int i = 0; i < N; i++) {
            if (!is_on(i)) continue;
            const double e = ec[size_t(i)];
            m2 += e;
            m4 += e * e;
            amp.add(std::sqrt(e));
        }
        const double cnt = amp.n > 0 ? amp.n : 1.0;
        m2 /= cnt;
        m4 /= cnt;
        float* h = out + group_offset("env_hist");
        if (m2 > 0.0) {
            std::vector<int> hist(32, 0);
            const double inv = 1.0 / std::sqrt(m2);
            tmpv.clear();
            for (int i = 0; i < N; i++) {
                if (!is_on(i)) continue;
                const double a = std::sqrt(double(ec[size_t(i)])) * inv;
                hist[size_t(std::min(31, int(a * 10.0)))]++;
                env.add(ec[size_t(i)] / m2);
                tmpv.push_back(ec[size_t(i)]);
            }
            for (int b = 0; b < 32; b++) h[b] = clampf(float(std::sqrt(hist[size_t(b)] / cnt / 0.1) / 2.0), 0.0f, 3.0f);
            float* em = out + group_offset("env_mom");
            // (of at most 16384 evenly strided values)
            const size_t st = (tmpv.size() + 16383) / 16384;
            if (st > 1) {
                size_t o = 0;
                for (size_t i = 0; i < tmpv.size(); i += st) tmpv[o++] = tmpv[i];
                tmpv.resize(o);
            }
            const float q999 = quantile(tmpv.data(), tmpv.size(), 0.999, scratch);
            em[0] = clampf(db10(q999 / m2) / 10.0f, -1.0f, 3.0f);
            em[1] = clampf(float(m4 / (m2 * m2) - 1.5), -1.0f, 4.0f);
            em[5] = clampf(float(std::sqrt(env.var())), 0.0f, 4.0f);
            em[6] = clampf(float(nonc) / float(N), 0.0f, 1.0f);
            em[11] = clampf(float(amp.skew()), -3.0f, 3.0f);
        }
    }
    FEAT_PHASE(P_ENVSPEC);
    {
        // envelope spectrum (all samples), re a Gaussian-noise envelope
        double em_all = 0.0;
        for (int i = 0; i < N; i++) em_all += ec[size_t(i)];
        em_all /= N;
        tmpv.resize(size_t(N));
        for (int i = 0; i < N; i++) tmpv[size_t(i)] = float(ec[size_t(i)] - em_all);
        const int seg = seg_for(ENV_SEG, N, 32);
        std::vector<float> Se;
        welch_r(tmpv.data(), seg, ENV_K, Se);
        float* g = out + group_offset("env_spec");
        const int nbin = seg / 2;
        const double norm = em_all > 0.0 ? 1.0 / (em_all * em_all) : 0.0;
        for (int b = 0; b < 128; b++) {
            const int k = 1 + int(int64_t(b) * nbin / 128);
            g[b] = clampf(db10(Se[size_t(std::min(k, nbin))] * norm) / 20.0f, -2.0f, 2.0f);
        }
    }

    // ---- low-frequency envelope / IF spectra from box sums of the whole view
    FEAT_PHASE(P_LO);
    {
        const int dlo = std::max(1, int(std::lround(fs_v / LO_RATE / ON_BOX)));
        const int nlo = nb8 / dlo;
        if (nlo >= 16) {
            const double flo = fs_v / (double(dlo) * ON_BOX);
            std::vector<float> elo(static_cast<size_t>(nlo)), filo(static_cast<size_t>(nlo));
            std::vector<uint8_t> onlo(static_cast<size_t>(nlo));
            double me = 0.0;
            for (int j = 0; j < nlo; j++) {
                double s = 0.0;
                cf z(0.0f, 0.0f);
                int o = 0;
                for (int q = 0; q < dlo; q++) {
                    s += eb8[size_t(j * dlo + q)];
                    z += zb8[size_t(j * dlo + q)];
                    o += on8[size_t(j * dlo + q)];
                }
                elo[size_t(j)] = float(s / (double(dlo) * ON_BOX));
                filo[size_t(j)] = fast_atan2(z.imag(), z.real()) * float(fs_v / (2.0 * PI));
                onlo[size_t(j)] = 2 * o >= dlo;
                me += elo[size_t(j)];
            }
            me /= nlo;
            int nfft = 64;
            while (nfft < nlo && nfft < 16384) nfft *= 2;
            const int use = std::min(nlo, nfft);
            Fft& f = plan(nfft, true);
            ra.assign(size_t(nfft), 0.0f);
            rb.resize(size_t(nfft));
            std::vector<float> w;
            hann(use, w);
            double w2 = 0.0;
            for (int i = 0; i < use; i++) w2 += double(w[size_t(i)]) * w[size_t(i)];
            auto spectrum = [&](std::vector<float>& Sx) {
                f.fwd_real(ra.data(), rb.data());
                Sx.assign(size_t(nfft / 2 + 1), 0.0f);
                Sx[0] = rb[0] * rb[0];
                Sx[size_t(nfft / 2)] = rb[1] * rb[1];
                for (int k = 1; k < nfft / 2; k++)
                    Sx[size_t(k)] = rb[size_t(2 * k)] * rb[size_t(2 * k)] + rb[size_t(2 * k + 1)] * rb[size_t(2 * k + 1)];
                for (float& v : Sx) v = float(v / w2);
            };
            std::vector<float> Sx, pooled;
            for (int i = 0; i < use; i++) ra[size_t(i)] = (elo[size_t(i)] - float(me)) * w[size_t(i)];
            spectrum(Sx);
            const double norm = me > 0.0 ? double(dlo) * ON_BOX / (me * me) : 0.0;
            log_pool(Sx, flo / nfft, LO_F0, LO_F1, 32, pooled);
            float* g = out + group_offset("env_spec_lo");
            for (int b = 0; b < 32; b++) g[b] = clampf(db10(pooled[size_t(b)] * norm) / 20.0f, -2.0f, 3.0f);
            // IF: mean removed over on boxes, off boxes 0
            double mf = 0.0;
            int cnt = 0;
            for (int j = 0; j < nlo; j++)
                if (onlo[size_t(j)]) { mf += filo[size_t(j)]; cnt++; }
            mf = cnt ? mf / cnt : 0.0;
            std::fill(ra.begin(), ra.end(), 0.0f);
            for (int i = 0; i < use; i++) ra[size_t(i)] = onlo[size_t(i)] ? (filo[size_t(i)] - float(mf)) * w[size_t(i)] : 0.0f;
            spectrum(Sx);
            log_pool(Sx, flo / nfft, LO_F0, LO_F1, 32, pooled);
            const float med = std::max(median_of(pooled, mscratch), 1e-30f);
            float* gf = out + group_offset("if_spec_lo");
            if (cnt >= 8)
                for (int b = 0; b < 32; b++) gf[b] = clampf(db10(pooled[size_t(b)] / med) / 20.0f, -1.0f, 3.0f);
        }
    }

    // ---- instantaneous frequency
    FEAT_PHASE(P_IF);
    {
        float* h = out + group_offset("if_hist");
        float* im = out + group_offset("if_mom");
        const double range = 0.3 * fs_v;
        std::vector<int> hist(64, 0);
        ScalarStats st;
        int near0 = 0, cnt = 0;
        double dsum = 0.0;
        int dcnt = 0;
        float fmin = 0.0f, fmax = 0.0f;
        tmpv.clear();
        for (const Span& s : spans) {
            for (int i = s.start + 1; i < s.start + s.len; i++) {
                if (!is_on(i)) continue;
                const float f = fc[size_t(i)];
                const int b = int(std::floor((f + range) / (2.0 * range) * 64.0));
                hist[size_t(std::clamp(b, 0, 63))]++;
                st.add(f / fs_v);
                near0 += std::fabs(f) < 0.05 * fs_v;
                cnt++;
                tmpv.push_back(std::fabs(f));
                fmin = std::min(fmin, f);
                fmax = std::max(fmax, f);
                if (i > s.start + 1 && is_on(i - 1)) {
                    dsum += std::fabs(f - fc[size_t(i - 1)]);
                    dcnt++;
                }
            }
        }
        if (cnt > 0) {
            for (int b = 0; b < 64; b++) h[b] = clampf(float(std::sqrt(hist[size_t(b)] * 64.0 / cnt) / 2.0), 0.0f, 4.0f);
            im[0] = clampf(float(std::sqrt(st.var()) * 4.0), 0.0f, 4.0f);
            im[1] = clampf(float(st.kurt() / 3.0), 0.0f, 10.0f);
            im[2] = float(near0) / float(cnt);
            im[3] = clampf(float(dcnt ? dsum / dcnt / fs_v * 4.0 : 0.0), 0.0f, 4.0f);
            // modes of the (3-bin smoothed) histogram above 20 % of its maximum
            std::vector<float> sm(64);
            float mx = 0.0f;
            for (int b = 0; b < 64; b++) {
                sm[size_t(b)] = float(hist[size_t(std::max(0, b - 1))] + 2 * hist[size_t(b)] + hist[size_t(std::min(63, b + 1))]);
                mx = std::max(mx, sm[size_t(b)]);
            }
            int modes = 0;
            for (int b = 1; b < 63; b++)
                if (sm[size_t(b)] > 0.2f * mx && sm[size_t(b)] >= sm[size_t(b - 1)] && sm[size_t(b)] > sm[size_t(b + 1)]) modes++;
            im[4] = float(std::min(modes, 16)) / 8.0f;
            im[5] = clampf(float(st.skew()), -3.0f, 3.0f);
            im[6] = clampf(float(median_of(tmpv, mscratch) / fs_v * 4.0), 0.0f, 4.0f);
            im[7] = clampf(float((fmax - fmin) / fs_v), 0.0f, 2.0f);
        }
        const int seg = seg_for(ENV_SEG, N, 32);
        std::vector<float> Sf;
        welch_r(fg.data(), seg, ENV_K, Sf);
        float* g = out + group_offset("if_spec");
        const int nbin = seg / 2;
        std::vector<float> vals(128);
        for (int b = 0; b < 128; b++) vals[size_t(b)] = Sf[size_t(std::min(nbin, 1 + int(int64_t(b) * nbin / 128)))];
        const float med = std::max(median_of(vals, mscratch), 1e-30f);
        if (cnt > 0)
            for (int b = 0; b < 128; b++) g[b] = clampf(db10(vals[size_t(b)] / med) / 20.0f, -1.0f, 3.0f);
    }

    // ---- carrier lines: spectra of x^2 and x^4
    FEAT_PHASE(P_SQ);
    {
        for (int pass = 0; pass < 2; pass++) {
            // p = x^2 or x^4 (scaled by the on power so values stay moderate)
            pw.resize(size_t(N));
            const float sc = m2 > 0.0 ? float(1.0 / m2) : 1.0f;
            for (int i = 0; i < N; i++) {
                cf v = yc[size_t(i)] * yc[size_t(i)] * sc;
                if (pass == 1) v = v * v;
                pw[size_t(i)] = v;
            }
            const int seg = seg_for(SQ_SEG, N, 64);
            std::vector<float> Sq;
            welch_c(pw.data(), seg, SQ_K, Sq);
            float* g = out + group_offset(pass == 0 ? "sq" : "quad");
            const float med = std::max(median_of(Sq, mscratch), 1e-30f);
            const int per = seg / 32;
            for (int b = 0; b < 32; b++) {
                float m = 0.0f;
                for (int q = 0; q < per; q++) m = std::max(m, Sq[size_t(b * per + q)]);
                g[b] = clampf(db10(m / med) / 20.0f, -1.0f, 3.0f);
            }
            int k1 = 0;
            double tot = 0.0;
            for (int k = 0; k < seg; k++) {
                tot += Sq[size_t(k)];
                if (Sq[size_t(k)] > Sq[size_t(k1)]) k1 = k;
            }
            int k2 = -1;
            for (int k = 0; k < seg; k++)
                if (std::abs(k - k1) > 3 && (k2 < 0 || Sq[size_t(k)] > Sq[size_t(k2)])) k2 = k;
            double line = 0.0;
            for (int k = k1 - 2; k <= k1 + 2; k++) line += Sq[size_t((k + seg) % seg)];
            double lg = 0.0;
            for (int k = 0; k < seg; k++) lg += std::log(std::max(double(Sq[size_t(k)]), 1e-30));
            const double am = tot / seg, gm = std::exp(lg / seg);
            g[32] = clampf(db10(Sq[size_t(k1)] / med) / 20.0f, -1.0f, 4.0f);
            g[33] = clampf(float(std::fabs(double(k1 - seg / 2)) / seg), 0.0f, 0.5f) * 2.0f;
            g[34] = k2 >= 0 ? clampf(db10(Sq[size_t(k2)] / med) / 20.0f, -1.0f, 4.0f) : 0.0f;
            g[35] = k2 >= 0 ? clampf(float(std::abs(k2 - k1)) / float(seg), 0.0f, 1.0f) * 2.0f : 0.0f;
            g[36] = tot > 0.0 ? clampf(float(line / tot), 0.0f, 1.0f) : 0.0f;
            g[37] = am > 0.0 ? clampf(float(gm / am), 0.0f, 1.0f) : 0.0f;
            float* em = out + group_offset("env_mom");
            em[pass == 0 ? 3 : 4] = g[36];
        }
        float* em = out + group_offset("env_mom");
        // C42 with the x^2 line in place of |E x^2| (offset invariant)
        if (m2 > 0.0) em[2] = clampf(float(m4 / (m2 * m2) - em[3] * em[3] - 2.0), -3.0f, 4.0f);
    }

    // ---- periodicity: autocorrelations on a log lag grid
    FEAT_PHASE(P_ACF);
    {
        // level A: segments of up to sa samples (zero-padded to 2 sa), lags <= la_max
        const int sa = vi.view >= NUM_VIEWS - 1 ? ACF_SA_FULL : ACF_SA;
        const int seg = std::min(sa, [&] { int m = 0; for (const Span& s : spans) m = std::max(m, s.len); return m; }());
        pick_segments(sa, ACF_KA);
        const int nfft = 2 * sa;
        Fft& fc_ = plan(nfft, false);
        Fft& fr_ = plan(nfft, true);
        Re.assign(size_t(sa), 0.0);
        Rf.assign(size_t(sa), 0.0);
        Rcc.assign(size_t(sa), cf(0.0f, 0.0f));
        A.resize(size_t(nfft));
        B.resize(size_t(nfft));
        a.resize(size_t(nfft));
        b.resize(size_t(nfft));
        const std::vector<std::pair<int, int>> my = segs;
        for (const auto& sg : my) {
            // complex: |FFT|^2 -> inverse = the (circular-free) autocorrelation
            std::fill(A.begin(), A.end(), cf(0.0f, 0.0f));
            for (int i = 0; i < sg.second; i++) A[size_t(i)] = yc[size_t(sg.first + i)];
            fc_.fwd(A.data(), B.data());
            for (int k = 0; k < nfft; k++) B[size_t(k)] = cf(std::norm(B[size_t(k)]), 0.0f);
            fc_.inv(B.data(), A.data());
            for (int l = 0; l < sa; l++) Rcc[size_t(l)] += A[size_t(l)];
            // real sequences: envelope and gated IF, segment mean removed
            for (int which = 0; which < 2; which++) {
                const float* src = which == 0 ? ec.data() : fg.data();
                double mu = 0.0;
                for (int i = 0; i < sg.second; i++) mu += src[sg.first + i];
                mu /= std::max(1, sg.second);
                std::fill(a.begin(), a.end(), 0.0f);
                for (int i = 0; i < sg.second; i++) a[size_t(i)] = float(src[sg.first + i] - mu);
                fr_.fwd_real(a.data(), b.data());
                b[0] = b[0] * b[0];
                b[1] = b[1] * b[1];
                for (int k = 1; k < nfft / 2; k++) {
                    b[size_t(2 * k)] = b[size_t(2 * k)] * b[size_t(2 * k)] + b[size_t(2 * k + 1)] * b[size_t(2 * k + 1)];
                    b[size_t(2 * k + 1)] = 0.0f;
                }
                fr_.inv_real(b.data(), a.data());
                std::vector<double>& R = which == 0 ? Re : Rf;
                for (int l = 0; l < sa; l++) R[size_t(l)] += a[size_t(l)];
            }
        }
        const int la_max = std::max(1, int(0.625 * seg));
        std::vector<float> rhoA[3];
        for (int w = 0; w < 3; w++) rhoA[w].assign(size_t(la_max) + 1, 0.0f);
        const double c0 = std::abs(Rcc[0]), e0 = Re[0], f0 = Rf[0];
        for (int l = 1; l <= la_max; l++) {
            const double unb = double(seg) / std::max(1, seg - l);
            rhoA[0][size_t(l)] = c0 > 0 ? float(std::min(1.0, std::abs(Rcc[size_t(l)]) / c0 * unb)) : 0.0f;
            rhoA[1][size_t(l)] = e0 > 0 ? float(std::min(1.0, std::fabs(Re[size_t(l)]) / e0 * unb)) : 0.0f;
            rhoA[2][size_t(l)] = f0 > 0 ? float(std::min(1.0, std::fabs(Rf[size_t(l)]) / f0 * unb)) : 0.0f;
        }
        // level B: box sums of the whole view, at most ACF_NB boxes
        int db8 = 1;
        while (nb8 / db8 > ACF_NB) db8++;
        const int nB = nb8 / db8, DB = db8 * ON_BOX;
        std::vector<float> rhoB[3];
        for (int w = 0; w < 3; w++) rhoB[w].assign(size_t(std::max(1, nB / 2)) + 1, 0.0f);
        if (nB >= 8) {
            const int nf = 2 * ACF_NB;
            Fft& fbc = plan(nf, false);
            Fft& fbr = plan(nf, true);
            P.resize(size_t(nf));
            Q.resize(size_t(nf));
            p.resize(size_t(nf));
            q.resize(size_t(nf));
            std::fill(P.begin(), P.end(), cf(0.0f, 0.0f));
            eB.resize(size_t(nB));
            fB.resize(size_t(nB));
            double me = 0.0, mf = 0.0;
            int con = 0;
            for (int j = 0; j < nB; j++) {
                cf s(0.0f, 0.0f), z(0.0f, 0.0f);
                double e = 0.0;
                int o = 0;
                for (int k = 0; k < db8; k++) {
                    s += yb8[size_t(j * db8 + k)];
                    z += zb8[size_t(j * db8 + k)];
                    e += eb8[size_t(j * db8 + k)];
                    o += on8[size_t(j * db8 + k)];
                }
                P[size_t(j)] = s;
                eB[size_t(j)] = float(e);
                const bool on = 2 * o >= db8;
                fB[size_t(j)] = on ? fast_atan2(z.imag(), z.real()) : 0.0f;
                me += e;
                if (on) { mf += fB[size_t(j)]; con++; }
            }
            me /= nB;
            mf = con ? mf / con : 0.0;
            fbc.fwd(P.data(), Q.data());
            for (int k = 0; k < nf; k++) Q[size_t(k)] = cf(std::norm(Q[size_t(k)]), 0.0f);
            fbc.inv(Q.data(), P.data());
            const double pc0 = std::abs(P[0]);
            for (int k = 1; k <= nB / 2; k++)
                rhoB[0][size_t(k)] = pc0 > 0 ? float(std::min(1.0, std::abs(P[size_t(k)]) / pc0 * nB / (nB - k))) : 0.0f;
            for (int which = 0; which < 2; which++) {
                std::fill(p.begin(), p.end(), 0.0f);
                for (int j = 0; j < nB; j++)
                    p[size_t(j)] = which == 0 ? float(eB[size_t(j)] - me) : (fB[size_t(j)] != 0.0f ? float(fB[size_t(j)] - mf) : 0.0f);
                fbr.fwd_real(p.data(), q.data());
                q[0] = q[0] * q[0];
                q[1] = q[1] * q[1];
                for (int k = 1; k < nf / 2; k++) {
                    q[size_t(2 * k)] = q[size_t(2 * k)] * q[size_t(2 * k)] + q[size_t(2 * k + 1)] * q[size_t(2 * k + 1)];
                    q[size_t(2 * k + 1)] = 0.0f;
                }
                fbr.inv_real(q.data(), p.data());
                const double r0 = std::fabs(p[0]);
                for (int k = 1; k <= nB / 2; k++)
                    rhoB[1 + which][size_t(k)] = r0 > 0 ? float(std::min(1.0, std::fabs(p[size_t(k)]) / r0 * nB / (nB - k))) : 0.0f;
            }
        }
        const double ratio = std::pow(LAG1 / LAG0, 1.0 / (NLAG - 1));
        const char* names[3] = {"acf_c", "acf_e", "acf_f"};
        for (int w = 0; w < 3; w++) {
            float* g = out + group_offset(names[w]);
            for (int i = 0; i < NLAG; i++) {
                const double tau = LAG0 * std::pow(ratio, i);
                const double lo = tau / std::sqrt(ratio), hi = tau * std::sqrt(ratio);
                int l0 = int(std::ceil(lo * fs_v)), l1 = int(std::floor(hi * fs_v));
                if (l1 < l0) l0 = l1 = int(std::lround(tau * fs_v));
                if (l1 < 1) { g[i] = 1.0f; continue; }
                l0 = std::max(l0, 1);
                float m = 0.0f;
                if (l1 <= la_max) {
                    for (int l = l0; l <= l1; l++) m = std::max(m, rhoA[w][size_t(l)]);
                } else {
                    int k0 = int(std::ceil(double(l0) / DB)), k1 = int(std::floor(double(l1) / DB));
                    if (k1 < k0) k0 = k1 = int(std::lround(tau * fs_v / DB));
                    k0 = std::max(k0, 1);
                    k1 = std::min(k1, nB / 2);
                    for (int k = k0; k <= k1; k++) m = std::max(m, rhoB[w][size_t(k)]);
                }
                g[i] = m;
            }
        }
        // frame periods on a linear lag grid: narrow views from the box sums of the
        // whole view (5 ms bins up to 240 ms), wide views from the sample-level
        // correlations (22 us bins up to 1.056 ms: OFDM symbol / guard lags)
        const char* fnames[3] = {"frame_c", "frame_e", "frame_f"};
        const bool narrow = vi.view <= 1;
        std::vector<float> fr(FRAME_N), frs(FRAME_N);
        for (int w = 0; w < 3; w++) {
            float* g = out + group_offset(fnames[w]);
            for (int i = 0; i < FRAME_N; i++) {
                float m = 0.0f;
                if (narrow) {
                    const double lo = (i + 0.5) * FRAME_DT_NARROW, hi = (i + 1.5) * FRAME_DT_NARROW;
                    int k0 = std::max(1, int(std::ceil(lo * fs_v / DB))), k1 = std::min(nB / 2, int(std::floor(hi * fs_v / DB)));
                    if (nB >= 8)
                        for (int k = k0; k <= k1; k++) m = std::max(m, rhoB[w][size_t(k)]);
                } else {
                    const double lo = i * FRAME_DT_WIDE, hi = (i + 1) * FRAME_DT_WIDE;
                    int l0 = std::max(1, int(std::ceil(lo * fs_v))), l1 = std::min(la_max, int(std::floor(hi * fs_v)));
                    for (int l = l0; l <= l1; l++) m = std::max(m, rhoA[w][size_t(l)]);
                }
                fr[size_t(i)] = m;
            }
            frs = fr;
            std::nth_element(frs.begin(), frs.begin() + FRAME_N / 2, frs.end());
            const float med = frs[FRAME_N / 2];
            for (int i = 0; i < FRAME_N; i++) g[i] = clampf((fr[size_t(i)] - med) * 8.0f, -2.0f, 8.0f);
        }
    }

    // ---- symbol-synchronous statistics
    FEAT_PHASE(P_SYMFIL);
    {
        float* g = out + group_offset("sym");
        // 0. the channel filter: the detection's band only (a neighbour channel in the
        // view, or the view's wider noise, swamps a weak signal's discriminator output -
        // real P25 / DMR sites: the rate was found in half the detections without it).
        // Windowed sinc, cutoff 0.8 x bw, per span (edges held), then the same
        // 3-term discriminator; its own median removed
        const cf* YC = yc.data();
        const float* FC = fc.data();
        const float* EC = ec.data();
        double sym_med = if_med;
        if (d.bw_hz > 0.0 && d.bw_hz < 0.5 * fs_v) {
            const double fcut = std::min(0.45, 0.8 * d.bw_hz / fs_v);
            const int nt = std::clamp(int(std::ceil(3.0 / fcut)) | 1, 15, 63), hh = nt / 2;
            std::vector<float> h(static_cast<size_t>(nt));
            double hs = 0.0;
            for (int k = 0; k < nt; k++) {
                const double x = k - hh;
                const double sn = x == 0 ? 2.0 * fcut : std::sin(2.0 * PI * fcut * x) / (PI * x);
                h[size_t(k)] = float(sn * (0.54 + 0.46 * std::cos(PI * x / (hh + 1))));
                hs += h[size_t(k)];
            }
            for (float& v : h) v = float(v / hs);
            yfil.resize(size_t(N));
            ffil.assign(size_t(N), 0.0f);
            efil.resize(size_t(N));
            const float k2 = float(fs_v / (2.0 * PI));
            for (const Span& s : spans) {
                const cf* x = yc.data() + s.start;
                cf* y = yfil.data() + s.start;
                for (int i = 0; i < s.len; i++) {
                    float re = 0.0f, im = 0.0f;
                    if (i >= hh && i + hh < s.len) {
                        const cf* q = x + (i - hh);
                        for (int k = 0; k < nt; k++) { re += h[size_t(k)] * q[k].real(); im += h[size_t(k)] * q[k].imag(); }
                    } else {
                        for (int k = 0; k < nt; k++) {
                            const cf v = x[std::clamp(i + k - hh, 0, s.len - 1)];
                            re += h[size_t(k)] * v.real();
                            im += h[size_t(k)] * v.imag();
                        }
                    }
                    y[i] = cf(re, im);
                    efil[size_t(s.start + i)] = re * re + im * im;
                }
                for (int i = 1; i < s.len; i++) {
                    cf z = y[i] * std::conj(y[i - 1]);
                    if (i >= 2) z += y[i - 1] * std::conj(y[i - 2]);
                    if (i >= 3) z += y[i - 2] * std::conj(y[i - 3]);
                    ffil[size_t(s.start + i)] = fast_atan2(z.imag(), z.real()) * k2;
                }
            }
            tmpv.clear();
            for (int i = 0; i < N; i += std::max(1, N / 8192))
                if (is_on(i)) tmpv.push_back(ffil[size_t(i)]);
            const float med = median_of(tmpv, mscratch);
            for (float& v : ffil) v -= med;
            sym_med = med;
            YC = yfil.data();
            FC = ffil.data();
            EC = efil.data();
        }
        FEAT_PHASE(P_SYMRATE);
        // 1. the symbol rate: the strongest cyclic line of |dIF| (FSK: the IF steps at
        // every symbol boundary) or of the envelope (linear modulation), each line
        // against the mean of its neighbourhood (not the coloured spectrum's level)
        int maxlen = 0;
        for (const Span& s : spans) maxlen = std::max(maxlen, s.len);
        int L = 256;
        while (L < maxlen && L < SYM_NFFT) L *= 2;
        Fft& fr_ = plan(L, true);
        const std::vector<float>& wh = win(L);
        std::vector<float> Sd(size_t(L / 2 + 1), 0.0f), Se(size_t(L / 2 + 1), 0.0f);
        ra.resize(size_t(L));
        rb.resize(size_t(L));
        int nseg = 0;
        for (const Span& s : spans) {
            for (int p0 = 0, ns = 0; p0 < s.len && ns < SYM_SEGS; p0 += L, ns++) {
                const int len = std::min(L, s.len - p0);
                if (len < 64) break;
                for (int which = 0; which < 2; which++) {
                    double mu = 0.0;
                    for (int i = 0; i < len; i++) {
                        const int j = s.start + p0 + i;
                        float v;
                        if (which == 0) v = (i > 0 && is_on(j) && is_on(j - 1)) ? std::fabs(FC[size_t(j)] - FC[size_t(j - 1)]) : 0.0f;
                        else v = EC[size_t(j)];
                        ra[size_t(i)] = v;
                        mu += v;
                    }
                    mu /= len;
                    // a short piece: its own Hann window (zero-padded)
                    for (int i = 0; i < len; i++) {
                        const float w = len == L ? wh[size_t(i)] : float(0.5 - 0.5 * std::cos(2.0 * PI * (i + 0.5) / len));
                        ra[size_t(i)] = float((ra[size_t(i)] - mu) * w);
                    }
                    std::fill(ra.begin() + len, ra.end(), 0.0f);
                    fr_.fwd_real(ra.data(), rb.data());
                    std::vector<float>& S = which == 0 ? Sd : Se;
                    for (int k = 1; k < L / 2; k++) S[size_t(k)] += rb[size_t(2 * k)] * rb[size_t(2 * k)] + rb[size_t(2 * k + 1)] * rb[size_t(2 * k + 1)];
                }
                nseg++;
            }
        }
        // search ranges (also by the occupied width: FSK rs >= bw / 40 - slow wide-deviation
        // paging -, a linear modulation rs >= bw / 4; either <= 1.5 bw)
        const int kmin = std::max(3, int(std::ceil(SYM_R0 * L))), kmax = std::min(L / 2 - 4, int(std::floor(SYM_R1 * L)));
        const double kbw = d.bw_hz / fs_v * L;
        const int kd0 = std::max(kmin, int(std::ceil(kbw / 40.0))), ke0 = std::max(kmin, int(std::ceil(kbw / 4.0)));
        const int k1 = std::min(kmax, int(std::floor(1.5 * kbw)));
        const int W = std::max(8, L / 512);
        auto line_ratio = [&](const std::vector<float>& S, int k) {
            // mean of k-W..k+W without k-2..k+2
            double s = 0.0;
            int c = 0;
            for (int j = std::max(1, k - W); j <= std::min(L / 2 - 1, k + W); j++)
                if (std::abs(j - k) > 2) { s += S[size_t(j)]; c++; }
            const double pk = std::max({S[size_t(k)], S[size_t(std::max(1, k - 1))], S[size_t(std::min(L / 2 - 1, k + 1))]});
            return c > 0 && s > 0.0 ? pk / (s / c) : 1.0;
        };
        auto best_line = [&](const std::vector<float>& S, int k0, int k1_, int& kb) {
            // neighbourhood mean by prefix sums, then the best ratio
            std::vector<double> pre(size_t(L / 2 + 1), 0.0);
            for (int k = 1; k <= L / 2; k++) pre[size_t(k)] = pre[size_t(k - 1)] + S[size_t(k - 1)];
            auto sum = [&](int a, int b) { a = std::max(a, 1); b = std::min(b, L / 2 - 1); return b >= a ? pre[size_t(b + 1)] - pre[size_t(a)] : 0.0; };
            double best = 0.0;
            kb = -1;
            for (int k = k0; k <= k1_; k++) {
                const double s = sum(k - W, k + W) - sum(k - 2, k + 2);
                const int c = (std::min(L / 2 - 1, k + W) - std::max(1, k - W) + 1) - 5;
                if (c <= 0 || s <= 0.0) continue;
                const double r = S[size_t(k)] / (s / c);
                if (r > best) { best = r; kb = k; }
            }
            return std::max(best, 1e-3);
        };
        int kd = -1, ke = -1;
        const double rd = k1 > kd0 ? best_line(Sd, kd0, k1, kd) : 1.0, re = k1 > ke0 ? best_line(Se, ke0, k1, ke) : 1.0;
        static const bool dbg_sym = std::getenv("SIGCLASS_DEBUG_SYM") != nullptr;
        if (dbg_sym) std::fprintf(stderr, "sym: view %d bw %.0f  dIF line %.1f Hz %.1f dB  env line %.1f Hz %.1f dB\n", vi.view, d.bw_hz, kd * fs_v / L, db10(rd), ke * fs_v / L, db10(re));
        // the two agree (a linear modulation shows its rate in both): the stronger; else
        // a clear |dIF| line wins (an FSK signal's envelope lines are the recording's or
        // the channel's, not its symbols), else the stronger
        const bool agree = kd > 0 && ke > 0 && std::abs(kd - ke) <= 2;
        const bool use_e = ke > 0 && (agree ? re > rd : (kd < 0 || (db10(rd) < 12.0f && re > rd)));
        const int kb = use_e ? ke : kd;
        const std::vector<float>& Sb = use_e ? Se : Sd;
        if (kb > 0 && nseg > 0) {
            // parabolic interpolation of the line on log power
            double dk = 0.0;
            {
                const double a = std::log(std::max(1e-30, double(Sb[size_t(kb - 1)]))), b = std::log(std::max(1e-30, double(Sb[size_t(kb)]))),
                             c = std::log(std::max(1e-30, double(Sb[size_t(kb + 1)])));
                const double den = a - 2.0 * b + c;
                if (den < -1e-12) dk = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
            }
            const double rs = (kb + dk) * fs_v / L;
            const double sps = fs_v / rs;
            g[0] = clampf(db10(use_e ? re : rd) / 20.0f, -1.0f, 3.0f);
            g[1] = use_e ? 1.0f : 0.0f;
            g[2] = clampf(db10(rd) / 20.0f, -1.0f, 3.0f);
            g[3] = clampf(db10(re) / 20.0f, -1.0f, 3.0f);
            g[4] = clampf(float(rs / fs_v * 4.0), 0.0f, 2.0f);
            g[5] = clampf(float((std::log10(std::max(rs, 1.0)) - 3.0) / 2.0), -2.0f, 2.0f);
            g[6] = clampf(float(rs / std::max(d.bw_hz, 1.0)), 0.0f, 4.0f);
            const int k2 = int(std::lround(2.0 * (kb + dk))), kh = int(std::lround(0.5 * (kb + dk)));
            g[7] = k2 < L / 2 - 3 ? clampf(db10(line_ratio(Sb, k2)) / 20.0f, -1.0f, 3.0f) : 0.0f;
            g[8] = kh >= 3 ? clampf(db10(line_ratio(Sb, kh)) / 20.0f, -1.0f, 3.0f) : 0.0f;
            FEAT_PHASE(P_SYMTIME);
            // 2. per block of <= SYM_BLOCK symbols the timing phase (of SYM_PHASES) whose
            // IF samples cluster most (FSK: the levels; PSK: the IF is still at the symbol
            // centres); at most SYM_MAXSYM symbols (blocks evenly picked); then at that
            // phase the IF at the symbol centres and the phase step over one symbol
            constexpr int NH = 64, NP = 32, NA = 24;
            const double hr = 0.25 * fs_v;
            const double hsc = NH / (2.0 * hr);
            std::vector<int> hif(NH, 0), hdp(NP, 0), cif(NH, 0);
            std::vector<float>& absv = tmpv;
            absv.clear();
            seqv.clear();
            seqp.clear();
            int M = 0, Mdp = 0, seqn = 0;
            const int blk = std::max(16, int(SYM_BLOCK * sps));
            int nblk = 0;
            for (const Span& s : spans) nblk += std::max(0, (s.len - int(2 * sps) - 2 + blk - 1) / blk);
            const double nsym = double(nblk) * std::min(double(SYM_BLOCK), double(N) / std::max(nblk, 1) / sps);
            const int skip = std::max(1, int(std::ceil(nsym / SYM_MAXSYM)));
            auto yat = [&](double t) {
                const int i0 = int(t);
                const float a = float(t - i0);
                return YC[size_t(i0)] * (1.0f - a) + YC[size_t(i0 + 1)] * a;
            };
            const float turn = float(2.0 * PI * sym_med / rs);   // the carrier offset's turn over one symbol
            int bi = 0;
            for (const Span& s : spans) {
                for (int b0 = 0; b0 + int(2 * sps) + 2 < s.len; b0 += blk, bi++) {
                    if (bi % skip) continue;
                    const int b1 = std::min(s.len - 2, b0 + blk);
                    int best = -1;
                    double bscore = -1.0;
                    for (int q = 0; q < SYM_PHASES; q++) {
                        std::fill(cif.begin(), cif.end(), 0);
                        int cnt = 0;
                        for (double t = b0 + sps * (q + 0.5) / SYM_PHASES; t < b1; t += sps) {
                            const int i0 = s.start + int(t);
                            if (!is_on(i0) || !is_on(i0 + 1)) continue;
                            const float a = float(t - int(t));
                            const float v = FC[size_t(i0)] * (1.0f - a) + FC[size_t(i0 + 1)] * a;
                            cif[size_t(std::clamp(int(std::floor((v + hr) * hsc)), 0, NH - 1))]++;
                            cnt++;
                        }
                        if (!cnt) continue;
                        double ci = 0.0;
                        for (int b = 0; b < NH; b++) ci += double(cif[size_t(b)]) * cif[size_t(b)];
                        const double sc = ci / (double(cnt) * cnt);
                        if (sc > bscore) { bscore = sc; best = q; }
                    }
                    if (best < 0) continue;
                    for (double t = b0 + sps * (best + 0.5) / SYM_PHASES; t < b1; t += sps) {
                        const int i0 = s.start + int(t);
                        if (!is_on(i0) || !is_on(i0 + 1)) continue;
                        const float a = float(t - int(t));
                        const float v = FC[size_t(i0)] * (1.0f - a) + FC[size_t(i0 + 1)] * a;
                        hif[size_t(std::clamp(int(std::floor((v + hr) * hsc)), 0, NH - 1))]++;
                        absv.push_back(std::fabs(v));
                        M++;
                        float ph = 0.0f;
                        bool has_ph = false;
                        if (t - sps >= 0.0 && is_on(s.start + int(t - sps))) {
                            const cf z = yat(s.start + t) * std::conj(yat(s.start + t - sps));
                            ph = fast_atan2(z.imag(), z.real()) - turn;
                            ph -= float(2.0 * PI) * std::floor((ph + float(PI)) / float(2.0 * PI));
                            hdp[size_t(std::clamp(int(std::floor((ph + PI) / (2.0 * PI) * NP)), 0, NP - 1))]++;
                            Mdp++;
                            has_ph = true;
                        }
                        if (int(seqv.size()) < SEQ_S) {
                            seqv.push_back(v);
                            seqp.push_back(has_ph ? ph : 0.0f);
                            seqn += has_ph ? 1 : 0;
                        }
                    }
                }
            }
            if (M >= 16) {
                double ci = 0.0, cp = 0.0;
                for (int b = 0; b < NH; b++) {
                    ci += double(hif[size_t(b)]) * hif[size_t(b)];
                    g[14 + b] = clampf(float(std::sqrt(hif[size_t(b)] * double(NH) / M) / 2.0), 0.0f, 4.0f);
                }
                const float q90 = quantile(absv.data(), absv.size(), 0.9, scratch);
                const float q50 = quantile(absv.data(), absv.size(), 0.5, scratch);
                std::vector<int> ha(NA, 0);
                if (q90 > 0.0f)
                    for (float v : absv) ha[size_t(std::min(NA - 1, int(v / q90 / 1.5f * NA)))]++;
                for (int b = 0; b < NA; b++) g[14 + NH + b] = clampf(float(std::sqrt(ha[size_t(b)] * double(NA) / M) / 2.0), 0.0f, 4.0f);
                if (Mdp > 0)
                    for (int b = 0; b < NP; b++) {
                        cp += double(hdp[size_t(b)]) * hdp[size_t(b)];
                        g[14 + NH + NA + b] = clampf(float(std::sqrt(hdp[size_t(b)] * double(NP) / Mdp) / 2.0), 0.0f, 4.0f);
                    }
                g[9] = clampf(float(ci / (double(M) * M) * 4.0), 0.0f, 4.0f);
                g[10] = Mdp > 0 ? clampf(float(cp / (double(Mdp) * Mdp) * 4.0), 0.0f, 4.0f) : 0.0f;
                g[11] = clampf(float(q90 / fs_v * 4.0), 0.0f, 2.0f);
                g[12] = q90 > 0.0f ? clampf(q50 / q90, 0.0f, 1.0f) : 0.0f;
                // the symbol sequence: IF / its robust scale, the phase step / pi
                float* sm = out + group_offset("symseq_meta");
                float* sq = out + group_offset("symseq");
                const int ns = int(seqv.size());
                if (q90 > 0.0f && ns > 0) {
                    const float is = 1.0f / q90;
                    for (int k = 0; k < ns; k++) {
                        sq[k] = clampf(seqv[size_t(k)] * is, -1.5f, 1.5f) / 1.5f;
                        sq[SEQ_S + k] = clampf(seqp[size_t(k)] / float(PI), -1.0f, 1.0f);
                    }
                    sm[0] = float(ns) / float(SEQ_S);
                    sm[1] = clampf(float(q90 / fs_v * 4.0), 0.0f, 2.0f);
                    sm[2] = float(seqn) / float(ns);
                }
                g[13] = clampf(float(M * sps / std::max(1, N)), 0.0f, 1.0f);
            }
        }
    }

    // ---- lag products (chirps)
    FEAT_PHASE(P_LAG);
    {
        float* g = out + group_offset("lagprod");
        static const int lags[3] = {4, 16, 64};
        pw.resize(size_t(N));
        const float sc = m2 > 0.0 ? float(1.0 / m2) : 1.0f;
        for (int q = 0; q < 3; q++) {
            const int dl = lags[q];
            std::fill(pw.begin(), pw.end(), cf(0.0f, 0.0f));
            for (const Span& s : spans)
                for (int i = s.start + dl; i < s.start + s.len; i++)
                    pw[size_t(i)] = yc[size_t(i)] * std::conj(yc[size_t(i - dl)]) * sc;
            const int seg = seg_for(LP_SEG, N, 64);
            std::vector<float> Sl;
            welch_c(pw.data(), seg, LP_K, Sl);
            const float med = std::max(median_of(Sl, mscratch), 1e-30f);
            int k1 = 0;
            double tot = 0.0;
            for (int k = 0; k < seg; k++) {
                tot += Sl[size_t(k)];
                if (Sl[size_t(k)] > Sl[size_t(k1)]) k1 = k;
            }
            int k2 = -1;
            for (int k = 0; k < seg; k++)
                if (std::abs(k - k1) > 3 && (k2 < 0 || Sl[size_t(k)] > Sl[size_t(k2)])) k2 = k;
            double line = 0.0;
            for (int k = k1 - 2; k <= k1 + 2; k++) line += Sl[size_t((k + seg) % seg)];
            g[4 * q + 0] = clampf(db10(Sl[size_t(k1)] / med) / 20.0f, -1.0f, 4.0f);
            g[4 * q + 1] = clampf(float(std::fabs(double(k1 - seg / 2)) / seg) * 2.0f, 0.0f, 1.0f);
            g[4 * q + 2] = tot > 0.0 ? clampf(float(line / tot), 0.0f, 1.0f) : 0.0f;
            g[4 * q + 3] = k2 >= 0 ? clampf(db10(Sl[size_t(k2)] / med) / 20.0f, -1.0f, 4.0f) : 0.0f;
        }
    }

    // ---- time-frequency summary
    FEAT_PHASE(P_TF);
    {
        float* g = out + group_offset("tf");
        Fft& f = plan(TF_N, false);
        const std::vector<float>& w = win(TF_N);
        // frames: within spans, evenly spread, at most TF_FRAMES
        std::vector<int> starts;
        for (const Span& s : spans)
            for (int p = 0; p + TF_N <= s.len; p += TF_N) starts.push_back(s.start + p);
        std::vector<int> fr;
        if (int(starts.size()) <= TF_FRAMES) fr = starts;
        else
            for (int q = 0; q < TF_FRAMES; q++) fr.push_back(starts[size_t(std::lround(double(q) * (starts.size() - 1) / (TF_FRAMES - 1)))]);
        const int nf = int(fr.size());
        if (nf >= 4) {
            A.resize(TF_N);
            B.resize(TF_N);
            spec.resize(size_t(nf) * TF_N);
            const float wn = 1.0f / (0.375f * TF_N);
            for (int t = 0; t < nf; t++) {
                for (int i = 0; i < TF_N; i++) A[size_t(i)] = yc[size_t(fr[size_t(t)] + i)] * w[size_t(i)];
                f.fwd(A.data(), B.data());
                for (int k = 0; k < TF_N; k++) spec[size_t(t) * TF_N + size_t((k + TF_N / 2) % TF_N)] = std::norm(B[size_t(k)]) * wn;
            }
            const double bwb = std::max(1.0, d.bw_hz / fs_v * TF_N);
            std::vector<float> cen, spr, lp;
            std::vector<int> tidx;
            std::vector<uint8_t> any(TF_N, 0);
            double act_sum = 0.0, corr_sum = 0.0;
            int corr_n = 0, prev = -1;
            for (int t = 0; t < nf; t++) {
                const float* r = spec.data() + size_t(t) * TF_N;
                double s0 = 0, s1 = 0, s2 = 0, tot = 0;
                int act = 0;
                for (int k = 0; k < TF_N; k++) {
                    tot += r[k];
                    if (r[k] > 3.0 * noise) {
                        s0 += r[k]; s1 += r[k] * k; s2 += double(r[k]) * k * k;
                        act++;
                        any[size_t(k)] = 1;
                    }
                }
                if (tot / TF_N < 2.0 * noise || act == 0) { prev = -1; continue; }
                const double c = s1 / s0;
                cen.push_back(float(c));
                spr.push_back(float(std::sqrt(std::max(0.0, s2 / s0 - c * c))));
                lp.push_back(float(std::log10(std::max(tot, 1e-30))));
                tidx.push_back(t);
                act_sum += act;
                if (prev >= 0 && prev == t - 1) {
                    const float* q = spec.data() + size_t(prev) * TF_N;
                    double ab = 0, aa = 0, bb = 0;
                    for (int k = 0; k < TF_N; k++) { ab += double(r[k]) * q[k]; aa += double(r[k]) * r[k]; bb += double(q[k]) * q[k]; }
                    if (aa > 0 && bb > 0) { corr_sum += ab / std::sqrt(aa * bb); corr_n++; }
                }
                prev = t;
            }
            const int nc = int(cen.size());
            g[6] = float(nc) / float(nf);
            if (nc >= 3) {
                ScalarStats cs, ls;
                for (float c : cen) cs.add(c);
                for (float l : lp) ls.add(l);
                double step = 0.0, stepmax = 0.0;
                int nstep = 0, up = 0, nz = 0;
                for (int i = 1; i < nc; i++) {
                    if (tidx[size_t(i)] != tidx[size_t(i - 1)] + 1) continue;
                    const double dlt = cen[size_t(i)] - cen[size_t(i - 1)];
                    step += std::fabs(dlt);
                    stepmax = std::max(stepmax, std::fabs(dlt));
                    nstep++;
                    if (std::fabs(dlt) > 0.25) { nz++; up += dlt > 0; }
                }
                double st = 0, tt = 0, ss = 0, mt = 0;
                for (int i = 0; i < nc; i++) mt += tidx[size_t(i)];
                mt /= nc;
                for (int i = 0; i < nc; i++) {
                    const double a = tidx[size_t(i)] - mt, b = cen[size_t(i)] - cs.mean();
                    st += a * b; tt += a * a; ss += b * b;
                }
                int nany = 0;
                for (uint8_t v : any) nany += v;
                double msp = 0.0;
                for (float s : spr) msp += s;
                msp /= nc;
                const double sdc = std::sqrt(cs.var());
                g[0] = clampf(float(sdc / bwb), 0.0f, 4.0f);
                g[1] = clampf(float(nstep ? step / nstep / bwb : 0.0), 0.0f, 4.0f);
                g[2] = (tt > 0 && ss > 0) ? clampf(float(st / std::sqrt(tt * ss)), -1.0f, 1.0f) : 0.0f;
                g[3] = clampf(float(msp / bwb), 0.0f, 4.0f);
                g[4] = nany ? clampf(float(act_sum / nc / nany), 0.0f, 1.0f) : 0.0f;
                g[5] = corr_n ? clampf(float(corr_sum / corr_n), 0.0f, 1.0f) : 0.0f;
                g[7] = nz ? float(up) / float(nz) : 0.5f;
                g[8] = clampf(float(std::sqrt(ls.var())), 0.0f, 4.0f);
                g[9] = clampf(float(stepmax / bwb), 0.0f, 8.0f);
                g[10] = clampf(float(cs.kurt() / 3.0), 0.0f, 10.0f);
                g[11] = sdc > 1e-6 ? clampf(float((nstep ? step / nstep : 0.0) / sdc), 0.0f, 8.0f) : 0.0f;
            }
        }
    }

    FEAT_PHASE_END();
    for (int i = 0; i < DIM; i++) out[i] = clampf(out[i], -16.0f, 16.0f);
}

}  // namespace detail
}  // namespace sigclass
