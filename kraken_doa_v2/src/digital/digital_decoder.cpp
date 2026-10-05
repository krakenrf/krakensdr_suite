#include "digital/digital_decoder.hpp"
#include "digital/dig_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <liquid/liquid.h>

#include "utils/json_escape.hpp"

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
constexpr uint32_t W_SAMPLES = 1, W_OPTION = 2, W_RESET = 3, W_VOICE_WANTED = 4, W_SYNC = 5, W_FACT = 16,
                   W_EVENT = 17, W_VALID = 18, W_AUDIO = 19, W_VOICE_STATE = 20, W_FREQ_ERROR = 21,
                   W_SYNC_DONE = 22;

int64_t mtime_ms(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}
}  // namespace

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
    int verbose_sent = -1, voice_sent = -1;
    std::map<std::string, std::string> opts_sent;   // key (without the id prefix) -> value
    uint32_t sync_sent = 0, sync_done = 0;
    std::string state = "starting", error;
    std::vector<std::string> tail;
    uint64_t dropped = 0;
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

    void process(const std::complex<float>* x, size_t n, float rate, double rf) {
        if (rate <= 0 || n == 0) return;
        if (std::fabs(rf - rf_hz_) > 100.0) {
            if (rf_hz_ != 0.0) {
                reset_all();
                report_.clear_all();
            }
            rf_hz_ = rf;
        }
        if (rate != in_rate_) {
            in_rate_ = rate;
            rebuild_resamplers();
        }
        const int64_t t = now_ms();
        if (t - check_ms_ > CHECK_MS) {
            check_ms_ = t;
            periodic(t);
        }
        // AFC mixer at the input rate
        mixed_.resize(n);
        const float w = -2.0f * static_cast<float>(M_PI) * afc_hz_ / in_rate_;
        for (size_t i = 0; i < n; i++) {
            std::complex<float> v = opts_.invert ? std::conj(x[i]) : x[i];
            mixed_[i] = v * std::polar(1.0f, static_cast<float>(nco_phase_));
            nco_phase_ += w;
        }
        nco_phase_ = std::remainder(nco_phase_, 2.0 * M_PI);
        resample(n);
        for (size_t i = 0; i < runners_.size(); i++) {
            const Resampled* r = rate_buf(*runners_[i]);
            run(i, r ? r->out.data() : nullptr, r ? r->ny : 0);
        }
        housekeeping();
    }

    // Everything back to a cold start (VFO retune, decoder switched off and
    // on): the plugins get a RESET, detection starts over
    void reset_all() {
        for (auto& r : runners_)
            if (r->proc && r->proc->running()) r->proc->send(W_RESET, nullptr, 0, 0);
        std::lock_guard<std::mutex> lk(st_mu_);
        detected_.clear();
        valid_.clear();
        totals_.clear();
        last_valid_ms_ = 0;
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
    };
    struct Status {
        std::string detected, detected_name;
        float offset_hz = 0;
        float rate = 0;
        float min_rate = 0;
        std::map<std::string, std::pair<int, uint64_t>> frames;   // id -> (4 s window, total)
        std::vector<PluginStatus> plugins;
    };
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
    double nco_phase_ = 0;
    std::vector<std::complex<float>> mixed_;
    struct Resampled {
        float rate = 0;
        msresamp_crcf q = nullptr;     // nullptr = same rate as the input
        std::vector<std::complex<float>> out;
        unsigned int ny = 0;
    };
    std::vector<Resampled> rs_;

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
    std::vector<PluginStatus> status_plugins_;

    // --- runner set ---------------------------------------------------------
    // all plugins get EOF at once and exit in parallel (a decoder in Auto
    // detect runs several; this can run on the uWS thread when a VFO is removed)
    void stop_all() {
        for (auto& r : runners_)
            if (r->proc) r->proc->request_stop();
        for (auto& r : runners_)
            if (r->proc) r->proc->stop();
        runners_.clear();
    }

    void rebuild_runners() {
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
            for (const auto& p : reg.list())
                if (p.built && p.auto_detect) ids.push_back(p.id);
            if (ids.empty()) report_.event("", "Auto detect: no auto-detect decoder plugins are built (run make)", 0.0);
        }
        for (const auto& id : ids) {
            auto r = std::make_unique<Runner>();
            r->id = id;
            r->known = reg.get(id, &r->info);
            report_.set_label(id, r->known ? r->info.name : id);
            runners_.push_back(std::move(r));
        }
        rebuild_resamplers();
        publish_status();
    }

    void destroy_resamplers() {
        for (auto& r : rs_)
            if (r.q) msresamp_crcf_destroy(r.q);
        rs_.clear();
    }

    static float rate_of(const Runner& rn) { return rn.known ? static_cast<float>(rn.info.sample_rate) : 48000.0f; }

    void rebuild_resamplers() {
        destroy_resamplers();
        if (in_rate_ <= 0) return;
        for (auto& rn : runners_) {
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
            p.rate = r->known ? r->info.sample_rate : 0;
            p.min_vfo_rate = r->known ? r->info.min_vfo_rate : 12500;
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
        const bool rebuilt = r.note == "rebuilt";
        const bool rate_changed = !r.known || pi.sample_rate != r.info.sample_rate;
        r.info = pi;
        r.known = true;
        report_.set_label(r.id, pi.name);
        if (rate_changed) rebuild_resamplers();
        r.proc = std::make_unique<PluginProcess>();
        std::string err;
        if (!r.proc->start(pi.exe, &err)) {
            r.proc.reset();
            return problem(r, "error", pi.name + ": " + err, PLUGIN_RETRY_MS);
        }
        r.started_ms = t;
        r.verbose_sent = r.voice_sent = -1;
        r.opts_sent.clear();
        r.sync_sent = r.sync_done = 0;
        r.state = "running";
        r.error.clear();
        // a fixed decoder says it started; in Auto detect only restarts are news
        if (rebuilt) report_.event(r.id, "Rebuilt plugin loaded: " + pi.name + " " + pi.version, 0.0);
        else if (!r.note.empty()) report_.event(r.id, pi.name + " restarted", 0.0);
        else if (mode_ == Mode::PLUGIN) report_.event(r.id, "Plugin started: " + pi.name + " " + pi.version, 0.0);
        r.note.clear();
        publish_status();
    }

    void send_settings(Runner& r) {
        const int verbose = opts_.verbose ? 1 : 0;
        if (verbose != r.verbose_sent) {
            std::string kv = std::string("verbose=") + (verbose ? "1" : "0");
            r.proc->send(W_OPTION, kv.data(), kv.size(), 0);
            r.verbose_sent = verbose;
        }
        // only the decoder whose voice is played runs its vocoder
        const int voice = (is_lead(r) && owner_->voice_wanted()) ? 1 : 0;
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

    void run(size_t idx, const std::complex<float>* y, unsigned int ny) {
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
        const size_t backlog = sync_io_ ? SIZE_MAX : static_cast<size_t>(r.info.sample_rate) * 8;   // 1 s
        for (size_t p = 0; p < ny; p += PLUGIN_CHUNK) {
            size_t k = std::min<size_t>(PLUGIN_CHUNK, ny - p);
            if (!r.proc->send(W_SAMPLES, y + p, k * sizeof(std::complex<float>), backlog)) r.dropped += k;
        }
        auto handler = [this, idx](uint32_t type, const char* p, size_t len) { on_message(idx, type, p, len); };
        if (sync_io_) r.proc->flush_blocking(handler, 60000);
        else r.proc->poll(handler);
        int st = 0;
        const bool bad = r.proc->protocol_error();
        if (bad || !r.proc->alive(&st)) {
            r.proc->poll(handler);   // whatever it said last
            std::string why;
            if (bad) why = "sent a malformed message (printing to stdout?)";
            else if (WIFSIGNALED(st)) why = std::string("crashed (") + strsignal(WTERMSIG(st)) + ")";
            else why = "exited with status " + std::to_string(WEXITSTATUS(st));
            r.tail = r.proc->stderr_tail();
            if (!r.tail.empty()) why += " - last output: " + r.tail.back();
            r.proc->stop();
            r.proc.reset();
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
                report_.event(r.id, text, std::isfinite(d) ? std::clamp<double>(d, 0.0, 3600.0) : 2.0);
                break;
            }
            case W_VALID:
                on_valid(r.id);
                break;
            case W_AUDIO:
                if (is_lead(r) && owner_->voice_wanted() && len >= 4) {
                    std::vector<float> a(len / 4);
                    memcpy(a.data(), p, a.size() * 4);
                    for (float& v : a)
                        if (!std::isfinite(v)) v = 0;
                    owner_->push_voice(a.data(), a.size());
                }
                break;
            case W_VOICE_STATE:
                if (is_lead(r)) owner_->set_voice_state(std::string(p, std::min<size_t>(len, 200)));
                break;
            case W_FREQ_ERROR:
                if (is_lead(r) && len >= 4) {
                    float hz;
                    memcpy(&hz, p, 4);
                    if (std::isfinite(hz)) on_freq_error(std::clamp(hz, -AFC_LIMIT_HZ, AFC_LIMIT_HZ));
                }
                break;
            case W_SYNC_DONE:
                if (len >= 4) memcpy(&r.sync_done, p, 4);
                break;
            default:
                break;
        }
    }

    // --- detection / AFC ----------------------------------------------------
    void on_valid(const std::string& id) {
        std::lock_guard<std::mutex> lk(st_mu_);
        int64_t t = now_ms();
        valid_.emplace_back(t, id);
        totals_[id]++;
        last_valid_ms_ = t;
    }

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
                afc_hz_ *= 0.99f;   // drift back to the VFO centre when idle
                residual_hz_ = 0;
            }
        }
        if (now != old) {
            if (!now.empty())
                report_.event(now, std::string(mode_ == Mode::AUTO ? "Detected " : "Receiving ") + name_of(now), 0.0);
            else
                report_.event(old, "Signal lost", 0.0);
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
}

