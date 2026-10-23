#pragma once

// Audio-band signalling under analog FM (the 12 kHz audio rx.cpp makes from
// the discriminator, in Hz of deviation): FFSK data bursts - MDC1200 and
// FleetSync (1200 / 2400 bit/s) - and tones: 5-tone selcall, DTMF, tone
// bursts (1750 Hz). Reports what it decoded with the audio sample span it
// came from (decoder.cpp maps that to host time).

#include <complex>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "kraken_fft.hpp"

#include "fleetsync.hpp"

namespace asig {

constexpr double FS_AU = 12000.0;
using cf = std::complex<float>;

struct SigEvent {
    enum Kind { MDC, FLEETSYNC, SELCALL, DTMF, TONE } kind = MDC;
    double ka = 0, kb = 0;          // first / last audio sample of the burst / sequence
    float dev_hz = 0;               // its deviation (peak, Hz)
    // MDC1200
    uint8_t op = 0, arg = 0;
    uint16_t unit = 0;
    bool dbl = false, dbl_ok = false;   // a double packet, its second block decoded
    uint8_t ex[4] = {};
    int fixed = 0;                  // bits the convolutional code corrected
    // FleetSync
    fleetsync::Message fs;
    // selcall / DTMF / tone burst
    std::string std_name;           // "CCIR1", "ZVEI1/ZVEI2/PZVEI" (selcall)
    std::string family;             // "CCIR"
    std::string digits;             // "04221", DTMF "123#"
    std::string alt;                // selcall: another standard's reading of the same tones
    float hz = 0;                   // tone burst
    float tone_ms = 0;              // selcall: measured tone length
};

// two sliding one-bit tone correlators per sample, kept in a ring
class Ffsk {
public:
    void init(double fa, double fb, int spb);
    void push(float x);
    int64_t end() const { return n_; }   // samples pushed (index of the next)
    bool has(double n) const { return n >= static_cast<double>(n_ - R + 2) && n <= static_cast<double>(n_ - 1); }
    // the correlators of the window ending at sample n (linear between samples)
    cf a(double n) const;
    cf b(double n) const;
    // tone soft value: +1 = tone a, -1 = tone b (each tone's magnitude
    // divided by ga / gb: the transmitter's pre-emphasis tilt)
    float q(double n, float ga = 1, float gb = 1) const;
    int spb() const { return spb_; }

private:
    static constexpr int R = 16384;
    int spb_ = 10;
    std::vector<cf> pa_, pb_, ca_, cb_;
    cf sa_{0, 0}, sb_{0, 0}, la_{1, 0}, lb_{1, 0}, wa_{1, 0}, wb_{1, 0};
    int64_t n_ = 0;
    cf at(const std::vector<cf>& v, double n) const;
};

class Sig {
public:
    Sig();
    ~Sig();
    Sig(const Sig&) = delete;
    Sig& operator=(const Sig&) = delete;
    void reset();
    void process(const float* a, size_t n);   // 12 kHz audio in Hz
    std::vector<SigEvent> events;             // filled by process(), taken by the caller
    bool dump = false;                        // ANALOG_ID_DUMP (offline diagnostics)
    bool want_mdc = true, want_fs = true, want_selcall = true, want_dtmf = true, want_burst = true;
    std::string sel_family;                   // selcall: only this family ("ZVEI", "CCIR" ...), "" = any

private:
    int64_t k_ = 0;                           // audio samples since reset
    double hp_b_[3] = {}, hp_a_[3] = {}, hp_z_[2] = {};   // 300 Hz high-pass for the FFSK correlators

    // ---- FFSK data bursts ----
    Ffsk f12_;                                // 1200 / 1800 Hz at 1200 bit/s (MDC1200, FleetSync)
    Ffsk f24_;                                // 1200 / 2400 Hz at 2400 bit/s (FleetSync)
    struct Peak {
        bool active = false;
        float best = 0;
        int64_t idx = 0;
        int age = 0;
    };
    struct Job {
        int kind;                             // 0 MDC, 1 FleetSync 1200, 2 FleetSync 2400
        double e0;                            // window end of the first bit after the sync (fractional)
        int64_t sync_end;
        // FleetSync, decoded block by block as the samples come
        int block = 0;
        float ga = 1, gb = 1;
        fleetsync::Message msg;
        // FleetSync II: the 16-bit quarters of the block being collected
        int fs2_n = 0;
        uint16_t fs2_q[4] = {};
        double ka = 0;
        float dev = 0;
        // FleetSync coherent reading: the expected correlator output of
        // each tone (learnt from the sync) and the phase parity at the
        // next block's first bit; bit index from the sync's first bit
        cf ra{0, 0}, rb{0, 0};
        int par = 0;
        int64_t bit = 0;
        int nref = 0;
    };
    Peak pk_mdc_, pk_fs12_, pk_fs24_;
    std::vector<Job> jobs_;
    double last_mdc_k_ = -1e9, last_fs_k_ = -1e9;
    void search(const Ffsk& f, const std::vector<int8_t>& p1, const std::vector<int8_t>* p2, int max_err, Peak& pk,
                int kind);
    bool run_mdc(Job& j);
    int run_fs(Job& j);                       // 1 = done, 0 = needs more samples
    double sync_start(const Ffsk& f, double e0, int sync_bits, int pre_tone) const;
    float balance(const Ffsk& f, double first, const std::vector<int8_t>& p) const;
    float deviation(double ka, double kb) const;

    // ---- tones: an FFT of the last 20 ms every 5 ms ----
    static constexpr int WIN = 240, HOP = 60, NFFT = 512;
    static constexpr int ARING = 32768;
    std::vector<float> au_;                   // audio ring (deviation, Hz)
    float* fft_in_ = nullptr;
    kfft_complex* fft_out_ = nullptr;
    kfft_plan plan_ = nullptr;
    std::vector<float> win_;
    float win_sum_ = 0;
    int64_t frame_ = 0;
    struct Frame {
        int64_t k_end;                        // last audio sample of the window
        float hz = 0, amp = 0, purity = 0;    // the strongest single tone
        int dtmf = -1;                        // DTMF digit index 0..15, -1 = none
        float dtmf_amp = 0, dtmf_lo = 0, dtmf_hi = 0;   // its level, its two tones' measured frequencies
    };
    void frame(int64_t k_end);
    bool voice_family(const float* P, float flo, float fhi, double pair) const;
    // single-tone segments -> sequences (selcall) / long tones (bursts)
    struct Seg {
        double f = 0, w = 0;                  // power-weighted frequency
        int64_t k0 = 0, k1 = 0;               // first / last frame's window end
        int frames = 0;
        float amp = 0, fmin = 1e9f, fmax = 0;
        double dur_s() const;
    };
    bool seg_on_ = false;
    Seg seg_;
    std::vector<Seg> seq_;
    int64_t seq_last_k_ = 0;
    void seg_close();
    void seq_eval(bool all);
    // DTMF digits -> strings
    int dt_cur_ = -1, dt_run_ = 0, dt_off_ = 0, dt_acc_ = -1;
    float dt_lo0_ = 0, dt_hi0_ = 0;           // the run's first frame's tones (a DTMF tone holds still)
    int dt_longest_ = 0;                      // the latest digit's frames
    bool dt_in_ = false;
    std::string dt_str_;
    double dt_ka_ = 0, dt_kb_ = 0;
    float dt_amp_ = 0;
    int64_t dt_last_k_ = 0;
    void dtmf_frame(const Frame& f);
    void dtmf_flush();
};

}  // namespace asig
