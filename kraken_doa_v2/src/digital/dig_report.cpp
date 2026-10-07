#include "digital/dig_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

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
    if (o.map) s += "&m=1";
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
        else if (k == "m") o.map = v == "1";
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

bool Report::event(const std::string& plugin, const std::string& text, double dedup_s) {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t t = now_ms();
    std::string key = plugin + "|" + text;
    auto it = recent_.find(key);
    if (it != recent_.end() && t - it->second < static_cast<int64_t>(dedup_s * 1000)) return false;
    recent_[key] = t;
    if (recent_.size() > 2000) {
        // drop entries older than a minute
        for (auto r = recent_.begin(); r != recent_.end();)
            r = (t - r->second > 60000) ? recent_.erase(r) : std::next(r);
    }
    events_.push_back({++seq_, t, plugin, text});
    while (events_.size() > MAX_EVENTS) events_.pop_front();
    return true;
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

void Report::map_set(MapPoint p) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t t = now_ms();
    p.updated_ms = t;
    p.seq = ++map_seq_;
    std::string key = p.plugin + "|" + p.id;
    auto it = map_.find(key);
    if (it == map_.end() && map_.size() >= MAX_MAP_POINTS) {
        map_expire(t);
        if (map_.size() >= MAX_MAP_POINTS) {
            // full: replace the point updated longest ago
            auto old = std::min_element(map_.begin(), map_.end(), [](const auto& a, const auto& b) {
                return a.second.updated_ms < b.second.updated_ms;
            });
            map_.erase(old);
        }
    }
    map_[key] = std::move(p);
}

void Report::map_remove(const std::string& plugin, const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    map_.erase(plugin + "|" + id);
}

void Report::map_clear() {
    std::lock_guard<std::mutex> lk(mu_);
    map_.clear();
}

size_t Report::map_size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return map_.size();
}

void Report::map_expire(int64_t t) {
    for (auto it = map_.begin(); it != map_.end();)
        it = t - it->second.updated_ms > static_cast<int64_t>(it->second.ttl_s * 1000) ? map_.erase(it) : std::next(it);
}

void Report::table_columns(const std::string& plugin, std::vector<std::string> cols) {
    std::lock_guard<std::mutex> lk(mu_);
    tables_[plugin].cols = std::move(cols);
}

void Report::table_row(const std::string& plugin, const std::string& key, std::vector<std::string> cells) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t t = now_ms();
    auto& rows = tables_[plugin].rows;
    auto it = rows.find(key);
    if (it == rows.end() && rows.size() >= MAX_TABLE_ROWS) {
        // full: drop the rows nobody updated for a while, else the oldest
        for (auto r = rows.begin(); r != rows.end();)
            r = t - r->second.updated_ms > TABLE_ROW_TTL_MS ? rows.erase(r) : std::next(r);
        if (rows.size() >= MAX_TABLE_ROWS)
            rows.erase(std::min_element(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                return a.second.updated_ms < b.second.updated_ms;
            }));
    }
    rows[key] = {std::move(cells), t};
}

void Report::table_remove(const std::string& plugin, const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tables_.find(plugin);
    if (it != tables_.end()) it->second.rows.erase(key);
}

void Report::table_clear() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : tables_) kv.second.rows.clear();
}

std::string Report::tables_json() const {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t t = now_ms();
    std::string o = "{";
    bool ft = true;
    for (const auto& kv : tables_) {
        if (kv.second.cols.empty() && kv.second.rows.empty()) continue;
        o += std::string(ft ? "" : ",") + "\"" + json_escape(kv.first) + "\":{\"cols\":[";
        ft = false;
        for (size_t i = 0; i < kv.second.cols.size(); i++)
            o += std::string(i ? "," : "") + "\"" + json_escape(kv.second.cols[i]) + "\"";
        o += "],\"rows\":[";
        bool fr = true;
        for (const auto& r : kv.second.rows) {
            const int64_t age = t - r.second.updated_ms;
            if (age > TABLE_ROW_TTL_MS) continue;
            o += std::string(fr ? "" : ",") + "[\"" + json_escape(r.first) + "\"," + std::to_string(age / 1000);
            fr = false;
            for (const auto& c : r.second.cells) o += ",\"" + json_escape(c) + "\"";
            o += "]";
        }
        o += "]}";
    }
    return o + "}";
}

uint64_t Report::map_json(int vfo, uint64_t after, std::string& pts, std::string& keys) {
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t t = now_ms();
    map_expire(t);
    const std::string v = std::to_string(vfo);
    auto num = [](double x, const char* f) {
        if (!is_finite_value(x)) return std::string("null");   // -Ofast: no std::isfinite
        char b[32];
        snprintf(b, sizeof b, f, x);
        return std::string(b);
    };
    for (const auto& kv : map_) {
        const MapPoint& p = kv.second;
        if (!keys.empty()) keys += ",";
        keys += "\"" + v + "|" + json_escape(kv.first) + "\"";
        if (p.seq <= after) continue;
        if (!pts.empty()) pts += ",";
        pts += "{\"v\":" + v + ",\"p\":\"" + json_escape(p.plugin) + "\",\"i\":\"" + json_escape(p.id) +
               "\",\"la\":" + num(p.lat, "%.6f") + ",\"lo\":" + num(p.lon, "%.6f") + ",\"l\":\"" +
               json_escape(p.label) + "\",\"k\":\"" + json_escape(p.kind) + "\",\"h\":" + num(p.heading, "%.1f") +
               ",\"a\":" + num(p.alt_m, "%.0f") + ",\"s\":" + num(p.speed_kmh, "%.1f") + ",\"x\":\"" +
               json_escape(p.info) + "\",\"age\":" + std::to_string((t - p.updated_ms) / 1000) + ",\"ttl\":" +
               num(p.ttl_s, "%.0f") + "}";
    }
    return map_seq_;
}

}  // namespace dig
