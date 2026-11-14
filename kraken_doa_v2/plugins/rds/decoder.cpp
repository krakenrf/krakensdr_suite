// RDS / RBDS + SCA decoder for wideband FM broadcasts.
//
// Input: the VFO's complex baseband at the VFO's OWN rate (Info::sample_rate
// 0: a 240 kHz VFO for RDS, 300-400 kHz to keep the outer FM sidebands a 92
// kHz SCA needs). Above 480 kHz it is filtered to the FM channel (+-120 kHz
// pass, 175 kHz stop) and decimated; 300-480 kHz VFOs get the same channel
// filter without decimation. In coherent mode with DoA on every antenna
// arrives (Info::multichannel) and is combined blindly first (the main
// eigenvector of their covariance): FM has a threshold, and +5..7 dB of
// carrier-to-noise from 5 antennas decide whether a weak station's RDS
// decodes at all.
//
// FM discriminator -> the multiplex (MPX, in Hz of deviation):
//  - RDS (57 kHz) -> rds_demod.hpp (biphase BPSK) -> rds_block.hpp (blocks,
//    offset words, error correction) -> rds_station.cpp (groups: PI, PS,
//    PTY, RadioText, RT+, eRT, Long PS, PTYN, CT, AF, ECC, PIN, EON, ODAs,
//    TMC, DAB cross-reference; RBDS call signs), per station (PI)
//  - the spectrum (mpx.cpp): pilot (stereo), RDS level, every other
//    subcarrier 20-99 kHz -> probed: SCA FM audio / FSK data (deviation,
//    bit rate), RDS2 streams (66.5 / 71.25 / 76 kHz, block-synced like RDS),
//    DARC-like wideband data, AM / DSB
//  - audio (8 kHz, Host::audio): an SCA's FM audio (150 us de-emphasis),
//    or the main programme in mono when no SCA carries audio
//
// ID separation: the PI names the programme / station. Every RDS group of
// the station on air is a kp::Talker packet "PI xxxx" (its 88 ms), so
// co-channel stations that take turns (fading DX, a drive between two
// transmitters on one frequency) each get their own bearing.

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "mpx.hpp"
#include "rds_block.hpp"
#include "rds_demod.hpp"
#include "rds_station.hpp"
#include "rds_tables.hpp"

#include <liquid/liquid.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using kp::cf;
using namespace rds;

constexpr double MAX_WORK_RATE = 480000;
constexpr double CH_PASS = 120000, CH_STOP = 175000;
const double RDS2_HZ[3] = {66500, 71250, 76000};
constexpr double RING_S = 0.6;            // MPX kept for the subcarrier probes
constexpr double PROBE_S = 0.5;
constexpr double SC_LOST_S = 10;
// the RDS subcarrier's peak deviation (Hz) per unit of the demodulator's
// half-symbol amplitude (1/2 from the DSB mixing x the shaped biphase pulse's
// peak over its matched-filter sample) - measured on rds_gen.py's signals: 2.0
// kHz peak -> amplitude 694
constexpr float INJ_PER_AMP = 2.88f;

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}
std::string fmt2(const char* f, double a, double b) {
    char o[128];
    snprintf(o, sizeof o, f, a, b);
    return o;
}

// channel filter / decimation + FM discriminator
class FmFront {
public:
    ~FmFront() { destroy(); }
    void init(double fs_in) {
        destroy();
        fs_in_ = fs_in;
        d0_ = fs_in > MAX_WORK_RATE + 1 ? static_cast<int>(std::ceil(fs_in / MAX_WORK_RATE)) : 1;
        fs_c_ = fs_in / d0_;
        filt_ = fs_c_ >= 300000;
        taps_a_ = taps_b_ = 0;
        // (a narrower filter doesn't help weak stations: the discriminator's noise
        // near 57 kHz is the noise density there, the same - measured 70-120 kHz)
        // above 480 kHz: a decimator that only keeps what would alias into the
        // channel out (stop at fs_c - 120 kHz: a wide transition, few taps)...
        if (d0_ > 1) {
            const auto h = design(CH_PASS, fs_c_ - CH_PASS, fs_in);
            taps_a_ = static_cast<int>(h.size());
            dec_ = firdecim_crcf_create(static_cast<unsigned int>(d0_), const_cast<float*>(h.data()), static_cast<unsigned int>(h.size()));
        }
        // ...then the FM channel (+-120 kHz pass, 175 kHz stop: the adjacent channels) at fs_c
        if (filt_) {
            const auto h = design(CH_PASS, CH_STOP, fs_c_);
            taps_b_ = static_cast<int>(h.size());
            flt_ = firfilt_crcf_create(const_cast<float*>(h.data()), static_cast<unsigned int>(h.size()));
        }
        reset();
    }
    static std::vector<float> design(double pass, double stop, double fs) {
        const float df = static_cast<float>((stop - pass) / fs);
        unsigned int len = estimate_req_filter_len(df, 60.0f);
        len = std::max(15u, len | 1u);
        std::vector<float> h(len);
        liquid_firdes_kaiser(len, static_cast<float>(0.5 * (pass + stop) / fs), 60.0f, 0.0f, h.data());
        float s = 0;
        for (float v : h) s += v;
        for (float& v : h) v /= s;
        return h;
    }
    void reset() {
        if (dec_) firdecim_crcf_reset(dec_);
        if (flt_) firfilt_crcf_reset(flt_);
        carry_.clear();
        prev_ = {1, 0};
        off_ = 0;
    }
    double rate() const { return fs_c_; }
    // the input sample (since reset) an MPX sample stands for
    double in_index(double j) const {
        // the channel filter's delay (fs_c samples), the decimator's, the discriminator (between two samples)
        const double jc = j - (taps_b_ ? (taps_b_ - 1) / 2.0 : 0.0) - 0.5;
        if (d0_ > 1) return jc * d0_ + (d0_ - 1) - (taps_a_ - 1) / 2.0;
        return jc;
    }
    void push(const cf* x, size_t n, std::vector<float>* mpx) {
        const cf* src = x;
        size_t cnt = n;
        if (d0_ > 1) {
            carry_.insert(carry_.end(), x, x + n);
            const size_t blocks = carry_.size() / static_cast<size_t>(d0_);
            z_.resize(blocks);
            if (blocks) firdecim_crcf_execute_block(dec_, carry_.data(), static_cast<unsigned int>(blocks), z_.data());
            carry_.erase(carry_.begin(), carry_.begin() + static_cast<long>(blocks * static_cast<size_t>(d0_)));
            src = z_.data();
            cnt = blocks;
        }
        if (flt_ && cnt) {
            tmp_.assign(src, src + cnt);
            z2_.resize(cnt);
            firfilt_crcf_execute_block(flt_, tmp_.data(), static_cast<unsigned int>(cnt), z2_.data());
            src = z2_.data();
        }
        const float k = static_cast<float>(fs_c_ / (2 * M_PI));
        const size_t base = mpx->size();
        mpx->resize(base + cnt);
        float* out = mpx->data() + base;
        for (size_t i = 0; i < cnt; i++) {
            const cf q = src[i] * std::conj(i ? src[i - 1] : prev_);
            out[i] = fast_atan2(q.imag(), q.real()) * k;
        }
        if (!cnt) return;
        prev_ = src[cnt - 1];
        double sum = 0;   // the carrier offset: the discriminator's mean
        for (size_t i = 0; i < cnt; i++) sum += out[i];
        off_ += 0.02f * (static_cast<float>(sum / static_cast<double>(cnt)) - off_);
    }
    float offset_hz() const { return off_; }
private:
    double fs_in_ = 240000, fs_c_ = 240000;
    int d0_ = 1, taps_a_ = 0, taps_b_ = 0;
    bool filt_ = false;
    firdecim_crcf dec_ = nullptr;
    firfilt_crcf flt_ = nullptr;
    std::vector<cf> carry_, z_, z2_, tmp_;
    cf prev_{1, 0};
    float off_ = 0;
    void destroy() {
        if (dec_) firdecim_crcf_destroy(dec_);
        if (flt_) firfilt_crcf_destroy(flt_);
        dec_ = nullptr;
        flt_ = nullptr;
    }
};

