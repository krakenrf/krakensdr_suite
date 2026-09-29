#include "networking/web_mapper.hpp"

// Built-in KrakenSDR web mapper output. Replaces the external Node.js
// web_mapper_middleware: the doapost record is built in-process from the same
// capture helpers as DOA_value.html / the DoA logger, and relayed either to
// the KrakenPro cloud map (WSS client) or to local LAN clients (plain WS
// server). See web_mapper.hpp for the overview.

#include "globals.hpp"
#include "config.hpp"
#include "channel_manager.hpp"
#include "control_handler.hpp"
#include "decimator_manager.hpp"
#include "doa_logger.hpp"
#include "station_info.hpp"
#include "signal_processing/music_processor.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

using namespace std;

WebMapper web_mapper;

extern DecimatorManager decimator_manager;

namespace {

int64_t now_ms() {
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch()).count();
}

// Shortest-ish numeric formatting, close to JS String(number) so the settings
// payload / record fields look like the legacy middleware's output.
string fmt_num(double v) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

string fmt_fixed(double v, int decimals) {
    char buf[40];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

string b64_encode(const unsigned char* data, size_t len) {
    string out;
    out.resize(4 * ((len + 2) / 3) + 1);
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, (int)len);
    out.resize(n > 0 ? n : 0);
    return out;
}

// ---------------------------------------------------------------------------
// WebSocket framing (shared by the cloud client and the local server)
// ---------------------------------------------------------------------------

constexpr size_t WS_MAX_FRAME = 4 * 1024 * 1024;

// Encode one FIN text/binary/control frame. Client->server frames are masked.
string ws_encode(uint8_t opcode, const string& payload, bool mask) {
    string f;
    f.reserve(payload.size() + 14);
    f += char(0x80 | (opcode & 0x0F));
    size_t len = payload.size();
    uint8_t mbit = mask ? 0x80 : 0x00;
    if (len < 126) {
        f += char(mbit | (uint8_t)len);
    } else if (len < 65536) {
        f += char(mbit | 126);
        f += char((len >> 8) & 0xFF);
        f += char(len & 0xFF);
    } else {
        f += char(mbit | 127);
        for (int i = 7; i >= 0; i--) f += char((uint64_t(len) >> (8 * i)) & 0xFF);
    }
    if (mask) {
        unsigned char m[4];
        if (RAND_bytes(m, 4) != 1) { m[0] = 0x5a; m[1] = 0xa5; m[2] = 0x3c; m[3] = 0xc3; }
        f.append(reinterpret_cast<char*>(m), 4);
        size_t base = f.size();
        f += payload;
        for (size_t i = 0; i < len; i++) f[base + i] ^= char(m[i & 3]);
    } else {
        f += payload;
    }
    return f;
}

// One decoded frame from the stream buffer. Returns false if a complete frame
// is not available yet; sets bad=true on a protocol/size violation.
struct WsFrame { uint8_t opcode; bool fin; string payload; };
bool ws_decode(string& buf, WsFrame& out, bool& bad) {
    bad = false;
    if (buf.size() < 2) return false;
    uint8_t b0 = (uint8_t)buf[0], b1 = (uint8_t)buf[1];
    bool masked = b1 & 0x80;
    uint64_t len = b1 & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (buf.size() < 4) return false;
        len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
        pos = 4;
    } else if (len == 127) {
        if (buf.size() < 10) return false;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | (uint8_t)buf[2 + i];
        pos = 10;
    }
    if (len > WS_MAX_FRAME) { bad = true; return false; }
    size_t need = pos + (masked ? 4 : 0) + (size_t)len;
    if (buf.size() < need) return false;
    unsigned char m[4] = {0, 0, 0, 0};
    if (masked) { memcpy(m, buf.data() + pos, 4); pos += 4; }
    out.opcode = b0 & 0x0F;
    out.fin = b0 & 0x80;
    out.payload.assign(buf, pos, (size_t)len);
    if (masked)
        for (size_t i = 0; i < out.payload.size(); i++) out.payload[i] ^= char(m[i & 3]);
    buf.erase(0, need);
    return true;
}

// Blocking send of a fully-encoded frame on a plain fd (used pre-TLS paths).
bool send_all(int fd, const string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

void set_timeout(int fd, int which, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, which, &tv, sizeof(tv));
}

// ---------------------------------------------------------------------------
// Cloud sink: WSS client to the KrakenPro mapping server
// ---------------------------------------------------------------------------

class CloudSink {
public:
    ~CloudSink() { close(); }

    bool isOpen() const { return open_; }

