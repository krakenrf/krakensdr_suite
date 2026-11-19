#include "digital/digital_decoder.hpp"
#include "digital/dig_plugin.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <limits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#include <pthread.h>

#include <liquid/liquid.h>

// the wire's shared rules (kp::wire::clean_text, host_option): by path, so
// every build line of this file (tools/digi_test's too) finds it
#include "../../plugins/sdk/kraken_plugin.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

namespace dig {

namespace {
constexpr float AFC_LIMIT_HZ = 4000.0f;
constexpr int64_t DETECT_WINDOW_MS = 4000;
constexpr size_t QUEUE_MAX_SECONDS = 1;
constexpr size_t PLUGIN_CHUNK = 16384;          // samples per SAMPLES message
constexpr int64_t PLUGIN_RETRY_MS = 5000;       // missing / unbuilt plugin: look again
constexpr int64_t CHECK_MS = 2000;              // rebuilt plugins / changed plugin list
// AFC: the plugins report the carrier offset they see a few blocks late
// (pipe latency; up to ~1 s when one falls behind), so corrections are
// averaged and applied at most every AFC_PERIOD_MS, and reports made before
// the previous correction reached the plugin (AFC_SETTLE_MS) are ignored -
// an every-report integrator ran away with the delay
constexpr int64_t AFC_PERIOD_MS = 300;
constexpr int64_t AFC_SETTLE_MS = 300;

// plugins/sdk/kraken_plugin.hpp kp::wire
constexpr uint32_t W_SAMPLES = 1, W_OPTION = 2, W_RESET = 3, W_VOICE_WANTED = 4, W_SYNC = 5, W_STATION = 6, W_RF = 7,
                   W_FACT = 16, W_EVENT = 17, W_VALID = 18, W_AUDIO = 19, W_VOICE_STATE = 20, W_FREQ_ERROR = 21,
                   W_SYNC_DONE = 22, W_MAP_POINT = 23, W_MAP_REMOVE = 24, W_TABLE_COLUMNS = 25, W_TABLE_ROW = 26,
                   W_TABLE_REMOVE = 27, W_MESSAGE = 28, W_RAW = 29, W_TALKER = 30, W_TALKER_END = 31,
                   W_TABLE_KEEP = 32, W_TALKER_FORGET = 33, W_AUDIO_CHANNEL = 34, W_SAMPLES_MC = 8,
                   W_SAMPLES_ARRAY = 9, W_SPATIAL = 35, W_PLOT = 36, W_TUNE = 37, W_TABLE_DETAIL = 38,
                   W_SPATIAL_REMOVE = 39, W_VIDEO = 40, W_PLOT_EX = 41, W_OPTION_CHOICES = 42, W_TRUNK_SYSTEM = 43,
                   W_TRUNK_CHANNEL = 44, W_CONTROL_CHANNEL = 45, W_GRANT = 46, W_GRANT_END = 47, W_CALL = 48;
// kp::wire::TRUNK_* limits (trunked systems; a plugin is untrusted input:
// everything is checked again here)
constexpr size_t TRUNK_KEY_MAX = 48, TRUNK_TEXT_MAX = 64, TRUNK_NAME_MAX = 32, TRUNK_ID_MAX = 32, TRUNK_DEMOD_MAX = 16,
                 TRUNK_OPTIONS_MAX = 16, TRUNK_OPTION_VALUE_MAX = 100;
constexpr int TRUNK_SLOT_MAX = 15;
constexpr double TRUNK_FREQ_MAX_HZ = 6e9, TRUNK_BW_MAX_HZ = 2.4e6;
constexpr size_t GRANT_FIXED_KNOWN = 12;   // kp::wire::GRANT's fixed fields this host reads
// kp::Host::option_choices limits (kp::wire::CHOICES_MAX ...)
constexpr size_t CHOICES_MAX = 100, CHOICE_VALUE_MAX = 100, CHOICE_LABEL_MAX = 80;
// named plots (kp::Plot::id) one plugin may keep
constexpr size_t MAX_PLOTS_PER_PLUGIN = 8;
// a plugin's retune requests (kp::Host::tune): at most one per this
constexpr int64_t TUNE_MIN_GAP_MS = 2000;
constexpr size_t MAX_PAYLOAD = 1u << 20;   // kp::wire::MAX_PAYLOAD
// how far back a plugin's sample positions stay mappable to the VFO stream
constexpr double POS_MAP_SECONDS = 60;
// a talker packet (kp::Talker::packet) longer than this is not one
constexpr double MAX_PACKET_S = 0.25;   // ADS-B 120 us .. AIS 5 slots (133 ms)
constexpr double MAX_SIGNAL_SPAN_S = 3.0;   // a valid frame's samples (Host::valid(start, end)): RS41 0.9 s, POCSAG batch at 512 bit/s 1.1 s, FLEX block 0 with its syncs 0.28 s

std::mutex record_mu;
RecordHandler record_handler;
// "Allow logging to disk" (set_log_allowed): Log All's raw frames and verbose
std::atomic<bool> log_allowed_{false};
std::atomic<bool> video_wanted{false};   // a browser is subscribed to decoder video
void deliver_record(DigitalDecoder* d, const LogRecord& r) {
    RecordHandler h;
    {
        std::lock_guard<std::mutex> lk(record_mu);
        h = record_handler;
    }
    if (h) h(d, r);
}

std::mutex voice_tap_mu;
VoiceTapHandler voice_tap_handler;
void deliver_voice_tap(DigitalDecoder* d, const float* x, size_t n, const std::string& plugin, const std::string& tid,
                       const std::string& tlabel, double rf_hz) {
    VoiceTapHandler h;
    {
        std::lock_guard<std::mutex> lk(voice_tap_mu);
        h = voice_tap_handler;
    }
    if (h) h(d, x, n, plugin, tid, tlabel, rf_hz);
}

std::mutex record_voice_mu;
RecordVoiceHandler record_voice_handler;
void deliver_record_voice(DigitalDecoder* d, const float* x, size_t n) {
    RecordVoiceHandler h;
    {
        std::lock_guard<std::mutex> lk(record_voice_mu);
        h = record_voice_handler;
    }
    if (h) h(d, x, n);
}

std::mutex message_mu;
MessageHandler message_handler;
void deliver_message(DigitalDecoder* d, const std::string& plugin, const std::string& from, const std::string& text,
                     double rf_hz) {
    MessageHandler h;
    {
        std::lock_guard<std::mutex> lk(message_mu);
        h = message_handler;
    }
    if (h) h(d, plugin, from, text, rf_hz);
}

// '\0'-separated fields of a wire message
std::vector<std::string> split0(const char* p, size_t len, size_t max_fields) {
    std::vector<std::string> f;
    size_t s = 0;
    for (size_t i = 0; i <= len && f.size() < max_fields; i++)
        if (i == len || p[i] == '\0') {
            f.emplace_back(p + s, i - s);
            s = i + 1;
        }
    return f;
}

// Auto detect leaves out the plugins that need a VFO more than this many
// times wider than the one it runs on (ADS-B wants the whole 2.4 MHz: run on
// a 12.5 kHz VFO it would only burn CPU on upsampled samples)
constexpr double AUTO_MAX_RATE_SHORTFALL = 2.0;

// Station location for the plugins (set_station_location); gen bumps on a change
std::mutex station_mu;
std::string station_text;          // "lat,lon" or "" (unknown)
std::atomic<uint64_t> station_gen{1};

// of a plugin's exe: a rebuild restarts it. Windows: the build replaces the
// running decoder.exe with MSYS2's mv -f (it moves the busy file aside) - the
// mtime changes as on Linux
int64_t mtime_ms(const std::string& path) { return fs_mtime_ms(path); }
}  // namespace

void set_record_handler(RecordHandler h) {
    std::lock_guard<std::mutex> lk(record_mu);
    record_handler = std::move(h);
}

void set_log_allowed(bool on) { log_allowed_ = on; }
bool log_allowed() { return log_allowed_.load(); }
void set_video_wanted(bool on) { video_wanted = on; }

void set_voice_tap_handler(VoiceTapHandler h) {
    std::lock_guard<std::mutex> lk(voice_tap_mu);
    voice_tap_handler = std::move(h);
}

void set_record_voice_handler(RecordVoiceHandler h) {
    std::lock_guard<std::mutex> lk(record_voice_mu);
    record_voice_handler = std::move(h);
}

void set_message_handler(MessageHandler h) {
    std::lock_guard<std::mutex> lk(message_mu);
    message_handler = std::move(h);
}

void set_station_location(bool valid, double lat, double lon) {
    std::string t;
    // is_finite_value, not std::isfinite: right under fast-math flags too
    if (valid && is_finite_value(lat) && is_finite_value(lon) && std::fabs(lat) <= 90 && std::fabs(lon) <= 180) {
        char b[64];
        snprintf(b, sizeof b, "%.6f,%.6f", lat, lon);
        t = b;
    }
    std::lock_guard<std::mutex> lk(station_mu);
    if (t != station_text) {
        station_text = t;
        station_gen++;
    }
}

// ---------------------------------------------------------------------------
// One plugin process of a decoder
// ---------------------------------------------------------------------------
struct Runner {
    std::string id;
    PluginInfo info;                   // of the running process (or the last lookup)
    bool known = false;                // info is valid
    std::unique_ptr<PluginProcess> proc;
    int64_t retry_ms = 0, started_ms = 0;
    int crashes = 0;
    std::string note;                  // last problem logged (no repeats)
    int verbose_sent = -1, voice_sent = -1, raw_sent = -1, video_sent = -1;
    uint64_t station_sent = 0;         // station_gen sent
    double rf_sent = -1;               // kp::Host::rf_hz sent (0 = unknown, -1 = nothing yet)
    std::map<std::string, std::string> opts_sent;   // key (without the id prefix) -> value
    uint32_t sync_sent = 0, sync_done = 0;
    // trunked-system reports are dropped until sync_done reaches this (the
    // SYNC sent after a RESET: reports still in the pipe from before it)
    uint32_t trunk_fence = 0;
    std::string state = "starting", error;
    std::vector<std::string> tail;
    uint64_t dropped = 0;
    // plugin input sample p0 + i = VFO stream position v0 + i * step (talker
    // spans arrive in the plugin's Host::time(), counted from its start)
    struct PosSeg { uint64_t p0, n; double v0, step; };
    std::deque<PosSeg> pos_map;
    uint64_t written = 0;              // samples this process was sent
    // the open talker of each talker channel (kp::Talker::channel, DMR timeslot)
    struct OpenTalker { std::string id, label; uint64_t start = 0, end = 0; };
    std::map<int, OpenTalker> talkers;
    // the talker channel its audio is from (wire AUDIO_CHANNEL, DMR: the
    // timeslot it plays), -1 = not said - Voice to Text names that radio
    int audio_channel = -1;
    // array plugins: Host::time() = VFO stream position / its rate
    double array_rate = 0;
    // a plugin at the VFO's own rate (kp::Info::sample_rate 0): the rate it
    // was started at (--serve --rate); a new VFO rate restarts it
    double vfo_rate = 0;
    // the rate this process gets its samples at
    double prate() const { return info.sample_rate > 0 ? info.sample_rate : vfo_rate; }
    int64_t last_tune_ms = -1000000;   // kp::Host::tune rate limit
    uint64_t array_next = ~0ull;       // stream position after the last block sent (gap detection)
    // Host::time() seconds -> VFO stream position (false: not mappable)
    bool vfo_pos(double t, double rate, uint64_t* out) const {
        if (info.array) {
            if (!(array_rate > 0) || !is_finite_value(t) || t < 0) return false;
            *out = static_cast<uint64_t>(std::llround(t * array_rate));
            return true;
        }
        if (pos_map.empty() || !is_finite_value(t) || t < 0) return false;
        const double p = t * rate;
        if (p < static_cast<double>(pos_map.front().p0)) return false;
        const PosSeg* g = &pos_map.back();
        for (auto it = pos_map.rbegin(); it != pos_map.rend(); ++it)
            if (static_cast<double>(it->p0) <= p) { g = &*it; break; }
        const double off = std::min(p - static_cast<double>(g->p0), static_cast<double>(g->n));
        *out = static_cast<uint64_t>(std::llround(std::max(0.0, g->v0 + off * g->step)));
        return true;
    }
};

// ---------------------------------------------------------------------------
// Engine: AFC mixer + per-rate resamplers + the plugin processes. Owned and
// driven by one thread at a time (the decoder's worker, or process()).
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine(Report& r, DigitalDecoder* owner, bool sync_io) : report_(r), owner_(owner), sync_io_(sync_io) {}
    ~Engine() {
        stop_all();
        destroy_resamplers();
    }

