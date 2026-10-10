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
// passed its checks (drives the "receiving" state), optional 8 kHz audio, and
// map points (positions) for the web UI's 🗺 Map.
// See plugins/SDK.md for the full guide and kraken_dsp.hpp for DSP helpers.

#include <cmath>
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

// A position for the web UI's map (Host::map_point). Sending the same id again
// moves / updates the marker; it disappears after ttl_s without an update, or
// on Host::map_remove(id).
struct MapPoint {
    std::string id;                 // stable key within this decoder (ICAO address, callsign...), <= 32 chars
    double lat = NAN, lon = NAN;    // degrees, WGS84
    std::string label;              // short text next to the marker ("" = the id)
    std::string kind = "point";     // marker: "aircraft", "vehicle", "ship", "person", "station", "balloon" or "point"
    float heading = NAN;            // degrees true (rotates the marker), NAN = unknown
    float altitude_m = NAN;         // NAN = unknown
    float speed_kmh = NAN;          // NAN = unknown
    std::string info;               // details shown when the marker is clicked, lines separated by '\n'
    double ttl_s = 300;             // seconds without an update before it is removed (10 s .. 24 h)
};

// Who is transmitting right now (Host::talker) - e.g. the P25 unit ID from
// the link control. kraken_doa cuts the VFO's signal at these boundaries and
// runs the DoA on each talker's own samples, so every radio gets its own
// bearing (and its own heat map while driving) on the 🗺 Map.
struct Talker {
    std::string id;                 // unit / radio ID, stable per radio, <= 32 chars
    std::string label;              // shown next to it ("TG 1201", talker alias...), <= 64 chars
    // Host::time() where the transmission began (e.g. the call header) and
    // up to where it is CONFIRMED (the end of the last frame that passed its
    // checks). NAN = now. Only the samples in between are used, so report the
    // frame boundaries - not the moment the frame was decoded, which is later.
    double start_s = NAN;
    double end_s = NAN;
    // For protocols that carry several transmissions on one frequency at once
    // (DMR's two timeslots): each channel 0..7 has a talker of its own. Frames
    // that hold two talkers' signals are never used for either's DoA.
    int channel = 0;
    // One short PACKET (an ADS-B / Mode S message) instead of a transmission
    // in progress: kraken_doa computes the DoA from exactly the samples
    // start_s .. end_s - so give them to the sample (Host::time() of the
    // packet's first sample + its length) - and averages each talker's
    // packets over the last seconds. One call per packet that passed its
    // checks, no talker_end; channel is ignored. For moving talkers whose
    // packets are far shorter than a MUSIC frame (many aircraft at once).
    bool packet = false;
    // Packets only, optional: where the packet is in the VFO's band - its
    // centre in Hz from the frequency the plugin receives at 0 Hz, and its
    // width. kraken_doa then filters exactly that channel out of every
    // antenna before the DoA, so a packet on one channel isn't mixed with
    // another talker on a neighbouring channel of the same VFO (AIS:
    // 161.975 / 162.025 MHz in one 100 kHz VFO). NAN / 0 = the whole VFO.
    double freq_hz = NAN;
    double bw_hz = 0;
    // Packets only: average each talker's packets over about this many
    // seconds (0 = the host's 1.5 s, for fast movers sending often - ADS-B).
    // Slow movers that send every few seconds want more (AIS: 20 s).
    double avg_s = 0;
    // Packets only, optional: where the talker WAS when it sent this packet
    // (WGS84 degrees, altitude in metres above mean sea level) - its last
    // reported position moved on to the packet's time, NAN = unknown. With
    // it kraken_doa can calibrate the antenna array against the known
    // bearings (✈ array calibration from ADS-B aircraft). Only give a
    // position that is accurate to some tens of metres at that moment.
    double lat = NAN, lon = NAN, alt_m = NAN;
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
    bool map = false;               // sends map points (Host::map_point): the panel offers "Plot on map"
    bool manual_only = false;       // never runs in Auto detect (no Auto detect tick in the plugin list)
    // > 0: picking the plugin for a VFO tunes the VFO (offset 0) and its tuner
    // to this frequency with the narrowest bandwidth >= min_vfo_rate; the VFO
    // is then drawn as a single line and can't be dragged (e.g. ADS-B 1090 MHz)
    double fixed_freq_hz = 0;
    bool voice = false;             // decodes voice (Host::audio): the panel offers "Listen"
    bool messages = false;          // sends free-text messages (Host::message): addresses in them become incidents
    bool talkers = false;           // reports who transmits (Host::talker): the DoA can be split per radio
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
    // The same, also saying WHERE the frame was: Host::time() of its first
    // sample and of its end (like Talker::start_s / end_s). Prefer it: the
    // VFO's "Digital" squelch opens only on confirmed frames and takes the
    // DoA from exactly these samples (nothing before, between or after the
    // frames - noise, other signals). Counts as one valid frame. The default
    // (an older host) just counts it.
    virtual void valid(double start_s, double end_s) { (void)start_s; (void)end_s; valid(); }
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
    // A position for the web UI's 🗺 Map (see MapPoint). Cheap to call often:
    // updates are sent to the browsers about once a second.
    virtual void map_point(const MapPoint& p) { (void)p; }
    // Takes the point off the map (e.g. the aircraft is out of range)
    virtual void map_remove(const std::string& id) { (void)id; }
    // A free-text message the decoder received (a pager text...). kraken_doa
    // looks for a street address in it (OpenStreetMap streets around the
    // station) and puts the incident on the 🗺 Map. from = who sent / was
    // paged ("RIC 1234567"). Set Info::messages.
    virtual void message(const std::string& from, const std::string& text) { (void)from; (void)text; }
    // A raw frame for the decoder data log (sidebar → Decoder Logging, "Raw
    // frames"): one line of text - e.g. a Mode S message in hex, an APRS
    // packet in TNC2 format. Only while raw_wanted() (it costs CPU / disk).
    virtual bool raw_wanted() const { return false; }
    virtual void raw(const std::string& data) { (void)data; }
    // A live table in the panel (e.g. the aircraft ADS-B hears): the column
    // headings once, then rows by key (the same key again = update). A row
    // not updated for 10 minutes is dropped; table_remove() drops it now.
    virtual void table_columns(const std::vector<std::string>& cols) { (void)cols; }
    virtual void table_row(const std::string& key, const std::vector<std::string>& cells) { (void)key; (void)cells; }
    virtual void table_remove(const std::string& key) { (void)key; }
    // The talker of the transmission in progress (see Talker). Call it again
    // with the same id as frames keep passing (end_s moves on); a different id
    // on the same channel ends the previous talker where the new one starts.
    // Cheap - once per frame is fine. Set Info::talkers.
    virtual void talker(const Talker& t) { (void)t; }
    // The channel's transmission ended at Host::time() at_s (NAN = now): e.g.
    // on the terminator frame, or when frames stopped coming without one
    virtual void talker_end(double at_s = NAN, int channel = 0) { (void)at_s; (void)channel; }
    // The receiver's location (sidebar → Station Information), if it has one:
    // a reference for local position decoding, distances, range checks
    virtual bool station(double* lat, double* lon) const { (void)lat; (void)lon; return false; }
    // The radio frequency (Hz) at 0 Hz of process()'s input: the VFO's centre
    // plus the host's frequency correction. NAN = unknown (an older host, or
    // an offline test of a file without --rf / an "rf_hz" sidecar). It can
    // change between process() calls; a retune also calls reset(). Lets a
    // decoder name the channel it receives (a DME channel from its reply
    // frequency) or find the channels inside a wide VFO.
    virtual double rf_hz() const { return NAN; }
    // true: the input is mirrored (the decoder's "invert" option) - a signal
    // at +f Hz in process()'s input is on rf_hz() - f
    virtual bool rf_inverted() const { return false; }
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
constexpr uint32_t STATION = 6;        // "lat,lon" (degrees) of the receiver, "" = unknown
constexpr uint32_t RF = 7;             // "rf_hz[,1]": RF at the input's 0 Hz (",1" = mirrored), "" = unknown
// plugin -> kraken_doa
constexpr uint32_t FACT = 16;          // key '\0' value ("" value = remove)
constexpr uint32_t EVENT = 17;         // float32 dedup_s, text
constexpr uint32_t VALID = 18;         // empty, or start_s '\0' end_s (decimal text, Host::valid(start, end))
constexpr uint32_t AUDIO = 19;         // float32 samples, 8 kHz
constexpr uint32_t VOICE_STATE = 20;   // text
constexpr uint32_t FREQ_ERROR = 21;    // float32 Hz
constexpr uint32_t SYNC_DONE = 22;     // uint32 id of the SYNC
// fields separated by '\0': id, lat, lon, label, kind, heading, altitude_m,
// speed_kmh, ttl_s, info (numbers as decimal text, "" = unknown)
constexpr uint32_t MAP_POINT = 23;
constexpr uint32_t MAP_REMOVE = 24;    // id
constexpr uint32_t TABLE_COLUMNS = 25; // column headings, '\0'-separated
constexpr uint32_t TABLE_ROW = 26;     // key '\0' cell '\0' cell ...
constexpr uint32_t TABLE_REMOVE = 27;  // key
constexpr uint32_t MESSAGE = 28;       // from '\0' text
constexpr uint32_t RAW = 29;           // one raw frame (text) for the decoder data log
// id '\0' label '\0' start_s '\0' end_s ['\0' channel ['\0' flags ['\0'
// freq_hz '\0' bw_hz '\0' avg_s ['\0' lat '\0' lon '\0' alt_m]]]]: Host::time()
// seconds as decimal text (7 decimals: a sample at 2.4 MHz is 0.4 us); no
// channel field = channel 0; flags "p" = Talker::packet; freq_hz "" = NAN;
// the position fields only on packets that have one
constexpr uint32_t TALKER = 30;
constexpr uint32_t TALKER_END = 31;    // at_s ['\0' channel] (decimal text)
// (raw_wanted() is the OPTION "log_raw=0|1" kraken_doa sends)
}  // namespace kp::wire
