// NXDN (Kenwood NEXEDGE / Icom IDAS) conventional, trunked traffic and
// trunked (Type-C) control channels: 4FSK, 192-symbol frames at 4800 Bd
// (NXDN96, 40 ms) or 2400 Bd (NXDN48, 80 ms). Both rates are searched at
// once on the discriminator, each with its own root-raised-cosine (0.2)
// filter. Per frame: frame sync word, LICH (channel type / payload layout),
// then on a traffic / conventional channel SACCH (RAN + slow signalling,
// CRC-6) and FACCH1 (fast signalling, CRC-12) or UDCH / FACCH2 (CRC-15), and
// four AMBE+2 voice frames (played through a user-installed mbelib, like
// DMR); on an outbound control channel the CAC (CRC-16) with one or two
// layer 3 messages. Every coded channel is rate-1/2 K=5 convolutional,
// punctured and interleaved. Written from the NXDN Forum's TS 1-A (Common
// Air Interface) and TS 1-C (Trunking Procedures, Type-C), version 1.3
// (2011) - section numbers below are theirs.
//
// Trunking (kp::Host, Info::trunking): the control channel's system and
// site (SITE_INFO / SRV_INFO -> trunk_system), its channels (SITE_INFO,
// CCH_INFO, ADJ_SITE_INFO -> trunk_channel; a CCH_INFO "New" for this site
// -> control_channel) and every channel assignment (VCALL_ASSGN /
// VCALL_ASSGN_DUP, DCALL_ASSGN / DCALL_ASSGN_DUP -> grant); on a traffic
// channel what it carries (VCALL -> call ACTIVE, TX_REL / TX_REL_EX / frames
// stopping -> ENDED, DISC -> RELEASED). Type-C channels are numbers (1-1023)
// whose frequencies are programmed into the radios (TS 1-C 6.1.5), so a
// grant's channel is "CH n" with no frequency: the user's channel table
// gives it.

#include "nxdn.hpp"
#include "dig_fec.hpp"
#include "mbe_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace dig {