Options DigitalDecoder::options() const {
    std::lock_guard<std::mutex> lk(opt_mu_);
    return opts_;
}

void DigitalDecoder::push(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz) {
    if (mode() == Mode::OFF || n == 0) return;
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        queue_.push_back({std::vector<std::complex<float>>(x, x + n), rate_hz, rf_hz});
        queued_samples_ += n;
        // decoder fell behind (CPU starved): drop the oldest data
        while (queued_samples_ > static_cast<size_t>(rate_hz) * QUEUE_MAX_SECONDS && queue_.size() > 1) {
            queued_samples_ -= queue_.front().x.size();
            queue_.pop_front();
            dropped_++;
        }
    }
    q_cv_.notify_one();
}

void DigitalDecoder::process(const std::complex<float>* x, size_t n, float rate_hz, double rf_hz) {
    if (reset_pending_.exchange(false)) engine_->reset_all();
    engine_->configure(mode(), options(), plugin_id());
    engine_->process(x, n, rate_hz, rf_hz);
}

void DigitalDecoder::drain(int timeout_ms) { engine_->drain(timeout_ms); }

void DigitalDecoder::run() {
    for (;;) {
        Block b;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [this] { return stop_ || !queue_.empty() || reset_pending_.load(); });
            if (stop_) return;
            if (!queue_.empty()) {
                b = std::move(queue_.front());
                queue_.pop_front();
                queued_samples_ -= b.x.size();
                have = true;
            }
        }
        if (reset_pending_.exchange(false)) engine_->reset_all();
        engine_->configure(mode(), options(), plugin_id());
        if (have && mode() != Mode::OFF) engine_->process(b.x.data(), b.x.size(), b.rate, b.rf);
    }
}

std::string DigitalDecoder::status_json(uint64_t events_after, size_t max_events) const {
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
    o << "},\"dropped\":" << dropped_.load() << ",\"plugins\":[";
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
    o << ",\"opts\":{\"verbose\":" << (op.verbose ? "true" : "false") << ",\"invert\":" << (op.invert ? "true" : "false")
      << ",\"plugin\":{";
    first = true;
    for (const auto& kv : op.plugin) {
        o << (first ? "" : ",") << "\"" << json_escape(kv.first) << "\":\"" << json_escape(kv.second) << "\"";
        first = false;
    }
    o << "}}";
    o << ",\"info\":" << report_.info_json() << ",\"events\":" << report_.events_json(events_after, max_events)
      << ",\"seq\":" << report_.last_seq() << "}";
    return o.str();
}

}  // namespace dig
