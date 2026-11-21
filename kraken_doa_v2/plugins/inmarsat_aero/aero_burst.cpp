#include "aero_burst.hpp"

#include "aero_fec.hpp"
#include "aero_pchan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace aero {

namespace {
constexpr double FS = 24000;
constexpr double RING_S = 14.0;        // longest burst kept (a T burst of 31 SUs at 600 bit/s is ~11 s)
constexpr double SHORT_MAX_S = 3.0;    // a burst with a short preamble tone (2400 / 10500 bit/s) lasts at most this
// a preamble tone this short (as measured: from the start of the first FFT
// with it to the end of the last, ~40 ms more than the tone) is a 2400 or
// 10500 bit/s burst's (24 / 33 ms: 0.05 - 0.08 s; 1200 bit/s: 105 ms, ~0.17 s)
constexpr double SHORT_TONE_S = 0.1;
constexpr float TONE_DB = 14.0f;       // the preamble's tone over its bin's noise (as over a median: 12.4 dB over the mean)
constexpr float TONE_ISO_DB = 10.0f;   // ... and over the bins 3..6 away (a tone, not a modulated spectrum)
constexpr float NOISE_A = 0.05f;       // the noise spectrum's smoothing per 512-point FFT (~0.2 s)
constexpr float NOISE_AL = 0.1f;       // ... per 4096-point FFT (~0.4 s)
constexpr int HN = 15, HW = 250;       // a burst's narrow (+-0.7 kHz) and wide (+-11.7 kHz) band, 512-point bins

struct BurstRate {
    double rate;
    int unmod, mod;   // preamble: unmodulated bit periods, modulated bits (table 4-12)
};
const BurstRate RATES[] = {{600, 150, 74}, {1200, 126, 74}, {2400, 78, 74}, {10500, 248, 256}};

inline float db_ratio(float db) { return std::pow(10.0f, db / 10); }
// distance of two bins on a circle of n
inline int cdist(int a, int b, int n) {
    const int d = ((a - b) % n + n) % n;
    return std::min(d, n - d);
}
// a bin's power as it goes into its noise mean: clipped at 4 x the noise
// (6 dB; noise alone gets there 1.8 % of the time) - one FFT of a preamble
// tone 15 dB up would otherwise lift its own bin's noise 2.5 times and the
// second FFT, which confirms it, no longer stands out. A signal that stays
// still raises the noise by up to 15 % per FFT. Not clipped while the mean
// starts up.
inline float clip_noise(float p, float noise, int navg) { return navg < 10 ? p : std::min(p, 4 * noise); }
inline double bin_to_hz(double b, int n) {
    double f = b * FS / n;
    return f >= FS / 2 ? f - FS : f;
}
}  // namespace

BurstRx::BurstRx(kp::Host& h, SuDecoder& su) : host(h), su_(su) {
    in_ = kfft_alloc_complex(NFFT);
    out_ = kfft_alloc_complex(NFFT);
    plan_ = kfft_plan_dft_1d(NFFT, in_, out_, KFFT_FORWARD);
    win_.resize(NFFT);
    for (int i = 0; i < NFFT; i++) win_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2 * kp::PI * static_cast<float>(i) / NFFT);
    pw_.resize(NFFT);
    inl_ = kfft_alloc_complex(NL);
    outl_ = kfft_alloc_complex(NL);
    planl_ = kfft_plan_dft_1d(NL, inl_, outl_, KFFT_FORWARD);
    winl_.resize(NL);
    for (int i = 0; i < NL; i++) winl_[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2 * kp::PI * static_cast<float>(i) / NL);
    pwl_.resize(NL);
    reset();
}

BurstRx::~BurstRx() {
    kfft_destroy_plan(plan_);
    kfft_free(in_);
    kfft_free(out_);
    kfft_destroy_plan(planl_);
    kfft_free(inl_);
    kfft_free(outl_);
}

void BurstRx::reset() {
    ring_.clear();
    ring_base_ = nin_ = 0;
    next_fft_ = NFFT;
    next_l_ = NL;
    tbase_ = false;
    slots_.clear();
    recent_.clear();
    pend_.clear();
    pend_l_.clear();
    noise_.assign(NFFT, 0.0f);
    smooth_.assign(NFFT, 0.0f);
    noise_l_.assign(NL, 0.0f);
    navg_ = navg_l_ = 0;
    bursts_ = decoded_ = 0;
}