    // Parse wss://host[:port][/path] and establish TLS + WebSocket upgrade.
    bool connect(const string& url, string& err) {
        close();
        string host, path;
        int port = 0;
        if (!parseUrl(url, host, port, path)) { err = "bad URL: " + url; return false; }

        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", port);
        if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
            err = "DNS lookup failed for " + host;
            return false;
        }
        int fd = -1;
        for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            set_timeout(fd, SO_SNDTIMEO, 8000);
            set_timeout(fd, SO_RCVTIMEO, 8000);
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) { err = "TCP connect to " + host + ":" + to_string(port) + " failed"; return false; }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { ::close(fd); err = "SSL_CTX_new failed"; return false; }
        // The mapping server historically runs a cert the legacy stack never
        // verified (rejectUnauthorized:false); keep that behavior.
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        ssl_ = SSL_new(ctx_);
        if (!ssl_) { cleanupSsl(); ::close(fd); err = "SSL_new failed"; return false; }
        SSL_set_fd(ssl_, fd);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) {
            cleanupSsl();
            ::close(fd);
            err = "TLS handshake with " + host + " failed";
            return false;
        }
        fd_ = fd;

        // WebSocket upgrade.
        unsigned char rnd[16];
        RAND_bytes(rnd, sizeof(rnd));
        string ws_key = b64_encode(rnd, sizeof(rnd));
        ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << ws_key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "User-Agent: kraken_doa\r\n\r\n";
        if (!sslWrite(req.str())) { close(); err = "WS upgrade send failed"; return false; }

        string resp;
        int64_t deadline = now_ms() + 8000;
        while (resp.find("\r\n\r\n") == string::npos) {
            if (now_ms() > deadline || resp.size() > 16384) {
                close();
                err = "WS upgrade response timeout";
                return false;
            }
            char buf[2048];
            int n = SSL_read(ssl_, buf, sizeof(buf));
            if (n <= 0) { close(); err = "WS upgrade read failed"; return false; }
            resp.append(buf, n);
        }
        if (resp.find(" 101 ") == string::npos) {
            close();
            err = "WS upgrade rejected: " + resp.substr(0, resp.find("\r\n"));
            return false;
        }
        // Any bytes past the header already belong to the frame stream.
        rxbuf_ = resp.substr(resp.find("\r\n\r\n") + 4);

        // Short read timeout from here on: the worker's pump doubles as its
        // sleep, so this bounds the loop cadence.
        set_timeout(fd_, SO_RCVTIMEO, 100);
        set_timeout(fd_, SO_SNDTIMEO, 5000);
        open_ = true;
        return true;
    }

    bool sendText(const string& payload) {
        if (!open_) return false;
        if (!sslWrite(ws_encode(0x1, payload, true))) { close(); return false; }
        return true;
    }

    bool sendPing() {
        if (!open_) return false;
        if (!sslWrite(ws_encode(0x9, "", true))) { close(); return false; }
        return true;
    }

    // Read whatever is available (blocking up to the 100 ms socket timeout)
    // and return complete text messages. Returns false when the connection
    // dropped.
    bool pump(vector<string>& messages) {
        if (!open_) return false;
        char buf[16384];
        int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            rxbuf_.append(buf, n);
            // Drain everything OpenSSL already has decrypted.
            while (SSL_pending(ssl_) > 0) {
                n = SSL_read(ssl_, buf, sizeof(buf));
                if (n <= 0) break;
                rxbuf_.append(buf, n);
            }
        } else {
            int serr = SSL_get_error(ssl_, n);
            bool timeout = (serr == SSL_ERROR_WANT_READ || serr == SSL_ERROR_WANT_WRITE ||
                            (serr == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)));
            if (!timeout) { close(); return false; }
        }

        WsFrame f;
        bool bad = false;
        while (ws_decode(rxbuf_, f, bad)) {
            switch (f.opcode) {
                case 0x0:  // continuation
                    frag_ += f.payload;
                    if (f.fin) { messages.push_back(std::move(frag_)); frag_.clear(); }
                    break;
                case 0x1:  // text
                case 0x2:  // binary (not expected; treat alike)
                    if (f.fin) messages.push_back(std::move(f.payload));
                    else frag_ = std::move(f.payload);
                    break;
                case 0x8: close(); return false;                    // close
                case 0x9: sslWrite(ws_encode(0xA, f.payload, true)); break;  // ping -> pong
                default: break;                                     // pong / ignore
            }
        }
        if (bad) { close(); return false; }
        return true;
    }

    void close() {
        open_ = false;
        cleanupSsl();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        rxbuf_.clear();
        frag_.clear();
    }

