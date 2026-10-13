// 🗣 Voice to Text - see include/voice2text.hpp for the design.
//
// Threads: feed_analog() (the decimation pass) and the digital decoders' voice
// tap (their worker threads) only queue copies (q_mu). ONE worker thread owns
// everything else that moves - the per-VFO taps (demodulation, cutting into
// transmissions, the bearing during each), the job queue and the helper
// process - and publishes results / status under st_mu for the uWS thread.

#include "voice2text.hpp"

#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <liquid/liquid.h>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "decimator_manager.hpp"
#include "decoder_log.hpp"
#include "digital/dig_plugin.hpp"
#include "digital/digital_decoder.hpp"
#include "globals.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "station_info.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include "voice2text_wire.hpp"

extern char** environ;

namespace v2t {

namespace {

namespace wire = v2t::wire;

// The models setup.sh / the UI can download - the same list as CATALOGUE in
// transcribe/setup.sh. mb = download size.
struct CatalogueEntry { const char* name; int mb; bool multi; const char* note; };
const CatalogueEntry MODEL_CATALOGUE[] = {
    {"tiny.en", 75, false, "fastest, least accurate"},
    {"base.en", 142, false, "recommended: good on a Pi 5, faster than real time"},
    {"base.en-q5_1", 57, false, "base.en compressed: less memory, about as accurate"},
    {"small.en", 466, false, "most accurate on a Pi: about 2x slower than real time"},
    {"small.en-q5_1", 181, false, "small.en compressed"},
    {"tiny", 75, true, "any language (or translated to English), fastest"},
    {"base", 142, true, "any language (or translated to English)"},
    {"base-q5_1", 57, true, "base compressed"},
    {"small", 466, true, "any language, most accurate on a Pi"},
    {"small-q5_1", 181, true, "small compressed"},
    {"medium.en-q5_0", 514, false, "PC-class CPU only"},
    {"large-v3-turbo-q5_0", 547, true, "PC-class CPU only, best for other languages"},
};
constexpr const char* MODEL_URL = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-";
constexpr const char* VAD_NAME = "silero-v6.2.0";
constexpr const char* VAD_URL = "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-";

constexpr double PREROLL_S = 0.3;       // audio kept from before the squelch opened
constexpr double HANG_S = 0.8;          // squelch closed this long = the transmission ended
constexpr double TAIL_S = 0.25;         // ... of which this much is kept
constexpr int64_t DIGITAL_GAP_MS = 1500;   // no decoded voice this long = the transmission ended
constexpr int64_t ANALOG_STALL_MS = 2000;  // no analog input this long = it ended (input stopped)
constexpr double MAX_QUEUED_S = 600;    // transmissions waiting for the helper (then the oldest go)
constexpr size_t MAX_QUEUED_JOBS = 40;
constexpr double MAX_INPUT_S = 3.0;     // per VFO, audio waiting for the worker
constexpr size_t MAX_HISTORY = 1000;
constexpr size_t MAX_CLIPS = 2000;
constexpr uint64_t MAX_CLIP_BYTES = 400ull << 20;
constexpr int64_t HELPER_IDLE_STOP_MS = 120000;   // no VFO transcribing: the helper (its RAM) goes
constexpr const char* HISTORY_FILE = "transcripts.jsonl";
constexpr const char* CLIP_DIR = "voice_clips";

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string transcribe_dir() {
    const char* e = std::getenv("KRAKEN_TRANSCRIBE_DIR");
    return (e && *e) ? std::string(e) : std::string("transcribe");
}
std::string helper_exe() { return transcribe_dir() + "/build/kraken_whisper"; }
std::string models_dir() {
    const char* e = std::getenv("KRAKEN_WHISPER_MODELS");
    return (e && *e) ? std::string(e) : transcribe_dir() + "/models";
}
std::string model_path(const std::string& name) { return models_dir() + "/ggml-" + name + ".bin"; }
bool file_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}
uint64_t file_size(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}
// a model name: the part of ggml-<name>.bin; letters, digits, . _ -
bool valid_model_name(const std::string& n) {
    if (n.empty() || n.size() > 48 || n[0] == '.') return false;
    for (char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
    return true;
}
const CatalogueEntry* catalogue(const std::string& name) {
    for (const auto& c : MODEL_CATALOGUE)
        if (name == c.name) return &c;
    return nullptr;
}
// installed speech models (ggml-*.bin, not the VAD model)
std::vector<std::string> installed_models() {
    std::vector<std::string> v;
    if (DIR* d = opendir(models_dir().c_str())) {
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name;
            if (f.size() < 10 || f.rfind("ggml-", 0) != 0 || f.substr(f.size() - 4) != ".bin") continue;
            std::string n = f.substr(5, f.size() - 9);
            if (n.rfind("silero", 0) == 0 || !valid_model_name(n)) continue;
            v.push_back(n);
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}
// the newest Silero VAD model in the folder ("" = none)
std::string vad_model_path() {
    std::string best;
    if (DIR* d = opendir(models_dir().c_str())) {
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name;
            if (f.rfind("ggml-silero", 0) == 0 && f.size() > 4 && f.substr(f.size() - 4) == ".bin" && f > best) best = f;
        }
        closedir(d);
    }
    return best.empty() ? "" : models_dir() + "/" + best;
}
bool model_multilingual(const std::string& name) {
    if (const CatalogueEntry* c = catalogue(name)) return c->multi;
    return name.find(".en") == std::string::npos;   // whisper.cpp's naming
}

std::string fmt(const char* f, double v) {
    char b[48];
    snprintf(b, sizeof b, f, v);
    return b;
}

// --- input (any thread) -> worker -------------------------------------------

struct Input {
    int vfo = -1;
    bool digital = false;
    std::vector<std::complex<float>> iq;   // analog: the VFO's decimated stream
    std::vector<float> a8k;                // digital: decoded voice, 8 kHz
    float rate = 0;
    double rf = 0;
    DemodulatorMode mode = DemodulatorMode::NBFM;
    bool open = false;
    std::string plugin, tid, tlabel;
    int64_t t_ms = 0;
};

// --- a transcription job / result -------------------------------------------

struct Utterance {
    int vfo = -1;
    double rf = 0;
    std::string src, tid, tlabel;
    int64_t t0_ms = 0;
    std::vector<int16_t> pcm;   // 16 kHz
    double doa = NAN, brg = NAN;
    int part = 0;               // > 0: a part of a longer transmission
    int retries = 0;
    double seconds() const { return pcm.size() / static_cast<double>(RATE); }
};

struct Transcript {
    uint64_t id = 0;
    int64_t t_ms = 0;
    double dur = 0;
    int vfo = -1;
    double mhz = 0;
    std::string src, tid, tlabel, lang, text;
    double doa = NAN, brg = NAN;
    float conf = 0;
    uint32_t proc_ms = 0;
    bool clip = false;
    int part = 0;
};

std::string transcript_json(const Transcript& t) {
    std::string j = "{\"id\":" + std::to_string(t.id) + ",\"t\":" + std::to_string(t.t_ms) + ",\"dur\":" +
                    fmt("%.1f", t.dur) + ",\"vfo\":" + std::to_string(t.vfo) + ",\"mhz\":" + fmt("%.6f", t.mhz) +
                    ",\"src\":\"" + json_escape(t.src) + "\"";
    if (!t.tid.empty()) j += ",\"tid\":\"" + json_escape(t.tid) + "\"";
    if (!t.tlabel.empty()) j += ",\"tl\":\"" + json_escape(t.tlabel) + "\"";
    if (is_finite_value(t.doa)) j += ",\"doa\":" + fmt("%.1f", t.doa);
    if (is_finite_value(t.brg)) j += ",\"brg\":" + fmt("%.1f", t.brg);
    if (t.part > 0) j += ",\"part\":" + std::to_string(t.part);
    j += ",\"conf\":" + fmt("%.2f", t.conf) + ",\"lang\":\"" + json_escape(t.lang) + "\",\"ms\":" +
         std::to_string(t.proc_ms) + ",\"clip\":" + (t.clip ? "true" : "false") + ",\"text\":\"" +
         json_escape(t.text) + "\"}";
    return j;
}

bool transcript_from_json(const std::string& line, Transcript* t) {
    std::string v;
    if (!json_find(line, "id", v)) return false;
    t->id = strtoull(v.c_str(), nullptr, 10);
    if (!t->id || !json_find(line, "text", t->text)) return false;
    if (json_find(line, "t", v)) t->t_ms = strtoll(v.c_str(), nullptr, 10);
    if (json_find(line, "dur", v)) t->dur = stod_finite(v);
    if (json_find(line, "vfo", v)) t->vfo = atoi(v.c_str());
    if (json_find(line, "mhz", v)) t->mhz = stod_finite(v);
    json_find(line, "src", t->src);
    json_find(line, "tid", t->tid);
    json_find(line, "tl", t->tlabel);
    json_find(line, "lang", t->lang);
    if (json_find(line, "doa", v)) t->doa = stod_finite(v);
    if (json_find(line, "brg", v)) t->brg = stod_finite(v);
    if (json_find(line, "conf", v)) t->conf = stof_finite(v);
    if (json_find(line, "ms", v)) t->proc_ms = static_cast<uint32_t>(atol(v.c_str()));
    if (json_find(line, "part", v)) t->part = atoi(v.c_str());
    if (json_find(line, "clip", v)) t->clip = v == "true";
    return true;
}

// --- per-VFO tap (worker thread only) ---------------------------------------

struct Tap {
    // what the VFO is now (refreshed from the VFO list)
    bool on = false;
    // analog demodulation -> 16 kHz
    bool digital = false;
    DemodulatorMode mode = DemodulatorMode::NBFM;
    float rate = 0;
    std::complex<float> prev{0.0f, 0.0f};
    msresamp_rrrf rs = nullptr;
    iirfilt_rrrf hp = nullptr, lp = nullptr;
    float de_a = 0, de_y = 0;      // WBFM de-emphasis (75 us, as the live audio)
    float am_dc = 0, am_dc_a = 0, am_pow = 1e-4f, am_pow_a = 0;
    firinterp_rrrf interp = nullptr;   // digital: 8 -> 16 kHz
    std::vector<float> work;
    // the transmission being collected
    double rf = 0;
    std::deque<float> pre;          // pre-roll (analog, while closed)
    std::vector<float> cur;
    bool rec = false;
    size_t closed = 0;              // samples since the squelch closed (analog)
    int64_t t0_ms = 0, last_audio_ms = 0;
    int part = 0;
    std::string src, tid, tlabel;
    long anon_from = -1;            // digital: where audio without the named talker began (-1 = none)
    // bearing during it
    int64_t doa_stamp = 0;
    double dsx = 0, dsy = 0, dw = 0;
    Tap() = default;
    Tap(const Tap&) = delete;   // owns the liquid objects
    Tap& operator=(const Tap&) = delete;
    ~Tap() { release(); }
    void release() {
        if (rs) msresamp_rrrf_destroy(rs);
        if (hp) iirfilt_rrrf_destroy(hp);
        if (lp) iirfilt_rrrf_destroy(lp);
        if (interp) firinterp_rrrf_destroy(interp);
        rs = nullptr;
        hp = lp = nullptr;
        interp = nullptr;
    }
    void setup_analog(float r, DemodulatorMode m) {
        release();
        rate = r;
        mode = m;
        prev = {0.0f, 0.0f};
        rs = msresamp_rrrf_create(static_cast<float>(RATE) / r, 60.0f);
        const bool wb = m == DemodulatorMode::WBFM;
        // voice band: CTCSS tones (67-254 Hz) and the FM noise above the voice out
        hp = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_HIGHPASS, LIQUID_IIRDES_SOS, 4,
                                           (wb ? 100.0f : 260.0f) / RATE, 0.0f, 1.0f, 60.0f);
        lp = iirfilt_rrrf_create_prototype(LIQUID_IIRDES_BUTTER, LIQUID_IIRDES_LOWPASS, LIQUID_IIRDES_SOS, 6,
                                           (wb ? 7000.0f : 3800.0f) / RATE, 0.0f, 1.0f, 60.0f);
        de_a = wb ? 1.0f - std::exp(-1.0f / (r * 75e-6f)) : 0.0f;
        de_y = 0;
        am_dc = 0;
        am_dc_a = 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * 30.0f / r);
        am_pow = 1e-4f;
        am_pow_a = 1.0f - std::exp(-1.0f / (0.5f * r));
    }
    // the VFO's samples -> 16 kHz audio (appended to out)
    void demod(const std::vector<std::complex<float>>& x, std::vector<float>& out) {
        work.resize(x.size());
        if (mode == DemodulatorMode::AM) {
            for (size_t i = 0; i < x.size(); i++) {
                const float e = std::abs(x[i]);
                am_dc += am_dc_a * (e - am_dc);
                const float v = e - am_dc;
                am_pow += am_pow_a * (v * v - am_pow);
                work[i] = 0.15f * v / std::sqrt(am_pow + 1e-12f);
            }
        } else {
            const float dev = mode == DemodulatorMode::WBFM ? 75000.0f : 5000.0f;
            const float k = rate / (2.0f * static_cast<float>(M_PI) * dev);
            for (size_t i = 0; i < x.size(); i++) {
                const std::complex<float> d = x[i] * std::conj(prev);
                prev = x[i];
                float v = std::atan2(d.imag(), d.real()) * k;
                if (de_a > 0) v = de_y += de_a * (v - de_y);
                work[i] = v;
            }
        }
        const size_t base = out.size();
        out.resize(base + static_cast<size_t>(std::ceil(x.size() * RATE / rate)) + 64);
        unsigned ny = 0;
        msresamp_rrrf_execute(rs, work.data(), static_cast<unsigned>(work.size()), out.data() + base, &ny);
        out.resize(base + ny);
        for (size_t i = base; i < out.size(); i++) {
            float v;
            iirfilt_rrrf_execute(hp, out[i], &v);
            iirfilt_rrrf_execute(lp, v, &out[i]);
        }
    }
    void upsample8k(const std::vector<float>& x, std::vector<float>& out) {
        if (!interp) interp = firinterp_rrrf_create_kaiser(2, 12, 60.0f);
        const size_t base = out.size();
        out.resize(base + 2 * x.size());
        for (size_t i = 0; i < x.size(); i++) firinterp_rrrf_execute(interp, x[i], out.data() + base + 2 * i);
    }
};

