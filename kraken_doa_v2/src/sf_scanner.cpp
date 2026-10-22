// 📻 Saved-frequency scanner - see sf_scanner.hpp (what it does) and
// sf_scan_plan.hpp (the bands, the level of an entry)

#include "sf_scanner.hpp"

#include "channel_manager.hpp"
#include "config.hpp"
#include "control_handler.hpp"
#include "decimator_manager.hpp"
#include "digital/dig_plugin.hpp"
#include "digital/dig_report.hpp"
#include "globals.hpp"
#include "message_builders.hpp"
#include "networking/websocket_server.hpp"
#include "playback.hpp"
#include "saved_freqs.hpp"
#include "scanner_manager.hpp"
#include "sf_scan_plan.hpp"
#include "signal_processing/audio_mixer.hpp"
#include "signal_processing/fft_processor.hpp"
#include "signal_processing/noise_floor.hpp"
#include "utils/json_escape.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

extern std::atomic<float> current_edge_clip;   // main.cpp

namespace sfscan {
namespace {

using DecimatorInstance = DecimatorManager::DecimatorInstance;

constexpr int TICK_MS = 100;
constexpr int64_t SETTLE_MS = 250;            // the retuned frequency in the stream -> the band is measured
constexpr int64_t HEADER_TIMEOUT_MS = 6000;   // a retune not seen in the stream by then: that band is left out
constexpr int64_t CAL_GIVE_UP_MS = 20000;     // phase state "calibrating" without the noise source: ignored after
constexpr float HYST_DB = 3.0f;               // a frequency stays active down to squelch - this
// heimdall's calibration HOLD while searching (coherent): a lease renewed
// every HOLD_RENEW_MS for HOLD_LEASE_MS - a scan that ends without its
// release (kraken_doa stopped) lets it lapse within the lease
constexpr int HOLD_LEASE_MS = 3000;
constexpr int64_t HOLD_RENEW_MS = 1000;
// a lock with a calibration pending: muted until it has run - unless none
// starts within this (a heimdall without the hold calibrates after its own
// cooldown)
constexpr int64_t CAL_START_WAIT_MS = 1500;
constexpr int CONFIRM_TICKS = 2;              // over the squelch on this many polls in a row = active
constexpr size_t MAX_ACT = 50, MAX_SKIPPED = 50;
constexpr int VFO_LIMIT = 16;                 // = DecimatorManager::MAX_DECIMATORS (private there)

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// After an FFT averaging reset (retune, noise source switched, calibration)
// the average restarts from one frame: nothing is measured until that frame
// weighs < 5 % in it - AVG and the FFT size set how long (0.12 s at the
// defaults 0.15 / 16384) - + 0.1 s, 0.25 .. 2.5 s. Was a fixed 1 s (the
// continuous scanner's): every band's first second lost
int64_t rebuild_ms() {
    float a = averaging_alpha.load(std::memory_order_relaxed);
    if (!(a > 0.001f)) a = 0.001f;
    const double frames = a >= 1.0f ? 1.0 : std::log(0.05) / std::log(1.0 - a);
    const double frame_s = std::max(1, current_fft_size.load()) / static_cast<double>(SAMPLE_RATE);
    return std::clamp<int64_t>(std::llround((frames * frame_s + 0.1) * 1000), 250, 2500);
}

// settings (persisted by the control handler)
std::atomic<float> g_squelch{12.0f};
std::atomic<int> g_dwell{2000}, g_hang{2000}, g_max{0};
std::atomic<bool> g_multi{false};   // one VFO per active frequency (the scan makes the extra ones)

std::mutex g_life;                 // start / stop
std::thread g_thread;
std::atomic<bool> g_run{false};
std::atomic<uint32_t> g_run_id{0};
std::atomic<bool> g_hold{false}, g_skip{false}, g_stop_asked{false};
std::atomic<bool> g_listening{false};
std::atomic<uint32_t> g_tuner_mask{0};
std::mutex g_cv_mu;
std::condition_variable g_cv;
bool g_dispatching = false;        // uWS loop only
std::function<void()> g_on_stop;   // uWS loop only

struct Cfg {
    int vfo = -1;
    bool independent = false;
    bool wideband = false;   // wideband scan mode (tuners side by side); neither = coherent
    bool was_heard = false;
    // the scan VFO's settings at the start: what an extra VFO starts from
    // (one VFO per active frequency) before the entry's own are applied
    std::string demod, dec;
    int bw = -1, sq_method = 0;
    bool sq_en = false, transcribe = false;
    float sq_level = 0;
};
Cfg g_cfg;   // written by start() before the thread starts, read after stop()'s join

std::mutex g_status_mu;
std::string g_status;   // the running scan's latest status (the thread builds it)
std::string g_msg;      // why the last scan stopped (uWS loop)

bool alive(uint32_t id) { return g_run.load() && g_run_id.load() == id; }

// The scan's commands go through the normal command path on the uWS loop
// (applied like a browser's, shown in every page) - but not saved: the
// settings file keeps the user's tuning (dispatching())
void dispatch(const std::vector<std::string>& cmds) {
    g_dispatching = true;
    for (const auto& c : cmds) {
        const std::string why = ControlHandler::apply_command(c);
        if (!why.empty()) std::cerr << "Saved-frequency scan: '" << c << "' refused (" << why << ")" << std::endl;
    }
    g_dispatching = false;
}
void post(uint32_t id, std::vector<std::string> cmds) {
    if (!loop || cmds.empty()) return;
    loop->defer([id, cmds = std::move(cmds)] {
        if (alive(id)) dispatch(cmds);
    });
}

// the VFO heard or not (only that one changes)
void set_heard(int vfo, bool on) {
    if (!decimator_manager.getDecimator(vfo)) return;
    auto heard = decimator_manager.audioVfoIds();
    const bool is = std::find(heard.begin(), heard.end(), vfo) != heard.end();
    if (is == on) return;
    if (on) heard.push_back(vfo);
    else heard.erase(std::remove(heard.begin(), heard.end(), vfo), heard.end());
    ControlHandler::set_audio_vfos(heard);
}

// A decoder that can't be scanned: one that holds its VFO on a fixed
// frequency (ADS-B, AIS) or on its tuner's centre (ism, cells, ...)
std::string decoder_why_not(const std::string& dec) {
    if (dec.rfind("PLUGIN:", 0) != 0) return "";
    dig::PluginInfo pi;
    if (!dig::PluginRegistry::instance().get(dec.substr(7), &pi)) return "";   // (refused when it locks)
    if (pi.fixed_freq_hz > 0 || pi.lock_vfo)
        return "its decoder (" + (pi.name.empty() ? pi.id : pi.name) + ") needs a VFO of its own";
    return "";
}

// what a Digital VFO's decoder needs (Hz): the page's digMinRateK
double decoder_need_hz(const std::string& dec) {
    if (dec.rfind("PLUGIN:", 0) == 0) {
        dig::PluginInfo pi;
        if (dig::PluginRegistry::instance().get(dec.substr(7), &pi)) return pi.min_vfo_rate > 0 ? pi.min_vfo_rate : 12500;
    }
    return 24000;   // Auto detect / off (TETRA's 24 kHz)
}

void tunable_range(uint64_t* lo, uint64_t* hi) {
    *lo = RTL_TUNER_MIN_HZ;
    *hi = RTL_TUNER_MAX_HZ;
    if (wb_variant_enabled.load(std::memory_order_relaxed)) wb_variant_rf_union_range(*lo, *hi);
}

double span_hz() {
    float clip = current_edge_clip.load(std::memory_order_relaxed);
    if (!(clip >= 0.1f)) clip = 0.1f;
    if (clip > 1.0f) clip = 1.0f;
    return static_cast<double>(clip) * SAMPLE_RATE;
}

int64_t vfo_bandwidth_hz(int vfo) {
    auto inst = decimator_manager.getDecimator(vfo);
    return std::llround(get_bandwidth_option(inst ? inst->bandwidth_index.load() : DEFAULT_BANDWIDTH_INDEX).bandwidth_mhz * 1e6);
}

// The tuners a band is received with: in wideband scan mode heimdall's tuner
// spread over the open tuners (setup_wideband_frequencies: spacing = edge clip
// x sample rate - float maths as there), else one tuner, edge-clipped
Layout layout_for(bool wideband) {
    if (!wideband) return single_layout(span_hz());
    const float clip = std::clamp(current_edge_clip.load(std::memory_order_relaxed), 0.1f, 1.0f);
    const int n = std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
    return spread_layout(n, static_cast<int64_t>(static_cast<uint32_t>(SAMPLE_RATE * clip)));
}

Plan make_plan(int vfo, const Layout& lay) {
    uint64_t lo, hi;
    tunable_range(&lo, &hi);
    return build_plan(savedfreq::scan_entries(), lay, lo, hi, vfo_bandwidth_hz(vfo), decoder_why_not);
}

bool is_extra(const DecimatorInstance* p);

// Independent mode: the tuners no other VFO is on (lowest first) - the scan's
// own extra VFOs (one per active frequency) don't count
std::vector<int> free_tuners(int vfo) {
    const int n = std::min(active_num_elements.load(), MAX_CHANNELS);
    uint32_t busy = 0;
    for (const auto& d : decimator_manager.getAllDecimators()) {
        if (!d || d->id == vfo || d->being_deleted.load() || is_extra(d.get())) continue;
        const int t = d->tuner_channel.load();
        if (t >= 0 && t < 32) busy |= 1u << t;
    }
    std::vector<int> out;
    for (int ch = 0; ch < n; ch++)
        if (!(busy >> ch & 1u)) out.push_back(ch);
    return out;
}

// the VFO's decoder holds it (fixed frequency / tuner centre): "" if not
std::string vfo_held_by(int vfo) {
    for (const auto& d : decimator_manager.getDecimatorInfoList()) {
        if (d.id != vfo) continue;
        if (static_cast<dig::Mode>(d.digital_mode) != dig::Mode::PLUGIN) return "";
        return decoder_why_not("PLUGIN:" + d.digital_plugin).empty() ? "" : d.digital_plugin;
    }
    return "";
}

bool gain_equal(double a, double b) {
    if (a < 0 && b < 0 && a > -100 && b > -100) return true;   // both Auto (-1); -999 = none
    return std::fabs(a - b) < 0.05;
}

// Heimdall is calibrating the array (coherent mode): the noise source on (or
// just off), or the phase compensation not settled - the spectrum isn't the
// antennas'. heimdall's PhaseCompensatorState: 0 WAITING_LAG, 1 MEASURING,
// 2 APPLYING, 3 VERIFYING, 4 CONVERGED, 5 WAITING_STABILITY (after a retune:
// identity compensation, noise off - the spectrum is fine), 6 MEASURING_PER_BIN.
// KerberosSDR manual mode parks with a low state and never calibrates by
// itself: only the noise source counts there (as the continuous scanner)
bool phase_calibrating() {
    if (server_kerberos_mode.load(std::memory_order_relaxed)) return false;
    const uint32_t ph = scanner_manager.getPhaseState();
    return ph <= 3 || ph == 6;
}

void broadcast(const std::string& s) { WebSocketServer::broadcast_json_message(s); }

// One VFO per active frequency (g_multi): the VFOs the scan made for the
// second, third ... active frequency, by a token per lock. Made and removed
// on the uWS loop, looked up by the scan thread. A VFO is the scan's only
// while its instance is the one it made: ids are reused (the user may remove
// one and add another under the same id - that one must never be removed)
struct Extra {
    int vfo = -1;                              // -1 = not made yet
    std::weak_ptr<DecimatorInstance> inst;
    bool failed = false;                       // couldn't be made (the VFO limit)
};
std::mutex g_xmu;
std::map<uint64_t, Extra> g_extras;
std::atomic<uint64_t> g_next_tok{0};

// the extra's VFO now: >= 0, -1 = not made yet, -2 = gone (removed by
// hand), -3 = couldn't be made
int extra_vfo(uint64_t tok) {
    Extra x;
    {
        std::lock_guard<std::mutex> lk(g_xmu);
        auto it = g_extras.find(tok);
        if (it == g_extras.end()) return -2;
        x = it->second;
    }
    if (x.failed) return -3;
    if (x.vfo < 0) return -1;
    auto now = decimator_manager.getDecimator(x.vfo);
    return now && now == x.inst.lock() ? x.vfo : -2;
}

bool is_extra(const DecimatorInstance* p) {
    std::lock_guard<std::mutex> lk(g_xmu);
    for (const auto& kv : g_extras)
        if (auto s = kv.second.inst.lock(); s && s.get() == p) return true;
    return false;
}

// uWS loop: an extra VFO muted (removing the last VFO heard would make
// another one heard) and removed - by the manager itself, not
// REMOVE_DECIMATOR: its decimator_removed makes every page reset its audio
// player, a gap in what the other VFOs play. false = nothing removed
bool remove_extra(const Extra& x) {
    if (x.vfo < 0) return false;
    auto now = decimator_manager.getDecimator(x.vfo);
    if (!now || now != x.inst.lock()) return false;
    set_heard(x.vfo, false);
    if (!decimator_manager.removeDecimator(x.vfo)) return false;
    audio_mixer.retain(decimator_manager.audioVfoIds());
    std::cout << "Saved-frequency scan: D" << x.vfo << " removed" << std::endl;
    return true;
}
void drop_extra(uint64_t tok) {
    Extra x;
    {
        std::lock_guard<std::mutex> lk(g_xmu);
        auto it = g_extras.find(tok);
        if (it == g_extras.end()) return;
        x = it->second;
    }
    const bool removed = remove_extra(x);
    {
        // out of the list only now: till it is gone it is the scan's (free_tuners)
        std::lock_guard<std::mutex> lk(g_xmu);
        g_extras.erase(tok);
    }
    if (removed) broadcast(MessageBuilders::build_decimator_info_message());
}

// uWS loop: a new VFO for token tok, set up as the scan VFO was at the start
// (its demod, bandwidth, decoder, squelch, Transcribe); -1 = the VFO limit
int make_extra(uint64_t tok, const Cfg& cfg) {
    const int v = decimator_manager.addDecimator();   // muted, at 0 Hz
    auto inst = v >= 0 ? decimator_manager.getDecimator(v) : nullptr;
    {
        std::lock_guard<std::mutex> lk(g_xmu);
        Extra& x = g_extras[tok];
        if (inst) {
            x.vfo = v;
            x.inst = inst;
        } else {
            x.failed = true;
        }
    }
    if (!inst) {
        std::cerr << "Saved-frequency scan: no VFO for another active frequency (" << VFO_LIMIT
                  << " VFOs at most)" << std::endl;
        return -1;
    }
    const std::string s = std::to_string(v);
    std::vector<std::string> c;
    if (!cfg.demod.empty()) c.push_back("SET_DECIMATOR_DEMOD:" + s + ":" + cfg.demod);
    if (cfg.bw >= 0) c.push_back("SET_DECIMATOR_BW:" + s + ":" + std::to_string(cfg.bw));
    if (!cfg.dec.empty() && cfg.dec != "OFF") c.push_back("DIGITAL_MODE:" + s + ":" + cfg.dec);
    c.push_back("DEC_SQUELCH_METHOD:" + s + ":" + (cfg.sq_method == 3 ? "DIGITAL" : "FFT"));
    char lv[64];
    snprintf(lv, sizeof lv, "DEC_SQUELCH_LEVEL:%d:%.1f", v, cfg.sq_level);
    c.push_back(lv);
    c.push_back("DEC_SQUELCH_ENABLE:" + s + ":" + (cfg.sq_en ? "1" : "0"));
    if (cfg.transcribe) c.push_back("VOICE2TEXT_VFO:" + s + ":1");
    dispatch(c);
    std::cout << "Saved-frequency scan: D" << v << " made for another active frequency" << std::endl;
    // the pages list it (decimator_info - not decimator_added, which resets their audio)
    broadcast(MessageBuilders::build_decimator_info_message());
    return v;
}

// heimdall's calibration hold (hold_calibration): ms > 0 = hold for that
// long from now, 0 = release - a pending calibration starts at once
void hold_calibration(int ms) {
    char c[64];
    snprintf(c, sizeof c, "{\"hold_calibration\":{\"hold_ms\":%d}}", ms);
    ControlHandler::send_control_command(c);
}

std::string settings_json() {
    std::ostringstream o;
    o << "\"sq\":" << g_squelch.load() << ",\"dwell\":" << g_dwell.load() << ",\"hang\":" << g_hang.load()
      << ",\"max\":" << g_max.load() << ",\"multi\":" << (g_multi.load() ? "true" : "false");
    return o.str();
}

std::string idle_status() {
    return "{\"sfscan\":{\"run\":false," + settings_json() + ",\"msg\":\"" + json_escape(g_msg) + "\"}}";
}

void request_stop(uint32_t id, const std::string& why) {
    if (g_stop_asked.exchange(true) || !loop) return;
    std::cout << "Saved-frequency scan: stopping - " << why << std::endl;
    loop->defer([id, why] {
        if (alive(id)) stop(why);
    });
}

// ============================================================================
// The scan thread
// ============================================================================
class Scan {
public:
    Scan(Cfg cfg, uint32_t id) : cfg_(cfg), id_(id) {}
    void run();

private:
    enum State { TUNE, SETTLE, WATCH, LISTEN };
    struct Slot {
        int ch = 0;            // tuner (independent) / the FFT channel (coherent)
        int band = -1;         // plan band watched, -1 = none this turn
        int64_t centre = 0;    // its tuner centre (Hz)
        bool retuned = false;  // a retune was sent this turn
        bool seen = false;     // ... and is in the stream
        int64_t seen_ms = 0;
        bool failed = false;   // ... never showed up: not watched this turn
    };

