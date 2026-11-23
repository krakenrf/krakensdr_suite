// Signal ID: the signal classifier at runtime - see include/sigclass_service.hpp
// and docs/dev/signal-classifier.md

#include "sigclass_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "config.hpp"
#include "doa_logger.hpp"   // doa_is_calibrating()
#include "globals.hpp"
#include "networking/websocket_server.hpp"
#include "sigclass/sigclass.hpp"
#include "sigclass_rules.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

// EDGE_CLIP: the part of each tuner's band the spectrum shows (and the
// wideband-scan stitch is built from), 0.1 .. 1 (main.cpp)
extern std::atomic<float> current_edge_clip;

namespace sigsvc {

std::atomic<bool> g_armed{false};

namespace {

// --- tuning knobs -----------------------------------------------------------
// analysis time <= this share of the wall time (one core); the cycle period
// (capture + analysis + rest) follows the measured analysis time
constexpr double CPU_SHARE = 0.04;
constexpr double MIN_PERIOD_S = 1.0, MAX_PERIOD_S = 10.0;
// a track not seen for this long is dropped: long enough that a bursty
// signal (a pager, a DMR repeater between overs) keeps its label between
// bursts, and that independent mode's round over 8 tuners at the longest
// period still revisits each before its tracks go
constexpr double TRACK_EXPIRE_S = 45.0;
// clean packets after the last one used before a snapshot counts: heimdall's
// noise flag trails the samples by up to 2 packets (bearing-spectrum.md)
constexpr int TRAIL_PACKETS = 3;
// a capture that has not completed by then: no data (heimdall stopped?)
constexpr int CAPTURE_TIMEOUT_MS = 4000;
// after a retune: the tuner's settling packets are not used
constexpr int SETTLE_MS = 500;
// the noise source seen by the tap within this: still calibrating
constexpr int64_t NOISE_HOLD_MS = 1500;
// broadcasts at most this often; an unchanged list is refreshed this often
constexpr int64_t SEND_MIN_MS = 500, SEND_REFRESH_MS = 5000;
// tracks sent at most (the most recently seen first)
constexpr size_t MAX_TRACKS_SENT = 200;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
double now_s() { return static_cast<double>(now_ms()) / 1000.0; }

std::string num(double v, int prec) {
    if (!is_finite_value(v)) return "null";
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}
std::string str_or_null(const std::string& s) { return s.empty() ? "null" : "\"" + json_escape(s) + "\""; }

// --- worker <-> everyone ----------------------------------------------------
std::atomic<bool> g_enabled{false}, g_stop{false};
std::mutex g_mu;                 // g_cv, g_done, g_wake
std::condition_variable g_cv;
bool g_done = false;             // the armed capture ended (any result)
bool g_wake = false;             // enable / disable / stop
std::thread* g_thread = nullptr;
std::atomic<int64_t> g_noise_ms{0};   // the tap's last noise-source packet

// --- the capture (the receiver thread owns it while g_armed) ---------------
enum Result { CAP_NONE, CAP_OK, CAP_NOISE, CAP_RETUNE, CAP_LAYOUT, CAP_CANCEL };
struct Cap {
    std::vector<uint8_t> buf;   // sized once (2 x NOMINAL_SNAPSHOT_SAMPLES)
    size_t need = 0, have = 0;  // bytes
    int ch = 0;
    bool first = true;
    uint32_t channels = 0;
    float freq = 0.0f;          // the channel's header frequency
    int trail = -1;             // -1 filling, else clean packets since full
    Result result = CAP_NONE;
    uint64_t gaps = 0;
};
std::mutex g_cap_mu;   // taken by the tap only while armed; never with g_mu held by the worker
Cap g_cap;

void finish_locked(Result r) {   // g_cap_mu held
    g_cap.result = r;
    g_armed.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_done = true;
    }
    g_cv.notify_all();
}

// --- the class table (the model's metadata) --------------------------------
struct ClassMeta {
    std::string id, name, family, plugin, demod;
    double bw_hz = NAN;
    double min_p = NAN;           // the class's own "sure" threshold (NaN: the model's min_confidence)
    bool burst = false, unknown = false, fills_band = false, hidden = false;
};
struct Table {
    std::vector<ClassMeta> classes;
    const ClassMeta* find(const std::string& id) const {
        for (const auto& c : classes)
            if (c.id == id) return &c;
        return nullptr;
    }
};

// position just past the JSON value starting at p (strings, nesting)
size_t skip_value(const std::string& j, size_t p) {
    if (p >= j.size()) return p;
    if (j[p] == '"') {
        std::string tmp;
        return json_read_string(j, p, tmp);
    }
    if (j[p] == '{' || j[p] == '[') {
        int depth = 0;
        while (p < j.size()) {
            const char c = j[p];
            if (c == '"') {
                std::string tmp;
                p = json_read_string(j, p, tmp);
                continue;
            }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']') p++;
    return p;
}

// "classes": [ {...}, ... ] of the metadata's top-level object -> the table
// (only the keys the pages get; anything else is ignored)
std::shared_ptr<const Table> parse_table(const std::string& meta) {
    auto t = std::make_shared<Table>();
    // find the top-level key "classes"
    int depth = 0;
    size_t p = 0, arr = std::string::npos;
    while (p < meta.size() && arr == std::string::npos) {
        const char c = meta[p];
        if (c == '"') {
            std::string tok;
            p = json_read_string(meta, p, tok);
            size_t q = p;
            while (q < meta.size() && std::isspace(static_cast<unsigned char>(meta[q]))) q++;
            if (depth == 1 && q < meta.size() && meta[q] == ':' && tok == "classes") {
                q++;
                while (q < meta.size() && std::isspace(static_cast<unsigned char>(meta[q]))) q++;
                if (q < meta.size() && meta[q] == '[') arr = q;
                else break;
            }
            continue;
        }
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        p++;
    }
    if (arr == std::string::npos) return t;
    p = arr + 1;
    while (p < meta.size()) {
        while (p < meta.size() && (std::isspace(static_cast<unsigned char>(meta[p])) || meta[p] == ',')) p++;
        if (p >= meta.size() || meta[p] == ']') break;
        const size_t e = skip_value(meta, p);
        if (meta[p] == '{') {
            const std::string obj = meta.substr(p, e - p);
            ClassMeta m;
            std::string v;
            if (json_find(obj, "id", v)) m.id = v;
            if (json_find(obj, "name", v)) m.name = v;
            if (json_find(obj, "family", v)) m.family = v;
            if (json_find(obj, "plugin", v) && v != "null") m.plugin = v;
            if (json_find(obj, "demod", v) && v != "null") m.demod = v;
            if (json_find(obj, "bw_hz", v) && v != "null") {
                char* end = nullptr;
                const double d = std::strtod(v.c_str(), &end);
                if (end != v.c_str() && is_finite_value(d) && d > 0) m.bw_hz = d;
            }
            if (json_find(obj, "min_p", v) && v != "null") {
                char* end = nullptr;
                const double d = std::strtod(v.c_str(), &end);
                if (end != v.c_str() && is_finite_value(d) && d >= 0.0 && d <= 1.01) m.min_p = d;
            }
            if (json_find(obj, "burst", v)) m.burst = v == "true";
            if (json_find(obj, "unknown", v)) m.unknown = v == "true";
            if (json_find(obj, "fills_band", v)) m.fills_band = v == "true";
            if (json_find(obj, "hidden", v)) m.hidden = v == "true";   // e.g. none: never shown
            if (!m.id.empty()) t->classes.push_back(std::move(m));
        }
        if (e <= p) break;
        p = e;
    }
    return t;
}

// a track's band edges: the core's own when its Track has them, else the
// centre +- half the bandwidth
template <class T>
void track_band(const T& k, double& lo, double& hi) {
    if constexpr (requires { k.lo_hz; k.hi_hz; }) {
        lo = k.lo_hz;
        hi = k.hi_hz;
    } else {
        lo = k.centre_hz - 0.5 * k.bw_hz;
        hi = k.centre_hz + 0.5 * k.bw_hz;
    }
}

// --- the published state (the uWS thread reads it) -------------------------
struct PubTrack {
    int64_t id = 0;               // the run's own (rules::follow_ids, INV-I: never reused, kept over a regroup)
    int tuner = 0;                // its tracker (coherent: 0)
    double hz = 0, bw = 0, lo = 0, hi = 0;
    double seen_real = 0;         // when it was last seen (now_s clock)
    bool fresh = false;           // seen in its tuner's latest snapshot (age 0)
    float snr = 0, duty = 0, p = 0;
    bool burst = false, wide = false, unknown = false;
    int hits = 0;
    std::string cls, name, family, plugin, demod;
    double vbw = NAN;
    double min_p = NAN;           // its class's own sure threshold (null: min_confidence)
    struct Alt {
        std::string id, name;
        float p = 0;
    } alt[2];
    int n_alt = 0;
};
std::mutex g_st_mu;
int g_model_state = 0;            // 0 not tried, 1 loaded, 2 refused / missing
std::string g_reason;             // why unavailable
std::string g_parked;             // why no captures now ("" = running)
double g_analyze_ms = 0, g_period_ms = 0;
double g_min_conf = NAN;          // the model's "min_confidence" (NaN: none)
uint64_t g_snapshots = 0;
std::vector<uint64_t> g_looks;    // snapshots per tuner since the mode / the switch-on (coherent: [0])
std::vector<PubTrack> g_tracks;
std::string g_last_sig;           // the last broadcast's signature (no ages)
int64_t g_last_sent_ms = 0;
bool g_pending = false;           // a change waiting for SEND_MIN_MS

std::string build_json_locked(std::string* sig) {
    const double t = now_s();
    const bool on = g_enabled.load();
    std::ostringstream o, s;
    o << "{\"sigclass\":{\"enabled\":" << (on ? "true" : "false")
      << ",\"available\":" << (g_model_state != 2 ? "true" : "false") << ",\"reason\":\"" << json_escape(g_reason)
      << "\",\"parked\":\"" << json_escape(on ? g_parked : "") << "\",\"analyze_ms\":" << num(g_analyze_ms, 1)
      << ",\"period_ms\":" << num(g_period_ms, 0)
      << ",\"duty\":" << num(g_period_ms > 0 ? g_analyze_ms / g_period_ms : 0.0, 4) << ",\"snapshots\":" << g_snapshots
      << ",\"looks\":[";
    for (size_t i = 0; i < g_looks.size(); i++) o << (i ? "," : "") << g_looks[i];
    o << "],\"min_confidence\":" << num(g_min_conf, 3) << ",\"tracks\":[";
    s << on << '|' << g_model_state << '|' << g_reason << '|' << (on ? g_parked : "") << '|';
    bool first = true;
    static const std::vector<PubTrack> none;   // (off: no tracks)
    for (const PubTrack& k : on ? g_tracks : none) {
        // age: 0 while its tuner's latest snapshot had it (with tuners in
        // turn a round can take a minute), else the time since it was seen
        const double age = k.fresh ? 0.0 : std::max(0.0, t - k.seen_real);
        o << (first ? "" : ",") << "{\"id\":" << k.id << ",\"hz\":" << num(k.hz, 0) << ",\"bw\":" << num(k.bw, 0)
          << ",\"lo\":" << num(k.lo, 0) << ",\"hi\":" << num(k.hi, 0) << ",\"tuner\":" << k.tuner
          << ",\"snr\":" << num(k.snr, 1) << ",\"duty\":" << num(k.duty, 2) << ",\"burst\":" << (k.burst ? "true" : "false")
          << ",\"wide\":" << (k.wide ? "true" : "false") << ",\"cls\":" << str_or_null(k.cls)
          << ",\"name\":" << str_or_null(k.name) << ",\"family\":" << str_or_null(k.family) << ",\"p\":" << num(k.p, 3)
          << ",\"alt\":[";
        for (int a = 0; a < k.n_alt; a++)
            o << (a ? "," : "") << "[\"" << json_escape(k.alt[a].id) << "\"," << num(k.alt[a].p, 3) << ",\""
              << json_escape(k.alt[a].name) << "\"]";
        o << "],\"plugin\":" << str_or_null(k.plugin) << ",\"demod\":" << str_or_null(k.demod)
          << ",\"vbw\":" << num(k.vbw, 0) << ",\"min_p\":" << num(k.min_p, 3) << ",\"age\":" << num(age, 1) << ",\"hits\":" << k.hits
          << ",\"unknown\":" << (k.unknown ? "true" : "false") << "}";
        s << k.id << ':' << k.cls << ':' << std::lround(k.p * 50) << ':' << std::llround(k.hz / 200) << ':'
          << std::llround(std::log(std::max(k.bw, 1.0)) * 10) << ':' << k.fresh << ';';
        first = false;
    }
    o << "]}}";
    if (sig) *sig = s.str();
    return o.str();
}

// tell the pages: a change goes out at once unless one went out within
// SEND_MIN_MS (then the worker sends it soon - flush_pending); an unchanged
// state only every SEND_REFRESH_MS (the ages, the analysis numbers)
void publish(bool force) {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_st_mu);
        std::string sig;
        const std::string j = build_json_locked(&sig);
        const int64_t t = now_ms();
        const bool changed = sig != g_last_sig;
        if (!force && !changed && t - g_last_sent_ms < SEND_REFRESH_MS) return;
        if (!force && t - g_last_sent_ms < SEND_MIN_MS) {
            g_pending = true;
            return;
        }
        g_pending = false;
        g_last_sig = sig;
        g_last_sent_ms = t;
        msg = j;
    }
    WebSocketServer::broadcast_json_message(msg);
}
void flush_pending() {
    bool p;
    {
        std::lock_guard<std::mutex> lk(g_st_mu);
        p = g_pending && now_ms() - g_last_sent_ms >= SEND_MIN_MS;
    }
    if (p) publish(false);
}
void set_parked(const std::string& why) {
    bool changed;
    {
        std::lock_guard<std::mutex> lk(g_st_mu);
        changed = g_parked != why;
        g_parked = why;
    }
    if (changed) publish(false);
}
void clear_tracks_pub() {
    std::lock_guard<std::mutex> lk(g_st_mu);
    g_tracks.clear();
    g_looks.clear();
}

