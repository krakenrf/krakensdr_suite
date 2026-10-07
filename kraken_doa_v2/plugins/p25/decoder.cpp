// P25 Phase 1 plugin: 48 kHz FM discriminator -> 4FSK sync receiver (P25
// only) -> P25 protocol decoder (p25.cpp). Voice: IMBE, built in.

#include "dig_common.hpp"
#include "dig_fsk4.hpp"
#include "p25.hpp"

namespace {

struct Sink : dig::Fsk4Sink {
    explicit Sink(dig::RxContext& c) : p25(c) {}
    bool p25_frame(const dig::SymSrc& s) override { return p25.decode(s); }
    void reset() override { p25.reset(); }
    dig::P25Proto p25;
};

class P25Plugin : public kp::Decoder {
public:
    explicit P25Plugin(kp::Host& h) : Decoder(h), br_(h), sink_(br_.ctx), rx_(sink_, true, false) { facts(); }
    void process(const kp::cf* x, size_t n) override {
        const auto& d = fe_.process(x, n);
        rx_.process(d.data(), d.size());
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        facts();
    }
    void option(const std::string& k, const std::string& v) override { br_.option(k, v); }
private:
    dig::Bridge br_;
    Sink sink_;
    dig::Fsk4Receiver rx_;
    dig::FmFrontEnd fe_;
    void facts() { host.fact("Voice codec", "IMBE (built in)"); }
};

}  // namespace

KRAKEN_PLUGIN(P25Plugin, {.id = "p25",
                          .name = "P25",
                          .description = "P25 Phase 1 (C4FM / CQPSK, 12.5 kHz). Control channels: system, site, neighbours "
                                         "and channel grants. Voice channels: talkgroup, unit ID, encryption. Voice: IMBE "
                                         "(built in). Bandwidth 12-24 kHz.",
                          .version = "2.0",
                          .sample_rate = 48000,
                          .min_vfo_rate = 12000,
                          .author = "KrakenSDR",
                          .options = {{"nac", "NAC", "", "",
                                       "Only decode frames with this Network Access Code (3 hex digits). Empty = any."}},
                          .voice = true})
