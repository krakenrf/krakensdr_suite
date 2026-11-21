// Inmarsat Classic Aero (AMSS, ICAO Annex 10 Vol III Part I chapter 4):
// P channel (satellite -> aircraft) at 0.6 / 1.2 / 2.4 kbit/s A-BPSK and
// 4.8 / 10.5 kbit/s A-QPSK, found and demodulated coherently, frames,
// signal units checked by their CRC, decoded by type; user data
// reassembled into ACARS messages. See README.md and
// kraken_doa_v2/docs/dev/plugin-inmarsat-aero.md.

#include "aero_acq.hpp"
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
constexpr float ACQ_SNR_DB = 17.0f;     // weaker squared-spectrum line over the median (noise: ~12 dB at most)
constexpr double HIST_S = 3.0;          // input kept to start the demodulator on
constexpr double SYNC_TIMEOUT_FRAMES = 4.5;

struct ModeDef {
    const char* key;
    double rate;        // 0 = any P channel rate
};

class AeroPlugin : public kp::Decoder {
public:
    explicit AeroPlugin(kp::Host& h) : Decoder(h), su_(h) {
        pch_.on_su = [this](const uint8_t* su, const aero::PFrame& f, int idx) { su_.p_su(su, f.t0, f.t1, idx); };
        pch_.on_frame = [this](const aero::PFrame& f) { on_frame(f); };
        reset();
    }

