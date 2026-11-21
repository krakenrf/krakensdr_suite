#pragma once

// The Inmarsat Classic Aero C channel's voice codec (AMBE, 4.8 kbit/s,
// 96-bit frames every 20 ms), loaded at run time from the user's own
// build - never vendored, linked or shipped (THIRD_PARTY_NOTICES section
// 3, README "Digital voice codecs"). The known implementation is
// libaeroambe's mbelib (github.com/jontio/libaeroambe, directory
// mbelib-master, MIT / ISC): mbelib 1.3 plus mbe_processAmbe4800x3600Framef.
// Its CMake build makes libmbe.so.1 (a superset of mbelib 1.3, so it may
// replace the system's) - or point KRAKEN_AERO_AMBE at it.
//
// Lookup: $KRAKEN_AERO_AMBE (a path; "none" / "off" disables), then
// $KRAKEN_MBELIB, then the system's libmbe.so.1 / libmbe.so (Windows
// libmbe.dll, ...): the first that has mbe_processAmbe4800x3600Framef.
// Used by plugins/inmarsat_aero.

#include "dig_vocoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dig {

class AeroAmbeLib {
public:
    static AeroAmbeLib& instance();
    bool available() const { return ok_; }
    // "AMBE (Aero 4800): <file> (mbelib <version>)" / "not installed (...)"
    const std::string& status() const { return status_; }
    // frame: the 96 bits as received (transmission order, one bit per
    // octet); out: 160 samples at 8 kHz scaled to +-1 (not limited).
    // Returns the Golay corrections mbelib reported (errs2), -1 if no codec.
    int decode(const uint8_t* bits96, void* state, float* out160) const;
    void init_state(void* state) const;
    static constexpr size_t STATE_BYTES = 3 * 4096;

private:
    AeroAmbeLib();
    bool ok_ = false;
    std::string status_;
    void (*init_)(void*, void*, void*) = nullptr;
    void (*ambe4800_)(float*, int*, int*, char*, char[6][24], char*, void*, void*, void*, int) = nullptr;
};

// one voice stream (codec state + the output limiter)
class AeroAmbeStream {
public:
    AeroAmbeStream();
    void reset();
    int decode(const uint8_t* bits96, float* out160);

private:
    std::vector<uint8_t> state_;
    VoiceLimiter lim_;
};

}  // namespace dig
