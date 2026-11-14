// ADS-B / Mode S decoder (1090 MHz), for a VFO of the full 2.4 MHz bandwidth.
//
// Physical layer: pulse-position modulation at 1 Mbit/s - every bit is a
// 0.5 us pulse in the first ("1") or second ("0") half of its 1 us slot -
// after an 8 us preamble with pulses at 0, 1, 3.5 and 4.5 us. 56-bit (short)
// or 112-bit (long) messages, protected by a 24-bit CRC whose parity field
// is either plain (DF11 all-call / DF17-18 extended squitter) or XORed with
// the aircraft address (replies DF0/4/5/16/20/21).
//
// Demodulator: at 2.4 MHz one 0.5 us chip is 1.2 samples, so the magnitude
// is integrated over each chip at five sub-sample timings (0.2-sample steps,
// box-filter weights precomputed per chip). A cheap amplitude gate picks
// preamble candidates; the preamble is then scored at the five timings and
// the best two are sliced into bits (first half > second half) and checked
// by CRC. DF17/18 messages with one bad bit (syndrome table) or two bad bits
// among the eight least certain ones are repaired - only for aircraft
// already heard cleanly, which keeps noise from inventing aircraft. Option
// "fix" 2 (extended) also repairs three of the twelve least certain bits,
// DF11 all-calls and the address / parity replies - those only when the
// message's spatial signature (array) or level (one antenna) is the
// aircraft's.
//
// Every antenna (Info::multichannel: coherent mode with DoA on, option
// "array"): the magnitude above is the noise-normalised sum of every
// antenna's power - its noise fluctuates far less than one antenna's, so
// weak preambles pass the gate. A candidate that doesn't decode that way is
// beamformed: maximum-ratio weights from its preamble pulses (the principal
// eigenvector of their covariance), the beam's magnitude sliced; then from
// every pulse of the message as sliced (decision-directed); and with option
// "separate" an MVDR beam aimed at the message's own pulses (minus its gaps,
// where an overlapping message shows too) that nulls what else is in its
// window. After a decode, "separate" also looks for a second message
// starting inside it, with the first one's signature projected out. The
// DoA of each message (option "pulse_doa") is the covariance of its own
// pulse samples, leaving out samples another decoded message has a pulse in
// (kp::Talker::cov).
//
// Decoding: identification (callsign, category), airborne and surface
// positions (CPR, global even/odd decoding, then local decoding against the
// aircraft's last position; range and speed checks), velocity (ground speed /
// track, airspeed / heading, vertical rate), emergency / squawk (TC 28),
// altitude and squawk from DF4/5/20/21 replies. Aircraft with a position
// are sent to the web UI's map. Option "cpr": even / odd pairs up to 30 s
// apart when the aircraft's speed keeps it within 4 km of the first message
// (10 s otherwise), and the last position - moved on with the velocity - as
// the local decoding's reference for 3 minutes: positions of weak, distant
// aircraft whose messages come rarely.

#include "array.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "modes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using adsb::cd;
using adsb::Mat;
using adsb::MAXM;
using adsb::Vec;

constexpr double FS = 2.4e6;
constexpr double SPC = FS * 0.5e-6;            // samples per 0.5 us chip (1.2)
// unusual-activity events (update_outputs)
constexpr float VRATE_ALERT_FPM = 6000;
constexpr float FAST_LOW_KT = 400, FAST_LOW_FT = 10000;
constexpr int NPHASE = 5;                      // sub-sample timings, 0.2 sample apart
constexpr int NCHIP = 16 + 2 * 112;            // preamble + longest message
constexpr int NEED = static_cast<int>(NCHIP * 1.2) + 8;   // samples a candidate needs
constexpr double AIRCRAFT_TIMEOUT_S = 60;      // forgotten after this long unheard
constexpr double POS_TIMEOUT_S = 60;           // map marker kept this long after the last position
constexpr double POS_AT_MAX_S = 3;             // a packet's position (Talker::lat): moved on from at most this old a fix
// the quick gate: pulses 1-3 above this x the noise floor (sens: lower). The antennas' sum fluctuates
// far less than one antenna's magnitude, so the array's gate sits lower
constexpr float GATE = 1.6f, GATE_SENS = 1.35f, GATE_ARRAY = 1.3f, GATE_ARRAY_SENS = 1.2f;
// sens, one antenna: a preamble this much above its gaps (loose rule) when the normal test finds none
constexpr float RATIO1_SENS = 1.6f;
// array: a second message may start this far into a decoded one (separate) - the scan keeps that much more
constexpr int SPAN_MAX = NEED;
constexpr int PRE_N = 24;                      // samples a preamble covers at any timing (chips 0-15)
// array: the pulses / gaps factor on the antennas' sum before a candidate is beamformed (the sum's noise
// varies far less than one antenna's, so a weak preamble shows as a smaller factor; sens: lower)
constexpr float ARRAY_RATIO = 1.35f, ARRAY_RATIO_SENS = 1.25f;
constexpr int SIG_MIN = 3;                     // signatures an aircraft needs before its replies / repairs are checked
constexpr double SIG_RHO_MIN = 0.7;            // |a^H s|^2 a checked message must reach (random: 1 / m on average)
constexpr double LEVEL_TOL_DB = 6;             // a repaired reply's level vs the aircraft's
constexpr double PDOA_MIN_W = 12;              // clean pulse samples a message's own covariance needs
constexpr double MVDR_LOAD = 1.0;              // diagonal loading (x the noise of one antenna)
// cpr: pair / reference rules
constexpr double PAIR_MAX_MOVE_M = 4000;       // an even / odd pair at most this far apart (global decoding)
constexpr double PAIR_LONG_S = 30, PAIR_NOVEL_S = 15;   // ... and at most this long (speed known / unknown)
constexpr double REF_AGE_LONG_S = 180;         // the last position as the reference for local decoding
// the panel's aircraft table (kp::Host::table_*), one row per aircraft
const std::vector<std::string> TABLE_COLS = {"ICAO", "Callsign", "Category", "Squawk", "Altitude ft", "GNSS alt ft",
                                             "V/S ft/min", "GS kt", "Track °", "Heading °", "IAS kt", "TAS kt",
                                             "Lat", "Lon", "Distance km", "Bearing °", "Air / ground", "Emergency",
                                             "Signal dBFS", "Messages", "Position age s"};

// how a message was demodulated (the facts' counts)
enum Path { P_SUM, P_BEAM, P_DD, P_MVDR, P_INSIDE, NPATH };

// box-filter weights of one chip at one timing: samples off, off+1, off+2
struct ChipW {
    int off;
    float w[3];
};

struct Cpr {
    int lat = 0, lon = 0;
    double t = -1e9;
    bool surface = false;
};

struct Aircraft {
    uint32_t addr = 0;
    bool non_icao = false;          // DF18 anonymous / non-ICAO address
    bool confirmed = false;         // two or more messages
    double first = 0, last = 0;
    uint64_t msgs = 0;
    float rssi = -99;               // dBFS, smoothed
    std::string callsign, category;
    float alt_baro = NAN, alt_geom = NAN;   // ft
    double alt_t = -1e9;
    float gs = NAN, track = NAN, ias = NAN, tas = NAN, heading = NAN, vrate = NAN;
    double vel_t = -1e9;
    int squawk = -1, emergency = 0;
    bool ground = false;
    double lat = NAN, lon = NAN, pos_t = -1e9;
    int pos_rejects = 0;
    Cpr cpr[2];                     // even, odd
    double map_t = -1e9, table_t = -1e9;
    bool on_map = false, in_table = false, said_callsign = false, said_pos = false;
    int said_emergency = 0;
    // unusual things, reported once each (update_outputs)
    int sq_pending = -1;            // a new squawk from a reply, waiting for a second one
    double sq_pending_t = -1e9;
    int said_special = -1;          // special squawk (7400 / 7777 / 0000) reported
    bool said_vrate = false, said_fastlow = false, said_category = false;
    double spi_t = -1e9, said_spi_t = -1e9;   // IDENT (SPI): last seen, last reported
    int spi_replies = 0;
    // the array: its spatial signature (unit vector over the antennas, raw samples), learnt from the
    // messages that name it explicitly with an exact parity
    kp::cf sig[MAXM] = {};
    int sig_n = 0, sig_m = 0;
};

// a message accepted with the array: where its pulses are (DoA / signatures of overlapping ones)
struct Acc {
    double s = 0;                   // first sample (absolute, fractional)
    int len = 0;
    uint8_t msg[14] = {};
};

// a talker packet waiting for its covariance (overlapping messages are decoded right after it)
struct Pend {
    kp::Talker t;
    double s = 0;                   // absolute start
    int len = 0;
    uint8_t msg[14] = {};
};

struct Stats {
    uint64_t path[NPATH] = {};
    uint64_t rep1 = 0, rep2 = 0, rep_reply = 0, rep_df11 = 0, refused = 0;
    uint64_t pdoa = 0, pdoa_short = 0;
    uint64_t handles = 0, beam_try = 0, beam_pre = 0, dd_try = 0, mvdr_try = 0, inside_try = 0, inside_cand = 0;
};

std::string hex6(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}

const char* category_name(const std::string& c) {
    static const std::pair<const char*, const char*> n[] = {
        {"A1", "light"}, {"A2", "small"}, {"A3", "large"}, {"A4", "high vortex large"}, {"A5", "heavy"},
        {"A6", "high performance"}, {"A7", "rotorcraft"}, {"B1", "glider"}, {"B2", "lighter than air"},
        {"B3", "parachutist"}, {"B4", "ultralight"}, {"B6", "UAV"}, {"B7", "space vehicle"},
        {"C1", "emergency vehicle"}, {"C2", "service vehicle"}, {"C3", "obstacle"}};
    for (const auto& x : n)
        if (c == x.first) return x.second;
    return "";
}

const char* emergency_name(int e) {
    static const char* n[] = {"", "general emergency", "lifeguard / medical", "minimum fuel", "no communications",
                              "unlawful interference", "downed aircraft", "reserved"};
    return (e >= 0 && e <= 7) ? n[e] : "";
}

