// The spectrum's noise floor per tuner channel, with the receiver's own floor
// remembered - see noise_floor.hpp.
#include "signal_processing/noise_floor.hpp"
#include "playback.hpp"

#include "channel_manager.hpp"
#include "config.hpp"
#include "doa_logger.hpp"      // doa_is_calibrating
#include "geo_address.hpp"     // geo::json_parse
#include "globals.hpp"         // multi_tuner_mode
#include "utils/fs_compat.hpp"
#include "utils/parse_num.hpp" // is_finite_value (-Ofast folds std::isfinite)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

namespace nfloor {
namespace {

// Without a coherence verdict: the band's floor this far above the remembered
// one = a signal fills the band; back under LEAVE_DB = noise again
constexpr float ENTER_DB = 6.0f;
constexpr float LEAVE_DB = 4.0f;
// Coherent, the quiet bins correlated: measured from the remembered floor once
// the band's is this far above it
constexpr float HELD_MIN_DB = 1.0f;
// The remembered floor follows a higher floor of noise at this rate, a held
// one with no verdict 1 dB a minute, a lower one within ~0.5 s
constexpr float RISE_DB_S = 1.0f;
constexpr float RISE_HELD_DB_S = 1.0f / 60.0f;
constexpr float FALL_TAU_S = 0.5f;
constexpr int64_t SETTLE_MS = 600;      // nothing learned this long after a retune
constexpr int64_t STALE_MS = 1500;      // floor_db() without a frame this recent: the caller measures
// Worked out at most this often per channel (and on every retune): a 10th
// percentile of 13k bins is ~0.1-0.2 ms on a Pi 5 and frames come ~146 / s
constexpr int64_t EVAL_MS = 50;
// Coherence test: at most every MSC_PERIOD_MS, a verdict counts MSC_FRESH_MS
// (and only after the last retune). Mean magnitude-squared coherence of the
// quietest bins >= MSC_SIGNAL = a signal (noise: ~1 / segments, 0.016 for a
// 16384-sample packet; a signal at SNR s: (s / (1 + s))^2, 0.2 at -1 dB)
constexpr int64_t MSC_PERIOD_MS = 250;
constexpr int64_t MSC_FRESH_MS = 2000;
constexpr float MSC_SIGNAL = 0.2f;
constexpr int MSC_LEN = 256;            // Welch segment: 9.4 kHz bins at 2.4 MHz
constexpr int MSC_MIN_SEGS = 16;
// The file at most every 5 min (SD card), when a floor moved this much
constexpr int64_t SAVE_EVERY_MS = 300000;
constexpr float SAVE_DELTA_DB = 0.5f;
const char* const FILE_NAME = "noise_floors.json";

// ---- Wideband scan mode (tuners side by side, one stitched spectrum) ----
// Every tuner's response across its band is the same - the RTL2832U's and
// R820T's filters: a tilt of ~3.5 dB, a slow roll-off below -1 MHz, a steep
// one above +1 MHz: one common SHAPE (dB over the band's floor at WB_SHAPE_N
// points across the sample rate, DC centred) flattens them all. Measured on a
// KrakenSDR (2026-10-20: 5 x R820T, 2.4 MS/s, the median of three quiet
// tuners), learned on from quiet tuners
constexpr int WB_SHAPE_N = 256;
const float WB_SHAPE_DEFAULT[WB_SHAPE_N] = {
    -7.02f, -6.80f, -6.53f, -6.23f, -5.91f, -5.60f, -5.32f, -5.04f,
    -4.81f, -4.58f, -4.32f, -4.10f, -3.93f, -3.70f, -3.44f, -3.21f,
    -2.99f, -2.76f, -2.53f, -2.36f, -2.19f, -2.02f, -1.89f, -1.72f,
    -1.54f, -1.39f, -1.26f, -1.11f, -1.00f, -0.91f, -0.80f, -0.72f,
    -0.59f, -0.48f, -0.37f, -0.31f, -0.25f, -0.21f, -0.18f, -0.14f,
    -0.07f, -0.01f, 0.02f, 0.08f, 0.12f, 0.14f, 0.15f, 0.19f,
    0.17f, 0.14f, 0.15f, 0.16f, 0.14f, 0.15f, 0.16f, 0.14f,
    0.11f, 0.12f, 0.15f, 0.12f, 0.13f, 0.14f, 0.12f, 0.09f,
    0.09f, 0.08f, 0.02f, 0.00f, -0.00f, 0.09f, 0.16f, 0.22f,
    0.21f, 0.20f, 0.08f, -0.04f, -0.12f, -0.14f, -0.18f, -0.20f,
    -0.21f, -0.20f, -0.18f, -0.15f, -0.10f, -0.07f, -0.06f, -0.06f,
    -0.05f, -0.06f, -0.06f, -0.06f, -0.07f, -0.09f, -0.08f, -0.02f,
    0.07f, 0.10f, 0.14f, 0.20f, 0.19f, 0.18f, 0.21f, 0.25f,
    0.28f, 0.33f, 0.39f, 0.45f, 0.46f, 0.47f, 0.49f, 0.50f,
    0.49f, 0.53f, 0.61f, 0.65f, 0.69f, 0.75f, 0.77f, 0.68f,
    0.67f, 0.69f, 0.68f, 0.65f, 0.68f, 0.67f, 0.61f, 0.55f,
    0.55f, 0.57f, 0.60f, 0.62f, 0.66f, 0.72f, 0.74f, 0.71f,
    0.71f, 0.74f, 0.70f, 0.62f, 0.60f, 0.59f, 0.54f, 0.49f,
    0.51f, 0.50f, 0.46f, 0.44f, 0.45f, 0.42f, 0.42f, 0.44f,
    0.43f, 0.40f, 0.36f, 0.34f, 0.31f, 0.28f, 0.25f, 0.23f,
    0.21f, 0.19f, 0.20f, 0.22f, 0.22f, 0.21f, 0.17f, 0.14f,
    0.11f, 0.11f, 0.12f, 0.16f, 0.20f, 0.20f, 0.20f, 0.23f,
    0.24f, 0.27f, 0.41f, 0.55f, 0.57f, 0.56f, 0.58f, 0.53f,
    0.47f, 0.49f, 0.54f, 0.55f, 0.54f, 0.55f, 0.56f, 0.59f,
    0.63f, 0.65f, 0.66f, 0.72f, 0.79f, 0.88f, 0.98f, 1.07f,
    1.13f, 1.19f, 1.22f, 1.21f, 1.22f, 1.28f, 1.28f, 1.28f,
    1.30f, 1.31f, 1.27f, 1.32f, 1.43f, 1.50f, 1.53f, 1.58f,
    1.58f, 1.50f, 1.49f, 1.53f, 1.53f, 1.53f, 1.52f, 1.51f,
    1.45f, 1.39f, 1.33f, 1.28f, 1.18f, 1.07f, 0.94f, 0.73f,
    0.49f, 0.19f, -0.16f, -0.57f, -1.07f, -1.65f, -2.32f, -3.01f,
    -3.75f, -4.49f, -5.26f, -5.97f, -6.66f, -7.23f, -7.71f, -8.08f,
    -8.34f, -8.50f, -8.58f, -8.54f, -8.30f, -8.04f, -7.77f, -7.55f,
};
// Neighbouring tuners overlap (each 2.4 MHz wide, spaced by edge clip x 2.4
// MHz): at the same frequencies their flattened spectra differ only by their
// gains, whatever the band holds - noise, carriers, DVB-T alike. The median of
// that difference over the bins where both responses are above
// WB_OVERLAP_MIN_DB (>= WB_OVERLAP_MIN_BINS of them; median: a strong signal
// just past one tuner's band aliases into that one only), smoothed over
// WB_STEP_TAU_S, links the tuners' levels
constexpr float WB_OVERLAP_MIN_DB = -6.0f;
constexpr int WB_OVERLAP_MIN_BINS = 64;
constexpr float WB_STEP_TAU_S = 1.0f;
// Along tuners linked so, the common floor is the quietest one's (gains taken
// off); a tuner standing WB_FILL_DB over it (left under WB_UNFILL_DB) is
// FILLED by a signal - its own percentile is the signal - and takes the
// common floor + its gain. The others keep their own floor: their noise lines
// up exactly. A raised floor in one tuner (overload, its antenna) is in the
// overlaps too: a gain difference, not a signal
constexpr float WB_FILL_DB = 3.0f;
constexpr float WB_UNFILL_DB = 2.0f;
constexpr int64_t WB_FRESH_MS = 400;        // a tuner's figures count this long
// The shape learned from quiet tuners (not filled, the flattened band's 50th
// - 10th percentile within WB_QUIET_MARGIN_DB of noise's - noise_spread_db)
// at most every WB_SHAPE_EVERY_MS, each point within WB_SHAPE_GATE_DB of it
// only (a signal is not the response), over WB_SHAPE_TAU_S; FFTs of
// WB_SHAPE_MIN_FFT bins and up
constexpr float WB_QUIET_MARGIN_DB = 1.5f;
constexpr int64_t WB_SHAPE_EVERY_MS = 1000;
constexpr float WB_SHAPE_GATE_DB = 3.0f;
constexpr float WB_SHAPE_TAU_S = 60.0f;
constexpr int WB_SHAPE_MIN_FFT = 4096;
// After a retune the averaged spectra hold both bands for a while: gain steps,
// decisions and the shape wait 5 averaging time constants (frame interval /
// alpha), WB_SETTLE_MIN_MS .. WB_SETTLE_MAX_MS. A move over WB_JUMP_HZ drops
// the decisions (another band)
constexpr int64_t WB_SETTLE_MIN_MS = 300;
constexpr int64_t WB_SETTLE_MAX_MS = 4000;
constexpr double WB_MOVE_HZ = 1000.0;
constexpr double WB_JUMP_HZ = 1e6;

struct Wb {
    bool ok = false;        // worked out (wideband scan mode)
    int64_t ms = 0;
    double fc = 0.0;        // the tuner's centre, Hz
    float f = 0.0f;         // the flattened band's floor (10th percentile of the centre 80 %)
    float spread = 0.0f;    // its 50th - 10th percentile
    int up = -1;            // the tuner next up whose band overlaps this one's
    float step = 0.0f;      // its flattened level minus this one's at the same frequencies
    bool step_ok = false;
    int64_t step_ms = 0;
    bool filled = false;
    int64_t settle_until = 0;
    double settle_fc = 0.0; // the centre before the retune that started the settling
    int64_t shape_ms = 0;
};

struct Chan {
    bool held = false;      // the band's floor is a signal: measured from the remembered one
    int64_t last_ms = 0;    // last frame
    int64_t reset_ms = 0;   // last retune
    bool msc_ok = false;    // coherence verdict (since the last retune)
    float msc = 0.0f;
    int64_t msc_ms = 0;
};

std::mutex mu;
Chan chans[MAX_CHANNELS];
// "ch|gain dB" -> the remembered floor: the 10th percentile of a frame's dB
// bins + 10 log10(fft size) (a bin's share of the noise halves with each
// doubling of the FFT; a frame's percentile doesn't depend on the averaging)
std::map<std::string, float> refs;
std::map<std::string, float> saved;    // what the file holds
int64_t last_save_ms = 0;
std::mutex save_mu;                    // one writer at a time

std::atomic<float> out_db[MAX_CHANNELS];
std::atomic<int64_t> out_ms[MAX_CHANNELS];      // when out_db was worked out
std::atomic<int64_t> frame_ms[MAX_CHANNELS];    // the channel's latest frame (staleness)
std::atomic<float> frame_dt_ms[MAX_CHANNELS];   // its frame interval (smoothed)
std::atomic<int64_t> last_msc_ms{0};

// Wideband scan mode, under mu
Wb wbs[MAX_CHANNELS];
float shape[WB_SHAPE_N];
bool shape_ready = false;
uint32_t shape_ver = 1;
std::vector<float> shape_bins;   // the shape for each bin of a shape_bins_n spectrum
int shape_bins_n = 0;
uint32_t shape_bins_ver = 0;

#ifdef NFLOOR_TEST_CLOCK
}  // namespace
int64_t test_now_ms = 1;   // tests step the clock (minutes in no time)
namespace {
int64_t now_ms() { return test_now_ms; }
#else
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
#endif

// The estimate FFTProcessor::calculate_noise_floor makes: the 10th percentile
// of the centre 80 % of the bins (the tuner's filter skirts left out)
float p10(const std::vector<float>& v, int n) {
    const int usable = static_cast<int>(n * 0.8f);
    const int skip = (n - usable) / 2;
    thread_local std::vector<float> m;
    m.assign(v.begin() + skip, v.begin() + skip + usable);
    const int idx = static_cast<int>(m.size() * 0.10f);
    std::nth_element(m.begin(), m.begin() + idx, m.end());
    return m[idx];
}

std::string key_of(int ch, float gain_db) {
    char b[32];
    snprintf(b, sizeof b, "%d|%.1f", ch, gain_db);
    return b;
}

// The gain part of a key ("|49.6")
std::string gain_part(const std::string& key) {
    const size_t p = key.find('|');
    return p == std::string::npos ? "" : key.substr(p);
}

void follow(float& ref, float d, float dt, float rise_db_s) {
    if (d < 0) ref += d * std::min(1.0f, dt / FALL_TAU_S);
    else ref += std::min(d, rise_db_s * dt);
}

// ---- wideband scan mode (everything under mu) ----

// The shape for each bin of an n-bin spectrum (bin i at (i - n/2) / n of the
// sample rate; grid point c at (c + 0.5) / WB_SHAPE_N - 0.5), linear between
const std::vector<float>& shape_for(int n) {
    if (!shape_ready) {
        std::copy(WB_SHAPE_DEFAULT, WB_SHAPE_DEFAULT + WB_SHAPE_N, shape);
        shape_ready = true;
    }
    if (shape_bins_n != n || shape_bins_ver != shape_ver) {
        shape_bins.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; i++) {
            const float u = std::clamp(static_cast<float>(i) * WB_SHAPE_N / n - 0.5f, 0.0f,
                                       static_cast<float>(WB_SHAPE_N - 1));
            const int a = std::min(static_cast<int>(u), WB_SHAPE_N - 2);
            shape_bins[static_cast<size_t>(i)] = shape[a] + (u - a) * (shape[a + 1] - shape[a]);
        }
        shape_bins_n = n;
        shape_bins_ver = shape_ver;
    }
    return shape_bins;
}

// The 50th - 10th percentile of an averaged spectrum of noise: a bin's dB of
// one frame has a 5.57 dB sd (8.2 dB between those percentiles), the
// spectrum's EMA a factor sqrt(alpha / (2 - alpha)) of that; the ratio 1.4
// fits the skew left at the usual alphas (0.15: 2.27 dB measured)
float noise_spread_db() {
    const float a = std::clamp(averaging_alpha.load(std::memory_order_relaxed), 0.01f, 1.0f);
    return std::min(8.2f, 1.4f * 5.57f * std::sqrt(a / (2.0f - a)));
}

// How long a tuner's averaged spectrum takes to forget its old band
int64_t settle_ms(int ch) {
    float dt = frame_dt_ms[ch].load(std::memory_order_relaxed);
    if (!(dt > 0.0f)) dt = 7.0f;
    const float alpha = std::max(averaging_alpha.load(std::memory_order_relaxed), 0.01f);
    return std::clamp(static_cast<int64_t>(5.0f * dt / alpha), WB_SETTLE_MIN_MS, WB_SETTLE_MAX_MS);
}

// The gain step from tuner ch (its averaged spectrum avg, flattened by h) to
// the tuner next up whose band overlaps: the median over the shared bins of
// the difference of the two flattened spectra. The neighbour's spectrum is
// fft_averaged[up] - on_frame runs under fft_mutex
void update_step(int ch, Wb& w, const std::vector<float>& avg, int n, const std::vector<float>& h,
                 int64_t now, bool settling) {
    const double sr = SAMPLE_RATE;
    int up = -1;
    double fu = 0.0;
    for (int j = 0; j < MAX_CHANNELS; j++) {
        const int64_t fm = frame_ms[j].load(std::memory_order_acquire);
        if (j == ch || fm == 0 || now - fm > WB_FRESH_MS) continue;
        const double f = static_cast<double>(tuner_frequencies[j].load(std::memory_order_relaxed));
        if (f - w.fc > WB_MOVE_HZ && f - w.fc < sr && (up < 0 || f < fu)) {
            up = j;
            fu = f;
        }
    }
    if (up != w.up) {
        w.up = up;
        w.step_ok = false;
    }
    if (up < 0 || settling || now < wbs[up].settle_until) return;
    if (up >= static_cast<int>(fft_averaged.size()) || fft_averaged[up].size() != static_cast<size_t>(n)) return;
    const std::vector<float>& b = fft_averaged[up];
    const double shift = (fu - w.fc) / (sr / n);   // bin i here = bin i - shift there
    thread_local std::vector<float> diff;
    diff.clear();
    for (int i = std::max(0, static_cast<int>(std::ceil(shift - 0.5))); i < n; i++) {
        const long long j = std::llround(i - shift);
        if (j < 0 || j >= n) continue;
        const size_t ju = static_cast<size_t>(j), iu = static_cast<size_t>(i);
        if (h[iu] <= WB_OVERLAP_MIN_DB || h[ju] <= WB_OVERLAP_MIN_DB) continue;
        diff.push_back((b[ju] - h[ju]) - (avg[iu] - h[iu]));
    }
    if (static_cast<int>(diff.size()) < WB_OVERLAP_MIN_BINS) return;
    const size_t mid = diff.size() / 2;
    std::nth_element(diff.begin(), diff.begin() + static_cast<std::ptrdiff_t>(mid), diff.end());
    const float s = diff[mid];
    if (!is_finite_value(s)) return;
    if (!w.step_ok) {
        w.step = s;
        w.step_ok = true;
    } else {
        const float dt = std::clamp((now - w.step_ms) / 1000.0f, 0.0f, 1.0f);
        w.step += (s - w.step) * std::min(1.0f, dt / WB_STEP_TAU_S);
    }
    w.step_ms = now;
}

// The shape from a quiet tuner's averaged spectrum: each grid point's 10th
// percentile over the band's (the default's definition)
void learn_shape(const std::vector<float>& avg, int n, float dt) {
    const float band = [&] {
        thread_local std::vector<float> m;
        const int usable = static_cast<int>(n * 0.8f), skip = (n - usable) / 2;
        m.assign(avg.begin() + skip, avg.begin() + skip + usable);
        const size_t idx = m.size() / 10;
        std::nth_element(m.begin(), m.begin() + static_cast<std::ptrdiff_t>(idx), m.end());
        return m[idx];
    }();
    if (!is_finite_value(band)) return;
    const int per = n / WB_SHAPE_N;
    const float k = std::min(1.0f, dt / WB_SHAPE_TAU_S);
    thread_local std::vector<float> blk;
    bool changed = false;
    for (int c = 0; c < WB_SHAPE_N; c++) {
        blk.assign(avg.begin() + c * per, avg.begin() + (c + 1) * per);
        const size_t idx = blk.size() / 10;
        std::nth_element(blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(idx), blk.end());
        const float v = blk[idx] - band;
        if (!is_finite_value(v) || std::fabs(v - shape[c]) > WB_SHAPE_GATE_DB) continue;
        const float nv = shape[c] + (v - shape[c]) * k;
        if (std::fabs(nv - shape[c]) > 1e-4f) changed = true;
        shape[c] = nv;
    }
    if (changed) shape_ver++;
}

// Every fresh tuner's floor (see WB_FILL_DB): runs of tuners linked by gain
// steps, the common floor = the quietest's. Sets out_db of the others, returns ch's
float wideband_pass(int ch, int64_t now) {
    int k[MAX_CHANNELS];
    int cnt = 0;
    for (int j = 0; j < MAX_CHANNELS; j++) {
        if (!wbs[j].ok || now - wbs[j].ms > WB_FRESH_MS) continue;
        int i = cnt++;   // by frequency (insertion: <= 8 tuners)
        for (; i > 0 && wbs[k[i - 1]].fc > wbs[j].fc; i--) k[i] = k[i - 1];
        k[i] = j;
    }
    float out = wbs[ch].f;
    float g[MAX_CHANNELS];
    for (int a = 0; a < cnt;) {
        int e = a;
        g[a] = 0.0f;
        while (e + 1 < cnt && wbs[k[e]].up == k[e + 1] && wbs[k[e]].step_ok) {
            g[e + 1] = g[e] + wbs[k[e]].step;
            e++;
        }
        float common = wbs[k[a]].f - g[a];
        for (int m = a + 1; m <= e; m++) common = std::min(common, wbs[k[m]].f - g[m]);
        for (int m = a; m <= e; m++) {
            Wb& w = wbs[k[m]];
            const float over = w.f - g[m] - common;
            if (now >= w.settle_until)
                w.filled = e > a && over > (w.filled ? WB_UNFILL_DB : WB_FILL_DB);
            const float fl = w.filled ? common + g[m] : w.f;
            if (k[m] == ch) out = fl;
            else out_db[k[m]].store(fl, std::memory_order_relaxed);
        }
        a = e + 1;
    }
    return out;
}

// Wideband scan mode: tuner ch's frame (its averaged spectrum avg) -> its floor
float wideband_frame(int ch, const std::vector<float>& avg, int n, int64_t now) {
    std::lock_guard<std::mutex> lk(mu);
    const std::vector<float>& h = shape_for(n);
    const int usable = static_cast<int>(n * 0.8f), skip = (n - usable) / 2;
    thread_local std::vector<float> flat;
    flat.resize(static_cast<size_t>(usable));
    for (int i = 0; i < usable; i++)
        flat[static_cast<size_t>(i)] = avg[static_cast<size_t>(skip + i)] - h[static_cast<size_t>(skip + i)];
    const size_t i10 = flat.size() / 10, i50 = flat.size() / 2;
    std::nth_element(flat.begin(), flat.begin() + static_cast<std::ptrdiff_t>(i10), flat.end());
    const float f = flat[i10];
    std::nth_element(flat.begin() + static_cast<std::ptrdiff_t>(i10), flat.begin() + static_cast<std::ptrdiff_t>(i50),
                     flat.end());
    const float f50 = flat[i50];
    Wb& w = wbs[ch];
    if (!is_finite_value(f) || !is_finite_value(f50)) {
        w = Wb{};
        return f;
    }
    const double fc = static_cast<double>(tuner_frequencies[ch].load(std::memory_order_relaxed));
    if (w.ok && std::fabs(fc - w.fc) > WB_MOVE_HZ) {   // retuned
        if (now >= w.settle_until) w.settle_fc = w.fc;
        w.settle_until = now + settle_ms(ch);
        if (std::fabs(fc - w.settle_fc) > WB_JUMP_HZ) w.filled = false;
    }
    const bool settling = now < w.settle_until;
    w.ok = true;
    w.ms = now;
    w.fc = fc;
    w.f = f;
    w.spread = f50 - f;
    update_step(ch, w, avg, n, h, now, settling);
    if (!settling && !w.filled && w.spread < noise_spread_db() + WB_QUIET_MARGIN_DB && n >= WB_SHAPE_MIN_FFT &&
        now - w.shape_ms >= WB_SHAPE_EVERY_MS) {
        learn_shape(avg, n, std::clamp((now - w.shape_ms) / 1000.0f, 0.0f, 5.0f));
        w.shape_ms = now;
    }
    return wideband_pass(ch, now);
}

// Radix-2 FFT of MSC_LEN points with a Hann window (the coherence test only)
struct SegFft {
    std::complex<float> tw[MSC_LEN / 2];
    int rev[MSC_LEN];
    float win[MSC_LEN];
    SegFft() {
        int bits = 0;
        while ((1 << bits) < MSC_LEN) bits++;
        for (int i = 0; i < MSC_LEN; i++) {
            int r = 0;
            for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[i] = r;
            win[i] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / MSC_LEN);
        }
        for (int i = 0; i < MSC_LEN / 2; i++)
            tw[i] = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * i / MSC_LEN);
    }
    // x: MSC_LEN input samples -> out: the windowed spectrum
    void run(const std::complex<float>* x, std::complex<float>* out) const {
        for (int i = 0; i < MSC_LEN; i++) out[rev[i]] = x[i] * win[i];
        for (int len = 2; len <= MSC_LEN; len <<= 1) {
            const int half = len / 2, step = MSC_LEN / len;
            for (int s = 0; s < MSC_LEN; s += len)
                for (int j = 0; j < half; j++) {
                    const std::complex<float> t = out[s + j + half] * tw[j * step];
                    out[s + j + half] = out[s + j] - t;
                    out[s + j] += t;
                }
        }
    }
};

}  // namespace

