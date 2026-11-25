// MPT1327 analogue trunking signalling (UK Radiocommunications Agency
// MPT 1327 / 1343): 1200 bit/s FFSK - a "1" is one cycle of 1200 Hz, a "0"
// one and a half cycles of 1800 Hz - on an NBFM carrier.
//
// Demodulation: two sliding one-bit (40-sample) correlators on the FM
// discriminator output, one per tone; the normalized difference of their
// energies is the soft bit at every sample. Sync: the 4 preamble bits +
// SYNC 0xC4D7 (control channel) / SYNT 0x3B28 (traffic channel) correlated
// at bit spacing. On the control channel the SYNC is the check field of the
// control channel system codeword (CCSC: 0, 15-bit system identity SYS, ...),
// which is followed by an address codeword carrying the message (Aloha,
// channel grants, acknowledgements, broadcasts...). On a traffic channel a
// message is preamble + SYNT + address codeword (the radios' call
// maintenance, the site's clear-down). Every codeword: 48 information bits,
// BCH(63,48) check (last bit inverted) and an even parity bit. On the
// control channel (where the CCSC vouches for the slot) single-bit errors
// are corrected for the facts and events; after a SYNT, and for everything
// that drives trunk following (grants, channels, calls), only error-free
// codewords count.
// Voice is analogue NBFM on the traffic channels the control channel
// assigns (the host's FM demodulator; this plugin follows the signalling).
//
// Field positions: MPT 1327 (1997 issue) section 5, bit n of a codeword =
// info bit n-1 here (bit 1 sent first). Channel numbers -> Hz: MPT 1343
// (1997) 4.1 / 5.1 (Band III sub-bands 1 and 2), only with the plan option.

#include "mpt1327.hpp"
#include "dig_fec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dig {

namespace {
constexpr int FS = 48000;
constexpr int SPB = 40;            // samples per bit
constexpr float THRESHOLD = 0.2f;   // soft floor; the hard 18-of-20 bit match decides
constexpr uint32_t SYNC_CC = 0xC4D7, SYNC_TC = 0x3B28;
constexpr int ALLI = 8191, TSCI = 8190, IPFIXI = 8189, SDMI = 8188, DIVERTI = 8187, INCI = 8186, REGI = 8185;
constexpr int PSTNGI = 8101, PABXI = 8102, DNI = 8103;
// noise squelch: rms of the discriminator's first difference over 10 ms (Hz);
// FM noise without a carrier is far above it, a carrier with speech far below
constexpr int HF_BLOCK = 480;
constexpr float HF_ON = 2400.0f, HF_OFF = 3400.0f;   // noise alone: >= 4100 (1 %: 4600), a carrier at 6 dB SNR: median 1300
constexpr double CARRIER_END_S = 1.0;   // a call is ENDED this long after its carrier went

// MPT1327 codeword check (3.2.3): BCH remainder (g = x^15+x^14+x^13+x^11+
// x^4+x^2+1) with the last check bit inverted, plus even parity over all 64
// bits
bool cw_ok(uint64_t cw) {
    if (__builtin_popcountll(cw) & 1) return false;
    uint64_t info = cw >> 16;
    uint64_t r = info << 15;
    for (int i = 62; i >= 15; i--)
        if ((r >> i) & 1) r ^= static_cast<uint64_t>(0xE815) << (i - 15);
    return ((r ^ 1) & 0x7FFF) == ((cw >> 1) & 0x7FFF);
}

// Table 4-1 (MPT 1327 section 4): idents above 8100 are special
std::string ident(int prefix, int id) {
    switch (id) {
        case 0: return "dummy";
        case PSTNGI: return "PSTN gateway";
        case PABXI: return "PABX gateway";
        case DNI: return "data network gateway";
        case REGI: return "registration";
        case INCI: return "include";
        case DIVERTI: return "divert";
        case SDMI: return "short data";
        case IPFIXI: return "inter-prefix";
        case TSCI: return "TSC";
        case ALLI: return "all radios";
        default: break;
    }
    char b[24];
    snprintf(b, sizeof b, "%03d-%04d", prefix, id);
    return b;
}

// A party of a call as Grant::target / source: units and groups (1..8100)
// "PPP-IIII", the system-wide call "ALL", gateways by name; "" where the
// message does not hold the party (inter-prefix: its address is in another
// message; dummy / TSC idents of multi-site calls)
std::string party(int prefix, int id) {
    if (id >= 1 && id <= 8100) return ident(prefix, id);
    if (id == ALLI) return "ALL";
    if (id == PSTNGI) return "PSTN";
    if (id == PABXI) return "PABX";
    if (id == DNI) return "data network";
    if (id >= 8121 && id <= 8135) return "PSTN short " + std::to_string(id - 8120);
    if (id == INCI) return "include";
    return "";
}

// bits [pos, pos + len) of the 48 information bits (pos 0 = codeword bit 1)
inline int field(uint64_t info, int pos, int len) {
    return static_cast<int>((info >> (48 - pos - len)) & ((1ULL << len) - 1));
}
}  // namespace

Mpt1327Receiver::Mpt1327Receiver(RxContext& c, kp::Host& h, const Mpt1327Options& o) : ctx_(c), host_(h), o_(o) {
    w12_ = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * 1200.0f / FS);
    w18_ = std::polar(1.0f, -2.0f * static_cast<float>(M_PI) * 1800.0f / FS);
    reset();
}

