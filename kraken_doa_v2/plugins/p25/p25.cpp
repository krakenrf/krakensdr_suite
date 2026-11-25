// P25 Phase 1 (TIA-102.BAAA) frame decoding: NID, HDU, LDU1/2 link control
// and encryption sync, TDULC, TSDU trunking signalling blocks and the PDU
// header. Voice: IMBE (dig_vocoder), muted unless the call is known to be clear.

#include "p25.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"
#include "p25_trunk.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace dig {

namespace {

const char* duid_name(int d) {
    switch (d) {
        case 0x0: return "HDU";
        case 0x3: return "TDU";
        case 0x5: return "LDU1";
        case 0x7: return "TSDU";
        case 0xA: return "LDU2";
        case 0xC: return "PDU";
        case 0xF: return "TDULC";
        default: return nullptr;
    }
}

std::string alg_name(int a) {
    switch (a) {
        case 0x80: return "clear";
        case 0x81: return "DES-OFB";
        case 0x83: return "3DES";
        case 0x84: return "AES-256";
        case 0x85: return "AES-128";
        case 0x9F: return "DES-XL";
        case 0xA0: return "DVI-XL";
        case 0xAA: return "ADP (RC4)";
        default: {
            char b[16];
            snprintf(b, sizeof b, "alg 0x%02X", a);
            return b;
        }
    }
}

std::string hex(uint64_t v, int digits) {
    char b[32];
    snprintf(b, sizeof b, "%0*llX", digits, static_cast<unsigned long long>(v));
    return b;
}

std::string mhz(uint64_t hz) {
    char b[32];
    snprintf(b, sizeof b, "%.5f MHz", static_cast<double>(hz) / 1e6);
    return b;
}

}  // namespace

void P25Proto::reset() {
    imbe_.reset();
    alg_ = -1;
    lc_prot_ = -1;
    held_.clear();
    iden_.clear();
    last_nac_ = -1;
    wacn_ = sysid_ = -1;
    call_desc_.clear();
    call_src_ = 0;
    call_zero_lcs_ = 0;
    call_voice_ = false;
    call_tgt_ = call_from_ = 0;
    call_grp_ = true;
    call_emerg_ = call_reported_ = false;
    // the host resets its own talker state together with ours
    call_start_ = -1;
    talker_id_.clear();
    talker_label_.clear();
}

// --- talkers (RxContext::talker) ----------------------------------------------
// A transmission = voice frames (HDU, LDU1, LDU2) less than gap() apart; its
// talker is the source unit ID of the LDU1 / TDULC link control. Reported
// with the frame boundaries, so the host can cut exactly this radio's
// samples out of the VFO's stream for the DoA.
void P25Proto::begin_call_if_new() {
    if (call_start_ >= 0 && frame_a_ - voice_end_ <= gap()) return;
    close_call(voice_end_);
    call_start_ = frame_a_;
    voice_end_ = frame_a_;
}

// The end of a transmission (terminator, a new header, frames stopped, or
// the start of the next one): its talker, its "Call ended" event (only if
// its start was announced) and its per-call state
void P25Proto::close_call(int64_t at) {
    end_talker(at);
    if (call_start_ >= 0 && !call_desc_.empty()) ctx_.report->event(Mode::P25, "Call ended: " + call_desc_, 0.0);
    if (call_start_ >= 0) ctx_.report->erase(Mode::P25, "Current call");
    // trunk following: the transmission is over (the channel may stay
    // assigned for the hang time - a TDULC's call termination releases it)
    if (call_reported_ && trunk)
        trunk->call(kp::Call::ENDED, call_tgt_ ? std::to_string(call_tgt_) : "", call_grp_,
                    call_from_ ? std::to_string(call_from_) : "", -1, encrypted(), call_emerg_);
    call_reported_ = false;
    call_tgt_ = call_from_ = 0;
    call_grp_ = true;
    call_emerg_ = false;
    call_start_ = -1;
    call_desc_.clear();
    call_src_ = 0;
    call_zero_lcs_ = 0;
    call_voice_ = false;
    alg_ = -1;
    lc_prot_ = -1;
    held_.clear();   // never known to be clear: never played
}

void P25Proto::voice_frame() {
    begin_call_if_new();
    call_voice_ = true;
    voice_end_ = std::max(voice_end_, frame_b_);
    if (!talker_id_.empty() && ctx_.talker) ctx_.talker(talker_id_, talker_label_, call_start_, voice_end_, 0);
}

void P25Proto::set_talker(uint32_t src, const std::string& label) {
    if (src == 0) return;   // no source given (some systems on talkgroup calls)
    begin_call_if_new();
    const std::string id = std::to_string(src);
    if (!talker_id_.empty() && talker_id_ != id) {
        // another radio without a terminator in between: the frames since the
        // old talker was last named can't be told apart - leave them out
        end_talker(lc_end_);
        call_start_ = frame_a_;
    }
    talker_id_ = id;
    talker_label_ = label;
    lc_end_ = frame_b_;
}

void P25Proto::end_talker(int64_t at) {
    if (!talker_id_.empty() && ctx_.talker_end) ctx_.talker_end(at, 0);
    talker_id_.clear();
}

