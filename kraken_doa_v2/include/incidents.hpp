#pragma once

// Incident map: street addresses in the text messages decoder plugins send
// (kp::Host::message - POCSAG text pages) are looked up online in
// OpenStreetMap (Nominatim, geo_http.hpp; finding the address in the text:
// geo_address.hpp), within radius_km of the station (Station Information;
// GEO_RADIUS_KM:, persisted, default 300 km - set in the decoder tab), and
// become 🗺 Map markers + an "Incidents" table in the decoder's tab. No map
// data is downloaded or stored. Without internet the messages wait and are
// tried again every minute for up to 30 minutes.
//
// Incidents are kept for INCIDENT_TTL_H hours (incidents.tsv, survives a
// restart); the same address paged again within 2 h updates the incident
// (pages count) instead of adding one. Each records the VFO + frequency it
// was received on; they are listed in the page's tab of the plugin (e.g.
// POCSAG) - also after that VFO is gone - and shown on the map while any
// decoder running that plugin has "Plot on map" on.

#include <cstdint>
#include <set>
#include <string>

namespace incidents {

constexpr int DEFAULT_RADIUS_KM = 300;
constexpr int INCIDENT_TTL_H = 24;

void start();                 // worker thread + the decoders' message handler; loads the saved incidents
void save_now();              // shutdown: write incidents.tsv
void set_radius_km(int km);   // 10 .. 1000
int radius_km();
void clear();                 // forget every incident
// {"radius_km":..,"state":"idle|ready|offline|error|no_station","error":"..","found":N,
//  "not_found":N,"requests":N,"incidents":N,"queued":N}
std::string status_json();
// Map points of the incidents found by the given plugins (those with a
// decoder running them with "Plot on map" on) for
// MessageBuilders::build_map_message, in dig::Report::map_json's form
// (plugin "incidents", v = the VFO that received it); returns the newest seq
uint64_t map_json(const std::set<std::string>& plugins, uint64_t after, std::string& pts, std::string& keys);
// Bumped by every change (new / updated / expired incident, clear)
uint64_t seq();
// {"incidents":{"seq":N,"geo":status_json(),"cols":["VFO","MHz","Paged",..],
//  "rows":[[id, age_s, plugin, vfo, cells..]]}} - every incident, for the
// page's decoder tabs (each shows its plugin's)
std::string message_json();

}  // namespace incidents
