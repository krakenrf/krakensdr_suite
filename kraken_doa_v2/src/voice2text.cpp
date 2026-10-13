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
#include <opus/opus.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
// Long audio is cut into pieces, at a pause where there is one. Two-way radio:
// a transmission is ONE transcript up to TWOWAY_MAX_S (its text comes when it
// ends anyway, and whisper does better with the whole of it); longer, it is
// cut there at its latest pause after TWOWAY_MIN_S. Continuous signals - WBFM
// broadcast, or anything open longer than CONTINUOUS_S (AM broadcast, a
// weather channel) - from CONT_SOFT_S on at their latest pause after
// CONT_MIN_S (at the latest CONT_MAX_S), so the text follows ~10-20 s
// behind. Pause = PAUSE_FRAMES 20 ms frames under a tenth of the piece's
// median energy (on hissy audio twice its noise floor - see maybe_cut). No
// pause by the maximum: cut at the quietest 100 ms of the last 3 s, the next
// piece repeating OVERLAP_S before it (a word cut there is in one of them whole)
constexpr double CONTINUOUS_S = 30.0;
constexpr double TWOWAY_MIN_S = 11.0, TWOWAY_MAX_S = 25.0;
constexpr double CONT_MIN_S = 5.0, CONT_SOFT_S = 8.0, CONT_MAX_S = 15.0;
constexpr size_t FRAME = 320;
constexpr size_t PAUSE_FRAMES = 12;
constexpr double OVERLAP_S = 0.3;
// Each transcript's lobe (the 🗺 map shows it from the 🗣 Voice tab): the MUSIC
// pseudospectra published during it, each min..max -> 0..1, averaged with the
// bearing's weights; LOBE_BINS of 0.5 deg clockwise from north (as the map's
// live lobe, rdf_mapper.cpp display_lobe) - or from the array's front when
// the station had no heading
constexpr int LOBE_BINS = 720;
constexpr double MAX_QUEUED_S = 600;    // transmissions waiting for the helper (then the oldest go)
constexpr size_t MAX_QUEUED_JOBS = 40;
constexpr double MAX_INPUT_S = 3.0;     // per VFO, audio waiting for the worker
constexpr size_t MAX_HISTORY = 1000;
constexpr int64_t HELPER_IDLE_STOP_MS = 120000;   // no VFO transcribing: the helper (its RAM) goes
constexpr const char* HISTORY_FILE = "transcripts.jsonl";
// Audio clips: Ogg Opus (16 kbps, ~7 MB an hour of speech) in the decoder
// log's folder, <dir>/voice_clips/YYYY-MM-DD/<id>.opus, kept as many days as
// the decoder log keeps its files; not written below MIN_FREE_BYTES free;
// MAX_CLIP_BYTES = a backstop (the oldest go)
constexpr const char* CLIP_DIR = "voice_clips";
constexpr int CLIP_BITRATE = 16000;
constexpr uint64_t MAX_CLIP_BYTES = 2ull << 30;
constexpr uint64_t MIN_FREE_BYTES = 100ull << 20;

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
    std::string lobe;           // b64 of LOBE_BINS u8 ("" = none)
    bool lobe_north = false;    // false: relative to the array (no heading)
    double slat = NAN, slon = NAN;   // where the station was
    int part = 0;               // > 0: a part of a longer transmission
    int retries = 0;
    double seconds() const { return pcm.size() / static_cast<double>(RATE); }
};

struct ClipFile {
    std::string path;   // <log folder>/voice_clips/YYYY-MM-DD/<id>.opus
    uint64_t bytes = 0;
};

struct Transcript {
    uint64_t id = 0;
    int64_t t_ms = 0;
    double dur = 0;
    int vfo = -1;
    double mhz = 0;
    std::string src, tid, tlabel, lang, text;
    double doa = NAN, brg = NAN;
    std::string lobe;           // see Utterance
    bool lobe_north = false;
    double slat = NAN, slon = NAN;
    float conf = 0;
    uint32_t proc_ms = 0;
    bool clip = false;
    int part = 0;
};

