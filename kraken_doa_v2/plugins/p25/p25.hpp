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
    // now = the receiver's current sample index: ends a transmission whose
    // voice frames stopped without a terminator
    void tick(int64_t now);

    // --- shared with the Phase 2 MAC layer (p25p2.cpp) ---
    // A TSBK body: t[0] low 6 bits = opcode, t[1] = MFID, t[2..9] = the 8
    // argument octets (Phase 2 partition-01 messages are these octets).
    // tdma = from a Phase 2 channel: channel numbers also name their
    // timeslot (chan_str); the Phase 1 decoder itself passes false.
    void tsbk(const uint8_t* t, bool tdma = false);
    // channel (identifier 4 + number 12) -> "8-602 (854.76250 MHz)", with
    // tdma ", LCH n" for a TDMA identifier with several slots
    std::string chan_str(uint32_t ch, bool tdma = false) const;
    struct Iden { uint64_t base_hz = 0; uint32_t spacing_hz = 0; int64_t tx_offset_hz = 0; bool tdma = false; int slots = 1; };
    const Iden* iden(int id) const {
        auto it = iden_.find(id);
        return it == iden_.end() ? nullptr : &it->second;
    }
    // network identity as learned so far (-1 = not yet): the NAC of the
    // last valid frame, WACN / system ID from NET_STS_BCST
    int nac() const { return last_nac_; }
    int wacn() const { return wacn_; }
    int sysid() const { return sysid_; }
    static std::string algorithm_name(int alg);   // "clear", "AES-256", "alg 0x.."
private:
    RxContext& ctx_;
    std::map<int, Iden> iden_;
    int last_nac_ = -1;
    int wacn_ = -1, sysid_ = -1;
    std::string call_desc_;
    int64_t last_voice_ms_ = 0;
    // voice: IMBE, muted while the call is encrypted
    ImbeDecoder imbe_;
    int alg_ = 0x80;                 // current call's algorithm (0x80 = clear)
    bool lc_encrypted_ = false;      // service options "protected" bit
    void voice_frames(const std::vector<uint8_t>& sf);
    void link_control(const uint8_t* lc, const char* where);
    void voice_activity(const std::string& desc);
    // the talker of the voice transmission in progress (RxContext::talker),
    // in receiver sample indexes
    int64_t frame_a_ = 0, frame_b_ = 0;   // the frame being decoded: first / one past its last sample
    int sps_ = 10;
    int64_t call_start_ = -1;             // start of the transmission's first frame, -1 = none
    int64_t voice_end_ = 0;               // end of its last valid voice frame
    int64_t lc_end_ = 0;                  // end of the last frame whose link control named the talker
    std::string talker_id_, talker_label_;   // "" = not known (yet)
    int64_t gap() const { return static_cast<int64_t>(0.75 * 4800) * sps_; }   // frames stopped this long: over
    void begin_call_if_new();
    void voice_frame();                   // a valid HDU / LDU1 / LDU2
    void set_talker(uint32_t src, const std::string& label);
    void end_talker(int64_t at);
};

}  // namespace dig
