#include "incidents.hpp"
#include "decoder_log.hpp"

#include "decimator_manager.hpp"
#include "digital/digital_decoder.hpp"
#include "geo_address.hpp"
#include "geo_http.hpp"
#include "globals.hpp"
#include "station_info.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace incidents {

namespace {

const char* const INCIDENT_FILE = "incidents.tsv";
constexpr int64_t SAME_INCIDENT_MS = 2 * 3600 * 1000;   // the same address paged again: same incident
constexpr int64_t OFFLINE_RETRY_MS = 60 * 1000;         // no internet: try a message again after this
constexpr int64_t OFFLINE_GIVE_UP_MS = 30 * 60 * 1000;  // ... until it is this old
constexpr size_t MAX_QUEUED = 300;
constexpr size_t MAX_INCIDENTS = 5000;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string clean(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return o;
}

std::string local_time(int64_t ms) {
    time_t t = static_cast<time_t>(ms / 1000);
    struct tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

struct Msg {
    dig::DigitalDecoder* dec;
    std::string plugin, from, text;
    double rf_hz;
    int64_t t_ms;
    int64_t retry_ms = 0;   // offline: not before this
};

struct Incident {
    std::string id, key, plugin;
    int vfo = 0;
    double rf_hz = 0;   // the VFO's frequency when paged (0 = unknown, older files)
    double lat = 0, lon = 0;
    std::string address, cross, place, area, precision, confidence, from, text;
    int alternatives = 0, pages = 1;
    // every VFO + frequency it was paged on (the same address on two
    // channels - fire and ambulance pagers - is one incident)
    std::vector<std::pair<int, int64_t>> heard;
    int64_t first_ms = 0, last_ms = 0;
    uint64_t seq = 0;
};

struct State {
    std::mutex mu;   // everything below
    std::condition_variable cv;
    std::deque<Msg> queue;
    std::string state = "idle", error;   // idle | ready | offline | error | no_station
    uint64_t found = 0, not_found = 0;
    std::map<std::string, Incident> inc;
    uint64_t seq = 0;
    bool dirty = false;
    std::atomic<int> radius{DEFAULT_RADIUS_KM};
    geo::Nominatim nominatim;   // has its own lock + rate limit
};
// never destroyed: the worker may still run while the process exits
State& S() {
    static State* s = new State;
    return *s;
}

bool station(double* lat, double* lon) {
    StationLocation sl = station_info.resolve();
    if (!is_finite_value(sl.lat) || !is_finite_value(sl.lon) || (sl.lat == 0.0 && sl.lon == 0.0)) return false;
    *lat = sl.lat;
    *lon = sl.lon;
    return true;
}

void save_locked(State& s) {
    const std::string tmp = std::string(INCIDENT_FILE) + ".tmp";
    std::ofstream f(tmp);
    if (!f) return;
    f << "# kraken_doa incidents v1\n";
    for (const auto& kv : s.inc) {
        const Incident& i = kv.second;
        char ll[64];
        snprintf(ll, sizeof ll, "%.6f\t%.6f", i.lat, i.lon);
        f << i.id << '\t' << i.vfo << '\t' << ll << '\t' << i.first_ms << '\t' << i.last_ms << '\t' << i.pages << '\t'
          << i.alternatives << '\t' << clean(i.key) << '\t' << clean(i.plugin) << '\t' << clean(i.address) << '\t'
          << clean(i.cross) << '\t' << clean(i.place) << '\t' << clean(i.precision) << '\t' << clean(i.confidence)
          << '\t' << clean(i.from) << '\t' << clean(i.text) << '\t' << clean(i.area) << '\t'
          << static_cast<int64_t>(i.rf_hz) << '\t';
        for (size_t k = 0; k < i.heard.size(); k++)
            f << (k ? "," : "") << i.heard[k].first << ':' << i.heard[k].second;
        f << '\n';
    }
    f.close();
    if (f) rename(tmp.c_str(), INCIDENT_FILE);
    s.dirty = false;
}

void load_incidents(State& s) {
    std::ifstream f(INCIDENT_FILE);
    std::string line;
    const int64_t t = now_ms();
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> v;
        size_t p = 0, q;
        while ((q = line.find('\t', p)) != std::string::npos) { v.push_back(line.substr(p, q - p)); p = q + 1; }
        v.push_back(line.substr(p));
        if (v.size() < 17) continue;
        Incident i;
        i.id = v[0];
        i.vfo = atoi(v[1].c_str());
        i.lat = atof(v[2].c_str());
        i.lon = atof(v[3].c_str());
        i.first_ms = atoll(v[4].c_str());
        i.last_ms = atoll(v[5].c_str());
        i.pages = atoi(v[6].c_str());
        i.alternatives = atoi(v[7].c_str());
        i.key = v[8]; i.plugin = v[9]; i.address = v[10]; i.cross = v[11]; i.place = v[12];
        i.precision = v[13]; i.confidence = v[14]; i.from = v[15]; i.text = v[16];
        if (v.size() > 17) i.area = v[17];
        if (v.size() > 18) i.rf_hz = atof(v[18].c_str());
        if (v.size() > 19) {
            std::stringstream hs(v[19]);
            std::string h;
            while (std::getline(hs, h, ','))
                if (h.find(':') != std::string::npos)
                    i.heard.push_back({atoi(h.c_str()), atoll(h.substr(h.find(':') + 1).c_str())});
        }
        if (i.heard.empty()) i.heard.push_back({i.vfo, static_cast<int64_t>(i.rf_hz)});
        if (t - i.last_ms > INCIDENT_TTL_H * 3600000LL || i.id.empty()) continue;
        i.seq = ++s.seq;
        s.inc[i.id] = std::move(i);
    }
}

std::string info_text(const Incident& i) {
    std::string x = "Address: " + i.address;
    if (!i.cross.empty()) x += "\nCross street: " + i.cross;
    if (!i.area.empty()) x += "\nArea: " + i.area;
    x += "\nLocated: " + std::string(i.precision == "address" ? "the address"
                                     : i.precision == "junction" ? "near the junction" : "on the street (not the house)") +
         " (" + i.confidence + " confidence" + (i.place.empty() ? "" : ", matched " + i.place) + ")";
    if (i.alternatives) x += "\nOther streets of that name: " + std::to_string(i.alternatives) + " (nearest chosen)";
    x += "\nFrom: " + i.from;
    std::string on;
    for (const auto& h : i.heard) {
        char f[48];
        if (h.second > 0) snprintf(f, sizeof f, "D%d %.4f MHz", h.first, h.second / 1e6);
        else snprintf(f, sizeof f, "D%d", h.first);
        on += (on.empty() ? "" : ", ") + std::string(f);
    }
    if (!on.empty()) x += "\nReceived on: " + on;
    if (i.pages > 1) x += "\nPages: " + std::to_string(i.pages) + " (first " + local_time(i.first_ms) + ")";
    x += "\nPaged: " + local_time(i.last_ms);
    x += "\nMessage: " + i.text;
    return x;
}

// false: try the message again later (no internet)
bool handle(State& s, const Msg& m) {
    // the VFO the decoder belongs to (it may have been removed meanwhile)
    int vfo = -1;
    std::shared_ptr<dig::DigitalDecoder> dd;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst) continue;
        auto d = inst->getDigital();
        if (d.get() == m.dec) { vfo = inst->id; dd = d; break; }
    }
    if (vfo < 0) return true;
    double st_lat, st_lon;
    if (!station(&st_lat, &st_lon)) {
        std::lock_guard<std::mutex> lk(s.mu);
        s.state = "no_station";
        return true;
    }
    geo::Result r;
    std::string err;
    bool offline = false;
    geo::SearchFn search = [&s](const std::string& q, double a, double b, double c, double d, std::string* js,
                                std::string* e, bool* off) { return s.nominatim.search(q, a, b, c, d, js, e, off); };
    const bool ok = geo::geocode(m.text, st_lat, st_lon, s.radius.load(), search, &r, &err, &offline);
    std::string ev;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!ok) {
            if (!err.empty()) {
                s.state = offline ? "offline" : "error";
                s.error = err;
                return !offline;
            }
            s.state = "ready";
            s.error.clear();
            s.not_found++;
            return true;
        }
        s.state = "ready";
        s.error.clear();
        s.found++;
        Incident* same = nullptr;
        for (auto& kv : s.inc)
            if (kv.second.plugin == m.plugin && kv.second.key == r.key && m.t_ms - kv.second.last_ms < SAME_INCIDENT_MS)
                same = &kv.second;
        if (same) {
            same->pages++;
            same->last_ms = std::max(same->last_ms, m.t_ms);
            same->text = m.text;
            if (same->from.find(m.from) == std::string::npos && same->from.size() < 200) same->from += ", " + m.from;
            same->rf_hz = m.rf_hz;
            // another VFO / frequency (same VFO retuned: its new frequency)
            const std::pair<int, int64_t> h{vfo, static_cast<int64_t>(m.rf_hz)};
            if (std::find(same->heard.begin(), same->heard.end(), h) == same->heard.end() && same->heard.size() < 16)
                same->heard.push_back(h);
            same->seq = ++s.seq;
        } else {
            Incident i;
            char id[32];
            snprintf(id, sizeof id, "i%llx", static_cast<unsigned long long>(
                                                std::hash<std::string>{}(r.key) ^ static_cast<uint64_t>(m.t_ms)));
            i.id = id;
            i.key = r.key;
            i.plugin = m.plugin;
            i.vfo = vfo;
            i.rf_hz = m.rf_hz;
            i.heard.push_back({vfo, static_cast<int64_t>(m.rf_hz)});
            i.lat = r.lat;
            i.lon = r.lon;
            i.address = r.address;
            i.cross = r.cross;
            i.place = r.place;
            i.area = r.area;
            i.precision = r.precision;
            i.confidence = r.confidence;
            i.alternatives = r.alternatives;
            i.from = m.from;
            i.text = m.text;
            i.first_ms = i.last_ms = m.t_ms;
            i.seq = ++s.seq;
            s.inc[i.id] = i;
            if (s.inc.size() > MAX_INCIDENTS) {
                auto old = std::min_element(s.inc.begin(), s.inc.end(), [](const auto& a, const auto& b) {
                    return a.second.last_ms < b.second.last_ms;
                });
                s.inc.erase(old);
            }
            char d[32];
            snprintf(d, sizeof d, "%.1f km", geo::distance_km(st_lat, st_lon, r.lat, r.lon));
            declog::record_incident(vfo, m.plugin, r.address + (r.cross.empty() ? "" : " / " + r.cross) +
                                                       (r.area.empty() ? "" : ", " + r.area),
                                    r.lat, r.lon, r.precision, r.confidence, m.text);
            ev = "📍 Incident: " + r.address + (r.cross.empty() ? "" : " / " + r.cross) +
                 (r.area.empty() ? "" : ", " + r.area) + " (" + r.precision + ", " + r.confidence + " confidence, " +
                 d + " from the station)";
        }
        s.dirty = true;
    }
    if (!ev.empty()) dd->report().event(m.plugin, ev, 0.0);
    return true;
}

