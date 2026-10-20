// kraken_whisper - local speech-to-text for kraken_doa's Voice to Text
// (src/voice2text.cpp), built on whisper.cpp (MIT, github.com/ggml-org/whisper.cpp).
// Free and fully offline: the model files live in transcribe/models/.
//
//   kraken_whisper --serve --model M.bin [--vad V.bin] [--threads N] [--language L]
//       live: jobs on stdin, results on stdout (include/voice2text_wire.hpp)
//   kraken_whisper --file clip.wav --model M.bin [--vad V.bin] [--threads N]
//                  [--language L] [--prompt "words"] [--beam N] [--no-vad]
//       offline test: transcribes a WAV (16-bit PCM) and prints the result
//
// One job at a time (kraken_doa queues them). Each job: the utterance ->
// Silero VAD (when its model is given): only the speech parts, padded, joined
// by short silences -> whisper -> text, cleaned of non-speech annotations
// ("[BLANK_AUDIO]", "(static)") and of the loops and stock phrases whisper
// invents on noise. The process runs at nice 10 (Windows: the below-normal
// priority class), below the receiver.

#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "whisper.h"
#include "voice2text_wire.hpp"
#ifdef _WIN32
#include "utils/process.hpp"   // kproc::lower_own_priority (no setpriority / nice on Windows)
#endif

namespace {

namespace wire = v2t::wire;
constexpr int SR = wire::SAMPLE_RATE;

struct Opts {
    std::string language = "auto";   // multilingual models only ("auto" = detect)
    std::string prompt;              // vocabulary / style hint (callsigns, place names)
    int threads = 2;
    bool vad = true;
    float vad_threshold = 0.5f;
    bool translate = false;          // multilingual models: output English
    int beam = 0;                    // 0 = greedy, else beam search width
};

struct Result {
    float conf = 0, no_speech = 0;
    int speech_ms = -1;
    std::string lang, text, dropped;
};

whisper_context* g_ctx = nullptr;
whisper_vad_context* g_vad = nullptr;
bool g_multilingual = false;

void quiet_log(enum ggml_log_level level, const char* text, void*) {
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "[BLANK_AUDIO]", "(static)", "*beep*", music notes: what whisper writes
// for non-speech - not words anyone said
std::string strip_annotations(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        const char close = c == '[' ? ']' : c == '(' ? ')' : c == '*' ? '*' : 0;
        if (close) {
            const size_t e = s.find(close, i + 1);
            if (e != std::string::npos && e - i < 60) { i = e + 1; continue; }
        }
        if (!s.compare(i, 3, "\xE2\x99\xAA") || !s.compare(i, 3, "\xE2\x99\xAB")) { i += 3; continue; }   // ♪ ♫
        o += c;
        i++;
    }
    // squeeze the spaces left behind
    std::string r;
    for (char c : o)
        if (!(c == ' ' && (r.empty() || r.back() == ' '))) r += c;
    return trim(r);
}

// letters and digits, lower case, single spaces (UTF-8 letters kept as they are)
std::string lower_alnum(const std::string& s) {
    std::string o;
    for (unsigned char c : s)
        if (c >= 0x80 || std::isalnum(c)) o += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        else if (c == ' ' && !o.empty() && o.back() != ' ') o += ' ';
    return trim(o);
}

// whisper's loops on noise / clipped audio: the same sentence or word 4+
// times in a row is kept once ("Okay. Okay. Okay. Okay." -> "Okay.")
std::string collapse_repeats(const std::string& s) {
    // units: sentences (ending at . ! ? before a space - not "146.52"), then words
    auto split = [](const std::string& t, bool sentences) {
        std::vector<std::string> u;
        std::string cur;
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            cur += c;
            const bool end = sentences ? ((c == '.' || c == '!' || c == '?') && (i + 1 == t.size() || t[i + 1] == ' '))
                                       : c == ' ';
            if (end) {
                if (!trim(cur).empty()) u.push_back(trim(cur));
                cur.clear();
            }
        }
        if (!trim(cur).empty()) u.push_back(trim(cur));
        return u;
    };
    std::string text = s;
    for (bool sentences : {true, false}) {
        auto u = split(text, sentences);
        std::vector<std::string> out;
        for (size_t i = 0; i < u.size();) {
            size_t j = i + 1;
            const std::string k = lower_alnum(u[i]);
            while (j < u.size() && !k.empty() && lower_alnum(u[j]) == k) j++;
            out.push_back(u[i]);
            if (j - i < 4)
                for (size_t m = i + 1; m < j; m++) out.push_back(u[m]);   // up to 3 times can be real speech
            i = j;
        }
        text.clear();
        for (const auto& x : out) text += (text.empty() ? "" : " ") + x;
    }
    return text;
}

