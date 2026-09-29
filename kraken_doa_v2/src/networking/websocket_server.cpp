#include "networking/websocket_server.hpp"
#include "utils/json_escape.hpp"
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
    string html = buffer.str();

    // Inject the API auth token (empty when auth is disabled) so the page can
    // authenticate on connect. Only template substitution in the client.
    const string placeholder = "{{AUTH_TOKEN}}";
    const string& token = auth_token();
    for (size_t pos = html.find(placeholder); pos != string::npos;
         pos = html.find(placeholder, pos + token.size())) {
        html.replace(pos, placeholder.size(), token);
    }
    return html;
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
        .get("/recordings/*", [](auto* res, auto* req) {
            // Download a recording from the fixed doa_recordings/ folder. Only a
            // sanitized base filename is honored, so no other device files are
            // reachable.
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
        .maxPayloadLength = 16 * 1024 * 1024,
        .idleTimeout = 0,
        .maxBackpressure = 2 * 1024 * 1024,  // 2MB - close slow clients to prevent OOM
        
        .open = [](auto* ws) {
            int count = ++ws_client_count;  // live count consumed by the status dashboard
            std::cout << "[WS] Client CONNECTED! Total clients: " << count << std::endl;

            if (auth_required()) {
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
                if (token_matches(message.substr(5))) {
                    ud->authed = true;
                    std::cout << "[WS] Client authenticated" << std::endl;
                    subscribe_and_sync(ws);
                } else {
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
        
        .close = [](auto* /*ws*/, int code, string_view reason) {
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

                // System status timer - broadcasts every 500ms
                struct us_timer_t* status_timer = us_create_timer(native_loop, 0, 0);
                us_timer_set(status_timer, [](struct us_timer_t* /*timer*/) {
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
            .get("/DOA_value.html", [](auto* res, auto* /*req*/) {
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
                       ->writeHeader("Access-Control-Allow-Origin", "*")
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
                   ->writeHeader("Access-Control-Allow-Origin", "*")
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