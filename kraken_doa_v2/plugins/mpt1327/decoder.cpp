// MPT1327 plugin: 48 kHz FM discriminator -> Mpt1327Receiver (mpt1327.cpp).

#include "dig_common.hpp"
#include "mpt1327.hpp"

namespace {

class Mpt1327Plugin : public kp::Decoder {
public:
    explicit Mpt1327Plugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx) {
        br_.clock(dig::FmFrontEnd::RATE);   // valid slots with their samples
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
    dig::Mpt1327Receiver rx_;
    dig::FmFrontEnd fe_;
    void facts() {  }
};

}  // namespace

KRAKEN_PLUGIN(Mpt1327Plugin, {.id = "mpt1327",
                   .name = "MPT1327",
                   .description = "MPT1327 analogue trunking (1200 bit/s FFSK signalling on NBFM). Control channel: system identity, neighbour sites, access mode, channel grants (idents as prefix-ident), acknowledgements, clear-downs. Voice is analogue FM on the granted traffic channel: listen there with NBFM. Bandwidth 12 kHz.",
                   .version = "2.0",
                   .sample_rate = 48000,
                   .min_vfo_rate = 12000,
                   .author = "KrakenSDR"})
