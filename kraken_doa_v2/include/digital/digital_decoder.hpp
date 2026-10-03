#pragma once

// Digital voice/data decoder attached to ONE decimator (VFO).
//
// The decimation pipeline pushes this VFO's decimated samples (the listened-
// to channel, or the beamformer output when beamforming runs) with push();
// a worker thread resamples them to the demodulators' rates and runs the
// protocol receivers (see dig_protocols.hpp). Mode AUTO runs every receiver
// and reports the protocol whose frames pass FEC/CRC checks.
//
// Voice is not synthesized (DMR/D-STAR AMBE, P25 IMBE and TETRA ACELP are
// not decoded); the decoders report the channel's signalling: identities,
// talkgroups/callsigns, call activity, encryption, network/cell parameters.

#include <atomic>
#include <complex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "digital/dig_report.hpp"

namespace dig {

class Engine;   // demodulators + receivers (digital_decoder.cpp)

class DigitalDecoder {
public:
    explicit DigitalDecoder(bool threaded = true);
    ~DigitalDecoder();
    DigitalDecoder(const DigitalDecoder&) = delete;
    DigitalDecoder& operator=(const DigitalDecoder&) = delete;

    void set_mode(Mode m);
    Mode mode() const { return mode_.load(std::memory_order_relaxed); }
    void set_options(const Options& o);
    Options options() const;

    // Decimation pipeline: queue a block (copied; never blocks). rf_hz = the
    // VFO's RF centre - a change of more than 100 Hz resets the decoders.
    void push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz);
    // Synchronous processing (offline tests; threaded = false)
    void process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz);
    // Test hook: FM-discriminator samples (Hz) at 48 kHz straight into the
    // FSK receivers (P25 / DMR / D-STAR)
    void process_discriminator(const float* hz, size_t n);

    // {"mode":..,"detected":..,"state":..,"info":[..],"events":[..],...}
    // events: those with seq > events_after (at most max_events)
    std::string status_json(uint64_t events_after, size_t max_events) const;
    uint64_t last_event_seq() const { return report_.last_seq(); }
    Report& report() { return report_; }

    // Recommended VFO rates (decimated sample rate, Hz)
    static float min_rate_for(Mode m);

    // --- Decoded voice (Digital demod mode) ---
    // Audio thread: fill out[0..n) with 48 kHz voice (zeros where there is
    // none). Also marks the voice as wanted for the next ~0.5 s, which is
    // what makes the receivers run their vocoders.
    void pull_voice(float* out, size_t n);
    // Decoder side (Engine): 8 kHz voice in
    void push_voice(const float* x8k, size_t n);
    void set_voice_state(const std::string& s);
    bool voice_wanted() const;

private:
    std::atomic<Mode> mode_{Mode::OFF};
    std::atomic<bool> reset_pending_{false};   // set_mode -> worker resets the engine
    mutable std::mutex opt_mu_;
    Options opts_;
    Report report_;
    std::unique_ptr<Engine> engine_;   // only touched by the worker (or process())

    // voice FIFO (48 kHz) between the decoder and the audio threads
    mutable std::mutex v_mu_;
    std::deque<float> vfifo_;
    void* interp_ = nullptr;           // liquid firinterp_rrrf, 8 -> 48 kHz
    float agc_gain_ = 4.0f;
    bool v_playing_ = false;
    int64_t v_last_push_ms_ = 0;
    std::string voice_state_;
    std::atomic<int64_t> voice_wanted_until_{0};

    // queue
    struct Block { std::vector<std::complex<float>> x; float rate; double rf; };
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Block> queue_;
    size_t queued_samples_ = 0;
    std::atomic<uint64_t> dropped_{0};
    bool stop_ = false;
    std::thread worker_;
    void run();
};

}  // namespace dig