// --- shared state ------------------------------------------------------------

struct Config {
    std::string model = "base.en";
    std::string language = "auto";
    int threads = 2;
    std::string prompt;
    bool vad = true;
    bool translate = false;
    bool keep_audio = true;
};

struct Download {
    std::string name, error;
    pid_t pid = -1;
    std::string part;     // the file growing
    uint64_t total = 0;   // expected bytes (catalogue)
    bool running = false;
};

struct VfoState { int id; std::string src; bool rec; double secs; bool digital; };

struct State {
    // input queue
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<Input> q;
    std::map<int, double> queued_s;   // per VFO, seconds of input waiting
    // settings
    std::mutex cfg_mu;
    Config cfg;
    // results + status for the pages
    std::mutex st_mu;
    std::deque<Transcript> history;
    std::vector<Transcript> fresh;   // since the last take_new_message()
    uint64_t last_id = 0;
    std::string state = "off", loaded, system;
    bool multilingual = false, vad_loaded = false;
    size_t queue = 0;
    double queued_audio_s = 0;
    uint64_t dropped = 0, done = 0, nospeech = 0, failed = 0;
    double rtf = 0;               // processing time / audio time (moving average)
    uint32_t last_ms = 0;
    std::vector<VfoState> vfos;
    std::deque<uint64_t> clips;   // ids with a WAV in voice_clips/, oldest first
    uint64_t clip_bytes = 0;
    size_t file_lines = 0;        // lines in transcripts.jsonl
    uint64_t clear_gen = 0;       // VOICE2TEXT_CLEAR count: a result begun before one is dropped
    std::string last_error;       // the latest helper problem, until a job goes through
    Download dl;
    // worker
    std::atomic<bool> running{false};
    std::thread worker;
};
State& S() {
    static State* s = new State;   // never destroyed: the voice tap may fire while the process exits
    return *s;
}

