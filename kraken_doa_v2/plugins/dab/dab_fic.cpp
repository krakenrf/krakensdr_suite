#include "dab_fic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dab {

// ---------------------------------------------------------------- channel decoding

FicDecoder::FicDecoder() {
    Profile p;
    p.blocks = {{21, 16}, {3, 15}};   // clause 11.2.1
    p.pad = 0;
    p.in_bits = 768;
    dp_.build(p);
    mother_.resize(static_cast<size_t>(dp_.mother));
    bits_.resize(768);
    reenc_.resize(static_cast<size_t>(dp_.mother));
}

int FicDecoder::decode(const int8_t* soft, uint8_t fibs[12][32], bool ok[12]) {
    int good = 0;
    last_bit_errors = last_bits = 0;
    const auto& prbs = prbs_bytes();
    for (int g = 0; g < 4; g++) {
        const int8_t* s = soft + 2304 * g;
        dp_.apply(s, mother_.data());
        vit_.decode(mother_.data(), 768, bits_.data());
        bool all = true;
        for (int f = 0; f < 3; f++) {
            uint8_t* fib = fibs[3 * g + f];
            for (int b = 0; b < 32; b++) {
                unsigned v = 0;
                for (int i = 0; i < 8; i++) v = (v << 1) | bits_[static_cast<size_t>(256 * f + 8 * b + i)];
                fib[b] = static_cast<uint8_t>(v ^ prbs[static_cast<size_t>(32 * f + b)]);
            }
            ok[3 * g + f] = fib_crc_ok(fib);
            good += ok[3 * g + f];
            all = all && ok[3 * g + f];
        }
        if (all) {   // channel bit errors: re-encode and compare with the hard decisions
            conv_encode(bits_.data(), 768, reenc_.data());
            for (size_t i = 0; i < dp_.pos.size(); i++) {
                const int p = dp_.pos[i];
                if (p < 0 || s[i] == 0) continue;
                last_bits++;
                last_bit_errors += (s[i] < 0) != (reenc_[static_cast<size_t>(p)] != 0);
            }
        }
    }
    return good;
}

// ---------------------------------------------------------------- database

const Component* Service::primary() const {
    for (const auto& c : comps)
        if (c.primary) return &c;
    return comps.empty() ? nullptr : &comps[0];
}

// ---------------------------------------------------------------- countries

namespace {

std::string hx(int v, int digits) {
    char b[16];
    snprintf(b, sizeof b, "%0*X", digits, static_cast<unsigned>(v));
    return b;
}

int sid_country_id(const Service& s) { return static_cast<int>(s.data ? (s.sid >> 20) & 15 : (s.sid >> 12) & 15); }

}   // namespace

int own_ecc(const Ensemble& e, const Service& s) {
    if (s.data) return static_cast<int>(s.sid >> 24);   // clause 6.3.1: ECC + country id + reference
    const auto it = e.service_ecc.find(s.sid);
    return it == e.service_ecc.end() ? -1 : it->second;
}

