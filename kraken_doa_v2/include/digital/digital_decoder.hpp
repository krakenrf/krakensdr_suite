#pragma once

// Digital voice/data decoder attached to ONE decimator (VFO).
//
// Every decoder is a plugin (plugins/<id>/, dig_plugin.hpp) - the protocols
// that ship with the suite (P25, DMR, TETRA, D-STAR, NXDN, YSF, MPT1327, POCSAG /
// FLEX, APRS) as well as ones written by hand or by the AI assistant. The
// decimation pipeline pushes this VFO's decimated samples (the listened-to
// channel, or the beamformer output when beamforming runs) with push(); a
// worker thread mixes them (AFC), resamples them to each plugin's rate and
// streams them to the plugin processes. Mode AUTO runs every plugin ticked
// for Auto detect and reports the one whose frames pass their checks; one
// that needs more bandwidth than the VFO has runs on an internal stream of
// the VFO's centre at the width it needs (StreamWant / AuxStream below).
//
// The plugins report facts and events (Report) and decoded voice, which is
// played through the voice FIFO here when the VFO is heard (🔊) in Digital
// demod mode.

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
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
// "Allow logging to disk" (decoder_log.cpp). A decoder whose VFO is set to
// Log All (Options::log) then gets raw frames - the OPTION log_raw=1
// (kp::Host::raw_wanted) - and verbose=1 (kp::Host::verbose, also on with
// the VFO's "Show all messages"); otherwise log_raw=0 and verbose = that tick
void set_log_allowed(bool on);
bool log_allowed();
// A browser shows decoder video (a page subscribed to VIDEO): passed to the
// video plugins as the OPTION video_wanted=0|1 (kp::Host::video_wanted), and
// while false their frames are dropped on arrival. Set from the uWS loop.
void set_video_wanted(bool on);

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

// A reconstructed picture a plugin sent (kp::Host::video_frame): a
// width x height raster drawn on the decoder panel's canvas (analog TV,
// screen-leakage reconstruction). format 0 = GRAY8 (1 byte/pixel), 1 = RGB24
// (3 bytes/pixel). The host forwards the image data opaquely: encoding 0 =
// width*height*bpp raw pixel bytes (row-major, top-left first), encoding 2 = a
// JPEG bitstream the browser decodes natively (the main app never decodes it).
// Delivered on the decoder's worker thread; unset = dropped.
struct VideoFrameReport {
    int width = 0, height = 0;
    int format = 0;              // 0 GRAY8, 1 RGB24 (describes the source frame)
    int encoding = 0;            // 0 raw pixels, 2 JPEG bitstream
    std::string label;           // caption ("" = none)
    std::vector<uint8_t> pixels; // the image data (raw pixels, or the JPEG blob)
};
using VideoHandler = std::function<void(const VideoFrameReport&)>;

// A plugin's retune request (kp::Host::tune): the RF to put at 0 Hz of the
// VFO. Returns "" when it was passed on, else why it was refused. Called on
// the decoder's worker thread.
using TuneHandler = std::function<std::string(double rf_hz)>;

// Auto detect's internal streams (docs/dev/digital-decoders.md, "Internal
// streams"): a plugin that needs a wider bandwidth than the VFO has runs on
// the VFO's centre decimated to a width of its own - what it would get if
// the user widened the VFO to that width. The engine says which widths it
// wants (DigitalDecoder::wanted_streams); the pipeline decimates them in the
// VFO's pass and hands them over with the VFO's block (push).
struct StreamWant {
    int width_hz = 0;   // an efficient size (bandwidth = output rate)
    bool mc = false;    // a multichannel plugin runs on it: every antenna, when the VFO's own stream would get them
};
struct AuxStream {
    int width_hz = 0;                              // = a StreamWant::width_hz
    float rate_hz = 0;                             // its output rate (= width_hz)
    const std::complex<float>* x = nullptr;        // the listened-to channel (never the beamformer output)
    size_t n = 0;
    double pos = 0;                                // VFO stream position of x[0] (the cascades' delays taken off)
    const std::complex<int16_t>* mc = nullptr;     // every antenna x MC_SCALE (StreamWant::mc), null = none
    int mc_m = 0;
};

class DigitalDecoder {
public:
    // threaded = false: offline tests - process() runs synchronously and
    // waits for slow plugins instead of dropping samples.
    // internal_streams = the caller of push() / process() makes the internal
    // streams wanted_streams() names (the live pipeline). Without them (the
    // offline tests) Auto detect runs as before they existed: on the VFO's
    // stream only, without the plugins that need more than
    // AUTO_MAX_RATE_SHORTFALL x its rate
    explicit DigitalDecoder(bool threaded = true, bool internal_streams = false);
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
    // (analog_id, pagers, adsb...) leaves the VFO's analog demod to Voice to Text
    bool takes_voice() const { return takes_voice_.load(std::memory_order_relaxed); }
    // the lead plugin's talker now (the open one that started first): Voice
    // to Text names an ANALOG transmission after it (analog_id: "CTCSS 88.5").
    // false = none
    bool lead_talker(std::string* id, std::string* label) const;
    void set_lead_talker(const std::string& id, const std::string& label);   // Engine
    void set_takes_voice(bool on) { takes_voice_.store(on, std::memory_order_relaxed); }   // Engine
    void set_options(const Options& o);
    Options options() const;
    // Options::log (LOG_*) without the lock and copy - the decoder log's
    // record handler asks it per record
    int log_mode() const { return log_mode_.load(std::memory_order_relaxed); }
    // Options::pos_s the same way (the record handler's "one position per object every N s")
    int pos_interval_s() const { return pos_s_.load(std::memory_order_relaxed); }

