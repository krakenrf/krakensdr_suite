// Sweep mode: the engine that stitches heimdall's hops into one spectrum.
// Design, decisions and measurements: docs/dev/sweep-mode.md, "The receiver".

#include "sweep.hpp"

#include "config.hpp"
#include "globals.hpp"                       // active_num_elements, sweep_mode_enabled
#include "kraken_fft.hpp"
#include "signal_processing/noise_floor.hpp" // nfloor::shape_db
#include "utils/parse_num.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#if defined(__linux__)
#include <sys/mman.h>   // BigAlloc
#endif
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <zlib.h>

namespace sweep {
namespace {

constexpr int16_t NONE = std::numeric_limits<int16_t>::min();   // a bin never measured
constexpr double FS = 2400000.0;
// A header frequency is a float32 (128 Hz steps at 1.7 GHz): a tuner further
// than this from a hop centre is not on a hop of the plan (still on an old one,
// or a tuner the sweep doesn't move) and its packets are not used
constexpr double HOP_TOL_HZ = 1500.0;
// The reference (see evaluate): its quantile over the hops' floors, the seam
// steps' width and limit, and the change that rewrites a stored hop
constexpr float REF_QUANTILE = 0.20f;
constexpr int SEAM_HOPS = 6;
constexpr int SEAM_MIN_BLOCK = 1;   // also K' = H: one hop per tuner
constexpr float SEAM_MAX_DB = 6.0f;
constexpr float REWRITE_DB = 0.25f;
constexpr int64_t EVAL_FIRST_MS = 250;    // until the first sweep on a grid is complete
constexpr int64_t EVAL_MS = 1000;
constexpr int SLOTS = 8;
// a hop's 10th percentile below this (dB, the FFT's units) is a tuner that delivers constant samples
constexpr float DEAD_DB = -100.0f;
// a hop that never ends (another control client paused heimdall, this
// client didn't): committed every this many samples and measured again
constexpr int64_t HOP_MAX_SAMPLES = 2400000;                  // packets queued for the worker (~55 ms at 16384 samples)

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int u_of(int n) { return n * 51 / 64; }

// The receiver noise is drawn with its median NOISE_SHOWN_DB over the 0 dB
// line (the regular spectrum's noise sits ~2 dB over its floor too)
constexpr float NOISE_SHOWN_DB = 2.0f;

// A noise bin averaged over m Hann frames (no overlap) is Gamma(m, 1/m)
// distributed: its median stands spread_db(m) over its 10th percentile - the
// hop's floor is measured as that percentile (robust to signals in 90 % of
// the hop) and moved to where the median is, so the noise sits at the same
// height for every FFT size and dwell (m = 2 .. 18750)
double gamma_cdf(int m, double x) {   // P(m, x) for an integer m
    if (x <= 0) return 0.0;
    double lsum = -INFINITY;
    const double lx = std::log(x);
    for (int k = 0; k < m; k++) {
        const double lt = k * lx - std::lgamma(k + 1.0);
        lsum = lsum > lt ? lsum + std::log1p(std::exp(lt - lsum)) : lt + std::log1p(std::exp(lsum - lt));
    }
    return 1.0 - std::exp(lsum - x);
}
double gamma_q(int m, double p) {
    double lo = 0, hi = m * 4.0 + 20;
    for (int i = 0; i < 80; i++) {
        const double mid = 0.5 * (lo + hi);
        (gamma_cdf(m, mid) < p ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}
float spread_db(int m) {
    static float cache[257] = {};
    m = std::max(m, 1);
    if (m <= 256) {
        if (cache[m] == 0.0f) cache[m] = static_cast<float>(10.0 * std::log10(gamma_q(m, 0.5) / gamma_q(m, 0.1)));
        return cache[m];
    }
    // Wilson-Hilferty (chi-square of 2m degrees of freedom), within 0.01 dB here
    const double nu = 2.0 * m, a = 2.0 / (9.0 * nu);
    const double q10 = std::pow(1.0 - a - 1.2815516 * std::sqrt(a), 3.0), q50 = std::pow(1.0 - a, 3.0);
    return static_cast<float>(10.0 * std::log10(q50 / q10));
}

// The samples a hop gets at a dwell: heimdall delivers the fewest whole USB
// packets that hold the dwell - ceil(dwell x 2.4 MS/s / 16384 samples), at
// least one (heimdall's sweep_hop_packets; 5 ms -> 1, 10 -> 2, 20 -> 3,
// 50 -> 8, 100 -> 15), exactly that many per tuner and hop (since
// 2026-11-21: before, a hop held 1.3-1.6 packets at 20 ms)
int64_t hop_samples(int dwell_ms) {
    constexpr int64_t packet = 16384, rate = 2400000;
    const int64_t packets = (static_cast<int64_t>(std::max(dwell_ms, 0)) * rate + 1000 * packet - 1) / (1000 * packet);
    return std::max<int64_t>(1, packets) * packet;
}
// Frames overlap by half where a hop would give fewer than
// SWEEP_OVERLAP_BELOW frames of its own: twice the frames from the same
// samples, at twice the FFTs - only where the averaging is short (a wide
// sweep at a small N has plenty and doesn't pay for it)
constexpr int SWEEP_OVERLAP_BELOW = 16;
bool overlapped(int n, int64_t samples) { return samples / n < SWEEP_OVERLAP_BELOW; }
// Hann frames of N in that many samples
int frames_for(int n, int64_t samples) {
    if (samples < n) return 0;
    return static_cast<int>(overlapped(n, samples) ? 2 * samples / n - 1 : samples / n);
}

// The median of Gamma(m, 1/m) in dB (<= 0): how far a noise bin averaged over
// m frames sits, by its median, under the noise's mean - hops of different m
// (1-2 packets a hop on the KrakenSDR) are levelled by their own
float median_db(int m) {
    static float cache[257] = {};
    static bool done[257] = {};
    m = std::max(m, 1);
    if (m <= 256) {
        if (!done[m]) {
            cache[m] = static_cast<float>(10.0 * std::log10(gamma_q(m, 0.5) / m));
            done[m] = true;
        }
        return cache[m];
    }
    const double a = 2.0 / (9.0 * 2.0 * m);   // Wilson-Hilferty, as spread_db
    return static_cast<float>(10.0 * std::log10(std::pow(1.0 - a, 3.0)));
}

// SWEEP_FULL:<n>'s pool: the smallest power of two with ceil(bins / pool) <= n
// (n >= 2; at least FULL_MIN_VALUES); 1 = every bin
uint32_t pool_for(int64_t bins, uint32_t n) {
    if (n <= 1 || bins <= 0) return 1;
    const uint64_t want = std::max(n, FULL_MIN_VALUES);
    uint32_t pool = 1;
    while (pool < (1u << 30) && (static_cast<uint64_t>(bins) + pool - 1) / pool > want) pool *= 2;
    return pool;
}

// The smallest dwell whose hop holds n samples (whole packets, as hop_samples)
int dwell_for(int n) {
    const int64_t packets = (static_cast<int64_t>(n) + 16383) / 16384;
    return static_cast<int>((packets - 1) * 16384 * 1000 / 2400000 + 1);
}

// The FFT size for the sweep's hops: the request (0 = auto), never more than
// SWEEP_MAX_BINS bins in the sweep nor more than a hop's samples at the dwell
// (an N past one packet spans the hop's packets); auto = the largest N up to
// one packet with at most SWEEP_AUTO_BINS bins that still averages
// SWEEP_AUTO_FRAMES frames in a hop at this dwell (docs: why)
int fft_size_for(int res, int64_t hops, int dwell_ms) {
    const int64_t cap = res == 0 ? SWEEP_AUTO_BINS : SWEEP_MAX_BINS;
    int n = res == 0 ? SWEEP_AUTO_FFT_MAX : res;
    const int64_t smp = hop_samples(dwell_ms);
    while (n > SWEEP_FFT_MIN &&
           (hops * u_of(n) > cap || n > smp || (res == 0 && frames_for(n, smp) < SWEEP_AUTO_FRAMES)))
        n /= 2;
    return n;
}

// --- the settings (uWS loop thread) ----------------------------------------------------
struct Settings {
    int64_t start = 87500000, stop = 108000000;
    uint32_t mask = 0;   // requested tuners (bit = index), 0 = all
    int res = 0;         // 0 = auto
    int dwell = SWEEP_DWELL_DEFAULT_MS;
    bool auto_gain = false;   // SWEEP_AUTO_GAIN (heimdall's per-hop gain; the plan doesn't change)
};
Settings g_set;
std::atomic<bool> g_run{true};
std::atomic<bool> g_active{false};

struct Plan {
    uint32_t gen = 0;        // + 1 on every change the worker must see
    uint32_t grid_gen = 0;   // + 1 when the grid (range, N) changes: the array is new
    int64_t start = 0, stop = 0, f0 = 0;
    int N = 0, U = 0, H = 0, K = 0, Kp = 0, R = 0;
    int64_t bins = 0;
    double bin_hz = 0;
    uint32_t tuners = 0;     // the tuners taking part (bit mask; the first Kp in ascending order measure)
    int dwell = SWEEP_DWELL_DEFAULT_MS;
    int res = 0;
    uint32_t grid = 0;       // the id the pages see
    int avg = 0;             // frames a hop is expected to average (hop_samples at the dwell)
    bool overlap = false;    // 50 % overlapped frames (overlapped())
    // the contract's block of each tuner taking part: hops [blo, bhi) (-1 = none)
    int blo[MAX_CHANNELS] = {-1, -1, -1, -1, -1, -1, -1, -1}, bhi[MAX_CHANNELS] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool ok = false;
};

uint32_t grid_id(int64_t start, int64_t stop, int n) {
    uint32_t h = 2166136261u;   // FNV-1a over the three values
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; i++) { h ^= static_cast<uint8_t>(v >> (8 * i)); h *= 16777619u; }
    };
    mix(static_cast<uint64_t>(start));
    mix(static_cast<uint64_t>(stop));
    mix(static_cast<uint64_t>(n));
    return h ? h : 1u;
}

Plan make_plan(const Settings& s, int avail) {
    Plan p;
    p.start = s.start;
    p.stop = s.stop;
    const int64_t span = s.stop - s.start;
    p.H = static_cast<int>(std::max<int64_t>(1, (span + SWEEP_STEP_HZ - 1) / SWEEP_STEP_HZ));
    p.res = s.res;
    p.N = fft_size_for(s.res, p.H, s.dwell);
    p.U = u_of(p.N);
    p.bin_hz = FS / p.N;
    // bin g at start + g * bin: the bins up to stop (exact integer arithmetic)
    p.bins = std::min<int64_t>(static_cast<int64_t>(p.H) * p.U, span * p.N / static_cast<int64_t>(FS) + 1);
    avail = std::clamp(avail, 1, MAX_CHANNELS);
    const uint32_t all = (1u << avail) - 1u;
    p.tuners = s.mask & all;
    if (!p.tuners) p.tuners = all;
    p.K = std::popcount(p.tuners);
    p.Kp = std::min(p.K, p.H);
    p.R = (p.H + p.Kp - 1) / p.Kp;
    p.f0 = s.start + SWEEP_STEP_HZ / 2;
    p.dwell = s.dwell;
    p.grid = grid_id(p.start, p.stop, p.N);
    p.avg = frames_for(p.N, hop_samples(p.dwell));
    p.overlap = overlapped(p.N, hop_samples(p.dwell));
    for (int t = 0; t < MAX_CHANNELS; t++) p.blo[t] = p.bhi[t] = -1;
    for (int t = 0, k = 0; t < MAX_CHANNELS && k < p.Kp; t++)
        if (p.tuners & (1u << t)) {
            p.blo[t] = static_cast<int>(static_cast<int64_t>(k) * p.H / p.Kp);
            p.bhi[t] = static_cast<int>(static_cast<int64_t>(k + 1) * p.H / p.Kp);
            k++;
        }
    p.ok = true;
    return p;
}

// the k-th tuner taking part (ascending), -1 = none
int tuner_of_rank(uint32_t mask, int k) {
    for (int t = 0; t < MAX_CHANNELS; t++)
        if (mask & (1u << t)) {
            if (k == 0) return t;
            k--;
        }
    return -1;
}

// --- the shared store (g_mu) -----------------------------------------------------------
// The store's big arrays get their own pages on Linux, so freeing them gives
// the memory back: glibc's heap kept it (measured: whole range 4096 -> 8192 ->
// 16384 -> 4096 held 69 MB instead of 56, leaving the mode then held all 91 -
// its mmap threshold rises with every big block freed, so later stores landed
// in a thread arena whose freed top is never returned, malloc_trim included).
// Elsewhere the C library's allocator maps big blocks itself
template<typename T>
struct BigAlloc {
    using value_type = T;
    BigAlloc() = default;
    template<typename U> BigAlloc(const BigAlloc<U>&) {}
    static constexpr size_t MAP_MIN = 256 * 1024;
    T* allocate(size_t n) {
#if defined(__linux__)
        if (n * sizeof(T) >= MAP_MIN) {
            void* q = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (q == MAP_FAILED) throw std::bad_alloc();
            return static_cast<T*>(q);
        }
#endif
        return std::allocator<T>().allocate(n);
    }
    void deallocate(T* q, size_t n) {
#if defined(__linux__)
        if (n * sizeof(T) >= MAP_MIN) {
            munmap(q, n * sizeof(T));
            return;
        }
#endif
        std::allocator<T>().deallocate(q, n);
    }
    template<typename U> bool operator==(const BigAlloc<U>&) const { return true; }
    template<typename U> bool operator!=(const BigAlloc<U>&) const { return false; }
};
using Level = std::vector<int16_t, BigAlloc<int16_t>>;

// lv[0] = the sweep bins in 0.01 dB over the reference (NONE = never measured),
// lv[l][i] = max(lv[l-1][2i], lv[l-1][2i + 1]): a frame's pool is the max over
// a few nodes (range_max), whatever the number of bins - no page re-pools the
// millions of bins of a wide sweep per frame
std::mutex g_mu;
struct Store {
    Plan plan;
    std::vector<float> hop_gain;  // per hop: the gain its last stored packets were taken with (header; NaN = none)
    std::vector<int16_t> map;     // the gain map of the last completed sweep, 0.1 dB (INT16_MIN = none)
    uint64_t map_ver = 0;
    int map_reduced = 0;          // hops in it under the ceiling
    float map_min = NAN;          // the lowest gain in it
    std::vector<Level> lv;
    std::vector<float> floor;     // per hop: its 10th percentile (dB, shape off, as measured), NaN = none
    std::vector<float> applied;   // per hop: the reference its bins are stored against
    std::vector<float> delta;     // per hop: median_db(its frames) - NOISE_SHOWN_DB (its reference = base + step + delta)
    std::vector<int8_t> tuner;    // per hop: who measured it last (-1 none)
    float base = NAN;             // the reference = base + off[tuner]
    std::array<float, MAX_CHANNELS> off{};
    int64_t eval_ms = 0;
    bool any = false;
    uint32_t done = 0;            // sweeps completed on this grid
    float last_s = 0;             // the last complete sweep's duration
    std::vector<float> scratch;   // evaluate
} S;
std::atomic<uint32_t> g_plan_gen{0};
std::atomic<uint32_t> g_mask{0};          // the plan's tuners, for the data receiver
std::atomic<uint64_t> g_version{0};
std::atomic<uint32_t> g_completions{0};
std::atomic<float> g_progress{0.0f};
std::atomic<uint64_t> g_drops{0};
std::atomic<bool> g_heim_dirty{false};
std::atomic<float> g_gain{NAN};       // the common gain (note_gain), NaN = not known yet

int16_t q16(float db) {
    const float v = std::round(db * 100.0f);
    return static_cast<int16_t>(std::clamp(v, -32767.0f, 32767.0f));
}

// level-0 bins [a, b] (inclusive) changed: their ancestors
void pyr_update(size_t a, size_t b) {
    for (size_t l = 1; l < S.lv.size(); l++) {
        a >>= 1;
        b >>= 1;
        const Level& c = S.lv[l - 1];
        Level& d = S.lv[l];
        for (size_t i = a; i <= b && i < d.size(); i++) {
            const size_t k = 2 * i;
            d[i] = std::max(c[k], k + 1 < c.size() ? c[k + 1] : NONE);
        }
    }
}

// max over level-0 bins [a, b)
int16_t range_max(size_t a, size_t b) {
    int16_t m = NONE;
    for (size_t l = 0; a < b && l < S.lv.size(); l++) {
        const Level& v = S.lv[l];
        if (a & 1) m = std::max(m, v[a++]);
        if (b & 1) m = std::max(m, v[--b]);
        a >>= 1;
        b >>= 1;
    }
    return m;
}

float quantile(std::vector<float>& v, size_t n, float q) {
    if (n == 0) return NAN;
    const size_t k = std::min(n - 1, static_cast<size_t>(q * static_cast<float>(n)));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.begin() + static_cast<std::ptrdiff_t>(n));
    return v[k];
}

// (g_mu) shift hop h's stored bins by d dB
void shift_hop(int h, float d) {
    const Plan& p = S.plan;
    const int64_t a = static_cast<int64_t>(h) * p.U, b = std::min<int64_t>(a + p.U, p.bins);
    if (a >= b) return;
    const int dq = static_cast<int>(std::lround(d * 100.0f));
    Level& v = S.lv[0];
    for (int64_t i = a; i < b; i++) {
        if (v[static_cast<size_t>(i)] == NONE) continue;
        v[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(v[static_cast<size_t>(i)] + dq, -32767, 32767));
    }
    pyr_update(static_cast<size_t>(a), static_cast<size_t>(b - 1));
}

// (g_mu) The reference: ONE for the whole sweep - base = the 20th percentile
// of the hops' floors (each hop's 10th percentile) - plus each tuner's gain
// step, measured where its block meets the next one's: the lowest floor of
// the SEAM_HOPS hops on each side (neighbours in frequency, so they differ
// only by the two dongles' gains; a signal only raises a hop's floor, so the
// lowest is noise as long as one hop of the six is). A step past SEAM_MAX_DB
// is no gain step (a wide signal on one side): 0. A signal that fills a hop,
// or several, keeps its height; the dongles' different floors (up to ~4 dB)
// don't show as steps. Hops whose reference moved by more than REWRITE_DB are
// rewritten.
void evaluate() {
    const Plan& p = S.plan;
    S.eval_ms = now_ms();
    if (S.tuner.size() != static_cast<size_t>(p.H) || S.lv.empty()) return;
    std::array<float, MAX_CHANNELS> off{};
    for (int k = 0; k + 1 < p.Kp; k++) {
        const int ta = tuner_of_rank(p.tuners, k), tb = tuner_of_rank(p.tuners, k + 1);
        if (ta < 0 || tb < 0) break;
        const int a0 = static_cast<int>(static_cast<int64_t>(k) * p.H / p.Kp);
        const int b0 = static_cast<int>(static_cast<int64_t>(k + 1) * p.H / p.Kp);
        const int b1 = static_cast<int>(static_cast<int64_t>(k + 2) * p.H / p.Kp);
        float step = 0.0f;
        if (b0 - a0 >= SEAM_MIN_BLOCK && b1 - b0 >= SEAM_MIN_BLOCK) {
            float fa[SEAM_HOPS], fb[SEAM_HOPS];
            int na = 0, nb = 0;
            for (int h = b0 - 1; h >= a0 && h >= b0 - SEAM_HOPS; h--)
                if (S.tuner[static_cast<size_t>(h)] == ta) fa[na++] = S.floor[static_cast<size_t>(h)];
            for (int h = b0; h < b1 && h < b0 + SEAM_HOPS; h++)
                if (S.tuner[static_cast<size_t>(h)] == tb) fb[nb++] = S.floor[static_cast<size_t>(h)];
            if (na >= 1 && nb >= 1) {
                const float d = *std::min_element(fb, fb + nb) - *std::min_element(fa, fa + na);
                if (std::fabs(d) <= SEAM_MAX_DB) step = d;
            }
        }
        off[static_cast<size_t>(tb)] = off[static_cast<size_t>(ta)] + step;
    }
    // tuners outside the plan (hops measured before a change) keep offset 0
    size_t n = 0;
    S.scratch.resize(static_cast<size_t>(p.H));
    for (int h = 0; h < p.H; h++) {
        const int8_t t = S.tuner[static_cast<size_t>(h)];
        if (t < 0) continue;
        S.scratch[n++] = S.floor[static_cast<size_t>(h)] - ((p.tuners >> t) & 1u ? off[static_cast<size_t>(t)] : 0.0f);
    }
    if (n == 0) return;
    S.base = quantile(S.scratch, n, REF_QUANTILE);
    S.off = off;
    bool moved = false;
    for (int h = 0; h < p.H; h++) {
        const int8_t t = S.tuner[static_cast<size_t>(h)];
        if (t < 0) continue;
        const float r = S.base + ((p.tuners >> t) & 1u ? off[static_cast<size_t>(t)] : 0.0f) + S.delta[static_cast<size_t>(h)];
        const float d = S.applied[static_cast<size_t>(h)] - r;
        if (std::fabs(d) > REWRITE_DB) {
            shift_hop(h, d);
            S.applied[static_cast<size_t>(h)] = r;
            moved = true;
        }
    }
    if (moved) g_version.fetch_add(1, std::memory_order_release);
}

// The store's arrays: made and freed outside g_mu (apply_plan)
struct Arrays {
    std::vector<Level> lv;
    std::vector<float> floor, applied, delta;
    std::vector<int8_t> tuner;
    std::vector<float> hop_gain;
    void swap_with_store() {
        lv.swap(S.lv);
        floor.swap(S.floor);
        applied.swap(S.applied);
        delta.swap(S.delta);
        tuner.swap(S.tuner);
        hop_gain.swap(S.hop_gain);
    }
};

void pyr_alloc(std::vector<Level>& lv, size_t n) {
    lv.clear();
    lv.emplace_back(n, NONE);
    size_t m = n;
    while (m > 1) {
        m = (m + 1) / 2;
        lv.emplace_back(m, NONE);
    }
}

// (loop thread) the settings -> the plan; a new grid gets a new array, and
// leaving the mode gives it back (the sweep count stays: a page keeps pairing
// by it). The old arrays go first, then the new ones are made - both outside
// g_mu (tens of MB at the bin cap: the worker isn't held, and the two never
// exist together). Meanwhile the store is empty: no hop is stored, no frame
// built. True = what heimdall's plan holds (centres, tuners, dwell) changed
bool apply_plan(bool bump) {
    Plan np = make_plan(g_set, active_num_elements.load(std::memory_order_relaxed));
    const bool active = g_active.load();
    bool grid = false, heim = false, want = false;
    {
        Arrays old;   // (destroyed after the lock is released)
        {
            std::lock_guard<std::mutex> lk(g_mu);
            const Plan& op = S.plan;
            grid = !op.ok || np.start != op.start || np.stop != op.stop || np.N != op.N;
            heim = !op.ok || np.f0 != op.f0 || np.H != op.H || np.tuners != op.tuners || np.dwell != op.dwell;
            const bool any = grid || heim || np.res != op.res || np.overlap != op.overlap;
            np.gen = op.gen + ((any || bump) ? 1 : 0);
            np.grid_gen = op.grid_gen + (grid ? 1 : 0);
            if (grid || !active) {
                // a new grid, or out of the mode: no data (a receiver that doesn't
                // sweep holds none - up to 64 MB at the bin cap). A new grid also
                // starts its sweep count again
                old.swap_with_store();
                S.base = NAN;
                S.off = {};
                S.any = false;
                if (grid) {
                    S.done = 0;
                    S.last_s = 0;
                    g_progress.store(0.0f);
                    S.map.clear();
                    S.map_reduced = 0;
                    S.map_min = NAN;
                    S.map_ver++;
                }
            }
            want = np.ok && active && S.lv.empty();
            S.plan = np;
            g_mask.store(np.tuners, std::memory_order_relaxed);
            g_plan_gen.store(np.gen, std::memory_order_release);
            if (grid || heim) g_version.fetch_add(1, std::memory_order_release);
            if (heim) g_heim_dirty.store(true);
        }
        old = Arrays{};   // (outside the lock: tens of MB unmapped)
    }
    if (want) {
        // (S.plan and S.lv change only on this thread: still np's grid, still empty)
        Arrays fresh;
        bool made = false;
        try {
            pyr_alloc(fresh.lv, static_cast<size_t>(np.bins));
            fresh.floor.assign(static_cast<size_t>(np.H), NAN);
            fresh.applied.assign(static_cast<size_t>(np.H), 0.0f);
            fresh.delta.assign(static_cast<size_t>(np.H), 0.0f);
            fresh.tuner.assign(static_cast<size_t>(np.H), -1);
            fresh.hop_gain.assign(static_cast<size_t>(np.H), NAN);
            made = true;
        } catch (const std::bad_alloc&) {
            std::cerr << "Sweep: out of memory for " << np.bins << " bins" << std::endl;
            fresh = Arrays{};
        }
        std::lock_guard<std::mutex> lk(g_mu);
        if (made) {
            fresh.swap_with_store();
            S.scratch.reserve(static_cast<size_t>(np.H));
        } else {
            // no store: the plan is not ok (the worker reloads and stops)
            S.plan.ok = false;
            S.plan.gen++;
            g_plan_gen.store(S.plan.gen, std::memory_order_release);
        }
        g_version.fetch_add(1, std::memory_order_release);
    }
    return heim;
}

// --- auto gain: the map, heimdall's answers ---------------------------------------------
std::atomic<bool> g_state_dirty{false};
std::atomic<int> g_ag_known{0};            // 0 not answered yet, 1 heimdall has it, -1 it has not
std::atomic<int64_t> g_ag_asked_ms{0};
std::atomic<bool> g_ag_on{false};          // g_set.auto_gain for the other threads

// (g_mu, at a completion) the gain map of the sweep just completed: per hop its
// packets' gain; a change moves its version (the server sends it). With Auto on
// the ceiling (the common gain) is the highest gain of the sweep - channel 0's
// header gain is a hop's gain then, not the common one
void update_gain_map() {
    const size_t H = S.hop_gain.size();
    std::vector<int16_t> m(H, std::numeric_limits<int16_t>::min());
    float hi = NAN, lo = NAN;
    for (size_t h = 0; h < H; h++) {
        const float g = S.hop_gain[h];
        if (!is_finite_value(g) || g < 0.0f) continue;   // (none, or the tuner's AGC)
        m[h] = static_cast<int16_t>(std::lround(g * 10.0f));
        hi = is_finite_value(hi) ? std::max(hi, g) : g;
        lo = is_finite_value(lo) ? std::min(lo, g) : g;
    }
    if (g_ag_on.load(std::memory_order_relaxed) && is_finite_value(hi)) {
        const float old = g_gain.exchange(std::round(hi * 10.0f) / 10.0f);
        if (!(old == std::round(hi * 10.0f) / 10.0f)) g_state_dirty.store(true);
    }
    const float ceil_db = g_gain.load(std::memory_order_relaxed);
    int reduced = 0;
    for (size_t h = 0; h < H; h++)
        if (m[h] != std::numeric_limits<int16_t>::min() && is_finite_value(ceil_db) && m[h] < std::lround(ceil_db * 10.0f)) reduced++;
    const float mn = reduced > 0 ? lo : NAN;
    if (m != S.map) {
        S.map.swap(m);
        S.map_ver++;
    }
    if (reduced != S.map_reduced || !(mn == S.map_min || (!is_finite_value(mn) && !is_finite_value(S.map_min)))) {
        S.map_reduced = reduced;
        S.map_min = mn;
        g_state_dirty.store(true);
    }
}

// --- the worker ------------------------------------------------------------------------
struct Slot {
    uint32_t counter = 0, round = 0, samples = 0;
    bool retuning = false;
    bool gap = false;                   // a packet before this one was dropped (the ring was full)
    int n = 0;                          // channels in data
    int8_t ch[MAX_CHANNELS] = {};
    float freq[MAX_CHANNELS] = {};
    float gain[MAX_CHANNELS] = {};      // the header's per-channel gain (NaN = unknown)
    std::vector<uint8_t> data;          // n x samples x 2 bytes (grows, never shrinks)
};
std::array<Slot, SLOTS> g_ring;
uint64_t g_head = 0, g_tail = 0;        // g_rmu
std::mutex g_rmu;
std::condition_variable g_rcv;
std::atomic<bool> g_stop{false};
std::thread g_thread;

struct Acc {
    int hop = -1;
    int frames = 0;
    std::vector<float> pw;              // |X|^2 summed, DC centred
    std::vector<uint8_t> pend;          // a block's first samples from the previous packet
    int pend_n = 0;
    int64_t samples = 0;                // since the hop began (HOP_MAX_SAMPLES)
    float gain = NAN;                   // the header's gain of the hop's packets (the last one's)
};

struct Worker {
    Plan p;
    bool have = false;
    std::array<Acc, MAX_CHANNELS> acc;
    kfft_complex* in = nullptr;
    kfft_complex* out = nullptr;
    kfft_plan fft = nullptr;
    int fft_n = 0;
    std::vector<float> win, db, tmp, shape;
    uint32_t shape_ver = 0;
    // rounds
    bool have_counter = false;
    uint32_t counter = 0;
    int last_r = 0, seen = 0;
    bool pass_ok = false;
    int64_t pass_ms = 0;

