// DMR trunking (ETSI TS 102 361-4 Tier III; TS 102 361-1 for the CACH short
// LC and the MBC framing): the system identity, channel grants and
// announcements of a trunked station control channel (TSCC) and what a
// payload channel carries, reported to kraken_doa's trunk following through
// kp::Host::trunk_system / trunk_channel / control_channel / grant / call.
// Every PDU here has passed its FEC and CRC (BPTC(196,96) + CRC-CCITT with
// the CSBK / MBC header mask, the MBC last block's CRC over the
// continuation blocks; CACH: Hamming(17,12) rows + column parity + CRC-8)
// before it gets here.

#include "dmr.hpp"
#include "dig_fec.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dig {

namespace {

std::string mhz(double hz) {
    char b[32];
    snprintf(b, sizeof b, "%.6f MHz", hz / 1e6);
    return b;
}

const char* model_name(int m) {
    static const char* n[4] = {"Tiny", "Small", "Large", "Huge"};
    return n[m & 3];
}

// C_SYScode (TS 102 361-4 6.3.2.2.1.1, figure 6.15): MODEL(2) NET SITE PAR(2);
// the model sets the split of the 12 NET + SITE bits (table 6.5)
struct SysCode {
    int model = 0, net = 0, site = 0, par = 0;
};
SysCode sys_code(int v) {
    static const int site_bits[4] = {3, 5, 8, 10};
    SysCode s;
    s.model = (v >> 14) & 3;
    s.par = v & 3;
    const int sb = site_bits[s.model];
    s.site = (v >> 2) & ((1 << sb) - 1);
    s.net = (v >> (2 + sb)) & ((1 << (12 - sb)) - 1);
    return s;
}

std::string sys_text(int v) {
    SysCode s = sys_code(v);
    return std::string(model_name(s.model)) + " network " + std::to_string(s.net) + " site " + std::to_string(s.site);
}

// A Tier III gateway / special identity (TS 102 361-4 A.4) by its alias
const char* gateway_name(uint32_t id) {
    switch (id) {
        case 0xFFFEC0: return "PSTNI";
        case 0xFFFEC1: return "PABXI";
        case 0xFFFEC2: return "LINEI";
        case 0xFFFEC3: return "IPI";
        case 0xFFFEC4: return "SUPLI";
        case 0xFFFEC5: return "SDMI";
        case 0xFFFEC6: return "REGI";
        case 0xFFFEC7: return "MSI";
        case 0xFFFEC9: return "DIVERTI";
        case 0xFFFECA: return "TSI";
        case 0xFFFECB: return "DISPATI";
        case 0xFFFECC: return "STUNI";
        case 0xFFFECD: return "AUTHI";
        case 0xFFFECE: return "GPI";
        case 0xFFFECF: return "KILLI";
        case 0xFFFED0: return "PSTNDI";
        case 0xFFFED1: return "PABXDI";
        case 0xFFFED2: return "LINEDI";
        case 0xFFFED3: return "DISPATDI";
        case 0xFFFED4: return "ALLMSI";
        case 0xFFFED5: return "IPDI";
        case 0xFFFED6: return "DGNAI";
        case 0xFFFED7: return "TATTSI";
        case 0xFFFFFD: return "ALLMSIDL";
        case 0xFFFFFE: return "ALLMSIDZ";
        case 0xFFFFFF: return "ALLMSID";
        default: return nullptr;
    }
}

std::string id_text(uint32_t id) {
    const char* g = gateway_name(id);
    return g ? std::to_string(id) + " (" + g + ")" : std::to_string(id);
}

// Appended absolute parameters (CG_AP / MV_AP / VN_AP / BC_AP: TS 102 361-4
// tables 7.16, 7.18, 7.72, 7.21, CdefParms 7.2.42 + annex C.1.1.4). The
// layout of their bits 16..79 is the same in all four; CG_AP / MV_AP / VN_AP
// carry the destination's colour code in bits 12..15, BC_AP doesn't.
struct Absolute {
    bool ok = false;
    int chan = 0;             // the logical channel these frequencies belong to (CHAN)
    double rx_hz = NAN;       // the MS receive frequency = the DOWNLINK
    double tx_hz = NAN;       // the MS transmit frequency = the uplink
    int cc = -1;
};
Absolute absolute(const uint8_t* ap, int csbko, bool has_cc) {
    Absolute a;
    if (!ap) return a;
    uint8_t b[96];
    for (int i = 0; i < 80; i++) b[i] = (ap[i / 8] >> (7 - i % 8)) & 1;
    if ((ap[0] & 0x3F) != csbko) return a;        // "the CSBKO of the first block of the MBC"
    if (bits_to_u32(b + 16, 4) != 0) return a;    // Cdeftype 0 is the only one defined
    a.chan = static_cast<int>(bits_to_u32(b + 22, 12));
    uint32_t txm = bits_to_u32(b + 34, 10), txk = bits_to_u32(b + 44, 13);
    uint32_t rxm = bits_to_u32(b + 57, 10), rxk = bits_to_u32(b + 67, 13);
    // annex C table C.7: 50 .. 999 MHz, the kHz part in 125 Hz steps below 1 MHz
    if (txm < 50 || txm > 999 || rxm < 50 || rxm > 999 || txk > 7999 || rxk > 7999) return a;
    a.tx_hz = txm * 1e6 + txk * 125.0;
    a.rx_hz = rxm * 1e6 + rxk * 125.0;
    if (has_cc) a.cc = static_cast<int>(bits_to_u32(b + 12, 4));
    a.ok = true;
    return a;
}

// Hamming(17,12,3) of the CACH BPTC (TS 102 361-1 B.3.3): g(x) = x^5 + x^2 + 1
const BlockCode& hamming_17_12() {
    static const BlockCode c(17, 12, [](uint32_t m) {
        uint32_t r = m << 5;
        for (int i = 16; i >= 5; i--)
            if (r & (1u << i)) r ^= 0x25u << (i - 5);
        return (m << 5) | (r & 0x1F);
    });
    return c;
}

// CRC-8 of the short LC (TS 102 361-1 B.3.7): x^8 + x^2 + x + 1, init 0, no inversion
uint8_t crc8_slc(uint32_t v28) {
    uint32_t r = 0;
    for (int i = 27; i >= 0; i--) {
        uint32_t fb = ((r >> 7) & 1) ^ ((v28 >> i) & 1);
        r = (r << 1) & 0xFF;
        if (fb) r ^= 0x07;
    }
    return static_cast<uint8_t>(r);
}

}  // namespace