void Mpt1327Receiver::reset() {
    m_ = SampleBuf();
    for (auto& v : ring12_) v = {0, 0};
    for (auto& v : ring18_) v = {0, 0};
    hpos_ = 0;
    c12_ = c18_ = {0, 0};
    r12_ = r18_ = {1, 0};
    lvl12_ = lvl18_ = 1.0f;
    n_ = 0;
    pk_ = Peak();
    jobs_.clear();
    cc_chan4_ = -1;
    carrier_since_ = -1;
    ended_at_ = -1;
    for (auto& c : cc_last_) c = {0, -1};
    pend_ = Slot();
    sys_ = 0xFFFFFFFF;
    last_aloha_.clear();
    prev_d_ = 0;
    dc_ = 0;
    fe_sent_ = -1;
    hf_acc_ = 0;
    hf_n_ = 0;
    carrier_ = false;
    carrier_lost_ = -1;
    cc_seen_ = sys_sent_ = -1;
    units_.clear();
    emerg_.clear();
    gtcs_.clear();
    call_ = NONE;
    call_target_.clear();
    call_source_.clear();
    call_group_ = true;
    talker_.clear();
    talker_start_ = talker_end_ = 0;
    clear_at_ = last_tc_ = -1;
    tc_gtc_ = 0;
    tc_gtc_at_ = -1;
    tc_chan_ = -1;
    released_ = false;
}

void Mpt1327Receiver::refresh() {
    sys_sent_ = -1;   // the system again with the next block
}

int Mpt1327Receiver::codeword(int64_t first_end, uint64_t* info) {
    uint64_t cw = 0;
    for (int j = 0; j < 64; j++) {
        int64_t at = first_end + static_cast<int64_t>(j) * SPB;
        if (!m_.has(at)) return -1;
        cw = (cw << 1) | (m_.at(at) > 0 ? 1u : 0u);
    }
    int fixed = 0;
    if (!cw_ok(cw)) {
        for (int b = 0; b < 64 && !fixed; b++)
            if (cw_ok(cw ^ (1ULL << b))) { cw ^= 1ULL << b; fixed = 1; }
        if (!fixed) return -1;
    }
    *info = cw >> 16;
    return fixed;
}

// --- trunk reports ----------------------------------------------------------

std::string Mpt1327Receiver::network_key(uint32_t sys) const {
    char b[24];
    if (o_.network == 1) {
        // MPT 1343 9.3.4.2.2: SYS bit 1 = 0: OPID (bits 2-8), NDD, LAB;
        // bit 1 = 1: NET (bits 2-3), NDD, LAB (national networks)
        if ((sys >> 14) & 1) snprintf(b, sizeof b, "NET-%u", (sys >> 12) & 3);
        else snprintf(b, sizeof b, "OPID-%u", (sys >> 7) & 0x7F);
    } else {
        snprintf(b, sizeof b, "%04X", sys & 0x7FFF);
    }
    return b;
}

kp::TrunkChannel Mpt1327Receiver::channel(int c) const {
    kp::TrunkChannel t;
    t.name = "CH " + std::to_string(c);
    t.number = c;
    if (o_.plan == 1) {
        // MPT 1343 4.1 / 5.1: 12.5 kHz channels; the 10-bit field's MSB
        // picks the sub-band, channel numbers 58..560 are in use. Sub-band 1
        // (field 513..1015 = channel 58..560): radios receive 176.5 MHz +
        // (channel - 1) x 12.5 kHz, transmit 8 MHz higher; sub-band 2
        // (field 1..503 = channel 58..560): receive 200.5 MHz + ..., transmit
        // 8 MHz lower
        if (c >= 513 && c <= 1015) {
            const int n = c - 455;
            t.freq_hz = 176.5e6 + (n - 1) * 12500.0;
            t.uplink_hz = 184.5e6 + (n - 1) * 12500.0;
        } else if (c >= 1 && c <= 503) {
            const int n = c + 57;
            t.freq_hz = 200.5e6 + (n - 1) * 12500.0;
            t.uplink_hz = 192.5e6 + (n - 1) * 12500.0;
        }
    }
    return t;
}

bool Mpt1327Receiver::is_control() const { return cc_seen_ >= 0 && n_ - cc_seen_ < 30LL * FS; }

void Mpt1327Receiver::send_system() {
    const bool control = is_control();
    uint32_t sys = control ? sys_ : (o_.sys >= 0 ? static_cast<uint32_t>(o_.sys) : 0xFFFFFFFF);
    if (sys == 0xFFFFFFFF) return;
    kp::TrunkSystem s;
    s.key = network_key(sys);
    char b[64];
    snprintf(b, sizeof b, "MPT1327 %s", s.key.c_str());
    s.name = o_.network == 1 ? b : "MPT1327 SYS " + s.key;
    if (o_.network == 1) {
        if ((sys >> 14) & 1)
            snprintf(b, sizeof b, "SYS %04X (NDD %u, LAB %u)", sys, (sys >> 3) & 0x1FF, sys & 7);
        else
            snprintf(b, sizeof b, "SYS %04X (NDD %u, LAB %u)", sys, (sys >> 3) & 0xF, sys & 7);
    } else {
        snprintf(b, sizeof b, "SYS %04X", sys);
    }
    s.site = b;
    s.control = control;
    host_.trunk_system(s);
    sys_sent_ = n_;
}

bool Mpt1327Receiver::recent(const std::vector<Recent>& v, int prefix, int id, double s) const {
    for (const auto& r : v)
        if (r.prefix == prefix && r.ident == id && n_ - r.at < static_cast<int64_t>(s * FS)) return true;
    return false;
}

