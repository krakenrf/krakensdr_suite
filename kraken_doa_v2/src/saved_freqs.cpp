#include "saved_freqs.hpp"

#include "geo_address.hpp"   // geo::json_parse
#include "utils/json_escape.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace savedfreq {
namespace {

constexpr const char* SF_FILE = "saved_frequencies.json";
constexpr const char* OLD_MARKER_FILE = "map_markers.json";   // before the merge
constexpr const char* DEFAULT_COL = "#90a4ae";

struct Category {
    std::string id, name, col;
};
struct Location {
    std::string id, name, notes;
    double lat = 0, lon = 0;
    int64_t t = 0;   // last change, ms since the epoch
};
struct Freq {
    std::string id, name;
    int64_t hz = 0;
    std::string cat, loc;   // ids, "" = none
    std::string mode;       // "" (the VFO's) | WBFM | NBFM | AM | DIGITAL
    int64_t bw = 0;         // Hz, 0 = the VFO's
    std::string dec;        // "" (the VFO's) | OFF | AUTO | PLUGIN:<id>
    std::string tone, notes;
    int64_t t = 0;
};
struct State {
    std::vector<Category> cats;   // in the order they were added
    std::vector<Location> locs;
    std::vector<Freq> freqs;
};

std::mutex mu;
State st;

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
// characters dropped (notes keep line breaks and tabs); trimmed
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
    const size_t a = o.find_first_not_of(" \t\n");
    if (a == std::string::npos) return "";
    return o.substr(a, o.find_last_not_of(" \t\n") - a + 1);
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool valid_col(const std::string& c) {
    if (c.size() != 7 || c[0] != '#') return false;
    for (size_t i = 1; i < 7; i++)
        if (!isxdigit(static_cast<unsigned char>(c[i]))) return false;
    return true;
}

bool valid_dec(const std::string& d) {
    if (d.empty() || d == "OFF" || d == "AUTO") return true;
    if (d.rfind("PLUGIN:", 0) != 0) return false;
    const std::string p = d.substr(7);
    if (p.empty() || p.size() > 32) return false;
    for (char c : p)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// a finite number in [lo, hi], or false
bool num(const geo::Json& j, const char* key, double lo, double hi, double* out) {
    const geo::Json* v = j.get(key);
    if (!v || v->type != geo::Json::NUM || !std::isfinite(v->n) || v->n < lo || v->n > hi) return false;
    *out = v->n;
    return true;
}

int64_t stamp(const geo::Json& j) {
    double t = 0;
    return num(j, "t", 0, 9e15, &t) ? static_cast<int64_t>(t) : 0;
}

// --- one entry from JSON (a command or the file) ---------------------------
bool cat_from_json(const geo::Json& j, Category* c, std::string* err) {
    if (j.type != geo::Json::OBJ) { *err = "not a JSON object"; return false; }
    c->id = j.str("id");
    if (!valid_id(c->id)) { *err = "bad category id"; return false; }
    c->name = clean(j.str("name"), MAX_CAT_NAME, false);
    if (c->name.empty()) { *err = "the category needs a name"; return false; }
    c->col = j.str("col");
    if (!valid_col(c->col)) c->col = DEFAULT_COL;
    return true;
}

bool loc_from_json(const geo::Json& j, Location* l, std::string* err) {
    if (j.type != geo::Json::OBJ) { *err = "not a JSON object"; return false; }
    l->id = j.str("id");
    if (!valid_id(l->id)) { *err = "bad location id"; return false; }
    if (!num(j, "lat", -90, 90, &l->lat) || !num(j, "lon", -180, 180, &l->lon)) { *err = "bad position"; return false; }
    l->name = clean(j.str("name"), MAX_NAME, false);
    if (l->name.empty()) l->name = "Marker";
    l->notes = clean(j.str("notes"), MAX_LOC_NOTES, true);
    l->t = stamp(j);
    return true;
}

bool freq_from_json(const geo::Json& j, Freq* f, std::string* err) {
    if (j.type != geo::Json::OBJ) { *err = "not a JSON object"; return false; }
    f->id = j.str("id");
    if (!valid_id(f->id)) { *err = "bad frequency id"; return false; }
    f->name = clean(j.str("name"), MAX_NAME, false);
    if (f->name.empty()) { *err = "the frequency needs a name"; return false; }
    double hz = 0;
    if (!num(j, "hz", 1, 1e11, &hz)) { *err = "bad frequency"; return false; }
    f->hz = std::llround(hz);
    f->cat = j.str("cat");
    f->loc = j.str("loc");
    if ((!f->cat.empty() && !valid_id(f->cat)) || (!f->loc.empty() && !valid_id(f->loc))) {
        *err = "bad category / location id";
        return false;
    }
    f->mode = j.str("mode");
    if (!f->mode.empty() && f->mode != "WBFM" && f->mode != "NBFM" && f->mode != "AM" && f->mode != "DIGITAL") {
        *err = "bad demod mode";
        return false;
    }
    double bw = 0;
    if (j.get("bw") && !num(j, "bw", 0, 1e8, &bw)) { *err = "bad bandwidth"; return false; }
    f->bw = std::llround(bw);
    f->dec = j.str("dec");
    if (!valid_dec(f->dec)) { *err = "bad decoder"; return false; }
    f->tone = clean(j.str("tone"), MAX_TONE, false);
    f->notes = clean(j.str("notes"), MAX_FREQ_NOTES, true);
    f->t = stamp(j);
    return true;
}

// --- JSON out ---------------------------------------------------------------
std::string to_json(const Category& c) {
    return "{\"id\":\"" + json_escape(c.id) + "\",\"name\":\"" + json_escape(c.name) + "\",\"col\":\"" +
           json_escape(c.col) + "\"}";
}
std::string to_json(const Location& l) {
    char ll[80];
    snprintf(ll, sizeof ll, "\"lat\":%.7f,\"lon\":%.7f", l.lat, l.lon);
    return "{\"id\":\"" + json_escape(l.id) + "\"," + ll + ",\"name\":\"" + json_escape(l.name) +
           "\",\"notes\":\"" + json_escape(l.notes) + "\",\"t\":" + std::to_string(l.t) + "}";
}
std::string to_json(const Freq& f) {
    return "{\"id\":\"" + json_escape(f.id) + "\",\"name\":\"" + json_escape(f.name) + "\",\"hz\":" +
           std::to_string(f.hz) + ",\"cat\":\"" + json_escape(f.cat) + "\",\"loc\":\"" + json_escape(f.loc) +
           "\",\"mode\":\"" + json_escape(f.mode) + "\",\"bw\":" + std::to_string(f.bw) + ",\"dec\":\"" +
           json_escape(f.dec) + "\",\"tone\":\"" + json_escape(f.tone) + "\",\"notes\":\"" + json_escape(f.notes) +
           "\",\"t\":" + std::to_string(f.t) + "}";
}

// the three lists as JSON members (message and file), sep between entries
std::string lists_json(const State& s, const char* cats, const char* locs, const char* freqs, const char* sep) {
    std::string o = std::string("\"") + cats + "\":[";
    for (size_t i = 0; i < s.cats.size(); i++) o += (i ? sep : "") + to_json(s.cats[i]);
    o += std::string("],\"") + locs + "\":[";
    for (size_t i = 0; i < s.locs.size(); i++) o += (i ? sep : "") + to_json(s.locs[i]);
    o += std::string("],\"") + freqs + "\":[";
    for (size_t i = 0; i < s.freqs.size(); i++) o += (i ? sep : "") + to_json(s.freqs[i]);
    return o + "]";
}

template <class T>
T* find(std::vector<T>& v, const std::string& id) {
    for (auto& x : v)
        if (x.id == id) return &x;
    return nullptr;
}

void save(const State& s) {
    const std::string tmp = std::string(SF_FILE) + ".tmp";
    std::ofstream f(tmp);
    if (f) {
        f << "{\"version\":2," << lists_json(s, "categories", "locations", "frequencies", ",\n  ") << "}\n";
        f.close();
    }
    if (!f || rename(tmp.c_str(), SF_FILE) != 0)
        std::cerr << "Saved frequencies: can't write " << SF_FILE << " - changes are lost at a restart" << std::endl;
}

// Make n the state if it fits (MAX_TOTAL) and save it; under mu
bool commit(State&& n, std::string* err) {
    if (lists_json(n, "c", "l", "f", ",").size() > MAX_TOTAL) {
        *err = "the saved frequency list is full (" + std::to_string(MAX_TOTAL / (1024 * 1024)) + " MB)";
        return false;
    }
    st = std::move(n);
    save(st);
    return true;
}

bool cat_name_taken(const State& s, const Category& c) {
    const std::string n = lower_ascii(c.name);
    for (const auto& o : s.cats)
        if (o.id != c.id && lower_ascii(o.name) == n) return true;
    return false;
}

// Create or replace one entry in n (limits and references checked)
bool put_cat(State& n, Category&& c, std::string* err) {
    if (cat_name_taken(n, c)) { *err = "there is already a category \"" + c.name + "\""; return false; }
    if (Category* o = find(n.cats, c.id)) { *o = std::move(c); return true; }
    if (n.cats.size() >= MAX_CATS) { *err = "too many categories (max " + std::to_string(MAX_CATS) + ")"; return false; }
    n.cats.push_back(std::move(c));
    return true;
}
bool put_loc(State& n, Location&& l, std::string* err) {
    l.t = now_ms();
    if (Location* o = find(n.locs, l.id)) { *o = std::move(l); return true; }
    if (n.locs.size() >= MAX_LOCS) { *err = "too many locations (max " + std::to_string(MAX_LOCS) + ")"; return false; }
    n.locs.push_back(std::move(l));
    return true;
}

// The default categories of a new list (the user can rename / delete them)
void seed_categories(State& s) {
    s.cats = {{"airband", "Airband", "#4fc3f7"},          {"broadcast", "Broadcast", "#ffb74d"},
              {"business", "Business band", "#81c784"},   {"amateur", "Amateur", "#ba68c8"},
              {"marine", "Marine", "#4db6ac"},             {"emergency", "Emergency services", "#e57373"}};
}

std::string base36(uint64_t v) {
    std::string o;
    do { o.insert(o.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36]); v /= 36; } while (v);
    return o;
}

// map_markers.json (version 1: [{id,lat,lon,name,notes,freqs:[{hz,label}],t}])
// -> a location per marker + a frequency per marker frequency (named by its
// label, else by the marker). False = nothing to migrate / unreadable.
bool migrate_markers(State& s) {
    std::ifstream f(OLD_MARKER_FILE);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    geo::Json j;
    const geo::Json* arr = nullptr;
    if (geo::json_parse(ss.str(), &j)) arr = j.get("markers");
    if (!arr || arr->type != geo::Json::ARR) {
        std::cerr << "Saved frequencies: " << OLD_MARKER_FILE << " is unreadable - not migrated" << std::endl;
        return false;
    }
    const std::string tag = base36(static_cast<uint64_t>(now_ms()));
    size_t n = 0;
    for (const auto& m : arr->a) {
        Location l;
        std::string err;
        if (!loc_from_json(m, &l, &err) || find(s.locs, l.id) || s.locs.size() >= MAX_LOCS) continue;
        if (const geo::Json* fq = m.get("freqs"); fq && fq->type == geo::Json::ARR) {
            for (const auto& x : fq->a) {
                double hz = 0;
                if (x.type != geo::Json::OBJ || !num(x, "hz", 1, 1e11, &hz) || s.freqs.size() >= MAX_FREQS) continue;
                Freq q;
                q.id = "m" + base36(n++) + "x" + tag;
                q.name = clean(x.str("label"), MAX_NAME, false);
                if (q.name.empty()) q.name = l.name;
                q.hz = std::llround(hz);
                q.loc = l.id;
                q.t = l.t;
                s.freqs.push_back(std::move(q));
            }
        }
        s.locs.push_back(std::move(l));
    }
    std::cout << "Saved frequencies: " << s.locs.size() << " map marker(s) with " << s.freqs.size()
              << " frequenc" << (s.freqs.size() == 1 ? "y" : "ies") << " taken over from " << OLD_MARKER_FILE
              << std::endl;
    return true;
}

}  // namespace