std::string b64(const uint8_t* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += i + 1 < n ? T[(v >> 6) & 63] : '=';
        o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

// a stored lobe as read back: LOBE_BINS bytes of base64, nothing else
bool lobe_ok(const std::string& b) {
    if (b.size() != (LOBE_BINS + 2) / 3 * 4) return false;
    for (char c : b)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=')) return false;
    return true;
}

// full = the history file (with the lobe); else the list pushed to the pages,
// which only says there is one ("lb") - VOICE2TEXT_LOBE:<id> fetches it
std::string transcript_json(const Transcript& t, bool full) {
    std::string j = "{\"id\":" + std::to_string(t.id) + ",\"t\":" + std::to_string(t.t_ms) + ",\"dur\":" +
                    fmt("%.1f", t.dur) + ",\"vfo\":" + std::to_string(t.vfo) + ",\"mhz\":" + fmt("%.6f", t.mhz) +
                    ",\"src\":\"" + json_escape(t.src) + "\"";
    if (!t.tid.empty()) j += ",\"tid\":\"" + json_escape(t.tid) + "\"";
    if (!t.tlabel.empty()) j += ",\"tl\":\"" + json_escape(t.tlabel) + "\"";
    if (is_finite_value(t.doa)) j += ",\"doa\":" + fmt("%.1f", t.doa);
    if (is_finite_value(t.brg)) j += ",\"brg\":" + fmt("%.1f", t.brg);
    if (!t.lobe.empty()) {
        if (full) {
            j += ",\"lobe\":\"" + t.lobe + "\",\"lf\":\"" + (t.lobe_north ? "n" : "a") + "\"";
            if (is_finite_value(t.slat) && is_finite_value(t.slon))
                j += ",\"slat\":" + fmt("%.6f", t.slat) + ",\"slon\":" + fmt("%.6f", t.slon);
        } else {
            j += ",\"lb\":\"" + std::string(t.lobe_north ? "n" : "a") + "\"";
        }
    }
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
    if (json_find(line, "lobe", v) && lobe_ok(v)) {
        t->lobe = v;
        t->lobe_north = json_find(line, "lf", v) && v == "n";
        std::string la, lo;
        if (json_find(line, "slat", la) && json_find(line, "slon", lo)) {
            try {
                t->slat = stod_finite(la);
                t->slon = stod_finite(lo);
            } catch (const std::exception&) {
                t->slat = t->slon = NAN;   // the lobe without its place (the map takes the station's)
            }
        }
    }
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
    size_t tx_samples = 0;          // the whole transmission so far (all its parts)
    // 20 ms frame energies of cur (pause search, maybe_cut) + the partial frame
    std::vector<float> fe;
    double fe_acc = 0;
    size_t fe_n = 0;
    // bearing during it
    int64_t doa_stamp = 0;
    double dsx = 0, dsy = 0, dw = 0;
    // its lobe (LOBE_BINS): the frames turned to north by the heading of
    // their moment (those with a heading), and relative to the array (all)
    std::vector<float> lobe_n, lobe_a;
    double lw_n = 0, lw_a = 0;
    double plat = 0, plon = 0;      // the station's position, summed (lon relative to plon0)
    double plon0 = 0;
    int pn = 0;
    void reset_doa() {
        dsx = dsy = dw = 0;
        lobe_n.clear();
        lobe_a.clear();
        lw_n = lw_a = 0;
        plat = plon = 0;
        pn = 0;
    }
    void add_fe(float v) {
        fe_acc += static_cast<double>(v) * v;
        if (++fe_n == FRAME) {
            fe.push_back(static_cast<float>(fe_acc / FRAME));
            fe_acc = 0;
            fe_n = 0;
        }
    }
    void append(const std::vector<float>& y) {
        cur.insert(cur.end(), y.begin(), y.end());
        tx_samples += y.size();
        for (float v : y) add_fe(v);
    }
    // cur was replaced: its frame energies again
    void refe() {
        fe.clear();
        fe_acc = 0;
        fe_n = 0;
        for (float v : cur) add_fe(v);
    }
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
    double in_dropped_s = 0;          // input dropped: the worker fell behind (q_mu)
    int64_t loop_max_ms = 0;          // the worker's slowest pass (st_mu)
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
    // audio clips in <decoder log folder>/voice_clips/ (the worker rescans on a
    // folder change), by id = oldest first
    std::map<uint64_t, ClipFile> clips;
    uint64_t clip_bytes = 0;
    std::string clip_root;        // <log folder>/voice_clips
    int clip_days = 0;            // the decoder log's Keep days (0 = forever)
    uint64_t clip_free = 0;       // free bytes there
    bool clip_low = false;        // < MIN_FREE_BYTES free: clips aren't written
    uint64_t clips_skipped = 0;   // not written: disk nearly full / write failed
    std::string clip_error;
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
    for (const auto& t : s.history) o << transcript_json(t, true) << "\n";
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
}

// --- audio clips: Ogg Opus files in the decoder log's folder ---------------------

std::string local_date(time_t t) {
    struct tm tm{};
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

// "YYYY-MM-DD" -> days since 1970, -1 = not a date
long day_number(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return -1;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit(static_cast<unsigned char>(d[i]))) return -1;
    struct tm tm{};
    tm.tm_year = std::stoi(d.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(d.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(d.substr(8, 2));
    return static_cast<long>(timegm(&tm) / 86400);
}

// "<digits>.opus" (also its ".tmp" leftover) - the only files retention touches
bool clip_name(const std::string& n, uint64_t* id, bool* tmp) {
    size_t i = 0;
    while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) i++;
    if (i == 0 || i > 20) return false;
    const std::string rest = n.substr(i);
    if (rest != ".opus" && rest != ".opus.tmp") return false;
    *id = strtoull(n.c_str(), nullptr, 10);
    *tmp = rest != ".opus";
    return true;
}

bool mkdirs(const std::string& path) {
    std::string cur = !path.empty() && path[0] == '/' ? "/" : "";
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        cur += "/";
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string absolute(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) return p;
    return std::string(cwd) + "/" + p;
}

// free bytes on the file system holding path (or its nearest existing parent)
bool free_space(std::string path, uint64_t* free_b) {
    path = absolute(path);
    for (int i = 0; i < 64 && !path.empty(); i++) {
        struct statvfs v{};
        if (statvfs(path.c_str(), &v) == 0) {
            *free_b = static_cast<uint64_t>(v.f_bavail) * v.f_frsize;
            return true;
        }
        const size_t sl = path.rfind('/');
        path = sl == 0 ? "/" : (sl == std::string::npos ? "" : path.substr(0, sl));
    }
    return false;
}

// the clips under root: <root>/YYYY-MM-DD/<id>.opus, by id (= oldest first)
std::map<uint64_t, ClipFile> scan_clips(const std::string& root) {
    std::map<uint64_t, ClipFile> m;
    DIR* d = opendir(root.c_str());
    if (!d) return m;
    while (dirent* e = readdir(d)) {
        const std::string day = e->d_name;
        if (day_number(day) < 0) continue;
        const std::string dir = root + "/" + day;
        if (DIR* d2 = opendir(dir.c_str())) {
            while (dirent* f = readdir(d2)) {
                uint64_t id;
                bool tmp;
                if (!clip_name(f->d_name, &id, &tmp) || tmp) continue;
                const std::string p = dir + "/" + f->d_name;
                m[id] = ClipFile{p, file_size(p)};
            }
            closedir(d2);
        }
    }
    closedir(d);
    return m;
}

// the day folders older than `days` (by name, like the decoder log's files):
// their clip files are deleted, then the folder if that emptied it. Only
// "YYYY-MM-DD" folders and "<id>.opus[.tmp]" files are touched
std::vector<std::string> clip_retention(const std::string& root, int days) {
    std::vector<std::string> gone;
    if (days <= 0) return gone;
    const long today = day_number(local_date(time(nullptr)));
    DIR* d = opendir(root.c_str());
    if (!d) return gone;
    std::vector<std::string> old;
    while (dirent* e = readdir(d)) {
        const long dn = day_number(e->d_name);
        if (dn >= 0 && today - dn > days) old.push_back(root + "/" + e->d_name);
    }
    closedir(d);
    for (const auto& dir : old) {
        if (DIR* d2 = opendir(dir.c_str())) {
            std::vector<std::string> files;
            while (dirent* f = readdir(d2)) {
                uint64_t id;
                bool tmp;
                if (clip_name(f->d_name, &id, &tmp)) files.push_back(dir + "/" + f->d_name);
            }
            closedir(d2);
            for (const auto& f : files) unlink(f.c_str());
        }
        rmdir(dir.c_str());   // only if nothing else is in it
        gone.push_back(dir + "/");
        std::cout << "Voice to Text: deleted the audio clips in " << dir << " (older than " << days << " days)"
                  << std::endl;
    }
    return gone;
}

// st_mu held: the oldest clips go while all of them pass MAX_CLIP_BYTES
void trim_clips_locked(State& s) {
    while (!s.clips.empty() && s.clip_bytes > MAX_CLIP_BYTES) {
        auto it = s.clips.begin();
        unlink(it->second.path.c_str());
        s.clip_bytes -= std::min(s.clip_bytes, it->second.bytes);
        s.clips.erase(it);
    }
}

void put_le16(std::string& s, uint16_t v) {
    for (int i = 0; i < 2; i++) s.push_back(static_cast<char>(v >> (8 * i)));
}
void put_le32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; i++) s.push_back(static_cast<char>(v >> (8 * i)));
}
void put_le64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; i++) s.push_back(static_cast<char>(v >> (8 * i)));
}

