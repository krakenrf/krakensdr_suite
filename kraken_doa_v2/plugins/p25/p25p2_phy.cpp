// P25 Phase 2 physical layer: see p25p2_phy.hpp. Written from the
// project's P25 Phase 2 fact sheet (TIA-102.BBAC-A burst maps and sync
// words, the H-DQPSK / H-CPM modulation definitions) and measurements on
// synthetic and real signals; no third-party decoder was consulted.
//
// Front end: one complex channel filter at 48 kHz (8 samples / symbol, -6
// dB at 5 kHz); its output is kept in a ring indexed by input time (the
// filter delay taken off), so every position below is an input sample
// index. Symbol k of a slot starting at t0 spans [t0 + 8k, t0 + 8k + 8);
// its centre is t0 + 8k + 4.
//
// Outbound (H-DQPSK): the phase changes by I * 45 degrees per symbol (I =
// +-1 / +-3); the transmit filter only smooths the transitions. Soft symbol
// = arg(y(c_k) conj(y(c_k-1)) e^-jw) / 45 degrees / scale, at the symbol
// centres c_k (w = carrier rotation per symbol). Neither the exact transmit
// filter (fact sheet Q1) nor the amplitude matters: a discriminator-derived,
// constant-envelope recording is detected the same way, and the scale is
// measured, not assumed (levels sit some 10 % off nominal with some
// transmit filters). Measured against a discriminator integrated between
// the centres (the same phase difference, but unwrapped): the wrapped
// differential detection has 1.4-1.7x fewer symbol errors at 7-14 dB SNR
// (a noise click costs the unwrapped sum a whole turn).
// - Search: d[m] = y[m] conj(y[m-8]) is correlated every 2nd sample with
//   the S-ISCH (20 symbols, all +-3 = +-135 degrees): the correlation is
//   -0.707 (A +- jB), A = sum d_k, B = sum sign_k d_k - additions only, any
//   carrier offset (it rotates the sum: arg = w, unambiguous to +-3 kHz),
//   the other sign is the spectrum-inverted signal. |C| / sum |d_k| >= 0.75
//   is a hit; two hits 180 or 540 symbols apart with the same polarity and
//   carrier (the S-ISCH pairs before slots 2/3, 6/7, 10/11) lock the grid
//   if the slot after the older one sits on the four levels (rms error <=
//   0.45; noise 0.58), and the slots from that one on are delivered.
// - Tracking, once per slot: symbols -10..189 are taken at the current
//   timing and 1 sample early / late; the decision-directed error energies
//   give a parabolic timing error (first + second order loop for the sample
//   clock), the mean residual phase the carrier, a least-squares fit the
//   angle scale. An S-ISCH at either end of the slot or the S-OEMI sync
//   inside it confirms the grid; 34 slots (~1 s) without one give it up.
//   After 3 unconfirmed slots the S-ISCH search runs again: a strong hit off
//   the grid (the sample stream slipped - real captures drop samples, often
//   a whole number of symbols, so the symbols still look clean) starts a
//   new grid at once.
//
// Inbound (H-CPM, h = 1/3, 4-symbol frequency pulse; fact sheet 1.3; this
// receiver's convention: the pulse of symbol k is centred on the middle of
// its period): the FM discriminator integrated between two symbol centres
// sees z_n = H1 (I_n + I_n+1) + H0 (I_n-1 + I_n+2), H1 = 489 Hz, H0 = 11 Hz
// per level unit - a duobinary partial response (measured through the
// channel filter: 488 / 12 Hz, the rest < 1 Hz).
// - Search: z is correlated every 2nd sample (mean removed, normalized) with
//   the 21 pair sums of the 22-symbol sync; the sign is the polarity, a
//   least-squares fit gives the carrier offset (the DC of z) and deviation
//   scale. The first burst that checks out sets the 30 ms grid; on a grid
//   only the windows where a sync can end are searched.
// - Every grid slot is examined once its samples are in: a hit inside it
//   is a burst with sync; otherwise the timing / carrier / scale of each
//   radio track seen recently (fine timing +-2 samples), or only if none
//   fits, the grid timing +-3 symbols with a blind carrier, are scored by a
//   cheap 4-state trellis; the best goes to the detector and is kept if it
//   fits well enough, looks like H-CPM, and (track) keeps the radio's
//   carrier / scale / timing or (blind) is unambiguous against one or two
//   symbols' shift. Each radio track (carrier, deviation scale, timing vs
//   the grid) is updated by its own bursts, its timing only by bursts with
//   a sync.
// - Detector: a 16-state Viterbi over (I_k-2, I_k-1) with the outer tap's
//   I_k-3 from each survivor (decision feedback); the known pilots / sync
//   at both ends start and end the trellis. Then a robust decision-directed
//   refit of timing (+-2 samples, parabolic), carrier and scale, and a
//   second pass when they moved.

#include "p25p2_phy.hpp"

#include <algorithm>
#include <cmath>

#include "kraken_dsp.hpp"

