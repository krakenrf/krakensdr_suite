#pragma once

// Decoded-information store of one digital decoder: a small table of
// "channel facts" per decoder plugin (colour code, NAC, MCC/MNC, current
// call...) plus an event log. Written by the decoder thread, read by the uWS
// loop when it builds the status JSON - everything is behind one mutex (a
// few hundred updates per second at most).

#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dig {

// OFF, AUTO (every plugin that declares auto_detect runs and the one whose
// frames pass their checks wins) or one fixed PLUGIN (plugins/<id>/; the
// built-in protocols are plugins too: p25, dmr, tetra, dstar, nxdn, mpt1327)
enum class Mode : int { OFF = 0, AUTO = 1, PLUGIN = 2 };
const char* mode_name(Mode m);     // "OFF", "AUTO", "PLUGIN"
// Wire / persisted form: "OFF", "AUTO" or "PLUGIN:<id>"
std::string mode_string(Mode m, const std::string& plugin_id);
// Parses mode_string(); also the old protocol names ("P25", "DSTAR", ...),
// which become the matching plugin. false for an unknown mode or bad id.
bool parse_mode_string(const std::string& s, Mode* m, std::string* plugin_id);
bool valid_plugin_id(const std::string& id);   // [a-z0-9_-]{1,32}, not "sdk"

// Options of one VFO's decoder (sidebar settings)
struct Options {
    bool verbose = false;      // also log repeating broadcasts / idle chatter
    bool invert = false;       // spectrum inverted (I/Q swapped source)
    bool map = false;          // plot the decoder's map points on the web UI's 🗺 Map (the decoder tab's "Plot on map")
    // the plugins' own options, "<plugin id>.<key>" -> value
    std::map<std::string, std::string> plugin;
};
// Persisted form "v=1&i=0&dmr.slot=1" (values percent-encoded); also reads
// the old "verbose/slot/nac/invert" form
std::string options_to_string(const Options& o);
Options options_from_string(const std::string& s);
bool valid_option_key(const std::string& k);   // "<plugin id>.<[a-z0-9_]{1,32}>"

int64_t now_ms();

// The receiver's location (Station Information), passed on to the plugins
// (kp::Host::station). valid = false: unknown.
void set_station_location(bool valid, double lat, double lon);

// A position a plugin put on the map (kp::MapPoint)
struct MapPoint {
    std::string plugin, id, label, kind, info;
    double lat = 0, lon = 0;
    float heading = NAN, alt_m = NAN, speed_kmh = NAN;
    double ttl_s = 300;
    int64_t updated_ms = 0;
    uint64_t seq = 0;
};

class Report {
public:
    struct Event {
        uint64_t seq;
        int64_t time_ms;       // wall clock (ms since epoch)
        std::string source;    // plugin id, "" = the decoder itself
        std::string text;
    };

    // Channel fact of a plugin. Kept in first-seen order; age shown in the UI.
    void set(const std::string& plugin, const std::string& key, const std::string& value);
    void erase(const std::string& plugin, const std::string& key);
    void clear(const std::string& plugin);
    void clear_all();
    // Log an event. A text identical to one logged by the same plugin within
    // dedup_s seconds is dropped (control channels repeat their broadcasts).
    // returns false if it was a repeat (not logged)
    bool event(const std::string& plugin, const std::string& text, double dedup_s = 2.0);
    // Display name of a plugin id (events carry it)
    void set_label(const std::string& plugin, const std::string& name);

    // JSON fragments for the status message
    std::string info_json() const;                  // {"<id>":[["key","value",age_s],...],...}
    // Events with seq > after (at most max), oldest first
    std::string events_json(uint64_t after, size_t max) const;
    uint64_t last_seq() const;

    // --- Map points (🗺 Map) ---
    void map_set(MapPoint p);   // same plugin + id = update
    void map_remove(const std::string& plugin, const std::string& id);
    void map_clear();
    size_t map_size() const;
    // For the map message: appends the points updated after seq `after` (all
    // for 0) to pts and the key of every live point ("vfo|plugin|id") to
    // keys, as comma-separated JSON items tagged with the VFO id. Drops
    // expired points first. Returns the newest map seq.
    uint64_t map_json(int vfo, uint64_t after, std::string& pts, std::string& keys);

    // --- Tables (kp::Host::table_*: e.g. the aircraft ADS-B hears) ---
    void table_columns(const std::string& plugin, std::vector<std::string> cols);
    void table_row(const std::string& plugin, const std::string& key, std::vector<std::string> cells);
    void table_remove(const std::string& plugin, const std::string& key);
    void table_clear();   // the rows (VFO retuned / decoder switched); the headings stay
    // {"<plugin>":{"cols":[..],"rows":[[key, age_s, cell, ...],...]}} - rows
    // not updated for TABLE_ROW_TTL_MS left out
    std::string tables_json() const;

private:
    struct Fact { std::string key, value; int64_t updated_ms; };
    mutable std::mutex mu_;
    std::map<std::string, std::vector<Fact>> facts_;
    std::map<std::string, std::string> labels_;
    std::deque<Event> events_;
    std::map<std::string, int64_t> recent_;   // id|text -> last time (dedup)
    uint64_t seq_ = 0;
    static constexpr size_t MAX_EVENTS = 400;
    std::map<std::string, MapPoint> map_;   // "plugin|id"
    uint64_t map_seq_ = 0;
    static constexpr size_t MAX_MAP_POINTS = 2000;
    void map_expire(int64_t t);
    struct TableRow { std::vector<std::string> cells; int64_t updated_ms; };
    struct Table { std::vector<std::string> cols; std::map<std::string, TableRow> rows; };
    std::map<std::string, Table> tables_;
    static constexpr size_t MAX_TABLE_ROWS = 1000;
    static constexpr int64_t TABLE_ROW_TTL_MS = 600000;
};

}  // namespace dig
