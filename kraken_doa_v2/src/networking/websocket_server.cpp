#include "sweep.hpp"
#include "networking/websocket_server.hpp"
#include "networking/net_stats.hpp"
#include "ai_mcp.hpp"
#include "playback.hpp"
#include "recorder.hpp"
#include "utils/json_escape.hpp"
#include "utils/host_check.hpp"
#include "utils/parse_num.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "control_handler.hpp"
#include "message_builders.hpp"
#include "decoder_log.hpp"
#include "wb_board.hpp"
#include "rdf_mapper.hpp"
#include "array_cal.hpp"
#include "voice2text.hpp"
#include "scanner_manager.hpp"
#include "decimator_manager.hpp"
#include "digital/digital_decoder.hpp"
#include "channel_manager.hpp"
#include "networking/gpsd_client.hpp"
#include "station_info.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/bin_doa.hpp"
#include "doa_logger.hpp"
#include "utils/status_dashboard.hpp"
#include "utils/platform.hpp"
#include "utils/fs_compat.hpp"
#include "utils/tls_compat.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cctype>
#include <charconv>
#include <type_traits>
#include <iomanip>
#include <cstdlib>
#include <iterator>
#include <algorithm>
#include <thread>
#include <functional>
#include <memory>
#include <vector>

using namespace std;

