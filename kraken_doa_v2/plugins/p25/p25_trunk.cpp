// P25 trunk following: the system, its channels, grants and the calls on a
// traffic channel in the plugin API's structured form (p25_trunk.hpp).
//
// Channel -> frequency (TIA-102.AABC, the channel identifier updates): a
// 16-bit channel is identifier (4 bits) + channel number (12); downlink =
// base + number x spacing, for an identifier with several TDMA slots per
// carrier (IDEN_UP_TDMA) base + (number / slots) x spacing on timeslot
// number % slots; uplink = downlink + the identifier's transmit offset (the
// sign as the identifier sends it: + = the radios transmit above the
// downlink). The explicit forms name the uplink channel themselves.

#include "p25_trunk.hpp"
#include "p25.hpp"

#include <cstdio>

namespace dig {

namespace {

constexpr double SYSTEM_REPEAT_S = 4.0;    // trunk_system again this often (Auto detect loses the first ones)
constexpr double CONTROL_FOR_S = 10.0;     // a control channel this long after its last block
constexpr double ANNOUNCE_REPEAT_S = 60.0; // a channel announced again after this long
constexpr double CALL_REPEAT_S = 1.0;      // an unchanged ACTIVE at most this often
constexpr double CC_MOVE_REPEAT_S = 30.0;  // control_channel() for the same channel
constexpr double CC_MOVE_MIN_HZ = 4000.0;  // the announced control channel is elsewhere (channels are >= 6.25 kHz apart)

std::string hexs(int v, int digits) {
    char b[16];
    snprintf(b, sizeof b, "%0*X", digits, static_cast<unsigned>(v));
    return b;
}

bool same_freq(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

}  // namespace

void P25Trunk::reset() {
    wacn_ = sysid_ = -1;
    hint_wacn_ = hint_sysid_ = -1;
    nac_ = -1;
    rfss_ = site_ = -1;
    control_at_ = -1e9;
    sent_ = kp::TrunkSystem();
    sys_sent_ = false;
    sys_at_ = -1e9;
    ann_.clear();
    cc_moved_at_ = -1e9;
    cc_moved_name_.clear();
    for (auto& s : calls_) s = Sent();
}

std::string P25Trunk::key() const {
    if (wacn_ >= 0 && sysid_ >= 0) return hexs(wacn_, 5) + "." + hexs(sysid_, 3);
    if (hint_wacn_ >= 0 && hint_sysid_ >= 0) return hexs(hint_wacn_, 5) + "." + hexs(hint_sysid_, 3);
    return "";
}

bool P25Trunk::is_control() const { return host_.time() - control_at_ < CONTROL_FOR_S; }

void P25Trunk::send_system(bool repeat) {
    kp::TrunkSystem s;
    s.key = key();
    s.name = s.key.empty() ? "P25" : "P25 " + s.key;
    if (rfss_ >= 0 && site_ >= 0) s.site = "RFSS " + std::to_string(rfss_) + " site " + std::to_string(site_);
    s.control = is_control();
    if (s.key.empty() && !s.control) return;   // nothing known about a system yet
    const bool changed = !sys_sent_ || s.key != sent_.key || s.name != sent_.name || s.site != sent_.site ||
                         s.control != sent_.control;
    const double now = host_.time();
    if (!changed && !(repeat && now - sys_at_ >= SYSTEM_REPEAT_S)) return;
    host_.trunk_system(s);
    sent_ = s;
    sys_sent_ = true;
    sys_at_ = now;
}

// The system goes out from here (once per input block: what one block's
// frames taught is sent once, in one message), and before every grant,
// channel or call that depends on it
void P25Trunk::tick() { send_system(true); }

void P25Trunk::control_seen() { control_at_ = host_.time(); }

void P25Trunk::network(int wacn, int sysid) {
    if (wacn < 0 || sysid < 0) return;
    wacn_ = wacn;
    sysid_ = sysid;
}

void P25Trunk::network_hint(int wacn, int sysid) {
    hint_wacn_ = wacn;
    hint_sysid_ = sysid;
}

void P25Trunk::nac(int n) { if (n >= 0 && n <= 0xFFF) nac_ = n; }

void P25Trunk::site(int sysid, int rfss, int site, uint32_t cc) {
    if (sysid_ >= 0 && sysid != sysid_) return;   // a neighbouring system's (it carries its own ID)
    rfss_ = rfss;
    site_ = site;
    announce(cc, 0xFFFF, sysid);
    // the site's control channel is not where this VFO is: a control
    // channel that moved (only from a control channel, only when the host
    // says where the VFO is)
    if (!is_control()) return;
    const kp::TrunkChannel c = channel(cc);
    const double rf = host_.rf_hz();
    send_system(false);
    if (std::isnan(c.freq_hz) || std::isnan(rf) || std::fabs(c.freq_hz - rf) < CC_MOVE_MIN_HZ) return;
    const double now = host_.time();
    if (c.name == cc_moved_name_ && now - cc_moved_at_ < CC_MOVE_REPEAT_S) return;
    cc_moved_name_ = c.name;
    cc_moved_at_ = now;
    host_.control_channel(c);
}

kp::TrunkChannel P25Trunk::channel(uint32_t ch, uint32_t ch_r) const {
    kp::TrunkChannel c;
    const int id = static_cast<int>((ch >> 12) & 0xF);
    const int num = static_cast<int>(ch & 0xFFF);
    c.name = std::to_string(id) + "-" + std::to_string(num);
    c.number = num;
    const P25Proto::Iden* in = p1_ ? p1_->iden(id) : nullptr;
    if (!in || !in->spacing_hz || !in->base_hz) return c;
    auto freq = [](const P25Proto::Iden& t, int n) {
        const int slots = (t.tdma && t.slots > 1) ? t.slots : 1;
        return static_cast<double>(t.base_hz) + static_cast<double>(n / slots) * static_cast<double>(t.spacing_hz);
    };
    c.freq_hz = freq(*in, num);
    if (in->tdma && in->slots > 1) c.slot = num % in->slots;
    const P25Proto::Iden* rin = (ch_r != 0xFFFF && ch_r != ch && p1_) ? p1_->iden(static_cast<int>((ch_r >> 12) & 0xF)) : nullptr;
    if (rin && rin->spacing_hz && rin->base_hz) c.uplink_hz = freq(*rin, static_cast<int>(ch_r & 0xFFF));
    else if (in->tx_offset_hz != 0) c.uplink_hz = c.freq_hz + static_cast<double>(in->tx_offset_hz);
    return c;
}

void P25Trunk::announce(uint32_t ch, uint32_t ch_r, int sysid) {
    if (ch == 0xFFFF || (ch == 0 && ch_r == 0xFFFF)) return;   // no channel
    if (sysid >= 0 && sysid_ >= 0 && sysid != sysid_) return;  // another system's numbering
    const kp::TrunkChannel c = channel(ch, ch_r);
    send_system(false);
    Ann& a = ann_[c.name];
    const double now = host_.time();
    if (same_freq(a.freq, c.freq_hz) && now - a.at < ANNOUNCE_REPEAT_S) return;
    a.ch = ch;
    a.ch_r = ch_r;
    a.freq = c.freq_hz;
    a.at = now;
    host_.trunk_channel(c);
}

void P25Trunk::iden_learned(int id) {
    for (auto& [name, a] : ann_) {
        if (static_cast<int>((a.ch >> 12) & 0xF) != id) continue;
        const kp::TrunkChannel c = channel(a.ch, a.ch_r);
        if (same_freq(a.freq, c.freq_hz)) continue;
        a.freq = c.freq_hz;
        a.at = host_.time();
        host_.trunk_channel(c);
    }
}

void P25Trunk::grant(uint32_t ch, const std::string& target, bool group, const std::string& source, int so, bool update,
                     bool data, uint32_t ch_r) {
    if (ch == 0xFFFF) return;
    send_system(false);
    kp::Grant g;
    g.channel = channel(ch, ch_r);
    g.target = target;
    g.group = group;
    g.source = source;
    g.encrypted = so >= 0 && (so & 0x40);
    g.emergency = so >= 0 && (so & 0x80);
    g.data = data;
    g.update = update;
    g.bw_hz = 12500;
    // what the traffic channel's instance needs before it has heard the
    // system itself (README "Trunk following"): the NAC (the system's on
    // every channel - checked on air), the Phase 2 scrambling identity, the
    // timeslot, and which phase the identifier's access type says ("" = not
    // known: the instance detects it). Every key every time ("" = the
    // option's default), so a VFO the host moves on keeps nothing stale
    const P25Proto::Iden* in = p1_ ? p1_->iden(static_cast<int>((ch >> 12) & 0xF)) : nullptr;
    const bool tdma = in && in->tdma && in->slots > 1;
    const std::string w = wacn_ >= 0 && sysid_ >= 0 ? hexs(wacn_, 5) : hint_wacn_ >= 0 && hint_sysid_ >= 0 ? hexs(hint_wacn_, 5) : "";
    const std::string s = wacn_ >= 0 && sysid_ >= 0 ? hexs(sysid_, 3) : hint_wacn_ >= 0 && hint_sysid_ >= 0 ? hexs(hint_sysid_, 3) : "";
    g.options = {{"nac", nac_ >= 0 ? hexs(nac_, 3) : ""},
                 {"wacn", w},
                 {"sysid", s},
                 {"lch", tdma && g.channel.slot >= 0 ? std::to_string(g.channel.slot + 1) : "0"},
                 {"phase", !in ? "" : tdma ? "2" : "1"}};
    host_.grant(g);
}

void P25Trunk::send_call(const kp::Call& c) {
    Sent& s = calls_[c.slot + 1];
    send_system(false);
    host_.call(c);
    s.any = true;
    s.c = c;
    s.at = host_.time();
}

void P25Trunk::call(int state, const std::string& target, bool group, const std::string& source, int slot, bool enc,
                    bool emerg) {
    if (slot < -1 || slot > 1) return;
    Sent& s = calls_[slot + 1];
    kp::Call c;
    c.state = state;
    c.target = target;
    c.group = group;
    c.source = source;
    c.slot = slot;
    c.encrypted = enc;
    c.emergency = emerg;
    if (state == kp::Call::ACTIVE) {
        // the slot carries another call now: the last one is over and the
        // channel is no longer its
        if (s.any && s.c.state != kp::Call::RELEASED && !s.c.target.empty() && !target.empty() &&
            (s.c.target != target || s.c.group != group)) {
            kp::Call r = s.c;
            r.state = kp::Call::RELEASED;
            send_call(r);
        }
        if (s.any && s.c.state == kp::Call::ACTIVE && s.c.target == c.target && s.c.group == c.group &&
            s.c.source == c.source && s.c.encrypted == c.encrypted && s.c.emergency == c.emergency &&
            host_.time() - s.at < CALL_REPEAT_S)
            return;
        send_call(c);
        return;
    }
    // ENDED / RELEASED: the call it ends, as last reported
    if (s.any && c.target.empty()) {
        c.target = s.c.target;
        c.group = s.c.group;
    }
    if (s.any && c.source.empty() && c.target == s.c.target) c.source = s.c.source;
    if (s.any && c.target == s.c.target) {
        c.encrypted = c.encrypted || s.c.encrypted;
        c.emergency = c.emergency || s.c.emergency;
    }
    if (!s.any && state == kp::Call::ENDED) return;   // nothing was on
    if (s.any && s.c.state == state && s.c.target == c.target) return;   // said already
    if (s.any && s.c.state == kp::Call::RELEASED && state == kp::Call::ENDED) return;
    send_call(c);
}

void P25Trunk::release(int slot) {
    if (slot < -1 || slot > 1) return;
    const Sent& s = calls_[slot + 1];
    if (!s.any || s.c.state == kp::Call::RELEASED) return;
    call(kp::Call::RELEASED, "", true, "", slot, false, false);
}

}  // namespace dig
