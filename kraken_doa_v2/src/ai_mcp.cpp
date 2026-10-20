#include "ai_mcp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "App.h"
#include "ai_capture.hpp"
#include "ai_manager.hpp"
#include "ai_map.hpp"
#include "channel_manager.hpp"
#include "config.hpp"
#include "continuous_scanner.hpp"
#include "control_handler.hpp"
#include "decimator_manager.hpp"
#include "decoder_log.hpp"
#include "digital/dig_plugin.hpp"
#include "digital/digital_decoder.hpp"
#include "doa_logger.hpp"
#include "geo_address.hpp"   // geo::json_parse
#include "globals.hpp"
#include "incidents.hpp"
#include "message_builders.hpp"
#include "networking/websocket_server.hpp"
#include "playback.hpp"
#include "rdf_mapper.hpp"
#include "saved_freqs.hpp"
#include "scanner_manager.hpp"
#include "station_info.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include "utils/process.hpp"
#include "voice2text.hpp"

namespace ai_mcp {
namespace {

using geo::Json;
using Res = uWS::HttpResponse<false>;
using Reply = std::function<void(std::string text, bool error)>;

uWS::App* g_app = nullptr;
uWS::Loop* g_loop = nullptr;
int g_port = 0;
std::atomic<bool> g_listening{false};
std::string g_token;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string num(double v, int decimals = -1) {
    if (!is_finite_value(v)) return "null";
    char b[64];
    if (decimals >= 0) snprintf(b, sizeof b, "%.*f", decimals, v);
    else if (std::fabs(v) < 1e15 && v == std::floor(v)) snprintf(b, sizeof b, "%.0f", v);
    else snprintf(b, sizeof b, "%.10g", v);
    return b;
}

std::string q(const std::string& s) { return "\"" + json_escape(s) + "\""; }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void dump(const Json& j, std::string& o) {
    switch (j.type) {
        case Json::NUL: o += "null"; break;
        case Json::BOOL: o += j.b ? "true" : "false"; break;
        case Json::NUM: o += num(j.n); break;
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

// --- tool arguments ---
bool has(const Json& a, const char* k) {
    const Json* x = a.get(k);
    return x && x->type != Json::NUL;
}
double num_arg(const Json& a, const char* k, double def) {
    const Json* x = a.get(k);
    if (!x) return def;
    if (x->type == Json::NUM && is_finite_value(x->n)) return x->n;
    if (x->type == Json::STR) {   // "433.92e6" from a model that quoted it
        char* end = nullptr;
        double v = strtod(x->s.c_str(), &end);
        if (end && end != x->s.c_str() && is_finite_value(v)) return v;
    }
    return def;
}
std::string str_arg(const Json& a, const char* k, const std::string& def = "") {
    const Json* x = a.get(k);
    if (!x) return def;
    if (x->type == Json::STR) return x->s;
    if (x->type == Json::NUM) return num(x->n);
    if (x->type == Json::BOOL) return x->b ? "true" : "false";
    return def;
}
bool bool_arg(const Json& a, const char* k, bool def) {
    const Json* x = a.get(k);
    if (!x) return def;
    if (x->type == Json::BOOL) return x->b;
    if (x->type == Json::NUM) return x->n != 0;
    if (x->type == Json::STR) return x->s == "true" || x->s == "1" || x->s == "on" || x->s == "yes";
    return def;
}

// work on a thread, the answer back on the uWS loop
void later(std::function<std::string(bool* error)> work, Reply reply) {
    std::thread([work = std::move(work), reply = std::move(reply)] {
        bool err = false;
        std::string text;
        try {
            text = work(&err);
        } catch (const std::exception& e) {
            text = std::string("failed: ") + e.what();
            err = true;
        }
        g_loop->defer([reply, text, err] { reply(text, err); });
    }).detach();
}

std::string random_token();

// ---- commands the asking PAGE carries out (its map, its viewshed / path
// loss - per browser, computed there): {"ai_page":{id, page, op, args}} to
// every page, the one whose id it is acks at once (AI_PAGE_REPLY {id, ack})
// and answers when done ({id, ok, result | error}) ----
struct PageCall {
    std::mutex mu;
    std::condition_variable cv;
    bool acked = false, done = false, ok = false;
    std::string result;
};
std::mutex g_pc_mu;
std::map<std::string, std::shared_ptr<PageCall>> g_pc;

std::string page_cmd(const std::string& id, const std::string& page, const std::string& op, const std::string& args) {
    return "{\"ai_page\":{\"id\":" + q(id) + ",\"page\":" + q(page) + ",\"op\":" + q(op) + ",\"args\":" + args + "}}";
}

void page_call(const std::string& op, const std::string& args_json, int timeout_ms, Reply r) {
    const std::string page = AiManager::instance().target_page();
    if (page.empty())
        return r("No page to do it in: the map and its viewshed live in the user's browser - ask from the receiver's web page", true);
    auto pc = std::make_shared<PageCall>();
    const std::string id = random_token().substr(0, 20);
    {
        std::lock_guard<std::mutex> lk(g_pc_mu);
        g_pc[id] = pc;
    }
    WebSocketServer::broadcast_json_message(page_cmd(id, page, op, args_json));
    later([pc, id, timeout_ms](bool* err) {
        std::string out;
        {
            std::unique_lock<std::mutex> lk(pc->mu);
            bool got = pc->cv.wait_for(lk, std::chrono::seconds(8), [&] { return pc->acked || pc->done; });
            if (got && !pc->done) pc->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return pc->done; });
            if (!pc->acked && !pc->done) {
                *err = true;
                out = "The user's page didn't answer (closed, or opened before this receiver was updated - it needs a reload)";
            } else if (!pc->done) {
                *err = true;
                out = "The page is still working on it after " + std::to_string(timeout_ms / 1000) +
                      " s - viewshed_get shows its progress";
            } else {
                *err = !pc->ok;
                out = pc->result;
            }
        }
        std::lock_guard<std::mutex> lk(g_pc_mu);
        g_pc.erase(id);
        return out;
    }, r);
}
// fire and forget (map_plot's "show it")
void page_tell(const std::string& op, const std::string& args_json) {
    const std::string page = AiManager::instance().target_page();
    if (!page.empty()) WebSocketServer::broadcast_json_message(page_cmd(random_token().substr(0, 20), page, op, args_json));
}

// ---------------------------------------------------------------------------
// The receiver, as the tools see it (uWS loop thread)
// ---------------------------------------------------------------------------
const char* mode_name() {
    return independent_mode_enabled.load() ? "independent" : wideband_mode_enabled.load() ? "wideband" : "coherent";
}

double tuner_center_hz(int tuner) {
    if (multi_tuner_mode()) return static_cast<double>(exact_tuner_hz(tuner));
    const uint64_t c = coherent_tuner_hz();
    return c ? static_cast<double>(c) : static_cast<double>(ChannelManager::get_frequency(0));
}

struct Vfo {
    DecimatorManager::DecimatorInfo d;
    int tuner = 0;
    double center = 0, hz = 0, bw = 0;
};

std::vector<Vfo> vfos() {
    std::vector<Vfo> out;
    for (const auto& d : decimator_manager.getDecimatorInfoList()) {
        Vfo v;
        v.d = d;
        if (multi_tuner_mode())
            if (auto inst = decimator_manager.getDecimator(d.id)) v.tuner = inst->tuner_channel.load();
        v.center = tuner_center_hz(v.tuner);
        v.hz = v.center + d.frequency_offset_hz;
        v.bw = d.bandwidth_mhz * 1e6;
        out.push_back(v);
    }
    return out;
}

bool find_vfo(int id, Vfo* out) {
    for (const auto& v : vfos())
        if (v.d.id == id) {
            *out = v;
            return true;
        }
    return false;
}

// the spectrum display holding hz (independent: that tuner's), -1 = none
int display_for(double hz, int* tuner_out = nullptr) {
    const SpectrumSnapshot& s = MessageBuilders::spectrum_snapshot();
    if (s.independent) {
        for (int ch = 0; ch < s.channels && ch < MAX_CHANNELS; ch++) {
            const auto& d = s.disp[1 + ch];
            if (!d.valid || d.db.size() < 2) continue;
            const double lo = d.f0_hz, hi = d.f0_hz + d.step_hz * static_cast<double>(d.db.size() - 1);
            if (hz >= lo && hz <= hi) {
                if (tuner_out) *tuner_out = ch;
                return 1 + ch;
            }
        }
        return -1;
    }
    const auto& d = s.disp[0];
    if (!d.valid || d.db.size() < 2) return -1;
    const double lo = d.f0_hz, hi = d.f0_hz + d.step_hz * static_cast<double>(d.db.size() - 1);
    return hz >= lo && hz <= hi ? 0 : -1;
}

// strongest level (dB above the noise floor) in [lo, hi]
double band_peak_db(double lo, double hi) {
    const int di = display_for((lo + hi) / 2);
    if (di < 0) return NAN;
    const auto& d = MessageBuilders::spectrum_snapshot().disp[di];
    const auto n = static_cast<long>(d.db.size());
    long a = static_cast<long>(std::ceil((lo - d.f0_hz) / d.step_hz)), b = static_cast<long>(std::floor((hi - d.f0_hz) / d.step_hz));
    a = std::clamp(a, 0L, n - 1);
    b = std::clamp(b, 0L, n - 1);
    if (b < a) b = a;
    float m = -1e9f;
    for (long i = a; i <= b; i++) m = std::max(m, d.db[static_cast<size_t>(i)]);
    return m;
}

// the band the receiver (or a tuner) shows: [lo, hi] Hz
bool display_band(int di, double* lo, double* hi) {
    if (di < 0 || di > MAX_CHANNELS) return false;
    const auto& d = MessageBuilders::spectrum_snapshot().disp[di];
    if (!d.valid || d.db.size() < 2) return false;
    *lo = d.f0_hz;
    *hi = d.f0_hz + d.step_hz * static_cast<double>(d.db.size() - 1);
    return true;
}

// the VFOs' bearings from the DF status (rdf_mapper): vfo id -> {peak, conf}
struct Brg { double peak = NAN, conf = 0; long frames = 0; };
std::vector<std::pair<int, Brg>> vfo_bearings(Json* status_out = nullptr) {
    std::vector<std::pair<int, Brg>> out;
    Json j;
    if (!geo::json_parse(rdfmap::status_message(true), &j)) return out;
    const Json* r = j.get("rdf");
    if (!r) return out;
    if (const Json* vs = r->get("vfos"); vs && vs->type == Json::ARR)
        for (const auto& v : vs->a) {
            Brg b;
            if (const Json* x = v.get("peak"); x && x->type == Json::NUM) b.peak = x->n;
            if (const Json* x = v.get("conf"); x && x->type == Json::NUM) b.conf = x->n;
            if (const Json* x = v.get("frames"); x && x->type == Json::NUM) b.frames = static_cast<long>(x->n);
            const Json* id = v.get("id");
            if (id && id->type == Json::NUM) out.push_back({static_cast<int>(id->n), b});
        }
    if (status_out) *status_out = *r;
    return out;
}

// a VFO's own DoA, from its MUSIC processor: array = MUSIC's angle
// (counter-clockwise from antenna 0 - the DoA plot's "unit circle" display),
// rel = clockwise from the array's front (the plot's default "compass"
// display, 360 - array), compass = heading + rel when the heading is known
struct VfoDoa {
    bool ok = false;
    double array = 0, rel = 0, compass = -1, conf = 0;
};
VfoDoa vfo_doa(int id) {
    VfoDoa r;
    if (!doa_enabled.load() || multi_tuner_mode()) return r;
    auto inst = decimator_manager.getDecimator(id);
    if (!inst || !inst->music_processor) return r;
    const auto& mp = *inst->music_processor;
    if (mp.getResultStampMs() == 0 || mp.isResultStale()) return r;
    auto [ang, conf] = mp.getPeakAngleWithConfidence();
    if (!is_finite_value(ang)) return r;
    r.ok = true;
    r.array = std::fmod(ang + 360.0, 360.0);
    r.rel = std::fmod(360.0 - r.array, 360.0);
    r.conf = conf;
    StationLocation s = station_info.resolve();
    if (s.heading_valid) r.compass = std::fmod(s.heading + r.rel + 360.0, 360.0);
    return r;
}

std::string doa_json(const VfoDoa& d) {
    if (!d.ok) return "null";
    return "{\"array_deg\":" + num(d.array, 1) + ",\"clockwise_from_front_deg\":" + num(d.rel, 1) +
           ",\"compass_deg\":" + (d.compass >= 0 ? num(d.compass, 1) : std::string("null")) + ",\"confidence\":" +
           num(d.conf, 2) + "}";
}

const char* squelch_method_name(int m) { return m == 3 ? "DIGITAL" : "FFT"; }

std::string station_json() {
    StationLocation s = station_info.resolve();
    std::ostringstream o;
    o << "{\"callsign\":" << q(s.id) << ",\"source\":" << q(s.source_str());
    if (s.lat != 0 || s.lon != 0) o << ",\"lat\":" << num(s.lat, 6) << ",\"lon\":" << num(s.lon, 6);
    else o << ",\"lat\":null,\"lon\":null";
    o << ",\"heading_deg\":" << (s.heading_valid ? num(s.heading, 1) : "null") << ",\"speed_mps\":" << num(s.speed, 1) << "}";
    return o.str();
}

// the decoder's status, parsed (mode, detected, facts, ...)
bool decoder_status(int vfo, size_t events, bool tables, Json* out) {
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst) return false;
    auto dd = inst->getDigital();
    if (!dd) return false;
    return geo::json_parse(dd->status_json(0, events, tables), out);
}

// the facts of the plugin that leads (fixed, or what Auto detect found) as
// {key: value}
std::string facts_json(const Json& st, size_t max) {
    std::string lead = st.str("detected");
    const Json* info = st.get("info");
    std::string o = "{";
    size_t n = 0;
    if (info && info->type == Json::OBJ)
        for (const auto& [pid, facts] : info->o) {
            if (!lead.empty() && pid != lead) continue;
            if (facts.type != Json::ARR) continue;
            for (const auto& f : facts.a) {
                if (f.type != Json::ARR || f.a.size() < 2 || n >= max) continue;
                const std::string k = f.a[0].type == Json::STR ? f.a[0].s : "";
                const std::string v = f.a[1].type == Json::STR ? f.a[1].s : dump(f.a[1]);
                if (k.empty()) continue;
                o += (n ? "," : "") + q(k) + ":" + q(v);
                n++;
            }
        }
    return o + "}";
}

}  // namespace

