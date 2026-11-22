#pragma once

// One Aero channel (P or C channel, or the R / T bursts) at 24 kHz: the
// decoder of the single-channel modes, and of each channel of the "all
// channels" mode (aero_all.cpp) behind its own kp::Host. Moved here from
// decoder.cpp; what it decodes is unchanged.

#include "aero_acq.hpp"
#include "aero_ambe.hpp"
#include "aero_burst.hpp"
#include "aero_cchan.hpp"
#include "aero_demod.hpp"
#include "aero_pchan.hpp"
#include "aero_su.hpp"
#include "kraken_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace aero {

namespace chan {

constexpr double FS = 24000;
// the line pair's score over the median that makes a carrier, by the number of
// (half-overlapping) FFTs summed: noise reached 13.5 / 10.4 / 9.9 dB in 70 / 70 /
// 1435 searches with 1 / 2 / 3 (noise300 in 4 s starts and whole); ~3 dB above
inline float acq_threshold_db(int k) { return k <= 1 ? 16.5f : k == 2 ? 13.5f : 12.5f; }
constexpr double HIST_S = 3.0;        // input kept to start the demodulator on
constexpr double SYNC_TIMEOUT_FRAMES = 6.5;
}  // namespace chan

using namespace chan;

class AeroChannel : public kp::Decoder {
public:
    explicit AeroChannel(kp::Host& h) : Decoder(h), su_(h), burst_(h, su_) {
        pch_.on_su = [this](const uint8_t* su, const aero::PFrame& f, int idx) { su_.p_su(su, f.t0, f.t1, idx); };
        pch_.on_frame = [this](const aero::PFrame& f) { on_pframe(f); };
        pch_.on_early = [this](const aero::PFrame& f) { valid_span(f.t0, f.t1); };
        cch_.on_su = [this](const uint8_t* su, const aero::CFrame& f) { su_.c_su(su, f.t0, f.t1); };
        cch_.on_frame = [this](const aero::CFrame& f) { on_cframe(f); };
        host.fact("Voice codec", "AMBE (Aero 4800): " + dig::AeroAmbeLib::instance().status());
        reset();
    }

    void reset() override {
        n_ = 0;
        tbase_set_ = false;
        hist_.clear();
        hist_base_ = 0;
        demod_.stop();
        pch_.stop();
        cch_.stop();
        su_.reset();
        burst_.reset();
        make_acq();
        state_ = ACQ;
        tried_.clear();
        last_fact_ = -10;
        voice_.reset();
        in_call_ = false;
        host.fact("Channel", bursts() ? "R / T bursts from aircraft: listening" : "searching");
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "mode") {
            const std::string m = v.empty() ? "auto" : v;
            if (m != mode_) {
                mode_ = m;
                reset();
            }
        } else {
            su_.option(k, v);
        }
    }

    void process(const cf* x, size_t n) override {
        // the VFO moved (or the receiver was retuned): another channel - start
        // over. Off air, a VFO moved from a 600 bit/s channel to a 1200 bit/s
        // one 2 kHz off kept the old state and locked 900 Hz beside the
        // carrier for a minute (408 of 960 SUs); from a fresh start, 594 / 594.
        // A small move (<= 2 kHz) with the carrier still in range while a
        // channel is synced keeps the lock: the demodulator follows the move
        const double rf = host.rf_hz();
        if (std::isfinite(rf) && rf > 0) {
            const double d = rf_seen_ > 0 ? rf - rf_seen_ : 0;
            if (std::fabs(d) > 100) {
                const double nc = demod_.carrier_hz() - d;
                if (state_ == LOCK && synced_now() && std::fabs(d) <= 2000 && std::fabs(nc) <= 2900) {
                    demod_.shift_carrier(-d);
                    lock_f0_ -= d;
                    if (debug_) dbg("VFO moved " + std::to_string(d) + " Hz: carrier followed");
                } else {
                    reset();
                }
            }
            rf_seen_ = rf;
        }
        if (!tbase_set_) {
            tbase_ = host.time();
            tbase_set_ = true;
        }
        if (bursts()) {
            burst_.process(x, n, host_t(static_cast<double>(n_)));
            n_ += static_cast<int64_t>(n);
            return;
        }
        // input history (to start the demodulator on what acquisition saw)
        hist_.insert(hist_.end(), x, x + n);
        const size_t hmax = static_cast<size_t>(HIST_S * FS);
        if (hist_.size() > hmax) {
            const size_t d = hist_.size() - hmax;
            hist_.erase(hist_.begin(), hist_.begin() + static_cast<long>(d));
            hist_base_ += static_cast<int64_t>(d);
        }
        const State before = state_;
        if (acq_ && acq_->process(x, n) && state_ == ACQ) try_lock();
        // a lock just now ran the demodulator over the history, x included
        if (state_ == LOCK && before == LOCK) run_demod(x, n);
        if (state_ == LOCK) check_stuck();
        n_ += static_cast<int64_t>(n);
        if (state_ == LOCK) supervise();
    }

    // for the "all channels" mode (aero_all.cpp)
    bool locked() const { return state_ == LOCK; }
    bool is_cchannel() const { return cmode_; }
    double locked_rate() const { return lock_rate_; }
    double carrier_hz() const { return demod_.carrier_hz(); }
    const SuDecoder& su() const { return su_; }
    void on_c_assign(std::function<void(double)> f) { su_.on_c_assign = std::move(f); }
    const std::string& mode() const { return mode_; }
    // the carrier search range (+- Hz; 2900 = the whole channel): a channel
    // of the "all channels" mode opened on a narrow peak searches only near it
    void set_search_hz(double hz) {
        max_off_ = hz;
        make_acq();
    }

