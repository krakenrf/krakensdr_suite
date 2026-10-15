// First-generation 406 MHz message contents, C/S T.001 Issue 4 Rev.13 Annex A
// (section numbers below). Checked against the worked examples of T.001
// Annex B and C/S G.005 Annex A (README).

#include "cs406.hpp"

#include "kraken_dsp.hpp"
#include "mid.hpp"

#include <cstdio>

namespace b406 {

namespace {

struct Bits {
    const std::vector<uint8_t>& b;
    // T.001 bit numbers (1-based), n bits MSB first
    uint32_t u(int a, int n) const {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            const size_t k = static_cast<size_t>(a - 1 + i);
            v = (v << 1) | (k < b.size() ? (b[k] & 1) : 0);
        }
        return v;
    }
};

// modified Baudot (Table A3): 6-bit code -> character
char baudot6(uint32_t c) {
    static const struct { uint8_t code; char ch; } T[] = {
        {0x38, 'A'}, {0x33, 'B'}, {0x2E, 'C'}, {0x32, 'D'}, {0x30, 'E'}, {0x36, 'F'}, {0x2B, 'G'}, {0x25, 'H'},
        {0x2C, 'I'}, {0x3A, 'J'}, {0x3E, 'K'}, {0x29, 'L'}, {0x27, 'M'}, {0x26, 'N'}, {0x23, 'O'}, {0x2D, 'P'},
        {0x3D, 'Q'}, {0x2A, 'R'}, {0x34, 'S'}, {0x21, 'T'}, {0x3C, 'U'}, {0x2F, 'V'}, {0x39, 'W'}, {0x37, 'X'},
        {0x35, 'Y'}, {0x31, 'Z'}, {0x24, ' '}, {0x18, '-'}, {0x17, '/'}, {0x0D, '0'}, {0x1D, '1'}, {0x19, '2'},
        {0x10, '3'}, {0x0A, '4'}, {0x01, '5'}, {0x15, '6'}, {0x1C, '7'}, {0x0C, '8'}, {0x03, '9'}};
    for (const auto& e : T)
        if (e.code == c) return e.ch;
    return '?';
}
// the 5-bit form (letters + space: the 6-bit code without its leading 1)
char baudot5(uint32_t c) { return baudot6(0x20 | (c & 0x1F)); }

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') i++;
    return s.substr(i);
}

std::string baudot_str(const Bits& m, int a, int nchar) {
    std::string s;
    for (int i = 0; i < nchar; i++) s += baudot6(m.u(a + 6 * i, 6));
    return trim(s);
}
std::string baudot5_str(const Bits& m, int a, int nchar) {
    std::string s;
    for (int i = 0; i < nchar; i++) s += baudot5(m.u(a + 5 * i, 5));
    return trim(s);
}

std::string hexv(uint32_t v, int digits) {
    char b[16];
    snprintf(b, sizeof b, "%0*X", digits, v);
    return b;
}
std::string bin(uint32_t v, int n) {
    std::string s;
    for (int i = n - 1; i >= 0; i--) s += static_cast<char>('0' + ((v >> i) & 1));
    return s;
}

void add(Message& g, const std::string& k, const std::string& v) { g.fields.push_back({k, kp::printable(v)}); }

const char* aux_text(uint32_t v) {   // A2.1
    switch (v) {
        case 0: return "none";
        case 1: return "121.5 MHz";
        case 2: return "9 GHz SART";
        default: return "other radio-locating device";
    }
}

// the 15-hex ID (3.3): bits 26-85, the PDF-1 position replaced by its default
std::string id15(const std::vector<uint8_t>& bits, int from, const char* dflt) {
    std::vector<uint8_t> b(bits.begin() + 25, bits.begin() + 85);   // bits 26..85
    if (dflt)
        for (size_t i = 0; dflt[i]; i++) b[static_cast<size_t>(from - 26) + i] = dflt[i] == '1';
    return bits_hex(b, 0, 60);
}

void type_for(Message& g, const std::string& t) {
    g.type = t;
    if (t == "EPIRB" || t == "SSAS") g.map_kind = "ship";
    else if (t == "ELT" || t == "ELT(DT)") g.map_kind = "aircraft";
    else if (t == "PLB") g.map_kind = "person";
    else g.map_kind = "point";
}