// Ogg's CRC-32 (polynomial 0x04C11DB7, not reflected, initial 0)
uint32_t ogg_crc(const std::string& d) {
    static const std::array<uint32_t, 256> T = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
            t[i] = r;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (unsigned char c : d) crc = (crc << 8) ^ T[((crc >> 24) ^ c) & 0xFF];
    return crc;
}

// one Ogg page holding whole packets (<= 255 lacing values)
void ogg_page(std::string& out, uint32_t serial, uint32_t seq, uint64_t granule, uint8_t flags,
              const std::vector<std::string>& packets) {
    std::string lacing, body;
    for (const auto& p : packets) {
        size_t n = p.size();
        for (; n >= 255; n -= 255) lacing.push_back(static_cast<char>(255));
        lacing.push_back(static_cast<char>(n));
        body += p;
    }
    std::string page = "OggS";
    page.push_back(0);   // version
    page.push_back(static_cast<char>(flags));
    put_le64(page, granule);
    put_le32(page, serial);
    put_le32(page, seq);
    put_le32(page, 0);   // CRC, below
    page.push_back(static_cast<char>(lacing.size()));
    page += lacing;
    page += body;
    const uint32_t crc = ogg_crc(page);
    for (int i = 0; i < 4; i++) page[22 + i] = static_cast<char>(crc >> (8 * i));
    out += page;
}