    void configure(Mode m, const Options& o, const std::string& plugin) {
        opts_ = o;
        if (m == mode_ && (m != Mode::PLUGIN || plugin == plugin_)) return;
        mode_ = m;
        plugin_ = m == Mode::PLUGIN ? plugin : "";
        rebuild_runners();
    }

    // the VFO moved (> 100 Hz): everything starts over
    void check_rf(double rf) {
        if (std::fabs(rf - rf_hz_) > 100.0) {
            if (rf_hz_ != 0.0) {
                reset_all();
                report_.clear_all();
                report_.map_clear();
                report_.table_clear();
                report_.plot_clear();
            }
            rf_hz_ = rf;
        }
    }

    // Array plugins: every antenna's block (x = m x n, antenna-major) as
    // SAMPLES_ARRAY (int16 I/Q with a per-message scale)
    void process_array(const std::complex<float>* x, int m, size_t n, float rate, double rf, uint64_t pos) {
        if (rate <= 0 || n == 0 || m < 1 || m > 8) return;
        check_rf(rf);
        for (size_t i = 0; i < runners_.size(); i++) {
            Runner& r = *runners_[i];
            if (!r.known || !r.info.array || !r.proc) continue;
            r.array_rate = rate;
            send_settings(r);
            const size_t per = (MAX_PAYLOAD - 32) / (4 * static_cast<size_t>(m));
            const size_t backlog = sync_io_ ? SIZE_MAX : static_cast<size_t>(rate) * 4 * static_cast<size_t>(m);   // ~1 s
            for (size_t a = 0; a < n; a += per) {
                const size_t k = std::min(per, n - a);
                float mx = 1e-9f;
                for (int c = 0; c < m; c++)
                    for (size_t j = a; j < a + k; j++) {
                        const std::complex<float> v = x[static_cast<size_t>(c) * n + j];
                        mx = std::max(mx, std::max(std::fabs(v.real()), std::fabs(v.imag())));
                    }
                const float scale = mx / 32000.0f;
                arr_buf_.resize(32 + k * static_cast<size_t>(m) * 4);
                char* p = arr_buf_.data();
                const uint32_t mm = static_cast<uint32_t>(m), nn = static_cast<uint32_t>(k);
                const uint64_t at = pos + a;
                const uint32_t flags = at != r.array_next ? 1u : 0u;
                const double rr = rate, t0 = static_cast<double>(at) / rate;
                memcpy(p, &mm, 4);
                memcpy(p + 4, &nn, 4);
                memcpy(p + 8, &flags, 4);
                memcpy(p + 12, &scale, 4);
                memcpy(p + 16, &rr, 8);
                memcpy(p + 24, &t0, 8);
                int16_t* q = reinterpret_cast<int16_t*>(p + 32);
                const float inv = 1.0f / scale;
                for (size_t j = a; j < a + k; j++)
                    for (int c = 0; c < m; c++) {
                        const std::complex<float> v = x[static_cast<size_t>(c) * n + j];
                        *q++ = static_cast<int16_t>(std::lrint(v.real() * inv));
                        *q++ = static_cast<int16_t>(std::lrint(v.imag() * inv));
                    }
                if (!r.proc->send(W_SAMPLES_ARRAY, arr_buf_.data(), arr_buf_.size(), backlog)) {
                    r.dropped += k;
                    continue;   // the next block is a gap for the plugin
                }
                r.array_next = at + k;
            }
            auto handler = [this, i](uint32_t type, const char* pp, size_t len) { on_message(i, type, pp, len); };
            if (sync_io_) r.proc->flush_blocking(handler, 60000);
            else r.proc->poll(handler);
        }
        housekeeping();
    }

    void process(const std::complex<float>* x, size_t n, float rate, double rf, uint64_t pos,
                 const std::complex<int16_t>* mc = nullptr, int mc_m = 0) {
        if (rate <= 0 || n == 0) return;
        if (pos == DigitalDecoder::NO_POS) pos = next_pos_;
        next_pos_ = pos + n;
        check_rf(rf);
        if (rate != in_rate_) {
            in_rate_ = rate;
            // plugins at the VFO's own rate start over at the new one
            for (auto& r : runners_)
                if (r->proc && r->known && r->info.sample_rate <= 0 && !r->info.array && r->vfo_rate != in_rate_) {
                    r->proc->stop();
                    r->proc.reset();
                    close_talker(*r);
                    trunk_reset_if(*r);
                    r->retry_ms = 0;
                    r->note = "new VFO rate";
                }
            // Auto detect: the plugin set depends on the VFO's rate
            if (mode_ == Mode::AUTO && auto_ids() != runner_ids()) rebuild_runners();
            else rebuild_resamplers();
        }
        const int64_t t = now_ms();
        if (t - check_ms_ > CHECK_MS) {
            check_ms_ = t;
            periodic(t);
        }
        // AFC mixer at the input rate
        mixed_.resize(n);
        const float w = -2.0f * static_cast<float>(M_PI) * afc_hz_ / in_rate_;
        if (w == 0.0f) {
            // no correction (yet, or the plugins report no offset - e.g. ADS-B
            // at 2.4 MHz): a plain copy instead of a sin/cos per sample, and
            // nothing to keep continuous - the mixer's phase goes back to 0.
            // It used to keep the last correction's phase for good, so once
            // any decoder on this VFO had reported a frequency error (D-STAR,
            // P25, ...) every later multichannel plugin (lora, ism) got one
            // antenna only (mc_ok below) until kraken_doa restarted
            nco_phase_ = 0.0;
            std::copy(x, x + n, mixed_.begin());
        } else {
            for (size_t i = 0; i < n; i++) {
                mixed_[i] = x[i] * std::polar(1.0f, static_cast<float>(nco_phase_));
                nco_phase_ += w;
            }
            nco_phase_ = std::remainder(nco_phase_, 2.0 * M_PI);
        }
        resample(n);
        // every antenna (multichannel plugins at the VFO's rate, no AFC
        // correction to apply: what they get is the VFO's samples as they are)
        const bool mc_ok = mc && mc_m >= 2 && w == 0.0f;
        for (size_t i = 0; i < runners_.size(); i++) {
            // array plugins get their samples in process_array(); here they
            // are only started / kept up to date (settings, replies)
            if (runners_[i]->known && runners_[i]->info.array) {
                run(i, nullptr, 0, pos, 1.0);
                continue;
            }
            const Resampled* r = rate_buf(*runners_[i]);
            const bool multi = mc_ok && runners_[i]->info.multichannel && rate_of(*runners_[i]) == in_rate_;
            run(i, r ? r->out.data() : nullptr, r ? r->ny : 0, pos, r ? in_rate_ / r->rate : 1.0,
                multi ? mc : nullptr, multi ? mc_m : 0, n);
        }
        housekeeping();
    }
    // Writes what the plugins' pipes take of what is still queued for them
    // (and reads their replies meanwhile); true while some is left. A burst of
    // an array plugin is ~17 MB, the pipe holds 1 MB: written only when a new
    // block came in, the plugin got ~10 MB/s between bursts (the 100 ms
    // ticks) - less than the 17 MB/s it is sent - and the queue grew
    bool flush_pending() {
        bool more = false;
        for (size_t i = 0; i < runners_.size(); i++) {
            Runner& r = *runners_[i];
            if (!r.proc || !r.proc->pending_writes()) continue;
            r.proc->poll([this, i](uint32_t type, const char* p, size_t len) { on_message(i, type, p, len); });
            more = more || r.proc->pending_writes();
        }
        return more;
    }
    // a running decoder plugin takes every antenna (kp::Info::multichannel)
    bool multichannel() const {
        for (const auto& r : runners_)
            if (r->info.multichannel) return true;
        return false;
    }

    // Everything back to a cold start (VFO retune, decoder switched off and
    // on): the plugins get a RESET, detection starts over
    void reset_all() {
        for (auto& r : runners_) {
            if (r->proc && r->proc->running()) {
                r->proc->send(W_RESET, nullptr, 0, 0);
                // what it reports about a trunked system before it has seen
                // the RESET is stale: dropped until it answers this SYNC
                r->trunk_fence = ++r->sync_sent;
                r->proc->send(W_SYNC, &r->trunk_fence, 4, 0);
            }
            close_talker(*r);   // the plugin forgets its call with the RESET
        }
        trunk_reset();   // ... and its trunked system
        std::lock_guard<std::mutex> lk(st_mu_);
        detected_.clear();
        valid_.clear();
        totals_.clear();
        last_valid_ms_ = 0;
        last_valid_by_.clear();
        afc_hz_ = 0;
        residual_hz_ = 0;
        afc_sum_ = 0;
        afc_n_ = 0;
    }

    // Offline: wait until every plugin answered a SYNC sent after all input
    void drain(int timeout_ms) {
        for (auto& r : runners_)
            if (r->proc && r->proc->running()) {
                uint32_t id = ++r->sync_sent;
                r->proc->send(W_SYNC, &id, 4, 0);
            }
        for (int i = 0; i < timeout_ms; i++) {
            bool done = true;
            for (size_t k = 0; k < runners_.size(); k++) {
                Runner& r = *runners_[k];
                if (!r.proc || !r.proc->running()) continue;
                r.proc->poll([this, k](uint32_t type, const char* p, size_t len) { on_message(k, type, p, len); });
                if (r.sync_done != r.sync_sent) {
                    done = false;
                    if (!r.proc->alive(nullptr)) r.sync_done = r.sync_sent;   // died: nothing more to come
                }
            }
            if (done) break;
            for (auto& r : runners_)
                if (r->proc && r->proc->running() && r->sync_done != r->sync_sent) {
                    r->proc->wait_io(1);
                    break;
                }
        }
        housekeeping();
    }

    struct PluginStatus {
        std::string id, name, version, state, error;
        std::vector<std::string> tail;
        uint64_t dropped = 0;
        double rate = 0, min_vfo_rate = 0;
        bool map = false;
    };
    struct Status {
        std::string detected, detected_name;
        float offset_hz = 0;
        float rate = 0;
        float min_rate = 0;
        std::map<std::string, std::pair<int, uint64_t>> frames;   // id -> (4 s window, total)
        std::vector<PluginStatus> plugins;
    };
    std::string detected() const {
        std::lock_guard<std::mutex> lk(st_mu_);
        return detected_;
    }
    Status status() const {
        std::lock_guard<std::mutex> lk(st_mu_);
        Status s;
        s.detected = detected_;
        s.offset_hz = afc_hz_ + residual_hz_;
        s.rate = in_rate_;
        int64_t t = now_ms();
        for (const auto& kv : totals_) s.frames[kv.first] = {0, kv.second};
        for (const auto& v : valid_)
            if (t - v.first < DETECT_WINDOW_MS) s.frames[v.second].first++;
        for (const auto& p : status_plugins_) {
            s.plugins.push_back(p);
            s.min_rate = std::max<float>(s.min_rate, static_cast<float>(p.min_vfo_rate));
            if (p.id == detected_) s.detected_name = p.name;
        }
        return s;
    }

private:
    Report& report_;
    DigitalDecoder* owner_;
    const bool sync_io_;
    Mode mode_ = Mode::OFF;
    std::string plugin_;
    Options opts_;
    std::vector<std::unique_ptr<Runner>> runners_;
    uint64_t reg_gen_ = 0;
    int64_t check_ms_ = 0;

    float in_rate_ = 0;
    double rf_hz_ = 0;
    uint64_t next_pos_ = 0;            // VFO stream position after the last block
    double nco_phase_ = 0;
    std::vector<std::complex<float>> mixed_;
    std::vector<char> mc_buf_;   // a SAMPLES_MC message
    struct Resampled {
        float rate = 0;
        msresamp_crcf q = nullptr;     // nullptr = same rate as the input
        std::vector<std::complex<float>> out;
        unsigned int ny = 0;
    };
    std::vector<Resampled> rs_;
    std::vector<char> arr_buf_;        // one SAMPLES_ARRAY message

    // detection / AFC / status (status() reads them from another thread)
    mutable std::mutex st_mu_;
    std::deque<std::pair<int64_t, std::string>> valid_;
    std::map<std::string, uint64_t> totals_;
    std::string detected_;
    float afc_hz_ = 0;
    float residual_hz_ = 0;
    float afc_sum_ = 0;
    int afc_n_ = 0;
    int64_t afc_changed_ms_ = 0;
    int64_t last_valid_ms_ = 0;
    std::map<std::string, int64_t> last_valid_by_;   // plugin -> its last valid frame (the Digital squelch)
    std::map<std::string, int64_t> hold_ms_by_;      // plugin -> its squelch hold (kp::Info::squelch_hold_s)
    std::string lead_tid_sent_, lead_tlabel_sent_;   // what set_lead_talker last got (worker thread)
    // the plugin whose trunked-system reports went out since the last trunk
    // RESET ("" = none: nothing to reset)
    std::string trunk_from_;
    std::vector<PluginStatus> status_plugins_;

