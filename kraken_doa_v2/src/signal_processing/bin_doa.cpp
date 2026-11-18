// 🎨 Bearing spectrum - see include/signal_processing/bin_doa.hpp and
// docs/dev/bearing-spectrum.md (design, estimator, limits, measurements).

#include "signal_processing/bin_doa.hpp"

#include "config.hpp"
#include "globals.hpp"
#include "decimator_manager.hpp"
#include "message_builders.hpp"
#include "kraken_fft.hpp"
#include "networking/websocket_server.hpp"
#include "utils/parse_num.hpp"
#include "utils/json_escape.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace bin_doa {
namespace {

using cf = std::complex<float>;

// The bearing FFT: the spectrum's FFT size, at most MAX_FFT bins (the cross-
// spectra of every bin are kept: M^2 floats each, and they should stay in
// the Pi's L2 cache - measurements in bearing-spectrum.md)
constexpr int MIN_FFT = 1024;
constexpr int MAX_FFT = 8192;
// A snapshot every TICK_S of stream (the page's 20 frames a second)
constexpr double TICK_S = 0.05;
// The covariance average's time constant: the spectrum trace's (one FFT per
// packet, weight averaging_alpha), within these bounds - shorter has too few
// snapshots per bin for a covariance of M antennas, longer smears a burst
constexpr double TAU_MIN_S = 0.1, TAU_MAX_S = 2.0;
// CPU budget of the worker (share of one core, by its own timing): blocks
// past it are skipped (the coverage in the state message says how many).
// Estimated need: ~15 % of a Pi 5 core, ~30 % of a Pi 4's (bearing-spectrum.md)
constexpr double CPU_SHARE = 0.25;
// The noise test: rho = lambda1 (M - 1) / (trace - lambda1) of the whitened
// covariance must pass the white-noise 99 % point for K snapshots,
// ~1 + 3 sqrt(M / K) + 3 M / K (simulated, M 3-8, K 2M - 4096: within
// -18 / +40 %), with this margin; full confidence DET_SPAN_DB above it
constexpr double NOISE_MARGIN = 1.15;
constexpr double DET_SPAN_DB = 6.0;
// The steering fit |a^H v|^2 (both unit norm) of the best bearing: a plane
// wave fits ~1, noise or a mix of directions less - confidence 0 at FIT_LO,
// full at FIT_HI
constexpr double FIT_LO = 0.5, FIT_HI = 0.8;
// packets held back before use (see Worker::handle): heimdall's noise flag
// trails the samples by 0-2 packets
constexpr size_t HOLD_PACKETS = 3;
// A point's power since the last snapshot over its noise (build_frame): no
// colour below 1 + RECENT_LO standard deviations of that power, full from
// 1 + RECENT_HI
constexpr double RECENT_LO = 2.0, RECENT_HI = 5.0;

std::atomic<bool> g_on{false};
std::atomic<int> g_viewers{0};

// --- the queue: packets for the worker. The decimation thread hands them
// on in bursts (a one-slot mailbox lost 3 of 4 in the test); past QUEUE_MAX
// the oldest is dropped (a gap - counted against the coverage) ---
struct Job {
    std::vector<std::vector<cf>> iq;
    int elements = 0;
    uint64_t seq = 0;
    double center_hz = 0;
    const char* park = nullptr;
};
constexpr size_t QUEUE_MAX = 8;
std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Job> g_queue;
std::atomic<int64_t> g_dropped_samples{0};
std::thread* g_thread = nullptr;   // never destroyed (the process ends with fast_exit)
std::atomic<bool> g_stop{false};

// --- the array model, as the bearing search uses it ---
struct Steer {
    int M = 0, n = 0;
    float res = 1.0f, offset = 0.0f;
    int topology = 0;          // 0 UCA, 1 ULA, 2 custom
    uint8_t flags = 0;         // 1 = ULA front half assumed, 2 = 3D (horizon)
    double freq_hz = 0;
    std::vector<cf> conjA;     // angle-major: conj(a_i[m]) at [i * M + m]
    std::vector<uint8_t> keep;
};

// --- a published snapshot (the uWS thread reads it) ---
struct Grid {
    int nb = 0, M = 0;
    double center_hz = 0, f0_hz = 0, step_hz = 0;
    double k_time = 0;                   // effective snapshots per bin (the average's weights)
    float w[MAX_CHANNELS] = {};          // 1 / sqrt(each antenna's noise) - the whitening
    std::vector<float> rows;             // M^2 rows of nb: diagonals, then re / im of each pair a < b
    double s1 = 0;                       // the weights' sum (rows / s1 = per block)
    std::vector<float> recent;           // M rows of nb: each antenna's power in the last recent_n blocks
    int recent_n = 0;
    std::shared_ptr<const Steer> steer;
    uint64_t gen = 0;                    // every snapshot its own (the frame cache's key)
};
std::mutex g_pub_mu;
std::shared_ptr<const Grid> g_pub;

// --- the state message ---
std::mutex g_state_mu;
std::string g_state_text = "off", g_state_reason;
int g_state_nb = 0, g_state_M = 0, g_state_cov = 0;
int64_t g_state_sent_ms = 0;
std::string g_state_json_sent;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string state_json_locked() {
    std::ostringstream o;
    o << "{\"bin_doa\":{\"on\":" << (g_on.load() ? "true" : "false") << ",\"state\":\"" << g_state_text
      << "\",\"reason\":\"" << json_escape(g_state_reason) << "\",\"fft\":" << g_state_nb
      << ",\"elements\":" << g_state_M << ",\"coverage\":" << g_state_cov << "}}";
    return o.str();
}

// a new state / reason / coverage: told to every page (a coverage change at
// most once a second)
void set_state(const char* state, const std::string& reason, int nb, int M, int cov) {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_state_mu);
        const bool big = g_state_text != state || g_state_reason != reason || g_state_nb != nb || g_state_M != M;
        const int64_t t = now_ms();
        if (!big && (std::abs(cov - g_state_cov) < 5 || t - g_state_sent_ms < 1000)) return;
        g_state_text = state;
        g_state_reason = reason;
        g_state_nb = nb;
        g_state_M = M;
        g_state_cov = cov;
        g_state_sent_ms = t;
        msg = state_json_locked();
        if (msg == g_state_json_sent) return;
        g_state_json_sent = msg;
    }
    WebSocketServer::broadcast_json_message(msg);
}

