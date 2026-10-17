#pragma once

// Sub-audible signalling receiver: FM discriminator -> the 0-300 Hz band at
// 1200 Hz -> a CTCSS tone detector / tracker and a DCS (134.4 bit/s Golay)
// receiver. Reports transmissions (the tone / code, where it began, how far
// it is confirmed, how it ended) as events for decoder.cpp.

#include <complex>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "kraken_fft.hpp"

#include "kraken_plugin.hpp"
#include "tones.hpp"

namespace ctcss {

constexpr double FS_IN = 24000.0;    // plugin input (complex)
constexpr int DECIM = 20;            // -> 1200 Hz sub-audio stream
constexpr double FS_SUB = FS_IN / DECIM;

// One transmission's news (Rx::events)
struct RxEvent {
    enum Type { START, PROGRESS, END, NOTE } type = START;   // NOTE: news about the ended one (DCS turn-off code)
    bool dcs = false;
    std::string id;            // "CTCSS 88.5" / "DCS 023N"
    std::string code;          // "88.5" / "023N"
    bool standard = true;      // a listed tone / code
    float hz = 0;              // CTCSS: measured frequency
    float dev_hz = 0;          // tone deviation (peak, Hz)
    DcsName dcs_name;          // DCS: the code and its other-polarity equivalent
    double t0 = 0;             // START: where the tone / code began (host time)
    double t1 = 0;             // confirmed up to (START / PROGRESS) / ended (END)
    std::string why;           // END: "reverse burst 180°", "tone off", "turn-off code" ...
};

struct RxParams {
    float min_dev_hz = 80;     // weakest tone deviation taken as a tone
    bool nonstandard = true;   // CTCSS tones off the list (measured frequency)
    bool invert = false;       // the input is mirrored (Host::rf_inverted)
};

class Rx {
public:
    Rx();
    ~Rx();
    Rx(const Rx&) = delete;
    Rx& operator=(const Rx&) = delete;
    void reset(double t0);                 // t0 = host time of the next input sample
    void process(const kp::cf* x, size_t n);
    RxParams p;
    std::vector<RxEvent> events;           // filled by process(), taken by the caller
    // diagnostics (CTCSS_DUMP=1 offline)
    bool dump = false;
    struct Bit { uint8_t b; float soft; double jc; };

private:
    // ---- front end ----
    double t0_ = 0;
    int64_t nin_ = 0;                      // input samples since reset
    std::vector<float> h_ch_, h_a_, h_b_, h_dcs_, h_trk_;
    std::vector<kp::cf> ch_ring_;          // channel filter (doubled ring)
    size_t ch_pos_ = 0;
    kp::cf prev_{1, 0};
    std::vector<float> a_ring_;            // discriminator -> stage A (24 kHz -> 2400 Hz)
    size_t a_pos_ = 0;
    int a_phase_ = 0;
    std::vector<float> b_ring_;            // stage B (2400 -> 1200 Hz)
    size_t b_pos_ = 0;
    int b_phase_ = 0;
    // HP biquad for the CTCSS path (removes the carrier offset)
    double hp_b_[3] = {}, hp_a_[3] = {}, hp_z_[2] = {};
    // sub-audio rings (index j = 1200 Hz samples since reset)
    static constexpr int RING = 4096;
    std::vector<float> hp_;                // high-passed (CTCSS)
    std::vector<float> dc_ring_;           // DCS filter input
    size_t dc_pos_ = 0;
    std::vector<float> y_;                 // DCS-filtered stream (ring by index)
    std::vector<float> cpw_;               // carrier power per sub-audio sample (|channel output|^2, aligned)
    std::vector<float> cblk_;              // per 20 input samples, before the alignment delay
    double cacc_ = 0;
    int cacc_n_ = 0;
    int64_t cblk_n_ = 0;
    float carrier_avg(int64_t jc, int half) const;   // mean carrier power over jc +- half
    int64_t carrier_onset(int64_t jc, int64_t lo) const;
    std::vector<float> ns_seen_;           // non-standard tones heard (kept across resets: stable ids)
    int64_t j_ = 0;                        // next sub-audio index
    double time_of(double j) const;        // sub-audio sample j -> host time
    void sub_sample(float s);