// a coarse position (magnitudes in degrees, S / W flags) + an offset in minutes /
// 4-second steps whose sign adds to / subtracts from the magnitude (A3.3.1)
struct Coarse {
    double lat, lon;
};
bool offset(const Bits& m, int a, int min_bits, double* deg) {
    const uint32_t s = m.u(a, 1), mi = m.u(a + 1, min_bits), q = m.u(a + 1 + min_bits, 4);
    if (q > 14) return false;   // default 1111 = no offset
    *deg = (s ? 1 : -1) * (mi / 60.0 + q * 4 / 3600.0);
    return true;
}
void set_pos(Message& g, bool south, double lat, bool west, double lon) {
    g.has_pos = true;
    g.lat = south ? -lat : lat;
    g.lon = west ? -lon : lon;
}

// standard location (A3.3.5): coarse 1/4 deg (bits 65-85), offsets (113-132)
void std_position(Message& g, const Bits& m, bool pdf2) {
    const uint32_t la = m.u(66, 9), lo = m.u(76, 10);
    if (la > 360 || lo > 720) {   // the default is all ones
        g.pos_text = "no position (default)";
        return;
    }
    double lat = la * 0.25, lon = lo * 0.25, dla, dlo;
    g.pos_text = "coarse only: 1/4 deg steps";
    g.pos_err_m = 15000;
    if (pdf2 && offset(m, 113, 5, &dla) && offset(m, 123, 5, &dlo)) {
        lat += dla;
        lon += dlo;
        g.pos_text = "1/4 deg + offset (4 s steps)";
        g.pos_err_m = 70;
    }
    set_pos(g, m.u(65, 1), lat, m.u(75, 1), lon);
}

// national location (A3.3.6): coarse 2' (bits 59-85), offsets (113-126) if bit 110
void nat_position(Message& g, const Bits& m, bool pdf2) {
    const uint32_t ld = m.u(60, 7), lm = m.u(67, 5), od = m.u(73, 8), om = m.u(81, 5);
    if (ld > 90 || od > 180 || lm > 29 || om > 29) {
        g.pos_text = "no position (default)";
        return;
    }
    double lat = ld + lm * 2 / 60.0, lon = od + om * 2 / 60.0, dla, dlo;
    g.pos_text = "coarse only: 2' steps";
    g.pos_err_m = 1900;
    if (pdf2 && m.u(110, 1) && offset(m, 113, 2, &dla) && offset(m, 120, 2, &dlo)) {
        lat += dla;
        lon += dlo;
        g.pos_text = "2' + offset (4 s steps)";
        g.pos_err_m = 70;
    }
    set_pos(g, m.u(59, 1), lat, m.u(72, 1), lon);
}

// RLS / ELT(DT) (A3.3.7 / A3.3.8): coarse 1/2 deg (bits 67-85), offsets (115-132)
void half_position(Message& g, const Bits& m, bool pdf2, bool offsets) {
    const uint32_t la = m.u(68, 8), lo = m.u(77, 9);
    if (la > 180 || lo > 360) {
        g.pos_text = "no position (default)";
        return;
    }
    double lat = la * 0.5, lon = lo * 0.5, dla, dlo;
    g.pos_text = "coarse only: 1/2 deg steps";
    g.pos_err_m = 30000;
    if (pdf2 && offsets && offset(m, 115, 4, &dla) && offset(m, 124, 4, &dlo)) {
        lat += dla;
        lon += dlo;
        g.pos_text = "1/2 deg + offset (4 s steps)";
        g.pos_err_m = 70;
    }
    set_pos(g, m.u(67, 1), lat, m.u(76, 1), lon);
}

void homing_bit(Message& g, uint32_t v) {
    g.homing = static_cast<int>(v);
    g.homing_text = v ? "121.5 MHz" : "none";
}
void source_bit(Message& g, uint32_t v) {
    g.pos_source = v ? "internal navigation device" : "external navigation device";
}