// One RDS data stream: its demodulator + block sync
struct Stream {
    RdsDemod demod;
    BlockSync sync;
    uint64_t groups = 0, groups_c = 0;
    double last_group = -1e9;
    bool was_synced = false;
    double try_until = 0, next_try = 0;   // RDS2: decoded for a while every 30 s until it synchronises
    // groups with >= 3 good blocks since the sync was acquired: a stream
    // counts as there from 3 (a false sync on noise - three offset words by
    // chance, about once an hour - gives none)
    int good_since_sync = 0;
    uint32_t gen = 0;    // the sync's generation good_since_sync counts for
    bool seen = false;   // has been there (RDS2: listed from then on)
    bool there() const { return sync.synced() && gen == sync.generation() && good_since_sync >= 3; }
    void count(const Group& g) {
        if (gen != sync.generation()) {
            gen = sync.generation();
            good_since_sync = 0;
        }
        if (g.good() >= 3) good_since_sync++;
    }
    std::vector<SoftBit> bits;
};

// The RDS band - RDS (57 kHz) and the RDS2 data streams 1-3 (IEC
// 62106-1:2018: 66.5 / 71.25 / 76 kHz, RDS's modulation and block
// structure), 54.6-78.4 kHz - as one complex stream at 48 kHz centred on 66.5
// kHz: one mixer and one resampler at the MPX rate serve all four
// demodulators (each then mixes its own stream at 48 kHz). The RDS2 streams
// are weak (2 kHz deviation sits a few dB over the MPX noise there - too
// little for the spectrum's subcarrier search), so they are decoded outright
// while the main RDS is synchronised: block sync is the detection
class Band {
public:
    static constexpr double CENTRE = 66500, HALF = 12500;   // 54-79 kHz
    ~Band() {
        if (dec_) firdecim_crcf_destroy(dec_);
    }
    // the band rate: fs / D >= 48 kHz (one polyphase decimator; the demodulators take any rate)
    void init(double fs) {
        fs_ = fs;
        d_ = std::max(1, static_cast<int>(fs / 48000));
        rate_ = fs / d_;
        if (dec_) firdecim_crcf_destroy(dec_);
        // pass +-12.5 kHz; stop where aliases would land in it (rate - 12.5 kHz)
        const auto h = FmFront::design(HALF, rate_ - HALF, fs);
        taps_ = static_cast<int>(h.size());
        dec_ = firdecim_crcf_create(static_cast<unsigned int>(d_), const_cast<float*>(h.data()), static_cast<unsigned int>(h.size()));
        reset();
    }
    double rate() const { return rate_; }
    void reset() {
        nco_.set(CENTRE, fs_);
        if (dec_) firdecim_crcf_reset(dec_);
        carry_.clear();
        n_out_ = 0;
    }
    // MPX -> out(); its first sample is band index first()
    void push(const float* mpx, size_t m) {
        const size_t old = carry_.size();
        carry_.resize(old + m);
        nco_.mix(mpx, carry_.data() + old, m);
        const size_t blocks = carry_.size() / static_cast<size_t>(d_);
        z_.resize(blocks);
        if (blocks) firdecim_crcf_execute_block(dec_, carry_.data(), static_cast<unsigned int>(blocks), z_.data());
        carry_.erase(carry_.begin(), carry_.begin() + static_cast<long>(blocks * static_cast<size_t>(d_)));
        first_ = n_out_;
        n_out_ += static_cast<int64_t>(blocks);
    }
    const std::vector<cf>& out() const { return z_; }
    int64_t first() const { return first_; }
    // a band sample index -> the MPX sample index (since reset) it stands for
    double to_mpx(double t) const { return t * d_ + (d_ - 1) - (taps_ - 1) / 2.0; }
private:
    double fs_ = 240000, rate_ = 48000;
    int d_ = 5, taps_ = 0;
    firdecim_crcf dec_ = nullptr;
    Mixer nco_;
    std::vector<cf> carry_, z_;
    int64_t n_out_ = 0, first_ = 0;
};

