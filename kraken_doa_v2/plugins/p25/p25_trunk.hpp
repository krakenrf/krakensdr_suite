#pragma once

// P25 trunk following (p25_trunk.cpp): what the Phase 1 and Phase 2 decoders
// of the plugin learn about a trunked system, reported in the plugin API's
// structured form (kp::Host::trunk_system, trunk_channel, control_channel,
// grant, call - SDK.md "Trunked systems"). Both phases share one instance:
// the channel identifier tables (P25Proto::iden) turn a 16-bit channel
// (identifier 4 + number 12) into the downlink, the uplink and the TDMA
// slot; the network identity and the NAC go into the grants' options, so a
// traffic channel's decoder starts with them.

#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#include "kraken_plugin.hpp"

namespace dig {

class P25Proto;

class P25Trunk {
public:
    explicit P25Trunk(kp::Host& h) : host_(h) {}
    void set_phase1(const P25Proto* p) { p1_ = p; }
    // forget what the signal said (the plugin's reset(), a retune): the
    // system is sent again once it is known
    void reset();
    // once per input block: repeats the system every few seconds
    void tick();

    // --- the system --------------------------------------------------------
    // a trunking block (TSBK) or a TDMA control channel's LCCH burst passed
    // its checks: this VFO is on a control channel
    void control_seen();
    // WACN / system ID from the network status broadcast (Phase 1 TSBK,
    // Phase 2 MAC): the network's key
    void network(int wacn, int sysid);
    // the identity a Phase 2 traffic channel works with (options, recovered
    // from its bursts): the key when no network status was heard
    void network_hint(int wacn, int sysid);
    // the NAC of the frames this VFO decodes (Phase 1 NID, Phase 2 colour code)
    void nac(int nac);
    // RFSS status (this site, its control channel)
    void site(int sysid, int rfss, int site, uint32_t cc);

    // --- channels -------------------------------------------------------------
    // a 16-bit channel -> name "id-number", number, downlink / uplink / slot
    // when the identifier is known; ch_r = the explicit forms' other (uplink)
    // channel, 0xFFFF = none
    kp::TrunkChannel channel(uint32_t ch, uint32_t ch_r = 0xFFFF) const;
    // the system announced a channel (control channels of this site and its
    // neighbours, secondary control channels, a data channel): reported
    // when it is new, its frequency became known, or a minute passed
    void announce(uint32_t ch, uint32_t ch_r = 0xFFFF, int sysid = -1);
    // a channel identifier became known or changed: the channels announced
    // before it again, now with their frequencies
    void iden_learned(int id);

    // --- grants -----------------------------------------------------------------
    // a grant heard (control channel, or a traffic channel's grant update):
    // so = the service options octet (0x80 emergency, 0x40 protected), -1
    // = none in the message
    void grant(uint32_t ch, const std::string& target, bool group, const std::string& source, int so, bool update,
               bool data = false, uint32_t ch_r = 0xFFFF);

    // --- calls on a traffic channel ----------------------------------------------
    // per slot (-1 = Phase 1, 0 / 1 = Phase 2 LCH): the call it carries.
    // ACTIVE repeats at most once a second unless something changed; ENDED /
    // RELEASED once per transition; an ACTIVE whose known target differs
    // from the slot's last call releases that one first
    void call(int state, const std::string& target, bool group, const std::string& source, int slot, bool enc,
              bool emerg);
    // RELEASED for the slot's last reported call (its target), if it was not
    // released already. Nothing when no call was reported: a clear-down
    // heard first may be the previous call's on a channel just granted again
    void release(int slot);

private:
    kp::Host& host_;
    const P25Proto* p1_ = nullptr;
    // system
    int wacn_ = -1, sysid_ = -1;         // network status
    int hint_wacn_ = -1, hint_sysid_ = -1;
    int nac_ = -1;
    int rfss_ = -1, site_ = -1;
    int site_sysid_ = -1;                // the system ID of this site's RFSS status
    int own_sysid() const;               // network status's, else the RFSS status's
    double control_at_ = -1e9;           // host time of the last control channel block
    kp::TrunkSystem sent_;
    bool sys_sent_ = false;
    double sys_at_ = -1e9;
    void send_system(bool force);
    bool is_control() const;
    // channels announced: name -> (freq, time)
    struct Ann { uint32_t ch = 0xFFFF, ch_r = 0xFFFF; double freq = NAN; double at = -1e9; };
    std::map<std::string, Ann> ann_;
    double cc_moved_at_ = -1e9;
    std::string cc_moved_name_;
    // calls per slot (index slot + 1)
    struct Sent {
        bool any = false;
        kp::Call c;
        double at = -1e9;
    };
    Sent calls_[3];
    void send_call(const kp::Call& c);
    std::string key() const;
};

}  // namespace dig
