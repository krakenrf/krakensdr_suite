#pragma once

// Digital voice/data decoder attached to ONE decimator (VFO).
//
// Every decoder is a plugin (plugins/<id>/, dig_plugin.hpp) - the protocols
// that ship with the suite (P25, DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG,
// APRS) as well as ones written by hand or by the AI Signal Lab. The
// decimation pipeline pushes this VFO's decimated samples (the listened-to
// channel, or the beamformer output when beamforming runs) with push(); a
// worker thread mixes them (AFC), resamples them to each plugin's rate and
// streams them to the plugin processes. Mode AUTO runs every plugin that
// declares auto_detect and reports the one whose frames pass their checks.
//
// The plugins report facts and events (Report) and decoded voice, which is
// played through the voice FIFO here when the VFO is the audio source in
// Digital demod mode.

#include <atomic>
#include <complex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "digital/dig_report.hpp"

namespace dig {

class Engine;   // plugin processes + front end (digital_decoder.cpp)
class DigitalDecoder;

// Free-text messages the plugins send (kp::Host::message) go to this handler
// - kraken_doa's incident map (incidents.cpp). Called on the decoder's worker
// thread; text "" = a plugin that sends messages has started (time to get
// the street data ready). Unset (offline tests): messages are dropped.
// rf_hz = the VFO's frequency when the message arrived
using MessageHandler = std::function<void(DigitalDecoder* dec, const std::string& plugin, const std::string& from,
                                          const std::string& text, double rf_hz)>;
void set_message_handler(MessageHandler h);

// The decoder data log (kraken_doa decoder_log.cpp): what the plugins report,
// as it arrives. type: "event" (a log line, after de-duplication), "message"
// (kp::Host::message: text + from), "position" (a map point: point set),
// "raw" (kp::Host::raw: text). rf_hz = the VFO's frequency. Called on the
// decoder's worker thread; unset = nothing is recorded.
struct LogRecord {
    const char* type = "";
    std::string plugin, text, from;
    const MapPoint* point = nullptr;
    double rf_hz = 0;
};
using RecordHandler = std::function<void(DigitalDecoder* dec, const LogRecord& r)>;
void set_record_handler(RecordHandler h);
// Raw frames wanted (the log's "Raw frames" type is on): passed to the
// plugins as the OPTION log_raw=0|1 (kp::Host::raw_wanted)
void set_raw_wanted(bool on);

class DigitalDecoder {
public:
    // threaded = false: offline tests - process() runs synchronously and
    // waits for slow plugins instead of dropping samples
    explicit DigitalDecoder(bool threaded = true);
    ~DigitalDecoder();
    DigitalDecoder(const DigitalDecoder&) = delete;
    DigitalDecoder& operator=(const DigitalDecoder&) = delete;

    // plugin = the plugin id for Mode::PLUGIN (plugins/<id>/)
    // the VFO (DecimatorInstance id) this decoder belongs to, -1 = none;
    // set once by DecimatorManager when it creates the decoder (ids never
    // change) - lets the record handler name the VFO without the manager
    void set_vfo(int id) { vfo_id_.store(id, std::memory_order_relaxed); }
    int vfo() const { return vfo_id_.load(std::memory_order_relaxed); }
    void set_mode(Mode m, const std::string& plugin = "");
    Mode mode() const { return mode_.load(std::memory_order_relaxed); }
    std::string plugin_id() const;
    // the plugin whose data this decoder shows: the fixed one, or in AUTO the
    // detected one ("" = none yet / off)
    std::string active_plugin() const;
    void set_options(const Options& o);
    Options options() const;

    // Decimation pipeline: queue a block (copied; never blocks). rf_hz = the
    // VFO's RF centre - a change of more than 100 Hz resets the decoders.
    void push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz);
    // Synchronous processing (offline tests; threaded = false)
    void process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz);
    // Offline tests: wait until the plugins have processed all input
    void drain(int timeout_ms = 30000);

    // {"mode":..,"detected":..,"state":..,"info":{..},"events":[..],...}
    // events: those with seq > events_after (at most max_events)
    // tables: include the plugins' tables (kp::Host::table_*) - large, so
    // the 4 Hz push sends them at most once a second
    std::string status_json(uint64_t events_after, size_t max_events, bool tables = false) const;
    uint64_t last_event_seq() const { return report_.last_seq(); }
    Report& report() { return report_; }

    // --- Decoded voice (Digital demod mode) ---
    // Audio thread: fill out[0..n) with 48 kHz voice (zeros where there is
    // none). Also marks the voice as wanted for the next ~0.5 s, which is
    // what makes the plugins run their vocoders.
    void pull_voice(float* out, size_t n);
    // Decoder side (Engine): 8 kHz voice in
    void push_voice(const float* x8k, size_t n);
    void set_voice_state(const std::string& s);
    bool voice_wanted() const;

private:
    std::atomic<int> vfo_id_{-1};
    std::atomic<Mode> mode_{Mode::OFF};
    std::atomic<bool> reset_pending_{false};   // set_mode -> worker reconfigures + resets
    mutable std::mutex opt_mu_;
    Options opts_;
    std::string plugin_id_;            // Mode::PLUGIN (opt_mu_)
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
