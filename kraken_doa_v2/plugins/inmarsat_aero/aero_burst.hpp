#pragma once

// R and T channel bursts (aircraft -> GES, 1626.5 - 1660.5 MHz): ICAO
// Annex 10 Vol III 4.4.3 / 4.4.4, figures 4-6 and 4-8, tables 4-12 /
// 4-13. Received directly from an aircraft in line of sight - the Aero
// transmissions with a transmitter worth a bearing.
//
// A burst: preamble (unmodulated carrier, then alternating bits: 150 + 74
// bit periods at 600, 126 + 74 at 1200, 78 + 74 at 2400, 248 + 256 at
// 10500 bit/s), unique word (the P channel's; A-QPSK: on I and Q), then
// the information field, coded (encoder reset per burst) and interleaved:
// - R (random access): one block of 64 x 5 = one 19-octet signal unit
//   (152 bits, scrambled) + 8 flush bits;
// - T (reserved TDMA): a 48-bit burst identifier (AES ID, GES ID, CRC),
//   n signal units of 96 bits (the P channel formats) and 16 flush bits;
//   blocks of 5 then 3 columns (A-BPSK) or one block of 2 + 3n columns
//   (10.5 kbit/s).
//
// Detection: the unmodulated preamble is a tone - a short FFT finds it
// (two FFTs in a row, a peak well over the median); the burst lasts while
// the band around it stays above the noise. Each burst is then demodulated
// from its start at every burst rate (four starting timing phases each)
// until a unique word and a checked signal unit come out. Every burst that
// passed its CRC is a talker packet of its AES (its 24-bit ICAO address).

#include "aero_demod.hpp"
#include "aero_su.hpp"
#include "kraken_fft.hpp"
#include "kraken_plugin.hpp"

#include <complex>
#include <cstdlib>
#include <cstdint>
#include <deque>
#include <vector>

namespace aero {

class BurstRx {
public:
    BurstRx(kp::Host& h, SuDecoder& su);
    ~BurstRx();
    BurstRx(const BurstRx&) = delete;
    BurstRx& operator=(const BurstRx&) = delete;
    void reset();
    // input at 24 kHz; t0 = host time of x[0]
    void process(const cf* x, size_t n, double t0);
    uint64_t bursts() const { return bursts_; }

private:
    kp::Host& host;
    SuDecoder& su_;
    static constexpr int NFFT = 512, HOP = 256;
    kfft_complex* in_ = nullptr;
    kfft_complex* out_ = nullptr;
    kfft_plan plan_ = nullptr;
    std::vector<float> win_, pw_;
    std::deque<cf> ring_;          // recent input
    int64_t ring_base_ = 0;        // absolute index of ring_[0]
    int64_t nin_ = 0;              // samples in so far
    int64_t next_fft_ = NFFT;      // absolute index where the next FFT ends
    double t0_base_ = 0;           // host time of sample 0
    bool tbase_ = false;
    // the burst being followed
    bool active_ = false;
    int tone_bin_ = -1, tone_hits_ = 0, quiet_ = 0;
    int64_t start_ = 0, tone_seen_ = 0;
    double f0_ = 0;
    uint64_t bursts_ = 0, decoded_ = 0;
    const bool debug_ = std::getenv("AERO_DEBUG") != nullptr;

    void fft_at(int64_t end);
    void decode(int64_t s0, int64_t s1, double f);
    bool try_rate(double rate, int64_t s0, int64_t s1, double f);
    double host_t(double s) const { return t0_base_ + s / 24000.0; }
};

}  // namespace aero
