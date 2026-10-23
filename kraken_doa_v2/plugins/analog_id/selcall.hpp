#pragma once

// Sequential-tone (5-tone) selective calling standards and tone bursts.
// Frequencies, tone lengths and tolerances: Midian Electronics' "Tone
// signaling charts" (five / six / seven tone table, burst tones) - the
// standards' own values: ZVEI (Zentralverband der Elektrotechnischen
// Industrie), CCIR (Rec. 257, CCIR-1 / CCIR-2), EEA (UK), EIA (US), NATEL
// (Scandinavia), Motorola MODAT. A-F: group / reset / repeat tones (E =
// repeat the digit before; several makers changed A-F).

#include <cmath>
#include <string>
#include <vector>

namespace selcall {

struct Standard {
    const char* name;      // "ZVEI1"
    const char* family;    // the talker id's prefix: standards with the same digits share it
    float digit[10];
    float letter[6];       // A..F, 0 = none
    float width_ms;        // nominal tone length
    float tol;             // relative decode tolerance
    float tol_hz;          // and at least this (absolute)
    // how much less likely it is (decoder scores): DZVEI's digits are ZVEI's
    // moved up one tone and ZVEI2's repeat tone is DZVEI's 1, so a sequence
    // can read both ways (ZVEI2 33344 = DZVEI 41451) - the common one wins
    // unless the "Selcall standard" option says otherwise
    float prior;
};

inline const std::vector<Standard>& standards() {
    static const std::vector<Standard> s = {
        {"ZVEI1", "ZVEI", {2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200}, {2800, 810, 970, 885, 2600, 680}, 70, 0.015f, 8, 0},
        {"ZVEI2", "ZVEI", {2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200}, {885, 810, 740, 680, 970, 2600}, 70, 0.015f, 8, 0.05f},
        {"PZVEI", "ZVEI", {2400, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000, 2200}, {970, 810, 2800, 885, 2600, 680}, 70, 0.015f, 8, 0.1f},
        {"ZVEI3", "DZVEI", {2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000}, {885, 0, 0, 0, 2400, 0}, 70, 0.015f, 8, 0.45f},
        {"DZVEI", "DZVEI", {2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000}, {825, 740, 2600, 885, 2400, 680}, 70, 0.015f, 8, 0.4f},
        {"PDZVEI", "DZVEI", {2200, 970, 1060, 1160, 1270, 1400, 1530, 1670, 1830, 2000}, {825, 886, 2600, 856, 2400, 0}, 70, 0.015f, 8, 0.5f},
        {"CCIR1", "CCIR", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, {2400, 930, 2247, 991, 2110, 1055}, 100, 0.01f, 8, 0},
        {"CCIR2", "CCIR", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, {2400, 930, 2247, 991, 2110, 1055}, 70, 0.01f, 8, 0},
        {"PCCIR", "CCIR", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, {1050, 930, 2400, 991, 2110, 0}, 100, 0.01f, 8, 0.1f},
        {"EEA", "EEA", {1981, 1124, 1197, 1275, 1358, 1446, 1540, 1640, 1747, 1860}, {1055, 930, 2400, 991, 2110, 2247}, 40, 0.01f, 8, 0},
        {"EIA", "EIA", {600, 741, 882, 1023, 1164, 1305, 1446, 1587, 1728, 1869}, {2151, 2433, 2010, 2292, 459, 1091}, 33, 0.0f, 16, 0},
        {"NATEL", "NATEL", {1633, 631, 697, 770, 852, 941, 1040, 1209, 1336, 1477}, {1995, 571, 2205, 2437, 1805, 2694}, 70, 0.015f, 8, 0.05f},
        {"MODAT", "MODAT", {637.5f, 787.5f, 937.5f, 1087.5f, 1237.5f, 1387.5f, 1537.5f, 1687.5f, 1837.5f, 1987.5f}, {0, 0, 0, 0, 487.5f, 0}, 40, 0.01f, 8, 0.1f},
    };
    return s;
}

// The symbol (digit '0'-'9', 'A'-'F') of a standard nearest hz within its
// tolerance, or 0; *err = the relative distance
inline char symbol_of(const Standard& s, float hz, float* err) {
    char best = 0;
    float bd = 1e9f;
    auto test = [&](float f, char c) {
        if (f <= 0) return;
        const float d = std::fabs(hz - f);
        const float lim = std::max(s.tol * f, s.tol_hz);
        if (d <= lim && d / f < bd) {
            bd = d / f;
            best = c;
        }
    };
    for (int i = 0; i < 10; i++) test(s.digit[i], static_cast<char>('0' + i));
    for (int i = 0; i < 6; i++) test(s.letter[i], static_cast<char>('A' + i));
    if (err) *err = bd;
    return best;
}

// Burst tones (repeater access / "tone burst"): the standard list 1600-2550
// Hz in 50 Hz steps; 1750 Hz is the European repeater access tone
inline float burst_tone(float hz) {
    const float r = std::round(hz / 50.0f) * 50.0f;
    if (r < 1600 || r > 2550) return 0;
    return std::fabs(hz - r) <= std::max(0.0075f * r, 6.0f) ? r : 0;
}

}  // namespace selcall