    ~Worker() {
        if (fft) kfft_destroy_plan(fft);
        kfft_free(in);
        kfft_free(out);
    }

    void reload() {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            p = S.plan;
        }
        have = p.ok;
        if (!have) return;
        if (fft_n != p.N) {
            // the new buffers first: a bad_alloc leaves the old ones (and have = false)
            have = false;
            kfft_complex* nin = kfft_alloc_complex(static_cast<size_t>(p.N));
            kfft_complex* nout = nullptr;
            kfft_plan nfft = nullptr;
            try {
                nout = kfft_alloc_complex(static_cast<size_t>(p.N));
                nfft = kfft_plan_dft_1d(p.N, nin, nout, KFFT_FORWARD);
            } catch (...) {
                kfft_free(nin);
                kfft_free(nout);
                throw;
            }
            if (fft) kfft_destroy_plan(fft);
            kfft_free(in);
            kfft_free(out);
            in = nin;
            out = nout;
            fft = nfft;
            fft_n = p.N;
            win.resize(static_cast<size_t>(p.N));
            for (int i = 0; i < p.N; i++)
                win[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / p.N);
            db.resize(static_cast<size_t>(p.N));
            tmp.resize(static_cast<size_t>(p.N));
            shape_ver = 0;
            shape.clear();
        }
        for (Acc& a : acc) {
            a.hop = -1;
            a.frames = 0;
            a.pend_n = 0;
            a.pw.assign(static_cast<size_t>(p.N), 0.0f);
            a.pend.resize(static_cast<size_t>(p.N) * 2);
        }
        have = true;
        have_counter = false;
        pass_ok = false;
        seen = 0;
    }