void on_frame(int ch, const std::vector<float>& raw, const std::vector<float>& avg,
              int fft_size, bool reset) {
    if (ch < 0 || ch >= MAX_CHANNELS || fft_size < 64 ||
        raw.size() < static_cast<size_t>(fft_size) || avg.size() < static_cast<size_t>(fft_size)) return;
    {
        const int64_t t = now_ms();
        const int64_t prev = frame_ms[ch].exchange(t, std::memory_order_acq_rel);
        if (prev > 0 && t > prev && t - prev < 1000) {
            const float d = static_cast<float>(t - prev), old = frame_dt_ms[ch].load(std::memory_order_relaxed);
            frame_dt_ms[ch].store(old > 0.0f ? old + 0.05f * (d - old) : d, std::memory_order_relaxed);
        }
        if (!reset && t - out_ms[ch].load(std::memory_order_relaxed) < EVAL_MS) return;
    }
    if (wideband_mode_enabled.load(std::memory_order_relaxed)) {
        // Tuners side by side: their floors from each other (see WB_FILL_DB);
        // the remembered floors aren't used - nor learned - here
        const int64_t now = now_ms();
        const float out = wideband_frame(ch, avg, fft_size, now);
        out_db[ch].store(out, std::memory_order_relaxed);
        out_ms[ch].store(now, std::memory_order_release);
        return;
    }
    const float f_avg = p10(avg, fft_size);   // today's floor of the averaged spectrum
    const float r = p10(raw, fft_size) + 10.0f * std::log10(static_cast<float>(fft_size));
    const float gain = ChannelManager::get_gain(ch);
    const bool cal = doa_is_calibrating();
    const bool coherent = !multi_tuner_mode();
    const int64_t now = now_ms();
    float out = f_avg;
    {
        std::lock_guard<std::mutex> lk(mu);
        Chan& c = chans[ch];
        if (reset) {
            c.reset_ms = now;
            c.msc_ok = false;   // the old band's verdict
        }
        const float dt = std::clamp((now - c.last_ms) / 1000.0f, 0.0f, 1.0f);
        c.last_ms = now;
        // Tuner AGC: the floor moves with what the band holds. Calibration:
        // the noise source fills every band. Both measured per frame as before.
        if (gain < 0.0f || cal || !is_finite_value(r) || !is_finite_value(f_avg)) {
            c.held = false;
        } else {
            const bool learn = now - c.reset_ms >= SETTLE_MS;
            const bool verdict = coherent && c.msc_ok && c.msc_ms > c.reset_ms && now - c.msc_ms < MSC_FRESH_MS;
            const bool signal = verdict && c.msc >= MSC_SIGNAL;
            const std::string key = key_of(ch, gain);
            auto it = refs.find(key);
            if (it == refs.end()) {
                // A first value: the other tuners' at this gain (they differ by
                // a dB or two), else this frame's once it is known to be noise
                float sum = 0.0f;
                int cnt = 0;
                const std::string g = gain_part(key);
                for (const auto& [k, v] : refs)
                    if (gain_part(k) == g) { sum += v; cnt++; }
                if (cnt) it = refs.emplace(key, sum / cnt).first;
                else if (learn && !signal) it = refs.emplace(key, r).first;
            }
            if (it == refs.end()) {
                c.held = false;
            } else {
                float& ref = it->second;
                const float d = r - ref;
                if (verdict) {
                    // Quiet bins correlated across the antennas: a signal fills
                    // the band, measured from the remembered floor. Uncorrelated:
                    // receiver noise, followed (a real rise - active antenna,
                    // interference - at once)
                    c.held = signal && d > HELD_MIN_DB;
                    if (learn && (!signal || d < 0.0f)) follow(ref, d, dt, RISE_DB_S);
                } else {
                    if (!c.held && d > ENTER_DB) c.held = true;
                    else if (c.held && d < LEAVE_DB) c.held = false;
                    if (learn) follow(ref, d, dt, c.held ? RISE_HELD_DB_S : RISE_DB_S);
                }
                // The averaged spectrum's percentile sits as far above its
                // noise as this frame's does (a band-filling signal fluctuates
                // like noise), so subtracting the excess leaves the noise's
                if (c.held) out = f_avg - (r - ref);
            }
        }
    }
    out_db[ch].store(out, std::memory_order_relaxed);
    out_ms[ch].store(now, std::memory_order_release);
}

