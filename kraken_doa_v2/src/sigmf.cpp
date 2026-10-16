// ============================================
// src/sigmf.cpp - SigMF metadata, archives (tar) and SHA-512
// ============================================
// See include/sigmf.hpp and SIGMF.md.

#include "sigmf.hpp"

#include "geo_address.hpp"   // geo::json_parse

#include <openssl/evp.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace sigmf {

// --- time ----------------------------------------------------------------------
std::string iso_utc(int64_t ms) {
    const time_t s = static_cast<time_t>(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
    const int frac = static_cast<int>(ms - static_cast<int64_t>(s) * 1000);
    struct tm tm{};
    gmtime_r(&s, &tm);
    char b[96];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, frac);
    return b;
}

bool parse_iso(const std::string& s, int64_t* out) {
    int Y, M, D, h, m;
    double sec;
    int n = 0;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%lf%n", &Y, &M, &D, &h, &m, &sec, &n) != 6) return false;
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || !(sec >= 0 && sec < 61)) return false;
    struct tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = 0;
    int64_t ms = static_cast<int64_t>(timegm(&tm)) * 1000 + static_cast<int64_t>(std::llround(sec * 1000));
    const char* z = s.c_str() + n;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        if (sscanf(z + 1, "%2d:%2d", &oh, &om) < 1) return false;
        const int64_t off = (oh * 60 + om) * 60000LL;
        ms += *z == '+' ? -off : off;
    }
    *out = ms;
    return true;
}

// --- SHA-512 ---------------------------------------------------------------------
Sha512::Sha512() { reset(); }
Sha512::~Sha512() {
    if (ctx_) EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}
void Sha512::reset() {
    if (!ctx_) ctx_ = EVP_MD_CTX_new();
    if (ctx_) EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx_), EVP_sha512(), nullptr);
}
void Sha512::update(const void* p, size_t n) {
    if (ctx_ && n) EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), p, n);
}
std::string Sha512::hex() {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!ctx_ || EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), md, &len) != 1) return "";
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < len; i++) {
        out += hx[md[i] >> 4];
        out += hx[md[i] & 15];
    }
    reset();
    return out;
}
std::string sha512_hex(const std::string& data) {
    Sha512 h;
    h.update(data.data(), data.size());
    return h.hex();
}

// --- tar --------------------------------------------------------------------------
namespace {

bool pread_all(int fd, void* buf, size_t n, uint64_t off) {
    auto* p = static_cast<char*>(buf);
    while (n) {
        const ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
        off += static_cast<uint64_t>(r);
    }
    return true;
}

// a numeric header field: octal text, or GNU base-256 (high bit of byte 0)
bool tar_num(const unsigned char* f, size_t len, uint64_t* out) {
    if (f[0] & 0x80) {
        uint64_t v = f[0] & 0x7F;
        for (size_t i = 1; i < len; i++) {
            if (v >> 55) return false;
            v = (v << 8) | f[i];
        }
        *out = v;
        return true;
    }
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && (f[i] == ' ' || f[i] == 0)) i++;
    for (; i < len && f[i] >= '0' && f[i] <= '7'; i++) v = (v << 3) | static_cast<uint64_t>(f[i] - '0');
    *out = v;
    return true;
}

std::string field(const unsigned char* f, size_t len) {
    size_t n = 0;
    while (n < len && f[n]) n++;
    return std::string(reinterpret_cast<const char*>(f), n);
}

bool checksum_ok(const unsigned char* h) {
    uint64_t want = 0;
    tar_num(h + 148, 8, &want);
    uint64_t u = 0;
    int64_t s = 0;
    for (int i = 0; i < 512; i++) {
        const unsigned char c = (i >= 148 && i < 156) ? ' ' : h[i];
        u += c;
        s += static_cast<signed char>(c);
    }
    return u == want || static_cast<int64_t>(want) == s;
}

