// RDS / RBDS group decoding - see rds_station.hpp
#include "rds_station.hpp"

#include "rds_tables.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rds {

namespace {

std::string hex4(uint16_t v) {
    char b[8];
    snprintf(b, sizeof b, "%04X", v);
    return b;
}

// Y-M-D of a Modified Julian Date (EN 50067 Annex G)
void mjd_date(int64_t mjd, int* y, int* m, int* d) {
    const int yp = static_cast<int>((static_cast<double>(mjd) - 15078.2) / 365.25);
    const int mp = static_cast<int>((static_cast<double>(mjd) - 14956.1 - std::trunc(yp * 365.25)) / 30.6001);
    *d = static_cast<int>(mjd - 14956 - static_cast<int64_t>(std::trunc(yp * 365.25)) - static_cast<int64_t>(std::trunc(mp * 30.6001)));
    const int k = (mp == 14 || mp == 15) ? 1 : 0;
    *y = yp + k + 1900;
    *m = mp - 1 - 12 * k;
}

std::string mjd_text(int64_t minutes) {   // minutes since MJD 0 -> "YYYY-MM-DD HH:MM"
    const int64_t mjd = minutes / 1440, mi = minutes % 1440;
    int y, m, d;
    mjd_date(mjd, &y, &m, &d);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", y, m, d, static_cast<int>(mi / 60), static_cast<int>(mi % 60));
    return b;
}

const char* dab_channel(uint32_t khz) {
    struct C { uint32_t k; const char* n; };
    static const C T[] = {
        {174928, "5A"}, {176640, "5B"}, {178352, "5C"}, {180064, "5D"}, {181936, "6A"}, {183648, "6B"},
        {185360, "6C"}, {187072, "6D"}, {188928, "7A"}, {190640, "7B"}, {192352, "7C"}, {194064, "7D"},
        {195936, "8A"}, {197648, "8B"}, {199360, "8C"}, {201072, "8D"}, {202928, "9A"}, {204640, "9B"},
        {206352, "9C"}, {208064, "9D"}, {209936, "10A"}, {210096, "10N"}, {211648, "10B"}, {213360, "10C"},
        {215072, "10D"}, {216928, "11A"}, {217088, "11N"}, {218640, "11B"}, {220352, "11C"}, {222064, "11D"},
        {223936, "12A"}, {224096, "12N"}, {225648, "12B"}, {227360, "12C"}, {229072, "12D"}, {230784, "13A"},
        {232496, "13B"}, {234208, "13C"}, {235776, "13D"}, {237488, "13E"}, {239200, "13F"}};
    for (const auto& c : T)
        if (c.k == khz) return c.n;
    static const char* const L[] = {"LA", "LB", "LC", "LD", "LE", "LF", "LG", "LH", "LI", "LJ", "LK", "LL", "LM", "LN", "LO", "LP"};
    for (int i = 0; i < 16; i++)
        if (khz == static_cast<uint32_t>(1452960 + 1712 * i)) return L[i];
    return "";
}

// a value that a corrected block may only change when a second copy agrees
template <typename T>
bool confirm(T* v, T* pend, T x, bool trusted) {
    if (trusted || *v == x || *pend == x) {
        *v = x;
        return true;
    }
    *pend = x;
    return false;
}

}  // namespace

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    return s.substr(a, b - a);
}

std::string rds_text(const std::vector<uint8_t>& b) {
    std::string o;
    for (uint8_t c : b) {
        if (c == 0x0D) break;                      // end of text
        if (c == 0x0A || c == 0x0B) o += ' ';      // line break / end of headline
        else o += rds_char(c);
    }
    return o;
}

std::string utf8_text(const std::vector<uint8_t>& b) {
    std::string o;
    size_t i = 0;
    while (i < b.size()) {
        const uint8_t c = b[i];
        if (c == 0x0D || c == 0) break;
        int len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && i + static_cast<size_t>(len) <= b.size();
        for (int k = 1; ok && k < len; k++) ok = (b[i + static_cast<size_t>(k)] & 0xC0) == 0x80;
        if (!ok) {
            o += '?';
            i++;
            continue;
        }
        if (len == 1) {
            o += c < 0x20 || c == 0x7F ? ' ' : static_cast<char>(c);
        } else if (len == 2 && c == 0xC2 && b[i + 1] < 0xA0) {
            o += ' ';   // C1 control
        } else {
            o.append(reinterpret_cast<const char*>(&b[i]), static_cast<size_t>(len));
        }
        i += static_cast<size_t>(len);
    }
    return o;
}