class RdsDecoder : public kp::Decoder {
public:
    explicit RdsDecoder(kp::Host& h) : Decoder(h) {
        fs_in_ = host.sample_rate();
        if (!(fs_in_ > 0)) fs_in_ = 240000;
        front_.init(fs_in_);
        fs_ = front_.rate();
        band_.init(fs_);
        rds_.demod.init(band_.rate(), 57000 - Band::CENTRE);
        rds_.sync.BIT_SAMPLES = fs_ / BIT_RATE;   // the bits' times are MPX indexes
        if (const char* d = getenv("RDS_DUMP")) rds_.demod.dump_ = fopen(d, "w");
        for (int i = 0; i < 3; i++) {
            rds2_[i].demod.init(band_.rate(), RDS2_HZ[i] - Band::CENTRE);
            rds2_[i].sync.BIT_SAMPLES = band_.rate() / BIT_RATE;
        }
        spec_.init(fs_);
        host.table_columns(COLS);
        host.table_keep(3600);
        reset_state();
    }

    void reset() override {
        front_.reset();
        rds_.demod.reset();
        rds_.sync.reset();
        band_.reset();
        for (auto& s : rds2_) {
            s.demod.reset();
            s.sync.reset();
        }
        spec_.reset();
        reset_state();
        host.table_columns(COLS);
    }

    void option(const std::string& key, const std::string& v) override {
        if (key == "audio") audio_mode_ = v.empty() ? "auto" : v;
        else if (key == "sca_khz") sca_khz_ = atof(v.c_str());
        else if (key == "deemph") deemph_ = v.empty() ? 150 : atof(v.c_str());
        else if (key == "pty") gd_.set_pty_mode(v == "rds" ? 1 : v == "rbds" ? 2 : 0);
        else if (key == "array") array_ = v != "0";
        else return;
        sca_.reset();   // re-chosen on the next block
        sca_target_ = 0;
    }

    void process(const kp::cf* x, size_t n) override {
        antennas_ = 1;
        run(x, n);
    }

    void process_multi(const kp::cf* const* x, int m, size_t n) override {
        if (!array_ || m < 2 || fs_in_ > MAX_WORK_RATE + 1) {
            antennas_ = 1;
            run(x[0], n);
            return;
        }
        if (comb_.m() != m) comb_.init(m, fs_in_);
        antennas_ = m;
        cy_.resize(n);
        comb_.push(x, n, cy_.data());
        run(cy_.data(), n);
    }

private:
    const std::vector<std::string> COLS = {"PI", "Station", "PS", "PTY", "RadioText", "Flags", "AF", "Groups", "BLER"};
    double fs_in_ = 240000, fs_ = 240000;
    FmFront front_;
    Combiner comb_;
    std::vector<cf> cy_;
    int antennas_ = 1;
    bool array_ = true;
    Band band_;
    Stream rds_;
    Stream rds2_[3];
    MpxSpectrum spec_;
    GroupDecoder gd_;
    std::vector<float> mpx_;
    std::vector<float> ring_;      // the last RING_S of MPX
    size_t ring_pos_ = 0, ring_fill_ = 0;
    std::vector<float> probe_buf_;
    int64_t mpx_n_ = 0;            // MPX samples since reset
    uint64_t in_n_ = 0;            // input samples since reset
    double t0_ = 0;                // host time of input sample 0
    double now_ = 0;
    double next_ana_ = 0, next_facts_ = 0;
    std::vector<Subcarrier> scs_;
    // audio
    std::string audio_mode_ = "auto";
    double sca_khz_ = 0, deemph_ = 150;
    std::unique_ptr<ScaAudio> sca_;
    std::unique_ptr<MainAudio> main_;
    double sca_target_ = 0;
    std::string voice_;
    std::vector<float> pcm_;
    // receiver measurements
    float pilot_hz_ = 0, pilot_snr_ = -99, rds_inj_ = 0;
    bool stereo_ = false;
    std::map<uint16_t, double> row_t_;
    double last_sync_note_ = -1e9;
    uint16_t last_pi_ = 0;

    void reset_state() {
        mpx_n_ = 0;
        in_n_ = 0;
        ring_.clear();
        ring_pos_ = ring_fill_ = 0;
        next_ana_ = next_facts_ = 0;
        scs_.clear();
        gd_.reset();
        rds_.groups = rds_.groups_c = 0;
        rds_.good_since_sync = 0;
        rds_.was_synced = false;
        for (auto& s : rds2_) {
            s.groups = s.groups_c = 0;
            s.good_since_sync = 0;
            s.seen = false;
            s.was_synced = false;
            s.try_until = s.next_try = 0;
        }
        sca_.reset();
        main_.reset();
        sca_target_ = 0;
        voice_.clear();
        pilot_hz_ = 0;
        pilot_snr_ = -99;
        rds_inj_ = 0;
        stereo_ = false;
        row_t_.clear();
        last_pi_ = 0;
        double la, lo;
        const bool have = host.station(&la, &lo);
        gd_.set_location(have, la, lo);
    }

    // host time of an MPX sample index
    double mpx_time(double j) const { return t0_ + front_.in_index(j) / fs_in_; }
    void run(const kp::cf* x, size_t n) {
        now_ = host.time();
        t0_ = now_ - static_cast<double>(in_n_) / fs_in_;
        in_n_ += n;
        mpx_.clear();
        front_.push(x, n, &mpx_);
        const size_t m = mpx_.size();
        mpx_n_ += static_cast<int64_t>(m);
        if (m == 0) return;
        spec_.push(mpx_.data(), m);
        keep_ring(m);
        // RDS (its bits' times: band indexes -> MPX indexes)
        band_.push(mpx_.data(), m);
        const auto& z = band_.out();
        rds_.bits.clear();
        rds_.demod.push(z.data(), z.size(), band_.first(), &rds_.bits);
        for (auto& b : rds_.bits) {
            b.t = band_.to_mpx(b.t);
            Group g;
            if (rds_.sync.push(b, &g)) {
                rds_.count(g);
                on_group(g);
            }
        }
        sync_note();
        // RDS2 streams: only beside a station's RDS; a stream that isn't
        // synchronised is tried for 4 s every 30 s (most stations have none)
        if (rds_.there() || now_ - rds_.last_group < 5)
            for (auto& s : rds2_) {
                if (!s.sync.synced() && now_ >= s.try_until) {
                    if (now_ < s.next_try) continue;
                    s.try_until = now_ + 4;
                    s.next_try = now_ + 30;
                    s.demod.reset();
                    s.sync.reset();
                }
                s.bits.clear();
                s.demod.push(z.data(), z.size(), band_.first(), &s.bits);
                for (const auto& b : s.bits) {
                    Group g;
                    if (s.sync.push(b, &g)) {
                        s.count(g);
                        s.groups++;
                        if (g.clean()) s.groups_c++;
                        s.last_group = now_;
                    }
                }
            }
        audio(m);
        if (now_ >= next_ana_) {
            next_ana_ = now_ + 0.5;
            analyse();
        }
        if (now_ >= next_facts_) {
            next_facts_ = now_ + 1.0;
            facts();
        }
    }

