// FM multiplex analysis, SCA / main programme audio, array combining - see mpx.hpp
#include "mpx.hpp"

#include "rds_demod.hpp"

#include <algorithm>
#include <cstdio>

namespace rds {

// ---- spectrum -------------------------------------------------------------------

MpxSpectrum::~MpxSpectrum() {
    kfft_destroy_plan(plan_);
    kfft_free(in_);
    kfft_free(out_);
}

void MpxSpectrum::init(double fs) {
    fs_ = fs;
    n_ = 256;
    while (fs / n_ > 500) n_ *= 2;   // bins of 250-500 Hz
    kfft_destroy_plan(plan_);
    kfft_free(in_);
    kfft_free(out_);
    in_ = kfft_alloc_real(static_cast<size_t>(n_));
    out_ = kfft_alloc_complex(static_cast<size_t>(n_ / 2 + 1));
    plan_ = kfft_plan_dft_r2c_1d(n_, in_, out_);
    win_.resize(static_cast<size_t>(n_));
    double s2 = 0;
    for (int i = 0; i < n_; i++) {
        win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * i / n_));
        s2 += win_[static_cast<size_t>(i)] * win_[static_cast<size_t>(i)];
    }
    wnorm_ = static_cast<float>(2.0 / (n_ * s2));
    reset();
}

void MpxSpectrum::reset() {
    buf_.assign(static_cast<size_t>(n_), 0);
    avg_.assign(static_cast<size_t>(bins()), 0);
    fill_ = 0;
    frames_ = 0;
    skip_ = 0;
}

void MpxSpectrum::push(const float* x, size_t n) {
    const float alpha_min = static_cast<float>(std::min(1.0, 2 * n_ / fs_ / 0.8));   // ~0.8 s average (every other frame)
    for (size_t i = 0; i < n; i++) {
        buf_[fill_++] = x[i];
        if (fill_ < static_cast<size_t>(n_)) continue;
        fill_ = 0;
        if (++skip_ & 1) continue;   // every other frame is plenty for a 0.8 s average
        for (int k = 0; k < n_; k++) in_[k] = buf_[static_cast<size_t>(k)] * win_[static_cast<size_t>(k)];
        kfft_execute(plan_);
        frames_++;
        const float a = std::max(alpha_min, 1.0f / static_cast<float>(frames_));
        for (int k = 0; k < bins(); k++) {
            const float p = (out_[k][0] * out_[k][0] + out_[k][1] * out_[k][1]) * wnorm_;
            avg_[static_cast<size_t>(k)] += a * (p - avg_[static_cast<size_t>(k)]);
        }
    }
}

double MpxSpectrum::band(double f0, double f1) const {
    const int a = std::max(0, bin_of(f0)), b = std::min(bins() - 1, bin_of(f1));
    double s = 0;
    for (int k = a; k <= b; k++) s += avg_[static_cast<size_t>(k)];
    return s;
}

// ---- subcarrier classification -----------------------------------------------------

static inline float fast_atan2_cf(cf q) { return fast_atan2(q.imag(), q.real()); }

const char* kind_name(int k) {
    switch (k) {
        case Subcarrier::FM_AUDIO: return "SCA (FM audio)";
        case Subcarrier::FM_DATA: return "SCA (FSK data)";
        case Subcarrier::FM_IDLE: return "SCA (FM, unmodulated)";
        case Subcarrier::RDS2: return "RDS2 data stream";
        case Subcarrier::DARC: return "DARC-like wideband data";
        case Subcarrier::AM: return "AM / DSB subcarrier";
        case Subcarrier::RDS: return "RDS signal (not synchronised)";
        default: return "subcarrier";
    }
}

