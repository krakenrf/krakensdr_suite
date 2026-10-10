// DME / TACAN decoder: every DME channel inside the VFO's band.
//
// DME (Distance Measuring Equipment) ground beacons reply to aircraft on
// 962..1213 MHz, 1 MHz channels (1X..126Y), with pairs of 3.5 us pulses: 12
// us apart in X mode, 30 us in Y mode, ~700..2700 pairs per second (random
// "squitter" filler plus the replies). About every 30-40 s the beacon sends
// its Morse ident: during each dot / dash the pairs come at exactly 1350 per
// second. Aircraft interrogate on 1025..1150 MHz (12 us pairs in X mode, 36
// us in Y mode). A TACAN beacon is a DME whose pulses are also amplitude
// modulated at 15 and 135 Hz by a rotating antenna pattern, with reference
// bursts (north: 12 pairs / 13 pulses, 15 per second; auxiliary: 6 pairs /
// 13 pulses, 8 more per turn) - the phase of the modulation against the
// bursts is the bearing from the beacon.
//
// Input 2.4 MHz (the VFO's "No decimation" bandwidth covers up to three
// channels; a narrower VFO is resampled to it). From the VFO's RF
// (Host::rf_hz) the plugin knows which channels lie in its band; each is
// cut out with an FFT channelizer (overlap-save, 1.2 MHz per channel, flat
// to +-350 kHz) and goes through:
//   pulses: magnitude above min_snr over the noise (the median magnitude),
//     a peak with a half-amplitude width of 2.4-5.6 us (SSR / Mode S pulses
//     are narrower), leading-edge time at half amplitude (interpolated)
//   pairs: 12 / 30 / 36 us apart (+-1 us), amplitudes within 4 dB. What a
//     pair is follows from the frequency band: a ground reply (12 us on the
//     X reply bands, 30 us on the Y ones) or an aircraft interrogation
//   ident: a reply pair with another one 1/1350 s earlier (+-5 us) is part
//     of an ident key-down; per 10 ms the key is down when most pairs are
//     -> Morse decoder (lib/navaid)
//   TACAN: runs of equally spaced pairs / pulses = reference bursts; the
//     north bursts repeat at 15 Hz. The other pulses' amplitudes, against
//     the rotation phase from the north bursts, are fitted with 15 + 135 Hz
//     sinusoids -> coarse + fine bearing (the north burst goes out when the
//     pattern's maximum points east; it turns clockwise)
// Everything a beacon sends is reported: ident, channel and its pairing
// (VOR / ILS localizer / glide slope), pulse spacing, width and rise time,
// pairs per second, level, SNR, the carrier's exact frequency, TACAN
// bearing and modulation. Some of each beacon's clean reply pairs are
// reported as kp::Talker packets: every beacon gets its own DoA on the 🗺 Map.

#include "kraken_plugin.hpp"
#include "navaid.hpp"

#include <fftw3.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using kp::cf;

constexpr double FS = 2.4e6;
constexpr int NFFT = 4096;              // channelizer FFT at the input rate
constexpr int DEC = 2;                  // -> 1.2 MHz per channel
constexpr int NOUT = NFFT / DEC;
constexpr int OVL = 1024;               // overlap-save run-in (>> the filter's impulse response)
constexpr int HOP = NFFT - OVL;
constexpr double FS_CH = FS / DEC;
constexpr double BIN_HZ = FS / NFFT;
constexpr double PASS_HZ = 350e3, STOP_HZ = 550e3;   // channel filter
constexpr double CH_MAX_OFF_HZ = 0.8e6;              // channels this close to the centre are decoded

// pulses (us)
constexpr double W_MIN_US = 2.4, W_MAX_US = 5.6;     // half-amplitude width (DME 3.5 +- 0.5)
constexpr double PAIR_TOL_US = 1.0;
constexpr double PAIR_DB = 4.0;                       // amplitude difference within a pair
constexpr double SPACING_US[3] = {12, 30, 36};
enum Kind { K12 = 0, K30 = 1, K36 = 2 };
const char* KIND_US[3] = {"12 us", "30 us", "36 us"};

// ident
constexpr double IDENT_PERIOD_US = 1e6 / 1350;
constexpr double IDENT_TOL_US = 5.0;
constexpr double TICK_S = 0.010;

// TACAN
constexpr double MRB_PERIOD_S = 1.0 / 15;
constexpr double TACAN_WINDOW_S = 3.0;               // bearing fit over this much
constexpr double TACAN_HOLD_S = 0.1;                 // pulses wait this long for their burst times

constexpr double REPORT_S = 1.0;
constexpr double ACTIVE_S = 5.0;                     // a channel without pairs this long is quiet

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
    return b;
}
double db20(double v) { return 20 * std::log10(std::max(v, 1e-12)); }
double wrap360(double d) { return std::fmod(std::fmod(d, 360.0) + 360.0, 360.0); }

struct Pulse {
    double t;          // leading edge at half amplitude (Host::time() s)
    double amp;        // peak magnitude
    double width_us, rise_us;
    std::complex<double> z;   // sum of y[k+1] conj(y[k]) over the pulse (carrier frequency)
    bool paired = false, burst = false;
};

struct Pair {
    double t1, t2;
    double amp;
    Kind kind;
    double w1, w2, rise;
};

// What the pairs of one kind on one frequency are
struct Role {
    bool reply = false;           // a ground beacon's replies (else interrogations, or unknown)
    nav::DmeChannel ch;           // their channel (invalid: unknown / not a DME frequency)
};

Role role_of(int mhz, Kind k) {
    Role r;
    if (mhz <= 0) {   // frequency unknown: 12 / 30 us pairs are taken as replies
        r.reply = k != K36;
        return r;
    }
    const bool xband = (mhz >= 962 && mhz <= 1024) || (mhz >= 1151 && mhz <= 1213);
    const bool iband = mhz >= 1025 && mhz <= 1150;
    if (k == K12 && xband) r = {true, nav::dme_by_reply(mhz)};
    else if (k == K30 && iband) r = {true, nav::dme_by_reply(mhz)};
    else if (k == K12 && iband) r = {false, nav::dme_by_interrogation(mhz, 'X')};
    else if (k == K36 && iband) r = {false, nav::dme_by_interrogation(mhz, 'Y')};
    return r;
}

