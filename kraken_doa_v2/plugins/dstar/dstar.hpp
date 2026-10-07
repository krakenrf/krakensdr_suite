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
    void decode_header_bytes(const uint8_t* h41, bool from_slow_data);
};

}  // namespace dig