void load() {
    std::lock_guard<std::mutex> lk(mu);
    st = State{};
    std::ifstream f(SF_FILE);
    if (!f) {
        // a new list: the default categories (+ the old map markers, once)
        seed_categories(st);
        if (migrate_markers(st)) {
            save(st);
            const std::string done = std::string(OLD_MARKER_FILE) + ".migrated";
            if (rename(OLD_MARKER_FILE, done.c_str()) != 0)
                std::cerr << "Saved frequencies: can't rename " << OLD_MARKER_FILE << " to " << done << std::endl;
        }
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    geo::Json j;
    const geo::Json *cats = nullptr, *locs = nullptr, *freqs = nullptr;
    if (geo::json_parse(ss.str(), &j)) {
        cats = j.get("categories");
        locs = j.get("locations");
        freqs = j.get("frequencies");
    }
    if (!cats || !locs || !freqs || cats->type != geo::Json::ARR || locs->type != geo::Json::ARR ||
        freqs->type != geo::Json::ARR) {
        // kept aside, not overwritten by the next change
        const std::string bad = std::string(SF_FILE) + ".bad";
        rename(SF_FILE, bad.c_str());
        std::cerr << "Saved frequencies: " << SF_FILE << " is unreadable (kept as " << bad
                  << ") - starting with an empty list" << std::endl;
        return;
    }
    size_t bad = 0;
    std::string err;
    for (const auto& x : cats->a) {
        Category c;
        if (!cat_from_json(x, &c, &err) || find(st.cats, c.id) || cat_name_taken(st, c) || st.cats.size() >= MAX_CATS) {
            bad++;
            continue;
        }
        st.cats.push_back(std::move(c));
    }
    for (const auto& x : locs->a) {
        Location l;
        if (!loc_from_json(x, &l, &err) || find(st.locs, l.id) || st.locs.size() >= MAX_LOCS) { bad++; continue; }
        st.locs.push_back(std::move(l));
    }
    for (const auto& x : freqs->a) {
        Freq q;
        if (!freq_from_json(x, &q, &err) || find(st.freqs, q.id) || st.freqs.size() >= MAX_FREQS) { bad++; continue; }
        if (!q.cat.empty() && !find(st.cats, q.cat)) q.cat.clear();
        if (!q.loc.empty() && !find(st.locs, q.loc)) q.loc.clear();
        st.freqs.push_back(std::move(q));
    }
    std::cout << "Saved frequencies: " << st.freqs.size() << " frequencies, " << st.locs.size() << " locations, "
              << st.cats.size() << " categories loaded" << std::endl;
    if (bad) std::cerr << "Saved frequencies: " << bad << " unreadable entr" << (bad == 1 ? "y" : "ies") << " in "
                       << SF_FILE << " skipped" << std::endl;
}

bool set_freq(const std::string& json, std::string* id, std::string* err) {
    geo::Json j;
    if (json.size() > 64 * 1024 || !geo::json_parse(json, &j)) { *err = "malformed frequency"; return false; }
    if (j.type == geo::Json::OBJ && valid_id(j.str("id"))) *id = j.str("id");
    Freq q;
    if (!freq_from_json(j, &q, err)) return false;
    q.t = now_ms();
    std::lock_guard<std::mutex> lk(mu);
    State n = st;
    // the category / location made in the same edit
    if (const geo::Json* c = j.get("newcat")) {
        Category nc;
        if (!cat_from_json(*c, &nc, err) || !put_cat(n, std::move(nc), err)) return false;
    }
    if (const geo::Json* l = j.get("newloc")) {
        Location nl;
        if (!loc_from_json(*l, &nl, err) || !put_loc(n, std::move(nl), err)) return false;
    }
    if (!q.cat.empty() && !find(n.cats, q.cat)) { *err = "that category no longer exists"; return false; }
    if (!q.loc.empty() && !find(n.locs, q.loc)) { *err = "that location no longer exists"; return false; }
    const bool added = !find(n.freqs, q.id);
    if (added && n.freqs.size() >= MAX_FREQS) { *err = "too many frequencies (max " + std::to_string(MAX_FREQS) + ")"; return false; }
    const std::string name = q.name;
    if (Freq* o = find(n.freqs, q.id)) *o = std::move(q);
    else n.freqs.push_back(std::move(q));
    if (!commit(std::move(n), err)) return false;
    if (added) std::cout << "Saved frequency added: " << name << std::endl;
    return true;
}

bool set_location(const std::string& json, std::string* id, std::string* err) {
    geo::Json j;
    if (json.size() > 64 * 1024 || !geo::json_parse(json, &j)) { *err = "malformed location"; return false; }
    if (j.type == geo::Json::OBJ && valid_id(j.str("id"))) *id = j.str("id");
    Location l;
    if (!loc_from_json(j, &l, err)) return false;
    std::lock_guard<std::mutex> lk(mu);
    State n = st;
    const bool added = !find(n.locs, l.id);
    const std::string name = l.name;
    if (!put_loc(n, std::move(l), err) || !commit(std::move(n), err)) return false;
    if (added) std::cout << "Map marker added: " << name << std::endl;
    return true;
}

bool set_category(const std::string& json, std::string* id, std::string* err) {
    geo::Json j;
    if (json.size() > 4096 || !geo::json_parse(json, &j)) { *err = "malformed category"; return false; }
    if (j.type == geo::Json::OBJ && valid_id(j.str("id"))) *id = j.str("id");
    Category c;
    if (!cat_from_json(j, &c, err)) return false;
    std::lock_guard<std::mutex> lk(mu);
    State n = st;
    return put_cat(n, std::move(c), err) && commit(std::move(n), err);
}

bool remove_freq(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < st.freqs.size(); i++) {
        if (st.freqs[i].id != id) continue;
        std::cout << "Saved frequency removed: " << st.freqs[i].name << std::endl;
        st.freqs.erase(st.freqs.begin() + static_cast<long>(i));
        save(st);
        return true;
    }
    return false;
}

bool remove_location(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < st.locs.size(); i++) {
        if (st.locs[i].id != id) continue;
        std::cout << "Map marker removed: " << st.locs[i].name << std::endl;
        st.locs.erase(st.locs.begin() + static_cast<long>(i));
        for (auto& q : st.freqs)
            if (q.loc == id) q.loc.clear();   // the frequencies stay, without a place
        save(st);
        return true;
    }
    return false;
}

bool remove_category(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < st.cats.size(); i++) {
        if (st.cats[i].id != id) continue;
        st.cats.erase(st.cats.begin() + static_cast<long>(i));
        for (auto& q : st.freqs)
            if (q.cat == id) q.cat.clear();
        save(st);
        return true;
    }
    return false;
}

std::string message_json(const std::string& error, const std::string& id) {
    std::string s = "{\"savedfreq\":{";
    {
        std::lock_guard<std::mutex> lk(mu);
        s += lists_json(st, "cats", "locs", "freqs", ",");
    }
    if (!error.empty()) s += ",\"error\":\"" + json_escape(error) + "\",\"id\":\"" + json_escape(id) + "\"";
    s += "}}";
    return s;
}

}  // namespace savedfreq