// Per pair kind on a channel: counters over the current report interval
struct KindStats {
    uint64_t total = 0;
    uint64_t n = 0;                      // this interval
    double amp = 0, w = 0, rise = 0, sp = 0;
    std::vector<float> lv;               // pair levels (dB) this interval (interrogations: aircraft count)
    double rate = 0;                     // pairs / s (last interval)
    double last_t = -1e9, first_t = -1;
    double amp_v = 0, w_v = 0, rise_v = 0, sp_v = 0;   // last interval's means
    int sources = 0;                     // interrogations: distinct levels = about this many aircraft
    std::string levels;                  // ... their levels
    bool announced = false, shown = false;
};

// Least-squares fit of the TACAN pattern: a(psi) = c0 + c1 cos psi + s1 sin psi + c9 cos 9psi + s9 sin 9psi
struct TacanFit {
    double A[5][5] = {}, b[5] = {};
    double n = 0;
    void add(double psi, double a, double w) {
        const double f[5] = {1, std::cos(psi), std::sin(psi), std::cos(9 * psi), std::sin(9 * psi)};
        for (int i = 0; i < 5; i++) {
            b[i] += w * f[i] * a;
            for (int j = 0; j < 5; j++) A[i][j] += w * f[i] * f[j];
        }
        n += w;
    }
    void decay(double k) {
        for (int i = 0; i < 5; i++) {
            b[i] *= k;
            for (int j = 0; j < 5; j++) A[i][j] *= k;
        }
        n *= k;
    }
    bool solve(double x[5]) const {
        double M[5][6];
        for (int i = 0; i < 5; i++) {
            for (int j = 0; j < 5; j++) M[i][j] = A[i][j];
            M[i][5] = b[i];
        }
        for (int c = 0; c < 5; c++) {
            int p = c;
            for (int r = c + 1; r < 5; r++)
                if (std::fabs(M[r][c]) > std::fabs(M[p][c])) p = r;
            if (std::fabs(M[p][c]) < 1e-12) return false;
            for (int j = 0; j < 6; j++) std::swap(M[c][j], M[p][j]);
            for (int r = 0; r < 5; r++) {
                if (r == c) continue;
                const double f = M[r][c] / M[c][c];
                for (int j = c; j < 6; j++) M[r][j] -= f * M[c][j];
            }
        }
        for (int i = 0; i < 5; i++) x[i] = M[i][5] / M[i][i];
        return true;
    }
};

struct Burst {
    double t;       // first pulse
    int pulses;
    double spacing_us;
    bool north;
};

// One DME frequency in the band
struct Channel {
    int mhz = 0;                    // 0 = unknown (no RF)
    double off_hz = 0;              // in the plugin's input band
    int bin = 0;                    // channelizer centre bin
    std::vector<int> src;           // the input FFT bin of each IFFT bin
    // channel samples: y_[i] is channel sample y0_ + i (channel sample k = input sample in0 + DEC k)
    std::vector<cf> y;
    std::vector<float> a;
    int64_t y0 = 0;
    size_t scan = 0;
    // noise: median magnitude
    std::vector<float> nsamp;
    double noise = 0;
    // pulses / pairs
    std::deque<Pulse> recent;       // unpaired pulses of the last 40 us
    double last_t = -1, last2_t = -1;   // the latest two pulses (isolation of DoA pairs)
    KindStats ks[3];
    std::complex<double> zsum{0, 0};   // reply pulses' carrier phase steps, this interval
    double zweight = 0;
    double carrier_hz = NAN;        // measured carrier offset from the channel's centre (input band, Hz)
    // ident
    std::deque<double> ipairs;      // reply pair times, last few ms
    int64_t tick = -1;
    int tick_n = 0, tick_m = 0;
    bool k1 = false, k2 = false;    // last two raw tick states (3-tick majority)
    double ident_lag_sum = 0;
    int ident_lag_n = 0;
    nav::MorseDecoder morse{TICK_S, 0.07, 0.20, 1.2};
    std::string ident, ident_code, last_decode;
    int ident_count = 0;
    double ident_t = -1, ident_unit = 0, ident_quality = 0, ident_rate = 0;
    double key_pairs = 0, key_time = 0;   // pairs counted / time spent with the key down (ident pair rate)
    // TACAN
    std::deque<Pulse> tpulses;      // reply pulses waiting for the burst times around them
    std::deque<Burst> bursts;       // last few seconds
    std::vector<double> run;        // the current run of equally spaced pairs (their t1)
    std::vector<double> prun;       // ... of equally spaced single pulses
    TacanFit fit;
    double tacan_brg = NAN, tacan_coarse = NAN, tacan_m15 = 0, tacan_m135 = 0, tacan_rot_hz = 0;
    int north_n = 0, aux_n = 0;
    bool tacan = false, tacan_announced = false;
    double fit_t = 0;
    // DoA
    double doa_tokens = 0, doa_t = 0;
    bool pend = false;
    Pair pend_pair{};
    // reporting
    bool active = false, announced = false;
    double last_any = -1e9;
};

