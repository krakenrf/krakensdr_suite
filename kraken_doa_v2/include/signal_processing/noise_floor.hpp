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
// remembered floor away. Without that test (independent / wideband scan mode)
// a floor more than 6 dB up is taken as a signal and creeps up 1 dB a minute.

#include <complex>
#include <cstddef>
#include <vector>

namespace nfloor {

// FFT worker, once per frame of channel ch, under fft_mutex: this frame's dB
// spectrum (raw) and the averaged one (avg) - fft_size bins each, DC centred.
// reset: the averaged spectrum restarted with this frame (retune).
void on_frame(int ch, const std::vector<float>& raw, const std::vector<float>& avg,
              int fft_size, bool reset);

// The floor to subtract from channel ch's AVERAGED spectrum (dB). False when
// there is no recent frame of that channel: measure it from the data then
// (FFTProcessor::calculate_noise_floor), as before.
bool floor_db(int ch, float* out);

// Coherent mode, the data receiver: a second antenna of the FFT channel's
// packet is wanted for the coherence test (a few times a second).
bool coherence_due();
// The coherence test on one packet of antennas a (the FFT channel) and b.
void measure_coherence(int a, const std::complex<float>* xa, const std::complex<float>* xb, size_t n);

void load();        // noise_floors.json (cwd), at startup
void maybe_save();  // FFT worker: writes the file now and then when it changed
void save_now();    // at shutdown

}  // namespace nfloor
