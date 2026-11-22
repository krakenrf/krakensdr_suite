#pragma once

// The "all channels in the VFO" mode: every Aero carrier of the input found,
// decoded and listened to at once.
//
// - aero_bank.*: one FFT filter bank cuts a 24 kHz channel out of the input
//   (the VFO's own rate, up to 2.4 MS/s) for each carrier.
// - Each channel is an AeroChannel (the single-channel decoder, unchanged:
//   the same counts by construction) behind its own ChanHost, a kp::Host
//   that names the channel in events, keeps its facts for the channel list,
//   merges the aircraft table and gives its audio to the mixer.
// - The survey: the bank's bin powers summed over 0.25 s against a local
//   floor; a narrow peak (600 / 1200 / 2400 bit/s carriers) or a wide bump
//   (4.8 / 8.4 / 10.5 kbit/s) not yet followed opens a trial channel in Auto;
//   a trial that passes no CRC within its time is closed and its frequency
//   left alone for a while - the other systems' carriers between the Aero
//   channels never become channels. A C channel assignment naming a
//   frequency in the input opens that C channel at once.
// - The window: one fact per channel (nominal frequency once the receiver's
//   offset is known from the advertised P channels; kind, rate, GES, signal
//   units, level, last activity), the satellite table once, events tagged
//   with their channel, the aircraft table merged across channels.

#include "aero_bank.hpp"
#include "aero_channel.hpp"
#include "dig_vocoder.hpp"
#include "kraken_plugin.hpp"

#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aero {

class AllChannels {
public:
    AllChannels(kp::Host& host, double rate);
    ~AllChannels();
    void process(const cf* x, size_t n);
    void reset();
    void option(const std::string& key, const std::string& value);

private:
    class ChanHost;
    struct Chan {
        int id = 0;
        int bank_id = 0;
        double centre_hz = 0;          // in the input
        std::unique_ptr<ChanHost> h;
        std::unique_ptr<AeroChannel> dec;
        std::vector<cf> pend;          // 24 kHz samples waiting for the decoder
        double t0 = 0;                 // host time of the channel's first sample
        int64_t nsamp = 0;
        double opened = 0, last_valid = -1, first_valid = -1;
        bool validated = false, assigned = false, ever_locked = false, dup = false, open_wide = false;
        std::map<std::string, std::string> facts;
        std::string name, shown_key, voice;
        std::deque<float> audio;
        double audio_start = -1;       // host time this call's audio began
        float level_db = 0;
        float open_db = 0;             // the survey's level when it was opened
    };
    kp::Host& host;
    double rate_;
    FilterBank bank_;
    std::map<int, std::unique_ptr<Chan>> ch_;
    int next_id_ = 1;
    double t_base_ = 0;
    bool t_set_ = false;
    int64_t nin_ = 0;
    double next_survey_ = 0.25, next_show_ = 1.0;
    std::map<double, double> blacklist_;     // centre (Hz) -> until (host time)
    std::vector<float> spec_, ratio_;        // the last survey's spectrum (dB over the floor), frequency order
    double offset_hz_ = NAN;                 // the receiver's offset (measured - advertised)
    int offset_n_ = 0;
    int quiet_surveys_ = 0;
    uint64_t closed_ok_ = 0, closed_tot_ = 0;   // the signal units of channels closed since
    std::string listen_ = "mix";
    bool verbose_opt_ = false;
    dig::VoiceLimiter lim_;
    int64_t aud_out_ = 0;                    // mixed audio samples given out (8 kHz, on the input's clock)
    std::map<std::string, std::map<int, std::vector<std::string>>> rows_;   // aircraft table: key -> channel -> cells
    std::set<std::string> global_facts_;
    const bool debug_ = std::getenv("AERO_DEBUG") != nullptr;
    const bool survey_dbg_ = debug_ && std::getenv("AERO_SURVEY") != nullptr;
    double now() const;
    void survey();
    Chan* open(double centre_hz, const std::string& mode, bool assigned, double search_hz = 2900);
    void close(int id, bool blacklist);
    void feed(Chan& c, const cf* y, size_t n);
    void housekeeping();
    void show();
    void mix();
    void voice_state();
    std::string name_of(const Chan& c) const;
    std::string summary(const Chan& c, uint64_t* ok, uint64_t* tot) const;
    double carrier_abs(const Chan& c) const;
    void on_assign(double to_aircraft_hz);
    void table_row(int ch, const std::string& key, const std::vector<std::string>& cells);
    friend class ChanHost;
};

}  // namespace aero
