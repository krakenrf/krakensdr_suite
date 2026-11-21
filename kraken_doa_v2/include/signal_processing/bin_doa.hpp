#pragma once

// 🎨 Bearing spectrum (docs/dev/bearing-spectrum.md): every point of the main
// spectrum coloured by the direction its signal comes from.
//
// A worker thread takes each coherent packet (every antenna), FFTs each
// antenna in blocks and keeps, per frequency bin, the M x M cross-spectral
// covariance of the power-normalised snapshots, exponentially averaged at the
// spectrum trace's time constant. Each tick it publishes a snapshot; a page's
// spectrum view (FFT type 7) then gets a type 9 frame with the same points:
// the covariances of each point's peak bins summed, whitened by each
// antenna's noise, its principal eigenvector matched against the MUSIC
// steering table of the array model (MUSICProcessor::azimuthSteering), so
// the bearing uses MUSIC's convention, offset and calibration. Every point
// gets a bearing (the page colours them all); a confidence rides along.
//
// Off by default (BIN_DOA:1), parked outside coherent mode, while heimdall
// calibrates (noise source), after a retune and while the calibration is
// stale. Costs nothing while off: the receiver converts the extra antennas
// and the worker runs only while the mode is on and a page shows it.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SpectrumDisplay;
struct SpectrumViewCut;

namespace bin_doa {

// BIN_DOA:0|1 (uWS loop thread; persisted). The worker thread starts on the
// first enable.
void set_enabled(bool on);
bool enabled();

// Pages that asked for type 9 frames (BIN_DOA_VIEW:1) and get spectrum views
// - the websocket server keeps the count (uWS loop thread)
void set_viewers(int n);

// True while the mode needs every antenna of a packet converted: on, coherent
// mode, a page watching. The data receiver thread asks per packet
bool wanted();

// The decimation thread, every packet while enabled(): the packet's antennas
// (moved in - the packet is done with them), its number (a gap restarts the
// block assembly), the tuner centre (exact Hz) and `park`: why this packet
// must not be used (nullptr = it may). Never blocks: queued for the worker,
// the oldest dropped past 8 waiting (a gap)
void on_packet(std::vector<std::vector<std::complex<float>>>&& iq, int elements, uint64_t seq,
               double center_hz, const char* park);

// Type 9 (little-endian, 32-byte header) for the points of the main spectrum
// view frame just built (build_spectrum_view's cut, the same seq):
//   u32 9 | u8 display (0) | u8 flags (1 = a ULA's front half assumed,
//   2 = a 3D array's horizon, 4 = every point's azimuth is its bearing) |
//   u8 elements | u8 topology (0 UCA, 1 ULA, 2 custom) | u32 view seq |
//   u32 n | f64 start Hz | f64 step Hz | u8 confidence[n] (0..255 = 0..1,
//   informational - the page does not use it) | u16 azimuth[n] (x 360 /
//   65536 deg, MUSIC's azimuth: counter-clockwise from ANT0, the array
//   offset added)
// "" = nothing to send (parked, no data yet, another centre)
std::string build_frame(const SpectrumDisplay& d, const SpectrumViewCut& cut, uint32_t seq);

// {"bin_doa":{"on":..,"state":"off|running|parked|idle","reason":..,...}} -
// broadcast on every change; the websocket server sends it to a page that
// asks (BIN_DOA_VIEW)
std::string state_json();

void shutdown();

}  // namespace bin_doa
