#pragma once

// TETRA downlink receiver (tetra.cpp) on 72 kHz RRC-filtered samples

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

// Bounds-checked reader of unpacked bits (MSB first). Reading past the end
// sets `bad` and returns 0: a truncated PDU can never yield a field.
struct TetraBits {
    const uint8_t* b = nullptr;
    int n = 0, pos = 0;
    bool bad = false;
    TetraBits(const uint8_t* bits, int len) : b(bits), n(len) {}
    uint32_t get(int k);
    bool flag() { return get(1) != 0; }
    void skip(int k);
    int left() const { return n - pos; }
};

// Channel allocation element (EN 300 392-2 21.5.2, table 21.87)
struct TetraChanAlloc {
    int type = 0;           // 0 replace, 1 additional, 2 quit and go, 3 replace + CSS
    int ts_mask = 0;        // timeslot bit map, MSB = timeslot 1; 0 = back to the common control channel
    int updown = 0;         // 0 augmented, 1 downlink, 2 uplink, 3 both
    bool clch = false, cell_change = false;
    int carrier = 0;
    bool ext = false;       // extended carrier numbering
    int band = 0, offset = 0, duplex = 0;
    bool reverse = false;
    int bw_khz = 25;        // augmented: 25 / 50 / 100 / 150
    int modulation = 0;     // augmented: 0 pi/4-DQPSK, 1 D8PSK, 2 QAM
    bool discard = false;   // the standard says to discard it (and its TM-SDU)
};

// One downlink CMCE PDU, its call control fields (EN 300 392-2 14.7.1)
struct TetraCmce {
    int type = -1;
    int cid = -1;                 // call identifier
    int bsi = -1;                 // basic service information (8 bits), -1 = not in the PDU
    int txgrant = -1;             // transmission grant (2 bits)
    int enc_ctrl = -1;            // D-TX GRANTED / INTERRUPT encryption control
    int cause = -1;               // disconnect cause
    int priority = -1;            // call priority (D-SETUP, D-CONNECT)
    int timeout = -1;             // call time-out (4 bits)
    int simplex = -1, hook = -1;
    int new_cid = -1;
    int64_t party = -1;           // calling party (D-SETUP) / transmitting party (D-TX GRANTED, INTERRUPT) SSI
    bool exact = false;           // the PDU's elements end exactly where the TM-SDU does
};

// MAC header of the PDU a TM-SDU came in (the address and the allocation)
struct TetraMacInfo {
    int addr_type = 0;
    uint32_t ssi = 0;             // the addressed SSI (event labels resolved), 0 = unknown
    int event_label = -1, usage_marker = -1;
    int enc = 0;                  // air-interface encryption mode
    bool has_alloc = false;
    TetraChanAlloc alloc;
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
    // the matched filter's delay (samples): slot k's input samples are this
    // much earlier than its filtered ones (ID-separation packets)
    int64_t input_delay = 0;
    // Trunked-system reports (kp::Host::trunk_system / grant / call ...);
    // nullptr = none
    kp::Host* trunk = nullptr;
    // --- options a trunk grant sets on the TRAFFIC instance (kept over reset()) ---
    // the assigned timeslot 1..4 (0 = none: a control / user VFO)
    void set_traffic_slot(int tn);
    // the scrambling identity, so the carrier descrambles before its first BSCH (-1 = unset)
    void set_identity(int64_t mcc, int64_t mnc, int64_t cc);
    // the call the grant was for (call identifier, -1 = unknown) and whether it is a group call
    void set_call_hint(int cid, int group);
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
    bool cell_from_bsch_ = false;
    uint32_t mcc_ = 0, mnc_ = 0, cc_ = 0;
    int tn_ = 0, fn_ = 0, mn_ = 0;
    uint32_t scramb_ = 0;
    std::string last_sysinfo_;
    // SYSINFO (the band / offset / duplex in force for channel allocations)
    bool have_sys_ = false;
    int sys_carrier_ = 0, sys_band_ = 0, sys_offset_ = 0, sys_duplex_ = 0;
    bool sys_reverse_ = false;
    // ID separation: every slot that passed its CRC under the cell's
    // scrambling code is a packet of that cell (RxContext::packet)
    uint32_t la_ = 0;
    bool have_la_ = false;
    std::string carrier_;
    int64_t packet_slot_ = -1;
    void cell_packet();
    // voice: TCH/S of one traffic timeslot -> the user-built ETSI codec
    int slot_usage_[5] = {-1, -1, -1, -1, -1};   // AACH usage marker per timeslot
    bool slot_e2e_[5] = {false, false, false, false, false};   // the call on the timeslot is end-to-end encrypted
    bool aach_ok_ = false;        // this slot's AACH passed its Reed-Muller check
    int voice_ch_sent_ = -2;      // the talker channel last given to the host's audio
    int voice_tn_ = 0;
    int64_t voice_tn_ms_ = 0;
    int64_t encrypted_ms_ = -1000000;  // last encrypted MAC-RESOURCE seen (sample-clock ms)
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
    void mac_pdus(const uint8_t* t1, int k1, const char* chan, bool second_half);
    void aach(const uint8_t* bits30);
    void sysinfo(const uint8_t* t1);
    int64_t now_ms_s() const;     // the sample clock in ms (slot start)
    double now_s() const;
    // valid frames and cell packets: a traffic instance reports only its own
    // timeslot's, and only while the AACH says it is assigned (assigned
    // control or traffic) - the filler of an unallocated slot keeps no voice VFO
    bool slot_reported() const {
        if (tslot_ == 0) return true;
        const int u = slot_usage_[tslot_];
        return tn_ == tslot_ && (u == 1 || u == 3 || u >= 4);
    }

