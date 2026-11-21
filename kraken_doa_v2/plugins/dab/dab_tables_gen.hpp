#pragma once
// GENERATED - do not edit by hand. Machine-extracted from the ETSI documents' text
// (pdftotext -layout) by a script, then checked by dab_tables.cpp's self test:
//   EN 300 401 V2.2.1: tables 8, 13, 15, 23, 24, 26
//   TS 101 756 V2.5.1: tables 3-7, 9, 10, 12, 13, C.2
//   TS 103 461 V1.2.1: table E.1
#include <cstdint>

namespace dab::gen {

// EN 300 401 table 13: puncturing vectors V_PI (PI = 1..24), v_PI,0 first
inline constexpr const char* PUNCT[25] = {"",
    "11001000100010001000100010001000",
    "11001000100010001100100010001000",
    "11001000110010001100100010001000",
    "11001000110010001100100011001000",
    "11001100110010001100100011001000",
    "11001100110010001100110011001000",
    "11001100110011001100110011001000",
    "11001100110011001100110011001100",
    "11101100110011001100110011001100",
    "11101100110011001110110011001100",
    "11101100111011001110110011001100",
    "11101100111011001110110011101100",
    "11101110111011001110110011101100",
    "11101110111011001110111011101100",
    "11101110111011101110111011101100",
    "11101110111011101110111011101110",
    "11111110111011101110111011101110",
    "11111110111011101111111011101110",
    "11111110111111101111111011101110",
    "11111110111111101111111011111110",
    "11111111111111101111111011111110",
    "11111111111111101111111111111110",
    "11111111111111111111111111111110",
    "11111111111111111111111111111111",
};

// EN 300 401 table 8: UEP short form, table index -> sub-channel size (CU), protection level, kbit/s
struct UepIndex { int size_cu, level, kbps; };
inline constexpr UepIndex TABLE8[64] = {
    {16, 5, 32},   // 0
    {21, 4, 32},   // 1
    {24, 3, 32},   // 2
    {29, 2, 32},   // 3
    {35, 1, 32},   // 4
    {24, 5, 48},   // 5
    {29, 4, 48},   // 6
    {35, 3, 48},   // 7
    {42, 2, 48},   // 8
    {52, 1, 48},   // 9
    {29, 5, 56},   // 10
    {35, 4, 56},   // 11
    {42, 3, 56},   // 12
    {52, 2, 56},   // 13
    {32, 5, 64},   // 14
    {42, 4, 64},   // 15
    {48, 3, 64},   // 16
    {58, 2, 64},   // 17
    {70, 1, 64},   // 18
    {40, 5, 80},   // 19
    {52, 4, 80},   // 20
    {58, 3, 80},   // 21
    {70, 2, 80},   // 22
    {84, 1, 80},   // 23
    {48, 5, 96},   // 24
    {58, 4, 96},   // 25
    {70, 3, 96},   // 26
    {84, 2, 96},   // 27
    {104, 1, 96},   // 28
    {58, 5, 112},   // 29
    {70, 4, 112},   // 30
    {84, 3, 112},   // 31
    {104, 2, 112},   // 32
    {64, 5, 128},   // 33
    {84, 4, 128},   // 34
    {96, 3, 128},   // 35
    {116, 2, 128},   // 36
    {140, 1, 128},   // 37
    {80, 5, 160},   // 38
    {104, 4, 160},   // 39
    {116, 3, 160},   // 40
    {140, 2, 160},   // 41
    {168, 1, 160},   // 42
    {96, 5, 192},   // 43
    {116, 4, 192},   // 44
    {140, 3, 192},   // 45
    {168, 2, 192},   // 46
    {208, 1, 192},   // 47
    {116, 5, 224},   // 48
    {140, 4, 224},   // 49
    {168, 3, 224},   // 50
    {208, 2, 224},   // 51
    {232, 1, 224},   // 52
    {128, 5, 256},   // 53
    {168, 4, 256},   // 54
    {192, 3, 256},   // 55
    {232, 2, 256},   // 56
    {280, 1, 256},   // 57
    {160, 5, 320},   // 58
    {208, 4, 320},   // 59
    {280, 2, 320},   // 60
    {192, 5, 384},   // 61
    {280, 3, 384},   // 62
    {416, 1, 384},   // 63
};

// EN 300 401 table 15: UEP profiles: kbit/s, P, L1..L4, PI1..PI4 (0 = none), padding bits
struct UepProfile { int kbps, level, L[4], PI[4], pad; };
inline constexpr UepProfile TABLE15[] = {
    {32, 5, {3, 4, 17, 0}, {5, 3, 2, 0}, 0},
    {32, 4, {3, 3, 18, 0}, {11, 6, 5, 0}, 0},
    {32, 3, {3, 4, 14, 3}, {15, 9, 6, 8}, 0},
    {32, 2, {3, 4, 14, 3}, {22, 13, 8, 13}, 0},
    {32, 1, {3, 5, 13, 3}, {24, 17, 12, 17}, 4},
    {48, 5, {4, 3, 26, 3}, {5, 4, 2, 3}, 0},
    {48, 4, {3, 4, 26, 3}, {9, 6, 4, 6}, 0},
    {48, 3, {3, 4, 26, 3}, {15, 10, 6, 9}, 4},
    {48, 2, {3, 4, 26, 3}, {24, 14, 8, 15}, 0},
    {48, 1, {3, 5, 25, 3}, {24, 18, 13, 18}, 0},
    {56, 5, {6, 10, 23, 3}, {5, 4, 2, 3}, 0},
    {56, 4, {6, 10, 23, 3}, {9, 6, 4, 5}, 0},
    {56, 3, {6, 12, 21, 3}, {16, 7, 6, 9}, 0},
    {56, 2, {6, 10, 23, 3}, {23, 13, 8, 13}, 8},
    {64, 5, {6, 9, 31, 2}, {5, 3, 2, 3}, 0},
    {64, 4, {6, 9, 33, 0}, {11, 6, 5, 0}, 0},
    {64, 3, {6, 12, 27, 3}, {16, 8, 6, 9}, 0},
    {64, 2, {6, 10, 29, 3}, {23, 13, 8, 13}, 8},
    {64, 1, {6, 11, 28, 3}, {24, 18, 12, 18}, 4},
    {80, 5, {6, 10, 41, 3}, {6, 3, 2, 3}, 0},
    {80, 4, {6, 10, 41, 3}, {11, 6, 5, 6}, 0},
    {80, 3, {6, 11, 40, 3}, {16, 8, 6, 7}, 0},
    {80, 2, {6, 10, 41, 3}, {23, 13, 8, 13}, 8},
    {80, 1, {6, 10, 41, 3}, {24, 17, 12, 18}, 4},
    {96, 5, {7, 9, 53, 3}, {5, 4, 2, 4}, 0},
    {96, 4, {7, 10, 52, 3}, {9, 6, 4, 6}, 0},
    {96, 3, {6, 12, 51, 3}, {16, 9, 6, 10}, 4},
    {96, 2, {6, 10, 53, 3}, {22, 12, 9, 12}, 0},
    {96, 1, {6, 13, 50, 3}, {24, 18, 13, 19}, 0},
    {112, 5, {14, 17, 50, 3}, {5, 4, 2, 5}, 0},
    {112, 4, {11, 21, 49, 3}, {9, 6, 4, 8}, 0},
    {112, 3, {11, 23, 47, 3}, {16, 8, 6, 9}, 0},
    {112, 2, {11, 21, 49, 3}, {23, 12, 9, 14}, 4},
    {128, 5, {12, 19, 62, 3}, {5, 3, 2, 4}, 0},
    {128, 4, {11, 21, 61, 3}, {11, 6, 5, 7}, 0},
    {128, 3, {11, 22, 60, 3}, {16, 9, 6, 10}, 4},
    {128, 2, {11, 21, 61, 3}, {22, 12, 9, 14}, 0},
    {128, 1, {11, 20, 62, 3}, {24, 17, 13, 19}, 8},
    {160, 5, {11, 19, 87, 3}, {5, 4, 2, 4}, 0},
    {160, 4, {11, 23, 83, 3}, {11, 6, 5, 9}, 0},
    {160, 3, {11, 24, 82, 3}, {16, 8, 6, 11}, 0},
    {160, 2, {11, 21, 85, 3}, {22, 11, 9, 13}, 0},
    {160, 1, {11, 22, 84, 3}, {24, 18, 12, 19}, 0},
    {192, 5, {11, 20, 110, 3}, {6, 4, 2, 5}, 0},
    {192, 4, {11, 22, 108, 3}, {10, 6, 4, 9}, 0},
    {192, 3, {11, 24, 106, 3}, {16, 10, 6, 11}, 0},
    {192, 2, {11, 20, 110, 3}, {22, 13, 9, 13}, 8},
    {192, 1, {11, 21, 109, 3}, {24, 20, 13, 24}, 0},
    {224, 5, {12, 22, 131, 3}, {8, 6, 2, 6}, 4},
    {224, 4, {12, 26, 127, 3}, {12, 8, 4, 11}, 0},
    {224, 3, {11, 20, 134, 3}, {16, 10, 7, 9}, 0},
    {224, 2, {11, 22, 132, 3}, {24, 16, 10, 15}, 0},
    {224, 1, {11, 24, 130, 3}, {24, 20, 12, 20}, 4},
    {256, 5, {11, 24, 154, 3}, {6, 5, 2, 5}, 0},
    {256, 4, {11, 24, 154, 3}, {12, 9, 5, 10}, 4},
    {256, 3, {11, 27, 151, 3}, {16, 10, 7, 10}, 0},
    {256, 2, {11, 22, 156, 3}, {24, 14, 10, 13}, 8},
    {256, 1, {11, 26, 152, 3}, {24, 19, 14, 18}, 4},
    {320, 5, {11, 26, 200, 3}, {8, 5, 2, 6}, 4},
    {320, 4, {11, 25, 201, 3}, {13, 9, 5, 10}, 8},
    {320, 2, {11, 26, 200, 3}, {24, 17, 9, 17}, 0},
    {384, 5, {11, 27, 247, 3}, {8, 6, 2, 7}, 0},
    {384, 3, {11, 24, 250, 3}, {16, 9, 7, 10}, 4},
    {384, 1, {12, 28, 245, 3}, {24, 20, 14, 23}, 8},
};
inline constexpr int TABLE15_N = 64;

// EN 300 401 table 23: carrier range kmin..kmax -> k', i, n (transmission mode I)
struct PrsRange { int kmin, kmax, kp, i, n; };
inline constexpr PrsRange TABLE23[48] = {
    {-768, -737, -768, 0, 1},
    {-736, -705, -736, 1, 2},
    {-704, -673, -704, 2, 0},
    {-672, -641, -672, 3, 1},
    {-640, -609, -640, 0, 3},
    {-608, -577, -608, 1, 2},
    {-576, -545, -576, 2, 2},
    {-544, -513, -544, 3, 3},
    {-512, -481, -512, 0, 2},
    {-480, -449, -480, 1, 1},
    {-448, -417, -448, 2, 2},
    {-416, -385, -416, 3, 3},
    {-384, -353, -384, 0, 1},
    {-352, -321, -352, 1, 2},
    {-320, -289, -320, 2, 3},
    {-288, -257, -288, 3, 3},
    {-256, -225, -256, 0, 2},
    {-224, -193, -224, 1, 2},
    {-192, -161, -192, 2, 2},
    {-160, -129, -160, 3, 1},
    {-128, -97, -128, 0, 1},
    {-96, -65, -96, 1, 3},
    {-64, -33, -64, 2, 1},
    {-32, -1, -32, 3, 2},
    {1, 32, 1, 0, 3},
    {33, 64, 33, 3, 1},
    {65, 96, 65, 2, 1},
    {97, 128, 97, 1, 1},
    {129, 160, 129, 0, 2},
    {161, 192, 161, 3, 2},
    {193, 224, 193, 2, 1},
    {225, 256, 225, 1, 0},
    {257, 288, 257, 0, 2},
    {289, 320, 289, 3, 2},
    {321, 352, 321, 2, 3},
    {353, 384, 353, 1, 3},
    {385, 416, 385, 0, 0},
    {417, 448, 417, 3, 2},
    {449, 480, 449, 2, 1},
    {481, 512, 481, 1, 3},
    {513, 544, 513, 0, 3},
    {545, 576, 545, 3, 3},
    {577, 608, 577, 2, 3},
    {609, 640, 609, 1, 0},
    {641, 672, 641, 0, 3},
    {673, 704, 673, 3, 0},
    {705, 736, 705, 2, 1},
    {737, 768, 737, 1, 1},
};
// EN 300 401 table 24: h_i,j
inline constexpr int8_t TABLE24[4][32] = {
    {0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1, 0, 2, 0, 0, 0, 0, 1, 1, 2, 0, 0, 0, 2, 2, 1, 1},
    {0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0, 0, 3, 2, 3, 0, 1, 3, 0, 2, 1, 2, 3, 2, 3, 3, 0},
    {0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3, 0, 0, 0, 2, 0, 2, 1, 3, 2, 2, 0, 2, 2, 0, 1, 3},
    {0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2, 0, 1, 2, 1, 0, 3, 3, 2, 2, 3, 2, 1, 2, 1, 3, 2},
};

// EN 300 401 table 26: TII pattern a_b(p), b = 0..7 left to right
inline constexpr const char* TII_PATTERN[70] = {
    "00001111",   // 0
    "00010111",   // 1
    "00011011",   // 2
    "00011101",   // 3
    "00011110",   // 4
    "00100111",   // 5
    "00101011",   // 6
    "00101101",   // 7
    "00101110",   // 8
    "00110011",   // 9
    "00110101",   // 10
    "00110110",   // 11
    "00111001",   // 12
    "00111010",   // 13
    "00111100",   // 14
    "01000111",   // 15
    "01001011",   // 16
    "01001101",   // 17
    "01001110",   // 18
    "01010011",   // 19
    "01010101",   // 20
    "01010110",   // 21
    "01011001",   // 22
    "01011010",   // 23
    "01011100",   // 24
    "01100011",   // 25
    "01100101",   // 26
    "01100110",   // 27
    "01101001",   // 28
    "01101010",   // 29
    "01101100",   // 30
    "01110001",   // 31
    "01110010",   // 32
    "01110100",   // 33
    "01111000",   // 34
    "10000111",   // 35
    "10001011",   // 36
    "10001101",   // 37
    "10001110",   // 38
    "10010011",   // 39
    "10010101",   // 40
    "10010110",   // 41
    "10011001",   // 42
    "10011010",   // 43
    "10011100",   // 44
    "10100011",   // 45
    "10100101",   // 46
    "10100110",   // 47
    "10101001",   // 48
    "10101010",   // 49
    "10101100",   // 50
    "10110001",   // 51
    "10110010",   // 52
    "10110100",   // 53
    "10111000",   // 54
    "11000011",   // 55
    "11000101",   // 56
    "11000110",   // 57
    "11001001",   // 58
    "11001010",   // 59
    "11001100",   // 60
    "11010001",   // 61
    "11010010",   // 62
    "11010100",   // 63
    "11011000",   // 64
    "11100001",   // 65
    "11100010",   // 66
    "11100100",   // 67
    "11101000",   // 68
    "11110000",   // 69
};

// TS 101 756 table C.2: complete EBU Latin based repertoire -> Unicode (0 = reserved, not displayed)
inline constexpr uint16_t EBU_LATIN[256] = {
    0x0000, 0x0118, 0x012E, 0x0172, 0x0102, 0x0116, 0x010E, 0x0218,
    0x021A, 0x010A, 0x0000, 0x0000, 0x0120, 0x0139, 0x017B, 0x0143,
    0x0105, 0x0119, 0x012F, 0x0173, 0x0103, 0x0117, 0x010F, 0x0219,
    0x021B, 0x010B, 0x0147, 0x011A, 0x0121, 0x013A, 0x017C, 0x0000,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0142, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x016E, 0x005D, 0x0141, 0x005F,
    0x0104, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00AB, 0x016F, 0x00BB, 0x013D, 0x0126,
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2,
    0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x00DF, 0x00A1, 0x0178,
    0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6,
    0x00FB, 0x00FC, 0x00F1, 0x00E7, 0x015F, 0x011F, 0x0131, 0x00FF,
    0x0136, 0x0145, 0x00A9, 0x0122, 0x011E, 0x011B, 0x0148, 0x0151,
    0x0150, 0x20AC, 0x00A3, 0x0024, 0x0100, 0x0112, 0x012A, 0x016A,
    0x0137, 0x0146, 0x013B, 0x0123, 0x013C, 0x0130, 0x0144, 0x0171,
    0x0170, 0x00BF, 0x013E, 0x00B0, 0x0101, 0x0113, 0x012B, 0x016B,
    0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC, 0x00D3, 0x00D2,
    0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F,
    0x00C2, 0x00C4, 0x00CA, 0x00CB, 0x00CE, 0x00CF, 0x00D4, 0x00D6,
    0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140,
    0x00C3, 0x00C5, 0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8,
    0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0164, 0x00F0,
    0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8,
    0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A, 0x0165, 0x0127,
};

// TS 101 756 tables 3-7: (ECC, country id) -> country (" / " where several share a code)
struct Country { uint8_t ecc, cid; const char* name; };
inline constexpr Country COUNTRIES[] = {
    {0xA0, 0x1, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x2, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x3, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x4, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x5, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x6, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x7, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x8, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0x9, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0xA, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0xB, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0xD, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA0, 0xE, "Puerto Rico / United States of America / Virgin Islands (USA)"},
    {0xA1, 0xB, "Canada"},
    {0xA1, 0xC, "Canada"},
    {0xA1, 0xD, "Canada"},
    {0xA1, 0xE, "Canada"},
    {0xA1, 0xF, "Greenland"},
    {0xA2, 0x1, "Anguilla"},
    {0xA2, 0x2, "Antigua and Barbuda"},
    {0xA2, 0x3, "Ecuador"},
    {0xA2, 0x4, "Falkland Islands"},
    {0xA2, 0x5, "Barbados"},
    {0xA2, 0x6, "Belize"},
    {0xA2, 0x7, "Cayman Islands"},
    {0xA2, 0x8, "Costa Rica"},
    {0xA2, 0x9, "Cuba"},
    {0xA2, 0xA, "Argentina"},
    {0xA2, 0xB, "Brazil"},
    {0xA2, 0xC, "Bermuda"},
    {0xA2, 0xD, "Netherlands Antilles"},
    {0xA2, 0xE, "Guadeloupe"},
    {0xA2, 0xF, "Bahamas"},
    {0xA3, 0x1, "Bolivia"},
    {0xA3, 0x2, "Colombia"},
    {0xA3, 0x3, "Jamaica"},
    {0xA3, 0x4, "Martinique"},
    {0xA3, 0x6, "Paraguay"},
    {0xA3, 0x7, "Nicaragua"},
    {0xA3, 0x9, "Panama"},
    {0xA3, 0xA, "Dominica"},
    {0xA3, 0xB, "Dominican Republic"},
    {0xA3, 0xC, "Chile"},
    {0xA3, 0xD, "Grenada"},
    {0xA3, 0xE, "Turks and Caicos islands"},
    {0xA3, 0xF, "Guyana"},
    {0xA4, 0x1, "Guatemala"},
    {0xA4, 0x2, "Honduras"},
    {0xA4, 0x3, "Aruba"},
    {0xA4, 0x5, "Montserrat"},
    {0xA4, 0x6, "Trinidad and Tobago"},
    {0xA4, 0x7, "Peru"},
    {0xA4, 0x8, "Suriname"},
    {0xA4, 0x9, "Uruguay"},
    {0xA4, 0xA, "St. Kitts and Nevis"},
    {0xA4, 0xB, "St. Lucia"},
    {0xA4, 0xC, "El Salvador"},
    {0xA4, 0xD, "Haiti"},
    {0xA4, 0xE, "Venezuela"},
    {0xA4, 0xF, "Virgin Islands (British)"},
    {0xA5, 0xB, "Mexico"},
    {0xA5, 0xC, "St. Vincent"},
    {0xA5, 0xD, "Mexico"},
    {0xA5, 0xE, "Mexico"},
    {0xA5, 0xF, "Mexico"},
    {0xA6, 0x3, "Brazil"},
    {0xA6, 0xC, "Brazil"},
    {0xA6, 0xD, "Brazil"},
    {0xA6, 0xF, "St. Pierre and Miquelon"},
    {0xD0, 0x1, "Cameroon"},
    {0xD0, 0x2, "Central African Republic"},
    {0xD0, 0x3, "Djibouti"},
    {0xD0, 0x4, "Madagascar"},
    {0xD0, 0x5, "Mali"},
    {0xD0, 0x6, "Angola"},
    {0xD0, 0x7, "Equatorial Guinea"},
    {0xD0, 0x8, "Gabon"},
    {0xD0, 0x9, "Guinea"},
    {0xD0, 0xA, "South Africa"},
    {0xD0, 0xB, "Burkina Faso"},
    {0xD0, 0xC, "Congo (Republic of)"},
    {0xD0, 0xD, "Togo"},
    {0xD0, 0xE, "Benin"},
    {0xD0, 0xF, "Malawi"},
    {0xD1, 0x1, "Namibia"},
    {0xD1, 0x2, "Liberia"},
    {0xD1, 0x3, "Ghana"},
    {0xD1, 0x4, "Mauritania"},
    {0xD1, 0x5, "Sao Tome and Principe"},
    {0xD1, 0x6, "Cape Verde"},
    {0xD1, 0x7, "Senegal"},
    {0xD1, 0x8, "Gambia"},
    {0xD1, 0x9, "Burundi"},
    {0xD1, 0xA, "Ascension Island"},
    {0xD1, 0xB, "Botswana"},
    {0xD1, 0xC, "Comoros"},
    {0xD1, 0xD, "Tanzania"},
    {0xD1, 0xE, "Ethiopia"},
    {0xD1, 0xF, "Nigeria"},
    {0xD2, 0x1, "Sierra Leone"},
    {0xD2, 0x2, "Zimbabwe"},
    {0xD2, 0x3, "Mozambique"},
    {0xD2, 0x4, "Uganda"},
    {0xD2, 0x5, "Eswatini"},
    {0xD2, 0x6, "Kenya"},
    {0xD2, 0x7, "Somalia"},
    {0xD2, 0x8, "Niger"},
    {0xD2, 0x9, "Chad"},
    {0xD2, 0xA, "Guinea-Bissau"},
    {0xD2, 0xB, "Dem. Republic of the Congo"},
    {0xD2, 0xC, "Cote d'Ivoire"},
    {0xD2, 0xD, "Zanzibar"},
    {0xD2, 0xE, "Zambia"},
    {0xD2, 0xF, "Eritrea"},
    {0xD3, 0x3, "Western Sahara"},
    {0xD3, 0x5, "Rwanda"},
    {0xD3, 0x6, "Lesotho"},
    {0xD3, 0x8, "Seychelles"},
    {0xD3, 0xA, "Mauritius"},
    {0xD3, 0xC, "Sudan"},
    {0xD4, 0xA, "South Sudan"},
    {0xE0, 0x1, "Germany"},
    {0xE0, 0x2, "Algeria"},
    {0xE0, 0x3, "Andorra"},
    {0xE0, 0x4, "Israel"},
    {0xE0, 0x5, "Italy"},
    {0xE0, 0x6, "Belgium"},
    {0xE0, 0x7, "Russian Federation"},
    {0xE0, 0x8, "Palestine"},
    {0xE0, 0x9, "Albania"},
    {0xE0, 0xA, "Austria"},
    {0xE0, 0xB, "Hungary"},
    {0xE0, 0xC, "Malta"},
    {0xE0, 0xD, "Germany"},
    {0xE0, 0xF, "Egypt"},
    {0xE1, 0x1, "Greece"},
    {0xE1, 0x2, "Cyprus"},
    {0xE1, 0x3, "San Marino"},
    {0xE1, 0x4, "Switzerland"},
    {0xE1, 0x5, "Jordan"},
    {0xE1, 0x6, "Finland"},
    {0xE1, 0x7, "Luxembourg"},
    {0xE1, 0x8, "Bulgaria"},
    {0xE1, 0x9, "Denmark / Faroe"},
    {0xE1, 0xA, "Gibraltar"},
    {0xE1, 0xB, "Iraq"},
    {0xE1, 0xC, "United Kingdom"},
    {0xE1, 0xD, "Libya"},
    {0xE1, 0xE, "Romania"},
    {0xE1, 0xF, "France"},
    {0xE2, 0x1, "Morocco"},
    {0xE2, 0x2, "Czech Republic"},
    {0xE2, 0x3, "Poland"},
    {0xE2, 0x4, "Vatican"},
    {0xE2, 0x5, "Slovakia"},
    {0xE2, 0x6, "Syrian Arab Republic"},
    {0xE2, 0x7, "Tunisia"},
    {0xE2, 0x9, "Liechtenstein"},
    {0xE2, 0xA, "Iceland"},
    {0xE2, 0xB, "Monaco"},
    {0xE2, 0xC, "Lithuania"},
    {0xE2, 0xD, "Serbia"},
    {0xE2, 0xE, "Canary Islands / Spain"},
    {0xE2, 0xF, "Norway"},
    {0xE3, 0x1, "Montenegro"},
    {0xE3, 0x2, "Ireland"},
    {0xE3, 0x3, "Turkey"},
    {0xE3, 0x5, "Tajikistan"},
    {0xE3, 0x8, "Netherlands"},
    {0xE3, 0x9, "Latvia"},
    {0xE3, 0xA, "Lebanon"},
    {0xE3, 0xB, "Azerbaijan"},
    {0xE3, 0xC, "Croatia"},
    {0xE3, 0xD, "Kazakhstan"},
    {0xE3, 0xE, "Sweden"},
    {0xE3, 0xF, "Belarus"},
    {0xE4, 0x1, "Moldova"},
    {0xE4, 0x2, "Estonia"},
    {0xE4, 0x3, "Republic of North Macedonia"},
    {0xE4, 0x6, "Ukraine"},
    {0xE4, 0x7, "Kosovo"},
    {0xE4, 0x8, "Madeira / Azores / Portugal"},
    {0xE4, 0x9, "Slovenia"},
    {0xE4, 0xA, "Armenia"},
    {0xE4, 0xB, "Uzbekistan"},
    {0xE4, 0xC, "Georgia"},
    {0xE4, 0xE, "Turkmenistan"},
    {0xE4, 0xF, "Bosnia and Herzegovina"},
    {0xE5, 0x3, "Kyrgyzstan"},
    {0xF0, 0x1, "Australia (capital cities, commercial / community)"},
    {0xF0, 0x2, "Australia (regional New South Wales / ACT)"},
    {0xF0, 0x3, "Australia (capital cities, national)"},
    {0xF0, 0x4, "Australia (regional Queensland)"},
    {0xF0, 0x5, "Australia (regional South Australia / NT)"},
    {0xF0, 0x6, "Australia (regional Western Australia)"},
    {0xF0, 0x7, "Australia (regional Victoria / Tasmania)"},
    {0xF0, 0x8, "Australia (regional)"},
    {0xF0, 0x9, "Saudi Arabia"},
    {0xF0, 0xA, "Afghanistan"},
    {0xF0, 0xB, "Myanmar"},
    {0xF0, 0xC, "China"},
    {0xF0, 0xD, "Korea (North)"},
    {0xF0, 0xE, "Bahrain"},
    {0xF0, 0xF, "Malaysia"},
    {0xF1, 0x1, "Kiribati"},
    {0xF1, 0x2, "Bhutan"},
    {0xF1, 0x3, "Bangladesh"},
    {0xF1, 0x4, "Pakistan"},
    {0xF1, 0x5, "Fiji"},
    {0xF1, 0x6, "Oman"},
    {0xF1, 0x7, "Nauru"},
    {0xF1, 0x8, "Iran"},
    {0xF1, 0x9, "New Zealand"},
    {0xF1, 0xA, "Solomon Islands"},
    {0xF1, 0xB, "Brunei Darussalam"},
    {0xF1, 0xC, "Sri Lanka"},
    {0xF1, 0xD, "Taiwan"},
    {0xF1, 0xE, "Korea (South)"},
    {0xF1, 0xF, "Hong Kong"},
    {0xF2, 0x1, "Kuwait"},
    {0xF2, 0x2, "Qatar"},
    {0xF2, 0x3, "Cambodia"},
    {0xF2, 0x4, "Samoa"},
    {0xF2, 0x5, "India"},
    {0xF2, 0x6, "Macau"},
    {0xF2, 0x7, "Vietnam"},
    {0xF2, 0x8, "Philippines"},
    {0xF2, 0x9, "Japan"},
    {0xF2, 0xA, "Singapore"},
    {0xF2, 0xB, "Maldives"},
    {0xF2, 0xC, "Indonesia"},
    {0xF2, 0xD, "United Arab Emirates"},
    {0xF2, 0xE, "Nepal"},
    {0xF2, 0xF, "Vanuatu"},
    {0xF3, 0x1, "Laos"},
    {0xF3, 0x2, "Thailand"},
    {0xF3, 0x3, "Tonga"},
    {0xF3, 0x9, "Papua New Guinea"},
    {0xF3, 0xB, "Yemen"},
    {0xF3, 0xE, "Micronesia"},
    {0xF3, 0xF, "Mongolia"},
};
inline constexpr int COUNTRIES_N = 241;

// TS 101 756 tables 9 / 10: language codes
struct Language { uint8_t code; const char* name; };
inline constexpr Language LANGUAGES[] = {
    {0x00, "Unknown/not applicable"},
    {0x01, "Albanian"},
    {0x02, "Breton"},
    {0x03, "Catalan"},
    {0x04, "Croatian"},
    {0x05, "Welsh"},
    {0x06, "Czech"},
    {0x07, "Danish"},
    {0x08, "German"},
    {0x09, "English"},
    {0x0A, "Spanish"},
    {0x0B, "Esperanto"},
    {0x0C, "Estonian"},
    {0x0D, "Basque"},
    {0x0E, "Faroese"},
    {0x0F, "French"},
    {0x10, "Frisian"},
    {0x11, "Irish"},
    {0x12, "Gaelic"},
    {0x13, "Galician"},
    {0x14, "Icelandic"},
    {0x15, "Italian"},
    {0x16, "Sami"},
    {0x17, "Latin"},
    {0x18, "Latvian"},
    {0x19, "Luxembourgian"},
    {0x1A, "Lithuanian"},
    {0x1B, "Hungarian"},
    {0x1C, "Maltese"},
    {0x1D, "Dutch"},
    {0x1E, "Norwegian"},
    {0x1F, "Occitan"},
    {0x20, "Polish"},
    {0x21, "Portuguese"},
    {0x22, "Romanian"},
    {0x23, "Romansh"},
    {0x24, "Serbian"},
    {0x25, "Slovak"},
    {0x26, "Slovene"},
    {0x27, "Finnish"},
    {0x28, "Swedish"},
    {0x29, "Turkish"},
    {0x2A, "Flemish"},
    {0x2B, "Walloon"},
    {0x40, "Background sound/clean feed"},
    {0x45, "Zulu"},
    {0x46, "Vietnamese"},
    {0x47, "Uzbek"},
    {0x48, "Urdu"},
    {0x49, "Ukranian"},
    {0x4A, "Thai"},
    {0x4B, "Telugu"},
    {0x4C, "Tatar"},
    {0x4D, "Tamil"},
    {0x4E, "Tadzhik"},
    {0x4F, "Swahili"},
    {0x50, "Sranan Tongo"},
    {0x51, "Somali"},
    {0x52, "Sinhalese"},
    {0x53, "Shona"},
    {0x54, "Serbo-Croat"},
    {0x55, "Rusyn"},
    {0x56, "Russian"},
    {0x57, "Quechua"},
    {0x58, "Pushtu"},
    {0x59, "Punjabi"},
    {0x5A, "Persian"},
    {0x5B, "Papiamento"},
    {0x5C, "Oriya"},
    {0x5D, "Nepali"},
    {0x5E, "Ndebele"},
    {0x5F, "Marathi"},
    {0x60, "Moldavian"},
    {0x61, "Malaysian"},
    {0x62, "Malagasay"},
    {0x63, "Macedonian"},
    {0x64, "Laotian"},
    {0x65, "Korean"},
    {0x66, "Khmer"},
    {0x67, "Kazakh"},
    {0x68, "Kannada"},
    {0x69, "Japanese"},
    {0x6A, "Indonesian"},
    {0x6B, "Hindi"},
    {0x6C, "Hebrew"},
    {0x6D, "Hausa"},
    {0x6E, "Gurani"},
    {0x6F, "Gujurati"},
    {0x70, "Greek"},
    {0x71, "Georgian"},
    {0x72, "Fulani"},
    {0x73, "Dari"},
    {0x74, "Chuvash"},
    {0x75, "Chinese"},
    {0x76, "Burmese"},
    {0x77, "Bulgarian"},
    {0x78, "Bengali"},
    {0x79, "Belorussian"},
    {0x7A, "Bambora"},
    {0x7B, "Azerbaijani"},
    {0x7C, "Assamese"},
    {0x7D, "Armenian"},
    {0x7E, "Arabic"},
    {0x7F, "Amharic"},
};
inline constexpr int LANGUAGES_N = 104;

// TS 101 756 table 12 (international table 1) and table 13 (international table 2, North America)
inline constexpr const char* PTY_TABLE12[32] = {
    "No programme type",
    "News",
    "Current Affairs",
    "Information",
    "Sport",
    "Education",
    "Drama",
    "Culture",
    "Science",
    "Varied",
    "Pop Music",
    "Rock Music",
    "Easy Listening Music",
    "Light Classical",
    "Serious Classical",
    "Other Music",
    "Weather/meteorology",
    "Finance/Business",
    "Children's programmes",
    "Social Affairs",
    "Religion",
    "Phone In",
    "Travel",
    "Leisure",
    "Jazz Music",
    "Country Music",
    "National Music",
    "Oldies Music",
    "Folk Music",
    "Documentary",
    "",
    "",
};
inline constexpr const char* PTY_TABLE13[32] = {
    "No program type",
    "News",
    "Information",
    "Sports",
    "Talk",
    "Rock",
    "Classic Rock",
    "Adult Hits",
    "Soft Rock",
    "Top 40",
    "Country",
    "Oldies",
    "Soft",
    "Nostalgia",
    "Jazz",
    "Classical",
    "Rhythm and Blues",
    "Soft Rhythm and Blues",
    "Foreign Language",
    "Religious Music",
    "Religious Talk",
    "Personality",
    "Public",
    "College",
    "",
    "",
    "",
    "",
    "",
    "Weather",
    "",
    "",
};

// TS 101 756 table 14: announcement types (bit flags)
inline constexpr const char* ANNOUNCEMENT[16] = {
    "Alarm",
    "Road Traffic flash",
    "Transport flash",
    "Warning/Service",
    "News flash",
    "Area weather flash",
    "Event announcement",
    "Special event",
    "Programme Information",
    "Sport report",
    "Financial report",
    "",
    "",
    "",
    "",
    "",
};

// TS 103 461 table E.1: Band III frequency blocks (centre in kHz)
struct Block { const char* name; int khz; };
inline constexpr Block BAND3[38] = {
    {"5A", 174928},
    {"5B", 176640},
    {"5C", 178352},
    {"5D", 180064},
    {"6A", 181936},
    {"6B", 183648},
    {"6C", 185360},
    {"6D", 187072},
    {"7A", 188928},
    {"7B", 190640},
    {"7C", 192352},
    {"7D", 194064},
    {"8A", 195936},
    {"8B", 197648},
    {"8C", 199360},
    {"8D", 201072},
    {"9A", 202928},
    {"9B", 204640},
    {"9C", 206352},
    {"9D", 208064},
    {"10A", 209936},
    {"10B", 211648},
    {"10C", 213360},
    {"10D", 215072},
    {"11A", 216928},
    {"11B", 218640},
    {"11C", 220352},
    {"11D", 222064},
    {"12A", 223936},
    {"12B", 225648},
    {"12C", 227360},
    {"12D", 229072},
    {"13A", 230784},
    {"13B", 232496},
    {"13C", 234208},
    {"13D", 235776},
    {"13E", 237488},
    {"13F", 239200},
};

}   // namespace dab::gen
