// Trunk following - see trunk_follow.hpp and docs/dev/trunk-following.md.
//
// The decoders hand over what their plugins report about a trunked system
// (on_plugin_event, any thread) -> a bounded queue -> the uWS loop, where
// everything below lives: the systems (what the user knows about each: names,
// talkgroups, the channel table; trunk_systems.json), the calls (one per
// system + target + group/unit, from the grants), the voice VFOs (made through
// the decimator manager, set up through apply_command like a browser's
// commands, never saved), the state machine (tick(), every TICK_MS).

#include "trunk_follow.hpp"

#include "config.hpp"
#include "channel_manager.hpp"
#include "continuous_scanner.hpp"
#include "control_handler.hpp"
#include "decimator_manager.hpp"
#include "decoder_log.hpp"
#include "digital/dig_plugin.hpp"
#include "digital/dig_report.hpp"
#include "geo_address.hpp"   // geo::json_parse
#include "globals.hpp"
#include "message_builders.hpp"
#include "networking/websocket_server.hpp"
#include "playback.hpp"
#include "scanner_manager.hpp"
#include "sf_scanner.hpp"
#include "signal_processing/audio_mixer.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

extern std::atomic<float> current_edge_clip;   // main.cpp

namespace trunk {
namespace {

using Inst = DecimatorManager::DecimatorInstance;

// --- limits and timings (docs/dev/trunk-following.md, "Limits") ------------
constexpr size_t QUEUE_MAX = 4096;            // events waiting for the loop (more: dropped, counted)
constexpr size_t MAX_SYSTEMS = 32;
constexpr size_t MAX_TGS = 2000;              // per system (talkgroups listed + heard)
constexpr size_t MAX_CHANS = 2000;            // per system (channel table entries)
constexpr size_t MAX_RULES = 32;              // per system
constexpr size_t MAX_UNITS = 1000;            // per system (units heard, not saved)
constexpr size_t MAX_UNRESOLVED = 200;        // per system
constexpr size_t MAX_CALLS = 64;              // calls in progress (followed or not), all systems
constexpr size_t HISTORY_MAX = 50;
constexpr size_t MAX_SOURCES = 8;             // sources kept per call
constexpr int VFO_LIMIT = 16;                 // = DecimatorManager::MAX_DECIMATORS (private there)
constexpr int64_t SETUP_GIVEUP_MS = 3000;     // SETUP: nothing received for this long -> no_signal ...
constexpr int64_t SETUP_MAX_MS = 6000;        // ... or up to this long while its grant updates still come (late entry)
constexpr int64_t FRAMES_ACTIVE_MS = 400;     // the decoder's valid frames this recent = ACTIVE
constexpr int64_t STALE_FRAMES_MS = 100;      // frames this soon after a (re)placement may be the old channel's
constexpr int64_t CALL_ACTIVE_MS = 1500;      // a CALL ACTIVE from the voice VFO counts this long
constexpr int64_t GRANT_FRESH_MS = 2000;      // a grant this recent keeps a call in HANG alive ...
constexpr int64_t MAX_SILENT_MS = 10000;      // ... but never past this long without activity
constexpr int64_t WAIT_EXPIRE_MS = 3000;      // a call not followed: over this long after its last grant
constexpr int64_t RECENT_MS = 30000;          // an over call stays listed ("recent") this long - the next over's grant revives it
constexpr int64_t REVIVE_UPDATE_MS = 2500;    // ... a grant UPDATE on the same channel + slot only this long after its end (the trailing repeats of the
                                              // over just ended don't bring it back; a first grant always does)
constexpr int64_t CONTROL_LOST_MS = 5000;     // a control VFO without any report this long: not receiving
constexpr int64_t PARK_KEEP_MS = 30000;       // a parked voice VFO is removed after this long unused
constexpr int64_t RESET_EXPECT_MS = 2000;     // our own retune of a VFO: its RESET within this is expected
constexpr int64_t CONTROL_MOVE_MIN_MS = 10000;   // CONTROL: a control VFO moved at most this often
constexpr int64_t PLACE_MIN_MS = 100;         // a voice VFO re-placed at most this often (grant floods)
constexpr int64_t LEARN_WINDOW_MS = 3000;     // passive learning: a CALL this soon after the grant
constexpr int64_t LEARN_SEPARATE_MS = 5000;   // ... the second observation from another grant this much later
constexpr double PLACE_GUARD_HZ = 2000;       // kept between a channel's edge and the usable band's (AFC, filter)
constexpr double LEARN_TOL_HZ = 1500;         // two observations agree within this
constexpr float ANALOG_SQ_DB = 10.0f;         // the level squelch of an analogue (nfm) traffic channel
constexpr int64_t STATUS_MIN_MS = 250;        // status at most 4 / s ...
constexpr int64_t STATUS_ACTIVE_MS = 1000;    // ... and at least 1 / s while anything is active
constexpr int64_t LISTS_STRUCT_MS = 1000;     // the big lists after a change of what is in them
constexpr int64_t LISTS_STATS_MS = 10000;     // ... after counters / last-heard only
constexpr int64_t SAVE_EDIT_MS = 0;           // the systems file: a user's edit at once,
constexpr int64_t SAVE_LEARN_MS = 30000;      // ... new announced / learned / heard entries within 30 s,
constexpr int64_t SAVE_STATS_MS = 600000;     // ... counters and last-heard times every 10 min
constexpr const char* SYSTEMS_FILE = "trunk_systems.json";
constexpr size_t SYSTEMS_FILE_MAX = 8u << 20;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
int64_t wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool fin(double v) { return is_finite_value(v); }
std::string num_json(double v, const char* fmt = "%.0f") {
    if (!fin(v)) return "null";
    char b[48];
    snprintf(b, sizeof b, fmt, v);
    return b;
}
const char* jb(bool b) { return b ? "true" : "false"; }
std::string js(const std::string& s) { return "\"" + json_escape(s) + "\""; }

// ============================================================================
// State (uWS loop only, unless marked)
// ============================================================================

struct Settings {
    bool follow = false;     // TRUNK_FOLLOW
    bool all_sites = false;  // TRUNK_ALL_SITES: a talkgroup followed through every site that grants it
    int max_vfos = 2;        // TRUNK_MAX_VFOS 1..8
    bool enc = true;         // TRUNK_ENCRYPTED: follow encrypted calls
    bool unit = true;        // TRUNK_UNIT: follow unit-to-unit calls
    bool audio = true;       // TRUNK_AUDIO: hear followed calls
    double hang_s = 2.0;     // TRUNK_HANG 0.5..10
};
Settings g_set;

enum TgMode { TG_NORMAL = 0, TG_PRIORITY = 1, TG_SKIP = 2 };
const char* tg_mode_name(int m) { return m == TG_PRIORITY ? "priority" : m == TG_SKIP ? "skip" : "normal"; }

struct Tg {
    std::string name;
    int mode = TG_NORMAL;
    bool listed = false;     // the user's entry (TRUNK_TG_SET) - "listed only" follows these
    int64_t last = 0;        // last heard (epoch ms, 0 = never)
    uint64_t calls = 0;
};

struct Chan {
    long long num = -1;
    double user_hz = NAN, user_up = NAN;      // the user's entry
    double ann_hz = NAN, ann_up = NAN;        // announced by the system (CHANNEL, or a grant's frequency)
    double learned_hz = NAN;                  // passive learning
    int learn_fail = 0;                       // calls on the learned frequency that received nothing (not saved)
    uint64_t grants = 0;                      // calls granted on it
    uint64_t data = 0;                        // data grants on it (counted only: no call rows)
    int64_t last = 0;                         // last granted (epoch ms)
};

struct Rule {
    long long first = 0, last = 0;
    double base_hz = 0, step_hz = 0;
    double up_off_hz = NAN;                   // uplink = downlink + this (NAN = none)
};

struct Unit {
    int64_t last = 0;
    uint64_t calls = 0;
};

struct Unresolved {
    long long num = -1;
    uint64_t n = 0;
    int64_t last = 0;
};

// a control VFO of a system = one site of it
struct Ctl {
    int64_t last_ms = 0;      // last report (steady)
    std::string site;
    uint64_t grant_msgs = 0, grants = 0, followed = 0, duplicate = 0, data = 0, skip = 0;
};

struct System {
    std::string id, plugin, key;           // id = plugin:key (key "" -> plugin:@<vfo>, not saved)
    std::string name, site;                // as reported
    std::string user_name;
    bool listed_only = false;
    std::map<std::string, Tg> tgs;
    std::map<std::string, Chan> chans;
    std::vector<Rule> rules;
    std::map<std::string, Unit> units;
    std::map<std::string, Unresolved> unresolved;
    std::map<int, Ctl> ctl;                // control VFOs
    // counters (this run)
    uint64_t grant_msgs = 0, grants = 0, followed = 0, no_signal = 0, moves = 0, preempted = 0;
    std::map<std::string, uint64_t> notf;  // calls not followed, by their first reason (data / skip: grants)
    std::unordered_map<std::string, int64_t> quiet;   // data / skip grants counted lately (key -> steady ms)
    bool heard = false;                    // an event of it has arrived (ever: saved) - false = the user's record only
    // the big lists changed: what is in them / only counters
    bool lists_struct = true, lists_stats = false;
    int64_t lists_sent = 0;
    // the suggested centre (status), recomputed every 2 s
    struct CentreCache {
        double hz = 0, w = 0, total = 0, now_w = 0;
        int n = 0, of = 0, ctl = 0, ctl_of = 0;
    } centre;
    bool centre_ok = false;
    int64_t centre_t = 0;
};
std::map<std::string, System> g_sys;
bool temp_id(const std::string& id) { return id.find(":@") != std::string::npos; }

// --- the systems file: dirty levels and the text waiting for main()'s last save
int64_t g_dirty_since = 0;     // 0 = clean
int64_t g_dirty_within = 0;    // save within this (ms)
std::mutex g_save_mu;
std::string g_unsaved;         // serialised, not yet written (save_now writes it)
std::atomic<bool> g_loaded{false};

void mark_dirty(int64_t within) {
    const int64_t now = now_ms();
    if (!g_dirty_since) {
        g_dirty_since = now;
        g_dirty_within = within;
    } else {
        g_dirty_within = std::min(g_dirty_within, within + (now - g_dirty_since));
    }
}

// --- calls --------------------------------------------------------------
// RECENT: over, kept listed for RECENT_MS (the same row comes back with the next over's grant)
enum CState { WAIT, SETUP, ACTIVE, HANG, RECENT };
const char* cstate_name(int s) {
    return s == SETUP ? "setup" : s == ACTIVE ? "active" : s == HANG ? "hang" : s == RECENT ? "recent" : "wait";
}

struct Call {
    uint64_t id = 0;
    std::string sys, key;
    std::string target, source;
    bool group = true;
    std::vector<std::string> sources;
    Channel ch;
    double hz = NAN, up_hz = NAN;
    std::string fsrc;                       // where hz came from: grant | user | rule | announced | learned
    bool enc = false, emerg = false, data = false, prio = false;
    std::string decoder, demod;
    double bw = 0;
    std::vector<std::pair<std::string, std::string>> options;
    int st = WAIT;
    std::string why;                        // WAIT: why it isn't followed
    int vfo = -1;                           // the voice VFO (followed), -1 = none
    int ctl_vfo = -1;                       // the VFO its last grant came from
    int64_t t0_wall = 0;                    // first grant (epoch ms)
    int64_t t_start = 0, t_grant = 0, t_state = 0, t_activity = 0, t_placed = 0, t_hang = 0;
    int64_t t_rx_grant = 0;                 // when the latest grant reached the loop (latency)
    int64_t t_call_active = 0, t_call_ended = 0;   // CALL from the voice VFO
    bool user_ended = false;                // TRUNK_END / its VFO removed by hand: not rejoined
    bool setup_failed = false;              // SETUP gave up: not followed again
    bool ever_followed = false, counted = false;
    bool pending_place = false;             // re-placement waiting for PLACE_MIN_MS
    int moves = 0;
    std::string end;                        // RECENT: how it ended
    int64_t t_end = 0;                      // RECENT: when (steady)
    int overs = 0;                          // grants that started it again (transmission trunking)
    bool guard = false;                     // RECENT: it was followed (or a duplicate of a followed one): trailing updates ignored
};
std::map<uint64_t, Call> g_calls;
std::unordered_map<std::string, uint64_t> g_call_by_key;
uint64_t g_next_call = 1;
std::deque<std::string> g_history;          // finished calls, JSON, newest last
uint64_t g_dropped_calls = 0;               // grants that would have made more than MAX_CALLS calls

// One call = system + site (the control VFO that granted it) + group / unit + target: a
// wide-area talkgroup granted on two sites at once is two carriers of one conversation
std::string call_key(const std::string& sys, int site, bool group, const std::string& target) {
    return sys + "\x1f" + std::to_string(site) + "\x1f" + (group ? "g" : "u") + "\x1f" + target;
}

// --- voice VFOs ---------------------------------------------------------------
struct Voice {
    int vfo = -1;
    std::weak_ptr<Inst> inst;
    const void* dec = nullptr;              // its DigitalDecoder (RESETs of ours told apart)
    uint64_t call = 0;                      // 0 = parked
    uint64_t last_call = 0;                 // the call it had last (its next over gets it back)
    int64_t parked_ms = 0;
    // what we set (a difference = the user changed it: taken back at the next grant)
    std::string plugin, demod;
    int bw = 0;
    float off_hz = 0;
    int tuner = 0;
    int mode = -1;                          // operating mode it was placed in
    double centre_hz = 0;                   // its tuner's centre then
    double rf_hz = 0;
    int64_t t_placed = 0;
    bool dirty = false;
    bool reset_seen = false;                // its decoder's RESET after the latest placement came
};
std::vector<Voice> g_voices;
void park(Voice& v);

// --- the VFOs that report (control / traffic) -------------------------------
struct Track {
    const void* dec = nullptr;
    std::string plugin, sys;
    bool control = false;
    bool system_said = false;               // a SYSTEM came (its control flag decides)
    int64_t last_ms = 0;
    int64_t expect_reset_until = 0;
    int64_t moved_ms = 0;                   // CONTROL: last moved
    std::string control_note;               // CONTROL that couldn't be done
};
std::map<int, Track> g_tracks;

// --- passive learning -------------------------------------------------------
struct LearnObs {
    double hz = NAN;
    uint64_t call = 0;
    int over = 0;
    int64_t t = 0;
    int n = 0;
};
std::map<std::string, LearnObs> g_learn;     // sys \x1f channel -> the observation so far
uint64_t g_learned_n = 0, g_learn_refused = 0, g_learn_dropped = 0;

// --- other --------------------------------------------------------------
bool g_dispatching = false;
bool g_paused = false;
std::string g_pause_why, g_pause_why_last;
int g_mode = -1;                            // operating mode last seen
bool g_pb = false;                          // playback last seen
std::map<std::string, System> g_pb_saved;   // the systems before the playback
std::string g_hold_sys, g_hold_tg;          // TRUNK_HOLD
std::deque<int64_t> g_lat_place, g_lat_active;   // ms: grant reached the loop -> VFO placed / -> ACTIVE
std::atomic<uint64_t> g_events_in{0}, g_events_dropped{0};
int64_t g_fence_until = 0;   // a playback started / ended: what the decoders report till then is the other side's
int64_t g_status_sent = 0;
std::string g_status_last;
bool g_status_now = false;
std::string g_last_err;

// Scanners move the user's VFOs, control VFOs included: where each control VFO
// was (kept up to date while not paused; frozen while paused), put back when
// the pause ends - and what was done, for the status (scan_restore, 60 s)
struct Home {
    std::weak_ptr<Inst> inst;
    double rf = 0;
    int bw = 0, tuner = 0;
    std::string mode;   // dig::mode_string
    std::string sys;
};
std::map<int, Home> g_home;
struct Restore {
    int vfo;
    std::string sys, note;
    double hz;
    bool restored;
    int64_t t;
};
std::vector<Restore> g_restore;

// --- the queue from the decoders (any thread) ---------------------------------
struct QEv {
    int vfo;
    const void* dec;
    std::string plugin;
    PluginEvent ev;
    int64_t t_rx;
};
std::mutex g_qmu;
std::deque<QEv> g_queue;
bool g_drain_posted = false;

// tests: the last events per VFO
std::mutex g_recent_mu;
std::map<int, std::deque<std::pair<std::string, PluginEvent>>> g_recent;

// --- VFO metadata for decimator_info / the snapshot (any thread) -------------
struct Meta {
    const Inst* inst = nullptr;
    bool voice = false;
    std::string json;
};
std::mutex g_meta_mu;
std::map<int, Meta> g_meta;

void broadcast(const std::string& s) { WebSocketServer::broadcast_json_message(s); }

// ============================================================================
// Helpers
// ============================================================================

// Text from a command: printable (no control characters), valid UTF-8, cut at max bytes, trimmed
std::string clean(const std::string& s, size_t max) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = json_utf8_len(s, i);
            if (n == 0) { i++; continue; }
            if (o.size() + n > max) break;
            o.append(s, i, n);
            i += n;
            continue;
        }
        i++;
        if (c < 0x20 || c == 0x7f) continue;
        if (o.size() + 1 > max) break;
        o += static_cast<char>(c);
    }
    const size_t a = o.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    return o.substr(a, o.find_last_not_of(' ') - a + 1);
}

std::shared_ptr<Inst> live(const Voice& v) {
    auto s = v.inst.lock();
    if (!s || v.vfo < 0) return nullptr;
    auto now = decimator_manager.getDecimator(v.vfo);
    return now == s ? s : nullptr;
}

Voice* voice_of_vfo(int vfo) {
    for (auto& v : g_voices)
        if (v.vfo == vfo && live(v)) return &v;
    return nullptr;
}
Voice* voice_of_call(const Call& c) {
    if (c.vfo < 0) return nullptr;
    for (auto& v : g_voices)
        if (v.vfo == c.vfo && v.call == c.id) return &v;
    return nullptr;
}