// ---------------------------------------------------------------------------
std::string screen_json(const std::string& view_json) {
    const auto list = vfos();
    std::ostringstream o;
    o << "{\"page\":" << (view_json.empty() ? "{}" : view_json);
    // receiver
    o << ",\"receiver\":{\"mode\":" << q(mode_name());
    const bool multi = multi_tuner_mode();
    if (!multi) {
        double lo = 0, hi = 0;
        o << ",\"center_hz\":" << num(tuner_center_hz(0), 0) << ",\"gain_db\":"
          << num(ChannelManager::get_gain(active_channel.load()), 1);
        if (display_band(0, &lo, &hi)) o << ",\"span_hz\":[" << num(lo, 0) << "," << num(hi, 0) << "]";
    } else {
        o << ",\"tuners\":[";
        const int n = std::min<int>(active_num_elements.load(), MAX_CHANNELS);
        for (int ch = 0; ch < n; ch++)
            o << (ch ? "," : "") << "{\"tuner\":" << ch << ",\"center_hz\":" << num(tuner_center_hz(ch), 0)
              << ",\"gain_db\":" << num(ChannelManager::get_gain(ch), 1) << "}";
        o << "]";
        double lo = 0, hi = 0;
        if (wideband_mode_enabled.load() && display_band(0, &lo, &hi))
            o << ",\"span_hz\":[" << num(lo, 0) << "," << num(hi, 0) << "]";
    }
    o << ",\"sample_rate_hz\":" << num(SAMPLE_RATE, 0) << ",\"antennas\":" << active_num_elements.load()
      << ",\"doa\":" << (doa_enabled.load() ? "true" : "false") << ",\"beamforming\":"
      << (beamforming_enabled.load() ? "true" : "false") << ",\"calibrating\":"
      << (doa_is_calibrating() ? "true" : "false") << ",\"playback\":" << (playback::active() ? "true" : "false")
      << ",\"discrete_scanner\":" << (scanner_manager.isRunning() ? "true" : "false")
      << ",\"continuous_scanner\":" << (continuous_scanner.isRunning() ? "true" : "false")
      << ",\"assistant_control\":" << q(AiManager::instance().readonly() ? "read only" : "allowed") << "}";
    o << ",\"station\":" << station_json();
    o << ",\"ai_map_layers\":" << aimap::brief_json();
    // VFOs
    o << ",\"vfos\":[";
    for (size_t i = 0; i < list.size(); i++) {
        const Vfo& v = list[i];
        const auto& d = v.d;
        o << (i ? "," : "") << "{\"vfo\":" << d.id << ",\"freq_hz\":" << num(v.hz, 0) << ",\"bandwidth_hz\":"
          << num(v.bw, 0) << ",\"demod\":" << q(DecimatorManager::demodModeToString(d.demod_mode));
        if (multi) o << ",\"tuner\":" << v.tuner;
        o << ",\"heard\":" << (d.audio_out ? "true" : "false") << ",\"level_db\":"
          << num(band_peak_db(v.hz - v.bw / 2, v.hz + v.bw / 2), 1);
        o << ",\"squelch\":{\"on\":" << (d.squelch_enabled ? "true" : "false") << ",\"method\":"
          << q(squelch_method_name(d.squelch_method)) << ",\"level_db\":" << num(d.squelch_level, 1)
          << ",\"open\":" << (d.squelch_open ? "true" : "false") << "}";
        Json st;
        if (d.digital_mode != 0 && decoder_status(d.id, 0, false, &st)) {
            o << ",\"decoder\":{\"mode\":" << q(st.str("mode")) << ",\"detected\":" << q(st.str("detected"))
              << ",\"detected_name\":" << q(st.str("detected_name")) << ",\"facts\":" << facts_json(st, 14) << "}";
        } else {
            o << ",\"decoder\":null";
        }
        if (d.transcribe) o << ",\"transcribe\":true";
        o << ",\"doa\":" << doa_json(vfo_doa(d.id)) << "}";
    }
    o << "]";
    o << ",\"notes\":" << q(
        "Levels are dB above the noise floor of the averaged spectrum. doa (coherent mode, DoA on): "
        "clockwise_from_front_deg = what the MUSIC DoA plot shows by default (0 = the array's front, antenna 0, "
        "clockwise), array_deg = 360 minus that (counter-clockwise, the plot's unit-circle option), compass_deg "
        "= the true bearing when the station heading is known. page = what the user's browser shows (zoom "
        "window, selected VFO, open tab, map view).");
    o << "}";
    return o.str();
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------
namespace {

struct Tool {
    const char* name;
    const char* title;
    const char* desc;
    const char* schema;      // JSON Schema of the arguments
    bool control;            // refused in Read only
    bool read_only_hint;
    std::function<void(const Json& args, Reply reply)> fn;
};
std::vector<Tool>& tools();

}  // namespace
}  // namespace ai_mcp

// ---------------------------------------------------------------------------
// Reading tools
// ---------------------------------------------------------------------------
namespace ai_mcp {
namespace {

void t_get_screen(const Json&, Reply r) { r(screen_json(AiManager::instance().view_json()), false); }

// levels + the signals in them, for a dB-over-the-floor spectrum db (bin i at
// f0 + i * step), window bins [i0, i1]
std::string spectrum_json(const std::vector<float>& db, double f0, double step, double center, long i0, long i1,
                          long points, double thr, int tuner, double hold_s, double hold_floor) {
    const long count = i1 - i0 + 1;
    const long pool = std::max(1L, (count + points - 1) / points);
    std::ostringstream o;
    o << "{\"center_hz\":" << num(center, 0);
    if (tuner >= 0) o << ",\"tuner\":" << tuner;
    if (hold_s > 0)
        o << ",\"peak_hold_s\":" << num(hold_s, 1) << ",\"hold_noise_db\":" << num(hold_floor, 1);
    o << ",\"start_hz\":" << num(f0 + (i0 + (pool - 1) / 2.0) * step, 0) << ",\"step_hz\":" << num(step * pool, 2)
      << ",\"fft_bin_hz\":" << num(step, 2) << ",\"levels_db\":[";
    for (long i = i0, k = 0; i <= i1; i += pool, k++) {
        float m = db[static_cast<size_t>(i)];
        for (long j = i + 1; j < std::min(i + pool, i1 + 1); j++) m = std::max(m, db[static_cast<size_t>(j)]);
        o << (k ? "," : "") << num(std::round(m * 10) / 10.0, 1);
    }
    // signals: bins over thr, runs closer than ~2 kHz joined (an FM signal's
    // sideband lines are one signal), each with its peak and width
    const long gap = std::max(2L, static_cast<long>(std::ceil(2000.0 / step)));
    struct Pk { double hz, peak_hz, db, w; };
    std::vector<Pk> pk;
    for (long i = i0; i <= i1;) {
        if (db[static_cast<size_t>(i)] < thr) { i++; continue; }
        long j = i, last = i;
        while (j + 1 <= i1 && j + 1 - last <= gap) {
            j++;
            if (db[static_cast<size_t>(j)] >= thr) last = j;
        }
        j = last;
        long p = i;
        for (long k = i; k <= j; k++)
            if (db[static_cast<size_t>(k)] > db[static_cast<size_t>(p)]) p = k;
        // its width: the bins within 20 dB of its peak (a strong carrier's
        // window leakage stays over min_db for kHz either side)
        const float top = db[static_cast<size_t>(p)];
        long l = j, rr = i;
        for (long k = i; k <= j; k++)
            if (db[static_cast<size_t>(k)] >= top - 20) {
                l = std::min(l, k);
                rr = std::max(rr, k);
            }
        pk.push_back({f0 + (l + rr) / 2.0 * step, f0 + p * step, top, (rr - l + 1) * step});
        i = j + 1;
    }
    std::sort(pk.begin(), pk.end(), [](const Pk& x, const Pk& y) { return x.db > y.db; });
    o << "],\"signals\":[";
    for (size_t k = 0; k < pk.size() && k < 30; k++)
        o << (k ? "," : "") << "{\"freq_hz\":" << num(pk[k].hz, 0) << ",\"peak_hz\":" << num(pk[k].peak_hz, 0)
          << ",\"level_db\":" << num(pk[k].db, 1) << ",\"width_hz\":" << num(pk[k].w, 0) << "}";
    o << "],\"signals_found\":" << pk.size() << ",\"note\":"
      << q(hold_s > 0
               ? "PEAK HOLD: the maximum of the raw (unaveraged) spectra over peak_hold_s, dB over the averaged "
                 "noise floor - noise itself reaches about hold_noise_db there, so a signal is what stands well "
                 "above that (min_db is applied as given). Signals as below."
               : "levels in dB above the noise floor (averaged spectrum, max of each point's FFT bins); signals = "
                 "the bins over min_db (gaps under ~2 kHz joined): each one's centre + width (the part within 20 dB "
                 "of its peak) and its strongest bin, strongest first. Bursty signals (sondes, pagers, LoRa, ADS-B) "
                 "barely show in the average: use hold_s. A 2.4 MHz receiver also shows images / the DC spur of "
                 "strong signals; the band edges roll off.")
      << "}";
    return o.str();
}

void t_get_spectrum(const Json& a, Reply r) {
    const SpectrumSnapshot& s = MessageBuilders::spectrum_snapshot();
    int di = 0, tuner = -1;
    if (s.independent) {
        tuner = static_cast<int>(num_arg(a, "tuner", -1));
        if (tuner < 0 && has(a, "start_hz")) display_for((num_arg(a, "start_hz", 0) + num_arg(a, "stop_hz", 0)) / 2, &tuner);
        if (tuner < 0) tuner = 0;
        di = 1 + tuner;
    }
    double lo = 0, hi = 0;
    if (!display_band(di, &lo, &hi)) return r("No spectrum yet (is the receiver connected to heimdall?)", true);
    const auto& d = s.disp[di];
    double start = std::max(lo, num_arg(a, "start_hz", lo)), stop = std::min(hi, num_arg(a, "stop_hz", hi));
    if (!(stop > start)) return r("The window is outside the band the receiver shows: " + num(lo, 0) + " .. " + num(hi, 0) + " Hz", true);
    const long n = static_cast<long>(d.db.size());
    const long i0 = std::clamp(static_cast<long>(std::ceil((start - d.f0_hz) / d.step_hz)), 0L, n - 1);
    const long i1 = std::clamp(static_cast<long>(std::floor((stop - d.f0_hz) / d.step_hz)), i0, n - 1);
    const long points = std::clamp(static_cast<long>(num_arg(a, "points", 256)), 8L, 1024L);
    const double thr = num_arg(a, "min_db", 6);
    const double hold = std::clamp(num_arg(a, "hold_s", 0), 0.0, 30.0);
    if (hold <= 0)
        return r(spectrum_json(d.db, d.f0_hz, d.step_hz, d.center_hz, i0, i1, points, thr, tuner, 0, 0), false);
    if (wideband_mode_enabled.load()) return r("Peak hold isn't available in wideband scan mode (use the average)", true);
    // peak hold: the raw spectra of the display's channel, sampled every
    // 40 ms for hold s, over the averaged spectrum's floor (its 10th percentile)
    const int ch = s.independent ? tuner : active_channel.load();
    const double f0 = d.f0_hz, step = d.step_hz, center = d.center_hz;
    const int tout = s.independent ? tuner : -1;
    later([=](bool* err) {
        std::vector<float> mx(static_cast<size_t>(n), -1e9f), raw, avg;
        const int64_t until = now_ms() + static_cast<int64_t>(hold * 1000);
        int samples = 0;
        while (now_ms() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            {
                std::lock_guard<std::mutex> lk(fft_mutex);
                if (ch < 0 || ch >= static_cast<int>(fft_magnitudes.size()) || ch >= static_cast<int>(fft_averaged.size())) continue;
                raw = fft_magnitudes[static_cast<size_t>(ch)];
                avg = fft_averaged[static_cast<size_t>(ch)];
            }
            if (raw.size() != static_cast<size_t>(n) || avg.size() != raw.size()) continue;
            std::nth_element(avg.begin(), avg.begin() + static_cast<long>(avg.size() / 10), avg.end());
            const float floor = avg[avg.size() / 10];
            for (size_t k = 0; k < raw.size(); k++) mx[k] = std::max(mx[k], raw[k] - floor);
            samples++;
        }
        if (!samples) {
            *err = true;
            return std::string("No spectrum frames came in during the hold (the FFT size changed, or no data)");
        }
        std::vector<float> med(mx.begin() + i0, mx.begin() + i1 + 1);
        std::nth_element(med.begin(), med.begin() + static_cast<long>(med.size() / 2), med.end());
        return spectrum_json(mx, f0, step, center, i0, i1, points, thr, tout, hold, med[med.size() / 2]);
    }, r);
}

void t_get_decoder(const Json& a, Reply r) {
    const int id = static_cast<int>(num_arg(a, "vfo", -1));
    Vfo v;
    if (!find_vfo(id, &v)) return r("No VFO D" + std::to_string(id), true);
    Json st;
    if (v.d.digital_mode == 0 || !decoder_status(id, static_cast<size_t>(std::clamp(num_arg(a, "events", 30.0), 0.0, 200.0)), true, &st))
        return r("D" + std::to_string(id) + " has no decoder on", true);
    const size_t rows = static_cast<size_t>(std::clamp(num_arg(a, "rows", 40.0), 0.0, 300.0));
    // trim the heavy parts: table rows, plot data
    for (auto& [k, v2] : st.o) {
        if (k == "tables" && v2.type == Json::OBJ)
            for (auto& [pid, t] : v2.o)
                for (auto& [tk, tv] : t.o) {
                    if (tk == "rows" && tv.type == Json::ARR && tv.a.size() > rows) tv.a.resize(rows);
                    if (tk == "det" && tv.type == Json::OBJ) tv.o.clear();
                }
        if (k == "plots" && v2.type == Json::OBJ)
            for (auto& [pid, p] : v2.o)
                for (auto& [pk, pv] : p.o)
                    if (pk == "y" && pv.type == Json::ARR) pv.a.clear();
    }
    r("{\"vfo\":" + std::to_string(id) + ",\"freq_hz\":" + num(v.hz, 0) + ",\"status\":" + dump(st) +
      ",\"note\":" + q("events: t = ms since epoch, src = plugin id, m = text; tables: rows [key, age_s, cells..] "
                       "under cols; info = facts [key, value, age_s] per plugin") + "}", false);
}

double haversine_km(double la1, double lo1, double la2, double lo2, double* brg) {
    const double R = 6371.0088, d2r = M_PI / 180;
    const double p1 = la1 * d2r, p2 = la2 * d2r, dp = (la2 - la1) * d2r, dl = (lo2 - lo1) * d2r;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    if (brg) {
        double b = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl));
        *brg = std::fmod(b / d2r + 360, 360);
    }
    return 2 * R * std::asin(std::min(1.0, std::sqrt(h)));
}

