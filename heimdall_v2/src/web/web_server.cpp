#include "web_server.hpp"
#include "html_loader.hpp"
#include "../sdr/sdr_init.hpp"
#include "../sdr/pipeline_control.hpp"
#include "../dsp/correlation.hpp"  // For build_correlation_message
#include "../dsp/compensation.hpp" // get_phase_compensation_state (kerberos stale check)
#include "../core/config.hpp"
#include "../core/logging.hpp"
#include "../core/utils.hpp"
#include "../core/settings.hpp"
#include "../core/forward_comp.hpp"
#include "App.h"  // uWebSockets main header
#include <iostream>
#include <map>
#include <functional>
#include <string>
#include <algorithm>
#include <cctype>
#include <thread>
#include <optional>
#include <cmath>

// Global web server state
std::thread web_thread;
void* global_app = nullptr;

// Global references for timer callback (set by main.cpp)
static CorrelationResult* global_correlation_result = nullptr;
static FFTProcessingControl* global_fft_control = nullptr;

// uWebSockets C interface for timer
extern "C" {
    struct us_timer_t* us_create_timer(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);
    void us_timer_set(struct us_timer_t* timer, void (*cb)(struct us_timer_t* t), int ms, int repeat_ms);
}

// Minimal JSON string escaper (filenames may contain spaces; quotes/backslashes
// are escaped, control chars dropped, invalid UTF-8 replaced with U+FFFD - a
// browser closes a WebSocket on invalid UTF-8 in a TEXT frame). Enough for
// the small strings we emit.
static std::string json_escape(const std::string& in) {
    const std::string s = utf8_sanitize(in);
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o;
}