// Phrases whisper produces from nothing (its training data's subtitle
// credits) - never on a radio channel
bool is_credit_phrase(const std::string& norm) {
    static const char* const P[] = {"thanks for watching", "thank you for watching", "please subscribe",
                                    "subtitles by", "subtitled by", "captions by", "amaraorg", "amara org",
                                    "transcribed by", "translated by", "see you in the next video",
                                    "like and subscribe"};
    for (const char* p : P)
        if (norm.find(p) != std::string::npos) return true;
    return false;
}
// ... and ones that are also real speech: only dropped when whisper itself
// doubts there was speech
bool is_filler_phrase(const std::string& norm) {
    static const char* const P[] = {"thank you", "thanks", "you", "bye", "okay", "oh", "so", "the end", "hmm"};
    for (const char* p : P)
        if (norm == p) return true;
    return false;
}

size_t word_count(const std::string& s) {
    size_t n = 0;
    bool in = false;
    for (unsigned char c : s) {
        const bool w = c >= 0x80 || std::isalnum(c);
        if (w && !in) n++;
        in = w;
    }
    return n;
}

// The user's vocabulary as whisper's prompt. Whisper reads a prompt as the
// text said BEFORE, so a bare list ("Houston, Roger, over") read like an
// unfinished sentence and greedy decoding often ended after one word ("the" -
// seen on 2 of 4 test transmissions): a list becomes a finished sentence
// ("Glossary: Houston, Roger, over."), which decoded every one in full
std::string prompt_text(const std::string& p) {
    const std::string t = trim(p);
    if (t.empty()) return t;
    const char last = t.back();
    if (last == '.' || last == '!' || last == '?') return t;
    return "Glossary: " + t + ".";
}

struct Decoded {
    bool ok = false;
    std::string text, lang;   // text as whisper wrote it (not cleaned yet)
    float conf = 0, no_speech = 0;
};

