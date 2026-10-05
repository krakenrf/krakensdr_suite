#pragma once

// DMR protocol decoder (dmr.cpp), fed by the 4FSK receiver

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
// DMR (ETSI TS 102 361)
// ---------------------------------------------------------------------------
class DmrProto {
public:
    explicit DmrProto(RxContext& c) : ctx_(c) {}
    enum SyncType { NONE = 0, BS_VOICE, BS_DATA, MS_VOICE, MS_DATA, TS1_VOICE, TS1_DATA, TS2_VOICE, TS2_DATA };
    // Decode one burst. sync = NONE for an expected voice burst B..F at a
    // position extrapolated from the last sync. Returns true if the burst
    // decoded (slot type / EMB valid). *period_syms receives the burst
    // repeat period to extrapolate the next one (144 BS, 288 direct).
    bool decode(const SymSrc& s, SyncType sync, int* period_syms);
    void reset();
private:
    RxContext& ctx_;
    struct Slot {
        int voice_idx = -1;          // burst A=0 .. F=5 within a superframe
        uint8_t emb_raw[128] = {0};
        int emb_state = 0;           // 0 none, 1..3 blocks collected
        std::string call;            // current call description
        int64_t last_ms = 0;
        bool privacy = false;
        int ta_format = 0, ta_len = 0, ta_have = 0;   // talker alias assembly
        std::vector<uint8_t> ta_bits;
        std::unique_ptr<AmbeStream> ambe;            // voice codec state
        int64_t last_voice_ms = 0;
    };
    Slot slots_[3];                  // [0] = unknown (MS/direct), 1, 2
    int last_tc_ = -1;
    int64_t last_tc_ms_ = 0;
    int cc_ = -1;
    void full_lc(int slot, const uint8_t* lc96, const char* what);
    void csbk(int slot, const uint8_t* c);
    void talker_alias(int slot, int block, const uint8_t* lc);
    int voice_slot_ = -1;            // slot being played (auto mode)
    void voice_burst(int slot, const uint8_t* bits264);
    void call_update(int slot, const std::string& desc);
    std::string slot_name(int slot) const;
    bool slot_wanted(int slot) const;
};

}  // namespace dig