// GTC (5.4): 1 | PFIX 7 | IDENT1 13 | 0 | D | CHAN 10 | IDENT2 13 | (N) 2.
// IDENT1 = the called party, IDENT2 = the calling party (a unit, or a
// gateway). Whether IDENT1 is a group is not in the message (units and
// groups share the 1..8100 idents, section 4): a called party is a UNIT when
// the TSC checked its availability (AHY, POINT 0 - only units answer) or it
// called itself before (IDENT2 of a GTC is always a unit or a gateway), the
// system-wide ALLI a group; any other is reported as a group.
void Mpt1327Receiver::grant(uint64_t info, bool on_traffic) {
    const int prefix = field(info, 1, 7), id1 = field(info, 8, 13), d = field(info, 22, 1);
    const int chan = field(info, 23, 10), id2 = field(info, 33, 13);
    if (sys_sent_ < 0) send_system();   // never a grant before the system (after a reset)
    kp::Grant g;
    g.channel = channel(chan);
    g.target = party(prefix, id1);
    g.source = party(prefix, id2);
    if (id1 == ALLI) g.group = true;
    else if (id1 > 8100) g.group = false;   // a gateway (PSTN, PABX)
    else g.group = !recent(units_, prefix, id1, 1e9) && o_.undecided_group;
    g.data = d != 0;
    g.emergency = recent(emerg_, prefix, id1, 20.0) || (id2 >= 1 && id2 <= 8100 && recent(emerg_, prefix, id2, 20.0));
    // MPT1327 has no grant-update flag: the TSC repeats a GTC (9.1.1.12) and
    // some systems re-send it during the call - the same GTC within 5 s is an
    // update
    const uint64_t key = (static_cast<uint64_t>(prefix) << 40) | (static_cast<uint64_t>(id1) << 27) |
                         (static_cast<uint64_t>(id2) << 14) | (static_cast<uint64_t>(chan) << 1) | static_cast<uint64_t>(d);
    gtcs_.erase(std::remove_if(gtcs_.begin(), gtcs_.end(), [this](const LastGtc& x) { return n_ - x.at > 5LL * FS; }),
                gtcs_.end());
    for (auto& x : gtcs_)
        if (x.key == key) { g.update = true; x.at = n_; }
    if (!g.update) gtcs_.push_back({key, n_});
    g.demod = "nfm";
    g.bw_hz = 12500;
    // the traffic channel's instance hasn't heard the control channel
    g.options.push_back({"chan", std::to_string(chan)});
    const uint32_t sys = is_control() ? sys_ : (o_.sys >= 0 ? static_cast<uint32_t>(o_.sys) : 0xFFFFFFFF);
    if (sys != 0xFFFFFFFF) {
        char b[8];
        snprintf(b, sizeof b, "%04X", sys);
        g.options.push_back({"sys", b});
    }
    if (id1 >= 1 && id1 <= 8100) g.options.push_back({"target", g.target});
    if (id2 >= 1 && id2 <= 8100) g.options.push_back({"source", g.source});
    g.options.push_back({"group", g.group ? "1" : "0"});
    if (g.emergency) g.options.push_back({"emergency", "1"});
    if (o_.plan) g.options.push_back({"plan", "mpt1343"});
    if (o_.network) g.options.push_back({"network", "mpt1343"});
    if (o_.uplink) g.options.push_back({"link", "up"});
    host_.grant(g);
    // a GTC on a traffic channel moves the call's radios to a replacement
    // channel (9.2.3.4): this one is left
    if (on_traffic) {
        const int own = o_.chan >= 0 ? o_.chan : tc_chan_;
        if (own >= 0 && own != chan) released("moved to a replacement channel");
    }
    // the calling party is a unit: a later call TO it is an individual call
    if (id2 >= 1 && id2 <= 8100) learn_unit(prefix, id2);
}

// a unit's ident (the oldest heard gives way when the table is full)
void Mpt1327Receiver::learn_unit(int prefix, int id) {
    size_t oldest = 0;
    for (size_t i = 0; i < units_.size(); i++) {
        if (units_[i].prefix == prefix && units_[i].ident == id) {
            units_[i].at = n_;
            return;
        }
        if (units_[i].at < units_[oldest].at) oldest = i;
    }
    if (units_.size() < MAX_UNITS) units_.push_back({prefix, id, n_, false});
    else units_[oldest] = {prefix, id, n_, false};
}

void Mpt1327Receiver::set_call(CallState s, const std::string& target, const std::string& source, bool group) {
    call_ = s;
    if (s == ENDED) ended_at_ = n_;
    call_target_ = target;
    call_source_ = source;
    call_group_ = group;
    kp::Call c;
    c.state = s == ACTIVE ? kp::Call::ACTIVE : kp::Call::ENDED;
    c.target = target;
    c.source = source;
    c.group = group;
    c.emergency = o_.emergency && !target.empty() && target == o_.target;   // the grant said so
    host_.call(c);
}

// the talker ends with its last confirmed message (the speech after it may
// be anyone's: a lost Pressel Off, a radio that sends no Pressel On)
void Mpt1327Receiver::close_talker() {
    if (talker_.empty()) return;
    if (ctx_.talker_end) ctx_.talker_end(talker_end_, 0);
    talker_.clear();
}

void Mpt1327Receiver::released(const char* why) {
    close_talker();
    kp::Call c;
    c.state = kp::Call::RELEASED;
    c.target = call_target_;
    c.source = call_source_;
    c.group = call_group_;
    c.emergency = o_.emergency && !call_target_.empty() && call_target_ == o_.target;
    host_.call(c);
    call_ = NONE;
    call_target_.clear();
    call_source_.clear();
    call_group_ = true;
    released_ = true;   // what follows on this channel is not the granted call
    if (ctx_.opts->verbose) ctx_.report->event(Mode::MPT1327, std::string("Traffic channel released: ") + why, 2.0);
}