bool floor_db(int ch, float* out) {
    if (ch < 0 || ch >= MAX_CHANNELS) return false;
    const int64_t f = frame_ms[ch].load(std::memory_order_acquire);
    if (out_ms[ch].load(std::memory_order_acquire) == 0 || now_ms() - f > STALE_MS) return false;
    *out = out_db[ch].load(std::memory_order_relaxed);
    return true;
}

#ifdef NFLOOR_TEST_CLOCK
void test_dump() {   // tests: the wideband state
    std::lock_guard<std::mutex> lk(mu);
    for (int c = 0; c < MAX_CHANNELS; c++) {
        const Wb& w = wbs[c];
        if (!w.ok) continue;
        printf("    wb t%d fc %.3f f %.2f spread %.2f up %d step %+.2f%s filled %d settle %lld\n", c, w.fc / 1e6, w.f,
               w.spread, w.up, w.step, w.step_ok ? "" : "(none)", w.filled, (long long)(w.settle_until - now_ms()));
    }
}
#endif

void shape_db(int fft_size, std::vector<float>& out, uint32_t* version) {
    if (fft_size < 2) return;
    std::lock_guard<std::mutex> lk(mu);
    const std::vector<float>& h = shape_for(fft_size);
    if (out.size() != h.size() || !version || *version != shape_ver) {
        out = h;
        if (version) *version = shape_ver;
    }
}

