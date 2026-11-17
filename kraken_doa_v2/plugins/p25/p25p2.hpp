#pragma once

// P25 Phase 2 (two-slot TDMA) MAC layer (p25p2.cpp), fed one timeslot at a
// time by the physical-layer receiver (p25p2_phy.cpp) through the
// P25p2SlotSink interface (p25p2_slot.hpp): ISCH, DUID, superframe and
// ultraframe tracking, descrambling (the seed from the options, a control
// channel or recovered blindly from a traffic burst), Reed-Solomon with the
// punctured symbols as erasures, CRC-12 / CRC-16, MAC PDUs and messages,
// AMBE+2 voice and ESS, encryption muting, talkers per logical channel.

#include <cstdint>
#include <memory>
#include <string>

#include "dig_common.hpp"
#include "dig_vocoder.hpp"
#include "p25.hpp"
#include "p25p2_slot.hpp"

namespace dig {

class P25p2Proto : public P25p2SlotSink {
public:
    // phase1: the Phase 1 decoder of the same plugin - its TSBK-body parser
    // (Phase 2 partition-01 messages), channel identifier table and network
    // identity (WACN / system ID / NAC) are shared
    P25p2Proto(RxContext& c, P25Proto& phase1);
    ~P25p2Proto() override;
    P25p2Proto(const P25p2Proto&) = delete;
    P25p2Proto& operator=(const P25p2Proto&) = delete;

    void p2_slot(const P25p2Slot& s) override;
    void p2_grid_lost() override;
    // Forget everything learned from the signal (the user's set_network /
    // set_lch stay). Call together with P25Proto::reset() (the host resets
    // its talkers with the plugin).
    void reset();
    // now = the receiver's current sample index (as P25p2Slot::start):
    // ends transmissions whose bursts stopped
    void tick(int64_t now);
    // User options: the system's WACN (20 bits) / system ID (12) / NAC (12),
    // -1 = not given. All three = the scrambling seed; a NAC alone only
    // restricts a blindly recovered seed to that NAC.
    void set_network(int wacn, int sysid, int nac);
    // Audio and display: 0 = both logical channels (auto: the first to talk
    // until it has been quiet for a second), 1 = LCH 0 only, 2 = LCH 1 only
    void set_lch(int which);
    // Phase 2 bursts passed their checks within the last 2 s (sample time)
    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace dig