// --- system identity -------------------------------------------------------
void DmrProto::system_seen(int code, bool control, bool from_slc) {
    if (from_slc && syscode_ >= 0 && (syscode_ >> 2) == (code >> 2)) code = syscode_;   // the short LC has no PAR
    const double now = host_ ? host_->time() : 0;
    if (control) tscc_t_ = now;
    bool ctl = control || (tscc_t_ > -1e8 && now - tscc_t_ < 5.0);
    bool changed = code != syscode_ || ctl != sys_control_;
    syscode_ = code;
    sys_control_ = ctl;
    SysCode s = sys_code(code);
    ctx_.report->set(Mode::DMR, "System", std::string("Tier III trunking (") + (ctl ? "control channel" : "payload channel") + ")");
    ctx_.report->set(Mode::DMR, "Network", std::string(model_name(s.model)) + " network " + std::to_string(s.net));
    ctx_.report->set(Mode::DMR, "Site", std::to_string(s.site) + (from_slc ? "" : " (PAR " + std::to_string(s.par) + ")"));
    if (changed) {
        char hx[8];
        snprintf(hx, sizeof hx, "%04X", code);
        ctx_.report->event(Mode::DMR, std::string("Tier III ") + (ctl ? "control channel" : "payload channel") + ": " +
                                          sys_text(code) + " (system code 0x" + hx + ")", 30.0);
    }
    send_system(changed);
}

void DmrProto::send_system(bool force) {
    if (!host_ || syscode_ < 0) return;
    const double now = host_->time();
    if (!force && now - sys_sent_ < 5.0) return;
    SysCode s = sys_code(syscode_);
    kp::TrunkSystem t;
    // the NETWORK: model + network code (the same on every site); the site apart
    t.key = std::string(model_name(s.model)) + "." + std::to_string(s.net);
    t.name = std::string("DMR Tier III ") + model_name(s.model) + " network " + std::to_string(s.net);
    t.site = "site " + std::to_string(s.site);
    t.control = sys_control_;
    host_->trunk_system(t);
    sys_sent_ = now;
}