// --- the worker ---------------------------------------------------------------
// waits until `ms` passed, or enable / disable / stop (true = woken); sends a
// pending broadcast meanwhile
bool rest(int64_t ms) {
    const int64_t end = now_ms() + std::max<int64_t>(0, ms);
    const int mode0 = operating_mode_index();
    while (!g_stop.load()) {
        const int64_t left = end - now_ms();
        if (left <= 0) return false;
        std::unique_lock<std::mutex> lk(g_mu);
        if (g_cv.wait_for(lk, std::chrono::milliseconds(std::min<int64_t>(left, 100)), [] { return g_wake; })) {
            g_wake = false;
            return true;
        }
        lk.unlock();
        // an operating-mode change: the old mode's tracks must leave the
        // pages now, not after this rest (up to a period, 10 s)
        if (operating_mode_index() != mode0) return true;
        flush_pending();
    }
    return true;
}

void cancel_capture() {
    std::lock_guard<std::mutex> lk(g_cap_mu);
    if (g_armed.load()) {
        g_cap.result = CAP_CANCEL;
        g_armed.store(false, std::memory_order_release);
    }
}

// arm the tap on channel ch and wait for the result
Result capture(int ch) {
    {
        std::lock_guard<std::mutex> lk(g_cap_mu);
        Cap& c = g_cap;
        c.need = 2 * sigclass::NOMINAL_SNAPSHOT_SAMPLES;
        if (c.buf.size() < c.need) c.buf.resize(c.need);
        c.have = 0;
        c.ch = ch;
        c.first = true;
        c.channels = 0;
        c.freq = 0.0f;
        c.trail = -1;
        c.result = CAP_NONE;
        {
            std::lock_guard<std::mutex> lk2(g_mu);
            g_done = false;
        }
        g_armed.store(true, std::memory_order_release);
    }
    const int64_t end = now_ms() + CAPTURE_TIMEOUT_MS;
    const int mode0 = operating_mode_index();
    for (;;) {
        std::unique_lock<std::mutex> lk(g_mu);
        const bool got = g_cv.wait_for(lk, std::chrono::milliseconds(100), [] { return g_done || g_wake || g_stop.load(); });
        const bool done = g_done;
        if (!done && got) g_wake = false;   // (consumed: the caller sees g_enabled)
        lk.unlock();
        if (done) break;
        // woken (disable / stop), timed out, or the operating mode changed (the
        // worker clears the old mode's tracks at once)
        if (got || now_ms() > end || !g_enabled.load() || operating_mode_index() != mode0) {
            cancel_capture();
            std::lock_guard<std::mutex> lk2(g_cap_mu);
            return g_cap.result == CAP_CANCEL || g_cap.result == CAP_NONE ? CAP_CANCEL : g_cap.result;
        }
        flush_pending();
    }
    std::lock_guard<std::mutex> lk(g_cap_mu);
    return g_cap.result;
}

