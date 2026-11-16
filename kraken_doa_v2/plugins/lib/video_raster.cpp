#include "video_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kraken_fft.hpp"   // pffft behind FFTW-like plans (sdk/)

namespace video {

namespace {

// is v a usable finite number?
inline bool finite(double v) { return std::isfinite(v); }

// smallest transform size >= n that pffft takes directly (2/3/5-smooth,
// multiple of 16). Period finding zero-pads to such a size so the FFT is the
// fast direct path, not Bluestein.
int next_fast_size(int n) {
    if (n < 16) n = 16;
    while (!kfft_direct(n)) n++;   // valid sizes are dense; this ends quickly
    return n;
}

}  // namespace

// === 1. AM envelope ========================================================
void Envelope::process(const std::complex<float>* x, size_t n, float* out) {
    for (size_t i = 0; i < n; i++) {
        const float re = x[i].real(), im = x[i].imag();
        float m = std::sqrt(re * re + im * im);   // |x| (std:: float overload)
        if (!primed_) {
            mean_ = m;
            scale_ = m > 0 ? m : 1.0f;
            primed_ = true;
        }
        float v = m;
        if (cfg_.dc_block) {
            mean_ += cfg_.dc_rate * (m - mean_);
            v = m - mean_;
        }
        if (cfg_.agc) {
            // track the slow magnitude of the (dc-blocked) signal and divide
            scale_ += cfg_.agc_rate * (std::fabs(v) - scale_);
            const float g = scale_ > 1e-12f ? scale_ : 1e-12f;
            v /= g;
        }
        out[i] = v;
    }
}

// === 2. Autocorrelation period finder ======================================
struct PeriodFinder::Impl {
    struct Plans {
        int N = 0;
        kfft_plan fwd = nullptr;   // r2c of size N
        kfft_plan bwd = nullptr;   // c2c backward of size N
    };
    std::vector<Plans> cache;

    int cap = 0;                   // allocated buffer length (samples)
    float* rin = nullptr;          // real input, zero-padded
    kfft_complex* spec = nullptr;  // r2c output (N/2+1 bins used)
    kfft_complex* full = nullptr;  // full two-sided power spectrum
    kfft_complex* acf = nullptr;   // backward output (real autocorrelation)

    ~Impl() {
        for (auto& p : cache) {
            kfft_destroy_plan(p.fwd);
            kfft_destroy_plan(p.bwd);
        }
        kfft_free(rin);
        kfft_free(spec);
        kfft_free(full);
        kfft_free(acf);
    }
    void ensure(int N) {
        if (N <= cap) return;
        kfft_free(rin);
        kfft_free(spec);
        kfft_free(full);
        kfft_free(acf);
        rin = kfft_alloc_real(static_cast<size_t>(N));
        spec = kfft_alloc_complex(static_cast<size_t>(N));
        full = kfft_alloc_complex(static_cast<size_t>(N));
        acf = kfft_alloc_complex(static_cast<size_t>(N));
        cap = N;
    }
    Plans& plans_for(int N) {
        for (auto& p : cache)
            if (p.N == N) return p;
        Plans p;
        p.N = N;
        // bound arrays are irrelevant: executed with explicit-array execute,
        // so a later buffer realloc can't invalidate the plan
        p.fwd = kfft_plan_dft_r2c_1d(N, rin, spec);
        p.bwd = kfft_plan_dft_1d(N, full, acf, KFFT_BACKWARD);
        cache.push_back(p);
        return cache.back();
    }
};

PeriodFinder::PeriodFinder() : impl_(std::make_unique<Impl>()) {}
PeriodFinder::~PeriodFinder() = default;

PeriodResult PeriodFinder::find(const float* x, size_t n, int min_lag, int max_lag) {
    PeriodResult r;
    if (!x || n < 4 || min_lag < 1 || max_lag < min_lag || static_cast<size_t>(max_lag) >= n) return r;

    const int N = next_fast_size(static_cast<int>(n) + max_lag);
    impl_->ensure(N);
    Impl::Plans& pl = impl_->plans_for(N);

    // mean-removed, zero-padded real input
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += x[i];
    mean /= static_cast<double>(n);
    for (size_t i = 0; i < n; i++) impl_->rin[i] = static_cast<float>(x[i] - mean);
    std::memset(impl_->rin + n, 0, sizeof(float) * static_cast<size_t>(N - static_cast<int>(n)));

    kfft_execute_dft_r2c(pl.fwd, impl_->rin, impl_->spec);

    // power spectrum |X|^2 -> full two-sided (real, even-symmetric)
    const int half = N / 2;
    auto pw = [&](int k) {
        const float re = impl_->spec[k][0], im = impl_->spec[k][1];
        return re * re + im * im;
    };
    impl_->full[0][0] = pw(0);
    impl_->full[0][1] = 0;
    for (int k = 1; k < half; k++) {
        const float p = pw(k);
        impl_->full[k][0] = p;
        impl_->full[k][1] = 0;
        impl_->full[N - k][0] = p;
        impl_->full[N - k][1] = 0;
    }
    impl_->full[half][0] = pw(half);
    impl_->full[half][1] = 0;

    // backward FFT -> autocorrelation (real part); normalize by lag 0
    kfft_execute_dft(pl.bwd, impl_->full, impl_->acf);
    const double r0 = impl_->acf[0][0];
    if (!(r0 > 0)) return r;   // flat / zero signal

    acf_.assign(static_cast<size_t>(max_lag) + 1, 0.0f);
    for (int m = 0; m <= max_lag; m++)
        acf_[static_cast<size_t>(m)] = static_cast<float>(impl_->acf[m][0] / r0);

    // integer peak in [min_lag, max_lag]
    int k = min_lag;
    for (int m = min_lag; m <= max_lag; m++)
        if (acf_[static_cast<size_t>(m)] > acf_[static_cast<size_t>(k)]) k = m;
    const double peak = acf_[static_cast<size_t>(k)];

    // parabolic sub-sample refinement
    double frac = 0;
    if (k > min_lag && k < max_lag) {
        const double a = acf_[static_cast<size_t>(k - 1)], b = peak, c = acf_[static_cast<size_t>(k + 1)];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) frac = 0.5 * (a - c) / den;
        if (frac > 1 || frac < -1) frac = 0;
    }