// --- CACH short LC (TS 102 361-1 7.1.4, B.2.3; TS 102 361-4 7.1.2) ------------
void DmrProto::cach_short_lc(const uint8_t* cach, int lcss) {
    static const int pos[17] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 15, 16, 17, 19, 20, 21, 23};
    // the four fragments come in consecutive CACH bursts (30 ms apart)
    const int64_t period = 144 * static_cast<int64_t>(sps_);
    bool next = slc_n_ >= 1 && std::llabs(burst_a_ - slc_at_ - period) <= 3 * sps_;
    if (lcss == 1) slc_n_ = 0;                       // first fragment
    else if (!(lcss == 3 || lcss == 2) || !next || slc_n_ < 1 || slc_n_ > 3 || (lcss == 2) != (slc_n_ == 3)) {
        slc_n_ = -1;                                 // no single-fragment short LC exists
        return;
    }
    for (int i = 0; i < 17; i++) slc_[slc_n_][i] = cach[pos[i]];
    slc_n_++;
    slc_at_ = burst_a_;
    if (lcss != 2) return;
    slc_n_ = -1;
    // transmit matrix (4 x 17, row = CACH burst) <- the encode matrix read by columns
    uint8_t m[4][17];
    for (int k = 0; k < 68; k++) m[k % 4][k / 4] = slc_[k / 17][k % 17];
    uint32_t rows[3];
    for (int r = 0; r < 3; r++) {
        uint32_t cw = 0, msg;
        for (int c = 0; c < 17; c++) cw = (cw << 1) | m[r][c];
        if (hamming_17_12().decode(cw, &msg, 1) < 0) return;
        uint32_t fixed = hamming_17_12().encode(msg);
        for (int c = 0; c < 17; c++) m[r][c] = (fixed >> (16 - c)) & 1;
        rows[r] = msg;
    }
    for (int c = 0; c < 17; c++)
        if ((m[0][c] ^ m[1][c] ^ m[2][c] ^ m[3][c]) != 0) return;   // column parity
    uint32_t lc28 = (rows[0] << 16) | (rows[1] << 4) | (rows[2] >> 8);
    if (crc8_slc(lc28) != (rows[2] & 0xFF)) return;
    short_lc(lc28);
}

void DmrProto::short_lc(uint32_t v) {
    const int slco = static_cast<int>(v >> 24);
    const uint32_t d = v & 0xFFFFFF;
    if (slco != 2 && slco != 3) return;              // Null / Activity Update / manufacturer: nothing to trunk
    // C_SYS_Parms (2) / P_SYS_Parms (3): MODEL(2) NET+SITE(12) Reg or
    // composite(1) Common_Slot_Counter(9) - the system code without its PAR
    const int code = static_cast<int>(((d >> 22) << 14) | (((d >> 10) & 0xFFF) << 2));
    if (slco == 3 && ((d >> 9) & 1)) ctx_.report->set(Mode::DMR, "Payload channel", "composite control channel");
    system_seen(code, slco == 2, true);
}

// --- channels ----------------------------------------------------------------
kp::TrunkChannel DmrProto::lcn_channel(int lcn, int ts) const {
    kp::TrunkChannel c;
    c.name = "LCN " + std::to_string(lcn);
    c.number = lcn;
    c.slot = ts;
    auto it = lcn_freq_.find(lcn);
    if (it != lcn_freq_.end()) {
        c.freq_hz = it->second.first;
        c.uplink_hz = it->second.second;
    }
    return c;
}

// a channel the system named (announcements, grants with absolute
// parameters): to the host's channel table - when new or changed, and again
// every 30 s (the announcements repeat every second or two)
void DmrProto::announce_channel(int lcn) {
    if (!host_ || lcn < 1 || lcn > 4094) return;
    const kp::TrunkChannel c = lcn_channel(lcn, -1);
    const double now = host_->time();
    auto it = chan_sent_.find(lcn);
    const double f = std::isfinite(c.freq_hz) ? c.freq_hz : -1;
    if (it != chan_sent_.end() && it->second.second == f && now - it->second.first < 30.0) return;
    chan_sent_[lcn] = {now, f};
    host_->trunk_channel(c);
}

// --- MBC assembly (TS 102 361-1 7.4) ---------------------------------------------
// dt 4 = header (its own CRC, mask 0xAAAA, checked by the caller), 5 =
// continuation; the last block (LB = 1) carries a CRC-CCITT over every block
// after the header (intermediate blocks: all 12 octets, the last one: 10),
// without a mask. The blocks follow each other on the slot.
void DmrProto::mbc_block(int slot, int dt, const uint8_t* by) {
    Slot& sl = slots_[slot];
    const bool lb = by[0] & 0x80;
    if (dt == 4) {
        sl.mbc_n = 0;
        if (lb) return;                              // a header always has more to come
        std::copy(by, by + 12, sl.mbc[0]);
        sl.mbc_n = 1;
        sl.mbc_at = burst_a_;
        return;
    }
    // the next burst of the same slot: 60 ms on a repeater, 30 ms .. 60 ms otherwise
    const int64_t d = burst_a_ - sl.mbc_at;
    if (sl.mbc_n < 1 || sl.mbc_n > 3 || d <= 0 || d > 300 * static_cast<int64_t>(sps_)) {
        sl.mbc_n = 0;
        return;
    }
    std::copy(by, by + 12, sl.mbc[sl.mbc_n]);
    sl.mbc_n++;
    sl.mbc_at = burst_a_;
    if (!lb) {
        if (sl.mbc_n >= 4) sl.mbc_n = 0;             // at most header + 2 intermediate + last
        return;
    }
    const int n = sl.mbc_n;
    sl.mbc_n = 0;
    uint8_t all[3 * 12];
    size_t len = 0;
    for (int b = 1; b < n - 1; b++) {
        std::copy(sl.mbc[b], sl.mbc[b] + 12, all + len);
        len += 12;
    }
    std::copy(sl.mbc[n - 1], sl.mbc[n - 1] + 10, all + len);
    len += 10;
    const uint8_t* last = sl.mbc[n - 1];
    if (crc16_ccitt_bytes(all, len, 0x0000) != ((last[10] << 8) | last[11])) {
        if (ctx_.opts->verbose && slot_wanted(slot))
            ctx_.report->event(Mode::DMR, slot_name(slot) + " MBC: CRC of the continuation blocks failed", 2.0);
        return;
    }
    const uint8_t* cont[3];
    for (int b = 1; b < n; b++) cont[b - 1] = sl.mbc[b];
    csbk(slot, sl.mbc[0], cont, n - 1);
}

