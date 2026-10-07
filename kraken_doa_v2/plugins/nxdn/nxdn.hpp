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
    explicit NxdnReceiver(RxContext& c);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
private:
    RxContext& ctx_;
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
    std::deque<Job> jobs_;
    // call / channel state
    int ran_ = -1;
    uint8_t sacch_sf_[72] = {0};
    int sacch_have_ = 0;
    std::string call_;
    int cipher_ = 0;
    int64_t last_voice_ms_ = 0;
    std::unique_ptr<AmbeStream> ambe_;
    void decode_frame(const Branch& b, const SymSrc& s);
    void layer3(const uint8_t* m, int nbits, const char* via);
    void cac_message(const uint8_t* m);
    void voice(const uint8_t* bits72x, int nframes);
    // the talker of the transmission in progress (RxContext::talker), in
    // receiver sample indexes
    int64_t frame_a_ = 0, frame_b_ = 0;   // the frame being decoded: first / one past its last sample
    bool frame_vcall_ = false, frame_rel_ = false;   // it carried a VCALL / TX_REL
    bool frame_outbound_ = false;         // sent by a repeater (LICH direction)
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
};

}  // namespace dig