double usable_half_hz() {
    float clip = current_edge_clip.load(std::memory_order_relaxed);
    if (!(clip >= 0.1f)) clip = 0.1f;
    if (clip > 1.0f) clip = 1.0f;
    return static_cast<double>(clip) * SAMPLE_RATE / 2.0;
}

int n_tuners() { return std::clamp(std::min(active_num_elements.load(), std::max(1, num_channels.load())), 1, MAX_CHANNELS); }

// the centre of tuner t in operating mode m (Hz, 0 = not known)
double tuner_centre(int mode, int t) {
    if (mode == 2) return static_cast<double>(exact_tuner_hz(t));
    if (mode == 1) return static_cast<double>(ChannelManager::get_frequency(t));
    return static_cast<double>(coherent_tuner_hz());
}

// the RF of a VFO (Hz) now
double vfo_rf(const Inst& inst) {
    const int mode = operating_mode_index();
    const int t = mode == 0 ? 0 : std::clamp(inst.tuner_channel.load(), 0, MAX_CHANNELS - 1);
    const double c = tuner_centre(mode, t);
    return c > 0 ? c + inst.frequency_offset_hz.load() : 0.0;
}

struct Place {
    bool ok = false;
    int tuner = 0;
    double centre = 0;     // the tuner's centre
    double off_hz = 0;     // what SET_DECIMATOR_FREQ takes (wideband: from the spread's middle)
};

// Where the channel hz (bw wide) goes in the receiver's span now - never a
// retune: coherent = the array's span, wideband scan = the tuner whose pane
// holds it, independent = a tuner whose span holds it (prefer first)
Place place_for(double hz, double bw, int prefer) {
    Place p;
    if (!fin(hz) || hz <= 0) return p;
    const int mode = operating_mode_index();
    const double half = usable_half_hz();
    const double need = bw / 2 + PLACE_GUARD_HZ;
    if (mode == 0) {
        const double c = tuner_centre(0, 0);
        if (c <= 0 || std::fabs(hz - c) + need > half) return p;
        p.ok = true;
        p.centre = c;
        p.off_hz = hz - c;
        return p;
    }
    const int n = n_tuners();
    if (mode == 1) {
        // the receiver's SET_DECIMATOR_FREQ puts a wideband VFO on the tuner
        // nearest it: that tuner's pane (half the spacing each side) must hold it
        int best = -1;
        double bd = 0;
        for (int k = 0; k < n; k++) {
            const double c = tuner_centre(1, k);
            if (c <= 0) continue;
            const double d = std::fabs(hz - c);
            if (best < 0 || d < bd) { best = k; bd = d; }
        }
        if (best < 0) return p;
        double pane = half;
        if (n >= 2) {
            const double sp = std::fabs(tuner_centre(1, 1) - tuner_centre(1, 0));
            if (sp > 0) pane = std::min(pane, sp / 2);
        }
        if (bd + need > pane) return p;
        const double mid = (tuner_centre(1, 0) + tuner_centre(1, n - 1)) / 2.0;
        p.ok = true;
        p.tuner = best;
        p.centre = tuner_centre(1, best);
        p.off_hz = hz - mid;
        return p;
    }
    std::vector<int> order;
    if (prefer >= 0 && prefer < n) order.push_back(prefer);
    for (int k = 0; k < n; k++)
        if (k != prefer) order.push_back(k);
    for (int k : order) {
        const double c = tuner_centre(2, k);
        if (c <= 0 || std::fabs(hz - c) + need > half) continue;
        p.ok = true;
        p.tuner = k;
        p.centre = c;
        p.off_hz = hz - c;
        return p;
    }
    return p;
}

void dispatch(const std::vector<std::string>& cmds) {
    const bool was = g_dispatching;
    g_dispatching = true;
    for (const auto& c : cmds) {
        const std::string why = ControlHandler::apply_command(c);
        if (!why.empty()) std::cerr << "Trunk following: '" << c << "' refused (" << why << ")" << std::endl;
    }
    g_dispatching = was;
}

void set_heard(int vfo, bool on) {
    if (!decimator_manager.getDecimator(vfo)) return;
    auto heard = decimator_manager.audioVfoIds();
    const bool is = std::find(heard.begin(), heard.end(), vfo) != heard.end();
    if (is == on) return;
    if (on) heard.push_back(vfo);
    else heard.erase(std::remove(heard.begin(), heard.end(), vfo), heard.end());
    // not ControlHandler::set_audio_vfos: its decimator_info broadcast would make
    // every page rebuild its VFO cards at each over (the trunk status carries "heard")
    decimator_manager.setAudioVfos(heard);
    audio_mixer.retain(decimator_manager.audioVfoIds());
    g_status_now = true;
}

// the plugin's facts the manager needs; false = no such plugin
bool plugin_info(const std::string& id, dig::PluginInfo* pi) {
    return !id.empty() && dig::PluginRegistry::instance().get(id, pi);
}

System* sys_of(const std::string& id) {
    auto it = g_sys.find(id);
    return it == g_sys.end() ? nullptr : &it->second;
}

std::string label_of(const System& s, const Call& c) {
    std::string l = c.group ? "TG " + c.target : "Unit " + c.target;
    if (c.group) {
        auto it = s.tgs.find(c.target);
        if (it != s.tgs.end() && !it->second.name.empty()) l += " " + it->second.name;
    }
    return l;
}

// --- VFO metadata ------------------------------------------------------------
void meta_set(int vfo, const Inst* inst, bool voice, const std::string& json) {
    std::lock_guard<std::mutex> lk(g_meta_mu);
    Meta& m = g_meta[vfo];
    m.inst = inst;
    m.voice = voice;
    m.json = json;
}
void meta_erase(int vfo, bool voice) {
    std::lock_guard<std::mutex> lk(g_meta_mu);
    auto it = g_meta.find(vfo);
    if (it != g_meta.end() && it->second.voice == voice) g_meta.erase(it);
}

void meta_voice(const Voice& v) {
    auto inst = live(v);
    if (!inst) return;
    std::string j;
    auto it = v.call ? g_calls.find(v.call) : g_calls.end();
    if (it == g_calls.end()) {
        j = ",\"trunk\":{\"role\":\"voice\",\"call\":0}";
    } else {
        const Call& c = it->second;
        const System* s = sys_of(c.sys);
        // (as placed: the call's state, heard / parked come with the trunk status' vfos)
        j = ",\"trunk\":{\"role\":\"voice\",\"call\":" + std::to_string(c.id) + ",\"sys\":" + js(c.sys) + ",\"ctl\":" + std::to_string(c.ctl_vfo) +
            ",\"label\":" + js(s ? label_of(*s, c) : c.target) + ",\"target\":" + js(c.target) +
            ",\"group\":" + jb(c.group) + ",\"ch\":" + js(c.ch.name) + ",\"slot\":" + std::to_string(c.ch.slot) + "}";
    }
    meta_set(v.vfo, inst.get(), true, j);
}

void meta_control(int vfo, const Track& t) {
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst) return;
    meta_set(vfo, inst.get(), false,
             ",\"trunk\":{\"role\":\"" + std::string(t.control ? "control" : "traffic") + "\",\"sys\":" + js(t.sys) + "}");
}

// ============================================================================
// Systems
// ============================================================================

System& ensure_system(const std::string& id, const std::string& plugin, const std::string& key) {
    auto it = g_sys.find(id);
    if (it != g_sys.end()) return it->second;
    if (g_sys.size() >= MAX_SYSTEMS) {
        // the oldest temporary (unnamed) one goes first, else one without control VFOs
        std::string drop;
        for (const auto& kv : g_sys)
            if (temp_id(kv.first) && kv.second.ctl.empty()) { drop = kv.first; break; }
        if (drop.empty())
            for (const auto& kv : g_sys)
                if (kv.second.ctl.empty()) { drop = kv.first; break; }
        if (!drop.empty()) g_sys.erase(drop);
    }
    System& s = g_sys[id];
    s.id = id;
    s.plugin = plugin;
    s.key = key;
    s.lists_struct = true;
    if (!temp_id(id)) mark_dirty(SAVE_LEARN_MS);
    return s;
}

// the frequency of channel ch in system s: the grant's, a user entry, a user
// rule, an announced or learned entry. src = which; false = unresolved
bool resolve(const System& s, const Channel& ch, double* hz, double* up, std::string* src) {
    *hz = NAN;
    *up = NAN;
    if (fin(ch.freq_hz)) {
        *hz = ch.freq_hz;
        *up = ch.uplink_hz;
        *src = "grant";
        return true;
    }
    auto it = s.chans.find(ch.name);
    if (it != s.chans.end() && fin(it->second.user_hz)) {
        *hz = it->second.user_hz;
        *up = it->second.user_up;
        *src = "user";
        return true;
    }
    const long long n = ch.number >= 0 ? ch.number : (it != s.chans.end() ? it->second.num : -1);
    if (n >= 0)
        for (const auto& r : s.rules)
            if (n >= r.first && n <= r.last) {
                *hz = r.base_hz + static_cast<double>(n - r.first) * r.step_hz;
                *up = fin(r.up_off_hz) ? *hz + r.up_off_hz : NAN;
                *src = "rule";
                return *hz > 0;
            }
    if (it != s.chans.end()) {
        if (fin(it->second.ann_hz)) {
            *hz = it->second.ann_hz;
            *up = it->second.ann_up;
            *src = "announced";
            return true;
        }
        if (fin(it->second.learned_hz)) {
            *hz = it->second.learned_hz;
            *src = "learned";
            return true;
        }
    }
    return false;
}

Chan* ensure_chan(System& s, const std::string& name) {
    auto it = s.chans.find(name);
    if (it != s.chans.end()) return &it->second;
    if (s.chans.size() >= MAX_CHANS) {
        // the least recently granted entry without a user frequency goes
        auto drop = s.chans.end();
        for (auto i = s.chans.begin(); i != s.chans.end(); ++i)
            if (!fin(i->second.user_hz) && (drop == s.chans.end() || i->second.last < drop->second.last)) drop = i;
        if (drop == s.chans.end()) return nullptr;
        s.chans.erase(drop);
    }
    s.lists_struct = true;
    return &s.chans[name];
}

Tg* ensure_tg(System& s, const std::string& id) {
    auto it = s.tgs.find(id);
    if (it != s.tgs.end()) return &it->second;
    if (s.tgs.size() >= MAX_TGS) {
        auto drop = s.tgs.end();
        for (auto i = s.tgs.begin(); i != s.tgs.end(); ++i)
            if (!i->second.listed && (drop == s.tgs.end() || i->second.last < drop->second.last)) drop = i;
        if (drop == s.tgs.end()) return nullptr;
        s.tgs.erase(drop);
    }
    s.lists_struct = true;
    return &s.tgs[id];
}

void note_unit(System& s, const std::string& id, bool call) {
    if (id.empty()) return;
    auto it = s.units.find(id);
    if (it == s.units.end()) {
        if (s.units.size() >= MAX_UNITS) {
            auto drop = s.units.begin();
            for (auto i = s.units.begin(); i != s.units.end(); ++i)
                if (i->second.last < drop->second.last) drop = i;
            s.units.erase(drop);
        }
        it = s.units.emplace(id, Unit{}).first;
        s.lists_struct = true;
    }
    it->second.last = wall_ms();
    if (call) it->second.calls++;
    s.lists_stats = true;
}

// A temporary system (no key yet) becomes the keyed one: what it gathered moves there
// (by value: a caller passes a Track's sys, which the loop below rewrites)
void merge_temp(const std::string from, const std::string to, const std::string plugin, const std::string key) {
    if (from == to || !g_sys.count(from)) return;
    System& d = ensure_system(to, plugin, key);
    auto it = g_sys.find(from);
    if (it == g_sys.end()) return;   // (made room for the new one)
    System& t = it->second;
    for (auto& kv : t.chans) {
        Chan& c = d.chans[kv.first];
        if (c.num < 0) c.num = kv.second.num;
        if (fin(kv.second.ann_hz)) {
            c.ann_hz = kv.second.ann_hz;
            c.ann_up = kv.second.ann_up;
        }
        c.grants += kv.second.grants;
        c.last = std::max(c.last, kv.second.last);
    }
    for (auto& kv : t.tgs) {
        Tg& g = d.tgs[kv.first];
        g.calls += kv.second.calls;
        g.last = std::max(g.last, kv.second.last);
    }
    for (auto& kv : t.units) d.units[kv.first] = kv.second;
    for (auto& kv : t.unresolved) d.unresolved[kv.first] = kv.second;
    for (auto& kv : t.ctl) d.ctl[kv.first] = kv.second;
    d.grant_msgs += t.grant_msgs;
    d.grants += t.grants;
    d.followed += t.followed;
    d.no_signal += t.no_signal;
    d.moves += t.moves;
    d.preempted += t.preempted;
    for (auto& kv : t.notf) d.notf[kv.first] += kv.second;
    d.lists_struct = true;
    // its calls move along (keys change); one the keyed system has already
    // (the same site and target - from before a RESET) gives way: a RECENT one
    // goes, a live one stays and the moving one goes
    std::vector<uint64_t> gone;
    for (auto& kv : g_calls) {
        Call& c = kv.second;
        if (c.sys != from) continue;
        g_call_by_key.erase(c.key);
        c.sys = to;
        c.key = call_key(to, c.ctl_vfo, c.group, c.target);
        auto k = g_call_by_key.find(c.key);
        if (k != g_call_by_key.end()) {
            auto o = g_calls.find(k->second);
            if (o != g_calls.end() && o->second.st != RECENT) {
                gone.push_back(c.id);
                continue;
            }
            if (o != g_calls.end()) gone.push_back(o->first);
        }
        g_call_by_key[c.key] = c.id;
    }
    for (uint64_t id : gone) {
        auto it2 = g_calls.find(id);
        if (it2 == g_calls.end()) continue;
        if (Voice* v = voice_of_call(it2->second)) park(*v);
        auto k = g_call_by_key.find(it2->second.key);
        if (k != g_call_by_key.end() && k->second == id) g_call_by_key.erase(k);
        g_calls.erase(it2);
    }
    for (auto& kv : g_tracks)
        if (kv.second.sys == from) kv.second.sys = to;
    g_sys.erase(from);
    mark_dirty(SAVE_LEARN_MS);
}

// The centre that keeps every control VFO of s in span and covers the most
// known channel frequencies (an entry of the table or resolved by a rule),
// weighted by their grants. When the control VFOs don't fit one span
// together, the one that keeps the most of them (then the most channels): the
// best partial centre, ctl < ctl_of. "Fits" = the whole channel (12.5 kHz;
// a control VFO: its own bandwidth) plus PLACE_GUARD_HZ inside the usable
// band (edge clip x 2.4 MHz) - the rule placement uses. false = nothing known
using Centre = System::CentreCache;
bool suggest_centre(const System& s, Centre* out) {
    const double half = usable_half_hz();
    const double need_ch = 12500.0 / 2 + PLACE_GUARD_HZ;   // (a narrowband channel)
    std::vector<std::pair<double, double>> ch;   // (hz, weight)
    for (const auto& kv : s.chans) {
        double hz, up;
        std::string src;
        Channel c;
        c.name = kv.first;
        c.number = kv.second.num;
        if (!resolve(s, c, &hz, &up, &src)) continue;
        ch.push_back({hz, static_cast<double>(std::max<uint64_t>(1, kv.second.grants))});
    }
    // the control VFOs (their own bandwidth)
    std::vector<std::pair<double, double>> ctl;   // (hz, need)
    for (const auto& kv : s.ctl) {
        auto inst = decimator_manager.getDecimator(kv.first);
        if (!inst) continue;
        const double f = vfo_rf(*inst);
        if (f > 0) ctl.push_back({f, inst->bandwidth_hz.load() / 2.0 + PLACE_GUARD_HZ});
    }
    if (ch.empty() && ctl.empty()) return false;
    const double reach = half - need_ch;   // a channel this far from the centre still fits
    std::sort(ch.begin(), ch.end());
    std::vector<double> pre(ch.size() + 1, 0.0);
    for (size_t i = 0; i < ch.size(); i++) pre[i + 1] = pre[i] + ch[i].second;
    // candidates: every window edge on a channel or a control VFO
    std::vector<double> cands;
    for (const auto& c : ch) {
        cands.push_back(c.first - (reach - 500));   // (whole kHz below)
        cands.push_back(c.first + (reach - 500));
        cands.push_back(c.first);
    }
    for (const auto& c : ctl) {
        cands.push_back(c.first - (half - c.second - 500));
        cands.push_back(c.first + (half - c.second - 500));
        cands.push_back(c.first);
    }
    const double cur = coherent_tuner_hz();
    auto weigh = [&](double c, int* n) {
        auto a = std::lower_bound(ch.begin(), ch.end(), std::make_pair(c - reach - 0.5, -1e300));
        auto b = std::upper_bound(ch.begin(), ch.end(), std::make_pair(c + reach + 0.5, 1e300));
        const size_t ia = static_cast<size_t>(a - ch.begin()), ib = static_cast<size_t>(b - ch.begin());
        *n = static_cast<int>(ib - ia);
        return pre[ib] - pre[ia];
    };
    auto ctls = [&](double c) {
        int k = 0;
        for (const auto& x : ctl)
            if (std::fabs(x.first - c) + x.second <= half) k++;
        return k;
    };
    Centre best;
    bool any = false;
    for (double c : cands) {
        c = std::round(c / 1000.0) * 1000.0;
        if (c < RTL_TUNER_MIN_HZ && !wb_variant_enabled.load()) continue;
        int n;
        const double w = weigh(c, &n);
        const int k = ctls(c);
        const bool better = !any || k > best.ctl || (k == best.ctl && (w > best.w + 1e-9 ||
                            (std::fabs(w - best.w) < 1e-9 && std::fabs(c - cur) < std::fabs(best.hz - cur))));
        if (better) {
            best.hz = c;
            best.w = w;
            best.n = n;
            best.ctl = k;
            any = true;
        }
    }
    if (!any) return false;
    best.total = pre.back();
    best.of = static_cast<int>(ch.size());
    best.ctl_of = static_cast<int>(ctl.size());
    int n_now;
    best.now_w = cur > 0 ? weigh(cur, &n_now) : 0;
    *out = best;
    return true;
}