// --- history / clips (st_mu held where noted) ---------------------------------

// st_mu held: the file = the history (it only grows by appending, so it is
// cut back to the history when it holds 3x as many lines)
void rewrite_history_locked(State& s) {
    const std::string tmp = std::string(HISTORY_FILE) + ".tmp";
    std::ofstream o(tmp, std::ios::trunc);
    for (const auto& t : s.history) o << transcript_json(t) << "\n";
    o.close();
    if (o && rename(tmp.c_str(), HISTORY_FILE) == 0) s.file_lines = s.history.size();
    else unlink(tmp.c_str());
}

void load_history() {
    State& s = S();
    std::ifstream f(HISTORY_FILE);
    std::deque<Transcript> h;
    size_t lines = 0;
    std::string line;
    while (std::getline(f, line)) {
        lines++;
        Transcript t;
        try {
            if (!transcript_from_json(line, &t)) continue;
        } catch (const std::exception&) {
            continue;   // a damaged / hand-edited line (a number that isn't one)
        }
        h.push_back(std::move(t));
        if (h.size() > MAX_HISTORY) h.pop_front();
    }
    std::lock_guard<std::mutex> lk(s.st_mu);
    s.history = std::move(h);
    for (const auto& t : s.history) s.last_id = std::max(s.last_id, t.id);
    s.file_lines = lines;
    if (lines > MAX_HISTORY * 3) rewrite_history_locked(s);
    // clips on disk (named <id>.wav), oldest first
    std::vector<uint64_t> ids;
    if (DIR* d = opendir(CLIP_DIR)) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() < 5 || n.substr(n.size() - 4) != ".wav") continue;
            const uint64_t id = strtoull(n.c_str(), nullptr, 10);
            if (id) ids.push_back(id);
        }
        closedir(d);
    }
    std::sort(ids.begin(), ids.end());
    s.clips.assign(ids.begin(), ids.end());
    s.clip_bytes = 0;
    for (uint64_t id : ids) s.clip_bytes += file_size(std::string(CLIP_DIR) + "/" + std::to_string(id) + ".wav");
}

bool write_wav(const std::string& path, const std::vector<int16_t>& pcm) {
    FILE* f = fopen((path + ".tmp").c_str(), "wb");
    if (!f) return false;
    const uint32_t data = static_cast<uint32_t>(pcm.size() * 2), rate = RATE, byte_rate = RATE * 2;
    const uint32_t riff = 36 + data, fmt_len = 16;
    const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f); fwrite(&pcm_fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    const bool ok = fwrite(pcm.data(), 2, pcm.size(), f) == pcm.size();
    if (fclose(f) != 0 || !ok) {
        unlink((path + ".tmp").c_str());
        return false;
    }
    return rename((path + ".tmp").c_str(), path.c_str()) == 0;
}

// st_mu held
void trim_clips_locked(State& s) {
    while (!s.clips.empty() && (s.clips.size() > MAX_CLIPS || s.clip_bytes > MAX_CLIP_BYTES)) {
        const std::string p = std::string(CLIP_DIR) + "/" + std::to_string(s.clips.front()) + ".wav";
        s.clip_bytes -= std::min(s.clip_bytes, file_size(p));
        unlink(p.c_str());
        s.clips.pop_front();
    }
}

// --- the worker ----------------------------------------------------------------

class Worker {
public:
    void run();

private:
    State& s_ = S();
    std::map<int, Tap> taps_;
    std::deque<Utterance> pending_;
    // helper process
    dig::PluginProcess proc_;
    bool proc_on_ = false, ready_ = false;
    std::string proc_model_;       // model it was started with
    int64_t next_start_ms_ = 0;
    int backoff_s_ = 2;
    bool has_job_ = false;
    Utterance job_;
    uint32_t job_id_ = 0;
    int64_t job_sent_ms_ = 0;
    int64_t last_wanted_ms_ = 0;
    bool usable_ = true;           // helper + model there: audio is worth collecting
    Config sent_;                  // options the helper has
    bool sent_valid_ = false;
    int64_t vfo_scan_ms_ = 0;
    std::map<int, std::pair<bool, std::string>> vfo_cfg_;   // id -> (transcribe on, analog src or "digital")

