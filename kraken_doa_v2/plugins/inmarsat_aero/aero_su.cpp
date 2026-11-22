#include "aero_su.hpp"

#include "acars_msg.hpp"
#include "adsc.hpp"
#include "aero_fec.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aero {

namespace {

uint16_t crc_kermit(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0x8408) : static_cast<uint16_t>(c >> 1);
    }
    return c;
}

uint32_t be24(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2]; }
int be16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

const char* rate_name(int code) {
    static const char* n[16] = {"600", "1200", "2400", "4800", "6000", "5250", "10500", "8400",
                                "?",   "21000", "?",   "?",    "?",    "?",    "?",     "?"};
    return n[code & 15];
}

const char* q_name(int q) {
    // Q numbers (precedence), appendix 3 item 43
    if (q == 15) return "distress";
    if (q == 14) return "distress data";
    if (q == 13) return "safety signalling";
    if (q == 12) return "flight safety";
    if (q == 11) return "flight safety data";
    if (q == 10) return "other safety";
    if (q == 9) return "non-safety";
    if (q >= 5 && q <= 8) return "safety data";
    return "non-safety data";
}

const char* logon_reason(int r) {
    static const char* n[16] = {"log-on table full",
                                "voice channel characteristic not available",
                                "beam not served",
                                "fixed network failure",
                                "unassigned",
                                "invalid satellite ID",
                                "invalid GES ID",
                                "P/R/T channels not available",
                                "packet-mode data services not available",
                                "reserved",
                                "voice and data services not available",
                                "unassigned",
                                "unassigned",
                                "unassigned",
                                "other reason",
                                "AES not authorized"};
    return n[r & 15];
}

const char* voice_ch(int v) {
    switch (v) {
    case 2: return "9.6 kbit/s LPC, 21 kbit/s channel";
    case 3: return "4.8 kbit/s AMBE, 8.4 kbit/s channel";
    case 4: return "LPC 21 kbit/s and AMBE 8.4 kbit/s";
    default: return nullptr;
    }
}

}  // namespace

std::string aes_hex(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}

std::string freq_str(int code, bool transmit) {
    if (code == 0) return "-";
    const int nn = code & 0x7FFF;
    const double f = (transmit ? 1611.5 : 1510.0) + nn * 0.0025;
    char b[48];
    snprintf(b, sizeof b, "%.4f MHz%s", f, (code & 0x8000) ? " (spot)" : "");
    return b;
}

SuDecoder::SuDecoder(kp::Host& h) : host(h) {
    host.table_columns({"Aircraft", "Registration", "Flight", "Label", "Messages", "Last"});
    host.table_keep(6 * 3600);
}

void SuDecoder::reset() {
    su_ok_ = su_total_ = fill_ = 0;
    type_count_.clear();
    st_ = Stats{};
    ges_ = -1;
    sats_.clear();
    call_aes_.clear();
    psmc_.clear();
    asm_.clear();
    air_.clear();
    multi_.clear();
    last_facts_ = -100;
}

void SuDecoder::option(const std::string& k, const std::string& v) {
    (void)k;
    (void)v;
}

std::string SuDecoder::aes_str(uint32_t aes) const { return aes_hex(aes); }

void SuDecoder::frame_done(int sus, int sus_ok) {
    su_total_ += static_cast<uint64_t>(sus);
    (void)sus_ok;
    facts(false);
}

