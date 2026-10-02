#include "tcp_control_server.hpp"
#include "rtl_tcp_server.hpp"
#include "../sdr/sdr_init.hpp"
#include "../sdr/pipeline_control.hpp"
#include "../sdr/downconverter.hpp"
#include "../sdr/sdr_pipeline.hpp"
#include "../dsp/compensation.hpp"
#include "../core/config.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>

namespace {

// A control client that stops reading must never stall a heimdall thread.
// The status broadcast is 2 Hz and nothing bounds how long a client can go
// without recv()ing, so output is queued and flushed non-blockingly; a client
// whose backlog passes this cap has a closed receive window and gets dropped.
constexpr size_t MAX_CONTROL_TX_BACKLOG = 256 * 1024;

// Bounded so a reconnect storm cannot exhaust fds, and so no client socket can
// reach select()'s FD_SETSIZE limit.
constexpr size_t MAX_CONTROL_CLIENTS = 16;

// Push as much of the queued output as the socket will take right now. Assumes
// a non-blocking socket: a full send buffer is normal back-pressure (retry when
// select() reports writable), not a connection failure.
void flush_output(TcpClient* client) {
    while (!client->tx_buffer.empty()) {
        ssize_t sent = send(client->socket, client->tx_buffer.data(),
                            client->tx_buffer.size(), MSG_NOSIGNAL);
        if (sent > 0) {
            client->tx_buffer.erase(0, static_cast<size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        client->active = false;  // real socket error - reap on the next pass
        client->tx_buffer.clear();
        return;
    }

    if (client->tx_buffer.size() > MAX_CONTROL_TX_BACKLOG) {
        std::cerr << "TCP Control: client not reading (" << client->tx_buffer.size()
                  << " bytes queued) - dropping" << std::endl;
        client->active = false;
        client->tx_buffer.clear();
    }
}

void queue_output(TcpClient* client, const std::string& data) {
    client->tx_buffer.append(data);
    flush_output(client);
}

} // namespace

TcpControlServer::TcpControlServer() : server_socket(-1) {}

TcpControlServer::~TcpControlServer() {
    stop();
}

bool TcpControlServer::start() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) return false;
    
    if (!set_socket_reuse(server_socket)) {
        close(server_socket);
        return false;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_CONTROL_PORT);
    
    if (bind(server_socket, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(server_socket, 5) < 0) {
        close(server_socket);
        return false;
    }
    
    server_thread = std::thread(&TcpControlServer::control_loop, this);
    std::cout << "TCP Control Server listening on port " << TCP_CONTROL_PORT << std::endl;
    return true;
}

namespace {
// A frequency in Hz from a JSON number: finite, and within the tuners' RF
// range (rf_frequency_valid). The wideband-scan base frequency used to go
// straight through static_cast<uint32_t>(stod(..)) - undefined for negative
// or huge values, and never range-checked.
bool parse_rf_frequency(const std::string& s, uint64_t& out) {
    const double d = std::stod(s);
    if (!std::isfinite(d) || d <= 0 || d >= 1e12) return false;
    out = static_cast<uint64_t>(std::llround(d));
    return rf_frequency_valid(out);
}

std::string rf_range_error(const std::string& given) {
    uint64_t rf_min, rf_max;
    rf_frequency_range(rf_min, rf_max);
    std::string g;
    for (char c : given) if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == 'e') g += c;
    return "{\"status\":\"error\",\"message\":\"Frequency " + g.substr(0, 24) + " out of range (" +
           std::to_string(rf_min / 1000000) + "-" + std::to_string(rf_max / 1000000) + " MHz)\"}";
}
}  // namespace

