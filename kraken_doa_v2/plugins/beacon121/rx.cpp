// 121.5 / 243 MHz swept-tone beacon receiver - see rx.hpp.

#include "rx.hpp"

#include "navaid.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace b121 {

namespace {

constexpr int NT = 31;                    // channel low-pass taps (at FS)
constexpr double LP_CUT_HZ = 4500;
constexpr int GROUP_DELAY = (NT - 1) / 2;   // input samples
constexpr double BACKLOG_S = 2.5;         // a new channel starts this far back
constexpr int RING = 3 * FS;
constexpr double FIND_DB = 9.0;           // a carrier over the finder's floor
constexpr double MATCH_HZ = 40;           // a finder line is this channel's
constexpr double DROP_S = 30;             // a channel without its carrier this long goes
constexpr int LAG_MIN = 22, LAG_MAX = 100;   // sweep period in frames: 0.18 - 0.8 s
constexpr double RATE_MIN = 1.25, RATE_MAX = 5.5;   // sweeps a second accepted (spec 2 - 4)
constexpr double SPAN_MIN_HZ = 450;       // sweep range accepted (spec >= 700 Hz)
constexpr double MORSE_LONG_S = 0.6;

double fmod_pos(double a, double m) {
    double r = std::fmod(a, m);
    return r < 0 ? r + m : r;
}

float median_of(std::vector<float>& v) {
    if (v.empty()) return 0;
    auto mid = v.begin() + static_cast<long>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Spearman rank correlation of (x, y), x already ranks 0..n-1
double pearson(const std::vector<double>& a, const std::vector<double>& b) {
    const size_t n = a.size();
    if (n < 3) return 0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; i++) ma += a[i], mb += b[i];
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; i++) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
}

std::vector<double> ranks(const std::vector<double>& v) {
    const size_t n = v.size();
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
    std::vector<double> r(n);
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) j++;
        const double rr = 0.5 * static_cast<double>(i + j);
        for (size_t k = i; k <= j; k++) r[idx[k]] = rr;
        i = j + 1;
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Morse from the runs of a key
bool RunMorse::push(bool key) {
    if (key == cur_) {
        len_++;
        // a run reaching the long length ends the letter group at once (the sweep after a letter
        // goes on for most of a minute)
        if (!long_fired_ && len_ * tick_ >= MORSE_LONG_S) {
            long_fired_ = true;
            bool got = false;
            if (seen_long_ && runs_.size() >= 2) {
                runs_.emplace_back(cur_, len_ * tick_);
                if (getenv("B121_MORSE")) {
                    fprintf(stderr, "morse %.3f runs:", tick_);
                    for (const auto& x : runs_) fprintf(stderr, " %c%.3f", x.first ? '+' : '-', x.second);
                    fprintf(stderr, "\n");
                }
                got = decode();
            }
            runs_.clear();
            seen_long_ = true;
            return got;
        }
        return false;
    }
    const double run = len_ * tick_;
    const bool was = cur_;
    cur_ = key;
    if (long_fired_) {
        // the long run is the lead of what comes next
        long_fired_ = false;
        runs_.clear();
        runs_.emplace_back(was, run);
        len_ = 1;
        return false;
    }
    // a glitch (< 25 ms) is part of the run around it
    if (run < 0.025 && !runs_.empty() && runs_.back().first == key) {
        len_ = static_cast<int>(std::lround(runs_.back().second / tick_)) + len_ + 1;
        runs_.pop_back();
        return false;
    }
    // the key's own smear: a spectrogram window lengthens marks, shortens gaps
    runs_.emplace_back(was, was ? std::max(0.0, run - smear_) : run + smear_);
    len_ = 1;
    if (runs_.size() > 40) {
        runs_.clear();   // flicker, not keying
        seen_long_ = false;
    }
    return false;
}

void RunMorse::reset() {
    runs_.clear();
    cur_ = false;
    len_ = 0;
    seen_long_ = long_fired_ = false;
}

bool RunMorse::decode() {
    // the short runs between the long ones; marks = key down
    lead_on_ = runs_.front().first;
    std::vector<std::pair<bool, double>> r(runs_.begin() + 1, runs_.end() - 1);
    while (!r.empty() && !r.front().first) r.erase(r.begin());
    while (!r.empty() && !r.back().first) r.pop_back();
    std::vector<double> marks;
    for (const auto& x : r)
        if (x.first) marks.push_back(x.second);
    if (marks.size() < 2) return false;
    for (const auto& x : r)
        if (x.second < 0.03 || x.second > MORSE_LONG_S) return false;
    std::vector<double> s = marks;
    std::sort(s.begin(), s.end());
    double cut;
    if (s.back() >= 2.0 * s.front()) {
        size_t at = 0;
        double best = 0;
        for (size_t i = 0; i + 1 < s.size(); i++)
            if (s[i + 1] / s[i] > best) best = s[i + 1] / s[i], at = i;
        cut = std::sqrt(s[at] * s[at + 1]);
    } else {
        cut = s[s.size() / 2] < 0.2 ? s.back() * 1.01 : s.front() * 0.99;
    }
    std::vector<double> dots, dashes;
    for (double m : marks) (m < cut ? dots : dashes).push_back(m);
    auto med = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const double unit = !dots.empty() ? med(dots) : med(dashes) / 3;
    if (unit < 0.03 || unit > 0.2) return false;
    for (double m : dots)
        if (m < 0.5 * unit || m > 1.7 * unit) return false;
    for (double m : dashes)
        if (m < 2.2 * unit || m > 4.2 * unit) return false;
    std::string code, letters, cur;
    for (const auto& x : r) {
        if (x.first) {
            cur += x.second < cut ? '.' : '-';
            continue;
        }
        if (x.second < 0.35 * unit) return false;
        if (x.second > 2.0 * unit) {   // a letter gap
            const char* c = nav::morse_char(cur);
            if (!c) return false;
            letters += c;
            code += cur + " ";
            cur.clear();
        } else if (x.second > 1.8 * unit && x.second < 2.0 * unit) {
            return false;              // neither
        }
    }
    const char* c = nav::morse_char(cur);
    if (!c) return false;
    letters += c;
    code += cur;
    text_ = letters;
    code_ = code;
    unit_ = unit;
    return true;
}