// The traffic channel's own signalling (error-free codewords after a SYNT):
// the radios' call maintenance (5.5.4.2, 9.2.3.1) and the site's clear-down
// (5.5.4.3, 9.2.3.8). start / end = the message's samples.
void Mpt1327Receiver::traffic(uint64_t info, int64_t start, int64_t end) {
    const int type = field(info, 21, 9);
    Report& r = *ctx_.report;
    if (type == 281) {   // MAINT: 1 | PFIX | IDENT1 | 1 000 11 001 | CHAN 10 | OPER 3 | RSVD 5
        const int prefix = field(info, 1, 7), id = field(info, 8, 13);
        const int chan = field(info, 30, 10), oper = field(info, 40, 3);
        if (o_.chan >= 0 && chan != o_.chan) {
            if (ctx_.opts->verbose)
                r.event(Mode::MPT1327, "Call maintenance for channel " + std::to_string(chan) + " (another channel's)", 5.0);
            return;
        }
        tc_chan_ = chan;
        last_tc_ = end;
        if (oper >= 4 || id < 1 || id > 8100) return;   // the TSC's (110, 111), spare / reserved; no unit
        // PFIX/IDENT1 = the unit's own address, or in a group call possibly
        // the group's (the system's choice, BCAST SYSDEF 2): the grant's
        // parties tell which. Without them the ident is taken as the
        // speaker and the target is not known
        const std::string who = ident(prefix, id);
        std::string target, source = who;
        bool group = call_group_;
        if (!o_.target.empty() && who == o_.target) {
            target = o_.target;
            group = o_.group != 0;
            // a group call: the group's address - the speaker isn't named; a
            // unit call: the called unit answers - the call stays the grant's
            // pair (the host keys a call on its target; who talks is the talker)
            source = group ? "" : o_.source;
        } else if (!o_.source.empty() && who == o_.source) {
            target = o_.target;
            group = o_.group != 0;
        } else if (!o_.target.empty() && !released_) {
            // another party of the granted call (a group member that sends
            // its own ident, an included unit): the TSC gives the channel to
            // one call from its GTC to its clear-down (9.1.1.12, 9.1.2.6)
            target = o_.target;
            group = o_.group != 0;
        }
        // Disconnects (end of channel use, 9.2.3.5) right after the same
        // unit's Pressel Off: that item is already over
        if (oper == 3 && call_ == ENDED && call_target_ == target && call_source_ == source) return;
        // the talker: the radio named by the message - on the downlink the
        // site repeats it (its bearing is the site's), on the uplink it is
        // the radio itself
        if (ctx_.talker) {
            if (talker_ != who) {
                close_talker();
                talker_ = who;
                talker_start_ = start;
            }
            talker_end_ = end;
            std::string label;
            if (!target.empty()) {
                if (group) label = "TG " + target;
                else label = "to " + (who == target && !o_.source.empty() ? o_.source : target);   // the called unit answers
                label += " · ";
            }
            label += "CH " + std::to_string(chan) + (o_.uplink ? "" : " · via site");
            ctx_.talker(who, label, talker_start_, end, 0);
        }
        if (oper == 0 || oper == 2) {   // pressel on, periodic: a speech item is on
            set_call(ACTIVE, target, source, group);
        } else {                        // pressel off, disconnect: this item is over
            if (call_ != ACTIVE || call_target_ != target || call_source_ != source) set_call(ACTIVE, target, source, group);
            close_talker();
            set_call(ENDED, target, source, group);
        }
        return;
    }
    if (type == 282) {   // CLEAR: 1 | CHAN 10 | CONT 10 | 1 000 11 010 | RSVD 4 | SPARE 2 | REVS 12
        const int chan = field(info, 1, 10);
        if (field(info, 36, 12) != 0xAAA) return;   // REVS must be 101010101010 (9.2.3.8)
        if (o_.chan >= 0 && chan != o_.chan) return;   // another channel's (breakthrough)
        last_tc_ = end;
        // the TSC sends at least two (9.1.2.6): one is enough, the others
        // within 2 s change nothing
        if (call_ == NONE && clear_at_ >= 0 && start - clear_at_ < 2LL * FS) return;
        clear_at_ = start;
        released("clear-down");
        // a control channel used for traffic (it sends SYNT then, 3.2.1.2)
        // is free again: its own VFO says so
        if (is_control()) host_.grant_end(channel(chan));
    }
}