    // one block of N samples (uint8 IQ): mean off, Hann, FFT, |X|^2 added
    void block(Acc& a, const uint8_t* b) {
        const int n = p.N;
        uint32_t si = 0, sq = 0;
        for (int i = 0; i < n; i++) {
            si += b[2 * i];
            sq += b[2 * i + 1];
        }
        const float ci = static_cast<float>(si) / n, cq = static_cast<float>(sq) / n;
        for (int i = 0; i < n; i++) {
            const float w = win[static_cast<size_t>(i)];
            in[i][0] = (b[2 * i] - ci) * w;
            in[i][1] = (b[2 * i + 1] - cq) * w;
        }
        kfft_execute(fft);
        float* pw = a.pw.data();
        const int half = n / 2;
        for (int j = 0; j < half; j++) {
            pw[j + half] += out[j][0] * out[j][0] + out[j][1] * out[j][1];
            pw[j] += out[j + half][0] * out[j + half][0] + out[j + half][1] * out[j + half][1];
        }
        a.frames++;
    }

    // Hann frames, with 50 % overlap when the plan says so (a hop of one
    // 16384-sample packet: 7 frames of 4096 instead of 4). Every sample goes
    // through pend (N samples): a frame when it is full, then (overlap) its
    // second half moves to the front
    void feed(Acc& a, const uint8_t* b, uint32_t samples) {
        const int n = p.N, half = n / 2;
        int rem = static_cast<int>(samples);
        while (rem > 0) {
            const int take = std::min(rem, n - a.pend_n);
            std::memcpy(a.pend.data() + 2 * a.pend_n, b, static_cast<size_t>(take) * 2);
            a.pend_n += take;
            b += 2 * take;
            rem -= take;
            if (a.pend_n == n) {
                block(a, a.pend.data());
                if (p.overlap) {
                    std::memmove(a.pend.data(), a.pend.data() + n, static_cast<size_t>(n));   // half the samples
                    a.pend_n = half;
                } else {
                    a.pend_n = 0;
                }
            }
        }
    }

