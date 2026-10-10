#pragma once

// ✈ Array calibration from aircraft (ADS-B).
//
// The KrakenSDR's noise source calibrates the receivers INSIDE the box, at
// every frequency; what lies outside - the antenna cables, the antennas, where
// they really stand - it can't see. ADS-B aircraft say where they are, so a
// packet whose talker gave its position (kp::Talker::lat / lon / alt_m) is a
// source in a KNOWN direction, and the dominant eigenvector of the packet's
// covariance (talker_doa.cpp) is the array's real response to it. A fit over
// many aircraft finds, in physical units:
//   - a rotation of the whole array (mounting / station heading error),
//   - each element's extra delay (cable length differences; element 0 = 0),
//   - each element's position error in the horizontal plane.
// Delays and positions are what carry over to other frequencies (a delay's
// phase scales with the frequency; copying the phases measured at 1090 MHz to
// another frequency would be wrong). MUSIC and the beamformer build their
// steering vectors from the corrected geometry at whatever frequency they
// run (corrections()). What the model can't explain - antenna / coupling
// differences, multipath - is reported as the residual: at one frequency an
// antenna's own phase offset looks exactly like a cable delay and is
// absorbed into it.
//
// The fit runs on a worker thread every FIT_EVERY_MS while new packets come
// in, cross-checked: fitted on half the aircraft, the bearings of the other
// half compared before / after (the operational el = 0 steering, aircraft
// below EVAL_MAX_EL_DEG). The user applies a result (array_cal.json, cwd).
//
// Threads: add / set_state / note_array on the rdf sampler thread,
// corrections() from MUSIC / beamformer setup (under their own locks - this
// module never calls back into them), commands on the uWS loop.

#include <Eigen/Dense>

#include <cstdint>
#include <string>

namespace array_cal {

// --- in use ---------------------------------------------------------------------
// Bumps whenever the corrections in use change: steering vectors are rebuilt.
uint64_t generation();
// Corrections for an array with these nominal element positions (metres, the
// frame MUSIC steers in: x towards ANT0's angle 0, y 90 deg counter-clockwise):
// position offsets dx / dy (metres, the rotation included when it is on) and
// each element's extra delay tau_s (seconds; the steering phase gets
// -2 pi f tau). false = no calibration in use for this array (none, off, or
// made for other positions / another element count) - outputs untouched.
bool corrections(int n, const double* x, const double* y, double* dx, double* dy, double* tau_s);

// --- collecting -----------------------------------------------------------------
bool collecting();
// Why packets are (not) used now: "ok", "not_coherent", "doa_off",
// "no_station", "no_heading", "moving", "ula", "calibrating" (the rdf sampler,
// every tick while collecting)
void set_state(const char* state);
// The array the steering uses now: nominal positions in metres (n = 0: not
// calibratable - a ULA). The packets are raw measurements, so other positions
// (radius / custom positions edited) only mean a new fit against them; another
// element count starts the collection over.
void note_array(int n, const double* x, const double* y, const double* z);
// One packet with a known position. The steering angle of compass bearing b
// is heading_deg - offset_deg - b (rdf_mapper's compass = heading - displayed
// angle; displayed = steering + array offset). Both are baked into the
// packets, so a changed array offset - or a changed static heading
// (heading_fixed) - starts the collection over. st_alt_m: station height
// above sea level (0 when unknown - a few hundred metres hardly move the
// elevation).
void add(const Eigen::MatrixXcd& R, const std::string& id, double lat, double lon, double alt_m, double st_lat,
         double st_lon, double st_alt_m, double heading_deg, bool heading_fixed, double offset_deg, double freq_hz);

// --- control (uWS loop) --------------------------------------------------------------
void start();   // loads array_cal.json, starts the fit worker
void stop();
void set_collect(bool on);
void reset_samples();
bool apply(std::string* err);   // the latest fit becomes the one in use (saved)
void set_use(bool on);          // use the saved calibration or not
void set_rotation(bool on);     // include its rotation
void remove();                  // forget the one in use (file deleted)
std::string status_message();   // {"array_cal":{...}}

}  // namespace array_cal