    // A frequency listened to: on the scan's VFO, or (one VFO per active
    // frequency) on an extra VFO the scan made for it
    struct Lock {
        int entry = -1, slot = -1;
        std::string id;
        uint64_t tok = 0;           // 0 = the scan's VFO, else the extra VFO's token (g_extras)
        int vfo = -1;               // the VFO (an extra's: -1 until it is made)
        int64_t t_active = 0;       // last time over the squelch (or not measurable)
        int64_t listened_ms = 0;    // clean time on it so far (Longest stay)
        bool heard = false;         // the VFO heard now (muted while heimdall calibrates)
        // locked with a calibration pending (right after a retune, the hold
        // released): muted until it has run (cal_seen: it started)
        bool cal_wait = false, cal_seen = false;
        int64_t cal_wait_t = 0;
        float db = -999;
    };

    void tick();
    void rebuild();
    void tune();
    void settle();
    bool ready();
    void measure();
    void watch();
    void listen();
    bool primary_locked() const;
    bool can_lock() const;
    bool locked(int entry) const;
    void lock(int entry, int slot);
    void end_lock(size_t i, bool done);
    void after_locks();
    void post_heard(const Lock& l, bool on);
    void place(const savedfreq::ScanEntry& e, const Slot& s, bool heard, uint64_t tok = 0);
    void resume(bool done);
    void next_turn();
    void hold();
    void status(bool now);
    int64_t stream_hz(const Slot& s) const;
    int64_t wide_centre_hz() const;
    bool in_stream(const Slot& s) const;
    const savedfreq::ScanEntry& first_pick(const Slot& s) const;
    bool fft(int ch, std::vector<float>* spec, float* floor_db) const;
    bool coherent() const { return !cfg_.independent && !cfg_.wideband; }
    // the FFT channel an entry of slot s is measured on and that tuner's centre
    int entry_tuner(const Slot& s, int64_t hz, int64_t* centre) const;
    // each tuner's averaged spectrum, fetched once per tick
    struct Specs {
        int got[MAX_CHANNELS];
        std::vector<float> spec[MAX_CHANNELS];
        float floor_db[MAX_CHANNELS];
        Specs() { std::fill(std::begin(got), std::end(got), -1); }
    };
    // an entry's level (dB over the floor) on slot s; -999 = not measurable
    float level(Specs& sp, const Slot& s, const savedfreq::ScanEntry& e);

