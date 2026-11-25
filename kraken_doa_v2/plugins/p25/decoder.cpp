// P25 plugin, both phases on the same input, detected automatically (a
// trunked system's traffic channel carries Phase 1 and Phase 2 calls in
// turn):
// - Phase 1: 48 kHz FM discriminator -> 4FSK sync receiver (P25 only) ->
//   P25 protocol decoder (p25.cpp). Voice: IMBE, built in.
// - Phase 2 (two-slot TDMA): 48 kHz complex -> H-DQPSK / H-CPM receiver
//   (p25p2_phy.cpp) -> 30 ms timeslots -> MAC layer (p25p2.cpp). Voice:
//   AMBE+2 via a user-installed mbelib. It shares Phase 1's TSBK parser,
//   channel identifiers and network identity.

#include <cstdlib>

#include "dig_common.hpp"
#include "dig_fsk4.hpp"
#include "dig_vocoder.hpp"
#include "p25.hpp"
#include "p25p2.hpp"
#include "p25p2_phy.hpp"
#include "p25_trunk.hpp"

namespace {

struct Sink : dig::Fsk4Sink {
    explicit Sink(dig::RxContext& c) : p25(c) {}
    bool p25_frame(const dig::SymSrc& s) override {
        const bool ok = p25.decode(s);
        if (ok && p2) p2->phase1_frame();   // the facts both phases write
        return ok;
    }
    void reset() override { p25.reset(); }
    dig::P25Proto p25;
    dig::P25p2Proto* p2 = nullptr;
};

// "" = not given; else hex up to max (a bad value counts as not given)
int parse_hex(const std::string& v, long max) {
    if (v.empty()) return -1;
    char* end = nullptr;
    const long x = strtol(v.c_str(), &end, 16);
    return (end && *end == '\0' && x >= 0 && x <= max) ? static_cast<int>(x) : -1;
}

class P25Plugin : public kp::Decoder {
public:
    explicit P25Plugin(kp::Host& h)
        : Decoder(h), br_(h), sink_(br_.ctx), rx_(sink_, true, false), p2_(br_.ctx, sink_.p25), phy_(p2_), trunk_(h) {
        br_.talkers(dig::FmFrontEnd::RATE);
        sink_.p2 = &p2_;
        trunk_.set_phase1(&sink_.p25);
        sink_.p25.trunk = &trunk_;
        facts();
    }
    // phase_ (the hidden option a grant passes, from the channel
    // identifier's access type): 1 = only the Phase 1 receiver runs, 2 =
    // only Phase 2's; 0 = both (the phases told apart by the signal)
    void process(const kp::cf* x, size_t n) override {
        if (phase_ != 2) {
            const auto& d = fe_.process(x, n);
            rx_.process(d.data(), d.size());
        }
        if (phase_ != 1) phy_.process(x, n);
        pos_ += static_cast<int64_t>(n);
        sink_.p25.tick(pos_);
        p2_.tick(pos_);
        trunk_.tick();
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        phy_.reset();
        p2_.reset();
        trunk_.reset();
        trunk_.network_hint(wacn_, sysid_);
        br_.restart_clock();
        pos_ = 0;
        facts();
    }
    void option(const std::string& k, const std::string& v) override {
        br_.option(k, v);
        if (k == "nac") nac_ = br_.opts.p25_nac;
        else if (k == "wacn") wacn_ = parse_hex(v, 0xFFFFF);
        else if (k == "sysid") sysid_ = parse_hex(v, 0xFFF);
        else if (k == "lch") {
            const int s = atoi(v.c_str());
            p2_.set_lch(s >= 0 && s <= 2 ? s : 0);
            return;
        } else if (k == "phase") {
            const int p = v == "1" ? 1 : v == "2" ? 2 : 0;
            if (p != phase_) {
                phase_ = p;
                // a receiver that was off has not counted the input since:
                // all start again from here (both count from the same reset)
                if (pos_ > 0) reset();
            }
            return;
        } else {
            return;
        }
        p2_.set_network(wacn_, sysid_, nac_);
        trunk_.network_hint(wacn_, sysid_);
    }
private:
    dig::Bridge br_;
    Sink sink_;
    dig::Fsk4Receiver rx_;
    dig::FmFrontEnd fe_;
    dig::P25p2Proto p2_;
    dig::P25p2Phy phy_;
    dig::P25Trunk trunk_;
    int phase_ = 0;
    int64_t pos_ = 0;     // input samples since the receivers' reset (both receivers count these)
    int wacn_ = -1, sysid_ = -1, nac_ = -1;
    void facts() {
        host.fact("Voice codec", "Phase 1 IMBE (built in) · Phase 2 AMBE+2: " + dig::MbeLib::instance().status());
    }
};

}  // namespace

KRAKEN_PLUGIN(P25Plugin, {.id = "p25",
                          .name = "P25",
                          .category = "Two-way radio",
                          .description = "P25 Phase 1 (C4FM / CQPSK) and Phase 2 (two-slot TDMA: base stations' "
                                         "H-DQPSK, radios' H-CPM), 12.5 kHz, told apart automatically. Control "
                                         "channels: system, site, neighbours and channel grants. Voice channels: "
                                         "talkgroup, unit ID, encryption, per Phase 2 timeslot. Voice: IMBE (built in), "
                                         "Phase 2 AMBE+2 (mbelib, README \"Digital voice codecs\"). Bandwidth 12-24 kHz. "
                                         "Trunk following: grants and calls; the Phase option is set by a grant.",
                          .version = "3.1",
                          .sample_rate = 48000,
                          .min_vfo_rate = 12000,
                          .author = "KrakenSDR",
                          .options = {{"nac", "NAC", "", "",
                                       "Only decode frames with this Network Access Code (3 hex digits). Empty = any. "
                                       "Phase 2: the system's colour code - with WACN and System ID it gives the "
                                       "scrambling; alone it only accepts a recovered identity with this NAC."},
                                      {"wacn", "WACN", "", "",
                                       "Phase 2 only: the system's WACN (5 hex digits). Empty = found from the signal "
                                       "(a control channel's network status, or recovered from a clean traffic burst). "
                                       "With System ID and NAC it descrambles weak signals at once."},
                                      {"sysid", "System ID", "", "",
                                       "Phase 2 only: the system ID (3 hex digits), with WACN and NAC. Empty = found "
                                       "from the signal."},
                                      {"lch", "Timeslot", "0", "0=Both|1=LCH 0|2=LCH 1",
                                       "Phase 2: only show and play this logical channel's calls. Both = the first to "
                                       "talk is played until it has been quiet for a second."},
                                      {"phase", "Phase", "", "",
                                       "Which receiver runs: 1 = Phase 1 only, 2 = Phase 2 only, empty = both (told "
                                       "apart by the signal). Set by trunk following from the granted channel's "
                                       "identifier.",
                                       true}},
                          .voice = true,
                          .talkers = true,
                          .trunking = true})
