// NXDN plugin: 48 kHz FM discriminator -> NxdnReceiver (nxdn.cpp).

#include "dig_common.hpp"
#include "nxdn.hpp"

namespace {

class NxdnPlugin : public kp::Decoder {
public:
    explicit NxdnPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx) {
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
    dig::NxdnReceiver rx_;
    dig::FmFrontEnd fe_;
    void facts() { host.fact("Voice codec", "AMBE+2: " + dig::MbeLib::instance().status()); }
};

}  // namespace

KRAKEN_PLUGIN(NxdnPlugin, {.id = "nxdn",
                   .name = "NXDN",
                   .description = "NXDN (Kenwood NEXEDGE / Icom IDAS), NXDN48 (6.25 kHz) and NXDN96 (12.5 kHz). RAN, talkgroup and unit IDs, call start / end, encryption; trunking control channels: system / site, control channel, voice and data channel grants. Voice: AMBE+2 (mbelib). Bandwidth 12 kHz.",
                   .version = "2.0",
                   .sample_rate = 48000,
                   .min_vfo_rate = 12000,
                   .author = "KrakenSDR",
                   .voice = true,
                   .talkers = true})
