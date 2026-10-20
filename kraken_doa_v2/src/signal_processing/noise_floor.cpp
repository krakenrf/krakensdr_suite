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
// Wideband scan mode (wideband_pass). A tuner is FILLED (takes the floor its
// neighbours hand on) when that floor lies WB_ENTER_DB under its own
// percentile (left under WB_LEAVE_DB) and its remembered floor agrees - the
// band stands WB_REF_ENTER (WB_REF_LEAVE) of that above it: a signal ending
// right at a boundary looks the same as one going on. It is QUIET (noise of
// its own - another antenna / gain than the others' -, learned at once) when
// a neighbour at its remembered floor (within WB_NOISE_REF_DB: known noise)
// hands on a floor within WB_NOISE_DB of its percentile, for WB_NOISE_HOLD_MS
// on end. A neighbour merely consistent with it isn't enough: a rise on every
// tuner (or a signal wider than all of them) is consistent everywhere, and
// right then the first tuner to see it meets the others' floors from before
// for a moment. Edge windows: WB_EDGE of the bins each, just inside a
// boundary; a tuner's edges count WB_FRESH_MS
constexpr float WB_ENTER_DB = 3.0f;
constexpr float WB_LEAVE_DB = 2.0f;
constexpr float WB_NOISE_DB = 1.5f;
constexpr float WB_NOISE_REF_DB = 3.0f;
constexpr int64_t WB_NOISE_HOLD_MS = 1000;
constexpr float WB_REF_ENTER = 0.5f;
constexpr float WB_REF_LEAVE = 0.3f;
constexpr float WB_EDGE = 0.02f;
constexpr int64_t WB_FRESH_MS = 400;
constexpr double WB_JOIN_HZ = 1000.0;   // the two sides' boundary frequencies agree to this

// Wideband scan mode: one tuner's part in the stitched spectrum's floor
struct Wb {
    bool ok = false;        // worked out (wideband scan mode, finite)
    int64_t ms = 0;
    double fc = 0.0;        // the tuner's centre, Hz
    double lo_hz = 0.0;     // its boundaries with the tuners next to it (0 = none)
    double hi_hz = 0.0;
    float lo_db = 0.0f;     // its averaged spectrum's floor just inside each
    float hi_db = 0.0f;
    float f = 0.0f;         // the band's own floor (the averaged spectrum's percentile)
    float own = 0.0f;       // the floor from this tuner alone (held or not)
    bool held = false;      // own = the remembered floor (Chan::held)
    float r = 0.0f;         // its raw floor (on_frame's r) ...
    float gain = 0.0f;      // ... at this gain
    float d = 0.0f;         // its raw floor over the remembered one
    bool has_ref = false;   // d known (no tuner AGC, a remembered floor)
    bool filled = false;    // a signal from the neighbours fills the band: their floor
    bool quiet = false;     // the neighbours' floor = its own: noise ...
    int64_t quiet_ms = 0;   // ... since
};

