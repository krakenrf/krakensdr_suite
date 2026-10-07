#include "geo_http.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace geo {

namespace {
const char* const NOMINATIM_HOST = "nominatim.openstreetmap.org";
const char* const USER_AGENT = "kraken_doa/2 (KrakenSDR incident map; https://github.com/krakenrf/krakensdr_suite)";
constexpr int64_t MIN_GAP_MS = 1100;                  // usage policy: at most 1 request / s
constexpr int64_t CACHE_MS = 24LL * 3600 * 1000;
constexpr size_t CACHE_MAX = 2000;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string url_encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
}  // namespace

bool https_get(const std::string& host, const std::string& path_query, const std::string& user_agent,
               std::string* body, int* status, std::string* err) {
    *status = 0;
    body->clear();
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "443", &hints, &res) != 0 || !res) {
        *err = "no internet connection (cannot resolve " + host + ")";
        return false;
    }
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        timeval tv{15, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { *err = "cannot connect to " + host; return false; }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL* ssl = nullptr;
    auto done = [&](bool ok) {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
        ::close(fd);
        return ok;
    };
    if (!ctx || SSL_CTX_set_default_verify_paths(ctx) != 1) { *err = "TLS setup failed"; return done(false); }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    ssl = SSL_new(ctx);
    if (!ssl) { *err = "TLS setup failed"; return done(false); }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        const long vr = SSL_get_verify_result(ssl);
        *err = vr != X509_V_OK ? "certificate of " + host + " rejected: " + X509_verify_cert_error_string(vr)
                               : "TLS handshake with " + host + " failed";
        return done(false);
    }
    const std::string req = "GET " + path_query + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: " + user_agent +
                            "\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    if (SSL_write(ssl, req.data(), static_cast<int>(req.size())) <= 0) { *err = "sending the request failed"; return done(false); }
    std::string raw;
    char rb[16384];
    for (;;) {
        int n = SSL_read(ssl, rb, sizeof rb);
        if (n <= 0) break;
        raw.append(rb, static_cast<size_t>(n));
        if (raw.size() > (16u << 20)) { *err = "answer too large"; return done(false); }
    }
    const size_t e = raw.find("\r\n\r\n");
    if (e == std::string::npos) { *err = raw.empty() ? "no answer from " + host : "bad HTTP answer"; return done(false); }
    const std::string head = raw.substr(0, e);
    sscanf(head.c_str(), "HTTP/%*s %d", status);
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string data = raw.substr(e + 4);
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        size_t p = 0;
        for (;;) {
            const size_t le = data.find("\r\n", p);
            if (le == std::string::npos) { *err = "the answer was cut off"; return done(false); }
            const size_t len = strtoul(data.substr(p, le - p).c_str(), nullptr, 16);
            if (len == 0) break;
            if (le + 2 + len > data.size()) { *err = "the answer was cut off"; return done(false); }
            body->append(data, le + 2, len);
            p = le + 2 + len + 2;
        }
    } else {
        *body = std::move(data);
    }
    if (*status != 200) {
        *err = host + " answered HTTP " + std::to_string(*status) +
               (*status == 429 || *status == 403 ? " (too many requests - try again later)" : "");
        return done(false);
    }
    return done(true);
}

bool Nominatim::search(const std::string& query, double s, double w, double n, double e, std::string* json,
                       std::string* err, bool* offline) {
    *offline = false;
    char box[128];
    snprintf(box, sizeof box, "%.4f,%.4f,%.4f,%.4f", w, n, e, s);
    const std::string path = "/search?q=" + url_encode(query) + "&format=jsonv2&addressdetails=1&limit=20&viewbox=" +
                             url_encode(box) + "&bounded=1";
    std::lock_guard<std::mutex> lk(mu_);   // one request at a time, in turn
    const int64_t t = now_ms();
    auto c = cache_.find(path);
    if (c != cache_.end() && t - c->second.first < CACHE_MS) {
        *json = c->second.second;
        return true;
    }
    if (t < blocked_until_ms_) {
        *offline = true;
        *err = "the address search asked to wait (busy) - next try in " +
               std::to_string((blocked_until_ms_ - t) / 1000 + 1) + " s";
        return false;
    }
    const int64_t wait = last_ms_ + MIN_GAP_MS - t;
    if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    int status = 0;
    const bool ok = https_get(NOMINATIM_HOST, path, USER_AGENT, json, &status, err);
    last_ms_ = now_ms();
    requests_++;
    if (!ok) {
        const bool busy = status == 429 || status == 502 || status == 503 || status == 504;
        *offline = status == 0 || busy;
        if (busy) {
            backoff_ms_ = std::min<int64_t>(30 * 60 * 1000, backoff_ms_ ? backoff_ms_ * 2 : 60 * 1000);
            blocked_until_ms_ = last_ms_ + backoff_ms_;
        }
        return false;
    }
    backoff_ms_ = 0;
    if (cache_.size() >= CACHE_MAX) {
        for (auto it = cache_.begin(); it != cache_.end();)
            it = t - it->second.first > CACHE_MS ? cache_.erase(it) : std::next(it);
        if (cache_.size() >= CACHE_MAX) cache_.erase(cache_.begin());
    }
    cache_[path] = {last_ms_, *json};
    return true;
}

}  // namespace geo