    // the hop tuner t measured ends: its centre U bins into the sweep
    void commit(int t) {
        Acc& a = acc[static_cast<size_t>(t)];
        const int h = a.hop, fr = a.frames;
        a.frames = 0;
        a.pend_n = 0;
        a.samples = 0;
        if (fr == 0) return;
        const int n = p.N, c = n / 2, k0 = (n - p.U) / 2;
        float* pw = a.pw.data();
        if (h >= 0 && h < p.H) {
            // the DC bins: the block's mean is taken off, which leaves a notch
            // there (and an RTL's LO leakage); the neighbours' mean instead -
            // no spike or hole every SWEEP_STEP_HZ
            const float dc = 0.5f * (pw[c - 2] + pw[c + 2]);
            pw[c - 1] = pw[c] = pw[c + 1] = dc;
            nfloor::shape_db(n, shape, &shape_ver);
            const bool sh = static_cast<int>(shape.size()) == n;
            const float norm = 10.0f * std::log10(static_cast<float>(fr));
            for (int i = 0; i < p.U; i++) {
                const int k = k0 + i;
                db[static_cast<size_t>(i)] = 10.0f * std::log10(pw[k] + 1e-20f) - norm - (sh ? shape[static_cast<size_t>(k)] : 0.0f);
            }
            std::copy(db.begin(), db.begin() + p.U, tmp.begin());
            // the hop's floor: where its noise's median would be, - NOISE_SHOWN_DB.
            // (K frames of 50 % overlap spread like Gamma(K) at these quantiles -
            // simulated 1 .. 20 frames, within 0.06 dB)
            const float p10 = quantile(tmp, static_cast<size_t>(p.U), 0.10f);
            // phi = the noise's MEAN (its median p10 + spread, less the median's
            // offset from the mean at this many frames): the same for hops of 1 or
            // 2 packets; the hop is stored against base + step + delta, so its
            // noise median shows at NOISE_SHOWN_DB whatever its frames
            // Auto gain (heimdall's per hop): the hop's levels referred to the ceiling
            // (the common gain) - a signal keeps its height whatever the gain it was
            // taken with; the noise too where the tuner's noise dominates (where
            // the lower gain puts the hop on the converter's floor, that floor
            // shows higher: the sensitivity lost). Manual gain: 0. The tuner's AGC
            // (-1) or an unknown gain: no correction
            const float hg = a.gain, ceil_db = g_gain.load(std::memory_order_relaxed);
            const float corr = (is_finite_value(hg) && hg >= 0.0f && is_finite_value(ceil_db) && ceil_db >= 0.0f) ? ceil_db - hg : 0.0f;
            if (corr != 0.0f)
                for (int i = 0; i < p.U; i++) db[static_cast<size_t>(i)] += corr;
            const float phi = p10 + corr + spread_db(fr) - median_db(fr);
            const float dlt = median_db(fr) - NOISE_SHOWN_DB;
            // a tuner delivering constant samples (a dead dongle: the mean taken off
            // leaves nothing, ~-200 dB) is no measurement - it would drag the reference
            // down for every hop; uint8 noise of even 1 count stands ~60 dB over it
            if (is_finite_value(phi) && p10 > DEAD_DB) {
                store(h, t, phi, dlt, hg);
            } else if (p10 <= DEAD_DB) {
                // and what it measured before goes out of the reference too: the hop
                // measured as the dongle died held part live, part constant samples
                // (a floor several dB low), which pulled the seam step and the base
                std::lock_guard<std::mutex> lk(g_mu);
                if (S.plan.ok && S.plan.grid_gen == p.grid_gen && static_cast<size_t>(h) < S.tuner.size())
                    S.tuner[static_cast<size_t>(h)] = -1;
            }
        }
        std::fill(a.pw.begin(), a.pw.end(), 0.0f);
    }

    void store(int h, int t, float phi, float dlt, float hg) {
        std::lock_guard<std::mutex> lk(g_mu);
        const Plan& sp = S.plan;
        if (!sp.ok || sp.grid_gen != p.grid_gen || S.lv.empty()) return;   // the grid changed meanwhile
        if (static_cast<size_t>(h) < S.hop_gain.size()) S.hop_gain[static_cast<size_t>(h)] = hg;
        if (!is_finite_value(S.base)) S.base = phi;   // the first hop of a grid
        S.floor[static_cast<size_t>(h)] = phi;
        S.delta[static_cast<size_t>(h)] = dlt;
        S.tuner[static_cast<size_t>(h)] = static_cast<int8_t>(t);
        const float r = S.base + ((sp.tuners >> t) & 1u ? S.off[static_cast<size_t>(t)] : 0.0f) + dlt;
        S.applied[static_cast<size_t>(h)] = r;
        const int64_t a = static_cast<int64_t>(h) * sp.U, b = std::min<int64_t>(a + sp.U, sp.bins);
        Level& v = S.lv[0];
        for (int64_t i = a; i < b; i++) v[static_cast<size_t>(i)] = q16(db[static_cast<size_t>(i - a)] - r);
        if (a < b) pyr_update(static_cast<size_t>(a), static_cast<size_t>(b - 1));
        S.any = true;
        if (now_ms() - S.eval_ms >= (S.done == 0 ? EVAL_FIRST_MS : EVAL_MS)) evaluate();
        g_version.fetch_add(1, std::memory_order_release);
    }