private:
    static bool parseUrl(const string& url, string& host, int& port, string& path) {
        // The cloud map is TLS-only, so only wss:// is accepted.
        if (url.rfind("wss://", 0) != 0) return false;
        string rest = url.substr(6);
        size_t slash = rest.find('/');
        path = (slash == string::npos) ? "/" : rest.substr(slash);
        string hostport = (slash == string::npos) ? rest : rest.substr(0, slash);
        size_t colon = hostport.rfind(':');
        if (colon != string::npos) {
            host = hostport.substr(0, colon);
            port = atoi(hostport.c_str() + colon + 1);
        } else {
            host = hostport;
            port = 443;
        }
        return !host.empty() && port > 0 && port < 65536;
    }

    bool sslWrite(const string& data) {
        size_t off = 0;
        while (off < data.size()) {
            int n = SSL_write(ssl_, data.data() + off, (int)(data.size() - off));
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    void cleanupSsl() {
        if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
    }

    int fd_ = -1;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    bool open_ = false;
    string rxbuf_;
    string frag_;
};

// ---------------------------------------------------------------------------
// Local sink: plain WebSocket broadcast server for LAN map clients
// ---------------------------------------------------------------------------

class LocalSink {
public:
    ~LocalSink() { close(); }

    bool isListening() const { return listen_fd_ >= 0; }
    int  port() const { return port_; }
    int  clientCount() const {
        int n = 0;
        for (const auto& c : clients_) if (c.open) n++;
        return n;
    }

    bool listen(int port, string& err) {
        close();
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        bool v6 = fd >= 0;
        if (!v6) fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { err = "socket() failed"; return false; }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        int ok;
        if (v6) {
            int zero = 0;  // dual-stack: accept IPv4 too
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
            struct sockaddr_in6 a = {};
            a.sin6_family = AF_INET6;
            a.sin6_addr = in6addr_any;
            a.sin6_port = htons((uint16_t)port);
            ok = ::bind(fd, (struct sockaddr*)&a, sizeof(a));
        } else {
            struct sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = INADDR_ANY;
            a.sin_port = htons((uint16_t)port);
            ok = ::bind(fd, (struct sockaddr*)&a, sizeof(a));
        }
        if (ok != 0 || ::listen(fd, 8) != 0) {
            ::close(fd);
            err = "cannot listen on port " + to_string(port) + " (" + strerror(errno) + ")";
            return false;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        listen_fd_ = fd;
        port_ = port;
        return true;
    }

    // Accept new connections and service existing ones (handshakes, pings,
    // closes). Blocks up to timeout_ms when idle.
    void pump(int timeout_ms) {
        if (listen_fd_ < 0) return;
        vector<struct pollfd> pfds;
        pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto& c : clients_) pfds.push_back({c.fd, POLLIN, 0});
        int n = poll(pfds.data(), pfds.size(), timeout_ms);
        if (n <= 0) return;

        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(listen_fd_, nullptr, nullptr);
                if (cfd < 0) break;
                fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
                int one = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                clients_.push_back(Client{cfd, false, {}});
            }
        }
        for (size_t i = 0; i < clients_.size(); i++) {
            if (i + 1 < pfds.size() && (pfds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)))
                service(clients_[i]);
        }
        clients_.erase(remove_if(clients_.begin(), clients_.end(),
                                 [](const Client& c) { return c.fd < 0; }),
                       clients_.end());
    }

    // Broadcast one text frame; drops clients that can't keep up (a partial
    // non-blocking send would corrupt their frame stream). Returns the number
    // of clients that received it.
    int broadcast(const string& payload) {
        if (clients_.empty()) return 0;
        string frame = ws_encode(0x1, payload, false);
        int sent = 0;
        for (auto& c : clients_) {
            if (c.fd < 0 || !c.open) continue;
            ssize_t n = ::send(c.fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            if (n == (ssize_t)frame.size()) sent++;
            else drop(c);
        }
        return sent;
    }

    void close() {
        for (auto& c : clients_) drop(c);
        clients_.clear();
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
        port_ = 0;
    }

private:
    struct Client {
        int fd = -1;
        bool open = false;   // WS handshake completed
        string rxbuf;
    };

    void drop(Client& c) {
        if (c.fd >= 0) ::close(c.fd);
        c.fd = -1;
        c.open = false;
    }

    void service(Client& c) {
        char buf[4096];
        for (;;) {
            ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
            if (n > 0) { c.rxbuf.append(buf, n); if (c.rxbuf.size() > 65536) { drop(c); return; } continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            drop(c);  // closed or errored
            return;
        }

        if (!c.open) {
            size_t hdr_end = c.rxbuf.find("\r\n\r\n");
            if (hdr_end == string::npos) return;
            // Extract Sec-WebSocket-Key (case-insensitive header match).
            string lower = c.rxbuf.substr(0, hdr_end);
            for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
            size_t kp = lower.find("sec-websocket-key:");
            if (kp == string::npos) { drop(c); return; }
            size_t vs = kp + strlen("sec-websocket-key:");
            size_t ve = c.rxbuf.find("\r\n", vs);
            string key = c.rxbuf.substr(vs, ve - vs);
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);

            string accept_src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            unsigned char sha[SHA_DIGEST_LENGTH];
            SHA1(reinterpret_cast<const unsigned char*>(accept_src.data()),
                 accept_src.size(), sha);
            string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " + b64_encode(sha, sizeof(sha)) + "\r\n\r\n";
            if (!send_all(c.fd, resp)) { drop(c); return; }
            c.rxbuf.erase(0, hdr_end + 4);
            c.open = true;
        }

        // Service control frames from the client (we ignore its data frames).
        WsFrame f;
        bool bad = false;
        while (c.fd >= 0 && ws_decode(c.rxbuf, f, bad)) {
            if (f.opcode == 0x8) { drop(c); return; }
            if (f.opcode == 0x9) {
                if (!send_all(c.fd, ws_encode(0xA, f.payload, false))) { drop(c); return; }
            }
        }
        if (bad) drop(c);
    }

    int listen_fd_ = -1;
    int port_ = 0;
    vector<Client> clients_;
};

