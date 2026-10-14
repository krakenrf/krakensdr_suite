#pragma once

// OGN tracker packets (the Open Glider Network's open radio protocol, the
// "OGN1" format of the OGN tracker; github.com/pjalocha/esp32-ogn-tracker is
// its published reference, used as documentation): 20 bytes = five 32-bit
// little-endian words - a header (24-bit address, address type, flags) and
// four words of position or status / info - + 6 bytes of LDPC(208,160)
// parity. The four data words are "whitened" with the TEA cipher under the
// all-zero key (8 rounds per word pair) - a published algorithm with a
// published key, undone here - unless the packet's Encrypted flag says its
// owner encrypted it with a private key: those are shown by address only.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ogn {

// LDPC: number of failed parity checks of a 26-byte packet (0 = valid)
int ldpc_errors(const uint8_t* pkt);
// soft decoding (normalised min-sum): llr[i] > 0 = bit i is 0 (bit i = byte
// i / 8, bit i % 8 from the LSB); writes the 26 bytes, returns the number of
// bits changed against the hard decision, or -1 if it didn't converge
int ldpc_decode(const float* llr, uint8_t* pkt, int iterations = 16);

struct Packet {
    uint32_t addr = 0;
    int addr_type = 0;          // 0 random, 1 ICAO, 2 FLARM, 3 OGN
    bool non_pos = false, parity_ok = false, encrypted = false, emergency = false;
    int relay = 0;
    // position (non_pos == false, !encrypted)
    bool pos = false;
    double lat = 0, lon = 0;    // degrees
    int fix_quality = 0, fix_mode = 0, time_s = -1;
    double dop = 0;
    int alt_m = 0;
    bool has_baro = false;
    int baro_alt_m = 0;         // standard-pressure altitude
    double speed_ms = 0, heading = 0;
    bool has_climb = false, has_turn = false;
    double climb_ms = 0, turn_dps = 0;
    bool stealth = false;
    int acft_type = 0;
    // non-position (non_pos)
    int report_type = -1;       // 0 status, 1 info, 2 wind / thermal, 15 manufacturer
    // status
    int firmware = -1, hardware = -1, tx_dbm = -99, satellites = -1;
    double voltage = 0;
    // info: (name, value) - pilot name / crew / ICE are left out (personal)
    std::vector<std::pair<std::string, std::string>> info;
};
// pkt = 26 bytes that passed the LDPC check
bool decode(const uint8_t* pkt, Packet& p);
const char* acft_type_name(int t);
const char* addr_prefix(int addr_type);   // "RND", "ICA", "FLR", "OGN"

}  // namespace ogn