    Cfg cfg_;
    uint32_t id_;
    Plan plan_;
    uint64_t plan_gen_ = ~0ull;
    float plan_clip_ = -1;
    int plan_tuners_ = -1;         // wideband: the tuner count the layout was made for
    Layout lay_;                   // the tuners a band is received with (layout_for)
    bool wide_place_ = false;      // wideband: pre-place the VFO once the stream has the new tuning
    std::vector<float> shape_;     // wideband: the tuners' common response (nfloor::shape_db)
    uint32_t shape_ver_ = 0;
    State st_ = TUNE;
    bool need_tune_ = false;       // the plan changed while listening: a new turn when it resumes
    std::vector<Slot> slots_;
    int cycle_ = 0;                // first band of this turn (turns: the bands in slot-sized blocks)
    bool first_tune_ = true;
    int64_t t_cmd_ = 0, hold_until_ = 0, ready_ms_ = 0;
    int64_t cal_since_ = 0;        // phase "calibrating" (noise off) since
    bool cal_ignored_ = false;
    uint32_t seen_gen_ = 0;
    std::vector<float> level_;     // per entry, dB over the floor (-999 = not measured)
    std::vector<uint8_t> consec_;  // per entry: polls in a row over the squelch
    // left by Skip / Longest stay: had their turn in this PASS (every band
    // once) - the others go first; a new pass starts them again
    std::set<std::string> done_;
    std::string last_heard_;       // the last one listened to (the next lock starts after it)
    std::vector<Lock> locks_;      // listening (LISTEN): one, or with g_multi one per active frequency
    bool holding_ = false;         // heimdall's calibration held (searching, coherent)
    int64_t hold_sent_ = 0;
    std::vector<std::string> act_;
    int64_t last_status_ = 0;
    std::string last_sent_;
};

void Scan::run() {
    std::cout << "Saved-frequency scan: started on D" << cfg_.vfo << " ("
              << (cfg_.independent ? "independent" : cfg_.wideband ? "wideband scan" : "coherent") << " mode)" << std::endl;
    seen_gen_ = fft_reset_generation.load(std::memory_order_acquire);
    while (g_run.load()) {
        {
            std::unique_lock<std::mutex> lk(g_cv_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(TICK_MS), [] { return !g_run.load(); });
        }
        if (!g_run.load()) break;
        if (g_stop_asked.load()) continue;   // the loop stops it
        try {
            tick();
        } catch (const std::exception& e) {
            std::cerr << "Saved-frequency scan: " << e.what() << std::endl;
            request_stop(id_, std::string("internal error: ") + e.what());
        }
    }
}

void Scan::tick() {
    // what ends the scan
    if (playback::active()) return request_stop(id_, "a playback started (the receiver can't be tuned)");
    if (operating_mode_index() != (cfg_.independent ? 2 : cfg_.wideband ? 1 : 0))
        return request_stop(id_, "the operating mode changed");
    if (!decimator_manager.getDecimator(cfg_.vfo)) return request_stop(id_, "its VFO D" + std::to_string(cfg_.vfo) + " was removed");

    if (savedfreq::generation() != plan_gen_ || current_edge_clip.load() != plan_clip_ ||
        (cfg_.wideband && std::clamp(active_num_elements.load(), 1, MAX_CHANNELS) != plan_tuners_))
        rebuild();
    if (plan_.entries.empty()) {
        return request_stop(id_, plan_.skipped.empty() ? "nothing left to scan"
                                                       : "none of the frequencies left can be scanned: " + plan_.skipped[0].why);
    }
    // independent: another VFO moved onto a scanned tuner -> new turn without it
    if (cfg_.independent && st_ != LISTEN && st_ != TUNE) {
        const auto fr = free_tuners(cfg_.vfo);
        for (const auto& s : slots_)
            if (s.band >= 0 && std::find(fr.begin(), fr.end(), s.ch) == fr.end()) { st_ = TUNE; break; }
    }

    switch (st_) {
        case TUNE: tune(); break;
        case SETTLE: settle(); break;
        case WATCH: watch(); break;
        case LISTEN: listen(); break;
    }
    hold();
    status(false);
}

// Coherent mode: while it searches, heimdall's retunes don't recalibrate
// (hold_calibration - a calibration per band cost ~3.5 s); a lock releases
// the hold and the calibration starts at once (the user's call 2026-10-22).
// Not in KerberosSDR manual mode (no automatic calibrations to hold)
void Scan::hold() {
    if (!coherent() || server_kerberos_mode.load(std::memory_order_relaxed)) return;
    const int64_t now = now_ms();
    if (st_ != LISTEN) {
        if (!holding_ || now - hold_sent_ >= HOLD_RENEW_MS) {
            hold_calibration(HOLD_LEASE_MS);
            hold_sent_ = now;
            holding_ = true;
        }
    } else if (holding_) {
        hold_calibration(0);
        holding_ = false;
    }
}

void Scan::rebuild() {
    plan_gen_ = savedfreq::generation();
    plan_clip_ = current_edge_clip.load();
    plan_tuners_ = std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
    lay_ = layout_for(cfg_.wideband);
    plan_ = make_plan(cfg_.vfo, lay_);
    level_.assign(plan_.entries.size(), -999.0f);
    consec_.assign(plan_.entries.size(), 0);
    std::cout << "Saved-frequency scan: " << plan_.entries.size() << " frequenc"
              << (plan_.entries.size() == 1 ? "y" : "ies") << " in " << plan_.bands.size() << " band"
              << (plan_.bands.size() == 1 ? "" : "s");
    if (!plan_.skipped.empty()) std::cout << " (" << plan_.skipped.size() << " left out)";
    std::cout << std::endl;
    // the ones listened to: still in the list? (entry indices are the new plan's)
    for (size_t i = locks_.size(); i-- > 0;) {
        int at = -1;
        for (size_t k = 0; k < plan_.entries.size(); k++)
            if (plan_.entries[k].id == locks_[i].id) at = static_cast<int>(k);
        if (at < 0) end_lock(i, false);   // no longer scanned
        else locks_[i].entry = at;
    }
    if (cycle_ >= static_cast<int>(plan_.bands.size())) cycle_ = 0;
    // a new turn (the slots' bands are plan indices); one that needs no
    // retune costs nothing (tune() keeps a centre that already holds the band).
    // Listening, the slots keep their tuning (the ones listened to are
    // measured on their slots) and get their new bands when it resumes
    for (auto& s : slots_) s.band = -1;
    if (st_ == LISTEN) need_tune_ = true;
    else st_ = TUNE;
    after_locks();   // none left listening: on with the scan
}

void Scan::tune() {
    std::vector<int> chs;
    if (cfg_.independent) {
        chs = free_tuners(cfg_.vfo);
        if (chs.empty()) return request_stop(id_, "every tuner has another VFO on it - free one for the scan");
    } else if (cfg_.wideband) {
        chs = {-1};   // every tuner, side by side: one band
    } else {
        chs = {std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1)};
    }
    const int nb = static_cast<int>(plan_.bands.size());
    const int ns = static_cast<int>(chs.size());
    if (nb <= ns) cycle_ = 0;
    const int64_t vfo_hz = vfo_bandwidth_hz(cfg_.vfo);
    std::vector<std::string> cmds;
    slots_.assign(chs.size(), Slot{});
    uint32_t mask = 0;
    for (int k = 0; k < ns; k++) {
        Slot& s = slots_[k];
        s.ch = chs[k];
        if (cycle_ + k >= nb) continue;   // more tuners than this turn's bands: the rest stay as they are
        s.band = cycle_ + k;
        const Band& b = plan_.bands[s.band];
        const int64_t now_hz = cfg_.independent ? static_cast<int64_t>(exact_tuner_hz(s.ch))
                               : cfg_.wideband  ? wide_centre_hz()
                                                : static_cast<int64_t>(coherent_tuner_hz());
        // a centre that already holds the band stays (a coherent retune recalibrates)
        const bool move = !band_fits(plan_, s.band, now_hz, lay_, vfo_hz);
        s.centre = move ? b.centre_hz : now_hz;
        s.retuned = move;
        s.seen = !move;
        char c[96];
        if (move) {
            // (wideband: FREQ would end wideband mode - WIDEBAND_FREQ moves the spread)
            if (cfg_.independent) snprintf(c, sizeof c, "TUNER_FREQ:%d:%.6f", s.ch, b.centre_hz / 1e6);
            else if (cfg_.wideband) snprintf(c, sizeof c, "WIDEBAND_FREQ:%.6f", b.centre_hz / 1e6);
            else snprintf(c, sizeof c, "FREQ:%.6f", b.centre_hz / 1e6);
            cmds.push_back(c);
        }
        if (b.has_gain) {
            const double g_now = cfg_.independent ? indep_tuner_gain_db[s.ch].load()
                                                  : ChannelManager::get_gain(active_channel.load());
            if (!gain_equal(g_now, b.gain)) {
                if (cfg_.independent) snprintf(c, sizeof c, "TUNER_GAIN:%d:%.1f", s.ch, b.gain);
                else snprintf(c, sizeof c, "GAIN:%.1f", b.gain);
                cmds.push_back(c);
            }
        }
        if (cfg_.independent) mask |= 1u << s.ch;
    }
    g_tuner_mask = mask;
    post(id_, cmds);
    // The VFO onto the band's first frequency that hasn't had its turn - its
    // demod / bandwidth / decoder, muted - together with the retune: a
    // coherent retune calibrates for a few seconds, and the VFO is then in
    // place (filters settled, decoder started) when the data is clean.
    // Wideband: once the stream has the new tuning (settle) - the receiver
    // puts a VFO on the tuner nearest it by the tuners' frequencies there
    wide_place_ = false;
    for (const auto& s : slots_) {
        if (s.band < 0 || (!s.retuned && !first_tune_)) continue;
        if (cfg_.wideband) wide_place_ = true;
        else place(first_pick(s), s, false);
        break;
    }
    first_tune_ = false;
    t_cmd_ = now_ms();
    st_ = SETTLE;
}

// the band's first frequency that hasn't had its turn this pass (else its first)
const savedfreq::ScanEntry& Scan::first_pick(const Slot& s) const {
    const auto& ents = plan_.bands[s.band].entries;
    for (int i : ents)
        if (!done_.count(plan_.entries[i].id)) return plan_.entries[i];
    return plan_.entries[ents.front()];
}

int64_t Scan::stream_hz(const Slot& s) const {
    return static_cast<int64_t>(tuner_frequencies[cfg_.independent ? s.ch : 0].load(std::memory_order_relaxed));
}

// wideband: the spread's centre now (tuner n / 2's, offset 0), whole kHz as
// the scan tunes it
int64_t Scan::wide_centre_hz() const {
    const int k0 = static_cast<int>(lay_.offs.size()) / 2;
    return std::llround(static_cast<double>(tuner_frequencies[k0].load(std::memory_order_relaxed)) / 1000.0) * 1000;
}

// the slot's tuning in the stream (wideband: every tuner of the spread)
bool Scan::in_stream(const Slot& s) const {
    if (!cfg_.wideband) return std::llabs(stream_hz(s) - s.centre) < 2000;
    for (size_t k = 0; k < lay_.offs.size() && k < static_cast<size_t>(MAX_CHANNELS); k++) {
        const int64_t f = static_cast<int64_t>(tuner_frequencies[k].load(std::memory_order_relaxed));
        if (std::llabs(f - (s.centre + lay_.offs[k])) >= 2000) return false;
    }
    return true;
}

void Scan::settle() {
    const int64_t now = now_ms();
    bool all = true, any_retuned = false;
    int64_t last_seen = 0;
    for (auto& s : slots_) {
        if (s.band < 0 || !s.retuned) continue;
        any_retuned = true;
        if (!s.seen && !s.failed) {
            if (in_stream(s)) {
                s.seen = true;
                s.seen_ms = now;
            } else if (now - t_cmd_ > HEADER_TIMEOUT_MS) {
                s.failed = true;
                std::cerr << "Saved-frequency scan: "
                          << (cfg_.wideband ? std::string("the tuners") : "tuner " + std::to_string(s.ch))
                          << " didn't reach " << s.centre / 1e6 << " MHz - that band is left out this turn"
                          << std::endl;
            } else {
                all = false;
            }
        }
        if (s.seen) last_seen = std::max(last_seen, s.seen_ms);
    }
    if (!all || (any_retuned && now - last_seen < SETTLE_MS)) return;
    if (any_retuned) {
        // the averaged spectrum still holds the old band (independent-mode
        // retunes don't reset it; coherent ones do when heimdall finishes)
        fft_reset_generation.fetch_add(1, std::memory_order_release);
        fft_data_valid.store(false, std::memory_order_release);
    }
    if (wide_place_) {   // wideband: the VFO onto the band now (tune())
        wide_place_ = false;
        for (const auto& s : slots_)
            if (s.band >= 0 && !s.failed) {
                place(first_pick(s), s, false);
                break;
            }
    }
    std::fill(consec_.begin(), consec_.end(), 0);
    std::fill(level_.begin(), level_.end(), -999.0f);
    ready_ms_ = 0;
    st_ = WATCH;
}

bool Scan::ready() {
    const int64_t now = now_ms();
    const uint32_t gen = fft_reset_generation.load(std::memory_order_acquire);
    if (gen != seen_gen_) {
        seen_gen_ = gen;
        hold_until_ = std::max(hold_until_, now + rebuild_ms());
    }
    bool cal = false;
    if (coherent()) {
        // the noise source on: always calibration. The phase state alone is
        // trusted for CAL_GIVE_UP_MS (a heimdall that never settles must not
        // stop the scan for good)
        const bool noise = scanner_manager.isNoiseRecentlyActive();
        const bool phase = phase_calibrating();
        if (phase && !noise) {
            if (!cal_since_) cal_since_ = now;
            if (!cal_ignored_ && now - cal_since_ > CAL_GIVE_UP_MS) {
                cal_ignored_ = true;
                std::cerr << "Saved-frequency scan: heimdall's calibration hasn't settled for "
                          << CAL_GIVE_UP_MS / 1000 << " s - scanning anyway" << std::endl;
            }
        } else {
            cal_since_ = 0;
            cal_ignored_ = false;
        }
        cal = noise || (phase && !cal_ignored_);
    } else {
        cal = scanner_manager.isNoiseRecentlyActive();
    }
    if (cal || server_retuning.load(std::memory_order_relaxed)) hold_until_ = std::max(hold_until_, now + rebuild_ms());
    return !cal && !server_retuning.load(std::memory_order_relaxed) && data_ready.load() &&
           fft_data_valid.load(std::memory_order_acquire) && now >= hold_until_;
}

bool Scan::fft(int ch, std::vector<float>* spec, float* floor_db) const {
    {
        std::lock_guard<std::mutex> lk(fft_mutex);
        if (ch < 0 || ch >= static_cast<int>(fft_averaged.size()) || ch >= num_channels.load()) return false;
        *spec = fft_averaged[ch];
    }
    if (spec->size() < 16) return false;
    *floor_db = FFTProcessor::channel_noise_floor(ch, *spec);
    return *floor_db > -190.0f;
}

int Scan::entry_tuner(const Slot& s, int64_t hz, int64_t* centre) const {
    if (cfg_.independent) {
        *centre = s.centre;
        return s.ch;
    }
    if (cfg_.wideband) {   // the tuner nearest it (where the receiver puts its VFO)
        const int k = nearest_tuner(lay_, s.centre, hz);
        *centre = s.centre + lay_.offs[k];
        return k;
    }
    *centre = s.centre;
    return std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1);
}