// short user messages: the non-protected bits 107-112 (A2.9)
void emergency_code(Message& g, const Bits& m, bool maritime) {
    const uint32_t e = m.u(107, 6);
    std::string s = bin(e, 6) + ": ";
    s += (e & 0x10) ? "automatic + manual activation" : "manual activation only";
    if (e & 0x20) {
        const uint32_t c = e & 0xF;
        if (maritime) {
            static const char* const N[9] = {"unspecified distress", "fire / explosion", "flooding", "collision",
                                             "grounding", "listing / capsizing", "sinking", "disabled and adrift",
                                             "abandoning ship"};
            s += std::string(", emergency code: ") + (c < 9 ? N[c] : "spare");
        } else {
            s += ", emergency code:";
            if (c & 8) s += " fire";
            if (c & 4) s += " medical help";
            if (c & 2) s += " disabled";
            if (!(c & 0xE)) s += " none given";
        }
    } else if (e & 0xF) {
        s += ", national use " + bin(e & 0xF, 4);
    }
    g.emergency = s;
}

void user_protocol(Message& g, const Bits& m, const std::vector<uint8_t>& bits, bool pdf2) {
    g.code = static_cast<int>(m.u(37, 3));
    const uint32_t aux = m.u(84, 2);
    auto aux_field = [&] {
        g.homing = aux == 1;
        g.homing_text = aux_text(aux);
    };
    bool maritime = false;
    switch (g.code) {
        case 2: {   // maritime user (A2.2)
            g.protocol = "Maritime user";
            type_for(g, "EPIRB");
            maritime = true;
            const std::string id = baudot_str(m, 40, 6);
            const bool digits = !id.empty() && id.find_first_not_of("0123456789") == std::string::npos;
            add(g, digits ? "MMSI (last 6 digits)" : "Radio call sign", id);
            add(g, "Beacon number", std::string(1, baudot6(m.u(76, 6))));
            g.ident = (digits ? "MMSI ..." : "call sign ") + id;
            aux_field();
            break;
        }
        case 6: {   // radio call sign user (A2.3)
            g.protocol = "Radio call sign user";
            type_for(g, "EPIRB");
            maritime = true;
            std::string cs;
            for (int i = 0; i < 4; i++) cs += baudot6(m.u(40 + 6 * i, 6));
            for (int i = 0; i < 3; i++) {
                const uint32_t d = m.u(64 + 4 * i, 4);
                cs += d < 10 ? static_cast<char>('0' + d) : d == 10 ? ' ' : '?';
            }
            cs = trim(cs);
            add(g, "Radio call sign", cs);
            add(g, "Beacon number", std::string(1, baudot6(m.u(76, 6))));
            g.ident = "call sign " + cs;
            aux_field();
            break;
        }
        case 1: {   // aviation user (A2.4)
            g.protocol = "Aviation user";
            type_for(g, "ELT");
            const std::string reg = baudot_str(m, 40, 7);
            add(g, "Aircraft registration", reg);
            add(g, "ELT number", std::to_string(m.u(82, 2)));
            g.ident = "reg " + reg;
            aux_field();
            break;
        }
        case 3: {   // serial user (A2.5)
            const uint32_t bt = m.u(40, 3);
            static const char* BT[8] = {"ELT (serial number)",           "ELT (aircraft operator + serial)",
                                        "float-free EPIRB (serial)",     "ELT (aircraft 24-bit address)",
                                        "non-float-free EPIRB (serial)", "spare",
                                        "PLB (serial)",                  "spare"};
            g.protocol = std::string("Serial user: ") + BT[bt];
            type_for(g, bt == 0 || bt == 1 || bt == 3 ? "ELT" : bt == 2 || bt == 4 ? "EPIRB" : bt == 6 ? "PLB" : "unknown");
            maritime = bt == 2 || bt == 4;
            if (bt == 3) {
                const std::string a = hexv(m.u(44, 24), 6);
                add(g, "Aircraft 24-bit address", a);
                add(g, "ELT number", std::to_string(m.u(68, 6)));
                g.ident = "aircraft " + a;
            } else if (bt == 1) {
                const std::string op = baudot_str(m, 44, 3);
                add(g, "Aircraft operator", op);
                add(g, "Serial number", std::to_string(m.u(62, 12)));
                g.ident = "operator " + op + " #" + std::to_string(m.u(62, 12));
            } else {
                add(g, "Serial number", std::to_string(m.u(44, 20)));
                if (m.u(64, 10)) add(g, "National use (bits 64-73)", bin(m.u(64, 10), 10));
                g.ident = "serial " + std::to_string(m.u(44, 20));
            }
            if (m.u(43, 1)) {
                add(g, "C/S type approval certificate", std::to_string(m.u(74, 10)));
                if (bt != 1 && bt != 3) g.ident += " (TAC " + std::to_string(m.u(74, 10)) + ")";
            } else if (m.u(74, 10)) {
                add(g, "National use (bits 74-83)", bin(m.u(74, 10), 10));
            }
            aux_field();
            break;
        }
        case 7:   // test user (A2.6)
            g.protocol = "Test user";
            type_for(g, "test");
            g.test = true;
            add(g, "Test / national data (bits 40-85)", hexv(m.u(40, 22), 6) + hexv(m.u(62, 24), 6));
            break;
        case 0:   // orbitography (C/S T.006)
            g.protocol = "Orbitography";
            type_for(g, "orbitography");
            g.orbitography = true;
            add(g, "Orbitography data (bits 40-85)", hexv(m.u(40, 22), 6) + hexv(m.u(62, 24), 6));
            break;
        case 4:   // national user (A2.8)
            g.protocol = "National user";
            type_for(g, "national");
            add(g, "National use (bits 40-85)", hexv(m.u(40, 22), 6) + hexv(m.u(62, 24), 6));
            break;
        default:
            g.protocol = "User protocol 101 (reserved for second-generation beacons)";
            type_for(g, "unknown");
            break;
    }
    g.hex15 = id15(bits, 0, nullptr);
    if (!g.long_msg) {
        emergency_code(g, m, maritime);
        g.pos_text = "short message: no position";
        return;
    }
    if (g.code == 4) {
        if (pdf2) add(g, "National use (bits 107-132)", hexv(m.u(107, 26), 7));
        return;
    }
    if (g.code == 0) return;
    if (!pdf2) return;
    // user-location (A3.3.4.3): position to 4 minutes
    g.protocol += " + location";
    source_bit(g, m.u(107, 1));
    const uint32_t ld = m.u(109, 7), lm = m.u(116, 4), od = m.u(121, 8), om = m.u(129, 4);
    if (ld <= 90 && od <= 180 && lm < 15 && om < 15) {
        set_pos(g, m.u(108, 1), ld + lm * 4 / 60.0, m.u(120, 1), od + om * 4 / 60.0);
        g.pos_text = "4' steps";
        g.pos_err_m = 3700;
    } else {
        g.pos_text = "no position (default)";
    }
}

}  // namespace