void P25Proto::tick(int64_t now) {
    if (call_start_ < 0 || now - voice_end_ <= gap()) return;
    close_call(voice_end_);
}

std::string P25Proto::algorithm_name(int alg) { return alg_name(alg); }

std::string P25Proto::chan_str(uint32_t ch, bool tdma) const {
    int id = static_cast<int>(ch >> 12);
    uint32_t num = ch & 0xFFF;
    auto it = iden_.find(id);
    std::string s = std::to_string(id) + "-" + std::to_string(num);
    if (it != iden_.end() && it->second.spacing_hz) {
        uint32_t n = it->second.tdma ? num / static_cast<uint32_t>(std::max(1, it->second.slots)) : num;
        s += " (" + mhz(it->second.base_hz + static_cast<uint64_t>(n) * it->second.spacing_hz);
        if (tdma && it->second.tdma && it->second.slots > 1)
            s += ", LCH " + std::to_string(num % static_cast<uint32_t>(it->second.slots));
        s += ")";
    }
    return s;
}

void P25Proto::voice_activity() {
    if (!call_desc_.empty()) ctx_.report->set(Mode::P25, "Current call", call_desc_);
}

// trunk following: the open transmission's call as far as it is known (the
// header's talkgroup, the voice user link control's target / source /
// service options, the encryption as the muting decides it)
void P25Proto::report_call() {
    if (!trunk || call_start_ < 0) return;
    call_reported_ = true;
    trunk->call(kp::Call::ACTIVE, call_tgt_ ? std::to_string(call_tgt_) : "", call_grp_,
                call_from_ ? std::to_string(call_from_) : "", -1, encrypted(), call_emerg_);
}

// A voice user link control (group / unit-to-unit) of the open transmission:
// one "Voice:" event per transmission, naming its unit - the first link
// control that names one (a transmission's first LDU1 often names unit 0),
// or, when two have named none, the talkgroup alone. Another unit later in
// the same transmission (no terminator between) ends the first one's call,
// as set_talker ends its talker. A terminator's link control in hang time
// (no voice frames: often "TG n <- 0") opens and ends nothing.
void P25Proto::voice_user(uint32_t src, const std::string& desc, const std::string& desc_nosrc, bool prot) {
    if (call_start_ < 0 || !call_voice_) return;
    lc_prot_ = prot ? 1 : 0;
    Report& r = *ctx_.report;
    if (src != 0) {
        if (src == call_src_) return;
        if (call_src_ != 0) r.event(Mode::P25, "Call ended: " + call_desc_, 0.0);
        call_src_ = src;
        call_desc_ = desc;
    } else {
        if (!call_desc_.empty() || ++call_zero_lcs_ < 2) return;
        call_desc_ = desc_nosrc;
    }
    r.event(Mode::P25, "Voice: " + call_desc_, 0.0);
    voice_activity();
}

void P25Proto::link_control(const uint8_t* lc, const char* where) {
    int lco = lc[0] & 0x3F;
    int mfid = lc[1];
    bool std_mfid = (mfid == 0x00 || mfid == 0x01);
    // implicit-MFID forms (SF bit set): the 8 octets after the LCO are the
    // body of the trunking block of the same message (as Phase 2's
    // abbreviated messages) - established on real terminators / LDU1s against
    // the same site's control channel: adjacent sites (8 of 8 sites and their
    // channels), the site's own RFSS status, and channel updates naming the
    // talkgroups and channels its grants name
    const bool sf = lc[0] & 0x40;
    const int as_tsbk = !sf ? -1 : lco == 0x02 ? 0x02 : lco == 0x22 ? 0x3C : lco == 0x23 ? 0x3A : -1;
    if (lco == 0x00 && std_mfid) {
        bool emerg = lc[2] & 0x80, enc = lc[2] & 0x40;
        uint32_t tg = (lc[4] << 8) | lc[5];
        uint32_t src = (lc[6] << 16) | (lc[7] << 8) | lc[8];
        std::string flags = std::string(enc ? " [encrypted]" : "") + (emerg ? " [EMERGENCY]" : "");
        if (call_start_ >= 0 && call_voice_) {   // the open transmission's (not a hang-time terminator's)
            call_tgt_ = tg;
            call_grp_ = true;
            call_emerg_ = emerg;
            if (src) call_from_ = src;
        }
        set_talker(src, "TG " + std::to_string(tg) + (emerg ? " EMERGENCY" : ""));
        voice_user(src, "TG " + std::to_string(tg) + " <- " + std::to_string(src) + flags, "TG " + std::to_string(tg) + flags,
                   enc);
        ctx_.report->set(Mode::P25, "Last talkgroup", std::to_string(tg));
        if (src) ctx_.report->set(Mode::P25, "Last source", std::to_string(src));
    } else if (lco == 0x03 && std_mfid) {
        uint32_t dst = (lc[3] << 16) | (lc[4] << 8) | lc[5];
        uint32_t src = (lc[6] << 16) | (lc[7] << 8) | lc[8];
        bool enc = lc[2] & 0x40;
        std::string flags = enc ? " [encrypted]" : "";
        if (call_start_ >= 0 && call_voice_) {
            call_tgt_ = dst;
            call_grp_ = false;
            call_emerg_ = lc[2] & 0x80;
            if (src) call_from_ = src;
        }
        set_talker(src, "unit call to " + std::to_string(dst));
        voice_user(src, "Unit " + std::to_string(src) + " -> unit " + std::to_string(dst) + flags,
                   "Unit call to " + std::to_string(dst) + flags, enc);
    } else if (lco == 0x0F) {
        ctx_.report->event(Mode::P25, std::string("Call termination (") + where + ")", 2.0);
        release_lc_ = true;
    } else if (as_tsbk >= 0) {
        uint8_t t[10];
        t[0] = static_cast<uint8_t>(as_tsbk);
        t[1] = 0x00;
        std::memcpy(t + 2, lc + 1, 8);
        tsbk(t);
    } else if (ctx_.opts->verbose) {
        static const struct { int lco; const char* name; } names[] = {
            {0x02, "group voice channel update"}, {0x04, "group voice channel update (explicit)"},
            {0x20, "system service broadcast"}, {0x21, "secondary control channel broadcast"},
            {0x22, "adjacent site status"}, {0x23, "RFSS status"}, {0x24, "network status"},
            {0x25, "protection parameter broadcast"}};
        const char* n = nullptr;
        for (const auto& x : names) if (x.lco == lco && std_mfid) n = x.name;
        uint64_t v = 0;
        for (int i = 2; i < 9; i++) v = (v << 8) | lc[i];
        ctx_.report->event(Mode::P25, std::string("LC ") + where + ": " + (n ? n : "LCO 0x" + hex(lco, 2)) +
                                          " MFID 0x" + hex(mfid, 2) + " " + hex(v, 14), 5.0);
    }
}

