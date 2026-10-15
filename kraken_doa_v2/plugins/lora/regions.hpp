// Region tables of the lora plugin - every Meshtastic region and every
// LoRaWAN regional channel plan, so the plugin works anywhere. Facts only
// (frequencies, data rates), from public sources:
//  - Meshtastic: the firmware's region table (src/mesh/RadioInterface.cpp:
//    start / end MHz, spacing 0 everywhere), its preset table
//    (src/mesh/MeshRadio.h: SF / BW / CR) and the preset display names
//    (src/DisplayFormatters.cpp - the default channel hashes that name;
//    LONG_MODERATE is "LongMod"). Checked 2026-10-16 (firmware master).
//  - LoRaWAN: the LoRa Alliance Regional Parameters RP002-1.0.x (default /
//    common channels, data rates, RX2, class-B beacon channel / data rate /
//    size), cross-checked against the region headers of Semtech's
//    LoRaMac-node (BSD). Network-configured extra channels are covered by
//    the common grids below (EU868 867.1-867.9, AS923 922.0-923.4, KR920 /
//    CN470 their whole 200 kHz grids) - and by the plan-free search for
//    anything else.
// 2.4 GHz LoRa (Meshtastic LORA_24, SX128x) is outside an RTL-SDR's range
// and not listed.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace lora_reg {

struct Preset {
    const char* key;
    const char* name;      // the firmware's display name = the default channel's name (hashed)
    int sf;
    double bw;
    int cr;                // 1 = 4/5 .. 4 = 4/8
};
inline const Preset PRESETS[] = {
    {"shortturbo", "ShortTurbo", 7, 500000, 1}, {"shortfast", "ShortFast", 7, 250000, 1},
    {"shortslow", "ShortSlow", 8, 250000, 1},   {"mediumfast", "MediumFast", 9, 250000, 1},
    {"mediumslow", "MediumSlow", 10, 250000, 1}, {"longfast", "LongFast", 11, 250000, 1},
    {"longturbo", "LongTurbo", 11, 500000, 4},  {"longmod", "LongMod", 11, 125000, 4},
    {"longslow", "LongSlow", 12, 125000, 4},
};
constexpr int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

struct MeshRegion {
    const char* key;
    const char* name;
    double f0_mhz, f1_mhz;   // freqStart, freqEnd (spacing 0 in every region)
};
inline const MeshRegion MESH_REGIONS[] = {
    {"us", "US", 902.0, 928.0},          {"eu_433", "EU_433", 433.0, 434.0},
    {"eu_868", "EU_868", 869.4, 869.65}, {"cn", "CN", 470.0, 510.0},
    {"jp", "JP", 920.5, 923.5},          {"anz", "ANZ", 915.0, 928.0},
    {"anz_433", "ANZ_433", 433.05, 434.79}, {"ru", "RU", 868.7, 869.2},
    {"kr", "KR", 920.0, 923.0},          {"tw", "TW", 920.0, 925.0},
    {"in", "IN", 865.0, 867.0},          {"nz_865", "NZ_865", 864.0, 868.0},
    {"th", "TH", 920.0, 925.0},          {"ua_433", "UA_433", 433.0, 434.7},
    {"ua_868", "UA_868", 868.0, 868.6},  {"my_433", "MY_433", 433.0, 435.0},
    {"my_919", "MY_919", 919.0, 924.0},  {"sg_923", "SG_923", 917.0, 925.0},
    {"ph_433", "PH_433", 433.0, 434.7},  {"ph_868", "PH_868", 868.0, 869.4},
    {"ph_915", "PH_915", 915.0, 918.0},  {"kz_433", "KZ_433", 433.075, 434.775},
    {"kz_863", "KZ_863", 863.0, 868.0},  {"np_865", "NP_865", 865.0, 868.0},
    {"br_902", "BR_902", 902.0, 907.5},
};
constexpr int N_MESH_REGIONS = sizeof(MESH_REGIONS) / sizeof(MESH_REGIONS[0]);

