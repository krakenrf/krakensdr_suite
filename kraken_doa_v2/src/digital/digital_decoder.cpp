#include "digital/digital_decoder.hpp"
#include "digital/dig_protocols.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include <pthread.h>

#include <liquid/liquid.h>

#include "utils/json_escape.hpp"

namespace dig {

namespace {
constexpr float FSK_RATE = 48000.0f;     // 10 samples/symbol at 4800 Bd
constexpr float TETRA_RATE = 72000.0f;   // 4 samples/symbol at 18 kBd
constexpr float AFC_LIMIT_HZ = 4000.0f;
constexpr int64_t DETECT_WINDOW_MS = 4000;
constexpr size_t QUEUE_MAX_SECONDS = 1;
}  // namespace

float DigitalDecoder::min_rate_for(Mode m) {
    switch (m) {
        case Mode::TETRA:
        case Mode::AUTO: return 24000.0f;   // TETRA occupies ~22 kHz
        case Mode::OFF: return 0.0f;
        default: return 12000.0f;
    }
}

// ---------------------------------------------------------------------------
// Engine: front end + receivers. Owned and driven by one thread at a time.
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine(Report& r, DigitalDecoder* owner) : report_(r) {
        ctx_.report = &report_;
        ctx_.opts = &opts_;
        ctx_.voice = [owner](Mode, const float* x, size_t n) { owner->push_voice(x, n); };
        ctx_.voice_state = [owner](const std::string& st) { owner->set_voice_state(st); };
        ctx_.voice_wanted = [owner] { return owner->voice_wanted(); };
        ctx_.valid = [this](Mode m) { on_valid(m); };
        ctx_.freq_error = [this](Mode m, float hz) { on_freq_error(m, hz); };
        fsk_ = std::make_unique<Fsk4Receiver>(ctx_);
        dstar_ = std::make_unique<DstarReceiver>(ctx_);
        tetra_ = std::make_unique<TetraReceiver>(ctx_);
        nxdn_ = std::make_unique<NxdnReceiver>(ctx_);
        mpt_ = std::make_unique<Mpt1327Receiver>(ctx_);
    }
    ~Engine() { destroy_frontend(); }

    void configure(Mode m, const Options& o) {
        if (m != mode_) {
            mode_ = m;
            reset_receivers();
            std::lock_guard<std::mutex> lk(st_mu_);
            detected_ = Mode::OFF;
        }
        opts_ = o;
        fsk_->set_enabled(m == Mode::AUTO || m == Mode::P25, m == Mode::AUTO || m == Mode::DMR);
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
        if (rate != in_rate_) build_frontend(rate);
        const bool want_fsk = mode_ == Mode::AUTO || mode_ == Mode::P25 || mode_ == Mode::DMR || mode_ == Mode::DSTAR ||
                              mode_ == Mode::NXDN || mode_ == Mode::MPT1327;
        const bool want_tetra = mode_ == Mode::AUTO || mode_ == Mode::TETRA;

        // AFC mixer at the input rate
        mixed_.resize(n);
        const float w = -2.0f * static_cast<float>(M_PI) * afc_hz_ / in_rate_;
        for (size_t i = 0; i < n; i++) {
            std::complex<float> v = opts_.invert ? std::conj(x[i]) : x[i];
            mixed_[i] = v * std::polar(1.0f, static_cast<float>(nco_phase_));
            nco_phase_ += w;
        }
        nco_phase_ = std::remainder(nco_phase_, 2.0 * M_PI);

        if (want_fsk) {
            rs_out_.resize(static_cast<size_t>(std::ceil(n * FSK_RATE / in_rate_)) + 64);
            unsigned int ny = 0;
            msresamp_crcf_execute(rs_fsk_, mixed_.data(), static_cast<unsigned int>(n), rs_out_.data(), &ny);
            disc_.resize(ny);
            for (unsigned int i = 0; i < ny; i++) {
                std::complex<float> y;
                firfilt_crcf_push(lp_fsk_, rs_out_[i]);
                firfilt_crcf_execute(lp_fsk_, &y);
                disc_[i] = std::arg(y * std::conj(prev_)) * (FSK_RATE / (2.0f * static_cast<float>(M_PI)));
                prev_ = y;
            }
            run_fsk(disc_.data(), disc_.size());
        }
        if (want_tetra) {
            rs_out_.resize(static_cast<size_t>(std::ceil(n * TETRA_RATE / in_rate_)) + 64);
            unsigned int ny = 0;
            msresamp_crcf_execute(rs_tetra_, mixed_.data(), static_cast<unsigned int>(n), rs_out_.data(), &ny);
            tet_.resize(ny);
            for (unsigned int i = 0; i < ny; i++) {
                firfilt_crcf_push(rrc_tetra_, rs_out_[i]);
                firfilt_crcf_execute(rrc_tetra_, &tet_[i]);
            }
            tetra_->process(tet_.data(), tet_.size());
        }
        housekeeping();
    }