void worker() {
    State& s = S();
    int64_t saved_ms = 0;
    for (;;) {
        Msg m;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv.wait_for(lk, std::chrono::seconds(5), [&] {
                const int64_t t = now_ms();
                return std::any_of(s.queue.begin(), s.queue.end(), [t](const Msg& x) { return x.retry_ms <= t; });
            });
            const int64_t t = now_ms();
            // drop what waited too long for the internet to come back
            while (!s.queue.empty() && s.queue.front().retry_ms && t - s.queue.front().t_ms > OFFLINE_GIVE_UP_MS)
                s.queue.pop_front();
            for (auto it = s.queue.begin(); it != s.queue.end(); ++it)
                if (it->retry_ms <= t) {
                    m = std::move(*it);
                    s.queue.erase(it);
                    have = true;
                    break;
                }
        }
        if (have && !handle(s, m)) {
            // no internet: try again in a minute (in order)
            std::lock_guard<std::mutex> lk(s.mu);
            m.retry_ms = now_ms() + OFFLINE_RETRY_MS;
            s.queue.push_front(std::move(m));
        }
        // expiry + saving
        const int64_t t = now_ms();
        std::lock_guard<std::mutex> lk(s.mu);
        for (auto it = s.inc.begin(); it != s.inc.end();) {
            if (t - it->second.last_ms > INCIDENT_TTL_H * 3600000LL) {
                it = s.inc.erase(it);
                s.dirty = true;
                s.seq++;
            }
            else ++it;
        }
        if (s.dirty && t - saved_ms > 10000) {
            save_locked(s);
            saved_ms = t;
        }
    }
}

}  // namespace

