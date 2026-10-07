#pragma once

// Mobile direction finding: DoA bearings taken while driving -> a likelihood
// grid ("heat map") of where the transmitter is. Pure algorithm (no globals,
// no I/O): used by the live mapper (rdf_mapper.cpp) and the offline drive
// simulator (tools/rdf_sim.cpp).
//
// Pipeline
//   1. A MUSIC frame (pseudospectrum relative to the array, unit circle CCW)
//      + the vehicle's position and heading AT THE FRAME'S TIME is turned
//      into a bearing probability in the north frame (north_lobe): rotated by
//      the heading, blurred by the frame's bearing uncertainty (MUSIC spread
//      + heading error) and normalised.
//   2. Gate: frames are combined (geometric mean of their probabilities = the
//      repeated views of one bearing multiply, tempered to one record) over a
//      window that closes
//      when the vehicle has moved far enough for the bearing to the
//      transmitter to change by about the bearing noise (R * tan(sigma),
//      15-60 m). One Record per window: parking adds nothing, a slow crawl
//      adds little, and the records are close to independent.
//   3. Grid: every record adds alpha * log((1 - eps) * p(bearing to cell) *
//      BINS + eps) to every cell (log-likelihood ratio against "no
//      information"; eps = outlier / multipath floor), bearing = exact great
//      circle initial bearing, lobe interpolated between bins. Cells near the
//      record get 0 (the bearing of a cell around the receiver is undefined).
//      Kept for K array/heading offsets delta (a systematic rotation of all
//      bearings - array mounting, compass error) and marginalised with a
//      Gaussian prior over delta, which also measures the offset.
//   4. Estimate: posterior = exp(s * (M - Mmax)), M = the marginal, s =
//      min(1, ess_cap / (alpha * records)) caps the certainty at ess_cap
//      independent bearings (systematic errors don't average out). Location =
//      posterior mean of the main mode (8-connected region around the peak),
//      ellipse from its covariance, 50 % / 95 % HPD radii, edge warning.
//   Solver: a coarse grid (fixed origin, re-centred only when the vehicle
//   leaves it) + a fine grid around a converging estimate, rebuilt from all
//   records when the estimate moves / shrinks.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rdf {

constexpr int BINS = 720;                  // north-frame lobe: 0.5 deg bins, compass bearing (CW from true north)
constexpr double BIN_DEG = 360.0 / BINS;

using Lobe = std::array<float, BINS>;      // probability per bin, sums to 1

struct Options {
    double eps = 0.05;                     // uniform mixture: one record's veto is capped (multipath, bad frames)
    double alpha = 0.5;                    // tempering per record (residual correlation between records)
    double ess_cap = 40;                   // certainty cap, in independent-record equivalents
    std::vector<double> offsets_deg{-8, -6, -4, -2, 0, 2, 4, 6, 8};
    double offset_prior_sd_deg = 4;
    bool marginalize_offsets = true;       // false: the best offset only (profile likelihood)
    bool interpolate = true;               // false: nearest lobe bin
    double near_m = 30;                    // cells closer than max(near_m, 1.5 cells) get no information
    double near_value = 0;                 // their log-likelihood ratio
};

// MUSIC spectrum (linear, >= 0; index i = i * res_deg unit-circle CCW from
// the array's forward axis) + heading (compass, deg) -> north-frame lobe,
// blurred by sigma_deg. false if the spectrum is empty / flat / not finite.
bool north_lobe(const float* spec, int n, double res_deg, double heading_deg, double sigma_deg, Lobe* out);

// The peak of a lobe (deg, parabolic between bins)
double lobe_peak_deg(const Lobe& p);

// One distance-gated window of frames
struct Record {
    int64_t t_ms = 0;
    double lat = 0, lon = 0;
    float conf = 0;
    int frames = 0;
    Lobe p{};
};

struct GateCfg {
    double gate_sigma_deg = 3;             // gate = R * tan(this)
    double min_gate_m = 15, max_gate_m = 60;
    double default_range_m = 1000;         // R before an estimate exists
    double max_gap_s = 10;                 // a pause longer than this restarts the window
    double max_window_s = 90;              // crawling: a window not closed after this is dropped
    int min_frames = 2;
};

