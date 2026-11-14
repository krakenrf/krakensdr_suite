// Digital TV (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB): what a broadcast TV
// signal tells about itself in the slice of the channel the VFO sees - the
// standard and its mode, the channel, the signalling (DVB-T TPS: modulation,
// code rate, guard, cell ID, net bit rate; ISDB-T TMCC: the hierarchical
// layers, partial reception, the emergency warning flag) and the echo
// profile of the channel impulse response (CIR), measured against the
// signal's KNOWN reference - pilots, field sync, PN header.
//
// Optional path separation (option `separate`, off by default): the signal
// arrives on several paths - reflections, and in a single frequency network
// (SFN) the other transmitters too: delayed copies of the same signal from
// their own directions, which nothing in a TV signal tells apart. Every
// antenna's CIR separates them by delay, and each path's spatial covariance
// goes to kraken_doa (kp::Spatial), which runs MUSIC on it: a talker
// "Path-n" with its own DoA lobe on the 🗺 Map's 📡 DF panel, delays from
// the main (strongest) path. An ARRAY plugin: it gets every antenna
// (coherent mode) in bursts of 0.35 s every second; with separation off
// only antenna 0 is analysed.
//
// Per burst: the standard's estimator (auto: the first that recognises the
// burst, kept while it stays locked) emits CIR snapshots + its signalling;
// sfn_core.cpp sums the snapshots per tap and finds the paths (the echo
// profile, or tracked paths with bearings). See README.md.

#include "kraken_plugin.hpp"

#include "estimator.hpp"
#include "sfn_core.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr double BURST_S = 0.35;
constexpr double PERIOD_S = 1.0;

// The broadcast channel whose centre is f (Hz, within 50 kHz of a raster
// centre) for the channel width found; "" = on no raster known here
std::string channel_label(double f, double bw_mhz, const std::string& standard) {
    if (!std::isfinite(f)) return "";
    int n = 0;
    auto grid = [&](double c0, double step, int n0, int n1) {
        const double k = std::round((f - c0) / step);
        if (k < 0 || k > n1 - n0 || std::fabs(f - (c0 + k * step)) > 50e3) return false;
        n = n0 + static_cast<int>(k);
        return true;
    };
    auto s = [](int v) { return std::to_string(v); };
    const long bw = std::lround(bw_mhz);
    if (standard == "ISDB-T") {
        // 6 MHz with the 1/7 MHz offset: Japan numbers from 13, the Americas from 14
        if (grid(473e6 + 1e6 / 7, 6e6, 13, 62)) return s(n) + " (Japan) / " + s(n + 1) + " (Americas)";
        if (grid(177e6 + 1e6 / 7, 6e6, 7, 13)) return s(n) + " (VHF, Americas)";
    }
    if (standard == "DTMB" && bw == 8) {
        // China: DS-6..12 VHF, DS-13..24 below 566 MHz, DS-25.. from 606 MHz
        if (grid(171e6, 8e6, 6, 12) || grid(474e6, 8e6, 13, 24) || grid(610e6, 8e6, 25, 68)) return "DS-" + s(n) + " (China)";
    }
    if (bw == 6) {
        if (grid(57e6, 6e6, 2, 4) || grid(79e6, 6e6, 5, 6) || grid(177e6, 6e6, 7, 13) || grid(473e6, 6e6, 14, 69))
            return s(n) + " (North America / Korea)";
    }
    if (bw == 7) {
        static const char* AU[8] = {"6", "7", "8", "9", "9A", "10", "11", "12"};
        if (grid(177.5e6, 7e6, 5, 12)) return "E" + s(n) + " (Europe) / " + AU[n - 5] + " (Australia)";
        if (grid(529.5e6, 7e6, 28, 51)) return s(n) + " (Australia UHF)";
    }
    if (bw == 8) {
        if (grid(474e6, 8e6, 21, 69)) return s(n) + " (UHF, 8 MHz raster)";
    }
    return "";
}

class DtvPlugin : public kp::Decoder {
public:
    explicit DtvPlugin(kp::Host& h) : Decoder(h), core_(h) {}   // the core starts profile-only (separation off)

    void process(const kp::cf*, size_t) override {}   // array plugin: process_array

