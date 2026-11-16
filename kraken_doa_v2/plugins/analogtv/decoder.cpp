// Analog TV (PAL / NTSC / SECAM): reconstruct a live grayscale LUMA picture
// from an off-air analog television vision carrier.
//
// Broadcast analog TV uses NEGATIVE amplitude modulation for the picture
// (System B/G PAL, System M NTSC, System L SECAM): the carrier is at MAXIMUM
// amplitude at the sync tips and MINIMUM at peak white. So the AM envelope
// |x| (video::Envelope) is an inverted-brightness signal - sync tips are the
// envelope MAXIMA, white is the envelope MINIMUM. This decoder therefore:
//   - locks the line / field timing from the sync pulses with
//     video::find_broadcast_sync(..., sync_is_min = false) (sync is the
//     envelope extreme = the maximum);
//   - resamples the active part of each line onto a regular raster grid
//     (video::resample) anchored to the field sync, so there is no per-line
//     horizontal jitter;
//   - weaves the two interlaced fields into one progressive frame and renders
//     it to GRAY8 with a per-frame percentile contrast stretch and the
//     brightness inverted (so minimum envelope = white). The `invert` option
//     flips that back for a baseband-composite / positive-modulation input.
//
// BANDWIDTH REALITY. A full analog channel is 6 MHz (M/NTSC) to 7-8 MHz (B/G
// PAL) with a vestigial sideband; an RTL-SDR tuner gives only a ~2.4 MHz
// slice. At 2.4 MHz the native horizontal detail is only ~150 envelope
// samples per whole line (2.4e6 / 15625 = 153.6 PAL, 2.4e6 / 15734 = 152.5
// NTSC; ~125 of those are active video), upscaled to the output width. The
// reconstruction is therefore a SOFT, recognisable luma image (large shapes,
// test-card structure, big captions) - not a sharp broadcast picture. The
// low-frequency sync pulses survive the slice intact, so locking is robust.
// This is expected; it is stated in a host.fact and the README, not hidden.
//
// Standards (ITU-R BT.470 / BT.1700), from video::tv_standards: PAL 625/50
// (15625 Hz line, 25 fps, 50 fields interlaced, ~576 active lines), NTSC
// 525/59.94 (15734.266 Hz line, 29.97 fps, ~480 active). SECAM shares PAL
// geometry - the line / frame rates can't separate it from PAL, so SECAM is
// treated as PAL geometry.
//
// Audio is out of scope for v1: the TV sound carrier sits +4.5 MHz (M) /
// +5.5 MHz (B/G) from the vision carrier, outside the 2.4 MHz slice when
// centred on the vision carrier. .voice is false; no audio is faked.
//
// Clean-room from the TV standards and the shared video-raster backbone
// (plugins/lib/video_raster.hpp); no GPL analog-TV code was consulted.

#include "kraken_plugin.hpp"
#include "video_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int OUT_W = 720;          // output width (upscaled from ~125 native active samples)
constexpr double BUILD_FPS = 15.0;  // attempt a reconstruction at most this often (live frame rate; keeps CPU low)
constexpr float FRAME_EMA = 0.6f;   // light temporal smoothing (prefer freshness; motion must not smear)

// Horizontal timing per standard (microseconds). The detected line start is
// the sync tip (the envelope maximum, i.e. the centre of the rounded sync
// notch), so the active video begins sync/2 + back-porch after it.
struct Geo {
    double active_us;        // active video duration in a line
    double h_after_tip_us;   // tip -> first active sample (= sync/2 + back porch)
};
Geo geo_for(const video::TvStandard& s) {
    const bool ntsc = std::string(s.name) == "NTSC";
    if (ntsc) return {52.856, 4.7 / 2 + 4.5};   // System M: sync 4.7us, back porch 4.5us
    return {51.95, 4.7 / 2 + 5.7};              // System B/G (PAL/SECAM): sync 4.7us, back porch 5.7us
}

