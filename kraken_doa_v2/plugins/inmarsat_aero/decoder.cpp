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

#include "aero_all.hpp"
#include "aero_channel.hpp"
#include "kraken_plugin.hpp"

#include <liquid/liquid.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// The plugin takes the VFO's own rate (sample_rate 0). The single-channel
// modes resample it to 24 kHz exactly as kraken_doa did for them before
// (liquid msresamp, 60 dB; nothing at 24 kHz): the same samples, the same
// counts. "All channels in the VFO" channelizes the whole input (aero_all).
class AeroPlugin : public kp::Decoder {
public:
    explicit AeroPlugin(kp::Host& h) : Decoder(h) {
        rate_ = host.sample_rate();
        if (!std::isfinite(rate_) || rate_ <= 0) rate_ = aero::chan::FS;
        single();
    }
    ~AeroPlugin() override { drop_rs(); }

    void process(const aero::cf* x, size_t n) override {
        if (all_) {
            all_->process(x, n);
            return;
        }
        if (!rs_) {
            chan_->process(x, n);
            return;
        }
        out_.resize(static_cast<size_t>(std::ceil(n * aero::chan::FS / rate_)) + 64);
        unsigned int ny = 0;
        msresamp_crcf_execute(rs_, const_cast<aero::cf*>(x), static_cast<unsigned int>(n), out_.data(), &ny);
        if (ny) chan_->process(out_.data(), ny);
    }

    void reset() override {
        if (all_) all_->reset();
        if (chan_) chan_->reset();
        if (rs_) msresamp_crcf_reset(rs_);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "mode") {
            const bool want_all = v == "all";
            if (want_all && !all_ && rate_ >= MIN_ALL_RATE) {
                // the single-channel decoder's facts go; the channel list replaces them
                chan_.reset();
                for (const char* f : {"Channel", "Carrier offset", "Signal units", "SU types", "User data", "GES", "GES time", "Satellite",
                                      "C channel frames", "Voice codec frames", "Last ACARS"})
                    host.fact(f, "");
                all_ = std::make_unique<aero::AllChannels>(host, rate_);
                for (const auto& [ok, ov] : opts_) all_->option(ok, ov);
                return;
            }
            if (!want_all && all_) {
                all_.reset();
                host.voice_state("");
                single();
            }
            if (want_all && rate_ < MIN_ALL_RATE) {
                // a narrow VFO holds one channel: Auto on it
                host.fact("Channels", "the VFO is too narrow for more than one channel (Auto)");
                chan_->option(k, "auto");
                return;
            }
            if (chan_) chan_->option(k, v);
            return;
        }
        opts_[k] = v;
        if (all_) all_->option(k, v);
        else if (chan_ && k != "listen") chan_->option(k, v);
    }

private:
    // below this, a VFO holds one channel and its neighbours' edges
    static constexpr double MIN_ALL_RATE = 96000;
    double rate_ = aero::chan::FS;
    std::unique_ptr<aero::AeroChannel> chan_;
    std::unique_ptr<aero::AllChannels> all_;
    msresamp_crcf rs_ = nullptr;
    std::vector<aero::cf> out_;
    std::map<std::string, std::string> opts_;

    void single() {
        chan_ = std::make_unique<aero::AeroChannel>(host);
        for (const auto& [ok, ov] : opts_)
            if (ok != "listen") chan_->option(ok, ov);
        drop_rs();
        if (std::fabs(rate_ - aero::chan::FS) > 0.5) rs_ = msresamp_crcf_create(static_cast<float>(aero::chan::FS / rate_), 60.0f);
    }
    void drop_rs() {
        if (rs_) msresamp_crcf_destroy(rs_);
        rs_ = nullptr;
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
                           .sample_rate = 0,
                           .min_vfo_rate = 15000,
                           .author = "KrakenRF",
                           .options = {{"mode", "Channel", "auto",
                                        "auto=Auto (P / C channel)|p600=P 600 bit/s|p1200=P 1200 bit/s|p2400=P 2400 bit/s|"
                                        "p4800=P 4800 bit/s|p10500=P 10500 bit/s|c8400=C channel 8400 bit/s (voice)|"
                                        "burst=R / T bursts from aircraft (1.6 GHz)|all=All channels in the VFO",
                                        "Auto finds a continuous channel's rate and carrier; the P / C choices search only that "
                                        "rate; R / T bursts listens for aircraft transmissions at every burst rate; All channels finds, decodes and "
                                        "listens to every P and C channel in the VFO at once (a wide VFO, up to 2.4 MHz)"},
                                       {"listen", "Calls (all channels)", "mix",
                                        "mix=Every call, mixed|newest=One call: the newest",
                                        "With All channels: several calls at once are mixed (through a limiter), or only "
                                        "the call that began last is heard until it ends"}},
                           .map = true,
                           .voice = true,
                           .messages = false,
                           .talkers = true})
