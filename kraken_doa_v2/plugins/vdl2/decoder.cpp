// VDL Mode 2 (VHF Digital Link, ICAO Annex 10 Vol III 6.4): the aircraft
// datalink on 25 kHz channels at 136.x MHz - D8PSK, 10 500 symbols/s,
// 31.5 kbit/s, bursts from aircraft and ground stations on one channel
// (CSMA). One channel per VFO, at the VFO's centre.
//
// Burst (6.4.3.1): ramp-up (5 symbols), unique word (16 symbols, not
// scrambled), then scrambled: reserved symbol (000), transmission length
// (17 bits, lsb first), header FEC ((25, 20) block code), the data: AVLC
// frames (HDLC flags, bit stuffing, CRC-16 FCS) split into rows of 249
// octets, each with RS(255, 249) check octets (a short row sends 2 or 4 of
// them), interleaved column by column. Bits go 3 per symbol, X first; the
// phase change per symbol is Gray coded (Table 6-1).
//
// Measured on a real recording where the standard leaves a choice (the
// layers check: header FEC, RS syndromes, FCS): the scrambler's PN bit is
// the feedback bit (stage 1 xor stage 15) that is shifted in; an octet's
// first bit is its most significant bit in the interleaver and its least
// significant bit as a GF(256) symbol of the RS code, whose first data
// octet is the highest power of x; AVLC addresses read the 7 bits of each
// octet after its extension bit as they are sent, most significant first:
// 1 flag bit, 3 type bits (1 aircraft, 4 / 5 ground station), 24 address
// bits (an aircraft's ICAO address, as ADS-B sends it).
//
// Receiver: 52.5 kHz (5 samples per symbol) -> channel low-pass -> unique
// word correlator on the one-symbol differential products (both spectrum
// orientations; an IQ-swapped input is decoded too) -> timing (parabolic
// peak), carrier frequency and phase from the unique word -> TWO
// demodulators of the burst side by side, each with its own Gardner timing
// loop on cubic-interpolated samples: coherent (decision-directed carrier
// loop, ~2-3 dB better in white noise) and differential (frequency loop
// only, robust to phase noise); the one with more valid frames wins ->
// descrambling -> header (one bit corrected) -> de-interleaving -> RS
// errors + erasures -> HDLC -> AVLC.

#include "acars_msg.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "rs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr double RS_SYM = 10500;         // symbols per second
constexpr int SPS = 5;
constexpr double FS = RS_SYM * SPS;      // 52.5 kHz
// channel filter: the signal is +-8.4 kHz (alpha 0.6); measured best (real
// recording, white noise): 33 taps, -6 dB at 8.5 kHz - wider ones let in
// ~1-2 dB more noise, sharper ones add intersymbol interference
constexpr int LPF_TAPS = 33;
constexpr float LPF_CUT = 8500;
constexpr int ZLEN = 1 << 12, ZMASK = ZLEN - 1;   // ring of filtered samples
constexpr int DLEN = 1 << 7, DMASK = DLEN - 1;    // ring of differential products
constexpr float SYNC_Q = 0.80f;          // unique word match: |sum| / sum of magnitudes
constexpr float MAX_OFFSET_HZ = 3000;    // carrier offset accepted at the unique word
constexpr int MAX_LEN = 131071;          // transmission length (17 bits)
constexpr float PI4 = kp::PI / 4;
constexpr float KP = 0.15f, KI = 0.005f; // coherent carrier loop
constexpr float KF = 0.01f;              // differential: frequency loop
constexpr double KT = 0.03;              // Gardner timing loop (samples per unit error): +-200 ppm clocks

// Table 6-1: (X Y Z) -> phase change in pi/4, and back
constexpr int GRAY[8] = {0, 1, 3, 2, 7, 6, 4, 5};   // index = X<<2 | Y<<1 | Z
constexpr int UNGRAY[8] = {0, 1, 3, 2, 6, 7, 5, 4};

// the unique word, as (X Y Z) triplets transmitted left to right
constexpr const char* UW = "000 010 011 110 000 001 101 110 001 100 011 111 101 111 100 010";
constexpr int UW_LEN = 16;

// header FEC parity matrix (6.4.3.1.1.5): P = [R1..R3 TL1..TL17] H^T
constexpr const char* HMAT[5] = {"00000000111111111111", "00111111000011111111", "11000111001100001111",
                                 "11011011010100110011", "01101001111001010101"};

constexpr uint16_t PN_INIT = 0x6959;     // stages 1..15 = 1101 0010 1011 001 (stage 1 = bit 14)