// ---------------------------------------------------------------------------
class Channel {
public:
    Channel(int serial, double f, uint64_t abs_start, const std::vector<float>& taps)
        : serial_(serial), f_(f), abs_in_(abs_start), taps_(taps), mtone_(FRAME_S, static_cast<double>(NS) / FS2), mcar_(BLK_S, 0) {
        set_step();
        hist_.assign(2 * NT, cf(0, 0));
        sbuf_.assign(NS, cf(0, 0));
        spec_.assign(static_cast<size_t>(WIN_FRAMES) * 2 * NB, 0.f);
        frame_abs_.assign(WIN_FRAMES, 0);
        blk_.resize(WIN_BLOCKS);
        env_.resize(BLK);
        sin_ = kfft_alloc_complex(NS);
        sout_ = kfft_alloc_complex(NS);
        splan_ = kfft_plan_dft_1d(NS, sin_, sout_, KFFT_FORWARD);
        hann_.resize(NS);
        for (int i = 0; i < NS; i++) hann_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / NS));
        // the carrier block's gain for white input noise: c = sum g_n x_n
        std::vector<double> g(static_cast<size_t>(DEC * (BLK - 1) + NT), 0.0);
        for (int m = 0; m < BLK; m++)
            for (int j = 0; j < NT; j++) g[static_cast<size_t>(DEC * m + j)] += taps_[static_cast<size_t>(j)] / BLK;
        g2_ = 0;
        for (double v : g) g2_ += v * v;
        last_seen_abs_ = static_cast<double>(abs_start);
        last_carrier_abs_ = static_cast<double>(abs_start);
        last_report_abs_ = static_cast<double>(abs_start);
    }
    ~Channel() {
        kfft_destroy_plan(splan_);
        kfft_free(sin_);
        kfft_free(sout_);
    }

    int serial() const { return serial_; }
    double freq() const { return f_; }
    int passes() const { return passes_; }
    double last_seen() const { return last_seen_abs_; }
    double last_carrier() const { return last_carrier_abs_; }
    double level() const { return level_; }
    void seen(double abs, double f) {
        last_seen_abs_ = abs;
        // the finder's frequency only while the carrier AFC has nothing (a weak carrier)
        if (!afc_ok_ && std::fabs(f - f_) > 3) {
            f_ = f;
            set_step();
        }
    }
    double neighbour_hz = NAN;

    void feed(const cf* x, size_t n, const Receiver& rx, std::vector<Report>* out) {
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> r = rot_;
            rot_ *= step_;
            const cf v = x[i] * cf(static_cast<float>(r.real()), static_cast<float>(r.imag()));
            hist_[static_cast<size_t>(hpos_)] = v;
            hist_[static_cast<size_t>(hpos_ + NT)] = v;
            hpos_ = (hpos_ + 1) % NT;
            abs_in_++;
            if (++dcount_ < DEC) continue;
            dcount_ = 0;
            // FIR over the last NT inputs (oldest first at hpos_)
            cf acc(0, 0);
            const cf* h = &hist_[static_cast<size_t>(hpos_)];
            for (int j = 0; j < NT; j++) acc += h[j] * taps_[static_cast<size_t>(j)];
            sample(acc, static_cast<double>(abs_in_) - 1 - GROUP_DELAY, rx, out);
        }
        rot_ /= std::abs(rot_);
    }

