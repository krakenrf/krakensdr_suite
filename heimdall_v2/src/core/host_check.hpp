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
// A request without a Host header (HTTP/1.0 tools) is allowed - browsers
// always send one. Kept identical in heimdall_v2/src/core/ and
// kraken_doa_v2/include/utils/.

#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace host_check_detail {
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
    return std::find(names.begin(), names.end(), h) != names.end();
}
