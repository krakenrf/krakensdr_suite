#pragma once

// Trunk following: kraken_doa follows the calls of a trunked radio system
// that a decoder plugin reads on a control channel - a temporary voice VFO
// with the right decoder on each granted channel (inside the receiver's
// span, no retune) for the length of the call. Design, commands, the status
// and the systems file: docs/dev/trunk-following.md.
//
// Threads: on_plugin_event() is called on decoder worker threads and only
// queues; everything else runs on the uWS loop (the commands through
// ControlHandler::apply_command, tick() from a loop timer), except the few
// functions marked "any thread".

#include <string>
#include <string_view>
#include <vector>

#include "digital/trunk_event.hpp"   // trunk::Channel, trunk::PluginEvent

namespace trunk {

// Any thread (DecimatorManager wires it to every VFO's DigitalDecoder): the
// plugin `plugin_id` running on VFO `vfo` (its decoder `decoder_serial` -
// DecimatorInstance::digital_serial, never repeated: ids are reused) reported
// this, already checked by the engine. In Auto detect only the lead plugin's
// reports arrive. Queues it for the loop and returns at once (bounded: when
// full, everything but a RESET is dropped - a RESET is coalesced per VFO and
// decoder instead)
void on_plugin_event(int vfo, uint64_t decoder_serial, const std::string& plugin_id, PluginEvent&& ev);

// Tests: the last RECENT_MAX events of a VFO, oldest first (each with the
// plugin id that sent it), and forgetting them all. Any thread
constexpr size_t RECENT_MAX = 32;
std::vector<std::pair<std::string, PluginEvent>> recent_events(int vfo);
void clear_recent();

// main(): the systems file (trunk_systems.json) at startup; at shutdown the
// current state, written once by the uWS loop (main waits up to 2 s for it,
// else writes the loop's last copy, at most 1 s old) - no write after it
void load();
void save_now();

// uWS loop: the state machine, every TICK_MS (websocket_server.cpp's timer)
constexpr int TICK_MS = 50;
void tick();

// uWS loop: one of the manager's own commands is running - not saved, and
// the VFO snapshot isn't recorded for it (as sfscan::dispatching())
bool dispatching();
// ControlHandler::apply_command, before every command (uWS loop): a command
// that ends an excursion outside the span (the user's FREQ - no return; a mode
// change, a scanner or a playback start - home first)
void before_command(std::string_view m);

// Any thread: VFO `id` (the instance the manager holds now) is a voice VFO
// of the manager - left out of the saved VFO snapshot (DECIMATORS:) and of
// the saved-frequency scan's tuner count
bool is_trunk_vfo(int id);
// Any thread: the VFO's "trunk" member for decimator_info (",\"trunk\":{...}",
// "" = neither a voice nor a control VFO of the manager)
std::string vfo_info_json(int id);

// uWS loop: every voice VFO removed at once and their calls ended (the VFO
// snapshot is about to be applied - the settings replay after a playback,
// RESET_SETTINGS: it works by list position)
void release_all_vfos(const std::string& why);

// uWS loop: a TRUNK_* / GET_TRUNK* command (control_handler.cpp).
// Returns false if `msg` is none of them; else true, with *err = why it was
// refused ("" = applied) and *persist = the command to record in the
// settings file ("" = none)
bool handle_command(const std::string& msg, std::string* err, std::string* persist);

// {"trunk":{...}} - the status (uWS loop)
std::string status_json();

}  // namespace trunk
