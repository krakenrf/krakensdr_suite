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
};