    // --- runner set ---------------------------------------------------------
    // all plugins get EOF at once and exit in parallel (a decoder in Auto
    // detect runs several; this can run on the uWS thread when a VFO is removed)
    void stop_all() {
        for (auto& r : runners_) {
            if (r->proc) r->proc->request_stop();
            close_talker(*r);
        }
        trunk_reset();
        for (auto& r : runners_)
            if (r->proc) r->proc->stop();
        runners_.clear();
    }

    void rebuild_runners() {
        for (const auto& r : runners_) report_.choices_clear(r->id);   // their processes stop
        stop_all();
        {
            std::lock_guard<std::mutex> lk(st_mu_);
            detected_.clear();
            valid_.clear();
            totals_.clear();
            afc_hz_ = residual_hz_ = 0;
            afc_sum_ = 0;
            afc_n_ = 0;
        }
        auto& reg = PluginRegistry::instance();
        reg_gen_ = reg.generation();
        std::vector<std::string> ids;
        if (mode_ == Mode::PLUGIN) {
            ids.push_back(plugin_);
        } else if (mode_ == Mode::AUTO) {
            ids = auto_ids();
            bool any = false;
            for (const auto& p : reg.list()) any |= p.built && p.auto_detect && !p.manual_only;
#ifdef _WIN32
            // the Windows build of the plugins is build_windows.bat
            if (!any) report_.event("", "Auto detect: no auto-detect decoder plugins are built (run build_windows.bat)", 0.0);
#else
            if (!any) report_.event("", "Auto detect: no auto-detect decoder plugins are built (run make)", 0.0);
#endif
        }
        for (const auto& id : ids) {
            auto r = std::make_unique<Runner>();
            r->id = id;
            r->known = reg.get(id, &r->info);
            report_.set_label(id, r->known ? r->info.name : id);
            runners_.push_back(std::move(r));
        }
        rebuild_resamplers();
        update_array_wanted();
        publish_status();
    }

    // the pipeline offers every antenna while an array plugin runs here (and
    // only ticks the single-channel path when nothing else needs it)
    void update_array_wanted() {
        bool on = false, only = !runners_.empty();
        double burst = 0.25, period = 1.0;
        for (const auto& r : runners_) {
            if (r->known && r->info.array) {
                on = true;
                burst = r->info.array_burst_s;
                period = r->info.array_period_s;
            } else {
                only = false;
            }
        }
        owner_->set_array_wanted(on, burst, period, on && only);
    }

    // The plugins Auto detect runs on this VFO: the ticked ones, without the
    // manual-only ones and those that need a much wider VFO (none until the
    // VFO's rate is known)
    std::vector<std::string> auto_ids() const {
        std::vector<std::string> ids;
        if (in_rate_ <= 0) return ids;
        for (const auto& p : PluginRegistry::instance().list())
            if (p.built && p.auto_detect && !p.manual_only && p.min_vfo_rate <= in_rate_ * AUTO_MAX_RATE_SHORTFALL)
                ids.push_back(p.id);
        return ids;
    }
    std::vector<std::string> runner_ids() const {
        std::vector<std::string> ids;
        for (const auto& r : runners_) ids.push_back(r->id);
        return ids;
    }

    void destroy_resamplers() {
        for (auto& r : rs_)
            if (r.q) msresamp_crcf_destroy(r.q);
        rs_.clear();
    }

    // a plugin's input rate (kp::Info::sample_rate 0: the VFO's own)
    float rate_of(const Runner& rn) const {
        if (!rn.known) return 48000.0f;
        return rn.info.sample_rate > 0 ? static_cast<float>(rn.info.sample_rate) : in_rate_;
    }

    void rebuild_resamplers() {
        destroy_resamplers();
        if (in_rate_ <= 0) return;
        for (auto& rn : runners_) {
            if (rn->known && rn->info.array) continue;   // their samples aren't resampled
            float rate = rate_of(*rn);
            bool have = false;
            for (auto& r : rs_) have |= r.rate == rate;
            if (have) continue;
            Resampled r;
            r.rate = rate;
            if (std::fabs(rate - in_rate_) > 0.5f) r.q = msresamp_crcf_create(rate / in_rate_, 60.0f);
            rs_.push_back(std::move(r));
        }
    }

    void resample(size_t n) {
        for (auto& r : rs_) {
            unsigned int ny = 0;
            r.out.resize(static_cast<size_t>(std::ceil(n * r.rate / in_rate_)) + 64);
            if (r.q) {
                msresamp_crcf_execute(r.q, mixed_.data(), static_cast<unsigned int>(n), r.out.data(), &ny);
            } else {
                std::copy(mixed_.begin(), mixed_.end(), r.out.begin());
                ny = static_cast<unsigned int>(n);
            }
            r.ny = ny;
        }
    }

    const Resampled* rate_buf(const Runner& rn) const {
        float rate = rate_of(rn);
        for (const auto& r : rs_)
            if (r.rate == rate) return &r;
        return nullptr;
    }

    // every CHECK_MS: rebuilt plugins, the plugin list (Auto detect), status
    void periodic(int64_t t) {
        auto& reg = PluginRegistry::instance();
        if (mode_ == Mode::AUTO && reg.generation() != reg_gen_) {
            report_.event("", "Decoder plugins changed - restarting auto detection", 0.0);
            rebuild_runners();
            return;
        }
        for (auto& r : runners_) {
            if (!r->proc) continue;
            int64_t m = mtime_ms(r->info.exe);
            if (m != 0 && m != r->info.mtime) {
                r->proc->stop();
                r->proc.reset();
                close_talker(*r);
                trunk_reset_if(*r);
                r->retry_ms = 0;
                r->note = "rebuilt";
                continue;
            }
            if (t - r->started_ms > 60000) r->crashes = 0;
            r->tail = r->proc->stderr_tail();
        }
        publish_status();
    }

    void publish_status() {
        std::vector<PluginStatus> v;
        for (const auto& r : runners_) {
            PluginStatus p;
            p.id = r->id;
            p.name = r->known ? r->info.name : r->id;
            p.version = r->known ? r->info.version : "";
            p.state = r->state;
            p.error = r->error;
            p.tail = r->tail;
            p.dropped = r->dropped;
            p.rate = r->known ? rate_of(*r) : 0;
            p.min_vfo_rate = r->known ? r->info.min_vfo_rate : 12500;
            p.map = r->known && r->info.map;
            v.push_back(std::move(p));
        }
        std::lock_guard<std::mutex> lk(st_mu_);
        status_plugins_ = std::move(v);
    }

    // --- one plugin ---------------------------------------------------------
    // the plugin whose voice / carrier offset counts: the fixed one, or the
    // one Auto detect locked onto
    bool is_lead(const Runner& r) const { return mode_ == Mode::PLUGIN || r.id == detected_; }

    void problem(Runner& r, const std::string& state, const std::string& err, int64_t retry_ms) {
        r.state = state;
        r.error = err;
        if (err != r.note) report_.event(r.id, err, 0.0);
        r.note = err;
        r.retry_ms = now_ms() + retry_ms;
        publish_status();
    }

    void start(Runner& r, int64_t t) {
        auto& reg = PluginRegistry::instance();
        PluginInfo pi;
        bool found = reg.get(r.id, &pi);
        if (!found || !pi.built || pi.mtime != mtime_ms(pi.exe)) {
            reg.scan();   // new / rebuilt since the last scan
            found = reg.get(r.id, &pi);
        }
        if (!found) return problem(r, "missing", "Plugin '" + r.id + "' is not installed", PLUGIN_RETRY_MS);
        if (!pi.built) return problem(r, "not built", pi.name + ": " + pi.error, PLUGIN_RETRY_MS);
        // at the VFO's own rate: not before it is known
        const bool native = pi.sample_rate <= 0 && !pi.array;
        if (native && !(in_rate_ > 0)) return;
        const bool rebuilt = r.note == "rebuilt";
        const bool rate_changed = !r.known || pi.sample_rate != r.info.sample_rate;
        r.info = pi;
        r.known = true;
        report_.set_label(r.id, pi.name);
        {
            std::lock_guard<std::mutex> lk(st_mu_);
            if (pi.squelch_hold_s > 0) hold_ms_by_[r.id] = static_cast<int64_t>(pi.squelch_hold_s * 1000);
            else hold_ms_by_.erase(r.id);
        }
        if (rate_changed) rebuild_resamplers();
        update_array_wanted();
        r.array_next = ~0ull;
        r.vfo_rate = native ? in_rate_ : 0;
        r.proc = std::make_unique<PluginProcess>();
        std::string err;
        std::vector<std::string> args{pi.exe, "--serve"};
        if (native) {
            char b[32];
            snprintf(b, sizeof b, "%.3f", static_cast<double>(in_rate_));
            args.push_back("--rate");
            args.push_back(b);
        }
        if (!r.proc->start(args, &err)) {
            r.proc.reset();
            return problem(r, "error", pi.name + ": " + err, PLUGIN_RETRY_MS);
        }
        r.started_ms = t;
        close_talker(r);
        report_.choices_clear(r.id);   // a new process: what the old one listed is gone
        r.audio_channel = -1;
        r.written = 0;
        r.pos_map.clear();
        if (pi.messages) deliver_message(owner_, r.id, "", "", 0);   // get the street data ready
        r.verbose_sent = r.voice_sent = r.raw_sent = r.video_sent = -1;
        r.station_sent = 0;
        r.rf_sent = -1;
        r.opts_sent.clear();
        r.sync_sent = r.sync_done = r.trunk_fence = 0;
        r.state = "running";
        r.error.clear();
        // a fixed decoder says it started; in Auto detect only restarts are news
        if (rebuilt) report_.event(r.id, "Rebuilt plugin loaded: " + pi.name + " " + pi.version, 0.0);
        else if (!r.note.empty()) report_.event(r.id, pi.name + " restarted", 0.0);
        else if (mode_ == Mode::PLUGIN) report_.event(r.id, "Plugin started: " + pi.name + " " + pi.version, 0.0);
        r.note.clear();
        publish_status();
    }

    // the VFO is set to Log All and logging to disk is allowed: raw frames
    // and the repeated messages are wanted
    bool log_all() const { return opts_.log == LOG_ALL && log_allowed_.load(); }

    void send_settings(Runner& r) {
        // kp::Host::verbose: "Show all messages", or Log All in effect
        const int verbose = opts_.verbose || log_all() ? 1 : 0;
        if (verbose != r.verbose_sent) {
            std::string kv = std::string("verbose=") + (verbose ? "1" : "0");
            r.proc->send(W_OPTION, kv.data(), kv.size(), 0);
            r.verbose_sent = verbose;
        }
        // the receiver's location (kp::Host::station)
        const uint64_t sg = station_gen.load();
        if (sg != r.station_sent) {
            std::string st;
            {
                std::lock_guard<std::mutex> lk(station_mu);
                st = station_text;
            }
            r.proc->send(W_STATION, st.data(), st.size(), 0);
            r.station_sent = sg;
        }
        // the RF of the plugin's 0 Hz (kp::Host::rf_hz): the VFO's frequency
        // plus the AFC correction (the mixer moves afc_hz_ to 0 Hz). Resent when
        // it moves > 10 Hz, and after a (re)start. Never ",1" (mirrored) since
        // the Invert option went
        if (const double rf0 = rf_hz_ > 0 ? rf_hz_ + afc_hz_ : 0.0; r.rf_sent < 0 || std::fabs(rf0 - r.rf_sent) > 10.0) {
            char b[64] = "";
            if (rf0 > 0) snprintf(b, sizeof b, "%.1f", rf0);
            r.proc->send(W_RF, b, strlen(b), 0);
            r.rf_sent = rf0;
        }
        // raw frames for the decoder data log (Log All)
        const int raw = log_all() ? 1 : 0;
        if (raw != r.raw_sent) {
            std::string kv = std::string("log_raw=") + (raw ? "1" : "0");
            r.proc->send(W_OPTION, kv.data(), kv.size(), 0);
            r.raw_sent = raw;
        }
        // video plugins: is anyone watching? (kp::Host::video_wanted - they
        // skip the render / encode while not)
        if (r.info.video) {
            const int vid = video_wanted.load() ? 1 : 0;
            if (vid != r.video_sent) {
                std::string kv = std::string("video_wanted=") + (vid ? "1" : "0");
                r.proc->send(W_OPTION, kv.data(), kv.size(), 0);
                r.video_sent = vid;
            }
        }
        // only the decoder whose voice is played (transcribed, recorded) runs its vocoder
        const int voice = (is_lead(r) && (owner_->voice_wanted() || owner_->voice_tap() || owner_->record_tap())) ? 1 : 0;
        if (voice != r.voice_sent) {
            uint8_t b = static_cast<uint8_t>(voice);
            r.proc->send(W_VOICE_WANTED, &b, 1, 0);
            r.voice_sent = voice;
        }
        // the plugin's own options ("<id>.<key>"); a removed one goes back
        // to the default (value "")
        const std::string prefix = r.id + ".";
        std::map<std::string, std::string> want;
        for (auto it = opts_.plugin.lower_bound(prefix); it != opts_.plugin.end() && it->first.rfind(prefix, 0) == 0; ++it)
            want[it->first.substr(prefix.size())] = it->second;
        for (const auto& kv : want) {
            auto s = r.opts_sent.find(kv.first);
            if (s != r.opts_sent.end() && s->second == kv.second) continue;
            std::string m = kv.first + "=" + kv.second;
            r.proc->send(W_OPTION, m.data(), m.size(), 0);
            r.opts_sent[kv.first] = kv.second;
        }
        for (auto it = r.opts_sent.begin(); it != r.opts_sent.end();) {
            if (want.count(it->first)) { ++it; continue; }
            std::string m = it->first + "=";
            r.proc->send(W_OPTION, m.data(), m.size(), 0);
            it = r.opts_sent.erase(it);
        }
    }