void SuDecoder::facts(bool force) {
    const double t = host.time();
    if (!force && t - last_facts_ < 1.0) return;
    last_facts_ = t;
    char b[160];
    snprintf(b, sizeof b, "%llu of %llu passed the CRC (%llu fill-in)", static_cast<unsigned long long>(su_ok_),
             static_cast<unsigned long long>(su_total_), static_cast<unsigned long long>(fill_));
    host.fact("Signal units", b);
    if (ges_ >= 0) {
        snprintf(b, sizeof b, "0x%02X (%d)", ges_, ges_);
        host.fact("GES", b);
    }
    if (!sats_.empty()) {
        // the table, and the satellite whose entry names this P channel (the
        // nearest within 2.5 kHz - half the 5 kHz raster; an RTL-SDR at 1.5 GHz
        // is off by 1 - 2 kHz)
        auto lon_str = [](double lon) {
            char s[24];
            snprintf(s, sizeof s, "%.1f deg %s", lon > 180 ? 360 - lon : lon, lon > 180 ? "W" : "E");
            return std::string(s);
        };
        std::string all;
        int best = -1;
        double bd = 2500;
        for (const auto& [id, e] : sats_) {
            all += (all.empty() ? "" : ", ") + std::to_string(id) + ": " + lon_str(e.lon);
            for (double f : {e.psmc1, e.psmc2})
                if (chan_hz_ > 0 && f > 0 && std::fabs(f - chan_hz_) < bd) bd = std::fabs(f - chan_hz_), best = id;
        }
        host.fact("Satellites (system table)", all);
        if (best >= 0) {
            const SatEntry& e = sats_[best];
            const double pf = std::fabs(e.psmc1 - chan_hz_) <= std::fabs(e.psmc2 - chan_hz_) ? e.psmc1 : e.psmc2;
            snprintf(b, sizeof b, "ID %d, %s (its P channel %.4f MHz; received %+.2f kHz from it)", best, lon_str(e.lon).c_str(),
                     pf / 1e6, (chan_hz_ - pf) / 1e3);
            host.fact("Satellite", b);
        }
    }
    if (st_.user_msgs) {
        snprintf(b, sizeof b, "%llu user data messages, %llu ACARS (%llu with a bad BCS)",
                 static_cast<unsigned long long>(st_.user_msgs), static_cast<unsigned long long>(st_.acars),
                 static_cast<unsigned long long>(st_.acars_bad_bcs));
        host.fact("User data", b);
    }
    if (!air_.empty()) host.fact("Aircraft heard", std::to_string(air_.size()));
    std::string types;
    for (const auto& kv : type_count_) {
        if (types.size() > 300) break;
        char e[24];
        snprintf(e, sizeof e, "%s%02X:%llu", types.empty() ? "" : " ", kv.first, static_cast<unsigned long long>(kv.second));
        types += e;
    }
    if (!types.empty()) host.fact("SU types", types);
}

void SuDecoder::row(uint32_t aes) {
    Air& a = air_[aes];
    const std::string key = aes_hex(aes);
    host.table_row(key, {key, a.reg, a.flight, a.label, std::to_string(a.msgs), a.text.substr(0, 200)});
}

