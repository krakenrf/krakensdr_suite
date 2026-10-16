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
// played through the voice FIFO here when the VFO is heard (🔊) in Digital
// demod mode.

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

// 🗣 Voice to Text (voice2text.cpp): the decoded voice of decoders whose
// voice tap is on (DigitalDecoder::set_voice_tap - the VFO's "Transcribe"),
// as the lead plugin sends it: 8 kHz audio + the plugin + who is talking
// (the most recent open talker: id "" = none reported, "?" = unnamed) and
// the VFO's RF. Called on the decoder's worker thread; unset = not tapped.
using VoiceTapHandler = std::function<void(DigitalDecoder* dec, const float* x8k, size_t n, const std::string& plugin,
                                           const std::string& talker_id, const std::string& talker_label,
                                           double rf_hz)>;
void set_voice_tap_handler(VoiceTapHandler h);
// 📼 Recorder (recorder_audio.cpp): the same decoded voice (8 kHz) of
// decoders whose record tap is on (DigitalDecoder::set_record_tap - a VFO in
// an audio recording). Decoder worker thread; unset = not tapped
using RecordVoiceHandler = std::function<void(DigitalDecoder* dec, const float* x8k, size_t n)>;
void set_record_voice_handler(RecordVoiceHandler h);

// Who transmits, as a plugin reports it (kp::Host::talker): the samples
// [start, end) of the VFO's decimated stream (DigitalDecoder::push pos - the
// same count the MUSIC frames carry, SharedDecimator::MultiChannelDecimated
// ::stream_pos) belong to radio `id`. Sent again as the transmission goes on
// (end grows); closed = it ended at `end`. rate = the VFO's sample rate.
// channel = kp::Talker::channel (DMR timeslot); id "?" = someone the plugin
// can't name (yet) - its samples belong to no talker.
struct TalkerSpan {
    std::string plugin, id, label;
    uint64_t start = 0, end = 0;
    bool closed = false;
    float rate = 0;
    int channel = 0;
    bool packet = false;   // one short packet (kp::Talker::packet): always closed
    // packets: the channel inside the VFO (Hz from its centre, NAN = whole VFO)
    // and how long to average the talker's packets (0 = default)
    double freq_hz = NAN, bw_hz = 0, avg_s = 0;
    // packets: where the talker was when it sent it (kp::Talker::lat / lon /
    // alt_m; NAN = unknown) - the array calibration from aircraft
    double lat = NAN, lon = NAN, alt_m = NAN;
    // packets: how long the talker is kept after its last packet (s, 0 = the
    // default; kp::Talker::keep_s)
    double keep_s = 0;
    // packets: the plugin's own covariance of the packet (kp::Talker::cov -
    // separation by code, the lora plugin): cov_m x cov_m, row-major; 0 = none
    int cov_m = 0;
    std::vector<std::complex<double>> cov;
    // not a span: the plugin asked to forget this talker now (kp::Host::talker_forget)
    bool forget = false;
    // a confirmed frame (kp::Host::valid(start, end)), not a talker: these
    // samples hold the decoded signal - the Digital squelch. id is "".
    bool signal = false;
};
using TalkerHandler = std::function<void(const TalkerSpan&)>;

