// DMR (ETSI TS 102 361-1/-2/-4) burst decoding: CACH (timeslot), slot type
// (colour code + data type), voice LC header / terminator (full LC), CSBK,
// data headers, EMB and the embedded LC carried by voice bursts B..E.
// Voice (AMBE+2) frames are counted, not synthesized.

#include "dmr.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <cstdio>

namespace dig {

namespace {

std::string hex(uint64_t v, int digits) {
    char b[32];
    snprintf(b, sizeof b, "%0*llX", digits, static_cast<unsigned long long>(v));
    return b;
}

const char* data_type_name(int dt) {
    static const char* n[16] = {"PI header", "Voice LC header", "Terminator with LC", "CSBK",
                                "MBC header", "MBC continuation", "Data header", "Rate 1/2 data",
                                "Rate 3/4 data", "Idle", "Rate 1 data", "Unified single block",
                                "Reserved", "Reserved", "Reserved", "Reserved"};
    return n[dt & 15];
}

const char* csbk_name(int op) {
    switch (op) {
        case 0x04: return "UU_V_Req";
        case 0x05: return "UU_Ans_Rsp";
        case 0x07: return "CT_CSBK";
        case 0x19: return "C_ALOHA";
        case 0x1A: return "C_UDT_DHDR";
        case 0x1C: return "C_AHOY";
        case 0x1E: return "C_ACKVIT";
        case 0x1F: return "C_RAND";
        case 0x20: return "C_ACKD";
        case 0x21: return "C_ACKU";
        case 0x22: return "P_ACKD";
        case 0x23: return "P_ACKU";
        case 0x28: return "C_BCAST";
        case 0x2A: return "P_MAINT";
        case 0x2E: return "C_MOVE";
        case 0x2F: return "P_CLEAR";
        case 0x30: return "PV_GRANT";
        case 0x31: return "TV_GRANT";
        case 0x32: return "BTV_GRANT";
        case 0x33: return "PD_GRANT";
        case 0x34: return "TD_GRANT";
        case 0x35: return "PV_GRANT_DX";
        case 0x36: return "PD_GRANT_DX";
        case 0x38: return "BS_Dwn_Act";
        case 0x3D: return "Preamble";
        default: return nullptr;
    }
}

const char* fid_name(int fid) {
    switch (fid) {
        case 0x00: return "standard";
        case 0x04: return "Flyde Micro";
        case 0x06: return "Motorola Connect Plus";
        case 0x08: return "Hytera";
        case 0x10: return "Motorola";
        case 0x58: return "Tait";
        case 0x68: return "Hytera";
        default: return nullptr;
    }
}

}  // namespace

void DmrProto::reset() {
    for (auto& s : slots_) s = Slot();
    voice_slot_ = -1;
    last_tc_ = -1;
    cc_ = -1;
}

std::string DmrProto::slot_name(int slot) const {
    return slot == 0 ? "Direct" : "Slot " + std::to_string(slot);
}

bool DmrProto::slot_wanted(int slot) const {
    return ctx_.opts->dmr_slot == 0 || slot == 0 || slot == ctx_.opts->dmr_slot;
}

void DmrProto::call_update(int slot, const std::string& desc) {
    Slot& s = slots_[slot];
    s.last_ms = now_ms();
    if (!desc.empty() && desc != s.call) {
        s.call = desc;
        if (slot_wanted(slot)) ctx_.report->event(Mode::DMR, slot_name(slot) + " voice: " + desc, 5.0);
    }
    if (!s.call.empty()) ctx_.report->set(Mode::DMR, slot_name(slot), "Voice: " + s.call);
}

void DmrProto::full_lc(int slot, const uint8_t* lc, const char* what) {
    int flco = lc[0] & 0x3F, fid = lc[1], so = lc[2];
    uint32_t dst = (lc[3] << 16) | (lc[4] << 8) | lc[5];
    uint32_t src = (lc[6] << 16) | (lc[7] << 8) | lc[8];
    std::string d;
    if (flco == 0x00) d = "TG " + std::to_string(dst) + " <- " + std::to_string(src);
    else if (flco == 0x03) d = "Unit " + std::to_string(src) + " -> unit " + std::to_string(dst);
    else if (flco == 0x08 && fid == 0) {
        // GPS info: reserved(4) position error(3) longitude(25) latitude(24)
        uint64_t v = 0;
        for (int i = 2; i < 9; i++) v = (v << 8) | lc[i];
        int32_t lon = static_cast<int32_t>((v >> 24) & 0x1FFFFFF), lat = static_cast<int32_t>(v & 0xFFFFFF);
        if (lon & 0x1000000) lon -= 0x2000000;
        if (lat & 0x800000) lat -= 0x1000000;
        char b[64];
        snprintf(b, sizeof b, "%.5f, %.5f", lat * 180.0 / 16777216.0, lon * 360.0 / 33554432.0);
        ctx_.report->set(Mode::DMR, slot_name(slot) + " GPS", b);
        if (slot_wanted(slot)) ctx_.report->event(Mode::DMR, slot_name(slot) + " GPS position " + b, 10.0);
        gps_point(slot, lat * 180.0 / 16777216.0, lon * 360.0 / 33554432.0, b);
        return;
    } else if (flco >= 0x04 && flco <= 0x07 && fid == 0) {
        talker_alias(slot, flco - 4, lc);
        return;
    } else {
        if (!ctx_.opts->verbose) return;
        if (slot_wanted(slot))
            ctx_.report->event(Mode::DMR, slot_name(slot) + " " + what + ": FLCO 0x" + hex(flco, 2) + " FID 0x" + hex(fid, 2) +
                                              " " + hex((uint64_t(so) << 48) | (uint64_t(dst) << 24) | src, 14),
                               ctx_.opts->verbose ? 2.0 : 20.0);
        return;
    }
    if (so & 0x80) d += " [EMERGENCY]";
    if (so & 0x40) d += " [privacy]";
    if (fid != 0) {
        const char* fn = fid_name(fid);
        d += fn ? std::string(" (") + fn + ")" : " (FID 0x" + hex(fid, 2) + ")";
    }
    if (slots_[slot].src != src) slots_[slot].alias.clear();
    slots_[slot].src = src;
    call_update(slot, d);
    ctx_.report->set(Mode::DMR, "Last talkgroup", flco == 0 ? std::to_string(dst) : "-");
    ctx_.report->set(Mode::DMR, "Last source", std::to_string(src));
}

// GPS info LC -> a map point for the radio that is talking on the slot
void DmrProto::gps_point(int slot, double lat, double lon, const char* text) {
    if (!(lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) || (lat == 0 && lon == 0)) return;
    const Slot& s = slots_[slot];
    kp::MapPoint p;
    p.id = s.src ? std::to_string(s.src) : slot_name(slot);
    p.lat = lat;
    p.lon = lon;
    p.label = !s.alias.empty() ? s.alias : (s.src ? "DMR " + std::to_string(s.src) : "DMR " + slot_name(slot));
    p.kind = "person";
    p.info = std::string(s.src ? "Radio ID: " + std::to_string(s.src) + "\n" : "") +
             (s.alias.empty() ? "" : "Talker alias: " + s.alias + "\n") + "Slot: " + slot_name(slot) + "\n" +
             (s.call.empty() ? "" : "Call: " + s.call + "\n") + "GPS: " + text;
    p.ttl_s = 3600;
    ctx_.report->map(Mode::DMR, p);
}

// Talker alias (FLCO 4 = header, 5..7 = blocks): the caller's name/alias
void DmrProto::talker_alias(int slot, int block, const uint8_t* lc) {
    Slot& sl = slots_[slot];
    if (block == 0) {
        sl.ta_format = lc[2] >> 6;
        sl.ta_len = (lc[2] >> 1) & 0x1F;
        sl.ta_bits.assign(49, 0);
        sl.ta_bits[0] = lc[2] & 1;
        for (int i = 0; i < 48; i++) sl.ta_bits[1 + i] = (lc[3 + i / 8] >> (7 - i % 8)) & 1;
        sl.ta_have = 1;
    } else {
        size_t base = 49 + 56 * (block - 1);
        if (sl.ta_bits.size() < base + 56) sl.ta_bits.resize(base + 56, 0);
        for (int i = 0; i < 56; i++) sl.ta_bits[base + i] = (lc[2 + i / 8] >> (7 - i % 8)) & 1;
        sl.ta_have |= 1 << block;
    }
    if (!(sl.ta_have & 1)) return;
    int csize = sl.ta_format == 0 ? 7 : (sl.ta_format == 3 ? 16 : 8);
    int skip = sl.ta_format == 0 ? 0 : 1;
    size_t need = static_cast<size_t>(skip + csize * sl.ta_len);
    // every block up to the needed length present?
    for (int b = 1; b <= 3 && 49 + 56 * (b - 1) < static_cast<int>(need); b++)
        if (!(sl.ta_have & (1 << b))) return;
    if (sl.ta_bits.size() < need) return;
    std::string a;
    for (int i = 0; i < sl.ta_len; i++) {
        uint32_t ch = 0;
        for (int k = 0; k < csize; k++) ch = (ch << 1) | sl.ta_bits[skip + i * csize + k];
        a += (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?';
    }
    while (!a.empty() && a.back() == ' ') a.pop_back();
    if (a.empty()) return;
    ctx_.report->set(Mode::DMR, slot_name(slot) + " talker alias", a);
    sl.alias = a;
    if (slot_wanted(slot)) ctx_.report->event(Mode::DMR, slot_name(slot) + " talker alias: " + a, 30.0);
    sl.ta_have = 0;
}

// The 3 AMBE+2 frames of a voice burst -> the user's mbelib. One timeslot is
// played at a time: the one set in the options, else the first to talk
// (until it has been quiet for a second).
void DmrProto::voice_burst(int slot, const uint8_t* bits) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    Slot& sl = slots_[slot];
    int64_t t = now_ms();
    int want = ctx_.opts->dmr_slot;
    if (want != 0 && slot != want && slot != 0) return;
    if (want == 0) {
        if (voice_slot_ >= 0 && voice_slot_ != slot && t - slots_[voice_slot_].last_voice_ms < 1000) return;
        voice_slot_ = slot;
    }
    sl.last_voice_ms = t;
    std::string who = slot_name(slot) + (sl.call.empty() ? "" : " " + sl.call);
    if (!MbeLib::instance().available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": AMBE+2 codec not installed (README: Digital voice codecs)");
        return;
    }
    if (sl.privacy) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": encrypted (privacy), muted");
        return;
    }
    if (!sl.ambe) sl.ambe = std::make_unique<AmbeStream>(false);
    // burst dibit j -> frame dibit (36 per frame); frame 2 straddles the
    // 24-dibit sync / EMB field in the middle of the burst
    float pcm[3 * 160];
    int clean = 0;
    for (int f = 0; f < 3; f++) {
        char fr[4][24] = {{0}};
        for (int i = 0; i < 36; i++) {
            int j = f == 1 ? (i < 18 ? 36 + i : 78 + (i - 18)) : (f == 0 ? i : 96 + i);
            fr[mbe_tables::rW[i]][mbe_tables::rX[i]] = static_cast<char>(bits[2 * j]);
            fr[mbe_tables::rY[i]][mbe_tables::rZ[i]] = static_cast<char>(bits[2 * j + 1]);
        }
        if (sl.ambe->decode(fr, pcm + 160 * f) <= 2) clean++;
    }
    // a burst none of whose frames decodes cleanly is not voice (a false
    // sync, or the call's tail in noise): don't play it
    if (clean == 0) return;
    if (ctx_.voice) ctx_.voice(Mode::DMR, pcm, 3 * 160);
    if (ctx_.voice_state)
        ctx_.voice_state(who + ": playing (AMBE+2, " + std::to_string(clean) + "/3 frames clean)");
}