// ============================================================================
// History and the decoder log
// ============================================================================

std::string call_json(const Call& c, bool ages, int64_t now);

// Not-followed calls the user can do something about go to the history (and
// the decoder log) as well as the followed ones: no frequency (enter it), out
// of span (retune), no free VFO / the VFO limit, encrypted / unit / unlisted
// (a setting), no decoder, no signal (a wrong channel table). Not: off, paused,
// hold, ended (the user's own doing), duplicate (the followed copy is there).
bool history_worthy(const Call& c) {
    if (c.ever_followed) return true;
    static const char* act[] = {"no_freq", "out_of_span", "no_vfo", "vfo_limit", "encrypted", "unit", "unlisted",
                                "no_decoder", "no_signal"};
    for (const char* a : act)
        if (c.why == a) return true;
    return false;
}

void record_history(const Call& c, const std::string& end) {
    if (!history_worthy(c)) return;
    const int64_t now = now_ms();
    Call e = c;
    e.end = end;   // (call_json writes "end")
    std::string j = call_json(e, false, now);
    // + how long it lasted
    j.pop_back();
    j += ",\"dur_ms\":" + std::to_string(now - c.t_start) +
         ",\"t_end\":" + std::to_string(wall_ms()) + ",\"followed\":" + jb(c.ever_followed) + "}";
    g_history.push_back(j);
    while (g_history.size() > HISTORY_MAX) g_history.pop_front();
    // the decoder data log: one record per call, when the control VFO logs
    if (playback::active() || c.ctl_vfo < 0) return;
    auto inst = decimator_manager.getDecimator(c.ctl_vfo);
    auto dd = inst ? inst->getDigital() : nullptr;
    if (!dd || dd->log_mode() == dig::LOG_OFF) return;
    const System* s = sys_of(c.sys);
    std::string f = ",\"sys\":" + js(c.sys) + ",\"target\":" + js(c.target) + ",\"group\":" + jb(c.group);
    if (s && c.group) {
        auto it = s->tgs.find(c.target);
        if (it != s->tgs.end() && !it->second.name.empty()) f += ",\"target_name\":" + js(it->second.name);
    }
    f += ",\"source\":" + js(c.source) + ",\"sources\":[";
    for (size_t i = 0; i < c.sources.size(); i++) f += (i ? "," : "") + js(c.sources[i]);
    f += "],\"ch\":" + js(c.ch.name) + ",\"call_mhz\":" + num_json(c.hz / 1e6, "%.6f") +
         ",\"slot\":" + std::to_string(c.ch.slot) + ",\"enc\":" + jb(c.enc) + ",\"emerg\":" + jb(c.emerg) +
         ",\"followed\":" + jb(c.ever_followed) + ",\"voice_vfo\":" + std::to_string(c.ever_followed ? c.vfo : -1) +
         ",\"dur_s\":" + num_json((now - c.t_start) / 1000.0, "%.1f") + ",\"end\":" + js(end) +
         ",\"why\":" + js(c.ever_followed ? "" : c.why);
    declog::record_call(c.ctl_vfo, dd->log_mode(), inst ? vfo_rf(*inst) : 0, s ? s->plugin : "", f);
}

// ============================================================================
// Voice VFOs
// ============================================================================

void park(Voice& v);
void end_call(uint64_t id, const std::string& why);

// uWS loop: remove a voice VFO (muted first - removing the last VFO heard
// makes another one heard - then through the manager, not REMOVE_DECIMATOR:
// its decimator_removed makes every page reset its audio player)
void remove_voice_at(size_t i) {
    if (i >= g_voices.size()) return;
    Voice v = g_voices[i];
    g_voices.erase(g_voices.begin() + static_cast<std::ptrdiff_t>(i));
    if (v.call) {
        auto it = g_calls.find(v.call);
        if (it != g_calls.end()) it->second.vfo = -1;
    }
    meta_erase(v.vfo, true);
    if (!live(v)) return;
    set_heard(v.vfo, false);
    if (decimator_manager.removeDecimator(v.vfo)) {
        audio_mixer.retain(decimator_manager.audioVfoIds());
        std::cout << "Trunk following: voice VFO D" << v.vfo << " removed" << std::endl;
    }
    broadcast(MessageBuilders::build_decimator_info_message());
}

void remove_all_voices() {
    while (!g_voices.empty()) remove_voice_at(g_voices.size() - 1);
}

// parked: muted, not decimated (enabled off: its decoder gets no samples and
// its plugin process idles), kept for the next grant
void park(Voice& v) {
    if (v.call) v.last_call = v.call;
    v.call = 0;
    v.parked_ms = now_ms();
    if (auto inst = live(v)) {
        set_heard(v.vfo, false);
        inst->enabled = false;
    }
    meta_voice(v);
    g_status_now = true;   // (no decimator_info: a page takes a voice VFO's state from the trunk status' vfos)
}

// A voice VFO for a call: a parked one (the one the call had at its last
// over first - the same VFO again -, then one already running the call's
// decoder: no process start), else a new one while there is room
Voice* free_voice(const std::string& plugin, uint64_t call, std::string* why) {
    Voice* best = nullptr;
    auto score = [&](const Voice& v) { return (v.last_call == call ? 2 : 0) + (v.plugin == plugin ? 1 : 0); };
    for (auto& v : g_voices) {
        if (v.call || !live(v)) continue;
        if (!best || score(v) > score(*best)) best = &v;
    }
    if (best) return best;
    if (static_cast<int>(g_voices.size()) >= g_set.max_vfos) {
        *why = "no_vfo";
        return nullptr;
    }
    if (static_cast<int>(decimator_manager.getDecimatorCount()) >= VFO_LIMIT) {
        *why = "vfo_limit";
        return nullptr;
    }
    const int id = decimator_manager.addDecimator();   // muted, at 0 Hz
    auto inst = id >= 0 ? decimator_manager.getDecimator(id) : nullptr;
    if (!inst) {
        *why = "vfo_limit";
        return nullptr;
    }
    Voice v;
    v.vfo = id;
    v.inst = inst;
    v.parked_ms = now_ms();
    g_voices.push_back(v);
    std::cout << "Trunk following: voice VFO D" << id << " made" << std::endl;
    return &g_voices.back();
}

// Set the voice VFO up for call c at p and hear it: options first (the
// decoder gets them before, or with, its first samples on the channel), then
// the decoder, demod, width, squelch, the frequency
void apply_voice(Voice& v, Call& c, const Place& p) {
    auto inst = live(v);
    if (!inst) return;
    const int64_t now = now_ms();
    const std::string sid = std::to_string(v.vfo);
    const bool analog = c.demod == "nfm";
    dig::PluginInfo pi;
    plugin_info(c.decoder, &pi);
    // the control VFO's Logging / Plot on map / Positions every / Show all messages / Transcribe
    dig::Options o;
    bool transcribe = false;
    if (auto ci = c.ctl_vfo >= 0 ? decimator_manager.getDecimator(c.ctl_vfo) : nullptr) {
        if (auto cd = ci->getDigital()) o = cd->options();
        transcribe = ci->transcribe.load();
    }
    o.plugin.clear();
    for (const auto& kv : c.options) o.plugin[c.decoder + "." + kv.first] = kv.second;
    std::vector<std::string> cmds;
    decimator_manager.setDigitalOptions(v.vfo, o);
    const std::string mode = "PLUGIN:" + c.decoder;
    const bool same_plugin = inst->digital_mode.load() == static_cast<int>(dig::Mode::PLUGIN) && v.plugin == c.decoder;
    if (!same_plugin || v.dirty) cmds.push_back("DIGITAL_MODE:" + sid + ":" + mode);
    const std::string demod = analog ? "NBFM" : "DIGITAL";
    if (DecimatorManager::demodModeToString(inst->demod_mode.load()) != demod || v.dirty)
        cmds.push_back("SET_DECIMATOR_DEMOD:" + sid + ":" + demod);
    const double need = std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0);
    const int bw = efficient_bandwidth_at_least(static_cast<int>(std::ceil(std::min(need, 2.4e6))));
    if (inst->bandwidth_hz.load() != bw || v.dirty) cmds.push_back("SET_DECIMATOR_BW_HZ:" + sid + ":" + std::to_string(bw));
    cmds.push_back("DEC_SQUELCH_METHOD:" + sid + ":" + (analog ? "FFT" : "DIGITAL"));
    if (analog) {
        char lv[64];
        snprintf(lv, sizeof lv, "DEC_SQUELCH_LEVEL:%d:%.1f", v.vfo, ANALOG_SQ_DB);
        cmds.push_back(lv);
    }
    cmds.push_back("DEC_SQUELCH_ENABLE:" + sid + ":1");
    if (inst->transcribe.load() != transcribe) cmds.push_back("VOICE2TEXT_VFO:" + sid + ":" + (transcribe ? "1" : "0"));
    char f[96];
    const int mode_now = operating_mode_index();
    if (mode_now == 2) snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f:%d", v.vfo, p.off_hz / 1000.0, p.tuner);
    else snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f", v.vfo, p.off_hz / 1000.0);
    cmds.push_back(f);
    // a retune > 100 Hz resets the decoder: that RESET is ours
    v.dec = inst->getDigital().get();
    g_tracks[v.vfo].expect_reset_until = now + RESET_EXPECT_MS;
    // heard first: a VFO heard first moves the audio channel with it (independent)
    set_heard(v.vfo, g_set.audio);
    dispatch(cmds);
    inst->enabled = true;
    v.dec = inst->getDigital().get();
    v.plugin = c.decoder;
    v.demod = demod;
    v.bw = inst->bandwidth_hz.load();
    v.off_hz = inst->frequency_offset_hz.load();
    v.tuner = inst->tuner_channel.load();
    v.mode = mode_now;
    v.centre_hz = tuner_centre(mode_now, mode_now == 0 ? 0 : v.tuner);
    v.rf_hz = c.hz;
    v.t_placed = now;
    v.dirty = false;
    v.reset_seen = false;
    v.call = c.id;
    c.vfo = v.vfo;
    c.t_placed = now;
    c.pending_place = false;
    meta_voice(v);
    broadcast(MessageBuilders::build_decimator_info_message());
    if (c.t_rx_grant > 0) {
        g_lat_place.push_back(now - c.t_rx_grant);
        while (g_lat_place.size() > 50) g_lat_place.pop_front();
    }
    std::cout << "Trunk following: " << c.sys << " " << (c.group ? "TG " : "unit ") << c.target << " on " << c.ch.name
              << " (" << c.hz / 1e6 << " MHz) -> D" << v.vfo << std::endl;
}

// ============================================================================
// Calls
// ============================================================================

int rank(const Call& c) { return c.prio ? 1 : 0; }

// Why call c may not be followed (policy only - frequency, span and VFOs
// come after): "" = it may
std::string policy_why(const System& s, const Call& c) {
    if (!g_set.follow) return "off";
    if (g_paused) return "paused";
    if (c.user_ended) return "stopped";
    if (c.setup_failed) return "no_signal";
    if (c.data) return "data";
    if (c.group) {
        auto it = s.tgs.find(c.target);
        const bool listed = it != s.tgs.end() && it->second.listed;
        if (it != s.tgs.end() && it->second.mode == TG_SKIP) return "skip";
        if (s.listed_only && !listed) return "unlisted";
    }
    if (c.enc && !g_set.enc) return "encrypted";
    if (!c.group && !g_set.unit) return "unit";
    if (!g_hold_sys.empty() && (c.sys != g_hold_sys || !c.group || c.target != g_hold_tg)) return "hold";
    // the same talkgroup / unit followed through another site of the system
    // already (one conversation, two carriers): not twice - unless that one
    // receives nothing (SETUP gave up: it is unfollowed, this one takes over).
    // Two followed at once (TRUNK_ALL_SITES switched off): the older stays
    if (!g_set.all_sites)
        for (const auto& kv : g_calls) {
            const Call& o = kv.second;
            if (o.id == c.id || o.vfo < 0 || o.sys != c.sys || o.group != c.group || o.target != c.target ||
                o.ctl_vfo == c.ctl_vfo)
                continue;
            if (c.vfo < 0 || o.t_placed < c.t_placed) return "duplicate";
        }
    dig::PluginInfo pi;
    if (!plugin_info(c.decoder, &pi) || !pi.built || pi.fixed_freq_hz > 0 || pi.lock_vfo) return "no_decoder";
    return "";
}

void set_why(Call& c, System& s, const std::string& why) {
    c.why = why;
    if (!c.counted && !why.empty()) {
        c.counted = true;
        s.notf[why]++;
        if (why == "duplicate") {
            auto it = s.ctl.find(c.ctl_vfo);
            if (it != s.ctl.end()) it->second.duplicate++;
        }
    }
}

// call c got a voice VFO (counted once per over)
void note_followed(System& s, Call& c) {
    if (c.ever_followed) return;
    c.ever_followed = true;
    s.followed++;
    auto it = s.ctl.find(c.ctl_vfo);
    if (it != s.ctl.end()) it->second.followed++;
}

void set_state(Call& c, int st) {
    if (c.st == st) return;
    c.st = st;
    c.t_state = now_ms();
    if (Voice* v = voice_of_call(c)) meta_voice(*v);
    g_status_now = true;
}

// the call loses its voice VFO (parked) and waits (why)
void unfollow(Call& c, const std::string& why) {
    if (Voice* v = voice_of_call(c)) park(*v);
    c.vfo = -1;
    c.pending_place = false;
    set_state(c, WAIT);
    if (System* s = sys_of(c.sys)) set_why(c, *s, why);
    else c.why = why;
}

// a followed call received nothing (SETUP gave up, or it was released while
// still in SETUP after SETUP_GIVEUP_MS): counted - a wrong channel table shows
// up here -, and a learned entry that leads nowhere twice is dropped
void note_no_signal(System& s, Call& c) {
    const int64_t now = now_ms();
    s.no_signal++;
    std::cout << "Trunk following: " << c.sys << " " << c.target << " on " << c.ch.name << " (" << c.hz / 1e6 << " MHz, "
              << c.fsrc << "): nothing received in " << (now - c.t_placed) / 1000.0 << " s" << std::endl;
    c.setup_failed = true;
    if (c.fsrc != "learned") return;
    auto ci = s.chans.find(c.ch.name);
    if (ci != s.chans.end() && ++ci->second.learn_fail >= 2) {
        std::cout << "Trunk following: " << c.sys << " channel " << c.ch.name << " - the learned "
                  << ci->second.learned_hz / 1e6 << " MHz received nothing twice: dropped" << std::endl;
        ci->second.learned_hz = NAN;
        ci->second.learn_fail = 0;
        g_learn_dropped++;
        s.lists_struct = true;
        mark_dirty(SAVE_LEARN_MS);
    }
}

// the call is gone from the list (RECENT expired, its system forgotten, a playback)
void drop_call(uint64_t id) {
    auto it = g_calls.find(id);
    if (it == g_calls.end()) return;
    if (Voice* v = voice_of_call(it->second)) park(*v);
    auto k = g_call_by_key.find(it->second.key);
    if (k != g_call_by_key.end() && k->second == id) g_call_by_key.erase(k);
    g_calls.erase(it);
    g_status_now = true;
}

// the call (this over of it) is over: its VFO parked, it goes to the history
// and stays listed as RECENT for RECENT_MS - the next over's grant brings the
// same call (row, id, VFO) back
void end_call(uint64_t id, const std::string& why) {
    auto it = g_calls.find(id);
    if (it == g_calls.end() || it->second.st == RECENT) return;
    Call& c = it->second;
    const bool was_followed = c.vfo >= 0;
    if (was_followed && c.st == SETUP && now_ms() - c.t_placed >= SETUP_GIVEUP_MS)
        if (System* sy = sys_of(c.sys)) note_no_signal(*sy, c);   // (released before SETUP gave up)
    if (Voice* v = voice_of_call(c)) park(*v);
    record_history(c, why);
    c.vfo = -1;
    c.pending_place = false;
    c.end = why;
    c.guard = was_followed || why == "duplicate";
    c.t_end = now_ms();
    c.st = RECENT;
    c.t_state = c.t_end;
    g_status_now = true;
    // the followed copy of a conversation ended normally: its copies waiting on
    // other sites ("duplicate") end with it - they would get a VFO only after the
    // transmission ended everywhere. (A copy that received nothing - no_signal -,
    // or lost its control channel / VFO, lets a duplicate take over instead)
    if (was_followed && (why == "released" || why == "ended" || why == "reassigned")) {
        std::vector<uint64_t> dup;
        for (const auto& kv : g_calls) {
            const Call& o = kv.second;
            if (o.id != c.id && o.st == WAIT && o.why == "duplicate" && o.sys == c.sys && o.group == c.group &&
                o.target == c.target && o.ctl_vfo != c.ctl_vfo)
                dup.push_back(o.id);
        }
        const std::string sys = c.sys;
        for (uint64_t d : dup) end_call(d, "duplicate");
    }
}

// the lowest-ranked call holding a VFO that call c (of rank r) may take: the
// oldest of rank < r. 0 = none (equal rank never pre-empts)
uint64_t victim_for(const Call& c) {
    uint64_t v = 0;
    int64_t t = 0;
    for (const auto& kv : g_calls) {
        const Call& o = kv.second;
        if (o.id == c.id || o.vfo < 0 || rank(o) >= rank(c)) continue;
        if (!v || o.t_start < t) {
            v = o.id;
            t = o.t_start;
        }
    }
    return v;
}

