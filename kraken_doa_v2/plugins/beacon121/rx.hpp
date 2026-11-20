#pragma once

// 121.5 / 243 MHz swept-tone beacon receiver (ELT / EPIRB / PLB homing
// signal; ITU-R M.690-3, ETSI EN 302 152-1 8.2, UK CAA Specification 16):
// AM with a full carrier, the audio tone sweeping over >= 700 Hz inside
// 300 - 1600 Hz, 2 - 4 sweeps a second.
//
// Carrier finder (whole VFO, 32 kHz): 4096-point FFT every 2048 samples
// (7.8 Hz bins, 64 ms), power averaged (~0.5 s), noise floor = the median bin
// of +-12 kHz; a carrier = a narrow line >= FIND_DB over it, seen in 3
// successive spectra, not a sideband of a stronger line (a steady tone's
// lines at fc +- f). Each carrier gets a Channel, started on the last
// BACKLOG_S of the input (a short self-test is caught from its start).
//
// Channel: mixed to 0 Hz (NCO + a carrier AFC), 31-tap low-pass, 16 kHz.
//  - carrier blocks of 20 ms: the block mean IS the carrier (sidebands >=
//    300 Hz average out) - its level, presence (pauses, keying), phase step
//    (AFC); C/N0 from it against the finder's noise floor
//  - envelope |z| per block: 2 / 98 % points A, B -> modulation factor
//    (A - B) / (A + B), share of samples over (A + B) / 2 -> duty cycle
//    (EN 302 152-1 8.2.3 / 8.2.4)
//  - spectrogram: 256-point FFT every 8 ms (62.5 Hz bins), 187.5 - 2000 Hz
//    on BOTH sides of the carrier; a cell = min(upper, lower) of the
//    normalised powers - AM sidebands are symmetric, a neighbour's are not
//  - every 0.5 s the last 6 s: each bin normalised by its median (a steady
//    tone vanishes), the autocorrelation over 0.18 - 0.8 s lags -> the sweep
//    period, refined by folding (the sharpest fold), then the folded trace
//    (audio frequency per phase of the sweep) must be lit over >= 40 % of
//    the period, monotonic (Spearman over the best circular shift) or
//    up-and-down, >= 450 Hz wide, filling its range, without jumps
//  - Morse: the carrier key and the modulation key, runs of 30 - 600 ms
//    between long states, dot / dash by their lengths
#include "kraken_plugin.hpp"
#include "kraken_fft.hpp"

#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace b121 {

constexpr int FS = 32000;            // plugin input
constexpr int DEC = 2;
constexpr int FS2 = FS / DEC;        // a channel's rate
constexpr int NF = 4096;             // finder FFT
constexpr int HOPF = 2048;
constexpr int NS = 256;              // spectrogram FFT (16 ms)
constexpr int HOPS = 128;            // 8 ms
constexpr double FRAME_S = static_cast<double>(HOPS) / FS2;
constexpr int BLK = 320;             // carrier block (20 ms at FS2)
constexpr double BLK_S = static_cast<double>(BLK) / FS2;
constexpr int BIN_LO = 3, BIN_HI = 32;   // 187.5 .. 2000 Hz
constexpr int NB = BIN_HI - BIN_LO + 1;
constexpr double BIN_HZ = static_cast<double>(FS2) / NS;
constexpr int WIN_FRAMES = 750;      // 6 s of spectrogram
constexpr int WIN_BLOCKS = 300;      // 6 s of carrier blocks
constexpr int EVAL_SAMPLES = FS2 / 2;    // a channel is evaluated every 0.5 s
constexpr int MAX_CHANNELS = 8;
constexpr double SEARCH_HZ = 12500;  // carriers within +-this of the VFO centre

using cf = std::complex<float>;

enum SweepDir { DIR_NONE = 0, DIR_DOWN = -1, DIR_UP = 1, DIR_UPDOWN = 2 };

