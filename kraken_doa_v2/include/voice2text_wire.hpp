#pragma once

// Wire protocol between kraken_doa (voice2text.cpp) and the speech-to-text
// helper transcribe/build/kraken_whisper (transcribe/kraken_whisper.cpp).
// Same framing as the decoder plugins: an 8-byte header - uint32 type,
// uint32 payload length (host byte order) - then the payload. The helper is
// a long-lived child process (the model stays loaded) that takes ONE job at a
// time: kraken_doa sends the next JOB only after the previous RESULT, so the
// queue (and what gets dropped when it overflows) lives in kraken_doa.

#include <cstdint>

namespace v2t::wire {

// kraken_doa -> helper
constexpr uint32_t JOB = 1;      // uint32 job id, then int16 PCM, 16 kHz mono
constexpr uint32_t OPTION = 2;   // "key=value": language (auto|en|de|..), prompt,
                                 // threads (1-4), vad (0|1), vad_threshold (0.1-0.9),
                                 // translate (0|1), beam (0 = greedy | 2-8)

// helper -> kraken_doa
constexpr uint32_t READY = 16;   // '\0'-separated key=value: model, type, multilingual (0|1),
                                 // vad (0|1: VAD model loaded), version, system
constexpr uint32_t RESULT = 17;  // uint32 id, float conf (0-1), float no_speech (0-1),
                                 // uint32 proc_ms, int32 speech_ms (-1 = no VAD),
                                 // then '\0'-separated: lang, text, dropped reason
                                 // ("" = kept; text is "" when dropped)
constexpr uint32_t ERROR = 18;   // text (fatal ones are followed by exit)

constexpr uint32_t MAX_PAYLOAD = 4u << 20;   // 4 MB: > 2 min of int16 audio
constexpr int SAMPLE_RATE = 16000;

}  // namespace v2t::wire