// Follow call c now if it may and can be: "" = followed, else why not
std::string try_follow(Call& c) {
    System* s = sys_of(c.sys);
    if (!s) return "no_system";
    std::string why = policy_why(*s, c);
    if (why.empty()) {
        double hz, up;
        std::string src;
        if (!resolve(*s, c.ch, &hz, &up, &src)) {
            why = "no_freq";
        } else {
            // (whatever gave it a frequency - a grant, an announcement, a rule, learning, an entry)
            if (s->unresolved.erase(c.ch.name)) g_status_now = true;
            c.hz = hz;
            c.up_hz = up;
            c.fsrc = src;
            dig::PluginInfo pi;
            plugin_info(c.decoder, &pi);
            const double bw = std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0);
            int prefer = -1;
            if (auto ci = c.ctl_vfo >= 0 ? decimator_manager.getDecimator(c.ctl_vfo) : nullptr) prefer = ci->tuner_channel.load();
            const Place p = place_for(hz, bw, prefer);
            if (!p.ok) {
                why = "out_of_span";
            } else {
                Voice* v = free_voice(c.decoder, c.id, &why);
                if (!v && rank(c) > 0) {
                    if (const uint64_t vic = victim_for(c)) {
                        auto vi = g_calls.find(vic);
                        const int vfo = vi->second.vfo;
                        s->preempted++;
                        std::cout << "Trunk following: " << c.sys << " TG " << c.target << " (priority) takes D" << vfo
                                  << " from " << vi->second.target << std::endl;
                        end_call(vic, "preempted");
                        v = voice_of_vfo(vfo);
                        if (v && v->call) v = nullptr;
                    }
                }
                if (v) {
                    apply_voice(*v, c, p);
                    note_followed(*s, c);
                    c.why.clear();
                    set_state(c, SETUP);
                    c.t_activity = now_ms();
                    return "";
                }
                if (why.empty()) why = "no_vfo";
            }
        }
    }
    if (why == "no_freq") {
        // the user needs to see WHICH channels lack a frequency
        Unresolved& u = s->unresolved[c.ch.name];
        u.num = c.ch.number;
        u.n++;
        u.last = wall_ms();
        if (s->unresolved.size() > MAX_UNRESOLVED) {
            auto drop = s->unresolved.begin();
            for (auto i = s->unresolved.begin(); i != s->unresolved.end(); ++i)
                if (i->second.last < drop->second.last) drop = i;
            s->unresolved.erase(drop);
        }
    }
    if (c.why != why) g_status_now = true;
    set_why(c, *s, why);
    return why;
}

// call c (followed) to its (new) channel: re-placed, or unfollowed when it
// can't be (rate-limited: a flood of moves re-places at most every PLACE_MIN_MS)
void replace_call(Call& c) {
    Voice* v = voice_of_call(c);
    if (!v) return;
    if (now_ms() - v->t_placed < PLACE_MIN_MS) {
        c.pending_place = true;
        return;
    }
    System* s = sys_of(c.sys);
    double hz, up;
    std::string src;
    if (!s || !resolve(*s, c.ch, &hz, &up, &src)) return unfollow(c, "no_freq");
    c.hz = hz;
    c.up_hz = up;
    c.fsrc = src;
    dig::PluginInfo pi;
    plugin_info(c.decoder, &pi);
    const double bw = std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0);
    const Place p = place_for(hz, bw, v->tuner);
    if (!p.ok) return unfollow(c, "out_of_span");
    apply_voice(*v, c, p);
    set_state(c, SETUP);
    c.t_activity = now_ms();
}

// the voice VFO still as we set it? (the user may have retuned / reconfigured it)
bool voice_as_set(const Voice& v, const Call& c) {
    auto inst = live(v);
    if (!inst) return false;
    if (std::fabs(inst->frequency_offset_hz.load() - v.off_hz) > 1.0f) return false;
    if (inst->bandwidth_hz.load() != v.bw) return false;
    if (DecimatorManager::demodModeToString(inst->demod_mode.load()) != v.demod) return false;
    auto dd = inst->getDigital();
    if (!dd || dd->mode() != dig::Mode::PLUGIN || dd->plugin_id() != c.decoder) return false;
    // the grant's options
    const dig::Options o = dd->options();
    for (const auto& kv : c.options) {
        auto it = o.plugin.find(c.decoder + "." + kv.first);
        if (it == o.plugin.end() || it->second != kv.second) return false;
    }
    return true;
}

void count_chan(System& s, const std::string& name) {
    if (Chan* ch = ensure_chan(s, name)) {
        ch->grants++;
        ch->last = wall_ms();
        s.lists_stats = true;
    }
}

// a call (an over) starts: the counters, its talkgroup / unit heard
void count_start(System& s, Call& c) {
    s.grants++;
    if (c.group) {
        if (Tg* g = ensure_tg(s, c.target)) {
            g->calls++;
            g->last = wall_ms();
            s.lists_stats = true;
            mark_dirty(SAVE_STATS_MS);
        }
        auto it = s.tgs.find(c.target);
        c.prio = it != s.tgs.end() && it->second.mode == TG_PRIORITY;
    } else {
        note_unit(s, c.target, true);
    }
}

Call* new_call(System& s, const std::string& key, const PluginEvent& ev, const std::string& decoder, int site) {
    if (g_calls.size() >= MAX_CALLS) {
        // the oldest RECENT one makes room
        uint64_t old = 0;
        int64_t t_old = 0;
        for (const auto& kv : g_calls)
            if (kv.second.st == RECENT && (!old || kv.second.t_end < t_old)) {
                old = kv.first;
                t_old = kv.second.t_end;
            }
        if (!old) {
            g_dropped_calls++;
            return nullptr;
        }
        drop_call(old);
    }
    Call c;
    c.id = g_next_call++;
    c.sys = s.id;
    c.key = key;
    c.target = ev.target;
    c.group = ev.group;
    c.ch = ev.ch;
    c.decoder = decoder;
    c.ctl_vfo = site;
    const int64_t now = now_ms();
    c.t_start = c.t_grant = c.t_state = now;
    c.t0_wall = wall_ms();
    count_start(s, c);
    auto r = g_calls.emplace(c.id, c);
    g_call_by_key[key] = c.id;
    g_status_now = true;
    return &r.first->second;
}

// a RECENT call's next over: the same call again, waiting to be followed
void revive(System& s, Call& c, const PluginEvent& ev) {
    c.st = WAIT;
    c.t_state = c.t_start = now_ms();
    c.t0_wall = wall_ms();
    c.end.clear();
    c.why.clear();
    c.ch = ev.ch;
    c.overs++;
    c.setup_failed = false;
    c.counted = c.ever_followed = false;
    c.enc = c.emerg = false;
    c.sources.clear();
    c.source.clear();
    c.t_call_active = c.t_call_ended = 0;
    count_start(s, c);
    g_status_now = true;
}

bool same_channel(const Channel& a, const Channel& b) {
    if (a.name != b.name) return false;
    return a.slot == b.slot || a.slot < 0 || b.slot < 0;
}

// ============================================================================
// Events
// ============================================================================

void update_sources(Call& c, const std::string& src, System& s) {
    if (src.empty()) return;
    c.source = src;
    if (std::find(c.sources.begin(), c.sources.end(), src) == c.sources.end()) {
        if (c.sources.size() >= MAX_SOURCES) c.sources.erase(c.sources.begin());
        c.sources.push_back(src);
    }
    note_unit(s, src, false);
}

void on_grant(int vfo, Track& t, const std::string& plugin, const PluginEvent& ev, int64_t t_rx) {
    // the system: a voice VFO's grant (late entry, a TDMA traffic channel's
    // grant updates) belongs to its call's system
    std::string sid = t.sys;
    if (Voice* v = voice_of_vfo(vfo)) {
        // (a parked one: its last call's system; none = nothing to file it under)
        auto it = g_calls.find(v->call ? v->call : v->last_call);
        if (it == g_calls.end()) return;
        sid = it->second.sys;
        if (!sys_of(sid)) return;
    }
    if (sid.empty()) {
        sid = plugin + ":@" + std::to_string(vfo);
        t.sys = sid;
    }
    System& s = ensure_system(sid, plugin, temp_id(sid) ? "" : sid.substr(sid.find(':') + 1));
    if (!t.system_said && !voice_of_vfo(vfo) && !t.control) {
        t.control = true;   // grants without a SYSTEM: a control VFO
        meta_control(vfo, t);
    }
    if (t.control && !voice_of_vfo(vfo)) {
        Ctl& cl = s.ctl[vfo];
        cl.last_ms = now_ms();
    }
    s.grant_msgs++;
    // the channel table: every granted channel has an entry (its grant count)
    if (Chan* ch = ensure_chan(s, ev.ch.name)) {
        if (ev.ch.number >= 0 && ch->num != ev.ch.number) {
            ch->num = ev.ch.number;
            s.lists_struct = true;
        }
        if (fin(ev.ch.freq_hz) && (!fin(ch->ann_hz) || std::fabs(ch->ann_hz - ev.ch.freq_hz) > 0.5)) {
            ch->ann_hz = ev.ch.freq_hz;
            ch->ann_up = ev.ch.uplink_hz;
            s.lists_struct = true;
            if (!temp_id(s.id)) mark_dirty(SAVE_LEARN_MS);
        }
        if (fin(ev.ch.freq_hz) && s.unresolved.erase(ev.ch.name)) g_status_now = true;
    }
    const std::string decoder = ev.decoder.empty() ? plugin : ev.decoder;
    // the site = the control VFO that granted it (a voice VFO's grant: its call's site)
    int site = vfo;
    if (Voice* v = voice_of_vfo(vfo)) {
        auto it = g_calls.find(v->call ? v->call : v->last_call);
        if (it != g_calls.end()) site = it->second.ctl_vfo;
    }
    auto cit = s.ctl.find(site);
    Ctl* cl = cit == s.ctl.end() ? nullptr : &cit->second;
    if (cl) cl->grant_msgs++;
    const std::string key = call_key(s.id, site, ev.group, ev.target);
    auto kit = g_call_by_key.find(key);
    Call* c = kit == g_call_by_key.end() ? nullptr : &g_calls[kit->second];

    // data grants and talkgroups set to skip: counters only - no call rows, no
    // history (a busy system's data channel would fill both). Counted once per
    // grant sequence (repeats within WAIT_EXPIRE_MS are the same one)
    bool skip_tg = false;
    if (ev.group) {
        auto ti = s.tgs.find(ev.target);
        skip_tg = ti != s.tgs.end() && ti->second.mode == TG_SKIP;
    }
    if ((ev.data || skip_tg) && (!c || c->vfo < 0)) {
        if (c) drop_call(c->id);
        const std::string qk = key + "\x1f" + ev.ch.name + (ev.data ? "\x1fd" : "\x1fs");
        const int64_t now = now_ms();
        auto q = s.quiet.find(qk);
        const bool fresh = q == s.quiet.end() || now - q->second > WAIT_EXPIRE_MS;
        s.quiet[qk] = now;
        if (s.quiet.size() > 4096)
            for (auto i = s.quiet.begin(); i != s.quiet.end();) i = now - i->second > WAIT_EXPIRE_MS ? s.quiet.erase(i) : std::next(i);
        if (fresh) {
            s.notf[ev.data ? "data" : "skip"]++;
            if (cl) (ev.data ? cl->data : cl->skip)++;
            if (Chan* ch = ensure_chan(s, ev.ch.name)) {
                if (ev.data) ch->data++;
                ch->last = wall_ms();
                s.lists_stats = true;
            }
            if (ev.group && !ev.data) {
                if (Tg* g = ensure_tg(s, ev.target)) {
                    g->calls++;
                    g->last = wall_ms();
                }
            }
            g_status_now = true;
        }
        return;
    }

    // the channel given to another target on this site: the call that had it
    // is over there - its VFO goes to this one (no retune) when this one may be followed
    int handover = -1;
    for (auto it = g_calls.begin(); it != g_calls.end();) {
        Call& o = it->second;
        ++it;
        if (c && o.id == c->id) continue;
        if (o.sys != s.id || o.ctl_vfo != site || o.st == RECENT || !same_channel(o.ch, ev.ch)) continue;
        if (o.vfo >= 0 && handover < 0 && (!c || c->vfo < 0)) handover = o.vfo;
        end_call(o.id, "reassigned");
    }
    bool moved = false, started = false;
    if (!c) {
        c = new_call(s, key, ev, decoder, site);
        if (!c) return;
        started = true;
    } else if (c->st == RECENT) {
        // the next over of the conversation: the same call (the same row) again -
        // not the repeats of the over just ended (updates within REVIVE_UPDATE_MS)
        if (ev.update && c->guard && now_ms() - c->t_end < REVIVE_UPDATE_MS && c->ch.name == ev.ch.name &&
            c->ch.slot == ev.ch.slot)
            return;
        revive(s, *c, ev);
        started = true;
    } else if (c->ch.name != ev.ch.name || c->ch.slot != ev.ch.slot) {
        // the next transmission of the conversation on another channel: the VFO moves
        if (c->ch.name != ev.ch.name) {
            c->moves++;
            s.moves++;
            count_chan(s, ev.ch.name);
        }
        c->ch = ev.ch;
        c->setup_failed = false;
        moved = true;
    } else {
        c->ch.number = ev.ch.number;
        c->ch.freq_hz = ev.ch.freq_hz;
        c->ch.uplink_hz = ev.ch.uplink_hz;
    }
    if (started) {
        count_chan(s, ev.ch.name);
        if (cl) cl->grants++;
    }
    c->t_grant = now_ms();
    c->t_rx_grant = t_rx;
    update_sources(*c, ev.source, s);
    if (ev.encrypted) c->enc = true;
    if (ev.emergency) c->emerg = true;
    c->data = ev.data;
    c->demod = ev.demod;
    c->bw = ev.bw_hz;
    const bool opts_changed = c->options != ev.options || c->decoder != decoder;
    c->options = ev.options;
    c->decoder = decoder;
    if (c->group) {
        auto it = s.tgs.find(c->target);
        c->prio = it != s.tgs.end() && it->second.mode == TG_PRIORITY;
    }
    if (moved && c->vfo >= 0) {
        const std::string why = policy_why(s, *c);
        if (!why.empty()) return unfollow(*c, why);
        if (Voice* v = voice_of_call(*c)) v->dirty = true;
        replace_call(*c);
        return;
    }
    if (handover >= 0 && c->vfo < 0) {
        // the VFO that was on this channel, for the new target
        Voice* v = voice_of_vfo(handover);
        if (v && !v->call && policy_why(s, *c).empty()) {
            double hz, up;
            std::string src;
            if (resolve(s, c->ch, &hz, &up, &src)) {
                c->hz = hz;
                c->up_hz = up;
                c->fsrc = src;
                dig::PluginInfo pi;
                plugin_info(c->decoder, &pi);
                const Place p = place_for(hz, std::max(c->bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0), v->tuner);
                if (p.ok) {
                    apply_voice(*v, *c, p);
                    note_followed(s, *c);
                    c->why.clear();
                    set_state(*c, SETUP);
                    c->t_activity = now_ms();
                }
            }
        }
    }
    if (c->vfo >= 0) {
        // followed: still allowed (a filter / hold may have changed), still as we set it
        const std::string why = policy_why(s, *c);
        if (!why.empty()) return unfollow(*c, why);
        Voice* v = voice_of_call(*c);
        if (v && (opts_changed || v->dirty || !voice_as_set(*v, *c))) {
            v->dirty = true;
            replace_call(*c);
        }
        return;
    }
    // not followed (yet): what keeps it from it (passive learning waits for a no_freq one's CALL)
    try_follow(*c);
}

// GRANT_END: the site's calls on that channel are over
void on_grant_end(const std::string& sys, int site, const PluginEvent& ev) {
    if (sys.empty()) return;
    std::vector<uint64_t> done;
    for (const auto& kv : g_calls) {
        const Call& c = kv.second;
        if (c.sys == sys && c.ctl_vfo == site && c.st != RECENT && same_channel(c.ch, ev.ch)) done.push_back(kv.first);
    }
    for (uint64_t id : done) end_call(id, "released");
}

void on_channel(int vfo, Track& t, const std::string& plugin, const PluginEvent& ev) {
    if (t.sys.empty()) t.sys = plugin + ":@" + std::to_string(vfo);
    System& s = ensure_system(t.sys, plugin, temp_id(t.sys) ? "" : t.sys.substr(t.sys.find(':') + 1));
    Chan* ch = ensure_chan(s, ev.ch.name);
    if (!ch) return;
    bool chg = false;
    if (ev.ch.number >= 0 && ch->num != ev.ch.number) { ch->num = ev.ch.number; chg = true; }
    if (fin(ev.ch.freq_hz) && (!fin(ch->ann_hz) || std::fabs(ch->ann_hz - ev.ch.freq_hz) > 0.5)) {
        ch->ann_hz = ev.ch.freq_hz;
        chg = true;
    }
    if (fin(ev.ch.uplink_hz) && (!fin(ch->ann_up) || std::fabs(ch->ann_up - ev.ch.uplink_hz) > 0.5)) {
        ch->ann_up = ev.ch.uplink_hz;
        chg = true;
    }
    if (chg) {
        s.lists_struct = true;
        if (!temp_id(s.id)) mark_dirty(SAVE_LEARN_MS);
        s.unresolved.erase(ev.ch.name);
    }
}

void on_system(int vfo, Track& t, const std::string& plugin, const PluginEvent& ev) {
    if (voice_of_vfo(vfo)) return;   // a voice VFO's decoder names the system too: not a control VFO
    const std::string sid = ev.sys_key.empty() ? (t.sys.empty() || !temp_id(t.sys) ? plugin + ":@" + std::to_string(vfo) : t.sys)
                                               : plugin + ":" + ev.sys_key;
    if (!t.sys.empty() && t.sys != sid) {
        if (temp_id(t.sys) && !temp_id(sid)) {
            merge_temp(t.sys, sid, plugin, ev.sys_key);
        } else if (System* old = sys_of(t.sys)) {
            old->ctl.erase(vfo);   // the VFO hears another system now
        }
    }
    t.sys = sid;
    t.system_said = true;
    t.control = ev.control;
    System& s = ensure_system(sid, plugin, ev.sys_key);
    if (s.name != ev.sys_name || s.site != ev.sys_site) {
        s.name = ev.sys_name;
        s.site = ev.sys_site;
        g_status_now = true;
    }
    if (ev.control) {
        Ctl& cl = s.ctl[vfo];
        cl.last_ms = now_ms();
        cl.site = ev.sys_site;
    } else {
        s.ctl.erase(vfo);
    }
    meta_control(vfo, t);
}