    // prominence: peak above the higher of the two troughs bounding it
    double left = peak, right = peak;
    for (int m = min_lag; m <= k; m++) left = std::min(left, static_cast<double>(acf_[static_cast<size_t>(m)]));
    for (int m = k; m <= max_lag; m++) right = std::min(right, static_cast<double>(acf_[static_cast<size_t>(m)]));
    double prom = peak - std::max(left, right);
    if (prom < 0) prom = 0;
    if (prom > 1) prom = 1;

    r.ok = true;
    r.period = static_cast<double>(k) + frac;
    r.peak = peak;
    r.confidence = prom;
    return r;
}

// === 3a. Analog TV standards (ITU-R BT.470 / BT.1700) ======================
namespace {
constexpr double NTSC_FR = 30000.0 / 1001.0;            // 29.970 fps
const TvStandard kTv[] = {
    {"PAL", "625/50", 625, 576, 25.0, 50.0, 15625.0, true, 2},
    {"NTSC", "525/60", 525, 480, NTSC_FR, 2 * NTSC_FR, 525.0 * NTSC_FR, true, 2},  // line 15734.27 Hz
    {"SECAM", "625/50", 625, 576, 25.0, 50.0, 15625.0, true, 2},
};
}  // namespace

const TvStandard* tv_standards(size_t* count) {
    if (count) *count = sizeof(kTv) / sizeof(kTv[0]);
    return kTv;
}

const TvStandard* match_tv_standard(double frame_rate_hz, double line_rate_hz) {
    const bool have_fr = finite(frame_rate_hz) && frame_rate_hz > 0;
    const bool have_lr = finite(line_rate_hz) && line_rate_hz > 0;
    if (!have_fr && !have_lr) return nullptr;
    const TvStandard* best = nullptr;
    double best_e = 0.08;   // reject beyond ~8% off
    for (const TvStandard& s : kTv) {
        double e = 0;
        int terms = 0;
        if (have_fr) {
            e += std::fabs(s.frame_rate_hz - frame_rate_hz) / frame_rate_hz;
            terms++;
        }
        if (have_lr) {
            e += std::fabs(s.line_rate_hz - line_rate_hz) / line_rate_hz;
            terms++;
        }
        e /= terms;
        if (e < best_e) {
            best_e = e;
            best = &s;
        }
    }
    return best;
}

