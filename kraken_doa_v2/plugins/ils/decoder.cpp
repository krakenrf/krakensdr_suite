// ILS / VOR / marker beacon decoder: the VHF / UHF navigation aids.
//
//   ILS localizer   108.10-111.95 MHz (odd tenths). AM, 90 Hz + 150 Hz tones
//                   20 % each on the runway's centre line: 90 Hz dominates
//                   left of the course (as an approaching aircraft sees it),
//                   150 Hz right. DDM = m90 - m150 (0.155 = full scale, 150
//                   uA), SDM = m90 + m150 (40 %). Morse ident on 1020 Hz.
//                   Some have two carriers (course + clearance, 5-14 kHz apart)
//   ILS glide slope 329.15-335.00 MHz, paired with the localizer. 90 Hz above
//                   the glide path, 150 Hz below; 40 % each (SDM 80 %),
//                   0.175 DDM = full scale. No ident. Two carriers possible
//   VOR             108.00-117.95 MHz. AM: 30 Hz "variable" (30 %) + a 9960 Hz
//                   subcarrier (30 %) frequency modulated +-480 Hz by the 30
//                   Hz "reference". The variable lags the reference by the
//                   radial (magnetic bearing from the station; Doppler VORs
//                   swap the two and turn the other way - the same result).
//                   Morse ident on 1020 Hz, sometimes voice
//   Marker beacon   75 MHz, 95 % AM: outer 400 Hz dashes (2/s), middle 1300
//                   Hz dots and dashes, inner 3000 Hz dots (6/s)
//
// Every 0.5 s the last 1 s (48 kHz) is analysed: carrier(s) found in its
// spectrum, each demodulated by cutting its band out of the FFT (zero
// phase) -> envelope -> modulation m(t) = envelope / mean - 1 -> its
// spectrum gives the tone depths at 1 Hz resolution; VOR: the subcarrier's
// band -> analytic signal -> instantaneous frequency -> 30 Hz reference
// phase vs the 30 Hz AM phase. Ident / marker keying: 10 ms Goertzel ticks
// of the 1020 / 400 / 1300 / 3000 Hz tones with an adaptive key threshold
// -> Morse (lib/navaid). The type comes from the signal itself, the paired
// frequencies (glide slope, DME channel) from the VFO's frequency.

#include "kraken_plugin.hpp"
#include "navaid.hpp"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace {

using kp::cf;
using cd = std::complex<double>;

constexpr int FS = 48000;
constexpr int N = FS;                   // analysis block: 1 s (1 Hz bins)
constexpr int HOP = N / 2;
constexpr int NB = 24000;               // demodulated band: 24 kHz (+-12 kHz)
constexpr int NS = 2048;                // VOR subcarrier band (+-1 kHz) at 2048 Hz
constexpr double SC_HZ = 9960;
constexpr double TICK_S = 0.010;
constexpr int TICK_N = NB / 100;        // 240 samples
constexpr int KEY_HIST = 3000;          // ticks of tone level history (30 s)
const double TONES[] = {400, 1020, 1300, 3000};
enum Tone { T400, T1020, T1300, T3000, NTONES };

std::string fmt(const char* f, double v) {
    char b[80];
    snprintf(b, sizeof b, f, v);
    return b;
}
double wrap360(double d) { return std::fmod(std::fmod(d, 360.0) + 360.0, 360.0); }

enum class Type { NONE, CARRIER, LOC, GS, VOR, MARKER };
const char* type_name(Type t) {
    switch (t) {
        case Type::LOC: return "ILS localizer";
        case Type::GS: return "ILS glide slope";
        case Type::VOR: return "VOR";
        case Type::MARKER: return "marker beacon";
        case Type::CARRIER: return "AM carrier without navigation tones";
        default: return "";
    }
}

