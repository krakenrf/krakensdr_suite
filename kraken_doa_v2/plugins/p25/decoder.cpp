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
    explicit P25Plugin(kp::Host& h) : Decoder(h), br_(h), sink_(br_.ctx), rx_(sink_, true, false) {
        // talkers: receiver sample indexes (48 kHz since its reset) -> Host::time()
        br_.ctx.talker = [this](const std::string& id, const std::string& label, int64_t a, int64_t b) {
            host.talker({id, label, t0_ + a / dig::FmFrontEnd::RATE, t0_ + b / dig::FmFrontEnd::RATE});
        };
        br_.ctx.talker_end = [this](int64_t at) { host.talker_end(t0_ + at / dig::FmFrontEnd::RATE); };
        facts();
    }
    void process(const kp::cf* x, size_t n) override {
        const auto& d = fe_.process(x, n);
        rx_.process(d.data(), d.size());
        pos_ += static_cast<int64_t>(d.size());
        sink_.p25.tick(pos_);
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        t0_ = host.time();
        pos_ = 0;
        facts();
    }
    void option(const std::string& k, const std::string& v) override { br_.option(k, v); }
private:
    dig::Bridge br_;
    Sink sink_;
    dig::Fsk4Receiver rx_;
    dig::FmFrontEnd fe_;
    double t0_ = 0;       // Host::time() of the receiver's sample 0
    int64_t pos_ = 0;     // receiver samples since then
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
                          .voice = true,
                          .talkers = true})
