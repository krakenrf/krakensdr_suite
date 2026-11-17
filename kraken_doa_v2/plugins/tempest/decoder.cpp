// TEMPEST screen reconstruction (Van Eck phreaking) - rebuilds the picture on
// a nearby monitor/TV from its UNINTENDED video emanations, for emissions-
// security (EMSEC) research. A digital display driven at pixel clock f_p
// radiates harmonics of f_p; the user tunes the KrakenSDR to one harmonic. The
// signal carries no sync pulses - after AM-magnitude detection the recovered
// intensity tracks the high-frequency/edge content of the raster being drawn.
//
// There is nothing to "demodulate": the whole job is to recover the display's
// TIMING with no sync to lock to, then average many aligned frames (the screen
// is near-static, which is exactly what makes the attack work) into a readable
// picture. Timing comes from two robust autocorrelation observables of the
// envelope - the FRAME period and the LINE period - and the identity
//     pixel_clock = htotal * vtotal * frame_rate
// so frame_rate = fs / frame_period and total_lines = frame_period / line_period.
// Snapping (frame_rate, total_lines) to a real VESA display mode fixes the
// active resolution, blanking and pixel clock. All of the timing / resampling /
// averaging DSP is the shared video-raster backbone (plugins/lib/video_raster).
//
// Secondary, and strictly defensive: in coherent mode with DoA on the plugin
// also gets every antenna (process_multi) and reports ONE talker packet with
// the leakage's own spatial covariance, so kraken_doa runs MUSIC (one source)
// and shows a bearing to the leaking device in the DF panel. A DoA failure
// must never break the picture.
//
// manual_only: TEMPEST must not run in Auto detect - it would false-trigger on
// anything. Clean-room from the physics and VESA timings; no GPL sources
// (gr-tempest / TempestSDR / Deep-TEMPEST) consulted. README.md has the
// method and limits; docs/dev/tempest-deep-plan.md the (unimplemented) neural
// reconstruction plan.

#include "kraken_plugin.hpp"

#include "video_raster.hpp"   // plugins/lib: the shared reconstruction backbone

#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using cf = kp::cf;
using cd = std::complex<double>;

// --- detection gates (tuned against plugins/tempest/tools + the offline test) ---
constexpr double FRAME_CONF_MIN = 0.12;   // frame autocorr prominence (0..1)
constexpr double FRAME_PEAK_MIN = 0.20;   // frame autocorr peak (normalized)
constexpr double LINE_PEAK_MIN = 0.12;    // line-comb autocorr peak
constexpr int MIN_VTOTAL = 400;           // plausible total lines (blanking included)
constexpr int MAX_VTOTAL = 2400;          // >= 4K's 2250 (3840x2160 CTA-861)
// Manual mode's line-period search (manual_line_period): the comb score a line
// count must reach, by how much it must beat the runner-up, and how many lines
// off the true frame the period finder's frame peak may be
constexpr double MAN_COMB_MIN = 0.02;
constexpr double MAN_COMB_RATIO = 1.3;
constexpr int MAN_ALIAS_LINES = 8;
// a known timing fits when the measured line period gives its refresh to this
// (about half a line per frame at 1000 lines; a display's clock is far closer)
constexpr double MAN_FIT_TOL_HZ = 0.03;
// Manual timing with nothing that gives the pixels per line (no resolution,
// no pixel clock): htotal ~ this x vtotal (DMT / CVT / CTA rasters run 1.5 - 2.1)
constexpr double MAN_HT_PER_VT = 1.8;
constexpr double LINE_RATE_MIN = 30000;   // horizontal line rate search (Hz): the line
constexpr double LINE_RATE_MAX = 140000;  // period lies in [fs/MAX, fs/MIN] (e.g. 17..80 @ 2.4 MHz)
constexpr double REFRESH_GATE = 0.08;     // a mode's refresh must be this close to the coarse frame rate
constexpr int LOSS_FAILS = 6;             // consecutive failed detects before dropping a lock

// --- cadence (seconds of input) ---
constexpr double DETECT_LOCKED_S = 1.0;   // re-estimate once a second when locked (FFT cost)
constexpr double DETECT_SEARCH_S = 0.30;  // more often while searching
constexpr double ACC_S = 0.05;            // add an aligned frame to the average this often (~20/s)
constexpr double VIDEO_S = 0.25;          // send a picture this often (a few/s)
constexpr double COV_WIN_S = 0.20;        // one DoA covariance packet per this window
constexpr double BLANK_S = 0.5;           // re-find the blanking offset this often

// Default longest-side cap for the EMITTED picture (option max_display_px).
// Display / transmission ONLY: mode detection uses the full htotal_ x vtotal_
// raster and the reconstruction every line and every envelope sample, so a 4K
// display is still fully received and reconstructed - send_video() scales only
// the OUTPUT frame down when it exceeds this. 1280 keeps the transmitted picture
// small and cheap to JPEG-encode; raise it (up to native 3840) for a sharper
// emitted picture, lower it for less bandwidth / CPU. On a build without libjpeg
// the host can only send RAW pixels under kp::wire::MAX_PAYLOAD (1 MiB), where a
// GRAY8 frame over ~1000x1000 is dropped - keep this modest on such a build.
constexpr int DEFAULT_MAX_DISPLAY_PX = 1280;

// The reconstruction's work raster has one row per display line but only this
// many columns per envelope sample across the line (work_width()): at 2.4 MHz
// a line holds ~18-60 samples whatever the display's width, so more columns
// only repeat the linear interpolation between them.
constexpr double RECON_OVERSAMPLE = 8.0;

// Above ~1440p-worth of work-raster pixels the accumulate and video cadences are
// stretched to keep the per-second pixel work bounded. With the sample-capped
// work raster (RECON_OVERSAMPLE) no table mode comes near it - kept as a guard.
constexpr double CADENCE_PX_REF = 2560.0 * 1440.0;

// Free-run preview (display only): while NOT locked, map the raw magnitude
// envelope onto the assumed geometry (the forced mode, else a default) and emit
// it, so the canvas always shows a live image (snow with no signal, a rolling
// picture with a weak one) like a TempestSDR while you tune. It never touches
// the detection, lock or DoA state - purely what gets drawn while searching.
// Capped to a small preview raster and a modest rate (noise frames never dedup,
// so a 4K mode must NOT make the preview huge; this bounds the bandwidth / CPU).
constexpr double FREE_RUN_S = 1.0 / 12.0;   // ~12 fps preview cadence
constexpr int FREE_RUN_MAX_PX = 720;        // longest-side cap for the preview raster
constexpr int FREE_RUN_QUALITY = 50;        // lower JPEG quality for the preview
constexpr const char* FREE_RUN_DEFAULT_MODE = "1024x768@60";   // assumed geometry when none is chosen

// Timing graphs beside the video display (Host::plot, PLOT_BESIDE_VIDEO): the
// frame-period autocorrelation against refresh rate, and how well each total
// line count explains the signal. A click on one sets the `fps` / `lines`
// option (and Manual mode). Display only, and only while a viewer watches the
// video (Host::video_wanted): they never change the detection or the lock.
constexpr double PLOT_S = 0.5;            // send both graphs this often
constexpr int PLOT_FPS_POINTS = 600;      // frame-rate graph resolution (<= 1000)
constexpr int PLOT_LINES_MAX = 1000;      // total-lines graph: at most this many counts (one point each)
// The total-lines graph folds a history of line energies (the envelope summed
// over each line at the line period): ROWS_N of them, autocorrelated up to
// ROWS_MAXLAG lines. A count V scores the mean of that autocorrelation at
// V, 2V, 3V ... - the picture's vertical profile (blanking band, horizontal
// edges) repeats every true frame and slips by (V - vtotal) lines a frame for
// any other count. Unlike the envelope's own autocorrelation at one frame
// lag, this doesn't depend on where the frame falls between two samples.
constexpr int ROWS_N = 16384;             // 0.12 s (4K) .. 0.55 s (640x480) of lines
constexpr int ROWS_MAXLAG = ROWS_N * 3 / 4;
constexpr double ROWS_RESET_REL = 2e-5;   // a line period this much off the history's restarts it
constexpr double LINE_EST_KEEP_S = 5.0;   // the line-period estimate is reused this long (same frame peak)
constexpr double LINE_EST_PEAK_MIN = 0.05; // no estimate below this frame peak (noise: ~0.01)
// ... nor below this lines_score (below Manual's MAN_COMB_MIN: the row
// history checks the period - a wrong one gives no confident count - and the
// frame peak stays on a whole-line tooth down to ~5 dB, scoring 0.009-0.015)
constexpr double LINE_EST_COMB_MIN = 0.008;
// ... nor below this fraction of the frame peak: a real line comb keeps in
// proportion to it (measured 0.048 - 0.29 over every test signal and SNR,
// lowest for the shortest lines, 24 samples), while 4K's ~18-sample lines
// alias - 80 samples, 4.5 lines, at 0.022 - and the row history would then
// count the frame in those (500) as consistently as in true lines
constexpr double LINE_EST_COMB_REL = 0.035;
constexpr double ROWS_HOLD_S = 2.0;       // without a line period this long, the row history stops

// The measured total lines (measure_lines): the count V in the plausible
// range whose row-history score (rows_score) is highest. It is trusted
// ("confident") when the peak is inside the range, scores at least
// MEAS_PEAK_MIN, beats both +-1 neighbours by MEAS_SIDE_MIN and every other
// local peak in the range by MEAS_OTHER_MIN, and it is ACCEPTED once
// MEAS_CONFIRM confident estimates in a row (each on at least half a fresh
// history) give the same count. The detection then uses the accepted count:
// Auto takes only a table mode with that count (and line period), else shows
// the display as not listed; Manual takes it for the total lines.
constexpr double MEAS_PEAK_MIN = 0.25;
constexpr double MEAS_SIDE_MIN = 0.010;
constexpr double MEAS_OTHER_MIN = 0.10;
constexpr double MEAS_PROM = 0.05;        // a dip this deep makes another local maximum a competing peak
constexpr int MEAS_CONFIRM = 2;
constexpr double MEAS_WAIT_S = 2.0;       // a first lock waits at most this long for the measurement
constexpr double MEAS_L_TOL = 0.005;      // line periods this close (relative) are the same line rate

double pnum(const std::string& s) {
    if (s.empty()) return NAN;
    char* e = nullptr;
    const double v = std::strtod(s.c_str(), &e);
    return (e == s.c_str() || !std::isfinite(v)) ? NAN : v;
}

const video::DisplayMode* mode_by_name(const std::string& name) {
    size_t n = 0;
    const video::DisplayMode* m = video::display_modes(&n);
    for (size_t i = 0; i < n; i++)
        if (name == m[i].name) return &m[i];
    return nullptr;
}

