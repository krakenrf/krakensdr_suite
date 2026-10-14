#pragma once

// LTE downlink of one cell beyond its sync / reference signals - what every
// phone reads before it attaches, all broadcast in the clear (36.211 /
// 36.212 / 36.213):
//   CellRx   the cell's resource grid (every antenna's subcarriers per OFDM
//            symbol of a subframe) and the channel of each antenna port from
//            its CRS; transmit-diversity combining (SFBC / SFBC + FSTD)
//   PBCH     -> MIB: bandwidth, antenna ports, PHICH configuration, frame
//            number
//   PCFICH   -> CFI (control region size)
//   PDCCH    -> DCI with the SI-RNTI (where SIB1 / SI messages are)
//   PDSCH    -> SIB1 / SI messages (lte_sib.cpp)
// Only what lies inside the VFO's 2.4 MHz is received: wider carriers spread
// their control channels over the whole bandwidth, so parts are missing -
// they are decoded as erasures, and SIB1 repeats are combined (rv / frame
// classes) until their CRC passes.

#include "search.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cells::lte {

// one subcarrier of one OFDM symbol of the subframe: l = 0..13, f signed (DC skipped)
struct Re {
    int l, f;
};

// k (0 .. 12 N_RB - 1, the carrier's subcarriers from the lowest) -> signed f
inline int f_of_k(int k, int nrb) {
    const int kappa = k - 6 * nrb;
    return kappa < 0 ? kappa : kappa + 1;
}

// N_gap,1 / N_gap,2 (36.211 Table 6.2.3.2-1)
inline int ngap1(int nrb) {
    if (nrb <= 10) return (nrb + 1) / 2;
    if (nrb == 11) return 4;
    if (nrb <= 19) return 8;
    if (nrb <= 26) return 12;
    if (nrb <= 44) return 18;
    if (nrb <= 63) return 27;
    if (nrb <= 79) return 32;
    return 48;
}
inline int ngap2(int nrb) { return nrb < 50 ? 0 : nrb <= 63 ? 9 : 16; }

// resource block group size P (36.213 Table 7.1.6.1-1)
inline int rbg_size(int nrb) { return nrb <= 10 ? 1 : nrb <= 26 ? 2 : nrb <= 63 ? 3 : 4; }

class CellRx {
public:
    // x: the antennas mixed so that the carrier is at 0 Hz; cfo_hz the cell's
    // residual carrier offset; carrier_off / vis_hz as in EstParams
    CellRx(const std::vector<const cf*>& x, size_t n, const Numerology& num, int pci, double frame0, double cfo_hz, double carrier_off,
           double vis_hz);
    int m() const { return m_; }
    int K() const { return K_; }
    int pci() const { return pci_; }
    // radio frames with a sample inside the burst: frame k starts at frame0 + k x 10 ms
    long first_frame() const;
    long last_frame() const;
    // FFT + channel estimates of subframe sf of frame k; false if it isn't
    // (wholly) in the samples
    bool load(long k, int sf);
    // the antenna ports whose channels load() estimates (4 until the MIB says)
    void set_ports(int n) { nports_ = std::clamp(n, 1, 4); }
    int nports() const { return nports_; }
    bool vis(int f) const { return f != 0 && f >= -K_ && f <= K_ && vis_[static_cast<size_t>(f + K_)]; }
    // received spectrum / channel of port p at subframe symbol l, antenna a: [f + K]
    const cf* y(int l, int a) const { return Y_.data() + (static_cast<size_t>(l) * m_ + static_cast<size_t>(a)) * W_; }
    const cf* h(int p, int l, int a) const { return H_.data() + ((static_cast<size_t>(p) * 14 + static_cast<size_t>(l)) * m_ + static_cast<size_t>(a)) * W_; }
    float noise() const { return noise_; }     // per-RE noise (+ interference) power, per antenna
    // the CRS power of port p (mean |h|^2 over the antennas): ~0 for ports the cell doesn't have
    float port_power(int p) const { return pp_[static_cast<size_t>(p)]; }
    int sf() const { return sf_; }
    long frame() const { return k_; }

private:
    std::vector<const cf*> x_;
    size_t n_;
    Numerology num_;
    int pci_;
    double frame0_, cfo_;
    int m_, N_, K_, W_;
    std::vector<uint8_t> vis_;
    SymFft sym_;
    std::vector<cf> Y_, H_;
    float noise_ = 1;
    int nports_ = 4;
    std::array<float, 4> pp_{};
    int sf_ = -1;
    long k_ = 0;
    // per port: the pilots' subcarriers (sorted) + the interpolation of every subcarrier between them
    struct Interp {
        int i0 = -1, i1 = -1;
        float w = 0;
    };
    std::vector<int> pf_[4];
    std::vector<Interp> ip_[4];
    std::vector<std::vector<cd>> crs_[3];      // [l 0 / 1 / 4][ns]
};

// Transmit-diversity combining over `ports` antenna ports (1 = none) of
// QPSK REs in mapping order: 2 soft bits per RE (> 0 = 0), 0 where an RE
// isn't received. ports 2: SFBC pairs, 4: SFBC + FSTD groups of 4 (36.211
// 6.3.3.3 / 6.3.4.3)
void txd_llr(const CellRx& rx, const std::vector<Re>& re, int ports, std::vector<float>* llr);

// --- PBCH (36.211 6.6, 36.212 5.3.1) ---

