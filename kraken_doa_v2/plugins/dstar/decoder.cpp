// D-STAR plugin: 48 kHz FM discriminator -> DstarReceiver (dstar.cpp).

#include "dig_common.hpp"
#include "dstar.hpp"

namespace {

class DstarPlugin : public kp::Decoder {
public:
    explicit DstarPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx) {
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
    dig::DstarReceiver rx_;
    dig::FmFrontEnd fe_;
    void facts() { host.fact("Voice codec", "AMBE: " + dig::MbeLib::instance().status()); }
};

}  // namespace

KRAKEN_PLUGIN(DstarPlugin, {.id = "dstar",
                   .name = "D-STAR",
                   .description = "D-STAR digital voice (6.25 kHz). Callsigns from the radio header (MY / UR / RPT1 / RPT2), the slow-data text message and GPS / DPRS. Voice: AMBE (mbelib, README \"Digital voice codecs\"). Bandwidth 12 kHz.",
                   .version = "2.0",
                   .sample_rate = 48000,
                   .min_vfo_rate = 12000,
                   .author = "KrakenSDR",
                   .map = true,
                   .voice = true,
                   .talkers = true})