struct Chan {
    bool held = false;      // the band's floor is a signal: measured from the remembered one
    int64_t last_ms = 0;    // last frame
    int64_t reset_ms = 0;   // last retune
    bool msc_ok = false;    // coherence verdict (since the last retune)
    float msc = 0.0f;
    int64_t msc_ms = 0;
    Wb wb;
    int64_t wb_since = 0;   // wideband scan stats since (0 = none)
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
std::atomic<int64_t> last_msc_ms{0};

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

// The 10th percentile of v[a, b)
float p10_of(const std::vector<float>& v, int a, int b) {
    thread_local std::vector<float> m;
    m.assign(v.begin() + a, v.begin() + b);
    const int idx = static_cast<int>(m.size() * 0.10f);
    std::nth_element(m.begin(), m.begin() + idx, m.end());
    return m[idx];
}

// The estimate FFTProcessor::calculate_noise_floor makes: the 10th percentile
// of the centre 80 % of the bins (the tuner's filter skirts left out)
float p10(const std::vector<float>& v, int n) {
    const int usable = static_cast<int>(n * 0.8f);
    const int skip = (n - usable) / 2;
    return p10_of(v, skip, skip + usable);
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

// Wideband scan mode: tuner ch's boundaries with the tuners next to it - half
// way between the centres, where the stitched spectrum changes tuner - and its
// averaged spectrum's floor just inside each. Neighbours: tuners with a recent
// frame whose band overlaps this one's
Wb edges_of(int ch, const std::vector<float>& avg, int n, int64_t now) {
    Wb e;
    e.fc = static_cast<double>(tuner_frequencies[ch].load(std::memory_order_relaxed));
    const double sr = SAMPLE_RATE;
    const double bin = sr / n;
    const int w = std::max(16, static_cast<int>(n * WB_EDGE));
    double below = 0.0, above = 0.0;   // the nearest centres (0 = none)
    for (int j = 0; j < MAX_CHANNELS; j++) {
        const int64_t fm = frame_ms[j].load(std::memory_order_acquire);
        if (j == ch || fm == 0 || now - fm > WB_FRESH_MS) continue;
        const double f = static_cast<double>(tuner_frequencies[j].load(std::memory_order_relaxed));
        const double gap = std::fabs(f - e.fc);
        // both windows fit between the centres; bands that don't overlap share no edge
        if (gap < 2.5 * w * bin || gap > 0.98 * sr) continue;
        if (f < e.fc) below = std::max(below, f);
        else if (above == 0.0 || f < above) above = f;
    }
    // bin i is centred on fc + (i - n/2) bin
    if (below > 0.0) {
        e.lo_hz = 0.5 * (below + e.fc);
        const int a = std::max(0, n / 2 + static_cast<int>(std::ceil((e.lo_hz - e.fc) / bin)));
        e.lo_db = p10_of(avg, a, std::min(a + w, n / 2));
    }
    if (above > 0.0) {
        e.hi_hz = 0.5 * (above + e.fc);
        const int b = std::min(n, n / 2 + static_cast<int>(std::ceil((e.hi_hz - e.fc) / bin)));
        e.hi_db = p10_of(avg, std::max(b - w, n / 2 + 1), b);
    }
    e.ok = is_finite_value(e.lo_db) && is_finite_value(e.hi_db);
    e.ms = now;
    return e;
}

// a (below) and b share a boundary, both sides measured at it
bool joined(const Wb& a, const Wb& b) {
    return a.hi_hz > 0.0 && b.lo_hz > 0.0 && std::fabs(a.hi_hz - b.lo_hz) < WB_JOIN_HZ;
}

// Wideband scan mode, under mu, after tuner ch's Wb was updated: every tuner
// with fresh edges gets its floor. The spectrum is continuous across a
// boundary, so a tuner's noise lies as far under its edge level as its
// neighbour's noise lies under the neighbour's edge level there: neighbour's
// floor + the step between the two spectra at the shared edge (the tuners'
// gains cancel). Where that lies well under the band's own percentile, a
// signal the neighbour shows above its noise fills this band (the percentile
// is the signal) - if the band's remembered floor agrees (Wb::d, see
// WB_REF_ENTER) - and the tuner takes it; along a run of filled tuners it is
// handed on tuner by tuner. Only a floor MEASURED somewhere is handed on (a
// tuner's own percentile, or one handed to it): held tuners (the remembered
// floor) don't vouch for each other, or a rise over every tuner (an active
// antenna) would never be learned. Where the floor a neighbour at its
// remembered floor hands on equals the band's own percentile, the band is
// noise (Wb::quiet - a held tuner's remembered floor was off). Sets each
// tuner's filled / quiet state and out_db, returns ch's
float wideband_pass(int ch, int64_t now) {
    int k[MAX_CHANNELS];
    int n = 0;
    for (int j = 0; j < MAX_CHANNELS; j++) {
        if (!chans[j].wb.ok || now - chans[j].wb.ms > WB_FRESH_MS) continue;
        int i = n++;   // by frequency (insertion: <= 8 tuners)
        for (; i > 0 && chans[k[i - 1]].wb.fc > chans[j].wb.fc; i--) k[i] = k[i - 1];
        k[i] = j;
    }
    float fl[MAX_CHANNELS];
    bool fill[MAX_CHANNELS], quiet[MAX_CHANNELS];
    bool anchor[MAX_CHANNELS];   // fl[i] measured: handed on
    bool noise[MAX_CHANNELS];    // ... and at its remembered floor: known noise
    for (int i = 0; i < n; i++) {
        const Wb& e = chans[k[i]].wb;
        fl[i] = e.own;
        fill[i] = quiet[i] = false;
        anchor[i] = !e.held;
        noise[i] = !e.held && e.has_ref && std::fabs(e.d) < WB_NOISE_REF_DB;
    }
    for (int round = 0; round < n; round++) {
        bool changed = false;
        for (int i = 0; i < n; i++) {
            const Wb& e = chans[k[i]].wb;
            bool have = false, have_q = false;
            float c = 0.0f;    // the floor the neighbours give it (-Ofast: no infinity)
            float cq = 0.0f;   // ... the known-noise ones
            const auto take = [&](int j, float v) {
                if (anchor[j] && (!have || v < c)) { c = v; have = true; }
                if (noise[j] && (!have_q || v < cq)) { cq = v; have_q = true; }
            };
            if (i > 0 && joined(chans[k[i - 1]].wb, e))
                take(i - 1, fl[i - 1] + (e.lo_db - chans[k[i - 1]].wb.hi_db));
            if (i + 1 < n && joined(e, chans[k[i + 1]].wb))
                take(i + 1, fl[i + 1] + (e.hi_db - chans[k[i + 1]].wb.lo_db));
            const float excess = e.f - c;
            const bool fills = have && excess > (e.filled ? WB_LEAVE_DB : WB_ENTER_DB) &&
                               (!e.has_ref || e.d >= excess * (e.filled ? WB_REF_LEAVE : WB_REF_ENTER));
            const float v = fills ? c : e.own;
            if (fills != fill[i] || std::fabs(v - fl[i]) > 0.01f) changed = true;
            fill[i] = fills;
            quiet[i] = have_q && std::fabs(e.f - cq) < WB_NOISE_DB;
            fl[i] = v;
            anchor[i] = fills || !e.held;
            if (fills) noise[i] = false;
        }
        if (!changed) break;
    }
    float out = chans[ch].wb.own;
    for (int i = 0; i < n; i++) {
        Wb& e = chans[k[i]].wb;
        e.filled = fill[i];
        if (quiet[i] && !e.quiet) e.quiet_ms = now;
        e.quiet = quiet[i];
        if (k[i] == ch) out = fl[i];
        else out_db[k[i]].store(fl[i], std::memory_order_relaxed);
    }
    return out;
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
        frame_ms[ch].store(t, std::memory_order_release);
        if (!reset && t - out_ms[ch].load(std::memory_order_relaxed) < EVAL_MS) return;
    }
    const float f_avg = p10(avg, fft_size);   // today's floor of the averaged spectrum
    const float r = p10(raw, fft_size) + 10.0f * std::log10(static_cast<float>(fft_size));
    const float gain = ChannelManager::get_gain(ch);
    const bool cal = doa_is_calibrating();
    const bool coherent = !multi_tuner_mode();
    const bool wb = wideband_mode_enabled.load(std::memory_order_relaxed);   // tuners side by side
    const int64_t now = now_ms();
    Wb e;
    if (wb && is_finite_value(f_avg)) e = edges_of(ch, avg, fft_size, now);
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
        // the last wideband pass: the neighbouring tuners show a signal going
        // on into this band / their noise going on at this band's floor
        const bool wb_fresh = wb && c.wb.ok && now - c.wb.ms <= WB_FRESH_MS;
        const bool wb_signal = wb_fresh && c.wb.filled;
        const bool wb_noise = wb_fresh && c.wb.quiet && now - c.wb.quiet_ms >= WB_NOISE_HOLD_MS;
        if (!e.ok) c.wb_since = 0;
        else if (c.wb_since == 0) c.wb_since = now;
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
                // a dB or two), else this frame's once it is known to be noise.
                // Wideband scan mode: no more than the quietest tuner's floor
                // at this gain - this one may sit in a signal that fills its
                // band, and every other tuner would take that as its first
                // value too; nothing until every tuner has reported once, nor
                // while one still reports another gain (a gain change on its
                // way: their floors aren't comparable)
                float quietest = r;
                bool settled = !wb || now - c.wb_since >= WB_FRESH_MS;
                for (int j = 0; wb && j < MAX_CHANNELS; j++) {
                    const Wb& o = chans[j].wb;
                    if (j == ch || !o.ok || now - o.ms > WB_FRESH_MS || !is_finite_value(o.r)) continue;
                    if (std::fabs(o.gain - gain) > 0.05f) settled = false;
                    else quietest = std::min(quietest, o.r);
                }
                float sum = 0.0f;
                int cnt = 0;
                const std::string g = gain_part(key);
                for (const auto& [k, v] : refs)
                    if (gain_part(k) == g) { sum += v; cnt++; }
                if (cnt && settled) it = refs.emplace(key, wb ? std::min(sum / cnt, quietest) : sum / cnt).first;
                else if (learn && !signal && settled) it = refs.emplace(key, quietest).first;
            }
            if (it == refs.end()) {
                c.held = false;
            } else {
                float& ref = it->second;
                const float d = r - ref;
                e.d = d;
                e.has_ref = true;
                if (verdict) {
                    // Quiet bins correlated across the antennas: a signal fills
                    // the band, measured from the remembered floor. Uncorrelated:
                    // receiver noise, followed (a real rise - active antenna,
                    // interference - at once)
                    c.held = signal && d > HELD_MIN_DB;
                    if (learn && (!signal || d < 0.0f)) follow(ref, d, dt, RISE_DB_S);
                } else if (wb_signal) {
                    // A signal the neighbouring tuners show goes on into this
                    // band: its floor comes from theirs (wideband_pass), the
                    // remembered one is the fallback and never learns the signal
                    c.held = d > HELD_MIN_DB;
                    if (learn && d < 0.0f) follow(ref, d, dt, RISE_DB_S);
                } else if (wb_noise) {
                    // The neighbours' noise goes on at this band's own floor:
                    // receiver noise (this antenna / gain differs from the
                    // others'), followed at once as a noise verdict
                    c.held = false;
                    if (learn) follow(ref, d, dt, RISE_DB_S);
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
        if (e.ok) {
            e.f = f_avg;
            e.r = r;
            e.gain = gain;
            e.own = out;
            e.held = c.held;
            e.filled = c.wb.filled;
            e.quiet = c.wb.quiet;
            e.quiet_ms = c.wb.quiet_ms;
            c.wb = e;
            out = wideband_pass(ch, now);
        } else {
            c.wb = Wb{};
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