    void scan_vfos(int64_t t);
    void take_input(Input& in, int64_t t);
    void analog(Tap& tap, int vfo, Input& in, int64_t t);
    void digital(Tap& tap, int vfo, Input& in, int64_t t);
    void sample_doa(Tap& tap, int vfo);
    void finish(Tap& tap, int vfo, bool more);
    void split(Tap& tap, int vfo, size_t at);
    void enqueue(Utterance&& u);
    void helper(int64_t t);
    void start_helper(const Config& c, int64_t t);
    void stop_helper(const std::string& why);
    void sync_options(const Config& c);
    void on_result(const char* p, size_t len);
    void set_state(const std::string& st, const std::string& err = "");
};

// err "" keeps the last problem: it stays shown (the state moves on to
// "starting" within ms of a crash) until the helper loads or a job goes through
void Worker::set_state(const std::string& st, const std::string& err) {
    std::lock_guard<std::mutex> lk(s_.st_mu);
    s_.state = st;
    if (!err.empty()) s_.last_error = err;
}

// which VFOs transcribe, and from what (every 250 ms)
void Worker::scan_vfos(int64_t t) {
    if (t - vfo_scan_ms_ < 250) return;
    vfo_scan_ms_ = t;
    std::map<int, std::pair<bool, std::string>> cfg;
    for (const auto& inst : decimator_manager.getAllDecimators()) {
        if (!inst || inst->being_deleted.load(std::memory_order_relaxed)) continue;
        if (!inst->transcribe.load(std::memory_order_relaxed)) continue;
        const bool dig = inst->digital_mode.load(std::memory_order_relaxed) != 0;
        cfg[inst->id] = {true, dig ? "digital" : DecimatorManager::demodModeToString(inst->demod_mode.load())};
    }
    // VFOs that stopped (or were removed): the transmission in progress is sent
    for (auto it = taps_.begin(); it != taps_.end();) {
        if (!cfg.count(it->first)) {
            if (it->second.rec) finish(it->second, it->first, false);
            it = taps_.erase(it);
        } else {
            ++it;
        }
    }
    vfo_cfg_ = std::move(cfg);
    std::vector<VfoState> vs;
    for (const auto& kv : vfo_cfg_) {
        auto tit = taps_.find(kv.first);
        const bool rec = tit != taps_.end() && tit->second.rec;
        const double secs = rec ? tit->second.cur.size() / static_cast<double>(RATE) : 0;
        const std::string src = rec && !tit->second.src.empty() ? tit->second.src : kv.second.second;
        vs.push_back({kv.first, src, rec, secs, kv.second.second == "digital"});
    }
    std::lock_guard<std::mutex> lk(s_.st_mu);
    s_.vfos = std::move(vs);
}

void Worker::take_input(Input& in, int64_t t) {
    auto c = vfo_cfg_.find(in.vfo);
    if (c == vfo_cfg_.end()) return;                    // not (or no longer) transcribed
    if ((c->second.second == "digital") != in.digital) return;   // its source changed meanwhile
    Tap& tap = taps_[in.vfo];
    if (in.digital) digital(tap, in.vfo, in, t);
    else analog(tap, in.vfo, in, t);
}

void Worker::analog(Tap& tap, int vfo, Input& in, int64_t t) {
    // a retune, another demod or rate: what was collected is sent, the demod restarts
    if (tap.digital || !tap.rs || tap.rate != in.rate || tap.mode != in.mode || std::fabs(tap.rf - in.rf) > 500) {
        if (tap.rec) finish(tap, vfo, false);
        tap.setup_analog(in.rate, in.mode);
        tap.digital = false;
        tap.rf = in.rf;
        tap.pre.clear();
    }
    std::vector<float> y;
    tap.demod(in.iq, y);
    const size_t preroll = static_cast<size_t>(PREROLL_S * RATE);
    if (!tap.rec) {
        if (!in.open) {
            tap.pre.insert(tap.pre.end(), y.begin(), y.end());
            while (tap.pre.size() > preroll) tap.pre.pop_front();
            return;
        }
        tap.rec = true;
        tap.cur.assign(tap.pre.begin(), tap.pre.end());
        tap.t0_ms = t - static_cast<int64_t>(tap.pre.size() * 1000 / RATE);
        tap.pre.clear();
        tap.closed = 0;
        tap.part = 0;
        tap.src = DecimatorManager::demodModeToString(in.mode);
        tap.tid.clear();
        tap.tlabel.clear();
        tap.dsx = tap.dsy = tap.dw = 0;
        tap.doa_stamp = 0;
    }
    tap.cur.insert(tap.cur.end(), y.begin(), y.end());
    tap.last_audio_ms = t;
    tap.closed = in.open ? 0 : tap.closed + y.size();
    if (tap.closed >= static_cast<size_t>(HANG_S * RATE)) {
        const size_t keep = static_cast<size_t>(TAIL_S * RATE);
        tap.cur.resize(tap.cur.size() - std::min(tap.cur.size(), tap.closed - keep));
        finish(tap, vfo, false);
    } else if (tap.cur.size() >= static_cast<size_t>(MAX_UTTERANCE_S * RATE)) {
        finish(tap, vfo, true);
    }
}

void Worker::digital(Tap& tap, int vfo, Input& in, int64_t t) {
    if (!tap.digital) {
        if (tap.rec) finish(tap, vfo, false);
        tap.release();
        tap.digital = true;
        tap.rs = nullptr;
    }
    const bool known = !in.tid.empty() && in.tid != "?";
    if (tap.rec && !tap.tid.empty() && tap.tid != "?") {
        if (!known) {
            // no / an unnamed talker after a named one: its call ended and the
            // next radio's first frames came before the plugin named it - or a
            // pause in the same call (P25 closes a talker after 0.75 s): decided
            // when a name comes
            if (tap.anon_from < 0) tap.anon_from = static_cast<long>(tap.cur.size());
        } else if (in.tid != tap.tid) {
            // another radio: what came since the unnamed frames is its
            split(tap, vfo, tap.anon_from >= 0 ? static_cast<size_t>(tap.anon_from) : tap.cur.size());
        } else {
            tap.anon_from = -1;   // the same radio went on
        }
    }
    std::vector<float> y;
    tap.upsample8k(in.a8k, y);
    if (!tap.rec) {
        tap.rec = true;
        tap.cur.clear();
        tap.t0_ms = t - static_cast<int64_t>(y.size() * 1000 / RATE);
        tap.part = 0;
        tap.src = in.plugin;
        tap.tid = in.tid;
        tap.tlabel = in.tlabel;
        tap.rf = in.rf;
        tap.dsx = tap.dsy = tap.dw = 0;
        tap.doa_stamp = 0;
    } else if ((tap.tid.empty() || tap.tid == "?") && known) {
        tap.tid = in.tid;   // the radio was named after its first frames
        tap.tlabel = in.tlabel;
    } else if (known && !in.tlabel.empty()) {
        tap.tlabel = in.tlabel;
    }
    tap.cur.insert(tap.cur.end(), y.begin(), y.end());
    tap.last_audio_ms = t;
    if (tap.cur.size() >= static_cast<size_t>(MAX_UTTERANCE_S * RATE)) finish(tap, vfo, true);
}