void BurstRx::process(const cf* x, size_t n, double t0) {
    if (!tbase_) {
        t0_base_ = t0;
        tbase_ = true;
    }
    ring_.insert(ring_.end(), x, x + n);
    nin_ += static_cast<int64_t>(n);
    // keep RING_S, or from the earliest burst still followed
    int64_t need = nin_ - static_cast<int64_t>(RING_S * FS);
    for (const auto& s : slots_) need = std::min(need, s.start - s.unc - 1024);
    if (need > ring_base_) {
        const size_t d = static_cast<size_t>(std::min<int64_t>(need - ring_base_, static_cast<int64_t>(ring_.size())));
        ring_.erase(ring_.begin(), ring_.begin() + static_cast<long>(d));
        ring_base_ += static_cast<int64_t>(d);
    }
    while (next_fft_ <= nin_ || next_l_ <= nin_) {
        if (next_fft_ <= next_l_) {
            fft_at(next_fft_);
            next_fft_ += HOP;
        } else {
            fft_long(next_l_);
            next_l_ += HL;
        }
    }
}

// the tones in a power spectrum: local maxima at least TONE (+ extra) over
// their bin's noise and TONE_ISO over the bins 3..6 away
void BurstRx::find_tones(const std::vector<float>& p, const std::vector<float>& noise, float extra_db, std::vector<Tone>& out) const {
    out.clear();
    const int n = static_cast<int>(p.size());
    // a floor under the noise: bins with no input at all (band-limited
    // input, the edges of a decimator's filter) would otherwise make
    // anything a tone
    // anything when there's no noise at all (a generated file before its
    // noise is added: spurs 80 dB under the burst), so also no more than
    // 70 dB under this FFT's strongest bin
    double mean = 0;
    for (float v : noise) mean += v;
    const float pmax = *std::max_element(p.begin(), p.end());
    const float floor_abs = std::max(static_cast<float>(1e-4 * mean / n), 1e-7f * pmax) + 1e-30f;
    const float thr = db_ratio(TONE_DB + extra_db) * 0.693f;   // over a median = over 0.693 x the mean
    const float iso = db_ratio(TONE_ISO_DB);
    auto at = [&](int i) { return p[static_cast<size_t>(((i % n) + n) % n)]; };
    for (int k = 0; k < n; k++) {
        const float v = p[static_cast<size_t>(k)];
        const float nk = std::max(noise[static_cast<size_t>(k)], floor_abs);
        if (v < thr * nk) continue;
        if (v < at(k - 1) || v < at(k + 1) || v < at(k - 2) || v < at(k + 2)) continue;
        float side = 0;
        for (int d = 3; d <= 6; d++) side = std::max({side, at(k - d), at(k + d)});
        if (v < iso * side) continue;
        const float l = at(k - 1), r = at(k + 1);
        const float den = l - 2 * v + r;
        const double frac = den != 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
        out.push_back({k, v / (thr * nk), frac});
    }
}

// A burst followed covers its spectrum: there, no new preamble is looked
// for (the modulated spectrum's random peaks, and at R / 4 and R / 2 the
// lines of its preamble's alternating bits, would pass for tones - at R / 2
// its differential detection even gives the same bits) and the noise is
// not updated. A-BPSK (a long preamble tone): +-2 HN (+-1.4 kHz); a short
// preamble tone (2400 / 10500 bit/s) once it is over: the wide band.
bool BurstRx::slot_wide(const Slot& s, int64_t now) const {
    return now - s.tone_seen > 4 * HOP && s.tone_seen - s.start < static_cast<int64_t>(SHORT_TONE_S * FS);
}

int BurstRx::slot_covering(int bin, int64_t now, int skip) const {
    for (size_t i = 0; i < slots_.size(); i++) {
        if (static_cast<int>(i) == skip) continue;
        const Slot& s = slots_[i];
        if (cdist(bin, s.bin, NFFT) <= (slot_wide(s, now) ? HW : 2 * HN)) return static_cast<int>(i);
    }
    return -1;
}