// By the standard: ECC (FIG 0/9) + the EId's country id. That is checked against the
// services that carry the ensemble's ECC (clause 8.1.3.2: every programme service the
// extended field doesn't list, and those whose own ECC is the ensemble's): when none of
// them is in the EId's country while all of them agree on another one (at least
// MIN_COUNTRY_SERVICES), the ensemble is in theirs - DAB Melbourne 1 (EId F003, ECC F0:
// Malaysia by the EId) with 29 services in Australia (country id 1).
EnsembleCountry ensemble_country(const Ensemble& e, const std::string& kept_base) {
    EnsembleCountry c;
    if (e.eid < 0 || e.ecc < 0) return c;
    const int cid = (e.eid >> 12) & 15;
    const std::string by_eid = country_name(e.ecc, cid);
    const std::string eid_base = country_base(by_eid);
    int n = 0, match = 0, foreign = 0;
    bool agree = true;
    std::string other;
    std::vector<int> cids;
    for (const auto& kv : e.services) {
        const Service& s = kv.second;
        if (s.comps.empty()) continue;
        const int own = own_ecc(e, s);
        if (own >= 0 && own != e.ecc) {
            foreign++;
            continue;
        }
        n++;
        const int sc = sid_country_id(s);
        const std::string b = country_base(country_name(e.ecc, sc));
        if (!eid_base.empty() && b == eid_base) {
            match++;
            continue;
        }
        if (b.empty() || (!other.empty() && b != other)) agree = false;
        else other = b;
        if (std::find(cids.begin(), cids.end(), sc) == cids.end()) cids.push_back(sc);
    }
    const bool from_services = match == 0 && agree && !other.empty() && (n >= MIN_COUNTRY_SERVICES || (n >= 1 && other == kept_base));
    if (!from_services) {
        c.name = by_eid;
        c.base = eid_base;
        c.text = (by_eid.empty() ? std::string("unknown") : by_eid) + " (ECC " + hx(e.ecc, 2) + ", country id " + hx(cid, 1) + ")";
        return c;
    }
    std::sort(cids.begin(), cids.end());
    std::string ids;
    for (int x : cids) ids += (ids.empty() ? "" : " / ") + hx(x, 1);
    c.name = c.base = other;
    c.from_services = true;
    c.text = other + " (ECC " + hx(e.ecc, 2) + ", country id" + (cids.size() > 1 ? "s " : " ") + ids + " of " +
             (n == 1 ? std::string("its service") : "all its " + std::to_string(n) + " services") + (foreign ? " with this ECC" : "") +
             "; the EId's country id " + hx(cid, 1) +
             (by_eid.empty() ? " names no country with this ECC)" : " would read " + by_eid + ")");
    return c;
}

// A service's own ECC (32-bit SId, FIG 0/9 extended field) names its country. Without
// one, clause 8.1.3.2 implies the ensemble's ECC; that is taken as fact when the
// country id gives the ensemble's country, or when the multiplexer sends the extended
// field at all (it lists the services of other ECCs). Otherwise (a country id of
// another country, no extended field) the receiver can't tell: on 12D in South Tyrol
// (ECC E0, no extended field) the Swiss programmes (country id 4) would read Israel.
// Then the candidates of the ECC's series are shown, not a name as fact.
std::string service_country(const Ensemble& e, const Service& s, const EnsembleCountry& ec) {
    const int cid = sid_country_id(s);
    const int own = own_ecc(e, s);
    if (own >= 0) {
        const std::string nm = country_name(own, cid);
        return nm.empty() ? "unknown (ECC " + hx(own, 2) + ", country id " + hx(cid, 1) + ")" : nm;
    }
    if (e.ecc < 0) return "";
    const std::string nm = country_name(e.ecc, cid);
    if (!nm.empty() && (!e.service_ecc.empty() || (!ec.base.empty() && country_base(nm) == ec.base))) return nm;
    const auto cand = country_candidates(e.ecc, cid);
    if (cand.empty()) return "";   // (country id 0 is not used)
    constexpr int MAX_SHOWN = 5;
    std::string t = "not determined (country id " + hx(cid, 1) + ", no ECC of its own) - candidates: ";
    int shown = 0, more = 0;
    for (int pass = 0; pass < 2; pass++)   // the ensemble's ECC first
        for (const auto& x : cand) {
            if ((x.first == e.ecc) != (pass == 0)) continue;
            if (shown >= MAX_SHOWN) {
                more++;
                continue;
            }
            t += (shown++ ? "; " : "") + x.second + " (" + hx(x.first, 2) + (x.first == e.ecc ? ", the ensemble's ECC)" : ")");
        }
    if (more) t += "; " + std::to_string(more) + " more";
    return t;
}

namespace {

inline uint32_t be16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
inline int16_t s16(const uint8_t* p) { return static_cast<int16_t>(static_cast<uint16_t>(be16(p))); }

}   // namespace

Service& FigParser::service(std::map<uint32_t, Service>& m, uint32_t sid, bool data, double now) {
    auto it = m.find(sid);
    if (it == m.end()) {
        if (m.size() >= MAX_SERVICES) {   // full: the least recently signalled one goes
            auto old = m.begin();
            for (auto j = m.begin(); j != m.end(); ++j)
                if (std::max(j->second.first_seen, j->second.last_seen) < std::max(old->second.first_seen, old->second.last_seen)) old = j;
            m.erase(old);
        }
        Service s;
        s.sid = sid;
        s.data = data;
        s.first_seen = now;
        it = m.emplace(sid, s).first;
    }
    return it->second;
}

