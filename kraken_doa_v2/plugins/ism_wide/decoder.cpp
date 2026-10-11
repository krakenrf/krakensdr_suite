// ISM band devices in the whole 2.4 MHz the tuner receives: the ism
// plugin's decoders (USES: ism - plugins/ism/ism_decoder.hpp) on a front end
// that watches the full band (64 x 10-point FFT, the same 3.75 kHz bins) and
// demodulates each burst after mixing it down and decimating it to 240 kHz.
// Up to 8 devices transmitting at once on different frequencies are kept
// apart, each with its own DoA (kraken_doa filters each one's channel out of
// the 2.4 MHz).
//
// lock_vfo: picking it puts the VFO on its tuner's centre with the 2.4 MHz
// bandwidth, drawn as a single line that can't be moved or resized - tune the
// receiver to the band (e.g. 433.92 MHz covers 432.72-435.12 MHz).

#include "ism_decoder.hpp"

namespace {
struct Ism2400 : ism_dec::IsmDecoder {
    explicit Ism2400(kp::Host& h) : IsmDecoder(h, 10) {}
};
}  // namespace

KRAKEN_PLUGIN(Ism2400,
              {.id = "ism_wide",
               .name = "ISM devices, full band (2.4 MHz)",
               .description = "The ISM devices decoder (weather stations, TPMS, key fobs and remotes, alarm sensors, "
                              "energy meters) on the whole 2.4 MHz the tuner receives: the VFO sits on the tuner's "
                              "centre with the full bandwidth - tune the receiver to the band (433.92 MHz covers "
                              "432.72-435.12 MHz). Each device gets its own DoA, its channel filtered out of the band",
               .version = "1.0",
               .sample_rate = 2400000,
               .min_vfo_rate = 2400000,
               .author = "KrakenSDR",
               .options = ism_dec::options(),
               .map = false,
               .manual_only = true,
               .talkers = true,
               .lock_vfo = true})