    // y = this block resampled to the plugin's rate; its sample j is VFO
    // stream position pos + j * step
    void run(size_t idx, const std::complex<float>* y, unsigned int ny, uint64_t pos, double step,
             const std::complex<int16_t>* mc = nullptr, int mc_m = 0, size_t mc_n = 0) {
        Runner& r = *runners_[idx];
        const int64_t t = now_ms();
        if (!r.proc) {
            if (t < r.retry_ms) return;
            start(r, t);
            if (!r.proc) return;
            // start() may have rebuilt the resamplers (new rate): this block
            // is skipped for this plugin
            const Resampled* rb = rate_buf(r);
            if (!rb || rb->out.data() != y) ny = 0;
        }
        send_settings(r);
        if (mc && ny) {
            // every antenna: SAMPLES_MC = uint32 m + n x m complex int16 (the
            // VFO's rate - step 1)
            const size_t backlog = sync_io_ ? SIZE_MAX : static_cast<size_t>(r.prate()) * 4 * static_cast<size_t>(mc_m);
            const size_t n = std::min<size_t>(mc_n, ny);
            for (size_t p = 0; p < n; p += PLUGIN_CHUNK) {
                const size_t k = std::min<size_t>(PLUGIN_CHUNK, n - p);
                mc_buf_.resize(4 + k * static_cast<size_t>(mc_m) * 4);
                const uint32_t m32 = static_cast<uint32_t>(mc_m);
                memcpy(mc_buf_.data(), &m32, 4);
                std::complex<int16_t>* d = reinterpret_cast<std::complex<int16_t>*>(mc_buf_.data() + 4);
                const std::complex<int16_t>* q = mc + p * static_cast<size_t>(mc_m);
                const size_t cnt = k * static_cast<size_t>(mc_m);
                std::copy(q, q + cnt, d);
                if (!r.proc->send(W_SAMPLES_MC, mc_buf_.data(), mc_buf_.size(), backlog)) {
                    r.dropped += k;
                    continue;
                }
                r.pos_map.push_back({r.written, k, static_cast<double>(pos) + static_cast<double>(p), 1.0});
                r.written += k;
            }
            ny = 0;   // (sent)
        }
        const size_t backlog = sync_io_ ? SIZE_MAX : static_cast<size_t>(r.prate()) * 8;   // 1 s
        for (size_t p = 0; p < ny; p += PLUGIN_CHUNK) {
            size_t k = std::min<size_t>(PLUGIN_CHUNK, ny - p);
            if (!r.proc->send(W_SAMPLES, y + p, k * sizeof(std::complex<float>), backlog)) {
                r.dropped += k;   // never reaches the plugin: its clock doesn't advance
                continue;
            }
            r.pos_map.push_back({r.written, k, static_cast<double>(pos) + static_cast<double>(p) * step, step});
            r.written += k;
        }
        const uint64_t keep = static_cast<uint64_t>(r.prate() * POS_MAP_SECONDS);
        while (r.pos_map.size() > 1 && r.pos_map.front().p0 + r.pos_map.front().n + keep < r.written)
            r.pos_map.pop_front();
        auto handler = [this, idx](uint32_t type, const char* p, size_t len) { on_message(idx, type, p, len); };
        if (sync_io_) r.proc->flush_blocking(handler, 60000);
        else r.proc->poll(handler);
        kproc::ExitStatus st;
        const bool bad = r.proc->protocol_error();
        if (bad || !r.proc->alive(&st)) {
            r.proc->poll(handler);   // whatever it said last
            std::string why;
            if (bad) why = "sent a malformed message (printing to stdout?)";
            else why = st.describe();   // "crashed (<strsignal>)" / "exited with status N"
            r.tail = r.proc->stderr_tail();
            if (!r.tail.empty()) why += " - last output: " + r.tail.back();
            r.proc->stop();
            r.proc.reset();
            close_talker(r);
            trunk_reset_if(r);
            report_.choices_clear(r.id);   // (the restarted process sends its own)
            r.crashes++;
            int64_t delay = std::min<int64_t>(30000, 1000LL << std::min(r.crashes, 5));
            r.note.clear();
            problem(r, "crashed", r.info.name + " " + why + "; restarting in " + std::to_string(delay / 1000) + " s", delay);
        }
    }

    void on_message(size_t idx, uint32_t type, const char* p, size_t len) {
        Runner& r = *runners_[idx];
        switch (type) {
            case W_FACT: {
                size_t z = std::find(p, p + len, '\0') - p;
                std::string k(p, z), v = z < len ? std::string(p + z + 1, len - z - 1) : std::string();
                if (k.empty() || k.size() > 64) break;
                if (v.size() > 400) v.resize(400);
                if (v.empty()) report_.erase(r.id, k);
                else report_.set(r.id, k, v);
                break;
            }
            case W_EVENT: {
                if (len < 4) break;
                float d;
                memcpy(&d, p, 4);
                std::string text(p + 4, std::min<size_t>(len - 4, 1000));
                if (report_.event(r.id, text, is_finite_value(d) ? std::clamp<double>(d, 0.0, 3600.0) : 2.0)) {
                    LogRecord lr;
                    lr.type = "event";
                    lr.plugin = r.id;
                    lr.text = text;
                    lr.rf_hz = rf_hz_;
                    deliver_record(owner_, lr);
                }
                break;
            }
            case W_VALID:
                on_valid(r.id);
                if (len > 0) on_valid_span(r, p, len);
                break;
            case W_AUDIO:
                if (is_lead(r) && (owner_->voice_wanted() || owner_->voice_tap() || owner_->record_tap()) && len >= 4) {
                    std::vector<float> a(len / 4);
                    memcpy(a.data(), p, a.size() * 4);
                    for (float& v : a)
                        if (!is_finite_value(v)) v = 0;
                    if (owner_->voice_wanted()) owner_->push_voice(a.data(), a.size());
                    if (owner_->voice_tap()) {
                        // who is talking: the open talker on the channel the
                        // plugin says its audio is from (DMR: the timeslot it
                        // plays - with both slots busy, the most recent talker
                        // was often the OTHER slot's radio); not said: the one
                        // that started first (decoders play the first to talk)
                        const Runner::OpenTalker* t = nullptr;
                        if (r.audio_channel >= 0) {
                            auto it = r.talkers.find(r.audio_channel);
                            if (it != r.talkers.end()) t = &it->second;
                        } else {
                            for (const auto& kv : r.talkers)
                                if (!t || kv.second.start < t->start) t = &kv.second;
                        }
                        deliver_voice_tap(owner_, a.data(), a.size(), r.id, t ? t->id : "", t ? t->label : "", rf_hz_);
                    }
                    if (owner_->record_tap()) deliver_record_voice(owner_, a.data(), a.size());
                }
                break;
            case W_AUDIO_CHANNEL:
                if (len >= 4) {
                    int32_t c;
                    memcpy(&c, p, 4);
                    r.audio_channel = c >= 0 && c < 8 ? c : -1;
                }
                break;
            case W_VOICE_STATE:
                if (is_lead(r)) owner_->set_voice_state(std::string(p, std::min<size_t>(len, 200)));
                break;
            case W_FREQ_ERROR:
                if (is_lead(r) && len >= 4) {
                    float hz;
                    memcpy(&hz, p, 4);
                    if (is_finite_value(hz)) on_freq_error(std::clamp(hz, -AFC_LIMIT_HZ, AFC_LIMIT_HZ));
                }
                break;
            case W_SYNC_DONE:
                if (len >= 4) memcpy(&r.sync_done, p, 4);
                break;
            case W_MAP_POINT:
                on_map_point(r.id, p, len);
                break;
            case W_MAP_REMOVE:
                report_.map_remove(r.id, std::string(p, std::min<size_t>(len, 32)));
                break;
            case W_TABLE_COLUMNS: {
                // no columns (an empty payload) = no table: the page shows none
                auto f = len ? split0(p, len, 40) : std::vector<std::string>{};
                for (auto& c : f) c.resize(std::min<size_t>(c.size(), 40));
                report_.table_columns(r.id, std::move(f));
                break;
            }
            case W_TABLE_ROW: {
                auto f = split0(p, len, 41);
                if (f.empty() || f[0].empty() || f[0].size() > 64) break;
                std::string key = f[0];
                f.erase(f.begin());
                for (auto& c : f) c.resize(std::min<size_t>(c.size(), 200));
                report_.table_row(r.id, key, std::move(f));
                break;
            }
            case W_RAW: {
                if (!log_all() || len == 0) break;
                LogRecord lr;
                lr.type = "raw";
                lr.plugin = r.id;
                lr.text.assign(p, std::min<size_t>(len, 2000));
                lr.rf_hz = rf_hz_;
                deliver_record(owner_, lr);
                break;
            }
            case W_TABLE_REMOVE:
                report_.table_remove(r.id, std::string(p, std::min<size_t>(len, 64)));
                break;
            case W_TABLE_KEEP: {
                const double s = strtod(std::string(p, std::min<size_t>(len, 32)).c_str(), nullptr);
                if (is_finite_value(s)) report_.table_keep(r.id, static_cast<int64_t>(s * 1000));
                break;
            }
            case W_TABLE_DETAIL: {
                // key, then label / value pairs
                auto f = split0(p, len, 1 + 2 * 200);
                if (f.empty() || f[0].empty() || f[0].size() > 64) break;
                std::vector<std::pair<std::string, std::string>> lines;
                for (size_t i = 1; i + 1 < f.size(); i += 2) {
                    f[i].resize(std::min<size_t>(f[i].size(), 80));
                    f[i + 1].resize(std::min<size_t>(f[i + 1].size(), 400));
                    lines.emplace_back(std::move(f[i]), std::move(f[i + 1]));
                }
                report_.table_detail(r.id, f[0], std::move(lines));
                break;
            }
            case W_TALKER:
                on_talker(r, p, len);
                break;
            case W_TALKER_FORGET: {
                if (len == 0) break;
                TalkerSpan s;
                s.plugin = r.id;
                s.id.assign(p, std::min<size_t>(len, 32));
                s.forget = true;
                owner_->deliver_talker(s);
                break;
            }
            case W_TALKER_END: {
                auto f = split0(p, std::min<size_t>(len, 64), 2);
                if (f.empty()) break;
                auto it = r.talkers.find(f.size() > 1 ? talker_channel(f[1]) : 0);
                if (it == r.talkers.end()) break;
                uint64_t at;
                if (r.vfo_pos(strtod(f[0].c_str(), nullptr), r.prate(), &at))
                    it->second.end = std::max(it->second.start, at);
                close_talker(r, it);
                break;
            }
            case W_SPATIAL:
                on_spatial(r, p, len);
                break;
            case W_SPATIAL_REMOVE: {
                // kp::Host::spatial_remove: id, "" = every spatial talker of the plugin
                SpatialReport s;
                s.plugin = r.id;
                s.id.assign(p, std::min<size_t>(len, 32));
                s.remove = true;
                owner_->deliver_spatial(s);
                break;
            }
            case W_PLOT:
                on_plot(r, p, len);
                break;
            case W_PLOT_EX:
                on_plot_ex(r, p, len);
                break;
            case W_VIDEO:
                on_video(r, p, len);
                break;
            case W_OPTION_CHOICES:
                on_option_choices(r, p, len);
                break;
            case W_TRUNK_SYSTEM:
            case W_TRUNK_CHANNEL:
            case W_CONTROL_CHANNEL:
            case W_GRANT:
            case W_GRANT_END:
            case W_CALL:
                on_trunk(r, type, p, len);
                break;
            case W_TUNE: {
                // kp::Host::tune - only plugins that declare kp::Info::tunes
                const double hz = num_or_nan(std::string(p, std::min<size_t>(len, 32)));
                char mhz[32];
                snprintf(mhz, sizeof mhz, "%.6f MHz", is_finite_value(hz) ? hz / 1e6 : 0.0);
                std::string why;
                const int64_t now = now_ms();
                if (!r.info.tunes) why = "the plugin doesn't declare kp::Info::tunes";
                else if (!is_finite_value(hz) || hz <= 0) why = "not a frequency";
                else if (now - r.last_tune_ms < TUNE_MIN_GAP_MS) why = "too soon after the last one";
                else why = owner_->deliver_tune(hz);
                if (why.empty()) r.last_tune_ms = now;
                else report_.event("", "Retune to " + std::string(mhz) + " asked by " + r.info.name + " refused: " + why, 10.0);
                break;
            }
            case W_MESSAGE: {
                auto f = split0(p, len, 2);
                if (f.size() == 2 && !f[1].empty()) {
                    deliver_message(owner_, r.id, f[0].substr(0, 64), f[1].substr(0, 2000), rf_hz_);
                    LogRecord lr;
                    lr.type = "message";
                    lr.plugin = r.id;
                    lr.from = f[0].substr(0, 64);
                    lr.text = f[1].substr(0, 2000);
                    lr.rf_hz = rf_hz_;
                    deliver_record(owner_, lr);
                }
                break;
            }
            default:
                break;
        }
    }