    void commit_all() {
        for (int t = 0; t < MAX_CHANNELS; t++)
            if (acc[static_cast<size_t>(t)].frames > 0 || acc[static_cast<size_t>(t)].pend_n > 0) commit(t);
    }

    // frequency_change_counter moved: round r began (all of the last one is in)
    void round_event(int r) {
        const int64_t now = now_ms();
        if (!have_counter) {
            pass_ok = r == 0;
            pass_ms = now;
            seen = 1;
        } else if (r == 0) {
            if (pass_ok && last_r == p.R - 1 && seen >= p.R) {
                std::lock_guard<std::mutex> lk(g_mu);
                if (S.plan.ok && S.plan.grid_gen == p.grid_gen) {
                    S.done++;
                    S.last_s = static_cast<float>(now - pass_ms) / 1000.0f;
                    evaluate();
                    update_gain_map();
                    g_completions.fetch_add(1, std::memory_order_release);
                    g_version.fetch_add(1, std::memory_order_release);
                }
            }
            pass_ok = true;
            pass_ms = now;
            seen = 1;
        } else if (r == last_r + 1) {
            seen++;
        } else if (r != last_r) {
            pass_ok = false;   // a round missed: this pass doesn't count
        }
        last_r = r;
        g_progress.store(p.R > 0 ? static_cast<float>(std::clamp(r, 0, p.R)) / static_cast<float>(p.R) : 0.0f,
                         std::memory_order_relaxed);
    }

    void process(const Slot& s) {
        if (g_plan_gen.load(std::memory_order_acquire) != p.gen || !have) reload();
        if (!have) return;
        // samples are missing before this packet: a frame begun in the packets
        // before would join two pieces of the stream (a frame past one packet,
        // or an overlapped one, spans packets) - it is dropped, the frames
        // already summed stay
        if (s.gap)
            for (Acc& a : acc) a.pend_n = 0;
        if (!have_counter || s.counter != counter) {
            commit_all();
            round_event(static_cast<int>(s.round));
            counter = s.counter;
            have_counter = true;
        }
        if (s.retuning) {
            commit_all();
            return;
        }
        const double step = static_cast<double>(SWEEP_STEP_HZ);
        for (int i = 0; i < s.n; i++) {
            const int t = s.ch[i];
            if (t < 0 || t >= MAX_CHANNELS || !((p.tuners >> t) & 1u)) continue;
            Acc& a = acc[static_cast<size_t>(t)];
            const double f = s.freq[i];
            const long long h = is_finite_value(f) ? std::llround((f - static_cast<double>(p.f0)) / step) : -1;
            // on a hop of the plan AND of this tuner's block (the contract's assignment):
            // a selected tuner heimdall doesn't use (K' < selected) is never retuned and
            // may sit on another tuner's hop centre; frequency 0 = heimdall's "retune
            // failed, no valid frequency"
            if (!is_finite_value(f) || f <= 0 || h < p.blo[t] || h >= p.bhi[t] ||
                std::fabs(f - (static_cast<double>(p.f0) + static_cast<double>(h) * step)) > HOP_TOL_HZ) {
                if (a.frames > 0 || a.pend_n > 0) commit(t);
                a.hop = -1;
                continue;
            }
            if (a.hop != static_cast<int>(h)) {
                if (a.frames > 0 || a.pend_n > 0) commit(t);
                a.hop = static_cast<int>(h);
            }
            feed(a, s.data.data() + static_cast<size_t>(i) * s.samples * 2, s.samples);
            a.gain = s.gain[i];
            a.samples += s.samples;
            if (a.samples >= HOP_MAX_SAMPLES) commit(t);   // (keeps the hop; measured again)
        }
    }
};

void worker_main() {
    Worker w;
    uint64_t drops_told = 0;
    int64_t told_ms = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        // packets the worker had no slot for (a machine too busy): said at most every 10 s
        if (const uint64_t d = g_drops.load(std::memory_order_relaxed); d != drops_told && now_ms() - told_ms > 10000) {
            std::cerr << "Sweep: " << d - drops_told << " packets dropped - the sweep engine fell behind" << std::endl;
            drops_told = d;
            told_ms = now_ms();
        }
        Slot* s = nullptr;
        {
            std::unique_lock<std::mutex> lk(g_rmu);
            g_rcv.wait_for(lk, std::chrono::milliseconds(200),
                           [] { return g_head != g_tail || g_stop.load(std::memory_order_relaxed); });
            if (g_head == g_tail) continue;
            s = &g_ring[static_cast<size_t>(g_tail % SLOTS)];
        }
        try {
            w.process(*s);
        } catch (const std::exception& e) {
            std::cerr << "Sweep: " << e.what() << std::endl;
        }
        std::lock_guard<std::mutex> lk(g_rmu);
        g_tail++;
    }
}

std::string mhz(int64_t hz) {
    char b[32];
    snprintf(b, sizeof b, "%.6f", static_cast<double>(hz) / 1e6);
    return b;
}

// a whole number / decimal, nothing after it
double number(const std::string& s) {
    size_t used = 0;
    double v = 0;
    try {
        v = std::stod(s, &used);
    } catch (const std::exception&) {
        throw std::invalid_argument("not a number: " + s);
    }
    while (used < s.size() && (s[used] == ' ' || s[used] == '\t')) used++;
    if (used != s.size() || !is_finite_value(v)) throw std::invalid_argument("not a number: " + s);
    return v;
}

std::string tuners_cmd() {
    if (!g_set.mask) return "SWEEP_TUNERS:all";
    std::string o = "SWEEP_TUNERS:";
    bool first = true;
    for (int t = 0; t < MAX_CHANNELS; t++)
        if (g_set.mask & (1u << t)) {
            o += (first ? "" : ",") + std::to_string(t);
            first = false;
        }
    return o;
}

}  // namespace

// --- settings ---------------------------------------------------------------------------
std::string set_range(double start_mhz, double stop_mhz) {
    if (!is_finite_value(start_mhz) || !is_finite_value(stop_mhz) || std::fabs(start_mhz) > 1e6 || std::fabs(stop_mhz) > 1e6)
        throw std::invalid_argument("not a frequency");
    int64_t a = std::llround(start_mhz * 1e6), b = std::llround(stop_mhz * 1e6);
    if (a > b) std::swap(a, b);
    a = std::clamp(a, SWEEP_MIN_HZ, SWEEP_MAX_HZ - SWEEP_MIN_SPAN_HZ);
    b = std::clamp(b, a + SWEEP_MIN_SPAN_HZ, SWEEP_MAX_HZ);
    g_set.start = a;
    g_set.stop = b;
    apply_plan(false);
    return "SWEEP_RANGE:" + mhz(a) + ":" + mhz(b);
}

std::string set_tuners(const std::string& list) {
    std::string l = list;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    uint32_t mask = 0;
    if (l != "all" && !l.empty()) {
        std::stringstream ss(l);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (tok.empty()) continue;
            const double v = number(tok);
            if (v != std::floor(v)) throw std::invalid_argument("not a tuner: " + tok);
            if (v >= 0 && v < MAX_CHANNELS) mask |= 1u << static_cast<int>(v);
        }
    }
    g_set.mask = mask;
    apply_plan(false);
    return tuners_cmd();
}

std::string set_res(const std::string& v) {
    std::string l = v;
    for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    int res = 0;
    if (l != "auto") {
        const double d = number(l);
        if (d > 0.5) {
            const double c = std::clamp(d, static_cast<double>(SWEEP_FFT_MIN), static_cast<double>(SWEEP_FFT_MAX));
            res = 1 << static_cast<int>(std::lround(std::log2(c)));   // the nearest power of two
        } else if (d < 0) {
            throw std::invalid_argument("negative FFT size");
        }
    }
    g_set.res = res;
    apply_plan(false);
    return res ? "SWEEP_RES:" + std::to_string(res) : std::string("SWEEP_RES:auto");
}

std::string set_dwell(double ms) {
    if (!is_finite_value(ms)) throw std::invalid_argument("not a number");
    g_set.dwell = static_cast<int>(std::lround(std::clamp(ms, static_cast<double>(SWEEP_DWELL_MIN_MS),
                                                          static_cast<double>(SWEEP_DWELL_MAX_MS))));
    apply_plan(false);
    return "SWEEP_DWELL:" + std::to_string(g_set.dwell);
}

std::string set_auto_gain(bool on) {
    g_set.auto_gain = on;
    g_ag_on.store(on);
    return on ? "SWEEP_AUTO_GAIN:1" : "SWEEP_AUTO_GAIN:0";
}
bool auto_gain() { return g_ag_on.load(); }
std::string auto_gain_command() {
    return std::string("{\"command\":\"set_sweep_auto_gain\",\"enable\":") + (g_set.auto_gain ? "true" : "false") + "}";
}
std::atomic<bool> g_ag_answered{false};   // an "auto_gain" line since the last ask
void note_auto_gain_asked() {
    g_ag_answered.store(false);
    g_ag_asked_ms.store(now_ms());
}