Decoded decode(const std::vector<float>& speech, const Opts& o, const std::string& prompt) {
    Decoded d;
    whisper_full_params p =
        whisper_full_default_params(o.beam > 1 ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    p.n_threads = std::clamp(o.threads, 1, 8);
    p.no_context = true;         // utterances are independent (other radios, other channels)
    p.no_timestamps = true;
    p.print_special = p.print_progress = p.print_realtime = p.print_timestamps = false;
    p.suppress_nst = true;       // no "(music)" / "[laughs]" tokens
    const std::string lang = g_multilingual ? (o.language.empty() ? "auto" : o.language) : "en";
    p.language = lang.c_str();
    p.translate = g_multilingual && o.translate;
    if (!prompt.empty()) {
        p.initial_prompt = prompt.c_str();
        p.carry_initial_prompt = true;   // every 30 s window of a long transmission
    }
    if (o.beam > 1) p.beam_search.beam_size = std::clamp(o.beam, 2, 8);
    if (whisper_full(g_ctx, p, speech.data(), static_cast<int>(speech.size())) != 0) return d;
    d.ok = true;
    const int nseg = whisper_full_n_segments(g_ctx);
    const whisper_token eot = whisper_token_eot(g_ctx);
    double psum = 0;
    int pn = 0;
    for (int i = 0; i < nseg; i++) {
        const float nsp = whisper_full_get_segment_no_speech_prob(g_ctx, i);
        double seg_p = 0, seg_lp = 0;
        int seg_n = 0;
        const int nt = whisper_full_n_tokens(g_ctx, i);
        for (int j = 0; j < nt; j++) {
            const whisper_token_data td = whisper_full_get_token_data(g_ctx, i, j);
            if (td.id >= eot) continue;   // special tokens
            seg_p += td.p;
            seg_lp += td.plog;
            seg_n++;
        }
        // whisper's own silence rule: likely no speech AND an unsure decode
        if (seg_n > 0 && nsp > 0.6f && seg_lp / seg_n < -1.0) continue;
        psum += seg_p;
        pn += seg_n;
        d.no_speech = std::max(d.no_speech, nsp);
        d.text += whisper_full_get_segment_text(g_ctx, i);
    }
    d.conf = pn ? static_cast<float>(psum / pn) : 0.0f;
    if (g_multilingual) {
        const int id = whisper_full_lang_id(g_ctx);
        const char* ls = id >= 0 ? whisper_lang_str(id) : nullptr;
        d.lang = ls ? ls : "";
    } else {
        d.lang = "en";
    }
    return d;
}

Result transcribe(const std::vector<float>& pcm, const Opts& o) {
    Result r;
    std::vector<float> speech;
    std::vector<size_t> joins;   // where in `speech` a VAD part ends (the middle of the gap after it)
    if (g_vad && o.vad) {
        whisper_vad_params vp = whisper_vad_default_params();
        vp.threshold = std::clamp(o.vad_threshold, 0.05f, 0.95f);
        vp.min_speech_duration_ms = 200;
        vp.min_silence_duration_ms = 300;
        vp.speech_pad_ms = 200;    // radio speech starts / ends abruptly: keep the word edges
        whisper_vad_segments* seg = whisper_vad_segments_from_samples(g_vad, vp, pcm.data(), static_cast<int>(pcm.size()));
        if (seg) {
            const int ns = whisper_vad_segments_n_segments(seg);
            r.speech_ms = 0;
            const std::vector<float> gap(SR * 15 / 100, 0.0f);   // 150 ms between the parts
            for (int i = 0; i < ns; i++) {
                // centiseconds
                long a = std::lround(whisper_vad_segments_get_segment_t0(seg, i) * SR / 100.0);
                long b = std::lround(whisper_vad_segments_get_segment_t1(seg, i) * SR / 100.0);
                a = std::clamp<long>(a, 0, static_cast<long>(pcm.size()));
                b = std::clamp<long>(b, a, static_cast<long>(pcm.size()));
                if (b <= a) continue;
                if (!speech.empty()) {
                    joins.push_back(speech.size() + gap.size() / 2);
                    speech.insert(speech.end(), gap.begin(), gap.end());
                }
                speech.insert(speech.end(), pcm.begin() + a, pcm.begin() + b);
                r.speech_ms += static_cast<int>((b - a) * 1000 / SR);
            }
            whisper_vad_free_segments(seg);
            if (r.speech_ms < 200) {
                r.dropped = "no speech";
                return r;
            }
        } else {
            speech = pcm;   // VAD failed: whisper gets everything
        }
    } else {
        speech = pcm;
    }
    // whisper wants at least ~1 s
    if (speech.size() < static_cast<size_t>(SR * 11 / 10)) speech.resize(SR * 11 / 10, 0.0f);

    const std::string prompt = prompt_text(o.prompt);
    Decoded d = decode(speech, o, prompt);
    if (!d.ok) {
        r.dropped = "whisper error";
        return r;
    }
    // even so, a prompt can end a decode early: far fewer words than the
    // speech holds -> once more without it, the longer text wins
    const double speech_s = (r.speech_ms >= 0 ? r.speech_ms : static_cast<double>(speech.size()) * 1000 / SR) / 1000;
    if (!prompt.empty() && speech_s >= 2.5 && word_count(d.text) < speech_s * 0.4) {
        Decoded d2 = decode(speech, o, "");
        if (d2.ok && word_count(d2.text) > word_count(d.text)) d = std::move(d2);
    }
    // whisper sometimes ends a decode after its first sentence ("Roger the
    // register." of 11 s of speech; timestamps don't make it go on): such a
    // short result -> the speech's two halves (split between VAD parts, at
    // the gap nearest the middle) decoded on their own, the longer text wins
    if (speech_s >= 4.0 && word_count(d.text) < speech_s * 0.6 && !joins.empty()) {
        size_t at = joins[0];
        for (size_t j : joins)
            if (std::labs(static_cast<long>(j) - static_cast<long>(speech.size() / 2)) <
                std::labs(static_cast<long>(at) - static_cast<long>(speech.size() / 2)))
                at = j;
        if (at >= static_cast<size_t>(SR) && speech.size() - at >= static_cast<size_t>(SR)) {
            const std::string pr = word_count(d.text) < speech_s * 0.4 ? "" : prompt;
            Decoded a = decode(std::vector<float>(speech.begin(), speech.begin() + static_cast<long>(at)), o, pr);
            Decoded b = decode(std::vector<float>(speech.begin() + static_cast<long>(at), speech.end()), o, pr);
            if (a.ok && b.ok) {
                const size_t na = word_count(a.text), nb = word_count(b.text);
                if (na + nb > word_count(d.text)) {
                    d.text = trim(a.text) + " " + trim(b.text);
                    d.conf = static_cast<float>((a.conf * na + b.conf * nb) / static_cast<double>(na + nb));
                    d.no_speech = std::max(a.no_speech, b.no_speech);
                }
            }
        }
    }
    r.conf = d.conf;
    r.no_speech = d.no_speech;
    r.lang = d.lang;
    const std::string text = collapse_repeats(strip_annotations(trim(d.text)));
    const std::string norm = lower_alnum(text);
    if (norm.empty()) r.dropped = "no words";
    else if (is_credit_phrase(norm)) r.dropped = "noise phrase";
    else if (is_filler_phrase(norm) && (r.no_speech > 0.35f || r.conf < 0.5f)) r.dropped = "noise phrase";
    else r.text = text;
    return r;
}

// --- live: jobs on stdin ----------------------------------------------------

int g_out = 1;

void write_all(const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n > 0) {
        ssize_t w = ::write(g_out, c, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            _exit(0);   // kraken_doa went away
        }
        c += w;
        n -= static_cast<size_t>(w);
    }
}

