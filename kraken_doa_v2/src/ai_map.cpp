#include "ai_map.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

#include <climits>
#include <cstdlib>
#include <sys/stat.h>

#include "geo_address.hpp"   // geo::Json
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

namespace aimap {
namespace {

using geo::Json;

constexpr const char* FILE_PATH = "ai/map_layers.json";
constexpr size_t MAX_LAYERS = 30, MAX_FEATS = 5000, MAX_VERTS = 20000;
constexpr size_t MAX_TOTAL = 1u << 20;          // all layers as one message
constexpr size_t MAX_FILE = 8u << 20;           // an input file
constexpr size_t MAX_INFO = 24;
const char* PALETTE[] = {"#ff7043", "#ab47bc", "#26c6da", "#d4e157", "#ec407a",
                         "#ffa726", "#42a5f5", "#9ccc65", "#8d6e63", "#ffee58"};

struct LL {
    double la, lo;
};
struct Feature {
    char g = 'p';                        // p point, l line, a polygon (area), c circle
    std::vector<std::vector<LL>> c;      // point / circle: c[0][0]; line: c[0]; polygon: rings
    double r = 0;                        // circle radius m
    std::string l, col, ic;
    double sz = 0, w = 0, fo = -1;
    bool da = false, ar = false;
    std::vector<std::pair<std::string, std::string>> x;
    size_t verts() const {
        size_t n = 0;
        for (const auto& r : c) n += r.size();
        return n;
    }
};
struct Layer {
    std::string id, name, desc, src, conv, col;
    int64_t t = 0;
    std::vector<std::string> f;          // features, serialized
    size_t verts = 0, bytes = 0;
    double s = 90, w = 180, n = -90, e = -180;
};

std::mutex g_mu;
std::vector<Layer> g_layers;
uint64_t g_seq = 1;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string fmt(double v, int prec) {
    if (!is_finite_value(v)) return "null";
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, v);
    // trailing zeros off (smaller messages)
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s == "-0" ? "0" : s;
}
std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// valid UTF-8 cut to max bytes; control characters dropped (newlines -> spaces)
std::string clean(const std::string& in, size_t max) {
    std::string out;
    for (size_t i = 0; i < in.size() && out.size() < max;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > in.size()) { i++; continue; }
        bool ok = true;
        for (size_t k = 1; k < n; k++)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (!ok) { i++; continue; }
        if (out.size() + n > max) break;
        if (n == 1 && (c < 0x20 || c == 0x7F)) {
            if ((c == '\n' || c == '\t' || c == '\r') && !out.empty() && out.back() != ' ') out += ' ';
            i++;
            continue;
        }
        out.append(in, i, n);
        i += n;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// "#rgb" / "#rrggbb" / a few names -> "#rrggbb"; "" = not a colour
std::string colour(const std::string& in) {
    std::string s = lower(clean(in, 20));
    static const std::pair<const char*, const char*> names[] = {
        {"red", "#e53935"}, {"green", "#43a047"}, {"blue", "#1e88e5"}, {"yellow", "#fdd835"}, {"orange", "#fb8c00"},
        {"purple", "#8e24aa"}, {"violet", "#8e24aa"}, {"cyan", "#00acc1"}, {"magenta", "#d81b60"}, {"pink", "#ec407a"},
        {"white", "#ffffff"}, {"black", "#000000"}, {"gray", "#9e9e9e"}, {"grey", "#9e9e9e"}, {"brown", "#795548"},
        {"lime", "#c0ca33"}, {"teal", "#00897b"}, {"navy", "#283593"}, {"gold", "#ffc107"}};
    for (const auto& [n, h] : names)
        if (s == n) return h;
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    if (s.size() == 4 && s[0] == '#' && hex(s[1]) && hex(s[2]) && hex(s[3]))
        return std::string("#") + s[1] + s[1] + s[2] + s[2] + s[3] + s[3];
    if (s.size() == 7 && s[0] == '#' && std::all_of(s.begin() + 1, s.end(), hex)) return s;
    return "";
}

std::string slug(const std::string& name) {
    std::string out;
    for (unsigned char c : lower(name)) {
        if (std::isalnum(c)) out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '-') out += '-';
        if (out.size() >= 40) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}
bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (unsigned char c : id)
        if (!(std::islower(c) || std::isdigit(c) || c == '-')) return false;
    return true;
}

bool jnum(const Json* j, double* out) {
    if (!j) return false;
    if (j->type == Json::NUM && is_finite_value(j->n)) {
        *out = j->n;
        return true;
    }
    if (j->type == Json::STR) {
        char* end = nullptr;
        const double v = strtod(j->s.c_str(), &end);
        if (end && end != j->s.c_str() && is_finite_value(v)) {
            *out = v;
            return true;
        }
    }
    return false;
}
const Json* first_of(const Json& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (const Json* v = o.get(k); v && v->type != Json::NUL) return v;
    return nullptr;
}
std::string text_of(const Json* v, size_t max) {
    if (!v) return "";
    if (v->type == Json::STR) return clean(v->s, max);
    if (v->type == Json::NUM) return fmt(v->n, 6);
    if (v->type == Json::BOOL) return v->b ? "yes" : "no";
    return "";
}
std::string dump(const Json& j);
void dump(const Json& j, std::string& o) {
    switch (j.type) {
        case Json::NUL: o += "null"; break;
        case Json::BOOL: o += j.b ? "true" : "false"; break;
        case Json::NUM: o += fmt(j.n, 6); break;
        case Json::STR: o += q(j.s); break;
        case Json::ARR:
            o += '[';
            for (size_t i = 0; i < j.a.size(); i++) {
                if (i) o += ',';
                dump(j.a[i], o);
            }
            o += ']';
            break;
        case Json::OBJ:
            o += '{';
            for (size_t i = 0; i < j.o.size(); i++) {
                if (i) o += ',';
                o += q(j.o[i].first) + ":";
                dump(j.o[i].second, o);
            }
            o += '}';
            break;
    }
}
std::string dump(const Json& j) {
    std::string o;
    dump(j, o);
    return o;
}

bool ll_ok(double la, double lo, LL* out) {
    if (!is_finite_value(la) || !is_finite_value(lo) || la < -90 || la > 90 || lo < -540 || lo > 540) return false;
    lo = std::fmod(lo + 180.0, 360.0);
    if (lo < 0) lo += 360;
    *out = {la, lo - 180.0};
    return true;
}

// a [lat, lon] pair / {lat, lon} object (geo = GeoJSON's [lon, lat])
bool point_of(const Json& j, bool geo, LL* out) {
    double la, lo;
    if (j.type == Json::ARR && j.a.size() >= 2 && jnum(&j.a[0], geo ? &lo : &la) && jnum(&j.a[1], geo ? &la : &lo))
        return ll_ok(la, lo, out);
    if (j.type == Json::OBJ && jnum(first_of(j, {"lat", "latitude"}), &la) && jnum(first_of(j, {"lon", "lng", "long", "longitude"}), &lo))
        return ll_ok(la, lo, out);
    return false;
}
bool points_of(const Json* j, bool geo, std::vector<LL>* out) {
    if (!j || j->type != Json::ARR) return false;
    for (const auto& p : j->a) {
        LL ll;
        if (!point_of(p, geo, &ll)) return false;
        out->push_back(ll);
    }
    return true;
}

// the great circle from (la, lo) along brg for d km
LL dest(double la, double lo, double brg, double d_km) {
    const double R = 6371.0088, d = d_km / R, p1 = la * M_PI / 180, l1 = lo * M_PI / 180, b = brg * M_PI / 180;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    LL out{};
    ll_ok(p2 * 180 / M_PI, l2 * 180 / M_PI, &out);
    return out;
}

void info_from(const Json& props, std::initializer_list<const char*> skip, Feature* f) {
    if (props.type != Json::OBJ) return;
    for (const auto& [k, v] : props.o) {
        if (f->x.size() >= MAX_INFO) break;
        bool sk = false;
        for (const char* s : skip)
            if (lower(k) == s) sk = true;
        if (sk || v.type == Json::NUL) continue;
        std::string val = v.type == Json::OBJ || v.type == Json::ARR ? clean(dump(v), 300) : text_of(&v, 300);
        const std::string key = clean(k, 40);
        if (!key.empty() && !val.empty()) f->x.push_back({key, val});
    }
}

// the style fields every input form shares (the tool's own names, then
// GeoJSON's simplestyle names)
void style_from(const Json& o, Feature* f) {
    f->l = text_of(first_of(o, {"label", "name", "title", "callsign"}), 80);
    f->col = colour(text_of(first_of(o, {"color", "colour", "marker-color", "stroke", "fill"}), 20));
    f->ic = clean(text_of(first_of(o, {"icon", "emoji"}), 16), 8);
    double v;
    if (jnum(first_of(o, {"size", "marker-size-px"}), &v)) f->sz = std::clamp(v, 2.0, 24.0);
    if (jnum(first_of(o, {"width", "stroke-width"}), &v)) f->w = std::clamp(v, 0.5, 10.0);
    if (jnum(first_of(o, {"fill_opacity", "fill-opacity"}), &v)) f->fo = std::clamp(v, 0.0, 1.0);
    const Json* d = first_of(o, {"dashed"});
    f->da = d && d->type == Json::BOOL && d->b;
    const Json* a = first_of(o, {"arrow"});
    f->ar = a && a->type == Json::BOOL && a->b;
}

const std::initializer_list<const char*> STYLE_KEYS = {
    "type", "lat", "lon", "lng", "latitude", "longitude", "points", "rings", "radius_m", "radius_km", "bearing_deg",
    "length_km", "label", "name", "title", "color", "colour", "marker-color", "stroke", "fill", "icon", "emoji", "size",
    "marker-size-px", "width", "stroke-width", "fill_opacity", "fill-opacity", "dashed", "arrow", "info", "from_station",
    "marker-symbol", "marker-size", "stroke-opacity"};

// one of the tool's own features
bool from_tool(const Json& o, const double* station, Feature* f, std::string* err) {
    if (o.type != Json::OBJ) {
        *err = "a feature isn't an object";
        return false;
    }
    std::string type = lower(o.str("type"));
    const Json* pts = o.get("points");
    const Json* rings = o.get("rings");
    if (type.empty()) type = rings ? "polygon" : pts ? "line" : "point";
    style_from(o, f);
    if (const Json* info = o.get("info"); info && info->type == Json::OBJ) info_from(*info, {}, f);
    LL ll{};
    double la = NAN, lo = NAN;
    const bool have_ll = jnum(first_of(o, {"lat", "latitude"}), &la) && jnum(first_of(o, {"lon", "lng", "longitude"}), &lo);
    if (type == "point" || type == "marker" || type == "circle") {
        if (!have_ll || !ll_ok(la, lo, &ll)) {
            *err = "a " + type + " needs lat / lon (degrees)";
            return false;
        }
        f->c = {{ll}};
        if (type == "circle") {
            double r = NAN, rk;
            if (!jnum(o.get("radius_m"), &r) && jnum(o.get("radius_km"), &rk)) r = rk * 1000;
            if (!(r >= 1 && r <= 3e6)) {
                *err = "a circle needs radius_m (1 m - 3000 km)";
                return false;
            }
            f->g = 'c';
            f->r = r;
        } else {
            f->g = 'p';
        }
        return true;
    }
    if (type == "line" || type == "polyline") {
        std::vector<LL> v;
        if (!points_of(pts, false, &v) || v.size() < 2) {
            *err = "a line needs points: [[lat, lon], ...] (2 or more)";
            return false;
        }
        f->g = 'l';
        f->c = {v};
        return true;
    }
    if (type == "polygon" || type == "area") {
        f->g = 'a';
        if (rings && rings->type == Json::ARR) {
            for (const auto& r : rings->a) {
                std::vector<LL> v;
                if (!points_of(&r, false, &v) || v.size() < 3) {
                    *err = "a polygon ring needs 3 or more [lat, lon] points";
                    return false;
                }
                f->c.push_back(v);
            }
        } else {
            std::vector<LL> v;
            if (!points_of(pts, false, &v) || v.size() < 3) {
                *err = "a polygon needs points: [[lat, lon], ...] (3 or more)";
                return false;
            }
            f->c = {v};
        }
        return !f->c.empty();
    }
    if (type == "bearing") {
        double b, len = 30;
        if (!jnum(o.get("bearing_deg"), &b)) {
            *err = "a bearing needs bearing_deg (true north, clockwise)";
            return false;
        }
        jnum(o.get("length_km"), &len);
        len = std::clamp(len, 0.1, 5000.0);
        if (have_ll) {
            if (!ll_ok(la, lo, &ll)) {
                *err = "bad lat / lon";
                return false;
            }
        } else if (station) {
            ll = {station[0], station[1]};
        } else {
            *err = "a bearing needs lat / lon (the station's position isn't known)";
            return false;
        }
        b = std::fmod(std::fmod(b, 360.0) + 360.0, 360.0);
        // along the great circle: drawn bent on the map's projection
        std::vector<LL> v;
        const int n = std::clamp(static_cast<int>(len / 20), 1, 64);
        for (int i = 0; i <= n; i++) v.push_back(i ? dest(ll.la, ll.lo, b, len * i / n) : ll);
        f->g = 'l';
        f->c = {v};
        f->ar = true;
        if (f->l.empty()) f->l = fmt(b, 1) + "°";
        f->x.insert(f->x.begin(), {"Bearing", fmt(b, 1) + "° (true), " + fmt(len, 1) + " km"});
        return true;
    }
    *err = "unknown feature type '" + clean(type, 20) + "' (point, line, polygon, circle, bearing)";
    return false;
}

// GeoJSON (RFC 7946: [lon, lat])
void from_geojson(const Json& j, const Json* props, std::vector<Feature>* out, std::vector<std::string>* probs,
                  int depth = 0) {
    if (j.type != Json::OBJ || depth > 4) return;
    const std::string type = j.str("type");
    auto prob = [&](const std::string& m) { if (probs->size() < 20) probs->push_back(m); };
    if (type == "FeatureCollection") {
        const Json* fs = j.get("features");
        if (!fs || fs->type != Json::ARR) return prob("a FeatureCollection without features");
        for (const auto& f : fs->a) {
            if (out->size() > MAX_FEATS) return;
            from_geojson(f, nullptr, out, probs, depth + 1);
        }
        return;
    }
    if (type == "Feature") {
        const Json* g = j.get("geometry");
        const Json* p = j.get("properties");
        if (!g || g->type != Json::OBJ) return prob("a Feature without geometry");
        return from_geojson(*g, p && p->type == Json::OBJ ? p : nullptr, out, probs, depth + 1);
    }
    auto make = [&](char g) {
        Feature f;
        f.g = g;
        if (props) {
            style_from(*props, &f);
            info_from(*props, STYLE_KEYS, &f);
        }
        return f;
    };
    const Json* co = j.get("coordinates");
    auto line = [&](const Json* c, std::vector<LL>* v) { return points_of(c, true, v) && v->size() >= 2; };
    if (type == "Point" || type == "MultiPoint") {
        std::vector<LL> v;
        LL ll;
        if (type == "Point" ? (co && point_of(*co, true, &ll) && (v.push_back(ll), true)) : points_of(co, true, &v)) {
            for (const auto& p : v) {
                Feature f = make('p');
                f.c = {{p}};
                double r;
                if (props && jnum(props->get("radius_m"), &r) && r >= 1 && r <= 3e6) {
                    f.g = 'c';
                    f.r = r;
                }
                out->push_back(f);
            }
        } else {
            prob("a bad " + type);
        }
    } else if (type == "LineString") {
        std::vector<LL> v;
        if (line(co, &v)) {
            Feature f = make('l');
            f.c = {v};
            out->push_back(f);
        } else {
            prob("a bad LineString");
        }
    } else if (type == "MultiLineString" || type == "Polygon") {
        if (!co || co->type != Json::ARR) return prob("a bad " + type);
        Feature poly = make('a');
        for (const auto& part : co->a) {
            std::vector<LL> v;
            if (!points_of(&part, true, &v) || v.size() < (type == "Polygon" ? 3u : 2u)) { prob("a bad " + type); continue; }
            if (type == "Polygon") {
                poly.c.push_back(v);
            } else {
                Feature f = make('l');
                f.c = {v};
                out->push_back(f);
            }
        }
        if (type == "Polygon" && !poly.c.empty()) out->push_back(poly);
    } else if (type == "MultiPolygon") {
        if (!co || co->type != Json::ARR) return prob("a bad MultiPolygon");
        for (const auto& pg : co->a) {
            Json g;
            g.type = Json::OBJ;
            g.o.push_back({"type", Json{}});
            g.o.back().second.type = Json::STR;
            g.o.back().second.s = "Polygon";
            g.o.push_back({"coordinates", pg});
            from_geojson(g, props, out, probs, depth + 1);
        }
    } else if (type == "GeometryCollection") {
        if (const Json* gs = j.get("geometries"); gs && gs->type == Json::ARR)
            for (const auto& g : gs->a) from_geojson(g, props, out, probs, depth + 1);
    } else {
        prob("unknown GeoJSON type '" + clean(type, 30) + "'");
    }
}

// CSV / TSV with a header: lat / latitude, lon / lng / longitude, a label
// column (name / label / title / callsign / site / id), optional color; the
// rest goes into the popup
std::vector<std::string> csv_row(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (quoted) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
            else if (c == '"') quoted = false;
            else cur += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    out.push_back(cur);
    for (auto& s : out) {
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
    }
    return out;
}
void from_csv(const std::string& text, std::vector<Feature>* out, std::vector<std::string>* probs, size_t* skipped) {
    std::istringstream in(text);
    std::string head;
    while (std::getline(in, head) && head.find_first_not_of(" \t\r") == std::string::npos) {}
    if (!head.empty() && static_cast<unsigned char>(head[0]) == 0xEF) head.erase(0, 3);   // a UTF-8 BOM
    const char sep = head.find('\t') != std::string::npos ? '\t'
                     : std::count(head.begin(), head.end(), ';') > std::count(head.begin(), head.end(), ',') ? ';' : ',';
    const auto cols = csv_row(head, sep);
    int ila = -1, ilo = -1, ilab = -1, icol = -1;
    for (int i = 0; i < static_cast<int>(cols.size()); i++) {
        const std::string c = lower(cols[static_cast<size_t>(i)]);
        if (ila < 0 && (c == "lat" || c == "latitude" || c == "y")) ila = i;
        else if (ilo < 0 && (c == "lon" || c == "lng" || c == "long" || c == "longitude" || c == "x")) ilo = i;
        else if (icol < 0 && (c == "color" || c == "colour")) icol = i;
        else if (ilab < 0 && (c == "name" || c == "label" || c == "title" || c == "callsign" || c == "site" || c == "id")) ilab = i;
    }
    if (ila < 0 || ilo < 0) {
        probs->push_back("the CSV has no lat / lon (or latitude / longitude) columns in its first line");
        return;
    }
    std::string line;
    while (std::getline(in, line) && out->size() <= MAX_FEATS) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const auto v = csv_row(line, sep);
        double la, lo;
        LL ll;
        Json a, b;
        a.type = b.type = Json::STR;
        a.s = static_cast<size_t>(ila) < v.size() ? v[static_cast<size_t>(ila)] : "";
        b.s = static_cast<size_t>(ilo) < v.size() ? v[static_cast<size_t>(ilo)] : "";
        if (!jnum(&a, &la) || !jnum(&b, &lo) || !ll_ok(la, lo, &ll)) {
            (*skipped)++;
            continue;
        }
        Feature f;
        f.c = {{ll}};
        if (ilab >= 0 && static_cast<size_t>(ilab) < v.size()) f.l = clean(v[static_cast<size_t>(ilab)], 80);
        if (icol >= 0 && static_cast<size_t>(icol) < v.size()) f.col = colour(v[static_cast<size_t>(icol)]);
        for (size_t i = 0; i < cols.size() && i < v.size() && f.x.size() < MAX_INFO; i++) {
            if (static_cast<int>(i) == ila || static_cast<int>(i) == ilo || static_cast<int>(i) == icol || v[i].empty()) continue;
            f.x.push_back({clean(cols[i], 40), clean(v[i], 300)});
        }
        out->push_back(f);
    }
}