void ingest_control_reply(const char* data, size_t n) {
    // heimdall answers every command with one line; whole lines only (a reply
    // may be split over reads). "auto_gain" in a reply (the success reply to
    // set_sweep_auto_gain, or its status) = it has it; "Unknown command" within
    // 2 s of asking with no such line = it has not
    static std::mutex mu;
    static std::string buf;
    std::lock_guard<std::mutex> lk(mu);
    buf.append(data, n);
    if (buf.size() > (64u << 10)) buf.erase(0, buf.size() - (64u << 10));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
        const std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        // (a heimdall replaced by an older one answers "Unknown command" to the next
        // ask: known goes back to false - a stray unknown-command reply of a new one
        // is corrected by its next status line, which carries "auto_gain")
        int known = 0;
        if (line.find("\"auto_gain\"") != std::string::npos) {
            known = 1;
            g_ag_answered.store(true);
        } else if (line.find("\"Unknown command\"") != std::string::npos) {
            const int64_t asked = g_ag_asked_ms.load();
            if (asked != 0 && now_ms() - asked < 2000 && !g_ag_answered.load()) known = -1;
        }
        if (known != 0 && g_ag_known.exchange(known) != known) g_state_dirty.store(true);
    }
}

bool take_state_dirty() { return g_state_dirty.exchange(false); }

uint64_t gain_map_version() {
    std::lock_guard<std::mutex> lk(g_mu);
    return S.map_ver;
}

std::string gain_map_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    const Plan& p = S.plan;
    std::ostringstream o;
    char b[64];
    o << "{\"sweep_gain\":{\"grid\":" << p.grid << ",\"sweeps\":" << S.done << ",\"f0_hz\":" << p.f0
      << ",\"step_hz\":" << SWEEP_STEP_HZ << ",\"hops\":" << p.H << ",\"ceiling_db\":";
    const float c = g_gain.load();
    if (is_finite_value(c)) {
        snprintf(b, sizeof b, "%.1f", c);
        o << b;
    } else {
        o << "null";
    }
    o << ",\"gain_db\":[";
    for (size_t h = 0; h < S.map.size(); h++) {
        if (h) o << ",";
        if (S.map[h] == std::numeric_limits<int16_t>::min()) {
            o << "null";
        } else {
            snprintf(b, sizeof b, "%.1f", S.map[h] / 10.0);
            o << b;
        }
    }
    o << "]}}";
    return o.str();
}

void set_running(bool on) {
    if (g_run.exchange(on) != on) g_version.fetch_add(1, std::memory_order_release);
}
bool running() { return g_run.load(); }

void on_mode(bool active) {
    g_active.store(active, std::memory_order_release);   // (before apply_plan: it allocates the arrays)
    // the worker restarts its hops and rounds (the tuners moved meanwhile)
    apply_plan(true);
}

bool on_elements() { return apply_plan(false); }
bool take_plan_changed() { return g_heim_dirty.exchange(false); }

bool note_gain(float db, bool from_header) {
    static std::atomic<int64_t> commanded_ms{0};
    static std::atomic<float> commanded{NAN};
    if (!is_finite_value(db)) return false;
    db = db < 0 ? -1.0f : std::round(db * 10.0f) / 10.0f;
    if (!from_header) {
        commanded.store(db);
        commanded_ms.store(now_ms());
    } else if (now_ms() - commanded_ms.load() < 1500 && !(db == commanded.load())) {
        return false;   // the header still shows the gain from before the command
    }
    const float old = g_gain.exchange(db);
    return !(old == db);
}

std::string plan_command() {
    Plan p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = S.plan;
    }
    std::ostringstream o;
    o << "{\"command\":\"set_sweep\",\"first_center_hz\":" << p.f0 << ",\"step_hz\":" << SWEEP_STEP_HZ
      << ",\"steps\":" << p.H << ",\"tuners\":[";
    bool first = true;
    for (int t = 0; t < MAX_CHANNELS; t++)
        if (p.tuners & (1u << t)) {
            o << (first ? "" : ",") << t;
            first = false;
        }
    o << "],\"dwell_ms\":" << p.dwell << ",\"run\":" << (g_run.load() ? "true" : "false") << "}";
    return o.str();
}

std::string state_json() {
    Plan p;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        p = S.plan;
    }
    std::ostringstream o;
    o << "{\"sweep\":{\"active\":" << (sweep_mode_enabled.load() ? "true" : "false") << ",\"start_hz\":" << p.start
      << ",\"stop_hz\":" << p.stop << ",\"min_hz\":" << SWEEP_MIN_HZ << ",\"max_hz\":" << SWEEP_MAX_HZ << ",\"tuners\":[";
    bool first = true;
    for (int t = 0; t < MAX_CHANNELS; t++)
        if (p.tuners & (1u << t)) {
            o << (first ? "" : ",") << t;
            first = false;
        }
    char bin[32];
    snprintf(bin, sizeof bin, "%.8f", p.bin_hz);   // (exact up to N = 65536: 36.62109375)
    o << "],\"available\":" << std::clamp(active_num_elements.load(), 1, MAX_CHANNELS) << ",\"res\":" << p.res
      << ",\"fft_size\":" << p.N << ",\"bin_hz\":" << bin << ",\"bins\":" << p.bins << ",\"hops\":" << p.H
      << ",\"rounds\":" << p.R << ",\"dwell_ms\":" << p.dwell << ",\"running\":" << (g_run.load() ? "true" : "false")
      << ",\"grid\":" << p.grid << ",\"avg\":" << p.avg;
    // (additive, for the page's Resolution list) every size and whether it runs
    // as asked at this range and dwell: "sizes" = those that do; "sizes_capped"
    // = the others and why - "bins" (the sweep would hold more than
    // SWEEP_MAX_BINS: only a narrower range gets it; a longer dwell doesn't) or
    // "dwell" (a hop holds fewer samples); "min_dwell_ms" = the dwell each size
    // past one packet needs (fixed)
    {
        const int64_t smp = hop_samples(p.dwell);
        std::string ok, capped, dw;
        for (int n = SWEEP_FFT_MIN; n <= SWEEP_FFT_MAX; n *= 2) {
            const bool bins_ok = static_cast<int64_t>(p.H) * u_of(n) <= SWEEP_MAX_BINS;
            if (bins_ok && n <= smp) {
                ok += (ok.empty() ? "" : ",") + std::to_string(n);
            } else {
                capped += std::string(capped.empty() ? "" : ",") + "\"" + std::to_string(n) + "\":\"" + (bins_ok ? "dwell" : "bins") + "\"";
            }
            if (n > 16384) dw += std::string(dw.empty() ? "" : ",") + "\"" + std::to_string(n) + "\":" + std::to_string(dwell_for(n));
        }
        o << ",\"sizes\":[" << ok << "],\"sizes_capped\":{" << capped << "},\"min_dwell_ms\":{" << dw << "}";
    }
    // (additive, for the page's Display list) the full-resolution steps this
    // receiver sends for the running grid: SWEEP_FULL:<n> and what it gives;
    // a step that would be the same as full is left out; full (n 1) last
    {
        o << ",\"full_z\":true";   // SWEEP_FULL_Z: deflated parts (type 11 flag 8)
        // auto gain (2026-11-23): the setting, whether heimdall has it, what the
        // last completed sweep's gain map holds
        {
            const int known = g_ag_known.load();
            int reduced;
            float mn;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                reduced = S.map_reduced;
                mn = S.map_min;
            }
            // active: on, heimdall has it, in sweep mode, a manual ceiling (with the
            // tuner's AGC, -1, heimdall's Auto does nothing)
            const float c = g_gain.load();
            const bool active = g_set.auto_gain && known == 1 && sweep_mode_enabled.load() && is_finite_value(c) && c >= 0.0f;
            o << ",\"auto_gain\":" << (g_set.auto_gain ? "true" : "false") << ",\"auto_gain_known\":" << (known == 1 ? "true" : "false")
              << ",\"auto_gain_active\":" << (active ? "true" : "false")
              << ",\"gain_reduced_hops\":" << reduced << ",\"gain_min_db\":";
            if (is_finite_value(mn)) {
                char gb[24];
                snprintf(gb, sizeof gb, "%.1f", mn);
                o << gb;
            } else {
                o << "null";
            }
        }
        o << ",\"full_sets\":[";
        const uint32_t steps[] = {16384, 65536, 262144, 1048576, 4194304};
        bool first = true;
        auto entry = [&](uint32_t n, uint32_t pool) {
            char e[160];
            snprintf(e, sizeof e, "%s{\"n\":%u,\"pool\":%u,\"values\":%lld,\"step_hz\":%.8f}", first ? "" : ",", n, pool,
                     static_cast<long long>((std::max<int64_t>(p.bins, 0) + pool - 1) / pool), p.bin_hz * pool);
            o << e;
            first = false;
        };
        if (p.ok && p.bins > 0) {
            for (uint32_t n : steps)
                if (const uint32_t pool = pool_for(p.bins, n); pool > 1) entry(n, pool);
            entry(1, 1);
        }
        o << "]";
    }
    // (additive) a size asked for that is not used: what caps it - "bins" (the
    // sweep would hold more than SWEEP_MAX_BINS: a narrower range gets it) or
    // "dwell" (a hop holds fewer samples: dwell_ms_needed gets it)
    if (p.res > p.N) {
        int nb = p.res;   // the bins alone
        while (nb > SWEEP_FFT_MIN && static_cast<int64_t>(p.H) * u_of(nb) > SWEEP_MAX_BINS) nb /= 2;
        if (nb > p.N) {
            o << ",\"cap\":\"dwell\",\"dwell_ms_needed\":" << std::min(dwell_for(nb), SWEEP_DWELL_MAX_MS);
        } else {
            o << ",\"cap\":\"bins\"";
        }
    }
    // the common gain (optional: once known)
    if (const float g = g_gain.load(); is_finite_value(g)) {
        char gb[24];
        snprintf(gb, sizeof gb, "%.1f", g);
        o << ",\"gain_db\":" << gb;
    }
    o << "}}";
    return o.str();
}

