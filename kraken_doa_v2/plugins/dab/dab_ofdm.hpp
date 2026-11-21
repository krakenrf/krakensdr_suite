#pragma once
// The DAB mode I OFDM front end at 2.4 MHz, without a resampler.
//
// At 2.4 MHz a useful symbol (Tu = 1 ms) is exactly 2400 samples, so the
// carrier spacing is one bin of a 2400-point FFT. The guard interval, the
// symbol and the null symbol are not whole samples (590.625 / 2990.625 /
// 3112.5), but the 96 ms frame is (230400). Each symbol's FFT window starts
// at a whole sample inside its guard interval; the window's sub-sample
// offset from the ideal start (different for every symbol) is a linear phase
// over the carriers that is taken out before the differential demodulation.
//
// Carrier frequency offset: the fractional part (|f| < 500 Hz) is removed by
// derotating each window's samples, with the phase running on the absolute
// sample index inside the frame (so the symbol-to-symbol phase stays
// right); the whole-kHz part N is a bin shift plus a per-window phase
// 2 pi f (w - frame start) / fs, which takes out the 2 pi N Tg / Tu turn
// between symbols as well. Acquisition searches +-410 kHz.
//
// Synchronisation: the null symbol (a 2880-sample power dip, found twice 96
// ms apart), the cyclic prefix correlation (fractional CFO), the phase
// reference symbol (whole-kHz CFO from its differential correlation, then
// the channel impulse response: the frame timing = the first path within 15
// dB of the strongest, with a sub-sample estimate). Tracked frame by frame
// (timing: proportional + integral on the sample clock; CFO from the
// cyclic prefixes of the whole frame).
//
// Antenna 0 is demodulated. With several antennas (process_multi) every
// antenna's samples are kept for a while, and the null symbol (TII) is
// transformed on each of them.

#include "dab_tables.hpp"
#include "kraken_fft.hpp"

#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dab {

using cf = std::complex<float>;

constexpr int MAX_ANT = 8;

struct Frame {
    int64_t start = 0;            // frame start (first sample of the null symbol), input sample index
    double start_exact = 0;
    double prs_metric = 0;        // normalised differential correlation with the phase reference (0..1)
    bool prs_ok = false;          // the phase reference was found (prs_metric above the threshold): the
                                  // soft bits are usable; false = the frame's MSC bits are erasures
    // the frame start measured at the phase reference minus where the tracking
    // expected it (samples; NAN = not measured). Large = the input jumped (a
    // gap or a timing jump since the previous frame)
    double timing_err = 0;
    double snr_db = 0;            // signal / noise per carrier (PRS power vs the null symbol's empty carriers)
    double mer_db = 0;            // modulation error ratio of the FIC symbols (phase error vs the QPSK points)
    double cfo_hz = 0;            // carrier offset of the ensemble's centre from the input's 0 Hz
    double clock_ppm = 0;         // sample clock offset (tracked frame timing)
    const int8_t* fic = nullptr;  // 3 x 3072 soft bits (symbols 2..4)
    const int8_t* msc = nullptr;  // 72 x 3072 soft bits (symbols 5..76), symbol l at (l - 5) * 3072
    const uint8_t* msc_ok = nullptr;   // [72]: soft bits of that symbol computed
    // the null symbol's last 1 ms (TII) per antenna: K carriers each (dense index, carrier_index())
    int null_m = 0;
    const cf* null_car[MAX_ANT] = {};
    int64_t null_w0 = 0;          // the window's first sample (input index); 2400 samples
    double null_noise = 0;        // per-carrier noise power estimate in the null window (antenna 0)
};

class OfdmRx {
public:
    OfdmRx();
    ~OfdmRx();
    OfdmRx(const OfdmRx&) = delete;
    OfdmRx& operator=(const OfdmRx&) = delete;

    void reset();
    // m = 1: antenna 0 only (process()); m >= 2: every antenna (process_multi())
    void push(const cf* const* x, int m, size_t n);
    // called for every frame decoded (inside push)
    std::function<void(const Frame&)> on_frame;
    // called when sync is found / lost (inside push)
    std::function<void(bool locked)> on_sync;
    // debug lines (acquisition), nullptr = none
    std::function<void(const std::string&)> log;
    // the MSC symbols the caller needs next frame (l = 5..76): soft bits are
    // computed only for those
    void want_symbols(const std::vector<uint8_t>& want72) { want_ = want72; }

    bool locked() const { return state_ == TRACK; }
    int64_t samples_in() const { return written_; }
    double cfo_hz() const { return f_tot_; }

private:
    enum State { SEARCH, TRACK };
    State state_ = SEARCH;

    // rings: antenna 0 2^20 samples, the others 2^19
    static constexpr int RING0_LOG2 = 20, RINGK_LOG2 = 19;
    std::vector<cf> ring_[MAX_ANT];
    int64_t valid_from_[MAX_ANT] = {};   // first input index held for antenna k (INT64_MAX = none)
    int m_ = 1;                          // antennas in the latest push
    int64_t written_ = 0;                // input samples so far

    // tracking state
    int64_t search_from_ = 0;
    double s0_ = 0;                      // next frame's start (exact)
    double eps_ = 0;                     // sample clock offset (fraction)
    double f_tot_ = 0;                   // CFO estimate, Hz
    int nbin_ = 0;                       // whole-kHz part (bins)
    int bad_frames_ = 0, nbin_votes_ = 0, nbin_cand_ = 0;

    // FFT
    kfft_complex* fin_ = nullptr;
    kfft_complex* fout_ = nullptr;
    kfft_plan plan_ = nullptr;           // forward 2400
    kfft_plan iplan_ = nullptr;          // backward 2400 (impulse response)
    std::vector<cf> rot_;                // e^(-j 2 pi f m / fs), m = 0..2399, for f_rot_
    double f_rot_ = 1e9;

    // per frame buffers
    std::vector<cf> car_[L + 1];         // carriers of symbol l (1..76), dense index
    std::vector<uint8_t> have_car_;
    std::vector<int8_t> fic_, msc_;
    std::vector<uint8_t> msc_ok_, want_;
    std::vector<cf> null_car_[MAX_ANT];
    std::vector<cf> prs_;                // z_1,k (dense index)
    std::vector<int> prs_q_;             // quarter turns
    std::vector<int> fidx_;              // QPSK symbol n -> dense carrier index
    std::vector<cf> work_;
    std::vector<float> e_;               // search: block energies
    std::vector<float> hann_, cir_, pbuf_;
    std::vector<cf> dbuf_;

    void copy_window(int ant, int64_t w, cf* dst) const;   // 2400 samples
    bool have(int ant, int64_t from, int64_t to) const;
    void set_rot(double f);
    // FFT of the window at w (antenna ant) with the current CFO -> carriers (dense), corrected for
    // the window's offset delta = w - ideal useful start, and the common phase vs frame start fs0
    void symbol_carriers(int ant, int64_t w, double delta, int64_t fs0, cf* out);
    void fft_window(int ant, int64_t w);   // -> fout_ (bins, derotated by the fractional CFO)
    bool search();
    bool acquire(double s0c);
    void track_frame();
    double cp_cfo(double s0, double* quality) const;
    double prs_corr(int nbin, double* norm) const;   // on fout_
    bool impulse_timing(double* tau_first, double* tau_peak);   // on fout_ with nbin_
    double cir_quality(const std::vector<cf>& spec, int nb);    // impulse response peak / mean
    void soft_bits(const cf* cur, const cf* prev, int8_t* out, double* mer_acc, int* mer_n);
    double useful_start(int l) const;   // relative to the frame start, with eps_
};

}   // namespace dab
