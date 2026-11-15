#pragma once
// The audio layer of ONE DAB sub-channel: what comes after the channel
// decoder. The receiver (decoder.cpp and the OFDM / FIC / MSC code) hands it
// the selected sub-channel's bytes, one logical frame (24 ms) at a time, and
// it does the rest:
//
//   DAB  (ASCTy 0):  MPEG-1 / MPEG-2 LSF Audio Layer II frames (ETSI TS 103
//                    466): header + ScF-CRC checks, F-PAD / X-PAD, PCM
//   DAB+ (ASCTy 63): audio super frames (ETSI TS 102 563): Fire code sync,
//                    Reed-Solomon RS(120,110), access units + their CRCs,
//                    PAD from the data stream element, HE-AAC v2 PCM through
//                    a system AAC library loaded at run time (none is
//                    shipped or linked - like the AMBE codec, the user
//                    installs it)
//   both:            the dynamic label (DLS, ETSI TS 102 980) from the PAD
//
// This header is the contract between the receiver and the audio layer;
// neither side includes anything else of the other.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dab {

// What the FIC says about the sub-channel
struct SubchannelConfig {
    bool dabplus = false;    // FIG 0/2 ASCTy: 0 = DAB (Layer II), 63 = DAB+ (HE-AAC v2)
    int bitrate_kbps = 0;    // the sub-channel's bit rate: a logical frame is bitrate_kbps * 3 bytes
    // which stream it is: another service or sub-channel is another stream
    // (configure() then starts from scratch even when codec and bit rate are
    // the same); the receiver keeps them across a multiplex reconfiguration
    // that only moves the sub-channel
    uint32_t sid = 0;
    int subch = -1;

    bool operator==(const SubchannelConfig&) const = default;
};

// What the audio layer reports. Every call happens inside push_frame() /
// push_lost(), on the caller's thread.
struct AudioSink {
    virtual ~AudioSink() = default;
    // decoded audio: 8 kHz mono, +-1 (already downmixed and resampled)
    virtual void pcm(const float* mono, size_t n) = 0;
    // a complete dynamic label that differs from the previous one: UTF-8,
    // NOT yet escaped (the receiver passes it through kp::printable); "" =
    // the broadcaster cleared the display (DL "clear display" command)
    virtual void dynamic_label(const std::string& utf8) = 0;
    // debug output (the receiver sends it to host.log)
    virtual void log(const std::string& text) = 0;
};

struct AudioStatus {
    // what the stream is, once known - e.g. "MPEG-1 Layer II, 48 kHz, joint
    // stereo, 128 kbit/s" or "HE-AAC v2 (SBR + PS), 48 kHz, stereo"; "" = not yet
    std::string codec;
    // the PCM decoder for THIS stream, for a "Voice codec" fact - e.g.
    // "built in", "libfdk-aac.so.2 (2.0.2)", "not installed (libfdk-aac2)"
    std::string decoder;
    bool can_decode = false;   // PCM comes out when want_audio is set
    bool synced = false;       // DAB: audio frame headers found; DAB+: super frame sync
    // frames that passed / failed their checks (DAB: header + ScF-CRC per
    // audio frame; DAB+: the CRC of each access unit)
    uint64_t frames_ok = 0;
    uint64_t frames_bad = 0;
    // DAB+ only: Reed-Solomon code words corrected (with >= 1 error) and
    // uncorrectable ones
    uint64_t rs_corrected = 0;
    uint64_t rs_failed = 0;
};

class SubchannelDecoder {
public:
    explicit SubchannelDecoder(AudioSink& sink);
    ~SubchannelDecoder();
    SubchannelDecoder(const SubchannelDecoder&) = delete;
    SubchannelDecoder& operator=(const SubchannelDecoder&) = delete;

    // The sub-channel to decode; a change resets everything (status too)
    void configure(const SubchannelConfig& cfg);
    // Drops all state (a retune, a lost signal) but keeps the configuration
    void reset();

    // One logical frame: the sub-channel's bytes of one CIF (24 ms), after
    // Viterbi decoding and energy dispersal removal, MSB first. n must be
    // bitrate_kbps * 3 (anything else is ignored). want_audio =
    // host.voice_wanted(): false skips ONLY the PCM decoding - framing,
    // checks, PAD and the dynamic label always run.
    // Returns how many audio frames (DAB) / access units (DAB+) passed
    // their checks in this call.
    int push_frame(const uint8_t* data, size_t n, bool want_audio);
    // A logical frame the receiver could not deliver (lost sync): keeps the
    // frame timeline, so one lost frame costs one frame / super frame
    void push_lost();

    const AudioStatus& status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Whether the AAC library for DAB+ audio can be loaded, as a status text
// for the codec check (e.g. "libfdk-aac.so.2 (2.0.2)" / "not installed
// (libfdk-aac2)"); *ok says which. Does not keep a decoder open.
std::string aac_library_status(bool* ok);

}   // namespace dab
