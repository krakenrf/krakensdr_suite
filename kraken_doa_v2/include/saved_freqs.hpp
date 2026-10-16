#pragma once

// 📻 Saved frequencies + 📍 map markers (one feature): the user's list of
// frequencies, grouped by category, and the places they are transmitted
// from. A LOCATION is a map marker (name, position, notes); a FREQUENCY has
// a name and a frequency (both required) and optionally a category, a
// location, how to receive it (demod, bandwidth, gain, digital decoder), a tone /
// code and notes. Every frequency that points at a location is listed in
// that marker's panel on the map; a marker without frequencies is a plain
// marker. Kept on the backend, so every browser shows the same list:
// saved_frequencies.json (cwd-relative, so the Docker volume gets it;
// written on every change). The old map_markers.json is turned into
// locations + frequencies on the first start (then renamed .migrated).
//
// Commands (control_handler.cpp, uWS loop thread), each answered with
// message_json() to every browser:
//   SF_SET:{"id","name","hz","cat","loc","mode","bw","gain","dec","tone","notes"
//           [,"newcat":{"id","name","col"}][,"newloc":{"id","name","lat",
//           "lon","notes"}]}           frequency create / replace (+ the new
//                                      category / location it refers to)
//   SF_DEL:<id>                        frequency
//   SF_LOC_SET:{"id","name","lat","lon","notes"}   location create / replace
//   SF_LOC_DEL:<id>                    location (its frequencies stay, unplaced)
//   SF_CAT_SET:{"id","name","col"}     category create / replace
//   SF_CAT_DEL:<id>                    category (its frequencies: uncategorised)
//   GET_SF

#include <string>

namespace savedfreq {

constexpr size_t MAX_FREQS = 2000;
constexpr size_t MAX_LOCS = 500;
constexpr size_t MAX_CATS = 64;
constexpr size_t MAX_NAME = 80;          // bytes (UTF-8), frequency / location names
constexpr size_t MAX_CAT_NAME = 40;
constexpr size_t MAX_TONE = 40;
constexpr size_t MAX_FREQ_NOTES = 2000;
constexpr size_t MAX_LOC_NOTES = 4000;
constexpr size_t MAX_TOTAL = 1024 * 1024;   // the whole list as JSON: one broadcast, well under
                                             // the WebSocket server's 2 MB backpressure limit

void load();   // startup: read saved_frequencies.json (or migrate map_markers.json)

// Each returns false + *err if refused; *id = the entry the command was for
// (when it could be read), so only the page that sent it reports the error
bool set_freq(const std::string& json, std::string* id, std::string* err);
bool set_location(const std::string& json, std::string* id, std::string* err);
bool set_category(const std::string& json, std::string* id, std::string* err);
bool remove_freq(const std::string& id);       // false = no such entry
bool remove_location(const std::string& id);
bool remove_category(const std::string& id);

// {"savedfreq":{"cats":[{id,name,col}],"locs":[{id,name,lat,lon,notes,t}],
//  "freqs":[{id,name,hz,cat,loc,mode,bw,gain,dec,tone,notes,t}]   gain: null =
//  keep the receiver's, -1 = Auto (AGC), else dB
//  [,"error":"..","id":".."]}}
std::string message_json(const std::string& error = "", const std::string& id = "");

}  // namespace savedfreq
