#pragma once

// 406 MHz beacon receiver: burst detector over the whole VFO (406.0 - 406.1
// MHz at 100 kHz) + the first-generation demodulator (C/S T.001 2.2: 160 ms
// unmodulated carrier, then 112 / 144 bits at 400 bit/s, biphase-L, phase
// deviation +-1.1 rad) + a detector of second-generation bursts (C/S T.018:
// DSSS OQPSK around 406.05 MHz, ~1 s, not decoded).
//
// Detector: 1024-point FFT every 512 samples (97.7 Hz bins, 5.12 ms hop),
// a gated noise floor per bin. A carrier = a bin >= START_DB over its floor;
// it is followed (+-CONT_BINS: the modulation's sidebands) until it stays
// under CONT_DB for MISS_FRAMES. A burst of 0.30 - 0.80 s is demodulated
// from the input ring (2.6 s): mixed to 0 Hz, 401-tap low-pass to +-1.5 kHz,
// decimated to 10 kHz (25 samples per bit), carrier frequency + phase from
// the unmodulated part, the bit sync + frame sync found by correlation (both
// frame sync patterns, both polarities), then bit by bit with a timing loop
// on the mid-bit transition (+-1 % bit rate) and a decision-free phase loop
// (a biphase bit's mean is the residual carrier).

#include "kraken_plugin.hpp"

#include "kraken_fft.hpp"

#include <cstdint>
#include <vector>

namespace b406 {

constexpr double FS = 100000;        // plugin input rate (the VFO's "100 kHz")
constexpr int NFFT = 1024, HOP = 512;
constexpr int DEC = 10;
constexpr double FSD = FS / DEC;     // demodulator rate, 10 kHz
constexpr double BAUD = 400;
constexpr double SPB = FSD / BAUD;   // 25 samples per bit

// One first-generation burst that passed bit + frame sync (the message is
// checked by the caller: BCH)
struct Burst {
    std::vector<uint8_t> bits;       // 144 hard bits, bit 1 of T.001 first
    std::vector<float> soft;         // per bit, +-1 nominal
    bool self_test = false;          // frame sync 011010000
    int sync_errors = 0;             // bit sync + frame sync bits wrong
    bool inverted = false;           // demodulated with the opposite phase sense
    double freq_hz = 0;              // carrier, Hz from the input's 0 Hz
    double t_carrier = 0;            // input sample index (absolute) of the carrier's start (= modulation - 160 ms)
    double t_mod = 0;                // ... of the first bit
    double bit_s = 1 / BAUD;         // measured bit period
    double level_db = 0;             // carrier power, dB (input full scale = 0)
    double snr_db = 0;               // carrier over the noise in a 400 Hz band
    double ber_q = 0;                // soft bit quality: mean |soft| / std
    double tail_q = 0;               // mean |soft| of bits 113-144 (~0 = the burst ended at bit 112)
};

// A second-generation (spread spectrum) burst
struct WideBurst {
    double t0 = 0, t1 = 0;           // absolute input sample indexes
    double centre_hz = 0;            // Hz from the input's 0 Hz
    double bw_hz = 0;                // occupied (where the excess is)
    double snr_db = 0;               // in-band
};

class Receiver {
public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;
    void reset();
    // sgb_hz: where 406.05 MHz is in the input (Hz from 0 Hz)
    void set_sgb_centre(double hz) { sgb_hz_ = hz; }
    void process(const kp::cf* x, size_t n, std::vector<Burst>* out, std::vector<WideBurst>* wide);
    uint64_t abs() const { return abs_; }
    int carriers_seen() const { return carriers_; }   // candidates demodulated
    bool verbose = false;
    std::vector<std::string> log;    // debug lines (verbose / offline)

private:
    struct Cand {
        int64_t f0 = 0, f1 = 0;      // first / last active frame
        double bin = 0;              // carrier bin (refined while it is unmodulated)
        double wb = 0, ws = 0;       // weighted bin sum / weight (first frames)
        float peak = 0;              // max power ratio
        int miss = 0;
        int hits = 0;                // frames >= START_DB (confirmation: 3 of the first 4)
        float bestp = 0;             // the strongest frame's power at the carrier (its bin is c.bin)
        bool steady = false;         // longer than any burst: a carrier, not a beacon
    };
    std::vector<kp::cf> ring_;       // RING input samples
    uint64_t abs_ = 0;               // input samples since reset
    int64_t frame_ = 0;              // frames computed
    float* win_;
    kfft_complex *fin_, *fout_;
    kfft_plan plan_;
    std::vector<float> pw_, nf_;     // power per bin (bin b at index b + NFFT/2), floor
    std::vector<uint8_t> claimed_;
    std::vector<uint8_t> claimed_conf_;   // ... by confirmed carriers only (the SGB detector: an unbiased band)
    std::vector<Cand> cands_;
    std::vector<std::pair<uint64_t, Cand>> jobs_;   // demodulated once the input reaches .first
    std::vector<float> lp_;          // channel filter (FS)
    double sgb_hz_ = 0;
    int carriers_ = 0;
    // second-generation detector
    double band_floor_ = 0, out_floor_ = 0;   // the SGB band / the band edges (reference)
    int band_init_ = 0;
    bool wide_on_ = false;
    int64_t wide_f0_ = 0, wide_f1_ = 0;
    int wide_miss_ = 0;
    std::vector<double> wide_sub_;   // excess per sub-band during the burst
    double wide_excess_ = 0, wide_out_ = 0;
    std::vector<float> band_hist_, out_hist_;
    std::vector<float> med_b_, med_e_;   // scratch

    void frame(std::vector<Burst>* out, std::vector<WideBurst>* wide);
    void wide_frame(std::vector<WideBurst>* wide);
    bool demod(const Cand& c, Burst* b);
    // input sample (absolute index) -> its place in the ring
    kp::cf at(uint64_t i) const;
};

}  // namespace b406