private:
    int serial_;
    double f_;
    uint64_t abs_in_;
    std::vector<float> taps_;
    std::complex<double> rot_{1, 0}, step_{1, 0};
    std::vector<cf> hist_;
    int hpos_ = 0, dcount_ = 0;
    // carrier blocks
    struct Blk {
        double abs = 0;          // centre (input index)
        float c2 = 0;
        bool on = false;
        float m = NAN, duty = NAN;
    };
    std::vector<Blk> blk_;
    uint64_t nblk_ = 0;
    cf bsum_{0, 0}, cprev_{0, 0};
    bool prev_on_ = false;
    std::vector<float> env_;
    int bi_ = 0;
    double g2_ = 0;
    double level_ = 0;
    std::complex<double> afc_acc_{0, 0};
    int afc_n_ = 0;
    bool afc_ok_ = false;
    int off_run_ = 0, on_run_ = 0;
    double off_start_ = 0;
    bool off_snr_ok_ = false;
    std::vector<Pause> pauses_, cw_;
    // spectrogram
    std::vector<cf> sbuf_;
    int spos_ = 0, scount_ = 0, sfill_ = 0;
    kfft_complex* sin_ = nullptr;
    kfft_complex* sout_ = nullptr;
    kfft_plan splan_ = nullptr;
    std::vector<float> hann_;
    std::vector<float> spec_;
    std::vector<double> frame_abs_;
    uint64_t nframes_ = 0;
    uint64_t key_frames_done_ = 0;
    int key_off_run_ = 0, key_glitch_ = 0;
    double key_off_start_ = 0;
    // evaluation
    int ecount_ = 0;
    int passes_ = 0;
    double period_prev_ = 0;
    double last_seen_abs_, last_carrier_abs_, last_report_abs_;
    RunMorse mtone_, mcar_;
    std::string morse_, morse_code_, morse_key_;
    double morse_unit_ = 0;
    std::vector<float> Y_, L_, U_, D_, med_, F_, Fbest_, Flog_, Fu_, Fd_, colbuf_;

    void set_step() { step_ = std::polar(1.0, -2 * M_PI * f_ / FS); }

    void sample(cf z, double abs, const Receiver& rx, std::vector<Report>* out) {
        // carrier block
        bsum_ += z;
        env_[static_cast<size_t>(bi_)] = std::abs(z);
        if (++bi_ == BLK) {
            block(abs - (BLK / 2.0) * DEC, rx);
            bi_ = 0;
        }
        // spectrogram
        sbuf_[static_cast<size_t>(spos_)] = z;
        spos_ = (spos_ + 1) % NS;
        if (sfill_ < NS) sfill_++;
        if (++scount_ >= HOPS && sfill_ == NS) {
            scount_ = 0;
            frame(abs - (NS / 2.0) * DEC);
        }
        if (++ecount_ >= EVAL_SAMPLES) {
            ecount_ = 0;
            evaluate(abs, rx, out);
        }
    }

    void block(double abs_mid, const Receiver& rx) {
        const cf c = bsum_ / static_cast<float>(BLK);
        bsum_ = cf(0, 0);
        const float c2 = std::norm(c);
        const double nv = rx.noise_var();
        const double nc = std::isfinite(nv) ? nv * g2_ : 0;
        bool on = c2 > 12 * nc && c2 > 0.1 * level_;
        if (level_ <= 0 && c2 > 12 * nc && nc > 0) on = true;
        if (on) level_ = level_ <= 0 ? c2 : level_ + 0.05 * (c2 - level_);
        // AFC: the carrier's phase step from block to block
        if (on && prev_on_) {
            afc_acc_ += std::complex<double>(c * std::conj(cprev_));
            afc_n_++;
            if (afc_n_ >= 25) {
                const double ph = std::arg(afc_acc_);
                const double snr = nc > 0 ? level_ / nc : 1e9;
                if (snr > 3) {
                    const double df = std::clamp(ph / (2 * M_PI * BLK_S), -20.0, 20.0);
                    f_ += 0.7 * df;
                    set_step();
                    afc_ok_ = true;
                }
                afc_acc_ = 0;
                afc_n_ = 0;
            }
        }
        cprev_ = c;
        prev_on_ = on;
        // the envelope's 2 / 98 % points
        const std::vector<float>& e = env_;
        std::vector<float> s(e.begin() + 1, e.end() - 1);
        const size_t n = s.size();
        std::nth_element(s.begin(), s.begin() + static_cast<long>(n * 98 / 100), s.end());
        const float A = s[n * 98 / 100];
        std::nth_element(s.begin(), s.begin() + static_cast<long>(n * 2 / 100), s.end());
        const float B = s[n * 2 / 100];
        Blk& b = blk_[static_cast<size_t>(nblk_ % WIN_BLOCKS)];
        b.abs = abs_mid;
        b.c2 = c2;
        b.on = on;
        b.m = A + B > 0 ? (A - B) / (A + B) : NAN;
        const float mid = 0.5f * (A + B);
        int above = 0;
        for (size_t i = 1; i + 1 < static_cast<size_t>(BLK); i++) above += e[i] > mid;
        b.duty = static_cast<float>(above) / static_cast<float>(BLK - 2);
        nblk_++;
        if (on) last_carrier_abs_ = abs_mid;
        if (on && !std::isfinite(first_on_abs_)) first_on_abs_ = abs_mid - (BLK / 2.0) * DEC;
        // pauses: >= 13 blocks (0.26 s) without the carrier, then >= 2 with it again (shorter gaps:
        // Morse keying, fades)
        const double snr = nc > 0 ? level_ / nc : 0;
        if (!on) {
            if (off_run_ == 0) {
                off_start_ = abs_mid - (BLK / 2.0) * DEC;
                off_snr_ok_ = snr >= 100;   // only a strong carrier's gaps are surely gaps
            }
            off_run_++;
            on_run_ = 0;
        } else {
            on_run_++;
            if (on_run_ == 2 && off_run_ >= 13) {
                const double len = off_run_ * BLK_S;
                if (off_snr_ok_ && len <= 10) {
                    pauses_.push_back({off_start_, len});
                    last_pause_end_ = off_start_ + len * FS;
                }
            }
            if (on_run_ >= 2) off_run_ = 0;
        }
        if (mcar_.push(on) && snr >= 100 && !mcar_.lead_on()) {
            morse_ = mcar_.text();
            morse_code_ = mcar_.code();
            morse_unit_ = mcar_.unit_s();
            morse_key_ = "carrier";
        }
    }

    void frame(double abs_mid) {
        for (int i = 0; i < NS; i++) {
            const cf v = sbuf_[static_cast<size_t>((spos_ + i) % NS)] * hann_[static_cast<size_t>(i)];
            sin_[i][0] = v.real();
            sin_[i][1] = v.imag();
        }
        kfft_execute(splan_);
        float* p = &spec_[static_cast<size_t>(nframes_ % WIN_FRAMES) * 2 * NB];
        for (int b = 0; b < NB; b++) {
            const int k = BIN_LO + b;
            p[b] = sout_[k][0] * sout_[k][0] + sout_[k][1] * sout_[k][1];
            p[NB + b] = sout_[NS - k][0] * sout_[NS - k][0] + sout_[NS - k][1] * sout_[NS - k][1];
        }
        frame_abs_[static_cast<size_t>(nframes_ % WIN_FRAMES)] = abs_mid;
        nframes_++;
    }

    // the blocks of the last EVAL period: on / off runs, C/N0
    void carrier_report(double abs_end, const Receiver& rx, Report* r) {
        const uint64_t nb = std::min<uint64_t>(nblk_, WIN_BLOCKS);
        double sum = 0;
        int non = 0, nall = 0;
        bool in = false;
        double a = 0, e = 0;
        for (uint64_t i = nblk_ - nb; i < nblk_; i++) {
            const Blk& b = blk_[static_cast<size_t>(i % WIN_BLOCKS)];
            if (b.abs <= last_report_abs_ || b.abs > abs_end) continue;
            nall++;
            const double b0 = b.abs - BLK / 2.0 * DEC, b1 = b.abs + BLK / 2.0 * DEC;
            if (b.on) {
                sum += b.c2;
                non++;
                if (!in) a = b0;
                in = true;
                e = b1;
            } else if (in) {
                r->on_runs.push_back({a, e});
                in = false;
            }
        }
        if (in) r->on_runs.push_back({a, e});
        r->carrier_share = nall ? static_cast<double>(non) / nall : 0;
        // a weak carrier's blocks come and go with the noise (under ~15 dB in a block): its runs mean
        // nothing - the whole period is the carrier's while the finder (7.8 Hz bins, ~0.5 s) sees it
        const double nvb = rx.noise_var() * g2_;
        if (std::isfinite(nvb) && level_ < 30 * nvb && nall > 0) {
            const bool seen = abs_end - last_seen_abs_ < 0.7 * FS;
            r->carrier_share = seen ? 1.0 : 0.0;
            r->on_runs.clear();
            if (seen) r->on_runs.push_back({last_report_abs_, abs_end});
        }
        r->carrier = nb > 0 && blk_[static_cast<size_t>((nblk_ - 1) % WIN_BLOCKS)].on;
        const double nv = rx.noise_var();
        if (non > 0 && std::isfinite(nv) && nv > 0) {
            const double c2 = sum / non - nv * g2_;
            r->cn0 = 10 * std::log10(std::max(c2, 1e-3 * nv * g2_) * FS / nv);
        }
    }

    void evaluate(double abs_end, const Receiver& rx, std::vector<Report>* out) {
        Report r;
        r.chan = serial_;
        r.freq_hz = f_;
        r.eval_abs = abs_end;
        r.neighbour_hz = neighbour_hz;
        r.first_on_abs = first_on_abs_;
        if (std::isfinite(first_on_abs_)) r.last_on_abs = last_carrier_abs_ + (BLK / 2.0) * DEC;
        carrier_report(abs_end, rx, &r);
        r.pauses.swap(pauses_);
        const int nf = static_cast<int>(std::min<uint64_t>(nframes_, WIN_FRAMES));
        if (nf >= 110) analyse(nf, &r);
        else r.sweep.why = "too few frames";
        if (r.sweep.ok && (passes_ == 0 || std::fabs(r.sweep.period_s - period_prev_) < 0.06 * period_prev_)) {
            passes_++;
        } else {
            passes_ = r.sweep.ok ? 1 : 0;
        }
        if (r.sweep.ok) period_prev_ = r.sweep.period_s;
        r.passes = passes_;
        r.cw.swap(cw_);
        if (!morse_.empty()) {
            r.morse = morse_;
            r.morse_code = morse_code_;
            r.morse_key = morse_key_;
            r.morse_unit_s = morse_unit_;
            morse_.clear();
        }
        last_report_abs_ = abs_end;
        out->push_back(std::move(r));
    }

    // the sweep in the last nf frames
    void analyse(int nf, Report* rep) {
        Sweep& sw = rep->sweep;
        const uint64_t f0 = nframes_ - static_cast<uint64_t>(nf);
        auto frame_p = [&](int t) { return &spec_[static_cast<size_t>((f0 + static_cast<uint64_t>(t)) % WIN_FRAMES) * 2 * NB]; };
        // each side's bins normalised by their median, a cell = the weaker side
        med_.assign(2 * NB, 1.f);
        colbuf_.resize(static_cast<size_t>(nf));
        for (int b = 0; b < 2 * NB; b++) {
            for (int t = 0; t < nf; t++) colbuf_[static_cast<size_t>(t)] = frame_p(t)[b];
            med_[static_cast<size_t>(b)] = std::max(median_of(colbuf_), 1e-30f);
        }
        Y_.resize(static_cast<size_t>(nf) * NB);
        for (int t = 0; t < nf; t++) {
            const float* p = frame_p(t);
            for (int b = 0; b < NB; b++)
                Y_[static_cast<size_t>(t) * NB + b] = std::min(p[b] / med_[static_cast<size_t>(b)], p[NB + b] / med_[static_cast<size_t>(NB + b)]);
        }
        std::vector<float> mx(NB);
        for (int b = 0; b < NB; b++) {
            for (int t = 0; t < nf; t++) colbuf_[static_cast<size_t>(t)] = Y_[static_cast<size_t>(t) * NB + b];
            mx[static_cast<size_t>(b)] = std::max(median_of(colbuf_), 1e-6f);
        }
        // the modulation key (Morse in tone, unmodulated periods) on the frames not keyed yet
        // (the weaker side of 30 bins of noise passes 12 x its median in 0.7 % of the frames)
        const float key_thr = 12.0f;
        for (int t = 0; t < nf; t++) {
            if (f0 + static_cast<uint64_t>(t) < key_frames_done_) continue;
            float m = 0;
            for (int b = 0; b < NB; b++) m = std::max(m, Y_[static_cast<size_t>(t) * NB + b] / mx[static_cast<size_t>(b)]);
            const bool key = m > key_thr;
            const double fabs_ = frame_abs_[static_cast<size_t>((f0 + static_cast<uint64_t>(t)) % WIN_FRAMES)];
            // a Morse letter in the tone: after key-up (no tone), within 4 s of a carrier pause's end
            // (EN 302 152-1: "immediately following each pause")
            if (mtone_.push(key) && level_snr() >= 100 && !mtone_.lead_on() && (fabs_ - last_pause_end_) / FS < 4) {
                morse_ = mtone_.text();
                morse_code_ = mtone_.code();
                morse_unit_ = mtone_.unit_s();
                morse_key_ = "tone";
            }
            // an unmodulated carrier for >= 1.2 s (no tone while the carrier is on)
            const int car = carrier_at(fabs_);   // -1: no block yet (the newest frames) - as before
            if (car >= 0) car_prev_ = car == 1;
            // (a noise frame or two over the threshold doesn't end it)
            if (!key && car_prev_) {
                if (key_off_run_ == 0) key_off_start_ = fabs_;
                key_off_run_ += 1 + key_glitch_;
                key_glitch_ = 0;
            } else if (key && car_prev_ && key_off_run_ > 0 && key_glitch_ < 3) {
                key_glitch_++;
            } else {
                if (key && key_off_run_ * FRAME_S >= 1.2 && key_off_run_ * FRAME_S <= 10 && level_snr() >= 100)
                    cw_.push_back({key_off_start_, key_off_run_ * FRAME_S});
                key_off_run_ = 0;
                key_glitch_ = 0;
            }
        }
        key_frames_done_ = nframes_;
        // Y: clipped at 13 dB (the period search, the fold); L: unclipped (where in a bin the tone is: the
        // log of its fold)
        L_.resize(Y_.size());
        for (int t = 0; t < nf; t++)
            for (int b = 0; b < NB; b++) {
                const size_t i = static_cast<size_t>(t) * NB + b;
                const float v = Y_[i] / mx[static_cast<size_t>(b)];
                L_[i] = v;
                Y_[i] = std::min(v, 20.f) - 1.f;
            }
        // autocorrelation over the lags of 1.25 - 5.5 sweeps a second
        double r0 = 0;
        for (float y : Y_) r0 += static_cast<double>(y) * y;
        r0 /= nf;
        if (!(r0 > 0)) {
            sw.why = "no modulation";
            return;
        }
        const int lmax = std::min(LAG_MAX, nf / 2);
        if (lmax <= LAG_MIN + 2) {
            sw.why = "window too short";
            return;
        }
        std::vector<double> rho(static_cast<size_t>(lmax + 2), 0.0);
        for (int L = LAG_MIN - 1; L <= lmax + 1; L++) {
            double s = 0;
            const int n = nf - L;
            for (int t = 0; t < n; t++) {
                const float* a = &Y_[static_cast<size_t>(t) * NB];
                const float* b = &Y_[static_cast<size_t>(t + L) * NB];
                for (int k = 0; k < NB; k++) s += a[k] * b[k];
            }
            rho[static_cast<size_t>(L)] = s / n / r0;
        }
        int Lb = LAG_MIN;
        for (int L = LAG_MIN; L <= lmax; L++)
            if (rho[static_cast<size_t>(L)] > rho[static_cast<size_t>(Lb)]) Lb = L;
        // a sub-multiple that holds most of it is the period (P / 2, P / 3, P / 4 of a P peak: a
        // period of 31.25 frames peaks at the lag nearest a whole multiple)
        for (int q = 4; q >= 2; q--) {
            const int h = static_cast<int>(std::lround(Lb / static_cast<double>(q)));
            if (h - 1 < LAG_MIN) continue;
            int hb = h;
            for (int L = h - 1; L <= h + 1; L++)
                if (rho[static_cast<size_t>(L)] > rho[static_cast<size_t>(hb)]) hb = L;
            if (rho[static_cast<size_t>(hb)] >= 0.6 * rho[static_cast<size_t>(Lb)]) {
                Lb = hb;
                break;
            }
        }
        sw.rho = rho[static_cast<size_t>(Lb)];
        const double rho_min = 0.04 + 5.0 / std::sqrt(static_cast<double>(nf) * NB);
        char why[128];
        if (sw.rho < rho_min) {
            snprintf(why, sizeof why, "no period (rho %.3f < %.3f)", sw.rho, rho_min);
            sw.why = why;
            return;
        }
        // the period to a fraction of a frame: the sharpest fold around it
        double T0 = Lb;
        {
            const double a = rho[static_cast<size_t>(Lb - 1)], b = rho[static_cast<size_t>(Lb)], c = rho[static_cast<size_t>(Lb + 1)];
            const double d = a - 2 * b + c;
            if (d < 0) T0 += std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
        }
        double best = -1e30, Tb = T0;
        int Kb = 0;
        for (int s = -20; s <= 20; s++) {
            const double T = T0 + 0.05 * s;
            const int K = static_cast<int>(std::lround(T));
            if (K < LAG_MIN - 1) continue;
            const double c = fold(Y_, nf, T, K, &F_);
            if (c > best) {
                best = c;
                Tb = T;
                Kb = K;
                Fbest_ = F_;
            }
        }
        if (Kb <= 0) {
            sw.why = "fold";
            return;
        }
        const int K = Kb;
        fold(L_, nf, Tb, K, &Flog_);
        sw.period_s = Tb * FRAME_S;
        sw.rate_hz = 1.0 / sw.period_s;
        // the fold's noise: median / MAD of its cells
        std::vector<float> cells(Fbest_.begin(), Fbest_.begin() + static_cast<long>(K) * NB);
        const float fmed = median_of(cells);
        for (auto& v : cells) v = std::fabs(v - fmed);
        const float sigma = std::max(1.4826f * median_of(cells), 1e-6f);
        const float thr = fmed + 4.5f * sigma;
        std::vector<double> kk, ff;
        std::vector<float> peaks;
        sw.trace.assign(static_cast<size_t>(K), NAN);
        std::vector<float> tr(static_cast<size_t>(K), NAN);
        for (int k = 0; k < K; k++) {
            const float* row = &Fbest_[static_cast<size_t>(k) * NB];
            int bm = 0;
            for (int b = 1; b < NB; b++)
                if (row[b] > row[bm]) bm = b;
            if (row[bm] < thr || row[bm] < 0.3f) continue;
            // where the tone is: the unclipped fold's maximum near it (the clipped one is flat on top)
            const float* lr = &Flog_[static_cast<size_t>(k) * NB];
            const float pk = row[bm];
            for (int b = std::max(0, bm - 3); b <= std::min(NB - 1, bm + 3); b++)
                if (lr[b] > lr[bm]) bm = b;
            double d = 0;
            if (bm > 0 && bm + 1 < NB) {
                const double a = std::log(std::max(lr[bm - 1], 1e-3f)), b = std::log(std::max(lr[bm], 1e-3f)),
                             c = std::log(std::max(lr[bm + 1], 1e-3f));
                const double den = a - 2 * b + c;
                if (den < 0) d = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
            }
            const double hz = (BIN_LO + bm + d) * BIN_HZ;
            kk.push_back(k);
            ff.push_back(hz);
            peaks.push_back(pk);
            tr[static_cast<size_t>(k)] = static_cast<float>(hz);
        }
        const int nlit = static_cast<int>(kk.size());
        sw.lit = static_cast<double>(nlit) / K;
        if (!peaks.empty()) {
            std::vector<float> pk = peaks;
            sw.peak_db = 10 * std::log10(std::max(1e-3, static_cast<double>(median_of(pk) - fmed) / sigma));
        }
        if (nlit < std::max(6, static_cast<int>(0.4 * K))) {
            snprintf(why, sizeof why, "period %.3f s, tone seen in %.0f %% of it", sw.period_s, 100 * sw.lit);
            sw.why = why;
            return;
        }
        // monotonic over the best circular shift (n candidate starts: the lit k)
        const std::vector<double> ry = ranks(ff);
        double best_r = 0;
        int best_s = 0;
        std::vector<double> rx(static_cast<size_t>(nlit));
        for (int s = 0; s < nlit; s++) {
            for (int i = 0; i < nlit; i++) rx[static_cast<size_t>(i)] = (i - s + nlit) % nlit;
            const double r = pearson(rx, ry);
            if (std::fabs(r) > std::fabs(best_r)) best_r = r, best_s = s;
        }
        sw.mono = std::fabs(best_r);
        int dir = DIR_NONE;
        int start = best_s;   // the sweep's first lit point
        if (sw.mono >= 0.8) {
            dir = best_r < 0 ? DIR_DOWN : DIR_UP;
        } else {
            // up and down: rising from the lowest point to the highest, falling back
            int ilo = 0, ihi = 0;
            for (int i = 0; i < nlit; i++) {
                if (ff[static_cast<size_t>(i)] < ff[static_cast<size_t>(ilo)]) ilo = i;
                if (ff[static_cast<size_t>(i)] > ff[static_cast<size_t>(ihi)]) ihi = i;
            }
            auto seg = [&](int from, int to) {
                std::vector<double> a, b;
                for (int i = from;; i = (i + 1) % nlit) {
                    a.push_back(static_cast<double>(a.size()));
                    b.push_back(ff[static_cast<size_t>(i)]);
                    if (i == to) break;
                }
                return std::make_pair(static_cast<int>(a.size()), pearson(a, ranks(b)));
            };
            const auto up = seg(ilo, ihi), dn = seg(ihi, ilo);
            if (up.first >= 0.25 * nlit && dn.first >= 0.25 * nlit && up.second >= 0.8 && dn.second <= -0.8) {
                dir = DIR_UPDOWN;
                start = ilo;
                sw.mono = std::min(up.second, -dn.second);
            }
        }
        if (dir == DIR_NONE) {
            snprintf(why, sizeof why, "period %.3f s, not a sweep (monotonic %.2f)", sw.period_s, sw.mono);
            sw.why = why;
            return;
        }
        // range, fill, jumps (in sweep order)
        // the range: the extremes of the points that have a neighbour (in sweep order) within 200 Hz
        sw.lo_hz = 1e9;
        sw.hi_hz = 0;
        for (int j = 0; j < nlit; j++) {
            const double v = ff[static_cast<size_t>(j)];
            const double p = ff[static_cast<size_t>((j + nlit - 1) % nlit)], q = ff[static_cast<size_t>((j + 1) % nlit)];
            if (std::fabs(v - p) > 200 && std::fabs(v - q) > 200) continue;
            sw.lo_hz = std::min(sw.lo_hz, v);
            sw.hi_hz = std::max(sw.hi_hz, v);
        }
        if (sw.hi_hz < sw.lo_hz) sw.lo_hz = sw.hi_hz = ff[0];
        const double span = sw.hi_hz - sw.lo_hz;
        int jumps = 0;
        std::vector<int> bins_seen;
        for (int j = 0; j < nlit; j++) {
            const int i = (start + j) % nlit;
            bins_seen.push_back(static_cast<int>(std::lround(ff[static_cast<size_t>(i)] / BIN_HZ)));
            if (j > 0) {
                const int ip = (start + j - 1) % nlit;
                if (std::fabs(ff[static_cast<size_t>(i)] - ff[static_cast<size_t>(ip)]) > 300) jumps++;
            }
        }
        std::sort(bins_seen.begin(), bins_seen.end());
        bins_seen.erase(std::unique(bins_seen.begin(), bins_seen.end()), bins_seen.end());
        const int blo = static_cast<int>(std::lround(sw.lo_hz / BIN_HZ)), bhi = static_cast<int>(std::lround(sw.hi_hz / BIN_HZ));
        int inside = 0;
        for (int b : bins_seen) inside += b >= blo && b <= bhi;
        const double fill = static_cast<double>(inside) / std::max(1, bhi - blo + 1);
        // the trace from the sweep's start
        const int k0 = static_cast<int>(kk[static_cast<size_t>(start)]);
        for (int k = 0; k < K; k++) sw.trace[static_cast<size_t>(k)] = tr[static_cast<size_t>((k0 + k) % K)];
        sw.dir = dir;
        if (sw.rate_hz < RATE_MIN || sw.rate_hz > RATE_MAX) {
            snprintf(why, sizeof why, "%.2f sweeps a second (outside %.2f - %.1f)", sw.rate_hz, RATE_MIN, RATE_MAX);
            sw.why = why;
            return;
        }
        if (span < SPAN_MIN_HZ) {
            snprintf(why, sizeof why, "sweep %.0f - %.0f Hz: too narrow", sw.lo_hz, sw.hi_hz);
            sw.why = why;
            return;
        }
        if (fill < 0.6) {
            snprintf(why, sizeof why, "range %.0f - %.0f Hz filled %.0f %% (steps, not a sweep)", sw.lo_hz, sw.hi_hz, 100 * fill);
            sw.why = why;
            return;
        }
        if (jumps > 1 + nlit / 10) {
            snprintf(why, sizeof why, "trace jumps %d times", jumps);
            sw.why = why;
            return;
        }
        // AM: the tone at the same frequency on BOTH sides of the carrier. The weaker side (the cells)
        // can also light up where two neighbours' one-sided sidebands cross - each side's own fold must
        // then show the tone elsewhere
        {
            U_.resize(static_cast<size_t>(nf) * NB);
            D_.resize(static_cast<size_t>(nf) * NB);
            for (int t = 0; t < nf; t++) {
                const float* p = frame_p(t);
                for (int b = 0; b < NB; b++) {
                    U_[static_cast<size_t>(t) * NB + b] = std::min(p[b] / med_[static_cast<size_t>(b)], 20.f);
                    D_[static_cast<size_t>(t) * NB + b] = std::min(p[NB + b] / med_[static_cast<size_t>(NB + b)], 20.f);
                }
            }
            fold(U_, nf, Tb, K, &Fu_);
            fold(D_, nf, Tb, K, &Fd_);
            int same = 0;
            for (int j = 0; j < nlit; j++) {
                const int k = static_cast<int>(kk[static_cast<size_t>(j)]);
                const float* u = &Fu_[static_cast<size_t>(k) * NB];
                const float* d = &Fd_[static_cast<size_t>(k) * NB];
                int bu = 0, bd = 0;
                for (int b = 1; b < NB; b++) {
                    if (u[b] > u[bu]) bu = b;
                    if (d[b] > d[bd]) bd = b;
                }
                const double hz = ff[static_cast<size_t>(j)] / BIN_HZ - BIN_LO;
                same += std::fabs(bu - hz) <= 1.6 && std::fabs(bd - hz) <= 1.6;
            }
            sw.sym = static_cast<double>(same) / nlit;
            if (sw.sym < 0.6) {
                snprintf(why, sizeof why, "period %.3f s: the sides differ (%.0f %% alike) - not one AM signal", sw.period_s,
                         100 * sw.sym);
                sw.why = why;
                return;
            }
        }
        sw.ok = true;
        measure(Tb, K, f0, nf, rep);
    }

    // folded spectrogram: F[k][b] = mean of the frames at phase k of period T
    double fold(const std::vector<float>& Y, int nf, double T, int K, std::vector<float>* F) {
        F->assign(static_cast<size_t>(K) * NB, 0.f);
        std::vector<int> cnt(static_cast<size_t>(K), 0);
        for (int t = 0; t < nf; t++) {
            int k = static_cast<int>(fmod_pos(t, T) / T * K);
            if (k >= K) k = K - 1;
            float* row = &(*F)[static_cast<size_t>(k) * NB];
            const float* y = &Y[static_cast<size_t>(t) * NB];
            for (int b = 0; b < NB; b++) row[b] += y[b];
            cnt[static_cast<size_t>(k)]++;
        }
        double c = 0;
        for (int k = 0; k < K; k++) {
            float* row = &(*F)[static_cast<size_t>(k) * NB];
            const float inv = cnt[static_cast<size_t>(k)] ? 1.f / cnt[static_cast<size_t>(k)] : 0.f;
            float m = -1e30f;
            for (int b = 0; b < NB; b++) {
                row[b] *= inv;
                m = std::max(m, row[b]);
            }
            c += m;
        }
        return c;
    }

    // modulation factor and duty cycle from the envelope blocks where the tone was seen
    void measure(double T, int K, uint64_t f0, int nf, Report* rep) {
        const double fa0 = frame_abs_[static_cast<size_t>(f0 % WIN_FRAMES)];
        const double fstep = static_cast<double>(HOPS) * DEC;
        // which phase bins had the tone (and how high)
        std::vector<float> fk(static_cast<size_t>(K), NAN);
        {
            // rebuild from Fbest_ (same threshold as the trace)
            std::vector<float> cells(Fbest_.begin(), Fbest_.begin() + static_cast<long>(K) * NB);
            const float fmed = median_of(cells);
            for (auto& v : cells) v = std::fabs(v - fmed);
            const float thr = fmed + 4.5f * std::max(1.4826f * median_of(cells), 1e-6f);
            for (int k = 0; k < K; k++) {
                const float* row = &Fbest_[static_cast<size_t>(k) * NB];
                int bm = 0;
                for (int b = 1; b < NB; b++)
                    if (row[b] > row[bm]) bm = b;
                if (row[bm] >= thr && row[bm] >= 0.3f) fk[static_cast<size_t>(k)] = static_cast<float>((BIN_LO + bm) * BIN_HZ);
            }
        }
        std::vector<float> ms, ds;
        const uint64_t nb = std::min<uint64_t>(nblk_, WIN_BLOCKS);
        for (uint64_t i = nblk_ - nb; i < nblk_; i++) {
            const Blk& b = blk_[static_cast<size_t>(i % WIN_BLOCKS)];
            if (!b.on || !std::isfinite(b.m)) continue;
            const double t = (b.abs - fa0) / fstep;
            if (t < 0 || t > nf - 1) continue;
            // the block spans 2.5 frames: every phase it covers must have the tone
            bool lit = true;
            float fmax = 0;
            for (double dt = -1.25; dt <= 1.25; dt += 0.625) {
                int k = static_cast<int>(fmod_pos(t + dt, T) / T * K);
                if (k >= K) k = K - 1;
                const float f = fk[static_cast<size_t>(k)];
                if (!std::isfinite(f)) lit = false;
                else fmax = std::max(fmax, f);
            }
            if (!lit) continue;
            ms.push_back(b.m);
            // the duty cycle needs the tone's harmonics: low tones only (2nd - 5th inside the channel)
            if (fmax < 700) ds.push_back(b.duty);
        }
        if (ms.size() >= 10) rep->depth = median_of(ms);
        if (ds.size() >= 6) rep->duty = median_of(ds);
    }

    double level_snr() const { return last_nc_ > 0 ? level_ / last_nc_ : 0; }
    double last_nc_ = 0;

    int carrier_at(double abs) const {
        const uint64_t nb = std::min<uint64_t>(nblk_, WIN_BLOCKS);
        if (nb == 0) return -1;
        if (abs > blk_[static_cast<size_t>((nblk_ - 1) % WIN_BLOCKS)].abs + BLK / 2.0 * DEC) return -1;
        for (uint64_t i = nblk_; i-- > nblk_ - nb;) {
            const Blk& b = blk_[static_cast<size_t>(i % WIN_BLOCKS)];
            if (std::fabs(b.abs - abs) <= BLK / 2.0 * DEC) return b.on ? 1 : 0;
            if (b.abs < abs - BLK * DEC) break;
        }
        return 0;
    }
    bool car_prev_ = false;
    double first_on_abs_ = NAN;
    double last_pause_end_ = -1e18;