double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// finite double from option text, else fallback
double num_opt(const std::string& v, double fallback) {
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    return (end != v.c_str() && std::isfinite(x)) ? x : fallback;
}

// WebP quality hint from the option: a preset or a number 1..100, else -1 (host default)
int quality_opt(const std::string& v) {
    if (v.empty() || v == "default") return -1;
    if (v == "low") return 40;
    if (v == "medium") return 65;
    if (v == "high") return 90;
    const double q = num_opt(v, NAN);
    if (!std::isfinite(q)) return -1;
    return static_cast<int>(clampd(q, 1, 100) + 0.5);
}

class AnalogTv : public kp::Decoder {
public:
    explicit AnalogTv(kp::Host& h) : Decoder(h) {}

    void process(const kp::cf* x, size_t n) override {
        if (rate_ <= 0) {
            rate_ = host.sample_rate();
            if (!(rate_ > 0)) rate_ = 2400000.0;   // an older host without sample_rate()
        }
        if (n == 0) return;

        // append the AM envelope |x| of this block to the rolling buffer
        if (env_.empty()) buf_t0_ = host.time();   // time of this block's first sample
        const size_t base = env_.size();
        env_.resize(base + n);
        envelope_.process(x, n, env_.data() + base);

        // field-size estimate for buffer bounds (nominal PAL field is the
        // largest, so this holds >= the needed fields for either standard)
        const double field_samp = rate_ / 50.0;
        const size_t max_buf = static_cast<size_t>(field_samp * 3.6);
        const size_t min_build = static_cast<size_t>(field_samp * 2.6);

        // keep the buffer bounded: drop the oldest samples in one chunk
        if (env_.size() > max_buf) {
            const size_t drop = env_.size() - static_cast<size_t>(field_samp * 2.8);
            env_.erase(env_.begin(), env_.begin() + static_cast<std::ptrdiff_t>(drop));
            buf_t0_ += static_cast<double>(drop) / rate_;
        }

        // attempt a reconstruction at most BUILD_FPS per second of input
        if (env_.size() < min_build) return;
        const double now = host.time() + static_cast<double>(n) / rate_;
        if (last_build_t_ > -1e8 && now - last_build_t_ < 1.0 / BUILD_FPS) return;
        last_build_t_ = now;
        reconstruct();
    }

    void reset() override {
        env_.clear();
        buf_t0_ = 0;
        last_build_t_ = -1e9;
        envelope_.reset();
        locked_ = false;
        locked_name_.clear();
        acc_.reset();
        acc_w_ = acc_h_ = 0;
        clear_facts();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "standard") {
            std::string v = value.empty() ? "auto" : value;
            if (v != "auto" && v != "pal" && v != "ntsc" && v != "secam") v = "auto";
            if (v != std_opt_) {
                std_opt_ = v;
                locked_ = false;
                locked_name_.clear();
                acc_.reset();
                acc_w_ = acc_h_ = 0;
                clear_facts();
            }
        } else if (key == "invert") {
            invert_ = value == "1";
        } else if (key == "deinterlace") {
            deint_ = (value == "bob" || value == "off") ? value : "weave";
            acc_.reset();
            acc_w_ = acc_h_ = 0;
        } else if (key == "contrast") {
            contrast_ = (value == "high" || value == "full") ? value : "normal";
        } else if (key == "gamma") {
            gamma_ = clampd(num_opt(value, 1.0), 0.2, 5.0);
        } else if (key == "line_adjust") {
            line_adjust_hz_ = clampd(num_opt(value, 0.0), -2000.0, 2000.0);
        } else if (key == "quality") {
            quality_ = quality_opt(value);
        } else if (key == "lossless") {
            lossless_ = value == "1";
        } else if (key == "verbose") {
            verbose_opt_ = value == "1";
        }
    }

