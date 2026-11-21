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
#include "settings_store.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/audio_mixer.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#ifdef __linux__
#include <sched.h>
#endif
#include <iomanip>
#include <thread>
#include <cstring>
#include <charconv>
#include <future>
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
// a voice VFO at most this wide: twice the widest traffic channel of the five
// protocols (TETRA's 25 kHz carrier; P25 / DMR / NXDN / MPT1327 12.5 kHz) - a
// grant can't make a 2.4 MHz VFO
constexpr double VOICE_BW_MAX_HZ = 50000;
constexpr int64_t STATUS_MIN_MS = 250;        // status at most 4 / s ...
constexpr int64_t STATUS_ACTIVE_MS = 1000;    // ... and at least 1 / s while anything is active
constexpr int64_t LISTS_STRUCT_MS = 1000;     // the big lists after a change of what is in them
constexpr int64_t LISTS_STATS_MS = 10000;     // ... after counters / last-heard only
constexpr int64_t SAVE_EDIT_MS = 0;           // the systems file: a user's edit at once,
constexpr int64_t SAVE_LEARN_MS = 30000;      // ... new announced / learned / heard entries within 30 s,
constexpr int64_t SAVE_STATS_MS = 600000;     // ... counters and last-heard times every 10 min
constexpr const char* SYSTEMS_FILE = "trunk_systems.json";
// what is written always reads back: the user's entries alone can reach
// 32 x (2000 talkgroups + 2000 channels) x ~180 B = 23 MB; a text over this is
// not written (the old file stays, the status says so)
constexpr size_t SYSTEMS_FILE_MAX = 32u << 20;

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
    bool retune = false;     // TRUNK_RETUNE: coherent mode - a call outside the span: an excursion (retune, follow, return)
    bool retune_cal = true;  // TRUNK_RETUNE_CAL: ... calibrated on arrival (else held for the whole excursion)
    double retune_max_s = 120;   // TRUNK_RETUNE_MAX: ... the longest time away (s, 0 = no limit)
    std::vector<int> tuners;     // TRUNK_TUNERS: independent mode - the tuners the manager may retune for voice channels
};
Settings g_set;

// --- an excursion outside the span (TRUNK_RETUNE; below, "Coherent mode: an excursion") ---
struct ExcCtl {
    int vfo = -1;
    uint64_t serial = 0;    // DecimatorInstance::serial (the id may be another VFO's by the return)
    float off = 0;          // its offset at home
    double rf = 0;          // its RF at home
    bool parked = false;    // outside the away span: disabled while away
};
struct Exc {
    int st = 0;             // 0 at home, 1 away, 2 returning (home again, waiting for calibration + control channel)
    uint64_t call = 0;      // the call it left for
    std::string key, sys, target, ch, decoder;
    double hz = 0;          // its channel
    double home = 0, centre = 0;
    int64_t t_left = 0, t_arrived = 0, t_cal = 0, t_idle = 0, t_back = 0, t_back_arr = 0, t_back_cal = 0;
    int64_t w_left = 0;     // wall ms
    bool cal = true;        // calibrated on arrival (else held for the whole excursion)
    int side = -1;          // the Wideband downconverter's mixer side at home (-1 = not that variant)
    int64_t t_cal_push = 0; // the voice VFOs' SETUP clocks wait for the calibration at most until then
    bool holding = false;
    int64_t hold_sent = 0;
    bool kerberos = false;  // KerberosSDR manual mode: no calibration requests
    std::string end;        // why it returned (the latest)
    std::string centre_how; // "control" (a control VFO of the system in the away span) | "offset"
    std::vector<ExcCtl> ctl;
};
Exc g_exc;
struct ExcStats {
    uint64_t n = 0;
    int64_t away_ms = 0;
    std::map<std::string, uint64_t> ends;
    // the latest excursion's timings (ms; -1 = not seen)
    int64_t arrive_ms = -1, cal_ms = -1, back_arrive_ms = -1, back_cal_ms = -1, ctl_back_ms = -1, first_frames_ms = -1;
    uint64_t calls_away = 0, calls_missed = 0;   // grants heard while away / not followed because of it
};
ExcStats g_exc_stats;
std::map<std::string, int64_t> g_exc_retry;   // call key -> no excursion for it before (steady ms)


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
    bool user = false;                     // the user edited it (or the file has their entries): never evicted by the air
    int64_t last_heard = 0;                // steady ms of its last event (the air systems' eviction order)
    int64_t chans_full_until = 0, tgs_full_until = 0;   // a full table of the user's entries: no rescan till then
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
std::mutex g_save_mu;          // the file: one writer at a time (the loop's save, main's save_now)
std::string g_unsaved;         // serialised, not yet written (save_now's fallback)
bool g_shutdown = false;       // (g_save_mu) main's last save ran: the loop writes no more
std::string g_file_note;       // the status' systems_file: "" = fine, else what went wrong
std::string g_file_bad;        // where an unreadable file was kept
std::atomic<bool> g_loaded{false};