namespace {

constexpr int FRAME_SYMS = 192;
constexpr int FSW_SYMS = 10;
constexpr float THRESHOLD = 0.88f;   // 10 symbols only: keep it tight
// FSW 0xCDF59 (dibits 11 00 11 01 11 11 01 01 10 01): -3 +1 -3 +3 -3 -3 +3 +3 -1 +3
const float FSW[FSW_SYMS] = {-3, 1, -3, 3, -3, -3, 3, 3, -1, 3};
// Whitening of the whole frame (flips the first bit of a dibit = negates
// the symbol), from the NXDN common air interface: the PN x^9 + x^4 + 1
// (preset 0 1 1 1 0 0 1 0 0, S8..S0; 4.6) from the LICH on, one bit per
// symbol, here as the first bit of each dibit of the 384-bit frame
const uint8_t SCRAMBLER[48] = {
    0x00, 0x00, 0x00, 0x82, 0xA0, 0x88, 0x8A, 0x00, 0xA2, 0xA8, 0x82, 0x8A, 0x82, 0x02, 0x20, 0x08,
    0x8A, 0x20, 0xAA, 0xA2, 0x82, 0x08, 0x22, 0x8A, 0xAA, 0x08, 0x28, 0x88, 0x28, 0x28, 0x00, 0x0A,
    0x02, 0x82, 0x20, 0x28, 0x82, 0x2A, 0xAA, 0x20, 0x22, 0x80, 0xA8, 0x8A, 0x08, 0xA0, 0xAA, 0x02};

constexpr int64_t RATE = 48000;      // the receiver's sample rate (FmFrontEnd)

std::vector<float> rrc(int sps, int span, float alpha) {
    std::vector<float> h;
    for (int i = -span * sps; i <= span * sps; i++) {
        float t = static_cast<float>(i) / sps, v;
        const float pi = static_cast<float>(M_PI);
        if (std::fabs(t) < 1e-6f) v = 1.0f - alpha + 4.0f * alpha / pi;
        else if (std::fabs(std::fabs(4.0f * alpha * t) - 1.0f) < 1e-6f)
            v = alpha / std::sqrt(2.0f) * ((1 + 2 / pi) * std::sin(pi / (4 * alpha)) + (1 - 2 / pi) * std::cos(pi / (4 * alpha)));
        else
            v = (std::sin(pi * t * (1 - alpha)) + 4 * alpha * t * std::cos(pi * t * (1 + alpha))) /
                (pi * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h.push_back(v);
    }
    float s = 0;
    for (float v : h) s += v;
    for (float& v : h) v /= s;
    return h;
}

// Punctured rate-1/2 K=5 code (G1 = 1+D^3+D^4, G2 = 1+D+D^2+D^4): the
// received bits are deinterleaved (written in `rows` rows, read by columns),
// the punctured positions - `offsets` within every `period` coded bits - are
// inserted as erasures, then Viterbi. Returns nout decoded bits (tail incl.)
Bits decode_conv(const float* soft, int n, int rows, int period, std::initializer_list<int> offsets, int nout) {
    std::vector<float> d(n);
    const int cols = n / rows;
    for (int i = 0; i < n; i++) d[i] = soft[(i % cols) * rows + i / cols];
    std::vector<float> m;
    m.reserve(2 * nout);
    int i = 0;
    while (i < n) {
        int pos = static_cast<int>(m.size()) % period;
        bool punct = false;
        for (int o : offsets) punct |= (pos == o);
        m.push_back(punct ? 0.0f : d[i++]);
    }
    while (static_cast<int>(m.size()) < 2 * nout) m.push_back(0.0f);
    return viterbi_decode(m.data(), nout, 5, {0x19, 0x17}, true);
}

// The CRCs of 4.5.5: register preset to all ones, the CRC is the register
// after the last bit (S(w-1) sent first). CAC: x^16+x^12+x^5+1 over SR + 144
// layer 3 bits + 3 null bits (155); UDCH / FACCH2: x^15+x^14+x^11+x^10+x^7+
// x^6+x^2+1 over SR + 176 bits (184)
bool cac_crc_ok(const uint8_t* b) { return crc_bits_ones(b, 155, 16, 0x1021) == bits_to_u32(b + 155, 16); }
bool udch_crc_ok(const uint8_t* b) { return crc_bits_ones(b, 184, 15, 0x4CC5) == bits_to_u32(b + 184, 15); }

// Location ID -> "category system/site"
std::string location(uint32_t id) {
    char b[96];
    int cat = static_cast<int>(id >> 22);
    uint32_t sys = 0, site = 0;
    const char* c = "reserved";
    if (cat == 0) { c = "global"; sys = (id >> 12) & 0x3FF; site = id & 0xFFF; }
    else if (cat == 2) { c = "regional"; sys = (id >> 8) & 0x3FFF; site = id & 0xFF; }
    else if (cat == 1) { c = "local"; sys = (id >> 5) & 0x1FFFF; site = id & 0x1F; }
    snprintf(b, sizeof b, "system %u, site %u (%s, location ID %06X)", sys, site, c, id);
    return b;
}

const char* category_name(int cat) { return cat == 0 ? "global" : cat == 1 ? "local" : cat == 2 ? "regional" : "reserved"; }

std::string id_str(uint32_t v) { return std::to_string(v); }

std::string chan_name(int ch) { return "CH " + std::to_string(ch); }

// Call Type (6.5.12): 000 broadcast, 001 conference (group calls), 010
// unspecified (TX_REL only), 100 individual, 110 interconnect, 111 speed dial
bool call_type_group(int ct) { return ct < 4; }

// Cause (6.5.30) as hex, for the verbose log
std::string hex2(uint32_t v) {
    char b[8];
    snprintf(b, sizeof b, "0x%02X", v & 0xFF);
    return b;
}

}  // namespace

NxdnReceiver::NxdnReceiver(RxContext& c, kp::Host* trunk) : ctx_(c), host_(trunk) {
    br_[0].sps = 10;
    br_[0].name = "NXDN96 (4800 Bd, 12.5 kHz)";
    br_[1].sps = 20;
    br_[1].name = "NXDN48 (2400 Bd, 6.25 kHz)";
    for (auto& b : br_) {
        b.taps = rrc(b.sps, 3, 0.2f);
        b.hist.assign(b.taps.size(), 0.0f);
    }
    float m = 0;
    for (float v : FSW) m += v;
    m /= FSW_SYMS;
    float e = 0;
    for (float v : FSW) { fsw_t_.push_back(v - m); e += (v - m) * (v - m); }
    for (float& v : fsw_t_) v /= std::sqrt(e);
}

void NxdnReceiver::reset() {
    for (auto& b : br_) {
        std::fill(b.hist.begin(), b.hist.end(), 0.0f);
        b.buf = SampleBuf();
        b.pk = Branch::Peak();
    }
    total_ = 0;
    jobs_.clear();
    ran_ = -1;
    sacch_have_ = 0;
    sacch_run_ = false;
    last_frame_[0] = last_frame_[1] = -1;
    last_locked_[0] = last_locked_[1] = false;
    ran_seen_[0] = ran_seen_[1] = -1;
    ran_bad_[0] = ran_bad_[1] = false;
    call_.clear();
    cipher_ = 0;
    efr_ = false;
    if (ambe_) ambe_->reset();
    // the host resets its own talker state together with ours
    call_start_ = -1;
    talker_id_.clear();
    talker_label_.clear();
    // ... and its trunking state: everything is sent again
    site_ = Location();
    sys_sent_ = -1;
    sys_control_ = false;
    chan_sent_.clear();
    crep_ = CallRep();
}

// --- talkers (RxContext::talker) ----------------------------------------------
// A transmission = valid voice / VCALL frames less than GAP apart; its talker
// is the VCALL's source unit ID (SACCH superframe or FACCH1). Reported with
// the frame boundaries, so the host can cut exactly this radio's samples out
// of the VFO's stream for the DoA.
void NxdnReceiver::begin_call_if_new() {
    if (call_start_ >= 0 && frame_a_ - voice_end_ <= GAP) return;
    end_talker(voice_end_);
    call_start_ = frame_a_;
    voice_end_ = frame_a_;
}

void NxdnReceiver::talker_frame() {
    begin_call_if_new();
    voice_end_ = std::max(voice_end_, frame_b_);
    if (crep_.active) crep_.last = std::max(crep_.last, frame_b_);
    if (!talker_id_.empty() && ctx_.talker) ctx_.talker(talker_id_, talker_label_, call_start_, voice_end_, 0);
}

void NxdnReceiver::set_talker(uint32_t src, const std::string& label) {
    if (src == 0) return;
    begin_call_if_new();
    const std::string id = std::to_string(src);
    if (!talker_id_.empty() && talker_id_ != id) {
        // another radio without a TX_REL in between: the frames since the old
        // one was last named can't be told apart - leave them out
        end_talker(lc_end_);
        call_start_ = frame_a_;
        voice_end_ = frame_a_;
    }
    talker_id_ = id;
    talker_label_ = label;
    lc_end_ = frame_b_;
}

void NxdnReceiver::end_talker(int64_t at) {
    if (!talker_id_.empty() && ctx_.talker_end) ctx_.talker_end(at, 0);
    talker_id_.clear();
    call_start_ = -1;
}

void NxdnReceiver::tick(int64_t now) {
    if (call_start_ >= 0 && now - voice_end_ > GAP) end_talker(voice_end_);
    // a traffic channel's call whose frames stopped without a TX_REL
    if (crep_.active && now - crep_.last > CALL_GAP) call_report(kp::Call::ENDED);
}

// --- trunking -------------------------------------------------------------

NxdnReceiver::Location NxdnReceiver::split_location(uint32_t id) {
    // 6.5.2: category 00 global (system 10 bits, site 12), 10 regional
    // (14 / 8), 01 local (17 / 5); 11 reserved
    Location l;
    int cat = static_cast<int>(id >> 22);
    if (cat == 0) { l.cat = 0; l.sys = (id >> 12) & 0x3FF; l.site = id & 0xFFF; }
    else if (cat == 2) { l.cat = 2; l.sys = (id >> 8) & 0x3FFF; l.site = id & 0xFF; }
    else if (cat == 1) { l.cat = 1; l.sys = (id >> 5) & 0x1FFFF; l.site = id & 0x1F; }
    return l;
}

// The network = category + system code (the same on every site); the site
// code goes in `site`. Sent when it changes and every 10 s.
void NxdnReceiver::trunk_system(const Location& loc, bool control) {
    if (!host_ || loc.cat < 0 || loc.sys == 0) return;
    const bool changed = loc.cat != site_.cat || loc.sys != site_.sys || loc.site != site_.site || control != sys_control_;
    if (!changed && sys_sent_ >= 0 && frame_b_ - sys_sent_ < 10 * RATE) return;
    site_ = loc;
    sys_control_ = control;
    sys_sent_ = frame_b_;
    kp::TrunkSystem s;
    s.key = std::string(category_name(loc.cat)) + "." + std::to_string(loc.sys);
    s.name = std::string("NXDN ") + category_name(loc.cat) + " system " + std::to_string(loc.sys);
    s.site = "site " + std::to_string(loc.site);
    s.control = control;
    host_->trunk_system(s);
}

// A channel the system announced (no frequency: the radios' channel table)
void NxdnReceiver::trunk_channel(int ch) {
    if (!host_ || ch <= 0) return;
    auto it = chan_sent_.find(ch);
    if (it != chan_sent_.end() && frame_b_ - it->second < 30 * RATE) return;
    chan_sent_[ch] = frame_b_;
    kp::TrunkChannel c;
    c.name = chan_name(ch);
    c.number = ch;
    host_->trunk_channel(c);
}

// VCALL_ASSGN (0x04) / VCALL_ASSGN_DUP (0x05) / DCALL_ASSGN (0x0E) /
// DCALL_ASSGN_DUP (0x0D), 6.4.1.25 / 26 / 31 / 32: CC Option (Em bit 7),
// Call Type + Voice / Data Call Option (Duplex, Transmission Mode), source,
// destination, Call Timer (6 bits), Channel (10 bits); the Location ID and
// Temporary Unit ID that may follow (inter-system calls) are not needed.
// The message carries no cipher type: whether the call is encrypted is only
// known on the traffic channel (VCALL).
void NxdnReceiver::grant(const uint8_t* m, int type, bool on_traffic) {
    auto f = [m](int pos, int len) { return bits_to_u32(m + pos, len); };
    const int ch = static_cast<int>(f(62, 10));
    const int ct = static_cast<int>(f(16, 3));
    const bool group = call_type_group(ct);
    const uint32_t src = f(24, 16), dst = f(40, 16);
    const bool emergency = m[8] != 0;
    const int mode = static_cast<int>(f(21, 3));   // Transmission Mode (6.5.13 / 14)
    const bool data = type == 0x0E || type == 0x0D;
    const bool dup = type == 0x05 || type == 0x0D;
    Report& r = *ctx_.report;
    std::string d = std::string(data ? "Data" : "Voice") + " grant: " + (group ? "TG " : "unit ") + id_str(dst) + " <- " +
                    id_str(src) + " on channel " + std::to_string(ch);
    if (ct >= 6) d += " (telephone)";
    if (emergency) d += " (emergency)";
    // the TC repeats a grant (Nr_ob) and then sends its _DUP every Tr_dup
    // (0.5 s) for the length of the call: one event per call
    r.event(Mode::NXDN, d, 10.0);
    r.set(Mode::NXDN, "Last grant", d);
    if (!host_ || ch == 0) return;   // channel 000 = Null
    kp::Grant g;
    g.channel.name = chan_name(ch);
    g.channel.number = ch;
    g.target = id_str(dst);
    g.group = group;
    g.source = src ? id_str(src) : "";
    g.emergency = emergency;
    g.data = data;
    // an _DUP is the late-entry repeat of a call in progress (5.3.1.1); an
    // ASSGN on a traffic channel only comes from a composite control channel
    g.update = dup || (on_traffic && frame_rfct_ != 3);
    // what the traffic channel's instance needs before it has heard it: the
    // site's RAN (every RF channel of a site uses it, TS 1-C 4.2) and the bit
    // rate the call uses (000 = 4800 bps, 010 / 011 = 9600 bps)
    if (ran_ >= 0) g.options.push_back({"ran", std::to_string(ran_)});
    if (mode == 0) g.options.push_back({"rate", "4800"});
    else if (mode == 2 || mode == 3) g.options.push_back({"rate", "9600"});
    host_->grant(g);
}

// The call on a traffic channel (kp::Host::call); state ENDED / RELEASED
// ends it
void NxdnReceiver::call_report(int state) {
    if (host_) {
        kp::Call c;
        c.state = state;
        c.target = crep_.target;
        c.group = crep_.group;
        c.source = crep_.source;
        c.encrypted = crep_.encrypted;
        c.emergency = crep_.emergency;
        host_->call(c);
    }
    if (state != kp::Call::ACTIVE) crep_.active = false;
    if (state == kp::Call::RELEASED) crep_.known = false;
}

void NxdnReceiver::call_seen(const std::string& target, bool group, const std::string& source, bool enc, bool emer) {
    if (!traffic_channel()) return;
    const bool changed = !crep_.active || target != crep_.target || group != crep_.group || source != crep_.source ||
                         enc != crep_.encrypted || emer != crep_.emergency;
    crep_.known = true;
    crep_.target = target;
    crep_.group = group;
    crep_.source = source;
    crep_.encrypted = enc;
    crep_.emergency = emer;
    crep_.last = std::max(crep_.last, frame_b_);
    if (changed || frame_b_ - crep_.sent >= RATE) {
        crep_.active = true;
        crep_.sent = frame_b_;
        call_report(kp::Call::ACTIVE);
    }
}

// --- layer 3 on a traffic / conventional channel (SACCH superframe 72 bits,
// FACCH1 80, UDCH / FACCH2 176) ----------------------------------------------
void NxdnReceiver::layer3(const uint8_t* m, int nbits, const char* via) {
    if (nbits < 56) return;
    uint8_t b[10] = {0};
    for (int i = 0; i < std::min(nbits, 80) / 8; i++) b[i] = static_cast<uint8_t>(bits_to_u32(m + 8 * i, 8));
    int type = b[0] & 0x3F;
    Report& r = *ctx_.report;
    switch (type) {
        case 0x01: {   // VCALL
            bool group = call_type_group(b[2] >> 5);
            uint32_t src = (b[3] << 8) | b[4], dst = (b[5] << 8) | b[6];
            int cipher = nbits >= 64 ? b[7] >> 6 : 0, key = nbits >= 64 ? b[7] & 0x3F : 0;
            bool emergency = (b[1] & 0x80) != 0;
            static const char* cn[4] = {"", "scrambler", "DES", "AES"};
            std::string d = (group ? "TG " : "unit ") + id_str(dst) + " <- " + id_str(src);
            if (cipher) d += std::string(" [encrypted, ") + cn[cipher] + " key " + std::to_string(key) + "]";
            cipher_ = cipher;
            // Voice Call Option, Transmission Mode 011 = 9600 bps / EFR
            // (full-rate AMBE+2 in 144-bit VCHs): not played
            efr_ = (b[2] & 0x07) == 3;
            if (d != call_) {
                call_ = d;
                r.event(Mode::NXDN, std::string("Voice: ") + d, 5.0);
            }
            r.set(Mode::NXDN, "Current call", d);
            r.set(Mode::NXDN, "Last talkgroup", group ? id_str(dst) : "-");
            r.set(Mode::NXDN, "Last source", id_str(src));
            r.set(Mode::NXDN, "Encryption", cipher ? cn[cipher] : "clear");
            set_talker(src, (group ? "TG " : "unit call to ") + id_str(dst) + (frame_outbound_ ? " · via repeater" : ""));
            frame_vcall_ = true;
            call_seen(id_str(dst), group, src ? id_str(src) : "", cipher != 0, emergency);
            break;
        }
        case 0x04:     // VCALL_ASSGN (composite control channel)
        case 0x05:     // VCALL_ASSGN_DUP (late entry on a traffic channel)
        case 0x0D:     // DCALL_ASSGN_DUP
        case 0x0E:     // DCALL_ASSGN
            // a grant only from a strongly checked message (not a lone
            // CRC-6 SACCH fragment set)
            if (nbits >= 72 && frame_strong_) grant(m, type, true);
            break;
        case 0x07: {   // TX_REL_EX: the TC, after a transmission (6.4.1.7, TS 1-C 16.4)
            const bool permit = nbits >= 64 && m[56];
            if (ctx_.opts->verbose)
                r.event(Mode::NXDN, std::string("Transmission released, talk-back ") + (permit ? "permitted" : "inhibited"), 5.0);
            if (crep_.active) call_report(kp::Call::ENDED);
            break;
        }
        case 0x08: {   // TX_REL
            // the release frames are still this radio's
            if (call_start_ >= 0) {
                voice_end_ = std::max(voice_end_, frame_b_);
                end_talker(voice_end_);
            }
            frame_rel_ = true;
            if (!call_.empty()) r.event(Mode::NXDN, "Call ended: " + call_, 2.0);
            call_.clear();
            cipher_ = 0;
            efr_ = false;
            r.erase(Mode::NXDN, "Current call");
            if (ctx_.voice_state) ctx_.voice_state("");
            if (crep_.active) call_report(kp::Call::ENDED);
            break;
        }
        case 0x09: {   // DCALL_HDR
            uint32_t src = (b[3] << 8) | b[4], dst = (b[5] << 8) | b[6];
            r.event(Mode::NXDN, "Data call: " + id_str(src) + " -> " + id_str(dst), 3.0);
            break;
        }
        case 0x10:     // IDLE
            break;
        case 0x11: {   // DISC: the TC releases the traffic channel (TS 1-C 5.5.2)
            uint32_t src = (b[3] << 8) | b[4], dst = (b[5] << 8) | b[6];
            const bool group = call_type_group(b[2] >> 5);
            r.event(Mode::NXDN, "Disconnect: " + std::string(group ? "TG " : "unit ") + id_str(dst), 3.0);
            if (call_start_ >= 0) end_talker(voice_end_);
            if (!traffic_channel()) break;
            if (crep_.known || crep_.active) {
                crep_.sent = frame_b_;
                call_report(kp::Call::RELEASED);
            } else if (crep_.sent < 0 || frame_b_ - crep_.sent > 2 * RATE) {
                // a channel released before we heard its call (the TC sends
                // DISC more than once): say so once, with the DISC's IDs
                crep_.target = id_str(dst);
                crep_.group = group;
                crep_.source = src ? id_str(src) : "";
                crep_.encrypted = crep_.emergency = false;
                crep_.sent = frame_b_;
                call_report(kp::Call::RELEASED);
            }
            break;
        }
        case 0x19:     // SRV_INFO
        case 0x1A:     // CCH_INFO
        case 0x1B:     // ADJ_SITE_INFO: the same layouts as on the control channel
            cac_message(m, std::min(nbits, 176));
            break;
        default:
            if (ctx_.opts->verbose) {
                char h[8];
                snprintf(h, sizeof h, "0x%02X", type);
                r.event(Mode::NXDN, std::string(via) + " message type " + h, 5.0);
            }
            break;
    }
}

// Outbound control channel message (CAC, 6.4): nbits 144 (single message
// format) or 72 (one of the dual message format's two); also the broadcast
// messages a traffic channel carries (SRV_INFO, CCH_INFO, ADJ_SITE_INFO)
void NxdnReceiver::cac_message(const uint8_t* m, int nbits) {
    Report& r = *ctx_.report;
    int type = static_cast<int>(bits_to_u32(m + 2, 6));
    auto f = [m](int pos, int len) { return bits_to_u32(m + pos, len); };
    const double bcast = ctx_.opts->verbose ? 5.0 : 120.0;
    const bool verbose = ctx_.opts->verbose;
    const bool control = frame_rfct_ == 0 || frame_rfct_ == 3;
    char h[16];
    switch (type) {
        case 0x18: {   // SITE_INFO (6.4.3.1), 18 octets: single message format only
            if (nbits < 144) break;
            const uint32_t lid = f(8, 24);
            std::string loc = location(lid);
            snprintf(h, sizeof h, "%04X", f(48, 16));
            const int cc1 = static_cast<int>(f(124, 10)), cc2 = static_cast<int>(f(134, 10));
            r.set(Mode::NXDN, "Site", loc);
            r.set(Mode::NXDN, "Control channels", std::to_string(cc1) + (cc2 ? ", " + std::to_string(cc2) : ""));
            // Channel Structure Information (6.5.35): Ns = Bn + (Pn + Mn) x Gn x In frames
            const int bn = static_cast<int>(f(32, 2)), gn = static_cast<int>(f(34, 3)), pn = static_cast<int>(f(37, 4)),
                      mn = static_cast<int>(f(41, 3)), in = static_cast<int>(f(44, 4));
            if (bn && gn && pn && in) r.set(Mode::NXDN, "Superframe", std::to_string(bn + (pn + mn) * gn * in) + " frames");
            r.set(Mode::NXDN, "Adjacent sites", std::to_string(f(120, 4)));
            r.event(Mode::NXDN, "Site information: " + loc + ", control channel " + std::to_string(cc1), bcast);
            const Location l = split_location(lid);
            trunk_system(l, control);
            trunk_channel(cc1);
            trunk_channel(cc2);
            break;
        }
        case 0x19: {   // SRV_INFO (6.4.3.2)
            const uint32_t lid = f(8, 24);
            std::string loc = location(lid);
            snprintf(h, sizeof h, "%04X", f(32, 16));
            r.set(Mode::NXDN, "Site", loc);
            r.set(Mode::NXDN, "Service info", std::string("0x") + h);
            // Service Information flags (6.5.33)
            static const char* sif[12] = {"multi-site", "multi-system", "registration", "group registration",
                                          "authentication", "composite control channel", "voice", "data",
                                          "short data", "status / remote control", "PSTN", "IP"};
            std::string sv;
            for (int i = 0; i < 12; i++)
                if (m[32 + i]) sv += (sv.empty() ? "" : ", ") + std::string(sif[i]);
            r.set(Mode::NXDN, "Services", sv.empty() ? "-" : sv);
            r.event(Mode::NXDN, "Service information: " + loc, bcast);
            trunk_system(split_location(lid), control);
            break;
        }
        case 0x1A: {   // CCH_INFO (6.4.3.3): this or another site's control channels
            const uint32_t lid = f(8, 24);
            const Location l = split_location(lid);
            const bool cur = m[32], nw = m[33], add = m[34], del = m[35];
            const int c1 = static_cast<int>(f(38, 10)), c2 = static_cast<int>(f(54, 10));
            const bool here = site_.cat >= 0 && l.cat == site_.cat && l.sys == site_.sys && l.site == site_.site;
            const bool system = site_.cat < 0 || (l.cat == site_.cat && l.sys == site_.sys);
            if (cur) {
                r.event(Mode::NXDN, "Control channels of " + location(lid) + ": " + std::to_string(c1) +
                                        (c2 ? ", " + std::to_string(c2) : ""), bcast);
                if (system) { trunk_channel(c1); trunk_channel(c2); }
            } else if (nw) {
                // the site's control channel changes (TS 1-C 16.5): the new one
                r.event(Mode::NXDN, "Control channel moves to channel " + std::to_string(c1) + " (" + location(lid) + ")", 30.0);
                if (system) trunk_channel(c1);
                if (here && host_ && c1 > 0) {
                    kp::TrunkChannel c;
                    c.name = chan_name(c1);
                    c.number = c1;
                    host_->control_channel(c);
                }
            } else if (add || del) {
                if (verbose)
                    r.event(Mode::NXDN, std::string("Control channel candidate ") + (add ? "added: " : "deleted: ") +
                                            std::to_string(c1) + " (" + location(lid) + ")", 5.0);
                if (add && system) trunk_channel(c1);
            }
            break;
        }
        case 0x1B: {   // ADJ_SITE_INFO (6.4.3.4): 1 (dual format / FACCH1), 3 (single), 4 (FACCH2) sites
            for (int k = 0; k < 4; k++) {
                const int o = 8 + 40 * k;
                if (o + 40 > nbits) break;
                const uint32_t lid = f(o, 24);
                if (lid == 0) continue;
                const int ch = static_cast<int>(f(o + 30, 10));
                r.event(Mode::NXDN, "Adjacent site: " + location(lid) + ", control channel " + std::to_string(ch), bcast);
                const Location l = split_location(lid);
                if (site_.cat >= 0 && l.cat == site_.cat && l.sys == site_.sys) trunk_channel(ch);
            }
            break;
        }
        case 0x04:     // VCALL_ASSGN
        case 0x05:     // VCALL_ASSGN_DUP
        case 0x0D:     // DCALL_ASSGN_DUP
        case 0x0E:     // DCALL_ASSGN
            if (nbits >= 72) grant(m, type, false);
            break;
        case 0x10:     // IDLE
            break;
        case 0x11:     // DISC (on the control channel: a service cancelled / refused)
            r.event(Mode::NXDN, "Disconnect: unit " + std::to_string(f(40, 16)), 3.0);
            break;
        case 0x01:     // VCALL_RESP
        case 0x09:     // DCALL_RESP
            if (verbose)
                r.event(Mode::NXDN, std::string(type == 0x01 ? "Voice" : "Data") + " call response: " +
                                        (call_type_group(static_cast<int>(f(16, 3))) ? "TG " : "unit ") +
                                        std::to_string(f(40, 16)) + " <- " + std::to_string(f(24, 16)) + ", cause " +
                                        hex2(f(56, 8)), 5.0);
            break;
        case 0x02:     // VCALL_REC_REQ
        case 0x0A:     // DCALL_REC_REQ
            if (verbose)
                r.event(Mode::NXDN, std::string("Paging (") + (type == 0x02 ? "voice" : "data") + "): unit " +
                                        std::to_string(f(40, 16)) + " <- " + std::to_string(f(24, 16)), 5.0);
            break;
        case 0x20:     // REG_RESP (6.4.2.2)
            if (verbose)
                r.event(Mode::NXDN, "Registration: unit " + std::to_string(f(32, 16)) + ", TG " + std::to_string(f(48, 16)) +
                                        ", cause " + hex2(f(64, 8)), 5.0);
            break;
        case 0x22:     // REG_C_RESP
        case 0x23:     // REG_COMM
            if (verbose)
                r.event(Mode::NXDN, std::string(type == 0x22 ? "Registration cleared" : "Registration command") +
                                        ": unit " + std::to_string(f(32, 16)), 5.0);
            break;
        case 0x24:     // GRP_REG_RESP (6.4.2.7)
            if (verbose)
                r.event(Mode::NXDN, "Group registration: unit " + std::to_string(f(16, 16)) + ", TG " +
                                        std::to_string(f(32, 16)) + ", cause " + hex2(f(48, 8)), 5.0);
            break;
        default:
            if (verbose) {
                snprintf(h, sizeof h, "0x%02X", type);
                r.event(Mode::NXDN, std::string("CAC message type ") + h, 5.0);
            }
            break;
    }
}

// The RAN option (a trunk grant's): a frame whose SR names another RAN (a
// co-channel site, 6.3.2: RAN = site code mod 64) is dropped. A frame that
// names none (its SACCH failed, or RAN 0 - the release frames of some
// radios) takes the verdict of the frames right before it on this branch
// (within 4 frames), else it passes.
bool NxdnReceiver::ran_rejected(int bi, int ran, int64_t sync_end, int64_t period) {
    if (ran_filter_ < 0) return false;
    const bool named = ran > 0 || (ran == 0 && ran_filter_ == 0);
    if (named) ran_bad_[bi] = ran != ran_filter_;
    else if (ran_seen_[bi] < 0 || sync_end - ran_seen_[bi] > 4 * period) ran_bad_[bi] = false;
    if (named || ran_bad_[bi]) ran_seen_[bi] = sync_end;
    return ran_bad_[bi];
}

void NxdnReceiver::voice(const uint8_t* bits, int nframes) {
    if (!ctx_.voice_wanted || !ctx_.voice_wanted()) return;
    std::string who = call_.empty() ? std::string("NXDN") : call_;
    if (!MbeLib::instance().available()) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": AMBE+2 codec not installed (README: Digital voice codecs)");
        return;
    }
    if (cipher_) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": encrypted, muted");
        return;
    }
    if (efr_) {
        if (ctx_.voice_state) ctx_.voice_state(who + ": full-rate (EFR) voice, not decoded");
        return;
    }
    if (!ambe_) ambe_ = std::make_unique<AmbeStream>(false);
    std::vector<float> pcm;
    int clean = 0;
    for (int f = 0; f < nframes; f++) {
        const uint8_t* v = bits + 72 * f;
        char fr[4][24] = {{0}};
        for (int i = 0; i < 36; i++) {
            fr[mbe_tables::nW[i]][mbe_tables::nX[i]] = static_cast<char>(v[2 * i]);
            fr[mbe_tables::nY[i]][mbe_tables::nZ[i]] = static_cast<char>(v[2 * i + 1]);
        }
        float out[160];
        if (ambe_->decode(fr, out) <= 2) clean++;
        pcm.insert(pcm.end(), out, out + 160);
    }
    if (clean == 0) return;
    if (ctx_.voice) ctx_.voice(Mode::NXDN, pcm.data(), pcm.size());
    if (ctx_.voice_state)
        ctx_.voice_state(who + ": playing (AMBE+2, " + std::to_string(clean) + "/" + std::to_string(nframes) + " frames clean)");
}