std::string describe(const Subcarrier& s) {
    char b[160];
    const double inj = 100 * s.inj_hz / 75000;
    switch (s.kind) {
        case Subcarrier::FM_AUDIO:
        case Subcarrier::FM_IDLE:
            if (s.tone_hz > 0)
                snprintf(b, sizeof b, "%.1f kHz %s (a %.0f Hz tone), deviation ±%.1f kHz, injection %.1f %%", s.hz / 1e3,
                         kind_name(s.kind), s.tone_hz, s.dev_peak / 1e3, inj);
            else
                snprintf(b, sizeof b, "%.1f kHz %s, deviation ±%.1f kHz, injection %.1f %%", s.hz / 1e3,
                         kind_name(s.kind), s.dev_peak / 1e3, inj);
            break;
        case Subcarrier::FM_DATA:
            if (s.baud > 0)
                snprintf(b, sizeof b, "%.1f kHz %s, ±%.1f kHz, ~%.0f bit/s, injection %.1f %%", s.hz / 1e3,
                         kind_name(s.kind), s.dev_peak / 1e3, s.baud, inj);
            else
                snprintf(b, sizeof b, "%.1f kHz %s, ±%.1f kHz, injection %.1f %%", s.hz / 1e3, kind_name(s.kind),
                         s.dev_peak / 1e3, inj);
            break;
        default:
            snprintf(b, sizeof b, "%.1f kHz %s, %.0f kHz wide, injection %.1f %%", s.hz / 1e3, kind_name(s.kind),
                     s.width / 1e3, inj);
            break;
    }
    return b;
}