// a feature in the page's compact form
std::string serialize(const Feature& f) {
    std::string o = "{\"g\":\"";
    o += f.g == 'p' ? "pt" : f.g == 'l' ? "ln" : f.g == 'a' ? "pg" : "ci";
    o += "\",\"c\":";
    auto pt = [](const LL& p) { return "[" + fmt(p.la, 6) + "," + fmt(p.lo, 6) + "]"; };
    auto ring = [&](const std::vector<LL>& r) {
        std::string s = "[";
        for (size_t i = 0; i < r.size(); i++) s += (i ? "," : "") + pt(r[i]);
        return s + "]";
    };
    if (f.g == 'p' || f.g == 'c') {
        o += pt(f.c[0][0]);
    } else if (f.g == 'l') {
        o += ring(f.c[0]);
    } else {
        o += "[";
        for (size_t i = 0; i < f.c.size(); i++) o += (i ? "," : "") + ring(f.c[i]);
        o += "]";
    }
    if (f.g == 'c') o += ",\"r\":" + fmt(f.r, 1);
    if (!f.l.empty()) o += ",\"l\":" + q(f.l);
    if (!f.col.empty()) o += ",\"col\":" + q(f.col);
    if (!f.ic.empty()) o += ",\"ic\":" + q(f.ic);
    if (f.sz > 0) o += ",\"sz\":" + fmt(f.sz, 1);
    if (f.w > 0) o += ",\"w\":" + fmt(f.w, 1);
    if (f.fo >= 0) o += ",\"fo\":" + fmt(f.fo, 2);
    if (f.da) o += ",\"da\":1";
    if (f.ar) o += ",\"ar\":1";
    if (!f.x.empty()) {
        o += ",\"x\":[";
        for (size_t i = 0; i < f.x.size(); i++) o += (i ? "," : "") + std::string("[") + q(f.x[i].first) + "," + q(f.x[i].second) + "]";
        o += "]";
    }
    return o + "}";
}