// A transmitter's spatial covariance from an array plugin (kp::Host::spatial):
// measured from the VFO stream samples [start, end) of every antenna
struct SpatialReport {
    std::string plugin, id, label;
    uint64_t start = 0, end = 0;
    float rate = 0;
    double delay_us = NAN, power_db = NAN, snr_db = NAN, avg_s = 0;
    bool ref = false;
    int m = 0;
    std::vector<std::complex<double>> cov;   // m x m, row-major
    // kp::Host::spatial_remove: forget the talker id ("" = all of the plugin's)
    bool remove = false;
};
using SpatialHandler = std::function<void(const SpatialReport&)>;
// A plugin's retune request (kp::Host::tune): the RF to put at 0 Hz of the
// VFO. Returns "" when it was passed on, else why it was refused. Called on
// the decoder's worker thread.
using TuneHandler = std::function<std::string(double rf_hz)>;

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
    // The Digital squelch's live state: the active plugin (fixed, or what
    // Auto detect locked onto) reported a valid frame within the last ms
    bool frames_within(int64_t ms) const;
    // ... within its own hold (kp::Info::squelch_hold_s; def_ms when it sets none)
    bool squelch_open(int64_t def_ms) const;
    // The VFO's voice comes from this decoder (Voice to Text, the analog
    // feed): its plugin decodes voice (kp::Info::voice) - in Auto detect the
    // detected one, or, still searching, any it runs. A decoder of no voice
    // (ctcss, pocsag, adsb...) leaves the VFO's analog demod to Voice to Text
    bool takes_voice() const { return takes_voice_.load(std::memory_order_relaxed); }
    // the lead plugin's talker now (the open one that started first): Voice
    // to Text names an ANALOG transmission after it (ctcss: "CTCSS 88.5").
    // false = none
    bool lead_talker(std::string* id, std::string* label) const;
    void set_lead_talker(const std::string& id, const std::string& label);   // Engine
    void set_takes_voice(bool on) { takes_voice_.store(on, std::memory_order_relaxed); }   // Engine
    void set_options(const Options& o);
    Options options() const;

    // Decimation pipeline: queue a block (copied; never blocks). rf_hz = the
    // VFO's RF centre - a change of more than 100 Hz resets the decoders.
    // pos = the VFO stream position of x[0] (talker spans are reported in
    // it); NO_POS = count the samples pushed here instead (offline tests)
    static constexpr uint64_t NO_POS = ~0ull;
    // mc: every antenna of a coherent array for multichannel plugins
    // (kp::Info::multichannel; the pipeline fills it only while
    // wants_multichannel()): n samples x mc_m antennas, sample by sample,
    // antenna 0 first, value x MC_SCALE
    void push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos = NO_POS,
              const std::complex<int16_t>* mc = nullptr, int mc_m = 0);
    static constexpr float MC_SCALE = 8192.0f;   // = kp::wire::MC_SCALE
    // a multichannel plugin runs (the pipeline then hands every antenna over)
    bool wants_multichannel() const { return want_mc_.load(std::memory_order_relaxed); }
    // Synchronous processing (offline tests; threaded = false)
    void process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos = NO_POS,
                 const std::complex<int16_t>* mc = nullptr, int mc_m = 0);

    // Talker spans (kp::Host::talker) of this decoder's plugins go to h -
    // the VFO's per-talker DoA (talker_doa.hpp). Called on the worker thread.
    void set_talker_handler(TalkerHandler h);
    void deliver_talker(const TalkerSpan& s);

    // Array plugins (kp::Info::array): the decimation pipeline offers every
    // antenna's block (ch[k] = antenna k's n samples, pos = stream position
    // of sample 0) while wants_array(); only the blocks inside the plugin's
    // bursts are queued (copied). Spatial reports go to the spatial handler
    // (the VFO's TalkerDoa) on the worker thread.
    bool wants_array() const { return want_array_.load(std::memory_order_relaxed); }
    void push_array(const std::complex<float>* const* ch, int m, size_t n, float rate_hz, double rf_hz, uint64_t pos);
    void set_array_wanted(bool on, double burst_s, double period_s, bool only);   // Engine
    void set_spatial_handler(SpatialHandler h);
    void deliver_spatial(const SpatialReport& s);
    // Retune requests of plugins that declare kp::Info::tunes (DecimatorManager)
    void set_tune_handler(TuneHandler h);
    std::string deliver_tune(double rf_hz);
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
    // Voice to Text taps this decoder's voice: the vocoder runs as if someone
    // listened, and the audio goes to the voice tap handler too
    void set_voice_tap(bool on) { voice_tap_.store(on, std::memory_order_relaxed); }
    bool voice_tap() const { return voice_tap_.load(std::memory_order_relaxed); }
    // 📼 an audio recording of this VFO takes the voice too (the vocoder runs)
    void set_record_tap(bool on) { record_tap_.store(on, std::memory_order_relaxed); }
    bool record_tap() const { return record_tap_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> vfo_id_{-1};
    std::atomic<bool> takes_voice_{true};
    mutable std::mutex lead_mu_;
    std::string lead_tid_, lead_tlabel_;
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
    std::atomic<bool> voice_tap_{false};
    std::atomic<bool> record_tap_{false};

    std::mutex talker_mu_;
    TalkerHandler talker_handler_;
    SpatialHandler spatial_handler_;
    TuneHandler tune_handler_;
    std::atomic<bool> want_array_{false};
    std::atomic<bool> array_only_{false};   // nothing but array plugins: push() only ticks
    uint64_t tick_pos_ = NO_POS;            // the last tick (pipeline thread)
    std::atomic<double> arr_burst_{0.25}, arr_period_{1.0};

    // queue
    struct Block {
        std::vector<std::complex<float>> x;
        float rate;
        double rf;
        uint64_t pos;
        std::vector<std::complex<int16_t>> mc;   // every antenna (push's mc), empty = none
        int m = 0;
        // am > 0: an array block (push_array), x = am antennas x ns samples, antenna-major
        int am = 0;
        size_t ns = 0;
    };
    std::atomic<bool> want_mc_{false};
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