void probe_subcarrier(const float* mpx, size_t n, double fs, Subcarrier* s) {
    constexpr double PR = 48000;
    msresamp_crcf rs = msresamp_crcf_create(static_cast<float>(PR / fs), 60.0f);
    Mixer nco;
    const double f0 = std::round(s->hz / 500) * 500;   // on a 500 Hz grid: a short mixing table
    nco.set(f0, fs);
    std::vector<cf> mix(n), z(static_cast<size_t>(n * PR / fs) + 64);
    nco.mix(mpx, mix.data(), n);
    unsigned int ny = 0;
    msresamp_crcf_execute(rs, mix.data(), static_cast<unsigned int>(n), z.data(), &ny);
    msresamp_crcf_destroy(rs);
    z.resize(ny);
    const float hb = static_cast<float>(std::clamp(s->width / 2 + 1500, 3000.0, 12000.0));
    kp::Fir<cf> lp(kp::fir_lowpass(63, hb, static_cast<float>(PR)));
    for (auto& v : z) v = lp.push(v);
    const size_t skip = 300;
    if (z.size() < skip + 2048) return;
    // envelope
    double sm = 0, s2 = 0;
    for (size_t i = skip; i < z.size(); i++) {
        const double a = std::abs(z[i]);
        sm += a;
        s2 += a * a;
    }
    const double cnt = static_cast<double>(z.size() - skip), mean = sm / cnt;
    s->cv = mean > 0 ? std::sqrt(std::max(0.0, s2 / cnt - mean * mean)) / mean : 9;
    // discriminator
    std::vector<float> f;
    f.reserve(z.size());
    const float k = static_cast<float>(PR / (2 * M_PI));
    for (size_t i = skip; i < z.size(); i++) f.push_back(fast_atan2_cf(z[i] * std::conj(z[i - 1])) * k);
    double mu = 0;
    for (float v : f) mu += v;
    mu /= static_cast<double>(f.size());
    kp::Fir<float> flp(kp::fir_lowpass(31, std::min(8000.0f, hb), static_cast<float>(PR)));
    std::vector<float> g(f.size());
    double m2 = 0, m4 = 0;
    for (size_t i = 0; i < f.size(); i++) {
        g[i] = flp.push(static_cast<float>(f[i] - mu));
        if (i < 40) continue;
        const double v = g[i];
        m2 += v * v;
        m4 += v * v * v * v;
    }
    const double cn = static_cast<double>(g.size() - 40);
    m2 /= cn;
    m4 /= cn;
    s->dev_rms = std::sqrt(m2);
    const double kurt = m2 > 0 ? m4 / (m2 * m2) : 3;
    std::vector<float> a(g.begin() + 40, g.end());
    // the extremes (0.5 / 99.5 %): FSK data's two tones - their midpoint is the
    // carrier (the mean depends on the data's ones and zeros)
    const size_t ql = a.size() * 5 / 1000, qh = a.size() * 995 / 1000;
    std::nth_element(a.begin(), a.begin() + static_cast<long>(ql), a.end());
    const float lo = a[ql];
    std::nth_element(a.begin(), a.begin() + static_cast<long>(qh), a.end());
    const float hi = a[qh];
    const double mid = 0.5 * (lo + hi);
    s->dev_peak = 0.5 * (hi - lo);
    // a bit clock: a line in the squared derivative of the deviation
    s->baud = 0;
    int nf = 1 << 14;
    while (nf > static_cast<int>(g.size()) - 64) nf >>= 1;
    if (nf >= 2048) {
        float* in = kfft_alloc_real(static_cast<size_t>(nf));
        kfft_complex* out = kfft_alloc_complex(static_cast<size_t>(nf / 2 + 1));
        kfft_plan p = kfft_plan_dft_r2c_1d(nf, in, out);
        double em = 0;
        for (int i = 0; i < nf; i++) {
            const float d = g[static_cast<size_t>(i + 41)] - g[static_cast<size_t>(i + 40)];
            in[i] = d * d;
            em += in[i];
        }
        em /= nf;
        for (int i = 0; i < nf; i++) in[i] = static_cast<float>((in[i] - em) * (0.5 - 0.5 * std::cos(2 * M_PI * i / nf)));
        kfft_execute(p);
        const double bw = PR / nf;
        const int k0 = static_cast<int>(250 / bw), k1 = std::min(nf / 2 - 1, static_cast<int>(20000 / bw));
        std::vector<float> mag;
        int best = -1;
        float bm = 0;
        for (int i = k0; i <= k1; i++) {
            const float m = out[i][0] * out[i][0] + out[i][1] * out[i][1];
            mag.push_back(m);
            if (m > bm) {
                bm = m;
                best = i;
            }
        }
        std::nth_element(mag.begin(), mag.begin() + static_cast<long>(mag.size() / 2), mag.end());
        const float med = mag[mag.size() / 2];
        if (best > k0 && best < k1 && med > 0 && bm > 60 * med) {
            auto pw = [&](int i) { return std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1]); };
            const float l = pw(best - 1), c = pw(best), r = pw(best + 1);
            const float den = l - 2 * c + r;
            const double d = den != 0 ? 0.5 * (l - r) / den : 0;
            s->baud = (best + d) * bw;
        }
        // tones (a test tone, a few notes in turn): a few lines hold most of
        // the deviation's power - audio, not data (a tone's derivative has a
        // line at twice its frequency, which the clock search takes for a bit
        // rate); FSK data's spectrum is continuous
        for (int i = 0; i < nf; i++)
            in[i] = static_cast<float>(g[static_cast<size_t>(i + 40)] * (0.5 - 0.5 * std::cos(2 * M_PI * i / nf)));
        kfft_execute(p);
        double tot = 0;
        std::vector<double> pw(static_cast<size_t>(nf / 2 + 1));
        for (int i = 1; i <= nf / 2; i++) {
            pw[static_cast<size_t>(i)] = out[i][0] * out[i][0] + out[i][1] * out[i][1];
            tot += pw[static_cast<size_t>(i)];
        }
        double lines = 0;
        int bk = -1;
        const int a0 = std::max(2, static_cast<int>(100 / bw)), a1 = std::min(nf / 2 - 1, static_cast<int>(6000 / bw));
        for (int n3 = 0; n3 < 3; n3++) {   // the 3 strongest lines (3 bins each), taken out in turn
            double bl = 0;
            int k = -1;
            for (int i = a0; i < a1; i++) {
                const double l3 = pw[static_cast<size_t>(i - 1)] + pw[static_cast<size_t>(i)] + pw[static_cast<size_t>(i + 1)];
                if (l3 > bl) {
                    bl = l3;
                    k = i;
                }
            }
            if (k < 0) break;
            if (n3 == 0) bk = k;
            lines += bl;
            pw[static_cast<size_t>(k - 1)] = pw[static_cast<size_t>(k)] = pw[static_cast<size_t>(k + 1)] = 0;
        }
        s->tone_hz = tot > 0 && lines > 0.5 * tot ? bk * bw : 0;
        kfft_destroy_plan(p);
        kfft_free(in);
        kfft_free(out);
    }
    // what it is
    const double snr = std::pow(10.0, std::max(0.0, s->snr_db) / 10);
    const double thr = std::max(0.3, 1.2 / std::sqrt(snr));
    const bool const_env = s->cv < thr;
    if (const_env) {
        if (s->baud > 0 && kurt < 2.2 && s->tone_hz <= 0) s->kind = Subcarrier::FM_DATA;
        else if (s->dev_rms < 150) s->kind = Subcarrier::FM_IDLE;
        else s->kind = Subcarrier::FM_AUDIO;
        // the carrier: FSK data between its two tones, audio its mean
        s->hz = f0 + mu + (s->kind == Subcarrier::FM_DATA ? mid : 0.0);
    } else {
        const double rds2[3] = {66500, 71250, 76000};
        bool r2 = false;
        if (std::fabs(s->hz - 57000) < 600 && s->width <= 7000) {   // RDS that doesn't synchronise (yet)
            s->kind = Subcarrier::RDS;
            s->probes++;
            return;
        }
        for (double f0 : rds2) r2 = r2 || (std::fabs(s->hz - f0) < 400 && s->width <= 7000);
        if (r2) s->kind = Subcarrier::RDS2;
        else if (std::fabs(s->hz - 76000) < 1500 && s->width >= 10000) s->kind = Subcarrier::DARC;
        else s->kind = Subcarrier::AM;
    }
    s->probes++;
}