// Same-origin guard for the state-changing endpoints (the WebSocket upgrade and
// the POST routes). Browsers attach an Origin header to every cross-site
// WebSocket handshake and POST, so without this any web page the operator
// visits could drive heimdall - retune, change the element count, switch the
// noise source (with --kerberos: a noise calibration against live antennas),
// upload/delete calibration files. A request WITHOUT Origin is not a browser
// cross-site request (curl, scripts, test tools) and is allowed; "null"
// (sandboxed / file:// pages) is refused.
static bool origin_allowed(uWS::HttpRequest* req) {
    const std::string_view origin = req->getHeader("origin");
    if (origin.empty()) return true;
    const size_t scheme_end = origin.find("://");
    if (scheme_end == std::string_view::npos) return false;
    const std::string_view origin_host = origin.substr(scheme_end + 3);
    const std::string_view host = req->getHeader("host");
    return !host.empty() && origin_host.size() == host.size() &&
           std::equal(origin_host.begin(), origin_host.end(), host.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

template <typename Res>
static void refuse_cross_origin(Res* res, uWS::HttpRequest* req) {
    std::cerr << "Web: refused cross-origin request (Origin: " << json_escape(std::string(req->getHeader("origin")))
              << ", Host: " << json_escape(std::string(req->getHeader("host"))) << ")" << std::endl;
    res->writeStatus("403 Forbidden")->end("cross-origin request refused");
}

// Periodic-recal + forward-comp state pushed to the UI as a TEXT frame (the
// binary broadcast feed carries correlation/FFT data). Sent on connect and
// re-broadcast at a low rate from the timer so the counters / settings stay live.
static std::string build_state_message() {
    std::string s = std::string("STATE:{\"num_elements\":")
        + std::to_string(active_num_elements.load(std::memory_order_relaxed))
        + ",\"max_elements\":"
        + std::to_string(expected_serials.size())
        + ",\"reconfiguring\":"
        + (reconfig_in_progress.load(std::memory_order_relaxed) ? "true" : "false")
        + ",\"periodic_recal_enabled\":"
        + (periodic_recal_enabled.load(std::memory_order_relaxed) ? "true" : "false")
        + ",\"periodic_recal_minutes\":"
        + std::to_string(periodic_recal_minutes.load(std::memory_order_relaxed))
        + ",\"recal_lag_fails\":"
        + std::to_string(periodic_recal_lag_fail_count.load(std::memory_order_relaxed))
        + ",\"recal_phase_fails\":"
        + std::to_string(periodic_recal_phase_fail_count.load(std::memory_order_relaxed))
        + ",\"antenna_bias_tee_mask\":"
        + std::to_string(antenna_bias_tee_mask.load(std::memory_order_relaxed));

    // Tunable RF range for the frequency box (depends on --wideband).
    uint64_t rf_min, rf_max;
    rf_frequency_range(rf_min, rf_max);
    s += ",\"rf_min_hz\":" + std::to_string(rf_min) + ",\"rf_max_hz\":" + std::to_string(rf_max);

    // KerberosSDR support mode: drives the warning banner + guarded recal button.
    s += ",\"kerberos_mode\":";
    s += kerberos_mode.load(std::memory_order_relaxed) ? "true" : "false";
    if (kerberos_mode.load(std::memory_order_relaxed)) {
        s += ",\"kerberos_sw\":";
        s += kerberos_sw_mode.load(std::memory_order_relaxed) ? "true" : "false";
        s += ",\"calibration_state\":\"";
        s += kerberos_calibration_state();
        s += "\"";
    }

    // Forward (S2P) phase compensation state.
    s += ",\"fwd_comp_enabled\":";
    s += forward_comp.enabled.load(std::memory_order_relaxed) ? "true" : "false";
    s += ",\"fwd_comp_amplitude\":";
    s += forward_comp.correct_amplitude.load(std::memory_order_relaxed) ? "true" : "false";
    {
        std::lock_guard<std::mutex> lk(forward_comp.mutex);
        s += ",\"fwd_comp_files\":[";
        for (int i = 0; i < NUM_DEVICES; ++i) {
            if (i) s += ',';
            s += '"'; s += json_escape(forward_comp.files[i]); s += '"';
        }
        s += "],\"fwd_comp_loaded\":[";
        for (int i = 0; i < NUM_DEVICES; ++i) {
            if (i) s += ',';
            s += forward_comp.loaded[i] ? "true" : "false";
        }
        s += "]";
    }
    s += "}";
    return s;
}

void web_server_main(CorrelationResult& correlation_result, FFTProcessingControl& fft_control) {
    // Store global references for timer callback
    global_correlation_result = &correlation_result;
    global_fft_control = &fft_control;
    
    struct PerSocketData {};
    
    auto app = uWS::App().get("/s2p_files", [](auto* res, auto* /*req*/) {
        // List the .s2p files available in the calibration folder so the UI can
        // populate the per-channel dropdowns. (Registered before "/*" so the
        // wildcard HTML handler does not swallow it.)
        std::string body = "{\"dir\":\"" + json_escape(fwdcomp::CAL_DIR) + "\",\"files\":[";
        auto files = fwdcomp::list_files();
        for (size_t i = 0; i < files.size(); ++i) {
            if (i) body += ',';
            body += '"'; body += json_escape(files[i]); body += '"';
        }
        body += "]}";
        res->writeHeader("Content-Type", "application/json")->end(body);
    }).post("/s2p_upload", [](auto* res, auto* req) {
        if (!origin_allowed(req)) { refuse_cross_origin(res, req); return; }
        // Browser uploads one .s2p file: target filename in the ?name= query
        // (URL-decoded), the raw file as the request body. The body streams in,
        // so we accumulate it and act on the final chunk. (req is only valid
        // synchronously - capture the name before returning.)
        std::string fname(req->getQuery("name"));
        auto body    = std::make_shared<std::string>();
        auto aborted = std::make_shared<bool>(false);
        auto over    = std::make_shared<bool>(false);
        res->onAborted([aborted]() { *aborted = true; });
        res->onData([res, body, aborted, over, fname = std::move(fname)]
                    (std::string_view chunk, bool last) mutable {
            constexpr size_t kMaxUpload = 8u * 1024 * 1024;  // S2P files are KB; cap abuse
            if (!*over) {
                if (body->size() + chunk.size() > kMaxUpload) *over = true;
                else body->append(chunk.data(), chunk.size());
            }
            if (!last || *aborted) return;

            std::string err;
            bool ok;
            if (*over) { ok = false; err = "file too large"; }
            else       ok = fwdcomp::save_uploaded_file(fname, *body, err);
            if (ok)
                std::cout << "Forward comp: uploaded S2P file '" << fname << "'" << std::endl;
            else
                std::cerr << "Forward comp: upload of '" << fname << "' rejected: " << err << std::endl;

            std::string out = std::string("{\"ok\":") + (ok ? "true" : "false")
                + ",\"name\":\"" + json_escape(fname) + "\"";
            if (!ok) out += ",\"error\":\"" + json_escape(err) + "\"";
            out += "}";
            if (ok) res->writeHeader("Content-Type", "application/json")->end(out);
            else    res->writeStatus("400 Bad Request")
                       ->writeHeader("Content-Type", "application/json")->end(out);
        });
    }).post("/s2p_delete", [](auto* res, auto* req) {
        if (!origin_allowed(req)) { refuse_cross_origin(res, req); return; }
        // Delete a calibration file (?name=). Synchronous - no body to read.
        std::string fname(req->getQuery("name"));
        std::string err;
        bool ok = fwdcomp::delete_file(fname, err);
        if (ok) {
            // delete_file cleared any channel that used it; rebuild the
            // correction at the current frequency and persist the change.
            fwdcomp::recompute(static_cast<double>(current_frequency.load()));
            settings::save();
            std::cout << "Forward comp: deleted S2P file '" << fname << "'" << std::endl;
        } else {
            std::cerr << "Forward comp: delete of '" << fname << "' failed: " << err << std::endl;
        }
        std::string out = std::string("{\"ok\":") + (ok ? "true" : "false")
            + ",\"name\":\"" + json_escape(fname) + "\"";
        if (!ok) out += ",\"error\":\"" + json_escape(err) + "\"";
        out += "}";
        if (ok) res->writeHeader("Content-Type", "application/json")->end(out);
        else    res->writeStatus("400 Bad Request")
                   ->writeHeader("Content-Type", "application/json")->end(out);
    }).get("/*", [](auto* res, auto* /*req*/) {
        // no-store: a phone rendering a cached copy of this page while the
        // server is unreachable is indistinguishable from a live server with
        // a broken WebSocket. A rendered page must mean a live server.
        res->writeHeader("Content-Type", "text/html; charset=utf-8")
           ->writeHeader("Cache-Control", "no-cache, no-store, must-revalidate")
           ->writeHeader("Pragma", "no-cache")
           ->writeHeader("Expires", "0")
           ->end(get_html_content());
    }).ws<PerSocketData>("/*", {
        .compression = uWS::DISABLED,
        .maxPayloadLength = 16 * 1024,
        .idleTimeout = 120,

        // Refuse cross-site WebSocket hijacking (see origin_allowed)
        .upgrade = [](auto* res, auto* req, auto* context) {
            if (!origin_allowed(req)) { refuse_cross_origin(res, req); return; }
            res->template upgrade<PerSocketData>(PerSocketData{},
                req->getHeader("sec-websocket-key"),
                req->getHeader("sec-websocket-protocol"),
                req->getHeader("sec-websocket-extensions"),
                context);
        },
        
        .open = [](auto* ws) {
            ws->subscribe("broadcast");
            std::cout << "WebSocket client connected" << std::endl;
            // Push the current periodic-recal state immediately so the UI reflects
            // the persisted settings + counters on load.
            ws->send(build_state_message(), uWS::TEXT);
        },
        
        .message = [&](auto* /*ws*/, std::string_view message, uWS::OpCode) {
            // While an element-count reconfiguration owns the devices, every
            // command is refused: most handlers below touch device handles that
            // are being closed/reopened on another thread.
            if (reconfig_in_progress.load(std::memory_order_acquire)) {
                std::cerr << "Web: command ignored during element-count reconfiguration: "
                          << std::string(message).substr(0, 40) << std::endl;
                return;
            }

            // Backstop: an exception escaping a uWS handler aborts the whole
            // server, so any malformed command a handler failed to guard is
            // logged and dropped here instead.
            try {
                static const std::map<std::string_view, std::function<void()>> handlers = {
                    {"FFT_ENABLE", [&fft_control]() {
                        std::lock_guard<std::mutex> lock(fft_control.control_mutex);
                        bool was_auto = fft_control.auto_disabled;
                        fft_control.fft_enabled = true;
                        fft_control.auto_disabled = false;
                        if (was_auto) fft_control.user_override = true;
                        std::cout << "FFT: Enabled" << std::endl;
                    }},
                    {"FFT_DISABLE", [&fft_control]() {
                        std::lock_guard<std::mutex> lock(fft_control.control_mutex);
                        fft_control.fft_enabled = false;
                        fft_control.auto_disabled = false;
                        std::cout << "FFT: Disabled" << std::endl;
                    }},
                    {"BIAS_TEE_ENABLE", []() { set_bias_tee_all_devices(true, devices); }},
                    {"BIAS_TEE_DISABLE", []() { set_bias_tee_all_devices(false, devices); }},
                    {"PER_BIN_ENABLE", []() {
                        per_bin_cal.enabled.store(true, std::memory_order_release);
                        settings::save();  // remember across restarts
                        std::cout << "Per-bin phase calibration: Enabled" << std::endl;
                        // If calibration already finished, re-open it to build the
                        // equalizer; otherwise the in-progress run will build it on
                        // convergence (per_bin_measured is false).
                        bool converged = false;
                        if (phase_compensation) {
                            std::lock_guard<std::mutex> lock(phase_compensation->state_mutex);
                            converged = (phase_compensation->state == PhaseCompensatorState::CONVERGED);
                        }
                        if (converged) handle_settings_change();
                    }},
                    {"PER_BIN_DISABLE", []() {
                        per_bin_cal.enabled.store(false, std::memory_order_release);
                        per_bin_cal.ready.store(false, std::memory_order_release);
                        settings::save();  // remember across restarts
                        std::cout << "Per-bin phase calibration: Disabled" << std::endl;
                    }},
                    {"PERIODIC_RECAL_ENABLE", []() {
                        periodic_recal_enabled.store(true, std::memory_order_release);
                        settings::save();  // remember across restarts
                        std::cout << "Periodic calibration check: Enabled" << std::endl;
                    }},
                    {"PERIODIC_RECAL_DISABLE", []() {
                        periodic_recal_enabled.store(false, std::memory_order_release);
                        settings::save();  // remember across restarts
                        std::cout << "Periodic calibration check: Disabled" << std::endl;
                    }},
                    {"FORCE_RECAL", []() {
                        // Routed through the coherence watchdog so all recalibration
                        // stays serialized on one thread (see coherence_watchdog).
                        force_recalibration.store(true, std::memory_order_release);
                        std::cout << "Force recalibration requested via web UI" << std::endl;
                    }},
                    {"RESET_LAG_COMPENSATION", []() {
                        // "Reset lag" button. Re-measuring lag needs the noise source
                        // on and the lag/phase machines running, and a new lag lock
                        // needs a phase calibration after it - a bare
                        // reset_lag_compensation_all_channels() does neither once
                        // calibration has converged (FFT auto-off idles the machines).
                        // That full sequence is the watchdog's recalibration.
                        force_recalibration.store(true, std::memory_order_release);
                        std::cout << "Lag reset requested via web UI (full recalibration)" << std::endl;
                    }},
                    {"FWD_COMP_ENABLE", []() {
                        forward_comp.enabled.store(true, std::memory_order_release);
                        // Build the correction at the current center frequency so it
                        // takes effect immediately (no recal needed - it is independent
                        // of the noise-source loop).
                        fwdcomp::recompute(static_cast<double>(current_frequency.load()));
                        settings::save();
                        std::cout << "Forward phase compensation: Enabled" << std::endl;
                    }},
                    {"FWD_COMP_DISABLE", []() {
                        forward_comp.enabled.store(false, std::memory_order_release);
                        settings::save();
                        std::cout << "Forward phase compensation: Disabled" << std::endl;
                    }},
                    {"FWD_COMP_AMP_ON", []() {
                        forward_comp.correct_amplitude.store(true, std::memory_order_release);
                        fwdcomp::recompute(static_cast<double>(current_frequency.load()));
                        settings::save();
                        std::cout << "Forward comp: amplitude correction ON" << std::endl;
                    }},
                    {"FWD_COMP_AMP_OFF", []() {
                        forward_comp.correct_amplitude.store(false, std::memory_order_release);
                        fwdcomp::recompute(static_cast<double>(current_frequency.load()));
                        settings::save();
                        std::cout << "Forward comp: amplitude correction OFF (phase-only)" << std::endl;
                    }}
                };
            
                // Check for SDR settings
                if (message.find("SDR_SETTINGS:") == 0) {
                    auto json_part = message.substr(13);
                    uint64_t new_freq = 0;
                    int new_gain = -999;
                
                    // Absent, malformed or non-finite values come back empty (a
                    // malformed one is logged), so a bad field is ignored, not fatal.
                    auto extract_value = [&](std::string_view key) -> std::optional<double> {
                        auto pos = json_part.find(key);
                        if (pos == std::string_view::npos) return std::nullopt;
                        pos += key.length();
                        auto end = json_part.find_first_of(",}", pos);
                        if (end == std::string_view::npos) return std::nullopt;
                        try {
                            const double v = std::stod(std::string(json_part.substr(pos, end - pos)));
                            if (std::isfinite(v)) return v;
                        } catch (const std::exception&) {}
                        std::cerr << "Web: ignoring malformed SDR_SETTINGS " << key << " value" << std::endl;
                        return std::nullopt;
                    };

                    // Bounds only keep the integer casts defined; the tuners
                    // clamp gain to their own table.
                    // Rounded, not truncated: the UI's 64.1 MHz arrives as
                    // 64099999.99999999 and became 64,099,999 Hz, so the next
                    // exact retune to 64.1 MHz counted as a change (full retune +
                    // recal); 49.6 dB * 10 truncated to 495.
                    if (auto freq = extract_value("\"frequency\":"); freq && *freq > 0 && *freq < 1e12) {
                        new_freq = static_cast<uint64_t>(std::llround(*freq));
                    }
                    if (auto gain = extract_value("\"gain\":")) {
                        new_gain = (*gain < 0) ? -1 : static_cast<int>(std::lround(std::min(*gain, 100.0) * 10));
                    }

                    // Wideband scan: the tuners are spread around the center, so a
                    // retune re-spreads them (as the control port's set_frequency
                    // does). update_sdr_settings would tune every tuner to the
                    // same frequency and collapse the scan. A gain change still
                    // goes through update_sdr_settings below.
                    if (new_freq > 0 && operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
                        if (rf_frequency_valid(new_freq)) {
                            std::lock_guard<std::mutex> lock(settings_mutex);
                            current_frequency = new_freq;
                            setup_wideband_frequencies(new_freq, devices);
                        } else {
                            std::cerr << "Web: rejecting wideband scan center " << new_freq / 1e6
                                      << " MHz (outside the tunable range)" << std::endl;
                        }
                        new_freq = 0;
                    }

                    if (new_freq > 0 || new_gain != -999) {
                        if (update_sdr_settings(new_freq, new_gain, devices)) {
                            if (new_freq > 0 && recovery_in_progress.load(std::memory_order_acquire)) {
                                // A coherence recovery is recalibrating: update_sdr_settings
                                // already retuned the hardware, and the recovery will
                                // recalibrate lag+phase at the new frequency. Do NOT run the
                                // cooldown override here - clobbering the recovery's phase
                                // state and killing its noise source would wedge calibration.
                                std::cout << "Frequency changed during coherence recovery: deferring to the full recal" << std::endl;
                            } else if (new_freq > 0) {
                                // Frequency changed - use cooldown approach
                                // (--kerberos: marks the calibration STALE instead)
                                begin_retune_cooldown("Frequency changed via web UI");
                            } else {
                                // Gain-only change - immediate calibration
                                handle_settings_change();
                            }
                        }
                    }
                } 
                // Runtime element-count change: "NUM_ELEMENTS:<n>". Runs on a
                // detached worker - it stops the pipeline and reopens devices
                // (seconds), which must never block the uWS event loop. Progress
                // is visible via the "reconfiguring" flag in the STATE broadcast,
                // and the recalibration that follows via the normal status feed.
                else if (message.find("NUM_ELEMENTS:") == 0) {
                    auto n_str = message.substr(13);
                    try {
                        const int n = std::stoi(std::string(n_str));
                        std::thread([n]() {
                            std::string err;
                            if (!reconfigure_num_elements(n, err)) {
                                std::cerr << "Element-count change to " << n << " failed: " << err << std::endl;
                            }
                        }).detach();
                    } catch (const std::exception&) {
                        std::cerr << "Web: invalid NUM_ELEMENTS: " << n_str << std::endl;
                    }
                }
                // RTL-TCP channel selection
                else if (message.find("RTL_TCP_CHANNEL:") == 0) {
                    auto channel_str = message.substr(16);
                    try {
                        int channel = std::stoi(std::string(channel_str));
                        if (channel >= 0 && channel < active_num_elements.load()) {
                            // The RTL-TCP server streams rtl_tcp_channel directly
                            rtl_tcp_channel = channel;
                            std::cout << "RTL-TCP: Channel changed to " << channel << std::endl;
                        } else {
                            std::cerr << "RTL-TCP: Invalid channel " << channel << " (must be 0-" << (active_num_elements.load() - 1) << ")" << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "RTL-TCP: Invalid channel format: " << channel_str << std::endl;
                    }
                }
                // Per-port antenna bias tees: "ANT_BIAS_MASK:<bitmask>" (bit N = channel N)
                else if (message.find("ANT_BIAS_MASK:") == 0) {
                    auto mask_str = message.substr(14);
                    try {
                        const uint32_t mask = static_cast<uint32_t>(std::stoul(std::string(mask_str)))
                                              & ((1u << NUM_DEVICES) - 1);
                        apply_antenna_bias_tees(mask, devices);
                        settings::save();  // remember across restarts
                    } catch (const std::exception&) {
                        std::cerr << "Bias tees: invalid ANT_BIAS_MASK: " << mask_str << std::endl;
                    }
                }
                // Periodic recalibration check period (minutes)
                else if (message.find("PERIODIC_RECAL_PERIOD:") == 0) {
                    auto minutes_str = message.substr(22);
                    try {
                        int minutes = std::stoi(std::string(minutes_str));
                        minutes = std::max(1, std::min(minutes, 1440));  // clamp 1 min .. 24 h
                        periodic_recal_minutes.store(minutes, std::memory_order_release);
                        settings::save();  // remember across restarts
                        std::cout << "Periodic calibration check: period set to " << minutes << " min" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "Periodic recal: invalid period: " << minutes_str << std::endl;
                    }
                }
                // Forward-comp per-channel file: "FWD_COMP_FILE:<ch>:<filename>"
                // (empty filename clears the channel).
                else if (message.find("FWD_COMP_FILE:") == 0) {
                    auto rest = message.substr(14);
                    auto colon = rest.find(':');
                    if (colon != std::string_view::npos) {
                        try {
                            int ch = std::stoi(std::string(rest.substr(0, colon)));
                            std::string fname(rest.substr(colon + 1));
                            std::string err;
                            bool ok = fwdcomp::set_channel_file(ch, fname, err);
                            if (!ok && !fname.empty())
                                std::cerr << "Forward comp: ch" << ch << " load failed: " << err << std::endl;
                            else
                                std::cout << "Forward comp: ch" << ch << " file = '" << fname << "'" << std::endl;
                            // Re-interpolate at the current frequency and persist.
                            fwdcomp::recompute(static_cast<double>(current_frequency.load()));
                            settings::save();
                        } catch (const std::exception& e) {
                            std::cerr << "Forward comp: bad FWD_COMP_FILE: " << e.what() << std::endl;
                        }
                    }
                }
                else {
                    auto it = handlers.find(message);
                    if (it != handlers.end()) it->second();
                }
            } catch (const std::exception& e) {
                std::cerr << "Web: ignoring malformed command '" << std::string(message).substr(0, 60)
                          << "' (" << e.what() << ")" << std::endl;
            }
        },
        
        .close = [](auto* /*ws*/, int, std::string_view) {
            std::cout << "WebSocket client disconnected" << std::endl;
        }
    // Exclusive port: uWS defaults to SO_REUSEPORT, which lets a second
    // heimdall instance bind the same port silently - the kernel then splits
    // incoming connections randomly between the processes. Fail loudly instead.
    }).listen(WEB_PORT, LIBUS_LISTEN_EXCLUSIVE_PORT, [&](auto* listen_socket) {
        if (listen_socket) {
            std::cout << "Web server listening on http://localhost:" << WEB_PORT << " (uWebSockets)" << std::endl;
            
            auto* uws_loop = uWS::Loop::get();
            struct us_loop_t* native_loop = (struct us_loop_t*)uws_loop;
            auto* timer = us_create_timer(native_loop, 0, 0);
            
            us_timer_set(timer, [](struct us_timer_t* /*timer*/) {
                if (global_app && global_correlation_result && global_fft_control) {
                    try {
                        auto message = build_correlation_message(*global_correlation_result, *global_fft_control);
                        static_cast<uWS::App*>(global_app)->publish("broadcast", message, uWS::BINARY, false);
                        // Re-broadcast the periodic-recal state (settings + drift-fail
                        // counters) about once a second so the UI counters update live.
                        static int state_tick = 0;
                        if (++state_tick >= 20) {
                            state_tick = 0;
                            static_cast<uWS::App*>(global_app)->publish("broadcast", build_state_message(), uWS::TEXT, false);
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error building correlation message: " << e.what() << std::endl;
                    }
                }
            }, 50, 50);
        } else {
            std::cerr << "FATAL: cannot listen on web port " << WEB_PORT
                      << " - is another heimdall instance already running?" << std::endl;
            global_running = false;  // trigger the normal clean shutdown in main()
        }
    });

    global_app = &app;
    app.run();
}