// a tone within 4 kHz of a burst followed and 40 dB or more under that
// burst's carrier: its spectrum's far lines (they only stand out with
// little noise), not a burst of its own
bool BurstRx::under_strong(int bin, float p, bool long_fft) const {
    for (const auto& s : slots_)
        if (cdist(bin, s.bin, NFFT) <= 85 && p < 1e-4f * (long_fft ? s.peak_tl : s.peak_t)) return true;
    return false;
}

void BurstRx::open_slot(int64_t start, double f0, int unc, int64_t now) {
    if (slots_.size() >= MAX_SLOTS) return;   // more at once than that: the later ones are missed
    // the preamble's alternating bits put lines at R / 4 and R / 2 from the
    // carrier, just after its tone: for 10.5 kbit/s (2625 / 5250 Hz) beyond
    // the band a burst covers while its tone is on (the slower rates' lines
    // are inside it)
    for (const auto& o : slots_) {
        if (now - o.start > static_cast<int64_t>(0.6 * FS)) continue;
        const double d = std::fabs(f0 - o.f0);
        for (double g : {150.0, 300.0, 600.0, 1200.0, 2625.0, 5250.0})
            if (std::fabs(d - g) <= 60) return;
    }
    Slot s;
    s.start = start;
    s.tone_seen = now;
    s.f0 = f0;
    s.unc = unc;
    s.bin = static_cast<int>(std::lround(f0 * NFFT / FS));
    if (s.bin < 0) s.bin += NFFT;
    for (int i = -HW; i <= HW; i++) s.noise_w += noise_[static_cast<size_t>(((s.bin + i) % NFFT + NFFT) % NFFT)];
    // the narrow band's noise: the median of its bins (the preamble tone
    // lifted its own bins' noise before it was confirmed - the 4096-point
    // detector takes ~0.2 s - and with the sum the burst seemed gone early)
    std::array<float, 2 * HN + 1> nb{};
    for (int i = -HN; i <= HN; i++) nb[static_cast<size_t>(i + HN)] = noise_[static_cast<size_t>(((s.bin + i) % NFFT + NFFT) % NFFT)];
    std::nth_element(nb.begin(), nb.begin() + HN, nb.end());
    s.noise_n = nb[HN] * (2 * HN + 1);
    slots_.push_back(s);
}

void BurstRx::end_slot(size_t i, int64_t end, bool too_long) {
    const Slot s = slots_[i];
    slots_.erase(slots_.begin() + static_cast<long>(i));
    bursts_++;
    const double tone_s = static_cast<double>(s.tone_seen - s.start) / FS;
    if (debug_) {
        char b[160];
        snprintf(b, sizeof b, "burst at %.3f s, %.3f s long, carrier %+.1f Hz, tone %.3f s%s", host_t(static_cast<double>(s.start)),
                 static_cast<double>(end - s.start) / FS, s.f0, tone_s, too_long ? " (too long: not a burst?)" : "");
        host.log(b);
    }
    if (too_long) {
        // a signal that stays (a continuous carrier, another system): its
        // level becomes this band's noise, or it would open a burst again
        // and again, each one decoded at every rate (not in another burst's
        // band: its noise from before it stays)
        for (int k = -HN; k <= HN; k++) {
            const int b = ((s.bin + k) % NFFT + NFFT) % NFFT;
            if (slot_covering(b, end) >= 0) continue;
            noise_[static_cast<size_t>(b)] = std::max(noise_[static_cast<size_t>(b)], smooth_[static_cast<size_t>(b)]);
        }
        for (int k = -8 * HN; k <= 8 * HN; k++) {
            const int j = ((8 * s.bin + k) % NL + NL) % NL;
            if (slot_covering(j / 8, end) >= 0) continue;
            noise_l_[static_cast<size_t>(j)] = std::max(noise_l_[static_cast<size_t>(j)], pwl_[static_cast<size_t>(j)]);
        }
    }
    decode(s.start - 512, end, s.f0, std::max(0.0, tone_s - 0.04), s.unc);   // the tone as measured runs ~40 ms long
}

