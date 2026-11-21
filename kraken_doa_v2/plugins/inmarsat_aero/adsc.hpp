#pragma once

// ADS-C (FANS-1/A, ARINC 745) reports carried in ACARS text (ARINC 622
// framing). See adsc.cpp.

#include <cmath>
#include <string>

namespace aero {

struct AdscReport {
    bool has_pos = false;
    double lat = NAN, lon = NAN, alt_ft = NAN;
    double track = NAN, gs_kt = NAN;
    std::string summary;   // one line for the event log
    std::string info;      // "\nKey: value" lines for the map popup
};

// label: the ACARS label; text: the block's text. True if it is an ADS-C
// message whose ARINC 622 CRC checks.
bool adsc_decode(const std::string& label, const std::string& text, AdscReport& r);

}  // namespace aero
