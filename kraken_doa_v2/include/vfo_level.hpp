#pragma once

// 📏 Each VFO's received level, in every operating mode: the VFO's band in
// its tuner's spectrum (coherent: the FFT channel, active_channel;
// independent / wideband scan: the VFO's own tuner) - FFTProcessor::
// band_power, absolute (dBFS: 0 = a full-scale complex tone) - sampled every
// SAMPLE_MS while the signal is above the band's noise (and the VFO's
// squelch, when on, is open), averaged in power (the first few as their
// mean, then an EMA over ~0.5 s), with that tuner's gain of the moment. A
// pause longer than GAP_MS (the next transmission), another gain or a
// retune (> RETUNE_HZ) starts it afresh; after the signal ends it is held,
// its age goes out with it.
//
// Read by: the page's hover readout on each VFO (system_status,
// decimator_squelch[].lvl - every page, 2 Hz) and the 📏 range estimate
// (rdf_mapper.cpp: a VFO's level, and a talker's from the VFO's moments
// while that radio alone transmits). The page turns dBFS into dBm with its
// calibration: dBm = dBFS - gain + C.

#include <cstdint>
#include <ostream>

namespace vlvl {

constexpr int SAMPLE_MS = 100;
constexpr int64_t GAP_MS = 3000;
constexpr double EMA = 0.2;
constexpr double RETUNE_HZ = 10000;

// a level averaged over the moments it was there (linear powers, 1 = full scale)
struct Level {
    double sig = 0;       // signal power (the band without its noise), averaged
    double noise = 0;     // the band's noise, the latest moment
    double total = 0;     // the whole tuner band then (the ADC's load: clipping)
    float gain = 0;       // the tuner gain then (dB; -1 = AGC)
    int64_t t = 0;        // the latest moment counted (now_ms clock; 0: none)
    int n = 0;            // moments in this average
    void add(int64_t now, double s, double nz, double tot, float g);
};

// the latest moment counted for a VFO (seq grows with each - a reader that
// keeps the last seq it saw takes every new one once)
struct Moment {
    double sig = 0, noise = 0, total = 0;
    float gain = 0;
    int64_t t = 0;
    uint64_t seq = 0;
};

void start();
void stop();

int64_t now_ms();   // the clock of Level::t / Moment::t (steady)

bool level(int vfo, Level* out);      // false: no moment yet
bool moment(int vfo, Moment* out);    // false: no moment yet

// ,"lvl":{"dbfs","noise","total","gain","age","n"} - nothing when L has no level
void write_json(std::ostream& o, const Level& L);

}  // namespace vlvl
