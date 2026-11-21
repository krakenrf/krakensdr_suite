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
    // a from-aircraft (R / T channel) signal unit or burst (Part 3)
    void r_su(const uint8_t* su19, double t0, double t1, float freq_hz);
    void t_burst(const uint8_t* id6, const std::vector<const uint8_t*>& sus, double t0, double t1, float freq_hz);

    uint64_t su_ok() const { return su_ok_; }

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
    std::string sat_;
    std::map<int, std::string> psmc_;   // GES -> its P / R channel advice
    double last_facts_ = -100;
    // user data being reassembled: key = (AES, Q, reference)
    struct Asm {
        uint32_t aes = 0;
        int ges = 0, q = 0, ref = 0, next = 0, last = 0, nss = 0;
        std::vector<uint8_t> data;
        double t0 = 0, t1 = 0;
        bool down = false;
    };
    std::map<int, Asm> asm_;   // P channel: key Q << 4 | ref (SSUs carry only that)
    // the aircraft heard (AES ID -> row)
    struct Air {
        std::string reg, flight, label, text;
        uint64_t msgs = 0;
        double lat = NAN, lon = NAN, alt = NAN;
    };
    std::map<uint32_t, Air> air_;

    void facts(bool force);
    void row(uint32_t aes);
    void user_data(const Asm& a);
    void acars(const Asm& a, const uint8_t* d, size_t n);
    std::string aes_str(uint32_t aes) const;
};

std::string aes_hex(uint32_t a);
std::string freq_str(int code, bool transmit);   // channel frequency field -> "1545.0250 MHz"

}  // namespace aero