void t_get_bearings(const Json& a, Reply r) {
    Json st;
    vfo_bearings(&st);
    if (st.type != Json::OBJ) return r("No direction finding data", true);
    const int only = static_cast<int>(num_arg(a, "vfo", -1));
    std::ostringstream o;
    o << "{\"df_state\":" << q(st.str("state")) << ",\"station\":";
    const Json* sta = st.get("station");
    o << (sta ? dump(*sta) : "null") << ",\"doa_on\":" << (doa_enabled.load() ? "true" : "false") << ",\"vfos\":[";
    bool first = true;
    for (const auto& v : vfos()) {
        if (only >= 0 && v.d.id != only) continue;
        o << (first ? "" : ",") << "{\"vfo\":" << v.d.id << ",\"freq_hz\":" << num(v.hz, 0) << ",\"doa\":"
          << doa_json(vfo_doa(v.d.id)) << "}";
        first = false;
    }
    o << "],\"talkers\":[";
    struct T { const Json* j; bool active; double age; };
    std::vector<T> ts;
    if (const Json* tl = st.get("talkers"); tl && tl->type == Json::ARR)
        for (const auto& t : tl->a) {
            const Json* v = t.get("vfo");
            if (only >= 0 && (!v || static_cast<int>(v->n) != only)) continue;
            const Json* ac = t.get("active");
            const Json* ag = t.get("age");
            ts.push_back({&t, ac && ac->type == Json::BOOL && ac->b, ag && ag->type == Json::NUM ? ag->n : 1e9});
        }
    std::sort(ts.begin(), ts.end(), [](const T& x, const T& y) { return x.active != y.active ? x.active : x.age < y.age; });
    const size_t lim = static_cast<size_t>(std::clamp(num_arg(a, "limit", 60.0), 1.0, 300.0));
    for (size_t i = 0; i < ts.size() && i < lim; i++) {
        const Json& t = *ts[i].j;
        auto f = [&](const char* k) { const Json* x = t.get(k); return x ? dump(*x) : std::string("null"); };
        o << (i ? "," : "") << "{\"vfo\":" << f("vfo") << ",\"id\":" << f("tid") << ",\"label\":" << f("label")
          << ",\"plugin\":" << f("pl") << ",\"on_air\":" << f("active") << ",\"bearing_deg\":" << f("peak")
          << ",\"array_deg\":" << f("doa") << ",\"confidence\":" << f("conf") << ",\"last_heard_s\":" << f("age")
          << ",\"transmissions\":" << f("tx");
        if (const Json* h = t.get("hist"); h && h->type == Json::ARR && !h->a.empty()) {
            Json hh = *h;
            if (hh.a.size() > 5) hh.a.resize(5);
            o << ",\"earlier\":" << dump(hh);
        }
        o << "}";
    }
    o << "],\"talkers_total\":" << ts.size() << ",\"note\":"
      << q("VFO doa: clockwise_from_front_deg = what the MUSIC DoA plot shows by default, array_deg = 360 "
           "minus that (counter-clockwise from antenna 0), compass_deg = the true bearing when the station "
           "heading is known. Talkers: bearing_deg = compass bearing when the heading is known (station.hdg), "
           "else clockwise from the array's front; array_deg = MUSIC's counter-clockwise angle; talkers = "
           "transmitters the "
           "decoders named (radios, aircraft, devices) with their own bearing; earlier = [age_s, compass, "
           "array, confidence, frames] of earlier transmissions")
      << "}";
    r(o.str(), false);
}

void t_get_map_points(const Json& a, Reply r) {
    const int only = static_cast<int>(num_arg(a, "vfo", -1));
    const std::string ql = lower(str_arg(a, "query"));
    const size_t lim = static_cast<size_t>(std::clamp(num_arg(a, "limit", 60.0), 1.0, 500.0));
    StationLocation sta = station_info.resolve();
    const bool have_sta = sta.lat != 0 || sta.lon != 0;
    struct P { double dist; std::string json; };
    std::vector<P> pts;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst || (only >= 0 && inst->id != only)) continue;
        auto dd = inst->getDigital();
        if (!dd) continue;
        std::string items, keys;
        dd->report().map_json(inst->id, 0, items, keys);
        Json arr;
        if (items.empty() || !geo::json_parse("[" + items + "]", &arr)) continue;
        for (const auto& p : arr.a) {
            const std::string blob = lower(dump(p));
            if (!ql.empty() && blob.find(ql) == std::string::npos) continue;
            const Json* la = p.get("la");
            const Json* lo = p.get("lo");
            double dist = 1e9, brg = NAN;
            std::ostringstream o;
            auto f = [&](const char* k) { const Json* x = p.get(k); return x ? dump(*x) : std::string("null"); };
            o << "{\"vfo\":" << inst->id << ",\"plugin\":" << f("p") << ",\"id\":" << f("i") << ",\"label\":" << f("l")
              << ",\"kind\":" << f("k") << ",\"lat\":" << f("la") << ",\"lon\":" << f("lo") << ",\"heading\":" << f("h")
              << ",\"alt_m\":" << f("a") << ",\"speed_kmh\":" << f("s") << ",\"info\":" << f("x") << ",\"age_s\":" << f("age");
            if (have_sta && la && lo && la->type == Json::NUM && lo->type == Json::NUM) {
                dist = haversine_km(sta.lat, sta.lon, la->n, lo->n, &brg);
                o << ",\"distance_km\":" << num(dist, 2) << ",\"bearing_from_station_deg\":" << num(brg, 1);
            }
            o << "}";
            pts.push_back({dist, o.str()});
        }
    }
    std::sort(pts.begin(), pts.end(), [](const P& x, const P& y) { return x.dist < y.dist; });
    std::string out = "{\"points\":[";
    for (size_t i = 0; i < pts.size() && i < lim; i++) out += (i ? "," : "") + pts[i].json;
    out += "],\"total\":" + std::to_string(pts.size()) + ",\"note\":" +
           q("positions decoders reported (aircraft, ships, stations, balloons ...), nearest first; only decoders "
             "with \"Plot on map\" ticked keep points") + "}";
    r(out, false);
}

void t_get_transcripts(const Json& a, Reply r) {
    Json arr;
    if (!geo::json_parse(v2t::history_json(1000), &arr) || arr.type != Json::ARR) return r("[]", false);
    const std::string ql = lower(str_arg(a, "query"));
    const int only = static_cast<int>(num_arg(a, "vfo", -1));
    const size_t lim = static_cast<size_t>(std::clamp(num_arg(a, "limit", 20.0), 1.0, 200.0));
    std::string out = "{\"transcripts\":[";
    size_t n = 0;
    for (auto it = arr.a.rbegin(); it != arr.a.rend() && n < lim; ++it) {
        const Json* v = it->get("vfo");
        if (only >= 0 && (!v || static_cast<int>(v->n) != only)) continue;
        if (!ql.empty() && lower(it->str("text") + " " + it->str("tl") + " " + it->str("tid")).find(ql) == std::string::npos) continue;
        out += (n ? "," : "") + dump(*it);
        n++;
    }
    r(out + "],\"note\":" + q("newest first; t = ms since epoch, dur = s, mhz, src = decoder / demod, tid / tl = "
                              "the talker, doa = array angle, brg = compass bearing") + "}", false);
}