void NxdnReceiver::decode_frame(const Branch& b, const SymSrc& s) {
    // frame symbol j (0 = first FSW symbol) is k = j - 9
    uint8_t bits[384];
    float soft[384];
    for (int j = 0; j < FRAME_SYMS; j++) {
        float v = s.sym(j - (FSW_SYMS - 1));
        uint8_t d = SymSrc::dibit(v);
        bits[2 * j] = d >> 1;
        bits[2 * j + 1] = d & 1;
        // soft bits (> 0 = 0): first bit = sign, second bit = outer level
        soft[2 * j] = v;
        soft[2 * j + 1] = 2.0f - std::fabs(v);
    }
    // descramble: flipping the first bit negates the symbol
    for (int i = 0; i < 384; i++) {
        if ((SCRAMBLER[i / 8] >> (7 - i % 8)) & 1) {
            bits[i] ^= 1;
            soft[i] = -soft[i];
        }
    }
    // LICH: 8 bits on the first bit of symbols 10..17, the second bit is 1
    uint32_t lich = 0;
    int ones = 0;
    for (int i = 0; i < 8; i++) {
        lich = (lich << 1) | bits[20 + 2 * i];
        ones += bits[21 + 2 * i];
    }
    int parity = ((lich >> 7) ^ (lich >> 6) ^ (lich >> 5) ^ (lich >> 4)) & 1;
    if (ones < 7 || parity != static_cast<int>(lich & 1)) return;
    int rfct = (lich >> 6) & 3, usc = (lich >> 4) & 3, option = (lich >> 2) & 3;
    bool outbound = (lich >> 1) & 1;
    Report& r = *ctx_.report;
    const int bi = &b == &br_[0] ? 0 : 1;
    // this frame's samples (frame symbol 0 is centred FSW_SYMS - 1 symbols before the sync end)
    const int64_t fa = s.sync_end - static_cast<int64_t>((FSW_SYMS - 1) * b.sps + b.sps / 2);
    const int64_t fb = fa + static_cast<int64_t>(FRAME_SYMS) * b.sps;
    if (rfct == 0) {
        // RCCH (trunking control channel). Only the outbound CAC (4.4.1:
        // 300 bits -> SR + 144 + 3 null + CRC-16 + tail) - an inbound frame
        // is a radio's Long / Short CAC on the uplink
        if (!outbound) return;
        Bits cac = decode_conv(soft + 36, 300, 25, 14, {3, 11}, 175);
        if (!cac_crc_ok(cac.data())) return;
        int ran = static_cast<int>(bits_to_u32(cac.data() + 2, 6));
        if (ran_filter_ >= 0 && ran != ran_filter_) return;   // another site's
        frame_a_ = fa;
        frame_b_ = fb;
        frame_rfct_ = 0;
        frame_strong_ = true;
        r.set(Mode::NXDN, "Channel type", "Trunking control channel");
        r.set(Mode::NXDN, "Variant", b.name);
        if (ran != ran_) {
            ran_ = ran;
            r.event(Mode::NXDN, "RAN " + std::to_string(ran), 30.0);
        }
        r.set(Mode::NXDN, "RAN", std::to_string(ran));
        // SR structure (6.3.1, Table 6.3-1): bit 0 = dual message format
        if (cac[1]) {
            cac_message(cac.data() + 8, 72);
            cac_message(cac.data() + 8 + 72, 72);
        } else {
            cac_message(cac.data() + 8, 144);
        }
        report_valid(ctx_, Mode::NXDN, fa, fb);
        if (ctx_.freq_error) ctx_.freq_error(Mode::NXDN, s.center);
        return;
    }
    static const char* ct[4] = {"", "Trunked traffic (RTCH)", "Conventional (RDCH)", "Composite control/traffic (RTCH-C)"};
    const int64_t period = static_cast<int64_t>(FRAME_SYMS) * b.sps;
    if (usc == 1) {
        // UDCH (data) / FACCH2 (option 00): 348 bits -> SR + 176 + CRC-15 +
        // tail, 29 x 12 interleave (4.5.2.3)
        Bits u = decode_conv(soft + 36, 348, 29, 14, {3, 11}, 203);
        if (!udch_crc_ok(u.data())) return;
        if (ran_rejected(bi, static_cast<int>(bits_to_u32(u.data() + 2, 6)), s.sync_end, period)) return;
        frame_a_ = fa;
        frame_b_ = fb;
        frame_vcall_ = frame_rel_ = false;
        frame_outbound_ = outbound;
        frame_rfct_ = rfct;
        frame_strong_ = true;
        r.set(Mode::NXDN, "Channel type", ct[rfct]);
        r.set(Mode::NXDN, "Variant", b.name);
        r.set(Mode::NXDN, "Direction", outbound ? "outbound (repeater)" : "inbound / direct");
        layer3(u.data() + 8, 176, option == 0 ? "FACCH2" : "UDCH");
        report_valid(ctx_, Mode::NXDN, fa, fb);
        if (ctx_.freq_error) ctx_.freq_error(Mode::NXDN, s.center);
        return;
    }
    // SACCH: 60 bits -> 36 (26 info + CRC-6 + tail)
    bool sacch_ok = false;
    Bits sa = decode_conv(soft + 36, 60, 5, 6, {5}, 36);
    sacch_ok = crc_bits_ones(sa.data(), 26, 6, 0x27) == bits_to_u32(sa.data() + 26, 6);
    // payload: option = which halves FACCH1 steals (0 both, 1 first, 2 second)
    bool facch_half[2] = {option == 0 || option == 1, option == 0 || option == 2};
    bool facch_ok[2] = {false, false};
    Bits fa1[2];
    for (int h = 0; h < 2; h++) {
        if (!facch_half[h]) continue;
        fa1[h] = decode_conv(soft + 96 + 144 * h, 144, 9, 4, {1}, 96);
        facch_ok[h] = crc_bits_ones(fa1[h].data(), 80, 12, 0x80F) == bits_to_u32(fa1[h].data() + 80, 12);
    }
    // Validity: a 6-bit CRC passes 1 random word in 64, so a SACCH counts
    // only on a frame that follows the previous good one exactly one frame
    // later (the stream is locked); FACCH1's CRC-12 counts on its own.
    bool adjacent = last_frame_[bi] >= 0 && std::llabs(s.sync_end - last_frame_[bi] - period) <= b.sps;
    bool locked = sacch_ok && adjacent;
    if (sacch_ok) last_frame_[bi] = s.sync_end;
    else if (!adjacent || s.sync_end - last_frame_[bi] > 4 * period) last_frame_[bi] = -1;
    last_locked_[bi] = locked;
    if (ran_rejected(bi, sacch_ok ? static_cast<int>(bits_to_u32(sa.data() + 2, 6)) : -1, s.sync_end, period)) return;
    bool valid = locked || facch_ok[0] || facch_ok[1];
    if (!valid) return;

    frame_a_ = fa;
    frame_b_ = fb;
    frame_vcall_ = frame_rel_ = false;
    frame_outbound_ = outbound;
    frame_rfct_ = rfct;

    r.set(Mode::NXDN, "Channel type", ct[rfct]);
    r.set(Mode::NXDN, "Variant", b.name);
    r.set(Mode::NXDN, "Direction", outbound ? "outbound (repeater)" : "inbound / direct");
    if (sacch_ok) {
        int sr = static_cast<int>(bits_to_u32(sa.data(), 2));
        int ran = static_cast<int>(bits_to_u32(sa.data() + 2, 6));
        // the RAN comes from superframe SACCHs; the non-superframe / idle
        // ones of a call's release carry 0
        if (usc != 2 && ran_ >= 0) ran = ran_;
        if (ran != ran_) {
            ran_ = ran;
            r.event(Mode::NXDN, "RAN " + std::to_string(ran), 30.0);
        }
        r.set(Mode::NXDN, "RAN", std::to_string(ran));
        if (usc == 2) {
            // superframe: 4 fragments, SR 3 (first) .. 0 (last)
            int idx = 3 - sr;
            if (idx == 0) {
                sacch_have_ = 0;
                sacch_run_ = true;
            } else if (!(sacch_have_ & (1 << (idx - 1))) ||
                       std::llabs(s.sync_end - sacch_t_[idx - 1] - period) > b.sps) {
                sacch_run_ = false;
            }
            sacch_t_[idx] = s.sync_end;
            std::copy(sa.begin() + 8, sa.begin() + 26, sacch_sf_ + 18 * idx);
            sacch_have_ |= 1 << idx;
            if (sr == 0 && sacch_have_ == 15) {
                // 4 CRC-6 + the fragments of 4 consecutive frames in order:
                // strong enough for a grant
                frame_strong_ = sacch_run_;
                layer3(sacch_sf_, 72, "SACCH");
            }
        }
    }
    for (int h = 0; h < 2; h++) {
        frame_strong_ = true;
        if (facch_ok[h]) layer3(fa1[h].data(), 80, "FACCH1");
        else if (!facch_half[h]) voice(bits + 96 + 144 * h, 2);   // RTCH / RDCH / RTCH-C all carry voice
    }
    // voice in a half (option != 0; not UDCH data) or the call's VCALL: part
    // of the transmission. Idle frames between calls are not.
    if (!frame_rel_ && (option != 0 || frame_vcall_)) talker_frame();
    report_valid(ctx_, Mode::NXDN, frame_a_, frame_b_);
    if (ctx_.freq_error) ctx_.freq_error(Mode::NXDN, s.center);
}

