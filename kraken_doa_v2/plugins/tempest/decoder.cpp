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
// Display / transmission ONLY: reconstruction and mode detection always use the
// monitor's full geometry (htotal_ x vtotal_ / the active area), so a 4K display
// is still fully received and reconstructed - send_video() box-downscales only
// the OUTPUT frame when it exceeds this. 1280 keeps the transmitted picture small
// and cheap to JPEG-encode; raise it (up to native 3840) for a sharper emitted
// picture, lower it for less bandwidth / CPU. On a build without libjpeg the host
// can only send RAW pixels under kp::wire::MAX_PAYLOAD (1 MiB), where a GRAY8
// frame over ~1000x1000 is dropped - keep this modest on such a build.
constexpr int DEFAULT_MAX_DISPLAY_PX = 1280;

// Above ~1440p the per-frame resample/accumulate/render cost grows with the
// output pixel count, so the accumulate and video cadences are stretched to keep
// the per-second pixel work bounded; at or below this (~3.7 Mpx) the cadence is
// unchanged, so the default-resolution performance does not regress.
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
        next_detect_ = next_acc_ = next_video_ = next_blank_ = next_free_ = 0;
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
    std::vector<float> rbuf_;   // resample scratch (one frame)
    std::vector<uint8_t> gray_, out8_;

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
    double phase_ = 0;          // global sample phase of the frame boundary, mod frame_period
    bool have_phase_ = false;
    int acc_w_ = 0, acc_h_ = 0;

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

    void drop_lock() {
        locked_ = false;
        announced_ = false;
        matched_ = false;
        have_phase_ = false;
        fails_ = 0;
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
        const size_t n = ebuf_.size();
        int flo = 0, fhi = 0;
        frame_window(flo, fhi);
        if (flo < 1) flo = 1;
        if (fhi <= flo) return;
        if (static_cast<size_t>(fhi) * 21 / 10 > n) return;   // need ~2.1 periods buffered
        if (static_cast<size_t>(fhi) >= n) fhi = static_cast<int>(n) - 1;

        const video::PeriodResult fr = pf_frame_.find(ebuf_.data(), n, flo, fhi);
        const bool manual = opt_mode_ == "manual";
        const bool frame_ok = fr.ok && fr.confidence >= FRAME_CONF_MIN && fr.peak >= FRAME_PEAK_MIN;
        if (!manual && !frame_ok) { on_fail(); return; }
        double frame_coarse = fr.ok ? fr.period : 0;
        if (!(frame_coarse > 0) && manual && std::isfinite(man_fps_) && man_fps_ > 0) frame_coarse = fs_ / man_fps_;
        if (!(frame_coarse > 0)) { on_fail(); return; }

        // the mode is chosen from the line-rate comb of this same full-buffer
        // autocorrelation (thousands of lines -> a sharp comb); the frame peak
        // is only a coarse refresh gate (it is +-1 line ambiguous under vertical
        // coherence, so it cannot fix the resolution on its own)
        if (!resolve_geometry(frame_coarse, pf_frame_.autocorr())) { on_fail(); return; }

        // success - resolve_geometry has set the geometry, frame_period_ and line_period_
        fails_ = 0;
        frame_rate_ = fs_ / frame_period_;
        if (!locked_) { next_blank_ = next_acc_ = next_video_ = host.time(); }   // start producing at once
        locked_ = true;
        publish_facts();
        maybe_announce();
    }

    void on_fail() {
        if (autolock_) return;   // frozen: ignore transient misses
        if (locked_ && ++fails_ >= LOSS_FAILS) {
            drop_lock();
            clear_facts();
        } else if (!locked_) {
            host.fact("Status", "searching for a display...");
        }
    }

    // turn the coarse frame period + the line-rate comb into a concrete raster
    // geometry, setting frame_period_ / line_period_ precisely
    bool resolve_geometry(double frame_coarse, const std::vector<float>& acf) {
        const std::string want = opt_mode_;

        if (want != "auto" && want != "manual") {   // a forced DMT mode: geometry fixed
            const video::DisplayMode* dm = mode_by_name(want);
            if (!dm || !(dm->line_rate_hz() > 0)) return false;
            const double mlp = fs_ / dm->line_rate_hz();
            set_mode(*dm, "forced", refine_spl_frame(acf, dm->vtotal, mlp));
            matched_ = true;
            match_err_ = 0;
            return true;
        }
        if (want == "manual") return resolve_manual(frame_coarse, acf);

        // auto: among the DMT modes whose refresh is near the coarse frame rate,
        // the one whose LINE-rate comb best explains the autocorrelation. The
        // line rate (pixel_clock/htotal) separates the modes cleanly.
        const double frate_coarse = fs_ / frame_coarse;
        size_t nm = 0;
        const video::DisplayMode* modes = video::display_modes(&nm);
        const video::DisplayMode* best = nullptr;
        double bestsc = -1;
        for (size_t i = 0; i < nm; i++) {
            const video::DisplayMode& m = modes[i];
            const double mr = m.actual_refresh_hz();
            if (!(mr > 0) || std::fabs(mr - frate_coarse) / frate_coarse > REFRESH_GATE) continue;
            if (!(m.line_rate_hz() > 0)) continue;
            const double sc = comb_score(acf, fs_ / m.line_rate_hz());
            if (sc > bestsc) { bestsc = sc; best = &m; }
        }
        if (best && bestsc >= LINE_PEAK_MIN) {
            const double mlp = fs_ / best->line_rate_hz();
            const double spl = refine_spl_frame(acf, best->vtotal, mlp);
            set_mode(*best, "auto", spl);
            matched_ = true;
            match_err_ = std::fabs(spl - mlp) / mlp;
            return true;
        }
        // no table mode fits: reconstruct at the native effective resolution,
        // with total lines from the (coarse) frame period and the strongest line
        // comb in the search band
        double lper = 0;
        if (!fallback_line(acf, &lper)) return false;
        const int vt = static_cast<int>(std::lround(frame_coarse / lper));
        if (vt < MIN_VTOTAL || vt > MAX_VTOTAL) return false;
        const int spl = std::max(8, static_cast<int>(std::lround(lper)));
        apply_geom(spl, vt, spl, vt, frate_coarse * vt * spl, lper);
        matched_ = false;
        match_err_ = 0;
        mode_label_ = "~" + std::to_string(spl) + "x" + std::to_string(vt) + " (estimate)";
        return true;
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

    bool resolve_manual(double frame_coarse, const std::vector<float>& acf) {
        const int vt = std::isfinite(man_lines_) ? static_cast<int>(std::lround(man_lines_)) : 0;
        if (vt < MIN_VTOTAL || vt > MAX_VTOTAL) return false;
        double spl;
        if (std::isfinite(man_spl_) && man_spl_ > 0) spl = man_spl_;
        else spl = refine_spl_frame(acf, vt, frame_coarse / vt);
        const double fperiod = spl * vt;
        const double frate = fs_ / fperiod;
        int ht = 0;
        if (std::isfinite(man_pclk_mhz_) && man_pclk_mhz_ > 0)
            ht = static_cast<int>(std::lround(man_pclk_mhz_ * 1e6 / (frate * vt)));
        if (ht < 8) ht = std::max(8, static_cast<int>(std::lround(spl)));   // htotal not otherwise known
        apply_geom(ht, vt, ht, vt, frate * vt * ht, spl);   // active unknown: show the full raster
        matched_ = false;
        match_err_ = 0;
        mode_label_ = std::to_string(vt) + " lines @ " + f1(frate) + " Hz (manual)";
        return true;
    }

    void apply_geom(int ht, int vt, int w, int h, double pclk, double spl) {
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
        if (announced_ || !matched_) return;
        if (opt_mode_ == "manual") return;
        char b[96];
        std::snprintf(b, sizeof b, "Detected %dx%d @ %.0f Hz", width_, height_, std::round(nominal_refresh_));
        host.event(b, 10.0);
        announced_ = true;
    }

    // -------- facts --------
    void publish_facts() {
        host.fact("Status", "");
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
        for (const char* k : {"Status", "Mode", "Frame rate", "Line rate", "Total lines", "Pixel clock",
                              "Resolution limit", "Tune near", "Frames averaged"})
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
    // sample of the true frame's first active pixel, and store its phase modulo
    // the frame period. Every render then anchors to that phase (not to a
    // per-call origin, whose sub-line phase changes as the buffer advances).
    void find_blanking() {
        if (htotal_ <= 0 || vtotal_ <= 0) return;
        const int W = htotal_, H = vtotal_;
        // guard against an absurd manual pixel clock; a 4K full raster is
        // 4400x2250 = 9.9M, so admit up to 12M (a ~40 MB float raster at 4K)
        if (static_cast<size_t>(W) * H > 12000000) return;
        const double spl = frame_period_ / vtotal_;
        const double spp = spl / htotal_;
        const double origin = scan_origin();
        rbuf_.assign(static_cast<size_t>(W) * H, 0.0f);
        video::RasterMap m;
        m.width = W;
        m.height = H;
        m.samples_per_line = spl;
        m.samples_per_pixel = spp;
        m.origin = origin;
        video::resample(ebuf_.data(), ebuf_.size(), m, rbuf_.data());
        const int bw = std::max(1, htotal_ - width_);
        const int bh = std::max(1, vtotal_ - height_);
        const video::BlankOffset bo = video::find_blanking_offset(rbuf_.data(), W, H, bw, bh);
        if (!bo.ok) return;
        // true active (0,0) in this raster, then its global sample and phase
        const int v_eff = ((bo.v_offset + bh) % vtotal_ + vtotal_) % vtotal_;
        const int h_eff = ((bo.h_offset + bw) % htotal_ + htotal_) % htotal_;
        const double fs0 = static_cast<double>(buf_base_) + origin + v_eff * spl + h_eff * spp;
        phase_ = fs0 - std::floor(fs0 / frame_period_) * frame_period_;   // in [0, frame_period)
        have_phase_ = true;
    }

    // the pixel count the reconstruction works on (what resample/render cost
    // scales with): the active area when cropping, else the full raster
    double recon_px() const {
        const bool crop = crop_ && width_ > 0 && height_ > 0 && (width_ < htotal_ || height_ < vtotal_);
        const double W = crop ? width_ : htotal_;
        const double H = crop ? height_ : vtotal_;
        return W * H;
    }
    // >1 stretches the accumulate / video cadence for large (>1440p) rasters so
    // the per-second pixel work stays bounded; 1 at or below ~1440p (no change)
    double cadence_factor() const {
        const double f = recon_px() / CADENCE_PX_REF;
        return f > 1.0 ? f : 1.0;
    }

    // -------- reconstruction --------
    void add_frame() {
        if (htotal_ <= 0 || vtotal_ <= 0 || !have_phase_) return;
        const bool crop = crop_ && width_ > 0 && height_ > 0 && (width_ < htotal_ || height_ < vtotal_);
        const int W = crop ? width_ : htotal_;
        const int H = crop ? height_ : vtotal_;
        if (W < 2 || H < 2) return;
        if (acc_w_ != W || acc_h_ != H) {
            acc_.configure(W, H, static_cast<float>(ema_));
            acc_w_ = W;
            acc_h_ = H;
        }
        const double spl = frame_period_ / vtotal_;
        const double spp = spl / htotal_;
        // the most recent frame boundary at least two frames back
        const double target = static_cast<double>(total_) - 2 * frame_period_ - 3 * spl;
        const double k = std::floor((target - phase_) / frame_period_);
        double framestart = phase_ + k * frame_period_ - static_cast<double>(buf_base_);   // buffer-local
        if (framestart < 0) framestart = 0;
        video::RasterMap m;
        m.width = W;
        m.height = H;
        m.samples_per_line = spl;
        m.samples_per_pixel = spp;
        m.origin = framestart;
        m.h_offset = man_hoff_ * spp;
        m.v_offset = man_voff_;
        rbuf_.assign(static_cast<size_t>(W) * H, 0.0f);
        video::resample(ebuf_.data(), ebuf_.size(), m, rbuf_.data());
        acc_.add(rbuf_.data(), 3);   // small shift search to absorb residual phase creep
    }

    void send_video() {
        if (acc_.frames() == 0 || acc_w_ < 2 || acc_h_ < 2) return;
        video::StretchOpts so;
        so.lo_pct = lo_pct_;
        so.hi_pct = hi_pct_;
        so.invert = invert_;
        so.gamma = static_cast<float>(gamma_);
        acc_.render(gray_, so);
        if (gray_.size() != static_cast<size_t>(acc_w_) * acc_h_) return;

        int W = acc_w_, H = acc_h_;
        const uint8_t* px = gray_.data();
        // The host compresses to JPEG. max_display_px caps the LONGEST side of
        // the EMITTED frame ONLY (display / transmission): the reconstruction
        // above (gray_, acc_w_ x acc_h_) always ran at the display's full
        // geometry, so a 4K monitor is still received and reconstructed in full -
        // we box-downscale only this output. Default 1280; raise for a sharper
        // picture, lower for less CPU / on a no-libjpeg build (raw pixels only,
        // 1 MiB wire cap). Box-downscale past the cap.
        const int cap = max_display_px_ > 0 ? max_display_px_ : DEFAULT_MAX_DISPLAY_PX;
        if (std::max(W, H) > cap) {
            const int d = (std::max(W, H) + cap - 1) / cap;
            const int ow = (W + d - 1) / d, oh = (H + d - 1) / d;
            out8_.assign(static_cast<size_t>(ow) * oh, 0);
            for (int r = 0; r < oh; r++)
                for (int c = 0; c < ow; c++) {
                    unsigned sum = 0, cnt = 0;
                    for (int dr = 0; dr < d; dr++)
                        for (int dc = 0; dc < d; dc++) {
                            const int sr = r * d + dr, sc = c * d + dc;
                            if (sr < H && sc < W) { sum += gray_[static_cast<size_t>(sr) * W + sc]; cnt++; }
                        }
                    out8_[static_cast<size_t>(r) * ow + c] = cnt ? static_cast<uint8_t>(sum / cnt) : 0;
                }
            W = ow;
            H = oh;
            px = out8_.data();
        }
        host.fact("Frames averaged", std::to_string(acc_.frames()));
        kp::VideoFrame f;
        f.width = W;
        f.height = H;
        f.format = 0;   // GRAY8
        f.pixels = px;
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

    // -------- DoA (secondary, defensive) --------
    void accumulate_cov(const cf* const* x, int m, size_t n) {
        if (!doa_ || m < 2 || m > 8 || n == 0) { cov_m_ = m; return; }
        if (cov_m_ != m) { cov_m_ = m; covR_.assign(static_cast<size_t>(m) * m, cd{}); cov_n_ = 0; cwin_start_ = NAN; }
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
                            "or Manual to set the timing fields below by hand. Force the high-res modes: 2560x1440 and "
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
                           {"fps", "Manual frame rate (Hz)", "", "", "Manual mode: the display's refresh rate"},
                           {"lines", "Manual total lines", "", "",
                            "Manual mode: total lines per frame including blanking (vtotal)"},
                           {"pixel_clock_mhz", "Manual pixel clock (MHz)", "", "",
                            "Manual mode: the dot clock, if known (sets the pixels per line)"},
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
                            "Longest-side cap on the EMITTED picture only - reconstruction always uses the display's full "
                            "geometry, so a 4K monitor is still fully received and reconstructed. Lower = less bandwidth/CPU; "
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