    // Decimation pipeline: queue a block (copied; never blocks). rf_hz = the
    // VFO's RF centre - a change of more than 100 Hz resets the decoders.
    // pos = the VFO stream position of x[0] (talker spans are reported in
    // it); NO_POS = count the samples pushed here instead (offline tests)
    static constexpr uint64_t NO_POS = ~0ull;
    // mc: every antenna of a coherent array for multichannel plugins
    // (kp::Info::multichannel; the pipeline fills it only while
    // wants_multichannel()): n samples x mc_m antennas, sample by sample,
    // antenna 0 first, value x MC_SCALE
    // bw_hz = the VFO's bandwidth (0 = take the rate for it); aux = the
    // internal streams of this block (Auto detect, wanted_streams()), copied
    // with it - one queue entry, so the 1 s cap bounds them too. n = 0 with
    // aux: the VFO has no samples this block (the beamformer held them back),
    // its internal streams still go - they must stay continuous
    void push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos = NO_POS,
              const std::complex<int16_t>* mc = nullptr, int mc_m = 0, int bw_hz = 0, const AuxStream* aux = nullptr,
              size_t naux = 0);
    static constexpr float MC_SCALE = 8192.0f;   // = kp::wire::MC_SCALE
    // a multichannel plugin runs on the VFO's own stream (the pipeline then
    // hands every antenna over; one on an internal stream: StreamWant::mc)
    bool wants_multichannel() const { return want_mc_.load(std::memory_order_relaxed); }
    // Synchronous processing (offline tests; threaded = false)
    void process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos = NO_POS,
                 const std::complex<int16_t>* mc = nullptr, int mc_m = 0, int bw_hz = 0,
                 const AuxStream* aux = nullptr, size_t naux = 0);

    // --- Auto detect's internal streams (StreamWant) ---
    // at most this many widths at once (a VFO in Auto detect)
    static constexpr int MAX_STREAMS = 4;
    bool internal_streams() const { return internal_streams_; }
    // the widths the engine wants now (none while the decoder is off, in a
    // fixed mode, or when the VFO is wide enough for every plugin); the
    // count fits MAX_STREAMS. streams_gen() changes with every change, so
    // the pipeline reads them only then; wants_streams() is the cheap test
    int wanted_streams(StreamWant* out) const;
    uint32_t streams_gen() const { return streams_gen_.load(std::memory_order_acquire); }
    bool wants_streams() const { return streams_n_.load(std::memory_order_relaxed) > 0; }
    void set_wanted_streams(const std::vector<StreamWant>& w);   // Engine

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
    // Reconstructed pictures (kp::Info::video plugins, kp::Host::video_frame)
    // go to h - the websocket server publishes them to the decoder panel.
    // Called on the decoder's worker thread.
    void set_video_handler(VideoHandler h);
    void deliver_video(const VideoFrameReport& f);
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
    std::atomic<int> log_mode_{LOG_OFF};   // = opts_.log
    std::atomic<int> pos_s_{POS_S_DEFAULT};  // = opts_.pos_s
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
    VideoHandler video_handler_;
    TuneHandler tune_handler_;
    std::atomic<bool> want_array_{false};
    std::atomic<bool> array_only_{false};   // nothing but array plugins: push() only ticks
    uint64_t tick_pos_ = NO_POS;            // the last tick (pipeline thread)
    std::atomic<double> arr_burst_{0.25}, arr_period_{1.0};

    // internal streams (set once in the constructor; the wants: Engine -> pipeline)
    const bool internal_streams_;
    mutable std::mutex streams_mu_;
    StreamWant streams_[MAX_STREAMS];
    std::atomic<int> streams_n_{0};
    std::atomic<uint32_t> streams_gen_{0};

    // queue
    struct AuxBlock {
        int w = 0;
        float rate = 0;
        double pos = 0;
        std::vector<std::complex<float>> x;
        std::vector<std::complex<int16_t>> mc;
        int m = 0;
    };
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
        int bw = 0;                              // the VFO's bandwidth (push's bw_hz)
        std::vector<AuxBlock> aux;               // internal streams (push's aux)
        double dur = 0;                          // seconds of signal (the queue's cap)
    };
    std::atomic<bool> want_mc_{false};
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Block> queue_;
    double queued_s_ = 0;   // seconds queued (QUEUE_MAX_SECONDS)
    void enqueue(Block&& b);
    std::atomic<uint64_t> dropped_{0};
    bool stop_ = false;
    std::thread worker_;
    void run();
};

}  // namespace dig