// --- CSBK / MBC dispatch -------------------------------------------------------------
bool DmrProto::trunk_csbk(int slot, const uint8_t* c, const uint8_t* const* cont, int ncont) {
    const int op = c[0] & 0x3F, fid = c[1];
    // the appended block that carries the absolute parameters: the last one
    const uint8_t* ap = ncont >= 1 ? cont[ncont - 1] : nullptr;
    if (fid == 0x10) {
        capplus(slot, c);
        return false;
    }
    if (fid != 0) return false;
    // every Tier III PDU below comes from the TS: a BS-sourced burst (an MS
    // never sends them; a direct-mode burst is no TSCC)
    if (!burst_bs_) return false;
    switch (op) {
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
            tier3_grant(slot, c, ap);
            return true;
        case 0x19: {                                 // C_ALOHA (table 7.19)
            system_seen((c[5] << 8) | c[6], true, false);
            return false;
        }
        case 0x28:                                   // C_BCAST (table 7.20)
            tier3_bcast(slot, c, ap);
            return false;
        case 0x39:                                   // C_MOVE (table 7.17)
            tier3_move(slot, c, ap);
            return true;
        case 0x2E: case 0x2F:                        // P_CLEAR / P_PROTECT
            tier3_payload(slot, c);
            return true;
        default:
            return false;
    }
}

