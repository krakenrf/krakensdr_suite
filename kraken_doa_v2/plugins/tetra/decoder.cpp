// TETRA plugin: 72 kHz complex -> RRC 0.35 (4 samples/symbol) -> TETRA
// downlink receiver (tetra.cpp). Voice: ACELP via the user-built ETSI codec.

#include "dig_common.hpp"
#include "tetra.hpp"

namespace {

class TetraPlugin : public kp::Decoder {
public:
    explicit TetraPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx), fe_(4, 6, 0.35f) { facts(); }
    void process(const kp::cf* x, size_t n) override {
        const auto& y = fe_.process(x, n);
        rx_.process(y.data(), y.size());
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        facts();
    }
    void option(const std::string& k, const std::string& v) override { br_.option(k, v); }
private:
    dig::Bridge br_;
    dig::TetraReceiver rx_;
    dig::RrcFrontEnd fe_;
    void facts() { host.fact("Voice codec", "ACELP: " + dig::TetraCodec::status()); }
};

}  // namespace

KRAKEN_PLUGIN(TetraPlugin, {.id = "tetra",
                            .name = "TETRA",
                            .description = "TETRA downlink (base station, 25 kHz). Network (MCC / MNC), colour code, location "
                                           "area, carrier, services, timeslot usage, call set-up and addresses. Voice: ACELP "
                                           "(ETSI codec, README \"Digital voice codecs\"). Bandwidth 24 kHz or more.",
                            .version = "2.0",
                            .sample_rate = 72000,
                            .min_vfo_rate = 24000,
                            .author = "KrakenSDR",
                            .voice = true})