bool g_edit_dirty = false;   // a user's edit is unsaved (with following off only these are written)
void mark_dirty(int64_t within) {
    const int64_t now = now_ms();
    if (within == 0) g_edit_dirty = true;
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
    std::string lost;                       // this over: why it lost its voice VFO ("" = it didn't)
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
void drop_call(uint64_t id);

std::string call_key(const std::string& sys, int site, bool group, const std::string& target) {
    return sys + "\x1f" + std::to_string(site) + "\x1f" + (group ? "g" : "u") + "\x1f" + target;
}

// --- voice VFOs ---------------------------------------------------------------
struct Voice {
    int vfo = -1;
    std::weak_ptr<Inst> inst;
    uint64_t dec = 0;                       // its decoder's serial (DecimatorInstance::digital_serial)
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
    // the latest placement: its decoder had delivered reports and the placement
    // resets it (a retune > 100 Hz, another plugin) - its reports wait for that
    // RESET (or 2 s); false: what it says counts at once
    bool await_reset = false;
    // the latest placement kept the frequency and the plugin (the previous
    // over's channel again): an ENDED / RELEASED counts only once the call was
    // active on it since the placement (a trailing release of the previous over
    // can't end the new one)
    bool same_place = false;
    bool active_here = false;               // since the latest placement: CALL ACTIVE, valid frames or the analogue squelch
};
std::vector<Voice> g_voices;
void park(Voice& v);

// --- the VFOs that report (control / traffic) -------------------------------
struct Track {
    uint64_t dec = 0;                       // the decoder's serial (DecimatorInstance::digital_serial)
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
bool g_srch_skip_restore = false;   // the user's retune ended a search: its pause's end puts nothing back
bool g_paused = false;
std::string g_pause_why, g_pause_why_last;
int g_mode = -1;                            // operating mode last seen
bool g_pb = false;                          // playback last seen
std::map<std::string, System> g_pb_saved;   // the systems before the playback
std::string g_hold_sys, g_hold_tg;          // TRUNK_HOLD
std::deque<int64_t> g_lat_place, g_lat_active;   // ms: grant reached the loop -> VFO placed / -> ACTIVE
std::atomic<uint64_t> g_events_in{0}, g_events_dropped{0}, g_resets_coalesced{0};
std::map<std::string, int64_t> g_opt_drop_logged;   // plugin -> last "options dropped" log line (steady ms)
uint64_t g_opts_dropped = 0;
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
    uint64_t dec;
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
    uint64_t inst = 0;   // DecimatorInstance::serial (an address can be the next VFO's)
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
    m.inst = inst->serial;
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

// Room in a container the air fills: when it is full, the oldest tenth of the
// entries `evictable` allows goes in one pass (so an insert at the bound costs
// O(log n) amortised, not a scan per insert); false = nothing may go (every
// entry is the user's) - then *no_scan_until keeps the next tries from
// rescanning for a second
template <class M, class Ok, class Stamp>
bool make_room(M& m, size_t max, Ok evictable, Stamp stamp, int64_t* no_scan_until = nullptr) {
    if (m.size() < max) return true;
    const int64_t now = now_ms();
    if (no_scan_until && now < *no_scan_until) return false;
    std::vector<std::pair<int64_t, typename M::key_type>> c;
    for (const auto& kv : m)
        if (evictable(kv.second)) c.push_back({stamp(kv.second), kv.first});
    if (c.empty()) {
        if (no_scan_until) *no_scan_until = now + 1000;
        return false;
    }
    const size_t k = std::min(c.size(), std::max<size_t>(1, max / 10));
    std::nth_element(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(k - 1), c.end());
    for (size_t i = 0; i < k; i++) m.erase(c[i].second);
    return true;
}

uint64_t g_sys_refused = 0;   // new air systems not made: the table full of the user's

// A system: found, or made. Full (MAX_SYSTEMS): an air system nobody edited and
// no control VFO reports goes - the temporary ones first, then the one heard
// longest ago; a system the user edited is never evicted. Every one the user's
// or in use: nullptr (counted, sys_refused) - the user deletes systems themselves
System* ensure_system(const std::string& id, const std::string& plugin, const std::string& key) {
    auto it = g_sys.find(id);
    if (it != g_sys.end()) return &it->second;
    if (g_sys.size() >= MAX_SYSTEMS) {
        std::string drop;
        int64_t best = 0;
        for (const auto& kv : g_sys) {
            const System& x = kv.second;
            if (x.user || !x.ctl.empty() || kv.first == g_hold_sys) continue;
            bool calls = false;
            for (const auto& c : g_calls)
                if (c.second.sys == kv.first && c.second.vfo >= 0) calls = true;
            if (calls) continue;
            const int64_t rank = (temp_id(kv.first) ? -(int64_t(1) << 60) : 0) + x.last_heard;
            if (drop.empty() || rank < best) {
                drop = kv.first;
                best = rank;
            }
        }
        if (drop.empty()) {
            g_sys_refused++;
            g_status_now = true;
            return nullptr;
        }
        std::vector<uint64_t> ids;
        for (const auto& c : g_calls)
            if (c.second.sys == drop) ids.push_back(c.first);
        for (uint64_t c : ids) drop_call(c);
        g_sys.erase(drop);
    }
    System& s = g_sys[id];
    s.id = id;
    s.plugin = plugin;
    s.key = key;
    s.lists_struct = true;
    s.last_heard = now_ms();
    if (!temp_id(id)) mark_dirty(SAVE_LEARN_MS);
    return &s;
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
    // full: the least recently granted entries without a user frequency go
    if (!make_room(s.chans, MAX_CHANS, [](const Chan& c) { return !fin(c.user_hz); },
                   [](const Chan& c) { return c.last; }, &s.chans_full_until))
        return nullptr;
    s.lists_struct = true;
    return &s.chans[name];
}

Tg* ensure_tg(System& s, const std::string& id) {
    auto it = s.tgs.find(id);
    if (it != s.tgs.end()) return &it->second;
    // full: the least recently heard ones not listed by the user go
    if (!make_room(s.tgs, MAX_TGS, [](const Tg& t) { return !t.listed; }, [](const Tg& t) { return t.last; },
                   &s.tgs_full_until))
        return nullptr;
    s.lists_struct = true;
    return &s.tgs[id];
}

void note_unit(System& s, const std::string& id, bool call) {
    if (id.empty()) return;
    auto it = s.units.find(id);
    if (it == s.units.end()) {
        make_room(s.units, MAX_UNITS, [](const Unit&) { return true; }, [](const Unit& u) { return u.last; });
        it = s.units.emplace(id, Unit{}).first;
        s.lists_struct = true;
    }
    it->second.last = wall_ms();
    if (call) it->second.calls++;
    s.lists_stats = true;
}

Unresolved* ensure_unresolved(System& s, const std::string& name) {
    auto it = s.unresolved.find(name);
    if (it != s.unresolved.end()) return &it->second;
    make_room(s.unresolved, MAX_UNRESOLVED, [](const Unresolved&) { return true; },
              [](const Unresolved& u) { return u.last; });
    return &s.unresolved[name];
}

// A temporary system (no key yet) becomes the keyed one: what it gathered moves there
// (by value: a caller passes a Track's sys, which the loop below rewrites)
void merge_temp(const std::string from, const std::string to, const std::string plugin, const std::string key) {
    if (from == to || !g_sys.count(from)) return;
    System* dp = ensure_system(to, plugin, key);
    auto it = g_sys.find(from);
    if (it == g_sys.end()) return;   // (made room for the new one)
    if (!dp) {
        // no room for the keyed one: the temporary one goes (its VFO's events
        // come back under the key and are refused, counted, until there is room)
        std::vector<uint64_t> ids;
        for (const auto& c : g_calls)
            if (c.second.sys == from) ids.push_back(c.first);
        for (uint64_t c : ids) drop_call(c);
        for (auto& kv : g_tracks)
            if (kv.second.sys == from) kv.second.sys = to;
        g_sys.erase(from);
        return;
    }
    System& d = *dp;
    System& t = it->second;
    // every entry through the containers' insertion functions (their bounds,
    // their eviction - never a user entry)
    for (auto& kv : t.chans) {
        Chan* c = ensure_chan(d, kv.first);
        if (!c) break;
        if (c->num < 0) c->num = kv.second.num;
        if (fin(kv.second.ann_hz)) {
            c->ann_hz = kv.second.ann_hz;
            c->ann_up = kv.second.ann_up;
        }
        c->grants += kv.second.grants;
        c->data += kv.second.data;
        c->last = std::max(c->last, kv.second.last);
    }
    for (auto& kv : t.tgs) {
        Tg* g = ensure_tg(d, kv.first);
        if (!g) break;
        g->calls += kv.second.calls;
        g->last = std::max(g->last, kv.second.last);
    }
    for (auto& kv : t.units) {
        note_unit(d, kv.first, false);
        Unit& u = d.units[kv.first];
        u.calls += kv.second.calls;
        u.last = std::max(u.last, kv.second.last);
    }
    for (auto& kv : t.unresolved) *ensure_unresolved(d, kv.first) = kv.second;
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
    // the history rows of the temporary id: the keyed one's now
    const std::string was = "\"sys\":" + js(from) + ",", now_is = "\"sys\":" + js(to) + ",";
    for (auto& h : g_history)
        for (size_t at = h.find(was); at != std::string::npos; at = h.find(was, at + now_is.size()))
            h.replace(at, was.size(), now_is);
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
         ",\"t_end\":" + std::to_string(wall_ms()) + ",\"followed\":" + jb(c.ever_followed) +
         (c.lost.empty() ? std::string() : ",\"lost\":" + js(c.lost)) + "}";
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
    } else if (auto inst = live(v)) {
        // the last VFO (the user removed theirs): it can't go - it is the user's
        // now, as a plain VFO: enabled, NBFM, its decoder off, muted, where it was
        inst->enabled = true;
        decimator_manager.setDigitalMode(v.vfo, dig::Mode::OFF, "");
        decimator_manager.setDemodMode(v.vfo, DemodulatorMode::NBFM);
        std::cout << "Trunk following: voice VFO D" << v.vfo << " is the last VFO - handed back as a plain NBFM VFO (decoder off)"
                  << std::endl;
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
bool g_on_grant = false;   // apply_event is handling a GRANT (placement latency)
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
    const double need = std::min(VOICE_BW_MAX_HZ, std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0));
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
    // will this placement reset a decoder that has reported (then a RESET comes,
    // and what is in the pipe before it is the previous channel's)? The engine
    // resets at a retune > 100 Hz and at another mode / plugin; it sends the
    // RESET only when it delivered reports since its last one (its own flag,
    // or reports of it still in our queue)
    {
        auto dd = inst->getDigital();
        bool delivered = dd && dd->trunk_reported();
        if (dd && !delivered) {
            const uint64_t sn = inst->digital_serial.load();
            std::lock_guard<std::mutex> lk(g_qmu);
            for (const auto& q : g_queue)
                if (q.vfo == v.vfo && q.dec == sn && q.ev.kind != PluginEvent::RESET) {
                    delivered = true;
                    break;
                }
        }
        const bool moved = v.rf_hz <= 0 || std::fabs(c.hz - v.rf_hz) > 100.0;
        v.await_reset = delivered && (moved || !same_plugin);
        v.same_place = !moved && same_plugin;
        v.active_here = false;
    }
    // a retune > 100 Hz resets the decoder: that RESET is ours
    v.dec = inst->digital_serial.load();
    g_tracks[v.vfo].expect_reset_until = now + RESET_EXPECT_MS;
    // heard first: a VFO heard first moves the audio channel with it (independent)
    set_heard(v.vfo, g_set.audio);
    dispatch(cmds);
    inst->enabled = true;
    v.dec = inst->digital_serial.load();
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
    // (only a placement on the grant's arrival: one that waited - for a free
    // VFO, a pre-emption, PLACE_MIN_MS - measures the wait, not the latency)
    if (c.t_rx_grant > 0 && g_on_grant) {
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
    if (!plugin_info(c.decoder, &pi) || !pi.built || !pi.trunking || pi.fixed_freq_hz > 0 || pi.lock_vfo) return "no_decoder";
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
    if (c.vfo >= 0) c.lost = why;
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
    c.guard = was_followed || why == "duplicate" || why == "taken_over";
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
        // (a copy that was followed first - it lost its VFO, e.g. no_signal, and
        // this site's took over: "taken_over", its row says why it lost it)
        for (uint64_t d : dup) end_call(d, g_calls[d].ever_followed ? "taken_over" : "duplicate");
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
std::string exc_try(Call& c, double hz, double bw);
std::string vt_try(Call& c, double hz, double bw);
bool plugin_trunking(const std::string& id);
bool exc_in_transit();
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
            const double bw = std::min(VOICE_BW_MAX_HZ, std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0));
            int prefer = -1;
            if (auto ci = c.ctl_vfo >= 0 ? decimator_manager.getDecimator(c.ctl_vfo) : nullptr) prefer = ci->tuner_channel.load();
            const Place p = place_for(hz, bw, prefer);
            if (exc_in_transit()) {
                why = c.id == g_exc.call ? "leaving" : "retuning";   // (placed at the next grant once it is there)
            } else if (!p.ok) {
                why = exc_try(c, hz, bw);                        // (coherent: an excursion)
                if (why == "out_of_span") why = vt_try(c, hz, bw);   // (independent: a voice tuner)
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
        Unresolved& u = *ensure_unresolved(*s, c.ch.name);
        u.num = c.ch.number;
        u.n++;
        u.last = wall_ms();
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
    const double bw = std::min(VOICE_BW_MAX_HZ, std::max(c.bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0));
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
    if (g_exc.st == 1) g_exc_stats.calls_away++;
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
    c.lost.clear();
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
    System* sp = ensure_system(sid, plugin, temp_id(sid) ? "" : sid.substr(sid.find(':') + 1));
    if (!sp) return;   // (no room: counted)
    System& s = *sp;
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
        const std::string qk = key + "\x1f" + ev.ch.name + (ev.data ? "\x1f" "d" : "\x1f" "s");
        const int64_t now = now_ms();
        auto q = s.quiet.find(qk);
        const bool fresh = q == s.quiet.end() || now - q->second > WAIT_EXPIRE_MS;
        // (bounded by count: past it the whole map goes - a sequence may then be
        // counted twice, never a scan per grant)
        if (s.quiet.size() >= 4096 && q == s.quiet.end()) s.quiet.clear();
        s.quiet[qk] = now;
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
    // the grant's options: only keys the traffic plugin declares (Info::options,
    // hidden ones too) - the engine checked only their form
    std::vector<std::pair<std::string, std::string>> opts;
    {
        dig::PluginInfo pi;
        const bool known = plugin_info(decoder, &pi);
        size_t dropped = 0;
        std::string first;
        for (const auto& kv : ev.options) {
            bool ok = false;
            if (known)
                for (const auto& o : pi.options)
                    if (o.key == kv.first) ok = true;
            if (ok) opts.push_back(kv);
            else if (!dropped++) first = kv.first;
        }
        if (dropped && known) {
            g_opts_dropped += dropped;
            int64_t& last = g_opt_drop_logged[decoder];
            if (now_ms() - last > 60000) {
                last = now_ms();
                std::cerr << "Trunk following: a grant for " << decoder << " carries option(s) it doesn't declare (\"" << first
                          << "\"" << (dropped > 1 ? " +" + std::to_string(dropped - 1) : "") << ") - dropped (logged once a minute)"
                          << std::endl;
            }
        }
    }
    const bool opts_changed = c->options != opts || c->decoder != decoder;
    c->options = opts;
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
                const Place p = place_for(hz, std::min(VOICE_BW_MAX_HZ, std::max(c->bw, pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500.0)), v->tuner);
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
    System* sp = ensure_system(t.sys, plugin, temp_id(t.sys) ? "" : t.sys.substr(t.sys.find(':') + 1));
    if (!sp) return;
    System& s = *sp;
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
    // (no key in the report - its decoder hasn't decoded it again since its
    // RESET, e.g. after our own retune of the VFO: the system it had stands;
    // only another KEY is another system)
    const std::string sid = ev.sys_key.empty() ? (t.sys.empty() ? plugin + ":@" + std::to_string(vfo) : t.sys)
                                               : plugin + ":" + ev.sys_key;
    if (!t.sys.empty() && t.sys != sid) {
        if (temp_id(t.sys) && !temp_id(sid)) {
            merge_temp(t.sys, sid, plugin, ev.sys_key);
        } else if (System* old = sys_of(t.sys)) {
            old->ctl.erase(vfo);   // the VFO hears another system now: its calls there are over
            std::vector<uint64_t> done;
            for (const auto& kv : g_calls)
                if (kv.second.sys == t.sys && kv.second.ctl_vfo == vfo) done.push_back(kv.first);
            for (uint64_t id : done) end_call(id, "control_reset");
        }
    }
    t.sys = sid;
    t.system_said = true;
    t.control = ev.control;
    System* sp = ensure_system(sid, plugin, ev.sys_key);
    if (!sp) return;
    System& s = *sp;
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
    if (!g_set.follow || g_paused) {
        // (following off: the user's VFO is never moved; paused: a scanner owns the VFOs)
        const std::string n = "control channel " + ev.ch.name + ": not moved (" +
                              (g_set.follow ? "paused" : "following is off") + ")";
        if (t.control_note != n) {
            t.control_note = n;
            g_status_now = true;
        }
        return;
    }
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
    // the user's VFO - through the dispatching guard: the move itself is not saved
    // (the VFO set the user saves next carries it where it is then)
    const bool was = g_dispatching;
    g_dispatching = true;
    const std::string why = ControlHandler::apply_command(f);
    g_dispatching = was;
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
// (g_learn: at most LEARN_MAX observations, each kept LEARN_KEEP_MS)
constexpr size_t LEARN_MAX = 512;
constexpr int64_t LEARN_KEEP_MS = 120000;
void prune_learn() {
    const int64_t now = now_ms();
    for (auto it = g_learn.begin(); it != g_learn.end();) it = now - it->second.t > LEARN_KEEP_MS ? g_learn.erase(it) : std::next(it);
    make_room(g_learn, LEARN_MAX, [](const LearnObs&) { return true; }, [](const LearnObs& o) { return o.t; });
}

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
    if (!g_learn.count(lk)) prune_learn();
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

uint64_t g_stale_reports = 0;   // voice-VFO CALL reports not taken as the placed call's (above)
// a CALL from a voice VFO: what its channel carries
void on_voice_call(Voice& v, const PluginEvent& ev) {
    auto it = g_calls.find(v.call);
    if (it == g_calls.end()) return;
    Call& c = it->second;
    System* s = sys_of(c.sys);
    if (!s) return;
    const int64_t now = now_ms();
    // reports of the previous channel may still be in the pipe after a placement
    // that reset a decoder which had reported: none counts before that RESET
    // (or 2 s). A fresh VFO / one that reported nothing: at once
    if (v.await_reset && !v.reset_seen && now - c.t_placed < 2000) {
        g_stale_reports++;
        return;
    }
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
        if (now - c.t_placed < STALE_FRAMES_MS * 3 || (v.await_reset && !v.reset_seen && now - c.t_placed < 2000)) return;
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
    // the previous over's channel again: its trailing ENDED / RELEASED is not
    // this over's end - taken only once this one was active here
    if (ev.call_state != 1 && v.same_place && !v.active_here) {
        g_stale_reports++;
        return;
    }
    if (ev.call_state == 2) return end_call(c.id, "released");
    if (ev.call_state == 1) {
        v.active_here = true;
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
    System* s = sys_of(sys);
    if (!s) return;
    s->last_heard = now_ms();
    if (!s->heard) {
        s->heard = true;
        if (!temp_id(s->id)) mark_dirty(SAVE_LEARN_MS);
        g_status_now = true;
    }
}

uint64_t g_untrunking = 0;   // events of plugins that don't declare trunking (dropped)
bool plugin_trunking(const std::string& id) {
    static std::map<std::string, std::pair<bool, int64_t>> cache;
    const int64_t now = now_ms();
    auto& e = cache[id];
    if (now - e.second > 5000 || e.second == 0) {
        dig::PluginInfo pi;
        e.first = plugin_info(id, &pi) && pi.trunking;
        e.second = now;
    }
    return e.first;
}

bool is_probe(int vfo);
bool searching();
void srch_event(int vfo, const PluginEvent& ev);
void cc_heard(int vfo, const std::string& plugin, const PluginEvent& ev);
uint64_t g_srch_dropped = 0;   // events of other VFOs while a search runs (the user's control VFOs are parked)
void apply_event(QEv& q) {
    // only plugins that declare trunking report systems / grants / calls (a
    // RESET is the engine's own: always taken)
    if (q.ev.kind != PluginEvent::RESET && !plugin_trunking(q.plugin)) {
        g_untrunking++;
        return;
    }
    // the VFO as it is now: an event of a decoder that is gone (removed VFO, id reused) is dropped
    auto inst = decimator_manager.getDecimator(q.vfo);
    if (!inst) return;
    // (by the serial, not the address: a decoder made after the one that sent
    // this - a removed VFO's id reused - can sit at the same address)
    auto dd = inst->getDigital();
    if (!dd || inst->digital_serial.load() != q.dec) return;
    if (searching()) {   // (the search owns the receiver: its probes' reports are its own, nothing else counts)
        if (is_probe(q.vfo)) srch_event(q.vfo, q.ev);
        else g_srch_dropped++;
        return;
    }
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
        case PluginEvent::SYSTEM:
            on_system(q.vfo, t, q.plugin, ev);
            if (ev.control && !voice_of_vfo(q.vfo)) cc_heard(q.vfo, q.plugin, ev);
            break;
        case PluginEvent::CHANNEL:
            if (!voice_of_vfo(q.vfo)) on_channel(q.vfo, t, q.plugin, ev);
            break;
        case PluginEvent::CONTROL:
            if (!voice_of_vfo(q.vfo)) on_control(q.vfo, t, ev);
            break;
        case PluginEvent::GRANT:
            g_on_grant = true;
            on_grant(q.vfo, t, q.plugin, ev, q.t_rx);
            g_on_grant = false;
            break;
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

// at most this many events per drain: a flood can't hold the loop - the rest
// waits for the next turn (commands, sockets run in between), the queue's bound
// drops what comes meanwhile
constexpr size_t DRAIN_MAX = 2000;
void drain() {
    std::deque<QEv> q;
    bool more = false;
    {
        std::lock_guard<std::mutex> lk(g_qmu);
        if (g_queue.size() <= DRAIN_MAX) {
            q.swap(g_queue);
        } else {
            for (size_t i = 0; i < DRAIN_MAX; i++) {
                q.push_back(std::move(g_queue.front()));
                g_queue.pop_front();
            }
            more = true;
        }
        g_drain_posted = more;
    }
    if (more && loop) loop->defer([] { drain(); });
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

// What survives a restart: the systems the user edited, and - while following
// is on - the keyed ones heard; of each: the names, rules, the talkgroups the
// user listed / named / gave a mode, the channels with a frequency (user,
// announced, learned - with their grant count and last time as the
// evidence). Not: heard-only talkgroups, channels never resolved, units.
std::string serialise() {
    std::ostringstream o;
    o << "{\"version\":1,\"systems\":[";
    bool first = true;
    for (const auto& kv : g_sys) {
        const System& s = kv.second;
        if (temp_id(s.id) || !(s.user || (g_set.follow && s.heard))) continue;
        o << (first ? "\n " : ",\n ") << "{\"id\":" << js(s.id) << ",\"plugin\":" << js(s.plugin) << ",\"key\":" << js(s.key)
          << ",\"name\":" << js(s.user_name) << ",\"reported_name\":" << js(s.name) << ",\"listed_only\":" << jb(s.listed_only)
          << ",\"heard\":" << jb(s.heard)
          << ",\"tgs\":[";
        first = false;
        bool f1 = true;
        for (const auto& t : s.tgs) {
            if (!t.second.listed && t.second.name.empty() && t.second.mode == TG_NORMAL) continue;
            o << (f1 ? "" : ",") << "{\"id\":" << js(t.first) << ",\"name\":" << js(t.second.name) << ",\"mode\":\""
              << tg_mode_name(t.second.mode) << "\",\"listed\":" << jb(t.second.listed) << ",\"last\":" << t.second.last
              << ",\"calls\":" << t.second.calls << "}";
            f1 = false;
        }
        o << "],\"chans\":[";
        f1 = true;
        for (const auto& c : s.chans) {
            if (!fin(c.second.user_hz) && !fin(c.second.ann_hz) && !fin(c.second.learned_hz)) continue;
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
    if (text.size() > SYSTEMS_FILE_MAX) {
        g_file_note = "not written: " + std::to_string(text.size() >> 20) + " MB is over the " +
                      std::to_string(SYSTEMS_FILE_MAX >> 20) + " MB a start reads - the file keeps the last good state";
        std::cerr << "Trunk following: " << SYSTEMS_FILE << " " << g_file_note << std::endl;
        return false;
    }
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
        if (g_shutdown) return;
        write_file(text);
    }
    g_dirty_since = 0;
    g_edit_dirty = false;
}

// a JSON number as an integer in [lo, hi] (no cast of an out-of-range double)
bool as_int(double v, long long lo, long long hi, long long* out) {
    if (!fin(v) || v != std::floor(v) || v < static_cast<double>(lo) || v > static_cast<double>(hi)) return false;
    *out = static_cast<long long>(v);
    return true;
}
long long int_or(double v, long long lo, long long hi, long long def) {
    long long o;
    return as_int(v, lo, hi, &o) ? o : def;
}

// a rule's values (the command and the file): "" = fine
std::string check_rule(Rule* r, double first, double last) {
    if (!as_int(first, 0, 1000000000, &r->first) || !as_int(last, 0, 1000000000, &r->last) || r->last < r->first)
        return "first / last: channel numbers, 0 <= first <= last <= 1e9";
    if (!(fin(r->base_hz) && r->base_hz > 0 && r->base_hz <= 6e9) || !fin(r->step_hz) || std::fabs(r->step_hz) > 10e6 ||
        r->step_hz == 0)
        return "base_hz: 0 - 6 GHz; step_hz: not 0, at most 10 MHz";
    const double top = r->base_hz + static_cast<double>(r->last - r->first) * r->step_hz;
    if (!(fin(top) && top > 0 && top <= 6e9)) return "the rule's last channel is outside 0 - 6 GHz";
    if (!std::isnan(r->up_off_hz) && (!fin(r->up_off_hz) || std::fabs(r->up_off_hz) > 1e9))
        return "up_off_hz: the uplink offset in Hz, or null";
    return "";
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

std::string exc_status_json(int64_t now);
bool exc_parked(int vfo);
std::string vt_status_json();
std::string srch_status_json(int64_t now);
std::string build_status(bool ages) {
    const int64_t now = now_ms();
    std::ostringstream o;
    char hang[16];
    snprintf(hang, sizeof hang, "%.1f", g_set.hang_s);
    const int mode = operating_mode_index();
    o << "{\"trunk\":{\"systems_file\":{\"ok\":" << jb(g_file_note.empty()) << ",\"note\":" << js(g_file_note)
      << ",\"kept_as\":" << js(g_file_bad) << "},\"sys_refused\":" << g_sys_refused << ",\"follow\":" << jb(g_set.follow) << ",\"all_sites\":" << jb(g_set.all_sites) << ",\"max_vfos\":" << g_set.max_vfos << ",\"encrypted\":" << jb(g_set.enc)
      << ",\"unit\":" << jb(g_set.unit) << ",\"audio\":" << jb(g_set.audio) << ",\"hang_s\":" << hang
      << ",\"retune\":" << jb(g_set.retune) << ",\"retune_cal\":" << jb(g_set.retune_cal)
      << ",\"retune_max_s\":" << static_cast<long long>(std::llround(g_set.retune_max_s)) << "," << exc_status_json(now)
      << "," << vt_status_json() << "," << srch_status_json(now)
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
              << ",\"rx\":" << jb(now - c.second.last_ms < CONTROL_LOST_MS) << ",\"parked\":" << jb(exc_parked(c.first))
              << ",\"site\":" << js(c.second.site)
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
      << ",\"resets_coalesced\":" << g_resets_coalesced.load() << ",\"options_dropped\":" << g_opts_dropped
      << ",\"untrunking_dropped\":" << g_untrunking << ",\"stale_reports\":" << g_stale_reports
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

// The status is built only when something changed (g_status_now - at most every
// STATUS_MIN_MS) or every STATUS_ACTIVE_MS while anything is active (a call, a
// voice VFO, a control VFO); a change that leaves the body as sent sends
// nothing - but a command (GET_TRUNK, every TRUNK_*) always gets it (answer)
void send_status(bool force, bool answer = false) {
    const int64_t now = now_ms();
    bool active = !g_calls.empty() || !g_voices.empty();
    if (searching()) active = true;   // (whatever following is: the page shows its progress)
    if (!active)
        for (const auto& kv : g_sys)
            if (!kv.second.ctl.empty()) {
                active = true;
                break;
            }
    const bool periodic = active && now - g_status_sent >= STATUS_ACTIVE_MS;
    if (!force && !periodic) return;
    const std::string body = build_status(false);
    if (!periodic && !answer && body == g_status_last) {   // (a command / GET_TRUNK is always answered: a page that connected later)
        g_status_now = false;
        return;
    }
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
    if (!g_set.follow) g_pending.clear();
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
        const bool was = g_dispatching;
        g_dispatching = true;   // (not saved: the user's saved tuning stays theirs)
        for (const auto& c : cmds) {
            const std::string why = ControlHandler::apply_command(c);
            if (!why.empty()) err = "'" + c + "' refused: " + why;
        }
        g_dispatching = was;
        note_restore(vfo, m, err.empty(), err.empty() ? "put back on " + num_json(m.rf / 1e6, "%.6f") + " MHz after the " +
                                                          g_pause_why_last : err);
        it = g_pending.erase(it);
    }
}

// ============================================================================
// Coherent mode: an excursion to a call outside the span (TRUNK_RETUNE)
// ============================================================================
// A grant that passes every rule but is out_of_span makes the receiver leave:
// the centre goes where the channel (and, if it can, a control VFO of that
// system) is in the usable band, heimdall calibrates at once (a calibration
// hold over the retune, released right after it: the pending cooldown is
// backdated), the call is followed there; when it is over the receiver goes
// home ("home" = the centre it had, which is also what is saved - none of this
// is), calibrates at once again and picks the control channel up again.

constexpr int64_t EXC_HOLD_LEASE_MS = 3000;   // the calibration hold's lease ...
constexpr int64_t EXC_HOLD_RENEW_MS = 1000;   // ... renewed this often while held for the whole excursion
constexpr int64_t EXC_RETRY_MS = 30000;       // a call that sent the receiver out for nothing (no_signal / the limit): not again this soon
constexpr int64_t EXC_BACK_MAX_MS = 20000;    // after the return: waiting for the control channel at most this long
constexpr double EXC_OFFSET_HZ = 250000;      // no control VFO fits: the channel this far from the centre (towards home)
constexpr double EXC_DC_GUARD_HZ = 30000;     // a channel (voice or control) at least this far from the centre (DC, LO leakage)

int g_exc_fail = 0;              // excursions in a row that never arrived
int64_t g_exc_block_until = 0;   // ... no excursion before then (growing)
std::atomic<int64_t> g_packet_ms{0};   // the latest packet from heimdall (steady ms; the data thread)

bool heimdall_up() { return now_ms() - g_packet_ms.load(std::memory_order_relaxed) < 2000; }

// a scanner runs (tested directly: g_paused follows them only once a tick)
bool scanning() { return sfscan::running() || scanner_manager.isRunning() || continuous_scanner.isRunning(); }

// The control VFOs for the retunes: every VFO not a voice VFO whose decoder is
// a trunking plugin - also one that has not reported a system yet, or whose
// entry a RESET erased until it reports again
std::vector<int> control_vfos() {
    std::vector<int> v;
    for (const auto& d : decimator_manager.getDecimatorInfoList()) {
        if (voice_of_vfo(d.id) || is_trunk_vfo(d.id)) continue;
        if (d.digital_mode != static_cast<int>(dig::Mode::PLUGIN) || !plugin_trunking(d.digital_plugin)) continue;
        v.push_back(d.id);
    }
    return v;
}

// coherent mode: the centre asked for and the stream's differ (a retune of
// ours - or anyone's - not there yet), or an excursion not arrived yet
bool exc_in_transit() {
    if (operating_mode_index() != 0) return false;
    if (g_exc.st == 1 && !g_exc.t_arrived) return true;
    const double req = static_cast<double>(requested_freq_hz.load()), hdr = static_cast<double>(tuner_frequencies[0].load());
    return g_exc.st != 0 && req > 0 && hdr > 0 && std::fabs(req - hdr) > 1000;
}

void heimdall_hold(int ms) {
    char c[64];
    snprintf(c, sizeof c, "{\"hold_calibration\":{\"hold_ms\":%d}}", ms);
    ControlHandler::send_control_command(c);
}

// a VFO's RF in coherent mode from the centre asked for (the stream's header
// may still show the previous one)
double coh_rf(const Inst& i) {
    const double c = static_cast<double>(requested_freq_hz.load(std::memory_order_relaxed));
    return c > 0 ? c + i.frequency_offset_hz.load() : 0.0;
}

bool exc_calibrated() {
    if (server_kerberos_mode.load(std::memory_order_relaxed)) return true;
    return (scanner_manager.getPhaseState() & 0xFF) == 4 && !scanner_manager.isNoiseSourceActive();
}

bool exc_range(double c) {
    if (wb_variant_enabled.load(std::memory_order_relaxed)) {
        uint64_t lo, hi;
        wb_variant_rf_union_range(lo, hi);
        return c >= static_cast<double>(lo) && c <= static_cast<double>(hi);
    }
    return c >= static_cast<double>(RTL_TUNER_MIN_HZ) && c <= static_cast<double>(RTL_TUNER_MAX_HZ);
}

// The away centre for channel hz (bw wide): one that also holds a control VFO
// of system s - the nearest home -, else the channel EXC_OFFSET_HZ from the
// centre on the side towards home. 0 = none (outside the tuner range)
double exc_centre(const System& s, double hz, double bw, double home, std::string* how) {
    const double half = usable_half_hz();
    const double need = bw / 2 + PLACE_GUARD_HZ;
    if (need + EXC_DC_GUARD_HZ > half) return 0;
    const double lo = hz - (half - need), hi = hz + (half - need);
    double best = 0;
    for (const auto& kv : s.ctl) {
        auto inst = decimator_manager.getDecimator(kv.first);
        if (!inst) continue;
        const double r = coh_rf(*inst);
        if (r <= 0) continue;
        const double cn = inst->bandwidth_hz.load() / 2.0 + PLACE_GUARD_HZ;
        const double clo = std::max(lo, r - (half - cn)), chi = std::min(hi, r + (half - cn));
        if (clo > chi) continue;
        // the point of [clo, chi] nearest home whose DC is off both channels
        const double c0 = std::clamp(home, clo, chi);
        for (int k = 0; k <= 400; k++) {
            bool found = false;
            for (int sgn : {1, -1}) {
                const double c = c0 + sgn * k * 5000.0;
                if (c < clo || c > chi || !exc_range(c)) continue;
                if (std::fabs(hz - c) < EXC_DC_GUARD_HZ || std::fabs(r - c) < EXC_DC_GUARD_HZ) continue;
                if (best == 0 || std::fabs(c - home) < std::fabs(best - home)) best = c;
                found = true;
            }
            if (found) break;
        }
    }
    if (best > 0) {
        *how = "control";
        return std::round(best);
    }
    const double off = std::min(EXC_OFFSET_HZ, half - need);
    const double c = hz + (home >= hz ? off : -off);
    if (!exc_range(c)) return 0;
    *how = "offset";
    return std::round(c);
}

std::string exc_status_json(int64_t now) {
    std::ostringstream o;
    o << "\"excursion\":";
    if (g_exc.st == 0) {
        o << "null";
    } else {
        const char* ret = "";
        if (g_exc.st == 2) ret = "returning";
        o << "{\"state\":\"" << (g_exc.st == 1 ? (g_exc.t_arrived ? "away" : "leaving") : ret) << "\""
          << ",\"since\":" << g_exc.w_left << ",\"away_ms\":" << (now - g_exc.t_left) << ",\"call\":" << g_exc.call
          << ",\"sys\":" << js(g_exc.sys) << ",\"target\":" << js(g_exc.target) << ",\"ch\":" << js(g_exc.ch)
          << ",\"hz\":" << num_json(g_exc.hz) << ",\"centre_hz\":" << num_json(g_exc.centre)
          << ",\"home_hz\":" << num_json(g_exc.home) << ",\"how\":" << js(g_exc.centre_how)
          << ",\"calibration\":\"" << (g_exc.kerberos ? "manual" : !g_exc.cal && g_exc.st == 1 ? "held" : exc_calibrated() ? "done" : "running")
          << "\",\"end\":" << js(g_exc.end);
        int64_t left = -1;
        if (g_exc.st == 1 && g_set.retune_max_s > 0) left = std::max<int64_t>(0, static_cast<int64_t>(g_set.retune_max_s * 1000) - (now - g_exc.t_left));
        o << ",\"limit_ms\":" << left << ",\"ctl\":[";
        for (size_t i = 0; i < g_exc.ctl.size(); i++)
            o << (i ? "," : "") << "{\"vfo\":" << g_exc.ctl[i].vfo << ",\"parked\":" << jb(g_exc.ctl[i].parked) << "}";
        o << "]}";
    }
    o << ",\"excursions\":{\"n\":" << g_exc_stats.n << ",\"away_ms\":" << g_exc_stats.away_ms << ",\"ends\":{";
    bool f = true;
    for (const auto& kv : g_exc_stats.ends) {
        o << (f ? "" : ",") << js(kv.first) << ":" << kv.second;
        f = false;
    }
    o << "},\"calls_away\":" << g_exc_stats.calls_away << ",\"calls_missed\":" << g_exc_stats.calls_missed
      << ",\"last\":{\"arrive_ms\":" << g_exc_stats.arrive_ms << ",\"cal_ms\":" << g_exc_stats.cal_ms
      << ",\"first_frames_ms\":" << g_exc_stats.first_frames_ms << ",\"back_arrive_ms\":" << g_exc_stats.back_arrive_ms
      << ",\"back_cal_ms\":" << g_exc_stats.back_cal_ms << ",\"ctl_back_ms\":" << g_exc_stats.ctl_back_ms << "}}";
    return o.str();
}

// the control VFOs of every system, where they are now (home)
std::vector<ExcCtl> exc_note_ctl() {
    std::vector<ExcCtl> v;
    for (int id : control_vfos()) {
        auto inst = decimator_manager.getDecimator(id);
        if (!inst) continue;
        ExcCtl e;
        e.vfo = id;
        e.serial = inst->serial;
        e.off = inst->frequency_offset_hz.load();
        e.rf = coh_rf(*inst);
        v.push_back(e);
    }
    return v;
}
bool exc_parked(int vfo) {
    if (g_exc.st != 1) return false;
    for (const auto& e : g_exc.ctl)
        if (e.vfo == vfo && e.parked) return true;
    return false;
}
// the VFO an ExcCtl remembers, if it is still the same instance
std::shared_ptr<Inst> exc_inst(const ExcCtl& e) {
    auto inst = decimator_manager.getDecimator(e.vfo);
    return inst && inst->serial == e.serial ? inst : nullptr;
}

void exc_expect_resets(int64_t ms) {
    const int64_t until = now_ms() + ms;
    for (const auto& e : g_exc.ctl)
        if (exc_inst(e)) g_tracks[e.vfo].expect_reset_until = std::max(g_tracks[e.vfo].expect_reset_until, until);
}

// The receiver to centre c, each control VFO kept on its RF where it fits the
// new span (else parked), or (home) back where it was - through the
// dispatching guard: nothing of it is saved. cal: hold -> retune -> release
// (the calibration at once); hold: keep the hold on (no calibration)
void exc_retune(double c, bool home, bool cal, bool hold) {
    const int64_t now = now_ms();
    if (!g_exc.kerberos) {
        if (cal || hold) {
            heimdall_hold(static_cast<int>(EXC_HOLD_LEASE_MS));
            g_exc.holding = true;
            g_exc.hold_sent = now;
        }
    }
    char f[64];
    snprintf(f, sizeof f, "FREQ:%.6f", c / 1e6);
    std::vector<std::string> cmds{f};
    // (the Wideband downconverter: the user's mixer side again at home, if it reaches home)
    if (home && g_exc.side >= 0 && wb_variant_mixer_side.load() != g_exc.side &&
        wb_side_valid(g_exc.side, static_cast<uint64_t>(c)))
        cmds.push_back(std::string("MIXER_SIDE:") + wb_mixer_side_name(g_exc.side));
    const double half = usable_half_hz();
    for (auto& e : g_exc.ctl) {
        auto inst = exc_inst(e);
        if (!inst) continue;
        char m[80];
        if (home) {
            snprintf(m, sizeof m, "SET_DECIMATOR_FREQ:%d:%.3f", e.vfo, e.off / 1000.0);
            cmds.push_back(m);
            continue;
        }
        const double cn = inst->bandwidth_hz.load() / 2.0 + PLACE_GUARD_HZ;
        e.parked = !(e.rf > 0 && std::fabs(e.rf - c) + cn <= half);
        if (!e.parked) {
            snprintf(m, sizeof m, "SET_DECIMATOR_FREQ:%d:%.3f", e.vfo, (e.rf - c) / 1000.0);
            cmds.push_back(m);
        }
    }
    exc_expect_resets(10000);
    dispatch(cmds);
    for (auto& e : g_exc.ctl) {
        auto inst = exc_inst(e);
        if (!inst) continue;
        inst->enabled = home || !e.parked;   // a parked control VFO hears nothing (no bogus SYSTEM / grants)
        if (home) e.parked = false;
    }
    if (!g_exc.kerberos && cal) {
        heimdall_hold(0);   // a hold in effect -> the pending cooldown backdated: the calibration starts now
        g_exc.holding = false;
    }
    broadcast(MessageBuilders::build_decimator_info_message());
}

void exc_return(const std::string& why, bool retune) {
    if (g_exc.st != 1) return;
    const int64_t now = now_ms();
    std::cout << "Trunk following: back home to " << g_exc.home / 1e6 << " MHz (" << why << ") after "
              << (now - g_exc.t_left) / 1000.0 << " s away" << std::endl;
    // what is still followed out there ends
    std::vector<uint64_t> ids;
    for (const auto& kv : g_calls)
        if (kv.second.vfo >= 0 && kv.second.st != RECENT) ids.push_back(kv.first);
    for (uint64_t id : ids) end_call(id, "returned");
    g_exc_stats.ends[why]++;
    g_exc_stats.away_ms += now - g_exc.t_left;
    g_exc.end = why;
    if (why == "no_signal" || why == "limit") g_exc_retry[g_exc.key] = now + EXC_RETRY_MS;
    if (why == "not_arrived") {
        // (heimdall refusing / busy / gone: every call waits, 30 s doubling to 8 min)
        g_exc_fail = std::min(g_exc_fail + 1, 5);
        g_exc_block_until = now + (EXC_RETRY_MS << (g_exc_fail - 1));
    } else if (g_exc.t_arrived) {
        g_exc_fail = 0;
    }
    if (retune && !playback::active()) {
        exc_retune(g_exc.home, true, true, false);
        g_exc.st = 2;
        g_exc.t_back = now;
        g_exc.t_back_arr = g_exc.t_back_cal = 0;
    } else {
        // (the user retuned it / a playback: no retune - the VFOs as they were, relative to the centre)
        std::vector<std::string> cmds;
        for (auto& e : g_exc.ctl) {
            auto inst = exc_inst(e);
            if (!inst) continue;
            char m[80];
            snprintf(m, sizeof m, "SET_DECIMATOR_FREQ:%d:%.3f", e.vfo, e.off / 1000.0);
            if (!playback::active()) cmds.push_back(m);
            inst->enabled = true;
            e.parked = false;
        }
        exc_expect_resets(10000);
        dispatch(cmds);
        if (g_exc.holding && !g_exc.kerberos && !playback::active()) heimdall_hold(0);
        g_exc.holding = false;
        g_exc.st = 0;
        broadcast(MessageBuilders::build_decimator_info_message());
    }
    g_status_now = true;
}

// try_follow: call c's channel hz is outside the span. "" never; the reason it
// waits: "leaving" (the receiver goes there now / is on its way), or why not
std::string exc_try(Call& c, double hz, double bw) {
    if (!g_set.retune || operating_mode_index() != 0 || playback::active() || g_paused || scanning() || searching() || !g_set.follow)
        return "out_of_span";
    const int64_t now = now_ms();
    if (!heimdall_up() || now < g_exc_block_until) return "no_retune";
    if (g_exc.st == 1) {
        if (c.id == g_exc.call) return "leaving";
        if (c.why != "away") g_exc_stats.calls_missed++;
        return "away";
    }
    auto r = g_exc_retry.find(c.key);
    if (r != g_exc_retry.end()) {
        if (now < r->second) return "retry";
        g_exc_retry.erase(r);
    }
    // a followed call in progress here: only a priority / held talkgroup's call leaves it (the pre-emption ranking)
    const bool held = !g_hold_sys.empty() && g_hold_sys == c.sys && g_hold_tg == c.target;
    std::vector<uint64_t> here;
    for (const auto& kv : g_calls)
        if (kv.second.vfo >= 0 && kv.second.st != RECENT) {
            if (!held && rank(kv.second) >= rank(c)) return "busy_here";
            here.push_back(kv.first);
        }
    System* s = sys_of(c.sys);
    if (!s) return "out_of_span";
    const double home = static_cast<double>(g_exc.st == 2 ? static_cast<uint64_t>(g_exc.home) : requested_freq_hz.load());
    if (home <= 0) return "out_of_span";
    std::string how;
    const double cen = exc_centre(*s, hz, bw, home, &how);
    if (cen <= 0) return "no_retune";
    for (uint64_t id : here) end_call(id, "excursion");
    // (a return still settling: its home stands, its control VFOs are home already)
    g_exc = Exc{};
    g_exc.st = 1;
    g_exc.call = c.id;
    g_exc.key = c.key;
    g_exc.sys = c.sys;
    g_exc.target = c.target;
    g_exc.ch = c.ch.name;
    g_exc.decoder = c.decoder;
    g_exc.hz = hz;
    g_exc.home = home;
    g_exc.centre = cen;
    g_exc.centre_how = how;
    g_exc.t_left = now;
    g_exc.w_left = wall_ms();
    g_exc.cal = g_set.retune_cal;
    g_exc.kerberos = server_kerberos_mode.load(std::memory_order_relaxed);
    g_exc.side = wb_variant_enabled.load() ? wb_variant_mixer_side.load() : -1;
    g_exc.ctl = exc_note_ctl();
    g_exc_stats.n++;
    g_exc_stats.arrive_ms = g_exc_stats.cal_ms = g_exc_stats.first_frames_ms = -1;
    std::cout << "Trunk following: leaving " << home / 1e6 << " MHz for " << c.sys << " " << c.target << " on " << c.ch.name
              << " (" << hz / 1e6 << " MHz): centre " << cen / 1e6 << " MHz (" << how << ")" << std::endl;
    exc_retune(cen, false, g_exc.cal, !g_exc.cal);
    g_status_now = true;
    return "leaving";
}

// ============================================================================
// Independent mode: voice tuners (TRUNK_TUNERS)
// ============================================================================
// The tuners the user lets the manager retune. A grant no tuner's span holds
// goes to a free voice tuner: retuned so the channel is in its usable band,
// the voice VFO placed once the stream shows it there. A tuner carrying a
// control VFO is never retuned. Each voice tuner goes back to the frequency
// and gain it had when the manager first took it when the manager lets go of
// it (following / the list off, a mode change, a scanner, a playback).

struct VTuner {
    bool taken = false;     // the manager has retuned it (home_* valid)
    double home_hz = 0;
    float home_gain = -999.0f;
    double hz = 0;          // where the manager put it
    uint64_t call = 0;      // retuned for this call, waiting for the stream (0 = none)
    int64_t t_set = 0;      // when (steady)
    int64_t tune_ms = -1;   // the latest retune -> the stream there
    float gain = -999.0f;   // the gain the manager left it at (another one: the user's - his again)
};
VTuner g_vt[MAX_CHANNELS];

bool vt_listed(int t) { return std::find(g_set.tuners.begin(), g_set.tuners.end(), t) != g_set.tuners.end(); }

std::set<int> vt_control_tuners() {
    std::set<int> c;
    for (int id : control_vfos())
        if (auto inst = decimator_manager.getDecimator(id)) c.insert(inst->tuner_channel.load());
    return c;
}

// the tuner's followed calls in progress (or the one it is being retuned for)
bool vt_busy(int t, uint64_t* victim, int rank_below) {
    bool busy = g_vt[t].call != 0;
    int64_t oldest = 0;
    for (const auto& kv : g_calls) {
        const Call& c = kv.second;
        if (c.vfo < 0 || c.st == RECENT) continue;
        auto inst = decimator_manager.getDecimator(c.vfo);
        if (!inst || inst->tuner_channel.load() != t) continue;
        busy = true;
        if (victim) {
            if (rank(c) >= rank_below) {
                *victim = UINT64_MAX;   // (a call that ranks as high: the tuner can't be taken)
            } else if (*victim != UINT64_MAX && (!*victim || c.t_start < oldest)) {
                *victim = c.id;
                oldest = c.t_start;
            }
        }
    }
    if (victim && *victim == UINT64_MAX) *victim = 0;
    return busy;
}

// the voice tuner's centre for channel hz: off DC (>= 30 kHz), holding as many
// of the system's known channels as it can (weighted by their grants), else
// the channel 250 kHz from the centre
double vt_centre(const System& s, double hz, double bw) {
    const double half = usable_half_hz();
    const double need = bw / 2 + PLACE_GUARD_HZ;
    const double lim = half - need;
    if (lim < EXC_DC_GUARD_HZ) return 0;
    std::vector<std::pair<double, double>> known;   // (hz, weight)
    for (const auto& kv : s.chans) {
        double f, up;
        std::string src;
        Channel ch;
        ch.name = kv.first;
        ch.number = kv.second.num;
        if (resolve(s, ch, &f, &up, &src) && std::fabs(f - hz) < 2 * half) known.push_back({f, 1.0 + static_cast<double>(kv.second.grants)});
    }
    double best = 0, bs = -1;
    for (double d = -lim; d <= lim; d += 10000.0) {
        if (std::fabs(d) < EXC_DC_GUARD_HZ) continue;
        const double c = hz + d;
        if (!exc_range(c)) continue;
        double sc = 0;
        for (const auto& k : known)
            if (std::fabs(k.first - c) + need <= half && std::fabs(k.first - c) >= EXC_DC_GUARD_HZ) sc += k.second;
        // (a tie: the channel nearest 250 kHz off the centre)
        sc -= std::fabs(std::fabs(d) - EXC_OFFSET_HZ) * 1e-9;
        if (sc > bs) {
            bs = sc;
            best = c;
        }
    }
    return std::round(best);
}

void vt_take(int t, double c, const std::string& why_log) {
    VTuner& v = g_vt[t];
    if (!v.taken) {
        v.taken = true;
        v.home_hz = indep_tuner_freq_hz[t].load();
        if (v.home_hz <= 0) v.home_hz = static_cast<double>(exact_tuner_hz(t));
        v.home_gain = indep_tuner_gain_db[t].load();
    }
    v.hz = c;
    v.t_set = now_ms();
    v.tune_ms = -1;
    v.gain = indep_tuner_gain_db[t].load();
    char f[64];
    snprintf(f, sizeof f, "TUNER_FREQ:%d:%.6f", t, c / 1e6);
    std::vector<std::string> cmds{f};
    // the control tuner's gain (the same band): the first control tuner's
    const std::set<int> ct = vt_control_tuners();
    if (!ct.empty()) {
        const float g = indep_tuner_gain_db[*ct.begin()].load();
        if (g != -999.0f && g != indep_tuner_gain_db[t].load()) {
            char m[64];
            snprintf(m, sizeof m, "TUNER_GAIN:%d:%.1f", t, g);
            cmds.push_back(m);
        }
    }
    dispatch(cmds);
    v.gain = indep_tuner_gain_db[t].load();
    std::cout << "Trunk following: voice tuner " << t << " to " << c / 1e6 << " MHz (" << why_log << ")" << std::endl;
    g_status_now = true;
}

// the voice tuner back where the manager found it (no: the user took it)
void vt_restore(int t, const std::string& why, bool retune) {
    VTuner& v = g_vt[t];
    if (!v.taken) return;
    std::vector<uint64_t> ids;
    for (const auto& kv : g_calls) {
        const Call& c = kv.second;
        if (c.vfo < 0 || c.st == RECENT) continue;
        auto inst = decimator_manager.getDecimator(c.vfo);
        if (inst && inst->tuner_channel.load() == t) ids.push_back(c.id);
    }
    for (uint64_t id : ids) end_call(id, "returned");
    if (retune && operating_mode_index() == 2 && !playback::active()) {
        std::vector<std::string> cmds;
        char f[64];
        if (v.home_hz > 0) {
            snprintf(f, sizeof f, "TUNER_FREQ:%d:%.6f", t, v.home_hz / 1e6);
            cmds.push_back(f);
        }
        if (v.home_gain != -999.0f && v.home_gain != indep_tuner_gain_db[t].load()) {
            snprintf(f, sizeof f, "TUNER_GAIN:%d:%.1f", t, v.home_gain);
            cmds.push_back(f);
        }
        dispatch(cmds);
        std::cout << "Trunk following: voice tuner " << t << " back to " << v.home_hz / 1e6 << " MHz (" << why << ")" << std::endl;
    } else if (v.home_hz > 0) {
        // (no command now - a mode change / playback: the remembered tuning is the user's, replayed at the next switch)
        indep_tuner_freq_hz[t] = v.home_hz;
        indep_tuner_gain_db[t] = v.home_gain;
    }
    v = VTuner{};
    g_status_now = true;
}

void vt_restore_all(const std::string& why, bool retune) {
    for (int t = 0; t < MAX_CHANNELS; t++) vt_restore(t, why, retune);
}

// try_follow, independent mode: call c's channel hz is in no tuner's span
std::string vt_try(Call& c, double hz, double bw) {
    if (g_set.tuners.empty() || operating_mode_index() != 2 || playback::active() || g_paused || scanning() || searching() || !g_set.follow)
        return "out_of_span";
    if (!heimdall_up()) return "no_retune";
    for (int t = 0; t < MAX_CHANNELS; t++)
        if (g_vt[t].call == c.id) return "tuning";
    System* s = sys_of(c.sys);
    if (!s) return "out_of_span";
    const std::set<int> ct = vt_control_tuners();
    const int n = n_tuners();
    int free_t = -1;
    uint64_t victim = 0;
    int victim_t = -1;
    for (int t : g_set.tuners) {
        if (t < 0 || t >= n || ct.count(t)) continue;
        uint64_t v = 0;
        if (!vt_busy(t, rank(c) > 0 ? &v : nullptr, rank(c))) {
            // (one the manager holds already first: the user's other tuners stay where they are longer)
            if (free_t < 0 || (g_vt[t].taken && !g_vt[free_t].taken)) free_t = t;
        } else if (v && !victim) {
            victim = v;
            victim_t = t;
        }
    }
    if (free_t < 0 && !victim) {
        // (no listed tuner can carry voice at all - every one a control tuner / missing)
        bool usable = false;
        for (int t : g_set.tuners)
            if (t >= 0 && t < n && !ct.count(t)) usable = true;
        return usable ? "no_tuner" : "out_of_span";
    }
    const double cen = vt_centre(*s, hz, bw);
    if (cen <= 0) return "no_retune";
    if (free_t < 0 && victim) {
        auto vi = g_calls.find(victim);
        std::cout << "Trunk following: " << c.sys << " TG " << c.target << " (priority) takes voice tuner " << victim_t << " from "
                  << vi->second.target << std::endl;
        s->preempted++;
        end_call(victim, "preempted");
        free_t = victim_t;
    }
    vt_take(free_t, cen, c.sys + " " + c.target + " on " + c.ch.name);
    g_vt[free_t].call = c.id;
    return "tuning";
}

void tick_vt() {
    const int64_t now = now_ms();
    const bool any = std::any_of(std::begin(g_vt), std::end(g_vt), [](const VTuner& v) { return v.taken; });
    if (!any) return;
    if (playback::active()) return vt_restore_all("playback", false);
    if (operating_mode_index() != 2) return vt_restore_all("mode", false);
    if (!g_set.follow) return vt_restore_all("off", true);
    if (g_paused) return vt_restore_all("scanner", true);
    const std::set<int> ct = vt_control_tuners();
    for (int t = 0; t < MAX_CHANNELS; t++) {
        VTuner& v = g_vt[t];
        if (!v.taken) continue;
        if (!vt_listed(t) || ct.count(t)) {
            vt_restore(t, ct.count(t) ? "a control VFO is on it" : "not a voice tuner any more", true);
            continue;
        }
        // the user retuned it (or changed its gain): his now (no restore)
        if (std::fabs(indep_tuner_freq_hz[t].load() - v.hz) > 1000 || indep_tuner_gain_db[t].load() != v.gain) {
            std::cout << "Trunk following: voice tuner " << t << " retuned by the user - his again" << std::endl;
            v = VTuner{};
            g_status_now = true;
            continue;
        }
        if (!v.call) continue;
        if (std::fabs(static_cast<double>(exact_tuner_hz(t)) - v.hz) < 1000 && std::fabs(tuner_frequencies[t].load() - v.hz) < 1000) {
            v.tune_ms = now - v.t_set;
            const uint64_t id = v.call;
            v.call = 0;
            auto it = g_calls.find(id);
            if (it != g_calls.end() && it->second.vfo < 0 && it->second.st != RECENT) try_follow(it->second);
            g_status_now = true;
        } else if (now - v.t_set > 5000) {
            v.call = 0;   // (the stream never showed it: the call's next grant tries again)
        }
    }
}

std::string vt_status_json() {
    std::ostringstream o;
    o << "\"tuners\":[";
    for (size_t i = 0; i < g_set.tuners.size(); i++) o << (i ? "," : "") << g_set.tuners[i];
    o << "],\"voice_tuners\":[";
    bool f = true;
    const std::set<int> ct = vt_control_tuners();
    for (int t = 0; t < MAX_CHANNELS; t++) {
        const VTuner& v = g_vt[t];
        if (!vt_listed(t) && !v.taken) continue;
        int calls = 0;
        for (const auto& kv : g_calls) {
            const Call& c = kv.second;
            if (c.vfo < 0 || c.st == RECENT) continue;
            auto inst = decimator_manager.getDecimator(c.vfo);
            if (inst && inst->tuner_channel.load() == t) calls++;
        }
        o << (f ? "" : ",") << "{\"tuner\":" << t << ",\"control\":" << jb(ct.count(t) > 0) << ",\"taken\":" << jb(v.taken)
          << ",\"hz\":" << num_json(static_cast<double>(exact_tuner_hz(t))) << ",\"home_hz\":" << num_json(v.taken ? v.home_hz : NAN)
          << ",\"home_gain\":" << num_json(v.taken && v.home_gain != -999.0f ? v.home_gain : NAN) << ",\"calls\":" << calls
          << ",\"tuning_for\":" << v.call << ",\"tune_ms\":" << v.tune_ms << "}";
        f = false;
    }
    o << "]";
    return o.str();
}

// ============================================================================
// Control channels: the search, the memory (trunk_control_channels.json),
// click to follow
// ============================================================================

constexpr const char* CC_FILE = "trunk_control_channels.json";
constexpr size_t CC_MAX = 1000;               // memory entries (the oldest the air brought, never named / followed, go)
constexpr int64_t CC_SAVE_MS = 5000;          // saved this long after a change
constexpr int64_t CC_LIST_MS = 1000;          // the list message at most this often
constexpr int64_t CC_HEARD_MS = 60000;        // a control VFO in normal use refreshes its entry at most this often
constexpr int64_t SRCH_ARRIVE_MAX_MS = 4000;  // a step whose centre the stream never shows: skipped
constexpr int64_t SRCH_PROBE_MIN_MS = 2500;   // a probe without any valid frame this long (stream time): given up ...
constexpr int64_t SRCH_PROBE_MAX_MS = 5000;   // ... and none longer (valid frames but no control report by then: traffic)
constexpr int64_t SRCH_KEY_WAIT_MS = 2500;    // control = true without a key yet: the key waited for this long
constexpr double SRCH_OVERLAP_HZ = 50000;     // neighbouring steps' usable bands overlap this much
constexpr double SRCH_MIN_W_HZ = 4000, SRCH_MAX_W_HZ = 32000;   // a candidate's width above the threshold

struct CcEntry {
    uint64_t id = 0;
    double hz = 0, meas_hz = 0;    // nominal (the signalling's / the snapped measurement), measured
    std::string plugin, sys, sys_name, site, name, src;   // src: search | heard | user
    std::map<std::string, std::string> opts;   // the control VFO's plugin options (key without the plugin prefix)
    float level_db = NAN, gain_db = NAN;
    int64_t first = 0, last = 0, followed = 0;   // epoch ms
    uint64_t n = 0;
    bool user() const { return !name.empty() || followed > 0 || src == "user"; }
};
std::map<uint64_t, CcEntry> g_cc;
uint64_t g_cc_next = 1;
int64_t g_cc_dirty = 0, g_cc_list_sent = 0;
bool g_cc_list_now = false;
std::string g_cc_note, g_cc_bad;
std::string g_cc_state = "ok";   // trunk_cc.file.state: ok | not_loaded (kept as kept_as) | not_written (kept in memory, tried again)
std::map<int, int64_t> g_cc_heard;            // control VFO -> last time it refreshed its entry
std::map<int, std::pair<uint64_t, uint64_t>> g_cc_follow;   // control VFO -> (its instance serial, the entry it follows)

double snap(double hz, double raster) { return raster > 0 ? std::round(hz / raster) * raster : std::round(hz); }

CcEntry* cc_find(const std::string& plugin, double hz, double tol) {
    CcEntry* best = nullptr;
    for (auto& kv : g_cc)
        if (kv.second.plugin == plugin && std::fabs(kv.second.hz - hz) <= tol &&
            (!best || std::fabs(kv.second.hz - hz) < std::fabs(best->hz - hz)))
            best = &kv.second;
    return best;
}

void cc_changed() {
    if (!g_cc_dirty) g_cc_dirty = now_ms();
    g_cc_list_now = true;
}

// an entry for (plugin, hz): the existing one within the tolerance, else a new
// one (the memory full: the oldest the air brought goes; all the user's: none)
CcEntry* cc_ensure(const std::string& plugin, double hz, double tol, bool* made) {
    *made = false;
    if (CcEntry* e = cc_find(plugin, hz, tol)) return e;
    if (g_cc.size() >= CC_MAX) {
        auto victim = g_cc.end();
        for (auto it = g_cc.begin(); it != g_cc.end(); ++it)
            if (!it->second.user() && (victim == g_cc.end() || it->second.last < victim->second.last)) victim = it;
        if (victim == g_cc.end()) return nullptr;
        g_cc.erase(victim);
    }
    CcEntry& e = g_cc[g_cc_next];
    e.id = g_cc_next++;
    e.plugin = plugin;
    e.hz = hz;
    e.first = wall_ms();
    *made = true;
    return &e;
}

// a control channel seen (search / heard): its entry updated. A different
// system on that frequency replaces the system fields (the user's name stays)
CcEntry* cc_seen(const std::string& plugin, double nominal, double measured, const std::string& key, const std::string& name,
                 const std::string& site, const std::map<std::string, std::string>& opts, float level, const char* src, double tol,
                 bool* made) {
    CcEntry* e = cc_ensure(plugin, nominal, tol, made);
    if (!e) return nullptr;
    const std::string sys = key.empty() ? "" : plugin + ":" + key;
    if (*made || (!sys.empty() && sys != e->sys)) {
        // new, or another system on that frequency: its fields as reported
        e->sys = sys;
        e->sys_name = clean(name, 64);
        e->site = clean(site, 64);
    } else {
        // the same system (or no key this time): a sighting with LESS keeps what the entry has
        if (!name.empty()) e->sys_name = clean(name, 64);
        if (!site.empty()) e->site = clean(site, 64);
    }
    if (*made) e->hz = nominal;
    if (fin(measured)) e->meas_hz = measured;
    if (!opts.empty()) e->opts = opts;
    if (fin(level)) e->level_db = level;
    const float g = ChannelManager::get_gain(active_channel.load());
    e->gain_db = g;
    e->last = wall_ms();
    e->n++;
    if (*made || e->src.empty()) e->src = src;
    cc_changed();
    return e;
}

std::string cc_entry_json(const CcEntry& e) {
    std::ostringstream o;
    o << "{\"id\":" << e.id << ",\"hz\":" << num_json(e.hz) << ",\"meas_hz\":" << num_json(e.meas_hz > 0 ? e.meas_hz : NAN)
      << ",\"plugin\":" << js(e.plugin) << ",\"sys\":" << js(e.sys) << ",\"sys_name\":" << js(e.sys_name) << ",\"site\":" << js(e.site)
      << ",\"name\":" << js(e.name) << ",\"src\":" << js(e.src) << ",\"level_db\":" << num_json(e.level_db, "%.1f")
      << ",\"gain_db\":" << num_json(e.gain_db, "%.1f") << ",\"first\":" << e.first << ",\"last\":" << e.last << ",\"n\":" << e.n
      << ",\"followed\":" << e.followed << ",\"opts\":{";
    bool f = true;
    for (const auto& kv : e.opts) {
        o << (f ? "" : ",") << js(kv.first) << ":" << js(kv.second);
        f = false;
    }
    o << "}}";
    return o.str();
}

std::string cc_list_json() {
    std::ostringstream o;
    o << "{\"trunk_cc\":{\"file\":{\"ok\":" << jb(g_cc_note.empty()) << ",\"state\":" << js(g_cc_state) << ",\"note\":" << js(g_cc_note)
      << ",\"kept_as\":" << js(g_cc_bad)
      << "},\"max\":" << CC_MAX << ",\"entries\":[";
    bool f = true;
    for (const auto& kv : g_cc) {
        o << (f ? "" : ",") << cc_entry_json(kv.second);
        f = false;
    }
    o << "]}}";
    return o.str();
}

void cc_save() {
    if (playback::active()) return;
    std::ostringstream o;
    o << "{\"version\":1,\"entries\":[";
    bool f = true;
    for (const auto& kv : g_cc) {
        o << (f ? "" : ",\n") << cc_entry_json(kv.second);
        f = false;
    }
    o << "]}\n";
    std::lock_guard<std::mutex> lk(g_save_mu);
    if (g_shutdown) return;
    const std::string tmp = std::string(CC_FILE) + ".tmp";
    {
        std::ofstream f2(tmp, std::ios::binary | std::ios::trunc);
        if (f2) f2 << o.str();
        if (!f2) {
            g_cc_note = std::string(CC_FILE) + " could not be written (kept in memory, tried again)";
        g_cc_state = "not_written";
            g_cc_dirty = now_ms();
            g_cc_list_now = true;
            return;
        }
    }
    if (fs_replace(tmp, CC_FILE) != 0) {
        static int64_t warned = 0;
        if (now_ms() - warned > 60000) {
            warned = now_ms();
            std::cerr << "Trunk following: " << CC_FILE << " could not be written" << std::endl;
        }
        g_cc_note = std::string(CC_FILE) + " could not be written (kept in memory, tried again)";
        g_cc_state = "not_written";
        g_cc_dirty = now_ms();   // (again in CC_SAVE_MS)
        g_cc_list_now = true;
        return;
    }
    if (g_cc_state == "not_written") {
        g_cc_note.clear();
        g_cc_state = g_cc_bad.empty() ? "ok" : "not_loaded";
        if (!g_cc_bad.empty()) g_cc_note = "the control channel memory could not be read - kept as " + g_cc_bad;
        g_cc_list_now = true;
    }
    g_cc_dirty = 0;
}

// a valid entry from JSON (the file, TRUNK_CC_SET): "" = ok, else why not
std::string cc_parse(const geo::Json& g, CcEntry* e) {
    const double hz = jnum(g, "hz");
    if (!hz_ok(hz)) return "hz: the frequency in Hz (0 - 6 GHz)";
    const std::string plugin = g.str("plugin");
    if (!dig::valid_plugin_id(plugin)) return "plugin: a plugin id";
    // (only decoders that declare trunking - at load the registry may not be read yet: checked again by tick_cc)
    if (!dig::PluginRegistry::instance().list().empty() && !plugin_trunking(plugin)) return "plugin: a decoder that declares trunking";
    e->hz = std::round(hz);
    e->plugin = plugin;
    const double m = jnum(g, "meas_hz");
    e->meas_hz = hz_ok(m) && std::fabs(m - e->hz) <= 50000 ? m : 0;   // (a measurement belongs near its channel)
    const std::string sys = g.str("sys");
    const size_t c = sys.find(':');
    if (!sys.empty() && (c == std::string::npos || sys.substr(0, c) != plugin || !valid_key(sys.substr(c + 1)))) return "sys: <plugin>:<key>";
    e->sys = sys;
    e->sys_name = clean(g.str("sys_name"), 64);
    e->site = clean(g.str("site"), 64);
    e->name = clean(g.str("name"), 64);
    const std::string src = g.str("src");
    e->src = src == "search" || src == "heard" ? src : "user";
    const double lv = jnum(g, "level_db"), gn = jnum(g, "gain_db");
    e->level_db = fin(lv) && lv >= 0 && lv <= 120 ? static_cast<float>(lv) : NAN;
    e->gain_db = fin(gn) && gn >= -1 && gn <= 60 ? static_cast<float>(gn) : NAN;
    // times: 0 (never) or 2020-01-01 .. now - a file from elsewhere can't make an entry "seen just now" for ever
    const long long lo_t = 1577836800000LL, now_t = wall_ms();
    auto when = [&](const char* k) {
        const long long t = int_or(jnum(g, k, 0), 0, 9000000000000000LL, 0);
        return t <= 0 ? 0LL : std::clamp(t, lo_t, now_t);
    };
    e->first = when("first");
    e->last = when("last");
    e->followed = when("followed");
    if (e->first > e->last && e->last > 0) e->first = e->last;
    e->n = static_cast<uint64_t>(int_or(jnum(g, "n", 0), 0, 1000000000LL, 0));
    e->opts.clear();
    if (const geo::Json* o = g.get("opts"); o && o->type == geo::Json::OBJ) {
        if (o->o.size() > 32) return "opts: at most 32";
        for (const auto& kv : o->o) {
            // (what DIGITAL_OPT applies: its key rule, values <= 100 bytes without control characters)
            bool ctl = false;
            if (kv.second.type == geo::Json::STR)
                for (unsigned char ch : kv.second.s)
                    if (ch < 0x20 || ch == 0x7F) ctl = true;
            if (kv.second.type != geo::Json::STR || kv.first.empty() || kv.first.size() > 32 || kv.second.s.size() > 100 || ctl ||
                !dig::valid_option_key(plugin + "." + kv.first))
                return "opts: {key: \"value\"} (the plugin's option keys, values <= 100 bytes, no control characters)";
            e->opts[kv.first] = kv.second.s;
        }
    }
    return "";
}

void cc_load() {
    std::ifstream f(CC_FILE, std::ios::binary | std::ios::ate);
    if (!f) return;
    const std::streamoff size = f.tellg();
    std::string text;
    if (size >= 0 && size <= (8 << 20)) {   // (its size first: a huge file isn't read)
        f.seekg(0);
        text.assign(static_cast<size_t>(size), '\0');
        f.read(&text[0], size);
    }
    f.close();
    geo::Json j;
    const geo::Json* a = nullptr;
    bool newer = false;
    if (!text.empty() && geo::json_parse(text, &j) && j.type == geo::Json::OBJ) {
        const double ver = jnum(j, "version", 1);
        if (ver > 1) newer = true;   // (a newer receiver's file: kept aside untouched)
        else a = j.get("entries");
    }
    if (!a || a->type != geo::Json::ARR) {
        char ts[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&t));
        std::string kept = std::string(CC_FILE) + ".bad-" + ts;
        for (int k = 1; std::ifstream(kept).good() && k < 100; k++) kept = std::string(CC_FILE) + ".bad-" + ts + "-" + std::to_string(k);
        if (std::rename(CC_FILE, kept.c_str()) == 0) g_cc_bad = kept;
        g_cc_note = std::string(newer ? "the control channel memory is of a newer version" : "the control channel memory could not be read") +
                    " - kept as " + kept + ", starting empty";
        g_cc_state = "not_loaded";
        std::cerr << "Trunk following: " << g_cc_note << std::endl;
        return;
    }
    int bad = 0, over = 0, notrunk = 0;
    for (const auto& g : a->a) {
        CcEntry e;
        const std::string why = g.type == geo::Json::OBJ ? cc_parse(g, &e) : std::string("not an object");
        if (!why.empty()) {
            // (a decoder without trunking: counted for the note, whether the registry was read before or after)
            if (why.rfind("plugin: a decoder that declares trunking", 0) == 0) notrunk++;
            else bad++;
            continue;
        }
        if (g_cc.size() >= CC_MAX) {
            over++;
            continue;
        }
        // (ids renumbered 1.. on load: an id from a file can't push the next one past what a page can address)
        e.id = g_cc_next++;
        g_cc[e.id] = e;
    }
    if (over) {
        // more than CC_MAX: the original kept aside before the first write would lose them
        char ts[32];
        const std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&t));
        const std::string kept = std::string(CC_FILE) + ".full-" + ts;
        std::ofstream k(kept, std::ios::binary);
        k << text;
        g_cc_note = std::to_string(over) + " entries over the " + std::to_string(CC_MAX) + " kept aside in " + kept;
        std::cerr << "Trunk following: " << g_cc_note << std::endl;
    }
    if (notrunk) {
        const std::string n = std::to_string(notrunk) + " entr" + (notrunk == 1 ? "y" : "ies") + " of decoders without trunking dropped";
        g_cc_note = g_cc_note.empty() ? n : g_cc_note + "; " + n;
        std::cerr << "Trunk following: " << n << std::endl;
    }
    std::cout << "Trunk following: " << g_cc.size() << " control channel(s) remembered (" << CC_FILE << ")"
              << (bad ? ", " + std::to_string(bad) + " unreadable skipped" : "") << std::endl;
}

void cc_follow_tick();
bool g_cc_stats_dirty = false;   // only last / n / level of heard entries changed: saved / sent at most every CC_STATS_MS
int64_t g_cc_stats_t = 0;
constexpr int64_t CC_STATS_MS = 300000;
bool g_cc_plugins_checked = false;
void tick_cc() {
    const int64_t now = now_ms();
    static int64_t t_follow = 0;
    if (now - t_follow >= 2000) {
        t_follow = now;
        cc_follow_tick();
    }
    if (g_cc_stats_dirty && now - g_cc_stats_t >= CC_STATS_MS) {
        g_cc_stats_dirty = false;
        g_cc_stats_t = now;
        cc_changed();
    }
    // entries of decoders that don't declare trunking (a file loaded before the registry was read): dropped
    if (!g_cc_plugins_checked && !dig::PluginRegistry::instance().list().empty()) {
        g_cc_plugins_checked = true;
        int n = 0;
        for (auto it = g_cc.begin(); it != g_cc.end();)
            if (!plugin_trunking(it->second.plugin)) {
                it = g_cc.erase(it);
                n++;
            } else {
                ++it;
            }
        if (n) {
            g_cc_note = std::to_string(n) + " entr" + (n == 1 ? "y" : "ies") + " of decoders without trunking dropped";
            std::cerr << "Trunk following: " << g_cc_note << std::endl;
            cc_changed();
        }
    }
    if (g_cc_dirty && now - g_cc_dirty >= CC_SAVE_MS && !playback::active()) cc_save();
    if (g_cc_list_now && now - g_cc_list_sent >= CC_LIST_MS) {
        g_cc_list_now = false;
        g_cc_list_sent = now;
        broadcast(cc_list_json());
    }
}

// the control VFO's plugin options as they are now (key without the prefix)
std::map<std::string, std::string> vfo_plugin_opts(int vfo, const std::string& plugin) {
    std::map<std::string, std::string> m;
    auto inst = decimator_manager.getDecimator(vfo);
    auto dd = inst ? inst->getDigital() : nullptr;
    if (!dd) return m;
    const std::string pre = plugin + ".";
    for (const auto& kv : dd->options().plugin)
        if (kv.first.compare(0, pre.size(), pre) == 0 && kv.first.size() > pre.size()) m[kv.first.substr(pre.size())] = kv.second;
    return m;
}

// --- the search ---------------------------------------------------------------
struct Probe {
    int vfo = -1;
    uint64_t serial = 0;
    size_t cand = 0;
    std::string plugin;
    int64_t t0 = 0, t_ctl = 0;
    uint64_t f0 = 0;        // the stream's spectrum frames when it was made (its deadlines count stream time)
    uint64_t f_ctl = 0;     // ... when its control flag came (the key waited for in stream time)
    bool control = false, done = false;
    std::string key, name, site;
    double ctl_hz = NAN;    // the signalling's own control channel frequency (CONTROL / CHANNEL near the probe)
};
struct Cand {
    double hz = 0, width = 0;
    float level = 0;
    bool found = false;
    std::vector<std::string> todo;   // plugins still to try
};
struct Search {
    bool active = false;
    std::vector<std::pair<double, double>> ranges;
    std::vector<double> centres;
    size_t step = 0;
    int st = 0;             // 0 tune, 1 arrive, 2 settle, 3 look 1, 4 look 2, 5 probe
    int64_t t_st = 0, t_start = 0, w_start = 0, hold_sent = 0;
    double home = 0, thr_db = 6, raster = 6250;
    std::vector<Cand> cands;
    std::vector<Probe> probes;
    std::vector<double> look1;
    std::vector<std::pair<double, double>> found_hz;   // (hz, plugin hash) this run: an overlap's second sight skipped
    std::vector<ExcCtl> ctl;    // the user's control VFOs, parked while it runs
    uint64_t carriers = 0, probed = 0, found_new = 0, found_known = 0;
    std::vector<int64_t> step_ms;
    int64_t t_step = 0;
    bool kerberos = false;
    int mode = 0;           // 0 coherent (the centre steps, calibration held), 2 independent (one tuner steps)
    double asked = 0;       // what the receiver took for the step (its FREQ refused: unchanged)
    uint64_t tries = 0, probed0 = 0;   // this step's (candidate, plugin) tries planned; probed when they were
    int64_t t_noroom = 0;   // no free VFO for a probe since
    bool skip_restore = false;
    uint64_t frames0 = 0;   // the step's spectrum frames counted from here (the search's clock is data, not time)
    std::vector<double> probed_hz;   // every candidate probed in this run, found or not (skipped again)
    std::vector<std::pair<double, double>> covered;   // the bands already looked at (the last step's overlap)
    int tuner = 0;          // independent: the tuner that steps
};
Search g_srch;
std::string g_srch_last;   // the last run's summary (JSON object)

bool searching() { return g_srch.active; }
void srch_recs_clear();

bool is_probe(int vfo) {
    for (const auto& p : g_srch.probes)
        if (p.vfo == vfo) return true;
    return false;
}

void srch_remove_probes() {
    bool any = false;
    for (auto& p : g_srch.probes) {
        auto inst = decimator_manager.getDecimator(p.vfo);
        if (!inst || inst->serial != p.serial) continue;
        meta_erase(p.vfo, true);
        if (!decimator_manager.removeDecimator(p.vfo)) {
            inst->enabled = true;   // (the receiver's last VFO: a plain one, as a voice VFO's hand-back)
            decimator_manager.setDigitalMode(p.vfo, dig::Mode::OFF, "");
        }
        any = true;
    }
    g_srch.probes.clear();
    if (any) {
        audio_mixer.retain(decimator_manager.audioVfoIds());
        broadcast(MessageBuilders::build_decimator_info_message());
    }
}

std::vector<std::string> trunking_plugins() {
    std::vector<std::string> v;
    for (const auto& pi : dig::PluginRegistry::instance().list())
        if (pi.built && pi.trunking && pi.fixed_freq_hz <= 0 && !pi.lock_vfo) v.push_back(pi.id);
    return v;
}

int srch_batch_max() {
    unsigned hc = std::thread::hardware_concurrency();
#ifdef __linux__
    // (the cores this process may run on - a receiver pinned with taskset, a container with fewer CPUs)
    cpu_set_t cs;
    if (sched_getaffinity(0, sizeof cs, &cs) == 0 && CPU_COUNT(&cs) > 0) hc = static_cast<unsigned>(CPU_COUNT(&cs));
#endif
    return std::clamp<int>(hc ? static_cast<int>(hc) / 2 : 2, 2, 12);
}

void srch_end(const std::string& why, bool home) {
    if (!g_srch.active) return;
    const int64_t now = now_ms();
    srch_remove_probes();
    std::vector<std::string> cmds;
    if (home && !playback::active() && operating_mode_index() == g_srch.mode) {
        char f[64];
        if (g_srch.mode == 2) snprintf(f, sizeof f, "TUNER_FREQ:%d:%.6f", g_srch.tuner, g_srch.home / 1e6);
        else snprintf(f, sizeof f, "FREQ:%.6f", g_srch.home / 1e6);
        cmds.push_back(f);
    }
    for (auto& e : g_srch.ctl)
        if (auto inst = exc_inst(e)) inst->enabled = true;
    for (auto& e : g_srch.ctl) g_tracks[e.vfo].expect_reset_until = now + 10000;
    if (!cmds.empty()) dispatch(cmds);
    if (g_srch.mode == 0 && !g_srch.kerberos && !playback::active()) heimdall_hold(0);   // (the calibration at home at once)
    std::ostringstream o;
    o << "{\"end\":" << js(why) << ",\"carriers\":" << g_srch.carriers << ",\"probed\":" << g_srch.probed << ",\"found_new\":"
      << g_srch.found_new << ",\"found_known\":" << g_srch.found_known << ",\"steps\":" << g_srch.centres.size()
      << ",\"steps_done\":" << g_srch.step << ",\"dur_ms\":" << (now - g_srch.t_start) << ",\"t_end\":" << wall_ms() << "}";
    g_srch_last = o.str();
    if (why == "user_retuned") g_srch_skip_restore = true;   // (his retune: the VFOs keep their offsets, as on any retune)
    std::cout << "Trunk following: control channel search over (" << why << "): " << g_srch.found_new << " new, "
              << g_srch.found_known << " known, " << g_srch.carriers << " carriers, " << (now - g_srch.t_start) / 1000.0 << " s"
              << std::endl;
    g_srch = Search{};
    broadcast(MessageBuilders::build_decimator_info_message());
    g_status_now = true;
}

std::string srch_start(const geo::Json& j) {
    if (g_srch.active) return "a search runs already";
    if (playback::active()) return "not during a playback";
    if (scanning()) return "a scanner runs - stop it first";
    const int mode = operating_mode_index();
    if (mode == 1) return "not in wideband scan mode (its tuners are spread) - switch to coherent or independent first";
    if (!heimdall_up()) return "heimdall sends nothing";
    const geo::Json* r = j.get("ranges");
    if (!r || r->type != geo::Json::ARR || r->a.empty() || r->a.size() > 16) return "ranges: [[lo_hz, hi_hz], ...] (1-16)";
    Search s;
    double total = 0;
    for (const auto& x : r->a) {
        if (x.type != geo::Json::ARR || x.a.size() != 2 || x.a[0].type != geo::Json::NUM || x.a[1].type != geo::Json::NUM)
            return "ranges: [[lo_hz, hi_hz], ...]";
        const double lo = x.a[0].n, hi = x.a[1].n;
        if (!(lo < hi) || !exc_range(lo) || !exc_range(hi)) return "a range outside the tuner's range (or lo >= hi)";
        s.ranges.push_back({lo, hi});
        total += hi - lo;
    }
    if (total > 2e9) return "ranges: at most 2000 MHz in all";
    const double thr = jnum(j, "threshold_db", 6), ras = jnum(j, "raster_hz", 6250);
    if (!(thr >= 3 && thr <= 40)) return "threshold_db: 3-40";
    if (!(ras == 0 || ras == 5000 || ras == 6250 || ras == 12500)) return "raster_hz: 0, 5000, 6250 or 12500";
    if (trunking_plugins().empty()) return "no built plugin declares trunking";
    if (static_cast<int>(decimator_manager.getDecimatorCount()) >= VFO_LIMIT) return "no free VFO for the probes (16 in use)";
    s.thr_db = thr;
    s.raster = ras;
    const double half = usable_half_hz(), stepw = 2 * half - SRCH_OVERLAP_HZ;
    for (const auto& rg : s.ranges)
        for (double c = rg.first + half - SRCH_OVERLAP_HZ / 2; ; c += stepw) {
        {
            double cc = std::min(c, std::max(rg.first + half, rg.second - half + SRCH_OVERLAP_HZ / 2));
            // (half a raster off it: a channel on the raster is never at the centre's DC, which can't be seen)
            if (ras > 0) cc = std::round(cc / ras) * ras + ras / 2;
            double lo_t = RTL_TUNER_MIN_HZ, hi_t = RTL_TUNER_MAX_HZ;
            if (wb_variant_enabled.load()) {
                uint64_t a, b;
                wb_variant_rf_union_range(a, b);
                lo_t = static_cast<double>(a);
                hi_t = static_cast<double>(b);
            }
            cc = std::clamp(cc, lo_t, hi_t);   // (the band covers the edge from inside)
            if (s.centres.empty() || std::fabs(s.centres.back() - cc) > 1000) s.centres.push_back(std::round(cc));
        }
            if (c + half >= rg.second) break;
        }
    // the receiver stands down: an excursion home, voice tuners back, calls freed
    if (g_exc.st == 1) exc_return("search", true);
    g_exc.st = 0;
    vt_restore_all("search", true);
    for (auto& kv : g_calls)
        if (kv.second.vfo >= 0) unfollow(kv.second, "paused");
    remove_all_voices();
    s.active = true;
    s.mode = mode;
    s.home = static_cast<double>(requested_freq_hz.load());
    if (mode == 2) {
        // independent mode / generic RTL-SDRs: ONE tuner steps - the user's control VFO's, else tuner 0
        const std::set<int> ct = vt_control_tuners();
        s.tuner = ct.empty() ? 0 : *ct.begin();
        s.home = indep_tuner_freq_hz[s.tuner].load();
        if (s.home <= 0) s.home = static_cast<double>(exact_tuner_hz(s.tuner));
    }
    s.t_start = s.t_st = now_ms();
    s.w_start = wall_ms();
    s.kerberos = server_kerberos_mode.load();
    s.ctl = exc_note_ctl();
    for (auto& e : s.ctl)
        if (auto inst = exc_inst(e)) inst->enabled = false;   // (the user's control VFOs hear nothing while it sweeps)
    g_srch = s;
    srch_recs_clear();
    std::cout << "Trunk following: control channel search, " << g_srch.centres.size() << " steps, " << total / 1e6 << " MHz"
              << std::endl;
    g_status_now = true;
    return "";
}

// the spectrum's carriers of channel width >= thr above the floor (Hz), at centre c
int srch_ch() { return g_srch.mode == 2 ? g_srch.tuner : std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1); }
// spectrum frames taken in a row at centre c (0 = none there yet)
uint64_t srch_frames(double c) {
    const int ch = srch_ch();
    if (std::fabs(static_cast<double>(fft_frame_hz[ch].load(std::memory_order_relaxed)) - c) > 1000) return 0;
    return fft_frames_here[ch].load(std::memory_order_acquire);
}
double srch_fps() { return SAMPLE_RATE / static_cast<double>(std::max(256, current_fft_size.load())); }
// frames that replace 95 % of the average (at least 3)
uint64_t srch_refill_frames() {
    float a = averaging_alpha.load(std::memory_order_relaxed);
    if (!(a > 0.001f)) a = 0.001f;
    const double f = a >= 1.0f ? 1.0 : std::log(0.05) / std::log(1.0 - a);
    return static_cast<uint64_t>(std::max(3.0, std::ceil(f) + 2));
}

// The spectrum's carriers at centre c: every local peak >= thr above the
// floor; its width is measured relative to its OWN peak (the run of bins
// within 20 dB of it - a strong carrier's skirt at a fixed threshold over the
// floor is far wider than the channel), its centre between those edges. 4-32
// kHz wide; one run above the threshold can hold several (12.5 kHz apart)
std::vector<Cand> srch_carriers(double c, double thr) {
    std::vector<Cand> out;
    std::vector<float> spec;
    const int ch = srch_ch();
    {
        std::lock_guard<std::mutex> lk(fft_mutex);
        if (ch >= static_cast<int>(fft_averaged.size())) return out;
        spec = fft_averaged[ch];
    }
    const int n = static_cast<int>(spec.size());
    if (n < 64) return out;
    const float floor_db = FFTProcessor::channel_noise_floor(ch, spec);
    if (!(floor_db > -190.0f)) return out;
    if (const char* dp = getenv("KRAKEN_TRUNK_SEARCH_DUMP")) {   // (debugging: the spectrum each look saw)
        std::ofstream df(std::string(dp) + "/look_" + std::to_string(static_cast<long long>(c)) + "_" + std::to_string(now_ms()) + ".txt");
        df << c << " " << floor_db << "\n";
        for (float v : spec) df << v << "\n";
    }
    const double bin = SAMPLE_RATE / static_cast<double>(n), half = usable_half_hz();
    // smoothed over ~1 kHz: a digital carrier's averaged spectrum is ragged bin to bin (+-3 dB at 146 Hz bins),
    // its -10 dB run would end in a dip
    {
        const int k = std::max(1, static_cast<int>(std::lround(500.0 / bin)));
        std::vector<float> sm(spec.size());
        double acc = 0;
        int cnt = 0;
        for (int i = 0; i < n + k; i++) {
            if (i < n) { acc += spec[i]; cnt++; }
            if (i - 2 * k - 1 >= 0) { acc -= spec[i - 2 * k - 1]; cnt--; }
            if (i - k >= 0 && i - k < n) sm[i - k] = static_cast<float>(acc / cnt);
        }
        spec.swap(sm);
    }
    // the LOCAL background (the 20th percentile within +-50 kHz, in 32-bin blocks): a busy band's background
    // sits well above the receiver's noise floor (measured on air: 11-14 dB around 499 MHz) - over the global
    // floor a weak carrier's skirt ran into its neighbours' and it was thrown away as "wide"
    std::vector<float> bg(static_cast<size_t>(n));
    {
        const int w = std::max(8, static_cast<int>(50000.0 / bin)), blk = 32;
        std::vector<float> cs, tmp;
        for (int b0 = 0; b0 < n; b0 += blk) {
            const int mid = b0 + blk / 2, a = std::max(0, mid - w), e = std::min(n, mid + w);
            tmp.assign(spec.begin() + a, spec.begin() + e);
            const size_t q = tmp.size() / 5;
            std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(q), tmp.end());
            cs.push_back(tmp[q]);
        }
        for (int i = 0; i < n; i++) {
            const double x = (i - blk / 2.0) / blk;
            const int j = std::clamp(static_cast<int>(std::floor(x)), 0, static_cast<int>(cs.size()) - 1);
            const int j2 = std::min(j + 1, static_cast<int>(cs.size()) - 1);
            const double fr = std::clamp(x - j, 0.0, 1.0);
            bg[i] = static_cast<float>(cs[j] * (1 - fr) + cs[j2] * fr);
        }
    }
    auto fq = [&](double i) { return c - SAMPLE_RATE / 2.0 + i * bin; };
    std::vector<char> used(static_cast<size_t>(n), 0);
    // every bin over the threshold, strongest first (all of them: a busy band's noise peaks can't use up a budget)
    std::vector<int> order;
    for (int i = 1; i + 1 < n; i++)
        if (spec[i] - bg[i] >= thr) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return spec[a] > spec[b]; });
    for (int pk : order) {
        if (used[pk]) continue;
        // the width: the run within 20 dB of the peak; the centre: between the edges of the run within 10 dB
        // (a neighbour merging into the skirt moves the -20 dB edge, not the -10 dB one)
        // (the threshold decides WHETHER a carrier is tried, never where: the runs don't depend on it)
        const float lim = std::max(spec[pk] - 20.0f, bg[pk] + 4.5f);
        const float lim10 = std::max(spec[pk] - 10.0f, bg[pk] + 4.5f);
        int lo = pk, hi = pk;
        while (lo - 1 >= 0 && !used[lo - 1] && spec[lo - 1] >= lim) lo--;
        while (hi + 1 < n && !used[hi + 1] && spec[hi + 1] >= lim) hi++;
        int clo = pk, chi = pk;
        // (the centre run on its own peak: a neighbour's run taken out before can't cut it short)
        while (clo - 1 >= 0 && spec[clo - 1] >= lim10) clo--;
        while (chi + 1 < n && spec[chi + 1] >= lim10) chi++;
        for (int k = lo; k <= hi; k++) used[k] = 1;
        // (a narrow spike on top - a pilot, a carrier inside a wider signal: the -20 dB run's centre)
        const bool spike = (chi - clo + 1) * 10 < (hi - lo + 1) * 4;
        // (the width too from the -10 dB run: a weak carrier's -20 dB run reaches its neighbours' skirts and looked "wide")
        const double w = (spike ? hi - lo + 1 : chi - clo + 1) * bin, f = spike ? fq((lo + hi) / 2.0) : fq((clo + chi) / 2.0);
        if (w >= SRCH_MIN_W_HZ && w <= SRCH_MAX_W_HZ && std::fabs(f - c) + w / 2 <= half && std::fabs(f - c) > 3 * bin) {
            Cand cd;
            cd.hz = f;
            cd.width = w;
            cd.level = spec[pk] - bg[pk];
            out.push_back(cd);
        }
    }
    // (peaks were taken strongest first: a later one in a stronger one's skirt - within 20 kHz, 15 dB weaker - is its skirt)
    std::vector<Cand> keep;
    for (const auto& cd : out) {
        bool skirt = false;
        for (const auto& k : keep)
            if (std::fabs(k.hz - cd.hz) < 20000 && k.level - cd.level >= 15) skirt = true;
        if (!skirt) keep.push_back(cd);
    }
    return keep;
}

void srch_probe_batch() {
    const int64_t now = now_ms();
    const int room = std::min(srch_batch_max(), VFO_LIMIT - static_cast<int>(decimator_manager.getDecimatorCount()));
    int made = 0;
    for (size_t ci = 0; ci < g_srch.cands.size() && made < room; ci++) {
        Cand& cd = g_srch.cands[ci];
        while (!cd.found && !cd.todo.empty() && made < room) {
            const std::string plugin = cd.todo.front();
            cd.todo.erase(cd.todo.begin());
            dig::PluginInfo pi;
            if (!plugin_info(plugin, &pi)) continue;
            const int id = decimator_manager.addDecimator();
            auto inst = id >= 0 ? decimator_manager.getDecimator(id) : nullptr;
            if (!inst) return;
            Probe p;
            p.vfo = id;
            p.serial = inst->serial;
            p.cand = ci;
            p.plugin = plugin;
            p.t0 = now;
            p.f0 = srch_frames(g_srch.centres[g_srch.step]);
            g_srch.probes.push_back(p);
            meta_set(id, inst.get(), true, ",\"trunk\":{\"role\":\"probe\"}");   // (true: never saved, never the user's)
            const int bw = efficient_bandwidth_at_least(static_cast<int>(std::ceil(std::max(12500.0, std::min(50000.0, pi.min_vfo_rate)))));
            char f[96];
            // (on the raster when one is set: control channels sit on it, and the decoders track the rest)
            const double sn = g_srch.raster > 0 ? snap(cd.hz, g_srch.raster) : cd.hz;
            const double at = std::fabs(sn - cd.hz) <= g_srch.raster / 4 ? sn : cd.hz;
            if (g_srch.mode == 2)
                snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f:%d", id, (at - static_cast<double>(exact_tuner_hz(g_srch.tuner))) / 1000.0,
                         g_srch.tuner);
            else
                snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f", id, (at - coherent_tuner_hz()) / 1000.0);
            dispatch({"SET_DECIMATOR_BW_HZ:" + std::to_string(id) + ":" + std::to_string(bw), f,
                      "DIGITAL_MODE:" + std::to_string(id) + ":PLUGIN:" + plugin});
            set_heard(id, false);
            inst->enabled = true;   // (addDecimator makes it disabled)
            g_srch.probed++;
            made++;
        }
    }
    if (made) broadcast(MessageBuilders::build_decimator_info_message());
}

