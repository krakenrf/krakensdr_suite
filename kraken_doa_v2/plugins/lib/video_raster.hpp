#pragma once

// Video-raster reconstruction backbone, shared by the analog-TV and the
// screen-leakage (TEMPEST) decoders. Both rebuild a near-static picture from
// an AM-demodulated signal: envelope -> estimate line / frame timing ->
// resample the 1-D envelope into a 2-D raster -> average many frames (the
// picture barely moves, so averaging lifts it out of the noise) ->
// percentile contrast-stretch to 8-bit for display. This module is the
// timing / resampling / averaging core; each plugin adds its own front end
// (analog TV uses the broadcast sync pulses; TEMPEST has none and recovers
// timing from autocorrelation + a mode table).
//
// Everything is clean-room from first principles and published facts: ITU-R
// BT.470 / BT.1700 for the TV line / frame rates, VESA DMT for the PC mode
// table and VESA CVT 1.2 for the generator, textbook DSP for the envelope,
// autocorrelation (Wiener-Khinchin) and interpolation.
//
// Conventions. All images are contiguous float, ROW-MAJOR, top-left pixel
// first (index = row * width + col). "samples" are input samples at the
// envelope's own sample rate; periods are returned as fractional samples.
// Rates (line_rate_hz, frame_rate_hz, pixel_clock_hz) are in Hz. The hot
// loops (envelope, resample) are allocation-free and write into a buffer the
// caller owns; the analysis calls (period finder, sync) allocate working
// state once and reuse it.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace video {

// === 1. AM envelope ========================================================
// |x| plus optional slow DC-block and slow AGC so a drifting signal level
// doesn't wreck the picture's contrast. The primary intensity for AM video
// reception is the magnitude; the DC-block removes a slowly varying offset
// (one-pole high-pass on the magnitude) and the AGC divides by a slowly
// tracked scale so the frame-to-frame contrast stays put. Both adapt far
// slower than one line, so they leave the picture itself alone.
class Envelope {
public:
    struct Config {
        bool dc_block = false;     // subtract a slowly tracked mean (removes level drift)
        float dc_rate = 2e-4f;     // mean tracker rate per sample (smaller = slower)
        bool agc = false;          // divide by a slowly tracked scale (mean |.|)
        float agc_rate = 1e-4f;    // scale tracker rate per sample
    };
    Envelope() = default;                          // all trackers off (plain |x|)
    explicit Envelope(const Config& c) : cfg_(c) {}
    // Write the real intensity of x[0..n) into out[0..n) (out != x's storage).
    // Allocation-free. out is |x| after the configured DC-block / AGC.
    void process(const std::complex<float>* x, size_t n, float* out);
    void reset() { mean_ = 0; scale_ = 0; primed_ = false; }

private:
    Config cfg_;
    float mean_ = 0;      // tracked DC
    float scale_ = 0;     // tracked AGC scale
    bool primed_ = false; // trackers seeded from the first sample
};

