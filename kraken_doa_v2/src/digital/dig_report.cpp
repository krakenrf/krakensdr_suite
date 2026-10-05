#include "digital/dig_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "utils/json_escape.hpp"

namespace dig {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::AUTO: return "AUTO";
        case Mode::PLUGIN: return "PLUGIN";
        default: return "OFF";
    }
}

bool valid_plugin_id(const std::string& id) {
    if (id.empty() || id.size() > 32 || id == "sdk" || id == "lib") return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

std::string mode_string(Mode m, const std::string& plugin_id) {
    if (m == Mode::PLUGIN) return "PLUGIN:" + plugin_id;
    return mode_name(m);
}

bool parse_mode_string(const std::string& s, Mode* m, std::string* plugin_id) {
    plugin_id->clear();
    if (s == "OFF") { *m = Mode::OFF; return true; }
    if (s == "AUTO") { *m = Mode::AUTO; return true; }
    std::string id;
    if (s.rfind("PLUGIN:", 0) == 0) {
        id = s.substr(7);
    } else {
        // the decoders that used to be built in (settings saved before they
        // became plugins, and the old wire names)
        static const std::pair<const char*, const char*> legacy[] = {
            {"P25", "p25"}, {"DMR", "dmr"}, {"TETRA", "tetra"}, {"DSTAR", "dstar"},
            {"D-STAR", "dstar"}, {"NXDN", "nxdn"}, {"MPT1327", "mpt1327"}};
        for (const auto& l : legacy)
            if (s == l.first) id = l.second;
        if (id.empty()) return false;
    }
    if (!valid_plugin_id(id)) return false;
    *m = Mode::PLUGIN;
    *plugin_id = id;
    return true;
}

bool valid_option_key(const std::string& k) {
    size_t dot = k.find('.');
    if (dot == std::string::npos || !valid_plugin_id(k.substr(0, dot))) return false;
    std::string o = k.substr(dot + 1);
    if (o.empty() || o.size() > 32) return false;
    for (char c : o)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

namespace {
std::string pct_encode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') o += static_cast<char>(c);
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
std::string pct_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            o += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}
}  // namespace

std::string options_to_string(const Options& o) {
    std::string s = std::string("v=") + (o.verbose ? "1" : "0") + "&i=" + (o.invert ? "1" : "0");
    for (const auto& kv : o.plugin) s += "&" + kv.first + "=" + pct_encode(kv.second);
    return s;
}

Options options_from_string(const std::string& s) {
    Options o;
    if (s.find('=') == std::string::npos) {
        // old form: verbose/dmr_slot/p25_nac/invert
        int a = 0, b = 0, c = -1, d = 0;
        if (sscanf(s.c_str(), "%d/%d/%d/%d", &a, &b, &c, &d) >= 3) {
            o.verbose = a != 0;
            o.invert = d != 0;
            if (b >= 1 && b <= 2) o.plugin["dmr.slot"] = std::to_string(b);
            if (c >= 0 && c <= 0xFFF) {
                char h[8];
                snprintf(h, sizeof h, "%03X", c);
                o.plugin["p25.nac"] = h;
            }
        }
        return o;
    }
    std::stringstream ss(s);
    std::string kv;
    while (std::getline(ss, kv, '&')) {
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = pct_decode(kv.substr(eq + 1));
        if (k == "v") o.verbose = v == "1";
        else if (k == "i") o.invert = v == "1";
        else if (valid_option_key(k) && v.size() <= 100) o.plugin[k] = v;
    }
    return o;
}

void Report::set(const std::string& plugin, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& v = facts_[plugin];
    int64_t t = now_ms();
    for (auto& f : v) {
        if (f.key == key) { f.value = value; f.updated_ms = t; return; }
    }
    if (v.size() < 64) v.push_back({key, value, t});
}

void Report::erase(const std::string& plugin, const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& v = facts_[plugin];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Fact& f) { return f.key == key; }), v.end());
}

void Report::clear(const std::string& plugin) {
    std::lock_guard<std::mutex> lk(mu_);
    facts_.erase(plugin);
}

void Report::clear_all() {
    std::lock_guard<std::mutex> lk(mu_);
    facts_.clear();
    recent_.clear();
}

void Report::set_label(const std::string& plugin, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    labels_[plugin] = name;
}

void Report::event(const std::string& plugin, const std::string& text, double dedup_s) {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t t = now_ms();
    std::string key = plugin + "|" + text;
    auto it = recent_.find(key);
    if (it != recent_.end() && t - it->second < static_cast<int64_t>(dedup_s * 1000)) return;
    recent_[key] = t;
    if (recent_.size() > 2000) {
        // drop entries older than a minute
        for (auto r = recent_.begin(); r != recent_.end();)
            r = (t - r->second > 60000) ? recent_.erase(r) : std::next(r);
    }
    events_.push_back({++seq_, t, plugin, text});
    while (events_.size() > MAX_EVENTS) events_.pop_front();
}

std::string Report::info_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream o;
    o << "{";
    int64_t t = now_ms();
    bool fp = true;
    for (const auto& p : facts_) {
        if (p.second.empty()) continue;
        o << (fp ? "" : ",") << "\"" << json_escape(p.first) << "\":[";
        fp = false;
        bool first = true;
        for (const auto& f : p.second) {
            o << (first ? "" : ",") << "[\"" << json_escape(f.key) << "\",\"" << json_escape(f.value) << "\","
              << (t - f.updated_ms) / 1000 << "]";
            first = false;
        }
        o << "]";
    }
    o << "}";
    return o.str();
}

std::string Report::events_json(uint64_t after, size_t max) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream o;
    o << "[";
    size_t start = events_.size();
    size_t count = 0;
    while (start > 0 && events_[start - 1].seq > after && count < max) { start--; count++; }
    bool first = true;
    for (size_t i = start; i < events_.size(); i++) {
        const auto& e = events_[i];
        if (!first) o << ",";
        first = false;
        auto l = labels_.find(e.source);
        const std::string& label = l != labels_.end() ? l->second : e.source;
        o << "{\"s\":" << e.seq << ",\"t\":" << e.time_ms << ",\"src\":\"" << json_escape(e.source) << "\",\"p\":\""
          << json_escape(label) << "\",\"m\":\"" << json_escape(e.text) << "\"}";
    }
    o << "]";
    return o.str();
}

uint64_t Report::last_seq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seq_;
}

}  // namespace dig