// Channel grants (TS 102 361-4 7.1.1.1.1, P_GRANT 7.1.1.3.1 - the same PDUs
// on a payload channel): LPCN(12) LCN(1) b2(1) Emergency(1) b0(1) Target(24)
// Source(24); b2 = Late_Entry (TV / BTV), HI_RATE (data), reserved (PV,
// PV_DX); b0 = Offset timing, or Call Direction (the duplex grants)
void DmrProto::tier3_grant(int slot, const uint8_t* c, const uint8_t* ap) {
    static const char* names[9] = {"PV_GRANT", "TV_GRANT", "BTV_GRANT", "PD_GRANT", "TD_GRANT",
                                   "PV_GRANT_DX", "PD_GRANT_DX", "PD_GRANT_MI", "TD_GRANT_MI"};
    const int op = c[0] & 0x3F;
    const bool lb = c[0] & 0x80;
    const int lpcn = (c[2] << 4) | (c[3] >> 4);
    const int ts = (c[3] >> 3) & 1;
    const bool b2 = c[3] & 4, emerg = c[3] & 2, b0 = c[3] & 1;
    uint32_t dst = (c[4] << 16) | (c[5] << 8) | c[6];
    uint32_t src = (c[7] << 16) | (c[8] << 8) | c[9];
    const bool group = op == 0x31 || op == 0x32 || op == 0x34 || op == 0x38;
    const bool data = op == 0x33 || op == 0x34 || op == 0x36 || op == 0x37 || op == 0x38;
    const bool duplex = op == 0x35 || op == 0x36;
    const bool late = (op == 0x31 || op == 0x32) && b2;
    // a duplex grant's Call Direction 1 = the target address is the CALLING
    // MS: the called one first, as in every other grant
    if (duplex && b0) std::swap(dst, src);
    const std::string name = names[op - 0x30];
    std::string where = "channel " + std::to_string(lpcn) + " slot " + std::to_string(ts + 1);
    kp::TrunkChannel ch;
    Absolute a;
    if (lpcn == 0) {
        // "the Physical Channel number is invalid"
        if (ctx_.opts->verbose && slot_wanted(slot))
            ctx_.report->event(Mode::DMR, slot_name(slot) + " " + name + ": invalid channel 0", 5.0);
        return;
    }
    if (lpcn == 0xFFF || !lb) {
        // the absolute frequencies follow in an appended block (CG_AP)
        a = absolute(ap, op, true);
        if (!a.ok) {
            if (ctx_.opts->verbose && slot_wanted(slot))
                ctx_.report->event(Mode::DMR, slot_name(slot) + " " + name + ": absolute channel parameters missing", 5.0);
            return;
        }
        if (a.chan >= 1 && a.chan <= 4094) {
            lcn_freq_[a.chan] = {a.rx_hz, a.tx_hz};
            ch = lcn_channel(a.chan, ts);
            announce_channel(a.chan);
            where = "channel " + std::to_string(a.chan) + " slot " + std::to_string(ts + 1);
        } else {
            ch.name = mhz(a.rx_hz);
            ch.freq_hz = a.rx_hz;
            ch.uplink_hz = a.tx_hz;
            ch.slot = ts;
            where = ch.name + " slot " + std::to_string(ts + 1);
        }
    } else {
        ch = lcn_channel(lpcn, ts);
    }
    std::string e = name + ": " + id_text(src) + " -> " + (group ? "TG " : "unit ") + id_text(dst) + " on " + where;
    if (late) e += " [late entry]";
    if (emerg) e += " [EMERGENCY]";
    if (!duplex && b0) e += " (offset timing)";
    if (data && (op == 0x33 || op == 0x34 || op == 0x37 || op == 0x38) && b2) e += " (dual slot data)";
    if (std::isfinite(ch.freq_hz)) {
        e += " (" + mhz(ch.freq_hz);
        if (std::isfinite(ch.uplink_hz)) e += ", uplink " + mhz(ch.uplink_hz);
        if (a.cc >= 0) e += ", CC " + std::to_string(a.cc);
        e += ")";
    }
    ctx_.report->event(Mode::DMR, slot_name(slot) + " " + e, 3.0);
    ctx_.report->set(Mode::DMR, "Last grant", (group ? "TG " : "unit ") + std::to_string(dst) + " on channel " +
                                                  std::to_string(ch.number >= 0 ? ch.number : lpcn) + "/" +
                                                  std::to_string(ts + 1));
    if (!host_) return;
    kp::Grant g;
    g.channel = ch;
    g.target = std::to_string(dst);
    g.group = group;
    g.source = std::to_string(src);
    g.emergency = emerg;
    g.data = data;
    g.update = late;
    // the payload channel's decoder: this plugin on the granted timeslot (and
    // only on the granted colour code when the grant names it)
    g.options.push_back({"slot", std::to_string(ts + 1)});
    if (a.cc >= 0) g.options.push_back({"cc", std::to_string(a.cc)});
    host_->grant(g);
}