// the compact form read back (the saved file): checked like new input
bool from_compact(const Json& j, Feature* f) {
    if (j.type != Json::OBJ) return false;
    const std::string g = j.str("g");
    const Json* c = j.get("c");
    if (!c) return false;
    LL ll;
    if (g == "pt" || g == "ci") {
        if (!point_of(*c, false, &ll)) return false;
        f->g = g == "pt" ? 'p' : 'c';
        f->c = {{ll}};
        if (f->g == 'c' && (!jnum(j.get("r"), &f->r) || f->r < 1 || f->r > 3e6)) return false;
    } else if (g == "ln") {
        std::vector<LL> v;
        if (!points_of(c, false, &v) || v.size() < 2) return false;
        f->g = 'l';
        f->c = {v};
    } else if (g == "pg") {
        if (c->type != Json::ARR) return false;
        f->g = 'a';
        for (const auto& r : c->a) {
            std::vector<LL> v;
            if (!points_of(&r, false, &v) || v.size() < 3) return false;
            f->c.push_back(v);
        }
        if (f->c.empty()) return false;
    } else {
        return false;
    }
    f->l = text_of(j.get("l"), 80);
    f->col = colour(j.str("col"));
    f->ic = clean(j.str("ic"), 8);
    double v;
    if (jnum(j.get("sz"), &v)) f->sz = std::clamp(v, 2.0, 24.0);
    if (jnum(j.get("w"), &v)) f->w = std::clamp(v, 0.5, 10.0);
    if (jnum(j.get("fo"), &v)) f->fo = std::clamp(v, 0.0, 1.0);
    f->da = jnum(j.get("da"), &v) && v != 0;
    f->ar = jnum(j.get("ar"), &v) && v != 0;
    if (const Json* x = j.get("x"); x && x->type == Json::ARR)
        for (const auto& kv : x->a)
            if (kv.type == Json::ARR && kv.a.size() == 2 && f->x.size() < MAX_INFO)
                f->x.push_back({text_of(&kv.a[0], 40), text_of(&kv.a[1], 300)});
    return true;
}

