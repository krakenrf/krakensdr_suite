// TETRA plugin: 72 kHz complex -> RRC 0.35 (4 samples/symbol) -> TETRA
// downlink receiver (tetra.cpp). Voice: ACELP via the user-built ETSI codec.
// Trunking: grants from the channel allocations of the control channel; an
// instance given a timeslot (a grant's options) follows that timeslot's call.

#include <cstdlib>

#include "dig_common.hpp"
#include "tetra.hpp"

namespace {

// a decimal option value, -1 = empty / not a number
long long opt_num(const std::string& v) {
    if (v.empty() || v.size() > 12) return -1;
    char* end = nullptr;
    long long x = strtoll(v.c_str(), &end, 10);
    return end && *end == '\0' && x >= 0 ? x : -1;
}

class TetraPlugin : public kp::Decoder {
public:
    explicit TetraPlugin(kp::Host& h) : Decoder(h), br_(h), rx_(br_.ctx), fe_(4, 6, 0.35f) {
        // valid slots, cell packets and the traffic timeslot's talkers with
        // their samples (the receiver runs at the input rate)
        br_.talkers(72000);
        rx_.input_delay = 4 * 6;   // the RRC front end's delay: (taps - 1) / 2 = sps * span
        rx_.trunk = &h;
        facts();
    }
    void process(const kp::cf* x, size_t n) override {
        const auto& y = fe_.process(x, n);
        rx_.process(y.data(), y.size());
    }
    void reset() override {
        fe_.reset();
        rx_.reset();
        br_.restart_clock();
        facts();
    }
    void option(const std::string& k, const std::string& v) override {
        if (k == "timeslot") {
            long long t = opt_num(v);
            tslot_ = t >= 1 && t <= 4 ? static_cast<int>(t) : 0;
            rx_.set_traffic_slot(tslot_);
            facts();
        } else if (k == "mcc" || k == "mnc" || k == "cc") {
            (k == "mcc" ? mcc_ : k == "mnc" ? mnc_ : cc_) = opt_num(v);
            rx_.set_identity(mcc_, mnc_, cc_);
        } else if (k == "call" || k == "group") {
            (k == "call" ? cid_ : group_) = opt_num(v);
            rx_.set_call_hint(static_cast<int>(cid_), static_cast<int>(group_));
        }
        br_.option(k, v);
    }
private:
    dig::Bridge br_;
    dig::TetraReceiver rx_;
    dig::RrcFrontEnd fe_;
    int tslot_ = 0;
    long long mcc_ = -1, mnc_ = -1, cc_ = -1, cid_ = -1, group_ = -1;
    void facts() {
        host.fact("Voice codec", "ACELP: " + dig::TetraCodec::status());
        host.fact("Traffic timeslot", tslot_ ? std::to_string(tslot_) + " (trunk following: only this timeslot is played and reported)" : "");
    }
};

}  // namespace

KRAKEN_PLUGIN(TetraPlugin,
              {.id = "tetra",
               .name = "TETRA",
               .category = "Two-way radio",
               .description = "TETRA downlink (base station, 25 kHz). Network (MCC / MNC), colour code, location "
                              "area, carrier, services, timeslot usage, call set-up, channel allocations and "
                              "addresses; reports grants for trunk following. Voice: ACELP (ETSI codec, README "
                              "\"Digital voice codecs\"). Bandwidth 24 kHz or more. The hidden options are set by "
                              "trunk following on the voice VFO of a call.",
               .version = "2.2",
               .sample_rate = 72000,
               .min_vfo_rate = 24000,
               .author = "KrakenSDR",
               .options = {{"timeslot", "Traffic timeslot", "", "",
                            "1-4: play and report only this timeslot's call (set by trunk following)", true},
                           {"mcc", "MCC", "", "", "the cell's MCC (scrambling; set by trunk following)", true},
                           {"mnc", "MNC", "", "", "the cell's MNC (scrambling; set by trunk following)", true},
                           {"cc", "Colour code", "", "", "the cell's colour code (scrambling; set by trunk following)", true},
                           {"call", "Call identifier", "", "", "the call the grant was for (set by trunk following)", true},
                           {"group", "Group call", "", "0=Individual|1=Group", "set by trunk following", true}},
               .voice = true,
               .talkers = true,
               .trunking = true})
