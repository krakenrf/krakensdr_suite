#include "map_markers.hpp"

#include "geo_address.hpp"   // geo::json_parse
#include "utils/json_escape.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace markers {
namespace {

constexpr const char* MARKER_FILE = "map_markers.json";

struct Freq {
    int64_t hz = 0;
    std::string label;
};
struct Marker {
    std::string id;
    double lat = 0, lon = 0;
    std::string name, notes;
    std::vector<Freq> freqs;
    int64_t t = 0;   // last change, ms since the epoch
};

std::mutex mu;
std::vector<Marker> list;   // in the order they were added

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// The page makes ids of letters / digits (random); nothing else is accepted,
// so an id is safe in a command, a file and an HTML attribute
bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 24) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    return true;
}

// Text cut to max bytes on a character boundary; invalid UTF-8 and control
// characters dropped (notes keep line breaks and tabs)
std::string clean(const std::string& s, size_t max, bool multiline) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            const size_t n = json_utf8_len(s, i);
            if (n == 0) { i++; continue; }
            if (o.size() + n > max) break;
            o.append(s, i, n);
            i += n;
            continue;
        }
        i++;
        if (c == 0x7f || (c < 0x20 && !(multiline && (c == '\n' || c == '\t')))) continue;
        if (o.size() + 1 > max) break;
        o += static_cast<char>(c);
    }
    return o;
}

bool from_json(const geo::Json& j, Marker* m, std::string* err) {
    if (j.type != geo::Json::OBJ) { *err = "not a JSON object"; return false; }
    m->id = j.str("id");
    if (!valid_id(m->id)) { *err = "bad marker id"; return false; }
    const geo::Json* la = j.get("lat");
    const geo::Json* lo = j.get("lon");
    if (!la || !lo || la->type != geo::Json::NUM || lo->type != geo::Json::NUM ||
        !std::isfinite(la->n) || !std::isfinite(lo->n) || std::fabs(la->n) > 90 || std::fabs(lo->n) > 180) {
        *err = "bad position";
        return false;
    }
    m->lat = la->n;
    m->lon = lo->n;
    m->name = clean(j.str("name"), MAX_NAME, false);
    if (m->name.empty()) m->name = "Marker";
    m->notes = clean(j.str("notes"), MAX_NOTES, true);
    m->freqs.clear();
    if (const geo::Json* f = j.get("freqs"); f && f->type == geo::Json::ARR) {
        if (f->a.size() > MAX_FREQS) { *err = "too many frequencies (max " + std::to_string(MAX_FREQS) + ")"; return false; }
        for (const auto& x : f->a) {
            const geo::Json* hz = x.type == geo::Json::OBJ ? x.get("hz") : nullptr;
            if (!hz || hz->type != geo::Json::NUM || !std::isfinite(hz->n) || hz->n < 1 || hz->n > 1e11) {
                *err = "bad frequency";
                return false;
            }
            m->freqs.push_back({std::llround(hz->n), clean(x.str("label"), MAX_LABEL, false)});
        }
    }
    const geo::Json* t = j.get("t");
    m->t = t && t->type == geo::Json::NUM && std::isfinite(t->n) ? static_cast<int64_t>(t->n) : 0;
    return true;
}

std::string to_json(const Marker& m) {
    char ll[80];
    snprintf(ll, sizeof ll, "\"lat\":%.7f,\"lon\":%.7f", m.lat, m.lon);
    std::string s = "{\"id\":\"" + json_escape(m.id) + "\"," + ll + ",\"name\":\"" + json_escape(m.name) +
                    "\",\"notes\":\"" + json_escape(m.notes) + "\",\"freqs\":[";
    for (size_t i = 0; i < m.freqs.size(); i++) {
        if (i) s += ',';
        s += "{\"hz\":" + std::to_string(m.freqs[i].hz) + ",\"label\":\"" + json_escape(m.freqs[i].label) + "\"}";
    }
    s += "],\"t\":" + std::to_string(m.t) + "}";
    return s;
}

void save_locked() {
    const std::string tmp = std::string(MARKER_FILE) + ".tmp";
    std::ofstream f(tmp);
    if (f) {
        f << "{\"version\":1,\"markers\":[";
        for (size_t i = 0; i < list.size(); i++) f << (i ? ",\n  " : "\n  ") << to_json(list[i]);
        f << "\n]}\n";
        f.close();
    }
    if (!f || rename(tmp.c_str(), MARKER_FILE) != 0)
        std::cerr << "Map markers: can't write " << MARKER_FILE << " - changes are lost at a restart" << std::endl;
}

}  // namespace

void load() {
    std::ifstream f(MARKER_FILE);
    if (!f) return;   // none yet
    std::stringstream ss;
    ss << f.rdbuf();
    geo::Json j;
    const geo::Json* arr = nullptr;
    if (geo::json_parse(ss.str(), &j)) arr = j.get("markers");
    if (!arr || arr->type != geo::Json::ARR) {
        std::cerr << "Map markers: " << MARKER_FILE << " is unreadable - starting without markers" << std::endl;
        return;
    }
    std::lock_guard<std::mutex> lk(mu);
    list.clear();
    size_t bad = 0;
    for (const auto& x : arr->a) {
        Marker m;
        std::string err;
        bool ok = from_json(x, &m, &err) && list.size() < MAX_MARKERS;
        for (const auto& o : list) ok = ok && o.id != m.id;   // a duplicate id
        if (!ok) { bad++; continue; }
        list.push_back(std::move(m));
    }
    std::cout << "Map markers: " << list.size() << " loaded" << std::endl;
    if (bad) std::cerr << "Map markers: " << bad << " unreadable marker(s) in " << MARKER_FILE << " skipped" << std::endl;
}

bool set(const std::string& json, std::string* id, std::string* err) {
    geo::Json j;
    if (json.size() > 64 * 1024 || !geo::json_parse(json, &j)) { *err = "malformed marker"; return false; }
    if (j.type == geo::Json::OBJ && valid_id(j.str("id"))) *id = j.str("id");
    Marker m;
    if (!from_json(j, &m, err)) return false;
    m.t = now_ms();
    std::lock_guard<std::mutex> lk(mu);
    for (auto& o : list) {
        if (o.id != m.id) continue;
        o = std::move(m);
        save_locked();
        return true;
    }
    if (list.size() >= MAX_MARKERS) { *err = "too many markers (max " + std::to_string(MAX_MARKERS) + ")"; return false; }
    std::cout << "Map marker added: " << m.name << std::endl;
    list.push_back(std::move(m));
    save_locked();
    return true;
}

bool remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].id != id) continue;
        std::cout << "Map marker removed: " << list[i].name << std::endl;
        list.erase(list.begin() + static_cast<long>(i));
        save_locked();
        return true;
    }
    return false;
}

std::string message_json(const std::string& error, const std::string& id) {
    std::string s = "{\"markers\":{\"list\":[";
    {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < list.size(); i++) {
            if (i) s += ',';
            s += to_json(list[i]);
        }
    }
    s += ']';
    if (!error.empty()) s += ",\"error\":\"" + json_escape(error) + "\",\"id\":\"" + json_escape(id) + "\"";
    s += "}}";
    return s;
}

}  // namespace markers