void Mpt1327Receiver::address(uint64_t info, bool control, bool strict, int64_t start, int64_t end) {
    auto f = [info](int pos, int len) { return field(info, pos, len); };
    if (!f(0, 1)) return;   // not an address codeword
    Report& r = *ctx_.report;
    const int prefix = f(1, 7), id1 = f(8, 13), type = f(21, 9);
    const bool verbose = ctx_.opts->verbose;
    const double chatter = verbose ? 5.0 : 1e9;   // effectively verbose-only
    std::string to = ident(prefix, id1);
    char h[160];
    if (type < 256) {   // GTC: go to channel (bit 22 = 0)
        int chan = f(23, 10), id2 = f(33, 13);
        std::string d = ident(prefix, id2) + " -> " + to + " on channel " + std::to_string(chan);
        const kp::TrunkChannel tc = channel(chan);
        if (std::isfinite(tc.freq_hz)) {
            snprintf(h, sizeof h, " (%.4f MHz)", tc.freq_hz / 1e6);
            d += h;
        }
        if (f(22, 1)) d += " (data)";
        r.event(Mode::MPT1327, "Grant: " + d, 3.0);
        r.set(Mode::MPT1327, "Last grant", d);
        if (!strict) return;
        if (control) {
            grant(info, false);   // in a slot of the confirmed CCSC grid
        } else {
            // after a SYNT (a replacement channel, 9.1.2.4; a control channel
            // used for traffic): nothing else vouches for one codeword - only
            // a GTC heard twice within 2 s counts (the TSC repeats GTCs)
            if (tc_gtc_at_ >= 0 && tc_gtc_ == info && n_ - tc_gtc_at_ < 2LL * FS) grant(info, true);
            tc_gtc_ = info;
            tc_gtc_at_ = n_;
        }
        return;
    }
    if (!control && strict) traffic(info, start, end);
    switch (type) {
        case 256: case 257: case 258: case 259: case 260: case 261: case 262: {
            // Aloha (5.5.1): ... FUNC 3 | CHAN4 4 | WT 3 | RSVD 2 | (M) 5 | (N) 4
            static const char* an[7] = {"all calls", "standard data excluded", "simple calls excluded",
                                        "emergency calls only", "emergency or registration",
                                        "registration excluded", "fallback mode"};
            std::string a = std::string("Aloha (") + an[type - 256] + ")";
            r.set(Mode::MPT1327, "Access", a);
            if (a != last_aloha_) {
                last_aloha_ = a;
                r.event(Mode::MPT1327, "Random access: " + a, 60.0);
            }
            r.set(Mode::MPT1327, "Control channel number (low 4 bits)", std::to_string(f(30, 4)));
            if (verbose) r.event(Mode::MPT1327, "ALOHA to " + to, chatter);
            break;
        }
        case 264: case 265: case 266: case 267: case 268: case 269: case 270: case 271: {
            // ACK (5.5.2): ... FUNC 3 | IDENT2 13 | QUAL 1 | (N) 4
            static const char* kn[8] = {"ACK", "ACKI (more to follow)", "ACKQ (call queued)", "ACKX (rejected)",
                                        "ACKV (called unit unavailable)", "ACKE (emergency)", "ACKT (try given address)",
                                        "ACKB (call back)"};
            const int id2 = f(30, 13);
            std::string e = std::string(kn[type - 264]) + ": " + ident(prefix, id2) + " -> " + to;
            r.event(Mode::MPT1327, e, (type == 266 || type == 267 || type == 269) ? 3.0 : chatter);
            // ACKV QUAL 0 = "called unit not in radio contact" (5.5.2.1): the called party is a unit
            if (type == 268 && f(43, 1) == 0 && strict && control && id1 >= 1 && id1 <= 8100) learn_unit(prefix, id1);
            if (type == 269 && strict && control) {   // ACKE: an emergency call is being set up
                emerg_.erase(std::remove_if(emerg_.begin(), emerg_.end(), [this](const Recent& x) { return n_ - x.at > 30LL * FS; }),
                             emerg_.end());
                emerg_.push_back({prefix, id1, n_, true});
                emerg_.push_back({prefix, id2, n_, true});
            }
            break;
        }
        case 272: {
            // AHY (5.5.3.2.1): ... FUNC 3 | IDENT2 13 | D | POINT | CHECK | E | AD
            const int id2 = f(30, 13), point = f(44, 1), e = f(46, 1);
            r.event(Mode::MPT1327, "AHOY (availability check) to " + to, chatter);
            if (strict && control) {
                if (point == 0 && id1 >= 1 && id1 <= 8100) learn_unit(prefix, id1);   // the called party is a unit
                if (e) {
                    emerg_.erase(std::remove_if(emerg_.begin(), emerg_.end(), [this](const Recent& x) { return n_ - x.at > 30LL * FS; }),
                                 emerg_.end());
                    emerg_.push_back({prefix, id1, n_, true});
                    emerg_.push_back({prefix, id2, n_, true});
                }
            }
            break;
        }
        case 274: r.event(Mode::MPT1327, "AHYX (cancel alert) to " + to, chatter); break;
        case 277: r.event(Mode::MPT1327, "AHYP (presence check) to " + to, chatter); break;
        case 278:
            snprintf(h, sizeof h, " status %d", f(43, 5));
            r.event(Mode::MPT1327, "Status message: " + ident(prefix, f(30, 13)) + " -> " + to + h, 3.0);
            break;
        case 279: r.event(Mode::MPT1327, "Short data message to " + to, 3.0); break;
        case 280:   // MARK (5.5.4.1): 1 | CHAN4 4 | A | SYS 15 | ...
            r.set(Mode::MPT1327, "Control channel number (low 4 bits)", std::to_string(f(1, 4)));
            if (verbose) {
                snprintf(h, sizeof h, "Control channel marker (SYS 0x%04X)", f(6, 15));
                r.event(Mode::MPT1327, h, 5.0);
            }
            break;
        case 281: {   // MAINT (5.5.4.2): ... | CHAN 10 | OPER 3 | RSVD 5
            static const char* on[8] = {"pressel on", "pressel off", "periodic", "disconnect", "spare", "reserved",
                                        "clear down other units", "disable transmission"};
            const int oper = f(40, 3);
            std::string e = std::string("Call maintenance (") + on[oper] + ") on channel " + std::to_string(f(30, 10));
            if (oper < 4) e += ": " + to;
            r.event(Mode::MPT1327, e, oper == 2 ? chatter : 3.0);   // periodic messages: verbose only
            break;
        }
        case 282: {
            std::string e = "Clear down: traffic channel " + std::to_string(f(1, 10)) + ", back to control channel " +
                            std::to_string(f(11, 10));
            r.event(Mode::MPT1327, e, 3.0);
            break;
        }
        case 283: {   // MOVE (5.5.4.4): ... | CONT 10 | (M) 5 | RSVD 2 | SPARE 1
            const int cont = f(30, 10), m = f(40, 5);
            r.event(Mode::MPT1327, "Move to control channel " + std::to_string(cont) + ": " + (m == 0 ? "all radios" : to), 3.0);
            // (M) = 0: no address bits compared - every radio moves
            if (strict && control && m == 0 && cont != 0) host_.control_channel(channel(cont));
            break;
        }
        case 284: {   // BCAST (5.5.4.5): 1 | SYSDEF 5 | SYS 15 | 1 000 11 100 | PARAMETERS 18
            static const char* sd[6] = {"announce control channel", "withdraw control channel",
                                        "call maintenance parameters", "registration parameters",
                                        "adjacent site control channel", "vote now advice"};
            const int sysdef = f(1, 5), sys = f(6, 15), chan = f(30, 10);
            const bool chan_form = sysdef == 0 || sysdef == 1 || sysdef == 4 || sysdef == 5;
            if (chan_form)
                snprintf(h, sizeof h, "System broadcast: %s %d (SYS 0x%04X)", sd[sysdef], chan, sys);
            else
                snprintf(h, sizeof h, "System broadcast: %s (SYS 0x%04X)", sysdef < 6 ? sd[sysdef] : "SYSDEF ?", sys);
            r.event(Mode::MPT1327, h, verbose ? 5.0 : 120.0);
            if ((sysdef == 4 || sysdef == 5) && static_cast<uint32_t>(sys) != sys_) {
                snprintf(h, sizeof h, "0x%04X", sys);
                r.set(Mode::MPT1327, "Neighbour site SYS", h);
                snprintf(h, sizeof h, "0x%04X on channel %d", sys, chan);
                r.set(Mode::MPT1327, "Neighbour site", h);
            }
            if (sysdef == 2 && static_cast<uint32_t>(sys) == sys_) {
                // PER 1 | IVAL 5 | PON 1 | ID 1
                const int per = f(30, 1), ival = f(31, 5), pon = f(36, 1), mid = f(37, 1);
                std::string v = per ? "no periodic messages" : "periodic messages every <= " + std::to_string(ival) + " s";
                v += pon ? ", no pressel on" : ", pressel on";
                v += mid ? ", group members send their own ident" : ", group members send the group's ident";
                r.set(Mode::MPT1327, "Call maintenance", v);
            }
            // a channel of this system's network (the announced control
            // channels, the neighbours' control channels) for the host's table
            if (strict && control && sys_ != 0xFFFFFFFF && (sysdef == 0 || sysdef == 4 || sysdef == 5) && chan != 0 &&
                network_key(static_cast<uint32_t>(sys)) == network_key(sys_))
                host_.trunk_channel(channel(chan));
            break;
        }
        default:
            if (type >= 288 && type <= 303) r.event(Mode::MPT1327, "Single address data message to " + to, 3.0);
            else if (type >= 304 && type <= 319) r.event(Mode::MPT1327, "Data message header to " + to, 3.0);
            else if (type >= 320 && type <= 335) r.event(Mode::MPT1327, "GTT (go to transaction) " + to, chatter);
            else if (verbose) {
                snprintf(h, sizeof h, "Message type %d to ", type);
                r.event(Mode::MPT1327, h + to, 5.0);
            }
            break;
    }
}

