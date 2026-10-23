#pragma once

// 🗣 Voice to Text: what is said on the VFOs, transcribed to text by a local,
// free speech-to-text model (whisper.cpp - transcribe/, set up with
// transcribe/setup.sh; nothing leaves the machine).
//
// Per VFO ("Transcribe" on its card, DecimatorInstance::transcribe, saved in
// the VFO snapshot). The audio is the VFO's own - not the 🔊 live audio - so
// any number of VFOs are transcribed at once, whatever is being listened to:
//  - a VFO with its digital decoder on: the decoder's voice (P25 / DMR /
//    D-STAR / NXDN / TETRA vocoder output, dig::set_voice_tap_handler), cut
//    into transmissions by the voice frames and the talker (unit ID) changes;
//  - otherwise its analog demodulation (NBFM / AM / WBFM - the VFO's demod)
//    of the stream the FM audio gets, cut by the VFO's squelch (or, with the
//    squelch off, by a signal 10 dB over the noise floor in the FFT).
// Each transmission (>= 0.4 s; one longer than 25 s cut into parts at its
// pauses - continuous signals such as broadcast FM in pieces of ~10 s) is queued for
// the helper process transcribe/build/kraken_whisper (one at a time, at nice
// 10; voice2text_wire.hpp), which skips non-speech with Silero VAD and
// returns the text. Transcripts carry the time, VFO, frequency, source,
// talker and - in coherent mode with DoA on - the VFO's bearing during the
// transmission. They go to the history (transcripts.jsonl, the UI's 🗣 Voice
// tab), the decoder data log (type "voice") and, with "Keep audio", an Ogg
// Opus clip each in the decoder log's folder (voice_clips/YYYY-MM-DD/, kept
// as long as the log files) playable from the tab.

#include <complex>
#include <cstdint>
#include <string>

enum class DemodulatorMode;

namespace v2t {

constexpr int RATE = 16000;                    // what the model takes
constexpr double MIN_UTTERANCE_S = 0.4;        // shorter: not sent
constexpr float AUTO_SQUELCH_DB = 10.0f;       // squelch off: "signal" = 10 dB over the noise floor

void start();   // worker thread + the digital decoders' voice tap
void stop();    // shutdown: the helper process ends, the history is saved

// --- settings (persisted, control_handler.cpp: VOICE2TEXT_*) ---
// model name as in the catalogue / file name: "base.en" = ggml-base.en.bin in
// the models folder (KRAKEN_WHISPER_MODELS, else <transcribe dir>/models)
bool set_model(const std::string& name, std::string* err);
bool set_language(const std::string& code);   // "auto" or a language code ("en", "de", ...)
void set_threads(int n);                      // 1-4
bool set_prompt(const std::string& p);        // vocabulary hint (callsigns, places), <= 300 chars
void set_vad(bool on);
void set_translate(bool on);                  // multilingual models: English text
void set_keep_audio(bool on);                 // a WAV clip per transcript (voice_clips/)
// A VFO's Transcribe tick changed (DecimatorManager::setTranscribe); also
// starts / stops the helper process
void vfo_changed(int vfo, bool on);

// --- audio in ---
// Decimation pass: a VFO with Transcribe on and no digital decoder of voice
// (none, or one like analog_id / pocsag) - its decimated samples (beamformer
// output when beamforming ran, else the listened-to channel), RF centre,
// demod, and whether its squelch is open for this block. tid / tlabel: the
// decoder's talker now (analog_id: "CTCSS 88.5" - the transcript is named after
// it, another one taking over splits it), "" = none. Copies, never blocks
// (queue capped at ~2 s per VFO).
void feed_analog(int vfo, const std::complex<float>* x, size_t n, float rate_hz, double rf_hz,
                 DemodulatorMode mode, bool open, const std::string& tid = "", const std::string& tlabel = "");

// --- out (pushed to the pages on TOPIC_CTL) ---
// {"voice2text":{"status":{...}}}: helper / model state, models installed,
// catalogue, settings, queue, per-VFO state, statistics
std::string status_message();
// {"voice2text":{"history":[...]}}: the last `max` transcripts
// the newest max transcripts [..] - no side effect (the AI assistant's tool)
std::string history_json(size_t max);
std::string history_message(size_t max);
// {"voice2text":{"new":[...]}} with the transcripts since the last call, or ""
std::string take_new_message();
void clear_history();
// {"voice2text":{"lobe":{id, t, vfo, mhz, dur, lobe, lf, doa, brg, slat, slon}}}
// (VOICE2TEXT_LOBE:<id>): the transcript's averaged DoA lobe - b64 of 720 u8,
// 0.5 deg bins clockwise from north (lf "n") or from the array's front (lf
// "a": no heading then) - and where the station was; or {id, error}
std::string lobe_message(uint64_t id);
// HTTP /voice_clip/<id>.opus: the clip's file ("" = none)
std::string clip_path(uint64_t id);
// Download a model of the catalogue into the models folder (curl, in the
// background; progress in the status). false + err: unknown / already running
bool download_model(const std::string& name, std::string* err);

}  // namespace v2t