void FigParser::fib(const uint8_t* fib, int cif, double now) {
    int pos = 0;
    while (pos < 30) {
        const uint8_t h = fib[pos];
        if (h == 0xFF) break;   // end marker
        const int type = h >> 5, len = h & 31;
        if (len == 0 || pos + 1 + len > 30) {
            if (h != 0) malformed++;
            break;
        }
        const uint8_t* d = fib + pos + 1;
        if (type == 0 && len >= 1) {
            fig_count[(0 << 8) | (d[0] & 31)]++;
            cif_ = cif;
            fig0(d, len, now);
            if (foreign_eid >= 0) return;   // another ensemble's FIB: not parsed
        } else if (type == 1) {
            fig_count[(1 << 8) | (d[0] & 7)]++;
            fig1(d, len, now);
        } else if (type == 2) {
            fig_count[(2 << 8) | (d[0] & 7)]++;
            fig2(d, len, now);
        }
        pos += 1 + len;
    }
}

void FigParser::fig0(const uint8_t* d0, int n0, double now) {
    const bool cn = d0[0] >> 7, oe = (d0[0] >> 6) & 1, pd = (d0[0] >> 5) & 1;
    const int ext = d0[0] & 31;
    const uint8_t* d = d0 + 1;
    int n = n0 - 1;
    const int sidlen = pd ? 4 : 2;
    switch (ext) {
        case 0: {   // ensemble information
            if (n < 4) { malformed++; return; }
            const int eid = static_cast<int>(be16(d));
            if (e_.eid >= 0 && eid != e_.eid) {
                // another ensemble (or a corrupted FIG that passed the CRC):
                // the receiver decides, nothing of it is applied
                foreign_eid = eid;
                return;
            }
            e_.eid = eid;
            e_.change_flags = d[2] >> 6;
            e_.alarm = (d[2] >> 5) & 1;
            e_.occurrence = (e_.change_flags && n >= 5 && d[4] < 250) ? d[4] : -1;
            // FIG 0/0 refers to this FIB's CIF; keep the count of CIF 0 of the frame
            const int c = (d[2] & 31) * 250 + d[3];
            if ((d[2] & 31) < 20 && d[3] < 250 && frame_cif < 0) {   // (the frame's first: mode I has it in CIF 0)
                e_.cif_count = (c - cif_ + 5000) % 5000;
                frame_cif = e_.cif_count;
                frame_flags = e_.change_flags;
                frame_occ = e_.occurrence;
                frame_fig00_at = cif_;
            }
            return;
        }
        case 1: {   // sub-channel organisation
            auto& m = cn ? e_.next_subch : e_.subch;
            for (int i = 0; i + 3 <= n;) {
                SubChannel s;
                s.id = d[i] >> 2;
                s.start = ((d[i] & 3) << 8) | d[i + 1];
                const bool lf = d[i + 2] >> 7;
                bool valid = true;
                if (!lf) {
                    s.prot.uep = true;
                    s.prot.table_index = d[i + 2] & 63;
                    valid = !((d[i + 2] >> 6) & 1);   // table switch 1: reserved
                    i += 3;
                } else {
                    if (i + 4 > n) { malformed++; return; }
                    const int option = (d[i + 2] >> 4) & 7;
                    s.prot.uep = false;
                    s.prot.option = option;
                    s.prot.level = ((d[i + 2] >> 2) & 3) + 1;
                    s.prot.size_cu = ((d[i + 2] & 3) << 8) | d[i + 3];
                    valid = option <= 1;
                    i += 4;
                }
                if (!valid || !protection_complete(s.prot) || s.start + s.prot.size_cu > 864) continue;
                s.seen = now;
                auto it = m.find(s.id);
                if (it != m.end()) s.fec = it->second.fec;
                m[s.id] = s;
            }
            return;
        }
        case 2: {   // services and their components
            auto& m = cn ? e_.next_services : e_.services;
            for (int i = 0; i + sidlen + 1 <= n;) {
                const uint32_t sid = pd ? be32(d + i) : be16(d + i);
                const int caid = (d[i + sidlen] >> 4) & 7, nc = d[i + sidlen] & 15;
                i += sidlen + 1;
                if (i + 2 * nc > n) { malformed++; return; }
                Service& s = service(m, sid, pd, now);
                s.ca_id = caid;
                s.last_seen = now;   // (next configuration: in its own map, aged out the same way)
                std::vector<Component> comps;
                for (int c = 0; c < nc; c++, i += 2) {
                    Component k;
                    k.tmid = d[i] >> 6;
                    if (k.tmid == 3) k.scid = ((d[i] & 63) << 6) | (d[i + 1] >> 2);
                    else {
                        k.type = d[i] & 63;
                        k.subch = d[i + 1] >> 2;
                    }
                    k.primary = (d[i + 1] >> 1) & 1;
                    k.ca = d[i + 1] & 1;
                    // keep what other FIGs said about the same component
                    for (const auto& o : s.comps)
                        if (o.tmid == k.tmid && (k.tmid == 3 ? o.scid == k.scid : o.subch == k.subch)) {
                            k.scids = o.scids;
                            k.language = o.language;
                            k.uas = o.uas;
                            k.label = o.label;
                            if (k.tmid == 3) {
                                k.subch = o.subch;
                                k.type = o.type;
                                k.packet_addr = o.packet_addr;
                                k.dg = o.dg;
                            }
                        }
                    if (k.primary) k.scids = 0;
                    comps.push_back(k);
                }
                s.comps = comps;
            }
            return;
        }
        case 3: {   // packet mode components
            for (int i = 0; i + 5 <= n;) {
                const int scid = static_cast<int>(d[i] << 4 | d[i + 1] >> 4);
                const bool caorg = d[i + 1] & 1;
                const bool dg = d[i + 2] >> 7;
                const int dscty = d[i + 2] & 63;
                const int sub = d[i + 3] >> 2, addr = ((d[i + 3] & 3) << 8) | d[i + 4];
                i += 5 + (caorg ? 2 : 0);
                if (i > n) { malformed++; return; }
                for (auto& kv : (cn ? e_.next_services : e_.services))
                    for (auto& k : kv.second.comps)
                        if (k.tmid == 3 && k.scid == scid) {
                            k.subch = sub;
                            k.type = dscty;
                            k.packet_addr = addr;
                            k.dg = dg;
                        }
            }
            return;
        }
        case 5: {   // service component language
            for (int i = 0; i < n;) {
                const bool ls = d[i] >> 7;
                if (i + (ls ? 3 : 2) > n) { malformed++; return; }
                if (!ls) {
                    const int sub = d[i] & 63, lang = d[i + 1];
                    for (auto& kv : e_.services)
                        for (auto& k : kv.second.comps)
                            if (k.tmid != 3 && k.subch == sub) k.language = lang;
                    i += 2;
                } else {
                    const int scid = static_cast<int>(((d[i] & 15) << 8) | d[i + 1]), lang = d[i + 2];
                    for (auto& kv : e_.services)
                        for (auto& k : kv.second.comps)
                            if (k.tmid == 3 && k.scid == scid) k.language = lang;
                    i += 3;
                }
            }
            return;
        }
        case 7: {   // configuration information
            if (n < 2 || cn) return;
            e_.services_total = d[0] >> 2;
            e_.reconfig_count = ((d[0] & 3) << 8) | d[1];
            return;
        }
        case 8: {   // service component global definition
            for (int i = 0; i + sidlen + 2 <= n;) {
                const uint32_t sid = pd ? be32(d + i) : be16(d + i);
                const bool extf = d[i + sidlen] >> 7;
                const int scids = d[i + sidlen] & 15;
                const bool ls = d[i + sidlen + 1] >> 7;
                int sub = -1, scid = -1;
                if (!ls) {
                    sub = d[i + sidlen + 1] & 63;
                    i += sidlen + 2;
                } else {
                    if (i + sidlen + 3 > n) { malformed++; return; }
                    scid = static_cast<int>(((d[i + sidlen + 1] & 15) << 8) | d[i + sidlen + 2]);
                    i += sidlen + 3;
                }
                if (extf) i++;
                if (i > n) { malformed++; return; }
                auto it = e_.services.find(sid);
                if (it == e_.services.end()) continue;
                for (auto& k : it->second.comps)
                    if ((sub >= 0 && k.tmid != 3 && k.subch == sub) || (scid >= 0 && k.tmid == 3 && k.scid == scid))
                        k.scids = scids;
            }
            return;
        }
        case 9: {   // country, LTO and international table
            // (OE and P/D are Rfu since V2.1.1. V1.x: OE = 1 described other ensembles - not
            // this one's; P/D = 1 listed 32-bit SIds, which carry their ECC themselves; the low
            // 6 bits of a sub-field's first byte were a per-service LTO, now Rfa2)
            if (oe) return;
            if (n < 3) { malformed++; return; }
            const int lto = d[0] & 31;
            e_.lto = (d[0] & 32) ? -lto : lto;
            e_.ecc = d[1];
            e_.inter_table = d[2];
            const bool extf = d[0] >> 7;
            // a single FIG 0/9 per ensemble (TS 101 756 D.3.1.3): its list replaces the last one
            std::map<uint32_t, int> listed;
            for (int i = 3; extf && i + 2 <= n;) {
                const int ns = d[i] >> 6, ecc = d[i + 1];
                i += 2;
                if (i + sidlen * ns > n) { malformed++; return; }
                for (int s = 0; s < ns; s++, i += sidlen)
                    if (!pd) listed[be16(d + i)] = ecc;
            }
            if (!pd) e_.service_ecc.swap(listed);
            return;
        }
        case 10: {   // date and time
            if (n < 4) { malformed++; return; }
            e_.mjd = ((d[0] & 0x7F) << 10) | (d[1] << 2) | (d[2] >> 6);
            e_.utc_long = (d[2] >> 3) & 1;
            e_.hh = ((d[2] & 7) << 2) | (d[3] >> 6);
            e_.mm = d[3] & 63;
            e_.ss = e_.ms = 0;
            if (e_.utc_long && n >= 6) {
                e_.ss = d[4] >> 2;
                e_.ms = ((d[4] & 3) << 8) | d[5];
            }
            e_.dt_at = now;
            return;
        }
        case 13: {   // user application information
            for (int i = 0; i + sidlen + 1 <= n;) {
                const uint32_t sid = pd ? be32(d + i) : be16(d + i);
                const int scids = d[i + sidlen] >> 4, nua = d[i + sidlen] & 15;
                i += sidlen + 1;
                std::vector<int> uas;
                for (int u = 0; u < nua; u++) {
                    if (i + 2 > n) { malformed++; return; }
                    uas.push_back(static_cast<int>(d[i] << 3 | d[i + 1] >> 5));
                    i += 2 + (d[i + 1] & 31);
                    if (i > n) { malformed++; return; }
                }
                auto it = (cn ? e_.next_services : e_.services).find(sid);
                if (it == (cn ? e_.next_services : e_.services).end()) continue;
                for (auto& k : it->second.comps)
                    if (k.scids == scids) k.uas = uas;
            }
            return;
        }
        case 14: {   // FEC sub-channel organisation
            for (int i = 0; i < n; i++) {
                auto it = (cn ? e_.next_subch : e_.subch).find(d[i] >> 2);
                if (it != (cn ? e_.next_subch : e_.subch).end()) it->second.fec = d[i] & 3;
            }
            return;
        }
        case 17: {   // programme type (V1.x: optional language / complementary code bytes)
            for (int i = 0; i + 4 <= n;) {
                const uint32_t sid = be16(d + i);
                const uint8_t f = d[i + 2];
                int j = i + 3;
                if ((f >> 5) & 1) j++;   // L flag (V1.x)
                if (j >= n) { malformed++; return; }
                const int code = d[j] & 31;
                j++;
                if ((f >> 4) & 1) j++;   // CC flag (V1.x)
                if (j > n) { malformed++; return; }
                auto it = e_.services.find(sid);
                if (it != e_.services.end()) {
                    it->second.pty = code;
                    it->second.pty_dynamic = f >> 7;
                }
                i = j;
            }
            return;
        }
        case 18: {   // announcement support
            for (int i = 0; i + 5 <= n;) {
                const uint32_t sid = be16(d + i);
                const unsigned asu = be16(d + i + 2);
                const int nc = d[i + 4] & 7;
                i += 5;
                if (i + nc > n) { malformed++; return; }
                auto it = e_.services.find(sid);
                if (it != e_.services.end()) {
                    it->second.asu = asu;
                    it->second.clusters.assign(d + i, d + i + nc);
                }
                i += nc;
            }
            return;
        }
        case 19: {   // announcement switching
            for (int i = 0; i + 4 <= n;) {
                const int cluster = d[i];
                const unsigned asw = be16(d + i + 1);
                const bool fresh = d[i + 3] >> 7, region = (d[i + 3] >> 6) & 1;
                const int sub = d[i + 3] & 63;
                i += 4 + (region ? 1 : 0);   // V1.x region id byte
                if (i > n) { malformed++; return; }
                if (oe) continue;
                if ((asw & 1) && !e_.alarm) continue;   // alarm only when the ensemble enables it
                auto it = e_.ann_active.find(cluster);
                if (asw) {
                    if (it == e_.ann_active.end() || it->second.first != asw || fresh) {
                        const bool news = it == e_.ann_active.end() || it->second.first != asw;
                        e_.ann_active[cluster] = {asw, sub};
                        if (news && ev.event) {
                            std::string who;
                            for (const auto& kv : e_.services)
                                for (const auto& k : kv.second.comps)
                                    if (k.tmid == 0 && k.subch == sub && !kv.second.label.empty()) who = kv.second.label;
                            char b[64];
                            snprintf(b, sizeof b, " (cluster %d, sub-channel %d", cluster, sub);
                            ev.event(std::string(asw & 1 ? "⚠ " : "") + "Announcement: " + announcement_names(asw) + b +
                                     (who.empty() ? "" : ", " + printable_utf8(who)) + ")");
                        }
                    }
                } else if (it != e_.ann_active.end()) {
                    e_.ann_active.erase(it);
                    if (ev.event) ev.event("Announcement ended (cluster " + std::to_string(cluster) + ")");
                }
            }
            return;
        }
        case 22: {   // TII database (EN 300 401 V1.4.1 clause 8.1.9; removed in V2)
            if (oe) return;
            for (int i = 0; i < n;) {
                const bool ms = d[i] >> 7;
                const int mainid = d[i] & 0x7F;
                if (!ms) {
                    if (i + 6 > n) { malformed++; return; }
                    TiiEntry t;
                    t.lat = s16(d + i + 1) * (90.0 / 32768.0) + (d[i + 5] >> 4) * (90.0 / 524288.0);
                    t.lon = s16(d + i + 3) * (180.0 / 32768.0) + (d[i + 5] & 15) * (180.0 / 524288.0);
                    t.main_ref = true;
                    if (mainid < 70 && std::fabs(t.lat) <= 90 && std::fabs(t.lon) <= 180) e_.tii_main[mainid] = t;
                    i += 6;
                } else {
                    if (i + 2 > n) { malformed++; return; }
                    const int ns = d[i + 1] & 7;
                    i += 2;
                    for (int s = 0; s < ns; s++) {
                        if (i + 6 > n) { malformed++; return; }
                        const int subid = d[i] >> 3;
                        TiiEntry t;
                        t.td_us = ((d[i] & 7) << 8) | d[i + 1];
                        t.lat = s16(d + i + 2) * (90.0 / 524288.0);   // offsets from the MainId's reference
                        t.lon = s16(d + i + 4) * (180.0 / 524288.0);
                        if (mainid < 70 && subid >= 1 && subid <= 23) e_.tii_sub[mainid * 32 + subid] = t;
                        i += 6;
                    }
                }
            }
            return;
        }
        default:
            return;
    }
}

