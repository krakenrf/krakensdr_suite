#pragma once

// Mobile direction finding on the 🗺 Map (coherent mode): each VFO's DoA lobe
// drawn at the station, and - while driving - a heat map of where the
// transmitter is (rdf_engine.hpp has the algorithm).
//
// Sampler thread (10 Hz): records the station's GPS track (position,
// heading, speed - stamped with the local receive time minus FIX_LATENCY),
// takes every NEW MUSIC frame of every VFO (MusicProcessor result stamp),
// pairs it with the position + heading interpolated to the frame's time
// (stamp - FRAME_LAG), rejects it while the heading is unusable (no fix /
// GPS course below MIN_SPEED / turning faster than the yaw limit / noise-
// source calibration), rotates it to true north (rdf::north_lobe, blurred by
// the bearing uncertainty of that heading source) and feeds the VFO's
// distance gate. Engine thread: adds the gated records to the VFO's
// rdf::Solver and prepares the grid messages. A VFO's map starts over when
// its frequency changes by more than RETUNE_RESET_HZ (another signal).
// Records are saved (rdf_session.bin, cwd) every 30 s and at shutdown and
// picked up again by a VFO on the same frequency within SESSION_MAX_AGE_H.
//
// Talkers (talker_doa.hpp: a P25 unit ID...) are maps of their own: each one's
// frames feed its own gate / solver (key VFO + talker ID; sessions saved per
// frequency + talker), and the status lists every talker with the lobe of its
// latest transmission. A talker's solver is created with its first record;
// at most MAX_TALKER_MAPS are kept in memory (the stalest one is parked).
//
// Settings (persisted): RDF:0|1 (collect, default on), RDF_RANGE_KM:1-50
// (half-width of the coarse grid, default 10). RDF_RESET:<vfo|-1>[:<talker>],
// GET_RDF (the next grid push carries every VFO / talker).

#include <cstdint>
#include <string>
#include <vector>

namespace rdfmap {

constexpr double FRAME_LAG_S = 0.3;        // a MUSIC frame describes the samples this long before its stamp
constexpr double FIX_LATENCY_S = 0.15;     // gpsd reports a fix this long after it was valid
constexpr double MIN_SPEED_MPS = 2.0;      // GPS course below this: no heading
constexpr double YAW_LIMIT_GPS_DPS = 8;    // turning faster: the course lags the vehicle
constexpr double YAW_LIMIT_COMPASS_DPS = 30;
constexpr double RETUNE_RESET_HZ = 10000;
constexpr double SESSION_MAX_AGE_H = 24;
constexpr double NATS_RANGE = 12;          // heat map colour scale: 0 .. -12 nats below the peak

void start();
void stop();   // shutdown: saves the session

void set_enabled(bool on);
bool set_range_km(double km);
// -1 = every VFO and talker; a VFO = it and its talkers; vfo + tid = that talker
void reset(int vfo, const std::string& tid = "");
void request_full();

// {"rdf":{...}}: station + every VFO's live lobe (north frame, 720 x 0.5 deg,
// linear min..max like the MUSIC DoA plot) and gate state, + every talker's
// (talkers:[{vfo, tid, label, active, tx, ...}]) - 2 Hz
std::string status_message();
// {"rdf_grid":{...}} per VFO whose map changed since the last call (at most
// every 2 s each; every VFO after request_full())
std::vector<std::string> grid_messages();

}  // namespace rdfmap
