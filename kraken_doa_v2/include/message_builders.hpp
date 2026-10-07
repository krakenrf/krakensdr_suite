#pragma once

#include <string>

class MessageBuilders {
public:
    static std::string build_fft_message();
    static std::string build_audio_message();
    static std::string build_multi_doa_message();
    static std::string build_system_status_message();
    // Beamformed FFT overlay for a single decimator (empty string if it has no
    // valid beamformed data). The websocket loop calls this for each decimator.
    static std::string build_beamformed_fft_message(int decimator_id);
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