void Mpt1327Receiver::decode_slot(int64_t sync_end, int which) {
    Slot sl;
    sl.sync_end = sync_end;
    sl.which = which;
    if (which == 0) {
        // the CCSC ends with the SYNC: it starts 44 bits before the pattern
        uint64_t ccsc = 0;
        if (codeword(sync_end - 63 * SPB, &ccsc) < 0) return;
        if ((ccsc >> 47) & 1) return;
        sl.sys = static_cast<uint32_t>((ccsc >> 32) & 0x7FFF);
    }
    sl.err = codeword(sync_end + SPB, &sl.addr);
    // after a SYNT nothing else vouches for the message (no CCSC): an
    // error-free ADDRESS codeword only - with one correction, a random word
    // passed 1 in 1000 (a carrier's key-up did, in a test)
    if (which == 1 && (sl.err != 0 || !((sl.addr >> 47) & 1))) sl.err = -1;
    if (which == 1) {
        if (sl.err >= 0) slot(sl);
        return;
    }
    // A CCSC counts only on the slot grid of another one with the same SYS
    // (128 bits apart, +-half a bit, up to 20 slots: lost and displaced slots
    // in between): a lone CCSC passed in noise 7 times an hour and in FM
    // speech 8 times (1 h each, 2026-11-20) - each a ghost system. The first
    // slot of a control channel waits for the second (107 ms)
    bool ok = false;
    for (const auto& c : cc_last_) {
        if (c.end < 0 || c.sys != sl.sys) continue;
        const int64_t d = sync_end - c.end, k = (d + 64 * SPB) / (128 * SPB);
        if (k >= 1 && k <= 20 && std::llabs(d - k * 128 * SPB) <= SPB / 2) ok = true;
    }
    // remember it (the site heard longest ago gives way)
    size_t at = 0;
    for (size_t i = 0; i < cc_last_.size(); i++) {
        if (cc_last_[i].sys == sl.sys && cc_last_[i].end >= 0) { at = i; break; }
        if (cc_last_[i].end < cc_last_[at].end) at = i;
    }
    cc_last_[at] = {sl.sys, sync_end};
    if (!ok) {
        pend_ = sl;
        return;
    }
    if (pend_.sync_end >= 0 && pend_.sys == sl.sys && pend_.sync_end < sync_end) slot(pend_);
    pend_ = Slot();
    slot(sl);
}