namespace {

// Per-connection state stored by uWS for each WebSocket.
struct PerSocketData {
    bool authed = false;     // has this connection passed the AUTH gate?
    uint32_t sub_mask = 0;   // subscribed data streams (0 = unset; see SUB_*)
    // Spectrum views (FFT_VIEW:, FFT message type 7): once a page sends one,
    // it gets its own spectra - the window each display shows (its zoom) at
    // its own width - instead of the shared type 0 / 6 broadcast
    bool spec_views = false;
    struct SpecView {
        double lo_hz = 0, hi_hz = 0;   // from the display's centre; lo >= hi = whole band
        uint32_t bins = 0;             // points wanted (0 = the user's points per frame)
        uint32_t seq = 0;              // echoed in the frames
    };
    std::array<SpecView, 1 + MAX_CHANNELS> views{};  // 0 = main, 1 + ch = tuner ch
    double main_step_hz = 0;   // point spacing of its last main frame (beamformed overlays follow it)
    bool bin_doa = false;      // 🎨 BIN_DOA_VIEW:1 - it takes type 9 frames (bin_doa.hpp)
    // Sweep mode (SWEEP_VIEW:, type 10 - sweep.hpp): the window this page
    // shows; a frame goes when the sweep's data, its pause or the view changed
    bool sweep_view = false;
    sweep::View sweep{};
    bool sweep_dirty = false;
    uint64_t sweep_version = 0;
    uint32_t sweep_completions = 0;
    uint32_t sweep_overview = 0;   // SWEEP_OVERVIEW:<points> (0 = off): the whole range with each completed sweep
    // SWEEP_FULL:1 - the whole range at the sweep's own bins (type 11) with each
    // completed sweep, in parts paced by this socket's buffer, one set at a time
    bool net_stats = false;       // NET_STATS:1 - this connection's byte counters about once a second
    int64_t net_ms = 0;           // when they last went
    uint32_t sweep_full = 0;      // SWEEP_FULL:<n>: 0 off, 1 every bin, else at most n values (sweep::full_pool)
    bool full_snapshot = false;   // the store as it is, once (on SWEEP_FULL:1)
    bool full_due = false;        // its flag-2 frame went this tick: a completed sweep's set
    std::shared_ptr<const sweep::FullSet> full;   // the set being sent
    size_t full_off = 0;
    uint32_t full_id = 0;
    uint8_t full_flags = 0;
};

// Data-stream subscription bits (the heavy/optional streams).
constexpr uint32_t SUB_FFT   = 1u << 0;
constexpr uint32_t SUB_AUDIO = 1u << 1;
constexpr uint32_t SUB_DOA   = 1u << 2;
// Reconstructed decoder pictures (binary type 8). NOT part of SUB_ALL: a page
// opts in only while its decoder panel shows a video-capable plugin, so idle
// browsers never carry the (up to ~1 MB) frames.
constexpr uint32_t SUB_VIDEO = 1u << 3;
constexpr uint32_t SUB_ALL   = SUB_FFT | SUB_AUDIO | SUB_DOA;

// pub/sub topics. "ctl" carries control-plane traffic (sync_cmd echoes,
// scanner/doa state, system_status) and is ALWAYS subscribed so multi-client
// settings-sync stays correct. fft/audio/doa are selectable via SUBSCRIBE.
constexpr string_view TOPIC_CTL   = "ctl";
constexpr string_view TOPIC_FFT   = "fft";     // shared spectrum (types 0 / 6 / 4) - pages without views
constexpr string_view TOPIC_AUDIO = "audio";
constexpr string_view TOPIC_DOA   = "doa";
constexpr string_view TOPIC_VIDEO = "video";   // reconstructed decoder pictures (type 8), opt-in

// Shared API token, loaded once. Empty => auth disabled (fail-open).
const string& auth_token() {
    static const string token = [] {
        if (const char* env = std::getenv("KRAKEN_API_TOKEN"); env && *env)
            return string(env);
        ifstream f(AUTH_TOKEN_FILE);
        if (f.good()) {
            string t((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
#ifdef _WIN32
            // Notepad's "UTF-8 with BOM" (the default of older builds): the
            // 3 bytes became part of the token, which then never matched
            if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
#endif
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' ||
                                  t.back() == ' '  || t.back() == '\t'))
                t.pop_back();
            return t;
        }
        return string();
    }();
    return token;
}

inline bool auth_required() { return !auth_token().empty(); }

// Same-origin + DNS-rebinding guard for the WebSocket upgrade, /recordings and
// the 8081 page. Browsers send Origin on every cross-site WebSocket handshake:
// without this, any web page the operator opened could drive the client when
// no API token is configured (the default). The Host must be a name this
// device is reached by (utils/host_check.hpp; KRAKEN_ALLOWED_HOSTS adds names),
// which also defeats DNS rebinding. A request with no Origin is not a browser
// cross-site request (native app, curl) and only needs the Host check.
bool request_allowed(uWS::HttpRequest* req) {
    const string_view host = req->getHeader("host");
    if (!host_allowed(host)) return false;
    const string_view origin = req->getHeader("origin");
    if (origin.empty()) return true;
    const size_t scheme_end = origin.find("://");
    if (scheme_end == string_view::npos) return false;
    const string_view origin_host = origin.substr(scheme_end + 3);
    return !host.empty() && origin_host.size() == host.size() &&
           std::equal(origin_host.begin(), origin_host.end(), host.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

template <typename Res>
void refuse_request(Res* res, uWS::HttpRequest* req) {
    std::cerr << "[WS] Refused cross-origin / unknown-host request (Host: "
              << req->getHeader("host") << ", Origin: " << req->getHeader("origin")
              << ") - set KRAKEN_ALLOWED_HOSTS to allow a name" << std::endl;
    res->writeStatus("403 Forbidden")->end("request refused");
}

constexpr int MAX_WS_CLIENTS = 32;

// --- Unauthenticated-connection limits (only with an API token configured) ---
// A socket that never sends AUTH: used to hold one of the 32 slots for good
// (browsers' automatic pongs satisfy idleTimeout), so 32 of them locked every
// browser out, and token guessing was unthrottled (one guess per connection,
// unlimited reconnects). All of this runs on the uWS loop thread only.
using ClientWS = uWS::WebSocket<true, true, PerSocketData>;
constexpr int MAX_PENDING_AUTH = 8;              // unauthenticated sockets at once
constexpr int64_t AUTH_DEADLINE_MS = 10000;      // ...each closed after this
constexpr int AUTH_MAX_FAILURES = 5;             // wrong tokens per IP...
constexpr int64_t AUTH_FAILURE_WINDOW_MS = 60000;  // ...per window, then refused
std::unordered_map<ClientWS*, int64_t> g_pending_auth;           // socket -> opened (steady ms)
std::unordered_set<ClientWS*> g_spec_sockets;  // sockets with spectrum views (uWS loop only)
std::unordered_set<ClientWS*> g_video_sockets;  // sockets subscribed to decoder video (uWS loop only)
std::unordered_set<ClientWS*> g_bin_doa_sockets;  // 🎨 sockets taking type 9 (uWS loop only)
std::unordered_set<ClientWS*> g_sweep_sockets;    // sockets with a sweep view, type 10 (uWS loop only)
std::unordered_map<std::string, std::pair<int, int64_t>> g_auth_failures;  // ip -> (count, window start)

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool auth_ip_blocked(const std::string& ip) {
    auto it = g_auth_failures.find(ip);
    if (it == g_auth_failures.end()) return false;
    if (steady_ms() - it->second.second > AUTH_FAILURE_WINDOW_MS) { g_auth_failures.erase(it); return false; }
    return it->second.first >= AUTH_MAX_FAILURES;
}
void note_auth_failure(const std::string& ip) {
    const int64_t now = steady_ms();
    if (g_auth_failures.size() > 256) {                      // bound the table
        for (auto it = g_auth_failures.begin(); it != g_auth_failures.end();)
            it = (now - it->second.second > AUTH_FAILURE_WINDOW_MS) ? g_auth_failures.erase(it) : std::next(it);
    }
    auto& e = g_auth_failures[ip];
    if (e.first == 0 || now - e.second > AUTH_FAILURE_WINDOW_MS) e = {0, now};
    e.first++;
    if (e.first == AUTH_MAX_FAILURES)
        std::cerr << "[WS] " << ip << ": " << AUTH_MAX_FAILURES << " wrong API tokens - refusing it for up to "
                  << AUTH_FAILURE_WINDOW_MS / 1000 << " s" << std::endl;
}
// Called from the 500 ms status timer: close sockets past the auth deadline.
void sweep_pending_auth() {
    if (g_pending_auth.empty()) return;
    const int64_t now = steady_ms();
    std::vector<ClientWS*> late;
    for (const auto& [ws, opened] : g_pending_auth)
        if (now - opened > AUTH_DEADLINE_MS) late.push_back(ws);
    for (ClientWS* ws : late) {
        g_pending_auth.erase(ws);       // before end(): .close erases too
        ws->end(1008, "authentication timeout");
    }
}

// Length-aware constant-time compare (avoid leaking the token via timing).
bool token_matches(string_view provided) {
    const string& expected = auth_token();
    if (provided.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        diff |= (unsigned char)provided[i] ^ (unsigned char)expected[i];
    return diff == 0;
}

// Parse a comma list like "FFT,AUDIO,DOA" (or "ALL") into a stream bitmask.
uint32_t parse_sub_mask(string_view list) {
    uint32_t mask = 0;
    size_t start = 0;
    while (true) {
        size_t comma = list.find(',', start);
        string_view tok = list.substr(start, comma == string_view::npos
                                                 ? string_view::npos : comma - start);
        while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
        while (!tok.empty() && tok.back() == ' ')  tok.remove_suffix(1);
        if      (tok == "FFT")   mask |= SUB_FFT;
        else if (tok == "AUDIO") mask |= SUB_AUDIO;
        else if (tok == "DOA")   mask |= SUB_DOA;
        else if (tok == "VIDEO") mask |= SUB_VIDEO;
        else if (tok == "ALL")   mask |= SUB_ALL;
        if (comma == string_view::npos) break;
        start = comma + 1;
    }
    return mask;
}

template<typename WS>
void apply_subscriptions(WS* ws, uint32_t mask) {
    ws->getUserData()->sub_mask = mask;
    ws->subscribe(TOPIC_CTL);  // always on; idempotent
    // a page with spectrum views is sent its own spectra (send_spectrum_views)
    if ((mask & SUB_FFT) && !ws->getUserData()->spec_views) ws->subscribe(TOPIC_FFT);
    else ws->unsubscribe(TOPIC_FFT);
    if (mask & SUB_AUDIO) ws->subscribe(TOPIC_AUDIO); else ws->unsubscribe(TOPIC_AUDIO);
    if (mask & SUB_DOA)   ws->subscribe(TOPIC_DOA);   else ws->unsubscribe(TOPIC_DOA);
    // publish_video sends per socket (its own backpressure drop), so track the
    // subscribers in a set rather than relying on the topic alone
    if (mask & SUB_VIDEO) { ws->subscribe(TOPIC_VIDEO); g_video_sockets.insert(ws); }
    else { ws->unsubscribe(TOPIC_VIDEO); g_video_sockets.erase(ws); }
    // the video plugins render / encode / send only while someone watches
    dig::set_video_wanted(!g_video_sockets.empty());
}

// FFT_VIEW:<display>:<seq>:<lo Hz>:<hi Hz>:<points> (see .message). A bad one
// is ignored; the first good one moves the page from the shared spectrum
// broadcast (types 0 / 6) to its own frames (type 7).
template<typename WS>
void handle_fft_view(WS* ws, string_view args) {
    string_view f[5];
    for (int i = 0; i < 5; i++) {
        const size_t c = args.find(':');
        if (i < 4 && c == string_view::npos) return;
        f[i] = args.substr(0, c);
        args = c == string_view::npos ? string_view() : args.substr(c + 1);
    }
    int disp = -1;
    uint32_t seq = 0, bins = 0;
    double lo = 0, hi = 0;
    auto num = [](string_view t, auto& v) {
#if !defined(__cpp_lib_to_chars)
        // a libc++ without floating-point from_chars (Apple clang < 15):
        // strtod on a copy, the whole field, no leading space / sign games
        if constexpr (std::is_floating_point_v<std::remove_reference_t<decltype(v)>>) {
            if (t.empty() || t.size() > 64 || !(std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-' || t[0] == '.'))
                return false;
            const std::string s(t);
            char* end = nullptr;
            v = std::strtod(s.c_str(), &end);
            return end == s.c_str() + s.size();
        } else
#endif
        {
            auto r = std::from_chars(t.data(), t.data() + t.size(), v);
            return r.ec == std::errc() && r.ptr == t.data() + t.size();
        }
    };
    if (!num(f[0], disp) || !num(f[1], seq) || !num(f[2], lo) || !num(f[3], hi) || !num(f[4], bins)) return;
    // is_finite_value, not std::isfinite: -Ofast folds that to true
    if (disp < 0 || disp > MAX_CHANNELS || !is_finite_value(lo) || !is_finite_value(hi) ||
        std::abs(lo) > 1e10 || std::abs(hi) > 1e10 || bins > 65536)
        return;
    PerSocketData* ud = ws->getUserData();
    ud->views[disp] = {lo, hi, bins, seq};
    if (!ud->spec_views) {
        ud->spec_views = true;
        g_spec_sockets.insert(ws);
        apply_subscriptions(ws, ud->sub_mask ? ud->sub_mask : SUB_ALL);  // off the shared broadcast
    }
}

constexpr unsigned SPEC_MAX_BUFFERED = 256 * 1024;   // a frame skipped past this (types 7 / 9 / 10)

// SWEEP_VIEW:<seq>:<lo Hz>:<hi Hz>:<points> (sweep mode; transport level,
// this connection only, like FFT_VIEW): the window the page shows (absolute
// Hz, lo >= hi = the whole range) at its plot's width. Junk is ignored.
template<typename WS>
void handle_sweep_view(WS* ws, string_view args) {
    string_view f[4];
    for (int i = 0; i < 4; i++) {
        const size_t c = args.find(':');
        if (i < 3 && c == string_view::npos) return;
        f[i] = args.substr(0, c);
        args = c == string_view::npos ? string_view() : args.substr(c + 1);
    }
    uint32_t seq = 0, points = 0;
    double lo = 0, hi = 0;
    auto num = [](string_view t, auto& v) {
#if !defined(__cpp_lib_to_chars)
        if constexpr (std::is_floating_point_v<std::remove_reference_t<decltype(v)>>) {
            if (t.empty() || t.size() > 64 || !(std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '-' || t[0] == '.'))
                return false;
            const std::string s(t);
            char* end = nullptr;
            v = std::strtod(s.c_str(), &end);
            return end == s.c_str() + s.size();
        } else
#endif
        {
            auto r = std::from_chars(t.data(), t.data() + t.size(), v);
            return r.ec == std::errc() && r.ptr == t.data() + t.size();
        }
    };
    if (!num(f[0], seq) || !num(f[1], lo) || !num(f[2], hi) || !num(f[3], points)) return;
    if (!is_finite_value(lo) || !is_finite_value(hi) || std::abs(lo) > 1e10 || std::abs(hi) > 1e10 || points > 65536) return;
    PerSocketData* ud = ws->getUserData();
    ud->sweep = {seq, lo, hi, points};
    ud->sweep_dirty = true;   // answered on the next tick (also while paused)
    if (!ud->sweep_view) {
        ud->sweep_view = true;
        ud->sweep_completions = sweep::completions();   // its first frame is no waterfall line
        g_sweep_sockets.insert(ws);
    }
}

// SWEEP_OVERVIEW:<points> (sweep mode; transport level, this connection only):
// with every completed sweep a zoomed page also gets the whole range at about
// that many points (type 10 flag 16, before the flag-2 frame) - its waterfall
// rows then hold the whole range. 0 = off (every socket's default), else 16 ...
// SPECTRUM_VIEW_MAX_POINTS. Junk is ignored
template<typename WS>
void handle_sweep_overview(WS* ws, string_view arg) {
    uint32_t n = 0;
    auto r = std::from_chars(arg.data(), arg.data() + arg.size(), n);
    if (arg.empty() || r.ec != std::errc() || r.ptr != arg.data() + arg.size()) return;
    ws->getUserData()->sweep_overview = n == 0 ? 0 : std::clamp<uint32_t>(n, 16, SPECTRUM_VIEW_MAX_POINTS);
}

// SWEEP_FULL:<1|0> (sweep mode; transport level, this connection only): the
// full set (type 11) with each completed sweep, and the store as it is right
// away (a snapshot: flag 2). Junk is ignored
template<typename WS>
void handle_sweep_full(WS* ws, string_view arg) {
    PerSocketData* ud = ws->getUserData();
    uint32_t n = 0;
    if (arg == "true") {
        n = 1;
    } else if (arg == "false") {
        n = 0;
    } else {
        auto r = std::from_chars(arg.data(), arg.data() + arg.size(), n);
        if (arg.empty() || r.ec != std::errc() || r.ptr != arg.data() + arg.size()) return;   // junk: ignored
    }
    if (n == ud->sweep_full) return;   // the same again: nothing (no new snapshot)
    ud->full.reset();                  // a set in flight is abandoned (the page drops it as incomplete)
    ud->full_due = false;
    ud->full_snapshot = n != 0;        // a new n: the store as it is, at that n, on the next tick
    ud->sweep_full = n;
}

// NET_STATS:<1|0> (every mode; transport level, this connection only): the
// kernel's byte counters of this connection (net_stats.hpp: Linux TCP_INFO) as
// {"net":{"tx_bytes","tx_sent"?,"rx_bytes","t_ms"}} at once and then every
// second - for the page's network rate. No counter (other platforms): no
// message. Junk is ignored
std::unordered_set<ClientWS*> g_net_sockets;   // sockets with NET_STATS on (uWS loop only)
template<typename WS>
void send_net_stats(WS* ws, int64_t now) {
    PerSocketData* ud = ws->getUserData();
    ud->net_ms = now;
    net_stats::Counters c;
    // (a uWS WebSocket is its us_socket_t; an SSL socket's struct begins with it,
    // and that with the poll: the TCP descriptor under the TLS)
    if (!net_stats::read(static_cast<int>(us_poll_fd(reinterpret_cast<us_poll_t*>(ws))), c)) return;
    char b[160];
    if (c.has_sent)
        snprintf(b, sizeof b, "{\"net\":{\"tx_bytes\":%llu,\"tx_sent\":%llu,\"rx_bytes\":%llu,\"t_ms\":%lld}}",
                 static_cast<unsigned long long>(c.tx_acked), static_cast<unsigned long long>(c.tx_sent),
                 static_cast<unsigned long long>(c.rx), static_cast<long long>(now));
    else
        snprintf(b, sizeof b, "{\"net\":{\"tx_bytes\":%llu,\"rx_bytes\":%llu,\"t_ms\":%lld}}",
                 static_cast<unsigned long long>(c.tx_acked), static_cast<unsigned long long>(c.rx), static_cast<long long>(now));
    ws->send(b, uWS::TEXT);
}
template<typename WS>
void handle_net_stats(WS* ws, string_view arg) {
    PerSocketData* ud = ws->getUserData();
    if (arg == "1" || arg == "true") {
        if (ud->net_stats) return;
        ud->net_stats = true;
        g_net_sockets.insert(ws);
        send_net_stats(ws, steady_ms());   // a baseline at once
    } else if (arg == "0" || arg == "false") {
        ud->net_stats = false;
        g_net_sockets.erase(ws);
    }
}
// on the broadcast tick (every mode): each such socket's counters every 1000 ms
void net_stats_tick() {
    if (g_net_sockets.empty()) return;
    const int64_t now = steady_ms();
    for (ClientWS* ws : g_net_sockets)
        if (now - ws->getUserData()->net_ms >= 1000) send_net_stats(ws, now);
}

// The full set's parts go while this socket has less than FULL_QUEUE queued
// (uWS drops past maxBackpressure, 2 MB): every tick, and when it drains. A set
// starts only with a tick (the tick's sets: every pool in use from one state of
// the store, built once, shared by the sockets) and
// only when the socket has no set in flight and at most SPEC_MAX_BUFFERED
// queued - else that sweep's set is skipped whole. A grid change abandons a set
constexpr unsigned FULL_QUEUE = 1024 * 1024;
bool g_full_active = false;   // some socket has a set in flight (released out of the mode)
using FullCache = std::vector<std::shared_ptr<const sweep::FullSet>>;   // this tick's sets, one per pool
template<typename WS>
void pump_full(WS* ws, FullCache* lazy, uint32_t grid) {
    PerSocketData* ud = ws->getUserData();
    if (!ud->sweep_full) return;
    if (ud->full && ud->full->grid != grid) ud->full.reset();
    if (lazy && !ud->full && (ud->full_due || ud->full_snapshot) && (ud->sub_mask & SUB_FFT) &&
        ws->getBufferedAmount() <= SPEC_MAX_BUFFERED) {
        const uint32_t pool = sweep::full_pool(ud->sweep_full);
        std::shared_ptr<const sweep::FullSet> set;
        for (const auto& s : *lazy)
            if (s && s->pool == pool) set = s;
        if (set && set->grid == grid) {
            ud->full = set;
            ud->full_off = 0;
            ud->full_id++;
            ud->full_flags = static_cast<uint8_t>((ud->full_due ? 0 : 2) | (sweep::running() ? 0 : 4));
            g_full_active = true;
        }
    }
    if (lazy) ud->full_due = ud->full_snapshot = false;
    while (ud->full && ws->getBufferedAmount() < FULL_QUEUE) {
        const size_t total = ud->full->lv.size();
        const size_t n = std::min(sweep::FULL_PART_VALUES, total - ud->full_off);
        const bool last = ud->full_off + n >= total;
        ws->send(sweep::full_part(*ud->full, static_cast<uint8_t>(ud->full_flags | (last ? 1 : 0)), ud->full_id, ud->full_off, n),
                 uWS::BINARY, false);
        ud->full_off += n;
        if (last) ud->full.reset();
    }
}

// Sweep mode, every 50 ms tick: each page with a sweep view gets a frame when
// something changed (at most 20 a second while hops arrive); a completed sweep
// marks its frame (flag 2: the page's waterfall line). A slow link skips
void send_sweep_views() {
    const uint64_t ver = sweep::data_version();
    const uint32_t done = sweep::completions();
    for (ClientWS* ws : g_sweep_sockets) {
        PerSocketData* ud = ws->getUserData();
        if (!(ud->sub_mask & SUB_FFT) || ws->getBufferedAmount() > SPEC_MAX_BUFFERED) continue;
        const bool completes = ud->sweep_completions != done;
        if (!ud->sweep_dirty && !completes && ud->sweep_version == ver) continue;
        string ov;
        const string m = sweep::build_frame(ud->sweep, completes, ud->sweep_overview, &ov);
        if (m.empty()) continue;
        if (!ov.empty()) ws->send(ov, uWS::BINARY, true);   // (the overview first: the page pairs them by sweeps)
        ws->send(m, uWS::BINARY, true);
        if (completes && ud->sweep_full) ud->full_due = true;   // (its full set follows, paced)
        ud->sweep_dirty = false;
        ud->sweep_version = ver;
        ud->sweep_completions = done;
    }
    // the full sets (SWEEP_FULL): after the frames, so the live trace goes first
    // the sets that start this tick, every pool from one state of the store
    std::vector<uint32_t> pools;
    for (ClientWS* ws : g_sweep_sockets) {
        PerSocketData* ud = ws->getUserData();
        if (ud->sweep_full && !ud->full && (ud->full_due || ud->full_snapshot) && (ud->sub_mask & SUB_FFT) &&
            ws->getBufferedAmount() <= SPEC_MAX_BUFFERED)
            pools.push_back(sweep::full_pool(ud->sweep_full));
    }
    FullCache lazy;
    if (!pools.empty()) lazy = sweep::build_fulls(pools);
    uint32_t grid = 0;
    bool have_grid = false;
    for (ClientWS* ws : g_sweep_sockets) {
        if (!ws->getUserData()->sweep_full) continue;
        if (!have_grid) {
            grid = sweep::grid_id();
            have_grid = true;
        }
        pump_full(ws, &lazy, grid);
    }
}

// out of sweep mode: no set stays in flight (its memory goes)
void release_full_sets() {
    if (!g_full_active) return;
    for (ClientWS* ws : g_sweep_sockets) {
        PerSocketData* ud = ws->getUserData();
        ud->full.reset();
        ud->full_due = ud->full_snapshot = false;
    }
    g_full_active = false;
}

// Each page with views gets every display of the current mode (independent:
// one frame per tuner, else the main one), cut to its window and width, and
// the beamformed overlays (coherent mode) at its main frame's point spacing -
// zoomed in, they get finer with the spectrum. A slow link drops frames
// instead of queueing them: uWS closes a socket past maxBackpressure (2 MB)
void send_spectrum_views(const SpectrumSnapshot& snap, const vector<BeamformedSpectrum>& bfs) {
    const int first = snap.independent ? 1 : 0;
    const int last = snap.independent ? std::min<int>(snap.channels, MAX_CHANNELS) : 0;
    for (ClientWS* ws : g_spec_sockets) {
        PerSocketData* ud = ws->getUserData();
        if (!(ud->sub_mask & SUB_FFT) || ws->getBufferedAmount() > SPEC_MAX_BUFFERED) continue;
        for (int d = first; d <= last; d++) {
            const auto& v = ud->views[d];
            // 🎨 the bearing spectrum (coherent mode): type 9 for the same
            // points, sent first so the page colours the frame it draws
            const bool brg = d == 0 && ud->bin_doa && !snap.independent && snap.marker != 0xFE && bin_doa::enabled();
            SpectrumViewCut cut;
            const string m = MessageBuilders::build_spectrum_view(snap, d, v.lo_hz, v.hi_hz, v.bins, v.seq,
                                                                  d == 0 ? &ud->main_step_hz : nullptr,
                                                                  brg ? &cut : nullptr);
            if (brg && !m.empty()) {
                const string b = bin_doa::build_frame(snap.disp[0], cut, v.seq);
                if (!b.empty()) ws->send(b, uWS::BINARY, true);
            }
            if (!m.empty()) ws->send(m, uWS::BINARY, true);
        }
        for (const auto& bf : bfs) {
            const string m = MessageBuilders::build_beamformed_fft_message(bf, ud->main_step_hz);
            if (!m.empty()) ws->send(m, uWS::BINARY, true);
        }
    }
}

// Subscribe the connection and replay current state so its UI matches the
// shared backend state (multi-client settings sync). Factored out of .open so
// it can also run right after a successful AUTH.
template<typename WS>
void subscribe_and_sync(WS* ws) {
    uint32_t mask = ws->getUserData()->sub_mask;
    if (mask == 0) mask = SUB_ALL;  // default: all streams
    apply_subscriptions(ws, mask);

    // Send current scanner state to the newly subscribed client so the UI
    // stays in sync after a (re)connect.
    bool scanner_running = scanner_manager.isRunning();
    ScannerState state = scanner_manager.getState();
    string state_str;
    switch (state) {
        case ScannerState::IDLE: state_str = "IDLE"; break;
        case ScannerState::SCANNING: state_str = "SCANNING"; break;
        case ScannerState::LOCKED: state_str = "LOCKED"; break;
        case ScannerState::PAUSED: state_str = "PAUSED"; break;
        case ScannerState::SIGNAL_LOST_WAIT: state_str = "SIGNAL_LOST_WAIT"; break;
        default: state_str = "IDLE"; break;
    }

    stringstream json;
    json << "{\"scanner_sync\":{\"running\":" << (scanner_running ? "true" : "false")
         << ",\"state\":\"" << state_str << "\"";
    if (state == ScannerState::LOCKED) {
        size_t locked_idx = scanner_manager.getLockedFreqIndex();
        float signal_db = scanner_manager.getLockedSignalDb();
        auto config = scanner_manager.getConfig();
        if (locked_idx < config.frequencies.size()) {
            const auto& freq = config.frequencies[locked_idx];
            json << ",\"locked_freq_mhz\":" << freq.freq_mhz
                 << ",\"locked_signal_db\":" << signal_db
                 << ",\"locked_label\":\"" << json_escape(freq.label) << "\"";
        }
    }
    json << "}}";
    ws->send(json.str(), uWS::TEXT);

    // Replay current settings so the new client's UI matches shared state.
    for (const auto& sync_msg : ControlHandler::get_connect_sync_messages()) {
        ws->send(sync_msg, uWS::TEXT);
    }
}

// Percent-decode a URL path segment (e.g. %20 -> space).
std::string url_decode(std::string_view s) {
    auto hex = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += static_cast<char>((hi << 4) | lo); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

// Can a URL-decoded name from a request (a recording, "<name>/<file>") go to
// the file APIs? Always on Linux, where the routes' own checks decide.
// Windows: std::filesystem::path throws on bytes that aren't UTF-8 (Linux
// takes any bytes) - an exception out of a route handler ends the process -
// and each '/'-part must be a portable file name: no '\' or ':' (a drive, an
// NTFS stream), no device name (NUL, COM3 ... exist in every folder).
bool request_name_ok(const std::string& s) {
#ifdef _WIN32
    for (size_t i = 0; i < s.size();) {
        if (static_cast<unsigned char>(s[i]) < 0x80) { i++; continue; }
        const size_t n = json_utf8_len(s, i);
        if (n == 0) return false;
        i += n;
    }
    for (size_t b = 0; b <= s.size();) {
        const size_t e = std::min(s.find('/', b), s.size());
        if (e > b && !fs_portable_name(std::string_view(s).substr(b, e - b))) return false;
        b = e + 1;
    }
#else
    (void)s;
#endif
    return true;
}

// Stream a file to the client as an attachment (download), with backpressure
// handling so large recordings don't have to be buffered in memory. Re-supplies
// data from getWriteOffset() on each writable event, per the uWS tryEnd model.
template<bool SSL>
void stream_download(uWS::HttpResponse<SSL>* res, const std::string& fullpath,
                     const std::string& dlname) {
#ifdef _WIN32
    // Windows: only a regular file - a device name opens the device (a
    // serial GPS on COM3 would be read here, on the uWS loop thread, for
    // ever; tellg() = -1 made its size UINTMAX_MAX)
    struct stat st{};
    if (::stat(fullpath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        res->writeStatus("404 Not Found")->end("Recording not found");
        return;
    }
#endif
    auto file = std::make_shared<std::ifstream>(fullpath, std::ios::binary);
    if (!file->good()) {
        res->writeStatus("404 Not Found")->end("Recording not found");
        return;
    }
    file->seekg(0, std::ios::end);
    const std::streamoff end = file->tellg();
    if (end < 0) {
        // no size (not a seekable file): -1 cast to uintmax_t announced an
        // endless body and the pump never finished
        res->writeStatus("500 Internal Server Error")->end("Cannot read recording");
        return;
    }
    uintmax_t total = static_cast<uintmax_t>(end);
    file->seekg(0, std::ios::beg);

    res->writeHeader("Content-Type", "application/octet-stream");
    res->writeHeader("Content-Disposition", "attachment; filename=\"" + dlname + "\"");
    res->writeHeader("Cache-Control", "no-store");

    if (total == 0) { res->end(); return; }

    auto aborted = std::make_shared<bool>(false);
    res->onAborted([aborted]() { *aborted = true; });

    auto buf = std::make_shared<std::vector<char>>(256 * 1024);

    // Sends data starting at `offset`; returns true when finished/aborted,
    // false when it must wait for the next writable event.
    auto pump = std::make_shared<std::function<bool(uintmax_t)>>();
    *pump = [res, file, total, buf, aborted](uintmax_t offset) -> bool {
        if (*aborted) return true;
        file->clear();
        file->seekg(static_cast<std::streamoff>(offset));
        while (offset < total) {
            if (*aborted) return true;
            size_t want = static_cast<size_t>(std::min<uintmax_t>(buf->size(), total - offset));
            file->read(buf->data(), static_cast<std::streamsize>(want));
            std::streamsize n = file->gcount();
            if (n <= 0) return true;  // read error: stop
            auto [ok, done] = res->tryEnd(std::string_view(buf->data(), static_cast<size_t>(n)), total);
            if (done) return true;
            if (!ok) return false;    // backpressure: resume on onWritable
            offset += static_cast<uintmax_t>(n);
        }
        return true;
    };

    res->onWritable([pump](uintmax_t offset) -> bool { return (*pump)(offset); });
    (*pump)(0);
}

// 📼 a recording (recorder.hpp): the parts one after the other - an IQ
// recording's folder goes out as one SigMF archive (tar headers made here,
// the files read in place), so a multi-GB download needs no copy on the disk
template<bool SSL>
void stream_parts(uWS::HttpResponse<SSL>* res, std::shared_ptr<rec::Download> d) {
    res->writeHeader("Content-Type", d->content_type);
    res->writeHeader("Content-Disposition", "attachment; filename=\"" + d->filename + "\"");
    res->writeHeader("Cache-Control", "no-store");
    if (d->total == 0) { res->end(); return; }
    auto aborted = std::make_shared<bool>(false);
    res->onAborted([aborted]() { *aborted = true; });
    auto buf = std::make_shared<std::vector<char>>(256 * 1024);
    auto pump = std::make_shared<std::function<bool(uintmax_t)>>();
    *pump = [res, d, buf, aborted](uintmax_t offset) -> bool {
        if (*aborted) return true;
        while (offset < d->total) {
            if (*aborted) return true;
            // the part holding offset
            uint64_t base = 0;
            size_t i = 0;
            while (i < d->parts.size() && base + d->parts[i].len <= offset) base += d->parts[i++].len;
            if (i == d->parts.size()) return true;
            const rec::Part& p = d->parts[i];
            const uint64_t in = offset - base;
            const size_t want = static_cast<size_t>(std::min<uint64_t>(buf->size(), p.len - in));
            size_t n = 0;
            if (p.path.empty()) {
                memcpy(buf->data(), p.mem.data() + in, want);
                n = want;
            } else {
                std::ifstream f(p.path, std::ios::binary);
                f.seekg(static_cast<std::streamoff>(p.off + in));
                f.read(buf->data(), static_cast<std::streamsize>(want));
                if (f.gcount() <= 0) { res->close(); return true; }   // the file shrank: abort rather than send garbage
                n = static_cast<size_t>(f.gcount());
            }
            auto [ok, done] = res->tryEnd(std::string_view(buf->data(), n), d->total);
            if (done) return true;
            if (!ok) return false;    // backpressure: resume on onWritable
            offset += n;
        }
        return true;
    };
    res->onWritable([pump](uintmax_t offset) -> bool { return (*pump)(offset); });
    (*pump)(0);
}

} // namespace

string WebSocketServer::load_html_content() {
    ifstream file(HTML_FILE);
    if (!file.is_open()) {
        return R"HTML(<!DOCTYPE html><html><head><title>KrakenSDR DoA - File Not Found</title></head>
<body><h1>KrakenSDR DoA with FM Audio and MUSIC DoA</h1><p>Error: Could not load kraken_doa.html</p></body></html>)HTML";
    }
    
    stringstream buffer;
    buffer << file.rdbuf();
    // The API token is deliberately NOT injected: the page is served to
    // anyone who can reach the port, so embedding it handed the token to any
    // LAN client. The user enters it in the browser once (see askForToken in
    // kraken_doa.html).
    return buffer.str();
}

void WebSocketServer::verify_ssl_certificates() {
#if defined(_WIN32) || defined(__APPLE__)
    // Windows / macOS have no install step (install.sh makes the pair with
    // the openssl CLI on Linux; Windows has no openssl.exe): a missing pair
    // is made here, in-process - the same kind, self-signed for 10 years
    {
        bool created = false;
        string err;
        if (!tls_ensure_self_signed_cert(SSL_CERT_FILE, SSL_KEY_FILE, &created, &err))
            cerr << "ERROR: cannot create the SSL certificate: " << err << endl;
        else if (created)
            cout << "Created a self-signed SSL certificate for the web UI (" << SSL_CERT_FILE << " + "
                 << SSL_KEY_FILE << ")" << endl;
    }
#endif
    ifstream cert_file(SSL_CERT_FILE);
    ifstream key_file(SSL_KEY_FILE);

    if (!cert_file.good() || !key_file.good()) {
        // Runs on the web-server thread: no exit() here either (see the
        // listen-failure path below for why).
        StatusDashboard::end();
        cerr << "ERROR: Cannot read SSL certificate files" << endl;
#if defined(_WIN32) || defined(__APPLE__)
        cerr << "kraken_doa makes " << SSL_CERT_FILE << " + " << SSL_KEY_FILE
             << " when they are missing - check that its folder is writable" << endl;
#else
        cerr << "Run: openssl req -x509 -newkey rsa:4096 -keyout " << SSL_KEY_FILE
             << " -out " << SSL_CERT_FILE << " -days 365 -nodes -subj \"/CN=krakensdr\"" << endl;
#endif
        platform::fast_exit(1);
    }
}

uWS::SSLApp WebSocketServer::create_ssl_app() {
    uWS::SocketContextOptions ssl_options = {};
    ssl_options.key_file_name = SSL_KEY_FILE;
    ssl_options.cert_file_name = SSL_CERT_FILE;

    return uWS::SSLApp(ssl_options)
        .get("/opus-decoder.min.js", [](auto* res, auto* req) {
            cout << "[WebServer] Request for opus-decoder.min.js from " << req->getUrl() << endl;

            // Serve opus-decoder.min.js from the same directory as the HTML
            ifstream file("opus-decoder.min.js", ios::binary);
            if (!file.is_open()) {
                cerr << "[WebServer] ERROR: Failed to open opus-decoder.min.js" << endl;
                res->writeStatus("404 Not Found")->end("opus-decoder.min.js not found");
                return;
            }

            stringstream buffer;
            buffer << file.rdbuf();
            string js_content = buffer.str();

            cout << "[WebServer] Serving opus-decoder.min.js (" << js_content.size() << " bytes)" << endl;

            res->writeHeader("Content-Type", "application/javascript; charset=utf-8")
               ->writeHeader("Cache-Control", "no-cache")  // Disable cache during debugging
               ->end(js_content);
        })
        .get("/array_calculator.html", [](auto* res, auto* /*req*/) {
            // Antenna array calculator (opened from the MUSIC DoA panel):
            // a static page, read from disk on each request like the main UI
            ifstream file("array_calculator.html", ios::binary);
            if (!file.is_open()) {
                res->writeStatus("404 Not Found")->end("array_calculator.html not found");
                return;
            }
            stringstream buffer;
            buffer << file.rdbuf();
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
               ->writeHeader("Cache-Control", "no-cache")
               ->end(buffer.str());
        })
        .get("/recordings/*", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // Download a recording from the fixed doa_recordings/ folder. Only a
            // sanitized base filename is honored, so no other device files are
            // reachable. With a token configured the request must carry it
            // (the page sends X-Kraken-Token): recordings hold bearings and
            // the station location.
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            std::string url(req->getUrl());
            const std::string prefix = "/recordings/";
            std::string raw = (url.size() > prefix.size()) ? url.substr(prefix.size()) : "";
            const std::string decoded = url_decode(raw);
            std::string name = request_name_ok(decoded) ? doa_sanitize_filename(decoded) : "";
            if (name.empty()) {
                res->writeStatus("400 Bad Request")->end("Invalid filename");
                return;
            }
            stream_download(res, doa_recordings_dir() + "/" + name, name);
        })
        .get("/recording/*", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // 📼 a recording of the recorder's folder: /recording/<name> (an IQ
            // recording = one .sigmf archive) or /recording/<name>/<file>.
            // Token as for /recordings: positions, what was said
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            std::string url(req->getUrl());
            const std::string prefix = "/recording/";
            const std::string rel = url_decode(url.size() > prefix.size() ? url.substr(prefix.size()) : "");
            if (!request_name_ok(rel)) {
                res->writeStatus("404 Not Found")->end("no such recording");
                return;
            }
            auto d = std::make_shared<rec::Download>();
            std::string status, err;
            if (!rec::download(rel, d.get(), &status, &err)) {
                res->writeStatus(status)->end(err);
                return;
            }
            stream_parts(res, d);
        })
        .post("/recording_upload/*", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // 📼 a SigMF archive into the recorder's folder (open it there to play
            // it). Streamed to the disk by a thread of its own; the socket is
            // paused while that is behind
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            std::string url(req->getUrl());
            const std::string prefix = "/recording_upload/";
            const std::string name = url_decode(url.size() > prefix.size() ? url.substr(prefix.size()) : "");
            if (!request_name_ok(name)) {
                res->writeStatus("400 Bad Request")->end("not a valid recording name");
                return;
            }
            const std::string cl(req->getHeader("content-length"));
            const uint64_t len = cl.empty() ? 0 : strtoull(cl.c_str(), nullptr, 10);
            std::string status, err;
            rec::Upload* up = rec::upload_begin(name, len, &status, &err);
            if (!up) {
                res->writeStatus(status)->end(err);
                return;
            }
            struct St { rec::Upload* up; bool aborted = false; };
            auto st = std::make_shared<St>(St{up});
            res->onAborted([st]() {
                st->aborted = true;
                if (st->up) rec::upload_abort(st->up);
                st->up = nullptr;
            });
            res->onData([res, st](std::string_view chunk, bool last) {
                if (!st->up) return;
                bool pause = false;
                std::string e;
                if (!rec::upload_data(st->up, chunk, &pause, &e)) {
                    rec::upload_abort(st->up);
                    st->up = nullptr;
                    res->writeStatus("500 Internal Server Error")->end(e);
                    return;
                }
                if (last) {
                    rec::Upload* u = st->up;
                    st->up = nullptr;
                    rec::upload_finish(u, [res, st](bool ok, std::string msg) {
                        if (!loop) return;
                        loop->defer([res, st, ok, msg]() {
                            if (st->aborted) return;
                            res->writeStatus(ok ? "200 OK" : "422 Unprocessable Entity")->end(msg);
                        });
                    });
                    return;
                }
                if (pause) {
                    res->pause();
                    rec::upload_on_drain(st->up, [res, st]() {
                        if (!loop) return;
                        loop->defer([res, st]() { if (!st->aborted) res->resume(); });
                    });
                }
            });
        })
        .get("/wb_telemetry/history", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // Wideband board telemetry sheet: the logged history of the last
            // ?range= seconds as JSON (wb_board_history_json). The log files
            // are read on a worker thread - a month of rows takes a while on a
            // Pi and must not stall the spectrum stream on the uWS loop.
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            const long range = strtol(std::string(req->getQuery("range")).c_str(), nullptr, 10);
            auto aborted = std::make_shared<bool>(false);
            res->onAborted([aborted]() { *aborted = true; });
            std::thread([res, aborted, range]() {
                std::string body = wb_board_history_json(range > 0 ? range : 3600);
                if (!loop) return;
                loop->defer([res, aborted, body = std::move(body)]() {
                    if (*aborted) return;
                    res->writeHeader("Content-Type", "application/json")->writeHeader("Cache-Control", "no-store")->end(body);
                });
            }).detach();
        })
        .get("/wb_telemetry/export.csv", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // ... and the logged rows of that range as a CSV download
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            const long range = strtol(std::string(req->getQuery("range")).c_str(), nullptr, 10);
            auto aborted = std::make_shared<bool>(false);
            res->onAborted([aborted]() { *aborted = true; });
            std::thread([res, aborted, range]() {
                std::string name;
                std::string path = wb_board_export_csv(range > 0 ? range : 3600, &name);
                if (!loop) return;
                loop->defer([res, aborted, path = std::move(path), name = std::move(name)]() {
                    if (*aborted) return;
                    if (path.empty()) { res->writeStatus("500 Internal Server Error")->end("Cannot write the export file"); return; }
                    stream_download(res, path, name);
                });
            }).detach();
        })
        .get("/voice_clip/*", [](auto* res, auto* req) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            // 🗣 Voice to Text: the audio clip of a transcript (<id>.opus in the
            // decoder log's voice_clips/ - only ids in the clip index). Token as
            // for /recordings: what was said.
            if (auth_required() && !token_matches(req->getHeader("x-kraken-token"))) {
                res->writeStatus("401 Unauthorized")->end("Access token required");
                return;
            }
            std::string url(req->getUrl());
            const std::string prefix = "/voice_clip/";
            const std::string name = url.size() > prefix.size() ? url.substr(prefix.size()) : "";
            const uint64_t id = strtoull(name.c_str(), nullptr, 10);
            const std::string path = id ? v2t::clip_path(id) : "";
            std::ifstream f(path, std::ios::binary);
            if (path.empty() || !f) {
                res->writeStatus("404 Not Found")->end("No such clip");
                return;
            }
            std::stringstream buf;
            buf << f.rdbuf();   // <= 25 s at 16 kbps: ~50 kB
            res->writeHeader("Content-Type", "audio/ogg")
               ->writeHeader("Cache-Control", "private, max-age=3600")
               ->end(buf.str());
        })
        .get("/*", [](auto* res, auto* /*req*/) {
            string html_content = load_html_content();
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
               ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
               ->writeHeader("Pragma", "no-cache")
               ->writeHeader("Expires", "0")
               ->end(html_content);
        }).ws<PerSocketData>("/*", {
        // permessage-deflate, used for the spectrum frames only (send(.., true)):
        // ~2x smaller there, ~0.1 ms each; audio (Opus) and the small JSON
        // messages go uncompressed. Shared compressor = no context per socket
        .compression = uWS::CompressOptions(uWS::SHARED_COMPRESSOR | uWS::SHARED_DECOMPRESSOR),
        // The largest real message is a scanner config (SCANNER_LOAD_CONFIG,
        // ~100 bytes per channel). uWS buffers a whole message before the
        // AUTH gate sees it, so this also bounds what an unauthenticated
        // client can make us hold (it was 16 MB per connection).
        .maxPayloadLength = 1024 * 1024,
        // A client that vanishes without closing (a phone leaving Wi-Fi) is
        // otherwise kept until the kernel gives up on TCP (~15 min), with
        // broadcasts piling up as backpressure. After 30 s with nothing from
        // the client uWS pings it (sendPingsAutomatically, the default) and
        // closes it if no pong follows; browsers answer pings on their own,
        // so live tabs - even idle, backgrounded ones - are never dropped.
        .idleTimeout = 30,
        .maxBackpressure = 2 * 1024 * 1024,  // 2MB - close slow clients to prevent OOM

        .upgrade = [](auto* res, auto* req, auto* context) {
            if (!request_allowed(req)) { refuse_request(res, req); return; }
            res->template upgrade<PerSocketData>(PerSocketData{},
                req->getHeader("sec-websocket-key"),
                req->getHeader("sec-websocket-protocol"),
                req->getHeader("sec-websocket-extensions"),
                context);
        },
        
        .open = [](auto* ws) {
            int count = ++ws_client_count;  // live count consumed by the status dashboard
            // Each browser gets every FFT/DoA/audio broadcast (up to 2 MB of
            // backpressure) - cap the count so a LAN host can't open hundreds.
            if (count > MAX_WS_CLIENTS) {
                std::cerr << "[WS] Refusing client: " << MAX_WS_CLIENTS << " already connected" << std::endl;
                ws->end(1013, "too many clients");  // "try again later"; .close decrements
                return;
            }
            std::cout << "[WS] Client CONNECTED! Total clients: " << count << std::endl;

            if (auth_required()) {
                const std::string ip(ws->getRemoteAddressAsText());
                if (auth_ip_blocked(ip)) {
                    ws->end(1008, "too many failed attempts");
                    return;
                }
                if (static_cast<int>(g_pending_auth.size()) >= MAX_PENDING_AUTH) {
                    ws->end(1013, "too many unauthenticated connections");
                    return;
                }
                g_pending_auth[ws] = steady_ms();
                // Hold off subscribing/syncing until AUTH:<token> arrives.
                ws->getUserData()->authed = false;
                ws->send("{\"auth_required\":true}", uWS::TEXT);
            } else {
                // Fail-open: behave exactly as before (subscribe + sync now).
                ws->getUserData()->authed = true;
                subscribe_and_sync(ws);
            }
        },

        .message = [](auto* ws, string_view message, uWS::OpCode) {
            auto* ud = ws->getUserData();

            // --- Authentication gate (no-op when no token is configured) ---
            if (message.starts_with("AUTH:")) {
                if (!auth_required() || ud->authed) return;  // harmless no-op
                g_pending_auth.erase(ws);
                const std::string ip(ws->getRemoteAddressAsText());
                if (auth_ip_blocked(ip)) {
                    ws->end(1008, "too many failed attempts");
                    return;
                }
                if (token_matches(message.substr(5))) {
                    ud->authed = true;
                    std::cout << "[WS] Client authenticated" << std::endl;
                    subscribe_and_sync(ws);
                } else {
                    note_auth_failure(ip);
                    ws->send("{\"auth_error\":\"invalid token\"}", uWS::TEXT);
                    ws->end(1008, "unauthorized");  // policy-violation close
                }
                return;
            }
            if (auth_required() && !ud->authed) return;  // drop until authed

            // --- Per-connection stream subscription (transport-level) ---
            if (message.starts_with("SUBSCRIBE:")) {
                uint32_t mask = parse_sub_mask(message.substr(10));
                apply_subscriptions(ws, mask);
                string ack = string("{\"subscribed\":{\"fft\":")
                    + ((mask & SUB_FFT)   ? "true" : "false")
                    + ",\"audio\":" + ((mask & SUB_AUDIO) ? "true" : "false")
                    + ",\"doa\":"   + ((mask & SUB_DOA)   ? "true" : "false")
                    + ",\"video\":" + ((mask & SUB_VIDEO) ? "true" : "false") + "}}";
                ws->send(ack, uWS::TEXT);
                return;
            }

            // --- Spectrum view (transport-level, this connection only) ---
            // FFT_VIEW:<display>:<seq>:<lo Hz>:<hi Hz>:<points> - display 0 =
            // the main spectrum, 1 + ch = independent-mode tuner ch; lo / hi
            // from that display's centre (lo >= hi = the whole band). The
            // first one switches the page to type 7 frames
            if (message.starts_with("FFT_VIEW:")) {
                handle_fft_view(ws, message.substr(9));
                return;
            }
            // SWEEP_VIEW:<seq>:<lo Hz>:<hi Hz>:<points> - sweep mode's window
            // (type 10 frames; an older page never sends it)
            if (message.starts_with("SWEEP_VIEW:")) {
                handle_sweep_view(ws, message.substr(11));
                return;
            }
            // SWEEP_OVERVIEW:<points> - the whole range with each completed sweep
            // for a zoomed page's waterfall (0 = off; an older receiver ignores it)
            if (message.starts_with("SWEEP_OVERVIEW:")) {
                handle_sweep_overview(ws, message.substr(15));
                return;
            }
            // SWEEP_FULL:<1|0> - the whole range at the sweep's own bins (type 11)
            // with each completed sweep (an older receiver ignores it)
            if (message.starts_with("SWEEP_FULL:")) {
                handle_sweep_full(ws, message.substr(11));
                return;
            }
            // NET_STATS:<1|0> - this connection's byte counters about once a
            // second (every mode; Linux; an older receiver ignores it)
            if (message.starts_with("NET_STATS:")) {
                handle_net_stats(ws, message.substr(10));
                return;
            }
            // 🎨 BIN_DOA_VIEW:1|0 (transport-level, this connection only):
            // the page takes type 9 frames with its main view's frames
            // (bin_doa.hpp); answered with the mode's state
            if (message.starts_with("BIN_DOA_VIEW:")) {
                const bool on = message.substr(13) == "1";
                ws->getUserData()->bin_doa = on;
                if (on) g_bin_doa_sockets.insert(ws); else g_bin_doa_sockets.erase(ws);
                bin_doa::set_viewers(static_cast<int>(g_bin_doa_sockets.size()));
                ws->send(bin_doa::state_json(), uWS::TEXT);
                return;
            }

            // a page's own command (power commands are taken only from here)
            ControlHandler::handle_page_message(message);
        },
        
        // a socket's queue drained: the next parts of its full set (SWEEP_FULL)
        .drain = [](auto* ws) {
            if (ws->getUserData()->full && sweep_mode_enabled.load(std::memory_order_relaxed))
                pump_full(ws, nullptr, sweep::grid_id());
        },

        .close = [](auto* ws, int code, string_view reason) {
            g_pending_auth.erase(ws);
            g_spec_sockets.erase(ws);
            g_sweep_sockets.erase(ws);
            g_net_sockets.erase(ws);
            g_video_sockets.erase(ws);
            dig::set_video_wanted(!g_video_sockets.empty());
            if (g_bin_doa_sockets.erase(ws)) bin_doa::set_viewers(static_cast<int>(g_bin_doa_sockets.size()));
            int remaining = --ws_client_count;  // keep the live dashboard count in sync
            if (remaining < 0) { ws_client_count.store(0); remaining = 0; }
            std::cout << "[WS] Client DISCONNECTED! Code=" << code
                      << ", reason='" << reason << "', clients remaining: " << remaining << std::endl;
        }
    });
}

