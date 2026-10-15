#pragma once

// Debug playback: a decoder's synthetic multi-antenna test transmission
// (plugins/<id>/tools/testsignals.json, built by
// plugins/sdk/tools/make_testsignal.py into testsignal_cache/) played through
// the real pipeline as if heimdall had received it - spectrum, waterfall,
// VFOs, decoders, DoA, the map's DF panel.
//
// While it plays: heimdall's 8091 stream is disconnected (the data receiver
// reads playback::read() instead - the same packet format, CONVERGED,
// coherent), nothing is sent to heimdall (DataReceiver::send_control_command
// drops it), commands that need the real receiver are refused (refuse()),
// the settings store is a sandbox (nothing written; the snapshot from before
// is put back and replayed on stop - a restart mid-playback comes back live
// with the file untouched), and nothing goes outward: web mapper, decoder
// data log, Voice to Text, incidents, DoA recording, array calibration,
// rdf_session.bin, noise_floors.json.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace playback {

extern std::atomic<bool> g_active;
inline bool active() { return g_active.load(std::memory_order_relaxed); }

// PLAYBACK_LIST | PLAYBACK_GEN:<id> | PLAYBACK_GEN_CANCEL | PLAYBACK_START:<id>
// | PLAYBACK_STOP | PLAYBACK_DELETE:<id>  (uWS loop thread)
bool handle_command(std::string_view msg);

// Why `msg` can't run during playback (nullptr = it can). Not for the
// playback's own session commands.
const char* refuse(std::string_view msg, uint64_t freq_hz_if_freq);

// true while playback applies its own session commands
bool internal();

// The data receiver's source while active(): the next packet (header +
// channel-major uint8 IQ), paced to real time. 0 = nothing (not active).
ssize_t read(uint8_t* buf, size_t cap);

// RF of the playback (Hz), 0 when not active
uint64_t rf_hz();

// {"playback":{...}} when something changed or every 2 s while active /
// generating; "" otherwise (1 Hz timer, uWS loop)
std::string tick_message();

}  // namespace playback