void SuDecoder::p_su(const uint8_t* su, double t0, double t1, int index) {
    (void)index;
    su_ok_++;
    const int type = su[0];
    const bool ssu = (type & 0xC0) == 0xC0;
    type_count_[ssu ? 0xC0 : type]++;
    if (type == 0x01) {
        fill_++;
        return;
    }
    char b[400];
    const bool v = host.verbose();
    if (ssu) {
        // a subsequent signal unit: continues the ISU with the same Q / ref
        auto it = asm_.find(asm_key(su[1]));
        if (it == asm_.end()) return;
        Asm& a = it->second;
        const int seq = type & 0x3F;
        if (seq != a.next) {   // out of order / missed one: drop the set
            asm_.erase(it);
            return;
        }
        const int n = seq == 0 ? std::min(a.last, 8) : 8;
        a.data.insert(a.data.end(), su + 2, su + 2 + n);
        a.t1 = t1;
        if (seq == 0) {
            const Asm done = a;
            asm_.erase(it);
            user_data(done);
        } else {
            a.next = seq - 1;
        }
        return;
    }
    switch (type) {
    case 0x02: case 0x05: case 0x0D: {   // GES P / R channel advice (A2-31)
        const int ges = su[3];
        ges_ = ges;
        const int lsu = su[2] & 3;
        std::string s = std::string(lsu <= 1 ? "Psmc " + freq_str(be16(su + 4), false) + ", Rsmc0 " + freq_str(be16(su + 6), true) +
                                                   ", Rsmc1 " + freq_str(be16(su + 8), true)
                                             : "Rsmc " + freq_str(be16(su + 4), true) + ", " + freq_str(be16(su + 6), true) + ", " +
                                                   freq_str(be16(su + 8), true));
        char k[40];
        snprintf(k, sizeof k, "GES 0x%02X channels %d", ges, lsu <= 1 ? 0 : lsu - 1);
        host.fact(k, s);
        break;
    }
    case 0x09: {   // system broadcast: universal time (A2-35), BCD
        ges_ = su[1];
        auto bcd = [](uint8_t x) { return (x >> 4) * 10 + (x & 15); };
        snprintf(b, sizeof b, "%02d%02d-%02d-%02d %02d:%02d:%02d UTC", bcd(su[3]), bcd(su[4]), bcd(su[5]), bcd(su[6]), bcd(su[7]),
                 bcd(su[8]), bcd(su[9]));
        host.fact("GES time", b);
        break;
    }
    case 0x0B: case 0x0C: {   // satellite identification (A2-32)
        // one SU per satellite of the system, the table broadcast in turn
        // (off air, I-4 F1: IDs 1 - 7 one after the other, octet 2 counting
        // down) - not the satellite this channel comes from
        const int sat = ((su[2] & 3) << 4) | (su[3] >> 4);
        auto hz = [](int code) { return code & 0x7FFF ? (1510.0 + (code & 0x7FFF) * 0.0025) * 1e6 : 0.0; };
        sats_[sat] = {su[5] * 1.5, hz(be16(su + 6)), hz(be16(su + 8))};
        break;
    }
    case 0x0A: case 0x26: case 0x03: case 0x04: case 0x06: case 0x07: case 0x08: case 0x28: case 0x29:
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88:
        break;   // system tables: counted (SU types fact)
    case 0x11: {   // log-on confirm (A2-3)
        const uint32_t aes = be24(su + 1);
        ges_ = su[4];
        const int vc = su[8] & 0x3F;
        const char* vn = voice_ch(vc);
        snprintf(b, sizeof b, "Log-on confirm: AES %s, GES 0x%02X, satellite %d, beam %d%s%s", aes_hex(aes).c_str(), su[4],
                 su[6] >> 2, ((su[6] & 3) << 4) | (su[7] >> 4), vn ? ", voice " : "", vn ? vn : "");
        host.event(b, 60);
        air_[aes];
        row(aes);
        break;
    }
    case 0x12: case 0x13: case 0x14: case 0x16: case 0x17: {   // log control (A2-4 / A2-5 / A2-6)
        const uint32_t aes = be24(su + 1);
        const char* what = type == 0x12   ? "log-off request"
                           : type == 0x13 ? "log-on reject"
                           : type == 0x14 ? "log-on interrogation"
                           : type == 0x16 ? "log-on prompt"
                                          : "data channel reassignment";
        snprintf(b, sizeof b, "Log control: %s, AES %s, GES 0x%02X%s%s", what, aes_hex(aes).c_str(), su[4],
                 type == 0x13 ? " - " : "", type == 0x13 ? logon_reason(su[6] >> 4) : "");
        host.event(b, 60);
        air_[aes];
        row(aes);
        break;
    }
    case 0x15: {   // log-on / log-off acknowledge (A2-25)
        const uint32_t aes = be24(su + 1);
        snprintf(b, sizeof b, "Log-on/log-off acknowledge: AES %s, GES 0x%02X", aes_hex(aes).c_str(), su[4]);
        host.event(b, 60);
        air_[aes];
        row(aes);
        break;
    }
    case 0x20: {   // call announcement (A2-8)
        const uint32_t aes = be24(su + 1);
        const int q = su[5] >> 4;
        snprintf(b, sizeof b, "Call announcement (%s): AES %s, GES 0x%02X, service %d", q_name(q), aes_hex(aes).c_str(), su[4],
                 su[6] & 15);
        host.event(b, 30);
        air_[aes];
        row(aes);
        break;
    }
    case 0x30: {   // call progress (A2-11 .. A2-16)
        const uint32_t aes = be24(su + 1);
        static const char* rt[8] = {"", "channel status report", "connect", "test", "call attempt result", "channel release", "", ""};
        const int r = su[6] & 15;
        snprintf(b, sizeof b, "Call progress: AES %s, GES 0x%02X, %s", aes_hex(aes).c_str(), su[4], r < 8 && *rt[r] ? rt[r] : "report");
        host.event(b, 30);
        break;
    }
    case 0x31: case 0x32: case 0x33: case 0x34: {   // C channel assignment (A2-17)
        const uint32_t aes = be24(su + 1);
        static const char* pr[4] = {"distress", "flight safety", "other safety", "non-safety"};
        snprintf(b, sizeof b, "C channel assignment (%s): AES %s, GES 0x%02X, to aircraft %s, from aircraft %s", pr[type - 0x31],
                 aes_hex(aes).c_str(), su[4], freq_str(be16(su + 6), false).c_str(), freq_str(be16(su + 8), true).c_str());
        host.event(b, 30);
        Air& a = air_[aes];
        a.label = "C channel";
        row(aes);
        break;
    }
    case 0x40: case 0x41: {   // P/R or T channel control (ISU)
        if (v) {
            snprintf(b, sizeof b, "%s channel control: AES %s, GES 0x%02X, %s, %s bit/s", type == 0x40 ? "P/R" : "T",
                     aes_hex(be24(su + 1)).c_str(), su[4], freq_str(be16(su + 8), type == 0x41).c_str(),
                     rate_name(type == 0x40 ? su[7] >> 4 : su[7] & 15));
            host.event(b, 30);
        }
        break;
    }
    case 0x50: case 0x51: case 0x53: {   // TDMA reservation (A2-22 / A2-23 / A2-24)
        if (v) {
            const uint32_t aes = be24(su + 1);
            if (type == 0x53) {
                snprintf(b, sizeof b, "Reservation forthcoming: AES %s, GES 0x%02X", aes_hex(aes).c_str(), su[4]);
            } else {
                snprintf(b, sizeof b, "T channel %s: AES %s, GES 0x%02X, %d burst(s) of %d SUs, slot %d, frame %d, channel %d",
                         type == 0x50 ? "unsolicited reservation" : "assignment", aes_hex(aes).c_str(), su[4], (su[7] >> 5) & 7,
                         su[7] & 31, su[8] >> 2, su[9] & 15, su[8] & 3);
            }
            host.event(b, 10);
        }
        break;
    }
    case 0x60: case 0x61: case 0x62: case 0x72: {   // acknowledgements, request for ack, RTX header
        if (v) {
            const char* w = type == 0x60 ? "Telephony acknowledge" : type == 0x61 ? "Request for acknowledgement" : type == 0x62 ? "Acknowledgement" : "Retransmission header";
            snprintf(b, sizeof b, "%s: AES %s, GES 0x%02X, Q %d ref %d", w, aes_hex(be24(su + 1)).c_str(), su[4], su[5] >> 4,
                     su[5] & 15);
            host.event(b, 10);
        }
        break;
    }
    case 0x71: {   // user data ISU (A2-37)
        Asm a;
        a.aes = be24(su + 1);
        a.ges = su[4];
        a.q = su[5] >> 4;
        a.ref = su[5] & 15;
        a.nss = su[6] & 0x3F;
        a.last = su[7] >> 4;
        a.next = a.nss - 1;
        a.data.assign(su + 8, su + 10);
        a.t0 = t0;
        a.t1 = t1;
        a.down = down_ctx_;
        if (a.nss == 0) {
            a.data.resize(std::min<size_t>(a.data.size(), static_cast<size_t>(std::max(a.last, 0))));
            user_data(a);
        } else {
            asm_[asm_key(su[5])] = a;
        }
        break;
    }
    case 0x74: case 0x76: {   // user data, 3 / 4 octet LSDU (A2-39 / A2-40)
        Asm a;
        a.aes = be24(su + 1);
        a.ges = su[4];
        a.q = su[5] >> 4;
        a.ref = su[5] & 15;
        a.data.assign(su + 6, su + (type == 0x74 ? 9 : 10));
        a.t0 = t0;
        a.t1 = t1;
        a.down = down_ctx_;
        user_data(a);
        break;
    }
    default:
        if (v) {
            snprintf(b, sizeof b, "Signal unit type 0x%02X", type);
            host.event(b, 30);
        }
        break;
    }
    if (host.raw_wanted()) {
        std::string r = "P SU";
        for (int i = 0; i < 12; i++) {
            char h[4];
            snprintf(h, sizeof h, " %02X", su[i]);
            r += h;
        }
        host.raw(r);
    }
}