struct SrchRec {
    double hz;
    std::string plugin, key;
    float level;
    uint64_t id;
    bool made;
};
std::vector<SrchRec> g_srch_recs;   // this run's records (cleared at its start)
void srch_recs_clear() { g_srch_recs.clear(); }

void srch_record(const Probe& p) {
    Cand& cd = g_srch.cands[p.cand];
    cd.found = true;
    for (auto it = g_srch_recs.begin(); it != g_srch_recs.end(); ++it)
        if (it->plugin == p.plugin && it->key == p.key && std::fabs(it->hz - cd.hz) < 15000) {
            if (it->level >= cd.level) return;   // (the weaker one beside it: the same channel heard off its centre)
            auto ue = g_cc.find(it->id);
            if (ue != g_cc.end() && ue->second.user()) return;   // (the user's: never erased by the search)
            if (it->made) {
                g_cc.erase(it->id);
                g_srch.found_new--;
            } else {
                g_srch.found_known--;
            }
            g_srch_recs.erase(it);
            break;
        }
    const double tol = std::max(3000.0, g_srch.raster / 2);
    // the nominal frequency: the signalling's, else the measured centre snapped to the raster
    double nominal = fin(p.ctl_hz) && std::fabs(p.ctl_hz - cd.hz) < 15000 ? p.ctl_hz : snap(cd.hz, g_srch.raster);
    bool made = false;
    CcEntry* e = cc_seen(p.plugin, nominal, cd.hz, p.key, p.name, p.site, vfo_plugin_opts(p.vfo, p.plugin), cd.level, "search", tol, &made);
    if (!e) return;
    if (made) g_srch.found_new++;
    else g_srch.found_known++;
    g_srch_recs.push_back({cd.hz, p.plugin, p.key, cd.level, e->id, made});
    std::cout << "Trunk following: control channel found at " << nominal / 1e6 << " MHz (" << p.plugin
              << (p.key.empty() ? ", no key" : ", " + p.plugin + ":" + p.key) << ")" << (made ? "" : " - known") << std::endl;
}