// Announcements (TS 102 361-4 7.1.1.1.5, 7.2.19): Announcement_type(5)
// Parms1(14) Reg(1) Backoff(4) C_SYScode(16) Parms2(24)
void DmrProto::tier3_bcast(int slot, const uint8_t* c, const uint8_t* ap) {
    const int type = c[2] >> 3;
    const uint32_t p1 = ((c[2] & 7u) << 11) | (c[3] << 3) | (c[4] >> 5);
    const int code = (c[5] << 8) | c[6];
    const uint32_t p2 = (c[7] << 16) | (c[8] << 8) | c[9];
    system_seen(code, true, false);
    const double quiet = ctx_.opts->verbose ? 2.0 : 300.0;
    auto announce = [&](int lcn) { announce_channel(lcn); };
    // a neighbour's system code: the 14 most significant bits (its PAR unknown)
    auto neighbour = [&](uint32_t ms14) { return sys_text(static_cast<int>(ms14 << 2)); };
    switch (type) {
        case 0: {   // Ann_WD_TSCC (table 7.69): reserved(4) CC1(4) CC2(4) AW1(1) AW2(1) / CH1(12) CH2(12)
            const int cc1 = (p1 >> 6) & 15, cc2 = (p1 >> 2) & 15;
            const bool wd1 = p1 & 2, wd2 = p1 & 1;
            int ch1 = static_cast<int>(p2 >> 12), ch2 = static_cast<int>(p2 & 0xFFF);
            Absolute a;
            if (ap) {
                // the MBC form names one TSCC, its frequencies appended (BC_AP)
                a = absolute(ap, 0x28, false);
                if (a.ok && a.chan >= 1 && a.chan <= 4094) {
                    lcn_freq_[a.chan] = {a.rx_hz, a.tx_hz};
                    ch1 = a.chan;
                }
                ch2 = 0;
            }
            for (int k = 0; k < 2; k++) {
                const int ch = k ? ch2 : ch1;
                if (ch < 1 || ch > 4094) continue;
                const bool wd = k ? wd2 : wd1;
                if (!wd) announce(ch);
                ctx_.report->event(Mode::DMR, std::string("TSCC ") + (wd ? "withdrawn" : "announced") + ": channel " +
                                                  std::to_string(ch) + (wd ? "" : " CC " + std::to_string(k ? cc2 : cc1)) +
                                                  (k == 0 && a.ok ? " (" + mhz(a.rx_hz) + ", uplink " + mhz(a.tx_hz) + ")" : ""),
                                   quiet);
            }
            break;
        }
        case 1: {   // CallTimer_Parms (table 7.70)
            auto t = [](uint32_t v, uint32_t inf) {
                return v == 0 ? std::string("MS default") : v == inf ? std::string("none") : std::to_string(v) + " s";
            };
            ctx_.report->set(Mode::DMR, "Call timers", "emergency " + t(p1 >> 5, 511) + ", packet " + t(p1 & 31, 31) +
                                                          ", MS-MS " + t(p2 >> 12, 4095) + ", line " + t(p2 & 0xFFF, 4095));
            break;
        }
        case 2:     // Vote_Now (table 7.71): a neighbour's system code + its TSCC
        case 6: {   // Adjacent_Site (table 7.79): the same layout
            const int ch = static_cast<int>(p2 & 0xFFF);
            std::string what = neighbour(p1) + " on channel " + std::to_string(ch);
            if (type == 2 && ap && ch == 0xFFF) {
                Absolute a = absolute(ap, 0x28, true);
                if (a.ok) {
                    what = neighbour(p1) + " on " + mhz(a.rx_hz) + " (uplink " + mhz(a.tx_hz) + ", CC " + std::to_string(a.cc) + ")";
                    if (a.chan >= 1 && a.chan <= 4094) {
                        lcn_freq_[a.chan] = {a.rx_hz, a.tx_hz};
                        announce(a.chan);
                    }
                }
            } else {
                announce(ch);
            }
            if (p2 & 0x800000) what += (p2 & 0x400000) ? ", connected to the network" : ", no network connection";
            SysCode s = sys_code(static_cast<int>(p1 << 2));
            ctx_.report->set(Mode::DMR, "Adjacent site " + std::to_string(s.site), what);
            ctx_.report->event(Mode::DMR, std::string(type == 2 ? "Vote now: " : "Adjacent site: ") + what, quiet);
            break;
        }
        case 3: {   // Local_Time (table 7.73)
            const int day = static_cast<int>(p1 >> 9), month = static_cast<int>((p1 >> 5) & 15);
            const int utc_off = static_cast<int>((p1 >> 1) & 15), sign = static_cast<int>(p1 & 1);
            const int hh = static_cast<int>(p2 >> 19), mm = static_cast<int>((p2 >> 13) & 63), ss = static_cast<int>((p2 >> 7) & 63);
            const int frac = static_cast<int>((p2 >> 2) & 3);
            char b[96];
            if (utc_off == 15) snprintf(b, sizeof b, "%02d:%02d:%02d", hh, mm, ss);
            else snprintf(b, sizeof b, "%02d:%02d:%02d (UTC%c%d:%02d)", hh, mm, ss, sign ? '-' : '+', utc_off, frac * 15);
            std::string v = b;
            if (day && month) v = std::to_string(day) + "/" + std::to_string(month) + " " + v;
            ctx_.report->set(Mode::DMR, "Network time", v);
            break;
        }
        case 4:     // MassReg
            if (ctx_.opts->verbose)
                ctx_.report->event(Mode::DMR, "Mass registration invited (window " + std::to_string((p1 >> 5) & 15) + ")", 30.0);
            break;
        case 5: {   // Chan_Freq (table 7.78): CH(12) in Parms2, the frequencies in the BC_AP
            const int ch = static_cast<int>(p2 & 0xFFF);
            Absolute a = absolute(ap, 0x28, false);
            if (!a.ok) {
                if (ctx_.opts->verbose)
                    ctx_.report->event(Mode::DMR, "Channel announcement for channel " + std::to_string(ch) +
                                                      " without its absolute parameters", 5.0);
                break;
            }
            const int lcn = (ch >= 1 && ch <= 4094) ? ch : a.chan;
            if (lcn < 1 || lcn > 4094) break;
            lcn_freq_[lcn] = {a.rx_hz, a.tx_hz};
            announce(lcn);
            ctx_.report->set(Mode::DMR, "Channel " + std::to_string(lcn), mhz(a.rx_hz) + " (uplink " + mhz(a.tx_hz) + ")");
            ctx_.report->event(Mode::DMR, "Channel " + std::to_string(lcn) + ": " + mhz(a.rx_hz) + ", uplink " + mhz(a.tx_hz), quiet);
            break;
        }
        case 7: {   // Gen_Site_Params (table 7.80): Parms2 = site info(8) reserved(8) network info(8)
            const bool hib = (p2 >> 16) & 0x40, no_tg = p2 & 0x80;
            ctx_.report->set(Mode::DMR, "Site parameters", std::string(hib ? "TSCC about to hibernate" : "TSCC active") +
                                                              (no_tg ? ", registration without talkgroups" : ""));
            break;
        }
        default:
            break;
    }
    (void)slot;
}