void SuDecoder::user_data(const Asm& a) {
    st_.user_msgs++;
    const uint8_t* d = a.data.data();
    const size_t n = a.data.size();
    Air& air = air_[a.aes];
    air.msgs++;
    if (n >= 3 + 13 + 3 && d[0] == 0xFF && d[1] == 0xFF && (d[2] & 0x7F) == 0x01) {
        acars(a, d, n);
        return;
    }
    // other user data (Data-3: X.25 / ISO 8208 packets carrying ATN):
    // the packet header only - GFI, logical channel, packet type
    char b[240];
    std::string x25;
    bool notable = false;   // call set-up / clearing / resets: shown without "Show all messages"
    if (n >= 3 && ((d[0] >> 4) == 1 || (d[0] >> 4) == 2)) {   // GFI: modulo 8 or 128, Q = D = 0
        const int lcn = ((d[0] & 15) << 8) | d[1], pt = d[2];
        const char* t = (pt & 1) == 0                 ? "data"
                        : pt == 0x0B                  ? "call request"
                        : pt == 0x0F                  ? "call accepted"
                        : pt == 0x13                  ? "clear request"
                        : pt == 0x17                  ? "clear confirmation"
                        : pt == 0x23                  ? "interrupt"
                        : pt == 0x27                  ? "interrupt confirmation"
                        : (pt & 0x1F) == 0x01         ? "receive ready"
                        : (pt & 0x1F) == 0x05         ? "receive not ready"
                        : (pt & 0x1F) == 0x09         ? "reject"
                        : pt == 0x1B                  ? "reset request"
                        : pt == 0x1F                  ? "reset confirmation"
                        : pt == 0xFB                  ? "restart request"
                        : pt == 0xFF                  ? "restart confirmation"
                        : pt == 0xF1                  ? "diagnostic"
                                                      : nullptr;
        if (t) {
            char e[96];
            snprintf(e, sizeof e, " · X.25 %s, LCN %d", t, lcn);
            x25 = e;
            notable = (pt & 1) && (pt & 0x1F) != 0x01 && (pt & 0x1F) != 0x05;
        }
    }
    // the first octets (a header) in hex, for the protocol's sake
    std::string head;
    for (size_t i = 0; i < n && i < 4; i++) {
        char h[4];
        snprintf(h, sizeof h, "%s%02X", i ? " " : "", d[i]);
        head += h;
    }
    snprintf(b, sizeof b, "%s AES %s · GES 0x%02X · %s user data, %zu octets [%s ...]%s", a.down ? "↓" : "↑",
             aes_hex(a.aes).c_str(), a.ges, q_name(a.q), n, head.c_str(), x25.c_str());
    if (host.verbose() || notable) host.event(b, 30);
    air.label = "data";
    row(a.aes);
}