public:
    void set_noise(double nv) { last_nc_ = std::isfinite(nv) ? nv * g2_ : 0; }
};

// ---------------------------------------------------------------------------
Receiver::Receiver() {
    fin_ = kfft_alloc_complex(NF);
    fout_ = kfft_alloc_complex(NF);
    fplan_ = kfft_plan_dft_1d(NF, fin_, fout_, KFFT_FORWARD);
    win_.resize(NF);
    win2_ = 0;
    for (int i = 0; i < NF; i++) {
        win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / NF));
        win2_ += static_cast<double>(win_[static_cast<size_t>(i)]) * win_[static_cast<size_t>(i)];
    }
    reset();
}

Receiver::~Receiver() {
    ch_.clear();
    kfft_destroy_plan(fplan_);
    kfft_free(fin_);
    kfft_free(fout_);
}

void Receiver::reset() {
    abs_ = 0;
    ring_.assign(RING, cf(0, 0));
    ring_pos_ = 0;
    ring_n_ = 0;
    fbuf_.assign(NF, cf(0, 0));
    fpos_ = fill_ = 0;
    nspec_ = 0;
    avg_.assign(NF, 0.f);
    noise_var_ = NAN;
    pending_.clear();
    ch_.clear();
}

int Receiver::channels() const { return static_cast<int>(ch_.size()); }

