#pragma once

// Generic pilot-based CIR estimator for OFDM standards with scattered pilots
// (DVB-T, DVB-T2, ISDB-T, ATSC 3.0) on a slice of the channel.
//
// The VFO sees up to ~2.4 MHz of a 6-8 MHz channel. The slice is resampled
// to the standard's elementary rate / D (D a power of two: an FFT of N/D
// samples then holds exactly the carriers inside the slice), and per burst:
//   1. mode + symbol timing + fractional carrier offset from the guard
//      interval: the cyclic-prefix correlation x(n) x*(n + N') summed over
//      the CP and folded modulo the symbol length (all candidate FFT sizes /
//      guards of the standard; the best normalised peak wins). Standards
//      whose frames start with a preamble that resets the symbol grid
//      (DVB-T2's P1) find the preambles instead (framed()) and take the
//      symbols from there,
//   2. the scattered-pilot comb: per symbol the residue class of FFT bins
//      (mod Dx Dy) with the boosted power, advancing by Dx each symbol,
//   3. the integer carrier offset (which absolute carrier is bin 0): the
//      candidate whose pilots, divided by their known values, give the
//      sparsest impulse response (searched once, kept while locked),
//   4. per Dy consecutive symbols (one full comb of Dx) or per symbol (when
//      the pilot values depend on the symbol): every antenna's pilot bins /
//      their values, Hann-weighted, zero-padded IFFT -> its CIR.
// Pilot values with a sign per symbol (DVB-T2's frame PN, symbol_signs()):
// the symbol's index in its frame (from the preambles) gives it. A sign
// search on a standard WITHOUT such signs rejects a signal that has them
// (DVB-T2 in PP1 looks like DVB-T otherwise).

#include "dsp_util.hpp"
#include "estimator.hpp"

#include <memory>
#include <string>
#include <vector>

