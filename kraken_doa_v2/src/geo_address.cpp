#include "geo_address.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace geo {

namespace {

// U+00C0..U+017F (Latin-1 Supplement + Latin Extended-A) -> base letter
// (' ' = not a letter). Generated from the Unicode decompositions.
// ß Æ æ Œ œ become two letters (SS, AE, OE) in words().
const char* const FOLD =
    "AAAAAAACEEEEIIIIDNOOOOO OUUUUYTSAAAAAAACEEEEIIIIDNOOOOO OUUUUYTYAAAAAACCCCCCCCDDDDEEEEEEEEEEGGGGGGGGHHHHIIIII"
    "IIIIIIIJJKK LLLLLLLLLLNNNNNNNNNOOOOOOOORRRRRRSSSSSSSSTTTTTTUUUUUUUUUUUUWWYYYZZZZZZS";

using Table = std::unordered_map<std::string, std::string>;
Table make(std::initializer_list<std::pair<const char*, std::initializer_list<const char*>>> l) {
    Table m;
    for (const auto& e : l) {
        m[e.first] = e.first;
        for (const char* f : e.second) m[f] = e.first;
    }
    return m;
}

// street-type words AFTER the name -> short form ("Queen Street", "Berliner
// Straße", "Karl Johans gate", "Váci utca")
const Table& types() {
    static const Table t = make({
        // English
        {"ST", {"STREET", "STR", "STRASSE", "STRAAT"}}, {"RD", {"ROAD"}}, {"AVE", {"AVENUE", "AV", "AVN"}},
        {"DR", {"DRIVE", "DRV"}}, {"PL", {"PLACE", "PLATZ", "PLEIN", "PLADS"}}, {"CRES", {"CRESCENT", "CR", "CRS", "CRESC"}},
        {"TCE", {"TERRACE", "TER", "TERR"}}, {"LN", {"LANE"}}, {"WAY", {"WY"}}, {"HWY", {"HIGHWAY", "HWAY"}},
        {"BLVD", {"BOULEVARD", "BVD"}}, {"CT", {"COURT", "CRT"}}, {"CL", {"CLOSE"}}, {"GR", {"GROVE", "GRV"}},
        {"PDE", {"PARADE", "PD"}}, {"SQ", {"SQUARE"}}, {"ESP", {"ESPLANADE"}}, {"QY", {"QUAY"}}, {"RISE", {}},
        {"VW", {"VIEW"}}, {"MEWS", {}}, {"WALK", {"WLK"}}, {"TRK", {"TRACK"}}, {"CIR", {"CIRCLE", "CIRCUS"}},
        {"LOOP", {}}, {"GLEN", {"GLN"}}, {"HTS", {"HEIGHTS"}}, {"PKWY", {"PARKWAY"}}, {"MWY", {"MOTORWAY"}},
        {"EXPY", {"EXPRESSWAY"}}, {"RDG", {"RIDGE"}}, {"GDNS", {"GARDENS"}}, {"BEND", {"BND"}}, {"ROW", {}},
        {"GRN", {"GREEN"}}, {"GATE", {}},
        // German / Dutch / Scandinavian / Finnish / Hungarian / Turkish
        // written as a separate word (often hyphenated: Karl-Marx-Allee)
        {"ALLEE", {}}, {"WEG", {}}, {"GASSE", {}}, {"RING", {}}, {"DAMM", {}}, {"UFER", {}}, {"CHAUSSEE", {}},
        {"LAAN", {}}, {"GRACHT", {}}, {"KADE", {}}, {"SINGEL", {}}, {"DIJK", {}}, {"GATAN", {}}, {"GATA", {}},
        {"VAGEN", {"VEGEN"}}, {"VEJ", {}}, {"GADE", {}}, {"VEI", {}}, {"VEIEN", {}}, {"KATU", {}}, {"TIE", {}},
        {"UTCA", {}}, {"UT", {}}, {"CAD", {"CADDESI", "CD"}}, {"SK", {"SOKAK", "SOKAGI"}},
    });
    return t;
}

// street-type words BEFORE the name -> short form ("Rue de la Paix", "Calle
// Mayor", "Via del Corso", "Rua Augusta", "ul. Marszałkowska", "Jalan ...")
const Table& prefix_types() {
    static const Table t = make({
        // French
        {"RUE", {}}, {"AV", {"AVENUE", "AVENIDA", "AVDA", "AVD"}}, {"BD", {"BOULEVARD", "BLVD", "BULEVARDUL"}},
        {"CH", {"CHEMIN"}}, {"ALLEE", {}}, {"PL", {"PLACE", "PLAZA", "PZA", "PLAC"}}, {"IMP", {"IMPASSE"}},
        {"QUAI", {}}, {"RTE", {"ROUTE"}}, {"COURS", {}}, {"RUELLE", {}}, {"SQ", {"SQUARE"}},
        // Spanish
        {"CALLE", {"CL", "CLL"}}, {"PASEO", {"PSO"}}, {"CRA", {"CARRERA", "KR"}}, {"CTRA", {"CARRETERA"}},
        {"CAMINO", {}}, {"RONDA", {}}, {"TRAV", {"TRAVESIA"}},
        // Italian
        {"VIA", {}}, {"VIALE", {"VLE"}}, {"PIAZZA", {"PIAZZALE"}}, {"CORSO", {}}, {"LARGO", {}}, {"VICOLO", {}},
        {"STRADA", {}}, {"LUNGOMARE", {}},
        // Portuguese
        {"RUA", {}}, {"PCA", {"PRACA"}}, {"TV", {"TRAVESSA"}}, {"EST", {"ESTRADA"}}, {"AL", {"ALAMEDA", "ALEJA"}},
        {"ROD", {"RODOVIA"}},
        // Polish / Romanian / Indonesian & Malay
        {"UL", {"ULICA"}}, {"OS", {"OSIEDLE"}}, {"CALEA", {}}, {"PIATA", {}}, {"SOS", {"SOSEAUA"}},
        {"JL", {"JALAN", "JLN"}},
    });
    return t;
}
// prefix types OSM leaves out of the name (Polish "ul. Marszałkowska" is
// just "Marszałkowska" in OSM)
bool prefix_not_in_name(const std::string& canon) { return canon == "UL" || canon == "OS"; }

// a type word joined to the name ("Friedrichstraße", "Kalverstraat",
// "Drottninggatan", "Vestergade", "Mannerheimintie"), longest first; the
// part before must be at least min_stem letters
struct Compound {
    const char* suffix;
    const char* canon;
    size_t min_stem;
};
const Compound COMPOUNDS[] = {
    {"STRASSE", "ST", 3}, {"STRAAT", "ST", 3}, {"CHAUSSEE", "CHAUSSEE", 3}, {"GRACHT", "GRACHT", 3},
    {"SINGEL", "SINGEL", 3}, {"TORGET", "TORGET", 3}, {"STEIG", "STEIG", 3}, {"GASSE", "GASSE", 3},
    {"PLATZ", "PL", 3}, {"ALLEE", "ALLEE", 3}, {"PLEIN", "PL", 3}, {"PLADS", "PL", 3}, {"GATAN", "GATAN", 3},
    {"VAGEN", "VAGEN", 3}, {"VEGEN", "VAGEN", 3}, {"VEIEN", "VEIEN", 3}, {"DREEF", "DREEF", 3}, {"GRAND", "GRAND", 4},
    {"DAMM", "DAMM", 3}, {"UFER", "UFER", 3}, {"RING", "RING", 4}, {"PFAD", "PFAD", 3}, {"LAAN", "LAAN", 3},
    {"KADE", "KADE", 3}, {"DIJK", "DIJK", 3}, {"GATA", "GATA", 3}, {"GADE", "GADE", 3}, {"ALLE", "ALLE", 4},
    {"KATU", "KATU", 3}, {"KUJA", "KUJA", 3}, {"UTCA", "UTCA", 3}, {"STR", "ST", 4}, {"WEG", "WEG", 3},
    {"VEJ", "VEJ", 3}, {"VEI", "VEI", 4}, {"TIE", "TIE", 4},
};
// "FRIEDRICHSTR" -> "FRIEDRICHST"; "" if not a compound street word
std::string compound_key(const std::string& w) {
    for (const Compound& c : COMPOUNDS) {
        const size_t sl = strlen(c.suffix);
        if (w.size() >= sl + c.min_stem && w.compare(w.size() - sl, sl, c.suffix) == 0) {
            const std::string stem = w.substr(0, w.size() - sl);
            if (std::any_of(stem.begin(), stem.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return "";
            return stem + c.canon;
        }
    }
    return "";
}

// a direction after the type ("Victoria Street West", "Queen St W")
const Table& directions() {
    static const Table d = make({{"N", {"NORTH"}}, {"S", {"SOUTH"}}, {"E", {"EAST"}}, {"W", {"WEST"}},
                                 {"UPPER", {"UPR"}}, {"LOWER", {"LWR"}}, {"EXT", {"EXTENSION"}}});
    return d;
}

// one spelling for words inside names: SAINT -> ST, MOUNT -> MT, NORTH -> N
// ("123 N MAIN ST" = North Main Street), FIFTH -> 5TH
std::string name_word(std::string w) {
    w.erase(std::remove(w.begin(), w.end(), '\''), w.end());
    static const Table m = make({
        {"ST", {"SAINT"}}, {"STE", {"SAINTE"}}, {"MT", {"MOUNT"}}, {"N", {"NORTH"}}, {"S", {"SOUTH"}}, {"E", {"EAST"}},
        {"W", {"WEST"}}, {"NE", {"NORTHEAST"}}, {"NW", {"NORTHWEST"}}, {"SE", {"SOUTHEAST"}}, {"SW", {"SOUTHWEST"}},
        {"1ST", {"FIRST"}}, {"2ND", {"SECOND"}}, {"3RD", {"THIRD"}}, {"4TH", {"FOURTH"}}, {"5TH", {"FIFTH"}},
        {"6TH", {"SIXTH"}}, {"7TH", {"SEVENTH"}}, {"8TH", {"EIGHTH"}}, {"9TH", {"NINTH"}}, {"10TH", {"TENTH"}},
    });
    auto it = m.find(w);
    return it == m.end() ? w : it->second;
}

// words that can't be part of a street name: a name stops there (pager
// vocabulary - "VEHICLE VS PEDESTRIAN GREAT NORTH RD")
bool stop_word(const std::string& w) {
    static const std::unordered_set<std::string> s = {
        "VS", "V", "NEAR", "NR", "AT", "ON", "IN", "OF", "FROM", "TO", "AND", "OR", "BY", "OFF", "OUTSIDE", "OPP",
        "OPPOSITE", "CNR", "CORNER", "BEHIND", "BETWEEN", "INTO", "ONTO", "XST", "X", "REPORTS", "REPORTED",
        "CALLER", "PATIENT", "RESPOND", "RESPONDING", "FIRE", "SMOKE", "ALARM", "MEDICAL", "CRASH", "MVA", "MVC",
        "RESCUE", "VEHICLE", "CAR", "TRUCK", "HOUSE", "BUILDING", "PERSON", "UNCONSCIOUS", "FALL", "ASSIST", "CALL",
        "UPDATE", "CREW", "SCENE", "STRUCTURE", "SCRUB", "GRASS", "RUBBISH", "BUSH", "ISSUING", "ROOF", "TEST",
        "PAGE", "PLEASE", "IGNORE", "FLAT", "UNIT", "APT", "LEVEL", "RTC", "EMS", "EINSATZ", "BRAND", "BRANN",
        "FEU", "INCENDIO", "INCENDIE", "POZAR", "TUZ", "KEBAKARAN", "SECOURS", "NOTFALL", "NO", "NUM", "NUMBER",
        "NUMERO"};
    return s.count(w) > 0;
}

bool has_digit(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 1ST 2ND 3RD 42ND: a name word (5TH AVE), not a house number
bool ordinal(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
    if (i == 0 || i + 2 != s.size()) return false;
    const std::string t = s.substr(i);
    return t == "ST" || t == "ND" || t == "RD" || t == "TH";
}

// 12, 12A, 2/15, 3-5, 2/15B
bool house_number(const std::string& s) {
    size_t i = 0;
    auto part = [&]() {
        size_t d = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && d < 5) { i++; d++; }
        if (d == 0) return false;
        if (i < s.size() && s[i] >= 'A' && s[i] <= 'Z') i++;
        return true;
    };
    if (!part()) return false;
    if (i < s.size() && (s[i] == '/' || s[i] == '-')) {
        i++;
        if (!part()) return false;
    }
    return i == s.size();
}

// a word that can be part of a street name
bool name_ok(const std::string& w) { return !stop_word(w) && (!has_digit(w) || ordinal(w)); }

}  // namespace

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    const double r = M_PI / 180.0;
    const double dlat = (lat2 - lat1) * r, dlon = (lon2 - lon1) * r;
    const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                     std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dlon / 2) * std::sin(dlon / 2);
    return 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

std::vector<std::string> words(const std::string& text, bool keep_numbers, std::vector<bool>* breaks) {
    std::vector<std::string> out;
    std::string cur;
    bool brk = false;   // , ; : ( ) seen since the last word
    auto push = [&](const std::string& w) {
        out.push_back(w);
        if (breaks) breaks->push_back(brk);
        brk = false;
    };
    auto flush = [&]() {
        while (!cur.empty() && cur.back() == '\'') cur.pop_back();
        if (cur.empty()) return;
        // "/" and "-" only stay inside a house number
        if (cur.find_first_of("/-") != std::string::npos && !house_number(cur)) {
            std::string part;
            for (char c : cur) {
                if (c == '/' || c == '-') {
                    if (!part.empty()) push(part);
                    part.clear();
                } else {
                    part += c;
                }
            }
            if (!part.empty()) push(part);
        } else {
            push(cur);
        }
        cur.clear();
    };
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xC0 && c < 0xE0 && i + 1 < text.size()) { cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F); len = 2; }
        else if (c >= 0xE0 && c < 0xF0 && i + 2 < text.size()) {
            cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
            len = 3;
        } else if (c >= 0xF0) { len = 4; cp = 0; }
        i += len;
        if ((cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9')) cur += static_cast<char>(cp);
        else if (cp >= 'a' && cp <= 'z') cur += static_cast<char>(cp - 32);
        else if (cp == '\'' || cp == '`' || cp == 0x2019 || cp == 0x2018) {
            if (!cur.empty()) cur += '\'';   // kept for the search (O'CONNELL), dropped from keys
        }
        else if (cp == 0xDF) cur += "SS";                                             // ß
        else if (cp == 0xC6 || cp == 0xE6) cur += "AE";                               // Æ
        else if (cp == 0x152 || cp == 0x153) cur += "OE";                             // Œ
        else if (cp >= 0xC0 && cp < 0x180 && FOLD[cp - 0xC0] != ' ') cur += FOLD[cp - 0xC0];
        else if (keep_numbers && (cp == '/' || cp == '-') && !cur.empty()) cur += static_cast<char>(cp);
        else {
            flush();
            if (cp == ',' || cp == ';' || cp == ':' || cp == '(' || cp == ')') brk = true;
        }
    }
    flush();
    return out;
}