// === 2. Autocorrelation period finder ======================================
// Dominant period of a real buffer within [min_lag, max_lag] samples, by
// NORMALIZED autocorrelation computed with an FFT (Wiener-Khinchin: FFT ->
// |.|^2 -> inverse FFT, unbiased by the overlap count, divided by lag-0). The
// peak is refined to a fractional lag by a parabola through the integer peak
// and its two neighbours; confidence is the peak's prominence above the
// trough that precedes it (0 = none .. 1 = a clean periodic signal). Used for
// the coarse frame period and the fine line period. Holds cached pffft plans
// (one per transform size) and reusable work buffers, so repeated calls at
// the same size allocate nothing.
struct PeriodResult {
    bool ok = false;
    double period = 0;      // fractional samples (0 if !ok)
    double confidence = 0;  // peak prominence, 0..1
    double peak = 0;        // normalized autocorrelation at the period, -1..1
};
class PeriodFinder {
public:
    PeriodFinder();
    ~PeriodFinder();
    PeriodFinder(const PeriodFinder&) = delete;
    PeriodFinder& operator=(const PeriodFinder&) = delete;
    // x[0..n): the real signal (e.g. the envelope). min_lag >= 1,
    // max_lag < n. Returns the best period in that lag window.
    PeriodResult find(const float* x, size_t n, int min_lag, int max_lag);
    // The normalized autocorrelation computed by the last find(), indexed by
    // integer lag 0..max_lag (acf[0] == 1). Lets a plugin refine the estimate
    // itself. Empty until find() has run.
    const std::vector<float>& autocorr() const { return acf_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<float> acf_;   // normalized, lag 0..max_lag
};

// === 3. Display-mode tables & matching =====================================

// --- Analog TV (ITU-R BT.470 / BT.1700) ---
// The geometry a reconstructor needs. line_rate_hz = total_lines * frame_rate.
struct TvStandard {
    const char* name;       // "PAL", "NTSC", "SECAM"
    const char* system;     // "625/50", "525/60"
    int total_lines;        // lines per FRAME, blanking included (625 / 525)
    int active_lines;       // visible lines per frame (~576 / ~480)
    double frame_rate_hz;   // frames per second (25 / 30000/1001)
    double field_rate_hz;   // fields per second (50 / 60000/1001)
    double line_rate_hz;    // horizontal lines per second (15625 / 15734.27)
    bool interlaced;        // true for all three
    int fields;             // fields per frame (2)
};
// The table and its length (PAL, NTSC, SECAM).
const TvStandard* tv_standards(size_t* count);
// Closest standard to measured rates (either argument may be NAN / <=0 =
// unknown; at least one must be given). nullptr if nothing is close enough.
const TvStandard* match_tv_standard(double frame_rate_hz, double line_rate_hz);

// --- PC display modes (VESA DMT) ---
// Timing of one progressive display mode. The published pixel clock and the
// blanking-included totals; the ACTUAL refresh is pixel_clock_hz /
// (htotal * vtotal), a hair off the nominal label (e.g. 59.94 for "60").
struct DisplayMode {
    int width = 0, height = 0;    // active pixels
    double refresh_hz = 0;        // nominal label (60 / 72 / 75)
    int htotal = 0, vtotal = 0;   // total pixels / lines, blanking included
    double pixel_clock_hz = 0;    // published dot clock
    const char* name = "";        // "1024x768@60" ("" for a CVT-generated mode)
    // actual line rate (Hz) = pixel_clock / htotal
    double line_rate_hz() const { return htotal ? pixel_clock_hz / htotal : 0; }
    // actual refresh (Hz) = pixel_clock / (htotal * vtotal)
    double actual_refresh_hz() const {
        const double t = static_cast<double>(htotal) * vtotal;
        return t > 0 ? pixel_clock_hz / t : 0;
    }
};
// The DMT table (the common 4:3 / 16:9 / 16:10 / 5:4 modes) and its length.
const DisplayMode* display_modes(size_t* count);

// CVT (VESA Coordinated Video Timings 1.2) generator for a mode not in the
// table: given active (w, h) and a refresh rate, it returns approximate
// htotal / vtotal / pixel_clock_hz. reduced = CVT reduced-blanking v1 (what
// modern flat panels use); false = the standard-blanking formula. name is "".
DisplayMode cvt_mode(int w, int h, double refresh_hz, bool reduced = false);

// What a reconstructor has measured; any field left NAN / <=0 is not used.
// For TEMPEST the two solid observables from autocorrelation are frame_rate
// (fs / frame_period) and total_lines (frame_period / line_period); the
// others sharpen the match when known (pixel_clock needs the sample rate).
struct ModeQuery {
    double frame_rate_hz = -1;   // = the display's refresh for a PC mode
    double total_lines = -1;     // vtotal: blanking-included lines per frame (fractional ok)
    double line_rate_hz = -1;    // horizontal line rate
    double pixel_clock_hz = -1;  // dot clock
};
struct ModeMatch {
    DisplayMode mode;
    double error = 0;            // summed relative error over the given fields (lower = better)
};
// Table modes ranked by closeness to the query (best first), at most
// max_results. CVT is NOT consulted here - call cvt_mode() when the best
// error is still too large for the resolution you expect.
std::vector<ModeMatch> match_display_modes(const ModeQuery& q, int max_results = 5);

// === 4. Raster resampler ===================================================
// Resample the 1-D envelope into a width x height float image by linear
// interpolation. The source sample for output pixel (row r, col c) is
//     pos(r,c) = origin + h_offset + (v_offset + r) * samples_per_line
//                + line_drift * (r*(r-1)/2) + c * samples_per_pixel
// so samples_per_line is the source stride between consecutive output rows
// (fractional), samples_per_pixel the stride between columns (0 => a whole
// line is spread across width, i.e. samples_per_line / width). line_drift is
// a per-row change in the line length that cancels a slow timing slope
// (shear); h_offset is a horizontal sub-sample shift, v_offset a vertical
// shift in lines. Source positions outside [0, n) read as 0 (black).
// Documented: linear (2-tap) interpolation only - enough for a near-static,
// frame-averaged picture; no windowed-sinc.
struct RasterMap {
    int width = 0, height = 0;
    double samples_per_line = 0;   // source samples per output row (fractional, > 0)
    double samples_per_pixel = 0;  // source samples per output column (0 => samples_per_line / width)
    double h_offset = 0;           // horizontal sub-sample offset (samples)
    double v_offset = 0;           // vertical offset (lines)
    double line_drift = 0;         // change in samples_per_line per row (shear compensation)
    double origin = 0;             // source sample mapped to (row 0, col 0) before the offsets
};
// Fill out[0 .. width*height) (caller-owned, row-major). Allocation-free.
void resample(const float* env, size_t n, const RasterMap& m, float* out);

// === 5. Frame accumulator & 8-bit render ===================================
// A running average of the reconstructed float frames (the picture is static,
// so this is the main SNR win), rendered to GRAY8 with a percentile contrast
// stretch. configure() picks exponential averaging (ema in (0,1]: new frame
// weight; smaller = steadier) or a boxcar of the last N frames (boxcar_n > 0,
// which then wins). add() can first search a small +-max_shift pixel 2-D
// integer shift to re-align the incoming frame to the current average, which
// counters slow horizontal / vertical creep; the applied shift is reported.
struct StretchOpts {
    float lo_pct = 0.5f;    // percentile mapped to black (0)
    float hi_pct = 99.5f;   // percentile mapped to white (255)
    bool invert = false;    // swap black / white (AM video is often inverted)
    float gamma = 1.0f;     // output = in^(1/gamma); 1 = linear, >1 lifts shadows
};
class FrameAccumulator {
public:
    // ema in (0,1] (ignored when boxcar_n > 0); boxcar_n > 0 keeps the last N
    // frames and averages them. Resets the average.
    void configure(int width, int height, float ema = 0.1f, int boxcar_n = 0);
    // Add one width*height float frame. If max_shift > 0, the frame is first
    // aligned to the current average by a coarse integer-shift search in
    // [-max_shift, max_shift] on both axes; the chosen shift (col, row) is
    // written to out_dx / out_dy when non-null. Before the first frame the
    // average is empty, so the frame is taken as-is.
    void add(const float* frame, int max_shift = 0, int* out_dx = nullptr, int* out_dy = nullptr);
    // The current averaged float frame (width*height, row-major), or nullptr
    // before any frame was added.
    const float* average() const { return frames_ ? avg_.data() : nullptr; }
    int width() const { return w_; }
    int height() const { return h_; }
    long long frames() const { return frames_; }
    void reset();
    // Render the current average to 8-bit GRAY8 (width*height bytes, row-major)
    // with the percentile stretch. Fills out (resized). No-op before the first
    // frame (out cleared).
    void render(std::vector<uint8_t>& out, const StretchOpts& o = StretchOpts{}) const;

private:
    int w_ = 0, h_ = 0;
    float ema_ = 0.1f;
    int boxcar_ = 0;        // 0 = EMA
    long long frames_ = 0;  // frames added
    std::vector<float> avg_;   // current average
    // boxcar state
    std::vector<float> ring_;  // boxcar_ * w_*h_ frames, round-robin
    std::vector<float> sum_;   // running sum of the ring
    int ring_pos_ = 0;
    int ring_filled_ = 0;
    // scratch for the shifted frame (so add() allocates nothing after warm-up)
    std::vector<float> shifted_;
    // best integer (dx,dy) aligning frame to avg_ (coarse, zero-mean correlation)
    void best_shift(const float* frame, int max_shift, int* dx, int* dy) const;
    void push(const float* frame);
};

// === 6. Sync helpers =======================================================

// --- Broadcast H/V sync (analog TV) ---
// Recover line and field timing from the sync pulses of a composite video
// envelope. The intensity must be presented so SYNC is the EXTREME end of the
// range: pass sync_is_min = true when sync tips are the MINIMUM (the natural
// video-voltage convention: sync below blanking below black below white), or
// false when they are the maximum (e.g. a raw AM-negative-modulation envelope,
// where the sync carrier is strongest) - internally the signal is flipped so
// sync is always the minimum.
//
// Method (documented as the robust, non-serration variant the SDK allows):
// line start = the periodic sync-tip minimum tracked once per line with a
// one-line search window (so it follows drift), refined to a sub-sample
// position by a parabola; field start = the run where the signal stays at
// sync level far longer than one hsync (the broad vertical-sync interval).
// Field parity (for interlace) is a best effort from whether that run begins
// on a line boundary or half a line into one; it may be wrong on noisy input.
struct BroadcastSync {
    bool ok = false;
    double samples_per_line = 0;          // refined mean line period (samples)
    double sync_level = 0, blank_level = 0;  // measured levels (in the oriented signal)
    std::vector<double> line_starts;      // sub-sample position of each line's sync tip
    std::vector<double> field_starts;     // sample position of each field's vertical sync
    std::vector<int> field_parity;        // 0/1 per field (best effort), size == field_starts
};
// env[0..n): the oriented intensity. approx_spl: a starting guess at the
// samples-per-line (from the line rate and the sample rate). A guess within
// ~10% is enough; the tracker locks from there.
BroadcastSync find_broadcast_sync(const float* env, size_t n, double approx_spl, bool sync_is_min = true);

// --- Blanking-offset sync (TEMPEST, no pulses) ---
// For a reconstructed raster with no sync pulses: find the pixel shift that
// parks the dark blanking band at the frame edge, by locating the column band
// and row band of minimum summed intensity. Returns the shift (in pixels) to
// apply so the blanking sits at the left / top edge - equivalently, add it to
// the resampler's h_offset (times samples_per_pixel) and v_offset. band_w /
// band_h are the blanking widths to look for (pixels); <=0 picks a default of
// ~1/8 of the dimension. The image must be the FULL raster (htotal x vtotal),
// not just the active area, for a blanking band to exist.
struct BlankOffset {
    int h_offset = 0;   // columns to roll left so blanking is at the left edge
    int v_offset = 0;   // rows to roll up so blanking is at the top edge
    bool ok = false;
};
BlankOffset find_blanking_offset(const float* img, int width, int height, int band_w = 0, int band_h = 0);

// === optional self-test ====================================================
// Synthesizes a known raster, AM-carries it, then runs envelope -> period
// finder -> resample -> accumulate and checks the recovered line period and
// the reconstruction correlation. true = pass. If log != nullptr it gets a
// one-line summary of the numbers. For a quick smoke test; the full unit
// check lives outside the repo.
bool self_test(std::string* log = nullptr);

}  // namespace video