    // the MPX ring (circular; ring_pos_ = the next write)
    void keep_ring(size_t m) {
        if (ring_.empty()) ring_.assign(static_cast<size_t>(RING_S * fs_), 0.0f);
        const size_t cap = ring_.size();
        for (size_t i = 0; i < m;) {
            const size_t k = std::min(m - i, cap - ring_pos_);
            std::copy(mpx_.begin() + static_cast<long>(i), mpx_.begin() + static_cast<long>(i + k), ring_.begin() + static_cast<long>(ring_pos_));
            ring_pos_ = (ring_pos_ + k) % cap;
            i += k;
        }
        ring_fill_ = std::min(cap, ring_fill_ + m);
    }
    // the last len samples of the ring, in order
    bool ring_last(size_t len, std::vector<float>* out) const {
        if (len > ring_fill_) return false;
        const size_t cap = ring_.size();
        out->resize(len);
        const size_t start = (ring_pos_ + cap - len) % cap;
        for (size_t i = 0; i < len; i++) (*out)[i] = ring_[(start + i) % cap];
        return true;
    }

    // "synchronised" when RDS comes, "lost" when it has been gone 3 s (a
    // fade, a moment of multipath - or a gap between two stations - says nothing)
    void sync_note() {
        const bool s = rds_.there();
        if (s) {
            last_sync_note_ = now_;
            if (!rds_.was_synced) host.event("RDS: synchronised", 10);
            rds_.was_synced = true;
        } else if (rds_.was_synced && now_ - last_sync_note_ > 3) {
            host.event("RDS: lost", 10);
            rds_.was_synced = false;
        }
    }

    // ---- groups ------------------------------------------------------------------------
    void on_group(const Group& g) {
        rds_.groups++;
        if (g.clean()) rds_.groups_c++;
        rds_.last_group = now_;
        const double ta = mpx_time(g.t0), tb = mpx_time(g.t1);
        if (host.raw_wanted()) {
            std::string r;
            for (int k = 0; k < 4; k++) {
                if (k) r += ' ';
                r += g.b[k].st ? GroupDecoder::pi_hex(g.b[k].v) : "----";
            }
            host.raw(r);
        }
        const uint16_t pi = gd_.handle(g, tb);
        for (auto& [txt, dd] : gd_.take_news()) host.event(txt, dd);
        if (gd_.current() != last_pi_) {   // another station: its RDS2 streams are tried at once
            last_pi_ = gd_.current();
            for (auto& s : rds2_)
                if (!s.sync.synced()) s.next_try = s.try_until = 0;
        }
        if (!pi || g.good() < 3) return;
        host.valid(ta, tb);
        const Station* s = gd_.station(pi);
        // this group's samples are the station's: its own bearing
        kp::Talker t;
        t.id = "PI " + GroupDecoder::pi_hex(pi);
        t.label = label(*s);
        t.start_s = ta;
        t.end_s = tb;
        t.packet = true;
        t.freq_hz = 1000 * std::round(front_.offset_hz() / 1000);   // the carrier, to the kHz (a 200 kHz channel)
        t.bw_hz = std::min(200000.0, 0.8 * fs_in_);
        t.avg_s = 10;
        t.keep_s = 1800;
        host.talker(t);
        if (host.verbose()) host.event(group_line(g, *s), 0.0);
        auto it = row_t_.find(pi);
        if (it == row_t_.end() || now_ - it->second >= 1.0) {
            row_t_[pi] = now_;
            row(*s);
        }
    }

    std::string label(const Station& s) const {
        std::string l = trim(s.ps_str);
        const std::string cs = gd_.callsign(s);
        if (!cs.empty()) l = l.empty() ? cs : cs + " · " + l;
        const double rf = host.rf_hz();
        if (std::isfinite(rf)) l += (l.empty() ? "" : " · ") + fmt("%.1f MHz", rf / 1e6);
        return l.substr(0, 64);
    }

    std::string group_line(const Group& g, const Station& s) const {
        std::string o = "[";
        if (g.b[1].st) o += std::to_string(g.b[1].v >> 12) + ((g.b[1].v >> 11 & 1) ? "B" : "A");
        else o += "?";
        o += "] PI " + GroupDecoder::pi_hex(s.pi);
        for (int k = 1; k < 4; k++) o += " " + (g.b[k].st ? GroupDecoder::pi_hex(g.b[k].v) : std::string("----"));
        int fixed = 0;
        for (const auto& b : g.b) fixed += b.nfix;
        if (fixed) o += " (" + std::to_string(fixed) + " bits corrected)";
        return o;
    }

    std::string flags(const Station& s) const {
        std::string f;
        auto add = [&](const std::string& x) { f += (f.empty() ? "" : " ") + x; };
        if (s.tp == 1) add("TP");
        if (s.ta == 1) add("TA");
        if (s.ms >= 0) add(s.ms ? "music" : "speech");
        if (s.di_known & 1) add(s.di & 1 ? "stereo" : "mono");
        if ((s.di_known & 8) && (s.di & 8)) add("dynPTY");
        if ((s.di_known & 4) && (s.di & 4)) add("compressed");
        if ((s.di_known & 2) && (s.di & 2)) add("art.head");
        return f;
    }

