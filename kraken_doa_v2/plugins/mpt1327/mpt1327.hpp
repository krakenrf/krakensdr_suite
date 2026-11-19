#pragma once

// MPT1327 receiver (mpt1327.cpp) on a 48 kHz FM discriminator

#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dig_common.hpp"

namespace dig {

// Settings of the plugin (its Info::options) the receiver uses
struct Mpt1327Options {
    int plan = 0;              // channel number -> Hz: 0 none (the receiver's channel table), 1 MPT 1343 Band III
    int network = 0;           // trunk system key: 0 the whole SYS, 1 MPT 1343's network part (OPID / NET)
    bool uplink = false;       // the VFO hears the radios themselves (else the site's downlink repeat)
    // what a grant passes to the traffic channel's instance (hidden options)
    int sys = -1;              // the control channel's SYS, -1 = not known
    int chan = -1;             // this traffic channel's number, -1 = not known
    std::string target, source;   // the grant's parties ("PPP-IIII"), "" = not known
    int group = -1;            // the grant's group flag, -1 = not known
    bool emergency = false;    // the grant's emergency flag
    bool undecided_group = true;   // a called party nothing shows to be a unit: a group (else a unit)
};

// ---------------------------------------------------------------------------
// MPT1327 analogue trunking signalling: 1200 bit/s FFSK (1 = 1200 Hz,
// 0 = 1800 Hz) on an NBFM carrier. Control channel slots of 128 bits: the
// control channel system codeword (CCSC, whose check bits are the SYNC
// 0xC4D7) and an address codeword. Traffic channels: messages after the SYNT
// 0x3B28 (the radios' call maintenance, the site's clear-down). Codewords: 48
// bits + BCH(63,48) + parity.
// ---------------------------------------------------------------------------
class Mpt1327Receiver {
public:
    Mpt1327Receiver(RxContext& c, kp::Host& h, const Mpt1327Options& o);
    void process(const float* d, size_t n);   // discriminator Hz @48 kHz
    void reset();
    // the system / call reports again (after an option changed)
    void refresh();
private:
    RxContext& ctx_;
    kp::Host& host_;
    const Mpt1327Options& o_;
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
    // carrier (noise squelch on the discriminator): 10 ms blocks of the
    // squared first difference - FM noise without a carrier is wideband
    float dc_ = 0;                   // the discriminator's DC = the carrier offset (Hz)
    int64_t fe_sent_ = -1;           // sample of the last freq_error report
    float prev_d_ = 0;
    double hf_acc_ = 0;
    int hf_n_ = 0;
    bool carrier_ = false;
    int64_t carrier_lost_ = -1;      // sample where the carrier went, -1 = present / never seen
    // control channel state
    int64_t cc_seen_ = -1;           // sample of the last good CCSC
    int64_t sys_sent_ = -1;          // sample of the last trunk_system()
    struct Recent { int prefix, ident; int64_t at; bool emergency; };
    std::vector<Recent> units_;      // called units checked with AHY (POINT 0): individual calls
    std::vector<Recent> emerg_;      // parties of an emergency call (ACKE, AHY with E)
    struct LastGtc { uint64_t key; int64_t at; };
    std::vector<LastGtc> gtcs_;      // recent GTCs: a repeat is a grant update
    // traffic channel state (call() / talker)
    enum CallState { NONE, ACTIVE, ENDED };
    CallState call_ = NONE;
    std::string call_target_, call_source_;
    bool call_group_ = true;
    std::string talker_;             // the ident of the open talker, "" none
    int64_t talker_start_ = 0, talker_end_ = 0;
    int64_t clear_at_ = -1;          // sample of the last CLEAR acted on
    int64_t last_tc_ = -1;           // sample of the last traffic channel message
    int tc_chan_ = -1;               // the channel number the traffic messages carry
    bool released_ = false;          // a clear-down since the reset: the granted call is over
    int cc_chan4_ = -1;              // CHAN4 of the control channel slots heard (Aloha, MARK), -1 = none yet
    int64_t carrier_since_ = -1;     // sample where the carrier came, -1 = none
    int64_t ended_at_ = -1;          // sample of the last call(ENDED)
    uint64_t tc_gtc_ = 0;            // the last GTC after a SYNT (counts only when repeated)
    int64_t tc_gtc_at_ = -1;
    static constexpr size_t MAX_UNITS = 4096;
    void learn_unit(int prefix, int id);
    // a decoded slot / message: sync_end, SYNC (0) or SYNT (1), the CCSC's
    // SYS, the address codeword and its corrected bits (-1 = bad)
    struct Slot { int64_t sync_end = -1; int which = 0; uint32_t sys = 0; uint64_t addr = 0; int err = -1; };
    struct CcLast { uint32_t sys; int64_t end; };
    std::array<CcLast, 4> cc_last_{};  // the last CCSC of the sites heard (slot grid check)
    Slot pend_;                      // a control channel's first slot, waiting for the second
    void decode_slot(int64_t sync_end, int which);
    void slot(const Slot& s);
    int codeword(int64_t first_bit_end, uint64_t* cw);   // 64 bits -> 48-bit info; bits corrected, -1 = bad
    void address(uint64_t info, bool control, bool strict, int64_t start, int64_t end);
    void traffic(uint64_t info, int64_t start, int64_t end);
    void grant(uint64_t info, bool on_traffic);
    void send_system();
    bool is_control() const;
    kp::TrunkChannel channel(int c) const;
    std::string network_key(uint32_t sys) const;
    void set_call(CallState s, const std::string& target, const std::string& source, bool group);
    void close_talker();
    void released(const char* why);
    void carrier_block(float hf, int64_t block_end);
    bool recent(const std::vector<Recent>& v, int prefix, int ident, double s) const;
};

}  // namespace dig
