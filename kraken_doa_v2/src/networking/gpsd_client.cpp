// strptime()/timegm() are glibc extensions gated behind _GNU_SOURCE. g++
// predefines it for C++, but define it defensively (before any include) in case
// a stricter front-end doesn't.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "networking/gpsd_client.hpp"
#include "config.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <iostream>

namespace {

// ---- Minimal JSON field extraction --------------------------------------
// gpsd emits one flat JSON object per line. We only need a handful of scalar
// fields, so rather than pull in a JSON library we scan for "key": and read the
// value. Searching for the full "key": token (with the leading quote) avoids
// matching a key that is a suffix of another, e.g. "track" vs "magtrack".

bool json_num(const std::string& s, const char* key, double& out) {
    std::string pat = "\"";
    pat += key;
    pat += "\":";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;  // not a number (e.g. null)
    out = v;
    return true;
}

bool json_str(const std::string& s, const char* key, std::string& out) {
    std::string pat = "\"";
    pat += key;
    pat += "\":\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    size_t e = s.find('"', p);
    if (e == std::string::npos) return false;
    out = s.substr(p, e - p);
    return true;
}

int count_occurrences(const std::string& s, const char* needle) {
    int n = 0;
    size_t len = std::strlen(needle);
    for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + len))
        ++n;
    return n;
}

// gpsd "time" is ISO8601 UTC, e.g. "2026-06-16T12:34:56.000Z". Returns epoch ms,
// or 0 if it can't be parsed.
int64_t parse_iso8601_ms(const std::string& t) {
    struct tm tm{};
    if (!strptime(t.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return 0;
    time_t secs = timegm(&tm);  // input is UTC
    if (secs == (time_t)-1) return 0;
    double frac = 0.0;
    size_t dot = t.find('.');
    if (dot != std::string::npos) frac = std::strtod(t.c_str() + dot, nullptr);
    return static_cast<int64_t>(secs) * 1000 + static_cast<int64_t>(frac * 1000.0);
}

// Initial great-circle bearing from (lat1,lon1) to (lat2,lon2), degrees [0,360).
double bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    constexpr double D2R = M_PI / 180.0;
    double phi1 = lat1 * D2R, phi2 = lat2 * D2R;
    double dlon = (lon2 - lon1) * D2R;
    double y = std::sin(dlon) * std::cos(phi2);
    double x = std::cos(phi1) * std::sin(phi2) -
               std::sin(phi1) * std::cos(phi2) * std::cos(dlon);
    double b = std::atan2(y, x) / D2R;
    if (b < 0.0) b += 360.0;
    return b;
}

// Approximate ground distance between two fixes in metres (equirectangular —
// plenty accurate over the small steps between consecutive GPS samples).
double approx_dist_m(double lat1, double lon1, double lat2, double lon2) {
    constexpr double D2R = M_PI / 180.0;
    constexpr double R = 6371000.0;
    double x = (lon2 - lon1) * D2R * std::cos((lat1 + lat2) * 0.5 * D2R);
    double y = (lat2 - lat1) * D2R;
    return std::sqrt(x * x + y * y) * R;
}

// gpsd endpoint: config defaults, overridable via env for remote/dev setups.
std::string gpsd_host() {
    if (const char* e = std::getenv("KRAKEN_GPSD_HOST"); e && *e) return e;
    return GPSD_HOST;
}
int gpsd_port() {
    if (const char* e = std::getenv("KRAKEN_GPSD_PORT"); e && *e) {
        int p = std::atoi(e);
        if (p > 0 && p < 65536) return p;
    }
    return GPSD_PORT;
}

// Open a TCP connection to gpsd with a bounded connect timeout. Returns the fd
// (with a read timeout already applied) or -1 on failure.
int connect_gpsd(const char* host, int port) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;

        // Non-blocking connect with a select() timeout so an unreachable host
        // can't wedge the thread.
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;  // connected immediately (typical for localhost)
        }
        if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval tv{2, 0};  // 2s connect timeout
            if (select(fd + 1, nullptr, &wset, nullptr, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;  // connected
                }
            }
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd >= 0) {
        // Read timeout lets the read loop poll the shutdown flag and notice a
        // silently dead daemon.
        struct timeval rtv{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        // Generous receive buffer so a CPU-starved reader (the kraken_doa runs
        // ~25 hot threads) doesn't overflow and get dropped by gpsd, and so one
        // read can drain a whole multi-report backlog (SKY frames are ~2.7 KB).
        int rcvbuf = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    return fd;
}

}  // namespace

void GpsdClient::start() {
    if (!GPS_ENABLED) {
        std::cout << "[GPS] gpsd support disabled in config" << std::endl;
        return;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;  // already running
    thread_ = std::thread(&GpsdClient::run, this);
    std::cout << "[GPS] gpsd client started (" << gpsd_host() << ":" << gpsd_port() << ")" << std::endl;
}

void GpsdClient::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

GpsFix GpsdClient::get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fix_;
}