    // --- trunked systems (kp::wire::TRUNK_SYSTEM .. CALL) -----------------
    // the lead plugin's reports, checked again (cut to the limits, control
    // characters removed, frequencies outside (0, 6 GHz] NAN, slots outside
    // -1..15 -1, options checked), to the owner's TrunkHandler. A channel
    // without a name, a CALL with an unknown state or a GRANT whose E fields
    // aren't there are dropped
    double stream_time() const { return in_rate_ > 0 ? static_cast<double>(next_pos_) / in_rate_ : static_cast<double>(NAN); }
    void trunk_send(const std::string& plugin, trunk::PluginEvent&& ev) {
        ev.t_s = stream_time();
        owner_->deliver_trunk(plugin, std::move(ev));
    }
    // the reports so far are stale: one RESET (only if there were any)
    void trunk_reset() {
        if (trunk_from_.empty()) return;
        std::string id;
        id.swap(trunk_from_);
        trunk::PluginEvent ev;
        ev.kind = trunk::PluginEvent::RESET;
        trunk_send(id, std::move(ev));
    }
    void trunk_reset_if(const Runner& r) {
        if (trunk_from_ == r.id) trunk_reset();
    }
    // text: valid UTF-8 without C0 / DEL / C1 controls, cut to max bytes at
    // a character boundary - the SDK's own rule (kp::wire::clean_text, the
    // plugin side applied it already), so a name is the same bytes everywhere
    static std::string tk_text(const std::string& in, size_t max) { return kp::wire::clean_text(in, max); }
    // a lowercase token ([a-z0-9_], with '-' if dash) of <= max bytes, else ""
    static std::string tk_token(const std::string& in, size_t max, bool dash) {
        if (in.size() > max) return "";
        for (char c : in)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (dash && c == '-'))) return "";
        return in;
    }
    static double tk_freq(const std::string& t) {
        const double v = num_or_nan(t);
        return is_finite_value(v) && v > 0 && v <= TRUNK_FREQ_MAX_HZ ? v : static_cast<double>(NAN);
    }
    // a whole decimal integer in lo..hi, else def ("" = def)
    static long long tk_int(const std::string& t, long long lo, long long hi, long long def) {
        if (t.empty() || t.size() > 20) return def;
        char* e = nullptr;
        errno = 0;
        const long long v = strtoll(t.c_str(), &e, 10);
        return (e && *e == '\0' && errno == 0 && v >= lo && v <= hi) ? v : def;
    }
    // a channel from f[at] ..: name, number, freq_hz, slot - and uplink_hz in f[up] (NAN when not there)
    static bool tk_channel(const std::vector<std::string>& f, size_t at, size_t up, trunk::Channel* c) {
        auto fld = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
        c->name = tk_text(fld(at), TRUNK_NAME_MAX);
        if (c->name.empty()) return false;
        c->number = tk_int(fld(at + 1), 0, std::numeric_limits<long long>::max(), -1);
        c->freq_hz = tk_freq(fld(at + 2));
        c->slot = static_cast<int>(tk_int(fld(at + 3), -1, TRUNK_SLOT_MAX, -1));
        c->uplink_hz = tk_freq(fld(up));
        return true;
    }
    void on_trunk(Runner& r, uint32_t type, const char* p, size_t len) {
        if (!is_lead(r)) return;   // Auto detect: only the plugin it locked onto
        if (r.sync_done < r.trunk_fence) return;   // from before the last RESET
        trunk::PluginEvent ev;
        auto fl = [](const std::string& flags, char c) { return flags.find(c) != std::string::npos; };
        switch (type) {
            case W_TRUNK_SYSTEM: {
                auto f = split0(p, len, 4);
                f.resize(4);
                ev.kind = trunk::PluginEvent::SYSTEM;
                ev.sys_key = f[0].substr(0, TRUNK_KEY_MAX);
                for (char& c : ev.sys_key)
                    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) c = '_';
                ev.sys_name = tk_text(f[1], TRUNK_TEXT_MAX);
                ev.sys_site = tk_text(f[2], TRUNK_TEXT_MAX);
                ev.control = f[3] == "1";
                break;
            }
            case W_TRUNK_CHANNEL:
            case W_CONTROL_CHANNEL:
            case W_GRANT_END: {
                const auto f = split0(p, len, 5);
                if (!tk_channel(f, 0, 4, &ev.ch)) return;
                ev.kind = type == W_TRUNK_CHANNEL     ? trunk::PluginEvent::CHANNEL
                          : type == W_CONTROL_CHANNEL ? trunk::PluginEvent::CONTROL
                                                      : trunk::PluginEvent::GRANT_END;
                break;
            }
            case W_GRANT: {
                // E, E fixed fields (the ones this host knows taken, the rest
                // skipped), then option key / value pairs
                const size_t nl = static_cast<size_t>(std::find(p, p + std::min<size_t>(len, 4), '\0') - p);
                const std::string es(p, nl);
                if (es.empty() || es.size() > 2 || es.find_first_not_of("0123456789") != std::string::npos) return;
                const size_t ne = static_cast<size_t>(std::stoi(es));
                const auto f = split0(p, len, 1 + ne + 2 * 64);
                if (f.size() < 1 + ne) return;
                std::vector<std::string> x(GRANT_FIXED_KNOWN);
                for (size_t i = 0; i < GRANT_FIXED_KNOWN && i < ne; i++) x[i] = f[1 + i];
                // name, number, freq_hz, slot, target, group, source, flags, decoder, demod, bw_hz, uplink_hz
                if (!tk_channel(x, 0, 11, &ev.ch)) return;
                ev.kind = trunk::PluginEvent::GRANT;
                ev.target = tk_text(x[4], TRUNK_ID_MAX);
                ev.group = x[5] != "0";
                ev.source = tk_text(x[6], TRUNK_ID_MAX);
                ev.encrypted = fl(x[7], 'e');
                ev.emergency = fl(x[7], 'm');
                ev.data = fl(x[7], 'd');
                ev.update = fl(x[7], 'u');
                ev.decoder = tk_token(x[8], TRUNK_ID_MAX, true);
                ev.demod = tk_token(x[9], TRUNK_DEMOD_MAX, false);
                const double bw = num_or_nan(x[10]);
                ev.bw_hz = is_finite_value(bw) && bw > 0 && bw <= TRUNK_BW_MAX_HZ ? bw : 0.0;
                for (size_t i = 1 + ne; i + 1 < f.size() && ev.options.size() < TRUNK_OPTIONS_MAX; i += 2) {
                    const std::string& k = f[i];
                    const std::string& v = f[i + 1];
                    // not one of the host's own OPTION keys (verbose, log_raw,
                    // video_wanted: kp::wire::host_option), the value clean text
                    bool ok = !tk_token(k, 32, false).empty() && !kp::wire::host_option(k) &&
                              v.size() <= TRUNK_OPTION_VALUE_MAX && kp::wire::clean_text(v, v.size()) == v;
                    for (const auto& o : ev.options) ok = ok && o.first != k;
                    if (ok) ev.options.emplace_back(k, v);
                }
                break;
            }
            case W_CALL: {
                auto f = split0(p, len, 6);
                f.resize(6);
                const long long st = f[0].empty() ? 1 : tk_int(f[0], 0, 2, -1);
                if (st < 0) return;
                ev.kind = trunk::PluginEvent::CALL;
                ev.call_state = static_cast<int>(st);
                ev.target = tk_text(f[1], TRUNK_ID_MAX);
                ev.group = f[2] != "0";
                ev.source = tk_text(f[3], TRUNK_ID_MAX);
                ev.call_slot = static_cast<int>(tk_int(f[4], -1, TRUNK_SLOT_MAX, -1));
                ev.encrypted = fl(f[5], 'e');
                ev.emergency = fl(f[5], 'm');
                break;
            }
            default:
                return;
        }
        if (!trunk_from_.empty() && trunk_from_ != r.id) trunk_reset();   // (another plugin's state)
        trunk_from_ = r.id;
        trunk_send(r.id, std::move(ev));
    }

    // kp::wire::OPTION_CHOICES: key, then value / label pairs (kp::Host::option_choices).
    // Only for an option the plugin declares; the text comes from a child process (and
    // a DAB label from the air), so it is bounded and checked here again: a value the
    // page couldn't set (DIGITAL_OPT: <= 100 bytes, no control characters) or that
    // repeats is dropped, a label cut at a character boundary
    void on_option_choices(Runner& r, const char* p, size_t len) {
        auto f = split0(p, len, 1 + 2 * CHOICES_MAX);
        if (f.empty() || !r.known) return;
        bool declared = false;
        for (const auto& o : r.info.options) declared = declared || o.key == f[0];
        if (!declared) return;
        std::vector<std::pair<std::string, std::string>> list;
        for (size_t i = 1; i + 1 < f.size(); i += 2) {
            std::string& v = f[i];
            std::string& l = f[i + 1];
            bool ok = v.size() <= CHOICE_VALUE_MAX;
            for (unsigned char c : v) ok = ok && c >= 0x20 && c != 0x7F;
            for (const auto& x : list) ok = ok && x.first != v;
            if (!ok) continue;
            for (char& c : l)
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
            if (l.size() > CHOICE_LABEL_MAX) {
                size_t e = CHOICE_LABEL_MAX;
                while (e > 0 && (static_cast<unsigned char>(l[e]) & 0xC0) == 0x80) e--;
                l.resize(e);
            }
            if (l.empty()) l = v;
            list.emplace_back(std::move(v), std::move(l));
        }
        report_.choices_set(r.id, f[0], std::move(list));
    }

    // kp::wire::SPATIAL: id, label, start_s, end_s, delay_us, power_db, snr_db,
    // ref, avg_s, m, cov ("re,im" pairs, row-major) - '\0'-separated text
    static double num_or_nan(const std::string& t) {
        if (t.empty()) return NAN;
        char* e = nullptr;
        const double v = strtod(t.c_str(), &e);
        return (e && *e == '\0' && is_finite_value(v)) ? v : static_cast<double>(NAN);
    }
    void on_spatial(Runner& r, const char* p, size_t len) {
        auto f = split0(p, len, 11);
        if (f.size() < 11 || f[0].empty()) return;
        SpatialReport s;
        s.plugin = r.id;
        s.id = f[0].substr(0, 32);
        s.label = f[1].substr(0, 64);
        uint64_t a, b;
        const double ta = num_or_nan(f[2]), tb = num_or_nan(f[3]);
        if (!r.vfo_pos(ta, r.array_rate > 0 ? r.array_rate : in_rate_, &a) ||
            !r.vfo_pos(tb, r.array_rate > 0 ? r.array_rate : in_rate_, &b))
            return;
        s.start = a;
        s.end = std::max(a, b);
        s.rate = r.array_rate > 0 ? static_cast<float>(r.array_rate) : in_rate_;
        s.delay_us = num_or_nan(f[4]);
        s.power_db = num_or_nan(f[5]);
        s.snr_db = num_or_nan(f[6]);
        s.ref = f[7] == "1";
        const double avg = num_or_nan(f[8]);
        s.avg_s = is_finite_value(avg) ? std::clamp(avg, 0.0, 3600.0) : 0.0;
        s.m = static_cast<int>(strtol(f[9].c_str(), nullptr, 10));
        if (s.m < 1 || s.m > 8) return;
        std::vector<double> v;
        v.reserve(static_cast<size_t>(2 * s.m * s.m));
        const char* c = f[10].c_str();
        while (*c && v.size() < static_cast<size_t>(2 * s.m * s.m)) {
            char* e = nullptr;
            const double x = strtod(c, &e);
            if (e == c || !is_finite_value(x)) return;
            v.push_back(x);
            c = *e == ',' ? e + 1 : e;
        }
        if (v.size() != static_cast<size_t>(2 * s.m * s.m)) return;
        s.cov.resize(static_cast<size_t>(s.m * s.m));
        for (size_t i = 0; i < s.cov.size(); i++) s.cov[i] = {v[2 * i], v[2 * i + 1]};
        owner_->deliver_spatial(s);
    }

    // kp::wire::PLOT -> the decoder's latest plot (JSON for the page)
    void on_plot(Runner& r, const char* p, size_t len) {
        auto f = split0(p, len, 8 + 3 * 64 + 1);
        std::string o = plot_json(f);
        if (!o.empty()) report_.plot_set(r.id, r.id, std::move(o));
    }

    // kp::wire::PLOT_EX: E, E extension fields (id, place, click_option,
    // click_also, click_snap, click_step - later ones skipped), then a PLOT
    // payload. Stored as "<plugin>/<id>" (or "<plugin>" with no id, as PLOT),
    // the extension as more JSON keys: id, pos ("video" = beside the video
    // display), opt / also (a click sets them; only valid option keys and
    // printable values pass), snap, step. A page that doesn't know them
    // shows a "<plugin>" plot as before and never looks at the others.
    void on_plot_ex(Runner& r, const char* p, size_t len) {
        const size_t nl = static_cast<size_t>(std::find(p, p + std::min<size_t>(len, 4), '\0') - p);
        const std::string es(p, nl);
        if (es.empty() || es.size() > 2 || es.find_first_not_of("0123456789") != std::string::npos) return;
        const size_t ne = static_cast<size_t>(std::stoi(es));
        auto f = split0(p, len, 1 + ne + 8 + 3 * 64 + 1);
        if (f.size() < 1 + ne) return;
        auto ext = [&](size_t i) { return i < ne ? f[1 + i] : std::string(); };
        const std::string id = ext(0);
        if (id.size() > 32) return;
        for (unsigned char c : id)
            if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return;
        std::string opt = ext(2), also = ext(3);
        if (!opt.empty() && !valid_option_key(r.id + "." + opt)) opt.clear();
        if (!also.empty()) {
            const size_t eq = also.find('=');
            bool ok = eq != std::string::npos && valid_option_key(r.id + "." + also.substr(0, eq)) && also.size() - eq - 1 <= 100;
            for (unsigned char c : also)
                if (c < 0x20 || c == 0x7F) ok = false;
            if (!ok) also.clear();
        }
        auto jn = [](const std::string& t) {
            const double v = num_or_nan(t);
            if (!is_finite_value(v) || v <= 0) return std::string("null");
            char b[48];
            snprintf(b, sizeof b, "%.9g", v);
            return std::string(b);
        };
        std::string o = plot_json(std::vector<std::string>(f.begin() + static_cast<std::ptrdiff_t>(1 + ne), f.end()));
        if (o.empty()) return;
        o.pop_back();   // the closing brace
        o += ",\"id\":\"" + json_escape(id) + "\",\"pos\":\"" + std::string(ext(1) == "1" ? "video" : "") + "\",\"opt\":\"" +
             json_escape(opt) + "\",\"also\":\"" + json_escape(also) + "\",\"snap\":" + jn(ext(4)) + ",\"step\":" + jn(ext(5)) + "}";
        report_.plot_set(r.id, id.empty() ? r.id : r.id + "/" + id, std::move(o), MAX_PLOTS_PER_PLUGIN);
    }

    // a PLOT payload's fields as the page's JSON; "" = malformed
    static std::string plot_json(const std::vector<std::string>& f) {
        if (f.size() < 9) return "";
        auto jn = [](const std::string& t, int prec) {
            const double v = num_or_nan(t);
            if (!is_finite_value(v)) return std::string("null");
            char b[48];
            snprintf(b, sizeof b, "%.*g", prec, v);
            return std::string(b);
        };
        std::string o = "{\"t\":\"" + json_escape(f[0].substr(0, 80)) + "\",\"xu\":\"" + json_escape(f[1].substr(0, 16)) +
                        "\",\"yu\":\"" + json_escape(f[2].substr(0, 16)) + "\",\"x0\":" + jn(f[3], 9) + ",\"dx\":" + jn(f[4], 9) +
                        ",\"ymin\":" + jn(f[5], 6) + ",\"ymax\":" + jn(f[6], 6) + ",\"thr\":" + jn(f[7], 6) + ",\"y\":[";
        const char* c = f[8].c_str();
        int n = 0;
        while (*c && n < 1000) {
            char* e = nullptr;
            const double x = strtod(c, &e);
            if (e == c) break;
            char b[32];
            snprintf(b, sizeof b, "%.4g", is_finite_value(x) ? x : 0.0);
            o += (n ? "," : "") + std::string(b);
            n++;
            c = *e == ',' ? e + 1 : e;
        }
        o += "],\"mk\":[";
        for (size_t i = 9, k = 0; i + 2 < f.size() + 0 && k < 64; i += 3, k++)
            o += std::string(k ? "," : "") + "[" + jn(f[i], 9) + ",\"" + json_escape(f[i + 1].substr(0, 16)) + "\",\"" +
                 json_escape(f[i + 2].substr(0, 32)) + "\"]";
        o += "]}";
        return o;
    }

    // kp::wire::VIDEO: uint16 width, height; uint8 format, flags; uint16
    // label_len; label; then width*height*bpp pixel bytes (bpp = 1 GRAY8 /
    // 3 RGB24), all little-endian. Forwarded as a binary frame to the panel's
    // canvas; a frame whose length doesn't match its header is dropped.
    void on_video(Runner& r, const char* p, size_t len) {
        constexpr size_t HDR = 8;   // width, height, format, encoding, label_len
        if (len < HDR || !video_wanted.load(std::memory_order_relaxed)) return;   // no viewer: no copies
        const auto* u = reinterpret_cast<const uint8_t*>(p);
        const uint32_t w = static_cast<uint32_t>(u[0]) | (static_cast<uint32_t>(u[1]) << 8);
        const uint32_t h = static_cast<uint32_t>(u[2]) | (static_cast<uint32_t>(u[3]) << 8);
        const uint8_t format = u[4];
        const uint8_t encoding = u[5];   // 0 = raw pixels, 2 = JPEG bitstream
        const uint32_t label_len = static_cast<uint32_t>(u[6]) | (static_cast<uint32_t>(u[7]) << 8);
        if (format > 1 || encoding > 2 || w == 0 || h == 0) return;
        if (len < HDR + label_len) return;   // the label must fit
        size_t data_len;
        if (encoding == 0) {   // raw: the pixel count must match the geometry exactly
            const size_t bpp = format == 1 ? 3 : 1;
            data_len = static_cast<size_t>(w) * static_cast<size_t>(h) * bpp;
            if (HDR + label_len + data_len != len) return;
        } else {   // JPEG: a variable-length blob - the host forwards it opaquely
            data_len = len - HDR - label_len;
            if (data_len == 0) return;
        }
        VideoFrameReport vf;
        vf.width = static_cast<int>(w);
        vf.height = static_cast<int>(h);
        vf.format = format;
        vf.encoding = encoding;
        vf.label.assign(p + HDR, label_len);
        vf.pixels.assign(u + HDR + label_len, u + HDR + label_len + data_len);
        owner_->deliver_video(vf);
        (void)r;
    }

    // kp::wire::MAP_POINT: id, lat, lon, label, kind, heading, altitude_m,
    // speed_kmh, ttl_s, info - '\0'-separated, numbers as text ("" = unknown)
    void on_map_point(const std::string& plugin, const char* p, size_t len) {
        std::vector<std::string> f = split0(p, len, 10);
        if (f.size() < 3) return;
        f.resize(10);
        auto num = [](const std::string& t) {
            if (t.empty()) return static_cast<double>(NAN);
            char* e = nullptr;
            double v = strtod(t.c_str(), &e);
            return (e && *e == '\0' && is_finite_value(v)) ? v : static_cast<double>(NAN);
        };
        MapPoint m;
        m.plugin = plugin;
        m.id = f[0].substr(0, 32);
        m.lat = num(f[1]);
        m.lon = num(f[2]);
        if (m.id.empty() || !is_finite_value(m.lat) || !is_finite_value(m.lon) || std::fabs(m.lat) > 90 ||
            std::fabs(m.lon) > 180)
            return;
        m.label = f[3].substr(0, 64);
        m.kind = f[4].empty() ? "point" : f[4].substr(0, 16);
        m.heading = static_cast<float>(num(f[5]));
        m.alt_m = static_cast<float>(num(f[6]));
        m.speed_kmh = static_cast<float>(num(f[7]));
        double ttl = num(f[8]);
        m.ttl_s = is_finite_value(ttl) ? std::clamp(ttl, 10.0, 86400.0) : 300.0;
        m.info = f[9].substr(0, 1000);
        LogRecord lr;
        lr.type = "position";
        lr.plugin = plugin;
        lr.point = &m;
        lr.rf_hz = rf_hz_;
        deliver_record(owner_, lr);
        report_.map_set(std::move(m));
    }

    // --- talkers (kp::wire::TALKER: id, label, start_s, end_s [, channel [, flags,
    // freq, bw, avg [, lat, lon, alt [, keep [, m, cov]]]]]) -------------------------
    using OpenTalker = Runner::OpenTalker;
    static int talker_channel(const std::string& v) {
        return static_cast<int>(std::clamp(strtol(v.c_str(), nullptr, 10), 0L, 7L));
    }
    void on_talker(Runner& r, const char* p, size_t len) {
        auto f = split0(p, len, 15);
        if (f.size() < 4 || f[0].empty()) return;
        const std::string id = f[0].substr(0, 32);
        const int ch = f.size() > 4 ? talker_channel(f[4]) : 0;
        if (f.size() > 5 && f[5].find('p') != std::string::npos) {
            // one packet: exactly these samples, nothing stays open
            uint64_t a, b;
            if (!r.vfo_pos(strtod(f[2].c_str(), nullptr), r.prate(), &a) ||
                !r.vfo_pos(strtod(f[3].c_str(), nullptr), r.prate(), &b) || b <= a ||
                static_cast<double>(b - a) > in_rate_ * MAX_PACKET_S)
                return;
            TalkerSpan s;
            s.plugin = r.id;
            s.id = id;
            s.label = f[1].substr(0, 64);
            s.start = a;
            s.end = b;
            s.closed = true;
            s.rate = in_rate_;
            s.packet = true;
            if (f.size() > 8) {
                // the plugin's frequency -> the VFO's: the AFC mixer moved the band
                // down by afc_hz_
                if (!f[6].empty()) {
                    const double pf = strtod(f[6].c_str(), nullptr);
                    if (is_finite_value(pf)) s.freq_hz = pf + afc_hz_;
                }
                s.bw_hz = std::clamp(strtod(f[7].c_str(), nullptr), 0.0, static_cast<double>(in_rate_));
                s.avg_s = std::clamp(strtod(f[8].c_str(), nullptr), 0.0, 3600.0);
                if (!is_finite_value(s.bw_hz)) s.bw_hz = 0;
                if (!is_finite_value(s.avg_s)) s.avg_s = 0;
            }
            if (f.size() > 11) {
                // where the talker was (the array calibration from aircraft)
                const double la = strtod(f[9].c_str(), nullptr), lo = strtod(f[10].c_str(), nullptr),
                             al = strtod(f[11].c_str(), nullptr);
                if (is_finite_value(la) && is_finite_value(lo) && is_finite_value(al) && std::fabs(la) <= 90 &&
                    std::fabs(lo) <= 180 && al > -1000 && al < 100000 && !(la == 0 && lo == 0)) {
                    s.lat = la;
                    s.lon = lo;
                    s.alt_m = al;
                }
            }
            if (f.size() > 12) {
                // how long the talker is kept after its last packet (0 = the default)
                const double k = strtod(f[12].c_str(), nullptr);
                if (is_finite_value(k)) s.keep_s = std::clamp(k, 0.0, 7 * 86400.0);
            }
            if (f.size() > 14) {
                // the packet's own covariance (kp::Talker::cov: separation by code)
                const long m = strtol(f[13].c_str(), nullptr, 10);
                if (m >= 2 && m <= 8) {
                    std::vector<std::complex<double>> c;
                    c.reserve(static_cast<size_t>(m * m));
                    const char* q = f[14].c_str();
                    char* e = nullptr;
                    while (*q && c.size() < static_cast<size_t>(m * m)) {
                        const double re = strtod(q, &e);
                        if (e == q || *e != ',') break;
                        q = e + 1;
                        const double im = strtod(q, &e);
                        if (e == q) break;
                        if (!is_finite_value(re) || !is_finite_value(im)) break;
                        c.emplace_back(re, im);
                        q = *e == ',' ? e + 1 : e;
                    }
                    if (c.size() == static_cast<size_t>(m * m)) {
                        s.cov_m = static_cast<int>(m);
                        s.cov = std::move(c);
                    }
                }
            }
            owner_->deliver_talker(s);
            return;
        }
        auto it = r.talkers.find(ch);
        uint64_t a, b;
        if (!r.vfo_pos(strtod(f[3].c_str(), nullptr), r.prate(), &b)) return;
        if (!r.vfo_pos(strtod(f[2].c_str(), nullptr), r.prate(), &a)) {
            // began before the oldest mapped sample (a transmission > POS_MAP_SECONDS)
            if (it != r.talkers.end() && it->second.id == id) a = it->second.start;
            else a = static_cast<uint64_t>(std::max(0.0, r.pos_map.front().v0));
        }
        if (it != r.talkers.end() && it->second.id != id) {
            // another radio on this channel: the previous one ended where this one starts
            it->second.end = std::clamp(a, it->second.start, it->second.end);
            close_talker(r, it);
            it = r.talkers.end();
        }
        if (it == r.talkers.end()) it = r.talkers.emplace(ch, Runner::OpenTalker{id, "", a, a}).first;
        OpenTalker& t = it->second;
        t.label = f[1].substr(0, 64);
        t.end = std::max(t.end, b);
        send_talker(r, ch, t, false);
    }
    void send_talker(const Runner& r, int ch, const OpenTalker& t, bool closed) {
        TalkerSpan s;
        s.channel = ch;
        s.plugin = r.id;
        s.id = t.id;
        s.label = t.label;
        s.start = t.start;
        s.end = t.end;
        s.closed = closed;
        s.rate = in_rate_;
        owner_->deliver_talker(s);
    }
    void close_talker(Runner& r, std::map<int, OpenTalker>::iterator it) {
        send_talker(r, it->first, it->second, true);
        r.talkers.erase(it);
    }
    void close_talker(Runner& r) {   // all of the plugin's
        while (!r.talkers.empty()) close_talker(r, r.talkers.begin());
    }

    // --- detection / AFC ----------------------------------------------------
    void on_valid(const std::string& id) {
        std::lock_guard<std::mutex> lk(st_mu_);
        int64_t t = now_ms();
        valid_.emplace_back(t, id);
        totals_[id]++;
        last_valid_ms_ = t;
        last_valid_by_[id] = t;
    }
    // kp::Host::valid(start, end): the frame's samples -> the VFO's stream
    // positions, as a signal span (the Digital squelch's DoA material)
    void on_valid_span(Runner& r, const char* p, size_t len) {
        auto f = split0(p, len, 2);
        if (f.size() < 2) return;
        uint64_t a, b;
        if (!r.vfo_pos(strtod(f[0].c_str(), nullptr), r.prate(), &a) ||
            !r.vfo_pos(strtod(f[1].c_str(), nullptr), r.prate(), &b) || b <= a ||
            static_cast<double>(b - a) > in_rate_ * MAX_SIGNAL_SPAN_S)
            return;
        TalkerSpan s;
        s.plugin = r.id;
        s.signal = true;
        s.start = a;
        s.end = b;
        s.closed = true;
        s.rate = in_rate_;
        owner_->deliver_talker(s);
    }