void P25Proto::tsbk(const uint8_t* t, bool tdma) {
    int op = t[0] & 0x3F;
    int mfid = t[1];
    uint64_t a = 0;
    for (int i = 2; i < 10; i++) a = (a << 8) | t[i];
    auto f = [&](int hi, int len) { return static_cast<uint32_t>((a >> (hi - len + 1)) & ((1ULL << len) - 1)); };
    // a: bit 63 = MSB of byte 2
    Report& r = *ctx_.report;
    const double bcast = ctx_.opts->verbose ? 5.0 : 120.0;
    auto uid = [](uint32_t u) { return u ? std::to_string(u) : std::string(); };   // 0 = not in the message
    if (mfid == 0x90 && (op == 0x02 || op == 0x03)) {
        // [GPL-fact OP25 / SDRTrunk docs] Motorola (MFID 0x90) patch / group
        // regroup voice channel grant 0x02 (service options, channel,
        // supergroup, source - the layout of GRP_V_CH_GRANT) and its update
        // 0x03 (2 x channel + supergroup - GRP_V_CH_GRANT_UPDT's). Seen on
        // air (a Motorola site, 2026-11-20): an 0x03 named the supergroup
        // the same site's regroup command (MFID 0x90 opcode 0x00) had just
        // formed, on a channel of its identifier tables; 0x02 not seen
        // [UNVERIFIED]
        if (op == 0x02) {
            uint32_t opts = f(63, 8), ch = f(55, 16), sg = f(39, 16), src = f(23, 24);
            std::string e = "Patch grant: supergroup " + std::to_string(sg) + " <- " + std::to_string(src) + " on " +
                            chan_str(ch, tdma);
            if (opts & 0x40) e += " [encrypted]";
            if (opts & 0x80) e += " [EMERGENCY]";
            r.event(Mode::P25, e, 3.0);
            if (trunk) trunk->grant(ch, std::to_string(sg), true, uid(src), static_cast<int>(opts), false);
        } else {
            uint32_t c1 = f(63, 16), g1 = f(47, 16), c2 = f(31, 16), g2 = f(15, 16);
            r.event(Mode::P25, "Patch grant update: supergroup " + std::to_string(g1) + " on " + chan_str(c1, tdma) +
                                   (g2 != g1 || c2 != c1 ? ", supergroup " + std::to_string(g2) + " on " + chan_str(c2, tdma) : ""),
                    10.0);
            if (trunk) {
                trunk->grant(c1, std::to_string(g1), true, "", -1, true);
                if ((g2 != g1 || c2 != c1) && (g2 || c2)) trunk->grant(c2, std::to_string(g2), true, "", -1, true);
            }
        }
        return;
    }
    if (mfid != 0x00 && mfid != 0x01) {
        r.event(Mode::P25, "TSBK MFID 0x" + hex(mfid, 2) + " opcode 0x" + hex(op, 2) + " " + hex(a, 16),
                ctx_.opts->verbose ? 5.0 : 60.0);
        return;
    }
    // Grants (layouts: TIA-102.AABC, the trunking control channel messages):
    // each also goes to trunk following (P25Trunk::grant), every one heard
    // - the host takes the repeats as the call's keep-alive
    switch (op) {
        case 0x00: {   // GRP_V_CH_GRANT
            uint32_t opts = f(63, 8), ch = f(55, 16), grp = f(39, 16), src = f(23, 24);
            std::string e = "Grant: TG " + std::to_string(grp) + " <- " + std::to_string(src) + " on " + chan_str(ch, tdma);
            if (opts & 0x40) e += " [encrypted]";
            if (opts & 0x80) e += " [EMERGENCY]";
            r.event(Mode::P25, e, 3.0);
            r.set(Mode::P25, "Last grant", "TG " + std::to_string(grp) + " on " + chan_str(ch, tdma));
            if (trunk) trunk->grant(ch, std::to_string(grp), true, uid(src), static_cast<int>(opts), false);
            break;
        }
        case 0x02: {   // GRP_V_CH_GRANT_UPDT
            uint32_t c1 = f(63, 16), g1 = f(47, 16), c2 = f(31, 16), g2 = f(15, 16);
            r.event(Mode::P25, "Grant update: TG " + std::to_string(g1) + " on " + chan_str(c1, tdma) +
                                   (g2 != g1 || c2 != c1 ? ", TG " + std::to_string(g2) + " on " + chan_str(c2, tdma) : ""),
                    10.0);
            if (trunk) {
                trunk->grant(c1, std::to_string(g1), true, "", -1, true);
                if ((g2 != g1 || c2 != c1) && (g2 || c2)) trunk->grant(c2, std::to_string(g2), true, "", -1, true);
            }
            break;
        }
        case 0x03: {   // GRP_V_CH_GRANT_UPDT_EXP: options, -, channel T (the downlink), channel R, group
            uint32_t opts = f(63, 8), ch = f(47, 16), chr = f(31, 16), grp = f(15, 16);
            r.event(Mode::P25, "Grant update: TG " + std::to_string(grp) + " on " + chan_str(ch, tdma), 10.0);
            if (trunk) trunk->grant(ch, std::to_string(grp), true, "", static_cast<int>(opts), true, false, chr);
            break;
        }
        case 0x04:     // UU_V_CH_GRANT: channel, target, source (no service options)
        case 0x06: {   // UU_V_CH_GRANT_UPDT: the same layout
            uint32_t ch = f(63, 16), dst = f(47, 24), src = f(23, 24);
            r.event(Mode::P25, std::string(op == 0x04 ? "Unit grant: " : "Unit grant update: ") + std::to_string(src) + " -> " +
                                   std::to_string(dst) + " on " + chan_str(ch, tdma),
                    op == 0x04 ? 3.0 : 10.0);
            if (trunk) trunk->grant(ch, std::to_string(dst), false, uid(src), -1, op == 0x06);
            break;
        }
        case 0x08:     // TELE_INT_CH_GRANT: options, channel, call timer, target
        case 0x09: {   // TELE_INT_CH_GRANT_UPDT: the same layout
            uint32_t opts = f(63, 8), ch = f(55, 16), tgt = f(23, 24);
            std::string e = std::string(op == 0x08 ? "Telephone grant: unit " : "Telephone grant update: unit ") +
                            std::to_string(tgt) + " on " + chan_str(ch, tdma);
            if (opts & 0x40) e += " [encrypted]";
            if (opts & 0x80) e += " [EMERGENCY]";
            r.event(Mode::P25, e, op == 0x08 ? 3.0 : 10.0);
            if (trunk) trunk->grant(ch, std::to_string(tgt), false, "", static_cast<int>(opts), op == 0x09);
            break;
        }
        case 0x28: {   // GRP_AFF_RSP
            uint32_t grp = f(39, 16), tgt = f(23, 24);
            r.event(Mode::P25, "Group affiliation: unit " + std::to_string(tgt) + " -> TG " + std::to_string(grp), 10.0);
            break;
        }
        case 0x2C: {   // U_REG_RSP
            uint32_t sid = f(55, 24), src = f(23, 24);
            (void)sid;
            r.event(Mode::P25, "Unit registration: " + std::to_string(src), 10.0);
            break;
        }
        case 0x2F:
            r.event(Mode::P25, "Unit de-registration ack: " + std::to_string(f(23, 24)), 10.0);
            break;
        case 0x20:
            r.event(Mode::P25, "Ack response: unit " + std::to_string(f(23, 24)), ctx_.opts->verbose ? 5.0 : 30.0);
            break;
        case 0x27:
            r.event(Mode::P25, "Deny response: unit " + std::to_string(f(23, 24)) + " reason 0x" + hex(f(55, 8), 2), 10.0);
            break;
        case 0x33:     // IDEN_UP_TDMA
        case 0x34:     // IDEN_UP_VU
        case 0x3D: {   // IDEN_UP
            int id = static_cast<int>(f(63, 4));
            Iden in;
            in.base_hz = static_cast<uint64_t>(f(31, 32)) * 5;
            in.spacing_hz = f(41, 10) * 125;
            if (op == 0x3D) {
                int off = static_cast<int>(f(50, 9));
                in.tx_offset_hz = static_cast<int64_t>(off & 0xFF) * 250000 * ((off & 0x100) ? 1 : -1);
            } else {
                int off = static_cast<int>(f(54, 13));
                in.tx_offset_hz = static_cast<int64_t>(off) * in.spacing_hz * (f(55, 1) ? 1 : -1);
            }
            if (op == 0x33) {
                in.tdma = true;
                static const int slots[16] = {1, 1, 1, 2, 4, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                in.slots = slots[f(59, 4)] ? slots[f(59, 4)] : 1;
            }
            bool fresh = !iden_.count(id) || iden_[id].base_hz != in.base_hz;
            const bool changed = !iden_.count(id) || iden_[id].base_hz != in.base_hz || iden_[id].spacing_hz != in.spacing_hz ||
                                 iden_[id].tx_offset_hz != in.tx_offset_hz || iden_[id].tdma != in.tdma || iden_[id].slots != in.slots;
            iden_[id] = in;
            if (changed && trunk) trunk->iden_learned(id);
            std::string d = "base " + mhz(in.base_hz) + ", step " + std::to_string(in.spacing_hz) + " Hz, tx offset " +
                            std::to_string(in.tx_offset_hz / 1000) + " kHz" + (in.tdma ? " (TDMA)" : "");
            r.set(Mode::P25, "Channel table " + std::to_string(id), d);
            if (fresh) r.event(Mode::P25, "Channel identifier " + std::to_string(id) + ": " + d, bcast);
            break;
        }
        case 0x3A: {   // RFSS_STS_BCST
            uint32_t sys = f(51, 12), rfss = f(39, 8), site = f(31, 8), ch = f(23, 16);
            r.set(Mode::P25, "System ID", hex(sys, 3));
            r.set(Mode::P25, "RFSS / Site", std::to_string(rfss) + " / " + std::to_string(site));
            r.set(Mode::P25, "Control channel", chan_str(ch, tdma));
            r.event(Mode::P25, "RFSS status: sys " + hex(sys, 3) + " RFSS " + std::to_string(rfss) + " site " +
                                   std::to_string(site) + " CC " + chan_str(ch, tdma), bcast);
            if (trunk) trunk->site(static_cast<int>(sys), static_cast<int>(rfss), static_cast<int>(site), ch);
            break;
        }
        case 0x3B: {   // NET_STS_BCST
            uint32_t wacn = f(55, 20), sys = f(35, 12), ch = f(23, 16);
            wacn_ = static_cast<int>(wacn);
            sysid_ = static_cast<int>(sys);
            r.set(Mode::P25, "WACN", hex(wacn, 5));
            r.set(Mode::P25, "System ID", hex(sys, 3));
            r.event(Mode::P25, "Network status: WACN " + hex(wacn, 5) + " sys " + hex(sys, 3) + " CC " + chan_str(ch, tdma), bcast);
            if (trunk) {
                trunk->network(wacn_, sysid_);
                trunk->announce(ch, 0xFFFF, sysid_);
            }
            break;
        }
        case 0x3C: {   // ADJ_STS_BCST
            uint32_t sys = f(51, 12), rfss = f(39, 8), site = f(31, 8), ch = f(23, 16);
            std::string d = "sys " + hex(sys, 3) + " RFSS " + std::to_string(rfss) + " site " + std::to_string(site) + " on " + chan_str(ch, tdma);
            r.set(Mode::P25, "Neighbour " + std::to_string(rfss) + "/" + std::to_string(site), chan_str(ch, tdma));
            r.event(Mode::P25, "Adjacent site: " + d, bcast);
            if (trunk) trunk->announce(ch, 0xFFFF, static_cast<int>(sys));
            break;
        }
        // Layouts below: the message names from Phase 2's list of the Phase 1
        // messages it carries, the fields placed as in their parsed siblings
        // and checked against each other on real control channels of five
        // systems (2026-11-19)
        case 0x39: {   // SCCB (implicit): RFSS, site, 2 x (channel, service class)
            // RFSS / site = the same site's RFSS_STS_BCST on 9 of 9 sites; the
            // channels are in the system's tables, several of them named as
            // control channels by the same system's ADJ_STS_BCSTs
            uint32_t rfss = f(63, 8), site = f(55, 8), c1 = f(47, 16), c2 = f(23, 16);
            r.event(Mode::P25, "Secondary control channel: RFSS " + std::to_string(rfss) + " site " + std::to_string(site) +
                                   " on " + chan_str(c1, tdma) + (c2 != c1 ? ", " + chan_str(c2, tdma) : ""), bcast);
            if (trunk) {
                trunk->announce(c1);
                if (c2 != c1) trunk->announce(c2);
            }
            break;
        }
        case 0x29: {   // SCCB (explicit): RFSS, site, downlink channel, -, uplink channel, service class
            // the two channels lie exactly the identifier's transmit offset apart
            uint32_t rfss = f(63, 8), site = f(55, 8), ct = f(47, 16), cr = f(23, 16);
            r.event(Mode::P25, "Secondary control channel: RFSS " + std::to_string(rfss) + " site " + std::to_string(site) +
                                   " on " + chan_str(ct, tdma) + ", uplink " + chan_str(cr, tdma), bcast);
            if (trunk) trunk->announce(ct, cr);
            break;
        }
        case 0x14: {   // SNDCP data channel grant: options, channel T, channel R, target
            // the channels are the ones the data channel announcements name;
            // R is 0xFFFF on every real one (not given)
            uint32_t opts = f(63, 8), ct = f(55, 16), cr = f(39, 16), tgt = f(23, 24);
            r.event(Mode::P25, "Data channel grant: unit " + std::to_string(tgt) + " on " + chan_str(ct, tdma) +
                                   (cr != 0xFFFF ? ", uplink " + chan_str(cr, tdma) : ""), 3.0);
            if (trunk) trunk->grant(ct, std::to_string(tgt), false, "", static_cast<int>(opts), false, true, cr);
            break;
        }
        case 0x16: {   // SNDCP data channel announcement (explicit): -, -, channel T, channel R, -
            uint32_t ct = f(47, 16), cr = f(31, 16);
            r.event(Mode::P25, "Data channel announcement: " + (ct == 0xFFFF ? std::string("no channel") : chan_str(ct, tdma)) +
                                   (cr != 0xFFFF ? ", uplink " + chan_str(cr, tdma) : ""), bcast);
            if (trunk && ct != 0xFFFF) trunk->announce(ct, cr);
            break;
        }
        case 0x2B: {   // LOC_REG_RSP: -, group, RFSS, site, target
            // RFSS / site = the broadcasting site's on 5 sites of 3 systems; the
            // groups are talkgroups the same systems grant / affiliate
            uint32_t grp = f(55, 16), rfss = f(39, 8), site = f(31, 8), tgt = f(23, 24);
            r.event(Mode::P25, "Location registration: unit " + std::to_string(tgt) + ", TG " + std::to_string(grp) + " (RFSS " +
                                   std::to_string(rfss) + " site " + std::to_string(site) + ")", 10.0);
            break;
        }
        case 0x38:
            r.event(Mode::P25, "System service broadcast", bcast);
            break;
        case 0x35:
            r.event(Mode::P25, "Time and date announcement", bcast);
            break;
        case 0x30:
            r.event(Mode::P25, "Synchronization broadcast (TDMA)", bcast);
            break;
        default:
            r.event(Mode::P25, "TSBK opcode 0x" + hex(op, 2) + " " + hex(a, 16), ctx_.opts->verbose ? 5.0 : 60.0);
            break;
    }
}

// The 9 IMBE frames of an LDU (status-free dibit offsets after the NID),
// deinterleaved into mbelib's [8][23] layout. Played only once the call is
// known to be clear: ALGID 0x80 (header / encryption sync) or a voice user
// link control without "protected". A call joined after its header whose
// first LDU1 carries another link control (adjacent site status, channel
// update - real encrypted calls: their first 180 ms played as noise) is held
// until the next LDU2's encryption sync says: clear - its frames play, none
// lost (the host's voice FIFO starts at 250 ms buffered: held and released,
// two LDUs reach it as when they come 180 ms apart); encrypted - dropped.
void P25Proto::voice_frames(const std::vector<uint8_t>& sf) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) {
        held_.clear();
        return;
    }
    if (encrypted()) {
        held_.clear();
        if (ctx_.voice_state) ctx_.voice_state(call_desc_ + (call_desc_.empty() ? "" : ": ") + "encrypted, muted");
        return;
    }
    static const int start[9] = {56, 128, 220, 312, 404, 496, 588, 680, 768};
    Ldu l{};
    for (int f = 0; f < 9; f++) {
        for (int j = 0; j < 72; j++) {
            size_t d = static_cast<size_t>(start[f] + j);
            if (2 * d + 1 >= sf.size()) return;
            l.fr[f][mbe_tables::iW[j]][mbe_tables::iX[j]] = static_cast<char>(sf[2 * d]);
            l.fr[f][mbe_tables::iY[j]][mbe_tables::iZ[j]] = static_cast<char>(sf[2 * d + 1]);
        }
    }
    if (!clear_known()) {
        if (held_.size() >= MAX_HELD) held_.erase(held_.begin());   // the oldest is lost
        held_.push_back(l);
        if (ctx_.voice_state)
            ctx_.voice_state((call_desc_.empty() ? std::string("Voice") : call_desc_) + ": encryption not known yet, held");
        return;
    }
    for (Ldu& h : held_) play_ldu(h);
    held_.clear();
    play_ldu(l);
}