void TcpControlServer::stop() {
    running = false;
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

// The status object: broadcast at 2 Hz (as_reply = false) and returned by
// {"command":"get_status"} (as_reply = true, which adds "status":"success"
// like every other command reply - clients tell replies from broadcasts by
// that field). No trailing newline.
std::string TcpControlServer::build_status_json(bool as_reply) {
    // Build status JSON including RTL-TCP channel info and wideband mode
    std::stringstream status_json;
    status_json << "{"
               << (as_reply ? "\"status\":\"success\"," : "")
               << "\"settings\":{"
               << "\"center_freq\":" << current_frequency.load() << ","
               << "\"gain\":" << (current_gain.load() == -1 ? -1 : current_gain.load() / 10.0f) << ","
               << "\"sample_rate\":" << SAMPLE_RATE << ","
               << "\"rtl_tcp_channel\":" << rtl_tcp_channel.load()
               << "},"
               << "\"num_channels\":" << active_num_elements.load() << ","
               << "\"max_elements\":" << expected_serials.size() << ","
               << "\"reconfiguring\":" << (reconfig_in_progress.load(std::memory_order_acquire) ? "true" : "false") << ","
               << "\"operating_mode\":\"" << (operating_mode.load() == OperatingMode::WIDEBAND_SCAN ? "wideband" : "coherent") << "\","
               << "\"wideband_enabled\":" << (wideband_config.enabled.load() ? "true" : "false");

    // KerberosSDR support mode: manual-calibration workflow state for client UIs.
    status_json << ",\"external_noise\":" << (external_noise_mode.load() ? "true" : "false");
    if (kerberos_mode.load()) {
        status_json << ",\"kerberos_mode\":true"
                    << ",\"kerberos_sw\":" << (kerberos_sw_mode.load() ? "true" : "false")
                    << ",\"calibration_state\":\"" << kerberos_calibration_state() << "\"";
    }

    // KrakenSDR Wideband (downconverter) variant state. center_freq above is
    // the true RF; this reports the fixed IF, injection side and computed LO.
    if (downconverter.enabled.load()) {
        status_json << ",\"downconverter\":{"
                    << "\"enabled\":true,"
                    << "\"if_hz\":" << WB_VARIANT_IF_HZ << ","
                    << "\"side\":\"" << mixer_side_name(downconverter.side.load()) << "\","
                    << "\"lo_hz\":" << downconverter.lo_hz.load() << ","
                    << "\"array\":" << downconverter.array_select.load() << ","
                    << "\"lo_current\":" << downconverter.lo_current.load()
                    << "}";
    }

    // Add per-tuner frequencies if in wideband mode
    int num_active = active_num_elements.load();
    if (wideband_config.enabled.load()) {
        status_json << ",\"tuner_frequencies\":[";
        for (int i = 0; i < num_active; i++) {
            if (i > 0) status_json << ",";
            status_json << wideband_config.get_tuner_frequency(i);
        }
        status_json << "]";
    }

    // Add cooldown status for drag-to-scroll feature
    if (phase_compensation) {
        bool cooldown = phase_compensation->cooldown_active.load();
        int remaining_ms = 0;
        if (cooldown) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - phase_compensation->last_frequency_change).count();
            remaining_ms = std::max(0, phase_compensation->stability_delay_override_ms.load() - static_cast<int>(elapsed));
        }
        status_json << ",\"cooldown_active\":" << (cooldown ? "true" : "false")
                   << ",\"cooldown_remaining_ms\":" << remaining_ms;

        // Applied per-channel calibration (the live closed-loop compensation
        // vector from the noise-source cal): gain in dB and phase in degrees.
        // Identity (0 dB / 0°) until the first apply; the reference channel is
        // always identity. Lock-free reads, cold path (2 Hz broadcast). Lets
        // clients/operators see the measured tuner gain mismatch directly.
        status_json << ",\"channel_comp\":[";
        for (int i = 0; i < num_active; i++) {
            const Complex c = phase_compensation->compensation_vector.load(i);
            const float amp_db = 20.0f * std::log10(std::max(std::abs(c), 1e-6f));
            const float phase_deg = std::arg(c) * 180.0f / static_cast<float>(M_PI);
            if (i > 0) status_json << ",";
            status_json << "{\"amp_db\":" << amp_db << ",\"phase_deg\":" << phase_deg << "}";
        }
        status_json << "]";
    }

    // Coherence health: a running tally of detected desync events and whether a
    // recovery (flush + recalibration) is in progress, so the UI/operator can see
    // that coherence was lost and is being restored.
    status_json << ",\"coherence_events\":" << coherence_event_count.load()
               << ",\"recovering\":" << (recovery_in_progress.load() ? "true" : "false");

    status_json << "}";
    return status_json.str();
}

void TcpControlServer::broadcast_status() {
    std::lock_guard<std::mutex> lock(clients_mutex);

    // Remove inactive clients
    clients.erase(std::remove_if(clients.begin(), clients.end(),
        [](const auto& client) { return !client->active; }), clients.end());

    if (clients.empty()) return;

    const std::string status_str = build_status_json(false) + "\n";

    // Send to all connected clients. Queued, never blocking: this runs on the
    // status broadcaster thread while holding clients_mutex, so a blocking
    // send() here would freeze the control loop (no accepts, no commands) and
    // every other clients_mutex user for as long as the client stayed stuck.
    for (auto& client : clients) {
        if (client->active) {
            queue_output(client.get(), status_str);
        }
    }
}

void TcpControlServer::control_loop() {
    while (running) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(server_socket, &read_fds);

        int max_fd = server_socket;

        // Add existing clients to read set
        {
            std::lock_guard<std::mutex> lock(clients_mutex);

            // Reap clients that went inactive since the last pass - the
            // TcpClient destructor closes the fd. Pruning used to happen only
            // in broadcast_status(), so a stalled broadcaster leaked one fd per
            // reconnect until accept() started failing.
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                [](const auto& client) { return !client->active; }), clients.end());

            for (const auto& client : clients) {
                if (client->active) {
                    FD_SET(client->socket, &read_fds);
                    // Only watch for writability while output is pending -
                    // an idle socket is writable every pass and would spin.
                    if (!client->tx_buffer.empty()) {
                        FD_SET(client->socket, &write_fds);
                    }
                    max_fd = std::max(max_fd, client->socket);
                }
            }
        }

        timeval timeout{0, 100000}; // 100ms timeout
        int result = select(max_fd + 1, &read_fds, &write_fds, nullptr, &timeout);

        if (result > 0) {
            // Check for new connections
            if (FD_ISSET(server_socket, &read_fds)) {
                sockaddr_in client_addr{};
                socklen_t addr_len = sizeof(client_addr);
                int client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
                
                if (client_socket >= 0) {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    // select() cannot watch an fd at or beyond FD_SETSIZE, and
                    // FD_SET on one corrupts memory - refuse rather than risk it.
                    if (client_socket >= FD_SETSIZE || clients.size() >= MAX_CONTROL_CLIENTS) {
                        std::cerr << "TCP Control: refusing connection (fd " << client_socket
                                  << ", " << clients.size() << " clients)" << std::endl;
                        close(client_socket);
                    } else {
                        // Non-blocking: every write to this socket goes through
                        // flush_output(), which treats EAGAIN as back-pressure.
                        set_socket_nonblocking(client_socket);
                        clients.push_back(std::make_unique<TcpClient>(client_socket));
                        std::cout << "TCP Control: Client connected from " << inet_ntoa(client_addr.sin_addr) << std::endl;
                    }
                }
            }

            // Check existing clients for data
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto& client : clients) {
                if (client->active && FD_ISSET(client->socket, &write_fds)) {
                    flush_output(client.get());
                }
                if (client->active && FD_ISSET(client->socket, &read_fds)) {
                    handle_client_data(client.get());
                }
            }
        }
    }
}

