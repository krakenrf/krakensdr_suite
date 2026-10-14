#pragma once

// LTE RRC broadcast messages (3GPP TS 36.331, BCCH-DL-SCH, unaligned PER):
// SystemInformationBlockType1 and SystemInformation (SIB2 - SIB5 decoded,
// the others named). Public cell configuration only - no user data exists
// on these channels.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cells::lte {

struct Plmn {
    int mcc = -1;           // inherited from the previous entry when absent
    int mnc = -1;
    int mnc_digits = 2;
    bool reserved = false;  // cellReservedForOperatorUse
    std::string text() const;   // "530-01"
};

struct SchedInfo {
    int period_rf = 0;              // si-Periodicity (radio frames)
    std::vector<int> sibs;          // SIB numbers (SIB2 is always in the first SI message)
};

struct Sib1 {
    std::vector<Plmn> plmn;
    int tac = -1;
    uint32_t cell_id = 0;           // 28 bits: eNB ID (20) + cell (8)
    bool barred = false, intra_resel_allowed = true, csg = false;
    long csg_id = -1;
    int q_rxlevmin_dbm = 0;
    int q_rxlevmin_offset_db = 0;   // 0 = absent
    int p_max_dbm = -999;           // -999 = absent
    int band = 0;
    std::vector<int> multi_band;    // multiBandInfoList (+ v9e0)
    std::vector<SchedInfo> sched;
    int tdd_sa = -1, tdd_ssp = -1;  // TDD: subframeAssignment, specialSubframePatterns
    int si_window_ms = 0;
    int value_tag = -1;
    bool ims_emergency = false;
    int q_qualmin_db = -999;        // -999 = absent
    std::vector<std::pair<std::string, std::string>> lines;   // every decoded field, for display
};

// a SystemInformation message: the SIBs it carries
struct SiMsg {
    std::vector<int> sibs;          // SIB numbers in the message
    int ul_earfcn = -1, ul_nrb = 0; // SIB2 freqInfo (-1 = the band's default duplex)
    std::vector<int> intra_pci;     // SIB4 neighbour cells
    std::vector<int> inter_earfcn;  // SIB5 other LTE carriers
    std::vector<std::pair<std::string, std::string>> lines;
    bool complete = true;           // every SIB in it decoded (false: one we can't skip stopped it)
};

// BCCH-DL-SCH message bits (MSB first) -> 1 = SIB1, 2 = SystemInformation, 0 = error
int parse_bcch(const std::vector<uint8_t>& bits, Sib1* sib1, SiMsg* si);

}  // namespace cells::lte