void grow_bbox(Layer& L, const Feature& f) {
    for (const auto& r : f.c)
        for (const auto& p : r) {
            L.s = std::min(L.s, p.la);
            L.n = std::max(L.n, p.la);
            L.w = std::min(L.w, p.lo);
            L.e = std::max(L.e, p.lo);
        }
    if (f.g == 'c') {   // the circle's extent
        const double dla = f.r / 111320.0, dlo = dla / std::max(0.01, std::cos(f.c[0][0].la * M_PI / 180));
        L.s = std::min(L.s, f.c[0][0].la - dla);
        L.n = std::max(L.n, f.c[0][0].la + dla);
        L.w = std::min(L.w, f.c[0][0].lo - dlo);
        L.e = std::max(L.e, f.c[0][0].lo + dlo);
    }
}

std::string layer_json(const Layer& L) {
    std::string o = "{\"id\":" + q(L.id) + ",\"name\":" + q(L.name) + ",\"desc\":" + q(L.desc) + ",\"src\":" + q(L.src) +
                    ",\"conv\":" + q(L.conv) + ",\"col\":" + q(L.col) + ",\"t\":" + std::to_string(L.t) + ",\"f\":[";
    for (size_t i = 0; i < L.f.size(); i++) o += (i ? "," : "") + L.f[i];
    return o + "]}";
}
size_t total_locked() {
    size_t n = 64;
    for (const auto& L : g_layers) n += L.bytes + 200 + L.name.size() + L.desc.size() + L.src.size();
    return n;
}
void save_locked() {
    mkdir("ai", 0755);
    std::string o = "{\"version\":1,\"layers\":[";
    for (size_t i = 0; i < g_layers.size(); i++) o += (i ? ",\n" : "\n") + layer_json(g_layers[i]);
    o += "\n]}\n";
    const std::string tmp = std::string(FILE_PATH) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    const bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
    fclose(f);
    if (ok) rename(tmp.c_str(), FILE_PATH);
}