// a probe VFO's event (the search owns it: nothing of it reaches the systems)
void srch_event(int vfo, const PluginEvent& ev) {
    for (auto& p : g_srch.probes) {
        if (p.vfo != vfo || p.done) continue;
        const Cand& cd = g_srch.cands[p.cand];
        if (ev.kind == PluginEvent::SYSTEM) {
            if (ev.control && !p.control) {
                p.control = true;
                p.t_ctl = now_ms();
                p.f_ctl = g_srch.step < g_srch.centres.size() ? srch_frames(g_srch.centres[g_srch.step]) : 0;
            }
            if (!ev.sys_key.empty()) p.key = ev.sys_key;
            if (!ev.sys_name.empty()) p.name = ev.sys_name;
            if (!ev.sys_site.empty()) p.site = ev.sys_site;
        } else if ((ev.kind == PluginEvent::CONTROL || ev.kind == PluginEvent::CHANNEL) && fin(ev.ch.freq_hz) &&
                   std::fabs(ev.ch.freq_hz - cd.hz) < 15000) {
            if (!fin(p.ctl_hz) || std::fabs(ev.ch.freq_hz - cd.hz) < std::fabs(p.ctl_hz - cd.hz)) p.ctl_hz = ev.ch.freq_hz;
        }
    }
}