struct Mib {
    int nrb = 0;                // 6 15 25 50 75 100
    int ports = 0;              // 1 2 4 (the CRC mask)
    int phich_ext = 0;          // PHICH duration: 0 normal, 1 extended
    int phich_ng = 0;           // PHICH resource Ng: 0 = 1/6, 1 = 1/2, 2 = 1, 3 = 2
    int sfn = 0;                // the frame's system frame number (0..1023)
    int sib1_br = 0;            // schedulingInfoSIB1-BR-r13 (0 = no LTE-M)
    uint32_t bits = 0;          // the 24 MIB bits
};

// the MIB from the PBCH of frame k (CellRx loaded with subframe 0 of it);
// ports_hint = the CRS ports seen (tried first)
bool decode_pbch(const CellRx& rx, int ports_hint, Mib* out);

std::string phich_ng_text(int ng);

// --- control region (36.211 6.2.4, 6.7 - 6.9) ---

// The resource element groups of the control region of a subframe, numbered
// as the PDCCH uses them (after PCFICH / PHICH)
struct Control {
    int nrb = 0, ports = 0, cfi = 0, nsym = 0;  // nsym = OFDM symbols of the control region
    std::vector<std::array<Re, 4>> reg;          // every REG of the PDCCH, in mapping order (36.211 6.8.5)
    std::vector<int> quad;                       // quadruplet i (CCE i / 9) -> REG index (interleaved + cyclic shift)
    int ncce = 0;
};

// the CFI from the PCFICH of the loaded subframe (0 = not readable); conf =
// the best codeword's margin over the next
int decode_pcfich(const CellRx& rx, const Mib& mib, double* conf);

// the control region's REG layout for a CFI (phich_m = TDD's m_i factor, 1 for FDD)
Control control_layout(int pci, const Mib& mib, int cfi, int phich_m);

// the PDCCH's soft bits of the loaded subframe in CCE order (72 per CCE),
// descrambled; 0 for REGs outside the received band. Only the first max_cce
// CCEs (the common search space is CCE 0 - 15)
void pdcch_llr(const CellRx& rx, const Control& c, std::vector<float>* llr, int max_cce = 16);

// --- DCI (36.212 5.3.3) ---

struct Dci {
    int size = 0;               // payload bits
    int L = 0, cce = 0;         // aggregation level, first CCE
    std::vector<uint8_t> bits;
    double agree = 0;           // re-encoded vs received: fraction of the received soft bits that agree
    int seen = 0;               // received (non-erased) coded bits
};

// DCI format 1A / 1C payload sizes for a carrier (FDD / TDD)
int dci1a_size(int nrb, bool tdd);
int dci1c_size(int nrb);

// the common search space of a subframe's PDCCH soft bits (pdcch_llr):
// candidates of aggregation 4 and 8 with one of the payload sizes whose CRC
// is scrambled with `rnti`
std::vector<Dci> dci_search(const std::vector<float>& llr, int ncce, const std::vector<int>& sizes, uint16_t rnti);

// --- PDSCH (36.211 6.2.3, 6.3, 6.4; 36.213 7.1.6.3, 7.1.7) ---

struct Grant {
    bool ok = false;
    bool f1c = false;           // format 1C (else 1A)
    bool distributed = false;
    int gap = 1;                // N_gap 1 / 2 (distributed)
    int vrb0 = 0, nvrb = 0;     // the VRBs
    std::vector<int> prb[2];    // PRBs of the even / odd slot (sorted)
    int mcs = 0;                // 1A: I_MCS, 1C: I_TBS
    int tbs = 0;
    int rv = -1;                // 1A: signalled, 1C: -1 (from the frame / window)
    int nprb1a = 0;             // 1A: the TBS table column
    std::string text() const;
};

// a format 1A / 1C DCI with SI-RNTI / P-RNTI / RA-RNTI -> its grant
Grant parse_dci(const Dci& d, int nrb, bool tdd);

// the redundancy version of a format 1C transmission (36.321 5.3.1): SIB1:
// k = (SFN / 2) mod 4, SI messages: k = the subframe's index in the SI window mod 4
inline int rv_of(int k) { return ((3 * k + 1) / 2) % 4; }

// the grant's REs in the subframe (the cell's ports, control region nsym,
// TDD = sync signals of TDD), in mapping order
std::vector<Re> pdsch_res(const Grant& g, int nrb, int ports, int nsym, int sf, int pci, bool tdd);

// the PDSCH soft bits of a grant in the loaded subframe, descrambled for rnti
void pdsch_llr(const CellRx& rx, const std::vector<Re>& re, int ports, uint16_t rnti, std::vector<float>* llr);

// Soft combining of the transmissions of one transport block (one code
// block, QPSK): every received bit added to its place in the mother code
class TbAcc {
public:
    void reset(int tbs);
    int tbs() const { return tbs_; }
    int count() const { return n_; }
    // E soft bits of one transmission with redundancy version rv
    void add(const std::vector<float>& llr, int rv);
    // turbo decoding + CRC24A; the TBS bits
    bool decode(std::vector<uint8_t>* tb, int* iters = nullptr);
    // the share of the mother code's bits received at least once
    double coverage() const;

private:
    int tbs_ = 0, K_ = 0, F_ = 0, n_ = 0;
    std::vector<float> s_[3];
    std::vector<uint8_t> hit_[3];
};

}  // namespace cells::lte