// a file the agent may hand in: in its conversation folder or ai/knowledge
bool read_input_file(const std::string& path_in, const std::string& conv_dir, std::string* text, std::string* err) {
    std::string path = path_in;
    if (path.empty()) return false;
    if (path[0] != '/') path = (conv_dir.empty() ? std::string("ai/knowledge") : conv_dir) + "/" + path;
    char rp[PATH_MAX], base[PATH_MAX];
    if (!realpath(path.c_str(), rp)) {
        *err = "file not found: " + clean(path_in, 200);
        return false;
    }
    bool ok = false;
    for (const std::string& d : {conv_dir, std::string("ai/knowledge")}) {
        if (d.empty() || !realpath(d.c_str(), base)) continue;
        const size_t n = strlen(base);
        if (strncmp(rp, base, n) == 0 && rp[n] == '/') ok = true;
    }
    if (!ok) {
        *err = "files are read from your conversation folder or ai/knowledge only";
        return false;
    }
    struct stat st{};
    if (stat(rp, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<size_t>(st.st_size) > MAX_FILE) {
        *err = "not a file, or over 8 MB";
        return false;
    }
    std::ifstream f(rp, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    *text = ss.str();
    return true;
}

Layer* find_locked(const std::string& which) {
    const std::string w = lower(which), s = slug(which);
    for (auto& L : g_layers)
        if (L.id == which || L.id == s || lower(L.name) == w) return &L;
    return nullptr;
}

}  // namespace

void load() {
    std::ifstream f(FILE_PATH);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    Json j;
    if (!geo::json_parse(ss.str(), &j) || j.type != Json::OBJ) {
        std::cerr << "AI map layers: " << FILE_PATH << " can't be read - renamed .bad" << std::endl;
        rename(FILE_PATH, (std::string(FILE_PATH) + ".bad").c_str());
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_layers.clear();
    const Json* ls = j.get("layers");
    if (!ls || ls->type != Json::ARR) return;
    for (const auto& lj : ls->a) {
        if (g_layers.size() >= MAX_LAYERS) break;
        Layer L;
        L.id = lj.str("id");
        if (!valid_id(L.id) || find_locked(L.id)) continue;
        L.name = clean(lj.str("name"), 60);
        L.desc = clean(lj.str("desc"), 300);
        L.src = clean(lj.str("src"), 300);
        L.conv = clean(lj.str("conv"), 64);
        L.col = colour(lj.str("col"));
        if (L.col.empty()) L.col = PALETTE[g_layers.size() % 10];
        double t = 0;
        jnum(lj.get("t"), &t);
        L.t = static_cast<int64_t>(t);
        if (const Json* fs = lj.get("f"); fs && fs->type == Json::ARR)
            for (const auto& fj : fs->a) {
                Feature ft;
                if (L.f.size() >= MAX_FEATS || !from_compact(fj, &ft)) continue;
                L.f.push_back(serialize(ft));
                L.bytes += L.f.back().size() + 1;
                L.verts += ft.verts();
                grow_bbox(L, ft);
            }
        if (!L.f.empty()) g_layers.push_back(std::move(L));
    }
    std::cout << "AI map layers: " << g_layers.size() << " loaded" << std::endl;
}

std::string plot(const Json& a, const std::string& conv_dir, const std::string& conv, const double* station, std::string* summary) {
    const std::string name = clean(a.str("layer"), 60);
    const std::string id = slug(name);
    if (id.empty()) return "layer: a name for the layer is needed (e.g. \"Licensed sites 162 MHz\")";
    std::vector<Feature> feats;
    std::vector<std::string> probs;
    size_t skipped = 0;
    if (const Json* fs = a.get("features"); fs && fs->type == Json::ARR) {
        for (const auto& fj : fs->a) {
            if (feats.size() > MAX_FEATS) break;
            Feature f;
            std::string err;
            if (from_tool(fj, station, &f, &err)) {
                feats.push_back(std::move(f));
            } else {
                skipped++;
                if (probs.size() < 20) probs.push_back(err);
            }
        }
    }
    if (const Json* g = a.get("geojson"); g && g->type == Json::OBJ) {
        const size_t before = feats.size();
        from_geojson(*g, nullptr, &feats, &probs);
        if (feats.size() == before && probs.empty()) probs.push_back("the GeoJSON held no features");
    }
    const std::string file = a.str("file");
    if (!file.empty()) {
        std::string text, err;
        if (!read_input_file(file, conv_dir, &text, &err)) return err;
        const std::string ext = lower(file.substr(file.rfind('.') == std::string::npos ? file.size() : file.rfind('.')));
        if (ext == ".geojson" || ext == ".json") {
            Json g;
            if (!geo::json_parse(text, &g)) return "the file isn't valid JSON";
            from_geojson(g, nullptr, &feats, &probs);
        } else if (ext == ".csv" || ext == ".tsv" || ext == ".txt") {
            from_csv(text, &feats, &probs, &skipped);
        } else {
            return "a .geojson / .json (GeoJSON) or .csv / .tsv file";
        }
    }
    if (feats.empty()) {
        std::string m = "nothing to plot";
        for (size_t i = 0; i < probs.size() && i < 5; i++) m += (i ? "; " : ": ") + probs[i];
        return m + " (features: [{type, lat, lon, label, ...}], geojson or file)";
    }
    const bool replace = !a.get("replace") || a.get("replace")->type != Json::BOOL || a.get("replace")->b;
    std::lock_guard<std::mutex> lk(g_mu);
    Layer* L = find_locked(id);
    const bool existed = L != nullptr;
    if (!L) {
        if (g_layers.size() >= MAX_LAYERS) return "the map already has " + std::to_string(MAX_LAYERS) + " AI layers - remove some (map_remove)";
        Layer n;
        n.id = id;
        // a colour no other layer has
        for (const char* c : PALETTE)
            if (std::none_of(g_layers.begin(), g_layers.end(), [&](const Layer& x) { return x.col == c; })) {
                n.col = c;
                break;
            }
        if (n.col.empty()) n.col = PALETTE[g_layers.size() % 10];
        g_layers.push_back(n);
        L = &g_layers.back();
    }
    const Layer before = *L;
    L->name = name;
    if (a.get("description")) L->desc = clean(a.str("description"), 300);
    if (a.get("source")) L->src = clean(a.str("source"), 300);
    if (const std::string c = colour(a.str("color")); !c.empty()) L->col = c;
    L->conv = conv;
    L->t = now_ms();
    if (replace) {
        L->f.clear();
        L->bytes = L->verts = 0;
        L->s = 90; L->w = 180; L->n = -90; L->e = -180;
    }
    size_t added = 0;
    for (const auto& f : feats) {
        if (L->f.size() >= MAX_FEATS || L->verts + f.verts() > MAX_VERTS) {
            probs.push_back("the layer is full (" + std::to_string(MAX_FEATS) + " features / " + std::to_string(MAX_VERTS) +
                            " points) - the rest left out");
            break;
        }
        L->f.push_back(serialize(f));
        L->bytes += L->f.back().size() + 1;
        L->verts += f.verts();
        grow_bbox(*L, f);
        added++;
    }
    if (total_locked() > MAX_TOTAL) {
        const size_t kb = total_locked() / 1024;
        if (existed) *L = before;
        else g_layers.pop_back();
        return "too much on the map (" + std::to_string(kb) + " KB of 1024 for all AI layers) - fewer features, "
               "simpler lines, or remove old layers";
    }
    g_seq++;
    save_locked();
    std::ostringstream o;
    o << "{\"layer\":" << q(L->id) << ",\"name\":" << q(L->name) << ",\"features\":" << L->f.size() << ",\"added\":" << added
      << ",\"skipped\":" << skipped << ",\"replaced\":" << (existed && replace ? "true" : "false") << ",\"bbox\":["
      << fmt(L->s, 5) << "," << fmt(L->w, 5) << "," << fmt(L->n, 5) << "," << fmt(L->e, 5) << "],\"layers\":" << g_layers.size()
      << ",\"size_kb\":" << total_locked() / 1024;
    if (!probs.empty()) {
        o << ",\"problems\":[";
        for (size_t i = 0; i < probs.size() && i < 8; i++) o << (i ? "," : "") << q(probs[i]);
        o << "]";
    }
    o << "}";
    *summary = o.str();
    return "";
}

std::string remove(const std::string& which, std::string* summary) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (lower(which) == "all") {
        const size_t n = g_layers.size();
        g_layers.clear();
        g_seq++;
        save_locked();
        *summary = "{\"removed\":" + std::to_string(n) + ",\"layers\":0}";
        return "";
    }
    Layer* L = find_locked(which);
    if (!L) return "no AI layer '" + clean(which, 60) + "' (map_layers lists them)";
    const std::string id = L->id;
    g_layers.erase(std::remove_if(g_layers.begin(), g_layers.end(), [&](const Layer& x) { return x.id == id; }), g_layers.end());
    g_seq++;
    save_locked();
    *summary = "{\"removed\":" + q(id) + ",\"layers\":" + std::to_string(g_layers.size()) + "}";
    return "";
}

std::string list_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string o = "{\"layers\":[";
    for (size_t i = 0; i < g_layers.size(); i++) {
        const auto& L = g_layers[i];
        char t[32] = "";
        const time_t s = static_cast<time_t>(L.t / 1000);
        strftime(t, sizeof t, "%Y-%m-%d %H:%M", localtime(&s));
        o += (i ? "," : "") + std::string("{\"id\":") + q(L.id) + ",\"name\":" + q(L.name) + ",\"description\":" + q(L.desc) +
             ",\"source\":" + q(L.src) + ",\"features\":" + std::to_string(L.f.size()) + ",\"created\":" + q(t) +
             ",\"conversation\":" + q(L.conv) + ",\"color\":" + q(L.col) + ",\"bbox\":[" + fmt(L.s, 5) + "," + fmt(L.w, 5) +
             "," + fmt(L.n, 5) + "," + fmt(L.e, 5) + "]}";
    }
    return o + "],\"size_kb\":" + std::to_string(total_locked() / 1024) + ",\"limit_kb\":1024}";
}

std::string brief_json() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string o = "[";
    for (size_t i = 0; i < g_layers.size(); i++)
        o += (i ? "," : "") + std::string("{\"id\":") + q(g_layers[i].id) + ",\"name\":" + q(g_layers[i].name) +
             ",\"features\":" + std::to_string(g_layers[i].f.size()) + "}";
    return o + "]";
}

std::string message() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string o = "{\"ai_map\":{\"seq\":" + std::to_string(g_seq) + ",\"layers\":[";
    for (size_t i = 0; i < g_layers.size(); i++) o += (i ? "," : "") + layer_json(g_layers[i]);
    return o + "]}}";
}

bool bbox(const std::string& id, double* s, double* w, double* n, double* e) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Layer* L = find_locked(id);
    if (!L || L->f.empty()) return false;
    *s = L->s;
    *w = L->w;
    *n = L->n;
    *e = L->e;
    return true;
}

std::string find(const std::string& which) {
    std::lock_guard<std::mutex> lk(g_mu);
    const Layer* L = find_locked(which);
    return L ? L->id : std::string();
}

}  // namespace aimap
