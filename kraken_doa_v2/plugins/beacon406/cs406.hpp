#pragma once

// Contents of a first-generation 406 MHz beacon message (C/S T.001 Annex A):
// the protocols, the beacon's identification, its 15-hex beacon ID, the
// encoded position. Bits are unpacked, index 0 = bit 1 of T.001.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace b406 {

struct Message {
    bool long_msg = false;          // format flag (bit 25)
    bool user = false;              // protocol flag (bit 26): user / user-location protocols
    int pdf2_errors = 0;            // bits corrected in PDF-2 (long), -1 = PDF-2 failed its BCH
    int country = 0;
    std::string country_name;
    int code = 0;                   // protocol code (3 bits user, 4 bits location)
    std::string protocol;           // "Standard location: EPIRB (MMSI)"
    std::string type;               // ELT / ELT(DT) / EPIRB / PLB / SSAS / test / orbitography / national / RLS
    std::string map_kind = "point"; // kp::MapPoint::kind
    std::string ident;              // short: "MMSI 512123456", "24-bit address C81234", "serial 1234 (TAC 123)"
    std::vector<std::pair<std::string, std::string>> fields;   // every decoded field, label / value
    std::string hex15;              // the 15-hex beacon ID (bits 26-85, position bits defaulted)
    std::string hex_msg;            // bits 25-112 / 25-144 in hex
    bool test = false;              // a test protocol (not an alert even with the normal frame sync)
    bool orbitography = false;
    bool security = false;          // ship security alert (SSAS)
    bool has_pos = false;
    double lat = NAN, lon = NAN;
    std::string pos_text;           // "coarse (+-15') + offset (4 s)", "coarse only (PDF-2 failed)"
    double pos_err_m = NAN;         // the encoding's resolution, roughly (half a step)
    int homing = -1;                // 121.5 MHz homing device: 1 / 0 / -1 = not encoded
    std::string homing_text;
    std::string pos_source;         // "internal (GNSS in the beacon)", "external navigation device"
    std::string emergency;          // short user messages: bits 107-112 (non-protected)
};

// bits: 144 (or 112) hard bits after BCH correction; pdf2_errors from the caller
Message parse(const std::vector<uint8_t>& bits, int pdf2_errors);

std::string bits_hex(const std::vector<uint8_t>& bits, size_t a, size_t n);
std::string fmt_latlon(double lat, double lon);

}  // namespace b406