private:
    double rate_ = 0;
    std::vector<float> env_;
    double buf_t0_ = 0;
    double last_build_t_ = -1e9;
    video::Envelope envelope_;   // plain |x|

    bool locked_ = false;
    std::string locked_name_;

    video::FrameAccumulator acc_;
    int acc_w_ = 0, acc_h_ = 0;
    std::vector<float> field_buf_;   // one field, active_field_lines x OUT_W
    std::vector<float> frame_buf_;   // woven frame, OUT_H x OUT_W
    std::vector<uint8_t> pix_;       // GRAY8 output

    // options
    std::string std_opt_ = "auto";
    bool invert_ = false;
    std::string deint_ = "weave";
    std::string contrast_ = "normal";
    double gamma_ = 1.0;
    double line_adjust_hz_ = 0;
    int quality_ = -1;        // WebP quality hint for the host (-1 = host default ~75)
    bool lossless_ = false;   // lossless WebP (frames are small; mostly a fidelity knob)
    bool verbose_opt_ = false;

    void clear_facts() {
        for (const char* k : {"Standard", "Line rate", "Field rate", "Samples/line", "Note"}) host.fact(k, "");
    }

    // The standard the signal looks like (or the forced one). nullptr = no lock.
    const video::TvStandard* pick_standard(double line_rate) const {
        size_t cnt = 0;
        const video::TvStandard* tv = video::tv_standards(&cnt);
        if (std_opt_ == "pal") return &tv[0];
        if (std_opt_ == "ntsc") return &tv[1];
        if (std_opt_ == "secam") return cnt > 2 ? &tv[2] : &tv[0];
        return video::match_tv_standard(NAN, line_rate);   // auto: by measured line rate
    }

    void reconstruct() {
        const size_t n = env_.size();
        const double nominal = std_opt_ == "ntsc" ? 15734.266 : 15625.0;
        const double approx_spl = rate_ / nominal;
        if (!(approx_spl > 2)) return;

        video::BroadcastSync sync = video::find_broadcast_sync(env_.data(), n, approx_spl, /*sync_is_min=*/false);
        if (!sync.ok || sync.line_starts.size() < 8 || sync.field_starts.size() < 2) {
            lose_lock();
            return;
        }

        // robust line period from the median spacing between detected sync tips
        std::vector<double> diffs;
        diffs.reserve(sync.line_starts.size());
        for (size_t i = 1; i < sync.line_starts.size(); i++) {
            const double d = sync.line_starts[i] - sync.line_starts[i - 1];
            if (d > 0.6 * approx_spl && d < 1.4 * approx_spl) diffs.push_back(d);
        }
        double spl_rough = sync.samples_per_line;
        if (diffs.size() >= 4) {
            std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
            spl_rough = diffs[diffs.size() / 2];
        }
        if (!(spl_rough > 2)) {
            lose_lock();
            return;
        }

        // the standard from the rough line rate (PAL vs NTSC differ by 0.7%,
        // far more than the tip-median error)
        const video::TvStandard* st = pick_standard(rate_ / spl_rough);
        if (!st) {   // not a recognisable TV line rate
            lose_lock();
            return;
        }

        const Geo g = geo_for(*st);
        const int active_field_lines = st->active_lines / 2;                 // 288 PAL / 240 NTSC
        const double fp_lines = st->total_lines / 2.0;                       // 312.5 PAL / 262.5 NTSC
        const int skip_lines = std::max(0, st->total_lines / 2 - active_field_lines);   // vertical-blank lines

        // The LINE period is derived from the FIELD period, not the per-line
        // tip medians: the vertical-sync runs are one field (fp_lines line
        // periods) apart, so their spacing / fp_lines gives a far more
        // accurate samples-per-line and cancels the diagonal shear a 0.03%
        // line-rate error would cause over ~300 lines. A bright picture trips
        // the shared detector's median sync threshold, so field_starts can
        // carry extras; the median of the diffs near one field period is robust.
        const double approx_fp = spl_rough * fp_lines;
        std::vector<double> fds;
        for (size_t i = 1; i < sync.field_starts.size(); i++) {
            const double d = sync.field_starts[i] - sync.field_starts[i - 1];
            if (d > 0.55 * approx_fp && d < 1.6 * approx_fp) fds.push_back(d);
        }
        if (fds.empty()) { lose_lock(); return; }   // no real field cadence = not a lock
        std::nth_element(fds.begin(), fds.begin() + fds.size() / 2, fds.end());
        const double fp_period = fds[fds.size() / 2];              // samples per field (measured)
        const double spl_refined = fp_period / fp_lines;
        const double spl_use = rate_ / (rate_ / spl_refined + line_adjust_hz_);
        if (!(spl_use > 2)) { lose_lock(); return; }
        const double active_samp = g.active_us * 1e-6 * rate_;
        const double h_off = g.h_after_tip_us * 1e-6 * rate_;
        const double spp = active_samp / OUT_W;

        // Field phase: take the newest vertical-sync run that repeats at the
        // field cadence, and step back exactly one field period for its
        // interlace partner - spurious starts between them are then ignored.
        const double field_span = (skip_lines + active_field_lines) * spl_use;
        const double field_rate = rate_ / fp_period;
        const double line_rate = rate_ / spl_use;
        const double tol = 0.18 * fp_period;
        auto periodic = [&](double fs) {
            for (double o : sync.field_starts) {
                if (o == fs) continue;
                const double r = std::fabs(fs - o);
                for (int k = 1; k <= 4; k++)
                    if (std::fabs(r - k * fp_period) < tol) return true;
            }
            return false;
        };
        double newer = NAN;
        int newer_idx = -1;
        for (int j = static_cast<int>(sync.field_starts.size()) - 1; j >= 0; j--) {
            const double fs = sync.field_starts[static_cast<size_t>(j)];
            if (fs + field_span <= static_cast<double>(n) - 1 && periodic(fs)) { newer = fs; newer_idx = j; break; }
        }
        if (!std::isfinite(newer)) { lose_lock(); return; }   // no real field cadence = not a lock
        const double older = newer - fp_period;

        const bool weave = deint_ == "weave" && older >= 0;
        const int out_h = (deint_ == "off") ? active_field_lines : st->active_lines;
        ensure_frame(out_h, active_field_lines);

        // Snap the active-region origin to the nearest detected line tip, so
        // the horizontal phase matches the sync grid (field_starts sit on a
        // line's leading edge, the tips the grid is built on are sync/2 + fp
        // later) and the vertical position lands on a real line boundary.
        auto snap = [&](double target) {
            if (sync.line_starts.empty()) return target;
            auto it = std::lower_bound(sync.line_starts.begin(), sync.line_starts.end(), target);
            double best = target, bd = 1e18;
            if (it != sync.line_starts.end() && std::fabs(*it - target) < bd) { bd = std::fabs(*it - target); best = *it; }
            if (it != sync.line_starts.begin()) {
                const double v = *std::prev(it);
                if (std::fabs(v - target) < bd) best = v;
            }
            return best;
        };
        auto origin_of = [&](double fs) { return snap(fs + skip_lines * spl_use); };
        auto resample_field = [&](double origin) {
            video::RasterMap m;
            m.width = OUT_W;
            m.height = active_field_lines;
            m.samples_per_line = spl_use;
            m.samples_per_pixel = spp;
            m.h_offset = h_off;
            m.origin = origin;
            video::resample(env_.data(), n, m, field_buf_.data());
        };

        if (weave) {
            // field parity (best effort); fall back to simple alternation
            int base_newer = 1, base_older = 0;
            if (sync.field_parity.size() == sync.field_starts.size() && newer_idx >= 0) {
                base_newer = sync.field_parity[static_cast<size_t>(newer_idx)] ? 1 : 0;
                base_older = 1 - base_newer;
            }
            resample_field(origin_of(older));
            weave_rows(base_older, active_field_lines);
            resample_field(origin_of(newer));
            weave_rows(base_newer, active_field_lines);
        } else if (deint_ == "off") {   // single newest field, native half height
            resample_field(origin_of(newer));
            std::copy(field_buf_.begin(), field_buf_.begin() + static_cast<std::ptrdiff_t>(active_field_lines) * OUT_W,
                      frame_buf_.begin());
        } else {   // bob (and the weave fallback before a second field arrives): stretch the newest field to full height
            video::RasterMap m;
            m.width = OUT_W;
            m.height = out_h;
            m.samples_per_line = spl_use * static_cast<double>(active_field_lines) / out_h;
            m.samples_per_pixel = spp;
            m.h_offset = h_off;
            m.origin = origin_of(newer);
            video::resample(env_.data(), n, m, frame_buf_.data());
        }

        // render with a per-frame percentile stretch; brightness inverted for
        // negative-AM broadcast (minimum envelope = white)
        acc_.add(frame_buf_.data());
        video::StretchOpts so;
        so.invert = !invert_;
        so.gamma = static_cast<float>(gamma_);
        if (contrast_ == "high") { so.lo_pct = 2.0f; so.hi_pct = 98.0f; }
        else if (contrast_ == "full") { so.lo_pct = 0.0f; so.hi_pct = 100.0f; }
        else { so.lo_pct = 0.5f; so.hi_pct = 99.5f; }
        acc_.render(pix_, so);
        if (static_cast<int>(pix_.size()) != OUT_W * out_h) return;

        char label[48];
        std::snprintf(label, sizeof label, "%s %s", st->name, st->system);
        kp::VideoFrame vf;
        vf.width = OUT_W;
        vf.height = out_h;
        vf.format = 0;   // GRAY8
        vf.pixels = pix_.data();
        vf.label = label;
        vf.quality = quality_;     // host compresses the raw pixels to WebP at this quality
        vf.lossless = lossless_;
        host.video_frame(vf);

        // facts + lock event + a valid span over the samples that built the frame
        const double px_per_line = rate_ / line_rate;
        char b[96];
        host.fact("Standard", std::string(st->name) + " " + st->system);
        std::snprintf(b, sizeof b, "%.1f Hz", line_rate);
        host.fact("Line rate", b);
        if (std::isfinite(field_rate)) {
            std::snprintf(b, sizeof b, "%.2f Hz", field_rate);
            host.fact("Field rate", b);
        }
        std::snprintf(b, sizeof b, "%.2f", spl_use);
        host.fact("Samples/line", b);
        std::snprintf(b, sizeof b, "%.1f MHz slice -> soft luma, ~%d px/line", rate_ / 1e6,
                      static_cast<int>(px_per_line + 0.5));
        host.fact("Note", b);

        const std::string name = std::string(st->name) + " " + st->system;
        if (!locked_ || locked_name_ != name) {
            host.event("Locked " + name, 0.0);
            locked_name_ = name;
        }
        locked_ = true;

        const double first = weave ? origin_of(older) : origin_of(newer);
        const double last = origin_of(newer) + active_field_lines * spl_use;
        const double s0 = buf_t0_ + clampd(first, 0, static_cast<double>(n)) / rate_;
        const double s1 = buf_t0_ + clampd(last, 0, static_cast<double>(n)) / rate_;
        if (s1 > s0) host.valid(s0, s1);

        if (verbose_opt_ && host.verbose()) {
            std::snprintf(b, sizeof b, "%s spl=%.3f line=%.1fHz field=%.2fHz fields=%zu lines=%zu", name.c_str(), spl_use,
                          line_rate, field_rate, sync.field_starts.size(), sync.line_starts.size());
            host.log(b);
        }
    }

    void ensure_frame(int out_h, int field_rows) {
        if (acc_w_ != OUT_W || acc_h_ != out_h) {
            acc_w_ = OUT_W;
            acc_h_ = out_h;
            acc_.configure(OUT_W, out_h, FRAME_EMA);
        }
        frame_buf_.assign(static_cast<size_t>(OUT_W) * out_h, 0.0f);
        const size_t fld = static_cast<size_t>(OUT_W) * field_rows;   // one resampled field
        if (field_buf_.size() < fld) field_buf_.resize(fld);
    }

    // copy a resampled field (active_field_lines rows) into every other frame
    // row starting at `base` (0 or 1)
    void weave_rows(int base, int field_lines) {
        for (int k = 0; k < field_lines; k++) {
            const int r = base + 2 * k;
            if (r >= acc_h_) break;
            std::copy(field_buf_.begin() + static_cast<std::ptrdiff_t>(k) * OUT_W,
                      field_buf_.begin() + static_cast<std::ptrdiff_t>(k + 1) * OUT_W,
                      frame_buf_.begin() + static_cast<std::ptrdiff_t>(r) * OUT_W);
        }
    }

    void lose_lock() {
        if (locked_) {
            locked_ = false;
            locked_name_.clear();
            host.fact("Standard", "searching");
            for (const char* k : {"Line rate", "Field rate", "Samples/line", "Note"}) host.fact(k, "");
        } else if (acc_.frames() == 0) {
            host.fact("Standard", "searching");
        }
    }
};

}  // namespace

