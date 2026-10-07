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
    // now = the receiver's current sample index: ends transmissions whose
    // bursts stopped without a terminator
    void tick(int64_t now);
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
        uint32_t src = 0;            // radio ID of the current / last call (GPS reports are its)
        std::string alias;           // its talker alias
        std::vector<uint8_t> ta_bits;
        std::unique_ptr<AmbeStream> ambe;            // voice codec state
        int64_t last_voice_ms = 0;
        // the talker of the slot's transmission (RxContext::talker, channel =
        // slot), in receiver sample indexes
        int64_t call_start = -1;     // its first confirmed burst, -1 = none
        int64_t voice_end = 0;       // end of its last confirmed burst
        int64_t lc_end = 0;          // end of the last burst whose LC named the talker
        std::string tid, tcall;      // radio ID ("" = not known yet), "TG 9" / "unit call to 123"
        bool tbs = false;            // sent by a repeater (BS sync)
        bool treported = false;      // the host knows of the transmission (by its ID, or as "?")
    };
    Slot slots_[3];                  // [0] = unknown (MS/direct), 1, 2
    int last_tc_ = -1;
    int64_t last_tc_ms_ = 0;
    int cc_ = -1;
    void full_lc(int slot, const uint8_t* lc96, const char* what);
    void csbk(int slot, const uint8_t* c);
    void talker_alias(int slot, int block, const uint8_t* lc);
    void gps_point(int slot, double lat, double lon, const char* text);
    // the burst being decoded: first / one past its last sample, sent by a repeater
    int64_t burst_a_ = 0, burst_b_ = 0;
    bool burst_bs_ = false;
    int sps_ = 10;
    // the last burst with a sync: bursts extrapolated from it (sync NONE) are
    // of the same kind - MS-sourced / direct ones have no CACH to read the slot from
    bool ext_bs_ = true;
    int ext_slot_ = 0;
    int64_t gap() const { return static_cast<int64_t>(0.5 * 4800) * sps_; }   // bursts stopped this long: over
    bool talker_slot(int slot) const { return slot != 0 || !burst_bs_; }        // slot known (a repeater's always is)
    void talker_begin(int slot);
    void talker_burst(int slot);     // a confirmed burst of the slot's transmission
    void talker_set(int slot, uint32_t src, const std::string& call);
    void talker_end(int slot, int64_t at);
    int voice_slot_ = -1;            // slot being played (auto mode)
    void voice_burst(int slot, const uint8_t* bits264);
    void call_update(int slot, const std::string& desc);
    std::string slot_name(int slot) const;
    bool slot_wanted(int slot) const;
};

}  // namespace dig
