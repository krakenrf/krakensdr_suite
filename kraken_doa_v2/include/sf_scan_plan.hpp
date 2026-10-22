#pragma once

// 📻 Saved-frequency scanner, the part without receiver state (sf_scanner.hpp
// runs it): which tuner bands the scan list needs and how loud an entry is in
// a spectrum. Pure functions - tested offline.
//
// A BAND is one tuning that holds several entries at once: every entry's VFO
// (its bandwidth around its frequency) lies inside the usable part (the edge
// clip) of a tuner's 2.4 MHz, so all of them are watched from the same
// spectra and only a change of band retunes. Entries that set a gain only
// share a band with the same gain (the band's gain is set with its retune);
// entries without one ("keep the receiver's") join any band they fit in. As
// few bands as the span allows, balanced: the narrowest packing that needs no
// more bands, so entries keep away from the band edges and from DC.
//
// The tuners a band is received with (Layout): one (coherent / independent
// mode), or in wideband scan mode heimdall's tuner spread - n tuners side by
// side, one stitched band n times as wide; an entry is then received on the
// tuner nearest it (as the receiver puts a VFO there), so its VFO must lie
// inside THAT tuner's usable part - not across a seam - and its channel off
// that tuner's DC.

#include "saved_freqs.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sfscan {

// The tuners' centres relative to the band's centre (Hz), each usable half
// either side
struct Layout {
    std::vector<int64_t> offs{0};
    double half = 0;
};
// one tuner, span_hz (edge clip x sample rate) usable
Layout single_layout(double span_hz);
// wideband scan mode: heimdall's setup_wideband_frequencies - tuner k on the
// centre + (k - n / 2) x spacing (integer n / 2), spacing = edge clip x
// sample rate, half = spacing / 2
Layout spread_layout(int tuners, int64_t spacing_hz);
// the tuner of lay nearest hz with the band on centre_hz (the lowest on a tie,
// as the receiver picks a VFO's tuner)
int nearest_tuner(const Layout& lay, int64_t centre_hz, int64_t hz);

struct Band {
    int64_t centre_hz = 0;      // the band's centre (one tuner: its centre; spread: tuner n / 2's), whole kHz
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

// lay: the tuners a band is received with; [min_hz, max_hz]: the tunable
// range (every tuner's centre inside it); vfo_hz: the scanning VFO's
// bandwidth now; why_not(dec): "" if the entry's decoder can be scanned, else
// why not (a fixed-frequency / locked-VFO decoder needs a VFO of its own)
Plan build_plan(std::vector<savedfreq::ScanEntry> entries, const Layout& lay, uint64_t min_hz, uint64_t max_hz,
                int64_t vfo_hz, const std::function<std::string(const std::string&)>& why_not);
// one tuner, span_hz usable (single_layout)
Plan build_plan(std::vector<savedfreq::ScanEntry> entries, double span_hz, uint64_t min_hz, uint64_t max_hz,
                int64_t vfo_hz, const std::function<std::string(const std::string&)>& why_not);

// Every entry of band b inside its tuner's usable part with the band centred
// on centre_hz, channels off DC (or the plan's own centre) - the receiver
// needn't retune for it
bool band_fits(const Plan& p, int b, int64_t centre_hz, const Layout& lay, int64_t vfo_hz);
bool band_fits(const Plan& p, int b, int64_t centre_hz, double span_hz, int64_t vfo_hz);   // single_layout

// An entry's level in dB over the noise floor: the strongest bin in the inner
// 80 % of its channel (at least +-2 bins) of spec (dB, bin 0 = centre -
// rate / 2). -999 = not inside the spectrum
float entry_level(const std::vector<float>& spec, float floor_db, double centre_hz, double rate_hz,
                  int64_t hz, int64_t channel_hz);

}  // namespace sfscan