void TcpControlServer::handle_client_data(TcpClient* client) {
    char buffer[1024];
    ssize_t bytes_read = recv(client->socket, buffer, sizeof(buffer), 0);

    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;  // spurious readability on the non-blocking socket, not a drop
    }
    if (bytes_read <= 0) {
        client->active = false;
        return;
    }

    client->rx_buffer.append(buffer, static_cast<size_t>(bytes_read));

    // Extract every complete top-level JSON object from the stream and
    // process each one. The old line-splitting parser lost commands two
    // ways: several commands coalesced into one TCP segment (the DoA
    // client's startup settings replay - Nagle merges its back-to-back
    // sends) were handed to process_command() as ONE string, which matched
    // only the first command it searched for; and a command split across
    // recv() calls was parsed as two broken fragments. Quote-aware brace
    // matching so braces inside string values don't confuse the scan.
    size_t start = 0;
    while (true) {
        // Skip inter-command whitespace / newline framing
        while (start < client->rx_buffer.size() &&
               (client->rx_buffer[start] == '\n' || client->rx_buffer[start] == '\r' ||
                client->rx_buffer[start] == ' '  || client->rx_buffer[start] == '\t')) {
            start++;
        }
        if (start >= client->rx_buffer.size()) break;

        if (client->rx_buffer[start] != '{') {
            // Garbage before any object: drop up to the next newline
            size_t nl = client->rx_buffer.find('\n', start);
            if (nl == std::string::npos) break;  // wait for more data
            // ...but an HTTP request is a web page (fetch/form POST to this
            // port): its header lines would be dropped and a JSON body run as
            // a command - a cross-protocol attack around 8070's origin and
            // host checks. Real clients send only JSON: hang up.
            if (client->rx_buffer.substr(start, nl - start).find("HTTP/") != std::string::npos) {
                static auto last_log = std::chrono::steady_clock::time_point{};
                const auto now = std::chrono::steady_clock::now();
                if (now - last_log > std::chrono::seconds(10)) {  // a page may retry in a loop
                    last_log = now;
                    std::cerr << "Control port: HTTP request refused (web pages may not send commands)" << std::endl;
                }
                client->rx_buffer.clear();
                client->active = false;
                return;
            }
            start = nl + 1;
            continue;
        }

        int depth = 0;
        bool in_string = false, escaped = false;
        size_t end = std::string::npos;
        for (size_t i = start; i < client->rx_buffer.size(); i++) {
            const char c = client->rx_buffer[i];
            if (escaped)          { escaped = false; continue; }
            if (c == '\\')        { escaped = in_string; continue; }
            if (c == '"')         { in_string = !in_string; continue; }
            if (in_string)        continue;
            if (c == '{')         depth++;
            else if (c == '}' && --depth == 0) { end = i; break; }
        }
        if (end == std::string::npos) break;  // incomplete - wait for more data

        std::string command = client->rx_buffer.substr(start, end - start + 1);
        start = end + 1;

        try {
            auto response = process_command(command);
            queue_output(client, response + "\n");
        } catch (const std::exception& e) {
            queue_output(client,
                         "{\"status\":\"error\",\"message\":\"" + std::string(e.what()) + "\"}\n");
        }
    }

    client->rx_buffer.erase(0, start);
    // A sender that never completes an object must not grow the buffer forever
    if (client->rx_buffer.size() > 65536) client->rx_buffer.clear();
}