void NxdnReceiver::process(const float* d, size_t n) {
    // a grant's "rate" option: only that branch (0 = NXDN96 / 9600 bps,
    // 1 = NXDN48 / 4800 bps)
    const bool use[2] = {rate_ != 4800, rate_ != 9600};
    for (size_t i = 0; i < n; i++) {
        float x = std::clamp(d[i], -4000.0f, 4000.0f);
        for (int bi = 0; bi < 2; bi++) {
            if (!use[bi]) continue;
            Branch& b = br_[bi];
            const size_t nt = b.taps.size();
            b.hist[b.pos] = x;
            float acc = 0;
            size_t p = b.pos;
            for (size_t k = 0; k < nt; k++) {
                acc += b.taps[k] * b.hist[p];
                p = p == 0 ? nt - 1 : p - 1;
            }
            b.pos = (b.pos + 1) % nt;
            b.buf.push(acc);
            // FSW correlation at symbol spacing
            const int64_t last = b.buf.end() - 1;
            const int64_t first = last - static_cast<int64_t>(FSW_SYMS - 1) * b.sps;
            if (first < b.buf.begin()) continue;
            float w[FSW_SYMS], m = 0;
            for (int k = 0; k < FSW_SYMS; k++) { w[k] = b.buf.at(first + k * b.sps); m += w[k]; }
            m /= FSW_SYMS;
            float e = 0, c = 0;
            for (int k = 0; k < FSW_SYMS; k++) { float v = w[k] - m; e += v * v; c += fsw_t_[k] * v; }
            c = e > 1e-3f ? c / std::sqrt(e) : 0.0f;
            auto& pk = b.pk;
            if (pk.active) {
                pk.age++;
                if (std::fabs(c) > std::fabs(pk.best)) { pk.best = c; pk.idx = last; }
                if (pk.age >= b.sps) {
                    pk.active = false;
                    // level fit on the FSW symbols (they include +-1 levels)
                    float sx = 0, sy = 0, sxx = 0, sxy = 0;
                    const int64_t f0 = pk.idx - static_cast<int64_t>(FSW_SYMS - 1) * b.sps;
                    for (int k = 0; k < FSW_SYMS; k++) {
                        float y = b.buf.at(f0 + k * b.sps);
                        sx += FSW[k]; sy += y; sxx += FSW[k] * FSW[k]; sxy += FSW[k] * y;
                    }
                    float scale = (FSW_SYMS * sxy - sx * sy) / (FSW_SYMS * sxx - sx * sx);
                    float center = (sy - scale * sx) / FSW_SYMS;
                    if (std::fabs(scale) > 40.0f) {
                        SymSrc src;
                        src.buf = &b.buf;
                        src.sync_end = pk.idx;
                        src.sps = b.sps;
                        src.scale = scale;
                        src.center = center;
                        jobs_.push_back({bi, src});
                    }
                }
            } else if (std::fabs(c) >= THRESHOLD) {
                pk.active = true;
                pk.best = c;
                pk.idx = last;
                pk.age = 0;
            }
        }
    }
    total_ += static_cast<int64_t>(n);
    for (int bi = 0; bi < 2; bi++) {
        if (use[bi]) continue;
        // a branch the rate option switched off stays empty, on the common
        // sample index (so it can be switched on again at any time)
        if (br_[bi].buf.end() != total_ || br_[bi].buf.begin() != total_) {
            br_[bi].buf.restart(total_);
            br_[bi].pk = Branch::Peak();
            last_frame_[bi] = -1;
        }
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!use[it->branch]) { it = jobs_.erase(it); continue; }
        if (!it->src.has(FRAME_SYMS - FSW_SYMS)) { ++it; continue; }
        decode_frame(br_[it->branch], it->src);
        it = jobs_.erase(it);
    }
    tick(total_);
    for (int bi = 0; bi < 2; bi++) {
        if (!use[bi]) continue;
        Branch& b = br_[bi];
        int64_t keep = b.buf.end() - 3 * FRAME_SYMS * b.sps;
        for (const auto& j : jobs_) keep = std::min(keep, j.src.sync_end - 20 * b.sps);
        if (keep > b.buf.begin() + 8192) b.buf.trim_before(keep);
    }
}

}  // namespace dig