    std::string af_list(const Station& s, size_t maxn) const {
        std::string a;
        size_t k = 0;
        const double rf = host.rf_hz();
        const int tuned = std::isfinite(rf) ? static_cast<int>(std::lround((rf / 1e6 - 87.5) * 10)) : -1;
        for (int c : s.af) {
            if (k++ == maxn) {
                a += " +" + std::to_string(s.af.size() - maxn);
                break;
            }
            a += (a.empty() ? "" : " ") + GroupDecoder::af_mhz(c) + (c == tuned ? "*" : "");
        }
        for (int khz : s.af_lfmf) a += (a.empty() ? "" : " ") + std::to_string(khz) + " kHz";
        return a;
    }

    std::string station_name(const Station& s) const {
        std::string n = gd_.callsign(s);
        const std::string c = gd_.country(s);
        if (!c.empty()) n += (n.empty() ? "" : " · ") + c;
        return n;
    }

    std::string rtplus_now(const Station& s) const {
        std::string title, artist, other;
        for (const auto& t : s.rtp) {
            if (t.type == 1) title = t.text;
            else if (t.type == 4 || t.type == 9) artist = t.text;
            else if (other.empty() && t.type != 0) other = std::string(rtplus_type(t.type)) + ": " + t.text;
        }
        if (!title.empty() && !artist.empty()) return title + " — " + artist;
        if (!title.empty()) return title;
        if (!artist.empty()) return artist;
        return other;
    }

    std::string oda_list(const Station& s) const {
        std::string o;
        for (const auto& [aid, msg] : s.oda_msg) {
            (void)msg;
            const char* nm = oda_name(aid);
            std::string where;
            for (const auto& [gt, a] : s.oda)
                if (a == aid) where = " (" + std::to_string(gt >> 1) + (gt & 1 ? "B" : "A") + ")";
            o += (o.empty() ? "" : ", ") + std::string(*nm ? nm : "AID " + GroupDecoder::pi_hex(aid)) + where;
        }
        return o;
    }

    std::string group_stats(const Station& s) const {
        std::vector<std::pair<uint64_t, int>> v;
        for (int i = 0; i < 32; i++)
            if (s.type_count[i]) v.emplace_back(s.type_count[i], i);
        std::sort(v.rbegin(), v.rend());
        std::string o;
        for (size_t k = 0; k < v.size() && k < 8; k++)
            o += (o.empty() ? "" : ", ") + std::to_string(v[k].second >> 1) + (v[k].second & 1 ? "B " : "A ") +
                 std::to_string(v[k].first);
        return o;
    }

    void row(const Station& s) {
        const std::string pi = GroupDecoder::pi_hex(s.pi);
        std::string pty = s.pty >= 0 ? pty_name(s.pty, gd_.rbds(s)) : "";
        if (!s.ptyn_str.empty()) pty += (pty.empty() ? "" : " / ") + s.ptyn_str;
        const float bler = rds_.sync.bler();
        host.table_row("PI " + pi, {pi, station_name(s), trim(s.ps_str), pty, s.rt_str, flags(s), af_list(s, 6),
                                    std::to_string(s.groups),
                                    s.pi == gd_.current() && rds_.there() ? fmt("%.0f %%", 100 * bler) : ""});
        std::vector<std::pair<std::string, std::string>> d;
        auto add = [&](const std::string& k, const std::string& v) {
            if (!v.empty()) d.emplace_back(k, v.substr(0, 400));
        };
        add("PI", pi + (gd_.rbds(s) ? "" : " · area: " + GroupDecoder::area(s.pi)));
        add("Call sign (from the PI)", gd_.callsign(s));
        if (s.ecc >= 0)
            add("Extended country code", GroupDecoder::pi_hex(static_cast<uint16_t>(s.ecc)).substr(2) +
                                             (gd_.country(s).empty() ? "" : " = " + gd_.country(s)));
        add("PS", trim(s.ps_str) + (s.dynamic_ps ? " (dynamic)" : ""));
        add("Long PS", s.lps_str);
        add("Programme type", gd_.pty_text(s, s.pty) + (gd_.rbds(s) ? " (RBDS list)" : " (RDS list)"));
        add("Programme type name", s.ptyn_str);
        add("RadioText", s.rt_str);
        add("Enhanced RadioText", s.ert_str);
        for (const auto& t : s.rtp) add(std::string("RT+ ") + rtplus_type(t.type), t.text);
        add("Flags", flags(s));
        add("Alternative frequencies", af_list(s, 40));
        add("Clock time", s.ct_local.empty() ? "" : s.ct_local + " = " + s.ct);
        add("Programme item", gd_.pin_text(s.pin));
        if (s.lang >= 0) add("Language", language_name(s.lang));
        add("Open data applications", oda_list(s));
        if (s.tmc_groups) add("RDS-TMC", std::to_string(s.tmc_groups) + " groups" + (s.tmc_info.empty() ? "" : ", " + s.tmc_info));
        add("DAB", s.dab);
        for (const auto& [opi, on] : s.eon) {
            std::string v = trim(on.ps_str);
            if (on.pty >= 0) v += (v.empty() ? "" : " · ") + std::string(pty_name(on.pty, gd_.rbds(s)));
            if (on.tp == 1) v += " · TP";
            if (on.ta == 1) v += " TA";
            if (!on.af.empty()) {
                std::string a;
                for (int c : on.af) a += (a.empty() ? "" : " ") + GroupDecoder::af_mhz(c);
                v += " · " + a;
            }
            add("EON PI " + GroupDecoder::pi_hex(opi), v);
        }
        if (s.ews_groups) add("Emergency warning groups (9A)", std::to_string(s.ews_groups));
        if (s.paging_groups) add("Paging groups", std::to_string(s.paging_groups));
        if (s.tdc_groups) add("Transparent data groups (5)", std::to_string(s.tdc_groups));
        if (s.ih_groups) add("In-house groups (6)", std::to_string(s.ih_groups));
        add("Groups", std::to_string(s.groups) + " (" + std::to_string(s.clean_groups) + " without errors): " + group_stats(s));
        host.table_detail("PI " + pi, d);
    }