// pax records "len key=value\n" -> path / size
void pax_parse(const std::string& rec, std::string* path, uint64_t* size, bool* have_size) {
    size_t p = 0;
    while (p < rec.size()) {
        const size_t sp = rec.find(' ', p);
        if (sp == std::string::npos) break;
        const size_t len = static_cast<size_t>(strtoull(rec.c_str() + p, nullptr, 10));
        if (len == 0 || p + len > rec.size()) break;
        const std::string kv = rec.substr(sp + 1, p + len - sp - 2);   // without the \n
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "path") *path = v;
            else if (k == "size") { *size = strtoull(v.c_str(), nullptr, 10); *have_size = true; }
        }
        p += len;
    }
}

void put_octal(char* f, size_t len, uint64_t v) {
    // len - 1 digits + NUL
    std::string s(len - 1, '0');
    for (size_t i = len - 1; i-- > 0;) {
        s[i] = static_cast<char>('0' + (v & 7));
        v >>= 3;
    }
    memcpy(f, s.data(), len - 1);
    f[len - 1] = 0;
}

std::string ustar_block(const std::string& name, const std::string& prefix, uint64_t size, int64_t mtime, char type,
                        int mode) {
    char h[512];
    memset(h, 0, sizeof h);
    memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
    put_octal(h + 100, 8, static_cast<uint64_t>(mode));
    put_octal(h + 108, 8, 0);
    put_octal(h + 116, 8, 0);
    put_octal(h + 124, 12, size);
    put_octal(h + 136, 12, static_cast<uint64_t>(std::max<int64_t>(0, mtime)));
    memset(h + 148, ' ', 8);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);   // + NUL
    memcpy(h + 263, "00", 2);
    memcpy(h + 265, "kraken", 6);
    memcpy(h + 297, "kraken", 6);
    memcpy(h + 345, prefix.data(), std::min<size_t>(prefix.size(), 155));
    unsigned sum = 0;
    for (unsigned char c : std::string(h, 512)) sum += c;
    char ck[8];
    snprintf(ck, sizeof ck, "%06o", sum);
    memcpy(h + 148, ck, 6);
    h[154] = 0;
    h[155] = ' ';
    return std::string(h, 512);
}

std::string pax_record(const std::string& key, const std::string& value) {
    const std::string body = " " + key + "=" + value + "\n";
    size_t len = body.size() + 1;
    while (std::to_string(len).size() + body.size() != len) len = std::to_string(len).size() + body.size();
    return std::to_string(len) + body;
}

std::string header(const std::string& name, uint64_t size, int64_t mtime, char type, int mode) {
    std::string prefix, short_name = name;
    bool need_pax = size >= (1ULL << 33);   // 11 octal digits hold < 8 GiB
    if (name.size() > 100) {
        // ustar: prefix (<= 155) "/" name (<= 100)
        bool split = false;
        for (size_t s = name.find('/'); s != std::string::npos; s = name.find('/', s + 1)) {
            if (s <= 155 && name.size() - s - 1 <= 100 && s > 0 && s + 1 < name.size()) {
                prefix = name.substr(0, s);
                short_name = name.substr(s + 1);
                split = true;
                break;
            }
        }
        if (!split) {
            need_pax = true;
            prefix.clear();
            short_name = name.substr(0, 100);
        }
    }
    std::string out;
    if (need_pax) {
        std::string recs;
        if (name.size() > 100 && prefix.empty()) recs += pax_record("path", name);
        if (size >= (1ULL << 33)) recs += pax_record("size", std::to_string(size));
        std::string base = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
        const std::string pname = ("PaxHeaders/" + base).substr(0, 100);
        out += ustar_block(pname, "", recs.size(), mtime, 'x', 0644);
        out += recs;
        out.append(tar_pad(recs.size()), '\0');
    }
    out += ustar_block(short_name, prefix, size >= (1ULL << 33) ? 0 : size, mtime, type, mode);
    return out;
}

}  // namespace

std::string tar_file_header(const std::string& name, uint64_t size, int64_t mtime) {
    return header(name, size, mtime, '0', 0644);
}

std::string tar_dir_header(const std::string& name, int64_t mtime) {
    return header(name.empty() || name.back() == '/' ? name : name + "/", 0, mtime, '5', 0755);
}