void BurstRx::fft_at(int64_t end) {
    const int64_t a = end - NFFT;
    if (a < ring_base_) return;
    for (int i = 0; i < NFFT; i++) {
        const cf v = ring_[static_cast<size_t>(a - ring_base_ + i)] * win_[static_cast<size_t>(i)];
        in_[i][0] = v.real();
        in_[i][1] = v.imag();
    }
    kfft_execute(plan_);
    for (int k = 0; k < NFFT; k++) pw_[static_cast<size_t>(k)] = out_[k][0] * out_[k][0] + out_[k][1] * out_[k][1];
    auto wrap = [](int i) { return static_cast<size_t>(((i % NFFT) + NFFT) % NFFT); };
    for (int k = 0; k < NFFT; k++) smooth_[static_cast<size_t>(k)] += 0.3f * (pw_[static_cast<size_t>(k)] - smooth_[static_cast<size_t>(k)]);

    // the bursts followed: still there? (the band around the carrier
    // against its noise from before the burst; the wide band too for a
    // burst whose preamble tone was short: 2400 / 10500 bit/s)
    for (size_t i = 0; i < slots_.size();) {
        Slot& s = slots_[i];
        for (int k = -1; k <= 1; k++) s.peak_t = std::max(s.peak_t, pw_[wrap(s.bin + k)]);
        float narrow = 0, wide = 0;
        for (int k = -HW; k <= HW; k++) {
            const size_t j = wrap(s.bin + k);
            wide += smooth_[j];
            if (std::abs(k) <= HN) {
                narrow += smooth_[j];
            }
        }
        const double tone_s = static_cast<double>(s.tone_seen - s.start) / FS;
        const bool tone_over = end - s.tone_seen > 4 * HOP;
        const bool short_tone = tone_over && tone_s < SHORT_TONE_S;
        s.peak_n = std::max(s.peak_n, narrow);
        s.peak_w = std::max(s.peak_w, wide);
        // over the noise from before it - and within 20 dB of its most (with
        // no noise at all - a generated file before the player adds it -
        // anything is over the noise)
        const bool present = narrow > std::max(1.4f * s.noise_n, 0.01f * s.peak_n) ||
                             (short_tone && wide > std::max(1.2f * s.noise_w, 0.01f * s.peak_w));
        s.quiet = present ? 0 : s.quiet + 1;
        const bool too_long = end - s.start > static_cast<int64_t>((short_tone ? SHORT_MAX_S : RING_S - 1) * FS);
        if (s.quiet >= 8 || too_long) {
            end_slot(i, end, too_long);   // end: 3 hops after the signal left
            continue;
        }
        i++;
    }

    if (navg_ >= 8) detect(end, a);
    // the noise per bin, outside the bursts followed - after the detection:
    // with this FFT in it, a tone could never stand more than 1 / NOISE_A
    // (13 dB) over its own bin's noise
    const float al = navg_ < 20 ? 1.0f / static_cast<float>(navg_ + 1) : NOISE_A;
    for (int k = 0; k < NFFT; k++) {
        if (slot_covering(k, end) >= 0) continue;
        float& nz = noise_[static_cast<size_t>(k)];
        nz += al * (clip_noise(pw_[static_cast<size_t>(k)], nz, navg_) - nz);
    }
    navg_++;
}