void DmrProto::csbk(int slot, const uint8_t* c) {
    int op = c[0] & 0x3F, fid = c[1];
    uint32_t dst = (c[4] << 16) | (c[5] << 8) | c[6];
    uint32_t src = (c[7] << 16) | (c[8] << 8) | c[9];
    uint64_t raw = 0;
    for (int i = 2; i < 10; i++) raw = (raw << 8) | c[i];
    std::string e;
    const char* name = fid == 0 ? csbk_name(op) : nullptr;
    const double chatter = ctx_.opts->verbose ? 2.0 : 60.0;
    if (fid == 0 && op == 0x3D) {
        bool data = c[2] & 0x80, grp = c[2] & 0x40;
        e = std::string("Preamble for ") + (data ? "data" : "CSBK") + " to " + (grp ? "TG " : "unit ") +
            std::to_string(dst) + " from " + std::to_string(src);
        ctx_.report->event(Mode::DMR, slot_name(slot) + " " + e, 3.0);
    } else if (fid == 0 && op >= 0x30 && op <= 0x36) {
        uint32_t lpcn = (c[2] << 4) | (c[3] >> 4);
        int ts = (c[3] >> 3) & 1;
        bool group = op == 0x31 || op == 0x32 || op == 0x34;
        e = std::string(name) + ": " + std::to_string(src) + " -> " + (group ? "TG " : "unit ") + std::to_string(dst) +
            " on channel " + std::to_string(lpcn) + " slot " + std::to_string(ts + 1);
        ctx_.report->event(Mode::DMR, slot_name(slot) + " " + e, 3.0);
        ctx_.report->set(Mode::DMR, "Last grant", (group ? "TG " : "unit ") + std::to_string(dst) + " on channel " +
                                                       std::to_string(lpcn) + "/" + std::to_string(ts + 1));
    } else if (fid == 0 && op == 0x04) {
        e = "Unit-to-unit voice request " + std::to_string(src) + " -> " + std::to_string(dst);
        ctx_.report->event(Mode::DMR, slot_name(slot) + " " + e, 3.0);
    } else {
        // control-channel chatter (Aloha, broadcasts, acks...): verbose only
        if (fid == 0 && (op == 0x19 || op == 0x28)) ctx_.report->set(Mode::DMR, "System", "Tier III trunking (control channel)");
        else if (fid == 0x10 && op == 0x3E) ctx_.report->set(Mode::DMR, "System", "Motorola Capacity Plus");
        else if (fid == 0x06) ctx_.report->set(Mode::DMR, "System", "Motorola Connect Plus");
        if (ctx_.opts->verbose) {
            std::string n = name ? std::string(name) : "opcode 0x" + hex(op, 2);
            const char* fn = fid_name(fid);
            e = "CSBK " + n + " (" + (fn ? std::string(fn) : "FID 0x" + hex(fid, 2)) + ") " + hex(raw, 16);
            ctx_.report->event(Mode::DMR, slot_name(slot) + " " + e, chatter);
        }
    }
}