    void reset() override {
        n_ = 0;
        tbase_set_ = false;
        hist_.clear();
        hist_base_ = 0;
        demod_.stop();
        pch_.stop();
        su_.reset();
        make_acq();
        state_ = ACQ;
        tried_.clear();
        last_fact_ = -10;
        frames_ok_ = frames_ = 0;
        host.fact("Channel", "searching");
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
        if (!tbase_set_) {
            tbase_ = host.time();
            tbase_set_ = true;
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
    aero::SuDecoder su_;
    std::vector<aero::SoftBit> bits_;
    int64_t lock_at_ = 0;           // sample index where the demodulator started
    double lock_rate_ = 0, lock_f0_ = 0;
    std::vector<std::pair<double, double>> tried_;   // (rate, f0) that did not sync
    double last_fact_ = -10, last_ferr_ = -10;
    uint64_t frames_ok_ = 0, frames_ = 0;
    const bool debug_ = std::getenv("AERO_DEBUG") != nullptr;

    double host_t(double sample) const { return tbase_ + sample / FS; }

    void make_acq() {
        std::vector<double> wide, narrow;
        auto want = [&](double r) { return mode_ == "auto" || mode_ == "p" + std::to_string(static_cast<int>(r)); };
        for (double r : {4800.0, 10500.0})
            if (want(r)) wide.push_back(r);
        for (double r : {600.0, 1200.0, 2400.0})
            if (want(r)) narrow.push_back(r);
        acq_ = std::make_unique<aero::Acquisition>(FS, wide, narrow, 2900.0, 2);
    }

    void try_lock() {
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
        const aero::PRate* pr = aero::p_rate(best->rate);
        if (!pr) return;
        aero::DemodCfg c;
        c.mod = pr->mod;
        c.rate = pr->rate;
        c.fs = FS;
        c.decim = pr->rate <= 1200 ? 5 : pr->rate <= 2400 ? 2 : 1;
        c.rolloff = pr->mod == aero::Mod::AQPSK ? 0.6f : 1.0f;
        c.pll_bw = static_cast<float>(std::max(0.0012, 5.0 / pr->rate));   // loop ~5 Hz (A-BPSK) .. 12 Hz
        c.tim_gain = pr->rate >= 4800 ? 1.2e-4f : 2e-4f;
        lock_rate_ = best->rate;
        lock_f0_ = best->f0;
        lock_at_ = hist_base_;
        if (const char* e = std::getenv("AERO_BW")) c.pll_bw = static_cast<float>(atof(e));     // TUNING
        if (const char* e = std::getenv("AERO_TG")) c.tim_gain = static_cast<float>(atof(e));   // TUNING
        if (const char* e = std::getenv("AERO_RO")) c.rolloff = static_cast<float>(atof(e));    // TUNING
        demod_.start(c, best->f0, static_cast<double>(hist_base_));
        if (const char* e = std::getenv("AERO_TS")) demod_.set_tsign(static_cast<float>(atof(e)));   // TUNING
        pch_.start(*pr);
        state_ = LOCK;
        char b[120];
        snprintf(b, sizeof b, "P channel %d bit/s %s, carrier %+.0f Hz (line %.0f dB): syncing",
                 static_cast<int>(pr->rate), pr->mod == aero::Mod::AQPSK ? "A-QPSK" : "A-BPSK", best->f0, best->snr_db);
        host.fact("Channel", b);
        if (debug_) {
            std::string all = "lock attempt: ";
            for (const auto& h : acq_->hits()) all += std::to_string(static_cast<int>(h.rate)) + "@" + std::to_string(h.f0) + "/" + std::to_string(h.snr_db) + " ";
            host.log(all);
        }
        // run it over the history first (the samples the acquisition saw)
        std::vector<cf> h(hist_.begin(), hist_.end());
        run_demod(h.data(), h.size());
    }

    void run_demod(const cf* x, size_t n) {
        bits_.clear();
        demod_.process(x, n, bits_);
        // bit times are input sample indexes: make them host times
        for (auto& b : bits_) b.t = host_t(b.t);
        pch_.push(bits_.data(), bits_.size());
    }

    void unlock(const char* why) {
        if (debug_) host.log(std::string("unlock: ") + why + " q=" + std::to_string(demod_.quality()));
        if (std::string(why) == "signal lost") {
            tried_.clear();
        } else {
            tried_.push_back({lock_rate_, lock_f0_});
            if (tried_.size() > 6) tried_.erase(tried_.begin());
        }
        demod_.stop();
        pch_.stop();
        state_ = ACQ;
        host.fact("Channel", std::string("searching (") + why + ")");
    }

    void supervise() {
        const double since = static_cast<double>(n_ - lock_at_) / FS;
        const aero::PRate& r = pch_.rate();
        const double frame_s = r.frame / r.rate;
        if (!pch_.synced() && pch_.frames() == 0 && since > SYNC_TIMEOUT_FRAMES * frame_s + HIST_S) {
            unlock("no frame sync");
            return;
        }
        if (pch_.frames() > 0 && !pch_.synced()) {
            tried_.clear();
            unlock("signal lost");
            return;
        }
        const double t = host.time();
        if (t - last_fact_ >= 1.0 && pch_.synced()) {
            last_fact_ = t;
            char b[160];
            snprintf(b, sizeof b, "P channel %d bit/s %s", static_cast<int>(r.rate), r.mod == aero::Mod::AQPSK ? "A-QPSK" : "A-BPSK");
            host.fact("Channel", b);
            snprintf(b, sizeof b, "%+.1f Hz", demod_.carrier_hz());
            host.fact("Carrier offset", b);
        }
    }

    void on_frame(const aero::PFrame& f) {
        frames_++;
        if (debug_) {
            char b[160];
            snprintf(b, sizeof b, "frame t=%.3f no=%d hdr=%d uwerr=%d sus %d/%d q=%.3f f=%.1f", f.t0, f.frame_no, f.header_ok,
                     f.uw_errors, f.sus_ok, f.sus, demod_.quality(), demod_.carrier_hz());
            host.log(b);
        }
        if (f.sus_ok > 0) {
            frames_ok_++;
            host.valid(f.t0, f.t1);
        }
        su_.frame_done(f.sus, f.sus_ok);
    }
};

}  // namespace

KRAKEN_PLUGIN(AeroPlugin, {.id = "inmarsat_aero",
                           .name = "Inmarsat Aero (Classic)",
                           .category = "Aviation",
                           .description = "Inmarsat Classic Aero, L band 1.5 GHz: P channel (0.6 / 1.2 kbit/s A-BPSK, 10.5 kbit/s "
                                          "A-QPSK) found and decoded - signal units, log-ons, C-channel assignments, ACARS "
                                          "and ADS-C positions",
                           .version = "1.0",
                           .sample_rate = 24000,
                           .min_vfo_rate = 15000,
                           .author = "KrakenRF",
                           .options = {{"mode", "Channel", "auto",
                                        "auto=Auto|p600=P 600 bit/s|p1200=P 1200 bit/s|p2400=P 2400 bit/s|p4800=P 4800 bit/s|"
                                        "p10500=P 10500 bit/s",
                                        "Auto finds the rate; the others search only that one"}},
                           .map = true,
                           .messages = false,
                           .talkers = true})