void WebSocketServer::web_server_main() {
    verify_ssl_certificates();
    
    try {
        auto ssl_app = create_ssl_app();
        
        // Exclusive port: uWS defaults to SO_REUSEPORT, which lets a second
        // process (another kraken_doa, or kraken_pr) bind the same port
        // silently - the kernel then splits incoming connections randomly
        // between the processes. Fail loudly instead.
        ssl_app.listen(WEB_PORT, LIBUS_LISTEN_EXCLUSIVE_PORT, [](auto* listen_socket) {
            if (listen_socket) {
                cout << "HTTPS server listening on port " << WEB_PORT << endl;
                
                loop = uWS::Loop::get();
                struct us_loop_t* native_loop = (struct us_loop_t*)loop;
                broadcast_timer = us_create_timer(native_loop, 0, 0);
                audio_timer = us_create_timer(native_loop, 0, 0);
                doa_timer = us_create_timer(native_loop, 0, 0);
                
                us_timer_set(broadcast_timer, [](struct us_timer_t* /*timer*/) {
                    net_stats_tick();   // NET_STATS (every mode)
                    // Sweep mode: the sweep frames only - no regular spectrum
                    // (nothing is FFT'd but the sweep's hops)
                    if (sweep_mode_enabled.load(std::memory_order_relaxed)) {
                        if (global_ssl_app && !g_sweep_sockets.empty()) send_sweep_views();
                        return;
                    }
                    release_full_sets();
                    if (global_ssl_app && data_ready) {
                        // built every tick (it feeds the scanners); encoded
                        // only for pages without spectrum views
                        // the snapshot every tick: the views and the AI
                        // assistant's tools read it
                        SpectrumSnapshot& snap = MessageBuilders::spectrum_snapshot();
                        const bool legacy = global_ssl_app->numSubscribers(TOPIC_FFT) > 0;
                        auto message = MessageBuilders::build_fft_message(&snap, legacy);
                        MessageBuilders::note_spectrum();
                        if (legacy && !message.empty())
                            global_ssl_app->publish(TOPIC_FFT, message, uWS::BINARY, true);

                        // A beamformed FFT overlay per decimator (each
                        // steered to its own DoA) if beamforming is enabled:
                        // pages without views at the old frames' spacing,
                        // the others at their own (send_spectrum_views)
                        static vector<BeamformedSpectrum> bfs;
                        size_t nbf = 0;
                        if (beamforming_enabled.load(std::memory_order_relaxed)) {
                            for (const auto& inst : decimator_manager.getAllDecimators()) {
                                if (!inst || !inst->enabled ||
                                    inst->being_deleted.load(std::memory_order_relaxed)) continue;
                                if (bfs.size() <= nbf) bfs.emplace_back();
                                if (!MessageBuilders::beamformed_spectrum(inst->id, bfs[nbf])) continue;
                                if (legacy) {
                                    const string m = MessageBuilders::build_beamformed_fft_message(bfs[nbf], 0);
                                    if (!m.empty()) global_ssl_app->publish(TOPIC_FFT, m, uWS::BINARY, true);
                                }
                                nbf++;
                            }
                        }
                        bfs.resize(nbf);
                        if (!g_spec_sockets.empty()) send_spectrum_views(snap, bfs);
                    }
                }, 50, 50);
                
                // The timer ticks at half the packet period and each tick
                // sends the packets due by the steady clock. One packet per
                // 40 ms tick ran slower than the mixer filled on BOTH
                // platforms: Windows' libuv re-arms a repeating timer from the
                // moment it ran, so late wake-ups add up (~1 %, 17 % without
                // 1 ms timers - the buffer overran, the audio dropped out);
                // Linux's timerfd doesn't drift, but uSockets reads its
                // expiration count and runs the callback ONCE - every tick a
                // busy loop missed (high CPU) was a packet never sent, so the
                // mixer's buffers fell that much further behind for good
                const int audio_timer_ms = std::max(1, static_cast<int>(round(AUDIO_PACKET_TIME_MS)) / 2);
                us_timer_set(audio_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app) return;

                    if (!fm_enabled.load() || sweep_mode_enabled.load(std::memory_order_relaxed)) {
                        return;   // (sweep mode: nothing is demodulated)
                    }

                    static int64_t due_us = 0;
                    const int64_t now_us = chrono::duration_cast<chrono::microseconds>(
                        chrono::steady_clock::now().time_since_epoch()).count();
                    const int64_t period_us = static_cast<int64_t>(AUDIO_PACKET_TIME_MS * 1000.0f);
                    // first tick, back from a stall or from FM off: no burst of
                    // old packets (what the mixer holds meanwhile is trimmed -
                    // get_audio_buffer's latency control)
                    if (due_us == 0 || now_us - due_us > 200000) due_us = now_us;
                    int packets;
                    for (packets = 0; packets < 3 && due_us <= now_us; packets++) due_us += period_us;
                    for (int i = 0; i < packets; i++) {
                        auto audio_message = MessageBuilders::build_audio_message();
                        if (!audio_message.empty()) {
                            global_ssl_app->publish(TOPIC_AUDIO, audio_message, uWS::BINARY, false);
                        }
                    }
                }, audio_timer_ms, audio_timer_ms);
                
                us_timer_set(doa_timer, [](struct us_timer_t* /*timer*/) {
                    if (global_ssl_app && doa_enabled.load()) {
                        auto doa_message = MessageBuilders::build_multi_doa_message();
                        if (!doa_message.empty()) {
                            global_ssl_app->publish(TOPIC_DOA, doa_message, uWS::BINARY, false);
                        }
                    }
                }, 200, 200);

                // Digital decoder status (VFOs with a decoder on) - 4 Hz
                struct us_timer_t* digital_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(digital_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app || !decimator_manager.anyDigitalActive()) return;
                    auto msg = MessageBuilders::build_digital_message();
                    if (!msg.empty()) global_ssl_app->publish(TOPIC_CTL, msg, uWS::TEXT, false);
                }, 250, 250);

                // 🗺 Map: decoder map points + station location - 1 Hz
                struct us_timer_t* map_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(map_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app) return;
                    global_ssl_app->publish(TOPIC_CTL, MessageBuilders::build_map_message(), uWS::TEXT, false);
                    // incidents for the decoder tabs, when they changed
                    const std::string inc = MessageBuilders::build_incidents_message();
                    if (!inc.empty()) global_ssl_app->publish(TOPIC_CTL, inc, uWS::TEXT, false);
                }, 1000, 1000);

                // 🗺 Map, mobile DF: live lobes 2 Hz + changed heat maps
                struct us_timer_t* rdf_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(rdf_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app || global_ssl_app->numSubscribers(TOPIC_CTL) == 0) return;
                    global_ssl_app->publish(TOPIC_CTL, rdfmap::status_message(), uWS::TEXT, false);
                    for (const auto& m : rdfmap::grid_messages()) global_ssl_app->publish(TOPIC_CTL, m, uWS::TEXT, false);
                }, 500, 500);

                // ✈ Array calibration (packets collected, the latest fit) - every 2 s
                struct us_timer_t* arraycal_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(arraycal_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app || global_ssl_app->numSubscribers(TOPIC_CTL) == 0) return;
                    global_ssl_app->publish(TOPIC_CTL, array_cal::status_message(), uWS::TEXT, false);
                }, 2000, 2000);

                // 🗣 Voice to Text: new transcripts as they come and the live
                // rows (Live text) when they changed - every 250 ms, the new
                // transcripts first, so a final replaces its live row in the
                // same tick -; the status when it changed, once a second (a
                // connecting page asks with GET_VOICE2TEXT)
                struct us_timer_t* v2t_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(v2t_timer, [](struct us_timer_t* /*timer*/) {
                    static std::string last_status;
                    static unsigned tick = 0;
                    if (!global_ssl_app) return;
                    std::string fresh, live;
                    v2t::take_pushes(&fresh, &live);   // (one lock: a final with its row's removal)
                    if (global_ssl_app->numSubscribers(TOPIC_CTL) == 0) return;
                    if (!fresh.empty()) global_ssl_app->publish(TOPIC_CTL, fresh, uWS::TEXT, false);
                    if (!live.empty()) global_ssl_app->publish(TOPIC_CTL, live, uWS::TEXT, false);
                    if (tick++ % 4) return;
                    std::string st = v2t::status_message();
                    if (st != last_status) {
                        global_ssl_app->publish(TOPIC_CTL, st, uWS::TEXT, false);
                        last_status = std::move(st);
                    }
                }, 250, 250);

                // 🧪 Debug playback: status when it changed, every 2 s while
                // playing / generating
                struct us_timer_t* pb_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(pb_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app) return;
                    const std::string m = playback::tick_message();
                    if (!m.empty()) global_ssl_app->publish(TOPIC_CTL, m, uWS::TEXT, false);
                }, 1000, 1000);

                // 📼 Recorder: status when it changed / every second while
                // recording, the recordings list when it changed
                struct us_timer_t* rec_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(rec_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app) return;
                    for (const auto& m : rec::tick_messages()) global_ssl_app->publish(TOPIC_CTL, m, uWS::TEXT, false);
                }, 1000, 1000);

                // 🗂 Decoder Logging status (disk space, sizes) - every 2 s
                struct us_timer_t* declog_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(declog_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app || global_ssl_app->numSubscribers(TOPIC_CTL) == 0) return;
                    global_ssl_app->publish(TOPIC_CTL, declog::status_message(), uWS::TEXT, false);
                }, 2000, 2000);

                // System status timer - broadcasts every 500ms
                struct us_timer_t* status_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(status_timer, [](struct us_timer_t* /*timer*/) {
                    sweep_pending_auth();
                    if (global_ssl_app) {
                        auto status_message = MessageBuilders::build_system_status_message();
                        global_ssl_app->publish(TOPIC_CTL, status_message, uWS::TEXT, false);
                    }
                }, 500, 500);

            } else {
                // exit() is NOT safe on this thread: static destructors would
                // run while the worker threads are live and trip std::terminate
                // on their joinable std::thread objects. Restore the terminal
                // first (the TUI otherwise swallows the message), then leave
                // without destructors - same rule as main()'s shutdown path.
                StatusDashboard::end();
                cerr << "FATAL ERROR: Failed to listen on HTTPS port " << WEB_PORT
                     << " - is another kraken_doa (or kraken_pr) already using it?"
#ifdef _WIN32
                     // or a port range Hyper-V / WSL / Docker reserved (bind is refused then)
                     << " (check: netstat -ano -p tcp | findstr :" << WEB_PORT
                     << " and: netsh interface ipv4 show excludedportrange protocol=tcp)" << endl;
#else
                     << " (check: ss -tlnp | grep " << WEB_PORT << ")" << endl;
#endif
                platform::fast_exit(1);
            }
        });

        global_ssl_app = &ssl_app;
        // the AI assistant's MCP server: a second app on this loop, so its
        // control tools run on the thread every command runs on
        ai_mcp::attach();
        ssl_app.run();

    } catch (const exception& e) {
        // Same as the listen-failure path: no exit() from this thread.
        StatusDashboard::end();
        cerr << "FATAL SSL ERROR: " << e.what() << endl;
        platform::fast_exit(1);
    }
}