void t_get_saved(const Json& a, Reply r) {
    Json j;
    if (!geo::json_parse(savedfreq::message_json(), &j)) return r("{}", false);
    const Json* sf = j.get("savedfreq");
    if (!sf) return r("{}", false);
    const std::string ql = lower(str_arg(a, "query"));
    auto name_of = [](const Json* list, const std::string& id) {
        if (list && list->type == Json::ARR)
            for (const auto& x : list->a)
                if (x.str("id") == id) return x.str("name");
        return std::string();
    };
    const Json* cats = sf->get("cats");
    const Json* locs = sf->get("locs");
    std::string out = "{\"frequencies\":[";
    size_t n = 0;
    if (const Json* fr = sf->get("freqs"); fr && fr->type == Json::ARR)
        for (const auto& f : fr->a) {
            const std::string cat = name_of(cats, f.str("cat")), loc = name_of(locs, f.str("loc"));
            const Json* hz = f.get("hz");
            const std::string mhz = hz && hz->type == Json::NUM ? num(hz->n / 1e6, 6) : "";
            if (!ql.empty() && lower(f.str("name") + " " + f.str("notes") + " " + f.str("tone") + " " + cat + " " + loc + " " + mhz)
                                       .find(ql) == std::string::npos)
                continue;
            if (n >= 300) break;
            out += (n ? "," : "") + std::string("{\"name\":") + q(f.str("name")) + ",\"freq_hz\":" + (hz ? dump(*hz) : "null") +
                   ",\"category\":" + q(cat) + ",\"location\":" + q(loc) + ",\"demod\":" + q(f.str("mode")) + ",\"decoder\":" +
                   q(f.str("dec")) + ",\"tone\":" + q(f.str("tone")) + ",\"notes\":" + q(f.str("notes")) + "}";
            n++;
        }
    out += "],\"locations\":[";
    n = 0;
    if (locs && locs->type == Json::ARR)
        for (const auto& l : locs->a) {
            if (n >= 200) break;
            auto f = [&](const char* k) { const Json* x = l.get(k); return x ? dump(*x) : std::string("null"); };
            out += (n ? "," : "") + std::string("{\"name\":") + f("name") + ",\"lat\":" + f("lat") + ",\"lon\":" + f("lon") +
                   ",\"notes\":" + f("notes") + "}";
            n++;
        }
    r(out + "]}", false);
}

void t_get_incidents(const Json&, Reply r) {
    Json j;
    if (!geo::json_parse(incidents::message_json(), &j)) return r("{}", false);
    const Json* in = j.get("incidents");
    if (!in) return r("{}", false);
    Json c = *in;
    for (auto& [k, v] : c.o)
        if (k == "rows" && v.type == Json::ARR && v.a.size() > 100) v.a.resize(100);
    r(dump(c), false);
}

void t_list_decoders(const Json&, Reply r) {
    auto list = dig::PluginRegistry::instance().list();
    std::ostringstream o;
    o << "{\"decoders\":[";
    for (size_t i = 0; i < list.size(); i++) {
        const auto& p = list[i];
        o << (i ? "," : "") << "{\"id\":" << q(p.id) << ",\"name\":" << q(p.name) << ",\"description\":" << q(p.description)
          << ",\"built\":" << (p.built ? "true" : "false") << ",\"min_vfo_bandwidth_hz\":" << num(p.min_vfo_rate, 0);
        if (p.fixed_freq_hz > 0) o << ",\"fixed_freq_hz\":" << num(p.fixed_freq_hz, 0);
        if (p.lock_vfo) o << ",\"whole_band\":true";
        if (p.manual_only) o << ",\"manual_only\":true";
        if (p.talkers) o << ",\"bearing_per_transmitter\":true";
        o << "}";
    }
    o << "],\"note\":" << q("set one on a VFO with set_vfo(decoder=<id>) or add_vfo; \"auto\" tries every decoder the user "
                           "ticked for Auto detect except manual_only ones. fixed_freq_hz decoders tune the receiver "
                           "there; whole_band ones take the tuner's whole 2.4 MHz") << "}";
    r(o.str(), false);
}

}  // namespace
}  // namespace ai_mcp

// ---------------------------------------------------------------------------
// Tools that take a while (a thread each; the answer comes back on the loop)
// ---------------------------------------------------------------------------
namespace ai_mcp {
namespace {

void t_search_log(const Json& a, Reply r) {
    const std::string query = lower(str_arg(a, "query"));
    const std::string type = str_arg(a, "type");
    const int only = static_cast<int>(num_arg(a, "vfo", -1));
    const int days = static_cast<int>(std::clamp(num_arg(a, "days", 1), 1.0, 31.0));
    const size_t lim = static_cast<size_t>(std::clamp(num_arg(a, "limit", 100.0), 1.0, 500.0));
    const std::string dir = declog::folder();
    later([=](bool*) {
        std::vector<std::string> hits;   // oldest first, the newest lim kept
        size_t matched = 0, files = 0;
        const std::string tneedle = type.empty() ? "" : "\"type\":\"" + type + "\"";
        const std::string vneedle = only < 0 ? "" : "\"vfo\":" + std::to_string(only) + ",";
        for (int k = days - 1; k >= 0; k--) {
            time_t t = time(nullptr) - static_cast<time_t>(k) * 86400;
            char day[16];
            strftime(day, sizeof day, "%Y-%m-%d", localtime(&t));
            for (const char* ext : {".jsonl", ".jsonl.gz"}) {
                const std::string path = dir + "/decoders-" + day + ext;
                if (access(path.c_str(), R_OK) != 0) continue;
                gzFile f = gzopen(path.c_str(), "rb");   // reads plain files too
                if (!f) continue;
                files++;
                std::vector<char> buf(65536);
                while (gzgets(f, buf.data(), static_cast<int>(buf.size()))) {
                    std::string line(buf.data());
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                    if (line.empty() || line[0] != '{') continue;
                    if (!tneedle.empty() && line.find(tneedle) == std::string::npos) continue;
                    if (!vneedle.empty() && line.find(vneedle) == std::string::npos) continue;
                    if (!query.empty() && lower(line).find(query) == std::string::npos) continue;
                    matched++;
                    if (line.size() > 3000) continue;
                    hits.push_back(line);
                    if (hits.size() > lim) hits.erase(hits.begin());
                }
                gzclose(f);
            }
        }
        std::string out = "{\"folder\":" + q(dir) + ",\"files\":" + std::to_string(files) + ",\"matched\":" +
                          std::to_string(matched) + ",\"records\":[";
        for (size_t i = 0; i < hits.size(); i++) out += (i ? "," : "") + hits[i];
        out += "],\"note\":" + q(files ? "the newest matching records, oldest first (decoder data log: events, messages, "
                                         "positions, incidents, voice transcripts, raw frames)"
                                       : "no decoder log files - Decoder Logging (sidebar) is off or empty") + "}";
        return out;
    }, r);
}

void t_capture(const Json& a, Reply r) {
    double hz = num_arg(a, "freq_hz", 0), rate = num_arg(a, "rate_hz", 0);
    const double secs = std::clamp(num_arg(a, "seconds", 10), 1.0, 60.0);
    int tuner = 0;
    Vfo v;
    if (has(a, "vfo")) {
        if (!find_vfo(static_cast<int>(num_arg(a, "vfo", -1)), &v)) return r("No such VFO", true);
        if (hz <= 0) hz = v.hz;
        if (rate <= 0) rate = v.bw;
        tuner = v.tuner;
    }
    if (hz <= 0) return r("freq_hz or vfo needed", true);
    if (display_for(hz, &tuner) < 0) return r("The frequency is outside the band the receiver shows - tune there first", true);
    if (rate <= 0) rate = 48000;
    rate = std::clamp(rate, 12500.0, 600000.0);
    const bool all = bool_arg(a, "all_antennas", false);
    if (all && rate * secs * 8 * active_num_elements.load() > 400e6)
        return r("Every antenna at that rate and length is over 400 MB - a lower rate_hz or fewer seconds", true);
    std::string dir = AiManager::instance().conversation_dir();
    if (dir.empty()) {
        dir = "ai/captures";
        mkdir("ai", 0755);
        mkdir(dir.c_str(), 0755);
    }
    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%H%M%S", localtime(&t));
    aicap::Request req;
    req.rf_hz = hz;
    req.rate_hz = rate;
    req.seconds = secs;
    req.antenna = has(a, "antenna") ? static_cast<int>(num_arg(a, "antenna", -1)) : -1;
    req.all_antennas = all;
    req.path = dir + "/capture-" + stamp + "-" + num(hz / 1e3, 0) + "kHz.cf32";
    later([=](bool* err) {
        // in kraken_doa itself (ai_capture.hpp): no second 8091 stream, a
        // few % of a core; then sigtool analyses the file (Python)
        const aicap::Result c = aicap::run(req);
        if (!c.ok) {
            *err = true;
            return "Capture failed: " + c.error;
        }
        std::string rep;
        kproc::ExitStatus st;
        dig::run_and_capture(kproc::python_cmd({"ai/sigtool.py", "analyze", c.path}), 240000, &rep, &st, true);
        if (rep.size() > 9000) rep = rep.substr(0, 9000) + "\n... (cut)";
        char abs[4096];
        auto full = [&](const std::string& p) { return realpath(p.c_str(), abs) ? std::string(abs) : p; };
        std::string out = "Captured " + full(c.path) + " (complex float32, " + num(c.seconds, 2) + " s at " + num(c.rate_hz, 1) +
                          " Hz, passband " + num(c.bandwidth_hz, 0) + " Hz, around " + num(hz / 1e6, 6) + " MHz; " + c.source +
                          "; sidecar .json).";
        if (c.gaps) out += " " + std::to_string(c.gaps) + " calibration / retune gap(s) cut out.";
        if (c.dropped) out += " " + std::to_string(c.dropped) + " packets dropped (the Pi was busy) - gaps.";
        if (!c.stopped.empty()) out += " Ended early: " + c.stopped + ".";
        if (!c.array_path.empty()) {
            out += "\nEvery antenna (" + std::to_string(c.antennas) + ", sample-major, sidecar with the array geometry): " +
                   full(c.array_path) + " - e.g. a plugin offline with each talker's bearing: plugins/<id>/build/decoder --file " +
                   full(c.array_path);
            if (c.radius_m > 0) out += " --uca " + num(c.radius_m, 4);
            out += " (the antennas / rate / RF come from the sidecar).";
        }
        return out + "\nsigtool analyze (its PNG plots are next to the file - Read them):\n" + rep;
    }, r);
}

// --- 🗺 the map: the assistant's layers (every page) + the asking page's view ---
void t_map_plot(const Json& a, Reply r) {
    StationLocation s = station_info.resolve();
    const double st[2] = {s.lat, s.lon};
    const bool have_st = s.lat != 0 || s.lon != 0;
    std::string conv = AiManager::instance().conversation_dir(), sum;
    const std::string id = conv.empty() ? "" : conv.substr(conv.rfind('/') + 1);
    const std::string err = aimap::plot(a, conv, id, have_st ? st : nullptr, &sum);
    if (!err.empty()) return r(err, true);
    WebSocketServer::broadcast_json_message(aimap::message());
    if (bool_arg(a, "show", true)) {
        Json sj;
        geo::json_parse(sum, &sj);
        page_tell("map_show", "{\"layer\":" + q(sj.str("layer")) + "}");
    }
    r(sum, false);
}
void t_map_layers(const Json&, Reply r) { r(aimap::list_json(), false); }
void t_map_remove(const Json& a, Reply r) {
    const std::string which = str_arg(a, "layer");
    if (which.empty()) return r("layer: a layer's id or name, or \"all\"", true);
    std::string sum;
    const std::string err = aimap::remove(which, &sum);
    if (!err.empty()) return r(err, true);
    WebSocketServer::broadcast_json_message(aimap::message());
    r(sum, false);
}
void t_map_show(const Json& a, Reply r) {
    if (has(a, "layer") && aimap::find(str_arg(a, "layer")).empty()) return r("no such AI layer (map_layers lists them)", true);
    Json b = a;
    if (has(a, "layer")) {
        for (auto& [k, v] : b.o)
            if (k == "layer") { v.type = Json::STR; v.s = aimap::find(str_arg(a, "layer")); }
    }
    page_call("map_show", dump(b), 15000, r);
}
void t_vs_get(const Json& a, Reply r) { page_call("vs_state", dump(a), 15000, r); }
void t_vs_set(const Json& a, Reply r) { page_call("vs_set", dump(a), 300000, r); }
void t_vs_add(const Json& a, Reply r) {
    double la = num_arg(a, "lat", NAN), lo = num_arg(a, "lon", NAN);
    if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180)) return r("lat / lon (degrees) needed", true);
    page_call("vs_add_tx", dump(a), 300000, r);
}
void t_vs_remove(const Json& a, Reply r) {
    if (str_arg(a, "site").empty()) return r("site: a site's id or name, or \"all\" (every TX antenna)", true);
    page_call("vs_remove", dump(a), 15000, r);
}
void t_path_loss(const Json& a, Reply r) {
    double la = num_arg(a, "lat", NAN), lo = num_arg(a, "lon", NAN);
    if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180)) return r("lat / lon (degrees) of the far end needed", true);
    page_call("pl_link", dump(a), 180000, r);
}
void t_terrain_at(const Json& a, Reply r) {
    const Json* p = a.get("points");
    if (!p || p->type != Json::ARR || p->a.empty() || p->a.size() > 200) return r("points: [{lat, lon, name}], 1-200", true);
    page_call("vs_query", dump(a), 30000, r);
}