bool tar_index(const std::string& path, std::vector<TarMember>* out, std::string* err) {
    out->clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return *err = "can't open " + path, false;
    struct stat st{};
    fstat(fd, &st);
    const uint64_t fsize = static_cast<uint64_t>(st.st_size);
    uint64_t off = 0;
    std::string long_name, pax_path;
    uint64_t pax_size = 0;
    bool pax_have_size = false;
    bool ok = true;
    int zero_blocks = 0;
    while (off + 512 <= fsize) {
        unsigned char h[512];
        if (!pread_all(fd, h, 512, off)) { ok = false; *err = "read error"; break; }
        bool zero = true;
        for (unsigned char c : h) if (c) { zero = false; break; }
        if (zero) {
            if (++zero_blocks == 2) break;
            off += 512;
            continue;
        }
        zero_blocks = 0;
        if (!checksum_ok(h)) { ok = false; *err = "not a tar file (bad header checksum at " + std::to_string(off) + ")"; break; }
        uint64_t size = 0, mtime = 0;
        tar_num(h + 124, 12, &size);
        tar_num(h + 136, 12, &mtime);
        const char type = static_cast<char>(h[156]);
        if (type == 'x' || type == 'g' || type == 'L') {
            if (size > (1 << 20)) { ok = false; *err = "oversized tar extension header"; break; }
            std::string body(static_cast<size_t>(size), '\0');
            if (size && !pread_all(fd, body.data(), body.size(), off + 512)) { ok = false; *err = "read error"; break; }
            if (type == 'x') pax_parse(body, &pax_path, &pax_size, &pax_have_size);
            else if (type == 'L') long_name = body.substr(0, body.find('\0'));
            off += 512 + size + tar_pad(size);
            continue;
        }
        std::string name = field(h, 100);
        const std::string magic = field(h + 257, 6);
        if (magic.rfind("ustar", 0) == 0) {
            const std::string pre = field(h + 345, 155);
            if (!pre.empty()) name = pre + "/" + name;
        }
        if (!long_name.empty()) name = long_name;
        if (!pax_path.empty()) name = pax_path;
        if (pax_have_size) size = pax_size;
        long_name.clear();
        pax_path.clear();
        pax_have_size = false;
        if (type == '0' || type == '\0' || type == '7') {
            while (name.rfind("./", 0) == 0) name.erase(0, 2);
            if (off + 512 + size > fsize) { ok = false; *err = "the archive is cut short (" + name + ")"; break; }
            out->push_back({name, off + 512, size, static_cast<int64_t>(mtime)});
        }
        off += 512 + size + tar_pad(size);
    }
    ::close(fd);
    if (ok && out->empty()) { *err = "no files in the archive"; ok = false; }
    return ok;
}

// --- datatypes ----------------------------------------------------------------------
bool parse_datatype(const std::string& s, Datatype* dt, std::string* err) {
    *dt = Datatype();
    if (s.size() < 3 || s[0] != 'c') {
        *err = s.empty() ? "no core:datatype" : "datatype " + s + " is not complex (the receiver needs I and Q)";
        return false;
    }
    const char k = s[1];
    if (k != 'u' && k != 'i' && k != 'f') return *err = "unknown datatype " + s, false;
    size_t p = 2;
    int bits = 0;
    while (p < s.size() && isdigit(static_cast<unsigned char>(s[p]))) bits = bits * 10 + (s[p++] - '0');
    const std::string end = s.substr(p);
    bool big = false;
    if (bits == 8) {
        if (!end.empty()) return *err = "unknown datatype " + s, false;
    } else if (end == "_le") {
    } else if (end == "_be") {
        big = true;
    } else {
        return *err = "datatype " + s + " needs _le or _be", false;
    }
    const bool fl = k == 'f';
    if ((fl && bits != 32 && bits != 64) || (!fl && bits != 8 && bits != 16 && bits != 32))
        return *err = "unsupported datatype " + s, false;
    dt->kind = k;
    dt->bits = bits;
    dt->big = big;
    return true;
}

