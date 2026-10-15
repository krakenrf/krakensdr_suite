// LoRa CSS demodulator: a streaming per-channel receiver at an oversampled
// working rate (OS x BW). It dechirps symbol-length windows, finds the
// preamble (repeated base up-chirps) cheaply, reads the sync word and the
// SFD down-chirps to split the integer CFO / STO, corrects fractional CFO,
// demodulates the data symbols and runs the coding chain (coding.hpp) to
// recover the PHY payload bytes. Explicit and implicit headers, SF7-12,
// CR 4/5-4/8, low-data-rate optimisation.
//
// Clean-room from the public LoRa PHY literature (see coding.hpp). The
// dechirp + FFT demodulator and the up/down-chirp sync are the standard,
// widely published chirp-spread-spectrum technique.
#pragma once
#include <complex>
#include <cstdint>
#include <vector>

#include <fftw3.h>

namespace lora_phy {

using cf = std::complex<float>;

struct Frame {
    std::vector<uint8_t> payload;   // PHY payload bytes (after header, de-whitened, CRC stripped)
    int sf = 7;
    int cr = 1;
    bool has_crc = false;
    bool crc_ok = false;
    bool implicit = false;
    bool header_ok = true;
    double cfo_hz = 0;
    float snr_db = 0;
    uint8_t sync_word = 0;
    // sample span in this receiver's own stream (sample index since start)
    int64_t start_sample = 0;
    int64_t end_sample = 0;
    int nsym = 0;
};

class LoraRx {
public:
    // sf, bw, oversampling os (2); sync_word 0 = accept any; implicit header
    // needs the length / cr / crc fixed in advance.
    LoraRx(int sf, double bw, int os, uint8_t sync_word, bool implicit, int impl_len, int impl_cr, bool impl_crc,
           bool ldro_auto);
    ~LoraRx();
    LoraRx(const LoraRx&) = delete;
    LoraRx& operator=(const LoraRx&) = delete;

    double rate() const { return rate_; }
    int sf() const { return sf_; }
    double bw() const { return bw_; }

    // Feed resampled complex samples (rate() Hz). Decoded frames are appended
    // to `out`. The absolute sample index of the first sample ever pushed is
    // 0; Frame::start/end_sample are in that count.
    void push(const cf* x, size_t n, std::vector<Frame>& out);
    void reset();

private:
    int sf_, os_, impl_len_, impl_cr_;
    double bw_, rate_;
    bool implicit_, impl_crc_, ldro_auto_;
    uint8_t sync_word_;
    int N_, L_;                     // 2^sf, os*2^sf
    std::vector<cf> down_, up_;     // reference chirps (length L_)
    fftwf_complex *fin_, *fout_;
    fftwf_plan plan_;

    // input ring
    std::vector<cf> ring_;
    int64_t abs_ = 0;               // samples pushed so far
    int ring_mask_;

    // detector history
    int det_count_ = 0;
    int det_bin_ = -1;
    int64_t det_start_ = -1;        // coarse symbol start of the run
    int64_t next_eval_ = 0;         // next absolute index to dechirp at

    // one pending packet being waited on (LoRa is half-duplex per channel)
    bool pending_ = false;
    int64_t pend_start_ = 0, pend_deadline_ = 0;
    int64_t need_abs_ = 0;          // a pending decode is retried once abs_ reaches this

    // demod a symbol starting at absolute index `a` with the down/up chirp;
    // returns the argmax bin (0..N-1, aliased) and fills pk/mean power.
    int demod_bin(int64_t a, bool use_up, float* pk, float* mean, float* frac, double derot_bins = 0);
    void get_window(int64_t a, cf* w);    // copy L_ samples at abs index a
    // 1 = decoded (out appended), 0 = failed/not a packet, 2 = need more samples
    int decode_from(int64_t preamble_start, std::vector<Frame>& out);
};

}  // namespace lora_phy