namespace sfn {

struct OfdmMode {
    std::string label;      // "8k 1/4"
    double bw_mhz = 8;      // channel bandwidth
    double fs = 0;          // elementary sample rate 1/T (Hz)
    int nfft = 0;           // FFT size at fs
    int ncp = 0;            // guard interval (samples at fs)
    int kmin = 0, kmax = 0; // active carriers (the standard's numbering)
    int kc = 0;             // the carrier at the channel centre
};

struct PilotPattern {
    std::string label;      // "PP2"
    int dx = 3, dy = 4;     // pilot every dx*dy carriers, moving dx per symbol (cycle of dy symbols)
};

class OfdmStandard {
public:
    virtual ~OfdmStandard() = default;
    virtual std::string name() const = 0;
    // the modes to try for a channel bandwidth (0 = every bandwidth)
    virtual std::vector<OfdmMode> modes(double bw_mhz) const = 0;
    virtual std::vector<PilotPattern> patterns(const OfdmMode& m) const = 0;
    // carrier k is a scattered pilot of a symbol with pattern phase ph (l mod dy)
    virtual bool is_pilot(const OfdmMode& m, const PilotPattern& p, int k, int ph) const {
        if (k < m.kmin || k > m.kmax) return false;
        const int M = p.dx * p.dy;
        return (((k - m.kmin) % M) + M) % M == p.dx * ph;
    }
    // its reference value (up to a factor common to the whole symbol)
    virtual kp::cf pilot_value(const OfdmMode& m, const PilotPattern& p, int k) const = 0;
    // pilot values depend on the carrier only: dy symbols fill one comb of dx
    virtual bool combine_symbols() const { return true; }
    // ... but with a sign per symbol: symbol_sign(l) for symbol l of a frame
    // (framed standards: l is known from the preamble; groups with a symbol
    // of unknown l are skipped - a sign search can't tell the true signs from
    // (+, -, +, -), which only moves the CIR by half its range)
    virtual bool symbol_signs() const { return false; }
    virtual int symbol_sign(int l) const { (void)l; return 1; }
    // frames start with a preamble that resets the symbol grid (DVB-T2 P1):
    // its starts in x (antenna 0, rate_in) for elementary rate fs, in samples
    // of x; its length in T; the symbols after it without scattered pilots
    // (P2) and whether the last symbol of a frame has none either (frame
    // closing symbol)
    virtual bool framed() const { return false; }
    virtual std::vector<double> frame_starts(const std::vector<kp::cf>& x, double rate_in, double fs) const {
        (void)x, (void)rate_in, (void)fs;
        return {};
    }
    virtual int preamble_T() const { return 0; }
    // the symbol grid isn't regular (ATSC 3.0: bootstraps, excess guard
    // samples of up to max_excess_T per symbol): the guard correlation is
    // followed from symbol to symbol instead of folded; the guard length is
    // the longest allowed one that fits the measured symbol period
    virtual bool tracked() const { return false; }
    virtual int max_excess_T() const { return 0; }
    // the channel centre in carriers from FFT bin 0 (default: kc - k_off +
    // the fractional offset; ATSC 3.0 picks its reduced-carrier setting)
    virtual double centre_bins(const OfdmMode& m, int k_off, double eps, double rf_hz, double df) const {
        (void)rf_hz, (void)df;
        return m.kc - k_off + eps;
    }
    virtual int preamble_symbols(const OfdmMode&) const { return 0; }
    virtual bool closing_symbol() const { return false; }
    // Signalling on dedicated carriers in every symbol (DVB-T TPS, ISDB-T
    // TMCC), differentially modulated from symbol to symbol: the absolute
    // carriers to read (sig) and carriers with the same value in every symbol
    // (ref: continual pilots - the symbol-to-symbol common phase)
    virtual void signal_carriers(const OfdmMode& m, std::vector<int>* sig, std::vector<int>* ref) const {
        (void)m;
        sig->clear();
        ref->clear();
    }
    // a burst's values of those carriers: per symbol (in order) sig / ref
    // values (0 = outside the slice), follows = right after the previous one
    struct SigBurst {
        std::vector<std::vector<kp::cf>> sig, ref;
        std::vector<char> follows;
    };
    // symbols the signalling wants in one go (more than the burst's analysed
    // part: read from the whole burst)
    virtual int signalling_symbols(const OfdmMode& m) const {
        (void)m;
        return 0;
    }
    virtual void signalling(const OfdmMode& m, const SigBurst& s) {
        (void)m;
        (void)s;
    }
    virtual std::vector<std::pair<std::string, std::string>> info(const OfdmMode& m) const {
        (void)m;
        return {};
    }
    virtual std::vector<std::string> take_news() { return {}; }
    virtual void reset_signalling() {}
    // Estimator::profile_floor_db
    virtual double profile_floor_db() const { return 0; }
    // the mode's own label for describe() (pattern / extras once found)
    virtual std::string describe(const OfdmMode& m, const PilotPattern& p) const {
        return m.label + ", " + (m.bw_mhz == static_cast<int>(m.bw_mhz) ? std::to_string(static_cast<int>(m.bw_mhz)) : std::to_string(m.bw_mhz)) +
               " MHz" + (p.label.empty() ? "" : ", " + p.label);
    }
};

class OfdmEstimator : public Estimator {
public:
    OfdmEstimator(kp::Host& h, std::unique_ptr<OfdmStandard> std) : host_(h), std_(std::move(std)) {}
    std::string name() const override { return std_->name(); }
    double detect(const Burst& b, const EstOptions& o) override;
    bool process(const Burst& b, const EstOptions& o, const Sink& sink) override;
    std::string describe() const override;
    void reset() override;
    std::vector<std::pair<std::string, std::string>> info() const override { return locked_ ? std_->info(mode_) : decltype(info()){}; }
    std::vector<std::string> take_news() override { return std_->take_news(); }
    double centre_hz() const override { return locked_ ? centre_hz_ : NAN; }
    double bw_mhz() const override { return locked_ ? mode_.bw_mhz : 0; }
    double profile_floor_db() const override { return std_->profile_floor_db(); }

private:
    kp::Host& host_;
    std::unique_ptr<OfdmStandard> std_;
    FftCache fft_;
    double centre_hz_ = NAN;
    // the signalling carriers of the locked mode (OfdmStandard::signal_carriers)
    std::string sig_mode_;
    std::vector<int> sig_k_, ref_k_;
    // lock
    bool locked_ = false;
    OfdmMode mode_;
    PilotPattern pat_;
    int D_ = 1;              // decimation of the elementary rate
    int k_off_ = 0;          // absolute carrier of FFT bin 0
    int fails_ = 0;
    double rho_ = 0;         // CP correlation of the last burst
    double cfo_carriers_ = 0;
    int frames_ = 0;         // preambles in the last burst (framed standards)
    // detect(): process() only counts the pilot combs whose best signs are all +
    bool sign_check_ = false;
    int sign_groups_ = 0, sign_plain_ = 0;
    // a slice resampled to fs/D (antenna-major) and its symbols
    struct Prep {
        std::vector<std::vector<kp::cf>> x;
        double rate = 0;
        int N = 0, ncp = 0;
        std::vector<long> starts;   // FFT window starts (mid-guard) of the usable symbols
        std::vector<double> frac;   // ... and the fraction of a sample they really start later
        std::vector<char> follows;  // the symbol right after the previous one in starts
        std::vector<int> lidx;      // its index in the frame (framed standards; -1 = unknown)
        double rho = 0;             // normalised CP correlation
        double eps = 0;             // fractional carrier offset
        int frames = 0;
    };
    bool prepare(const Burst& b, const OfdmMode& m, int antennas, double max_s, Prep* p,
                 const std::vector<double>* frame_starts = nullptr);
    static bool cp_fold(const std::vector<kp::cf>& x, int N, int ncp, int* o, double* rho, double* eps);
    struct CpProducts { std::vector<float> pr, pi, pe; };
    static void cp_products(const std::vector<kp::cf>& x, int N, CpProducts* cp);
    static bool cp_fold(const CpProducts& cp, int N, int ncp, int* o, double* rho, double* eps);
    // framed standards: the symbol starts (guard start, at the resampled
    // rate) after / before each preamble; flags: follows the previous one
    void frame_grid(const std::vector<double>& fstarts, double scale, long n, const OfdmMode& m, int N, int ncp,
                    std::vector<double>* sym, std::vector<char>* follows, std::vector<int>* lidx = nullptr) const;
    int lf_ = 0;             // symbols per frame (framed standards; from two preambles in a burst)
    double step_ = 0;        // the common phase step from symbol to symbol (last measured)
    static double cp_at(const std::vector<kp::cf>& x, const std::vector<double>& sym, int N, int ncp, double* eps);
    // tracked standards: the guard starts symbol by symbol (runs of
    // contiguous symbols fitted with a straight line); the mean normalised
    // guard correlation over them; the runs' symbol period (median)
    static double track(const CpProducts& cp, int N, int ncp, int max_excess, std::vector<double>* sym,
                        std::vector<char>* follows, double* period);
    static double track_pass(const CpProducts& cp, int N, int ncp, int max_excess, std::vector<double>* sym,
                             std::vector<char>* follows, double* period);
    // per symbol: FFT of antenna a, bins in natural order
    void symbol_fft(const Prep& p, int a, size_t start, std::vector<kp::cf>* Y);
    int valid_bins(const Prep& p, double rate_in) const;
    bool find_pattern(const Prep& p, int B, PilotPattern* out, std::vector<int>* residues, double* score);
    bool find_offset(const Prep& p, int B, const PilotPattern& pat, const std::vector<int>& res, int* k_off);
    // one comb (J carriers kf + i dk) of dy symbols: the signs (per symbol of
    // the group) that give antenna 0 the sparsest CIR; returns its peak / mean
    static double pdp_shift(const std::vector<double>& a, const std::vector<double>& b, int L);
    double best_signs(const std::vector<std::vector<kp::cf>>& part, const std::vector<std::vector<char>>& have, int J,
                      std::vector<int>* signs);
};

}  // namespace sfn