bool WebSocketServer::auth_enabled() { return auth_required(); }

void WebSocketServer::broadcast_json_message(const string& json) {
    if (!global_ssl_app || !loop) return;

    // CRITICAL: uWebSockets publish() is NOT thread-safe.
    // The scanner thread (and potentially others) call this from outside the
    // event loop thread. Using loop->defer() queues the publish on the event
    // loop thread where it's safe. defer() is the ONLY thread-safe uWS method.
    string json_copy = json;
    loop->defer([json_copy = std::move(json_copy)]() {
        if (global_ssl_app) {
            global_ssl_app->publish(TOPIC_CTL, json_copy, uWS::TEXT, false);
        }
    });
}

void WebSocketServer::publish_video(string msg) {
    if (!global_ssl_app || !loop || msg.empty()) return;
    // publish() is not thread safe and the frame comes from the decoder's
    // worker thread, so defer onto the uWS loop (the only thread-safe entry).
    // A dedicated video frame can be ~1 MB - drop it for a socket whose send
    // buffer is already deep rather than queueing or closing the connection.
    constexpr unsigned VIDEO_MAX_BUFFERED = 512 * 1024;
    // permessage-deflate only for raw pixels (type-8 byte 13 = encoding 0):
    // a JPEG is already compressed, and deflating it again on the loop thread
    // costs CPU for nothing
    const bool compress = msg.size() > 13 && static_cast<uint8_t>(msg[13]) == 0;
    loop->defer([msg = std::move(msg), compress]() {
        if (!global_ssl_app) return;
        for (ClientWS* ws : g_video_sockets) {
            PerSocketData* ud = ws->getUserData();
            if (!(ud->sub_mask & SUB_VIDEO) || ws->getBufferedAmount() > VIDEO_MAX_BUFFERED) continue;
            ws->send(msg, uWS::BINARY, compress);
        }
    });
}

