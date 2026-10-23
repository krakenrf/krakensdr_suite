// Pagers: POCSAG and FLEX pages - the example plugin (see plugins/SDK.md).
//
// One FM front end (8 kHz low-pass, discriminator in Hz at 38.4 kHz) feeds
// every receiver at once: POCSAG at 512 / 1200 / 2400 bit/s (pocsag.hpp, a
// chain per rate) and FLEX at 1600 / 3200 / 6400 bit/s (flex.hpp: sync 1 at
// 1600 bit/s names each frame's speed). Neither needs a setting.
//
// Neither protocol names its transmitter: the address is the pager called
// (the recipient), and paging networks simulcast - no talkers. The VFO's DoA
// with the Digital squelch (Host::valid per POCSAG batch / FLEX block) is the
// paging site's.

#include "flex.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "pages.hpp"
#include "pocsag.hpp"

#include <vector>

namespace {

constexpr double FS = 38400;   // POCSAG 75 / 32 / 16 samples a bit, FLEX 24 / 12 a symbol

class Pagers : public kp::Decoder {
public:
    explicit Pagers(kp::Host& h) : Decoder(h), pages_(h), flex_(h, pages_, FS), lp_(kp::fir_lowpass(63, 8000, FS)), fm_(FS) {
        for (int b : {512, 1200, 2400}) pocsag_.emplace_back(b, FS, host, pages_);
    }
    void process(const kp::cf* x, size_t n) override {
        const double t0 = host.time();
        for (size_t i = 0; i < n; i++) {
            float f = fm_.push(lp_.push(x[i]));
            const double t = t0 + static_cast<double>(i) / FS;
            for (auto& c : pocsag_) c.push(f, t);
            flex_.push(f, t);
        }
    }
    void reset() override {
        lp_.reset();
        fm_.reset();
        for (auto& c : pocsag_) c.reset();
        flex_.reset();
    }

private:
    pagers::Pages pages_;
    std::vector<pagers::PocsagChain> pocsag_;
    pagers::FlexRx flex_;
    kp::Fir<kp::cf> lp_;
    kp::FmDemod fm_;
};

}  // namespace

KRAKEN_PLUGIN(Pagers, {.id = "pagers",
                       .name = "Pagers (POCSAG + FLEX)",
                       .description = "Pager messages: POCSAG 512/1200/2400 bit/s and FLEX 1600/3200/6400 bit/s "
                                      "(numeric, alphanumeric, tone only, binary)",
                       .version = "2.0",
                       .sample_rate = FS,
                       .min_vfo_rate = 12500,
                       .author = "KrakenSDR example",
                       .messages = true})