std::string ucs2_text(const std::vector<uint8_t>& b) {
    std::string o;
    for (size_t i = 0; i + 1 < b.size(); i += 2) {
        const unsigned u = static_cast<unsigned>(b[i]) << 8 | b[i + 1];
        if (u == 0x000D || u == 0) break;
        if (u < 0x20 || (u >= 0x7F && u < 0xA0)) {
            o += ' ';
        } else if (u < 0x80) {
            o += static_cast<char>(u);
        } else if (u < 0x800) {
            o += static_cast<char>(0xC0 | (u >> 6));
            o += static_cast<char>(0x80 | (u & 0x3F));
        } else if (u >= 0xD800 && u < 0xE000) {
            o += '?';   // surrogates: UCS-2 has none
        } else {
            o += static_cast<char>(0xE0 | (u >> 12));
            o += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (u & 0x3F));
        }
    }
    return o;
}

std::string GroupDecoder::pi_hex(uint16_t pi) { return hex4(pi); }

std::string GroupDecoder::af_mhz(int code) {
    if (code < 1 || code > 204) return "";
    char b[16];
    snprintf(b, sizeof b, "%.1f", 87.5 + 0.1 * code);
    return b;
}

std::string GroupDecoder::area(uint16_t pi) {
    static const char* const A[16] = {"local", "international", "national", "supra-regional"};
    const int a = pi >> 8 & 0xF;
    if (a < 4) return A[a];
    return "regional " + std::to_string(a - 3);
}

std::string GroupDecoder::pin_text(uint16_t pin) const {
    const int day = pin >> 11, hour = pin >> 6 & 0x1F, minute = pin & 0x3F;
    if (pin == 0 || day == 0 || hour > 23 || minute > 59) return "";
    char b[48];
    snprintf(b, sizeof b, "day %d, %02d:%02d", day, hour, minute);
    return b;
}

bool GroupDecoder::rbds(const Station& s) const {
    if (pty_mode_ == 1) return false;
    if (pty_mode_ == 2) return true;
    if (s.ecc >= 0) {
        const std::string c = country_code(s.pi >> 12, s.ecc);
        return c == "us" || c == "ca" || c == "mx";
    }
    // no ECC (yet): the receiver's location - North America
    return have_loc_ && lat_ > 14 && lat_ < 72 && lon_ > -170 && lon_ < -50;
}

std::string GroupDecoder::pty_text(const Station& s, int pty) const {
    if (pty < 0) return "";
    return std::string(pty_name(pty, rbds(s))) + " (" + std::to_string(pty) + ")";
}

std::string GroupDecoder::country(const Station& s) const {
    if (s.ecc < 0) return "";
    return country_name(country_code(s.pi >> 12, s.ecc));
}

std::string GroupDecoder::callsign(const Station& s) const { return rbds(s) ? rbds_callsign(s.pi) : ""; }

Station& GroupDecoder::sta(uint16_t pi, double t) {
    auto it = st_.find(pi);
    if (it != st_.end()) return it->second;
    // at most 64 stations: the one heard longest ago goes
    if (st_.size() >= 64) {
        auto old = st_.begin();
        for (auto i = st_.begin(); i != st_.end(); ++i)
            if (i->second.last < old->second.last && i->first != cur_) old = i;
        st_.erase(old);
    }
    Station& s = st_[pi];
    s.pi = pi;
    s.first = t;
    return s;
}

