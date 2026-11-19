#pragma once

// P25 Phase 2 physical layer receiver (p25p2_phy.cpp): 48 kHz complex
// baseband -> 30 ms timeslots of 180 symbols for the MAC layer
// (p25p2_slot.hpp). One object handles whichever direction the channel
// carries:
// - outbound (base station, continuous H-DQPSK): acquired on two S-ISCH
//   sync words 180 or 540 symbols apart, then every slot is delivered
//   (soft symbols) while the grid is confirmed by an S-ISCH / S-OEMI at
//   least once a second;
// - inbound (subscriber bursts, H-CPM): bursts with the 22-symbol sync are
//   found by correlation and set up a common 30 ms grid; voice bursts
//   (pilots only) are looked for on that grid. Each burst gets its own
//   carrier offset, deviation scale and timing, and its symbols come from a
//   sequence detector matched to the CPM partial response (hard decisions).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kraken_plugin.hpp"
#include "p25p2_slot.hpp"

namespace dig {

class P25p2Phy {
public:
    explicit P25p2Phy(P25p2SlotSink& sink);
    // 48 kHz complex baseband, any block size
    void process(const kp::cf* x, size_t n);
    void reset();
    // A slot grid is held (either direction)
    bool locked() const { return out_locked_ || in_grid_; }
    // The grid held is an inbound one (bursts)
    bool inbound() const { return in_grid_ && !out_locked_; }
    // Carrier offset of the last delivered slot (Hz)
    float freq_hz() const { return last_freq_; }

    // Counters (status display, tests)
    struct Stats {
        int64_t out_hits = 0;       // S-ISCH correlation peaks while searching
        int64_t out_locks = 0;      // outbound grids acquired
        int64_t out_slots = 0;      // outbound slots delivered
        int64_t in_hits = 0;        // inbound sync correlation peaks
        int64_t in_grids = 0;       // inbound grids that delivered a burst
        int64_t in_synced = 0;      // inbound bursts delivered with their own sync
        int64_t in_unsynced = 0;    // inbound bursts delivered without a sync (voice)
        int64_t in_rejected = 0;    // sync peaks rejected by the burst checks
        int64_t grid_lost = 0;
    };
    const Stats& stats() const { return st_; }

    // Thresholds (defaults measured, see p25p2_phy.cpp); public for tests
    struct Tune {
        float out_thr = 0.75f;      // S-ISCH search: normalized |correlation|
        float in_thr = 0.75f;       // inbound sync search: normalized correlation
        float rms_synced = 0.75f;   // inbound: largest model residual (level units) of a burst with a sync ...
        float rms_unsynced = 0.70f; // ... of one without, on a radio track's timing / carrier
        float rms_blind = 0.55f;    // ... and of one found on the grid alone
    };
    Tune tune;

private:
    static constexpr int RB = 1 << 14;      // sample rings (341 ms; the searches look back <= 120 ms)
    static constexpr int RMASK = RB - 1;
    P25p2SlotSink& sink_;
    Stats st_;

    // --- front end: channel filter, delay-compensated sample rings ---------
    std::vector<float> taps_;
    std::vector<float> fre_, fim_;          // doubled input rings for the FIR (re / im)
    size_t fpos_ = 0;
    int fdelay_ = 0;
    int64_t nin_ = 0;                       // input samples consumed
    int64_t yend_ = 0;                      // y / idd samples available: [.., yend_)
    std::vector<kp::cf> y_;                 // filtered signal, index = input time
    std::vector<float> idd_;                // discriminator integrated over one symbol (Hz)
    float fd_[8] = {};                      // last 8 per-sample discriminator values (rad)
    // Per sample phase (m mod 8): the symbol-spaced history of the search
    // streams as doubled rings, so the correlators read contiguous windows
    static constexpr int PW = 32;
    struct Phase {
        float re[2 * PW] = {}, im[2 * PW] = {}, mag[2 * PW] = {};   // d[m] = y[m] conj(y[m-8])
        float z[2 * PW] = {};                                        // idd
        int pos = 0;
    };
    Phase phase_[8];
    kp::cf yat(double x) const;             // interpolated y
    float zat(double x) const;              // interpolated idd

    // Correlation peak picking: the maximum over one symbol after the
    // threshold is crossed; the values around it give a parabolic position
    struct Peak { bool active = false; float best = 0; int64_t at = 0; int pol = 0; int age = 0; kp::cf c; };
    float orr_[16][2] = {};                 // outbound |C| / N per sample (normal, inverted)
    float irr_[16] = {};                    // inbound |rho| per sample

    // --- outbound -----------------------------------------------------------
    struct OutHit { double t0; float w, scale; int pol; };
    std::vector<OutHit> out_hits_;
    Peak opk_;
    bool out_locked_ = false;
    double ot0_ = 0, oT_ = 8, oper_ = 0;    // next slot's symbol-0 start, samples / symbol, period integrator
    float ow_ = 0, oscale_ = 1;             // carrier rotation (rad / symbol), angle scale
    int opol_ = 1;
    int64_t oindex_ = 0;
    bool onew_ = false;
    int omiss_ = 0;                         // slots since the last sync confirmation
    void out_search(int64_t m);
    void out_hit(double pos, int pol, kp::cf c, float rho);
    bool lock_check() const;
    void out_slots();
    float out_demod(double t0, float* soft) const;
    void out_lose();

    // --- inbound ------------------------------------------------------------
    float tmpl_[21] = {};                   // sync template (pair sums), mean removed, unit norm
    float tmpl_raw_[21] = {};
    Peak ipk_;
    struct InHit { double t0; float rho; int pol; float center, scale; };
    std::vector<InHit> in_hits_;
    bool in_grid_ = false;
    bool inew_ = false;                     // grid not confirmed by a delivered burst yet
    double gref_ = 0;                       // grid: start of slot gnext_ = 0
    int64_t gnext_ = 0;                     // next grid slot to examine
    int64_t gbase_ = 0;                     // slot index of the grid's slot 0
    int64_t glast_sync_ = 0, glast_good_ = 0;
    struct Track { bool live = false; double off = 0; float center = 0, scale = 1; int pol = 1; int64_t last = 0; };
    Track tracks_[4];                       // radios seen recently: timing vs the grid, carrier, deviation
    bool in_window(int64_t m) const;
    bool sync_like(double t0, int pol) const;
    void in_search(int64_t m);
    void in_hit(double pos, int pol, float rho);
    void in_start_grid(const InHit& h);
    void in_slots();
    void in_lose();
    // A burst at t0: decisions into dec[6..173]; t0 / center / scale are
    // refined in place; returns the model residual (level units)
    float in_burst(double& t0, float& center, float& scale, int pol, bool synced, int8_t* dec);
    // Sequence detector; returns the mean squared residual (level units)
    float detect(double t0, float center, float scale, int pol, bool synced, int8_t* dec);
    float score(double t0, float center, float scale, int pol) const;
    void in_obs(double t0, float* z) const;
    float in_fit(double t0, const int8_t* dec, int pol, float& center, float& scale) const;
    bool in_plausible(double t0, const int8_t* dec, float center, float scale) const;
    static constexpr float H0 = 10.84f, H1 = 489.2f;   // CPM integrate-and-dump taps (Hz per level unit)
    std::vector<uint8_t> surv_;             // detector survivors

    float last_freq_ = 0;
};

}  // namespace dig
