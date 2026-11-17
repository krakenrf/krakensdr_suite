#pragma once
// The Fast Information Channel: the 4 FIB groups of a frame (EN 300 401
// clauses 11.2, 10.2, 5.2) and the ensemble database their FIGs describe
// (clauses 5.2.2, 6, 8; FIG 0/22 from V1.4.1 clause 8.1.9).
//
// Every FIG length is checked against its FIB; a field that doesn't fit ends
// that FIG (clause 5.2.2.0: "the rest of the FIG shall be ignored").

#include "dab_fec.hpp"
#include "dab_tables.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dab {

// FIC channel decoding: 9216 soft bits -> 12 FIBs (32 bytes each) + their CRC result
class FicDecoder {
public:
    FicDecoder();
    // returns the number of FIBs whose CRC passed; fibs[12][32], ok[12]
    int decode(const int8_t* soft, uint8_t fibs[12][32], bool ok[12]);
    // channel bit errors found by re-encoding the decoded FIBs of good CRC (last call)
    int last_bit_errors = 0, last_bits = 0;
private:
    Viterbi vit_;
    Depuncture dp_;
    std::vector<int8_t> mother_;
    std::vector<uint8_t> bits_, reenc_;
};

struct SubChannel {
    int id = -1;
    int start = 0;               // first CU
    Protection prot;
    int fec = -1;                // FIG 0/14 FEC scheme (packet mode), -1 = not signalled
    double seen = 0;
};

struct Component {
    int tmid = 0;                // 0 stream audio, 1 stream data, 3 packet data
    int type = 0;                // ASCTy / DSCTy
    int subch = -1;
    bool primary = false;
    bool ca = false;
    int scid = -1;               // packet mode
    int scids = -1;              // from FIG 0/8 (primary: 0)
    int packet_addr = -1;
    bool dg = false;
    int language = -1;
    std::vector<int> uas;        // FIG 0/13 user application types
    std::string label;           // FIG 1/4 / 2/4
};

struct Service {
    uint32_t sid = 0;
    bool data = false;           // 32-bit SId
    std::string label, short_label;
    bool label2 = false;         // the label came from FIG 2 (extended)
    int pty = -1;
    bool pty_dynamic = false;
    unsigned asu = 0;            // FIG 0/18 announcement support flags
    std::vector<int> clusters;
    int ca_id = 0;
    std::vector<Component> comps;
    double first_seen = 0, last_seen = 0;
    const Component* primary() const;
};

struct TiiEntry {               // FIG 0/22 (deprecated since V2.1.1, but cheap)
    double lat = 0, lon = 0;     // degrees
    int td_us = -1;              // time delay (sub-identifiers)
    bool main_ref = false;
};

struct Ensemble {
    int eid = -1;
    std::string label, short_label;
    bool label2 = false;
    int ecc = -1;
    // FIG 0/9 extended field (P/D = 0): the programme services whose ECC differs from the
    // ensemble's, as the latest FIG 0/9 listed them (at most 12: the field has <= 25 bytes)
    std::map<uint32_t, int> service_ecc;
    int lto = -999;              // local time offset, half hours
    int inter_table = 1;
    bool alarm = false;
    int cif_count = -1;          // of the first CIF of the latest frame with FIG 0/0
    int change_flags = 0, occurrence = -1;
    int services_total = -1, reconfig_count = -1;   // FIG 0/7
    // date and time (FIG 0/10): MJD + UTC, and the frame time (s) it arrived with
    int mjd = -1, hh = 0, mm = 0, ss = 0, ms = 0;
    bool utc_long = false;
    double dt_at = 0;
    std::map<int, SubChannel> subch, next_subch;
    std::map<uint32_t, Service> services, next_services;
    std::map<int, TiiEntry> tii_main;              // MainId -> reference position
    std::map<int, TiiEntry> tii_sub;               // MainId * 32 + SubId -> position
    // announcements in progress (FIG 0/19): cluster -> (flags, subch)
    std::map<int, std::pair<unsigned, int>> ann_active;