// the VFO's DoA while the transmission lasts: confidence-weighted circular
// mean of the frames published during it (coherent mode, DoA on)
void Worker::sample_doa(Tap& tap, int vfo) {
    if (!tap.rec || multi_tuner_mode() || !doa_enabled.load(std::memory_order_relaxed)) return;
    auto inst = decimator_manager.getDecimator(vfo);
    if (!inst || !inst->music_processor) return;
    MUSICProcessor& mp = *inst->music_processor;
    if (!mp.isEnabled() || mp.isResultStale()) return;
    const int64_t stamp = mp.getResultStampMs();
    if (stamp == tap.doa_stamp || stamp < tap.t0_ms - 300) return;
    tap.doa_stamp = stamp;
    auto [bearing, conf] = mp.getPeakAngleWithConfidence();
    if (bearing < 0 || !is_finite_value(bearing)) return;
    // unit circle -> compass sense, relative to the array (the DoA logger's app_bearing)
    double app = 360.0 - bearing;
    if (app >= 360.0) app -= 360.0;
    const double w = std::max(0.05, static_cast<double>(is_finite_value(conf) ? conf : 0.0f));
    tap.dsx += w * std::cos(app * M_PI / 180.0);
    tap.dsy += w * std::sin(app * M_PI / 180.0);
    tap.dw += w;
}

void Worker::finish(Tap& tap, int vfo, bool more) {
    Utterance u;
    u.vfo = vfo;
    u.rf = tap.rf;
    u.src = tap.src;
    u.tid = tap.tid == "?" ? "" : tap.tid;
    u.tlabel = tap.tlabel;
    u.t0_ms = tap.t0_ms;
    u.part = more || tap.part > 0 ? tap.part + 1 : 0;
    if (tap.dw > 0) {
        double d = std::atan2(tap.dsy, tap.dsx) * 180.0 / M_PI;
        if (d < 0) d += 360.0;
        u.doa = d;
        const StationLocation sl = station_info.resolve();
        if (sl.heading_valid && is_finite_value(sl.heading)) u.brg = std::fmod(d + sl.heading + 720.0, 360.0);
    }
    u.pcm.resize(tap.cur.size());
    for (size_t i = 0; i < tap.cur.size(); i++)
        u.pcm[i] = static_cast<int16_t>(std::lround(std::clamp(tap.cur[i], -1.0f, 1.0f) * 32767.0f));
    const double dur = u.seconds();
    if (more) {
        // a long transmission: this part goes, the next continues
        tap.t0_ms += static_cast<int64_t>(dur * 1000);
        tap.part++;
        tap.dsx = tap.dsy = tap.dw = 0;
    } else {
        tap.rec = false;
        tap.part = 0;
    }
    tap.cur.clear();
    tap.closed = 0;
    tap.anon_from = -1;
    if (dur >= MIN_UTTERANCE_S) enqueue(std::move(u));
}

// The transmission ends at sample `at` of what was collected; the rest
// starts the next one (another radio took over there, still unnamed)
void Worker::split(Tap& tap, int vfo, size_t at) {
    at = std::min(at, tap.cur.size());
    std::vector<float> rest(tap.cur.begin() + static_cast<long>(at), tap.cur.end());
    const int64_t t_rest = tap.t0_ms + static_cast<int64_t>(at * 1000 / RATE);
    tap.cur.resize(at);
    finish(tap, vfo, false);
    if (rest.empty()) return;
    tap.rec = true;
    tap.cur = std::move(rest);
    tap.t0_ms = t_rest;
    tap.part = 0;
    tap.tid.clear();
    tap.tlabel.clear();
    tap.dsx = tap.dsy = tap.dw = 0;
    tap.doa_stamp = 0;
}

void Worker::enqueue(Utterance&& u) {
    pending_.push_back(std::move(u));
    double total = 0;
    for (const auto& p : pending_) total += p.seconds();
    uint64_t dropped = 0;
    while (pending_.size() > 1 && (pending_.size() > MAX_QUEUED_JOBS || total > MAX_QUEUED_S)) {
        total -= pending_.front().seconds();
        pending_.pop_front();   // the helper can't keep up: the oldest go
        dropped++;
    }
    std::lock_guard<std::mutex> lk(s_.st_mu);
    s_.dropped += dropped;
}

void Worker::stop_helper(const std::string& why) {
    if (proc_on_) {
        proc_.stop();
        if (!why.empty()) std::cout << "Voice to Text: helper stopped (" << why << ")" << std::endl;
    }
    proc_on_ = ready_ = false;
    sent_valid_ = false;
    if (has_job_) {
        // put the job back for the next helper (once)
        if (job_.retries++ < 1) pending_.push_front(std::move(job_));
        else {
            std::lock_guard<std::mutex> lk(s_.st_mu);
            s_.failed++;
        }
        has_job_ = false;
    }
}

void Worker::start_helper(const Config& c, int64_t t) {
    const std::string exe = helper_exe(), mp = model_path(c.model);
    std::vector<std::string> args{exe, "--serve", "--model", mp, "--threads", std::to_string(c.threads)};
    const std::string vad = vad_model_path();
    if (!vad.empty()) {
        args.push_back("--vad");
        args.push_back(vad);
    }
    std::string err;
    if (!proc_.start(args, &err)) {
        set_state("error", "cannot start " + exe + ": " + err);
        next_start_ms_ = t + backoff_s_ * 1000;
        backoff_s_ = std::min(backoff_s_ * 2, 30);
        return;
    }
    proc_on_ = true;
    ready_ = false;
    proc_model_ = c.model;
    sent_valid_ = false;
    set_state("starting");
    std::cout << "Voice to Text: helper started, model " << c.model << (vad.empty() ? " (no VAD model)" : "")
              << std::endl;
}

void Worker::sync_options(const Config& c) {
    auto opt = [&](const std::string& kv) { proc_.send(wire::OPTION, kv.data(), kv.size(), SIZE_MAX); };
    if (!sent_valid_ || c.language != sent_.language) opt("language=" + c.language);
    if (!sent_valid_ || c.prompt != sent_.prompt) opt("prompt=" + c.prompt);
    if (!sent_valid_ || c.threads != sent_.threads) opt("threads=" + std::to_string(c.threads));
    if (!sent_valid_ || c.vad != sent_.vad) opt(std::string("vad=") + (c.vad ? "1" : "0"));
    if (!sent_valid_ || c.translate != sent_.translate) opt(std::string("translate=") + (c.translate ? "1" : "0"));
    sent_ = c;
    sent_valid_ = true;
}

