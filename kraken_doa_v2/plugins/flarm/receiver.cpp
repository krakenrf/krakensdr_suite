// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KrakenRF Inc. - part of the KrakenSDR `flarm` decoder
// plugin, licensed under the GNU General Public License version 3 (LICENSE in
// this folder; README.md "Licence").

// FLARM / OGN receiver: burst detector + demodulator (see receiver.hpp).

#include "receiver.hpp"

#include "kraken_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace flarm {

namespace {

constexpr int RING_BITS = 18;                  // input history: 2^18 samples = 109 ms
constexpr uint64_t RING = 1ull << RING_BITS;
constexpr uint64_t RING_MASK = RING - 1;
constexpr int HALF_BINS = 8;                   // +-8 bins = +-75 kHz around the channel
constexpr int LEARN_FRAMES = 64;
constexpr double MIN_BURST_S = 0.002, MAX_BURST_S = 0.014;
constexpr int64_t CARRIER_FRAMES = 225;        // still active 24 ms after its start: a carrier
constexpr int QUIET_END = 3;                   // inactive frames that end a burst
constexpr int64_t DECIDE_AFTER = 4;            // frames from candidate to decision
constexpr int64_t RECENT_FRAMES = 400;         // ~43 ms of decided candidates for the broadband rule
constexpr int64_t BROADBAND_FRAMES = 2;        // "starting together": first frames within +-2
constexpr double BROADBAND_SPAN_HZ = 600e3;
constexpr double LEAK_HZ = 250e3;
constexpr int PAD_BEFORE = 3, PAD_AFTER = 4;   // frames of input around the burst
constexpr int LP_TAPS = 49;
constexpr double LP_CUT_HZ = 110e3;
constexpr int MIN_DEC = SPC * PACKET_CHIPS + 8;   // 1896 decimated samples
constexpr int MAX_CHIP_ERRORS = 6;
constexpr double CLEAN_SYNC = SYNC_CHIPS * SPC * 0.785;   // |c| of a clean sync
constexpr uint32_t SYNC_TAIL[2] = {0x531FAB6u, 0xAF3656Cu};   // FLARM F531FAB6, OGN 0AF3656C: last 28 bits

}  // namespace

Receiver::Receiver() {
    ring_.assign(RING, kp::cf(0, 0));
    for (int i = 0; i < NFFT; i++) hann_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / NFFT));
    fin_ = kfft_alloc_complex(NFFT);
    fout_ = kfft_alloc_complex(NFFT);
    plan_ = kfft_plan_dft_1d(NFFT, fin_, fout_, KFFT_FORWARD);
    lp_ = kp::fir_lowpass(LP_TAPS, static_cast<float>(LP_CUT_HZ), static_cast<float>(FS));
    // a data bit 1 = chips "0 1" (-1, +1), a 0 = "1 0"
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < SYNC_CHIPS / 2; i++) {
            const bool one = (SYNC_TAIL[p] >> (27 - i)) & 1;
            tmpl_[p][2 * i] = one ? -1.0f : 1.0f;
            tmpl_[p][2 * i + 1] = one ? 1.0f : -1.0f;
        }
}

Receiver::~Receiver() {
    kfft_destroy_plan(plan_);
    kfft_free(fin_);
    kfft_free(fout_);
}

void Receiver::reset() {
    std::fill(ring_.begin(), ring_.end(), kp::cf(0, 0));
    abs_ = 0;
    frame_ = 0;
    win_.clear();
    ch_.clear();
    pending_.clear();
    recent_.clear();
    log.clear();
}

void Receiver::set_windows(const std::vector<Window>& w) {
    win_ = w;
    ch_.assign(win_.size(), Chan{});
    bool need[NFFT] = {};
    for (size_t i = 0; i < win_.size(); i++) {
        const long k = std::lround(win_[i].off_hz / BIN_HZ);
        ch_[i].k0 = static_cast<int>(((k % NFFT) + NFFT) % NFFT);
        for (int d = -HALF_BINS; d <= HALF_BINS; d++) need[(ch_[i].k0 + d + NFFT) & (NFFT - 1)] = true;
    }
    bins_.clear();
    for (int k = 0; k < NFFT; k++)
        if (need[k]) bins_.push_back(k);
    pending_.clear();
    recent_.clear();
}

void Receiver::set_threshold_db(double db) { thr_ = std::pow(10.0, db / 10.0); }