    void process_array(const kp::ArrayBlock& b) override {
        if (b.m < 1 || b.n == 0) return;
        // a new burst: the previous one is complete
        const bool shape = !burst_.ch.empty() && (b.m != antennas_ || b.rate != burst_.rate);
        if ((b.gap || shape) && burst_.n > 0) finish();
        // separation off: antenna 0 is all the profile and the signalling need
        const int m = separate_ ? b.m : 1;
        if (burst_.n == 0) {
            burst_.m = m;
            burst_.rate = b.rate;
            burst_.t0 = b.t0;
            burst_.rf_hz = host.rf_hz();
            burst_.ch.assign(static_cast<size_t>(m), {});
            antennas_ = b.m;
        }
        for (int k = 0; k < burst_.m && k < b.m; k++)
            burst_.ch[static_cast<size_t>(k)].insert(burst_.ch[static_cast<size_t>(k)].end(), b.ch[k], b.ch[k] + b.n);
        burst_.n += b.n;
        // the whole burst arrived (the host sends whole blocks: >= BURST_S)
        if (static_cast<double>(burst_.n) >= BURST_S * b.rate - 1) finish();
    }

    void reset() override {
        burst_ = sfn::Burst{};
        active_ = nullptr;
        core_.reset();
        est_.clear();
        locked_name_.clear();
        searches_ = 0;
        clear_info();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "standard") {
            // an unknown value (a saved "dab" from before DAB moved out) = auto
            static const std::set<std::string> KNOWN = {"auto", "dvbt", "dvbt2", "isdbt", "atsc", "atsc3", "dtmb"};
            const std::string v = KNOWN.count(value) ? value : "auto";
            if (v != standard_) {
                standard_ = v;
                reset();
            }
        } else if (key == "separate") {
            const bool on = value == "1";
            if (on != separate_) {
                separate_ = on;
                burst_ = sfn::Burst{};          // a burst being collected has the other antenna count
                core_.set_profile_only(!on);
            }
        } else if (key == "bw") {
            opts_.bw_mhz = value == "auto" || value.empty() ? 0 : atof(value.c_str());
            reset();
        } else if (key == "channel_mhz") {
            const double v = atof(value.c_str());
            opts_.channel_hz = v > 0 ? v * 1e6 : NAN;
            reset();
        } else if (key == "min_snr") {
            const double v = atof(value.c_str());
            core_.set_min_snr_db(v > 0 ? v : 8);
        } else if (key == "max_rel_db") {
            const double v = atof(value.c_str());
            core_.set_max_rel_db(value.empty() ? 30 : std::max(0.0, v));
        } else if (key == "max_tx") {
            const int v = atoi(value.c_str());
            core_.set_max_tx(v > 0 ? std::min(v, 16) : 8);
        } else if (key == "verbose") {
            opts_.verbose = value == "1";
        }
    }

private:
    sfn::SfnCore core_;
    sfn::Burst burst_;
    sfn::EstOptions opts_;
    std::string standard_ = "auto";
    bool separate_ = false;
    int antennas_ = 0;
    std::vector<std::unique_ptr<sfn::Estimator>> est_;
    sfn::Estimator* active_ = nullptr;
    std::string locked_name_;
    int searches_ = 0;              // bursts searched without a lock
    std::set<std::string> info_keys_;   // facts from the estimator's info() shown now

    void build() {
        auto add = [&](std::unique_ptr<sfn::Estimator> e) {
            if (e) est_.push_back(std::move(e));
        };
        const std::string& s = standard_;
        // DVB-T2 before DVB-T: the same FFT sizes / guards, its P1 tells them apart
        if (s == "auto" || s == "dvbt2") add(sfn::make_dvbt2(host));
        if (s == "auto" || s == "dvbt") add(sfn::make_dvbt(host));
        if (s == "auto" || s == "isdbt") add(sfn::make_isdbt(host));
        if (s == "auto" || s == "atsc") add(sfn::make_atsc(host));
        if (s == "auto" || s == "atsc3") add(sfn::make_atsc3(host));
        if (s == "auto" || s == "dtmb") add(sfn::make_dtmb(host));
    }

    // the signalling facts of a standard no longer locked
    void clear_info() {
        for (const auto& k : info_keys_) host.fact(k, "");
        info_keys_.clear();
        host.fact("Channel", "");
    }

    void show_info() {
        std::set<std::string> now;
        for (const auto& kv : active_->info()) {
            host.fact(kv.first, kv.second);
            now.insert(kv.first);
        }
        for (const auto& k : info_keys_)
            if (!now.count(k)) host.fact(k, "");
        info_keys_ = std::move(now);
        for (const auto& n : active_->take_news()) host.event(n, 0.0);
        const std::string ch = channel_label(active_->centre_hz(), active_->bw_mhz(), active_->name());
        host.fact("Channel", ch);
    }

    void finish() {
        sfn::Burst b;
        std::swap(b, burst_);
        burst_ = sfn::Burst{};
        // a fragment (the host's blocks overshooting the burst) is too short
        if (b.n < 1000 || static_cast<double>(b.n) < 0.5 * BURST_S * b.rate) return;
        if (est_.empty()) build();
        // searching costs CPU (every mode of every standard): after 10 bursts
        // without a signal, look at every 4th burst only
        if (!active_ && ++searches_ > 10 && searches_ % 4 != 0) return;
        if (!active_) {
            double best = 0;
            for (auto& e : est_) {
                const double sc = e->detect(b, opts_);
                if (sc > best) {
                    best = sc;
                    active_ = e.get();
                }
            }
            if (!active_ || best < 0.2) {
                active_ = nullptr;
                host.fact("Standard", "searching (" + standard_ + ")");
                return;
            }
            searches_ = 0;
            if (active_->name() != locked_name_) {
                core_.reset();
                clear_info();
                locked_name_ = active_->name();
                host.event("Locked: " + active_->describe(), 0.0);
            }
        }
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = active_->process(b, opts_, [this](const sfn::CirSnapshot& s) { core_.add(s); });
        const auto t1 = std::chrono::steady_clock::now();
        host.fact("Standard", active_->describe());
        host.fact("Antennas", separate_ ? std::to_string(b.m) + (b.m < 2 ? " (no bearings outside coherent mode)" : "")
                                        : std::to_string(antennas_) + " (path separation off: antenna 0 analysed)");
        show_info();
        core_.set_ids(active_->has_ids(), active_->ids());
        core_.set_profile_floor_db(active_->profile_floor_db());
        core_.cycle(active_->describe());
        if (ok) host.valid(b.t0, b.t0 + static_cast<double>(b.n) / b.rate);
        if (opts_.verbose) {
            const auto t2 = std::chrono::steady_clock::now();
            char buf[96];
            snprintf(buf, sizeof buf, "time: estimator %.3f s, core %.3f s", std::chrono::duration<double>(t1 - t0).count(),
                     std::chrono::duration<double>(t2 - t1).count());
            host.log(buf);
        }
        if (!ok) {
            host.event("Lost " + active_->name() + " - searching", 0.0);
            active_ = nullptr;
        }
    }
};

}  // namespace