// the modulation of one carrier in one block
struct Mod {
    double off_hz = 0;            // carrier, from the VFO centre
    double snr_db = 0;            // carrier over the noise per 1 Hz
    double level_db = 0;          // carrier dBFS
    double m30 = 0, m90 = 0, m150 = 0, m400 = 0, m1020 = 0, m1300 = 0, m3000 = 0;
    double sc = 0;                // 9960 Hz subcarrier depth
    double noise = 0;             // modulation noise per 1 Hz bin (depth units)
    double voice = 0;             // 300-3000 Hz without the tones
    double radial = NAN, dev_hz = 0, radial_w = 0;
};

// adaptive on / off keying of one tone from its 10 ms levels
struct Keyer {
    std::deque<float> hist;
    float on = 0, off = 0, thr = 0;
    bool key = false, keyed = false;   // keyed: on / off levels clearly apart
    int ticks = 0;
    void update() {
        if (hist.size() < 100) return;
        std::vector<float> v(hist.begin(), hist.end());
        auto at = [&](double q) {
            auto it = v.begin() + static_cast<long>(q * (v.size() - 1));
            std::nth_element(v.begin(), it, v.end());
            return *it;
        };
        on = at(0.98);
        off = at(0.15);
        // noise alone: 98th / 15th percentile of its levels ~5x
        keyed = on > 6 * std::max(off, 1e-6f) && on > 0.02f;
        thr = std::sqrt(on * std::max(off, on * 0.01f));
    }
    bool push(float a) {
        hist.push_back(a);
        if (hist.size() > KEY_HIST) hist.pop_front();
        if (++ticks % 50 == 0) update();
        if (thr <= 0 || !keyed) return key = false;
        key = key ? a > thr * 0.8f : a > thr * 1.25f;
        return key;
    }
};

// keying pattern of a marker tone: mark lengths and repetition
struct Pattern {
    int len = 0, gap = 0;
    bool cur = false;
    std::deque<std::pair<float, float>> marks;   // (mark s, gap before s)
    void push(bool k) {
        if (k == cur) {
            (k ? len : gap)++;
            return;
        }
        if (cur) {   // a mark ended: keep it with the gap before it
            marks.emplace_back(len * TICK_S, gap * TICK_S);
            if (marks.size() > 40) marks.pop_front();
            gap = 1;
            len = 0;
        } else {
            len = 1;
        }
        cur = k;
    }
    // "dashes 2.0/s", "dots 6.0/s", "dots and dashes"
    std::string text() const {
        if (marks.size() < 4) return "";
        int dots = 0, dashes = 0;
        double sum = 0;
        for (const auto& m : marks) {
            (m.first < 0.14 ? dots : dashes)++;
            sum += m.first + m.second;
        }
        const double rate = marks.size() / std::max(sum, 1e-3);
        if (!dashes) return fmt("dots, %.1f /s", rate);
        if (!dots) return fmt("dashes, %.1f /s", rate);
        return fmt("dots and dashes, %.1f marks/s", rate);
    }
};