KRAKEN_PLUGIN(AnalogTv,
              {.id = "analogtv",
               .name = "Analog TV",
               .category = "Broadcast & cellular",
               .description = "Off-air analog TV (PAL / NTSC / SECAM): reconstructs a live grayscale luma picture from the "
                              "vision carrier. A 2.4 MHz tuner slice gives only ~150 samples per line, so the image is a "
                              "soft, recognisable luma (large shapes, test cards, big captions), not a sharp picture. "
                              "Tune to the vision carrier; the VFO is locked to 2.4 MHz. No sound (its carrier is outside the slice).",
               .version = "1.0",
               .sample_rate = 0,            // the VFO's own rate (want the full 2.4 MHz)
               .min_vfo_rate = 2000000,
               .author = "KrakenSDR",
               .options = {{"standard", "TV standard", "auto",
                            "auto=Auto detect|pal=PAL 625/50|ntsc=NTSC 525/60|secam=SECAM (PAL geometry)",
                            "Which standard to decode. Auto picks PAL vs NTSC from the measured line rate. SECAM shares "
                            "PAL geometry and cannot be told apart by timing."},
                           {"invert", "Invert brightness", "0", "0=Off|1=On",
                            "Off: normal broadcast (negative AM - the sync tips are the strongest carrier, peak white the "
                            "weakest). On: flips black/white, for a baseband-composite or positive-modulation input."},
                           {"deinterlace", "Deinterlace", "weave",
                            "weave=Weave (both fields)|bob=Bob (one field)|off=Off (single field, half height)",
                            "Weave interleaves the two fields into a full-height frame (best detail, can comb on motion). "
                            "Bob stretches the newest field to full height (softer, no comb). Off shows one field at half height."},
                           {"contrast", "Contrast", "normal", "normal=Normal|high=High|full=Full range",
                            "Per-frame percentile stretch: Normal 0.5-99.5%, High 2-98% (punchier), Full 0-100% (no clipping)."},
                           {"gamma", "Gamma", "1.0", "",
                            "Brightness curve: >1 lifts shadows, <1 deepens them (1 = linear). Range 0.2-5."},
                           {"line_adjust", "Line nudge (Hz)", "0", "",
                            "Fine nudge to the assumed line rate (Hz) if the picture won't lock or tears diagonally. "
                            "Normally 0; try a few Hz either way."},
                           {"quality", "Picture quality", "default", "",
                            "WebP compression quality for the picture sent to the browser: a number 1-100, or "
                            "low/medium/high (40/65/90), or 'default' for the host default (~75). Higher is sharper "
                            "but larger (analog-TV frames are small either way)."},
                           {"lossless", "Lossless", "0", "0=Off|1=On",
                            "Send the picture losslessly instead of lossy WebP."}},
               .manual_only = false,
               .voice = false,
               .talkers = false,
               .lock_vfo = true,
               .video = true})
