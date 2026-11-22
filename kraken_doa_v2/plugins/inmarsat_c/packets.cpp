// Inmarsat-C TDM packets: see packets.hpp
#include "packets.hpp"

#include <algorithm>
#include <cstdio>

namespace inmc {

bool fletcher_ok(const uint8_t* p, int n) {
    unsigned a = 0, b = 0;
    for (int i = 0; i < n; i++) {
        a = (a + p[i]) & 255;
        b = (b + a) & 255;
    }
    return a == 0 && b == 0;
}

void fletcher_make(const uint8_t* p, int n, uint8_t* c0, uint8_t* c1) {
    unsigned a = 0, b = 0;
    for (int i = 0; i < n; i++) {
        a = (a + p[i]) & 255;
        b = (b + a) & 255;
    }
    // a1 = a + x, b1 = b + a1; a2 = a1 + y = 0 and b2 = b1 + a2 = b1 = 0
    // -> x = -(a + b), y = -a1 = b   (mod 256)
    *c0 = static_cast<uint8_t>((512 - a - b) & 255);
    *c1 = static_cast<uint8_t>(b & 255);
}

int packet_length(const uint8_t* p, int avail) {
    if (avail < 1) return -1;
    const uint8_t t = p[0];
    if (t < 0x80) return (t & 0x0F) + 1;
    if (t < 0xC0) return avail >= 2 ? p[1] + 2 : -1;
    return avail >= 3 ? ((p[1] << 8) | p[2]) + 3 : -1;
}

WalkResult walk_frame(const uint8_t* info, int n) {
    WalkResult r;
    int i = 0;
    while (i < n) {
        if (info[i] == 0) {
            r.idle_from = i;
            break;
        }
        const int len = packet_length(info + i, n - i);
        if (len < 3 || i + len > n || !fletcher_ok(info + i, len)) {
            r.bad = true;
            r.bad_type = info[i];
            r.bad_offset = i;
            break;
        }
        r.packets.push_back({i, len, info[i]});
        i += len;
    }
    return r;
}

std::string station_id(uint8_t id) {
    char b[8];
    std::snprintf(b, sizeof b, "%d%02d", station_region(id), station_number(id));
    return b;
}

const char* region_name(int region) {
    static const char* n[4] = {"AOR-W", "AOR-E", "POR", "IOR"};
    return n[region & 3];
}

std::string freq_text(int code) {
    char b[48];
    std::snprintf(b, sizeof b, "%d (%.4f MHz)", code, downlink_mhz(code));
    return b;
}

bool parse_bulletin_board(const uint8_t* p, int n, BulletinBoard& bb) {
    if (n != 14 || p[0] != 0x7D) return false;
    bb.network_version = p[1];
    bb.frame_number = (p[2] << 8) | p[3];
    bb.b4 = p[4];
    bb.b5 = p[5];
    bb.b6 = p[6];
    bb.origin = p[7];
    bb.status = p[8];
    bb.services = static_cast<uint16_t>((p[9] << 8) | p[10]);
    bb.random_interval = p[11];
    return true;
}

std::string frame_time(int frame_number) {
    const long ms = static_cast<long>(frame_number) * 8640;
    const long s = ms / 1000;
    char b[24];
    std::snprintf(b, sizeof b, "%02ld:%02ld:%02ld UTC", (s / 3600) % 24, (s / 60) % 60, s % 60);
    return b;
}

bool parse_sig_descriptor(const uint8_t* p, int n, SigChannelDescriptor& sd) {
    if (n != 13 || p[0] != 0x6C) return false;
    sd.flags = p[1];
    sd.uplink_code = (p[2] << 8) | p[3];
    for (int k = 0; k < 4; k++) sd.slots[k] = 0;
    for (int i = 0; i < 7; i++)
        for (int s = 3; s >= 0; s--) sd.slots[(p[4 + i] >> (2 * s)) & 3]++;
    return true;
}

static bool plausible_code(int c) { return c >= 6000 && c <= 14000; }

bool parse_0x81(const uint8_t* p, int n, LesTdmRef& r) {
    if (p[0] != 0x81 || n != 19) return false;
    r.mes = (static_cast<uint32_t>(p[2]) << 16) | (p[3] << 8) | p[4];
    r.les = p[5];
    r.tdm_code = (p[6] << 8) | p[7];
    r.counter = (p[8] << 8) | p[9];
    return plausible_code(r.tdm_code) && !station_is_ncs(r.les);
}

bool parse_station_list(const uint8_t* p, int n, std::vector<LesListEntry>& out, int* network_version) {
    out.clear();
    int h;
    if (p[0] == 0x92) h = 9;
    else if (p[0] == 0xAB) h = 4;
    else return false;
    if (n < h + 2) return false;
    const int count = p[h - 1];
    if (count < 1 || n != h + 6 * count + 2) return false;
    if (network_version) *network_version = p[h - 2];
    for (int i = h; i + 6 <= n - 2; i += 6) {
        LesListEntry e;
        e.les = p[i];
        e.status = (static_cast<uint32_t>(p[i + 1]) << 16) | (p[i + 2] << 8) | p[i + 3];
        e.tdm_code = (p[i + 4] << 8) | p[i + 5];
        if (plausible_code(e.tdm_code)) out.push_back(e);   // (an entry with code 0: a station without a TDM)
    }
    return true;
}

// ---- EGC -------------------------------------------------------------------------
bool is_egc_type(uint8_t t) { return t == 0xB1 || t == 0xB2; }

bool parse_egc_header(const uint8_t* p, int n, EgcHeader& h) {
    if (!is_egc_type(p[0]) || n < 13) return false;
    h.type = p[0];
    h.service = p[2];
    h.flags = p[3];
    h.more = (p[3] & 0x80) != 0;
    h.p_bit = (p[3] >> 6) & 1;
    h.repetition = p[3] & 0x3F;
    h.msn = (p[4] << 8) | p[5];
    h.packet_seq = p[6];
    h.presentation = p[7];
    h.les = p[8];
    const int alen = h.service & 0x0F;
    auto try_len = [&](int a) {
        const int hl = 9 + a + 2;
        return hl <= n - 2 && fletcher_ok(p, hl);
    };
    int a = -1;
    if (try_len(alen)) a = alen;
    else
        for (int k = 0; k <= 8 && a < 0; k++)
            if (k != alen && try_len(k)) a = k;
    if (a < 0) return false;
    h.address_rule = a == alen;
    h.address.assign(p + 9, p + 9 + a);
    h.header_len = 9 + a + 2;
    return true;
}

const char* egc_service_name(uint8_t s) {
    switch (s) {
    case 0x00: return "General call (all ships)";
    case 0x02: return "Group call";
    case 0x04: return "Urgency / navigational warning, rectangular area";
    case 0x11: return "Inmarsat system message";
    case 0x13: return "Coastal warning";
    case 0x14: return "Shore-to-ship distress alert, circular area";
    case 0x23: return "EGC system message";
    case 0x24: return "Urgency / MET / NAV warning, circular area";
    case 0x31: return "MET / NAVAREA warning or MET forecast";
    case 0x33: return "Download group identity";
    case 0x34: return "SAR coordination, rectangular area";
    case 0x44: return "SAR coordination, circular area";
    case 0x72: return "Chart correction service";
    case 0x73: return "Chart correction service, fixed areas";
    default: return nullptr;
    }
}

const char* egc_service_family(uint8_t s) {
    switch (s) {
    case 0x00: case 0x04: case 0x13: case 0x14: case 0x24: case 0x31: case 0x34: case 0x44: return "SafetyNET";
    case 0x02: case 0x72: case 0x73: case 0x33: return "FleetNET";
    case 0x11: case 0x23: return "System";
    default: return "EGC";
    }
}

std::string hex(const uint8_t* p, int n) {
    std::string s;
    char b[4];
    for (int i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, i ? " %02X" : "%02X", p[i]);
        s += b;
    }
    return s;
}