class Dme : public kp::Decoder {
public:
    explicit Dme(kp::Host& h) : Decoder(h) {
        fin_ = fftwf_alloc_complex(NFFT);
        fout_ = fftwf_alloc_complex(NFFT);
        cin_ = fftwf_alloc_complex(NOUT);
        cout_ = fftwf_alloc_complex(NOUT);
        fwd_ = fftwf_plan_dft_1d(NFFT, fin_, fout_, FFTW_FORWARD, FFTW_ESTIMATE);
        inv_ = fftwf_plan_dft_1d(NOUT, cin_, cout_, FFTW_BACKWARD, FFTW_ESTIMATE);
        // channel filter over the kept bins (relative bin r = -NOUT/2 .. NOUT/2 - 1)
        H_.resize(NOUT);
        for (int r = -NOUT / 2; r < NOUT / 2; r++) {
            const double f = std::fabs(r * BIN_HZ);
            double h = f <= PASS_HZ ? 1.0 : f >= STOP_HZ ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * (f - PASS_HZ) / (STOP_HZ - PASS_HZ));
            H_[(r + NOUT) % NOUT] = static_cast<float>(h / NFFT);
        }
        host.table_columns({"Channel", "Reply MHz", "Signal", "Ident", "Pairs/s", "Level dBFS", "SNR dB", "Spacing us",
                            "Width us", "Carrier kHz", "Paired with", "TACAN", "Ident heard"});
    }
    ~Dme() override {
        fftwf_destroy_plan(fwd_);
        fftwf_destroy_plan(inv_);
        fftwf_free(fin_);
        fftwf_free(fout_);
        fftwf_free(cin_);
        fftwf_free(cout_);
    }

    void option(const std::string& k, const std::string& v) override {
        if (k == "min_snr") min_snr_db_ = std::clamp(v.empty() ? 10.0 : atof(v.c_str()), 4.0, 30.0);
        else if (k == "doa") doa_ = v != "0";
        else if (k == "doa_rate") doa_rate_ = std::clamp(v.empty() ? 100.0 : atof(v.c_str()), 5.0, 1000.0);
    }

    void reset() override {
        for (auto& c : chans_)
            for (int k = 0; k < 3; k++)
                if (c->ks[k].shown) host.table_remove(row_key(*c, k));
        chans_.clear();
        chan_rf_ = NAN;
        need_setup_ = true;
    }

    void process(const cf* x, size_t n) override {
        const double rf = host.rf_hz();
        if (need_setup_ || (std::isfinite(rf) != std::isfinite(chan_rf_)) ||
            (std::isfinite(rf) && std::fabs(rf - chan_rf_) > 1000))
            setup(rf);
        buf_.insert(buf_.end(), x, x + n);
        abs_in_ += n;
        while (buf_.size() >= static_cast<size_t>(NFFT)) {
            block();
            buf_.erase(buf_.begin(), buf_.begin() + HOP);
            base_ += HOP;
        }
        const double now = host.time();
        // valid frames: pulse pairs coming (every 0.25 s - the Digital
        // squelch stays open 1.5 s after one)
        if (now - valid_t_ >= 0.25) {
            if (valid_pairs_ >= 3) host.valid();
            valid_pairs_ = 0;
            valid_t_ = now;
        }
        if (now - report_t_ >= REPORT_S) {
            report_t_ = now;
            report(now);
        }
    }