public:
    int64_t last_valid_of(const std::string& id) const {
        std::lock_guard<std::mutex> lk(st_mu_);
        auto it = last_valid_by_.find(id);
        return it == last_valid_by_.end() ? 0 : it->second;
    }
    int64_t squelch_hold_ms(const std::string& id, int64_t def) const {
        std::lock_guard<std::mutex> lk(st_mu_);
        auto it = hold_ms_by_.find(id);
        return it == hold_ms_by_.end() ? def : it->second;
    }
private:

    void on_freq_error(float hz) {
        std::lock_guard<std::mutex> lk(st_mu_);
        residual_hz_ = hz;
        if (now_ms() - afc_changed_ms_ < AFC_SETTLE_MS) return;   // measured before the last correction
        afc_sum_ += hz;
        afc_n_++;
    }

    // slow integrator (the plugins tolerate the residual themselves)
    void afc_update(int64_t t) {
        if (afc_n_ == 0 || t - afc_changed_ms_ < AFC_PERIOD_MS) return;
        float mean = afc_sum_ / static_cast<float>(afc_n_);
        afc_hz_ = std::clamp(afc_hz_ + 0.5f * mean, -AFC_LIMIT_HZ, AFC_LIMIT_HZ);
        residual_hz_ -= 0.5f * mean;
        afc_sum_ = 0;
        afc_n_ = 0;
        afc_changed_ms_ = t;
    }

    std::string name_of(const std::string& id) const {
        for (const auto& r : runners_)
            if (r->id == id) return r->known ? r->info.name : id;
        return id;
    }

    void housekeeping() {
        int64_t t = now_ms();
        std::string old, now;
        {
            std::lock_guard<std::mutex> lk(st_mu_);
            while (!valid_.empty() && t - valid_.front().first > DETECT_WINDOW_MS) valid_.pop_front();
            std::map<std::string, int> cnt;
            for (const auto& v : valid_) cnt[v.second]++;
            afc_update(t);
            old = detected_;
            if (mode_ == Mode::PLUGIN) {
                detected_ = cnt[plugin_] > 0 ? plugin_ : "";
            } else {
                std::string best;
                int bc = 0;
                for (const auto& kv : cnt)
                    if (kv.second > bc) { bc = kv.second; best = kv.first; }
                // switch only on a clear majority (2+ frames), keep the
                // current one while it is still receiving
                if (!detected_.empty() && cnt[detected_] > 0 && cnt[detected_] * 2 >= bc) {
                    // keep
                } else {
                    detected_ = bc >= 2 ? best : "";
                }
            }
            now = detected_;
            if (now.empty() && t - last_valid_ms_ > 5000) {
                // drift back to the VFO centre when idle - and land on it (a
                // float decaying by 0.99 takes minutes of denormals to reach
                // 0; at exactly 0 the mixer is a copy and multichannel
                // plugins get every antenna again)
                afc_hz_ = std::fabs(afc_hz_) < 0.5f ? 0.0f : afc_hz_ * 0.99f;
                residual_hz_ = 0;
            }
        }
        // Auto detect locked onto another plugin: what the previous lead
        // said about a trunked system is stale (it going quiet is not)
        if (now != old && !now.empty() && !trunk_from_.empty() && now != trunk_from_) trunk_reset();
        if (now != old) {
            if (!now.empty())
                report_.event(now, std::string(mode_ == Mode::AUTO ? "Detected " : "Receiving ") + name_of(now), 0.0);
            else
                report_.event(old, "Signal lost", 0.0);
        }
        // who has the VFO's voice (Voice to Text): the lead plugin if it decodes
        // voice; in Auto detect, still searching, any of its plugins that does;
        // an unknown plugin (not started yet) counts as a voice decoder
        bool voice = false;
        const Runner* lead = nullptr;
        for (const auto& r : runners_) {
            if (mode_ == Mode::PLUGIN || (!now.empty() && r->id == now)) lead = r.get();
            if (mode_ == Mode::AUTO && now.empty() && (!r->known || r->info.voice)) voice = true;
        }
        if (lead) voice = !lead->known || lead->info.voice;
        else if (mode_ == Mode::PLUGIN) voice = true;
        owner_->set_takes_voice(voice);
        // its talker now: the open one that started first
        const Runner::OpenTalker* tk = nullptr;
        if (lead)
            for (const auto& kv : lead->talkers)
                if (!tk || kv.second.start < tk->start) tk = &kv.second;
        const std::string tid = tk && tk->id != "?" ? tk->id : "", tl = tk && tk->id != "?" ? tk->label : "";
        if (tid != lead_tid_sent_ || tl != lead_tlabel_sent_) {
            lead_tid_sent_ = tid;
            lead_tlabel_sent_ = tl;
            owner_->set_lead_talker(tid, tl);
        }
    }
};