void Worker::on_result(const char* p, size_t len) {
    if (len < 20 || !has_job_) return;
    uint32_t id, ms;
    float conf, nsp;
    int32_t speech_ms;
    memcpy(&id, p, 4);
    memcpy(&conf, p + 4, 4);
    memcpy(&nsp, p + 8, 4);
    memcpy(&ms, p + 12, 4);
    memcpy(&speech_ms, p + 16, 4);
    if (id != job_id_) return;
    std::vector<std::string> f;
    size_t a = 20;
    for (size_t i = 20; i <= len && f.size() < 3; i++)
        if (i == len || p[i] == '\0') {
            f.emplace_back(p + a, i - a);
            a = i + 1;
        }
    f.resize(3);
    Utterance u = std::move(job_);
    has_job_ = false;
    backoff_s_ = 2;   // a job went through: the helper works
    const double dur = u.seconds();
    bool keep_audio;
    {
        std::lock_guard<std::mutex> lk(s_.cfg_mu);
        keep_audio = s_.cfg.keep_audio;
    }
    Transcript t;
    t.text = f[1].substr(0, 4000);
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(s_.st_mu);
        s_.last_ms = ms;
        s_.last_error.clear();   // the helper works
        if (dur > 0) s_.rtf = s_.rtf > 0 ? 0.8 * s_.rtf + 0.2 * (ms / 1000.0 / dur) : ms / 1000.0 / dur;
        if (t.text.empty()) {
            s_.nospeech++;
            return;
        }
        s_.done++;
        t.id = s_.last_id = std::max<uint64_t>(s_.last_id + 1, static_cast<uint64_t>(u.t0_ms));
        gen = s_.clear_gen;
    }
    t.t_ms = u.t0_ms;
    t.dur = dur;
    t.vfo = u.vfo;
    t.mhz = u.rf / 1e6;
    t.src = u.src;
    t.tid = u.tid;
    t.tlabel = u.tlabel;
    t.lang = f[0];
    t.doa = u.doa;
    t.brg = u.brg;
    t.conf = is_finite_value(conf) ? std::clamp(conf, 0.0f, 1.0f) : 0.0f;
    t.proc_ms = ms;
    t.part = u.part;
    const std::string clip = std::string(CLIP_DIR) + "/" + std::to_string(t.id) + ".wav";
    if (keep_audio) {
        mkdir(CLIP_DIR, 0755);
        t.clip = write_wav(clip, u.pcm);
    }
    {
        // the clip, the file and the history together: a Clear (VOICE2TEXT_CLEAR)
        // since the id was taken drops this result instead of bringing it back
        std::lock_guard<std::mutex> lk(s_.st_mu);
        if (gen != s_.clear_gen) {
            if (t.clip) unlink(clip.c_str());
            return;
        }
        if (t.clip) {
            s_.clips.push_back(t.id);
            s_.clip_bytes += file_size(clip);
            trim_clips_locked(s_);
        }
        s_.history.push_back(t);
        while (s_.history.size() > MAX_HISTORY) s_.history.pop_front();
        s_.fresh.push_back(t);
        if (s_.fresh.size() > 200) s_.fresh.erase(s_.fresh.begin());
        std::ofstream o(HISTORY_FILE, std::ios::app);
        o << transcript_json(t) << "\n";
        o.close();
        if (++s_.file_lines > MAX_HISTORY * 3) rewrite_history_locked(s_);
    }
    // the decoder data log (type "voice")
    std::string fields = ",\"text\":\"" + json_escape(t.text) + "\",\"src\":\"" + json_escape(t.src) + "\",\"dur\":" +
                         fmt("%.1f", t.dur) + ",\"conf\":" + fmt("%.2f", t.conf);
    if (!t.tid.empty()) fields += ",\"from\":\"" + json_escape(t.tid) + "\"";
    if (!t.tlabel.empty()) fields += ",\"to\":\"" + json_escape(t.tlabel) + "\"";
    if (is_finite_value(t.doa)) fields += ",\"doa\":" + fmt("%.1f", t.doa);
    if (is_finite_value(t.brg)) fields += ",\"brg\":" + fmt("%.1f", t.brg);
    if (!t.lang.empty() && t.lang != "en") fields += ",\"lang\":\"" + json_escape(t.lang) + "\"";
    if (t.clip) fields += ",\"clip\":\"" + clip + "\"";
    declog::record_voice(t.vfo, u.rf, t.t_ms, u.src, fields);
}

void Worker::helper(int64_t t) {
    Config c;
    {
        std::lock_guard<std::mutex> lk(s_.cfg_mu);
        c = s_.cfg;
    }
    const bool any_vfo = !vfo_cfg_.empty();
    if (any_vfo || !pending_.empty() || has_job_) last_wanted_ms_ = t;
    const bool want = t - last_wanted_ms_ < HELPER_IDLE_STOP_MS;
    {
        std::lock_guard<std::mutex> lk(s_.st_mu);
        double q = 0;
        for (const auto& p : pending_) q += p.seconds();
        s_.queue = pending_.size() + (has_job_ ? 1 : 0);
        s_.queued_audio_s = q + (has_job_ ? job_.seconds() : 0);
    }
    if (!want) {
        if (proc_on_) stop_helper("idle");
        set_state(any_vfo ? "starting" : "off");
        return;
    }
    // the model changed: restart
    if (proc_on_ && proc_model_ != c.model) stop_helper("model changed to " + c.model);
    if (proc_on_) {
        int status = 0;
        proc_.poll([&](uint32_t type, const char* p, size_t len) {
            if (type == wire::READY) {
                ready_ = true;
                std::string kv(p, len);
                std::lock_guard<std::mutex> lk(s_.st_mu);
                s_.multilingual = kv.find(std::string("multilingual=1")) != std::string::npos;
                s_.vad_loaded = kv.find(std::string("vad=1")) != std::string::npos;
                s_.last_error.clear();   // the model loaded
                s_.loaded = c.model;
                const size_t sy = kv.find(std::string("\0system=", 8));
                s_.system = sy == std::string::npos ? "" : kv.substr(sy + 8, 300);
            } else if (type == wire::RESULT) {
                on_result(p, len);
            } else if (type == wire::ERROR) {
                set_state("error", std::string(p, std::min<size_t>(len, 300)));
            }
        });
        if (proc_.protocol_error() || !proc_.alive(&status)) {
            std::string why = "the helper stopped";
            if (WIFSIGNALED(status)) why += " (signal " + std::to_string(WTERMSIG(status)) + ")";
            else if (WIFEXITED(status)) why += " (exit " + std::to_string(WEXITSTATUS(status)) + ")";
            const auto tail = proc_.stderr_tail();
            if (!tail.empty()) why += ": " + tail.back();
            std::cerr << "Voice to Text: " << why << std::endl;
            stop_helper("");
            set_state("error", why);
            next_start_ms_ = t + backoff_s_ * 1000;
            backoff_s_ = std::min(backoff_s_ * 2, 30);
            return;
        }
        // a job stuck far longer than it can take: the helper is killed
        if (has_job_ && t - job_sent_ms_ > std::max<int64_t>(90000, static_cast<int64_t>(job_.seconds() * 15000))) {
            job_.retries = 1;   // not again
            stop_helper("job timed out");
            set_state("error", "a transcription took too long - helper restarted");
            return;
        }
    }
    if (!proc_on_) {
        // nothing to transcribe with: what was collected goes, nothing more is
        usable_ = file_exists(helper_exe()) && valid_model_name(c.model) && file_exists(model_path(c.model));
        if (!usable_) {
            pending_.clear();
            if (!file_exists(helper_exe())) set_state("not installed");
            else set_state("no model", "model " + c.model + " is not downloaded");
            return;
        }
        if (t < next_start_ms_) return;
        start_helper(c, t);
        return;
    }
    if (!ready_) return;
    sync_options(c);
    if (!has_job_ && !pending_.empty()) {
        job_ = std::move(pending_.front());
        pending_.pop_front();
        has_job_ = true;
        job_id_++;
        std::string payload(4 + job_.pcm.size() * 2, '\0');
        memcpy(&payload[0], &job_id_, 4);
        memcpy(&payload[4], job_.pcm.data(), job_.pcm.size() * 2);
        proc_.send(wire::JOB, payload.data(), payload.size(), SIZE_MAX);
        job_sent_ms_ = t;
    }
    set_state(has_job_ ? "busy" : "ready");
}