float Scan::level(Specs& sp, const Slot& s, const savedfreq::ScanEntry& e) {
    int64_t tc = 0;
    const int ch = entry_tuner(s, e.hz, &tc);
    if (ch < 0 || ch >= MAX_CHANNELS) return -999.0f;
    if (sp.got[ch] < 0) {
        sp.got[ch] = fft(ch, &sp.spec[ch], &sp.floor_db[ch]) ? 1 : 0;
        // wideband scan mode: the tuners' common response (tilt, roll-off)
        // taken off as the stitched spectrum does - its floor (nfloor) is that
        // flattened spectrum's, so this is the level the page shows (and the
        // scan's squelch line is drawn against)
        if (sp.got[ch] && cfg_.wideband) {
            std::vector<float>& v = sp.spec[ch];
            nfloor::shape_db(static_cast<int>(v.size()), shape_, &shape_ver_);
            if (shape_.size() == v.size())
                for (size_t i = 0; i < v.size(); i++) v[i] -= shape_[i];
        }
    }
    if (!sp.got[ch]) return -999.0f;
    return entry_level(sp.spec[ch], sp.floor_db[ch], static_cast<double>(tc), SAMPLE_RATE, e.hz, channel_width_hz(e));
}

// every watched entry's level; consec_ counts the polls over the squelch
void Scan::measure() {
    const float sq = g_squelch.load();
    Specs sp;
    for (const auto& s : slots_) {
        if (s.band < 0 || s.failed) continue;
        for (int i : plan_.bands[s.band].entries) {
            const float lv = level(sp, s, plan_.entries[i]);
            if (lv <= -900.0f) continue;   // no spectrum (yet)
            level_[i] = lv;
            consec_[i] = lv >= sq ? static_cast<uint8_t>(std::min(consec_[i] + 1, 100)) : 0;
        }
    }
}