void publish(std::shared_ptr<const Grid> g) {
    std::lock_guard<std::mutex> lk(g_pub_mu);
    g_pub = std::move(g);
}

// The model's steering at freq_hz: the first VFO's MUSIC processor (every
// VFO's has the same array settings - the control handler sets them all)
std::shared_ptr<const Steer> fetch_steer(double freq_hz, int M, std::string* why) {
    MUSICProcessor::AzimuthSteering st;
    bool ok = false;
    for (const auto& d : decimator_manager.getAllDecimators()) {
        if (!d || !d->music_processor || d->being_deleted.load(std::memory_order_relaxed)) continue;
        try {
            ok = d->music_processor->azimuthSteering(freq_hz, &st);
        } catch (const std::exception&) {
            ok = false;   // (Eigen allocation) - as no steering
        }
        break;
    }
    if (!ok) { *why = "the array is not set up (custom positions, or a UCA of 3+ elements)"; return nullptr; }
    if (st.M != M || st.A.rows() != M || st.A.cols() != st.num_angles || st.num_angles < 4) {
        *why = "the array model has another element count";
        return nullptr;
    }
    auto s = std::make_shared<Steer>();
    s->M = M;
    s->n = st.num_angles;
    s->res = st.resolution_deg;
    s->offset = st.offset_deg;
    s->freq_hz = freq_hz;
    s->topology = st.topology == ArrayTopology::ULA ? 1 : st.topology == ArrayTopology::CUSTOM ? 2 : 0;
    s->keep = st.keep;
    if (s->keep.size() != static_cast<size_t>(s->n)) s->keep.assign(static_cast<size_t>(s->n), 1);
    // a ULA can't tell front from back: without a half-plane setting the
    // front half (as MUSIC's FORWARD) - else every bearing has two answers
    if (st.topology == ArrayTopology::ULA && st.half == ULAOutputMode::BOTH) {
        for (int i = 0; i < s->n; i++) {
            const float a = i * s->res;
            s->keep[i] = (a <= 90.0f || a >= 270.0f) ? 1 : 0;
        }
        s->flags |= 1;
    }
    if (st.is3d) s->flags |= 2;
    s->conjA.resize(static_cast<size_t>(s->n) * M);
    for (int i = 0; i < s->n; i++)
        for (int m = 0; m < M; m++)
            s->conjA[static_cast<size_t>(i) * M + m] = cf(static_cast<float>(st.A(m, i).real()),
                                                          static_cast<float>(-st.A(m, i).imag()));
    return s;
}