void start() {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        load_incidents(s);
    }
    dig::set_message_handler([](dig::DigitalDecoder* d, const std::string& plugin, const std::string& from,
                                const std::string& text, double rf_hz) {
        if (text.empty()) return;   // a decoder that sends messages started: nothing to prepare
        State& st = S();
        std::lock_guard<std::mutex> lk(st.mu);
        st.queue.push_back({d, plugin, from, text, rf_hz, now_ms()});
        while (st.queue.size() > MAX_QUEUED) st.queue.pop_front();
        st.cv.notify_all();
    });
    std::thread(worker).detach();
}

void save_now() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.dirty) save_locked(s);
}

void set_radius_km(int km) { S().radius = std::clamp(km, 10, 1000); }

int radius_km() { return S().radius.load(); }

void clear() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.inc.clear();
    s.dirty = true;
    s.seq++;
}

uint64_t seq() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.seq;
}

std::string status_json() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    std::ostringstream o;
    o << "{\"radius_km\":" << s.radius.load() << ",\"state\":\"" << s.state << "\",\"error\":\""
      << json_escape(s.error) << "\",\"found\":" << s.found << ",\"not_found\":" << s.not_found
      << ",\"requests\":" << s.nominatim.requests() << ",\"incidents\":" << s.inc.size()
      << ",\"queued\":" << s.queue.size() << "}";
    return o.str();
}