static char ia5_letter(uint8_t b) {
    const uint8_t c = b & 0x7F;
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? static_cast<char>(c) : '?';
}

std::string egc_address_text(uint8_t s, const std::vector<uint8_t>& a) {
    char b[96];
    const int n = static_cast<int>(a.size());
    const std::string raw = hex(a.data(), n);
    switch (s) {
    case 0x00:
        return n == 0 ? "all ships" : "all ships (" + raw + ")";
    case 0x31:
        if (n == 1) {
            std::snprintf(b, sizeof b, "NAVAREA / METAREA %d", a[0]);
            return b;
        }
        break;
    case 0x02:
        if (n == 2) {
            std::snprintf(b, sizeof b, "group %d", (a[0] << 8) | a[1]);
            return b;
        }
        break;
    case 0x13:
        if (n == 3) {
            std::snprintf(b, sizeof b, "coastal: NAVAREA %d, station %c, subject %c (%s)", a[0], ia5_letter(a[1]),
                          ia5_letter(a[2]), raw.c_str());
            return b;
        }
        break;
    case 0x23: case 0x33:
        if (n == 3) {
            std::snprintf(b, sizeof b, "MES %02X%02X%02X", a[0], a[1], a[2]);
            return b;
        }
        break;
    case 0x72: case 0x73: {
        unsigned v = 0;
        for (uint8_t x : a) v = (v << 8) | x;
        std::snprintf(b, sizeof b, "chart area %u", v);
        return b;
    }
    case 0x04: case 0x34:
        return "rectangular area (" + raw + ", layout not decoded)";
    case 0x14: case 0x24: case 0x44:
        return "circular area (" + raw + ", layout not decoded)";
    default: break;
    }
    return n ? raw : "-";
}