// ============================================================================
// The worker: blocks of nb samples of every antenna -> windowed FFTs -> each
// bin's cross-spectra added to the running covariances
// ============================================================================
class Worker {
public:
    void run() {
        while (!g_stop.load()) {
            Job job;
            bool got = false;
            {
                std::unique_lock<std::mutex> lk(g_mu);
                g_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return !g_queue.empty() || g_stop.load(); });
                if (g_stop.load()) break;
                if (!g_queue.empty()) {
                    job = std::move(g_queue.front());
                    g_queue.pop_front();
                    got = true;
                }
            }
            if (!got) {
                // no packets (the mode is off, nobody watches, no stream)
                if (!g_on.load()) idle("off", "");
                else if (g_viewers.load() <= 0) idle("idle", "no page shows the spectrum");
                else if (now_ms() - last_job_ms_ > 1000) idle("idle", "no data from heimdall");
                continue;
            }
            last_job_ms_ = now_ms();
            handle(job);
        }
    }

private:
    // accumulation state
    int nb_ = 0, M_ = 0;
    double center_hz_ = 0;
    std::vector<float> rows_;
    double s1_ = 0, s2_ = 0;               // the average's weights: sum w, sum w^2
    std::vector<float> recent_;            // M rows of nb: each antenna's power since the last snapshot
    int recent_n_ = 0;                     // ... in this many blocks
    std::vector<std::vector<cf>> stash_;   // samples not yet in a block
    std::deque<Job> delay_;                // the newest packets, not used yet (handle)
    uint64_t last_seq_ = 0;
    bool seq_seen_ = false;
    size_t since_tick_ = 0;
    int64_t last_job_ms_ = 0;
    // the FFT
    kfft_plan plan_ = nullptr;
    kfft_complex* in_ = nullptr;
    kfft_complex* out_ = nullptr;
    std::vector<float> win_;
    std::vector<float> re_, im_;           // M x nb, DC-centred
    // CPU budget (token bucket, seconds) and coverage
    double credit_s_ = 0;
    int blocks_done_ = 0, blocks_all_ = 0;
    int64_t cov_t0_ = 0;
    int coverage_ = 100;
    // publishing
    std::vector<std::shared_ptr<Grid>> pool_;
    std::shared_ptr<const Steer> steer_;
    int64_t steer_ms_ = 0;
    std::string steer_why_;
    std::vector<float> scratch_;
    uint64_t gen_ = 0;

    void idle(const char* state, const std::string& why) {
        delay_.clear();
        clear();
        publish(nullptr);
        if (!g_on.load() && M_) {
            // off: give the memory back (M^2 x nb floats, the snapshots)
            std::vector<float>().swap(rows_);
            std::vector<float>().swap(recent_);
            std::vector<float>().swap(re_);
            std::vector<float>().swap(im_);
            stash_.clear();
            pool_.clear();
            steer_.reset();
            M_ = 0;   // setup() runs again on the next packet
        }
        set_state(state, why, nb_, M_, 0);
    }

    // drop everything accumulated (a retune, a park, other dimensions)
    void clear() {
        std::fill(rows_.begin(), rows_.end(), 0.0f);
        std::fill(recent_.begin(), recent_.end(), 0.0f);
        s1_ = s2_ = 0;
        recent_n_ = 0;
        for (auto& s : stash_) s.clear();
        since_tick_ = 0;
        seq_seen_ = false;
    }

    void setup(int nb, int M) {
        if (nb != nb_) {
            kfft_destroy_plan(plan_);
            kfft_free(in_);
            kfft_free(out_);
            in_ = kfft_alloc_complex(nb);
            out_ = kfft_alloc_complex(nb);
            plan_ = kfft_plan_dft_1d(nb, in_, out_, KFFT_FORWARD);
            win_.resize(static_cast<size_t>(nb));
            for (int i = 0; i < nb; i++)   // Hamming, as the main spectrum
                win_[i] = static_cast<float>(0.54 - 0.46 * std::cos(2.0 * M_PI * i / (nb - 1)));
        }
        nb_ = nb;
        M_ = M;
        rows_.assign(static_cast<size_t>(M) * M * nb, 0.0f);
        recent_.assign(static_cast<size_t>(M) * nb, 0.0f);
        re_.assign(static_cast<size_t>(M) * nb, 0.0f);
        im_.assign(static_cast<size_t>(M) * nb, 0.0f);
        stash_.assign(static_cast<size_t>(M), {});
        pool_.clear();
        steer_.reset();
        clear();
    }

    void handle(Job& job) {
        if (!g_on.load()) { delay_.clear(); idle("off", ""); return; }
        if (job.park) {
            delay_.clear();
            clear();
            publish(nullptr);
            set_state("parked", job.park, nb_, job.elements, 0);
            return;
        }
        // heimdall's noise flag trails the samples by up to 2 packets: a
        // packet is used only once HOLD_PACKETS more have come unparked, so
        // the noise source's first samples (the same on every antenna - a
        // confident false bearing) never reach the averages
        delay_.push_back(std::move(job));
        if (delay_.size() <= HOLD_PACKETS) return;
        Job j = std::move(delay_.front());
        delay_.pop_front();
        process(j);
    }

    void process(Job& job) {
        const int M = job.elements;
        const int nb = std::clamp(current_fft_size.load(std::memory_order_relaxed), MIN_FFT, MAX_FFT);
        if (M < 2 || M > MAX_CHANNELS || static_cast<int>(job.iq.size()) < M) return;
        const size_t S = job.iq[0].size();
        for (int m = 0; m < M; m++)
            if (job.iq[m].size() != S || S == 0) return;   // antennas missing (DoA conversion raced a mode change)
        if (nb != nb_ || M != M_) setup(nb, M);
        if (job.center_hz != center_hz_) {
            clear();
            center_hz_ = job.center_hz;
            steer_.reset();
        }
        if (seq_seen_ && job.seq != last_seq_ + 1)
            for (auto& s : stash_) s.clear();   // blocks are contiguous samples only
        seq_seen_ = true;
        last_seq_ = job.seq;

        // whole blocks straight from the packet while nothing is left over
        // (heimdall's 16384-sample packets: always), else through the stash
        const bool direct = stash_[0].empty();
        if (!direct)
            for (int m = 0; m < M; m++) stash_[m].insert(stash_[m].end(), job.iq[m].begin(), job.iq[m].end());
        const cf* src[MAX_CHANNELS];
        for (int m = 0; m < M; m++) src[m] = direct ? job.iq[m].data() : stash_[m].data();
        const size_t avail = direct ? S : stash_[0].size();
        size_t pos = 0;
        const double block_s = nb / static_cast<double>(SAMPLE_RATE);
        while (avail - pos >= static_cast<size_t>(nb)) {
            blocks_all_++;
            credit_s_ = std::min(credit_s_ + CPU_SHARE * block_s, 0.05);
            if (credit_s_ >= 0) {
                const auto t0 = std::chrono::steady_clock::now();
                block(src, pos);
                credit_s_ -= std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                blocks_done_++;
            }
            pos += static_cast<size_t>(nb);
        }
        if (direct) {
            if (pos < S)
                for (int m = 0; m < M; m++)
                    stash_[m].assign(job.iq[m].begin() + static_cast<std::ptrdiff_t>(pos), job.iq[m].end());
        } else if (pos) {
            for (auto& s : stash_) s.erase(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(pos));
        }

        since_tick_ += S;
        const size_t tick = static_cast<size_t>(TICK_S * SAMPLE_RATE);
        if (since_tick_ >= tick) {
            since_tick_ -= tick;
            const double alpha = std::clamp(static_cast<double>(averaging_alpha.load(std::memory_order_relaxed)), 1e-3, 1.0);
            const double tau = std::clamp((S / static_cast<double>(SAMPLE_RATE)) / alpha, TAU_MIN_S, TAU_MAX_S);
            on_tick(std::exp(-TICK_S / tau));
        }
        const int64_t t = now_ms();
        if (t - cov_t0_ >= 1000) {
            blocks_all_ += static_cast<int>(g_dropped_samples.exchange(0) / nb_);   // dropped in the queue
            coverage_ = blocks_all_ ? static_cast<int>(std::lround(100.0 * blocks_done_ / blocks_all_)) : 100;
            blocks_all_ = blocks_done_ = 0;
            cov_t0_ = t;
        }
    }

    // one block (samples [pos, pos + nb) of every antenna)
    void block(const cf* const* src, size_t pos) {
        const int nb = nb_, M = M_, half = nb / 2;
        for (int m = 0; m < M; m++) {
            const cf* x = src[m] + pos;
            for (int i = 0; i < nb; i++) {
                in_[i][0] = x[i].real() * win_[i];
                in_[i][1] = x[i].imag() * win_[i];
            }
            kfft_execute(plan_);
            // DC-centred, as the spectrum: bin k = FFT bin k + nb / 2 (mod nb)
            float* re = re_.data() + static_cast<size_t>(m) * nb;
            float* im = im_.data() + static_cast<size_t>(m) * nb;
            for (int k = 0; k < half; k++) { re[k] = out_[k + half][0]; im[k] = out_[k + half][1]; }
            for (int k = 0; k < half; k++) { re[k + half] = out_[k][0]; im[k + half] = out_[k][1]; }
        }
        // the cross-spectra x_a conj(x_b), in chunks that stay in L1
        constexpr int CH = 512;
        for (int k0 = 0; k0 < nb; k0 += CH) {
            const int n = std::min(CH, nb - k0);
            int row = M;
            for (int a = 0; a < M; a++) {
                const float* ra = re_.data() + static_cast<size_t>(a) * nb + k0;
                const float* ia = im_.data() + static_cast<size_t>(a) * nb + k0;
                float* p = rows_.data() + static_cast<size_t>(a) * nb + k0;
                float* rc = recent_.data() + static_cast<size_t>(a) * nb + k0;
                for (int k = 0; k < n; k++) {
                    const float pw = ra[k] * ra[k] + ia[k] * ia[k];
                    p[k] += pw;
                    rc[k] += pw;
                }
                for (int b = a + 1; b < M; b++, row += 2) {
                    const float* rb = re_.data() + static_cast<size_t>(b) * nb + k0;
                    const float* ib = im_.data() + static_cast<size_t>(b) * nb + k0;
                    float* cr = rows_.data() + static_cast<size_t>(row) * nb + k0;
                    float* ci = rows_.data() + static_cast<size_t>(row + 1) * nb + k0;
                    for (int k = 0; k < n; k++) {
                        cr[k] += ra[k] * rb[k] + ia[k] * ib[k];
                        ci[k] += ia[k] * rb[k] - ra[k] * ib[k];
                    }
                }
            }
        }
        s1_ += 1.0;
        s2_ += 1.0;
        recent_n_++;
    }

    // every TICK_S of stream: publish the averages, then age them by d
    void on_tick(double d) {
        if (s1_ > 0) {
            const int64_t t = now_ms();
            if (!steer_ || t - steer_ms_ > 500) {   // array settings may change at any time
                steer_ = fetch_steer(center_hz_, M_, &steer_why_);
                steer_ms_ = t;
            }
            if (!steer_) {
                publish(nullptr);
                set_state("parked", steer_why_, nb_, M_, 0);
            } else {
                std::shared_ptr<Grid> g;
                for (auto& p : pool_)
                    if (p.use_count() == 1) { g = p; break; }   // not published, nobody reading it
                if (!g) { g = std::make_shared<Grid>(); pool_.push_back(g); }
                g->nb = nb_;
                g->M = M_;
                g->center_hz = center_hz_;
                g->step_hz = static_cast<double>(SAMPLE_RATE) / nb_;
                g->f0_hz = center_hz_ - static_cast<double>(SAMPLE_RATE) / 2.0;
                g->k_time = s1_ * s1_ / std::max(s2_, 1e-9);
                g->rows = rows_;
                g->s1 = s1_;
                g->recent = recent_;
                g->recent_n = recent_n_;
                g->steer = steer_;
                g->gen = ++gen_;
                // each antenna's noise: the 20th percentile of its bins'
                // power over the middle 80 % (the tuner's roll-off aside)
                const int lo = nb_ / 10, n = nb_ - 2 * lo;
                for (int m = 0; m < M_; m++) {
                    scratch_.assign(rows_.begin() + static_cast<std::ptrdiff_t>(m) * nb_ + lo,
                                    rows_.begin() + static_cast<std::ptrdiff_t>(m) * nb_ + lo + n);
                    auto nth = scratch_.begin() + n / 5;
                    std::nth_element(scratch_.begin(), nth, scratch_.end());
                    g->w[m] = *nth > 0 ? static_cast<float>(1.0 / std::sqrt(static_cast<double>(*nth))) : 0.0f;
                }
                publish(g);
                set_state("running", "", nb_, M_, coverage_);
            }
        }
        const float df = static_cast<float>(d);
        for (float& v : rows_) v *= df;
        s1_ *= d;
        s2_ *= d * d;
        std::fill(recent_.begin(), recent_.end(), 0.0f);
        recent_n_ = 0;
    }
};