void SuDecoder::acars(const Asm& a, const uint8_t* d, size_t n) {
    // FF FF SOH | mode .. suffix | BCS (2, low first) | DEL
    size_t end = n;
    while (end > 0 && (d[end - 1] & 0x7F) == 0x7F) end--;   // DEL (and any padding)
    if (end < 3 + 13 + 2) return;
    const size_t body = 3, blen = end - 2 - body;   // mode .. suffix
    const uint16_t bcs = static_cast<uint16_t>(d[end - 2] | (d[end - 1] << 8));
    if (crc_kermit(d + body, blen) != bcs) {
        st_.acars_bad_bcs++;
        return;
    }
    acars::Block blk;
    if (!acars::parse(d + body, static_cast<int>(blen), blk)) return;
    st_.acars++;
    // the downlink / uplink: from the channel (P = ground to air), not the block ID
    const bool down = a.down;
    const std::string reg = acars::registration(blk.addr);
    const std::string lab = acars::label_text(blk.label);
    const char* lname = acars::label_name(blk.label);
    std::string msn, flight, text = blk.text;
    if (down) acars::split_downlink(blk.text, msn, flight, text);
    Air& air = air_[a.aes];
    if (!reg.empty()) air.reg = reg;
    if (!flight.empty()) air.flight = kp::printable(flight);
    air.label = lab;
    const std::string one = acars::one_line(text);
    if (!one.empty()) air.text = one;
    std::string ev = std::string(down ? "↓ " : "↑ ") + (reg.empty() ? aes_hex(a.aes) : reg + " (" + aes_hex(a.aes) + ")");
    if (!flight.empty()) ev += " " + kp::printable(flight);
    ev += " · " + lab + (*lname ? std::string(" ") + lname : "");
    if (blk.bid > ' ' && blk.bid < 0x7F) ev += std::string(" · blk ") + blk.bid;
    if (!msn.empty()) ev += " · " + kp::printable(msn);
    if (!one.empty()) ev += ": " + one;
    if (blk.etb) ev += " …";
    if (!one.empty() || host.verbose()) host.event(ev, 30);
    // a message of several blocks (ETB on all but the last): put together
    // and reported whole with its last block (as plugins/acars)
    {
        const double now = host.time();
        for (auto it = multi_.begin(); it != multi_.end();)
            it = now - it->second.t > 300 ? multi_.erase(it) : std::next(it);
        const std::string k = std::string(down ? "D" : "U") + aes_hex(a.aes) + blk.label;
        auto it = multi_.find(k);
        if (blk.etb) {
            Multi& m = multi_[k];
            if (m.blocks == 0 || blk.bid != m.last_bid) {   // a retransmitted block once
                m.text += text;
                m.blocks++;
                m.last_bid = blk.bid;
            }
            m.t = now;
        } else if (it != multi_.end() && blk.bid != it->second.last_bid) {
            const Multi m = it->second;
            multi_.erase(it);
            host.event(std::string(down ? "↓ " : "↑ ") + (reg.empty() ? aes_hex(a.aes) : reg) + " · " + lab + " · message of " +
                           std::to_string(m.blocks + 1) + " blocks: " + acars::one_line(m.text + text),
                       30);
        }
    }
    if (host.raw_wanted()) {
        char h[64];
        snprintf(h, sizeof h, "ACARS %s AES %s GES %02X ", down ? "DL" : "UL", aes_hex(a.aes).c_str(), a.ges);
        host.raw(std::string(h) + kp::printable(blk.addr) + " " + lab + " " + kp::printable(std::string(1, blk.bid)) + " " +
                 kp::printable(blk.text));
    }
    host.fact("Last ACARS", (reg.empty() ? aes_hex(a.aes) : reg) + " · " + lab + (one.empty() ? "" : ": " + one.substr(0, 160)));
    // ADS-C (ARINC 622 / 745) inside the text
    AdscReport r;
    if (adsc_decode(blk.label, blk.text, r)) {
        st_.adsc++;
        if (r.has_pos) {
            air.lat = r.lat;
            air.lon = r.lon;
            air.alt = r.alt_ft;
            kp::MapPoint p;
            p.id = aes_hex(a.aes);
            p.lat = r.lat;
            p.lon = r.lon;
            p.label = !air.flight.empty() ? air.flight : !air.reg.empty() ? air.reg : p.id;
            p.kind = "aircraft";
            p.heading = r.track;
            p.altitude_m = std::isfinite(r.alt_ft) ? r.alt_ft * 0.3048 : NAN;
            p.speed_kmh = std::isfinite(r.gs_kt) ? r.gs_kt * 1.852 : NAN;
            p.info = "AES: " + aes_hex(a.aes) + (air.reg.empty() ? "" : "\nRegistration: " + air.reg) +
                     (air.flight.empty() ? "" : "\nFlight: " + air.flight) + "\nSource: ADS-C" + r.info;
            p.ttl_s = 3600;
            host.map_point(p);
        }
        if (!r.summary.empty()) host.event(std::string("ADS-C ") + (reg.empty() ? aes_hex(a.aes) : reg) + ": " + r.summary, 30);
    }
    row(a.aes);
}