    // --- upper MAC / LLC / MLE / CMCE ---
    bool parse_alloc(TetraBits& r, TetraChanAlloc& a);
    void tm_sdu(const uint8_t* sdu, int n, const TetraMacInfo& m, const char* chan);
    bool parse_cmce(const uint8_t* p, int n, TetraCmce& c);
    void on_cmce(const TetraCmce& c, const TetraMacInfo& m, const char* chan);
    // fragment reassembly (MAC-RESOURCE start, MAC-FRAG, MAC-END) per timeslot
    struct Frag {
        bool on = false;
        std::vector<uint8_t> sdu;
        TetraMacInfo mac;
        int idle_slots = 0;
        std::string chan;
    };
    Frag frag_[5];
    void frag_abandon(int tn);
    // event labels -> SSI (MAC-RESOURCE address types 5 / 7), bounded
    std::map<int, uint32_t> labels_;
    // statistics (facts "Channel allocations", "Not parsed")
    uint64_t n_alloc_ = 0, n_alloc_parsed_ = 0, n_alloc_call_ = 0, n_alloc_enc_ = 0, n_grants_ = 0;
    std::map<std::string, uint64_t> unparsed_;
    double stats_s_ = -1e9;
    void not_parsed(const std::string& what);
    void stats_fact(bool force);

    // --- trunking ---
    struct CallRec {
        int cid = -1;
        uint32_t target = 0;      // the called SSI (the GSSI of a group call), 0 = unknown
        int group = -1;           // -1 unknown, 0 individual, 1 group
        uint32_t source = 0;      // the calling / transmitting party, 0 = unknown
        bool enc = false, emerg = false, data = false;
        bool granted = false;
        kp::TrunkChannel ch;
        double last_s = 0;
    };
    std::vector<CallRec> calls_;  // bounded (64), the least recently heard go first
    CallRec& call_rec(int cid);
    CallRec* find_call(int cid);
    void forget_call(int cid);
    kp::TrunkChannel channel_of(const TetraChanAlloc& a, int* first_tn) const;
    void send_grant(CallRec& c, const TetraMacInfo& m, int first_tn, const kp::TrunkChannel& ch, bool data);
    void trunk_tick();
    void send_system();
    int64_t sys_sent_ = -1;       // what the last trunk_system() said (a packed key), -1 = nothing yet
    double sys_sent_s_ = -1e9;
    int64_t sys_state() const;
    bool mcch_seen_ = false;
    std::string main_sent_, control_sent_;
    bool air_enc_noted_ = false;

    // --- traffic instance (a grant's timeslot option) ---
    int tslot_ = 0;
    int64_t opt_mcc_ = -1, opt_mnc_ = -1, opt_cc_ = -1;
    int opt_cid_ = -1, opt_group_ = -1;
    struct Traffic {
        bool active = false;      // a transmission is on (call ACTIVE sent)
        bool known = false;       // the slot carries / carried a call we report
        uint32_t target = 0, source = 0;
        int cid = -1, group = -1;
        bool enc = false, emerg = false;
        int64_t start = 0, last_tch = 0;   // receiver sample indexes
        double last_report_s = -1e9;
        int unalloc = 0;          // consecutive AACH "unallocated" on the slot
        bool talker_on = false;
    } tr_;
    void traffic_call(int state);
    void traffic_start(int64_t at);
    void traffic_end(int state, int64_t at);
    void traffic_tick();
    void traffic_talker(int64_t end);
};

}  // namespace dig