void t_wait_for(const Json& a, Reply r) {
    const double secs = std::clamp(num_arg(a, "seconds", 30), 1.0, 300.0);
    const int only = static_cast<int>(num_arg(a, "vfo", -1));
    const std::string ql = lower(str_arg(a, "contains"));
    // where each decoder's log stands now
    std::vector<std::pair<int, uint64_t>> start;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst || (only >= 0 && inst->id != only)) continue;
        if (auto dd = inst->getDigital()) start.push_back({inst->id, dd->last_event_seq()});
    }
    if (start.empty()) return r(only >= 0 ? "D" + std::to_string(only) + " has no decoder on" : "No VFO has a decoder on", true);
    later([=](bool*) {
        const int64_t t0 = now_ms(), until = t0 + static_cast<int64_t>(secs * 1000);
        int64_t first_hit = 0;
        std::string found;
        size_t n = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            found.clear();
            n = 0;
            for (const auto& [id, seq] : start) {
                auto inst = decimator_manager.getDecimator(id);
                auto dd = inst ? inst->getDigital() : nullptr;
                if (!dd || dd->last_event_seq() <= seq) continue;
                Json st;
                if (!geo::json_parse(dd->status_json(seq, 50), &st)) continue;
                const Json* ev = st.get("events");
                if (!ev || ev->type != Json::ARR) continue;
                for (const auto& e : ev->a) {
                    if (!ql.empty() && lower(e.str("m")).find(ql) == std::string::npos) continue;
                    found += (n ? "," : "") + std::string("{\"vfo\":") + std::to_string(id) + ",\"t\":" +
                             (e.get("t") ? dump(*e.get("t")) : "0") + ",\"src\":" + q(e.str("src")) + ",\"text\":" + q(e.str("m")) + "}";
                    n++;
                }
            }
            const int64_t now = now_ms();
            if (n && !first_hit) first_hit = now;
            // a moment more after the first event (they come in groups)
            if ((first_hit && now - first_hit >= 1500) || now >= until) break;
        }
        return "{\"waited_s\":" + num((now_ms() - t0) / 1000.0, 1) + ",\"events\":[" + found + "],\"note\":" +
               q(n ? "new decoder events (t = ms since epoch)" : "nothing new from the decoders in that time") + "}";
    }, r);
}

void t_remember(const Json& a, Reply r) {
    std::string id;
    const std::string err = AiManager::instance().remember(num_arg(a, "freq_hz", 0), num_arg(a, "bandwidth_hz", 0),
                                                           str_arg(a, "name"), str_arg(a, "summary"),
                                                           num_arg(a, "bearing_deg", -1), str_arg(a, "confidence"), &id);
    if (!err.empty()) return r(err, true);
    r("Remembered as " + id + " (shown as an AI badge on the spectrum)", false);
}

void t_find_ids(const Json& a, Reply r) {
    r("{\"identifications\":" +
          AiManager::instance().identifications_json(num_arg(a, "freq_hz", 0), num_arg(a, "within_hz", 25000),
                                                      str_arg(a, "query"),
                                                      static_cast<size_t>(std::clamp(num_arg(a, "limit", 30.0), 1.0, 300.0))) +
          ",\"note\":" + q("what the assistant found out earlier (remember_signal): t = ms since epoch, brg = compass "
                           "bearing, conv = the conversation") + "}",
      false);
}

void t_forget(const Json& a, Reply r) {
    const std::string err = AiManager::instance().forget(str_arg(a, "id"));
    if (!err.empty()) return r(err, true);
    r("Forgotten", false);
}

}  // namespace
}  // namespace ai_mcp