uint16_t GroupDecoder::handle(const Group& g, double t) {
    // a random word passes a block's offset check once in 1024: a group with
    // a single good block is most likely noise (a fade, the gap before the
    // sync is dropped) - nothing of it is used
    if (g.good() < 2) return 0;
    // the PI: block A, or block C' of a version B group
    int pi = -1;
    bool pi_trusted = false;
    if (g.b[0].st) {
        pi = g.b[0].v;
        pi_trusted = g.b[0].st == 1;
    } else if (g.cprime && g.b[2].st) {
        pi = g.b[2].v;
        pi_trusted = g.b[2].st == 1;
    }
    if (pi >= 0 && pi != cur_) {
        // another PI: a new station takes over (co-channel, a retune) only
        // after 2 (none yet) / 3 groups in a row, one of them uncorrected
        if (pi == cand_) {
            cand_n_++;
            cand_clean_ = cand_clean_ || pi_trusted;
        } else {
            cand_ = static_cast<uint16_t>(pi);
            cand_n_ = 1;
            cand_clean_ = pi_trusted;
        }
        if (cand_clean_ && cand_n_ >= (cur_ ? 3 : 2) && pi != 0) {
            const uint16_t old = cur_;
            cur_ = cand_;
            cand_n_ = 0;
            sta(cur_, t);   // a station entry from its first group on
            auto name = [&](uint16_t p) {
                const Station* q = station(p);
                const std::string ps = q ? trim(q->ps_str) : "";
                return "PI " + hex4(p) + (ps.empty() ? "" : " (" + ps + ")");
            };
            news(old ? "Station changed: " + name(old) + " -> " + name(cur_) : "Station " + name(cur_), 5.0);
        } else {
            return 0;
        }
    } else if (pi < 0) {
        // no PI in this group: ours only right after a group that showed our
        // PI (a group without one doesn't extend that - through a fade or a
        // gap the next station's groups would be taken for ours), and only
        // with its block B
        if (!cur_ || t - last_ours_ > 0.25 || !g.b[1].st) return 0;
    } else {
        cand_n_ = 0;
    }
    if (pi >= 0) last_ours_ = t;
    Station& s = sta(cur_, t);
    s.last = t;
    s.groups++;
    if (g.clean()) s.clean_groups++;
    if (!g.b[1].st) return cur_;
    basic(s, g, t);
    const int type = g.b[1].v >> 12, ver_b = g.b[1].v >> 11 & 1;
    s.type_count[type * 2 + ver_b]++;
    // an ODA assigned to this group type (3A) decodes it - except the
    // groups with a fixed meaning
    auto oda = s.oda.find(type * 2 + ver_b);
    if (oda != s.oda.end() && !(type == 0 || type == 1 || type == 2 || type == 3 || type == 4 || type == 10 ||
                                type == 14 || type == 15)) {
        oda_group(s, g, oda->second);
        return cur_;
    }
    switch (type) {
        case 0: g0(s, g, ver_b, t); break;
        case 1: g1(s, g, ver_b); break;
        case 2: g2(s, g, ver_b); break;
        case 3: if (!ver_b) g3a(s, g); break;
        case 4: if (!ver_b) g4a(s, g, t); break;
        case 5: s.tdc_groups++; break;
        case 6: s.ih_groups++; break;
        case 7: if (!ver_b) s.paging_groups++; break;
        case 8:
            if (!ver_b) {
                s.tmc_groups++;
                if (s.tmc_groups == 1) news("RDS-TMC traffic messages (group 8A)", 600);
            }
            break;
        case 9:
            if (!ver_b) {
                s.ews_groups++;
                news("⚠ Emergency warning data (group 9A) from PI " + hex4(s.pi), 600);
            }
            break;
        case 10: if (!ver_b) g10a(s, g); break;
        case 13: if (!ver_b) s.paging_groups++; break;
        case 14: g14(s, g, ver_b, t); break;
        case 15:
            if (ver_b) g15b(s, g);
            else g15a(s, g);
            break;
        default: break;
    }
    return cur_;
}

void GroupDecoder::basic(Station& s, const Group& g, double t) {
    (void)t;
    const bool tr = g.b[1].st == 1;
    const uint16_t b = g.b[1].v;
    const int old_pty = s.pty;
    confirm(&s.tp, &s.p_tp, b >> 10 & 1, tr);
    if (confirm(&s.pty, &s.p_pty, b >> 5 & 0x1F, tr) && s.pty != old_pty) {
        if (old_pty >= 0) news("Programme type: " + pty_text(s, s.pty), 30);
        if (s.pty == 31) news(std::string("⚠ PTY 31 \"") + pty_name(31, rbds(s)) + "\" from PI " + hex4(s.pi), 60);
        else if (s.pty == 30) news(std::string("PTY 30 \"") + pty_name(30, rbds(s)) + "\" from PI " + hex4(s.pi), 60);
    }
}