void send(uint32_t type, const std::string& payload) {
    const uint32_t hdr[2] = {type, static_cast<uint32_t>(payload.size())};
    write_all(hdr, 8);
    if (!payload.empty()) write_all(payload.data(), payload.size());
}

bool read_all(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        ssize_t r = ::read(fd, c, n);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void set_option(Opts& o, const std::string& kv) {
    const size_t eq = kv.find('=');
    const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
    if (k == "language") o.language = v.substr(0, 16);
    else if (k == "prompt") o.prompt = v.substr(0, 600);
    else if (k == "threads") o.threads = std::clamp(atoi(v.c_str()), 1, 8);
    else if (k == "vad") o.vad = v == "1";
    else if (k == "vad_threshold") o.vad_threshold = static_cast<float>(std::clamp(atof(v.c_str()), 0.05, 0.95));
    else if (k == "translate") o.translate = v == "1";
    else if (k == "beam") o.beam = std::clamp(atoi(v.c_str()), 0, 8);
}

int serve(Opts o, const std::string& model) {
    // stdout carries the protocol: keep a private copy, point fd 1 at stderr
    // so nothing printed by the libraries can corrupt it
    g_out = dup(1);
    dup2(2, 1);
#ifdef _WIN32
    const char* const seps = "/\\";   // a Windows path may use either (KRAKEN_WHISPER_MODELS=C:\models)
#else
    const char* const seps = "/";
#endif
    std::string ready = "model=" + model.substr(model.find_last_of(seps) + 1);
    ready += std::string("\0type=", 6) + whisper_model_type_readable(g_ctx);
    ready += std::string("\0multilingual=", 14) + (g_multilingual ? "1" : "0");
    ready += std::string("\0vad=", 5) + (g_vad ? "1" : "0");
    ready += std::string("\0version=", 9) + whisper_version();
    ready += std::string("\0system=", 8) + whisper_print_system_info();
    send(wire::READY, ready);
    std::vector<char> buf;
    std::vector<float> pcm;
    for (;;) {
        uint32_t hdr[2];
        if (!read_all(0, hdr, 8)) return 0;
        if (hdr[1] > wire::MAX_PAYLOAD) {
            send(wire::ERROR, "message too long");
            return 2;
        }
        buf.resize(hdr[1]);
        if (hdr[1] && !read_all(0, buf.data(), hdr[1])) return 0;
        if (hdr[0] == wire::OPTION) {
            set_option(o, std::string(buf.begin(), buf.end()));
        } else if (hdr[0] == wire::JOB && buf.size() >= 4) {
            uint32_t id;
            memcpy(&id, buf.data(), 4);
            const size_t n = (buf.size() - 4) / 2;
            pcm.resize(n);
            const char* src = buf.data() + 4;
            for (size_t i = 0; i < n; i++) {
                int16_t v;
                memcpy(&v, src + 2 * i, 2);
                pcm[i] = v / 32768.0f;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const Result r = transcribe(pcm, o);
            const uint32_t ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - t0).count());
            std::string out(20, '\0');
            const int32_t sp = r.speech_ms;
            memcpy(&out[0], &id, 4);
            memcpy(&out[4], &r.conf, 4);
            memcpy(&out[8], &r.no_speech, 4);
            memcpy(&out[12], &ms, 4);
            memcpy(&out[16], &sp, 4);
            out += r.lang;
            out += '\0';
            out += r.text;
            out += '\0';
            out += r.dropped;
            send(wire::RESULT, out);
        }
    }
}

// --- offline: a WAV file ----------------------------------------------------