// C_MOVE: reserved(9) Mask(5) reserved(5) Reg(1) Backoff(4) reserved(4)
// LPCN(12) MS address(24) - applicable to every MS when Mask = 0
void DmrProto::tier3_move(int slot, const uint8_t* c, const uint8_t* ap) {
    const int mask = (c[3] >> 2) & 31;
    int lpcn = ((c[5] & 15) << 8) | c[6];
    const uint32_t ms = (c[7] << 16) | (c[8] << 8) | c[9];
    kp::TrunkChannel ch;
    std::string where;
    if (lpcn == 0xFFF) {
        Absolute a = absolute(ap, 0x39, true);
        if (!a.ok) return;
        if (a.chan >= 1 && a.chan <= 4094) {
            lcn_freq_[a.chan] = {a.rx_hz, a.tx_hz};
            ch = lcn_channel(a.chan, -1);
        } else {
            ch.name = mhz(a.rx_hz);
            ch.freq_hz = a.rx_hz;
            ch.uplink_hz = a.tx_hz;
        }
        where = ch.name + " (" + mhz(a.rx_hz) + ", CC " + std::to_string(a.cc) + ")";
    } else if (lpcn >= 1) {
        ch = lcn_channel(lpcn, -1);
        where = "channel " + std::to_string(lpcn);
    } else {
        return;
    }
    const bool all = mask == 0;
    ctx_.report->event(Mode::DMR, slot_name(slot) + " C_MOVE: " + (all ? std::string("every radio") : "radios matching " + id_text(ms) + " (mask " + std::to_string(mask) + ")") +
                                      " to the TSCC on " + where, all ? 5.0 : (ctx_.opts->verbose ? 2.0 : 60.0));
    if (all && host_) host_->control_channel(ch);
}

// Payload-channel CSBKs (TS 102 361-4 7.1.1.3): P_CLEAR clears the slot
// (its radios return to the TSCC on LPCN, 0 = the one they came from),
// P_PROTECT controls who may transmit
void DmrProto::tier3_payload(int slot, const uint8_t* c) {
    const int op = c[0] & 0x3F;
    const uint32_t dst = (c[4] << 16) | (c[5] << 8) | c[6];
    const uint32_t src = (c[7] << 16) | (c[8] << 8) | c[9];
    const bool gi = c[3] & 1;
    if (op == 0x2E) {
        const int lpcn = (c[2] << 4) | (c[3] >> 4);
        if (slot_wanted(slot))
            ctx_.report->event(Mode::DMR, slot_name(slot) + " P_CLEAR: " + (gi ? "TG " : "") + id_text(dst) +
                                              " back to the " + (lpcn == 0 ? std::string("control channel") : "TSCC on channel " + std::to_string(lpcn)),
                               ctx_.opts->verbose ? 0.01 : 3.0);   // verbose: every repeat
        Slot& sl = slots_[slot];
        const double now = host_ ? host_->time() : 0;
        // the TS repeats it: one RELEASED per clear-down
        if (now - sl.cleared > 2.0 || sl.cstate != kp::Call::RELEASED) {
            if (sl.ctarget.empty() && gateway_name(dst) == nullptr) {
                // no call heard (a grant whose radio never keyed up): the P_CLEAR names it
                sl.ctarget = std::to_string(dst);
                sl.cgroup = gi;
            }
            call_report(slot, kp::Call::RELEASED);
        }
        sl.cleared = now;
        return;
    }
    static const char* kinds[8] = {"disable PTT", "enable PTT", "clear illegally parked radios", "enable PTT for one radio only",
                                   "reserved 4", "reserved 5", "reserved 6", "reserved 7"};
    const int kind = (c[3] >> 1) & 7;
    if (slot_wanted(slot))
        ctx_.report->event(Mode::DMR, slot_name(slot) + " P_PROTECT: " + kinds[kind] + " - " + (gi ? "TG " : "") + id_text(dst) +
                                          " / " + id_text(src), ctx_.opts->verbose ? 2.0 : 30.0);
}

// --- payload channel: the call per slot --------------------------------------------
void DmrProto::call_report(int slot, int state) {
    Slot& sl = slots_[slot];
    if (!host_ || !slot_wanted(slot)) {
        sl.cstate = state;
        return;
    }
    kp::Call c;
    c.state = state;
    c.target = sl.ctarget;
    c.group = sl.cgroup;
    c.source = sl.csource;
    c.slot = call_slot(slot);
    c.encrypted = sl.cenc;
    c.emergency = sl.cemerg;
    host_->call(c);
    sl.cstate = state;
    sl.csent = host_->time();
    if (state != kp::Call::ACTIVE) {
        // the next transmission is reported from what it carries itself
        sl.ctarget.clear();
        sl.csource.clear();
        sl.cgroup = true;
        sl.cenc = sl.cemerg = false;
    }
}