void tick_search() {
    if (!g_srch.active) return;
    const int64_t now = now_ms();
    if (playback::active()) return srch_end("playback", false);
    const double asked = g_srch.mode == 2 ? indep_tuner_freq_hz[g_srch.tuner].load() : static_cast<double>(requested_freq_hz.load());
    if (g_srch.st != 0 && g_srch.step < g_srch.centres.size() && std::fabs(asked - g_srch.asked) > 1000)
        return srch_end("user_retuned", false);
    if (operating_mode_index() != g_srch.mode) return srch_end("mode", true);   // (the receiver's requested state home)
    if (g_srch.mode == 0 && !g_srch.kerberos && now - g_srch.hold_sent >= EXC_HOLD_RENEW_MS && heimdall_up()) {
        heimdall_hold(static_cast<int>(EXC_HOLD_LEASE_MS));
        g_srch.hold_sent = now;
    }
    const double c = g_srch.step < g_srch.centres.size() ? g_srch.centres[g_srch.step] : 0;
    auto next = [&] {
        srch_remove_probes();
        g_srch.step_ms.push_back(now - g_srch.t_step);
        g_srch.step++;
        g_srch.st = 0;
        g_srch.cands.clear();
        g_status_now = true;
    };
    switch (g_srch.st) {
        case 0: {
            if (g_srch.step >= g_srch.centres.size()) return srch_end("done", true);
            char f[64];
            if (g_srch.mode == 2) snprintf(f, sizeof f, "TUNER_FREQ:%d:%.6f", g_srch.tuner, c / 1e6);
            else snprintf(f, sizeof f, "FREQ:%.6f", c / 1e6);
            dispatch({f});
            g_srch.asked = g_srch.mode == 2 ? indep_tuner_freq_hz[g_srch.tuner].load() : static_cast<double>(requested_freq_hz.load());
            g_srch.st = 1;
            g_srch.t_st = g_srch.t_step = now;
            return;
        }
        case 1:
            // the stream at the new centre AND a spectrum frame taken there: the average reset now (frames of the
            // old centre still queued can't land in it), counted from here
            if (srch_frames(c) >= 1 && !server_retuning.load()) {
                fft_reset_generation.fetch_add(1, std::memory_order_release);
                fft_data_valid.store(false, std::memory_order_release);
                g_srch.frames0 = srch_frames(c);
                g_srch.st = 2;
                g_srch.t_st = now;
            } else if (now - g_srch.t_st > SRCH_ARRIVE_MAX_MS * 3) {
                if (!heimdall_up()) return srch_end("heimdall", true);   // (home asked for: home when the stream is back)
                next();
            }
            return;
        case 2:
            // the average refilled by frames taken here (+1: the reset lands on the next one)
            if (srch_frames(c) >= g_srch.frames0 + srch_refill_frames() + 1) {
                g_srch.look1.clear();
                for (const auto& cd : srch_carriers(c, g_srch.thr_db)) g_srch.look1.push_back(cd.hz);
                g_srch.frames0 = srch_frames(c);
                g_srch.st = 3;
                g_srch.t_st = now;
            } else if (now - g_srch.t_st > 15000) {
                next();   // (the spectrum never came: a dead stream - the next step / the end decide)
            }
            return;
        case 3: {
            if (srch_frames(c) < g_srch.frames0 + std::max<uint64_t>(3, srch_refill_frames() / 2) && now - g_srch.t_st < 15000) return;
            // a carrier in both looks (control channels are continuous), not probed already in this run, not in
            // a band an earlier step looked at (the overlap; the last step's clamp)
            const std::vector<std::string> all = trunking_plugins();
            const double tol = std::max(3000.0, g_srch.raster / 2);
            for (auto& cd : srch_carriers(c, g_srch.thr_db)) {
                bool twice = false, seen = false;
                for (double h : g_srch.look1)
                    if (std::fabs(h - cd.hz) < tol) twice = true;
                for (double h : g_srch.probed_hz)
                    if (std::fabs(h - cd.hz) < tol) seen = true;
                for (const auto& cv : g_srch.covered)
                    if (cd.hz - cd.width / 2 >= cv.first && cd.hz + cd.width / 2 <= cv.second) seen = true;
                if (!twice || seen) continue;
                g_srch.carriers++;
                g_srch.probed_hz.push_back(cd.hz);
                // a remembered control channel there: its plugin first
                std::vector<std::string> order;
                for (const auto& kv : g_cc)
                    if (std::fabs(kv.second.hz - cd.hz) < 6000 &&
                        std::find(all.begin(), all.end(), kv.second.plugin) != all.end() &&
                        std::find(order.begin(), order.end(), kv.second.plugin) == order.end())
                        order.push_back(kv.second.plugin);
                for (const auto& p : all)
                    if (std::find(order.begin(), order.end(), p) == order.end()) order.push_back(p);
                cd.todo = order;
                g_srch.cands.push_back(cd);
            }
            {
                const double h = usable_half_hz();
                g_srch.covered.push_back({c - h, c + h});
            }
            if (getenv("KRAKEN_TRUNK_SEARCH_LOG")) {
                std::cout << "Trunk following: search step " << g_srch.step << " at " << c / 1e6 << " MHz: candidates";
                std::cout << std::fixed << std::setprecision(4);
                for (const auto& cd : g_srch.cands) std::cout << " " << cd.hz / 1e6 << "(" << static_cast<int>(cd.width / 1000) << "k," << static_cast<int>(cd.level) << "dB)";
                std::cout << std::defaultfloat << std::setprecision(6) << std::endl;
            }
            if (g_srch.cands.empty()) return next();
            g_srch.tries = 0;
            for (const auto& cd : g_srch.cands) g_srch.tries += cd.todo.size();
            g_srch.probed0 = g_srch.probed;
            g_srch.st = 4;
            g_status_now = true;   // (the page sees the probing phase, however short)
            srch_probe_batch();
            return;
        }
        case 4: {
            bool live = false;
            for (auto& p : g_srch.probes) {
                if (p.done) continue;
                auto inst = decimator_manager.getDecimator(p.vfo);
                auto dd = inst && inst->serial == p.serial ? inst->getDigital() : nullptr;
                // (its age in STREAM time since it was made: a slow receiver's plugin that starts late isn't cut short;
                // a wall-clock ceiling only against a dead stream)
                const double age_s = static_cast<double>(srch_frames(c) - std::min(srch_frames(c), p.f0)) / srch_fps();
                const int64_t age = static_cast<int64_t>(age_s * 1000) + (now - p.t0 > 30000 ? SRCH_PROBE_MAX_MS : 0);
                const bool frames = dd && dd->frames_within(1500);
                const double key_wait_s = static_cast<double>(srch_frames(c) - std::min(srch_frames(c), p.f_ctl)) / srch_fps();
                if (p.control && (!p.key.empty() || key_wait_s * 1000 >= SRCH_KEY_WAIT_MS || age >= SRCH_PROBE_MAX_MS ||
                                  now - p.t0 > 30000)) {   // (the control flag: always recorded, with or without its key)
                    srch_record(p);
                    g_srch.found_hz.push_back({g_srch.cands[p.cand].hz, 0});
                    p.done = true;
                } else if (age >= SRCH_PROBE_MAX_MS || (age >= SRCH_PROBE_MIN_MS && !frames && !p.control) ||
                           g_srch.cands[p.cand].found) {
                    p.done = true;
                    // (its plugin decoded the carrier - valid frames - but it is no control channel: no other
                    // plugin needs to try it - a traffic channel of that protocol)
                    if (frames && !p.control) g_srch.cands[p.cand].todo.clear();
                } else {
                    live = true;
                }
            }
            if (live) return;
            srch_remove_probes();
            bool more = false;
            for (const auto& cd : g_srch.cands)
                if (!cd.found && !cd.todo.empty()) more = true;
            if (more) {
                if (static_cast<int>(decimator_manager.getDecimatorCount()) >= VFO_LIMIT) {   // (the user added VFOs)
                    if (!g_srch.t_noroom) g_srch.t_noroom = now;
                    if (now - g_srch.t_noroom > 10000) return srch_end("no_vfo", true);
                    return;
                }
                g_srch.t_noroom = 0;
                srch_probe_batch();
            } else {
                next();
            }
            return;
        }
    }
}