class Gate {
public:
    explicit Gate(GateCfg c = {}) : cfg_(c) {}
    // a frame at (lat, lon, t); est_range_m <= 0 = unknown. True + *out when a window closed
    bool add(int64_t t_ms, double lat, double lon, const Lobe& p, float conf, double est_range_m, Record* out);
    void reset();
    int frames() const { return n_; }
    double gate_m() const { return gate_m_; }
    double moved_m() const { return moved_m_; }

private:
    GateCfg cfg_;
    int n_ = 0;
    double lat0_ = 0, lon0_ = 0, slat_ = 0, slon_ = 0, sconf_ = 0;
    int64_t t0_ = 0, tlast_ = 0, tsum_ = 0;
    std::array<double, BINS> sum_{};
    double gate_m_ = 0, moved_m_ = 0;
};

struct Estimate {
    bool valid = false;
    double lat = 0, lon = 0;               // the estimate: most likely point (MAP, sub-cell)
    double peak_lat = 0, peak_lon = 0;     // = lat / lon
    double mean_lat = 0, mean_lon = 0;     // posterior mean of the main mode
    double r50_m = 0, r95_m = 0;           // radius of a circle of the 50 / 95 % HPD region's area
    double ell_a_m = 0, ell_b_m = 0, ell_ang_deg = 0;   // 1-sigma ellipse (major, minor, major axis bearing)
    double mode_mass = 0;                  // posterior mass in the main mode (0..1)
    bool at_edge = false;                  // the mode touches the grid edge: transmitter may be outside
    double offset_deg = 0, offset_sd_deg = 0;   // systematic bearing offset (measured = true - offset)
    int records = 0;
};

// Local metric frame (azimuthal equidistant around an origin)
void to_enu(double lat0, double lon0, double lat, double lon, double* e, double* n);
void from_enu(double lat0, double lon0, double e, double n, double* lat, double* lon);
double distance_m(double lat1, double lon1, double lat2, double lon2);
double bearing_deg(double lat1, double lon1, double lat2, double lon2);

class Grid {
public:
    Grid(const Options& o, double lat0, double lon0, double half_m, int n);
    void add(const Record& r);
    void clear();
    Estimate estimate() const;
    // u8 per cell, row 0 = north: 255 * (1 + s * (M - Mmax) / nats_range), clamped
    std::vector<uint8_t> raster(double nats_range) const;
    double lat0() const { return lat0_; }
    double lon0() const { return lon0_; }
    double half_m() const { return half_; }
    double cell_m() const { return cell_; }
    int n() const { return n_; }
    int records() const { return records_; }

private:
    void marginal(std::vector<float>* m) const;
    Options o_;
    double lat0_, lon0_, half_, cell_;
    int n_, k_;
    int records_ = 0;
    std::vector<float> L_;                 // k_ planes of n_ * n_
    std::vector<float> slat_, clat_, slon_, clon_, e_, nn_;
    std::vector<double> lprior_;
};

// Coarse + adaptive fine grid over a growing set of records
class Solver {
public:
    explicit Solver(Options o = {}, double half_km = 10, int n = 256);
    void add(const Record& r);
    void set_range_km(double half_km);
    void recentre(double lat, double lon);
    void reset();
    const Estimate& estimate() const { return est_; }
    const Grid* coarse() const { return coarse_.get(); }
    const Grid* fine() const { return fine_.get(); }
    const std::vector<Record>& records() const { return recs_; }
    double range_km() const { return half_km_; }
    uint64_t version() const { return version_; }
    void load(std::vector<Record> recs);   // replaces the records, rebuilds

private:
    void rebuild_coarse(double lat, double lon);
    void update_fine(bool force);
    void refresh_estimate();
    Options o_;
    double half_km_;
    int n_;
    std::vector<Record> recs_;
    std::unique_ptr<Grid> coarse_, fine_;
    Estimate est_;
    uint64_t version_ = 0;
};

}  // namespace rdf
