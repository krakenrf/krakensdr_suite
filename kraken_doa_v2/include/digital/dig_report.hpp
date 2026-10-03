#pragma once

// Decoded-information store of one digital decoder: a small table of
// "channel facts" per protocol (colour code, NAC, MCC/MNC, current call...)
// plus an event log. Written by the decoder thread, read by the uWS loop when
// it builds the status JSON - everything is behind one mutex (a few hundred
// updates per second at most).

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dig {

enum class Mode : int { OFF = 0, AUTO = 1, P25 = 2, DMR = 3, TETRA = 4, DSTAR = 5, NXDN = 6, MPT1327 = 7 };
constexpr int NUM_PROTOCOLS = 6;   // P25, DMR, TETRA, DSTAR, NXDN, MPT1327
const char* mode_name(Mode m);     // "OFF", "AUTO", "P25", "DMR", "TETRA", "DSTAR", "NXDN", "MPT1327"
Mode mode_from_string(const std::string& s);   // unknown -> OFF
inline bool is_protocol(Mode m) { return m >= Mode::P25 && m <= Mode::MPT1327; }

// Options shared by the protocol decoders of one VFO (sidebar settings)
struct Options {
    bool verbose = false;      // also log repeating broadcasts / idle / CSBK chatter
    int dmr_slot = 0;          // 0 = both, 1 / 2 = only that timeslot's calls
    int p25_nac = -1;          // -1 = any; else only frames with this NAC
    bool invert = false;       // spectrum inverted (I/Q swapped source)
};

class Report {
public:
    struct Event {
        uint64_t seq;
        int64_t time_ms;       // wall clock (ms since epoch)
        Mode proto;
        std::string text;
    };

    // Channel fact. Kept in first-seen order per protocol; age shown in the UI.
    void set(Mode proto, const std::string& key, const std::string& value);
    void erase(Mode proto, const std::string& key);
    void clear(Mode proto);
    void clear_all();
    // Log an event. A text identical to one logged for the same protocol
    // within dedup_s seconds is dropped (control channels repeat their
    // broadcasts every few hundred ms).
    void event(Mode proto, const std::string& text, double dedup_s = 2.0);

    // JSON fragments for the status message
    std::string info_json(Mode proto) const;       // [["key","value",age_s],...]
    // Events with seq > after (at most max), oldest first
    std::string events_json(uint64_t after, size_t max) const;
    uint64_t last_seq() const;

private:
    struct Fact { std::string key, value; int64_t updated_ms; };
    mutable std::mutex mu_;
    std::map<Mode, std::vector<Fact>> facts_;
    std::deque<Event> events_;
    std::map<std::string, int64_t> recent_;   // proto|text -> last time (dedup)
    uint64_t seq_ = 0;
    static constexpr size_t MAX_EVENTS = 400;
};

int64_t now_ms();

}  // namespace dig
