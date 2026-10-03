#include "networking/websocket_server.hpp"
#include "utils/json_escape.hpp"
#include "utils/host_check.hpp"
#include "globals.hpp"
#include "config.hpp"
#include "control_handler.hpp"
#include "message_builders.hpp"
#include "scanner_manager.hpp"
#include "decimator_manager.hpp"
#include "channel_manager.hpp"
#include "networking/gpsd_client.hpp"
#include "station_info.hpp"
#include "signal_processing/fft_processor.hpp"
#include "doa_logger.hpp"
#include "utils/status_dashboard.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <unordered_map>
#include <iomanip>
#include <cstdlib>
#include <iterator>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

using namespace std;

namespace {

// Per-connection state stored by uWS for each WebSocket.
struct PerSocketData {
    bool authed = false;     // has this connection passed the AUTH gate?
    uint32_t sub_mask = 0;   // subscribed data streams (0 = unset; see SUB_*)
};

// Data-stream subscription bits (the heavy/optional streams).
constexpr uint32_t SUB_FFT   = 1u << 0;
constexpr uint32_t SUB_AUDIO = 1u << 1;
constexpr uint32_t SUB_DOA   = 1u << 2;
constexpr uint32_t SUB_ALL   = SUB_FFT | SUB_AUDIO | SUB_DOA;

// pub/sub topics. "ctl" carries control-plane traffic (sync_cmd echoes,
// scanner/doa state, system_status) and is ALWAYS subscribed so multi-client
// settings-sync stays correct. fft/audio/doa are selectable via SUBSCRIBE.
constexpr string_view TOPIC_CTL   = "ctl";
constexpr string_view TOPIC_FFT   = "fft";
constexpr string_view TOPIC_AUDIO = "audio";
constexpr string_view TOPIC_DOA   = "doa";

// Shared API token, loaded once. Empty => auth disabled (fail-open).
const string& auth_token() {
    static const string token = [] {
        if (const char* env = std::getenv("KRAKEN_API_TOKEN"); env && *env)
            return string(env);
        ifstream f(AUTH_TOKEN_FILE);
        if (f.good()) {
            string t((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
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
    if (mask & SUB_FFT)   ws->subscribe(TOPIC_FFT);   else ws->unsubscribe(TOPIC_FFT);
    if (mask & SUB_AUDIO) ws->subscribe(TOPIC_AUDIO); else ws->unsubscribe(TOPIC_AUDIO);
    if (mask & SUB_DOA)   ws->subscribe(TOPIC_DOA);   else ws->unsubscribe(TOPIC_DOA);
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

// Stream a file to the client as an attachment (download), with backpressure
// handling so large recordings don't have to be buffered in memory. Re-supplies
// data from getWriteOffset() on each writable event, per the uWS tryEnd model.
template<bool SSL>
void stream_download(uWS::HttpResponse<SSL>* res, const std::string& fullpath,
                     const std::string& dlname) {
    auto file = std::make_shared<std::ifstream>(fullpath, std::ios::binary);
    if (!file->good()) {
        res->writeStatus("404 Not Found")->end("Recording not found");
        return;
    }
    file->seekg(0, std::ios::end);
    uintmax_t total = static_cast<uintmax_t>(file->tellg());
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
    ifstream cert_file(SSL_CERT_FILE);
    ifstream key_file(SSL_KEY_FILE);
    
    if (!cert_file.good() || !key_file.good()) {
        // Runs on the web-server thread: no exit() here either (see the
        // listen-failure path below for why).
        StatusDashboard::end();
        cerr << "ERROR: Cannot read SSL certificate files" << endl;
        cerr << "Run: openssl req -x509 -newkey rsa:4096 -keyout " << SSL_KEY_FILE
             << " -out " << SSL_CERT_FILE << " -days 365 -nodes -subj \"/CN=krakensdr\"" << endl;
        _Exit(1);
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
            std::string name = doa_sanitize_filename(url_decode(raw));
            if (name.empty()) {
                res->writeStatus("400 Bad Request")->end("Invalid filename");
                return;
            }
            stream_download(res, doa_recordings_dir() + "/" + name, name);
        })
        .get("/*", [](auto* res, auto* /*req*/) {
            string html_content = load_html_content();
            res->writeHeader("Content-Type", "text/html; charset=utf-8")
               ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
               ->writeHeader("Pragma", "no-cache")
               ->writeHeader("Expires", "0")
               ->end(html_content);
        }).ws<PerSocketData>("/*", {
        .compression = uWS::DISABLED,
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
                    + ",\"doa\":"   + ((mask & SUB_DOA)   ? "true" : "false") + "}}";
                ws->send(ack, uWS::TEXT);
                return;
            }

            ControlHandler::handle_websocket_message(message);
        },
        
        .close = [](auto* ws, int code, string_view reason) {
            g_pending_auth.erase(ws);
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
                    if (global_ssl_app && data_ready) {
                        auto message = MessageBuilders::build_fft_message();
                        global_ssl_app->publish(TOPIC_FFT, message, uWS::BINARY, false);

                        // Send a beamformed FFT overlay per decimator (each
                        // steered to its own DoA) if beamforming is enabled.
                        if (beamforming_enabled.load(std::memory_order_relaxed)) {
                            for (const auto& inst : decimator_manager.getAllDecimators()) {
                                if (!inst || !inst->enabled ||
                                    inst->being_deleted.load(std::memory_order_relaxed)) continue;
                                auto bf_message = MessageBuilders::build_beamformed_fft_message(inst->id);
                                if (!bf_message.empty()) {
                                    global_ssl_app->publish(TOPIC_FFT, bf_message, uWS::BINARY, false);
                                }
                            }
                        }
                    }
                }, 50, 50);
                
                int audio_timer_ms = static_cast<int>(round(AUDIO_PACKET_TIME_MS));
                us_timer_set(audio_timer, [](struct us_timer_t* /*timer*/) {
                    if (!global_ssl_app) return;

                    if (!fm_enabled.load()) {
                        return;
                    }

                    auto audio_message = MessageBuilders::build_audio_message();
                    if (!audio_message.empty()) {
                        global_ssl_app->publish(TOPIC_AUDIO, audio_message, uWS::BINARY, false);
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
                     << " (check: ss -tlnp | grep " << WEB_PORT << ")" << endl;
                _Exit(1);
            }
        });
        
        global_ssl_app = &ssl_app;
        ssl_app.run();
        
    } catch (const exception& e) {
        // Same as the listen-failure path: no exit() from this thread.
        StatusDashboard::end();
        cerr << "FATAL SSL ERROR: " << e.what() << endl;
        _Exit(1);
    }
}

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
                // Last fully-computed payload, served verbatim while a retune
                // calibration is in progress. Confined to this thread's event
                // loop (the only place this handler runs), so no lock needed.
                static std::string last_good_message;

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

                // Build the body from the shared capture/format helpers so the
                // served CSV stays byte-identical to what the logger writes.
                std::string message;
                for (const DoaRecord& rec : capture_doa_records()) {
                    message += format_doa_csv_line(rec);
                }

                // Calibration is settled here: this is a trustworthy frame, so
                // remember it as the value to hold during the next retune cycle.
                last_good_message = message;

                res->writeHeader("Content-Type", "text/html; charset=utf-8")
                   ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
                   ->end(message);
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