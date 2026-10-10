#pragma once

// Aeronautical radio navigation aids, shared by the dme and ils plugins:
//  - the ICAO frequency plan (Annex 10 Vol I, Table A) that ties every DME /
//    TACAN channel (1X..126Y) to its interrogation and reply frequencies and
//    to the VOR or ILS localizer it is paired with, and every localizer to
//    its glide slope
//  - a Morse decoder for station idents (DME: the key-down periods of its
//    1350 pps pulse pairs; VOR / ILS localizer: the keyed 1020 Hz tone)

#include <cmath>
#include <string>
#include <vector>

namespace nav {

struct DmeChannel {
    int number = 0;                 // 1..126, 0 = none
    char mode = 0;                  // 'X' or 'Y'
    int interrogation_mhz = 0;      // aircraft -> ground
    int reply_mhz = 0;              // ground -> aircraft
    double vhf_mhz = NAN;           // the paired VOR / ILS localizer, NAN = unpaired (TACAN-only channel)
    double gs_mhz = NAN;            // the glide slope paired with that localizer, NAN = none
    bool valid() const { return number > 0; }
    bool ils() const { return std::isfinite(gs_mhz); }
    std::string name() const;       // "40X"
};

DmeChannel dme_channel(int number, char mode);
// The channel whose ground station replies on this frequency (every MHz of
// 962..1213 belongs to exactly one): 962-1024 X 1-63, 1025-1087 Y 64-126,
// 1088-1150 Y 1-63, 1151-1213 X 64-126
DmeChannel dme_by_reply(int mhz);
// The channel aircraft interrogate on this frequency (1025..1150 MHz) in a mode
DmeChannel dme_by_interrogation(int mhz, char mode);
// The channel paired with a VOR / localizer frequency (108.00..117.95 MHz, 50 kHz steps)
DmeChannel dme_by_vhf(double mhz);
bool is_localizer(double mhz);      // 108.10..111.95 MHz with an odd tenth
bool is_vor(double mhz);            // 108.00..117.95 MHz, not a localizer
bool is_glideslope(double mhz);     // 329.15..335.00 MHz
double glideslope_for(double loc_mhz);   // NAN = not a localizer
double localizer_for(double gs_mhz);     // NAN = not a glide slope channel
// "ILS localizer 110.30 MHz, glide slope 335.00 MHz" / "VOR 110.20 MHz" /
// "no VOR / ILS (TACAN / DME only)"
std::string pairing_text(const DmeChannel& c);
std::string mhz_text(double mhz, int decimals = 2);

// --- Morse -----------------------------------------------------------------
const char* morse_char(const std::string& code);   // ".-" -> "A", nullptr = not a character

// Turns a key (on / off), sampled at a steady tick, into text. A
// transmission is decoded once the key has stayed up for end_gap_s. Dot and
// dash are told apart by the marks' own lengths (whatever the speed); a
// transmission of only one length is read with the expected dot length
// (unit_min_s .. unit_max_s).
class MorseDecoder {
public:
    MorseDecoder(double tick_s, double unit_min_s, double unit_max_s, double end_gap_s);
    // one tick; true = a transmission just ended (text() etc. hold it)
    bool push(bool key);
    void reset();
    const std::string& text() const { return text_; }    // "IAA", '?' = unknown character, ' ' between words
    const std::string& code() const { return code_; }    // ".. .- .-"
    double unit_s() const { return unit_s_; }           // the dot length found
    double length_s() const { return len_s_; }          // first mark .. end of the last mark
    double ended_ago_s() const { return gap_s_; }       // how long ago the last mark ended
    // 0..1: how well the marks / gaps fit 1 and 3 dot lengths
    double quality() const { return quality_; }
    bool keying() const { return !runs_.empty(); }      // a transmission is in progress

private:
    double tick_, umin_, umax_, endgap_;
    std::vector<std::pair<bool, int>> runs_;   // (key, ticks), first one a mark
    bool cur_ = false;
    int curlen_ = 0;
    std::string text_, code_;
    double unit_s_ = 0, len_s_ = 0, gap_s_ = 0, quality_ = 0;
    bool decode();
};

}  // namespace nav