std::string summary() {
    std::lock_guard<std::mutex> lk(g_mu);
    const Plan& p = S.plan;
    char b[200];
    snprintf(b, sizeof b, "%.3f-%.3f MHz, %d hops, FFT %d (%.0f Hz bins), %d tuner%s%s, %u sweeps (last %.1f s)%s",
             p.start / 1e6, p.stop / 1e6, p.H, p.N, p.bin_hz, p.Kp, p.Kp == 1 ? "" : "s", g_run.load() ? "" : ", paused",
             S.done, S.last_s, g_drops.load() ? (", " + std::to_string(g_drops.load()) + " packets dropped").c_str() : "");
    return b;
}

int64_t bins() {
    std::lock_guard<std::mutex> lk(g_mu);
    return S.plan.bins;
}

// --- the data receiver ----------------------------------------------------------------------
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const float* freq_hz, const float* gain_db, uint32_t counter,
               uint32_t round, bool retuning) {
    if (!g_active.load(std::memory_order_acquire) || !g_run.load(std::memory_order_relaxed)) return;
    // a retuning packet is wanted once (the hops end there), not for every
    // packet of the settle time
    static bool have = false, last_retuning = false;
    static uint32_t last_counter = 0;
    const bool event = !have || counter != last_counter || retuning != last_retuning;
    have = true;
    last_counter = counter;
    last_retuning = retuning;
    if (retuning && !event) return;
    uint64_t head;
    static bool gap = false;
    {
        std::lock_guard<std::mutex> lk(g_rmu);
        if (g_head - g_tail >= SLOTS) {
            g_drops.fetch_add(1, std::memory_order_relaxed);
            gap = true;
            return;   // the worker is behind: the next packet's counter / frequency still tell it what changed
        }
        head = g_head;
    }
    // the slot at head is the producer's until it is published (one producer)
    Slot& s = g_ring[static_cast<size_t>(head % SLOTS)];
    s.counter = counter;
    s.round = round;
    s.retuning = retuning;
    s.gap = gap;
    gap = false;
    s.samples = samples;
    s.n = 0;
    if (!retuning) {
        const uint32_t mask = g_mask.load(std::memory_order_relaxed);
        const size_t per = static_cast<size_t>(samples) * 2;
        int want = 0;
        for (uint32_t c = 0; c < channels && c < static_cast<uint32_t>(MAX_CHANNELS); c++) want += (mask >> c) & 1u;
        if (s.data.size() < per * static_cast<size_t>(want)) s.data.resize(per * static_cast<size_t>(want));
        for (uint32_t c = 0; c < channels && c < static_cast<uint32_t>(MAX_CHANNELS); c++) {
            if (!((mask >> c) & 1u)) continue;
            std::memcpy(s.data.data() + per * static_cast<size_t>(s.n), iq + per * c, per);
            s.ch[s.n] = static_cast<int8_t>(c);
            s.freq[s.n] = freq_hz[c];
            s.gain[s.n] = gain_db ? gain_db[c] : NAN;
            s.n++;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_rmu);
        g_head++;
    }
    g_rcv.notify_one();
}

// --- frames -------------------------------------------------------------------------------------
uint64_t data_version() { return g_version.load(std::memory_order_acquire); }
uint32_t completions() { return g_completions.load(std::memory_order_acquire); }

namespace {
// (g_mu) the bins of a window and the pool, as build_spectrum_view picks them
struct Cut {
    size_t i0 = 0, i1 = 0, pool = 1, n = 0;
    bool windowed = false;
};
Cut cut_for(double lo, double hi, uint32_t points) {
    Cut c;
    const Plan& p = S.plan;
    const size_t L = static_cast<size_t>(p.bins);
    if (L < 2) return c;
    c.i1 = L - 1;
    if (hi > lo) {
        const double a = std::ceil((lo - static_cast<double>(p.start)) / p.bin_hz - 1e-6);
        const double b = std::floor((hi - static_cast<double>(p.start)) / p.bin_hz + 1e-6);
        const double last = static_cast<double>(L - 1);
        const double ca = std::clamp(a, 0.0, last), cb = std::clamp(b, 0.0, last);
        if (cb - ca >= 1.0) {
            c.i0 = static_cast<size_t>(ca);
            c.i1 = static_cast<size_t>(cb);
            c.windowed = true;
        }
    }
    const size_t count = c.i1 - c.i0 + 1;
    const size_t budget = SPECTRUM_VIEW_MAX_POINTS;
    const size_t want = std::clamp<size_t>(points ? points : SPECTRUM_LEGACY_POINTS, 16, budget);
    c.pool = std::max({size_t(1), (count + budget - 1) / budget, (count + want / 2) / want});
    c.n = (count + c.pool - 1) / c.pool;
    return c;
}
}  // namespace

namespace {
// (g_mu) one type 10 frame of a cut; overview = flag 16 (the whole range for the
// waterfall's history of a zoomed page, sent before its flag-2 frame)
std::string frame_locked(const Cut& c, uint32_t seq, bool completes, bool overview) {
    const Plan& p = S.plan;
    std::string out;
    out.resize(64 + c.n);
    char* o = out.data();
    auto put = [&o](const auto& x) { std::memcpy(o, &x, sizeof x); o += sizeof x; };
    put(uint32_t(10));
    put(static_cast<uint8_t>((c.windowed ? 1 : 0) | (completes ? 2 : 0) | (g_run.load() ? 0 : 4) | (S.any ? 0 : 8) |
                             (overview ? 16 : 0)));
    put(static_cast<uint8_t>(p.Kp));
    put(uint16_t(0));
    put(seq);
    put(S.done);
    put(static_cast<double>(p.start));
    put(static_cast<double>(p.stop));
    put(static_cast<double>(p.start) + (static_cast<double>(c.i0) + (static_cast<double>(c.pool) - 1.0) / 2.0) * p.bin_hz);
    put(p.bin_hz * static_cast<double>(c.pool));
    put(completes ? 1.0f : g_progress.load(std::memory_order_relaxed));
    put(S.last_s);
    put(p.grid);
    put(static_cast<uint32_t>(c.n));
    // 0.01 dB -> 0..255 = 0..120 dB (as compress_magnitudes / type 7)
    constexpr float SCALE = 0.01f * 255.0f / 120.0f;
    for (size_t j = 0; j < c.n; j++) {
        const size_t a = c.i0 + j * c.pool, b = std::min(a + c.pool, c.i1 + 1);
        const int16_t m = range_max(a, b);
        const int q = m == NONE ? 0 : static_cast<int>(std::lround(static_cast<float>(m) * SCALE));
        *o++ = static_cast<char>(static_cast<uint8_t>(std::clamp(q, 0, 255)));
    }
    return out;
}
}  // namespace

std::string build_frame(const View& v, bool completes, uint32_t overview_points, std::string* overview) {
    if (overview) overview->clear();
    std::lock_guard<std::mutex> lk(g_mu);
    const Plan& p = S.plan;
    if (!p.ok || S.lv.empty()) return std::string();
    const Cut c = cut_for(v.lo_hz, v.hi_hz, v.points);
    if (c.n == 0) return std::string();
    // the overview: only with a completed sweep, only for a zoomed view (a whole-range
    // view's frame is one), cut as a whole-range view of that many points, under the
    // same lock as the frame (the same state of the sweep)
    if (overview && overview_points && completes && c.windowed) {
        const Cut w = cut_for(0, 0, overview_points);
        if (w.n) *overview = frame_locked(w, v.seq, true, true);
    }
    return frame_locked(c, v.seq, completes, false);
}

// int16 store value (0.01 dB) -> type 10's u8 level, exactly as build_frame
// rounds it (lround of m x SCALE, clamped; NONE -> 0): for x >= 0 below 2^22,
// int(x + 0.5f) is lround(x) (0.5 is a whole number of x's ulps, the sum
// exact), negative values and NONE go to 0 - checked for all 65 536 values
// (bench/conv.cpp). A plain loop the compiler vectorizes: 2.4 ms for 11.9 M
// bins on x86 against 5.3 for a table
inline uint8_t level_u8(int16_t m) {
    constexpr float SCALE = 0.01f * 255.0f / 120.0f;
    const float x = std::max(static_cast<float>(m) * SCALE, 0.0f) + 0.5f;
    return static_cast<uint8_t>(std::min(static_cast<int>(x), 255));
}

uint32_t full_pool(uint32_t n) {
    std::lock_guard<std::mutex> lk(g_mu);
    return pool_for(S.plan.ok ? S.plan.bins : 0, n);
}

