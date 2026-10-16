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
#include <cstdlib>
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
    // separation by code (Despreader): each de-spread symbol's value on every
    // antenna - sym_y[i * ant + k] - and how many of the packet's symbols there
    // were to take them from (ant 0 = none)
    int ant = 0;
    std::vector<cf> sym_y;
    int sym_total = 0;
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
    // the preamble + sync symbols de-spread on every antenna (Despreader)
    int ant = 0;
    std::vector<cf> sym_y;
};

class LoraRx;

// Separation by CODE: a demodulated symbol de-spread on every antenna with
// its own chirp. The receiver knows each symbol's window (ring index a, one
// symbol long), the bin it decided and the fractional offset it de-rotated
// by; the despreader correlates every antenna's input with exactly that
// chirp - y[k] = the antenna's complex amplitude of THIS transmitter's
// symbol (another LoRa signal - other SF / BW, or the same SF at another
// timing / offset - stays spread over all bins). Returns the antennas
// filled (0 = not available: no antennas, or the samples are gone).
class Despreader {
public:
    virtual ~Despreader() = default;
    virtual int despread(const LoraRx& rx, int64_t a, int bin, double frac, cf* y) = 0;
    virtual int antennas() const = 0;
};
// a packet's de-spreading is held to about this much of its airtime (s; the
// input rate x every antenna per second): longer packets take every k-th symbol
constexpr double DESPREAD_BUDGET_S = 0.1;
// ... but at least this many symbols (an SF12 BW125 symbol is 33 ms)
constexpr int DESPREAD_MIN_SYMS = 12;
constexpr int DESPREAD_PRE = 8;   // preamble up-chirps de-spread (the last ones before the sync word)

// One channel's resampled samples, shared by its receivers. calloc'd: a
// large block comes as fresh zero pages from the OS, so a ring sized for the
// longest packet (16-32 MB at 500 kHz) costs no zeroing and only the memory
// actually written (the search's probes create rings all the time)
struct Ring {
    cf* buf = nullptr;
    int mask = 0;
    int64_t abs = 0;                // samples pushed so far
    Ring() = default;
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;
    ~Ring() { std::free(buf); }
    void init(int min_len) {
        int rs = 1;
        while (rs < min_len) rs <<= 1;
        std::free(buf);
        buf = static_cast<cf*>(std::calloc(static_cast<size_t>(rs), sizeof(cf)));
        mask = rs - 1;
        abs = 0;
    }
    void push(const cf* x, size_t n) {
        for (size_t i = 0; i < n; i++) buf[static_cast<size_t>((abs + static_cast<int64_t>(i)) & mask)] = x[i];
        abs += static_cast<int64_t>(n);
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
    bool pending() const { return pending_ || pk_.active; }
    // the packet in progress: its first sample (stream index), else -1 -
    // busy_from: a preamble found (header pending) or the header decoded;
    // decoding_from: the header decoded (its payload follows)
    int64_t busy_from() const { return pk_.active ? pk_.fr.start_sample : pending_ ? pend_start_ : -1; }
    int64_t decoding_from() const { return pk_.active ? pk_.fr.start_sample : -1; }
    int triggers() const { return triggers_; }     // preamble candidates looked at so far

    // every preamble + SFD found is appended here (nullptr = not logged)
    std::vector<Detect>* det_log = nullptr;
    // de-spreads the packet's symbols on every antenna (nullptr = none)
    Despreader* desp = nullptr;
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
    // the packet whose payload is demodulated block by block
    struct Pk {
        bool active = false;
        Frame fr;
        int64_t base = 0;
        int total_syms = 0;
        std::vector<uint8_t> nib, ok;
        int fec_bad = 0, fec_total = 0;
        double track = 0;
        int resid = 0;
        bool ldro = false, has_crc = false, low_sf = false;
        int cr = 1, length = 0, sf_app = 7, need_nibbles = 0;
        // separation by code: the de-spread symbols (every stride-th, ym antennas each)
        std::vector<cf> ys;
        int ym = 0, stride = 1, sym_i = 0, sym_total = 0;
    } pk_;
    // a symbol to de-spread: its window, decided bin, fractional de-rotation
    struct SymRef { int64_t a; int bin; double frac; };
    void despread_into(const SymRef& r, std::vector<cf>& ys, int* ym);
    int continue_packet(std::vector<Frame>& out);

    // demod a symbol starting at absolute index `a` with the down/up chirp;
    // returns the argmax bin (0..N-1, aliased) and fills pk/mean power.
    int demod_bin(int64_t a, bool use_up, float* pk, float* mean, float* frac, double derot_bins = 0);
    int detect_bin(int64_t a, float* pk, float* mean);   // the cheap detector's dechirp
    // 1 = decoded (out appended), 0 = failed/not a packet, 2 = need more samples
    int decode_from(int64_t preamble_start, std::vector<Frame>& out);
};

}  // namespace lora_phy