std::string model_path() {
    const char* e = std::getenv("KRAKEN_SIGCLASS_MODEL");
    return (e && *e) ? std::string(e) : std::string("sigclass.model");
}

// One tracker per band (coherent mode: [0] only; tuners in turn: one per
// tuner, the tuners on one band sharing their representative's). A track is
// counted against the looks at ITS band: each tracker runs on its own clock,
// which moves only with a snapshot of its band - by the wall time since the
// previous one, but at most TRACK_EXPIRE_S / MIN_MISSED_LOOKS (EXPIRE_STEP_S).
// So a track goes once it was missed for TRACK_EXPIRE_S AND in at least
// MIN_MISSED_LOOKS snapshots of its band. Coherent mode looks at its band
// every 1-10 s (17-45 looks in 45 s on a desktop); with tuners in turn a
// band gets a look every round (5 bands: 5-50 s), and a track that needed
// just 2 missed looks went before an intermittent signal (a P25 / DMR voice
// channel, a telemetry burst) came back - its new track started at one hit
// and stayed unpublished (measured: 11-59 % of the time labelled against
// 82-100 % in coherent mode on the same IQ). 6 looks = a coherent mode's
// at a 7.5 s period (a Raspberry Pi). In coherent mode (one tuner, every
// snapshot is a look) the clock also runs between snapshots (no data,
// retunes): there it is the wall clock.
constexpr int MIN_MISSED_LOOKS = 6;
constexpr double EXPIRE_STEP_S = TRACK_EXPIRE_S / MIN_MISSED_LOOKS + 0.01;
// a band's first look (after the switch-on, a mode change, a retune) may skip
// the budget's rest, so every band gets its first labels within a round of
// captures; the analysis not waited for is paid back by the next periods -
// at most this many looks' worth outstanding
constexpr double FIRST_LOOK_DEBT_LOOKS = 8.0;
struct TunerTracks {
    sigclass::Tracker trk;
    double clock = 0;            // the tracker's clock (s)
    double last_look = -1;       // now_s() of its latest snapshot; < 0 none yet
    std::vector<std::pair<int, double>> seen;   // track id -> now_s() last seen
    std::vector<rules::TrackPubId> book;        // track id -> the id the pages get (moves with the tracker)
    explicit TunerTracks(const sigclass::TrackerConfig& c) : trk(c) {}
    double seen_at(int id) const {
        for (const auto& p : seen)
            if (p.first == id) return p.second;
        return -1;
    }
};