std::vector<std::shared_ptr<const FullSet>> build_fulls(const std::vector<uint32_t>& pools) {
    std::vector<std::shared_ptr<const FullSet>> done;
    std::vector<std::shared_ptr<FullSet>> out;
    std::vector<int> levels;
    // (the buffers first, outside the lock; the bins can't change size meanwhile:
    // only the loop thread - this one - makes a new grid). Level l of the max
    // pyramid holds ceil(bins / 2^l) values, value i = the max over bins
    // [i 2^l, (i + 1) 2^l): the pooled set, nothing pooled anew
    const int64_t bins = std::max<int64_t>(S.plan.bins, 0);
    for (uint32_t pool : pools) {
        pool = std::max<uint32_t>(pool, 1);
        const int level = std::countr_zero(pool);
        if ((1u << level) != pool || pool > 65535) continue;   // (a power of two the header's u16 carries)
        bool dup = false;
        for (const auto& o : out) dup = dup || o->pool == pool;
        if (dup) continue;
        auto f = std::make_shared<FullSet>();
        f->pool = static_cast<uint16_t>(pool);
        f->lv.resize(static_cast<size_t>((bins + pool - 1) / pool));
        out.push_back(f);
        levels.push_back(level);
    }
    if (out.empty()) return done;
    const auto t0 = std::chrono::steady_clock::now();
    {
        // every pool from ONE state of the store: the sets of a tick are the same
        // sweep whatever their pool (a pooled set = the full set max-pooled)
        std::lock_guard<std::mutex> lk(g_mu);
        const Plan& p = S.plan;
        if (!p.ok || S.lv.empty() || !S.any || S.lv[0].size() != static_cast<size_t>(bins)) return done;
        for (size_t k = 0; k < out.size(); k++) {
            FullSet& f = *out[k];
            const size_t level = static_cast<size_t>(levels[k]);
            if (S.lv.size() <= level || S.lv[level].size() != f.lv.size()) continue;
            f.grid = p.grid;
            f.sweeps = S.done;
            // the centre of the first group (as type 10's point 0), the groups' spacing
            f.f0_hz = static_cast<double>(p.start) + (static_cast<double>(f.pool) - 1.0) / 2.0 * p.bin_hz;
            f.step_hz = p.bin_hz * static_cast<double>(f.pool);
            const int16_t* src = S.lv[level].data();
            uint8_t* dst = f.lv.data();
            const size_t n = f.lv.size();
            for (size_t i = 0; i < n; i++) dst[i] = level_u8(src[i]);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (auto& f : out)
        if (f->grid) {
            f->lock_ms = ms;
            done.push_back(f);
        }
    return done;
}

std::shared_ptr<const FullSet> build_full(uint32_t pool) {
    auto v = build_fulls({pool});
    return v.empty() ? nullptr : v[0];
}

std::string full_part(const FullSet& s, uint8_t flags, uint32_t set_id, size_t first, size_t n, const std::string* z) {
    if (z) flags = static_cast<uint8_t>(flags | 8);
    std::string out;
    out.resize(48 + (z ? z->size() : n));
    char* o = out.data();
    auto put = [&o](const auto& x) { std::memcpy(o, &x, sizeof x); o += sizeof x; };
    put(uint32_t(11));
    put(flags);
    put(uint8_t(0));
    put(s.pool);
    put(s.grid);
    put(s.sweeps);
    put(s.f0_hz);
    put(s.step_hz);
    put(static_cast<uint32_t>(s.lv.size()));
    put(static_cast<uint32_t>(first));
    put(static_cast<uint32_t>(n));
    put(set_id);
    if (z)
        std::memcpy(o, z->data(), z->size());
    else
        std::memcpy(o, s.lv.data() + first, n);
    return out;
}

uint32_t grid_id() {
    std::lock_guard<std::mutex> lk(g_mu);
    return S.plan.ok ? S.plan.grid : 0;
}

bool pooled_db(double lo_hz, double hi_hz, int points, std::vector<float>& db, double* first_hz, double* step_hz,
               double* bin_hz) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Plan& p = S.plan;
    if (!p.ok || S.lv.empty() || !S.any) return false;
    const Cut c = cut_for(lo_hz, hi_hz, static_cast<uint32_t>(std::max(points, 16)));
    db.resize(c.n);
    for (size_t j = 0; j < c.n; j++) {
        const size_t a = c.i0 + j * c.pool, b = std::min(a + c.pool, c.i1 + 1);
        const int16_t m = range_max(a, b);
        db[j] = m == NONE ? NAN : static_cast<float>(m) * 0.01f;
    }
    *first_hz = static_cast<double>(p.start) + (static_cast<double>(c.i0) + (static_cast<double>(c.pool) - 1.0) / 2.0) * p.bin_hz;
    *step_hz = p.bin_hz * static_cast<double>(c.pool);
    *bin_hz = p.bin_hz;
    return true;
}

// --- the compressor (SWEEP_FULL_Z) ------------------------------------------------------
namespace {
constexpr double Z_MIN_RATE_BYTES_MS = 32000.0;   // 32 MB/s: a part's expected compression
constexpr int64_t Z_TICK_MS = 50;
std::mutex g_zmu;
std::condition_variable g_zcv;
std::vector<std::shared_ptr<const FullSet>> g_zjob;   // the latest hand-over
uint64_t g_zgen = 0;
bool g_zstop = false;
std::thread g_zthread;
std::function<void()> g_zwake;
int g_ztest_delay_ms = -1;   // KRAKEN_DOA_TEST_Z_DELAY_MS: a test build's slow compressor

void z_main() {
    z_stream zs{};
    if (deflateInit2(&zs, 1, Z_DEFLATED, 15, 9, Z_HUFFMAN_ONLY) != Z_OK) return;
    std::string out;
    for (;;) {
        std::vector<std::shared_ptr<const FullSet>> job;
        uint64_t gen;
        std::function<void()> wake;
        {
            std::unique_lock<std::mutex> lk(g_zmu);
            g_zcv.wait(lk, [] { return g_zstop || !g_zjob.empty(); });
            if (g_zstop) break;
            job.swap(g_zjob);
            gen = g_zgen;
            wake = g_zwake;
        }
        bool replaced = false;
        for (const auto& s : job) {
            const size_t parts = s->zpart.size();
            for (size_t k = 0; k < parts; k++) {
                {
                    std::lock_guard<std::mutex> lk(g_zmu);
                    replaced = g_zstop || g_zgen != gen;
                }
                if (replaced) break;
                if (s->zstate[k].load(std::memory_order_acquire) != Z_PENDING) continue;
                const size_t first = k * FULL_PART_VALUES;
                const size_t n = std::min(FULL_PART_VALUES, s->lv.size() - first);
                out.resize(deflateBound(&zs, static_cast<uLong>(n)));
                deflateReset(&zs);
                zs.next_in = const_cast<Bytef*>(s->lv.data() + first);
                zs.avail_in = static_cast<uInt>(n);
                zs.next_out = reinterpret_cast<Bytef*>(out.data());
                zs.avail_out = static_cast<uInt>(out.size());
                const int r = deflate(&zs, Z_FINISH);
                const size_t used = out.size() - zs.avail_out;
                if (g_ztest_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(g_ztest_delay_ms));
                if (r == Z_STREAM_END && used < n) {
                    s->zpart[k].assign(out.data(), used);
                    s->zstate[k].store(Z_READY, std::memory_order_release);
                } else {
                    s->zstate[k].store(Z_RAW, std::memory_order_release);   // not smaller: raw
                }
                if (wake) wake();
            }
            if (replaced) break;
        }
        if (replaced)
            for (const auto& s : job) s->z_dropped.store(true, std::memory_order_release);
        // (the job's references go here: a set lives as long as a socket still sends it)
    }
    deflateEnd(&zs);
}
}  // namespace

int64_t z_now_ms() { return now_ms(); }

int64_t z_deadline_ms(const FullSet& s, size_t k) {
    const double bytes = static_cast<double>(s.z_before + std::min(s.lv.size(), (k + 1) * FULL_PART_VALUES));
    return s.z_t0_ms + Z_TICK_MS + static_cast<int64_t>(bytes / Z_MIN_RATE_BYTES_MS);
}

void z_set_wake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lk(g_zmu);
    g_zwake = std::move(wake);
}

void z_submit(const std::vector<std::shared_ptr<const FullSet>>& sets_in) {
    // smallest first: a page at a step is not held behind a page at Full (a
    // step is at most a quarter of it - a few ms), and each set's deadline
    // counts the bytes ahead of it
    std::vector<std::shared_ptr<const FullSet>> sets(sets_in);
    std::stable_sort(sets.begin(), sets.end(), [](const auto& a, const auto& b) { return a->lv.size() < b->lv.size(); });
    // (loop thread) the parts' slots first: their size is fixed before the
    // compressor sees the set
    const int64_t t0 = now_ms();
    size_t before = 0;
    for (const auto& s : sets) {
        if (s->zstate) continue;   // (already handed over once: a set is compressed once)
        const size_t parts = (s->lv.size() + FULL_PART_VALUES - 1) / FULL_PART_VALUES;
        s->zpart.resize(parts);
        s->zstate.reset(new std::atomic<uint8_t>[parts]);
        for (size_t k = 0; k < parts; k++) s->zstate[k].store(Z_PENDING, std::memory_order_relaxed);
        s->z_t0_ms = t0;
        s->z_before = before;
        before += s->lv.size();
    }
    std::lock_guard<std::mutex> lk(g_zmu);
    if (!g_zthread.joinable() && !g_zstop) {
        if (const char* d = std::getenv("KRAKEN_DOA_TEST_Z_DELAY_MS")) g_ztest_delay_ms = std::atoi(d);
        g_zthread = std::thread(z_main);
    }
    g_zjob = sets;   // replaces what the compressor has not started
    g_zgen++;
    g_zcv.notify_one();
}

void start() {
    apply_plan(false);
    g_stop.store(false);
    g_thread = std::thread(worker_main);
}

void stop() {
    g_stop.store(true);
    g_rcv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_zmu);
        g_zstop = true;
        g_zjob.clear();
    }
    g_zcv.notify_all();
    if (g_zthread.joinable()) g_zthread.join();
}

}  // namespace sweep
