#include "tcp_data_server.hpp"
#include "../core/config.hpp"
#include "../core/utils.hpp"
#include "../dsp/compensation.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace {
// Each client costs a send of every packet (~164 KB at 5 channels, ~40 per
// second) plus up to one packet tail buffered while it is behind. The DoA
// client and a GNU Radio source or two are the expected consumers; cap the
// rest so a LAN host opening connections can't exhaust memory or CPU.
constexpr size_t MAX_DATA_CLIENTS = 8;
}

extern std::atomic<bool> bias_tee_enabled;

TcpDataServer::TcpDataServer() : server_socket(-1) {}

TcpDataServer::~TcpDataServer() {
    stop();
}

bool TcpDataServer::start() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) return false;
    
    if (!set_socket_reuse(server_socket)) {
        close(server_socket);
        return false;
    }
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(TCP_DATA_PORT);
    
    if (bind(server_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_socket);
        return false;
    }
    
    if (listen(server_socket, 5) < 0) {
        close(server_socket);
        return false;
    }
    
    server_thread = std::thread(&TcpDataServer::accept_loop, this);
    std::cout << "TCP Data Server listening on port " << TCP_DATA_PORT << std::endl;
    return true;
}

void TcpDataServer::stop() {
    running = false;
    if (server_socket >= 0) {
        close(server_socket);
        server_socket = -1;
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

void TcpDataServer::broadcast_data(const std::vector<ComplexBuffer>& channel_data) {
    if (channel_data.empty()) return;
    
    std::lock_guard<std::mutex> lock(clients_mutex);
    
    // Remove inactive clients
    clients.erase(std::remove_if(clients.begin(), clients.end(),
        [](const auto& client) { return !client->active; }), clients.end());
    
    if (clients.empty()) return;
    
    static bool first_broadcast = true;
    static int broadcast_counter = 0;
    
    // Validate all channels have the same number of samples. Throttled: this
    // runs per broadcast (~150/s), so a persistent mismatch must not flood the
    // errors-only logfile.
    static std::chrono::steady_clock::time_point last_size_warn{};
    size_t expected_samples = channel_data[0].size();
    for (size_t ch = 1; ch < channel_data.size(); ch++) {
        if (channel_data[ch].size() != expected_samples &&
            std::chrono::steady_clock::now() - last_size_warn > std::chrono::seconds(5)) {
            last_size_warn = std::chrono::steady_clock::now();
            std::cerr << "WARNING: Channel " << ch << " has " << channel_data[ch].size()
                 << " samples, expected " << expected_samples << std::endl;
        }
    }
    
    // Build data packet with per-channel header
    uint32_t num_channels = static_cast<uint32_t>(channel_data.size());
    uint32_t num_samples = static_cast<uint32_t>(expected_samples);
    // Header: magic(4) + channels(4) + samples(4) + phase_state(4) + noise_source(4) +
    //         freq_change_counter(4) + current_group_index(4) + retuning_in_progress(4) + per-channel(8*N)
    size_t header_size = 32 + (num_channels * 8);  // Basic header + status + scanner fields + retuning flag + per-channel freq/gain

    std::vector<uint8_t> packet;
    packet.reserve(header_size + channel_data.size() * expected_samples * 2);

    // Basic Header: magic, num_channels, num_samples
    uint32_t magic = TCP_MAGIC;

    // Get gain from global state (same for all channels)
    float gain_db = (current_gain.load() == -1) ? -1.0f : (current_gain.load() / 10.0f);

    // Check if we're in wideband mode - use per-tuner frequencies if so
    bool is_wideband = wideband_config.enabled.load();

    if (first_broadcast) {
        std::cout << "TCP Data Format: " << num_channels << " channels, "
             << num_samples << " samples/channel" << std::endl;
        std::cout << "Per-Channel Header: Magic(4) + Channels(4) + Samples(4) + " << num_channels
             << " * [Freq(4) + Gain(4)] = " << header_size << " bytes" << std::endl;

        if (is_wideband) {
            std::cout << "Wideband mode: Using per-tuner frequencies" << std::endl;
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                float ch_freq = wideband_config.get_tuner_frequency(ch) / 1e6f;
                std::cout << "  Ch" << ch << ": " << ch_freq << " MHz" << std::endl;
            }
        } else {
            float frequency_hz = static_cast<float>(current_frequency.load());
            std::cout << "Coherent mode: All channels at " << (frequency_hz / 1e6f) << " MHz, "
                 << (gain_db < 0 ? "Auto gain" : std::to_string(gain_db) + " dB") << std::endl;
        }

        std::cout << "Packet structure: [Per-Channel Header " << header_size << " bytes][Ch0: " << num_samples*2
             << " bytes][Ch1: " << num_samples*2 << " bytes]..." << std::endl;
        std::cout << "Total packet size: " << (header_size + num_channels * num_samples * 2) << " bytes" << std::endl;
        first_broadcast = false;
    }

    if (++broadcast_counter % 1000 == 0) {
        if (is_wideband) {
            std::cout << "TCP broadcast #" << broadcast_counter << " (Wideband: ";
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                if (ch > 0) std::cout << ", ";
                std::cout << (wideband_config.get_tuner_frequency(ch) / 1e6f) << "MHz";
            }
            std::cout << ")" << std::endl;
        } else {
            float frequency_hz = static_cast<float>(current_frequency.load());
            std::cout << "TCP broadcast #" << broadcast_counter << " (Coherent: "
                 << (frequency_hz / 1e6f) << " MHz, "
                 << (gain_db < 0 ? "Auto" : std::to_string(gain_db) + " dB") << ")" << std::endl;
        }
    }

    // Build basic header
    append_big_endian(packet, magic);
    append_big_endian(packet, num_channels);
    append_big_endian(packet, num_samples);

    // Add phase compensation state and noise source status
    // This allows client to know when calibration is in progress.
    // High bits piggyback the KerberosSDR support state without changing the
    // wire layout (consumers must mask the low byte for the state enum):
    //   bit 8 (0x100) = kerberos MANUAL-calibration mode active
    //   bit 9 (0x200) = calibration STALE (settings changed since calibrating)
    // --kerberos_sw is intentionally NOT flagged: calibration is automatic
    // there, so the client should behave exactly as with a KrakenSDR.
    auto phase_state = get_phase_compensation_state();
    uint32_t phase_state_value = phase_state ? static_cast<uint32_t>(*phase_state) : 0;
    if (kerberos_manual_cal_only()) {
        phase_state_value |= 0x100u;
        if (kerberos_cal_stale.load(std::memory_order_relaxed)) phase_state_value |= 0x200u;
    }
    uint32_t noise_source_active = bias_tee_enabled.load() ? 1 : 0;

    append_big_endian(packet, phase_state_value);
    append_big_endian(packet, noise_source_active);

    // Add discrete scanner fields (server-side scanner implementation)
    // These values come from the global discrete_scanner state
    uint32_t frequency_change_counter = discrete_scanner.frequency_change_counter.load(std::memory_order_acquire);
    uint32_t current_group_index = discrete_scanner.current_group_index.load(std::memory_order_acquire);
    uint32_t retuning_in_progress = discrete_scanner.retuning_in_progress.load(std::memory_order_acquire) ? 1 : 0;

    append_big_endian(packet, frequency_change_counter);
    append_big_endian(packet, current_group_index);
    append_big_endian(packet, retuning_in_progress);

    // Add per-channel frequency and gain
    // In wideband mode, each tuner has a different frequency
    // In coherent mode, all tuners share the same frequency
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        float ch_frequency_hz;
        if (is_wideband) {
            ch_frequency_hz = static_cast<float>(wideband_config.get_tuner_frequency(ch));
        } else {
            ch_frequency_hz = static_cast<float>(current_frequency.load());
        }

        packet.insert(packet.end(), reinterpret_cast<const uint8_t*>(&ch_frequency_hz),
                      reinterpret_cast<const uint8_t*>(&ch_frequency_hz) + sizeof(float));
        packet.insert(packet.end(), reinterpret_cast<const uint8_t*>(&gain_db),
                      reinterpret_cast<const uint8_t*>(&gain_db) + sizeof(float));
    }
    
    // Convert complex samples back to interleaved uint8 IQ
    // Data layout: [All Ch0 samples][All Ch1 samples][All Ch2 samples]...
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        if (ch < channel_data.size() && channel_data[ch].size() == expected_samples) {
            // Channel has correct data
            for (size_t s = 0; s < expected_samples; s++) {
                const auto& sample = channel_data[ch][s];
                
                // Convert float (-1 to +1) back to uint8 with the exact inverse
                // of every decoder (heimdall's iq_lut, the DoA client, the GR
                // source: (u - 127.5) / 127.5), so a code round-trips unchanged.
                // (x*127 + 128 mapped zero to 128 = +0.5 LSB: an identical DC
                // term on every channel, and merged codes 127/128.)
                uint8_t i_val = static_cast<uint8_t>(clamp(
                    roundf(sample.real() * 127.5f + 127.5f), 0.0f, 255.0f));
                uint8_t q_val = static_cast<uint8_t>(clamp(
                    roundf(sample.imag() * 127.5f + 127.5f), 0.0f, 255.0f));
                
                packet.push_back(i_val);
                packet.push_back(q_val);
            }
        } else {
            // Channel data is missing or wrong size - send zeros (reuses the
            // 5s warn throttle above: same root cause, same per-broadcast rate)
            if (std::chrono::steady_clock::now() - last_size_warn > std::chrono::seconds(5)) {
                last_size_warn = std::chrono::steady_clock::now();
                std::cerr << "ERROR: Channel " << ch << " has invalid data, sending zeros" << std::endl;
            }
            for (uint32_t s = 0; s < num_samples; s++) {
                packet.push_back(128);  // Zero I
                packet.push_back(128);  // Zero Q
            }
        }
    }
    
    // Verify packet size
    size_t expected_size = header_size + num_channels * num_samples * 2;  // Dynamic header size + data
    if (packet.size() != expected_size) {
        std::cerr << "ERROR: Packet size mismatch! Expected " << expected_size 
             << " got " << packet.size() << std::endl;
    }
    
    // Send to all connected clients. The sockets are non-blocking and a packet
    // (~164 KB at 5 channels) is larger than a socket send buffer usually
    // holds, so a short write is normal back-pressure, not an error - over
    // Ethernet the first packet alone typically goes out in pieces. Clients
    // parse a byte stream, so a packet is never cut: its unsent tail waits in
    // tx_buffer and is finished first on the next broadcast, and while a
    // client is behind, NEW packets are dropped whole (a data gap, but the
    // framing stays intact and memory is bounded to one tail per client).
    static std::chrono::steady_clock::time_point last_drop_warn{};
    static uint64_t dropped_since_warn = 0;
    auto send_some = [](TcpClient& c, const uint8_t* data, size_t len) -> ssize_t {
        ssize_t n = send(c.socket, data, len, MSG_NOSIGNAL);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        std::cerr << "TCP Data: client disconnected (" << strerror(errno) << ")" << std::endl;
        c.active = false;
        return -1;
    };
    for (auto& client : clients) {
        if (!client->active) continue;

        // Finish the in-flight packet first.
        if (!client->tx_buffer.empty()) {
            ssize_t n = send_some(*client, reinterpret_cast<const uint8_t*>(client->tx_buffer.data()),
                                  client->tx_buffer.size());
            if (n < 0) continue;
            client->tx_buffer.erase(0, static_cast<size_t>(n));
            if (!client->tx_buffer.empty()) {
                dropped_since_warn++;  // still behind: skip this packet whole
                continue;
            }
        }

        ssize_t n = send_some(*client, packet.data(), packet.size());
        if (n < 0) continue;
        if (n == 0) {
            dropped_since_warn++;  // nothing of it went out: drop it whole
        } else if (static_cast<size_t>(n) < packet.size()) {
            client->tx_buffer.assign(reinterpret_cast<const char*>(packet.data()) + n,
                                     packet.size() - static_cast<size_t>(n));
        }
    }
    if (dropped_since_warn > 0 &&
        std::chrono::steady_clock::now() - last_drop_warn > std::chrono::seconds(5)) {
        last_drop_warn = std::chrono::steady_clock::now();
        std::cerr << "TCP Data: slow client(s), dropped " << dropped_since_warn
                  << " packet(s) since the last report" << std::endl;
        dropped_since_warn = 0;
    }
}

void TcpDataServer::accept_loop() {
    while (running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        
        timeval timeout{0, 100000}; // 100ms timeout
        int result = select(server_socket + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (result > 0 && FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr{};
            socklen_t addr_len = sizeof(client_addr);
            int client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
            
            if (client_socket >= 0) {
                std::lock_guard<std::mutex> lock(clients_mutex);
                clients.erase(std::remove_if(clients.begin(), clients.end(),
                    [](const auto& client) { return !client->active; }), clients.end());
                if (clients.size() >= MAX_DATA_CLIENTS) {
                    std::cerr << "TCP Data: refusing connection from " << inet_ntoa(client_addr.sin_addr)
                              << " (" << clients.size() << " clients connected)" << std::endl;
                    close(client_socket);
                    continue;
                }
                // Set non-blocking mode
                set_socket_nonblocking(client_socket);

                clients.push_back(std::make_unique<TcpClient>(client_socket));
                std::cout << "TCP Data: FFT Viewer connected from " << inet_ntoa(client_addr.sin_addr) 
                     << ":" << ntohs(client_addr.sin_port) << std::endl;
                std::cout << "TCP Data: Now streaming " << active_num_elements.load() << " channels with per-channel frequency/gain info" << std::endl;
            }
        }
    }
}