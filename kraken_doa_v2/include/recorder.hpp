#pragma once

// 📼 Recorder (sidebar "📼 IQ & Audio Recorder"): records to the disk of this
// device either
//  - IQ (coherent mode): every antenna, as a SigMF collection (sigmf.hpp,
//    SIGMF.md) - "full" = heimdall's 8091 stream itself (2.4 MHz, cu8, the
//    phase-calibrated bytes), "band" = one VFO's decimated band on every
//    antenna (ci16 at the VFO's rate: small enough for an SD card), or
//  - audio: the demodulated audio of one or more VFOs (their own demod - NBFM /
//    AM / WBFM - or their digital decoder's voice), one WAV or Ogg Opus file
//    per VFO.
// A recording is limited by a time (10 s .. 24 h) or none ("Unlimited") and
// always by the disk: it doesn't start when the projected size doesn't fit
// (keeping a reserve free) and stops when the free space reaches the reserve
// or a file reaches the file system's limit (FAT32: 4 GiB). A disk slower than
// the data (an SD card takes ~8 MB/s, full-band IQ of 5 antennas is 24 MB/s)
// is buffered in memory: the start is refused when the buffer can't hold the
// difference for the whole recording, and the rest is written after the end.
//
// The recordings live in one folder (RECORDER_DIR, default "recordings",
// cwd-relative), one sub-folder each: <name>/ = a SigMF collection (IQ) or the
// audio files of one recording; uploaded SigMF archives as <name>.sigmf. The
// page lists them, plays IQ recordings back through the receiver (playback.hpp
// - "Open"), downloads them (IQ as one .sigmf archive made on the fly) and
// deletes them.

#include <atomic>
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

enum class DemodulatorMode;

namespace rec {

void start();   // worker threads
void stop();    // shutdown: an open recording is finished (metadata written)

// RECORDER_* (uWS loop). false + *err = refused. Settings (persisted:
// settings_store.cpp) return the value as applied in *applied
bool handle_command(std::string_view m, std::string* err, std::string* applied);
bool is_setting(std::string_view m);

// {"recorder":{...}} when it changed / every second while recording, and the
// list {"recordings":{...}} when it changed; "" when nothing (1 Hz timer)
std::vector<std::string> tick_messages();
std::string status_message();
std::string list_message();

// true while an IQ recording runs (playback refuses to start then)
bool iq_recording();
// a recording in the folder by its listed name (folder / .sigmf archive) -> its path, "" = none
std::string item_path(const std::string& name);
std::string recording_name();   // the one being written ("" = none)

// --- IQ taps ---
// 0 = none, 1 = full band, 2 = a VFO's band (g_band_vfo)
extern std::atomic<int> g_iq_mode;
extern std::atomic<int> g_band_vfo;
inline bool full_active() { return g_iq_mode.load(std::memory_order_relaxed) == 1; }
inline int band_vfo() {
    return g_iq_mode.load(std::memory_order_relaxed) == 2 ? g_band_vfo.load(std::memory_order_relaxed) : -1;
}

struct PacketInfo {
    uint32_t phase_state = 0;   // low byte (heimdall PhaseCompensatorState)
    bool noise = false;         // noise source on (calibration burst)
    bool retuning = false;      // heimdall retunes: the samples are noise - not recorded
    const float* freq_hz = nullptr;   // per channel, from the header (float32)
    const float* gain_db = nullptr;
};
// data receiver thread: one 8091 packet (channel-major uint8 IQ)
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const PacketInfo& pi);
// decimation thread: the band VFO's block of every antenna. seq = the raw
// packet's number (a jump = packets lost before the decimation)
void on_band_block(int vfo, uint64_t seq, const std::complex<float>* const* ch, int m, size_t n, float rate_hz,
                   double rf_hz, float offset_hz, bool noise, uint32_t phase_state);

// --- audio taps ---
bool audio_active();
bool audio_wants(int vfo);
// decimation thread: the VFO's stream (as the FM audio / the decoders get it)
void feed_audio(int vfo, const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, DemodulatorMode mode,
                bool squelch_on, bool squelch_open, bool digital_voice);
// a digital decoder's voice (8 kHz), its worker thread
void feed_voice(int vfo, const float* x8k, size_t n);

// --- HTTP (websocket_server.cpp) ---
// a download: the parts that make up the file, in order
struct Part {
    std::string mem;          // bytes (tar headers, padding) ...
    std::string path;         // ... or a range of a file
    uint64_t off = 0, len = 0;
};
struct Download {
    std::string filename, content_type;
    uint64_t total = 0;
    std::vector<Part> parts;
};
// rel = "<name>" (a recording: IQ -> .sigmf archive, an archive as it is) or
// "<name>/<file>" (one file of it). false: *status ("404 Not Found" ...) + *err
bool download(const std::string& rel, Download* d, std::string* status, std::string* err);

// an upload of a SigMF archive (<name>.sigmf) into the folder, written by a
// thread of its own (an SD card must not block the uWS loop)
struct Upload;
Upload* upload_begin(const std::string& filename, uint64_t length, std::string* status, std::string* err);
// queue a chunk (copied). *pause = true: much is queued - pause the socket;
// the function given to upload_on_drain then runs once (on the writer's
// thread) when it has been written. false = the upload failed (*err)
bool upload_data(Upload* u, std::string_view chunk, bool* pause, std::string* err);
void upload_on_drain(Upload* u, std::function<void()> f);
// the last chunk is queued: the writer checks the archive, renames it into
// place and calls done(ok, message) on its thread. u is gone afterwards
void upload_finish(Upload* u, std::function<void(bool, std::string)> done);
void upload_abort(Upload* u);   // the connection dropped; u is gone

}  // namespace rec
