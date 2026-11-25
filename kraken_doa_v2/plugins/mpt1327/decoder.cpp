// MPT1327 plugin: 48 kHz FM discriminator -> Mpt1327Receiver (mpt1327.cpp).

#include <cstdlib>

#include "dig_common.hpp"
#include "mpt1327.hpp"

namespace {

class Mpt1327Plugin : public kp::Decoder {
public:
    explicit Mpt1327Plugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx, h, o_) {
        // valid slots, the sites' packets and the traffic channels' talkers with their samples
        br_.talkers(dig::FmFrontEnd::RATE);
    }
    void process(const kp::cf* x, size_t n) override {
        const auto& d = fe_.process(x, n);
        rx_.process(d.data(), d.size());
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        br_.restart_clock();
    }
    void option(const std::string& k, const std::string& v) override {
        if (k == "plan") o_.plan = v == "mpt1343" ? 1 : 0;
        else if (k == "network") o_.network = v == "mpt1343" ? 1 : 0;
        else if (k == "link") o_.uplink = v == "up";
        else if (k == "sys") o_.sys = parse(v, 16, 0x7FFF);
        else if (k == "chan") o_.chan = parse(v, 10, 1023);
        else if (k == "target") o_.target = v.substr(0, 32);
        else if (k == "source") o_.source = v.substr(0, 32);
        else if (k == "group") o_.group = v == "1" ? 1 : v == "0" ? 0 : -1;
        else if (k == "emergency") o_.emergency = v == "1";
        else if (k == "undecided") o_.undecided_group = v != "unit";
        else br_.option(k, v);
        rx_.refresh();
    }
private:
    dig::Mpt1327Options o_;
    dig::Bridge br_;
    dig::Mpt1327Receiver rx_;
    dig::FmFrontEnd fe_;
    static int parse(const std::string& v, int base, long long max) {
        if (v.empty()) return -1;
        char* end = nullptr;
        const long long x = strtoll(v.c_str(), &end, base);
        return (end && *end == '\0' && x >= 0 && x <= max) ? static_cast<int>(x) : -1;
    }
};

}  // namespace

KRAKEN_PLUGIN(Mpt1327Plugin, {.id = "mpt1327",
                   .name = "MPT1327",
                   .category = "Two-way radio",
                   .description = "MPT1327 analogue trunking (1200 bit/s FFSK signalling on NBFM). Control channel: system identity, neighbour sites, access mode, channel grants (idents as prefix-ident), acknowledgements, broadcasts - and the grants for trunk following (channel numbers to MHz with a Channel plan, else the receiver's channel table). Traffic channel: the radios' call maintenance (who talks) and the site's clear-down; the speech is analogue FM (listen with NBFM). The grant's channel / system / parties are hidden options set by trunk following. Bandwidth 12 kHz.",
                   .version = "3.2",
                   .sample_rate = 48000,
                   .min_vfo_rate = 12000,
                   .author = "KrakenSDR",
                   .options = {{"plan", "Channel plan", "none",
                                "none=None (the receiver's channel table)|mpt1343=MPT 1343 Band III (UK: 177-208 MHz)",
                                "How channel numbers become frequencies. MPT 1327 leaves the plan to the system: only "
                                "MPT 1343's Band III plan is standard; elsewhere enter the system's plan in the "
                                "receiver's channel table."},
                               {"network", "System key", "sys", "sys=The whole SYS code|mpt1343=MPT 1343 network part",
                                "What the receiver files a system's channel table under. The whole SYS code is safe "
                                "anywhere but differs from site to site; MPT 1343 networks (UK Band III) keep their "
                                "operator in part of it (OPID / NET), the same on every site."},
                               {"undecided", "Calls not known as unit calls", "group",
                                "group=Group calls|unit=Unit calls",
                                "MPT 1327 doesn't say whether a called ident is a group or a unit. A call is a unit "
                                "call when the system checked the called unit first (AHY) or that ident has called "
                                "itself; the others are reported as this. A network of unit calls (taxis, "
                                "dispatch): Unit calls."},
                               {"link", "Traffic channel side", "down",
                                "down=Downlink (the site repeats the radios)|up=Uplink (the radios themselves)",
                                "Which side of a traffic channel this VFO hears: on the downlink a talker's bearing "
                                "is the site's."},
                               {"sys", "System (SYS)", "", "", "Set by trunk following: the control channel's SYS (hex).", true},
                               {"chan", "Channel", "", "", "Set by trunk following: this traffic channel's number.", true},
                               {"target", "Called party", "", "", "Set by trunk following: the grant's called party.", true},
                               {"source", "Calling party", "", "", "Set by trunk following: the grant's calling party.", true},
                               {"group", "Group call", "", "", "Set by trunk following: 1 group, 0 unit.", true},
                               {"emergency", "Emergency call", "", "", "Set by trunk following: 1 = the grant was an emergency call's.", true}},
                   .talkers = true,
                   .trunking = true})
