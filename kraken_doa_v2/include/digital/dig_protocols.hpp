#pragma once

// Demodulators + protocol decoders behind one digital decoder (one VFO).
//
//   48 kHz complex -> FM discriminator (Hz) -+-> Fsk4Receiver: P25 Phase 1
//                                            |   (integrate & dump filter)
//                                            |   and DMR (RRC 0.2) 4FSK at
//                                            |   4800 sym/s, 10 samples/symbol
//                                            +-> DstarReceiver: GMSK 4800 bit/s
//   72 kHz complex -> RRC 0.35 -> TetraReceiver: pi/4-DQPSK 18 ksym/s, 4 sps
//
// Every receiver is sync-driven: a frame/burst is decoded only where its
// sync pattern (or training sequence) correlates, and only what passes the
// protocol's FEC / CRC is reported - so "valid frames" are a reliable basis
// for the automatic mode detection.

#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "digital/dig_report.hpp"
#include "digital/dig_vocoder.hpp"

namespace dig {

struct RxContext {
    Report* report = nullptr;
    const Options* opts = nullptr;
    // A frame of this protocol passed its FEC/CRC checks
    std::function<void(Mode)> valid;
    // Carrier offset measured by a receiver (Hz, positive = signal above the
    // VFO centre), for the AFC and the UI
    std::function<void(Mode, float)> freq_error;
    // Decoded voice, 8 kHz mono (nominal +-1)
    std::function<void(Mode, const float*, size_t)> voice;
    // Voice status for the UI ("" = no call), e.g. "TG 3038: encrypted"
    std::function<void(const std::string&)> voice_state;
    // true while this VFO is the audio source in Digital demod mode - the
    // receivers only run their vocoders then
    std::function<bool()> voice_wanted;
};

// Sample history with absolute indexing (only the newest few hundred ms kept)
class SampleBuf {
public:
    void push(float v) { buf_.push_back(v); }
    float at(int64_t abs) const { return buf_[static_cast<size_t>(abs - base_)]; }
    int64_t begin() const { return base_; }
    int64_t end() const { return base_ + static_cast<int64_t>(buf_.size()); }
    bool has(int64_t abs) const { return abs >= base_ && abs < end(); }
    void trim_before(int64_t abs);
private:
    std::vector<float> buf_;
    int64_t base_ = 0;
};

// Soft symbols around a detected sync: symbol k (0 = last sync symbol,
// negative = before it) sampled at sync_end + k*sps, normalized so the
// nominal levels are +-1 / +-3 (or +-1 for binary).
struct SymSrc {
    const SampleBuf* buf = nullptr;
    int64_t sync_end = 0;
    int sps = 10;
    float scale = 1.0f;   // Hz per level unit
    float center = 0.0f;  // Hz
    float pol = 1.0f;
    bool has(int k) const { return buf->has(sync_end + static_cast<int64_t>(k) * sps); }
    float sym(int k) const {
        return pol * (buf->at(sync_end + static_cast<int64_t>(k) * sps) - center) / scale;
    }
    // 4FSK dibit: +3 -> 01, +1 -> 00, -1 -> 10, -3 -> 11
    static uint8_t dibit(float s) { return s >= 2.0f ? 1 : (s >= 0.0f ? 0 : (s >= -2.0f ? 2 : 3)); }
};

// ---------------------------------------------------------------------------
// P25 Phase 1
// ---------------------------------------------------------------------------
class P25Proto {
public:
    explicit P25Proto(RxContext& c) : ctx_(c) {}
    static constexpr int FRAME_DIBITS = 864;   // longest frame (LDU)
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

class Fsk4Receiver {
public:
    explicit Fsk4Receiver(RxContext& c);
    void set_enabled(bool p25, bool dmr) { en_p25_ = p25; en_dmr_ = dmr; }
    // discriminator output in Hz, 48 kHz (10 samples/symbol)
    void process(const float* d, size_t n);
    void reset();
private:
    RxContext& ctx_;
    bool en_p25_ = true, en_dmr_ = true;
    P25Proto p25_;
    DmrProto dmr_;
    SampleBuf bp_, bd_;             // P25 (boxcar) / DMR (RRC) filtered
    std::vector<float> rrc_taps_, rrc_hist_;
    size_t rrc_pos_ = 0;
    float box_hist_[10] = {0};
    float box_sum_ = 0;
    int box_pos_ = 0;
    struct Pattern { std::vector<float> t; Mode proto; int id; };
    std::vector<Pattern> pats_p25_, pats_dmr_;
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int pat = 0; int age = 0; };
    Peak pk_p25_, pk_dmr_;
    struct Job { Mode proto; SymSrc src; int need_k; DmrProto::SyncType sync; };
    std::deque<Job> jobs_;
    int64_t dmr_expect_ = -1;       // sync_end of the next expected DMR burst
    int dmr_expect_left_ = 0;
    SymSrc dmr_last_src_;
    int dmr_period_ = 144;
    void correlate(SampleBuf& b, std::vector<Pattern>& pats, Peak& pk, Mode proto);
    void on_sync(Mode proto, int pat, int64_t sync_end, float corr_sign);
    void run_jobs();
};

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
};

