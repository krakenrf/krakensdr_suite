// NXDN plugin: 48 kHz FM discriminator -> NxdnReceiver (nxdn.cpp).

#include "dig_common.hpp"
#include "nxdn.hpp"

#include <cstdlib>

namespace {

class NxdnPlugin : public kp::Decoder {
public:
    explicit NxdnPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx, &h) {
        br_.talkers(dig::FmFrontEnd::RATE);
        facts();
    }
    void process(const kp::cf* x, size_t n) override {
        const auto& d = fe_.process(x, n);
        rx_.process(d.data(), d.size());
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        br_.restart_clock();
        facts();
    }
    void option(const std::string& k, const std::string& v) override {
        // the hidden options a trunk grant passes to a traffic channel's
        // instance (they take effect at once; "" = back to any)
        if (k == "ran") {
            char* end = nullptr;
            long r = std::strtol(v.c_str(), &end, 10);
            rx_.set_ran_filter(!v.empty() && end && *end == '\0' && r >= 0 && r <= 63 ? static_cast<int>(r) : -1);
        } else if (k == "rate") {
            rx_.set_rate(v == "4800" ? 4800 : v == "9600" ? 9600 : 0);
        } else {
            br_.option(k, v);
        }
    }
private:
    dig::Bridge br_;
    dig::NxdnReceiver rx_;
    dig::FmFrontEnd fe_;
    void facts() { host.fact("Voice codec", "AMBE+2: " + dig::MbeLib::instance().status()); }
};

}  // namespace

KRAKEN_PLUGIN(NxdnPlugin, {.id = "nxdn",
                   .name = "NXDN",
                   .category = "Two-way radio",
                   .description = "NXDN (Kenwood NEXEDGE / Icom IDAS), NXDN48 (6.25 kHz) and NXDN96 (12.5 kHz). RAN, talkgroup and unit IDs, call start / end, encryption; trunking (Type-C) control channels: system / site, control channels, adjacent sites, voice and data channel grants - trunk following puts a voice VFO on each granted channel (channel numbers: their frequencies come from the trunk channel table). Voice: AMBE+2 (mbelib). Bandwidth 12 kHz. RAN and Rate are set by a trunk grant.",
                   .version = "2.1",
                   .sample_rate = 48000,
                   .min_vfo_rate = 12000,
                   .author = "KrakenSDR",
                   .options = {{"ran", "RAN", "", "",
                                "Only frames of this Radio Access Number (0-63; empty = any) - a traffic channel's instance gets the control channel's RAN with the grant, so a co-channel site is ignored",
                                true},
                               {"rate", "Rate", "", "4800=NXDN48 (4800 bps)|9600=NXDN96 (9600 bps)",
                                "Only this bit rate (empty = both) - set from a grant's transmission mode", true}},
                   .voice = true,
                   .talkers = true,
                   .trunking = true})
