#pragma once

// DNS-rebinding guard for the HTTP/WebSocket servers. A web page on
// attacker.example can re-point its own DNS name at this device's LAN IP;
// the browser then treats the device as same-origin with the attacker's page,
// so an Origin == Host comparison passes (both say attacker.example) and the
// page can drive the UI. The defence is to accept requests only for names this
// device is actually reached by:
//   - an IP literal (IPv4, or bracketed IPv6)
//   - localhost
//   - this machine's hostname, bare or with a common LAN suffix
//     (.local .lan .home .localdomain .home.arpa)
//   - any name in KRAKEN_ALLOWED_HOSTS (comma-separated, e.g. for a reverse
//     proxy or a custom DNS name)
//   - names the program adds at startup (host_add_allowed_names: kraken_doa
//     the DNS names of its HTTPS certificate)
// A request without a Host header (HTTP/1.0 tools) is allowed - browsers
// always send one. Kept identical in heimdall_v2/src/core/ and
// kraken_doa_v2/include/utils/.

#ifdef _WIN32
#include "net_compat.hpp"  // Winsock inet_pton / gethostname (next to this file in both apps)
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace host_check_detail {
// Names the program itself adds at startup (host_add_allowed_names);
// guarded: a server thread may already be checking requests
inline std::mutex& added_mutex() {
    static std::mutex m;
    return m;
}
inline std::vector<std::string>& added_names() {
    static std::vector<std::string> v;
    return v;
}
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!s.empty() && s.back() == '.') s.pop_back();  // FQDN trailing dot
    return s;
}

inline const std::vector<std::string>& allowed_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v{"localhost"};
        char buf[256] = {};
        if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0]) {
            std::string h = lower(buf);
            const size_t dot = h.find('.');
            const std::string bare = (dot == std::string::npos) ? h : h.substr(0, dot);
            v.push_back(h);
            for (const char* suffix : {"", ".local", ".lan", ".home", ".localdomain", ".home.arpa"})
                v.push_back(bare + suffix);
        }
#ifdef _WIN32
        // Windows' gethostname() is the bare computer name, never with its
        // domain: a domain-joined PC reached as pc.corp.example was refused.
        // Its DNS name with the primary suffix is a name it is reached by too.
        {
            char fq[256] = {};
            DWORD n = sizeof(fq);
            if (GetComputerNameExA(ComputerNameDnsFullyQualified, fq, &n) && fq[0]) {
                const std::string f = lower(fq);
                if (std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f);
            }
        }
#endif
        if (const char* env = std::getenv("KRAKEN_ALLOWED_HOSTS")) {
            std::string list(env), item;
            for (size_t i = 0; i <= list.size(); i++) {
                if (i == list.size() || list[i] == ',') {
                    item.erase(0, item.find_first_not_of(" \t"));
                    item.erase(item.find_last_not_of(" \t") + 1);
                    if (!item.empty()) v.push_back(lower(item));
                    item.clear();
                } else {
                    item += list[i];
                }
            }
        }
        return v;
    }();
    return names;
}
}  // namespace host_check_detail

// host: the raw Host header value ("name", "name:port", "1.2.3.4:8070",
// "[::1]:8080").
inline bool host_allowed(std::string_view host) {
    if (host.empty()) return true;
    std::string h(host);
    if (h.front() == '[') {                          // [IPv6]:port - a literal
        const size_t e = h.find(']');
        if (e == std::string::npos) return false;
        in6_addr a6;
        return inet_pton(AF_INET6, h.substr(1, e - 1).c_str(), &a6) == 1;
    }
    const size_t colon = h.rfind(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    h = host_check_detail::lower(h);
    in_addr a4;
    if (inet_pton(AF_INET, h.c_str(), &a4) == 1) return true;
    const auto& names = host_check_detail::allowed_names();
    if (std::find(names.begin(), names.end(), h) != names.end()) return true;
    std::lock_guard<std::mutex> lock(host_check_detail::added_mutex());
    const auto& added = host_check_detail::added_names();
    return std::find(added.begin(), added.end(), h) != added.end();
}

// More names this device is reached by, known only at run time - kraken_doa:
// the DNS names of the HTTPS certificate it serves (an avahi conflict name
// like krakensdr-2.local, KRAKEN_CERT_HOST / KRAKEN_CERT_NAMES, a DNS search
// domain; cert_status.cpp). Call it before the servers listen; heimdall
// doesn't. Exact names only (a wildcard is left out); thread-safe.
inline void host_add_allowed_names(const std::vector<std::string>& more) {
    std::lock_guard<std::mutex> lock(host_check_detail::added_mutex());
    auto& added = host_check_detail::added_names();
    for (const std::string& n : more) {
        const std::string h = host_check_detail::lower(n);
        if (h.empty() || h.find('*') != std::string::npos) continue;
        if (std::find(added.begin(), added.end(), h) == added.end()) added.push_back(h);
    }
}