bool coherence_due() {
    return now_ms() - last_msc_ms.load(std::memory_order_relaxed) >= MSC_PERIOD_MS;
}

void measure_coherence(int a, const std::complex<float>* xa, const std::complex<float>* xb, size_t n) {
    if (a < 0 || a >= MAX_CHANNELS || !xa || !xb) return;
    const int segs = static_cast<int>(n / MSC_LEN);
    if (segs < MSC_MIN_SEGS) return;
    const int64_t now = now_ms();
    last_msc_ms.store(now, std::memory_order_relaxed);
    if (doa_is_calibrating()) return;   // the noise source is the same in every antenna

    static const SegFft fft;
    thread_local std::vector<std::complex<float>> fa(MSC_LEN), fb(MSC_LEN), s01(MSC_LEN);
    thread_local std::vector<float> s00(MSC_LEN), s11(MSC_LEN);
    std::fill(s00.begin(), s00.end(), 0.0f);
    std::fill(s11.begin(), s11.end(), 0.0f);
    std::fill(s01.begin(), s01.end(), std::complex<float>(0.0f, 0.0f));
    for (int s = 0; s < segs; s++) {
        fft.run(xa + static_cast<size_t>(s) * MSC_LEN, fa.data());
        fft.run(xb + static_cast<size_t>(s) * MSC_LEN, fb.data());
        for (int k = 0; k < MSC_LEN; k++) {
            s00[k] += std::norm(fa[k]);
            s11[k] += std::norm(fb[k]);
            s01[k] += fa[k] * std::conj(fb[k]);
        }
    }
    // The bins the floor is measured on: the centre 80 % (k in FFT order, j in
    // frequency order), without the DC bins; their quietest quarter
    thread_local std::vector<std::pair<float, float>> bins;   // power, coherence
    bins.clear();
    const int edge = MSC_LEN / 10;
    for (int k = 0; k < MSC_LEN; k++) {
        const int j = (k + MSC_LEN / 2) % MSC_LEN;
        if (j < edge || j >= MSC_LEN - edge || k <= 1 || k >= MSC_LEN - 1) continue;
        const float den = s00[k] * s11[k];
        if (!(den > 0.0f)) continue;
        bins.emplace_back(s00[k], std::norm(s01[k]) / den);
    }
    if (bins.size() < 8) return;
    const size_t q = bins.size() / 4;
    std::nth_element(bins.begin(), bins.begin() + q, bins.end());
    float m = 0.0f;
    for (size_t i = 0; i < q; i++) m += bins[i].second;
    m /= static_cast<float>(q);

    std::lock_guard<std::mutex> lk(mu);
    Chan& c = chans[a];
    c.msc = c.msc_ok ? 0.5f * (c.msc + m) : m;
    c.msc_ok = true;
    c.msc_ms = now;
}

