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
    // Trunked systems (dmr_trunk.cpp): the host the structured reports go to
    // (kp::Host::trunk_system / grant / call ...), nullptr = none
    void set_host(kp::Host* h) { host_ = h; }
    // Hidden option "cc": only bursts of this colour code (-1 = any) - a
    // traffic channel set up by trunk following ignores a co-channel repeater
    void set_cc_filter(int cc) { cc_filter_ = cc; if (cc >= 0) cc_ = cc; }
private:
    RxContext& ctx_;
    kp::Host* host_ = nullptr;
    int cc_filter_ = -1;
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
        bool hang = false;           // a terminator ended the transmission; repeats of it follow
        // multi block control (MBC header + continuation blocks) being assembled
        uint8_t mbc[4][12] = {{0}};
        int mbc_n = 0;               // blocks held (0 = none)
        int64_t mbc_at = 0;          // burst start of the last block
        // the call this slot carries (kp::Host::call), from its own signalling
        int cstate = -1;             // -1 none reported, else kp::Call::State
        std::string ctarget, csource;
        bool cgroup = true, cenc = false, cemerg = false;
        int64_t clast = 0;           // end of its last voice burst (receiver samples)
        double csent = -1e9;         // host time of the last call() sent
        double cleared = -1e9;       // host time of the last P_CLEAR reported
    };
    Slot slots_[3];                  // [0] = unknown (MS/direct), 1, 2

    // --- trunking (dmr_trunk.cpp) ---
    // Tier III system identity code (C_SYScode) heard on this carrier, -1 none
    int syscode_ = -1;
    bool sys_control_ = false;       // heard from a TSCC (C_ALOHA / C_BCAST / C_SYS_Parms)
    double sys_sent_ = -1e9;         // host time trunk_system() was last sent
    double tscc_t_ = -1e9;           // host time of the last TSCC-only PDU
    // logical channel -> absolute downlink / uplink (Hz) as the system
    // announced them (C_BCAST Chan_Freq, grants with appended parameters)
    std::map<int, std::pair<double, double>> lcn_freq_;
    std::map<int, std::pair<double, double>> chan_sent_;   // LCN -> host time sent, downlink sent (-1 none)
    void announce_channel(int lcn);
    // CACH short LC assembly (4 CACH bursts)
    uint8_t slc_[4][17] = {{0}};
    int slc_n_ = -1;                 // fragments held, -1 = not started
    int64_t slc_at_ = 0;             // burst start of the last fragment
    // Motorola Capacity Plus rest channel (FID 0x10), -1 unknown
    int cp_rest_ = -1;
    bool cp_seen_ = false;
    int sys_key_cp_ = -1;            // the colour code of the Capacity Plus key sent
    void cach_short_lc(const uint8_t* cach24, int lcss);
    void short_lc(uint32_t slc28);
    void system_seen(int syscode, bool control, bool from_slc);
    void send_system(bool force);
    // a CSBK / MBC (all blocks, CRC-checked): returns true if it was a trunking PDU
    bool trunk_csbk(int slot, const uint8_t* c, const uint8_t* const* cont, int ncont);
    void tier3_grant(int slot, const uint8_t* c, const uint8_t* ap);
    void tier3_bcast(int slot, const uint8_t* c, const uint8_t* ap);
    void tier3_move(int slot, const uint8_t* c, const uint8_t* ap);
    void tier3_payload(int slot, const uint8_t* c);
    void capplus(int slot, const uint8_t* c);
    void mbc_block(int slot, int dt, const uint8_t* by);
    kp::TrunkChannel lcn_channel(int lcn, int ts) const;
    void call_report(int slot, int state);
    void call_lc(int slot, bool group, uint32_t dst, uint32_t src, int so);
    void call_voice(int slot);       // a confirmed voice burst on the slot
    void call_tick(int64_t now);
    int call_slot(int slot) const { return slot >= 1 ? slot - 1 : -1; }
    int last_tc_ = -1;               // slot of the last BS burst whose slot is known
    int64_t last_tc_at_ = 0;         // its burst start (receiver samples)
    int cc_ = -1;
    void full_lc(int slot, const uint8_t* lc96, const char* what);
    // a CSBK, or an MBC header with its continuation blocks (cont[0..ncont))
    void csbk(int slot, const uint8_t* c, const uint8_t* const* cont = nullptr, int ncont = 0);
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
