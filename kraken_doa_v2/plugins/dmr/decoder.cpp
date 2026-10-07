// DMR plugin: 48 kHz FM discriminator -> 4FSK sync receiver (DMR only) ->
// DMR protocol decoder (dmr.cpp). Voice: AMBE+2 via a user-installed mbelib.

#include "dig_common.hpp"
#include "dig_fsk4.hpp"
#include "dmr.hpp"

namespace {

struct Sink : dig::Fsk4Sink {
    explicit Sink(dig::RxContext& c) : dmr(c) {}
    bool dmr_burst(const dig::SymSrc& s, dig::DmrSync st, int* period) override {
        return dmr.decode(s, static_cast<dig::DmrProto::SyncType>(st), period);
    }
    void reset() override { dmr.reset(); }
    dig::DmrProto dmr;
};

class DmrPlugin : public kp::Decoder {
public:
    explicit DmrPlugin(kp::Host& h) : Decoder(h), br_(h), sink_(br_.ctx), rx_(sink_, false, true) {
        br_.talkers(dig::FmFrontEnd::RATE);
        facts();
    }
    void process(const kp::cf* x, size_t n) override {
        const auto& d = fe_.process(x, n);
        rx_.process(d.data(), d.size());
        pos_ += static_cast<int64_t>(d.size());
        sink_.dmr.tick(pos_);
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        br_.restart_clock();
        pos_ = 0;
        facts();
    }
    void option(const std::string& k, const std::string& v) override { br_.option(k, v); }
private:
    dig::Bridge br_;
    Sink sink_;
    dig::Fsk4Receiver rx_;
    dig::FmFrontEnd fe_;
    int64_t pos_ = 0;     // receiver samples since its reset
    void facts() { host.fact("Voice codec", "AMBE+2: " + dig::MbeLib::instance().status()); }
};

}  // namespace

KRAKEN_PLUGIN(DmrPlugin, {.id = "dmr",
                          .name = "DMR",
                          .description = "DMR Tier II / III (12.5 kHz, 2 timeslots). Colour code, talkgroups and radio IDs "
                                         "per slot, CSBK control messages, privacy flags. Voice: AMBE+2 (mbelib, README "
                                         "\"Digital voice codecs\"). Bandwidth 12-24 kHz.",
                          .version = "2.0",
                          .sample_rate = 48000,
                          .min_vfo_rate = 12000,
                          .author = "KrakenSDR",
                          .options = {{"slot", "Timeslot", "0", "0=Both|1=Slot 1|2=Slot 2",
                                       "Only show and play this timeslot's calls."}},
                          .map = true,
                          .voice = true,
                          .talkers = true})