    // ---- CTCSS ----
    struct Spec {
        bool cand = false;
        int len = 0;                       // samples analysed (the window after a carrier's onset)
        float hz = 0, amp = 0, snr_db = 0, purity = 0;
        int64_t j_end = 0;
    };
    float* fft_in_ = nullptr;
    kfft_complex* fft_out_ = nullptr;
    kfft_plan plan_ = nullptr;
    std::vector<float> win_, med_;
    float win_sum_ = 0;
    std::deque<Spec> hops_;                // recent hop results
    Spec analyse(int64_t j_end);
    void ctcss_hop();
    void ctcss_tick();
    std::complex<float> track_z(int64_t j_now, double hz) const;
    enum class CState { IDLE, LOCKED, TAIL } cs_ = CState::IDLE;
    float c_hz_ = 0;                       // the tone (listed value or the measured one)
    float lock_hz_ = 0, lock_spread_ = 0;  // the candidates' frequency at lock and their spread
    double trk_hz_ = 0;                    // tracker's frequency
    double th_ref_ = 0;                    // tracker phase at j_ref_ (phase-continuous frequency updates)
    int64_t j_ref_ = 0;
    double trk_enbw_ = 30;                 // the tracker filter's noise bandwidth (Hz)
    float a_ref_ = 0;                      // tone amplitude reference (Hz)
    float cl_ref_ = 0;                     // carrier power reference while locked
    int cl_low_ = 0;                       // ticks with the carrier gone
    std::string c_code_;
    bool c_std_ = true;
    int64_t c_start_j_ = 0;                // where the transmission began (sub-audio index, centre convention)
    int64_t c_lock_j_ = 0;
    double c_meas_ = 0;                    // measured frequency
    double meas_ph_ = 0, meas_t_ = 0;      // the tone's phase advance (rad) over the ticks' time (s)
    std::vector<float> meas_f_;            // each tick's own frequency (the median: a robust check)
    double meas_med_ = 0;
    struct MeasSnap { int64_t jc; double ph, t; size_t nf; };
    std::deque<MeasSnap> meas_snap_;       // the last ticks' sums (a reverse burst's step is taken back out)
    struct Tick { int64_t jc; float amp, ph; };
    std::deque<Tick> ticks_;
    int dip_k_ = -1;                       // ticks_ index where a dip started (-1 = none)
    int64_t rev_j_ = -1;                   // a phase reversal (reverse burst?) waiting for the end (-1 = none)
    int64_t rev_seen_j_ = 0;
    float rev_deg_ = 0;
    float ph_before_ = 0;
    int64_t tail_until_j_ = 0;
    float block_hz_ = 0;                   // a just-ended tone: no new lock on it until block_j_
    int64_t block_j_ = 0;
    int switch_n_ = 0;
    float switch_hz_ = 0;
    int64_t switch_j_ = 0;
    int64_t last_end_j_ = -1000000;        // the end of the last transmission (either kind)
    int64_t tick_n_ = 0;
    void ctcss_start(const Spec& s, int64_t j_now);
    void ctcss_end(int64_t j_end, const std::string& why, bool tail);
    void ctcss_progress(int64_t j_conf);

    // ---- DCS ----
    double d_tau_ = 0;                     // next bit centre (y index)
    double d_sps_ = FS_SUB / 134.4;
    float d_hi_ = 300, d_lo_ = -300;
    double d_mean_ = 0;
    float d_prev_ = 0;
    bool d_have_prev_ = false;
    std::deque<Bit> bits_;                 // recent bits (time order)
    int64_t bit_n_ = 0;                    // bits since reset (bits_.back() is bit_n_ - 1)
    bool d_locked_ = false;
    uint32_t d_pat_ = 0;                   // locked: the pattern, aligned so bit k has E[k mod 23]
    uint8_t d_exp_[23] = {};
    DcsName d_name_;                       // the locked / last code
    int64_t d_lock_bit_ = 0, d_start_bit_ = 0;
    int64_t d_last_good_ = 0;              // the last bit index confirmed
    int64_t d_end_j_ = -1000000;           // where the last DCS transmission ended (sub-audio index)
    double d_dev_ = 0;
    int d_turnoff_ = 0;                    // ticks of a 134.4 Hz turn-off tone seen after the end
    void dcs_sample(int64_t jy);
    void dcs_bit(uint8_t b, float soft, double jc);
    bool dcs_try_lock();
    void dcs_end(const std::string& why);
    bool dcs_recent(int64_t j) const { return d_locked_ || j - d_end_j_ < static_cast<int64_t>(0.8 * FS_SUB); }
};

}  // namespace ctcss
