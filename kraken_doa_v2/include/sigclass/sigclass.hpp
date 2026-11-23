#pragma once

// Signal classifier core (sigclass): finds every signal in one snapshot of
// the receiver's band and labels it with a small per-view MLP whose classes
// come from the model file. Self-contained (C++20 standard library + pffft),
// used identically offline (tools/sigclass_tool.cpp, the training pipeline)
// and at runtime, so training and runtime features are the same by
// construction. No protocol knowledge here: class ids / names live in the
// model's metadata.
//
// Pipeline per snapshot (interleaved uint8 I/Q, (u - 127.5) / 127.5, the
// snapshot mean removed):
//   A. detector - a fine PSD (20 Hz bins, from the channelizer's own FFTs)
//      against a frequency-dependent noise floor, a short spectrogram for
//      duty / max-hold / bursts, a band-filling test -> Detections
//   B. views - each detection cut to a complex baseband view at fs / 100,
//      fs / 25, fs / 5 or fs (24 k / 96 k / 480 k / 2.4 MS/s at fs = 2.4 MHz),
//      the smallest whose rate is >= bw / view_occupancy; an overlap-save
//      frequency-domain channelizer (one set of FFTs per snapshot, an
//      inverse FFT per detection and block)
//   C. features - one fixed layout of feature_dim() floats for all views
//      (layout_json() documents the groups)
//   D. model - one MLP per view (Model)
//   E. tracker - associates detections across snapshots (Tracker)
//
// One Analyzer is used from one thread at a time. A loaded Model is
// immutable and may be shared between threads and Analyzers.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sigclass {

constexpr double NOMINAL_FS_HZ = 2.4e6;
constexpr double NOMINAL_SNAPSHOT_S = 0.5;
constexpr size_t NOMINAL_SNAPSHOT_SAMPLES = 1200000;   // complex samples
// shorter snapshots give no detections (the spectrogram needs some frames)
constexpr size_t MIN_SNAPSHOT_SAMPLES = 8192;

constexpr int NUM_VIEWS = 4;
// view v runs at fs / VIEW_DECIM[v]
constexpr int VIEW_DECIM[NUM_VIEWS] = {100, 25, 5, 1};

// The feature layout: bump FEATURE_LAYOUT_VERSION whenever a feature's
// meaning changes (the hash also covers the group table and parameters);
// a model trained on another layout refuses to load.
constexpr int FEATURE_LAYOUT_VERSION = 3;
int feature_dim();
std::string layout_json();
uint64_t layout_hash();

struct Config {
    int max_candidates = 32;        // strongest first (by peak_db)
    float detect_z = 6.0f;          // continuous detection: sigmas over the floor
    float burst_fa = 0.01f;         // burst detection: false alarms per scale and snapshot
    double edge_guard = 0.03;       // fraction of fs ignored at each band edge
    double dc_guard_hz = 400.0;     // +- this around the centre is not a signal (DC spike)
    float view_occupancy = 0.45f;   // a view's rate is >= bw / view_occupancy
    bool band_fill_test = true;     // report a band-filling wideband signal
    float band_fill_corr = 0.012f;  // ... when its residual autocorrelation exceeds this
    float band_fill_prominence = 4.0f;  // ... as a peak this many times the lags around it
    int spectrogram_hop = 512;      // short spectrogram: 1024-point frames, this hop
    // a detection a little too wide for view 0 (at most this wide; 0 = off) is also
    // classified in view 0: a narrow channel measured wide (a shoulder, a neighbour, a
    // strong signal's skirt) whose class view 1's model does not have - see analyzer.cpp
    double second_view_max_hz = 40e3;
    float second_view_min_p = 0.5f;
    float second_view_skip_p = 0.99f;   // ... not when view 1 is already this sure of its answer
    // a detection at the mirror frequency of a much stronger signal is its IQ image when
    // the conjugate of that signal holds at least this share of its view's energy over the noise
    float image_share = 0.1f;
    // two detections that are the tones of one wide-shift FSK transmitter (on and off
    // together at burst level, complementary power at sample level) become one
    bool tone_merge = true;
    // a burst's keying sidelobes are part of it out to this many of its widths (the
    // 1 / (pi f Tp)^2 envelope test, detector.cpp; 0 = only the near splatter rule)
    float sidelobe_widths = 8.0f;
};