    // Everything back to a cold start, without "signal lost" events (VFO
    // retune, decoder switched off and on)
    void reset_all() {
        reset_receivers();
        std::lock_guard<std::mutex> lk(st_mu_);
        detected_ = Mode::OFF;
        for (auto& t : total_) t = 0;
        last_valid_ms_ = 0;
    }

    void process_disc(const float* hz, size_t n) {
        run_fsk(hz, n);
        housekeeping();
    }

    struct Status {
        Mode detected = Mode::OFF;
        float offset_hz = 0;
        float rate = 0;
        int frames[NUM_PROTOCOLS] = {0, 0, 0, 0};
        uint64_t total[NUM_PROTOCOLS] = {0, 0, 0, 0};
    };
    Status status() const {
        std::lock_guard<std::mutex> lk(st_mu_);
        Status s;
        s.detected = detected_;
        s.offset_hz = afc_hz_ + residual_hz_;
        s.rate = in_rate_;
        int64_t t = now_ms();
        for (const auto& v : valid_)
            if (t - v.first < DETECT_WINDOW_MS) s.frames[static_cast<int>(v.second) - 2]++;
        for (int i = 0; i < NUM_PROTOCOLS; i++) s.total[i] = total_[i];
        return s;
    }

private:
    Report& report_;
    Options opts_;
    RxContext ctx_;
    Mode mode_ = Mode::OFF;
    std::unique_ptr<Fsk4Receiver> fsk_;
    std::unique_ptr<DstarReceiver> dstar_;
    std::unique_ptr<TetraReceiver> tetra_;
    std::unique_ptr<NxdnReceiver> nxdn_;
    std::unique_ptr<Mpt1327Receiver> mpt_;

    float in_rate_ = 0;
    double rf_hz_ = 0;
    msresamp_crcf rs_fsk_ = nullptr, rs_tetra_ = nullptr;
    firfilt_crcf lp_fsk_ = nullptr, rrc_tetra_ = nullptr;
    std::complex<float> prev_{1.0f, 0.0f};
    double nco_phase_ = 0;
    std::vector<std::complex<float>> mixed_, rs_out_, tet_;
    std::vector<float> disc_;

    // detection / AFC state (status() reads it from another thread)
    mutable std::mutex st_mu_;
    std::deque<std::pair<int64_t, Mode>> valid_;
    uint64_t total_[NUM_PROTOCOLS] = {0, 0, 0, 0};
    Mode detected_ = Mode::OFF;
    float afc_hz_ = 0;
    float residual_hz_ = 0;
    int64_t last_valid_ms_ = 0;

    void run_fsk(const float* d, size_t n) {
        if (mode_ == Mode::AUTO || mode_ == Mode::P25 || mode_ == Mode::DMR) fsk_->process(d, n);
        if (mode_ == Mode::AUTO || mode_ == Mode::DSTAR) dstar_->process(d, n);
        if (mode_ == Mode::AUTO || mode_ == Mode::NXDN) nxdn_->process(d, n);
        if (mode_ == Mode::AUTO || mode_ == Mode::MPT1327) mpt_->process(d, n);
    }

    void destroy_frontend() {
        if (rs_fsk_) msresamp_crcf_destroy(rs_fsk_);
        if (rs_tetra_) msresamp_crcf_destroy(rs_tetra_);
        if (lp_fsk_) firfilt_crcf_destroy(lp_fsk_);
        if (rrc_tetra_) firfilt_crcf_destroy(rrc_tetra_);
        rs_fsk_ = rs_tetra_ = nullptr;
        lp_fsk_ = rrc_tetra_ = nullptr;
    }