bool DmrProto::decode(const SymSrc& s, SyncType sync, int* period_syms) {
    // burst symbol j (0..131), sync end = symbol 77; CACH = symbols -12..-1
    uint8_t bits[264];
    for (int j = 0; j < 132; j++) {
        uint8_t d = SymSrc::dibit(s.sym(j - 77));
        bits[2 * j] = d >> 1;
        bits[2 * j + 1] = d & 1;
    }
    const bool bs = sync == BS_VOICE || sync == BS_DATA || sync == NONE;
    const bool voice_sync = sync == BS_VOICE || sync == MS_VOICE || sync == TS1_VOICE || sync == TS2_VOICE;
    *period_syms = (sync == MS_VOICE || sync == MS_DATA || sync == TS1_VOICE || sync == TS1_DATA ||
                    sync == TS2_VOICE || sync == TS2_DATA) ? 288 : 144;
    Report& r = *ctx_.report;
    int64_t t = now_ms();

    // timeslot
    int slot = 0;
    if (sync == TS1_VOICE || sync == TS1_DATA) slot = 1;
    else if (sync == TS2_VOICE || sync == TS2_DATA) slot = 2;
    else if (bs) {
        uint8_t cach[24];
        for (int c = 0; c < 12; c++) {
            uint8_t d = SymSrc::dibit(s.sym(c - 89));
            cach[2 * c] = d >> 1;
            cach[2 * c + 1] = d & 1;
        }
        static const int tact_pos[7] = {0, 4, 8, 12, 14, 18, 22};
        uint32_t tact = 0;
        for (int p : tact_pos) tact = (tact << 1) | cach[p];
        uint32_t m;
        if (hamming_7_4().decode(tact, &m, 1) >= 0) {
            slot = ((m >> 2) & 1) + 1;
            last_tc_ = slot;
            last_tc_ms_ = t;
        } else if (last_tc_ > 0 && t - last_tc_ms_ < 200) {
            slot = last_tc_ == 1 ? 2 : 1;   // bursts alternate
            last_tc_ = slot;
            last_tc_ms_ = t;
        } else {
            slot = 0;
        }
        if (slot == 0 && sync == NONE) return false;
    }
    Slot& sl = slots_[slot];

    if (sync == NONE || voice_sync) {
        // voice burst: A carries the sync, B..F the EMB + embedded signalling
        if (voice_sync) {
            sl.voice_idx = 0;
            sl.emb_state = 0;
            voice_burst(slot, bits);
            return true;   // validated by the EMBs of bursts B..F
        }
        if (sl.voice_idx < 0 || sl.voice_idx >= 5) return false;
        uint32_t emb = bits_to_u32(bits + 108, 8) << 8 | bits_to_u32(bits + 148, 8);
        uint32_t m;
        int ee = qr16_7().decode(emb, &m, 2);
        int cc = m >> 3, pi = (m >> 2) & 1, lcss = m & 3;
        // a random 16-bit word is within 2 bits of a QR codeword 1 time in
        // 4: accept only near-clean EMBs carrying the channel's colour code
        if (ee < 0 || ee > 1 || (cc_ >= 0 && cc != cc_)) { sl.voice_idx = -1; return false; }
        sl.voice_idx++;
        sl.privacy = pi;
        voice_burst(slot, bits);
        // embedded LC: LCSS 1 = first, 3 = continuation, 2 = last fragment
        const uint8_t* frag = bits + 116;
        if (lcss == 1) {
            std::copy(frag, frag + 32, sl.emb_raw);
            sl.emb_state = 1;
        } else if (lcss == 3 && sl.emb_state >= 1 && sl.emb_state <= 2) {
            std::copy(frag, frag + 32, sl.emb_raw + 32 * sl.emb_state);
            sl.emb_state++;
        } else if (lcss == 2 && sl.emb_state == 3) {
            std::copy(frag, frag + 32, sl.emb_raw + 96);
            sl.emb_state = 0;
            uint8_t lcb[72], lc[9];
            if (dmr_embedded_lc(sl.emb_raw, lcb)) {
                pack_bits(lcb, 72, lc);
                full_lc(slot, lc, "embedded LC");
            }
        } else {
            sl.emb_state = 0;
        }
        sl.last_ms = t;
        call_update(slot, "");
        if (cc_ >= 0 && ctx_.valid) ctx_.valid(Mode::DMR);
        if (ctx_.freq_error) ctx_.freq_error(Mode::DMR, s.center);
        return true;
    }

    // data burst: slot type (Golay 20,8) around the sync
    uint32_t st = (bits_to_u32(bits + 98, 10) << 10) | bits_to_u32(bits + 156, 10);
    uint32_t stm;
    // Golay(20,8) corrects 3, but 1/3 of random words are within 3 bits of
    // a codeword: more than 2 corrections is treated as no burst
    int ge = golay20().decode(st, &stm, 3);
    if (ge < 0 || ge > 2) return false;
    int cc = stm >> 4, dt = stm & 15;
    sl.voice_idx = -1;

    uint8_t info[196];
    std::copy(bits, bits + 98, info);
    std::copy(bits + 166, bits + 264, info + 98);
    // ok = the burst's own CRC / FEC confirmed it (what auto-detect and the
    // colour code trust); a slot type alone is too weak
    bool ok = true;
    uint8_t d96[96], by[12];
    switch (dt) {
        case 1:   // voice LC header
        case 2: { // terminator with LC
            if (bptc_196_96(info, d96) < 0) { ok = false; break; }
            pack_bits(d96, 96, by);
            uint8_t mask = dt == 1 ? 0x96 : 0x99;
            for (int i = 9; i < 12; i++) by[i] ^= mask;
            if (!rs129_check(by)) { ok = false; break; }
            if (dt == 1) {
                sl.privacy = (by[2] & 0x40) != 0;
                if (sl.ambe) sl.ambe->reset();
                full_lc(slot, by, "LC header");
            } else {
                full_lc(slot, by, "terminator");
                if (!sl.call.empty() && slot_wanted(slot))
                    r.event(Mode::DMR, slot_name(slot) + " call ended: " + sl.call, 2.0);
                sl.call.clear();
                sl.privacy = false;
                if (voice_slot_ == slot && ctx_.voice_state) ctx_.voice_state("");
                r.set(Mode::DMR, slot_name(slot), "Idle");
            }
            break;
        }
        case 3: { // CSBK
            if (bptc_196_96(info, d96) < 0) { ok = false; break; }
            pack_bits(d96, 96, by);
            by[10] ^= 0xA5;
            by[11] ^= 0xA5;
            if (crc16_ccitt_bytes(by, 10, 0x0000) != ((by[10] << 8) | by[11])) { ok = false; break; }
            csbk(slot, by);
            break;
        }
        case 0: { // privacy indicator header
            if (bptc_196_96(info, d96) < 0) { ok = false; break; }
            pack_bits(d96, 96, by);
            by[10] ^= 0x69;
            by[11] ^= 0x69;
            if (crc16_ccitt_bytes(by, 10, 0x0000) != ((by[10] << 8) | by[11])) { ok = false; break; }
            int alg = by[0] & 7, key = by[2];
            uint32_t dst = (by[7] << 16) | (by[8] << 8) | by[9];
            r.event(Mode::DMR, slot_name(slot) + " privacy header: algorithm " + std::to_string(alg) + " key " +
                                   std::to_string(key) + " to " + std::to_string(dst), 3.0);
            sl.privacy = true;
            break;
        }
        case 6: { // data header
            if (bptc_196_96(info, d96) < 0) { ok = false; break; }
            pack_bits(d96, 96, by);
            by[10] ^= 0xCC;
            by[11] ^= 0xCC;
            if (crc16_ccitt_bytes(by, 10, 0x0000) != ((by[10] << 8) | by[11])) { ok = false; break; }
            bool grp = by[0] & 0x80;
            int dpf = by[0] & 15, sap = by[1] >> 4;
            uint32_t dst = (by[2] << 16) | (by[3] << 8) | by[4];
            uint32_t src = (by[5] << 16) | (by[6] << 8) | by[7];
            static const char* dpfn[16] = {"UDT", "response", "unconfirmed data", "confirmed data", "", "", "", "",
                                           "", "", "", "", "", "short data (defined)", "short data (raw)", "proprietary"};
            static const char* sapn[16] = {"UDT", "", "TCP/IP compression", "UDP/IP compression", "IP packet", "ARP",
                                           "", "", "", "proprietary", "short data", "", "", "", "", ""};
            std::string e = slot_name(slot) + " data: " + dpfn[dpf] + (sapn[sap][0] ? std::string(" / ") + sapn[sap] : "") +
                            " " + std::to_string(src) + " -> " + (grp ? "TG " : "unit ") + std::to_string(dst);
            if (slot_wanted(slot)) r.event(Mode::DMR, e, 3.0);
            r.set(Mode::DMR, slot_name(slot), "Data " + std::to_string(src) + " -> " + std::to_string(dst));
            break;
        }
        case 7:   // rate 1/2 data
        case 10:  // rate 1 data (no FEC to check)
            ok = dt == 7 && bptc_196_96(info, d96) >= 0;
            break;
        case 9:   // idle
            ok = bptc_196_96(info, d96) >= 0;
            if (ok && (sl.call.empty() || t - sl.last_ms > 1500)) {
                if (!sl.call.empty() && slot_wanted(slot))
                    r.event(Mode::DMR, slot_name(slot) + " call ended: " + sl.call, 2.0);
                sl.call.clear();
                r.set(Mode::DMR, slot_name(slot), "Idle");
            }
            break;
        default:
            ok = false;
            if (ctx_.opts->verbose && slot_wanted(slot)) r.event(Mode::DMR, slot_name(slot) + " " + data_type_name(dt), 2.0);
            break;
    }
    if (ok) {
        if (bs) r.set(Mode::DMR, "Source", "Repeater / base station");
        else r.set(Mode::DMR, "Source", slot ? "Direct mode (TDMA)" : "Mobile / direct mode");
        if (cc != cc_) { cc_ = cc; r.event(Mode::DMR, "Colour code " + std::to_string(cc), 30.0); }
        r.set(Mode::DMR, "Colour code", std::to_string(cc));
        if (ctx_.valid) ctx_.valid(Mode::DMR);
        if (ctx_.freq_error) ctx_.freq_error(Mode::DMR, s.center);
    }
    return ok;
}

}  // namespace dig
