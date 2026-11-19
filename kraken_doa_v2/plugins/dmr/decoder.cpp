// DMR plugin: 48 kHz FM discriminator -> 4FSK sync receiver (DMR only) ->
// DMR protocol decoder (dmr.cpp). Voice: AMBE+2 via a user-installed mbelib.

#include "dig_common.hpp"
#include "dig_fsk4.hpp"
#include "dmr.hpp"

#include <cstdlib>

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
        sink_.dmr.set_host(&h);
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
    void option(const std::string& k, const std::string& v) override {
        if (k == "cc") {
            // hidden, set by trunk following from a grant: 0..15, "" = any
            char* end = nullptr;
            long c = strtol(v.c_str(), &end, 10);
            sink_.dmr.set_cc_filter((!v.empty() && end && *end == '\0' && c >= 0 && c <= 15) ? static_cast<int>(c) : -1);
            return;
        }
        if (k == "net_tag") {
            sink_.dmr.set_net_tag(v);
            return;
        }
        if (k == "traffic") {
            // hidden, set by trunk following: a payload channel's instance
            sink_.dmr.set_traffic(v == "1");
            return;
        }
        br_.option(k, v);
    }
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
                          .category = "Two-way radio",
                          .description = "DMR Tier II / III (12.5 kHz, 2 timeslots). Colour code, talkgroups and radio IDs "
                                         "per slot, CSBK control messages, privacy flags; Tier III control channels: the "
                                         "system, channel grants and announcements for trunk following. Voice: AMBE+2 "
                                         "(mbelib, README \"Digital voice codecs\"). Bandwidth 12-24 kHz. The colour code "
                                         "filter is set by trunk following.",
                          .version = "2.4",
                          .sample_rate = 48000,
                          .min_vfo_rate = 12000,
                          .author = "KrakenSDR",
                          .options = {{"slot", "Timeslot", "0", "0=Both|1=Slot 1|2=Slot 2",
                                       "Only show and play this timeslot's calls."},
                                      {"cc", "Colour code", "", "",
                                       "Only bursts of this colour code (0-15, empty = any): a trunked payload channel "
                                       "ignores a co-channel repeater. Set by trunk following.", true},
                                      {"net_tag", "Network tag", "", "",
                                       "Tells two networks apart that send the same network code (letters, digits, _ "
                                       "and -, up to 16): their channel tables and talkgroups are kept separately. "
                                       "Set it on the control channel VFOs of one of them."},
                                      {"traffic", "Traffic channel", "", "0=No|1=Yes",
                                       "This VFO follows a granted payload channel: it never reports a control "
                                       "channel. Set by trunk following.", true}},
                          .map = true,
                          .voice = true,
                          .talkers = true,
                          .trunking = true})