// An Ogg Opus file (RFC 7845) of 16 kHz mono speech: libopus (already linked
// for the live audio) at CLIP_BITRATE in 20 ms frames, ~1 s per page; tags
// = Vorbis comments ("KEY=value") so the file says what it is elsewhere too
bool write_opus(const std::string& path, const std::vector<int16_t>& pcm, const std::vector<std::string>& tags,
                uint32_t serial) {
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) return false;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(CLIP_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
    opus_int32 la = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&la));
    const uint16_t pre_skip = static_cast<uint16_t>(la * (48000 / RATE));   // in 48 kHz samples
    std::string out;
    uint32_t seq = 0;
    std::string head = "OpusHead";
    head.push_back(1);   // version
    head.push_back(1);   // channels
    put_le16(head, pre_skip);
    put_le32(head, RATE);   // the input's rate (informational)
    put_le16(head, 0);      // output gain
    head.push_back(0);      // mapping family 0: mono / stereo
    ogg_page(out, serial, seq++, 0, 0x02, {head});
    std::string tg = "OpusTags";
    const std::string vendor = std::string("kraken_doa Voice to Text, ") + opus_get_version_string();
    put_le32(tg, static_cast<uint32_t>(vendor.size()));
    tg += vendor;
    put_le32(tg, static_cast<uint32_t>(tags.size()));
    for (const auto& c : tags) {
        put_le32(tg, static_cast<uint32_t>(c.size()));
        tg += c;
    }
    ogg_page(out, serial, seq++, 0, 0, {tg});
    // frames until the encoder has put out the last input sample (its lookahead)
    constexpr size_t F = RATE / 50;   // 20 ms
    const size_t nframes = std::max<size_t>(1, (pcm.size() + static_cast<size_t>(la) + F - 1) / F);
    std::vector<opus_int16> frame(F);
    unsigned char buf[1500];
    std::vector<std::string> packets;
    bool ok = true;
    for (size_t k = 0; k < nframes && ok; k++) {
        for (size_t i = 0; i < F; i++) frame[i] = k * F + i < pcm.size() ? pcm[k * F + i] : 0;
        const int n = opus_encode(enc, frame.data(), static_cast<int>(F), buf, sizeof buf);
        if (n < 0) {
            ok = false;
            break;
        }
        packets.emplace_back(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
        const bool last = k + 1 == nframes;
        if (packets.size() == 50 || last) {
            // granule: 48 kHz samples decodable so far incl. the pre-skip; the
            // last page's says where the audio ends (the padding is trimmed)
            const uint64_t g = last ? pre_skip + static_cast<uint64_t>(pcm.size()) * (48000 / RATE)
                                    : static_cast<uint64_t>(k + 1) * 960;
            ogg_page(out, serial, seq++, g, last ? 0x04 : 0, packets);
            packets.clear();
        }
    }
    opus_encoder_destroy(enc);
    if (!ok) return false;
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool wrote = fwrite(out.data(), 1, out.size(), f) == out.size();
    if (fclose(f) != 0 || !wrote) {
        unlink(tmp.c_str());
        return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
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
    void maybe_cut(Tap& tap, int vfo);
    void cut(Tap& tap, int vfo, size_t at, size_t overlap);
    // audio clips: the decoder log's folder + Keep days, followed every 2 s
    std::string clip_root_, clip_day_;
    int clip_days_ = -1;
    int64_t clip_tick_ms_ = 0, clip_ret_ms_ = 0;
    void clips_tick(int64_t t);
    std::string save_clip(const Transcript& t, const Utterance& u);
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
// The clips follow the decoder log: its folder (a new one is scanned, the old
// one's clips stay there, unlisted) and its Keep days (retention every 10
// min, on a new day and when Keep changes); the free space every 2 s
void Worker::clips_tick(int64_t t) {
    if (t - clip_tick_ms_ < 2000) return;
    clip_tick_ms_ = t;
    const std::string root = declog::folder() + "/" + CLIP_DIR;
    const int days = declog::keep_days();
    const std::string today = local_date(time(nullptr));
    if (root != clip_root_) {
        clip_root_ = root;
        auto idx = scan_clips(root);   // file system work outside the lock
        uint64_t bytes = 0;
        for (const auto& kv : idx) bytes += kv.second.bytes;
        std::lock_guard<std::mutex> lk(s_.st_mu);
        s_.clips = std::move(idx);
        s_.clip_bytes = bytes;
        s_.clip_root = root;
        clip_ret_ms_ = 0;
    }
    if (days != clip_days_ || today != clip_day_) {
        clip_days_ = days;
        clip_day_ = today;
        clip_ret_ms_ = 0;
    }
    if (clip_ret_ms_ == 0 || t - clip_ret_ms_ >= 600000) {
        clip_ret_ms_ = t;
        const auto gone = clip_retention(root, days);
        std::lock_guard<std::mutex> lk(s_.st_mu);
        s_.clip_days = days;
        for (auto it = s_.clips.begin(); it != s_.clips.end();) {
            bool del = false;
            for (const auto& g : gone)
                if (it->second.path.rfind(g, 0) == 0) del = true;
            if (del) {
                s_.clip_bytes -= std::min(s_.clip_bytes, it->second.bytes);
                it = s_.clips.erase(it);
            } else {
                ++it;
            }
        }
        trim_clips_locked(s_);
    }
    uint64_t fr = 0;
    const bool known = free_space(root, &fr);
    std::lock_guard<std::mutex> lk(s_.st_mu);
    s_.clip_free = known ? fr : 0;
    s_.clip_low = known && fr < MIN_FREE_BYTES;
}

// A transcript's audio as <root>/<its local date>/<id>.opus ("" = not saved:
// disk nearly full, or it couldn't be written - counted, and said in the box)
std::string Worker::save_clip(const Transcript& t, const Utterance& u) {
    {
        std::lock_guard<std::mutex> lk(s_.st_mu);
        if (s_.clip_low || clip_root_.empty()) {
            s_.clips_skipped++;
            return "";
        }
    }
    const std::string dir = clip_root_ + "/" + local_date(static_cast<time_t>(t.t_ms / 1000));
    const std::string path = dir + "/" + std::to_string(t.id) + ".opus";
    char when[32];
    const time_t tt = static_cast<time_t>(t.t_ms / 1000);
    struct tm tm{};
    localtime_r(&tt, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
    std::vector<std::string> tags{"TITLE=" + t.text.substr(0, 1000), std::string("DATE=") + when,
                                  "KRAKEN_VFO=D" + std::to_string(t.vfo), "KRAKEN_MHZ=" + fmt("%.6f", t.mhz),
                                  "KRAKEN_SOURCE=" + t.src};
    if (!t.tid.empty()) tags.push_back("KRAKEN_TALKER=" + t.tid + (t.tlabel.empty() ? "" : " -> " + t.tlabel));
    if (is_finite_value(t.brg)) tags.push_back("KRAKEN_BEARING=" + fmt("%.1f", t.brg));
    if (mkdirs(dir) && write_opus(path, u.pcm, tags, static_cast<uint32_t>(t.id))) return path;
    std::lock_guard<std::mutex> lk(s_.st_mu);
    s_.clips_skipped++;
    s_.clip_error = "cannot write " + absolute(path) + ": " + strerror(errno);
    return "";
}

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
        tap.refe();
        tap.tx_samples = tap.cur.size();
        tap.t0_ms = t - static_cast<int64_t>(tap.pre.size() * 1000 / RATE);
        tap.pre.clear();
        tap.closed = 0;
        tap.part = 0;
        tap.src = DecimatorManager::demodModeToString(in.mode);
        tap.tid.clear();
        tap.tlabel.clear();
        tap.reset_doa();
        tap.doa_stamp = 0;
    }
    tap.append(y);
    tap.last_audio_ms = t;
    tap.closed = in.open ? 0 : tap.closed + y.size();
    if (tap.closed >= static_cast<size_t>(HANG_S * RATE)) {
        const size_t keep = static_cast<size_t>(TAIL_S * RATE);
        tap.cur.resize(tap.cur.size() - std::min(tap.cur.size(), tap.closed - keep));
        finish(tap, vfo, false);
    } else {
        maybe_cut(tap, vfo);
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
        tap.refe();
        tap.tx_samples = 0;
        tap.t0_ms = t - static_cast<int64_t>(y.size() * 1000 / RATE);
        tap.part = 0;
        tap.src = in.plugin;
        tap.tid = in.tid;
        tap.tlabel = in.tlabel;
        tap.rf = in.rf;
        tap.reset_doa();
        tap.doa_stamp = 0;
    } else if ((tap.tid.empty() || tap.tid == "?") && known) {
        tap.tid = in.tid;   // the radio was named after its first frames
        tap.tlabel = in.tlabel;
    } else if (known && !in.tlabel.empty()) {
        tap.tlabel = in.tlabel;
    }
    tap.append(y);
    tap.last_audio_ms = t;
    maybe_cut(tap, vfo);
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

    // the lobe: this frame's pseudospectrum, min..max -> 0..1 (the MUSIC DoA
    // plot's scale), added with the same weight; array angle a = index x res,
    // unit circle - compass tau = heading - a (rdf_mapper.cpp display_lobe)
    const auto spec = mp.getPseudospectrum();
    const int n = mp.getNumAngles();
    const double res = mp.getAngularResolution();
    if (static_cast<int>(spec.size()) != n || n < 8 || !(res > 0)) return;
    double mn = std::fabs(spec(0)), mx = mn;
    for (int i = 1; i < n; i++) {
        const double v = std::fabs(spec(i));
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    if (!(mx > mn) || !is_finite_value(mx)) return;
    auto at = [&](double a) {   // array angle (deg) -> 0..1, linear between the bins
        a = std::fmod(a, 360.0);
        if (a < 0) a += 360.0;
        const double x = a / res;
        int i0 = static_cast<int>(x);
        const double f = x - i0;
        i0 %= n;
        return static_cast<float>((std::fabs(spec(i0)) * (1 - f) + std::fabs(spec((i0 + 1) % n)) * f - mn) / (mx - mn));
    };
    const float wf = static_cast<float>(w);
    if (tap.lobe_a.empty()) tap.lobe_a.assign(LOBE_BINS, 0.0f);
    for (int k = 0; k < LOBE_BINS; k++) tap.lobe_a[k] += wf * at(-k * 360.0 / LOBE_BINS);   // heading 0
    tap.lw_a += w;
    const StationLocation sl = station_info.resolve();
    if (sl.heading_valid && is_finite_value(sl.heading)) {
        if (tap.lobe_n.empty()) tap.lobe_n.assign(LOBE_BINS, 0.0f);
        for (int k = 0; k < LOBE_BINS; k++) tap.lobe_n[k] += wf * at(sl.heading - k * 360.0 / LOBE_BINS);
        tap.lw_n += w;
    }
    if (!(sl.lat == 0.0 && sl.lon == 0.0) && is_finite_value(sl.lat) && is_finite_value(sl.lon)) {
        if (tap.pn == 0) tap.plon0 = sl.lon;
        tap.plat += sl.lat;
        tap.plon += std::remainder(sl.lon - tap.plon0, 360.0);   // across the antimeridian too
        tap.pn++;
    }
}

// a summed lobe -> LOBE_BINS u8 (min..max -> 0..255) as base64
std::string lobe_b64(const std::vector<float>& l) {
    if (l.size() != static_cast<size_t>(LOBE_BINS)) return "";
    const auto [lo, hi] = std::minmax_element(l.begin(), l.end());
    if (!(*hi > *lo)) return "";
    uint8_t u[LOBE_BINS];
    for (int k = 0; k < LOBE_BINS; k++) u[k] = static_cast<uint8_t>(std::lround(255.0f * (l[k] - *lo) / (*hi - *lo)));
    return b64(u, LOBE_BINS);
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
    // its lobe: turned to north when most of its frames had a heading, else
    // relative to the array; and where the station was
    const bool north = tap.lw_n > 0 && tap.lw_n >= 0.5 * tap.lw_a;
    u.lobe = lobe_b64(north ? tap.lobe_n : tap.lobe_a);
    u.lobe_north = north && !u.lobe.empty();
    if (!u.lobe.empty() && tap.pn > 0) {
        u.slat = tap.plat / tap.pn;
        u.slon = std::remainder(tap.plon0 + tap.plon / tap.pn, 360.0);
    }
    u.pcm.resize(tap.cur.size());
    for (size_t i = 0; i < tap.cur.size(); i++)
        u.pcm[i] = static_cast<int16_t>(std::lround(std::clamp(tap.cur[i], -1.0f, 1.0f) * 32767.0f));
    const double dur = u.seconds();
    if (more) {
        // a long transmission: this part goes, the next continues
        tap.t0_ms += static_cast<int64_t>(dur * 1000);
        tap.part++;
        tap.reset_doa();
    } else {
        tap.rec = false;
        tap.part = 0;
    }
    tap.cur.clear();
    tap.refe();
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
    tap.refe();
    tap.tx_samples = tap.cur.size();   // a new transmission
    tap.t0_ms = t_rest;
    tap.part = 0;
    tap.tid.clear();
    tap.tlabel.clear();
    tap.reset_doa();
    tap.doa_stamp = 0;
}

// A long piece (see TWOWAY_* / CONT_*): from its trigger length on (two-way:
// the maximum, continuous: CONT_SOFT_S) it is cut in the middle of its latest
// pause after the minimum; with no pause by the maximum, at the quietest
// 100 ms of the last 3 s, the next piece repeating OVERLAP_S before that
void Worker::maybe_cut(Tap& tap, int vfo) {
    const bool cont = (!tap.digital && tap.mode == DemodulatorMode::WBFM) ||
                      tap.tx_samples > static_cast<size_t>(CONTINUOUS_S * RATE);
    const size_t min = static_cast<size_t>((cont ? CONT_MIN_S : TWOWAY_MIN_S) * RATE);
    const size_t hard = static_cast<size_t>((cont ? CONT_MAX_S : TWOWAY_MAX_S) * RATE);
    const size_t trigger = cont ? static_cast<size_t>(CONT_SOFT_S * RATE) : hard;
    const std::vector<float>& fe = tap.fe;
    if (tap.cur.size() < trigger || fe.size() < 4 * PAUSE_FRAMES) return;
    // quiet = under a tenth of the piece's median frame energy - or, on hissy
    // radio audio where the gaps between words sit at the noise floor (a third
    // of the median on an Apollo recording: no pause at all at a tenth), under
    // twice its 10th percentile, at most half the median
    std::vector<float> e(fe);
    std::nth_element(e.begin(), e.begin() + static_cast<long>(e.size() / 2), e.end());
    const float med = e[e.size() / 2];
    std::nth_element(e.begin(), e.begin() + static_cast<long>(e.size() / 10), e.end());
    const float quiet = std::min(std::max(0.1f * med, 2.0f * e[e.size() / 10]), 0.5f * med);
    const size_t first = min / FRAME;
    size_t cut_frame = 0, run = 0;
    for (size_t i = first; i < fe.size(); i++) {
        run = fe[i] < quiet ? run + 1 : 0;
        if (run >= PAUSE_FRAMES) cut_frame = i + 1 - run / 2;   // the middle of the pause so far
    }
    if (cut_frame > 0) {
        cut(tap, vfo, cut_frame * FRAME, 0);
        return;
    }
    if (tap.cur.size() < hard) return;
    constexpr size_t W = 5;   // 100 ms
    const size_t from = fe.size() > 150 + W ? fe.size() - 150 : 0;
    size_t best = from;
    double best_e = -1;
    for (size_t i = from; i + W <= fe.size(); i++) {
        double s = 0;
        for (size_t k = 0; k < W; k++) s += fe[i + k];
        if (best_e < 0 || s < best_e) {
            best_e = s;
            best = i;
        }
    }
    cut(tap, vfo, (best + W / 2) * FRAME, static_cast<size_t>(OVERLAP_S * RATE));
}

// The piece ends at sample `at`; the transmission goes on in the next part,
// which starts `overlap` samples before it
void Worker::cut(Tap& tap, int vfo, size_t at, size_t overlap) {
    at = std::min(at, tap.cur.size());
    const size_t from = at > overlap ? at - overlap : 0;
    std::vector<float> rest(tap.cur.begin() + static_cast<long>(from), tap.cur.end());
    const int64_t t_rest = tap.t0_ms + static_cast<int64_t>(from * 1000 / RATE);
    const size_t closed = tap.closed;
    const long anon = tap.anon_from;
    tap.cur.resize(at);
    finish(tap, vfo, true);
    tap.cur = std::move(rest);
    tap.refe();
    tap.t0_ms = t_rest;
    tap.closed = closed;
    if (anon >= 0) tap.anon_from = static_cast<size_t>(anon) > from ? anon - static_cast<long>(from) : 0;
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
    t.lobe = u.lobe;
    t.lobe_north = u.lobe_north;
    t.slat = u.slat;
    t.slon = u.slon;
    t.conf =is_finite_value(conf) ? std::clamp(conf, 0.0f, 1.0f) : 0.0f;
    t.proc_ms = ms;
    t.part = u.part;
    const std::string clip = keep_audio ? save_clip(t, u) : "";
    t.clip = !clip.empty();
    {
        // the clip, the file and the history together: a Clear (VOICE2TEXT_CLEAR)
        // since the id was taken drops this result instead of bringing it back
        std::lock_guard<std::mutex> lk(s_.st_mu);
        if (gen != s_.clear_gen) {
            if (t.clip) unlink(clip.c_str());
            return;
        }
        if (t.clip) {
            const uint64_t b = file_size(clip);
            s_.clips[t.id] = ClipFile{clip, b};
            s_.clip_bytes += b;
            s_.clip_error.clear();
            trim_clips_locked(s_);
        }
        s_.history.push_back(t);
        while (s_.history.size() > MAX_HISTORY) s_.history.pop_front();
        s_.fresh.push_back(t);
        if (s_.fresh.size() > 200) s_.fresh.erase(s_.fresh.begin());
        std::ofstream o(HISTORY_FILE, std::ios::app);
        o << transcript_json(t, true) << "\n";
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
    // the clip relative to the log folder: voice_clips/YYYY-MM-DD/<id>.opus
    if (t.clip && clip.rfind(clip_root_, 0) == 0)
        fields += ",\"clip\":\"" + json_escape(std::string(CLIP_DIR) + clip.substr(clip_root_.size())) + "\"";
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
        clips_tick(t);
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
        const int64_t took = now_ms() - t;
        std::lock_guard<std::mutex> lk(s_.st_mu);
        s_.loop_max_ms = std::max(s_.loop_max_ms, took);
    }
    // shutdown: what was collected is lost; the helper exits on EOF
    stop_helper("");
}

void push_input(Input&& in, double seconds) {
    State& s = S();
    {
        std::lock_guard<std::mutex> lk(s.q_mu);
        double& q = s.queued_s[in.vfo];
        if (q > MAX_INPUT_S) {   // the worker is far behind: a hole in that VFO's audio
            s.in_dropped_s += seconds;
            return;
        }
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
    double in_dropped;
    {
        std::lock_guard<std::mutex> lk(s.q_mu);
        in_dropped = s.in_dropped_s;
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
      << fmt("%.1f", s.clip_bytes / 1048576.0) << ",\"clip_dir\":\""
      << json_escape(s.clip_root.empty() ? "" : absolute(s.clip_root)) << "\",\"clip_days\":" << s.clip_days
      << ",\"clip_free_mb\":" << s.clip_free / 1048576 << ",\"clip_low\":" << (s.clip_low ? "true" : "false")
      << ",\"clips_skipped\":" << s.clips_skipped << ",\"clip_error\":\"" << json_escape(s.clip_error)
      << "\",\"loop_max_ms\":" << s.loop_max_ms << ",\"in_dropped_s\":" << fmt("%.1f", in_dropped) << ",\"vfos\":[";
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
        o += transcript_json(s.history[i], false);
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
    for (size_t i = 0; i < s.fresh.size(); i++) o += (i ? "," : "") + transcript_json(s.fresh[i], false);
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
    for (const auto& kv : s.clips) unlink(kv.second.path.c_str());   // the day folders stay (retention empties them)
    s.clips.clear();
    s.clip_bytes = 0;
}

std::string lobe_message(uint64_t id) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    for (auto it = s.history.rbegin(); it != s.history.rend(); ++it) {
        if (it->id != id) continue;
        const Transcript& t = *it;
        if (t.lobe.empty()) break;
        std::string o = "{\"voice2text\":{\"lobe\":{\"id\":" + std::to_string(t.id) + ",\"t\":" + std::to_string(t.t_ms) +
                        ",\"vfo\":" + std::to_string(t.vfo) + ",\"mhz\":" + fmt("%.6f", t.mhz) + ",\"dur\":" +
                        fmt("%.1f", t.dur) + ",\"lobe\":\"" + t.lobe + "\",\"lf\":\"" + (t.lobe_north ? "n" : "a") + "\"";
        if (is_finite_value(t.doa)) o += ",\"doa\":" + fmt("%.1f", t.doa);
        if (is_finite_value(t.brg)) o += ",\"brg\":" + fmt("%.1f", t.brg);
        if (is_finite_value(t.slat) && is_finite_value(t.slon))
            o += ",\"slat\":" + fmt("%.6f", t.slat) + ",\"slon\":" + fmt("%.6f", t.slon);
        return o + "}}}";
    }
    return "{\"voice2text\":{\"lobe\":{\"id\":" + std::to_string(id) + ",\"error\":\"no lobe kept for this transcript\"}}}";
}

std::string clip_path(uint64_t id) {
    State& s = S();
    std::lock_guard<std::mutex> lk(s.st_mu);
    auto it = s.clips.find(id);
    return it == s.clips.end() ? "" : it->second.path;
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
