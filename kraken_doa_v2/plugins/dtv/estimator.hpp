#pragma once

// A standard's CIR estimator: takes a burst of every antenna's samples,
// finds the signal's known reference in it and emits each antenna's channel
// impulse response (sfn_core.hpp CirSnapshot).

#include <functional>
#include <string>
#include <vector>

#include "kraken_plugin.hpp"
#include "sfn_core.hpp"

namespace sfn {

struct Burst {
    int m = 0;
    size_t n = 0;
    double rate = 0;
    double t0 = 0;                          // Host::time() of sample 0
    double rf_hz = NAN;                     // RF at 0 Hz (the VFO centre), NAN = unknown
    std::vector<std::vector<kp::cf>> ch;    // m x n
};

// options the estimators read (decoder.cpp)
struct EstOptions {
    double bw_mhz = 0;                      // TV channel bandwidth, 0 = try all
    double channel_hz = NAN;                // TV channel centre, NAN = from the signal / raster
    bool verbose = false;
};

using Sink = std::function<void(const CirSnapshot&)>;

class Estimator {
public:
    virtual ~Estimator() = default;
    virtual std::string name() const = 0;   // "DVB-T"
    // How well this burst looks like the standard (0 = not at all, ~1 =
    // clearly); prepares the lock if it does. Called while not locked.
    virtual double detect(const Burst& b, const EstOptions& o) = 0;
    // A burst while locked: CIR snapshots to sink; false = lock lost
    virtual bool process(const Burst& b, const EstOptions& o, const Sink& sink) = 0;
    virtual std::string describe() const = 0;   // the mode found ("8k, 1/4, 8 MHz, PP2")
    virtual void reset() = 0;
    // transmitter identifiers the standard sends, for SfnCore::set_ids (none
    // of the TV standards here; DAB's TII used it)
    virtual bool has_ids() const { return false; }
    virtual std::vector<IdHint> ids() const { return {}; }
    // What the signal's own signalling says (DVB-T TPS, ISDB-T TMCC) and what
    // was measured: label / value lines for the facts, empty = nothing yet
    virtual std::vector<std::pair<std::string, std::string>> info() const { return {}; }
    // decoded / changed signalling since the last call, for the event log
    virtual std::vector<std::string> take_news() { return {}; }
    // the channel's centre frequency measured on the signal (Hz), NAN = unknown
    virtual double centre_hz() const { return NAN; }
    // the channel bandwidth of the mode found (MHz), 0 = unknown
    virtual double bw_mhz() const { return 0; }
    // one antenna's CIR (echo profile) holds estimator artefacts this far
    // below the strongest path: weaker paths aren't marked (0 = none known;
    // with every antenna the spatial checks remove them)
    virtual double profile_floor_db() const { return 0; }
};

std::unique_ptr<Estimator> make_dvbt(kp::Host& h);
std::unique_ptr<Estimator> make_dvbt2(kp::Host& h);
std::unique_ptr<Estimator> make_isdbt(kp::Host& h);
std::unique_ptr<Estimator> make_atsc3(kp::Host& h);
std::unique_ptr<Estimator> make_atsc(kp::Host& h);
std::unique_ptr<Estimator> make_dtmb(kp::Host& h);

}  // namespace sfn