void Scan::watch() {
    if (!ready()) return;
    ready_ms_ += TICK_MS;
    measure();
    // the active ones, in list order - those that had their turn this pass wait
    std::vector<std::pair<int, int>> act;   // (entry, slot)
    bool done_active = false;
    act_.clear();
    for (int k = 0; k < static_cast<int>(slots_.size()); k++) {
        const Slot& s = slots_[k];
        if (s.band < 0 || s.failed) continue;
        for (int i : plan_.bands[s.band].entries) {
            if (consec_[i] < CONFIRM_TICKS) continue;
            if (act_.size() < MAX_ACT) act_.push_back(plan_.entries[i].id);
            if (done_.count(plan_.entries[i].id)) done_active = true;
            else act.push_back({i, k});
        }
    }
    if (!act.empty()) {
        // the first after the one heard last (round the list), so two busy
        // frequencies take turns; one VFO per active frequency: every one,
        // in that order, while there are VFOs to be had
        std::sort(act.begin(), act.end());
        int after = -1;
        for (size_t i = 0; i < plan_.entries.size(); i++)
            if (plan_.entries[i].id == last_heard_) after = static_cast<int>(i);
        auto it = std::find_if(act.begin(), act.end(), [after](const auto& a) { return a.first > after; });
        if (it != act.end()) std::rotate(act.begin(), it, act.end());
        for (const auto& [i, k] : act) {
            if (!can_lock()) break;   // without g_multi: the first only
            lock(i, k);
        }
        return;
    }
    const int nb = static_cast<int>(plan_.bands.size());
    const int ns = static_cast<int>(slots_.size());
    const bool skip = g_skip.exchange(false);
    if (g_hold.load()) return;
    if (nb > ns) {
        // the next bands: at once when what is active here has had its turn
        // (or Skip), else after Dwell of watching them (it adds up over the
        // turn - a busy channel can't hold its band for good)
        if (skip || done_active || ready_ms_ >= g_dwell.load()) next_turn();
    } else if (done_active) {
        // every band watched at once and all that is active has had its
        // turn: a new pass (they take turns again, from the one after the
        // last heard)
        done_.clear();
    }
}