struct Detection {
    double offset_hz = 0.0;   // power centroid of the occupied band, from the capture centre
    double bw_hz = 0.0;       // occupied bandwidth (98 % of the power above the floor)
    float snr_db = 0.0f;      // in-band SNR over the floor (while on, for bursty signals)
    float peak_db = 0.0f;     // highest short-frame in-band SNR (max-hold)
    float duty = 0.0f;        // fraction of short frames (427 us) with the signal on
    int view = 0;             // 0..NUM_VIEWS-1
    bool bursty = false;      // duty < 0.5 or found only in max-hold
    bool fills_band = false;  // a wideband signal fills the whole band (no floor seen)
};

struct Scored {
    Detection det;
    std::vector<float> probs;   // one per class of det.view's model; empty without one
    int best = -1;              // index into probs
    float best_p = 0.0f;
};

struct ClassInfo {
    std::string id;     // e.g. a plugin id - opaque to the core
    std::string name;   // display name
};

// sigclass.model: see src/sigclass/model.cpp for the binary format,
// tools/sigclass/model_io.py writes it
class Model {
public:
    Model();
    ~Model();
    Model(const Model&);
    Model& operator=(const Model&);
    bool load(const std::string& path, std::string* err = nullptr);
    bool load_from_memory(const void* data, size_t size, std::string* err = nullptr);
    bool loaded() const;
    bool has_view(int view) const;
    int classes(int view) const;                       // 0 without a model for the view
    const ClassInfo& class_info(int view, int cls) const;
    const std::string& meta_json() const;              // the model's metadata, verbatim
    // features (feature_dim() floats) -> probabilities (classes(view)); false
    // without a model for the view. Thread-safe.
    bool predict(int view, const float* features, std::vector<float>& probs) const;
    // the RF plausibility mask (model format 3: per class where it can exist,
    // exclusive bands): probabilities of the classes a detection at [lo_hz,
    // hi_hz] (absolute RF) cannot be set to 0, the rest renormalised; all
    // masked -> the generic classes keep their share. false (probs untouched)
    // without a table, a band, or anything left.
    bool has_rf_table() const;
    bool rf_mask(int view, double lo_hz, double hi_hz, std::vector<float>& probs) const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

class Analyzer {
public:
    explicit Analyzer(const Config& cfg = {});
    ~Analyzer();
    Analyzer(const Analyzer&) = delete;
    Analyzer& operator=(const Analyzer&) = delete;

    // One snapshot of interleaved uint8 I/Q (n_samples complex samples).
    // features_out (optional): n_det x feature_dim() floats, row-major, in
    // the order of the result. Features are computed only with a model or a
    // features_out; without either only the detector runs.
    // centre_hz: the capture's RF centre (0 = unknown: no RF mask)
    std::vector<Scored> analyze(const uint8_t* iq, size_t n_samples, double fs_hz,
                                const Model* model, std::vector<float>* features_out = nullptr, double centre_hz = 0.0);
    // The same for complex float samples on the uint8 path's scale (full
    // scale = 1.0; the uint8 path is (u - 127.5) / 127.5)
    std::vector<Scored> analyze(const std::complex<float>* iq, size_t n_samples, double fs_hz,
                                const Model* model, std::vector<float>* features_out = nullptr, double centre_hz = 0.0);

    static int feature_dim() { return sigclass::feature_dim(); }
    static std::string layout_json() { return sigclass::layout_json(); }
    // the view for a bandwidth: the smallest whose rate is >= bw / occupancy
    static int view_for_bandwidth(double bw_hz, double fs_hz, float occupancy = 0.45f);

    // per stage of the last analyze(); frontend_ms includes spectrogram_ms
    struct Timing {
        double convert_ms = 0, frontend_ms = 0, spectrogram_ms = 0, detect_ms = 0,
               channelize_ms = 0, features_ms = 0, model_ms = 0, total_ms = 0;
        int second_view = 0, second_view_taken = 0;   // detections also classified in view 0 / that took its answer
    };
    const Timing& last_timing() const;
    const Config& config() const;