std::string street_key(const std::string& name) {
    std::vector<std::string> w = words(name);
    if (w.empty()) return "";
    const auto& T = types();
    const auto& P = prefix_types();
    const auto& D = directions();
    size_t end = w.size();
    std::string tail, head;
    if (end >= 3 && D.count(w[end - 1]) && T.count(w[end - 2])) {
        tail = T.at(w[end - 2]) + " " + D.at(w[end - 1]);
        end -= 2;
    } else if (end >= 2 && T.count(w[end - 1])) {
        tail = T.at(w[end - 1]);
        end -= 1;
    }
    size_t start = 0;
    if (tail.empty() && end >= 2 && P.count(w[0])) {
        const std::string pc = P.at(w[0]);
        if (!prefix_not_in_name(pc)) head = pc;
        start = 1;
    }
    std::string k = head;
    for (size_t i = start; i < end; i++) {
        std::string x = name_word(w[i]);
        if (i + 1 == end && tail.empty()) {
            const std::string c = compound_key(x);   // x: name_word, apostrophes gone
            if (!c.empty()) x = c;
        }
        k += (k.empty() ? "" : " ") + x;
    }
    if (!tail.empty()) k += (k.empty() ? "" : " ") + tail;
    return k;
}

std::string place_key(const std::string& name) {
    std::string k;
    for (const auto& w : words(name)) k += (k.empty() ? "" : " ") + name_word(w);
    return k;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
const Json* Json::get(const std::string& key) const {
    for (const auto& kv : o)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string Json::str(const std::string& key) const {
    const Json* v = get(key);
    return v && v->type == STR ? v->s : std::string();
}

namespace {
struct Parser {
    const std::string& t;
    size_t p = 0;
    int depth = 0;
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) p++; }
    static void utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o += static_cast<char>(c);
        else if (c < 0x800) { o += static_cast<char>(0xC0 | (c >> 6)); o += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            o += static_cast<char>(0xE0 | (c >> 12));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (c >> 18));
            o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    bool str(std::string& o) {
        if (p >= t.size() || t[p] != '"') return false;
        p++;
        while (p < t.size() && t[p] != '"') {
            if (t[p] == '\\') {
                if (++p >= t.size()) return false;
                char e = t[p++];
                switch (e) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': o += '\r'; break;
                    case 'b': o += '\b'; break;
                    case 'f': o += '\f'; break;
                    case 'u': {
                        if (p + 4 > t.size()) return false;
                        uint32_t c = static_cast<uint32_t>(strtoul(t.substr(p, 4).c_str(), nullptr, 16));
                        p += 4;
                        if (c >= 0xD800 && c < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                            uint32_t lo = static_cast<uint32_t>(strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16));
                            p += 6;
                            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(o, c);
                        break;
                    }
                    default: o += e;
                }
            } else {
                o += t[p++];
            }
        }
        if (p >= t.size()) return false;
        p++;
        return true;
    }
    bool val(Json& v) {
        if (++depth > 64) return false;
        ws();
        if (p >= t.size()) return false;
        bool ok = true;
        const char c = t[p];
        if (c == '{') {
            v.type = Json::OBJ;
            p++;
            ws();
            if (p < t.size() && t[p] == '}') { p++; depth--; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= t.size() || t[p] != ':') return false;
                p++;
                Json x;
                if (!val(x)) return false;
                v.o.emplace_back(std::move(k), std::move(x));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == '}') { p++; break; }
                return false;
            }
        } else if (c == '[') {
            v.type = Json::ARR;
            p++;
            ws();
            if (p < t.size() && t[p] == ']') { p++; depth--; return true; }
            for (;;) {
                Json x;
                if (!val(x)) return false;
                v.a.push_back(std::move(x));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == ']') { p++; break; }
                return false;
            }
        } else if (c == '"') {
            v.type = Json::STR;
            ok = str(v.s);
        } else if (t.compare(p, 4, "true") == 0) { v.type = Json::BOOL; v.b = true; p += 4; }
        else if (t.compare(p, 5, "false") == 0) { v.type = Json::BOOL; p += 5; }
        else if (t.compare(p, 4, "null") == 0) { v.type = Json::NUL; p += 4; }
        else {
            char* e = nullptr;
            v.type = Json::NUM;
            v.n = strtod(t.c_str() + p, &e);
            if (!e || e == t.c_str() + p) return false;
            p = static_cast<size_t>(e - t.c_str());
        }
        depth--;
        return ok;
    }
};
}  // namespace