class Ils : public kp::Decoder {
public:
    explicit Ils(kp::Host& h) : Decoder(h) {
        fin_ = fftwf_alloc_complex(N);
        fout_ = fftwf_alloc_complex(N);
        bin_ = fftwf_alloc_complex(NB);
        bout_ = fftwf_alloc_complex(NB);
        rin_ = fftwf_alloc_real(NB);
        rout_ = fftwf_alloc_complex(NB / 2 + 1);
        sin_ = fftwf_alloc_complex(NS);
        sout_ = fftwf_alloc_complex(NS);
        fwd_ = fftwf_plan_dft_1d(N, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
        band_ = fftwf_plan_dft_1d(NB, bin_, bout_, FFTW_BACKWARD, FFTW_ESTIMATE);
        tone_ = fftwf_plan_dft_r2c_1d(NB, rin_, rout_, FFTW_ESTIMATE);
        sub_ = fftwf_plan_dft_1d(NS, sin_, sout_, FFTW_BACKWARD, FFTW_ESTIMATE);
        m_.resize(NB);
        win_.resize(TICK_N);
        for (int i = 0; i < TICK_N; i++) win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / TICK_N));
    }
    ~Ils() override {
        for (fftwf_plan p : {fwd_, band_, tone_, sub_}) fftwf_destroy_plan(p);
        fftwf_free(fin_);
        fftwf_free(fout_);
        fftwf_free(bin_);
        fftwf_free(bout_);
        fftwf_free(rin_);
        fftwf_free(rout_);
        fftwf_free(sin_);
        fftwf_free(sout_);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "magvar") magvar_ = v.empty() ? NAN : atof(v.c_str());
    }

    void reset() override {
        buf_.clear();
        for (auto& k : keys_) k = Keyer{};
        for (auto& p : pats_) p = Pattern{};
        morse_.reset();
        morse_armed_ = false;
        quiet_ = 0;
        type_ = Type::NONE;
        type_votes_ = 0;
        cand_ = Type::NONE;
        ident_.clear();
        ident_count_ = 0;
        rad_ = cd(0, 0);
        ddm_avg_ = sdm_avg_ = NAN;
        said_type_ = Type::NONE;
        for (const char* f : {"Navaid", "Frequency", "Paired with", "Ident", "Carrier", "Modulation", "Course",
                              "Glide path", "Radial", "Marker", "Second carrier", "Audio"})
            host.fact(f, "");
    }

    void process(const cf* x, size_t n) override {
        if (buf_.empty()) buf_t0_ = host.time();
        buf_.insert(buf_.end(), x, x + n);
        while (buf_.size() >= static_cast<size_t>(N)) {
            analyse(buf_t0_);
            buf_.erase(buf_.begin(), buf_.begin() + HOP);
            buf_t0_ += static_cast<double>(HOP) / FS;
        }
    }