void GpsdClient::reset_disconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    fix_ = GpsFix{};  // all zero / disconnected
    have_last_pos_ = false;
}

void GpsdClient::run() {
    using namespace std::chrono_literals;

    std::string host = gpsd_host();
    int port = gpsd_port();

    while (running_.load()) {
        int fd = connect_gpsd(host.c_str(), port);
        if (fd < 0) {
            reset_disconnected();
            // gpsd not up yet — wait quietly and retry (poll the flag so
            // shutdown isn't delayed).
            for (int i = 0; i < 20 && running_.load(); ++i)
                std::this_thread::sleep_for(100ms);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            fix_.connected = true;
        }

        const char* watch = "?WATCH={\"enable\":true,\"json\":true}\n";
        if (send(fd, watch, std::strlen(watch), MSG_NOSIGNAL) < 0) {
            ::close(fd);
            reset_disconnected();
            continue;
        }

        std::string buf;
        char tmp[16384];  // large enough to drain a full backlog per read
        while (running_.load()) {
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                buf.append(tmp, n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    process_line(buf.substr(0, nl));
                    buf.erase(0, nl + 1);
                }
                if (buf.size() > 65536) buf.clear();  // runaway guard
            } else if (n == 0) {
                break;  // gpsd closed the connection
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;  // read timeout — loop to re-check running_
                break;         // real error
            }
        }

        ::close(fd);
        reset_disconnected();
    }
}

void GpsdClient::process_line(const std::string& line) {
    std::string cls;
    if (!json_str(line, "class", cls)) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (cls == "DEVICES") {
        // The device list is sent on connect and whenever it changes. A device
        // object always carries a "path", so its presence => a GPS is attached.
        fix_.device_present = (line.find("\"path\"") != std::string::npos);
    } else if (cls == "DEVICE") {
        fix_.device_present = true;  // attach event
    } else if (cls == "TPV") {
        fix_.device_present = true;
        double v;
        int mode = 0;
        if (json_num(line, "mode", v)) mode = static_cast<int>(v);
        fix_.mode = mode;

        std::string t;
        if (json_str(line, "time", t)) {
            int64_t ms = parse_iso8601_ms(t);
            if (ms) fix_.timestamp_ms = ms;
        }

        if (mode >= 2) {
            double lat = fix_.lat, lon = fix_.lon;
            bool got_lat = json_num(line, "lat", lat);
            bool got_lon = json_num(line, "lon", lon);
            if (got_lat) fix_.lat = lat;
            if (got_lon) fix_.lon = lon;

            double a;
            if (json_num(line, "altMSL", a) || json_num(line, "alt", a) ||
                json_num(line, "altHAE", a))
                fix_.alt = a;

            if (json_num(line, "speed", v)) fix_.speed = v;

            // Movement-derived heading: prefer gpsd's course-over-ground; fall
            // back to a bearing between fixes for receivers that omit "track".
            double trk;
            if (json_num(line, "track", trk)) {
                fix_.track = trk;
                fix_.track_valid = true;
            } else if (have_last_pos_ && got_lat && got_lon) {
                double d = approx_dist_m(last_lat_, last_lon_, fix_.lat, fix_.lon);
                if (d >= 3.0 && fix_.speed >= 0.5) {  // moved enough to trust it
                    fix_.track = bearing_deg(last_lat_, last_lon_, fix_.lat, fix_.lon);
                    fix_.track_valid = true;
                }
            }

            if (got_lat && got_lon) {
                last_lat_ = fix_.lat;
                last_lon_ = fix_.lon;
                have_last_pos_ = true;
            }
        } else {
            // No fix: zero the position so consumers report 0's, and drop the
            // movement heading (we can't infer one without a fix).
            fix_.lat = 0.0;
            fix_.lon = 0.0;
            fix_.alt = 0.0;
            fix_.speed = 0.0;
            fix_.track = 0.0;
            fix_.track_valid = false;
            have_last_pos_ = false;
        }
    } else if (cls == "ATT") {
        // Attitude report from a compass/IMU-equipped receiver.
        double h;
        if (json_num(line, "heading", h)) {
            fix_.compass_heading = h;
            fix_.has_compass = true;
        }
    } else if (cls == "SKY") {
        // gpsd emits more than one SKY per cycle: a full one (nSat/uSat + a
        // "satellites" array) and a DOP-only one with neither. Only update the
        // counts from a report that actually carries satellite info, otherwise
        // the DOP-only SKY would clobber the real values back to 0.
        double v;
        bool has_sats = line.find("\"satellites\":") != std::string::npos;
        if (json_num(line, "uSat", v))
            fix_.sats_used = static_cast<int>(v);
        else if (has_sats)
            fix_.sats_used = count_occurrences(line, "\"used\":true");

        if (json_num(line, "nSat", v))
            fix_.sats_visible = static_cast<int>(v);
        else if (has_sats)
            fix_.sats_visible = count_occurrences(line, "\"used\":true") +
                                count_occurrences(line, "\"used\":false");
    }
}
