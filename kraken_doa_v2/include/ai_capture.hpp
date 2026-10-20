#pragma once

// The AI assistant's IQ capture (MCP tool capture_iq), done inside kraken_doa:
// the data receiver hands each 8091 packet's bytes of the wanted channel(s) to
// a queue (a copy, ~32 KB per antenna per packet), the MCP tool's worker
// thread converts, mixes and decimates them (the VFO decimator's Kaiser
// cascade on liquid's NEON firdecim) into
//   <base>.cf32 + <base>.json         one antenna / tuner, ai/sigtool.py's format
//   <base>_array.cf32 + _array.json   (coherent, all_antennas) every antenna,
//                                     sample-major - a plugin's offline
//                                     `decoder --file <it> --uca R` reads it
// It used to be `ai/sigtool.py capture`: a second 8091 connection (heimdall
// sent its whole 24 MB/s stream twice) and numpy at 46-86 % of a Pi 5 core;
// here one antenna costs a few %, and a capture sees what the receiver
// shows - also a playback (the sidecar says so).
// Packets with the noise source on or retuning are cut out (counted as
// gaps); a retune or an element-count change ends the capture early with
// what was captured.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace aicap {

struct Request {
    double rf_hz = 0;             // centre of the capture (absolute RF)
    double rate_hz = 48000;       // output rate wanted; the actual one = 2.4 MHz / an integer
    double seconds = 10;          // of signal (gaps cut out)
    int antenna = -1;             // coherent: the antenna of <base>.cf32 (-1 = the CH selector's)
    bool all_antennas = false;    // coherent: also <base>_array.cf32
    std::string path;             // <base>.cf32
};

struct Result {
    bool ok = false;
    std::string error;            // !ok
    std::string path, array_path; // written files ("" = none)
    std::string source;           // "antenna 0 of heimdall's stream", "tuner 2 ...", "playback ..."
    std::string stopped;          // why it ended early ("" = the whole length)
    double seconds = 0, rate_hz = 0, bandwidth_hz = 0, center_hz = 0, offset_hz = 0, gain_db = NAN;
    double radius_m = 0;          // UCA radius for `decoder --uca` (0 = not a UCA / unknown)
    int channel = 0, antennas = 1, gaps = 0;
    uint64_t skipped = 0, dropped = 0;
};

// Captures and returns when done. Blocks the calling thread for the capture's
// length: call it from a worker, never the uWS loop or the data receiver.
// One capture at a time (a second one is refused).
Result run(const Request& req);

extern std::atomic<bool> g_active;
inline bool active() { return g_active.load(std::memory_order_acquire); }

// data receiver thread, every parsed 8091 packet while active() (channel-major
// uint8 IQ; freq_hz / gain_db per channel from the header)
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, bool noise, bool retuning,
               const float* freq_hz, const float* gain_db);

}  // namespace aicap
