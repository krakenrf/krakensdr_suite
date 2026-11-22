#pragma once

// Signal-unit decoding (ICAO Annex 10 Vol III Part I chapter 4,
// appendices 2 and 3) and the reports built from it: facts (GES,
// satellite, channel tables, counters), events (log-on / log-off,
// C channel assignments, calls, T channel reservations, user data),
// the aircraft table, and the user data's ACARS messages (ARINC 618
// blocks inside Data-2 user data: 0xFF 0xFF, SOH, mode ... suffix, BCS,
// DEL), parsed with plugins/lib/acars_msg.hpp; ADS-C reports inside them
// (adsc.hpp) go on the map.

#include "kraken_plugin.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aero {

class SuDecoder {
public:
    explicit SuDecoder(kp::Host& h);
    void reset();
    void option(const std::string& k, const std::string& v);
    // a P channel signal unit that passed its CRC; t0 / t1 = its frame
    void p_su(const uint8_t* su, double t0, double t1, int index);
    void frame_done(int sus, int sus_ok);
    // a C channel sub-band signal unit that passed its CRC
    void c_su(const uint8_t* su, double t0, double t1);
    // the AES of the C channel call (from its sub-band signalling), "" = unknown
    const std::string& call_aes() const { return call_aes_; }
    // a from-aircraft (R / T channel) signal unit or burst (Part 3)
    // returns the AES that sent it (0 = unknown)
    uint32_t r_su(const uint8_t* su19, double t0, double t1);
    void t_burst(const uint8_t* id6, const std::vector<const uint8_t*>& sus, double t0, double t1);
    // what the aircraft table knows of an AES (flight, else registration)
    std::string label_of(uint32_t aes) const;

    uint64_t su_ok() const { return su_ok_; }
    // the P channel's frequency as received (Hz, the VFO's RF + the carrier
    // offset): picks the satellite whose entry names it
    void set_channel_hz(double hz) { chan_hz_ = hz; }
    // a C channel assignment (its to-aircraft frequency, Hz) - the "all
    // channels" mode opens that channel
    std::function<void(double)> on_c_assign;
    // the P channel frequencies the signalling names (GES advice, the
    // satellite table), Hz: against them the receiver's offset is measured
    std::vector<double> advertised_hz() const {
        std::vector<double> v;
        for (const auto& [g, f] : adv_) v.push_back(f);
        for (const auto& [id, e] : sats_)
            for (double f : {e.psmc1, e.psmc2})
                if (f > 0) v.push_back(f);
        return v;
    }
    int ges() const { return ges_; }

    struct Stats {
        uint64_t acars = 0, acars_bad_bcs = 0, user_msgs = 0, adsc = 0;
    };
    const Stats& stats() const { return st_; }

private:
    kp::Host& host;
    uint64_t su_ok_ = 0, su_total_ = 0, fill_ = 0;
    std::map<int, uint64_t> type_count_;
    Stats st_;
    // GES / satellite state
    int ges_ = -1;
    std::string call_aes_;
    // the satellite identification table (type 0x0B / 0x0C, one SU per
    // satellite of the system): ID -> longitude (deg E), its P channels (Hz)
    struct SatEntry {
        double lon = 0, psmc1 = 0, psmc2 = 0;
    };
    std::map<int, SatEntry> sats_;
    double chan_hz_ = 0;
    std::map<int, std::string> psmc_;   // GES -> its P / R channel advice
    std::map<int, double> adv_;         // GES -> its P channel (Hz)
    double last_facts_ = -100;
    // user data being reassembled: key = (AES, Q, reference)
    struct Asm {
        uint32_t aes = 0;
        int ges = 0, q = 0, ref = 0, next = 0, last = 0, nss = 0;
        std::vector<uint8_t> data;
        double t0 = 0, t1 = 0;
        bool down = false;
    };
    std::map<int64_t, Asm> asm_;   // key: [T burst: 1 << 40 | AES << 8 |] Q << 4 | ref (SSUs carry only Q / ref)
    bool down_ctx_ = false;        // the SUs come from an aircraft (T burst)
    uint32_t aes_ctx_ = 0;         // ... whose burst identifier named this AES
    int64_t asm_key(int qref) const { return down_ctx_ ? (int64_t(1) << 40) | (int64_t(aes_ctx_) << 8) | qref : qref; }
    // the aircraft heard (AES ID -> row)
    struct Air {
        std::string reg, flight, label, text;
        uint64_t msgs = 0;
        double lat = NAN, lon = NAN, alt = NAN;
    };
    std::map<uint32_t, Air> air_;
    struct Multi {
        std::string text;
        int blocks = 0;
        char last_bid = 0;
        double t = 0;
    };
    std::map<std::string, Multi> multi_;   // ACARS messages of several blocks being joined

    void facts(bool force);
    void row(uint32_t aes);
    void user_data(const Asm& a);
    void acars(const Asm& a, const uint8_t* d, size_t n);
    std::string aes_str(uint32_t aes) const;
};

std::string aes_hex(uint32_t a);
std::string freq_str(int code, bool transmit);   // channel frequency field -> "1545.0250 MHz"

}  // namespace aero
