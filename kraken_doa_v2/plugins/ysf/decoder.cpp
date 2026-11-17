// Yaesu System Fusion plugin: 48 kHz FM discriminator -> YsfReceiver (ysf.cpp).

#include "dig_common.hpp"
#include "ysf.hpp"

namespace {

class YsfPlugin : public kp::Decoder {
public:
    explicit YsfPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx) {
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
    void option(const std::string& k, const std::string& v) override { br_.option(k, v); }
private:
    dig::Bridge br_;
    dig::YsfReceiver rx_;
    dig::FmFrontEnd fe_;
    void facts() {
        const auto& m = dig::MbeLib::instance();
        std::string a = "AMBE+2: " + m.status();
        if (m.available() && !m.has_ambe49()) a += ", without mbe_processAmbe2450Dataf (no DN voice)";
        host.fact("Voice codec", "VW: IMBE (built in); DN, V/D mode 1: " + a);
    }
};

}  // namespace

KRAKEN_PLUGIN(YsfPlugin, {.id = "ysf",
                  .name = "Yaesu System Fusion (C4FM)",
                  .category = "Two-way radio",
                  .description = "Yaesu System Fusion (YSF, C4FM Fusion) amateur digital voice: DN (V/D mode 2), VW "
                                 "(Voice FR), V/D mode 1 and Data FR. Source / destination callsigns, repeater "
                                 "downlink / uplink, DG-ID (squelch code), call type, wide / narrow deviation, "
                                 "via internet; call start / end. Voice: VW built in (IMBE), DN and V/D mode 1 "
                                 "AMBE+2 (mbelib). Bandwidth 12 kHz.",
                  .version = "1.0",
                  .sample_rate = 48000,
                  .min_vfo_rate = 12000,
                  .author = "KrakenSDR",
                  .voice = true,
                  .talkers = true})