bool json_parse(const std::string& text, Json* out) {
    Parser ps{text};
    *out = Json();
    if (!ps.val(*out)) return false;
    ps.ws();
    return ps.p == text.size();
}


// ---------------------------------------------------------------------------
// Geocoding
// ---------------------------------------------------------------------------
namespace {

struct Hit {
    double lat = 0, lon = 0;
    std::string road, number, area;
    std::vector<std::pair<std::string, std::string>> places;   // (key, name) of its suburb, town...
};

// house numbers compare without case; "2/15" (unit 2 at 15) also matches 15
bool same_number(const std::string& msg, const std::string& osm) {
    if (osm.empty()) return false;
    std::string o;
    for (char c : osm)
        if (c != ' ') o += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (o == msg) return true;
    const size_t sl = msg.find('/');
    return sl != std::string::npos && msg.substr(sl + 1) == o;
}

// umlauts written as AE / OE / UE in a message ("MUELLERSTR" = Müllerstraße):
// compared with those pairs as single letters on both sides
std::string loose(const std::string& k) {
    std::string o;
    for (size_t i = 0; i < k.size(); i++) {
        if (k[i] == ' ' || k[i] == '\'') continue;   // "MH THAMRIN" = "M H THAMRIN"
        o += k[i];
        if ((k[i] == 'A' || k[i] == 'O' || k[i] == 'U') && i + 1 < k.size() && k[i + 1] == 'E') i++;
    }
    return o;
}

// the road found is the street asked for; motorways / highways also
// without their leading words ("Auckland Southern Motorway" for SOUTHERN MWY)
bool same_street(const std::string& found, const std::string& want) {
    if (found == want || loose(found) == loose(want)) return true;
    // OSM leaves the leading type out ("MH Thamrin" for JL MH THAMRIN)
    const size_t sp0 = want.find(' ');
    if (sp0 != std::string::npos) {
        static const std::unordered_set<std::string> prefixes = [] {
            std::unordered_set<std::string> v;
            for (const auto& kv : prefix_types()) v.insert(kv.second);
            return v;
        }();
        if (prefixes.count(want.substr(0, sp0)) && loose(found) == loose(want.substr(sp0 + 1))) return true;
    }
    // the text leaves out a direction OSM has ("O'Connell Street Lower", "Queen Street West")
    if (found.size() > want.size() + 1 && found.compare(0, want.size() + 1, want + " ") == 0) {
        const std::string d = found.substr(want.size() + 1);
        if (d == "N" || d == "S" || d == "E" || d == "W" || d == "UPPER" || d == "LOWER" || d == "EXT") return true;
    }
    const size_t sp = want.rfind(' ');
    const std::string type = sp == std::string::npos ? "" : want.substr(sp + 1);
    return (type == "MWY" || type == "HWY" || type == "EXPY") && found.size() > want.size() &&
           found.compare(found.size() - want.size(), want.size(), want) == 0 && found[found.size() - want.size() - 1] == ' ';
}

std::vector<Hit> hits_of(const Json& js, const std::string& key, double st_lat, double st_lon, double radius_km) {
    std::vector<Hit> out;
    if (js.type != Json::ARR) return out;
    for (const Json& it : js.a) {
        const Json* ad = it.get("address");
        std::string road = ad ? ad->str("road") : "";
        if (road.empty() && it.str("category") == "highway") road = it.str("name");
        if (road.empty() || !same_street(street_key(road), key)) continue;
        Hit h;
        h.lat = atof(it.str("lat").c_str());
        h.lon = atof(it.str("lon").c_str());
        if (!(std::fabs(h.lat) <= 90 && std::fabs(h.lon) <= 180) || (h.lat == 0 && h.lon == 0)) continue;
        if (distance_km(st_lat, st_lon, h.lat, h.lon) > radius_km) continue;
        h.road = road;
        if (ad) {
            h.number = ad->str("house_number");
            std::string local, town;
            for (const char* f : {"suburb", "neighbourhood", "quarter", "city_district", "hamlet", "village", "town",
                                  "city", "municipality"}) {
                std::string v = ad->str(f);
                if (v.empty()) continue;
                std::string k = place_key(v);
                h.places.emplace_back(k, v);
                // "Hamilton City", "Hamilton Central" - the text may say just HAMILTON
                static const char* generic[] = {" CITY", " CENTRAL", " DISTRICT", " TOWNSHIP", " N", " S", " E",
                                                " W", " MITTE", " CENTRUM", " CENTRO"};
                for (const char* g : generic) {
                    const size_t gl = strlen(g);
                    if (k.size() > gl + 2 && k.compare(k.size() - gl, gl, g) == 0) {
                        h.places.emplace_back(k.substr(0, k.size() - gl), v);
                        break;
                    }
                }
                if (local.empty() && (!strcmp(f, "suburb") || !strcmp(f, "neighbourhood") || !strcmp(f, "quarter") ||
                                      !strcmp(f, "hamlet") || !strcmp(f, "village")))
                    local = v;
                if (town.empty() && (!strcmp(f, "town") || !strcmp(f, "city") || !strcmp(f, "municipality"))) town = v;
            }
            h.area = local.empty() ? town : (town.empty() || town == local ? local : local + ", " + town);
        }
        out.push_back(std::move(h));
    }
    return out;
}

// stretches of one road name: points within 3 km link up
int stretches(const std::vector<Hit>& h, const Hit& chosen, int* chosen_group) {
    const size_t m = std::min<size_t>(h.size(), 200);
    std::vector<int> g(m, -1);
    int groups = 0;
    for (size_t s0 = 0; s0 < m; s0++) {
        if (g[s0] >= 0) continue;
        std::vector<size_t> todo{s0};
        g[s0] = groups;
        while (!todo.empty()) {
            const size_t x = todo.back();
            todo.pop_back();
            for (size_t y = 0; y < m; y++)
                if (g[y] < 0 && distance_km(h[x].lat, h[x].lon, h[y].lat, h[y].lon) <= 3) {
                    g[y] = groups;
                    todo.push_back(y);
                }
        }
        groups++;
    }
    double best = 1e9;
    *chosen_group = -1;
    for (size_t s0 = 0; s0 < m; s0++) {
        const double d = distance_km(chosen.lat, chosen.lon, h[s0].lat, h[s0].lon);
        if (d < best) { best = d; *chosen_group = g[s0]; }
    }
    return groups;
}

}  // namespace