void SuDecoder::c_su(const uint8_t* su, double t0, double t1) {
    (void)t0;
    (void)t1;
    su_ok_++;
    const int type = su[0];
    const bool ssu = (type & 0xC0) == 0xC0;
    type_count_[ssu ? 0xC0 : type]++;
    if (type == 0x01) {
        fill_++;
        return;
    }
    if (ssu) return;   // service address digits (dialled numbers are not shown)
    const uint32_t aes = be24(su + 1);
    char b[300];
    switch (type) {
    case 0x30: {   // call progress (A2-11 .. A2-16)
        static const char* rt[8] = {"report", "channel status report", "connect", "test", "call attempt result", "channel release", "report", "report"};
        const int r = su[6] & 15;
        std::string cause;
        if (r == 4 || r == 5) {
            char c[48];
            snprintf(c, sizeof c, ", cause %d/%d", (su[7] >> 4) & 7, su[7] & 15);
            cause = c;
        }
        snprintf(b, sizeof b, "C channel: call progress, AES %s, GES 0x%02X, %s%s", aes_hex(aes).c_str(), su[4], rt[r & 7],
                 cause.c_str());
        host.event(b, 20);
        call_aes_ = aes_hex(aes);
        ges_ = su[4];
        break;
    }
    case 0x60:   // telephony acknowledge (A2-26)
        snprintf(b, sizeof b, "C channel: telephony acknowledge, AES %s, GES 0x%02X", aes_hex(aes).c_str(), su[4]);
        if (host.verbose()) host.event(b, 20);
        call_aes_ = aes_hex(aes);
        break;
    case 0x21:   // call information, service address (A2-9): the number is not shown
        snprintf(b, sizeof b, "C channel: call information (service address), AES %s, GES 0x%02X", aes_hex(aes).c_str(), su[4]);
        host.event(b, 20);
        call_aes_ = aes_hex(aes);
        break;
    default:
        if (host.verbose()) {
            snprintf(b, sizeof b, "C channel signal unit type 0x%02X", type);
            host.event(b, 30);
        }
        break;
    }
    if (!call_aes_.empty()) {
        air_[aes];
        air_[aes].label = "C channel";
        row(aes);
    }
    if (host.raw_wanted()) {
        std::string r = "C SU";
        for (int i = 0; i < 12; i++) {
            char h[4];
            snprintf(h, sizeof h, " %02X", su[i]);
            r += h;
        }
        host.raw(r);
    }
}