void Receiver::process(const kp::cf* x, size_t n, double now, std::vector<RxPacket>* out) {
    t_now_ = now;
    t_abs_ = abs_;
    size_t i = 0;
    while (i < n) {
        // up to the next frame boundary (a frame never wraps the ring: RING is a multiple of NFFT)
        const size_t room = NFFT - static_cast<size_t>(abs_ % NFFT);
        const size_t k = std::min(room, n - i);
        std::copy(x + i, x + i + k, &ring_[abs_ & RING_MASK]);
        abs_ += k;
        i += k;
        if (abs_ % NFFT == 0) frame(out);
    }
}

void Receiver::frame(std::vector<RxPacket>* out) {
    const int64_t f = frame_++;
    if (ch_.empty()) return;   // no channel inside the VFO
    const kp::cf* in = &ring_[(abs_ - NFFT) & RING_MASK];
    for (int i = 0; i < NFFT; i++) {
        fin_[i][0] = in[i].real() * hann_[i];
        fin_[i][1] = in[i].imag() * hann_[i];
    }
    kfft_execute(plan_);
    for (int k : bins_) pw_[k] = fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];

    for (size_t w = 0; w < ch_.size(); w++) {
        Chan& c = ch_[w];
        float e = 0;
        for (int d = -HALF_BINS; d <= HALF_BINS; d++) e += pw_[(c.k0 + d + NFFT) & (NFFT - 1)];
        if (c.learnt < LEARN_FRAMES) {
            c.floor = c.learnt == 0 ? e : std::min(1.05f * c.floor, std::max(e, 0.5f * c.floor));
            c.learnt++;
            continue;
        }
        const bool active = e > thr_ * c.floor;
        if (!active) c.floor += (e < 2 * c.floor ? 0.004f : 0.0005f) * (e - c.floor);
        if (active) {
            if (!c.on) {
                c.on = true;
                c.f0 = f;
                c.peak = e;
            } else {
                c.peak = std::max(c.peak, e);
            }
            c.f1 = f;
            c.quiet = 0;
            if (f - c.f0 >= CARRIER_FRAMES) c.on = false;   // a carrier, not a packet
        } else if (c.on && ++c.quiet >= QUIET_END) {
            c.on = false;
            const double dur = static_cast<double>(c.f1 - c.f0 + 1) * FRAME_S;
            if (dur >= MIN_BURST_S && dur <= MAX_BURST_S)
                pending_.push_back({static_cast<int>(w), c.f0, c.f1, static_cast<double>(c.peak) / c.floor, f + DECIDE_AFTER});
        }
    }

    // the candidates due now: judged together (each other's still pending), then decided
    while (!recent_.empty() && recent_.front().at < f - RECENT_FRAMES) recent_.pop_front();
    size_t due = 0;
    while (due < pending_.size() && pending_[due].decide <= f) due++;
    if (!due) return;
    std::vector<int> verdict(due, 0);   // 0 demodulate, 1 broadband, 2 leakage
    for (size_t i = 0; i < due; i++) {
        const Cand& c = pending_[i];
        const double off = win_[static_cast<size_t>(c.win)].off_hz;
        // broadband: others starting together
        int together = 0;
        double lo = off, hi = off;
        for (size_t j = 0; j < pending_.size(); j++) {
            if (j == i || std::llabs(pending_[j].f0 - c.f0) > BROADBAND_FRAMES) continue;
            together++;
            const double o = win_[static_cast<size_t>(pending_[j].win)].off_hz;
            lo = std::min(lo, o);
            hi = std::max(hi, o);
        }
        for (const Decided& r : recent_) {
            if (std::llabs(r.f0 - c.f0) > BROADBAND_FRAMES) continue;
            together++;
            lo = std::min(lo, r.off);
            hi = std::max(hi, r.off);
        }
        if (together >= 2 && hi - lo >= BROADBAND_SPAN_HZ) {
            verdict[i] = 1;
            continue;
        }
        // leakage: a stronger overlapping candidate nearby
        for (size_t j = 0; j < pending_.size(); j++) {
            const Cand& o = pending_[j];
            if (j == i || o.snr <= c.snr || o.f0 > c.f1 || c.f0 > o.f1) continue;
            if (std::fabs(win_[static_cast<size_t>(o.win)].off_hz - off) <= LEAK_HZ) {
                verdict[i] = 2;
                break;
            }
        }
    }
    std::vector<Cand> batch(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(due));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(due));
    for (size_t i = 0; i < due; i++) {
        if (verdict[i] == 1) broadband_++;
        else if (verdict[i] == 0) demod(batch[i], out);
        recent_.push_back({f, batch[i].f0, win_[static_cast<size_t>(batch[i].win)].off_hz});
    }
}