KRAKEN_PLUGIN(DtvPlugin,
              {.id = "dtv",
               .name = "Digital TV (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB)",
               .category = "Broadcast & cellular",
               .description = "Broadcast TV signals: standard, mode, channel, DVB-T TPS (constellation, code rate, guard, cell ID, "
                              "bit rate), ISDB-T TMCC (layers, emergency warning flag) and the echo profile - plus optional path "
                              "separation: every path the signal arrives on (a reflection, or another transmitter of a single "
                              "frequency network) with its own DoA lobe",
               .version = "2.1",
               .sample_rate = 2400000,
               .min_vfo_rate = 2000000,
               .author = "KrakenSDR",
               .options = {{"separate", "Path separation", "0", "0=Off|1=On",
                            "Every path the signal arrives on gets its own DoA lobe on the map, told apart by its delay (coherent "
                            "mode; analyses every antenna - about 3x the CPU). A path is a reflection or, in a single frequency "
                            "network, another transmitter - nothing in the signal tells which: one whose bearing points at a "
                            "broadcast mast is likely a transmitter. Delays are counted from the main (strongest) path. Off: "
                            "the echo profile of antenna 0, no bearings"},
                           {"standard", "Standard", "auto",
                            "auto=Auto|dvbt=DVB-T|dvbt2=DVB-T2|isdbt=ISDB-T|atsc=ATSC 1.0|atsc3=ATSC 3.0|dtmb=DTMB",
                            "Which signal to look for (Auto tries them all)"},
                           {"bw", "Channel width (MHz)", "auto", "auto=Auto|5=5|6=6|7=7|8=8",
                            "The TV channel's bandwidth (OFDM standards: sets the carrier spacing)"},
                           {"channel_mhz", "Channel centre (MHz)", "",
                            "", "ATSC / DTMB: the TV channel's centre frequency when the VFO doesn't see the ATSC pilot (empty = automatic)"},
                           {"min_snr", "Detection threshold (dB)", "8", "",
                            "A path's peak in the impulse response must be this far above the noise"},
                           {"max_rel_db", "Weakest path (dB below the strongest)", "30", "",
                            "Leave out peaks further below the strongest (0 = keep all): with a strong signal, local reflections "
                            "20-35 dB down pass the detection threshold too"},
                           {"max_tx", "Max paths", "8", "", "Path separation: at most this many paths (the strongest)"}},
               .manual_only = true,
               .talkers = true,
               .lock_vfo = true,
               .array = true,
               .array_burst_s = BURST_S,
               .array_period_s = PERIOD_S})