// ---------------------------------------------------------------------------
// DigitalDecoder
// ---------------------------------------------------------------------------
DigitalDecoder::DigitalDecoder(bool threaded) : engine_(std::make_unique<Engine>(report_, this, !threaded)) {
    interp_ = firinterp_rrrf_create_kaiser(6, 8, 60.0f);
    if (threaded) worker_ = std::thread([this] {
        pthread_setname_np(pthread_self(), "digital-dec");
        run();
    });
}

DigitalDecoder::~DigitalDecoder() {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        stop_ = true;
    }
    q_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    engine_.reset();   // stops the plugin processes
    if (interp_) firinterp_rrrf_destroy(static_cast<firinterp_rrrf>(interp_));
}

// ---------------------------------------------------------------------------
// Decoded voice: 8 kHz from the vocoders -> AGC -> 48 kHz FIFO -> audio thread
// ---------------------------------------------------------------------------
namespace {
constexpr size_t VOICE_RATE = 48000;
constexpr size_t VOICE_PREFILL = VOICE_RATE / 4;     // 250 ms: frames arrive in bursts (P25 LDU = 180 ms)
constexpr size_t VOICE_MAX = VOICE_RATE * 2;
constexpr int64_t VOICE_DRAIN_MS = 300;              // a call's tail plays without prefill
constexpr int64_t VOICE_WANTED_MS = 500;
}  // namespace

void DigitalDecoder::push_voice(const float* x, size_t n) {
    if (n == 0) return;
    // slow AGC towards ~-15 dBFS rms while speech is present
    double e = 0;
    for (size_t i = 0; i < n; i++) e += x[i] * x[i];
    float rms = static_cast<float>(std::sqrt(e / n));
    std::lock_guard<std::mutex> lk(v_mu_);
    if (rms > 0.002f) {
        float want = std::clamp(0.18f / rms, 0.5f, 30.0f);
        agc_gain_ += 0.15f * (want - agc_gain_);
    }
    auto q = static_cast<firinterp_rrrf>(interp_);
    float y[6];
    for (size_t i = 0; i < n; i++) {
        float v = std::tanh(x[i] * agc_gain_);   // soft limit
        firinterp_rrrf_execute(q, v, y);
        for (float s : y) vfifo_.push_back(s);
    }
    while (vfifo_.size() > VOICE_MAX) vfifo_.pop_front();
    v_last_push_ms_ = now_ms();
}

void DigitalDecoder::pull_voice(float* out, size_t n) {
    int64_t t = now_ms();
    voice_wanted_until_.store(t + VOICE_WANTED_MS, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(v_mu_);
    if (!v_playing_ && !vfifo_.empty() &&
        (vfifo_.size() >= VOICE_PREFILL || t - v_last_push_ms_ > VOICE_DRAIN_MS))
        v_playing_ = true;
    size_t k = 0;
    if (v_playing_) {
        for (; k < n && !vfifo_.empty(); k++) {
            out[k] = vfifo_.front();
            vfifo_.pop_front();
        }
        if (vfifo_.empty()) v_playing_ = false;   // underrun: prefill again
    }
    for (; k < n; k++) out[k] = 0.0f;
}

void DigitalDecoder::set_voice_state(const std::string& s) {
    std::lock_guard<std::mutex> lk(v_mu_);
    voice_state_ = s;
}

bool DigitalDecoder::voice_wanted() const {
    return now_ms() < voice_wanted_until_.load(std::memory_order_relaxed);
}

void DigitalDecoder::set_mode(Mode m, const std::string& plugin) {
    bool plugin_changed;
    {
        std::lock_guard<std::mutex> lk(opt_mu_);
        std::string p = m == Mode::PLUGIN ? plugin : "";
        plugin_changed = p != plugin_id_;
        plugin_id_ = p;
    }
    Mode old = mode_.exchange(m);
    if (old != m || plugin_changed) {
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            reset_pending_.store(true);
        }
        q_cv_.notify_all();   // the worker reconfigures (starts / stops plugins) even without data
        report_.clear_all();
        report_.map_clear();
        report_.table_clear();
        report_.plot_clear();
        set_voice_state("");
        if (m == Mode::PLUGIN) report_.event("", "Decoder set to " + plugin, 0.0);
        else if (m == Mode::AUTO) report_.event("", "Decoder on, detecting the mode automatically", 0.0);
    }
    if (!worker_.joinable()) {
        if (reset_pending_.exchange(false)) engine_->reset_all();
        engine_->configure(m, options(), plugin_id());
    }
}

