// LoRa CSS demodulator: a streaming per-channel receiver at an oversampled
// working rate (OS x BW). It dechirps symbol-length windows, finds the
// preamble (repeated base up-chirps) cheaply, reads the sync word and the
// SFD down-chirps to split the integer CFO / STO, corrects fractional CFO,
// demodulates the data symbols (tracking the fractional offset the sample
// clock drift adds over a long packet) and runs the coding chain
// (coding.hpp) to recover the PHY payload bytes. Explicit and implicit
// headers, SF7-12, CR 4/5-4/8, low-data-rate optimisation, normal and
// INVERTED IQ (LoRaWAN downlinks: the whole packet conjugated).
//
// Several receivers (SFs, IQ orientations) share one channel's sample ring
// (`Ring`): the channel stores each resampled sample once.
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
    bool inverted = false;          // inverted IQ (LoRaWAN downlink)
    int fec_bad = 0;                // payload codewords whose parity failed (no-CRC frames)
    int fec_total = 0;
    std::vector<uint8_t> byte_ok;   // per payload byte: both nibbles' codewords valid (parity / corrected)
    double cfo_hz = 0;
    float snr_db = 0;
    int sync_word = -1;             // measured from the two sync symbols (-1 = unclear)
    // sample span in the channel's stream (sample index since start)
    int64_t start_sample = 0;
    int64_t end_sample = 0;
    int nsym = 0;
};

// A preamble + SFD found (the header may still fail): what the plan-free
// search reports about a LoRa signal it could not decode
struct Detect {
    int64_t sfd = 0;                // SFD start (channel stream sample)
    int sync_word = -1;
    double cfo_hz = 0;
    float snr_db = 0;
    bool header_ok = false;
    int sf = 0;
    bool inverted = false;
};

// One channel's resampled samples, shared by its receivers
struct Ring {
    std::vector<cf> buf;
    int mask = 0;
    int64_t abs = 0;                // samples pushed so far
    void init(int min_len) {
        int rs = 1;
        while (rs < min_len) rs <<= 1;
        buf.assign(static_cast<size_t>(rs), cf(0, 0));
        mask = rs - 1;
        abs = 0;
    }
    void push(const cf* x, size_t n) {
        for (size_t i = 0; i < n; i++) buf[static_cast<size_t>((abs + static_cast<int64_t>(i)) & mask)] = x[i];
        abs += static_cast<int64_t>(n);
    }
    void reset() {
        std::fill(buf.begin(), buf.end(), cf(0, 0));
        abs = 0;
    }
};

class LoraRx {
public:
    // sf, bw, oversampling os (2), the shared ring, inverted IQ; implicit
    // header needs the length / cr / crc fixed in advance.
    LoraRx(int sf, double bw, int os, Ring* ring, bool invert, bool implicit = false, int impl_len = 0,
           int impl_cr = 1, bool impl_crc = true, bool ldro_auto = true);
    ~LoraRx();
    LoraRx(const LoraRx&) = delete;
    LoraRx& operator=(const LoraRx&) = delete;

    double rate() const { return rate_; }
    int sf() const { return sf_; }
    double bw() const { return bw_; }
    bool inverted() const { return invert_; }
    int window() const { return L_; }

    // Look at the samples added to the ring since the last call; decoded
    // frames are appended to `out`.
    void poll(std::vector<Frame>& out);
    void reset();
    bool pending() const { return pending_; }
    int triggers() const { return triggers_; }     // preamble candidates looked at so far

    // every preamble + SFD found is appended here (nullptr = not logged)
    std::vector<Detect>* det_log = nullptr;
    // frames without a payload CRC need (almost) every payload codeword's
    // parity to hold (frames from unknown transmitters: the plan-free search)
    bool strict_nocrc = false;

private:
    int sf_, os_, impl_len_, impl_cr_;
    double bw_, rate_;
    bool invert_, implicit_, impl_crc_, ldro_auto_;
    int N_, L_;                     // 2^sf, os*2^sf
    std::vector<cf> down_, up_;     // reference chirps (length L_)
    fftwf_complex *fin_, *fout_;
    fftwf_plan plan_;
    std::vector<cf> down1_;         // detection: one sample per chip (N points)
    fftwf_complex *fin1_, *fout1_;
    fftwf_plan plan1_;
    Ring* ring_;

    // detector history
    int det_count_ = 0;
    int det_bin_ = -1;
    int64_t det_start_ = -1;        // coarse symbol start of the run
    int64_t next_eval_ = 0;         // next absolute index to dechirp at

    // one pending packet being waited on (LoRa is half-duplex per channel)
    bool pending_ = false;
    int64_t pend_start_ = 0, pend_deadline_ = 0;
    int64_t need_abs_ = 0;          // a pending decode is retried once the ring reaches this
    int triggers_ = 0;

    // demod a symbol starting at absolute index `a` with the down/up chirp;
    // returns the argmax bin (0..N-1, aliased) and fills pk/mean power.
    int demod_bin(int64_t a, bool use_up, float* pk, float* mean, float* frac, double derot_bins = 0);
    int detect_bin(int64_t a, float* pk, float* mean);   // the cheap detector's dechirp
    // 1 = decoded (out appended), 0 = failed/not a packet, 2 = need more samples
    int decode_from(int64_t preamble_start, std::vector<Frame>& out);
};

}  // namespace lora_phy