uint64_t map_json(const std::set<std::string>& plugins, uint64_t after, std::string& pts, std::string& keys) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t t = now_ms();
    for (const auto& kv : s.inc) {
        const Incident& i = kv.second;
        if (!plugins.count(i.plugin)) continue;
        const std::string v = std::to_string(i.vfo);
        if (!keys.empty()) keys += ",";
        keys += "\"" + v + "|incidents|" + json_escape(i.id) + "\"";
        if (i.seq <= after) continue;
        char ll[96];
        snprintf(ll, sizeof ll, "\"la\":%.6f,\"lo\":%.6f", i.lat, i.lon);
        if (!pts.empty()) pts += ",";
        pts += "{\"v\":" + v + ",\"p\":\"incidents\",\"i\":\"" + json_escape(i.id) + "\"," + ll + ",\"l\":\"" +
               json_escape(i.address) + "\",\"k\":\"incident\",\"h\":null,\"a\":null,\"s\":null,\"x\":\"" +
               json_escape(info_text(i)) + "\",\"age\":" + std::to_string((t - i.last_ms) / 1000) +
               ",\"ttl\":" + std::to_string(INCIDENT_TTL_H * 3600) + "}";
    }
    return s.seq;
}

std::string message_json() {
    State& s = S();
    double st_lat = 0, st_lon = 0;
    const bool have = station(&st_lat, &st_lon);
    const std::string geo = status_json();
    std::lock_guard<std::mutex> lk(s.mu);
    const int64_t t = now_ms();
    std::string rows;
    for (const auto& kv : s.inc) {
        const Incident& i = kv.second;
        char d[32] = "";
        if (have) snprintf(d, sizeof d, "%.1f", geo::distance_km(st_lat, st_lon, i.lat, i.lon));
        // every VFO / frequency it was paged on: "D0, D2" / "153.2750, 157.4500"
        std::string vfos, mhz;
        for (const auto& h : i.heard) {
            const std::string v = "D" + std::to_string(h.first);
            if ((", " + vfos + ",").find(", " + v + ",") == std::string::npos) vfos += (vfos.empty() ? "" : ", ") + v;
            if (h.second > 0) {
                char f[32];
                snprintf(f, sizeof f, "%.4f", h.second / 1e6);
                if ((", " + mhz + ",").find(std::string(", ") + f + ",") == std::string::npos)
                    mhz += (mhz.empty() ? "" : ", ") + std::string(f);
            }
        }
        const std::string cells[] = {vfos, mhz, local_time(i.last_ms), i.address, i.cross, i.area,
                                     i.precision, i.confidence, d, i.from, std::to_string(i.pages), i.text};
        rows += std::string(rows.empty() ? "" : ",") + "[\"" + json_escape(i.id) + "\"," +
                std::to_string((t - i.last_ms) / 1000) + ",\"" + json_escape(i.plugin) + "\"," + std::to_string(i.vfo);
        for (const auto& c : cells) rows += ",\"" + json_escape(c) + "\"";
        rows += "]";
    }
    return "{\"incidents\":{\"seq\":" + std::to_string(s.seq) + ",\"geo\":" + geo +
           ",\"cols\":[\"VFO\",\"MHz\",\"Paged\",\"Address\",\"Cross street\",\"Area\",\"Located\",\"Confidence\","
           "\"Distance km\",\"From\",\"Pages\",\"Message\"],\"rows\":[" + rows + "]}}";
}

}  // namespace incidents