private:
    // channelizer
    fftwf_complex *fin_, *fout_, *cin_, *cout_;
    fftwf_plan fwd_, inv_;
    std::vector<float> H_;
    std::vector<cf> buf_;           // input; buf_[0] is input sample base_
    int64_t base_ = 0;
    int64_t abs_in_ = 0;            // input samples received (Host::time() * FS)
    int64_t in0_ = 0;               // input sample of channel sample 0
    std::vector<std::unique_ptr<Channel>> chans_;
    double chan_rf_ = NAN;
    bool need_setup_ = true;
    // options
    double min_snr_db_ = 10;
    bool doa_ = true;
    double doa_rate_ = 100;
    double report_t_ = 0, valid_t_ = 0;
    uint64_t valid_pairs_ = 0;
    std::string main_key_;          // the channel the facts describe

    double t_of(double ch_sample) const { return (static_cast<double>(in0_) + DEC * ch_sample) / FS; }

    void setup(double rf) {
        reset();
        need_setup_ = false;
        chan_rf_ = rf;
        // the channelizer starts over at the current input position
        buf_.assign(OVL, cf(0, 0));
        base_ = abs_in_ - OVL;
        in0_ = abs_in_;
        std::vector<std::pair<int, double>> want;   // (MHz, offset in the input band)
        if (std::isfinite(rf)) {
            const int lo = static_cast<int>(std::ceil((rf - CH_MAX_OFF_HZ) / 1e6));
            const int hi = static_cast<int>(std::floor((rf + CH_MAX_OFF_HZ) / 1e6));
            for (int f = std::max(lo, 962); f <= std::min(hi, 1213); f++) {
                const double off = (f * 1e6 - rf) * (host.rf_inverted() ? -1 : 1);
                want.emplace_back(f, off);
            }
            if (want.empty())
                host.fact("Channel", fmt("%.3f MHz is not a DME frequency (962 - 1213 MHz): decoding the VFO's centre", rf / 1e6));
        }
        if (want.empty()) want.emplace_back(0, 0.0);
        for (auto& w : want) {
            auto c = std::make_unique<Channel>();
            c->mhz = w.first;
            c->off_hz = w.second;
            c->bin = static_cast<int>(std::lround(w.second / BIN_HZ));
            c->src.resize(NOUT);
            for (int r = -NOUT / 2; r < NOUT / 2; r++) c->src[(r + NOUT) % NOUT] = ((c->bin + r) % NFFT + NFFT) % NFFT;
            c->morse = nav::MorseDecoder(TICK_S, 0.07, 0.20, 1.2);
            chans_.push_back(std::move(c));
        }
        std::string list;
        for (auto& c : chans_) list += (list.empty() ? "" : ", ") + (c->mhz ? std::to_string(c->mhz) + " MHz" : std::string("VFO centre"));
        host.log("decoding " + list);
    }

    // one overlap-save block: FFT, then every channel's bins -> IFFT -> detector
    void block() {
        static_assert(sizeof(cf) == sizeof(fftwf_complex));
        std::copy(buf_.begin(), buf_.begin() + NFFT, reinterpret_cast<cf*>(fin_));
        fftwf_execute(fwd_);
        for (auto& cp : chans_) {
            Channel& c = *cp;
            for (int o = 0; o < NOUT; o++) {
                const int k = c.src[o];
                cin_[o][0] = fout_[k][0] * H_[o];
                cin_[o][1] = fout_[k][1] * H_[o];
            }
            fftwf_execute(inv_);
            // the shift by c.bin is relative to this block's first sample:
            // turn it back to one continuous phase (carrier measurement)
            const double ph = -2 * M_PI * static_cast<double>(c.bin) * static_cast<double>(base_ % NFFT) / NFFT;
            const cf rot(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            const size_t at = c.y.size();
            c.y.resize(at + (NFFT - OVL) / DEC);
            for (int m = 0; m < (NFFT - OVL) / DEC; m++)
                c.y[at + m] = cf(cout_[OVL / DEC + m][0], cout_[OVL / DEC + m][1]) * rot;
            detect(c);
        }
    }

    // --- pulses ----------------------------------------------------------------
    void detect(Channel& c) {
        const size_t n = c.y.size();
        const size_t from = c.a.size();   // new samples
        c.a.resize(n);
        for (size_t i = from; i < n; i++) c.a[i] = std::sqrt(std::norm(c.y[i]));
        // noise: median magnitude of every 7th sample, ~every 50 ms
        for (size_t i = from; i < n; i += 7) c.nsamp.push_back(c.a[i]);
        if (c.nsamp.size() >= 8000) {
            auto mid = c.nsamp.begin() + c.nsamp.size() / 2;
            std::nth_element(c.nsamp.begin(), mid, c.nsamp.end());
            c.noise = c.noise > 0 ? 0.7 * c.noise + 0.3 * *mid : *mid;
            c.nsamp.clear();
        }
        constexpr size_t TAIL = 12;   // a pulse needs this many samples after its peak
        if (c.noise <= 0) {
            keep_tail(c, n > 64 ? n - 64 : 0);
            return;
        }
        // threshold: SNR (peak power over the noise power median^2 / ln 2)
        const float thr = static_cast<float>(c.noise * std::sqrt(std::pow(10.0, min_snr_db_ / 10) / std::log(2.0)));
        size_t i = std::max<size_t>(c.scan, 3);
        for (; i + TAIL < n; i++) {
            const float v = c.a[i];
            if (v < thr || v < c.a[i - 1] || v < c.a[i + 1]) continue;
            // the highest sample within +-3 (2.5 us)
            bool top = true;
            for (int d = -3; d <= 3 && top; d++)
                if (d && c.a[i + d] > v) top = false;
            if (!top) continue;
            Pulse p;
            if (measure(c, i, &p)) on_pulse(c, p);
            i += 3;
        }
        c.scan = i;
        keep_tail(c, c.scan > 16 ? c.scan - 16 : 0);
    }

    void keep_tail(Channel& c, size_t from) {
        if (from == 0) return;
        c.y.erase(c.y.begin(), c.y.begin() + static_cast<long>(from));
        c.a.erase(c.a.begin(), c.a.begin() + static_cast<long>(from));
        c.y0 += static_cast<int64_t>(from);
        c.scan -= std::min(c.scan, from);
    }

    // half-amplitude edges, width, rise time and carrier phase steps of the pulse peaking at i
    bool measure(const Channel& c, size_t i, Pulse* p) const {
        const float* a = c.a.data();
        // parabolic peak
        const double l = a[i - 1], m = a[i], r = a[i + 1];
        const double den = l - 2 * m + r;
        const double d = den < 0 ? 0.5 * (l - r) / den : 0;
        const double pk = m - 0.25 * (l - r) * d;
        auto cross = [&](double level, int dir, double* at) {
            for (int k = 1; k <= 8; k++) {
                const long j = static_cast<long>(i) + dir * k;
                if (j < 1 || j >= static_cast<long>(c.a.size())) return false;
                if (a[j] < level) {
                    const double a0 = a[j], a1 = a[j - dir];
                    *at = j - dir + dir * (a1 - level) / std::max(a1 - a0, 1e-12);
                    return true;
                }
            }
            return false;
        };
        double le, te, l10, l90;
        if (!cross(0.5 * pk, -1, &le) || !cross(0.5 * pk, +1, &te)) return false;
        p->width_us = (te - le) / FS_CH * 1e6;
        if (p->width_us < W_MIN_US || p->width_us > W_MAX_US) return false;
        p->rise_us = cross(0.1 * pk, -1, &l10) && cross(0.9 * pk, -1, &l90) ? (l90 - l10) / FS_CH * 1e6 : NAN;
        p->t = t_of(static_cast<double>(c.y0) + le);
        p->amp = pk;
        std::complex<double> z{0, 0};
        for (long k = static_cast<long>(std::ceil(le)); k + 1 <= static_cast<long>(te); k++)
            z += std::complex<double>(c.y[k + 1] * std::conj(c.y[k]));
        p->z = z;
        return true;
    }

    // --- pairs -----------------------------------------------------------------
    void on_pulse(Channel& c, Pulse p) {
        while (!c.recent.empty() && p.t - c.recent.front().t > 40e-6) c.recent.pop_front();
        // the pending DoA pair is used if nothing came right after it
        if (c.pend && p.t - c.pend_pair.t2 > 15e-6) doa_packet(c, c.pend_pair);
        c.pend = false;
        int best = -1;
        Kind bk = K12;
        double bdev = 1e9;
        for (size_t j = 0; j < c.recent.size(); j++) {
            const Pulse& q = c.recent[j];
            if (q.paired) continue;
            const double dt = (p.t - q.t) * 1e6;
            if (std::fabs(db20(p.amp / q.amp)) > PAIR_DB) continue;
            for (int k = 0; k < 3; k++) {
                const double dev = std::fabs(dt - SPACING_US[k]);
                if (dev <= PAIR_TOL_US && dev < bdev) {
                    bdev = dev;
                    best = static_cast<int>(j);
                    bk = static_cast<Kind>(k);
                }
            }
        }
        tacan_pulse_run(c, p);
        if (best >= 0) {
            Pulse& q = c.recent[best];
            q.paired = true;
            p.paired = true;
            Pair pr{q.t, p.t, 0.5 * (q.amp + p.amp), bk, q.width_us, p.width_us, q.rise_us};
            on_pair(c, pr, q, p);
        }
        c.recent.push_back(p);
        c.last2_t = c.last_t;
        c.last_t = p.t;
    }

    void on_pair(Channel& c, const Pair& pr, const Pulse& q, const Pulse& p) {
        KindStats& s = c.ks[pr.kind];
        s.total++;
        s.n++;
        s.amp += pr.amp;
        s.w += 0.5 * (pr.w1 + pr.w2);
        if (std::isfinite(pr.rise)) s.rise += pr.rise;
        s.sp += (pr.t2 - pr.t1) * 1e6;
        if (s.lv.size() < 20000) s.lv.push_back(static_cast<float>(db20(pr.amp)));
        if (s.first_t < 0) s.first_t = pr.t1;
        s.last_t = pr.t2;
        c.last_any = pr.t2;
        valid_pairs_++;
        const Role role = role_of(c.mhz, pr.kind);
        if (!role.reply) return;
        // carrier: phase steps inside both pulses
        c.zsum += q.z + p.z;
        c.zweight += 1;
        ident_pair(c, pr);
        tacan_pair(c, pr, q, p);
        // DoA: a clean, strong pair - no other pulse between its two, none
        // in the 15 us before it (and, checked when the next pulse comes,
        // after it)
        const bool clean = std::fabs(c.last_t - q.t) < 1e-9 && q.t - c.last2_t > 15e-6;
        if (doa_ && clean && snr_db(c, pr.amp) >= 15) {
            c.pend = true;
            c.pend_pair = pr;
        }
    }

    double snr_db(const Channel& c, double amp) const {
        return c.noise > 0 ? 10 * std::log10(amp * amp * std::log(2.0) / (c.noise * c.noise)) : 0;
    }

    void doa_packet(Channel& c, const Pair& pr) {
        // token bucket: doa_rate_ pairs per second
        c.doa_tokens = std::min(doa_rate_ * 0.2, c.doa_tokens + (pr.t1 - c.doa_t) * doa_rate_);
        c.doa_t = pr.t1;
        if (c.doa_tokens < 1) return;
        c.doa_tokens -= 1;
        const Role role = role_of(c.mhz, pr.kind);
        kp::Talker t;
        t.id = role.ch.valid() ? role.ch.name() : (c.mhz ? std::to_string(c.mhz) + " MHz" : "DME");
        t.label = (c.ident.empty() ? std::string() : c.ident + " · ") + (c.mhz ? std::to_string(c.mhz) + " MHz" : "DME");
        t.packet = true;
        // both pulses with some margin: >= 2 whole 16-sample covariance chunks
        t.start_s = pr.t1 - 4e-6;
        t.end_s = pr.t2 + 8e-6;
        t.freq_hz = c.off_hz;
        t.bw_hz = 2 * PASS_HZ;
        t.avg_s = 10;   // a fixed station
        host.talker(t);
    }

    // --- ident ---------------------------------------------------------------
    void ident_pair(Channel& c, const Pair& pr) {
        const double period = c.ident_lag_n > 50 ? c.ident_lag_sum / c.ident_lag_n : IDENT_PERIOD_US;
        while (!c.ipairs.empty() && pr.t1 - c.ipairs.front() > 3e-3) c.ipairs.pop_front();
        bool periodic = false;
        for (auto it = c.ipairs.rbegin(); it != c.ipairs.rend(); ++it) {
            const double lag = (pr.t1 - *it) * 1e6;
            if (lag > period + IDENT_TOL_US) break;
            if (std::fabs(lag - period) <= IDENT_TOL_US) {
                periodic = true;
                if (c.k1 && c.k2) {   // refine the ident period while the key is down
                    c.ident_lag_sum += lag;
                    c.ident_lag_n++;
                    if (c.ident_lag_n > 2000) {
                        c.ident_lag_sum *= 0.5;
                        c.ident_lag_n /= 2;
                    }
                }
                break;
            }
        }
        c.ipairs.push_back(pr.t1);
        const int64_t tk = static_cast<int64_t>(std::floor(pr.t2 / TICK_S));
        advance_ticks(c, tk);
        c.tick_n++;
        c.tick_m += periodic;
    }

    // close the 10 ms ticks before tick tk
    void advance_ticks(Channel& c, int64_t tk) {
        if (c.tick < 0) c.tick = tk;
        int guard = 0;
        while (c.tick < tk && guard++ < 1000) {
            const bool raw = c.tick_n >= 6 && c.tick_m * 2 >= c.tick_n;
            // 3-tick majority (one tick late)
            const bool key = static_cast<int>(raw) + c.k1 + c.k2 >= 2;
            if (key) {
                c.key_pairs += c.tick_n;
                c.key_time += TICK_S;
            }
            c.k2 = c.k1;
            c.k1 = raw;
            if (c.morse.push(key)) ident_done(c, (c.tick + 1) * TICK_S);
            c.tick++;
            c.tick_n = c.tick_m = 0;
        }
        if (c.tick < tk) c.tick = tk;   // a long silence
    }

    void ident_done(Channel& c, double t) {
        const std::string txt = c.morse.text();
        if (txt.empty() || txt.size() > 8) return;
        const bool clean = txt.find('?') == std::string::npos;
        if (host.verbose())
            host.log(fmt("%.0f MHz ident: ", c.mhz) + txt + " (" + c.morse.code() + fmt(", dot %.0f ms", c.morse.unit_s() * 1000) +
                     fmt(", fit %.2f)", c.morse.quality()));
        if (!clean || c.morse.quality() < 0.6) return;
        const bool confirmed = txt == c.last_decode || txt == c.ident;
        c.last_decode = txt;
        c.ident = txt;
        c.ident_code = c.morse.code();
        c.ident_count++;
        c.ident_t = t;
        c.ident_unit = c.morse.unit_s();
        c.ident_quality = c.morse.quality();
        c.ident_rate = c.key_time > 0 ? c.key_pairs / c.key_time : 0;
        c.key_pairs = c.key_time = 0;
        const Role role = role_of(c.mhz, c.mhz ? (in_iband(c.mhz) ? K30 : K12) : K12);
        std::string where = role.ch.valid() ? "DME " + role.ch.name() + " (" + std::to_string(c.mhz) + " MHz)" : "DME";
        std::string pair = role.ch.valid() ? nav::pairing_text(role.ch) : "";
        host.event("Ident " + txt + " [" + c.ident_code + "] - " + where + (pair.empty() ? "" : ", paired with " + pair) +
                       (confirmed ? "" : " (first copy)"),
                   0.0);
        if (host.raw_wanted()) host.raw(where + " ident " + txt + " " + c.ident_code);
    }

    static bool in_iband(int mhz) { return mhz >= 1025 && mhz <= 1150; }

    // --- TACAN ---------------------------------------------------------------
    // runs of single pulses at a steady 12 / 15 / 30 us spacing: Y-mode
    // reference bursts (13 pulses at 30 / 15 us), or X bursts whose pairs
    // follow each other at 24 us (pulses every 12 us)
    void tacan_pulse_run(Channel& c, const Pulse& p) {
        auto& r = c.prun;
        if (!r.empty()) {
            const double gap = (p.t - r.back()) * 1e6;
            const double sp = r.size() >= 2 ? (r[1] - r[0]) * 1e6 : gap;
            if (std::fabs(gap - sp) <= PAIR_TOL_US && (r.size() >= 2 || (gap >= 11 && gap <= 31))) {
                r.push_back(p.t);
                return;
            }
            end_pulse_run(c);
        }
        r.assign(1, p.t);
    }
    void end_pulse_run(Channel& c) {
        auto& r = c.prun;
        if (r.size() >= 9) {
            const double sp = (r.back() - r.front()) / (r.size() - 1) * 1e6;
            Burst b{r.front(), static_cast<int>(r.size()), sp, false};
            if (std::fabs(sp - 12) < 1.5) b.north = r.size() >= 18;           // X: 24 pulses north, 12 auxiliary
            else if (std::fabs(sp - 30) < 1.5) b.north = true;                // Y north: 13 at 30 us
            else if (std::fabs(sp - 15) < 1.5) b.north = false;               // Y auxiliary: 13 at 15 us
            else {
                r.clear();
                return;
            }
            on_burst(c, b);
        }
        r.clear();
    }
    // runs of equally spaced 12 us pairs (X reference bursts: pairs every
    // 20..35 us; 12 north, 6 auxiliary)
    void tacan_pair(Channel& c, const Pair& pr, const Pulse&, const Pulse&) {
        // the pulses wait for the bursts around them (bearing fit)
        auto& r = c.run;
        if (pr.kind == K12) {
            if (!r.empty()) {
                const double gap = (pr.t1 - r.back()) * 1e6;
                const double sp = r.size() >= 2 ? (r[1] - r[0]) * 1e6 : gap;
                if (gap >= 20 && gap <= 36 && std::fabs(gap - sp) <= PAIR_TOL_US) {
                    r.push_back(pr.t1);
                } else {
                    end_pair_run(c);
                    r.assign(1, pr.t1);
                }
            } else {
                r.assign(1, pr.t1);
            }
        }
        c.tpulses.push_back(Pulse{pr.t1, pr.amp, 0, 0, {}, true, false});
        tacan_fit(c, pr.t2);
    }
    void end_pair_run(Channel& c) {
        auto& r = c.run;
        if (r.size() >= 5) {
            const double sp = (r.back() - r.front()) / (r.size() - 1) * 1e6;
            on_burst(c, Burst{r.front(), static_cast<int>(r.size()) * 2, sp, r.size() >= 9});
        }
        r.clear();
    }
    void on_burst(Channel& c, const Burst& b) {
        // the same burst seen by both run detectors
        for (auto it = c.bursts.rbegin(); it != c.bursts.rend() && b.t - it->t < 1e-3; ++it)
            if (std::fabs(it->t - b.t) < 30e-6) return;
        c.bursts.push_back(b);
        (b.north ? c.north_n : c.aux_n)++;
        while (!c.bursts.empty() && b.t - c.bursts.front().t > TACAN_WINDOW_S + 1) c.bursts.pop_front();
        // the pulses of the burst don't sample the rotating pattern
        const double end = b.t + (b.pulses + 1) * b.spacing_us * 1e-6;
        for (auto& p : c.tpulses)
            if (p.t >= b.t - 1e-6 && p.t <= end) p.burst = true;
    }

    // the north bursts' times around t: (start of the turn, its length)
    bool turn_at(const Channel& c, double t, double* t0, double* T) const {
        const Burst* before = nullptr;
        const Burst* after = nullptr;
        for (const auto& b : c.bursts) {
            if (!b.north) continue;
            if (b.t <= t) before = &b;
            else if (!after) after = &b;
        }
        if (!before || !after) return false;
        const double len = after->t - before->t;
        if (std::fabs(len - MRB_PERIOD_S) > 0.05 * MRB_PERIOD_S) return false;   // a burst missed
        *t0 = before->t;
        *T = len;
        return true;
    }

    void tacan_fit(Channel& c, double now) {
        // pulses old enough that the next north burst has come
        while (!c.tpulses.empty() && now - c.tpulses.front().t > TACAN_HOLD_S) {
            const Pulse p = c.tpulses.front();
            c.tpulses.pop_front();
            double t0, T;
            if (p.burst || !turn_at(c, p.t, &t0, &T)) continue;
            const double psi = 2 * M_PI * (p.t - t0) / T;
            c.fit.add(psi, p.amp, 1.0);
        }
        if (now - c.fit_t < 0.5) return;
        c.fit_t = now;
        // north bursts at a steady 15 Hz = TACAN
        int good = 0;
        double sumT = 0;
        const Burst* prev = nullptr;
        for (const auto& b : c.bursts) {
            if (!b.north || now - b.t > 2.0) continue;
            if (prev && std::fabs(b.t - prev->t - MRB_PERIOD_S) < 0.05 * MRB_PERIOD_S) {
                good++;
                sumT += b.t - prev->t;
            }
            prev = &b;
        }
        c.tacan = good >= 10;
        c.tacan_rot_hz = good ? good / sumT : 0;
        if (c.tacan && c.fit.n > 300) {
            double x[5];
            if (c.fit.solve(x) && x[0] > 0) {
                c.tacan_m15 = std::hypot(x[1], x[2]) / x[0];
                c.tacan_m135 = std::hypot(x[3], x[4]) / x[0];
                // the maximum of the 15 Hz pattern passes the receiver phi
                // after the north burst (which goes out as it points east)
                const double phi1 = std::atan2(x[2], x[1]) * 180 / M_PI;
                const double coarse = wrap360(90 + phi1);
                const double phi9 = std::atan2(x[4], x[3]) * 180 / M_PI;
                double best = coarse, bd = 1e9;
                for (int k = 0; k < 9; k++) {
                    const double f = wrap360(90 + (phi9 + 360.0 * k) / 9);
                    const double d = std::fabs(std::remainder(f - coarse, 360.0));
                    if (d < bd) {
                        bd = d;
                        best = f;
                    }
                }
                c.tacan_coarse = coarse;
                c.tacan_brg = c.tacan_m135 > 0.02 && bd < 20 ? best : coarse;
            }
            c.fit.decay(std::exp(-0.5 / TACAN_WINDOW_S));
        }
        if (c.tacan && !c.tacan_announced && std::isfinite(c.tacan_brg)) {
            c.tacan_announced = true;
            host.event(fmt("TACAN on %.0f MHz", c.mhz) + fmt(": north reference bursts at %.2f Hz", c.tacan_rot_hz) +
                           fmt(", bearing from the beacon %.1f°", c.tacan_brg),
                       0.0);
        }
    }

    // --- reporting -------------------------------------------------------------
    // a kind of pairs worth showing on a channel: its replies, or
    // interrogations / other pairs that keep coming
    static bool shown(const KindStats& s, double now) { return s.total >= 20 && now - s.last_t < 60; }

    void report(double now) {
        Channel* main = nullptr;
        double main_rate = -1;
        std::string list, interr;
        for (auto& cp : chans_) {
            Channel& c = *cp;
            for (int k = 0; k < 3; k++) close_interval(c.ks[k]);
            if (c.zweight > 20) {
                const double f = std::arg(c.zsum) * FS_CH / (2 * M_PI);
                c.carrier_hz = f + c.bin * BIN_HZ - c.off_hz;   // the channel's 0 Hz is its centre bin
            }
            c.zsum = 0;
            c.zweight = 0;
            const int rk = reply_kind(c);
            const bool was = c.active;
            c.active = rk >= 0 && now - c.ks[rk].last_t < ACTIVE_S;
            if (!c.active && was) host.event(where(c) + ": no beacon replies for " + fmt("%.0f s", ACTIVE_S), 0.0);
            for (int k = 0; k < 3; k++) {
                KindStats& s = c.ks[k];
                if (!shown(s, now)) {
                    if (s.shown) host.table_remove(row_key(c, k));
                    s.shown = false;
                    continue;
                }
                if (!s.announced) announce(c, k);
                s.announced = s.shown = true;
                table(c, k, now);
                const Role r = role_of(c.mhz, static_cast<Kind>(k));
                if (!r.reply && now - s.last_t < ACTIVE_S)
                    interr += (interr.empty() ? "" : "; ") + std::to_string(c.mhz) + " MHz: " + signal_text(c, k) +
                              fmt(", %.0f pairs/s", s.rate) + (s.sources ? fmt(", about %.0f aircraft", s.sources) : "");
            }
            if (c.active) {
                list += (list.empty() ? "" : ", ") + where(c, false) + (c.ident.empty() ? "" : " " + c.ident);
                if (c.ks[rk].rate > main_rate) {
                    main = &c;
                    main_rate = c.ks[rk].rate;
                }
            }
        }
        if (!main && !chans_.empty()) main = chans_.front().get();
        if (main) facts(*main, now, list);
        host.fact("Interrogations", interr);
    }

    void close_interval(KindStats& s) {
        s.rate = s.n / REPORT_S;
        if (s.n) {
            s.amp_v = s.amp / s.n;
            s.w_v = s.w / s.n;
            s.rise_v = s.rise / s.n;
            s.sp_v = s.sp / s.n;
        }
        // interrogators: pair levels that cluster (gaps > 2 dB between
        // clusters, a cluster = 5 % of the pairs or 5 pairs)
        if (s.lv.size() >= 10) {
            std::sort(s.lv.begin(), s.lv.end());
            const size_t need = std::max<size_t>(5, s.lv.size() / 20);
            int n = 0;
            size_t start = 0;
            std::string lv;
            for (size_t i = 1; i <= s.lv.size(); i++) {
                if (i == s.lv.size() || s.lv[i] - s.lv[i - 1] > 2.0f) {
                    if (i - start >= need) {
                        n++;
                        lv = fmt("%.0f", s.lv[(start + i) / 2]) + (lv.empty() ? "" : ", ") + lv;
                    }
                    start = i;
                }
            }
            s.sources = n;
            s.levels = lv;
        }
        s.lv.clear();
        s.n = 0;
        s.amp = s.w = s.rise = s.sp = 0;
    }

    // the pair kind of this channel's beacon replies (-1: none heard)
    int reply_kind(const Channel& c) const {
        int best = -1;
        for (int k = 0; k < 3; k++)
            if (role_of(c.mhz, static_cast<Kind>(k)).reply && c.ks[k].total > 0 &&
                (best < 0 || c.ks[k].total > c.ks[best].total))
                best = k;
        return best;
    }

    std::string row_key(const Channel& c, int k) const { return std::to_string(c.mhz) + "/" + std::to_string(k); }

    std::string where(const Channel& c, bool long_form = true) const {
        if (!c.mhz) return "DME (VFO centre)";
        const nav::DmeChannel ch = nav::dme_by_reply(c.mhz);
        if (!long_form) return ch.valid() ? ch.name() : std::to_string(c.mhz) + " MHz";
        return std::to_string(c.mhz) + " MHz" + (ch.valid() ? " (reply of DME " + ch.name() + ")" : "");
    }

    std::string signal_text(const Channel& c, int k) const {
        const Role r = role_of(c.mhz, static_cast<Kind>(k));
        const std::string mode = k == K12 ? "X" : "Y";
        if (r.reply) return (c.tacan ? "TACAN" : "DME beacon") + std::string(" replies (") + mode + ", " + KIND_US[k] + ")";
        if (r.ch.valid()) return "aircraft interrogating " + r.ch.name() + " (" + KIND_US[k] + ")";
        return std::string(KIND_US[k]) + " pulse pairs (not a DME use of this frequency)";
    }

    void announce(Channel& c, int k) {
        const Role r = role_of(c.mhz, static_cast<Kind>(k));
        std::string s = "Heard on " + (c.mhz ? std::to_string(c.mhz) + " MHz" : std::string("the VFO centre")) + ": " +
                        signal_text(c, k);
        if (r.ch.valid()) s += std::string(r.reply ? " - DME " : " - ") + (r.reply ? r.ch.name() : "its beacon replies on " +
                               std::to_string(r.ch.reply_mhz) + " MHz") + ", paired with " + nav::pairing_text(r.ch);
        host.event(s, 0.0);
    }

    void table(Channel& c, int k, double now) {
        const KindStats& s = c.ks[k];
        const Role r = role_of(c.mhz, static_cast<Kind>(k));
        const bool rep = r.reply;
        std::string tacan;
        if (rep) tacan = c.tacan && std::isfinite(c.tacan_brg) ? fmt("%.1f°", c.tacan_brg) : (now - s.first_t > 5 ? "no" : "");
        std::string lvl = s.amp_v > 0 ? fmt("%.1f", db20(s.amp_v)) : "";
        if (!rep && s.sources > 1) lvl = s.levels;
        host.table_row(row_key(c, k),
                       {r.ch.valid() ? r.ch.name() : "", c.mhz ? std::to_string(c.mhz) : "",
                        signal_text(c, k) + (!rep && s.sources ? fmt(", about %.0f aircraft", s.sources) : ""),
                        rep ? c.ident : "", now - s.last_t < ACTIVE_S ? fmt("%.0f", s.rate) : "0", lvl,
                        s.amp_v > 0 ? fmt("%.0f", snr_db(c, s.amp_v)) : "", s.sp_v > 0 ? fmt("%.2f", s.sp_v) : "",
                        s.w_v > 0 ? fmt("%.2f", s.w_v) : "",
                        rep && std::isfinite(c.carrier_hz) ? fmt("%+.1f", c.carrier_hz / 1e3) : "",
                        r.ch.valid() ? nav::pairing_text(r.ch) : "", tacan,
                        rep && c.ident_t >= 0 ? fmt("%.0f s ago", now - c.ident_t) + fmt(" (x%.0f)", c.ident_count) : ""});
    }

    // what else a frequency is (1025..1150: also an interrogation frequency;
    // 1030 / 1090: SSR)
    std::string channel_text(int mhz) const {
        const nav::DmeChannel rc = nav::dme_by_reply(mhz);
        std::string s = "DME " + rc.name() + ": reply " + std::to_string(rc.reply_mhz) + " MHz, interrogation " +
                        std::to_string(rc.interrogation_mhz) + " MHz";
        if (in_iband(mhz)) {
            const nav::DmeChannel x = nav::dme_by_interrogation(mhz, 'X'), y = nav::dme_by_interrogation(mhz, 'Y');
            s += "; aircraft interrogate " + x.name() + " / " + y.name() + " here (replies " + std::to_string(x.reply_mhz) +
                 " / " + std::to_string(y.reply_mhz) + " MHz)";
        }
        if (mhz == 1030) s += "; also the SSR / Mode S interrogation frequency";
        if (mhz == 1090) s += "; also the SSR / Mode S / ADS-B reply frequency";
        return s;
    }

    void facts(Channel& c, double now, const std::string& list) {
        const int rk = reply_kind(c);
        const nav::DmeChannel rc = c.mhz ? nav::dme_by_reply(c.mhz) : nav::DmeChannel{};
        host.fact("Channels heard", list.find(',') != std::string::npos ? list : "");
        if (rc.valid()) {
            host.fact("Channel", channel_text(c.mhz));
            host.fact("Paired with", nav::pairing_text(rc));
        } else {
            host.fact("Channel", c.mhz ? "" : "unknown (no VFO frequency)");
            host.fact("Paired with", "");
        }
        if (rk < 0) {
            host.fact("Signal", "no DME beacon replies yet");
            for (const char* k : {"Ident", "Pulse pairs", "Level", "Pulses", "Carrier", "TACAN"}) host.fact(k, "");
            return;
        }
        const KindStats& s = c.ks[rk];
        host.fact("Signal", signal_text(c, rk));
        host.fact("Ident", c.ident.empty() ? "not heard yet (sent every 30-40 s)"
                                           : c.ident + "  " + c.ident_code + fmt("  (x%.0f, ", c.ident_count) +
                                                 fmt("%.0f s ago", now - c.ident_t) + fmt("; dot %.0f ms", c.ident_unit * 1000) +
                                                 (c.ident_rate > 0 ? fmt(", %.0f pairs/s keyed)", c.ident_rate) : ")"));
        host.fact("Pulse pairs", fmt("%.0f /s", s.rate) + fmt(" (%.0f in total)", static_cast<double>(s.total)));
        if (s.amp_v > 0) host.fact("Level", fmt("%.1f dBFS peak", db20(s.amp_v)) + fmt(", SNR %.0f dB", snr_db(c, s.amp_v)));
        if (s.sp_v > 0)
            host.fact("Pulses", fmt("spacing %.2f us", s.sp_v) + fmt(", width %.2f us", s.w_v) +
                                    (s.rise_v > 0 ? fmt(", rise %.1f us (10-90 %%)", s.rise_v) : ""));
        if (std::isfinite(c.carrier_hz))
            host.fact("Carrier", (c.mhz ? fmt("%.4f MHz", (c.mhz * 1e6 + c.carrier_hz) / 1e6) + ", " : std::string()) +
                                     fmt("%+.1f kHz from the channel", c.carrier_hz / 1e3) + " (incl. the receiver's own error)");
        if (c.tacan && std::isfinite(c.tacan_brg))
            host.fact("TACAN", fmt("bearing from the beacon %.1f°", c.tacan_brg) + fmt(" (coarse %.0f°)", c.tacan_coarse) +
                                   fmt("; 15 Hz %.0f %%", c.tacan_m15 * 100) + fmt(", 135 Hz %.0f %%", c.tacan_m135 * 100) +
                                   fmt("; north bursts %.2f /s", c.tacan_rot_hz));
        else if (now - s.first_t > 5)
            host.fact("TACAN", c.north_n ? fmt("reference bursts seen (%d north", c.north_n) + fmt(", %d auxiliary) - no steady 15 Hz", c.aux_n)
                                         : "no (no reference bursts: a plain DME)");
    }
};

}  // namespace

