// Sub-audible signalling receiver (rx.hpp): front end, CTCSS, DCS.
//
// Front end (24 kHz complex in): 41-tap channel filter (+-7.5 kHz), FM
// discriminator (Hz), 24 kHz -> 2400 Hz -> 1200 Hz (Blackman FIRs: flat to
// ~350 Hz, the voice band above folded nowhere near 0-300 Hz). Two streams:
// a 45 Hz high-passed one for CTCSS (no carrier offset) and a 130-290 Hz
// low-passed one for the DCS bits.
//
// CTCSS: every 50 ms a Hann-windowed 0.3 s FFT (2048 points, 0.59 Hz bins)
// of 55-275 Hz (only what follows a carrier's onset inside the window); a
// candidate = the strongest peak 60-257 Hz >= 13 dB above the band's
// median, holding >= 55 % of the band's power (one pure tone, not voice /
// noise / DCS's line spectrum), 80 Hz .. 1.5 kHz of deviation, not one line
// of a periodic signal's family (f +- f/k). A tone = 3 consecutive
// candidates (2 when strong) agreeing in frequency and amplitude, its phase
// linear over 0.2 s; one not at a carrier onset only after 0.8 s (a voice
// harmonic holds still for less). Then a narrowband tracker (mixed down,
// 97-tap 35 Hz low-pass, every 10 ms) follows its amplitude / phase - the
// frequency (measured to ~0.01 Hz from the whole phase advance), the end
// (amplitude below half: "tone off"; the carrier gone), a reverse burst (the
// phase turning > 100° while the amplitude and carrier are there, confirmed
// by the end following within 0.5 s), another transmitter taking over
// (another tone dominating the FFT; the end put at its carrier's rise). The
// start is found back in the stream: the carrier's onset, or where the
// tracker's amplitude rose through half of the tone's.
//
// DCS: zero-crossing clock recovery (134.4 bit/s +-3 %) on the low-passed
// stream, decision-directed levels (TX droop, carrier offset), hard bits.
// A code = the last 46 bits 23-periodic without an error and a Golay
// codeword (~1 false lock in years of noise), or 69 bits whose 3-word
// majority is a codeword with <= 5 bits disagreeing (weak signals). Locked:
// every bit checked against the code; > 6 of the last 23 wrong = it ended
// (at the last bit of a clean run); the 134.4 Hz turn-off tone after it is
// recognised. The start is found back in the bits as for CTCSS.

#include "rx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kraken_dsp.hpp"

namespace ctcss {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr int NFFT = 2048;
constexpr int WIN = 360;                 // 0.3 s
constexpr int HOP = 60;                  // 50 ms
constexpr int TICK = 12;                 // 10 ms
constexpr int TRK_TAPS = 97;             // tracker low-pass (centre = 48 samples back)
constexpr int TRK_HALF = TRK_TAPS / 2;
constexpr int DCS_TAPS = 41;
constexpr int DCS_DELAY = DCS_TAPS / 2;
constexpr int D_IN = 20 + 40 + 15 * 10;  // input-sample delay of the sub-audio stream (channel + A + B)
constexpr double BIN_HZ = FS_SUB / NFFT;
constexpr float SNR_MIN_DB = 13.0f;
constexpr float PURITY_MIN = 0.55f;
constexpr float MAX_DEV_HZ = 1500.0f;
constexpr double DCS_BAUD = 134.4;

inline float wrapf(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

// symmetric FIR over the newest n samples of a doubled ring
inline float fir_ring(const float* ring_newest_end, const std::vector<float>& h) {
    const float* x = ring_newest_end - h.size() + 1;
    float acc = 0;
    for (size_t i = 0; i < h.size(); i++) acc += h[i] * x[i];
    return acc;
}
}  // namespace

Rx::Rx() {
    h_ch_ = kp::fir_lowpass(41, 7500, static_cast<float>(FS_IN));
    h_a_ = kp::fir_lowpass(81, 1200, static_cast<float>(FS_IN));
    h_b_ = kp::fir_lowpass(31, 560, 2400);
    h_dcs_ = kp::fir_lowpass(DCS_TAPS, 210, static_cast<float>(FS_SUB));
    h_trk_ = kp::fir_lowpass(TRK_TAPS, 35, static_cast<float>(FS_SUB));
    {
        double s1 = 0, s2 = 0;
        for (float v : h_trk_) {
            s1 += v;
            s2 += static_cast<double>(v) * v;
        }
        trk_enbw_ = FS_SUB * s2 / (s1 * s1);
    }
    ch_ring_.assign(2 * h_ch_.size(), kp::cf(0, 0));
    a_ring_.assign(2 * h_a_.size(), 0.0f);
    b_ring_.assign(2 * h_b_.size(), 0.0f);
    dc_ring_.assign(2 * h_dcs_.size(), 0.0f);
    hp_.assign(RING, 0.0f);
    y_.assign(RING, 0.0f);
    cpw_.assign(RING, 0.0f);
    cblk_.assign(64, 0.0f);
    // 2nd-order Butterworth high-pass, 45 Hz at 1200 Hz (bilinear)
    const double K = std::tan(PI * 45.0 / FS_SUB), nm = 1.0 / (1.0 + std::sqrt(2.0) * K + K * K);
    hp_b_[0] = nm;
    hp_b_[1] = -2 * nm;
    hp_b_[2] = nm;
    hp_a_[0] = 1;
    hp_a_[1] = 2 * (K * K - 1) * nm;
    hp_a_[2] = (1 - std::sqrt(2.0) * K + K * K) * nm;
    win_.resize(WIN);
    win_sum_ = 0;
    for (int i = 0; i < WIN; i++) {
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / WIN));
        win_sum_ += win_[i];
    }
    fft_in_ = fftwf_alloc_real(NFFT);
    fft_out_ = fftwf_alloc_complex(NFFT / 2 + 1);
    plan_ = fftwf_plan_dft_r2c_1d(NFFT, fft_in_, fft_out_, FFTW_ESTIMATE);
    reset(0);
}

Rx::~Rx() {
    if (plan_) fftwf_destroy_plan(plan_);
    if (fft_in_) fftwf_free(fft_in_);
    if (fft_out_) fftwf_free(fft_out_);
}