const char* presentation_name(uint8_t p) {
    switch (p) {
    case 0x00: return "IA5";
    case 0x06: return "ITA2";
    case 0x07: return "data";
    default: return "other";
    }
}

std::string ia5_text(const std::vector<uint8_t>& b, int* errors) {
    std::string s;
    int err = 0;
    for (uint8_t x : b) {
        const bool odd = __builtin_popcount(x) & 1;
        const uint8_t c = x & 0x7F;
        if (!odd) {
            err++;
            s += '_';
        } else if (c == '\r') {
            // CR LF / LF CR / lone CR -> one newline
            if (s.empty() || s.back() != '\n') s += '\n';
        } else if (c == '\n') {
            if (s.empty() || s.back() != '\n') s += '\n';
        } else if (c >= 32 && c < 127) {
            s += static_cast<char>(c);
        }   // other control characters (NUL fill, ETX, ...) dropped
    }
    if (errors) *errors += err;
    return s;
}

std::string ita2_text(const std::vector<uint8_t>& b) {
    // ITU-T S.1 (ITA2), first bit = LSB. One character per byte (low 5 bits).
    static const char* L = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x01MXV\x02";
    static const char* F = "\0" "3\n- '87\r\x05" "4\x07,!:(5+)2#6019?&\x01./=\x02";
    std::string s;
    bool figs = false;
    for (uint8_t x : b) {
        const int c = x & 31;
        const char ch = (figs ? F : L)[c];
        if (ch == '\x01') figs = true;
        else if (ch == '\x02') figs = false;
        else if (ch == '\r' || ch == '\n') {
            if (s.empty() || s.back() != '\n') s += '\n';
        } else if (ch >= 32) s += ch;
    }
    return s;
}

bool EgcMessage::complete() const {
    if (last_seq <= 0) return false;
    for (int s = 1; s <= last_seq; s++) {
        auto it = have.find(s);
        if (it == have.end()) return false;
        const bool dbl = double_header.at(s);
        if (!it->second[0] || (dbl && !it->second[1])) return false;
    }
    return true;
}

