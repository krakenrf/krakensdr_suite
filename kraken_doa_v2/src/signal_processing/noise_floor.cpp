// The spectrum's noise floor per tuner channel, with the receiver's own floor
// remembered - see noise_floor.hpp.
#include "signal_processing/noise_floor.hpp"

#include "channel_manager.hpp"
#include "config.hpp"
#include "doa_logger.hpp"      // doa_is_calibrating
#include "geo_address.hpp"     // geo::json_parse
#include "globals.hpp"         // multi_tuner_mode
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
    if (!f || rename(tmp.c_str(), FILE_NAME) != 0)
        std::cerr << "Noise floors: can't write " << FILE_NAME << std::endl;
}

void save(bool force) {
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