void FigParser::set_label(int ext, uint32_t id, int scids, const std::string& label, const std::string& shortl, bool ext_label,
                          double now) {
    if (label.empty()) return;
    if (ext == 0) {
        if (e_.eid < 0 || static_cast<int>(id) != e_.eid) return;
        if (e_.label2 && !ext_label) return;   // the extended label wins
        e_.label = label;
        if (!shortl.empty()) e_.short_label = shortl;
        e_.label2 = ext_label;
    } else if (ext == 1 || ext == 5) {
        Service& s = service(e_.services, id, ext == 5, now);
        if (s.label2 && !ext_label) return;
        s.label = label;
        if (!shortl.empty()) s.short_label = shortl;
        s.label2 = ext_label;
    } else if (ext == 4) {
        auto it = e_.services.find(id);
        if (it == e_.services.end()) return;
        for (auto& k : it->second.comps)
            if (k.scids == scids) k.label = label;
    }
}

void FigParser::fig1(const uint8_t* d, int n, double now) {
    const int charset = d[0] >> 4, ext = d[0] & 7;
    int idlen = 0;
    uint32_t id = 0;
    int scids = -1;
    switch (ext) {
        case 0:
        case 1: idlen = 2; break;
        case 5: idlen = 4; break;
        case 4: idlen = n > 1 && (d[1] >> 7) ? 5 : 3; break;
        default: return;
    }
    if (n < 1 + idlen + 18) { malformed++; return; }
    const uint8_t* p = d + 1;
    if (ext == 4) {
        scids = p[0] & 15;
        id = (p[0] >> 7) ? be32(p + 1) : be16(p + 1);
    } else {
        id = idlen == 4 ? be32(p) : be16(p);
    }
    const uint8_t* chars = p + idlen;
    const unsigned flags = be16(chars + 16);
    const std::string label = label_to_utf8(chars, 16, charset);
    std::string shortl;
    if (charset == 0 && flags) {
        uint8_t sb[16];
        int ns = 0;
        for (int i = 0; i < 16; i++)
            if (flags & (0x8000u >> i)) sb[ns++] = chars[i];
        shortl = label_to_utf8(sb, ns, 0);
    }
    set_label(ext, id, scids, label, shortl, false, now);
}