// --- reading -----------------------------------------------------------------------
namespace {

using geo::Json;

bool ends_with(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

std::string dir_of(const std::string& p) {
    const size_t sl = p.rfind('/');
    return sl == std::string::npos ? "" : p.substr(0, sl);
}

std::string base_of(const std::string& p) {
    const size_t sl = p.rfind('/');
    return sl == std::string::npos ? p : p.substr(sl + 1);
}

double num(const Json* j, double def) { return j && j->type == Json::NUM ? j->n : def; }
std::string str(const Json* j) { return j && j->type == Json::STR ? j->s : ""; }

// the files of a recording: a folder, or the members of an archive
struct Files {
    std::string tar;                               // "" = a folder
    std::map<std::string, std::pair<uint64_t, uint64_t>> at;   // name -> offset, size
    std::string folder;

    bool read(const std::string& name, std::string* text, size_t max = 64 << 20) const {
        auto it = at.find(name);
        if (it == at.end() || it->second.second > max) return false;
        const std::string path = tar.empty() ? folder + "/" + name : tar;
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        text->assign(static_cast<size_t>(it->second.second), '\0');
        const bool ok = text->empty() || pread_all(fd, text->data(), text->size(), it->second.first);
        ::close(fd);
        return ok;
    }
};

bool list_folder(const std::string& dir, Files* f) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    f->folder = dir;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        struct stat st{};
        if (stat((dir + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        f->at[n] = {0, static_cast<uint64_t>(st.st_size)};
    }
    closedir(d);
    return true;
}

bool parse_point(const Json& p, std::array<double, 3>* out) {
    const Json* pt = p.get("point");
    if (!pt || pt->type != Json::ARR || pt->a.size() < 2) return false;
    for (size_t i = 0; i < 3; i++) (*out)[i] = i < pt->a.size() && pt->a[i].type == Json::NUM ? pt->a[i].n : 0.0;
    return true;
}

void read_geometry(const Json* g, std::vector<std::array<double, 3>>* out) {
    if (!g || g->type != Json::ARR || !out->empty()) return;
    std::vector<std::array<double, 3>> v;
    for (const auto& p : g->a) {
        std::array<double, 3> xyz{};
        if (!parse_point(p, &xyz)) return;   // an unknown point: no geometry
        v.push_back(xyz);
    }
    *out = v;
}

// the krakensdr namespace + the descriptive fields of an object (collection
// or a metadata file's global)
void read_kraken(const Json& o, Recording* r) {
    auto take = [&](const char* k, std::string* dst) {
        const std::string v = str(o.get(k));
        if (dst->empty() && !v.empty()) *dst = v;
    };
    take("core:description", &r->description);
    take("core:author", &r->author);
    take("core:hw", &r->hw);
    take("core:recorder", &r->recorder);
    take("krakensdr:format", &r->format);
    take("krakensdr:vfos", &r->decimators);
    if (const Json* c = o.get("krakensdr:array_commands"); c && c->type == Json::ARR && r->commands.empty())
        for (const auto& s : c->a)
            if (s.type == Json::STR) r->commands.push_back(s.s);
    if (const Json* b = o.get("krakensdr:band"); b && b->type == Json::OBJ) {
        r->band_offset_hz = num(b->get("offset_hz"), r->band_offset_hz);
        r->tuner_hz = num(b->get("tuner_hz"), r->tuner_hz);
    }
    if (const Json* s = o.get("krakensdr:station"); s && s->type == Json::OBJ && !r->station_valid) {
        const double la = num(s->get("lat"), NAN), lo = num(s->get("lon"), NAN);
        if (std::isfinite(la) && std::isfinite(lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180) {
            r->station_valid = true;
            r->station_lat = la;
            r->station_lon = lo;
            r->station_heading = num(s->get("heading"), -1);
        }
        r->station_id = str(s->get("id"));
        r->station_source = str(s->get("source"));
    }
    if (const Json* t = o.get("krakensdr:track"); t && t->type == Json::ARR && r->track.empty()) {
        for (const auto& p : t->a) {
            if (p.type != Json::ARR || p.a.size() < 3) continue;
            std::array<double, 4> v{num(&p.a[0], -1), num(&p.a[1], NAN), num(&p.a[2], NAN),
                                    p.a.size() > 3 ? num(&p.a[3], -1) : -1};
            if (v[0] >= 0 && std::isfinite(v[1]) && std::isfinite(v[2])) r->track.push_back(v);
        }
    }
    read_geometry(o.get("spatial:element_geometry"), &r->geometry);
    // core:geolocation (GeoJSON point [lon, lat, alt]) when no krakensdr:station
    if (!r->station_valid) {
        if (const Json* g = o.get("core:geolocation"); g && g->type == Json::OBJ) {
            const Json* c = g->get("coordinates");
            if (c && c->type == Json::ARR && c->a.size() >= 2 && c->a[0].type == Json::NUM && c->a[1].type == Json::NUM &&
                std::fabs(c->a[1].n) <= 90 && std::fabs(c->a[0].n) <= 180) {
                r->station_valid = true;
                r->station_lon = c->a[0].n;
                r->station_lat = c->a[1].n;
            }
        }
    }
}

bool load_stream(const Files& f, const std::string& meta_name, Stream* s, Json* meta, std::string* err) {
    std::string text;
    if (!f.read(meta_name, &text)) return *err = "can't read " + meta_name, false;
    if (!geo::json_parse(text, meta) || meta->type != Json::OBJ) return *err = meta_name + " is not valid JSON", false;
    const Json* g = meta->get("global");
    if (!g || g->type != Json::OBJ) return *err = meta_name + " has no global object", false;
    s->name = meta_name.substr(0, meta_name.size() - std::string(".sigmf-meta").size());
    if (!parse_datatype(str(g->get("core:datatype")), &s->dt, err)) return *err = meta_name + ": " + *err, false;
    s->rate = num(g->get("core:sample_rate"), 0);
    if (!(s->rate > 0)) return *err = meta_name + ": no core:sample_rate", false;
    s->nch = static_cast<int>(num(g->get("core:num_channels"), 1));
    if (s->nch < 1 || s->nch > 64) return *err = meta_name + ": bad core:num_channels", false;
    s->index = static_cast<int>(num(g->get("spatial:channel_index"), -1));
    s->iq_scale = num(g->get("krakensdr:iq_scale"), 0);
    if (meta->get("global")->get("core:metadata_only") && meta->get("global")->get("core:metadata_only")->b)
        return *err = meta_name + " is metadata only (no samples)", false;
    // the dataset: <base>.sigmf-data, or a non-conforming one (core:dataset)
    std::string data = s->name + ".sigmf-data";
    if (const std::string ds = str(g->get("core:dataset")); !ds.empty()) {
        if (ds.find('/') != std::string::npos || ds.find("..") != std::string::npos)
            return *err = meta_name + ": core:dataset must be a file next to it", false;
        const std::string d = dir_of(meta_name);
        data = d.empty() ? ds : d + "/" + ds;
    }
    auto it = f.at.find(data);
    if (it == f.at.end()) return *err = "missing " + data, false;
    s->data_path = f.tar.empty() ? f.folder + "/" + data : f.tar;
    s->data_offset = it->second.first;
    s->data_bytes = it->second.second;
    uint64_t trailing = static_cast<uint64_t>(num(g->get("core:trailing_bytes"), 0));
    if (trailing > s->data_bytes) trailing = s->data_bytes;
    s->data_bytes -= trailing;
    if (const Json* caps = meta->get("captures"); caps && caps->type == Json::ARR) {
        uint64_t hdr = 0;
        for (const auto& c : caps->a) {
            if (c.type != Json::OBJ) continue;
            Capture cap;
            cap.sample_start = static_cast<uint64_t>(std::max(0.0, num(c.get("core:sample_start"), 0)));
            cap.global_index = static_cast<uint64_t>(std::max(0.0, num(c.get("core:global_index"), static_cast<double>(cap.sample_start))));
            cap.freq_hz = num(c.get("core:frequency"), 0);
            if (const std::string dt = str(c.get("core:datetime")); !dt.empty()) parse_iso(dt, &cap.t_ms);
            cap.gain_db = num(c.get("krakensdr:gain_db"), -999);
            cap.phase_state = static_cast<int>(num(c.get("krakensdr:phase_state"), -1));
            const Json* cal = c.get("spatial:calibration");
            const Json* ns = c.get("krakensdr:noise_source");
            cap.calibration = (cal && cal->type == Json::OBJ) || (ns && ns->type == Json::BOOL && ns->b);
            cap.heading = num(c.get("spatial:aperture_azimuth"), -1);
            hdr += static_cast<uint64_t>(num(c.get("core:header_bytes"), 0));
            s->captures.push_back(cap);
        }
        if (hdr) {
            // a header before the samples (only the first capture's is supported)
            const uint64_t first = static_cast<uint64_t>(num(caps->a.empty() ? nullptr : caps->a[0].get("core:header_bytes"), 0));
            if (first != hdr) return *err = meta_name + ": header bytes inside the dataset are not supported", false;
            if (first > s->data_bytes) return *err = meta_name + ": header longer than the dataset", false;
            s->data_offset += first;
            s->data_bytes -= first;
        }
    }
    std::sort(s->captures.begin(), s->captures.end(),
              [](const Capture& a, const Capture& b) { return a.sample_start < b.sample_start; });
    if (s->captures.empty()) s->captures.push_back(Capture());
    const uint64_t bps = static_cast<uint64_t>(s->dt.bytes()) * static_cast<uint64_t>(s->nch);
    s->data_bytes -= s->data_bytes % bps;
    if (s->data_bytes == 0) return *err = data + " holds no samples", false;
    return true;
}

}  // namespace

bool open_recording(const std::string& path_in, Recording* r, std::string* err) {
    *r = Recording();
    std::string path = path_in;
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    r->path = path;
    Files f;
    std::string want_coll, want_meta;   // one collection / metadata file asked for
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return *err = "not found: " + path, false;
    if (S_ISDIR(st.st_mode)) {
        r->form = "folder";
        r->name = base_of(path);
        if (!list_folder(path, &f)) return *err = "can't read the folder " + path, false;
    } else if (ends_with(path, ".sigmf")) {
        r->form = "archive";
        r->name = base_of(path).substr(0, base_of(path).size() - 6);
        std::vector<TarMember> mem;
        if (!tar_index(path, &mem, err)) return false;
        f.tar = path;
        for (const auto& m : mem) f.at[m.name] = {m.offset, m.size};
    } else if (ends_with(path, ".sigmf-collection") || ends_with(path, ".sigmf-meta")) {
        r->form = "meta";
        const std::string d = dir_of(path);
        if (!list_folder(d.empty() ? "." : d, &f)) return *err = "can't read the folder of " + path, false;
        (ends_with(path, ".sigmf-meta") ? want_meta : want_coll) = base_of(path);
        r->name = base_of(path).substr(0, base_of(path).rfind('.'));
    } else {
        return *err = "not a SigMF recording (a folder, .sigmf, .sigmf-collection or .sigmf-meta)", false;
    }

    // which metadata files, in which order
    std::vector<std::string> metas;
    Json coll;
    bool have_coll = false;
    std::string coll_name = want_coll;
    if (coll_name.empty() && want_meta.empty()) {
        std::vector<std::string> cs;
        for (const auto& [n, _] : f.at)
            if (ends_with(n, ".sigmf-collection")) cs.push_back(n);
        if (cs.size() > 1) {
            // several: the one named like the recording, else refuse
            for (const auto& c : cs)
                if (base_of(c) == r->name + ".sigmf-collection") coll_name = c;
            if (coll_name.empty()) return *err = "several .sigmf-collection files - open one of them", false;
        } else if (cs.size() == 1) {
            coll_name = cs[0];
        }
    }
    if (!coll_name.empty()) {
        std::string text;
        if (!f.read(coll_name, &text)) return *err = "can't read " + coll_name, false;
        Json j;
        if (!geo::json_parse(text, &j) || !j.get("collection") || j.get("collection")->type != Json::OBJ)
            return *err = coll_name + " is not a SigMF collection", false;
        coll = *j.get("collection");
        have_coll = true;
        const std::string cdir = dir_of(coll_name);
        const Json* streams = coll.get("core:streams");
        if (!streams || streams->type != Json::ARR || streams->a.empty()) return *err = coll_name + " lists no streams", false;
        for (const auto& s : streams->a) {
            std::string nm;
            if (s.type == Json::OBJ) nm = str(s.get("name"));
            else if (s.type == Json::ARR && !s.a.empty()) nm = str(&s.a[0]);   // the older [name, hash] tuples
            if (nm.empty() || nm.find("..") != std::string::npos) return *err = coll_name + ": a stream without a name", false;
            std::string m = (cdir.empty() ? "" : cdir + "/") + nm + ".sigmf-meta";
            if (!f.at.count(m)) {
                // anywhere in the archive / folder by its file name
                m.clear();
                for (const auto& [n, _] : f.at)
                    if (base_of(n) == nm + ".sigmf-meta") { m = n; break; }
            }
            if (m.empty()) return *err = "the collection names " + nm + " but its .sigmf-meta is missing", false;
            metas.push_back(m);
        }
    } else if (!want_meta.empty()) {
        metas.push_back(want_meta);
    } else {
        for (const auto& [n, _] : f.at)
            if (ends_with(n, ".sigmf-meta")) metas.push_back(n);   // std::map: sorted by name
        if (metas.empty()) return *err = "no .sigmf-meta in " + path, false;
    }

    std::vector<Json> meta_json(metas.size());
    for (size_t i = 0; i < metas.size(); i++) {
        Stream s;
        if (!load_stream(f, metas[i], &s, &meta_json[i], err)) return false;
        r->streams.push_back(std::move(s));
    }
    // channel order: spatial:channel_index when every stream has a distinct one
    {
        bool all = true;
        std::vector<int> seen;
        for (const auto& s : r->streams) {
            if (s.index < 0 || std::find(seen.begin(), seen.end(), s.index) != seen.end()) all = false;
            seen.push_back(s.index);
        }
        if (all && r->streams.size() > 1) {
            std::vector<size_t> ord(r->streams.size());
            for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return r->streams[a].index < r->streams[b].index; });
            std::vector<Stream> s2;
            std::vector<Json> j2;
            for (size_t i : ord) {
                s2.push_back(std::move(r->streams[i]));
                j2.push_back(std::move(meta_json[i]));
            }
            r->streams = std::move(s2);
            meta_json = std::move(j2);
        }
    }
    // one datatype and rate
    const Stream& s0 = r->streams[0];
    r->dt = s0.dt;
    r->datatype = str(meta_json[0].get("global")->get("core:datatype"));
    r->rate = s0.rate;
    r->samples = UINT64_MAX;
    for (const auto& s : r->streams) {
        if (s.dt.kind != s0.dt.kind || s.dt.bits != s0.dt.bits || s.dt.big != s0.dt.big)
            return *err = "the channels have different datatypes", false;
        if (std::fabs(s.rate - s0.rate) > 1e-6 * s0.rate) return *err = "the channels have different sample rates", false;
        r->channels += s.nch;
        r->samples = std::min<uint64_t>(r->samples, s.data_bytes / (static_cast<uint64_t>(s.dt.bytes()) * s.nch));
    }
    if (have_coll) read_kraken(coll, r);
    read_kraken(*meta_json[0].get("global"), r);
    if (r->geometry.empty()) {
        // the extension also allows it in the captures
        if (const Json* caps = meta_json[0].get("captures"); caps && caps->type == Json::ARR && !caps->a.empty())
            read_geometry(caps->a[0].get("spatial:element_geometry"), &r->geometry);
    }
    if (static_cast<int>(r->geometry.size()) != r->channels) r->geometry.clear();
    r->start_ms = s0.captures.front().t_ms;
    return true;
}

}  // namespace sigmf