std::string f1(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}
std::string f2(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

class Tempest : public kp::Decoder {
public:
    explicit Tempest(kp::Host& h) : Decoder(h) {
        fs_ = host.sample_rate();
        if (!(fs_ > 0) || !std::isfinite(fs_)) fs_ = 2400000.0;   // native VFO rate; a floor for odd hosts
    }

    void process(const cf* x, size_t n) override {
        ensure_fs();
        cov_m_ = 0;   // single channel: no DoA this stretch
        feed_envelope(x, n);
        tick(n);
    }

    void process_multi(const cf* const* x, int m, size_t n) override {
        ensure_fs();
        feed_envelope(x[0], n);          // reconstruct from antenna 0 (combining blind would cancel)
        accumulate_cov(x, m, n);         // secondary, defensive: the leakage's spatial covariance
        tick(n);
    }

    void reset() override {
        ebuf_.clear();
        buf_base_ = total_ = 0;
        env_ = video::Envelope{};
        drop_lock();
        acc_w_ = acc_h_ = 0;
        next_detect_ = next_acc_ = next_video_ = next_blank_ = next_free_ = next_plot_ = 0;
        acf_have_ = false;
        acf_frame_ = 0;
        est_frame_ = -1;
        rows_L_ = 0;
        rows_cnt_ = 0;
        rows_L_at_ = -1e9;
        ra_seq_ = -1;
        clear_meas();
        cov_m_ = 0;
        cov_n_ = 0;
        covR_.clear();
        cwin_start_ = NAN;
        clear_facts();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "mode") {
            opt_mode_ = value.empty() ? "auto" : value;
            drop_lock();
        } else if (key == "refresh_lo") {
            const double v = pnum(value);
            refresh_lo_ = std::isfinite(v) && v > 0 ? v : 50.0;
            drop_lock();
        } else if (key == "refresh_hi") {
            const double v = pnum(value);
            refresh_hi_ = std::isfinite(v) && v > 0 ? v : 90.0;
            drop_lock();
        } else if (key == "fps") {
            man_fps_ = pnum(value);
            drop_lock();
        } else if (key == "lines") {
            man_lines_ = pnum(value);
            drop_lock();
        } else if (key == "pixel_clock_mhz") {
            man_pclk_mhz_ = pnum(value);
            drop_lock();
        } else if (key == "samples_per_line") {
            man_spl_ = pnum(value);
            drop_lock();
        } else if (key == "width") {
            man_w_ = pnum(value);
            drop_lock();
        } else if (key == "height") {
            man_h_ = pnum(value);
            drop_lock();
        } else if (key == "h_offset") {
            const double v = pnum(value);
            man_hoff_ = std::isfinite(v) ? static_cast<int>(std::lround(v)) : 0;
        } else if (key == "v_offset") {
            const double v = pnum(value);
            man_voff_ = std::isfinite(v) ? static_cast<int>(std::lround(v)) : 0;
        } else if (key == "invert") {
            invert_ = value == "1";
        } else if (key == "gamma") {
            const double v = pnum(value);
            gamma_ = std::isfinite(v) && v > 0 ? v : 1.0;
        } else if (key == "contrast") {
            if (value == "high") { lo_pct_ = 2.0f; hi_pct_ = 98.0f; }
            else if (value == "low") { lo_pct_ = 0.1f; hi_pct_ = 99.9f; }
            else { lo_pct_ = 0.5f; hi_pct_ = 99.5f; }
        } else if (key == "average") {
            const double v = pnum(value);
            ema_ = std::isfinite(v) && v > 0 && v <= 1 ? v : 0.1;
            acc_w_ = acc_h_ = 0;   // force reconfigure
        } else if (key == "autolock") {
            autolock_ = value == "1";
        } else if (key == "crop") {
            const bool c = value != "0";
            if (c != crop_) { crop_ = c; acc_w_ = acc_h_ = 0; }
        } else if (key == "quality") {
            const double v = pnum(value);
            quality_ = std::isfinite(v) ? std::clamp(static_cast<int>(std::lround(v)), 1, 100) : 60;
        } else if (key == "lossless") {
            lossless_ = value == "1";
        } else if (key == "max_display_px") {
            const double v = pnum(value);
            max_display_px_ = std::isfinite(v) && v > 0
                                  ? std::clamp(static_cast<int>(std::lround(v)), 64, 4096)
                                  : DEFAULT_MAX_DISPLAY_PX;
        } else if (key == "doa") {
            doa_ = value != "0";
        } else if (key == "free_run") {
            free_run_ = value != "0";   // display-only; does not affect detection
        }
    }

private:
    double fs_ = 2400000.0;
    video::Envelope env_;
    std::vector<float> ebuf_;   // recent envelope, linear; ebuf_[0] is global index buf_base_
    std::vector<float> etmp_;   // per-block scratch
    int64_t buf_base_ = 0;      // global sample index of ebuf_[0]
    int64_t total_ = 0;         // envelope samples ever appended

    video::PeriodFinder pf_frame_;
    video::FrameAccumulator acc_;
    std::vector<float> rbuf_;   // resample scratch (one work-raster frame)
    std::vector<uint8_t> gray_, out8_;
    std::vector<double> bcol_, brow_;   // blanking search: the full raster's column / row sums
    std::vector<int> col_j_;            // send_video: work column left of each output column
    std::vector<float> col_f_;          // ... and the interpolation weight

    // lock state
    bool locked_ = false;
    bool announced_ = false;
    bool matched_ = false;      // a DMT mode matched (vs an estimate)
    int fails_ = 0;
    double frame_period_ = 0, line_period_ = 0, frame_rate_ = 0, match_err_ = 0;
    int htotal_ = 0, vtotal_ = 0, width_ = 0, height_ = 0;
    double pixel_clock_ = 0;
    std::string mode_label_;
    double nominal_refresh_ = 0;   // the matched mode's label refresh (for the event / facts)
    // global sample of a frame boundary (the true frame's first active pixel),
    // kept near "now": add_frame rolls it forward and apply_geom re-anchors it
    // before the frame period changes, so a period update moves the next frame
    // start by ~one frame's worth of the change, not (frames since lock) x it
    double anchor_ = 0;
    bool have_phase_ = false;
    int acc_w_ = 0, acc_h_ = 0;   // the work raster (acc_)
    int disp_w_ = 0;              // the display width it stands for (acc_h_ rows either way)

    // cadence markers (host.time())
    double next_detect_ = 0, next_acc_ = 0, next_video_ = 0, next_blank_ = 0;

    // DoA covariance
    int cov_m_ = 0;
    std::vector<cd> covR_;
    uint64_t cov_n_ = 0;
    double cwin_start_ = NAN;

    // options
    std::string opt_mode_ = "auto";
    double refresh_lo_ = 50, refresh_hi_ = 90;
    double man_fps_ = NAN, man_lines_ = NAN, man_pclk_mhz_ = NAN, man_spl_ = NAN;
    double man_w_ = NAN, man_h_ = NAN;   // manual active width / height (pixels); both needed
    int man_hoff_ = 0, man_voff_ = 0;
    bool invert_ = false;
    double gamma_ = 1.0;
    float lo_pct_ = 0.5f, hi_pct_ = 99.5f;
    double ema_ = 0.1;
    bool autolock_ = false;
    bool crop_ = true;
    int quality_ = 60;          // JPEG quality for the host (set by the slider)
    bool lossless_ = false;     // max-quality JPEG (a frozen / averaged still)
    int max_display_px_ = DEFAULT_MAX_DISPLAY_PX;   // longest-side cap on the emitted picture
    bool doa_ = true;
    bool free_run_ = true;      // show a live preview while unlocked (display only)

    // free-run preview state (display only; never touches the lock / DoA state)
    double next_free_ = 0;
    std::vector<float> free_f_;      // free-run resample scratch (reused)
    std::vector<uint8_t> free_pix_;  // free-run GRAY8 output

    // timing graphs (display only; see PLOT_S)
    double next_plot_ = 0;
    bool acf_have_ = false;          // pf_frame_.autocorr() holds the last search ...
    int acf_flo_ = 0, acf_fhi_ = 0;  // ... over this lag window
    double acf_frame_ = 0;           // ... and its frame peak (fractional lag; 0 = none)
    double acf_peak_ = 0;            // ... and that peak's height
    double est_frame_ = -1, est_line_ = 0, est_at_ = -1e9;   // line-period estimate from the signal (cached) ...
    double est_score_ = 0;           // ... and its lines_score
    double rows_L_ = 0;              // line period the row history is summed at (0 = off)
    double row_next_ = 0;            // global sample position of the next row boundary
    double row_acc_ = 0;             // the row being summed
    std::vector<float> rows_;        // ring of ROWS_N line energies
    size_t rows_head_ = 0, rows_cnt_ = 0;
    std::vector<float> rows_lin_;    // the ring, oldest first
    video::PeriodFinder pf_rows_;    // its autocorrelation (one fixed-length plan)
    std::vector<double> msc_;        // measure_lines: the counts' scores
    int64_t rows_seq_ = 0;          // rows completed since the history (re)started
    int64_t ra_seq_ = -1;            // rows_seq_ when pf_rows_ last ran (-1: stale)
    double rows_L_at_ = -1e9;        // when a line period last held the history up

    // the measured total lines (measure_lines)
    struct LineCount {
        int v = 0;                   // the best count in range (0: none)
        double L = 0;                // the line period the rows were summed at
        double score = 0, side = 0, other = 0;   // its score, the higher +-1 neighbour, the best other peak
        bool confident = false;
        bool hyp = false;            // summed at a hypothesis' period, not the measured one (count +-1)
    };
    LineCount meas_now_;             // the latest estimate
    int meas_v_ = 0;                 // the accepted count (0: none) ...
    double meas_L_ = 0;              // ... and the line period it was measured at
    bool meas_hyp_ = false;          // ... which was a hypothesis' (LineCount::hyp)
    bool rows_hyp_ = false;          // the row history is summed at a hypothesis' period
    int cand_v_ = 0, cand_n_ = 0;    // the run of confident estimates of one count
    int64_t cand_seq_ = -1;          // rows_seq_ of the last estimate counted in the run
    double search_t0_ = -1;          // when this search could first have locked (MEAS_WAIT_S)
    bool relearn_ = false;           // locked, and a line-rate change cleared the count (update_rows)
    bool unlisted_ = false;          // Auto: locked on a measured count no table mode has
    bool announced_unlisted_ = false;
    // Auto's table candidates of this search (auto_candidates), best comb first
    struct AutoCand {
        const video::DisplayMode* m;
        double comb, spl;            // its line-comb score and refined samples per line
    };
    std::vector<AutoCand> cands_;
    double man_spl0_ = 0;            // Manual: the line period measured by this search (manual_spl0)

    void ensure_fs() {
        if (!(fs_ > 0) || !std::isfinite(fs_)) {
            const double r = host.sample_rate();
            if (std::isfinite(r) && r > 0) fs_ = r;
        }
    }

    double now_end(size_t n) const { return host.time() + static_cast<double>(n) / fs_; }

    void feed_envelope(const cf* x, size_t n) {
        if (!x || n == 0) return;
        etmp_.resize(n);
        env_.process(x, n, etmp_.data());
        ebuf_.insert(ebuf_.end(), etmp_.begin(), etmp_.end());
        if (rows_L_ > 0) feed_rows(etmp_.data(), n, total_);   // the total-lines graph's history
        total_ += static_cast<int64_t>(n);
        const size_t cap = buf_cap();
        if (ebuf_.size() > cap + 65536) {   // compact rarely to amortise the erase
            const size_t drop = ebuf_.size() - cap;
            ebuf_.erase(ebuf_.begin(), ebuf_.begin() + static_cast<long>(drop));
            buf_base_ += static_cast<int64_t>(drop);
        }
    }

    // the slowest frame period the current settings would search for
    double slowest_frame_period() const {
        const video::DisplayMode* dm = (opt_mode_ != "auto" && opt_mode_ != "manual") ? mode_by_name(opt_mode_) : nullptr;
        if (dm && dm->actual_refresh_hz() > 0) return fs_ / dm->actual_refresh_hz();
        if (opt_mode_ == "manual" && std::isfinite(man_fps_) && man_fps_ > 0) return fs_ / man_fps_;
        const double lo = std::min(refresh_lo_, refresh_hi_);
        return fs_ / std::max(1.0, lo);
    }

    size_t buf_cap() const {
        // >= 3 frames so the two-frames-back render origin stays inside the buffer
        const double want = 3.1 * slowest_frame_period();
        return static_cast<size_t>(std::clamp(want, 150000.0, 360000.0));
    }

    // An option change: the measured count is found again (from the history
    // that is there - the range it was found in may have changed). A lost
    // lock keeps it (keep_meas): the history goes on at the same line
    // period, a different display changes the line rate (update_rows clears
    // it then) or is confirmed as a new count, and a re-lock needn't wait.
    void drop_lock(bool keep_meas = false) {
        locked_ = false;
        announced_ = false;
        matched_ = false;
        unlisted_ = false;
        announced_unlisted_ = false;
        have_phase_ = false;
        fails_ = 0;
        search_t0_ = -1;
        relearn_ = false;
        if (!keep_meas) clear_meas();
    }

    void clear_meas() {
        meas_v_ = 0;
        meas_L_ = 0;
        meas_hyp_ = false;
        cand_v_ = cand_n_ = 0;
        cand_seq_ = -1;
        meas_now_ = LineCount{};
    }

    // -------- detection --------
    void tick(size_t n) {
        const double t = host.time();   // block's first-sample time
        if (!autolock_ || !locked_) {
            if (t >= next_detect_) {
                try_detect();
                next_detect_ = t + (locked_ ? DETECT_LOCKED_S : DETECT_SEARCH_S);
            }
        }
        // locked without an accepted count (a forced mode, Manual, a lock on
        // the comb alone): estimate whenever half the row history is fresh,
        // not only at the once-a-second re-detection, so a count measured at
        // the lock's line period (4K: the only one there is) comes in ~0.2 s
        if (locked_ && meas_v_ == 0 && rows_cnt_ == static_cast<size_t>(ROWS_N) &&
            (cand_seq_ < 0 || rows_seq_ - cand_seq_ >= ROWS_N / 2))
            measure_lines();
        if (locked_) {
            const double cad = cadence_factor();   // >1 for big (>1440p) rasters
            if (t >= next_blank_) { find_blanking(); next_blank_ = t + BLANK_S; }
            if (t >= next_acc_) { add_frame(); next_acc_ = t + ACC_S * cad; }
            if (t >= next_video_) { send_video(); next_video_ = t + VIDEO_S * cad; }
        } else if (free_run_ && t >= next_free_) {
            // live preview while searching (display only; never affects detection)
            send_free_run();
            next_free_ = t + FREE_RUN_S;
        }
        // the timing graphs beside the picture - only for a viewer (the row
        // history behind the total-lines graph runs regardless: the detection
        // uses the count it measures)
        if (host.video_wanted() && t >= next_plot_) {
            send_plots();
            next_plot_ = t + PLOT_S;
        }
        // DoA covariance window (end on the block boundary)
        if (doa_ && locked_ && cov_m_ >= 2 && std::isfinite(cwin_start_) && now_end(n) - cwin_start_ >= COV_WIN_S)
            report_cov(n);
    }

    // frame-period search window [min_lag, max_lag]
    void frame_window(int& lo, int& hi) const {
        const video::DisplayMode* dm = (opt_mode_ != "auto" && opt_mode_ != "manual") ? mode_by_name(opt_mode_) : nullptr;
        double centre = 0;
        if (dm && dm->actual_refresh_hz() > 0) centre = fs_ / dm->actual_refresh_hz();
        else if (opt_mode_ == "manual" && std::isfinite(man_fps_) && man_fps_ > 0) centre = fs_ / man_fps_;
        if (centre > 0) {   // a known refresh: a tight window around it
            lo = static_cast<int>(std::floor(centre * 0.96));
            hi = static_cast<int>(std::ceil(centre * 1.04));
        } else {
            const double a = std::min(refresh_lo_, refresh_hi_), b = std::max(refresh_lo_, refresh_hi_);
            lo = static_cast<int>(std::floor(fs_ / b));
            hi = static_cast<int>(std::ceil(fs_ / a));
        }
    }

    // parabolic sub-sample peak around integer lag k in acf
    static double refine(const std::vector<float>& a, int k) {
        if (k < 1 || static_cast<size_t>(k) + 1 >= a.size()) return k;
        const double y0 = a[static_cast<size_t>(k) - 1], y1 = a[static_cast<size_t>(k)], y2 = a[static_cast<size_t>(k) + 1];
        const double den = y0 - 2 * y1 + y2;
        double frac = std::fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0;
        if (frac > 1 || frac < -1) frac = 0;
        return k + frac;
    }

    // How well a line-rate comb of period mlp (samples) explains acf: the
    // fundamental must be a real peak, scored with its first few harmonics. This
    // tests DISCRETE candidate periods (the real display modes), so there is no
    // sub-multiple ambiguity - a plain "strongest peak in a band" search picks a
    // 2x or 1/2x harmonic when the screen has strong vertical coherence.
    double comb_score(const std::vector<float>& acf, double mlp) const {
        if (!(mlp > 1) || acf.empty()) return -1;
        const double f = acf_interp(acf, mlp);
        if (f < LINE_PEAK_MIN) return -1;
        double s = f;
        for (int h = 2; h <= 4; h++) {
            const double v = acf_interp(acf, h * mlp);
            if (v > 0) s += v / h;   // weight the fundamental most
        }
        return s;
    }

    // precise samples-per-line, starting from a known guess (the chosen mode's
    // nominal line period, good to the display's clock tolerance). It climbs the
    // comb: at each target lag the current estimate places the true tooth within
    // a couple of samples of H*lper, and dividing the refined lag by H divides
    // the parabolic error by H. A 0.1 % error shears the raster across the
    // frame, so this must be accurate; starting from the known guess (not a
    // noisy local peak) keeps the climb from latching a 2x harmonic.
    double refine_spl(const std::vector<float>& acf, double guess) const {
        if (!(guess > 1)) return guess;
        double lper = guess;
        for (const double target : {120.0, 400.0, 1200.0}) {
            if (target >= static_cast<double>(acf.size()) - 2) break;
            const int H = std::max(2, static_cast<int>(std::lround(target / lper)));
            const int kk = static_cast<int>(std::lround(H * lper));
            int best = -1;
            for (int k = kk - 3; k <= kk + 3; k++)
                if (k > 1 && static_cast<size_t>(k) + 1 < acf.size() &&
                    (best < 0 || acf[static_cast<size_t>(k)] > acf[static_cast<size_t>(best)]))
                    best = k;
            if (best > 1 && acf[static_cast<size_t>(best)] > 0.12) {
                const double cand = refine(acf, best) / H;
                if (std::fabs(cand - guess) < 0.03 * guess) lper = cand;   // stay near the known line rate
            }
        }
        return lper;
    }

    // precise samples-per-line from the FRAME peak, given the line count: the
    // frame repeats exactly every vtotal lines, so the autocorrelation peak
    // within +-0.5 line of vtotal*guess is the true frame period (the +-1 line
    // aliases are a whole line away). Divided by vtotal this is the sample clock
    // to ~0.01 % for a display at its nominal clock - no horizontal shear. If no
    // clear frame peak sits there (a display whose clock is off by more than
    // half a line), fall back to the comb climb, which tracks the real clock.
    double refine_spl_frame(const std::vector<float>& acf, int vtotal, double guess) const {
        if (vtotal < 1 || !(guess > 1)) return guess;
        const double sc = refine_spl(acf, guess);   // comb climb: sharp when the comb is clean
        // frame anchor: the peak within +-0.5 line of vtotal*guess, divided by
        // vtotal - robust to content and disambiguates the +-1 line aliases
        const double target = vtotal * guess;
        const int w = std::max(2, static_cast<int>(std::lround(0.5 * guess)));
        const int lo = std::max(2, static_cast<int>(std::lround(target)) - w);
        const int hi = std::min(static_cast<int>(acf.size()) - 2, static_cast<int>(std::lround(target)) + w);
        double sf = -1;
        if (hi > lo) {
            int pk = lo;
            for (int k = lo; k <= hi; k++)
                if (acf[static_cast<size_t>(k)] > acf[static_cast<size_t>(pk)]) pk = k;
            if (acf[static_cast<size_t>(pk)] >= 0.10) {
                const double s = refine(acf, pk) / vtotal;
                if (std::fabs(s - guess) < 0.02 * guess) sf = s;
            }
        }
        if (sf > 0) return std::fabs(sc - sf) < 0.004 * guess ? sc : sf;   // agree -> sharp comb, else trust frame
        return sc;   // clock off nominal (no frame peak in the window): track it from the comb
    }

    static double acf_interp(const std::vector<float>& a, double lag) {
        const int k = static_cast<int>(std::floor(lag));
        if (k < 1 || static_cast<size_t>(k) + 1 >= a.size()) return -2.0;
        const double f = lag - k;
        return a[static_cast<size_t>(k)] * (1 - f) + a[static_cast<size_t>(k) + 1] * f;
    }

    void try_detect() {
        int flo = 0, fhi = 0;
        frame_window(flo, fhi);
        if (flo < 1) flo = 1;
        if (fhi <= flo) return;
        // the newest buf_cap() samples (or the ~2.1 periods a very slow refresh
        // window needs, if more): a fixed length for the given settings, so the
        // period finder keeps one FFT plan - ebuf_ itself drifts between cap and
        // cap + 64 K samples as it is compacted
        const size_t want = std::max(buf_cap(), static_cast<size_t>(fhi) * 21 / 10);
        const size_t n = std::min(ebuf_.size(), want);
        const float* x = ebuf_.data() + (ebuf_.size() - n);
        if (static_cast<size_t>(fhi) * 21 / 10 > n) return;   // need ~2.1 periods buffered
        if (static_cast<size_t>(fhi) >= n) fhi = static_cast<int>(n) - 1;

        const video::PeriodResult fr = pf_frame_.find(x, n, flo, fhi);
        acf_have_ = fr.ok;   // the frame-rate graph shows this search
        acf_flo_ = flo;
        acf_fhi_ = fhi;
        acf_frame_ = fr.ok ? fr.period : 0;
        acf_peak_ = fr.ok ? fr.peak : 0;
        const bool manual = opt_mode_ == "manual";
        const bool frame_ok = fr.ok && fr.confidence >= FRAME_CONF_MIN && fr.peak >= FRAME_PEAK_MIN;
        double frame_coarse = fr.ok ? fr.period : 0;
        if (!(frame_coarse > 0) && manual && std::isfinite(man_fps_) && man_fps_ > 0) frame_coarse = fs_ / man_fps_;
        // the line period the total-lines measurement tests while unlocked:
        // Auto's best table candidate's, Manual's measured one
        cands_.clear();
        if (opt_mode_ == "auto" && frame_ok) auto_candidates(fr.period, pf_frame_.autocorr());
        man_spl0_ = manual && frame_coarse > 0 ? manual_spl0(frame_coarse, pf_frame_.autocorr()) : 0;
        track_line_count(locked_ ? line_period_ : !cands_.empty() ? cands_.front().spl : man_spl0_);
        if (!manual && !frame_ok) { on_fail(); return; }
        if (!(frame_coarse > 0)) { on_fail(); return; }

        // the mode is chosen from the line-rate comb of this same full-buffer
        // autocorrelation (thousands of lines -> a sharp comb) and the measured
        // total lines; the frame peak is only a coarse refresh gate (it is +-1
        // line ambiguous under vertical coherence, so it cannot fix the
        // resolution on its own)
        const Geom g = resolve_geometry(frame_coarse, pf_frame_.autocorr());
        if (g == Geom::wait) {   // waiting for the measured count (MEAS_WAIT_S at most); a lock stays as it is
            host.fact("Status", "measuring the total lines...");
            return;
        }
        if (g == Geom::fail) { on_fail(); return; }

        // success - resolve_geometry has set the geometry, frame_period_ and line_period_
        fails_ = 0;
        frame_rate_ = fs_ / frame_period_;
        if (!locked_) { next_blank_ = next_acc_ = next_video_ = host.time(); }   // start producing at once
        locked_ = true;
        if (!(rows_L_ > 0)) track_line_count(line_period_);   // nothing measured a line period before the lock
        publish_facts();
        maybe_announce();
    }

    void on_fail() {
        if (autolock_) return;   // frozen: ignore transient misses
        if (locked_ && ++fails_ >= LOSS_FAILS) {
            drop_lock(true);
            clear_facts();
        } else if (!locked_) {
            host.fact("Status", "searching for a display...");
        }
    }

    enum class Geom { fail, ok, wait };

    // Auto's candidates: the table modes whose refresh is within REFRESH_GATE
    // of the coarse frame rate and whose LINE-rate comb explains the
    // autocorrelation (comb_score), best comb first, each with its refined
    // samples per line. The line rate (pixel_clock/htotal) separates the modes
    // cleanly; the measured total lines then checks the line count.
    void auto_candidates(double frame_coarse, const std::vector<float>& acf) {
        if (!(frame_coarse > 0)) return;
        const double frate_coarse = fs_ / frame_coarse;
        size_t nm = 0;
        const video::DisplayMode* modes = video::display_modes(&nm);
        for (size_t i = 0; i < nm; i++) {
            const video::DisplayMode& m = modes[i];
            const double mr = m.actual_refresh_hz();
            if (!(mr > 0) || std::fabs(mr - frate_coarse) / frate_coarse > REFRESH_GATE) continue;
            if (!(m.line_rate_hz() > 0)) continue;
            const double sc = comb_score(acf, fs_ / m.line_rate_hz());
            if (sc < LINE_PEAK_MIN) continue;   // (comb_score is -1 below it)
            cands_.push_back({&m, sc, refine_spl_frame(acf, m.vtotal, fs_ / m.line_rate_hz())});
        }
        // the first of equal scores stays first, as the table order decided before
        std::stable_sort(cands_.begin(), cands_.end(), [](const AutoCand& a, const AutoCand& b) { return a.comb > b.comb; });
    }

    // a table mode is the measured display when it has the measured count at
    // the line period the rows were summed at (to the clock tolerance: a
    // 59.94 Hz variant of a 60 Hz mode is 0.1 % off). Exactly that count -
    // one line off rolls the picture - unless the rows were summed at a
    // hypothesis' period: that is only as exact as the refinement (4K seen
    // as 1280x720: 0.08 % off, which counts 749 lines for 750), so +-1
    bool count_agrees(const video::DisplayMode& m, int v, double L, bool hyp) const {
        if (v <= 0 || !(L > 0) || !(m.line_rate_hz() > 0)) return false;
        return std::abs(m.vtotal - v) <= (hyp ? 1 : 0) && std::fabs(fs_ / m.line_rate_hz() - L) <= MEAS_L_TOL * L;
    }

    // a first lock waits for the measured count while one may still come
    // (a line period to sum the rows at, and less than MEAS_WAIT_S of trying)
    bool meas_pending() {
        if (meas_v_ > 0 || !(rows_L_ > 0)) return false;
        const double t = host.time();
        if (search_t0_ < 0) search_t0_ = t;
        return t - search_t0_ < MEAS_WAIT_S;
    }

    // v / L: the measured count agreeing with it and its line period, which
    // the rows were summed at (0: none, or a hypothesis')
    void use_auto_mode(const AutoCand& c, int v, double L) {
        const double mlp = fs_ / c.m->line_rate_hz();
        double spl = c.spl;
        // the measured count agrees: the frame is v lines of L, so where the
        // refinement lands more than a line per ~10 frames off (a display
        // off its nominal clock, e.g. 1920x1080 at 59.94 Hz) the measured
        // line period holds the picture still
        if (v == c.m->vtotal && L > 0 && std::fabs(spl - L) > 1e-4 * L) spl = L;
        set_mode(*c.m, "auto", spl);
        matched_ = true;
        unlisted_ = false;
        match_err_ = std::fabs(spl - mlp) / mlp;
    }

    // Auto, a confident measured count that no table mode has: the full
    // raster of that many lines at the measured line period (the active area
    // is not known; Manual with the width and height gives it)
    void use_unlisted(int vt, double spl) {
        const int ht = std::max(8, static_cast<int>(std::lround(vt * MAN_HT_PER_VT)));
        const double frate = fs_ / (spl * vt);
        apply_geom(ht, vt, ht, vt, frate * vt * ht, spl);
        matched_ = false;
        unlisted_ = true;
        match_err_ = 0;
        mode_label_ = std::to_string(vt) + " lines @ " + f2(frate) + " Hz (not listed)";
    }

    // turn the coarse frame period + the line-rate comb into a concrete raster
    // geometry, setting frame_period_ / line_period_ precisely
    Geom resolve_geometry(double frame_coarse, const std::vector<float>& acf) {
        const std::string want = opt_mode_;

        if (want != "auto" && want != "manual") {   // a forced DMT mode: geometry fixed
            const video::DisplayMode* dm = mode_by_name(want);
            if (!dm || !(dm->line_rate_hz() > 0)) return Geom::fail;
            const double mlp = fs_ / dm->line_rate_hz();
            set_mode(*dm, "forced", refine_spl_frame(acf, dm->vtotal, mlp));
            matched_ = true;
            match_err_ = 0;
            return Geom::ok;
        }
        if (want == "manual") return resolve_manual(frame_coarse, acf);

        // auto. With an accepted measured count: the best-comb table mode that
        // has it, else the display is not listed - never a table mode whose
        // line count the signal contradicts
        if (meas_v_ > 0) {
            const double L = rows_L_ > 0 && std::fabs(rows_L_ - meas_L_) <= MEAS_L_TOL * meas_L_ ? rows_L_ : meas_L_;
            for (const AutoCand& c : cands_)
                if (count_agrees(*c.m, meas_v_, L, meas_hyp_)) {
                    use_auto_mode(c, meas_v_, meas_hyp_ ? 0 : L);
                    return Geom::ok;
                }
            // a table mode with the measured count and line rate whose comb
            // missed the gate (a weak signal: 1920x1080 at 7 dB) - the two
            // measurements are the line-rate check; the nearest line period.
            // Not at a hypothesis' period: that count only confirms its own
            // hypothesis
            if (!meas_hyp_) {
                size_t nm = 0;
                const video::DisplayMode* modes = video::display_modes(&nm);
                const video::DisplayMode* best = nullptr;
                for (size_t i = 0; i < nm; i++)
                    if (count_agrees(modes[i], meas_v_, L, false) &&
                        (!best || std::fabs(fs_ / modes[i].line_rate_hz() - L) < std::fabs(fs_ / best->line_rate_hz() - L)))
                        best = &modes[i];
                if (best) {
                    use_auto_mode({best, 0, L}, meas_v_, L);
                    return Geom::ok;
                }
            }
            if (!meas_hyp_) {
                if (meas_v_ < MIN_VTOTAL || meas_v_ > MAX_VTOTAL) return Geom::fail;
                use_unlisted(meas_v_, L);
                return Geom::ok;
            }
            // counted at a hypothesis' period, which it contradicts: the rows
            // give the frame in lines of the period they are summed at, so that
            // says the hypothesis is wrong, not what the count is - the comb
            // alone decides, as before (the estimate failed: a weak signal)
        }
        // not accepted yet: a confident estimate that agrees with the best comb
        // is two independent observables saying the same - lock at once
        if (!cands_.empty() && meas_now_.confident &&
            count_agrees(*cands_.front().m, meas_now_.v, meas_now_.L, meas_now_.hyp)) {
            use_auto_mode(cands_.front(), meas_now_.v, meas_now_.hyp ? 0 : meas_now_.L);
            return Geom::ok;
        }
        if ((!locked_ || relearn_) && meas_pending()) return Geom::wait;
        relearn_ = false;
        // no measurement to go on (weak signal, no line structure): the line
        // comb alone, as before the measurement existed
        if (!cands_.empty()) {
            use_auto_mode(cands_.front(), 0, 0);
            return Geom::ok;
        }
        // no table mode fits: reconstruct at the native effective resolution,
        // with total lines from the (coarse) frame period and the strongest line
        // comb in the search band
        double lper = 0;
        if (!fallback_line(acf, &lper)) return Geom::fail;
        const int vt = static_cast<int>(std::lround(frame_coarse / lper));
        if (vt < MIN_VTOTAL || vt > MAX_VTOTAL) return Geom::fail;
        // a typical raster shape (as Manual's timing-only path), not one pixel
        // per sample - that was a sliver many screens tall
        const int ht = std::max(8, static_cast<int>(std::lround(vt * MAN_HT_PER_VT)));
        const double frate_coarse = fs_ / frame_coarse;
        apply_geom(ht, vt, ht, vt, frate_coarse * vt * ht, lper);
        matched_ = false;
        unlisted_ = false;
        match_err_ = 0;
        mode_label_ = "~" + std::to_string(vt) + " lines @ " + f1(frate_coarse) + " Hz (estimate)";
        return Geom::ok;
    }

    // strongest single comb peak in the line-rate band - only for the
    // unmatched-mode fallback (a real display always hits the DMT table)
    bool fallback_line(const std::vector<float>& acf, double* out) const {
        const int lo = std::max(2, static_cast<int>(std::floor(fs_ / LINE_RATE_MAX)));
        const int hi = std::min(static_cast<int>(acf.size()) - 2, static_cast<int>(std::ceil(fs_ / LINE_RATE_MIN)));
        if (hi <= lo) return false;
        int pk = lo;
        for (int k = lo; k <= hi; k++)
            if (acf[static_cast<size_t>(k)] > acf[static_cast<size_t>(pk)]) pk = k;
        if (acf[static_cast<size_t>(pk)] < LINE_PEAK_MIN) return false;
        *out = refine_spl(acf, refine(acf, pk));
        return *out > 0;
    }

    // How well a line period of frame / lines explains acf: with the right
    // period, frame * N / lines is a whole number of lines for every N, so the
    // autocorrelation sits on a line-comb tooth at each of these lags; with a
    // wrong one the lags sweep through every line phase and average to the
    // level between the teeth. Every N across the middle of the frame is used
    // (not near 0 or one frame, where every period agrees) - a sparse set of
    // lags would also line up for particular wrong counts.
    double lines_score(const std::vector<float>& acf, double frame, int lines) const {
        if (lines < 16 || !(frame > 0)) return -1;
        double s = 0;
        int cnt = 0;
        for (int n = lines / 8; n <= lines - lines / 8; n++) {
            const double v = acf_interp(acf, frame * n / lines);
            if (v < -1.5) continue;   // outside the autocorrelation
            s += v;
            cnt++;
        }
        return cnt >= lines / 2 ? s / cnt : -1;
    }

    // The precise line period (samples) for manual mode, from the frame peak
    // the period finder returned and a range of plausible line counts. That
    // peak is a whole number of lines, but not always the frame's own count:
    // with a vertically coherent picture every line multiple correlates, and
    // the strongest is the one nearest a whole number of samples - often a few
    // lines off the true frame. So the frame period is not trusted, only that
    // frame / (some count in [lo, hi]) is the line period: the count whose
    // comb the autocorrelation follows best (lines_score) gives it. 0 = no
    // clear answer (a weak signal, or no frame peak at all).
    double manual_line_period(const std::vector<float>& acf, double frame, int lo, int hi) const {
        int best = 0;
        double bs = -1, second = -1;
        for (int n = std::max(2, lo); n <= hi; n++) {
            const double sc = lines_score(acf, frame, n);
            if (sc > bs) { second = bs; bs = sc; best = n; }
            else if (sc > second) second = sc;
        }
        if (best == 0 || bs < MAN_COMB_MIN || bs < MAN_COMB_RATIO * second) return 0;
        return frame / best;
    }

    // the entered Manual resolution (both, else 0 x 0)
    void manual_res(int& aw, int& ah) const {
        const auto dim = [](double v, int max) {
            return std::isfinite(v) && v >= 16 && v <= max ? static_cast<int>(std::lround(v)) : 0;
        };
        aw = dim(man_w_, 8192);
        ah = dim(man_h_, MAX_VTOTAL - 1);
        if (aw <= 0 || ah <= 0) aw = ah = 0;
    }
    // the plausible total lines for an active height: up to 20 % blanking
    static int manual_vt_hi(int ah) { return ah + std::max(8, ah / 5) + MAN_ALIAS_LINES; }

    // Manual mode. The timing comes from the entered fields; with the active
    // width and height entered (both), whatever is missing is filled in:
    //  - total lines: the measured count (measure_lines), unless a known
    //    timing of that resolution - a built-in mode of that size, the VESA
    //    CVT reduced- and standard-blanking timings - whose own refresh the
    //    measured line period reproduces is one line from it (that settles
    //    a +-1 tie). Without an accepted count (a weak signal; a first lock
    //    waits up to MEAS_WAIT_S for one): that known timing; if none fits,
    //    the count that gives the whole-number refresh (a custom timing at
    //    60.00, 75.00 ...); without a measurable line period, the first
    //    known timing.
    //  - pixels per line: from the pixel clock, else the known timing whose
    //    line count is nearest (an approximation for a custom timing).
    // Without a resolution the total lines are required and the full raster is
    // shown. Entered total lines always win.
    // Manual's line period from the signal (manual_line_period): near the
    // entered line count, else anywhere from the height up to 20 % vertical
    // blanking; 0 without either, or with no clear answer
    double manual_spl0(double frame_coarse, const std::vector<float>& acf) const {
        int aw = 0, ah = 0;
        manual_res(aw, ah);
        const int vt = std::isfinite(man_lines_) ? static_cast<int>(std::lround(man_lines_)) : 0;
        if (vt > 0) return manual_line_period(acf, frame_coarse, vt - MAN_ALIAS_LINES, vt + MAN_ALIAS_LINES);
        if (ah > 0) return manual_line_period(acf, frame_coarse, ah + 1 - MAN_ALIAS_LINES, manual_vt_hi(ah));
        return 0;
    }

    Geom resolve_manual(double frame_coarse, const std::vector<float>& acf) {
        int aw = 0, ah = 0;
        manual_res(aw, ah);
        const bool have_res = aw > 0 && ah > 0;
        int vt = std::isfinite(man_lines_) ? static_cast<int>(std::lround(man_lines_)) : 0;
        double spl0 = man_spl0_;   // manual_spl0 of this search

        int ht_known = 0;   // pixels per line of the known timing nearest in line count
        if (have_res) {
            // the measured count: in this resolution's range and summed at
            // (about) the line period measured here
            // (and at the measured line period - one summed at the lock's own
            // only says whether the lock's frame is right: the rows give the
            // frame in lines of the period they are summed at)
            const bool meas_ok = vt <= 0 && meas_v_ > ah && meas_v_ <= manual_vt_hi(ah) && meas_L_ > 0 && !meas_hyp_ &&
                                 (!(spl0 > 0) || std::fabs(meas_L_ - spl0) <= MEAS_L_TOL * spl0);
            // the refresh the known timings are looked up at: as entered, else
            // the measured count's (the frame peak's is a coarse gate - 89 Hz
            // for a 30 Hz display under the default 50-90 Hz window)
            const double refresh = std::isfinite(man_fps_) && man_fps_ > 0 ? man_fps_
                                   : meas_ok                                ? std::round(fs_ / (meas_v_ * meas_L_))
                                                                            : std::round(fs_ / frame_coarse);
            // the known timings of this resolution, most likely first
            std::vector<video::DisplayMode> known;
            size_t nm = 0;
            const video::DisplayMode* modes = video::display_modes(&nm);
            for (size_t i = 0; i < nm; i++)
                if (modes[i].width == aw && modes[i].height == ah && std::fabs(modes[i].refresh_hz - refresh) <= 1.0)
                    known.push_back(modes[i]);
            known.push_back(video::cvt_mode(aw, ah, refresh, true));    // reduced blanking: what flat panels use
            known.push_back(video::cvt_mode(aw, ah, refresh, false));
            std::erase_if(known, [ah](const video::DisplayMode& m) { return m.vtotal <= ah || m.htotal <= 0; });
            if (vt <= 0) {
                int vt_fit = 0;   // the known timing the measured line period reproduces
                if (spl0 > 0) {
                    double be = MAN_FIT_TOL_HZ;
                    for (const video::DisplayMode& m : known) {
                        const double e = std::fabs(fs_ / (m.vtotal * spl0) - m.actual_refresh_hz());
                        if (e < be) { be = e; vt_fit = m.vtotal; }
                    }
                }
                if (meas_ok) {
                    vt = vt_fit > 0 && std::abs(vt_fit - meas_v_) == 1 ? vt_fit : meas_v_;
                    if (!(spl0 > 0)) spl0 = rows_L_ > 0 ? rows_L_ : meas_L_;
                } else if (spl0 > 0 && !locked_ && meas_pending()) {
                    return Geom::wait;
                } else if (spl0 > 0) {
                    vt = vt_fit;
                    if (vt <= 0 && refresh > 0) vt = static_cast<int>(std::lround(fs_ / (refresh * spl0)));
                    if (vt <= ah) vt = 0;   // fewer lines than the picture has: not a usable answer
                }
            }
            if (vt <= 0 && !known.empty()) vt = known.front().vtotal;   // nothing measured
            int bd = INT_MAX;
            for (const video::DisplayMode& m : known)
                if (std::abs(m.vtotal - vt) < bd) { bd = std::abs(m.vtotal - vt); ht_known = m.htotal; }
        }
        if (vt < MIN_VTOTAL || vt > MAX_VTOTAL) return Geom::fail;
        // samples per line: as entered; else the measured line period (exact:
        // the period finder's sub-sample frame peak over ~1000 lines); else the
        // comb / frame-peak refinement from the coarse frame period
        double spl;
        if (std::isfinite(man_spl_) && man_spl_ > 0) spl = man_spl_;
        else if (spl0 > 0) spl = spl0;
        else spl = refine_spl_frame(acf, vt, frame_coarse / vt);
        const double fperiod = spl * vt;
        const double frate = fs_ / fperiod;
        int ht = 0;
        if (std::isfinite(man_pclk_mhz_) && man_pclk_mhz_ > 0)
            ht = static_cast<int>(std::lround(man_pclk_mhz_ * 1e6 / (frate * vt)));
        else if (ht_known > 0)
            ht = ht_known;
        // htotal not otherwise known (only the line count): a typical raster
        // shape, so the picture is shown at a sensible aspect - it used to be
        // one pixel per sample (~50 x 806), a sliver many screens tall. The
        // pixel clock that follows is a rough estimate (the fact says "est")
        if (ht < 8) ht = std::max(8, static_cast<int>(std::lround(vt * MAN_HT_PER_VT)));
        // the active area: as entered (never larger than the raster), else
        // unknown - show the full raster
        const int w = have_res ? std::min(aw, ht) : ht;
        const int h = have_res ? std::min(ah, vt) : vt;
        apply_geom(ht, vt, w, h, frate * vt * ht, spl);
        matched_ = false;
        match_err_ = 0;
        if (have_res) mode_label_ = std::to_string(w) + "x" + std::to_string(h) + " @ " + f1(frate) + " Hz (manual)";
        else mode_label_ = std::to_string(vt) + " lines @ " + f1(frate) + " Hz (manual)";
        return Geom::ok;
    }

    void apply_geom(int ht, int vt, int w, int h, double pclk, double spl) {
        // re-anchor the frame boundary under the OLD period to the one add_frame
        // renders next (~two frames back), so the new period only shifts frame
        // starts by a frame or two's worth of the change
        if (have_phase_ && frame_period_ > 0) roll_anchor(frame_period_);
        htotal_ = ht;
        vtotal_ = vt;
        width_ = w;
        height_ = h;
        pixel_clock_ = pclk;
        line_period_ = spl;
        frame_period_ = spl * vt;
    }

    void set_mode(const video::DisplayMode& m, const char* tag, double spl) {
        apply_geom(m.htotal, m.vtotal, m.width, m.height, m.pixel_clock_hz, spl);
        nominal_refresh_ = m.refresh_hz;   // the mode's label refresh, for the event/facts
        char b[96];
        std::snprintf(b, sizeof b, "%dx%d @ %g Hz (%s)", m.width, m.height, m.refresh_hz, tag);
        mode_label_ = b;
    }

    void maybe_announce() {
        if (opt_mode_ == "manual") return;
        char b[128];
        if (unlisted_) {   // once per lock, also after a table mode was announced
            if (announced_unlisted_) return;
            std::snprintf(b, sizeof b, "Display not listed: %d lines @ %.2f Hz - enter its width and height in Manual",
                          vtotal_, frame_rate_);
            host.event(b, 10.0);
            announced_unlisted_ = announced_ = true;
            return;
        }
        if (announced_ || !matched_) return;
        std::snprintf(b, sizeof b, "Detected %dx%d @ %.0f Hz", width_, height_, std::round(nominal_refresh_));
        host.event(b, 10.0);
        announced_ = true;
    }

    // the accepted measured count, and where it disagrees with the timing in
    // use, what that is ("" while there is no accepted count)
    std::string measured_text() const {
        if (meas_v_ <= 0 || !(meas_L_ > 0)) return "";
        char b[160];
        const double fr = fs_ / (meas_v_ * meas_L_);
        if (unlisted_) std::snprintf(b, sizeof b, "%d (%.2f Hz) - no listed mode has it", meas_v_, fr);
        else if (std::abs(meas_v_ - vtotal_) <= (meas_hyp_ ? 1 : 0)) std::snprintf(b, sizeof b, "%d", vtotal_);
        else if (meas_hyp_)   // summed at the lock's own period: only whether its frame fits
            std::snprintf(b, sizeof b, "the frame repeats every %d lines of this timing, not %d: it does not fit",
                          meas_v_, vtotal_);
        else if (opt_mode_ == "manual")
            std::snprintf(b, sizeof b, "%d (%.2f Hz) - %d in use", meas_v_, fr, vtotal_);
        else
            std::snprintf(b, sizeof b, "%d (%.2f Hz) - the forced mode has %d: try Auto, or Manual with the resolution",
                          meas_v_, fr, vtotal_);
        return b;
    }

    // -------- facts --------
    void publish_facts() {
        host.fact("Status", unlisted_ ? "the display is not a listed mode - choose Manual and enter its width and height"
                                      : "");
        host.fact("Measured lines", measured_text());
        host.fact("Mode", mode_label_ + (matched_ ? ("  err " + f1(match_err_ * 100) + "%") : ""));
        host.fact("Frame rate", f2(frame_rate_) + " Hz");
        host.fact("Line rate", f1(frame_rate_ * vtotal_ / 1000.0) + " kHz");
        host.fact("Total lines", std::to_string(vtotal_));
        host.fact("Pixel clock", f1(pixel_clock_ / 1e6) + (matched_ ? " MHz" : " MHz (est)"));
        host.fact("Resolution limit", "~" + std::to_string(std::max(1, static_cast<int>(std::lround(frame_period_ / vtotal_)))) +
                                           " px/line at " + f1(fs_ / 1e6) + " MHz");
        host.fact("Tune near", harmonics_text());
        host.fact("Frames averaged", std::to_string(acc_.frames()));
    }

    void clear_facts() {
        for (const char* k : {"Status", "Mode", "Frame rate", "Line rate", "Total lines", "Measured lines",
                              "Pixel clock", "Resolution limit", "Tune near", "Frames averaged"})
            host.fact(k, "");
    }

    // the pixel-clock harmonics that land in VHF/UHF - where to tune the receiver
    std::string harmonics_text() const {
        if (!(pixel_clock_ > 0)) return "";
        const double pmhz = pixel_clock_ / 1e6;
        std::string out;
        int shown = 0;
        for (int k = 1; k <= 64 && shown < 4; k++) {
            const double f = k * pmhz;
            if (f < 40 || f > 1700) continue;
            char b[64];
            std::snprintf(b, sizeof b, "%dx%.1f = %.0f MHz", k, pmhz, f);
            if (!out.empty()) out += ", ";
            out += b;
            shown++;
        }
        return out;
    }

    // a buffer-local origin two frames back (with a line of margin) for the
    // arbitrary-phase raster the blanking search works on; the picture repeats
    // every frame, so which frame is used does not matter
    double scan_origin() const {
        const double spl = frame_period_ / vtotal_;
        int64_t g = total_ - static_cast<int64_t>(std::llround(2 * frame_period_ + 3 * spl)) - 2;
        if (g < buf_base_) g = buf_base_;
        return static_cast<double>(g - buf_base_);
    }

    // -------- blanking phase --------
    // Locate the blanking in an arbitrary-phase raster, convert it to the GLOBAL
    // sample of the true frame's first active pixel, and store it as the frame
    // anchor. Every render then anchors to it (not to a per-call origin, whose
    // sub-line phase changes as the buffer advances). Only the raster's column
    // and row sums are needed, so they are formed at the full htotal x vtotal
    // geometry without storing the raster (a 4K one would be ~40 MB).
    void find_blanking() {
        if (htotal_ <= 0 || vtotal_ <= 0) return;
        const int W = htotal_, H = vtotal_;
        // guard against an absurd manual pixel clock; a 4K full raster is
        // 4400x2250 = 9.9M, so admit up to 12M pixels per search
        if (static_cast<size_t>(W) * H > 12000000) return;
        const double spl = frame_period_ / vtotal_;
        const double spp = spl / htotal_;
        const double origin = scan_origin();
        video::RasterMap m;
        m.width = W;
        m.height = H;
        m.samples_per_line = spl;
        m.samples_per_pixel = spp;
        m.origin = origin;
        video::raster_profiles(ebuf_.data(), ebuf_.size(), m, bcol_, brow_);
        const int bw = std::max(1, htotal_ - width_);
        const int bh = std::max(1, vtotal_ - height_);
        const video::BlankOffset bo = video::find_blanking_offset(bcol_, brow_, bw, bh);
        if (!bo.ok) return;
        // true active (0,0) in this raster, then its global sample
        const int v_eff = ((bo.v_offset + bh) % vtotal_ + vtotal_) % vtotal_;
        const int h_eff = ((bo.h_offset + bw) % htotal_ + htotal_) % htotal_;
        anchor_ = static_cast<double>(buf_base_) + origin + v_eff * spl + h_eff * spp;   // near now
        have_phase_ = true;
    }

    // move anchor_ by whole periods `period` to the latest frame boundary at or
    // before the frame add_frame renders (two frames and a few lines back)
    void roll_anchor(double period) {
        if (!(period > 0)) return;
        const double spl = vtotal_ > 0 ? period / vtotal_ : 0;
        const double target = static_cast<double>(total_) - 2 * period - 3 * spl;
        anchor_ += std::floor((target - anchor_) / period) * period;
    }

    // The DISPLAY geometry the picture shows: the active area when cropping,
    // else the full raster (what the emitted frame's aspect / size follow)
    void recon_dims(int& W, int& H) const {
        const bool crop = crop_ && width_ > 0 && height_ > 0 && (width_ < htotal_ || height_ < vtotal_);
        W = crop ? width_ : htotal_;
        H = crop ? height_ : vtotal_;
    }
    // Columns of the WORK raster the reconstruction runs on. Each row is one
    // display line (the vertical detail is real), but a line holds only
    // W * spp envelope samples (~18-60 at 2.4 MHz) however wide the display
    // is, so the columns are capped at RECON_OVERSAMPLE per sample - a 4K
    // line becomes ~124 columns, not 3840. send_video() maps it back to the
    // display's width; the picture is the same (the envelope is linearly
    // interpolated between samples either way), the memory / CPU are not.
    int work_width(int W) const {
        const double spl = frame_period_ / vtotal_;
        const double span = W * spl / htotal_;   // envelope samples across W display pixels
        if (!(span > 0)) return W;
        const double rw = std::ceil(RECON_OVERSAMPLE * span);
        return rw >= W ? W : std::max(2, static_cast<int>(rw));
    }
    // the pixel count the reconstruction works on (what resample/render cost
    // scales with): the work raster
    double recon_px() const {
        int W = 0, H = 0;
        recon_dims(W, H);
        return W > 0 && H > 0 ? static_cast<double>(work_width(W)) * H : 0.0;
    }
    // >1 stretches the accumulate / video cadence for a large work raster so
    // the per-second pixel work stays bounded; with the sample-capped columns
    // even 4K stays far below the reference, so this is 1 for every mode
    double cadence_factor() const {
        const double f = recon_px() / CADENCE_PX_REF;
        return f > 1.0 ? f : 1.0;
    }

    // -------- reconstruction --------
    void add_frame() {
        if (htotal_ <= 0 || vtotal_ <= 0 || !have_phase_) return;
        int W = 0, H = 0;
        recon_dims(W, H);
        if (W < 2 || H < 2) return;
        const int RW = work_width(W);
        if (acc_w_ != RW || acc_h_ != H || disp_w_ != W) {
            acc_.configure(RW, H, static_cast<float>(ema_));
            acc_w_ = RW;
            acc_h_ = H;
            disp_w_ = W;
        }
        const double spl = frame_period_ / vtotal_;
        const double spp = spl / htotal_;                     // samples per DISPLAY pixel
        const double wspp = spp * W / RW;                     // ... per work-raster column
        // the most recent frame boundary at least two frames back
        roll_anchor(frame_period_);
        double framestart = anchor_ - static_cast<double>(buf_base_);   // buffer-local
        if (framestart < 0) framestart = 0;
        video::RasterMap m;
        m.width = RW;
        m.height = H;
        m.samples_per_line = spl;
        m.samples_per_pixel = wspp;
        m.origin = framestart;
        m.h_offset = man_hoff_ * spp;
        m.v_offset = man_voff_;
        rbuf_.assign(static_cast<size_t>(RW) * H, 0.0f);
        video::resample(ebuf_.data(), ebuf_.size(), m, rbuf_.data());
        // small shift search to absorb residual phase creep: +-3 lines, and
        // +-3 DISPLAY pixels (as many work columns as that spans, often 0)
        const int sx = static_cast<int>(std::lround(3.0 * RW / W));
        acc_.add_xy(rbuf_.data(), sx, 3);
    }

    void send_video() {
        if (acc_.frames() == 0 || acc_w_ < 2 || acc_h_ < 2 || disp_w_ < 2) return;
        host.fact("Frames averaged", std::to_string(acc_.frames()));
        if (!host.video_wanted()) return;   // nobody is watching: skip the render
        video::StretchOpts so;
        so.lo_pct = lo_pct_;
        so.hi_pct = hi_pct_;
        so.invert = invert_;
        so.gamma = static_cast<float>(gamma_);
        acc_.render(gray_, so);
        const int RW = acc_w_, W = disp_w_, H = acc_h_;
        if (gray_.size() != static_cast<size_t>(RW) * H) return;

        // The host compresses to JPEG. The emitted frame has the display's
        // geometry (W x H), its LONGEST side capped at max_display_px (display
        // / transmission only): past the cap it is box-downscaled by d, as if
        // the full-resolution raster were. Rows are box-averaged d at a time;
        // each output column samples the work raster (linear interpolation) at
        // the centre of the d display columns it covers. Default 1280; raise
        // for a sharper picture, lower for less CPU / on a no-libjpeg build
        // (raw pixels only, 1 MiB wire cap).
        const int cap = max_display_px_ > 0 ? max_display_px_ : DEFAULT_MAX_DISPLAY_PX;
        const int d = std::max(W, H) > cap ? (std::max(W, H) + cap - 1) / cap : 1;
        const int ow = (W + d - 1) / d, oh = (H + d - 1) / d;
        col_j_.resize(static_cast<size_t>(ow));
        col_f_.resize(static_cast<size_t>(ow));
        for (int c = 0; c < ow; c++) {
            const int x0 = c * d, x1 = std::min(W, x0 + d) - 1;
            double u = 0.5 * (x0 + x1) * RW / W;   // display column -> work column
            if (u > RW - 1) u = RW - 1;
            const int j = std::min(static_cast<int>(u), RW - 2);
            col_j_[static_cast<size_t>(c)] = j;
            col_f_[static_cast<size_t>(c)] = static_cast<float>(u - j);
        }
        out8_.assign(static_cast<size_t>(ow) * oh, 0);
        for (int r = 0; r < oh; r++) {
            const int r0 = r * d, r1 = std::min(H, r0 + d);
            const float inv = 1.0f / static_cast<float>(r1 - r0);
            for (int c = 0; c < ow; c++) {
                const size_t j = static_cast<size_t>(col_j_[static_cast<size_t>(c)]);
                const float f = col_f_[static_cast<size_t>(c)];
                float sum = 0;
                for (int sr = r0; sr < r1; sr++) {
                    const uint8_t* g = gray_.data() + static_cast<size_t>(sr) * RW;
                    sum += g[j] + (static_cast<float>(g[j + 1]) - g[j]) * f;
                }
                out8_[static_cast<size_t>(r) * ow + c] = static_cast<uint8_t>(std::min(255.0f, sum * inv + 0.5f));
            }
        }
        kp::VideoFrame f;
        f.width = ow;
        f.height = oh;
        f.format = 0;   // GRAY8
        f.pixels = out8_.data();
        f.label = mode_label_;
        f.quality = quality_;     // host compresses raw pixels to JPEG at this quality
        f.lossless = lossless_;
        host.video_frame(f);
    }

    // -------- free-run preview (display only) --------
    // Map the raw magnitude envelope straight onto the assumed geometry (the
    // forced mode, else FREE_RUN_DEFAULT_MODE) downscaled to a small preview
    // raster, and emit it so the canvas shows a live image while unlocked. No
    // sync recovery, no frame averaging (it must look live and moving), and it
    // never touches the lock / detection / DoA state. The brightness mapping is
    // the locked path's (invert / contrast / gamma options).
    void send_free_run() {
        if (!host.video_wanted()) return;   // display only, and nobody is watching
        const video::DisplayMode* dm =
            (opt_mode_ != "auto" && opt_mode_ != "manual") ? mode_by_name(opt_mode_) : nullptr;
        if (!dm) dm = mode_by_name(FREE_RUN_DEFAULT_MODE);
        if (!dm || !(dm->line_rate_hz() > 0) || dm->htotal <= 0) return;
        const int aw = dm->width > 0 ? dm->width : dm->htotal;    // active width (fall back to the full raster)
        const int ah = dm->height > 0 ? dm->height : dm->vtotal;
        if (aw < 2 || ah < 2) return;
        const double spl = fs_ / dm->line_rate_hz();   // nominal source samples per line
        const double spp = spl / dm->htotal;           // nominal source samples per active pixel
        if (!(spl > 1)) return;

        // cap the preview to FREE_RUN_MAX_PX on the longest side - a 4K mode must
        // not make the preview huge. Resample straight into the small raster
        // (fewer output pixels = cheaper): each preview row / column then spans
        // 1/scale source samples, so it still covers the full active area.
        double scale = static_cast<double>(FREE_RUN_MAX_PX) / std::max(aw, ah);
        if (scale > 1.0) scale = 1.0;
        const int pw = std::max(2, static_cast<int>(std::lround(aw * scale)));
        const int ph = std::max(2, static_cast<int>(std::lround(ah * scale)));

        video::FreeRunOpts fo;
        fo.width = pw;
        fo.height = ph;
        fo.samples_per_line = spl / scale;    // ph rows span the full active height
        fo.samples_per_pixel = spp / scale;   // pw cols span the full active width
        fo.stretch.lo_pct = lo_pct_;
        fo.stretch.hi_pct = hi_pct_;
        fo.stretch.invert = invert_;
        fo.stretch.gamma = static_cast<float>(gamma_);

        const double need = fo.samples_per_line * ph;
        if (static_cast<double>(ebuf_.size()) < need + 2) return;   // not enough buffered yet

        video::free_run_frame(ebuf_.data(), ebuf_.size(), fo, free_f_, free_pix_);
        if (free_pix_.size() != static_cast<size_t>(pw) * ph) return;

        char lbl[64];
        std::snprintf(lbl, sizeof lbl, "%dx%d - free-run (searching)", aw, ah);
        kp::VideoFrame f;
        f.width = pw;
        f.height = ph;
        f.format = 0;   // GRAY8
        f.pixels = free_pix_.data();
        f.label = lbl;
        f.quality = FREE_RUN_QUALITY;   // a lower-quality throwaway preview
        f.lossless = false;
        host.video_frame(f);
    }

    // -------- the row history: measured total lines, and the timing graphs --------

    // add envelope samples e[0..n) (global samples g0..) to the row history:
    // each row is the envelope integrated over one line period, the boundary
    // sample split by its fraction, so rows stay exact over thousands of lines
    void feed_rows(const float* e, size_t n, int64_t g0) {
        const double base = static_cast<double>(g0);
        size_t i = 0;
        while (i < n) {
            const double b = row_next_ - base;   // the next boundary, in block samples
            if (b >= static_cast<double>(n)) {
                double s = 0;
                for (size_t k = i; k < n; k++) s += e[k];
                row_acc_ += s;
                return;
            }
            const size_t kb = b > static_cast<double>(i) ? static_cast<size_t>(b) : i;   // the sample it falls in
            double s = 0;
            for (size_t k = i; k < kb; k++) s += e[k];
            const double f = std::clamp(b - static_cast<double>(kb), 0.0, 1.0);
            row_acc_ += s + f * e[kb];
            rows_[rows_head_] = static_cast<float>(row_acc_);
            rows_head_ = (rows_head_ + 1) % ROWS_N;
            if (rows_cnt_ < ROWS_N) rows_cnt_++;
            rows_seq_++;
            row_acc_ = (1.0 - f) * e[kb];
            row_next_ += rows_L_;
            i = kb + 1;
        }
    }

    // (re)start the row history at line period L when it isn't summed at it
    // already (0 = no line period known: off)
    void update_rows(double L) {
        if (!(L > 1)) { rows_L_ = 0; rows_cnt_ = 0; ra_seq_ = -1; return; }
        if (rows_L_ > 0 && std::fabs(L - rows_L_) <= ROWS_RESET_REL * rows_L_) return;
        // another line rate altogether: what was measured at the old one no
        // longer says anything about this one
        if (meas_L_ > 0 && std::fabs(L - meas_L_) > MEAS_L_TOL * meas_L_) {
            clear_meas();
            // locked on the old line rate (the display changed): keep that
            // geometry while the new count is measured, rather than take the
            // comb's guess in between (meas_pending)
            if (locked_) {
                relearn_ = true;
                search_t0_ = host.time();
            }
        }
        rows_.assign(ROWS_N, 0.0f);
        rows_head_ = rows_cnt_ = 0;
        rows_seq_ = 0;
        ra_seq_ = cand_seq_ = -1;
        rows_L_ = L;
        row_acc_ = 0;
        row_next_ = static_cast<double>(total_) + L;   // the next block starts the first row
    }

    // The line period measured from the last frame search: the
    // frame peak is a whole number of lines (not always the frame's own count),
    // so frame / n is the line period for the count n whose comb the
    // autocorrelation follows (lines_score) - and for its divisors, which
    // score the same: the largest of those. 0 = no line structure. Cached
    // while the frame peak stays put (the scan reads ~2 M autocorrelation values).
    double estimate_line_period() {
        if (!acf_have_ || !(acf_frame_ > 0) || acf_peak_ < LINE_EST_PEAK_MIN) return 0;
        const double t = host.time();
        if (std::fabs(acf_frame_ - est_frame_) < 0.5 && t - est_at_ < LINE_EST_KEEP_S) return est_line_;
        est_frame_ = acf_frame_;
        est_at_ = t;
        est_line_ = est_score_ = 0;
        const std::vector<float>& acf = pf_frame_.autocorr();
        const int lo = std::max(MIN_VTOTAL - MAN_ALIAS_LINES, static_cast<int>(std::ceil(acf_frame_ * LINE_RATE_MIN / fs_)));
        const int hi = std::min(MAX_VTOTAL + MAN_ALIAS_LINES, static_cast<int>(std::floor(acf_frame_ * LINE_RATE_MAX / fs_)));
        if (hi < lo) return 0;
        std::vector<double> sc(static_cast<size_t>(hi - lo + 1));
        double mx = -1;
        for (int k = lo; k <= hi; k++) {
            sc[static_cast<size_t>(k - lo)] = lines_score(acf, acf_frame_, k);
            mx = std::max(mx, sc[static_cast<size_t>(k - lo)]);
        }
        if (mx < LINE_EST_COMB_MIN || mx < LINE_EST_COMB_REL * acf_peak_) return 0;
        est_score_ = mx;
        for (int k = hi; k >= lo; k--)
            if (sc[static_cast<size_t>(k - lo)] >= 0.8 * mx) { est_line_ = acf_frame_ / k; break; }
        return est_line_;
    }

    // The line period the row history is summed at. It is MEASURED
    // (estimate_line_period), so a hypothesis on the wrong mode doesn't skew
    // the count - the lock's, or while searching Auto's best-comb mode's
    // (hyp). The hypothesis is used instead when it is another line rate
    // that explains the comb about as well: the estimate aliased (4K's
    // ~18-sample lines estimate as 80). Within MEAS_L_TOL the two are the
    // same line rate and the estimate (the frame peak / a whole count, exact)
    // is the sharper. 0 = none known; *is_hyp: the hypothesis' was taken.
    double rows_line_period(double hyp, bool* is_hyp) {
        const double est = estimate_line_period();
        *is_hyp = false;
        if (!(hyp > 1)) return est;
        if (est > 1 && std::fabs(hyp - est) <= MEAS_L_TOL * est) return est;
        const double sc = est > 1 && acf_have_ && acf_frame_ > 0
                              ? lines_score(pf_frame_.autocorr(), acf_frame_,
                                            static_cast<int>(std::lround(acf_frame_ / hyp)))
                              : -1;
        if (est > 1 && sc < 0.5 * est_score_) return est;
        *is_hyp = true;
        // the frame peak is a whole number of lines, so divided by the nearest
        // whole number of the hypothesis' lines it is the sharper period (the
        // refinement alone: 2560x1440 at 8 dB 2e-4 off, a third of a line a
        // frame; this moves it at most half a line a frame)
        if (acf_have_ && acf_frame_ > hyp) return acf_frame_ / std::round(acf_frame_ / hyp);
        return hyp;
    }

    // once per frame search: keep the row history at the line period to test
    // and update the measured count. A history that loses its line period
    // (the estimate gated off by a fade) keeps running ROWS_HOLD_S, so a
    // brief dropout doesn't restart it
    void track_line_count(double hyp) {
        bool is_hyp = false;
        const double L = rows_line_period(hyp, &is_hyp);
        const double t = host.time();
        if (L > 1) {
            update_rows(L);
            rows_hyp_ = is_hyp;
            rows_L_at_ = t;
        } else if (rows_L_ > 0 && t - rows_L_at_ > ROWS_HOLD_S) {
            update_rows(0);
        }
        measure_lines();
    }

    // the row history's autocorrelation (pf_rows_), recomputed when rows have
    // arrived since; false until the ring is full
    bool rows_acf() {
        if (!(rows_L_ > 0) || rows_cnt_ < static_cast<size_t>(ROWS_N)) return false;
        if (ra_seq_ >= 0 && ra_seq_ == rows_seq_) return true;
        rows_lin_.resize(ROWS_N);
        for (size_t i = 0; i < static_cast<size_t>(ROWS_N); i++) rows_lin_[i] = rows_[(rows_head_ + i) % ROWS_N];
        const video::PeriodResult rr = pf_rows_.find(rows_lin_.data(), ROWS_N, 1, ROWS_MAXLAG);
        ra_seq_ = rr.ok && pf_rows_.autocorr().size() > static_cast<size_t>(ROWS_MAXLAG) ? rows_seq_ : -1;
        return ra_seq_ >= 0;
    }

    // a count V's score: the mean of the line energies' autocorrelation at
    // V, 2V, 3V ... up to ROWS_MAXLAG, each unbiased by its overlap (-1 = none)
    double rows_score(int v) const {
        const std::vector<float>& ra = pf_rows_.autocorr();
        if (v < 1 || v > ROWS_MAXLAG || ra.size() <= static_cast<size_t>(ROWS_MAXLAG)) return -1;
        double s = 0;
        int m = 0;
        for (int lag = v; lag <= ROWS_MAXLAG; lag += v, m++)
            s += ra[static_cast<size_t>(lag)] * (static_cast<double>(ROWS_N) / (ROWS_N - lag));
        return m ? s / m : -1;
    }

    // the counts the measurement considers: Manual with a resolution, the
    // height + 1 up to 20 % blanking; else those whose frame (V lines of the
    // rows' period) falls in the frame search window - the refresh window,
    // or +-4 % around a forced / entered refresh. That keeps out 2x the true
    // count (a half refresh), which repeats as well
    bool meas_range(int& lo, int& hi) const {
        if (!(rows_L_ > 1)) return false;
        int aw = 0, ah = 0;
        manual_res(aw, ah);
        if (opt_mode_ == "manual" && ah > 0 && !std::isfinite(man_lines_)) {
            lo = ah + 1;
            hi = manual_vt_hi(ah);
        } else {
            int flo = 0, fhi = 0;
            frame_window(flo, fhi);
            lo = static_cast<int>(std::ceil(flo / rows_L_));
            hi = static_cast<int>(std::floor(fhi / rows_L_));
        }
        lo = std::max(lo, 100);
        hi = std::min({hi, MAX_VTOTAL + MAN_ALIAS_LINES, ROWS_MAXLAG / 3});   // >= 3 multiples per count
        return hi - lo >= 2;
    }

    // The measured total lines from the row history (see MEAS_PEAK_MIN): the
    // latest estimate (meas_now_) and, after MEAS_CONFIRM confident ones in a
    // row with the same count, the accepted count (meas_v_ / meas_L_). An
    // accepted count stays until a confirmed different one replaces it, the
    // line rate changes or the lock is dropped - an unsure estimate doesn't
    // undo it, so the mode doesn't flap at a marginal SNR.
    void measure_lines() {
        meas_now_ = LineCount{};
        int lo = 0, hi = 0;
        if (!meas_range(lo, hi) || !rows_acf()) return;
        msc_.resize(static_cast<size_t>(hi - lo + 3));   // lo-1 .. hi+1
        for (int v = lo - 1; v <= hi + 1; v++) msc_[static_cast<size_t>(v - lo + 1)] = rows_score(v);
        const auto S = [&](int v) { return msc_[static_cast<size_t>(v - lo + 1)]; };
        LineCount c;
        c.L = rows_L_;
        c.hyp = rows_hyp_;
        c.score = -2;
        for (int v = lo; v <= hi; v++)
            if (S(v) > c.score) { c.score = S(v); c.v = v; }
        if (c.v <= 0) return;
        c.side = std::max(S(c.v - 1), S(c.v + 1));
        // the best other peak: a local maximum with a dip of MEAS_PROM between
        // it and the count (bumps on the count's own slope don't compete - a
        // picture with large flat areas gives a broad, uneven hill)
        c.other = -1;
        for (const int dir : {-1, 1}) {
            double dip = c.score;
            for (int v = c.v + dir; v >= lo && v <= hi; v += dir) {
                if (std::abs(v - c.v) > 1 && S(v) >= S(v - 1) && S(v) >= S(v + 1) && S(v) - dip >= MEAS_PROM)
                    c.other = std::max(c.other, S(v));
                dip = std::min(dip, S(v));
            }
        }
        c.confident = c.v > lo && c.v < hi && c.score >= MEAS_PEAK_MIN && c.score - c.side >= MEAS_SIDE_MIN &&
                      c.score - c.other >= MEAS_OTHER_MIN;
        meas_now_ = c;
        // the run: each estimate counted on at least half a fresh history
        if (cand_seq_ >= 0 && rows_seq_ - cand_seq_ < ROWS_N / 2) return;
        cand_seq_ = rows_seq_;
        if (!c.confident) { cand_n_ = 0; return; }
        if (c.v == cand_v_) cand_n_++;
        else { cand_v_ = c.v; cand_n_ = 1; }
        if (cand_n_ >= MEAS_CONFIRM) {
            meas_v_ = c.v;
            meas_L_ = c.L;
            meas_hyp_ = c.hyp;
        }
    }

    void send_plots() {
        send_fps_plot(rows_L_ > 1 ? rows_L_ : estimate_line_period());
        send_lines_plot();
    }

    // Frame rate: the frame-period autocorrelation over the search window, one
    // point per refresh-rate bin (the highest lag in it, so the comb of
    // one-line-apart peaks a vertically coherent picture gives isn't aliased
    // away). Marker: the frame rate in use. A click sets `fps` (+ Manual).
    void send_fps_plot(double L) {
        kp::Plot p;
        p.id = "fps";
        p.title = "Frame rate";
        p.x_unit = "Hz";
        p.place = kp::PLOT_BESIDE_VIDEO;
        p.click_option = "fps";
        p.click_also = "mode=manual";
        p.click_step = 0.001;
        p.y_min = 0;
        const std::vector<float>& acf = pf_frame_.autocorr();
        if (acf_have_ && acf_fhi_ > acf_flo_ && acf_flo_ >= 1 && static_cast<size_t>(acf_fhi_) < acf.size()) {
            const double f_lo = fs_ / acf_fhi_, f_hi = fs_ / acf_flo_;
            const int np = PLOT_FPS_POINTS;
            const double df = (f_hi - f_lo) / (np - 1);
            p.x0 = f_lo;
            p.dx = df;
            p.y.resize(np);
            for (int i = 0; i < np; i++) {
                const double f = f_lo + i * df;
                const int k0 = std::max(acf_flo_, static_cast<int>(std::ceil(fs_ / (f + 0.5 * df))));
                const int k1 = std::min(acf_fhi_, static_cast<int>(std::floor(fs_ / std::max(1e-9, f - 0.5 * df))));
                float v = -1;
                for (int k = k0; k <= k1; k++) v = std::max(v, acf[static_cast<size_t>(k)]);
                if (k0 > k1) v = static_cast<float>(std::max(-1.0, acf_interp(acf, fs_ / f)));   // a bin narrower than a lag
                p.y[static_cast<size_t>(i)] = v;
            }
            // snap: to the highest peak within half a line period of the pointer
            const double fc = 0.5 * (f_lo + f_hi);
            p.click_snap = L > 1 ? 0.5 * fc * fc * L / fs_ : 0.03;
        }
        const double in_use = locked_ ? frame_rate_ : (opt_mode_ == "manual" && man_fps_ > 0 ? man_fps_ : NAN);
        if (std::isfinite(in_use)) {
            char b[24];
            std::snprintf(b, sizeof b, "%.3f", in_use);
            p.markers.push_back({in_use, b, ""});
        }
        host.plot(p);
    }

    // Total lines: for each count V, the mean autocorrelation of the line
    // energies at V, 2V, 3V ... lines (rows_score). Markers: the count in use,
    // and the accepted measured count where it differs (a forced mode or
    // entered lines the signal contradicts). A click sets `lines` (+ Manual),
    // snapped to the peak at whole lines.
    void send_lines_plot() {
        kp::Plot p;
        p.id = "lines";
        p.title = "Total lines";
        p.x_unit = "lines";
        p.place = kp::PLOT_BESIDE_VIDEO;
        p.click_option = "lines";
        p.click_also = "mode=manual";
        p.click_snap = 12;
        p.click_step = 1;
        p.y_min = 0;
        const double in_use = locked_ ? vtotal_ : (opt_mode_ == "manual" && man_lines_ > 0 ? std::round(man_lines_) : NAN);
        // the counts shown: +-25 % around the count in use, else the frame
        // peak's (a few lines off the true count at most); steps of 50 so the
        // axis doesn't wander with the estimate
        const double c = std::isfinite(in_use) ? in_use : (rows_L_ > 0 && acf_frame_ > 0 ? acf_frame_ / rows_L_ : 0);
        if (rows_L_ > 0 && rows_cnt_ == static_cast<size_t>(ROWS_N) && c >= 100) {
            int lo = std::max(100, static_cast<int>(std::floor(0.75 * c / 50)) * 50);
            int hi = static_cast<int>(std::ceil(1.25 * c / 50)) * 50;
            hi = std::min({hi, lo + PLOT_LINES_MAX - 1, ROWS_MAXLAG});
            if (hi > lo && rows_acf()) {
                p.x0 = lo;
                p.dx = 1;
                p.y.resize(static_cast<size_t>(hi - lo + 1));
                for (int v = lo; v <= hi; v++) p.y[static_cast<size_t>(v - lo)] = static_cast<float>(rows_score(v));
            }
        }
        if (std::isfinite(in_use)) p.markers.push_back({in_use, std::to_string(static_cast<int>(in_use)), ""});
        if (meas_v_ > 0 && !(in_use == meas_v_)) p.markers.push_back({static_cast<double>(meas_v_), std::to_string(meas_v_) + " measured", ""});
        host.plot(p);
    }

    // -------- DoA (secondary, defensive) --------
    void accumulate_cov(const cf* const* x, int m, size_t n) {
        if (!doa_ || m < 2 || m > 8 || n == 0) { cov_m_ = 0; return; }   // nothing accumulated this stretch
        // (re)size on the vector itself, not just cov_m_: DoA turned on later,
        // or a reset(), leaves covR_ empty whatever cov_m_ says
        const size_t mm = static_cast<size_t>(m) * m;
        if (cov_m_ != m || covR_.size() != mm) { cov_m_ = m; covR_.assign(mm, cd{}); cov_n_ = 0; cwin_start_ = NAN; }
        if (!std::isfinite(cwin_start_)) cwin_start_ = host.time();
        if (!locked_) return;   // never accumulate on an unlocked (possibly noise) signal

        // threshold at the block's mean combined power (subsampled); keep the
        // strong-edge samples where the leakage dominates the wideband noise
        double sp = 0;
        size_t cnt = 0;
        for (size_t i = 0; i < n; i += 16) {
            double p = 0;
            for (int k = 0; k < m; k++) p += std::norm(x[k][i]);
            sp += p;
            cnt++;
        }
        const double thr = cnt ? sp / static_cast<double>(cnt) : 0;
        for (size_t i = 0; i < n; i += 8) {
            double p = 0;
            for (int k = 0; k < m; k++) p += std::norm(x[k][i]);
            if (p <= thr) continue;
            for (int a = 0; a < m; a++) {
                const cd xa(x[a][i].real(), x[a][i].imag());
                for (int b = 0; b < m; b++) {
                    const cd xb(x[b][i].real(), x[b][i].imag());
                    covR_[static_cast<size_t>(a) * m + b] += xa * std::conj(xb);
                }
            }
            cov_n_++;
        }
    }

    void report_cov(size_t n) {
        const int m = cov_m_;
        const double end = now_end(n);
        if (m >= 2 && m <= 8 && cov_n_ >= 64 && covR_.size() == static_cast<size_t>(m) * m) {
            double tr = 0;
            for (int k = 0; k < m; k++) tr += covR_[static_cast<size_t>(k) * m + k].real();
            if (tr > 0) {
                kp::Talker t;
                t.id = "screen";
                t.label = mode_label_;
                t.start_s = cwin_start_;
                t.end_s = end;
                t.packet = true;
                t.m = m;
                t.cov.resize(static_cast<size_t>(m) * m);
                for (size_t i = 0; i < covR_.size(); i++)
                    t.cov[i] = cf(static_cast<float>(covR_[i].real() / tr), static_cast<float>(covR_[i].imag() / tr));
                t.avg_s = 4.0;
                t.keep_s = 120.0;
                host.talker(t);
            }
        }
        covR_.assign(static_cast<size_t>(m) * m, cd{});
        cov_n_ = 0;
        cwin_start_ = end;
    }
};

}  // namespace