// preamble tones: a new burst after two FFTs in a row - or one, well above
// the noise (the 10.5 kbit/s burst's unmodulated preamble lasts only 24
// ms). Several bursts on different frequencies are followed at once.
void BurstRx::detect(int64_t end, int64_t a) {
    find_tones(pw_, noise_, 0, tones_);
    for (auto& p : pend_) p.hit = false;
    for (const Tone& t : tones_) {
        // on a burst's own carrier: its tone, or the next burst there
        int sl = -1;
        for (size_t i = 0; i < slots_.size(); i++)
            if (cdist(t.bin, slots_[i].bin, NFFT) <= 2) sl = static_cast<int>(i);
        if (slot_covering(t.bin, end, sl) >= 0) continue;   // inside another burst's spectrum
        if (sl < 0 && under_strong(t.bin, pw_[static_cast<size_t>(t.bin)], false)) continue;
        bool restart = false;
        if (sl >= 0) {
            Slot& s = slots_[static_cast<size_t>(sl)];
            if (end - s.tone_seen <= static_cast<int64_t>(0.15 * FS)) {
                s.tone_seen = end;   // its preamble, still on
                continue;
            }
            restart = true;   // a new preamble where a burst is still followed: the next burst on that frequency
        }
        Pend* p = nullptr;
        for (auto& q : pend_)
            if (cdist(q.bin, t.bin, NFFT) <= 1) p = &q;
        if (p) {
            p->hits++;
            p->bin = t.bin;
        } else {
            pend_.push_back({t.bin, 1, a, false});
            p = &pend_.back();
        }
        p->hit = true;
        const bool go = restart ? p->hits >= 6 : (p->hits >= 2 || t.ratio >= db_ratio(4));
        if (!go) continue;
        const int64_t st = p->start;
        p->hits = -1000;   // used
        if (restart) end_slot(static_cast<size_t>(sl), a + NFFT / 2, false);
        open_slot(st, bin_to_hz(t.bin + t.frac, NFFT), 1024, end);
    }
    pend_.erase(std::remove_if(pend_.begin(), pend_.end(), [](const Pend& p) { return !p.hit || p.hits < 0; }), pend_.end());
}

// the slow rates' long unmodulated preambles (250 ms at 600, 105 ms at 1200
// bit/s): a 4096-point FFT (5.9 Hz bins) finds them 9 dB weaker than the
// 512-point one
void BurstRx::fft_long(int64_t end) {
    const int64_t a = end - NL;
    if (a < ring_base_) return;
    for (int i = 0; i < NL; i++) {
        const cf v = ring_[static_cast<size_t>(a - ring_base_ + i)] * winl_[static_cast<size_t>(i)];
        inl_[i][0] = v.real();
        inl_[i][1] = v.imag();
    }
    kfft_execute(planl_);
    for (int k = 0; k < NL; k++) pwl_[static_cast<size_t>(k)] = outl_[k][0] * outl_[k][0] + outl_[k][1] * outl_[k][1];
    for (auto& s : slots_)
        for (int k = -8; k <= 8; k++) s.peak_tl = std::max(s.peak_tl, pwl_[static_cast<size_t>(((8 * s.bin + k) % NL + NL) % NL)]);
    if (navg_l_ >= 4) detect_long(end, a);
    const float al = navg_l_ < 10 ? 1.0f / static_cast<float>(navg_l_ + 1) : NOISE_AL;
    for (int k = 0; k < NL; k++) {
        if (slot_covering(k / 8, end) >= 0) continue;
        float& nz = noise_l_[static_cast<size_t>(k)];
        nz += al * (clip_noise(pwl_[static_cast<size_t>(k)], nz, navg_l_) - nz);
    }
    navg_l_++;
}

void BurstRx::detect_long(int64_t end, int64_t a) {
    find_tones(pwl_, noise_l_, -1, tones_);
    for (auto& p : pend_l_) p.hit = false;
    for (const Tone& t : tones_) {
        const double fb = (t.bin + t.frac) / 8.0;   // in 512-point bins
        const int kb = static_cast<int>(std::lround(fb)) % NFFT;
        if (slot_covering(kb, end) >= 0 || under_strong(kb, pwl_[static_cast<size_t>(t.bin)], true)) continue;
        Pend* p = nullptr;
        for (auto& q : pend_l_)
            if (cdist(q.bin, t.bin, NL) <= 1) p = &q;
        if (p) {
            p->hits++;
            p->bin = t.bin;
        } else {
            pend_l_.push_back({t.bin, 1, a, false});
            p = &pend_l_.back();
        }
        p->hit = true;
        if (p->hits < 2) continue;
        const int64_t st = p->start;
        p->hits = -1000;
        open_slot(st, bin_to_hz(t.bin + t.frac, NL), NL + HL, end);
    }
    pend_l_.erase(std::remove_if(pend_l_.begin(), pend_l_.end(), [](const Pend& p) { return !p.hit || p.hits < 0; }), pend_l_.end());
}

