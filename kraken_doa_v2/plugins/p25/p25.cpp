// P25 Phase 1 (TIA-102.BAAA) frame decoding: NID, HDU, LDU1/2 link control
// and encryption sync, TDULC, TSDU trunking signalling blocks and the PDU
// header. Voice (IMBE) frames are counted, not synthesized.

#include "p25.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <cstdio>
#include <sstream>

namespace dig {

namespace {

// NID: BCH(63,16) - encoder as in the standard's shift register form
std::vector<uint64_t> build_nid_table() {
    static const int g[48] = {1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 1, 1,
                              1, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1};
    std::vector<uint64_t> t(65536);
    for (uint32_t m = 0; m < 65536; m++) {
        int data[16];
        for (int i = 0; i < 16; i++) data[i] = (m >> (15 - i)) & 1;
        int bb[47] = {0};
        for (int i = 15; i >= 0; i--) {
            int fb = data[i] ^ bb[46];
            for (int j = 46; j > 0; j--) bb[j] = (fb && g[j]) ? (bb[j - 1] ^ fb) : bb[j - 1];
            bb[0] = g[0] && fb;
        }
        uint64_t cw = m;
        for (int i = 0; i < 47; i++) cw = (cw << 1) | static_cast<uint64_t>(bb[i]);
        t[m] = cw;   // 63 bits, first transmitted bit = bit 62
    }
    return t;
}

const std::vector<uint64_t>& nid_table() {
    static const std::vector<uint64_t> t = build_nid_table();
    return t;
}

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
    alg_ = 0x80;
    lc_encrypted_ = false;
    iden_.clear();
    last_nac_ = -1;
    call_desc_.clear();
    last_voice_ms_ = 0;
}

std::string P25Proto::chan_str(uint32_t ch) const {
    int id = static_cast<int>(ch >> 12);
    uint32_t num = ch & 0xFFF;
    auto it = iden_.find(id);
    std::string s = std::to_string(id) + "-" + std::to_string(num);
    if (it != iden_.end() && it->second.spacing_hz) {
        uint32_t n = it->second.tdma ? num / static_cast<uint32_t>(std::max(1, it->second.slots)) : num;
        s += " (" + mhz(it->second.base_hz + static_cast<uint64_t>(n) * it->second.spacing_hz) + ")";
    }
    return s;
}

void P25Proto::voice_activity(const std::string& desc) {
    last_voice_ms_ = now_ms();
    if (!desc.empty() && desc != call_desc_) {
        call_desc_ = desc;
        ctx_.report->event(Mode::P25, "Voice: " + desc, 5.0);
    }
    if (!call_desc_.empty()) ctx_.report->set(Mode::P25, "Current call", call_desc_);
}

void P25Proto::link_control(const uint8_t* lc, const char* where) {
    int lco = lc[0] & 0x3F;
    int mfid = lc[1];
    bool std_mfid = (mfid == 0x00 || mfid == 0x01);
    if (lco == 0x00 && std_mfid) {
        bool emerg = lc[2] & 0x80, enc = lc[2] & 0x40;
        lc_encrypted_ = enc;
        uint32_t tg = (lc[4] << 8) | lc[5];
        uint32_t src = (lc[6] << 16) | (lc[7] << 8) | lc[8];
        std::string d = "TG " + std::to_string(tg) + " <- " + std::to_string(src);
        if (enc) d += " [encrypted]";
        if (emerg) d += " [EMERGENCY]";
        voice_activity(d);
        ctx_.report->set(Mode::P25, "Last talkgroup", std::to_string(tg));
        ctx_.report->set(Mode::P25, "Last source", std::to_string(src));
    } else if (lco == 0x03 && std_mfid) {
        uint32_t dst = (lc[3] << 16) | (lc[4] << 8) | lc[5];
        uint32_t src = (lc[6] << 16) | (lc[7] << 8) | lc[8];
        std::string d = "Unit " + std::to_string(src) + " -> unit " + std::to_string(dst);
        lc_encrypted_ = lc[2] & 0x40;
        if (lc[2] & 0x40) d += " [encrypted]";
        voice_activity(d);
    } else if (lco == 0x0F) {
        ctx_.report->event(Mode::P25, std::string("Call termination (") + where + ")", 2.0);
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

void P25Proto::tsbk(const uint8_t* t) {
    int op = t[0] & 0x3F;
    int mfid = t[1];
    uint64_t a = 0;
    for (int i = 2; i < 10; i++) a = (a << 8) | t[i];
    auto f = [&](int hi, int len) { return static_cast<uint32_t>((a >> (hi - len + 1)) & ((1ULL << len) - 1)); };
    // a: bit 63 = MSB of byte 2
    Report& r = *ctx_.report;
    const double bcast = ctx_.opts->verbose ? 5.0 : 120.0;
    if (mfid != 0x00 && mfid != 0x01) {
        r.event(Mode::P25, "TSBK MFID 0x" + hex(mfid, 2) + " opcode 0x" + hex(op, 2) + " " + hex(a, 16),
                ctx_.opts->verbose ? 5.0 : 60.0);
        return;
    }
    switch (op) {
        case 0x00: {   // GRP_V_CH_GRANT
            uint32_t opts = f(63, 8), ch = f(55, 16), grp = f(39, 16), src = f(23, 24);
            std::string e = "Grant: TG " + std::to_string(grp) + " <- " + std::to_string(src) + " on " + chan_str(ch);
            if (opts & 0x40) e += " [encrypted]";
            if (opts & 0x80) e += " [EMERGENCY]";
            r.event(Mode::P25, e, 3.0);
            r.set(Mode::P25, "Last grant", "TG " + std::to_string(grp) + " on " + chan_str(ch));
            break;
        }
        case 0x02: {   // GRP_V_CH_GRANT_UPDT
            uint32_t c1 = f(63, 16), g1 = f(47, 16), c2 = f(31, 16), g2 = f(15, 16);
            r.event(Mode::P25, "Grant update: TG " + std::to_string(g1) + " on " + chan_str(c1) +
                                   (g2 != g1 || c2 != c1 ? ", TG " + std::to_string(g2) + " on " + chan_str(c2) : ""),
                    10.0);
            break;
        }
        case 0x03: {   // GRP_V_CH_GRANT_UPDT_EXP
            uint32_t ch = f(47, 16), grp = f(15, 16);
            r.event(Mode::P25, "Grant update: TG " + std::to_string(grp) + " on " + chan_str(ch), 10.0);
            break;
        }
        case 0x04: {   // UU_V_CH_GRANT
            uint32_t ch = f(63, 16), dst = f(47, 24), src = f(23, 24);
            r.event(Mode::P25, "Unit grant: " + std::to_string(src) + " -> " + std::to_string(dst) + " on " + chan_str(ch), 3.0);
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
            iden_[id] = in;
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
            r.set(Mode::P25, "Control channel", chan_str(ch));
            r.event(Mode::P25, "RFSS status: sys " + hex(sys, 3) + " RFSS " + std::to_string(rfss) + " site " +
                                   std::to_string(site) + " CC " + chan_str(ch), bcast);
            break;
        }
        case 0x3B: {   // NET_STS_BCST
            uint32_t wacn = f(55, 20), sys = f(35, 12), ch = f(23, 16);
            r.set(Mode::P25, "WACN", hex(wacn, 5));
            r.set(Mode::P25, "System ID", hex(sys, 3));
            r.event(Mode::P25, "Network status: WACN " + hex(wacn, 5) + " sys " + hex(sys, 3) + " CC " + chan_str(ch), bcast);
            break;
        }
        case 0x3C: {   // ADJ_STS_BCST
            uint32_t sys = f(51, 12), rfss = f(39, 8), site = f(31, 8), ch = f(23, 16);
            std::string d = "sys " + hex(sys, 3) + " RFSS " + std::to_string(rfss) + " site " + std::to_string(site) + " on " + chan_str(ch);
            r.set(Mode::P25, "Neighbour " + std::to_string(rfss) + "/" + std::to_string(site), chan_str(ch));
            r.event(Mode::P25, "Adjacent site: " + d, bcast);
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
// deinterleaved into mbelib's [8][23] layout
void P25Proto::voice_frames(const std::vector<uint8_t>& sf) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    bool enc = alg_ != 0x80 || lc_encrypted_;
    if (enc) {
        if (ctx_.voice_state) ctx_.voice_state(call_desc_ + (call_desc_.empty() ? "" : ": ") + "encrypted, muted");
        return;
    }
    static const int start[9] = {56, 128, 220, 312, 404, 496, 588, 680, 768};
    float pcm[9 * 160];
    int good = 0;
    for (int f = 0; f < 9; f++) {
        char fr[8][23] = {{0}};
        for (int j = 0; j < 72; j++) {
            size_t d = static_cast<size_t>(start[f] + j);
            if (2 * d + 1 >= sf.size()) return;
            fr[mbe_tables::iW[j]][mbe_tables::iX[j]] = static_cast<char>(sf[2 * d]);
            fr[mbe_tables::iY[j]][mbe_tables::iZ[j]] = static_cast<char>(sf[2 * d + 1]);
        }
        if (imbe_.decode(fr, pcm + 160 * f) >= 0) good++;
    }
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

    // NID
    uint64_t rx = bits_to_u64(sf.data() + 48, 63);
    const auto& tab = nid_table();
    int best = 64;
    uint32_t msg = 0;
    // fast path: known NAC, valid DUIDs
    static const int duids[7] = {0x0, 0x3, 0x5, 0x7, 0xA, 0xC, 0xF};
    if (last_nac_ >= 0) {
        for (int d : duids) {
            uint32_t m = (static_cast<uint32_t>(last_nac_) << 4) | d;
            int e = popcount64(tab[m] ^ rx);
            if (e < best) { best = e; msg = m; }
        }
    }
    if (best > 4) {
        for (uint32_t m = 0; m < 65536; m++) {
            int e = popcount64(tab[m] ^ rx);
            if (e < best) { best = e; msg = m; }
        }
    }
    if (best > 9) return false;
    int nac = static_cast<int>(msg >> 4), duid = static_cast<int>(msg & 0xF);
    const char* dn = duid_name(duid);
    if (!dn) return false;
    if (ctx_.opts->p25_nac >= 0 && nac != ctx_.opts->p25_nac) return false;

    Report& r = *ctx_.report;
    // The NID alone is weak evidence (random data that happens to correlate
    // with the sync - e.g. D-STAR's binary symbols - can land near a
    // codeword): frames count as P25 when their payload FEC/CRC passes, or
    // for the payload-less TDU, when the NID is nearly clean.
    bool valid = duid == 0x3 && best <= 3;

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
                tsbk(t);
                if (t[0] & 0x80) break;   // last block
            }
            if (ok) {
                valid = true;
                r.set(Mode::P25, "Channel type", "Control channel (trunked)");
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
                voice_activity("");
                r.set(Mode::P25, "Channel type", "Voice");
            }
            if (valid) voice_frames(sf);
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
                alg_ = alg;
                uint32_t kid = (by[11] << 8) | by[12], tg = (by[13] << 8) | by[14];
                std::string d = "TG " + std::to_string(tg);
                if (alg != 0x80) d += " [" + alg_name(alg) + "]";
                r.set(Mode::P25, "Encryption", alg == 0x80 ? "clear" : alg_name(alg) + ", key 0x" + hex(kid, 4));
                r.event(Mode::P25, "Call header: " + d, 3.0);
                voice_activity("");
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
            alg_ = 0x80;
            lc_encrypted_ = false;
            imbe_.reset();
            if (ctx_.voice_state) ctx_.voice_state("");
            if (!call_desc_.empty()) {
                r.event(Mode::P25, "Call ended: " + call_desc_, 2.0);
                call_desc_.clear();
            }
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
    if (valid) {
        if (nac != last_nac_) {
            last_nac_ = nac;
            r.event(Mode::P25, "NAC 0x" + hex(nac, 3), 30.0);
        }
        r.set(Mode::P25, "NAC", "0x" + hex(nac, 3));
        r.set(Mode::P25, "Last frame", dn);
    }
    if (valid && ctx_.valid) ctx_.valid(Mode::P25);
    if (valid && ctx_.freq_error) ctx_.freq_error(Mode::P25, s.center);
    return valid;
}

}  // namespace dig