// ---------------------------------------------------------------------------
// Control tools: the commands a browser sends (ControlHandler::apply_command
// - persisted, echoed to every page), refused in Read only
// ---------------------------------------------------------------------------
namespace ai_mcp {
namespace {

std::string readonly_refusal() {
    return AiManager::instance().readonly()
               ? "Refused: the user switched the assistant to Read only (the toggle in the assistant panel) - it may "
                 "look at the receiver but not change it. Say what you would change and ask the user to allow control."
               : "";
}

std::string cmd(const std::string& c) {
    std::cout << "AI assistant: " << c.substr(0, 200) << std::endl;
    return ControlHandler::apply_command(c);
}

std::string mhz(double hz) { return num(hz / 1e6, 6); }

// the bandwidth option for hz (options run from 2.4 MHz down): at_least =
// the narrowest >= hz (a decoder's minimum), else the nearest (log scale)
int bw_index_for(double hz, bool at_least) {
    int best = 0;
    double bd = 1e9;
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        const double b = BANDWIDTH_OPTIONS[i].bandwidth_mhz * 1e6;
        if (at_least) {
            if (b >= hz * 0.999) best = i;
        } else if (const double dd = std::fabs(std::log(b / std::max(hz, 1.0))); dd < bd) {
            bd = dd;
            best = i;
        }
    }
    return best;
}

std::string set_bw(int id, double hz, double* applied = nullptr, bool at_least = true) {
    const int idx = bw_index_for(hz, at_least);
    if (applied) *applied = BANDWIDTH_OPTIONS[idx].bandwidth_mhz * 1e6;
    return cmd("SET_DECIMATOR_BW:" + std::to_string(id) + ":" + std::to_string(idx));
}

std::string move_vfo(int id, double hz) {
    Vfo v;
    if (!find_vfo(id, &v)) return "no VFO D" + std::to_string(id);
    if (wideband_mode_enabled.load())
        return "the receiver is in wideband scan mode - VFOs are placed by the scanner there (switch to coherent mode)";
    if (independent_mode_enabled.load()) {
        int tuner = -1;
        if (display_for(hz, &tuner) < 0)
            return "no tuner shows " + mhz(hz) + " MHz - tune a tuner there first (tune with tuner=N)";
        return cmd("SET_DECIMATOR_FREQ:" + std::to_string(id) + ":" + num((hz - tuner_center_hz(tuner)) / 1e3, 3) + ":" +
                   std::to_string(tuner));
    }
    const double off = hz - tuner_center_hz(0);
    if (std::fabs(off) + v.bw / 2 > SAMPLE_RATE / 2)
        return mhz(hz) + " MHz is outside the band the receiver shows (" + mhz(tuner_center_hz(0)) +
               " MHz +- 1.2 MHz) - tune the receiver first";
    return cmd("SET_DECIMATOR_FREQ:" + std::to_string(id) + ":" + num(off / 1e3, 3));
}

std::string set_decoder(int id, std::string dec, std::vector<std::string>& notes) {
    dec = lower(dec);
    if (dec == "off" || dec == "none" || dec.empty()) return cmd("DIGITAL_MODE:" + std::to_string(id) + ":OFF");
    if (dec == "auto") return cmd("DIGITAL_MODE:" + std::to_string(id) + ":AUTO");
    if (dec.rfind("plugin:", 0) == 0) dec = dec.substr(7);
    dig::PluginInfo pi;
    if (!dig::PluginRegistry::instance().get(dec, &pi)) return "no decoder plugin \"" + dec + "\" (list_decoders)";
    if (!pi.built) return "the decoder plugin \"" + dec + "\" isn't built";
    Vfo v;
    if (!find_vfo(id, &v)) return "no VFO D" + std::to_string(id);
    std::string e = cmd("DIGITAL_MODE:" + std::to_string(id) + ":PLUGIN:" + dec);
    if (!e.empty()) return e;
    const std::string tsuf = independent_mode_enabled.load() ? ":" + std::to_string(v.tuner) : "";
    if (pi.fixed_freq_hz > 0 || pi.lock_vfo) {
        // as the web UI does when one is picked: the full bandwidth it
        // needs, the VFO on its tuner's centre, and (fixed) the tuner there
        double bw = 0;
        if (v.bw < pi.min_vfo_rate - 1) set_bw(id, pi.min_vfo_rate, &bw);
        cmd("SET_DECIMATOR_FREQ:" + std::to_string(id) + ":0" + tsuf);
        if (pi.fixed_freq_hz > 0) {
            e = independent_mode_enabled.load() ? cmd("TUNER_FREQ:" + std::to_string(v.tuner) + ":" + mhz(pi.fixed_freq_hz))
                                                : cmd("FREQ:" + mhz(pi.fixed_freq_hz));
            if (!e.empty()) return "decoder set, but tuning to " + mhz(pi.fixed_freq_hz) + " MHz was refused: " + e;
            notes.push_back(pi.name + " works on a fixed frequency: the receiver was tuned to " + mhz(pi.fixed_freq_hz) +
                            " MHz and the VFO put on its centre");
        } else {
            notes.push_back(pi.name + " takes the tuner's whole band: the VFO was put on the tuner's centre (move the "
                                      "receiver, not the VFO)");
        }
    } else if (v.bw < pi.min_vfo_rate - 1) {
        double bw = 0;
        set_bw(id, pi.min_vfo_rate, &bw);
        notes.push_back("bandwidth widened to " + num(bw, 0) + " Hz (" + pi.name + " needs at least " +
                        num(pi.min_vfo_rate, 0) + " Hz)");
    }
    return "";
}

std::string vfo_state(int id) {
    Vfo v;
    if (!find_vfo(id, &v)) return "null";
    return "{\"vfo\":" + std::to_string(id) + ",\"freq_hz\":" + num(v.hz, 0) + ",\"bandwidth_hz\":" + num(v.bw, 0) +
           ",\"demod\":" + q(DecimatorManager::demodModeToString(v.d.demod_mode)) + ",\"decoder\":" +
           q(dig::mode_string(static_cast<dig::Mode>(v.d.digital_mode), v.d.digital_plugin)) + ",\"heard\":" +
           (v.d.audio_out ? "true" : "false") + ",\"squelch\":" + (v.d.squelch_enabled ? "true" : "false") + "}";
}

// set_vfo / add_vfo: everything but the frequency's own checks
std::string apply_vfo_args(int id, const Json& a, bool skip_freq, std::vector<std::string>& notes) {
    std::string e;
    bool locked = false;
    if (has(a, "decoder")) {
        e = set_decoder(id, str_arg(a, "decoder"), notes);
        if (!e.empty()) return e;
        dig::PluginInfo pi;
        const std::string d = lower(str_arg(a, "decoder"));
        locked = dig::PluginRegistry::instance().get(d.rfind("plugin:", 0) == 0 ? d.substr(7) : d, &pi) &&
                 (pi.fixed_freq_hz > 0 || pi.lock_vfo);
    }
    if (has(a, "bandwidth_hz") && !locked) {
        double bw = 0;
        e = set_bw(id, num_arg(a, "bandwidth_hz", 12500), &bw, false);
        if (!e.empty()) return e;
        if (std::fabs(bw - num_arg(a, "bandwidth_hz", 0)) > 1) notes.push_back("bandwidth set to the nearest option, " + num(bw, 0) + " Hz");
    }
    if (has(a, "freq_hz") && !skip_freq) {
        if (locked) notes.push_back("freq_hz ignored: the decoder holds the VFO on the tuner's centre");
        else if (!(e = move_vfo(id, num_arg(a, "freq_hz", 0))).empty()) return e;
    }
    if (has(a, "demod")) {
        std::string d = str_arg(a, "demod");
        for (auto& c : d) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (d != "NBFM" && d != "WBFM" && d != "AM" && d != "DIGITAL") return "demod must be NBFM, WBFM, AM or DIGITAL";
        if (!(e = cmd("SET_DECIMATOR_DEMOD:" + std::to_string(id) + ":" + d)).empty()) return e;
    }
    if (has(a, "squelch_method")) {
        std::string m = str_arg(a, "squelch_method");
        for (auto& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (m != "FFT" && m != "DIGITAL") return "squelch_method must be FFT or DIGITAL";
        if (!(e = cmd("DEC_SQUELCH_METHOD:" + std::to_string(id) + ":" + m)).empty()) return e;
    }
    if (has(a, "squelch_db")) {
        const double db = std::clamp(num_arg(a, "squelch_db", 10), 0.0, 80.0);
        if (!(e = cmd("DEC_SQUELCH_LEVEL:" + std::to_string(id) + ":" + num(db, 1))).empty()) return e;
        if (!has(a, "squelch")) cmd("DEC_SQUELCH_ENABLE:" + std::to_string(id) + ":1");
    }
    if (has(a, "squelch") && !(e = cmd("DEC_SQUELCH_ENABLE:" + std::to_string(id) + ":" + (bool_arg(a, "squelch", false) ? "1" : "0"))).empty())
        return e;
    if (has(a, "listen")) {
        if (!(e = cmd("AUDIO_VFO:" + std::to_string(id) + ":" + (bool_arg(a, "listen", false) ? "1" : "0"))).empty()) return e;
        if (bool_arg(a, "listen", false)) notes.push_back("the user hears it only with the page's audio switched on");
    }
    if (has(a, "transcribe") &&
        !(e = cmd("VOICE2TEXT_VFO:" + std::to_string(id) + ":" + (bool_arg(a, "transcribe", false) ? "1" : "0"))).empty())
        return e;
    return "";
}

std::string notes_text(const std::vector<std::string>& notes) {
    std::string s;
    for (const auto& n : notes) s += (s.empty() ? " Notes: " : "; ") + n;
    return s;
}

void t_tune(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const double hz = num_arg(a, "freq_hz", 0);
    if (!(hz > 0)) return r("freq_hz needed", true);
    if (wideband_mode_enabled.load()) return r("The receiver is in wideband scan mode (tuners spread by the scan) - not tuned", true);
    std::string e;
    const bool indep = independent_mode_enabled.load();
    int tuner = 0;
    if (indep) {
        tuner = static_cast<int>(num_arg(a, "tuner", -1));
        if (tuner < 0 || tuner >= active_num_elements.load())
            return r("Independent mode: say which tuner to tune (tuner = 0.." + std::to_string(active_num_elements.load() - 1) + ")", true);
        e = cmd("TUNER_FREQ:" + std::to_string(tuner) + ":" + mhz(hz));
    } else {
        e = cmd("FREQ:" + mhz(hz));
    }
    if (!e.empty()) return r("Refused: " + e, true);
    // wait until the spectrum shows the new band (and, coherent, the
    // calibration after a retune is over) - the agent looks next
    later([=](bool*) {
        const int64_t t0 = now_ms();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const double waited = (now_ms() - t0) / 1000.0;
            if (waited > 12) break;
            if (indep) {
                if (waited > 2.5) break;
                continue;
            }
            if (std::fabs(MessageBuilders::spectrum_center_hz() - hz) < 2000 && !doa_is_calibrating() && waited > 1.0) break;
        }
        return "Tuned " + std::string(indep ? "tuner " + std::to_string(tuner) : "the receiver") + " to " + mhz(hz) +
               " MHz (it shows " + mhz(hz - 1.2e6) + " .. " + mhz(hz + 1.2e6) + " MHz; settled after " +
               num((now_ms() - t0) / 1000.0, 1) + " s). VFOs keep their offsets from the centre, so they moved with it.";
    }, r);
}

void t_set_gain(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    if (!has(a, "gain_db")) return r("gain_db needed (dB 0-49.6, or -1 for the tuner's AGC)", true);
    const double g = num_arg(a, "gain_db", 0);
    std::string e;
    if (independent_mode_enabled.load()) {
        const int tuner = static_cast<int>(num_arg(a, "tuner", -1));
        if (tuner < 0) return r("Independent mode: say which tuner (tuner = N)", true);
        e = cmd("TUNER_GAIN:" + std::to_string(tuner) + ":" + num(g, 1));
    } else {
        e = cmd("GAIN:" + num(g, 1));
    }
    if (!e.empty()) return r("Refused: " + e, true);
    r("Gain set to " + (g < 0 ? std::string("auto (AGC)") : num(g, 1) + " dB") +
      (independent_mode_enabled.load() ? "" : " - the array recalibrates after a gain change (a few seconds)"), false);
}

void t_add_vfo(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const double hz = num_arg(a, "freq_hz", 0);
    if (!(hz > 0)) return r("freq_hz needed", true);
    if (wideband_mode_enabled.load()) return r("The receiver is in wideband scan mode - switch to coherent first", true);
    if (display_for(hz) < 0) return r(mhz(hz) + " MHz is outside the band the receiver shows - tune there first", true);
    int before = -1;
    for (const auto& v : vfos()) before = std::max(before, v.d.id);
    if (auto e = cmd("ADD_DECIMATOR"); !e.empty()) return r("Refused: " + e, true);
    int id = -1;
    for (const auto& v : vfos()) id = std::max(id, v.d.id);
    if (id <= before) return r("No VFO was added (limit reached?)", true);
    std::vector<std::string> notes;
    Json args = a;
    if (!has(a, "bandwidth_hz")) {
        const std::string d = lower(str_arg(a, "demod", "NBFM"));
        Json bw;
        bw.type = Json::NUM;
        bw.n = d == "wbfm" ? 200000 : d == "am" ? 10000 : 12500;
        args.o.push_back({"bandwidth_hz", bw});
    }
    std::string e = move_vfo(id, hz);
    if (e.empty()) e = apply_vfo_args(id, args, true, notes);
    if (!e.empty()) {
        cmd("REMOVE_DECIMATOR:" + std::to_string(id));
        return r("Refused: " + e + " (the new VFO was removed again)", true);
    }
    r("Added D" + std::to_string(id) + ": " + vfo_state(id) + notes_text(notes), false);
}

void t_set_vfo(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const int id = static_cast<int>(num_arg(a, "vfo", -1));
    Vfo v;
    if (!find_vfo(id, &v)) return r("No VFO D" + std::to_string(id), true);
    std::vector<std::string> notes;
    if (auto e = apply_vfo_args(id, a, false, notes); !e.empty()) return r("Refused: " + e, true);
    r("D" + std::to_string(id) + " now: " + vfo_state(id) + notes_text(notes), false);
}

void t_set_option(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const int id = static_cast<int>(num_arg(a, "vfo", -1));
    Vfo v;
    if (!find_vfo(id, &v)) return r("No VFO D" + std::to_string(id), true);
    std::string key = str_arg(a, "key");
    if (key.empty() || key.find(':') != std::string::npos) return r("key needed (an option of the VFO's decoder)", true);
    if (key.find('.') == std::string::npos && key != "verbose" && key != "invert" && key != "map") {
        std::string p = v.d.digital_plugin;
        if (p.empty()) {
            Json st;
            if (decoder_status(id, 0, false, &st)) p = st.str("detected");
        }
        if (p.empty()) return r("The VFO's decoder isn't known yet - give the key as <plugin>.<key>", true);
        key = p + "." + key;
    }
    const std::string val = str_arg(a, "value");
    if (val.find('\n') != std::string::npos) return r("bad value", true);
    if (auto e = cmd("DIGITAL_OPT:" + std::to_string(id) + ":" + key + ":" + val); !e.empty()) return r("Refused: " + e, true);
    r("Set " + key + " = " + val + " on D" + std::to_string(id), false);
}

void t_remove_vfo(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const int id = static_cast<int>(num_arg(a, "vfo", -1));
    Vfo v;
    if (!find_vfo(id, &v)) return r("No VFO D" + std::to_string(id), true);
    if (vfos().size() <= 1) return r("That is the only VFO - not removed", true);
    if (auto e = cmd("REMOVE_DECIMATOR:" + std::to_string(id)); !e.empty()) return r("Refused: " + e, true);
    r("Removed D" + std::to_string(id), false);
}

void t_set_doa(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const bool on = bool_arg(a, "on", true);
    if (auto e = cmd(std::string("DOA:") + (on ? "1" : "0")); !e.empty()) return r("Refused: " + e, true);
    r(std::string("DoA ") + (on ? "on" : "off") + (multi_tuner_mode() ? " (applies when the receiver is back in coherent mode)" : ""), false);
}

void t_save_freq(const Json& a, Reply r) {
    if (auto e = readonly_refusal(); !e.empty()) return r(e, true);
    const double hz = num_arg(a, "freq_hz", 0);
    const std::string name = str_arg(a, "name");
    if (!(hz > 0) || name.empty()) return r("name and freq_hz needed", true);
    // a category by its name (none = uncategorised)
    std::string cat;
    const std::string want = lower(str_arg(a, "category"));
    Json j;
    if (!want.empty() && geo::json_parse(savedfreq::message_json(), &j))
        if (const Json* sf = j.get("savedfreq"))
            if (const Json* cs = sf->get("cats"); cs && cs->type == Json::ARR)
                for (const auto& c : cs->a)
                    if (lower(c.str("name")) == want) cat = c.str("id");
    std::string dec = lower(str_arg(a, "decoder"));
    if (dec == "off") dec = "OFF";
    else if (dec == "auto") dec = "AUTO";
    else if (!dec.empty()) dec = "PLUGIN:" + (dec.rfind("plugin:", 0) == 0 ? dec.substr(7) : dec);
    std::string demod = str_arg(a, "demod");
    for (auto& c : demod) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    char idb[24];
    snprintf(idb, sizeof idb, "ai%lld", static_cast<long long>(now_ms() % 100000000000LL));
    std::ostringstream o;
    o << "{\"id\":" << q(idb) << ",\"name\":" << q(name) << ",\"hz\":" << num(std::round(hz), 0) << ",\"cat\":" << q(cat)
      << ",\"loc\":\"\",\"mode\":" << q(demod) << ",\"bw\":" << num(std::max(0.0, num_arg(a, "bandwidth_hz", 0)), 0)
      << ",\"gain\":null,\"dec\":" << q(dec) << ",\"tone\":" << q(str_arg(a, "tone")) << ",\"notes\":" << q(str_arg(a, "notes")) << "}";
    if (auto e = cmd("SF_SET:" + o.str()); !e.empty()) return r("Refused: " + e, true);
    r("Saved \"" + name + "\" " + mhz(hz) + " MHz in the sidebar's Saved Frequencies" +
      (!want.empty() && cat.empty() ? " (no category named \"" + str_arg(a, "category") + "\" - uncategorised)" : ""), false);
}

}  // namespace
}  // namespace ai_mcp