void load() {
    std::ifstream f(FILE_NAME);
    if (!f) return;   // none yet
    std::stringstream ss;
    ss << f.rdbuf();
    geo::Json j;
    const geo::Json* fl = nullptr;
    if (geo::json_parse(ss.str(), &j)) fl = j.get("floors");
    if (!fl || fl->type != geo::Json::OBJ) {
        std::cerr << "Noise floors: " << FILE_NAME << " is unreadable - learning them again" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lk(mu);
    for (const auto& [k, v] : fl->o) {
        int ch = -1;
        float g = 0.0f;
        char tail = 0;
        if (v.type != geo::Json::NUM || !is_finite_value(v.n) || v.n < -400.0 || v.n > 200.0 ||
            sscanf(k.c_str(), "%d|%f%c", &ch, &g, &tail) != 2 || ch < 0 || ch >= MAX_CHANNELS ||
            !is_finite_value(g) || g < 0.0f || g > 100.0f || key_of(ch, g) != k) continue;
        refs[k] = static_cast<float>(v.n);
    }
    saved = refs;
    last_save_ms = now_ms();
    std::cout << "Noise floors: " << refs.size() << " remembered" << std::endl;
}

namespace {
void write_file(const std::map<std::string, float>& m) {
    const std::string tmp = std::string(FILE_NAME) + ".tmp";
    std::ofstream f(tmp);
    if (f) {
        f << "{\"version\":1,\"floors\":{";
        bool first = true;
        char num[32];
        for (const auto& [k, v] : m) {
            snprintf(num, sizeof num, "%.2f", v);
            f << (first ? "\n  \"" : ",\n  \"") << k << "\":" << num;
            first = false;
        }
        f << "\n}}\n";
        f.close();
    }
    if (!f || fs_replace(tmp.c_str(), FILE_NAME) != 0)   // Windows' rename() refuses an existing target
        std::cerr << "Noise floors: can't write " << FILE_NAME << std::endl;
}

void save(bool force) {
    if (playback::active()) return;   // debug playback: the playback's floors aren't the receiver's
    std::unique_lock<std::mutex> sl(save_mu, std::try_to_lock);
    if (!sl.owns_lock()) return;   // another thread is writing
    std::map<std::string, float> snap;
    {
        std::lock_guard<std::mutex> lk(mu);
        const int64_t now = now_ms();
        if (!force && now - last_save_ms < SAVE_EVERY_MS) return;
        bool changed = refs.size() != saved.size();
        for (const auto& [k, v] : refs) {
            auto it = saved.find(k);
            if (it == saved.end() || std::fabs(it->second - v) >= SAVE_DELTA_DB) { changed = true; break; }
        }
        last_save_ms = now;
        if (!changed && !(force && refs != saved)) return;
        snap = refs;
        saved = refs;
    }
    write_file(snap);
}
}  // namespace

void maybe_save() { save(false); }
void save_now() { save(true); }

}  // namespace nfloor