void worker() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "sigclass");   // (top -H, the CPU figures)
#endif
    sigclass::Model model;
    std::shared_ptr<const Table> table;
    sigclass::Analyzer analyzer;
    sigclass::TrackerConfig tc;
    tc.expire_s = TRACK_EXPIRE_S;
    std::vector<TunerTracks> tt(MAX_CHANNELS, TunerTracks(tc));
    const auto clear_all = [&]() {
        for (TunerTracks& x : tt) {
            x.trk.clear();
            x.clock = 0;
            x.last_look = -1;
            x.seen.clear();
            x.book.clear();
        }
    };
    const auto any_tracks = [&]() {
        for (const TunerTracks& x : tt)
            if (!x.trk.tracks().empty()) return true;
        return false;
    };
    // a tracker's clock now (coherent: it also runs between snapshots)
    const auto clock_now = [&](const TunerTracks& x, bool coherent) {
        if (x.last_look < 0) return x.clock;
        return coherent ? x.clock + (now_s() - x.last_look) : x.clock;
    };
    int last_mode = -1, rr = 0, rr_next = 0;
    int retried = -1;                                // tuners in turn: the tuner whose aborted capture is tried again (L3)
    double ewma_ms = 0, period_now = MIN_PERIOD_S;   // (the cycle period, s)
    double sure_conf = NAN;                          // the model's min_confidence (the one-hit rule)
    int64_t settle_until = 0;                        // coherent mode: after a retune
    int64_t last_start_ms = -1;                      // the latest look's start (the budget's clock)
    rules::Budget budget;
    budget.share = CPU_SHARE;
    budget.min_s = MIN_PERIOD_S;
    budget.max_s = MAX_PERIOD_S;
    bool was_on = false;
    // tuners in turn: each tuner's frequency, settling and looks at its band;
    // reps: the tuner that stands for each one's band (rules::band_reps)
    std::vector<rules::TunerSlot> slots;
    std::vector<int> reps;
    std::vector<uint64_t> band_hz;      // per tracker: the band it serves (0: none) - rules::regroup
    int64_t look_seq = 0;               // the looks numbered (rules::note_look)
    int64_t next_pub = 0;               // the published track ids handed out (rules::follow_ids)
    const auto fresh_tracks = [&](TunerTracks& x) {
        x.trk.clear();
        x.clock = 0;
        x.last_look = -1;
        x.seen.clear();
        x.book.clear();
    };
    // (switch-on, mode change: every band's next look a first look again; the bands, their
    // turn, the tokens and the cursor go on - INV-H, rules::restart_bands)
    const auto restart_bands = [&]() { rules::restart_bands(slots, now_ms()); };
    const auto update_slots = [&](int M) {
        std::vector<uint64_t> hz(static_cast<size_t>(M));
        for (int i = 0; i < M; i++) hz[size_t(i)] = exact_tuner_hz(i);
        // the bands now, and which tracker / band state each one continues
        // (rules::update_bands / regroup: a band that stays occupied keeps
        // both, whichever tuner now stands for it - no gap in its labels; a
        // tuner on a band nobody was on keeps its own tracker, the old band's
        // tracks out of sight, and its band is new: a first look ahead of its
        // turn while it holds the token)
        bool moved = false;
        const rules::Regroup g = rules::update_bands(slots, band_hz, reps, hz, now_ms(), SETTLE_MS, look_seq, moved);
        if (!moved) return;
        std::vector<TunerTracks> old;
        old.reserve(size_t(M));
        for (int i = 0; i < M; i++) old.push_back(std::move(tt[size_t(i)]));
        for (int i = 0; i < M; i++) {
            const int src = g.src[size_t(i)];
            if (src >= 0) {
                tt[size_t(i)] = std::move(old[size_t(src)]);
            } else {
                tt[size_t(i)] = TunerTracks(tc);
                fresh_tracks(tt[size_t(i)]);
            }
        }
    };
    // a track nobody has seen for TRACK_CEILING_S of wall time goes, whether
    // or not its band was looked at (no looks: starved, no data, a tuner that
    // moved away) - the 45 s / 6 missed looks rule stays below it
    const auto ceiling = [&]() {
        const double t = now_s();
        for (TunerTracks& x : tt) {
            std::vector<int> gone;
            for (const sigclass::Track& k : x.trk.tracks()) {
                const double sa = x.seen_at(k.id);
                if (sa >= 0 && t - sa > rules::TRACK_CEILING_S) gone.push_back(k.id);
            }
            for (int id : gone) x.trk.erase(id);
        }
    };

    // what the pages get: per tracker the tracks inside its tuner's band now
    // (after a retune the old band's tracks stay in the tracker - back if the
    // receiver returns before they expire - but leave the list); classes
    // marked hidden (none: no signal) never; a signal two overlapping tuners
    // both see (wideband scan) once. live = the latest snapshots are current
    // (false while no data comes: ages then count)
    const auto make_pub = [&](int mode, double coh_centre, bool live) {
        const double t = now_s();
        const int M = mode == 0 ? 1 : std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
        // only what the pages show: a track whose centre is inside the
        // edge-clipped part of its tuner's band (EDGE_CLIP, 0.80 = +-0.96 MHz;
        // beyond it are filter skirts and artefacts the model labels wrongly);
        // one outside stays in the tracker (it may drift in), unsent
        const float clip = current_edge_clip.load(std::memory_order_relaxed);
        const double half = 0.5 * SAMPLE_RATE_HZ * (clip >= 0.1f && clip <= 1.0f ? static_cast<double>(clip) : 0.8);
        std::vector<PubTrack> pub;
        // tuners in turn: the bands now (tuners on one band: its representative's tracker)
        std::vector<rules::TunerSlot> now_slots(size_t(mode == 0 ? 0 : M));
        for (size_t i = 0; i < now_slots.size(); i++) now_slots[i].hz = exact_tuner_hz(int(i));
        const std::vector<int> now_reps = rules::band_reps(now_slots);
        int bands = 0;
        for (size_t i = 0; i < now_reps.size(); i++) bands += now_reps[i] == int(i) ? 1 : 0;
        bands = std::max(bands, 1);
        for (int ti = 0; ti < M; ti++) {
            if (mode != 0 && now_reps[size_t(ti)] != ti) continue;
            const TunerTracks& x = tt[size_t(ti)];
            const double band_c = mode == 0 ? coh_centre : static_cast<double>(exact_tuner_hz(ti));
            if (!(band_c > 0)) continue;
            for (const sigclass::Track& k : x.trk.tracks()) {
                if (std::abs(k.centre_hz - band_c) > half) continue;
                const int64_t pid = rules::pub_id(x.book, k.id);
                if (pid < 0) continue;   // (not seen by an update yet - none such: the book follows every update)
                PubTrack q;
                q.id = pid;
                q.tuner = ti;
                q.hz = k.centre_hz;
                q.bw = k.bw_hz;
                // the band the bracket shows (centre_hz is the smoothed power
                // centre; a core whose Track has its band edges gives those)
                track_band(k, q.lo, q.hi);
                // (a wide one's bracket stays on the plot)
                q.lo = std::clamp(q.lo, band_c - half, band_c + half);
                q.hi = std::clamp(q.hi, band_c - half, band_c + half);
                const double sa = x.seen_at(k.id);
                q.seen_real = sa >= 0 ? sa : t;
                if (t - q.seen_real > rules::TRACK_CEILING_S) continue;   // (the wall-clock ceiling, between ceiling() runs)
                // fresh: seen in its band's latest snapshot, and that one is
                // not overdue (a round of the bands + 5 s; no snapshots while
                // retunes keep coming or no data: the ages count)
                q.fresh = live && k.updated && t - x.last_look <= bands * period_now + 5.0;
                q.snr = k.snr_db;
                q.duty = k.duty;
                q.burst = k.bursty;
                q.hits = k.hits;
                if (k.best >= 0 && k.best < static_cast<int>(k.probs.size())) {
                    q.p = k.best_p;
                    const sigclass::ClassInfo& ci = model.class_info(k.view, k.best);
                    q.cls = ci.id;
                    const ClassMeta* m = table ? table->find(ci.id) : nullptr;
                    if (m && m->hidden) continue;   // a class the pages never show (none = no signal)
                    q.name = m && !m->name.empty() ? m->name : ci.name;
                    if (m) {
                        q.family = m->family;
                        q.plugin = m->plugin;
                        q.demod = m->demod;
                        q.vbw = m->bw_hz;
                        q.min_p = m->min_p;
                        q.unknown = m->unknown;
                    }
                    // the next two candidates (not hidden ones)
                    std::vector<int> idx(k.probs.size());
                    for (size_t c = 0; c < idx.size(); c++) idx[c] = static_cast<int>(c);
                    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return k.probs[size_t(a)] > k.probs[size_t(b)]; });
                    for (size_t c = 0; c < idx.size() && q.n_alt < 2; c++) {
                        if (idx[c] == k.best) continue;
                        const sigclass::ClassInfo& ai = model.class_info(k.view, idx[c]);
                        const ClassMeta* am = table ? table->find(ai.id) : nullptr;
                        if (am && am->hidden) continue;
                        q.alt[q.n_alt++] = {ai.id, am && !am->name.empty() ? am->name : ai.name, k.probs[size_t(idx[c])]};
                    }
                }
                q.wide = k.fills_band || k.bw_hz >= 0.9 * SAMPLE_RATE_HZ;
                // seen once only: published only when its label is sure already (the
                // class's min_p, else min_confidence - the page's rule) - a one-hit stray
                // piece would flash a "?" label; a sure one-hit label shows at once
                if (k.hits < 2) {
                    const double sure = is_finite_value(q.min_p) ? q.min_p : (is_finite_value(sure_conf) ? sure_conf : 0.7);
                    if (q.cls.empty() || !(q.p >= sure)) continue;
                }
                pub.push_back(std::move(q));
            }
        }
        // overlapping tuners: one signal seen by two of them is sent once - the
        // fresher / more recently / more often seen (sigclass_rules.hpp)
        rules::drop_overlap_duplicates(pub);
        if (pub.size() > MAX_TRACKS_SENT) {
            std::partial_sort(pub.begin(), pub.begin() + MAX_TRACKS_SENT, pub.end(),
                              [](const PubTrack& a, const PubTrack& b) { return a.seen_real > b.seen_real; });
            pub.resize(MAX_TRACKS_SENT);
        }
        std::sort(pub.begin(), pub.end(), [](const PubTrack& a, const PubTrack& b) { return a.hz < b.hz; });
        return pub;
    };
    // no snapshot now (no data, a retune's settling, a capture that ended
    // early): the trackers' expiry still runs (coherent: by the wall clock;
    // tuners in turn: only their own snapshots age them) and the list follows
    // the bands received now
    const auto age_out = [&](bool live) {
        const int mode = operating_mode_index();
        for (TunerTracks& x : tt) x.trk.expire(clock_now(x, mode == 0));
        ceiling();
        // the bands follow the tuners before the list is made: a band whose lead
        // changed keeps its tracks in the list (INV-E: a retune seen by an aborted
        // capture or a settle used to publish the band from the old slot - empty)
        if (mode != 0 && mode == last_mode) update_slots(std::clamp(active_num_elements.load(), 1, MAX_CHANNELS));
        std::vector<PubTrack> pub = make_pub(mode, static_cast<double>(coherent_tuner_hz()), live);
        {
            std::lock_guard<std::mutex> lk(g_st_mu);
            g_tracks = std::move(pub);
        }
        publish(false);
    };

    while (!g_stop.load()) {
        const bool on = g_enabled.load();
        if (!on) {
            if (was_on) {
                clear_all();
                clear_tracks_pub();
                set_parked("");
                publish(true);
                was_on = false;
            }
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_wake || g_stop.load(); });
            g_wake = false;
            continue;
        }
        if (!was_on) {
            was_on = true;
            // (the budget's debt and its clock stay over a disable: toggling
            // Signal ID buys no analysis beyond the share - every band's
            // first look is ahead of its turn again, within the same debt)
            restart_bands();
            // the model: tried on the first enable, and again on an enable
            // after it was refused (the user may have put the file there)
            int st;
            {
                std::lock_guard<std::mutex> lk(g_st_mu);
                st = g_model_state;
            }
            if (st != 1) {
                const std::string path = model_path();
                std::string err;
                const bool ok = model.load(path, &err);
                double mc = NAN;
                if (ok) {
                    table = parse_table(model.meta_json());
                    std::string v;
                    if (json_find(model.meta_json(), "min_confidence", v)) {
                        char* end = nullptr;
                        const double d = std::strtod(v.c_str(), &end);
                        if (end != v.c_str() && is_finite_value(d) && d > 0.0 && d <= 1.0) mc = d;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(g_st_mu);
                    g_model_state = ok ? 1 : 2;
                    g_reason = ok ? "" : (err.empty() ? "cannot load " + path : err);
                    g_min_conf = mc;
                }
                sure_conf = mc;
                if (ok) {
                    int views = 0;
                    for (int v = 0; v < sigclass::NUM_VIEWS; v++) views += model.has_view(v) ? 1 : 0;
                    std::cout << "Signal ID: model " << path << " loaded (" << table->classes.size() << " classes, "
                              << views << " views)" << std::endl;
                } else {
                    std::cerr << "Signal ID unavailable: " << (err.empty() ? "cannot load " + path : err) << std::endl;
                }
            }
            publish(true);
        }
        if (!model.loaded()) {
            // unavailable: nothing until the next disable (then an enable
            // tries the file again) - no retries in a loop
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_wake || g_stop.load(); });
            g_wake = false;
            continue;
        }

        const int mode = operating_mode_index();
        if (mode != last_mode) {
            if (last_mode >= 0) {
                clear_all();
                clear_tracks_pub();
                publish(true);   // (at once: the pages show the new mode already)
            }
            last_mode = mode;
            restart_bands();
        }
        // parked: the noise source (calibration) - the tracks go, a burst of
        // the noise source is no signal; a retune's settling packets
        if (doa_is_calibrating() || now_ms() - g_noise_ms.load() < NOISE_HOLD_MS) {
            if (any_tracks()) {
                clear_all();
                clear_tracks_pub();
            }
            set_parked("calibrating (noise source on)");
            rest(250);
            continue;
        }
        if (now_ms() < settle_until) {
            age_out(true);
            set_parked("settling after a retune");
            rest(settle_until - now_ms());
            continue;
        }

        // the tuner and its exact centre: coherent mode the selected antenna;
        // else the bands in turn (rules::pick_band - tuners on one band are
        // looked at once; a band nobody has looked at yet first; a retuned
        // tuner settles SETTLE_MS first, the others go on meanwhile)
        const int M = std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
        // heimdall lowered the channel count: the trackers of the tuners gone
        // go too (they kept their tracks on a frozen clock and came back aged)
        for (int i = M; i < MAX_CHANNELS; i++)
            if (!tt[size_t(i)].trk.tracks().empty()) fresh_tracks(tt[size_t(i)]);
        int ch = 0;
        bool first_look = false;
        if (mode == 0) {
            ch = std::clamp(active_channel.load(), 0, M - 1);
        } else {
            update_slots(M);
            rr_next = rr;   // (taken only when the look starts)
            ch = rules::pick_band(slots, reps, rr_next, now_ms(), first_look);
            if (ch < 0) {
                int64_t wait = -1;
                for (int i = 0; i < M; i++)
                    if (reps[size_t(i)] == i && slots[size_t(i)].settle_until_ms > now_ms())
                        wait = wait < 0 ? slots[size_t(i)].settle_until_ms - now_ms() : std::min(wait, slots[size_t(i)].settle_until_ms - now_ms());
                if (wait >= 0) {   // every band settling after a retune
                    age_out(true);
                    set_parked("settling after a retune");
                    rest(std::max<int64_t>(wait, 20));
                    continue;
                }
                ch = 0;   // (no tuner has a frequency: "no data" below)
            }
        }
        const uint64_t want_hz = mode == 0 ? coherent_tuner_hz() : exact_tuner_hz(ch);
        if (want_hz == 0) {
            age_out(false);
            set_parked("no data from the receiver");
            rest(1000);
            continue;
        }
        // the CPU budget: wait the period planned after the latest look -
        // unless this is a band's first look and the debt allows it
        if (last_start_ms >= 0) {
            const double elapsed = static_cast<double>(now_ms() - last_start_ms) / 1000.0;
            const double cap = FIRST_LOOK_DEBT_LOOKS * ewma_ms / 1000.0;
            const bool skip = first_look && budget.may_skip(elapsed, ewma_ms / 1000.0, cap);
            if (!skip && elapsed < budget.planned_s) {
                rest(static_cast<int64_t>(std::ceil((budget.planned_s - elapsed) * 1000.0)));
                continue;   // (everything again: parked, retunes, the mode, the band)
            }
        }

        const int64_t cycle0 = now_ms();
        if (mode != 0) rr = rr_next;
        const Result r = capture(ch);
        if (g_stop.load()) break;
        if (r == CAP_CANCEL) {
            if (g_enabled.load() && now_ms() - cycle0 >= CAPTURE_TIMEOUT_MS) {
                age_out(false);
                set_parked("no data from the receiver");
                rest(500);
            }
            continue;
        }
        if (r == CAP_NOISE) continue;   // parked on the next round
        if (r == CAP_RETUNE) {
            if (mode == 0) settle_until = now_ms() + SETTLE_MS;
            else slots[size_t(ch)].settle_until_ms = now_ms() + SETTLE_MS;   // (its own; the others go on)
            age_out(true);
            // tuners in turn: wait for it to settle and choose again - its band, still the oldest, is tried
            // at once (once: a second abort in a row goes the usual way). Before, the next band was taken
            // right away and the aborted one came a whole period later - with a tuner retuned every period
            // + capture, again just before the next retune: a swap every 2.8 s gave the swapped bands 2
            // looks in 1200 s against 142 (the rules test's model; docs/dev/signal-classifier.md, L3)
            if (mode != 0 && retried != ch) {
                retried = ch;
                rest(SETTLE_MS);
            } else {
                retried = -1;
            }
            continue;
        }
        if (r != CAP_OK) {              // the channel count changed: choose again
            age_out(true);
            continue;
        }
        if (operating_mode_index() != mode) continue;

        // the capture's centre: the exact request when the header agrees
        // (float32 is ~64 Hz exact near 1 GHz), else the header's
        double centre;
        {
            std::lock_guard<std::mutex> lk(g_cap_mu);
            centre = static_cast<double>(g_cap.freq);
        }
        const double exact = static_cast<double>(mode == 0 ? coherent_tuner_hz() : exact_tuner_hz(ch));
        if (exact > 0 && std::abs(exact - centre) < 2000.0) centre = exact;
        if (!(centre > 0)) continue;

        set_parked("");
        retried = -1;
        // the budget's clock: looks that were analysed (a capture that ended
        // early cost nothing - the next one may follow at once, as before)
        if (last_start_ms >= 0) budget.started(static_cast<double>(cycle0 - last_start_ms) / 1000.0, ewma_ms / 1000.0);
        last_start_ms = cycle0;
        const auto t0 = std::chrono::steady_clock::now();
        // (the buffer is the worker's while not armed)
        const std::vector<sigclass::Scored> res =
            analyzer.analyze(g_cap.buf.data(), sigclass::NOMINAL_SNAPSHOT_SAMPLES, static_cast<double>(SAMPLE_RATE_HZ), &model,
                             nullptr, centre);   // (centre: the RF mask of the model)
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ewma_ms = ewma_ms > 0 ? ewma_ms + 0.3 * (ms - ewma_ms) : ms;
        // the next look's period (the budget; a first-look debt lengthens it)
        const double period_s = budget.plan(ewma_ms / 1000.0);
        period_now = period_s;
        const double t = now_s();
        // this band's look: its tracker's clock moves on (see TunerTracks)
        TunerTracks& x = tt[size_t(mode == 0 ? 0 : ch)];
        if (x.last_look >= 0) x.clock += mode == 0 ? t - x.last_look : std::min(t - x.last_look, EXPIRE_STEP_S);
        x.last_look = t;
        if (mode != 0) rules::note_look(slots, reps, ch, first_look, look_seq++);
        ceiling();
        const std::vector<sigclass::Track>& tracks = x.trk.update(res, centre, x.clock);
        std::vector<std::pair<int, double>> seen;
        std::vector<int> ids;
        seen.reserve(tracks.size());
        ids.reserve(tracks.size());
        for (const sigclass::Track& k : tracks) {
            const double sa = x.seen_at(k.id);
            seen.push_back({k.id, k.updated || sa < 0 ? t : sa});
            ids.push_back(k.id);
        }
        x.seen = std::move(seen);
        rules::follow_ids(x.book, ids, next_pub);   // (the ids the pages get: INV-I)
        // (a retune during the capture: the band trackers follow the tuners first - INV-E)
        if (mode != 0) update_slots(M);
        std::vector<PubTrack> pub = make_pub(mode, centre, true);
        {
            std::lock_guard<std::mutex> lk(g_st_mu);
            g_tracks = std::move(pub);
            g_analyze_ms = ewma_ms;
            g_period_ms = period_s * 1000.0;
            g_snapshots++;
            const size_t li = size_t(mode == 0 ? 0 : ch);
            if (g_looks.size() <= li) g_looks.resize(li + 1, 0);
            g_looks[li]++;
        }
        publish(false);
        // (the rest before the next look: at the top, once its band is known)
    }
}

}  // namespace