void FigParser::fig2(const uint8_t* d, int n, double now) {
    const int toggle = d[0] >> 7, seg = (d[0] >> 4) & 7, ver = (d[0] >> 3) & 1, ext = d[0] & 7;
    if (!ver) return;   // version 1: deprecated (clause 5.2.2.3.4)
    int idlen = 0;
    switch (ext) {
        case 0:
        case 1: idlen = 2; break;
        case 5: idlen = 4; break;
        case 4: idlen = n > 1 && (d[1] >> 7) ? 5 : 3; break;
        default: return;
    }
    if (n < 1 + idlen + 1) { malformed++; return; }
    const uint8_t* p = d + 1;
    uint32_t id = 0;
    int scids = -1;
    if (ext == 4) {
        scids = p[0] & 15;
        id = (p[0] >> 7) ? be32(p + 1) : be16(p + 1);
    } else {
        id = idlen == 4 ? be32(p) : be16(p);
    }
    const uint64_t key = (static_cast<uint64_t>(ext) << 40) | (static_cast<uint64_t>(scids & 15) << 32) | id;
    if (fig2_.size() >= MAX_FIG2_SETS && !fig2_.count(key)) {
        // full: drop the sets not continued for a while, else the oldest
        for (auto it = fig2_.begin(); it != fig2_.end();) it = now - it->second.t > FIG2_MAX_AGE ? fig2_.erase(it) : std::next(it);
        if (fig2_.size() >= MAX_FIG2_SETS) {
            auto old = fig2_.begin();
            for (auto it = fig2_.begin(); it != fig2_.end(); ++it)
                if (it->second.t < old->second.t) old = it;
            fig2_.erase(old);
        }
    }
    Seg& s = fig2_[key];
    if (s.toggle != toggle) {
        s = Seg();
        s.toggle = toggle;
    }
    s.t = now;
    const uint8_t* body = p + idlen;
    int blen = n - 1 - idlen;
    if (seg == 0) {
        s.enc16 = body[0] >> 7;
        s.count = (body[0] >> 4) & 7;
        body++;
        blen--;
    }
    s.part[seg].assign(reinterpret_cast<const char*>(body), static_cast<size_t>(std::max(0, blen)));
    s.have |= static_cast<uint8_t>(1u << seg);
    if (s.count < 0) return;
    const uint8_t all = static_cast<uint8_t>((1u << (s.count + 1)) - 1);
    if ((s.have & all) != all) return;
    std::string raw;
    for (int i = 0; i <= s.count; i++) raw += s.part[i];
    const std::string label = label_to_utf8(reinterpret_cast<const uint8_t*>(raw.data()), static_cast<int>(raw.size()),
                                            s.enc16 ? 6 : 15);
    set_label(ext, id, scids, label, "", true, now);
}

}   // namespace dab