    void build_frontend(float rate) {
        destroy_frontend();
        in_rate_ = rate;
        rs_fsk_ = msresamp_crcf_create(FSK_RATE / rate, 60.0f);
        rs_tetra_ = msresamp_crcf_create(TETRA_RATE / rate, 60.0f);
        // 12.5 kHz channel filter (4FSK +-1.9 kHz deviation + 2.4 kHz
        // modulation bandwidth, plus room for a residual offset)
        float h[63];
        liquid_firdes_kaiser(63, 6500.0f / FSK_RATE, 60.0f, 0.0f, h);
        float s = 0;
        for (float v : h) s += v;
        for (float& v : h) v /= s;
        lp_fsk_ = firfilt_crcf_create(h, 63);
        // TETRA matched filter: RRC 0.35, 4 samples/symbol
        float r[49];
        liquid_firdes_rrcos(4, 6, 0.35f, 0.0f, r);
        rrc_tetra_ = firfilt_crcf_create(r, 49);
        reset_receivers();
    }

    void reset_receivers() {
        fsk_->reset();
        dstar_->reset();
        tetra_->reset();
        nxdn_->reset();
        mpt_->reset();
        prev_ = {1.0f, 0.0f};
        std::lock_guard<std::mutex> lk(st_mu_);
        afc_hz_ = 0;
        residual_hz_ = 0;
        valid_.clear();
    }

    void on_valid(Mode m) {
        std::lock_guard<std::mutex> lk(st_mu_);
        int64_t t = now_ms();
        valid_.emplace_back(t, m);
        total_[static_cast<int>(m) - 2]++;
        last_valid_ms_ = t;
    }

    void on_freq_error(Mode m, float hz) {
        std::lock_guard<std::mutex> lk(st_mu_);
        Mode lead = is_protocol(mode_) ? mode_ : detected_;
        if (m != lead) return;
        residual_hz_ = hz;
        // slow integrator: the receivers already tolerate the residual
        afc_hz_ = std::clamp(afc_hz_ + 0.2f * hz, -AFC_LIMIT_HZ, AFC_LIMIT_HZ);
    }

    void housekeeping() {
        int64_t t = now_ms();
        Mode old, now;
        {
            std::lock_guard<std::mutex> lk(st_mu_);
            while (!valid_.empty() && t - valid_.front().first > DETECT_WINDOW_MS) valid_.pop_front();
            int cnt[NUM_PROTOCOLS] = {0, 0, 0, 0};
            for (const auto& v : valid_) cnt[static_cast<int>(v.second) - 2]++;
            old = detected_;
            if (is_protocol(mode_)) {
                detected_ = cnt[static_cast<int>(mode_) - 2] > 0 ? mode_ : Mode::OFF;
            } else {
                int best = -1, bc = 0;
                for (int i = 0; i < NUM_PROTOCOLS; i++)
                    if (cnt[i] > bc) { bc = cnt[i]; best = i; }
                // switch only on a clear majority (2+ frames), keep the
                // current one while it is still receiving
                if (detected_ != Mode::OFF && cnt[static_cast<int>(detected_) - 2] > 0 &&
                    cnt[static_cast<int>(detected_) - 2] * 2 >= bc) {
                    // keep
                } else {
                    detected_ = bc >= 2 ? static_cast<Mode>(best + 2) : Mode::OFF;
                }
            }
            now = detected_;
            if (now == Mode::OFF && t - last_valid_ms_ > 5000) {
                afc_hz_ *= 0.99f;   // drift back to the VFO centre when idle
                residual_hz_ = 0;
            }
        }
        if (now != old) {
            if (now != Mode::OFF)
                report_.event(now, std::string(mode_ == Mode::AUTO ? "Detected " : "Receiving ") + mode_name(now), 0.0);
            else
                report_.event(old, "Signal lost", 0.0);
        }
    }
};