    // ---- the spectrum: pilot, RDS, subcarriers --------------------------------------
    void analyse() {
        if (spec_.frames() < 20) return;
        const auto& P = spec_.p();
        const int nb = spec_.bins();
        const double bw = spec_.bin_hz();
        // the floor: a low percentile around each bin (+-16 kHz)
        std::vector<float> fl(static_cast<size_t>(nb));
        const int W = std::max(4, static_cast<int>(16000 / bw));
        std::vector<float> tmp;
        for (int k = 0; k < nb; k++) {
            const int a = std::max(0, k - W), b = std::min(nb - 1, k + W);
            tmp.assign(P.begin() + a, P.begin() + b + 1);
            const size_t q = tmp.size() / 5;
            std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(q), tmp.end());
            fl[static_cast<size_t>(k)] = std::max(tmp[q], 1e-12f);
        }
        // pilot (19 kHz +-1 bin) against the floor around it
        const int kp = spec_.bin_of(19000);
        double pp = 0, pf = 0;
        for (int k = kp - 1; k <= kp + 1; k++) {
            pp += P[static_cast<size_t>(k)];
            pf += fl[static_cast<size_t>(k)];
        }
        pilot_snr_ = static_cast<float>(10 * std::log10(std::max(1e-12, pp / pf)));
        stereo_ = pilot_snr_ > 15;
        pilot_hz_ = stereo_ ? static_cast<float>(std::sqrt(2 * std::max(0.0, pp - pf))) : 0;
        // RDS injection (peak deviation of the subcarrier) from the demodulator's
        // half-symbol amplitude - the spectrum's 57 kHz band is biased by a 67 kHz SCA's skirt
        if (rds_.there()) rds_inj_ = rds_.demod.amp() * INJ_PER_AMP;
        // candidate subcarriers 20 (stereo: 54) .. 99 kHz
        const double lo = stereo_ ? 53500 : 20000, hi = std::min(100000.0, 0.47 * fs_);
        std::vector<float> sm(static_cast<size_t>(nb));
        const int S = std::max(1, static_cast<int>(1000 / bw));
        for (int k = 0; k < nb; k++) {
            double s = 0;
            int c = 0;
            for (int i = std::max(0, k - S); i <= std::min(nb - 1, k + S); i++, c++) s += P[static_cast<size_t>(i)];
            sm[static_cast<size_t>(k)] = static_cast<float>(s / c);
        }
        auto excluded = [&](double f) {
            if (std::fabs(f - 19000) < 1500 || std::fabs(f - 38000) < 1500) return true;
            if (rds_.there() && std::fabs(f - 57000) < 2600) return true;
            for (int i = 0; i < 3; i++)   // a decoded RDS2 stream is listed as such
                if (rds2_[i].there() && std::fabs(f - RDS2_HZ[i]) < 2600) return true;
            return false;
        };
        // a subcarrier: >= 5 dB over the floor (the 0.8 s average of ~200
        // spectra is steady to a few %), >= 3 kHz of it >= 2.5 dB, a peak >= 7 dB
        std::vector<Subcarrier> found;
        int k = spec_.bin_of(lo);
        const int kend = spec_.bin_of(hi);
        while (k <= kend) {
            if (excluded(k * bw) || sm[static_cast<size_t>(k)] < 3.2f * fl[static_cast<size_t>(k)]) {
                k++;
                continue;
            }
            int a = k;
            double peak = 0, sp = 0, sf = 0, sw = 0;
            while (k <= kend && !excluded(k * bw) && sm[static_cast<size_t>(k)] >= 1.8f * fl[static_cast<size_t>(k)]) {
                const double e = P[static_cast<size_t>(k)] - fl[static_cast<size_t>(k)];
                peak = std::max(peak, static_cast<double>(sm[static_cast<size_t>(k)] / fl[static_cast<size_t>(k)]));
                if (e > 0) {
                    sp += e;
                    sw += e * k * bw;
                }
                sf += fl[static_cast<size_t>(k)];
                k++;
            }
            const double width = (k - a) * bw;
            if (width < 3000 || peak < 5 || sp <= 0) continue;
            Subcarrier s;
            s.hz = sw / sp;
            s.width = width;
            s.inj_hz = std::sqrt(2 * sp);
            s.snr_db = 10 * std::log10(std::max(1e-12, sp / std::max(sf, 1e-12)));
            found.push_back(s);
        }
        // one subcarrier in two pieces - wide FSK's two tones, with a dip between
        // them while the average builds up: neighbours < 2.5 kHz apart within 6
        // dB of each other (RDS next to an SCA is ~10 dB weaker), <= 24 kHz in all
        for (size_t i = 0; i + 1 < found.size();) {
            Subcarrier& a = found[i];
            const Subcarrier& b = found[i + 1];
            const double lo_a = a.hz - a.width / 2, hi_b = b.hz + b.width / 2;
            const double gap = (b.hz - b.width / 2) - (a.hz + a.width / 2);
            const double r = a.inj_hz * a.inj_hz / std::max(1e-9, b.inj_hz * b.inj_hz);
            if (gap < 2500 && r < 4 && r > 0.25 && hi_b - lo_a <= 24000) {
                const double pa = a.inj_hz * a.inj_hz, pb = b.inj_hz * b.inj_hz;
                a.hz = (a.hz * pa + b.hz * pb) / (pa + pb);
                a.width = hi_b - lo_a;
                a.inj_hz = std::sqrt(pa + pb);
                a.snr_db = std::max(a.snr_db, b.snr_db);
                found.erase(found.begin() + static_cast<long>(i + 1));
            } else {
                i++;
            }
        }
        // the skirt of a stronger neighbour (an FM subcarrier's outer
        // sidebands past a dip) is not a subcarrier: within 2.5 kHz of its band,
        // 12 dB or more below it
        for (size_t i = 0; i < found.size();) {
            bool skirt = false;
            for (size_t j = 0; j < found.size() && !skirt; j++) {
                if (j == i) continue;
                const double gap = std::fabs(found[i].hz - found[j].hz) - 0.5 * (found[i].width + found[j].width);
                skirt = gap < 2500 && found[i].inj_hz * found[i].inj_hz * 16 < found[j].inj_hz * found[j].inj_hz;
            }
            if (skirt) found.erase(found.begin() + static_cast<long>(i));
            else i++;
        }
        // match with the tracked ones
        for (auto& f : found) {
            Subcarrier* t = nullptr;
            for (auto& s : scs_)
                // the same subcarrier: its band overlaps (a wide FSK's centroid
                // wanders with the data while the average builds up)
                if (std::fabs(s.hz - f.hz) < std::max(2000.0, 0.35 * (s.width + f.width))) t = &s;
            if (!t) {
                f.first = now_;
                scs_.push_back(f);
                t = &scs_.back();
            } else {
                t->width = f.width;
                t->inj_hz = f.inj_hz;
                t->snr_db = f.snr_db;
                if (t->kind == Subcarrier::UNKNOWN || t->kind == Subcarrier::AM) t->hz = f.hz;
            }
            t->last = now_;
        }
        // probe one at a time, the least recently probed (each again every 30 s)
        Subcarrier* pr = nullptr;
        for (auto& s : scs_)
            if (s.last == now_ && now_ - s.probe_t >= (s.probes ? 30.0 : 0.0) && (!pr || s.probe_t < pr->probe_t)) pr = &s;
        if (pr && ring_last(static_cast<size_t>(PROBE_S * fs_), &probe_buf_)) {
            probe_subcarrier(probe_buf_.data(), probe_buf_.size(), fs_, pr);
            pr->probe_t = now_;
            if (!pr->announced) {
                pr->announced = true;
                host.event("Subcarrier " + describe(*pr), 60);
            }
        }
        // gone
        for (auto it = scs_.begin(); it != scs_.end();) {
            if (now_ - it->last > SC_LOST_S) {
                if (it->announced) host.event(fmt("Subcarrier %.1f kHz gone", it->hz / 1e3), 60);
                it = scs_.erase(it);
            } else {
                ++it;
            }
        }
        // RDS2 streams
        for (int i = 0; i < 3; i++) {
            Stream& st = rds2_[i];
            const bool s = st.there();
            st.seen = st.seen || s;
            if (s && !st.was_synced)
                host.event(fmt("RDS2 data stream %.0f", i + 1.0) + fmt(" (%.2f kHz) synchronised", RDS2_HZ[i] / 1e3), 60);
            st.was_synced = s;
        }
    }

    // ---- audio -------------------------------------------------------------------------
    // what to play: an SCA (its frequency), 0 = the main programme, -1 = nothing
    double audio_choice() const {
        const Subcarrier* best = nullptr;
        for (const auto& s : scs_)
            if (s.kind == Subcarrier::FM_AUDIO && (!best || s.inj_hz > best->inj_hz)) best = &s;
        auto near = [&](double f) {
            for (const auto& s : scs_)
                if (std::fabs(s.hz - f) < 2000) return s.hz;
            return f;
        };
        if (audio_mode_ == "main") return 0;
        if (audio_mode_ == "67") return near(67000);
        if (audio_mode_ == "92") return near(92000);
        if (audio_mode_ == "other") return sca_khz_ >= 20 && sca_khz_ <= 99 ? near(sca_khz_ * 1e3) : -1;
        if (audio_mode_ == "sca") return best ? best->hz : -1;
        return best ? best->hz : 0;   // auto
    }

    void audio(size_t m) {
        if (!host.voice_wanted()) {
            if (!voice_.empty()) {
                voice_.clear();
                host.voice_state("");
            }
            return;
        }
        const double c = audio_choice();
        std::string vs;
        pcm_.clear();
        if (c > 0) {
            if (!sca_ || std::fabs(c - sca_target_) > 300) {
                double half = 8000, dev = 7500;
                for (const auto& s : scs_)
                    if (std::fabs(s.hz - c) < 300) {
                        half = std::clamp(s.width / 2 + 1000, 5000.0, 10000.0);
                        if (s.kind == Subcarrier::FM_AUDIO) dev = std::max(3000.0, 1.4 * s.dev_peak);
                    }
                sca_ = std::make_unique<ScaAudio>();
                sca_->init(fs_, c, half, deemph_, dev);
                sca_target_ = c;
            }
            sca_->push(mpx_.data(), m, &pcm_);
            vs = fmt("SCA %.1f kHz", c / 1e3);
            // the subcarrier went off the air (the spectrum hasn't shown it for
            // 1.5 s; it stays listed 10 s): silence instead of its noise
            for (const auto& s : scs_)
                if (std::fabs(s.hz - c) < 300 && now_ - s.last > 1.5) {
                    std::fill(pcm_.begin(), pcm_.end(), 0.0f);
                    vs += " (no carrier)";
                }
        } else if (c == 0) {
            if (!main_) {
                main_ = std::make_unique<MainAudio>();
                main_->init(fs_, 75);
            }
            main_->push(mpx_.data(), m, &pcm_);
            vs = "main programme (mono)";
        } else {
            vs = "no SCA audio";
        }
        if (!pcm_.empty()) host.audio(pcm_.data(), pcm_.size());
        if (vs != voice_) {
            voice_ = vs;
            host.voice_state(vs);
        }
    }

    // ---- facts -------------------------------------------------------------------------
    void facts() {
        if (getenv("RDS_DEBUG"))
            host.log(fmt("t %.1f", now_) + fmt(" amp %.1f", rds_.demod.amp()) + fmt(" snr %.1f", rds_.demod.snr_db()) +
                     fmt(" cf %.2f Hz", rds_.demod.carrier_hz()) + fmt(" sync %.0f", rds_.sync.synced()) +
                     fmt(" ok %.0f", static_cast<double>(rds_.sync.blocks_ok)) +
                     fmt(" fixed %.0f", static_cast<double>(rds_.sync.blocks_fixed)) +
                     fmt(" bad %.0f", static_cast<double>(rds_.sync.blocks_bad)));
        const Station* s = gd_.current() ? gd_.station(gd_.current()) : nullptr;
        auto fact = [&](const std::string& k, const std::string& v) { host.fact(k, v.substr(0, 400)); };
        if (s) {
            std::string st = "PI " + GroupDecoder::pi_hex(s->pi);
            const std::string cs = gd_.callsign(*s), cn = gd_.country(*s);
            if (!cs.empty()) st += " · " + cs;
            if (!cn.empty()) st += " · " + cn;
            fact("Station", st);
            fact("PS", trim(s->ps_str) + (s->dynamic_ps ? " (dynamic)" : ""));
            fact("Long PS", s->lps_str);
            fact("Programme type", gd_.pty_text(*s, s->pty) + (s->ptyn_str.empty() ? "" : " · " + s->ptyn_str));
            fact("RadioText", s->rt_str);
            fact("Enhanced RadioText", s->ert_str);
            fact("Now playing (RT+)", rtplus_now(*s));
            fact("Flags", flags(*s));
            fact("Alternative frequencies", af_list(*s, 12));
            fact("Clock time", s->ct_local);
            fact("Programme item", gd_.pin_text(s->pin));
            fact("Open data applications", oda_list(*s));
            fact("Other networks (EON)", s->eon.empty() ? "" : std::to_string(s->eon.size()));
            fact("Groups", std::to_string(s->groups) + ": " + group_stats(*s));
        } else {
            for (const char* k : {"Station", "PS", "Long PS", "Programme type", "RadioText", "Enhanced RadioText",
                                  "Now playing (RT+)", "Flags", "Alternative frequencies", "Clock time", "Programme item",
                                  "Open data applications", "Other networks (EON)", "Groups"})
                host.fact(k, "");
        }
        std::string r;
        if (rds_.there()) {
            r = fmt("synchronised · block errors %.0f %%", 100 * rds_.sync.bler());
            r += fmt(" · SNR %.0f dB", rds_.demod.snr_db());
            if (rds_inj_ > 0) r += fmt2(" · injection %.1f kHz (%.1f %%)", rds_inj_ / 1e3, 100 * rds_inj_ / 75000);
        } else {
            r = rds_.groups ? "lost - searching" : "searching (no RDS found yet)";
        }
        fact("RDS", r);
        fact("Pilot", stereo_ ? fmt2("19 kHz, %.1f kHz (%.1f %%) - stereo", pilot_hz_ / 1e3, 100 * pilot_hz_ / 75000)
                              : (spec_.frames() >= 20 ? "none - mono" : ""));
        std::string sc;
        for (const auto& c : scs_)
            if (c.probes) sc += (sc.empty() ? "" : " · ") + describe(c);
        fact("Subcarriers", sc);
        std::string r2;
        for (int i = 0; i < 3; i++) {
            const Stream& st = rds2_[i];
            if (!st.seen) continue;   // never there: no stream
            r2 += (r2.empty() ? "" : " · ") + fmt("%.2f kHz: ", RDS2_HZ[i] / 1e3) +
                  (st.there() ? fmt("%.0f groups", static_cast<double>(st.groups)) + fmt(", block errors %.0f %%", 100 * st.sync.bler())
                              : std::string("lost"));
        }
        fact("RDS2 streams", r2);
        fact("Audio", host.voice_wanted() ? voice_ : "");
        fact("Carrier offset", fmt("%+.1f kHz", front_.offset_hz() / 1e3));
        fact("Antennas", antennas_ > 1 ? fmt("%.0f combined", static_cast<double>(antennas_)) + fmt(" (main eigenvalue %.1f dB over the rest)", comb_.ratio_db())
                                       : "1");
        std::string bwn = fmt("%.0f kHz", fs_in_ / 1e3);
        if (fs_in_ < 200000) bwn += " - narrow for FM broadcast: 240 kHz for RDS";
        else if (fs_ < 300000) bwn += " - SCA above ~80 kHz needs 300-400 kHz";
        fact("VFO bandwidth", bwn);
    }
};

}  // namespace