std::string srch_status_json(int64_t now) {
    std::ostringstream o;
    o << "\"search\":";
    if (!g_srch.active) {
        o << "null";
    } else {
        const size_t n = g_srch.centres.size();
        // progress inside a step counts: the share of its tries done (a probe step can last tens of seconds);
        // the time left from the elapsed time and that fraction - null while there is nothing to base it on
        double frac = 0;
        if (g_srch.st == 4 && g_srch.tries)
            frac = std::clamp(static_cast<double>(g_srch.probed - g_srch.probed0) / static_cast<double>(g_srch.tries), 0.0, 0.95);
        else if (g_srch.st >= 2)
            frac = 0.05;
        const double done = n ? (static_cast<double>(g_srch.step) + frac) / static_cast<double>(n) : 1.0;
        const int pct = static_cast<int>(std::floor(100.0 * done));
        std::string eta = "null";
        if (done >= 0.05 && !(g_srch.step == 0 && g_srch.st < 4)) {
            const double el = static_cast<double>(now - g_srch.t_start) / 1000.0;
            eta = std::to_string(std::max<long long>(1, std::llround(el * (1.0 - done) / done)));
        }
        int64_t avg = 0;
        for (int64_t x : g_srch.step_ms) avg += x;
        avg = g_srch.step_ms.empty() ? 2000 : avg / static_cast<int64_t>(g_srch.step_ms.size());
        static const char* names[] = {"tuning", "tuning", "settling", "looking", "probing"};
        o << "{\"since\":" << g_srch.w_start << ",\"ranges\":[";
        for (size_t i = 0; i < g_srch.ranges.size(); i++)
            o << (i ? "," : "") << "[" << num_json(g_srch.ranges[i].first) << "," << num_json(g_srch.ranges[i].second) << "]";
        o << "],\"mode\":\"" << (g_srch.mode == 2 ? "independent" : "coherent") << "\",\"tuner\":"
          << (g_srch.mode == 2 ? g_srch.tuner : -1) << ",\"threshold_db\":" << g_srch.thr_db << ",\"raster_hz\":" << g_srch.raster << ",\"home_hz\":" << num_json(g_srch.home)
          << ",\"centre_hz\":" << num_json(g_srch.step < n ? g_srch.centres[g_srch.step] : NAN) << ",\"step\":" << g_srch.step
          << ",\"steps\":" << n << ",\"pct\":" << pct << ",\"eta_s\":" << eta
          << ",\"phase\":\"" << names[std::clamp(g_srch.st, 0, 4)] << "\",\"carriers\":" << g_srch.carriers << ",\"probing\":"
          << g_srch.probes.size() << ",\"probed\":" << g_srch.probed << ",\"found_new\":" << g_srch.found_new << ",\"found_known\":"
          << g_srch.found_known << ",\"elapsed_ms\":" << (now - g_srch.t_start) << ",\"frames\":" << (g_srch.step < n ? srch_frames(g_srch.centres[g_srch.step]) : 0) << ",\"frame_hz\":" << fft_frame_hz[srch_ch()].load() << "}";
    }
    o << ",\"search_last\":" << (g_srch_last.empty() ? "null" : g_srch_last);
    // the control VFOs a click made, and the entries they follow
    o << ",\"cc_followed\":[";
    bool f = true;
    for (const auto& kv : g_cc_follow) {
        o << (f ? "" : ",") << "{\"vfo\":" << kv.first << ",\"id\":" << kv.second.second << "}";
        f = false;
    }
    o << "]";
    return o.str();
}