std::string bits_hex(const std::vector<uint8_t>& bits, size_t a, size_t n) {
    std::string s;
    static const char* H = "0123456789ABCDEF";
    for (size_t i = 0; i + 4 <= n; i += 4) {
        int v = 0;
        for (size_t k = 0; k < 4; k++) v = (v << 1) | (a + i + k < bits.size() ? bits[a + i + k] & 1 : 0);
        s += H[v];
    }
    return s;
}

std::string fmt_latlon(double lat, double lon) {
    char b[64];
    snprintf(b, sizeof b, "%.5f %c, %.5f %c", std::fabs(lat), lat < 0 ? 'S' : 'N', std::fabs(lon), lon < 0 ? 'W' : 'E');
    return b;
}

Message parse(const std::vector<uint8_t>& bits, int pdf2_errors) {
    Message g;
    const Bits m{bits};
    g.long_msg = m.u(25, 1);
    g.user = m.u(26, 1);
    g.pdf2_errors = g.long_msg ? pdf2_errors : 0;
    const bool pdf2 = g.long_msg && pdf2_errors >= 0 && bits.size() >= 144;
    g.country = static_cast<int>(m.u(27, 10));
    if (const char* c = mid_name(g.country)) g.country_name = c;
    g.hex_msg = bits_hex(bits, 24, bits.size() >= 144 ? 120 : 88);
    if (g.user) {
        user_protocol(g, m, bits, pdf2);
        return g;
    }
    // location protocols (A3), bits 37-40
    g.code = static_cast<int>(m.u(37, 4));
    switch (g.code) {
        case 2: case 3: case 4: case 5: case 6: case 7: case 12: case 14: {   // standard location (A3.3.5)
            static const char* const N[16] = {"", "", "EPIRB (MMSI)", "ELT (24-bit address)", "ELT (serial)",
                                              "ELT (aircraft operator)", "EPIRB (serial)", "PLB (serial)", "", "", "", "",
                                              "ship security (SSAS)", "", "test", ""};
            g.protocol = std::string("Standard location: ") + N[g.code];
            if (g.code == 2 || g.code == 12) {
                char b[16];
                snprintf(b, sizeof b, "%06u", m.u(41, 20));
                add(g, "MMSI (last 6 digits)", b);
                if (g.code == 2) add(g, "Beacon number", std::to_string(m.u(61, 4)));
                g.ident = std::string("MMSI ...") + b;
                type_for(g, g.code == 2 ? "EPIRB" : "SSAS");
                g.security = g.code == 12;
            } else if (g.code == 3) {
                const std::string a = hexv(m.u(41, 24), 6);
                add(g, "Aircraft 24-bit address", a);
                g.ident = "aircraft " + a;
                type_for(g, "ELT");
            } else if (g.code == 5) {
                const std::string op = baudot5_str(m, 41, 3);
                add(g, "Aircraft operator", op);
                add(g, "Serial number", std::to_string(m.u(56, 9)));
                g.ident = "operator " + op + " #" + std::to_string(m.u(56, 9));
                type_for(g, "ELT");
            } else if (g.code == 14) {
                add(g, "Test data (bits 41-64)", hexv(m.u(41, 24), 6));
                type_for(g, "test");
                g.test = true;
            } else {
                add(g, "C/S type approval certificate", std::to_string(m.u(41, 10)));
                add(g, "Serial number", std::to_string(m.u(51, 14)));
                g.ident = "serial " + std::to_string(m.u(51, 14)) + " (TAC " + std::to_string(m.u(41, 10)) + ")";
                type_for(g, g.code == 4 ? "ELT" : g.code == 6 ? "EPIRB" : "PLB");
            }
            g.hex15 = id15(bits, 65, "011111111101111111111");
            std_position(g, m, pdf2);
            if (pdf2) {
                source_bit(g, m.u(111, 1));
                homing_bit(g, m.u(112, 1));
            }
            break;
        }
        case 8: case 10: case 11: case 15: {   // national location (A3.3.6)
            const char* t = g.code == 8 ? "ELT" : g.code == 10 ? "EPIRB" : g.code == 11 ? "PLB" : "test";
            g.protocol = std::string("National location: ") + t;
            type_for(g, t);
            g.test = g.code == 15;
            add(g, "National serial", std::to_string(m.u(41, 18)));
            g.ident = "national " + std::to_string(m.u(41, 18));
            g.hex15 = id15(bits, 59, "011111110000001111111100000");
            nat_position(g, m, pdf2);
            if (pdf2) {
                source_bit(g, m.u(111, 1));
                homing_bit(g, m.u(112, 1));
                if (!m.u(110, 1)) add(g, "National use (bits 113-126)", bin(m.u(113, 14), 14));
                if (m.u(127, 6)) add(g, "National use (bits 127-132)", bin(m.u(127, 6), 6));
            }
            break;
        }
        case 13: {   // RLS location (A3.3.7)
            const uint32_t bt = m.u(41, 2);
            static const char* const T[4] = {"ELT", "EPIRB", "PLB", "test"};
            g.protocol = std::string("RLS location: ") + (bt == 3 ? "RLS test" : T[bt]);
            type_for(g, T[bt]);
            g.test = bt == 3;
            if (m.u(43, 4) == 15) {
                char b[16];
                snprintf(b, sizeof b, "%06u", m.u(47, 20));
                add(g, "MMSI (last 6 digits)", b);
                g.ident = std::string("MMSI ...") + b;
                if (bt == 0 || bt == 1) {
                    type_for(g, "EPIRB");
                    add(g, "Beacon", bt == 0 ? "first EPIRB" : "second EPIRB");
                }
            } else {
                static const int SERIES[4] = {2000, 1000, 3000, 0};
                const uint32_t tac = m.u(43, 10);
                add(g, "C/S type approval certificate", std::to_string(SERIES[bt] + static_cast<int>(tac)) + " (field " + std::to_string(tac) + ")");
                add(g, "Serial number", std::to_string(m.u(53, 14)));
                g.ident = "serial " + std::to_string(m.u(53, 14)) + " (TAC " + std::to_string(SERIES[bt] + static_cast<int>(tac)) + ")";
            }
            g.hex15 = id15(bits, 67, "0111111110111111111");
            half_position(g, m, pdf2, true);
            if (pdf2) {
                source_bit(g, m.u(107, 1));
                homing_bit(g, m.u(108, 1));
                std::string rlm;
                if (m.u(109, 1)) rlm += "Type-1 (automatic)";
                if (m.u(110, 1)) rlm += std::string(rlm.empty() ? "" : " + ") + "Type-2 (manual)";
                add(g, "Return link messages accepted", rlm.empty() ? "none (invalid)" : rlm);
                std::string ack;
                if (m.u(111, 1)) ack += "Type-1";
                if (m.u(112, 1)) ack += std::string(ack.empty() ? "" : " + ") + "Type-2";
                add(g, "Return link acknowledged", ack.empty() ? "not yet" : ack);
                static const char* const P[4] = {"spare", "Galileo", "GLONASS", "BDS"};
                add(g, "RLS provider", P[m.u(113, 2)]);
            }
            break;
        }
        case 9: {   // ELT(DT) location (A3.3.8)
            g.protocol = "ELT(DT) location (distress tracking)";
            type_for(g, "ELT(DT)");
            const uint32_t it = m.u(41, 2);
            const uint32_t id24 = m.u(43, 24);
            if (id24 == 0 || id24 == 0xFFFFFF) {
                g.test = true;
                g.protocol += ", test";
                add(g, "Identity", "test (all " + std::string(id24 ? "1s)" : "0s)"));
            } else if (it == 0) {
                const std::string a = hexv(id24, 6);
                add(g, "Aircraft 24-bit address", a);
                g.ident = "aircraft " + a;
            } else if (it == 1) {
                const std::string op = baudot5_str(m, 43, 3);
                add(g, "Aircraft operator", op);
                add(g, "Serial number", std::to_string(m.u(58, 9)));
                g.ident = "operator " + op + " #" + std::to_string(m.u(58, 9));
            } else if (it == 2) {
                add(g, "C/S type approval certificate", std::to_string(m.u(43, 10)));
                add(g, "Serial number", std::to_string(m.u(53, 14)));
                g.ident = "serial " + std::to_string(m.u(53, 14)) + " (TAC " + std::to_string(m.u(43, 10)) + ")";
            } else {
                add(g, "Identity (type 11, reserved)", hexv(id24, 6));
            }
            g.hex15 = id15(bits, 67, "0111111110111111111");
            const uint32_t age = pdf2 ? m.u(113, 2) : 0;
            // the cancellation message (A3.3.8.5)
            if (pdf2 && m.u(67, 9) == 0x1FA && m.u(76, 10) == 0x3FA && m.u(107, 8) == 0x3C) {
                g.protocol += " - CANCELLATION";
                g.test = true;   // not an alert: the beacon cancels its earlier one
                add(g, "Message", "cancellation of a false alert");
                g.pos_text = "cancellation message";
                break;
            }
            half_position(g, m, pdf2, age != 0);
            if (pdf2) {
                static const char* const A[4] = {"manual", "automatic (by the beacon)", "automatic (by external means)", "spare"};
                add(g, "Activation", A[m.u(107, 2)]);
                static const char* const ALT[16] = {"<= 400 m", "400-800 m", "800-1200 m", "1200-1600 m", "1600-2200 m",
                                                    "2200-2800 m", "2800-3400 m", "3400-4000 m", "4000-4800 m", "4800-5600 m",
                                                    "5600-6600 m", "6600-7600 m", "7600-8800 m", "8800-10000 m", "> 10000 m",
                                                    "not available"};
                add(g, "Altitude", ALT[m.u(109, 4)]);
                static const char* const AGE[4] = {"(rotating field)", "older than 60 s or none", "2-60 s old", "<= 2 s old"};
                add(g, "Position fix", AGE[age]);
                if (age == 0) {
                    const uint32_t rt = m.u(115, 3);
                    if (rt == 0) add(g, "Aircraft operator (rotating field)", baudot5_str(m, 118, 3));
                    else add(g, "Rotating field type", std::to_string(rt) + " (spare)");
                }
            }
            break;
        }
        default:
            g.protocol = "Location protocol " + bin(static_cast<uint32_t>(g.code), 4) + " (spare)";
            type_for(g, "unknown");
            g.hex15 = id15(bits, 0, nullptr);
            break;
    }
    if (g.long_msg && pdf2_errors == -2) g.pos_text += " (truncated self-test: no PDF-2)";
    return g;
}

}  // namespace b406