// ---- SCA audio --------------------------------------------------------------------

ScaAudio::~ScaAudio() {
    if (rs_) msresamp_crcf_destroy(rs_);
}

void ScaAudio::init(double fs, double fc, double half_bw, double deemph_us, double full_dev) {
    constexpr double AR = 32000;
    fs_ = fs;
    fc_ = fc;
    if (rs_) msresamp_crcf_destroy(rs_);
    rs_ = msresamp_crcf_create(static_cast<float>(AR / fs), 60.0f);
    lp_.set_taps(kp::fir_lowpass(63, static_cast<float>(half_bw), static_cast<float>(AR)));
    alp_.set_taps(kp::fir_lowpass(63, 3600, static_cast<float>(AR)));
    a_de_ = deemph_us > 0 ? static_cast<float>(1 - std::exp(-1 / (deemph_us * 1e-6 * AR))) : 1.0f;
    gain_ = static_cast<float>(1 / std::max(1000.0, full_dev));
    nco_.set(fc, fs);
    prev_ = {1, 0};
    dc_ = de_ = 0;
    dec_ = 0;
}

void ScaAudio::push(const float* mpx, size_t n, std::vector<float>* pcm8k) {
    if (!rs_) return;
    mix_.resize(n);
    nco_.mix(mpx, mix_.data(), n);
    z_.resize(static_cast<size_t>(n * 32000 / fs_) + 32);
    unsigned int ny = 0;
    msresamp_crcf_execute(rs_, mix_.data(), static_cast<unsigned int>(n), z_.data(), &ny);
    const float k = static_cast<float>(32000 / (2 * M_PI));
    for (unsigned int i = 0; i < ny; i++) {
        const cf v = lp_.push(z_[i]);
        float f = std::arg(v * std::conj(prev_)) * k;
        prev_ = v;
        dc_ += 0.0005f * (f - dc_);
        f -= dc_;
        de_ += a_de_ * (f - de_);
        const float y = alp_.push(de_);
        if (++dec_ == 4) {
            dec_ = 0;
            pcm8k->push_back(std::clamp(y * gain_, -1.2f, 1.2f));
        }
    }
}

// ---- main programme --------------------------------------------------------------------

MainAudio::~MainAudio() {
    if (rs_) msresamp_rrrf_destroy(rs_);
}

void MainAudio::init(double fs, double deemph_us) {
    constexpr double AR = 16000;
    if (rs_) msresamp_rrrf_destroy(rs_);
    rs_ = msresamp_rrrf_create(static_cast<float>(AR / fs), 60.0f);
    lp_.set_taps(kp::fir_lowpass(63, 3600, static_cast<float>(AR)));
    a_de_ = deemph_us > 0 ? static_cast<float>(1 - std::exp(-1 / (deemph_us * 1e-6 * AR))) : 1.0f;
    de_ = dc_ = 0;
    dec_ = 0;
}

void MainAudio::push(const float* mpx, size_t n, std::vector<float>* pcm8k) {
    if (!rs_) return;
    in_.assign(mpx, mpx + n);
    y_.resize(n / 4 + 64);   // 16 kHz from >= 120 kHz
    unsigned int ny = 0;
    msresamp_rrrf_execute(rs_, in_.data(), static_cast<unsigned int>(n), y_.data(), &ny);
    for (unsigned int i = 0; i < ny; i++) {
        float v = y_[i];
        dc_ += 0.0005f * (v - dc_);
        v -= dc_;
        de_ += a_de_ * (v - de_);
        const float o = lp_.push(de_);
        if (++dec_ == 2) {
            dec_ = 0;
            pcm8k->push_back(std::clamp(o / 65000.0f, -1.2f, 1.2f));
        }
    }
}

// ---- array combining -----------------------------------------------------------------