// CONTROL: the control channel is now there - move the control VFO when the
// frequency is resolved and placeable (rate-limited), else show why
void on_control(int vfo, Track& t, const PluginEvent& ev) {
    System* s = sys_of(t.sys);
    if (!s) return;
    double hz, up;
    std::string src;
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst) return;
    if (!resolve(*s, ev.ch, &hz, &up, &src)) {
        t.control_note = "control channel " + ev.ch.name + ": no frequency";
        g_status_now = true;
        return;
    }
    const double now_rf = vfo_rf(*inst);
    if (std::fabs(now_rf - hz) < 100) {
        t.control_note.clear();
        return;   // there already
    }
    const int64_t now = now_ms();
    if (playback::active()) {
        t.control_note = "control channel " + ev.ch.name + " (" + num_json(hz / 1e6, "%.6f") + " MHz): not moved during a playback";
        g_status_now = true;
        return;
    }
    if (t.moved_ms && now - t.moved_ms < CONTROL_MOVE_MIN_MS) {
        t.control_note = "control channel " + ev.ch.name + " (" + num_json(hz / 1e6, "%.6f") + " MHz): moved less than " +
                         std::to_string(CONTROL_MOVE_MIN_MS / 1000) + " s ago - waiting";
        g_status_now = true;
        return;
    }
    const Place p = place_for(hz, inst->bandwidth_hz.load(), inst->tuner_channel.load());
    if (!p.ok) {
        t.control_note = "control channel " + ev.ch.name + " (" + num_json(hz / 1e6, "%.6f") + " MHz): outside the receiver's span";
        g_status_now = true;
        return;
    }
    char f[96];
    if (operating_mode_index() == 2) snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f:%d", vfo, p.off_hz / 1000.0, p.tuner);
    else snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f", vfo, p.off_hz / 1000.0);
    t.moved_ms = now;
    t.expect_reset_until = now + RESET_EXPECT_MS;
    t.control_note.clear();
    std::cout << "Trunk following: " << t.sys << " control channel moved to " << ev.ch.name << " (" << hz / 1e6
              << " MHz) - D" << vfo << " follows" << std::endl;
    // the user's own VFO: saved where the control channel is now (not dispatching)
    const std::string why = ControlHandler::apply_command(f);
    if (!why.empty()) {
        t.control_note = "control channel " + ev.ch.name + ": " + why;
        std::cerr << "Trunk following: '" << f << "' refused (" << why << ")" << std::endl;
    }
    g_status_now = true;
}

// --- passive learning ----------------------------------------------------
// A grant whose channel has no frequency waits for a CALL with the same
// target (and source, when both give one) from ANOTHER VFO of the same
// plugin within LEARN_WINDOW_MS: that VFO's frequency is the channel's. Two
// such observations from separate grants (>= LEARN_SEPARATE_MS apart, the
// same frequency within LEARN_TOL_HZ, one VFO reporting) make it a learned
// entry. A learned entry whose calls receive nothing twice (SETUP gives up)
// is dropped. Never over a user entry, never a frequency another channel of
// the system already has.
void on_learn_call(int vfo, const Track& t, const std::string& plugin, const PluginEvent& ev) {
    if (ev.call_state != 1 || ev.target.empty()) return;
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst) return;
    const double rf = vfo_rf(*inst);
    if (rf <= 0) return;
    const int64_t now = now_ms();
    // the unresolved call(s) this could be
    std::vector<Call*> match;
    for (auto& kv : g_calls) {
        Call& c = kv.second;
        if (c.vfo >= 0 || c.st == RECENT || c.why != "no_freq" || c.decoder != plugin || c.target != ev.target || c.group != ev.group) continue;
        if (!t.sys.empty() && t.sys != c.sys) continue;   // a VFO that named another system
        if (now - c.t_start > LEARN_WINDOW_MS && now - c.t_grant > LEARN_WINDOW_MS) continue;
        if (!c.source.empty() && !ev.source.empty() && c.source != ev.source) {
            g_learn_refused++;
            continue;
        }
        if (c.ctl_vfo == vfo) continue;
        match.push_back(&c);
    }
    if (match.size() != 1) {
        if (match.size() > 1) g_learn_refused++;
        return;
    }
    Call& c = *match.front();
    System* s = sys_of(c.sys);
    if (!s) return;
    Chan* ch = ensure_chan(*s, c.ch.name);
    if (!ch || fin(ch->user_hz)) {
        g_learn_refused++;
        return;
    }
    // another channel of the system already has this frequency: not this one
    for (const auto& kv : s->chans) {
        if (kv.first == c.ch.name) continue;
        const double f = fin(kv.second.user_hz) ? kv.second.user_hz : fin(kv.second.ann_hz) ? kv.second.ann_hz : kv.second.learned_hz;
        if (fin(f) && std::fabs(f - rf) < LEARN_TOL_HZ) {
            g_learn_refused++;
            return;
        }
    }
    // a second VFO with the same target at the same time: ambiguous
    const std::string lk = c.sys + "\x1f" + c.ch.name;
    LearnObs& o = g_learn[lk];
    if (o.call == c.id && o.over == c.overs) {
        if (std::fabs(o.hz - rf) > LEARN_TOL_HZ) {
            // two VFOs with this call at once: nothing learned from it
            o = LearnObs{};
            g_learn_refused++;
        }
        return;   // (one observation per grant)
    }
    if (fin(o.hz) && std::fabs(o.hz - rf) < LEARN_TOL_HZ && now - o.t >= LEARN_SEPARATE_MS) {
        o.n++;
    } else {
        o.hz = rf;
        o.n = 1;
    }
    o.call = c.id;
    o.over = c.overs;
    o.t = now;
    if (o.n < 2) {
        std::cout << "Trunk following: " << c.sys << " channel " << c.ch.name << " seen on D" << vfo << " ("
                  << rf / 1e6 << " MHz) - learned once seen again" << std::endl;
        return;
    }
    ch->learned_hz = std::round(rf);
    ch->learn_fail = 0;
    g_learned_n++;
    s->lists_struct = true;
    s->unresolved.erase(c.ch.name);
    if (!temp_id(s->id)) mark_dirty(SAVE_LEARN_MS);
    std::cout << "Trunk following: " << c.sys << " channel " << c.ch.name << " learned: " << rf / 1e6 << " MHz (D"
              << vfo << ")" << std::endl;
    g_learn.erase(lk);
    try_follow(c);
}

// a CALL from a voice VFO: what its channel carries
void on_voice_call(Voice& v, const PluginEvent& ev) {
    auto it = g_calls.find(v.call);
    if (it == g_calls.end()) return;
    Call& c = it->second;
    System* s = sys_of(c.sys);
    if (!s) return;
    const int64_t now = now_ms();
    // its tuner retuned / the mode changed since it was placed: what it hears isn't the call's
    {
        const int mode = operating_mode_index();
        if (mode != v.mode || std::fabs(tuner_centre(mode, mode == 0 ? 0 : v.tuner) - v.centre_hz) > 1000) return;
    }
    // another timeslot's call on the same carrier (DMR, P25 Phase 2, TETRA): not this one's
    if (ev.call_slot >= 0 && c.ch.slot >= 0 && ev.call_slot != c.ch.slot) return;
    // (an empty target = not known yet: the grant's stays)
    if (!ev.target.empty() && (ev.target != c.target || ev.group != c.group)) {
        // (only an ACTIVE one says what the channel carries now: an ENDED / RELEASED of another
        // target is the previous call's end, late)
        if (ev.call_state != 1) return;
        // the channel carries a different call: re-labelled when that one may
        // be followed, else the VFO is freed
        // (the previous channel's, still in the pipe: not before its decoder's RESET for this placement - or 2 s)
        if (now - c.t_placed < STALE_FRAMES_MS * 3 || (!v.reset_seen && now - c.t_placed < 2000)) return;
        const std::string key = call_key(c.sys, c.ctl_vfo, ev.group, ev.target);
        auto kit = g_call_by_key.find(key);
        Call probe = c;
        probe.target = ev.target;
        probe.group = ev.group;
        probe.enc = ev.encrypted;
        probe.user_ended = probe.setup_failed = false;
        if (kit != g_call_by_key.end()) {
            const Call& o = g_calls[kit->second];
            probe.user_ended = o.user_ended;
            probe.setup_failed = o.setup_failed;
        }
        const std::string why = policy_why(*s, probe);
        std::cout << "Trunk following: D" << v.vfo << " carries " << (ev.group ? "TG " : "unit ") << ev.target << ", not "
                  << c.target << (why.empty() ? " - re-labelled" : " - freed (" + why + ")") << std::endl;
        if (!why.empty()) return end_call(c.id, "wrong_call");
        const int vfo = v.vfo;
        if (kit != g_call_by_key.end()) {
            Call& o = g_calls[kit->second];
            if (o.vfo >= 0) return end_call(c.id, "wrong_call");   // followed elsewhere already
            // the waiting call takes this VFO
            Channel ch = c.ch;
            double hz = c.hz, up = c.up_hz;
            std::string fsrc = c.fsrc;
            const uint64_t oid = o.id;
            v.call = 0;   // (keeps it from being parked)
            c.vfo = -1;
            end_call(c.id, "relabelled");
            Call& n = g_calls[oid];
            Voice* vv = voice_of_vfo(vfo);
            if (!vv) return;
            n.ch = ch;
            n.hz = hz;
            n.up_hz = up;
            n.fsrc = fsrc;
            n.vfo = vfo;
            vv->call = n.id;
            if (n.st == RECENT) revive(*s, n, ev);
            n.ch = ch;
            note_followed(*s, n);
            n.why.clear();
            n.t_placed = vv->t_placed;
            n.t_call_active = now;
            n.t_activity = now;
            set_state(n, ACTIVE);
            meta_voice(*vv);
            return;
        }
        // re-keyed
        g_call_by_key.erase(c.key);
        c.target = ev.target;
        c.group = ev.group;
        c.key = key;
        c.sources.clear();
        c.source.clear();
        g_call_by_key[key] = c.id;
        if (c.group) {
            if (Tg* g = ensure_tg(*s, c.target)) {
                g->calls++;
                g->last = wall_ms();
            }
            auto ti = s->tgs.find(c.target);
            c.prio = ti != s->tgs.end() && ti->second.mode == TG_PRIORITY;
        }
        meta_voice(v);
        g_status_now = true;
    }
    update_sources(c, ev.source, *s);
    if (ev.encrypted && !c.enc) {
        c.enc = true;
        if (!g_set.enc) return unfollow(c, "encrypted");
    }
    if (ev.emergency) c.emerg = true;
    if (ev.call_state == 2) return end_call(c.id, "released");
    if (ev.call_state == 1) {
        c.t_call_active = now;
    } else {
        c.t_call_ended = now;
    }
}

void on_reset(int vfo, Track& t) {
    const int64_t now = now_ms();
    if (Voice* v = voice_of_vfo(vfo)) {
        // our own retune of it: expected; else the user changed it - taken back at the next grant
        if (now > t.expect_reset_until) v->dirty = true;
        else v->reset_seen = true;   // (its decoder dropped the previous channel: what it says now is this one's)
        return;
    }
    if (now <= t.expect_reset_until) return;   // our CONTROL move
    // the control VFO's state is stale (retuned, decoder changed, plugin restarted): its calls are freed
    if (System* s = sys_of(t.sys)) s->ctl.erase(vfo);
    std::vector<uint64_t> done;
    for (const auto& kv : g_calls)
        if (kv.second.ctl_vfo == vfo) done.push_back(kv.first);
    for (uint64_t id : done) end_call(id, "control_reset");
    t.sys.clear();
    t.control = t.system_said = false;
    meta_erase(vfo, false);
    g_status_now = true;
}

void note_heard(const std::string& sys) {
    if (System* s = sys_of(sys); s && !s->heard) {
        s->heard = true;
        if (!temp_id(s->id)) mark_dirty(SAVE_LEARN_MS);
        g_status_now = true;
    }
}

void apply_event(QEv& q) {
    // the VFO as it is now: an event of a decoder that is gone (removed VFO, id reused) is dropped
    auto inst = decimator_manager.getDecimator(q.vfo);
    if (!inst) return;
    auto dd = inst->getDigital();
    if (!dd || dd.get() != q.dec) return;
    Track& t = g_tracks[q.vfo];
    if (t.dec != q.dec) {
        // a new decoder on this id: its earlier state is not this one's
        if (System* s = sys_of(t.sys)) s->ctl.erase(q.vfo);
        const int64_t keep = t.expect_reset_until;
        t = Track{};
        t.expect_reset_until = keep;
        t.dec = q.dec;
    }
    t.plugin = q.plugin;
    t.last_ms = now_ms();
    if (System* s = sys_of(t.sys)) {
        auto it = s->ctl.find(q.vfo);
        if (it != s->ctl.end()) it->second.last_ms = t.last_ms;
    }
    const PluginEvent& ev = q.ev;
    switch (ev.kind) {
        case PluginEvent::SYSTEM: on_system(q.vfo, t, q.plugin, ev); break;
        case PluginEvent::CHANNEL:
            if (!voice_of_vfo(q.vfo)) on_channel(q.vfo, t, q.plugin, ev);
            break;
        case PluginEvent::CONTROL:
            if (!voice_of_vfo(q.vfo)) on_control(q.vfo, t, ev);
            break;
        case PluginEvent::GRANT: on_grant(q.vfo, t, q.plugin, ev, q.t_rx); break;
        case PluginEvent::GRANT_END: {
            if (Voice* v = voice_of_vfo(q.vfo)) {
                auto it = g_calls.find(v->call);
                if (it != g_calls.end()) on_grant_end(it->second.sys, it->second.ctl_vfo, ev);
            } else {
                on_grant_end(t.sys, q.vfo, ev);
            }
            break;
        }
        case PluginEvent::CALL:
            if (Voice* v = voice_of_vfo(q.vfo); v && v->call) on_voice_call(*v, ev);
            else if (!voice_of_vfo(q.vfo)) on_learn_call(q.vfo, t, q.plugin, ev);
            break;
        case PluginEvent::RESET: on_reset(q.vfo, t); break;
    }
    if (ev.kind != PluginEvent::RESET) note_heard(g_tracks[q.vfo].sys);
}

void drain() {
    std::deque<QEv> q;
    {
        std::lock_guard<std::mutex> lk(g_qmu);
        q.swap(g_queue);
        g_drain_posted = false;
    }
    const int64_t now = now_ms();
    for (auto& e : q) {
        if (now < g_fence_until) continue;
        try {
            apply_event(e);
        } catch (const std::exception& ex) {
            std::cerr << "Trunk following: event dropped (" << ex.what() << ")" << std::endl;
        }
    }
}

// ============================================================================
// The systems file
// ============================================================================

std::string serialise() {
    std::ostringstream o;
    o << "{\"version\":1,\"systems\":[";
    bool first = true;
    for (const auto& kv : g_sys) {
        const System& s = kv.second;
        if (temp_id(s.id)) continue;
        o << (first ? "\n " : ",\n ") << "{\"id\":" << js(s.id) << ",\"plugin\":" << js(s.plugin) << ",\"key\":" << js(s.key)
          << ",\"name\":" << js(s.user_name) << ",\"reported_name\":" << js(s.name) << ",\"listed_only\":" << jb(s.listed_only)
          << ",\"heard\":" << jb(s.heard)
          << ",\"tgs\":[";
        first = false;
        bool f1 = true;
        for (const auto& t : s.tgs) {
            o << (f1 ? "" : ",") << "{\"id\":" << js(t.first) << ",\"name\":" << js(t.second.name) << ",\"mode\":\""
              << tg_mode_name(t.second.mode) << "\",\"listed\":" << jb(t.second.listed) << ",\"last\":" << t.second.last
              << ",\"calls\":" << t.second.calls << "}";
            f1 = false;
        }
        o << "],\"chans\":[";
        f1 = true;
        for (const auto& c : s.chans) {
            o << (f1 ? "" : ",") << "{\"ch\":" << js(c.first) << ",\"num\":" << c.second.num
              << ",\"user_hz\":" << num_json(c.second.user_hz) << ",\"user_up\":" << num_json(c.second.user_up)
              << ",\"ann_hz\":" << num_json(c.second.ann_hz) << ",\"ann_up\":" << num_json(c.second.ann_up)
              << ",\"learned_hz\":" << num_json(c.second.learned_hz) << ",\"grants\":" << c.second.grants
              << ",\"last\":" << c.second.last << "}";
            f1 = false;
        }
        o << "],\"rules\":[";
        for (size_t i = 0; i < s.rules.size(); i++) {
            const Rule& r = s.rules[i];
            o << (i ? "," : "") << "{\"first\":" << r.first << ",\"last\":" << r.last << ",\"base_hz\":" << num_json(r.base_hz)
              << ",\"step_hz\":" << num_json(r.step_hz, "%.3f") << ",\"up_off_hz\":" << num_json(r.up_off_hz) << "}";
        }
        o << "]}";
    }
    o << "\n]}\n";
    return o.str();
}

bool write_file(const std::string& text) {
    const std::string tmp = std::string(SYSTEMS_FILE) + ".tmp";
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (f) {
        f << text;
        f.close();
    }
    if (!f || fs_replace(tmp.c_str(), SYSTEMS_FILE) != 0) {
        std::cerr << "Trunk following: can't write " << SYSTEMS_FILE << " - changes are lost at a restart" << std::endl;
        return false;
    }
    return true;
}

