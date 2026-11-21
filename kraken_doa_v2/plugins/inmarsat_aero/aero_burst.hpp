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
// (two FFTs in a row, a peak well over its bin's noise; a long FFT for the
// slow rates); the burst lasts while the band around it stays above the
// noise. Up to four bursts on different frequencies are followed at once. Each burst is then demodulated
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
    uint64_t bursts_ = 0, decoded_ = 0;
    // the bursts being followed (several at once, on different frequencies)
    static constexpr size_t MAX_SLOTS = 4;
    struct Slot {
        int64_t start = 0, tone_seen = 0;   // first sample; last FFT with its preamble tone
        double f0 = 0;                      // carrier (Hz)
        int bin = 0;                        // ... as a 512-point bin
        int unc = 1024;                     // how early it may have started before `start` (samples)
        float noise_w = 0;                  // the wide band's noise before it
        float noise_n = 0;                  // the narrow band's: its median bin x the bins (the tone has lifted its own bins' noise a little before it was confirmed)
        float peak_n = 0, peak_w = 0;       // the most its narrow / wide band held (smoothed)
        float peak_t = 0, peak_tl = 0;      // its carrier bin's most in the 512- / 4096-point FFT
        int quiet = 0;                      // FFTs in a row without it
    };
    std::vector<Slot> slots_;
    // tones seen in the last FFT(s), not a burst yet
    struct Pend {
        int bin, hits;
        int64_t start;
        bool hit;
    };
    std::vector<Pend> pend_, pend_l_;
    struct Tone {
        int bin;
        float ratio;   // over the threshold
        double frac;   // interpolated offset from bin
    };
    std::vector<Tone> tones_;
    std::deque<std::pair<double, uint32_t>> recent_;   // bursts decoded: unique word time, CRC key
    bool seen_before(double t_uw, uint32_t key);
    // the noise per bin (mean power; not updated inside a burst's band) and
    // the spectrum smoothed over ~4 FFTs. A per-bin noise, not the median of
    // the spectrum: an input with power in only part of the band (receiver
    // audio turned into IQ, a narrow decimator filter) has a median far
    // under the noise where the signals are
    std::vector<float> noise_, smooth_, noise_l_;
    int navg_ = 0, navg_l_ = 0;
    // the long FFT (slow rates' preambles)
    static constexpr int NL = 4096, HL = 1024;
    kfft_complex* inl_ = nullptr;
    kfft_complex* outl_ = nullptr;
    kfft_plan planl_ = nullptr;
    std::vector<float> winl_, pwl_;
    int64_t next_l_ = NL;
    void fft_long(int64_t end);
    const bool debug_ = std::getenv("AERO_DEBUG") != nullptr;

    void fft_at(int64_t end);
    void detect(int64_t end, int64_t a);
    void detect_long(int64_t end, int64_t a);
    void find_tones(const std::vector<float>& p, const std::vector<float>& noise, float extra_db, std::vector<Tone>& out) const;
    bool slot_wide(const Slot& s, int64_t now) const;
    int slot_covering(int bin, int64_t now, int skip = -1) const;
    bool under_strong(int bin, float p, bool long_fft) const;
    void open_slot(int64_t start, double f0, int unc, int64_t now);
    void end_slot(size_t i, int64_t end, bool too_long);
    void decode(int64_t s0, int64_t s1, double f, double tone_s, int unc);
    bool try_rate(double rate, int64_t s0, int64_t s1, double f, int unc);
    // decode the field after a unique word found at bit p (polarities sa / sb)
    bool decode_cand(double rate, int uwl, bool qpsk, double f, const std::vector<float>& s, const std::vector<double>& t, int64_t p,
                     float sa, float sb);
    double host_t(double s) const { return t0_base_ + s / 24000.0; }
};

}  // namespace aero
