// The web UI's certificate as the page shows it - see cert_status.hpp and
// docs/dev/https-certificate.md.
#include "cert_status.hpp"

#include "playback.hpp"
#include "utils/host_check.hpp"
#include "utils/json_escape.hpp"
#include "utils/tls_compat.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using std::string;
using std::vector;

constexpr int EXPIRES_SOON_DAYS = 30;
constexpr auto REFRESH = std::chrono::seconds(60);
constexpr size_t MAX_RETIRED = 3;   // the newest; older ones are long gone from devices

struct State {
    std::mutex mu;
    bool ready = false;
    // what this run serves (read once: the listener loaded these files)
    tlscert::Inspect ins;
    string download;
    // re-checked every 60 s by the worker
    tlscert::Host host;
    vector<string> ips_now;     // private IPv4 addresses (what a wide certificate names)
    vector<string> not_covered; // wanted names outside the served CA: left out (fine rule 18)
    bool scope_next_wide = false;
    bool new_ca_next = false;   // cert_options' new_ca=1 (fine rule 29)
    uint32_t scope_gen = 0;     // CERT_SCOPE: / CERT_NEW_CA: writes (a refresh that read the file before one doesn't apply it)
    // the last CERT_SCOPE: refusal (the page that asked shows it)
    string scope_error;
    uint32_t scope_seq = 0;
    string json;
};
// never destroyed: the worker thread is detached and may outlive main()
State& st() {
    static State* s = new State;
    return *s;
}

void str_array(std::ostringstream& o, const vector<string>& v) {
    o << '[';
    for (size_t i = 0; i < v.size(); i++) o << (i ? ",\"" : "\"") << json_escape(v[i]) << '"';
    o << ']';
}

// caller holds mu
void rebuild() {
    State& s = st();
    const tlscert::Inspect& in = s.ins;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t secs_left = in.server.not_after - now;
    // floor, also below zero (an expired certificate: -1 and less)
    const int64_t days_left = secs_left >= 0 ? secs_left / 86400 : -((-secs_left + 86399) / 86400);
    // the served CA's scope: "narrow" / "wide" / "none" (no name constraints:
    // the next start makes a new CA, as for any other mismatch)
    const string served = in.has_ca ? in.ca.scope : "";
    const bool served_wide = served == "wide";

    vector<string> notices;
    if (in.present && secs_left < static_cast<int64_t>(EXPIRES_SOON_DAYS) * 86400) notices.push_back("expires_soon");
    // a restart re-issues a managed one; a certificate of the user's own
    // names what its owner wants
    if (in.managed && s.host.usable &&
        std::find(in.server.dns.begin(), in.server.dns.end(), s.host.name) == in.server.dns.end())
        notices.push_back("name_changed");
    if (in.managed && served_wide &&
        std::any_of(s.ips_now.begin(), s.ips_now.end(), [&](const string& ip) {
            return std::find(in.server.ips.begin(), in.server.ips.end(), ip) == in.server.ips.end();
        }))
        notices.push_back("addresses_changed");
    if (in.managed && in.has_ca && served != (s.scope_next_wide ? "wide" : "narrow")) notices.push_back("restart_pending");
    if (in.managed && in.has_ca && !s.not_covered.empty()) notices.push_back("names_not_covered");
    // fine rule 29: a new CA asked for, made at the next start
    if (in.managed && s.new_ca_next) notices.push_back("new_ca_pending");
    // fine rule 23: the CA starts more than a day after this clock - kept
    // (the clock is not known to be right: a Pi without a real-time clock)
    if (in.managed && in.has_ca && in.ca.not_before > now + 86400) notices.push_back("clock_suspect");

    std::ostringstream o;
    o << "{\"managed\":" << (in.managed ? "true" : "false");
    o << ",\"scope\":" << (in.has_ca ? "\"" + served + "\"" : string("null"));
    o << ",\"scope_next\":" << (s.scope_next_wide ? "\"wide\"" : "\"narrow\"");
    o << ",\"host\":\"" << json_escape(s.host.raw) << "\",\"host_ok\":" << (s.host.usable ? "true" : "false");
    // the name avahi announces when it is another (an mDNS conflict: fine rule 24)
    o << ",\"host_conflict\":\"" << json_escape(s.host.avahi) << "\",\"new_ca_next\":" << (s.new_ca_next ? "true" : "false");
    if (in.has_ca) {
        o << ",\"ca\":{\"name\":\"" << json_escape(in.ca.cn) << "\",\"sha256\":\"" << in.ca.sha256 << "\",\"sha1\":\""
          << in.ca.sha1 << "\",\"not_after\":" << in.ca.not_after << "}";
    } else {
        o << ",\"ca\":null";
    }
    if (in.present) {
        o << ",\"server\":{\"name\":\"" << json_escape(in.server.cn) << "\",\"org\":\"" << json_escape(in.server.org)
          << "\",\"names\":";
        str_array(o, in.server.dns);
        o << ",\"ips\":";
        str_array(o, in.server.ips);
        o << ",\"not_after\":" << in.server.not_after << ",\"days_left\":" << days_left << "}";
    } else {
        o << ",\"server\":null";
    }
    o << ",\"ips_now\":";
    str_array(o, s.ips_now);
    o << ",\"names_not_covered\":";
    str_array(o, s.not_covered);
    o << ",\"retired\":[";
    for (size_t i = 0; i < in.retired.size() && i < MAX_RETIRED; i++)
        o << (i ? "," : "") << "{\"name\":\"" << json_escape(in.retired[i].cert.cn) << "\",\"sha256\":\""
          << in.retired[i].cert.sha256 << "\",\"date\":\"" << in.retired[i].date << "\"}";
    o << "],\"notices\":";
    str_array(o, notices);
    o << ",\"scope_error\":\"" << json_escape(s.scope_error) << "\",\"scope_seq\":" << s.scope_seq << "}";
    s.json = o.str();
}

