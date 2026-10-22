#pragma once
// RDS / RBDS lookup tables: the RDS character set (EN 50067:1998 Annex E =
// IEC 62106 Annex E, code table G0), programme type names for RDS (Annex F)
// and RBDS (NRSC-4-B), the ECC + PI country table (Annex D, RDS Forum
// R08/008_7), programme languages (Annex J), Open Data Application names
// (RDS Forum ODA list), RadioText+ content types (RDS Forum R06/040_1) and
// the North American call-sign rules (NRSC-4-B Annex D).
//
// The table data follow the compilation in redsea by Oona Raisanen
// (https://github.com/windytan/redsea, src/tables.cc and
// src/text/rdsstring.cc, ISC licence):
//   Copyright (c) Oona Raisanen
//   Permission to use, copy, modify, and/or distribute this software for any
//   purpose with or without fee is hereby granted, provided that the above
//   copyright notice and this permission notice appear in all copies.
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
//   WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
//   MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
//   ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
//   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
//   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
// The code below is this plugin's own.

#include <cstdint>
#include <string>

namespace rds {

// One RDS character (8-bit code) as UTF-8. Control codes 0x00-0x1F come out
// as "" except the ones a text uses (the callers handle 0x0D / 0x0A first)
inline const char* rds_char(uint8_t c) {
    static const char* const G0[256] = {
        "", "", "", "", "", "", "", "", "", "", "\n", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "\xC2\xAD",
        " ", "!", "\"", "#", "\xC2\xA4", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
        "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "\xE2\x80\x95", "_",
        "\xE2\x80\x96", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "\xC2\xAF", " ",
        "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "β", "¡", "Ĳ",
        "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ǧ", "ı", "ĳ",
        "ª", "α", "©", "‰", "Ǧ", "ě", "ň", "ő", "π", "€", "£", "$", "←", "↑", "→", "↓",
        "º", "¹", "²", "³", "±", "İ", "ń", "ű", "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
        "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
        "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
        "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
        "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", " "};
    return G0[c];
}

inline const char* pty_name(int pty, bool rbds) {
    static const char* const RDS[32] = {
        "No PTY", "News", "Current affairs", "Information", "Sport", "Education", "Drama", "Culture",
        "Science", "Varied", "Pop music", "Rock music", "Easy listening", "Light classical",
        "Serious classical", "Other music", "Weather", "Finance", "Children's programmes",
        "Social affairs", "Religion", "Phone-in", "Travel", "Leisure", "Jazz music", "Country music",
        "National music", "Oldies music", "Folk music", "Documentary", "Alarm test", "Alarm"};
    static const char* const RBDS[32] = {
        "No PTY", "News", "Information", "Sports", "Talk", "Rock", "Classic rock", "Adult hits",
        "Soft rock", "Top 40", "Country", "Oldies", "Soft", "Nostalgia", "Jazz", "Classical",
        "Rhythm and blues", "Soft rhythm and blues", "Language", "Religious music", "Religious talk",
        "Personality", "Public", "College", "Spanish talk", "Spanish music", "Hip hop", "(unassigned)",
        "(unassigned)", "Weather", "Emergency test", "Emergency"};
    if (pty < 0 || pty > 31) return "?";
    return rbds ? RBDS[pty] : RDS[pty];
}

// Two-letter country code from the PI's first nibble (1..15) and the ECC
// ("" = unknown / not allocated)
inline const char* country_code(int cc, int ecc) {
    struct Row { uint8_t ecc; const char* c[15]; };
    static const Row T[] = {
        {0xA0, {"us", "us", "us", "us", "us", "us", "us", "us", "us", "us", "us", "", "us", "us", ""}},
        {0xA1, {"", "", "", "", "", "", "", "", "", "", "ca", "ca", "ca", "ca", "gl"}},
        {0xA2, {"ai", "ag", "ec", "fk", "bb", "bz", "ky", "cr", "cu", "ar", "br", "bm", "an", "gp", "bs"}},
        {0xA3, {"bo", "co", "jm", "mq", "gf", "py", "ni", "", "pa", "dm", "do", "cl", "gd", "tc", "gy"}},
        {0xA4, {"gt", "hn", "aw", "", "ms", "tt", "pe", "sr", "uy", "kn", "lc", "sv", "ht", "ve", ""}},
        {0xA5, {"", "", "", "", "", "", "", "", "", "", "mx", "vc", "mx", "mx", "mx"}},
        {0xA6, {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "pm"}},
        {0xD0, {"cm", "cf", "dj", "mg", "ml", "ao", "gq", "ga", "gn", "za", "bf", "cg", "tg", "bj", "mw"}},
        {0xD1, {"na", "lr", "gh", "mr", "st", "cv", "sn", "gm", "bi", "", "bw", "km", "tz", "et", "bg"}},
        {0xD2, {"sl", "zw", "mz", "ug", "sz", "ke", "so", "ne", "td", "gw", "zr", "ci", "tz", "zm", ""}},
        {0xD3, {"", "", "eh", "", "rw", "ls", "", "sc", "", "mu", "", "sd", "", "", ""}},
        {0xE0, {"de", "dz", "ad", "il", "it", "be", "ru", "ps", "al", "at", "hu", "mt", "de", "", "eg"}},
        {0xE1, {"gr", "cy", "sm", "ch", "jo", "fi", "lu", "bg", "dk", "gi", "iq", "gb", "ly", "ro", "fr"}},
        {0xE2, {"ma", "cz", "pl", "va", "sk", "sy", "tn", "", "li", "is", "mc", "lt", "rs", "es", "no"}},
        {0xE3, {"me", "ie", "tr", "mk", "", "", "", "nl", "lv", "lb", "az", "hr", "kz", "se", "by"}},
        {0xE4, {"md", "ee", "kg", "", "", "ua", "ks", "pt", "si", "am", "", "ge", "", "", "ba"}},
        {0xF0, {"au", "au", "au", "au", "au", "au", "au", "au", "sa", "af", "mm", "cn", "kp", "bh", "my"}},
        {0xF1, {"ki", "bt", "bd", "pk", "fj", "om", "nr", "ir", "nz", "sb", "bn", "lk", "tw", "kr", "hk"}},
        {0xF2, {"kw", "qa", "kh", "ws", "in", "mo", "vn", "ph", "jp", "sg", "mv", "id", "ae", "np", "vu"}},
        {0xF3, {"la", "th", "to", "", "", "", "", "", "pg", "", "ye", "", "", "fm", "mn"}}};
    if (cc < 1 || cc > 15) return "";
    for (const auto& r : T)
        if (r.ecc == ecc) return r.c[cc - 1];
    return "";
}

// Country name for a two-letter code the table above gives (English)
inline const char* country_name(const std::string& c) {
    struct N { const char* c; const char* n; };
    static const N T[] = {
        {"us", "United States"}, {"ca", "Canada"}, {"gl", "Greenland"}, {"ai", "Anguilla"},
        {"ag", "Antigua and Barbuda"}, {"ec", "Ecuador"}, {"fk", "Falkland Islands"}, {"bb", "Barbados"},
        {"bz", "Belize"}, {"ky", "Cayman Islands"}, {"cr", "Costa Rica"}, {"cu", "Cuba"},
        {"ar", "Argentina"}, {"br", "Brazil"}, {"bm", "Bermuda"}, {"an", "Netherlands Antilles"},
        {"gp", "Guadeloupe"}, {"bs", "Bahamas"}, {"bo", "Bolivia"}, {"co", "Colombia"}, {"jm", "Jamaica"},
        {"mq", "Martinique"}, {"gf", "French Guiana"}, {"py", "Paraguay"}, {"ni", "Nicaragua"},
        {"pa", "Panama"}, {"dm", "Dominica"}, {"do", "Dominican Republic"}, {"cl", "Chile"},
        {"gd", "Grenada"}, {"tc", "Turks and Caicos"}, {"gy", "Guyana"}, {"gt", "Guatemala"},
        {"hn", "Honduras"}, {"aw", "Aruba"}, {"ms", "Montserrat"}, {"tt", "Trinidad and Tobago"},
        {"pe", "Peru"}, {"sr", "Suriname"}, {"uy", "Uruguay"}, {"kn", "Saint Kitts and Nevis"},
        {"lc", "Saint Lucia"}, {"sv", "El Salvador"}, {"ht", "Haiti"}, {"ve", "Venezuela"},
        {"mx", "Mexico"}, {"vc", "Saint Vincent"}, {"pm", "Saint Pierre and Miquelon"},
        {"cm", "Cameroon"}, {"cf", "Central African Republic"}, {"dj", "Djibouti"}, {"mg", "Madagascar"},
        {"ml", "Mali"}, {"ao", "Angola"}, {"gq", "Equatorial Guinea"}, {"ga", "Gabon"}, {"gn", "Guinea"},
        {"za", "South Africa"}, {"bf", "Burkina Faso"}, {"cg", "Congo"}, {"tg", "Togo"}, {"bj", "Benin"},
        {"mw", "Malawi"}, {"na", "Namibia"}, {"lr", "Liberia"}, {"gh", "Ghana"}, {"mr", "Mauritania"},
        {"st", "Sao Tome and Principe"}, {"cv", "Cape Verde"}, {"sn", "Senegal"}, {"gm", "Gambia"},
        {"bi", "Burundi"}, {"bw", "Botswana"}, {"km", "Comoros"}, {"tz", "Tanzania"}, {"et", "Ethiopia"},
        {"bg", "Bulgaria"}, {"sl", "Sierra Leone"}, {"zw", "Zimbabwe"}, {"mz", "Mozambique"},
        {"ug", "Uganda"}, {"sz", "Eswatini"}, {"ke", "Kenya"}, {"so", "Somalia"}, {"ne", "Niger"},
        {"td", "Chad"}, {"gw", "Guinea-Bissau"}, {"zr", "DR Congo"}, {"ci", "Ivory Coast"}, {"zm", "Zambia"},
        {"eh", "Western Sahara"}, {"rw", "Rwanda"}, {"ls", "Lesotho"}, {"sc", "Seychelles"},
        {"mu", "Mauritius"}, {"sd", "Sudan"}, {"de", "Germany"}, {"dz", "Algeria"}, {"ad", "Andorra"},
        {"il", "Israel"}, {"it", "Italy"}, {"be", "Belgium"}, {"ru", "Russia"}, {"ps", "Palestine"},
        {"al", "Albania"}, {"at", "Austria"}, {"hu", "Hungary"}, {"mt", "Malta"}, {"eg", "Egypt"},
        {"gr", "Greece"}, {"cy", "Cyprus"}, {"sm", "San Marino"}, {"ch", "Switzerland"}, {"jo", "Jordan"},
        {"fi", "Finland"}, {"lu", "Luxembourg"}, {"dk", "Denmark"}, {"gi", "Gibraltar"}, {"iq", "Iraq"},
        {"gb", "United Kingdom"}, {"ly", "Libya"}, {"ro", "Romania"}, {"fr", "France"}, {"ma", "Morocco"},
        {"cz", "Czechia"}, {"pl", "Poland"}, {"va", "Vatican"}, {"sk", "Slovakia"}, {"sy", "Syria"},
        {"tn", "Tunisia"}, {"li", "Liechtenstein"}, {"is", "Iceland"}, {"mc", "Monaco"},
        {"lt", "Lithuania"}, {"rs", "Serbia"}, {"es", "Spain"}, {"no", "Norway"}, {"me", "Montenegro"},
        {"ie", "Ireland"}, {"tr", "Turkey"}, {"mk", "North Macedonia"}, {"nl", "Netherlands"},
        {"lv", "Latvia"}, {"lb", "Lebanon"}, {"az", "Azerbaijan"}, {"hr", "Croatia"}, {"kz", "Kazakhstan"},
        {"se", "Sweden"}, {"by", "Belarus"}, {"md", "Moldova"}, {"ee", "Estonia"}, {"kg", "Kyrgyzstan"},
        {"ua", "Ukraine"}, {"ks", "Kosovo"}, {"pt", "Portugal"}, {"si", "Slovenia"}, {"am", "Armenia"},
        {"ge", "Georgia"}, {"ba", "Bosnia and Herzegovina"}, {"au", "Australia"}, {"sa", "Saudi Arabia"},
        {"af", "Afghanistan"}, {"mm", "Myanmar"}, {"cn", "China"}, {"kp", "North Korea"}, {"bh", "Bahrain"},
        {"my", "Malaysia"}, {"ki", "Kiribati"}, {"bt", "Bhutan"}, {"bd", "Bangladesh"}, {"pk", "Pakistan"},
        {"fj", "Fiji"}, {"om", "Oman"}, {"nr", "Nauru"}, {"ir", "Iran"}, {"nz", "New Zealand"},
        {"sb", "Solomon Islands"}, {"bn", "Brunei"}, {"lk", "Sri Lanka"}, {"tw", "Taiwan"},
        {"kr", "South Korea"}, {"hk", "Hong Kong"}, {"kw", "Kuwait"}, {"qa", "Qatar"}, {"kh", "Cambodia"},
        {"ws", "Samoa"}, {"in", "India"}, {"mo", "Macao"}, {"vn", "Vietnam"}, {"ph", "Philippines"},
        {"jp", "Japan"}, {"sg", "Singapore"}, {"mv", "Maldives"}, {"id", "Indonesia"},
        {"ae", "United Arab Emirates"}, {"np", "Nepal"}, {"vu", "Vanuatu"}, {"la", "Laos"},
        {"th", "Thailand"}, {"to", "Tonga"}, {"pg", "Papua New Guinea"}, {"ye", "Yemen"},
        {"fm", "Micronesia"}, {"mn", "Mongolia"}};
    for (const auto& e : T)
        if (c == e.c) return e.n;
    return "";
}

inline const char* language_name(int code) {
    static const char* const L[128] = {
        "Unknown", "Albanian", "Breton", "Catalan", "Croatian", "Welsh", "Czech", "Danish", "German",
        "English", "Spanish", "Esperanto", "Estonian", "Basque", "Faroese", "French", "Frisian", "Irish",
        "Gaelic", "Galician", "Icelandic", "Italian", "Lappish", "Latin", "Latvian", "Luxembourgian",
        "Lithuanian", "Hungarian", "Maltese", "Dutch", "Norwegian", "Occitan", "Polish", "Portuguese",
        "Romanian", "Romansh", "Serbian", "Slovak", "Slovene", "Finnish", "Swedish", "Turkish", "Flemish",
        "Walloon", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "",
        "Background", "", "", "", "", "Zulu", "Vietnamese", "Uzbek", "Urdu", "Ukrainian", "Thai",
        "Telugu", "Tatar", "Tamil", "Tadzhik", "Swahili", "Sranan Tongo", "Somali", "Sinhalese", "Shona",
        "Serbo-Croat", "Ruthenian", "Russian", "Quechua", "Pushtu", "Punjabi", "Persian", "Papamiento",
        "Oriya", "Nepali", "Ndebele", "Marathi", "Moldovian", "Malaysian", "Malagasay", "Macedonian",
        "Laotian", "Korean", "Khmer", "Kazakh", "Kannada", "Japanese", "Indonesian", "Hindi", "Hebrew",
        "Hausa", "Gurani", "Gujurati", "Greek", "Georgian", "Fulani", "Dari", "Churash", "Chinese",
        "Burmese", "Bulgarian", "Bengali", "Belorussian", "Bambora", "Azerbaijan", "Assamese", "Armenian",
        "Arabic", "Amharic"};
    return code >= 0 && code < 128 ? L[code] : "";
}

// Open Data Application, by its AID (group 3A)
inline const char* oda_name(uint16_t aid) {
    struct A { uint16_t aid; const char* n; };
    static const A T[] = {
        {0x0000, "None"}, {0x0093, "Cross referencing DAB within RDS"},
        {0x0BCB, "Leisure & Practical Info for Drivers"}, {0x0C24, "ELECTRABEL-DSM 7"},
        {0x0CC1, "Wireless Playground broadcast control signal"}, {0x0D45, "RDS-TMC: ALERT-C / EN ISO 14819-1"},
        {0x0D8B, "ELECTRABEL-DSM 18"}, {0x0E2C, "ELECTRABEL-DSM 3"}, {0x0E31, "ELECTRABEL-DSM 13"},
        {0x0F87, "ELECTRABEL-DSM 2"}, {0x125F, "I-FM-RDS for fixed and mobile devices"},
        {0x1BDA, "ELECTRABEL-DSM 1"}, {0x1C5E, "ELECTRABEL-DSM 20"}, {0x1C68, "ITIS In-vehicle data base"},
        {0x1CB1, "ELECTRABEL-DSM 10"}, {0x1D47, "ELECTRABEL-DSM 4"}, {0x1DC2, "CITIBUS 4"},
        {0x1DC5, "Encrypted TTI using ALERT-Plus"}, {0x1E8F, "ELECTRABEL-DSM 17"}, {0x4400, "RDS-Light"},
        {0x4AA1, "RASANT"}, {0x4AB7, "ELECTRABEL-DSM 9"}, {0x4BA2, "ELECTRABEL-DSM 5"},
        {0x4BD7, "RadioText+ (RT+)"}, {0x4BD8, "RadioText Plus for eRT (eRT+)"}, {0x4C59, "CITIBUS 2"},
        {0x4D87, "Radio Commerce System (RCS)"}, {0x4D95, "ELECTRABEL-DSM 16"}, {0x4D9A, "ELECTRABEL-DSM 11"},
        {0x50DD, "Disaster / emergency warning"}, {0x5757, "Personal weather station"},
        {0x6363, "Hybradio RDS-Net"}, {0x6365, "RDS2 9-bit AF lists"}, {0x6552, "Enhanced RadioText (eRT)"},
        {0x6A7A, "Warning receiver"}, {0x7373, "Enhanced early warning system"}, {0xA112, "NL Alert system"},
        {0xA911, "Data FM Selective Multipoint Messaging"}, {0xABCF, "RF Power Monitoring"},
        {0xC350, "NRSC Song Title and Artist"}, {0xC3A1, "Personal Radio Service"},
        {0xC3B0, "iTunes Tagging"}, {0xC3C3, "NAVTEQ Traffic Plus"}, {0xC4D4, "eEAS"},
        {0xC549, "Smart Grid Broadcast Channel"}, {0xC563, "ID Logic"},
        {0xC6A7, "Veil Enabled Interactive Device"}, {0xC737, "Utility Message Channel (UMC)"},
        {0xCB73, "CITIBUS 1"}, {0xCB97, "ELECTRABEL-DSM 14"}, {0xCC21, "CITIBUS 3"},
        {0xCD46, "RDS-TMC: ALERT-C"}, {0xCD47, "RDS-TMC: ALERT-C"}, {0xCD9E, "ELECTRABEL-DSM 8"},
        {0xCE6B, "Encrypted TTI using ALERT-Plus"}, {0xE123, "APS Gateway"}, {0xE1C1, "Action code"},
        {0xE319, "ELECTRABEL-DSM 12"}, {0xE411, "Beacon downlink"}, {0xE440, "ELECTRABEL-DSM 15"},
        {0xE4A6, "ELECTRABEL-DSM 19"}, {0xE5D7, "ELECTRABEL-DSM 6"}, {0xE911, "EAS open protocol"},
        {0xFF7F, "RFT: Station logo"}, {0xFF80, "RFT+ (work title)"}};
    for (const auto& e : T)
        if (e.aid == aid) return e.n;
    return "";
}

inline const char* rtplus_type(int t) {
    static const char* const T[64] = {
        "dummy", "item.title", "item.album", "item.tracknumber", "item.artist", "item.composition",
        "item.movement", "item.conductor", "item.composer", "item.band", "item.comment", "item.genre",
        "info.news", "info.news.local", "info.stockmarket", "info.sport", "info.lottery", "info.horoscope",
        "info.daily_diversion", "info.health", "info.event", "info.scene", "info.cinema", "info.tv",
        "info.date_time", "info.weather", "info.traffic", "info.alarm", "info.advertisement", "info.url",
        "info.other", "stationname.short", "stationname.long", "programme.now", "programme.next",
        "programme.part", "programme.host", "programme.editorial_staff", "programme.frequency",
        "programme.homepage", "programme.subchannel", "phone.hotline", "phone.studio", "phone.other",
        "sms.studio", "sms.other", "email.hotline", "email.studio", "email.other", "mms.other", "chat",
        "chat.centre", "vote.question", "vote.centre", "rfu", "rfu", "rfu", "rfu", "rfu", "place",
        "appointment", "identifier", "purchase", "get_data"};
    return t >= 0 && t < 64 ? T[t] : "?";
}

// Call sign of a North American (RBDS) station from its PI (NRSC-4-B Annex
// D): K / W + three letters, the three-letter calls, nationally linked
// networks. "" when the PI isn't a call-sign PI
inline std::string rbds_callsign(uint16_t pi) {
    struct C { uint16_t pi; const char* c; };
    static const C THREE[] = {
        {0x99A5, "KBW"}, {0x9992, "KOY"}, {0x9978, "WHO"}, {0x99A6, "KCY"}, {0x9993, "KPQ"}, {0x999C, "WHP"},
        {0x9990, "KDB"}, {0x9964, "KQV"}, {0x999D, "WIL"}, {0x99A7, "KDF"}, {0x9994, "KSD"}, {0x997A, "WIP"},
        {0x9950, "KEX"}, {0x9965, "KSL"}, {0x99B3, "WIS"}, {0x9951, "KFH"}, {0x9966, "KUJ"}, {0x997B, "WJR"},
        {0x9952, "KFI"}, {0x9995, "KUT"}, {0x99B4, "WJW"}, {0x9953, "KGA"}, {0x9967, "KVI"}, {0x99B5, "WJZ"},
        {0x9991, "KGB"}, {0x9968, "KWG"}, {0x997C, "WKY"}, {0x9954, "KGO"}, {0x9996, "KXL"}, {0x997D, "WLS"},
        {0x9955, "KGU"}, {0x9997, "KXO"}, {0x997E, "WLW"}, {0x9956, "KGW"}, {0x996B, "KYW"}, {0x999E, "WMC"},
        {0x9957, "KGY"}, {0x9999, "WBT"}, {0x999F, "WMT"}, {0x99AA, "KHQ"}, {0x996D, "WBZ"}, {0x9981, "WOC"},
        {0x9958, "KID"}, {0x996E, "WDZ"}, {0x99A0, "WOI"}, {0x9959, "KIT"}, {0x996F, "WEW"}, {0x9983, "WOL"},
        {0x995A, "KJR"}, {0x999A, "WGH"}, {0x9984, "WOR"}, {0x995B, "KLO"}, {0x9971, "WGL"}, {0x99A1, "WOW"},
        {0x995C, "KLZ"}, {0x9972, "WGN"}, {0x99B9, "WRC"}, {0x995D, "KMA"}, {0x9973, "WGR"}, {0x99A2, "WRR"},
        {0x995E, "KMJ"}, {0x999B, "WGY"}, {0x99A3, "WSB"}, {0x995F, "KNX"}, {0x9975, "WHA"}, {0x99A4, "WSM"},
        {0x9960, "KOA"}, {0x9976, "WHB"}, {0x9988, "WWJ"}, {0x99AB, "KOB"}, {0x9977, "WHK"}, {0x9989, "WWL"}};
    struct L { uint16_t pi; const char* n; };
    static const L LINKED[] = {
        {0xB001, "NPR-1"}, {0xB002, "CBC English Radio One"}, {0xB003, "CBC English Radio Two"},
        {0xB004, "Radio-Canada Premiere Chaine"}, {0xB005, "Radio-Canada Espace Musique"}, {0xB006, "CBC"},
        {0xB007, "CBC"}, {0xB008, "CBC"}, {0xB009, "CBC"}, {0xB00A, "NPR-2"}, {0xB00B, "NPR-3"},
        {0xB00C, "NPR-4"}, {0xB00D, "NPR-5"}, {0xB00E, "NPR-6"}};
    // PIs with zero nibbles are sent with an A / AF prefix
    uint32_t p = pi;
    if ((p & 0xFFF0) == 0xAFA0 && (p & 0xF) < 0xA) p = (p << 12) & 0xFFFF;
    else if ((p & 0xFF00) == 0xAF00) p = (p << 8) & 0xFFFF;
    else if ((p & 0xF000) == 0xA000) p = ((p & 0x0F00) << 4) | (p & 0xFF);
    if (p >= 0x9950 && p <= 0x9EFF) {
        for (const auto& e : THREE)
            if (e.pi == p) return e.c;
        return "";
    }
    const uint32_t n1 = p >> 12;
    if (n1 == 0xB || n1 == 0xD || n1 == 0xE) {
        const uint32_t q = p & 0xF0FF;
        for (const auto& e : LINKED)
            if (e.pi == q) return e.n;
        return "";
    }
    if (p >= 0x1000 && p <= 0x994F) {
        std::string s(4, 'K');
        uint32_t v = p;
        if (v <= 0x54A7) v -= 0x1000;
        else {
            s[0] = 'W';
            v -= 0x54A8;
        }
        s[1] = static_cast<char>('A' + (v / 676) % 26);
        s[2] = static_cast<char>('A' + (v / 26) % 26);
        s[3] = static_cast<char>('A' + v % 26);
        return s;
    }
    return "";
}

}  // namespace rds