// === 3b. PC display modes (VESA DMT) =======================================
namespace {
// width, height, nominal-refresh, htotal, vtotal, pixel-clock(Hz), name.
// Published DMT dot clocks; pixel_clock == htotal*vtotal*actual_refresh.
const DisplayMode kModes[] = {
    {640, 480, 60, 800, 525, 25175000.0, "640x480@60"},
    {640, 480, 72, 832, 520, 31500000.0, "640x480@72"},
    {640, 480, 75, 840, 500, 31500000.0, "640x480@75"},
    {800, 600, 60, 1056, 628, 40000000.0, "800x600@60"},
    {800, 600, 72, 1040, 666, 50000000.0, "800x600@72"},
    {800, 600, 75, 1056, 625, 49500000.0, "800x600@75"},
    {1024, 768, 60, 1344, 806, 65000000.0, "1024x768@60"},
    {1024, 768, 70, 1328, 806, 75000000.0, "1024x768@70"},
    {1024, 768, 75, 1312, 800, 78750000.0, "1024x768@75"},
    {1280, 720, 60, 1650, 750, 74250000.0, "1280x720@60"},
    {1280, 1024, 60, 1688, 1066, 108000000.0, "1280x1024@60"},
    {1280, 1024, 75, 1688, 1066, 135000000.0, "1280x1024@75"},
    {1366, 768, 60, 1792, 798, 85500000.0, "1366x768@60"},
    {1440, 900, 60, 1904, 934, 106500000.0, "1440x900@60"},
    {1600, 900, 60, 1800, 1000, 108000000.0, "1600x900@60"},   // CVT reduced-blanking
    {1680, 1050, 60, 2240, 1089, 146250000.0, "1680x1050@60"},
    {1920, 1080, 60, 2200, 1125, 148500000.0, "1920x1080@60"},
    // --- high-resolution / QHD / 4K / ultrawide (VESA CVT reduced-blanking v1
    //     and CTA-861) ---
    // Each pixel_clock == htotal*vtotal*actual_refresh. The five CVT-RB rows
    // below are reproduced exactly by cvt_mode(w, h, 60, /*reduced=*/true) (the
    // generator in this file), i.e. they are the published reduced-blanking
    // timings; the two 4K rows are the CTA-861 UHD timings (htotal 4400, vtotal
    // 2250; 594.0 MHz for 60 Hz, half that for 30 Hz). Keep this ORDER: on a
    // line-rate tie the TEMPEST auto-detector takes the first match, and
    // 2560x1440 and 3440x1440 share a line rate (both vtotal 1481, ~88.8 kHz),
    // so the 16:9 QHD mode must precede the ultrawide for the common case.
    {1920, 1200, 60, 2080, 1235, 154000000.0, "1920x1200@60"},   // CVT-RB, 59.95 Hz
    {2560, 1440, 60, 2720, 1481, 241500000.0, "2560x1440@60"},   // CVT-RB (QHD), 59.95 Hz
    {2560, 1600, 60, 2720, 1646, 268500000.0, "2560x1600@60"},   // CVT-RB (WQXGA), 59.97 Hz
    {2560, 1080, 60, 2720, 1111, 181250000.0, "2560x1080@60"},   // CVT-RB ultrawide, 59.98 Hz
    {3440, 1440, 60, 3600, 1481, 319750000.0, "3440x1440@60"},   // CVT-RB ultrawide (UWQHD), 59.97 Hz
    {3840, 2160, 30, 4400, 2250, 297000000.0, "3840x2160@30"},   // CTA-861 4K UHD @30, 30.00 Hz
    {3840, 2160, 60, 4400, 2250, 594000000.0, "3840x2160@60"},   // CTA-861 4K UHD @60, 60.00 Hz
};
}  // namespace

const DisplayMode* display_modes(size_t* count) {
    if (count) *count = sizeof(kModes) / sizeof(kModes[0]);
    return kModes;
}