// ---------------------------------------------------------------------------
// doapost record building (parity with the middleware's mapper.js, which in
// turn mirrors DOA_value.html)
// ---------------------------------------------------------------------------

// One record per DoaRecord; returns the legacy doapost JSON object.
string build_doapost_json(const DoaRecord& rec, const StationLocation& sl,
                          int num_corr_sources) {
    // The app/cloud contract uses the integer compass bearing, exactly as
    // DOA_value.html serves it.
    int bearing = static_cast<int>(lround(rec.app_bearing)) % 360;

    string doa_array;
    doa_array.reserve(rec.spectrum.size() * 7);
    char buf[24];
    for (double v : rec.spectrum) {
        snprintf(buf, sizeof(buf), "%.2f,", v);
        doa_array += buf;
    }

    int64_t wall_ms = chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();

    double heading = sl.heading_valid ? sl.heading : 0.0;

    ostringstream js;
    js << "{\"station_id\":\"" << json_escape(sl.id) << "\""
       << ",\"tStamp\":" << wall_ms
       << ",\"gps_timestamp\":" << sl.timestamp_ms
       << ",\"latitude\":\"" << fmt_num(sl.lat) << "\""
       << ",\"longitude\":\"" << fmt_num(sl.lon) << "\""
       << ",\"gpsBearing\":\"" << fmt_num(heading) << "\""
       << ",\"speed\":\"" << fmt_num(sl.speed) << "\""
       << ",\"radioBearing\":\"" << bearing << "\""
       << ",\"conf\":\"" << fmt_fixed(rec.confidence, 2) << "\""
       << ",\"power\":\"" << fmt_fixed(rec.power_db, 1) << "\""
       << ",\"freq\":" << rec.freq_hz
       << ",\"antType\":\"" << rec.antenna << "\""
       << ",\"latency\":0"
       << ",\"processing_time\":0"
       << ",\"doaArray\":\"" << doa_array << "\""
       << ",\"adc_overdrive\":0"
       << ",\"num_corr_sources\":\"" << num_corr_sources << "\""
       << ",\"snr_db\":\"" << fmt_fixed(rec.power_db, 1) << "\"}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Cloud settings payload (legacy flat settings.json schema) - the mapper UI
// reads this whole shape, so send it populated with live receiver state.
// ---------------------------------------------------------------------------

string build_settings_payload(const string& apikey) {
    int ch = active_channel.load(memory_order_relaxed);
    double center_mhz = ChannelManager::get_frequency(ch) / 1e6;
    double gain = ChannelManager::get_gain(ch);
    gain = round(gain * 10.0) / 10.0;  // snap to the 0.1 dB RTL-SDR gain step

    // Array/MUSIC state from the first live decimator's processor (settings
    // are shared across all decimators).
    string topo = "UCA";
    string ula_dir = "Both";
    double spacing_m = 0.0;
    double array_offset = 0.0;
    int sources = 1;
    for (const auto& d : decimator_manager.getAllDecimators()) {
        if (!d || !d->music_processor || d->being_deleted.load(memory_order_relaxed)) continue;
        auto* mp = d->music_processor.get();
        switch (mp->getArrayTopology()) {
            case ArrayTopology::ULA:    topo = "ULA"; break;
            case ArrayTopology::CUSTOM: topo = "Custom"; break;
            default:                    topo = "UCA"; break;
        }
        switch (mp->getULAOutputMode()) {
            case ULAOutputMode::FORWARD:  ula_dir = "Forward"; break;
            case ULAOutputMode::BACKWARD: ula_dir = "Backward"; break;
            default:                      ula_dir = "Both"; break;
        }
        // "Array size" in the mapper UI: ULA element spacing, else UCA radius.
        spacing_m = (topo == "ULA" ? mp->getElementSpacing() : mp->getArrayRadius()) / 1000.0;
        array_offset = mp->getArrayOffset();
        sources = max(1, mp->getNumSignalSources());
        break;
    }

    StationLocation sl = station_info.resolve();
    const char* loc_src = "Static";
    switch (sl.source) {
        case LocationSource::STATIC: loc_src = "Static"; break;
        case LocationSource::GPS:    loc_src = "gpsd"; break;
        case LocationSource::MOBILE: loc_src = "Web"; break;
    }

    auto info = decimator_manager.getDecimatorInfoList();
    double center_hz = center_mhz * 1e6;

    ostringstream js;
    js << "{\"center_freq\":" << fmt_num(center_mhz)
       << ",\"uniform_gain\":" << fmt_num(gain)
       << ",\"data_interface\":\"eth\""
       << ",\"default_ip\":\"0.0.0.0\""
       << ",\"en_doa\":true"
       << ",\"ant_arrangement\":\"" << topo << "\""
       << ",\"ula_direction\":\"" << ula_dir << "\""
       << ",\"ant_spacing_meters\":" << fmt_num(spacing_m)
       << ",\"custom_array_x_meters\":\"\""
       << ",\"custom_array_y_meters\":\"\""
       << ",\"array_offset\":" << fmt_num(array_offset)
       << ",\"doa_method\":\"MUSIC\""
       << ",\"en_fbavg\":false"
       << ",\"compass_offset\":0"
       << ",\"doa_fig_type\":\"Polar\""
       << ",\"en_peak_hold\":false"
       << ",\"en_hw_check\":0"
       << ",\"logging_level\":5"
       << ",\"disable_tooltips\":0"
       << ",\"doa_data_format\":\"Kraken Pro Remote\""
       << ",\"station_id\":\"" << json_escape(sl.id) << "\""
       << ",\"location_source\":\"" << loc_src << "\""
       << ",\"latitude\":" << fmt_num(sl.lat)
       << ",\"longitude\":" << fmt_num(sl.lon)
       << ",\"heading\":" << fmt_num(sl.heading_valid ? sl.heading : 0.0)
       << ",\"krakenpro_key\":\"" << json_escape(apikey) << "\""
       << ",\"rdf_mapper_server\":\"http://MY_RDF_MAPPER_SERVER.com/save.php\""
       << ",\"spectrum_calculation\":\"Single\""
       << ",\"vfo_mode\":\"Standard\""
       << ",\"dsp_decimation\":1"
       << ",\"active_vfos\":" << max<size_t>(1, info.size())
       << ",\"output_vfo\":0"
       << ",\"en_optimize_short_bursts\":false"
       << ",\"expected_num_of_sources\":" << sources;

    // Per-VFO fields (legacy has 0..15); fill from decimators where known.
    for (int i = 0; i < 16; i++) {
        int64_t vfo_freq = llround(center_hz);
        int bw_hz = 12500;
        double squelch = -120.0;
        if (i < (int)info.size()) {
            vfo_freq = llround(center_hz + info[i].frequency_offset_hz);
            int bwi = info[i].bandwidth_index;
            if (bwi >= 0 && bwi < NUM_BANDWIDTH_OPTIONS)
                bw_hz = (int)lround(BANDWIDTH_OPTIONS[bwi].bandwidth_mhz * 1e6);
            squelch = info[i].squelch_level;
        }
        js << ",\"vfo_freq_" << i << "\":" << vfo_freq
           << ",\"vfo_bw_" << i << "\":" << bw_hz
           << ",\"vfo_squelch_" << i << "\":" << fmt_num(squelch);
    }
    js << "}";
    return js.str();
}

// ---------------------------------------------------------------------------
// Cloud -> receiver settings application (remote retune)
// ---------------------------------------------------------------------------

int closest_bandwidth_index(double hz) {
    int best = 0;
    double best_diff = 1e18;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        double diff = fabs(BANDWIDTH_OPTIONS[i].bandwidth_mhz * 1e6 - hz);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

// Runs ON THE uWS LOOP THREAD (deferred), where control commands normally
// execute - so sync echo, persistence and heimdall forwarding all behave
// exactly as if a browser had sent each command. Reads decimator state fresh
// between steps, so VFO reconciliation has no async races.
void apply_cloud_settings_on_loop(map<string, string> s) {
    auto has = [&](const char* k) { return s.count(k) != 0; };
    auto cmd = [](const string& c) { ControlHandler::handle_websocket_message(c); };

    // Station identity / web-mapper config pushed back by the cloud.
    if (has("station_id")) cmd("STATION_ID:" + s["station_id"]);
    if (has("krakenpro_key")) cmd("WEB_MAPPER_KEY:" + s["krakenpro_key"]);
    if (has("mapping_server_url") && !s["mapping_server_url"].empty())
        cmd("WEB_MAPPER_URL:" + s["mapping_server_url"]);
    if (has("latitude") || has("longitude") || has("heading")) {
        double lat, lon, hd;
        station_info.getStatic(lat, lon, hd);
        try {
            if (has("latitude"))  lat = stod_finite(s["latitude"]);
            if (has("longitude")) lon = stod_finite(s["longitude"]);
            if (has("heading"))   hd  = stod_finite(s["heading"]);
            cmd("STATIC_LOCATION:" + fmt_num(lat) + "," + fmt_num(lon) + "," + fmt_num(hd));
        } catch (const exception&) { /* malformed number: leave unchanged */ }
    }

    // Center frequency must be applied BEFORE per-VFO offsets, since a VFO
    // frequency is sent to the receiver as an offset from the center.
    if (has("center_freq")) cmd("FREQ:" + s["center_freq"]);
    if (has("uniform_gain")) cmd("GAIN:" + s["uniform_gain"]);

    string arrangement;
    if (has("ant_arrangement")) {
        arrangement = s["ant_arrangement"];
        for (char& c : arrangement) c = (char)toupper((unsigned char)c);
        cmd("TOPOLOGY:" + arrangement);
    }
    // Array size: the cloud sends `ant_spacing_meters` for either topology,
    // but the receiver command differs - RADIUS (mm) for a UCA, SPACING (mm)
    // for a ULA.
    if (has("ant_spacing_meters")) {
        try {
            long mm = lround(stod_finite(s["ant_spacing_meters"]) * 1000.0);
            string topo = arrangement;
            if (topo.empty()) {
                for (const auto& d : decimator_manager.getAllDecimators()) {
                    if (!d || !d->music_processor) continue;
                    topo = (d->music_processor->getArrayTopology() == ArrayTopology::ULA)
                               ? "ULA" : "UCA";
                    break;
                }
            }
            cmd((topo == "ULA" ? "SPACING:" : "RADIUS:") + to_string(mm));
        } catch (const exception&) { /* ignore malformed */ }
    }
    if (has("array_offset")) cmd("ARRAY_OFFSET:" + s["array_offset"]);
    if (has("expected_num_of_sources"))
        cmd("MUSIC_SIGNAL_SOURCES:" + s["expected_num_of_sources"]);

    // --- Per-VFO reconciliation (vfo_freq_N / vfo_bw_N / vfo_squelch_N) ---
    auto info = decimator_manager.getDecimatorInfoList();
    long target = -1;
    if (has("active_vfos")) {
        try { target = stol(s["active_vfos"]); } catch (const exception&) { target = -1; }
    }
    if (target >= 1 && target != (long)info.size()) {
        while ((long)info.size() < target) {
            cmd("ADD_DECIMATOR");
            auto next = decimator_manager.getDecimatorInfoList();
            if (next.size() == info.size()) break;  // add failed; stop
            info = std::move(next);
        }
        while ((long)info.size() > target) {
            cmd("REMOVE_DECIMATOR:" + to_string(info.back().id));
            auto next = decimator_manager.getDecimatorInfoList();
            if (next.size() == info.size()) break;
            info = std::move(next);
        }
    }

    double center_hz = ChannelManager::get_frequency(active_channel.load(memory_order_relaxed));
    for (size_t i = 0; i < info.size(); i++) {
        int id = info[i].id;
        string k = "vfo_freq_" + to_string(i);
        if (has(k.c_str()) && center_hz > 0) {
            try {
                double offset_khz = (stod_finite(s[k]) - center_hz) / 1000.0;
                cmd("SET_DECIMATOR_FREQ:" + to_string(id) + ":" + fmt_num(offset_khz));
            } catch (const exception&) {}
        }
        // Bandwidth: the mapper allows any integer Hz, but the receiver only
        // supports the discrete BANDWIDTH_OPTIONS set - snap to the nearest.
        k = "vfo_bw_" + to_string(i);
        if (has(k.c_str())) {
            try {
                int idx = closest_bandwidth_index(stod_finite(s[k]));
                cmd("SET_DECIMATOR_BW:" + to_string(id) + ":" + to_string(idx));
            } catch (const exception&) {}
        }
        // The web mapper has no squelch on/off toggle - it assumes squelch is
        // always enabled, so any vfo_squelch_N sets the level AND turns it on.
        k = "vfo_squelch_" + to_string(i);
        if (has(k.c_str())) {
            cmd("DEC_SQUELCH_LEVEL:" + to_string(id) + ":" + s[k]);
            cmd("DEC_SQUELCH_ENABLE:" + to_string(id) + ":1");
        }
    }
}

// Parse a cloud "settings" push and defer its application to the uWS loop.
void handle_cloud_message(const string& text) {
    string fn;
    if (!json_find(text, "function", fn) || fn != "settings") return;
    string inner;
    if (!json_find(text, "settings", inner)) return;  // stringified JSON payload

    static const char* KEYS[] = {
        "station_id", "krakenpro_key", "mapping_server_url",
        "latitude", "longitude", "heading",
        "center_freq", "uniform_gain", "ant_arrangement", "ant_spacing_meters",
        "array_offset", "expected_num_of_sources", "active_vfos",
    };
    map<string, string> s;
    string v;
    for (const char* k : KEYS)
        if (json_find(inner, k, v)) s[k] = v;
    for (int i = 0; i < 16; i++) {
        for (const char* p : {"vfo_freq_", "vfo_bw_", "vfo_squelch_"}) {
            string k = string(p) + to_string(i);
            if (json_find(inner, k, v)) s[k] = v;
        }
    }
    if (s.empty()) return;

    if (!loop) return;  // web server not up yet
    loop->defer([s = std::move(s)]() mutable {
        apply_cloud_settings_on_loop(std::move(s));
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// WebMapper: config plumbing + worker thread
// ---------------------------------------------------------------------------

void WebMapper::setEnabled(bool en) {
    if (enabled_.exchange(en) != en)
        config_gen_.fetch_add(1, memory_order_release);
}

void WebMapper::setMode(const string& mode) {
    string m = (mode == "local") ? "local" : "remote";
    lock_guard<mutex> lk(cfg_mtx_);
    if (mode_ != m) { mode_ = m; config_gen_.fetch_add(1, memory_order_release); }
}

void WebMapper::setKey(const string& key) {
    lock_guard<mutex> lk(cfg_mtx_);
    if (key_ != key) { key_ = key; config_gen_.fetch_add(1, memory_order_release); }
}

void WebMapper::setServerUrl(const string& url) {
    lock_guard<mutex> lk(cfg_mtx_);
    if (url.empty() || server_url_ == url) return;
    server_url_ = url;
    config_gen_.fetch_add(1, memory_order_release);
}

void WebMapper::setLocalWsPort(int port) {
    if (port < 1 || port > 65535) return;
    if (local_ws_port_.exchange(port) != port)
        config_gen_.fetch_add(1, memory_order_release);
}

string WebMapper::getMode() const { lock_guard<mutex> lk(cfg_mtx_); return mode_; }
string WebMapper::getKey() const { lock_guard<mutex> lk(cfg_mtx_); return key_; }
string WebMapper::getServerUrl() const { lock_guard<mutex> lk(cfg_mtx_); return server_url_; }

void WebMapper::appendStatusJson(string& out) const {
    string state, err;
    {
        lock_guard<mutex> lk(status_mtx_);
        state = status_state_;
        err = status_error_;
    }
    out += "\"web_mapper\":{\"enabled\":";
    out += isEnabled() ? "true" : "false";
    out += ",\"mode\":\"" + getMode() + "\"";
    out += ",\"state\":\"" + json_escape(state) + "\"";
    out += ",\"clients\":" + to_string(local_clients_.load(memory_order_relaxed));
    out += ",\"records\":" + to_string(records_sent_.load(memory_order_relaxed));
    out += ",\"error\":\"" + json_escape(err) + "\"}";
}

void WebMapper::start() {
    if (running_.exchange(true)) return;
    worker_ = thread(&WebMapper::workerThread, this);
}

void WebMapper::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) worker_.join();
}

void WebMapper::workerThread() {
    CloudSink cloud;
    LocalSink local;

    uint32_t seen_gen = config_gen_.load(memory_order_acquire) - 1;
    int64_t next_capture = 0;
    int64_t next_settings = 0;
    int64_t next_ping = 0;
    int64_t next_reconnect = 0;
    int64_t reconnect_ms = 1000;
    int64_t connected_at = 0;
    int64_t last_err_log = 0;
    string last_settings_payload;
    string mode, key, url;

    auto set_status = [&](const string& state, const string& err = string()) {
        lock_guard<mutex> lk(status_mtx_);
        status_state_ = state;
        status_error_ = err;
    };

    while (running_.load(memory_order_relaxed)) {
        int64_t now = now_ms();

        uint32_t gen = config_gen_.load(memory_order_acquire);
        if (gen != seen_gen) {
            seen_gen = gen;
            cloud.close();
            local.close();
            {
                lock_guard<mutex> lk(cfg_mtx_);
                mode = mode_;
                key = key_;
                url = server_url_;
            }
            last_settings_payload.clear();
            reconnect_ms = 1000;
            next_reconnect = 0;
            local_clients_.store(0, memory_order_relaxed);
        }

        if (!enabled_.load(memory_order_relaxed)) {
            set_status("disabled");
            this_thread::sleep_for(chrono::milliseconds(200));
            continue;
        }

        bool slept = false;  // the cloud pump / local poll double as the sleep

        if (mode == "remote") {
            if (!cloud.isOpen() && now >= next_reconnect) {
                set_status("connecting");
                string err;
                if (cloud.connect(url, err)) {
                    cout << "[WebMapper] connected to " << url << endl;
                    // A missing key still connects, but the map rejects the
                    // data - surface that as a warning on the connected state.
                    set_status("connected",
                               key.empty() ? "KrakenPro key is not set" : "");
                    connected_at = now;  // backoff resets once this stays up
                    last_settings_payload.clear();  // force a settings push
                    next_settings = now;
                    next_ping = now + 10000;
                } else {
                    if (now - last_err_log > 30000) {  // throttled fault log
                        cerr << "[WebMapper] cloud connect failed: " << err
                             << " (retrying)" << endl;
                        last_err_log = now;
                    }
                    set_status("error", err);
                    next_reconnect = now + reconnect_ms;
                    reconnect_ms = min<int64_t>(reconnect_ms * 2, 15000);
                }
            }

            if (cloud.isOpen()) {
                vector<string> messages;
                if (!cloud.pump(messages)) {
                    // A server that accepts and then drops us (e.g. closing on
                    // a rejected key) used to reset the backoff on every
                    // connect: a reconnect + log line every second, forever.
                    // Only a connection that stayed up resets it, and drops
                    // share the connect-failure log throttle.
                    if (now - connected_at > 30000) reconnect_ms = 1000;
                    if (now - last_err_log > 30000) {
                        cerr << "[WebMapper] cloud connection lost; reconnecting" << endl;
                        last_err_log = now;
                    }
                    set_status("connecting");
                    next_reconnect = now + reconnect_ms;
                    reconnect_ms = min<int64_t>(reconnect_ms * 2, 15000);
                } else {
                    slept = true;  // pump blocked up to the 100 ms socket timeout
                    for (const string& m : messages) handle_cloud_message(m);

                    // Push settings promptly when receiver state changes
                    // (hash-guarded => silent at steady state); every 10 s
                    // send settings-or-ping as the keep-alive.
                    if (now >= next_settings) {
                        next_settings = now + 1000;
                        string payload = build_settings_payload(key);
                        if (payload != last_settings_payload) {
                            last_settings_payload = payload;
                            cloud.sendText("{\"apikey\":\"" + json_escape(key) +
                                           "\",\"type\":\"settings\",\"data\":" + payload + "}");
                            next_ping = now + 10000;
                        }
                    }
                    if (now >= next_ping) {
                        next_ping = now + 10000;
                        cloud.sendText("{\"apikey\":\"" + json_escape(key) + "\",\"type\":\"ping\"}");
                    }
                }
            }
        } else {  // local
            if (!local.isListening()) {
                string err;
                int port = local_ws_port_.load(memory_order_relaxed);
                if (local.listen(port, err)) {
                    cout << "[WebMapper] local WS broadcast on port " << port << endl;
                    set_status("listening");
                } else {
                    if (now - last_err_log > 30000) {
                        cerr << "[WebMapper] local sink: " << err << endl;
                        last_err_log = now;
                    }
                    set_status("error", err);
                    this_thread::sleep_for(chrono::seconds(2));
                    slept = true;
                }
            }
            if (local.isListening()) {
                local.pump(100);
                slept = true;
                local_clients_.store(local.clientCount(), memory_order_relaxed);
            }
        }

        // --- Record capture tick (200 ms, matching the browser DoA stream) ---
        if (now >= next_capture) {
            next_capture = now + 200;
            bool sink_ready = (mode == "remote") ? cloud.isOpen()
                                                 : (local.clientCount() > 0);
            if (sink_ready && doa_enabled.load(memory_order_relaxed) &&
                !doa_is_calibrating()) {
                StationLocation sl = station_info.resolve();

                // Respect each VFO's own squelch setting: squelch off =>
                // always transmit; squelch on => only while it is open.
                auto info = decimator_manager.getDecimatorInfoList();
                auto squelched = [&](int id) {
                    for (const auto& d : info)
                        if (d.id == id) return d.squelch_enabled && !d.squelch_open;
                    return false;
                };

                for (const DoaRecord& rec : capture_doa_records()) {
                    if (squelched(rec.decimator_id)) continue;
                    int sources = 1;
                    auto dec = decimator_manager.getDecimator(rec.decimator_id);
                    if (dec && dec->music_processor)
                        sources = max(1, dec->music_processor->getNumSignalSources());
                    string record = build_doapost_json(rec, sl, sources);
                    bool sent = false;
                    if (mode == "remote") {
                        sent = cloud.sendText("{\"apikey\":\"" + json_escape(key) +
                                              "\",\"data\":" + record + "}");
                    } else {
                        sent = local.broadcast(record) > 0;
                    }
                    if (sent) records_sent_.fetch_add(1, memory_order_relaxed);
                }
            }
        }

        if (!slept) this_thread::sleep_for(chrono::milliseconds(100));
    }

    cloud.close();
    local.close();
}
