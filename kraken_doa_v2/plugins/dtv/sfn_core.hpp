#pragma once

// The standard-independent part of the dtv plugin's path separation: channel
// impulse responses (CIR) of every antenna -> the PATHS the signal arrives
// on. A path is a reflection (a hill, a building) or another transmitter of
// a single frequency network: both are delayed copies of the same signal
// from their own direction, and nothing in a TV signal tells them apart.
//
// A standard's estimator (ofdm.cpp, atsc.cpp, dtmb.cpp) measures each
// antenna's CIR against the signal's KNOWN reference (pilots, field sync, PN
// header): h_m(tau) = sum over paths k of a_m(theta_k) g_k p(tau - tau_k),
// p = the slice's band-limited pulse. At a path's delay the antennas' values
// are its spatial signature a(theta_k) - clean, whatever the other paths do.
// Per cycle (one burst of samples) the core sums C(tau) = sum h(tau)
// h(tau)^H over the snapshots (non-coherent: Doppler / phase drift between
// snapshots don't matter), estimates the noise floor F (the taps without
// paths), finds the peaks of D(tau) = tr(F^-1 C(tau)) / m, takes each peak's
// covariance (its main lobe minus the floor) and tracks the peaks across
// cycles (the receiver's timing reference may move between bursts: a common
// offset is fitted first). Confirmed paths go out as kp::Spatial "Path-n"
// (kraken_doa runs MUSIC on them: a DoA lobe each on the map), delays from
// the main (strongest) path, plus the CIR as a plot, a table and events.

#include "kraken_plugin.hpp"
#include "linalg.hpp"

#include <string>
#include <vector>

namespace sfn {

// One snapshot: every antenna's CIR on a circular tap grid
struct CirSnapshot {
    int m = 0;
    int ntap = 0;                   // taps (tap ntap = tap 0: the estimator's unambiguous range)
    double tap_us = 0;              // tap spacing
    std::vector<kp::cf> h;          // m x ntap, antenna-major
    double t0 = 0, t1 = 0;          // Host::time() of the samples it comes from
};

// A transmitter identifier the standard sends (DAB TII) with the spatial
// signature it was received with: a transmitter of the CIR whose signature
// matches takes it as its label
struct IdHint {
    std::string id;                 // "TII 12-03"
    std::vector<cd> sig;            // unit vector, m long
    double db = 0;                  // its level above the noise
};

class SfnCore {
public:
    explicit SfnCore(kp::Host& h) : host_(h) {}
    void set_min_snr_db(double db) { min_snr_db_ = db; }
    void set_max_tx(int n) { max_tx_ = n; }
    // peaks more than this far below the strongest are left out (0 = none):
    // with a strong signal, local reflections 30+ dB down pass min_snr too
    void set_max_rel_db(double db) { max_rel_db_ = db; }
    void add(const CirSnapshot& s);
    // Analyse the snapshots since the last cycle and report. what = the
    // estimator's description ("DVB-T 8k 1/4, 8 MHz")
    void cycle(const std::string& what);
    // the identifiers the estimator heard (has = the standard sends them:
    // the table gets an ID column)
    void set_ids(bool has, std::vector<IdHint> ids) {
        has_ids_ = has;
        ids_ = std::move(ids);
    }
    // path separation off: no tracks, table, spatial reports or events - the
    // CIR is shown as the echo profile of the signal (one antenna is
    // enough), its paths marked relative to the strongest
    void set_profile_only(bool on);
    bool profile_only() const { return profile_only_; }
    // profile-only: paths further below the strongest aren't marked (the
    // estimator's Estimator::profile_floor_db; 0 = max_rel_db alone)
    void set_profile_floor_db(double db) { profile_floor_db_ = db; }
    void reset();                   // forget everything (retune, another standard)
    int confirmed() const;
    int snapshots_total() const { return snaps_total_; }

private:
    kp::Host& host_;
    double min_snr_db_ = 8.0;
    int max_tx_ = 8;
    double max_rel_db_ = 30.0;
    bool profile_only_ = true;      // the plugin's default: separation off
    double profile_floor_db_ = 0;
    int main_id_ = -1;              // the main path's track (kept unless another is 1 dB stronger)
    // profile-only: the paths of the last cycles (delay from the strongest, taps)
    std::vector<std::vector<double>> prof_hist_;
    // accumulation
    int m_ = 0, ntap_ = 0;
    double tap_us_ = 0;
    std::vector<Mat> C_;            // per tap
    int snaps_ = 0;
    double t0_ = 0, t1_ = 0;
    int snaps_total_ = 0;

    struct Track {
        int id = 0;
        double d = 0;               // delay (taps, in the current timing frame)
        std::vector<cd> sig;        // spatial signature (unit)
        Mat Cavg;                   // averaged unit-trace covariance
        double pw = 0, snr_db = 0;
        int hits = 0, misses = 0;
        bool confirmed = false;
        double first_t = 0, last_t = 0;
        std::string ident;          // matched IdHint
    };
    bool has_ids_ = false;
    std::vector<IdHint> ids_;
    void match_ids();
    // the first path to arrive: the one after the largest circular gap
    int earliest() const;
    // the main path: the strongest confirmed one (with hysteresis)
    int strongest();
    // a track's talker / table key
    static std::string tid(int id) { return "Path-" + std::to_string(id); }
    std::vector<Track> tracks_;
    int next_id_ = 1;
    double last_tap_us_ = 0;
    int last_ntap_ = 0;
    // first / main = track indexes (-1 = none)
    void report(const std::vector<double>& D, double floor, double thr, const std::string& what, int first, int main);
    // profile-only: the paths found this cycle (delay in taps, power)
    void report_profile(const std::vector<double>& D, double floor, double thr, const std::vector<std::pair<double, double>>& paths,
                        const std::string& what);
};

}  // namespace sfn
