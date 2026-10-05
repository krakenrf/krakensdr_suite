#pragma once

// P25 Phase 1 protocol decoder (p25.cpp), fed by the 4FSK receiver

#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"
#include "dig_fsk4.hpp"
#include "dig_vocoder.hpp"

namespace dig {

// ---------------------------------------------------------------------------
// P25 Phase 1
// ---------------------------------------------------------------------------
class P25Proto {
public:
    explicit P25Proto(RxContext& c) : ctx_(c) {}
    static constexpr int FRAME_DIBITS = P25_FRAME_DIBITS;
    // Decode the frame whose sync ends at s.sync_end (all FRAME_DIBITS
    // available). Returns true if the NID was valid.
    bool decode(const SymSrc& s);
    void reset();
private:
    RxContext& ctx_;
    struct Iden { uint64_t base_hz = 0; uint32_t spacing_hz = 0; int64_t tx_offset_hz = 0; bool tdma = false; int slots = 1; };
    std::map<int, Iden> iden_;
    int last_nac_ = -1;
    std::string call_desc_;
    int64_t last_voice_ms_ = 0;
    // voice: IMBE, muted while the call is encrypted
    ImbeDecoder imbe_;
    int alg_ = 0x80;                 // current call's algorithm (0x80 = clear)
    bool lc_encrypted_ = false;      // service options "protected" bit
    void voice_frames(const std::vector<uint8_t>& sf);
    void tsbk(const uint8_t* t);
    void link_control(const uint8_t* lc, const char* where);
    std::string chan_str(uint32_t ch) const;
    void voice_activity(const std::string& desc);
};

}  // namespace dig
