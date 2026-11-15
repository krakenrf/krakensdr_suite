#pragma once
// Transmitter identification (EN 300 401 clause 14.8): in the null symbol
// every transmitter of the single frequency network switches on 4 of the 8
// carrier pairs of its comb c (SubId, 0..23) - the pattern p (MainId,
// 0..69) - repeated in the 4 quarters of the band: 32 carriers of its own.
//
// Detection per null symbol: the power of every (comb, pair) slot summed
// over its 8 carriers (and every antenna), against the noise of the null
// symbol's carriers (median). A code is a comb with exactly 4 slots above
// the threshold (or 4 that stand 6 dB above the rest) that passes a
// structure check: both carriers of each pair carry power (leakage from a
// neighbouring comb's carriers falls off across the pair), at least 3 of the
// 4 quarters carry it, and it is not a copy of a stronger code with the same
// pattern on a nearby comb (13 dB stronger on the next comb, 25 dB within 4
// combs: inter-carrier interference of a strong transmitter - a sample clock
// offset, phase noise - lands on exactly those carriers). The TII has no CRC: a code counts only after 5
// detections in the last 20 null symbols that carried TII.
//
// Per transmitter, from every antenna (process_multi): the spatial
// covariance over its own carriers (those no other code detected in the
// same null symbol uses), minus the noise covariance of the empty carriers
// of that symbol, if its main eigenvalue holds the signal.
//
// Delay against the strongest transmitter: both carriers of a pair carry
// the same phase (z = A_c,p(k) e^(j phi_k) + A_c,p(k-1) e^(j phi_k-1)) - or,
// as some transmitters on the air do, phi_k+1 on the second (both are kept
// apart and each transmitter gets the one its pairs agree with), so the
// pairs' phase step gives the delay modulo 1 ms (coarse); a delay
// profile over the 32 carriers (non-coherently averaged) gives it to a
// fraction of a sample but repeats every 50 samples (the 48-carrier spacing):
// the coarse value picks the period. Reported only when both agree.

#include "dab_tables.hpp"

#include <complex>
#include <cstdint>
#include <map>
#include <vector>

namespace dab {

struct TiiPacket {
    int p = 0, c = 0;
    int m = 0;
    std::vector<std::complex<float>> cov;   // m x m, row-major
    double share = 0;                       // main eigenvalue / trace (noise removed)
    int carriers = 0;
};

class Tii {
public:
    struct Tx {
        int p = 0, c = 0;
        uint32_t hist = 0;         // detections in the last 32 TII-bearing null symbols (bit 0 = latest)
        uint64_t hits = 0;
        bool confirmed = false;
        double snr_lin = 0;        // per carrier, smoothed
        double last_hit = 0, first = 0;   // frame times (s)
        // delay
        std::complex<double> qsum2[2];   // pair phase steps, per convention (second carrier phi_k0 / phi_k0+1)
        double qabs = 0;
        std::vector<float> prof2[2];     // delay profiles, PROF_N points from PROF_LO samples
        int convention = 0;              // 0 = the standard's (both carriers phi_k0), 1 = phi_k0+1 on the second
        int nprof = 0;
        double tau = 0;            // samples after the reference (the frame timing), NAN = not reliable
        bool tau_ok = false;
        double tau_coarse = 0, coherence = 0, tau_fine = 0;   // diagnostics
        bool seen_now = false;     // detected in the latest null symbol
    };
    static constexpr int PROF_LO = -300, PROF_N = 1000;

    void reset();
    // one null symbol: car[a] = antenna a's K carriers (dense index), corrected
    // to the frame timing; now = its time (s). With m >= 2 and want_cov, the
    // confirmed codes in it get a covariance packet.
    void process(const std::complex<float>* const* car, int m, double now, bool want_cov, std::vector<TiiPacket>& out);
    const std::map<int, Tx>& txs() const { return tx_; }
    // the strongest confirmed transmitter's key (p * 32 + c), -1 = none
    int strongest() const;
    // delay of tx after the strongest, microseconds; false = not reliable
    bool delay_us(const Tx& t, double* us) const;
    uint64_t symbols = 0, tii_symbols = 0;
    // codes dropped since the last call (no detection for a long time)
    std::vector<int> take_dropped();
private:
    std::map<int, Tx> tx_;
    std::vector<int> dropped_;
    std::vector<float> pw_, srt_;
    void update_delay(Tx& t, const std::complex<float>* const* car, int m, const std::vector<int>& own);
};

}   // namespace dab
