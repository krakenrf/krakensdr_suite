#pragma once

// 🗺 The AI assistant's map layers: what it plots on the page's map
// (transmitter sites from a licence register or its own knowledge, bearing
// lines, areas, measurement points - any data it comes across). One list for
// every page, kept in ai/map_layers.json (cwd-relative, gitignored: positions
// near the station), shown by the page's "🤖 AI layers" panel (show / hide per
// browser, delete). Written by the MCP tools map_plot / map_remove (ai_mcp.cpp)
// and the page's AI_MAP_DELETE.
//
// A layer = id (slug of its name), name, description, source (where the data
// came from), conversation, colour, time, features. Features in the page's
// compact form: {"g":"pt"|"ln"|"pg"|"ci", "c": [lat,lon] | [[lat,lon]..] |
// [[[lat,lon]..]..] (rings), "r": circle radius m, "l": label, "col": "#rrggbb",
// "ic": icon (emoji), "sz": point size px, "w": line width px, "da": dashed,
// "fo": fill opacity, "ar": an arrow at a line's end, "x": [[key, value]..]}.
// Input: the tool's own features (lat / lon order), GeoJSON (lon / lat, as the
// standard says) or a .geojson / .json / .csv file in the conversation's folder
// or ai/knowledge. Limits: 30 layers, 5000 features / 20000 vertices a layer,
// 1 MB for all of it (one WebSocket message to every page).

#include <cstdint>
#include <string>

namespace geo { struct Json; }

namespace aimap {

void load();   // startup: ai/map_layers.json

// map_plot: a layer added / replaced / extended from the tool's arguments
// (layer, description, source, color, replace, features, geojson, file).
// conv_dir = the asking conversation's folder (relative paths in `file` are
// taken from there), conv = its id. "" + *summary (JSON for the agent) or
// the error
// station = the station's [lat, lon] (bearing lines without a start begin
// there), nullptr = unknown
std::string plot(const geo::Json& args, const std::string& conv_dir, const std::string& conv, const double* station,
                 std::string* summary);
// a layer by id or name, or "all" -> "" + *summary or the error
std::string remove(const std::string& which, std::string* summary);
std::string list_json();       // [{id,name,description,source,features,created,conversation,bbox}]
std::string brief_json();      // [{id,name,features}] (get_screen)
// {"ai_map":{"seq":N,"layers":[..]}} - the whole set, to every page
std::string message();
// a layer's extent (south, west, north, east); false = no such layer / empty
bool bbox(const std::string& id, double* s, double* w, double* n, double* e);
// the layer's id for an id or a name ("" = none)
std::string find(const std::string& which);

}  // namespace aimap