Worker* g_worker = nullptr;

void ensure_thread() {
    if (g_thread) return;
    g_worker = new Worker();
    g_thread = new std::thread([] {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "bin_doa");   // (top -H, the CPU figures)
#endif
        g_worker->run();
    });
}

// ---------------------------------------------------------------------------
// The estimate for one point: R = the summed covariances (rows sums s), K =
// its effective snapshots
// ---------------------------------------------------------------------------
struct Est {
    float az = 0, conf = 0;
    double noise = 0;   // the noise per antenna (whitened, the sums' units): the eigenvalues past lambda1
};

Est estimate(const double* s, int M, double K, const float* w, const Steer& st) {
    Est e;
    if (K < 2.0 * M) return e;
    std::complex<double> R[MAX_CHANNELS][MAX_CHANNELS];
    double T = 0;
    int jmax = 0;
    for (int a = 0; a < M; a++) {
        R[a][a] = s[a] * w[a] * w[a];
        T += R[a][a].real();
        if (R[a][a].real() > R[jmax][jmax].real()) jmax = a;
    }
    if (!(T > 0)) return e;
    int row = M;
    for (int a = 0; a < M; a++)
        for (int b = a + 1; b < M; b++, row += 2) {
            R[a][b] = std::complex<double>(s[row], s[row + 1]) * static_cast<double>(w[a] * w[b]);
            R[b][a] = std::conj(R[a][b]);
        }
    // rho > thr needs lambda1 > thr T / (M - 1 + thr); lambda1 is at most the
    // largest absolute row sum (Gershgorin) - most noise-only points stop here
    const double thr = NOISE_MARGIN * (1.0 + 3.0 * std::sqrt(M / K) + 3.0 * M / K);
    {
        double gmax = 0;
        for (int a = 0; a < M; a++) {
            double g = 0;
            for (int b = 0; b < M; b++) g += std::abs(R[a][b]);
            gmax = std::max(gmax, g);
        }
        if (!(gmax > thr * T / (M - 1 + thr))) return e;
    }
    // the principal eigenvector by power iteration from the strongest column
    std::complex<double> v[MAX_CHANNELS], u[MAX_CHANNELS];
    double nv = 0;
    for (int a = 0; a < M; a++) { v[a] = R[a][jmax]; nv += std::norm(v[a]); }
    if (!(nv > 0)) return e;
    for (int it = 0; it < 6; it++) {
        double nu = 0;
        for (int a = 0; a < M; a++) {
            std::complex<double> acc = 0;
            for (int b = 0; b < M; b++) acc += R[a][b] * v[b];
            u[a] = acc;
            nu += std::norm(acc);
        }
        if (!(nu > 0)) return e;
        const double inv = 1.0 / std::sqrt(nu);
        for (int a = 0; a < M; a++) v[a] = u[a] * inv;
    }
    double l1 = 0;   // Rayleigh quotient (v unit norm)
    for (int a = 0; a < M; a++) {
        std::complex<double> acc = 0;
        for (int b = 0; b < M; b++) acc += R[a][b] * v[b];
        l1 += (std::conj(v[a]) * acc).real();
    }
    const double rest = T - l1;
    e.noise = std::max(rest, 0.0) / (M - 1);
    const double rho = rest > 1e-12 * T ? l1 * (M - 1) / rest : 1e12;
    if (!(rho > thr)) return e;
    const double det = std::min(1.0, 10.0 * std::log10(rho / thr) / DET_SPAN_DB);

    // the bearing: the best |a^H v|^2 over the kept angles - coarse, then the
    // degrees around it, then a parabola through the three best
    cf vf[MAX_CHANNELS];
    for (int a = 0; a < M; a++) vf[a] = cf(static_cast<float>(v[a].real()), static_cast<float>(v[a].imag()));
    auto score = [&](int i) -> float {
        i = ((i % st.n) + st.n) % st.n;
        if (!st.keep[i]) return -1.0f;
        const cf* c = st.conjA.data() + static_cast<size_t>(i) * M;
        cf acc = 0;
        for (int a = 0; a < M; a++) acc += c[a] * vf[a];
        return std::norm(acc);
    };
    const int coarse = std::max(1, static_cast<int>(std::lround(5.0f / st.res)));
    int best = -1;
    float bs = -1.0f;
    for (int i = 0; i < st.n; i += coarse) {
        const float sc = score(i);
        if (sc > bs) { bs = sc; best = i; }
    }
    if (best < 0 || bs <= 0) return e;
    const int c0 = best;
    for (int i = c0 - coarse + 1; i < c0 + coarse; i++) {
        const float sc = score(i);
        if (sc > bs) { bs = sc; best = ((i % st.n) + st.n) % st.n; }
    }
    const float sl = score(best - 1), sr = score(best + 1);
    float frac = 0.0f;
    if (sl >= 0 && sr >= 0) {
        const float den = sl - 2.0f * bs + sr;
        if (den < 0) frac = std::clamp(0.5f * (sl - sr) / den, -0.5f, 0.5f);
    }
    const double fit = std::min(1.0, static_cast<double>(bs));
    const double cfit = std::clamp((fit - FIT_LO) / (FIT_HI - FIT_LO), 0.0, 1.0);
    e.conf = static_cast<float>(det * cfit);
    e.az = wrap_degrees((best + frac) * st.res + st.offset);
    return e;
}