DisplayMode cvt_mode(int w, int h, double refresh, bool reduced) {
    DisplayMode d;
    d.width = w;
    d.height = h;
    d.refresh_hz = refresh;
    d.name = "";
    if (w <= 0 || h <= 0 || !(refresh > 0)) return d;

    const double CELL = 8.0;         // character cell granularity (pixels)
    const double CLOCK_STEP = 0.25;  // dot-clock step (MHz)
    const int hdisp = static_cast<int>(std::floor(w / CELL)) * static_cast<int>(CELL);
    const int vdisp = h;

    if (reduced) {
        // CVT reduced-blanking v1: fixed 160-pixel horizontal blank, 460 us
        // minimum vertical blank.
        const double RB_H_BLANK = 160.0, RB_MIN_V_BLANK = 460.0;  // pixels / us
        const int htotal = hdisp + static_cast<int>(RB_H_BLANK);
        const double hperiod = ((1.0 / refresh) - RB_MIN_V_BLANK / 1e6) / vdisp * 1e6;  // us/line
        if (!(hperiod > 0)) return d;
        const int vbi = static_cast<int>(std::floor(RB_MIN_V_BLANK / hperiod)) + 1;
        const int vtotal = vdisp + vbi;
        const double pclk_mhz =
            CLOCK_STEP * std::floor(static_cast<double>(htotal) * vtotal * refresh / 1e6 / CLOCK_STEP);
        d.htotal = htotal;
        d.vtotal = vtotal;
        d.pixel_clock_hz = pclk_mhz * 1e6;
        return d;
    }

    // CVT standard blanking (GTF-derived duty-cycle model).
    const double MIN_VSYNC_BP = 550.0;  // us (vsync + back porch minimum)
    const int MIN_V_PORCH = 3;          // front-porch lines
    const int MIN_V_BPORCH = 6;         // back-porch lines
    const double C_PRIME = 30.0, M_PRIME = 300.0;  // blanking duty model
    int vsync = 10;                     // vsync lines by aspect ratio
    if (w * 3 == h * 4) vsync = 4;
    else if (w * 9 == h * 16) vsync = 5;
    else if (w * 10 == h * 16) vsync = 6;
    else if (w * 4 == h * 5) vsync = 7;
    else if (w * 9 == h * 15) vsync = 7;

    const double hperiod = ((1.0 / refresh) - MIN_VSYNC_BP / 1e6) / (vdisp + MIN_V_PORCH) * 1e6;  // us/line
    if (!(hperiod > 0)) return d;
    int vsync_bp = static_cast<int>(std::floor(MIN_VSYNC_BP / hperiod)) + 1;
    if (vsync_bp < vsync + MIN_V_BPORCH) vsync_bp = vsync + MIN_V_BPORCH;
    const int vtotal = vdisp + MIN_V_PORCH + vsync_bp;

    double duty = C_PRIME - M_PRIME * hperiod / 1000.0;
    if (duty < 20.0) duty = 20.0;
    const double hblank = std::floor(hdisp * duty / (100.0 - duty) / (2 * CELL)) * (2 * CELL);
    const int htotal = hdisp + static_cast<int>(hblank);
    const double pclk_mhz = CLOCK_STEP * std::floor((htotal / hperiod) / CLOCK_STEP);

    d.htotal = htotal;
    d.vtotal = vtotal;
    d.pixel_clock_hz = pclk_mhz * 1e6;
    return d;
}

std::vector<ModeMatch> match_display_modes(const ModeQuery& q, int max_results) {
    const bool have_fr = finite(q.frame_rate_hz) && q.frame_rate_hz > 0;
    const bool have_tl = finite(q.total_lines) && q.total_lines > 0;
    const bool have_lr = finite(q.line_rate_hz) && q.line_rate_hz > 0;
    const bool have_pc = finite(q.pixel_clock_hz) && q.pixel_clock_hz > 0;

    std::vector<ModeMatch> out;
    if (!have_fr && !have_tl && !have_lr && !have_pc) return out;

    for (const DisplayMode& m : kModes) {
        double e = 0;
        int terms = 0;
        if (have_fr) {
            e += std::fabs(m.actual_refresh_hz() - q.frame_rate_hz) / q.frame_rate_hz;
            terms++;
        }
        if (have_tl) {
            e += std::fabs(static_cast<double>(m.vtotal) - q.total_lines) / q.total_lines;
            terms++;
        }
        if (have_lr) {
            e += std::fabs(m.line_rate_hz() - q.line_rate_hz) / q.line_rate_hz;
            terms++;
        }
        if (have_pc) {
            e += std::fabs(m.pixel_clock_hz - q.pixel_clock_hz) / q.pixel_clock_hz;
            terms++;
        }
        out.push_back({m, e / terms});
    }
    std::sort(out.begin(), out.end(), [](const ModeMatch& a, const ModeMatch& b) { return a.error < b.error; });
    if (max_results > 0 && static_cast<int>(out.size()) > max_results) out.resize(static_cast<size_t>(max_results));
    return out;
}