void Scan::next_turn() {
    cycle_ += static_cast<int>(slots_.size());
    if (cycle_ >= static_cast<int>(plan_.bands.size())) {
        cycle_ = 0;
        done_.clear();   // back at the first band: a new pass
    }
    st_ = TUNE;
}

bool Scan::primary_locked() const {
    for (const auto& l : locks_)
        if (!l.tok) return true;
    return false;
}

// a VFO for one more frequency: the scan's own when it is free, else (one VFO
// per active frequency) a new one while the receiver has room for it
bool Scan::can_lock() const {
    if (!primary_locked()) return true;
    if (!g_multi.load()) return false;
    int pending = 0;   // asked for, not made yet
    for (const auto& l : locks_)
        if (l.tok && l.vfo < 0) pending++;
    return static_cast<int>(decimator_manager.getAllDecimators().size()) + pending < VFO_LIMIT;
}

bool Scan::locked(int entry) const {
    for (const auto& l : locks_)
        if (l.entry == entry) return true;
    return false;
}

void Scan::lock(int entry, int slot) {
    const auto e = plan_.entries[entry];
    const Slot& s = slots_[slot];
    Lock l;
    l.entry = entry;
    l.slot = slot;
    l.id = e.id;
    // the scan's VFO first, then extra ones (can_lock decided there is room)
    if (primary_locked()) {
        l.tok = g_next_tok.fetch_add(1) + 1;
        std::lock_guard<std::mutex> lk(g_xmu);
        g_extras[l.tok];   // asked for: a stop removes it even if it is made after
    } else {
        l.vfo = cfg_.vfo;
    }
    last_heard_ = e.id;
    l.t_active = l.cal_wait_t = now_ms();
    // right after a retune in coherent mode heimdall sits in its cooldown
    // (held): the release starts the calibration - muted until it has run,
    // instead of a moment of audio cut off by the noise source
    l.cal_wait = coherent() && !server_kerberos_mode.load(std::memory_order_relaxed) &&
                 scanner_manager.getPhaseState() == 5;   // WAITING_FOR_STABILITY
    l.heard = !l.cal_wait;
    l.db = level_[entry];
    if (st_ != LISTEN) {   // the first one
        act_.clear();
        g_skip = false;
    }
    locks_.push_back(l);
    g_listening = primary_locked();
    st_ = LISTEN;
    std::cout << "Saved-frequency scan: " << e.name << " " << e.hz / 1e6 << " MHz active (" << l.db << " dB) -> "
              << (l.tok ? std::string("a VFO of its own") : "D" + std::to_string(cfg_.vfo)) << std::endl;

    place(e, s, l.heard, l.tok);
    status(true);
}

// its turn is over (done: until the next pass); an extra VFO goes, the
// scan's own is muted
void Scan::end_lock(size_t i, bool done) {
    if (i >= locks_.size()) return;
    const Lock l = locks_[i];
    locks_.erase(locks_.begin() + static_cast<std::ptrdiff_t>(i));
    if (done) done_.insert(l.id);
    g_listening = primary_locked();
    const uint32_t id = id_;
    const uint64_t tok = l.tok;
    const int vfo = cfg_.vfo;
    if (loop) loop->defer([id, tok, vfo] {
        if (!alive(id)) return;
        if (tok) drop_extra(tok);
        else if (!g_listening.load()) set_heard(vfo, false);   // nothing to hear while scanning
    });
}

// none left listening: on with the scan (the bands changed while listening -
// rebuild - : a new turn)
void Scan::after_locks() {
    if (st_ != LISTEN || !locks_.empty()) return;
    std::fill(consec_.begin(), consec_.end(), 0);
    // (ready_ms_ goes on: Dwell is the turn's watching time)
    st_ = need_tune_ || slots_.empty() ? TUNE : WATCH;
    need_tune_ = false;
    status(true);
}

void Scan::post_heard(const Lock& l, bool on) {
    const uint32_t id = id_;
    const uint64_t tok = l.tok;
    const int vfo = cfg_.vfo;
    if (loop) loop->defer([id, tok, vfo, on] {
        if (!alive(id)) return;
        if (!tok) {
            if (g_listening.load()) set_heard(vfo, on);
        } else if (const int v = extra_vfo(tok); v >= 0) {
            set_heard(v, on);
        }
    });
}

// On the uWS loop: the VFO the way entry e says (as sfTune in the page:
// decoder, demod - with its usual bandwidth -, bandwidth, then the frequency
// on slot s's tuner), heard or muted. tok: an extra VFO, made first
void apply_entry(int vfo, int ch, bool indep, int64_t centre, const savedfreq::ScanEntry& e, bool heard);

void Scan::place(const savedfreq::ScanEntry& e, const Slot& s, bool heard, uint64_t tok) {
    const int ch = s.ch;
    const bool indep = cfg_.independent;
    // the receiver takes a wideband VFO's offset from the middle of the spread
    // ((first + last tuner) / 2 - tuner n / 2's centre with an odd count) and
    // puts the VFO on the tuner nearest it
    const int64_t centre = cfg_.wideband ? s.centre + (lay_.offs.front() + lay_.offs.back()) / 2 : s.centre;
    const uint32_t id = id_;
    if (!loop) return;
    loop->defer([id, tok, cfg = cfg_, ch, indep, centre, e, heard] {
        if (!alive(id)) return;
        const int vfo = tok ? make_extra(tok, cfg) : cfg.vfo;
        if (vfo >= 0) apply_entry(vfo, ch, indep, centre, e, heard);
    });
}