std::vector<float> lowpass_taps() {
    std::vector<float> h(NT);
    double s = 0;
    const double fc = LP_CUT_HZ / FS;
    for (int i = 0; i < NT; i++) {
        const double x = i - (NT - 1) / 2.0;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
        const double w = 0.54 - 0.46 * std::cos(2 * M_PI * i / (NT - 1));
        h[static_cast<size_t>(i)] = static_cast<float>(sinc * w);
        s += sinc * w;
    }
    for (auto& v : h) v = static_cast<float>(v / s);
    return h;
}

void Receiver::process(const kp::cf* x, size_t n, std::vector<Report>* out) {
    size_t i = 0;
    while (i < n) {
        const size_t len = std::min(n - i, static_cast<size_t>(HOPF - fpos_ % HOPF));
        for (size_t j = 0; j < len; j++) {
            const cf v = x[i + j];
            ring_[ring_pos_] = v;
            ring_pos_ = (ring_pos_ + 1) % ring_.size();
            fbuf_[static_cast<size_t>(fill_ % NF)] = v;
            fill_++;
        }
        ring_n_ += len;
        for (auto& c : ch_) {
            c->set_noise(noise_var_);
            c->feed(x + i, len, *this, out);
        }
        abs_ += len;
        fpos_ += static_cast<int>(len);
        i += len;
        if (fpos_ % HOPF == 0) {
            fpos_ = 0;
            if (fill_ >= NF) spectrum();
            if (fill_ > 4 * NF) fill_ = NF + fill_ % NF;   // keep the index small (same position mod NF)
        }
    }
}

