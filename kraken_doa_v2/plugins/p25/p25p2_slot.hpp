#pragma once

// P25 Phase 2 (two-slot TDMA, 6000 symbols/s): the interface between the
// physical layer (p25p2_phy.cpp: H-DQPSK outbound / H-CPM inbound receiver,
// sync, the 30 ms slot grid) and the MAC layer (p25p2.cpp: ISCH, DUID,
// scrambling, Reed-Solomon, MAC PDUs, voice). The receiver hands over one
// timeslot at a time.

#include <cstdint>

namespace dig {

constexpr int P25P2_SLOT_SYMS = 180;     // one timeslot: 30 ms, 360 bits
constexpr int P25P2_SPS = 8;             // samples per symbol at the plugin's 48 kHz
constexpr int P25P2_SLOT_SAMPLES = P25P2_SLOT_SYMS * P25P2_SPS;   // 1440

struct P25p2Slot {
    // false: a timeslot of an outbound (base station, H-DQPSK, continuous)
    // signal. true: a subscriber's inbound burst (H-CPM); only slots that
    // hold a burst are delivered.
    bool inbound = false;
    // Running timeslot counter on the receiver's slot grid: +1 every 30 ms
    // for as long as the grid holds, so two slots' index difference is their
    // distance in timeslots (mod 12 = their distance in the superframe; the
    // MAC layer finds which index is superframe slot 0). Undelivered slots
    // (inbound: no burst) still count.
    int64_t index = 0;
    // The grid was (re)acquired with this slot: its index is unrelated to
    // those delivered before.
    bool grid_new = false;
    // A sync word was correlated in or next to this slot (outbound: an
    // S-ISCH at either end or the S-OEMI sync; inbound: the burst's own
    // 22-symbol sync). false = its position is extrapolated from the grid.
    bool synced = false;
    // Outbound only: pre[] holds the 10 symbols before the slot - the first
    // half, ISCH(39..20), of the ISCH that describes this slot (its second
    // half is sym[0..9]). false = not received (the first slot of a grid).
    bool have_pre = false;
    float pre[10] = {};
    // The slot's symbols 0..179 in transmission order, in level units: the
    // nominal values are +-1 / +-3 (dibit = dig::SymSrc::dibit(): +3 -> 01,
    // +1 -> 00, -1 -> 10, -3 -> 11). Outbound: soft values. Inbound: the
    // sequence detector's decisions (exactly +-1 / +-3); symbols 0-5 and
    // 174-179 (ramp / guard) are 0.
    float sym[P25P2_SLOT_SYMS] = {};
    // The slot's input samples [start, end): indexes into the receiver's
    // 48 kHz input since its last reset, the receiver's own filter delays
    // taken off. start = where symbol 0's symbol period begins, end = start
    // + P25P2_SLOT_SAMPLES (nominally); symbol k spans start + 8 k ..
    // start + 8 (k + 1).
    int64_t start = 0, end = 0;
    // 0..1: how well the slot's symbols sit on the nominal levels (1 =
    // clean). For display / gating weak slots; the MAC layer's codes decide.
    float quality = 0;
    // Carrier offset the receiver measured for this slot / burst (Hz, + =
    // above the input's centre). The MAC layer reports it for slots that
    // passed its checks (RxContext::freq_error).
    float freq_hz = 0;
};

class P25p2SlotSink {
public:
    virtual ~P25p2SlotSink() = default;
    virtual void p2_slot(const P25p2Slot& s) = 0;
    // The receiver gave up its slot grid (the signal is gone): slot indexes
    // delivered from now on belong to a new grid.
    virtual void p2_grid_lost() {}
};

}  // namespace dig
