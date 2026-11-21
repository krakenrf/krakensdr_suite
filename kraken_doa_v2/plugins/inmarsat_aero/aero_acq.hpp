#pragma once

// Carrier and bit-rate acquisition for the continuous Aero channels.
//
// A-BPSK turns the carrier by +-90 deg per bit and A-QPSK is offset QPSK:
// the squared signal of either has two spectral lines, at 2 fc - R/2 and
// 2 fc + R/2 (R = bit rate) - whatever the data. A pair of lines R apart
// gives the rate and their mid point twice the carrier offset, in one
// search and without timing or phase. The squared signal's spectrum is
// averaged over a few FFTs (non-coherently) and every candidate rate's
// pair is scored by its weaker line over the median (noise) level.
//
// Two searches run side by side: the wide one at the input rate (24 kHz:
// 4.8 / 8.4 / 10.5 kbit/s) and a narrow one at 12 kHz behind a +-4 kHz
// low-pass (0.6 / 1.2 / 2.4 kbit/s: 6.6 dB less noise, 13 dB more line
// over noise after squaring).

#include "kraken_dsp.hpp"
#include "kraken_fft.hpp"

#include <complex>
#include <vector>

namespace aero {

using cf = std::complex<float>;

struct AcqHit {
    double rate = 0;     // bit/s
    double f0 = 0;       // carrier offset (Hz) at the input
    float snr_db = 0;    // weaker line over the median of the squared spectrum
    float bw = 0;        // not used
};

class Acquisition {
public:
    // fs: input rate; rates_wide / rates_narrow: the candidates; max_off:
    // the carrier offsets searched (+- Hz); navg: FFTs averaged
    Acquisition(double fs, std::vector<double> rates_wide, std::vector<double> rates_narrow, double max_off, int navg);
    ~Acquisition();
    Acquisition(const Acquisition&) = delete;
    Acquisition& operator=(const Acquisition&) = delete;
    void reset();
    // returns true when a new estimate is ready (best())
    bool process(const cf* x, size_t n);
    const std::vector<AcqHit>& hits() const { return hits_; }   // every candidate's best pair
    double seconds_per_result() const;

private:
    struct Band {
        double fs = 0;
        int n = 0;           // FFT size
        int navg = 0;
        std::vector<double> rates;
        kfft_complex* in = nullptr;
        kfft_complex* out = nullptr;
        kfft_plan plan = nullptr;
        std::vector<float> acc;
        int fill = 0, nacc = 0;
        bool ready = false;
    };
    double fs_, max_off_;
    Band wide_, narrow_;
    // narrow front end: low-pass + decimate by 2
    std::vector<float> hlp_, hwide_;
    std::vector<cf> lbuf_, wbuf_;
    int lpos_ = 0, lph_ = 0, wpos_ = 0;
    std::vector<AcqHit> hits_;
    void setup(Band& b, double fs, int n, int navg, std::vector<double> rates);
    void push(Band& b, cf v);
    void search(Band& b);
};

}  // namespace aero