void Combiner::init(int m, double fs) {
    m_ = m;
    fs_ = fs;
    r_.assign(static_cast<size_t>(m * m), 0);
    s_.assign(static_cast<size_t>(m * m), cf{});
    w_.assign(static_cast<size_t>(m), cf{});
    w_[0] = 1;
    wn_ = w_;
    blk_ = 256;
    pos_ = 0;
    alpha_ = std::min(1.0, blk_ / (0.25 * fs));
    have_ = false;
    ratio_db_ = 0;
}

void Combiner::push(const cf* const* x, size_t n, cf* y) {
    const int m = m_;
    for (size_t i = 0; i < n; i++) {
        // the block's covariance from every 4th sample (FM at these rates is oversampled)
        if ((pos_ & 3) == 0) {
            for (int r = 0; r < m; r++) {
                const cf xr = x[r][i];
                for (int c = r; c < m; c++) s_[static_cast<size_t>(r * m + c)] += xr * std::conj(x[c][i]);
            }
        }
        // the weights move from w_ to wn_ across the block
        const float u = static_cast<float>(pos_) / static_cast<float>(blk_);
        cf acc{};
        for (int k = 0; k < m; k++) acc += std::conj(w_[static_cast<size_t>(k)] + u * (wn_[static_cast<size_t>(k)] - w_[static_cast<size_t>(k)])) * x[k][i];
        y[i] = acc;
        if (++pos_ == blk_) {
            pos_ = 0;
            w_ = wn_;
            // R = (1 - a) R + a (the block's mean)
            const double inv = 1.0 / (blk_ / 4);
            for (int r = 0; r < m; r++)
                for (int c = r; c < m; c++) {
                    const size_t e = static_cast<size_t>(r * m + c);
                    const std::complex<double> b(s_[e].real() * inv, s_[e].imag() * inv);
                    r_[e] = have_ ? (1 - alpha_) * r_[e] + alpha_ * b : b;
                    s_[e] = 0;
                }
            update();
        }
    }
}

void Combiner::update() {
    const int m = m_;
    // Hermitian matrix from its upper triangle
    std::vector<std::complex<double>> R(static_cast<size_t>(m * m));
    double tr = 0;
    for (int r = 0; r < m; r++) {
        for (int c = r; c < m; c++) {
            const auto v = r_[static_cast<size_t>(r * m + c)];
            R[static_cast<size_t>(r * m + c)] = v;
            R[static_cast<size_t>(c * m + r)] = std::conj(v);
        }
        tr += R[static_cast<size_t>(r * m + r)].real();
    }
    if (tr <= 0) return;
    std::vector<std::complex<double>> v(static_cast<size_t>(m)), t(static_cast<size_t>(m));
    if (!have_) {
        for (int k = 0; k < m; k++) v[static_cast<size_t>(k)] = R[static_cast<size_t>(k * m)];   // first column
    } else {
        for (int k = 0; k < m; k++) v[static_cast<size_t>(k)] = std::complex<double>(wn_[static_cast<size_t>(k)].real(), wn_[static_cast<size_t>(k)].imag());
    }
    double lam = 0;
    for (int it = 0; it < (have_ ? 3 : 30); it++) {
        double nn = 0;
        for (int r = 0; r < m; r++) {
            std::complex<double> s = 0;
            for (int c = 0; c < m; c++) s += R[static_cast<size_t>(r * m + c)] * v[static_cast<size_t>(c)];
            t[static_cast<size_t>(r)] = s;
            nn += std::norm(s);
        }
        nn = std::sqrt(nn);
        if (nn <= 0) return;
        lam = nn;
        for (int k = 0; k < m; k++) v[static_cast<size_t>(k)] = t[static_cast<size_t>(k)] / nn;
    }
    // keep the phase of the previous weights (no jump in the combined phase)
    std::complex<double> c = 0;
    for (int k = 0; k < m; k++) c += std::conj(std::complex<double>(wn_[static_cast<size_t>(k)].real(), wn_[static_cast<size_t>(k)].imag())) * v[static_cast<size_t>(k)];
    const double cm = std::abs(c);
    const std::complex<double> rot = cm > 0 ? std::conj(c) / cm : 1.0;
    for (int k = 0; k < m; k++) {
        const auto q = v[static_cast<size_t>(k)] * rot;
        wn_[static_cast<size_t>(k)] = cf(static_cast<float>(q.real()), static_cast<float>(q.imag()));
    }
    have_ = true;
    const double rest = m > 1 ? (tr - lam) / (m - 1) : 0;
    ratio_db_ = rest > 0 ? static_cast<float>(10 * std::log10(lam / rest)) : 40.0f;
}

}  // namespace rds
