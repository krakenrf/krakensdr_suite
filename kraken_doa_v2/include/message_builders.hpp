#pragma once

#include "config.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Spectrum views (FFT message type 7): one display's averaged spectrum at full
// FFT resolution, kept for a tick so each page can be sent the part it shows
// (its zoom) - see MessageBuilders::build_spectrum_view
struct SpectrumDisplay {
    bool valid = false;
    double center_hz = 0;       // the display's centre (tuner / stitched centre)
    double f0_hz = 0;           // bin i at f0_hz + i * step_hz
    double step_hz = 0;
    float gain_db = 0;
    std::vector<float> db;      // dB, noise floor = 0
};
struct SpectrumSnapshot {
    bool independent = false;   // displays 1..channels (tuners), else display 0
    uint8_t marker = 0;         // display 0: 0x80 | channel (coherent), 0xFE (wideband scan)
    uint8_t channels = 0;
    float avg_alpha = 0;
    std::array<SpectrumDisplay, 1 + MAX_CHANNELS> disp;  // 0 = main, 1 + ch = tuner ch
};
// One VFO's beamformed spectrum (overlay, FFT message type 4) at the
// beamformer's full resolution, normalized - kept for a tick so each page
// gets it at the point spacing of the spectrum it shows
struct BeamformedSpectrum {
    uint32_t id = 0;
    float center_mhz = 0, bandwidth_mhz = 0;
    float steering_deg = 0, snr_boost_db = 0;
    std::vector<float> db;      // dB, noise floor = 0, DC-centred
};

class MessageBuilders {
public:
    // The legacy spectrum message (types 0 / 6, for pages that asked for no
    // view). Runs once per tick whether or not anyone gets it: it also feeds
    // the scanners and keeps the wideband scan's average. snap: filled with
    // the spectra for the views; legacy = false skips encoding the message
    // ("" returned) when no page wants it
    static std::string build_fft_message(SpectrumSnapshot* snap = nullptr, bool legacy = true);
    // The latest spectra (every display), filled by the websocket timer's
    // build_fft_message every 50 ms tick - the spectrum views and the AI
    // assistant's tools (ai_mcp.cpp) read it. uWS loop thread only
    static SpectrumSnapshot& spectrum_snapshot();
    // When it was last filled (ms since epoch) and its main display's centre
    // (Hz) - any thread (the assistant's tune tool waits for a retune)
    static void note_spectrum();
    static int64_t spectrum_ms();
    static double spectrum_center_hz();
    // Type 7: display (0 = main, 1 + ch = independent-mode tuner ch) between
    // lo / hi Hz from its centre (lo >= hi = the whole band), max-pooled to
    // about `bins` points (0 = SPECTRUM_LEGACY_POINTS), never more than
    // SPECTRUM_VIEW_MAX_POINTS - so a zoomed view is finer, not bigger.
    // point_step_hz: set to the frame's point spacing. "" = no data
    static std::string build_spectrum_view(const SpectrumSnapshot& snap, int display, double lo_off_hz,
                                           double hi_off_hz, uint32_t bins, uint32_t seq,
                                           double* point_step_hz = nullptr);
    static std::string build_audio_message();
    static std::string build_multi_doa_message();
    static std::string build_system_status_message();
    // Beamformed FFT overlay: a decimator's spectrum (false if it has no valid
    // beamformed data - beamforming off, or none produced yet), then type 4
    // max-pooled to about point_step_hz per point - the spacing of the main
    // spectrum the page shows, so the overlay follows its zoom (<= 0: the
    // old-format frames' spacing)
    static bool beamformed_spectrum(int decimator_id, BeamformedSpectrum& out);
    static std::string build_beamformed_fft_message(const BeamformedSpectrum& bf, double point_step_hz);
    // {"decimator_info":[...]} snapshot of every decimator for the UI. In
    // wideband mode offsets are converted to be relative to the wideband
    // center.
    static std::string build_decimator_info_message();
    // {"digital":[...]} status of the VFOs' digital decoders: every VFO with a
    // decoder on and the events since the last push (only_id < 0), or one
    // VFO's state with its whole event log (history = true). "" = nothing.
    static std::string build_digital_message(int only_id = -1, bool history = false);
    // {"map":{...}} for the web UI's 🗺 Map (sent once a second): the station
    // location, the points that changed since the last push of every VFO
    // decoder with "Plot on map" on, and the keys of all live points (pages
    // drop the others). Also hands the station location to the decoder
    // plugins. The next push carries every point after request_map_full()
    // (a page opened the map: GET_MAP, or a decoder's map option changed).
    static std::string build_map_message();
    // {"incidents":{seq, geo, cols, rows}} - "" when nothing changed since the
    // last one (unless force)
    static std::string build_incidents_message(bool force = false);
    static void request_map_full();
};