void Rx::reset(double t0) {
    t0_ = t0;
    nin_ = 0;
    std::fill(ch_ring_.begin(), ch_ring_.end(), kp::cf(0, 0));
    std::fill(a_ring_.begin(), a_ring_.end(), 0.0f);
    std::fill(b_ring_.begin(), b_ring_.end(), 0.0f);
    std::fill(dc_ring_.begin(), dc_ring_.end(), 0.0f);
    std::fill(hp_.begin(), hp_.end(), 0.0f);
    std::fill(y_.begin(), y_.end(), 0.0f);
    std::fill(cpw_.begin(), cpw_.end(), 0.0f);
    std::fill(cblk_.begin(), cblk_.end(), 0.0f);
    cacc_ = 0;
    cacc_n_ = 0;
    cblk_n_ = 0;
    cl_ref_ = 0;
    cl_low_ = 0;
    d_name_ = DcsName{};
    ch_pos_ = a_pos_ = b_pos_ = dc_pos_ = 0;
    a_phase_ = b_phase_ = 0;
    prev_ = kp::cf(1, 0);
    hp_z_[0] = hp_z_[1] = 0;
    j_ = 0;
    hops_.clear();
    cs_ = CState::IDLE;
    ticks_.clear();
    dip_k_ = -1;
    block_j_ = 0;
    switch_n_ = 0;
    last_end_j_ = -1000000;
    tick_n_ = 0;
    d_tau_ = 0;
    d_sps_ = FS_SUB / DCS_BAUD;
    d_hi_ = 300;
    d_lo_ = -300;
    d_mean_ = 0;
    d_have_prev_ = false;
    bits_.clear();
    bit_n_ = 0;
    d_locked_ = false;
    d_end_j_ = -1000000;
    d_turnoff_ = 0;
    events.clear();
}

float Rx::carrier_avg(int64_t jc, int half) const {
    double a = 0;
    int n = 0;
    for (int64_t j = jc - half; j <= jc + half; j++) {
        if (j < 0 || j >= j_ || j < j_ - RING + 1) continue;
        a += cpw_[j & (RING - 1)];
        n++;
    }
    return n ? static_cast<float>(a / n) : 0.0f;
}

// where the carrier came up, searching back from jc (sub-audio index) to
// lo: the level of the 0.1 s before jc against the level going back; -1 =
// no rise found (it was there all along)
int64_t Rx::carrier_onset(int64_t jc0, int64_t lo) const {
    const float cref = carrier_avg(jc0 - 60, 60);
    if (!(cref > 0)) return -1;
    for (int64_t jc = jc0; jc >= lo + 12 && jc >= j_ - RING + 20; jc -= 6) {
        if (carrier_avg(jc, 12) < 0.5f * cref && carrier_avg(jc - 12, 12) < 0.5f * cref) {
            // to the sample: the first one back up at half the level
            int64_t c = jc;
            while (c < jc0 && carrier_avg(c, 3) < 0.5f * cref) c++;
            return c;
        }
    }
    return -1;
}

double Rx::time_of(double j) const { return t0_ + (j * DECIM + (DECIM - 1) - D_IN) / FS_IN; }

// ---------------------------------------------------------------------------
// front end
void Rx::process(const kp::cf* x, size_t n) {
    const size_t nc = h_ch_.size(), na = h_a_.size(), nb = h_b_.size();
    for (size_t i = 0; i < n; i++) {
        ch_ring_[ch_pos_] = ch_ring_[ch_pos_ + nc] = x[i];
        const kp::cf* w = &ch_ring_[ch_pos_ + 1];
        float re = 0, im = 0;
        for (size_t k = 0; k < nc; k++) {
            re += h_ch_[k] * w[k].real();
            im += h_ch_[k] * w[k].imag();
        }
        ch_pos_ = ch_pos_ + 1 == nc ? 0 : ch_pos_ + 1;
        const kp::cf y(re, im);
        cacc_ += std::norm(y);
        if (++cacc_n_ == DECIM) {   // one carrier power per 20 input samples
            cblk_[cblk_n_ & 63] = static_cast<float>(cacc_ / DECIM);
            cblk_n_++;
            cacc_ = 0;
            cacc_n_ = 0;
        }
        const kp::cf d = y * std::conj(prev_);
        prev_ = y;
        float hz = 0;
        if (std::norm(d) > 1e-30f) hz = std::atan2(d.imag(), d.real()) * static_cast<float>(FS_IN / (2 * PI));
        if (p.invert) hz = -hz;
        a_ring_[a_pos_] = a_ring_[a_pos_ + na] = hz;
        const float* aend = &a_ring_[a_pos_ + na];
        a_pos_ = a_pos_ + 1 == na ? 0 : a_pos_ + 1;
        nin_++;
        if (++a_phase_ < 10) continue;
        a_phase_ = 0;
        const float v = fir_ring(aend, h_a_);
        b_ring_[b_pos_] = b_ring_[b_pos_ + nb] = v;
        const float* bend = &b_ring_[b_pos_ + nb];
        b_pos_ = b_pos_ + 1 == nb ? 0 : b_pos_ + 1;
        if (++b_phase_ < 2) continue;
        b_phase_ = 0;
        sub_sample(fir_ring(bend, h_b_));
    }
}

void Rx::sub_sample(float s) {
    const int64_t j = j_;
    // CTCSS path: high-pass (carrier offset out)
    const double v = hp_b_[0] * s + hp_z_[0];
    hp_z_[0] = hp_b_[1] * s - hp_a_[1] * v + hp_z_[1];
    hp_z_[1] = hp_b_[2] * s - hp_a_[2] * v;
    hp_[j & (RING - 1)] = static_cast<float>(v);
    // DCS path: low-pass
    const size_t nd = h_dcs_.size();
    dc_ring_[dc_pos_] = dc_ring_[dc_pos_ + nd] = s;
    const float* dend = &dc_ring_[dc_pos_ + nd];
    dc_pos_ = dc_pos_ + 1 == nd ? 0 : dc_pos_ + 1;
    y_[j & (RING - 1)] = fir_ring(dend, h_dcs_);
    // the carrier power at the time sub-audio sample j stands for (the
    // decimation filters delay it by D_IN input samples = ~10.5 blocks)
    const int64_t cb = cblk_n_ - 1 - (D_IN / DECIM);
    cpw_[j & (RING - 1)] = cb >= 0 ? cblk_[cb & 63] : 0.0f;
    j_++;
    if (j_ % TICK == 0 && cs_ != CState::IDLE) ctcss_tick();
    if (j_ % HOP == 0 && j_ >= WIN) ctcss_hop();
    if (j_ > DCS_TAPS) dcs_sample(j);
}

