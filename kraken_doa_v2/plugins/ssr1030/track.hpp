#pragma once

// Interrogators: which transmitter sent each interrogation (ID separation).
//
// Three kinds, told apart by how they interrogate:
//   SSR    a rotating secondary radar: an all-call train - Mode A / C (+ P4)
//          and Mode S-only all-calls (UF11) - at a PRF of 50-500 Hz (often
//          jittered), its roll-calls in between; its main beam sweeps past
//          once per turn (the scan period), P2 (SLS) below P1 there.
//          Found as a PERIODIC CHAIN of all-calls (sequence search), named
//          by its interrogator code (II / SI from its checked UF11 all-calls)
//   FIXED  a non-rotating ground interrogator (multilateration / WAM, a test
//          transponder): BURSTS (all-calls + a roll-call per target 1 ms
//          apart here) at ONE steady level - it doesn't rotate
//   ACAS   an aircraft's TCAS / ACAS: whisper-shout - Mode C-only all-calls
//          stepping up in power, each with an S1 suppression pulse 2 us
//          before P1, a sequence about once a second; UF0 / UF16 to the
//          aircraft it tracks; named by its own address (UF16 broadcast /
//          coordination MID)
// Roll-calls (UF4 / 5 / 20 / 21 / 0 / 16) are given to the interrogator
// whose II / SI they carry, whose burst they are in, whose main beam is on
// us at that moment - or to none.

#include "interr.hpp"
#include "radar_pri.hpp"
#include "radar_scan.hpp"

#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ssr {

enum SrcType { SRC_SSR = 0, SRC_FIXED, SRC_ACAS };

struct Rec {
    double t = 0, t_a = 0, t_b = 0;   // P1 / the DoA span (Host::time())
    int kind = 0;                     // Interr::kind()
    float db = 0, p2 = NAN, s1 = NAN;
    bool clipped = false;
    int uf = -1;                      // Mode S: its UF (-1 = Mode A / C, or not decoded)
    uint32_t addr = 0;
    bool verified = false;
    int ii = -1, si = -1;             // the interrogator code it carries (UF11 IC, roll-calls' IIS / SIS)
    uint32_t acas = 0;                // UF16: the sending aircraft
    bool allcall() const { return kind < 12 || uf == 11; }
};

struct Target {
    int n = 0;
    double last_t = 0;
    std::map<int, int> ufs;
};

struct Source {
    int type = SRC_SSR;
    int serial = 0;                   // creation order (ids before a name is known)
    std::string tid;                  // talker id (also the table key)
    double first_t = 0, last_t = 0;
    uint64_t n = 0;
    std::map<int, uint64_t> kinds;    // Interr::kind -> count (all-call side)
    std::map<int, uint64_t> ufs;      // Mode S UF -> count
    std::map<uint32_t, Target> targets;
    std::map<int, int> ii_votes, si_votes;   // IIS / SIS of its roll-calls
    int ii = -1, si = -1;             // from checked UF11 all-calls (or 5 agreeing roll-calls)
    bool ii_checked = false;
    uint32_t acas_addr = 0;           // ACAS: the aircraft's own address
    std::map<uint32_t, int> acas_votes;   // ... from UF16 coordination MIDs (not checkable)
    float peak_db = -1e9f, peak_dbfs = -1e9f;
    bool clipped = false;
    std::deque<double> rate_t;        // interrogation times, last 2 s
    // SSR train
    double pri = 0;                   // mean PRI (s)
    double jit_e = 0;                 // mean deviation of its PRIs from pri (s)
    double t_last = 0;                // last chain member
    float lvl_last = 0;
    std::deque<double> chain;         // chain member times (last 4 s)
    std::deque<std::string> slots;    // what each PRI slot held, last 60 ("A", "S+C", ...)
    std::string slot_open;
    double slot_t = -1;
    std::deque<double> s11_t;         // its UF11 all-calls (mark the PRI slot they open)
    radar::PriInfo pri_info;
    radar::LevelSeries lv;            // level over time (main beam -> scan)
    std::deque<double> beams;
    std::deque<float> beam_db;
    std::deque<double> beam_w3;
    double scan_s = 0;
    int scan_support = 0;
    double scan_told = 0;
    double mb_dwell = 0;
    float mb_db = NAN;
    std::deque<float> p2_main, p2_side;   // P2 - P1 in the main beam / elsewhere (dB)
    std::deque<float> p2abs;          // P2's level (dB over the noise): the same all round
    // FIXED bursts
    double burst_period = 0, burst_start = -1, burst_last = -1;
    std::deque<double> bursts;        // burst start times
    float burst_lvl = 0;
    int burst_ac = 0, burst_s = 0, burst_n = 0;     // the last complete burst
    int cur_ac = 0, cur_s = 0, cur_n = 0;
    double grid_us = 0;
    bool burst_regular = false;       // its bursts' spacing within 3 %
    // ACAS whisper-shout
    double seq_start = -1, seq_last = -1;
    float seq_lvl0 = 0, seq_lvl = 0, seq_s1 = 0;
    int seq_steps = 0;
    std::deque<double> seqs;          // sequence start times
    int last_steps = 0;
    int bcasts = 0;                   // its checked UF16 broadcasts
    double bc_t = -1;                 // ... the latest
    float last_step_db = 0, last_span_db = 0;
    double last_step_ms = 0;
    // DoA
    std::deque<Rec> cand;
    double tokens = 0, tok_t = 0;
    uint64_t packets = 0;
    bool announced = false, lost = false;
    std::string label;
};


