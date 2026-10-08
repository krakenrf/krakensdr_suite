#pragma once

// 🗺 Map markers: places the user pins on the map (right-click -> Add
// Marker) with a name, notes and frequencies - clicking a frequency in the
// page tunes the selected VFO there. Kept on the backend, so every browser
// shows the same markers: map_markers.json (cwd-relative, so the Docker
// volume gets it; written on every change, survives a restart).
//
// Commands (control_handler.cpp, uWS loop thread), each answered with
// message_json() to every browser:
//   MARKER_SET:{"id":..,"lat":..,"lon":..,"name":..,"notes":..,
//               "freqs":[{"hz":..,"label":..}]}    create / replace
//   MARKER_DEL:<id>
//   GET_MARKERS

#include <string>

namespace markers {

constexpr size_t MAX_MARKERS = 500;
constexpr size_t MAX_FREQS = 50;        // per marker
constexpr size_t MAX_NAME = 80;         // bytes (UTF-8)
constexpr size_t MAX_LABEL = 80;
constexpr size_t MAX_NOTES = 4000;

void load();   // startup: read map_markers.json
// Create or replace the marker given as a MARKER_SET JSON object.
// *id = its id (when it could be read); false + *err if refused.
bool set(const std::string& json, std::string* id, std::string* err);
bool remove(const std::string& id);   // false = no such marker
// {"markers":{"list":[{id,lat,lon,name,notes,freqs:[{hz,label}],t}]
//  [,"error":"..","id":".."]}} - error: a MARKER_SET refused (id = the
// marker it was for, so only the page that sent it reports it)
std::string message_json(const std::string& error = "", const std::string& id = "");

}  // namespace markers