// ---------------------------------------------------------------------------
// CTCSS
Rx::Spec Rx::analyse(int64_t j_end) {
    Spec s;
    s.j_end = j_end;
    // a carrier that came up inside the window: only the part after it (the
    // noise before it would hide a new tone for a whole window)
    int L = WIN;
    const float cnow = carrier_avg(j_end - 30, 30);
    for (int64_t j = j_end - 60; j >= j_end - WIN + 6; j -= 6) {
        if (carrier_avg(j, 6) < 0.5f * cnow) {
            L = static_cast<int>(j_end - j) - 6;
            break;
        }
    }
    if (last_end_j_ > j_end - L) L = static_cast<int>(j_end - last_end_j_);   // nor the last transmission
    s.len = L;
    if (L < 150) {   // under 0.125 s: not yet
        if (dump) fprintf(stderr, "HOP t=%.3f (window %d)\n", time_of(static_cast<double>(j_end)), L);
        return s;
    }
    double mean = 0;
    for (int i = 0; i < L; i++) mean += hp_[(j_end - L + 1 + i) & (RING - 1)];
    mean /= L;
    float wsum = win_sum_;
    if (L == WIN) {
        for (int i = 0; i < L; i++)
            fft_in_[i] = static_cast<float>((hp_[(j_end - L + 1 + i) & (RING - 1)] - mean) * win_[i]);
    } else {
        wsum = 0;
        for (int i = 0; i < L; i++) {
            const float w = static_cast<float>(0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / L));
            wsum += w;
            fft_in_[i] = static_cast<float>((hp_[(j_end - L + 1 + i) & (RING - 1)] - mean) * w);
        }
    }
    std::fill(fft_in_ + L, fft_in_ + NFFT, 0.0f);
    fftwf_execute(plan_);
    const int k55 = static_cast<int>(55 / BIN_HZ), k275 = static_cast<int>(275 / BIN_HZ);
    // tones 60-257 Hz (the highest listed is 254.1; above it only voice leaks
    // in, through the transmitters' 300 Hz high-pass)
    const int k60 = static_cast<int>(std::ceil(60 / BIN_HZ)), k262 = static_cast<int>(257 / BIN_HZ);
    const int k30 = static_cast<int>(30 / BIN_HZ), k560 = static_cast<int>(560 / BIN_HZ);
    float pw[NFFT / 2 + 1];
    double band = 0;
    int kb = k60;
    for (int k = k30; k <= k560; k++) pw[k] = fft_out_[k][0] * fft_out_[k][0] + fft_out_[k][1] * fft_out_[k][1];
    for (int k = k55; k <= k275; k++) {
        band += pw[k];
        if (k >= k60 && k <= k262 && pw[k] > pw[kb]) kb = k;
    }
    med_.assign(pw + k55, pw + k275 + 1);
    std::nth_element(med_.begin(), med_.begin() + med_.size() / 2, med_.end());
    const float floor = std::max(med_[med_.size() / 2] / 0.6931f, 1e-12f);
    // parabolic interpolation on the log power
    const float l0 = std::log(std::max(pw[kb - 1], 1e-20f)), l1 = std::log(std::max(pw[kb], 1e-20f)),
                l2 = std::log(std::max(pw[kb + 1], 1e-20f));
    const float den = l0 - 2 * l1 + l2;
    float dl = den < 0 ? 0.5f * (l0 - l2) / den : 0.0f;
    dl = std::clamp(dl, -0.5f, 0.5f);
    const float lp = l1 - 0.25f * (l0 - l2) * dl;
    const float ppk = std::exp(lp);
    s.hz = static_cast<float>((kb + dl) * BIN_HZ);
    s.amp = 2.0f * std::sqrt(ppk) / wsum;
    s.snr_db = 10.0f * std::log10(ppk / floor);
    double lobe = 0;
    const int half = static_cast<int>(std::ceil(2.2 * FS_SUB / L / BIN_HZ));   // the Hann main lobe
    for (int k = std::max(k55, kb - half); k <= std::min(k275, kb + half); k++) lobe += pw[k];
    s.purity = band > 0 ? static_cast<float>(lobe / band) : 0.0f;
    // a CTCSS tone is 300-800 Hz of deviation (ETSI TS 103 236 6.2.3); one
    // at several kHz is the whole modulation - a test / signalling tone, a
    // data signal - not a sub-audible tone (seen live: 256 Hz at 5.7 kHz)
    s.cand = s.snr_db >= SNR_MIN_DB && s.purity >= PURITY_MIN && s.amp >= p.min_dev_hz && s.amp <= MAX_DEV_HZ;
    // one line of a periodic signal's family, not a tone: a data signal with
    // a frame rate (live: a 151.7 MHz carrier with lines every 33.33 Hz - 30
    // ms TDMA slots - whose 200 Hz line passed for a 199.5 Hz tone) has its
    // neighbours f +- f/k at comparable power; a CTCSS tone has none (voice
    // below 300 Hz is 20-40 dB down behind the transmitter's high-pass)
    int fam = 0;
    if (s.cand) {
        const float lim = ppk * 0.0316f;   // -15 dB
        auto line = [&](double hz) {
            const int c = static_cast<int>(std::lround(hz / BIN_HZ));
            if (c - 2 < k30 || c + 2 > k560) return false;
            float m = 0;
            for (int k = c - 2; k <= c + 2; k++) m = std::max(m, pw[k]);
            return m >= lim;
        };
        for (int k = 2; k <= 10 && !fam; k++) {
            const double f0 = s.hz / k;
            if (f0 < 20) break;
            if (line(s.hz - f0) && line(s.hz + f0)) fam = k;
        }
        if (fam) s.cand = false;
    }
    if (dump)
        fprintf(stderr, "HOP t=%.3f L=%d f=%.2f amp=%.0f snr=%.1f pur=%.2f%s%s\n", time_of(static_cast<double>(j_end)),
                L, s.hz, s.amp, s.snr_db, s.purity, s.cand ? " CAND" : "", fam ? " (harmonic family)" : "");
    return s;
}

