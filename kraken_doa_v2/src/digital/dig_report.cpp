#include "digital/dig_report.hpp"

#include <algorithm>
#include <chrono>
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
        case Mode::P25: return "P25";
        case Mode::DMR: return "DMR";
        case Mode::TETRA: return "TETRA";
        case Mode::DSTAR: return "DSTAR";
        case Mode::NXDN: return "NXDN";
        case Mode::MPT1327: return "MPT1327";
        default: return "OFF";
    }
}

Mode mode_from_string(const std::string& s) {
    if (s == "AUTO") return Mode::AUTO;
    if (s == "P25") return Mode::P25;
    if (s == "DMR") return Mode::DMR;
    if (s == "TETRA") return Mode::TETRA;
    if (s == "DSTAR" || s == "D-STAR") return Mode::DSTAR;
    if (s == "NXDN") return Mode::NXDN;
    if (s == "MPT1327") return Mode::MPT1327;
    return Mode::OFF;
}

void Report::set(Mode proto, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& v = facts_[proto];
    int64_t t = now_ms();
    for (auto& f : v) {
        if (f.key == key) { f.value = value; f.updated_ms = t; return; }
    }
    v.push_back({key, value, t});
}

void Report::erase(Mode proto, const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& v = facts_[proto];
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Fact& f) { return f.key == key; }), v.end());
}

void Report::clear(Mode proto) {
    std::lock_guard<std::mutex> lk(mu_);
    facts_.erase(proto);
}

void Report::clear_all() {
    std::lock_guard<std::mutex> lk(mu_);
    facts_.clear();
    recent_.clear();
}

void Report::event(Mode proto, const std::string& text, double dedup_s) {
    std::lock_guard<std::mutex> lk(mu_);
    int64_t t = now_ms();
    std::string key = std::string(mode_name(proto)) + "|" + text;
    auto it = recent_.find(key);
    if (it != recent_.end() && t - it->second < static_cast<int64_t>(dedup_s * 1000)) return;
    recent_[key] = t;
    if (recent_.size() > 2000) {
        // drop entries older than a minute
        for (auto r = recent_.begin(); r != recent_.end();)
            r = (t - r->second > 60000) ? recent_.erase(r) : std::next(r);
    }
    events_.push_back({++seq_, t, proto, text});
    while (events_.size() > MAX_EVENTS) events_.pop_front();
}

std::string Report::info_json(Mode proto) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream o;
    o << "[";
    auto it = facts_.find(proto);
    if (it != facts_.end()) {
        int64_t t = now_ms();
        bool first = true;
        for (const auto& f : it->second) {
            if (!first) o << ",";
            first = false;
            o << "[\"" << json_escape(f.key) << "\",\"" << json_escape(f.value) << "\","
              << (t - f.updated_ms) / 1000 << "]";
        }
    }
    o << "]";
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
        o << "{\"s\":" << e.seq << ",\"t\":" << e.time_ms << ",\"p\":\"" << mode_name(e.proto)
          << "\",\"m\":\"" << json_escape(e.text) << "\"}";
    }
    o << "]";
    return o.str();
}

uint64_t Report::last_seq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seq_;
}

}  // namespace dig
