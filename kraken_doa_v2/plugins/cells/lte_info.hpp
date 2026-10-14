#pragma once

// Reference data for showing what the cells broadcast:
//   LTE operating bands (3GPP TS 36.101 Table 5.7.3-1: EARFCN <-> frequency)
//   NR operating bands (38.104 / 38.101-1, downlink ranges)
//   MCC -> country (ITU-T E.212)

#include <cmath>
#include <string>
#include <vector>

namespace cells {

struct LteBand {
    int band;
    double dl_low_mhz;   // F_DL_low
    int dl_offs;         // N_Offs-DL
    int dl_count;        // EARFCNs in the band (N_DL range = dl_offs .. dl_offs + dl_count - 1)
    double ul_low_mhz;   // F_UL_low (0 = TDD / downlink only: the UL is the DL)
    int ul_offs;         // N_Offs-UL
};

inline const std::vector<LteBand>& lte_bands() {
    static const std::vector<LteBand> b = {
        {1, 2110, 0, 600, 1920, 18000},        {2, 1930, 600, 600, 1850, 18600},       {3, 1805, 1200, 750, 1710, 19200},
        {4, 2110, 1950, 450, 1710, 19950},     {5, 869, 2400, 250, 824, 20400},        {6, 875, 2650, 100, 830, 20650},
        {7, 2620, 2750, 700, 2500, 20750},     {8, 925, 3450, 350, 880, 21450},        {9, 1844.9, 3800, 350, 1749.9, 21800},
        {10, 2110, 4150, 600, 1710, 22150},    {11, 1475.9, 4750, 200, 1427.9, 22750}, {12, 729, 5010, 170, 699, 23010},
        {13, 746, 5180, 100, 777, 23180},      {14, 758, 5280, 100, 788, 23280},       {17, 734, 5730, 120, 704, 23730},
        {18, 860, 5850, 150, 815, 23850},      {19, 875, 6000, 150, 830, 24000},       {20, 791, 6150, 300, 832, 24150},
        {21, 1495.9, 6450, 150, 1447.9, 24450}, {22, 3510, 6600, 800, 3410, 24600},    {23, 2180, 7500, 200, 2000, 25500},
        {24, 1525, 7700, 340, 1626.5, 25700},  {25, 1930, 8040, 650, 1850, 26040},     {26, 859, 8690, 350, 814, 26690},
        {27, 852, 9040, 170, 807, 27040},      {28, 758, 9210, 450, 703, 27210},       {29, 717, 9660, 110, 0, 0},
        {30, 2350, 9770, 100, 2305, 27660},    {31, 462.5, 9870, 50, 452.5, 27760},    {32, 1452, 9920, 440, 0, 0},
        {33, 1900, 36000, 200, 0, 0},          {34, 2010, 36200, 150, 0, 0},           {35, 1850, 36350, 600, 0, 0},
        {36, 1930, 36950, 600, 0, 0},          {37, 1910, 37550, 200, 0, 0},           {38, 2570, 37750, 500, 0, 0},
        {39, 1880, 38250, 400, 0, 0},          {40, 2300, 38650, 1000, 0, 0},          {41, 2496, 39650, 1940, 0, 0},
        {42, 3400, 41590, 2000, 0, 0},         {43, 3600, 43590, 2000, 0, 0},          {44, 703, 45590, 1000, 0, 0},
        {45, 1447, 46590, 200, 0, 0},          {46, 5150, 46790, 7750, 0, 0},          {47, 5855, 54540, 700, 0, 0},
        {48, 3550, 55240, 1500, 0, 0},         {50, 1432, 58240, 850, 0, 0},           {51, 1427, 59090, 50, 0, 0},
        {52, 3300, 59140, 1000, 0, 0},         {53, 2483.5, 60140, 115, 0, 0},         {65, 2110, 65536, 900, 1920, 131072},
        {66, 2110, 66436, 900, 1710, 131972},  {67, 738, 67336, 200, 0, 0},            {68, 753, 67536, 300, 698, 132672},
        {69, 2570, 67836, 500, 0, 0},          {70, 1995, 68336, 250, 1695, 132972},   {71, 617, 68586, 350, 663, 133122},
        {72, 461, 68936, 50, 451, 133472},     {73, 460, 68986, 50, 450, 133522},      {74, 1475, 69036, 430, 1427, 133572},
        {75, 1432, 69466, 850, 0, 0},          {76, 1427, 70316, 50, 0, 0},            {85, 728, 70366, 180, 698, 134002},
        {87, 420, 70546, 50, 410, 134182},     {88, 422, 70596, 50, 412, 134232},
    };
    return b;
}

inline const LteBand* lte_band(int band) {
    for (const auto& b : lte_bands())
        if (b.band == band) return &b;
    return nullptr;
}

// downlink EARFCN of a frequency in a band (-1 = not in it / not on the 100 kHz raster)
inline int lte_earfcn(const LteBand& b, double f_hz) {
    const double n = b.dl_offs + (f_hz / 1e6 - b.dl_low_mhz) * 10;
    const long r = std::lround(n);
    if (std::fabs(n - static_cast<double>(r)) > 0.05 || r < b.dl_offs || r >= b.dl_offs + b.dl_count) return -1;
    return static_cast<int>(r);
}

// the bands whose downlink holds f
inline std::vector<int> lte_bands_at(double f_hz) {
    std::vector<int> v;
    for (const auto& b : lte_bands())
        if (lte_earfcn(b, f_hz) >= 0) v.push_back(b.band);
    return v;
}

// a downlink / uplink EARFCN -> MHz (0 = unknown)
inline double lte_dl_mhz(int n) {
    for (const auto& b : lte_bands())
        if (n >= b.dl_offs && n < b.dl_offs + b.dl_count) return b.dl_low_mhz + 0.1 * (n - b.dl_offs);
    return 0;
}
inline double lte_ul_mhz(int n) {
    for (const auto& b : lte_bands())
        if (b.ul_low_mhz > 0 && n >= b.ul_offs && n < b.ul_offs + b.dl_count) return b.ul_low_mhz + 0.1 * (n - b.ul_offs);
    return 0;
}

struct NrBand {
    int band;
    double dl_lo_mhz, dl_hi_mhz;
};

inline const std::vector<NrBand>& nr_bands() {
    static const std::vector<NrBand> b = {
        {1, 2110, 2170},   {2, 1930, 1990},   {3, 1805, 1880},   {5, 869, 894},     {7, 2620, 2690},   {8, 925, 960},
        {12, 729, 746},    {13, 746, 756},    {14, 758, 768},    {18, 860, 875},    {20, 791, 821},    {24, 1525, 1559},
        {25, 1930, 1995},  {26, 859, 894},    {28, 758, 803},    {29, 717, 728},    {30, 2350, 2360},  {34, 2010, 2025},
        {38, 2570, 2620},  {39, 1880, 1920},  {40, 2300, 2400},  {41, 2496, 2690},  {46, 5150, 5925},  {48, 3550, 3700},
        {50, 1432, 1517},  {51, 1427, 1432},  {53, 2483.5, 2495}, {65, 2110, 2200}, {66, 2110, 2200},  {67, 738, 758},
        {70, 1995, 2020},  {71, 617, 652},    {74, 1475, 1518},  {75, 1432, 1517},  {76, 1427, 1432},  {77, 3300, 4200},
        {78, 3300, 3800},  {79, 4400, 5000},  {85, 728, 746},    {90, 2496, 2690},  {91, 1427, 1432},  {92, 1432, 1517},
        {93, 1427, 1432},  {94, 1432, 1517},  {96, 5925, 7125},  {100, 919.4, 925}, {101, 1900, 1910}, {104, 6425, 7125},
        {105, 612, 652},
    };
    return b;
}

inline std::vector<int> nr_bands_at(double f_hz) {
    std::vector<int> v;
    for (const auto& b : nr_bands())
        if (f_hz >= b.dl_lo_mhz * 1e6 && f_hz <= b.dl_hi_mhz * 1e6) v.push_back(b.band);
    return v;
}

// NR-ARFCN of a frequency (38.104 5.4.2.1, global raster)
inline long nr_arfcn(double f_hz) {
    const double f = f_hz / 1e3;   // kHz
    if (f < 3e6) return std::lround(f / 5);
    if (f < 24250080) return 600000 + std::lround((f - 3e6) / 15);
    return 2016667 + std::lround((f - 24250080) / 60);
}

// MCC -> country (ITU-T E.212); "" if unknown
inline std::string mcc_country(int mcc) {
    static const struct {
        short mcc;
        const char* name;
    } T[] = {
        {202, "Greece"}, {204, "Netherlands"}, {206, "Belgium"}, {208, "France"}, {212, "Monaco"}, {213, "Andorra"}, {214, "Spain"},
        {216, "Hungary"}, {218, "Bosnia and Herzegovina"}, {219, "Croatia"}, {220, "Serbia"}, {221, "Kosovo"}, {222, "Italy"},
        {225, "Vatican"}, {226, "Romania"}, {228, "Switzerland"}, {230, "Czechia"}, {231, "Slovakia"}, {232, "Austria"},
        {234, "United Kingdom"}, {235, "United Kingdom"}, {238, "Denmark"}, {240, "Sweden"}, {242, "Norway"}, {244, "Finland"},
        {246, "Lithuania"}, {247, "Latvia"}, {248, "Estonia"}, {250, "Russia"}, {255, "Ukraine"}, {257, "Belarus"}, {259, "Moldova"},
        {260, "Poland"}, {262, "Germany"}, {266, "Gibraltar"}, {268, "Portugal"}, {270, "Luxembourg"}, {272, "Ireland"}, {274, "Iceland"},
        {276, "Albania"}, {278, "Malta"}, {280, "Cyprus"}, {282, "Georgia"}, {283, "Armenia"}, {284, "Bulgaria"}, {286, "Turkey"},
        {288, "Faroe Islands"}, {290, "Greenland"}, {292, "San Marino"}, {293, "Slovenia"}, {294, "North Macedonia"}, {295, "Liechtenstein"},
        {297, "Montenegro"}, {302, "Canada"}, {308, "Saint Pierre and Miquelon"}, {310, "United States"}, {311, "United States"},
        {312, "United States"}, {313, "United States"}, {314, "United States"}, {315, "United States"}, {316, "United States"},
        {330, "Puerto Rico"}, {334, "Mexico"}, {338, "Jamaica"}, {340, "French Antilles"}, {342, "Barbados"}, {344, "Antigua and Barbuda"},
        {346, "Cayman Islands"}, {348, "British Virgin Islands"}, {350, "Bermuda"}, {352, "Grenada"}, {354, "Montserrat"},
        {356, "Saint Kitts and Nevis"}, {358, "Saint Lucia"}, {360, "Saint Vincent and the Grenadines"}, {362, "Curacao / Bonaire"},
        {363, "Aruba"}, {364, "Bahamas"}, {365, "Anguilla"}, {366, "Dominica"}, {368, "Cuba"}, {370, "Dominican Republic"}, {372, "Haiti"},
        {374, "Trinidad and Tobago"}, {376, "Turks and Caicos Islands"}, {400, "Azerbaijan"}, {401, "Kazakhstan"}, {402, "Bhutan"},
        {404, "India"}, {405, "India"}, {406, "India"}, {410, "Pakistan"}, {412, "Afghanistan"}, {413, "Sri Lanka"}, {414, "Myanmar"},
        {415, "Lebanon"}, {416, "Jordan"}, {417, "Syria"}, {418, "Iraq"}, {419, "Kuwait"}, {420, "Saudi Arabia"}, {421, "Yemen"},
        {422, "Oman"}, {424, "United Arab Emirates"}, {425, "Israel / Palestine"}, {426, "Bahrain"}, {427, "Qatar"}, {428, "Mongolia"},
        {429, "Nepal"}, {430, "United Arab Emirates"}, {431, "United Arab Emirates"}, {432, "Iran"}, {434, "Uzbekistan"}, {436, "Tajikistan"},
        {437, "Kyrgyzstan"}, {438, "Turkmenistan"}, {440, "Japan"}, {441, "Japan"}, {450, "South Korea"}, {452, "Vietnam"},
        {454, "Hong Kong"}, {455, "Macau"}, {456, "Cambodia"}, {457, "Laos"}, {460, "China"}, {461, "China"}, {466, "Taiwan"},
        {467, "North Korea"}, {470, "Bangladesh"}, {472, "Maldives"}, {502, "Malaysia"}, {505, "Australia"}, {510, "Indonesia"},
        {514, "Timor-Leste"}, {515, "Philippines"}, {520, "Thailand"}, {525, "Singapore"}, {528, "Brunei"}, {530, "New Zealand"},
        {536, "Nauru"}, {537, "Papua New Guinea"}, {539, "Tonga"}, {540, "Solomon Islands"}, {541, "Vanuatu"}, {542, "Fiji"},
        {543, "Wallis and Futuna"}, {544, "American Samoa"}, {545, "Kiribati"}, {546, "New Caledonia"}, {547, "French Polynesia"},
        {548, "Cook Islands"}, {549, "Samoa"}, {550, "Micronesia"}, {551, "Marshall Islands"}, {552, "Palau"}, {553, "Tuvalu"},
        {554, "Tokelau"}, {555, "Niue"}, {602, "Egypt"}, {603, "Algeria"}, {604, "Morocco"}, {605, "Tunisia"}, {606, "Libya"},
        {607, "Gambia"}, {608, "Senegal"}, {609, "Mauritania"}, {610, "Mali"}, {611, "Guinea"}, {612, "Cote d'Ivoire"},
        {613, "Burkina Faso"}, {614, "Niger"}, {615, "Togo"}, {616, "Benin"}, {617, "Mauritius"}, {618, "Liberia"}, {619, "Sierra Leone"},
        {620, "Ghana"}, {621, "Nigeria"}, {622, "Chad"}, {623, "Central African Republic"}, {624, "Cameroon"}, {625, "Cape Verde"},
        {626, "Sao Tome and Principe"}, {627, "Equatorial Guinea"}, {628, "Gabon"}, {629, "Congo"}, {630, "DR Congo"}, {631, "Angola"},
        {632, "Guinea-Bissau"}, {633, "Seychelles"}, {634, "Sudan"}, {635, "Rwanda"}, {636, "Ethiopia"}, {637, "Somalia"},
        {638, "Djibouti"}, {639, "Kenya"}, {640, "Tanzania"}, {641, "Uganda"}, {642, "Burundi"}, {643, "Mozambique"}, {645, "Zambia"},
        {646, "Madagascar"}, {647, "Reunion / Mayotte"}, {648, "Zimbabwe"}, {649, "Namibia"}, {650, "Malawi"}, {651, "Lesotho"},
        {652, "Botswana"}, {653, "Eswatini"}, {654, "Comoros"}, {655, "South Africa"}, {657, "Eritrea"}, {658, "Saint Helena"},
        {659, "South Sudan"}, {702, "Belize"}, {704, "Guatemala"}, {706, "El Salvador"}, {708, "Honduras"}, {710, "Nicaragua"},
        {712, "Costa Rica"}, {714, "Panama"}, {716, "Peru"}, {722, "Argentina"}, {724, "Brazil"}, {730, "Chile"}, {732, "Colombia"},
        {734, "Venezuela"}, {736, "Bolivia"}, {738, "Guyana"}, {740, "Ecuador"}, {742, "French Guiana"}, {744, "Paraguay"},
        {746, "Suriname"}, {748, "Uruguay"}, {750, "Falkland Islands"}, {901, "International"},
    };
    for (const auto& t : T)
        if (t.mcc == mcc) return t.name;
    return "";
}

}  // namespace cells
