#pragma once

// 📻 Saved-frequency scanner, the part without receiver state (sf_scanner.hpp
// runs it): which tuner bands the scan list needs and how loud an entry is in
// a spectrum. Pure functions - tested offline.
//
// A BAND is one tuner centre that holds several entries at once: every entry's
// VFO (its bandwidth around its frequency) lies inside the usable part of the
// tuner's 2.4 MHz (the edge clip), so all of them are watched from the same
// spectrum and only a change of band retunes. Entries that set a gain only
// share a band with the same gain (the band's gain is set with its retune);
// entries without one ("keep the receiver's") join any band they fit in.

#include "saved_freqs.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sfscan {

struct Band {
    int64_t centre_hz = 0;      // tuner centre, whole kHz
    bool has_gain = false;      // false = the receiver's gain is kept
    double gain = 0;            // dB, -1 = Auto (AGC)
    std::vector<int> entries;   // indices into Plan::entries, lowest first
};

struct Skipped {
    std::string id, name, why;
};

struct Plan {
    std::vector<savedfreq::ScanEntry> entries;   // the scannable ones, lowest first
    std::vector<int> band_of;                    // per entry: its band
    std::vector<Band> bands;                     // lowest centre first
    std::vector<Skipped> skipped;                // not scanned, and why
};

// The VFO bandwidth an entry is received with (Hz): its own, else its demod's
// usual one (what the page's demod buttons set: WBFM 240 kHz, NBFM / AM 16
// kHz), else the VFO's present one (vfo_hz)
int64_t vfo_width_hz(const savedfreq::ScanEntry& e, int64_t vfo_hz);

// The bandwidth option index (config.hpp BANDWIDTH_OPTIONS) nearest hz
int bandwidth_index_for(int64_t hz);

// The width whose inner part is measured for activity: its bandwidth, else
// 180 kHz for WBFM, else a 12.5 kHz channel
int64_t channel_width_hz(const savedfreq::ScanEntry& e);

// span_hz: the usable part of one tuner's band (edge clip x sample rate);
// [min_hz, max_hz]: the tunable range; vfo_hz: the scanning VFO's bandwidth
// now; why_not(dec): "" if the entry's decoder can be scanned, else why not
// (a fixed-frequency / locked-VFO decoder needs a VFO of its own)
Plan build_plan(std::vector<savedfreq::ScanEntry> entries, double span_hz, uint64_t min_hz, uint64_t max_hz,
                int64_t vfo_hz, const std::function<std::string(const std::string&)>& why_not);

// Every entry of band b inside the band centred on centre_hz (span_hz usable)
// with the same gain - the receiver needn't retune for it
bool band_fits(const Plan& p, int b, int64_t centre_hz, double span_hz, int64_t vfo_hz);

// An entry's level in dB over the noise floor: the strongest bin in the inner
// 80 % of its channel (at least +-2 bins) of spec (dB, bin 0 = centre -
// rate / 2). -999 = not inside the spectrum
float entry_level(const std::vector<float>& spec, float floor_db, double centre_hz, double rate_hz,
                  int64_t hz, int64_t channel_hz);

}  // namespace sfscan
