#pragma once

// What one LTE cell broadcasts, collected burst by burst: the MIB (every
// burst - it gives the frame numbers), SIB1 (subframe 5 of even frames) and
// the SI messages SIB1 schedules (SIB2 ...), each transport block soft-
// combined over its repeats until its CRC passes (a 2.4 MHz VFO sees only a
// part of a wider carrier). Decoded blocks are refreshed every few minutes.

#include "lte_dl.hpp"
#include "lte_rrc.hpp"

#include <map>
#include <string>
#include <vector>

namespace cells::lte {

struct CellMeta {
    // MIB
    bool mib_ok = false;
    Mib mib;
    double mib_t = -1e9;
    int mib_frames = 0;                 // frames read in the last burst
    // SIB1
    bool sib1_ok = false;
    Sib1 sib1;
    double sib1_t = -1e9;               // decoded at (burst time)
    std::string sib1_dci;
    int sib1_tx = 0;                    // transmissions it took
    TbAcc sib1_acc;
    // SI messages by number (1 = the one with SIB2)
    struct Si {
        bool ok = false;
        SiMsg msg;
        double t = -1e9;
        std::string dci;
        int tx = 0;
        TbAcc acc;
    };
    std::map<int, Si> si;
    int cfi_seen[4] = {0, 0, 0, 0};     // PCFICH decisions (0 = unreadable)
    std::map<int, Control> layouts;     // control region layouts by CFI x 4 + PHICH m_i (cleared with the MIB)
    std::string status;                 // what is being waited for
    int version = 0;                    // bumped with every newly decoded block
    double served = -1e9;               // burst time it last got processing time
    size_t si_rr = 0;                   // the SI message looked for first (round robin)
    std::vector<std::string> news;      // events since the caller last took them
    std::vector<std::string> debug;     // verbose: every transmission combined (MetaParams::verbose)
};

struct MetaParams {
    double now = 0;                     // burst time (s)
    bool tdd = false;
    int ports_hint = 2;
    double budget_s = 0.15;             // processing time per burst and cell
    double refresh_s = 600;             // decoded blocks are read again after this
    bool verbose = false;
};

void meta_burst(CellRx& rx, CellMeta& m, const MetaParams& p);

// display lines of everything decoded (MIB, SIB1, SI messages)
std::vector<std::pair<std::string, std::string>> meta_lines(const CellMeta& m, double carrier_hz);

}  // namespace cells::lte