void WebSocketServer::doa_http_server_thread() {
    // Plain HTTP server for DOA_value.html (KrakenSDR Android app compatibility)
    // Runs on a separate thread with its own event loop
    try {
        uWS::App()
            .get("/DOA_value.html", [](auto* res, auto* req) {
                // Open to native clients (the Android app can't send the API
                // token), but only under a name this device is reached by (DNS
                // rebinding) and with no CORS header: Access-Control-Allow-
                // Origin: * let any web page in a LAN browser read the bearings
                // and the station location.
                if (!host_allowed(req->getHeader("host"))) {
                    res->writeStatus("403 Forbidden")->end("request refused");
                    return;
                }
                // Android Doppler compatibility adjustment (2026):
                // Retain a complete message per source-frame set, not per HTTP poll.
                // This handler owns its cache on the same event-loop thread.
                static std::string last_good_message;
                static std::string last_source_key;

                // "Retune calibration in progress": while the server retunes it
                // pulses the noise source, and bearings computed on that injected
                // noise/settling data are garbage, so we hold the previous values
                // until it settles. doa_is_calibrating() gates ONLY on the noise
                // source (+ a short hold for bursts), NOT phase_state — in steady
                // fixed-frequency DoA the server reports phase_state 4 (CONVERGED),
                // so a phase_state test would freeze ALL output. (Shared with the
                // DoA logger, see doa_logger.cpp.)
                if (doa_is_calibrating()) {
                    // Freeze: serve the last good bearings unchanged.
                    res->writeHeader("Content-Type", "text/html; charset=utf-8")
                       ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
                           ->end(last_good_message);
                    return;
                }

                // Rebuild the whole legacy CSV only if a MUSIC source frame
                // changes; different HTTP polls must not invent measurements.
                auto records = capture_doa_records();
                std::string source_key;
                for (const DoaRecord& rec : records) {
                    const int64_t stamp = rec.source_stamp_ms > 0
                        ? rec.source_stamp_ms : rec.timestamp_ms;
                    source_key += std::to_string(rec.decimator_id) + ":" +
                                  std::to_string(stamp) + ";";
                }
                if (last_good_message.empty() || source_key != last_source_key) {
                    std::string message;
                    for (const DoaRecord& rec : records) {
                        message += format_doa_csv_line(rec);
                    }
                    last_good_message = std::move(message);
                    last_source_key = std::move(source_key);
                }

                res->writeHeader("Content-Type", "text/html; charset=utf-8")
                   ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
                   ->end(last_good_message);
            })
            .get("/*", [](auto* res, auto* /*req*/) {
                res->writeStatus("404 Not Found")->end("Not found");
            })
            .listen(DOA_HTTP_PORT, LIBUS_LISTEN_EXCLUSIVE_PORT, [](auto* listen_socket) {
                if (listen_socket) {
                    cout << "DOA HTTP server listening on http://localhost:" << DOA_HTTP_PORT << "/DOA_value.html" << endl;
                } else {
                    cerr << "WARNING: Failed to listen on HTTP port " << DOA_HTTP_PORT << " for DOA_value.html" << endl;
                }
            }).run();
    } catch (const exception& e) {
        cerr << "DOA HTTP server error: " << e.what() << endl;
    }
}