// a control VFO in normal use reporting control = true: remembered (heard)
void cc_heard(int vfo, const std::string& plugin, const PluginEvent& ev) {
    if (playback::active() || g_srch.active || g_exc.st != 0) return;
    const int64_t now = now_ms();
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst) return;
    const double rf = vfo_rf(*inst);
    if (rf <= 0) return;
    auto it = g_cc_heard.find(vfo);
    if (it != g_cc_heard.end() && now - it->second < CC_HEARD_MS) {
        // (within the minute: only what it says about its system - the key comes after the control flag)
        if (CcEntry* e = cc_find(plugin, snap(rf, 6250), 3125)) {
            const std::string sys = ev.sys_key.empty() ? "" : plugin + ":" + ev.sys_key;
            if ((!sys.empty() && e->sys != sys) || (!ev.sys_site.empty() && e->site != clean(ev.sys_site, 64)) ||
                (!ev.sys_name.empty() && e->sys_name != clean(ev.sys_name, 64))) {
                if (!sys.empty()) e->sys = sys;
                if (!ev.sys_name.empty()) e->sys_name = clean(ev.sys_name, 64);
                if (!ev.sys_site.empty()) e->site = clean(ev.sys_site, 64);
                cc_changed();
            }
        }
        return;
    }
    g_cc_heard[vfo] = now;
    bool made = false;
    CcEntry before;
    if (const CcEntry* x = cc_find(plugin, snap(rf, 6250), 3125)) before = *x;
    const int64_t dirty0 = g_cc_dirty;
    const bool list0 = g_cc_list_now;
    CcEntry* e = cc_seen(plugin, snap(rf, 6250), rf, ev.sys_key, ev.sys_name, ev.sys_site, vfo_plugin_opts(vfo, plugin), NAN, "heard",
                         3125, &made);
    if (e && !made && e->sys == before.sys && e->sys_name == before.sys_name && e->site == before.site && e->opts == before.opts) {
        // (only last / n changed: the list and the file at most every CC_STATS_MS - the page shows the VFO anyway)
        g_cc_list_now = list0;   // (as before this refresh)
        g_cc_dirty = dirty0;
        g_cc_stats_dirty = true;
    }
    if (e && made) std::cout << "Trunk following: control channel " << e->hz / 1e6 << " MHz (" << plugin << ") remembered" << std::endl;
}

// click to follow: the remembered control channel id on a control VFO, following on
std::string cc_follow(uint64_t id, bool add) {
    auto it = g_cc.find(id);
    if (it == g_cc.end()) return "no such control channel";
    if (playback::active()) return "not during a playback";
    if (scanning()) return "a scanner runs - stop it first";
    CcEntry& e = it->second;
    dig::PluginInfo pi;
    if (!plugin_info(e.plugin, &pi) || !pi.built) return "its decoder (" + e.plugin + ") is not built";
    if (!pi.trunking) return "its decoder (" + e.plugin + ") does not declare trunking";
    const int mode = operating_mode_index();
    if (mode == 1) return "not in wideband scan mode";
    const double bw = std::max(12500.0, std::min(50000.0, pi.min_vfo_rate));
    const double need = bw / 2 + PLACE_GUARD_HZ, half = usable_half_hz();
    // where the receiver WILL be (a search / an excursion goes home first; the stream may still show a step)
    int tuner = 0;
    double centre = 0;
    if (mode == 2) {
        const std::set<int> ct = vt_control_tuners();
        tuner = ct.empty() ? 0 : *ct.begin();
        centre = indep_tuner_freq_hz[tuner].load();
        if (g_srch.active && g_srch.mode == 2 && g_srch.tuner == tuner) centre = g_srch.home;
        if (g_vt[tuner].taken) centre = g_vt[tuner].home_hz;
    } else {
        centre = g_srch.active ? g_srch.home : g_exc.st == 1 ? g_exc.home : static_cast<double>(requested_freq_hz.load());
    }
    if (centre <= 0) return "the receiver's frequency is not known yet";
    auto rf_of = [&](const Inst& i) { return centre + i.frequency_offset_hz.load(); };
    // the VFO a previous click made (replaced) - unless adding
    int reuse = -1;
    if (!add)
        for (const auto& kv : g_cc_follow) {
            auto inst = decimator_manager.getDecimator(kv.first);
            if (inst && inst->serial == kv.second.first) reuse = kv.first;
        }
    // (already followed: a double click - nothing re-applied; add of the same channel: no second VFO)
    for (const auto& kv : g_cc_follow) {
        auto inst = decimator_manager.getDecimator(kv.first);
        if (!inst || inst->serial != kv.second.first || kv.second.second != id) continue;
        auto dd = inst->getDigital();
        if (dd && dd->plugin_id() == e.plugin && std::fabs(rf_of(*inst) - e.hz) < 1000) {
            if (add) return "already followed on D" + std::to_string(kv.first);
            return "";
        }
    }
    // every refusal before anything changes
    double c = centre;
    const bool fits = std::fabs(e.hz - centre) + need <= half;
    if (!fits) {
        bool others = false;
        for (const auto& kv : g_cc_follow)
            if (auto inst = decimator_manager.getDecimator(kv.first); inst && inst->serial == kv.second.first) others = true;
        if (add && others) return "it doesn't fit the span with the control channel(s) followed now - follow it alone";
        c = e.hz + 250000;
        if (System* s2 = sys_of(e.sys)) {
            Centre sc;
            if (suggest_centre(*s2, &sc) && std::fabs(sc.hz - e.hz) + need <= half && exc_range(sc.hz)) c = std::round(sc.hz);
        }
        if (!exc_range(c)) c = e.hz - 250000;
        if (!exc_range(c)) return "outside the tuner's range";
    }
    if (reuse < 0 && static_cast<int>(decimator_manager.getDecimatorCount()) >= VFO_LIMIT) return "16 VFOs in use";
    // now: a search / an excursion ends (home), voice tuners back
    if (g_srch.active) srch_end("follow", true);
    if (g_exc.st == 1) exc_return("follow", true);
    if (mode == 2) vt_restore_all("follow", true);
    if (!fits) {
        char f[96];
        if (mode == 2) snprintf(f, sizeof f, "TUNER_FREQ:%d:%.6f", tuner, c / 1e6);
        else snprintf(f, sizeof f, "FREQ:%.6f", c / 1e6);
        const std::string why = ControlHandler::apply_command(f);   // (the user's tuning - he asked for it: saved)
        if (!why.empty()) return "the retune was refused: " + why;
    }
    int vfo = reuse;
    if (vfo < 0) {
        std::set<int> before;
        for (const auto& d : decimator_manager.getDecimatorInfoList()) before.insert(d.id);
        const std::string why = ControlHandler::apply_command("ADD_DECIMATOR");
        if (!why.empty()) return "a VFO couldn't be added: " + why;
        for (const auto& d : decimator_manager.getDecimatorInfoList())
            if (!before.count(d.id)) vfo = d.id;
        if (vfo < 0) return "a VFO couldn't be added";
    }
    // the plugin options of a replaced channel go back to the defaults (only the entry's own are set)
    if (auto inst = decimator_manager.getDecimator(vfo)) {
        if (auto dd = inst->getDigital()) {
            dig::Options o = dd->options();
            if (!o.plugin.empty()) {
                o.plugin.clear();
                decimator_manager.setDigitalOptions(vfo, o);
            }
        }
    }
    const std::string sid = std::to_string(vfo);
    std::vector<std::string> c2;
    c2.push_back("SET_DECIMATOR_BW_HZ:" + sid + ":" + std::to_string(efficient_bandwidth_at_least(static_cast<int>(bw))));
    char f[96];
    if (mode == 2) snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f:%d", vfo, (e.hz - c) / 1000.0, tuner);
    else snprintf(f, sizeof f, "SET_DECIMATOR_FREQ:%d:%.3f", vfo, (e.hz - c) / 1000.0);
    c2.push_back(f);
    std::string refused;
    for (const auto& kv : e.opts) c2.push_back("DIGITAL_OPT:" + sid + ":" + e.plugin + "." + kv.first + ":" + kv.second);
    c2.push_back("DIGITAL_MODE:" + sid + ":PLUGIN:" + e.plugin);
    for (const auto& cmd : c2) {
        const std::string why = ControlHandler::apply_command(cmd);   // (the user's VFO now: saved)
        if (!why.empty()) refused += (refused.empty() ? "" : "; ") + cmd.substr(0, 80) + ": " + why;
    }
    auto inst = decimator_manager.getDecimator(vfo);
    if (!add) g_cc_follow.clear();
    if (inst) g_cc_follow[vfo] = {inst->serial, e.id};
    e.followed = wall_ms();
    cc_changed();
    if (!g_set.follow) {
        g_set.follow = true;
        SettingsStore::record("TRUNK_FOLLOW:1");
        std::cout << "Trunk following: on" << std::endl;
    }
    std::cout << "Trunk following: following the control channel " << e.hz / 1e6 << " MHz (" << e.plugin << ") on D" << vfo << std::endl;
    g_status_now = true;
    return refused.empty() ? "" : "followed on D" + sid + ", but not applied: " + refused;
}

// the clicked control VFOs: a VFO gone, or changed by hand (another plugin, or moved more than 3 kHz off its
// entry), is no longer the followed one; with none (a restart), a control VFO on a followed entry's frequency
// with its plugin is (the most recently followed entry)
void cc_follow_tick() {
    for (auto it = g_cc_follow.begin(); it != g_cc_follow.end();) {
        auto inst = decimator_manager.getDecimator(it->first);
        auto e = g_cc.find(it->second.second);
        auto dd = inst ? inst->getDigital() : nullptr;
        const bool ok = inst && inst->serial == it->second.first && e != g_cc.end() && dd && dd->plugin_id() == e->second.plugin &&
                        std::fabs(vfo_rf(*inst) - e->second.hz) <= 3000;
        if (ok) ++it;
        else {
            it = g_cc_follow.erase(it);
            g_status_now = true;
        }
    }
    if (!g_cc_follow.empty() || g_srch.active || g_exc.st != 0) return;
    for (int id : control_vfos()) {
        auto inst = decimator_manager.getDecimator(id);
        auto dd = inst ? inst->getDigital() : nullptr;
        if (!dd) continue;
        const CcEntry* best = nullptr;
        for (const auto& kv : g_cc)
            if (kv.second.followed > 0 && kv.second.plugin == dd->plugin_id() && std::fabs(vfo_rf(*inst) - kv.second.hz) <= 3000 &&
                (!best || kv.second.followed > best->followed))
                best = &kv.second;
        if (best) {
            g_cc_follow[id] = {inst->serial, best->id};
            g_status_now = true;
            return;
        }
    }
}

// main thread's hook (ControlHandler::apply_command, before a command runs):
// commands that end an excursion - the user's retune (no return: he took the
// receiver), a mode change / scanner / playback (home first, then the command)
const char* exc_before_command(std::string_view m) {
    if (g_dispatching) return nullptr;
    if (g_srch.active) {
        if (m.starts_with("SCANNER_START") || m.starts_with("CONTINUOUS_SCANNER_START") || m.starts_with("SFSCAN_START"))
            return "a control-channel search runs - stop it first";
        if (m.starts_with("PLAYBACK_START:") || m.starts_with("PLAYBACK_OPEN:")) return "a control-channel search runs - stop it first";
        if (m.starts_with("FREQ:") || m.starts_with("WIDEBAND_FREQ:")) srch_end("user_retuned", false);
        else if (m.starts_with("OPERATING_MODE:") || m.starts_with("WIDEBAND_MODE:")) srch_end("mode", true);
    }
    // the user's own retune / decoder change of a control VFO within the
    // expected-reset window of an excursion's retune: its RESET is his
    for (const char* p : {"SET_DECIMATOR_FREQ:", "DIGITAL_MODE:", "SET_DECIMATOR_BW_HZ:", "REMOVE_DECIMATOR:"})
        if (m.starts_with(p)) {
            const std::string_view r = m.substr(std::strlen(p));
            int id = -1;
            std::from_chars(r.data(), r.data() + r.size(), id);
            auto it = g_tracks.find(id);
            if (it != g_tracks.end() && !voice_of_vfo(id)) it->second.expect_reset_until = 0;
        }
    // voice tuners: a mode change / scanner / playback - back first; the user's
    // own tuning of every tuner - his (no restore)
    if (m.starts_with("OPERATING_MODE:") || m.starts_with("SCANNER_START") || m.starts_with("CONTINUOUS_SCANNER_START") ||
        m.starts_with("SFSCAN_START") || m.starts_with("PLAYBACK_START:") || m.starts_with("PLAYBACK_OPEN:"))
        vt_restore_all(m.starts_with("OPERATING_MODE:") ? "mode" : m.starts_with("PLAYBACK_") ? "playback" : "scanner", true);
    else if ((m.starts_with("FREQ:") && operating_mode_index() == 2) || m.starts_with("TUNERS:"))
        for (auto& v : g_vt) v = VTuner{};
    if (g_exc.st == 0) return nullptr;
    if (m.starts_with("FREQ:") || m.starts_with("WIDEBAND_FREQ:")) {
        if (g_exc.st == 1) exc_return("user_retuned", false);
        else g_exc.st = 0;   // (returning: the user's tuning from now on)
        g_status_now = true;
        return nullptr;
    }
    const char* why = nullptr;
    if (m.starts_with("OPERATING_MODE:") || m.starts_with("WIDEBAND_MODE:")) why = "mode";
    else if (m.starts_with("SCANNER_START") || m.starts_with("CONTINUOUS_SCANNER_START") || m.starts_with("SFSCAN_START")) why = "scanner";
    else if (m.starts_with("PLAYBACK_START:") || m.starts_with("PLAYBACK_OPEN:")) why = "playback";
    if (!why) return nullptr;
    if (g_exc.st == 1) exc_return(why, true);
    g_exc.st = 0;   // (no waiting for the control channel: the command takes the receiver)
    g_status_now = true;
    return nullptr;
}

void tick_exc() {
    const int64_t now = now_ms();
    for (auto it = g_exc_retry.begin(); it != g_exc_retry.end();) it = now >= it->second ? g_exc_retry.erase(it) : std::next(it);
    if (g_exc.st == 0) return;
    if (playback::active()) {   // (the hook went home first; a playback that started otherwise: no commands to heimdall)
        if (g_exc.st == 1) exc_return("playback", false);
        g_exc.st = 0;
        return;
    }
    const double at = static_cast<double>(coherent_tuner_hz());
    if (g_exc.st == 2) {
        if (!g_exc.t_back_arr && std::fabs(at - g_exc.home) < 1000) {
            g_exc.t_back_arr = now;
            g_exc_stats.back_arrive_ms = now - g_exc.t_back;
        }
        if (g_exc.t_back_arr && !g_exc.t_back_cal && exc_calibrated() && now - g_exc.t_back_arr > 100) {
            g_exc.t_back_cal = now;
            g_exc_stats.back_cal_ms = now - g_exc.t_back_arr;
        }
        bool rx = false;
        for (const auto& e : g_exc.ctl) {
            auto tr = g_tracks.find(e.vfo);
            if (tr == g_tracks.end()) continue;
            if (System* s = sys_of(tr->second.sys)) {
                auto c = s->ctl.find(e.vfo);
                if (c != s->ctl.end() && c->second.last_ms > g_exc.t_back + 300) rx = true;
            }
        }
        if (rx) g_exc_stats.ctl_back_ms = now - g_exc.t_back;
        if (rx || now - g_exc.t_back > EXC_BACK_MAX_MS || g_exc.ctl.empty()) {
            g_exc.st = 0;
            g_status_now = true;
        }
        return;
    }
    // away
    if (!g_set.follow || !g_set.retune) return exc_return("off", true);
    if (operating_mode_index() != 0) return exc_return("mode", false);
    if (g_paused) return exc_return("scanner", false);
    if (std::fabs(static_cast<double>(requested_freq_hz.load()) - g_exc.centre) > 1000) return exc_return("user_retuned", false);
    if (!g_exc.t_arrived && std::fabs(at - g_exc.centre) < 1000) {
        g_exc.t_arrived = now;
        g_exc_stats.arrive_ms = now - g_exc.t_left;
        auto it = g_calls.find(g_exc.call);
        if (it != g_calls.end() && it->second.vfo < 0 && it->second.st != RECENT) try_follow(it->second);
    }
    if (g_exc.t_arrived && !g_exc.t_cal && exc_calibrated() && now - g_exc.t_arrived > 100) {
        g_exc.t_cal = now;
        g_exc_stats.cal_ms = now - g_exc.t_arrived;
    }
    if (!g_exc.cal && !g_exc.kerberos && now - g_exc.hold_sent >= EXC_HOLD_RENEW_MS && heimdall_up()) {
        heimdall_hold(static_cast<int>(EXC_HOLD_LEASE_MS));
        g_exc.hold_sent = now;
        g_exc.holding = true;
    }
    // while it calibrates the voice VFOs hear the noise source: their SETUP clock waits
    const bool calibrating = g_exc.cal && !exc_calibrated() && g_exc.t_arrived && now - g_exc.t_arrived < 5000;
    bool busy = false;
    for (auto& kv : g_calls) {
        Call& c = kv.second;
        if (c.vfo < 0 || c.st == RECENT) continue;
        busy = true;
        if (calibrating && c.st == SETUP) c.t_placed = now;
        if (c.id == g_exc.call && g_exc_stats.first_frames_ms < 0 && c.st == ACTIVE) g_exc_stats.first_frames_ms = now - g_exc.t_left;
    }
    if (g_set.retune_max_s > 0 && now - g_exc.t_left > static_cast<int64_t>(g_set.retune_max_s * 1000)) return exc_return("limit", true);
    auto it = g_calls.find(g_exc.call);
    if (it != g_calls.end() && it->second.st == RECENT && it->second.end == "no_signal" && !busy) return exc_return("no_signal", true);
    if (busy) {
        g_exc.t_idle = 0;
        return;
    }
    if (!g_exc.t_arrived) {
        if (now - g_exc.t_left > 5000) return exc_return("not_arrived", true);
        return;
    }
    if (!g_exc.t_idle) g_exc.t_idle = now;
    // (the next over of a system that grants every transmission anew: within the hang time)
    if (now - g_exc.t_idle >= static_cast<int64_t>(g_set.hang_s * 1000)) return exc_return("call_over", true);
}

