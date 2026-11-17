#pragma once

// Yaesu System Fusion receiver (ysf.cpp) on a 48 kHz FM discriminator

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"
#include "dig_vocoder.hpp"

namespace dig {

// The channel coding of the standard (Yaesu "Amateur Radio Digital
// Standards" 1.02, section 4.5), on soft bits in frame order (> 0 = 0, < 0 =
// 1). Exposed for the round-trip tests.
namespace ysf {

struct Fich {   // Figure 4-9 / Table 4-1
    uint32_t raw = 0;
    int fi = 0, cs = 0, cm = 0, bn = 0, bt = 0, fn = 0, ft = 0;
    int dev = 0, mr = 0, voip = 0, dt = 0, sql = 0, sc = 0;
};
// 200 coded FICH bits -> its fields; false unless all four Golay words
// decode and the CRC-16 matches
bool decode_fich(const float* soft200, Fich& f);
// One DCH unit: 360 coded bits (160 data: header / terminator, V/D mode 1,
// Data FR, Voice FR sub header) or 200 (80 data: V/D mode 2) -> the
// de-whitened data bits; false if the CRC-16 fails
bool decode_dch(const float* soft, int ncoded, uint8_t* data);
// V/D mode 2: VCH (72) + VeCH (32) -> the 49 voice bits. Returns how many of
// the 27 triplicated bits had a dissenting copy (a bit-error estimate)
int decode_vch2(const float* soft104, uint8_t* bits49);
// The PN(9,5) whitening sequence from the start of a unit (Figure 4-28)
const uint8_t* pn9();   // 160 bits

}  // namespace ysf

// ---------------------------------------------------------------------------
// YSF / C4FM: 4800 Bd 4FSK (RRC 0.2), 100 ms frames of 960 bits: FS (40),
// FICH (200), payload (720) laid out by the FICH's frame type and data type
// ---------------------------------------------------------------------------
class YsfReceiver {
public:
    explicit YsfReceiver(RxContext& c);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
private:
    RxContext& ctx_;
    std::vector<float> taps_, hist_;
    size_t pos_ = 0;
    SampleBuf buf_;
    std::vector<float> fs_t_;               // normalized frame-sync template
    float cring_[32] = {0};                 // sync correlation of the last 32 samples (index & 31)
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int age = 0; } pk_;
    struct Job { SymSrc src; bool tracked; };
    std::deque<Job> jobs_;
    // the 100 ms frame grid (section 4.7): the next frame's expected sync
    // end, frames missed in a row, the level / centre of the last good one
    int64_t expect_ = -1;
    int misses_ = 0;
    float trk_scale_ = 0, trk_center_ = 0;
    int64_t last_sync_ = -1;                // sync end of the last valid frame
    bool dump_ = false;                     // KRAKEN_YSF_DUMP: frames / voice bits to stderr

    bool fit(int64_t idx, float* scale, float* center) const;
    void add_job(int64_t idx);
    void tracked_job();
    bool decode_frame(const SymSrc& s);
    bool dch(const float* soft, int ncoded, uint8_t* d, int slot);   // ysf::decode_dch (+ dump)

    // the transmission in progress (call), from its CSD as far as decoded
    struct Call {
        bool active = false;
        std::string dest, src, down, up, rem[4];   // trimmed display text, "" = blank / not received
        bool have_dest = false, have_src = false, have_csd2 = false;
        int dt = 0, cm = 0, mr = 0, voip = 0, sql = 1, sc = 0;
        bool announced = false;
        int frames = 0;
        bool only_hc = true;                // nothing but headers so far (N0 headers, 4.6 (1))
        bool dch_ok = false;                // a DCH unit passed its CRC
        std::string desc;                   // "SRC → DST"
    } call_;
    std::string data_[13];                  // DT1..DT13 of the current cycle (raw bytes)
    int64_t frame_a_ = 0, frame_b_ = 0;     // the frame being decoded: first / one past its last sample
    int64_t last_end_ = 0;                  // end of the last valid frame of the call
    void begin_call();
    void end_call(const char* why);
    void csd(int id, const uint8_t* bits);
    void announce(bool force);
    void apply_fich(const ysf::Fich& f);
    void data_unit(int idx, const uint8_t* bits, int nbytes);
    void flush_data(const ysf::Fich& f);
    std::string mode_name(bool brief) const;
    std::string via() const;
    std::map<std::string, std::string> facts_;
    void fact(const std::string& k, const std::string& v);

    // talker (RxContext::talker): the source callsign, from the
    // transmission's first frame to its terminator or a gap
    int64_t call_start_ = -1;               // talker span start, -1 = none
    int64_t voice_end_ = 0;                 // end of the last frame of the transmission
    int64_t lc_end_ = 0;                    // end of the last frame that named the talker
    std::string talker_id_;
    static constexpr int64_t GAP = 48000;   // frames stopped this long (1 s): over
    void talker_frame();
    void set_talker(const std::string& id);
    void end_talker(int64_t at);
    void tick(int64_t now);

    // voice
    ImbeDecoder imbe_;                      // Voice FR
    std::unique_ptr<AmbeStream> ambe_;      // V/D mode 1 / 2 (mbelib)
    bool dn_lead_ = true;                   // no DN frame but silence since the codec's reset
    void voice_vw(const uint8_t* bits, int nframes, int first);
    void voice_vd1(const uint8_t* bits);
    void voice_vd2(const float* soft);
    void voice_out(const std::vector<float>& pcm, const char* codec, int clean, int n);
};

}  // namespace dig