struct Sweep {
    bool ok = false;
    std::string why;                 // why not (verbose)
    double period_s = 0, rate_hz = 0;
    double lo_hz = 0, hi_hz = 0;     // range of the folded trace (5 / 95 %)
    int dir = DIR_NONE;
    double rho = 0;                  // autocorrelation at the period
    double mono = 0;                 // |Spearman| of the trace
    double lit = 0;                  // share of the period with the tone seen
    double sym = 0;                  // share of it with the tone at the same frequency on both sides
    double peak_db = 0;              // the folded trace over the fold's noise
    std::vector<float> trace;        // Hz per phase bin of one sweep (NAN = not seen), from its start
};

struct Pause {
    double start_abs = 0;            // input sample index
    double len_s = 0;
};

// what a channel found in its last evaluation
struct Report {
    int chan = 0;                    // channel serial
    double freq_hz = 0;              // carrier, Hz from the input's 0 Hz
    double cn0 = NAN;                // dBHz (carrier, last 0.5 s)
    bool carrier = false;            // present at the end of the evaluation
    double carrier_share = 0;        // share of the last 0.5 s with the carrier on
    Sweep sweep;
    int passes = 0;                  // consecutive evaluations with a sweep (consistent period)
    double depth = NAN, duty = NAN;  // NAN = not measurable
    double eval_abs = 0;             // input index of the end of the evaluated samples
    double first_on_abs = NAN;       // the channel's first carrier block (its start)
    double last_on_abs = NAN;        // the end of its last carrier block
    std::vector<std::pair<double, double>> on_runs;   // carrier-on spans since the last report (input indexes)
    std::vector<Pause> pauses;       // carrier pauses completed since the last report (reliable ones)
    std::vector<Pause> cw;           // unmodulated-carrier periods completed since the last report
    std::string morse, morse_code;   // a Morse letter decoded since the last report
    std::string morse_key;           // "carrier" / "tone"
    double morse_unit_s = 0;
    double neighbour_hz = NAN;       // the nearest other carrier (Hz away; NAN = none)
};

// Turns runs of a key (on / off, with lengths) into Morse letters: the marks
// of 30 - 600 ms between two long runs (>= 0.6 s - the sweep, a pause, the
// steady carrier)
class RunMorse {
public:
    RunMorse(double tick_s, double smear_s) : tick_(tick_s), smear_(smear_s) {}
    // one tick; true = a letter group just ended (text() holds it)
    bool push(bool key);
    void reset();
    const std::string& text() const { return text_; }
    const std::string& code() const { return code_; }
    double unit_s() const { return unit_; }
    bool lead_on() const { return lead_on_; }   // the long run before the letter was key DOWN

private:
    double tick_, smear_;
    bool cur_ = false, seen_long_ = false, long_fired_ = false;
    int len_ = 0;
    std::vector<std::pair<bool, double>> runs_;
    std::string text_, code_;
    double unit_ = 0;
    bool lead_on_ = false;
    bool decode();
};

class Channel;

class Receiver {
public:
    Receiver();
    ~Receiver();
    void reset();
    void process(const kp::cf* x, size_t n, std::vector<Report>* out);
    uint64_t abs() const { return abs_; }
    // noise: per input sample (variance), as the finder measures it; NAN before its first spectrum
    double noise_var() const { return noise_var_; }
    int channels() const;
    bool verbose = false;
    std::vector<std::string> log;

private:
    friend class Channel;
    uint64_t abs_ = 0;
    // input ring (the backlog a new channel starts on)
    std::vector<cf> ring_;
    size_t ring_pos_ = 0;
    uint64_t ring_n_ = 0;
    // finder
    kfft_complex* fin_ = nullptr;
    kfft_complex* fout_ = nullptr;
    kfft_plan fplan_ = nullptr;
    std::vector<float> win_, avg_;
    std::vector<cf> fbuf_;
    int fpos_ = 0, fill_ = 0;
    int nspec_ = 0;
    double win2_ = 0;
    double noise_var_ = NAN;
    struct Pending { double f; int n; int miss; };
    std::vector<Pending> pending_;
    std::vector<std::unique_ptr<Channel>> ch_;
    int serial_ = 0;
    void spectrum();
    void start_channel(double f);
};

}  // namespace b121