// Meshtastic's channel-name hash (djb2, 32 bit, as the firmware)
inline uint32_t djb2(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}
// The firmware's frequency: numChannels = floor((end - start) / (spacing +
// bw)), slot = hash(channel name) % numChannels, f = start + bw/2 + slot x
// bw. 0 when the preset's bandwidth doesn't fit the region (the firmware
// falls back to LongFast there).
inline double mesh_freq_hz(const MeshRegion& r, double bw_hz, const std::string& name, int slot_override = 0) {
    const double bw = bw_hz / 1e6;
    const int n = static_cast<int>(std::floor((r.f1_mhz - r.f0_mhz) / bw + 1e-6));
    if (n < 1) return 0;
    const uint32_t slot = slot_override > 0 ? static_cast<uint32_t>(slot_override - 1) % static_cast<uint32_t>(n)
                                            : djb2(name) % static_cast<uint32_t>(n);
    return (r.f0_mhz + bw / 2 + slot * bw) * 1e6;
}

// LoRaWAN: a grid of channels with the data rates' SF range at one
// bandwidth; iq 0 = uplinks (normal IQ), 1 = downlinks (inverted IQ), 2 =
// both (the plans whose RX1 downlink uses the uplink channel)
struct LwGrid {
    double f0_mhz, step_mhz;
    int n;
    double bw;
    int sf_lo, sf_hi;
    int iq;
};
// Class-B beacon: implicit header, CR 4/5, no PHY CRC, preamble 10, `len`
// bytes (RFU1 | Param | Time | CRC1 | GwSpecific 7 | RFU2 | CRC2), sent by
// the gateways every 128 s (US915 / AU915: hopping over the 8 downlink
// channels)
struct LwBeacon {
    double f0_mhz, step_mhz;
    int n;
    int sf;
    double bw;
    int len;
};
struct LwPlan {
    const char* key;
    const char* name;
    double lo_mhz, hi_mhz;     // the band the plan lives in (Auto: the VFO overlaps it)
    LwGrid g[5];
    LwBeacon bcn;
};
inline const LwPlan LW_PLANS[] = {
    {"eu868", "EU868", 863.0, 870.0,
     {{868.1, 0.2, 3, 125000, 7, 12, 2},       // default channels (uplink + RX1)
      {867.1, 0.2, 5, 125000, 7, 12, 2},       // the common extra channels
      {868.3, 0, 1, 250000, 7, 7, 2},          // DR6 SF7 BW250 (where networks use it)
      {869.525, 0, 1, 125000, 7, 12, 1},       // RX2
      {}},
     {869.525, 0, 1, 9, 125000, 17}},
    {"us915", "US915", 902.0, 928.0,
     {{902.3, 0.2, 64, 125000, 7, 10, 0},      // uplink DR0-3
      {903.0, 1.6, 8, 500000, 8, 8, 0},        // uplink DR4
      {923.3, 0.6, 8, 500000, 7, 12, 1},       // downlink DR8-13 (RX2 923.3 DR8)
      {}, {}},
     {923.3, 0.6, 8, 12, 500000, 23}},
    {"au915", "AU915", 915.0, 928.0,
     {{915.2, 0.2, 64, 125000, 7, 12, 0},      // uplink DR0-5
      {915.9, 1.6, 8, 500000, 8, 8, 0},        // uplink DR6
      {923.3, 0.6, 8, 500000, 7, 12, 1},       // downlink DR8-13
      {}, {}},
     {923.3, 0.6, 8, 12, 500000, 23}},
    {"as923-1", "AS923-1", 915.0, 928.0,
     {{922.0, 0.2, 8, 125000, 7, 12, 2},       // 923.2 / 923.4 default + the common 922.0-923.0
      {922.1, 0, 1, 250000, 7, 7, 2},          // DR6 SF7 BW250
      {}, {}, {}},
     {923.4, 0, 1, 9, 125000, 17}},
    {"as923-2", "AS923-2", 915.0, 928.0,       // AS923-1 - 1.8 MHz
     {{920.2, 0.2, 8, 125000, 7, 12, 2}, {920.3, 0, 1, 250000, 7, 7, 2}, {}, {}, {}},
     {921.6, 0, 1, 9, 125000, 17}},
    {"as923-3", "AS923-3", 915.0, 928.0,       // - 6.6 MHz
     {{915.4, 0.2, 8, 125000, 7, 12, 2}, {915.5, 0, 1, 250000, 7, 7, 2}, {}, {}, {}},
     {916.8, 0, 1, 9, 125000, 17}},
    {"as923-4", "AS923-4", 915.0, 928.0,       // - 5.9 MHz
     {{916.1, 0.2, 8, 125000, 7, 12, 2}, {916.2, 0, 1, 250000, 7, 7, 2}, {}, {}, {}},
     {917.5, 0, 1, 9, 125000, 17}},
    {"kr920", "KR920", 920.9, 923.3,
     {{920.9, 0.2, 13, 125000, 7, 12, 2}, {}, {}, {}, {}},   // 922.1-922.5 default, RX2 921.9
     {923.1, 0, 1, 9, 125000, 17}},
    {"in865", "IN865", 865.0, 867.0,
     {{865.0625, 0, 1, 125000, 7, 12, 2}, {865.4025, 0, 1, 125000, 7, 12, 2},
      {865.985, 0, 1, 125000, 7, 12, 2}, {866.55, 0, 1, 125000, 7, 12, 1},   // RX2
      {}},
     {866.55, 0, 1, 8, 125000, 19}},
    {"ru864", "RU864", 864.0, 870.0,
     {{868.9, 0.2, 2, 125000, 7, 12, 2}, {}, {}, {}, {}},    // 868.9 / 869.1 (RX2 869.1)
     {869.1, 0, 1, 9, 125000, 17}},
    {"cn470", "CN470", 470.0, 510.0,
     {{470.3, 0.2, 200, 125000, 7, 12, 2}, {}, {}, {}, {}},  // every plan (legacy, 20 / 26 MHz A / B) is on this grid
     {0, 0, 0, 10, 125000, 19}},                             // beacon channel depends on the plan: not watched
    {"eu433", "EU433", 433.05, 434.79,
     {{433.175, 0.2, 3, 125000, 7, 12, 2}, {434.665, 0, 1, 125000, 7, 12, 1}, {}, {}, {}},
     {434.665, 0, 1, 9, 125000, 17}},
    {"cn779", "CN779", 779.0, 787.0,
     {{779.5, 0.2, 3, 125000, 7, 12, 2}, {786.0, 0, 1, 125000, 7, 12, 1}, {}, {}, {}},
     {785.0, 0, 1, 9, 125000, 17}},
};
constexpr int N_LW_PLANS = sizeof(LW_PLANS) / sizeof(LW_PLANS[0]);

// the LoRa bandwidths (SX127x / SX126x) and whether a channelizer channel
// can carry them (every one is a whole number of 520.8 Hz bins x 2)
inline const double BANDWIDTHS[] = {7812.5, 125000.0 / 12, 15625, 125000.0 / 6, 31250, 125000.0 / 3, 62500, 125000, 250000, 500000};
constexpr int N_BANDWIDTHS = sizeof(BANDWIDTHS) / sizeof(BANDWIDTHS[0]);
// a bandwidth as typed (10.4, 41.7 kHz ...) -> the exact LoRa bandwidth (0 = none within 3 %)
inline double snap_bw(double hz) {
    for (double b : BANDWIDTHS)
        if (std::fabs(hz - b) <= 0.03 * b) return b;
    return 0;
}

}  // namespace lora_reg