// ---------------------------------------------------------------------------
// The tool table + the MCP endpoint
// ---------------------------------------------------------------------------
namespace ai_mcp {
namespace {

#define OBJ(props, req) "{\"type\":\"object\",\"properties\":{" props "}" req ",\"additionalProperties\":false}"
#define NUMP(n, d) "\"" n "\":{\"type\":\"number\",\"description\":\"" d "\"}"
#define INTP(n, d) "\"" n "\":{\"type\":\"integer\",\"description\":\"" d "\"}"
#define STRP(n, d) "\"" n "\":{\"type\":\"string\",\"description\":\"" d "\"}"
#define BOOLP(n, d) "\"" n "\":{\"type\":\"boolean\",\"description\":\"" d "\"}"
#define ARRP(n, d) "\"" n "\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"" d "\"}"
#define OBJP(n, d) "\"" n "\":{\"type\":\"object\",\"description\":\"" d "\"}"
#define REQ(...) ",\"required\":[" __VA_ARGS__ "]"

std::vector<Tool>& tools() {
    static std::vector<Tool> t = {
        {"get_screen", "What is on screen",
         "The receiver's state and what the user's page shows: mode, centre frequency, gain, the station, every VFO "
         "(D0, D1 ...: frequency, bandwidth, demod, level, squelch, its decoder and the decoder's facts, bearing) and "
         "the page's view (zoom window, selected VFO, open tab, map). Start here for questions about the screen.",
         OBJ("", ""), false, true, t_get_screen},
        {"get_spectrum", "Spectrum",
         "The live averaged spectrum (dB above the noise floor) between start_hz and stop_hz (default: the whole band "
         "shown), at most `points` values, plus the signals found in it (peaks over min_db with their -6 dB width). "
         "Zoom in (a narrow window) for detail down to the FFT bin. hold_s > 0 = a peak hold over that time "
         "(bursty signals - sondes, pagers, data bursts - hardly show in the average).",
         OBJ(NUMP("start_hz", "window start, Hz") "," NUMP("stop_hz", "window end, Hz") "," INTP("points", "values to return, 8-1024 (256)")
                 "," NUMP("min_db", "signal threshold, dB above the floor (6)") "," INTP("tuner", "independent mode: the tuner")
                 "," NUMP("hold_s", "peak hold over this many seconds (0-30) instead of the average: catches bursty signals"),
             ""),
         false, true, t_get_spectrum},
        {"get_decoder", "A VFO's decoder",
         "Everything a VFO's digital decoder reports: mode, the plugin found by Auto detect, facts, its event log "
         "(newest `events`), tables (aircraft, ships, devices, cells ... first `rows` rows), plugin states.",
         OBJ(INTP("vfo", "VFO number (D0 = 0)") "," INTP("events", "newest events (30)") "," INTP("rows", "table rows (40)"),
             REQ("\"vfo\"")),
         false, true, t_get_decoder},
        {"get_bearings", "Direction finding",
         "Bearings (DoA) of the VFOs and of every transmitter the decoders named (radios by unit ID, aircraft, ships, "
         "devices, beacons ...): compass bearing, confidence, last heard, earlier transmissions. Coherent mode with "
         "DoA on only.",
         OBJ(INTP("vfo", "only this VFO") "," INTP("limit", "talkers (60)"), ""), false, true, t_get_bearings},
        {"get_map_points", "Decoded positions",
         "Positions decoders reported (aircraft, ships, APRS stations, balloons, beacons, LoRa nodes ...) with their "
         "details, distance and bearing from the station - nearest first.",
         OBJ(INTP("vfo", "only this VFO") "," STRP("query", "text the point must contain") "," INTP("limit", "points (60)"), ""),
         false, true, t_get_map_points},
        {"get_transcripts", "Voice to Text",
         "Recent speech transcripts (Voice to Text) with VFO, frequency, talker and bearing - newest first.",
         OBJ(STRP("query", "text to look for") "," INTP("vfo", "only this VFO") "," INTP("limit", "transcripts (20)"), ""),
         false, true, t_get_transcripts},
        {"search_decoder_log", "Decoder data log",
         "Searches the decoder data log of the last `days` days (every decoded event, message, position, incident, "
         "transcript - when the user turned Decoder Logging on): the newest matching records.",
         OBJ(STRP("query", "text the record must contain (case-insensitive)") "," NUMP("days", "1-31 (1 = today)")
                 "," STRP("type", "event | message | position | raw | incident | voice") "," INTP("vfo", "only this VFO")
                 "," INTP("limit", "records (100)"),
             ""),
         false, true, t_search_log},
        {"get_saved_frequencies", "Saved frequencies",
         "The user's saved frequencies (name, category, location / map marker, demod, decoder, notes) and map markers. "
         "A local list of known transmitters.",
         OBJ(STRP("query", "text to look for (name, MHz, notes, category, place)"), ""), false, true, t_get_saved},
        {"get_incidents", "Incidents",
         "Incidents found in decoded pager messages (addresses looked up on the map).", OBJ("", ""), false, true, t_get_incidents},
        {"list_decoders", "Decoder plugins",
         "The decoder plugins installed here (id, name, what they decode, bandwidth they need, fixed frequency).",
         OBJ("", ""), false, true, t_list_decoders},
        {"capture_iq", "Capture + analyse",
         "Records the IQ of a signal (a VFO's, or freq_hz inside the band the receiver shows) for `seconds` into the "
         "conversation's folder and analyses it (sigtool: bursts, spectrum, modulation, symbol rate ...). Returns the "
         "file and the report; the PNG plots are next to it. It records what the receiver shows - during a playback "
         "the playback. Coherent mode: one antenna (the CH selector's, or `antenna`); all_antennas also writes every "
         "antenna to <file>_array.cf32 (sample-major, with the array geometry in its sidecar) for a plugin's offline "
         "per-talker bearings (`decoder --file <it> --uca R`).",
         OBJ(INTP("vfo", "capture this VFO's signal") "," NUMP("freq_hz", "or this frequency") "," NUMP("seconds", "1-60 (10)")
                 "," NUMP("rate_hz", "sample rate 12500-600000 (default: the VFO's bandwidth / 48000)")
                 "," INTP("antenna", "coherent mode: the antenna to record (default: the CH selector's)")
                 "," BOOLP("all_antennas", "coherent mode: also every antenna (<= 400 MB)"),
             ""),
         false, false, t_capture},
        {"map_plot", "Plot on the map",
         "Draws data on the map as a named layer of the user's 🗺 map (the '🤖 AI layers' panel; every page sees it, it "
         "stays until removed): transmitter sites from a licence register or your knowledge, bearing lines, areas, "
         "points of interest, measurements - anything with a position. The same layer name replaces that layer "
         "(replace:false adds to it). features: [{type: point | line | polygon | circle | bearing, lat, lon (degrees), "
         "points: [[lat, lon], ...] (line / polygon), radius_m (circle), bearing_deg + length_km (bearing: a great-circle "
         "line from lat / lon - from the station when they are left out), label, color (#rrggbb or a name), icon (one "
         "emoji), size, width, dashed, fill_opacity, arrow, info: {key: value} (what the user sees on a click: frequency, "
         "licensee, source ...)}], or geojson (a FeatureCollection - [lon, lat] order as the standard says), or file (a "
         ".geojson or .csv - lat, lon, name columns, the others become info - in your folder or the knowledge folder). "
         "The asking page's map opens on it (show:false = not). <= 5000 features a layer, 30 layers, 1 MB in all.",
         OBJ(STRP("layer", "the layer's name (also its id)") "," STRP("description", "what it shows") ","
                 STRP("source", "where the data came from (URL, register, file)") "," STRP("color", "the layer's colour")
                 "," BOOLP("replace", "true (default): replace a layer of that name; false: add to it")
                 "," ARRP("features", "the features (lat / lon order)") "," OBJP("geojson", "a GeoJSON object instead")
                 "," STRP("file", "a .geojson / .json / .csv file instead") "," BOOLP("show", "open the map on it (true)"),
             REQ("\"layer\"")),
         false, false, t_map_plot},
        {"map_layers", "Map layers", "The assistant's layers on the map (id, name, description, source, features, extent).",
         OBJ("", ""), false, true, t_map_layers},
        {"map_remove", "Remove a map layer", "Removes one of your map layers (id or name) - or \"all\".",
         OBJ(STRP("layer", "id / name / all"), REQ("\"layer\"")), false, false, t_map_remove},
        {"map_show", "Show on the map",
         "Opens the map on the user's page and shows a place (lat, lon, zoom 2-18), a box (south, west, north, east), "
         "one of your layers (layer) or the station (station: true).",
         OBJ(NUMP("lat", "deg") "," NUMP("lon", "deg") "," NUMP("zoom", "2-18") "," STRP("layer", "a layer to fit")
                 "," NUMP("south", "deg") "," NUMP("west", "deg") "," NUMP("north", "deg") "," NUMP("east", "deg")
                 "," BOOLP("station", "centre on the station"),
             ""),
         false, true, t_map_show},
        {"viewshed_get", "Viewshed / path loss state",
         "The user's Viewshed panel (it runs in their browser, on terrain data): the mode (line_of_sight: the station's "
         "line of sight only; signal: coverage of TX antennas - path loss over the terrain, link budget, reliability), "
         "the target (the other antenna's) height, the sites with their results (% in sight / % covered), the radio "
         "settings and the path loss link with its budget.",
         OBJ("", ""), false, true, t_vs_get},
        {"viewshed_set", "Viewshed settings",
         "Changes the Viewshed panel and waits for the results: enabled, mode (line_of_sight | signal), "
         "target_height_m + target_reference (ground | sea - aircraft / balloons: sea), refraction (radio | light | none, "
         "line of sight only), station_height_m / station_radius_km (the station's line of sight), and the signal "
         "mode's radio: freq_mhz, tx_power_w, tx_gain_dbi, rx_gain_dbi, tx_cable_db, rx_cable_db, rx_bandwidth_khz, "
         "noise_figure_db, required_snr_db, model (p1812 | itm | free_space - itm is much slower), tx_polarization / "
         "rx_polarization (vertical | horizontal | circular), tx_clutter / rx_clutter (none | water | open | suburban "
         "| urban | dense_urban), fading (auto | rician | rayleigh | none), reliability_pct, los_edge. Returns the "
         "state as viewshed_get.",
         OBJ(BOOLP("enabled", "the viewshed on / off") "," STRP("mode", "line_of_sight | signal") ","
                 NUMP("target_height_m", "the other antenna's height") "," STRP("target_reference", "ground | sea") ","
                 STRP("refraction", "radio | light | none") "," NUMP("station_height_m", "the station antenna's height above ground")
                 "," NUMP("station_radius_km", "1-1000") "," NUMP("freq_mhz", "20-20000") "," NUMP("tx_power_w", "W") ","
                 NUMP("tx_gain_dbi", "dBi") "," NUMP("rx_gain_dbi", "dBi") "," NUMP("tx_cable_db", "dB") ","
                 NUMP("rx_cable_db", "dB") "," NUMP("rx_bandwidth_khz", "kHz") "," NUMP("noise_figure_db", "dB") ","
                 NUMP("required_snr_db", "dB") "," STRP("model", "p1812 | itm | free_space") ","
                 STRP("tx_polarization", "vertical | horizontal | circular") "," STRP("rx_polarization", "vertical | horizontal | circular")
                 "," STRP("tx_clutter", "none | water | open | suburban | urban | dense_urban") ","
                 STRP("rx_clutter", "none | water | open | suburban | urban | dense_urban") ","
                 STRP("fading", "auto | rician | rayleigh | none") "," NUMP("reliability_pct", "1-99.99") ","
                 BOOLP("los_edge", "draw the line of sight edge"),
             ""),
         false, false, t_vs_set},
        {"viewshed_add_tx", "Add a TX antenna",
         "Adds a transmitter site to the Viewshed panel (signal mode: its coverage is coloured - how reliably receivers "
         "at the target height around it get its signal, also beyond the line of sight) and waits for the result. "
         "At most 6 sites. E.g. a candidate transmitter site, then terrain_at / path_loss towards the station.",
         OBJ(NUMP("lat", "deg") "," NUMP("lon", "deg") "," STRP("name", "a short name") ","
                 NUMP("height_m", "antenna height above the ground (default the panel's)") "," NUMP("radius_km", "1-1000"),
             REQ("\"lat\",\"lon\"")),
         false, false, t_vs_add},
        {"viewshed_remove", "Remove a TX antenna", "Removes a Viewshed site (id or name; \"all\" = every TX antenna).",
         OBJ(STRP("site", "id / name / all"), REQ("\"site\"")), false, false, t_vs_remove},
        {"path_loss", "Path loss link",
         "The terrain profile and link budget from a TX antenna of the Viewshed panel (site: its id or name; default the "
         "first) to a place (lat, lon; the receiver there at the target height): distance, bearing, line of sight / "
         "Fresnel clearance / the obstruction, the loss by ITU-R P.1812, Longley-Rice and free space, received power, "
         "SNR, margin and reliability. Shown in the strip under the user's map.",
         OBJ(NUMP("lat", "deg") "," NUMP("lon", "deg") "," STRP("name", "the far end's name") "," STRP("site", "TX site id / name"),
             REQ("\"lat\",\"lon\"")),
         false, false, t_path_loss},
        {"terrain_at", "Line of sight / signal at places",
         "For places (points: [{lat, lon, name}], up to 200): the ground height and, for every site the Viewshed panel "
         "has worked out, whether a target at the target height there is in its line of sight (or how high it would "
         "need to be) - in signal mode also the reliability and received power. Only within the sites' radius.",
         OBJ(ARRP("points", "[{lat, lon, name}]"), REQ("\"points\"")), false, true, t_terrain_at},
        {"wait_for", "Wait for decoded data",
         "Waits up to `seconds` for new decoder events (on one VFO or all) - optionally only events containing a text "
         "- and returns them. Use it while watching for something (a balloon, an aircraft, a call).",
         OBJ(NUMP("seconds", "1-300 (30)") "," INTP("vfo", "only this VFO") "," STRP("contains", "only events with this text"), ""),
         false, true, t_wait_for},
        {"remember_signal", "Remember an identification",
         "Records what a signal is (frequency, name, short summary of the evidence, bearing) in the receiver's local "
         "list: it shows as an AI badge on the spectrum and find_identifications finds it next time. The same name "
         "near the same frequency updates the entry.",
         OBJ(NUMP("freq_hz", "centre frequency") "," STRP("name", "short name, e.g. 'Auckland ATIS'") "," STRP("summary", "what it is and why")
                 "," NUMP("bandwidth_hz", "occupied bandwidth") "," NUMP("bearing_deg", "compass bearing to it")
                 "," STRP("confidence", "low | medium | high"),
             REQ("\"freq_hz\",\"name\"")),
         false, false, t_remember},
        {"find_identifications", "Earlier identifications",
         "Signals identified before on this receiver (remember_signal) near freq_hz and/or matching a text.",
         OBJ(NUMP("freq_hz", "near this frequency") "," NUMP("within_hz", "how near (25000)") "," STRP("query", "text") "," INTP("limit", "(30)"), ""),
         false, true, t_find_ids},
        {"forget_signal", "Forget an identification", "Removes an identification (its id from find_identifications).",
         OBJ(STRP("id", "the identification's id"), REQ("\"id\"")), false, false, t_forget},
        {"tune", "Tune",
         "Tunes the receiver (independent mode: one tuner) to freq_hz as the centre of its 2.4 MHz band and waits "
         "until the spectrum shows it. VFOs keep their offsets. Changes what the user sees.",
         OBJ(NUMP("freq_hz", "new centre, Hz (24 MHz - 1.766 GHz)") "," INTP("tuner", "independent mode: which tuner"), REQ("\"freq_hz\"")),
         true, false, t_tune},
        {"set_gain", "Gain",
         "Sets the RF gain in dB (0-49.6, -1 = AGC). In coherent mode the array recalibrates afterwards.",
         OBJ(NUMP("gain_db", "dB, -1 = auto") "," INTP("tuner", "independent mode: which tuner"), REQ("\"gain_db\"")), true, false, t_set_gain},
        {"add_vfo", "Add a VFO",
         "Adds a VFO at freq_hz (inside the band shown) with an optional bandwidth, demod (NBFM | WBFM | AM | DIGITAL) "
         "and decoder (a plugin id from list_decoders, 'auto' or 'off'); listen = play its audio. Returns its number.",
         OBJ(NUMP("freq_hz", "Hz") "," NUMP("bandwidth_hz", "Hz (nearest option)") "," STRP("demod", "NBFM | WBFM | AM | DIGITAL")
                 "," STRP("decoder", "plugin id | auto | off") "," BOOLP("listen", "hear it") "," BOOLP("transcribe", "Voice to Text"),
             REQ("\"freq_hz\"")),
         true, false, t_add_vfo},
        {"set_vfo", "Change a VFO",
         "Changes a VFO: frequency, bandwidth, demod, decoder (plugin id | auto | off), squelch (on/off, squelch_db "
         "above the floor, squelch_method FFT | DIGITAL - Digital opens on decoded frames), listen, transcribe.",
         OBJ(INTP("vfo", "VFO number") "," NUMP("freq_hz", "Hz") "," NUMP("bandwidth_hz", "Hz") "," STRP("demod", "NBFM | WBFM | AM | DIGITAL")
                 "," STRP("decoder", "plugin id | auto | off") "," BOOLP("squelch", "on / off") "," NUMP("squelch_db", "dB 0-80")
                 "," STRP("squelch_method", "FFT | DIGITAL") "," BOOLP("listen", "hear it") "," BOOLP("transcribe", "Voice to Text"),
             REQ("\"vfo\"")),
         true, false, t_set_vfo},
        {"set_decoder_option", "Decoder option",
         "Sets an option of a VFO's decoder (the plugin's options, see get_decoder / list_decoders): key = the "
         "option's key (or <plugin>.<key>), value as text ('' = its default).",
         OBJ(INTP("vfo", "VFO number") "," STRP("key", "option key") "," STRP("value", "value"), REQ("\"vfo\",\"key\",\"value\"")),
         true, false, t_set_option},
        {"remove_vfo", "Remove a VFO", "Removes a VFO (not the last one).", OBJ(INTP("vfo", "VFO number"), REQ("\"vfo\"")),
         true, false, t_remove_vfo},
        {"set_doa", "DoA on / off", "Switches direction finding (MUSIC DoA) on or off (coherent mode).",
         OBJ(BOOLP("on", "true = on"), REQ("\"on\"")), true, false, t_set_doa},
        {"save_frequency", "Save a frequency",
         "Adds an entry to the user's Saved Frequencies (sidebar): name, frequency, optional demod, bandwidth, decoder, "
         "tone, notes, category (an existing category's name).",
         OBJ(STRP("name", "name") "," NUMP("freq_hz", "Hz") "," STRP("demod", "NBFM | WBFM | AM | DIGITAL") "," NUMP("bandwidth_hz", "Hz")
                 "," STRP("decoder", "plugin id | auto") "," STRP("tone", "CTCSS / DCS / NAC") "," STRP("notes", "notes")
                 "," STRP("category", "category name"),
             REQ("\"name\",\"freq_hz\"")),
         true, false, t_save_freq},
    };
    return t;
}

std::string tools_list_json() {
    std::string o = "{\"tools\":[";
    bool first = true;
    for (const auto& t : tools()) {
        o += (first ? "" : ",") + std::string("{\"name\":") + q(t.name) + ",\"title\":" + q(t.title) + ",\"description\":" +
             q(std::string(t.desc) + (t.control ? " [changes the receiver - refused while the user has Read only on]" : "")) +
             ",\"inputSchema\":" + t.schema + ",\"annotations\":{\"readOnlyHint\":" + (t.read_only_hint ? "true" : "false") +
             ",\"destructiveHint\":false,\"openWorldHint\":false}}";
        first = false;
    }
    return o + "]}";
}

struct Pending {
    Res* res = nullptr;
    bool aborted = false, done = false;
    std::string body;
};

void respond(const std::shared_ptr<Pending>& p, const char* status, const std::string& body) {
    if (p->aborted || p->done) return;
    p->done = true;
    p->res->cork([&] {
        p->res->writeStatus(status);
        if (!body.empty()) p->res->writeHeader("Content-Type", "application/json");
        p->res->end(body);
    });
}

std::string rpc_id(const Json* id) {
    if (!id) return "null";
    if (id->type == Json::STR) return q(id->s);
    if (id->type == Json::NUM) return num(id->n);
    return "null";
}

void rpc_error(const std::shared_ptr<Pending>& p, const std::string& id, int code, const std::string& msg) {
    respond(p, "200 OK", "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
                             ",\"message\":" + q(msg) + "}}");
}

void handle_rpc(const std::shared_ptr<Pending>& p) {
    Json j;
    if (!geo::json_parse(p->body, &j) || j.type != Json::OBJ) return rpc_error(p, "null", -32700, "parse error");
    const std::string method = j.str("method");
    const Json* idj = j.get("id");
    if (!idj) return respond(p, "202 Accepted", "");   // a notification
    const std::string id = rpc_id(idj);
    static const Json empty_obj = [] { Json e; e.type = Json::OBJ; return e; }();
    const Json* params = j.get("params");
    if (!params || params->type != Json::OBJ) params = &empty_obj;
    if (method == "initialize") {
        std::string ver = params->str("protocolVersion");
        if (ver != "2025-11-25" && ver != "2025-06-18" && ver != "2025-03-26" && ver != "2024-11-05") ver = "2025-06-18";
        return respond(p, "200 OK",
                       "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"protocolVersion\":" + q(ver) +
                           ",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"kraken\",\"title\":"
                           "\"KrakenSDR receiver\",\"version\":\"1\"},\"instructions\":" +
                           q("The KrakenSDR receiver this assistant runs on: read its screen, spectrum, decoders and "
                             "bearings, and control it (tune, VFOs, decoders) unless the user set Read only.") + "}}");
    }
    if (method == "ping") return respond(p, "200 OK", "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}");
    if (method == "tools/list") return respond(p, "200 OK", "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + tools_list_json() + "}");
    if (method != "tools/call") return rpc_error(p, id, -32601, "method not found: " + method);
    const std::string name = params->str("name");
    const Json* args = params->get("arguments");
    if (!args || args->type != Json::OBJ) args = &empty_obj;
    for (const auto& t : tools()) {
        if (name != t.name) continue;
        std::cout << "AI assistant tool: " << name << " " << dump(*args).substr(0, 200) << std::endl;
        Reply reply = [p, id](std::string text, bool error) {
            respond(p, "200 OK", "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":" +
                                     q(text) + "}],\"isError\":" + (error ? "true" : "false") + "}}");
        };
        try {
            t.fn(*args, reply);
        } catch (const std::exception& e) {
            reply(std::string("failed: ") + e.what(), true);
        }
        return;
    }
    rpc_error(p, id, -32602, "unknown tool: " + name);
}

bool host_ok(std::string_view host) {
    const std::string port = ":" + std::to_string(g_port);
    return host == "127.0.0.1" + port || host == "localhost" + port || host == "[::1]" + port;
}

bool token_ok(std::string_view auth) {
    const std::string want = "Bearer " + g_token;
    if (auth.size() != want.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < want.size(); i++) diff |= static_cast<unsigned char>(auth[i] ^ want[i]);
    return diff == 0;
}

std::string random_token() {
    std::ifstream f("/dev/urandom", std::ios::binary);
    std::string out;
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 24; i++) {
        unsigned char c = static_cast<unsigned char>(f.get());
        out += hx[c >> 4];
        out += hx[c & 15];
    }
    return out;
}

}  // namespace