uint32_t SuDecoder::r_su(const uint8_t* su, double t0, double t1) {
    // R channel signal unit (19 octets, appendix 2 A2-2, A2-44 .. A2-54)
    su_ok_++;
    const int seqi = su[0] >> 4, sutype = su[0] & 15;
    char b[300];
    if (sutype == 15) {   // signalling: message type in octet 3
        const int type = su[2];
        const uint32_t aes = be24(su + 3);
        type_count_[type]++;
        const char* what = nullptr;
        bool always = true;
        switch (type) {
        case 0x10: what = "log-on request"; break;
        case 0x12: what = "log-off request"; break;
        case 0x13: what = "reassignment reject"; break;
        case 0x15: what = "log-on acknowledgement"; always = false; break;
        case 0x17: what = "ready for reassignment"; break;
        case 0x20: what = "telephone call request"; break;
        case 0x22: what = "request for reservation (data)"; always = false; break;
        case 0x23: what = "abbreviated telephone call request"; break;
        case 0x30: what = "call progress"; break;
        case 0x60: what = "telephony acknowledgement"; always = false; break;
        case 0x61: what = "request for acknowledgement"; always = false; break;
        case 0x62: what = "acknowledgement (PACK)"; always = false; break;
        default: break;
        }
        if (what && (always || host.verbose())) {
            snprintf(b, sizeof b, "↓ AES %s → GES 0x%02X · R channel %s", aes_hex(aes).c_str(), su[6], what);
            if (type == 0x10) {
                const int cls = su[10] >> 6;
                snprintf(b + strlen(b), sizeof b - strlen(b), " (class %d AES)", cls == 0 ? 1 : cls == 2 ? 3 : cls == 3 ? 4 : 2);
            }
            host.event(b, 30);
        }
        air_[aes];
        row(aes);
        return aes;
    }
    // user data (A2-44): sequence indicator, Q / ref, AES, GES, sutype octets of data
    type_count_[0xF0]++;
    const uint32_t aes = be24(su + 2);
    const int ges = su[5];
    const int n = std::min(sutype, 11);
    const int64_t key = (int64_t(2) << 40) | (int64_t(aes) << 8) | (su[1] & 0xF7);
    Asm* a = nullptr;
    auto first = [&](int left) {
        Asm x;
        x.aes = aes;
        x.ges = ges;
        x.q = su[1] >> 4;
        x.ref = su[1] & 7;
        x.next = left;
        x.t0 = t0;
        x.down = true;
        asm_[key] = x;
        return &asm_[key];
    };
    if (seqi == 1 || seqi == 2 || seqi == 4) {
        a = first(seqi == 1 ? 0 : seqi == 2 ? 1 : 2);
    } else if (seqi == 3 || seqi == 5 || seqi == 6) {
        auto it = asm_.find(key);
        const int want = seqi == 5 ? 1 : 0;   // what this one should leave
        if (it == asm_.end() || it->second.next != want + 1) return aes;
        a = &it->second;
    } else {
        return aes;
    }
    a->data.insert(a->data.end(), su + 6, su + 6 + n);
    a->t1 = t1;
    if (seqi == 1 || seqi == 3 || seqi == 6) {
        const Asm done = *a;
        asm_.erase(key);
        user_data(done);
    } else {
        a->next--;
    }
    return aes;
}

void SuDecoder::t_burst(const uint8_t* id6, const std::vector<const uint8_t*>& sus, double t0, double t1) {
    // the burst identifier names the AES; its SUs are the P / T formats
    const uint32_t aes = be24(id6);
    down_ctx_ = true;
    aes_ctx_ = aes;
    for (const uint8_t* s : sus) p_su(s, t0, t1, 0);
    down_ctx_ = false;
    if (host.verbose()) {
        char b[120];
        snprintf(b, sizeof b, "↓ AES %s → GES 0x%02X · T burst of %zu signal units", aes_hex(aes).c_str(), id6[3], sus.size());
        host.event(b, 5);
    }
    air_[aes];
    row(aes);
}

std::string SuDecoder::label_of(uint32_t aes) const {
    auto it = air_.find(aes);
    if (it == air_.end()) return "";
    return !it->second.flight.empty() ? it->second.flight : it->second.reg;
}

}  // namespace aero