void Mpt1327Receiver::slot(const Slot& sl) {
    Report& r = *ctx_.report;
    const int64_t sync_end = sl.sync_end;
    const int which = sl.which;
    const uint64_t addr = sl.addr;
    const int err = sl.err;
    bool valid = false, addr_ok = false;
    if (which == 0) {
        // Whose control channel? The Aloha (and MARK) carry the low 4 bits of
        // the number of the channel they are sent on, "to protect against
        // breakthrough" (5.5.1). On a traffic channel's VFO (`chan` set) a
        // control channel whose CHAN4 isn't this channel's is another
        // channel's signal that got in - on air (2026-11-19, NZ): the site's
        // control channel on 5 of its traffic channels (3rd-order
        // intermodulation with a busy traffic channel: 2 x CH 207 - CH 247 =
        // CH 167), 304 slots in 600 s on one of them. It is no control
        // channel here: no system, no grants, no release of the call
        if (err >= 0 && (addr >> 47) & 1) {
            const int ty = field(addr, 21, 9);
            if (ty >= 256 && ty <= 262) cc_chan4_ = field(addr, 30, 4);
            else if (ty == 280) cc_chan4_ = field(addr, 1, 4);
        }
        if (o_.chan >= 0 && cc_chan4_ != (o_.chan & 15)) {
            if (cc_chan4_ >= 0) {
                char b[96];
                snprintf(b, sizeof b, "SYS %04X (channel number ...%d, not this channel's: breakthrough)", sl.sys, cc_chan4_);
                r.set(Mode::MPT1327, "Control channel heard", b);
            }
            return;
        }
        const uint32_t sys = sl.sys;
        if (sys != sys_) {
            sys_ = sys;
            sys_sent_ = -1;
            char b[64];
            snprintf(b, sizeof b, "Control channel, system identity 0x%04X", sys);
            r.event(Mode::MPT1327, b, 60.0);
        }
        char b[32];
        snprintf(b, sizeof b, "0x%04X (%u)", sys, sys);
        r.set(Mode::MPT1327, "System identity (SYS)", b);
        r.set(Mode::MPT1327, "Channel type", "Control channel");
        cc_seen_ = sync_end;
        if (sys_sent_ < 0) send_system();   // at once (a new system, or the first after a reset)
        valid = true;
        // a traffic channel back to control signalling: the call is over
        if (call_ != NONE) released("control channel signalling");
    }
    const int64_t start = sync_end - (which == 0 ? 64 : 20) * static_cast<int64_t>(SPB);
    const int64_t end = sync_end + 65 * static_cast<int64_t>(SPB);
    if (err >= 0) {
        valid = addr_ok = true;
        if (which == 1 && !is_control()) r.set(Mode::MPT1327, "Channel type", "Traffic channel (voice is analogue NBFM)");
        if (host_.raw_wanted()) {
            char b[64];
            if (which == 0)
                snprintf(b, sizeof b, "CC SYS %04X %012llX%s", sys_, static_cast<unsigned long long>(addr), err ? " c1" : "");
            else
                snprintf(b, sizeof b, "TC %012llX%s", static_cast<unsigned long long>(addr), err ? " c1" : "");
            host_.raw(b);
        }
        address(addr, which == 0, err == 0, start, end);
    }
    // the slot's samples: the CCSC (64 bits, ending with the SYNC) or preamble + SYNT, then the address codeword
    if (valid) {
        report_valid(ctx_, Mode::MPT1327, start, sync_end + (addr_ok ? 65 : 1) * static_cast<int64_t>(SPB));
        // the carrier offset, measured while a message is on (the host re-centres the VFO slowly)
        if (ctx_.freq_error && (fe_sent_ < 0 || n_ - fe_sent_ >= FS)) {
            ctx_.freq_error(Mode::MPT1327, dc_);
            fe_sent_ = n_;
        }
    }
    // ID separation: a control channel slot is the SITE's transmission - its
    // CCSC names the site (SYS) - so each slot with a good CCSC is a packet of
    // that site: CCSC + address codeword (107 ms), 2 ms inside both ends (the
    // tone correlators lag a bit). Co-channel sites (frequency reuse) heard in
    // turn keep their own bearings. Traffic channels: talkers from the call
    // maintenance messages (traffic()).
    if (which == 0 && ctx_.packet) {
        RxContext::Packet p;
        char id[24];
        snprintf(id, sizeof id, "SYS %04X", sys_);
        p.id = id;
        p.label = "control channel";
        p.start = sync_end - 64 * static_cast<int64_t>(SPB) + FS / 500;
        p.end = sync_end + (addr_ok ? 65 : 1) * static_cast<int64_t>(SPB) - FS / 500;
        p.freq_hz = 0;
        p.bw_hz = 12500;
        p.avg_s = 10;      // a fixed site, ~9 slots a second
        p.keep_s = 1800;
        ctx_.packet(p);
    }
}