KRAKEN_PLUGIN(Dme, {.id = "dme",
                    .name = "DME / TACAN",
                    .description = "DME and TACAN beacons (962-1213 MHz): every DME channel in the VFO's band - "
                                   "Morse ident, channel and its VOR / ILS pairing, X / Y mode, pulse spacing / width, "
                                   "pairs per second, the carrier's exact frequency, TACAN bearing; aircraft "
                                   "interrogations on 1025-1150 MHz. Each beacon gets its own DoA on the 🗺 Map. A 2.4 MHz "
                                   "VFO covers up to three channels",
                    .version = "1.0",
                    .sample_rate = FS,
                    .min_vfo_rate = 600000,
                    .author = "KrakenSDR",
                    .options = {{"min_snr", "Pulse threshold", "10", "6=6 dB|8=8 dB|10=10 dB|12=12 dB|15=15 dB|20=20 dB",
                                 "How far above the noise a pulse must rise. Lower finds weaker beacons, higher ignores "
                                 "more interference"},
                                {"doa", "DoA per beacon", "1", "1=On|0=Off",
                                 "Report clean reply pairs of every beacon for its own bearing on the 🗺 Map (DF panel)"},
                                {"doa_rate", "DoA pairs per second", "100", "20=20|50=50|100=100|200=200|500=500",
                                 "Pulse pairs per beacon and second used for its bearing (more = steadier, more CPU)"}},
                    .manual_only = true,
                    .talkers = true})