class Tracker {
public:
    struct Out {
        // things for the decoder to report
        std::vector<std::pair<Source*, Rec>> packets;   // DoA packets
        std::vector<std::string> events;
        std::vector<std::pair<std::string, std::string>> renamed;   // old tid -> new tid
    };
    void reset();
    void set_doa(bool on, double doa_db) {
        doa_ = on;
        doa_db_ = doa_db;
    }
    // one interrogation (in time order)
    void add(const Interr& it, double t_a, double t_b, Out& out);
    // every 50 ms / every 0.5 s
    void tick_fast(double now, Out& out);
    void tick_slow(double now, Out& out);
    std::vector<std::unique_ptr<Source>>& sources() { return src_; }
    uint64_t unattributed() const { return unattr_; }
    uint64_t total() const { return total_; }
    static std::string type_name(int t) { return t == SRC_SSR ? "SSR" : t == SRC_FIXED ? "Fixed" : "ACAS"; }

private:
    std::vector<std::unique_ptr<Source>> src_;
    std::deque<Rec> pend_;            // not attributed yet (last 4 s)
    const bool dbg_ = getenv("SSR_DUMP") != nullptr;   // offline debugging: the chains found, as events
    int serial_ = 0;
    bool doa_ = true;
    double doa_db_ = 10;
    uint64_t unattr_ = 0, total_ = 0;
    // ACAS sequences being collected (not yet a source)
    struct Seq {
        double start = 0, last = 0;
        float lvl0 = 0, lvl = 0, s1 = 0;
        int steps = 0;
        std::vector<Rec> recs;
    };
    std::vector<Seq> seqs_;
    struct Burst {
        double start = 0, last = 0;
        float lvl = 0;
        int ac = 0, s = 0, n = 0;
        std::vector<double> ac_t;     // its Mode A / C all-calls' times
    };
    std::deque<Burst> orphan_bursts_;

    Source* make(int type, double t, Out& out);
    void attach(Source& S, const Rec& r, Out& out);
    bool try_ssr(const Rec& r, Out& out);
    bool try_sidelobe(const Rec& r, Out& out);
    bool try_fixed(const Rec& r, Out& out);
    void fixed_add(Source& S, const Rec& r);
    bool try_acas(const Rec& r, Out& out);
    bool try_rollcall(const Rec& r, Out& out);
    void close_seq(Seq& q, Out& out);
    void find_trains(double now, Out& out);
    void find_bursts(double now, Out& out);
    void scan_update(Source& S, double now, Out& out);
    void name(Source& S, Out& out);
};

}  // namespace ssr