std::vector<int> EgcMessage::missing() const {
    std::vector<int> m;
    int top = last_seq;
    if (!have.empty()) top = std::max(top, have.rbegin()->first);
    for (int s = 1; s <= top; s++) {
        auto it = have.find(s);
        if (it == have.end() || !it->second[0] || (double_header.at(s) && !it->second[1])) m.push_back(s);
    }
    return m;
}

int EgcMessage::body_bytes() const {
    int n = 0;
    for (const auto& [s, v] : parts)
        for (const auto& b : v) n += static_cast<int>(b.size());
    return n;
}

std::string EgcMessage::text(int* errors) const {
    std::string out;
    if (errors) *errors = 0;
    int top = last_seq;
    if (!have.empty()) top = std::max(top, have.rbegin()->first);
    std::vector<uint8_t> run;   // consecutive bytes decoded together (ITA2 shifts carry over)
    auto flush = [&]() {
        if (run.empty()) return;
        if (presentation == 0x00) out += ia5_text(run, errors);
        else if (presentation == 0x06) out += ita2_text(run);
        else out += hex(run.data(), static_cast<int>(run.size())) + " ";
        run.clear();
    };
    for (int s = 1; s <= top; s++) {
        auto it = have.find(s);
        const int np = (it != have.end() && double_header.at(s)) ? 2 : 1;
        for (int k = 0; k < np; k++) {
            if (it != have.end() && it->second[k]) {
                const auto& b = parts.at(s)[k];
                run.insert(run.end(), b.begin(), b.end());
            } else {
                flush();
                out += "[...]";
            }
        }
    }
    flush();
    if (!complete() && last_seq == 0) out += "[...]";
    return out;
}

EgcMessage* EgcAssembler::add(const EgcHeader& h, const uint8_t* body, int blen, double t, bool* fresh) {
    char k[40];
    std::snprintf(k, sizeof k, "%s-%02X-%d", station_id(h.les).c_str(), h.service, h.msn);
    auto [it, inserted] = msgs_.try_emplace(k);
    EgcMessage& m = it->second;
    if (inserted) {
        m.key = k;
        m.les = h.les;
        m.service = h.service;
        m.msn = h.msn;
        m.presentation = h.presentation;
        m.address = h.address;
        m.flags = h.flags;
        m.first_t = t;
    }
    m.last_t = t;
    m.receptions++;
    const int seq = h.packet_seq;
    const int part = h.type == 0xB2 ? 1 : 0;
    auto& hv = m.have[seq];
    if (hv.empty()) {
        hv.assign(2, false);
        m.parts[seq].assign(2, {});
    }
    m.double_header[seq] = true;   // 0xB1 / 0xB2 are the two halves of one block
    if (!h.more) m.last_seq = std::max(m.last_seq, seq);
    if (hv[part]) {
        m.repeats++;
        *fresh = false;
    } else {
        hv[part] = true;
        m.parts[seq][part].assign(body, body + blen);
        *fresh = true;
    }
    return &m;
}

std::vector<EgcMessage*> EgcAssembler::stale(double now, double timeout_s) {
    std::vector<EgcMessage*> v;
    for (auto& [k, m] : msgs_)
        if (!m.reported && now - m.last_t > timeout_s) v.push_back(&m);
    std::sort(v.begin(), v.end(), [](const EgcMessage* a, const EgcMessage* b) { return a->first_t < b->first_t; });
    return v;
}

void EgcAssembler::prune(double now, double keep_s) {
    for (auto it = msgs_.begin(); it != msgs_.end();)
        if (now - it->second.last_t > keep_s) it = msgs_.erase(it);
        else ++it;
    while (msgs_.size() > 1000) {   // memory bound: drop the oldest
        auto old = msgs_.begin();
        for (auto it = msgs_.begin(); it != msgs_.end(); ++it)
            if (it->second.last_t < old->second.last_t) old = it;
        msgs_.erase(old);
    }
}

}  // namespace inmc