void attach() {
    if (g_app) return;
    g_loop = uWS::Loop::get();
    // KRAKEN_AI_MCP_TOKEN: a fixed token (>= 16 characters) - tests only
    const char* tk = std::getenv("KRAKEN_AI_MCP_TOKEN");
    g_token = tk && strlen(tk) >= 16 ? std::string(tk) : random_token();
    g_port = AI_MCP_PORT;
    if (const char* e = std::getenv("KRAKEN_AI_MCP_PORT"); e && *e) g_port = atoi(e);
    if (g_port <= 0 || g_port > 65535) return;
    g_app = new uWS::App();
    g_app->post("/mcp", [](auto* res, auto* req) {
              auto p = std::make_shared<Pending>();
              p->res = res;
              res->onAborted([p] { p->aborted = true; });
              // loopback, by name too (DNS rebinding); never a browser (no
              // Origin); the run's token
              if (!host_ok(req->getHeader("host")) || !req->getHeader("origin").empty()) return respond(p, "403 Forbidden", "");
              if (!token_ok(req->getHeader("authorization"))) return respond(p, "401 Unauthorized", "");
              res->onData([p](std::string_view chunk, bool last) {
                  if (p->done) return;
                  p->body.append(chunk);
                  if (p->body.size() > (1u << 20)) return respond(p, "413 Payload Too Large", "");
                  if (last) handle_rpc(p);
              });
          })
        .get("/mcp", [](auto* res, auto*) { res->writeStatus("405 Method Not Allowed")->writeHeader("Allow", "POST")->end(); })
        .any("/*", [](auto* res, auto*) { res->writeStatus("404 Not Found")->end(); })
        .listen("127.0.0.1", g_port, [](auto* ls) {
            if (ls) {
                g_listening = true;
                std::cout << "AI assistant MCP server on http://127.0.0.1:" << g_port << "/mcp" << std::endl;
            } else {
                std::cerr << "AI assistant: cannot listen on 127.0.0.1:" << g_port
                          << " for its MCP server - the assistant can't see or control the receiver" << std::endl;
            }
        });
}

std::string to_json(const geo::Json& j) { return dump(j); }

void page_reply(const std::string& json) {
    Json j;
    if (json.size() > (512u << 10) || !geo::json_parse(json, &j) || j.type != Json::OBJ) return;
    std::shared_ptr<PageCall> pc;
    {
        std::lock_guard<std::mutex> lk(g_pc_mu);
        auto it = g_pc.find(j.str("id"));
        if (it == g_pc.end()) return;
        pc = it->second;
    }
    std::lock_guard<std::mutex> lk(pc->mu);
    const Json* ack = j.get("ack");
    if (ack && ack->type == Json::BOOL && ack->b) {
        pc->acked = true;
    } else {
        const Json* ok = j.get("ok");
        pc->done = true;
        pc->ok = ok && ok->type == Json::BOOL && ok->b;
        const Json* res = j.get("result");
        pc->result = pc->ok ? (res ? dump(*res) : std::string("{}")) : (j.str("error").empty() ? "failed" : j.str("error"));
    }
    pc->cv.notify_all();
}

std::string url() { return g_listening.load() ? "http://127.0.0.1:" + std::to_string(g_port) + "/mcp" : std::string(); }
std::string token() { return g_token; }

}  // namespace ai_mcp
