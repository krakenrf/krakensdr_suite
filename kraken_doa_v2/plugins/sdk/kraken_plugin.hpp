#pragma once

// KrakenSDR decoder plugin API.
//
// A plugin is ONE directory under kraken_doa_v2/plugins/<id>/ holding C++
// source (decoder.cpp plus any other .cpp/.hpp files). `make` (or
// `make -C plugins PLUGIN=<id>`) links it with plugin_host.cpp into a small
// executable, plugins/<id>/build/decoder, which kraken_doa starts as a child
// process for every VFO that selects the plugin in its Digital Decoder. The
// same executable decodes recordings offline (`decoder --file x.cf32`), so a
// decoder is tested exactly as it runs live. Running out of process means a
// crashing or hanging plugin cannot take the receiver down.
//
// A decoder implements kp::Decoder and registers itself with KRAKEN_PLUGIN:
//
//   #include "kraken_plugin.hpp"
//   class MyDecoder : public kp::Decoder {
//   public:
//       using Decoder::Decoder;
//       void process(const kp::cf* x, size_t n) override { ... }
//   };
//   KRAKEN_PLUGIN(MyDecoder, {.id = "mydec", .name = "My decoder",
//                             .description = "...", .sample_rate = 48000})
//
// process() receives the VFO's complex baseband (centred on the VFO,
// resampled to Info::sample_rate) in blocks of a few ms. Everything a decoder
// learns goes out through `host` (kp::Host): facts (a key/value table shown
// in the sidebar), events (a timestamped log), valid() for every frame that
// passed its checks (drives the "receiving" state), and optional 8 kHz audio.
// See plugins/SDK.md for the full guide and kraken_dsp.hpp for DSP helpers.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kp {

using cf = std::complex<float>;

// A setting the panel shows for this decoder (sent with Decoder::option()).
// choices "" = free text, else "value=Label|value=Label|..." (a list).
// The value "" means "default" (the user cleared it).
struct Option {
    const char* key = "";           // [a-z0-9_], <= 32 chars
    const char* label = "";
    const char* def = "";           // default value
    const char* choices = "";
    const char* help = "";
};

struct Info {
    const char* id = "";            // directory name: [a-z0-9_-], <= 32 chars
    const char* name = "";          // shown in the mode list, e.g. "POCSAG"
    const char* description = "";   // one line (the panel's help text)
    const char* version = "1.0";
    double sample_rate = 48000;     // complex input rate process() gets (Hz)
    double min_vfo_rate = 12500;    // VFO bandwidth the signal needs (Hz) - the UI warns below it
    const char* author = "";
    // Deprecated and ignored: which decoders run in the Digital Decoder's
    // "Auto detect" is the user's choice (the Auto detect ticks in the
    // sidebar's plugin list; every plugin takes part by default). Kept so
    // plugins that set it still compile.
    bool auto_detect = false;
    std::vector<Option> options;    // settings shown in the panel
};

class Host {
public:
    virtual ~Host() = default;
    // Channel fact (sidebar table): set or update a value; "" removes the key
    virtual void fact(const std::string& key, const std::string& value) = 0;
    // Log line. Identical texts within dedup_s seconds are dropped.
    virtual void event(const std::string& text, double dedup_s = 2.0) = 0;
    // A frame/message passed its checks (sync + FEC/CRC). Drives the
    // "receiving" state and the frame counter - call it ONLY for frames you
    // are confident in, never for every sync candidate.
    virtual void valid() = 0;
    // Decoded audio, 8 kHz mono, nominal +-1. Only played while the user
    // listens to this VFO with Demod "Digital"; cheap to skip otherwise
    // (see voice_wanted()).
    virtual void audio(const float* pcm8k, size_t n) = 0;
    // Short voice/call status for the UI ("" = none), e.g. "Call from 1234"
    virtual void voice_state(const std::string& s) = 0;
    // Measured carrier offset (Hz, + = signal above the VFO centre). The host
    // slowly steers its frequency correction from it. Optional.
    virtual void freq_error(float hz) = 0;
    virtual bool verbose() const = 0;        // sidebar "verbose" option
    virtual bool voice_wanted() const = 0;   // someone is listening
    // Seconds of input processed so far (sample clock - deterministic in
    // offline tests, unlike the wall clock)
    virtual double time() const = 0;
    // Debug output (stderr; shown in the plugin's log tail). Do NOT print to
    // stdout: in live mode it carries the binary protocol (the host redirects
    // printf/std::cout to stderr anyway).
    virtual void log(const std::string& text) = 0;
};

class Decoder {
public:
    explicit Decoder(Host& h) : host(h) {}
    virtual ~Decoder() = default;
    // n complex samples at Info::sample_rate
    virtual void process(const cf* x, size_t n) = 0;
    // Drop all state (VFO retuned / decoder restarted)
    virtual void reset() {}
    // An option: "verbose" = "0"|"1", or one of Info::options ("" = default)
    virtual void option(const std::string& key, const std::string& value) { (void)key; (void)value; }
protected:
    Host& host;
};

}  // namespace kp

// Defined by KRAKEN_PLUGIN in the plugin's source
kp::Info kp_plugin_info();
std::unique_ptr<kp::Decoder> kp_plugin_create(kp::Host& host);

#define KRAKEN_PLUGIN(CLASS, ...)                                                          \
    kp::Info kp_plugin_info() { return kp::Info __VA_ARGS__; }                             \
    std::unique_ptr<kp::Decoder> kp_plugin_create(kp::Host& host) {                        \
        return std::make_unique<CLASS>(host);                                              \
    }

// ---------------------------------------------------------------------------
// Wire protocol between kraken_doa and a plugin process (--serve mode).
// Not needed by plugin authors - plugin_host.cpp and kraken_doa implement it.
// Every message: uint32 type, uint32 payload length (little-endian), payload.
// ---------------------------------------------------------------------------
namespace kp::wire {
constexpr uint32_t MAX_PAYLOAD = 1u << 20;
// kraken_doa -> plugin
constexpr uint32_t SAMPLES = 1;        // complex float32 pairs at Info::sample_rate
constexpr uint32_t OPTION = 2;         // "key=value"
constexpr uint32_t RESET = 3;          // empty
constexpr uint32_t VOICE_WANTED = 4;   // 1 byte, 0/1
constexpr uint32_t SYNC = 5;           // uint32 id: answered with SYNC_DONE once everything before it is processed
// plugin -> kraken_doa
constexpr uint32_t FACT = 16;          // key '\0' value ("" value = remove)
constexpr uint32_t EVENT = 17;         // float32 dedup_s, text
constexpr uint32_t VALID = 18;         // empty
constexpr uint32_t AUDIO = 19;         // float32 samples, 8 kHz
constexpr uint32_t VOICE_STATE = 20;   // text
constexpr uint32_t FREQ_ERROR = 21;    // float32 Hz
constexpr uint32_t SYNC_DONE = 22;     // uint32 id of the SYNC
}  // namespace kp::wire