// TA, M/S, DI (groups 0A / 0B / 15B: block B bits 4-0)
static void ta_ms_di(Station& s, uint16_t b, bool tr, std::vector<std::pair<std::string, double>>* news) {
    const int old_ta = s.ta;
    if (confirm(&s.ta, &s.p_ta, b >> 4 & 1, tr) && old_ta >= 0 && s.ta != old_ta && (s.tp == 1 || s.ta == 1))
        news->emplace_back(s.ta ? "Traffic announcement started (TA on)" : "Traffic announcement ended (TA off)", 1.0);
    confirm(&s.ms, &s.p_ms, b >> 3 & 1, tr);
    if (tr) {
        const int seg = b & 3, bit = 3 - seg;   // segment 0 -> d3 ... segment 3 -> d0
        s.di = (s.di & ~(1 << bit)) | ((b >> 2 & 1) << bit);
        s.di_known |= 1 << bit;
    }
}

void GroupDecoder::g0(Station& s, const Group& g, bool ver_b, double t) {
    const uint16_t b = g.b[1].v;
    const bool trb = g.b[1].st == 1;
    ta_ms_di(s, b, trb, &news_);
    const int seg = b & 3;
    // AF codes (version A, block C): a frequency list
    if (!ver_b && g.b[2].st) {
        const bool tr = trb && g.b[2].st == 1;
        const int c[2] = {g.b[2].v >> 8, g.b[2].v & 0xFF};
        for (int k = 0; k < 2; k++) {
            const int v = c[k];
            if (s.lfmf_next) {
                // the code after 250: LF 153-279 kHz (1..15) or MF 531-1602 kHz (16..135), 9 kHz steps
                if (tr && v >= 1 && v <= 15) s.af_lfmf.insert(144 + 9 * v);
                else if (tr && v >= 16 && v <= 135) s.af_lfmf.insert(531 + 9 * (v - 16));
                s.lfmf_next = false;
                continue;
            }
            if (v == 250) s.lfmf_next = true;
            else if (v >= 1 && v <= 204 && tr) s.af.insert(v);
        }
    }
    if (g.b[3].st) {
        const bool tr = trb && g.b[3].st == 1;
        const uint8_t c0 = static_cast<uint8_t>(g.b[3].v >> 8), c1 = static_cast<uint8_t>(g.b[3].v & 0xFF);
        const int o0 = s.ps.at(2 * seg), o1 = s.ps.at(2 * seg + 1);
        const bool changed = (o0 >= 0 && o0 != c0) || (o1 >= 0 && o1 != c1);
        const bool a = s.ps.put(2 * seg, c0, tr);
        const bool c = s.ps.put(2 * seg + 1, c1, tr);
        if (a && c) {
            // the PS is complete when all four segments are confirmed since
            // one last changed (in any order: a weak signal loses groups; a
            // dynamic PS changes a segment, and the others must come again)
            s.ps_mask = changed ? 1u << seg : s.ps_mask | 1u << seg;
            if (s.ps_mask == 0xF) finish_ps(s, t);
        }
    }
}

void GroupDecoder::finish_ps(Station& s, double t) {
    const std::string p = rds_text(s.ps.bytes(8));
    if (p == s.ps_str) return;
    const bool first = s.ps_str.empty();
    s.ps_str = p;
    // dynamic PS (scrolling text in the PS): 3+ different names within 60 s
    s.ps_hist.emplace_back(t, p);
    while (!s.ps_hist.empty() && t - s.ps_hist.front().first > 60) s.ps_hist.erase(s.ps_hist.begin());
    if (s.ps_hist.size() >= 3 && !s.dynamic_ps) {
        s.dynamic_ps = true;
        news("PI " + hex4(s.pi) + ": the PS changes (dynamic / scrolling PS)", 600);
    }
    if (!s.dynamic_ps) news(std::string(first ? "PS: " : "PS changed: ") + "\"" + trim(p) + "\" (PI " + hex4(s.pi) + ")", 60);
}

void GroupDecoder::g1(Station& s, const Group& g, bool ver_b) {
    const bool trb = g.b[1].st == 1;
    if (!ver_b && g.b[2].st) {
        const bool tr = trb && g.b[2].st == 1;
        const uint16_t c = g.b[2].v;
        const int variant = c >> 12 & 7;
        switch (variant) {
            case 0: {
                const int old = s.ecc;
                if ((c & 0xFF) != 0 && confirm(&s.ecc, &s.p_ecc, c & 0xFF, tr) && old != s.ecc) {
                    const std::string cn = country(s);
                    news("PI " + hex4(s.pi) + ": extended country code " + hex4(static_cast<uint16_t>(s.ecc)).substr(2) +
                             (cn.empty() ? "" : " = " + cn),
                         600);
                }
                break;
            }
            case 1: confirm(&s.tmc_id, &s.p_tmc, c & 0xFFF, tr); break;
            case 3:
                if ((c & 0x7F) != 0) confirm(&s.lang, &s.p_lang, c & 0x7F, tr);   // 0 = unknown
                break;
            case 7: confirm(&s.ews_id, &s.p_ews, c & 0xFFF, tr); break;
            default: break;
        }
        if (tr) s.la = c >> 15;
    }
    if (g.b[3].st) {
        confirm(&s.pin, &s.p_pin, g.b[3].v, trb && g.b[3].st == 1);
    }
}