// chip c of a message (0-15 the preamble, then two per bit) carries a pulse
bool pulse(const uint8_t* msg, int c) {
    if (c < 16) return c == 0 || c == 2 || c == 7 || c == 9;
    const int i = (c - 16) >> 1;
    const bool bit = (msg[i >> 3] >> (7 - (i & 7))) & 1;
    return ((c - 16) & 1) == (bit ? 0 : 1);
}

void flip(uint8_t* msg, int b) { msg[b >> 3] ^= static_cast<uint8_t>(0x80 >> (b & 7)); }

class Adsb : public kp::Decoder {
public:
    explicit Adsb(kp::Host& h) : Decoder(h) {
        for (int ph = 0; ph < NPHASE; ph++)
            for (int c = 0; c < NCHIP; c++) {
                const double s = ph * (1.0 / NPHASE) + c * SPC, e = s + SPC;
                ChipW& w = w_[ph][c];
                w.off = static_cast<int>(std::floor(s));
                for (int q = 0; q < 3; q++) {
                    const double a = w.off + q, b = a + 1;
                    w.w[q] = static_cast<float>(std::max(0.0, std::min(e, b) - std::max(s, a)));
                }
            }
        mag_.reserve(1 << 16);
        bm_.resize(NEED + 8);
        host.table_columns(TABLE_COLS);
    }

    ~Adsb() override {
        if (!getenv("ADSB_STATS")) return;
        fprintf(stderr,
                "adsb stats: handles %llu  beam tries %llu (preamble %llu, dd %llu, mvdr %llu)  inside %llu (cand %llu)\n"
                "  accepted: sum %llu beam %llu dd %llu mvdr %llu inside %llu  repairs 1 %llu, 2+ %llu, replies %llu, "
                "df11 %llu, refused %llu  pulse doa %llu (short %llu)\n",
                ull(st_.handles), ull(st_.beam_try), ull(st_.beam_pre), ull(st_.dd_try), ull(st_.mvdr_try),
                ull(st_.inside_try), ull(st_.inside_cand), ull(st_.path[P_SUM]), ull(st_.path[P_BEAM]),
                ull(st_.path[P_DD]), ull(st_.path[P_MVDR]), ull(st_.path[P_INSIDE]), ull(st_.rep1), ull(st_.rep2),
                ull(st_.rep_reply), ull(st_.rep_df11), ull(st_.refused), ull(st_.pdoa), ull(st_.pdoa_short));
    }

    // VFO retuned / decoder restarted: kraken_doa has cleared the facts and
    // the map, so the aircraft go too
    void reset() override {
        mag_.clear();
        xs_.clear();
        pos_ = 0;
        abs0_ = 0;
        noise_ = 0;
        for (int k = 0; k < MAXM; k++) pn_[k] = 0;
        acc_.clear();
        pk_.clear();
        ac_.clear();
        host.table_columns(TABLE_COLS);
        max_range_ = 0;
        max_range_who_.clear();
        house_t_ = summary_t_ = -1e9;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "range") range_km_ = std::max(0.0, atof(value.c_str()));
        else if (key == "fix") fix_ = value.empty() ? 1 : std::clamp(atoi(value.c_str()), 0, 2);
        else if (key == "array") array_ = value != "0";
        else if (key == "separate") sep_ = value != "0";
        else if (key == "pulse_doa") pdoa_ = value != "0";
        else if (key == "cpr") cpr_ = value != "0";
        else if (key == "sens") sens_ = value == "1";
    }

    void process(const kp::cf* x, size_t n) override {
        if (m_ != 1) set_antennas(1);
        const size_t old = mag_.size();
        mag_.resize(old + n);
        for (size_t i = 0; i < n; i++) mag_[old + i] = std::sqrt(std::norm(x[i]));
        update_noise(mag_.data() + old, n);
        scan(old);
    }

    // every antenna (coherent mode, DoA on): their noise-normalised power sum is the magnitude the
    // detector sees; the samples stay for the beams
    void process_multi(const kp::cf* const* x, int m, size_t n) override {
        if (!array_ || m < 2) {
            process(x[0], n);
            return;
        }
        m = std::min(m, MAXM);
        if (m != m_) set_antennas(m);
        if (n == 0) return;
        const size_t seg = std::min<size_t>(512, n);
        if (pn_[0] <= 0) {   // the first block: the weights before its sum
            float lo[MAXM];
            for (int k = 0; k < m; k++) {
                lo[k] = -1;
                for (size_t s = 0; s + seg <= n; s += seg) {
                    float sum = 0;
                    for (size_t i = 0; i < seg; i++) sum += std::norm(x[k][s + i]);
                    if (lo[k] < 0 || sum < lo[k]) lo[k] = sum;
                }
                lo[k] /= static_cast<float>(seg);
            }
            set_noise(lo);
        }
        const size_t old = mag_.size();
        mag_.resize(old + n);
        xs_.resize((old + n) * static_cast<size_t>(m));
        kp::cf* xp = xs_.data() + old * m;
        float* mp = mag_.data() + old;
        // the sum with the weights so far; each antenna's lowest 512-sample power for the next block's
        float sum[MAXM] = {}, lo[MAXM];
        for (int k = 0; k < m; k++) lo[k] = -1;
        size_t in_seg = 0;
        for (size_t i = 0; i < n; i++) {
            float s = 0;
            for (int k = 0; k < m; k++) {
                const kp::cf v = x[k][i];
                xp[i * m + k] = v;
                const float p = std::norm(v);
                sum[k] += p;
                s += g_[k] * p;
            }
            mp[i] = std::sqrt(s);
            if (++in_seg == seg) {
                for (int k = 0; k < m; k++) {
                    if (lo[k] < 0 || sum[k] < lo[k]) lo[k] = sum[k];
                    sum[k] = 0;
                }
                in_seg = 0;
            }
        }
        for (int k = 0; k < m; k++) lo[k] /= static_cast<float>(seg);
        set_noise(lo);
        update_noise(mp, n);
        scan(old);
    }