// Carrier present or not, from one 10 ms block of the noise squelch
void Mpt1327Receiver::carrier_block(float hf, int64_t block_end) {
    if (carrier_ && hf > HF_OFF) {
        carrier_ = false;
        carrier_lost_ = block_end - HF_BLOCK;
    } else if (!carrier_ && hf < HF_ON) {
        carrier_ = true;
        carrier_lost_ = -1;
        carrier_since_ = block_end - HF_BLOCK;
    }
    // A granted traffic channel's carrier (0.2 s on) before any clear-down
    // is the granted call: the TSC gives the channel to one call from GTC to
    // CLEAR, and on air (NZ, 2026-11-19) the site kept the downlink carrier
    // up from 0.12-0.58 s before the GTC to the clear-down - while the
    // radios' call maintenance hardly ever reached the downlink (2 Pressel
    // On in 24 calls). So the call is ACTIVE from the carrier, not only
    // from a MAINT
    if (carrier_ && !released_ && !o_.target.empty() && carrier_since_ >= 0 && block_end - carrier_since_ >= FS / 5 &&
        (call_ == NONE || (call_ == ENDED && carrier_since_ > ended_at_))) {   // (again when it comes back)
        const bool group = o_.group != 0;
        set_call(ACTIVE, o_.target, group ? "" : o_.source, group);
    }
    // a speech item whose pressel off was lost: over when its carrier is
    if (call_ == ACTIVE && !carrier_ && carrier_lost_ >= 0 &&
        block_end - carrier_lost_ > static_cast<int64_t>(CARRIER_END_S * FS)) {
        close_talker();
        set_call(ENDED, call_target_, call_source_, call_group_);
        if (ctx_.opts->verbose) ctx_.report->event(Mode::MPT1327, "Call ended: carrier gone", 2.0);
    }
}

void Mpt1327Receiver::process(const float* d, size_t n) {
    // sync templates (+1 = 1200 Hz = bit 1)
    static const auto tmpl = [] {
        std::array<std::array<float, 20>, 2> t{};
        const uint32_t w[2] = {SYNC_CC, SYNC_TC};
        for (int k = 0; k < 2; k++) {
            uint32_t v = (0xAu << 16) | w[k];
            for (int i = 0; i < 20; i++) t[k][i] = ((v >> (19 - i)) & 1) ? 1.0f : -1.0f;
        }
        return t;
    }();
    for (size_t i = 0; i < n; i++) {
        // noise squelch (only a traffic channel's call uses it)
        const float df = d[i] - prev_d_;
        prev_d_ = d[i];
        hf_acc_ += static_cast<double>(df) * df;
        if (++hf_n_ == HF_BLOCK) {
            carrier_block(static_cast<float>(std::sqrt(hf_acc_ / HF_BLOCK)), n_ + 1);
            hf_acc_ = 0;
            hf_n_ = 0;
        }
        // the carrier offset is the discriminator's DC: taken off before the
        // tone correlators (1.5 cycles of 1800 Hz don't reject it - a 2 kHz
        // error lost most slots); FFSK and speech have none to speak of
        dc_ += 2e-4f * (d[i] - dc_);
        const float x = d[i] - dc_;
        // one-bit sliding correlators (absolute phase reference)
        std::complex<float> p12 = x * r12_, p18 = x * r18_;
        r12_ *= w12_;
        r18_ *= w18_;
        if ((n_ & 1023) == 0) { r12_ /= std::abs(r12_); r18_ /= std::abs(r18_); }
        size_t k = hpos_;
        c12_ += p12 - ring12_[k];
        c18_ += p18 - ring18_[k];
        ring12_[k] = p12;
        ring18_[k] = p18;
        hpos_ = (hpos_ + 1) % SPB;
        if ((n_ % 4096) == 0) {   // refresh the sums (rounding drift)
            c12_ = c18_ = {0, 0};
            for (int j = 0; j < SPB; j++) { c12_ += ring12_[j]; c18_ += ring18_[j]; }
        }
        n_++;
        // each tone against its own running level: the radios' pre-emphasis
        // makes 1800 Hz louder than 1200 Hz
        float e12 = std::sqrt(std::norm(c12_)), e18 = std::sqrt(std::norm(c18_));
        lvl12_ += 0.0005f * (e12 - lvl12_);
        lvl18_ += 0.0005f * (e18 - lvl18_);
        float a = e12 / (lvl12_ + 1e-9f), b = e18 / (lvl18_ + 1e-9f);
        m_.push((a - b) / (a + b + 1e-9f));
        // sync at bit spacing
        const int64_t last = m_.end() - 1;
        const int64_t first = last - 19 * SPB;
        if (first < m_.begin()) continue;
        // sync: hard bit decisions, at most 2 of the 20 bits wrong; the soft
        // correlation picks the best sample within a bit
        float best = 0;
        int which = 0;
        for (int w = 0; w < 2; w++) {
            float c = 0;
            int errs = 0;
            for (int b = 0; b < 20; b++) {
                float v = m_.at(first + b * SPB);
                c += tmpl[w][b] * v;
                errs += (v > 0) != (tmpl[w][b] > 0);
            }
            c /= 20.0f;
            if (errs <= 2 && c > best) { best = c; which = w; }
        }
        if (pk_.active) {
            pk_.age++;
            if (best > pk_.best) { pk_.best = best; pk_.idx = last; pk_.which = which; }
            if (pk_.age >= SPB) {
                pk_.active = false;
                jobs_.push_back({pk_.idx, pk_.which});
            }
        } else if (best > THRESHOLD) {
            pk_.active = true;
            pk_.best = best;
            pk_.idx = last;
            pk_.which = which;
            pk_.age = 0;
        }
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!m_.has(it->end + 64 * SPB)) { ++it; continue; }
        decode_slot(it->end, it->which);
        it = jobs_.erase(it);
    }
    // the system: when it is known or changed, then every 5 s (a traffic
    // channel's instance: from the grant's sys option, every 10 s)
    const int64_t every = (is_control() ? 5LL : 10LL) * FS;
    if (sys_sent_ < 0 || n_ - sys_sent_ >= every) send_system();
    int64_t keep = m_.end() - 300 * SPB;
    for (const auto& j : jobs_) keep = std::min(keep, j.end - 80 * SPB);
    if (keep > m_.begin() + 16384) m_.trim_before(keep);
}

}  // namespace dig