void GroupDecoder::g2(Station& s, const Group& g, bool ver_b) {
    const uint16_t b = g.b[1].v;
    const bool trb = g.b[1].st == 1;
    const int ab = b >> 4 & 1, seg = b & 0xF;
    if (static_cast<bool>(ver_b) != s.rt_b) {
        s.rt_b = ver_b;
        s.rt.resize(ver_b ? 32 : 64);
        s.rt.clear();
        s.rt_mask = 0;
    }
    // the A/B flag toggles: a new message, the screen is cleared
    if (trb && s.rt_ab >= 0 && ab != s.rt_ab) {
        s.rt.clear();
        s.rt_mask = 0;
    }
    if (trb) s.rt_ab = ab;
    bool got = false;
    if (!ver_b) {
        if (g.b[2].st && g.b[3].st) {
            const bool t2 = trb && g.b[2].st == 1, t3 = trb && g.b[3].st == 1;
            bool ok = s.rt.put(4 * seg, static_cast<uint8_t>(g.b[2].v >> 8), t2);
            ok = s.rt.put(4 * seg + 1, static_cast<uint8_t>(g.b[2].v & 0xFF), t2) && ok;
            ok = s.rt.put(4 * seg + 2, static_cast<uint8_t>(g.b[3].v >> 8), t3) && ok;
            ok = s.rt.put(4 * seg + 3, static_cast<uint8_t>(g.b[3].v & 0xFF), t3) && ok;
            got = ok;
        }
    } else if (g.b[3].st) {
        const bool t3 = trb && g.b[3].st == 1;
        bool ok = s.rt.put(2 * seg, static_cast<uint8_t>(g.b[3].v >> 8), t3);
        ok = s.rt.put(2 * seg + 1, static_cast<uint8_t>(g.b[3].v & 0xFF), t3) && ok;
        got = ok;
    }
    if (got) {
        s.rt_mask |= 1u << seg;
        finish_rt(s);
    }
}

void GroupDecoder::finish_rt(Station& s) {
    // complete: every segment up to the one with the end-of-text code, or all 16
    const int per = s.rt_b ? 2 : 4;
    const int term = s.rt.terminator();
    const int nseg = term >= 0 ? term / per + 1 : 16;
    if ((s.rt_mask & ((1u << nseg) - 1)) != (1u << nseg) - 1) return;
    const int n = term >= 0 ? term : s.rt.size();
    if (!s.rt.known_to(n)) return;
    const std::string r = trim(rds_text(s.rt.bytes(n)));
    if (r.empty() || r == s.rt_str) return;
    s.rt_str = r;
    news("RadioText: " + r, 3600);
}

void GroupDecoder::g3a(Station& s, const Group& g) {
    if (!(g.b[2].st && g.b[3].st)) return;
    const bool tr = g.b[1].st == 1 && g.b[2].st == 1 && g.b[3].st == 1;
    if (!tr) return;   // a directory entry only from a clean group
    const int agt = g.b[1].v & 0x1F;   // application group type: number * 2 + version
    const uint16_t aid = g.b[3].v, msg = g.b[2].v;
    if (aid == 0) return;
    const bool fresh = !s.oda_msg.count(aid);
    s.oda_msg[aid] = msg;
    if (agt != 0) {   // 0 = the AID uses no group type of its own (only 3A messages)
        auto it = s.oda.find(agt);
        if (it == s.oda.end() || it->second != aid) {
            s.oda[agt] = aid;
            const char* nm = oda_name(aid);
            news("ODA " + std::string(*nm ? nm : "AID " + hex4(aid)) + " in group " + std::to_string(agt >> 1) +
                     (agt & 1 ? "B" : "A") + " (PI " + hex4(s.pi) + ")",
                 600);
        }
    } else if (fresh) {
        const char* nm = oda_name(aid);
        news("ODA " + std::string(*nm ? nm : "AID " + hex4(aid)) + " announced (PI " + hex4(s.pi) + ")", 600);
    }
    if (aid == 0x6552) s.ert_utf8 = msg & 1;
    if (aid == 0xCD46 || aid == 0xCD47) {
        const int variant = msg >> 14 & 3;
        if (variant == 0) {
            char b[96];
            snprintf(b, sizeof b, "location table %d", msg >> 6 & 0x3F);
            s.tmc_info = b;
        }
    }
}