KRAKEN_PLUGIN(RdsDecoder,
              {.id = "rds",
               .name = "RDS / RBDS + SCA (FM broadcast)",
               .category = "Broadcast & cellular",
               .description = "Wideband FM broadcast subcarriers: RDS / RBDS on 57 kHz (PI, call sign, PS, programme "
                              "type, RadioText, RT+, eRT, Long PS, clock time, alternative frequencies, country, EON, "
                              "ODAs / TMC / DAB links), RDS2 streams, the pilot and every SCA subcarrier (FM audio - "
                              "reading services, background music -, FSK data, DARC). Listen plays an SCA's audio, or "
                              "the main programme in mono. A DoA per station (PI). VFO 240 kHz for RDS, 400 kHz for "
                              "SCA at 92 kHz",
               .version = "1.0",
               .sample_rate = 0,
               .min_vfo_rate = 150000,
               .author = "KrakenSDR",
               .options = {{"audio", "Audio", "auto",
                            "auto=Auto (an SCA with audio, else the main programme)|main=Main programme (mono)|"
                            "sca=Strongest SCA only|67=SCA 67 kHz|92=SCA 92 kHz|other=SCA at the frequency below",
                            "What Listen (Demod Digital) plays: an SCA subcarrier's FM audio, or the station's main "
                            "programme in mono (8 kHz)"},
                           {"sca_khz", "SCA kHz", "", "",
                            "The subcarrier frequency in kHz (20-99) when Audio is 'SCA at the frequency below'"},
                           {"deemph", "SCA de-emphasis", "150", "150=150 us (SCA standard)|75=75 us|50=50 us|0=Off",
                            "De-emphasis of the SCA audio"},
                           {"pty", "Programme types", "auto",
                            "auto=Auto (RBDS in the US / Canada / Mexico)|rds=RDS (Europe and elsewhere)|rbds=RBDS (North America)",
                            "Which programme type list names the PTY code: RDS and RBDS use different ones. Auto "
                            "takes the station's extended country code, else the receiver's location"},
                           {"array", "Array combining", "1", "0=Off|1=On",
                            "In coherent mode with DoA on: combine every antenna before the FM discriminator "
                            "(weak stations decode with several dB more signal); VFOs up to 480 kHz"}},
               .map = false,
               .manual_only = false,
               .voice = true,
               .messages = false,
               .talkers = true,
               .multichannel = true})