void apply_entry(int vfo, int ch, bool indep, int64_t centre, const savedfreq::ScanEntry& e, bool heard) {
    {
        std::string demod, dec;
        int bw = -1;
        for (const auto& d : decimator_manager.getDecimatorInfoList()) {
            if (d.id != vfo) continue;
            demod = DecimatorManager::demodModeToString(d.demod_mode);
            dec = dig::mode_string(static_cast<dig::Mode>(d.digital_mode), d.digital_plugin);
            bw = d.bandwidth_index;
        }
        if (bw < 0) return;   // gone
        std::vector<std::string> cmds;
        const std::string sid = std::to_string(vfo);
        if (!e.dec.empty() && e.dec != dec) {
            cmds.push_back("DIGITAL_MODE:" + sid + ":" + e.dec);
            dec = e.dec;
        }
        int want_bw = -1;
        if (!e.mode.empty() && e.mode != demod) {
            cmds.push_back("SET_DECIMATOR_DEMOD:" + sid + ":" + e.mode);
            if (e.mode == "WBFM") want_bw = 7;                         // 240 kHz (the page's demod buttons)
            else if (e.mode == "NBFM" || e.mode == "AM") want_bw = 20; // 16 kHz
            demod = e.mode;
        }
        if (e.bw > 0) want_bw = bandwidth_index_for(e.bw);
        if (demod == "DIGITAL") {
            // the decoder's minimum (the narrowest option that holds it)
            const double need = decoder_need_hz(dec);
            const int have = want_bw >= 0 ? want_bw : bw;
            if (get_bandwidth_option(have).bandwidth_mhz * 1e6 + 1 < need) {
                int best = 0;
                for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++)
                    if (BANDWIDTH_OPTIONS[i].bandwidth_mhz * 1e6 + 1 >= need &&
                        BANDWIDTH_OPTIONS[i].bandwidth_mhz < BANDWIDTH_OPTIONS[best].bandwidth_mhz)
                        best = i;
                want_bw = best;
            }
        }
        if (want_bw >= 0 && want_bw != bw) cmds.push_back("SET_DECIMATOR_BW:" + sid + ":" + std::to_string(want_bw));
        char c[96];
        const double off_khz = static_cast<double>(e.hz - centre) / 1000.0;
        if (indep) snprintf(c, sizeof c, "SET_DECIMATOR_FREQ:%d:%.3f:%d", vfo, off_khz, ch);
        else snprintf(c, sizeof c, "SET_DECIMATOR_FREQ:%d:%.3f", vfo, off_khz);
        cmds.push_back(c);
        set_heard(vfo, heard);   // first: a VFO heard first moves the audio channel with it (independent)
        dispatch(cmds);
    }
}

void Scan::listen() {
    const int64_t now = now_ms();
    const bool rd = ready();
    // the extra VFOs: made, or not to be had (the VFO limit), or removed by
    // the user - that frequency's turn is over this pass (else a new VFO
    // would be made for it at once)
    for (size_t i = 0; i < locks_.size();) {
        if (locks_[i].tok) {
            const int v = extra_vfo(locks_[i].tok);
            if (v <= -2) {
                if (v == -2) std::cout << "Saved-frequency scan: D" << locks_[i].vfo << " removed by hand" << std::endl;
                end_lock(i, v == -2);
                continue;
            }
            locks_[i].vfo = v;
        }
        i++;
    }
    if (!g_multi.load())   // switched off while listening: the extra VFOs go
        for (size_t i = locks_.size(); i-- > 0;)
            if (locks_[i].tok) end_lock(i, false);

    // each tuner's spectrum once; a locked entry is measured on the tuner it is on
    Specs sp;
    auto level_of = [&](const Lock& l, float* db) {
        if (l.slot < 0 || l.slot >= static_cast<int>(slots_.size()) || l.entry < 0) return false;
        *db = level(sp, slots_[l.slot], plan_.entries[l.entry]);
        return *db > -900.0f;
    };
    for (auto& l : locks_) {
        if (l.cal_wait) {
            if (!rd) l.cal_seen = true;   // the calibration runs
            if ((l.cal_seen && rd) || (!l.cal_seen && now - l.cal_wait_t >= CAL_START_WAIT_MS)) l.cal_wait = false;
        }
        const bool clean = rd && !l.cal_wait;
        if (clean) {
            l.listened_ms += TICK_MS;
            float db;
            if (level_of(l, &db)) {
                l.db = db;
                if (db >= g_squelch.load() - HYST_DB) l.t_active = now;
            }
        } else {
            l.t_active = now;   // calibrating / settling / calibration pending: not "gone quiet"
        }
        // muted while heimdall calibrates (its noise source would be heard) or
        // a calibration is pending, heard as soon as the data is clean
        if (clean != l.heard) {
            l.heard = clean;
            post_heard(l, clean);
        }
    }

    if (g_skip.exchange(false)) return resume(true);
    if (g_hold.load()) {
        for (auto& l : locks_) l.t_active = now;
    } else {
        const int max = g_max.load(), hang = g_hang.load();
        for (size_t i = locks_.size(); i-- > 0;) {
            if (max > 0 && locks_[i].listened_ms >= max) {
                std::cout << "Saved-frequency scan: " << plan_.entries[locks_[i].entry].name << " - longest stay reached"
                          << std::endl;
                end_lock(i, true);
            } else if (now - locks_[i].t_active >= hang) {
                end_lock(i, false);
            }
        }
    }

    // one VFO per active frequency: the others that become active in the bands
    // watched get theirs (not those that had their turn this pass)
    if (g_multi.load() && rd && !locks_.empty()) {
        measure();
        act_.clear();
        std::vector<std::pair<int, int>> fresh;   // (entry, slot)
        for (int k = 0; k < static_cast<int>(slots_.size()); k++) {
            const Slot& s = slots_[k];
            if (s.band < 0 || s.failed) continue;
            for (int i : plan_.bands[s.band].entries) {
                if (consec_[i] < CONFIRM_TICKS) continue;
                if (act_.size() < MAX_ACT) act_.push_back(plan_.entries[i].id);
                if (!done_.count(plan_.entries[i].id) && !locked(i)) fresh.push_back({i, k});
            }
        }
        for (const auto& [i, k] : fresh) {
            if (!can_lock()) break;
            lock(i, k);
        }
    }
    after_locks();
}

// every one listened to has had its turn (done) or is left: on with the scan
void Scan::resume(bool done) {
    while (!locks_.empty()) end_lock(locks_.size() - 1, done);
    after_locks();
}

void Scan::status(bool now_flag) {
    const int64_t now = now_ms();
    if (!now_flag && now - last_status_ < 500) return;
    last_status_ = now;
    std::ostringstream o;
    const char* st = st_ == LISTEN ? "listen" : st_ == WATCH ? "watch" : "tune";
    o << "{\"sfscan\":{\"run\":true,\"state\":\"" << st << "\",\"hold\":" << (g_hold.load() ? "true" : "false")
      << ",\"vfo\":" << cfg_.vfo << ",\"mode\":\""
      << (cfg_.independent ? "independent" : cfg_.wideband ? "wideband" : "coherent") << "\""
      << ",\"n\":" << plan_.entries.size() << ",\"nb\":" << plan_.bands.size() << ",\"cycle\":" << cycle_
      << ",\"ready\":" << (st_ == WATCH && now < hold_until_ ? "false" : "true") << ",\"watch\":[";
    bool first = true;
    for (const auto& s : slots_) {
        if (s.band < 0 || s.band >= static_cast<int>(plan_.bands.size())) continue;
        const Band& b = plan_.bands[s.band];
        o << (first ? "" : ",") << "{\"b\":" << s.band << ",\"ch\":" << s.ch << ",\"hz\":" << s.centre
          << ",\"lo\":" << plan_.entries[b.entries.front()].hz << ",\"hi\":" << plan_.entries[b.entries.back()].hz
          << ",\"n\":" << b.entries.size() << ",\"ok\":" << (s.failed ? "false" : "true") << "}";
        first = false;
    }
    // cur = the first one listened to (pages from before one VFO per active
    // frequency read only that), locks = every one, with its VFO (-1 = being made)
    auto lock_json = [&](const Lock& l) {
        const auto& e = plan_.entries[l.entry];
        char db[32];
        snprintf(db, sizeof db, "%.1f", l.db > -900 ? l.db : 0.0f);
        std::ostringstream j;
        j << "{\"id\":\"" << json_escape(e.id) << "\",\"name\":\"" << json_escape(e.name) << "\",\"hz\":" << e.hz
          << ",\"db\":" << db << ",\"ms\":" << l.listened_ms << ",\"quiet\":" << (now - l.t_active)
          << ",\"heard\":" << (l.heard ? "true" : "false") << ",\"vfo\":" << l.vfo << ",\"extra\":"
          << (l.tok ? "true" : "false") << "}";
        return j.str();
    };
    std::vector<const Lock*> ls;
    if (st_ == LISTEN)
        for (const auto& l : locks_)
            if (l.entry >= 0 && l.entry < static_cast<int>(plan_.entries.size())) ls.push_back(&l);
    o << "],\"cur\":" << (ls.empty() ? std::string("null") : lock_json(*ls.front())) << ",\"locks\":[";
    for (size_t i = 0; i < ls.size(); i++) o << (i ? "," : "") << lock_json(*ls[i]);
    o << "]";
    o << ",\"act\":[";
    for (size_t i = 0; i < act_.size(); i++) o << (i ? "," : "") << "\"" << json_escape(act_[i]) << "\"";
    o << "],\"skipped\":[";
    for (size_t i = 0; i < plan_.skipped.size() && i < MAX_SKIPPED; i++) {
        const auto& s = plan_.skipped[i];
        o << (i ? "," : "") << "{\"id\":\"" << json_escape(s.id) << "\",\"name\":\"" << json_escape(s.name)
          << "\",\"why\":\"" << json_escape(s.why) << "\"}";
    }
    o << "],\"nskipped\":" << plan_.skipped.size() << "," << settings_json() << "}}";
    std::string s = o.str();
    {
        std::lock_guard<std::mutex> lk(g_status_mu);
        g_status = s;
    }
    if (s != last_sent_) {
        last_sent_ = s;
        broadcast(s);
    }
}

}  // namespace