void Receiver::spectrum() {
    // the last NF samples, oldest first
    const int start = fill_ % NF;
    for (int i = 0; i < NF; i++) {
        const cf v = fbuf_[static_cast<size_t>((start + i) % NF)] * win_[static_cast<size_t>(i)];
        fin_[i][0] = v.real();
        fin_[i][1] = v.imag();
    }
    kfft_execute(fplan_);
    const float a = nspec_ < 8 ? 1.f / static_cast<float>(nspec_ + 1) : 0.125f;
    for (int k = 0; k < NF; k++) {
        const float p = fout_[k][0] * fout_[k][0] + fout_[k][1] * fout_[k][1];
        avg_[static_cast<size_t>(k)] += a * (p - avg_[static_cast<size_t>(k)]);
    }
    nspec_++;
    const double binw = static_cast<double>(FS) / NF;
    const int kmax = static_cast<int>(SEARCH_HZ / binw);
    auto at = [&](int k) { return avg_[static_cast<size_t>((k + NF) % NF)]; };   // k: -NF/2 .. NF/2
    std::vector<float> band;
    band.reserve(static_cast<size_t>(2 * kmax + 1));
    for (int k = -kmax; k <= kmax; k++) band.push_back(at(k));
    const double floor = median_of(band) / 0.96;   // the median of an average of ~15 spectra
    if (!(floor > 0)) return;
    noise_var_ = floor / win2_;
    if (nspec_ < 4) return;
    // lines
    struct Line { double f; double p; };
    std::vector<Line> lines;
    const double thr = floor * std::pow(10.0, FIND_DB / 10);
    for (int k = -kmax + 6; k <= kmax - 6; k++) {
        const float p = at(k);
        if (p < thr) continue;
        bool peak = true;
        for (int d = 1; d <= 4 && peak; d++) peak = p >= at(k - d) && p > at(k + d);
        if (!peak) continue;
        if (p < 3 * std::max(at(k - 6), at(k + 6))) continue;   // a narrow line, not a hump
        const double l = std::log(std::max(at(k - 1), 1e-30f)), c = std::log(p), r = std::log(std::max(at(k + 1), 1e-30f));
        const double den = l - 2 * c + r;
        const double d = den < 0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0;
        double sum = 0;
        for (int j = -2; j <= 2; j++) sum += at(k + j);
        lines.push_back({(k + d) * binw, sum});
    }
    // a steady tone's sidebands (fc +- f, both there) are not carriers
    std::vector<bool> side(lines.size(), false);
    for (size_t i = 0; i < lines.size(); i++)
        for (size_t s = 0; s < lines.size() && !side[i]; s++) {
            if (s == i || lines[s].p < 2 * lines[i].p) continue;
            const double mirror = 2 * lines[s].f - lines[i].f;
            if (std::fabs(lines[s].f - lines[i].f) < 150 && lines[s].p > 10 * lines[i].p) side[i] = true;
            // the spectrum itself at the mirror frequency (a voice harmonic's mirror needn't pass as a line)
            const int km = static_cast<int>(std::lround(mirror / binw));
            // (AM audio reaches ~3.4 kHz; further out a line is another transmitter's)
            if (std::abs(km) > kmax || side[i] || std::fabs(lines[s].f - lines[i].f) > 3500) continue;
            double pm = 0;
            for (int j = -2; j <= 2; j++) pm = std::max(pm, static_cast<double>(at(km + j - 2)) + at(km + j - 1) + at(km + j) + at(km + j + 1) + at(km + j + 2));
            if (pm > 0.25 * lines[i].p && pm < 4 * lines[i].p) side[i] = true;
        }
    const double now = static_cast<double>(abs_);
    std::vector<bool> used(pending_.size(), false);
    for (size_t i = 0; i < lines.size(); i++) {
        if (side[i]) continue;
        const double f = lines[i].f;
        bool known = false;
        for (auto& c : ch_)
            if (std::fabs(c->freq() - f) < MATCH_HZ) {
                c->seen(now, f);
                known = true;
            }
        if (known) continue;
        bool pend = false;
        for (size_t j = 0; j < pending_.size(); j++)
            if (std::fabs(pending_[j].f - f) < 16) {
                pending_[j].f = f;
                pending_[j].n++;
                pending_[j].miss = 0;
                used[j] = true;
                pend = true;
                break;
            }
        if (!pend) {
            pending_.push_back({f, 1, 0});
            used.push_back(true);
        }
    }
    for (size_t j = 0; j < pending_.size(); j++)
        if (!used[j]) pending_[j].miss++;
    std::vector<Pending> keep;
    for (const auto& p : pending_) {
        if (p.n >= 3) {
            start_channel(p.f);
            continue;
        }
        if (p.miss <= 1) keep.push_back(p);
    }
    pending_.swap(keep);
    // channels whose carrier is gone, and two channels on one carrier
    for (size_t i = 0; i < ch_.size();) {
        Channel& c = *ch_[i];
        const double gone = (now - std::max(c.last_seen(), c.last_carrier())) / FS;
        bool dup = false;
        for (size_t j = 0; j < ch_.size() && !dup; j++)
            if (j != i && std::fabs(ch_[j]->freq() - c.freq()) < 20 &&
                (ch_[j]->passes() > c.passes() || (ch_[j]->passes() == c.passes() && ch_[j]->serial() < c.serial())))
                dup = true;
        if (gone > DROP_S || dup) {
            if (verbose) {
                char b[96];
                snprintf(b, sizeof b, "channel %d (%+.1f Hz) closed: %s", c.serial(), c.freq(), dup ? "duplicate" : "carrier gone");
                log.push_back(b);
            }
            ch_.erase(ch_.begin() + static_cast<long>(i));
        } else {
            i++;
        }
    }
    for (auto& c : ch_) {
        double best = NAN;
        for (auto& o : ch_)
            if (o != c) {
                const double d = std::fabs(o->freq() - c->freq());
                if (!(d >= best)) best = d;
            }
        c->neighbour_hz = best;
    }
}