// the same burst decoded twice (two of the bursts followed were one: a line
// of its spectrum taken for another preamble): same unique word time, same
// CRC
bool BurstRx::seen_before(double t_uw, uint32_t key) {
    while (!recent_.empty() && recent_.front().first < t_uw - 30) recent_.pop_front();
    for (const auto& r : recent_)
        if (r.second == key && std::fabs(r.first - t_uw) < 0.02) return true;
    recent_.push_back({t_uw, key});
    return false;
}

void BurstRx::decode(int64_t s0, int64_t s1, double f, double tone_s, int unc) {
    s0 = std::max(s0, ring_base_);
    s1 = std::min(s1, nin_);
    if (s1 - s0 < 1200) return;
    // rates in order of the tone's length (the unmodulated preamble)
    std::vector<std::pair<double, double>> order;
    for (const auto& r : RATES) order.push_back({std::fabs(r.unmod / r.rate - tone_s), r.rate});
    std::sort(order.begin(), order.end());
    for (const auto& o : order)
        if (try_rate(o.second, s0, s1, f, unc)) {
            decoded_++;
            return;
        }
}

bool BurstRx::try_rate(double rate, int64_t s0, int64_t s1, double f, int unc) {
    const BurstRate* br = nullptr;
    for (const auto& r : RATES)
        if (r.rate == rate) br = &r;
    if (!br) return false;
    const bool qpsk = rate >= 4800;
    const int uwl = qpsk ? 64 : 32;
    DemodCfg c;
    c.mod = qpsk ? Mod::AQPSK : Mod::ABPSK;
    c.rate = rate;
    c.fs = FS;
    c.decim = rate <= 1200 ? 5 : rate <= 2400 ? 2 : 1;
    c.rolloff = qpsk ? 0.6f : 1.0f;
    c.pll_bw = static_cast<float>(std::max(0.002, 10.0 / rate));
    c.tim_gain = 4e-4f;
    c.gear_bits = br->unmod + br->mod + 64 + static_cast<int>(512 / (FS / rate));   // fast loops through the preamble
    const double sps_in = FS / rate;
    // no more than this rate's longest burst (31 SUs) after the start: a
    // long window (a burst followed while another signal kept its band
    // busy) costs the demodulator at four phases for nothing
    const int64_t longest = unc + 512 + static_cast<int64_t>((br->unmod + br->mod + uwl + 64 * 95 + 128) * sps_in);
    s1 = std::min(s1, s0 + longest);
    std::vector<cf> x(ring_.begin() + static_cast<long>(s0 - ring_base_), ring_.begin() + static_cast<long>(s1 - ring_base_));
    // the demodulator from four starting timing phases; each one's best
    // unique word is a candidate, decoded in order of its UW errors until
    // one passes its CRC
    struct Best {
        int err = 1 << 30;
        std::vector<float> s;
        std::vector<double> t;
        int64_t p = 0;
        float sa = 1, sb = 1;
    };
    std::vector<Best> cands;
    const int pre = br->unmod + br->mod;
    for (int ph = 0; ph < 4; ph++) {
        Best best;
        const int o = static_cast<int>(std::lround(ph * sps_in / 4));
        Demod d;
        d.start(c, f, static_cast<double>(s0 + o));
        std::vector<SoftBit> bits;
        d.process(x.data() + o, x.size() - static_cast<size_t>(o), bits);
        std::vector<float> s;
        std::vector<double> t;
        if (qpsk) {
            for (const auto& b : bits) {
                s.push_back(b.d);
                t.push_back(b.t);
            }
        } else {   // differential detection, + = "0"
            for (size_t i = 1; i < bits.size(); i++) {
                const float m = std::min(std::fabs(bits[i].d), std::fabs(bits[i - 1].d));
                s.push_back(bits[i].d * bits[i - 1].d > 0 ? -m : m);
                t.push_back(bits[i].t);
            }
        }
        const int64_t nb = static_cast<int64_t>(s.size());
        // the UW: after the preamble, counted from the start the detector saw
        // (it may be up to two FFTs early: 1024 samples)
        const int64_t hi = std::min<int64_t>(nb - uwl - 320, pre + 64 + static_cast<int64_t>(unc / sps_in) + 64);
        for (int64_t p = 0; p <= hi; p++) {
            int e = 0, ea = 0, eb = 0;
            if (qpsk) {
                for (int k = 0; k < 32; k++) {
                    const int u = static_cast<int>((UW_P >> (31 - k)) & 1);
                    ea += (s[static_cast<size_t>(p + 2 * k)] < 0) != u;
                    eb += (s[static_cast<size_t>(p + 2 * k + 1)] < 0) != u;
                }
                e = std::min(ea, 32 - ea) + std::min(eb, 32 - eb);
            } else {
                for (int k = 0; k < 32; k++) ea += (s[static_cast<size_t>(p + k)] < 0) != static_cast<int>((UW_P >> (31 - k)) & 1);
                e = std::min(ea, 32 - ea);
                eb = ea;
            }
            if (e < best.err) {
                best.err = e;
                best.p = p;
                best.sa = ea <= 16 ? 1.0f : -1.0f;
                best.sb = eb <= 16 ? 1.0f : -1.0f;
            }
        }
        if (debug_) {
            char b[160];
            snprintf(b, sizeof b, "burst try %.0f phase %d: UW errors %d at bit %lld of %lld (window %lld), carrier %+.1f Hz", rate,
                     ph, best.err, static_cast<long long>(best.p), static_cast<long long>(nb), static_cast<long long>(hi), f);
            host.log(b);
        }
        if (best.err <= (qpsk ? 6 : 3)) {
            best.s = std::move(s);
            best.t = std::move(t);
            cands.push_back(std::move(best));
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Best& a, const Best& b) { return a.err < b.err; });
    for (const Best& cand : cands)
        if (decode_cand(rate, uwl, qpsk, f, cand.s, cand.t, cand.p, cand.sa, cand.sb)) return true;
    return false;
}