    // Tests / tools: the view of the LAST analyzed snapshot around offset_hz
    // (the result's channelizer; view 0..3). Empty without a snapshot.
    bool extract_view(double offset_hz, int view, std::vector<std::complex<float>>& out);
    // ... and the features of a detection on that snapshot (feature_dim() floats)
    bool features_for(const Detection& det, std::vector<float>& out);
    // the noise floor of the last snapshot: power per fine bin (fs / 120000 Hz),
    // index 0 = -fs/2 (for tests)
    const std::vector<float>& last_floor() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TrackerConfig {
    double expire_s = 10.0;      // a track not seen for this long is dropped
    double alpha = 0.1;          // weight of a new detection in the class evidence (the first 1 / alpha
                                 // looks are averaged, then an EWMA with this weight)
    double min_iou = 0.3;        // band overlap / union to associate
    double freq_tol_hz = 600.0;  // ... or centres this close (narrow signals)
    // one signal = one track: a track whose band lies mostly (this fraction of its
    // width) inside another's is merged into it (see tracker.cpp); 0 = never
    double merge_overlap = 0.5;
    // ... except a channel's core: a detection / track at least min_channel_hz wide (a
    // modulated channel, not a carrier line) is not a piece of a wider track that holds
    // at most core_energy_ratio times its energy (that wider track is its skirt, its
    // receiver's plateau or a key-up transient around it - not a signal of its own)
    double min_channel_hz = 2000.0;
    double core_energy_ratio = 1.5;
    // ... and a faint continuous pedestal (mean SNR under pedestal_db) is not the band of a
    // signal at least pedestal_contrast_db stronger inside it
    float pedestal_db = 3.0f;
    float pedestal_contrast_db = 10.0f;
    // a burst hump (bursty, >= 20x as wide and >= 200 kHz: an overloaded front end's
    // clipping splatter, a key-up click) neither takes a continuous channel inside it
    // nor counts it as its piece, whatever the hits (false: the rules before 2026-11-24)
    bool hump_guard = true;
};

struct Track {
    int id = 0;                  // stable, increasing
    double centre_hz = 0.0;      // absolute (capture centre + offset)
    double bw_hz = 0.0;          // = hi_hz - lo_hz
    double lo_hz = 0.0, hi_hz = 0.0;   // the band its detections cover (absolute): widens at once, narrows slowly
    double first_s = 0.0, last_s = 0.0;
    int hits = 0;
    int view = 0;                // the view most of its detections had
    float snr_db = 0.0f, duty = 0.0f;
    bool bursty = false;
    bool fills_band = false;     // its latest detection was a band-filling one
    bool updated = false;        // detected in the latest update()
    std::vector<float> probs;    // smoothed class probabilities (view's model); may be empty
    int best = -1;
    float best_p = 0.0f;
};

class Tracker {
public:
    explicit Tracker(const TrackerConfig& cfg = {});
    // results of one snapshot taken at t_s (seconds, any monotonic clock)
    // with the capture centred on centre_hz
    const std::vector<Track>& update(const std::vector<Scored>& results, double centre_hz, double t_s);
    // only the expiry (no snapshot at t_s): tracks unseen for expire_s go
    const std::vector<Track>& expire(double t_s);
    const std::vector<Track>& tracks() const { return tracks_; }
    void clear();
    // drop one track (by id; false: no such track) - the service's wall-clock ceiling
    bool erase(int id);

private:
    struct Evidence {
        std::vector<float> logp[NUM_VIEWS];
        int view_hits[NUM_VIEWS] = {0, 0, 0, 0};
        double lo = 0.0, hi = 0.0;   // the band its detections cover (grows at once, shrinks slowly)
        double energy = 0.0;         // its latest detection's power over the floor x width (x duty when bursty)
        double det_bw = 0.0;         // ... and that detection's width
        float look_snr = -100.0f;    // the strongest SNR of its detections (hits and pieces), - 1 dB a snapshot
    };
    TrackerConfig cfg_;
    std::vector<Track> tracks_;
    std::vector<Evidence> ev_;   // parallel to tracks_
    int next_id_ = 1;
};

}  // namespace sigclass