void Worker::run() {
    std::deque<Input> batch;
    while (s_.running.load()) {
        {
            std::unique_lock<std::mutex> lk(s_.q_mu);
            s_.q_cv.wait_for(lk, std::chrono::milliseconds(proc_on_ && has_job_ ? 20 : 50),
                             [&] { return !s_.q.empty() || !s_.running.load(); });
            batch.swap(s_.q);
            s_.queued_s.clear();
        }
        const int64_t t = now_ms();
        scan_vfos(t);
        if (usable_)
            for (auto& in : batch) take_input(in, t);
        batch.clear();
        for (auto& kv : taps_) {
            Tap& tap = kv.second;
            sample_doa(tap, kv.first);
            if (tap.rec && tap.digital && t - tap.last_audio_ms > DIGITAL_GAP_MS) finish(tap, kv.first, false);
            // analog input stopped mid-transmission (heimdall gone, the VFO
            // no longer decimated): what was collected goes; the demod restarts
            if (tap.rec && !tap.digital && t - tap.last_audio_ms > ANALOG_STALL_MS) {
                finish(tap, kv.first, false);
                tap.rate = 0;
            }
        }
        helper(t);
    }
    // shutdown: what was collected is lost; the helper exits on EOF
    stop_helper("");
}

void push_input(Input&& in, double seconds) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.q_mu);
        double& q = s.queued_s[in.vfo];
        if (q > MAX_INPUT_S) return;   // the worker is far behind
        q += seconds;
        s.q.push_back(std::move(in));
    }
    s.q_cv.notify_one();
}

// --- model download (curl, in the background) ----------------------------------

void download_thread(std::vector<std::pair<std::string, std::string>> files) {
    State& s = S();
    std::string err;
    for (const auto& f : files) {
        const std::string url = f.first, out = f.second, part = out + ".part";
        mkdir(transcribe_dir().c_str(), 0755);
        mkdir(models_dir().c_str(), 0755);
        std::vector<std::string> args{"curl", "-fsSL", "--retry", "3", "--connect-timeout", "20", "-o", part, url};
        std::vector<char*> av;
        for (auto& a : args) av.push_back(a.data());
        av.push_back(nullptr);
        pid_t pid = -1;
        const int rc = posix_spawnp(&pid, "curl", nullptr, nullptr, av.data(), environ);
        if (rc != 0) {
            err = "cannot run curl (" + std::string(strerror(rc)) + ") - install it, or use transcribe/setup.sh";
            break;
        }
        {
            std::lock_guard<std::mutex> lk(s.st_mu);
            s.dl.pid = pid;
            s.dl.part = part;
        }
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        {
            std::lock_guard<std::mutex> lk(s.st_mu);
            s.dl.pid = -1;
        }
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || file_size(part) < 100000) {
            unlink(part.c_str());
            err = WIFEXITED(st) ? "download failed (curl exit " + std::to_string(WEXITSTATUS(st)) + ")" : "download stopped";
            break;
        }
        if (rename(part.c_str(), out.c_str()) != 0) {
            err = "cannot save " + out;
            break;
        }
    }
    std::lock_guard<std::mutex> lk(s.st_mu);
    s.dl.running = false;
    s.dl.error = err;
    std::cout << "Voice to Text: model download " << s.dl.name << (err.empty() ? " done" : ": " + err) << std::endl;
}

}  // namespace

// --- public API -------------------------------------------------------------------

void start() {
    State& s = S();
    if (s.running.exchange(true)) return;
    load_history();
    // the digital decoders' voice (8 kHz, the lead plugin's) of VFOs with Transcribe on
    dig::set_voice_tap_handler([](dig::DigitalDecoder* d, const float* x, size_t n, const std::string& plugin,
                                  const std::string& tid, const std::string& tlabel, double rf_hz) {
        if (!d || n == 0) return;
        Input in;
        in.vfo = d->vfo();
        in.digital = true;
        in.a8k.assign(x, x + n);
        in.rf = rf_hz;
        in.plugin = plugin;
        in.tid = tid;
        in.tlabel = tlabel;
        push_input(std::move(in), n / 8000.0);
    });
    s.worker = std::thread([] {
        Worker w;
        w.run();
    });
}

void stop() {
    State& s = S();
    if (!s.running.exchange(false)) return;
    dig::set_voice_tap_handler(nullptr);
    s.q_cv.notify_all();
    if (s.worker.joinable()) s.worker.join();
    std::lock_guard<std::mutex> lk(s.st_mu);
    if (s.dl.pid > 0) kill(s.dl.pid, SIGTERM);
}

void feed_analog(int vfo, const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, DemodulatorMode mode,
                 bool open) {
    if (!S().running.load(std::memory_order_relaxed) || n == 0 || !(rate_hz > 1000)) return;
    Input in;
    in.vfo = vfo;
    in.iq.assign(x, x + n);
    in.rate = rate_hz;
    in.rf = rf_hz;
    in.mode = mode == DemodulatorMode::DIGITAL ? DemodulatorMode::NBFM : mode;
    in.open = open;
    push_input(std::move(in), n / rate_hz);
}

bool set_model(const std::string& name, std::string* err) {
    if (!valid_model_name(name)) {
        *err = "invalid model name";
        return false;
    }
    State& s = S();
    std::lock_guard<std::mutex> lk(s.cfg_mu);
    s.cfg.model = name;
    return true;
}

bool set_language(const std::string& code) {
    if (code != "auto" && (code.size() < 2 || code.size() > 8)) return false;
    for (char c : code)
        if (!std::islower(static_cast<unsigned char>(c)) && c != '-') return false;
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.language = code;
    return true;
}

void set_threads(int n) {
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.threads = std::clamp(n, 1, 4);
}

bool set_prompt(const std::string& p) {
    if (p.size() > 300) return false;
    for (unsigned char c : p)
        if (c < 0x20 || c == 0x7F) return false;
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.prompt = p;
    return true;
}

void set_vad(bool on) {
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.vad = on;
}

