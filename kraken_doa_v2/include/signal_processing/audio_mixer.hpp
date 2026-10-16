#pragma once

// 🔊 The live audio: any number of VFOs at once (the speaker toggle on each
// VFO card, DecimatorInstance::audio_out). Each VFO heard has its own channel
// - its own FMDemodulatorRobust: demodulator, filters, levels and the 48 kHz
// ring that absorbs the jitter between the packet clock and the audio timer
// (one ring per VFO, so each starts and fills exactly like the single audio
// source did). The audio timer (websocket_server.cpp, every
// AUDIO_PACKET_TIME_MS) sums every channel's ring into ONE Opus stream - the
// page plays it as before. The FM thread (data_receiver.cpp) feeds the
// channels; the control handler drops the channels of VFOs no longer heard.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <opus/opus.h>
#include "signal_processing/fm_demodulator.hpp"

class AudioMixer {
public:
    struct Channel {
        explicit Channel(int v) : vfo(v) {}
        const int vfo;
        FMDemodulatorRobust demod;
        std::atomic<int64_t> fed_ms{0};   // steady ms of the last block fed
        // the VFO's demod (DIGITAL included - demod runs it as NBFM), for the stats
        std::atomic<DemodulatorMode> mode{DemodulatorMode::WBFM};
    };

    AudioMixer();
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    // The channel of a VFO, made on first use (FM thread). nullptr while
    // the audio is off
    std::shared_ptr<Channel> channel(int vfo);
    // Keep only the channels of these VFOs (control thread, after the set
    // changed): a VFO muted is silent at once
    void retain(const std::vector<int>& vfos);
    // Every channel's buffer + demodulator state (retune, FFT change, gap)
    void reset();

    // FM:1 / FM:0 (off drops every channel)
    void enable_processing();
    void disable_processing();
    bool is_processing_enabled() const { return enabled_.load(); }

    // The next n samples of the mix, Opus encoded (audio timer thread).
    // Silence when no VFO is heard. Channels not fed for IDLE_MS are dropped
    // (the VFO's stream stopped; it comes back by itself)
    std::vector<unsigned char> get_opus_packet(size_t n);

    struct Stats {
        int channels = 0;
        size_t over = 0, under = 0;   // summed over the channels
        float fill = 0.0f;            // the fullest ring, %
        float signal = 0.0f;          // the strongest channel's
        std::string modes;            // "D0 NBFM, D2 AM"
    };
    Stats stats() const;

    static constexpr int64_t IDLE_MS = 3000;

private:
    mutable std::mutex mu_;                     // ch_
    std::vector<std::shared_ptr<Channel>> ch_;
    std::atomic<bool> enabled_{true};

    // audio timer thread only
    OpusEncoder* enc_ = nullptr;
    bool enc_failed_ = false;
    std::vector<float> mix_;
    std::vector<opus_int16> pcm_;
    std::vector<unsigned char> out_;
};

extern AudioMixer audio_mixer;