// --- the tap (data receiver thread) ------------------------------------------
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const float* freq_hz, bool noise, bool retuning,
               bool gap) {
    std::lock_guard<std::mutex> lk(g_cap_mu);
    if (!g_armed.load(std::memory_order_relaxed)) return;
    Cap& c = g_cap;
    if (gap) {   // bytes lost: start over (the snapshot must be contiguous)
        c.gaps++;
        c.have = 0;
        c.trail = -1;
    }
    if (c.first) {
        c.first = false;
        c.channels = channels;
        c.freq = (c.ch >= 0 && static_cast<uint32_t>(c.ch) < channels && c.ch < MAX_CHANNELS) ? freq_hz[c.ch] : 0.0f;
    }
    if (channels != c.channels || c.ch < 0 || static_cast<uint32_t>(c.ch) >= channels || c.ch >= MAX_CHANNELS) {
        finish_locked(CAP_LAYOUT);
        return;
    }
    if (noise) {
        g_noise_ms.store(now_ms(), std::memory_order_relaxed);
        finish_locked(CAP_NOISE);
        return;
    }
    if (retuning || std::abs(freq_hz[c.ch] - c.freq) > 1.0f) {
        finish_locked(CAP_RETUNE);
        return;
    }
    if (c.trail < 0) {
        const size_t per = static_cast<size_t>(samples) * 2;
        const size_t n = std::min(per, c.need - c.have);
        std::memcpy(c.buf.data() + c.have, iq + static_cast<size_t>(c.ch) * per, n);
        c.have += n;
        if (c.have >= c.need) c.trail = 0;
    } else if (++c.trail >= TRAIL_PACKETS) {
        finish_locked(CAP_OK);
    }
}

// --- control (uWS loop thread) / main --------------------------------------------
void set_enabled(bool on) {
    const bool was = g_enabled.exchange(on);
    if (was == on) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_wake = true;
    }
    g_cv.notify_all();
    if (!on) cancel_capture();
    std::cout << "Signal ID " << (on ? "on" : "off") << std::endl;
    publish(true);
}

bool enabled() { return g_enabled.load(); }

std::string state_json() {
    std::lock_guard<std::mutex> lk(g_st_mu);
    return build_json_locked(nullptr);
}

void start() {
    if (g_thread) return;
    g_thread = new std::thread(worker);
}

void stop() {
    g_stop.store(true);
    cancel_capture();
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_wake = true;
    }
    g_cv.notify_all();
    if (g_thread && g_thread->joinable()) g_thread->join();
}

}  // namespace sigsvc