// ---------------------------------------------------------------------------
// DigitalDecoder
// ---------------------------------------------------------------------------
DigitalDecoder::DigitalDecoder(bool threaded) : engine_(std::make_unique<Engine>(report_, this)) {
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

void DigitalDecoder::set_mode(Mode m) {
    Mode old = mode_.exchange(m);
    if (old != m) {
        reset_pending_.store(true);
        report_.clear_all();
        if (m != Mode::OFF)
            report_.event(m == Mode::AUTO ? Mode::AUTO : m, std::string("Decoder ") +
                          (m == Mode::AUTO ? "on, detecting the mode automatically" : std::string("set to ") + mode_name(m)), 0.0);
    }
    if (!worker_.joinable()) engine_->configure(m, options());
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
    engine_->configure(mode(), options());
    engine_->process(x, n, rate_hz, rf_hz);
}

void DigitalDecoder::process_discriminator(const float* hz, size_t n) {
    engine_->configure(mode(), options());
    engine_->process_disc(hz, n);
}

void DigitalDecoder::run() {
    for (;;) {
        Block b;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            b = std::move(queue_.front());
            queue_.pop_front();
            queued_samples_ -= b.x.size();
        }
        if (reset_pending_.exchange(false)) engine_->reset_all();
        engine_->configure(mode(), options());
        if (mode() != Mode::OFF) engine_->process(b.x.data(), b.x.size(), b.rate, b.rf);
    }
}

std::string DigitalDecoder::status_json(uint64_t events_after, size_t max_events) const {
    Engine::Status s = engine_->status();
    Mode m = mode();
    std::ostringstream o;
    char buf[64];
    o << "{\"mode\":\"" << mode_name(m) << "\",\"detected\":\"" << (s.detected == Mode::OFF ? "" : mode_name(s.detected)) << "\"";
    const char* state = m == Mode::OFF ? "off" : (s.detected != Mode::OFF ? "receiving" : "searching");
    o << ",\"state\":\"" << state << "\"";
    snprintf(buf, sizeof buf, "%.0f", s.offset_hz);
    o << ",\"offset_hz\":" << buf << ",\"rate\":" << static_cast<int>(s.rate);
    float need = min_rate_for(m);
    o << ",\"rate_ok\":" << ((s.rate == 0 || s.rate >= need) ? "true" : "false");
    o << ",\"min_rate\":" << static_cast<int>(need);
    o << ",\"frames\":{";
    for (int i = 0; i < NUM_PROTOCOLS; i++)
        o << (i ? "," : "") << "\"" << mode_name(static_cast<Mode>(i + 2)) << "\":[" << s.frames[i] << "," << s.total[i] << "]";
    o << "},\"dropped\":" << dropped_.load();
    {
        std::lock_guard<std::mutex> lk(v_mu_);
        o << ",\"voice\":{\"listening\":" << (voice_wanted() ? "true" : "false")
          << ",\"state\":\"" << json_escape(voice_state_) << "\"}";
    }
    o << ",\"codecs\":{\"IMBE\":\"built in\",\"AMBE\":\"" << json_escape(MbeLib::instance().status())
      << "\",\"AMBE_ok\":" << (MbeLib::instance().available() ? "true" : "false")
      << ",\"ACELP\":\"" << json_escape(TetraCodec::status()) << "\",\"ACELP_ok\":"
      << (TetraCodec::available() ? "true" : "false") << "}";
    Options op = options();
    o << ",\"opts\":{\"verbose\":" << (op.verbose ? "true" : "false") << ",\"dmr_slot\":" << op.dmr_slot
      << ",\"p25_nac\":" << op.p25_nac << ",\"invert\":" << (op.invert ? "true" : "false") << "}";
    // facts of the detected protocol (or the fixed one), plus any others
    o << ",\"info\":{";
    bool first = true;
    for (int i = 0; i < NUM_PROTOCOLS; i++) {
        Mode p = static_cast<Mode>(i + 2);
        std::string j = report_.info_json(p);
        if (j == "[]") continue;
        o << (first ? "" : ",") << "\"" << mode_name(p) << "\":" << j;
        first = false;
    }
    o << "},\"events\":" << report_.events_json(events_after, max_events) << ",\"seq\":" << report_.last_seq() << "}";
    return o.str();
}

}  // namespace dig