// ---------------------------------------------------------------------------
// MPT1327 analogue trunking signalling: 1200 bit/s FFSK (1 = 1200 Hz,
// 0 = 1800 Hz) on an NBFM carrier. Control channel slots of 128 bits: the
// control channel system codeword (CCSC, whose check bits are the SYNC
// 0xC4D7) and an address codeword. Codewords: 48 bits + BCH(63,48) + parity.
// ---------------------------------------------------------------------------
class Mpt1327Receiver {
public:
    explicit Mpt1327Receiver(RxContext& c);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
private:
    RxContext& ctx_;
    SampleBuf m_;                    // per-sample tone metric (+ = 1200 Hz)
    std::complex<float> ring12_[40], ring18_[40];   // last bit's tone products
    size_t hpos_ = 0;
    std::complex<float> c12_{0, 0}, c18_{0, 0}, r12_{1, 0}, r18_{1, 0};
    float lvl12_ = 1.0f, lvl18_ = 1.0f;   // running tone levels
    std::complex<float> w12_, w18_;
    int64_t n_ = 0;
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int age = 0; int which = 0; } pk_;
    struct Job { int64_t end; int which; };
    std::deque<Job> jobs_;
    uint32_t sys_ = 0xFFFFFFFF;
    std::string last_aloha_;
    void decode_slot(int64_t sync_end, int which);
    bool codeword(int64_t first_bit_end, uint64_t* cw);   // 64 bits -> corrected 48-bit info
    void address(uint64_t info, bool control);
};

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

// ---------------------------------------------------------------------------
// TETRA downlink (EN 300 392-2), pi/4-DQPSK 18 ksym/s
// ---------------------------------------------------------------------------
class TetraReceiver {
public:
    explicit TetraReceiver(RxContext& c);
    // matched-filtered complex samples, 72 kHz (4 samples/symbol)
    void process(const std::complex<float>* x, size_t n);
    void reset();
private:
    RxContext& ctx_;
    std::vector<std::complex<float>> hist_;   // recent samples
    int64_t hist_base_ = 0;
    // timeslot tracking
    int64_t slot_start_ = -1;     // sample index of the current slot start
    bool conj_ = false;           // spectrum inverted (I/Q swapped source)
    float dphi_ = 0.0f;           // per-symbol rotation (frequency offset)
    int locked_slots_ = 0;
    int miss_ = 0;
    // cell
    bool have_cell_ = false;
    uint32_t mcc_ = 0, mnc_ = 0, cc_ = 0;
    int tn_ = 0, fn_ = 0, mn_ = 0;
    uint32_t scramb_ = 0;
    std::string last_sysinfo_;
    // voice: TCH/S of one traffic timeslot -> the user-built ETSI codec
    int slot_usage_[5] = {-1, -1, -1, -1, -1};   // AACH usage marker per timeslot
    int voice_tn_ = 0;
    int64_t voice_tn_ms_ = 0;
    int64_t encrypted_ms_ = 0;       // last encrypted MAC-RESOURCE seen
    int64_t last_tch_ms_ = 0;
    bool codec_flushed_ = true;
    std::unique_ptr<TetraCodec> codec_;
    std::string last_call_;
    void voice_slot(const uint8_t* bits);
    void voice_idle();
    void search();
    bool demod_slot(int64_t start, int phase, float dphi, uint8_t* bits510, float* soft510);
    void decode_slot(const uint8_t* bits, const float* soft);
    bool decode_block(const float* soft, int n345, int k1, int a, uint32_t scr, std::vector<uint8_t>& type1);
    void bsch(const uint8_t* t1);
    void mac_pdus(const uint8_t* t1, int k1, const char* chan);
    void aach(const uint8_t* bits30);
    void sysinfo(const uint8_t* t1);
};

}  // namespace dig