// === 4. Raster resampler ===================================================
void resample(const float* env, size_t n, const RasterMap& m, float* out) {
    if (!env || !out || m.width <= 0 || m.height <= 0 || !(m.samples_per_line > 0)) return;
    const double spp = m.samples_per_pixel > 0 ? m.samples_per_pixel : m.samples_per_line / m.width;
    const double nmax = static_cast<double>(n) - 1.0;
    for (int r = 0; r < m.height; r++) {
        const double drift = m.line_drift * (0.5 * r * (r - 1));   // accumulated shear
        const double line_start = m.origin + m.h_offset + (m.v_offset + r) * m.samples_per_line + drift;
        float* row = out + static_cast<size_t>(r) * m.width;
        double pos = line_start;
        for (int c = 0; c < m.width; c++, pos += spp) {
            float v = 0;
            if (pos >= 0 && pos <= nmax) {
                const int64_t i = static_cast<int64_t>(pos);
                const double f = pos - static_cast<double>(i);
                const float a = env[i];
                const float b = (static_cast<size_t>(i) + 1 < n) ? env[i + 1] : a;
                v = static_cast<float>(a + (b - a) * f);
            }
            row[c] = v;
        }
    }
}

// === 5. Frame accumulator & render =========================================
void FrameAccumulator::configure(int width, int height, float ema, int boxcar_n) {
    w_ = width > 0 ? width : 0;
    h_ = height > 0 ? height : 0;
    ema_ = (ema > 0 && ema <= 1) ? ema : 0.1f;
    boxcar_ = boxcar_n > 0 ? boxcar_n : 0;
    reset();
}

void FrameAccumulator::reset() {
    frames_ = 0;
    ring_pos_ = 0;
    ring_filled_ = 0;
    const size_t px = static_cast<size_t>(w_) * h_;
    avg_.assign(px, 0.0f);
    shifted_.assign(px, 0.0f);
    if (boxcar_ > 0) {
        ring_.assign(px * static_cast<size_t>(boxcar_), 0.0f);
        sum_.assign(px, 0.0f);
    } else {
        ring_.clear();
        sum_.clear();
    }
}

void FrameAccumulator::best_shift(const float* frame, int max_shift, int* dx, int* dy) const {
    *dx = 0;
    *dy = 0;
    const int m = max_shift;
    const int r0 = m, r1 = h_ - 1 - m, c0 = m, c1 = w_ - 1 - m;
    if (r1 < r0 || c1 < c0) return;
    const int step = std::max(1, std::min(w_, h_) / 64);
    // reference (avg_) grid stats, constant over shifts
    double sa = 0, saa = 0;
    int64_t cnt = 0;
    for (int r = r0; r <= r1; r += step)
        for (int c = c0; c <= c1; c += step) {
            const float a = avg_[static_cast<size_t>(r) * w_ + c];
            sa += a;
            saa += static_cast<double>(a) * a;
            cnt++;
        }
    if (cnt < 4) return;
    const double amean = sa / cnt;
    const double avar = saa / cnt - amean * amean;
    if (!(avar > 0)) return;

    double best = -1e300;
    for (int ddy = -m; ddy <= m; ddy++)
        for (int ddx = -m; ddx <= m; ddx++) {
            double sf = 0, sff = 0, sfa = 0;
            for (int r = r0; r <= r1; r += step)
                for (int c = c0; c <= c1; c += step) {
                    const float f = frame[static_cast<size_t>(r + ddy) * w_ + (c + ddx)];
                    const float a = avg_[static_cast<size_t>(r) * w_ + c];
                    sf += f;
                    sff += static_cast<double>(f) * f;
                    sfa += static_cast<double>(f) * a;
                }
            const double fmean = sf / cnt;
            const double fvar = sff / cnt - fmean * fmean;
            const double cov = sfa / cnt - fmean * amean;
            const double score = cov / std::sqrt(fvar * avar + 1e-12);
            if (score > best) {
                best = score;
                *dx = ddx;
                *dy = ddy;
            }
        }
}

void FrameAccumulator::push(const float* frame) {
    const size_t px = static_cast<size_t>(w_) * h_;
    if (boxcar_ > 0) {
        float* slot = &ring_[static_cast<size_t>(ring_pos_) * px];
        if (ring_filled_ == boxcar_)
            for (size_t i = 0; i < px; i++) sum_[i] -= slot[i];   // evict oldest
        std::memcpy(slot, frame, sizeof(float) * px);
        for (size_t i = 0; i < px; i++) sum_[i] += slot[i];
        ring_pos_ = (ring_pos_ + 1) % boxcar_;
        if (ring_filled_ < boxcar_) ring_filled_++;
        const float inv = 1.0f / static_cast<float>(ring_filled_);
        for (size_t i = 0; i < px; i++) avg_[i] = sum_[i] * inv;
    } else {
        if (frames_ == 0)
            std::memcpy(avg_.data(), frame, sizeof(float) * px);
        else
            for (size_t i = 0; i < px; i++) avg_[i] += ema_ * (frame[i] - avg_[i]);
    }
    frames_++;
}