bool BurstRx::decode_cand(double rate, int uwl, bool qpsk, double f, const std::vector<float>& s, const std::vector<double>& tt,
                          int64_t p, float sa, float sb) {
    struct {
        int64_t p;
        float sa, sb;
        const std::vector<double>& t;
    } best{p, sa, sb, tt};
    const int64_t q = best.p + uwl;   // first coded bit
    const int64_t avail = static_cast<int64_t>(s.size()) - q;
    auto soft = [&](int64_t j) {
        const float v = s[static_cast<size_t>(q + j)];
        return (j & 1) ? v * best.sb : v * best.sa;
    };
    const double t_uw = host_t(best.t[static_cast<size_t>(best.p)]);   // bit times are input sample indexes
    const double bit_s = 1.0 / rate;
    // decode a list of blocks (columns each), from the start of the field
    auto run = [&](const std::vector<int>& cols, std::vector<uint8_t>& info) {
        Viterbi v;
        v.reset(true);
        int64_t j0 = 0;
        for (int cc : cols) {
            const int nbits = 64 * cc;
            for (int k = 0; k < nbits; k += 2)
                v.push(soft(j0 + deinterleave_index(k, cc)), soft(j0 + deinterleave_index(k + 1, cc)));
            j0 += nbits;
        }
        v.flush();
        info.clear();
        v.take(info);
        return j0;
    };
    auto descramble = [](std::vector<uint8_t>& b, size_t n) {
        Scrambler sc;
        for (size_t i = 0; i < n && i < b.size(); i++) b[i] ^= static_cast<uint8_t>(sc.next());
    };
    auto talker = [&](uint32_t aes, double t_end) {
        host.valid(std::max(t_uw, t_end - 1.2), t_end);   // the part still in kraken_doa's 1.5 s stream
        if (!aes) return;
        kp::Talker t;
        t.id = aes_hex(aes);
        const std::string l = su_.label_of(aes);
        t.label = l.empty() ? "aircraft" : l;
        // the burst's LAST 0.24 s (from the UW on, all of it is the
        // aircraft's): kraken_doa keeps the VFO's samples for packets only
        // ~1.5 s, and a long T burst is decoded seconds after its UW
        t.end_s = t_end;
        t.start_s = std::max(t_uw, t_end - 0.24);
        t.packet = true;
        t.freq_hz = static_cast<float>(f);
        t.bw_hz = static_cast<float>(qpsk ? 10000 : 2 * rate + 400);
        t.avg_s = 10;
        t.keep_s = 1800;
        host.talker(t);
    };
    if (avail < 320) return false;
    std::vector<uint8_t> info;
    // R burst: one block of 5 columns: a 19-octet SU + 8 flush bits
    run({5}, info);
    {
        std::vector<uint8_t> b(info.begin(), info.begin() + 152);
        descramble(b, 152);
        uint8_t su[19];
        pack_lsb(b.data(), 19, su);
        if (su_crc_ok(su, 19)) {
            if (seen_before(t_uw, 0x1000000u | su[17] | (su[18] << 8))) return true;
            const uint32_t aes = su_.r_su(su, t_uw, t_uw + (uwl + 320) * bit_s);
            if (host.raw_wanted()) {
                std::string r = "R SU";
                for (int i = 0; i < 19; i++) {
                    char h[4];
                    snprintf(h, sizeof h, " %02X", su[i]);
                    r += h;
                }
                host.raw(r);
            }
            talker(aes, t_uw + (uwl + 320) * bit_s);
            return true;
        }
    }
    // T burst: burst identifier (6 octets) + signal units
    std::vector<int> cols;
    if (!qpsk) {
        // as many blocks as the burst held (5 columns, then 3 each)
        const int64_t nblk = 1 + std::max<int64_t>(0, (avail - 320) / 192);
        cols.push_back(5);
        for (int64_t k = 1; k < std::min<int64_t>(nblk, 31); k++) cols.push_back(3);
        run(cols, info);
    } else {
        // one block of 2 + 3n columns: the n whose block structure checks

        bool ok = false;
        for (int64_t n = 2; n <= 31; n++) {
            if (n < 2 || n > 31 || 64 * (2 + 3 * n) > avail) continue;
            run({static_cast<int>(2 + 3 * n)}, info);
            std::vector<uint8_t> b(info);
            descramble(b, b.size() - 16);
            uint8_t id[6];
            pack_lsb(b.data(), 6, id);
            if (!su_crc_ok(id, 6)) continue;
            bool all = true;
            for (int64_t j = 0; j < n && all; j++) {
                uint8_t su[12];
                pack_lsb(&b[static_cast<size_t>(48 + 96 * j)], 12, su);
                all = su_crc_ok(su, 12);
            }
            if (all) {
                ok = true;
                cols = {static_cast<int>(2 + 3 * n)};
                break;
            }
        }
        if (!ok) return false;
    }
    std::vector<uint8_t> b(info);
    descramble(b, b.size());
    uint8_t id[6];
    pack_lsb(b.data(), 6, id);
    if (!su_crc_ok(id, 6)) return false;
    std::vector<std::vector<uint8_t>> sus;
    for (size_t j = 0; 48 + 96 * (j + 1) <= b.size(); j++) {
        std::vector<uint8_t> su(12);
        pack_lsb(&b[48 + 96 * j], 12, su.data());
        if (!su_crc_ok(su.data(), 12)) break;
        sus.push_back(su);
    }
    if (sus.empty()) return false;
    std::vector<const uint8_t*> ptrs;
    for (const auto& v : sus) ptrs.push_back(v.data());
    // the end of the field: the blocks that held the SUs (A-BPSK: 5 + 3 (n - 1) columns; 10.5k: 2 + 3n)
    const double cols_used = qpsk ? 2 + 3 * static_cast<double>(sus.size()) : 5 + 3 * static_cast<double>(sus.size() - 1);
    const double t_end = t_uw + (uwl + 64 * cols_used) * bit_s;
    if (seen_before(t_uw, 0x2000000u | (static_cast<uint32_t>(sus.size()) << 16) | id[4] | (id[5] << 8))) return true;
    su_.t_burst(id, ptrs, t_uw, t_end);
    if (host.raw_wanted()) {
        char h[64];
        snprintf(h, sizeof h, "T burst AES %06X GES %02X SUs %zu", (id[0] << 16) | (id[1] << 8) | id[2], id[3], sus.size());
        host.raw(h);
    }
    talker((static_cast<uint32_t>(id[0]) << 16) | (static_cast<uint32_t>(id[1]) << 8) | id[2], t_end);
    return true;
}

}  // namespace aero