// the slow part (avahi's tools, getifaddrs, a file) without the lock
void refresh() {
    State& s = st();
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        gen = s.scope_gen;
    }
    tlscert::Host h = tlscert::host_name();
    vector<string> ips = tlscert::private_ipv4();
    const bool wide = tlscert::read_scope_wide("");   // also an edit by hand
    const bool new_ca = tlscert::read_new_ca("");
    vector<string> wanted = tlscert::wanted_names(h);
    const vector<string> own = tlscert::own_names(h);
    std::lock_guard<std::mutex> lk(s.mu);
    // fine rule 18: a wanted name the served CA doesn't permit (a search
    // domain of another network, a KRAKEN_CERT_NAMES name added later) is
    // left out of the certificate - a new CA (--new-ca) takes it in. The own
    // names outside it make a new CA at the next start (name_changed). A CA
    // without DNS or IP subtrees or serverAuth, one of another scope than
    // cert_options', one expiring within a year, one an own name is outside
    // of and a new CA asked for are replaced at the next start - with every
    // name: then nothing is reported (fine rule 34).
    vector<string> nc;
    const vector<string>& perm = s.ins.ca.permitted_dns;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const bool replaced = !s.ins.has_ca || perm.empty() || !s.ins.ca.has_ip_subtree || !s.ins.ca.server_auth_only ||
                          new_ca || s.ins.ca.scope != (wide ? "wide" : "narrow") ||
                          s.ins.ca.not_after < now + 365LL * 86400 ||
                          std::any_of(own.begin(), own.end(), [&](const string& n) { return !tlscert::name_permitted(perm, n); });
    if (s.ins.managed && !replaced)
        for (const string& n : wanted)
            if (std::find(own.begin(), own.end(), n) == own.end() && !tlscert::name_permitted(perm, n)) nc.push_back(n);
    s.not_covered = std::move(nc);
    s.host = std::move(h);
    s.ips_now = std::move(ips);
    if (gen == s.scope_gen) {
        s.scope_next_wide = wide;
        s.new_ca_next = new_ca;
    }
    rebuild();
}

}  // namespace