void set_translate(bool on) {
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.translate = on;
}

void set_keep_audio(bool on) {
    std::lock_guard<std::mutex> lk(S().cfg_mu);
    S().cfg.keep_audio = on;
}

void vfo_changed(int, bool) { S().q_cv.notify_one(); }

std::string status_message() {
    State& s = S();
    Config c;
    {
        std::lock_guard<std::mutex> lk(s.cfg_mu);
        c = s.cfg;
    }
    const auto inst = installed_models();
    const std::string vad = vad_model_path();
    const bool have_exe = file_exists(helper_exe());
    std::ostringstream o;
    o << "{\"voice2text\":{\"status\":{\"installed\":" << (have_exe ? "true" : "false") << ",\"dir\":\""
      << json_escape(transcribe_dir()) << "\",\"models_dir\":\"" << json_escape(models_dir()) << "\",\"models\":[";
    for (size_t i = 0; i < inst.size(); i++) {
        const CatalogueEntry* ce = catalogue(inst[i]);
        o << (i ? "," : "") << "{\"name\":\"" << json_escape(inst[i]) << "\",\"mb\":"
          << file_size(model_path(inst[i])) / 1048576 << ",\"multi\":" << (model_multilingual(inst[i]) ? "true" : "false")
          << ",\"note\":\"" << json_escape(ce ? ce->note : "") << "\"}";
    }
    o << "],\"catalogue\":[";
    bool first = true;
    for (const auto& ce : MODEL_CATALOGUE) {
        o << (first ? "" : ",") << "{\"name\":\"" << ce.name << "\",\"mb\":" << ce.mb << ",\"multi\":"
          << (ce.multi ? "true" : "false") << ",\"note\":\"" << json_escape(ce.note) << "\",\"have\":"
          << (std::find(inst.begin(), inst.end(), ce.name) != inst.end() ? "true" : "false") << "}";
        first = false;
    }
    o << "],\"vad_model\":" << (vad.empty() ? "false" : "true") << ",\"model\":\"" << json_escape(c.model)
      << "\",\"language\":\"" << json_escape(c.language) << "\",\"threads\":" << c.threads << ",\"prompt\":\""
      << json_escape(c.prompt) << "\",\"vad\":" << (c.vad ? "true" : "false") << ",\"translate\":"
      << (c.translate ? "true" : "false") << ",\"keep_audio\":" << (c.keep_audio ? "true" : "false")
      << ",\"model_multi\":" << (model_multilingual(c.model) ? "true" : "false")
      << ",\"log\":{\"enabled\":" << (declog::type_logged("voice") ? "true" : "false") << "}";
    std::lock_guard<std::mutex> lk(s.st_mu);
    o << ",\"state\":\"" << s.state << "\",\"error\":\"" << json_escape(s.last_error) << "\",\"loaded\":\""
      << json_escape(s.loaded) << "\",\"vad_loaded\":" << (s.vad_loaded ? "true" : "false")
      << ",\"system\":\"" << json_escape(s.system) << "\",\"queue\":" << s.queue << ",\"queued_s\":"
      << fmt("%.1f", s.queued_audio_s) << ",\"dropped\":" << s.dropped << ",\"done\":" << s.done
      << ",\"nospeech\":" << s.nospeech << ",\"failed\":" << s.failed << ",\"rtf\":" << fmt("%.2f", s.rtf)
      << ",\"last_ms\":" << s.last_ms << ",\"clips\":" << s.clips.size() << ",\"clip_mb\":"
      << fmt("%.1f", s.clip_bytes / 1048576.0) << ",\"vfos\":[";
    for (size_t i = 0; i < s.vfos.size(); i++) {
        const auto& v = s.vfos[i];
        o << (i ? "," : "") << "{\"id\":" << v.id << ",\"src\":\"" << json_escape(v.src) << "\",\"rec\":"
          << (v.rec ? "true" : "false") << ",\"secs\":" << fmt("%.1f", v.secs) << ",\"digital\":"
          << (v.digital ? "true" : "false") << "}";
    }
    o << "]";
    if (!s.dl.name.empty()) {
        const uint64_t got = s.dl.running ? file_size(s.dl.part) : 0;
        o << ",\"download\":{\"name\":\"" << json_escape(s.dl.name) << "\",\"running\":"
          << (s.dl.running ? "true" : "false") << ",\"pct\":"
          << (s.dl.total ? std::min<uint64_t>(99, got * 100 / s.dl.total) : 0) << ",\"error\":\""
          << json_escape(s.dl.error) << "\"}";
    }
    o << "}}}";
    return o.str();
}

std::string history_message(size_t max) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    std::string o = "{\"voice2text\":{\"history\":[";
    const size_t n = std::min(max, s.history.size());
    for (size_t i = s.history.size() - n; i < s.history.size(); i++) {
        o += transcript_json(s.history[i]);
        if (i + 1 < s.history.size()) o += ",";
    }
    s.fresh.clear();   // they are in the history the pages get now
    return o + "]}}";
}

std::string take_new_message() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    if (s.fresh.empty()) return "";
    std::string o = "{\"voice2text\":{\"new\":[";
    for (size_t i = 0; i < s.fresh.size(); i++) o += (i ? "," : "") + transcript_json(s.fresh[i]);
    s.fresh.clear();
    return o + "]}}";
}

void clear_history() {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    s.history.clear();
    s.fresh.clear();
    s.clear_gen++;
    s.file_lines = 0;
    FILE* f = fopen(HISTORY_FILE, "w");
    if (f) fclose(f);
    for (uint64_t id : s.clips) unlink((std::string(CLIP_DIR) + "/" + std::to_string(id) + ".wav").c_str());
    s.clips.clear();
    s.clip_bytes = 0;
}

std::string clip_path(uint64_t id) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    if (std::find(s.clips.begin(), s.clips.end(), id) == s.clips.end()) return "";
    return std::string(CLIP_DIR) + "/" + std::to_string(id) + ".wav";
}

bool download_model(const std::string& name, std::string* err) {
    const CatalogueEntry* ce = catalogue(name);
    if (!ce) {
        *err = "unknown model";
        return false;
    }
    State& s = S();
    std::vector<std::pair<std::string, std::string>> files;
    if (!file_exists(model_path(name))) files.push_back({MODEL_URL + name + ".bin", model_path(name)});
    if (vad_model_path().empty())
        files.push_back({std::string(VAD_URL) + VAD_NAME + ".bin", models_dir() + "/ggml-" + VAD_NAME + ".bin"});
    if (files.empty()) {
        *err = "already downloaded";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(s.st_mu);
        if (s.dl.running) {
            *err = "a download is already running";
            return false;
        }
        s.dl = Download{};
        s.dl.name = name;
        s.dl.running = true;
        s.dl.total = static_cast<uint64_t>(ce->mb) * 1048576;
    }
    std::cout << "Voice to Text: downloading model " << name << std::endl;
    std::thread(download_thread, std::move(files)).detach();
    return true;
}

}  // namespace v2t
