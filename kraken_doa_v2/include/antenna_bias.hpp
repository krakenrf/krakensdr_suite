#pragma once
// Antenna bias tees from the web page (docs/dev/bias-tees.md): the top bar's
// "Bias tee" button switches heimdall's per-port antenna bias tees (DC on the
// antenna ports, for active antennas and LNAs).
//
//   ANT_BIAS:<mask>   bit N = channel N, 0-255: the open channels' bias tees
//                     as wanted -> heimdall {"command":"set_antenna_bias_tees",
//                     "mask":N} on its control port (8092)
//
// heimdall owns the state (it applies, saves and re-applies it), so nothing
// is persisted here and nothing replayed: the pages draw what heimdall's 2 Hz
// status says ({"antenna_bias":{...}} in system_status, and as a message of
// its own after a refusal). Taken only from a page's own WebSocket (as
// POWER: - never the settings replay, the AI assistant, the web mapper, a
// test transmission or the scanners), refused while a debug playback or a
// recording plays (nothing may reach heimdall), and when heimdall can't do
// it: not connected, too old (its status has no antenna_bias_tees_available),
// generic RTL-SDRs, the Wideband variant, a reserved channel.
#include <cstddef>
#include <string>
#include <string_view>

namespace antbias {

// Bytes read from heimdall's control socket (any chunking): its status lines
// and command replies. Thread-safe (the status thread's drain).
void ingest(const char* data, size_t n);

bool is_command(std::string_view m);   // "ANT_BIAS:..."

// uWS loop thread. from_page = it came in on a browser's WebSocket. "" = sent
// to heimdall, else why not (every page is told: {"antenna_bias":{...}}).
std::string handle_command(std::string_view m, bool from_page);

// The state object, {"state":"ok|none|old|generic|wideband",...}, for
// system_status ("antenna_bias":...). Thread-safe.
std::string state_json();
// {"antenna_bias":<state_json()>} - a connecting page's copy, a refusal
std::string state_message();

}  // namespace antbias