// independent snapshots in n adjacent Hamming-windowed bins (their noise is
// correlated: power coefficient 0.39 / 0.018 at 1 / 2 bins - see
// fft_processor.cpp noise_quantile)
double bins_eff(int n) {
    if (n <= 1) return 1.0;
    double var = n + 2.0 * (n - 1) * 0.390;
    if (n > 2) var += 2.0 * (n - 2) * 0.0177;
    return static_cast<double>(n) * n / var;
}

}  // namespace

void set_enabled(bool on) {
    const bool was = g_on.exchange(on);
    if (on) ensure_thread();
    if (was != on) {
        if (!on) publish(nullptr);
        {
            std::lock_guard<std::mutex> lk(g_state_mu);
            if (!on) { g_state_text = "off"; g_state_reason.clear(); g_state_cov = 0; }
            else if (g_state_text == "off") { g_state_text = "idle"; g_state_reason = "starting"; }
            g_state_json_sent = state_json_locked();
        }
        WebSocketServer::broadcast_json_message(state_json());
        g_cv.notify_one();
        std::cout << "Bearing spectrum " << (on ? "on" : "off") << std::endl;
    }
}

bool enabled() { return g_on.load(std::memory_order_relaxed); }

void set_viewers(int n) { g_viewers.store(n, std::memory_order_relaxed); }

