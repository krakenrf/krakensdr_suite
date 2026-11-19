#pragma once

// What a decoder plugin reports about a TRUNKED system (kp::Host::
// trunk_system, trunk_channel, control_channel, grant, grant_end, call -
// plugins/sdk/kraken_plugin.hpp), as the decoder engine hands it over after
// checking it (digital_decoder.cpp: every text cut to the kp::wire::TRUNK_*
// limits, control characters gone, frequencies outside (0, 6 GHz] = NAN,
// slots outside -1..15 = -1, option keys / values checked). Kept apart from
// trunk_follow.hpp (the trunk manager that consumes it) so the decoder - and
// the offline tools built on it (tools/digi_test.cpp) - need nothing of the
// manager.

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace trunk {

// kp::TrunkChannel
struct Channel {
    std::string name;          // never "" (such a message is dropped)
    long long number = -1;     // -1 = none
    double freq_hz = NAN;      // the downlink, NAN = not known
    double uplink_hz = NAN;    // the uplink (radios' transmit), NAN = not known / simplex
    int slot = -1;             // TDMA slot 0..15, -1 = the whole channel
};

struct PluginEvent {
    // RESET is the host's own: the VFO's earlier SYSTEM / GRANT / CALL state
    // is stale (its decoder was reset by a retune > 100 Hz, stopped,
    // switched to another plugin, or the plugin process died / restarted /
    // was rebuilt; in Auto detect: the lead plugin changed). Sent only after
    // that VFO delivered at least one event since the previous RESET
    enum Kind { SYSTEM, CHANNEL, CONTROL, GRANT, GRANT_END, CALL, RESET } kind = RESET;
    std::string sys_key, sys_name, sys_site;   // SYSTEM (key: [A-Za-z0-9._-]{0,48})
    bool control = false;                      // SYSTEM: this VFO is on a control channel
    Channel ch;                                // CHANNEL / CONTROL / GRANT / GRANT_END
    // GRANT / CALL
    std::string target, source;
    bool group = true, encrypted = false, emergency = false;
    bool data = false, update = false;         // GRANT only
    // GRANT: the traffic channel's plugin ("" = the reporting one), demod
    // ("" = the decoder's voice, "nfm" = analogue FM), width (0 = the
    // decoder's min_vfo_rate), options by the traffic decoder's keys
    std::string decoder, demod;
    double bw_hz = 0;
    std::vector<std::pair<std::string, std::string>> options;
    int call_state = 1;                        // CALL: 0 ENDED, 1 ACTIVE, 2 RELEASED (kp::Call::State)
    int call_slot = -1;                        // CALL
    // the VFO's stream time (s) of the newest input when the event was
    // delivered (the pipeline's stream position / the VFO rate; offline: the
    // samples pushed) - deterministic in offline tests. The plugin's own
    // Host::time() is not on the wire. NAN = no input yet
    double t_s = NAN;
};

}  // namespace trunk
