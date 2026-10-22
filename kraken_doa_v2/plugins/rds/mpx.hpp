#pragma once
// The FM multiplex (MPX: the discriminator output in Hz of deviation) apart
// from RDS: its averaged spectrum (pilot, stereo, subcarriers), the
// classification of the subcarriers found (SCA = subsidiary carriers:
// FM audio - reading services, background music -, FSK data, RDS2 data
// streams, DARC-like wideband data, AM / DSB), the SCA audio demodulator,
// the main programme's mono audio, and the blind array combiner (every
// antenna -> one, before the discriminator).

#include "kraken_dsp.hpp"
#include "kraken_fft.hpp"
#include "rds_demod.hpp"   // Mixer, fast_atan2

#include <liquid/liquid.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace rds {

using kp::cf;

// Averaged one-sided power spectrum of the real MPX: P[k] in Hz^2 per bin
// (a tone of amplitude A Hz sums to A^2 / 2 over its bins)
class MpxSpectrum {
public:
    ~MpxSpectrum();
    void init(double fs);
    void reset();
    void push(const float* x, size_t n);
    int bins() const { return n_ / 2 + 1; }
    double bin_hz() const { return fs_ / n_; }
    int bin_of(double hz) const { return static_cast<int>(std::lround(hz / bin_hz())); }
    const std::vector<float>& p() const { return avg_; }
    uint64_t frames() const { return frames_; }
    // sum of P over [f0, f1] Hz
    double band(double f0, double f1) const;
private:
    double fs_ = 240000;
    int n_ = 512;
    std::vector<float> win_, buf_;
    size_t fill_ = 0;
    float* in_ = nullptr;
    kfft_complex* out_ = nullptr;
    kfft_plan plan_ = nullptr;
    std::vector<float> avg_;
    float wnorm_ = 1;
    uint64_t frames_ = 0;
    unsigned skip_ = 0;
};

// A subcarrier found in the spectrum, with what the probe made of it
struct Subcarrier {
    double hz = 0;           // centre (refined by the probe)
    double width = 0;        // occupied width (Hz)
    double inj_hz = 0;       // its deviation of the main carrier (Hz; injection = / 75 kHz)
    double snr_db = 0;       // over the spectrum floor around it
    double first = 0, last = 0;
    int kind = 0;            // Kind below
    double dev_rms = 0, dev_peak = 0;   // FM: the subcarrier's own deviation (Hz)
    double baud = 0;         // FSK data: bit rate from the clock line (0 = none found)
    double tone_hz = 0;      // FM audio that is one steady tone (a test tone): its frequency
    double cv = 0;           // envelope std / mean
    int probes = 0;
    double probe_t = -1e9;
    bool announced = false;
    enum Kind { UNKNOWN = 0, FM_AUDIO, FM_DATA, FM_IDLE, RDS2, DARC, AM, RDS };
};
const char* kind_name(int k);
std::string describe(const Subcarrier& s);

// Classify a subcarrier from n MPX samples (fs) around it
void probe_subcarrier(const float* mpx, size_t n, double fs, Subcarrier* s);

// FM subcarrier -> 8 kHz audio
class ScaAudio {
public:
    ~ScaAudio();
    void init(double fs, double fc, double half_bw, double deemph_us, double full_dev);
    double fc() const { return fc_; }
    void push(const float* mpx, size_t n, std::vector<float>* pcm8k);
private:
    double fs_ = 0, fc_ = 0;
    msresamp_crcf rs_ = nullptr;
    std::vector<cf> mix_, z_;
    kp::Fir<cf> lp_;
    kp::Fir<float> alp_;
    cf prev_{1, 0};
    Mixer nco_;
    float dc_ = 0, de_ = 0, a_de_ = 0, gain_ = 1;
    int dec_ = 0;
};

// The main programme (L+R, mono) -> 8 kHz audio
class MainAudio {
public:
    ~MainAudio();
    void init(double fs, double deemph_us);
    void push(const float* mpx, size_t n, std::vector<float>* pcm8k);
private:
    msresamp_rrrf rs_ = nullptr;
    std::vector<float> in_, y_;
    kp::Fir<float> lp_;
    float de_ = 0, a_de_ = 0, dc_ = 0;
    int dec_ = 0;
};

// Every antenna -> one: the main eigenvector of the antennas' covariance
// (exponentially averaged, ~0.25 s), interpolated sample by sample between
// updates, phase-aligned to the previous one - blind maximum-ratio combining,
// no calibration needed. FM is constant-envelope: the combined signal's
// carrier-to-noise ratio is what the discriminator's threshold sees
class Combiner {
public:
    void init(int m, double fs);
    int m() const { return m_; }
    // n samples of every antenna -> y
    void push(const cf* const* x, size_t n, cf* y);
    // main eigenvalue over the mean of the others (dB): ~ the array's gain on this signal
    float ratio_db() const { return ratio_db_; }
private:
    int m_ = 0;
    double fs_ = 0;
    std::vector<std::complex<double>> r_;   // m x m (upper triangle used)
    std::vector<cf> s_;                     // the current block's sum
    std::vector<cf> w_, wn_;                // current / next weights
    int blk_ = 0, pos_ = 0;
    double alpha_ = 0.01;
    float ratio_db_ = 0;
    bool have_ = false;
    void update();
};

}  // namespace rds