uint8_t rev8(uint8_t b) {
    b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

uint16_t crc_x25(const uint8_t* p, int n) {
    static const std::array<uint16_t, 256> tab = [] {
        std::array<uint16_t, 256> t{};
        for (int i = 0; i < 256; i++) {
            uint16_t c = static_cast<uint16_t>(i);
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) c = (c >> 8) ^ tab[(c ^ p[i]) & 0xFF];
    return c ^ 0xFFFF;
}

// check octets a row sends for its number of data octets (6.4.3.1.2.3)
int row_parity(int k) { return k <= 2 ? 0 : k <= 30 ? 2 : k <= 67 ? 4 : 6; }

// An AVLC address (4 octets): flag bit, type (3), address (24)
struct Addr {
    int flag = 0, type = 0;
    uint32_t a = 0;
    bool aircraft() const { return type == 1; }
    bool ground() const { return type == 4 || type == 5; }
    bool broadcast() const { return a == 0xFFFFFF; }
};
Addr address(const uint8_t* o) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        for (int k = 1; k < 8; k++) v = (v << 1) | ((o[i] >> k) & 1u);
    return {static_cast<int>(v >> 27), static_cast<int>((v >> 24) & 7), v & 0xFFFFFF};
}
std::string hex6(uint32_t a) {
    char b[8];
    snprintf(b, sizeof b, "%06X", a & 0xFFFFFF);
    return b;
}

// One AVLC frame that passed its FCS
struct Frame {
    Addr dst, src;
    uint8_t ctl = 0;
    std::vector<uint8_t> raw;   // the whole frame without its FCS
    const uint8_t* info() const { return raw.data() + 9; }
    int info_len() const { return static_cast<int>(raw.size()) - 9; }
};

// One demodulator of a burst: timing, carrier, bits, the interleaver table
struct Demod {
    enum State { RUN, DONE, FAIL };
    bool coherent = true;
    State st = FAIL;
    double t_sym = 0;          // next symbol (absolute sample index, fractional)
    float omega = 0, theta = 0, ypow = 1;
    float evm = 0;             // mean |phase error| of the decisions (rad)
    int a_prev = 0, weak = 0, nsym = 0;
    kp::cf y_prev, raw_prev;
    uint16_t pn = PN_INIT;
    int nbits = 0;
    uint32_t hdr = 0;          // header bits, the first in bit 0
    int len_bits = 0, rows = 0, total = 0, got = 0, obits = 0;
    uint8_t oct = 0;
    std::vector<int> rowlen, order;
    std::vector<uint8_t> table;
    // result
    std::vector<Frame> frames;
    int rs_fixed = 0;
    double t_end = 0;
};

class Vdl2 : public kp::Decoder {
public:
    explicit Vdl2(kp::Host& h) : Decoder(h), lpf_(kp::fir_lowpass(LPF_TAPS, LPF_CUT, FS)) {
        // the unique word's 15 phase changes inside it, and its phases
        int st[UW_LEN];
        for (int k = 0; k < UW_LEN; k++) {
            const char* t = UW + 4 * k;
            st[k] = GRAY[(t[0] - '0') << 2 | (t[1] - '0') << 1 | (t[2] - '0')];
        }
        uw_cum_[0] = 0;
        for (int k = 1; k < UW_LEN; k++) {
            uw_ref_[k - 1] = std::polar(1.0f, st[k] * PI4);
            uw_cum_[k] = (uw_cum_[k - 1] + st[k]) & 7;
        }
        for (int r = 0; r < 5; r++)
            for (int i = 0; i < 20; i++) hcol_[i] |= static_cast<uint8_t>((HMAT[r][i] - '0') << (4 - r));
        dm_[0].coherent = true;
        dm_[1].coherent = false;
        host.table_columns({"Station", "Kind", "Name", "Flight", "Frames", "Last"});
        host.table_keep(3600);
        reset();
    }

    void reset() override {
        lpf_.reset();
        zb_.fill({});
        db_.fill({});
        mag_.fill(0);
        n_ = 0;
        pwr_ = 0;
        nf_ = -1;
        burst_ = false;
        cand_ = false;
        qhist_.fill(0);
    }

    void process(const kp::cf* x, size_t n) override {
        // Host::time() of absolute sample index i (the filter's delay out)
        t_base_ = host.time() - static_cast<double>(n_) / FS - (LPF_TAPS - 1) / 2 / FS;
        for (size_t i = 0; i < n; i++) sample(lpf_.push(x[i]));
    }

private:
    kp::Fir<kp::cf> lpf_;
    vdl2::ReedSolomon rs_;
    std::array<kp::cf, ZLEN> zb_{};
    std::array<kp::cf, DLEN> db_{};
    std::array<float, DLEN> mag_{};
    std::array<kp::cf, 15> uw_ref_;
    std::array<int, UW_LEN> uw_cum_{};
    std::array<uint8_t, 20> hcol_{};
    int64_t n_ = 0;            // absolute index of the next sample
    double t_base_ = 0;
    float pwr_ = 0, nf_ = -1;  // short-term power, noise floor

    // hunting: the best unique word match within one symbol
    bool cand_ = false;
    int64_t cand_n_ = 0;
    float cand_q_ = 0;
    kp::cf cand_c_;
    bool cand_inv_ = false;
    std::array<float, 3> qhist_{};   // |c| of the last three samples
    float cand_prev_ = 0, cand_next_ = 0;

    // the burst being received
    bool burst_ = false, inv_ = false;
    double t_uw_ = 0;          // the unique word's first symbol
    double offset_hz_ = 0;
    std::array<Demod, 2> dm_;  // coherent, differential

    // statistics and what was learnt
    long bursts_ = 0, frames_ = 0, fcs_bad_ = 0, rs_fixed_ = 0;
    struct Station {
        bool ground = false;
        std::string name, flight, last, kind;
        std::string coverage;   // ground station: airport(s) it serves
        double lat = NAN, lon = NAN;
        long frames = 0;
    };
    std::map<std::string, Station> st_;
    std::set<uint32_t> aircraft_, ground_;

    // --- samples -------------------------------------------------------------

    kp::cf z_at(int64_t i) const { return zb_[i & ZMASK]; }
    // cubic (4-point Lagrange) interpolation at fractional index t
    kp::cf interp(double t) const {
        const int64_t i = static_cast<int64_t>(std::floor(t));
        const float u = static_cast<float>(t - static_cast<double>(i));
        const kp::cf a = z_at(i - 1), b = z_at(i), c = z_at(i + 1), d = z_at(i + 2);
        const float wa = -u * (u - 1) * (u - 2) / 6, wb = (u + 1) * (u - 1) * (u - 2) / 2;
        const float wc = -(u + 1) * u * (u - 2) / 2, wd = (u + 1) * u * (u - 1) / 6;
        const kp::cf v = wa * a + wb * b + wc * c + wd * d;
        return inv_ ? std::conj(v) : v;
    }

    void sample(kp::cf z) {
        const int64_t n = n_++;
        zb_[n & ZMASK] = z;
        pwr_ += 0.05f * (std::norm(z) - pwr_);
        // noise floor: follows the power down quickly, up slowly (~0.4 s; in
        // the first 0.1 s of input quickly), and not up at all during a burst
        if (nf_ < 0) nf_ = pwr_;
        if (pwr_ < nf_) nf_ += 0.02f * (pwr_ - nf_);
        else if (!burst_) nf_ += (n < FS / 10 ? 0.002f : 0.00005f) * (pwr_ - nf_);
        const kp::cf d = z * std::conj(z_at(n - SPS));
        db_[n & DMASK] = d;
        mag_[n & DMASK] = std::abs(d);
        if (burst_) burst(n);
        else hunt(n);
    }

    // --- unique word ---------------------------------------------------------

    void hunt(int64_t n) {
        kp::cf c1{0, 0}, c2{0, 0};
        float e = 0;
        for (int k = 0; k < 15; k++) {
            const int64_t j = n - SPS * (14 - k);
            const kp::cf d = db_[j & DMASK];
            c1 += d * std::conj(uw_ref_[k]);
            c2 += d * uw_ref_[k];
            e += mag_[j & DMASK];
        }
        // an IQ-swapped (spectrum inverted) input matches the conjugate
        const bool inv = std::abs(c2) > std::abs(c1);
        const kp::cf c = inv ? std::conj(c2) : c1;
        const float mc = std::abs(c);
        qhist_ = {qhist_[1], qhist_[2], mc};
        const float q = e > 0 ? mc / e : 0;
        const float f = std::arg(c) * static_cast<float>(RS_SYM / (2 * M_PI));
        // a match, 6 dB above the noise floor, at a plausible carrier offset
        const bool ok = q >= SYNC_Q && e > 15 * 4 * nf_ && std::fabs(f) < MAX_OFFSET_HZ;
        if (cand_) {
            if (n == cand_n_ + 1) cand_next_ = mc;
            if (ok && q > cand_q_) {
                take(n, q, c, inv);
            } else if (n - cand_n_ >= SPS) {
                cand_ = false;
                lock();
            }
            return;
        }
        if (ok) take(n, q, c, inv);
    }
    void take(int64_t n, float q, kp::cf c, bool inv) {
        cand_ = true;
        cand_n_ = n;
        cand_q_ = q;
        cand_c_ = c;
        cand_inv_ = inv;
        cand_prev_ = qhist_[1];
        cand_next_ = 0;
    }

    // a unique word ended at cand_n_ (its last symbol): start the burst
    void lock() {
        inv_ = cand_inv_;
        // timing to a fraction of a sample (parabola through |c|)
        const float a = cand_prev_, b = std::abs(cand_c_), c = cand_next_;
        float frac = 0;
        if (a > 0 && c > 0 && (a - 2 * b + c) < 0) frac = std::clamp(0.5f * (a - c) / (a - 2 * b + c), -0.5f, 0.5f);
        const double t_last = static_cast<double>(cand_n_) + frac;
        const float omega = std::arg(cand_c_);   // carrier offset, rad per symbol
        // carrier phase at the last unique word symbol, from all 16
        kp::cf acc{0, 0};
        for (int k = 0; k < UW_LEN; k++) {
            const kp::cf y = interp(t_last - SPS * (UW_LEN - 1 - k));
            acc += y * std::polar(1.0f, -(uw_cum_[k] * PI4 + omega * static_cast<float>(k - (UW_LEN - 1))));
        }
        for (Demod& d : dm_) {
            d.st = Demod::RUN;
            d.t_sym = t_last + SPS;
            d.omega = omega;
            d.theta = std::arg(acc);
            d.a_prev = uw_cum_[UW_LEN - 1];
            d.raw_prev = interp(t_last);
            d.y_prev = d.raw_prev * std::polar(1.0f, -d.theta);
            d.ypow = std::norm(acc) / (UW_LEN * UW_LEN);
            d.weak = 0;
            d.nsym = 0;
            d.evm = 0.1f;
            d.pn = PN_INIT;
            d.nbits = 0;
            d.hdr = 0;
            d.total = d.got = d.obits = 0;
            d.frames.clear();
            d.rs_fixed = 0;
        }
        t_uw_ = t_last - SPS * (UW_LEN - 1);
        offset_hz_ = (inv_ ? -1 : 1) * omega * RS_SYM / (2 * M_PI);
        burst_ = true;
    }

    // --- burst ---------------------------------------------------------------

    void burst(int64_t n) {
        bool running = false;
        for (Demod& d : dm_) {
            while (d.st == Demod::RUN && d.t_sym + 3 <= static_cast<double>(n)) symbol(d);
            running = running || d.st == Demod::RUN;
        }
        if (!running) finish();
    }

    void symbol(Demod& d) {
        const double t = d.t_sym;
        d.theta += d.omega;
        const kp::cf raw = interp(t);
        const kp::cf y = raw * std::polar(1.0f, -d.theta);
        int step;
        float err;
        if (d.coherent) {
            // decision on the absolute phase; carrier loop on its error
            const float ph = std::arg(y);
            const int a = static_cast<int>(std::lround(ph / PI4));
            err = std::remainder(ph - static_cast<float>(a) * PI4, 2 * kp::PI);
            d.theta += KP * err;
            d.omega += KI * err;
            step = (a - d.a_prev + 16) & 7;
            d.a_prev = a & 7;
        } else {
            // decision on the phase change; frequency loop on its error
            const float dp = std::remainder(std::arg(raw * std::conj(d.raw_prev)) - d.omega, 2 * kp::PI);
            const int s = static_cast<int>(std::lround(dp / PI4));
            err = std::remainder(dp - static_cast<float>(s) * PI4, 2 * kp::PI);
            d.omega += KF * err;
            step = s & 7;
        }
        // not a D8PSK signal (any more): the decisions' phase errors spread
        // over +-pi/8 (mean 0.39 rad; a signal at 12 dB SNR: < 0.2)
        d.evm += (std::fabs(err) - d.evm) / 32;
        if (++d.nsym > 24 && d.evm > 0.3f) {
            d.st = Demod::FAIL;
            return;
        }
        d.raw_prev = raw;
        // Gardner timing: the mid point between this symbol and the last
        const kp::cf mid = interp(t - SPS / 2.0) * std::polar(1.0f, -(d.theta - d.omega / 2));
        const float te = ((y - d.y_prev) * std::conj(mid)).real() / (d.ypow + 1e-20f);
        d.t_sym = t + SPS - KT * std::clamp(te, -1.0f, 1.0f);
        d.y_prev = y;
        // the burst ends when its power is gone
        const float p = std::norm(y);
        d.ypow += 0.05f * (p - d.ypow);
        if (p < 2 * nf_) {
            if (++d.weak > 12) {
                d.st = Demod::FAIL;
                return;
            }
        } else {
            d.weak = 0;
        }
        const int xyz = UNGRAY[step];
        for (int k = 2; k >= 0 && d.st == Demod::RUN; k--) bit(d, (xyz >> k) & 1);   // X first
    }

    void bit(Demod& d, int b) {
        const int f = ((d.pn >> 14) ^ d.pn) & 1;
        d.pn = static_cast<uint16_t>((d.pn >> 1) | (f << 14));
        b ^= f;
        if (d.nbits < 25) {
            d.hdr |= static_cast<uint32_t>(b) << d.nbits;
            if (++d.nbits == 25 && !header(d)) d.st = Demod::FAIL;
            return;
        }
        d.oct = static_cast<uint8_t>(d.oct << 1 | b);   // first bit = the octet's msb
        if (++d.obits < 8) return;
        d.obits = 0;
        d.table[d.order[d.got]] = d.oct;
        if (++d.got == d.total) {
            d.t_end = time_of(d.t_sym);
            decode_rows(d);
            d.st = Demod::DONE;
        }
    }

    // reserved symbol, length, FEC: one bit error corrected
    bool header(Demod& d) {
        uint32_t h = d.hdr;
        uint8_t s = 0;
        for (int i = 0; i < 20; i++)
            if (h >> i & 1) s ^= hcol_[i];
        for (int r = 0; r < 5; r++) s ^= static_cast<uint8_t>(((h >> (20 + r)) & 1) << (4 - r));
        if (s) {
            bool fixed = (s & (s - 1)) == 0;   // a check bit
            for (int i = 0; i < 20 && !fixed; i++)
                if (hcol_[i] == s) {
                    h ^= 1u << i;
                    fixed = true;
                }
            if (!fixed) return false;
        }
        if (h & 7) return false;   // the reserved symbol is 000
        d.len_bits = static_cast<int>((h >> 3) & 0x1FFFF);
        if (d.len_bits < 8 * 11 + 16 || d.len_bits > MAX_LEN) return false;   // at least one AVLC frame + flags
        const int octets = (d.len_bits + 7) / 8;
        d.rows = (d.len_bits + 1991) / 1992;
        d.rowlen.assign(d.rows, 249);
        d.rowlen.back() = octets - 249 * (d.rows - 1);
        // the order octets arrive in: column by column, the check octets a
        // row doesn't send skipped
        d.order.clear();
        for (int col = 0; col < 255; col++)
            for (int r = 0; r < d.rows; r++)
                if (col < d.rowlen[r] || (col >= 249 && col < 249 + row_parity(d.rowlen[r]))) d.order.push_back(r * 255 + col);
        d.total = static_cast<int>(d.order.size());
        d.table.assign(static_cast<size_t>(d.rows) * 255, 0);
        return true;
    }

    double time_of(double idx) const { return t_base_ + idx / FS; }

    // --- data: RS, HDLC, AVLC --------------------------------------------------

    void decode_rows(Demod& d) {
        std::vector<uint8_t> bits;
        bits.reserve(static_cast<size_t>(d.len_bits) + 8);
        for (int r = 0; r < d.rows; r++) {
            uint8_t* row = &d.table[static_cast<size_t>(r) * 255];
            const int k = d.rowlen[r], np = row_parity(k);
            if (np > 0) {
                uint8_t cw[255] = {};
                for (int j = 0; j < k; j++) cw[254 - j] = rev8(row[j]);
                for (int p = 0; p < np; p++) cw[5 - p] = rev8(row[249 + p]);
                int er[6], ne = 0;
                for (int p = np; p < 6; p++) er[ne++] = 5 - p;
                const int c = rs_.decode(cw, er, ne, 6, 254 - k);
                if (c > ne) d.rs_fixed += c - ne;
                if (c > 0)
                    for (int j = 0; j < k; j++) row[j] = rev8(cw[254 - j]);
            }
            for (int j = 0; j < k; j++)
                for (int b = 7; b >= 0; b--) bits.push_back((row[j] >> b) & 1);
        }
        bits.resize(static_cast<size_t>(d.len_bits));
        hdlc(bits, d.frames);
    }

    // the burst is over: the demodulator with more valid frames is reported
    void finish() {
        burst_ = false;
        cand_ = false;
        const bool any = dm_[0].st == Demod::DONE || dm_[1].st == Demod::DONE;
        if (!any) return;
        bursts_++;
        Demod& best = dm_[1].frames.size() > dm_[0].frames.size() ? dm_[1] : dm_[0];
        if (best.frames.empty()) return;
        rs_fixed_ += best.rs_fixed;
        report(best.frames, time_of(t_uw_), best.t_end);
    }

    void hdlc(const std::vector<uint8_t>& bits, std::vector<Frame>& out) {
        std::vector<uint8_t> cur;
        bool in = false;
        int ones = 0;
        for (uint8_t b : bits) {
            if (b) {
                if (++ones > 6) in = false;   // abort
                if (in) cur.push_back(1);
                continue;
            }
            if (ones == 6) {   // flag: closes the frame (its 0 + six 1s are in cur)
                if (in && cur.size() >= 7) {
                    cur.resize(cur.size() - 7);
                    frame(cur, out);
                }
                cur.clear();
                in = true;
            } else if (ones != 5 && in) {   // after five 1s a stuffed 0
                cur.push_back(0);
            }
            ones = 0;
        }
    }

    void frame(const std::vector<uint8_t>& fb, std::vector<Frame>& out) {
        if (fb.size() % 8 || fb.size() < 11 * 8) return;
        std::vector<uint8_t> by(fb.size() / 8);
        for (size_t i = 0; i < by.size(); i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) v |= static_cast<uint8_t>(fb[i * 8 + k] << k);   // lsb first
            by[i] = v;
        }
        const int n = static_cast<int>(by.size());
        if (crc_x25(by.data(), n - 2) != (by[n - 2] | by[n - 1] << 8)) {
            fcs_bad_++;
            return;
        }
        // address field: extension bits 0 in octets 1-7, 1 in octet 8
        for (int i = 0; i < 7; i++)
            if (by[i] & 1) return;
        if (!(by[7] & 1)) return;
        Frame f;
        f.dst = address(by.data());
        f.src = address(by.data() + 4);
        f.ctl = by[8];
        f.raw.assign(by.begin(), by.end() - 2);
        out.push_back(std::move(f));
    }

    // --- reporting -------------------------------------------------------------

    static std::string id_of(const Addr& a) { return a.ground() ? "GS " + hex6(a.a) : hex6(a.a); }

    static std::string kind_of(const Frame& f) {
        const uint8_t c = f.ctl;
        char b[40];
        if (!(c & 1)) {
            snprintf(b, sizeof b, "I N(S)=%d N(R)=%d", (c >> 1) & 7, (c >> 5) & 7);
        } else if ((c & 3) == 1) {
            static const char* S[4] = {"RR", "RNR", "REJ", "SREJ"};
            snprintf(b, sizeof b, "%s N(R)=%d", S[(c >> 2) & 3], (c >> 5) & 7);
        } else {
            switch (c & 0xEF) {
                case 0x03: return "UI";
                case 0xAF: return "XID";
                case 0xE3: return "TEST";
                case 0x0F: return "DM";
                case 0x43: return "DISC";
                case 0x63: return "UA";
                case 0x87: return "FRMR";
                default: snprintf(b, sizeof b, "U 0x%02X", c);
            }
        }
        return b;
    }

    // a ground station's XID: ISO 8885 groups; the VDL private group (0xF0)
    // holds its airport coverage (0xC1, ICAO codes) and its location (0xC8:
    // 12-bit latitude, 12-bit longitude, 0.1 degree, two's complement)
    static void xid(const Frame& f, std::string& coverage, double& lat, double& lon) {
        const uint8_t* p = f.info();
        const int n = f.info_len();
        if (n < 4 || p[0] != 0x82) return;
        int i = 1;
        while (i + 3 <= n) {
            const int gi = p[i], gl = p[i + 1] << 8 | p[i + 2];
            int j = i + 3;
            const int ge = std::min(n, j + gl);
            while (j + 2 <= ge) {
                const int pi = p[j], pl = p[j + 1];
                const uint8_t* v = p + j + 2;
                if (j + 2 + pl > ge) break;
                if (gi == 0xF0 && pi == 0xC1 && pl >= 4) {
                    coverage.clear();
                    for (int k = 0; k + 4 <= pl; k += 4) {
                        if (!coverage.empty()) coverage += " ";
                        coverage += kp::printable(std::string(reinterpret_cast<const char*>(v + k), 4));
                    }
                }
                if (gi == 0xF0 && pi == 0xC8 && pl == 3) {
                    int la = v[0] << 4 | v[1] >> 4, lo = (v[1] & 0x0F) << 8 | v[2];
                    if (la & 0x800) la -= 0x1000;
                    if (lo & 0x800) lo -= 0x1000;
                    if (std::abs(la) <= 900 && std::abs(lo) <= 1800) {
                        lat = la / 10.0;
                        lon = lo / 10.0;
                    }
                }
                j += 2 + pl;
            }
            i = ge;
        }
    }

    // ID separation: a burst is ONE transmitter's, named by the frames'
    // source address - an aircraft (its ICAO address) or a ground station
    void talker(const Frame& f, double start, double end) {
        const Addr& s = f.src;
        if (!(s.aircraft() || s.ground()) || s.broadcast() || s.a == 0) return;
        const std::string id = id_of(s);
        const Station& st = st_[id];
        kp::Talker t;
        t.id = id;
        t.label = (st.ground ? (st.coverage.empty() ? std::string("ground station") : st.coverage)
                             : (st.name.empty() ? std::string("aircraft") : st.name + (st.flight.empty() ? "" : " " + st.flight)))
                      .substr(0, 64);
        t.start_s = start;
        t.end_s = std::min(end, start + 0.24);   // packets <= 0.25 s: the burst's start (unique word on)
        if (!(t.end_s > t.start_s)) return;
        t.packet = true;
        t.freq_hz = offset_hz_;   // this transmitter's carrier
        t.bw_hz = 17000;          // raised cosine, alpha 0.6, 10.5 ksymbols/s
        t.avg_s = 10;
        t.keep_s = 1800;
        host.talker(t);
    }

    void report(const std::vector<Frame>& fr, double start, double end) {
        host.valid(start, end);
        bool same = true;
        for (const Frame& f : fr) same = same && f.src.a == fr[0].src.a && f.src.type == fr[0].src.type;
        for (const Frame& f : fr) one(f);
        if (same) talker(fr[0], start, end);
        char o[32];
        snprintf(o, sizeof o, "%+.0f Hz", offset_hz_);
        host.fact("Bursts", std::to_string(bursts_) + " decoded, " + std::to_string(frames_) + " frames");
        host.fact("Aircraft heard", std::to_string(aircraft_.size()));
        host.fact("Ground stations", std::to_string(ground_.size()));
        host.fact("Last carrier offset", o);
        if (rs_fixed_) host.fact("RS corrections", std::to_string(rs_fixed_) + " octets");
        if (fcs_bad_) host.fact("Frames failed FCS", std::to_string(fcs_bad_));
    }

    void one(const Frame& f) {
        frames_++;
        const std::string sid = id_of(f.src), did = id_of(f.dst);
        if (f.src.aircraft() && !f.src.broadcast()) aircraft_.insert(f.src.a);
        if (f.src.ground()) ground_.insert(f.src.a);
        Station& st = st_[sid];
        st.ground = f.src.ground();
        st.frames++;
        const std::string kind = kind_of(f);
        st.kind = kind;
        const bool up = f.src.ground();
        const std::string arrow = up ? "↑ " : "↓ ";
        const std::string dname = f.dst.broadcast() ? std::string("all") : did;

        if (host.raw_wanted()) {
            std::string hx;
            char b[4];
            for (uint8_t v : f.raw) {
                snprintf(b, sizeof b, "%02X", v);
                hx += b;
            }
            host.raw("VDL2 " + sid + ">" + dname + " " + kind + " " + hx);
        }

        const uint8_t* info = f.info();
        const int ni = f.info_len();
        // ACARS over AVLC: an I frame whose information starts FF FF 01,
        // followed by the ACARS block (mode .. suffix, BCS, DEL)
        if (!(f.ctl & 1) && ni >= 3 + 13 && info[0] == 0xFF && info[1] == 0xFF && info[2] == 0x01) {
            const uint8_t* c = info + 3;
            int end = -1;
            for (int i = 12; i < ni - 3; i++)
                if ((c[i] & 0x7F) == acars::ETX || (c[i] & 0x7F) == acars::ETB) {
                    end = i;
                    break;
                }
            acars::Block b;
            if (end > 0 && acars::parse(c, end + 1, b)) {
                const std::string reg = acars::registration(b.addr);
                const std::string lab = acars::label_text(b.label);
                const char* lname = acars::label_name(b.label);
                std::string msn, flight, body = b.text;
                if (b.down) acars::split_downlink(b.text, msn, flight, body);
                // the aircraft's registration / flight: the source of a
                // downlink, the destination of an uplink
                const std::string acid = up ? did : sid;
                Station& ac = st_[acid];
                if (!reg.empty()) ac.name = reg;
                if (!flight.empty()) ac.flight = kp::printable(flight);
                const std::string txt = acars::one_line(body);
                std::string ev = arrow + (up ? sid + " → " : "") + (reg.empty() ? acid : reg + " (" + acid + ")") +
                                 (flight.empty() ? "" : " " + kp::printable(flight)) + " · ACARS " + lab +
                                 (*lname ? std::string(" ") + lname : "");
                if (b.bid > ' ' && b.bid < acars::DEL) ev += std::string(" · blk ") + b.bid;
                if (!msn.empty()) ev += " · " + msn;
                if (!txt.empty()) ev += ": " + txt;
                if (!b.text.empty() || host.verbose()) host.event(ev, 30);
                st.last = "ACARS " + lab + (txt.empty() ? "" : ": " + txt.substr(0, 150));
                if (&ac != &st) ac.last = st.last;
                host.fact("Last ACARS", (reg.empty() ? acid : reg) + " " + lab + (txt.empty() ? "" : ": " + txt.substr(0, 200)));
                row(acid);
                if (acid != sid) row(sid);
                return;
            }
        }
        if ((f.ctl & 0xEF) == 0xAF && f.src.ground()) {
            std::string cov;
            double lat = NAN, lon = NAN;
            xid(f, cov, lat, lon);
            if (!cov.empty()) st.coverage = cov;
            if (std::isfinite(lat)) {
                st.lat = lat;
                st.lon = lon;
            }
            char pos[48] = "";
            if (std::isfinite(st.lat))
                snprintf(pos, sizeof pos, " %.1f%c %.1f%c", std::fabs(st.lat), st.lat < 0 ? 'S' : 'N', std::fabs(st.lon),
                         st.lon < 0 ? 'W' : 'E');
            st.last = "XID (station information)";
            host.event("📡 ground station " + hex6(f.src.a) + (st.coverage.empty() ? "" : " " + st.coverage) + pos, 600);
            if (!st.coverage.empty()) host.fact("Last ground station", hex6(f.src.a) + " " + st.coverage + pos);
            map_point(sid, st);
            row(sid);
            return;
        }
        // everything else (link control, X.25 / ATN packets): verbose only
        std::string what = kind;
        if ((!(f.ctl & 1) || (f.ctl & 0xEF) == 0x03) && ni > 0) what += " · X.25 / ATN " + std::to_string(ni) + " bytes";
        st.last = what;
        if (host.verbose()) host.event(arrow + sid + " → " + dname + " · " + what, 5);
        row(sid);
    }

    void row(const std::string& id) {
        auto it = st_.find(id);
        if (it == st_.end()) return;
        const Station& s = it->second;
        host.table_row(id, {id, s.ground ? "Ground" : "Aircraft", s.ground ? s.coverage : s.name, s.flight,
                            std::to_string(s.frames), s.last.substr(0, 200)});
        std::vector<std::pair<std::string, std::string>> det = {{"Last frame", s.kind}};
        if (std::isfinite(s.lat)) {
            char p[48];
            snprintf(p, sizeof p, "%.1f, %.1f (to 0.1 degree)", s.lat, s.lon);
            det.push_back({"Position", p});
        }
        if (!s.last.empty()) det.push_back({"Last", s.last.substr(0, 400)});
        host.table_detail(id, det);
        if (st_.size() > 3000) st_.erase(st_.begin());   // a long run: bounded
    }

    void map_point(const std::string& id, const Station& s) {
        if (!std::isfinite(s.lat)) return;
        kp::MapPoint p;
        p.id = id;
        p.lat = s.lat;
        p.lon = s.lon;
        p.label = s.coverage.empty() ? id : s.coverage;
        p.kind = "station";
        char pos[64];
        snprintf(pos, sizeof pos, "%.1f, %.1f (to 0.1 degree, from its XID)", s.lat, s.lon);
        p.info = "VDL2 ground station: " + id.substr(3) + (s.coverage.empty() ? "" : "\nAirport coverage: " + s.coverage) +
                 "\nPosition: " + pos;
        p.ttl_s = 3600;
        host.map_point(p);
    }
};

}  // namespace

KRAKEN_PLUGIN(Vdl2, {.id = "vdl2",
                     .name = "VDL Mode 2 (aircraft datalink)",
                     .category = "Aviation",
                     .description = "VHF Digital Link Mode 2 (136.x MHz, D8PSK 31.5 kbit/s): ACARS over AVLC, "
                                    "aircraft ICAO addresses, ground stations; a DoA per aircraft and station",
                     .version = "1.0",
                     .sample_rate = FS,
                     .min_vfo_rate = 25000,
                     .author = "KrakenRF",
                     .map = true,
                     .talkers = true})
