#pragma once

// Per-cell channel estimation: the cell's OWN reference signals, descrambled
// with its cell ID, give every antenna's channel to that cell alone - the
// other cells on the carrier use other codes (LTE: other CRS sequences, and
// for 5 in 6 PCIs other subcarriers; NR: other SSS m-sequences), so after
// descrambling they spread like noise over the impulse response, while the
// cell's own paths stay sharp peaks.
//
// LTE: CRS of antenna ports 0 and 1 in symbols 0 and 4 of every slot (one
// slot = one snapshot, both symbols together: every 3rd subcarrier -> 22 us
// of impulse response). NR: the SSS of every SS/PBCH block (127 subcarriers).
// Each snapshot -> every antenna's impulse response (Hann-windowed) -> the
// per-tap spatial covariance; the estimation floor (taps without the cell:
// noise + other cells' leakage) is subtracted from the chosen path's main
// lobe, which gives the cell's spatial covariance (kp::Spatial).

#include "linalg.hpp"
#include "search.hpp"

#include <vector>

namespace cells {

struct EstParams {
    Numerology num;
    double carrier_off = 0;             // Hz, as in SearchParams
    double vis_hz = 1.05e6;
    double min_snr_db = 8;
    bool first_path = false;            // the bearing from the first path (line of sight) instead of the strongest
    int max_snaps = 160;                // LTE: slots used per burst
    bool quick = false;                 // LTE: port 0 only (a cheap check of a candidate)
};

struct CellEst {
    bool ok = false;
    int m = 0;
    Mat cov;                            // the chosen path's spatial covariance, unit trace
    double power = 0;                   // its power per antenna (linear; the same scale for every cell)
    double snr_db = 0;                  // its peak above the estimation floor
    double delay_us = 0;                // the chosen path after the cell's sync timing
    double spread_us = 0;               // rms delay spread of the paths above the threshold
    double first_us = 0;                // the first path above the threshold
    int paths = 0;                      // peaks above the threshold
    int ports = 1;                      // LTE: CRS antenna ports seen (1 / 2)
    double cfo_hz = NAN;                // LTE: residual carrier offset of the cell
    int snaps = 0, used = 0;            // snapshots taken / kept (TDD: uplink slots dropped)
    double purity = 0;                  // largest eigenvalue / all: ~1 = one direction
    std::vector<float> profile;         // the impulse response (dB above the floor) for verbose output
    double tap_us = 0;
    int tap0 = 0;
};

// x: the antennas mixed so that the carrier / SSB centre is at 0 Hz
CellEst estimate(const std::vector<const cf*>& x, size_t n, const CellHit& c, const EstParams& p);

}  // namespace cells