namespace dig {

namespace {

constexpr int SPS = P25P2_SPS;
constexpr float PI_F = 3.14159265358979f;
constexpr float RAD2HZ = 48000.0f / (2.0f * PI_F);       // rad / sample -> Hz
constexpr double SLOT = P25P2_SLOT_SAMPLES;
constexpr int SEARCH_STEP = 2;          // correlate every 2nd sample (half the search cost; the peak is broad)

// S-ISCH, S-OEMI, inbound sync (fact sheet 3), symbols first -> last
constexpr int8_t S_ISCH[20] = {3, 3, 3, -3, 3, 3, -3, 3, 3, 3, 3, -3, -3, -3, 3, -3, -3, -3, -3, -3};
constexpr float SIGN_ISCH[20] = {1, 1, 1, -1, 1, 1, -1, 1, 1, 1, 1, -1, -1, -1, 1, -1, -1, -1, -1, -1};
constexpr int8_t S_OEMI[21] = {-3, -3, -3, -3, 3, 3, 3, 3, 3, -3, 3, -3, 3, -3, -3, 3, 3, -3, -3, 3, 3};
constexpr int8_t S_IN[22] = {3, 3, -3, -3, 3, 3, 3, -3, 3, -3, -3, 3, 3, 3, 3, -3, 3, -3, -3, -3, -3, -3};
constexpr int8_t P1[4] = {1, -1, 1, -1};   // first pilot of a burst without sync (symbols 6-9)
constexpr int8_t P2[4] = {-1, 1, -1, 1};   // last pilot of every burst (symbols 170-173)

constexpr int OUT_K0 = -10, OUT_K1 = 189;  // symbols taken per outbound slot (both ISCH halves)
constexpr int OUT_NK = OUT_K1 - OUT_K0 + 1;
constexpr float LOCK_RMS = 0.45f;          // outbound lock: largest soft-symbol rms error of the first slot (noise: 0.577)
constexpr int OUT_MISS_LIMIT = 34;         // outbound slots (~1 s) without a sync confirmation
constexpr int IN_SYNC_LIMIT = 34;          // inbound: grid slots without a burst with sync ...
constexpr int IN_GOOD_LIMIT = 12;          // ... and without any accepted burst
constexpr int IN_K0 = 6, IN_K1 = 173;      // inbound symbols with defined content (pilot .. pilot)
constexpr int IN_NZ = IN_K1 - IN_K0 + 2;   // pair sums z_5 .. z_173
constexpr float RMS_NEW_GRID = 0.65f;      // inbound: largest residual of the burst that starts a grid
constexpr float TRACK_FIT = 0.35f;         // detector metric under which a track's timing is taken

// atan2 to ~1e-5 rad (the per-sample discriminator of the search)
inline float fast_atan2(float y, float x) {
    const float ax = std::fabs(x), ay = std::fabs(y);
    const float mx = std::max(ax, ay), mn = std::min(ax, ay);
    if (mx <= 0.0f) return 0.0f;
    const float a = mn / mx, s = a * a;
    float r = ((((-0.0046496474f * s + 0.0159569028f) * s - 0.0287900129f) * s + 0.0425093700f) * s - 0.0601504009f) * s;
    r = (((r + 0.0812948300f) * s - 0.1110670948f) * s + 0.1428533196f) * s;
    r = ((r - 0.1999995791f) * s + 0.3333333314f) * s;
    r = a - a * r;
    if (ay > ax) r = 1.5707963268f - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}

inline float wrap_pi(float a) {
    while (a > PI_F) a -= 2 * PI_F;
    while (a < -PI_F) a += 2 * PI_F;
    return a;
}

inline float decide4(float s) { return s >= 2.0f ? 3.0f : (s >= 0.0f ? 1.0f : (s >= -2.0f ? -1.0f : -3.0f)); }

// normalized correlation of soft symbols with a +-3 pattern
float pattern_corr(const float* s, const int8_t* p, int n) {
    float c = 0, e = 0, ep = 0;
    for (int i = 0; i < n; i++) {
        c += s[i] * p[i];
        e += s[i] * s[i];
        ep += static_cast<float>(p[i] * p[i]);
    }
    return e > 0 ? c / std::sqrt(e * ep) : 0.0f;
}

// parabolic vertex offset (-0.5 .. 0.5) of a peak b between a and c
inline double vertex(float a, float b, float c) {
    const float den = a - 2 * b + c;
    return den < 0 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
}

}  // namespace

P25p2Phy::P25p2Phy(P25p2SlotSink& sink) : sink_(sink) {
    // Channel filter, -6 dB at 5 kHz: outbound H-DQPSK (raised cosine alpha
    // 1, -6 dB at 3.6 kHz, zero from 7.2 kHz) and inbound H-CPM (+-3 kHz
    // deviation). Measured (symbol errors at 9 / 12 dB, 0 and 1 kHz offset):
    // narrower filters cut the FM-click noise of the inbound discriminator
    // (6.5 kHz: 1.4e-2 -> 5 kHz: 7.8e-3 at 12 dB) and help outbound too, but
    // 4.5 kHz and less lose more than they gain at a 1 kHz carrier offset.
    taps_ = kp::fir_lowpass(41, 5000.0f, 48000.0f);
    fdelay_ = static_cast<int>(taps_.size() - 1) / 2;
    fre_.assign(2 * taps_.size(), 0.0f);
    fim_.assign(2 * taps_.size(), 0.0f);
    y_.assign(RB, kp::cf(0, 0));
    idd_.assign(RB, 0.0f);
    // inbound sync template: the 21 pair sums I_n + I_n+1 of the sync
    float m = 0;
    for (int i = 0; i < 21; i++) {
        tmpl_raw_[i] = static_cast<float>(S_IN[i] + S_IN[i + 1]);
        m += tmpl_raw_[i];
    }
    m /= 21;
    float e = 0;
    for (int i = 0; i < 21; i++) {
        tmpl_[i] = tmpl_raw_[i] - m;
        e += tmpl_[i] * tmpl_[i];
    }
    e = std::sqrt(e);
    for (float& v : tmpl_) v /= e;
    surv_.assign(static_cast<size_t>(IN_K1 + 1) * 16, 0);
    out_hits_.reserve(16);
    in_hits_.reserve(16);
}

void P25p2Phy::reset() {
    std::fill(fre_.begin(), fre_.end(), 0.0f);
    std::fill(fim_.begin(), fim_.end(), 0.0f);
    fpos_ = 0;
    nin_ = yend_ = 0;
    std::fill(y_.begin(), y_.end(), kp::cf(0, 0));
    std::fill(idd_.begin(), idd_.end(), 0.0f);
    std::fill(std::begin(fd_), std::end(fd_), 0.0f);
    for (Phase& p : phase_) p = Phase();
    out_hits_.clear();
    in_hits_.clear();
    opk_ = Peak();
    ipk_ = Peak();
    std::fill(&orr_[0][0], &orr_[0][0] + 32, 0.0f);
    std::fill(std::begin(irr_), std::end(irr_), 0.0f);
    out_locked_ = false;
    omiss_ = 0;
    oindex_ = 0;
    in_grid_ = false;
    inew_ = false;
    gnext_ = gbase_ = 0;
    for (auto& t : tracks_) t = Track();
    last_freq_ = 0;
    st_ = Stats();
}

// --- samples ------------------------------------------------------------------
// Cubic (Catmull-Rom) interpolation of the filtered signal at position x
kp::cf P25p2Phy::yat(double x) const {
    const double fl = std::floor(x);
    const int64_t i = static_cast<int64_t>(fl);
    const float u = static_cast<float>(x - fl);
    const kp::cf a = y_[(i - 1) & RMASK], b = y_[i & RMASK], c = y_[(i + 1) & RMASK], d = y_[(i + 2) & RMASK];
    const float u2 = u * u, u3 = u2 * u;
    const float wa = -0.5f * u3 + u2 - 0.5f * u;
    const float wb = 1.5f * u3 - 2.5f * u2 + 1.0f;
    const float wc = -1.5f * u3 + 2.0f * u2 + 0.5f * u;
    const float wd = 0.5f * u3 - 0.5f * u2;
    return a * wa + b * wb + c * wc + d * wd;
}

// Linear interpolation of the symbol-integrated discriminator (Hz)
float P25p2Phy::zat(double x) const {
    const double fl = std::floor(x);
    const int64_t i = static_cast<int64_t>(fl);
    const float u = static_cast<float>(x - fl);
    return idd_[i & RMASK] * (1.0f - u) + idd_[(i + 1) & RMASK] * u;
}

void P25p2Phy::process(const kp::cf* x, size_t n) {
    const int nt = static_cast<int>(taps_.size()), half = nt / 2;
    const float* h = taps_.data();
    for (size_t i = 0; i < n; i++) {
        // a non-finite or absurdly large input sample (a broken source) is a
        // zero: one would make every product it reaches inf / NaN
        float xr = x[i].real(), xi = x[i].imag();
        if (!p2_finite(xr) || !p2_finite(xi) || std::fabs(xr) > 1e15f || std::fabs(xi) > 1e15f) xr = xi = 0.0f;
        fre_[fpos_] = fre_[fpos_ + nt] = xr;
        fim_[fpos_] = fim_[fpos_ + nt] = xi;
        fpos_ = fpos_ + 1 == static_cast<size_t>(nt) ? 0 : fpos_ + 1;
        // symmetric taps, folded; 8 independent partial sums (vectorizable
        // without reassociation)
        const float* wr = &fre_[fpos_];   // oldest .. newest
        const float* wi = &fim_[fpos_];
        float ar[8] = {}, ai[8] = {};
        int k = 0;
        for (; k + 8 <= half; k += 8)
            for (int j = 0; j < 8; j++) {
                ar[j] += h[k + j] * (wr[k + j] + wr[nt - 1 - k - j]);
                ai[j] += h[k + j] * (wi[k + j] + wi[nt - 1 - k - j]);
            }
        float re = h[half] * wr[half], im = h[half] * wi[half];
        for (; k < half; k++) {
            re += h[k] * (wr[k] + wr[nt - 1 - k]);
            im += h[k] * (wi[k] + wi[nt - 1 - k]);
        }
        for (int j = 0; j < 8; j++) { re += ar[j]; im += ai[j]; }
        const int64_t m = nin_ - fdelay_;
        nin_++;
        if (m < 0) continue;
        const kp::cf v(re, im);
        y_[m & RMASK] = v;
        yend_ = m + 1;
        // Per-sample streams of the searches. While an outbound grid is held
        // only the S-ISCH search runs (it re-anchors the grid after a slip
        // of the sample stream); the symbol-integrated discriminator serves
        // the inbound search and bursts.
        Phase& ph = phase_[m & 7];
        const int p = ph.pos;
        const kp::cf d = v * std::conj(y_[(m - SPS) & RMASK]);
        ph.re[p] = ph.re[p + PW] = d.real();
        ph.im[p] = ph.im[p + PW] = d.imag();
        ph.mag[p] = ph.mag[p + PW] = std::sqrt(d.real() * d.real() + d.imag() * d.imag());
        if (!out_locked_) {
            const kp::cf q = v * std::conj(y_[(m - 1) & RMASK]);
            fd_[m & 7] = fast_atan2(q.imag(), q.real());
            float sum = 0;
            for (int j = 0; j < SPS; j++) sum += fd_[j];
            const float z = sum * (RAD2HZ / SPS);
            idd_[m & RMASK] = z;
            ph.z[p] = ph.z[p + PW] = z;
        }
        ph.pos = p + 1 == PW ? 0 : p + 1;
        if (m > 24 * SPS && (m & (SEARCH_STEP - 1)) == 0) {   // correlators every SEARCH_STEP samples
            // a channel carries one direction; a held outbound grid is
            // confirmed every 1-2 slots, so it is only searched when that stops
            if ((!in_grid_ || inew_) && (!out_locked_ || omiss_ >= 2)) out_search(m);
            if (!out_locked_ && in_window(m)) in_search(m);
        }
        if ((m & 63) == 0) {   // slots are cut soon after their samples are in
            if (out_locked_) out_slots();
            else if (in_grid_) in_slots();
        }
    }
    if (out_locked_) out_slots();
    else if (in_grid_) in_slots();
}

// --- outbound -----------------------------------------------------------------
void P25p2Phy::out_search(int64_t m) {
    // the last 20 symbol-spaced values of this sample phase, oldest first
    const Phase& ph = phase_[m & 7];
    const int o = ph.pos + PW - 20;
    const float *re = ph.re + o, *im = ph.im + o, *mg = ph.mag + o;
    float a_r[4] = {}, a_i[4] = {}, b_r[4] = {}, b_i[4] = {}, nn[4] = {};
    for (int k = 0; k < 20; k += 4)
        for (int j = 0; j < 4; j++) {
            a_r[j] += re[k + j];
            a_i[j] += im[k + j];
            b_r[j] += SIGN_ISCH[k + j] * re[k + j];
            b_i[j] += SIGN_ISCH[k + j] * im[k + j];
            nn[j] += mg[k + j];
        }
    const float Ar = a_r[0] + a_r[1] + a_r[2] + a_r[3], Ai = a_i[0] + a_i[1] + a_i[2] + a_i[3];
    const float Br = b_r[0] + b_r[1] + b_r[2] + b_r[3], Bi = b_i[0] + b_i[1] + b_i[2] + b_i[3];
    const float N = nn[0] + nn[1] + nn[2] + nn[3];
    // C = -0.707 (A + jB) upright, -0.707 (A - jB) spectrum-inverted
    const kp::cf cn(-(Ar - Bi), -(Ai + Br)), ci(-(Ar + Bi), -(Ai - Br));
    float rn = 0, ri = 0;
    if (N > 1e-20f) {
        rn = 0.70710678f * std::abs(cn) / N;
        ri = 0.70710678f * std::abs(ci) / N;
    }
    orr_[m & 15][0] = rn;
    orr_[m & 15][1] = ri;
    const int pol = rn >= ri ? 1 : -1;
    const float r = std::max(rn, ri);
    Peak& pk = opk_;
    if (pk.active) {
        if (r > pk.best) { pk.best = r; pk.at = m; pk.pol = pol; pk.c = pol > 0 ? cn : ci; pk.age = 0; }
        else if (++pk.age >= SPS / SEARCH_STEP) {
            pk.active = false;
            const int c = pk.pol > 0 ? 0 : 1;
            const double off = SEARCH_STEP * vertex(orr_[(pk.at - SEARCH_STEP) & 15][c], pk.best, orr_[(pk.at + SEARCH_STEP) & 15][c]);
            st_.out_hits++;
            out_hit(static_cast<double>(pk.at) + off, pk.pol, pk.c, pk.best);
        }
    } else if (r >= tune.out_thr) {
        pk.active = true; pk.best = r; pk.at = m; pk.pol = pol; pk.c = pol > 0 ? cn : ci; pk.age = 0;
    }
}

void P25p2Phy::out_hit(double pos, int pol, kp::cf c, float rho) {
    // pos = centre of the S-ISCH's last symbol (slot symbol 9)
    OutHit h;
    h.t0 = pos - (9 * SPS + SPS / 2);
    h.w = std::arg(c);
    h.pol = pol;
    // angle scale from the sync's own symbols (all +-3 = +-135 degrees)
    float sa = 0;
    const int64_t mi = static_cast<int64_t>(std::llround(pos));
    const kp::cf rot = std::polar(1.0f, -h.w);
    for (int k = 0; k < 20; k++) {
        const int64_t j = mi - static_cast<int64_t>(19 - k) * SPS;
        const kp::cf d = y_[j & RMASK] * std::conj(y_[(j - SPS) & RMASK]) * rot;
        sa += static_cast<float>(pol * S_ISCH[k]) / 3.0f * std::arg(d);
    }
    h.scale = sa / 20 / (0.75f * PI_F);
    if (h.scale < 0.5f || h.scale > 1.6f) return;
    if (out_locked_) {
        // A strong S-ISCH off the grid while the grid has not been confirmed
        // for a while: the sample stream slipped (dropped samples - seen on
        // real captures, often a whole number of symbols, so the symbols
        // still look clean). Start a new grid on it right away instead of
        // waiting for the 1 s timeout.
        double off = std::fmod(h.t0 - ot0_, SLOT);
        if (off < 0) off += SLOT;
        if (std::min(off, SLOT - off) < 6.0 || omiss_ < 3 || rho < 0.85f || h.pol != opol_) return;
        const double t0_old = ot0_;
        const float w_old = ow_, sc_old = oscale_;
        // the new grid starts with the slot before the hit: it is complete
        ot0_ = h.t0 - SLOT;
        ow_ = h.w;
        oscale_ = h.scale;
        if (!lock_check()) {
            ot0_ = t0_old; ow_ = w_old; oscale_ = sc_old;
            return;
        }
        out_lose();
        out_locked_ = true;
        st_.out_locks++;
        oT_ = SPS;
        oper_ = 0;
        onew_ = true;
        omiss_ = 0;
        return;
    }
    // hits older than 600 symbols are of no use
    out_hits_.erase(std::remove_if(out_hits_.begin(), out_hits_.end(),
                                   [&](const OutHit& o) { return h.t0 - o.t0 > 600.0 * SPS; }),
                    out_hits_.end());
    for (const OutHit& o : out_hits_) {
        if (o.pol != h.pol) continue;
        const double dt = h.t0 - o.t0;
        const bool pair = std::fabs(dt - 180.0 * SPS) < 4.0 || std::fabs(dt - 540.0 * SPS) < 6.0;
        if (!pair || std::fabs(wrap_pi(h.w - o.w)) > 0.35f) continue;
        // lock: deliver from the slot after the older S-ISCH - if that
        // slot's symbols sit on the H-DQPSK levels (two noise peaks the
        // right distance apart do not)
        ot0_ = o.t0;
        oT_ = SPS;
        oper_ = 0;
        ow_ = std::arg(std::polar(1.0f, o.w) + std::polar(1.0f, h.w));
        oscale_ = 0.5f * (o.scale + h.scale);
        opol_ = h.pol;
        if (!lock_check()) continue;
        if (in_grid_) in_lose();
        out_locked_ = true;
        st_.out_locks++;
        onew_ = true;
        omiss_ = 0;
        out_hits_.clear();
        in_hits_.clear();
        opk_ = Peak();
        ipk_ = Peak();
        return;
    }
    if (out_hits_.size() >= 12) out_hits_.erase(out_hits_.begin());
    out_hits_.push_back(h);
}

// The slot at ot0_ (current carrier / scale / polarity) sits on the
// H-DQPSK levels
bool P25p2Phy::lock_check() const {
    float soft[OUT_NK], se = 0;
    out_demod(ot0_, soft);
    for (int k = 0; k < P25P2_SLOT_SYMS; k++) {
        const float r = soft[k - OUT_K0] - decide4(soft[k - OUT_K0]);
        se += r * r;
    }
    return std::sqrt(se / P25P2_SLOT_SYMS) <= LOCK_RMS;
}

// Soft symbols OUT_K0..OUT_K1 of the slot whose symbol 0 starts at t0;
// returns the decision-directed error energy over them
float P25p2Phy::out_demod(double t0, float* soft) const {
    const kp::cf rot = std::polar(1.0f, -ow_);
    const float k = static_cast<float>(opol_) / (0.25f * PI_F * oscale_);
    kp::cf prev = yat(t0 + (OUT_K0 - 0.5) * oT_);
    float e = 0;
    for (int i = OUT_K0; i <= OUT_K1; i++) {
        const kp::cf c = yat(t0 + (i + 0.5) * oT_);
        const kp::cf d = c * std::conj(prev) * rot;
        prev = c;
        const float s = std::atan2(d.imag(), d.real()) * k;
        soft[i - OUT_K0] = s;
        const float r = s - decide4(s);
        e += std::min(r * r, 1.0f);
    }
    return e;
}

void P25p2Phy::out_lose() {
    out_locked_ = false;
    out_hits_.clear();
    opk_ = Peak();
    st_.grid_lost++;
    sink_.p2_grid_lost();
}

void P25p2Phy::out_slots() {
    float soft[OUT_NK];
    while (out_locked_ && static_cast<double>(yend_) > ot0_ + (OUT_K1 + 1.0) * oT_ + 6.0) {
        // timing: error energy 1 sample early / on time / late, parabolic
        const float em = out_demod(ot0_ - 1.0, soft);
        const float ep = out_demod(ot0_ + 1.0, soft);
        const float e0 = out_demod(ot0_, soft);
        const float den = em - 2 * e0 + ep;
        float tau = 0;
        if (den > 1e-6f) tau = std::clamp(0.5f * (em - ep) / den, -1.0f, 1.0f);
        else tau = em < ep ? (em < e0 ? -1.0f : 0.0f) : (ep < e0 ? 1.0f : 0.0f);
        if (std::fabs(tau) > 0.05f) out_demod(ot0_ + tau, soft);
        const float* s = soft - OUT_K0;   // s[k], k = -10 .. 189
        // quality, decision-directed carrier and scale over the slot
        float se = 0, sxd = 0, sdd = 0, sres = 0;
        for (int k = 0; k < P25P2_SLOT_SYMS; k++) {
            const float d = decide4(s[k]), r = s[k] - d;
            se += r * r;
            sxd += s[k] * d;
            sdd += d * d;
            sres += r;
        }
        const float rms = std::sqrt(se / P25P2_SLOT_SYMS);
        const float quality = std::clamp(1.0f - rms / 0.577f, 0.0f, 1.0f);   // 0.577 = uniform noise
        // syncs: S-ISCH before (pre + symbols 0..9) or after (170..189) the slot, S-OEMI inside
        const bool synced = pattern_corr(s - 10, S_ISCH, 20) > 0.8f || pattern_corr(s + 170, S_ISCH, 20) > 0.8f ||
                            pattern_corr(s + 79, S_OEMI, 21) > 0.8f;
        P25p2Slot sl;
        sl.inbound = false;
        sl.index = oindex_++;
        sl.grid_new = onew_;
        sl.synced = synced;
        sl.have_pre = true;
        for (int k = 0; k < 10; k++) sl.pre[k] = s[k - 10];
        for (int k = 0; k < P25P2_SLOT_SYMS; k++) sl.sym[k] = s[k];
        const double start = ot0_ + tau;
        sl.start = static_cast<int64_t>(std::llround(start));
        sl.end = static_cast<int64_t>(std::llround(start + P25P2_SLOT_SYMS * oT_));
        sl.quality = quality;
        sl.freq_hz = ow_ * 6000.0f / (2 * PI_F);
        last_freq_ = sl.freq_hz;
        onew_ = false;
        st_.out_slots++;
        // loops; a weak slot (fade) only extrapolates
        if (quality > 0.25f) {
            oper_ = std::clamp(oper_ + 0.04 * tau, -0.5, 0.5);   // samples per slot: sample-clock error
            ot0_ += (synced ? 0.6 : 0.4) * tau;
            const float res = sres / P25P2_SLOT_SYMS * (0.25f * PI_F * oscale_) * static_cast<float>(opol_);
            ow_ = wrap_pi(ow_ + 0.5f * res);
            if (sdd > 0) oscale_ = std::clamp(oscale_ * (1.0f + 0.3f * (sxd / sdd - 1.0f)), 0.5f, 1.6f);
        }
        oT_ = SPS + oper_ / P25P2_SLOT_SYMS;
        ot0_ += P25P2_SLOT_SYMS * oT_;
        omiss_ = synced ? 0 : omiss_ + 1;
        // never a non-finite value to the MAC layer (the input was sanitised;
        // this guards the loops' arithmetic): such a slot is not delivered
        bool finite = p2_finite(sl.freq_hz) && p2_finite(sl.quality);
        for (int k = 0; k < P25P2_SLOT_SYMS && finite; k++) finite = p2_finite(sl.sym[k]);
        for (int k = 0; k < 10 && finite; k++) finite = p2_finite(sl.pre[k]);
        if (finite) sink_.p2_slot(sl);
        if (omiss_ >= OUT_MISS_LIMIT) out_lose();
    }
}

// --- inbound ------------------------------------------------------------------
// The pair sums at a burst's sync positions resemble the sync (a sync whose
// correlation peak the search missed - weak or disturbed - though the burst
// may still be decoded)
bool P25p2Phy::sync_like(double t0, int pol) const {
    float sx = 0, sxx = 0, sxt = 0;
    for (int i = 0; i < 21; i++) {
        const float x = zat(t0 + (7 + i) * SPS + SPS / 2);   // z_6 .. z_26
        sx += x; sxx += x * x; sxt += x * tmpl_[i];
    }
    const float var = sxx - sx * sx / 21;
    return var > 1.0f && static_cast<float>(pol) * sxt / std::sqrt(var) > 0.5f;
}

// Inbound search window: anywhere without a grid; on a confirmed grid only
// where a burst's sync can end (slot symbol 27, +-8 symbols for the radios'
// timing differences) - 11 % of the samples
bool P25p2Phy::in_window(int64_t m) const {
    if (!in_grid_ || inew_) return true;
    double off = std::fmod(static_cast<double>(m) - gref_ - (27 * SPS + SPS / 2), SLOT);
    if (off < 0) off += SLOT;
    return off < 8 * SPS || off > SLOT - 8 * SPS;
}

void P25p2Phy::in_search(int64_t m) {
    const Phase& ph = phase_[m & 7];
    const float* zz = ph.z + ph.pos + PW - 21;   // z at the last 21 symbol spacings, oldest first
    float a[4] = {}, b[4] = {}, c[4] = {};
    for (int i = 0; i < 20; i += 4)
        for (int j = 0; j < 4; j++) {
            const float x = zz[i + j];
            a[j] += x;
            b[j] += x * x;
            c[j] += x * tmpl_[i + j];
        }
    const float x20 = zz[20];
    const float sx = a[0] + a[1] + a[2] + a[3] + x20;
    const float sxx = b[0] + b[1] + b[2] + b[3] + x20 * x20;
    const float sxt = c[0] + c[1] + c[2] + c[3] + x20 * tmpl_[20];
    const float var = sxx - sx * sx / 21;
    const float rho = var > 1.0f ? sxt / std::sqrt(var) : 0.0f;
    const float r = std::fabs(rho);
    irr_[m & 15] = r;
    Peak& pk = ipk_;
    if (pk.active) {
        if (r > pk.best) { pk.best = r; pk.at = m; pk.pol = rho > 0 ? 1 : -1; pk.age = 0; }
        else if (++pk.age >= SPS / SEARCH_STEP) {
            pk.active = false;
            const double off = SEARCH_STEP * vertex(irr_[(pk.at - SEARCH_STEP) & 15], pk.best, irr_[(pk.at + SEARCH_STEP) & 15]);
            st_.in_hits++;
            in_hit(static_cast<double>(pk.at) + off, pk.pol, pk.best);
        }
    } else if (r >= tune.in_thr) {
        pk.active = true; pk.best = r; pk.at = m; pk.pol = rho > 0 ? 1 : -1; pk.age = 0;
    }
}

void P25p2Phy::in_hit(double pos, int pol, float rho) {
    // pos = centre of symbol 27 (the sync's last): its pair sum z_26 ends there
    InHit h;
    h.t0 = pos - (27 * SPS + SPS / 2);
    h.pol = pol;
    h.rho = rho;
    // least-squares level fit of the sync: z = c + pol * scale * H1 * t
    float sx = 0, st = 0, stt = 0, sxt = 0;
    for (int i = 0; i < 21; i++) {
        const float x = zat(pos - (20 - i) * SPS), t = tmpl_raw_[i];
        sx += x; st += t; stt += t * t; sxt += x * t;
    }
    const float slope = (21 * sxt - st * sx) / (21 * stt - st * st);
    h.scale = slope * static_cast<float>(pol) / H1;
    h.center = (sx - slope * st) / 21;
    if (h.scale < 0.4f || h.scale > 2.5f) { st_.in_rejected++; return; }
    // one hit per burst: the correlation's side peaks (a symbol or two off)
    // give way to the main one
    for (InHit& o : in_hits_) {
        if (std::fabs(o.t0 - h.t0) < 3 * SPS) {
            if (h.rho > o.rho) o = h;
            return;
        }
    }
    if (in_hits_.size() >= 16) in_hits_.erase(in_hits_.begin());
    in_hits_.push_back(h);
    if (!in_grid_) in_start_grid(h);
}

// A tentative grid on a hit; it stays once a burst on it is delivered
void P25p2Phy::in_start_grid(const InHit& h) {
    in_grid_ = true;
    inew_ = true;
    gref_ = h.t0;
    gnext_ = 0;
    glast_sync_ = glast_good_ = 0;
    for (auto& t : tracks_) t = Track();
}

void P25p2Phy::in_lose() {
    const bool delivered = !inew_;
    in_grid_ = false;
    in_hits_.clear();
    for (auto& t : tracks_) t = Track();
    if (delivered) {
        gbase_ += gnext_ + 1;
        st_.grid_lost++;
        sink_.p2_grid_lost();
    }
}

// Pair sums z_n (n = IN_K0 - 1 .. IN_K1) of a burst at t0: the phase change
// from the centre of symbol n to that of n + 1, in Hz
void P25p2Phy::in_obs(double t0, float* z) const {
    for (int n = IN_K0 - 1; n <= IN_K1; n++) z[n - (IN_K0 - 1)] = zat(t0 + (n + 1) * SPS + SPS / 2);
}

// Sequence detector over symbols IN_K0..IN_K1. State = (I_k-2, I_k-1), the
// two symbols on the main taps of the observation z_k-2 = H1 (I_k-2 + I_k-1)
// + H0 (I_k-3 + I_k): the branch symbol I_k enters through H0, I_k-3 comes
// from each survivor's own history (decision feedback). Known pilots /
// sync start and end the trellis. Measured against the full 64-state
// trellis over (I_k-3 .. I_k): the same symbol error rate within the
// statistics at 8-16 dB, at a quarter of the work. (A 4-state trellis over
// I_k-1 alone, with I_k-2 as a decision, lost 50x: a main-tap symbol must
// not be fed back.)
float P25p2Phy::detect(double t0, float center, float scale, int pol, bool synced, int8_t* dec) {
    float z[IN_NZ];
    in_obs(t0, z);
    int8_t known[P25P2_SLOT_SYMS] = {};
    for (int k = 0; k < 4; k++) known[IN_K0 + k] = P1[k];
    if (synced) for (int k = 0; k < 22; k++) known[IN_K0 + k] = S_IN[k];
    for (int k = 0; k < 4; k++) known[170 + k] = P2[k];
    const int kf = synced ? IN_K0 + 22 : IN_K0 + 4;   // first unknown symbol
    const float g = static_cast<float>(pol) * scale;
    const float inv = 1.0f / (H1 * scale);
    static constexpr float LV[4] = {-3, -1, 1, 3};
    auto idx = [](int8_t v) { return (v + 3) >> 1; };
    constexpr float INF = 1e30f;
    float pm[16], nm[16];
    int8_t hist[16], nh[16];   // survivor's I_k-3
    for (int s = 0; s < 16; s++) { pm[s] = INF; hist[s] = 0; }
    const int s0 = (idx(known[kf - 2]) << 2) | idx(known[kf - 1]);
    pm[s0] = 0;
    hist[s0] = known[kf - 3];
    int nobs = 0;
    for (int k = kf; k <= IN_K1; k++) {
        const float zz = z[k - 2 - (IN_K0 - 1)] - center;
        const int d0 = known[k] ? idx(known[k]) : 0, d1 = known[k] ? d0 : 3;
        for (int s = 0; s < 16; s++) nm[s] = INF;
        uint8_t* sv = &surv_[static_cast<size_t>(k) * 16];
        for (int s = 0; s < 16; s++) {
            if (pm[s] >= INF) continue;
            const float base = H1 * (LV[s >> 2] + LV[s & 3]) + H0 * hist[s];
            for (int d = d0; d <= d1; d++) {
                float e = (zz - g * (base + H0 * LV[d])) * inv;
                e = std::min(e * e, 4.0f);
                const float v = pm[s] + e;
                const int ns = ((s & 3) << 2) | d;
                if (v < nm[ns]) { nm[ns] = v; sv[ns] = static_cast<uint8_t>(s); nh[ns] = static_cast<int8_t>(LV[s >> 2]); }
            }
        }
        for (int s = 0; s < 16; s++) { pm[s] = nm[s]; hist[s] = nh[s]; }
        nobs++;
    }
    int s = (idx(known[IN_K1 - 1]) << 2) | idx(known[IN_K1]);
    const float total = pm[s];
    if (dec) {
        for (int k = IN_K1; k >= kf; k--) {
            dec[k] = static_cast<int8_t>(LV[s & 3]);
            s = surv_[static_cast<size_t>(k) * 16 + s];
        }
        for (int k = IN_K0; k < kf; k++) dec[k] = known[k];
    }
    return nobs > 0 ? total / static_cast<float>(nobs) : INF;
}

// Cheap score of a candidate burst position (no decisions): a 4-state
// trellis over the main taps only, z_k-1 = H1 (I_k-1 + I_k); the pilots at
// both ends force it. Mean squared residual in level units.
float P25p2Phy::score(double t0, float center, float scale, int pol) const {
    float z[IN_NZ];
    in_obs(t0, z);
    const float g = static_cast<float>(pol) * scale * H1;
    const float inv = 1.0f / (H1 * scale);
    static constexpr float LV[4] = {-3, -1, 1, 3};
    constexpr float INF = 1e30f;
    float pm[4] = {INF, INF, INF, INF}, nm[4];
    pm[(P1[3] + 3) >> 1] = 0;
    const int kf = IN_K0 + 4;
    for (int k = kf; k <= IN_K1; k++) {
        const float zz = z[k - 1 - (IN_K0 - 1)] - center;
        const bool kn = k >= 170;
        const int d0 = kn ? (P2[k - 170] + 3) >> 1 : 0, d1 = kn ? d0 : 3;
        nm[0] = nm[1] = nm[2] = nm[3] = INF;
        for (int s = 0; s < 4; s++) {
            if (pm[s] >= INF) continue;
            for (int d = d0; d <= d1; d++) {
                float e = (zz - g * (LV[s] + LV[d])) * inv;
                const float v = pm[s] + std::min(e * e, 4.0f);
                if (v < nm[d]) nm[d] = v;
            }
        }
        for (int s = 0; s < 4; s++) pm[s] = nm[s];
    }
    return pm[(P2[3] + 3) >> 1] / static_cast<float>(IN_K1 - kf + 1);
}

// A burst of H-CPM, not a carrier / analogue FM / another modulation the
// trellis happens to fit: the spread of z must match the fitted deviation
// (random symbols: rms pair sum sqrt(10) = 3.2 level units) and the decided
// pair sums must not be mostly zero (a plain carrier fits +1 -1 +1 ...)
bool P25p2Phy::in_plausible(double t0, const int8_t* dec, float center, float scale) const {
    float z[IN_NZ];
    in_obs(t0, z);
    float sq = 0;
    int nz = 0, n = 0;
    for (int i = IN_K0 + 4; i <= 168; i++) {
        const float v = z[i - (IN_K0 - 1)] - center;
        sq += v * v;
        if (dec[i] + dec[i + 1] != 0) nz++;
        n++;
    }
    const float spread = std::sqrt(sq / static_cast<float>(n)) / (H1 * scale);
    return spread > 2.0f && spread < 4.5f && nz > n / 2;
}

// Least-squares refit of carrier (centre) and scale to decided symbols,
// robust: pair sums more than 1.5 level units off the current model (noise
// clicks of the discriminator, wrong decisions) are left out. Returns the
// rms residual in level units, each residual clipped at 2 like the
// detector's metric.
float P25p2Phy::in_fit(double t0, const int8_t* dec, int pol, float& center, float& scale) const {
    float z[IN_NZ], md[IN_NZ];
    in_obs(t0, z);
    const float g = static_cast<float>(pol) * scale, lim = 1.5f * H1 * scale;
    float sx = 0, sm = 0, smm = 0, sxm = 0;
    int n = 0;
    for (int i = IN_K0 + 1; i <= IN_K1 - 2; i++) {
        const int j = i - (IN_K0 - 1);
        md[j] = H0 * static_cast<float>(dec[i - 1] + dec[i + 2]) + H1 * static_cast<float>(dec[i] + dec[i + 1]);
        if (std::fabs(z[j] - center - g * md[j]) > lim) continue;
        sx += z[j]; sm += md[j]; smm += md[j] * md[j]; sxm += z[j] * md[j];
        n++;
    }
    const float fn = static_cast<float>(n);
    const float den = fn * smm - sm * sm;
    if (n < 40 || den <= 0) return 1e3f;
    const float slope = (fn * sxm - sm * sx) / den;
    const float c = (sx - slope * sm) / fn;
    const float s = slope * static_cast<float>(pol);
    if (s > 0.3f && s < 3.0f) { center = c; scale = s; }
    const float gg = static_cast<float>(pol) * scale, inv = 1.0f / (H1 * scale);
    float res = 0;
    int nn = 0;
    for (int i = IN_K0 + 1; i <= IN_K1 - 2; i++) {
        const int j = i - (IN_K0 - 1);
        const float r = (z[j] - center - gg * md[j]) * inv;
        res += std::min(r * r, 4.0f);
        nn++;
    }
    return std::sqrt(res / static_cast<float>(nn));
}

float P25p2Phy::in_burst(double& t0, float& center, float& scale, int pol, bool synced, int8_t* dec) {
    const float m0 = detect(t0, center, scale, pol, synced, dec);
    // decision-directed timing: model residual at -2 / 0 / +2 samples
    float r[3];
    for (int j = 0; j < 3; j++) {
        float c = center, s = scale;
        const float rr = in_fit(t0 + (j - 1) * 2.0, dec, pol, c, s);
        r[j] = rr * rr;
    }
    double t1 = t0;
    float c1 = center, s1 = scale;
    const float den = r[0] - 2 * r[1] + r[2];
    if (den > 1e-9f) t1 += std::clamp(static_cast<double>(r[0] - r[2]) / den, -2.0, 2.0);   // 2 samples per step
    in_fit(t1, dec, pol, c1, s1);
    // a second pass only when the refit moved something that matters
    if (std::fabs(t1 - t0) < 0.25 && std::fabs(c1 - center) < 0.03f * H1 * scale && std::fabs(s1 - scale) < 0.02f * scale) {
        center = c1; scale = s1;
        float c = center, s = scale;
        return in_fit(t0, dec, pol, c, s);
    }
    int8_t d1[P25P2_SLOT_SYMS];
    const float m1 = detect(t1, c1, s1, pol, synced, d1);
    if (m1 < m0) {   // keep the refit only when the detector agrees
        t0 = t1; center = c1; scale = s1;
        std::copy(d1 + IN_K0, d1 + IN_K1 + 1, dec + IN_K0);
    }
    float c = center, s = scale;
    return in_fit(t0, dec, pol, c, s);
}

void P25p2Phy::in_slots() {
    // a slot is examined once a burst anywhere in it (a sync hit up to half
    // a slot off the nominal start) is complete
    while (in_grid_ && static_cast<double>(yend_) > gref_ + (gnext_ + 1.5) * SLOT + 5 * SPS + 16) {
        double nominal = gref_ + static_cast<double>(gnext_) * SLOT;
        // the best sync hit inside this slot
        int best = -1;
        for (size_t i = 0; i < in_hits_.size(); i++)
            if (std::fabs(in_hits_[i].t0 - nominal) < SLOT / 2 && (best < 0 || in_hits_[i].rho > in_hits_[best].rho))
                best = static_cast<int>(i);
        int8_t dec[P25P2_SLOT_SYMS] = {};
        bool have = false, synced = false;
        double t0 = nominal;
        float center = 0, scale = 1, rms = 1e3f;
        int pol = 1;
        if (best >= 0) {
            const InHit h = in_hits_[best];
            t0 = h.t0; center = h.center; scale = h.scale; pol = h.pol;
            rms = in_burst(t0, center, scale, pol, true, dec);
            // the first burst of a grid must fit better (nothing else vouches for it)
            if (rms <= (inew_ ? std::min(tune.rms_synced, RMS_NEW_GRID) : tune.rms_synced) && in_plausible(t0, dec, center, scale)) have = synced = true;
            else st_.in_rejected++;
        }
        if (!have && !inew_) {
            // No sync: a voice burst on the grid? First each recent radio
            // track's timing, carrier and scale; the grid timing +-3 symbols
            // with a blind carrier only if no track fits (an integer symbol
            // shift is hard to see: the alternating pilots look alike
            // shifted by two symbols).
            struct Cand { double t0; float c, s; int pol; const Track* tr; };
            Cand bc{};
            float bm = 1e30f;
            int lpol = 1;
            float lscale = 1;
            int64_t newest = -1;
            for (const Track& t : tracks_) {
                if (!t.live || gnext_ - t.last > 40) continue;
                if (t.last > newest) { newest = t.last; lpol = t.pol; lscale = t.scale; }
                for (int j = -1; j <= 1; j++) {
                    const Cand c{nominal + t.off + 2.0 * j, t.center, t.scale, t.pol, &t};
                    const float mm = score(c.t0, c.c, c.s, c.pol);
                    if (mm < bm) { bm = mm; bc = c; }
                }
            }
            if (bm > TRACK_FIT) {
                for (int sh = -3; sh <= 3; sh++) {
                    for (int j = -1; j <= 1; j += 2) {
                        const double tc = nominal + sh * SPS + 2.0 * j;
                        // blind carrier: the mean pair sum over the burst
                        // (random data averages to ~0, the pilots sit at 0)
                        float z[IN_NZ];
                        in_obs(tc, z);
                        float sm = 0, sq = 0;
                        for (int i = 0; i < IN_NZ; i++) sm += z[i];
                        sm /= IN_NZ;
                        for (int i = 0; i < IN_NZ; i++) sq += (z[i] - sm) * (z[i] - sm);
                        const float blind_s = std::sqrt(sq / IN_NZ) / (H1 * std::sqrt(10.0f));
                        if (blind_s < 0.3f || blind_s > 3.0f) continue;
                        Cand c{tc, sm, lscale, lpol, nullptr};
                        float mm = score(c.t0, c.c, c.s, c.pol);
                        if (mm < bm) { bm = mm; bc = c; }
                        if (std::fabs(blind_s - lscale) > 0.15f * lscale) {
                            c.s = blind_s;
                            mm = score(c.t0, c.c, c.s, c.pol);
                            if (mm < bm) { bm = mm; bc = c; }
                        }
                    }
                }
            }
            if (bm < 1.0f) {
                t0 = bc.t0; center = bc.c; scale = bc.s; pol = bc.pol;
                rms = in_burst(t0, center, scale, pol, false, dec);
                // the sync may be there but its correlation missed (noise):
                // try that hypothesis too
                double t1 = bc.t0;
                float c1 = bc.c, s1 = bc.s;
                int8_t d1[P25P2_SLOT_SYMS] = {};
                const float r1 = rms > 0.25f && sync_like(t0, pol) ? in_burst(t1, c1, s1, pol, true, d1) : 1e3f;
                if (r1 < rms - 0.05f && r1 <= tune.rms_synced && in_plausible(t1, d1, c1, s1)) {
                    t0 = t1; center = c1; scale = s1; rms = r1;
                    std::copy(d1, d1 + P25P2_SLOT_SYMS, dec);
                    have = synced = true;
                } else if (in_plausible(t0, dec, center, scale)) {
                    // a burst found on the grid alone must fit well, and one on
                    // a radio track must also keep that radio's carrier, scale
                    // and timing (noise fits come out with a far larger scale)
                    if (bc.tr) {
                        have = rms <= tune.rms_unsynced && std::fabs(center - bc.tr->center) < 200.0f &&
                               std::fabs(scale / bc.tr->scale - 1.0f) < 0.2f &&
                               std::fabs((t0 - nominal) - bc.tr->off) < 4.0;
                    } else {
                        // ... and be unambiguous: one or two symbols either
                        // side must fit clearly worse
                        float worst = 1e30f;
                        for (int sh = -2; sh <= 2; sh++)
                            if (sh) worst = std::min(worst, score(t0 + sh * SPS, center, scale, pol));
                        have = rms <= tune.rms_blind && score(t0, center, scale, pol) < 0.6f * worst;
                    }
                }
            }
        }
        if (have) {
            P25p2Slot sl;
            sl.inbound = true;
            sl.index = gbase_ + gnext_;
            sl.grid_new = inew_;
            sl.synced = synced;
            sl.have_pre = false;
            for (int k = IN_K0; k <= IN_K1; k++) sl.sym[k] = static_cast<float>(dec[k]);   // upright symbols
            sl.start = static_cast<int64_t>(std::llround(t0));
            sl.end = sl.start + P25P2_SLOT_SAMPLES;
            sl.quality = std::clamp(1.0f - rms / 1.2f, 0.0f, 1.0f);
            sl.freq_hz = center;
            last_freq_ = center;
            if (inew_) {
                // the grid starts on the first burst delivered (the hit that
                // opened it may have been a false one nearby)
                st_.in_grids++;
                gref_ = t0 - static_cast<double>(gnext_) * SLOT;
                nominal = t0;
            }
            inew_ = false;
            glast_good_ = gnext_;
            if (synced) {
                glast_sync_ = gnext_;
                const double e = t0 - nominal;
                // the grid follows the sample clock slowly; the radios' own
                // timing differences (a few symbols) live in their tracks
                if (std::fabs(e) < 6 * SPS) gref_ += 0.02 * e;
                st_.in_synced++;
            } else {
                st_.in_unsynced++;
            }
            // radio tracks: the same carrier (+-150 Hz) and timing (+-3 symbols)
            Track* tr = nullptr;
            for (Track& t : tracks_)
                if (t.live && std::fabs(t.center - center) < 150.0f && std::fabs(t.off - (t0 - nominal)) < 3 * SPS) { tr = &t; break; }
            if (!tr) {
                tr = &tracks_[0];
                for (Track& t : tracks_) {
                    if (!t.live) { tr = &t; break; }
                    if (t.last < tr->last) tr = &t;
                }
                *tr = Track();
                tr->live = true;
                tr->off = t0 - nominal;
                tr->center = center;
                tr->scale = scale;
            }
            // the timing only from bursts with a sync (unambiguous: a voice
            // burst can fit one symbol off, and must not drag the track there)
            if (synced) tr->off += 0.7 * ((t0 - nominal) - tr->off);
            tr->center += 0.5f * (center - tr->center);
            tr->scale += 0.5f * (scale - tr->scale);
            tr->pol = pol;
            tr->last = gnext_;
            if (p2_finite(sl.freq_hz) && p2_finite(sl.quality)) sink_.p2_slot(sl);
        } else if (inew_) {
            // the hit that started the grid held no burst: drop it; a later
            // hit may start the next grid
            in_grid_ = false;
            in_hits_.erase(std::remove_if(in_hits_.begin(), in_hits_.end(),
                                          [&](const InHit& x) { return x.t0 < nominal + SLOT / 2; }),
                           in_hits_.end());
            if (!in_hits_.empty()) in_start_grid(in_hits_.front());
            continue;
        }
        in_hits_.erase(std::remove_if(in_hits_.begin(), in_hits_.end(),
                                      [&](const InHit& x) { return x.t0 < nominal + SLOT / 2; }),
                       in_hits_.end());
        gnext_++;
        if ((gnext_ - glast_sync_ > IN_SYNC_LIMIT && gnext_ - glast_good_ > IN_GOOD_LIMIT) ||
            gnext_ - glast_sync_ > 3 * IN_SYNC_LIMIT)
            in_lose();
    }
}

}  // namespace dig
