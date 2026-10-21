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
#include "networking/websocket_server.hpp"
#include "playback.hpp"
#include "saved_freqs.hpp"
#include "scanner_manager.hpp"
#include "sf_scan_plan.hpp"
#include "signal_processing/fft_processor.hpp"
#include "utils/json_escape.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

extern std::atomic<float> current_edge_clip;   // main.cpp

namespace sfscan {
namespace {

constexpr int TICK_MS = 100;
constexpr int64_t SETTLE_MS = 250;            // the retuned frequency in the stream -> the band is measured
constexpr int64_t HEADER_TIMEOUT_MS = 6000;   // a retune not seen in the stream by then: that band is left out
constexpr int64_t CAL_GIVE_UP_MS = 20000;     // phase state "calibrating" without the noise source: ignored after
constexpr float HYST_DB = 3.0f;               // a frequency stays active down to squelch - this
constexpr int CONFIRM_TICKS = 2;              // over the squelch on this many polls in a row = active
constexpr size_t MAX_ACT = 50, MAX_SKIPPED = 50;

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
    bool was_heard = false;
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

Plan make_plan(int vfo) {
    uint64_t lo, hi;
    tunable_range(&lo, &hi);
    return build_plan(savedfreq::scan_entries(), span_hz(), lo, hi, vfo_bandwidth_hz(vfo), decoder_why_not);
}

// Independent mode: the tuners no other VFO is on (lowest first)
std::vector<int> free_tuners(int vfo) {
    const int n = std::min(active_num_elements.load(), MAX_CHANNELS);
    uint32_t busy = 0;
    for (const auto& d : decimator_manager.getAllDecimators()) {
        if (!d || d->id == vfo || d->being_deleted.load()) continue;
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

std::string settings_json() {
    std::ostringstream o;
    o << "\"sq\":" << g_squelch.load() << ",\"dwell\":" << g_dwell.load() << ",\"hang\":" << g_hang.load()
      << ",\"max\":" << g_max.load();
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

    void tick();
    void rebuild();
    void tune();
    void settle();
    bool ready();
    void measure();
    void watch();
    void listen();
    void lock(int entry, int slot);
    void place(const savedfreq::ScanEntry& e, const Slot& s, bool heard);
    void resume(bool done);
    void next_turn();
    void status(bool now);
    int64_t stream_hz(const Slot& s) const;
    bool fft(int ch, std::vector<float>* spec, float* floor_db) const;

    Cfg cfg_;
    uint32_t id_;
    Plan plan_;
    uint64_t plan_gen_ = ~0ull;
    float plan_clip_ = -1;
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
    int cur_ = -1, cur_slot_ = -1; // listening: entry, slot
    std::string cur_id_;
    int64_t t_active_ = 0;         // listening: last time it was over the squelch (or not measurable)
    int64_t listened_ms_ = 0;      // ... clean time on it so far (Longest stay)
    bool heard_ = false;           // ... the VFO heard now (muted while heimdall calibrates)
    float cur_db_ = -999;
    std::vector<std::string> act_;
    int64_t last_status_ = 0;
    std::string last_sent_;
};

void Scan::run() {
    std::cout << "Saved-frequency scan: started on D" << cfg_.vfo << " ("
              << (cfg_.independent ? "independent" : "coherent") << " mode)" << std::endl;
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
    if (operating_mode_index() != (cfg_.independent ? 2 : 0)) return request_stop(id_, "the operating mode changed");
    if (!decimator_manager.getDecimator(cfg_.vfo)) return request_stop(id_, "its VFO D" + std::to_string(cfg_.vfo) + " was removed");

    if (savedfreq::generation() != plan_gen_ || current_edge_clip.load() != plan_clip_) rebuild();
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
    status(false);
}

void Scan::rebuild() {
    plan_gen_ = savedfreq::generation();
    plan_clip_ = current_edge_clip.load();
    plan_ = make_plan(cfg_.vfo);
    level_.assign(plan_.entries.size(), -999.0f);
    consec_.assign(plan_.entries.size(), 0);
    std::cout << "Saved-frequency scan: " << plan_.entries.size() << " frequenc"
              << (plan_.entries.size() == 1 ? "y" : "ies") << " in " << plan_.bands.size() << " band"
              << (plan_.bands.size() == 1 ? "" : "s");
    if (!plan_.skipped.empty()) std::cout << " (" << plan_.skipped.size() << " left out)";
    std::cout << std::endl;
    // the one listened to: still in the list?
    if (st_ == LISTEN) {
        cur_ = -1;
        for (size_t i = 0; i < plan_.entries.size(); i++)
            if (plan_.entries[i].id == cur_id_) cur_ = static_cast<int>(i);
        if (cur_ < 0) resume(false);   // no longer scanned
    }
    if (cycle_ >= static_cast<int>(plan_.bands.size())) cycle_ = 0;
    // a new turn (the slots' bands are plan indices); one that needs no
    // retune costs nothing (tune() keeps a centre that already holds the band).
    // Listening, the slots keep their tuning (the one listened to is measured
    // on its slot) and get their new bands when it resumes
    for (auto& s : slots_) s.band = -1;
    if (st_ == LISTEN) need_tune_ = true;
    else st_ = TUNE;
}

void Scan::tune() {
    std::vector<int> chs;
    if (cfg_.independent) {
        chs = free_tuners(cfg_.vfo);
        if (chs.empty()) return request_stop(id_, "every tuner has another VFO on it - free one for the scan");
    } else {
        chs = {std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1)};
    }
    const int nb = static_cast<int>(plan_.bands.size());
    const int ns = static_cast<int>(chs.size());
    if (nb <= ns) cycle_ = 0;
    const double span = span_hz();
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
                                                : static_cast<int64_t>(coherent_tuner_hz());
        // a centre that already holds the band stays (a coherent retune recalibrates)
        const bool move = !band_fits(plan_, s.band, now_hz, span, vfo_hz);
        s.centre = move ? b.centre_hz : now_hz;
        s.retuned = move;
        s.seen = !move;
        char c[96];
        if (move) {
            if (cfg_.independent) snprintf(c, sizeof c, "TUNER_FREQ:%d:%.6f", s.ch, b.centre_hz / 1e6);
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
    // place (filters settled, decoder started) when the data is clean
    for (const auto& s : slots_) {
        if (s.band < 0 || (!s.retuned && !first_tune_)) continue;
        const auto& ents = plan_.bands[s.band].entries;
        int pick = ents.front();
        for (int i : ents)
            if (!done_.count(plan_.entries[i].id)) { pick = i; break; }
        place(plan_.entries[pick], s, false);
        break;
    }
    first_tune_ = false;
    t_cmd_ = now_ms();
    st_ = SETTLE;
}

int64_t Scan::stream_hz(const Slot& s) const {
    return static_cast<int64_t>(tuner_frequencies[cfg_.independent ? s.ch : 0].load(std::memory_order_relaxed));
}

void Scan::settle() {
    const int64_t now = now_ms();
    bool all = true, any_retuned = false;
    int64_t last_seen = 0;
    for (auto& s : slots_) {
        if (s.band < 0 || !s.retuned) continue;
        any_retuned = true;
        if (!s.seen && !s.failed) {
            if (std::llabs(stream_hz(s) - s.centre) < 2000) {
                s.seen = true;
                s.seen_ms = now;
            } else if (now - t_cmd_ > HEADER_TIMEOUT_MS) {
                s.failed = true;
                std::cerr << "Saved-frequency scan: tuner " << s.ch << " didn't reach " << s.centre / 1e6
                          << " MHz - that band is left out this turn" << std::endl;
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
    if (!cfg_.independent) {
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

// every watched entry's level; consec_ counts the polls over the squelch
void Scan::measure() {
    const float sq = g_squelch.load();
    std::vector<float> spec;
    float floor_db = 0;
    for (const auto& s : slots_) {
        if (s.band < 0 || s.failed) continue;
        const int ch = cfg_.independent ? s.ch : std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1);
        if (!fft(ch, &spec, &floor_db)) continue;
        for (int i : plan_.bands[s.band].entries) {
            const auto& e = plan_.entries[i];
            const float lv = entry_level(spec, floor_db, static_cast<double>(s.centre), SAMPLE_RATE, e.hz,
                                         channel_width_hz(e));
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
        // frequencies take turns
        std::sort(act.begin(), act.end());
        int after = -1;
        for (size_t i = 0; i < plan_.entries.size(); i++)
            if (plan_.entries[i].id == last_heard_) after = static_cast<int>(i);
        auto it = std::find_if(act.begin(), act.end(), [after](const auto& a) { return a.first > after; });
        if (it == act.end()) it = act.begin();
        return lock(it->first, it->second);
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

void Scan::lock(int entry, int slot) {
    const auto e = plan_.entries[entry];
    const Slot& s = slots_[slot];
    cur_ = entry;
    cur_slot_ = slot;
    cur_id_ = e.id;
    last_heard_ = e.id;
    t_active_ = now_ms();
    listened_ms_ = 0;
    heard_ = true;
    cur_db_ = level_[entry];
    act_.clear();
    g_listening = true;
    g_skip = false;
    st_ = LISTEN;
    std::cout << "Saved-frequency scan: " << e.name << " " << e.hz / 1e6 << " MHz active (" << cur_db_
              << " dB) -> D" << cfg_.vfo << std::endl;

    place(e, s, true);
    status(true);
}

// On the uWS loop: the VFO the way entry e says (as sfTune in the page:
// decoder, demod - with its usual bandwidth -, bandwidth, then the frequency
// on slot s's tuner), heard or muted
void Scan::place(const savedfreq::ScanEntry& e, const Slot& s, bool heard) {
    const int vfo = cfg_.vfo, ch = s.ch;
    const bool indep = cfg_.independent;
    const int64_t centre = s.centre;
    const uint32_t id = id_;
    if (!loop) return;
    loop->defer([id, vfo, ch, indep, centre, e, heard] {
        if (!alive(id)) return;
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
    });
}

void Scan::listen() {
    const int64_t now = now_ms();
    if (cur_ < 0 || cur_slot_ < 0 || cur_slot_ >= static_cast<int>(slots_.size())) return resume(false);
    const bool clean = ready();
    if (clean) {
        listened_ms_ += TICK_MS;
        // only the one listened to, on the tuner it is on
        const Slot& s = slots_[cur_slot_];
        const int ch = cfg_.independent ? s.ch : std::clamp(active_channel.load(), 0, MAX_CHANNELS - 1);
        std::vector<float> spec;
        float floor_db = 0;
        if (fft(ch, &spec, &floor_db)) {
            const auto& e = plan_.entries[cur_];
            cur_db_ = entry_level(spec, floor_db, static_cast<double>(s.centre), SAMPLE_RATE, e.hz, channel_width_hz(e));
            if (cur_db_ >= g_squelch.load() - HYST_DB) t_active_ = now;
        }
    } else {
        t_active_ = now;   // calibrating / settling: not "gone quiet"
    }
    // muted while heimdall calibrates (its noise source would be heard),
    // heard again as soon as the data is clean
    if (clean != heard_) {
        heard_ = clean;
        const int vfo = cfg_.vfo;
        const uint32_t id = id_;
        if (loop) loop->defer([id, vfo, clean] {
            if (alive(id) && g_listening.load()) set_heard(vfo, clean);
        });
    }
    if (g_skip.exchange(false)) return resume(true);
    if (g_hold.load()) {
        t_active_ = now;
        return;
    }
    const int max = g_max.load();
    if (max > 0 && listened_ms_ >= max) {
        std::cout << "Saved-frequency scan: " << plan_.entries[cur_].name << " - longest stay reached" << std::endl;
        return resume(true);
    }
    if (now - t_active_ >= g_hang.load()) resume(false);
}

void Scan::resume(bool done) {
    if (done && !cur_id_.empty()) done_.insert(cur_id_);   // its turn this pass is over
    cur_ = cur_slot_ = -1;
    cur_id_.clear();
    g_listening = false;
    heard_ = false;
    // nothing to hear while scanning
    const int vfo = cfg_.vfo;
    const uint32_t id = id_;
    if (loop) loop->defer([id, vfo] {
        if (alive(id) && !g_listening.load()) set_heard(vfo, false);
    });
    std::fill(consec_.begin(), consec_.end(), 0);
    // (ready_ms_ goes on: Dwell is the turn's watching time)
    // the bands changed while listening (rebuild): a new turn
    st_ = need_tune_ || slots_.empty() ? TUNE : WATCH;
    need_tune_ = false;
    status(true);
}

void Scan::status(bool now_flag) {
    const int64_t now = now_ms();
    if (!now_flag && now - last_status_ < 500) return;
    last_status_ = now;
    std::ostringstream o;
    const char* st = st_ == LISTEN ? "listen" : st_ == WATCH ? "watch" : "tune";
    o << "{\"sfscan\":{\"run\":true,\"state\":\"" << st << "\",\"hold\":" << (g_hold.load() ? "true" : "false")
      << ",\"vfo\":" << cfg_.vfo << ",\"mode\":\"" << (cfg_.independent ? "independent" : "coherent") << "\""
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
    o << "],\"cur\":";
    if (st_ == LISTEN && cur_ >= 0) {
        const auto& e = plan_.entries[cur_];
        char db[32];
        snprintf(db, sizeof db, "%.1f", cur_db_ > -900 ? cur_db_ : 0.0f);
        o << "{\"id\":\"" << json_escape(e.id) << "\",\"name\":\"" << json_escape(e.name) << "\",\"hz\":" << e.hz
          << ",\"db\":" << db << ",\"ms\":" << listened_ms_ << ",\"quiet\":" << (now - t_active_)
          << ",\"heard\":" << (heard_ ? "true" : "false") << "}";
    } else {
        o << "null";
    }
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
    const int mode = operating_mode_index();
    if (mode == 1) return "the saved-frequency scan runs in coherent or independent mode (wideband mode has the Discrete Scanner)";
    if (vfo < 0 || !decimator_manager.getDecimator(vfo)) {
        vfo = -1;
        for (const auto& d : decimator_manager.getDecimatorInfoList()) { vfo = d.id; break; }
        if (vfo < 0) return "add a VFO first - the scan tunes one";
    }
    if (const std::string p = vfo_held_by(vfo); !p.empty())
        return "D" + std::to_string(vfo) + " is held by its decoder (" + p + ") - select another VFO";
    if (mode == 2 && free_tuners(vfo).empty()) return "every tuner has another VFO on it - free one for the scan";
    const Plan plan = make_plan(vfo);
    if (plan.entries.empty()) {
        if (plan.skipped.empty()) return "nothing to scan - tick the frequencies or categories to scan first";
        return "none of the ticked frequencies can be scanned (" + plan.skipped[0].name + ": " + plan.skipped[0].why + ")";
    }
    if (g_thread.joinable()) g_thread.join();

    auto heard = decimator_manager.audioVfoIds();
    g_cfg = Cfg{vfo, mode == 2, std::find(heard.begin(), heard.end(), vfo) != heard.end()};
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