// ============================================================================
// Control (uWS loop)
// ============================================================================

std::string start(int vfo) {
    std::lock_guard<std::mutex> lk(g_life);
    if (g_run.load()) return "the scan is already running";
    if (playback::active()) return "a playback is running - the receiver can't be tuned";
    const int mode = operating_mode_index();   // 0 coherent, 1 wideband scan, 2 independent
    if (vfo < 0 || !decimator_manager.getDecimator(vfo)) {
        vfo = -1;
        for (const auto& d : decimator_manager.getDecimatorInfoList()) { vfo = d.id; break; }
        if (vfo < 0) return "add a VFO first - the scan tunes one";
    }
    if (const std::string p = vfo_held_by(vfo); !p.empty())
        return "D" + std::to_string(vfo) + " is held by its decoder (" + p + ") - select another VFO";
    if (mode == 2 && free_tuners(vfo).empty()) return "every tuner has another VFO on it - free one for the scan";
    const Plan plan = make_plan(vfo, layout_for(mode == 1));
    if (plan.entries.empty()) {
        if (plan.skipped.empty()) return "nothing to scan - tick the frequencies or categories to scan first";
        return "none of the ticked frequencies can be scanned (" + plan.skipped[0].name + ": " + plan.skipped[0].why + ")";
    }
    if (g_thread.joinable()) g_thread.join();

    auto heard = decimator_manager.audioVfoIds();
    g_cfg = Cfg{};
    g_cfg.vfo = vfo;
    g_cfg.independent = mode == 2;
    g_cfg.wideband = mode == 1;
    g_cfg.was_heard = std::find(heard.begin(), heard.end(), vfo) != heard.end();
    for (const auto& d : decimator_manager.getDecimatorInfoList()) {
        if (d.id != vfo) continue;
        g_cfg.demod = DecimatorManager::demodModeToString(d.demod_mode);
        g_cfg.dec = dig::mode_string(static_cast<dig::Mode>(d.digital_mode), d.digital_plugin);
        g_cfg.bw = d.bandwidth_index;
        g_cfg.sq_en = d.squelch_enabled;
        g_cfg.sq_method = d.squelch_method;
        g_cfg.sq_level = d.squelch_level;
        g_cfg.transcribe = d.transcribe;
    }
    {
        std::lock_guard<std::mutex> xl(g_xmu);
        g_extras.clear();
    }
    g_hold = false;
    g_skip = false;
    g_stop_asked = false;
    g_listening = false;
    g_tuner_mask = 0;
    g_msg.clear();
    {
        std::lock_guard<std::mutex> sl(g_status_mu);
        g_status.clear();
    }
    const uint32_t id = g_run_id.fetch_add(1) + 1;
    g_run = true;
    set_heard(vfo, false);   // silent until something is found
    g_thread = std::thread([cfg = g_cfg, id] {
        Scan s(cfg, id);
        s.run();
    });
    return "";
}

bool stop(const std::string& why) {
    std::lock_guard<std::mutex> lk(g_life);
    const bool was = g_run.exchange(false);
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
    if (!was) return false;
    g_tuner_mask = 0;
    // the VFOs it made (one per active frequency) go - before the tuning it
    // leaves is saved: they are never part of the user's setup
    std::map<uint64_t, Extra> xs;
    {
        std::lock_guard<std::mutex> xl(g_xmu);
        xs.swap(g_extras);
    }
    bool removed = false;
    for (const auto& kv : xs) removed = remove_extra(kv.second) || removed;
    if (removed) broadcast(MessageBuilders::build_decimator_info_message());
    // heimdall's calibration hold: released - a pending calibration runs now,
    // on the frequency the scan stopped on
    if (!g_cfg.independent && !g_cfg.wideband) hold_calibration(0);
    // its VFO: heard if it was listening, else as before the scan
    set_heard(g_cfg.vfo, g_listening.load() || g_cfg.was_heard);
    g_listening = false;
    g_hold = false;
    g_msg = why;
    std::cout << "Saved-frequency scan: stopped" << (why.empty() ? "" : " - " + why) << std::endl;
    broadcast(idle_status());
    if (g_on_stop) g_on_stop();
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_life);
    g_run = false;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

bool running() { return g_run.load(); }
bool dispatching() { return g_dispatching; }
bool uses_tuner(int ch) { return ch >= 0 && ch < 32 && (g_tuner_mask.load() >> ch & 1u); }
void skip() { if (g_run.load()) g_skip = true; }
void hold(bool on) { g_hold = on; }

std::string set(const std::string& key, double v, std::string* applied) {
    if (!std::isfinite(v)) return "not a number";
    char c[64];
    if (key == "squelch") {
        const float db = static_cast<float>(std::clamp(std::round(v * 2) / 2, 3.0, 40.0));
        g_squelch = db;
        snprintf(c, sizeof c, "SFSCAN_SQUELCH:%.1f", db);
    } else if (key == "dwell") {
        g_dwell = static_cast<int>(std::clamp(std::llround(v), 500LL, 60000LL));
        snprintf(c, sizeof c, "SFSCAN_DWELL:%d", g_dwell.load());
    } else if (key == "hang") {
        g_hang = static_cast<int>(std::clamp(std::llround(v), 0LL, 60000LL));
        snprintf(c, sizeof c, "SFSCAN_HANG:%d", g_hang.load());
    } else if (key == "max") {
        g_max = static_cast<int>(std::clamp(std::llround(v), 0LL, 3600000LL));
        snprintf(c, sizeof c, "SFSCAN_MAX:%d", g_max.load());
    } else if (key == "multi") {
        g_multi = v != 0;
        snprintf(c, sizeof c, "SFSCAN_MULTI:%d", g_multi.load() ? 1 : 0);
    } else {
        return "unknown setting";
    }
    if (applied) *applied = c;
    return "";
}

std::string status_json() {
    if (g_run.load()) {
        std::lock_guard<std::mutex> lk(g_status_mu);
        if (!g_status.empty()) {
            // the settings may have changed since the thread built it
            std::string s = g_status;
            const size_t at = s.rfind(",\"sq\":");
            if (at != std::string::npos) s = s.substr(0, at) + "," + settings_json() + "}}";
            return s;
        }
        return "{\"sfscan\":{\"run\":true,\"state\":\"tune\",\"vfo\":" + std::to_string(g_cfg.vfo) + "," +
               settings_json() + "}}";
    }
    return idle_status();
}

void set_on_stop(std::function<void()> fn) { g_on_stop = std::move(fn); }

}  // namespace sfscan