void pause_check() {
    std::string why;
    if (sfscan::running()) why = "saved-frequency scan";
    else if (scanner_manager.isRunning()) why = "discrete scanner";
    else if (continuous_scanner.isRunning()) why = "continuous scanner";
    else if (searching()) why = "control-channel search";
    const bool p = !why.empty();
    if (!p && !g_paused && g_set.follow && g_exc.st == 0) note_homes();   // (following off: nothing is put back; away: home's stand)
    if (!g_set.follow) g_home.clear();
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
        if (resumed && !playback::active() && g_set.follow && !g_srch_skip_restore) restore_homes();
        if (resumed && g_srch_skip_restore) {
            g_srch_skip_restore = false;
            g_home.clear();
        }
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
        if (frames || analog) v->active_here = true;
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
        if (!dd || inst->digital_serial.load() != it->second.dec) {
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
    // a hold on a system that is gone: released
    if (!g_hold_sys.empty() && !sys_of(g_hold_sys)) {
        std::cout << "Trunk following: the held system " << g_hold_sys << " is gone - hold released" << std::endl;
        g_hold_sys.clear();
        g_hold_tg.clear();
        g_status_now = true;
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

// with following off only a user's edit is written (the air changes nothing on disk)
int64_t g_unsaved_t = 0;
void tick_save() {
    if (!g_dirty_since || playback::active()) return;
    if (!g_set.follow && !g_edit_dirty) return;
    const int64_t now = now_ms();
    if (now - g_dirty_since >= g_dirty_within) {
        save();
    } else if (now - g_unsaved_t >= 1000) {
        // save_now's fallback if the loop doesn't answer at shutdown: at most 1 s old
        g_unsaved_t = now;
        const std::string t = serialise();
        std::lock_guard<std::mutex> lk(g_save_mu);
        g_unsaved = t;
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

void on_plugin_event(int vfo, uint64_t decoder, const std::string& plugin_id, PluginEvent&& ev) {
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
        // full: dropped (counted, events_dropped) - except a RESET: it says the
        // VFO's earlier state is stale, and losing it would keep stale calls / a
        // stale control VFO. One pending per VFO + decoder is enough (coalesced)
        static const size_t qmax = [] {
            const char* e = getenv("KRAKEN_TRUNK_QUEUE_MAX");   // tests: a smaller bound
            const long v = e ? atol(e) : 0;
            return v >= 8 && v <= 100000 ? static_cast<size_t>(v) : QUEUE_MAX;
        }();
        if (g_queue.size() >= qmax) {
            // (only when that VFO + decoder's LAST queued event is a RESET: a RESET
            // before later events of the VFO doesn't make those stale)
            bool pending = false;
            if (ev.kind == PluginEvent::RESET)
                for (auto it = g_queue.rbegin(); it != g_queue.rend(); ++it)
                    if (it->vfo == vfo && it->dec == decoder) {
                        pending = it->ev.kind == PluginEvent::RESET;
                        break;
                    }
            if (ev.kind != PluginEvent::RESET || pending) {
                (ev.kind == PluginEvent::RESET ? g_resets_coalesced : g_events_dropped).fetch_add(1, std::memory_order_relaxed);
                return;
            }
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
    uint64_t p = 0;
    {
        std::lock_guard<std::mutex> lk(g_meta_mu);
        auto it = g_meta.find(id);
        if (it == g_meta.end() || !it->second.voice) return false;   // (voice = a voice VFO or a search probe)
        p = it->second.inst;
    }
    auto now = decimator_manager.getDecimator(id);
    return now && now->serial == p;
}

std::string vfo_info_json(int id) {
    uint64_t p = 0;
    std::string j;
    {
        std::lock_guard<std::mutex> lk(g_meta_mu);
        auto it = g_meta.find(id);
        if (it == g_meta.end()) return "";
        p = it->second.inst;
        j = it->second.json;
    }
    auto now = decimator_manager.getDecimator(id);
    return now && now->serial == p ? j : "";
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
        tick_exc();
        tick_vt();
        tick_search();
        tick_cc();
        tick_voices();
        tick_lists();
        tick_save();
        send_status(g_status_now && now_ms() - g_status_sent >= STATUS_MIN_MS);
        if (now_ms() % 5000 < TICK_MS) prune_learn();
    } catch (const std::exception& e) {
        std::cerr << "Trunk following: " << e.what() << std::endl;
    }
}

void cc_load_public();
void load() {
    cc_load_public();
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
        // kept under a name nothing overwrites: .bad-<date>-<time>[-n]
        char ts[32];
        const std::time_t tt = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
        std::string bad = std::string(SYSTEMS_FILE) + ".bad-" + ts;
        for (int n = 1; std::ifstream(bad).good() && n < 1000; n++) bad = std::string(SYSTEMS_FILE) + ".bad-" + ts + "-" + std::to_string(n);
        f.close();
        fs_replace(SYSTEMS_FILE, bad.c_str());
        g_file_bad = bad;
        g_file_note = text.size() > SYSTEMS_FILE_MAX ? "not loaded: larger than " + std::to_string(SYSTEMS_FILE_MAX >> 20) + " MB"
                                                     : "not loaded: unreadable";
        std::cerr << "Trunk following: " << SYSTEMS_FILE << " " << g_file_note << " (kept as " << bad
                  << ") - starting without systems" << std::endl;
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
                e.last = int_or(jnum(g, "last", 0), 0, 9000000000000000LL, 0);
                e.calls = static_cast<uint64_t>(int_or(jnum(g, "calls", 0), 0, 1LL << 53, 0));
            }
        if (const geo::Json* c = x.get("chans"); c && c->type == geo::Json::ARR)
            for (const auto& g : c->a) {
                const std::string name = clean(g.str("ch"), 32);
                if (name.empty() || s.chans.size() >= MAX_CHANS) { bad++; continue; }
                Chan& e = s.chans[name];
                e.num = int_or(jnum(g, "num", -1), -1, 1000000000000LL, -1);
                auto hz = [&](const char* k) { const double v = jnum(g, k); return hz_ok(v) ? v : NAN; };
                e.user_hz = hz("user_hz");
                e.user_up = hz("user_up");
                e.ann_hz = hz("ann_hz");
                e.ann_up = hz("ann_up");
                e.learned_hz = hz("learned_hz");
                e.grants = static_cast<uint64_t>(int_or(jnum(g, "grants", 0), 0, 1LL << 53, 0));
                e.last = int_or(jnum(g, "last", 0), 0, 9000000000000000LL, 0);
            }
        if (const geo::Json* r = x.get("rules"); r && r->type == geo::Json::ARR)
            for (const auto& g : r->a) {
                Rule e;
                e.base_hz = jnum(g, "base_hz");
                e.step_hz = jnum(g, "step_hz");
                if (const geo::Json* u = g.get("up_off_hz"); u && u->type != geo::Json::NUL) e.up_off_hz = jnum(g, "up_off_hz", INFINITY);
                if (!check_rule(&e, jnum(g, "first", -1), jnum(g, "last", -1)).empty() || s.rules.size() >= MAX_RULES) {
                    bad++;
                    continue;
                }
                s.rules.push_back(e);
            }
    }
    // what the user entered: never evicted by the air
    for (auto& kv : g_sys) {
        System& x = kv.second;
        bool u = !x.user_name.empty() || x.listed_only || !x.rules.empty();
        for (const auto& t : x.tgs) u = u || t.second.listed || !t.second.name.empty() || t.second.mode != TG_NORMAL;
        for (const auto& c : x.chans) u = u || fin(c.second.user_hz);
        x.user = u;
    }
    std::cout << "Trunk following: " << g_sys.size() << " system(s) loaded from " << SYSTEMS_FILE << std::endl;
    if (bad) std::cerr << "Trunk following: " << bad << " unreadable entr" << (bad == 1 ? "y" : "ies") << " in " << SYSTEMS_FILE
                       << " skipped" << std::endl;
}

void save_now() {
    // the loop serialises the current state (it owns it) and writes it; main
    // waits up to 2 s for that - a loop that doesn't answer: the fallback
    // (g_unsaved, at most 1 s old). Then no more writes either way
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    bool posted = false;
    if (loop) {
        loop->defer([done] {
            srch_end("shutdown", true);
            if (g_exc.st == 1) exc_return("shutdown", true);
            if (g_cc_dirty) cc_save();
            vt_restore_all("shutdown", true);
            if (g_dirty_since && !playback::active() && (g_set.follow || g_edit_dirty)) save();
            {
                std::lock_guard<std::mutex> lk(g_save_mu);
                g_shutdown = true;
            }
            done->set_value();
        });
        posted = true;
    }
    if (posted && fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) return;
    std::lock_guard<std::mutex> lk(g_save_mu);
    if (!g_shutdown && !g_unsaved.empty() && !playback::active()) write_file(g_unsaved);
    g_shutdown = true;
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
// The system a command names. A temporary one (no key yet, "<plugin>:@<vfo>")
// can't be edited - its entries would not survive the merge into the keyed
// one (allow_temp: forgetting it). create: a well-formed "<plugin>:<key>" not
// known yet is made (*created = its id - the caller removes it again when the
// command is refused: a refused command leaves nothing)
System* parse_sys(const geo::Json& j, std::string* err, bool create = false, std::string* created = nullptr,
                  bool allow_temp = false) {
    const std::string id = j.str("sys");
    if (temp_id(id) && !allow_temp) {
        *err = "this system has no key yet (its control channel hasn't named it) - edit it once it has one";
        return nullptr;
    }
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
    System* n = ensure_system(id, id.substr(0, c), id.substr(c + 1));
    if (!n) *err = "too many systems (" + std::to_string(MAX_SYSTEMS) + ", all edited or in use): delete one first";
    else if (created) *created = id;
    return n;
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
    s.user = true;   // never evicted by the air
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
    std::string created;   // a system this command made: gone again if it is refused
    auto done = [&](bool status = true) {
        if (status) g_status_now = true;
        if (!err->empty() && !created.empty()) g_sys.erase(created);
        if (!err->empty()) send_err(msg, *err);
        send_status(true, true);   // (an answer: sent even if the body is as last sent)
        return true;
    };
    if (cmd == "GET_TRUNK") {
        send_status(true, true);
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
    if (cmd == "TRUNK_SEARCH") {
        geo::Json j;
        if (!parse_obj(arg, &j, err)) return done();
        *err = srch_start(j);
        return done();
    }
    if (cmd == "TRUNK_SEARCH_STOP") {
        if (!g_srch.active) *err = "no search runs";
        else srch_end("stopped", true);
        return done();
    }
    if (cmd == "GET_TRUNK_CC") {
        g_cc_list_now = true;   // (the list at most once a second: tick_cc)
        return true;
    }
    if (cmd == "TRUNK_CC_CLEAR") {
        g_cc.clear();
        g_cc_follow.clear();
        cc_changed();
        return done();
    }
    if (cmd == "TRUNK_CC_NAME" || cmd == "TRUNK_CC_DEL" || cmd == "TRUNK_CC_FOLLOW" || cmd == "TRUNK_CC_SET") {
        geo::Json j;
        if (!parse_obj(arg, &j, err)) return done();
        if (cmd == "TRUNK_CC_SET") {
            CcEntry e;
            *err = cc_parse(j, &e);
            if (!err->empty()) return done();
            const double tol = 3125;
            bool made = false;
            CcEntry* x = nullptr;
            const long long id = int_or(jnum(j, "id", 0), 0, 1LL << 53, 0);
            auto it = id > 0 ? g_cc.find(static_cast<uint64_t>(id)) : g_cc.end();
            if (it != g_cc.end()) x = &it->second;
            else x = cc_ensure(e.plugin, e.hz, tol, &made);
            if (!x) {
                *err = "the memory is full (" + std::to_string(CC_MAX) + " of the user's entries)";
                return done();
            }
            const uint64_t keep = x->id;
            if (!made && e.first == 0) e.first = x->first;
            *x = e;
            x->id = keep;
            if (!x->first) x->first = wall_ms();
            cc_changed();
            return true;   // (answered by the list message - an import of 1000 is not 1000 statuses)
        }
        const long long id = int_or(jnum(j, "id", 0), 1, 1LL << 53, 0);
        auto it = id > 0 ? g_cc.find(static_cast<uint64_t>(id)) : g_cc.end();
        if (it == g_cc.end()) {
            *err = "no such control channel";
            return done();
        }
        if (cmd == "TRUNK_CC_NAME") {
            it->second.name = clean(j.str("name"), 64);
        } else if (cmd == "TRUNK_CC_DEL") {
            for (auto f = g_cc_follow.begin(); f != g_cc_follow.end();) f = f->second.second == it->first ? g_cc_follow.erase(f) : std::next(f);
            g_cc.erase(it);
        } else {
            *err = cc_follow(it->first, jbool(j, "add"));
            return done();
        }
        cc_changed();
        return done();
    }
    if (cmd == "TRUNK_RETURN") {
        if (g_exc.st != 1) {
            *err = "the receiver is home";
            return done();
        }
        auto it = g_calls.find(g_exc.call);
        if (it != g_calls.end()) {   // stopped by the user: its grant updates don't send the receiver out again
            Call& c = it->second;
            c.user_ended = true;
            if (c.vfo >= 0) unfollow(c, "stopped");
            else if (System* s = sys_of(c.sys)) set_why(c, *s, "stopped");
        }
        exc_return("command", true);
        return done();
    }
    if (cmd == "TRUNK_RETUNE" || cmd == "TRUNK_RETUNE_CAL") {
        bool on;
        if (!parse_bool01(arg, &on)) {
            *err = "format: " + cmd + ":0|1";
            return done();
        }
        if (cmd == "TRUNK_RETUNE") {
            if (g_set.retune != on) std::cout << "Trunk following: excursions outside the span " << (on ? "on" : "off") << std::endl;
            g_set.retune = on;
        } else {
            g_set.retune_cal = on;
        }
        *persist = cmd + ":" + (on ? "1" : "0");
        return done();
    }
    if (cmd == "TRUNK_TUNERS") {
        std::vector<int> v;
        std::string a = arg;
        bool bad = a.size() > 32;
        for (size_t i = 0; !bad && i < a.size();) {
            size_t j = a.find(',', i);
            if (j == std::string::npos) j = a.size();
            const std::string x = a.substr(i, j - i);
            if (x.size() != 1 || x[0] < '0' || x[0] > '7') bad = true;
            else if (std::find(v.begin(), v.end(), x[0] - '0') == v.end()) v.push_back(x[0] - '0');
            i = j + 1;
        }
        if (bad) {
            *err = "format: TRUNK_TUNERS:<tuner numbers 0-7, comma separated; empty = none>";
            return done();
        }
        std::sort(v.begin(), v.end());
        g_set.tuners = v;
        std::string out;
        for (int t : v) out += (out.empty() ? "" : ",") + std::to_string(t);
        *persist = "TRUNK_TUNERS:" + out;
        return done();
    }
    if (cmd == "TRUNK_RETUNE_MAX") {
        double v;
        if (!parse_number(arg, &v) || !(v == 0 || (v >= 10 && v <= 3600))) {
            *err = "format: TRUNK_RETUNE_MAX:<seconds 10-3600, 0 = no limit>";
            return done();
        }
        g_set.retune_max_s = std::round(v);
        char c[48];
        snprintf(c, sizeof c, "TRUNK_RETUNE_MAX:%lld", static_cast<long long>(g_set.retune_max_s));
        *persist = c;
        return done();
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
        long long cid = 0;
        auto it = as_int(id, 1, 1LL << 53, &cid) ? g_calls.find(static_cast<uint64_t>(cid)) : g_calls.end();
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
        System* s = parse_sys(j, err, creates, &created, cmd == "TRUNK_SYS_DEL");
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
                if (System* n = ensure_system(id, plugin, key)) n->ctl = ctl;
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
                if (g_hold_sys == s->id && g_hold_tg == tg) {   // (a hold on it: released)
                    g_hold_sys.clear();
                    g_hold_tg.clear();
                }
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
        r.base_hz = jnum(j, "base_hz");
        r.step_hz = jnum(j, "step_hz");
        if (const geo::Json* u = j.get("up_off_hz"); u && u->type != geo::Json::NUL) {
            r.up_off_hz = jnum(j, "up_off_hz", INFINITY);   // (not a number: refused below)
        }
        *err = check_rule(&r, jnum(j, "first", -1), jnum(j, "last", -1));
        if (!err->empty()) return done();
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

bool home_offset(int id, float* off) {
    if (g_exc.st != 1) return false;
    for (const auto& e : g_exc.ctl)
        if (e.vfo == id && exc_inst(e)) {
            *off = e.off;
            return true;
        }
    return false;
}

bool tuner_home(int t, double* hz, float* gain) {
    if (t < 0 || t >= MAX_CHANNELS || !g_vt[t].taken) return false;
    *hz = g_vt[t].home_hz;
    *gain = g_vt[t].home_gain;
    return true;
}

void note_packet() { g_packet_ms.store(now_ms(), std::memory_order_relaxed); }

void cc_load_public() { cc_load(); }

const char* before_command(std::string_view m) {
    if (m.starts_with("TRUNK_") || m.starts_with("GET_")) return nullptr;
    return exc_before_command(m);
}

}  // namespace trunk
