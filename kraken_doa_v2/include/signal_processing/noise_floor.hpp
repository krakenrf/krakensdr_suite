#pragma once

// The spectrum's 0 dB reference ("noise floor") per tuner channel.
//
// Every spectrum is shown, squelched and scanned relative to its noise floor,
// measured per frame as the 10th percentile of the centre 80 % of the bins
// (FFTProcessor::calculate_noise_floor). A signal wider than the 2.4 MHz a
// tuner sees (DVB-T, LTE) fills those bins: the percentile lands on the signal,
// it was drawn at ~0 dB and an FFT squelch inside it never opened. So the
// receiver's OWN floor is remembered per tuner and gain (noise_floors.json -
// with heimdall's IF VGA fixed it depends on the gain, hardly on the frequency
// or what the band holds), and a frame whose floor is far above it is measured
// against the remembered floor instead. In coherent mode two antennas decide:
// receiver noise is independent per antenna, a signal isn't (the coherence of
// the band's quietest bins) - so a real rise of the floor (an active antenna,
// interference) is learned at once and a long stay on DVB-T never wears the
// remembered floor away. Without that test (independent mode) a floor more
// than 6 dB up is taken as a signal and creeps up 1 dB a minute.
// Wideband scan mode (tuners side by side, one stitched spectrum) doesn't use
// the remembered floors - they are per tuner and frequency-blind, and the
// tuners' gains and the external noise differ: it compares the tuners with
// each other at the same moment. Their common response shape (the filters'
// tilt and roll-off) is taken off, the gain steps between neighbours come
// from the frequencies their bands share, and a tuner whose floor stands well
// above the quietest one's (gains taken off) is filled by a signal (DVB-T, LTE
// over several tuners - its own percentile is the signal, so it was drawn at
// ~0 dB past the boundary): it takes the common floor + its gain, blended in
// over 1 -> 3 dB, so a signal sliding over a tuner in a drag keeps its level.
// The others keep their own floor, so their noise lines up exactly.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nfloor {

// FFT worker, once per frame of channel ch, under fft_mutex: this frame's dB
// spectrum (raw) and the averaged one (avg) - fft_size bins each, DC centred.
// reset: the averaged spectrum restarted with this frame (retune). fc_hz: the
// RF the averaged spectrum's centre bin holds (FFTProcessor's shift of it on a
// retune; 0 = unknown: the tuner's latest header frequency).
void on_frame(int ch, const std::vector<float>& raw, const std::vector<float>& avg,
              int fft_size, bool reset, double fc_hz = 0.0);

// The floor to subtract from channel ch's AVERAGED spectrum (dB) - in
// wideband scan mode the one the stitched spectrum (after shape_db) and the
// VFO squelch on that tuner share. False when there is no recent frame of that
// channel: measure it from the data then (FFTProcessor::calculate_noise_floor),
// as before.
bool floor_db(int ch, float* out);

// Wideband scan mode: the tuners' common response (dB) at each bin of an
// fft_size spectrum (DC centred) - the stitched spectrum takes it off before
// floor_db. Copied into out when it changed since *version (updated).
void shape_db(int fft_size, std::vector<float>& out, uint32_t* version);

// Coherent mode, the data receiver: a second antenna of the FFT channel's
// packet is wanted for the coherence test (a few times a second).
bool coherence_due();
// The coherence test on one packet of antennas a (the FFT channel) and b.
void measure_coherence(int a, const std::complex<float>* xa, const std::complex<float>* xb, size_t n);

void load();        // noise_floors.json (cwd), at startup
void maybe_save();  // FFT worker: writes the file now and then when it changed
void save_now();    // at shutdown

}  // namespace nfloor
