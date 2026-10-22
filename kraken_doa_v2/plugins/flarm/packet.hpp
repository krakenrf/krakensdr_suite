#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 KrakenRF Inc. - part of the KrakenSDR `flarm` decoder
// plugin, licensed under the GNU General Public License version 3 (LICENSE in
// this folder; README.md "Licence").

// FLARM / OGN packet layer: what 26 demodulated bytes (+ their soft bits)
// are, and whether they passed their checks.
//
// FLARM legacy (an nRF905 payload): bytes 0-2 = the 24-bit radio ID, little
// endian; byte 3 bits 4-5 = the address type; bytes 4-23 are encrypted with
// keys FLARM has not published - NOT decoded or analysed in any way; bytes
// 24-25 = CRC-16 (poly 0x1021, init 0xFFFF, MSB first - CRC-16/IBM-3740)
// over the nRF905 address 31 FA B6 + bytes 0-23. Also accepted with every
// byte complemented (a radio whose data is inverted against its sync).
//
// OGN tracker ("OGN1"): 20 bytes = five little-endian 32-bit words (W0 the
// header, never whitened; W1..W4 the data, TEA-whitened unless the owner
// encrypted them) + 6 bytes of LDPC(208,160) parity (ogn_ldpc_matrix.hpp).
// Checked by the parity checks on the hard bits, else by normalised min-sum
// soft decoding (<= 16 iterations, <= 16 bits changed), then by the
// header's own parity bit.

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pkt {

constexpr int BYTES = 26;
constexpr int BITS = 208;

enum class Proto { FLARM, OGN };

// The aircraft type names (index 0..15) - shown everywhere
const char* type_name(int t);

// A decoded OGN data word set (W1..W4 de-whitened). Absent values are NAN.
struct OgnContent {
    int report = -1;            // non-position reports: 0 status, 1 info, ... (-1 = a position report)
    // position report
    bool fix = false;           // fix quality > 0 and the position in range: lat / lon usable
    double lat = NAN, lon = NAN;
    int second = 63, fix_quality = 0;
    double dop = NAN;
    bool fix3d = false;
    double alt_m = NAN;         // GNSS altitude
    bool baro = false;          // pressure altitude present
    double palt_m = NAN;
    double speed_ms = NAN, track_deg = NAN, climb_ms = NAN, turn_dps = NAN;
    bool stealth = false;
    int acft_type = 0;
    // status report
    std::string status;
    // info report: shown fields (personal ones never kept), in packet order
    std::vector<std::pair<std::string, std::string>> info;
    bool info_ok = false;       // its XOR checksum matched
};

// One packet that passed its checks
struct Frame {
    Proto proto = Proto::FLARM;
    uint8_t b[BYTES] = {};      // the packet as accepted: complemented back / LDPC-corrected
    uint32_t addr = 0;
    int addr_type = 0;          // 0 random, 1 ICAO, 2 FLARM, 3 OGN
    // FLARM
    bool inverted = false;
    // OGN
    int corrected = 0;          // bits the soft decoding changed
    bool non_position = false, encrypted = false, emergency = false;
    int relay = 0;              // 0 = sent by the aircraft itself
    OgnContent ogn;             // not filled when encrypted
};

// The 26 hard-decided bytes (MSB-first bits) of a FLARM sync
bool accept_flarm(const uint8_t* bytes, Frame* f);
// ... of an OGN sync, with the soft bits (> 0 = a 1) and their mean |soft|
bool accept_ogn(const uint8_t* bytes, const float* soft, float amp, Frame* f);

// The parity checks, the systematic layout, TEA - shared with the tests
bool ldpc_ok(const uint8_t* bytes);
void tea_decrypt(uint32_t* v0, uint32_t* v1);
uint16_t crc16_flarm(const uint8_t* payload24);

}  // namespace pkt