void GroupDecoder::g4a(Station& s, const Group& g, double t) {
    if (!(g.b[2].st && g.b[3].st)) return;
    const uint16_t b = g.b[1].v, c = g.b[2].v, d = g.b[3].v;
    const int64_t mjd = static_cast<int64_t>((b & 3) << 15 | c >> 1);
    const int hour = (c & 1) << 4 | d >> 12, minute = d >> 6 & 0x3F;
    const int off = (d & 0x1F) * ((d >> 5 & 1) ? -1 : 1);   // half hours
    if (mjd < 15079 || hour > 23 || minute > 59 || std::abs(off) > 28) return;
    const int64_t mins = mjd * 1440 + hour * 60 + minute;
    // from a clean group, or confirmed by the previous clock time (one per minute)
    const bool clean = g.clean();
    const bool follows = s.ct_minutes >= 0 && std::fabs(static_cast<double>(mins - s.ct_minutes) - (t - s.ct_t) / 60.0) <= 1.5;
    const bool pend_ok = s.p_ct >= 0 && std::fabs(static_cast<double>(mins - s.p_ct) - (t - s.p_ct_t) / 60.0) <= 1.5;
    if (!clean && !follows && !pend_ok) {
        s.p_ct = mins;
        s.p_ct_t = t;
        return;
    }
    const bool jump = s.ct_minutes >= 0 && !follows;
    const bool first = s.ct_minutes < 0;
    s.ct_minutes = mins;
    s.ct_t = t;
    s.ct = mjd_text(mins) + " UTC";
    char o[16];
    snprintf(o, sizeof o, "UTC%c%d%s", off < 0 ? '-' : '+', std::abs(off) / 2, std::abs(off) % 2 ? ":30" : "");
    s.ct_local = mjd_text(mins + off * 30) + " (" + o + ")";
    if (first || jump) news("Clock time: " + s.ct_local + " (PI " + hex4(s.pi) + ")", 30);
}

void GroupDecoder::g10a(Station& s, const Group& g) {
    const uint16_t b = g.b[1].v;
    const bool trb = g.b[1].st == 1;
    const int ab = b >> 4 & 1, seg = b & 1;
    if (trb && s.ptyn_ab >= 0 && ab != s.ptyn_ab) {
        s.ptyn.clear();
        s.ptyn_mask = 0;
    }
    if (trb) s.ptyn_ab = ab;
    if (!(g.b[2].st && g.b[3].st)) return;
    const bool t2 = trb && g.b[2].st == 1, t3 = trb && g.b[3].st == 1;
    bool ok = s.ptyn.put(4 * seg, static_cast<uint8_t>(g.b[2].v >> 8), t2);
    ok = s.ptyn.put(4 * seg + 1, static_cast<uint8_t>(g.b[2].v & 0xFF), t2) && ok;
    ok = s.ptyn.put(4 * seg + 2, static_cast<uint8_t>(g.b[3].v >> 8), t3) && ok;
    ok = s.ptyn.put(4 * seg + 3, static_cast<uint8_t>(g.b[3].v & 0xFF), t3) && ok;
    if (!ok) return;
    s.ptyn_mask |= 1u << seg;
    if (s.ptyn_mask == 3) {
        const std::string p = trim(rds_text(s.ptyn.bytes(8)));
        if (!p.empty() && p != s.ptyn_str) {
            s.ptyn_str = p;
            news("Programme type name: \"" + p + "\" (PI " + hex4(s.pi) + ")", 300);
        }
    }
}

