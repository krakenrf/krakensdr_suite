#pragma once

// D-STAR receiver (dstar.cpp) on a 48 kHz FM discriminator

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
// D-STAR (JARL), GMSK 4800 bit/s
// ---------------------------------------------------------------------------
class DstarReceiver {
public:
    explicit DstarReceiver(RxContext& c);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
private:
    RxContext& ctx_;
    SampleBuf b_;
    float hist_[10] = {0};
    float sum_ = 0;
    int pos_ = 0;
    std::vector<float> t_hdr_, t_data_, t_end_;
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int which = 0; int age = 0; };
    Peak pk_;
    struct Job { int which; SymSrc src; int need_k; };
    std::deque<Job> jobs_;
    // voice-frame state (21-frame superframes, sync in frame 0)
    int64_t frame_sync_end_ = -1;   // last data-sync end sample
    uint8_t slow_[60] = {0};       // 20 frames x 3 bytes
    std::string msg_[4];
    std::string header_desc_;
    std::string gps_line_;
    std::string my_;                 // MY callsign of the current / last transmission
    std::string msg_text_;           // its slow-data text message
    void gps_point(const std::string& line);
    std::unique_ptr<AmbeStream> ambe_;
    int64_t end_sync_end_ = -1;      // last end pattern (voice frames after it are noise)
    void voice_superframe(const SymSrc& s);
    uint8_t hdr_copy_[45] = {0};
    int hdr_copy_pos_ = 0;
    void correlate();
    void decode_header(const SymSrc& s);
    void decode_superframe(const SymSrc& s);
    void slow_data_block(const uint8_t* b6);
    // hdr_sync = the RF header's frame sync end, -1 = a header copy from the slow data
    void decode_header_bytes(const uint8_t* h41, bool from_slow_data, int64_t hdr_sync = -1);
    // the talker of the transmission in progress (RxContext::talker), in
    // receiver sample indexes. Voice frames have no FEC: a stretch counts as
    // confirmed once the next data sync arrives exactly in the superframe
    // cadence (or the end pattern ends it)
    int64_t call_start_ = -1;        // -1 = none
    int64_t voice_end_ = 0;          // confirmed up to here
    int64_t last_sync_ = -1;         // its last data sync (cadence reference)
    int64_t tx_hdr_sync_ = -1;       // the RF header it began with (-1 = late entry)
    int64_t hdr_cand_ = -1;          // the last header frame sync seen (CRC not checked yet)
    int64_t hdr_ok_sync_ = -1;       // the last header whose CRC passed, and its talker
    std::string hdr_id_, hdr_label_;
    std::string talker_id_, talker_label_;   // "" = not known (yet)
    void tx_sync(int64_t sync_end);
    void tx_end_pattern(int64_t sync_end);
    void tx_talker(const std::string& id, const std::string& label);
    void tx_report();
    void tx_end(int64_t at);
};

}  // namespace dig
