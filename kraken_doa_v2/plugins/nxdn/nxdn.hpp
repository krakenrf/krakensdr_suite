#pragma once

// NXDN receiver (nxdn.cpp) on a 48 kHz FM discriminator

#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"
#include "dig_vocoder.hpp"

namespace dig {

// ---------------------------------------------------------------------------
// NXDN (Kenwood NEXEDGE / Icom IDAS): 4FSK, NXDN96 = 4800 Bd (12.5 kHz) and
// NXDN48 = 2400 Bd (6.25 kHz), both searched at once
// ---------------------------------------------------------------------------
class NxdnReceiver {
public:
    // trunk: where the trunking reports go (kp::Host::trunk_system / grant /
    // call ...), nullptr = none
    explicit NxdnReceiver(RxContext& c, kp::Host* trunk = nullptr);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
    // Options a trunk grant passes to a traffic channel (hidden options
    // "ran" / "rate"): only frames of this RAN (-1 = any), only this bit
    // rate (0 = both, 4800 = NXDN48, 9600 = NXDN96)
    void set_ran_filter(int ran) { ran_filter_ = ran; }
    void set_rate(int bps) { rate_ = bps; }
private:
    RxContext& ctx_;
    kp::Host* host_;
    int ran_filter_ = -1, rate_ = 0;
    int64_t total_ = 0;                // input samples since the last reset (= the branches' buffer index)
    struct Branch {
        int sps;                       // 10 (NXDN96) / 20 (NXDN48)
        const char* name;
        std::vector<float> taps, hist;
        size_t pos = 0;
        SampleBuf buf;
        struct Peak { bool active = false; float best = 0; int64_t idx = 0; int age = 0; } pk;
    };
    Branch br_[2];
    std::vector<float> fsw_t_;         // normalized FSW template
    struct Job { int branch; SymSrc src; };
    int64_t last_frame_[2] = {-1, -1};      // sync_end of the previous frame with a good SACCH
    bool last_locked_[2] = {false, false};
    int64_t ran_seen_[2] = {-1, -1};        // sync_end of the last good SACCH with a RAN (traffic channel)
    bool ran_bad_[2] = {false, false};      // ... and it was not the RAN filter's
    std::deque<Job> jobs_;
    // call / channel state
    int ran_ = -1;
    uint8_t sacch_sf_[72] = {0};
    int sacch_have_ = 0;
    int64_t sacch_t_[4] = {0, 0, 0, 0};     // sync_end of each superframe fragment
    bool sacch_run_ = false;                // the 4 fragments came from 4 consecutive frames
    std::string call_;
    int cipher_ = 0;
    bool efr_ = false;                      // the call is 9600 bps / EFR (full-rate AMBE+2: not played)
    int64_t last_voice_ms_ = 0;
    std::unique_ptr<AmbeStream> ambe_;
    bool ran_rejected(int bi, int ran, int64_t sync_end, int64_t period);
    void decode_frame(const Branch& b, const SymSrc& s);
    void layer3(const uint8_t* m, int nbits, const char* via);
    void cac_message(const uint8_t* m, int nbits);
    void voice(const uint8_t* bits72x, int nframes);
    // the talker of the transmission in progress (RxContext::talker), in
    // receiver sample indexes
    int64_t frame_a_ = 0, frame_b_ = 0;   // the frame being decoded: first / one past its last sample
    bool frame_vcall_ = false, frame_rel_ = false;   // it carried a VCALL / TX_REL
    bool frame_outbound_ = false;         // sent by a repeater (LICH direction)
    int frame_rfct_ = -1;                 // its LICH RF channel type (0 RCCH, 1 RTCH, 2 RDCH, 3 RTCH_C)
    bool frame_strong_ = false;           // its message passed a strong check (CRC-12 / 15 / 16, or a
                                          // SACCH superframe of 4 consecutive frames)
    int64_t call_start_ = -1;             // start of the transmission's first frame, -1 = none
    int64_t voice_end_ = 0;               // end of its last valid voice / VCALL frame
    int64_t lc_end_ = 0;                  // end of the last frame whose VCALL named the talker
    std::string talker_id_, talker_label_;   // "" = not known (yet)
    static constexpr int64_t GAP = 24000; // frames stopped this long (0.5 s): over
    void begin_call_if_new();
    void talker_frame();                  // a valid frame of the transmission
    void set_talker(uint32_t src, const std::string& label);
    void end_talker(int64_t at);
    void tick(int64_t now);

    // --- trunking (TS 1-A 6.4 / TS 1-C) --------------------------------
    // The site heard (SITE_INFO / SRV_INFO / CCH_INFO): category (0 global,
    // 1 local, 2 regional), system code, site code; cat -1 = not known
    struct Location { int cat = -1; uint32_t sys = 0, site = 0; };
    static Location split_location(uint32_t id);
    Location site_;
    int64_t sys_sent_ = -1;               // sample index of the last trunk_system()
    bool sys_control_ = false;
    std::map<int, int64_t> chan_sent_;    // channel number -> sample index of its last trunk_channel()
    void trunk_system(const Location& loc, bool control);
    void trunk_channel(int ch);
    void grant(const uint8_t* m, int type, bool on_traffic);
    // What a traffic channel carries (kp::Host::call)
    struct CallRep {
        bool active = false;              // ACTIVE sent and not yet ENDED / RELEASED
        bool known = false;               // a call was seen on this channel (target / source below)
        std::string target, source;
        bool group = true, encrypted = false, emergency = false;
        int64_t last = 0;                 // end of its last VCALL / voice frame
        int64_t sent = -1;                // sample index of the last ACTIVE sent
    } crep_;
    static constexpr int64_t CALL_GAP = 48000;   // no VCALL / voice frame for 1 s: ENDED
    bool traffic_channel() const { return frame_rfct_ == 1 || frame_rfct_ == 3; }
    void call_report(int state);
    void call_seen(const std::string& target, bool group, const std::string& source, bool enc, bool emer);
};

}  // namespace dig