std::string TcpControlServer::process_command(const std::string& json_str) {
    // While an element-count reconfiguration owns the devices, refuse every
    // command except get_status: most handlers touch device handles that are
    // being closed/reopened on another thread.
    if (reconfig_in_progress.load(std::memory_order_acquire) &&
        json_str.find("\"get_status\"") == std::string::npos) {
        return "{\"status\":\"error\",\"message\":\"Element-count reconfiguration in progress - retry shortly\"}";
    }

    // Current status on demand (same object the 2 Hz broadcast carries).
    // Documented in the READMEs/CLAUDE.md but used to fall through to
    // "Unknown command".
    if (json_str.find("\"get_status\"") != std::string::npos) {
        return build_status_json(true);
    }

    // Runtime element-count change: {"command":"set_num_elements","num_elements":N}
    // Stops the pipeline, closes and reopens devices, restarts, then runs a
    // full recalibration. Runs on a detached worker (takes seconds); watch the
    // "reconfiguring" status field and phase_state for completion.
    if (json_str.find("\"set_num_elements\"") != std::string::npos) {
        size_t pos = json_str.find("\"num_elements\":");
        if (pos == std::string::npos) {
            return "{\"status\":\"error\",\"message\":\"Missing num_elements field\"}";
        }
        pos += 15;
        size_t end_pos = json_str.find_first_of(",}", pos);
        int n;
        try {
            n = std::stoi(json_str.substr(pos, end_pos - pos));
        } catch (...) {
            return "{\"status\":\"error\",\"message\":\"Invalid num_elements value\"}";
        }
        const int max_n = static_cast<int>(expected_serials.size());
        if (n < 2 || n > max_n) {
            return "{\"status\":\"error\",\"message\":\"num_elements must be 2-" + std::to_string(max_n) + "\"}";
        }
        // (Unless the pipeline is down after a failed reconfiguration: then
        // the same count is a retry that reopens the devices.)
        if (n == active_num_elements.load() && pipeline_running.load(std::memory_order_acquire)) {
            remember_num_elements(n);  // still an explicit choice: save it
            return "{\"status\":\"success\",\"num_elements\":" + std::to_string(n) + ",\"message\":\"already active\"}";
        }
        if (std::string err; !reconfigure_num_elements_precheck(n, err)) {
            return "{\"status\":\"error\",\"message\":\"" + err + "\"}";
        }
        std::thread([n]() {
            std::string err;
            if (!reconfigure_num_elements(n, err)) {
                std::cerr << "Element-count change to " << n << " failed: " << err << std::endl;
            }
        }).detach();
        return "{\"status\":\"success\",\"num_elements\":" + std::to_string(n) +
               ",\"message\":\"reconfiguration started (devices reopen + full recalibration)\"}";
    }

    // Simple JSON parsing for command processing
    if (json_str.find("\"set_frequency\"") != std::string::npos) {
        size_t freq_pos = json_str.find("\"frequency\":");
        if (freq_pos != std::string::npos) {
            freq_pos += 12; // Skip "frequency":
            size_t end_pos = json_str.find_first_of(",}", freq_pos);
            if (end_pos != std::string::npos) {
                std::string freq_str = json_str.substr(freq_pos, end_pos - freq_pos);
                // Range-check the double before the cast: a negative or huge
                // value would make the conversion undefined.
                const double freq_d = std::stod(freq_str);
                const uint64_t frequency = (freq_d > 0 && freq_d < 1e12) ? static_cast<uint64_t>(std::llround(freq_d)) : 0;  // rounded, not truncated

                // Valid RF range: tuner limits normally; in the wideband
                // variant the union of all three injection sides' spans -
                // the retune auto-switches side (and antenna ring) as needed.
                uint64_t rf_min, rf_max;
                rf_frequency_range(rf_min, rf_max);

                if (frequency >= rf_min && frequency <= rf_max) {
                    // In wideband mode, SKIP update_sdr_settings (which sets all tuners to same freq)
                    // and ONLY call setup_wideband_frequencies (which sets each tuner to its spread freq)
                    // This prevents a race condition where packets briefly show all tuners at same frequency
                    if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
                        std::lock_guard<std::mutex> lock(settings_mutex);
                        // Update base frequency tracking
                        current_frequency = frequency;
                        // Directly set spread frequencies - no cooldown needed in wideband mode
                        setup_wideband_frequencies(frequency, devices);
                        std::cout << "Wideband frequency changed to " << (frequency / 1e6) << " MHz" << std::endl;
                        return "{\"status\":\"success\",\"frequency\":" + std::to_string(frequency) + "}";
                    }

                    // Coherent mode: update all tuners to same frequency
                    bool changed = update_sdr_settings(frequency, -999, devices);
                    // Skip the cooldown override during a coherence recovery: the
                    // hardware is already retuned and the recovery recalibrates
                    // lag+phase at the new frequency; clobbering its state / killing
                    // the noise source here would wedge phase calibration.
                    // (begin_retune_cooldown restarts a running recovery's phase
                    // measurement instead - see recovery_settings_changed)
                    if (changed) {
                        // Coherent mode: start the cooldown instead of an immediate
                        // calibration, so rapid frequency scrolling doesn't trigger one
                        // (--kerberos: marks the calibration STALE instead)
                        begin_retune_cooldown("Frequency changed");
                    }
                    return "{\"status\":\"success\",\"frequency\":" + std::to_string(frequency) + "}";
                } else {
                    return "{\"status\":\"error\",\"message\":\"Frequency out of range (" +
                           std::to_string(rf_min / 1000000) + "-" +
                           std::to_string(rf_max / 1000000) + " MHz)\"}";
                }
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid frequency format\"}";
    }

    // Wideband variant: manual mixer-side override. High (LO = IF + RF)
    // covers 24-4132 MHz with an inverted spectrum (corrected in software);
    // low (LO = IF - RF) is upright but caps the RF below the IF; below
    // (LO = RF - IF) is upright and the only side past 4132 MHz (up to
    // 6668 MHz). The side normally follows the frequency automatically
    // (wideband_retune_rf); this override only accepts sides that can reach
    // the CURRENT RF - the client UI greys the others out.
    if (json_str.find("\"set_mixer_side\"") != std::string::npos) {
        if (!downconverter.enabled.load()) {
            return "{\"status\":\"error\",\"message\":\"Not in wideband variant mode (start with --wideband)\"}";
        }

        MixerSide side;
        if (json_str.find("\"side\":\"high\"") != std::string::npos) {
            side = MixerSide::HIGH;
        } else if (json_str.find("\"side\":\"low\"") != std::string::npos) {
            side = MixerSide::LOW;
        } else if (json_str.find("\"side\":\"below\"") != std::string::npos) {
            side = MixerSide::BELOW;
        } else {
            return "{\"status\":\"error\",\"message\":\"side must be \\\"high\\\", \\\"low\\\" or \\\"below\\\"\"}";
        }

        std::string side_json = std::string("\"side\":\"") + mixer_side_name(side) + "\"";

        // RF read, checks and LO programming all under settings_mutex (every
        // retune path holds it): read before the lock, a retune landing in
        // between left the LO programmed for the previous frequency.
        uint64_t rf;
        {
            std::lock_guard<std::mutex> lock(settings_mutex);
            rf = current_frequency.load();

            if (side == downconverter.side.load()) {
                return "{\"status\":\"success\"," + side_json +
                       ",\"lo_hz\":" + std::to_string(downconverter.lo_hz.load()) + "}";
            }

            if (!downconverter_rf_valid(rf, side)) {
                uint64_t min_hz, max_hz;
                downconverter_rf_range(side, min_hz, max_hz);
                return "{\"status\":\"error\",\"message\":\"" +
                       std::string(mixer_side_name(side)) + " side cannot reach RF " +
                       std::to_string(rf / 1000000) + " MHz (covers " +
                       std::to_string(min_hz / 1000000) + "-" + std::to_string(max_hz / 1000000) +
                       " MHz)\"}";
            }

            if (!downconverter_set_side(side, rf)) {
                return "{\"status\":\"error\",\"message\":\"Failed to program downconverter LO\"}";
            }
        }
        std::cout << "Downconverter: mixer side set to " << mixer_side_name(side)
                  << " at RF " << rf / 1e6 << " MHz (spectral inversion "
                  << (side == MixerSide::HIGH ? "corrected in software" : "off")
                  << ")" << std::endl;

        // The LO moved AND the spectral orientation flipped, so the existing
        // phase calibration is invalid. Run the same cooldown -> phase-recal
        // flow a frequency change uses (skip during a coherence recovery,
        // which will recalibrate at the current settings anyway).
        begin_retune_cooldown("Mixer side changed");  // (restarts a running recovery's phase stage)

        return "{\"status\":\"success\"," + side_json +
               ",\"frequency\":" + std::to_string(rf) +
               ",\"lo_hz\":" + std::to_string(downconverter.lo_hz.load()) + "}";
    }

    // Wideband variant: select the antenna ring (0 = outer, 1 = center,
    // 2 = inner). Throws the on-board RF switches on the channel-0 chip.
    if (json_str.find("\"set_array\"") != std::string::npos) {
        if (!downconverter.enabled.load()) {
            return "{\"status\":\"error\",\"message\":\"Not in wideband variant mode (start with --wideband)\"}";
        }

        size_t pos = json_str.find("\"array\":");
        if (pos == std::string::npos) {
            return "{\"status\":\"error\",\"message\":\"Missing array field (0=outer, 1=center, 2=inner)\"}";
        }
        pos += 8;
        size_t end_pos = json_str.find_first_of(",}", pos);
        int array;
        try {
            array = std::stoi(json_str.substr(pos, end_pos - pos));
        } catch (...) {
            return "{\"status\":\"error\",\"message\":\"Invalid array value\"}";
        }
        if (array < 0 || array > 2) {
            return "{\"status\":\"error\",\"message\":\"array must be 0 (outer), 1 (center) or 2 (inner)\"}";
        }

        if (array == downconverter.array_select.load()) {
            return "{\"status\":\"success\",\"array\":" + std::to_string(array) + "}";
        }

        downconverter.array_select.store(array);
        // Re-throw the switches for the new ring on whichever path (noise /
        // antennas) is currently routed.
        wideband_set_noise_path(bias_tee_enabled.load(), devices);
        std::cout << "Wideband: antenna array set to " << array
                  << " (0=outer, 1=center, 2=inner)" << std::endl;

        // A different ring means different cable/switch path lengths, so the
        // existing phase calibration is invalid. Run the same cooldown ->
        // phase-recal flow a frequency change uses (skip during a coherence
        // recovery, which recalibrates at current settings anyway).
        begin_retune_cooldown("Array changed");  // (restarts a running recovery's phase stage)

        return "{\"status\":\"success\",\"array\":" + std::to_string(array) + "}";
    }

    // Wideband variant: LO synthesizer output drive current (0-7). The LO is
    // shared by all mixers, so a drive change is common-mode across channels
    // and does not invalidate the phase calibration - no recal is triggered.
    // Diagnostic: fix the R820T IF VGA step on all tuners,
    // {"command":"set_if_vga","index":0-15}. Not persisted; the next gain
    // change re-applies the configured step (R820T_IF_VGA_* in config.h).
    if (json_str.find("\"set_if_vga\"") != std::string::npos) {
        size_t pos = json_str.find("\"index\":");
        if (pos == std::string::npos) {
            return "{\"status\":\"error\",\"message\":\"Missing index field (0-15)\"}";
        }
        pos += 8;
        int index;
        try {
            index = std::stoi(json_str.substr(pos, json_str.find_first_of(",}", pos) - pos));
        } catch (...) {
            return "{\"status\":\"error\",\"message\":\"Invalid index value\"}";
        }
        if (index < 0 || index > 15) {
            return "{\"status\":\"error\",\"message\":\"index must be 0-15\"}";
        }
        if (!set_if_vga_all(index, devices)) {
            return "{\"status\":\"error\",\"message\":\"Failed to set the IF VGA on some tuners\"}";
        }
        return "{\"status\":\"success\",\"if_vga_index\":" + std::to_string(index) + "}";
    }

    if (json_str.find("\"set_lo_current\"") != std::string::npos) {
        if (!downconverter.enabled.load()) {
            return "{\"status\":\"error\",\"message\":\"Not in wideband variant mode (start with --wideband)\"}";
        }

        size_t pos = json_str.find("\"current\":");
        if (pos == std::string::npos) {
            return "{\"status\":\"error\",\"message\":\"Missing current field (0-7)\"}";
        }
        pos += 10;
        size_t end_pos = json_str.find_first_of(",}", pos);
        int current;
        try {
            current = std::stoi(json_str.substr(pos, end_pos - pos));
        } catch (...) {
            return "{\"status\":\"error\",\"message\":\"Invalid current value\"}";
        }
        if (current < 0 || current > 7) {
            return "{\"status\":\"error\",\"message\":\"current must be 0-7\"}";
        }

        if (!downconverter_set_current(current)) {
            return "{\"status\":\"error\",\"message\":\"Failed to program LO current\"}";
        }
        return "{\"status\":\"success\",\"lo_current\":" + std::to_string(current) + "}";
    }

    if (json_str.find("\"set_stability_delay\"") != std::string::npos) {
        size_t pos = json_str.find("\"delay_ms\":");
        if (pos != std::string::npos) {
            pos += 11;
            size_t end_pos = json_str.find_first_of(",}", pos);
            if (end_pos != std::string::npos) {
                int delay_ms = std::stoi(json_str.substr(pos, end_pos - pos));
                if (delay_ms >= 100 && delay_ms <= 10000) {
                    if (phase_compensation) {
                        phase_compensation->stability_delay_override_ms = delay_ms;
                        std::cout << "Stability delay set to " << delay_ms << "ms" << std::endl;
                    }
                    return "{\"status\":\"success\",\"stability_delay_ms\":" + std::to_string(delay_ms) + "}";
                }
                return "{\"status\":\"error\",\"message\":\"Delay out of range (100-10000 ms)\"}";
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid delay format\"}";
    }

    if (json_str.find("\"set_gain\"") != std::string::npos) {
        size_t gain_pos = json_str.find("\"gain\":");
        if (gain_pos != std::string::npos) {
            gain_pos += 7; // Skip "gain":
            size_t end_pos = json_str.find_first_of(",}", gain_pos);
            if (end_pos != std::string::npos) {
                std::string gain_str = json_str.substr(gain_pos, end_pos - gain_pos);
                const double gain_db = std::stod(gain_str);
                if (!std::isfinite(gain_db) || gain_db > 1000.0) {
                    return "{\"status\":\"error\",\"message\":\"Gain out of range (0-50 dB or auto)\"}";
                }
                // Round, don't truncate: 49.6 * 10 is 495.99.. in binary, so
                // the cast stored 495 (49.5 dB) instead of the tuner's 496.
                // Negative = auto.
                int gain = (gain_db < 0) ? -1 : static_cast<int>(std::lround(gain_db * 10.0));

                if (gain == -1 || (gain >= 0 && gain <= 500)) {
                    bool changed = update_sdr_settings(0, gain, devices);
                    if (changed) {
                        // In wideband mode, skip cooldown - phase calibration is disabled anyway
                        if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
                            std::cout << "Wideband gain changed to " << gain_db << " dB" << std::endl;
                        } else {
                            // (during a recovery this restarts its phase stage instead)
                            // Coherent mode: start the cooldown instead of an immediate
                            // calibration, so rapid gain adjustments don't trigger one
                            begin_retune_cooldown("Gain changed");
                        }
                    }
                    std::ostringstream applied;
                    applied << (gain < 0 ? -1.0 : gain / 10.0);
                    return "{\"status\":\"success\",\"gain\":" + applied.str() + "}";
                } else {
                    return "{\"status\":\"error\",\"message\":\"Gain out of range (0-50 dB or auto)\"}";
                }
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid gain format\"}";
    }
    
    // RTL-TCP channel selection
    if (json_str.find("\"set_rtl_tcp_channel\"") != std::string::npos) {
        size_t channel_pos = json_str.find("\"channel\":");
        if (channel_pos != std::string::npos) {
            channel_pos += 10; // Skip "channel":
            size_t end_pos = json_str.find_first_of(",}", channel_pos);
            if (end_pos != std::string::npos) {
                std::string channel_str = json_str.substr(channel_pos, end_pos - channel_pos);
                int channel = std::stoi(channel_str);
                
                if (channel >= 0 && channel < active_num_elements.load()) {
                    // The server streams rtl_tcp_channel; its setter logs the change
                    if (rtl_tcp_server_ref) rtl_tcp_server_ref->set_source_channel(channel);
                    else rtl_tcp_channel = channel;
                    return "{\"status\":\"success\",\"rtl_tcp_channel\":" + std::to_string(channel) + "}";
                } else {
                    return "{\"status\":\"error\",\"message\":\"Channel out of range (0-" + std::to_string(active_num_elements.load() - 1) + ")\"}";
                }
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid channel format\"}";
    }

    // Wideband mode control
    if (json_str.find("\"set_wideband_mode\"") != std::string::npos) {
        size_t enable_pos = json_str.find("\"enable\":");
        if (enable_pos != std::string::npos) {
            enable_pos += 9; // Skip "enable":
            size_t end_pos = json_str.find_first_of(",}", enable_pos);
            if (end_pos != std::string::npos) {
                std::string enable_str = json_str.substr(enable_pos, end_pos - enable_pos);
                // Remove whitespace and quotes
                enable_str.erase(std::remove_if(enable_str.begin(), enable_str.end(), ::isspace), enable_str.end());
                bool enable = (enable_str.find("true") != std::string::npos);

                // Validate the optional base_frequency BEFORE switching mode,
                // so a bad value is refused without entering the scan.
                bool have_base = false;
                uint64_t base_freq = 0;
                if (size_t freq_pos = json_str.find("\"base_frequency\":"); enable && freq_pos != std::string::npos) {
                    freq_pos += 17; // Skip "base_frequency":
                    const std::string freq_str = json_str.substr(freq_pos, json_str.find_first_of(",}", freq_pos) - freq_pos);
                    if (!parse_rf_frequency(freq_str, base_freq)) return rf_range_error(freq_str);
                    have_base = true;
                }

                bool changed = set_wideband_mode(enable, devices);

                // CRITICAL FIX: Always apply base_frequency if provided, even if mode didn't change
                // This is essential for the discrete scanner which needs to retune while already in wideband mode
                if (enable && operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
                    if (have_base) {
                        setup_wideband_frequencies(base_freq, devices);
                        std::cout << "Wideband: Applied base_frequency " << base_freq/1e6 << " MHz" << std::endl;
                    } else if (changed) {
                        // Only use current frequency as default if mode just changed
                        setup_wideband_frequencies(current_frequency.load(), devices);
                    }
                }

                if (changed) {
                    return "{\"status\":\"success\",\"wideband_enabled\":" + std::string(enable ? "true" : "false") + "}";
                } else if (enable != (operating_mode.load() == OperatingMode::WIDEBAND_SCAN)) {
                    // Not already there: refused (Wideband variant, scanner, ...)
                    return "{\"status\":\"error\",\"message\":\"Wideband scan mode change refused\"}";
                } else {
                    return "{\"status\":\"success\",\"message\":\"Already in requested mode\"}";
                }
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid enable format\"}";
    }

    // Set wideband frequencies
    if (json_str.find("\"set_wideband_frequencies\"") != std::string::npos) {
        size_t freq_pos = json_str.find("\"base_frequency\":");
        if (freq_pos != std::string::npos) {
            freq_pos += 17; // Skip "base_frequency":
            size_t end_pos = json_str.find_first_of(",}", freq_pos);
            if (end_pos != std::string::npos) {
                std::string freq_str = json_str.substr(freq_pos, end_pos - freq_pos);
                uint64_t base_freq = 0;
                if (!parse_rf_frequency(freq_str, base_freq)) {
                    return rf_range_error(freq_str);
                }

                if (operating_mode.load() != OperatingMode::WIDEBAND_SCAN) {
                    return "{\"status\":\"error\",\"message\":\"Not in wideband scan mode\"}";
                }

                setup_wideband_frequencies(base_freq, devices);
                return "{\"status\":\"success\",\"base_frequency\":" + std::to_string(base_freq) + "}";
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid frequency format\"}";
    }

    // Set wideband edge clip
    if (json_str.find("\"set_wideband_edge_clip\"") != std::string::npos) {
        size_t clip_pos = json_str.find("\"edge_clip\":");
        if (clip_pos != std::string::npos) {
            clip_pos += 12; // Skip "edge_clip":
            size_t end_pos = json_str.find_first_of(",}", clip_pos);
            if (end_pos != std::string::npos) {
                std::string clip_str = json_str.substr(clip_pos, end_pos - clip_pos);
                float edge_clip = std::stof(clip_str);
                // NaN passes both clamps below (every comparison is false)
                if (!std::isfinite(edge_clip)) {
                    return "{\"status\":\"error\",\"message\":\"Invalid edge_clip value\"}";
                }

                // Clamp to valid range
                if (edge_clip < 0.1f) edge_clip = 0.1f;
                if (edge_clip > 1.0f) edge_clip = 1.0f;

                float old_clip = wideband_config.edge_clip.exchange(edge_clip);

                std::cout << "Wideband edge clip changed: " << (old_clip * 100.0f)
                          << "% -> " << (edge_clip * 100.0f) << "%" << std::endl;

                // If in wideband mode, reconfigure tuner spacing with new edge clip
                if (operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
                    setup_wideband_frequencies(current_frequency.load(), devices);
                }

                return "{\"status\":\"success\",\"edge_clip\":" + std::to_string(edge_clip) + "}";
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid edge_clip format\"}";
    }

    // Get wideband status
    if (json_str.find("\"get_wideband_status\"") != std::string::npos) {
        std::stringstream response;
        response << "{\"status\":\"success\","
                 << "\"operating_mode\":\"" << (operating_mode.load() == OperatingMode::WIDEBAND_SCAN ? "wideband" : "coherent") << "\","
                 << "\"wideband_enabled\":" << (wideband_config.enabled.load() ? "true" : "false");

        if (wideband_config.enabled.load()) {
            const int num_tuners = active_num_elements.load();
            response << ",\"tuner_frequencies\":[";
            for (int i = 0; i < num_tuners; i++) {
                if (i > 0) response << ",";
                response << wideband_config.get_tuner_frequency(i);
            }
            response << "]";

            // Calculate total coverage using current edge clip
            const float edge_clip = wideband_config.edge_clip.load();
            const uint32_t usable_bandwidth = static_cast<uint32_t>(SAMPLE_RATE * edge_clip);
            response << ",\"usable_bandwidth_per_tuner\":" << usable_bandwidth
                     << ",\"total_coverage\":" << (num_tuners * usable_bandwidth)
                     << ",\"edge_clip\":" << edge_clip;
        }

        response << "}";
        return response.str();
    }

    // Reset lag compensation
    // Same as the web "Reset lag" button: re-measuring lag needs the noise
    // source on and the machines running, and a new lag lock needs a phase
    // calibration after it. A bare reset_lag_compensation_all_channels() did
    // neither once calibrated (FFT auto-off idles the machines): the sub-sample
    // trim just vanished, and mid-VERIFYING it could latch a false CONVERGED.
    if (json_str.find("\"reset_lag_compensation\"") != std::string::npos) {
        force_recalibration.store(true, std::memory_order_release);
        std::cout << "Lag reset requested via control port (full recalibration)" << std::endl;
        return "{\"status\":\"success\",\"message\":\"Full recalibration (lag + phase) started\"}";
    }

    // Configure discrete scanner
    if (json_str.find("\"configure_scanner\"") != std::string::npos) {
        // Parse frequency groups array
        size_t freq_array_pos = json_str.find("\"frequency_groups\":");
        size_t dwell_pos = json_str.find("\"dwell_time_ms\":");

        if (freq_array_pos != std::string::npos && dwell_pos != std::string::npos) {
            // Parse dwell time
            dwell_pos += 16; // Skip "dwell_time_ms":
            size_t dwell_end = json_str.find_first_of(",}", dwell_pos);
            if (dwell_end != std::string::npos) {
                std::string dwell_str = json_str.substr(dwell_pos, dwell_end - dwell_pos);
                // stoul wraps "-5" to ~4e9 (and the uint32 cast truncated
                // larger values) - parse signed and bound it instead.
                const long long dwell_ll = std::stoll(dwell_str);
                constexpr long long MAX_DWELL_TIME_MS = 3600000;  // 1 hour
                if (dwell_ll < 0 || dwell_ll > MAX_DWELL_TIME_MS) {
                    return "{\"status\":\"error\",\"message\":\"dwell_time_ms must be 0-3600000\"}";
                }
                uint32_t dwell_time = static_cast<uint32_t>(dwell_ll);

                // Enforce minimum dwell time of 500ms (settling time + processing overhead)
                constexpr uint32_t MIN_DWELL_TIME_MS = 500;
                if (dwell_time < MIN_DWELL_TIME_MS) {
                    std::cout << "Scanner: Requested dwell time " << dwell_time
                              << "ms is below minimum, clamping to " << MIN_DWELL_TIME_MS << "ms" << std::endl;
                    dwell_time = MIN_DWELL_TIME_MS;
                }

                // Parse frequency array
                size_t array_start = json_str.find("[", freq_array_pos);
                size_t array_end = json_str.find("]", array_start);
                if (array_start != std::string::npos && array_end != std::string::npos) {
                    std::string array_str = json_str.substr(array_start + 1, array_end - array_start - 1);

                    std::vector<uint64_t> frequencies;
                    std::stringstream ss(array_str);
                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
                        if (!token.empty()) {
                            // stoull wraps "-5" to a huge value, which the range check rejects
                            const uint64_t f = std::stoull(token);
                            if (!rf_frequency_valid(f)) {
                                uint64_t rf_min, rf_max;
                                rf_frequency_range(rf_min, rf_max);
                                return "{\"status\":\"error\",\"message\":\"Frequency " + std::to_string(f) +
                                       " out of range (" + std::to_string(rf_min / 1000000) + "-" +
                                       std::to_string(rf_max / 1000000) + " MHz)\"}";
                            }
                            frequencies.push_back(f);
                        }
                    }

                    if (frequencies.empty()) {
                        return "{\"status\":\"error\",\"message\":\"No valid frequencies provided\"}";
                    }

                    // Configure the scanner: list + dwell installed BEFORE the
                    // generation bump, so a scan thread that sees the new
                    // generation also sees the new list; then the bump and the
                    // index reset together (see the scan thread's index
                    // advance): restart at group 0 now
                    discrete_scanner.set_frequency_groups(frequencies);
                    discrete_scanner.dwell_time_ms = dwell_time;
                    {
                        std::lock_guard<std::mutex> cl(discrete_scanner.config_mutex);
                        discrete_scanner.run_generation.fetch_add(1);
                        discrete_scanner.current_group_index = 0;
                    }

                    std::cout << "Scanner configured: " << frequencies.size() << " groups, "
                             << dwell_time << " ms dwell" << std::endl;

                    return "{\"status\":\"success\",\"num_groups\":" + std::to_string(frequencies.size())
                           + ",\"dwell_time_ms\":" + std::to_string(dwell_time) + "}";
                }
            }
        }
        return "{\"status\":\"error\",\"message\":\"Invalid scanner configuration format\"}";
    }

    // Start discrete scanner
    if (json_str.find("\"start_scanner\"") != std::string::npos) {
        if (discrete_scanner.get_num_groups() == 0) {
            return "{\"status\":\"error\",\"message\":\"Scanner not configured. Use configure_scanner first.\"}";
        }

        // New run: generation first (ends a previous run's dwell; see the
        // scan thread's index advance), then the index - both before enabled,
        // which the scan thread acts on
        {
            std::lock_guard<std::mutex> cl(discrete_scanner.config_mutex);
            discrete_scanner.run_generation.fetch_add(1);
            discrete_scanner.current_group_index = 0;
        }
        discrete_scanner.frequency_change_counter = 0;

        // A calibration in flight (noise source on, phase machine not
        // CONVERGED: startup, recovery, retune recal) can't converge across
        // hops and would inject the noise into the scanned data. Stand it down
        // and recalibrate once the scan stops. Under state_mutex so one can't
        // start in between. (A periodic check in flight - CONVERGED - stands
        // itself down; a pending cooldown waits for the scan to stop.)
        if (phase_compensation && !kerberos_manual_cal_only()) {
            std::lock_guard<std::mutex> sl(phase_compensation->state_mutex);
            if (phase_compensation->state != PhaseCompensatorState::CONVERGED &&
                bias_tee_enabled.load(std::memory_order_acquire)) {
                set_bias_tee_all_devices(false, devices);
                scanner_cal_deferred.store(true, std::memory_order_release);
                std::cerr << "Scanner: calibration in progress - stood down until the scan stops" << std::endl;
                l2_raw_cap.store(L2_RAW_MAX, std::memory_order_relaxed);  // no calibration during the scan
            }
            discrete_scanner.enabled = true;   // under the lock: no calibration starts after the check
        }
        discrete_scanner.enabled = true;

        std::cout << "Scanner started with " << discrete_scanner.get_num_groups() << " frequency groups" << std::endl;
        return "{\"status\":\"success\",\"message\":\"Scanner started\"}";
    }

    // Stop discrete scanner
    if (json_str.find("\"stop_scanner\"") != std::string::npos) {
        discrete_scanner.enabled = false;
        discrete_scanner.run_generation.fetch_add(1);  // wake the scan thread out of its dwell
        std::cout << "Scanner stopped" << std::endl;
        return "{\"status\":\"success\",\"message\":\"Scanner stopped\"}";
    }

    // Get scanner status
    if (json_str.find("\"get_scanner_status\"") != std::string::npos) {
        std::stringstream response;
        response << "{\"status\":\"success\","
                 << "\"enabled\":" << (discrete_scanner.enabled.load() ? "true" : "false") << ","
                 << "\"num_groups\":" << discrete_scanner.get_num_groups() << ","
                 << "\"current_group_index\":" << discrete_scanner.current_group_index.load() << ","
                 << "\"frequency_change_counter\":" << discrete_scanner.frequency_change_counter.load() << ","
                 << "\"dwell_time_ms\":" << discrete_scanner.dwell_time_ms.load();

        if (discrete_scanner.get_num_groups() > 0) {
            response << ",\"current_frequency\":" << discrete_scanner.get_frequency(discrete_scanner.current_group_index.load());
        }

        response << "}";
        return response.str();
    }

    return "{\"status\":\"error\",\"message\":\"Unknown command\"}";
}