void P25Proto::play_ldu(Ldu& l) {
    float pcm[9 * 160];
    int good = 0;
    for (int f = 0; f < 9; f++)
        if (imbe_.decode(l.fr[f], pcm + 160 * f) >= 0) good++;
    // the talker channel of this audio: Phase 1's transmissions are channel 0
    // (the plugin's Phase 2 LCHs are 1 / 2)
    if (ctx_.voice_channel) ctx_.voice_channel(0);
    if (ctx_.voice) ctx_.voice(Mode::P25, pcm, 9 * 160);
    if (ctx_.voice_state)
        ctx_.voice_state((call_desc_.empty() ? std::string("Voice") : call_desc_) + ": playing (IMBE, " +
                         std::to_string(good) + "/9 frames clean)");
}

bool P25Proto::decode(const SymSrc& s) {
    // frame dibit i (0 = first sync dibit) is symbol k = i - 23
    const int ND = FRAME_DIBITS;
    std::vector<uint8_t> raw(ND * 2);
    std::vector<float> soft(ND);
    for (int i = 0; i < ND; i++) {
        float v = s.sym(i - 23);
        soft[i] = v;
        uint8_t d = SymSrc::dibit(v);
        raw[2 * i] = d >> 1;
        raw[2 * i + 1] = d & 1;
    }
    // status symbols (dibit 35 + 36n) removed
    std::vector<uint8_t> sf;
    std::vector<float> sfs;
    sf.reserve(ND * 2);
    for (int i = 0; i < ND; i++) {
        if (i % 36 == 35) continue;
        sf.push_back(raw[2 * i]);
        sf.push_back(raw[2 * i + 1]);
        sfs.push_back(soft[i]);
    }
    // raw bit range [start, stop) with the status bits skipped
    auto raw_range = [&](int start, int stop) {
        Bits o;
        for (int i = start; i < stop; i++)
            if (i % 72 != 70 && i % 72 != 71) o.push_back(raw[i]);
        return o;
    };

    // NID: BCH(63,16) (dig_fec, one table shared with the 4FSK receiver,
    // which decodes it too for the frame's length), the last valid frame's
    // NAC tried first
    uint32_t msg = 0;
    const int best = p25_nid_decode(bits_to_u64(sf.data() + 48, 63), last_nac_, &msg);
    if (best > 9) return false;
    int nac = static_cast<int>(msg >> 4), duid = static_cast<int>(msg & 0xF);
    const char* dn = duid_name(duid);
    if (!dn) return false;
    if (ctx_.opts->p25_nac >= 0 && nac != ctx_.opts->p25_nac) return false;

    // this frame's samples (frame dibit 0 is centred 23 symbols before the
    // sync end); lengths incl. status symbols
    {
        int dibits = 864;
        switch (duid) {
            case 0x0: dibits = 396; break;   // HDU
            case 0x3: dibits = 72; break;    // TDU
            case 0xF: dibits = 216; break;   // TDULC
            default: break;
        }
        sps_ = s.sps;
        frame_a_ = s.sync_end - static_cast<int64_t>(47 * s.sps / 2);
        frame_b_ = frame_a_ + static_cast<int64_t>(dibits) * s.sps;
    }

    Report& r = *ctx_.report;
    // The NID alone is weak evidence (random data that happens to correlate
    // with the sync - e.g. D-STAR's binary symbols - can land near a
    // codeword): frames count as P25 when their payload FEC/CRC passes, or
    // for the payload-less TDU, when the NID is nearly clean.
    bool valid = duid == 0x3 && best <= 3;
    release_lc_ = false;

    switch (duid) {
        case 0x7: {   // TSDU: up to 3 TSBKs
            int ok = 0;
            for (int b = 0; b < 3; b++) {
                int bit0 = 112 + 196 * b;
                if (bit0 / 2 + 98 > static_cast<int>(sfs.size())) break;
                uint8_t t[12];
                float metric;
                p25_trellis_half(sfs.data() + bit0 / 2, t, &metric);
                uint16_t crc = crc16_ccitt_bytes(t, 10, 0x0000);
                if (crc != ((t[10] << 8) | t[11])) break;
                ok++;
                // trunk following: a control channel, its NAC (the grants'
                // option) known before the block's grants go out
                if (ok == 1 && trunk) {
                    trunk->nac(nac);
                    trunk->control_seen();
                }
                tsbk(t);
                if (t[0] & 0x80) break;   // last block
            }
            if (ok) {
                valid = true;
                // a traffic channel that carries control signalling now has
                // no call any more (a control channel that moved here)
                if (trunk) trunk->release(-1);
                r.set(Mode::P25, "Channel type", "Control channel (trunked)");
                // the frame ends after its last good block (status symbols: 1 in 36 dibits)
                frame_b_ = frame_a_ + static_cast<int64_t>((56 + 98 * ok) * 36 / 35 + 1) * sps_;
            }
            break;
        }
        case 0x5:     // LDU1: link control
        case 0xA: {   // LDU2: encryption sync
            static const int seg[6][2] = {{410, 452}, {600, 640}, {788, 830}, {978, 1020}, {1168, 1208}, {1356, 1398}};
            uint8_t hexs[24];
            int h = 0;
            for (const auto& sg : seg) {
                Bits b = raw_range(sg[0], sg[1]);
                for (int w = 0; w < 4 && h < 24; w++) {
                    uint32_t m;
                    hamming_10_6().decode(bits_to_u32(b.data() + 10 * w, 10), &m, 1);
                    hexs[h++] = static_cast<uint8_t>(m);
                }
            }
            int nroots = duid == 0x5 ? 12 : 8;
            int e = rs64_decode(hexs, 24, nroots);
            if (e >= 0) {
                valid = true;
                // a frame of a new transmission: the previous one's call
                // ends before this frame's link control / encryption sync
                // belong to the new one
                begin_call_if_new();
                call_voice_ = true;
                uint8_t bits[72], bytes[9];
                for (int i = 0; i < 12; i++) u64_to_bits(hexs[i], 6, bits + 6 * i);
                if (duid == 0x5) {
                    pack_bits(bits, 72, bytes);
                    link_control(bytes, "LDU1");
                } else {
                    uint8_t ebits[96], eb[12];
                    for (int i = 0; i < 16; i++) u64_to_bits(hexs[i], 6, ebits + 6 * i);
                    pack_bits(ebits, 96, eb);
                    int alg = eb[9];
                    alg_ = alg;
                    uint32_t kid = (eb[10] << 8) | eb[11];
                    r.set(Mode::P25, "Encryption", alg == 0x80 ? "clear" : alg_name(alg) + ", key 0x" + hex(kid, 4));
                }
            }
            if (valid) {
                voice_frame();
                voice_activity();
                r.set(Mode::P25, "Channel type", "Voice");
            }
            if (valid) voice_frames(sf);
            if (valid) report_call();
            break;
        }
        case 0x0: {   // HDU
            Bits b = raw_range(114, 780);
            uint8_t hexs[36];
            for (int i = 0; i < 36; i++) {
                uint32_t m;
                golay18().decode(bits_to_u32(b.data() + 18 * i, 18), &m, 3);
                hexs[i] = static_cast<uint8_t>(m);
            }
            if (rs64_decode(hexs, 36, 16) >= 0) {
                valid = true;
                uint8_t bits[120], by[15];
                for (int i = 0; i < 20; i++) u64_to_bits(hexs[i], 6, bits + 6 * i);
                pack_bits(bits, 120, by);
                int alg = by[10];
                uint32_t kid = (by[11] << 8) | by[12], tg = (by[13] << 8) | by[14];
                std::string d = "TG " + std::to_string(tg);
                if (alg != 0x80) d += " [" + alg_name(alg) + "]";
                r.set(Mode::P25, "Encryption", alg == 0x80 ? "clear" : alg_name(alg) + ", key 0x" + hex(kid, 4));
                r.event(Mode::P25, "Call header: " + d, 3.0);
                // a new transmission starts here, its talker comes with the first LDU1
                if (call_start_ >= 0) close_call(voice_end_);
                voice_frame();
                alg_ = alg;
                voice_activity();
                if (tg) {   // a unit-to-unit call's header carries none: its link control names the called unit
                    call_tgt_ = tg;
                    call_grp_ = true;
                }
                report_call();
            }
            break;
        }
        case 0xF: {   // TDULC: 12 Golay(24,12) words -> RS(24,12,13)
            uint8_t hexs[24];
            bool g_ok = true;
            for (int i = 0; i < 12; i++) {
                uint32_t m;
                if (golay24().decode(bits_to_u32(sf.data() + 112 + 24 * i, 24), &m, 3) < 0) g_ok = false;
                hexs[2 * i] = static_cast<uint8_t>(m >> 6);
                hexs[2 * i + 1] = static_cast<uint8_t>(m & 0x3F);
            }
            if (g_ok && rs64_decode(hexs, 24, 12) >= 0) {
                valid = true;
                uint8_t bits[72], by[9];
                for (int i = 0; i < 12; i++) u64_to_bits(hexs[i], 6, bits + 6 * i);
                pack_bits(bits, 72, by);
                link_control(by, "TDULC");
            }
            [[fallthrough]];
        }
        case 0x3:     // TDU
            if (!valid) break;
            // the end of the transmission (in hang time a terminator ends nothing)
            if (call_start_ >= 0) close_call(std::max(voice_end_, frame_a_));
            // trunk following: a terminator ends the transmission (above);
            // the call termination link control (TDULC LCO 0x0F) is the
            // system clearing the channel. Only for a call this instance
            // reported: one heard first may be the previous call's clear-down
            // on a channel just granted again (the new call comes next)
            if (release_lc_ && trunk) trunk->release(-1);
            imbe_.reset();
            if (ctx_.voice_state) ctx_.voice_state("");
            r.erase(Mode::P25, "Current call");
            break;
        case 0xC: {   // PDU header block
            uint8_t t[12];
            float metric;
            p25_trellis_half(sfs.data() + 56, t, &metric);
            if (crc16_ccitt_bytes(t, 10, 0x0000) == ((t[10] << 8) | t[11])) {
                valid = true;
                int fmt = t[1] & 0x1F, sap = t[1] & 0x3F;
                uint32_t llid = (t[3] << 16) | (t[4] << 8) | t[5];
                (void)fmt;
                r.event(Mode::P25, "Data packet: SAP 0x" + hex(sap, 2) + " LLID " + std::to_string(llid) +
                                       " blocks " + std::to_string(t[6] & 0x7F), 3.0);
            }
            break;
        }
    }
    if (valid && trunk) trunk->nac(nac);
    if (valid) {
        if (nac != last_nac_) {
            last_nac_ = nac;
            r.event(Mode::P25, "NAC 0x" + hex(nac, 3), 30.0);
        }
        r.set(Mode::P25, "NAC", "0x" + hex(nac, 3));
        r.set(Mode::P25, "Last frame", dn);
    }
    if (valid) report_valid(ctx_, Mode::P25, frame_a_, frame_b_);
    if (valid && ctx_.freq_error) ctx_.freq_error(Mode::P25, s.center);
    return valid;
}

}  // namespace dig