bool read_wav(const std::string& path, std::vector<float>* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { *err = "cannot open " + path; return false; }
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) { *err = "not a WAV file"; return false; }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        uint32_t len;
        memcpy(&len, d.data() + p + 4, 4);
        const char* body = d.data() + p + 8;
        if (!memcmp(d.data() + p, "fmt ", 4) && len >= 16) {
            memcpy(&fmt, body, 2);
            memcpy(&ch, body + 2, 2);
            memcpy(&rate, body + 4, 4);
            memcpy(&bits, body + 14, 2);
        } else if (!memcmp(d.data() + p, "data", 4)) {
            if (fmt != 1 || bits != 16 || ch < 1) { *err = "need 16-bit PCM"; return false; }
            const size_t frames = std::min<size_t>(len, d.size() - (p + 8)) / (2u * ch);
            std::vector<float> mono(frames);
            for (size_t i = 0; i < frames; i++) {
                int16_t v;
                memcpy(&v, body + 2 * ch * i, 2);   // first channel
                mono[i] = v / 32768.0f;
            }
            // other rates: linear resampling (tests only - kraken_doa sends 16 kHz)
            if (rate != static_cast<uint32_t>(SR) && rate > 0) {
                const size_t n = static_cast<size_t>(frames * static_cast<double>(SR) / rate);
                out->resize(n);
                for (size_t i = 0; i < n; i++) {
                    const double x = i * static_cast<double>(rate) / SR;
                    const size_t k = static_cast<size_t>(x);
                    const double fr = x - k;
                    const float a = mono[std::min(k, frames - 1)], b = mono[std::min(k + 1, frames - 1)];
                    (*out)[i] = static_cast<float>(a + (b - a) * fr);
                }
            } else {
                *out = std::move(mono);
            }
            return true;
        }
        p += 8 + len + (len & 1);
    }
    *err = "no audio in the WAV file";
    return false;
}

void usage() {
    fprintf(stderr,
            "usage: kraken_whisper --serve --model M.bin [--vad V.bin] [--threads N] [--language L]\n"
            "       kraken_whisper --file clip.wav --model M.bin [--vad V.bin] [--threads N] [--language L]\n"
            "                      [--prompt \"words\"] [--beam N] [--no-vad] [--translate]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string model, vad, file;
    bool serve_mode = false;
    Opts o;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(1); }
            return argv[++i];
        };
        if (a == "--serve") serve_mode = true;
        else if (a == "--model") model = next();
        else if (a == "--vad") vad = next();
        else if (a == "--file") file = next();
        else if (a == "--threads") o.threads = std::clamp(atoi(next().c_str()), 1, 8);
        else if (a == "--language") o.language = next();
        else if (a == "--prompt") o.prompt = next();
        else if (a == "--beam") o.beam = std::clamp(atoi(next().c_str()), 0, 8);
        else if (a == "--no-vad") o.vad = false;
        else if (a == "--translate") o.translate = true;
        else { usage(); return 1; }
    }
    if (model.empty() || (!serve_mode && file.empty())) { usage(); return 1; }
    // below the receiver: the Pi's cores are shared with the DSP pipeline
#ifdef _WIN32
    kproc::lower_own_priority();   // BELOW_NORMAL_PRIORITY_CLASS
#else
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    whisper_log_set(quiet_log, nullptr);

    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    g_ctx = whisper_init_from_file_with_params(model.c_str(), cp);
    if (!g_ctx) {
        const std::string e = "cannot load the model " + model;
        if (serve_mode) {
            g_out = dup(1);
            send(wire::ERROR, e);
        }
        fprintf(stderr, "%s\n", e.c_str());
        return 2;
    }
    g_multilingual = whisper_is_multilingual(g_ctx) != 0;
    if (!vad.empty()) {
        whisper_vad_context_params vcp = whisper_vad_default_context_params();
        vcp.n_threads = 1;
        vcp.use_gpu = false;
        g_vad = whisper_vad_init_from_file_with_params(vad.c_str(), vcp);
        if (!g_vad) fprintf(stderr, "cannot load the VAD model %s - transcribing without it\n", vad.c_str());
    }

    int rc = 0;
    if (serve_mode) {
        rc = serve(o, model);
    } else {
        std::vector<float> pcm;
        std::string err;
        if (!read_wav(file, &pcm, &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            rc = 1;
        } else {
            const auto t0 = std::chrono::steady_clock::now();
            const Result r = transcribe(pcm, o);
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            printf("audio %.1f s, speech %d ms, processed in %.2f s, lang %s, conf %.2f, no_speech %.2f%s%s\n",
                   pcm.size() / static_cast<double>(SR), r.speech_ms, s, r.lang.c_str(), r.conf, r.no_speech,
                   r.dropped.empty() ? "" : ", dropped: ", r.dropped.c_str());
            printf("%s\n", r.text.c_str());
        }
    }
    if (g_vad) whisper_vad_free(g_vad);
    whisper_free(g_ctx);
    return rc;
}