void Receiver::start_channel(double f) {
    if (static_cast<int>(ch_.size()) >= MAX_CHANNELS) {
        // room for it: the channel heard least lately that never showed a sweep
        size_t worst = ch_.size();
        double oldest = 1e300;
        for (size_t i = 0; i < ch_.size(); i++)
            if (ch_[i]->passes() == 0 && ch_[i]->last_seen() < oldest) oldest = ch_[i]->last_seen(), worst = i;
        if (worst == ch_.size() || (static_cast<double>(abs_) - oldest) / FS < 5) return;
        ch_.erase(ch_.begin() + static_cast<long>(worst));
    }
    static const std::vector<float> taps = lowpass_taps();
    const uint64_t back = std::min<uint64_t>(ring_n_, static_cast<uint64_t>(BACKLOG_S * FS));
    auto c = std::make_unique<Channel>(++serial_, f, abs_ - back, taps);
    c->set_noise(noise_var_);
    // the backlog from the ring, oldest first
    std::vector<Report> scratch;
    const size_t cap = ring_.size();
    size_t p = (ring_pos_ + cap - static_cast<size_t>(back)) % cap;
    uint64_t left = back;
    while (left > 0) {
        const size_t len = static_cast<size_t>(std::min<uint64_t>(left, cap - p));
        c->feed(&ring_[p], len, *this, &scratch);
        p = (p + len) % cap;
        left -= len;
    }
    if (verbose) {
        char b[96];
        snprintf(b, sizeof b, "channel %d on a carrier at %+.1f Hz", serial_, f);
        log.push_back(b);
    }
    ch_.push_back(std::move(c));
}

}  // namespace b121