void GroupDecoder::g14(Station& s, const Group& g, bool ver_b, double t) {
    if (!g.b[3].st) return;
    const bool trb = g.b[1].st == 1;
    const uint16_t on_pi = g.b[3].v;
    if (on_pi == 0 || on_pi == s.pi) return;
    const bool trd = trb && g.b[3].st == 1;
    // a new other network only from a clean PI(ON)
    if (!s.eon.count(on_pi) && !trd) return;
    if (s.eon.size() >= 32 && !s.eon.count(on_pi)) return;
    OtherNetwork& on = s.eon[on_pi];
    const bool fresh = on.pi == 0;
    on.pi = on_pi;
    on.last = t;
    if (fresh) news("EON: other network PI " + hex4(on_pi) + " (from PI " + hex4(s.pi) + ")", 600);
    const uint16_t b = g.b[1].v;
    if (trb) on.tp = b >> 4 & 1;
    if (ver_b) {
        if (trb) on.ta = b >> 3 & 1;
        return;
    }
    if (!g.b[2].st) return;
    const bool tr = trb && g.b[2].st == 1 && g.b[3].st == 1;
    const uint16_t c = g.b[2].v;
    const int variant = b & 0xF;
    if (variant <= 3) {
        const bool a = on.ps.put(2 * variant, static_cast<uint8_t>(c >> 8), tr);
        const bool z = on.ps.put(2 * variant + 1, static_cast<uint8_t>(c & 0xFF), tr);
        if (a && z && on.ps.known_to(8)) {
            const std::string p = trim(rds_text(on.ps.bytes(8)));
            if (p != on.ps_str && !p.empty()) {
                on.ps_str = p;
                news("EON: PI " + hex4(on_pi) + " is \"" + p + "\"", 600);
            }
        }
    } else if (variant == 4 && tr) {
        for (int v : {c >> 8, c & 0xFF})
            if (v >= 1 && v <= 204) on.af.insert(v);
    } else if (variant >= 5 && variant <= 9 && tr) {
        const int v = c & 0xFF;
        if (v >= 1 && v <= 204) on.af.insert(v);
    } else if (variant == 13 && tr) {
        on.pty = c >> 11 & 0x1F;
        on.ta = c & 1;
    }
}

void GroupDecoder::g15a(Station& s, const Group& g) {
    // Long PS (IEC 62106-2:2018): 32 bytes UTF-8, 8 segments of 4
    const int seg = g.b[1].v & 7;
    if (!(g.b[2].st && g.b[3].st)) return;
    const bool trb = g.b[1].st == 1;
    const bool t2 = trb && g.b[2].st == 1, t3 = trb && g.b[3].st == 1;
    bool ok = s.lps.put(4 * seg, static_cast<uint8_t>(g.b[2].v >> 8), t2);
    ok = s.lps.put(4 * seg + 1, static_cast<uint8_t>(g.b[2].v & 0xFF), t2) && ok;
    ok = s.lps.put(4 * seg + 2, static_cast<uint8_t>(g.b[3].v >> 8), t3) && ok;
    ok = s.lps.put(4 * seg + 3, static_cast<uint8_t>(g.b[3].v & 0xFF), t3) && ok;
    if (!ok) return;
    s.lps_mask |= 1u << seg;
    const int term = s.lps.terminator();
    const int nseg = term >= 0 ? term / 4 + 1 : 8;
    if ((s.lps_mask & ((1u << nseg) - 1)) != (1u << nseg) - 1) return;
    const int n = term >= 0 ? term : 32;
    if (!s.lps.known_to(n)) return;
    const std::string p = trim(utf8_text(s.lps.bytes(n)));
    if (!p.empty() && p != s.lps_str) {
        s.lps_str = p;
        news("Long PS: \"" + p + "\" (PI " + hex4(s.pi) + ")", 300);
    }
}

void GroupDecoder::g15b(Station& s, const Group& g) {
    // fast basic tuning: block B (and its copy in block D) as in 0B
    const Block& b = g.b[1].st == 1 ? g.b[1] : g.b[3];
    if (b.st) ta_ms_di(s, b.v, b.st == 1, &news_);
}

void GroupDecoder::oda_group(Station& s, const Group& g, uint16_t aid) {
    switch (aid) {
        case 0x4BD7: rtplus(s, g); break;
        case 0x6552: ert_group(s, g); break;
        case 0xCD46: case 0xCD47: tmc_group(s, g); break;
        case 0x0093: dab_group(s, g); break;
        default: break;
    }
}