void FrameAccumulator::add(const float* frame, int max_shift, int* out_dx, int* out_dy) {
    if (!frame || w_ <= 0 || h_ <= 0) return;
    int sdx = 0, sdy = 0;
    if (frames_ > 0 && max_shift > 0) best_shift(frame, max_shift, &sdx, &sdy);
    if (out_dx) *out_dx = sdx;
    if (out_dy) *out_dy = sdy;

    const float* f = frame;
    if (sdx != 0 || sdy != 0) {
        // frame content sits (sdx,sdy) off the average: sample it shifted back
        for (int r = 0; r < h_; r++)
            for (int c = 0; c < w_; c++) {
                const int sr = r + sdy, sc = c + sdx;
                shifted_[static_cast<size_t>(r) * w_ + c] =
                    (sr >= 0 && sr < h_ && sc >= 0 && sc < w_) ? frame[static_cast<size_t>(sr) * w_ + sc] : 0.0f;
            }
        f = shifted_.data();
    }
    push(f);
}

void FrameAccumulator::render(std::vector<uint8_t>& out, const StretchOpts& o) const {
    const size_t px = static_cast<size_t>(w_) * h_;
    if (frames_ == 0 || px == 0) {
        out.clear();
        return;
    }
    // range of the finite values
    float lo = 0, hi = 0;
    bool any = false;
    for (size_t i = 0; i < px; i++) {
        const float v = avg_[i];
        if (!std::isfinite(v)) continue;
        if (!any) {
            lo = hi = v;
            any = true;
        } else {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    out.assign(px, 0);
    if (!any || !(hi > lo)) return;

    // histogram -> lo / hi percentile values
    constexpr int NB = 4096;
    int hist[NB] = {0};
    const double scale = (NB - 1) / static_cast<double>(hi - lo);
    long long total = 0;
    for (size_t i = 0; i < px; i++) {
        const float v = avg_[i];
        if (!std::isfinite(v)) continue;
        int b = static_cast<int>((v - lo) * scale);
        if (b < 0) b = 0;
        if (b >= NB) b = NB - 1;
        hist[b]++;
        total++;
    }
    if (total == 0) return;
    auto pct_value = [&](float pct) {
        const long long want = static_cast<long long>(pct / 100.0f * total);
        long long acc = 0;
        for (int b = 0; b < NB; b++) {
            acc += hist[b];
            if (acc >= want) return lo + static_cast<float>((b + 0.5) / scale);
        }
        return hi;
    };
    float plo = pct_value(o.lo_pct), phi = pct_value(o.hi_pct);
    if (!(phi > plo)) {
        plo = lo;
        phi = hi;
        if (!(phi > plo)) return;
    }
    const float inv = 1.0f / (phi - plo);
    const bool do_gamma = std::isfinite(o.gamma) && o.gamma > 0 && std::fabs(o.gamma - 1.0f) > 1e-3f;
    const float ginv = do_gamma ? 1.0f / o.gamma : 1.0f;
    for (size_t i = 0; i < px; i++) {
        float t = (avg_[i] - plo) * inv;
        if (!std::isfinite(t)) t = 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        if (o.invert) t = 1.0f - t;
        if (do_gamma) t = std::pow(t, ginv);
        out[i] = static_cast<uint8_t>(t * 255.0f + 0.5f);
    }
}

// === 6a. Broadcast H/V sync ================================================
BroadcastSync find_broadcast_sync(const float* env, size_t n, double approx_spl, bool sync_is_min) {
    BroadcastSync out;
    if (!env || n < 16 || !(approx_spl > 2) || static_cast<double>(n) < approx_spl * 2) return out;

    // orient so sync is the MINIMUM
    std::vector<float> sig(n);
    for (size_t i = 0; i < n; i++) sig[i] = sync_is_min ? env[i] : -env[i];

    // level estimate: sync tip ~ low end, blanking ~ a robust higher reference
    float lo = sig[0], hi = sig[0];
    for (size_t i = 1; i < n; i++) {
        lo = std::min(lo, sig[i]);
        hi = std::max(hi, sig[i]);
    }
    // sliding minimum per line to find the sync tips; track drift
    const double win = approx_spl * 0.35;   // +- search window after warm-up
    auto tip_in = [&](double from, double to) -> double {
        int64_t a = std::max<int64_t>(0, static_cast<int64_t>(std::floor(from)));
        int64_t b = std::min<int64_t>(static_cast<int64_t>(n) - 1, static_cast<int64_t>(std::ceil(to)));
        if (b <= a) return -1;
        int64_t bi = a;
        for (int64_t i = a + 1; i <= b; i++)
            if (sig[i] < sig[bi]) bi = i;
        // parabolic refine
        double frac = 0;
        if (bi > 0 && bi + 1 < static_cast<int64_t>(n)) {
            const double y0 = sig[bi - 1], y1 = sig[bi], y2 = sig[bi + 1];
            const double den = y0 - 2 * y1 + y2;
            if (std::fabs(den) > 1e-12) frac = 0.5 * (y0 - y2) / den;
            if (frac > 1 || frac < -1) frac = 0;
        }
        return static_cast<double>(bi) + frac;
    };

    // first tip in the opening 1.5 lines
    double t = tip_in(0, approx_spl * 1.5);
    if (t < 0) return out;
    double spl = approx_spl;
    out.line_starts.push_back(t);
    double sync_sum = sig[static_cast<int64_t>(t)];
    int sync_n = 1;
    while (true) {
        const double expect = t + spl;
        if (expect + win >= static_cast<double>(n)) break;
        const double nt = tip_in(expect - win, expect + win);
        if (nt < 0) break;
        const double d = nt - t;
        if (d > approx_spl * 0.5 && d < approx_spl * 1.6) spl += 0.05 * (d - spl);  // slow lock
        t = nt;
        out.line_starts.push_back(t);
        sync_sum += sig[static_cast<int64_t>(t)];
        sync_n++;
    }
    if (out.line_starts.size() < 8) return out;

    out.samples_per_line = spl;
    out.sync_level = sync_n ? sync_sum / sync_n : lo;
    // blanking reference: median of the whole oriented signal (cheap, robust)
    {
        std::vector<float> s2(sig);
        std::nth_element(s2.begin(), s2.begin() + s2.size() / 2, s2.end());
        out.blank_level = s2[s2.size() / 2];
    }

    // vertical sync = a run that stays at sync level far longer than one hsync
    const double thr = out.sync_level + 0.5 * (out.blank_level - out.sync_level);
    const int64_t bridge = static_cast<int64_t>(approx_spl * 0.3);   // span serrations
    const int64_t min_run = static_cast<int64_t>(approx_spl * 0.6);  // longer than any hsync
    int64_t i = 0;
    while (i < static_cast<int64_t>(n)) {
        if (sig[i] < thr) {
            const int64_t start = i;
            int64_t last_low = i;
            int64_t j = i + 1;
            while (j < static_cast<int64_t>(n) && (sig[j] < thr || j - last_low <= bridge)) {
                if (sig[j] < thr) last_low = j;
                j++;
            }
            if (last_low - start >= min_run) {
                out.field_starts.push_back(static_cast<double>(start));
                // parity: phase of the run start within the line grid
                double phase = 0.5;
                // nearest line start at or before `start`
                auto it = std::upper_bound(out.line_starts.begin(), out.line_starts.end(), static_cast<double>(start));
                if (it != out.line_starts.begin()) {
                    const double ls = *(it - 1);
                    phase = (static_cast<double>(start) - ls) / spl;
                    phase -= std::floor(phase);
                }
                out.field_parity.push_back((phase < 0.25 || phase > 0.75) ? 0 : 1);
            }
            i = last_low + 1;
        } else {
            i++;
        }
    }
    out.ok = true;
    return out;
}

// === 6b. Blanking-offset sync (TEMPEST) ====================================
namespace {
// start index of the length-`band` circular window of `axis` (size len) with
// the smallest sum
int min_band_start(const std::vector<double>& axis, int band) {
    const int len = static_cast<int>(axis.size());
    if (len <= 0) return 0;
    if (band < 1) band = 1;
    if (band > len) band = len;
    double sum = 0;
    for (int i = 0; i < band; i++) sum += axis[i];
    double best = sum;
    int best_i = 0;
    for (int s = 1; s < len; s++) {
        sum += axis[(s + band - 1) % len] - axis[s - 1];
        if (sum < best) {
            best = sum;
            best_i = s;
        }
    }
    return best_i;
}
}  // namespace

BlankOffset find_blanking_offset(const float* img, int width, int height, int band_w, int band_h) {
    BlankOffset out;
    if (!img || width <= 0 || height <= 0) return out;
    if (band_w <= 0) band_w = std::max(1, width / 8);
    if (band_h <= 0) band_h = std::max(1, height / 8);

    std::vector<double> col(width, 0.0), row(height, 0.0);
    for (int r = 0; r < height; r++) {
        const float* p = img + static_cast<size_t>(r) * width;
        double rs = 0;
        for (int c = 0; c < width; c++) {
            const float v = std::isfinite(p[c]) ? p[c] : 0.0f;
            col[c] += v;
            rs += v;
        }
        row[r] = rs;
    }
    out.h_offset = min_band_start(col, band_w);
    out.v_offset = min_band_start(row, band_h);
    out.ok = true;
    return out;
}

// === self-test =============================================================
bool self_test(std::string* log) {
    // synthesize a known raster: 160 pixels/line (active), 120 lines/frame,
    // vertical bars + a ramp; AM-carry it with noise.
    const int W = 160, H = 120;
    const double spl = 192.0;          // samples per line (blanking included)
    const double active = 160.0;       // active samples per line
    const int frames = 6;
    const double fpf = spl * H;        // samples per frame
    const size_t n = static_cast<size_t>(fpf * frames);

    std::vector<float> pattern(static_cast<size_t>(W) * H);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            const float bars = ((c / 16) & 1) ? 0.9f : 0.1f;
            const float ramp = static_cast<float>(r) / H;
            pattern[static_cast<size_t>(r) * W + c] = 0.5f * bars + 0.5f * ramp;
        }

    // build the complex AM signal: envelope = DC + image intensity + noise
    std::vector<std::complex<float>> iq(n);
    uint32_t rng = 12345;
    auto urand = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 8) / static_cast<float>(1 << 24);   // 0..1
    };
    for (size_t i = 0; i < n; i++) {
        const double fp = std::fmod(static_cast<double>(i), fpf);
        const int line = static_cast<int>(fp / spl);
        const double into = fp - line * spl;
        float inten = 0.05f;   // blanking level
        if (into < active && line < H) {
            const int col = static_cast<int>(into / active * W);
            inten = 0.1f + 0.8f * pattern[static_cast<size_t>(line) * W + std::min(col, W - 1)];
        }
        const float env = 0.2f + inten + 0.08f * (urand() - 0.5f);
        const double ph = 0.37 * i;   // arbitrary carrier phase
        iq[i] = std::complex<float>(env * std::cos(ph), env * std::sin(ph));
    }

    Envelope::Config ec;
    ec.dc_block = false;
    ec.agc = false;
    Envelope env(ec);
    std::vector<float> e(n);
    env.process(iq.data(), n, e.data());

    // recover the line period from a one-frame window
    PeriodFinder pf;
    const int guess = static_cast<int>(spl);
    PeriodResult pr = pf.find(e.data(), static_cast<size_t>(fpf), guess - 40, guess + 40);
    const double err = pr.ok ? std::fabs(pr.period - spl) / spl : 1.0;

    // reconstruct using the recovered line period, accumulate frames, render
    const double rspl = pr.ok ? pr.period : spl;
    FrameAccumulator acc;
    acc.configure(W, H, 0.3f);
    std::vector<float> fr(static_cast<size_t>(W) * H);
    for (int k = 0; k < frames; k++) {
        RasterMap m;
        m.width = W;
        m.height = H;
        m.samples_per_line = rspl;
        m.samples_per_pixel = active / W;
        m.origin = k * fpf;
        resample(e.data(), n, m, fr.data());
        acc.add(fr.data());
    }

    // correlate the averaged float frame with the ground-truth pattern
    const float* avg = acc.average();
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    const size_t px = static_cast<size_t>(W) * H;
    for (size_t i = 0; i < px; i++) {
        const double x = pattern[i], y = avg[i];
        sx += x;
        sy += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
    }
    const double cnt = static_cast<double>(px);
    const double cov = sxy / cnt - (sx / cnt) * (sy / cnt);
    const double vx = sxx / cnt - (sx / cnt) * (sx / cnt);
    const double vy = syy / cnt - (sy / cnt) * (sy / cnt);
    const double corr = cov / std::sqrt(vx * vy + 1e-12);

    if (log) {
        char b[160];
        std::snprintf(b, sizeof b, "period true=%.2f rec=%.3f (err %.2f%%, conf %.2f) corr=%.3f", spl, pr.period,
                      err * 100, pr.confidence, corr);
        *log = b;
    }
    return pr.ok && err < 0.02 && corr > 0.9;
}

}  // namespace video