KRAKEN_PLUGIN(Tempest,
              {.id = "tempest",
               .name = "TEMPEST screen",
               .category = "EMSEC",
               .description = "Reconstructs the picture on a nearby monitor/TV from its unintended video emanations "
                              "(Van Eck / TEMPEST): auto-detects the display mode and timing, averages many frames into "
                              "a live picture, and with the array gives a bearing to the leaking device",
               .version = "1.0",
               .sample_rate = 0,            // the VFO's own (native 2.4 MHz) rate: no resampling
               .min_vfo_rate = 2000000,
               .author = "KrakenSDR",
               .options = {{"mode", "Display mode", "auto",
                            "auto=Auto detect|640x480@60=640x480 @ 60|800x600@60=800x600 @ 60|1024x768@60=1024x768 @ 60|"
                            "1280x720@60=1280x720 @ 60|1280x1024@60=1280x1024 @ 60|1366x768@60=1366x768 @ 60|"
                            "1600x900@60=1600x900 @ 60|1920x1080@60=1920x1080 @ 60|1920x1200@60=1920x1200 @ 60|"
                            "2560x1440@60=2560x1440 @ 60 (QHD)|2560x1600@60=2560x1600 @ 60|"
                            "2560x1080@60=2560x1080 @ 60 (ultrawide)|3440x1440@60=3440x1440 @ 60 (ultrawide)|"
                            "3840x2160@30=3840x2160 @ 30 (4K)|3840x2160@60=3840x2160 @ 60 (4K)|manual=Manual",
                            "Auto detect finds the mode from the signal; pick a mode to force it (incl. 1440p/4K/ultrawide), "
                            "or Manual to enter the display's resolution and / or timing below by hand. Force the high-res modes: 2560x1440 and "
                            "3440x1440 share a line rate so Auto can report either, 4K @60 has so few samples per line that "
                            "Auto can alias it to a lower mode, and 4K @30 sits below the default refresh window (force it or "
                            "lower Refresh search min). Auto is reliable for the 640x480..1920x1080 modes"},
                           {"free_run", "Free-run preview", "1", "1=On|0=Off",
                            "Show a live, unsynced picture while still searching (snow with no signal, a rolling picture "
                            "with a weak one), like a TempestSDR while you tune - using the forced mode's geometry, else a "
                            "default, at a small preview size. Display only: it never affects detection, the lock or the "
                            "bearing. Off: a blank canvas until it locks."},
                           {"refresh_lo", "Refresh search min (Hz)", "50", "",
                            "Auto detect: the lowest frame rate to look for (the frame-period autocorrelation window)"},
                           {"refresh_hi", "Refresh search max (Hz)", "90", "",
                            "Auto detect: the highest frame rate to look for"},
                           {"width", "Manual width (px)", "", "",
                            "Manual mode: the display's active width in pixels (e.g. 1680). Set it together with the "
                            "height: the picture is then cropped to that area and shown at its shape, and the total "
                            "lines / pixels per line you leave empty are found from the signal or the standard (CVT) "
                            "timing of that resolution"},
                           {"height", "Manual height (px)", "", "",
                            "Manual mode: the display's active height in pixels (e.g. 1050); see Manual width"},
                           {"fps", "Manual frame rate (Hz)", "", "", "Manual mode: the display's refresh rate"},
                           {"lines", "Manual total lines", "", "",
                            "Manual mode: total lines per frame including blanking (vtotal). Required unless the "
                            "width and height are set (then leave it empty to have it found)"},
                           {"pixel_clock_mhz", "Manual pixel clock (MHz)", "", "",
                            "Manual mode: the dot clock, if known (sets the total pixels per line; else the standard "
                            "timing of the entered resolution, or the samples per line)"},
                           {"samples_per_line", "Manual samples/line", "", "",
                            "Manual mode: samples per line at the VFO rate, instead of deriving it from the frame rate"},
                           {"h_offset", "Horizontal nudge", "0", "",
                            "Shift the picture left/right by this many samples (fixes the blanking alignment)"},
                           {"v_offset", "Vertical nudge", "0", "", "Shift the picture up/down by this many lines"},
                           {"invert", "Invert", "0", "0=Off|1=On",
                            "Swap black and white (AM video is often negative)"},
                           {"gamma", "Gamma", "1.0", "", "Output gamma: >1 lifts the shadows, <1 deepens them"},
                           {"contrast", "Contrast", "normal", "normal=Normal|high=High|low=Low",
                            "Percentile contrast stretch: High clips harder for more punch, Low keeps the extremes"},
                           {"average", "Averaging", "0.1", "",
                            "Frame-average strength (EMA factor 0..1): smaller is steadier but slower to settle"},
                           {"autolock", "Auto-lock timing", "0", "0=Off|1=On",
                            "Freeze the detected timing so it stops re-estimating and drifting once the picture is good"},
                           {"crop", "Crop to active area", "1", "0=Full raster|1=Active only",
                            "Show only the visible picture, or the whole raster including the blanking bands"},
                           {"quality", "Picture quality", "60", "range:10:95:5",
                            "JPEG quality: lower = less bandwidth/CPU, higher = sharper."},
                           {"lossless", "Lossless", "0", "0=Off|1=On",
                            "Send the picture at maximum JPEG quality (best for a frozen / heavily averaged still). "
                            "Larger than the quality slider's setting."},
                           {"max_display_px", "Max picture size", "1280",
                            "640=640 px|960=960 px|1280=1280 px|1920=1920 px|3840=3840 px (native 4K)",
                            "Longest-side cap on the EMITTED picture only - reconstruction always uses every line and "
                            "sample of the display, so a 4K monitor is still fully received and reconstructed. Lower = less bandwidth/CPU; "
                            "3840 sends the native raster. On a build without libjpeg the raw picture is capped at ~1 MB and "
                            "larger frames are dropped."},
                           {"doa", "Bearing to source", "1", "0=Off|1=On",
                            "Coherent mode with DoA on: estimate a direction to the leaking device (shown in the DF panel). "
                            "Never affects the picture"}},
               .manual_only = true,         // must NOT run in Auto detect - it would false-trigger on anything
               .talkers = true,             // reports the leakage's bearing as one talker packet
               .lock_vfo = true,            // a whole-band decoder at the tuner's centre
               .multichannel = true,        // every antenna in coherent mode (for the DoA)
               .video = true})              // sends a reconstructed picture
