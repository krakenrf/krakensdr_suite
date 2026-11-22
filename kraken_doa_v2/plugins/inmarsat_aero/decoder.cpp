// Inmarsat Classic Aero (AMSS, ICAO Annex 10 Vol III Part I chapter 4):
// - P channel (GES -> aircraft, continuous TDM) at 0.6 / 1.2 / 2.4 kbit/s
//   A-BPSK and 4.8 / 10.5 kbit/s A-QPSK: found by the acquisition (rate
//   and carrier), demodulated coherently, frames, signal units checked by
//   their CRC and decoded by type, user data reassembled into ACARS (and
//   ADS-C);
// - C channel (voice, 8.4 kbit/s A-QPSK): frames, sub-band signal units,
//   the AMBE voice frames through the user's codec library (aero_ambe).
// R / T channel bursts from aircraft: aero_burst.cpp. See README.md and
// kraken_doa_v2/docs/dev/plugin-inmarsat-aero.md.

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

namespace {

using aero::cf;
constexpr double FS = 24000;
constexpr float ACQ_SNR_DB = 13.5f;   // mean of the two squared-spectrum lines over the median (noise: <= 10.3 dB in 220 searches)
constexpr double HIST_S = 3.0;        // input kept to start the demodulator on
constexpr double SYNC_TIMEOUT_FRAMES = 6.5;

class AeroPlugin : public kp::Decoder {
public:
    explicit AeroPlugin(kp::Host& h) : Decoder(h), su_(h), burst_(h, su_) {
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
        // carrier for a minute (408 of 960 SUs); from a fresh start, 594 / 594
        const double rf = host.rf_hz();
        if (std::isfinite(rf) && rf > 0) {
            if (rf_seen_ > 0 && std::fabs(rf - rf_seen_) > 100) reset();
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
        n_ += static_cast<int64_t>(n);
        if (state_ == LOCK) supervise();
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
        acq_ = std::make_unique<aero::Acquisition>(FS, wide, narrow, 2900.0, 2);
    }

    void try_lock() {
        if (debug_) {
            std::string all = "acq: ";
            for (const auto& h : acq_->hits()) {
                char e[64];
                snprintf(e, sizeof e, "%d@%+.1f/%.1fdB ", static_cast<int>(h.rate), h.f0, h.snr_db);
                all += e;
            }
            dbg(all);
        }
        const aero::AcqHit* best = nullptr;
        for (const auto& h : acq_->hits()) {
            if (h.snr_db < ACQ_SNR_DB) continue;
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
                if (h.snr_db >= ACQ_SNR_DB && h.rate < best->rate && std::fabs(h.f0 - best->f0) < 10) best = &h;
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
        lock_rate_ = best->rate;
        lock_f0_ = best->f0;
        lock_at_ = hist_base_;
        demod_.start(c, best->f0, static_cast<double>(hist_base_));
        state_ = LOCK;
        host.fact("Channel", channel_name() + ": syncing");
        // run it over the history first (the samples the acquisition saw)
        std::vector<cf> h(hist_.begin(), hist_.end());
        run_demod(h.data(), h.size());
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

}  // namespace

KRAKEN_PLUGIN(AeroPlugin, {.id = "inmarsat_aero",
                           .name = "Inmarsat Aero (Classic)",
                           .category = "Aviation",
                           .description = "Inmarsat Classic Aero, L band: P channel (0.6 / 1.2 kbit/s A-BPSK, 10.5 kbit/s A-QPSK; "
                                          "rate and carrier found automatically) - signal units, log-ons, C channel assignments, "
                                          "ACARS, ADS-C positions; C channel voice (AMBE, README \"Digital voice codecs\"); "
                                          "aircraft R / T bursts (1.6 GHz) with a DoA per aircraft. Bandwidth 15 kHz.",
                           .version = "1.0",
                           .sample_rate = 24000,
                           .min_vfo_rate = 15000,
                           .author = "KrakenRF",
                           .options = {{"mode", "Channel", "auto",
                                        "auto=Auto (P / C channel)|p600=P 600 bit/s|p1200=P 1200 bit/s|p2400=P 2400 bit/s|"
                                        "p4800=P 4800 bit/s|p10500=P 10500 bit/s|c8400=C channel 8400 bit/s (voice)|"
                                        "burst=R / T bursts from aircraft (1.6 GHz)",
                                        "Auto finds a continuous channel's rate and carrier; the P / C choices search only that "
                                        "rate; R / T bursts listens for aircraft transmissions at every burst rate"}},
                           .map = true,
                           .voice = true,
                           .messages = false,
                           .talkers = true})