private:
    fftwf_complex *fin_, *fout_, *bin_, *bout_, *rout_, *sin_, *sout_;
    float* rin_;
    fftwf_plan fwd_, band_, tone_, sub_;
    std::vector<cf> buf_;
    double buf_t0_ = 0;
    std::vector<float> m_;          // modulation of the main carrier (24 kHz, 1 s)
    std::vector<float> win_;        // Hann window of a tick (keeps the 1300 Hz marker tone out of 1020 Hz)
    Keyer keys_[NTONES];
    Pattern pats_[NTONES];
    nav::MorseDecoder morse_{TICK_S, 0.06, 0.25, 1.5};
    bool morse_armed_ = false;
    int quiet_ = 0;
    Type type_ = Type::NONE, cand_ = Type::NONE, said_type_ = Type::NONE;
    int type_votes_ = 0;
    std::string ident_, ident_code_;
    int ident_count_ = 0;
    double ident_t_ = -1;
    cd rad_{0, 0};                  // radial vectors (averaged)
    double ddm_avg_ = NAN, sdm_avg_ = NAN;
    double magvar_ = NAN;
    double facts_t_ = -10;
    Mod main_{}, second_{};
    bool have_second_ = false;
    int marker_tone_ = -1;

    double power(int k) const {   // Hann-windowed power of FFT bin k (from the unwindowed bins)
        auto X = [&](int i) {
            i = ((i % N) + N) % N;
            return cd(fout_[i][0], fout_[i][1]);
        };
        return std::norm(0.5 * X(k) - 0.25 * X(k - 1) - 0.25 * X(k + 1));
    }

    void analyse(double t0) {
        std::copy(buf_.begin(), buf_.begin() + N, reinterpret_cast<cf*>(fin_));
        fftwf_execute(fwd_);
        // --- carriers (|f| <= 20 kHz)
        const int span = 20000;
        std::vector<double> p(2 * span + 1);
        for (int f = -span; f <= span; f++) p[f + span] = power(f);
        std::vector<double> q = p;
        std::nth_element(q.begin(), q.begin() + q.size() / 2, q.end());
        const double floor = std::max(q[q.size() / 2], 1e-30);
        int k1 = static_cast<int>(std::max_element(p.begin(), p.end()) - p.begin()) - span;
        const double snr1 = 10 * std::log10(p[k1 + span] / floor);
        if (snr1 < 15) {
            no_signal(t0);
            return;
        }
        // a second carrier 4-32 kHz away (two-frequency ILS), not a VOR sideband
        int k2 = 0;
        double best = 0;
        for (int f = -span; f <= span; f++) {
            const int d = std::abs(f - k1);
            if (d < 4000 || d > 32000 || std::fabs(d - SC_HZ) < 900) continue;
            const double v = p[f + span];
            if (v <= best) continue;
            // a line: well above what is 30 Hz either side
            const double side = std::max(f - 30 >= -span ? p[f - 30 + span] : 0, f + 30 <= span ? p[f + 30 + span] : 0);
            if (v > 30 * side) {
                best = v;
                k2 = f;
            }
        }
        const bool two = best > 0 && 10 * std::log10(best / floor) >= 20;
        const int sep = two ? std::abs(k2 - k1) : 1000000;
        main_ = demod(k1, std::min(12000, sep - 1500), floor, true);
        main_.snr_db = snr1;
        have_second_ = false;
        if (two) {
            second_ = demod(k2, std::min(3000, sep - 1500), floor, false);
            second_.snr_db = 10 * std::log10(best / floor);
            have_second_ = second_.m90 + second_.m150 > 0.15;   // it carries ILS tones too
        }
        ticks(main_);
        classify(t0);
    }

    // carrier at bin k: cut +-bw out of the spectrum, envelope, tones
    Mod demod(int k, int bw, double floor, bool keep) {
        Mod m;
        m.off_hz = k;
        // fractional carrier position (Hann-windowed parabola)
        const double a = std::log(power(k - 1) + 1e-30), b = std::log(power(k) + 1e-30), c = std::log(power(k + 1) + 1e-30);
        const double den = a - 2 * b + c;
        if (den < 0) m.off_hz += 0.5 * (a - c) / den;
        m.level_db = 10 * std::log10(power(k) / (0.25 * double(N) * N) + 1e-30);   // Hann coherent gain 0.5
        (void)floor;
        for (int i = 0; i < NB; i++) bin_[i][0] = bin_[i][1] = 0;
        for (int r = -bw; r <= bw; r++) {
            const int src = (((k + r) % N) + N) % N;
            const int dst = (r + NB) % NB;
            bin_[dst][0] = fout_[src][0];
            bin_[dst][1] = fout_[src][1];
        }
        fftwf_execute(band_);
        double mean = 0;
        for (int i = 0; i < NB; i++) {
            const float e = std::sqrt(bout_[i][0] * bout_[i][0] + bout_[i][1] * bout_[i][1]);
            rin_[i] = e;
            mean += e;
        }
        mean /= NB;
        if (mean <= 0) return m;
        for (int i = 0; i < NB; i++) rin_[i] = static_cast<float>(rin_[i] / mean - 1);
        if (keep) std::copy(rin_, rin_ + NB, m_.begin());
        fftwf_execute(tone_);
        auto M = [&](int f) { return cd(rout_[f][0], rout_[f][1]); };
        // depth of a tone: amplitude in m(t), its power summed over +-w bins
        auto depth = [&](double f, int w) {
            double s = 0;
            const int c0 = static_cast<int>(std::lround(f));
            for (int i = std::max(1, c0 - w); i <= std::min(NB / 2, c0 + w); i++) s += std::norm(M(i));
            return 2 * std::sqrt(s) / NB;
        };
        m.m30 = depth(30, 1);
        m.m90 = depth(90, 3);
        m.m150 = depth(150, 4);
        m.m400 = depth(400, 15);
        m.m1020 = depth(1020, 60);
        m.m1300 = depth(1300, 30);
        m.m3000 = depth(3000, 60);
        m.sc = depth(SC_HZ, 800);
        // noise: 5-8 kHz, per bin
        double ns = 0;
        int nn = 0;
        for (int f = 5000; f < 8000; f++, nn++) ns += std::norm(M(f));
        m.noise = 2 * std::sqrt(ns / nn) / NB;
        // voice / other audio: 300-3000 Hz without the tones' bands
        double vs = 0;
        for (int f = 300; f <= 3000; f++) {
            bool tone = false;
            for (double t : TONES) tone |= std::fabs(f - t) < 150;   // with the keying's sidebands
            if (!tone) vs += std::norm(M(f)) - (m.noise * NB / 2) * (m.noise * NB / 2);
        }
        m.voice = 2 * std::sqrt(std::max(vs, 0.0)) / NB;
        // VOR: the subcarrier's instantaneous frequency against the 30 Hz AM
        if (keep && m.sc > 0.05 && bw >= 11000) {
            for (int i = 0; i < NS; i++) sin_[i][0] = sin_[i][1] = 0;
            for (int r = -900; r <= 900; r++) {
                const cd v = M(static_cast<int>(SC_HZ) + r);
                sin_[(r + NS) % NS][0] = static_cast<float>(v.real());
                sin_[(r + NS) % NS][1] = static_cast<float>(v.imag());
            }
            fftwf_execute(sub_);
            cd R(0, 0);
            for (int i = 0; i < NS; i++) {
                const cd s1(sout_[i][0], sout_[i][1]), s0(sout_[(i + NS - 1) % NS][0], sout_[(i + NS - 1) % NS][1]);
                const double f = std::arg(s1 * std::conj(s0)) * NS / (2 * M_PI);
                const double t = (i - 0.5) / NS;   // between the two samples
                R += f * std::polar(1.0, -2 * M_PI * 30 * t);
            }
            const cd V = M(30);
            m.dev_hz = 2 * std::abs(R) / NS;
            if (m.dev_hz > 200 && std::abs(V) > 0) {
                m.radial = wrap360((std::arg(R) - std::arg(V)) * 180 / M_PI);
                m.radial_w = std::min(m.m30, 1.0) * std::min(m.dev_hz / 480, 1.0);
            }
        }
        return m;
    }

    // 10 ms tone levels of the middle half of the block (0.25..0.75 s: with
    // the 0.5 s hop the ticks follow each other without gaps)
    void ticks(const Mod&) {
        for (int t = NB / 4; t + TICK_N <= 3 * NB / 4; t += TICK_N) {
            for (int k = 0; k < NTONES; k++) {
                const double w = 2 * M_PI * TONES[k] / NB;
                double s1 = 0, s2 = 0;
                const double cw = 2 * std::cos(w);
                for (int i = 0; i < TICK_N; i++) {
                    const double s0 = m_[t + i] * win_[i] + cw * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                const double pw = s1 * s1 + s2 * s2 - cw * s1 * s2;
                const float a = static_cast<float>(4 * std::sqrt(std::max(pw, 0.0)) / TICK_N);   // Hann gain 0.5
                const bool key = keys_[k].push(a);
                if (k == T1020) {
                    // the Morse decoder starts after a clear gap once the key
                    // threshold is known (else the first ident is cut short)
                    if (!keys_[k].keyed) {
                        morse_.reset();
                        morse_armed_ = false;
                        quiet_ = 0;
                    } else if (!morse_armed_) {
                        quiet_ = key ? 0 : quiet_ + 1;
                        morse_armed_ = quiet_ * TICK_S >= 1.2;
                    } else if (morse_.push(key)) {
                        on_ident();
                    }
                } else {
                    pats_[k].push(key);
                }
            }
        }
    }

    void on_ident() {
        const std::string txt = morse_.text();
        if (host.verbose())
            host.log("ident: " + txt + " (" + morse_.code() + fmt(", dot %.0f ms", morse_.unit_s() * 1000) +
                     fmt(", fit %.2f)", morse_.quality()));
        if (txt.empty() || txt.size() > 8 || txt.find('?') != std::string::npos || morse_.quality() < 0.6) return;
        if (type_ == Type::MARKER || type_ == Type::GS) return;   // their keying isn't an ident
        const bool again = txt == ident_;
        ident_ = txt;
        ident_code_ = morse_.code();
        ident_count_ = again ? ident_count_ + 1 : 1;
        ident_t_ = host.time();
        host.event("Ident " + txt + " [" + ident_code_ + "]" + (type_ != Type::NONE ? std::string(" - ") + type_name(type_) : "") +
                       freq_text(),
                   0.0);
        if (host.raw_wanted()) host.raw(std::string(type_name(type_)) + freq_text() + " ident " + txt + " " + ident_code_);
    }

    double carrier_mhz() const {
        const double rf = host.rf_hz();
        if (!std::isfinite(rf)) return NAN;
        return (rf + (host.rf_inverted() ? -1 : 1) * main_.off_hz) / 1e6;
    }
    // the nominal channel (50 kHz grid; markers 75 MHz)
    double channel_mhz() const {
        const double f = carrier_mhz();
        return std::isfinite(f) ? std::round(f * 20) / 20 : NAN;
    }
    std::string freq_text() const {
        const double ch = channel_mhz();
        return std::isfinite(ch) ? " on " + nav::mhz_text(ch) : "";
    }

    Type judge(const Mod& m) const {
        const double ch = channel_mhz();
        const double sdm = m.m90 + m.m150;
        if (m.sc > 0.1 && m.m30 > 0.08 && m.dev_hz > 250 && m.dev_hz < 800) return Type::VOR;
        if (sdm > 0.2 && sdm < 1.2 && std::max(m.m90, m.m150) > 0.08 && m.m30 < 0.08) {
            if (std::isfinite(ch)) {
                if (ch >= 328.5 && ch <= 335.5) return Type::GS;
                if (ch >= 108 && ch <= 112.1) return Type::LOC;
            }
            return sdm >= 0.6 ? Type::GS : Type::LOC;
        }
        // markers: a strong keyed tone - on 75 MHz when the frequency is
        // known (airband voice can have keyed-looking 400 Hz bursts)
        if (!std::isfinite(ch) || std::fabs(ch - 75.0) < 0.5)
            for (int k : {T400, T1300, T3000})
                if (keys_[k].keyed && keys_[k].on > 0.3) return Type::MARKER;
        return Type::CARRIER;
    }

    void classify(double t0) {
        const Type t = judge(main_);
        if (t == cand_) type_votes_++;
        else {
            cand_ = t;
            type_votes_ = 1;
        }
        if (type_votes_ >= 3 && type_ != cand_) type_ = cand_;
        // averages
        if (type_ == Type::LOC || type_ == Type::GS) {
            const double ddm = main_.m90 - main_.m150, sdm = main_.m90 + main_.m150;
            ddm_avg_ = std::isfinite(ddm_avg_) ? 0.7 * ddm_avg_ + 0.3 * ddm : ddm;
            sdm_avg_ = std::isfinite(sdm_avg_) ? 0.7 * sdm_avg_ + 0.3 * sdm : sdm;
        }
        if (type_ == Type::VOR && std::isfinite(main_.radial)) {
            rad_ = 0.8 * rad_ + 0.2 * std::polar(main_.radial_w, main_.radial * M_PI / 180);
        }
        if (type_ == Type::MARKER) {
            marker_tone_ = -1;
            double best = 0;
            for (int k : {T400, T1300, T3000})
                if (keys_[k].keyed && keys_[k].on > best) {
                    best = keys_[k].on;
                    marker_tone_ = k;
                }
        }
        const bool nav = type_ == Type::LOC || type_ == Type::GS || type_ == Type::VOR || type_ == Type::MARKER;
        if (nav && t == type_) host.valid(t0, t0 + static_cast<double>(N) / FS);
        if (type_ != said_type_ && nav) {
            said_type_ = type_;
            host.event(std::string(type_name(type_)) + freq_text() + (paired().empty() ? "" : " - " + paired()), 0.0);
        }
        const double now = host.time();
        if (now - facts_t_ >= 1.0) {
            facts_t_ = now;
            facts();
        }
    }

    void no_signal(double) {
        if (host.time() - facts_t_ >= 1.0) {
            facts_t_ = host.time();
            host.fact("Carrier", "none (no carrier 15 dB over the noise within +-20 kHz)");
        }
        cand_ = Type::NONE;
        type_votes_ = 0;
    }

    // what the frequency is paired with
    std::string paired() const {
        const double ch = channel_mhz();
        if (!std::isfinite(ch)) return "";
        if (type_ == Type::GS || nav::is_glideslope(ch)) {
            const double loc = nav::localizer_for(ch);
            if (!std::isfinite(loc)) return "";
            const nav::DmeChannel d = nav::dme_by_vhf(loc);
            return "localizer " + nav::mhz_text(loc) + (d.valid() ? ", DME " + d.name() + " (reply " + std::to_string(d.reply_mhz) + " MHz)" : "");
        }
        if (ch >= 108 && ch <= 118) {
            const nav::DmeChannel d = nav::dme_by_vhf(ch);
            std::string s;
            if (nav::is_localizer(ch)) s = "glide slope " + nav::mhz_text(nav::glideslope_for(ch));
            if (d.valid()) s += (s.empty() ? "" : ", ") + std::string("DME ") + d.name() + " (reply " + std::to_string(d.reply_mhz) +
                                " MHz, interrogation " + std::to_string(d.interrogation_mhz) + " MHz)";
            return s;
        }
        return "";
    }

    static std::string pct(double v) { return fmt("%.1f %%", v * 100); }

    void ils_facts(const Mod& m, const char* key, bool gs) {
        const double ddm = &m == &main_ && std::isfinite(ddm_avg_) ? ddm_avg_ : m.m90 - m.m150;
        const double sdm = &m == &main_ && std::isfinite(sdm_avg_) ? sdm_avg_ : m.m90 + m.m150;
        const double full = gs ? 0.175 : 0.155;   // DDM of a full-scale (150 uA) needle
        const double ua = std::min(std::fabs(ddm) / full * 150, 150.0);
        std::string where, fly;
        if (gs) {
            where = ddm > 0 ? "above the glide path" : "below the glide path";
            fly = ddm > 0 ? "fly down" : "fly up";
        } else {
            where = ddm > 0 ? "left of the course as an approaching aircraft sees it" : "right of the course as an approaching aircraft sees it";
            fly = ddm > 0 ? "fly right" : "fly left";
        }
        std::string s = fmt("DDM %+.3f", ddm) + " (" + (ddm >= 0 ? "90" : "150") + " Hz dominant): ";
        if (std::fabs(ddm) < 0.002) s += gs ? "on the glide path" : "on the course centre line";
        else s += where + " - needle " + fmt("%.0f uA ", ua) + fly + (std::fabs(ddm) >= full ? " (full scale)" : "");
        s += fmt("; SDM %.1f %%", sdm * 100);
        host.fact(key, s);
    }

    void facts() {
        const bool nav = type_ != Type::NONE && type_ != Type::CARRIER;
        host.fact("Navaid", type_ == Type::NONE ? "" : type_name(type_));
        const double ch = channel_mhz();
        host.fact("Frequency", std::isfinite(ch) ? nav::mhz_text(ch, ch < 100 ? 2 : 2) +
                                                       (nav::is_localizer(ch) ? " (ILS localizer channel)" :
                                                        nav::is_vor(ch) ? " (VOR channel)" :
                                                        nav::is_glideslope(ch) ? " (glide slope channel)" : "")
                                                 : "");
        host.fact("Paired with", paired());
        const double cm = carrier_mhz();
        host.fact("Carrier", (std::isfinite(cm) ? fmt("%.5f MHz, ", cm) : std::string()) + fmt("%+.0f Hz from the VFO", main_.off_hz) +
                                 fmt("; %.1f dBFS", main_.level_db) + fmt(", %.0f dB over the noise floor (1 Hz bins)", main_.snr_db));
        host.fact("Ident", ident_.empty() ? (type_ == Type::LOC || type_ == Type::VOR ? "not heard yet" : "")
                                          : ident_ + "  " + ident_code_ + fmt("  (x%.0f, ", ident_count_) +
                                                fmt("%.0f s ago)", host.time() - ident_t_));
        std::string mod;
        const std::string ils = fmt("90 Hz %.1f %%", main_.m90 * 100) + fmt(", 150 Hz %.1f %%", main_.m150 * 100);
        const std::string vor = fmt("30 Hz %.1f %%", main_.m30 * 100) + fmt(", 9960 Hz %.1f %%", main_.sc * 100);
        if (type_ == Type::LOC || type_ == Type::GS) mod = ils;
        else if (type_ == Type::VOR) mod = vor + fmt(" (FM +-%.0f Hz)", main_.dev_hz);
        else if (type_ == Type::MARKER && marker_tone_ >= 0) mod = fmt("%.0f Hz tone ", TONES[marker_tone_]) + pct(keys_[marker_tone_].on) + " (keyed)";
        else mod = ils + ", " + vor;
        if ((type_ == Type::LOC || type_ == Type::VOR) && keys_[T1020].keyed) mod += fmt(", ident tone %.0f %%", keys_[T1020].on * 100);
        host.fact("Modulation", mod);
        if (type_ == Type::LOC) ils_facts(main_, "Course", false);
        else host.fact("Course", "");
        if (type_ == Type::GS) ils_facts(main_, "Glide path", true);
        else host.fact("Glide path", "");
        if (have_second_ && (type_ == Type::LOC || type_ == Type::GS)) {
            const double d = second_.m90 - second_.m150;
            host.fact("Second carrier", fmt("%+.1f kHz from the first", (second_.off_hz - main_.off_hz) / 1e3) +
                                            fmt(", %.0f dB weaker", main_.level_db - second_.level_db) +
                                            fmt(": DDM %+.3f", d) + fmt(", SDM %.1f %%", (second_.m90 + second_.m150) * 100) +
                                            " (a two-frequency system: course + clearance; an aircraft's receiver follows the stronger)");
        } else {
            host.fact("Second carrier", "");
        }
        if (type_ == Type::VOR && std::abs(rad_) > 0.05) {
            double r = wrap360(std::arg(rad_) * 180 / M_PI);
            if (r >= 359.95) r -= 360;   // shown as 0.0, not 360.0
            std::string s = fmt("%.1f° from the station (magnetic)", r) + fmt("; the station bears %.1f° magnetic from here", wrap360(r + 180));
            if (std::isfinite(magvar_)) s += fmt(", %.1f° true", wrap360(r + 180 + magvar_));
            host.fact("Radial", s);
        } else {
            host.fact("Radial", "");
        }
        if (type_ == Type::MARKER && marker_tone_ >= 0) {
            const char* which = marker_tone_ == T400 ? "outer marker (OM)" : marker_tone_ == T1300 ? "middle marker (MM)" : "inner marker (IM) or fan marker";
            host.fact("Marker", std::string(which) + fmt(", %.0f Hz", TONES[marker_tone_]) +
                                    (pats_[marker_tone_].text().empty() ? "" : ", " + pats_[marker_tone_].text()));
        } else {
            host.fact("Marker", "");
        }
        host.fact("Audio", nav && (type_ == Type::LOC || type_ == Type::VOR) && main_.voice > 0.05
                               ? fmt("voice / other audio at %.0f %% (e.g. ATIS)", main_.voice * 100) : "");
    }
};

}  // namespace

KRAKEN_PLUGIN(Ils, {.id = "ils",
                    .name = "ILS / VOR / marker",
                    .description = "Aviation navigation aids: ILS localizer (108-112 MHz) and glide slope (329-335 MHz) - "
                                   "course / glide path deviation (DDM), two-carrier systems; VOR (108-118 MHz) - radial; "
                                   "marker beacons (75 MHz); Morse idents; the paired glide slope / DME channel. A 48 kHz VFO "
                                   "on the station's frequency",
                    .version = "1.0",
                    .sample_rate = FS,
                    .min_vfo_rate = FS,
                    .author = "KrakenSDR",
                    .options = {{"magvar", "Magnetic variation", "", "",
                                 "Degrees east (west negative) at the station, e.g. 20 for Auckland: the VOR bearing is also "
                                 "given as true (as the DoA shows it)"}}})