namespace certstatus {

void init() {
    State& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.ready) return;
        s.ready = true;
        s.ins = tlscert::inspect("");
        // fine rule 19: the CA certificate re-encoded from the parsed one -
        // never the file's bytes, which may hold a private key too (a combined
        // PEM); with no CA, the certificate the server sends (its first)
        s.download = s.ins.has_ca ? s.ins.ca.pem : s.ins.server.pem;
        s.ins.server.pem.clear();
        s.ins.ca.pem.clear();
    }
    refresh();
    {
        // Fine rule 20: names this device is reached by that the host
        // allow-list (gethostname-based) doesn't know - of the served
        // certificate's names only the host's own (<host>, <host>.local: the
        // avahi conflict name, KRAKEN_CERT_HOST), KRAKEN_CERT_NAMES' and
        // <host>.<d> for the suffixes the allow-list trusts anyway. Not a
        // name from an arbitrary DHCP search domain: a network that says
        // `search attacker.example` must not open DNS rebinding from it.
        State& c = st();
        std::lock_guard<std::mutex> lk(c.mu);
        vector<string> ok = tlscert::own_names(c.host), allow;
        if (!c.host.avahi.empty()) {   // fine rule 24's extra name
            ok.push_back(c.host.avahi);
            ok.push_back(c.host.avahi + ".local");
        }
        for (const string& n : tlscert::env_names()) ok.push_back(n);
        if (c.host.usable)
            for (const char* d : {".lan", ".home", ".localdomain", ".home.arpa"}) ok.push_back(c.host.name + d);
        for (const string& n : c.ins.server.dns)
            if (std::find(ok.begin(), ok.end(), n) != ok.end()) allow.push_back(n);
        host_add_allowed_names(allow);
    }
    {
        State& c = st();
        std::lock_guard<std::mutex> lk(c.mu);
        const tlscert::Inspect& in = c.ins;
        if (in.present)
            // fine rule 26: the CA's SHA-256 where only the owner sees it - to
            // compare with what a device shows before trusting it
            std::cout << "HTTPS certificate: " << (in.managed ? "" : "not managed, ") << "CN=" << in.server.cn
                      << (in.has_ca ? ", CA \"" + in.ca.cn + "\" (" + in.ca.scope + " scope), SHA-256 " + in.ca.sha256 : "")
                      << std::endl;
    }
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(REFRESH);
            refresh();
        }
    }).detach();
}

string json() {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.json;
}

bool download(string* pem) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.download.empty()) return false;
    *pem = s.download;
    return true;
}

string set_scope(std::string_view value, bool allowed, string* applied) {
    State& s = st();
    string why;
    const bool wide = value == "wide";
    if (!allowed) {
        why = "the certificate scope is set from the page only";
    } else if (value != "wide" && value != "narrow") {
        why = "scope must be narrow or wide";
    } else if (playback::active()) {
        why = "Playback: the certificate is the real receiver's - stop the playback to change its scope";
    } else {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.ins.present && !s.ins.managed)
            why = "a certificate of your own is in use (not managed): the scope applies to the receiver's own CA only";
    }
    string err;
    if (why.empty()) {
        bool keep_new_ca;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            keep_new_ca = s.new_ca_next;
        }
        if (!tlscert::write_options("", wide, keep_new_ca, &err)) why = err;
    }
    std::lock_guard<std::mutex> lk(s.mu);
    if (allowed) {
        // a page's request: it shows the outcome (scope_seq tells it apart)
        s.scope_error = why;
        s.scope_seq++;
    }
    if (why.empty()) {
        s.scope_next_wide = wide;
        s.scope_gen++;
        *applied = string("CERT_SCOPE:") + (wide ? "wide" : "narrow");
        std::cout << "HTTPS certificate: scope " << (wide ? "wide" : "narrow") << " from the next start (cert_options)"
                  << std::endl;
    } else {
        std::cout << "HTTPS certificate: CERT_SCOPE:" << string(value.substr(0, 16)) << " refused (" << why << ")"
                  << std::endl;
    }
    if (s.ready) rebuild();
    return why;
}

string set_new_ca(std::string_view value, bool allowed, string* applied) {
    State& s = st();
    string why;
    const bool on = value == "1";
    if (!allowed) {
        why = "a new certificate authority is asked for from the page only";
    } else if (value != "1" && value != "0") {
        why = "CERT_NEW_CA: takes 1 or 0";
    } else if (playback::active()) {
        why = "Playback: the certificate is the real receiver's - stop the playback to ask for a new CA";
    } else {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.ins.present && !s.ins.managed)
            why = "a certificate of your own is in use (not managed): the receiver makes no CA then";
    }
    string err;
    if (why.empty()) {
        bool wide;
        {
            std::lock_guard<std::mutex> lk(s.mu);
            wide = s.scope_next_wide;
        }
        if (!tlscert::write_options("", wide, on, &err)) why = err;
    }
    std::lock_guard<std::mutex> lk(s.mu);
    if (allowed) {
        s.scope_error = why;
        s.scope_seq++;
    }
    if (why.empty()) {
        s.new_ca_next = on;
        s.scope_gen++;
        *applied = string("CERT_NEW_CA:") + (on ? "1" : "0");
        std::cout << "HTTPS certificate: " << (on ? "a new certificate authority at the next start" : "no new certificate authority")
                  << " (cert_options)" << std::endl;
    } else {
        std::cout << "HTTPS certificate: CERT_NEW_CA:" << string(value.substr(0, 8)) << " refused (" << why << ")" << std::endl;
    }
    if (s.ready) rebuild();
    return why;
}

}  // namespace certstatus
