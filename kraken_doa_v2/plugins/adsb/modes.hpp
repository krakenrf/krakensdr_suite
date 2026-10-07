#pragma once

// Mode S / ADS-B message helpers: CRC-24 parity, single-bit error repair,
// field extraction, altitude / identity / callsign codes and CPR position
// decoding. References: ICAO Annex 10 Vol IV, RTCA DO-260B; "The 1090 MHz
// Riddle" (J. Sun, TU Delft) for worked examples.

#include <cmath>
#include <cstdint>
#include <string>

namespace modes {

// --- CRC-24 (generator 0xFFF409, MSB first) -------------------------------
uint32_t crc24(const uint8_t* msg, int nbytes);
// Parity syndrome of a message: CRC of the data bits XOR the parity field
// (0 for a clean DF17/18; the ICAO address for address/parity replies)
uint32_t syndrome(const uint8_t* msg, int bits);
// Bit to flip (0-based from the first bit) that makes a 112-bit message's
// syndrome zero, -1 if no single-bit error explains it
int single_bit_error(uint32_t syn);
// Syndrome a single flipped bit b (0..111) of a 112-bit message causes (the
// CRC is linear: flipping bits XORs their syndromes into the message's)
uint32_t bit_syndrome(int b);

// Bits first..last (1-based, inclusive, as in the specs) as an integer
inline uint32_t bits(const uint8_t* m, int first, int last) {
    uint32_t v = 0;
    for (int i = first - 1; i < last; i++) v = (v << 1) | ((m[i >> 3] >> (7 - (i & 7))) & 1);
    return v;
}

// 13-bit altitude code (DF0/4/16/20 AC field); NAN = unknown / metric
float ac13_altitude_ft(uint32_t ac13);
// 12-bit ADS-B altitude (airborne position ME bits 9-20); NAN = unknown
float ac12_altitude_ft(uint32_t ac12);
// 13-bit identity (Mode A code) field -> the squawk's four octal digits as
// a decimal number (7700 for an emergency)
int squawk(uint32_t id13);
// 8-character callsign from the 48 bits of an identification message
std::string callsign(const uint8_t* me);   // me = the 7-byte ME field
// Surface movement field -> ground speed (kt), NAN = unknown
float surface_speed_kt(int mov);

// --- CPR position decoding ------------------------------------------------
int cpr_nl(double lat);
// Global decode from an even and an odd message (17-bit lat/lon values).
// odd_latest: the odd message is the newer one (its position is returned).
// Surface positions need a reference (receiver or last position) for the
// 90-degree ambiguity. false if the pair is inconsistent.
bool cpr_global(int lat_e, int lon_e, int lat_o, int lon_o, bool odd_latest, bool surface, bool have_ref,
                double ref_lat, double ref_lon, double* lat, double* lon);
// Local decode of one message relative to a reference position (within half
// a CPR zone: ~180 NM airborne, ~45 NM surface)
bool cpr_local(int cpr_lat, int cpr_lon, bool odd, bool surface, double ref_lat, double ref_lon, double* lat,
               double* lon);

// Great-circle distance (km)
double distance_km(double lat1, double lon1, double lat2, double lon2);

}  // namespace modes
