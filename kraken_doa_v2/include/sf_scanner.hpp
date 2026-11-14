#pragma once

// 📻 Saved-frequency scanner (sidebar "📻 Saved Frequencies": ▶ Scan). Scans
// the frequencies the scan list takes (saved_freqs.hpp: ticked one by one or
// with their category) and puts ONE VFO - the page's selected one - on each
// that is active, received the way the entry says (demod, bandwidth, decoder;
// the gain with its band), while it is active plus the delay after it ends.
//
// How (sf_scan_plan.hpp): the entries are grouped into tuner BANDS - one tuner
// centre holds every entry whose VFO fits the usable (edge-clipped) part of
// its 2.4 MHz - and every entry of a band is watched at once in the averaged
// FFT (level = strongest bin of its channel over the noise floor, squelch in
// dB). Only a change of band retunes:
//  - coherent mode: the array is one receiver - one band at a time, the
//    bands taking turns every Dwell when there are several. heimdall
//    recalibrates after each retune: nothing is measured while it does (the
//    noise source on / phase compensation not settled) nor until the
//    spectrum's averaging has rebuilt. DoA follows the VFO as usual
//  - independent mode: every tuner no OTHER VFO is on watches a band of its
//    own (5 tuners = 5 bands at once, no calibration); with more bands than
//    tuners they take turns
//  - wideband scan mode: one band = heimdall's tuner spread (n tuners side
//    by side, edge clip x 2.4 MHz apart: ~10.8 MHz with 5), each entry
//    watched on the tuner nearest it (where the receiver puts its VFO) with
//    its VFO kept off the seams; retuned with WIDEBAND_FREQ, no calibration
// Not with the other scanners or a playback. A manual retune (FREQ,
// TUNER_FREQ of a scanned tuner, WIDEBAND_FREQ) or a mode change stops it;
// so does removing its VFO.
//
// While nothing is active its VFO is muted; locked on a frequency it is heard
// (🔊). Stop leaves the VFO where it is - heard if it was listening, else as
// it was before the scan. The scan's own retunes / VFO changes are not saved
// while it runs (set_on_stop persists the live tuning when it ends).
//
// One VFO per active frequency (SFSCAN_MULTI:1): every frequency active at
// the same time in the band(s) watched is listened to at once - the scan's
// VFO takes the first, a VFO the scan makes each further one (a copy of the
// scan VFO as it was at the start: demod, bandwidth, decoder, squelch,
// Transcribe - then the entry's own settings), so they are all heard,
// decoded and transcribed together. Each ends on its own (Delay, Longest
// stay; Skip ends all); a made VFO is removed then and at the stop (never
// saved). At most DecimatorManager's 16 VFOs; the band changes only when
// none is listened to any more.
//
// Commands (control_handler.cpp, uWS loop), answered to every page with
// status_json():
//   SFSCAN_START:<vfo id>   (-1 = the first VFO)
//   SFSCAN_STOP | SFSCAN_SKIP (leave the frequency - not taken again until it
//   has been quiet - or the band) | SFSCAN_HOLD:0|1 (stay on the frequency /
//   band) | GET_SFSCAN
//   SFSCAN_SQUELCH:<dB 3-40> | SFSCAN_DWELL:<ms 500-60000> (per band when the
//   bands take turns) | SFSCAN_HANG:<ms 0-60000> (delay after a transmission)
//   | SFSCAN_MAX:<ms 0-3600000> (longest stay on one frequency, 0 = no limit)
//   | SFSCAN_MULTI:0|1 (one VFO per active frequency) - persisted settings

#include <functional>
#include <string>

namespace sfscan {

std::string start(int vfo);                 // "" = started, else why not
bool stop(const std::string& why = "");     // uWS loop; true = it was running
void shutdown();                            // main, at exit: the thread only
bool running();
bool dispatching();                         // uWS loop: one of the scan's own commands is running
bool uses_tuner(int ch);                    // independent mode: tuner ch is scanned
void skip();
void hold(bool on);
// key squelch | dwell | hang | max | multi: "" = taken (*applied = the command as
// applied, for the settings file), else why not
std::string set(const std::string& key, double value, std::string* applied);
std::string status_json();
void set_on_stop(std::function<void()> fn);   // called on the uWS loop after a stop

}  // namespace sfscan