// the tone's complex envelope at the tracker's frequency, from the 97
// samples ending at j_now (centre = j_now - 48); phase continuous with the
// tracker's reference
std::complex<float> Rx::track_z(int64_t j_now, double hz) const {
    const double w = 2 * PI * hz / FS_SUB;
    const double th = th_ref_ + w * static_cast<double>(j_now - j_ref_);
    std::complex<double> ph(std::cos(th), -std::sin(th));
    const std::complex<double> step(std::cos(w), std::sin(w));   // going back in time
    std::complex<double> acc(0, 0);
    for (int i = 0; i < TRK_TAPS; i++) {
        acc += static_cast<double>(h_trk_[i]) * static_cast<double>(hp_[(j_now - i) & (RING - 1)]) * ph;
        ph *= step;
    }
    return std::complex<float>(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
}

void Rx::ctcss_hop() {
    const int64_t j_now = j_ - 1;
    Spec s = analyse(j_now);
    // a 134.4 Hz tone just after a DCS code: its turn-off code, never a CTCSS tone
    if (s.cand && dcs_recent(j_now) && std::fabs(s.hz - 134.4f) < 2.0f) {
        if (!d_locked_ && d_turnoff_++ == 0 && d_name_.code >= 0) {
            RxEvent e;   // the ended code's turn-off: a note for its log line
            e.type = RxEvent::NOTE;
            e.dcs = true;
            e.code = d_name_.id();
            e.id = "DCS " + e.code;
            e.dcs_name = d_name_;
            e.why = "turn-off code";
            e.t1 = time_of(static_cast<double>(j_now));
            events.push_back(e);
        }
        s.cand = false;
    }
    if (d_locked_) s.cand = false;   // DCS on air: its line spectrum is no tone
    hops_.push_back(s);
    while (hops_.size() > 12) hops_.pop_front();
    auto tol = [](const Spec& a) {
        const float snr = std::pow(10.0f, a.snr_db / 10.0f);
        return std::clamp(1.8f / std::sqrt(snr), 0.2f, 1.2f) + 0.003f * a.hz;
    };
    if (cs_ == CState::TAIL) {
        if (j_now >= tail_until_j_) cs_ = CState::IDLE;
        else return;
    }
    if (cs_ == CState::LOCKED) {
        // another transmitter's tone taking over (capture): it dominates the FFT
        if (s.cand && std::fabs(s.hz - c_hz_) > 0.015f * c_hz_) {
            if (switch_n_ > 0 && std::fabs(s.hz - switch_hz_) < tol(s)) {
                switch_n_++;
            } else {
                switch_n_ = 1;
                switch_j_ = j_now - WIN / 2;
            }
            switch_hz_ = s.hz;
            if (switch_n_ >= 3) {
                ctcss_end(switch_j_, "another transmitter", false);
                return;
            }
        } else {
            switch_n_ = 0;
        }
        if (dip_k_ < 0 && !ticks_.empty()) ctcss_progress(rev_j_ >= 0 ? std::min(rev_j_, ticks_.back().jc) : ticks_.back().jc);
        return;
    }
    // IDLE: a tone = 3 consecutive candidates (2 when strong) agreeing in
    // frequency to the measurement's precision (a tone's frequency is crystal
    // stable; a voice harmonic's drifts) and in amplitude (a tone's deviation
    // is constant; voice comes in syllables)
    if (!s.cand) return;
    if (j_now < block_j_ && std::fabs(s.hz - block_hz_) < 0.015f * block_hz_) return;
    // off the lists (where voice leaking in lands): one candidate more, purer
    const bool listed = nearest_tone(s.hz, 0.014f) != nullptr;
    if (!listed && (s.purity < 0.7f || s.hz < 62.0f || s.hz > 252.0f)) return;
    int need = ((s.snr_db >= 25 && s.purity >= 0.8f) ? 2 : 3) + (listed ? 0 : 1);
    // 67.2 Hz = a DCS code's 1010 runs (half its bit rate) - next to the
    // 67.0 Hz tone: there a tone must last until a DCS code would have
    // locked (0.36 s; then no CTCSS lock at all), so a code like 274 isn't
    // taken for 67.0
    const bool dcs_line = std::fabs(s.hz - 67.2f) < 0.6f;
    if (dcs_line) need += 5;
    if (static_cast<int>(hops_.size()) < need) return;
    float fmin = s.hz, fmax = s.hz, amin = s.amp, amax = s.amp, snr_min = s.snr_db;
    for (int i = 0; i < need; i++) {
        const Spec& h = hops_[hops_.size() - 1 - static_cast<size_t>(i)];
        if (!h.cand) return;
        fmin = std::min(fmin, h.hz);
        fmax = std::max(fmax, h.hz);
        amin = std::min(amin, h.amp);
        amax = std::max(amax, h.amp);
        snr_min = std::min(snr_min, h.snr_db);
    }
    const float ftol = std::clamp(1.5f / std::sqrt(std::pow(10.0f, snr_min / 10.0f)), 0.12f, 0.8f);
    if (dump) fprintf(stderr, "  no lock: spread %.3f Hz (tol %.2f), amplitude %.3f\n", fmax - fmin, ftol, amax / amin);
    if (fmax - fmin > ftol || amax > 1.3f * amin) return;
    const float hz = 0.5f * (fmin + fmax);
    lock_hz_ = hz;
    lock_spread_ = fmax - fmin;
    // 134.4 Hz is DCS's bit rate / turn-off tone, no radio's CTCSS tone (131.8
    // and 136.5 are the neighbours): a DCS code's line, never a tone
    if (hz > 133.0f && hz < 135.8f) return;
    const ToneDef* t = nearest_tone(hz, 0.014f);
    if (!t && !p.nonstandard) return;
    ctcss_start(s, j_now);
}

void Rx::ctcss_start(const Spec& s, int64_t j_now) {
    // the consistent candidates' frequency (their spread: how sure)
    const float hz = lock_hz_;
    trk_hz_ = hz;
    th_ref_ = 0;
    j_ref_ = j_now;
    // back in the stream: the amplitude every 10 ms, newest first
    // tick centres back to 1.5 s, not before the last transmission's end; the
    // filter's samples must still be in the ring
    const int64_t lo_c = std::max(j_now - static_cast<int64_t>(1.5 * FS_SUB), last_end_j_);
    const int64_t lo_ring = std::max<int64_t>(j_ - RING + 4, 0);
    std::vector<Tick> back;
    for (int64_t jn = j_now; jn - TRK_HALF >= lo_c && jn - TRK_TAPS >= lo_ring; jn -= TICK) {
        const auto z = track_z(jn, trk_hz_);
        back.push_back({jn - TRK_HALF, 2 * std::abs(z), std::arg(z)});
    }
    float a = s.amp;
    if (back.size() >= 4) {
        float m[4] = {back[0].amp, back[1].amp, back[2].amp, back[3].amp};
        std::sort(m, m + 4);
        a = 0.5f * (m[1] + m[2]);
    }
    int64_t start = back.empty() ? j_now - TRK_HALF : back.back().jc;   // late entry: as far back as we can see
    for (size_t m = 0; m + 2 < back.size(); m++) {
        if (back[m].amp < 0.5f * a && back[m + 1].amp < 0.5f * a && back[m + 2].amp < 0.5f * a) {
            if (m == 0) {
                start = back[0].jc;
            } else {
                const Tick& hi = back[m - 1];
                const Tick& lw = back[m];
                const double f = (hi.amp - 0.5 * a) / std::max(1e-6, static_cast<double>(hi.amp - lw.amp));
                start = hi.jc + static_cast<int64_t>(std::lround(f * static_cast<double>(lw.jc - hi.jc)));
            }
            break;
        }
    }
    // the carrier's onset: before it the discriminator's noise fills the
    // tracker's band as loudly as a tone, so the amplitude alone can't tell
    // (a tone switching on a carrier that was already there: no onset found)
    cl_ref_ = carrier_avg(j_now - TRK_HALF - 60, 60);
    cl_low_ = 0;
    const int64_t con = carrier_onset(j_now - TRK_HALF, lo_c);
    // a transmitter's tone starts with its carrier. A "tone" that came up in
    // the middle of a carrier is either a voice harmonic holding still for a
    // moment or a real tone after a capture / a late entry: it must have
    // lasted 0.8 s first (the start is backdated anyway; a voice harmonic
    // drifts away long before)
    const bool late = con < 0 || start - con > static_cast<int64_t>(0.2 * FS_SUB);
    if (con >= 0) start = std::max(start, con);
    start = std::max(start, last_end_j_);
    if (late && j_now - start < static_cast<int64_t>(0.8 * FS_SUB)) {
        if (dump) fprintf(stderr, "  not with a carrier onset: waits (%.2f s so far)\n", (j_now - start) / FS_SUB);
        trk_hz_ = 0;
        return;   // stays IDLE
    }
    // the frequency from the phase drift of the ticks whose filter lies wholly
    // inside the transmission (the FFT's first estimate comes from windows the
    // tone only partly fills, partly noise)
    double se = 0, se2 = 0;
    int ne = 0;
    for (size_t m = 0; m + 1 < back.size() && back[m + 1].jc - TRK_HALF >= start; m++) {
        if (back[m + 1].amp < 0.6f * a || back[m].amp < 0.6f * a) continue;
        const double e = wrapf(back[m].ph - back[m + 1].ph);
        se += e;
        se2 += e * e;
        ne++;
    }
    // (only when that is the better measurement: voice leaking into the
    // tracker's +-35 Hz pulls its phase - the FFT's candidates agreed to
    // lock_spread_)
    double sigma_hz = std::max(0.05, static_cast<double>(lock_spread_));
    if (ne >= 5) {
        const double me = se / ne, sd = std::sqrt(std::max(0.0, se2 / ne - me * me));
        const double ferr = me / (2 * PI * TICK / FS_SUB);
        const double sig = sd / (2 * PI * TICK / FS_SUB) / std::sqrt(static_cast<double>(ne));
        if (sig < 0.15 && sig < sigma_hz && std::fabs(ferr) < 1.0) {
            sigma_hz = sig;
            trk_hz_ += ferr;
            back.clear();
            for (int64_t jn = j_now; jn - TRK_HALF >= lo_c && jn - TRK_TAPS >= lo_ring; jn -= TICK) {
                const auto z = track_z(jn, trk_hz_);
                back.push_back({jn - TRK_HALF, 2 * std::abs(z), std::arg(z)});
            }
            if (back.size() >= 4) {
                float m[4] = {back[0].amp, back[1].amp, back[2].amp, back[3].amp};
                std::sort(m, m + 4);
                a = 0.5f * (m[1] + m[2]);
            }
        }
    }
    const float hz_ref = static_cast<float>(trk_hz_);
    // a tone's phase advances evenly: the residual from a straight line over
    // the last 0.2 s (inside the transmission) stays at what the noise in the
    // tracker's band makes of it. A voice harmonic wanders (pitch jitter,
    // intonation) - no lock then
    {
        std::vector<double> ph;
        double prev = 0;
        for (size_t m = 0; m < back.size() && m < 21 && back[m].jc - TRK_HALF >= start; m++) {
            double v = back[m].ph;
            if (!ph.empty()) v = prev + wrapf(static_cast<float>(v - prev));
            ph.push_back(v);
            prev = v;
        }
        if (ph.size() >= 8) {
            const double n = static_cast<double>(ph.size()), mx = (n - 1) / 2;
            double my = 0;
            for (double v : ph) my += v;
            my /= n;
            double sxy = 0, sxx = 0;
            for (size_t i = 0; i < ph.size(); i++) {
                sxy += (i - mx) * (ph[i] - my);
                sxx += (i - mx) * (i - mx);
            }
            const double b = sxy / sxx;
            double r2 = 0;
            for (size_t i = 0; i < ph.size(); i++) {
                const double r = ph[i] - my - b * (i - mx);
                r2 += r * r;
            }
            const double resid = std::sqrt(r2 / n);
            const double snr_trk = std::pow(10.0, s.snr_db / 10.0) * 1.5 * FS_SUB / std::max(s.len, 150) / trk_enbw_;
            const double sig = 1.0 / std::sqrt(2.0 * std::max(snr_trk, 0.5));
            const double lim = std::max(0.12, 3.0 * sig);
            if (dump)
                fprintf(stderr, "  phase residual %.3f rad over %zu ticks (noise %.3f, limit %.3f) %.2f Hz\n", resid,
                        ph.size(), sig, lim, hz_ref);
            if (resid > lim) {
                trk_hz_ = 0;
                return;   // stays IDLE
            }
        }
    }
    a_ref_ = a;
    c_start_j_ = start;
    c_lock_j_ = j_now;
    // a listed tone within the encoders' +-0.75 % (+ this measurement's
    // uncertainty), else a non-standard one - the same id as one heard
    // before within 0.5 %
    const float tol = std::min(0.014f, 0.0085f + static_cast<float>(2.0 * sigma_hz) / hz_ref);
    const ToneDef* t = nearest_tone(hz_ref, tol);
    c_std_ = t != nullptr;
    if (t) {
        c_hz_ = t->hz;
    } else {
        c_hz_ = std::round(hz_ref * 10.0f) / 10.0f;
        for (float v : ns_seen_)
            if (std::fabs(v - hz_ref) < 0.005f * v) c_hz_ = v;
        if (std::find(ns_seen_.begin(), ns_seen_.end(), c_hz_) == ns_seen_.end() && ns_seen_.size() < 64)
            ns_seen_.push_back(c_hz_);
    }
    c_code_ = tone_name(c_hz_);
    c_meas_ = hz_ref;
    meas_ph_ = meas_t_ = 0;
    meas_f_.clear();
    meas_snap_.clear();
    meas_med_ = hz_ref;
    ticks_.clear();
    for (size_t m = std::min<size_t>(back.size(), 10); m-- > 0;) ticks_.push_back(back[m]);
    dip_k_ = -1;
    rev_j_ = -1;
    switch_n_ = 0;
    cs_ = CState::LOCKED;
    RxEvent e;
    e.type = RxEvent::START;
    e.code = c_code_;
    e.id = "CTCSS " + c_code_;
    e.standard = t ? t->standard : false;
    e.hz = hz_ref;
    e.dev_hz = a_ref_;
    e.t0 = time_of(static_cast<double>(start));
    e.t1 = time_of(static_cast<double>(j_now - TRK_HALF));
    events.push_back(e);
    if (dump)
        fprintf(stderr, "CTCSS LOCK %.2f Hz -> %.2f +- %.2f (%s) start %.3f lock %.3f amp %.0f\n", hz, hz_ref,
                sigma_hz, c_code_.c_str(), e.t0, time_of(static_cast<double>(j_now)), a_ref_);
}

void Rx::ctcss_progress(int64_t j_conf) {
    RxEvent e;
    e.type = RxEvent::PROGRESS;
    e.id = "CTCSS " + c_code_;
    e.code = c_code_;
    e.hz = static_cast<float>(c_meas_);
    e.dev_hz = a_ref_;
    e.t1 = time_of(static_cast<double>(j_conf));
    events.push_back(e);
}

void Rx::ctcss_end(int64_t j_end, const std::string& why_in, bool tail) {
    std::string why = why_in;
    if (rev_j_ >= 0 && rev_j_ <= j_end + static_cast<int64_t>(0.05 * FS_SUB)) {
        // the tone / carrier ended after a phase reversal: a reverse burst, it
        // ended there
        char b[48];
        snprintf(b, sizeof b, "reverse burst %.0f°", rev_deg_);
        why = b;
        j_end = rev_j_;
    }
    rev_j_ = -1;
    // another, stronger carrier coming up ends this transmission there (a
    // capture): its rise in the carrier power is exact, the tone's loss /
    // phase jump / the other tone's dominance in the FFT come later (a
    // reverse burst has no such rise: the carrier stays, then drops)
    if (why != "carrier off") {
        const int64_t lo = std::max(c_start_j_ + static_cast<int64_t>(0.3 * FS_SUB), j_end - static_cast<int64_t>(0.8 * FS_SUB));
        const int64_t c = carrier_onset(j_ - 1 - TRK_HALF, lo);
        if (c >= 0 && c - static_cast<int64_t>(0.01 * FS_SUB) < j_end) j_end = c - static_cast<int64_t>(0.01 * FS_SUB);
    }
    j_end = std::max(j_end, c_start_j_);
    RxEvent e;
    e.type = RxEvent::END;
    e.id = "CTCSS " + c_code_;
    e.code = c_code_;
    e.standard = c_std_;
    e.hz = static_cast<float>(c_meas_);
    e.dev_hz = a_ref_;
    e.t1 = time_of(static_cast<double>(j_end));
    e.why = why;
    events.push_back(e);
    if (dump) fprintf(stderr, "CTCSS END %s at %.3f (%s)\n", c_code_.c_str(), e.t1, why.c_str());
    last_end_j_ = std::max(last_end_j_, j_end);
    block_hz_ = static_cast<float>(trk_hz_);
    // the FFT window still holds the tone (and a reverse burst's tail) for a while
    block_j_ = std::max(j_end, j_ - 1) + static_cast<int64_t>((tail ? 0.6 : 0.36) * FS_SUB);
    cs_ = tail ? CState::TAIL : CState::IDLE;
    tail_until_j_ = j_ + static_cast<int64_t>(0.6 * FS_SUB);
    dip_k_ = -1;
    switch_n_ = 0;
}

void Rx::ctcss_tick() {
    const int64_t j_now = j_ - 1;
    tick_n_++;
    const auto z = track_z(j_now, trk_hz_);
    Tick tk{j_now - TRK_HALF, 2 * std::abs(z), std::arg(z)};
    ticks_.push_back(tk);
    while (ticks_.size() > 64) {
        ticks_.pop_front();
        if (dip_k_ >= 0) dip_k_--;
    }
    if (cs_ == CState::TAIL) {
        // the tail of a reverse burst: until the tone is gone
        const size_t n = ticks_.size();
        if ((n >= 3 && ticks_[n - 1].amp < 0.45f * a_ref_ && ticks_[n - 2].amp < 0.45f * a_ref_ &&
             ticks_[n - 3].amp < 0.45f * a_ref_) ||
            carrier_avg(tk.jc, 6) < 0.4f * cl_ref_)
            tail_until_j_ = std::min(tail_until_j_, j_now);
        return;
    }
    const size_t n = ticks_.size();
    const Tick& pv = ticks_[n >= 2 ? n - 2 : 0];
    // the measured frequency: the tone's whole phase advance (the mixer's +
    // the envelope's) over every tick where it is there at all - a tick
    // disturbed by voice / noise must count too, its phase comes back (left
    // out, its jump biased the sum); next to it the median of the ticks'
    // own frequencies, robust against a cycle slip in a deep fade
    if (n >= 2 && rev_j_ < 0 && tk.amp >= 0.3f * a_ref_ && pv.amp >= 0.3f * a_ref_) {
        const float e = wrapf(tk.ph - pv.ph);
        meas_snap_.push_back({tk.jc, meas_ph_, meas_t_, meas_f_.size()});
        while (meas_snap_.size() > 24) meas_snap_.pop_front();
        meas_ph_ += 2 * PI * trk_hz_ * TICK / FS_SUB + e;
        meas_t_ += TICK / FS_SUB;
        if (meas_f_.size() < 6000) meas_f_.push_back(static_cast<float>(trk_hz_ + e / (2 * PI * TICK / FS_SUB)));
        if (meas_t_ >= 0.3) {
            const double a = meas_ph_ / (2 * PI * meas_t_);
            if ((tick_n_ & 15) == 0 || meas_f_.size() < 64) {
                std::vector<float> m(meas_f_);
                std::nth_element(m.begin(), m.begin() + m.size() / 2, m.end());
                meas_med_ = m[m.size() / 2];
            }
            c_meas_ = std::fabs(a - meas_med_) < 0.3 ? a : meas_med_;
        }
    }
    // the tracker: the phase's drift between ticks
    if (dip_k_ < 0 && n >= 2 && tk.amp >= 0.6f * a_ref_ && pv.amp >= 0.6f * a_ref_) {
        const float e = wrapf(tk.ph - pv.ph);
        if (std::fabs(e) < 0.5f) {
            const double ferr = e / (2 * PI * TICK / FS_SUB);
            if (meas_t_ < 0.3) c_meas_ += 0.03 * (trk_hz_ + ferr - c_meas_);
            const double nh = trk_hz_ + 0.06 * ferr;
            th_ref_ += 2 * PI * trk_hz_ / FS_SUB * static_cast<double>(j_now - j_ref_);
            th_ref_ = std::fmod(th_ref_, 2 * PI);
            j_ref_ = j_now;
            trk_hz_ = nh;
        }
    }
    if (tk.amp > 0.6f * a_ref_ && tk.amp < 1.6f * a_ref_) a_ref_ += 0.02f * (tk.amp - a_ref_);
    // the carrier gone: the end, wherever the tracker's amplitude is (in the
    // noise without a carrier it can look like the tone)
    const float cl = carrier_avg(tk.jc, 6);
    if (cl < 0.4f * cl_ref_) {
        if (++cl_low_ >= 3) {
            int64_t c = ticks_[n >= 3 ? n - 3 : 0].jc - TICK;
            while (c < tk.jc && carrier_avg(c, 3) >= 0.5f * cl_ref_) c++;
            if (dip_k_ >= 0) c = std::min(c, ticks_[static_cast<size_t>(dip_k_)].jc);
            ctcss_end(c, "carrier off", false);
            return;
        }
    } else {
        cl_low_ = 0;
        if (dip_k_ < 0) cl_ref_ += 0.01f * (cl - cl_ref_);
    }
    auto min_amp_jc = [this](size_t from) {
        size_t best = from;
        for (size_t i = from; i < ticks_.size(); i++)
            if (ticks_[i].amp < ticks_[best].amp) best = i;
        return ticks_[best].jc;
    };
    // a reverse burst waiting for its confirmation: the tone (or the
    // carrier) ends within 0.5 s - else it was a phase glitch and the
    // transmission goes on
    if (rev_j_ >= 0 && j_now - rev_seen_j_ > static_cast<int64_t>(0.5 * FS_SUB)) {
        if (dump) fprintf(stderr, "CTCSS phase step at %.3f not followed by the end: ignored\n", time_of(rev_j_));
        rev_j_ = -1;
        dip_k_ = -1;
    }
    auto reversal = [&](int64_t jr, float deg) {
        if (rev_j_ >= 0) return;   // already waiting for one (the phase back later: the same)
        rev_j_ = jr;
        rev_seen_j_ = j_now;
        rev_deg_ = deg;
        // the measured frequency: back to before the step (its +-180 deg is
        // no frequency); left there while the reversal waits for the end
        for (auto it = meas_snap_.rbegin(); it != meas_snap_.rend(); ++it)
            if (it->jc + TRK_HALF <= jr) {
                meas_ph_ = it->ph;
                meas_t_ = it->t;
                meas_f_.resize(std::min(meas_f_.size(), it->nf));
                break;
            }
        if (meas_t_ >= 0.3) c_meas_ = meas_ph_ / (2 * PI * meas_t_);
        dip_k_ = -1;
        if (dump) fprintf(stderr, "CTCSS phase step %.0f° at %.3f\n", deg, time_of(jr));
    };
    const bool carrier_on = cl >= 0.6f * cl_ref_;
    if (dip_k_ < 0) {
        if (tk.amp < 0.45f * a_ref_) {
            dip_k_ = static_cast<int>(n) - 1;
            ph_before_ = ticks_[n >= 3 ? n - 3 : 0].ph;
            return;
        }
        // a phase reversal with the amplitude hardly dipping (120-135°)
        if (n >= 9 && carrier_on) {
            const Tick& t8 = ticks_[n - 9];
            const float d = wrapf(tk.ph - t8.ph);
            if (tk.amp >= 0.6f * a_ref_ && t8.amp >= 0.6f * a_ref_ && std::fabs(d) > 1.75f)
                reversal(min_amp_jc(n - 9), std::fabs(d) * 180 / static_cast<float>(PI));
        }
        return;
    }
    // in a dip: a reverse burst (back with the phase turned), a dropout (back
    // as it was) or the end (not back)
    const int n_dip = static_cast<int>(n) - 1 - dip_k_;
    if (tk.amp >= 0.6f * a_ref_) {
        const float d = wrapf(tk.ph - ph_before_);
        if (std::fabs(d) > 1.75f && carrier_on)
            reversal(min_amp_jc(static_cast<size_t>(std::max(0, dip_k_ - 2))), std::fabs(d) * 180 / static_cast<float>(PI));
        dip_k_ = -1;
        return;
    }
    if (n_dip >= 12) {
        // the end: where the amplitude fell through half of the tone's (after
        // a reverse burst: at the reversal)
        int64_t jend = ticks_[static_cast<size_t>(dip_k_)].jc;
        for (int i = dip_k_; i >= 1; i--) {
            const Tick& lw = ticks_[static_cast<size_t>(i)];
            const Tick& hi = ticks_[static_cast<size_t>(i - 1)];
            if (hi.amp >= 0.5f * a_ref_ && lw.amp < 0.5f * a_ref_) {
                const double f = (hi.amp - 0.5 * a_ref_) / std::max(1e-6, static_cast<double>(hi.amp - lw.amp));
                jend = hi.jc + static_cast<int64_t>(std::lround(f * static_cast<double>(lw.jc - hi.jc)));
                break;
            }
        }
        ctcss_end(jend, "tone off", false);
    }
}

// ---------------------------------------------------------------------------
// DCS
void Rx::dcs_sample(int64_t jy) {
    const float y0 = y_[jy & (RING - 1)];
    const float thr = 0.5f * (d_hi_ + d_lo_);
    d_mean_ += (d_locked_ ? 0.0005 : 0.004) * (y0 - d_mean_);
    if (d_have_prev_) {
        const float v1 = d_prev_ - thr, v0 = y0 - thr;
        if ((v1 > 0) != (v0 > 0) && v1 != v0) {
            // a transition: the bit boundary is half a bit before the next centre
            const double tc = static_cast<double>(jy - 1) + v1 / (v1 - v0);
            double err = tc - (d_tau_ - 0.5 * d_sps_);
            while (err > 0.5 * d_sps_) err -= d_sps_;
            while (err < -0.5 * d_sps_) err += d_sps_;
            if (std::fabs(err) < 0.45 * d_sps_) {
                // phase + rate (encoders: +-2 %); searching, the rate leaks back
                // to nominal (a tone / voice must not hold it at a limit)
                const double nom = FS_SUB / DCS_BAUD;
                d_tau_ += (d_locked_ ? 0.08 : 0.15) * err;
                d_sps_ = std::clamp(d_sps_ + 0.004 * err, nom * 0.97, nom * 1.03);
            }
        }
    }
    d_prev_ = y0;
    d_have_prev_ = true;
    if (d_tau_ < static_cast<double>(jy) - 2 * d_sps_) d_tau_ = static_cast<double>(jy);   // (re)start
    while (d_tau_ <= static_cast<double>(jy) - 1.0 && d_tau_ >= static_cast<double>(jy) - 2.0 * d_sps_) {
        // y at the bit centre (linear between the two samples around it)
        const int64_t a = static_cast<int64_t>(std::floor(d_tau_));
        const double fr = d_tau_ - static_cast<double>(a);
        const float ya = y_[a & (RING - 1)], yb = y_[(a + 1) & (RING - 1)];
        const float yc = static_cast<float>(ya + fr * (yb - ya));
        const float v = yc - 0.5f * (d_hi_ + d_lo_);
        const uint8_t b = v > 0 ? 1 : 0;
        if (b) d_hi_ += 0.04f * (yc - d_hi_);
        else d_lo_ += 0.04f * (yc - d_lo_);
        if (!d_locked_) d_sps_ += 0.01 * (FS_SUB / DCS_BAUD - d_sps_);
        // levels collapsed, run away or not around the signal any more (no DCS,
        // a carrier offset that changed): around the mean again
        const float half = 0.5f * (d_hi_ - d_lo_);
        if (half < 20 || half > 3000 || d_hi_ < d_mean_ || d_lo_ > d_mean_) {
            const float h = std::clamp(half, 100.0f, 1000.0f);
            d_hi_ = static_cast<float>(d_mean_ + h);
            d_lo_ = static_cast<float>(d_mean_ - h);
        }
        const float soft = v / std::max(0.5f * (d_hi_ - d_lo_), 30.0f);
        dcs_bit(b, soft, d_tau_ - DCS_DELAY);
        d_tau_ += d_sps_;
    }
}

namespace {
inline uint32_t word_at(const std::deque<Rx::Bit>& bits, size_t first) {   // 23 bits, time order
    uint32_t w = 0;
    for (int i = 0; i < 23; i++) w |= static_cast<uint32_t>(bits[first + static_cast<size_t>(i)].b) << i;
    return w;
}
}  // namespace

void Rx::dcs_bit(uint8_t b, float soft, double jc) {
    bits_.push_back({b, soft, jc});
    while (bits_.size() > 1200) bits_.pop_front();
    bit_n_++;
    if (!d_locked_) {
        // not while a CTCSS tone is on: a sampled sine can look 23-periodic
        // (the clock loop has +-3 % of rate to find such a fit)
        if (cs_ == CState::IDLE) dcs_try_lock();
        return;
    }
    const int64_t k = bit_n_ - 1;
    const int64_t base = bit_n_ - static_cast<int64_t>(bits_.size());
    auto bad = [&](int64_t i) { return bits_[static_cast<size_t>(i - base)].b != d_exp_[i % 23]; };
    int m8 = 0;
    for (int64_t i = k - 7; i <= k; i++) m8 += bad(i);
    if (m8 <= 1) d_last_good_ = k;
    int m23 = 0;
    for (int64_t i = k - 22; i <= k; i++) m23 += bad(i);
    if (m23 > 6) {
        dcs_end("code off");
        return;
    }
    if (k % 6 == 0) {
        RxEvent e;
        e.type = RxEvent::PROGRESS;
        e.dcs = true;
        e.id = "DCS " + d_name_.id();
        e.code = d_name_.id();
        e.dcs_name = d_name_;
        e.dev_hz = 0.5f * (d_hi_ - d_lo_);
        e.t1 = time_of(bits_[static_cast<size_t>(d_last_good_ - base)].jc + 0.5 * d_sps_);
        events.push_back(e);
    }
}

bool Rx::dcs_try_lock() {
    const size_t nb = bits_.size();
    if (nb < 46) return false;
    const uint32_t w1 = word_at(bits_, nb - 23), w2 = word_at(bits_, nb - 46);
    uint32_t pat = 0;
    if (w1 == w2 && golay_ok(w1)) {
        pat = w1;
    } else if (nb >= 69) {
        const uint32_t w3 = word_at(bits_, nb - 69);
        const uint32_t maj = (w1 & w2) | (w1 & w3) | (w2 & w3);
        if (!golay_ok(maj)) return false;
        const int mism = __builtin_popcount(w1 ^ maj) + __builtin_popcount(w2 ^ maj) + __builtin_popcount(w3 ^ maj);
        if (mism > 5) return false;
        pat = maj;
    } else {
        return false;
    }
    if (pat == 0 || pat == MASK23) return false;   // no modulation / a stuck slicer
    const DcsName nm = dcs_name(pat);
    if (nm.code < 0) return false;                  // no 001 alignment: not a DCS code
    const int64_t k = bit_n_ - 1;
    const int64_t base = bit_n_ - static_cast<int64_t>(nb);
    // E[i mod 23] = the expected bit at index i (pat bit 0 = index k - 22)
    for (int i = 0; i < 23; i++) d_exp_[(k - 22 + i) % 23] = static_cast<uint8_t>((pat >> i) & 1u);
    // where it began: back while 8-bit windows hold <= 1 wrong bit
    auto bad = [&](int64_t i) { return bits_[static_cast<size_t>(i - base)].b != d_exp_[i % 23]; };
    int64_t s = k - 22;
    while (s - 1 >= base + 7) {
        int m = 0;
        for (int64_t i = s - 1; i < s + 7; i++) m += bad(i);
        if (m > 1) break;
        if (time_of(bits_[static_cast<size_t>(s - 1 - base)].jc) < time_of(static_cast<double>(last_end_j_))) break;
        s--;
    }
    // the first bits of a clean run can be wrong (the slicer's levels settling): skip leading bad ones
    while (s < k && bad(s)) s++;
    double t_start = time_of(bits_[static_cast<size_t>(s - base)].jc - 0.5 * d_sps_);
    // the carrier's onset, when it is near: the code starts with it (the bits
    // just after it are often wrong while the slicer settles; noise before it
    // can match a few bits by chance)
    {
        const int64_t jl = static_cast<int64_t>(bits_[nb - 1].jc);
        const int64_t c = carrier_onset(jl, std::max(last_end_j_, jl - static_cast<int64_t>(1.5 * FS_SUB)));
        if (c >= 0 && time_of(static_cast<double>(c)) > t_start - 0.4) t_start = time_of(static_cast<double>(c));
    }
    d_locked_ = true;
    d_pat_ = pat;
    d_name_ = nm;
    d_lock_bit_ = k;
    d_start_bit_ = s;
    d_last_good_ = k;
    d_dev_ = 0.5 * (d_hi_ - d_lo_);
    d_turnoff_ = 0;
    // a CTCSS "tone" locked on the code's lines (shouldn't happen: purity)
    if (cs_ == CState::LOCKED) ctcss_end(c_start_j_, "DCS", false);
    RxEvent e;
    e.type = RxEvent::START;
    e.dcs = true;
    e.code = nm.id();
    e.id = "DCS " + e.code;
    e.dcs_name = nm;
    e.standard = nm.listed;
    e.dev_hz = static_cast<float>(d_dev_);
    e.t0 = t_start;
    e.t1 = time_of(bits_[nb - 1].jc + 0.5 * d_sps_);
    events.push_back(e);
    if (dump) fprintf(stderr, "DCS LOCK %s start %.3f lock %.3f dev %.0f\n", e.code.c_str(), e.t0, e.t1, d_dev_);
    return true;
}

void Rx::dcs_end(const std::string& why) {
    const int64_t base = bit_n_ - static_cast<int64_t>(bits_.size());
    const double jend = bits_[static_cast<size_t>(std::max(d_last_good_, base) - base)].jc + 0.5 * d_sps_;
    d_locked_ = false;
    d_end_j_ = static_cast<int64_t>(jend);
    last_end_j_ = std::max(last_end_j_, d_end_j_);
    d_turnoff_ = 0;
    RxEvent e;
    e.type = RxEvent::END;
    e.dcs = true;
    e.code = d_name_.id();
    e.id = "DCS " + e.code;
    e.dcs_name = d_name_;
    e.standard = d_name_.listed;
    e.dev_hz = static_cast<float>(d_dev_);
    e.t1 = time_of(jend);
    e.why = why;
    events.push_back(e);
    if (dump) fprintf(stderr, "DCS END %s at %.3f (%s)\n", e.code.c_str(), e.t1, why.c_str());
    // the bits so far can't start another lock
    bits_.clear();
}

}  // namespace ctcss