// the systems file now (never during a playback: its systems aren't the user's)
void save() {
    if (playback::active() || !g_loaded.load()) return;
    const std::string text = serialise();
    {
        std::lock_guard<std::mutex> lk(g_save_mu);
        g_unsaved.clear();
    }
    write_file(text);
    g_dirty_since = 0;
}

double jnum(const geo::Json& j, const char* k, double def = NAN) {
    const geo::Json* v = j.get(k);
    return v && v->type == geo::Json::NUM && fin(v->n) ? v->n : def;
}
bool jbool(const geo::Json& j, const char* k, bool def = false) {
    const geo::Json* v = j.get(k);
    return v && v->type == geo::Json::BOOL ? v->b : def;
}
bool hz_ok(double hz) { return fin(hz) && hz > 0 && hz <= 6e9; }
bool valid_key(const std::string& k) {
    if (k.size() > 48) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

// ============================================================================
// Status
// ============================================================================

std::string call_json(const Call& c, bool ages, int64_t now) {
    const System* s = sys_of(c.sys);
    std::string tname, site;
    if (s) {
        auto it = s->ctl.find(c.ctl_vfo);
        if (it != s->ctl.end()) site = it->second.site;
    }
    if (s && c.group) {
        auto it = s->tgs.find(c.target);
        if (it != s->tgs.end()) tname = it->second.name;
    }
    std::ostringstream o;
    o << "{\"id\":" << c.id << ",\"sys\":" << js(c.sys) << ",\"target\":" << js(c.target) << ",\"target_name\":" << js(tname)
      << ",\"group\":" << jb(c.group) << ",\"source\":" << js(c.source) << ",\"sources\":[";
    for (size_t i = 0; i < c.sources.size(); i++) o << (i ? "," : "") << js(c.sources[i]);
    o << "],\"ch\":" << js(c.ch.name) << ",\"num\":" << c.ch.number << ",\"hz\":" << num_json(c.hz)
      << ",\"up_hz\":" << num_json(c.up_hz) << ",\"slot\":" << c.ch.slot << ",\"fsrc\":" << js(c.fsrc)
      << ",\"enc\":" << jb(c.enc) << ",\"emerg\":" << jb(c.emerg) << ",\"data\":" << jb(c.data) << ",\"prio\":" << jb(c.prio)
      << ",\"decoder\":" << js(c.decoder) << ",\"demod\":" << js(c.demod) << ",\"state\":\"" << cstate_name(c.st)
      << "\",\"vfo\":" << c.vfo << ",\"ctl_vfo\":" << c.ctl_vfo << ",\"why\":" << js(c.why) << ",\"moves\":" << c.moves
      << ",\"t0\":" << c.t0_wall << ",\"overs\":" << c.overs << ",\"end\":" << js(c.end) << ",\"site\":" << js(site);
    if (ages) o << ",\"age_ms\":" << (now - c.t_start) << ",\"state_ms\":" << (now - c.t_state) << ",\"grant_ms\":" << (now - c.t_grant);
    o << "}";
    return o.str();
}

int64_t median(const std::deque<int64_t>& d) {
    if (d.empty()) return -1;
    std::vector<int64_t> v(d.begin(), d.end());
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
int64_t maxof(const std::deque<int64_t>& d) {
    int64_t m = -1;
    for (int64_t x : d) m = std::max(m, x);
    return m;
}

std::string build_status(bool ages) {
    const int64_t now = now_ms();
    std::ostringstream o;
    char hang[16];
    snprintf(hang, sizeof hang, "%.1f", g_set.hang_s);
    const int mode = operating_mode_index();
    o << "{\"trunk\":{\"follow\":" << jb(g_set.follow) << ",\"all_sites\":" << jb(g_set.all_sites) << ",\"max_vfos\":" << g_set.max_vfos << ",\"encrypted\":" << jb(g_set.enc)
      << ",\"unit\":" << jb(g_set.unit) << ",\"audio\":" << jb(g_set.audio) << ",\"hang_s\":" << hang
      << ",\"paused\":" << jb(g_paused) << ",\"paused_why\":" << js(g_pause_why) << ",\"playback\":" << jb(playback::active())
      << ",\"mode\":\"" << (mode == 1 ? "wideband" : mode == 2 ? "independent" : "coherent") << "\",\"hold\":";
    if (g_hold_sys.empty()) o << "null";
    else o << "{\"sys\":" << js(g_hold_sys) << ",\"tg\":" << js(g_hold_tg) << "}";
    // control VFOs put back (or not) after a scanner, the last 60 s
    o << ",\"scan_restore\":[";
    {
        bool f0 = true;
        for (const auto& r : g_restore) {
            if (now - r.t > 60000) continue;
            o << (f0 ? "" : ",") << "{\"vfo\":" << r.vfo << ",\"sys\":" << js(r.sys) << ",\"hz\":" << num_json(r.hz)
              << ",\"restored\":" << jb(r.restored) << ",\"note\":" << js(r.note) << "}";
            f0 = false;
        }
    }
    o << "],\"vfos\":[";
    for (size_t i = 0; i < g_voices.size(); i++) {
        const Voice& v = g_voices[i];
        auto it = v.call ? g_calls.find(v.call) : g_calls.end();
        auto inst = live(v);
        o << (i ? "," : "") << "{\"vfo\":" << v.vfo << ",\"call\":" << (it == g_calls.end() ? 0 : v.call) << ",\"state\":\""
          << (it == g_calls.end() ? "parked" : cstate_name(it->second.st)) << "\",\"hz\":" << num_json(v.rf_hz)
          << ",\"plugin\":" << js(v.plugin) << ",\"tuner\":" << v.tuner
          << ",\"heard\":" << jb(inst && inst->audio_out.load()) << ",\"enabled\":" << jb(inst && inst->enabled.load());
        if (it != g_calls.end()) {
            const Call& c = it->second;
            const System* sy = sys_of(c.sys);
            o << ",\"sys\":" << js(c.sys) << ",\"ctl\":" << c.ctl_vfo << ",\"label\":" << js(sy ? label_of(*sy, c) : c.target)
              << ",\"target\":" << js(c.target) << ",\"group\":" << jb(c.group) << ",\"ch\":" << js(c.ch.name)
              << ",\"slot\":" << c.ch.slot;
        }
        o << "}";
    }
    o << "],\"systems\":[";
    bool first = true;
    for (const auto& kv : g_sys) {
        const System& s = kv.second;
        o << (first ? "" : ",") << "{\"id\":" << js(s.id) << ",\"plugin\":" << js(s.plugin) << ",\"key\":" << js(s.key)
          << ",\"name\":" << js(s.name) << ",\"user_name\":" << js(s.user_name) << ",\"site\":" << js(s.site)
          << ",\"saved\":" << jb(!temp_id(s.id)) << ",\"heard\":" << jb(s.heard) << ",\"listed_only\":" << jb(s.listed_only)
          << ",\"control\":[";
        first = false;
        bool f1 = true;
        for (const auto& c : s.ctl) {
            auto inst = decimator_manager.getDecimator(c.first);
            const double hz = inst ? vfo_rf(*inst) : 0;
            auto tit = g_tracks.find(c.first);
            const std::string note = tit != g_tracks.end() ? tit->second.control_note : "";
            o << (f1 ? "" : ",") << "{\"vfo\":" << c.first << ",\"hz\":" << num_json(hz > 0 ? hz : NAN)
              << ",\"rx\":" << jb(now - c.second.last_ms < CONTROL_LOST_MS) << ",\"site\":" << js(c.second.site)
              << ",\"note\":" << js(note) << ",\"counts\":{\"grant_msgs\":" << c.second.grant_msgs
              << ",\"grants\":" << c.second.grants << ",\"followed\":" << c.second.followed
              << ",\"duplicate\":" << c.second.duplicate << ",\"data\":" << c.second.data
              << ",\"skip\":" << c.second.skip << "}";
            if (ages) o << ",\"last_ms\":" << (now - c.second.last_ms);
            o << "}";
            f1 = false;
        }
        o << "],\"counts\":{\"grant_msgs\":" << s.grant_msgs << ",\"grants\":" << s.grants << ",\"followed\":" << s.followed
          << ",\"no_signal\":" << s.no_signal << ",\"moves\":" << s.moves << ",\"preempted\":" << s.preempted << ",\"not\":{";
        f1 = true;
        for (const auto& n : s.notf) {
            o << (f1 ? "" : ",") << js(n.first) << ":" << n.second;
            f1 = false;
        }
        o << "}},\"unresolved\":[";
        // the most granted first, 50 at most
        std::vector<std::pair<std::string, Unresolved>> un(s.unresolved.begin(), s.unresolved.end());
        std::sort(un.begin(), un.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
        for (size_t i = 0; i < un.size() && i < 50; i++)
            o << (i ? "," : "") << "{\"ch\":" << js(un[i].first) << ",\"num\":" << un[i].second.num << ",\"n\":" << un[i].second.n
              << ",\"last\":" << un[i].second.last << "}";
        o << "],\"n_unresolved\":" << s.unresolved.size() << ",\"centre\":";
        // (recomputed every 2 s: the channel table can hold 2000 entries)
        System& sm = const_cast<System&>(s);
        if (now - sm.centre_t >= 2000 || sm.centre_t == 0) {
            sm.centre_ok = suggest_centre(s, &sm.centre);
            sm.centre_t = now;
        }
        const Centre& ce = sm.centre;
        if (sm.centre_ok) {
            o << "{\"hz\":" << num_json(ce.hz) << ",\"weight\":" << num_json(ce.w) << ",\"total\":" << num_json(ce.total)
              << ",\"now_weight\":" << num_json(ce.now_w) << ",\"channels\":" << ce.n << ",\"of\":" << ce.of
              << ",\"controls\":" << ce.ctl << ",\"controls_of\":" << ce.ctl_of << "}";
        } else {
            o << "null";
        }
        o << ",\"n_tgs\":" << s.tgs.size() << ",\"n_chans\":" << s.chans.size() << ",\"n_rules\":" << s.rules.size()
          << ",\"n_units\":" << s.units.size() << "}";
    }
    o << "],\"calls\":[";
    first = true;
    for (const auto& kv : g_calls) {
        o << (first ? "" : ",") << call_json(kv.second, ages, now);
        first = false;
    }
    o << "],\"history\":[";
    for (size_t i = 0; i < g_history.size(); i++) o << (i ? "," : "") << g_history[i];
    o << "],\"learn\":{\"learned\":" << g_learned_n << ",\"refused\":" << g_learn_refused << ",\"dropped\":" << g_learn_dropped
      << "},\"lat\":{\"place_ms\":" << median(g_lat_place) << ",\"place_max_ms\":" << maxof(g_lat_place)
      << ",\"active_ms\":" << median(g_lat_active) << ",\"active_max_ms\":" << maxof(g_lat_active) << ",\"n\":" << g_lat_place.size()
      << "},\"events\":" << g_events_in.load() << ",\"events_dropped\":" << g_events_dropped.load()
      << ",\"calls_dropped\":" << g_dropped_calls;
    if (ages) o << ",\"now\":" << wall_ms();
    o << "}}";
    return o.str();
}

std::string lists_json(const System& s) {
    std::ostringstream o;
    o << "{\"trunk_sys\":{\"id\":" << js(s.id) << ",\"user_name\":" << js(s.user_name) << ",\"name\":" << js(s.name)
      << ",\"listed_only\":" << jb(s.listed_only) << ",\"tgs\":[";
    bool f = true;
    for (const auto& t : s.tgs) {
        o << (f ? "" : ",") << "{\"id\":" << js(t.first) << ",\"name\":" << js(t.second.name) << ",\"mode\":\""
          << tg_mode_name(t.second.mode) << "\",\"listed\":" << jb(t.second.listed) << ",\"last\":" << t.second.last
          << ",\"calls\":" << t.second.calls << "}";
        f = false;
    }
    o << "],\"chans\":[";
    f = true;
    for (const auto& c : s.chans) {
        double hz, up;
        std::string src;
        Channel ch;
        ch.name = c.first;
        ch.number = c.second.num;
        const bool ok = resolve(s, ch, &hz, &up, &src);
        o << (f ? "" : ",") << "{\"ch\":" << js(c.first) << ",\"num\":" << c.second.num << ",\"hz\":" << num_json(ok ? hz : NAN)
          << ",\"up_hz\":" << num_json(ok ? up : NAN) << ",\"src\":" << js(ok ? src : "") << ",\"user_hz\":" << num_json(c.second.user_hz)
          << ",\"user_up\":" << num_json(c.second.user_up) << ",\"ann_hz\":" << num_json(c.second.ann_hz)
          << ",\"ann_up\":" << num_json(c.second.ann_up) << ",\"learned_hz\":" << num_json(c.second.learned_hz)
          << ",\"grants\":" << c.second.grants << ",\"data\":" << c.second.data << ",\"last\":" << c.second.last << "}";
        f = false;
    }
    o << "],\"rules\":[";
    for (size_t i = 0; i < s.rules.size(); i++) {
        const Rule& r = s.rules[i];
        o << (i ? "," : "") << "{\"i\":" << i << ",\"first\":" << r.first << ",\"last\":" << r.last
          << ",\"base_hz\":" << num_json(r.base_hz) << ",\"step_hz\":" << num_json(r.step_hz, "%.3f")
          << ",\"up_off_hz\":" << num_json(r.up_off_hz) << "}";
    }
    o << "],\"units\":[";
    f = true;
    for (const auto& u : s.units) {
        o << (f ? "" : ",") << "{\"id\":" << js(u.first) << ",\"last\":" << u.second.last << ",\"calls\":" << u.second.calls << "}";
        f = false;
    }
    o << "]}}";
    return o.str();
}

void send_status(bool force) {
    const int64_t now = now_ms();
    if (!force && now - g_status_sent < STATUS_MIN_MS) return;
    const std::string body = build_status(false);
    bool active = !g_calls.empty() || !g_voices.empty();
    for (const auto& kv : g_sys)
        if (!kv.second.ctl.empty()) active = true;
    if (!force && body == g_status_last && !(active && now - g_status_sent >= STATUS_ACTIVE_MS)) return;
    g_status_last = body;
    g_status_sent = now;
    g_status_now = false;
    broadcast(build_status(true));
}

void send_err(const std::string& cmd, const std::string& err) {
    broadcast("{\"trunk_error\":{\"cmd\":" + js(cmd.substr(0, 64)) + ",\"error\":" + js(err) + "}}");
}

// ============================================================================
// The state machine
// ============================================================================

// while not paused: where the control VFOs are now
void note_homes() {
    std::map<int, Home> h;
    for (const auto& kv : g_sys)
        for (const auto& c : kv.second.ctl) {
            auto inst = decimator_manager.getDecimator(c.first);
            auto dd = inst ? inst->getDigital() : nullptr;
            if (!inst || !dd) continue;
            const double rf = vfo_rf(*inst);
            if (rf <= 0) continue;
            Home& m = h[c.first];
            m.inst = inst;
            m.rf = rf;
            m.bw = inst->bandwidth_hz.load();
            m.tuner = inst->tuner_channel.load();
            m.mode = dig::mode_string(dd->mode(), dd->plugin_id());
            m.sys = kv.first;
        }
    g_home.swap(h);
}

// The pause ended: the control VFOs the scan moved go back (normal commands:
// the user's VFOs, saved where they were) - from 1 s after the scan stopped
// (a scanner resets the VFOs it used as it stops), retried every 0.5 s for
// RESTORE_MS while the VFO stays as the scan left it and its place isn't in
// the span (the receiver tuned back by hand). Moved meanwhile by the user:
// left as it is
constexpr int64_t RESTORE_MS = 30000;
struct Pending {
    Home home;
    float left_off = 0;
    int left_bw = 0, left_tuner = 0;
    std::string left_mode;
    int64_t from = 0, until = 0;
};
std::map<int, Pending> g_pending;
int64_t g_pending_t = 0;

std::string vfo_mode(const Inst& inst) {
    auto dd = inst.getDigital();
    return dd ? dig::mode_string(dd->mode(), dd->plugin_id()) : "OFF";
}

void note_restore(int vfo, const Home& m, bool ok, const std::string& note) {
    Restore r{vfo, m.sys, note, m.rf, ok, now_ms()};
    std::cout << "Trunk following: control VFO D" << vfo << " " << note << std::endl;
    for (auto it = g_restore.begin(); it != g_restore.end();)
        it = it->vfo == vfo ? g_restore.erase(it) : std::next(it);
    g_restore.push_back(r);
    while (g_restore.size() > 16) g_restore.erase(g_restore.begin());
    g_status_now = true;
}

void restore_homes() {
    const int64_t now = now_ms();
    for (const auto& kv : g_home) {
        const Home& m = kv.second;
        auto inst = decimator_manager.getDecimator(kv.first);
        if (!inst || inst != m.inst.lock()) continue;   // removed meanwhile (an id reused: not ours)
        Pending& pd = g_pending[kv.first];
        pd.home = m;
        pd.left_off = inst->frequency_offset_hz.load();
        pd.left_bw = inst->bandwidth_hz.load();
        pd.left_tuner = inst->tuner_channel.load();
        pd.left_mode = vfo_mode(*inst);
        pd.from = now + 1000;
        pd.until = now + RESTORE_MS;
    }
}

void tick_restore() {
    const int64_t now = now_ms();
    if (g_pending.empty() || g_paused || now - g_pending_t < 500) return;
    g_pending_t = now;
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        const int vfo = it->first;
        Pending& pd = it->second;
        const Home& m = pd.home;
        if (now < pd.from) { ++it; continue; }
        auto inst = decimator_manager.getDecimator(vfo);
        if (!inst || inst != m.inst.lock()) { it = g_pending.erase(it); continue; }
        const std::string mode = vfo_mode(*inst);
        const double rf = vfo_rf(*inst);
        if (std::fabs(rf - m.rf) < 100 && inst->bandwidth_hz.load() == m.bw && mode == m.mode) {
            it = g_pending.erase(it);   // where it was (the scan didn't move it, or it is back)
            continue;
        }
        const bool as_left = std::fabs(inst->frequency_offset_hz.load() - pd.left_off) < 1.0f &&
                             inst->bandwidth_hz.load() == pd.left_bw && mode == pd.left_mode &&
                             inst->tuner_channel.load() == pd.left_tuner;
        if (!as_left) {
            note_restore(vfo, m, false, "moved by the " + g_pause_why_last + ", then set by hand - left as it is");
            it = g_pending.erase(it);
            continue;
        }
        const Place p = place_for(m.rf, m.bw, m.tuner);
        if (!p.ok) {
            if (now > pd.until) {
                note_restore(vfo, m, false, "moved by the " + g_pause_why_last + " - " + num_json(m.rf / 1e6, "%.6f") +
                                                " MHz is outside the receiver's span: tune back (or move the VFO) to follow again");
                it = g_pending.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        std::vector<std::string> cmds;
        const std::string sid = std::to_string(vfo);
        if (inst->bandwidth_hz.load() != m.bw) cmds.push_back("SET_DECIMATOR_BW_HZ:" + sid + ":" + std::to_string(m.bw));
        if (mode != m.mode) cmds.push_back("DIGITAL_MODE:" + sid + ":" + m.mode);
        char f[96];
        if (operating_mode_index() == 2) snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f:%d", vfo, p.off_hz / 1000.0, p.tuner);
        else snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f", vfo, p.off_hz / 1000.0);
        cmds.push_back(f);
        std::string err;
        for (const auto& c : cmds) {
            const std::string why = ControlHandler::apply_command(c);
            if (!why.empty()) err = "'" + c + "' refused: " + why;
        }
        note_restore(vfo, m, err.empty(), err.empty() ? "put back on " + num_json(m.rf / 1e6, "%.6f") + " MHz after the " +
                                                          g_pause_why_last : err);
        it = g_pending.erase(it);
    }
}

void pause_check() {
    std::string why;
    if (sfscan::running()) why = "saved-frequency scan";
    else if (scanner_manager.isRunning()) why = "discrete scanner";
    else if (continuous_scanner.isRunning()) why = "continuous scanner";
    const bool p = !why.empty();
    if (!p && !g_paused) note_homes();
    if (p) g_pause_why_last = why;
    if (p == g_paused && why == g_pause_why) return;
    const bool resumed = g_paused && !p;
    g_paused = p;
    g_pause_why = why;
    g_status_now = true;
    if (p) {
        std::cout << "Trunk following: paused (" << why << " running)" << std::endl;
        for (auto& kv : g_calls)
            if (kv.second.vfo >= 0) unfollow(kv.second, "paused");
        remove_all_voices();
    } else {
        std::cout << "Trunk following: resumed" << std::endl;
        if (resumed && !playback::active()) restore_homes();
    }
    if (p) {
        g_pending.clear();   // (a scan again: its own end decides)
    }
}

void tick_calls() {
    const int64_t now = now_ms();
    const int64_t hang = std::llround(g_set.hang_s * 1000);
    std::vector<std::pair<uint64_t, std::string>> over;
    std::vector<uint64_t> gone;
    for (auto& kv : g_calls) {
        Call& c = kv.second;
        System* s = sys_of(c.sys);
        if (!s) {
            gone.push_back(c.id);
            continue;
        }
        if (c.st == RECENT) {
            if (now - c.t_end > RECENT_MS) gone.push_back(c.id);
            continue;
        }
        if (c.vfo < 0) {
            if (now - c.t_grant > WAIT_EXPIRE_MS) over.push_back({c.id, c.why.empty() ? "expired" : c.why});
            else if (c.why != "off" || g_set.follow) try_follow(c);   // a VFO may be free now, a filter changed
            continue;
        }
        Voice* v = voice_of_call(c);
        auto inst = v ? live(*v) : nullptr;
        if (!v || !inst) {
            // its VFO removed by hand: simply gone - the call isn't rejoined
            std::cout << "Trunk following: D" << c.vfo << " removed by hand - " << c.target << " not followed any more"
                      << std::endl;
            if (v) {
                for (size_t i = 0; i < g_voices.size(); i++)
                    if (&g_voices[i] == v) {
                        meta_erase(v->vfo, true);
                        g_voices.erase(g_voices.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
            }
            c.vfo = -1;
            c.user_ended = true;
            set_state(c, WAIT);
            set_why(c, *s, "stopped");
            continue;
        }
        if (c.pending_place && now - v->t_placed >= PLACE_MIN_MS) {
            replace_call(c);
            if (c.vfo < 0) continue;
        }
        // its tuner retuned (centre / tuner frequency) or the mode changed: the channel isn't there any more
        const int mode = operating_mode_index();
        const double cen = tuner_centre(mode, mode == 0 ? 0 : v->tuner);
        if (mode != v->mode || std::fabs(cen - v->centre_hz) > 1000) {
            over.push_back({c.id, mode != v->mode ? "mode" : "retuned"});
            continue;
        }
        // the user changed it: taken back at the next grant
        if (!v->dirty && !voice_as_set(*v, c)) v->dirty = true;
        auto dd = inst->getDigital();
        const int64_t since = now - c.t_placed - STALE_FRAMES_MS;
        const bool frames = dd && since > 0 && dd->frames_within(std::min<int64_t>(FRAMES_ACTIVE_MS, since));
        const bool called = c.t_call_active > c.t_call_ended && now - c.t_call_active < CALL_ACTIVE_MS &&
                            c.t_call_active > c.t_placed;
        const bool analog = c.demod == "nfm" && inst->squelch_open.load() && now - c.t_placed > 300;
        const bool act = frames || called || analog;
        if (act) c.t_activity = now;
        switch (c.st) {
            case SETUP:
                if (act) {
                    set_state(c, ACTIVE);
                    if (c.t_rx_grant > 0) {
                        g_lat_active.push_back(now - c.t_rx_grant);
                        while (g_lat_active.size() > 50) g_lat_active.pop_front();
                    }
                } else if (now - c.t_placed > SETUP_GIVEUP_MS &&
                           (now - c.t_grant > 1000 || now - c.t_placed > SETUP_MAX_MS)) {
                    // a wrong channel table shows up here
                    note_no_signal(*s, c);
                    unfollow(c, "no_signal");
                }
                break;
            case ACTIVE:
                if (!act || c.t_call_ended > c.t_call_active) {
                    set_state(c, HANG);
                    c.t_hang = now;
                }
                break;
            case HANG:
                if (act && !(c.t_call_ended > c.t_call_active)) {
                    set_state(c, ACTIVE);
                } else if (now - c.t_hang >= hang) {
                    // grant updates keep it through a pause, but not past MAX_SILENT_MS of silence
                    const bool kept = now - c.t_grant < GRANT_FRESH_MS && now - c.t_activity < MAX_SILENT_MS;
                    if (!kept) over.push_back({c.id, "ended"});
                }
                break;
            default: break;
        }
    }
    for (const auto& o : over) end_call(o.first, o.second);
    for (uint64_t id : gone) drop_call(id);
}

void tick_voices() {
    const int64_t now = now_ms();
    // gone (removed by hand), unused too long, more than allowed
    for (size_t i = g_voices.size(); i-- > 0;) {
        Voice& v = g_voices[i];
        if (!live(v)) {
            if (!v.call) {
                meta_erase(v.vfo, true);
                g_voices.erase(g_voices.begin() + static_cast<std::ptrdiff_t>(i));
            }
            continue;   // (a call's: tick_calls says so)
        }
        if (!v.call && (now - v.parked_ms > PARK_KEEP_MS || !g_set.follow)) remove_voice_at(i);
    }
    int over = static_cast<int>(g_voices.size()) - g_set.max_vfos;
    for (size_t i = g_voices.size(); over > 0 && i-- > 0;)
        if (!g_voices[i].call) {
            remove_voice_at(i);
            over--;
        }
    while (over > 0) {
        // the lowest-ranked, newest call's VFO goes
        uint64_t vic = 0;
        for (const auto& kv : g_calls) {
            if (kv.second.vfo < 0) continue;
            if (!vic || rank(kv.second) < rank(g_calls[vic]) ||
                (rank(kv.second) == rank(g_calls[vic]) && kv.second.t_start > g_calls[vic].t_start))
                vic = kv.first;
        }
        if (!vic) break;
        Call& c = g_calls[vic];
        const int vfo = c.vfo;
        unfollow(c, "no_vfo");
        for (size_t i = 0; i < g_voices.size(); i++)
            if (g_voices[i].vfo == vfo) {
                remove_voice_at(i);
                break;
            }
        over--;
    }
}

void tick_tracks() {
    // control VFOs removed
    for (auto it = g_tracks.begin(); it != g_tracks.end();) {
        auto inst = decimator_manager.getDecimator(it->first);
        auto dd = inst ? inst->getDigital() : nullptr;
        if (!dd || dd.get() != it->second.dec) {
            if (voice_of_vfo(it->first)) { ++it; continue; }
            const int vfo = it->first;
            if (System* s = sys_of(it->second.sys)) s->ctl.erase(vfo);
            std::vector<uint64_t> done;
            for (const auto& kv : g_calls)
                if (kv.second.ctl_vfo == vfo) done.push_back(kv.first);
            for (uint64_t id : done) end_call(id, "control_removed");
            meta_erase(vfo, false);
            it = g_tracks.erase(it);
            g_status_now = true;
            continue;
        }
        ++it;
    }
    // control entries of VFOs that report another system now (or nothing): gone
    for (auto& kv : g_sys)
        for (auto c = kv.second.ctl.begin(); c != kv.second.ctl.end();) {
            auto tr = g_tracks.find(c->first);
            if (tr == g_tracks.end() || tr->second.sys != kv.first) {
                c = kv.second.ctl.erase(c);
                g_status_now = true;
            } else {
                ++c;
            }
        }
    // a temporary system (no key yet): folded into the keyed one its VFO reports
    // now; dropped, with its calls, once it has no control VFO left
    std::vector<std::pair<std::string, std::string>> fold;
    std::vector<std::string> drop;
    for (const auto& kv : g_sys) {
        if (!temp_id(kv.first)) continue;
        const System& t = kv.second;
        int vfo = -1;
        try {
            vfo = std::stoi(kv.first.substr(kv.first.find(":@") + 2));
        } catch (const std::exception&) {}
        auto tr = g_tracks.find(vfo);
        if (tr != g_tracks.end() && !tr->second.sys.empty() && !temp_id(tr->second.sys) && sys_of(tr->second.sys) &&
            tr->second.plugin == t.plugin)
            fold.push_back({kv.first, tr->second.sys});
        else if (t.ctl.empty())
            drop.push_back(kv.first);
    }
    for (const auto& f : fold) {
        const System* d = sys_of(f.second);
        if (d) merge_temp(f.first, f.second, d->plugin, d->key);
        g_status_now = true;
    }
    for (const auto& id : drop) {
        std::vector<uint64_t> ids;
        for (const auto& kv : g_calls)
            if (kv.second.sys == id) ids.push_back(kv.first);
        for (uint64_t c : ids) {
            end_call(c, "no_system");
            drop_call(c);
        }
        g_sys.erase(id);
        g_status_now = true;
    }
}

void tick_lists() {
    const int64_t now = now_ms();
    for (auto& kv : g_sys) {
        System& s = kv.second;
        const bool due = (s.lists_struct && now - s.lists_sent >= LISTS_STRUCT_MS) ||
                         (s.lists_stats && now - s.lists_sent >= LISTS_STATS_MS);
        if (!due) continue;
        s.lists_struct = s.lists_stats = false;
        s.lists_sent = now;
        broadcast(lists_json(s));
    }
}

void tick_save() {
    if (!g_dirty_since || playback::active()) return;
    const int64_t now = now_ms();
    if (now - g_dirty_since >= g_dirty_within) {
        save();
    } else {
        // what main()'s last save writes if the process ends first
        std::lock_guard<std::mutex> lk(g_save_mu);
        if (g_unsaved.empty()) g_unsaved = serialise();
    }
}

void release_everything(const std::string& why) {
    std::vector<uint64_t> ids;
    for (const auto& kv : g_calls) ids.push_back(kv.first);
    for (uint64_t id : ids) {
        end_call(id, why);
        drop_call(id);
    }
    remove_all_voices();
}

}  // namespace

// ============================================================================
// Public
// ============================================================================

void on_plugin_event(int vfo, const void* decoder, const std::string& plugin_id, PluginEvent&& ev) {
    {
        std::lock_guard<std::mutex> lk(g_recent_mu);
        auto& q = g_recent[vfo];
        q.emplace_back(plugin_id, ev);
        while (q.size() > RECENT_MAX) q.pop_front();
    }
    g_events_in.fetch_add(1, std::memory_order_relaxed);
    bool post = false;
    {
        std::lock_guard<std::mutex> lk(g_qmu);
        if (g_queue.size() >= QUEUE_MAX) {
            g_events_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_queue.push_back(QEv{vfo, decoder, plugin_id, std::move(ev), now_ms()});
        if (!g_drain_posted) {
            g_drain_posted = true;
            post = true;
        }
    }
    // on the loop at once: a call's first frames follow its grant by 100-300 ms
    if (post && loop) loop->defer([] { drain(); });
}

std::vector<std::pair<std::string, PluginEvent>> recent_events(int vfo) {
    std::lock_guard<std::mutex> lk(g_recent_mu);
    auto it = g_recent.find(vfo);
    if (it == g_recent.end()) return {};
    return {it->second.begin(), it->second.end()};
}

void clear_recent() {
    std::lock_guard<std::mutex> lk(g_recent_mu);
    g_recent.clear();
}

bool dispatching() { return g_dispatching; }

bool is_trunk_vfo(int id) {
    const Inst* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_meta_mu);
        auto it = g_meta.find(id);
        if (it == g_meta.end() || !it->second.voice) return false;
        p = it->second.inst;
    }
    auto now = decimator_manager.getDecimator(id);
    return now && now.get() == p;
}

std::string vfo_info_json(int id) {
    const Inst* p = nullptr;
    std::string j;
    {
        std::lock_guard<std::mutex> lk(g_meta_mu);
        auto it = g_meta.find(id);
        if (it == g_meta.end()) return "";
        p = it->second.inst;
        j = it->second.json;
    }
    auto now = decimator_manager.getDecimator(id);
    return now && now.get() == p ? j : "";
}

void release_all_vfos(const std::string& why) {
    if (g_voices.empty()) return;
    for (auto& kv : g_calls)
        if (kv.second.vfo >= 0) unfollow(kv.second, why);
    remove_all_voices();
}

void tick() {
    try {
        drain();
        // a playback: its systems are a sandbox - put back as they were at its end
        const bool pb = playback::active();
        if (pb != g_pb) {
            g_pb = pb;
            release_everything(pb ? "playback" : "playback_end");
            g_tracks.clear();
            {
                std::lock_guard<std::mutex> lk(g_meta_mu);
                g_meta.clear();
            }
            g_learn.clear();
            g_fence_until = now_ms() + 1500;
            g_hold_sys.clear();
            g_hold_tg.clear();
            if (pb) {
                g_pb_saved = g_sys;
                for (auto& kv : g_sys) kv.second.ctl.clear();
            } else {
                g_sys = g_pb_saved;
                g_pb_saved.clear();
                for (auto& kv : g_sys) kv.second.lists_struct = true;
            }
            std::cout << "Trunk following: " << (pb ? "playback - nothing it finds is kept" : "playback over - the systems as before")
                      << std::endl;
            g_status_now = true;
        }
        const int mode = operating_mode_index();
        if (g_mode >= 0 && mode != g_mode) {
            std::cout << "Trunk following: the operating mode changed - the calls are freed" << std::endl;
            std::vector<uint64_t> ids;
            for (const auto& kv : g_calls)
                if (kv.second.vfo >= 0) ids.push_back(kv.first);
            for (uint64_t id : ids) end_call(id, "mode");
        }
        g_mode = mode;
        pause_check();
        tick_restore();
        tick_tracks();
        tick_calls();
        tick_voices();
        tick_lists();
        tick_save();
        send_status(g_status_now && now_ms() - g_status_sent >= STATUS_MIN_MS);
    } catch (const std::exception& e) {
        std::cerr << "Trunk following: " << e.what() << std::endl;
    }
}

void load() {
    std::ifstream f(SYSTEMS_FILE, std::ios::binary);
    g_loaded = true;
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    geo::Json j;
    const geo::Json* arr = nullptr;
    if (text.size() <= SYSTEMS_FILE_MAX && geo::json_parse(text, &j)) arr = j.get("systems");
    if (!arr || arr->type != geo::Json::ARR) {
        const std::string bad = std::string(SYSTEMS_FILE) + ".bad";
        fs_replace(SYSTEMS_FILE, bad.c_str());
        std::cerr << "Trunk following: " << SYSTEMS_FILE << " is unreadable (kept as " << bad << ") - starting without systems"
                  << std::endl;
        return;
    }
    size_t bad = 0;
    for (const auto& x : arr->a) {
        if (x.type != geo::Json::OBJ) { bad++; continue; }
        const std::string plugin = x.str("plugin"), key = x.str("key");
        if (!dig::valid_plugin_id(plugin) || key.empty() || !valid_key(key) || g_sys.size() >= MAX_SYSTEMS) { bad++; continue; }
        const std::string id = plugin + ":" + key;
        if (g_sys.count(id)) { bad++; continue; }
        System& s = g_sys[id];
        s.id = id;
        s.plugin = plugin;
        s.key = key;
        s.user_name = clean(x.str("name"), 64);
        s.name = clean(x.str("reported_name"), 64);
        s.listed_only = jbool(x, "listed_only");
        s.heard = jbool(x, "heard", true);   // (a file from before the flag: they were all heard)
        if (const geo::Json* t = x.get("tgs"); t && t->type == geo::Json::ARR)
            for (const auto& g : t->a) {
                const std::string tid = clean(g.str("id"), 32);
                if (tid.empty() || s.tgs.size() >= MAX_TGS) { bad++; continue; }
                Tg& e = s.tgs[tid];
                e.name = clean(g.str("name"), 32);
                const std::string m = g.str("mode");
                e.mode = m == "priority" ? TG_PRIORITY : m == "skip" ? TG_SKIP : TG_NORMAL;
                e.listed = jbool(g, "listed");
                e.last = static_cast<int64_t>(jnum(g, "last", 0));
                e.calls = static_cast<uint64_t>(std::max(0.0, jnum(g, "calls", 0)));
            }
        if (const geo::Json* c = x.get("chans"); c && c->type == geo::Json::ARR)
            for (const auto& g : c->a) {
                const std::string name = clean(g.str("ch"), 32);
                if (name.empty() || s.chans.size() >= MAX_CHANS) { bad++; continue; }
                Chan& e = s.chans[name];
                e.num = static_cast<long long>(jnum(g, "num", -1));
                auto hz = [&](const char* k) { const double v = jnum(g, k); return hz_ok(v) ? v : NAN; };
                e.user_hz = hz("user_hz");
                e.user_up = hz("user_up");
                e.ann_hz = hz("ann_hz");
                e.ann_up = hz("ann_up");
                e.learned_hz = hz("learned_hz");
                e.grants = static_cast<uint64_t>(std::max(0.0, jnum(g, "grants", 0)));
                e.last = static_cast<int64_t>(jnum(g, "last", 0));
            }
        if (const geo::Json* r = x.get("rules"); r && r->type == geo::Json::ARR)
            for (const auto& g : r->a) {
                Rule e;
                e.first = static_cast<long long>(jnum(g, "first", -1));
                e.last = static_cast<long long>(jnum(g, "last", -1));
                e.base_hz = jnum(g, "base_hz");
                e.step_hz = jnum(g, "step_hz");
                e.up_off_hz = jnum(g, "up_off_hz");
                if (e.first < 0 || e.last < e.first || !hz_ok(e.base_hz) || !fin(e.step_hz) || s.rules.size() >= MAX_RULES) {
                    bad++;
                    continue;
                }
                s.rules.push_back(e);
            }
    }
    std::cout << "Trunk following: " << g_sys.size() << " system(s) loaded from " << SYSTEMS_FILE << std::endl;
    if (bad) std::cerr << "Trunk following: " << bad << " unreadable entr" << (bad == 1 ? "y" : "ies") << " in " << SYSTEMS_FILE
                       << " skipped" << std::endl;
}

void save_now() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_save_mu);
        text.swap(g_unsaved);
    }
    if (!text.empty() && !playback::active()) write_file(text);
}

std::string status_json() { return build_status(true); }

// ============================================================================
// Commands
// ============================================================================

namespace {

// JSON argument of a TRUNK_*: command -> its system (must exist)
bool parse_obj(const std::string& body, geo::Json* j, std::string* err) {
    if (body.size() > 16 * 1024 || !geo::json_parse(body, j) || j->type != geo::Json::OBJ) {
        *err = "malformed (a JSON object is expected)";
        return false;
    }
    return true;
}
System* parse_sys(const geo::Json& j, std::string* err, bool create = false) {
    const std::string id = j.str("sys");
    System* s = sys_of(id);
    if (s || !create) {
        if (!s) *err = "no such system";
        return s;
    }
    // "<plugin>:<key>" of a system not heard yet: its entry is made (a channel
    // table entered before the first grant, a debug playback's commands)
    const size_t c = id.find(':');
    if (c == std::string::npos || !dig::valid_plugin_id(id.substr(0, c)) || id.size() - c - 1 == 0 || !valid_key(id.substr(c + 1))) {
        *err = "no such system (sys: <plugin id>:<system key>)";
        return nullptr;
    }
    if (g_sys.size() >= MAX_SYSTEMS) {
        *err = "too many systems (" + std::to_string(MAX_SYSTEMS) + ")";
        return nullptr;
    }
    return &ensure_system(id, id.substr(0, c), id.substr(c + 1));
}
bool parse_bool01(const std::string& v, bool* out) {
    if (v == "1" || v == "true") { *out = true; return true; }
    if (v == "0" || v == "false") { *out = false; return true; }
    return false;
}
bool parse_number(const std::string& v, double* out) {
    if (v.empty() || v.size() > 32) return false;
    char* end = nullptr;
    const double d = strtod(v.c_str(), &end);
    if (!end || *end || !fin(d)) return false;
    *out = d;
    return true;
}

// a command changed a system: saved now (an edit), its lists out soon, the status
void edited(System& s) {
    s.lists_struct = true;
    if (!temp_id(s.id)) mark_dirty(SAVE_EDIT_MS);
    g_status_now = true;
    // a call waiting for a frequency / a filter may go now
    for (auto& kv : g_calls) {
        Call& c = kv.second;
        if (c.sys != s.id) continue;
        if (c.group) {
            auto it = s.tgs.find(c.target);
            c.prio = it != s.tgs.end() && it->second.mode == TG_PRIORITY;
        }
        if (c.vfo >= 0) {
            const std::string why = policy_why(s, c);
            if (!why.empty()) unfollow(c, why);
        }
    }
    for (auto it = s.unresolved.begin(); it != s.unresolved.end();) {
        Channel ch;
        ch.name = it->first;
        ch.number = it->second.num;
        double hz, up;
        std::string src;
        if (resolve(s, ch, &hz, &up, &src)) it = s.unresolved.erase(it);
        else ++it;
    }
}

}  // namespace

bool handle_command(const std::string& msg, std::string* err, std::string* persist) {
    err->clear();
    persist->clear();
    const size_t colon = msg.find(':');
    const std::string cmd = msg.substr(0, colon);
    const std::string arg = colon == std::string::npos ? "" : msg.substr(colon + 1);
    auto done = [&](bool status = true) {
        if (status) g_status_now = true;
        if (!err->empty()) send_err(msg, *err);
        send_status(true);
        return true;
    };
    if (cmd == "GET_TRUNK") {
        send_status(true);
        return true;
    }
    if (cmd == "GET_TRUNK_SYS") {
        System* s = sys_of(arg);
        if (!s) {
            *err = "no such system";
            send_err(msg, *err);
            return true;
        }
        s->lists_sent = now_ms();
        s->lists_struct = s->lists_stats = false;
        broadcast(lists_json(*s));
        return true;
    }
    if (cmd == "TRUNK_FOLLOW" || cmd == "TRUNK_ENCRYPTED" || cmd == "TRUNK_UNIT" || cmd == "TRUNK_AUDIO" ||
        cmd == "TRUNK_ALL_SITES") {
        bool on;
        if (!parse_bool01(arg, &on)) {
            *err = "format: " + cmd + ":0|1";
            return done();
        }
        if (cmd == "TRUNK_FOLLOW") {
            if (g_set.follow != on) std::cout << "Trunk following: " << (on ? "on" : "off") << std::endl;
            g_set.follow = on;
            if (!on) {
                for (auto& kv : g_calls)
                    if (kv.second.vfo >= 0) unfollow(kv.second, "off");
                remove_all_voices();
            }
        } else if (cmd == "TRUNK_ENCRYPTED") {
            g_set.enc = on;
        } else if (cmd == "TRUNK_UNIT") {
            g_set.unit = on;
        } else if (cmd == "TRUNK_ALL_SITES") {
            g_set.all_sites = on;
        } else {
            g_set.audio = on;
            for (auto& v : g_voices)
                if (v.call && live(v)) set_heard(v.vfo, on);
        }
        if (cmd != "TRUNK_FOLLOW" && cmd != "TRUNK_AUDIO")
            for (auto& kv : g_calls) {
                Call& c = kv.second;
                System* s = sys_of(c.sys);
                if (c.vfo >= 0 && s) {
                    const std::string why = policy_why(*s, c);
                    if (!why.empty()) unfollow(c, why);
                }
            }
        *persist = cmd + ":" + (on ? "1" : "0");
        return done();
    }
    if (cmd == "TRUNK_MAX_VFOS" || cmd == "TRUNK_HANG") {
        double v;
        if (!parse_number(arg, &v)) {
            *err = "format: " + cmd + (cmd == "TRUNK_HANG" ? ":<seconds 0.5-10>" : ":<1-8>");
            return done();
        }
        char c[48];
        if (cmd == "TRUNK_MAX_VFOS") {
            g_set.max_vfos = static_cast<int>(std::clamp<long long>(std::llround(v), 1, 8));
            snprintf(c, sizeof c, "TRUNK_MAX_VFOS:%d", g_set.max_vfos);
        } else {
            g_set.hang_s = std::clamp(std::round(v * 10) / 10, 0.5, 10.0);
            snprintf(c, sizeof c, "TRUNK_HANG:%.1f", g_set.hang_s);
        }
        *persist = c;
        return done();
    }
    if (cmd == "TRUNK_END") {
        double id;
        if (!parse_number(arg, &id)) {
            *err = "format: TRUNK_END:<call id>";
            return done();
        }
        auto it = g_calls.find(static_cast<uint64_t>(id));
        if (it == g_calls.end()) {
            *err = "no such call";
            return done();
        }
        Call& c = it->second;
        c.user_ended = true;
        std::cout << "Trunk following: " << c.sys << " " << c.target << " ended by the user" << std::endl;
        if (c.vfo >= 0) unfollow(c, "stopped");
        else if (System* s = sys_of(c.sys)) set_why(c, *s, "stopped");
        return done();
    }
    if (cmd == "TRUNK_HOLD") {
        if (arg.empty()) {
            g_hold_sys.clear();
            g_hold_tg.clear();
            std::cout << "Trunk following: hold released" << std::endl;
            return done();
        }
        geo::Json j;
        if (!parse_obj(arg, &j, err)) return done();
        System* s = parse_sys(j, err);
        if (!s) return done();
        const std::string tg = clean(j.str("tg"), 32);
        if (tg.empty()) {
            *err = "the talkgroup is missing";
            return done();
        }
        g_hold_sys = s->id;
        g_hold_tg = tg;
        std::cout << "Trunk following: holding on " << s->id << " TG " << tg << std::endl;
        for (auto& kv : g_calls) {
            Call& c = kv.second;
            if (c.vfo >= 0 && (c.sys != g_hold_sys || !c.group || c.target != g_hold_tg)) unfollow(c, "hold");
        }
        return done();
    }
    if (cmd == "TRUNK_SYS_SET" || cmd == "TRUNK_SYS_DEL" || cmd == "TRUNK_TG_SET" || cmd == "TRUNK_TG_DEL" ||
        cmd == "TRUNK_CH_SET" || cmd == "TRUNK_CH_DEL" || cmd == "TRUNK_RULE_SET" || cmd == "TRUNK_RULE_DEL") {
        geo::Json j;
        if (!parse_obj(arg, &j, err)) return done();
        const bool creates = cmd == "TRUNK_SYS_SET" || cmd == "TRUNK_TG_SET" || cmd == "TRUNK_CH_SET" || cmd == "TRUNK_RULE_SET";
        System* s = parse_sys(j, err, creates);
        if (!s) return done();
        if (cmd == "TRUNK_SYS_SET") {
            if (j.get("name")) {
                const geo::Json* n = j.get("name");
                if (n->type != geo::Json::STR) { *err = "name: a string"; return done(); }
                s->user_name = clean(n->s, 64);
            }
            if (const geo::Json* l = j.get("listed_only")) {
                if (l->type != geo::Json::BOOL) { *err = "listed_only: true or false"; return done(); }
                s->listed_only = l->b;
            }
            edited(*s);
            return done();
        }
        if (cmd == "TRUNK_SYS_DEL") {
            // forgotten: its calls end, the file loses it (it comes back unnamed when heard again)
            const std::string id = s->id;
            std::vector<uint64_t> ids;
            for (const auto& kv : g_calls)
                if (kv.second.sys == id) ids.push_back(kv.first);
            for (uint64_t c : ids) {
                end_call(c, "forgotten");
                drop_call(c);
            }
            std::map<int, Ctl> ctl = s->ctl;
            const std::string plugin = s->plugin, key = s->key;
            g_sys.erase(id);
            if (g_hold_sys == id) g_hold_sys.clear();
            // a control VFO still on it: the system starts over, empty
            if (!ctl.empty()) {
                System& n = ensure_system(id, plugin, key);
                n.ctl = ctl;
            }
            if (!temp_id(id)) mark_dirty(SAVE_EDIT_MS);
            std::cout << "Trunk following: system " << id << " forgotten" << std::endl;
            return done();
        }
        if (cmd == "TRUNK_TG_SET" || cmd == "TRUNK_TG_DEL") {
            const std::string tg = clean(j.str("tg"), 32);
            if (tg.empty() || tg != j.str("tg")) {
                *err = "tg: 1-32 characters, no control characters";
                return done();
            }
            if (cmd == "TRUNK_TG_DEL") {
                s->tgs.erase(tg);
                edited(*s);
                return done();
            }
            int mode = TG_NORMAL;
            std::string m = j.str("mode");
            if (!m.empty() && m != "normal" && m != "priority" && m != "skip") {
                *err = "mode: normal, priority or skip";
                return done();
            }
            mode = m == "priority" ? TG_PRIORITY : m == "skip" ? TG_SKIP : TG_NORMAL;
            Tg* t = ensure_tg(*s, tg);
            if (!t) {
                *err = "too many talkgroups (" + std::to_string(MAX_TGS) + ")";
                return done();
            }
            if (j.get("name")) t->name = clean(j.str("name"), 32);
            if (!m.empty()) t->mode = mode;
            t->listed = true;
            edited(*s);
            return done();
        }
        if (cmd == "TRUNK_CH_SET" || cmd == "TRUNK_CH_DEL") {
            const std::string ch = clean(j.str("ch"), 32);
            if (ch.empty() || ch != j.str("ch")) {
                *err = "ch: 1-32 characters, no control characters";
                return done();
            }
            if (cmd == "TRUNK_CH_DEL") {
                // what goes: src user (default) | announced | learned | all
                const std::string src = j.str("src").empty() ? "user" : j.str("src");
                auto it = s->chans.find(ch);
                if (it == s->chans.end()) {
                    *err = "no such channel";
                    return done();
                }
                if (src == "user") it->second.user_hz = it->second.user_up = NAN;
                else if (src == "announced") it->second.ann_hz = it->second.ann_up = NAN;
                else if (src == "learned") it->second.learned_hz = NAN;
                else if (src == "all") s->chans.erase(it);
                else {
                    *err = "src: user, announced, learned or all";
                    return done();
                }
                edited(*s);
                return done();
            }
            const double hz = jnum(j, "hz");
            if (!hz_ok(hz)) {
                *err = "hz: the downlink frequency in Hz (0 - 6 GHz)";
                return done();
            }
            double up = NAN;
            if (const geo::Json* u = j.get("up_hz"); u && u->type != geo::Json::NUL) {
                up = jnum(j, "up_hz");
                if (!hz_ok(up)) {
                    *err = "up_hz: the uplink frequency in Hz, or null";
                    return done();
                }
            }
            double num = -1;
            if (const geo::Json* n = j.get("num"); n && n->type != geo::Json::NUL) {
                num = jnum(j, "num", -2);
                if (num < 0 || num > 1e12 || num != std::floor(num)) {
                    *err = "num: a channel number >= 0, or null";
                    return done();
                }
            }
            Chan* c = ensure_chan(*s, ch);
            if (!c) {
                *err = "too many channels (" + std::to_string(MAX_CHANS) + ")";
                return done();
            }
            c->user_hz = std::round(hz);
            c->user_up = fin(up) ? std::round(up) : NAN;
            if (num >= 0) c->num = static_cast<long long>(num);
            edited(*s);
            return done();
        }
        // rules
        if (cmd == "TRUNK_RULE_DEL") {
            const double i = jnum(j, "i", -1);
            if (i < 0 || i >= static_cast<double>(s->rules.size()) || i != std::floor(i)) {
                *err = "no such rule";
                return done();
            }
            s->rules.erase(s->rules.begin() + static_cast<std::ptrdiff_t>(i));
            edited(*s);
            return done();
        }
        Rule r;
        const double first = jnum(j, "first", -1), last = jnum(j, "last", -1);
        r.base_hz = jnum(j, "base_hz");
        r.step_hz = jnum(j, "step_hz");
        if (first < 0 || last < first || first != std::floor(first) || last != std::floor(last) || last > 1e9) {
            *err = "first / last: channel numbers, 0 <= first <= last";
            return done();
        }
        if (!hz_ok(r.base_hz) || !fin(r.step_hz) || std::fabs(r.step_hz) > 10e6 || r.step_hz == 0) {
            *err = "base_hz: 0 - 6 GHz; step_hz: not 0, at most 10 MHz";
            return done();
        }
        r.first = static_cast<long long>(first);
        r.last = static_cast<long long>(last);
        const double top = r.base_hz + static_cast<double>(r.last - r.first) * r.step_hz;
        if (!hz_ok(top)) {
            *err = "the rule's last channel is outside 0 - 6 GHz";
            return done();
        }
        if (const geo::Json* u = j.get("up_off_hz"); u && u->type != geo::Json::NUL) {
            r.up_off_hz = jnum(j, "up_off_hz");
            if (!fin(r.up_off_hz) || std::fabs(r.up_off_hz) > 1e9) {
                *err = "up_off_hz: the uplink offset in Hz, or null";
                return done();
            }
        }
        const double i = jnum(j, "i", -1);
        if (i >= 0) {
            if (i >= static_cast<double>(s->rules.size()) || i != std::floor(i)) {
                *err = "no such rule";
                return done();
            }
            s->rules[static_cast<size_t>(i)] = r;
        } else {
            if (s->rules.size() >= MAX_RULES) {
                *err = "too many rules (" + std::to_string(MAX_RULES) + ")";
                return done();
            }
            s->rules.push_back(r);
        }
        edited(*s);
        return done();
    }
    if (cmd.rfind("TRUNK_", 0) == 0) {
        *err = "unknown trunk command";
        return done(false);
    }
    return false;
}

}  // namespace trunk