bool wanted() {
    return g_on.load(std::memory_order_relaxed) && g_viewers.load(std::memory_order_relaxed) > 0 &&
           !multi_tuner_mode() && !server_generic_rtlsdr.load(std::memory_order_relaxed);
}

void on_packet(std::vector<std::vector<std::complex<float>>>&& iq, int elements, uint64_t seq, double center_hz,
               const char* park) {
    if (!g_on.load(std::memory_order_relaxed) || g_viewers.load(std::memory_order_relaxed) <= 0) return;
    Job job, dropped;
    job.iq = std::move(iq);
    job.elements = elements;
    job.seq = seq;
    job.center_hz = center_hz;
    job.park = park;
    {
        // the worker is behind: the oldest goes (a gap - the worker restarts
        // its block assembly at the next one); freed outside the lock
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push_back(std::move(job));
        if (g_queue.size() > QUEUE_MAX) {
            dropped = std::move(g_queue.front());
            g_queue.pop_front();
        }
    }
    if (!dropped.iq.empty() && !dropped.park)
        g_dropped_samples.fetch_add(static_cast<int64_t>(dropped.iq[0].size()), std::memory_order_relaxed);
    g_cv.notify_one();
}

std::string build_frame(const SpectrumDisplay& d, const SpectrumViewCut& cut, uint32_t seq) {
    if (!g_on.load(std::memory_order_relaxed) || cut.n == 0 || !(d.step_hz > 0)) return std::string();
    std::shared_ptr<const Grid> g;
    {
        std::lock_guard<std::mutex> lk(g_pub_mu);
        g = g_pub;
    }
    if (!g || !g->steer || g->steer->M != g->M || g->nb < 2) return std::string();
    // the same tuning as the spectrum (a retune: until the next snapshot)
    if (std::fabs(g->center_hz - d.center_hz) > 0.5 * std::min(g->step_hz, d.step_hz)) return std::string();
    const Steer& st = *g->steer;
    const int M = g->M, nb = g->nb;
    const size_t n = cut.n;

    // pages showing the same points (the same width and zoom) share the
    // estimates of this snapshot: only the seq differs (uWS thread only)
    struct Cached { uint64_t gen; double f0, step; size_t i0, i1, pool, n; std::string msg; };
    static Cached cache[4];
    static uint64_t cache_gen = 0;
    static int cache_next = 0;
    if (cache_gen != g->gen) {
        for (auto& c : cache) c.msg.clear();
        cache_gen = g->gen;
    }
    for (const auto& c : cache)
        if (!c.msg.empty() && c.gen == g->gen && c.f0 == d.f0_hz && c.step == d.step_hz && c.i0 == cut.i0 &&
            c.i1 == cut.i1 && c.pool == cut.pool && c.n == n) {
            std::string out = c.msg;
            std::memcpy(out.data() + 8, &seq, sizeof seq);
            return out;
        }

    std::string out;
    out.resize(32 + 3 * n);
    char* p = out.data();
    auto put = [&p](const auto& v) { std::memcpy(p, &v, sizeof v); p += sizeof v; };
    put(uint32_t(9));
    put(uint8_t(0));
    put(st.flags);
    put(static_cast<uint8_t>(M));
    put(static_cast<uint8_t>(st.topology));
    put(seq);
    put(static_cast<uint32_t>(n));
    put(d.f0_hz + (static_cast<double>(cut.i0) + (static_cast<double>(cut.pool) - 1.0) / 2.0) * d.step_hz);
    put(d.step_hz * static_cast<double>(cut.pool));
    uint8_t* conf = reinterpret_cast<uint8_t*>(p);
    char* azp = p + n;

    const int R = M * M;
    double sums[MAX_CHANNELS * MAX_CHANNELS];
    int last_k0 = -1, last_k1 = -1;
    Est last;
    for (size_t j = 0; j < n; j++) {
        // the display bins of point j, and the bearing bins whose centres lie in them
        const size_t a = cut.i0 + j * cut.pool, b = std::min(a + cut.pool, cut.i1 + 1);
        const double flo = d.f0_hz + (static_cast<double>(a) - 0.5) * d.step_hz;
        const double fhi = d.f0_hz + (static_cast<double>(b) - 0.5) * d.step_hz;
        int k0 = static_cast<int>(std::ceil((flo - g->f0_hz) / g->step_hz - 1e-9));
        int k1 = static_cast<int>(std::ceil((fhi - g->f0_hz) / g->step_hz - 1e-9)) - 1;
        if (k1 < k0) k0 = k1 = static_cast<int>(std::lround((0.5 * (flo + fhi) - g->f0_hz) / g->step_hz));
        k0 = std::clamp(k0, 0, nb - 1);
        k1 = std::clamp(k1, 0, nb - 1);
        Est e;
        if (k0 == last_k0 && k1 == last_k1) {
            e = last;   // a coarser bearing grid than the points: the same bins
        } else {
            for (int r = 0; r < R; r++) {
                const float* src = g->rows.data() + static_cast<size_t>(r) * nb;
                double acc = 0;
                for (int k = k0; k <= k1; k++) acc += src[k];
                sums[r] = acc;
            }
            e = estimate(sums, M, g->k_time * bins_eff(k1 - k0 + 1), g->w, st);
            if (e.conf > 0 && g->recent_n > 0 && g->s1 > 0 && e.noise > 0) {
                // the average remembers a strong burst for many time
                // constants after it ended (linear power, where the trace
                // averages dB): the point keeps its colour only while its
                // power since the last snapshot stands above its noise (the
                // average's eigenvalues past lambda1), by 2 to 5 standard
                // deviations of that power - the colour goes with the burst
                double now = 0;
                for (int m = 0; m < M; m++) {
                    const float* src = g->recent.data() + static_cast<size_t>(m) * nb;
                    double acc = 0;
                    for (int k = k0; k <= k1; k++) acc += src[k];
                    now += acc * g->w[m] * g->w[m];
                }
                const double q = (now / g->recent_n) / (M * e.noise / g->s1);
                const double sd = 1.0 / std::sqrt(g->recent_n * bins_eff(k1 - k0 + 1) * M);
                e.conf *= static_cast<float>(std::clamp((q - 1.0 - RECENT_LO * sd) / ((RECENT_HI - RECENT_LO) * sd), 0.0, 1.0));
            }
            last = e;
            last_k0 = k0;
            last_k1 = k1;
        }
        const int q = static_cast<int>(std::lround(e.conf * 255.0f));
        conf[j] = static_cast<uint8_t>(std::clamp(q, 0, 255));
        const uint16_t az = q > 0 ? static_cast<uint16_t>(std::lround(e.az * (65536.0 / 360.0)) & 0xFFFF) : 0;
        std::memcpy(azp + 2 * j, &az, 2);
    }
    cache[cache_next] = {g->gen, d.f0_hz, d.step_hz, cut.i0, cut.i1, cut.pool, n, out};
    cache_next = (cache_next + 1) % 4;
    return out;
}

std::string state_json() {
    std::lock_guard<std::mutex> lk(g_state_mu);
    return state_json_locked();
}

void shutdown() {
    g_stop.store(true);
    g_cv.notify_all();
    if (g_thread && g_thread->joinable()) g_thread->join();
}

}  // namespace bin_doa