private:
    enum State { ACQ, LOCK };
    State state_ = ACQ;
    std::string mode_ = "auto";
    int64_t n_ = 0;
    double tbase_ = 0;
    bool tbase_set_ = false;
    std::deque<cf> hist_;
    int64_t hist_base_ = 0;
    std::unique_ptr<aero::Acquisition> acq_;
    aero::Demod demod_;
    aero::PChannel pch_;
    aero::CChannel cch_;
    aero::SuDecoder su_;
    aero::BurstRx burst_;
    bool cmode_ = false;            // locked on a C channel (else a P channel)
    std::vector<aero::SoftBit> bits_;
    int64_t lock_at_ = 0;
    double lock_rate_ = 0, lock_f0_ = 0;
    float lock_snr_ = 0;
    aero::DemodCfg lock_cfg_;
    bool restarted_ = false;
    int c_restarts_ = 0;
    int64_t c_check_from_ = 0;
    double max_off_ = 2900;
    uint64_t su_at_lock_ = 0;
    std::vector<std::pair<double, double>> tried_;   // (rate, f0) that did not sync
    double last_fact_ = -10;
    double latest_t_ = 0;   // host time of the newest bit the demodulator gave
    double rf_seen_ = 0;    // the VFO's RF last seen (a move restarts the search)
    dig::AeroAmbeStream voice_;
    bool in_call_ = false;
    double call_start_ = 0, call_last_ = 0;
    uint64_t cframes_ = 0, cframes_ok_ = 0, vframes_ = 0, verr_sum_ = 0, verr_bad_ = 0;
    const bool debug_ = std::getenv("AERO_DEBUG") != nullptr;
    // the debug lines (AERO_DEBUG=1), to the host's log. A plugin writes no
    // files (SDK rule 5); the off-air same-sample comparison used a test
    // build that also appended them to a file - docs/dev/plugin-inmarsat-aero.md
    void dbg(const std::string& s) { host.log(s); }

    bool bursts() const { return mode_.rfind("burst", 0) == 0; }
    bool synced_now() const { return cmode_ ? cch_.synced() : pch_.synced(); }
    double host_t(double sample) const { return tbase_ + sample / FS; }

    void make_acq() {
        std::vector<double> wide, narrow;
        auto want = [&](double r) {
            const std::string k = r == 8400 ? "c8400" : "p" + std::to_string(static_cast<int>(r));
            return mode_ == "auto" || mode_ == k;
        };
        for (double r : {4800.0, 8400.0, 10500.0})
            if (want(r)) wide.push_back(r);
        for (double r : {600.0, 1200.0, 2400.0})
            if (want(r)) narrow.push_back(r);
        acq_ = std::make_unique<aero::Acquisition>(FS, wide, narrow, max_off_, 3);
    }

    void try_lock() {
        if (debug_) {
            std::string all = "acq: ";
            for (const auto& h : acq_->hits()) {
                char e[64];
                snprintf(e, sizeof e, "%d@%+.1f/%.1fdB/k%d ", static_cast<int>(h.rate), h.f0, h.snr_db, h.k);
                all += e;
            }
            dbg(all);
        }
        const aero::AcqHit* best = nullptr;
        const bool narrow_wanted = mode_ == "auto" || mode_ == "p600" || mode_ == "p1200" || mode_ == "p2400";
        bool narrow_seen = false;
        for (const auto& h : acq_->hits())
            if (h.rate <= 2400) narrow_seen = true;
        for (const auto& h : acq_->hits()) {
            if (h.snr_db < acq_threshold_db(h.k)) continue;
            // a wide-band rate before the narrow band's first result only when
            // clearly so: a strong 600 / 1200 bit/s carrier scores up to 16 dB
            // as 4800 / 8400 bit/s there
            if (h.rate >= 4800 && narrow_wanted && !narrow_seen && h.snr_db < acq_threshold_db(h.k) + 4) continue;
            bool bad = false;
            for (const auto& t : tried_)
                if (t.first == h.rate && std::fabs(t.second - h.f0) < 20) bad = true;
            if (bad) continue;
            if (!best || h.snr_db > best->snr_db) best = &h;
        }
        // a lower rate's harmonics look like a higher rate's pair at the
        // same carrier: prefer the lowest rate found there
        if (best)
            for (const auto& h : acq_->hits())
                if (h.snr_db >= acq_threshold_db(h.k) && h.rate < best->rate && std::fabs(h.f0 - best->f0) < 10) best = &h;
        if (!best) return;
        aero::DemodCfg c;
        c.fs = FS;
        const aero::PRate* pr = aero::p_rate(best->rate);
        cmode_ = best->rate == 8400;
        if (cmode_) {
            c.mod = aero::Mod::AQPSK;
            c.rate = 8400;
            c.decim = 1;
            c.rolloff = 0.6f;
            c.pll_bw = 0.0015f;
            c.tim_gain = 1.2e-4f;
            cch_.start();
        } else if (pr) {
            c.mod = pr->mod;
            c.rate = pr->rate;
            c.decim = pr->rate <= 1200 ? 5 : pr->rate <= 2400 ? 2 : 1;
            c.rolloff = pr->mod == aero::Mod::AQPSK ? 0.6f : 1.0f;
            c.pll_bw = static_cast<float>(std::max(0.0012, 5.0 / pr->rate));   // loop ~5 Hz (A-BPSK) .. 12 Hz
            c.tim_gain = pr->rate >= 4800 ? 1.2e-4f : 2e-4f;
            pch_.start(*pr);
        } else {
            return;
        }
        if (debug_) {
            char e[64];
            snprintf(e, sizeof e, "lock %d@%+.1f", static_cast<int>(best->rate), best->f0);
            dbg(e);
        }
        lock_rate_ = best->rate;
        lock_f0_ = best->f0;
        lock_snr_ = best->snr_db;
        su_at_lock_ = su_.su_ok();
        lock_cfg_ = c;
        restarted_ = false;
        c_restarts_ = 0;
        c_check_from_ = n_;
        state_ = LOCK;
        host.fact("Channel", channel_name() + ": syncing");
        start_demod(0);
    }

    // the demodulator from the oldest sample of the history (the samples the
    // acquisition saw), `skip` input samples later
    void start_demod(size_t skip) {
        skip = std::min(skip, hist_.size());
        lock_at_ = hist_base_ + static_cast<int64_t>(skip);
        demod_.start(lock_cfg_, lock_f0_, static_cast<double>(lock_at_));
        std::vector<cf> h(hist_.begin() + static_cast<long>(skip), hist_.end());
        run_demod(h.data(), h.size());
    }

    // Off air, from some starting points the A-BPSK demodulator settled on a
    // wrong timing / phase equilibrium (quality ~0.87 instead of ~0.98 on a
    // strong carrier) and stayed there - UWs found, every SU failing - for
    // up to 16 s; started half a bit later it locks at once. So: a strong
    // carrier, a second of demodulation, low quality and no SU yet -> start
    // the demodulator again on the history, half a bit later (once)
    void check_stuck() {
        if (cmode_) {
            // a C channel (A-QPSK, 15 dB less margin): its quality says
            // little (0.8 working or not), so: 1.5 s since the lock / the last
            // restart without a clean frame -> again, up to 3 times
            if (c_restarts_ >= 3 || cframes_ok_ > 0) return;
            if (static_cast<double>(n_ - c_check_from_) / FS < 1.5) return;
            c_restarts_++;
            c_check_from_ = n_;
            if (debug_) dbg("restart C demod (" + std::to_string(c_restarts_) + ")");
            cch_.start();
            start_demod(static_cast<size_t>(std::lround(0.5 * c_restarts_ * FS / lock_rate_)));
            return;
        }
        if (restarted_ || su_.su_ok() > su_at_lock_) return;
        const double since = static_cast<double>(n_ - lock_at_) / FS;
        if (lock_snr_ < 20 || since < 1.0 || demod_.quality() >= 0.93) return;
        restarted_ = true;
        if (debug_) dbg("restart demod (quality " + std::to_string(demod_.quality()) + ")");
        const aero::PRate* pr = aero::p_rate(lock_rate_);
        if (pr) pch_.start(*pr);
        start_demod(static_cast<size_t>(std::lround(0.5 * FS / lock_rate_)));
    }

    std::string channel_name() const {
        char b[96];
        if (cmode_) snprintf(b, sizeof b, "C channel 8400 bit/s A-QPSK (voice)");
        else
            snprintf(b, sizeof b, "P channel %d bit/s %s", static_cast<int>(lock_rate_), lock_rate_ >= 4800 ? "A-QPSK" : "A-BPSK");
        return b;
    }

    void run_demod(const cf* x, size_t n) {
        bits_.clear();
        demod_.process(x, n, bits_);
        // bit times are input sample indexes: make them host times
        for (auto& b : bits_) b.t = host_t(b.t);
        if (!bits_.empty()) latest_t_ = bits_.back().t;
        if (cmode_) cch_.push(bits_.data(), bits_.size());
        else pch_.push(bits_.data(), bits_.size());
    }

    void unlock(const char* why) {
        if (debug_) dbg(std::string("unlock: ") + why + " q=" + std::to_string(demod_.quality()));
        if (std::string(why) == "signal lost") {
            tried_.clear();
        } else {
            tried_.push_back({lock_rate_, lock_f0_});
            if (tried_.size() > 1) tried_.erase(tried_.begin());   // only the last failure: a strong one is tried again next
        }
        demod_.stop();
        pch_.stop();
        cch_.stop();
        end_call();
        state_ = ACQ;
        host.fact("Channel", std::string("searching (") + why + ")");
    }

    void supervise() {
        const double since = static_cast<double>(n_ - lock_at_) / FS;
        const double frame_s = cmode_ ? 0.5 : pch_.rate().frame / pch_.rate().rate;
        const bool synced = cmode_ ? cch_.synced() : pch_.synced();
        const uint64_t frames = cmode_ ? cch_.frames() : pch_.frames();
        if (!synced && frames == 0 && since > SYNC_TIMEOUT_FRAMES * frame_s + HIST_S) {
            unlock("no frame sync");
            return;
        }
        if (frames > 0 && !synced) {
            unlock(cmode_ ? "C channel ended" : "signal lost");
            return;
        }
        const double t = host.time();
        if (t - last_fact_ >= 1.0 && synced) {
            last_fact_ = t;
            host.fact("Channel", channel_name());
            char b[64];
            snprintf(b, sizeof b, "%+.1f Hz", demod_.carrier_hz());
            host.fact("Carrier offset", b);
            const double rf = host.rf_hz();
            if (std::isfinite(rf) && rf > 0) su_.set_channel_hz(rf + demod_.carrier_hz());
        }
        // No freq_error: kraken_doa's AFC moves the input by half of each
        // report every 300 ms - frequency steps the carrier loop has to
        // recover from (measured live: a 10.5 kbit/s P channel lost 4 of 94
        // frames to the steps). The plugin finds and tracks +-2.5 kHz
        // itself; the offset is the "Carrier offset" fact and, for bursts,
        // the talker packet's freq_hz.
        if (in_call_ && host.time() - call_last_ > 1.5) end_call();
    }

    // A checked frame's samples for the Digital squelch's DoA. kraken_doa
    // keeps the VFO's stream ~1.5 s (talker_doa.hpp RING_S): a 2 s frame at
    // 600 bit/s reaches back further than that even when it is reported
    // the moment it is in (PChannel's early pass) - report only the part
    // within the last 1.2 s (kraken_doa drops a span it no longer has).
    void valid_span(double t0, double t1) {
        const double from = std::max(t0, latest_t_ - 1.2);
        if (t1 > from) host.valid(from, t1);
        else host.valid();
    }

    void on_pframe(const aero::PFrame& f) {
        if (debug_) {
            char b[160];
            snprintf(b, sizeof b, "frame t=%.3f no=%d hdr=%d uwerr=%d sus %d/%d q=%.3f f=%.1f", f.t0, f.frame_no, f.header_ok,
                     f.uw_errors, f.sus_ok, f.sus, demod_.quality(), demod_.carrier_hz());
            dbg(b);
        }
        if (f.sus_ok > 0 && f.sus_early == 0) valid_span(f.t0, f.t1);   // else on_early gave it
        su_.frame_done(f.sus, f.sus_ok);
    }

    void on_cframe(const aero::CFrame& f) {
        cframes_++;
        if (debug_) {
            char b[160];
            snprintf(b, sizeof b, "C frame t=%.3f uwerr=%d ref errors %d/26 sus %d/3 q=%.3f", f.t0, f.uw_errors, f.ref_errors,
                     f.sus_ok, demod_.quality());
            dbg(b);
        }
        // a frame counts when a sub-band SU passed its CRC, or the reference
        // bits (26 known "1"s) came through nearly clean
        const bool ok = f.sus_ok > 0 || f.ref_errors <= 2;
        if (!ok) return;
        cframes_ok_++;
        valid_span(f.t0, f.t1);
        su_.frame_done(3, f.sus_ok);
        if (!in_call_) {
            in_call_ = true;
            call_start_ = f.t0;
            voice_.reset();
        }
        call_last_ = f.t1;
        // talker: a from-aircraft C channel (1.6 GHz) is the AES's transmission
        talker_frame(f);
        char b[120];
        snprintf(b, sizeof b, "%llu frames (%llu clean), reference bit errors %d/26 in the last",
                 static_cast<unsigned long long>(cframes_), static_cast<unsigned long long>(cframes_ok_), f.ref_errors);
        host.fact("C channel frames", b);
        if (!host.voice_wanted()) return;
        if (!dig::AeroAmbeLib::instance().available()) {
            host.voice_state("AMBE codec not installed");
            return;
        }
        std::vector<float> pcm(25 * 160);
        int esum = 0, emax = 0;
        for (int k = 0; k < 25; k++) {
            const int e = voice_.decode(f.voice[static_cast<size_t>(k)].data(), &pcm[static_cast<size_t>(160 * k)]);
            vframes_++;
            if (e > 0) {
                esum += e;
                emax = std::max(emax, e);
                verr_sum_ += static_cast<uint64_t>(e);
                if (e > 2) verr_bad_++;
            }
        }
        host.audio(pcm.data(), pcm.size());
        // the codec's own error count (mbelib's Golay corrections, errs2)
        // per 20 ms frame: near 0 for a voice frame received cleanly, high
        // for random bits - the check of the frame layout on a real call
        {
            char b[120];
            snprintf(b, sizeof b, "%llu frames, %.2f corrections per frame, %llu with more than 2",
                     static_cast<unsigned long long>(vframes_), static_cast<double>(verr_sum_) / static_cast<double>(vframes_),
                     static_cast<unsigned long long>(verr_bad_));
            host.fact("Voice codec frames", b);
            if (debug_) {
                snprintf(b, sizeof b, "voice t=%.3f errs2 sum %d max %d (25 frames)", f.t0, esum, emax);
                dbg(b);
            }
        }
        const std::string who = su_.call_aes();
        host.voice_state("voice" + (who.empty() ? std::string() : " · AES " + who));
    }

    void talker_frame(const aero::CFrame& f) {
        const double rf = host.rf_hz();
        const std::string aes = su_.call_aes();
        if (!(std::isfinite(rf) && rf > 1600e6) || aes.empty()) return;
        kp::Talker t;
        t.id = aes;
        t.label = "voice";
        t.start_s = call_start_;
        t.end_s = f.t1;
        host.talker(t);
    }

    void end_call() {
        if (!in_call_) return;
        in_call_ = false;
        const double rf = host.rf_hz();
        if (std::isfinite(rf) && rf > 1600e6 && !su_.call_aes().empty()) host.talker_end(call_last_);
        host.voice_state("");
    }
};

}  // namespace aero