void GroupDecoder::rtplus(Station& s, const Group& g) {
    if (!g.clean()) return;   // tags point into the RadioText: only from clean groups
    const uint16_t b = g.b[1].v, c = g.b[2].v, d = g.b[3].v;
    const int toggle = b >> 4 & 1, running = b >> 3 & 1;
    if (toggle != s.rtp_toggle || running != s.rtp_running) {
        s.rtp.clear();
        s.rtp_toggle = toggle;
        s.rtp_running = running;
    }
    const int t1 = (b & 7) << 3 | c >> 13, s1 = c >> 7 & 0x3F, l1 = (c >> 1 & 0x3F) + 1;
    const int t2 = (c & 1) << 5 | d >> 11, s2 = d >> 5 & 0x3F, l2 = (d & 0x1F) + 1;
    std::string what;
    const int tags[2][3] = {{t1, s1, l1}, {t2, s2, l2}};
    for (const auto& tg : tags) {
        const int ty = tg[0], st = tg[1], ln = tg[2];
        if (ty == 0 || st + ln > s.rt.size() || !s.rt.known_to(st + ln)) continue;
        const std::vector<uint8_t> all = s.rt.bytes(st + ln);
        const std::string txt = trim(rds_text(std::vector<uint8_t>(all.begin() + st, all.end())));
        if (txt.empty()) continue;
        RtPlusTag* have = nullptr;
        for (auto& tag : s.rtp)
            if (tag.type == ty) have = &tag;
        if (have && have->text == txt) continue;
        if (have) have->text = txt;
        else s.rtp.push_back({ty, txt});
        what += std::string(what.empty() ? "" : ", ") + rtplus_type(ty) + " = " + txt;
    }
    if (!what.empty() && running) news("RT+: " + what, 600);
}

void GroupDecoder::ert_group(Station& s, const Group& g) {
    const int seg = g.b[1].v & 0x1F;
    if (!(g.b[2].st && g.b[3].st)) return;
    const bool trb = g.b[1].st == 1;
    const bool t2 = trb && g.b[2].st == 1, t3 = trb && g.b[3].st == 1;
    bool ok = s.ert.put(4 * seg, static_cast<uint8_t>(g.b[2].v >> 8), t2);
    ok = s.ert.put(4 * seg + 1, static_cast<uint8_t>(g.b[2].v & 0xFF), t2) && ok;
    ok = s.ert.put(4 * seg + 2, static_cast<uint8_t>(g.b[3].v >> 8), t3) && ok;
    ok = s.ert.put(4 * seg + 3, static_cast<uint8_t>(g.b[3].v & 0xFF), t3) && ok;
    if (!ok) return;
    s.ert_mask |= 1u << seg;
    // the end: 0x0D (UTF-8) / 0x000D (UCS-2), else all 128 bytes
    int n = 128;
    if (s.ert_utf8) {
        const int t = s.ert.terminator();
        if (t >= 0) n = t;
    } else {
        for (int i = 0; i + 1 < 128; i += 2)
            if (s.ert.at(i) == 0 && s.ert.at(i + 1) == 0x0D) {
                n = i;
                break;
            }
    }
    const int nseg = std::min(32, n / 4 + 1);
    const uint32_t need = nseg >= 32 ? 0xFFFFFFFFu : (1u << nseg) - 1;
    if ((s.ert_mask & need) != need || !s.ert.known_to(n)) return;
    const std::string r = trim(s.ert_utf8 ? utf8_text(s.ert.bytes(n)) : ucs2_text(s.ert.bytes(n)));
    if (!r.empty() && r != s.ert_str) {
        s.ert_str = r;
        news("Enhanced RadioText: " + r, 3600);
    }
}

void GroupDecoder::tmc_group(Station& s, const Group& g) {
    s.tmc_groups++;
    if (s.tmc_groups == 1) news("RDS-TMC traffic messages (PI " + hex4(s.pi) + ")", 600);
    (void)g;
}

void GroupDecoder::dab_group(Station& s, const Group& g) {
    if (!g.clean()) return;
    const uint16_t b = g.b[1].v;
    if (b >> 4 & 1) return;   // service table, not the ensemble
    const uint32_t khz = 16 * ((static_cast<uint32_t>(b & 3) << 16) | g.b[2].v);
    const uint16_t eid = g.b[3].v;
    char t[96];
    const char* ch = dab_channel(khz);
    snprintf(t, sizeof t, "ensemble %04X on %.3f MHz%s%s", eid, khz / 1000.0, *ch ? " " : "", ch);
    if (s.dab != t) {
        s.dab = t;
        news(std::string("DAB cross-reference: ") + t, 600);
    }
}

}  // namespace rds
