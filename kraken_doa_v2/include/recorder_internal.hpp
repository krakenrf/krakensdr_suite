#pragma once

// Shared between recorder.cpp (settings, IQ, files, HTTP) and
// recorder_audio.cpp (the audio recordings) - see recorder.hpp.

#include <cstdint>
#include <string>
#include <vector>

namespace rec::detail {

int64_t now_ms();                        // system clock, ms
std::string name_time(int64_t ms);       // local "20261016_181503"
std::string mhz_tag(double hz);          // "433_920MHz"
uint64_t mem_budget();                   // bytes a recording may buffer in memory

// a recording ended (IQ or audio): the result shown, the list refreshed
void finished(const std::string& kind, const std::string& name, const std::string& reason, uint64_t bytes,
              double seconds, const std::string& error);
void changed();                          // the status changed

struct AudioConfig {
    std::string folder;                  // <dir>/<name> (created by the caller)
    std::string name;
    std::vector<int> vfos;
    bool wav = true;
    int rate = 48000;                    // WAV sample rate
    int kbps = 32;                       // Opus bit rate
    bool skip_closed = false;            // leave out what the squelch closed
    double limit_s = 0;                  // 0 = no time limit
    uint64_t reserve = 0;                // stop when the drive's free space reaches it
    bool fat = false;                    // FAT32: 4 GiB per file (a new file then)
    std::string station, notes;
    bool has_pos = false;
    double lat = 0, lon = 0;
};
bool audio_start(const AudioConfig& c, std::string* err);
void audio_request_stop(const std::string& reason);
bool audio_running();                    // recording or finishing
void audio_shutdown();                   // stop and wait (kraken_doa ends)
std::string audio_status_json();         // the running recording, "" = none
double audio_bytes_per_s(bool wav, int rate, int kbps);   // one VFO's file

}  // namespace rec::detail
