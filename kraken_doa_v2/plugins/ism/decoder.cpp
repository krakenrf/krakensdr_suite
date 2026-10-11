// ISM band devices at 240 kHz (+-120 kHz around the VFO) - see
// ism_decoder.hpp. The whole 2.4 MHz at once: ism_wide (same decoders).

#include "ism_decoder.hpp"

namespace {
struct Ism240 : ism_dec::IsmDecoder {
    explicit Ism240(kp::Host& h) : IsmDecoder(h, 1) {}
};
}  // namespace

KRAKEN_PLUGIN(Ism240,
              {.id = "ism",
               .name = "ISM devices (433 MHz etc.)",
               .description = "Weather stations and thermometers, tyre pressure sensors (TPMS), key fobs and remotes, "
                              "door / motion / smoke sensors and energy meters on 315 / 345 / 433 / 868 / 915 MHz "
                              "(OOK and FSK, +-120 kHz around the VFO). Each device gets its own DoA - one bearing "
                              "per transmission - in the 🗺 Map's 📡 DF panel; signals no protocol knows show as "
                              "unknown devices when they repeat a code",
               .version = "1.0",
               .sample_rate = ism::FS,
               .min_vfo_rate = 200000,
               .author = "KrakenSDR",
               .options = ism_dec::options(),
               .map = false,
               .manual_only = true,
               .talkers = true})