private:
    std::array<std::array<ChipW, NCHIP>, NPHASE> w_;
    std::vector<float> mag_;
    float conf_[112] = {};
    size_t pos_ = 0;
    uint64_t abs0_ = 0;            // input sample index (since reset) of mag_[0]
    uint64_t blk_abs_ = 0;         // ... of the current block's first sample
    double cand_start_ = 0;        // the candidate being decoded: its first sample (fractional, mag_ index)
    float noise_ = 0;
    double now_ = 0;
    double range_km_ = 500;
    int fix_ = 1;                  // 0 off, 1 standard, 2 extended
    bool array_ = true, sep_ = true, pdoa_ = true, cpr_ = true, sens_ = false;
    // the array (process_multi)
    int m_ = 1;                    // antennas in the ring (1 = one channel: no samples kept)
    std::vector<kp::cf> xs_;       // the antennas' samples, sample by sample (i * m_ + k), like mag_
    float pn_[MAXM] = {};          // each antenna's noise power
    float g_[MAXM] = {};           // ... its weight in the sum (all brought to the mean noise, / m)
    float rs_[MAXM] = {};          // ... 1 / sqrt(noise power): the beams' noise-normalised samples
    std::vector<float> bm_, pm_;   // a candidate's beam magnitude; the null-space magnitude inside a message
    Vec cur_vn_;                   // the message just accepted: its signature (noise-normalised space)
    int path_ = P_SUM;
    std::vector<Acc> acc_;
    std::vector<Pend> pk_;
    Stats st_;

    std::unordered_map<uint32_t, Aircraft> ac_;
    uint64_t total_ = 0, total_rate_mark_ = 0;
    double rate_t_ = 0, rate_ = 0, house_t_ = -1e9, summary_t_ = -1e9;   // summary rows first
    double max_range_ = 0;
    std::string max_range_who_;

    static unsigned long long ull(uint64_t v) { return static_cast<unsigned long long>(v); }

    // the input switched between one channel and every antenna: the samples waiting go
    void set_antennas(int m) {
        abs0_ += mag_.size();
        mag_.clear();
        xs_.clear();
        pos_ = 0;
        m_ = m;
        noise_ = 0;
        for (int k = 0; k < MAXM; k++) pn_[k] = 0;
        acc_.clear();
    }

    // noise floor: lowest mean magnitude of 512-sample stretches (ADS-B is
    // bursty, so that is the noise between messages), smoothed
    void update_noise(const float* x, size_t n) {
        if (n == 0) return;
        const size_t seg = std::min<size_t>(512, n);
        float lo = 0;
        bool have = false;
        for (size_t s = 0; s + seg <= n; s += seg) {
            float sum = 0;
            for (size_t i = 0; i < seg; i++) sum += x[s + i];
            float mean = sum / static_cast<float>(seg);
            if (!have || mean < lo) { lo = mean; have = true; }
        }
        noise_ = noise_ <= 0 ? lo : noise_ + 0.05f * (lo - noise_);
    }

    // the same per antenna, in power (lo: each antenna's lowest 512-sample mean of a block), smoothed:
    // the weights that bring every antenna to one noise level
    void set_noise(const float* lo) {
        double mean = 0;
        for (int k = 0; k < m_; k++) {
            const float v = std::max(lo[k], 1e-12f);
            pn_[k] = pn_[k] <= 0 ? v : pn_[k] + 0.05f * (v - pn_[k]);
            mean += pn_[k];
        }
        mean /= m_;
        for (int k = 0; k < m_; k++) {
            g_[k] = static_cast<float>(mean / pn_[k] / m_);
            rs_[k] = 1.0f / std::sqrt(pn_[k]);
        }
    }

    float gate_k() const { return m_ > 1 ? (sens_ ? GATE_ARRAY_SENS : GATE_ARRAY) : (sens_ ? GATE_SENS : GATE); }
    float array_ratio() const { return sens_ ? ARRAY_RATIO_SENS : ARRAY_RATIO; }

    double mean_pn() const {
        double s = 0;
        for (int k = 0; k < m_; k++) s += pn_[k];
        return s / std::max(1, m_);
    }

    void scan(size_t old) {
        now_ = host.time();
        blk_abs_ = abs0_ + old;   // the block's first sample (its time is now_)
        const float gate = gate_k() * noise_;
        const float* m = mag_.data();
        const size_t need = NEED + (m_ > 1 && sep_ ? SPAN_MAX : 0);
        const size_t end = mag_.size() > need ? mag_.size() - need : 0;
        size_t j = pos_;
        while (j < end) {
            const float* p = m + j;
            // quick gate: pulses 1, 2 and 3 above the noise, the gap between
            // pulses 2 and 3 (samples 5..7 at any timing) quieter than both
            const float p1 = std::max(p[0], p[1]), p2 = std::max(p[2], p[3]);
            if (p1 < gate || p2 < gate || std::max(std::max(p[8], p[9]), p[10]) < gate) { j++; continue; }
            const float q = std::max(std::max(p[5], p[6]), p[7]);
            if (q >= p1 || q >= p2) { j++; continue; }
            int len = try_decode(j);
            flush_packets();
            j += len > 0 ? static_cast<size_t>((16 + 2 * len) * SPC) : 1;
        }
        // keep the tail a candidate still needs
        const size_t drop = std::min(j, end);
        mag_.erase(mag_.begin(), mag_.begin() + static_cast<long>(drop));
        if (m_ > 1) xs_.erase(xs_.begin(), xs_.begin() + static_cast<long>(drop * m_));
        abs0_ += drop;
        pos_ = j - drop;
        // accepted messages far behind can't overlap a new one
        const double old_s = static_cast<double>(abs0_) + static_cast<double>(pos_) - 2.0 * SPAN_MAX;
        acc_.erase(std::remove_if(acc_.begin(), acc_.end(), [&](const Acc& e) { return e.s < old_s; }), acc_.end());

        housekeeping();
    }

    float chip(const float* p, int ph, int c) const {
        const ChipW& w = w_[ph][c];
        const float* q = p + w.off;
        return w.w[0] * q[0] + w.w[1] * q[1] + w.w[2] * q[2];
    }

    struct Cand {
        int ph;
        float score, hi;
    };

    // the preamble at the five timings of magnitude p: the timings whose pulses are >= ratio x the
    // gaps and no gap as strong as a pulse (loose: as the pulses' mean - the antennas' sum before a
    // beam, where the strongest of twelve gaps often passes the weakest of four weak pulses), best
    // first; returns how many
    int preamble(const float* p, float ratio, Cand* cand, bool loose = false) const {
        int nc = 0;
        for (int ph = 0; ph < NPHASE; ph++) {
            const float h0 = chip(p, ph, 0), h2 = chip(p, ph, 2), h7 = chip(p, ph, 7), h9 = chip(p, ph, 9);
            const float hi = (h0 + h2 + h7 + h9) * 0.25f;
            float lo = 0, lmax = 0;
            for (int c : {1, 3, 4, 5, 6, 8, 10, 11, 12, 13, 14, 15}) {
                float v = chip(p, ph, c);
                lo += v;
                lmax = std::max(lmax, v);
            }
            lo /= 12;
            if (hi < ratio * lo || lmax >= (loose ? hi : std::min(std::min(h0, h2), std::min(h7, h9)))) continue;
            cand[nc++] = {ph, hi - lo, hi};
        }
        for (int k = 1; k < nc; k++)
            for (int i = k; i > 0 && cand[i].score > cand[i - 1].score; i--) std::swap(cand[i], cand[i - 1]);
        return nc;
    }

    // Returns the message length in bits if a valid message starts at j, else 0
    int try_decode(size_t j) {
        const float* p = mag_.data() + j;
        Cand c[NPHASE];
        const int nc = preamble(p, 2.0f, c);   // ~6 dB between the pulses and the gaps
        const int tries = sens_ ? nc : std::min(nc, 2);
        for (int k = 0; k < tries; k++)
            if (int len = decode_on(p, c[k].ph, j, c[k].hi / static_cast<float>(SPC), P_SUM)) {
                after_decode(j, len);
                return len;
            }
        if (m_ < 2) {
            // one antenna, sens: a weaker preamble too
            if (!sens_ || nc) return 0;
            Cand r[NPHASE];
            const int nr = preamble(p, RATIO1_SENS, r, true);
            for (int k = 0; k < std::min(nr, 2); k++)
                if (int len = decode_on(p, r[k].ph, j, r[k].hi / static_cast<float>(SPC), P_SUM)) return len;
            return 0;
        }
        // the array: beamform what the antennas' sum didn't decode
        Cand r[NPHASE];
        const int nr = nc ? 0 : preamble(p, array_ratio(), r, true);
        if (!nc && !nr) return 0;
        if (int len = decode_beams(j, nc ? c[0] : r[0])) {
            after_decode(j, len);
            return len;
        }
        return 0;
    }

    void after_decode(size_t j, int len) {
        if (m_ >= 2 && sep_) scan_inside(j, len);
    }

    // slices the message at timing ph of magnitude p (a candidate at mag_ index j) and hands it on;
    // the message length if it was accepted
    int decode_on(const float* p, int ph, size_t j, float level, int path) {
        uint8_t msg[14];
        const int len = slice(p, ph, msg, conf_);
        if (!len) return 0;
        cand_start_ = static_cast<double>(j) + ph * (1.0 / NPHASE);
        path_ = path;
        return handle(msg, len, level) ? len : 0;
    }

    // conf[i]: |first half - second half| of bit i (how certain it is)
    int slice(const float* p, int ph, uint8_t* msg, float* conf) const {
        auto bit = [&](int i) {
            const float d = chip(p, ph, 16 + 2 * i) - chip(p, ph, 17 + 2 * i);
            conf[i] = std::fabs(d);
            return d > 0;
        };
        int df = 0;
        for (int i = 0; i < 5; i++) df = (df << 1) | bit(i);
        int len;
        switch (df) {
            case 0: case 4: case 5: case 11: len = 56; break;
            case 16: case 17: case 18: case 20: case 21: len = 112; break;
            default: return 0;
        }
        memset(msg, 0, 14);
        for (int i = 0; i < len; i++)
            if (bit(i)) msg[i >> 3] |= static_cast<uint8_t>(0x80 >> (i & 7));
        return len;
    }

    // --- the array ----------------------------------------------------------
    // sample i of the ring: raw, or noise-normalised (x~), projected off unit vector proj
    void sample(size_t i, bool raw, const Vec* proj, cd* y) const {
        const kp::cf* x = xs_.data() + i * m_;
        for (int k = 0; k < m_; k++) y[k] = raw ? cd(x[k]) : cd(x[k]) * static_cast<double>(rs_[k]);
        if (proj) {
            cd d = 0;
            for (int k = 0; k < m_; k++) d += std::conj(proj->v[k]) * y[k];
            for (int k = 0; k < m_; k++) y[k] -= proj->v[k] * d;
        }
    }

    // |w^H x~| of the samples j + from .. j + to of the ring into out[from .. to)
    void beam(size_t j, const Vec& w, int from, int to, float* out) const {
        kp::cf wc[MAXM];
        for (int k = 0; k < m_; k++) wc[k] = kp::cf(std::conj(w.v[k]) * static_cast<double>(rs_[k]));
        const kp::cf* x = xs_.data() + j * m_;
        for (int i = from; i < to; i++) {
            kp::cf y = 0;
            for (int k = 0; k < m_; k++) y += wc[k] * x[i * m_ + k];
            out[i] = std::sqrt(std::norm(y));
        }
    }

    // does another accepted message have a pulse in (or within half a sample of) absolute sample ia?
    bool dirty(double ia, double self) const {
        for (const Acc& e : acc_) {
            if (std::fabs(e.s - self) < 0.5) continue;
            const double rel = ia - e.s;
            const int nchip = 16 + 2 * e.len;
            if (rel < -1.5 || rel > nchip * SPC + 0.5) continue;
            const int c0 = static_cast<int>(std::floor((rel - 0.5) / SPC));
            const int c1 = static_cast<int>(std::floor((rel + 1.5) / SPC));
            for (int c = std::max(0, c0); c <= c1 && c < nchip; c++)
                if (pulse(e.msg, c)) return true;
        }
        return false;
    }

    // covariance of the samples of a message's pulses (on) and of its gaps (off) for a start at s (mag_
    // index), each sample weighted by how much of it lies in the chip; len 0 = only the preamble.
    // Samples another accepted message has a pulse in are left out.
    void msg_cov(double s, const uint8_t* msg, int len, bool raw, const Vec* proj, Mat* on, double* won, Mat* off,
                 double* woff) const {
        on->zero(m_);
        *won = 0;
        if (off) {
            off->zero(m_);
            *woff = 0;
        }
        const double self = static_cast<double>(abs0_) + s;
        const bool check = !acc_.empty();
        cd y[MAXM];
        const int nchip = 16 + 2 * len;
        for (int c = 0; c < nchip; c++) {
            const bool p = pulse(msg, c);
            if (!p && !off) continue;
            const double a = s + c * SPC, b = a + SPC;
            for (long i = static_cast<long>(std::floor(a)); i < b; i++) {
                const double w = std::min(b, i + 1.0) - std::max(a, static_cast<double>(i));
                if (w < 0.25 || i < 0 || static_cast<size_t>(i) >= mag_.size()) continue;
                if (check && dirty(static_cast<double>(abs0_) + static_cast<double>(i), self)) continue;
                sample(static_cast<size_t>(i), raw, proj, y);
                if (p) {
                    adsb::add_outer(*on, y, w);
                    *won += w;
                } else {
                    adsb::add_outer(*off, y, w);
                    *woff += w;
                }
            }
        }
        on->mirror();
        if (off) off->mirror();
    }

    // a candidate the antennas' sum didn't decode: beams from its own pulses
    int decode_beams(size_t j, const Cand& c0) {
        st_.beam_try++;
        const float level = c0.hi / static_cast<float>(SPC);
        float* b = bm_.data();
        // maximum-ratio weights from the preamble pulses
        Mat R, Roff;
        double w, woff;
        msg_cov(static_cast<double>(j) + c0.ph * (1.0 / NPHASE), nullptr, 0, false, nullptr, &R, &w, nullptr, nullptr);
        if (!(w > 0)) return 0;
        Vec v = adsb::principal(R);
        beam(j, v, 0, PRE_N, b);   // the preamble first: most candidates end here
        Cand c[NPHASE];
        int nc = preamble(b, 2.0f, c);
        if (!nc) return 0;
        st_.beam_pre++;
        beam(j, v, PRE_N, NEED, b);
        const int tries = sens_ ? nc : std::min(nc, 2);
        for (int k = 0; k < tries; k++)
            if (int len = decode_on(b, c[k].ph, j, level, P_BEAM)) return len;
        // decision-directed: every pulse of the message as sliced
        uint8_t msg[14];
        float conf[112];
        const int len = slice(b, c[0].ph, msg, conf);
        if (!len) return 0;
        st_.dd_try++;
        const double s1 = static_cast<double>(j) + c[0].ph * (1.0 / NPHASE);
        msg_cov(s1, msg, len, false, nullptr, &R, &w, &Roff, &woff);
        if (!(w > 0)) return 0;
        const Vec v2 = adsb::principal(R);
        beam(j, v2, 0, NEED, b);
        nc = preamble(b, 2.0f, c);
        for (int k = 0; k < std::min(nc, 2); k++)
            if (int l2 = decode_on(b, c[k].ph, j, level, P_DD)) return l2;
        if (!sep_ || !(woff > 0)) return 0;
        // MVDR: aimed at the message's pulses minus its gaps (an overlapping message shows in both), the
        // window's covariance inverted - whatever else is there from another direction is nulled
        st_.mvdr_try++;
        Mat D;
        D.zero(m_);
        for (int i = 0; i < m_; i++)
            for (int k = 0; k < m_; k++) D.a[i][k] = R.a[i][k] / w - Roff.a[i][k] / woff;
        const Vec a = adsb::principal(D, false);
        Mat Rw;
        Rw.zero(m_);
        cd y[MAXM];
        const double e1 = s1 + (16 + 2 * len) * SPC;
        double nw = 0;
        for (size_t i = static_cast<size_t>(s1); i < e1 && i < mag_.size(); i++) {
            sample(i, false, nullptr, y);
            adsb::add_outer(Rw, y, 1.0);
            nw += 1;
        }
        Rw.mirror();
        for (int i = 0; i < m_; i++)
            for (int k = 0; k < m_; k++) Rw.a[i][k] /= nw;
        for (int i = 0; i < m_; i++) Rw.a[i][i] += MVDR_LOAD;
        Vec wv;
        if (!adsb::solve(Rw, a, &wv) || !adsb::normalize(wv)) return 0;
        beam(j, wv, 0, NEED, b);
        nc = preamble(b, 2.0f, c);
        for (int k = 0; k < std::min(nc, 2); k++)
            if (int l2 = decode_on(b, c[k].ph, j, level, P_MVDR)) return l2;
        return 0;
    }

    // a second message starting inside the one just decoded (at mag_ index j): the first one's
    // signature projected out of every antenna, the rest scanned like the input
    void scan_inside(size_t j, int len) {
        const Vec v = cur_vn_;
        if (v.m != m_) return;
        st_.inside_try++;
        const size_t span = static_cast<size_t>((16 + 2 * len) * SPC);
        const size_t i0 = j + 4, i1 = j + span;
        const size_t n = i1 - i0 + NEED;
        if (i0 + n > mag_.size()) return;
        pm_.resize(n);
        const float sc = static_cast<float>(std::sqrt(mean_pn() / (m_ - 1)));
        kp::cf vc[MAXM];
        for (int k = 0; k < m_; k++) vc[k] = kp::cf(std::conj(v.v[k]) * static_cast<double>(rs_[k]));
        for (size_t t = 0; t < n; t++) {
            const kp::cf* x = xs_.data() + (i0 + t) * m_;
            float e = 0;
            kp::cf d = 0;
            for (int k = 0; k < m_; k++) {
                e += std::norm(x[k]) * rs_[k] * rs_[k];
                d += vc[k] * x[k];
            }
            pm_[t] = std::sqrt(std::max(0.0f, e - std::norm(d))) * sc;
        }
        const float gate = gate_k() * noise_;
        for (size_t t = 0; i0 + t < i1; t++) {
            const float* p = pm_.data() + t;
            const float p1 = std::max(p[0], p[1]), p2 = std::max(p[2], p[3]);
            if (p1 < gate || p2 < gate || std::max(std::max(p[8], p[9]), p[10]) < gate) continue;
            const float q = std::max(std::max(p[5], p[6]), p[7]);
            if (q >= p1 || q >= p2) continue;
            Cand c[NPHASE];
            const int nc = preamble(p, array_ratio(), c, true);
            if (!nc) continue;
            st_.inside_cand++;
            if (decode_null(i0 + t, c[0], v)) return;
        }
    }

    // a candidate at mag_ index j with the signature vA projected out: weights from its preamble in the
    // null space, then from all its pulses
    bool decode_null(size_t j, const Cand& c0, const Vec& vA) {
        const float level = c0.hi / static_cast<float>(SPC);
        float* b = bm_.data();
        Mat R;
        double w;
        msg_cov(static_cast<double>(j) + c0.ph * (1.0 / NPHASE), nullptr, 0, false, &vA, &R, &w, nullptr, nullptr);
        if (!(w > 0)) return false;
        auto in_null = [&](Vec u) {   // u with vA's part removed, unit length
            const cd d = adsb::dot(vA, u);
            for (int k = 0; k < m_; k++) u.v[k] -= vA.v[k] * d;
            return adsb::normalize(u) ? u : Vec{};
        };
        Vec wv = in_null(adsb::principal(R));
        if (wv.m != m_) return false;
        beam(j, wv, 0, PRE_N, b);
        Cand c[NPHASE];
        int nc = preamble(b, 2.0f, c);
        if (!nc) return false;
        beam(j, wv, PRE_N, NEED, b);
        for (int k = 0; k < std::min(nc, 2); k++)
            if (decode_on(b, c[k].ph, j, level, P_INSIDE)) return true;
        uint8_t msg[14];
        float conf[112];
        const int len = slice(b, c[0].ph, msg, conf);
        if (!len) return false;
        msg_cov(static_cast<double>(j) + c[0].ph * (1.0 / NPHASE), msg, len, false, &vA, &R, &w, nullptr, nullptr);
        if (!(w > 0)) return false;
        wv = in_null(adsb::principal(R));
        if (wv.m != m_) return false;
        beam(j, wv, 0, NEED, b);
        nc = preamble(b, 2.0f, c);
        for (int k = 0; k < std::min(nc, 2); k++)
            if (decode_on(b, c[k].ph, j, level, P_INSIDE)) return true;
        return false;
    }

    // the candidate's spatial signature from its own pulses: vn = the beam's weights (noise-normalised
    // space), sr = the same over the raw antennas (unit length) - what the aircraft's signature is in
    bool msg_sig(const uint8_t* msg, int len, Vec* vn, Vec* sr) const {
        Mat R;
        double w;
        msg_cov(cand_start_, msg, len, false, nullptr, &R, &w, nullptr, nullptr);
        if (w < PDOA_MIN_W) return false;
        *vn = adsb::principal(R);
        sr->m = m_;
        for (int k = 0; k < m_; k++) sr->v[k] = vn->v[k] * std::sqrt(static_cast<double>(pn_[k]));
        return adsb::normalize(*sr);
    }

    static double sig_match(const Aircraft& a, const Vec& sr) {
        cd d = 0;
        for (int k = 0; k < sr.m; k++) d += std::conj(cd(a.sig[k])) * sr.v[k];
        return std::norm(d);
    }

    void sig_update(Aircraft& a, const Vec& sr) const {
        if (a.sig_m != sr.m) {
            for (int k = 0; k < sr.m; k++) a.sig[k] = kp::cf(sr.v[k]);
            a.sig_m = sr.m;
            a.sig_n = 1;
            return;
        }
        cd d = 0;
        for (int k = 0; k < sr.m; k++) d += std::conj(cd(a.sig[k])) * sr.v[k];
        const cd ph = std::abs(d) > 0 ? std::conj(d) / std::abs(d) : cd(1);   // sr turned to a's phase
        const double mu = std::max(0.2, 1.0 / (a.sig_n + 1));
        Vec u;
        u.m = sr.m;
        for (int k = 0; k < sr.m; k++) u.v[k] = (1 - mu) * cd(a.sig[k]) + mu * sr.v[k] * ph;
        if (!adsb::normalize(u)) return;
        for (int k = 0; k < sr.m; k++) a.sig[k] = kp::cf(u.v[k]);
        a.sig_n++;
    }

    // the same message accepted a moment ago (a beam / the null-space scan found it again)?
    bool duplicate(const uint8_t* msg, int len) const {
        const double self = static_cast<double>(abs0_) + cand_start_;
        for (const Acc& e : acc_)
            if (e.len == len && std::fabs(e.s - self) < 3 && memcmp(e.msg, msg, len / 8) == 0) return true;
        return false;
    }

    // --- message level ----------------------------------------------------
    Aircraft* find(uint32_t a) {
        auto it = ac_.find(a);
        return it == ac_.end() ? nullptr : &it->second;
    }

    Aircraft& get(uint32_t a, bool non_icao) {
        auto it = ac_.find(a);
        if (it != ac_.end()) return it->second;
        Aircraft& n = ac_[a];
        n.addr = a;
        n.non_icao = non_icao;
        n.first = now_;
        return n;
    }

    bool level_ok(const Aircraft& a, float level_db) const {
        return a.msgs >= 2 && std::fabs(level_db - a.rssi) <= LEVEL_TOL_DB;
    }

    // CRC check + dispatch. true if the message was accepted
    bool handle(uint8_t* msg, int len, float level) {
        st_.handles++;
        const int df = msg[0] >> 3;
        const uint32_t syn = modes::syndrome(msg, len);
        const float level_db = 20.0f * std::log10(std::max(level, 1e-6f));
        Aircraft* a = nullptr;
        int fixed = 0;                 // bits repaired
        bool clean = false;            // explicit address with an exact parity: the signature learns from it
        bool weak_id = false;          // not proven by an exact parity: the signature must agree (array)
        bool reply = false;
        if (df == 17 || df == 18) {
            if (syn != 0) {
                if (!fix_ || !(fixed = repair_es(msg, syn))) return false;
            }
            const uint32_t addr = modes::bits(msg, 9, 32);
            const int cf = msg[0] & 7;
            if (df == 18 && cf != 0 && cf != 1 && cf != 6) return false;   // TIS-B / reserved
            a = fixed ? find(addr) : &get(addr, df == 18 && cf == 1);
            if (!a) return false;
            clean = !fixed;
            weak_id = fixed > 0;
        } else if (df == 11) {
            if (syn == 0) {
                a = &get(modes::bits(msg, 9, 32), false);
                clean = true;
            } else if ((syn & 0xFFFF80) == 0) {
                a = find(modes::bits(msg, 9, 32));   // interrogator id in the parity
                weak_id = true;
            } else if (fix_ >= 2 && (fixed = repair_df11(msg, syn)) > 0) {
                a = find(modes::bits(msg, 9, 32));
                weak_id = true;
            }
            if (!a) return false;
        } else {
            // address / parity: the syndrome is the address
            reply = true;
            a = find(syn);
            if (!a && fix_ >= 2) a = repair_reply(msg, len, syn, level_db, &fixed);
            if (!a) return false;
            weak_id = fixed > 0;
        }
        // the array: the message's own spatial signature - the aircraft's, unless the parity proved it
        Vec vn, sr;
        bool have_sig = false;
        if (m_ >= 2) {
            if (duplicate(msg, len)) return false;
            have_sig = msg_sig(msg, len, &vn, &sr);
            if (weak_id && have_sig && a->sig_n >= SIG_MIN && a->sig_m == m_ && sig_match(*a, sr) < SIG_RHO_MIN) {
                st_.refused++;
                return false;
            }
        }
        accept(*a, level);
        st_.path[path_]++;
        if (fixed) {
            (fixed == 1 ? st_.rep1 : st_.rep2)++;
            if (reply) st_.rep_reply++;
            if (df == 11) st_.rep_df11++;
        }
        if (m_ >= 2) {
            if (have_sig) {
                if (clean) sig_update(*a, sr);
                cur_vn_ = vn;
            } else {
                cur_vn_ = Vec{};
            }
            Acc e;
            e.s = static_cast<double>(abs0_) + cand_start_;
            e.len = len;
            memcpy(e.msg, msg, len / 8);
            acc_.push_back(e);
        }
        if (a->confirmed) queue_packet(*a, msg, len);
        if (df == 17 || df == 18) extended_squitter(*a, msg, df);
        else if (df == 4 || df == 20 || df == 0 || df == 16) reply_altitude(*a, msg, df);
        else if (df == 5 || df == 21) reply_identity(*a, msg);
        if ((df == 4 || df == 5 || df == 20 || df == 21) && ((msg[0] & 7) == 4 || (msg[0] & 7) == 5))
            note_spi(*a, false);   // flight status 4 / 5 = SPI
        else if (df == 11) {
            const int ca = msg[0] & 7;
            if (ca == 4) a->ground = true;
            else if (ca == 5) a->ground = false;
        }
        if (host.verbose()) host.event(describe(*a, msg, len, df, fixed), 0.0);
        if (host.raw_wanted()) {
            // the decoder data log: the message in hex + its level (dBFS)
            std::string h;
            h += hex_bytes(msg, len / 8);
            host.raw(h + fmt(" %.1f", level_db));
        }
        if (a->confirmed) update_outputs(*a);
        return true;
    }

    // the n least certain bits of a len-bit message (never the DF field) into out, the least first
    int weakest(int len, int n, int* out) const {
        int idx[112];
        const int m = len - 5;
        for (int i = 0; i < m; i++) idx[i] = i + 5;
        n = std::min(n, m);
        std::partial_sort(idx, idx + n, idx + m, [&](int x, int y) { return conf_[x] < conf_[y]; });
        for (int i = 0; i < n; i++) out[i] = idx[i];
        return n;
    }

    // up to maxb of the bits weak[0..nw) whose syndromes explain syn (only the bits in mask: DF11's
    // interrogator code may stay in the others); flips them, returns how many (0 = none)
    static int search(uint8_t* msg, int len, uint32_t syn, uint32_t mask, const int* weak, int nw, int maxb) {
        uint32_t s[16];
        for (int i = 0; i < nw; i++) s[i] = modes::bit_syndrome(weak[i] + 112 - len);
        for (int x = 0; x < nw; x++)
            if (((syn ^ s[x]) & mask) == 0) {
                flip(msg, weak[x]);
                return 1;
            }
        if (maxb >= 2)
            for (int x = 0; x < nw; x++)
                for (int y = x + 1; y < nw; y++)
                    if (((syn ^ s[x] ^ s[y]) & mask) == 0) {
                        flip(msg, weak[x]);
                        flip(msg, weak[y]);
                        return 2;
                    }
        if (maxb >= 3)
            for (int x = 0; x < nw; x++)
                for (int y = x + 1; y < nw; y++) {
                    const uint32_t sxy = syn ^ s[x] ^ s[y];
                    for (int z = y + 1; z < nw; z++)
                        if (((sxy ^ s[z]) & mask) == 0) {
                            flip(msg, weak[x]);
                            flip(msg, weak[y]);
                            flip(msg, weak[z]);
                            return 3;
                        }
                }
        return 0;
    }

    // DF17/18: one bad bit anywhere (syndrome table), or two among the eight least certain bits
    // (extended: up to three among the twelve least certain); never in the DF field
    int repair_es(uint8_t* msg, uint32_t syn) const {
        const int b = modes::single_bit_error(syn);
        if (b >= 5) {
            flip(msg, b);
            return 1;
        }
        if (b >= 0) return 0;
        int weak[12];
        const int nw = weakest(112, fix_ >= 2 ? 12 : 8, weak);
        return search(msg, 112, syn, 0xFFFFFF, weak, nw, fix_ >= 2 ? 3 : 2);
    }

    // DF11 (extended): one bad bit anywhere with interrogator code 0, or one / two of the eight least
    // certain with any code - the address in the message must be an aircraft heard before
    int repair_df11(uint8_t* msg, uint32_t syn) const {
        const int b = modes::single_bit_error(syn);   // the 112-bit table: a 56-bit message's bit b sits at b + 56
        if (b >= 56 + 5) {
            flip(msg, b - 56);
            return 1;
        }
        int weak[8];
        const int nw = weakest(56, 8, weak);
        return search(msg, 56, syn, 0xFFFF80, weak, nw, 2);
    }

    // what a reply says agrees with what is known of the aircraft: an altitude reply within 1000 ft of
    // its altitude (known in the last 2 min), an identity reply its squawk - nothing known, no repair
    bool reply_consistent(const Aircraft& a, const uint8_t* msg) const {
        const int df = msg[0] >> 3;
        if (df == 4 || df == 20 || df == 0 || df == 16) {
            const float alt = modes::ac13_altitude_ft(modes::bits(msg, 20, 32));
            return std::isfinite(alt) && std::isfinite(a.alt_baro) && now_ - a.alt_t < 120 &&
                   std::fabs(alt - a.alt_baro) <= 1000;
        }
        const uint32_t id = modes::bits(msg, 20, 32);
        return a.squawk >= 0 && id && modes::squawk(id) == a.squawk;
    }

    // An address / parity reply (extended): an aircraft heard before whose address explains the
    // syndrome with one or two of the eight least certain bits, at its usual level and saying what
    // is known of it
    Aircraft* repair_reply(uint8_t* msg, int len, uint32_t syn, float level_db, int* fixed) {
        int weak[8];
        const int nw = weakest(len, 8, weak);
        uint32_t s[8];
        for (int i = 0; i < nw; i++) s[i] = modes::bit_syndrome(weak[i] + 112 - len);
        uint8_t fx[14];
        const int nb = len / 8;
        auto ok = [&](uint32_t addr, int b1, int b2) -> Aircraft* {
            Aircraft* a = find(addr);
            if (!a || !a->confirmed || !level_ok(*a, level_db)) return nullptr;
            memcpy(fx, msg, nb);
            flip(fx, b1);
            if (b2 >= 0) flip(fx, b2);
            return reply_consistent(*a, fx) ? a : nullptr;
        };
        for (int x = 0; x < nw; x++)
            if (Aircraft* a = ok(syn ^ s[x], weak[x], -1)) {
                flip(msg, weak[x]);
                *fixed = 1;
                return a;
            }
        for (int x = 0; x < nw; x++)
            for (int y = x + 1; y < nw; y++)
                if (Aircraft* a = ok(syn ^ s[x] ^ s[y], weak[x], weak[y])) {
                    flip(msg, weak[x]);
                    flip(msg, weak[y]);
                    *fixed = 2;
                    return a;
                }
        return nullptr;
    }

    // This message's samples belong to aircraft a (kp::Talker::packet): the
    // DoA per aircraft. Preamble (8 us) + len bits of 1 us from the first
    // preamble pulse. Queued: a message overlapping it may still be decoded
    // (flush_packets, right after).
    void queue_packet(const Aircraft& a, const uint8_t* msg, int len) {
        Pend p;
        kp::Talker& t = p.t;
        t.id = id_of(a);
        t.label = a.callsign;
        if (std::isfinite(a.alt_baro) && !a.ground) t.label += (t.label.empty() ? "" : " · ") + fmt("%.0f ft", a.alt_baro);
        t.start_s = now_ + (static_cast<double>(abs0_) + cand_start_ - static_cast<double>(blk_abs_)) / FS;
        t.end_s = t.start_s + (8 + len) * 1e-6;
        t.packet = true;
        packet_position(a, t.start_s, &t);
        p.s = static_cast<double>(abs0_) + cand_start_;
        p.len = len;
        memcpy(p.msg, msg, len / 8);
        pk_.push_back(std::move(p));
    }

    // the queued talker packets go out - with the array (pulse_doa) each with the covariance of its
    // own pulse samples, leaving out those another decoded message has a pulse in
    void flush_packets() {
        for (Pend& p : pk_) {
            if (m_ >= 2 && pdoa_) {
                Mat R;
                double w;
                msg_cov(p.s - static_cast<double>(abs0_), p.msg, p.len, true, nullptr, &R, &w, nullptr, nullptr);
                if (w < PDOA_MIN_W) {
                    st_.pdoa_short++;
                    continue;
                }
                p.t.m = m_;
                p.t.cov.resize(static_cast<size_t>(m_ * m_));
                for (int i = 0; i < m_; i++)
                    for (int k = 0; k < m_; k++) p.t.cov[static_cast<size_t>(i * m_ + k)] = kp::cf(R.a[i][k] / w);
                st_.pdoa++;
            }
            host.talker(p.t);
        }
        pk_.clear();
    }

    // Where the aircraft was at time ts (its last position moved on with its
    // velocity) - for the array calibration against ADS-B bearings
    // (kp::Talker::lat / lon / alt_m). Airborne only (taxiing aircraft sit in
    // the ground clutter and reflections), a position at most POS_AT_MAX_S
    // old, a velocity for anything older than 0.1 s.
    void packet_position(const Aircraft& a, double ts, kp::Talker* t) const {
        if (a.ground || !std::isfinite(a.lat) || !std::isfinite(a.lon)) return;
        const double dt = ts - a.pos_t;
        if (dt < -0.05 || dt > POS_AT_MAX_S) return;
        float alt_ft = a.alt_geom;
        if (!std::isfinite(alt_ft)) alt_ft = now_ - a.alt_t < 30 ? a.alt_baro : NAN;
        if (!std::isfinite(alt_ft)) return;
        double lat = a.lat, lon = a.lon, alt_m = alt_ft * 0.3048;
        if (dt > 0.1) {
            if (!std::isfinite(a.gs) || !std::isfinite(a.track) || now_ - a.vel_t > 30) return;
            const double d = a.gs * 0.514444 * dt, tr = a.track * M_PI / 180;
            lat += d * std::cos(tr) / 111320.0;
            lon += d * std::sin(tr) / (111320.0 * std::max(0.01, std::cos(a.lat * M_PI / 180)));
            if (std::isfinite(a.vrate)) alt_m += a.vrate * 0.3048 / 60 * dt;
        }
        t->lat = lat;
        t->lon = lon;
        t->alt_m = alt_m;
    }

    void accept(Aircraft& a, float level) {
        a.last = now_;
        a.msgs++;
        total_++;
        const float db = 20.0f * std::log10(std::max(level, 1e-6f));
        a.rssi = a.msgs == 1 ? db : a.rssi + 0.2f * (db - a.rssi);
        if (!a.confirmed && a.msgs >= 2) {
            a.confirmed = true;
            host.event("New aircraft " + std::string(a.non_icao ? "~" : "") + hex6(a.addr), 5.0);
        }
        if (a.confirmed) host.valid();
    }

    void extended_squitter(Aircraft& a, const uint8_t* msg, int df) {
        const uint8_t* me = msg + 4;
        const int tc = static_cast<int>(modes::bits(me, 1, 5));
        if (df == 17) {
            const int ca = msg[0] & 7;
            if (ca == 4) a.ground = true;
            else if (ca == 5) a.ground = false;
        }
        if (tc >= 1 && tc <= 4) {
            static const char set[] = {'?', 'D', 'C', 'B', 'A'};
            const int cat = static_cast<int>(modes::bits(me, 6, 8));
            std::string cs = modes::callsign(me);
            if (cs.find('#') == std::string::npos && !cs.empty()) a.callsign = cs;
            if (cat) a.category = std::string(1, set[tc]) + std::to_string(cat);
        } else if ((tc >= 5 && tc <= 8) || (tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            const bool surface = tc <= 8;
            if (surface) {
                a.ground = true;
                const float gs = modes::surface_speed_kt(static_cast<int>(modes::bits(me, 6, 12)));
                if (std::isfinite(gs)) a.gs = gs;
                if (modes::bits(me, 13, 13)) a.track = modes::bits(me, 14, 20) * 360.0f / 128.0f;
                a.vel_t = now_;
            } else {
                a.ground = false;
                const uint32_t ac = modes::bits(me, 9, 20);
                if (tc <= 18) {
                    float alt = modes::ac12_altitude_ft(ac);
                    if (std::isfinite(alt)) { a.alt_baro = alt; a.alt_t = now_; }
                } else if (ac) {
                    a.alt_geom = static_cast<float>(ac * 3.28084);   // GNSS height, metres
                }
            }
            if (!surface && modes::bits(me, 6, 7) == 3) note_spi(a, true);   // surveillance status 3 = SPI
            const int odd = static_cast<int>(modes::bits(me, 22, 22));
            Cpr& c = a.cpr[odd];
            c.lat = static_cast<int>(modes::bits(me, 23, 39));
            c.lon = static_cast<int>(modes::bits(me, 40, 56));
            c.t = now_;
            c.surface = surface;
            position(a, odd != 0);
        } else if (tc == 19) {
            velocity(a, me);
        } else if (tc == 28) {
            if (modes::bits(me, 6, 8) == 1) {
                a.emergency = static_cast<int>(modes::bits(me, 9, 11));
                const uint32_t id = modes::bits(me, 12, 24);
                if (id) set_squawk(a, modes::squawk(id), true);
            }
        }
    }

    void velocity(Aircraft& a, const uint8_t* me) {
        const int st = static_cast<int>(modes::bits(me, 6, 8));
        if (st == 1 || st == 2) {
            const int mult = st == 2 ? 4 : 1;
            const int vew = static_cast<int>(modes::bits(me, 15, 24)), vns = static_cast<int>(modes::bits(me, 26, 35));
            if (vew && vns) {
                float ve = static_cast<float>((vew - 1) * mult), vn = static_cast<float>((vns - 1) * mult);
                if (modes::bits(me, 14, 14)) ve = -ve;
                if (modes::bits(me, 25, 25)) vn = -vn;
                a.gs = std::hypot(ve, vn);
                float trk = std::atan2(ve, vn) * 180.0f / static_cast<float>(M_PI);
                a.track = trk < 0 ? trk + 360.0f : trk;
            }
        } else if (st == 3 || st == 4) {
            const int mult = st == 4 ? 4 : 1;
            if (modes::bits(me, 14, 14)) a.heading = modes::bits(me, 15, 24) * 360.0f / 1024.0f;
            const int as = static_cast<int>(modes::bits(me, 26, 35));
            if (as) {
                float v = static_cast<float>((as - 1) * mult);
                if (modes::bits(me, 25, 25)) a.tas = v;
                else a.ias = v;
            }
        } else {
            return;
        }
        const int vr = static_cast<int>(modes::bits(me, 38, 46));
        if (vr) a.vrate = static_cast<float>((vr - 1) * 64 * (modes::bits(me, 37, 37) ? -1 : 1));
        a.vel_t = now_;
    }

    void reply_altitude(Aircraft& a, const uint8_t* msg, int df) {
        if (df == 4 || df == 20) {
            const int fs = msg[0] & 7;
            if (fs == 1 || fs == 3) a.ground = true;
            else if (fs == 0 || fs == 2) a.ground = false;
        }
        // the extended squitter's altitude is preferred while it is fresh
        if (now_ - a.alt_t < 30) return;
        float alt = modes::ac13_altitude_ft(modes::bits(msg, 20, 32));
        if (!std::isfinite(alt)) return;
        // replies are only address-checked: ignore an implausible jump
        if (std::isfinite(a.alt_baro) && std::fabs(alt - a.alt_baro) > 3000 && now_ - a.alt_t < 120) return;
        a.alt_baro = alt;
    }

    void reply_identity(Aircraft& a, const uint8_t* msg) {
        const uint32_t id = modes::bits(msg, 20, 32);
        if (id) set_squawk(a, modes::squawk(id), false);
    }

    // A squawk from an extended squitter (CRC-checked) is taken at once; one
    // from a reply (only address / parity-checked - a bit error changes the
    // code) only when the next reply within 30 s has the same new code. A
    // change of a known squawk is reported (ATC assigns one per flight)
    void set_squawk(Aircraft& a, int v, bool checked) {
        if (v == a.squawk) { a.sq_pending = -1; return; }
        if (!checked && (v != a.sq_pending || now_ - a.sq_pending_t > 30)) {
            a.sq_pending = v;
            a.sq_pending_t = now_;
            return;
        }
        const int old = a.squawk;
        a.squawk = v;
        a.sq_pending = -1;
        if (old >= 0 && a.confirmed)
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") squawk changed " + fmt("%04.0f", old) + " -> " +
                       fmt("%04.0f", v) + where(a), 5.0);
    }

    // IDENT: the pilot pressed the ident button (SPI) - from an extended
    // squitter's surveillance status, or two replies with flight status 4/5
    // within 10 s (replies are only parity-checked)
    void note_spi(Aircraft& a, bool checked) {
        if (!checked) {
            a.spi_replies = now_ - a.spi_t < 10 ? a.spi_replies + 1 : 1;
            a.spi_t = now_;
            if (a.spi_replies < 2) return;
        }
        a.spi_t = now_;
        if (a.confirmed && now_ - a.said_spi_t > 120) {
            a.said_spi_t = now_;
            host.event(label(a) + " (" + id_of(a) + ") IDENT (SPI)" + where(a), 5.0);
        }
    }

    // --- positions --------------------------------------------------------
    bool station(double* lat, double* lon) const { return host.station(lat, lon); }

    // how long the last position stays the reference for local decoding
    double ref_age() const { return cpr_ ? REF_AGE_LONG_S : 60.0; }

    bool plausible(const Aircraft& a, double lat, double lon) const {
        double slat, slon;
        if (range_km_ > 0 && station(&slat, &slon) && modes::distance_km(slat, slon, lat, lon) > range_km_) return false;
        if (std::isfinite(a.lat) && now_ - a.pos_t < std::max(120.0, ref_age())) {
            // no faster than ~Mach 2.5 (+ slack for CPR / timing)
            const double d = modes::distance_km(a.lat, a.lon, lat, lon);
            if (d > 3.0 + (now_ - a.pos_t) * 0.85) return false;
        }
        return true;
    }

    // the aircraft's last position, moved on with its velocity when that is older than 5 s (cpr)
    void reference(const Aircraft& a, double* lat, double* lon) const {
        *lat = a.lat;
        *lon = a.lon;
        const double dt = now_ - a.pos_t;
        if (!cpr_ || dt < 5 || !std::isfinite(a.gs) || !std::isfinite(a.track) || now_ - a.vel_t > 60) return;
        const double d = a.gs * 0.514444 * dt, tr = a.track * M_PI / 180;
        *lat += d * std::cos(tr) / 111320.0;
        *lon += d * std::sin(tr) / (111320.0 * std::max(0.01, std::cos(a.lat * M_PI / 180)));
    }

    void position(Aircraft& a, bool odd) {
        const Cpr& c = a.cpr[odd];
        const Cpr& o = a.cpr[!odd];
        double slat = 0, slon = 0;
        const bool have_station = station(&slat, &slon);
        // reference for surface decoding / local decoding
        bool have_ref = false, own_ref = false;
        double rlat = 0, rlon = 0;
        if (std::isfinite(a.lat) && now_ - a.pos_t < ref_age()) {
            reference(a, &rlat, &rlon);
            have_ref = own_ref = true;
        } else if (have_station) {
            rlat = slat;
            rlon = slon;
            have_ref = true;
        }

        double lat, lon;
        bool ok = false, global = false;
        // an even / odd pair: at most 10 s apart (cpr: as long as the aircraft can't have moved 4 km)
        double pair_window = c.surface ? 25.0 : 10.0;
        if (cpr_ && !c.surface) {
            const bool vel = std::isfinite(a.gs) && a.gs > 30 && now_ - a.vel_t < 60;
            pair_window = vel ? std::clamp(PAIR_MAX_MOVE_M / (a.gs * 0.514444), 10.0, PAIR_LONG_S) : PAIR_NOVEL_S;
        }
        if (o.surface == c.surface && now_ - o.t <= pair_window) {
            const Cpr& e = odd ? o : c;
            const Cpr& d = odd ? c : o;
            ok = modes::cpr_global(e.lat, e.lon, d.lat, d.lon, odd, c.surface, have_ref, rlat, rlon, &lat, &lon);
            global = ok;
        }
        if (!ok && own_ref) ok = modes::cpr_local(c.lat, c.lon, odd, c.surface, rlat, rlon, &lat, &lon);
        if (!ok) return;
        if (!plausible(a, lat, lon)) {
            // a run of rejected global fixes: the old position was the wrong one
            if (!global || ++a.pos_rejects < 4) return;
        }
        a.pos_rejects = 0;
        a.lat = lat;
        a.lon = lon;
        a.pos_t = now_;
        if (have_station) {
            const double d = modes::distance_km(slat, slon, lat, lon);
            if (d > max_range_ && a.confirmed) {
                max_range_ = d;
                max_range_who_ = label(a);
            }
        }
    }

    // --- output -----------------------------------------------------------
    std::string label(const Aircraft& a) const {
        return a.callsign.empty() ? std::string(a.non_icao ? "~" : "") + hex6(a.addr) : a.callsign;
    }

    static std::string fmt(const char* f, double v) {
        char b[48];
        snprintf(b, sizeof b, f, v);
        return b;
    }

    // bytes as upper-case hex (not through fmt: %02X needs an integer)
    static std::string hex_bytes(const uint8_t* b, int n) {
        static const char* const X = "0123456789ABCDEF";
        std::string h;
        for (int i = 0; i < n; i++) {
            h += X[b[i] >> 4];
            h += X[b[i] & 15];
        }
        return h;
    }

    std::string alt_text(const Aircraft& a) const {
        if (a.ground) return "ground";
        if (std::isfinite(a.alt_baro)) return fmt("%.0f ft", a.alt_baro);
        if (std::isfinite(a.alt_geom)) return fmt("%.0f ft (GNSS)", a.alt_geom);
        return "";
    }

    std::string id_of(const Aircraft& a) const { return std::string(a.non_icao ? "~" : "") + hex6(a.addr); }

    // ", at -36.8812, 174.7712, 3500 ft" (what is known) for the unusual events
    std::string where(const Aircraft& a) const {
        std::string w;
        if (std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S) w = fmt("%.4f", a.lat) + ", " + fmt("%.4f", a.lon);
        const std::string alt = alt_text(a);
        if (!alt.empty()) w += (w.empty() ? "" : ", ") + alt;
        return w.empty() ? "" : " at " + w;
    }

    // the aircraft's row of the panel's table (TABLE_COLS)
    std::vector<std::string> table_cells(const Aircraft& a) const {
        auto num = [&](float v, const char* f) { return std::isfinite(v) ? fmt(f, v) : std::string(); };
        const bool pos = std::isfinite(a.lat);
        double slat, slon, dist = NAN, brg = NAN;
        if (pos && station(&slat, &slon)) {
            dist = modes::distance_km(slat, slon, a.lat, a.lon);
            const double r = M_PI / 180;
            const double y = std::sin((a.lon - slon) * r) * std::cos(a.lat * r);
            const double x = std::cos(slat * r) * std::sin(a.lat * r) -
                             std::sin(slat * r) * std::cos(a.lat * r) * std::cos((a.lon - slon) * r);
            brg = std::fmod(std::atan2(y, x) / r + 360.0, 360.0);
        }
        std::string emerg = a.emergency ? emergency_name(a.emergency)
                          : a.squawk == 7500 ? "hijack" : a.squawk == 7600 ? "radio failure"
                          : a.squawk == 7700 ? "emergency" : "";
        return {id_of(a),
                a.callsign,
                a.category.empty() ? "" : a.category + " " + category_name(a.category),
                a.squawk >= 0 ? fmt("%04.0f", a.squawk) : "",
                a.ground ? "ground" : num(a.alt_baro, "%.0f"),
                num(a.alt_geom, "%.0f"),
                num(a.vrate, "%+.0f"),
                num(a.gs, "%.0f"),
                num(a.track, "%.0f"),
                num(a.heading, "%.0f"),
                num(a.ias, "%.0f"),
                num(a.tas, "%.0f"),
                pos ? fmt("%.5f", a.lat) : "",
                pos ? fmt("%.5f", a.lon) : "",
                std::isfinite(dist) ? fmt("%.1f", dist) : "",
                std::isfinite(brg) ? fmt("%.0f", brg) : "",
                a.ground ? "ground" : "air",
                emerg,
                fmt("%.1f", a.rssi),
                std::to_string(a.msgs),
                pos ? fmt("%.0f", now_ - a.pos_t) : ""};
    }

    std::string info_text(const Aircraft& a) const {
        std::string s = "ICAO: " + std::string(a.non_icao ? "~" : "") + hex6(a.addr);
        auto line = [&](const char* k, const std::string& v) { if (!v.empty()) s += std::string("\n") + k + ": " + v; };
        line("Callsign", a.callsign);
        if (!a.category.empty()) line("Category", a.category + " " + category_name(a.category));
        line("Altitude", alt_text(a));
        if (std::isfinite(a.gs)) line("Ground speed", fmt("%.0f kt", a.gs));
        if (std::isfinite(a.track)) line("Track", fmt("%.0f°", a.track));
        if (std::isfinite(a.heading)) line("Heading", fmt("%.0f° (magnetic)", a.heading));
        if (std::isfinite(a.ias)) line("IAS", fmt("%.0f kt", a.ias));
        if (std::isfinite(a.tas)) line("TAS", fmt("%.0f kt", a.tas));
        if (std::isfinite(a.vrate)) line("Vertical rate", fmt("%+.0f ft/min", a.vrate));
        if (a.squawk >= 0) line("Squawk", fmt("%04.0f", a.squawk));
        if (a.emergency) line("Emergency", emergency_name(a.emergency));
        line("Signal", fmt("%.1f dBFS", a.rssi) + " · " + std::to_string(a.msgs) + " messages");
        return s;
    }

    void update_outputs(Aircraft& a) {
        if (!a.callsign.empty() && !a.said_callsign) {
            a.said_callsign = true;
            host.event(hex6(a.addr) + " is " + a.callsign +
                       (a.category.empty() ? "" : " (" + a.category + " " + category_name(a.category) + ")"), 5.0);
        }
        const bool emerg = a.emergency || a.squawk == 7500 || a.squawk == 7600 || a.squawk == 7700;
        if (emerg) {
            int key = a.emergency ? a.emergency : a.squawk;
            if (key != a.said_emergency) {
                a.said_emergency = key;
                std::string why = a.emergency ? emergency_name(a.emergency)
                                : a.squawk == 7500 ? "hijack" : a.squawk == 7600 ? "radio failure" : "emergency";
                host.event("⚠ EMERGENCY " + label(a) + " (" + hex6(a.addr) + "): " + why +
                           (a.squawk >= 0 ? fmt(", squawk %04.0f", a.squawk) : "") + where(a), 30.0);
            }
        } else if (a.said_emergency) {
            a.said_emergency = 0;
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") emergency over" +
                       (a.squawk >= 0 ? fmt(", squawk %04.0f", a.squawk) : "") + where(a), 30.0);
        }
        // other special squawks: 7400 lost link (unmanned aircraft, ICAO),
        // 7777 (US: military interceptor operations), 0000 (not a valid code)
        const char* special = a.squawk == 7400 ? "lost link (unmanned aircraft)"
                            : a.squawk == 7777 ? "military interception (US)"
                            : a.squawk == 0 ? "code 0000 (not a valid squawk)" : nullptr;
        if (special && a.said_special != a.squawk) {
            a.said_special = a.squawk;
            host.event("⚠ " + label(a) + " (" + id_of(a) + ") squawk " + fmt("%04.0f", a.squawk) + ": " + special +
                       where(a), 30.0);
        } else if (!special) {
            a.said_special = -1;
        }
        // unusual aircraft types
        if (!a.said_category && !a.category.empty()) {
            a.said_category = true;
            static const char* odd[] = {"A6", "B2", "B3", "B4", "B6", "B7"};
            for (const char* c : odd)
                if (a.category == c)
                    host.event("⚠ " + label(a) + " (" + id_of(a) + ") is a " + category_name(a.category) +
                               " (category " + a.category + ")" + where(a), 30.0);
        }
        // extreme vertical rate (airliners climb / descend at ~1000-3000
        // ft/min; an emergency descent is 6000+)
        if (!a.ground && std::isfinite(a.vrate) && now_ - a.vel_t < 30) {
            if (!a.said_vrate && std::fabs(a.vrate) >= VRATE_ALERT_FPM) {
                a.said_vrate = true;
                host.event("⚠ " + label(a) + " (" + id_of(a) + ") " + (a.vrate < 0 ? "descending " : "climbing ") +
                           fmt("%.0f ft/min", std::fabs(a.vrate)) + where(a), 30.0);
            } else if (a.said_vrate && std::fabs(a.vrate) < VRATE_ALERT_FPM * 2 / 3) {
                a.said_vrate = false;
            }
        }
        // fast and low: >= 400 kt below 10000 ft (civil traffic keeps to
        // 250 kt there - military jets don't)
        if (!a.ground && std::isfinite(a.gs) && std::isfinite(a.alt_baro) && now_ - a.vel_t < 30 &&
            now_ - a.alt_t < 30) {
            if (!a.said_fastlow && a.gs >= FAST_LOW_KT && a.alt_baro < FAST_LOW_FT) {
                a.said_fastlow = true;
                host.event("⚠ " + label(a) + " (" + id_of(a) + ") fast and low: " + fmt("%.0f kt", a.gs) + where(a),
                           30.0);
            } else if (a.said_fastlow && (a.gs < FAST_LOW_KT - 50 || a.alt_baro > FAST_LOW_FT + 1000)) {
                a.said_fastlow = false;
            }
        }
        const bool has_pos = std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S;
        if (has_pos && !a.said_pos) {
            a.said_pos = true;
            std::string t = label(a) + " first position " + fmt("%.4f", a.lat) + ", " + fmt("%.4f", a.lon);
            std::string alt = alt_text(a);
            if (!alt.empty()) t += ", " + alt;
            double slat, slon;
            if (station(&slat, &slon)) t += fmt(", %.0f km away", modes::distance_km(slat, slon, a.lat, a.lon));
            host.event(t, 5.0);
        }
        if (has_pos && now_ - a.map_t >= 0.5) {
            a.map_t = now_;
            kp::MapPoint p;
            p.id = std::string(a.non_icao ? "~" : "") + hex6(a.addr);
            p.lat = a.lat;
            p.lon = a.lon;
            p.label = label(a);
            p.kind = (a.category == "C1" || a.category == "C2") ? "vehicle" : "aircraft";
            p.heading = std::isfinite(a.track) ? a.track : a.heading;
            if (a.ground) p.altitude_m = 0;
            else if (std::isfinite(a.alt_baro)) p.altitude_m = a.alt_baro * 0.3048f;
            else if (std::isfinite(a.alt_geom)) p.altitude_m = a.alt_geom * 0.3048f;
            if (std::isfinite(a.gs)) p.speed_kmh = a.gs * 1.852f;
            p.info = info_text(a);
            p.ttl_s = POS_TIMEOUT_S;
            host.map_point(p);
            a.on_map = true;
        }
        if (now_ - a.table_t >= 1.0) {
            a.table_t = now_;
            a.in_table = true;
            host.table_row(id_of(a), table_cells(a));
        }
    }

    void forget(Aircraft& a) {
        const std::string id = std::string(a.non_icao ? "~" : "") + hex6(a.addr);
        if (a.on_map) host.map_remove(id);
        if (a.in_table) host.table_remove(id);
    }

    void housekeeping() {
        if (now_ - house_t_ < 1.0) return;
        const double dt = now_ - rate_t_;
        if (dt >= 5.0) {
            rate_ = (total_ - total_rate_mark_) / dt;
            total_rate_mark_ = total_;
            rate_t_ = now_;
        }
        house_t_ = now_;
        int n = 0, npos = 0;
        for (auto it = ac_.begin(); it != ac_.end();) {
            Aircraft& a = it->second;
            const double idle = now_ - a.last;
            if (idle > AIRCRAFT_TIMEOUT_S || (!a.confirmed && idle > 10)) {
                forget(a);
                it = ac_.erase(it);
                continue;
            }
            if (a.on_map && now_ - a.pos_t >= POS_TIMEOUT_S) {
                host.map_remove(std::string(a.non_icao ? "~" : "") + hex6(a.addr));
                a.on_map = false;
            }
            if (a.confirmed) {
                n++;
                if (std::isfinite(a.lat) && now_ - a.pos_t < POS_TIMEOUT_S) npos++;
            }
            ++it;
        }
        if (now_ - summary_t_ < 2.0) return;
        summary_t_ = now_;
        host.fact("Aircraft", std::to_string(n) + " (" + std::to_string(npos) + " with position)");
        host.fact("Messages", std::to_string(total_) + fmt(" (%.0f/s)", rate_));
        double slat, slon;
        if (station(&slat, &slon)) {
            host.fact("Max range", max_range_ > 0 ? fmt("%.0f km", max_range_) + " (" + max_range_who_ + ")" : "-");
        } else {
            host.fact("Max range", "set the station location (Station Information) for distances");
        }
        host.fact("Noise floor", fmt("%.1f dBFS", 20.0 * std::log10(std::max(noise_, 1e-6f))));
        host.fact("Antennas", antennas_fact());
        host.fact("Bit repair", repair_fact());
    }

    // how the messages were demodulated (the array's share)
    std::string antennas_fact() const {
        const uint64_t* p = st_.path;
        const uint64_t beam = p[P_BEAM] + p[P_DD] + p[P_MVDR] + p[P_INSIDE], sep = p[P_MVDR] + p[P_INSIDE];
        if (m_ < 2) {
            std::string s = array_ ? "1 (every antenna is combined in coherent mode with DoA on)" : "1 (array combining off)";
            if (beam) s += fmt(" - %.0f messages so far only with the array", static_cast<double>(beam));
            return s;
        }
        std::string s = fmt("%.0f combined: ", m_) + fmt("%.0f messages on their sum, ", static_cast<double>(p[P_SUM])) +
                        fmt("%.0f only with a beam", static_cast<double>(beam));
        if (sep_) s += fmt(" (%.0f of them separated from an overlap)", static_cast<double>(sep));
        if (pdoa_) s += fmt(" · bearings from the pulses of %.0f", static_cast<double>(st_.pdoa));
        return s;
    }

    std::string repair_fact() const {
        if (!fix_) return "off";
        const uint64_t n = st_.rep1 + st_.rep2;
        std::string s = (fix_ >= 2 ? "extended: " : "standard: ") + fmt("%.0f messages", static_cast<double>(n));
        if (n) s += fmt(" (%.0f with 2-3 bits", static_cast<double>(st_.rep2)) +
                    (fix_ >= 2 ? fmt(", %.0f replies", static_cast<double>(st_.rep_reply)) +
                                 fmt(", %.0f all-calls", static_cast<double>(st_.rep_df11))
                               : std::string()) + ")";
        if (st_.refused) s += fmt(" · %.0f refused (not the aircraft's signature)", static_cast<double>(st_.refused));
        return s;
    }

    std::string describe(const Aircraft& a, const uint8_t* msg, int len, int df, int fixed) const {
        static const char* const how[NPATH] = {"", " (beam)", " (beam, all pulses)", " (beam, overlap nulled)",
                                               " (inside another message)"};
        std::string h;
        h += hex_bytes(msg, len / 8);
        std::string s = "DF" + std::to_string(df) + " " + hex6(a.addr) + " " + h;
        if (df == 17 || df == 18) s += " TC" + std::to_string(modes::bits(msg + 4, 1, 5));
        if (fixed) s += fixed == 1 ? " (1 bit repaired)" : " (" + std::to_string(fixed) + " bits repaired)";
        if (m_ >= 2) s += how[path_];
        s += fmt(" %.1f dBFS", a.rssi);
        return s;
    }
};

}  // namespace