void Receiver::demod(const Cand& c, std::vector<RxPacket>* out) {
    demodulated_++;
    const int64_t first_i = static_cast<int64_t>(NFFT) * (c.f0 - PAD_BEFORE);
    const int64_t end_i = static_cast<int64_t>(NFFT) * (c.f1 + PAD_AFTER);
    if (first_i < 0 || abs_ - static_cast<uint64_t>(first_i) > RING || end_i > static_cast<int64_t>(abs_)) return;
    const uint64_t first = static_cast<uint64_t>(first_i);
    const size_t count = static_cast<size_t>(end_i - first_i);
    const Window& w = win_[static_cast<size_t>(c.win)];

    // mix to 0 Hz
    mix_.resize(count);
    const double step = -2 * M_PI * w.off_hz / FS;
    std::complex<double> rot(1, 0);
    const std::complex<double> dr = std::polar(1.0, step);
    for (size_t i = 0; i < count; i++) {
        mix_[i] = ring_[(first + i) & RING_MASK] * kp::cf(static_cast<float>(rot.real()), static_cast<float>(rot.imag()));
        rot *= dr;
        if ((i & 1023) == 1023) rot /= std::abs(rot);
    }
    // low-pass + decimate: output o centred on input first + 6 o + 24
    const int N = count >= LP_TAPS ? static_cast<int>((count - LP_TAPS) / DEC) : 0;
    y_.resize(static_cast<size_t>(std::max(N, 0)));
    for (int o = 0; o < N; o++) {
        const kp::cf* p = &mix_[static_cast<size_t>(o) * DEC];
        float re = 0, im = 0;
        for (int k = 0; k < LP_TAPS; k++) {
            re += lp_[static_cast<size_t>(k)] * p[k].real();
            im += lp_[static_cast<size_t>(k)] * p[k].imag();
        }
        y_[static_cast<size_t>(o)] = kp::cf(re, im);
    }
    if (N < MIN_DEC) {
        if (!dump_dir_.empty()) {
            double pwr = 0;
            const int L = std::min(N, SPC * PACKET_CHIPS);
            for (int o = 0; o < L; o++) pwr += std::norm(y_[static_cast<size_t>(o)]);
            dump(c, first, N, 0, 0, 10 * std::log10(std::max(L ? pwr / L : 0.0, 1e-12)));
        }
        return;
    }
    // FM discriminator, then the phase advance over each chip (4 samples)
    d_.assign(static_cast<size_t>(N), 0.0f);
    for (int o = 1; o < N; o++) d_[static_cast<size_t>(o)] = std::arg(y_[static_cast<size_t>(o)] * std::conj(y_[static_cast<size_t>(o - 1)]));
    m_.assign(static_cast<size_t>(N), 0.0f);
    for (int o = 0; o + 3 < N; o++)
        m_[static_cast<size_t>(o)] = d_[static_cast<size_t>(o)] + d_[static_cast<size_t>(o + 1)] + d_[static_cast<size_t>(o + 2)] +
                                     d_[static_cast<size_t>(o + 3)];
    // both sync tails, the strongest correlation of each
    const int n_end = N - SPC * PACKET_CHIPS - 4;
    int best_n[2] = {0, 0};
    float best_c[2] = {0, 0};
    for (int p = 0; p < 2; p++) {
        float best = -1;
        for (int n = 1; n < n_end; n++) {
            float cc = 0;
            const float* mm = &m_[static_cast<size_t>(n)];
            for (int k = 0; k < SYNC_CHIPS; k++) cc += tmpl_[p][k] * mm[SPC * k];
            if (std::fabs(cc) > best) {
                best = std::fabs(cc);
                best_n[p] = n;
                best_c[p] = cc;
            }
        }
    }
    // the packet's level from the later of the two
    const int from = std::max(best_n[0], best_n[1]);
    double pwr = 0;
    for (int o = from; o < from + SPC * PACKET_CHIPS; o++) pwr += std::norm(y_[static_cast<size_t>(o)]);
    const double level_db = 10 * std::log10(std::max(pwr / (SPC * PACKET_CHIPS), 1e-12));
    if (!dump_dir_.empty()) dump(c, first, N, std::fabs(best_c[0]) / CLEAN_SYNC, std::fabs(best_c[1]) / CLEAN_SYNC, level_db);

    const int order[2] = {std::fabs(best_c[0]) >= std::fabs(best_c[1]) ? 0 : 1,
                          std::fabs(best_c[0]) >= std::fabs(best_c[1]) ? 1 : 0};
    for (int p : order) {
        const int n = best_n[p];
        const float P = best_c[p] >= 0 ? 1.0f : -1.0f;   // the polarity
        // the sync's chips must be nearly all right
        float dc = 0;
        for (int k = 0; k < SYNC_CHIPS; k++) dc += m_[static_cast<size_t>(n + SPC * k)];
        dc /= SYNC_CHIPS;
        int errors = 0;
        for (int k = 0; k < SYNC_CHIPS; k++) {
            const float v = P * (m_[static_cast<size_t>(n + SPC * k)] - dc);
            errors += (v > 0) != (tmpl_[p][k] > 0);
        }
        if (errors > MAX_CHIP_ERRORS) continue;
        for (int n0 : {n, n - 1, n + 1}) {
            RxPacket pk;
            if (!try_bits(p, n0, P, pk)) continue;
            // the carrier: Manchester has no DC, so the discriminator's mean
            double sum = 0;
            for (int o = n0; o < n0 + SPC * PACKET_CHIPS; o++) sum += d_[static_cast<size_t>(o)];
            pk.carrier_hz = sum / (SPC * PACKET_CHIPS) * FSD / (2 * M_PI) + w.off_hz;
            // span: just inside the first matched sync chip .. the last data chip
            const double s = static_cast<double>(first) + DEC * (n0 - 0.5) + (LP_TAPS - 1) / 2.0;
            pk.t0 = s + 12;
            pk.t1 = s + DEC * SPC * PACKET_CHIPS - 12;
            pk.level_db = level_db;
            pk.snr_db = 10 * std::log10(c.snr);
            pk.win = c.win;
            out->push_back(pk);
            return;
        }
    }
}

