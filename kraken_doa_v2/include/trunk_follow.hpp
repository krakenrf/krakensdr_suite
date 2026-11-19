#pragma once

// Trunk following: kraken_doa follows the calls of a trunked radio system
// that a decoder plugin reads on a control channel - a temporary voice VFO
// with the right decoder on each granted channel (inside the receiver's
// 2.4 MHz) for the length of the call. Design: docs/dev/trunk-following.md.
//
// Stage A (this file as committed first): only the hand-over from the
// decoders - on_plugin_event() keeps the last events per VFO for tests.

#include <string>
#include <vector>

#include "digital/trunk_event.hpp"   // trunk::Channel, trunk::PluginEvent

namespace trunk {

// Called from a decoder worker thread (DecimatorManager::ensureDigital wires
// it to every VFO's DigitalDecoder): the plugin `plugin_id` running on VFO
// `vfo` reported this (already checked by the engine). In Auto detect only
// the lead plugin's reports arrive. Returns at once; safe from any thread.
void on_plugin_event(int vfo, const std::string& plugin_id, PluginEvent&& ev);

// Tests: the last RECENT_MAX events of a VFO, oldest first (each with the
// plugin id that sent it), and forgetting them all
constexpr size_t RECENT_MAX = 32;
std::vector<std::pair<std::string, PluginEvent>> recent_events(int vfo);
void clear_recent();

}  // namespace trunk