KRAKEN_PLUGIN(Adsb, {.id = "adsb",
                     .name = "ADS-B (1090 MHz)",
                     .category = "Aviation",
                     .description = "ADS-B / Mode S aircraft on 1090 MHz: callsign, position, altitude, speed, squawk - "
                                    "picking it tunes the VFO and its tuner to 1090 MHz at 2.4 MHz; aircraft appear on the 🗺 Map",
                     .version = "2.0",
                     .sample_rate = FS,
                     .min_vfo_rate = 2.0e6,
                     .author = "KrakenSDR",
                     .options = {{"range", "Max range", "500", "300=300 km|500=500 km|1000=1000 km|0=No limit",
                                  "Positions further than this from the station (Station Information) are rejected "
                                  "as decoding errors"},
                                 {"fix", "Bit repair", "1",
                                  "0=Off|1=Standard (1 bit, 2 weak bits)|2=Extended (3 weak bits, replies)",
                                  "Repairs messages with bad bits - only for aircraft already heard cleanly. Standard: "
                                  "extended squitters with one bad bit, or two among the eight least certain. Extended "
                                  "adds three among the twelve least certain, and all-call / altitude / identity replies "
                                  "with one or two bad bits whose level (with the array: spatial signature) is the "
                                  "aircraft's"},
                                 {"array", "Array combining", "1", "1=On|0=Off",
                                  "Coherent mode with DoA on: detect on every antenna's power and beamform the weak "
                                  "messages (up to ~7 dB more with 5 antennas). Off: antenna 0 only"},
                                 {"separate", "Overlap separation", "1", "1=On|0=Off",
                                  "With the array: decode messages that overlap another aircraft's from another "
                                  "direction (an interference-nulling beam; a second message inside a decoded one)"},
                                 {"pulse_doa", "DoA from the pulses", "1", "1=On|0=Off",
                                  "With the array: each aircraft's bearing from its messages' pulse samples only, "
                                  "without samples another decoded message has a pulse in"},
                                 {"cpr", "Long position pairing", "1", "1=On|0=Off",
                                  "Positions of weak / distant aircraft: even and odd position messages up to 30 s "
                                  "apart (while the aircraft's speed keeps it within 4 km), the last position as the "
                                  "reference for 3 minutes"},
                                 {"sens", "High sensitivity", "0", "1=On|0=Off",
                                  "Lower preamble thresholds and every timing tried: a few more weak messages, more CPU"}},
                     .map = true,
                     .manual_only = true,
                     .fixed_freq_hz = 1090e6,
                     .talkers = true,
                     .multichannel = true})