// a voice LC (header, embedded, terminator) named the call
void DmrProto::call_lc(int slot, bool group, uint32_t dst, uint32_t src, int so) {
    Slot& sl = slots_[slot];
    const std::string t = std::to_string(dst), s = std::to_string(src);
    const bool enc = (so & 0x40) || sl.privacy, em = so & 0x80;
    const bool changed = sl.cstate != kp::Call::ACTIVE || t != sl.ctarget || s != sl.csource || group != sl.cgroup ||
                         enc != sl.cenc || em != sl.cemerg;
    sl.ctarget = t;
    sl.csource = s;
    sl.cgroup = group;
    sl.cenc = enc;
    sl.cemerg = em;
    sl.clast = burst_b_;
    const double now = host_ ? host_->time() : 0;
    if (changed || now - sl.csent >= 1.0) call_report(slot, kp::Call::ACTIVE);
}

void DmrProto::call_voice(int slot) {
    Slot& sl = slots_[slot];
    sl.clast = burst_b_;
    if (sl.privacy && !sl.cenc && sl.cstate == kp::Call::ACTIVE) {
        sl.cenc = true;
        call_report(slot, kp::Call::ACTIVE);
        return;
    }
    if (sl.cstate == kp::Call::ACTIVE) return;
    sl.cenc = sl.privacy;
    call_report(slot, kp::Call::ACTIVE);
}

void DmrProto::call_tick(int64_t now) {
    // voice bursts stopped without a terminator: the transmission is over
    for (int i = 0; i < 3; i++)
        if (slots_[i].cstate == kp::Call::ACTIVE && now - slots_[i].clast > 48000) call_report(i, kp::Call::ENDED);
}

// --- Motorola Capacity Plus (FID 0x10) -------------------------------------------------
// Capacity Plus has no public standard. Idle radios wait on the REST
// channel; its CSBKO 0x3E ("channel status") says which channel is the rest
// channel. What is used here:
//   [GPL-fact DSD-FME / SDRTrunk] octet 2 = fragment(2) timeslot(1)
//   reserved(1) rest channel(4) - the timeslot bit CONFIRMED-RF (one 30 s
//   rest-channel recording: 0x3E on slot 1 carried 0, on slot 2 1, both
//   fragment bits set); [UNVERIFIED] that the low nibble is the rest
//   channel's number (that recording showed 1 on both slots and no traffic).
// Only a CRC-clean BS-sourced CSBK with FID 0x10 / CSBKO 0x3E whose timeslot
// bit matches the slot it came on is used. Not used (no recording with
// traffic here to check them against): the active-channel bitmap and group
// list that follow - so no grants; the host follows the calls of a Capacity
// Plus system only through the rest channel moving (control_channel).
void DmrProto::capplus(int slot, const uint8_t* c) {
    const int op = c[0] & 0x3F;
    if (op != 0x3E || !burst_bs_ || slot < 1) return;
    cp_seen_ = true;
    ctx_.report->set(Mode::DMR, "System", "Motorola Capacity Plus");
    const int ts = (c[2] >> 5) & 1, rest = c[2] & 15;
    if (ts != slot - 1 || rest == 0) return;
    ctx_.report->set(Mode::DMR, "Rest channel", std::to_string(rest));
    if (!host_) return;
    const double now = host_->time();
    // no network identity on air: the colour code is the best key there is
    // (stable, but two Capacity Plus systems on one colour code in reach
    // share it)
    if (cc_ >= 0 && (now - sys_sent_ >= 5.0 || sys_key_cp_ != cc_)) {
        kp::TrunkSystem t;
        t.key = "CapPlus.CC" + std::to_string(cc_);
        t.name = "Motorola Capacity Plus, colour code " + std::to_string(cc_);
        t.control = true;   // the rest channel: where idle radios wait and calls start
        host_->trunk_system(t);
        sys_sent_ = now;
        sys_key_cp_ = cc_;
    }
    if (cp_rest_ >= 0 && rest != cp_rest_) {
        kp::TrunkChannel ch;
        ch.name = "LCN " + std::to_string(rest);
        ch.number = rest;
        host_->control_channel(ch);
        ctx_.report->event(Mode::DMR, "Capacity Plus: the rest channel moved to channel " + std::to_string(rest), 5.0);
    }
    cp_rest_ = rest;
}

}  // namespace dig