bool geocode(const std::string& text, double st_lat, double st_lon, double radius_km, const SearchFn& search,
             Result* r, std::string* err, bool* offline) {
    err->clear();
    *offline = false;
    std::vector<bool> brk;
    const std::vector<std::string> w = words(text, true, &brk);
    const size_t n = w.size();
    const auto& T = types();
    const auto& P = prefix_types();
    const auto& D = directions();
    std::vector<bool> used(n, false);
    // the search box: radius_km around the station
    const double dlat = radius_km / 111.32;
    const double dlon = radius_km / (111.32 * std::max(0.1, std::cos(st_lat * M_PI / 180.0)));
    const double bs = std::max(-90.0, st_lat - dlat), bn = std::min(90.0, st_lat + dlat);
    const double bw = std::max(-180.0, st_lon - dlon), be = std::min(180.0, st_lon + dlon);
    int lookups = 0;
    constexpr int MAX_LOOKUPS = 10;
    // a search: -1 failed (err set), else the matching hits
    auto find = [&](const std::string& q, const std::string& key, std::vector<Hit>* hits) -> int {
        if (lookups >= MAX_LOOKUPS) return 0;
        lookups++;
        std::string js;
        if (!search(q, bs, bw, bn, be, &js, err, offline)) return -1;
        Json j;
        if (!json_parse(js, &j)) { *err = "unreadable answer from the address search"; return -1; }
        *hits = hits_of(j, key, st_lat, st_lon, radius_km);
        return 0;
    };
    auto join = [&](size_t a, size_t b) {   // words a..b as written
        std::string s;
        for (size_t x = a; x <= b; x++) s += (s.empty() ? "" : " ") + w[x];
        return s;
    };
    // a word right before / after a street that is its house number
    // (after: also "NO 1", "NR 5")
    auto num_prefix = [&](size_t x) {
        return x < n && (w[x] == "NO" || w[x] == "NR" || w[x] == "NUM" || w[x] == "NUMBER" || w[x] == "NUMERO" || w[x] == "N");
    };
    auto num_at = [&](long x) -> std::string {
        if (x >= 0 && static_cast<size_t>(x) + 1 < n && num_prefix(static_cast<size_t>(x)) &&
            house_number(w[static_cast<size_t>(x) + 1]) && !used[static_cast<size_t>(x) + 1])
            return w[static_cast<size_t>(x) + 1];
        return x >= 0 && static_cast<size_t>(x) < n && !used[static_cast<size_t>(x)] && house_number(w[static_cast<size_t>(x)])
                   ? w[static_cast<size_t>(x)] : "";
    };

    // Candidate streets, three patterns. Each group lists its spans (first,
    // last word), most words first; a group's number is the house number
    // next to its longest span (before it, else after it)
    struct Span { size_t a, b; };
    struct Group {
        std::vector<Span> spans;
        std::string number;
        bool num_before = true;
        int prio;
    };
    std::vector<Group> groups;
    for (size_t i = 0; i < n; i++) {
        // 1. name + type ("12 QUEEN ST", "BERLINER STR 5", "QUEEN ST W")
        auto t = T.find(w[i]);
        if (t != T.end() && i >= 1) {
            size_t j = i, cnt = 0;
            while (j > 0 && cnt < 4 && name_ok(w[j - 1]) && !brk[j]) {
                if (T.count(w[j - 1]) && j >= 2 && T.count(w[j - 2])) break;
                j--;
                cnt++;
            }
            if (cnt) {
                const size_t end = (i + 1 < n && D.count(w[i + 1]) && !brk[i + 1]) ? i + 1 : i;
                Group g;
                for (size_t k = cnt; k >= 1; k--) {
                    if (end > i) g.spans.push_back({i - k, end});
                    g.spans.push_back({i - k, i});
                }
                g.number = num_at(static_cast<long>(j) - 1);
                if (g.number.empty()) { g.number = num_at(static_cast<long>(end) + 1); g.num_before = false; }
                g.prio = 0;
                groups.push_back(std::move(g));
            }
        }
        // 2. type + name ("RUE DE LA PAIX", "CALLE MAYOR 1", "UL MARSZALKOWSKA 10")
        if (P.count(w[i]) && i + 1 < n && !brk[i + 1]) {
            size_t j = i, cnt = 0;
            while (j + 1 < n && cnt < 4 && name_ok(w[j + 1]) && (j == i || !brk[j + 1])) {
                j++;
                cnt++;
            }
            if (cnt) {
                Group g;
                for (size_t k = cnt; k >= 1; k--) g.spans.push_back({i, i + k});
                g.number = num_at(static_cast<long>(j) + 1);
                g.num_before = false;
                if (g.number.empty()) { g.number = num_at(static_cast<long>(i) - 1); g.num_before = true; }
                g.prio = 1;
                groups.push_back(std::move(g));
            }
        }
        // 3. one word with the type joined on ("FRIEDRICHSTR 43", "PRINSENGRACHT 263")
        if (w[i].size() >= 6 && !T.count(w[i]) && !P.count(w[i]) && name_ok(w[i]) && !compound_key(w[i]).empty()) {
            Group g;
            g.spans.push_back({i, i});
            g.number = num_at(static_cast<long>(i) + 1);
            g.num_before = false;
            if (g.number.empty()) { g.number = num_at(static_cast<long>(i) - 1); g.num_before = true; }
            g.prio = 2;
            groups.push_back(std::move(g));
        }
    }
    // with a house number first, then by pattern, then in text order
    std::stable_sort(groups.begin(), groups.end(), [](const Group& x, const Group& y) {
        if (x.number.empty() != y.number.empty()) return !x.number.empty();
        return x.prio < y.prio;
    });

    struct Cand {
        std::string key, number, street;   // street: the words as written (query text)
        bool num_before = true;
        std::vector<Hit> hits;
        bool exact = false;                // hits are the house itself
        std::vector<std::string> after;    // up to 2 words after the street (a suburb / town?)
        std::string query(bool with_num) const {
            if (!with_num || number.empty()) return street;
            return num_before ? number + " " + street : street + " " + number;
        }
    };
    std::vector<Cand> cands;
    for (const Group& g : groups) {
        if (lookups >= MAX_LOOKUPS) break;
        bool found = false;
        for (size_t si = 0; si < g.spans.size() && !found && lookups < MAX_LOOKUPS; si++) {
            const Span sp = g.spans[si];
            bool free = true;
            for (size_t x = sp.a; x <= sp.b; x++) free &= !used[x];
            if (!free) continue;
            Cand c;
            c.street = join(sp.a, sp.b);
            c.key = street_key(c.street);
            if (c.key.empty()) continue;
            // the number only goes with a span it is right next to
            const bool next_to = !g.number.empty() &&
                                 (g.num_before ? sp.a >= 1 && w[sp.a - 1] == g.number
                                               : (sp.b + 1 < n && w[sp.b + 1] == g.number) ||
                                                     (sp.b + 2 < n && num_prefix(sp.b + 1) && w[sp.b + 2] == g.number));
            c.number = next_to ? g.number : "";
            c.num_before = g.num_before;
            std::vector<Hit> hits;
            if (!c.number.empty()) {
                if (find(c.query(true), c.key, &hits) < 0) return false;
                std::vector<Hit> ex;
                for (const Hit& h : hits)
                    if (same_number(c.number, h.number)) ex.push_back(h);
                c.exact = !ex.empty();
                hits.swap(ex);
            }
            if (!c.exact) {
                hits.clear();
                if (find(c.street, c.key, &hits) < 0) return false;
            }
            if (hits.empty()) continue;
            c.hits = std::move(hits);
            found = true;
            size_t lo = sp.a, hi = sp.b;
            if (!c.number.empty()) {
                // mark the number word too
                if (c.num_before && sp.a >= 1 && w[sp.a - 1] == c.number) lo = sp.a - 1;
                if (!c.num_before && sp.b + 1 < n && w[sp.b + 1] == c.number) hi = sp.b + 1;
                if (!c.num_before && sp.b + 2 < n && num_prefix(sp.b + 1) && w[sp.b + 2] == c.number) hi = sp.b + 2;
            }
            for (size_t x = hi + 1; x < n && c.after.size() < 2; x++) {
                if (used[x] || stop_word(w[x]) || has_digit(w[x]) || T.count(w[x]) || P.count(w[x])) break;
                c.after.push_back(w[x]);
            }
            for (size_t x = lo; x <= hi; x++) used[x] = true;
            cands.push_back(std::move(c));
        }
    }
    if (cands.empty()) return false;

    // the rest of the text, to find place names in (used words blocked);
    // compared loosely: ZUERICH = Zürich, apostrophes / spaces aside
    std::vector<std::string> rest_words;
    for (size_t x = 0; x < n; x++) rest_words.push_back(used[x] ? std::string("|") : name_word(w[x]));
    auto named = [&](const Hit& h, std::string* which) {
        for (const auto& pl : h.places) {
            if (pl.first.size() < 3) continue;
            const std::string want = loose(pl.first);
            const size_t nw = static_cast<size_t>(std::count(pl.first.begin(), pl.first.end(), ' ')) + 1;
            for (size_t a = 0; a + nw <= rest_words.size(); a++) {
                std::string got;
                bool ok = true;
                for (size_t x = a; x < a + nw; x++) {
                    if (rest_words[x] == "|") { ok = false; break; }
                    got += rest_words[x];
                }
                if (ok && loose(got) == want) {
                    if (which) *which = pl.second;
                    return true;
                }
            }
        }
        return false;
    };

    // the address: the first street with a house number, else the first one
    size_t pi = 0;
    for (size_t x = 0; x < cands.size(); x++)
        if (!cands[x].number.empty()) { pi = x; break; }
    Cand& A = cands[pi];
    Result res;
    res.number = A.number;
    res.key = A.key + "|" + A.number;
    Hit pick;
    bool chosen = false, exact = false;
    std::vector<Hit>* pool = &A.hits;   // what "other streets of that name" is counted in

    // 1. the house, in a place the text names
    if (A.exact)
        for (const Hit& h : A.hits)
            if (named(h, &res.place)) { pick = h; chosen = exact = true; break; }
    // 2. a cross street named in the text: the closest pair (within 1.5 km)
    if (!chosen && !A.exact) {
        for (size_t x = 0; x < cands.size() && !chosen; x++) {
            if (x == pi || cands[x].key == A.key) continue;
            double best = 1e9;
            const Hit *ba = nullptr, *bb = nullptr;
            for (const Hit& a : A.hits)
                for (const Hit& b : cands[x].hits) {
                    const double d = distance_km(a.lat, a.lon, b.lat, b.lon);
                    if (d < best) { best = d; ba = &a; bb = &b; }
                }
            if (ba && best <= 1.5) {
                pick = *ba;
                pick.lat = (ba->lat + bb->lat) / 2;
                pick.lon = (ba->lon + bb->lon) / 2;
                res.cross = bb->road;
                res.precision = "junction";
                chosen = true;
            }
        }
    }
    // 3. the street + the words after it (a suburb / town), e.g. "17 ATKINSON
    //    AVE OTAHUHU" when another 17 Atkinson Ave exists elsewhere
    std::vector<Hit> extra;
    for (size_t m = std::min<size_t>(2, A.after.size()); m >= 1 && !chosen && lookups < MAX_LOOKUPS; m--) {
        std::string tail;
        for (size_t x = 0; x < m; x++) tail += " " + A.after[x];
        for (int with_num = A.number.empty() ? 0 : 1; with_num >= 0 && !chosen; with_num--) {
            std::vector<Hit> hs;
            if (find(A.query(with_num) + tail, A.key, &hs) < 0) return false;
            // the house first, then the street, in the named place. Nominatim
            // only answers when every word matched, so a hit that doesn't
            // name the place still matched it somewhere (its first one, then)
            for (int want_exact = with_num; want_exact >= 0 && !chosen; want_exact--)
                for (const Hit& h : hs)
                    if ((!want_exact || same_number(A.number, h.number)) && named(h, &res.place)) {
                        pick = h;
                        chosen = true;
                        exact = want_exact;
                        break;
                    }
            if (!chosen && !hs.empty()) {
                pick = hs[0];
                for (const Hit& h : hs)
                    if (same_number(A.number, h.number)) { pick = h; break; }
                exact = same_number(A.number, pick.number);
                res.place = tail.substr(1);
                chosen = true;
            }
            if (chosen) { extra = std::move(hs); pool = &extra; }
        }
    }
    // words after the street that found nothing: the text may name a place
    // the chosen street isn't in
    const bool tail_missed = !A.after.empty() && !chosen;
    // 4. the house (several: the nearest)
    if (!chosen && A.exact) {
        double best = 1e9;
        for (const Hit& h : A.hits) {
            const double d = distance_km(st_lat, st_lon, h.lat, h.lon);
            if (d < best) { best = d; pick = h; chosen = exact = true; }
        }
    }
    // 5. the street in a place the text names, 6. the nearest
    if (!chosen)
        for (const Hit& h : A.hits)
            if (named(h, &res.place)) { pick = h; chosen = true; break; }
    if (!chosen) {
        double best = 1e9;
        for (const Hit& h : A.hits) {
            const double d = distance_km(st_lat, st_lon, h.lat, h.lon);
            if (d < best) { best = d; pick = h; chosen = true; }
        }
    }
    res.lat = pick.lat;
    res.lon = pick.lon;
    if (res.precision.empty()) res.precision = exact ? "address" : "street";
    res.street = pick.road;
    res.area = pick.area;
    res.address = A.number.empty() ? pick.road : A.num_before ? A.number + " " + pick.road : pick.road + " " + A.number;
    int group = -1;
    const int nstretch = stretches(*pool, pick, &group);
    res.alternatives = group >= 0 ? nstretch - 1 : 0;
    res.confidence = res.precision == "junction" || !res.place.empty() ? "high"
                   : res.alternatives || tail_missed                  ? "low"
                   : exact                                            ? "high"
                                                                      : "medium";
    res.lookups = lookups;
    *r = res;
    return true;
}

}  // namespace geo