std::string DigitalDecoder::plugin_id() const {
    std::lock_guard<std::mutex> lk(opt_mu_);
    return plugin_id_;
}

void DigitalDecoder::set_options(const Options& o) {
    std::lock_guard<std::mutex> lk(opt_mu_);
    opts_ = o;
    log_mode_.store(o.log, std::memory_order_relaxed);
    pos_s_.store(o.pos_s, std::memory_order_relaxed);
}

Options DigitalDecoder::options() const {
    std::lock_guard<std::mutex> lk(opt_mu_);
    return opts_;
}

void DigitalDecoder::set_talker_handler(TalkerHandler h) {
    std::lock_guard<std::mutex> lk(talker_mu_);
    talker_handler_ = std::move(h);
}

void DigitalDecoder::deliver_talker(const TalkerSpan& s) {
    TalkerHandler h;
    {
        std::lock_guard<std::mutex> lk(talker_mu_);
        h = talker_handler_;
    }
    if (h) h(s);
}

void DigitalDecoder::push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos,
                          const std::complex<int16_t>* mc, int mc_m) {
    if (mode() == Mode::OFF || n == 0) return;
    // only an array plugin runs: it never sees this stream - a one-sample
    // tick every 100 ms keeps the engine going (starts the plugin, reads its
    // replies, follows the RF) without copying 2.4 MHz of samples
    if (array_only_.load(std::memory_order_relaxed) && rate_hz > 0) {
        if (pos != NO_POS && tick_pos_ != NO_POS && pos >= tick_pos_ && static_cast<double>(pos - tick_pos_) < 0.1 * rate_hz) return;
        tick_pos_ = pos;
        n = 1;
    }
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        queue_.push_back({std::vector<std::complex<float>>(x, x + n), rate_hz, rf_hz, pos, {}, 0, 0, n});
        if (mc && mc_m >= 2) {
            queue_.back().mc.assign(mc, mc + n * static_cast<size_t>(mc_m));
            queue_.back().m = mc_m;
        }
        queued_samples_ += n;
        // decoder fell behind (CPU starved): drop the oldest data
        while (queued_samples_ > static_cast<size_t>(rate_hz) * QUEUE_MAX_SECONDS && queue_.size() > 1) {
            queued_samples_ -= queue_.front().ns;
            queue_.pop_front();
            dropped_++;
        }
    }
    q_cv_.notify_one();
}

void DigitalDecoder::set_array_wanted(bool on, double burst_s, double period_s, bool only) {
    arr_burst_.store(std::max(0.01, burst_s), std::memory_order_relaxed);
    arr_period_.store(std::max(burst_s, period_s), std::memory_order_relaxed);
    want_array_.store(on, std::memory_order_relaxed);
    array_only_.store(only, std::memory_order_relaxed);
}

void DigitalDecoder::push_array(const std::complex<float>* const* ch, int m, size_t n, float rate_hz, double rf_hz,
                                uint64_t pos) {
    if (mode() == Mode::OFF || n == 0 || m < 1 || m > 8 || !wants_array() || !(rate_hz > 0)) return;
    // only the plugin's bursts (stream time): a block whose first sample is in one
    const double burst = arr_burst_.load(std::memory_order_relaxed), period = arr_period_.load(std::memory_order_relaxed);
    if (burst < period && std::fmod(static_cast<double>(pos) / rate_hz, period) >= burst) return;
    Block b;
    b.am = m;
    b.ns = n;
    b.rate = rate_hz;
    b.rf = rf_hz;
    b.pos = pos;
    b.x.resize(static_cast<size_t>(m) * n);
    for (int k = 0; k < m; k++) std::copy_n(ch[k], n, b.x.data() + static_cast<size_t>(k) * n);
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        queue_.push_back(std::move(b));
        queued_samples_ += n;
        while (queued_samples_ > static_cast<size_t>(rate_hz) * QUEUE_MAX_SECONDS && queue_.size() > 1) {
            queued_samples_ -= queue_.front().ns;
            queue_.pop_front();
            dropped_++;
        }
    }
    q_cv_.notify_one();
}

void DigitalDecoder::set_tune_handler(TuneHandler h) {
    std::lock_guard<std::mutex> lk(talker_mu_);
    tune_handler_ = std::move(h);
}

std::string DigitalDecoder::deliver_tune(double rf_hz) {
    TuneHandler h;
    {
        std::lock_guard<std::mutex> lk(talker_mu_);
        h = tune_handler_;
    }
    return h ? h(rf_hz) : std::string("this receiver can't retune for plugins");
}

void DigitalDecoder::set_trunk_handler(TrunkHandler h) {
    std::lock_guard<std::mutex> lk(talker_mu_);
    trunk_handler_ = std::move(h);
}

void DigitalDecoder::deliver_trunk(const std::string& plugin, trunk::PluginEvent&& ev) {
    TrunkHandler h;
    {
        std::lock_guard<std::mutex> lk(talker_mu_);
        h = trunk_handler_;
    }
    if (h) h(plugin, std::move(ev));
}

void DigitalDecoder::set_spatial_handler(SpatialHandler h) {
    std::lock_guard<std::mutex> lk(talker_mu_);
    spatial_handler_ = std::move(h);
}

void DigitalDecoder::deliver_spatial(const SpatialReport& s) {
    SpatialHandler h;
    {
        std::lock_guard<std::mutex> lk(talker_mu_);
        h = spatial_handler_;
    }
    if (h) h(s);
}

void DigitalDecoder::set_video_handler(VideoHandler h) {
    std::lock_guard<std::mutex> lk(talker_mu_);
    video_handler_ = std::move(h);
}

void DigitalDecoder::deliver_video(const VideoFrameReport& f) {
    VideoHandler h;
    {
        std::lock_guard<std::mutex> lk(talker_mu_);
        h = video_handler_;
    }
    if (h) h(f);
}

void DigitalDecoder::process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, uint64_t pos,
                             const std::complex<int16_t>* mc, int mc_m) {
    if (reset_pending_.exchange(false)) engine_->reset_all();
    engine_->configure(mode(), options(), plugin_id());
    want_mc_.store(engine_->multichannel(), std::memory_order_relaxed);
    engine_->process(x, n, rate_hz, rf_hz, pos, mc, mc_m);
}

void DigitalDecoder::drain(int timeout_ms) { engine_->drain(timeout_ms); }

void DigitalDecoder::run() {
    bool pending = false;   // samples still queued for a plugin's pipe
    for (;;) {
        Block b;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            const auto ready = [this] { return stop_ || !queue_.empty() || reset_pending_.load(); };
            if (!pending) {
                q_cv_.wait(lk, ready);
            } else if (!q_cv_.wait_for(lk, std::chrono::milliseconds(2), ready)) {
                // no new block: keep the plugins' pipes full
                lk.unlock();
                pending = engine_->flush_pending();
                continue;
            }
            if (stop_) return;
            if (!queue_.empty()) {
                b = std::move(queue_.front());
                queue_.pop_front();
                queued_samples_ -= b.ns;
                have = true;
            }
        }
        if (reset_pending_.exchange(false)) engine_->reset_all();
        engine_->configure(mode(), options(), plugin_id());
        want_mc_.store(mode() != Mode::OFF && engine_->multichannel(), std::memory_order_relaxed);
        if (have && mode() != Mode::OFF) {
            if (b.am > 0) engine_->process_array(b.x.data(), b.am, b.ns, b.rate, b.rf, b.pos);
            else engine_->process(b.x.data(), b.x.size(), b.rate, b.rf, b.pos, b.m ? b.mc.data() : nullptr, b.m);
        }
        pending = engine_->flush_pending();
    }
}

std::string DigitalDecoder::active_plugin() const {
    const Mode m = mode();
    if (m == Mode::PLUGIN) return plugin_id();
    if (m == Mode::AUTO) return engine_->detected();
    return "";
}

bool DigitalDecoder::squelch_open(int64_t def_ms) const {
    const std::string id = active_plugin();
    if (id.empty()) return false;
    const int64_t t = engine_->last_valid_of(id);
    return t != 0 && now_ms() - t < engine_->squelch_hold_ms(id, def_ms);
}

bool DigitalDecoder::lead_talker(std::string* id, std::string* label) const {
    std::lock_guard<std::mutex> lk(lead_mu_);
    if (lead_tid_.empty()) return false;
    *id = lead_tid_;
    *label = lead_tlabel_;
    return true;
}

void DigitalDecoder::set_lead_talker(const std::string& id, const std::string& label) {
    std::lock_guard<std::mutex> lk(lead_mu_);
    lead_tid_ = id;
    lead_tlabel_ = label;
}

bool DigitalDecoder::frames_within(int64_t ms) const {
    const std::string id = active_plugin();
    if (id.empty()) return false;
    const int64_t t = engine_->last_valid_of(id);
    return t != 0 && now_ms() - t < ms;
}

std::string DigitalDecoder::status_json(uint64_t events_after, size_t max_events, bool tables) const {
    Engine::Status s = engine_->status();
    Mode m = mode();
    std::ostringstream o;
    char buf[64];
    o << "{\"mode\":\"" << json_escape(mode_string(m, plugin_id())) << "\",\"detected\":\"" << json_escape(s.detected)
      << "\",\"detected_name\":\"" << json_escape(s.detected_name) << "\"";
    const char* state = m == Mode::OFF ? "off" : (!s.detected.empty() ? "receiving" : "searching");
    o << ",\"state\":\"" << state << "\"";
    snprintf(buf, sizeof buf, "%.0f", s.offset_hz);
    o << ",\"offset_hz\":" << buf << ",\"rate\":" << static_cast<int>(s.rate);
    o << ",\"rate_ok\":" << ((s.rate == 0 || s.rate >= s.min_rate) ? "true" : "false");
    o << ",\"min_rate\":" << static_cast<int>(s.min_rate);
    o << ",\"frames\":{";
    bool first = true;
    for (const auto& f : s.frames) {
        o << (first ? "" : ",") << "\"" << json_escape(f.first) << "\":[" << f.second.first << "," << f.second.second << "]";
        first = false;
    }
    bool map_capable = false;
    for (const auto& p : s.plugins) map_capable |= p.map;
    o << "},\"dropped\":" << dropped_.load() << ",\"map_capable\":" << (map_capable ? "true" : "false")
      << ",\"map_points\":" << report_.map_size() << ",\"plugins\":[";
    for (size_t i = 0; i < s.plugins.size(); i++) {
        const auto& p = s.plugins[i];
        o << (i ? "," : "") << "{\"id\":\"" << json_escape(p.id) << "\",\"name\":\"" << json_escape(p.name)
          << "\",\"version\":\"" << json_escape(p.version) << "\",\"state\":\"" << json_escape(p.state)
          << "\",\"error\":\"" << json_escape(p.error) << "\",\"rate\":" << p.rate << ",\"dropped\":" << p.dropped
          << ",\"log\":[";
        for (size_t k = 0; k < p.tail.size(); k++) o << (k ? "," : "") << "\"" << json_escape(p.tail[k]) << "\"";
        o << "]}";
    }
    o << "]";
    {
        std::lock_guard<std::mutex> lk(v_mu_);
        o << ",\"voice\":{\"listening\":" << (voice_wanted() ? "true" : "false")
          << ",\"state\":\"" << json_escape(voice_state_) << "\"}";
    }
    Options op = options();
    o << ",\"opts\":{\"verbose\":" << (op.verbose ? "true" : "false") << ",\"log\":" << op.log << ",\"pos_s\":" << op.pos_s
      << ",\"map\":" << (op.map ? "true" : "false")
      << ",\"plugin\":{";
    first = true;
    for (const auto& kv : op.plugin) {
        o << (first ? "" : ",") << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
        first = false;
    }
    o << "}}";
    // the options' run-time choices (kp::Host::option_choices), keyed as opts.plugin;
    // {} = none: the page draws the declared list / text field
    o << ",\"choices\":" << report_.choices_json();
    // always when asked ({} = none): pages keep the last tables between pushes without them
    if (tables) {
        o << ",\"tables\":" << report_.tables_json() << ",\"plots\":" << report_.plots_json();
    }
    o << ",\"info\":" << report_.info_json() << ",\"events\":" << report_.events_json(events_after, max_events)
      << ",\"seq\":" << report_.last_seq() << "}";
    return o.str();
}

}  // namespace dig