bool Receiver::try_bits(int proto, int n0, float P, RxPacket& pk) {
    const int start = n0 + SPC * SYNC_CHIPS;
    float soft[pkt::BITS];
    uint8_t bytes[pkt::BYTES] = {};
    double amp = 0;
    for (int i = 0; i < pkt::BITS; i++) {
        const size_t a = static_cast<size_t>(start + 2 * SPC * i);
        soft[i] = P * (m_[a + SPC] - m_[a]);   // second chip minus first
        if (soft[i] > 0) bytes[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
        amp += std::fabs(soft[i]);
    }
    amp /= pkt::BITS;
    if (proto == 0) return pkt::accept_flarm(bytes, &pk.frame);
    return pkt::accept_ogn(bytes, soft, static_cast<float>(amp), &pk.frame);
}

void Receiver::dump(const Cand& c, uint64_t first, int N, double best_f, double best_o, double level_db) {
    const Window& w = win_[static_cast<size_t>(c.win)];
    const double mhz = std::isfinite(w.mhz) ? w.mhz : w.off_hz / 1e6;
    char name[64];
    snprintf(name, sizeof name, "/burst_%05llu_%.3f.cf32", static_cast<unsigned long long>(demodulated_), mhz);
    if (FILE* f = fopen((dump_dir_ + name).c_str(), "wb")) {
        if (N > 0) fwrite(y_.data(), sizeof(kp::cf), static_cast<size_t>(N), f);   // interleaved float32 I / Q
        fclose(f);
    }
    char s[192];
    snprintf(s, sizeof s, "burst %llu t=%.3f f=%.3f MHz dur=%.1f ms snr=%.1f dB flarm=%.2f ogn=%.2f level=%.1f",
             static_cast<unsigned long long>(demodulated_), time_of(static_cast<double>(first)), mhz,
             static_cast<double>(c.f1 - c.f0 + 1) * FRAME_S * 1e3, 10 * std::log10(c.snr), best_f, best_o, level_db);
    log.emplace_back(s);
}

}  // namespace flarm