    void clear() { *this = Ensemble(); }
};

// Countries (README "Country"): what the FIC determines, and only that.
struct EnsembleCountry {
    std::string name;            // the country named ("" = none)
    std::string base;            // name without a sub-area ("Australia"): what a service is compared with
    std::string text;            // the "Country" fact ("" = no ECC / EId yet)
    bool from_services = false;  // the EId's country id was contradicted by the services
};
// kept_base: the base of the previous result when it came from the services (it is
// kept while no service contradicts it, with fewer than MIN_COUNTRY_SERVICES)
EnsembleCountry ensemble_country(const Ensemble& e, const std::string& kept_base);
constexpr int MIN_COUNTRY_SERVICES = 3;
// a service's own ECC: in its 32-bit SId, or listed in the FIG 0/9 extended field (-1 = none)
int own_ecc(const Ensemble& e, const Service& s);
// the service's "Country" detail ("" = none)
std::string service_country(const Ensemble& e, const Service& s, const EnsembleCountry& ec);

// hooks for happenings while parsing
struct FicEvents {
    std::function<void(const std::string&)> event;              // reconfiguration, announcements
};

// Bounds on what the FIC can make the database hold (a hostile or corrupted
// FIC must cost a bounded amount of memory): FIG 0/7 counts at most 63
// services, a sub-channel id has 6 bits, a component count 4 bits
constexpr size_t MAX_SERVICES = 64;     // per map (current, next); the least recently signalled one goes
constexpr size_t MAX_FIG2_SETS = 256;   // FIG 2 label segment sets being assembled
constexpr double FIG2_MAX_AGE = 30;     // s: an incomplete segment set older than this is dropped

class FigParser {
public:
    explicit FigParser(Ensemble& e) : e_(e) {}
    // before the FIBs of one transmission frame
    void begin_frame() {
        frame_cif = -1;
        frame_flags = 0;
        frame_occ = -1;
        frame_fig00_at = 0;
        foreign_eid = -1;
    }
    // one FIB that passed its CRC; cif = its CIF's index in the frame (0..3)
    void fib(const uint8_t* fib, int cif, double now);
    // drops the parser's own state (segments, counters) - with Ensemble::clear()
    void clear() {
        fig2_.clear();
        fig_count.clear();
        malformed = 0;
        begin_frame();
    }
    FicEvents ev;
    // counters of FIGs seen, by type/ext (for the details)
    std::map<int, uint64_t> fig_count;
    uint64_t malformed = 0;
    // from this frame's FIG 0/0 (of the current ensemble): the CIF count of
    // the frame's first CIF (0..4999, -1 = none), the change flags and the
    // occurrence change (-1 = none), and the index (0..3) of the CIF whose FIB
    // carried it (mode I: always 0, clause 6.4.1)
    int frame_cif = -1, frame_flags = 0, frame_occ = -1, frame_fig00_at = 0;
    // a FIG 0/0 with ANOTHER EId than the ensemble's was seen in this frame
    // (it is not applied: the receiver decides whether the ensemble changed)
    int foreign_eid = -1;
    size_t fig2_sets() const { return fig2_.size(); }
private:
    Ensemble& e_;
    int cif_ = 0;   // the CIF index of the FIB being parsed
    void fig0(const uint8_t* d, int n, double now);
    void fig1(const uint8_t* d, int n, double now);
    void fig2(const uint8_t* d, int n, double now);
    Service& service(std::map<uint32_t, Service>& m, uint32_t sid, bool data, double now);
    void set_label(int ext, uint32_t id, int scids, const std::string& label, const std::string& shortl, bool ext_label,
                   double now);
    struct Seg {
        int toggle = -1;
        int count = -1;
        bool enc16 = false;
        std::string part[8];
        uint8_t have = 0;
        double t = 0;   // last segment
    };
    std::map<uint64_t, Seg> fig2_;   // (ext, id, scids) -> segments
};

}   // namespace dab
